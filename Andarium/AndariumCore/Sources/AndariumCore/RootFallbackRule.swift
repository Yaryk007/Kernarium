// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a start with root that ends before Android has booted does to the
/// switch, so that a ramdisk that cannot boot never traps the person. The
/// behaviour is one constant here.
public enum RootFallbackRule {
    public enum Behaviour: Sendable {
        /// Starts boot without root until the person says what to do. The
        /// switch stays on and Home marks it paused, with the two ways out:
        /// try with root again, or turn it off.
        case bootStockOnce
        /// The switch turns itself off, and Home says so.
        case turnOff
        /// Nothing changes: the failure screen of the start is all there is.
        case none
    }

    /// Starts boot without root until the person says what to do.
    public static let behaviour = Behaviour.bootStockOnce

    /// Whether a session record, read at the next launch or at the end of a
    /// run, is the end of a start with root that never finished booting: the
    /// record says the boot used the Magisk ramdisk, the guest's kernel
    /// panicked, crashed or ended the process with an error status, and the
    /// console had not said that Android booted, whether or not init had
    /// printed anything ("boot unobserved" is a failed start, as the first
    /// line of init is the first thing a ramdisk with no /init never prints).
    /// What the application asked for is no evidence, only the record of the
    /// boot that ran: a boot that restarted into recovery, one the runtime
    /// never began and a record of a runtime that does not name the ramdisk or
    /// the boot are not failed starts with root. A record still running (the
    /// system or the person ended the process), a host fault, an orderly end
    /// and a hibernation failure are ordinary and take today's paths.
    public static func endedBeforeBoot(_ record: SessionRecord) -> Bool {
        guard !record.running, record.ramdisk == .root else { return false }
        switch record.bootProgress {
        case .unfinished?, .unobserved?: break
        case .completed?, nil: return false
        }
        switch record.end {
        case "guest-panic", "kernel-abort": return true
        case "kernel-exit": return record.status != 0
        default: return false
        }
    }

    /// Whether a run that ended in the runtime's restart-loop guard never
    /// reached Android's boot completion in its last boot.
    public static func endedBeforeBoot(_ end: RunEnd, bootCompleted: Bool) -> Bool {
        end.restartedInALoop && !bootCompleted
    }

    /// Applies the behaviour after a start with root failed: returns whether
    /// the state changed.
    @discardableResult
    public static func failed(_ state: inout RootState, behaviour: Behaviour = RootFallbackRule.behaviour) -> Bool {
        state.startRooted = false
        switch behaviour {
        case .bootStockOnce:
            state.attention = .failedStart
        case .turnOff:
            state.on = false
            state.appVersionCode = nil
            state.appInstall = nil
            state.attention = .turnedOffAfterFailure
        case .none:
            return false
        }
        return true
    }

    /// A start with root the person stopped before Android booted is no
    /// failure by itself: Home offers "Start without root" for the next one.
    public static func stopped(_ state: inout RootState) {
        guard state.attention == nil else { return }
        state.attention = .stoppedStart
    }
}
