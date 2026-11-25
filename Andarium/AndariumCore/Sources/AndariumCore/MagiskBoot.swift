// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Makes the Magisk-patched `init_boot` of a release on the device: the stock
/// image plus exactly what Magisk's own patcher adds
/// (android/lineage-24/tools/magisk_image.py is the rule), from the official
/// app's unmodified binaries. The ramdisk stays a raw newc archive, and the xz
/// members hold their bytes stored, both of which the guest reads as it reads
/// Magisk's own.
public enum MagiskBoot {
    public enum Problem: Error, Equatable, Sendable {
        /// Not a kernelless boot image v4 with a raw newc ramdisk.
        case unsupportedImage
        /// The ramdisk already holds what the patch adds.
        case alreadyPatched
    }

    /// The official release's files, of the arm64 guest.
    public struct Binaries: Sendable {
        public static let initMember = "lib/arm64-v8a/libmagiskinit.so"
        public static let magiskMember = "lib/arm64-v8a/libmagisk.so"
        public static let loaderMember = "lib/arm64-v8a/libinit-ld.so"

        public var initBinary: Data
        public var magisk: Data
        public var loader: Data

        public init(initBinary: Data, magisk: Data, loader: Data) {
            self.initBinary = initBinary
            self.magisk = magisk
            self.loader = loader
        }
    }

    /// `preinit` is the guest's name of the data disk's userdata partition and
    /// `stockSHA1` the digest of the whole stock image, which Magisk keeps.
    public static func patch(stock: Data, binaries: Binaries, preinit: String, stockSHA1: String) throws -> Data {
        let image = [UInt8](stock)
        guard image.count > pageSize, Array(image[0..<8]) == Array("ANDROID!".utf8),
              little32(image, 8) == 0, little32(image, 40) == 4
        else { throw Problem.unsupportedImage }
        let size = little32(image, 12)
        guard size > 0, size <= image.count - pageSize else { throw Problem.unsupportedImage }
        let ramdisk = Array(image[pageSize..<(pageSize + size)])

        var entries = try parse(ramdisk)
        let added = [
            ".backup", ".backup/.magisk", ".backup/.rmlist", ".backup/init.xz",
            "overlay.d", "overlay.d/sbin", "overlay.d/sbin/magisk.xz", "overlay.d/sbin/init-ld.xz",
        ]
        guard !entries.contains(where: { added.contains($0.name) }) else { throw Problem.alreadyPatched }
        let inits = entries.indices.filter { entries[$0].name == "init" }
        guard inits.count == 1, let index = inits.first, entries[index].mode & typeMask == regularFile else {
            throw Problem.unsupportedImage
        }
        let stockInit = entries[index]
        entries[index].payload = [UInt8](binaries.initBinary)
        entries[index].mode = regularFile | 0o750

        var inode = entries.map(\.inode).max() ?? 0
        func add(_ name: String, _ mode: UInt32, _ payload: [UInt8] = []) {
            inode += 1
            entries.append(Entry(name: name, inode: inode, mode: mode, links: 1, payload: payload))
        }
        let config = """
            KEEPVERITY=true
            KEEPFORCEENCRYPT=false
            RECOVERYMODE=false
            VENDORBOOT=false
            PREINITDEVICE=\(preinit)
            SHA1=\(stockSHA1)

            """
        let removal = ["overlay.d", "overlay.d/sbin", "overlay.d/sbin/init-ld.xz", "overlay.d/sbin/magisk.xz"]
        add(".backup", directory)
        add(".backup/.magisk", regularFile, Array(config.utf8))
        add(".backup/.rmlist", regularFile, removal.flatMap { Array($0.utf8) + [0] })
        add(".backup/init.xz", stockInit.mode, storedXZ(stockInit.payload))
        add("overlay.d", directory | 0o750)
        add("overlay.d/sbin", directory | 0o750)
        add("overlay.d/sbin/magisk.xz", regularFile | 0o644, storedXZ([UInt8](binaries.magisk)))
        add("overlay.d/sbin/init-ld.xz", regularFile | 0o644, storedXZ([UInt8](binaries.loader)))

        let patched = serialize(entries)
        var output = Array(image[0..<pageSize])
        put32(UInt32(patched.count), into: &output, at: 12)
        output += patched
        output += [UInt8](repeating: 0, count: (pageSize - output.count % pageSize) % pageSize)
        return Data(output)
    }

    // MARK: newc

    private static let pageSize = 4096
    private static let headerSize = 110
    private static let typeMask: UInt32 = 0o170000
    private static let regularFile: UInt32 = 0o100000
    private static let directory: UInt32 = 0o040000

    private struct Entry {
        var name: String
        var inode: UInt32
        var mode: UInt32
        var uid: UInt32 = 0
        var gid: UInt32 = 0
        var links: UInt32
        var modified: UInt32 = 0
        var device: (UInt32, UInt32) = (0, 0)
        var rdevice: (UInt32, UInt32) = (0, 0)
        var payload: [UInt8]
    }

