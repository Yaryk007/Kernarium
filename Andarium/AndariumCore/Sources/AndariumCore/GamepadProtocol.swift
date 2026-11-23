// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the host asks the guest's gamepad relay to do (`GamepadHostFrame`):
/// make a HID device for a controller, destroy it, give it an input report, and
/// answer the kernel driver's requests of it.
public struct GamepadCreate: Equatable, Sendable {
    public var slot: Int
    /// The HID bus: 3 for USB, 5 for Bluetooth.
    public var bus: UInt16
    public var vendor: UInt32
    public var product: UInt32
    public var version: UInt32
    public var country: UInt32
    public var name: String
    public var uniq: String
    public var descriptor: [UInt8]

    public init(
        slot: Int, bus: UInt16, vendor: UInt32, product: UInt32, version: UInt32, country: UInt32 = 0, name: String,
        uniq: String, descriptor: [UInt8]
    ) {
        self.slot = slot
        self.bus = bus
        self.vendor = vendor
        self.product = product
        self.version = version
        self.country = country
        self.name = name
        self.uniq = uniq
        self.descriptor = descriptor
    }
}

/// A frame the host sends to the guest's relay.
public enum GamepadHostFrame: Equatable, Sendable {
    /// Makes the slot's device. The descriptor is 1 through 4096 bytes.
    case create(GamepadCreate)
    case destroy(slot: Int)
    /// An input report of the slot's device, with its report ID.
    case input(slot: Int, report: [UInt8])
    /// The answer to a `GamepadGuestFrame.getReport`: `error` is 0, or an errno.
    case getReportReply(slot: Int, id: UInt32, error: UInt16, data: [UInt8])
    case setReportReply(slot: Int, id: UInt32, error: UInt16)

    public var slot: Int {
        switch self {
        case .create(let create): create.slot
        case .destroy(let slot): slot
        case .input(let slot, _): slot
        case .getReportReply(let slot, _, _, _): slot
        case .setReportReply(let slot, _, _): slot
        }
    }
}

/// What the kernel's HID layer did with a slot's device, as the relay passes it on.
public enum GamepadDeviceEvent: UInt8, Sendable {
    /// The driver started the device.
    case start = 0
    case stop = 1
    /// Something opened the device: the driver, or Android's input reader.
    case open = 2
    case close = 3
}

/// A frame the guest's relay sends to the host.
public enum GamepadGuestFrame: Equatable, Sendable {
    /// Sent once, on accept; the host sends nothing before it.
    case hello(version: UInt16, slotCount: UInt8)
    /// The relay made (or could not make) the slot's device: `result` is 0, or
    /// the negated errno of its failure.
    case created(slot: Int, result: Int32)
    case event(slot: Int, event: GamepadDeviceEvent)
    /// An output report the kernel driver sent (rumble, lights): `reportType`
    /// is uhid's, 1 for an output report.
    case output(slot: Int, reportType: UInt8, data: [UInt8])
    /// A request of the driver for a report of the device: `reportType` 0 is a
    /// feature report.
    case getReport(slot: Int, reportType: UInt8, number: UInt8, id: UInt32)
    case setReport(slot: Int, reportType: UInt8, number: UInt8, id: UInt32, data: [UInt8])

    public var slot: Int? {
        switch self {
        case .hello: nil
        case .created(let slot, _): slot
        case .event(let slot, _): slot
        case .output(let slot, _, _): slot
        case .getReport(let slot, _, _, _): slot
        case .setReport(let slot, _, _, _, _): slot
        }
    }
}

/// The private gamepad protocol on vsock 5558, between the guest's relay
/// (which listens, and creates HID devices through `/dev/uhid`) and the host
/// (which connects, as for motion and haptics). All fields are little endian.
/// A frame is an 8-byte header, the magic `AGP1`, a `uint16` type and a
/// `uint16` payload length of at most 4400, and the payload:
///
/// Host to guest:
/// - `CREATE` (1): `uint8` slot (0 through 7), `uint8` flags (0), `uint16` bus,
///   `uint32` vendor, `uint32` product, `uint32` version, `uint32` country,
///   `char[128]` name, `char[64]` uniq (both NUL padded), `uint16` descriptor
///   length (1 through 4096) and the report descriptor;
/// - `DESTROY` (2): `uint8` slot and three reserved bytes;
/// - `INPUT` (3): `uint8` slot, a reserved byte, `uint16` length (1 through
///   4096) and the report, with its report ID;
/// - `GET_REPORT_REPLY` (4): `uint8` slot, a reserved byte, `uint16` error,
///   `uint32` id, `uint16` length and the data;
/// - `SET_REPORT_REPLY` (5): `uint8` slot, a reserved byte, `uint16` error and
///   `uint32` id.
///
/// Guest to host:
/// - `HELLO` (16): `uint16` protocol version (1), `uint8` slot count (8) and a
///   reserved byte;
/// - `CREATED` (17): `uint8` slot, three reserved bytes and `int32` result;
/// - `EVENT` (18): `uint8` slot, `uint8` kind (`GamepadDeviceEvent`) and two
///   reserved bytes;
/// - `OUTPUT` (19): `uint8` slot, `uint8` report type, `uint16` length and
///   the data;
/// - `GET_REPORT` (20): `uint8` slot, `uint8` report type, `uint8` report
///   number, a reserved byte and `uint32` id;
/// - `SET_REPORT` (21): as `GET_REPORT`, then `uint16` length and the data.
///
/// A header or payload that breaks the format closes the connection. The
/// golden frames the guest's test shares are in `tests/unit/andarium/fixtures`.
public enum GamepadProtocol {
    public static let port: UInt32 = 5558
    public static let magic: [UInt8] = [0x41, 0x47, 0x50, 0x31]
    public static let headerSize = 8
    public static let maximumPayload = 4400
    public static let version: UInt16 = 1
    public static let slotCount = 8
    public static let maximumDescriptor = 4096
    public static let maximumReport = 4096
    public static let nameSize = 128
    public static let uniqSize = 64

