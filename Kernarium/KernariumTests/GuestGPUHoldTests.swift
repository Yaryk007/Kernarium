// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The hold on the guest's GPU requests has a bound that is a timer of its own,
/// so the hold lands whatever the main thread is doing. These tests drive the
/// hold directly; the policy's use of it is in `BackgroundControllerTests`.
struct GuestGPUHoldTests {
    private struct Rig {
        let log = CallLog()
        let timer = ManualHoldTimer()
        let hold: GuestGPUHold

        init() {
            hold = GuestGPUHold(gate: RecordingGate(log), timer: timer.timer)
        }
    }

    @Test
    func aHoldWithADeadlineTellsTheGateWhatItLeaves() {
        let log = CallLog()
        let gate = BudgetGate(log)
        let hold = GuestGPUHold(gate: gate, timer: ManualHoldTimer().timer)

        #expect(hold.hold(before: DispatchTime.now() + .milliseconds(2500)))
        #expect(log.entries == ["hold within"])
        let left = gate.left
        #expect(left.count == 1)
        #expect((2.3...2.5).contains(left.first ?? 0))

        #expect(hold.release())
        // A deadline that has passed is told as what is left of it: nothing.
        #expect(hold.hold(before: DispatchTime.now() - .milliseconds(100)))
        #expect((gate.left.last ?? 1) <= 0)
    }

    @Test
    func aHoldWithNoDeadlineWaitsForTheCameraAsItAlwaysDid() {
        let log = CallLog()
        let gate = BudgetGate(log)
        let hold = GuestGPUHold(gate: gate, timer: ManualHoldTimer().timer)

        #expect(hold.hold())
        #expect(log.entries == ["hold"])
        #expect(gate.left.isEmpty)
    }

    @Test
    func aHoldTakenIsTakenOnceAndReleasedOnce() {
        let rig = Rig()

        #expect(rig.hold.hold())
        #expect(!rig.hold.hold())
        #expect(rig.hold.isHeld)
        #expect(rig.log.entries == ["hold"])

        #expect(rig.hold.release())
        #expect(!rig.hold.release())
        #expect(!rig.hold.isHeld)
        #expect(rig.log.entries == ["hold", "release"])

        // A released hold can be taken again.
        #expect(rig.hold.hold())
        #expect(rig.log.entries == ["hold", "release", "hold"])
    }

    @Test
    func theBoundTakesTheHoldWhenItFires() {
        let rig = Rig()
        let landed = CallLog()

        rig.hold.arm(after: 1.5) { landed.add("landed") }
        #expect(rig.timer.delays == [1.5])
        #expect(rig.log.entries.isEmpty)

        rig.timer.fire()
        #expect(rig.log.entries == ["hold"])
        #expect(landed.entries == ["landed"])
        #expect(rig.hold.isHeld)
    }

    @Test
    func aHoldTakenFirstLeavesTheBoundNothingToDo() {
        let rig = Rig()
        let landed = CallLog()
        rig.hold.arm(after: 1.5) { landed.add("landed") }

        #expect(rig.hold.hold())
        #expect(rig.timer.pending == 0)
        rig.timer.fire()

        #expect(rig.log.entries == ["hold"])
        #expect(landed.entries.isEmpty)
    }

    /// A real timer can fire as it is withdrawn: its handler is already on its
    /// way when the withdrawal reaches it. The handler must not hold what the
    /// return has just released.
    @Test
    func aBoundThatFiresAsItIsWithdrawnHoldsNothing() {
        let rig = Rig()
        rig.timer.honorsWithdrawal = false
        let landed = CallLog()
        rig.hold.arm(after: 1.5) { landed.add("landed") }

        #expect(!rig.hold.release())
        rig.timer.fire()

        #expect(rig.log.entries.isEmpty)
        #expect(landed.entries.isEmpty)
        #expect(!rig.hold.isHeld)
    }

    @Test
    func aStaleBoundDoesNotHoldAfterAHoldAndARelease() {
        let rig = Rig()
        rig.timer.honorsWithdrawal = false
        rig.hold.arm(after: 1.5)

        #expect(rig.hold.hold())
        #expect(rig.hold.release())
        rig.timer.fire()

        #expect(rig.log.entries == ["hold", "release"])
        #expect(!rig.hold.isHeld)
    }

    @Test
    func armingAgainReplacesTheBound() {
        let rig = Rig()
        let landed = CallLog()
        rig.hold.arm(after: 1.5) { landed.add("first") }
        rig.hold.arm(after: 3) { landed.add("second") }
        #expect(rig.timer.delays == [1.5, 3])
        #expect(rig.timer.pending == 1)

        rig.timer.fire()
        #expect(rig.log.entries == ["hold"])
        #expect(landed.entries == ["second"])
    }

