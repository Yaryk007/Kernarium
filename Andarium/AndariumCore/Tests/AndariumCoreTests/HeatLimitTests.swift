// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// Settings' "When hot": what the thermal session tells Android with the
/// limit off and on, when the cap is announced, what "hot now" says, and how
/// the choice is stored.
struct HeatLimitSessionTests {
    private final class Timer: @unchecked Sendable {
        private let lock = NSLock()
        private var asked: [@Sendable () -> Void] = []
        func schedule(_ work: @escaping @Sendable () -> Void) { lock.withLock { asked.append(work) } }
        func fireLatest() {
            let work = lock.withLock { asked.last }
            work?()
        }
    }

    private final class Log: @unchecked Sendable {
        private let lock = NSLock()
        private var caps = 0
        private var hot: [Bool] = []
        func cap() { lock.withLock { caps += 1 } }
        func heat(_ value: Bool) { lock.withLock { hot.append(value) } }
        var capCount: Int { lock.withLock { caps } }
        var hotChanges: [Bool] { lock.withLock { hot } }
    }

    private struct Rig {
        var session: ThermalSession
        var guest: FakeThermalGuest
        var clock: ThermalTestClock
        var timer: Timer
        var log: Log
    }

    private func rig() -> Rig {
        let guest = FakeThermalGuest()
        let clock = ThermalTestClock()
        let timer = Timer()
        let log = Log()
        let session = ThermalSession(
            opener: guest, retryDelay: .milliseconds(10), clock: { clock.read() },
            schedule: { _, work in timer.schedule(work) }, hotChanged: { log.heat($0) },
            capStarted: { log.cap() })
        return Rig(session: session, guest: guest, clock: clock, timer: timer, log: log)
    }

    private func frame(_ name: String) throws -> [UInt8] { try ThermalFixture.bytes(name) }