    public enum HostType: UInt16, Sendable {
        case create = 1, destroy = 2, input = 3, getReportReply = 4, setReportReply = 5
    }

    public enum GuestType: UInt16, Sendable {
        case hello = 16, created = 17, event = 18, output = 19, getReport = 20, setReport = 21
    }

    /// Why bytes are not a frame.
    public enum Failure: Error, Equatable, Sendable {
        case badMagic
        /// A type the sender does not send.
        case unknownType(UInt16)
        /// A length the type cannot have, or more than the format allows.
        case badLength(UInt16, Int)
        case badSlot(Int)
        case badDescriptor(Int)
        case badKind(UInt8)
    }

    // MARK: Decoding

    /// The type and payload length a frame's first 8 bytes declare, whichever
    /// end sent it. A length over the limit is refused before the type is
    /// looked at, so the header alone decides.
    package static func parseHeader(_ bytes: some Collection<UInt8>) throws(Failure) -> (type: UInt16, length: Int) {
        let header = Array(bytes.prefix(headerSize))
        precondition(header.count == headerSize, "a header is 8 bytes")
        guard Array(header[0..<4]) == magic else { throw Failure.badMagic }
        let type = uint16(header, 4)
        let length = Int(uint16(header, 6))
        guard length <= maximumPayload else { throw Failure.badLength(type, length) }
        return (type, length)
    }

    /// The type and payload length of a guest's frame.
    public static func parseGuestHeader(_ bytes: some Collection<UInt8>) throws(Failure) -> (type: GuestType, length: Int) {
        let header = try parseHeader(bytes)
        guard let type = GuestType(rawValue: header.type) else { throw Failure.unknownType(header.type) }
        switch type {
        case .hello, .event: guard header.length == 4 else { throw Failure.badLength(header.type, header.length) }
        case .created, .getReport: guard header.length == 8 else { throw Failure.badLength(header.type, header.length) }
        case .output: guard header.length >= 4 else { throw Failure.badLength(header.type, header.length) }
        case .setReport: guard header.length >= 10 else { throw Failure.badLength(header.type, header.length) }
        }
        return (type, header.length)
    }

    /// The fixed part of a `CREATE` payload, before the descriptor.
    package static let createFixedSize = 2 + 2 + 4 * 4 + nameSize + uniqSize + 2

    /// The frame in the payload a guest header of `type` declared.
    public static func decodeGuestPayload(_ payload: some Collection<UInt8>, type: GuestType) throws(Failure) -> GamepadGuestFrame {
        let bytes = Array(payload)
        func slot(_ index: Int) throws(Failure) -> Int {
            guard Int(bytes[index]) < slotCount else { throw Failure.badSlot(Int(bytes[index])) }
            return Int(bytes[index])
        }
        switch type {
        case .hello:
            guard bytes.count == 4 else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .hello(version: uint16(bytes, 0), slotCount: bytes[2])
        case .created:
            guard bytes.count == 8 else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .created(slot: try slot(0), result: Int32(bitPattern: uint32(bytes, 4)))
        case .event:
            guard bytes.count == 4 else { throw Failure.badLength(type.rawValue, bytes.count) }
            guard let event = GamepadDeviceEvent(rawValue: bytes[1]) else { throw Failure.badKind(bytes[1]) }
            return .event(slot: try slot(0), event: event)
        case .output:
            guard bytes.count >= 4 else { throw Failure.badLength(type.rawValue, bytes.count) }
            let length = Int(uint16(bytes, 2))
            guard length <= maximumReport, bytes.count == 4 + length else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .output(slot: try slot(0), reportType: bytes[1], data: Array(bytes[4...]))
        case .getReport:
            guard bytes.count == 8 else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .getReport(slot: try slot(0), reportType: bytes[1], number: bytes[2], id: uint32(bytes, 4))
        case .setReport:
            guard bytes.count >= 10 else { throw Failure.badLength(type.rawValue, bytes.count) }
            let length = Int(uint16(bytes, 8))
            guard length <= maximumReport, bytes.count == 10 + length else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .setReport(
                slot: try slot(0), reportType: bytes[1], number: bytes[2], id: uint32(bytes, 4), data: Array(bytes[10...]))
        }
    }

