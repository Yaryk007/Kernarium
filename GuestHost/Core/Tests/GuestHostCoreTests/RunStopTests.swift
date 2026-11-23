// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// The person's stop of a run across the hand-over to the runtime's thread:
/// kept until the run ends, delivered after the thread entered the runtime.
struct RunStopTests {
    @Test func aStopIsAskedForOnce() {
        let stop = RunStop()
        #expect(!stop.isRequested)
        #expect(stop.request() && stop.isRequested)
        #expect(!stop.request())
    }

    @Test func aStopBeforeTheThreadEntersKeepsItOutOfTheRuntime() {
        let stop = RunStop()
        stop.request()
        #expect(!stop.enter(), "the thread entered the runtime after the stop")
        var asked = 0
        // Nothing is delivered to a runtime that was never entered; the
        // delivery goes on until the run ends.
        #expect(stop.deliver { asked += 1 } && asked == 0)
        stop.end()
        #expect(!stop.deliver { asked += 1 } && asked == 0)
    }

    @Test func aStopAfterTheThreadEnteredIsDeliveredAgainAndAgainUntilTheRunEnds() {
        let stop = RunStop()
        #expect(stop.enter())
        var asked = 0
        // Nothing asked for yet: nothing delivered.
        #expect(stop.deliver { asked += 1 } && asked == 0)
        stop.request()
        // The runtime drops a stop made before its run begins, so the same one
        // is delivered once more, as often as the owner schedules it.
        #expect(stop.deliver { asked += 1 } && asked == 1)
        #expect(stop.deliver { asked += 1 } && asked == 2)
        stop.end()
        #expect(!stop.deliver { asked += 1 } && asked == 2, "a run that ended was asked to stop")
    }

    @Test func endingWaitsForADeliveryUnderWay() {
        let stop = RunStop()
        #expect(stop.enter())
        stop.request()
        let inside = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let ended = DispatchSemaphore(value: 0)
        let delivered = ResultBox()
        let endedWith = ResultBox()
        Thread {
            _ = stop.deliver {
                inside.signal()
                release.wait()
                delivered.set()
            }
        }.start()
        inside.wait()
        Thread {
            stop.end()
            if delivered.value { endedWith.set() }
            ended.signal()
        }.start()
        // The end cannot return while the request is still being made.
        Thread.sleep(forTimeInterval: 0.1)
        #expect(ended.wait(timeout: .now()) == .timedOut)
        release.signal()
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(endedWith.value, "the run ended with a request still on its way to the runtime")
    }

    // MARK: The guest's write-back before the stop

    /// What a delivery did, in order.
    private final class Calls: @unchecked Sendable {
        private let lock = NSLock()
        private var calls: [String] = []
        func add(_ call: String) { lock.withLock { calls.append(call) } }
        var all: [String] { lock.withLock { calls } }
    }

    @Test func theGuestIsAskedToWriteBackOnceBeforeTheFirstStop() {
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        for _ in 0..<3 {
            #expect(stop.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })
        }
        // The runtime drops a stop that arrives early, so the stop is made
        // again and again; the guest is asked once.
        #expect(calls.all == ["settle", "stop", "stop", "stop"])
    }

    @Test func nothingIsAskedOfAGuestWhoseRunWasNeverEnteredOrHasEnded() {
        let calls = Calls()
        let early = RunStop()
        early.request()
        #expect(!early.enter())
        #expect(early.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })
        early.end()
        #expect(!early.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })

        // Entered but not stopped: nothing yet; stopped and then ended: nothing after.
        let late = RunStop()
        #expect(late.enter())
        #expect(late.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })
        late.end()
        late.request()
        #expect(!late.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })
        #expect(calls.all.isEmpty)
    }

    @Test func aStopWithoutAWriteBackIsAsBeforeAndDoesNotUseUpTheOne() {
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        #expect(stop.deliver { calls.add("stop") })
        #expect(stop.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })
        #expect(calls.all == ["stop", "settle", "stop"])
    }

    @Test func endingWaitsForTheWriteBackToo() {
        let stop = RunStop()
        #expect(stop.enter())
        stop.request()
        let inside = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let ended = DispatchSemaphore(value: 0)
        let calls = Calls()
        Thread {
            _ = stop.deliver(settle: {
                inside.signal()
                release.wait()
                calls.add("settle")
                return true
            }) { calls.add("stop") }
        }.start()
        inside.wait()
        Thread {
            stop.end()
            ended.signal()
        }.start()
        Thread.sleep(forTimeInterval: 0.1)
        #expect(ended.wait(timeout: .now()) == .timedOut)
        release.signal()
        #expect(ended.wait(timeout: .now() + 10) == .success)
        #expect(calls.all == ["settle", "stop"], "the run ended inside the stop's own write-back")
    }

