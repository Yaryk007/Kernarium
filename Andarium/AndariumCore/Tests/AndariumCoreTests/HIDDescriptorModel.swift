// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A small reader of HID report descriptors, for the tests: it keeps what a
/// descriptor declares (each report's fields, their usages, bit positions and
/// ranges) so a test can read a value out of a report the way the guest's
/// kernel does, independently of the code that wrote the report and the
/// descriptor. It handles what these descriptors use: short items, one usage
/// per field or a list or a range, and constants as padding.
struct HIDDescriptorModel {
    enum Kind: Hashable { case input, output, feature }

    struct Field {
        var kind: Kind
        var reportID: Int
        var usagePage: Int
        /// One usage for each of the field's `count` elements; nil for padding.
        var usages: [Int?]
        /// Where the field starts, in bits from the start of the report's data
        /// (after its ID).
        var bitOffset: Int
        var bitSize: Int
        var count: Int
        var logicalMinimum: Int
        var logicalMaximum: Int
        var flags: Int

        var isConstant: Bool { flags & 1 != 0 }
        var hasNullState: Bool { flags & 0x40 != 0 }
    }

    private(set) var fields: [Field] = []
    private var bits: [Key: Int] = [:]
    private(set) var collections = 0

    private struct Key: Hashable {
        var kind: Kind
        var reportID: Int
    }

    struct Invalid: Error { var reason: String }

    init(_ descriptor: [UInt8]) throws {
        var usagePage = 0
        var logicalMinimum = 0
        var logicalMaximum = 0
        var reportSize = 0
        var reportCount = 0
        var reportID = 0
        var usages: [Int] = []
        var usageMinimum: Int?
        var usageMaximum: Int?
        var depth = 0
        var index = 0
        while index < descriptor.count {
            let prefix = descriptor[index]
            let sizeCode = Int(prefix & 3)
            let size = sizeCode == 3 ? 4 : sizeCode
            guard index + 1 + size <= descriptor.count else { throw Invalid(reason: "an item runs past the end") }
            let raw = (0..<size).reduce(0) { $0 | Int(descriptor[index + 1 + $1]) << (8 * $1) }
            // Signed values sign extend from the item's own size.
            let signed = size == 0 ? 0 : (raw << (64 - 8 * size)) >> (64 - 8 * size)
            let type = (prefix >> 2) & 3
            let tag = prefix >> 4
            index += 1 + size
            switch (type, tag) {
            case (0, 0x8), (0, 0x9), (0, 0xb):
                let kind: Kind = tag == 0x8 ? .input : tag == 0x9 ? .output : .feature
                let key = Key(kind: kind, reportID: reportID)
                let offset = bits[key] ?? 0
                var elementUsages: [Int?] = []
                if raw & 1 == 0 {
                    var list = usages
                    if let low = usageMinimum, let high = usageMaximum { list = Array(low...high) }
                    guard !list.isEmpty else { throw Invalid(reason: "a field has no usage") }
                    while list.count < reportCount { list.append(list[list.count - 1]) }
                    elementUsages = list.prefix(reportCount).map { Optional($0) }
                } else {
                    elementUsages = Array(repeating: nil, count: reportCount)
                }
                fields.append(
                    Field(
                        kind: kind, reportID: reportID, usagePage: usagePage, usages: elementUsages, bitOffset: offset,
                        bitSize: reportSize, count: reportCount, logicalMinimum: logicalMinimum,
                        logicalMaximum: logicalMaximum, flags: raw))
                bits[key] = offset + reportSize * reportCount
                usages = []
                usageMinimum = nil
                usageMaximum = nil
            case (0, 0xa):
                depth += 1
                collections += 1
                usages = []
            case (0, 0xc):
                depth -= 1
                usages = []
            case (1, 0x0): usagePage = raw
            case (1, 0x1): logicalMinimum = signed
            case (1, 0x2): logicalMaximum = signed
            case (1, 0x7): reportSize = raw
            case (1, 0x8): reportID = raw
            case (1, 0x9): reportCount = raw
            case (1, 0x3), (1, 0x4), (1, 0x5), (1, 0x6): break
            case (2, 0x0): usages.append(raw)
            case (2, 0x1): usageMinimum = raw
            case (2, 0x2): usageMaximum = raw
            default: throw Invalid(reason: "an item of type \(type) and tag \(tag) is not handled")
            }
        }
        guard depth == 0 else { throw Invalid(reason: "a collection is not closed") }
    }

    /// The bytes of a report with its ID (a descriptor with report IDs puts
    /// the ID first), or nil for a report the descriptor does not have.
    func reportSize(_ kind: Kind, id: Int) -> Int? {
        guard let total = bits[Key(kind: kind, reportID: id)] else { return nil }
        return 1 + (total + 7) / 8
    }

    /// The field that carries `usage` on `page`, with the element's index.
    func field(page: Int, usage: Int, kind: Kind = .input, reportID: Int = 1) -> (field: Field, element: Int)? {
        for field in fields where field.kind == kind && field.reportID == reportID && field.usagePage == page {
            if let element = field.usages.firstIndex(of: usage) { return (field, element) }
        }
        return nil
    }

    /// The value of an element in a report (with its ID first), as the kernel
    /// reads it: little endian bits, signed when the range goes below 0.
    func value(page: Int, usage: Int, in report: [UInt8], kind: Kind = .input, reportID: Int = 1) -> Int? {
        guard let (field, element) = field(page: page, usage: usage, kind: kind, reportID: reportID) else { return nil }
        return read(field, element: element, in: report)
    }

    func read(_ field: Field, element: Int, in report: [UInt8]) -> Int {
        let start = 8 + field.bitOffset + element * field.bitSize
        var value = 0
        for bit in 0..<field.bitSize {
            let position = start + bit
            guard position / 8 < report.count else { break }
            if report[position / 8] >> UInt8(position % 8) & 1 == 1 { value |= 1 << bit }
        }
        if field.logicalMinimum < 0, value >> (field.bitSize - 1) & 1 == 1 { value -= 1 << field.bitSize }
        return value
    }
}