    private static func parse(_ data: [UInt8]) throws -> [Entry] {
        var entries: [Entry] = []
        var offset = 0
        while true {
            guard offset + headerSize <= data.count, Array(data[offset..<(offset + 6)]) == Array("070701".utf8) else {
                throw Problem.unsupportedImage
            }
            var fields: [UInt32] = []
            for number in 0..<13 {
                let start = offset + 6 + 8 * number
                let digits = String(decoding: data[start..<(start + 8)], as: UTF8.self)
                guard let value = UInt32(digits, radix: 16) else { throw Problem.unsupportedImage }
                fields.append(value)
            }
            let nameSize = Int(fields[11]), fileSize = Int(fields[6])
            let nameStart = offset + headerSize
            guard nameSize >= 2, nameSize <= 4096, nameStart + nameSize <= data.count,
                  data[nameStart + nameSize - 1] == 0
            else { throw Problem.unsupportedImage }
            let name = String(decoding: data[nameStart..<(nameStart + nameSize - 1)], as: UTF8.self)
            let body = aligned(nameStart + nameSize)
            if name == "TRAILER!!!" {
                guard fileSize == 0 else { throw Problem.unsupportedImage }
                // A second archive would be dropped here.
                guard body >= data.count || data[body...].allSatisfy({ $0 == 0 }) else { throw Problem.unsupportedImage }
                return entries
            }
            guard body + fileSize <= data.count else { throw Problem.unsupportedImage }
            entries.append(Entry(
                name: name, inode: fields[0], mode: fields[1], uid: fields[2], gid: fields[3], links: fields[4],
                modified: fields[5], device: (fields[7], fields[8]), rdevice: (fields[9], fields[10]),
                payload: Array(data[body..<(body + fileSize)])))
            offset = aligned(body + fileSize)
        }
    }

    private static func serialize(_ entries: [Entry]) -> [UInt8] {
        var output: [UInt8] = []
        let trailer = Entry(name: "TRAILER!!!", inode: 0, mode: 0, links: 1, payload: [])
        for entry in entries + [trailer] {
            let name = Array(entry.name.utf8) + [0]
            let fields = [
                entry.inode, entry.mode, entry.uid, entry.gid, entry.links, entry.modified,
                UInt32(entry.payload.count), entry.device.0, entry.device.1, entry.rdevice.0, entry.rdevice.1,
                UInt32(name.count), 0,
            ]
            output += Array("070701".utf8)
            for field in fields {
                let digits = String(field, radix: 16, uppercase: true)
                output += Array((String(repeating: "0", count: 8 - digits.count) + digits).utf8)
            }
            output += name
            pad(&output)
            output += entry.payload
            pad(&output)
        }
        return output
    }

    private static func aligned(_ value: Int) -> Int { (value + 3) / 4 * 4 }
    private static func pad(_ bytes: inout [UInt8]) {
        bytes += [UInt8](repeating: 0, count: aligned(bytes.count) - bytes.count)
    }

    // MARK: xz

    /// An xz stream of one block whose LZMA2 chunks are stored, with the CRC32
    /// check that Magisk's own decoder reads.
    static func storedXZ(_ data: [UInt8]) -> [UInt8] {
        let flags: [UInt8] = [0, 1]
        var output: [UInt8] = [0xfd, 0x37, 0x7a, 0x58, 0x5a, 0] + flags + bytes32(crc32(flags))
        // One LZMA2 filter with a 1 MiB dictionary.
        let header: [UInt8] = [2, 0, 0x21, 1, 16, 0, 0, 0]
        let blockStart = output.count
        output += header + bytes32(crc32(header))
        var position = 0
        while position < data.count {
            let count = min(1 << 16, data.count - position)
            output += [position == 0 ? 1 : 2, UInt8((count - 1) >> 8), UInt8((count - 1) & 0xff)]
            output += data[position..<(position + count)]
            position += count
        }
        output.append(0)
        let unpadded = output.count - blockStart + 4
        pad(&output)
        output += bytes32(crc32(data))

        var index: [UInt8] = [0, 1] + varint(unpadded) + varint(data.count)
        pad(&index)
        index += bytes32(crc32(index))
        output += index
        let footer = bytes32(UInt32(index.count / 4 - 1)) + flags
        return output + bytes32(crc32(footer)) + footer + [0x59, 0x5a]
    }

    private static func varint(_ value: Int) -> [UInt8] {
        var rest = value
        var output: [UInt8] = []
        while rest >= 0x80 {
            output.append(UInt8(rest & 0x7f) | 0x80)
            rest >>= 7
        }
        return output + [UInt8(rest)]
    }

    private static let crcTable: [UInt32] = (0..<256).map { byte in
        (0..<8).reduce(UInt32(byte)) { value, _ in (value >> 1) ^ ((value & 1) != 0 ? 0xedb8_8320 : 0) }
    }

    private static func crc32(_ data: [UInt8]) -> UInt32 {
        ~data.reduce(~UInt32(0)) { ($0 >> 8) ^ crcTable[Int(($0 ^ UInt32($1)) & 0xff)] }
    }

    private static func bytes32(_ value: UInt32) -> [UInt8] {
        (0..<4).map { UInt8(truncatingIfNeeded: value >> (8 * UInt32($0))) }
    }

    private static func little32(_ data: [UInt8], _ offset: Int) -> Int {
        (0..<4).reduce(0) { $0 | Int(data[offset + $1]) << (8 * $1) }
    }

    private static func put32(_ value: UInt32, into data: inout [UInt8], at offset: Int) {
        data.replaceSubrange(offset..<(offset + 4), with: bytes32(value))
    }
}
