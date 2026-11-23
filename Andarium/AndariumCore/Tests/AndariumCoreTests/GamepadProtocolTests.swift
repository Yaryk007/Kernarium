// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// The gamepad wire format against the golden frames in
/// `tests/unit/andarium/fixtures/gamepad-golden-frames.txt`, which the guest's
/// relay is tested against too: what each frame means, that what is encoded
/// decodes to itself, and every way a frame can break the format.
struct GamepadProtocolTests {
    // MARK: The golden frames

    @Test func destroyEncodesToTheGoldenFrame() throws {
        #expect(GamepadHostFrame.destroy(slot: 1).encoded == (try GamepadFixture.bytes("destroy-slot-1")))
    }

    @Test func inputEncodesToTheGoldenFrame() throws {
        #expect(
            GamepadHostFrame.input(slot: 0, report: [0x01, 0x80, 0x80]).encoded
                == (try GamepadFixture.bytes("input-slot-0")))
    }

    @Test func helloDecodesFromTheGoldenFrame() throws {
        #expect(try GamepadProtocol.decodeGuest(GamepadFixture.bytes("hello")) == .hello(version: 1, slotCount: 8))
    }

    @Test func theGoldenHostFramesDecodeToWhatEncodedThem() throws {
        #expect(try GamepadProtocol.decodeHost(GamepadFixture.bytes("destroy-slot-1")) == .destroy(slot: 1))
        #expect(
            try GamepadProtocol.decodeHost(GamepadFixture.bytes("input-slot-0"))
                == .input(slot: 0, report: [0x01, 0x80, 0x80]))
    }

    @Test func everyRejectFrameIsRefusedAtItsHeaderOrPayload() throws {
        for name in ["reject-magic", "reject-type", "reject-length"] {
            let bytes = try GamepadFixture.bytes(name)
            #expect(throws: GamepadProtocol.Failure.self, "\(name) is no frame") {
                _ = try GamepadProtocol.parseGuestHeader(bytes)
            }
            #expect(throws: GamepadProtocol.Failure.self, "\(name) is no frame") {
                _ = try GamepadProtocol.parseHostHeader(bytes)
            }
        }
    }

    @Test func theRejectFramesFailForTheirOwnReason() throws {
        #expect(throws: GamepadProtocol.Failure.badMagic) {
            _ = try GamepadProtocol.parseGuestHeader(GamepadFixture.bytes("reject-magic"))
        }
        #expect(throws: GamepadProtocol.Failure.unknownType(255)) {
            _ = try GamepadProtocol.parseGuestHeader(GamepadFixture.bytes("reject-type"))
        }
        // 0x1131 is 4401: over the limit whatever the type is.
        #expect(throws: GamepadProtocol.Failure.badLength(3, 4401)) {
            _ = try GamepadProtocol.parseHostHeader(GamepadFixture.bytes("reject-length"))
        }
    }

    // MARK: Host frames

    private func create(slot: Int = 3, descriptor: [UInt8] = [0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0xc0]) -> GamepadCreate {
        GamepadCreate(
            slot: slot, bus: 3, vendor: 0x054c, product: 0x0ce6, version: 0x8100, country: 0, name: "Wireless Controller",
            uniq: "kernarium-gamepad-3", descriptor: descriptor)
    }

    @Test func createRoundTripsAndHasTheDocumentedLayout() throws {
        let frame = GamepadHostFrame.create(create())
        let bytes = frame.encoded
        #expect(Array(bytes[0..<4]) == [0x41, 0x47, 0x50, 0x31])
        #expect(Array(bytes[4..<6]) == [1, 0])
        #expect(Int(bytes[6]) | Int(bytes[7]) << 8 == GamepadProtocol.createFixedSize + 7)
        // The slot, a reserved byte, then the bus, vendor, product, version, country.
        #expect(bytes[8] == 3)
        #expect(Array(bytes[10..<12]) == [3, 0])
        #expect(Array(bytes[12..<16]) == [0x4c, 0x05, 0, 0])
        #expect(Array(bytes[16..<20]) == [0xe6, 0x0c, 0, 0])
        #expect(Array(bytes[20..<24]) == [0x00, 0x81, 0, 0])
        #expect(try GamepadProtocol.decodeHost(bytes) == frame)
    }

    @Test func aLongNameIsCutAtACharacterBoundaryShortOfTheNul() throws {
        // 127 bytes fit, and a 3 byte character that would straddle the limit goes whole or not at all.
        let name = String(repeating: "a", count: 125) + "\u{3042}"
        var frame = create()
        frame.name = name
        let decoded = try GamepadProtocol.decodeHost(GamepadHostFrame.create(frame).encoded)
        guard case .create(let made) = decoded else { Issue.record("not a create"); return }
        #expect(made.name == String(repeating: "a", count: 125))
        #expect(made.name.utf8.count <= GamepadProtocol.nameSize - 1)
    }

    @Test func aDescriptorOfTheLimitIsAcceptedAndOneOverIsNot() throws {
        let limit = create(descriptor: [UInt8](repeating: 0x05, count: GamepadProtocol.maximumDescriptor))
        #expect(try GamepadProtocol.decodeHost(GamepadHostFrame.create(limit).encoded) == .create(limit))
        // A payload of the fixed part and a zero length descriptor.
        var bytes = GamepadHostFrame.create(create(descriptor: [0x05])).encoded
        bytes[8 + GamepadProtocol.createFixedSize - 2] = 0
        bytes[8 + GamepadProtocol.createFixedSize - 1] = 0
        #expect(throws: GamepadProtocol.Failure.self) { _ = try GamepadProtocol.decodeHost(bytes) }
    }

    @Test func theRepliesToTheDriverRoundTrip() throws {
        let get = GamepadHostFrame.getReportReply(slot: 2, id: 0xdead_beef, error: 0, data: [0x05, 1, 2, 3])
        let set = GamepadHostFrame.setReportReply(slot: 7, id: 41, error: GamepadErrno.invalid)
        #expect(try GamepadProtocol.decodeHost(get.encoded) == get)
        #expect(try GamepadProtocol.decodeHost(set.encoded) == set)
        // A reply that fails carries no data.
        let failed = GamepadHostFrame.getReportReply(slot: 2, id: 9, error: GamepadErrno.invalid, data: [])
        #expect(try GamepadProtocol.decodeHost(failed.encoded) == failed)
    }

    @Test func aHostFrameHasItsSlot() {
        #expect(GamepadHostFrame.destroy(slot: 5).slot == 5)
        #expect(GamepadHostFrame.input(slot: 6, report: [1]).slot == 6)
        #expect(GamepadHostFrame.setReportReply(slot: 4, id: 1, error: 0).slot == 4)
    }

    // MARK: Guest frames

    @Test func everyGuestFrameRoundTrips() throws {
        let frames: [GamepadGuestFrame] = [
            .hello(version: 1, slotCount: 8),
            .created(slot: 2, result: 0),
            .created(slot: 2, result: -22),
            .event(slot: 0, event: .start),
            .event(slot: 7, event: .stop),
            .event(slot: 1, event: .open),
            .event(slot: 1, event: .close),
            .output(slot: 3, reportType: GamepadReportType.output, data: [0x02, 0xff, 0x00]),
            .getReport(slot: 4, reportType: GamepadReportType.feature, number: 0x05, id: 0x0102_0304),
            .setReport(slot: 5, reportType: GamepadReportType.feature, number: 0x08, id: 77, data: [8, 1, 2]),
        ]
        for frame in frames {
            #expect(try GamepadProtocol.decodeGuest(frame.encoded) == frame)
        }
        #expect(GamepadGuestFrame.hello(version: 1, slotCount: 8).slot == nil)
        #expect(GamepadGuestFrame.created(slot: 2, result: 0).slot == 2)
        #expect(GamepadGuestFrame.setReport(slot: 5, reportType: 0, number: 8, id: 1, data: []).slot == 5)
    }

    @Test func aGuestFrameWithASlotBeyondEightIsRefused() {
        var bytes = GamepadGuestFrame.event(slot: 0, event: .start).encoded
        bytes[8] = 8
        #expect(throws: GamepadProtocol.Failure.badSlot(8)) { _ = try GamepadProtocol.decodeGuest(bytes) }
    }

    @Test func anUnknownEventKindIsRefused() {
        var bytes = GamepadGuestFrame.event(slot: 0, event: .start).encoded
        bytes[9] = 9
        #expect(throws: GamepadProtocol.Failure.badKind(9)) { _ = try GamepadProtocol.decodeGuest(bytes) }
    }

    @Test func aFrameOfTheWrongLengthForItsTypeIsRefused() {
        // HELLO is 4 bytes, CREATED 8.
        var hello = GamepadGuestFrame.hello(version: 1, slotCount: 8).encoded
        hello[6] = 5
        hello.append(0)
        #expect(throws: GamepadProtocol.Failure.badLength(16, 5)) { _ = try GamepadProtocol.parseGuestHeader(hello) }
        var created = GamepadGuestFrame.created(slot: 0, result: 0).encoded
        created[6] = 7
        created.removeLast()
        #expect(throws: GamepadProtocol.Failure.badLength(17, 7)) { _ = try GamepadProtocol.parseGuestHeader(created) }
    }

    @Test func anOutputReportWhoseLengthDisagreesWithItsPayloadIsRefused() {
        var bytes = GamepadGuestFrame.output(slot: 0, reportType: 1, data: [1, 2, 3]).encoded
        bytes[8 + 2] = 4  // The report length field says 4 where 3 follow.
        #expect(throws: GamepadProtocol.Failure.self) { _ = try GamepadProtocol.decodeGuest(bytes) }
    }

    @Test func aHostFrameIsNotAGuestFrameAndTheOtherWayRound() {
        #expect(throws: GamepadProtocol.Failure.unknownType(2)) {
            _ = try GamepadProtocol.parseGuestHeader(GamepadHostFrame.destroy(slot: 0).encoded)
        }
        #expect(throws: GamepadProtocol.Failure.unknownType(16)) {
            _ = try GamepadProtocol.parseHostHeader(GamepadGuestFrame.hello(version: 1, slotCount: 8).encoded)
        }
    }

    // MARK: Reading a stream

    @Test func aReaderAssemblesFramesFromBytesOfAnySize() throws {
        let frames: [GamepadGuestFrame] = [
            .hello(version: 1, slotCount: 8), .created(slot: 1, result: 0),
            .output(slot: 1, reportType: 1, data: Array(repeating: 7, count: 63)),
            .event(slot: 1, event: .open),
        ]
        let stream = frames.flatMap(\.encoded)
        for chunk in [1, 2, 7, 8, 13, 1000] {
            var reader = GamepadGuestFrameReader()
            var got: [GamepadGuestFrame] = []
            var index = 0
            while index < stream.count {
                let output = reader.feed(stream[index..<min(index + chunk, stream.count)])
                #expect(output.failure == nil)
                got += output.frames
                index += chunk
            }
            #expect(got == frames, "chunks of \(chunk)")
            #expect(reader.pendingByteCount == 0)
        }
    }

    @Test func aReaderKeepsTheFramesBeforeABadOneAndThenFails() {
        var bytes = GamepadGuestFrame.hello(version: 1, slotCount: 8).encoded
        bytes += GamepadGuestFrame.event(slot: 0, event: .start).encoded
        bytes += [0x41, 0x47, 0x50, 0x32, 0, 0, 0, 0]
        var reader = GamepadGuestFrameReader()
        let output = reader.feed(bytes)
        #expect(output.frames.count == 2)
        #expect(output.failure == .badMagic)
    }

    @Test func aHeaderThatBreaksTheFormatFailsBeforeItsPayloadArrives() throws {
        var reader = GamepadGuestFrameReader()
        let output = reader.feed(try GamepadFixture.bytes("reject-length"))
        #expect(output.failure == .badLength(3, 4401))
        var host = GamepadHostFrameReader()
        #expect(host.feed(try GamepadFixture.bytes("reject-type")).failure == .unknownType(255))
    }

    @Test func theHostReaderAssemblesHostFrames() throws {
        let frames: [GamepadHostFrame] = [
            .create(create()), .input(slot: 3, report: [1, 2, 3]), .destroy(slot: 3),
            .getReportReply(slot: 0, id: 1, error: 0, data: [9]), .setReportReply(slot: 0, id: 2, error: 0),
        ]
        var reader = GamepadHostFrameReader()
        var got: [GamepadHostFrame] = []
        for byte in frames.flatMap(\.encoded) { got += reader.feed([byte]).frames }
        #expect(got == frames)
    }
}
