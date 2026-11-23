// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public enum Phase: Equatable, Sendable {
    case home
    case booting(BootKind)
    case running(StartMode)
    case failed
}

public enum HomeNotice: Equatable, Sendable {
    /// iOS ended Andarium while the guest was paused out of the foreground.
    case closedInBackground
    /// The previous session ended badly, as its session record says.
    case sessionEnded(SessionEndNotice)
}

/// Whether a start may go ahead.
public enum StartVerdict: Equatable, Sendable {
    case allowed(BootKind)
    /// This process has an active or failed run and cannot start another.
    case relaunchRequired
    case noGuest
    /// The device's GPU cannot draw the guest (`HostDevice.runsGuestGraphics`).
    case needsNewerChip
    /// The host has less free space than Android's own writes need.
    case lowSpace(neededBytes: UInt64, freeBytes: UInt64)
    /// An update did not complete and this start cannot try it again (it is
    /// Recovery, or this Android is not the one updating): Restore or Erase
    /// is the way on.
    case unfinishedUpdate
    /// The guest cannot start with this Android; erasing it is the way out.
    case blocked(StartBlock)
}

/// A clean power-off returns to Home. A failed run requires a fresh process;
/// a restart Android asks for happens inside the run.
public struct Lifecycle: Equatable, Sendable {
    /// Andarium holds at most one guest.
    public var guest: Guest?
    /// Moves only through the methods below in the application; the preview
    /// and the tests set it to reach a screen directly.
    public var phase: Phase = .home
    public var notice: HomeNotice?
    /// The capacity the start now booting grew the guest's storage from, for
    /// its boot screen; nil when it grew nothing. Never stored.
    public var grewFromGiB: Int?

    public init(guest: Guest?) {
        self.guest = guest
    }

    public mutating func create(_ guest: Guest) {
        self.guest = guest
        notice = nil
    }

    /// Erase guest: nothing is left of the guest, its choices included, and
    /// the application is where it was before the first one was set up.
    public mutating func erase() {
        guest = nil
        notice = nil
    }

    /// Android's own factory reset from Recovery: its data erased from
    /// inside, the guest and its choices kept.
    public mutating func factoryReset() {
        guard let guest else { return }
        self.guest = guest.erased
    }

    /// `plan` is what the instance on disk says a start would be. Start
    /// tries an unfinished update again when it can; Recovery and a start
    /// that cannot wait for Restore or Erase.
    public func startVerdict(
        _ mode: StartMode, host: HostDevice, plan: StartPlan = .normal
    ) -> StartVerdict {
        guard phase == .home else { return .relaunchRequired }
        guard host.runsGuestGraphics else { return .needsNewerChip }
        guard guest != nil else { return .noGuest }
        guard host.hasRoom(for: Limits.startReserveBytes) else {
            return .lowSpace(neededBytes: Limits.startReserveBytes, freeBytes: host.freeStorageBytes)
        }
        switch plan {
        case .blocked(let reason):
            return .blocked(reason)
        case .unfinishedUpdate:
            return plan.allowsStart(mode) ? .allowed(.upgrade) : .unfinishedUpdate
        case .firstStart, .normal, .update:
            return .allowed(plan.bootKind(mode))
        }
    }

    /// Starts the guest if the host allows it; the verdict says why not. An
    /// allowed start is handed to the runtime next, which grows the data disk
    /// to the capacity the start carries, Recovery included: a pending growth
    /// applies here (`Guest.applyPendingGrowth`), and the caller saves the
    /// guest before it hands the start over.
    @discardableResult
    public mutating func start(
        _ mode: StartMode, host: HostDevice, plan: StartPlan = .normal
    ) -> StartVerdict {
        let verdict = startVerdict(mode, host: host, plan: plan)
        if case let .allowed(kind) = verdict {
            phase = .booting(kind)
            grewFromGiB = guest?.applyPendingGrowth()
        }
        return verdict
    }

