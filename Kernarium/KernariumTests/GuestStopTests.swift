// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import GuestHostCore
import Testing
@testable import Kernarium

/// What the application does to end a running guest from the host side: the
/// screen closing and Shut down over a paused guest both come to
/// `EmulatorLauncher.stop()`. The guest is asked to write its storage back
/// once, for as long as a lifecycle edge may take at most, and the host stops
/// it after that; the main actor waits for none of it. The runtime is a
/// double here: the application's own guest runs behind these tests, and its
/// write-back is not theirs to ask.
@MainActor
struct GuestStopTests {
    // MARK: Doubles

    /// What the stop's path asked of the runtime, in order, and whether any of
    /// it ran on the main thread.
    private final class Calls: @unchecked Sendable {
        private let lock = NSLock()
        private var calls: [String] = []
        private var onMain = false

        func add(_ call: String) {
            lock.withLock {
                calls.append(call)
                if Thread.isMainThread { onMain = true }
            }
        }

        var all: [String] { lock.withLock { calls } }
        var askedOnMain: Bool { lock.withLock { onMain } }
    }

    private nonisolated static func settled(
        _ storage: GuestSettle.Storage = .confirmed
    ) -> GuestSettle {
        GuestSettle(unflushed: 0, storage: storage, suspension: .notAsked)
    }

    /// The stops a launcher handed to its delivery.
    private final class Delivered {
        var stops: [RunStop] = []
    }

    private func launcher(
        deliver: @escaping @MainActor (RunStop) -> Void
    ) -> EmulatorLauncher {
        EmulatorLauncher(
            configuration: .load(),
            job: BackgroundJob(scheduler: FakeJobScheduler()),
            deliverStop: deliver
        )
    }

    /// Waits, for at most ten seconds, for what a queue does off the main
    /// actor.
    private func until(_ condition: () -> Bool) async -> Bool {
        (try? await eventually(within: .seconds(10), every: .milliseconds(5), condition))
            ?? false
    }

    // MARK: The launcher

    @Test
    func aStopWithNoRunInHandHasNothingToEnd() {
        let delivered = Delivered()
        let launcher = launcher { delivered.stops.append($0) }
        launcher.stop()
        #expect(launcher.stopRequested)
        #expect(delivered.stops.isEmpty)
    }

    @Test
    func aStopOfTheRunIsDeliveredOnceHoweverOftenItIsAsked() {
        let delivered = Delivered()
        let launcher = launcher { delivered.stops.append($0) }
        let latch = launcher.runBegan()
        #expect(launcher.phase == .running)
        for _ in 0..<3 { launcher.stop() }
        #expect(launcher.stopRequested)
        #expect(latch.isRequested)
        #expect(delivered.stops.count == 1)
        #expect(delivered.stops.first === latch)
        latch.end()
        launcher.finished(result: 0)
    }

    @Test
    func aStopAfterTheRunEndedIsDeliveredNowhere() {
        let delivered = Delivered()
        let launcher = launcher { delivered.stops.append($0) }
        let latch = launcher.runBegan()
        latch.end()
        launcher.finished(result: 0)
        launcher.stop()
        #expect(delivered.stops.isEmpty)
    }

    /// The runtime drops a stop from before its own run begins, so a stop that
    /// came before the thread entered keeps the run out of it.
    @Test
    func aStopBeforeTheThreadEntersKeepsTheRunOutOfTheRuntime() {
        let launcher = launcher { _ in }
        let latch = launcher.runBegan()
        launcher.stop()
        var entered = false
        let result = EmulatorLauncher.runThread(latch) {
            entered = true
            return 7
        }
        #expect(!entered)
        #expect(result == 0)
        #expect(!latch.isLive)
        launcher.finished(result: result)
        #expect(launcher.phase == .ended(result: 0))
    }

    @Test
    func theLatchEndsWhenTheRuntimeReturns() {
        let launcher = launcher { _ in }
        let latch = launcher.runBegan()
        #expect(latch.isLive)
        let result = EmulatorLauncher.runThread(latch) { -5 }
        #expect(result == -5)
        #expect(!latch.isLive)
        launcher.finished(result: result)
    }

    /// The write-back waits as long as a lifecycle edge's own work.
    @Test
    func theWriteBackWaitsAsLongAsALifecycleEdge() {
        #expect(StopSettle.bound == BackgroundController.edgeBudget)
    }

    // MARK: The path to the runtime