    /// Both paths racing for the hold take it between them. The bound does not
    /// wait for a hold that is landing, so the gate may be told twice (the
    /// runtime's gate is one state), and it is released once.
    @Test
    func theBoundAndTheHoldRacingTakeTheHold() async {
        for _ in 0..<200 {
            let rig = Rig()
            rig.hold.arm(after: 1.5)
            await withTaskGroup(of: Void.self) { group in
                group.addTask { rig.timer.fire() }
                group.addTask { rig.hold.hold() }
            }
            #expect(rig.hold.isHeld)
            #expect(rig.log.entries.allSatisfy { $0 == "hold" })
            #expect((1...2).contains(rig.log.entries.count))
            #expect(rig.hold.release())
            #expect(rig.log.entries.last == "release")
        }
    }

    /// The edge's main path holds through the gate, which on an Android guest
    /// waits for the guest's camera video to stop before it holds. The bound
    /// fires meanwhile, because the edge is already late: it holds at once,
    /// without that wait and without waiting for the main path, and the main
    /// path's wait ends and finds the gate held. One release gives it back.
    @Test
    nonisolated func aBoundThatFiresWhileTheHoldWaitsForTheCameraHoldsAtOnce() {
        final class WaitingGate: GuestGPUGate, @unchecked Sendable {
            let log = CallLog()
            let began = DispatchSemaphore(value: 0)
            let proceed = DispatchSemaphore(value: 0)

            func hold() {
                log.add("hold begins")
                began.signal()
                // Bounded, so a bound that does not end the wait fails the
                // test instead of hanging it.
                if proceed.wait(timeout: .now() + .seconds(3)) == .timedOut {
                    log.add("hold timed out")
                }
                log.add("hold ends")
            }

            /// The runtime's hold at once ends the wait that is pending.
            func holdNow() {
                log.add("holdNow")
                proceed.signal()
            }

            func release() { log.add("release") }
        }
        let gate = WaitingGate()
        let timer = ManualHoldTimer()
        let hold = GuestGPUHold(gate: gate, timer: timer.timer)
        let landed = CallLog()
        hold.arm(after: 1.5) { landed.add("landed") }

        let finished = DispatchSemaphore(value: 0)
        let result = CallLog()
        let mainPath = Thread {
            result.add(hold.hold() ? "took" : "found it taken")
            finished.signal()
        }
        mainPath.start()
        #expect(gate.began.wait(timeout: .now() + .seconds(5)) == .success)
        // The main path is waiting on the gate. Firing the bound returns
        // without waiting for it.
        timer.fire()
        #expect(Array(gate.log.entries.prefix(2)) == ["hold begins", "holdNow"])
        #expect(landed.entries == ["landed"])

        #expect(finished.wait(timeout: .now() + .seconds(5)) == .success)
        #expect(result.entries == ["took"])
        #expect(gate.log.entries == ["hold begins", "holdNow", "hold ends"])
        #expect(hold.isHeld)
        #expect(hold.release())
        #expect(!hold.release())
        #expect(gate.log.entries.last == "release")
    }

    /// The point of the bound: the main thread is blocked, as it is while it
    /// waits for the guest's storage to settle, and the hold lands anyway. The
    /// production timer is the one under test here, so nothing is simulated.
    @MainActor
    @Test
    func theBoundHoldsWhileTheMainThreadIsBlocked() {
        final class Gate: GuestGPUGate, @unchecked Sendable {
            let held = DispatchSemaphore(value: 0)
            private let lock = NSLock()
            private var onMain: Bool?

            var heldOnMainThread: Bool? { lock.withLock { onMain } }

            func hold() {
                lock.withLock { onMain = Thread.isMainThread }
                held.signal()
            }

            func release() {}
        }
        let gate = Gate()
        let hold = GuestGPUHold(gate: gate)

        hold.arm(after: 0.05)
        // This test runs on the main thread, and the main thread waits here
        // until the hold lands, which it can only do from another thread.
        let landed = gate.held.wait(timeout: .now() + .seconds(5))

        #expect(landed == .success)
        #expect(gate.heldOnMainThread == false)
        #expect(hold.isHeld)
    }

    @MainActor
    @Test
    func aWithdrawnBoundNeverFiresOnTheProductionTimer() {
        let log = CallLog()
        let hold = GuestGPUHold(gate: RecordingGate(log))

        hold.arm(after: 0.05)
        hold.release()
        Thread.sleep(forTimeInterval: 0.3)

        #expect(log.entries.isEmpty)
        #expect(!hold.isHeld)
    }
}
