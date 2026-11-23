// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing

@testable import AndariumCore

/// The haptics stream against a guest on the other end of a real socket that
/// reads nothing: what it does with acknowledgements the connection cannot take.
struct HapticsStreamTests {
    private final class Guest: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var peers: [Int32] = []
        var connections: Int { lock.withLock { peers.count } }
        var latest: Int32? { lock.withLock { peers.last.flatMap { $0 >= 0 ? $0 : nil } } }
        deinit { for peer in peers where peer >= 0 { close(peer) } }

        /// The guest's end of the latest connection goes away: closed here and
        /// forgotten, so that it is closed once. A second close would be of a
        /// number that another test may be using for a file by then.
        func dropLatest() {
            lock.withLock {
                guard let peer = peers.last, peer >= 0 else { return }
                peers[peers.count - 1] = -1
                close(peer)
            }
        }

        func openStream() throws -> Int32 {
            try lock.withLock {
                var sockets: [Int32] = [-1, -1]
                guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
                peers.append(sockets[1])
                return sockets[0]
            }
        }
    }

    private final class Events: @unchecked Sendable {
        private let lock = NSLock()
        private var seen: [HapticsStream.Event] = []
        func add(_ event: HapticsStream.Event) { lock.withLock { seen.append(event) } }
        var connected: Bool { lock.withLock { seen.contains { if case .connected = $0 { true } else { false } } } }
        var lost: Int { lock.withLock { seen.filter { if case .lost = $0 { true } else { false } }.count } }
    }

    @Test func aGuestThatReadsNothingNeverHoldsTheCallerAndTheAcknowledgementsAreDropped() throws {
        let guest = Guest()
        let events = Events()
        let stream = HapticsStream(opener: guest, retryDelay: .milliseconds(10)) { events.add($0) }
        stream.start()
        defer { stream.stop() }
        let deadline = Date().addingTimeInterval(3)
        while !events.connected, Date() < deadline { usleep(2000) }
        #expect(events.connected)
        let acknowledgement = HapticsAcknowledgement(sequence: 1, microseconds: 5, result: .played)
        let finished = returnsWithoutWaiting {
            // An acknowledgement is 17 bytes and the socket holds some hundred: more than it takes.
            for _ in 0..<2_000 { stream.send(acknowledgement) }
        }
        // The peer is closed if a regression left the sender blocked, which frees it.
        if !finished { guest.dropLatest() }
        #expect(finished, "a send waited for a guest that reads nothing")
        // What cannot be written is dropped (the guest never waits for it); the connection stays.
        #expect(events.lost == 0)
        #expect(guest.connections == 1)
        // And the guest can read what was written, then the next acknowledgement arrives.
        let peer = try #require(guest.latest)
        var buffer = [UInt8](repeating: 0, count: 65536)
        while recv(peer, &buffer, buffer.count, MSG_DONTWAIT) > 0 {}
        let last = HapticsAcknowledgement(sequence: 99, microseconds: 1, result: .played)
        stream.send(last)
        var received: [UInt8] = []
        let until = Date().addingTimeInterval(3)
        while received.count < 17, Date() < until {
            var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, 10) > 0 else { continue }
            var bytes = [UInt8](repeating: 0, count: 17 - received.count)
            let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
            guard size > 0 else { break }
            received.append(contentsOf: bytes.prefix(size))
        }
        #expect(received == Array(last.encoded))
    }
}
