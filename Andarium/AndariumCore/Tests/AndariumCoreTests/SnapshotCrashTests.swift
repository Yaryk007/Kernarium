// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

private func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }

private let crashDate = Date(timeIntervalSince1970: 1_800_000_000)

/// A crash between any two file calls of an operation, then the next launch:
/// the guest's directory is copied before each call (`SnapshotFaults`), each
/// copy is recovered as a launch recovers it, and the result must be the state
/// the operation started from or the one an uninterrupted run reaches.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotCrashTests {
    /// The storage on a copy of the guest's directory, as the next launch has it.
    private func launch(_ capture: URL, _ fixture: SnapshotFixture, on release: ReleaseIdentity? = nil) -> TwoDiskStorage {
        TwoDiskStorage(
            locations: GuestLocations(directory: capture), release: release ?? fixture.storage.release,
            releaseDirectory: fixture.storage.releaseDirectory)
    }

    /// Recovers every copy and compares it with what the operation starts
    /// from, or with what it ends in, whichever `ended` says its state is.
    private func checkEveryCopy(
        of faults: SnapshotFaults, _ fixture: SnapshotFixture, initial: [String: String], final: [String: String],
        on release: ReleaseIdentity? = nil, ended: (Int, URL) -> Bool, sourceLocation: SourceLocation = #_sourceLocation
    ) throws {
        let copies = faults.captures
        #expect(copies.count > 3, sourceLocation: sourceLocation)
        for (index, capture) in copies.enumerated() {
            let storage = launch(capture, fixture, on: release)
            let expected = ended(index, capture) ? final : initial
            try storage.recoverSnapshots()
            let after = GuestDirectory.describeIgnoringIdentities(capture)
            let call = index < faults.calls.count
                ? "\(faults.calls[index].name) \((faults.calls[index].path as NSString).lastPathComponent)" : "the end"
            #expect(
                after == expected, "before call \(index + 1) (\(call)): \(GuestDirectory.difference(after, expected))",
                sourceLocation: sourceLocation)
            #expect(GuestDirectory.leftovers(capture).isEmpty, "before call \(index + 1) (\(call))", sourceLocation: sourceLocation)
            // Launching again changes nothing.
            try storage.recoverSnapshots()
            #expect(GuestDirectory.describeIgnoringIdentities(capture) == after, sourceLocation: sourceLocation)
        }
    }

    // MARK: Create

    @Test func aCreateCrashedBetweenAnyTwoCallsIsWholeOrNothingAfterTheNextLaunch() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let initial = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        let faults = SnapshotFaults(guestDirectory: fixture.directory, captureTo: fixture.scratch("copies"))
        _ = try fixture.app(using: faults.operations).createSnapshot(named: "crash", now: crashDate)
        faults.captureNow()
        let final = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        #expect(final != initial)
        let publish = try #require(faults.calls.firstIndex { $0.name == "rename" })

        // No reader sees a half-made snapshot, recovered or not.
        for (index, capture) in faults.captures.enumerated() {
            let published = index > publish
            #expect(launch(capture, fixture).snapshotListing().items.count == (published ? 1 : 0), "before call \(index + 1)")
        }
        try checkEveryCopy(of: faults, fixture, initial: initial, final: final, ended: { index, _ in index > publish })
        // The next create is as if nothing had happened.
        let next = try fixture.storage.createSnapshot(named: "after")
        #expect(fixture.storage.snapshotListing().items.contains { $0.id == next.id })
    }

    @Test func aFailureThatIsNotACrashRemovesItsStagingAtOnce() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        // A destination that cannot be made: a file where the staging folder's parent should be.
        try Data("x".utf8).write(to: fixture.snapshots)
        #expect(throws: (any Error).self) { try fixture.storage.createSnapshot(named: "") }
        #expect(GuestDirectory.leftovers(fixture.directory).isEmpty)
    }

    // MARK: Delete and rename

    @Test func aDeleteCrashedBetweenAnyTwoCallsIsGoneOrWholeAfterTheNextLaunch() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let kept = try fixture.storage.createSnapshot(named: "kept")
        let doomed = try fixture.storage.createSnapshot(named: "doomed")
        let initial = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        let faults = SnapshotFaults(guestDirectory: fixture.directory, captureTo: fixture.scratch("copies"))
        try fixture.app(using: faults.operations).deleteSnapshot(doomed.id)
        faults.captureNow()
        let final = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        let commit = try #require(faults.calls.firstIndex { $0.name == "rename" })

        for (index, capture) in faults.captures.enumerated() {
            let gone = index > commit
            #expect(
                Set(launch(capture, fixture).snapshotListing().items.map(\.id)) == (gone ? [kept.id] : [doomed.id, kept.id]),
                "before call \(index + 1)")
        }
        try checkEveryCopy(of: faults, fixture, initial: initial, final: final, ended: { index, _ in index > commit })
    }

    @Test func aRenameCrashedBetweenAnyTwoCallsHasTheOldNameOrTheNew() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "old")
        let initial = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        let faults = SnapshotFaults(guestDirectory: fixture.directory, captureTo: fixture.scratch("copies"))
        try fixture.app(using: faults.operations).renameSnapshot(made.id, to: "new")
        faults.captureNow()
        let final = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        let commit = try #require(faults.calls.firstIndex { $0.name == "write" })
        try checkEveryCopy(of: faults, fixture, initial: initial, final: final, ended: { index, _ in index > commit })
    }

    // MARK: Restore

    /// The state a restore replaces and what it must end in.
    struct Scenario {
        var fixture: SnapshotFixture
        var app: TwoDiskStorage
        var snapshot: SnapshotID
        var record: Data
        /// A second snapshot of the same data, made before the divergence, when asked for.
        var other: SnapshotID? = nil

        /// A disk of 8 GiB, snapshotted, then moved on: other data, an
        /// update that began and did not finish, and settings that say 4 GiB.
        /// `io` is what the snapshot store of `app` calls.
        static func make(
            io: SnapshotFileOperations = .live, includeInBackups: Bool = false, withOther: Bool = false
        ) async throws -> Scenario {
            let fixture = try await SnapshotFixture.make(gib: 8, includeInBackups: includeInBackups, marker: 1)
            let record = try Data(contentsOf: fixture.files.state)
            let made = try fixture.storage.createSnapshot(named: "good")
            let other = withOther ? try fixture.storage.createSnapshot(named: "other").id : nil
            try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
            let app = fixture.app(using: io, on: snapshotNewer)
            try await app.prepareStart(fixture.guest)
            try TestDisk.write(fixture.data, at: 1 << 30, count: 1 << 20, value: 3)
            var choices = fixture.guest
            choices.capacityGiB = 4
            choices.pendingCapacityGiB = 6
            choices.root = Self.rootAfterTheDivergence
            try app.save(choices)
            return Scenario(fixture: fixture, app: app, snapshot: made.id, record: record, other: other)
        }

        /// The guest as the application holds it before the restore.
        var guest: Guest {
            var choices = fixture.guest
            choices.capacityGiB = 4
            choices.pendingCapacityGiB = 6
            choices.root = Self.rootAfterTheDivergence
            return choices
        }

        /// Root on, with the Magisk app put in after the snapshot was taken:
        /// the snapshot's data disk does not have it.
        static var rootAfterTheDivergence: RootState {
            var root = RootState()
            root.on = true
            root.appVersionCode = 31_100
            root.appEverHandled = true
            return root
        }

        /// The same guest and files with other file calls.
        func using(_ io: SnapshotFileOperations) -> TwoDiskStorage {
            fixture.app(using: io, on: snapshotNewer)
        }

        var intent: URL { fixture.snapshots.appendingPathComponent("restore.json") }
    }

    static func expectRestored(_ scenario: Scenario, sourceLocation: SourceLocation = #_sourceLocation) throws {
        let fixture = scenario.fixture
        #expect(try TestDisk.read(fixture.data, at: 0) == [1], sourceLocation: sourceLocation)
        #expect(try TestDisk.read(fixture.data, at: 1 << 30) == [1], sourceLocation: sourceLocation)
        #expect(try Data(contentsOf: fixture.files.state) == scenario.record, sourceLocation: sourceLocation)
        #expect(!fixture.names().contains { $0.hasPrefix("data.img.pre-") }, sourceLocation: sourceLocation)
        #expect(!exists(scenario.intent), sourceLocation: sourceLocation)
        #expect(GuestDirectory.leftovers(fixture.directory).isEmpty, sourceLocation: sourceLocation)
        // The capacity is the snapshot's disk and the stale growth is gone.
        let settings = try #require(try scenario.app.load(), sourceLocation: sourceLocation)
        #expect(settings.capacityGiB == 8 && settings.pendingCapacityGiB == nil, sourceLocation: sourceLocation)
        // The data disk is the snapshot's, so the Magisk app the application
        // put in later is not on it; the person's switch stays as it was.
        #expect(settings.root.on, sourceLocation: sourceLocation)
        #expect(settings.root.appVersionCode == nil && !settings.root.appEverHandled, sourceLocation: sourceLocation)
        // The snapshot is still whole and can be restored again.
        let items = scenario.app.snapshotListing().items
        #expect(items.map(\.id) == [scenario.snapshot] && items.first?.isDamaged == false, sourceLocation: sourceLocation)
        #expect(scenario.app.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"), sourceLocation: sourceLocation)
        // The guest's backup choice is applied to the files that were put back,
        // and the snapshots are never backed up, whatever it is.
        let included = fixture.guest.includeInBackups
        for url in [fixture.directory, fixture.data, fixture.files.state, fixture.storage.locations.settings] {
            #expect(fixture.settles(url, to: !included), "\(url.lastPathComponent) backup exclusion", sourceLocation: sourceLocation)
        }
        let snapshot = fixture.snapshots.appendingPathComponent(scenario.snapshot.rawValue)
        for url in [fixture.snapshots, snapshot] + ["data.img", "instance.json", "snapshot.json"].map({ snapshot.appendingPathComponent($0) }) {
            #expect(fixture.settles(url, to: true), "\(url.lastPathComponent) is backed up", sourceLocation: sourceLocation)
        }
    }

    @Test func anUninterruptedRestoreReachesTheFinalState() async throws {
        let scenario = try await Scenario.make()
        defer { scenario.fixture.cleanUp() }
        _ = try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest)
        try Self.expectRestored(scenario)
    }

    @Test func aRestoreCrashedBetweenAnyTwoCallsIsCarriedThroughOrNeverBegunAfterTheNextLaunch() async throws {
        let setup = try await Scenario.make()
        defer { setup.fixture.cleanUp() }
        let fixture = setup.fixture
        let faults = SnapshotFaults(guestDirectory: fixture.directory, captureTo: fixture.scratch("copies"))
        let scenario = Scenario(
            fixture: fixture, app: setup.using(faults.operations), snapshot: setup.snapshot, record: setup.record)
        let initial = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        _ = try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest)
        faults.captureNow()
        let final = GuestDirectory.describeIgnoringIdentities(fixture.directory)
        try Self.expectRestored(scenario)

        // Before any recovery: the snapshot's data is never in place without
        // the intent that finishes the restore.
        for (index, capture) in faults.captures.enumerated() {
            let intent = exists(capture.appendingPathComponent("Snapshots/restore.json"))
            let replaced = try TestDisk.read(capture.appendingPathComponent("data.img"), at: 0) == [1]
            let finished = GuestDirectory.describeIgnoringIdentities(capture) == final
            #expect(
                !replaced || intent || finished,
                "before call \(index + 1): the data is replaced and nothing says a restore is under way")
            // The guest's update did not finish before the restore, so Create says that when no restore is.
            #expect(
                launch(capture, fixture, on: snapshotNewer).snapshotListing().createRefusal
                    == (intent ? .restoreUnfinished : (finished ? nil : .unfinishedUpdate)),
                "before call \(index + 1)")
        }
        try checkEveryCopy(
            of: faults, fixture, initial: initial, final: final, on: snapshotNewer,
            ended: { _, capture in
                exists(capture.appendingPathComponent("Snapshots/restore.json"))
                    || GuestDirectory.describeIgnoringIdentities(capture) == final
            })
    }

    /// A restore that stopped after its data was in place, leaving its intent:
    /// the write of the record failed.
    private func stoppedRestore() async throws -> Scenario {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let setup = try await Scenario.make(io: faults.operations)
        faults.restart()
        faults.fail(when: { $0.name == "write" && $0.path.hasSuffix("/instance.json") })
        #expect(throws: (any Error).self) { try setup.app.restoreSnapshot(setup.snapshot, guest: setup.guest) }
        #expect(exists(setup.intent))
        return setup
    }

    /// A failure in the recovery itself, at any of its calls: the next
    /// recovery carries the restore through from the top.
    @Test func aFailureInTheRecoveryIsRecoveredAgain() async throws {
        // How many calls a recovery makes here.
        let counting = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let probe = try await stoppedRestore()
        defer { probe.fixture.cleanUp() }
        let recovery = probe.using(counting.operations)
        try recovery.recoverSnapshots()
        try Self.expectRestored(probe)
        let calls = counting.calls.count
        #expect(calls >= 5)

        for failing in 1...calls {
            let scenario = try await stoppedRestore()
            defer { scenario.fixture.cleanUp() }
            let faults = SnapshotFaults(guestDirectory: scenario.fixture.directory)
            faults.fail(call: failing)
            // The failed recovery may have gone through (a call it ignores) or not.
            _ = try? scenario.using(faults.operations).recoverSnapshots()
            try scenario.app.recoverSnapshots()
            try Self.expectRestored(scenario)
            // Launching again changes nothing.
            try scenario.app.recoverSnapshots()
            try Self.expectRestored(scenario)
        }
    }
}
