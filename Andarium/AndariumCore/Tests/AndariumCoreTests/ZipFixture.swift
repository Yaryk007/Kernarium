// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import zlib

@testable import AndariumCore

/// Writes ZIP archives for the tests: stored and deflated entries, the data
/// descriptor form, ZIP64 records, and, for the hostile cases, directory
/// fields that disagree with the data.
struct ZipFixture {
    struct Item {
        var name: String
        var data: Data
        /// 0 stored, 8 deflate; anything else is written as the stored bytes
        /// under that method number.
        var method: UInt16 = 8
        var flags: UInt16 = 0
        /// The sizes follow the data (bit 3) and the local header holds zeros.
        var descriptor = false
        /// ZIP64 extra fields in both headers, whatever the sizes.
        var zip64 = false
        var crc: UInt32?
        var declaredSize: UInt64?
        var declaredCompressedSize: UInt64?
        var headerOffset: UInt64?
        var localName: String?
        var rawName: Data?
        /// The bytes written as the entry's data, in place of the data stored or deflated.
        var storedBytes: Data?

        init(_ name: String, _ data: Data = Data(), method: UInt16 = 8) {
            self.name = name
            self.data = data
            self.method = method
        }
    }

    var items: [Item]
    var zip64End = false
    var comment = Data()
    var diskNumber: UInt16 = 0
    var declaredEntryCount: UInt16?
    var directoryOffsetShift: Int64 = 0

    init(_ items: [Item]) { self.items = items }

