// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

/// The golden frames of the device-orientation part of the motion wire, which
/// the guest's sub-HAL tests against too (`tests/unit/andarium/fixtures`).
enum MotionOrientationFixture {
    struct Frame {
        var name: String
        var bytes: [UInt8]
    }

    static func frames() throws -> [Frame] {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/motion-orientation-golden-frames.txt")
        let text = try String(contentsOf: url, encoding: .utf8)
        return text.split(separator: "\n").compactMap { line -> Frame? in
            guard !line.hasPrefix("#"), let colon = line.firstIndex(of: ":") else { return nil }
            let bytes = line[line.index(after: colon)...].split(separator: " ").map { UInt8($0, radix: 16)! }
            return Frame(name: String(line[..<colon]), bytes: bytes)
        }
    }

    static func bytes(_ name: String) throws -> [UInt8] {
        try #require(frames().first { $0.name == name }, "the fixture has \(name)").bytes
    }
}
