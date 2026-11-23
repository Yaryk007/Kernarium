// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

struct MonotonicClockTests {
    @Test func bothClocksCountFromTheFirstReadingAndOnlyForward() async throws {
        let continuous = MonotonicClock.continuousSeconds()
        let suspending = MonotonicClock.suspendingSeconds()
        try await Task.sleep(for: .milliseconds(60))
        let continuousLater = MonotonicClock.continuousSeconds()
        let suspendingLater = MonotonicClock.suspendingSeconds()
        // Not the system boot time (hours or days), which is what the readings replace.
        #expect(continuous >= 0 && continuous < 3600)
        #expect(suspending >= 0 && suspending < 3600)
        #expect(continuousLater - continuous >= 0.05)
        #expect(suspendingLater - suspending >= 0.05)
        // A sleep of the test's own length shows on both, and neither runs ahead of real time by seconds.
        #expect(continuousLater - continuous < 30)
        #expect(suspendingLater - suspending < 30)
    }
}
