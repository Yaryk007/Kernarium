// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The guest's GPU request gate as the background policy drives it. The runtime
/// serializes the two calls, which may be made from any thread.
nonisolated protocol GuestGPUGate: Sendable {
    /// Closes the gate: no guest request reaches the host GPU afterwards. On
    /// an Android guest it first ends the guest's running camera video and
    /// waits (two seconds at most) until the guest has stopped it, so a
    /// recording that runs ends before the guest's GPU requests stop.
    func hold()
    /// `hold()` for a caller whose deadline leaves `left` seconds: the camera
    /// wait takes what is left of them after the second the gate itself may
    /// wait for a request that is running, and never more than it does in
    /// `hold()`. The deadline limits the camera's wait and not that second,
    /// which the gate always has.
    func hold(within left: TimeInterval)
    /// Closes the gate at once, without that wait: the hold the bound takes
    /// when the edge is already late. It also ends a wait that is pending in
    /// `hold()`, which the held gate makes of no use.
    func holdNow()
    func release()
}

nonisolated extension GuestGPUGate {
    func hold(within left: TimeInterval) { hold() }
    func holdNow() { hold() }
}

/// Runs an action once, after a delay, on a thread that is not the main
/// thread, and returns what withdraws it.
typealias GuestGPUHoldTimer =
    @Sendable (TimeInterval, @escaping @Sendable () -> Void) -> @Sendable () -> Void

/// The hold on the guest's GPU requests, which more than one path can take and
/// none may delay.
///
/// iOS revokes the host GPU a few seconds after the app enters the background,
/// and a guest request that reaches the GPU then is rejected: the Vulkan device
/// survives only the rejections the driver can see. The hold therefore has a
/// bound that is a timer of its own: it runs off the main thread, so a main
/// thread that is waiting for the guest's storage to settle, or for its screen
/// to go off, cannot hold the hold back. Whichever comes first, the path that
/// holds (`hold()`, which may wait for the guest's camera) or the bound (which
/// holds at once, `GuestGPUGate.holdNow()`), takes it; the other finds it
/// taken. A bound that fires while `hold()` waits for the camera lands the hold
/// then, and `hold()` ends its wait and finds the gate held.
nonisolated final class GuestGPUHold: @unchecked Sendable {
    private let gate: any GuestGPUGate
    private let timer: GuestGPUHoldTimer
    /// The state below. Never held across the gate's `hold()`, which may wait
    /// for the guest's camera: the bound must be able to land meanwhile.
    private let lock = NSLock()
    /// Serializes `hold()` and `release()`, which may wait on the gate, so a
    /// release never runs between a hold's start and its landing. The bound
    /// does not take it.
    private let serial = NSLock()
    /// A hold is taken or being taken.
    private var held = false
    /// The gate holds: `hold()` has landed or the bound has.
    private var gateHolds = false
    /// Which bound is armed: a bound that fires as it is withdrawn or replaced
    /// sees another number and does nothing.
    private var generation = 0
    private var withdrawBound: (@Sendable () -> Void)?

    init(
        gate: any GuestGPUGate,
        timer: @escaping GuestGPUHoldTimer = GuestGPUHold.dispatchTimer
    ) {
        self.gate = gate
        self.timer = timer
    }

    var isHeld: Bool { lock.withLock { held } }

    /// Holds the GPU now, and withdraws the bound that would have. Returns
    /// whether this call took the hold, which is not so when it was taken.
    ///
    /// The gate may wait for requests already inside the device, and for the
    /// guest's camera, so a caller that arrives while another is holding waits
    /// for it. The bound does not: it holds at once. `deadline` is the one the
    /// caller must be back inside (the edge's): the camera's wait is limited
    /// to what it leaves when the gate is asked, after any wait for another
    /// hold.
    @discardableResult
    func hold(before deadline: DispatchTime? = nil) -> Bool {
        serial.lock()
        defer { serial.unlock() }
        lock.lock()
        guard !held else {
            withdrawLocked()
            lock.unlock()
            return false
        }
        held = true
        lock.unlock()
        // The bound stays armed while the gate waits for the camera, and
        // lands the hold if it fires then. It is withdrawn once this hold has.
        if let deadline {
            let left = Double(
                Int64(clamping: deadline.uptimeNanoseconds) -
                    Int64(clamping: DispatchTime.now().uptimeNanoseconds)
            ) / 1_000_000_000
            gate.hold(within: left)
        } else {
            gate.hold()
        }
        lock.withLock {
            gateHolds = true
            withdrawLocked()
        }
        return true
    }

    /// Holds the GPU in `seconds` unless something held or released it first,
    /// on a thread of its own. `landed` runs on that thread once the bound has
    /// taken the hold.
    func arm(
        after seconds: TimeInterval,
        landed: (@Sendable () -> Void)? = nil
    ) {
        lock.lock()
        defer { lock.unlock() }
        withdrawLocked()
        let mine = generation
        withdrawBound = timer(max(seconds, 0)) { [weak self] in
            self?.bound(mine, landed)
        }
    }

    /// Withdraws the bound and releases the hold if it is taken. Returns
    /// whether it released one.
    @discardableResult
    func release() -> Bool {
        serial.lock()
        defer { serial.unlock() }
        lock.lock()
        defer { lock.unlock() }
        withdrawLocked()
        guard held else { return false }
        gate.release()
        held = false
        gateHolds = false
        return true
    }

    private func bound(_ armed: Int, _ landed: (@Sendable () -> Void)?) {
        lock.lock()
        guard generation == armed else {
            lock.unlock()
            return
        }
        withdrawBound = nil
        generation += 1
        // The bound holds at once, also when a hold is waiting for the camera:
        // that hold is not on the gate yet.
        let took = !gateHolds
        if took {
            held = true
            gateHolds = true
            gate.holdNow()
        }
        lock.unlock()
        if took { landed?() }
    }

    private func withdrawLocked() {
        generation += 1
        withdrawBound?()
        withdrawBound = nil
    }

    /// A dispatch timer on a queue of its own, at the top quality of service:
    /// the hold is what keeps the guest off a GPU that iOS is about to revoke.
    static let dispatchTimer: GuestGPUHoldTimer = { seconds, action in
        let source = DispatchSource.makeTimerSource(
            queue: DispatchQueue(
                label: "com.imxieyi.Kernarium.gpu-hold",
                qos: .userInteractive
            )
        )
        source.schedule(
            deadline: .now() + seconds,
            leeway: .milliseconds(1)
        )
        let handle = source
        source.setEventHandler {
            action()
            handle.cancel()
        }
        source.resume()
        return { handle.cancel() }
    }
}
