// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// The gamepad connection against a fake guest on the other end of real
/// sockets: connecting and connecting again, what is read, what is written
/// without blocking, what is kept when the connection is full and what ends it.
struct GamepadStreamTests {
    private final class Events: @unchecked Sendable {
        private let lock = NSLock()
        private var list: [GamepadStream.Event] = []
        var all: [GamepadStream.Event] { lock.withLock { list } }
        func add(_ event: GamepadStream.Event) { lock.withLock { list.append(event) } }
        func count(where match: (GamepadStream.Event) -> Bool) -> Int { all.filter(match).count }
        var connected: Int { count { if case .connected = $0 { true } else { false } } }
        var failed: Int { count { if case .attemptFailed = $0 { true } else { false } } }
        var lost: Int { count { if case .lost = $0 { true } else { false } } }
        var frames: [GamepadGuestFrame] { all.compactMap { if case .frame(let frame) = $0 { frame } else { nil } } }
    }

    private struct Rig {
        var stream: GamepadStream
        var guest: GamepadFakeGuest
        var events: Events
    }

    private func rig(retryDelay: DispatchTimeInterval = .milliseconds(10), buffer: Int32? = nil) -> Rig {
        let guest = GamepadFakeGuest()
        if let buffer { guest.limitHostSendBuffer(to: buffer) }
        let events = Events()
        let stream = GamepadStream(opener: guest, retryDelay: retryDelay) { events.add($0) }
        return Rig(stream: stream, guest: guest, events: events)
    }

    private func connected(buffer: Int32? = nil) -> Rig {
        let rig = rig(buffer: buffer)
        rig.stream.start()
        #expect(gamepadEventually { rig.events.connected == 1 })
        return rig
    }

    // MARK: Connecting

    @Test func aStreamConnectsAndAnswersWhatItWrites() throws {
        let rig = connected()
        #expect(rig.stream.send(.destroy(slot: 1)))
        #expect(rig.guest.read(count: 12) == (try GamepadFixture.bytes("destroy-slot-1")))
        #expect(rig.stream.send(.input(slot: 0, report: [0x01, 0x80, 0x80])))
        #expect(rig.guest.read(count: 15) == (try GamepadFixture.bytes("input-slot-0")))
    }

    @Test func aFrameSentWithNoConnectionIsNotSent() {
        let rig = rig()
        #expect(!rig.stream.send(.destroy(slot: 0)))
        rig.stream.start()
        #expect(gamepadEventually { rig.events.connected == 1 })
        rig.stream.stop()
        #expect(!rig.stream.send(.destroy(slot: 0)))
    }

    @Test func theRelaysFramesArriveInOrderWhateverTheirSizes() throws {
        let rig = connected()
        let frames: [GamepadGuestFrame] = [
            .hello(version: 1, slotCount: 8), .created(slot: 0, result: 0), .event(slot: 0, event: .start),
            .output(slot: 0, reportType: 1, data: Array(repeating: 5, count: 63)),
        ]
        for byte in frames.flatMap(\.encoded) { rig.guest.send([byte]) }
        #expect(gamepadEventually { rig.events.frames.count == frames.count })
        #expect(rig.events.frames == frames)
    }

    @Test func attemptsThatFailAreReportedAndRepeatedUntilOneSucceeds() {
        let rig = rig()
        rig.guest.accepting = false
        rig.stream.start()
        #expect(gamepadEventually { rig.events.failed >= 3 })
        #expect(rig.events.connected == 0)
        rig.guest.accepting = true
        #expect(gamepadEventually { rig.events.connected == 1 })
        let failed = rig.events.failed
        usleep(100_000)
        // Connected: no more attempts.
        #expect(rig.events.failed == failed)
        #expect(rig.guest.connections == 1)
    }

    @Test func aStopClosesTheConnectionAndNothingConnectsAgain() {
        let rig = connected()
        rig.stream.stop()
        #expect(rig.guest.closedByHost())
        let attempts = rig.guest.attempts
        usleep(100_000)
        #expect(rig.guest.attempts == attempts)
        #expect(rig.events.lost == 0)
    }

    @Test func aStartAfterAStopConnectsAgain() {
        let rig = connected()
        rig.stream.stop()
        rig.stream.start()
        #expect(gamepadEventually { rig.events.connected == 2 })
    }

    @Test func aConnectionThatEndsIsReportedAndMadeAgain() {
        let rig = connected()
        rig.guest.dropLatest()
        #expect(gamepadEventually { rig.events.lost == 1 })
        #expect(gamepadEventually { rig.events.connected == 2 })
    }

    @Test func aRelayThatBreaksTheFormatIsDroppedAndMadeAgain() throws {
        let rig = connected()
        rig.guest.send(try GamepadFixture.bytes("reject-magic"))
        #expect(gamepadEventually { rig.events.lost == 1 })
        #expect(gamepadEventually { rig.events.connected == 2 })
        // The frames that came before a bad one still count.
        let again = connected()
        again.guest.send(GamepadGuestFrame.hello(version: 1, slotCount: 8).encoded + (try GamepadFixture.bytes("reject-type")))
        #expect(gamepadEventually { again.events.lost == 1 })
        #expect(again.events.frames == [.hello(version: 1, slotCount: 8)])
    }

    @Test func aDropClosesTheConnectionAndConnectsAgain() {
        let rig = connected()
        rig.stream.drop()
        #expect(gamepadEventually { rig.events.lost == 1 })
        #expect(gamepadEventually { rig.events.connected == 2 })
    }

