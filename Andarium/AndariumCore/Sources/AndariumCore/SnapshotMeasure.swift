// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// A run of bytes on the volume, as the file system places a file's data.
public struct PhysicalExtent: Equatable, Sendable {
    public var offset: Int64
    public var length: Int64

    public init(offset: Int64, length: Int64) {
        self.offset = offset
        self.length = length
    }

    var end: Int64 { offset + length }
}

/// What the file system says about the space a clone shares. The calls are
/// APFS's: `getattrlist` with `ATTR_CMNEXT_PRIVATESIZE` (the blocks only this
/// file holds, which deleting it frees) and `fcntl(F_LOG2PHYS_EXT)` over the
/// file's data ranges (where its data lies, so what several files hold
/// together can be counted once). Either can be refused, on a volume that is
/// not APFS or by the platform, and every figure then says it is a lower
/// bound or is absent.
public enum SnapshotMeasure {
    /// The two calls, replaceable so a test can have either refused.
    public struct Primitives: Sendable {
        public var privateSize: @Sendable (URL) -> UInt64?
        public var extents: @Sendable (URL) -> [PhysicalExtent]?

        public init(
            privateSize: @escaping @Sendable (URL) -> UInt64?, extents: @escaping @Sendable (URL) -> [PhysicalExtent]?
        ) {
            self.privateSize = privateSize
            self.extents = extents
        }

        public static let live = Primitives(
            privateSize: { SnapshotMeasure.privateSize(of: $0) }, extents: { SnapshotMeasure.physicalExtents(of: $0) }
        )
    }

    /// The bytes only `url` holds: the blocks it shares with no clone. nil
    /// where the volume or the platform does not say.
    public static func privateSize(of url: URL) -> UInt64? {
        var request = attrlist()
        request.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
        request.commonattr = attrgroup_t(ATTR_CMN_RETURNED_ATTRS)
        request.forkattr = attrgroup_t(ATTR_CMNEXT_PRIVATESIZE)
        // The reply: its length, the attributes returned (five words, the
        // fork attributes the last), then the size.
        var reply = [UInt8](repeating: 0, count: 64)
        let status = reply.withUnsafeMutableBytes { buffer in
            getattrlist(
                url.path, &request, buffer.baseAddress, buffer.count,
                UInt32(FSOPT_ATTR_CMN_EXTENDED | FSOPT_NOFOLLOW))
        }
        guard status == 0 else { return nil }
        let length = reply.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 0, as: UInt32.self) }
        let returnedFork = reply.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 20, as: UInt32.self) }
        guard length >= 32, returnedFork & UInt32(ATTR_CMNEXT_PRIVATESIZE) != 0 else { return nil }
        let size = reply.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 24, as: Int64.self) }
        return size >= 0 ? UInt64(size) : nil
    }

    /// Where the data of `url` lies on the volume, in the file's order.
    /// nil when the file cannot be read or the extent map is refused.
    public static func physicalExtents(of url: URL) -> [PhysicalExtent]? {
        let descriptor = open(url.path, O_RDONLY | O_NOFOLLOW)
        guard descriptor >= 0 else { return nil }
        defer { close(descriptor) }
        var details = stat()
        guard fstat(descriptor, &details) == 0 else { return nil }
        var result: [PhysicalExtent] = []
        var position: off_t = 0
        while position < details.st_size {
            let data = lseek(descriptor, position, SEEK_DATA)
            if data < 0 {
                // ENXIO: no data from here on.
                return errno == ENXIO ? result : nil
            }
            var hole = lseek(descriptor, data, SEEK_HOLE)
            if hole < 0 { hole = details.st_size }
            var at = data
            while at < hole {
                var map = log2phys()
                map.l2p_contigbytes = hole - at
                map.l2p_devoffset = at
                guard fcntl(descriptor, F_LOG2PHYS_EXT, &map) == 0, map.l2p_contigbytes > 0 else { return nil }
                result.append(PhysicalExtent(offset: Int64(map.l2p_devoffset), length: Int64(map.l2p_contigbytes)))
                at += off_t(map.l2p_contigbytes)
            }
            position = hole
        }
        return result
    }

    /// The extents sorted and merged where they touch or overlap.
    public static func merged(_ extents: [PhysicalExtent]) -> [PhysicalExtent] {
        var sorted = extents.filter { $0.length > 0 }
        sorted.sort { $0.offset < $1.offset }
        var result: [PhysicalExtent] = []
        for extent in sorted {
            if let last = result.last, extent.offset <= last.end {
                if extent.end > last.end { result[result.count - 1].length = extent.end - last.offset }
            } else {
                result.append(extent)
            }
        }
        return result
    }

    /// The bytes of `held` that `others` do not cover. Both are sorted and
    /// merged (`merged`).
    public static func bytes(of held: [PhysicalExtent], notIn others: [PhysicalExtent]) -> UInt64 {
        var total: Int64 = 0
        var next = 0
        for extent in held {
            while next < others.count, others[next].end <= extent.offset { next += 1 }
            var cursor = extent.offset
            var index = next
            while index < others.count, others[index].offset < extent.end {
                if others[index].offset > cursor { total += others[index].offset - cursor }
                cursor = max(cursor, others[index].end)
                index += 1
            }
            if cursor < extent.end { total += extent.end - cursor }
        }
        return UInt64(total)
    }

    /// What the file system has allocated to `url`: what it would take on
    /// its own. Zero when it cannot be read.
    public static func allocatedBytes(of url: URL) -> UInt64 {
        // A URL caches the resource values it has read, so each measure
        // starts from the path.
        let values = try? URL(fileURLWithPath: url.path).resourceValues(forKeys: [.fileAllocatedSizeKey])
        return UInt64(values?.fileAllocatedSize ?? 0)
    }

    /// What deleting `held` frees while `others` stay: the held files' bytes
    /// that no other file shares.
    ///
    /// One file is its private size, which the file system keeps exactly.
    /// Several are what their extents hold together less what `others` hold,
    /// which is exact too; if the extent map is refused, the sum of the
    /// private sizes, which is a lower bound because blocks that only two of
    /// them share belong to neither. Without either, zero as a lower bound.
    public static func freed(by held: [URL], keeping others: [URL], using calls: Primitives = .live) -> SnapshotFigure {
        guard !held.isEmpty else { return SnapshotFigure(bytes: 0) }
        if held.count == 1, let size = calls.privateSize(held[0]) {
            return SnapshotFigure(bytes: size)
        }
        let heldMaps = held.map { calls.extents($0) }
        let otherMaps = others.map { calls.extents($0) }
        if heldMaps.allSatisfy({ $0 != nil }), otherMaps.allSatisfy({ $0 != nil }) {
            let mine = merged(heldMaps.flatMap { $0 ?? [] })
            let theirs = merged(otherMaps.flatMap { $0 ?? [] })
            return SnapshotFigure(bytes: bytes(of: mine, notIn: theirs))
        }
        let sizes = held.compactMap { calls.privateSize($0) }
        return SnapshotFigure(bytes: sizes.reduce(0, +), isLowerBound: true)
    }
}
