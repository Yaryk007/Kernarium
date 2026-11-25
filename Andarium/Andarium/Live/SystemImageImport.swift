// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Compression
import CryptoKit
import Foundation

/// Turns whatever image the person picked into a bootable Andarium release
/// folder: a full GPT system disk, an ext4 or EROFS partition image such as
/// a GSI's `system.img` (written into a copy of the carried disk, growing its
/// partition when it is larger), an Android sparse image of either, or any of
/// those compressed with xz or gzip. ROM ZIPs can supply the system image.
///
/// The host builds the system disk's GPT from `partition-layout.json` and
/// compares it byte for byte with the disk (darwin/device/block/android_gpt),
/// so every disk written here carries a GPT built by the same rules
/// (`GptBuilder`), and every layout written here satisfies the host's parser:
/// partitions aligned, packed one after another from the first aligned offset
/// after the table, with `init_boot` and `recovery` among them.
nonisolated enum SystemImageImport {
    /// Called with a fraction of the whole import (0...1) and what it does now.
    typealias Progress = @Sendable (Double, String) -> Void

    static let sector: UInt64 = 512
    static let entrySize: UInt64 = 128

    // MARK: - What a file is

    enum Kind: Equatable {
        case gptDisk
        case erofs
        case ext4
        case sparse
        case xz
        case gzip
        case zip
        case androidBoot
        case unknown
    }

    static func kind(of url: URL) throws -> Kind {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let head = try handle.read(upToCount: 4096) ?? Data()
        func bytes(_ offset: Int, _ count: Int) -> [UInt8] {
            guard head.count >= offset + count else { return [] }
            return Array(head[head.startIndex + offset ..< head.startIndex + offset + count])
        }
        if bytes(0, 6) == [0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00] { return .xz }
        if bytes(0, 2) == [0x1f, 0x8b] { return .gzip }
        if bytes(0, 4) == [0x50, 0x4b, 0x03, 0x04] { return .zip }
        if bytes(0, 4) == [0x3a, 0xff, 0x26, 0xed] { return .sparse }
        if bytes(0, 8) == Array("ANDROID!".utf8) { return .androidBoot }
        if bytes(512, 8) == Array("EFI PART".utf8) { return .gptDisk }
        if bytes(1024, 4) == [0xe2, 0xe1, 0xf5, 0xe0] { return .erofs }
        if bytes(1024 + 0x38, 2) == [0x53, 0xef] { return .ext4 }
        return .unknown
    }

    // MARK: - Unpacking

    /// Expands an xz, gzip or sparse image into `target`, a new file.
    static func unpack(_ source: URL, kind: Kind, into target: URL, progress: Progress) throws {
        switch kind {
        case .xz: try decompress(source, algorithm: .lzma, skipping: 0, trailer: 0, into: target, progress: progress)
        case .gzip: try decompress(source, algorithm: .zlib, skipping: try gzipHeaderLength(source), trailer: 8,
                                   into: target, progress: progress)
        case .sparse: try unsparse(source, into: target, progress: progress)
        default: throw SystemInstallation.Problem.invalid("Nothing to unpack.")
        }
    }

    private static func gzipHeaderLength(_ url: URL) throws -> UInt64 {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let head = [UInt8](try handle.read(upToCount: 64 * 1024) ?? Data())
        guard head.count > 10, head[2] == 8 else { throw SystemInstallation.Problem.invalid("The gzip file is damaged.") }
        let flags = head[3]
        var offset = 10
        if flags & 0x04 != 0 {
            guard offset + 2 <= head.count else { throw SystemInstallation.Problem.invalid("The gzip header is truncated.") }
            let extraLength = Int(head[offset]) | (Int(head[offset + 1]) << 8)
            offset += 2 + extraLength
        }
        for bit: UInt8 in [0x08, 0x10] where flags & bit != 0 {
            while offset < head.count, head[offset] != 0 { offset += 1 }
            offset += 1
        }
        if flags & 0x02 != 0 { offset += 2 }
        guard offset < head.count else { throw SystemInstallation.Problem.invalid("The gzip file is damaged.") }
        return UInt64(offset)
    }

    static func extractZIP(_ archive: ZipArchive, into folder: URL, progress: Progress) throws -> URL {
        let supported = ["system.img", "system.img.xz", "system.img.gz"]
        let images = archive.entries.filter {
            !$0.isDirectory && supported.contains($0.name.split(separator: "/").last?.lowercased() ?? "")
        }
        guard images.count == 1, let image = images.first, image.size > 0,
              image.size <= 16 * 1024 * 1024 * 1024 else { throw SkinImageError.missingSystem }
        let name = String(image.name.split(separator: "/").last!).lowercased()
        let output = folder.appendingPathComponent(name)
        try requireSpace(image.size, at: folder)
        guard FileManager.default.createFile(atPath: output.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        let file = try FileHandle(forWritingTo: output)
        defer { try? file.close() }
        let reader = try archive.reader(for: image, capacity: 256 * 1024)
        var written: UInt64 = 0
        while let data = try reader.read(upTo: 256 * 1024) {
            try file.write(contentsOf: data)
            written += UInt64(data.count)
            progress(Double(written) / Double(image.size), "Unpacking the system image")
        }
        return output
    }

    /// The space an install may count on: what is free, and what the system
    /// says it would give up for something the person asked for. A guest's own
    /// writes hold to the first alone (`HostStorage`), but an install that the
    /// system does not make room for after all only fails and leaves nothing.
    static func installableBytes(at folder: URL) -> UInt64 {
        let values = try? folder.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        let offered = values?.volumeAvailableCapacityForImportantUsage ?? 0
        return max(HostStorage.freeBytes(on: folder), UInt64(clamping: offered))
    }

    static func requireSpace(_ bytes: UInt64, at folder: URL) throws {
        let free = installableBytes(at: folder)
        guard free > 0, free < bytes else { return }
        func text(_ value: UInt64) -> String {
            ByteCountFormatter.string(fromByteCount: Int64(clamping: value), countStyle: .file)
        }
        throw SystemInstallation.Problem.invalid(
            "The next step needs \(text(bytes)) of free storage, and Andarium can use \(text(free)) now.")
    }

    /// What an install that the process did not live to end left behind: its
    /// work in the temporary directory, and its stages beside the systems. Read
    /// at the process's launch, before it can begin one of its own.
    static func leftovers(
        temporary: URL = FileManager.default.temporaryDirectory,
        systems: URL = SystemInstallation.directory(for: .lineage).deletingLastPathComponent()
    ) -> [URL] {
        func entries(_ prefix: String, in folder: URL) -> [URL] {
            ((try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [])
                .filter { $0.lastPathComponent.hasPrefix(prefix) }
        }
        return entries("andarium-system-", in: temporary) + entries(".android-install-", in: systems)
            + GuestSystemChoice.allCases.flatMap {
                entries(".root-install-", in: systems.appendingPathComponent($0.storageName, isDirectory: true))
            }
    }

    /// Gives a read part of a file back to the volume. Both ends are block
    /// aligned, and a volume that cannot do it keeps the bytes until the file goes.
    private static func release(_ offset: UInt64, _ length: UInt64, of file: FileHandle) {
        let block: UInt64 = 16384
        guard offset % block == 0, length >= block else { return }
        var hole = fpunchhole_t(
            fp_flags: 0, reserved: 0, fp_offset: off_t(offset), fp_length: off_t(length / block * block))
        _ = fcntl(file.fileDescriptor, F_PUNCHHOLE, &hole)
    }

    /// Change the copied init_boot ramdisk's first-stage mount before hashing the disk.
    static func mountExt4(_ name: String, disk: URL, layout: DiskLayout) throws {
        guard let boot = layout.partitions.first(where: { $0.name == "init_boot" }), boot.imageBytes >= 4096 else {
            throw SkinImageError.ramdisk
        }
        let file = try FileHandle(forUpdating: disk)
        defer { try? file.close() }
        try file.seek(toOffset: boot.start)
        guard let header = try file.read(upToCount: 44), header.count == 44,
              header.prefix(8) == Data("ANDROID!".utf8), header[8..<12].allSatisfy({ $0 == 0 }),
              header[40..<44] == Data([4, 0, 0, 0]) else { throw SkinImageError.ramdisk }
        let size = header[12..<16].enumerated().reduce(0) { $0 | (Int($1.element) << (8 * $1.offset)) }
        guard size > 0, UInt64(size) <= boot.imageBytes - 4096 else { throw SkinImageError.ramdisk }
        let start = boot.start + 4096
        try file.seek(toOffset: start)
        guard let ramdisk = try file.read(upToCount: size), ramdisk.count == size else { throw SkinImageError.ramdisk }
        let offset = try ext4MountOffset(for: name, in: ramdisk)
        try file.seek(toOffset: start + UInt64(offset))
        try file.write(contentsOf: Data("ext4 ".utf8))
    }

    private static func decompress(_ source: URL, algorithm: Algorithm, skipping: UInt64, trailer: UInt64,
                                   into target: URL, progress: Progress) throws {
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        let total = try input.seekToEnd()
        guard total > skipping + trailer else { throw SystemInstallation.Problem.invalid("The compressed file is empty.") }
        let end = total - trailer
        try input.seek(toOffset: skipping)
        guard FileManager.default.createFile(atPath: target.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        let output = try FileHandle(forWritingTo: target)
        defer { try? output.close() }
        var writeError: (any Error)?
        var written: UInt64 = 0
        let filter = try OutputFilter(.decompress, using: algorithm) { data in
            guard let data, writeError == nil else { return }
            written += UInt64(data.count)
            guard written <= 16 * 1024 * 1024 * 1024 else { writeError = SkinImageError.archive; return }
            do { try output.write(contentsOf: data) } catch { writeError = error }
        }
        var position = skipping
        while position < end {
            try Task.checkCancellation()
            let count = Int(min(UInt64(8 << 20), end - position))
            guard let chunk = try input.read(upToCount: count), !chunk.isEmpty else { throw SkinImageError.archive }
            try filter.write(chunk)
            if let writeError { throw writeError }
            position += UInt64(chunk.count)
            progress(Double(position - skipping) / Double(end - skipping), "Unpacking the system image")
        }
        try filter.finalize()
        if let writeError { throw writeError }
    }

    /// Android's sparse format (libsparse): a header, then raw, fill,
    /// don't-care and CRC chunks of whole blocks.
    private static func unsparse(_ source: URL, into target: URL, progress: Progress) throws {
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        func u16(_ d: Data, _ o: Int) -> UInt32 { UInt32(d[d.startIndex + o]) | UInt32(d[d.startIndex + o + 1]) << 8 }
        func u32(_ d: Data, _ o: Int) -> UInt32 { u16(d, o) | u16(d, o + 2) << 16 }
        guard let header = try input.read(upToCount: 28), header.count == 28 else {
            throw SystemInstallation.Problem.invalid("The sparse image is damaged.")
        }
        let headerSize = UInt64(u16(header, 8)), chunkHeaderSize = Int(u16(header, 10))
        let blockSize = UInt64(u32(header, 12)), blocks = UInt64(u32(header, 16)), chunks = u32(header, 20)
        guard headerSize >= 28, chunkHeaderSize >= 12, blockSize > 0, blockSize % 4 == 0 else {
            throw SystemInstallation.Problem.invalid("The sparse image has a header this version does not read.")
        }
        try input.seek(toOffset: headerSize)
        guard FileManager.default.createFile(atPath: target.path, contents: nil) else {
            throw SystemInstallation.Problem.invalid("Andarium cannot write into its Systems folder.")
        }
        let output = try FileHandle(forWritingTo: target)
        defer { try? output.close() }
        try output.truncate(atOffset: blocks * blockSize)
        var block: UInt64 = 0
        for index in 0 ..< chunks {
            guard let chunk = try input.read(upToCount: chunkHeaderSize), chunk.count == chunkHeaderSize else {
                throw SystemInstallation.Problem.invalid("The sparse image ends early.")
            }
            let type = u16(chunk, 0), count = UInt64(u32(chunk, 4)), bytes = UInt64(u32(chunk, 8)) - UInt64(chunkHeaderSize)
            guard block + count <= blocks else { throw SystemInstallation.Problem.invalid("The sparse image is damaged.") }
            switch type {
            case 0xcac1: // raw
                try output.seek(toOffset: block * blockSize)
                var left = count * blockSize
                while left > 0 {
                    guard let data = try input.read(upToCount: Int(min(left, 8 << 20))), !data.isEmpty else {
                        throw SystemInstallation.Problem.invalid("The sparse image ends early.")
                    }
                    try output.write(contentsOf: data)
                    left -= UInt64(data.count)
                }
            case 0xcac2: // fill
                guard let value = try input.read(upToCount: 4), value.count == 4 else {
                    throw SystemInstallation.Problem.invalid("The sparse image ends early.")
                }
                if value != Data(count: 4) {
                    let pattern = Data((0 ..< (1 << 18)).flatMap { _ in value })
                    try output.seek(toOffset: block * blockSize)
                    var left = count * blockSize
                    while left > 0 {
                        let piece = pattern.prefix(Int(min(left, UInt64(pattern.count))))
                        try output.write(contentsOf: piece)
                        left -= UInt64(piece.count)
                    }
                }
            case 0xcac3: break // don't care: the file is already zero there
            case 0xcac4: try input.seek(toOffset: try input.offset() + bytes) // CRC
            default: throw SystemInstallation.Problem.invalid("The sparse image has a chunk this version does not read.")
            }
            block += count
            progress(Double(index + 1) / Double(max(chunks, 1)), "Expanding the sparse image")
        }
    }

    // MARK: - Layouts

    struct Partition {
        var name: String
        var start: UInt64
        var size: UInt64
        var imageBytes: UInt64
        var typeGUID: String
        var uniqueGUID: String
        var sha256 = ""
    }

    struct DiskLayout {
        var size: UInt64
        var alignment: UInt64
        var entryCount: UInt64
        var diskGUID: String
        var partitions: [Partition]
        var tableBytes: UInt64 { entryCount * SystemImageImport.entrySize }
    }

    /// The carried release's layout document and its system disk.
    static func carriedLayout(in folder: URL) throws -> (document: [String: Any], disk: DiskLayout) {
        guard let document = try JSONSerialization.jsonObject(
                with: Data(contentsOf: folder.appendingPathComponent(SystemFolder.layout))) as? [String: Any],
              let system = document["system_disk"] as? [String: Any],
              let disk = diskLayout(system) else {
            throw SystemInstallation.Problem.invalid("The base system’s partition layout cannot be read.")
        }
        return (document, disk)
    }

    static func diskLayout(_ system: [String: Any]) -> DiskLayout? {
        func number(_ object: [String: Any], _ key: String) -> UInt64? { (object[key] as? NSNumber)?.uint64Value }
        guard let size = number(system, "size_bytes"), let alignment = number(system, "alignment_bytes"),
              let entries = number(system, "partition_entry_count"), let guid = system["disk_guid"] as? String,
              let list = system["partitions"] as? [[String: Any]] else { return nil }
        var partitions: [Partition] = []
        for item in list {
            guard let name = item["name"] as? String, let start = number(item, "start_bytes"),
                  let length = number(item, "size_bytes"), let image = number(item, "image_bytes"),
                  let type = item["type_guid"] as? String, let unique = item["unique_guid"] as? String else { return nil }
            partitions.append(Partition(name: name, start: start, size: length, imageBytes: image, typeGUID: type,
                                        uniqueGUID: unique, sha256: item["sha256"] as? String ?? ""))
        }
        return DiskLayout(size: size, alignment: alignment, entryCount: entries, diskGUID: guid, partitions: partitions)
    }

    static func json(_ disk: DiskLayout, wholeSHA256: String) -> [String: Any] {
        [
            "size_bytes": disk.size,
            "sha256": wholeSHA256,
            "alignment_bytes": disk.alignment,
            "partition_entry_count": disk.entryCount,
            "disk_guid": disk.diskGUID,
            "partitions": disk.partitions.map {
                ["name": $0.name, "start_bytes": $0.start, "size_bytes": $0.size, "image_bytes": $0.imageBytes,
                 "sha256": $0.sha256, "type_guid": $0.typeGUID, "unique_guid": $0.uniqueGUID] as [String: Any]
            },
        ]
    }

    static func alignUp(_ value: UInt64, _ alignment: UInt64) -> UInt64 { (value + alignment - 1) / alignment * alignment }

    /// What the host's layout parser would refuse (android_layout.cpp), in
    /// words, or nil.
    static func hostProblem(_ disk: DiskLayout) -> String? {
        guard disk.alignment % 4096 == 0, disk.size % disk.alignment == 0 else { return "The disk is not aligned to 4 KiB." }
        guard disk.entryCount > 0, disk.entryCount <= 4096, disk.tableBytes % sector == 0 else {
            return "The disk's GPT has an unsupported entry count."
        }
        guard !disk.partitions.isEmpty, disk.partitions.count <= Int(disk.entryCount) else { return "The disk has no partitions." }
        var cursor = alignUp(2 * sector + disk.tableBytes, disk.alignment)
        for partition in disk.partitions {
            guard partition.start % disk.alignment == 0, partition.size % disk.alignment == 0, partition.size > 0 else {
                return "Partition \(partition.name) is not aligned to \(disk.alignment) bytes."
            }
            guard partition.start == cursor else {
                return "Partition \(partition.name) does not follow the one before it; Andarium needs packed partitions."
            }
            guard partition.imageBytes <= partition.size, partition.size - partition.imageBytes < disk.alignment else {
                return "Partition \(partition.name) does not fit its image."
            }
            cursor += partition.size
        }
        guard alignUp(cursor + disk.tableBytes + sector, disk.alignment) == disk.size else {
            return "The disk has space after its last partition."
        }
        let names = Set(disk.partitions.map(\.name))
        for required in ["init_boot", "recovery", "system", "vendor"] where !names.contains(required) {
            return "The disk has no \(required) partition, so it is not an Andarium system disk."
        }
        guard names.count == disk.partitions.count else { return "The disk repeats a partition name." }
        return nil
    }

    // MARK: - GPT

    /// Reads a disk's primary GPT into a layout. Image sizes are taken as the
    /// whole partitions: the host only bounds the boot ramdisk with them.
    static func readGPT(_ url: URL, size: UInt64, alignment: UInt64) throws -> DiskLayout {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        try handle.seek(toOffset: sector)
        guard let header = try handle.read(upToCount: 92), header.count == 92 else {
            throw SystemInstallation.Problem.invalid("The disk's GPT header cannot be read.")
        }
        let h = [UInt8](header)
        func le(_ b: [UInt8], _ o: Int, _ n: Int) -> UInt64 { (0 ..< n).reduce(0) { $0 | UInt64(b[o + $1]) << (8 * UInt64($1)) } }
        let entriesLBA = le(h, 72, 8), count = le(h, 80, 4), entryStride = le(h, 84, 4)
        guard entryStride == entrySize, count > 0, count <= 4096 else {
            throw SystemInstallation.Problem.invalid("The disk's GPT has an unsupported entry size.")
        }
        try handle.seek(toOffset: entriesLBA * sector)
        guard let table = try handle.read(upToCount: Int(count * entryStride)), UInt64(table.count) == count * entryStride else {
            throw SystemInstallation.Problem.invalid("The disk's GPT entries cannot be read.")
        }
        let t = [UInt8](table)
        var partitions: [Partition] = []
        for index in 0 ..< Int(count) {
            let o = index * Int(entryStride)
            let type = Array(t[o ..< o + 16])
            if type.allSatisfy({ $0 == 0 }) { continue }
            let first = le(t, o + 32, 8), last = le(t, o + 40, 8)
            var name = ""
            for c in stride(from: o + 56, to: o + 128, by: 2) {
                let value = UInt16(t[c]) | UInt16(t[c + 1]) << 8
                if value == 0 { break }
                name.append(Character(UnicodeScalar(UInt8(truncatingIfNeeded: value))))
            }
            let bytes = (last - first + 1) * sector
            partitions.append(Partition(name: name, start: first * sector, size: bytes, imageBytes: bytes,
                                        typeGUID: guidString(type), uniqueGUID: guidString(Array(t[o + 16 ..< o + 32]))))
        }
        return DiskLayout(size: size, alignment: alignment, entryCount: count,
                          diskGUID: guidString(Array(h[56 ..< 72])), partitions: partitions)
    }

    /// The canonical text of GUID bytes as GPT stores them (the first three
    /// fields little endian).
    static func guidString(_ b: [UInt8]) -> String {
        let order = [3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11, 12, 13, 14, 15]
        return order.map { $0 < 0 ? "-" : String(format: "%02x", b[$0]) }.joined()
    }

    static func guidBytes(_ text: String) -> [UInt8] {
        let hex = Array(text.replacingOccurrences(of: "-", with: ""))
        let canonical = stride(from: 0, to: 32, by: 2).map { UInt8(String(hex[$0 ... $0 + 1]), radix: 16) ?? 0 }
        return [canonical[3], canonical[2], canonical[1], canonical[0], canonical[5], canonical[4],
                canonical[7], canonical[6]] + canonical[8 ..< 16]
    }

    static func crc32(_ bytes: [UInt8]) -> UInt32 {
        var value: UInt32 = ~0
        for byte in bytes {
            value ^= UInt32(byte)
            for _ in 0 ..< 8 { value = (value >> 1) ^ ((value & 1) != 0 ? 0xedb8_8320 : 0) }
        }
        return ~value
    }

    /// Writes the protective MBR and both GPTs exactly as the host's
    /// buildGpt() makes them.
    static func writeGPT(_ disk: DiskLayout, to handle: FileHandle) throws {
        let sectors = disk.size / sector
        let tableSectors = disk.tableBytes / sector
        let firstUsable = 2 + tableSectors, lastUsable = sectors - tableSectors - 2
        func put(_ value: UInt64, _ count: Int, into buffer: inout [UInt8], at offset: Int) {
            for i in 0 ..< count { buffer[offset + i] = UInt8(truncatingIfNeeded: value >> (8 * UInt64(i))) }
        }
        var entries = [UInt8](repeating: 0, count: Int(disk.tableBytes))
        for (index, partition) in disk.partitions.enumerated() {
            let o = index * Int(entrySize)
            entries.replaceSubrange(o ..< o + 16, with: guidBytes(partition.typeGUID))
            entries.replaceSubrange(o + 16 ..< o + 32, with: guidBytes(partition.uniqueGUID))
            put(partition.start / sector, 8, into: &entries, at: o + 32)
            put((partition.start + partition.size) / sector - 1, 8, into: &entries, at: o + 40)
            for (c, scalar) in partition.name.utf8.prefix(36).enumerated() { entries[o + 56 + c * 2] = scalar }
        }
        let entriesCRC = crc32(entries)
        func header(current: UInt64, backup: UInt64, entriesLBA: UInt64) -> [UInt8] {
            var h = [UInt8](repeating: 0, count: Int(sector))
            h.replaceSubrange(0 ..< 8, with: Array("EFI PART".utf8))
            put(0x0001_0000, 4, into: &h, at: 8)
            put(92, 4, into: &h, at: 12)
            put(current, 8, into: &h, at: 24)
            put(backup, 8, into: &h, at: 32)
            put(firstUsable, 8, into: &h, at: 40)
            put(lastUsable, 8, into: &h, at: 48)
            h.replaceSubrange(56 ..< 72, with: guidBytes(disk.diskGUID))
            put(entriesLBA, 8, into: &h, at: 72)
            put(disk.entryCount, 4, into: &h, at: 80)
            put(entrySize, 4, into: &h, at: 84)
            put(UInt64(entriesCRC), 4, into: &h, at: 88)
            put(UInt64(crc32(Array(h[0 ..< 92]))), 4, into: &h, at: 16)
            return h
        }
        var mbr = [UInt8](repeating: 0, count: Int(sector))
        mbr[446 + 2] = 0x02
        mbr[446 + 4] = 0xee
        mbr[446 + 5] = 0xff; mbr[446 + 6] = 0xff; mbr[446 + 7] = 0xff
        put(1, 4, into: &mbr, at: 446 + 8)
        put(min(sectors - 1, UInt64(UInt32.max)), 4, into: &mbr, at: 446 + 12)
        mbr[510] = 0x55; mbr[511] = 0xaa
        let backupEntriesLBA = sectors - 1 - tableSectors
        for (offset, bytes) in [(UInt64(0), mbr), (sector, header(current: 1, backup: sectors - 1, entriesLBA: 2)),
                                (2 * sector, entries), (backupEntriesLBA * sector, entries),
                                ((sectors - 1) * sector, header(current: sectors - 1, backup: 1, entriesLBA: backupEntriesLBA))] {
            try handle.seek(toOffset: offset)
            try handle.write(contentsOf: Data(bytes))
        }
    }

    // MARK: - Disks

    /// Hashes a disk and each of its partitions in one pass.
    static func hash(_ url: URL, layout: inout DiskLayout, progress: Progress) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var whole = SHA256()
        var parts = layout.partitions.map { _ in SHA256() }
        var position: UInt64 = 0
        while let chunk = try handle.read(upToCount: 8 << 20), !chunk.isEmpty {
            try Task.checkCancellation()
            whole.update(data: chunk)
            let end = position + UInt64(chunk.count)
            for (index, partition) in layout.partitions.enumerated() {
                let from = max(position, partition.start), to = min(end, partition.start + partition.size)
                if from < to {
                    parts[index].update(data: chunk[chunk.startIndex + Int(from - position) ..< chunk.startIndex + Int(to - position)])
                }
            }
            position = end
            progress(Double(position) / Double(max(layout.size, 1)), "Checking the disk")
        }
        func hex(_ digest: SHA256.Digest) -> String { digest.map { String(format: "%02x", $0) }.joined() }
        for index in layout.partitions.indices { layout.partitions[index].sha256 = hex(parts[index].finalize()) }
        return hex(whole.finalize())
    }

    /// Builds a copy of the carried disk with `image` in partition `target`,
    /// grown to hold it when it is larger. With `consuming` the image is the
    /// install's own work, and gives its space back as it is copied.
    static func assemble(replacing target: String, with image: URL, consuming: Bool, carried: DiskLayout,
                         carriedDisk: URL, into output: URL, progress: Progress) throws -> DiskLayout {
        let imageBytes = UInt64((try image.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        guard imageBytes > 0 else { throw SystemInstallation.Problem.invalid("The image is empty.") }
        var disk = carried
        var cursor = alignUp(2 * sector + disk.tableBytes, disk.alignment)
        for index in disk.partitions.indices {
            if disk.partitions[index].name == target {
                disk.partitions[index].size = alignUp(imageBytes, disk.alignment)
                disk.partitions[index].imageBytes = imageBytes
            }
            disk.partitions[index].start = cursor
            cursor += disk.partitions[index].size
        }
        disk.size = alignUp(cursor + disk.tableBytes + sector, disk.alignment)
        if let problem = hostProblem(disk) { throw SystemInstallation.Problem.invalid(problem) }

        guard FileManager.default.createFile(atPath: output.path, contents: nil) else {
            throw SystemInstallation.Problem.invalid("Andarium cannot write into its Systems folder.")
        }
        let out = try FileHandle(forWritingTo: output)
        defer { try? out.close() }
        try out.truncate(atOffset: disk.size)
        try writeGPT(disk, to: out)
        let source = try FileHandle(forReadingFrom: carriedDisk)
        defer { try? source.close() }
        let replacement = try consuming ? FileHandle(forUpdating: image) : FileHandle(forReadingFrom: image)
        defer { try? replacement.close() }
        var copied: UInt64 = 0
        for (index, partition) in disk.partitions.enumerated() {
            let original = carried.partitions[index]
            let input = partition.name == target ? replacement : source
            try input.seek(toOffset: partition.name == target ? 0 : original.start)
            try out.seek(toOffset: partition.start)
            var left = partition.name == target ? imageBytes : original.size
            while left > 0 {
                try Task.checkCancellation()
                guard let data = try input.read(upToCount: Int(min(left, 8 << 20))), !data.isEmpty else {
                    throw SystemInstallation.Problem.invalid("A system partition ended early.")
                }
                try out.write(contentsOf: data)
                if consuming, partition.name == target {
                    release(imageBytes - left, UInt64(data.count), of: replacement)
                }
                left -= UInt64(data.count)
                copied += UInt64(data.count)
                progress(Double(copied) / Double(disk.size), "Writing the system disk")
            }
        }
        try out.synchronize()
        return disk
    }

    /// The carried partition an image goes into, from its file name
    /// (`vendor.img`, `product_gsi.img`…); a GSI or anything else is `system`.
    static func targetPartition(for fileName: String, in carried: DiskLayout) -> String {
        let stem = fileName.lowercased().split(separator: ".").first.map(String.init) ?? ""
        let names = carried.partitions.map(\.name).sorted { $0.count > $1.count }
        for name in names where name != "system" && ["system_ext", "product", "vendor", "odm"].contains(name) {
            if stem == name || stem.hasPrefix(name + "_") || stem.hasPrefix(name + "-") { return name }
        }
        return "system"
    }
}
