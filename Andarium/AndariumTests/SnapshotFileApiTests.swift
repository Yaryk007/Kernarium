// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Darwin
import Foundation
import Testing
@testable import Andarium

/// The file system calls the guest's snapshots rest on, on a small sparse file
/// in the test host's own temporary directory: `clonefile`, the private size
/// (`getattrlist` with `ATTR_CMNEXT_PRIVATESIZE`), the extent map
/// (`fcntl(F_LOG2PHYS_EXT)`), the copy-on-write unit and the backup attribute
/// of a clone, and one round trip through the snapshot store itself. It
/// touches no guest's files, boots nothing and runs the same in the Simulator,
/// as an iPad app on a Mac and on a device.
///
/// Where the platform may refuse a call, the store has a fallback (the sum of
/// private sizes, marked as a lower bound), so on a device a refused call is
/// reported in the `SNAPSHOT_FILE_APIS` line the test prints and does not fail
/// the run; in the Simulator and on a Mac, which share the Mac's APFS, every
/// call must answer. Only `clonefile` must work everywhere.
struct SnapshotFileApiTests {
    /// What the platform is, for the report and for what the run requires.
    private static var environment: String {
        #if targetEnvironment(simulator)
        "simulator"
        #else
        ProcessInfo.processInfo.isiOSAppOnMac ? "ipad-app-on-mac" : "device"
        #endif
    }

    /// The Mac's APFS answers every call; a device's answers are reported.
    private static var requiresEveryCall: Bool { environment != "device" }

    private struct Fixture {
        let directory: URL
        let live: URL

        /// A 256 MiB sparse file with 8 MiB written at its start and 4 MiB at 128 MiB.
        init() throws {
            directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("andarium-snapshot-api-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            live = directory.appendingPathComponent("live.img")
            let fd = open(live.path, O_RDWR | O_CREAT, 0o600)
            guard fd >= 0, ftruncate(fd, 256 << 20) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            close(fd)
            try SnapshotTestDisk.write(live, at: 0, count: 8 << 20, value: 1)
            try SnapshotTestDisk.write(live, at: 128 << 20, count: 4 << 20, value: 2)
        }

        func remove() { try? FileManager.default.removeItem(at: directory) }
    }

    private func excluded(_ url: URL) -> Bool? {
        try? URL(fileURLWithPath: url.path).resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup
    }

    @Test func theFileApisAnswerOnASparseFileAndAClone() throws {
        let fixture = try Fixture()
        defer { fixture.remove() }
        let clone = fixture.directory.appendingPathComponent("clone.img")

        // Clone: instant, and later writes to the source never reach it.
        let started = ContinuousClock.now
        #expect(clonefile(fixture.live.path, clone.path, 0) == 0, "clonefile: \(String(cString: strerror(errno)))")
        let cloneTime = ContinuousClock.now - started
        #expect(cloneTime < .seconds(5))
        #expect(try SnapshotTestDisk.read(clone, at: 0) == 1 && SnapshotTestDisk.read(clone, at: 128 << 20) == 2)
        try SnapshotTestDisk.write(fixture.live, at: 0, count: 16384, value: 9)
        #expect(try SnapshotTestDisk.read(clone, at: 0) == 1 && SnapshotTestDisk.read(fixture.live, at: 0) == 9)

        // Private size: the blocks only this file holds. One 4 KiB rewrite of
        // shared data leaves the clone a whole copy-on-write unit.
        try SnapshotTestDisk.write(fixture.live, at: 1 << 20, count: 4096, value: 7)
        let cloneSize = SnapshotMeasure.privateSize(of: clone)
        let liveSize = SnapshotMeasure.privateSize(of: fixture.live)
        // Extent map: where the data lies. The clone and the live file share
        // all of it but what was rewritten.
        let cloneMap = SnapshotMeasure.physicalExtents(of: clone)
        let liveMap = SnapshotMeasure.physicalExtents(of: fixture.live)
        var unheld: UInt64?
        if let cloneMap, let liveMap {
            unheld = SnapshotMeasure.bytes(of: SnapshotMeasure.merged(cloneMap), notIn: SnapshotMeasure.merged(liveMap))
        }
        let allocated = SnapshotMeasure.allocatedBytes(of: clone)

        // The backup attribute: a clone of an excluded file is excluded, and
        // the store sets it itself on every snapshot file.
        var source = fixture.live
        var flag = URLResourceValues()
        flag.isExcludedFromBackup = true
        try source.setResourceValues(flag)
        let flagged = fixture.directory.appendingPathComponent("flagged.img")
        #expect(clonefile(fixture.live.path, flagged.path, 0) == 0)
        let carried = excluded(flagged)

        print(
            "SNAPSHOT_FILE_APIS: environment=\(Self.environment) clone=ok cloneMs=\(Int(cloneTime.components.seconds * 1000 + cloneTime.components.attoseconds / 1_000_000_000_000_000)) "
                + "privateSizeClone=\(cloneSize.map(String.init) ?? "refused") privateSizeLive=\(liveSize.map(String.init) ?? "refused") "
                + "extents=\(cloneMap.map { "\($0.count)" } ?? "refused")/\(liveMap.map { "\($0.count)" } ?? "refused") "
                + "extentsOnlyInClone=\(unheld.map(String.init) ?? "n/a") allocated=\(allocated) "
                + "backupFlagCarriedByClone=\(carried.map(String.init) ?? "unreadable") pageSize=\(getpagesize())")

        if Self.requiresEveryCall {
            let size = try #require(cloneSize, "the private size is refused")
            // One unit (16 KiB on the Apple host's page), with room for tables.
            #expect(size >= 4096 && size <= 64 << 10, "private size of the clone is \(size)")
            #expect(try #require(liveSize) >= 4096)
            #expect(try #require(cloneMap).isEmpty == false && liveMap?.isEmpty == false, "the extent map is refused")
            let unit = try #require(unheld)
            #expect(unit >= 4096 && unit <= 64 << 10, "the clone holds \(unit) bytes the live file does not")
            #expect(carried == true, "a clone of an excluded file is not excluded")
        }
    }

    /// The snapshot store itself in the sandbox: a snapshot, its flushes and
    /// exclusions, and a restore, on a guest this test makes in its own
    /// directory.
    @Test func aSnapshotIsMadeAndRestoredInTheSandbox() async throws {
        let fixture = try await SnapshotTestGuest.make()
        defer { fixture.remove() }
        let storage = fixture.storage
        let disk = fixture.disk

        let made = try storage.createSnapshot(named: "Test")
        #expect(made.restore == .asIs && made.diskGiB == 1)
        try SnapshotTestDisk.write(disk, at: 0, count: 1 << 20, value: 2)
        #expect(try SnapshotTestDisk.read(disk, at: 0) == 2)
        let restored = try storage.restoreSnapshot(made.id, guest: fixture.guest)
        #expect(try SnapshotTestDisk.read(disk, at: 0) == 1)
        #expect(restored.capacityGiB >= 1)

        let listing = storage.snapshotListing()
        let item = try #require(listing.items.first)
        print(
            "SNAPSHOT_FILE_APIS: environment=\(Self.environment) store=ok "
                + "freesWhenDeleted=\(item.freesWhenDeleted.bytes)\(item.freesWhenDeleted.isLowerBound ? "+" : "") "
                + "use=\(listing.space.snapshotsUse.bytes)\(listing.space.snapshotsUse.isLowerBound ? "+" : "")")
        try storage.deleteSnapshot(made.id)
        #expect(storage.snapshotListing().items.isEmpty)
    }
}
