// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// The release the snapshot tests run on: a disk of 4 to 64 GiB.
func snapshotRelease(
    _ id: String = "20261031-los24-r4", build: Int = 100, sha: String = String(repeating: "a", count: 64),
    layout: Int = 1, minimumGiB: Int = 4, maximumGiB: Int = 64
) -> ReleaseIdentity {
    ReleaseIdentity(
        releaseID: id, buildFingerprint: "fp-\(id)", buildNumber: build, securityPatch: "2026-10-01",
        systemDiskSHA256: sha, dataLayout: layout, minimumGiB: minimumGiB, defaultGiB: 8, maximumGiB: maximumGiB
    )
}

let snapshotHost = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)

/// Whether the volume of the temporary directory clones files (APFS, every Mac
/// and iOS volume). The suites that need it run only where it does: on a
/// volume that cannot clone, a run passes with the store, crash, fault, erase
/// and listing tests skipped, so none of the file-level behaviour is covered.
/// The gate forbids a custom TMPDIR, so its run is on APFS.
let temporaryVolumeClones: Bool = {
    let manager = FileManager.default
    let directory = manager.temporaryDirectory.appendingPathComponent("andarium-core-clone-probe-\(UUID().uuidString)")
    defer { try? manager.removeItem(at: directory) }
    guard (try? manager.createDirectory(at: directory, withIntermediateDirectories: true)) != nil else { return false }
    let source = directory.appendingPathComponent("a")
    guard manager.createFile(atPath: source.path, contents: Data(count: 16384)) else { return false }
    return clonefile(source.path, directory.appendingPathComponent("b").path, 0) == 0
}()

/// Sparse files with known contents, written through to the volume.
enum TestDisk {
    static let mib: Int64 = 1 << 20
    static let gib: Int64 = 1 << 30

    private static func bytes(_ count: Int, _ value: UInt8) -> [UInt8] {
        [UInt8](repeating: value, count: count)
    }

    static func descriptor(_ url: URL, create: Bool = false) throws -> Int32 {
        let result = open(url.path, create ? O_RDWR | O_CREAT : O_RDWR, 0o600)
        guard result >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        return result
    }

    /// A sparse disk of `gib` GiB whose first and last mebibyte, and the one at
    /// 1 GiB, hold `marker` (the one at 1 GiB when the disk is larger).
    static func make(at url: URL, gib: Int = 4, marker: UInt8) throws {
        let fd = try descriptor(url, create: true)
        defer { close(fd) }
        guard ftruncate(fd, off_t(Int64(gib) * Self.gib)) == 0 else { throw POSIXError(.EIO) }
        try write(fd, at: 0, count: Int(mib), value: marker)
        try write(fd, at: Int64(gib) * Self.gib - mib, count: Int(mib), value: marker)
        // Some data in the middle too, away from the ends.
        if gib > 1 { try write(fd, at: Self.gib, count: Int(mib), value: marker) }
    }

    static func write(_ url: URL, at offset: Int64, count: Int, value: UInt8) throws {
        let fd = try descriptor(url)
        defer { close(fd) }
        try write(fd, at: offset, count: count, value: value)
    }

    private static func write(_ fd: Int32, at offset: Int64, count: Int, value: UInt8) throws {
        let data = bytes(count, value)
        guard pwrite(fd, data, count, off_t(offset)) == count else { throw POSIXError(.EIO) }
        _ = fcntl(fd, F_FULLFSYNC)
    }

    static func read(_ url: URL, at offset: Int64, count: Int = 1) throws -> [UInt8] {
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(fd) }
        var data = [UInt8](repeating: 0, count: count)
        guard pread(fd, &data, count, off_t(offset)) == count else { throw POSIXError(.EIO) }
        return data
    }

    /// The disk's size and a byte at its start, at 1 GiB and at its end.
    static func fingerprint(_ url: URL) -> String {
        guard let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int64 else { return "missing" }
        let marks = [0, gib, size - 1].map { offset in (try? read(url, at: offset).first).map { String($0) } ?? "?" }
        return "\(size):\(marks.joined(separator: ","))"
    }
}

/// A guest in a temporary directory that has started and been shut down by
/// Android: what a person has when they open Andarium on Home after a run.
struct SnapshotFixture {
    let root: URL
    var storage: TwoDiskStorage
    var guest: Guest
    /// The time the guest was set up; the timeline runs from it.
    let origin: Date

    var data: URL { storage.locations.data }
    var directory: URL { storage.locations.directory }
    var snapshots: URL { directory.appendingPathComponent("Snapshots") }
    var files: InstanceFiles { storage.files }

    static func make(
        release: ReleaseIdentity = snapshotRelease(), gib: Int = 4, includeInBackups: Bool = false, marker: UInt8 = 1,
        shutDown: Bool = true
    ) async throws -> SnapshotFixture {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-core-snapshots-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: root.appendingPathComponent("Guest")),
            release: release, releaseDirectory: root.appendingPathComponent("andarium-system")
        )
        var choices = Guest.fresh(on: snapshotHost)
        choices.includeInBackups = includeInBackups
        let made = try await storage.create(choices)
        try TestDisk.make(at: storage.locations.data, gib: gib, marker: marker)
        let origin = Date()
        let fixture = SnapshotFixture(root: root, storage: storage, guest: made, origin: origin)
        // The apply of the backup policy to a disk the runtime created.
        try storage.save(made)
        if shutDown { try fixture.shutDown(at: origin.addingTimeInterval(30)) }
        return fixture
    }

    func cleanUp() { try? FileManager.default.removeItem(at: root) }

    /// Android's own power off at `date`.
    func shutDown(at date: Date) throws {
        try files.complete(release: storage.release, now: date)
    }

    /// A start at `date` that does not end with Android's own shut down.
    func startWithoutShutDown(at date: Date) throws {
        try files.apply(files.decide(for: storage.release), release: storage.release, now: date)
    }

    /// The same files on another release, as another version of the app sees them.
    func app(on release: ReleaseIdentity) -> TwoDiskStorage {
        TwoDiskStorage(locations: storage.locations, release: release, releaseDirectory: storage.releaseDirectory)
    }

    /// The same files with the snapshot store's file calls replaced
    /// (`SnapshotFaults`).
    func app(using io: SnapshotFileOperations, on release: ReleaseIdentity? = nil) -> TwoDiskStorage {
        TwoDiskStorage(
            locations: storage.locations, release: release ?? storage.release, releaseDirectory: storage.releaseDirectory,
            snapshotIO: io)
    }

    /// A directory under the fixture's root for what a test keeps apart.
    func scratch(_ name: String) -> URL {
        let url = root.appendingPathComponent(name, isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    /// Whether the backup exclusion of `url` reads `value`. macOS applies a
    /// cleared flag through the backup service, so it reads back a moment
    /// after the call returns; the wait is bounded.
    func settles(_ url: URL, to value: Bool) -> Bool {
        let deadline = Date().addingTimeInterval(5)
        while true {
            let flag = try? URL(fileURLWithPath: url.path).resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup
            if flag == value { return true }
            if Date() > deadline { return false }
            Thread.sleep(forTimeInterval: 0.02)
        }
    }

    /// The names under the guest's directory, with the snapshots'.
    func names() -> [String] {
        var result: [String] = []
        let manager = FileManager.default
        for entry in (try? manager.subpathsOfDirectory(atPath: directory.path)) ?? [] {
            result.append(entry)
        }
        return result.sorted()
    }
}
