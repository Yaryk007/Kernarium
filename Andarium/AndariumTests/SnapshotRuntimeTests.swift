// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Darwin
import Foundation
import Observation
import Testing
@testable import Andarium

/// Counts what a `@Sendable` closure reports; the tests read it on the main
/// actor after the report.
private nonisolated final class Counter: @unchecked Sendable {
    nonisolated(unsafe) var value = 0
}

/// A guest's storage work held at its last step, off the main actor, until
/// the test lets it go.
private nonisolated final class Hold: @unchecked Sendable {
    private let reached = DispatchSemaphore(value: 0)
    private let released = DispatchSemaphore(value: 0)

    /// Called from the work's progress report: says it is there and waits (at
    /// most a minute, so that a test that fails does not hang).
    func park() {
        reached.signal()
        _ = released.wait(timeout: .now() + 60)
    }

    func release() { released.signal() }
}

/// The runtime's gate on the snapshots: an operation that changes the data
/// runs only while the guest is stopped, one at a time, and what else changes
/// the guest's files waits for it. A guest on a temporary directory that has
/// run once; nothing boots.
@MainActor
struct SnapshotRuntimeTests {
    /// A runtime and its model. The model plays the runtime's driver only
    /// when asked: with it, the model's phase changes start the readings that
    /// a run has.
    private func make(_ fixture: SnapshotTestGuest, asDriver: Bool = false) -> (LiveRuntime, AppModel) {
        let model = AppModel(
            guest: fixture.guest, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: fixture.storage.createRequirementBytes)
        let runtime = LiveRuntime(
            model: model, storage: fixture.storage,
            marker: PauseMarker(url: fixture.directory.appendingPathComponent("pause.json")),
            keyStore: AdbKeyStore(tag: "com.imxieyi.andarium.adb-key.test-\(UUID().uuidString)"))
        if asDriver { model.driver = runtime }
        model.snapshotDriver = runtime
        return (runtime, model)
    }

    /// Lets the main actor run until `condition` holds, a bounded number of times.
    private func untilTrue(_ condition: () -> Bool) async -> Bool {
        for _ in 0..<2000 {
            if condition() { return true }
            await Task.yield()
        }
        return condition()
    }

    @Test func dataChangingOperationsAreRefusedUnlessTheGuestIsStopped() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        let made = try #require(await runtime.createSnapshot(named: "kept").value)
        #expect(runtime.snapshotGate == nil && model.snapshotGate == nil)
        #expect(await runtime.snapshotListing().whileNotStopped == nil)

