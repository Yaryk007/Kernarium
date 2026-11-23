// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// The whole of a host-side end of a guest, as an application composes it:
/// the run's latch asks the guest's write-back (`StopSettle`, with the runtime
/// as a double) before the host's stop, and what the write-back came to
/// decides whether the next delivery asks again.
struct StopPathTests {
    private final class Calls: @unchecked Sendable {
        private let lock = NSLock()
        private var calls: [String] = []
        func add(_ call: String) { lock.withLock { calls.append(call) } }
        var all: [String] { lock.withLock { calls } }
    }

    private static func settle(_ storage: GuestSettle.Storage) -> GuestSettle {
        GuestSettle(unflushed: 0, storage: storage, suspension: .notAsked)
    }

    /// One delivery, with `runtime` as the runtime's write-back and `bound` as
    /// how long to wait for it.
    private func deliver(
        _ stop: RunStop, _ calls: Calls, within bound: DispatchTimeInterval = StopSettle.bound,
        runtime: @escaping @Sendable () -> GuestSettle
    ) -> Bool {
        stop.deliver(settle: {
            calls.add("settle")
            return StopSettle.run(for: stop, within: bound, runtime).reachedGuest
        }) { calls.add("stop") }
    }

    @Test func theGuestIsAskedOnceAndTheHostStopsAfterItEveryTime() {
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        for _ in 0..<3 {
            #expect(deliver(stop, calls) { Self.settle(.confirmed) })
        }
        #expect(calls.all == ["settle", "stop", "stop", "stop"])
    }

    @Test func theHostStopsAtTheBoundWhenTheGuestDoesNotAnswer() {
        let stop = RunStop()
        let calls = Calls()
        let release = DispatchSemaphore(value: 0)
        #expect(stop.enter())
        stop.request()
        let before = ContinuousClock.now
        #expect(deliver(stop, calls, within: .milliseconds(200)) {
            release.wait()
            return Self.settle(.confirmed)
        })
        let took = ContinuousClock.now - before
        #expect(calls.all == ["settle", "stop"])
        #expect(took >= .milliseconds(150) && took < .seconds(5), "the stop came after \(took)")
        // The guest that did not answer was still reached: it is not asked
        // again.
        #expect(deliver(stop, calls) { Self.settle(.confirmed) })
        #expect(calls.all == ["settle", "stop", "stop"])
        release.signal()
    }

    @Test func aGuestThatRefusedOrCouldNotBeReachedIsNotAskedAgain() {
        for storage in [GuestSettle.Storage.refused, .unconfirmed, .unreachable] {
            let stop = RunStop()
            let calls = Calls()
            #expect(stop.enter())
            stop.request()
            for _ in 0..<2 {
                #expect(deliver(stop, calls) { Self.settle(storage) })
            }
            #expect(calls.all == ["settle", "stop", "stop"], "\(storage)")
        }
    }

    @Test func theHostStopsAndTheNextDeliveryAsksAgainWhenThereIsNoGuest() {
        for storage in [GuestSettle.Storage.noGuest, .notReported] {
            let stop = RunStop()
            let calls = Calls()
            #expect(stop.enter())
            stop.request()
            for _ in 0..<2 {
                #expect(deliver(stop, calls) { Self.settle(storage) })
            }
            #expect(calls.all == ["settle", "stop", "settle", "stop"], "\(storage)")
        }
    }

    @Test func aRunThatHasEndedIsAskedNothing() {
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        stop.end()
        #expect(!deliver(stop, calls) { Self.settle(.confirmed) })
        #expect(calls.all.isEmpty)
    }

    /// A write-back whose worker begins after the run ended is dropped before
    /// it calls the runtime.
    @Test func aWriteBackThatBeginsAfterTheRunEndedNeverReachesTheRuntime() {
        let stop = RunStop()
        stop.end()
        let calls = Calls()
        let outcome = StopSettle.run(for: stop) {
            calls.add("runtime")
            return Self.settle(.confirmed)
        }
        #expect(outcome == .runEnded)
        #expect(calls.all.isEmpty)
    }
}
