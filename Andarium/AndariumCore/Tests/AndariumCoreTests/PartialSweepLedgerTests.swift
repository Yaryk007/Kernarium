// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// What the file says, read without the ledger: the record is on disk when the
/// call that changed it returns, not queued behind it.
private func onDisk(_ directory: URL) -> (owed: Bool, imports: Bool)? {
    let url = PartialSweepLedger.url(in: directory)
    guard let data = try? Data(contentsOf: url),
          let object = try? JSONSerialization.jsonObject(with: data) as? [String: Bool],
          let owed = object["owed"], let imports = object["imports"] else { return nil }
    return (owed, imports)
}

private func temporaryDirectory() throws -> URL {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-core-ledger-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    return directory
}

private let first = GuestRunMark("0123abcd")!
private let second = GuestRunMark("89ef4567")!

private struct WalkFailed: Error {}

struct PartialSweepLedgerTests {
    @Test func aGuestWithNoRecordOwesTheWalkAndAnUnreadableRecordToo() throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        #expect(PartialSweepLedger.isOwed(in: directory))
        #expect(PartialSweepLedger(directory: directory).isOwed)
        // Looking at it writes nothing: the default is not a record.
        #expect(onDisk(directory) == nil)
        try Data("not json".utf8).write(to: PartialSweepLedger.url(in: directory))
        #expect(PartialSweepLedger.isOwed(in: directory))
    }

    @Test func aFinishedSweepSettlesTheRecordAndAFailedOneLeavesItOwed() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let ledger = PartialSweepLedger(directory: directory)

        await #expect(throws: WalkFailed.self) {
            _ = try await ledger.sweeping(excluding: first) { throw WalkFailed() }
        }
        #expect(ledger.isOwed)
        // A new process (the next run) finds it still owed.
        #expect(PartialSweepLedger(directory: directory).isOwed)

        let removed = try await ledger.sweeping(excluding: first) { 3 }
        #expect(removed == 3)
        #expect(!ledger.isOwed)
        let record = try #require(onDisk(directory))
        #expect(record.owed == false && record.imports == false)
        // A later process asks again and the answer is the same: no walk.
        #expect(!PartialSweepLedger(directory: directory).isOwed)
        #expect(!PartialSweepLedger.isOwed(in: directory))
    }

    @Test func aCleanImportLeavesTheRecordAsItWasAndItIsOwedOnlyWhileItRuns() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let ledger = PartialSweepLedger(directory: directory)
        _ = try await ledger.sweeping(excluding: first) { 0 }
        #expect(!ledger.isOwed)

        try ledger.importBegan(mark: first)
        // Written before the first byte could be: a process that ends now
        // leaves it on disk.
        let during = try #require(onDisk(directory))
        #expect(during.imports && !during.owed)
        #expect(ledger.isOwed)
        ledger.importEnded(mark: first, clean: true)
        #expect(!ledger.isOwed)
        let after = try #require(onDisk(directory))
        #expect(after.imports == false && after.owed == false)
    }

    @Test func aCleanImportDoesNotSettleWhatWasOwedBefore() throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        try PartialSweepLedger.markOwed(in: directory)
        let ledger = PartialSweepLedger(directory: directory)

        try ledger.importBegan(mark: first)
        ledger.importEnded(mark: first, clean: true)
        #expect(ledger.isOwed)
        let record = try #require(onDisk(directory))
        #expect(record.owed && !record.imports)
    }

    @Test func anImportThatCouldNotRemoveItsPartialFileLeavesTheRecordOwed() throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        let ledger = PartialSweepLedger(directory: directory)
        try ledger.importBegan(mark: first)
        ledger.importEnded(mark: first, clean: false)
        #expect(ledger.isOwed)
        #expect(PartialSweepLedger(directory: directory).isOwed)
    }

    @Test func severalImportsOweUntilTheLastOneEnds() throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        let ledger = PartialSweepLedger(directory: directory)
        try ledger.importBegan(mark: first)
        try ledger.importBegan(mark: first)
        ledger.importEnded(mark: first, clean: true)
        #expect(ledger.isOwed)
        ledger.importEnded(mark: first, clean: true)
        #expect(!ledger.isOwed)
        // An end nobody began changes nothing.
        ledger.importEnded(mark: second, clean: false)
        #expect(!ledger.isOwed)
    }

    @Test func aProcessEndedInsideAnImportLeavesTheNextOneOwingTheWalk() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        let ended = PartialSweepLedger(directory: directory)
        try ended.importBegan(mark: first)
        // The process is gone: no end, no sweep, nothing else was written.

        let next = PartialSweepLedger(directory: directory)
        #expect(next.isOwed)
        // The record of the new process carries the debt as its own: an
        // import of this run that ends cleanly does not forgive it.
        let record = try #require(onDisk(directory))
        #expect(record.owed && !record.imports)
        try next.importBegan(mark: second)
        next.importEnded(mark: second, clean: true)
        #expect(next.isOwed)
        _ = try await next.sweeping(excluding: second) { 1 }
        #expect(!next.isOwed)
    }

    @Test func aSweepDoesNotCoverTheImportsItSpares() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        let ledger = PartialSweepLedger(directory: directory)
        try ledger.importBegan(mark: first)
        ledger.importEnded(mark: first, clean: false)

        // A sweep of the same run leaves that import's file in place.
        _ = try await ledger.sweeping(excluding: first) { 0 }
        #expect(ledger.isOwed)
        // One that spares another mark (the run after a guest restart, or
        // the next run) removes it.
        _ = try await ledger.sweeping(excluding: second) { 1 }
        #expect(!ledger.isOwed)
    }

    @Test func aSweepDoesNotCoverAnImportThatEndedAfterItBegan() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        let ledger = PartialSweepLedger(directory: directory)
        try ledger.importBegan(mark: first)

        _ = try await ledger.sweeping(excluding: second) {
            // The walk is on, and the import is cut off meanwhile.
            ledger.importEnded(mark: first, clean: false)
            return 0
        }
        #expect(ledger.isOwed)
        _ = try await ledger.sweeping(excluding: second) { 1 }
        #expect(!ledger.isOwed)
    }

    @Test func aSweepDoesNotSettleWhileAnImportIsRunning() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let ledger = PartialSweepLedger(directory: directory)
        try ledger.importBegan(mark: second)

        _ = try await ledger.sweeping(excluding: second) { 0 }
        // What was owed before is gone, the running import's file is not.
        let record = try #require(onDisk(directory))
        #expect(record.owed == false && record.imports)
        #expect(ledger.isOwed)
        ledger.importEnded(mark: second, clean: true)
        #expect(!ledger.isOwed)
    }

    @Test func anEraseForgetsTheImportsOfTheDiskItRemoved() throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try PartialSweepLedger.markSettled(in: directory)
        let ledger = PartialSweepLedger(directory: directory)
        try ledger.importBegan(mark: first)
        ledger.importEnded(mark: first, clean: false)
        try ledger.importBegan(mark: second)

        // The disk goes and a new one comes: the record is the new disk's.
        ledger.diskErased()
        try PartialSweepLedger.markSettled(in: directory)
        #expect(!ledger.isOwed)
        // The end of an import of the old disk is no import of the new one.
        ledger.importEnded(mark: second, clean: false)
        #expect(!ledger.isOwed)
        try ledger.importBegan(mark: second)
        ledger.importEnded(mark: second, clean: true)
        #expect(!ledger.isOwed)
    }

    @Test func anImportThatCannotBeRecordedDoesNotBegin() throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        // A directory that cannot be made: a file stands in its place.
        let blocked = directory.appendingPathComponent("blocked")
        try Data().write(to: blocked)
        let ledger = PartialSweepLedger(directory: blocked.appendingPathComponent("Guest"))
        #expect(throws: (any Error).self) { try ledger.importBegan(mark: first) }
        // Nothing was counted, so its end is not one either.
        ledger.importEnded(mark: first, clean: false)
    }

    @Test func aNewGuestIsSettledAndEraseGoesBackToTheDefault() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory.appendingPathComponent("Guest")), release: snapshotRelease(),
            releaseDirectory: directory.appendingPathComponent("andarium-system"))
        let guestDirectory = storage.locations.directory
        #expect(PartialSweepLedger.isOwed(in: guestDirectory))

        _ = try await storage.create(Guest.fresh(on: snapshotHost))
        #expect(!PartialSweepLedger.isOwed(in: guestDirectory))
        try PartialSweepLedger.markOwed(in: guestDirectory)

        try await storage.erase()
        #expect(!FileManager.default.fileExists(atPath: PartialSweepLedger.url(in: guestDirectory).path))
        // The next guest is a new disk: nothing is owed.
        _ = try await storage.create(Guest.fresh(on: snapshotHost))
        #expect(!PartialSweepLedger.isOwed(in: guestDirectory))
    }
}

