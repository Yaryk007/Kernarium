// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A frame the guest's vibrator HAL sends to the host.
public enum HapticsFrame: Equatable, Sendable {
    /// Plays a score on its target, replacing what the target plays.
    case play(HapticScore)
    /// Sets the intensity of the tone the target is playing (`IVibrator.setAmplitude`).
    case intensity(target: HapticTarget, sequence: UInt32, intensity: Float)
    /// Stops whatever the target plays (`IVibrator.off`, a cancelled composition).
    case stop(target: HapticTarget, sequence: UInt32)

    public var sequence: UInt32 {
        switch self {
        case .play(let score): score.sequence
        case .intensity(_, let sequence, _): sequence
        case .stop(_, let sequence): sequence
        }
    }

    public var target: HapticTarget {
        switch self {
        case .play(let score): score.target
        case .intensity(let target, _, _): target
        case .stop(let target, _): target
        }
    }
}

/// What the host tells the guest about a frame it received. The guest's HAL
/// keeps it for its diagnostics and never waits for it.
public struct HapticsAcknowledgement: Equatable, Sendable {
    public enum Result: UInt8, Sendable {
        case played = 0
        /// The frame was dropped: the application was not in front.
        case notInFront = 1
        /// The host's haptic engine could not play it.
        case engineFailed = 2
    }

    public var sequence: UInt32
    /// From the frame's receipt to the player's start; 0 when it was dropped.
    public var microseconds: UInt32
    public var result: Result

    public init(sequence: UInt32, microseconds: UInt32, result: Result) {
        self.sequence = sequence
        self.microseconds = microseconds
        self.result = result
    }

    /// The frame as it goes on the wire (17 bytes).
    public var encoded: Data {
        var bytes = HapticsProtocol.header(type: .acknowledgement, length: HapticsProtocol.acknowledgementPayloadSize)
        HapticsProtocol.append(sequence, to: &bytes)
        HapticsProtocol.append(microseconds, to: &bytes)
        bytes.append(result.rawValue)
        return Data(bytes)
    }
}

/// The private haptics protocol on vsock 5557, between the guest's vibrator HAL
/// (which listens) and the host (which connects, as for motion). All fields are
/// little endian. A frame is an 8-byte header, the magic `AHV1`, a `uint16`
/// type and a `uint16` payload length of at most 4096, and the payload:
///
/// - `PLAY` (1), guest to host: `uint8` target, `uint8` flags (0), `uint16`
///   count (1 through 128), `uint32` sequence and `count` events of 20 bytes:
///   `uint8` kind (0 tone, 1 primitive, 2 effect), `uint8` code, `uint8`
///   strength, a reserved 0, `uint32` start and `uint32` duration in
///   milliseconds from the start of the frame, `float32` intensity and
///   `float32` sharpness;
/// - `INTENSITY` (2), guest to host: `uint8` target, three reserved bytes,
///   `uint32` sequence and `float32` intensity;
/// - `STOP` (3), guest to host: `uint8` target, three reserved bytes and
///   `uint32` sequence;
/// - `ACK` (16), host to guest: `uint32` sequence, `uint32` microseconds and
///   `uint8` result (`HapticsAcknowledgement`).
///
/// A frame that breaks the format closes the connection. The golden frames the
/// guest's test shares are in `tests/unit/andarium/fixtures`.
public enum HapticsProtocol {
    public static let port: UInt32 = 5557
    public static let magic: [UInt8] = [0x41, 0x48, 0x56, 0x31]
    public static let headerSize = 8
    public static let maximumPayload = 4096
    public static let maximumEvents = 128
    public static let eventSize = 20
    public static let playPayloadHeaderSize = 8
    public static let intensityPayloadSize = 12
    public static let stopPayloadSize = 8
    public static let acknowledgementPayloadSize = 9

    public enum MessageType: UInt16, Sendable {
        case play = 1
        case intensity = 2
        case stop = 3
        case acknowledgement = 16
    }

