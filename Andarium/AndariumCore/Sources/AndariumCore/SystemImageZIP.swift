// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The system image described by a ZIP central directory. Only this member is
/// extracted, to a path chosen by Andarium.
public struct SystemImageZIP: Sendable {
    public let fileName: String
    public let offset: UInt64
    public let compressedBytes: UInt64
    public let imageBytes: UInt64
    public let compression: UInt64
    public let crc: UInt32

    public static func read(_ archive: URL) throws -> Self {
        let input = try FileHandle(forReadingFrom: archive)
        defer { try? input.close() }
        let size = try input.seekToEnd()
        func read(_ offset: UInt64, _ count: Int) throws -> Data {
            guard offset <= size, UInt64(count) <= size - offset else { throw SkinImageError.archive }
            try input.seek(toOffset: offset)
            guard let data = try input.read(upToCount: count), data.count == count else { throw SkinImageError.archive }
            return data
        }

        let tailSize = Int(min(size, 22 + 65535))
        let tail = try read(size - UInt64(tailSize), tailSize)
        guard tailSize >= 22, let end = stride(from: tailSize - 22, through: 0, by: -1).first(where: {
            tail.le($0, 4) == 0x06054b50 && $0 + 22 + Int(tail.le($0 + 20, 2)) == tailSize
        }), tail.le(end + 4, 4) == 0 else { throw SkinImageError.archive }
        let endOffset = size - UInt64(tailSize) + UInt64(end)
        var count = tail.le(end + 10, 2)
        guard tail.le(end + 8, 2) == count else { throw SkinImageError.archive }
        var directoryBytes = tail.le(end + 12, 4)
        var directory = tail.le(end + 16, 4)
        if count == 0xffff || directoryBytes == 0xffffffff || directory == 0xffffffff {
            guard endOffset >= 20 else { throw SkinImageError.archive }
            let locator = try read(endOffset - 20, 20)
            guard locator.le(0, 4) == 0x07064b50, locator.le(4, 4) == 0, locator.le(16, 4) == 1
            else { throw SkinImageError.archive }
            let zip64 = try read(locator.le(8, 8), 56)
            guard zip64.le(0, 4) == 0x06064b50, zip64.le(4, 8) >= 44,
                  zip64.le(16, 8) == 0, zip64.le(24, 8) == zip64.le(32, 8)
            else { throw SkinImageError.archive }
            count = zip64.le(32, 8)
            directoryBytes = zip64.le(40, 8)
            directory = zip64.le(48, 8)
        }
        guard directory <= endOffset, directoryBytes <= endOffset - directory,
              count <= directoryBytes / 46 else { throw SkinImageError.archive }
        let directoryEnd = directory + directoryBytes
        var cursor = directory
        var match: Self?
        for _ in 0..<count {
            guard directoryEnd - cursor >= 46 else { throw SkinImageError.archive }
            let header = try read(cursor, 46)
            guard header.le(0, 4) == 0x02014b50 else { throw SkinImageError.archive }
            let nameLength = Int(header.le(28, 2))
            let extraLength = Int(header.le(30, 2))
            let recordLength = UInt64(46 + nameLength + extraLength + Int(header.le(32, 2)))
            guard recordLength <= directoryEnd - cursor else { throw SkinImageError.archive }
            let fields = try read(cursor + 46, nameLength + extraLength)
            cursor += recordLength
            let path = fields.prefix(nameLength)
            let name = String(decoding: path, as: UTF8.self).components(separatedBy: "/").last ?? ""
            guard ["system.img", "system.img.xz", "system.img.gz"].contains(name.lowercased()) else { continue }
            guard match == nil else { throw SkinImageError.missingSystem }

            let method = header.le(10, 2)
            let flags = header.le(8, 2)
            let mode = header.le(38, 4) >> 16 & 0xf000
            guard flags & 0x2041 == 0, method == 0 || method == 8,
                  (mode == 0 || mode == 0x8000), header.le(34, 2) == 0 else { throw SkinImageError.archive }
            let sizes = try zip64Values([header.le(24, 4), header.le(20, 4), header.le(42, 4)],
                                        in: fields, from: nameLength)
            let expanded = sizes[0], packed = sizes[1], localOffset = sizes[2]
            guard expanded > 0, expanded <= UInt64(Int64.max), packed > 0,
                  method != 0 || expanded == packed, localOffset <= directory,
                  directory - localOffset >= 30 else { throw SkinImageError.archive }
            let local = try read(localOffset, 30)
            guard local.le(0, 4) == 0x04034b50, local.le(6, 2) == flags,
                  local.le(8, 2) == method, local.le(26, 2) == UInt64(nameLength)
            else { throw SkinImageError.archive }
            let offset = localOffset + 30 + UInt64(nameLength) + local.le(28, 2)
            guard offset <= directory, packed <= directory - offset,
                  try read(localOffset + 30, nameLength) == path else { throw SkinImageError.archive }
            match = Self(fileName: name.lowercased(), offset: offset, compressedBytes: packed,
                         imageBytes: expanded, compression: method, crc: UInt32(header.le(16, 4)))
        }
        guard let match else { throw SkinImageError.missingSystem }
        return match
    }

    private static func zip64Values(_ fields: [UInt64], in data: Data, from start: Int) throws -> [UInt64] {
        guard fields.contains(0xffffffff) else { return fields }
        var cursor = start
        while cursor + 4 <= data.count {
            let field = data.le(cursor, 2)
            let end = cursor + 4 + Int(data.le(cursor + 2, 2))
            cursor += 4
            guard end <= data.count else { break }
            if field == 1 {
                return try fields.map { value in
                    guard value == 0xffffffff else { return value }
                    guard end - cursor >= 8 else { throw SkinImageError.archive }
                    defer { cursor += 8 }
                    return data.le(cursor, 8)
                }
            }
            cursor = end
        }
        throw SkinImageError.archive
    }
}

private extension Data {
    func le(_ offset: Int, _ bytes: Int) -> UInt64 {
        (0..<bytes).reduce(0) { $0 | UInt64(self[startIndex + offset + $1]) << ($1 * 8) }
    }
}