/// What puts an older data disk in place owes the walk, and it is on disk
/// before the disk is in place.
@Suite(.enabled(if: temporaryVolumeClones, "needs a volume that clones files"))
struct PartialSweepRestoreTests {
    @Test func aSnapshotRestoreOwesTheWalk() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "Before")
        try PartialSweepLedger.markSettled(in: fixture.directory)
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)

        _ = try fixture.storage.restoreSnapshot(made.id, guest: fixture.guest)

        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        #expect(PartialSweepLedger.isOwed(in: fixture.directory))
        let record = try #require(onDisk(fixture.directory))
        #expect(record.owed)
    }

    @Test func theRecordIsOnDiskBeforeTheRestoredDataIsInPlace() async throws {
        final class Seen: @unchecked Sendable { var owed: Bool? }
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "Before")
        try PartialSweepLedger.markSettled(in: fixture.directory)
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)

        // The step that cannot be taken back is the rename over `data.img`.
        let seen = Seen()
        let directory = fixture.directory
        var io = SnapshotFileOperations.live
        let rename = io.rename
        io.rename = { source, destination in
            if destination.lastPathComponent == InstanceFiles.dataDisk { seen.owed = onDisk(directory)?.owed }
            try rename(source, destination)
        }
        _ = try fixture.app(using: io).restoreSnapshot(made.id, guest: fixture.guest)
        #expect(seen.owed == true)
    }

    @Test func aRestoreRefusedAtTheCommitLeavesTheWalkOwedAndTheDataAsItWas() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "Before")
        try PartialSweepLedger.markSettled(in: fixture.directory)
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)

        var io = SnapshotFileOperations.live
        let rename = io.rename
        io.rename = { source, destination in
            if destination.lastPathComponent == InstanceFiles.dataDisk { throw POSIXError(.EIO) }
            try rename(source, destination)
        }
        #expect(throws: (any Error).self) { try fixture.app(using: io).restoreSnapshot(made.id, guest: fixture.guest) }

        // The data is the guest's own, and a walk too many is the safe answer.
        #expect(try TestDisk.read(fixture.data, at: 0) == [2])
        #expect(PartialSweepLedger.isOwed(in: fixture.directory))
    }

    @Test func aRestoreThatIsRolledForwardAtLaunchOwesTheWalk() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let made = try fixture.storage.createSnapshot(named: "Before")
        try PartialSweepLedger.markSettled(in: fixture.directory)
        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
        // A crash right after the restore's intent was written.
        try Data(#"{"schema": 1, "snapshot": "\#(made.id.rawValue)"}"#.utf8)
            .write(to: fixture.snapshots.appendingPathComponent("restore.json"))

        try fixture.storage.recoverSnapshots()

        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        #expect(PartialSweepLedger.isOwed(in: fixture.directory))
    }

    @Test func theUpdatesRestoreOwesTheWalkAndAnUpdateThatKeepsTheDataDoesNot() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let newer = fixture.app(on: snapshotNewer)
        try PartialSweepLedger.markSettled(in: fixture.directory)

        // The update copies the data and starts on it: nothing is put back.
        try await newer.prepareStart(fixture.guest)
        #expect(!PartialSweepLedger.isOwed(in: fixture.directory))

        try TestDisk.write(fixture.data, at: 0, count: 1 << 20, value: 2)
        try newer.restoreUpdate()
        #expect(try TestDisk.read(fixture.data, at: 0) == [1])
        #expect(PartialSweepLedger.isOwed(in: fixture.directory))
    }

    @Test func anUpdateThatRestoresTheDataBeforeItStartsAgainOwesTheWalk() async throws {
        let fixture = try await SnapshotFixture.make(marker: 1)
        defer { fixture.cleanUp() }
        let newer = fixture.app(on: snapshotNewer)
        try await newer.prepareStart(fixture.guest)
        try PartialSweepLedger.markSettled(in: fixture.directory)
        // An even newer system, with the first update never finished: the
        // data goes back to what that update started from, then updates.
        let newest = fixture.app(on: snapshotRelease("20261104-los24-r6", build: 300, sha: String(repeating: "c", count: 64)))
        try await newest.prepareStart(fixture.guest)
        #expect(PartialSweepLedger.isOwed(in: fixture.directory))
    }
}
