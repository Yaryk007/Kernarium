// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCoreTestSupport
import Testing

@testable import AndariumCore

struct NetworkAccessMappingTests {
    @Test func aSatisfiedPathIsAllowed() {
        #expect(NetworkPathReport(status: .satisfied).access == .allowed)
        // A reason on a satisfied path means nothing.
        #expect(NetworkPathReport(status: .satisfied, reason: .cellularDenied).access == .allowed)
    }

    @Test func onlyCellularAndWifiDenialsAreDenials() {
        let reasons: [(NetworkPathReport.Reason?, NetworkAccess)] = [
            (.cellularDenied, .denied(.cellular)),
            (.wifiDenied, .denied(.wifi)),
            // Airplane Mode, no signal, a VPN that is not up and the Local
            // Network permission are not this notice.
            (.notAvailable, .unavailable),
            (.localNetworkDenied, .unavailable),
            (.vpnInactive, .unavailable),
            (nil, .unavailable),
        ]
        for (reason, access) in reasons {
            #expect(NetworkPathReport(status: .unsatisfied, reason: reason).access == access, "\(String(describing: reason))")
        }
    }

    @Test func aPathThatNeedsAConnectionFirstIsNotADenial() {
        #expect(NetworkPathReport(status: .requiresConnection).access == .unavailable)
        #expect(NetworkPathReport(status: .requiresConnection, reason: .cellularDenied).access == .unavailable)
    }
}

struct NetworkNoticeTests {
    @Test func aDenialShowsOneNoticeUntilThePathIsAllowed() {
        var notice = NetworkNotice()
        #expect(!notice.isShowing && notice.showing == nil)
        notice.apply(.denied(.cellular))
        #expect(notice.showing == .cellular)
        // The same denial reported again changes nothing.
        let before = notice
        notice.apply(.denied(.cellular))
        #expect(notice == before)
        notice.apply(.allowed)
        #expect(!notice.isShowing && notice.denial == nil)
    }

    @Test func aDismissedDenialDoesNotComeBackUntilTheNetworkWasAllowed() {
        var notice = NetworkNotice()
        notice.apply(.denied(.wifi))
        notice.dismiss()
        #expect(!notice.isShowing && notice.denial == .wifi)
        // Reported again, or through a moment with no network at all.
        notice.apply(.denied(.wifi))
        #expect(!notice.isShowing)
        notice.apply(.unavailable)
        notice.apply(.denied(.wifi))
        #expect(!notice.isShowing)
        // Allowed in between: the next denial is a new notice.
        notice.apply(.allowed)
        notice.apply(.denied(.wifi))
        #expect(notice.showing == .wifi)
    }

    @Test func aDenialByTheOtherCauseIsAnotherNotice() {
        var notice = NetworkNotice()
        notice.apply(.denied(.cellular))
        notice.dismiss()
        notice.apply(.denied(.wifi))
        #expect(notice.showing == .wifi)
        // And there is only ever one of them.
        notice.apply(.denied(.cellular))
        #expect(notice.showing == nil && notice.denial == .cellular)
    }

    @Test func noNetworkAtAllIsNotTheNotice() {
        var notice = NetworkNotice()
        notice.apply(.denied(.cellular))
        notice.apply(.unavailable)
        #expect(!notice.isShowing && notice.denial == nil)
        notice.dismiss()
        notice.apply(.denied(.cellular))
        #expect(notice.showing == .cellular, "a dismissal of nothing dismisses nothing")
    }
}

@MainActor
struct NetworkAccessWatcherTests {
    @Test func theSourceRunsOnlyWhileAWatchIsWanted() {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        #expect(!watcher.isWatching && source.starts == 0)
        watcher.setWanted(true)
        watcher.setWanted(true)
        #expect(watcher.isWatching && source.starts == 1 && source.stops == 0)
        watcher.setWanted(false)
        watcher.setWanted(false)
        #expect(!watcher.isWatching && source.starts == 1 && source.stops == 1)
        watcher.setWanted(true)
        #expect(source.starts == 2)
    }

    @Test func aWatcherNeverWantedNeverTouchesTheSource() {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        watcher.setWanted(false)
        #expect(source.starts == 0 && source.stops == 0)
    }

    @Test func reportsMakeAndTakeTheNotice() {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        var changes = 0
        watcher.changed = { changes += 1 }
        watcher.setWanted(true)
        source.send(.init(status: .unsatisfied, reason: .cellularDenied))
        #expect(watcher.notice.showing == .cellular && changes == 1)
        // The same report again is no change.
        source.send(.init(status: .unsatisfied, reason: .cellularDenied))
        #expect(changes == 1)
        // Airplane Mode takes it away.
        source.send(.init(status: .unsatisfied, reason: .notAvailable))
        #expect(!watcher.notice.isShowing && changes == 2)
        source.send(.init(status: .satisfied))
        #expect(!watcher.notice.isShowing && changes == 2)
    }

    @Test func dismissingTellsTheScreensOnce() {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        var changes = 0
        watcher.changed = { changes += 1 }
        watcher.dismiss()
        #expect(changes == 0, "nothing was showing")
        watcher.setWanted(true)
        source.send(.init(status: .unsatisfied, reason: .wifiDenied))
        watcher.dismiss()
        watcher.dismiss()
        #expect(!watcher.notice.isShowing && changes == 2)
    }

    @Test func aReportAfterTheStopIsDropped() throws {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        watcher.setWanted(true)
        let late = try #require(source.deliver)
        watcher.setWanted(false)
        late(.init(status: .unsatisfied, reason: .cellularDenied))
        #expect(!watcher.notice.isShowing)
    }

    @Test func aReportOfAnEarlierStartIsDroppedAfterTheNextStart() throws {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        watcher.setWanted(true)
        // A report the first monitor queued, still on its way as the source
        // is stopped and started again (the system's callback queue is not
        // the main actor's).
        let late = try #require(source.deliver)
        watcher.setWanted(false)
        watcher.setWanted(true)
        late(.init(status: .unsatisfied, reason: .cellularDenied))
        #expect(!watcher.notice.isShowing, "an earlier monitor's report counted for the new watch")
        // The new monitor's own reports are heard.
        source.send(.init(status: .unsatisfied, reason: .cellularDenied))
        #expect(watcher.notice.showing == .cellular)
    }

    @Test func aDenialSurvivesAStopAndStartUntilTheNextReport() {
        let source = ScriptedNetworkPathSource()
        let watcher = NetworkAccessWatcher(source: source)
        watcher.setWanted(true)
        source.send(.init(status: .unsatisfied, reason: .wifiDenied))
        watcher.dismiss()
        watcher.setWanted(false)
        watcher.setWanted(true)
        // The system's first report after the start says the same, and the
        // person's dismissal still holds.
        source.send(.init(status: .unsatisfied, reason: .wifiDenied))
        #expect(!watcher.notice.isShowing)
        source.send(.init(status: .satisfied))
        source.send(.init(status: .unsatisfied, reason: .wifiDenied))
        #expect(watcher.notice.showing == .wifi)
    }
}
