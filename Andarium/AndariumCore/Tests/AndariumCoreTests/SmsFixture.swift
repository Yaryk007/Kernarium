// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The golden bytes of the text message channel and its payload, which the
/// guest's service and the host both test against
/// (`tests/unit/andarium/fixtures/sms-golden-frames.txt` says what each line is).
enum SmsFixture {
    struct Vector {
        var name: String
        /// The words before `=>` (or none): the inputs the bytes are made from.
        var fields: [String]
        var bytes: Data
    }

    static func vectors() throws -> [Vector] {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/sms-golden-frames.txt")
        let text = try String(contentsOf: url, encoding: .utf8)
        return text.split(separator: "\n").compactMap { line -> Vector? in
            guard !line.hasPrefix("#"), let colon = line.firstIndex(of: ":") else { return nil }
            let rest = line[line.index(after: colon)...]
            let (before, after): (Substring, Substring)
            if let arrow = rest.range(of: "=>") {
                (before, after) = (rest[..<arrow.lowerBound], rest[arrow.upperBound...])
            } else {
                (before, after) = ("", rest)
            }
            let hex = after.filter { !$0.isWhitespace }
            var bytes = Data()
            var index = hex.startIndex
            while index < hex.endIndex {
                let next = hex.index(index, offsetBy: 2)
                bytes.append(UInt8(hex[index..<next], radix: 16)!)
                index = next
            }
            return Vector(
                name: String(line[..<colon]), fields: before.split(separator: " ").map(String.init), bytes: bytes)
        }
    }

    static func vectors(prefix: String) throws -> [Vector] {
        try vectors().filter { $0.name.hasPrefix(prefix) }
    }

    static func vector(_ name: String) throws -> Vector {
        guard let found = try vectors().first(where: { $0.name == name }) else { throw Missing(name: name) }
        return found
    }

    struct Missing: Error { var name: String }

    /// The bytes a field of hex digits stands for; "-" is none.
    static func bytes(ofHex field: String) -> Data {
        guard field != "-" else { return Data() }
        var data = Data()
        var index = field.startIndex
        while index < field.endIndex {
            let next = field.index(index, offsetBy: 2)
            data.append(UInt8(field[index..<next], radix: 16)!)
            index = next
        }
        return data
    }
}
