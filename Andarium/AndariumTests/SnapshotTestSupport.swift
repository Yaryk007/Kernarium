// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Darwin
import Foundation
@testable import Andarium

/// Sparse disks with known contents in the test host's own temporary
/// directory, written through to the volume. The snapshot tests touch no
/// guest's files.
enum SnapshotTestDisk {
    /// A sparse disk of `size` bytes with `first` mebibytes written at its start.
    static func make(at url: URL, size: Int64, firstMiB: Int = 0, value: UInt8 = 1) throws {
        let fd = open(url.path, O_RDWR | O_CREAT, 0o600)
        guard fd >= 0, ftruncate(fd, off_t(size)) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        close(fd)
        if firstMiB > 0 { try write(url, at: 0, count: firstMiB << 20, value: value) }
    }

    static func write(_ url: URL, at offset: Int64, count: Int, value: UInt8) throws {
        let fd = open(url.path, O_RDWR)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(fd) }
        let bytes = [UInt8](repeating: value, count: count)
        guard pwrite(fd, bytes, count, off_t(offset)) == count else { throw POSIXError(.EIO) }
        _ = fcntl(fd, F_FULLFSYNC)
    }

    static func read(_ url: URL, at offset: Int64) throws -> UInt8 {
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(fd) }
        var byte: UInt8 = 0
        guard pread(fd, &byte, 1, off_t(offset)) == 1 else { throw POSIXError(.EIO) }
        return byte
    }
}

/// A guest in a temporary directory that has run once and been shut down by
/// Android: a 1 GiB data disk with its first mebibyte written, a release that
/// accepts it, and the record of a completed run.
struct SnapshotTestGuest {
    let directory: URL
    let storage: TwoDiskStorage
    let guest: Guest
    var disk: URL { storage.locations.data }

    static func make() async throws -> SnapshotTestGuest {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-snapshot-test-\(UUID().uuidString)", isDirectory: true)
        let release = ReleaseIdentity(
            releaseID: "20261031-los24-r4", buildFingerprint: "fp", buildNumber: 100, securityPatch: "2026-10-01",
            systemDiskSHA256: String(repeating: "a", count: 64), dataLayout: 1, minimumGiB: 1, defaultGiB: 1,
            maximumGiB: 8)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory.appendingPathComponent("Guest")),
            release: release, releaseDirectory: directory.appendingPathComponent("system"))
        let guest = try await storage.create(Guest.fresh(on: AppEntry.hostDevice()))
        // The guest was set up an hour ago, the runtime made the disk, and
        // Android shut itself down a minute later: a later start is told
        // from this run by the clock.
        let files = InstanceFiles(directory: storage.locations.directory)
        let setUp = Date().addingTimeInterval(-3600)
        try files.apply(InstanceDecision(.create, "A new guest."), release: release, now: setUp)
        try SnapshotTestDisk.make(at: storage.locations.data, size: 1 << 30, firstMiB: 1)
        try files.complete(release: release, now: setUp.addingTimeInterval(60))
        return SnapshotTestGuest(directory: directory, storage: storage, guest: guest)
    }

    func remove() { try? FileManager.default.removeItem(at: directory) }
}
