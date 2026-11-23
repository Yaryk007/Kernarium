// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// When the boot screen says a start is slow: three minutes of one start (eight
/// for a guest's first start and the first after an update), counted across the
/// guest's restarts while the start is unfinished, on a monotonic clock that
/// leaves out the time the guest is held.
struct SlowStartTests {
    private let t0: TimeInterval = 1_000

    @Test func theNoticeComesAtThreeMinutesAndOnlyOnce() {
        var start = SlowStart()
        start.begin(at: t0)
        let result1 = start.update(at: t0 + 179.9)
        #expect(!result1)
        #expect(!start.showing)
        let result2 = start.update(at: t0 + 180)
        #expect(result2)
        #expect(start.showing)
        // Looking again, later, says nothing new.
        let result3 = start.update(at: t0 + 181)
        #expect(!result3)
        #expect(start.showing)
        #expect(SlowStart.threshold == 180)
    }

    @Test func noStartIsNeverSlow() {
        var start = SlowStart()
        let result4 = start.update(at: t0 + 10_000)
        #expect(!result4)
        #expect(!start.showing && start.elapsedSeconds(at: t0) == 0)
        let result5 = start.requestStop()
        #expect(!result5)
    }

    @Test func aReadingBeforeTheStartNeitherShowsTheNoticeNorCountsNegativeTime() {
        var start = SlowStart()
        start.begin(at: t0)
        #expect(start.elapsedSeconds(at: t0 - 50) == 0)
        let result6 = start.update(at: t0 - 50)
        #expect(!result6)
    }

    @Test func aRestartOfAnUnfinishedStartKeepsTheTime() {
        var start = SlowStart()
        start.begin(at: t0)
        start.guestRestarted(at: t0 + 100, startUnfinished: true)
        #expect(start.began == t0 && start.restarts == 1)
        #expect(start.elapsedSeconds(at: t0 + 150) == 150)
        // So a slow loop of restarts reaches the notice.
        start.guestRestarted(at: t0 + 170, startUnfinished: true)
        let result7 = start.update(at: t0 + 181)
        #expect(result7)
        // The notice, once up, and the stop asked for, survive the next restart.
        let result8 = start.requestStop()
        #expect(result8)
        start.guestRestarted(at: t0 + 200, startUnfinished: true)
        #expect(start.showing && start.stopping && start.began == t0)
    }

    @Test func aRestartOfARunningSessionIsANewBoot() {
        var start = SlowStart()
        start.begin(at: t0)
        start.finish()
        #expect(start.began == nil)
        // The session was running, so the restart's boot begins its own time.
        start.guestRestarted(at: t0 + 500, startUnfinished: false)
        #expect(start.began == t0 + 500 && start.restarts == 0)
        let result9 = start.update(at: t0 + 600)
        #expect(!result9)
        let result10 = start.update(at: t0 + 680)
        #expect(result10)
    }

    @Test func aRestartWithNoStartBeginsOne() {
        var start = SlowStart()
        start.guestRestarted(at: t0, startUnfinished: true)
        #expect(start.began == t0)
    }

    @Test func finishingTheBootTakesTheNoticeAway() {
        var start = SlowStart()
        start.begin(at: t0)
        start.update(at: t0 + 200)
        let result11 = start.requestStop()
        #expect(result11)
        start.finish()
        #expect(start == SlowStart())
        let result12 = start.update(at: t0 + 400)
        #expect(!result12)
    }

    @Test func aNewStartForgetsTheLastOnesNotice() {
        var start = SlowStart()
        start.begin(at: t0)
        start.update(at: t0 + 200)
        start.begin(at: t0 + 1000)
        #expect(!start.showing && !start.stopping)
        #expect(start.elapsedSeconds(at: t0 + 1010) == 10)
    }

    @Test func stopIsAskedForOnce() {
        var start = SlowStart()
        start.begin(at: t0)
        let result13 = start.requestStop()
        #expect(result13)
        let result14 = start.requestStop()
        #expect(!result14)
        #expect(start.stopping)
    }

    @Test func aFirstStartAndTheFirstAfterAnUpdateGetLongerAndAPlainNotice() {
        for kind in [BootKind.firstStart, .upgrade] {
            var start = SlowStart()
            start.begin(at: t0, kind: kind)
            #expect(start.longByNature && start.patience == SlowStart.firstStartThreshold)
            #expect(SlowStart.firstStartThreshold == 480)
            // Past the ordinary threshold nothing is said.
            let at300 = start.update(at: t0 + 300)
            let at479 = start.update(at: t0 + 479.9)
            #expect(!at300 && !at479)
            let at480 = start.update(at: t0 + 480)
            #expect(at480 && start.showing)
        }
        for kind in [BootKind.normal, .recovery, .restart] {
            var start = SlowStart()
            start.begin(at: t0, kind: kind)
            #expect(!start.longByNature && start.patience == SlowStart.threshold)
        }
    }

    /// The guest's first boot is still the first when an earlier start ended
    /// before Android booted (`InstanceFiles.firstBootPending`): an ordinary
    /// start of it is long by its nature, a Recovery start or a restart of a
    /// running guest is not.
    @Test func anOrdinaryStartOfAGuestWhoseFirstBootDidNotCompleteIsLong() {
        var start = SlowStart()
        start.begin(at: t0, kind: .normal, firstBootPending: true)
        #expect(start.longByNature && start.patience == SlowStart.firstStartThreshold)
        let at200 = start.update(at: t0 + 200)
        #expect(!at200)
        for kind in [BootKind.recovery, .restart] {
            var other = SlowStart()
            other.begin(at: t0, kind: kind, firstBootPending: true)
            #expect(!other.longByNature && other.patience == SlowStart.threshold)
        }
        var booted = SlowStart()
        booted.begin(at: t0, kind: .normal, firstBootPending: false)
        #expect(!booted.longByNature)
    }

    @Test func theKindOfTheStartSurvivesRestartsOfItButNotANewBoot() {
        var start = SlowStart()
        start.begin(at: t0, kind: .firstStart)
        start.guestRestarted(at: t0 + 100, startUnfinished: true)
        #expect(start.longByNature && start.restarts == 1)
        // A running session's restart is an ordinary boot of its own.
        start.guestRestarted(at: t0 + 900, startUnfinished: false)
        #expect(!start.longByNature && start.restarts == 0 && start.began == t0 + 900)
    }

    @Test func theNoticeCountsOnlyTheTimeOfAClockThatTheOwnerHoldsStill() {
        // The owner's clock does not move while the guest is held (a
        // background span, a pause for space): ten running seconds, then five
        // held minutes, are still ten seconds, and a reading that jumped
        // forward by itself is not time.
        var start = SlowStart()
        start.begin(at: 0)
        let first = start.update(at: 10)
        #expect(!first && start.elapsedSeconds(at: 10) == 10)
        let afterTheHold = start.update(at: 10)
        #expect(!afterTheHold && start.elapsedSeconds(at: 10) == 10)
        let running = start.update(at: 10 + 169)
        #expect(!running)
        let reached = start.update(at: 10 + 170)
        #expect(reached)
    }

    @Test func theReportNamesTheStepInEnglish() {
        let names = (0..<BootSteps.count).map { BootSteps.reportName(ofStep: $0, recovery: false) }
        #expect(names == ["System image", "Linux kernel", "Storage", "Startup"])
        #expect(BootSteps.reportName(ofStep: BootSteps.count - 1, recovery: true) == "Recovery")
    }
}
