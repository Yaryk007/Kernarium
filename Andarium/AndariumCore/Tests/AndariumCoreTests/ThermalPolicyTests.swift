// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// What the guest is told from iOS's thermal state: nothing for nominal, fair
/// and serious, and Android's critical at critical (its frame rate cap). The
/// reported state rises at once, falls 120 s after it ended, and restarts the
/// wait when it comes back. Serious is told nothing of and so has no hold. The
/// policy is pure, so every test gives it a clock of its own.
struct ThermalPolicyTests {
    private func at(_ seconds: Double) -> UInt64 { UInt64(seconds * 1_000_000_000) }

    /// A policy the tests can call inside `#expect`.
    private final class Watch {
        private var policy = ThermalPolicy()
        var severity: AndroidThermalSeverity { policy.severity }
        var nextExpiry: UInt64? { policy.nextExpiry }
        @discardableResult func observe(_ state: HostThermalState, at now: UInt64) -> Bool {
            policy.observe(state, at: now)
        }
        @discardableResult func advance(to now: UInt64) -> Bool { policy.advance(to: now) }
        func claimAnnouncement() -> Bool { policy.claimAnnouncement() }
    }

    // MARK: The mapping

    @Test func eachHostStateHasItsAndroidSeverity() {
        #expect(HostThermalState.nominal.severity == .none)
        #expect(HostThermalState.fair.severity == .none)
        #expect(HostThermalState.serious.severity == .none)
        #expect(HostThermalState.critical.severity == .critical)
        #expect(ProcessInfoMapping.all == [.nominal, .fair, .serious, .critical])
    }

    @Test func onlyCriticalCapsTheFrameRateAndNothingBelowItDoes() {
        #expect(ThermalPolicy.capSeverity == .severe)
        #expect(HostThermalState.critical.severity >= ThermalPolicy.capSeverity)
        for state in [HostThermalState.nominal, .fair, .serious] {
            #expect(state.severity < ThermalPolicy.capSeverity, "\(state) caps the frame rate")
        }
    }

    @Test func theHostSaysNoneOrCriticalAndNothingElse() {
        let told = Set(HostThermalState.allCases.map(\.severity))
        #expect(told == [.none, .critical], "light, moderate and severe are never sent")
    }

    private enum ProcessInfoMapping {
        static var all: [HostThermalState] {
            [ProcessInfo.ThermalState.nominal, .fair, .serious, .critical].map { HostThermalState($0) }
        }
    }

    @Test func aSessionStartsAtNone() {
        let policy = Watch()
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
        #expect(!policy.observe(.nominal, at: at(1)))
        #expect(policy.severity == .none)
    }

    // MARK: Rising and falling

