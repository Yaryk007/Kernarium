// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// The file calls the snapshot store makes, as values, so that a test can have
/// any of them fail with a real error and can look at the guest's files after
/// each one (`SnapshotStore`). The application uses `live`. A call that reads
/// (a record, the size of a file) is not here: only what changes files, and
/// the listing of a directory.
struct SnapshotFileOperations: Sendable {
    var createDirectory: @Sendable (_ directory: URL) throws -> Void
    /// An APFS clone of `source` at the absent `destination`
    /// (`CloneFile.Failure` carries the errno).
    var clone: @Sendable (_ source: URL, _ destination: URL) throws -> Void
    var rename: @Sendable (_ source: URL, _ destination: URL) throws -> Void
    /// Replaces the file's contents atomically; not yet flushed.
    var write: @Sendable (_ data: Data, _ file: URL) throws -> Void
    /// Makes a file's contents, or a directory's entries, survive a power loss.
    var flush: @Sendable (_ url: URL) throws -> Void
    var remove: @Sendable (_ url: URL) throws -> Void
    var list: @Sendable (_ directory: URL) throws -> [String]
    /// Whether the volume `url` is on can clone files; nil when it does not say.
    var volumeClones: @Sendable (_ url: URL) -> Bool?
    var measure = SnapshotMeasure.Primitives.live

    static let live = SnapshotFileOperations(
        createDirectory: { try FileManager.default.createDirectory(at: $0, withIntermediateDirectories: true) },
        clone: { try CloneFile.clone($0, to: $1) },
        rename: { source, destination in
            guard Darwin.rename(source.path, destination.path) == 0 else {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        },
        write: { try $0.write(to: $1, options: .atomic) },
        flush: { url in
            let descriptor = open(url.path, O_RDONLY)
            guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
            defer { close(descriptor) }
            // `F_FULLFSYNC` where the volume has it, else `fsync`.
            if fcntl(descriptor, F_FULLFSYNC) != 0, fsync(descriptor) != 0 {
                throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
            }
        },
        remove: { try FileManager.default.removeItem(at: $0) },
        list: { try FileManager.default.contentsOfDirectory(atPath: $0.path) },
        volumeClones: { VolumeCapabilities.clones(at: $0) }
    )
}

/// What the volume under a path says it can do.
enum VolumeCapabilities {
    /// Whether the volume supports `clonefile(2)` (`VOL_CAP_INT_CLONE`); nil
    /// when the question is refused or the volume does not know it.
    static func clones(at url: URL) -> Bool? {
        var request = attrlist()
        request.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
        request.volattr = ATTR_VOL_INFO | attrgroup_t(ATTR_VOL_CAPABILITIES)
        // The reply: its length, then the capabilities as four words of
        // supported bits and four of the bits the volume has an answer for.
        var reply = [UInt8](repeating: 0, count: 64)
        let status = reply.withUnsafeMutableBytes { buffer in
            getattrlist(url.path, &request, buffer.baseAddress, buffer.count, 0)
        }
        guard status == 0 else { return nil }
        let length = reply.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) }
        guard length >= 4 + 8 * 4 else { return nil }
        let word = 4 * Int(VOL_CAPABILITIES_INTERFACES)
        let supported = reply.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4 + word, as: UInt32.self) }
        let known = reply.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4 + 16 + word, as: UInt32.self) }
        let bit = UInt32(VOL_CAP_INT_CLONE)
        // A bit the volume has no answer for says nothing.
        guard known & bit != 0 else { return nil }
        return supported & bit != 0
    }
}