    /// A rig in a run, connected, with the first record read.
    private func connected(limit: Bool = true) throws -> Rig {
        let rig = rig()
        rig.session.setLimit(limit)
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.isConnected })
        #expect(rig.guest.record() == (try frame("none")))
        return rig
    }

    // MARK: Off tells Android nothing

    @Test func offTellsAndroidNothingAtSeriousAndAtCritical() throws {
        let rig = try connected(limit: false)
        for state in [HostThermalState.fair, .serious, .critical, .serious, .nominal] {
            rig.session.observe(state)
            rig.session.settle()
            #expect(!rig.guest.hasPending(), "\(state) told Android with the limit off")
            #expect(rig.session.severity == .none, "\(state)")
        }
        #expect(rig.log.capCount == 0, "a cap was announced with the limit off")
    }

    @Test func aRunThatBeginsHotWithTheLimitOffStartsWithNone() throws {
        for (state, hot) in [(HostThermalState.serious, false), (.critical, true)] {
            let rig = rig()
            rig.session.setLimit(false)
            rig.session.observe(state)
            rig.session.update(running: true, booted: false)
            #expect(rig.guest.record() == (try frame("none")), "\(state)")
            rig.session.settle()
            #expect(!rig.guest.hasPending())
            #expect(rig.log.capCount == 0)
            #expect(rig.session.isHot == hot, "\(state) hot is not \(hot)")
        }
    }

    @Test func turningTheLimitOffWhileLimitedEndsItAtOnceWithoutWaitingForTheHold() throws {
        let rig = try connected()
        rig.session.observe(.critical)
        #expect(rig.guest.record() == (try frame("critical")))
        rig.session.setLimit(false)
        #expect(rig.guest.record() == (try frame("none")))
        #expect(rig.session.severity == .none)
        // The hold has not ended, and Android already hears none.
        #expect(rig.session.isHot)
        // The cool-down's own timer then changes nothing Android hears.
        rig.session.observe(.nominal)
        rig.session.settle()
        rig.clock.advance(seconds: 130)
        rig.timer.fireLatest()
        rig.session.settle()
        #expect(!rig.guest.hasPending())
        #expect(!rig.session.isHot)
    }

    @Test func turningTheLimitOnWhileHotSendsTheCurrentSeverityAtOnce() throws {
        let rig = try connected(limit: false)
        rig.session.observe(.critical)
        rig.session.settle()
        #expect(!rig.guest.hasPending())
        rig.session.setLimit(true)
        #expect(rig.guest.record() == (try frame("critical")))
        #expect(rig.session.severity == .critical)
        // From there on the hold rules apply as ever.
        rig.session.settle()
        rig.clock.advance(seconds: 30)
        rig.session.observe(.serious)
        rig.session.settle()
        #expect(!rig.guest.hasPending(), "the fall to serious was sent at once")
        rig.clock.advance(seconds: 120)
        rig.timer.fireLatest()
        #expect(rig.guest.record() == (try frame("none")))
    }

    @Test func turningTheLimitOnAtSeriousSendsNoneAndAnnouncesNothing() throws {
        let rig = try connected(limit: false)
        rig.session.observe(.serious)
        rig.session.setLimit(true)
        // Turned on at serious: the HAL is told none again, which it already knew.
        #expect(rig.guest.record() == (try frame("none")))
        rig.session.settle()
        #expect(!rig.guest.hasPending())
        #expect(rig.session.severity == .none)
        #expect(!rig.session.isHot)
        #expect(rig.log.capCount == 0)
        #expect(rig.log.hotChanges.isEmpty)
    }

    @Test func turningTheLimitOnWhileNotHotSendsNothingAndASecondChangeSendsNothingNew() throws {
        let rig = try connected(limit: false)
        rig.session.setLimit(false)
        rig.session.setLimit(true)
        rig.session.setLimit(true)
        rig.session.settle()
        // Turned on at nominal: the HAL is told none again, which it already knew.
        #expect(rig.guest.record() == (try frame("none")))
        #expect(!rig.guest.hasPending(), "an unchanged choice sent a record")
    }

    @Test func aReconnectionWhileOffStartsWithNoneAndWhileOnWithTheSeverity() throws {
        let rig = try connected(limit: false)
        rig.session.observe(.critical)
        rig.session.restart()
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.guest.connections == 2 })
        #expect(rig.guest.record() == (try frame("none")))
        rig.session.setLimit(true)
        #expect(rig.guest.record() == (try frame("critical")))
        rig.session.restart()
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.guest.connections == 3 })
        #expect(rig.guest.record() == (try frame("critical")))
    }

    // MARK: A give-up after a connection that worked

    /// A booted run whose HAL connected, then went and stayed away: the five
    /// attempts end. The service comes back after that.
    private func givenUpAfterAConnection(limit: Bool, observing state: HostThermalState) throws -> Rig {
        let rig = rig()
        rig.session.setLimit(limit)
        rig.session.observe(state)
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.isConnected })
        _ = rig.guest.record()
        rig.guest.accepting = false
        rig.guest.dropLatest()
        #expect(thermalEventually { rig.session.hasGivenUp }, "the attempts did not end")
        rig.guest.accepting = true
        return rig
    }

    @Test(arguments: [false, true])
    func aRecoveredServiceHearsTheNewChoiceAfterAGiveUp(target: Bool) throws {
        // Android holds the severity it was last told through the outage, so
        // the new choice must reach it: turned off while it holds critical,
        // and turned on while the HAL holds none.
        let rig = try givenUpAfterAConnection(limit: !target, observing: .critical)
        rig.session.setLimit(target)
        rig.session.update(running: true, booted: true)  // LiveRuntime.setHeatLimit's own call
        #expect(thermalEventually { rig.session.isConnected }, "the choice never reached the recovered service")
        #expect(rig.guest.record() == (try frame(target ? "critical" : "none")))
        #expect(!rig.session.hasGivenUp)
    }

    @Test(arguments: [false, true])
    func aRecoveredServiceHearsTheNewChoiceWhenTheConnectionWasMadeBeforeBootCompleted(target: Bool) throws {
        // The application's own order: the run begins and the HAL connects
        // while Android is still booting; boot completion is reported later
        // on the connection that stands. That is not a new boot, so what the
        // connection did survives it.
        let rig = rig()
        rig.session.setLimit(!target)
        rig.session.observe(.critical)
        rig.session.update(running: true, booted: false)
        #expect(thermalEventually { rig.session.isConnected })
        #expect(rig.guest.record() == (try frame(target ? "none" : "critical")))
        rig.session.update(running: true, booted: true)  // boot completed, the connection stands
        rig.session.settle()
        #expect(rig.session.isConnected)
        rig.guest.accepting = false
        rig.guest.dropLatest()
        #expect(thermalEventually { rig.session.hasGivenUp }, "the attempts did not end")
        rig.guest.accepting = true
        rig.session.setLimit(target)
        rig.session.update(running: true, booted: true)  // LiveRuntime.setHeatLimit's own call
        #expect(thermalEventually { rig.session.isConnected }, "the choice never reached the recovered service")
        #expect(rig.guest.record() == (try frame(target ? "critical" : "none")))
        #expect(!rig.session.hasGivenUp)
    }

    @Test func aChangeOfTheSeverityAfterAGiveUpStartsOneNewRound() throws {
        let rig = try givenUpAfterAConnection(limit: true, observing: .nominal)
        // Nothing changes: no timer keeps trying.
        let before = rig.guest.attempts
        rig.session.settle()
        usleep(300_000)
        #expect(rig.guest.attempts == before, "an attempt was made with nothing changed")
        #expect(!rig.session.isConnected)
        rig.session.observe(.critical)
        #expect(thermalEventually { rig.session.isConnected }, "the changed severity did not start a new round")
        #expect(rig.guest.record() == (try frame("critical")))
    }

    @Test func aChangeThatTellsNothingNewStartsNoRound() throws {
        // The limit is off: the severity Android is told stays none whatever iOS says.
        let rig = try givenUpAfterAConnection(limit: false, observing: .nominal)
        let before = rig.guest.attempts
        rig.session.observe(.critical)
        rig.session.observe(.nominal)
        rig.session.settle()
        usleep(300_000)
        #expect(rig.guest.attempts == before)
        #expect(rig.session.hasGivenUp)
    }

    @Test func aNewRoundThatFailsEndsAgainAndOnlyAnotherEventStartsTheNext() throws {
        let rig = try givenUpAfterAConnection(limit: true, observing: .nominal)
        rig.guest.accepting = false
        let first = rig.guest.attempts
        rig.session.setLimit(false)
        #expect(thermalEventually { rig.guest.attempts >= first + ThermalSession.attemptsAfterBoot })
        #expect(thermalEventually { rig.session.hasGivenUp }, "the new round did not end")
        let ended = rig.guest.attempts
        usleep(300_000)
        #expect(rig.guest.attempts == ended, "an attempt was made after the round ended")
        rig.guest.accepting = true
        rig.session.setLimit(true)
        #expect(thermalEventually { rig.session.isConnected })
    }

    @Test func aHalThatNeverConnectedInThisBootStaysGivenUp() throws {
        let rig = rig()
        rig.guest.accepting = false
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.hasGivenUp })
        rig.guest.accepting = true
        let before = rig.guest.attempts
        rig.session.setLimit(false)
        rig.session.observe(.critical)
        rig.session.setLimit(true)
        rig.session.update(running: true, booted: true)
        rig.session.settle()
        usleep(300_000)
        #expect(rig.guest.attempts == before, "an absent HAL was tried again")
        #expect(rig.session.hasGivenUp && !rig.session.isConnected)
    }

    // MARK: A run begins from the present state

    @Test func aSecondRunDoesNotInheritTheHeatTheLastOneEndedWith() throws {
        let rig = rig()
        rig.session.setLimit(false)
        rig.session.observe(.critical)
        rig.session.update(running: true, booted: true)
        #expect(thermalEventually { rig.session.isConnected })
        _ = rig.guest.record()
        rig.session.update(running: false, booted: false)
        rig.session.settle()
        rig.clock.advance(seconds: 300)
        // The device cooled while the run was away. The next run starts
        // before the monitor gives its first reading, which says nominal.
        rig.session.update(running: true, booted: false)
        rig.session.observe(.nominal)
        rig.session.settle()
        #expect(thermalEventually { rig.session.isConnected })
        _ = rig.guest.record()
        rig.session.setLimit(true)
        #expect(rig.guest.record() == (try frame("none")), "the new run started cool and was told something hot")
        #expect(!rig.session.isHot, "the new run inherited a hold")
        #expect(rig.log.hotChanges.last == false)
    }

    private final class Present: @unchecked Sendable {
        private let lock = NSLock()
        private var state = HostThermalState.nominal
        var value: HostThermalState {
            get { lock.withLock { state } }
            set { lock.withLock { state = newValue } }
        }
    }

    @Test func aRunBeginsInTheStateIOSIsInWhenItBeginsEvenWithAStaleReadingBefore() throws {
        let guest = FakeThermalGuest()
        let clock = ThermalTestClock()
        let now = Present()
        let session = ThermalSession(
            opener: guest, retryDelay: .milliseconds(10), clock: { clock.read() },
            schedule: { _, _ in }, present: { now.value }, capStarted: {})
        now.value = .critical
        session.update(running: true, booted: false)
        #expect(thermalEventually { session.isConnected })
        #expect(guest.record() == (try frame("critical")), "the opening record")
        #expect(session.severity == .critical && session.isHot, "a run that begins hot is hot")
        session.update(running: false, booted: false)
        session.settle()
        // A reading from the last run's monitor arrives late, and the device has cooled.
        session.observe(.critical)
        now.value = .nominal
        clock.advance(seconds: 300)
        session.update(running: true, booted: false)
        session.settle()
        #expect(thermalEventually { session.isConnected })
        #expect(session.severity == .none && !session.isHot, "the run began in the last run's heat")
        // And a run that begins hot is hot without a reading being given.
        session.update(running: false, booted: false)
        session.settle()
        now.value = .critical
        session.update(running: true, booted: false)
        session.settle()
        #expect(session.severity == .critical && session.isHot)
        // A run that begins at serious is told nothing and is not hot.
        session.update(running: false, booted: false)
        session.settle()
        now.value = .serious
        session.update(running: true, booted: false)
        session.settle()
        #expect(session.severity == .none && !session.isHot)
    }

    // MARK: The notice

    @Test func noCapIsAnnouncedWhileOffAndTheRunAnnouncesItIfTheLimitComesOnWhileHot() throws {
        let rig = try connected(limit: false)
        rig.session.observe(.critical)
        rig.session.settle()
        #expect(rig.log.capCount == 0)
        rig.session.setLimit(true)
        #expect(thermalEventually { rig.log.capCount == 1 }, "the cap that starts when the limit comes on was not announced")
        // Once in a run, however the choice moves after that.
        rig.session.setLimit(false)
        rig.session.setLimit(true)
        rig.session.settle()
        #expect(rig.log.capCount == 1)
    }

    @Test func aCapAnnouncedBeforeTheLimitWentOffIsNotAnnouncedAgainWhenItComesOn() throws {
        let rig = try connected()
        rig.session.observe(.critical)
        #expect(thermalEventually { rig.log.capCount == 1 })
        rig.session.setLimit(false)
        rig.session.setLimit(true)
        rig.session.settle()
        #expect(rig.log.capCount == 1)
    }

    // MARK: Hot now

    @Test func hotNowFollowsThePolicyWithTheLimitOffAndOn() throws {
        for limit in [false, true] {
            let rig = try connected(limit: limit)
            #expect(!rig.session.isHot)
            rig.session.observe(.fair)
            rig.session.settle()
            #expect(!rig.session.isHot, "fair is not hot (limit \(limit))")
            rig.session.observe(.serious)
            rig.session.settle()
            #expect(!rig.session.isHot, "serious is not hot (limit \(limit))")
            rig.session.observe(.critical)
            rig.session.settle()
            #expect(rig.session.isHot, "critical (limit \(limit))")
            rig.clock.advance(seconds: 40)
            rig.session.observe(.fair)
            rig.session.settle()
            #expect(rig.session.isHot, "the hold ended early (limit \(limit))")
            rig.clock.advance(seconds: 120)
            rig.timer.fireLatest()
            rig.session.settle()
            #expect(!rig.session.isHot, "the hold did not end (limit \(limit))")
            #expect(rig.log.hotChanges == [true, false], "limit \(limit): \(rig.log.hotChanges)")
        }
    }

    @Test func theHotChangeIsToldOnceEachWayAndNotAfterTheRunEnds() throws {
        let rig = try connected(limit: false)
        rig.session.observe(.serious)
        rig.session.settle()
        #expect(rig.log.hotChanges.isEmpty, "serious said the device is held hot")
        rig.session.observe(.critical)
        rig.session.observe(.serious)
        rig.session.settle()
        #expect(rig.log.hotChanges == [true])
        rig.session.update(running: false, booted: false)
        rig.session.settle()
        #expect(rig.log.hotChanges == [true, false], "the end of the run did not say the device is no longer held hot")
        #expect(!rig.session.isHot)
    }
}