    @Test func fairAndSeriousChangeNothingAndHoldNothing() {
        let policy = Watch()
        #expect(!policy.observe(.fair, at: at(10)))
        #expect(!policy.observe(.serious, at: at(11)))
        #expect(!policy.observe(.nominal, at: at(12)))
        #expect(!policy.observe(.serious, at: at(13)))
        #expect(!policy.observe(.fair, at: at(14)))
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil, "a state that is not reported left a wait behind")
        #expect(!policy.advance(to: at(1000)))
        #expect(!policy.claimAnnouncement(), "a state that is not reported announced a cap")
    }

    @Test func aRiseIsReportedAtCriticalAndNowhereBelowIt() {
        let policy = Watch()
        #expect(!policy.observe(.fair, at: at(10)))
        #expect(!policy.observe(.serious, at: at(11)))
        #expect(policy.severity == .none)
        #expect(policy.observe(.critical, at: at(12)))
        #expect(policy.severity == .critical)
        #expect(!policy.observe(.critical, at: at(13)))
    }

    @Test func aStateThatBeginsCriticalIsCappedFromItsFirstReading() {
        let critical = Watch()
        #expect(critical.observe(.critical, at: at(0)))
        #expect(critical.severity == .critical)
        #expect(critical.claimAnnouncement())
    }

    @Test func aStateThatBeginsSeriousIsNotCapped() {
        let serious = Watch()
        #expect(!serious.observe(.serious, at: at(0)))
        #expect(serious.severity == .none)
        #expect(serious.nextExpiry == nil)
        #expect(!serious.claimAnnouncement())
    }

    @Test func aStateThatBeginsFairIsNotCappedUntilCritical() {
        let policy = Watch()
        #expect(!policy.observe(.fair, at: at(0)))
        #expect(policy.severity == .none)
        #expect(!policy.claimAnnouncement())
        #expect(!policy.observe(.serious, at: at(30)))
        #expect(policy.severity == .none)
        #expect(!policy.claimAnnouncement())
        #expect(policy.observe(.critical, at: at(60)))
        #expect(policy.severity == .critical)
        #expect(policy.claimAnnouncement())
    }

    @Test func seriousHasNoHoldWhateverItFollows() {
        let policy = Watch()
        policy.observe(.serious, at: at(10))
        #expect(!policy.observe(.nominal, at: at(100)), "the end of serious was held")
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
        #expect(!policy.advance(to: at(220)))
        #expect(policy.severity == .none)
    }

    @Test func theSeverityFallsOneHundredTwentySecondsAfterCriticalEnded() {
        let policy = Watch()
        policy.observe(.critical, at: at(10))
        policy.observe(.nominal, at: at(100))
        #expect(policy.severity == .critical, "the fall was not held")
        #expect(policy.nextExpiry == at(220))
        #expect(!policy.advance(to: at(219.999)))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(220)))
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
    }

    @Test func aFallFromCriticalToFairWaitsAsWellAndEndsAtNone() {
        let policy = Watch()
        policy.observe(.critical, at: at(10))
        policy.observe(.fair, at: at(100))
        #expect(policy.severity == .critical)
        policy.advance(to: at(219))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(220)))
        #expect(policy.severity == .none, "fair is not reported")
        #expect(policy.nextExpiry == nil)
    }

    @Test func aFallFromCriticalToSeriousHoldsCriticalAndThenEndsAtNone() {
        let policy = Watch()
        policy.observe(.critical, at: at(30))
        #expect(policy.observe(.serious, at: at(60)) == false, "the fall to serious was reported at once")
        #expect(policy.severity == .critical)
        #expect(policy.nextExpiry == at(180))
        #expect(!policy.advance(to: at(179.999)))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(180)))
        #expect(policy.severity == .none, "serious is told nothing of")
        #expect(policy.nextExpiry == nil)
        #expect(!policy.advance(to: at(10_000)))
    }

    @Test func aStepwiseFallHoldsCriticalForItsOwnWaitAndSeriousAddsNone() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        policy.observe(.serious, at: at(100))
        policy.observe(.fair, at: at(150))
        // Critical ended at 100; serious, which is not reported, left no wait.
        #expect(policy.severity == .critical)
        #expect(policy.nextExpiry == at(220))
        policy.advance(to: at(219.9))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(220)))
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
    }

    @Test func nominalSeriousCriticalSeriousNominalIsCappedOnlyFromCriticalToItsWaitsEnd() {
        let policy = Watch()
        #expect(!policy.observe(.nominal, at: at(0)))
        #expect(!policy.observe(.serious, at: at(10)), "serious was reported")
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
        #expect(policy.observe(.critical, at: at(40)))
        #expect(policy.severity == .critical)
        #expect(policy.nextExpiry == nil, "a state that is current has no wait")
        #expect(!policy.observe(.serious, at: at(70)), "the fall to serious was reported at once")
        #expect(policy.severity == .critical)
        // Critical ended at 70: the wait ends at 190.
        #expect(policy.nextExpiry == at(190))
        #expect(!policy.observe(.nominal, at: at(100)))
        #expect(policy.severity == .critical)
        #expect(policy.nextExpiry == at(190), "serious ending moved the wait")
        policy.advance(to: at(189.999))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(190)))
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
    }

    @Test func criticalSeriousCriticalWithinTheHoldIsOneSteadyCap() {
        let policy = Watch()
        #expect(policy.observe(.critical, at: at(0)))
        // Critical ended at 30: the wait would end at 150.
        #expect(!policy.observe(.serious, at: at(30)))
        #expect(policy.severity == .critical)
        #expect(policy.nextExpiry == at(150))
        // It comes back at 100, before the wait ends: the wait is void.
        #expect(!policy.observe(.critical, at: at(100)))
        #expect(policy.nextExpiry == nil)
        #expect(!policy.advance(to: at(150)), "the first wait still ran")
        #expect(policy.severity == .critical)
        #expect(!policy.advance(to: at(220)))
        #expect(policy.severity == .critical)
        // It ends again at 230: the fall is 120 s after that, to none.
        #expect(!policy.observe(.serious, at: at(230)))
        #expect(policy.nextExpiry == at(350))
        #expect(!policy.advance(to: at(349.999)))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(350)))
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
    }

    @Test func aFlickerAroundSeriousNeverTellsAndroidAnything() {
        let policy = Watch()
        #expect(!policy.observe(.serious, at: at(0)))
        for step in 1...20 {
            let state: HostThermalState = step % 2 == 1 ? .fair : .serious
            #expect(!policy.observe(state, at: at(Double(step) * 40)), "the severity moved at step \(step)")
        }
        #expect(policy.severity == .none)
        #expect(policy.nextExpiry == nil)
        #expect(!policy.claimAnnouncement())
    }

    @Test func aFlickerAroundCriticalIsOneSteadyCapUntilItEnds() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        var changes = 0
        for step in 1...20 {
            let state: HostThermalState = step % 2 == 1 ? .serious : .critical
            if policy.observe(state, at: at(Double(step) * 40)) { changes += 1 }
        }
        #expect(changes == 0, "the severity moved while the state flickered")
        #expect(policy.severity == .critical)
    }

    @Test func aStateThatComesBackRestartsTheWait() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        policy.observe(.nominal, at: at(50))
        // The first wait would end at 170.
        policy.observe(.critical, at: at(100))
        #expect(policy.severity == .critical)
        policy.observe(.nominal, at: at(130))
        policy.advance(to: at(170))
        #expect(policy.severity == .critical, "the first wait still ran")
        policy.advance(to: at(249.9))
        #expect(policy.severity == .critical)
        #expect(policy.advance(to: at(250)))
        #expect(policy.severity == .none)
    }

    @Test func aReadingAfterALongGapFindsTheExpiryAlreadyPassed() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        policy.observe(.nominal, at: at(10))
        // The application was away longer than the wait.
        #expect(policy.observe(.nominal, at: at(1000)))
        #expect(policy.severity == .none)
    }

    @Test func aRepeatedReadingChangesNothing() {
        let policy = Watch()
        policy.observe(.serious, at: at(0))
        for second in stride(from: 1.0, to: 60.0, by: 7) {
            #expect(!policy.observe(.serious, at: at(second)))
        }
        #expect(policy.nextExpiry == nil)
        policy.observe(.critical, at: at(100))
        for second in stride(from: 101.0, to: 160.0, by: 7) {
            #expect(!policy.observe(.critical, at: at(second)))
        }
        #expect(policy.nextExpiry == nil)
    }

    // MARK: Against a definition

    /// The severity by its definition, from the whole history: the hottest
    /// state in effect at any moment of the last 120 s.
    private func reference(history: [(start: Double, state: HostThermalState)], now: Double) -> AndroidThermalSeverity {
        var hottest = HostThermalState.nominal
        for (index, entry) in history.enumerated() {
            let end = index + 1 < history.count ? history[index + 1].start : Double.infinity
            // In effect during [start, end); the window is (now - 120, now].
            if entry.start <= now, end > now - 120, entry.state > hottest { hottest = entry.state }
        }
        return hottest.severity
    }

    private struct Generator {
        var state: UInt64
        mutating func next() -> UInt64 {
            state &+= 0x9E37_79B9_7F4A_7C15
            var value = state
            value = (value ^ (value >> 30)) &* 0xBF58_476D_1CE4_E5B9
            value = (value ^ (value >> 27)) &* 0x94D0_49BB_1331_11EB
            return value ^ (value >> 31)
        }
    }

    @Test func randomWalksAgreeWithTheDefinitionAndSayOnlyNoneOrCritical() {
        var expiriesChecked = 0
        for seed in UInt64(1)...40 {
            var random = Generator(state: seed)
            let policy = Watch()
            var history: [(start: Double, state: HostThermalState)] = [(0, .nominal)]
            var clock = 0.0
            for _ in 0..<60 {
                clock += Double(random.next() % 90) + 1
                let state = HostThermalState.allCases[Int(random.next() % 4)]
                // Look at every expiry that passed before this reading.
                while let expiry = policy.nextExpiry, Double(expiry) / 1e9 <= clock {
                    let moment = Double(expiry) / 1e9
                    policy.advance(to: expiry)
                    expiriesChecked += 1
                    #expect(policy.severity == reference(history: history, now: moment), "seed \(seed) at expiry \(moment)")
                }
                if history.last?.state != state { history.append((clock, state)) }
                policy.observe(state, at: at(clock))
                #expect(policy.severity == reference(history: history, now: clock), "seed \(seed) at \(clock)")
                #expect([.none, .critical].contains(policy.severity), "seed \(seed) at \(clock)")
                if policy.nextExpiry != nil { #expect(policy.severity == .critical, "a wait outlived the cap") }
            }
        }
        #expect(expiriesChecked > 100, "the walks never reached an expiry")
    }

    // MARK: The announcement

    @Test func theCapIsAnnouncedOnceAtTheFirstCriticalAndNotBefore() {
        let policy = Watch()
        policy.observe(.fair, at: at(0))
        #expect(!policy.claimAnnouncement(), "announced at fair")
        policy.observe(.serious, at: at(10))
        #expect(!policy.claimAnnouncement(), "announced at serious")
        #expect(!policy.claimAnnouncement(), "a refusal consumed the announcement")
        policy.observe(.critical, at: at(20))
        #expect(policy.claimAnnouncement())
        #expect(!policy.claimAnnouncement())
        policy.observe(.serious, at: at(30))
        policy.observe(.critical, at: at(40))
        #expect(!policy.claimAnnouncement(), "a second critical announced a second cap")
    }

    @Test func theCapIsNotAnnouncedAgainAfterCoolingAndHeatingAgain() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        #expect(policy.claimAnnouncement())
        policy.observe(.nominal, at: at(30))
        policy.advance(to: at(150))
        #expect(policy.severity == .none)
        policy.observe(.serious, at: at(200))
        #expect(!policy.claimAnnouncement())
        policy.observe(.critical, at: at(210))
        policy.observe(.fair, at: at(400))
        policy.observe(.critical, at: at(600))
        #expect(!policy.claimAnnouncement())
    }

    @Test func aSessionThatNeverReachesCriticalAnnouncesNothing() {
        let policy = Watch()
        for (second, state) in [
            (0.0, HostThermalState.fair), (30, .serious), (200, .nominal), (500, .fair), (550, .serious), (600, .nominal),
        ] {
            policy.observe(state, at: at(second))
            #expect(!policy.claimAnnouncement())
        }
        policy.advance(to: at(1000))
        #expect(!policy.claimAnnouncement())
    }

    @Test func aJumpStraightToCriticalIsTheFirstCap() {
        let policy = Watch()
        policy.observe(.critical, at: at(5))
        #expect(policy.claimAnnouncement())
    }

    @Test func aLaterAnnouncementWaitsForTheCallThatCanMakeItWhileTheCapHolds() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        // The caller could not announce yet and the state cooled: the cap
        // still counts for 120 s, so it is still to be announced.
        policy.observe(.nominal, at: at(20))
        #expect(policy.claimAnnouncement())
    }

    @Test func aCapThatHasFallenIsNotAnnouncedLate() {
        let policy = Watch()
        policy.observe(.critical, at: at(0))
        policy.observe(.nominal, at: at(20))
        policy.advance(to: at(140))
        #expect(policy.severity == .none)
        #expect(!policy.claimAnnouncement())
    }
}
