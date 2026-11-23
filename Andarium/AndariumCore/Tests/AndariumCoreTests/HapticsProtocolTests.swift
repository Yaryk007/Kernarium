// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// The haptics wire format against the golden frames in
/// `tests/unit/andarium/fixtures/haptics-golden-frames.txt`, which the guest's
/// encoder is tested against too: what each frame means, the acknowledgements
/// the host sends, and every way a frame can break the format.
struct HapticsProtocolTests {
    private func decoded(_ name: String) throws -> HapticsFrame {
        try HapticsProtocol.decode(HapticsFixture.bytes(name))
    }

    // MARK: Guest to host

    @Test func theDesignsGoldenFramesAreInTheFixture() throws {
        // The two frames the design fixes byte for byte.
        #expect(try HapticsFixture.bytes("play-primitive-click") == [
            0x41, 0x48, 0x56, 0x31, 0x01, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x07, 0x00, 0x00, 0x00,
            0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
            0x00, 0x00, 0x00, 0x00,
        ])
        #expect(try HapticsFixture.bytes("ack-played") == [
            0x41, 0x48, 0x56, 0x31, 0x10, 0x00, 0x09, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2c, 0x01, 0x00, 0x00, 0x00,
        ])
    }

    @Test func aPrimitiveIsDecodedWithItsScaleAndDuration() throws {
        #expect(try decoded("play-primitive-click") == .play(HapticScore(sequence: 7, events: [
            .primitive(.click, scale: 1, durationMilliseconds: 12),
        ])))
    }

    @Test func aToneIsDecodedWithItsSharpness() throws {
        #expect(try decoded("play-tone") == .play(HapticScore(sequence: 1, events: [
            .tone(durationMilliseconds: 500, intensity: 1, sharpness: 0.5),
        ])))
    }

    @Test func anEffectIsDecodedWithItsStrength() throws {
        #expect(try decoded("play-effect-double-click") == .play(HapticScore(sequence: 2, events: [
            .effect(.doubleClick, strength: .medium, durationMilliseconds: 112),
        ])))
        #expect(try decoded("play-effect-texture-tick") == .play(HapticScore(sequence: 3, events: [
            .effect(.textureTick, strength: .light, durationMilliseconds: 8),
        ])))
    }

    @Test func aCompositionKeepsItsElementsInTime() throws {
        let frame = try decoded("play-composition")
        #expect(frame == .play(HapticScore(sequence: 4, events: [
            .primitive(.click, scale: 0.5, durationMilliseconds: 12),
            .primitive(.thud, scale: 1, startMilliseconds: 62, durationMilliseconds: 60),
        ])))
        guard case .play(let score) = frame else { return }
        #expect(score.endMilliseconds == 122)
    }

    @Test func aScoreNamesItsTarget() throws {
        #expect(try decoded("play-second-target").target == HapticTarget(rawValue: 1))
        #expect(try decoded("play-primitive-click").target == .device)
    }

    @Test func anIntensityAndAStopAreDecoded() throws {
        #expect(try decoded("intensity-half") == .intensity(target: .device, sequence: 6, intensity: 0.5))
        #expect(try decoded("stop") == .stop(target: .device, sequence: 8))
    }

    @Test func theLargestScoreFitsAFrame() throws {
        var bytes = HapticsProtocol.header(
            type: .play, length: HapticsProtocol.playPayloadHeaderSize + 128 * HapticsProtocol.eventSize)
        bytes += [0, 0, 128, 0, 1, 0, 0, 0]
        for _ in 0..<128 { bytes += try Array(HapticsFixture.bytes("play-primitive-click")[16...]) }
        #expect(bytes.count == 2576 && bytes.count - 8 <= HapticsProtocol.maximumPayload)
        guard case .play(let score) = try HapticsProtocol.decode(bytes) else {
            Issue.record("not a PLAY")
            return
        }
        #expect(score.events.count == 128)
    }

    // MARK: Host to guest

    @Test func theAcknowledgementsAreEncodedAsTheFixtureHasThem() throws {
        #expect(HapticsAcknowledgement(sequence: 7, microseconds: 300, result: .played).encoded
            == Data(try HapticsFixture.bytes("ack-played")))
        #expect(HapticsAcknowledgement(sequence: 9, microseconds: 0, result: .notInFront).encoded
            == Data(try HapticsFixture.bytes("ack-not-in-front")))
        #expect(HapticsAcknowledgement(sequence: 10, microseconds: 1500, result: .engineFailed).encoded
            == Data(try HapticsFixture.bytes("ack-engine-failed")))
    }

    // MARK: A frame that breaks the format

    @Test func everyBrokenFrameOfTheFixtureIsRefused() throws {
        let broken = try HapticsFixture.frames().filter { $0.name.hasPrefix("reject-") }
        #expect(broken.count >= 20)
        for frame in broken {
            #expect(throws: HapticsProtocol.Failure.self, "\(frame.name)") { try HapticsProtocol.decode(frame.bytes) }
            // Fed to a stream, it ends the stream's use and produces nothing.
            var reader = HapticsFrameReader()
            let output = reader.feed(frame.bytes)
            if output.failure == nil {
                // A length that promises more than arrived waits for the rest,
                // which the decoder of one whole message cannot.
                #expect(reader.pendingByteCount > 0, "\(frame.name)")
            }
            #expect(output.frames.isEmpty, "\(frame.name)")
        }
    }

    @Test func eachFailureIsNamed() throws {
        func failure(_ name: String) throws -> HapticsProtocol.Failure? {
            do {
                _ = try HapticsProtocol.decode(HapticsFixture.bytes(name))
                return nil
            } catch let error as HapticsProtocol.Failure {
                return error
            }
        }
        #expect(try failure("reject-bad-magic") == .badMagic)
        #expect(try failure("reject-unknown-type") == .unknownType(99))
        #expect(try failure("reject-ack-from-guest") == .unknownType(16))
        #expect(try failure("reject-length-over-4096") == .badLength(1, 4097))
        #expect(try failure("reject-length-not-whole-events") == .badLength(1, 27))
        #expect(try failure("reject-play-without-events") == .badLength(1, 8))
        #expect(try failure("reject-play-count-mismatch") == .badLength(1, 28))
        #expect(try failure("reject-play-flags") == .badReserved)
        #expect(try failure("reject-event-kind") == .badEvent(0))
        #expect(try failure("reject-event-reserved") == .badReserved)
        #expect(try failure("reject-nan-intensity") == .nonFiniteNumber)
        #expect(try failure("reject-infinite-sharpness") == .nonFiniteNumber)
        #expect(try failure("reject-intensity-nan") == .nonFiniteNumber)
        #expect(try failure("reject-intensity-zero") == .badIntensity)
        #expect(try failure("reject-stop-reserved") == .badReserved)
    }

    @Test func moreThanTheMostEventsIsRefusedWhateverTheLength() throws {
        let events = HapticsProtocol.maximumEvents + 1
        var bytes = HapticsProtocol.header(
            type: .play, length: HapticsProtocol.playPayloadHeaderSize + events * HapticsProtocol.eventSize)
        bytes += [0, 0, UInt8(events & 0xff), UInt8(events >> 8), 1, 0, 0, 0]
        for _ in 0..<events { bytes += try Array(HapticsFixture.bytes("play-primitive-click")[16...]) }
        #expect(throws: HapticsProtocol.Failure.badCount(129)) { try HapticsProtocol.decode(bytes) }
    }

    // MARK: The stream

    @Test func framesComeOutOfAnyPiecesOfTheStream() throws {
        let names = ["play-primitive-click", "intensity-half", "stop", "play-composition"]
        let bytes = try names.flatMap { try HapticsFixture.bytes($0) }
        let expected = try names.map { try decoded($0) }
        for piece in [1, 2, 7, 36, bytes.count] {
            var reader = HapticsFrameReader()
            var frames: [HapticsFrame] = []
            var offset = 0
            while offset < bytes.count {
                let output = reader.feed(bytes[offset..<min(offset + piece, bytes.count)])
                #expect(output.failure == nil)
                frames += output.frames
                offset += piece
            }
            #expect(frames == expected, "pieces of \(piece)")
            #expect(reader.pendingByteCount == 0)
        }
    }

    @Test func framesBeforeABadOneStillCount() throws {
        var reader = HapticsFrameReader()
        let bytes = try HapticsFixture.bytes("stop") + HapticsFixture.bytes("reject-event-kind")
            + HapticsFixture.bytes("stop")
        let output = reader.feed(bytes)
        #expect(output.frames == [.stop(target: .device, sequence: 8)])
        #expect(output.failure == .badEvent(0))
        #expect(reader.pendingByteCount == 0)
    }

    @Test func aLengthThatCannotBeRightIsRefusedBeforeItsBytesArrive() throws {
        var reader = HapticsFrameReader()
        // A header alone is enough to refuse 4097 bytes of payload.
        let output = reader.feed(try HapticsFixture.bytes("reject-length-over-4096"))
        #expect(output.failure == .badLength(1, 4097))
    }

    @Test func aResetForgetsTheStartOfAFrame() throws {
        var reader = HapticsFrameReader()
        let stop = try HapticsFixture.bytes("stop")
        #expect(reader.feed(stop.prefix(5)).frames.isEmpty)
        #expect(reader.pendingByteCount == 5)
        reader.reset()
        #expect(reader.feed(stop).frames == [.stop(target: .device, sequence: 8)])
    }
}