    /// The boot reached Android (or Recovery).
    public mutating func finishBoot(_ kind: BootKind) {
        grewFromGiB = nil
        phase = .running(kind == .recovery ? .recovery : .normal)
    }

    public mutating func powerOff() {
        grewFromGiB = nil
        phase = .home
    }

    public mutating func fail() {
        grewFromGiB = nil
        phase = .failed
    }

    /// Android's own Restart boots the guest again in place.
    public mutating func restart() {
        grewFromGiB = nil
        phase = .booting(.restart)
    }
}

/// How a run ended, from the runtime's result.
public struct RunEnd: Equatable, Sendable {
    public var result: Int32
    /// The runtime stopped a guest that kept restarting: five times within
    /// seconds of booting, or eight times before the boot finished.
    public var restartedInALoop: Bool
    /// Why the run could not begin, when it did not.
    public var startFailure: String?
    /// The person stopped the run from the boot screen's notice. The end is
    /// still the runtime's (a host stop of an Android guest ends the process's
    /// guest for good), so `phase` does not change; the screen that follows
    /// says whose choice it was.
    public var stoppedByUser: Bool
    /// The size of memory, in MiB, that this device would not give the
    /// virtual machine: the run ended before its kernel started, because the
    /// host could not map the guest's memory (the runtime's own result,
    /// `KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED`), or the size was one
    /// the runtime does not admit. A smaller size starts, so the failure
    /// screen names the cause and what to change.
    public var memoryRefusedMiB: Int?

    public init(
        result: Int32, restartedInALoop: Bool = false, startFailure: String? = nil, stoppedByUser: Bool = false,
        memoryRefusedMiB: Int? = nil
    ) {
        self.result = result
        self.restartedInALoop = restartedInALoop
        self.startFailure = startFailure
        self.stoppedByUser = stoppedByUser
        self.memoryRefusedMiB = memoryRefusedMiB
    }

    public var poweredOff: Bool { result == 0 && !restartedInALoop && startFailure == nil }

    /// Whether a size below the refused one is offered, so that the advice to
    /// lower the memory can be followed. `leastOfferedMiB` is the bottom of
    /// the memory ruler (`MemoryPolicy.minimumMiB`).
    public func canLowerMemory(leastOfferedMiB: Int) -> Bool {
        guard let memoryRefusedMiB else { return false }
        return memoryRefusedMiB > leastOfferedMiB
    }

    public var phase: Phase { poweredOff ? .home : .failed }

    /// The lines the failure screen offers to copy, in the person's language.
    public func report(
        appVersion: String, system: SystemInfo, guest: Guest?, uptimeSeconds: Int?
    ) -> String {
        var lines = [
            "Andarium \(appVersion) · \(system.release)",
            "\(system.kernel) · \(Formats.vcpus(guest?.cores ?? 0)) · \(Formats.size(mib: guest?.memoryMiB ?? 0))",
        ]
        let reason = if let startFailure {
            String(localized: "it could not start (\(startFailure))", bundle: .module)
        } else if let memoryRefusedMiB {
            String(
                localized: "it could not start (this device could not give it \(Formats.size(mib: memoryRefusedMiB)) of memory)",
                bundle: .module)
        } else if stoppedByUser {
            String(localized: "it was stopped from the boot screen", bundle: .module)
        } else if restartedInALoop {
            String(localized: "the virtual machine kept restarting", bundle: .module)
        } else {
            String(localized: "runtime result \(Int(result))", bundle: .module)
        }
        // A run the host refused for its memory never began, so it did not run
        // for any time, whatever the caller measured from its start.
        if let uptimeSeconds, memoryRefusedMiB == nil {
            lines.append(String(
                localized: "Virtual machine ended: \(reason) after \(Formats.elapsed(uptimeSeconds))", bundle: .module))
        } else {
            lines.append(String(localized: "Virtual machine ended: \(reason)", bundle: .module))
        }
        return lines.joined(separator: "\n")
    }
}