    /// Why bytes are not a frame.
    public enum Failure: Error, Equatable, Sendable {
        case badMagic
        /// A type the guest does not send.
        case unknownType(UInt16)
        /// A length the type cannot have, or more than the format allows.
        case badLength(UInt16, Int)
        case badCount(Int)
        /// A flag or reserved byte that is not 0.
        case badReserved
        case badEvent(Int)
        /// An `INTENSITY` outside 0 (exclusive) through 1.
        case badIntensity
        case nonFiniteNumber
    }

    // MARK: Decoding

    /// The type and payload length a frame's first 8 bytes declare.
    public static func parseHeader(_ bytes: some Collection<UInt8>) throws(Failure) -> (type: MessageType, length: Int) {
        let header = Array(bytes.prefix(headerSize))
        precondition(header.count == headerSize, "a header is 8 bytes")
        guard Array(header[0..<4]) == magic else { throw Failure.badMagic }
        let rawType = uint16(header, 4)
        let length = Int(uint16(header, 6))
        guard let type = MessageType(rawValue: rawType), type != .acknowledgement else {
            throw Failure.unknownType(rawType)
        }
        guard length <= maximumPayload else { throw Failure.badLength(rawType, length) }
        switch type {
        case .play:
            let events = (length - playPayloadHeaderSize) / eventSize
            guard length > playPayloadHeaderSize, (length - playPayloadHeaderSize) % eventSize == 0 else {
                throw Failure.badLength(rawType, length)
            }
            guard events <= maximumEvents else { throw Failure.badCount(events) }
        case .intensity:
            guard length == intensityPayloadSize else { throw Failure.badLength(rawType, length) }
        case .stop:
            guard length == stopPayloadSize else { throw Failure.badLength(rawType, length) }
        case .acknowledgement:
            break
        }
        return (type, length)
    }

    /// The frame in the payload a header of `type` declared.
    public static func decodePayload(_ payload: some Collection<UInt8>, type: MessageType) throws(Failure) -> HapticsFrame {
        let bytes = Array(payload)
        switch type {
        case .play:
            guard bytes.count >= playPayloadHeaderSize else { throw Failure.badLength(type.rawValue, bytes.count) }
            guard bytes[1] == 0 else { throw Failure.badReserved }
            let count = Int(uint16(bytes, 2))
            guard (1...maximumEvents).contains(count) else { throw Failure.badCount(count) }
            guard bytes.count == playPayloadHeaderSize + count * eventSize else {
                throw Failure.badLength(type.rawValue, bytes.count)
            }
            var events: [HapticEvent] = []
            events.reserveCapacity(count)
            for index in 0..<count {
                events.append(try event(bytes, at: playPayloadHeaderSize + index * eventSize, index: index))
            }
            return .play(HapticScore(target: HapticTarget(rawValue: bytes[0]), sequence: uint32(bytes, 4), events: events))
        case .intensity:
            guard bytes.count == intensityPayloadSize else { throw Failure.badLength(type.rawValue, bytes.count) }
            guard bytes[1] == 0, bytes[2] == 0, bytes[3] == 0 else { throw Failure.badReserved }
            let intensity = Float(bitPattern: uint32(bytes, 8))
            guard intensity.isFinite else { throw Failure.nonFiniteNumber }
            guard intensity > 0, intensity <= 1 else { throw Failure.badIntensity }
            return .intensity(target: HapticTarget(rawValue: bytes[0]), sequence: uint32(bytes, 4), intensity: intensity)
        case .stop:
            guard bytes.count == stopPayloadSize else { throw Failure.badLength(type.rawValue, bytes.count) }
            guard bytes[1] == 0, bytes[2] == 0, bytes[3] == 0 else { throw Failure.badReserved }
            return .stop(target: HapticTarget(rawValue: bytes[0]), sequence: uint32(bytes, 4))
        case .acknowledgement:
            throw Failure.unknownType(type.rawValue)
        }
    }