        for phase: Phase in [.booting(.normal), .running(.normal), .running(.recovery), .booting(.restart), .failed] {
            model.phase = phase
            #expect(runtime.snapshotGate == .guestNotStopped, "\(phase)")
            #expect(model.snapshotGate == .guestNotStopped, "\(phase)")
            #expect(await runtime.createSnapshot(named: "x").refusal == .guestNotStopped)
            #expect(await runtime.restoreSnapshot(made.id, guest: fixture.guest).refusal == .guestNotStopped)
            #expect(await runtime.deleteSnapshot(made.id).refusal == .guestNotStopped)
            #expect(await runtime.snapshotListing().whileNotStopped == .guestNotStopped)
            // Rename and the list do not wait for a stopped guest.
            #expect(await runtime.renameSnapshot(made.id, to: "named while \(phase)").value != nil)
        }
        // Nothing was changed while it ran.
        model.phase = .home
        let listing = await runtime.snapshotListing()
        #expect(listing.items.count == 1 && listing.items.first?.name == "named while failed")
        #expect(await runtime.deleteSnapshot(made.id).value != nil)
        #expect(await runtime.snapshotListing().items.isEmpty)
    }

    /// A run that is pending for its display holds the disk before the phase
    /// says so: the model's own gate and the runtime's agree.
    @Test func aRunThatIsPendingKeepsOperationsOutOnHome() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture, asDriver: true)
        let made = try #require(await runtime.createSnapshot(named: "kept").value)
        #expect(model.phase == .home && model.snapshotGate == nil && !model.snapshotActivity.runtimeActive)

        // The display has no size yet, so the run stays pending.
        runtime.begin(guest: fixture.guest, mode: .normal, kind: .normal)
        #expect(runtime.activity.runtimeActive && model.snapshotActivity.runtimeActive)
        #expect(model.phase == .home)
        #expect(runtime.snapshotGate == .guestNotStopped && model.snapshotGate == .guestNotStopped)
        #expect(await runtime.createSnapshot(named: "x").refusal == .guestNotStopped)
        #expect(await runtime.restoreSnapshot(made.id, guest: fixture.guest).refusal == .guestNotStopped)
        #expect(await runtime.deleteSnapshot(made.id).refusal == .guestNotStopped)
        #expect(await runtime.snapshotListing().whileNotStopped == .guestNotStopped)
    }

    @Test func aRestoreIsAdoptedByTheModelAndItsPlanIsRead() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (_, model) = make(fixture, asDriver: true)
        let made = try #require(await model.takeSnapshot(named: "good").value)
        try SnapshotTestDisk.write(fixture.disk, at: 0, count: 1 << 20, value: 2)
        // A growth the person asked for stays; the capacity is at least the disk's.
        model.guest?.pendingCapacityGiB = 16
        let result = await model.restoreSnapshot(made.id)
        #expect(result.value != nil)
        #expect(try SnapshotTestDisk.read(fixture.disk, at: 0) == 1)
        #expect(model.guest?.capacityGiB == 8 && model.guest?.pendingCapacityGiB == 16)
        #expect(try fixture.storage.load()?.pendingCapacityGiB == 16)
        #expect(model.plan == .normal)
        #expect(model.guest?.usedBytes ?? 0 > 0)
    }

    @Test func oneOperationRunsAtATime() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        // The runtime holds its model weakly: without one it is not stopped.
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        async let first = runtime.createSnapshot(named: "first")
        async let second = runtime.createSnapshot(named: "second")
        let results = await [first.refusal, second.refusal]
        let made = await [first.value != nil, second.value != nil]
        // One went ahead and the other found it under way, whichever was first.
        #expect(results.filter { $0 == .busy }.count == 1)
        #expect(made.filter { $0 }.count == 1)
        #expect(await runtime.snapshotListing().items.count == 1)
        #expect(runtime.snapshotGate == nil, "the operation under way is cleared before it returns")
    }

    // MARK: What the screens follow

    @Test func theModelFollowsWhatTheDriverIsDoingAndAViewCanObserveIt() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        #expect(model.snapshotActivity == runtime.activity && model.snapshotActivity.revision == 0)

        // What the driver reports around an operation, in order, on its way to the model.
        var reported: [SnapshotActivity] = []
        runtime.activityChanged = { [weak model] in
            reported.append($0)
            model?.snapshotActivity = $0
        }
        // A view that reads the gate is told when it changes.
        let told = Counter()
        withObservationTracking { _ = model.snapshotGate } onChange: { told.value += 1 }

        let made = try #require(await runtime.createSnapshot(named: "one").value)
        #expect(reported.map(\.operation) == [.create, nil, nil])
        #expect(reported.map(\.revision) == [0, 0, 1])
        #expect(told.value == 1, "a view that reads the gate is told when an operation starts")
        #expect(model.snapshotActivity.operation == nil && model.snapshotActivity.revision == 1)
        #expect(await runtime.deleteSnapshot(made.id).value != nil)
        #expect(model.snapshotActivity.revision == 2)
    }

    @Test func aListingIsNeverOlderThanTheLastFinishedOperation() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        var last: SnapshotListing?
        for round in 1...6 {
            async let created = runtime.createSnapshot(named: "round \(round)")
            async let listed = runtime.snapshotListing()
            let (result, listing) = await (created, listed)
            last = listing
            // Whatever the order, a listing that is as new as the driver's last
            // operation has everything that operation made.
            if listing.revision == runtime.activity.revision {
                #expect(listing.items.count == round, "round \(round)")
            } else {
                #expect(listing.revision < runtime.activity.revision, "round \(round)")
            }
            #expect(result.value != nil)
        }
        let settled = await runtime.snapshotListing()
        #expect(settled.items.count == 6 && settled.revision == runtime.activity.revision)
        #expect(last != nil)
    }

    @Test func anUnmeasuredListingNeedsNoMeasuringAndTheDataReleaseIsNamed() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        _ = try #require(await runtime.createSnapshot(named: "one").value)
        let quick = await runtime.snapshotListing(measured: false)
        let full = await runtime.snapshotListing(measured: true)
        #expect(!quick.isMeasured && full.isMeasured)
        #expect(quick.items.map(\.id) == full.items.map(\.id) && quick.items.map(\.name) == ["one"])
        #expect(quick.dataRelease == SnapshotRelease(releaseID: "20261031-los24-r4", buildNumber: 100))
        #expect(quick.revision == full.revision)
    }

    // MARK: What else changes the guest's files waits

    /// A create that is under way, then `body` once it is: the one started
    /// first holds the disk.
    private func whileACreateIsUnderWay(
        _ runtime: LiveRuntime, _ body: () async throws -> Void
    ) async throws -> SnapshotResult<SnapshotSummary> {
        let creating = Task { @MainActor in await runtime.createSnapshot(named: "held") }
        #expect(await untilTrue { runtime.activity.operation == .create })
        try await body()
        return await creating.value
    }

    @Test func eraseWaitsForAnOperationUnderWayAndRemovesWhatItMade() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        var reported: [SnapshotWork?] = []
        runtime.activityChanged = { reported.append($0.operation) }
        let result = try await whileACreateIsUnderWay(runtime) {
            try await runtime.erase(progress: { _ in })
        }
        // The snapshot was made whole before the erase took it away.
        #expect(result.value != nil)
        #expect(try fixture.storage.load() == nil)
        #expect(!FileManager.default.fileExists(atPath: fixture.directory.appendingPathComponent("Guest/Snapshots").path))
        // The guest's storage is under way only after the snapshot operation ended.
        let created = try #require(reported.firstIndex(of: .create))
        let erasing = try #require(reported.firstIndex(of: .guestStorage))
        #expect(created < erasing)
        #expect(runtime.activity.operation == nil && runtime.snapshotGate == nil)
    }

    @Test func creatingAGuestWaitsForAnOperationUnderWay() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        var reported: [SnapshotWork?] = []
        runtime.activityChanged = { reported.append($0.operation) }
        let result = try await whileACreateIsUnderWay(runtime) {
            _ = try await runtime.create(fixture.guest, progress: { _ in })
        }
        #expect(result.value != nil)
        let created = try #require(reported.firstIndex(of: .create))
        let setUp = try #require(reported.firstIndex(of: .guestStorage))
        #expect(created < setUp)
    }

    /// A listing waits for a guest being erased, and a cancelled one does not:
    /// it answers at once instead of polling on the main actor.
    @Test(.timeLimit(.minutes(2))) func aCancelledListingDuringAnEraseAnswersAtOnce() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        let hold = Hold()
        let erasing = Task { @MainActor in try await runtime.erase(progress: { _ in hold.park() }) }
        #expect(await untilTrue { runtime.activity.operation == .guestStorage })

        let waiting = Task { @MainActor in await runtime.snapshotListing() }
        let cancelled = Task { @MainActor in await runtime.snapshotListing() }
        for _ in 0..<20 { await Task.yield() }
        cancelled.cancel()
        // The cancelled caller returns while the erase still holds the files.
        let answer = await cancelled.value
        #expect(runtime.activity.operation == .guestStorage)
        #expect(!answer.isMeasured)
        // The other one is still waiting for the erase.
        try await Task.sleep(for: .milliseconds(150))
        #expect(runtime.activity.operation == .guestStorage)

        hold.release()
        try await erasing.value
        let after = await waiting.value
        #expect(after.items.isEmpty && after.isMeasured && after.revision == runtime.activity.revision)
        #expect(runtime.activity.operation == nil)
    }

    /// A start prepares its files after a restore that is under way, and from
    /// what the restore put back.
    @Test func aStartsFilesAreReadAfterTheRestoreAndFromWhatItPutBack() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        let made = try #require(await runtime.createSnapshot(named: "good").value)
        let files = InstanceFiles(directory: fixture.storage.locations.directory)
        let restoredBoot = try #require(try files.readState()?.lastBooted)
        try SnapshotTestDisk.write(fixture.disk, at: 0, count: 1 << 20, value: 2)

        let restoring = Task { @MainActor in await runtime.restoreSnapshot(made.id, guest: fixture.guest) }
        #expect(await untilTrue { runtime.activity.operation == .restore })
        try await runtime.prepareFiles(for: fixture.guest, progress: { _ in })
        #expect(await restoring.value.value != nil)

        // The restore came first (its record put back the old start time) and
        // the start's own update of the record followed it.
        #expect(try SnapshotTestDisk.read(fixture.disk, at: 0) == 1)
        let now = try #require(try files.readState()?.lastBooted)
        #expect(now != restoredBoot, "the start was prepared before the restore put its record back")
    }

    /// The update's Restore never answers with a snapshot refusal, which would
    /// reach an alert as a generic system message: it waits behind the operation.
    @Test func theUpdatesRestoreWaitsBehindAnOperationAndNeverRefusesAsASnapshot() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture)
        defer { withExtendedLifetime(model) {} }
        let made = try #require(await runtime.createSnapshot(named: "good").value)

        let restoring = Task { @MainActor in await runtime.restoreSnapshot(made.id, guest: fixture.guest) }
        #expect(await untilTrue { runtime.activity.operation == .restore })
        var failure: (any Error)?
        do { try await runtime.restoreUpdate() } catch { failure = error }
        #expect(await restoring.value.value != nil)
        // No update is pending in this guest: the update's own answer.
        #expect(failure is InstanceError)
        #expect(!(failure is SnapshotRefusal))
        #expect(runtime.activity.revision >= 3)
    }

    // MARK: A restore that cannot finish

    @Test func aRestoreThatCannotFinishIsRefusedAsSuchAndTheListingSaysWhichAndWhy() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let (runtime, model) = make(fixture, asDriver: true)
        let other = try #require(await runtime.createSnapshot(named: "other").value)
        let made = try #require(await runtime.createSnapshot(named: "good").value)
        try SnapshotTestDisk.write(fixture.disk, at: 0, count: 1 << 20, value: 2)
        // A record the volume will not replace: the write after the data went in fails.
        let record = fixture.storage.locations.instance
        #expect(chflags(record.path, UInt32(UF_IMMUTABLE)) == 0)
        defer { _ = chflags(record.path, 0) }

        let result = await model.restoreSnapshot(made.id)
        #expect(result.refusal == .restoreUnfinished)
        #expect(try SnapshotTestDisk.read(fixture.disk, at: 0) == 1, "the snapshot's data is in place")

        let listing = await runtime.snapshotListing()
        let unfinished = try #require(listing.unfinishedRestore)
        #expect(unfinished.snapshot == made.id)
        #expect(unfinished.lastFailure != nil)
        #expect(listing.createRefusal == .restoreUnfinished)
        // Room can be made by deleting another snapshot, not the one being restored.
        #expect(await runtime.deleteSnapshot(made.id).refusal == .restoreUnfinished)
        #expect(await runtime.createSnapshot(named: "x").refusal == .restoreUnfinished)
        #expect(await runtime.deleteSnapshot(other.id).value != nil)

        // The next start finishes it.
        #expect(chflags(record.path, 0) == 0)
        try await runtime.prepareFiles(for: fixture.guest, progress: { _ in })
        let after = await runtime.snapshotListing()
        #expect(after.unfinishedRestore == nil && after.items.map(\.id) == [made.id])
    }

    @Test func aModelWithNoDriverRefusesEveryOperation() async {
        let model = AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 0)
        #expect(await model.takeSnapshot(named: "x").refusal == .noData)
        #expect(await model.restoreSnapshot(SnapshotID()).refusal == .noData)
        #expect(await model.deleteSnapshot(SnapshotID()).refusal == .noData)
        #expect(await model.renameSnapshot(SnapshotID(), to: "x").refusal == .noData)
        #expect(model.snapshotGate == .noData)
        #expect(await model.snapshotListing().createRefusal == .noData)
        #expect(await model.snapshotListing(measured: false).createRefusal == .noData)
    }

    // MARK: A start meanwhile

    @Test func aStartIsIgnoredWhileASnapshotOperationIsUnderWayAndWorksAfter() {
        let host = AppEntry.hostDevice()
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.refreshPlan()

        // An operation that holds the guest's files is not interleaved with a start.
        for work in [SnapshotWork.restore, .create, .delete, .guestStorage] {
            model.snapshotActivity = SnapshotActivity(runtimeActive: false, operation: work, revision: 0)
            model.start(.normal)
            #expect(model.phase == .home && driver.begun.isEmpty && model.alert == nil, "\(work)")
        }
        model.snapshotActivity = SnapshotActivity(runtimeActive: false, operation: nil, revision: 1)
        model.start(.normal)
        #expect(model.phase == .booting(.normal) && driver.begun.count == 1)
    }

    /// A rename is not one: the runtime makes the start wait for it, and a
    /// start tapped as the page goes is not lost.
    @Test func aStartGoesAheadWhileARenameIsUnderWay() {
        let host = AppEntry.hostDevice()
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.refreshPlan()
        model.snapshotActivity = SnapshotActivity(runtimeActive: false, operation: .rename, revision: 0)
        model.start(.normal)
        #expect(model.phase == .booting(.normal) && driver.begun.count == 1)
    }
}
