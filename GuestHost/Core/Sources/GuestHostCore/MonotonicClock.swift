// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Seconds on a monotonic clock, for the intervals an application measures
/// itself (Caps Lock, the plug timers, a start's elapsed time). It is not
/// `ProcessInfo.systemUptime`: that reads the system boot time, a
/// required-reason API that the application's privacy manifest would have to
/// declare, and nothing here compares a reading with another clock's
/// timestamp. A reading counts from the first one the process takes.
public nonisolated enum MonotonicClock {
    private static let continuousOrigin = ContinuousClock.now
    private static let suspendingOrigin = SuspendingClock.now

    /// Runs while the device sleeps.
    public static func continuousSeconds() -> Double {
        seconds(continuousOrigin.duration(to: .now))
    }

    /// Stands still while the device sleeps, as `systemUptime` did: a start's
    /// time on the boot screen does not grow across a sleep.
    public static func suspendingSeconds() -> Double {
        seconds(suspendingOrigin.duration(to: .now))
    }

    private static func seconds(_ elapsed: Duration) -> Double {
        Double(elapsed.components.seconds) + Double(elapsed.components.attoseconds) / 1e18
    }
}