    @Test func aDropWithNoConnectionDoesNothing() {
        let rig = rig()
        rig.stream.drop()
        usleep(50_000)
        #expect(rig.events.all.isEmpty)
    }

    // MARK: A connection that is full

    /// Sends reports for `slot` until the connection can take no more, and
    /// answers how many were sent. The guest does not read meanwhile.
    private func fill(_ rig: Rig, slot: Int = 0, limit: Int = 100_000) -> Int {
        var sent = 0
        while sent < limit, !rig.stream.hasPendingWrites {
            #expect(rig.stream.send(.input(slot: slot, report: [1, UInt8(sent & 0xff), UInt8(sent >> 8 & 0xff)])))
            sent += 1
        }
        return sent
    }

    /// Reads every frame the host has written, until the stream has nothing
    /// waiting and the guest has read all of it.
    private func drain(_ rig: Rig) -> [GamepadHostFrame] {
        var frames: [GamepadHostFrame] = []
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline {
            if let frame = rig.guest.nextFrame(timeout: 0.05) {
                frames.append(frame)
            } else if !rig.stream.hasPendingWrites {
                break
            }
        }
        return frames
    }

    @Test func aGuestThatReadsNothingHasOnlyAFewReportsAhead() {
        let rig = connected()
        let sent = fill(rig)
        // A report of three bytes is 15 on the wire; the last one sent did not fit.
        let ahead = (sent - 1) * 15
        #expect(ahead <= Int(GamepadStream.sendBufferBytes))
        #expect(ahead >= Int(GamepadStream.sendBufferBytes) / 2)
    }

    @Test func aFullConnectionKeepsOnlyTheNewestReportOfEachSlot() {
        let rig = connected(buffer: 2048)
        let sent = fill(rig)
        #expect(sent < 100_000, "the connection fills")
        #expect(rig.stream.hasPendingWrites)
        // Many more states of three slots; none of them blocks, and only the newest of each is kept.
        for value in 0..<500 {
            for slot in 1...3 { #expect(rig.stream.send(.input(slot: slot, report: [1, UInt8(slot), UInt8(value & 0xff)]))) }
            #expect(rig.stream.send(.input(slot: 0, report: [1, 0, UInt8(value & 0xff)])))
        }
        let frames = drain(rig)
        #expect(!rig.stream.hasPendingWrites)
        // Far fewer frames than were sent, and the last report of each slot is the newest sent.
        #expect(frames.count < sent + 2000)
        for slot in 0...3 {
            let last = frames.last { $0.slot == slot }
            #expect(last == .input(slot: slot, report: [1, UInt8(slot), UInt8(499 & 0xff)]), "slot \(slot)")
        }
    }

    @Test func aFullConnectionDoesNotBlockTheCaller() {
        let rig = connected(buffer: 2048)
        _ = fill(rig)
        let stream = rig.stream
        let finished = returnsWithoutWaiting {
            for value in 0..<10_000 { _ = stream.send(.input(slot: 0, report: [1, UInt8(value & 0xff)])) }
        }
        if !finished { rig.guest.dropLatest() }
        #expect(finished, "a send waited for a guest that reads nothing")
        #expect(rig.events.lost == 0)
    }

    @Test func theReportsWaitingAreWrittenWhenTheGuestReadsAgain() {
        let rig = connected(buffer: 2048)
        _ = fill(rig)
        #expect(rig.stream.send(.input(slot: 5, report: [1, 99])))
        let frames = drain(rig)
        #expect(frames.contains(.input(slot: 5, report: [1, 99])))
        #expect(!rig.stream.hasPendingWrites)
        // And the connection works as before.
        #expect(rig.stream.send(.destroy(slot: 5)))
        #expect(rig.guest.nextFrame() == .destroy(slot: 5))
    }

    @Test func aControlFrameThatCannotBeWrittenWholeEndsTheConnection() {
        let rig = connected(buffer: 2048)
        _ = fill(rig)
        // A part of a frame cannot be taken back: the connection is closed and the relay starts again.
        #expect(!rig.stream.send(.create(GamepadFixtureCreate.sample)))
        #expect(gamepadEventually { rig.events.lost == 1 })
        #expect(gamepadEventually { rig.events.connected == 2 })
        #expect(!rig.stream.hasPendingWrites)
    }

    @Test func aControlFrameGoesAfterWhatWasWrittenBeforeIt() {
        let rig = connected()
        #expect(rig.stream.send(.input(slot: 0, report: [1, 1])))
        #expect(rig.stream.send(.destroy(slot: 0)))
        #expect(rig.stream.send(.input(slot: 1, report: [1, 2])))
        // A frame of its own ends the wait: nothing is read after a time has passed.
        #expect(rig.stream.send(.destroy(slot: 7)))
        var frames: [GamepadHostFrame] = []
        while let frame = rig.guest.nextFrame(), frame != .destroy(slot: 7) { frames.append(frame) }
        #expect(frames == [.input(slot: 0, report: [1, 1]), .destroy(slot: 0), .input(slot: 1, report: [1, 2])])
    }

    @Test func aStopWithReportsWaitingLeavesNothingBehind() {
        let rig = connected(buffer: 2048)
        _ = fill(rig)
        rig.stream.stop()
        #expect(!rig.stream.hasPendingWrites)
        #expect(rig.guest.closedByHost())
    }
}

/// A `CREATE` that is small enough to be a control frame of a test.
enum GamepadFixtureCreate {
    static let sample = GamepadCreate(
        slot: 0, bus: 3, vendor: 0, product: 0, version: 0, name: "Pad", uniq: "kernarium-gamepad-0",
        descriptor: [0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0xc0])
}