    /// A write-back that found no guest asked nobody: the next delivery asks
    /// again, and the one that reaches a guest is the one of the run.
    @Test func aWriteBackThatFoundNoGuestIsMadeAgainUntilOneReachesAGuest() {
        let stop = RunStop()
        let calls = Calls()
        let reached = Script([false, false, true, false])
        #expect(stop.enter())
        stop.request()
        for _ in 0..<5 {
            #expect(stop.deliver(settle: { calls.add("settle"); return reached.next() }) { calls.add("stop") })
        }
        // Three asks, the third reached a guest; the stop follows each of them.
        #expect(calls.all == ["settle", "stop", "settle", "stop", "settle", "stop", "stop", "stop"])
    }

    @Test func aGuestThatDidNotAnswerStillCountsAsReached() {
        // The write-back reports true for a guest that refused or timed out:
        // the stop does not go on asking it every second.
        let stop = RunStop()
        let calls = Calls()
        #expect(stop.enter())
        stop.request()
        for _ in 0..<3 {
            #expect(stop.deliver(settle: { calls.add("settle"); return true }) { calls.add("stop") })
        }
        #expect(calls.all == ["settle", "stop", "stop", "stop"])
    }

    @Test func aWriteBackIsNeverUnderWayTwiceAtOnce() {
        let stop = RunStop()
        #expect(stop.enter())
        stop.request()
        let inside = DispatchSemaphore(value: 0)
        let release = DispatchSemaphore(value: 0)
        let calls = Calls()
        Thread {
            _ = stop.deliver(settle: {
                calls.add("settle")
                inside.signal()
                release.wait()
                return false
            }) { calls.add("stop") }
        }.start()
        inside.wait()
        // A second delivery while the first write-back is still under way
        // asks nothing of the guest.
        #expect(stop.deliver(settle: { calls.add("second settle"); return true }) { calls.add("second stop") })
        release.signal()
        stop.end()
        #expect(!calls.all.contains("second settle"))
    }

    @Test func aRunIsLiveUntilItEnds() {
        let stop = RunStop()
        #expect(stop.isLive)
        #expect(stop.enter())
        stop.request()
        #expect(stop.isLive)
        stop.end()
        #expect(!stop.isLive)
    }

    @Test func theWriteBackIsBoundedByFourSeconds() {
        #expect(StopSettle.bound == .seconds(4))
    }

    private static let settledGuest = GuestSettle(unflushed: 0, storage: .confirmed, suspension: .notAsked)

    @Test func aWriteBackThatFinishesReturnsAtOnce() {
        let stop = RunStop()
        let before = ContinuousClock.now
        let outcome = StopSettle.run(for: stop, within: .seconds(30)) { Self.settledGuest }
        #expect(outcome == .answered(Self.settledGuest))
        #expect(ContinuousClock.now - before < .seconds(5))
    }

    @Test func aWriteBackThatNeverReturnsIsLeftAtTheBound() {
        let stop = RunStop()
        let release = DispatchSemaphore(value: 0)
        let before = ContinuousClock.now
        let outcome = StopSettle.run(for: stop, within: .milliseconds(200)) {
            release.wait()
            return Self.settledGuest
        }
        let took = ContinuousClock.now - before
        #expect(outcome == .timedOut)
        #expect(took >= .milliseconds(150) && took < .seconds(5), "the wait was \(took)")
        release.signal()
    }

    /// The worker that is left behind at the bound, or that starts after the
    /// run ended, must not reach a later run: it asks the run's latch before it
    /// calls the runtime.
    @Test func aWriteBackForARunThatHasEndedIsDroppedBeforeItCallsTheRuntime() {
        let stop = RunStop()
        stop.end()
        let calls = Calls()
        let outcome = StopSettle.run(for: stop, within: .seconds(30)) {
            calls.add("runtime")
            return Self.settledGuest
        }
        #expect(outcome == .runEnded)
        #expect(calls.all.isEmpty, "the runtime was asked for a run that had ended")
        #expect(!outcome.reachedGuest)
    }

    @Test func onlyAWriteBackThatAskedAGuestIsMadeOnce() {
        func outcome(_ storage: GuestSettle.Storage) -> StopSettle.Outcome {
            .answered(GuestSettle(unflushed: 0, storage: storage, suspension: .notAsked))
        }
        #expect(outcome(.confirmed).reachedGuest)
        #expect(outcome(.unconfirmed).reachedGuest)
        #expect(outcome(.refused).reachedGuest)
        #expect(outcome(.unreachable).reachedGuest)
        #expect(!outcome(.noGuest).reachedGuest)
        #expect(!outcome(.notReported).reachedGuest)
        #expect(StopSettle.Outcome.timedOut.reachedGuest)
        #expect(!StopSettle.Outcome.runEnded.reachedGuest)
    }

    /// Answers in turn, then the last.
    private final class Script: @unchecked Sendable {
        private let lock = NSLock()
        private var answers: [Bool]
        init(_ answers: [Bool]) { self.answers = answers }
        func next() -> Bool { lock.withLock { answers.count > 1 ? answers.removeFirst() : answers[0] } }
    }

    private final class ResultBox: @unchecked Sendable {
        private let lock = NSLock()
        private var done = false
        func set() { lock.withLock { done = true } }
        var value: Bool { lock.withLock { done } }
    }
}
