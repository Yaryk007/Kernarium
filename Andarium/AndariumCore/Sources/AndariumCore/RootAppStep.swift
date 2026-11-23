// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The boot screen's Magisk app step: after Android has booted with root, the
/// application asks the guest whether the official Magisk app is there and puts
/// it in when it is not (`RootInstallRule`). The check, the install and the
/// opening of the app (`MagiskAppOpening`) are bounded together by `limit`;
/// past it, or on a failure, the boot goes on with root on and no app, and
/// Home says so. An app that was put in and did not open is not a failure: the
/// state records it. A cancelled caller is released at once.
public enum RootAppStep {
    /// The hard bound on the check and the install together.
    public static let limit = Duration.seconds(45)
    /// The bound on the short command that checks for the app.
    static let commandLimit = Duration.seconds(10)

    public enum Outcome: Equatable, Sendable {
        /// Nothing was installed.
        case notNeeded
        /// The app was put in; the state records it.
        case installed(RootState)
        /// The check or the install failed, or the bound passed; the state
        /// carries the install that was tried and why, when one was.
        case failed(RootState)

        public var isFailure: Bool {
            if case .failed = self { return true }
            return false
        }
    }

    /// How the bound is counted: the time now and a wait until an instant. The
    /// deadline is one instant, taken when the step begins and carried through
    /// the check, the install and the opening, so that a timer that starts late
    /// still ends at it. A test replaces both.
    public struct Clocking: Sendable {
        public var now: @Sendable () -> ContinuousClock.Instant
        public var wait: @Sendable (ContinuousClock.Instant) async -> Void

        public init(
            now: @escaping @Sendable () -> ContinuousClock.Instant,
            wait: @escaping @Sendable (ContinuousClock.Instant) async -> Void
        ) {
            self.now = now
            self.wait = wait
        }

        public static let live = Clocking(
            now: { ContinuousClock.now },
            wait: { try? await Task.sleep(until: $0, clock: ContinuousClock()) })
    }

    /// Runs the step. `installing` is told once, when an install is about to
    /// begin and not before, so that the screen adds its step only for a boot
    /// that installs. `install` puts the carried app in the guest. `opening`
    /// and `sleep` are how the app is then opened and how it waits between
    /// tries, and `clocking` how the bound is counted, which a test replaces.
    public static func run(
        shell: any GuestShell, release: RootRelease, state: RootState,
        installing: @escaping @Sendable () async -> Void,
        install: @escaping @Sendable () async throws -> Void,
        limit: Duration = RootAppStep.limit,
        opening: MagiskAppOpening.Policy = .standard,
        sleep: @escaping @Sendable (Duration) async -> Void = MagiskAppOpening.pause,
        clocking: Clocking = .live
    ) async -> Outcome {
        let attempt = Attempt(state)
        let deadline = clocking.now().advanced(by: limit)
        let outcome = await bounded(until: deadline, clocking) { () -> Outcome in
            let presence = await MagiskApp.presence(shell: shell, limit: commandLimit)
            if Task.isCancelled { return .failed(state) }
            let action = RootInstallRule.decide(
                presence: presence, carriedVersionCode: release.versionCode, state: state)
            guard action == .install else { return .notNeeded }
            let reason = RootInstallRule.reason(presence: presence, carriedVersionCode: release.versionCode)
            var tried = state
            tried.appInstall = .init(reason: reason, installed: false)
            attempt.set(tried)
            await installing()
            do {
                try await install()
            } catch {
                return .failed(tried)
            }
            var done = RootInstallRule.recorded(state, versionCode: release.versionCode)
            done.appInstall = .init(reason: reason, installed: true)
            return .installed(done)
        }
        guard let outcome else { return .failed(attempt.state) }
        // Opening the app is inside the same bound, up to the same deadline.
        // It says nothing of the install it follows but whether the app was
        // seen on the screen: an app that did not open is recorded, not failed.
        guard case .installed(var learned) = outcome, RootInstallRule.opensAppAfterInstall else { return outcome }
        var opened = false
        if clocking.now() < deadline {
            opened = await bounded(until: deadline, clocking) {
                await MagiskAppOpening.run(shell: shell, policy: opening, sleep: sleep)
            } ?? false
        }
        learned.appInstall?.opened = opened
        return .installed(learned)
    }

    /// The state with the install the step has begun, for a bound that passes
    /// before the work says.
    private final class Attempt: @unchecked Sendable {
        private let lock = NSLock()
        private var value: RootState

        init(_ state: RootState) { value = state }

        var state: RootState { lock.withLock { value } }
        func set(_ state: RootState) { lock.withLock { value = state } }
    }

    /// `work`'s value, or nil when `deadline` passes first or the caller is
    /// cancelled. The caller is released at either even if the work ignores
    /// its cancellation. The timer waits until the instant itself, not for
    /// the time left when it starts, so a late start does not move it.
    static func bounded<Value: Sendable>(
        until deadline: ContinuousClock.Instant, _ clocking: Clocking, _ work: @escaping @Sendable () async -> Value
    ) async -> Value? {
        let gate = Gate<Value>()
        let task = Task { gate.finish(await work()) }
        let timer = Task {
            await clocking.wait(deadline)
            task.cancel()
            gate.finish(nil)
        }
        defer { timer.cancel() }
        return await withTaskCancellationHandler {
            await withCheckedContinuation { gate.install($0) }
        } onCancel: {
            task.cancel()
            gate.finish(nil)
        }
    }

    /// Resumes its continuation once, with whichever finishes first, even
    /// when that came before the continuation was there.
    private final class Gate<Value: Sendable>: @unchecked Sendable {
        private let lock = NSLock()
        private var continuation: CheckedContinuation<Value?, Never>?
        private var result: Value?
        private var finished = false

        func install(_ continuation: CheckedContinuation<Value?, Never>) {
            lock.lock()
            if finished {
                let result = result
                lock.unlock()
                continuation.resume(returning: result)
                return
            }
            self.continuation = continuation
            lock.unlock()
        }

        func finish(_ value: Value?) {
            lock.lock()
            guard !finished else { lock.unlock(); return }
            finished = true
            result = value
            let continuation = continuation
            self.continuation = nil
            lock.unlock()
            continuation?.resume(returning: value)
        }
    }
}