    /// The frame in one whole message, header and payload.
    public static func decode(_ message: some Collection<UInt8>) throws(Failure) -> HapticsFrame {
        let bytes = Array(message)
        guard bytes.count >= headerSize else { throw Failure.badLength(0, bytes.count) }
        let header = try parseHeader(bytes)
        guard bytes.count == headerSize + header.length else { throw Failure.badLength(header.type.rawValue, bytes.count) }
        return try decodePayload(bytes[headerSize...], type: header.type)
    }

    private static func event(_ bytes: [UInt8], at offset: Int, index: Int) throws(Failure) -> HapticEvent {
        guard let kind = HapticEvent.Kind(rawValue: bytes[offset]) else { throw Failure.badEvent(index) }
        let code = bytes[offset + 1]
        let strength = bytes[offset + 2]
        guard bytes[offset + 3] == 0 else { throw Failure.badReserved }
        let intensity = Float(bitPattern: uint32(bytes, offset + 12))
        let sharpness = Float(bitPattern: uint32(bytes, offset + 16))
        guard intensity.isFinite, sharpness.isFinite else { throw Failure.nonFiniteNumber }
        switch kind {
        case .tone:
            // A tone has no code or strength, an intensity above 0, and a
            // sharpness; the host plays it for the duration the HAL declared.
            guard code == 0, strength == 0, intensity > 0, intensity <= 1, (0...1).contains(sharpness) else {
                throw Failure.badEvent(index)
            }
        case .primitive:
            guard strength == 0, (0...1).contains(intensity), sharpness == 0 else { throw Failure.badEvent(index) }
        case .effect:
            guard strength <= HapticStrength.strong.rawValue, sharpness == 0 else { throw Failure.badEvent(index) }
        }
        return HapticEvent(
            kind: kind, code: code, strength: strength, startMilliseconds: uint32(bytes, offset + 4),
            durationMilliseconds: uint32(bytes, offset + 8), intensity: intensity, sharpness: sharpness)
    }

    // MARK: Encoding

    static func header(type: MessageType, length: Int) -> [UInt8] {
        var bytes = magic
        bytes.append(contentsOf: [UInt8(type.rawValue & 0xff), UInt8(type.rawValue >> 8)])
        bytes.append(contentsOf: [UInt8(length & 0xff), UInt8(length >> 8)])
        return bytes
    }

    static func append(_ value: UInt32, to bytes: inout [UInt8]) {
        for shift in stride(from: 0, to: 32, by: 8) { bytes.append(UInt8((value >> UInt32(shift)) & 0xff)) }
    }

    private static func uint16(_ bytes: [UInt8], _ offset: Int) -> UInt16 {
        UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
    }

    private static func uint32(_ bytes: [UInt8], _ offset: Int) -> UInt32 {
        (0..<4).reduce(0) { $0 | UInt32(bytes[offset + $1]) << UInt32(8 * $1) }
    }
}

/// The guest's frames out of a byte stream. Bytes arrive in any pieces;
/// whole frames come out in order. A frame that breaks the format ends the
/// stream's use: the owner closes the connection, and the frames that arrived
/// before the bad one still count.
public struct HapticsFrameReader: Sendable {
    public struct Output: Sendable {
        public var frames: [HapticsFrame] = []
        public var failure: HapticsProtocol.Failure?
    }

    private var buffer: [UInt8] = []

    public init() {}

    /// The start of a frame that has not all arrived.
    public var pendingByteCount: Int { buffer.count }

    public mutating func reset() { buffer.removeAll() }

    public mutating func feed(_ bytes: some Collection<UInt8>) -> Output {
        buffer.append(contentsOf: bytes)
        var output = Output()
        while buffer.count >= HapticsProtocol.headerSize {
            do {
                let header = try HapticsProtocol.parseHeader(buffer.prefix(HapticsProtocol.headerSize))
                let total = HapticsProtocol.headerSize + header.length
                guard buffer.count >= total else { return output }
                output.frames.append(
                    try HapticsProtocol.decodePayload(buffer[HapticsProtocol.headerSize..<total], type: header.type))
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
