// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What device motion tells the magnetic field's source, as Core Motion's
/// handler turns it.
public enum MagneticFieldEvent: Sendable, Equatable {
    /// A calibrated field in the device's own axes, in microtesla, with
    /// Android's status for its accuracy.
    case reading(x: Double, y: Double, z: Double, status: UInt8)
    /// Core Motion's one report that the device must be moved for the field
    /// to be calibrated (`CMErrorDeviceRequiresMovement`). It is no failure:
    /// the readings follow once the device is moved.
    case needsMovement
    /// Any other error: device motion cannot be read. `reason` is for a log.
    case failed(reason: String)
}

/// What the source does to Core Motion's device motion, which a test replaces.
/// Events arrive on a queue of the backend's own, one at a time.
public protocol MagneticFieldBackend: AnyObject, Sendable {
    /// Starts device motion in the magnetic-north frame at `interval`.
    func begin(interval: TimeInterval, events: @escaping @Sendable (MagneticFieldEvent) -> Void)
    /// Gives device motion that runs another interval, without a restart.
    func retime(interval: TimeInterval)
    /// Stops device motion.
    func end()
}

/// What the source says about itself for a log.
public enum MagneticFieldNote: Sendable, Equatable {
    case needsMovement
    /// Nothing was delivered in the bound, and no movement was asked for.
    case silent
    case failed(reason: String)
}

/// The magnetic field of one device, as the session starts and stops it: a
/// run of device motion, the decisions about what it delivers, and when it has
/// failed. It is the part of the device's magnetometer that decides, apart
/// from Core Motion so that each decision can be reached by a test.
///
/// - A reading of an older start, or after a stop, is not delivered, and
///   neither is a field that cannot be sent (not finite). Only a field that
///   can be sent counts as the start having delivered.
/// - A start that delivers nothing within `silence` is a failure, unless Core
///   Motion has said the device must be moved (`.needsMovement`): then the
///   readings are waited for, and a still device is not a broken one.
/// - Any other error is a failure, told once however often Core Motion says it.
/// - Starting it while it runs only retimes it.
/// - Only a source that started device motion stops it, so a second source
///   (another device object sharing the process's one manager) cannot stop
///   the first one's.
///
/// What it hands out may still be on its way when the session ends the
/// subscription; the session checks that where it sends (`MotionSession`).
public final class MagneticFieldSource: @unchecked Sendable {
    public typealias Handler = @Sendable (_ x: Double, _ y: Double, _ z: Double, _ status: UInt8) -> Void
    public typealias Timer = @Sendable (DispatchTimeInterval, @escaping @Sendable () -> Void) -> Void

    /// How long a start may deliver nothing, without a movement notice.
    public static let silence: DispatchTimeInterval = .seconds(5)

    private struct State {
        var running = false
        var generation: UInt64 = 0
        var failed = false
        var delivered = false
        var noticed = false
        var handler: Handler?
        var failure: (@Sendable () -> Void)?
    }

    private let backend: any MagneticFieldBackend
    private let silence: DispatchTimeInterval
    private let after: Timer
    private let log: @Sendable (MagneticFieldNote) -> Void
    private let lock = NSLock()
    private var state = State()

    public init(
        backend: any MagneticFieldBackend,
        silence: DispatchTimeInterval = MagneticFieldSource.silence,
        after: @escaping Timer = { interval, block in
            DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + interval, execute: block)
        },
        log: @escaping @Sendable (MagneticFieldNote) -> Void = { _ in }
    ) {
        self.backend = backend
        self.silence = silence
        self.after = after
        self.log = log
    }

    deinit { stop() }

    /// Starts the field, or retimes it if it runs. `handler` and `failure` are
    /// the ones of the latest call.
    public func start(
        interval: TimeInterval, handler: @escaping Handler, failure: @escaping @Sendable () -> Void
    ) {
        let begun: UInt64? = lock.withLock {
            state.handler = handler
            state.failure = failure
            guard !state.running else { return nil }
            state = State(running: true, generation: state.generation &+ 1, handler: handler, failure: failure)
            return state.generation
        }
        guard let generation = begun else {
            backend.retime(interval: interval)
            return
        }
        backend.begin(interval: interval) { [weak self] event in self?.receive(event, generation: generation) }
        after(silence) { [weak self] in self?.silenceElapsed(generation: generation) }
    }

    public func stop() {
        let wasRunning = lock.withLock { () -> Bool in
            let running = state.running
            state = State(generation: state.generation &+ 1)
            return running
        }
        if wasRunning { backend.end() }
    }

    private func receive(_ event: MagneticFieldEvent, generation: UInt64) {
        switch event {
        case .needsMovement:
            let first = lock.withLock { () -> Bool in
                guard state.generation == generation, !state.noticed else { return false }
                state.noticed = true
                return true
            }
            if first { log(.needsMovement) }
        case .failed(let reason):
            let failure = lock.withLock { () -> (@Sendable () -> Void)? in
                guard state.running, state.generation == generation, !state.failed else { return nil }
                state.failed = true
                return state.failure
            }
            guard let failure else { return }
            log(.failed(reason: reason))
            failure()
        case .reading(let x, let y, let z, let status):
            guard MagneticField.isUsable(x: x, y: y, z: z) else { return }
            let handler = lock.withLock { () -> Handler? in
                guard state.running, state.generation == generation, !state.failed else { return nil }
                state.delivered = true
                return state.handler
            }
            handler?(x, y, z, status)
        }
    }

    private func silenceElapsed(generation: UInt64) {
        let failure = lock.withLock { () -> (@Sendable () -> Void)? in
            guard state.running, state.generation == generation, !state.delivered, !state.noticed, !state.failed
            else { return nil }
            state.failed = true
            return state.failure
        }
        guard let failure else { return }
        log(.silent)
        failure()
    }
}
