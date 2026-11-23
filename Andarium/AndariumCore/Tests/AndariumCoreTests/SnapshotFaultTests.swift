// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

private func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }

private let faultDate = Date(timeIntervalSince1970: 1_800_000_000)

/// A real I/O error at each file call in turn, the store's own error paths
/// running: an operation that fails changes nothing or finishes through the
/// next launch, and one that fails after its commit point does not report a
/// failure of what took effect.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotFaultTests {
    private let intentPath = "Snapshots/restore.json"

    // MARK: Restore

    /// What one restore's calls are, from an uninterrupted run: how many, and
    /// which one renames the clone over `data.img`.
    private func learnRestoreCalls() async throws -> (count: Int, commit: Int) {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
        defer { scenario.fixture.cleanUp() }
        faults.restart()
        _ = try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest)
        let commit = try #require(faults.calls.firstIndex { $0.name == "rename" && $0.path.hasSuffix("/data.img") })
        return (faults.calls.count, commit + 1)
    }

    @Test func aRestoreThatFailsAtAnyCallIsNeverBegunOrFinishesThroughTheNextLaunch() async throws {
        let learned = try await learnRestoreCalls()
        #expect(learned.count >= 12 && learned.commit > 3 && learned.commit < learned.count)
        var failedBeforeCommit = 0
        var failedAfterCommit = 0
        var ignored = 0

        for failing in 1...learned.count {
            let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
            let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
            defer { scenario.fixture.cleanUp() }
            let directory = scenario.fixture.directory
            let initial = GuestDirectory.describeIgnoringIdentities(directory)
            faults.restart()
            faults.fail(call: failing)
            let label = "call \(failing) (\(faults.calls.count))"

            var error: (any Error)?
            do { _ = try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) } catch let thrown { error = thrown }
            let call = faults.calls.indices.contains(failing - 1) ? faults.calls[failing - 1].name : "?"

            if let unfinished = error as? SnapshotRestoreUnfinished {
                failedAfterCommit += 1
                #expect(failing > learned.commit, "\(label) \(call): failed after the commit")
                _ = unfinished.cause
                // The data is the snapshot's and the intent stands.
                #expect(exists(scenario.intent), "\(label) \(call): a restore that took effect has no intent")
                #expect(try TestDisk.read(scenario.fixture.data, at: 0) == [1], "\(label)")
                // The next launch finishes it.
                try scenario.app.recoverSnapshots()
                try SnapshotCrashTests.expectRestored(scenario)
            } else if error != nil {
                failedBeforeCommit += 1
                #expect(failing <= learned.commit, "\(label) \(call): failed before the commit")
                // Nothing changed and nothing is left to finish.
                #expect(
                    GuestDirectory.describeIgnoringIdentities(directory) == initial,
                    "\(label) \(call): \(GuestDirectory.difference(GuestDirectory.describeIgnoringIdentities(directory), initial))")
                #expect(!exists(scenario.intent))
                #expect(GuestDirectory.leftovers(directory).isEmpty)
                #expect(try TestDisk.read(scenario.fixture.data, at: 0) == [2])
                #expect(scenario.app.snapshotListing().createRefusal == .unfinishedUpdate)
            } else {
                // A call whose failure the store ignores (a flush of a directory, a cleanup).
                ignored += 1
                try SnapshotCrashTests.expectRestored(scenario)
            }
        }
        #expect(failedBeforeCommit >= 5 && failedAfterCommit >= 5 && ignored >= 1)
    }

    @Test func aRestoreThatRunsOutOfRoomRefusesAsSuch() async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
        defer { scenario.fixture.cleanUp() }
        let initial = GuestDirectory.describeIgnoringIdentities(scenario.fixture.directory)
        faults.restart()
        faults.fail(when: { $0.name == "clone" }, with: CloneFile.Failure(code: ENOSPC))
        #expect(throws: SnapshotRefusal.notEnoughSpace) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        #expect(GuestDirectory.describeIgnoringIdentities(scenario.fixture.directory) == initial)
    }

    @Test func aRestoreOnAVolumeThatCannotCloneRefusesAsSuch() async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
        defer { scenario.fixture.cleanUp() }
        let initial = GuestDirectory.describeIgnoringIdentities(scenario.fixture.directory)
        faults.restart()
        faults.fail(when: { $0.name == "clone" }, with: CloneFile.Failure(code: ENOTSUP))
        #expect(throws: SnapshotRefusal.volumeCannotClone) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        #expect(GuestDirectory.describeIgnoringIdentities(scenario.fixture.directory) == initial)
    }

    /// The update's own clone keeps the error and the words it always had; the
    /// store's refusals are not routed through it.
    @Test func theUpdatesOwnCloneKeepsItsErrorAndItsWords() throws {
        let missing = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-no-such-\(UUID().uuidString)")
        let destination = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-clone-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: destination) }
        let reason = String(cString: strerror(ENOENT))
        #expect(throws: InstanceError.cannotClone(reason)) { try InstanceFiles.clone(missing, to: destination) }
        do {
            try InstanceFiles.clone(missing, to: destination)
        } catch let error as InstanceError {
            #expect(
                error.errorDescription == "The virtual machine's data cannot be snapshotted (it needs an APFS volume): \(reason)")
        }
        #expect(!FileManager.default.fileExists(atPath: destination.path))
        #expect(!FileManager.default.fileExists(atPath: destination.appendingPathExtension("tmp").path))
    }

    // MARK: Create

    @Test func aCreateThatFailsAtAnyCallLeavesNothingOrTheWholeSnapshot() async throws {
        let counting = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let probe = try await SnapshotFixture.make()
        defer { probe.cleanUp() }
        _ = try probe.app(using: counting.operations).createSnapshot(named: "x", now: faultDate)
        let count = counting.calls.count
        let commit = try #require(counting.calls.firstIndex { $0.name == "rename" }) + 1
        #expect(count >= 8)

        for failing in 1...count {
            let fixture = try await SnapshotFixture.make()
            defer { fixture.cleanUp() }
            let initial = GuestDirectory.describeIgnoringIdentities(fixture.directory)
            let faults = SnapshotFaults(guestDirectory: fixture.directory)
            faults.fail(call: failing)
            let app = fixture.app(using: faults.operations)
            do {
                let made = try app.createSnapshot(named: "x", now: faultDate)
                // The failure was in a call the store ignores, after the commit or beside it.
                #expect(app.snapshotListing().items.map(\.id) == [made.id], "call \(failing)")
            } catch {
                #expect(failing <= commit, "call \(failing) failed after the commit and the create reported it")
                #expect(
                    GuestDirectory.describeIgnoringIdentities(fixture.directory) == initial, "call \(failing)")
                #expect(GuestDirectory.leftovers(fixture.directory).isEmpty, "call \(failing)")
            }
            // Either way the next launch has nothing to do.
            let before = GuestDirectory.describeIgnoringIdentities(fixture.directory)
            try fixture.storage.recoverSnapshots()
            #expect(GuestDirectory.describeIgnoringIdentities(fixture.directory) == before, "call \(failing)")
        }
    }

    // MARK: Delete and rename

    @Test func aDeleteThatFailsAtAnyCallIsWholeOrGone() async throws {
        let counting = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let probe = try await SnapshotFixture.make()
        defer { probe.cleanUp() }
        let one = try probe.storage.createSnapshot(named: "one")
        try probe.app(using: counting.operations).deleteSnapshot(one.id)
        let count = counting.calls.count
        let commit = try #require(counting.calls.firstIndex { $0.name == "rename" }) + 1
        #expect(count >= 3)

        for failing in 1...count {
            let fixture = try await SnapshotFixture.make()
            defer { fixture.cleanUp() }
            let kept = try fixture.storage.createSnapshot(named: "kept")
            let doomed = try fixture.storage.createSnapshot(named: "doomed")
            let initial = GuestDirectory.describeIgnoringIdentities(fixture.directory)
            let faults = SnapshotFaults(guestDirectory: fixture.directory)
            faults.fail(call: failing)
            let app = fixture.app(using: faults.operations)
            do {
                try app.deleteSnapshot(doomed.id)
                // It took effect: it is not listed, and what is left of it goes with the next launch.
                #expect(Set(app.snapshotListing().items.map(\.id)) == [kept.id], "call \(failing)")
            } catch {
                #expect(failing <= commit, "call \(failing) failed after the commit and the delete reported it")
                #expect(GuestDirectory.describeIgnoringIdentities(fixture.directory) == initial, "call \(failing)")
            }
            try fixture.storage.recoverSnapshots()
            #expect(GuestDirectory.leftovers(fixture.directory).isEmpty, "call \(failing)")
            let listed = Set(fixture.storage.snapshotListing().items.map(\.id))
            #expect(listed == [kept.id] || listed == [kept.id, doomed.id], "call \(failing)")
        }
    }

    @Test func aRenameThatFailsAtAnyCallHasTheOldNameOrTheNew() async throws {
        let counting = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let probe = try await SnapshotFixture.make()
        defer { probe.cleanUp() }
        let one = try probe.storage.createSnapshot(named: "old")
        try probe.app(using: counting.operations).renameSnapshot(one.id, to: "new")
        let count = counting.calls.count
        let commit = try #require(counting.calls.firstIndex { $0.name == "write" }) + 1
        #expect(count >= 3)

        for failing in 1...count {
            let fixture = try await SnapshotFixture.make()
            defer { fixture.cleanUp() }
            let made = try fixture.storage.createSnapshot(named: "old")
            let faults = SnapshotFaults(guestDirectory: fixture.directory)
            faults.fail(call: failing)
            let app = fixture.app(using: faults.operations)
            do {
                try app.renameSnapshot(made.id, to: "new")
                #expect(app.snapshotListing().items.first?.name == "new", "call \(failing)")
            } catch {
                #expect(failing <= commit, "call \(failing) failed after the commit and the rename reported it")
                #expect(app.snapshotListing().items.first?.name == "old", "call \(failing)")
            }
        }
    }
}