struct HeatLimitPreferenceTests {
    private func withDefaults<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-heat-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let name = directory.appendingPathComponent("defaults").path
        let defaults = UserDefaults(suiteName: name)!
        defer {
            defaults.removePersistentDomain(forName: name)
            try? FileManager.default.removeItem(at: directory)
        }
        return body(defaults)
    }

    @Test func aFreshInstallLimitsTheFrameRate() {
        withDefaults { defaults in
            let preference = HeatLimitPreference(defaults: defaults)
            #expect(preference.isOn)
            let start = preference.launch(automated: false)
            #expect(start.isOn)
            #expect(start.keepsChanges)
        }
    }

    @Test func theChoiceSurvivesANewSessionUntilTurnedBackOn() {
        withDefaults { defaults in
            HeatLimitPreference(defaults: defaults).keep(false)
            // A new process reads the same defaults.
            let again = HeatLimitPreference(defaults: defaults)
            #expect(!again.isOn)
            let person = again.launch(automated: false)
            #expect(!person.isOn && person.keepsChanges)
            again.keep(true)
            #expect(HeatLimitPreference(defaults: defaults).isOn)
        }
    }

    @Test func aHarnessLaunchRunsWithTheDefaultAndLeavesTheChoiceAlone() {
        withDefaults { defaults in
            let preference = HeatLimitPreference(defaults: defaults)
            preference.keep(false)
            let harness = preference.launch(automated: true)
            #expect(harness.isOn && !harness.keepsChanges)
            let stated = preference.launch(automated: true, stated: false)
            #expect(!stated.isOn && !stated.keepsChanges)
            let statedOn = preference.launch(automated: false, stated: true)
            #expect(statedOn.isOn && !statedOn.keepsChanges)
            #expect(!preference.isOn, "a launch changed what the person chose")
        }
    }

    @Test func theWordsAreFixedEnglish() {
        #expect(HeatLimitWords.state(isOn: true) == "on")
        #expect(HeatLimitWords.state(isOn: false) == "off")
    }
}
