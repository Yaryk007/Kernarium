// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

// The gamepad wire read from the other end: what the guest's relay does with
// the host's frames, and what it sends. The application writes the host's
// frames and reads the guest's (`GamepadHostFrame.encoded`,
// `GamepadGuestFrameReader`); only a test or a tool that plays the relay needs
// these halves, so they stay out of the application.

extension GamepadProtocol {
    /// The type and payload length of a host's frame.
    package static func parseHostHeader(_ bytes: some Collection<UInt8>) throws(Failure) -> (type: HostType, length: Int) {
        let header = try parseHeader(bytes)
        guard let type = HostType(rawValue: header.type) else { throw Failure.unknownType(header.type) }
        switch type {
        case .create: guard header.length >= createFixedSize + 1 else { throw Failure.badLength(header.type, header.length) }
        case .destroy: guard header.length == 4 else { throw Failure.badLength(header.type, header.length) }
        case .input: guard header.length >= 5 else { throw Failure.badLength(header.type, header.length) }
        case .getReportReply: guard header.length >= 10 else { throw Failure.badLength(header.type, header.length) }
        case .setReportReply: guard header.length == 8 else { throw Failure.badLength(header.type, header.length) }
        }
        return (type, header.length)
    }

    /// The frame in the payload a host header of `type` declared.
    package static func decodeHostPayload(_ payload: some Collection<UInt8>, type: HostType) throws(Failure) -> GamepadHostFrame {
        let bytes = Array(payload)
        func slot(_ index: Int) throws(Failure) -> Int {
            guard Int(bytes[index]) < slotCount else { throw Failure.badSlot(Int(bytes[index])) }
            return Int(bytes[index])
        }
        switch type {
        case .create:
            guard bytes.count >= createFixedSize + 1 else { throw Failure.badLength(type.rawValue, bytes.count) }
            let descriptorOffset = createFixedSize
            let length = Int(uint16(bytes, descriptorOffset - 2))
            guard (1...maximumDescriptor).contains(length) else { throw Failure.badDescriptor(length) }
            guard bytes.count == descriptorOffset + length else { throw Failure.badLength(type.rawValue, bytes.count) }
            let nameOffset = 2 + 2 + 4 * 4
            return .create(
                GamepadCreate(
                    slot: try slot(0), bus: uint16(bytes, 2), vendor: uint32(bytes, 4), product: uint32(bytes, 8),
                    version: uint32(bytes, 12), country: uint32(bytes, 16),
                    name: string(bytes[(nameOffset)..<(nameOffset + nameSize)]),
                    uniq: string(bytes[(nameOffset + nameSize)..<(nameOffset + nameSize + uniqSize)]),
                    descriptor: Array(bytes[descriptorOffset...])))
        case .destroy:
            guard bytes.count == 4 else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .destroy(slot: try slot(0))
        case .input:
            guard bytes.count >= 5 else { throw Failure.badLength(type.rawValue, bytes.count) }
            let length = Int(uint16(bytes, 2))
            guard (1...maximumReport).contains(length), bytes.count == 4 + length else {
                throw Failure.badLength(type.rawValue, bytes.count)
            }
            return .input(slot: try slot(0), report: Array(bytes[4...]))
        case .getReportReply:
            guard bytes.count >= 10 else { throw Failure.badLength(type.rawValue, bytes.count) }
            let length = Int(uint16(bytes, 8))
            guard length <= maximumReport, bytes.count == 10 + length else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .getReportReply(slot: try slot(0), id: uint32(bytes, 4), error: uint16(bytes, 2), data: Array(bytes[10...]))
        case .setReportReply:
            guard bytes.count == 8 else { throw Failure.badLength(type.rawValue, bytes.count) }
            return .setReportReply(slot: try slot(0), id: uint32(bytes, 4), error: uint16(bytes, 2))
        }
    }

    /// The guest's frame in one whole message, header and payload.
    package static func decodeGuest(_ message: some Collection<UInt8>) throws(Failure) -> GamepadGuestFrame {
        let bytes = Array(message)
        guard bytes.count >= headerSize else { throw Failure.badLength(0, bytes.count) }
        let header = try parseGuestHeader(bytes)
        guard bytes.count == headerSize + header.length else { throw Failure.badLength(header.type.rawValue, bytes.count) }
        return try decodeGuestPayload(bytes[headerSize...], type: header.type)
    }

    /// The host's frame in one whole message, header and payload.
    package static func decodeHost(_ message: some Collection<UInt8>) throws(Failure) -> GamepadHostFrame {
        let bytes = Array(message)
        guard bytes.count >= headerSize else { throw Failure.badLength(0, bytes.count) }
        let header = try parseHostHeader(bytes)
        guard bytes.count == headerSize + header.length else { throw Failure.badLength(header.type.rawValue, bytes.count) }
        return try decodeHostPayload(bytes[headerSize...], type: header.type)
    }
}

extension GamepadGuestFrame {
    /// The frame as it goes on the wire; the guest's relay sends these, and
    /// tests and the probe tool play the relay.
    public var encoded: [UInt8] {
        var payload = [UInt8]()
        let type: GamepadProtocol.GuestType
        switch self {
        case .hello(let version, let slotCount):
            type = .hello
            GamepadProtocol.append(version, to: &payload)
            payload.append(slotCount)
            payload.append(0)
        case .created(let slot, let result):
            type = .created
            payload = [UInt8(slot), 0, 0, 0]
            GamepadProtocol.append(UInt32(bitPattern: result), to: &payload)
        case .event(let slot, let event):
            type = .event
            payload = [UInt8(slot), event.rawValue, 0, 0]
        case .output(let slot, let reportType, let data):
            type = .output
            payload = [UInt8(slot), reportType]
            GamepadProtocol.append(UInt16(data.count), to: &payload)
            payload.append(contentsOf: data)
        case .getReport(let slot, let reportType, let number, let id):
            type = .getReport
            payload = [UInt8(slot), reportType, number, 0]
            GamepadProtocol.append(id, to: &payload)
        case .setReport(let slot, let reportType, let number, let id, let data):
            type = .setReport
            payload = [UInt8(slot), reportType, number, 0]
            GamepadProtocol.append(id, to: &payload)
            GamepadProtocol.append(UInt16(data.count), to: &payload)
            payload.append(contentsOf: data)
        }
        return GamepadProtocol.header(type: type.rawValue, length: payload.count) + payload
    }
}

/// The host's frames out of a byte stream, for whatever plays the guest's
/// relay: the same rules as `GamepadGuestFrameReader`.
public struct GamepadHostFrameReader: Sendable {
    public struct Output: Sendable {
        public var frames: [GamepadHostFrame] = []
        public var failure: GamepadProtocol.Failure?
    }

    private var buffer: [UInt8] = []

    public init() {}

    public var pendingByteCount: Int { buffer.count }

    public mutating func reset() { buffer.removeAll() }

    public mutating func feed(_ bytes: some Collection<UInt8>) -> Output {
        buffer.append(contentsOf: bytes)
        var output = Output()
        while buffer.count >= GamepadProtocol.headerSize {
            do {
                let header = try GamepadProtocol.parseHostHeader(buffer.prefix(GamepadProtocol.headerSize))
                let total = GamepadProtocol.headerSize + header.length
                guard buffer.count >= total else { return output }
                output.frames.append(
                    try GamepadProtocol.decodeHostPayload(buffer[GamepadProtocol.headerSize..<total], type: header.type))
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