    // MARK: Encoding

    package static func header(type: UInt16, length: Int) -> [UInt8] {
        var bytes = magic
        append(type, to: &bytes)
        append(UInt16(length), to: &bytes)
        return bytes
    }

    /// A C string field of `size` bytes: UTF-8 cut at a character boundary
    /// short of the NUL that ends it, and padded with zeros.
    package static func append(_ text: String, size: Int, to bytes: inout [UInt8]) {
        var field = [UInt8]()
        for character in text {
            let encoded = Array(String(character).utf8)
            guard field.count + encoded.count <= size - 1 else { break }
            field.append(contentsOf: encoded)
        }
        field.append(contentsOf: [UInt8](repeating: 0, count: size - field.count))
        bytes.append(contentsOf: field)
    }

    package static func append(_ value: UInt16, to bytes: inout [UInt8]) {
        bytes.append(UInt8(value & 0xff))
        bytes.append(UInt8(value >> 8))
    }

    package static func append(_ value: UInt32, to bytes: inout [UInt8]) {
        for shift in stride(from: 0, to: 32, by: 8) { bytes.append(UInt8((value >> UInt32(shift)) & 0xff)) }
    }

    package static func uint16(_ bytes: [UInt8], _ offset: Int) -> UInt16 {
        UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
    }

    package static func uint32(_ bytes: [UInt8], _ offset: Int) -> UInt32 {
        (0..<4).reduce(0) { $0 | UInt32(bytes[offset + $1]) << UInt32(8 * $1) }
    }

    /// The text of a NUL padded field.
    package static func string(_ field: ArraySlice<UInt8>) -> String {
        String(decoding: field.prefix { $0 != 0 }, as: UTF8.self)
    }
}

extension GamepadHostFrame {
    /// The frame as it goes on the wire. A name or a uniq too long for its
    /// field is cut short of it.
    public var encoded: [UInt8] {
        var payload = [UInt8]()
        let type: GamepadProtocol.HostType
        switch self {
        case .create(let create):
            type = .create
            payload.append(UInt8(create.slot))
            payload.append(0)
            GamepadProtocol.append(create.bus, to: &payload)
            GamepadProtocol.append(create.vendor, to: &payload)
            GamepadProtocol.append(create.product, to: &payload)
            GamepadProtocol.append(create.version, to: &payload)
            GamepadProtocol.append(create.country, to: &payload)
            GamepadProtocol.append(create.name, size: GamepadProtocol.nameSize, to: &payload)
            GamepadProtocol.append(create.uniq, size: GamepadProtocol.uniqSize, to: &payload)
            GamepadProtocol.append(UInt16(create.descriptor.count), to: &payload)
            payload.append(contentsOf: create.descriptor)
        case .destroy(let slot):
            type = .destroy
            payload = [UInt8(slot), 0, 0, 0]
        case .input(let slot, let report):
            type = .input
            payload = [UInt8(slot), 0]
            GamepadProtocol.append(UInt16(report.count), to: &payload)
            payload.append(contentsOf: report)
        case .getReportReply(let slot, let id, let error, let data):
            type = .getReportReply
            payload = [UInt8(slot), 0]
            GamepadProtocol.append(error, to: &payload)
            GamepadProtocol.append(id, to: &payload)
            GamepadProtocol.append(UInt16(data.count), to: &payload)
            payload.append(contentsOf: data)
        case .setReportReply(let slot, let id, let error):
            type = .setReportReply
            payload = [UInt8(slot), 0]
            GamepadProtocol.append(error, to: &payload)
            GamepadProtocol.append(id, to: &payload)
        }
        return GamepadProtocol.header(type: type.rawValue, length: payload.count) + payload
    }
}

/// The guest's frames out of a byte stream. Bytes arrive in any pieces; whole
/// frames come out in order. A frame that breaks the format ends the stream's
/// use: the owner closes the connection, and the frames that arrived before
/// the bad one still count.
public struct GamepadGuestFrameReader: Sendable {
    public struct Output: Sendable {
        public var frames: [GamepadGuestFrame] = []
        public var failure: GamepadProtocol.Failure?
    }

    private var buffer: [UInt8] = []

    public init() {}

    /// The start of a frame that has not all arrived.
    public var pendingByteCount: Int { buffer.count }

    public mutating func reset() { buffer.removeAll() }

    public mutating func feed(_ bytes: some Collection<UInt8>) -> Output {
        buffer.append(contentsOf: bytes)
        var output = Output()
        while buffer.count >= GamepadProtocol.headerSize {
            do {
                let header = try GamepadProtocol.parseGuestHeader(buffer.prefix(GamepadProtocol.headerSize))
                let total = GamepadProtocol.headerSize + header.length
                guard buffer.count >= total else { return output }
                output.frames.append(
                    try GamepadProtocol.decodeGuestPayload(buffer[GamepadProtocol.headerSize..<total], type: header.type))
                buffer.removeFirst(total)
            } catch {
                output.failure = error
                buffer.removeAll()
                return output
            }
        }
        return output
    }
}