/// A restore that cannot finish leaves the person able to make room and says
/// which restore it is.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotUnfinishedRestoreTests {
    @Test func afterAFailedRollForwardTheLeftoversAreGoneOtherSnapshotsCanBeDeletedAndTheListingSaysWhich() async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations, withOther: true)
        defer { scenario.fixture.cleanUp() }
        let other = try #require(scenario.other)
        // A half-deleted snapshot and a half-made one hold space.
        for prefix in [".gone-", ".new-"] {
            let leftover = scenario.fixture.snapshots.appendingPathComponent(prefix + SnapshotID().rawValue)
            try FileManager.default.createDirectory(at: leftover, withIntermediateDirectories: true)
            try Data(count: 1 << 20).write(to: leftover.appendingPathComponent("data.img"))
        }
        // A restore that took effect and stopped after the commit.
        faults.restart()
        faults.fail(when: { $0.name == "write" && $0.path.hasSuffix("/instance.json") })
        let result = SnapshotResult<Guest>(catching: { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) })
        #expect(result.refusal == .restoreUnfinished)
        #expect(FileManager.default.fileExists(atPath: scenario.intent.path))

        // The next launch cannot finish it (no room, here), and still clears the leftovers first.
        faults.restart()
        faults.fail(when: { $0.name == "clone" }, with: CloneFile.Failure(code: ENOSPC))
        #expect(throws: SnapshotRestoreUnfinished.self) { try scenario.app.recoverSnapshots() }
        #expect(GuestDirectory.leftovers(scenario.fixture.directory).isEmpty)
        #expect(FileManager.default.fileExists(atPath: scenario.intent.path))

        // The listing says which restore, and why it is not finished is for the caller to add.
        let listing = scenario.app.snapshotListing()
        #expect(listing.unfinishedRestore == SnapshotUnfinishedRestore(snapshot: scenario.snapshot))
        #expect(listing.createRefusal == .restoreUnfinished)

        // Room is made by deleting another snapshot; the one being restored stays.
        #expect(throws: SnapshotRefusal.restoreUnfinished) { try scenario.app.deleteSnapshot(scenario.snapshot) }
        try scenario.app.deleteSnapshot(other)
        #expect(scenario.app.snapshotListing().items.map(\.id) == [scenario.snapshot])
        // And the restore finishes once there is room again.
        try scenario.app.recoverSnapshots()
        try SnapshotCrashTests.expectRestored(scenario)
        #expect(scenario.app.snapshotListing().unfinishedRestore == nil)
    }

    @Test func aFailureIsAValueWithTheSystemsOwnDescription() {
        let failure = SnapshotFailure(POSIXError(.ENOSPC))
        #expect(failure.domain == NSPOSIXErrorDomain && failure.code == Int(ENOSPC) && !failure.description.isEmpty)
    }

    /// A clone that is refused while a restore finishes reaches the person in
    /// the system's words for the errno, and the record keeps the errno, not a
    /// Swift type name and a case index.
    @Test func aCloneFailureThatStopsARestoreKeepsItsErrnoAndItsWords() async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
        defer { scenario.fixture.cleanUp() }
        faults.restart()
        faults.fail(when: { $0.name == "write" && $0.path.hasSuffix("/instance.json") })
        #expect(throws: SnapshotRestoreUnfinished.self) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }

        let reason = String(cString: strerror(EIO))
        faults.restart()
        faults.fail(when: { $0.name == "clone" }, with: CloneFile.Failure(code: EIO))
        do {
            try scenario.app.recoverSnapshots()
            Issue.record("the recovery was meant to fail")
        } catch let unfinished as SnapshotRestoreUnfinished {
            #expect(unfinished.localizedDescription == reason)
            #expect(unfinished.errorDescription == reason)
            let failure = SnapshotFailure(unfinished.cause)
            #expect(failure.domain == NSPOSIXErrorDomain && failure.code == Int(EIO) && failure.description == reason)
        }
    }

    /// A restore that stopped after its commit, recovered on a launch whose
    /// backup choice is `includeInBackups`.
    @Test(arguments: [true, false])
    func aRecoveredRestoreAppliesTheGuestsBackupChoiceToWhatItPutBack(includeInBackups: Bool) async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations, includeInBackups: includeInBackups)
        defer { scenario.fixture.cleanUp() }
        faults.restart()
        faults.fail(when: { $0.name == "write" && $0.path.hasSuffix("/instance.json") })
        #expect(throws: SnapshotRestoreUnfinished.self) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        try scenario.app.recoverSnapshots()
        try SnapshotCrashTests.expectRestored(scenario)
    }

    @Test func aRecoveryOfAGuestWithNoSettingsStillRestoresTheFiles() async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
        defer { scenario.fixture.cleanUp() }
        faults.restart()
        faults.fail(when: { $0.name == "write" && $0.path.hasSuffix("/instance.json") })
        #expect(throws: SnapshotRestoreUnfinished.self) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        // The settings are gone (a guest erased, or never saved): there is nothing to settle.
        try FileManager.default.removeItem(at: scenario.fixture.storage.locations.settings)
        try scenario.app.recoverSnapshots()
        #expect(try TestDisk.read(scenario.fixture.data, at: 0) == [1])
        #expect(try Data(contentsOf: scenario.fixture.files.state) == scenario.record)
        #expect(!FileManager.default.fileExists(atPath: scenario.intent.path))
        #expect(!scenario.fixture.names().contains { $0.hasPrefix("data.img.pre-") })
        #expect(try scenario.app.load() == nil)
    }

    /// The update's own Restore waits behind a snapshot restore that did not
    /// finish: that one completes first, and nothing is left for this one.
    @Test func theUpdatesRestoreCompletesAnUnfinishedSnapshotRestoreFirst() async throws {
        let faults = SnapshotFaults(guestDirectory: URL(fileURLWithPath: "/"))
        let scenario = try await SnapshotCrashTests.Scenario.make(io: faults.operations)
        defer { scenario.fixture.cleanUp() }
        // The files as a restore that stopped after its commit leaves them: the
        // snapshot's data under the record of the update that did not finish.
        faults.restart()
        faults.fail(when: { $0.name == "write" && $0.path.hasSuffix("/instance.json") })
        #expect(throws: SnapshotRestoreUnfinished.self) { try scenario.app.restoreSnapshot(scenario.snapshot, guest: scenario.guest) }
        #expect(try scenario.fixture.files.readState()?.pendingUpdate != nil)

        #expect(throws: InstanceError.refused("No update is pending.")) { try scenario.app.restoreUpdate() }
        try SnapshotCrashTests.expectRestored(scenario)
    }
}

