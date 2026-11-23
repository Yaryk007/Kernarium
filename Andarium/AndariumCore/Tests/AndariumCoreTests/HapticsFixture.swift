// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The golden frames of the haptics wire, which the guest's vibrator HAL and
/// the host both test against (`tests/unit/andarium/fixtures`).
enum HapticsFixture {
    struct Frame {
        var name: String
        var bytes: [UInt8]
    }

    static func frames() throws -> [Frame] {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/haptics-golden-frames.txt")
        let text = try String(contentsOf: url, encoding: .utf8)
        return text.split(separator: "\n").compactMap { line -> Frame? in
            guard !line.hasPrefix("#"), let colon = line.firstIndex(of: ":") else { return nil }
            let bytes = line[line.index(after: colon)...].split(separator: " ").map { UInt8($0, radix: 16)! }
            return Frame(name: String(line[..<colon]), bytes: bytes)
        }
    }

    static func bytes(_ name: String) throws -> [UInt8] {
        guard let frame = try frames().first(where: { $0.name == name }) else {
            throw Missing(name: name)
        }
        return frame.bytes
    }

    struct Missing: Error { var name: String }
}
