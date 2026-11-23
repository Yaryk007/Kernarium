// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// The runtime's hold for a guest whose camera video never stops and whose GPU
/// has one request that never ends, as `KernariumGpuHold.hpp` runs it: a hold
/// waits for the camera (two seconds at most, or what its caller's deadline
/// leaves after the drain), then closes the gate and waits one second for the
/// request; the bound's hold closes at once and ends a camera wait that is
/// pending. The C++ rows of `kernarium-gpu-hold` pin that sequence, over the
/// real gate and camera control; this is the model the application's clock is
/// measured against.
private nonisolated final class StuckRuntimeGate: GuestGPUGate, @unchecked Sendable {
    static let cameraBound: TimeInterval = 2
    static let drain: TimeInterval = 1

    private let edge: DispatchTime
    /// The runtime's mutex over the GPU service, held for a close and its drain.
    private let gpu = NSLock()
    private let camera = NSCondition()
    private var abandons = 0
    private let state = NSLock()
    private var closures: [TimeInterval] = []

    init(edge: DispatchTime) {
        self.edge = edge
    }

    /// Seconds after the edge at which the gate closed, the first time.
    var firstClosure: TimeInterval? { state.withLock { closures.first } }

    func hold() { hold(cameraWait: Self.cameraBound) }

    func hold(within left: TimeInterval) {
        hold(cameraWait: min(Self.cameraBound, max(0, left - Self.drain)))
    }

    func holdNow() {
        close()
        camera.lock()
        abandons += 1
        camera.broadcast()
        camera.unlock()
    }

    func release() {}

    private func hold(cameraWait seconds: TimeInterval) {
        camera.lock()
        let started = abandons
        let until = Date().addingTimeInterval(seconds)
        while abandons == started, camera.wait(until: until) {}
        camera.unlock()
        close()
    }

    private func close() {
        gpu.lock()
        defer { gpu.unlock() }
        // A gate that is closed already was drained by the hold that closed it.
        if state.withLock({ !closures.isEmpty }) { return }
        let now = DispatchTime.now().uptimeNanoseconds - edge.uptimeNanoseconds
        state.withLock { closures.append(Double(now) / 1e9) }
        Thread.sleep(forTimeInterval: Self.drain)
    }
}

/// An Android job whose display stays on: the wait for it goes off at its
/// deadline, as the real one does.
@MainActor
private final class EdgeEffects: BackgroundEffects {
    let gpuHold: GuestGPUHold
    let edge: DispatchTime
    /// Seconds after the edge, and seconds before the edge's deadline, at which
    /// the policy reached its flush.
    private(set) var flushed: (afterEdge: TimeInterval, beforeDeadline: TimeInterval)?

    var jobActive: Bool { true }
    var guestIsAndroid: Bool { true }
    var guestDisplayActive: Bool? { true }
    var automationHoldDelay: TimeInterval { 0 }
    var pictureInPictureCanStart: Bool { false }
    var pictureInPictureActive: Bool { false }

    init(gate: any GuestGPUGate, edge: DispatchTime) {
        self.edge = edge
        gpuHold = GuestGPUHold(gate: gate)
    }

    func flushGuest(until deadline: DispatchTime?) {
        let now = DispatchTime.now().uptimeNanoseconds
        let after = Double(now - edge.uptimeNanoseconds) / 1e9
        let before = Double(Int64(clamping: deadline?.uptimeNanoseconds ?? 0) -
            Int64(clamping: now)) / 1e9
        flushed = (after, before)
    }

    func waitForGuestDisplayOff(until deadline: DispatchTime) -> Bool {
        while DispatchTime.now() < deadline {
            Thread.sleep(forTimeInterval: 0.001)
        }
        return false
    }

    func quiesceGuest(reason: String, until deadline: DispatchTime?) {}
    func resumeGuest() {}
    func setPageCacheTrimming(_ enabled: Bool) {}
    func setBackgroundThrottle(_ enabled: Bool) {}
    func pressPowerKey() {}
    func stopPictureInPicture() {}
    func keepScheduled(named name: String) -> any BackgroundCancellable { Token() }
    func schedule(
        after seconds: TimeInterval,
        _ action: @escaping @MainActor () -> Void
    ) -> any BackgroundCancellable { Token() }

    private final class Token: BackgroundCancellable {
        func cancel() {}
    }
}

/// The edge's clock, from the edge: what the application's four-second budget
/// is measured against, with the bound's own timer (the production one) and the
/// main path that waits for the screen first.
@MainActor
struct BackgroundEdgeTimingTests {
    /// An Android job, a display that stays on, a camera video that never
    /// stops and one GPU request that never ends: the gate closes when the
    /// bound says (1.5 s after the edge), not after another camera wait, and
    /// the main path reaches its flush inside the edge's four seconds.
    @Test
    func theBoundClosesTheGateAtItsTimeAndTheFlushIsInsideTheEdge() {
        let edge = DispatchTime.now()
        let gate = StuckRuntimeGate(edge: edge)
        let effects = EdgeEffects(gate: gate, edge: edge)
        let policy = BackgroundController(effects: effects, mode: { .keepRunning })

        policy.didEnterBackground()

        let closed = gate.firstClosure
        #expect(closed != nil && closed! >= 1.4 && closed! <= 1.5 + 0.15)
        // The bound's close and its drain end 1 s later, and the main path,
        // whichever took its hold first, is back by then.
        let flushed = effects.flushed
        #expect(flushed != nil)
        #expect((flushed?.beforeDeadline ?? -1) > 0)
        #expect((flushed?.afterEdge ?? 9) <= 1.5 + 1 + 0.3)
    }
}