    static func deflate(_ data: Data) -> Data {
        var stream = z_stream()
        deflateInit2_(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size))
        defer { deflateEnd(&stream) }
        var out = Data()
        var buffer = [UInt8](repeating: 0, count: 64 * 1024)
        var input = [UInt8](data)
        input.withUnsafeMutableBufferPointer { source in
            stream.next_in = source.baseAddress
            stream.avail_in = UInt32(source.count)
            repeat {
                buffer.withUnsafeMutableBufferPointer { sink in
                    stream.next_out = sink.baseAddress
                    stream.avail_out = UInt32(sink.count)
                    _ = zlib.deflate(&stream, Z_FINISH)
                    out.append(sink.baseAddress!, count: sink.count - Int(stream.avail_out))
                }
            } while stream.avail_out == 0
        }
        return out
    }

    static func checksum(_ data: Data) -> UInt32 {
        var bytes = [UInt8](data)
        return UInt32(truncatingIfNeeded: zlib.crc32(0, &bytes, UInt32(bytes.count)))
    }

    func build() -> Data {
        var file = Data()
        var directory = Data()
        for item in items {
            let stored: Data
            if let bytes = item.storedBytes {
                stored = bytes
            } else if item.method == 8 {
                stored = Self.deflate(item.data)
            } else {
                stored = item.data
            }
            let crc = item.crc ?? Self.checksum(item.data)
            let size = item.declaredSize ?? UInt64(item.data.count)
            let compressed = item.declaredCompressedSize ?? UInt64(stored.count)
            let offset = item.headerOffset ?? UInt64(file.count)
            let name = item.rawName ?? Data(item.name.utf8)
            let localName = item.localName.map { Data($0.utf8) } ?? name
            let big = item.zip64
            var local = Data()
            local.le(UInt32(0x0403_4b50))
            local.le(UInt16(big ? 45 : 20))
            local.le(item.flags | (item.descriptor ? 0x0008 : 0))
            local.le(item.method)
            local.le(UInt16(0))
            local.le(UInt16(0x21))
            local.le(item.descriptor ? 0 : crc)
            var localExtra = Data()
            if big {
                localExtra.le(UInt16(0x0001))
                localExtra.le(UInt16(16))
                localExtra.le(item.descriptor ? 0 : size)
                localExtra.le(item.descriptor ? 0 : compressed)
                local.le(UInt32(0xFFFF_FFFF))
                local.le(UInt32(0xFFFF_FFFF))
            } else {
                local.le(item.descriptor ? 0 : UInt32(truncatingIfNeeded: compressed))
                local.le(item.descriptor ? 0 : UInt32(truncatingIfNeeded: size))
            }
            local.le(UInt16(localName.count))
            local.le(UInt16(localExtra.count))
            local.append(localName)
            local.append(localExtra)
            file.append(local)
            file.append(stored)
            if item.descriptor {
                file.le(UInt32(0x0807_4b50))
                file.le(crc)
                if big {
                    file.le(compressed)
                    file.le(size)
                } else {
                    file.le(UInt32(truncatingIfNeeded: compressed))
                    file.le(UInt32(truncatingIfNeeded: size))
                }
            }
            var central = Data()
            central.le(UInt32(0x0201_4b50))
            central.le(UInt16(0x031E))
            central.le(UInt16(big ? 45 : 20))
            central.le(item.flags | (item.descriptor ? 0x0008 : 0))
            central.le(item.method)
            central.le(UInt16(0))
            central.le(UInt16(0x21))
            central.le(crc)
            var centralExtra = Data()
            if big {
                central.le(UInt32(0xFFFF_FFFF))
                central.le(UInt32(0xFFFF_FFFF))
                centralExtra.le(UInt16(0x0001))
                centralExtra.le(UInt16(24))
                centralExtra.le(size)
                centralExtra.le(compressed)
                centralExtra.le(offset)
            } else {
                central.le(UInt32(truncatingIfNeeded: compressed))
                central.le(UInt32(truncatingIfNeeded: size))
            }
            central.le(UInt16(name.count))
            central.le(UInt16(centralExtra.count))
            central.le(UInt16(0))
            central.le(UInt16(0))
            central.le(UInt16(0))
            central.le(UInt32(0o100644) << 16)
            central.le(big ? UInt32(0xFFFF_FFFF) : UInt32(truncatingIfNeeded: offset))
            central.append(name)
            central.append(centralExtra)
            directory.append(central)
        }
        let directoryOffset = UInt64(Int64(file.count) + directoryOffsetShift)
        file.append(directory)
        let count = UInt16(truncatingIfNeeded: declaredEntryCount ?? UInt16(items.count))
        if zip64End {
            let recordOffset = UInt64(file.count)
            file.le(UInt32(0x0606_4b50))
            file.le(UInt64(44))
            file.le(UInt16(45))
            file.le(UInt16(45))
            file.le(UInt32(0))
            file.le(UInt32(0))
            file.le(UInt64(count))
            file.le(UInt64(count))
            file.le(UInt64(directory.count))
            file.le(directoryOffset)
            file.le(UInt32(0x0706_4b50))
            file.le(UInt32(0))
            file.le(recordOffset)
            file.le(UInt32(1))
        }
        file.le(UInt32(0x0605_4b50))
        file.le(diskNumber)
        file.le(UInt16(0))
        file.le(zip64End ? UInt16(0xFFFF) : count)
        file.le(zip64End ? UInt16(0xFFFF) : count)
        file.le(zip64End ? UInt32(0xFFFF_FFFF) : UInt32(directory.count))
        file.le(zip64End ? UInt32(0xFFFF_FFFF) : UInt32(truncatingIfNeeded: directoryOffset))
        file.le(UInt16(comment.count))
        file.append(comment)
        return file
    }
}

extension Data {
    mutating func le<Value: FixedWidthInteger>(_ value: Value) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}

/// Bytes that do not compress: a fixed pseudo-random sequence.
func incompressible(_ count: Int, seed: UInt64 = 0x9E37_79B9_7F4A_7C15) -> Data {
    var state = seed
    var bytes = [UInt8](repeating: 0, count: count)
    for index in 0..<count {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        bytes[index] = UInt8(truncatingIfNeeded: state >> 33)
    }
    return Data(bytes)
}