/// Erase takes the snapshots with the rest, and a failure part-way leaves the
/// guest in view for another try.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotEraseTests {
    @Test func anEraseThatCannotRemoveASnapshotFileFailsAndAnotherTryFinishes() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        _ = try fixture.storage.createSnapshot(named: "one")
        let two = try fixture.storage.createSnapshot(named: "two")
        // A file the volume will not remove: immutable.
        let stuck = fixture.snapshots.appendingPathComponent(two.id.rawValue).appendingPathComponent("data.img")
        #expect(chflags(stuck.path, UInt32(UF_IMMUTABLE)) == 0)
        defer { _ = chflags(stuck.path, 0) }

        await #expect(throws: (any Error).self) { try await fixture.storage.erase() }
        // The data and the record are gone and the snapshots stay as far as the
        // file system went; the guest is still in view, for another try.
        #expect(!FileManager.default.fileExists(atPath: fixture.data.path))
        #expect(!FileManager.default.fileExists(atPath: fixture.files.state.path))
        #expect(FileManager.default.fileExists(atPath: stuck.path))
        #expect(try fixture.storage.load() != nil)

        #expect(chflags(stuck.path, 0) == 0)
        try await fixture.storage.erase()
        #expect(try fixture.storage.load() == nil)
        #expect(!FileManager.default.fileExists(atPath: fixture.snapshots.path))
    }

    @Test func anEraseInterruptedAfterTheDataWentFinishesWithTheSnapshotsAndTheChoices() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        _ = try fixture.storage.createSnapshot(named: "kept")
        // A crash after the data and the record were removed, the snapshots and the choices not yet.
        try FileManager.default.removeItem(at: fixture.data)
        try FileManager.default.removeItem(at: fixture.files.state)
        #expect(try fixture.storage.load() != nil)

        try await fixture.storage.erase()
        #expect(try fixture.storage.load() == nil)
        #expect(!FileManager.default.fileExists(atPath: fixture.snapshots.path))
        #expect(fixture.names().allSatisfy { !$0.hasPrefix("Snapshots") && !$0.hasPrefix("data.img") })
    }
}
