// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

let snapshotNewer = snapshotRelease("20261102-los24-r5", build: 200, sha: String(repeating: "b", count: 64))

private func contents(_ url: URL) throws -> Data { try Data(contentsOf: url) }

private func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }

/// What a crash right after a restore's intent leaves: the intent, with the
/// data as it was.
private func writeIntent(for id: SnapshotID, in fixture: SnapshotFixture) throws {
    try Data(#"{"schema": 1, "snapshot": "\#(id.rawValue)"}"#.utf8)
        .write(to: fixture.snapshots.appendingPathComponent("restore.json"))
}

/// The guest rewrites 2 MiB of data the disk holds: the first mebibyte and the
/// one at 1 GiB (a hole would cost a snapshot nothing).
private func rewriteTwoMiB(_ fixture: SnapshotFixture) throws {
    try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 9)
    try TestDisk.write(fixture.data, at: 1 << 30, count: 1 << 20, value: 9)
}

/// The operations on real files in a temporary directory: APFS clones of a
/// sparse disk, the records and the settings they touch.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct SnapshotStoreTests {
    // MARK: Create

    @Test func aSnapshotIsACloneOfTheDataAndACopyOfTheRecord() async throws {
        let fixture = try await SnapshotFixture.make(gib: 4, marker: 1)
        defer { fixture.cleanUp() }
        let record = try contents(fixture.files.state)

        let made = try fixture.storage.createSnapshot(named: "  Before the update \n")
        let folder = fixture.snapshots.appendingPathComponent(made.id.rawValue)
        #expect(made.name == "Before the update")
        #expect(made.diskGiB == 4 && made.releaseID == "20261031-los24-r4" && made.buildNumber == 100)
        #expect(made.restore == .asIs && !made.isDamaged)
        // A clone holds everything the disk held and costs nothing yet.
        #expect(made.kept.bytes >= 3 << 20)
        #expect(made.freesWhenDeleted.bytes < 1 << 20)
        #expect(try contents(folder.appendingPathComponent("instance.json")) == record)
        #expect(TestDisk.fingerprint(folder.appendingPathComponent("data.img")) == TestDisk.fingerprint(fixture.data))
        #expect(fixture.storage.snapshotListing().items.map(\.id) == [made.id])
        // The live disk and its record are untouched.
        #expect(try contents(fixture.files.state) == record)
        #expect(fixture.names().filter { $0.contains(".new-") || $0.contains(".gone-") }.isEmpty)
    }

    @Test func laterWritesToTheDiskNeverReachTheSnapshot() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 9)
        let snapshot = fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent("data.img")
        #expect(try TestDisk.read(snapshot, at: 0) == [1])
        #expect(try TestDisk.read(fixture.data, at: 0) == [9])
    }

    @Test(arguments: [true, false])
    func snapshotsAreNeverBackedUpWhateverTheGuestsChoiceIs(includeInBackups: Bool) async throws {
        let fixture = try await SnapshotFixture.make(includeInBackups: includeInBackups)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "Keep")
        let folder = fixture.snapshots.appendingPathComponent(made.id.rawValue)
        // The guest's own files follow its choice.
        #expect(fixture.settles(fixture.data, to: !includeInBackups))
        for url in [fixture.snapshots, folder] + ["data.img", "instance.json", "snapshot.json"].map({ folder.appendingPathComponent($0) }) {
            #expect(fixture.settles(url, to: true), "\(url.lastPathComponent)")
        }
        // A later save of the choices keeps the snapshots out, and a rename
        // writes a new snapshot.json that is out as well.
        try fixture.storage.save(fixture.guest)
        try fixture.storage.renameSnapshot(made.id, to: "Renamed")
        #expect(fixture.settles(fixture.snapshots, to: true))
        #expect(fixture.settles(folder.appendingPathComponent("snapshot.json"), to: true))
        #expect(fixture.settles(fixture.data, to: !includeInBackups))
    }

    @Test func createRefusesEachWayTheDiskIsNotOneToKeepAndChangesNothing() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let storage = fixture.storage

        #expect(throws: SnapshotRefusal.invalidName) { try storage.createSnapshot(named: "a\u{0}b") }
        // Another system disk under the same release.
        let other = fixture.app(on: snapshotRelease(sha: String(repeating: "c", count: 64)))
        #expect(throws: SnapshotRefusal.mismatch) { try other.createSnapshot(named: "") }
        // The last start did not end with Android's own shut down.
        try fixture.startWithoutShutDown(at: fixture.origin.addingTimeInterval(60))
        #expect(throws: SnapshotRefusal.notShutDown) { try storage.createSnapshot(named: "") }
        #expect(storage.snapshotListing().createRefusal == .notShutDown)
        // Android shut down again after it.
        try fixture.shutDown(at: fixture.origin.addingTimeInterval(120))
        #expect(storage.snapshotListing().createRefusal == nil)
        // No data disk (a guest that has not started once).
        try FileManager.default.removeItem(at: fixture.data)
        #expect(throws: SnapshotRefusal.noData) { try storage.createSnapshot(named: "") }
        #expect(!exists(fixture.snapshots), "a refusal makes nothing")
    }

    @Test func theVolumeAnswersWhetherItClonesAndCreateSaysSoInAdvance() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        // This volume is one that clones (the suite runs only where it does).
        #expect(VolumeCapabilities.clones(at: fixture.directory) == true)
        #expect(fixture.storage.snapshotListing().createRefusal == nil)

        var io = SnapshotFileOperations.live
        io.volumeClones = { _ in false }
        let app = fixture.app(using: io)
        // It comes before what the person could change: a guest not shut down is told the permanent thing first.
        try fixture.startWithoutShutDown(at: fixture.origin.addingTimeInterval(120))
        #expect(app.snapshotListing().createRefusal == .volumeCannotClone)
        #expect(throws: SnapshotRefusal.volumeCannotClone) { try app.createSnapshot(named: "x") }
        #expect(GuestDirectory.describe(fixture.directory).filter { $0.key.hasPrefix("Snapshots") }.isEmpty)

        // A volume that does not say is not a refusal; the clone itself decides.
        io.volumeClones = { _ in nil }
        #expect(fixture.app(using: io).snapshotListing().createRefusal == .notShutDown)
    }

    @Test func createRefusesAnUnreadableRecordAndAnUnfinishedUpdate() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        // An update that began on a newer Android and did not finish.
        let newer = fixture.app(on: snapshotNewer)
        try await newer.prepareStart(fixture.guest)
        #expect(try fixture.files.readState()?.pendingUpdate != nil)
        for storage in [newer, fixture.storage] {
            #expect(throws: SnapshotRefusal.unfinishedUpdate) { try storage.createSnapshot(named: "") }
        }
        #expect(newer.snapshotListing().createRefusal == .unfinishedUpdate)

        try Data("not json".utf8).write(to: fixture.files.state)
        #expect(throws: SnapshotRefusal.unreadableRecord) { try fixture.storage.createSnapshot(named: "") }
        #expect(!exists(fixture.snapshots))
    }

    @Test func theListingNamesTheReleaseTheDataIsOnWhateverThePlanIs() async throws {
        let fixture = try await SnapshotFixture.make(release: snapshotNewer)
        defer { fixture.cleanUp() }
        #expect(fixture.storage.snapshotListing().dataRelease == SnapshotRelease(releaseID: snapshotNewer.releaseID, buildNumber: 200))
        // An older app than the data: a start is blocked, and the release is still said.
        let older = fixture.app(on: snapshotRelease())
        #expect(older.startPlan() == .blocked(.olderAndroid))
        #expect(older.snapshotListing().dataRelease == SnapshotRelease(releaseID: snapshotNewer.releaseID, buildNumber: 200))
        // A record that cannot be read, or none, says nothing.
        try Data("not json".utf8).write(to: fixture.files.state)
        #expect(fixture.storage.snapshotListing().dataRelease == nil)
        try FileManager.default.removeItem(at: fixture.files.state)
        #expect(fixture.storage.snapshotListing().dataRelease == nil)
    }

    @Test func anUnmeasuredListingHasEverythingButTheFiguresAndReadsNoDisk() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        _ = try fixture.storage.createSnapshot(named: "first")
        let second = try fixture.storage.createSnapshot(named: "second")
        try rewriteTwoMiB(fixture)
        try fixture.storage.deleteSnapshot(second.id)
        _ = try fixture.storage.createSnapshot(named: "third")

        // No measuring call is made at all.
        var io = SnapshotFileOperations.live
        io.measure = SnapshotMeasure.Primitives(
            privateSize: { _ in Issue.record("a private size was asked for"); return nil },
            extents: { _ in Issue.record("an extent map was asked for"); return nil })
        let quick = fixture.app(using: io).snapshotListing(measured: false)
        let full = fixture.storage.snapshotListing()

        #expect(full.isMeasured && !quick.isMeasured)
        #expect(quick.items.count == 2 && quick.items.map(\.id) == full.items.map(\.id))
        #expect(quick.items.map(\.name) == full.items.map(\.name) && quick.items.map(\.restore) == full.items.map(\.restore))
        #expect(quick.items.map(\.created) == full.items.map(\.created))
        #expect(quick.createRefusal == full.createRefusal && quick.dataRelease == full.dataRelease)
        // Every figure says it knows nothing.
        let unknown = SnapshotFigure(bytes: 0, isLowerBound: true)
        #expect(quick.items.allSatisfy { $0.freesWhenDeleted == unknown && $0.kept == unknown })
        #expect(quick.space.snapshotsUse == unknown)
    }

    @Test func aListingThatIsCancelledStopsMeasuringAndAnswersUnmeasured() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        for name in ["one", "two", "three"] { _ = try fixture.storage.createSnapshot(named: name) }
        try rewriteTwoMiB(fixture)

        /// The measuring calls made, and the moment a test cancels the task that makes them.
        final class Calls: @unchecked Sendable {
            private let lock = NSLock()
            private var made = 0
            var cancelAt: Int?
            var count: Int { lock.withLock { made } }
            func call() {
                let now = lock.withLock { made += 1; return made }
                if now == cancelAt { withUnsafeCurrentTask { $0?.cancel() } }
            }
        }
        func listing(cancelAt: Int?, beforeStart: Bool = false) async -> (SnapshotListing, Int) {
            let calls = Calls()
            calls.cancelAt = cancelAt
            var io = SnapshotFileOperations.live
            io.measure = SnapshotMeasure.Primitives(
                privateSize: { calls.call(); return SnapshotMeasure.privateSize(of: $0) },
                extents: { calls.call(); return SnapshotMeasure.physicalExtents(of: $0) })
            let app = fixture.app(using: io)
            let answer = await Task.detached {
                if beforeStart { withUnsafeCurrentTask { $0?.cancel() } }
                return app.snapshotListing(measured: true)
            }.value
            return (answer, calls.count)
        }

        let (whole, wholeCalls) = await listing(cancelAt: nil)
        #expect(whole.isMeasured && wholeCalls > 3)
        // Cancelled before it starts: no measuring call at all.
        let (early, earlyCalls) = await listing(cancelAt: nil, beforeStart: true)
        #expect(!early.isMeasured && earlyCalls == 0)
        // Cancelled by the first call: it stops there, between two measures.
        let (partial, partialCalls) = await listing(cancelAt: 1)
        #expect(!partial.isMeasured && partialCalls < wholeCalls)
        // What the unmeasured answer says is what a quick read says: every name, no figure.
        let quick = fixture.storage.snapshotListing(measured: false)
        #expect(partial.items.map(\.id) == quick.items.map(\.id))
        let unknown = SnapshotFigure(bytes: 0, isLowerBound: true)
        #expect(partial.items.allSatisfy { $0.freesWhenDeleted == unknown && $0.kept == unknown })
        #expect(partial.space.snapshotsUse == unknown)
    }

    @Test func createIsAllowedWhileANewerAndroidWaitsToUpdateTheData() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let newer = fixture.app(on: snapshotNewer)
        #expect(newer.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"))
        let made = try newer.createSnapshot(named: "Before r5")
        #expect(made.releaseID == "20261031-los24-r4")
        // Restoring it on that app updates at the next start, as the guest would.
        #expect(made.restore == .updatesAtNextStart(from: "20261031-los24-r4", to: "20261102-los24-r5"))
    }

    @Test func createIsAllowedWhenThisAppIsOlderThanTheData() async throws {
        let fixture = try await SnapshotFixture.make(release: snapshotNewer)
        defer { fixture.cleanUp() }
        let older = fixture.app(on: snapshotRelease())
        #expect(older.startPlan() == .blocked(.olderAndroid))
        let made = try older.createSnapshot(named: "Kept for a newer app")
        // The snapshot cannot be restored here: its Android is newer.
        #expect(made.restore == .refused(.newerAndroid))
        #expect(throws: SnapshotRefusal.newerAndroid) { try older.restoreSnapshot(made.id, guest: fixture.guest) }
        // A newer app can.
        #expect(fixture.storage.snapshotListing().items.first?.restore == .asIs)
    }

    @Test func aRestoreThatIsUnderWayRefusesEveryOtherOperation() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        // A restore that crashed right after its intent was written.
        try writeIntent(for: made.id, in: fixture)

        let operations: [() throws -> Void] = [
            { _ = try fixture.storage.createSnapshot(named: "") },
            { try fixture.storage.deleteSnapshot(made.id) },
            { _ = try fixture.storage.restoreSnapshot(made.id, guest: fixture.guest) },
        ]
        for operation in operations {
            #expect(throws: SnapshotRefusal.restoreUnfinished, performing: operation)
        }
        #expect(fixture.storage.snapshotListing().createRefusal == .restoreUnfinished)
        // The next launch finishes it.
        try fixture.storage.recoverSnapshots()
        #expect(fixture.storage.snapshotListing().createRefusal == nil)
    }

    // MARK: Restore

    /// The state a restore replaces: other data, an update that began and did
    /// not finish (with its copy), and a larger capacity than the snapshot's disk.
    private func diverged(_ fixture: SnapshotFixture) async throws -> TwoDiskStorage {
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
        let newer = fixture.app(on: snapshotNewer)
        try await newer.prepareStart(fixture.guest)
        try TestDisk.write(fixture.data, at: 1 << 30, count: 1 << 20, value: 3)
        return newer
    }

    @Test func restoreReplacesTheDataAndTheRecordAndKeepsNoCurrentState() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let record = try contents(fixture.files.state)
        // The installation's identity (synthetic here) is not part of a snapshot: a restore goes back in
        // time on the same installation and keeps the one it has.
        let identity = Data(#"{"ethernet_mac": "02:aa:bb:cc:dd:ee", "schema": 1, "serialno": "0123456789abcdef"}"#.utf8)
        try identity.write(to: fixture.files.identity)
        let made = try fixture.storage.createSnapshot(named: "Good state")
        #expect(!exists(fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent(InstanceFiles.identityFile)))
        let newer = try await diverged(fixture)
        #expect(fixture.names().contains("data.img.pre-20261102-los24-r5"))
        #expect(try TestDisk.read(fixture.data, at: 0) == [2])

        let guest = try newer.restoreSnapshot(made.id, guest: fixture.guest)

        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        #expect(try TestDisk.read(fixture.data, at: 1 << 30) == [1])
        #expect(try contents(fixture.files.state) == record)
        #expect(try contents(fixture.files.identity) == identity)
        // The unfinished update ended with its copy, and nothing of the replaced state was kept.
        #expect(!fixture.names().contains { $0.hasPrefix("data.img.pre-") })
        #expect(newer.snapshotListing().items.map(\.id) == [made.id])
        #expect(!exists(fixture.snapshots.appendingPathComponent("restore.json")))
        #expect(newer.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"))
        #expect(guest.usedBytes == newer.measuredUsedBytes() && guest.usedBytes > 0)
        // The snapshot stays and can be restored again.
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 4)
        _ = try fixture.storage.restoreSnapshot(made.id, guest: guest)
        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        // The restored disk is independent of the snapshot.
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 5)
        #expect(try TestDisk.read(fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent("data.img"), at: 0) == [1])
    }

    @Test func restoreRaisesTheCapacityToTheSnapshotsDiskAndKeepsALargerGrowth() async throws {
        let fixture = try await SnapshotFixture.make(gib: 8, marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        #expect(made.diskGiB == 8)

        // The choices say less than the disk the snapshot holds (and a growth that is no longer one).
        var choices = fixture.guest
        choices.capacityGiB = 4
        choices.pendingCapacityGiB = 6
        try fixture.storage.save(choices)
        let restored = try fixture.storage.restoreSnapshot(made.id, guest: choices)
        #expect(restored.capacityGiB == 8 && restored.pendingCapacityGiB == nil)
        #expect(try fixture.storage.load()?.capacityGiB == 8)

        // A growth still above the disk stays, and a larger capacity is never lowered.
        choices.capacityGiB = 8
        choices.pendingCapacityGiB = 16
        try fixture.storage.save(choices)
        let grown = try fixture.storage.restoreSnapshot(made.id, guest: choices)
        #expect(grown.capacityGiB == 8 && grown.pendingCapacityGiB == 16)
        choices.capacityGiB = 32
        choices.pendingCapacityGiB = nil
        try fixture.storage.save(choices)
        #expect(try fixture.storage.restoreSnapshot(made.id, guest: choices).capacityGiB == 32)
    }

    @Test(arguments: [true, false])
    func restoreAppliesTheGuestsBackupPolicyToTheRestoredFiles(includeInBackups: Bool) async throws {
        let fixture = try await SnapshotFixture.make(includeInBackups: includeInBackups)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        // The snapshot's own files are excluded, and its clone carries that.
        let restored = try fixture.storage.restoreSnapshot(made.id, guest: fixture.guest)
        #expect(restored.includeInBackups == includeInBackups)
        #expect(fixture.settles(fixture.data, to: !includeInBackups))
        #expect(fixture.settles(fixture.files.state, to: !includeInBackups))
        #expect(fixture.settles(fixture.snapshots, to: true))
        #expect(fixture.settles(fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent("data.img"), to: true))
    }

    @Test func restoreRefusesWhatCannotBeRestoredHereAndChangesNothing() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
        let record = try contents(fixture.files.state)

        // A disk size the release does not accept.
        let small = fixture.app(on: snapshotRelease(minimumGiB: 1, maximumGiB: 3))
        #expect(throws: SnapshotRefusal.diskSizeNotAccepted(gib: 4)) { try small.restoreSnapshot(made.id, guest: fixture.guest) }
        // Another system disk.
        let other = fixture.app(on: snapshotRelease(sha: String(repeating: "c", count: 64)))
        #expect(throws: SnapshotRefusal.differentSystem) { try other.restoreSnapshot(made.id, guest: fixture.guest) }
        // No such snapshot, and a damaged one.
        let missing = SnapshotID()
        #expect(throws: SnapshotRefusal.missing) { try fixture.storage.restoreSnapshot(missing, guest: fixture.guest) }
        let metadata = fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent("snapshot.json")
        try FileManager.default.removeItem(at: metadata)
        #expect(throws: SnapshotRefusal.incomplete) { try fixture.storage.restoreSnapshot(made.id, guest: fixture.guest) }

        #expect(try TestDisk.read(fixture.data, at: 0) == [2])
        #expect(try contents(fixture.files.state) == record)
        #expect(!exists(fixture.snapshots.appendingPathComponent("restore.json")))
    }

    @Test func aSnapshotOfAnOlderAndroidRestoresAndTheNextStartUpdates() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "r4")
        let newer = fixture.app(on: snapshotNewer)
        // The guest has moved on to the newer Android.
        try await newer.prepareStart(fixture.guest)
        try newer.completeRun()
        #expect(newer.startPlan() == .normal)

        #expect(newer.snapshotListing().items.first?.restore == .updatesAtNextStart(from: "20261031-los24-r4", to: "20261102-los24-r5"))
        _ = try newer.restoreSnapshot(made.id, guest: fixture.guest)
        #expect(newer.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"))
        try await newer.prepareStart(fixture.guest)
        #expect(newer.startPlan() == .unfinishedUpdate(to: "20261102-los24-r5", canRetry: true, canRestore: true))
        try newer.completeRun()
        // An older app on the newer data cannot start; restoring the snapshot is a way out.
        let older = fixture.storage
        #expect(older.startPlan() == .blocked(.olderAndroid))
        _ = try older.restoreSnapshot(made.id, guest: fixture.guest)
        #expect(older.startPlan() == .normal)
    }

    // MARK: Delete, rename, list

    @Test func deleteRemovesTheSnapshotAndRefusesAMissingOne() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let first = try fixture.storage.createSnapshot(named: "one")
        let second = try fixture.storage.createSnapshot(named: "two")
        try fixture.storage.deleteSnapshot(first.id)
        #expect(!exists(fixture.snapshots.appendingPathComponent(first.id.rawValue)))
        #expect(fixture.storage.snapshotListing().items.map(\.id) == [second.id])
        #expect(throws: SnapshotRefusal.missing) { try fixture.storage.deleteSnapshot(first.id) }
        // The live disk stands apart from what was deleted.
        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        #expect(fixture.names().filter { $0.contains(".gone-") }.isEmpty)
    }

    @Test func renameChangesOnlyTheNameAndRefusesWhatItCannot() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "old")
        try fixture.storage.renameSnapshot(made.id, to: "  New name ")
        let renamed = try #require(fixture.storage.snapshotListing().items.first)
        #expect(renamed.name == "New name" && renamed.created == made.created && renamed.releaseID == made.releaseID)
        try fixture.storage.renameSnapshot(made.id, to: "")
        #expect(fixture.storage.snapshotListing().items.first?.name == "")
        #expect(throws: SnapshotRefusal.invalidName) { try fixture.storage.renameSnapshot(made.id, to: "x\u{7}") }
        #expect(throws: SnapshotRefusal.missing) { try fixture.storage.renameSnapshot(SnapshotID(), to: "x") }
        // Rename needs no stopped guest and no shut down: a guest that was not shut down still renames.
        try fixture.startWithoutShutDown(at: fixture.origin.addingTimeInterval(60))
        try fixture.storage.renameSnapshot(made.id, to: "again")
        #expect(fixture.storage.snapshotListing().items.first?.name == "again")
    }

    @Test func theListIsNewestFirstAndADamagedSnapshotIsListedLastAndCanBeDeleted() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let base = Date(timeIntervalSince1970: 1_793_000_000)
        let middle = try fixture.storage.createSnapshot(named: "middle", now: base.addingTimeInterval(10))
        let newest = try fixture.storage.createSnapshot(named: "newest", now: base.addingTimeInterval(20))
        let oldest = try fixture.storage.createSnapshot(named: "oldest", now: base)
        // One whose record is gone is listed last; one that lost its copy of
        // the guest's record still has a time to be placed by.
        let broken = try fixture.storage.createSnapshot(named: "broken", now: base.addingTimeInterval(30))
        let headless = try fixture.storage.createSnapshot(named: "headless", now: base.addingTimeInterval(40))
        let folder = { (id: SnapshotID) in fixture.snapshots.appendingPathComponent(id.rawValue) }
        try FileManager.default.removeItem(at: folder(broken.id).appendingPathComponent("snapshot.json"))
        try FileManager.default.removeItem(at: folder(headless.id).appendingPathComponent("instance.json"))

        let listing = fixture.storage.snapshotListing()
        #expect(listing.items.map(\.id) == [headless.id, newest.id, middle.id, oldest.id, broken.id])
        for damaged in [try #require(listing.items.first), try #require(listing.items.last)] {
            #expect(damaged.isDamaged && damaged.restore == .refused(.incomplete))
        }
        #expect(!newest.isDamaged)
        for id in [broken.id, headless.id] {
            #expect(throws: SnapshotRefusal.incomplete) { try fixture.storage.restoreSnapshot(id, guest: fixture.guest) }
            try fixture.storage.deleteSnapshot(id)
        }
        #expect(fixture.storage.snapshotListing().items.count == 3)
    }

    // MARK: Space

    @Test func theListSaysWhatEachSnapshotAndAllOfThemHold() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let first = try fixture.storage.createSnapshot(named: "first")
        let second = try fixture.storage.createSnapshot(named: "second")
        // The guest rewrites 2 MiB that both snapshots hold.
        try rewriteTwoMiB(fixture)

        let listing = fixture.storage.snapshotListing()
        func near(_ figure: SnapshotFigure, _ expected: Int64, sourceLocation: SourceLocation = #_sourceLocation) {
            #expect(abs(Int64(figure.bytes) - expected) <= 256 << 10, "\(figure.bytes) against \(expected)", sourceLocation: sourceLocation)
        }
        // Shared by the two and no longer by the live disk: private to neither,
        // yet it is what deleting both frees.
        for item in listing.items { near(item.freesWhenDeleted, 0) }
        near(listing.space.snapshotsUse, 2 << 20)
        #expect(!listing.space.snapshotsUse.isLowerBound)
        #expect(listing.space.updateCopy == nil)
        // Deleting one raises what the other frees to what both held.
        try fixture.storage.deleteSnapshot(first.id)
        let after = fixture.storage.snapshotListing()
        near(try #require(after.items.first { $0.id == second.id }).freesWhenDeleted, 2 << 20)
        near(after.space.snapshotsUse, 2 << 20)
    }

    @Test func withNoLiveDiskTheSnapshotsHoldWhatTheyHoldTogether() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let first = try fixture.storage.createSnapshot(named: "first")
        _ = try fixture.storage.createSnapshot(named: "second")
        try FileManager.default.removeItem(at: fixture.data)

        let listing = fixture.storage.snapshotListing()
        // Both clones share every block and the live disk is gone: deleting
        // both frees them once, deleting one frees none of them.
        #expect(!listing.space.snapshotsUse.isLowerBound)
        #expect(listing.space.snapshotsUse.bytes >= 3 << 20 && listing.space.snapshotsUse.bytes < 4 << 20)
        #expect(listing.items.allSatisfy { $0.freesWhenDeleted.bytes < 1 << 20 })
        #expect(listing.items.allSatisfy { $0.kept.bytes >= 3 << 20 })
        #expect(listing.createRefusal == .noData)
        // Either can still be restored: the data comes back.
        _ = try fixture.storage.restoreSnapshot(first.id, guest: fixture.guest)
        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
    }

    @Test func aDamagedSnapshotThatKeepsItsDiskStillSaysWhatDeletingItFrees() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        try rewriteTwoMiB(fixture)
        try FileManager.default.removeItem(at: fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent("snapshot.json"))
        let damaged = try #require(fixture.storage.snapshotListing().items.first)
        #expect(damaged.isDamaged)
        #expect(abs(Int64(damaged.freesWhenDeleted.bytes) - (2 << 20)) <= 256 << 10)
        #expect(damaged.kept.bytes >= 3 << 20)
    }

    @Test func whereTheExtentMapIsRefusedTheUseIsALowerBoundAndSaysSo() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        _ = try fixture.storage.createSnapshot(named: "first")
        _ = try fixture.storage.createSnapshot(named: "second")
        try rewriteTwoMiB(fixture)
        var io = SnapshotFileOperations.live
        io.measure = SnapshotMeasure.Primitives(privateSize: { SnapshotMeasure.privateSize(of: $0) }, extents: { _ in nil })
        let listing = fixture.app(using: io).snapshotListing()
        #expect(listing.space.snapshotsUse.isLowerBound)
        #expect(listing.space.snapshotsUse.bytes < 2 << 20)
        // One snapshot's own figure is exact without the map.
        #expect(listing.items.allSatisfy { !$0.freesWhenDeleted.isLowerBound })
    }

    @Test func whereThePrivateSizeIsRefusedEachFigureStillCountsWhatTheOthersHold() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let first = try fixture.storage.createSnapshot(named: "first")
        let second = try fixture.storage.createSnapshot(named: "second")
        try rewriteTwoMiB(fixture)
        func near(_ figure: SnapshotFigure, _ expected: Int64, sourceLocation: SourceLocation = #_sourceLocation) {
            #expect(abs(Int64(figure.bytes) - expected) <= 256 << 10, "\(figure.bytes) against \(expected)", sourceLocation: sourceLocation)
        }
        var io = SnapshotFileOperations.live
        io.measure = SnapshotMeasure.Primitives(privateSize: { _ in nil }, extents: { SnapshotMeasure.physicalExtents(of: $0) })
        let app = fixture.app(using: io)
        // What both snapshots share is private to neither: the extent map says so, exactly.
        for item in app.snapshotListing().items {
            near(item.freesWhenDeleted, 0)
            #expect(!item.freesWhenDeleted.isLowerBound)
        }
        try app.deleteSnapshot(first.id)
        near(try #require(app.snapshotListing().items.first { $0.id == second.id }).freesWhenDeleted, 2 << 20)

        // With neither call, a figure is a lower bound and never the whole disk.
        io.measure = SnapshotMeasure.Primitives(privateSize: { _ in nil }, extents: { _ in nil })
        let blind = fixture.app(using: io).snapshotListing()
        #expect(blind.items.allSatisfy { $0.freesWhenDeleted.isLowerBound && $0.freesWhenDeleted.bytes < 1 << 20 })
    }

    @Test func theUpdatesOwnCopyIsAFigureApartFromTheSnapshots() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "before")
        #expect(fixture.storage.snapshotListing().space.updateCopy == nil)
        let newer = fixture.app(on: snapshotNewer)
        try await newer.prepareStart(fixture.guest)
        // The Android update rewrites 2 MiB of what the snapshot and the copy hold.
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 7)
        try TestDisk.write(fixture.data, at: 1 << 30, count: 1 << 20, value: 7)

        let shared = newer.snapshotListing()
        #expect(shared.items.first?.id == made.id)
        #expect(shared.createRefusal == .unfinishedUpdate)
        // The snapshot and the copy hold the same blocks: deleting either frees none of them.
        #expect(try #require(shared.space.updateCopy).bytes < 512 << 10)
        #expect(shared.space.snapshotsUse.bytes < 512 << 10)

        // Without the snapshot the copy alone holds them.
        try newer.deleteSnapshot(made.id)
        let alone = newer.snapshotListing()
        let copy = try #require(alone.space.updateCopy)
        #expect(abs(Int64(copy.bytes) - (2 << 20)) <= 512 << 10)
        #expect(alone.space.snapshotsUse == SnapshotFigure(bytes: 0))
    }

    // MARK: Erase and launch

    @Test func eraseDeletesTheSnapshotsWithTheRest() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        _ = try fixture.storage.createSnapshot(named: "one")
        _ = try fixture.storage.createSnapshot(named: "two")
        // What a crash can leave is erased too.
        try FileManager.default.createDirectory(at: fixture.snapshots.appendingPathComponent(".new-leftover"), withIntermediateDirectories: true)
        try Data("{}".utf8).write(to: fixture.snapshots.appendingPathComponent("restore.json"))

        try await fixture.storage.erase()
        #expect(!exists(fixture.snapshots))
        #expect(!exists(fixture.data) && !exists(fixture.files.state) && !exists(fixture.storage.locations.settings))
        #expect(try fixture.storage.load() == nil)
        // A guest set up afterwards has no snapshot.
        _ = try await fixture.storage.create(Guest.fresh(on: snapshotHost))
        #expect(fixture.storage.snapshotListing().items.isEmpty)
    }

    @Test func launchRemovesWhatACreateOrADeleteLeftAndKeepsTheRest() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let kept = try fixture.storage.createSnapshot(named: "kept")
        let manager = FileManager.default
        let staging = fixture.snapshots.appendingPathComponent(".new-\(SnapshotID().rawValue)")
        let gone = fixture.snapshots.appendingPathComponent(".gone-\(SnapshotID().rawValue)")
        for folder in [staging, gone] {
            try manager.createDirectory(at: folder, withIntermediateDirectories: true)
            try Data(count: 4096).write(to: folder.appendingPathComponent("data.img"))
        }
        let tmp = fixture.data.appendingPathExtension("tmp")
        try Data(count: 4096).write(to: tmp)

        try fixture.storage.recoverSnapshots()
        #expect(!exists(staging) && !exists(gone) && !exists(tmp))
        #expect(fixture.storage.snapshotListing().items.map(\.id) == [kept.id])
        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
    }

    @Test func launchOnAGuestWithNoSnapshotsChangesNothing() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let before = fixture.names()
        try fixture.storage.recoverSnapshots()
        #expect(fixture.names() == before)
        #expect(!exists(fixture.snapshots))
    }

    @Test func aRestoreWhoseSnapshotIsGoneOrWhoseFileIsUnreadableIsForgotten() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        let intent = fixture.snapshots.appendingPathComponent("restore.json")
        // Unreadable.
        try Data("garbage".utf8).write(to: intent)
        try fixture.storage.recoverSnapshots()
        #expect(!exists(intent))
        // The snapshot it names is not there: nothing can be carried on.
        try Data(#"{"schema": 1, "snapshot": "\#(SnapshotID().rawValue)"}"#.utf8).write(to: intent)
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
        try fixture.storage.recoverSnapshots()
        #expect(!exists(intent))
        #expect(try TestDisk.read(fixture.data, at: 0) == [2])
        #expect(fixture.storage.snapshotListing().items.map(\.id) == [made.id])
    }

    @Test func aStartCompletesARestoreThatThisProcessLeftUnfinished() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "")
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
        try writeIntent(for: made.id, in: fixture)
        #expect(try TestDisk.read(fixture.data, at: 0) == [2])

        // No relaunch: the start finishes the restore before it reads the files.
        try await fixture.storage.prepareStart(fixture.guest)
        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        #expect(!exists(fixture.snapshots.appendingPathComponent("restore.json")))
    }

    @Test func anUnfinishedRestorePlansTheStartFromTheSnapshotsRecord() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "then")
        // The guest moved on to a newer Android, which this older app cannot start.
        let newer = fixture.app(on: snapshotNewer)
        try await newer.prepareStart(fixture.guest)
        try newer.completeRun()
        #expect(fixture.storage.startPlan() == .blocked(.olderAndroid))
        #expect(newer.startPlan() == .normal)

        // A restore of the older snapshot put its data in place and did not finish. A start completes it
        // first, so the start is the snapshot's, and Start is offered on that.
        try writeIntent(for: made.id, in: fixture)
        #expect(fixture.storage.startPlan() == .normal)
        #expect(fixture.storage.startPlan()?.allowsStart == true)
        #expect(newer.startPlan() == .update(from: "20261031-los24-r4", to: "20261102-los24-r5"))

        // What the plan said is what the start then is.
        try await fixture.storage.prepareStart(fixture.guest)
        #expect(!exists(fixture.snapshots.appendingPathComponent("restore.json")))
        #expect(fixture.storage.startPlan() == .normal)
    }

    @Test func aRestoreThatCannotBeCarriedThroughLeavesThePlanAsTheFilesSayIt() async throws {
        let fixture = try await SnapshotFixture.make()
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "then")
        let newer = fixture.app(on: snapshotNewer)
        try await newer.prepareStart(fixture.guest)
        try newer.completeRun()
        let intent = fixture.snapshots.appendingPathComponent("restore.json")

        // An intent that cannot be read: launch and Start drop it, so nothing changes.
        try Data("garbage".utf8).write(to: intent)
        #expect(fixture.storage.startPlan() == .blocked(.olderAndroid))
        // A snapshot that is no longer whole.
        try writeIntent(for: made.id, in: fixture)
        try FileManager.default.removeItem(at: fixture.snapshots.appendingPathComponent(made.id.rawValue).appendingPathComponent("data.img"))
        #expect(fixture.storage.startPlan() == .blocked(.olderAndroid))
        // One that is not there at all.
        try FileManager.default.removeItem(at: fixture.snapshots.appendingPathComponent(made.id.rawValue))
        #expect(fixture.storage.startPlan() == .blocked(.olderAndroid))
        // And no restore: the same.
        try FileManager.default.removeItem(at: intent)
        #expect(fixture.storage.startPlan() == .blocked(.olderAndroid))
    }

    /// A volume that cannot clone, and one that is full, refuse as values.
    @Test func theCallsOwnErrorsBecomeRefusalsAsValues() {
        let store = SnapshotStore(
            locations: GuestLocations(directory: FileManager.default.temporaryDirectory), release: snapshotRelease())
        #expect(store.refusal(for: CloneFile.Failure(code: ENOTSUP)) == .volumeCannotClone)
        #expect(store.refusal(for: CloneFile.Failure(code: ENOSPC)) == .notEnoughSpace)
        #expect(store.refusal(for: CocoaError(.fileWriteOutOfSpace)) == .notEnoughSpace)
        #expect(store.refusal(for: POSIXError(.ENOSPC)) == .notEnoughSpace)
        #expect(store.refusal(for: CloneFile.Failure(code: EACCES)) == nil)
        #expect(store.refusal(for: POSIXError(.EIO)) == nil)
    }
}
