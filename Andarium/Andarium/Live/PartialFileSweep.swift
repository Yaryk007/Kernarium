// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The sweep of the partial files that earlier runs' imports left in the
/// guest's internal storage. An import that the guest's abrupt end or iOS
/// ending the application cut short cannot remove its own partial file, and
/// nothing else would, so a start that owes the walk (`PartialSweepLedger`)
/// makes it after Android is up: no person, no import and no open Settings is
/// needed. A start that does not owe it sends nothing and says nothing.
///
/// The run waits for the storage to be mounted (Android reports its boot done
/// before it is, and a walk of the empty folder that stands there before would
/// find nothing and settle), then runs `sweep` once on its own task, off the
/// main actor, within the time bound of its steps. An import of this run may be
/// writing while it does, so a leftover is told from it by the mark in its
/// name, never by its age or the guest's clock (`GuestRunMark`). A failure is
/// one console line and the sweep is not tried again in the run (the record
/// stays owed, so the next run tries). A guest that restarts inside the run
/// asks again at its next boot: a sweep cut off by the restart is made, and so
/// is one that an import cut off by it now owes.
nonisolated final class PartialFileSweep: @unchecked Sendable {
    /// How long the run waits for the storage: about three minutes, which is
    /// longer than Android takes after its boot is done, and each reading is
    /// bounded by the guest's own running time.
    static let storagePolls = 36
    static let storageInterval = Duration.seconds(5)

    enum Phase: Equatable, Sendable {
        /// The boot under way has not been looked at yet, or the sweep was
        /// cut off by the guest restarting.
        case waiting
        /// Waiting for the storage, or removing.
        case running
        /// The boot under way owed nothing, or its sweep finished; a restart
        /// asks again.
        case settled
        /// A sweep failed or the run ended: no more attempts.
        case stopped
    }

    private enum Outcome {
        case removed(Int)
        case failed(String)
        case cancelled
    }

    private let owed: @Sendable () -> Bool
    private let storageReady: @Sendable () async -> Bool?
    private let sweep: @Sendable () async throws -> Int
    private let pause: @Sendable (Duration) async throws -> Void
    private let report: @Sendable (String) -> Void

    private let lock = NSLock()
    private var phase = Phase.waiting
    private var task: Task<Void, Never>?
    /// Tells an attempt that ended from the one that replaced it.
    private var generation = 0
    private var started = 0

    /// `owed` says whether the guest's storage may hold partial files to
    /// remove. `storageReady` reads whether internal storage is up (nil when
    /// it cannot be read yet) and `sweep` removes the leftovers and returns
    /// how many; `pause` waits between two readings and `report` is the
    /// console.
    init(
        owed: @escaping @Sendable () -> Bool,
        storageReady: @escaping @Sendable () async -> Bool?,
        sweep: @escaping @Sendable () async throws -> Int,
        pause: @escaping @Sendable (Duration) async throws -> Void = { try await Task.sleep(for: $0) },
        report: @escaping @Sendable (String) -> Void = { print($0) }
    ) {
        self.owed = owed
        self.storageReady = storageReady
        self.sweep = sweep
        self.pause = pause
        self.report = report
    }

    var currentPhase: Phase { lock.withLock { phase } }

    /// How many attempts the run has begun.
    var attempts: Int { lock.withLock { started } }

    /// Android reported its boot done. The first report of a boot looks at the
    /// record and begins the sweep when it owes one; any later call, while it
    /// runs or after it, does nothing.
    func guestBooted() {
        lock.withLock {
            guard phase == .waiting else { return }
            guard owed() else {
                phase = .settled
                return
            }
            phase = .running
            started += 1
            generation += 1
            let mine = generation
            task = Task.detached(priority: .utility) { [self] in
                let outcome = await attempt()
                finish(outcome, generation: mine)
            }
        }
    }

    /// The guest restarts inside the run: an attempt that has not finished is
    /// cut off, and the next boot looks at the record again.
    func guestRestarting() {
        lock.withLock {
            switch phase {
            case .running:
                task?.cancel()
                task = nil
                generation += 1
                phase = .waiting
            case .settled:
                phase = .waiting
            case .waiting, .stopped:
                break
            }
        }
    }

    /// The run is over, and so is the sweep.
    func runEnded() {
        lock.withLock {
            task?.cancel()
            task = nil
            generation += 1
            phase = .stopped
        }
    }

    private func attempt() async -> Outcome {
        var polls = 0
        while true {
            if Task.isCancelled { return .cancelled }
            if await storageReady() == true { break }
            polls += 1
            if polls >= Self.storagePolls { return .failed("the storage did not come up") }
            do { try await pause(Self.storageInterval) } catch { return .cancelled }
        }
        do {
            return .removed(try await sweep())
        } catch is CancellationError {
            return .cancelled
        } catch {
            return Task.isCancelled ? .cancelled : .failed("\(error)")
        }
    }

    private func finish(_ outcome: Outcome, generation mine: Int) {
        let current = lock.withLock { () -> Bool in
            guard generation == mine, phase == .running else { return false }
            if case .failed = outcome { phase = .stopped } else { phase = .settled }
            task = nil
            return true
        }
        guard current else { return }
        switch outcome {
        case let .removed(count) where count > 0:
            report("Andarium: removed \(count) partial file\(count == 1 ? "" : "s") that earlier imports left")
        case .removed, .cancelled:
            break
        case let .failed(reason):
            report("Andarium: could not remove the partial files that earlier imports left: \(reason)")
        }
    }
}