    /// The launcher's stop, on the application's own delivery: the main actor
    /// is not held for the guest, the guest is asked once, and the host's stop
    /// follows the write-back and is made again.
    @Test
    func aStopAsksTheGuestOnceBeforeTheHostStopsAndKeepsTheMainActorFree() async throws {
        let sequence = Calls()
        let release = DispatchSemaphore(value: 0)
        let launcher = launcher {
            GuestPersistence.shared.deliverStop(
                $0, every: .milliseconds(50),
                runtime: .init(
                    sync: {
                        sequence.add("settle began")
                        release.wait()
                        sequence.add("settle ended")
                        return Self.settled()
                    },
                    requestStop: { sequence.add("stop") }
                )
            )
        }
        let latch = launcher.runBegan()
        #expect(latch.enter())

        let before = ContinuousClock.now
        launcher.stop()
        // The guest is slow to answer, and the screen's closing waits for none
        // of it.
        #expect(ContinuousClock.now - before < .milliseconds(500))
        try #require(await until { sequence.all == ["settle began"] })
        try await Task.sleep(for: .milliseconds(300))
        #expect(sequence.all == ["settle began"], "the host stopped the guest while it wrote back")

        release.signal()
        try #require(await until { sequence.all.count >= 4 }, "the stop was not made again")
        latch.end()
        launcher.finished(result: 0)
        try await Task.sleep(for: .milliseconds(200))
        let calls = sequence.all
        #expect(Array(calls.prefix(3)) == ["settle began", "settle ended", "stop"])
        #expect(calls.filter { $0.hasPrefix("settle") }.count == 2, "the guest was asked more than once")
        #expect(calls.dropFirst(3).allSatisfy { $0 == "stop" })
        #expect(!sequence.askedOnMain, "the runtime was asked on the main thread")
    }

    /// A guest that does not answer holds the host's stop for the bound at
    /// most: the runtime's write-back never returns, and the stop follows at
    /// the application's own bound.
    @Test
    func aGuestThatDoesNotAnswerHoldsTheStopForTheBoundAtMost() async throws {
        let sequence = Calls()
        let release = DispatchSemaphore(value: 0)
        let launcher = launcher {
            GuestPersistence.shared.deliverStop(
                $0, every: .milliseconds(50),
                runtime: .init(
                    sync: {
                        release.wait()
                        return Self.settled()
                    },
                    requestStop: { sequence.add("stop") }
                )
            )
        }
        let latch = launcher.runBegan()
        #expect(latch.enter())
        let before = ContinuousClock.now
        launcher.stop()
        try #require(await until { !sequence.all.isEmpty }, "the host's stop never came")
        let waited = ContinuousClock.now - before
        #expect(
            waited >= .seconds(3) && waited < .seconds(9),
            "the stop came after \(waited), not at the bound"
        )
        // A guest that was reached is the run's one write-back: the rest of
        // the deliveries are stops alone.
        try #require(await until { sequence.all.count >= 3 })
        release.signal()
        latch.end()
        launcher.finished(result: 0)
    }

    /// A delivery that finds no guest goes on to the host's stop, and the next
    /// one asks again.
    @Test
    func aStopWithNoGuestYetAsksAgainUntilOneIsReached() async throws {
        let sequence = Calls()
        let launcher = launcher {
            GuestPersistence.shared.deliverStop(
                $0, every: .milliseconds(50),
                runtime: .init(
                    sync: {
                        sequence.add("settle")
                        // The guest is there from the third ask on.
                        return Self.settled(
                            sequence.all.filter { $0 == "settle" }.count >= 3
                                ? .confirmed : .noGuest
                        )
                    },
                    requestStop: { sequence.add("stop") }
                )
            )
        }
        let latch = launcher.runBegan()
        #expect(latch.enter())
        launcher.stop()
        try #require(await until { sequence.all.count >= 8 }, "the stop was not made again")
        latch.end()
        launcher.finished(result: 0)
        try await Task.sleep(for: .milliseconds(200))
        let calls = sequence.all
        #expect(calls.filter { $0 == "settle" }.count == 3, "the guest was asked \(calls)")
        #expect(Array(calls.prefix(6)) == ["settle", "stop", "settle", "stop", "settle", "stop"])
        #expect(calls.dropFirst(6).allSatisfy { $0 == "stop" })
    }

    /// The application's own stop, with nothing replaced but the runtime's two
    /// calls: the launcher's default delivery, the queue it goes through and
    /// the write-back the default composes. Removing the write-back from the
    /// default leaves the host stop alone, which this fails.
    @Test
    func theApplicationsOwnStopAsksTheGuestOnceBeforeTheHostStops() async throws {
        let calls = Calls()
        let launcher = EmulatorLauncher(
            configuration: .load(),
            job: BackgroundJob(scheduler: FakeJobScheduler()),
            stopRuntime: .init(
                sync: {
                    calls.add("write back")
                    return Self.settled()
                },
                requestStop: { calls.add("stop") }
            )
        )
        let latch = launcher.runBegan()
        #expect(latch.enter())
        launcher.stop()
        // The delivery is made again every second until the run ends.
        try #require(await until { calls.all.count >= 3 }, "the stop was not made again")
        latch.end()
        launcher.finished(result: 0)
        #expect(
            Array(calls.all.prefix(3)) == ["write back", "stop", "stop"],
            "the application's stop was \(calls.all)"
        )
        #expect(!calls.askedOnMain, "the runtime was asked on the main thread")
    }

    /// The write-back of a run that has ended is dropped before it reaches the
    /// runtime, where it would ask whatever runs next.
    @Test
    func theWriteBackOfARunThatEndedAsksNoRuntime() {
        let stop = RunStop()
        stop.end()
        #expect(!GuestStopDelivery.settleBeforeStop(for: stop, logPrefix: "Kernarium: "))
    }
}
