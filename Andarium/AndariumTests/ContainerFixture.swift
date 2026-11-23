// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import zlib

/// One entry of an archive a hosted test writes: stored, with a CRC the test
/// may get wrong on purpose.
nonisolated struct ContainerEntry {
    enum Content {
        case bytes(Data)
        /// This many zero bytes, left as a hole in the file.
        case zeros(UInt64)
    }

    var name: String
    var content: Content
    var crc: UInt32?
    /// ZIP64 fields in both headers whatever the size.
    var forcesZip64 = false

    init(_ name: String, _ data: Data) {
        self.name = name
        content = .bytes(data)
    }

    init(_ name: String, zeros: UInt64) {
        self.name = name
        content = .zeros(zeros)
    }

    var size: UInt64 {
        switch content {
        case let .bytes(data): UInt64(data.count)
        case let .zeros(count): count
        }
    }
}

/// Writes archives of stored entries for the hosted tests of Install APK:
/// the headers a ZIP tool writes (ZIP64 fields only where a field needs
/// them), with entry data that is real bytes or a hole, so that an archive of
/// more than 4 GiB costs no disk.
nonisolated enum ContainerFixture {
    static func write(_ entries: [ContainerEntry], name: String) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-\(name)")
        let descriptor = open(url.path, O_CREAT | O_RDWR | O_EXCL, 0o600)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(descriptor) }
        do {
            try build(entries, into: descriptor)
        } catch {
            try? FileManager.default.removeItem(at: url)
            throw error
        }
        return url
    }

    /// The CRC-32 of `count` zero bytes.
    static func zeroChecksum(_ count: UInt64) -> UInt32 {
        let block = [UInt8](repeating: 0, count: 1 << 20)
        var crc: uLong = 0
        var left = count
        block.withUnsafeBufferPointer { pointer in
            while left > 0 {
                let step = min(left, UInt64(pointer.count))
                crc = zlib.crc32(crc, pointer.baseAddress, uInt(step))
                left -= step
            }
        }
        return UInt32(truncatingIfNeeded: crc)
    }

    static func checksum(_ data: Data) -> UInt32 {
        var bytes = [UInt8](data)
        return UInt32(truncatingIfNeeded: zlib.crc32(0, &bytes, uInt(bytes.count)))
    }

    private static func put(_ data: Data, at offset: UInt64, in descriptor: Int32) throws {
        try data.withUnsafeBytes { raw in
            var done = 0
            while done < raw.count {
                let wrote = pwrite(descriptor, raw.baseAddress! + done, raw.count - done, off_t(offset) + off_t(done))
                guard wrote > 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                done += wrote
            }
        }
    }

    private static func build(_ entries: [ContainerEntry], into descriptor: Int32) throws {
        var offset: UInt64 = 0
        var directory = Data()
        for entry in entries {
            let name = Data(entry.name.utf8)
            let size = entry.size
            let crc: UInt32
            switch entry.content {
            case let .bytes(data): crc = entry.crc ?? checksum(data)
            case let .zeros(count): crc = entry.crc ?? zeroChecksum(count)
            }
            let bigSize = size >= 0xFFFF_FFFF || entry.forcesZip64
            let headerOffset = offset

            var local = Data()
            local.le(UInt32(0x0403_4b50))
            local.le(UInt16(bigSize ? 45 : 20))
            local.le(UInt16(0))
            local.le(UInt16(0))
            local.le(UInt16(0))
            local.le(UInt16(0x21))
            local.le(crc)
            var localExtra = Data()
            if bigSize {
                local.le(UInt32(0xFFFF_FFFF))
                local.le(UInt32(0xFFFF_FFFF))
                localExtra.le(UInt16(0x0001))
                localExtra.le(UInt16(16))
                localExtra.le(size)
                localExtra.le(size)
            } else {
                local.le(UInt32(size))
                local.le(UInt32(size))
            }
            local.le(UInt16(name.count))
            local.le(UInt16(localExtra.count))
            local.append(name)
            local.append(localExtra)
            try put(local, at: offset, in: descriptor)
            offset += UInt64(local.count)
            if case let .bytes(data) = entry.content { try put(data, at: offset, in: descriptor) }
            offset += size

            let bigOffset = headerOffset >= 0xFFFF_FFFF || entry.forcesZip64
            var fields = Data()
            var central = Data()
            central.le(UInt32(0x0201_4b50))
            central.le(UInt16(0x031E))
            central.le(UInt16(bigSize || bigOffset ? 45 : 20))
            central.le(UInt16(0))
            central.le(UInt16(0))
            central.le(UInt16(0))
            central.le(UInt16(0x21))
            central.le(crc)
            if bigSize {
                central.le(UInt32(0xFFFF_FFFF))
                central.le(UInt32(0xFFFF_FFFF))
                fields.le(size)
                fields.le(size)
            } else {
                central.le(UInt32(size))
                central.le(UInt32(size))
            }
            if bigOffset { fields.le(headerOffset) }
            var centralExtra = Data()
            if !fields.isEmpty {
                centralExtra.le(UInt16(0x0001))
                centralExtra.le(UInt16(fields.count))
                centralExtra.append(fields)
            }
            central.le(UInt16(name.count))
            central.le(UInt16(centralExtra.count))
            central.le(UInt16(0))
            central.le(UInt16(0))
            central.le(UInt16(0))
            central.le(UInt32(0o100644) << 16)
            central.le(bigOffset ? UInt32(0xFFFF_FFFF) : UInt32(headerOffset))
            central.append(name)
            central.append(centralExtra)
            directory.append(central)
        }

        let directoryOffset = offset
        try put(directory, at: directoryOffset, in: descriptor)
        offset += UInt64(directory.count)
        var end = Data()
        let big = directoryOffset >= 0xFFFF_FFFF
        if big {
            let record = offset
            end.le(UInt32(0x0606_4b50))
            end.le(UInt64(44))
            end.le(UInt16(45))
            end.le(UInt16(45))
            end.le(UInt32(0))
            end.le(UInt32(0))
            end.le(UInt64(entries.count))
            end.le(UInt64(entries.count))
            end.le(UInt64(directory.count))
            end.le(directoryOffset)
            end.le(UInt32(0x0706_4b50))
            end.le(UInt32(0))
            end.le(record)
            end.le(UInt32(1))
        }
        end.le(UInt32(0x0605_4b50))
        end.le(UInt16(0))
        end.le(UInt16(0))
        end.le(UInt16(entries.count))
        end.le(UInt16(entries.count))
        end.le(UInt32(directory.count))
        end.le(big ? UInt32(0xFFFF_FFFF) : UInt32(directoryOffset))
        end.le(UInt16(0))
        try put(end, at: offset, in: descriptor)
    }
}

extension Data {
    fileprivate mutating func le<Value: FixedWidthInteger>(_ value: Value) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}
