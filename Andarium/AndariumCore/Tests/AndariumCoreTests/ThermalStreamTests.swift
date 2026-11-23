// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

/// The thermal stream against a fake HAL on the other end of real sockets:
/// the record written on connect and at each change, the reconnection, and
/// what a stop leaves.
struct ThermalStreamTests {
    private final class Events: @unchecked Sendable {
        private let lock = NSLock()
        private var seen: [ThermalStream.Event] = []
        func add(_ event: ThermalStream.Event) { lock.withLock { seen.append(event) } }
        func count(_ matches: (ThermalStream.Event) -> Bool) -> Int { lock.withLock { seen.filter(matches).count } }
        var connected: Int { count { if case .connected = $0 { true } else { false } } }
        var failed: Int { count { if case .attemptFailed = $0 { true } else { false } } }
        var lost: Int { count { if case .lost = $0 { true } else { false } } }
    }

    private func stream(_ guest: FakeThermalGuest, _ events: Events) -> ThermalStream {
        ThermalStream(opener: guest, retryDelay: .milliseconds(10)) { events.add($0) }
    }

    private func frame(_ name: String) throws -> [UInt8] { try ThermalFixture.bytes(name) }

    @Test func theLatestSeverityIsWrittenOnConnect() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        #expect(guest.record() == (try frame("none")), "a connection starts with the initial severity")
        #expect(thermalEventually { events.connected == 1 })
        stream.stop()
    }

    @Test func aSeverityGivenBeforeTheConnectionIsTheFirstRecord() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.send(.severe)
        stream.start()
        #expect(guest.record() == (try frame("severe")))
        stream.stop()
    }

    @Test func eachChangeIsOneRecordInOrder() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        #expect(guest.record() == (try frame("none")))
        stream.send(.light)
        stream.send(.severe)
        stream.send(.critical)
        let bytes = guest.read(count: 3 * ThermalProtocol.recordSize)
        let expected = try frame("light") + frame("severe") + frame("critical")
        #expect(bytes == expected)
        #expect(!guest.hasPending(), "more than one record per change")
        stream.stop()
    }

    @Test func aLostConnectionIsMadeAgainWithTheLatestSeverity() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        _ = guest.record()
        stream.send(.severe)
        _ = guest.record()
        guest.dropLatest()
        #expect(thermalEventually { guest.connections == 2 && events.connected == 2 })
        #expect(guest.record() == (try frame("severe")))
        #expect(events.lost == 1)
        stream.stop()
    }

    @Test func aWriteToAGoneGuestEndsTheConnectionAndItIsMadeAgain() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        _ = guest.record()
        guest.dropLatest()
        // The host may not have seen the end yet; the write sees it.
        stream.send(.light)
        stream.send(.severe)
        #expect(thermalEventually { guest.connections == 2 })
        #expect(guest.record() == (try frame("severe")))
        stream.stop()
    }

    @Test func aGuestThatReadsNothingNeverHoldsTheCallerAndTheConnectionIsMadeAgain() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        #expect(thermalEventually { events.connected == 1 })
        let finished = returnsWithoutWaiting {
            // A record is a few bytes and the socket holds some thousand: more changes than it takes.
            for round in 0..<1_500 { stream.send(round % 2 == 0 ? .light : .severe) }
            stream.send(.critical)
        }
        #expect(finished, "a send waited for a guest that reads nothing")
        guard finished else {
            // Frees a send that a regression left blocked.
            guest.dropLatest()
            stream.stop()
            return
        }
        // The record that did not fit closed the connection, and the next one carries the latest severity.
        // Whether the loop was still running when it was made again decides what the new connection starts
        // with, so the check is on the last record before a sentinel that nothing else sends.
        #expect(thermalEventually { guest.connections == 2 && events.lost == 1 })
        stream.send(AndroidThermalSeverity.none)
        let sentinel = try frame("none")
        var record: [UInt8] = []
        var previous: [UInt8] = []
        repeat {
            previous = record
            record = guest.read(count: ThermalProtocol.recordSize)
        } while record != sentinel && record.count == ThermalProtocol.recordSize
        #expect(record == sentinel, "the sentinel never came")
        #expect(previous == (try frame("critical")), "the last record before it is not the latest severity")
        stream.stop()
    }

    @Test func attemptsThatFailAreTriedAgainUntilTheGuestAccepts() throws {
        let guest = FakeThermalGuest()
        guest.accepting = false
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        #expect(thermalEventually { guest.attempts >= 3 && events.failed >= 3 })
        #expect(events.connected == 0)
        guest.accepting = true
        #expect(guest.record() == (try frame("none")))
        #expect(thermalEventually { events.connected == 1 })
        stream.stop()
    }

    @Test func aStopClosesTheConnectionAndEndsTheAttempts() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        _ = guest.record()
        stream.stop()
        var byte: UInt8 = 0
        let peer = try #require(guest.latest)
        var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
        #expect(poll(&descriptor, 1, 3000) > 0)
        #expect(recv(peer, &byte, 1, MSG_DONTWAIT) == 0, "the host did not close the connection")
        stream.send(.severe)
        usleep(100_000)
        #expect(guest.connections == 1, "a stopped stream connected again")
        #expect(events.lost == 0, "a stop is not a loss")
    }

    @Test func whatTheGuestSendsIsIgnored() throws {
        let guest = FakeThermalGuest()
        let events = Events()
        let stream = stream(guest, events)
        stream.start()
        _ = guest.record()
        let peer = try #require(guest.latest)
        let noise: [UInt8] = [1, 2, 3, 4, 5, 6, 7, 8, 9]
        _ = noise.withUnsafeBytes { Darwin.send(peer, $0.baseAddress, $0.count, 0) }
        usleep(50_000)
        stream.send(.severe)
        #expect(guest.record() == (try frame("severe")))
        #expect(events.lost == 0)
        #expect(guest.connections == 1)
        stream.stop()
    }
}
