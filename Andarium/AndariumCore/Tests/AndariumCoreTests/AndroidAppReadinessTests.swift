// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A guest shell that answers each command from a table.
private final class AnswerShell: GuestShell, @unchecked Sendable {
    struct Unreachable: Error {}
    private let lock = NSLock()
    private var answers: [String: GuestShellOutput]
    private(set) var asked: [String] = []
    var reachable = true

    static let homeAnswer =
        "priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\ncom.android.launcher3/com.android.launcher3.uioverrides.QuickstepLauncher\n"
    static let launcherResumed = """
        ACTIVITY MANAGER ACTIVITIES (dumpsys activity activities)
          topResumedActivity=ActivityRecord{1a2b3c u0 com.android.launcher3/.uioverrides.QuickstepLauncher t5}
          ResumedActivity: ActivityRecord{1a2b3c u0 com.android.launcher3/.uioverrides.QuickstepLauncher t5}
        """

    init(boot: String = "1", home: String = homeAnswer, setup: String? = "1", resumed: String? = launcherResumed) {
        answers = [
            AndroidAppReadiness.bootCompletedCommand: GuestShellOutput(text: boot + "\n", exitCode: 0),
            AndroidAppReadiness.homeCommand: GuestShellOutput(text: home, exitCode: 0),
        ]
        if let setup { answers[AndroidAppReadiness.setupCommand] = GuestShellOutput(text: setup + "\n", exitCode: 0) }
        if let resumed { answers[AndroidAppReadiness.resumedCommand] = GuestShellOutput(text: resumed + "\n", exitCode: 0) }
    }

    func set(_ command: String, _ output: GuestShellOutput?) { lock.withLock { answers[command] = output } }

    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
        try lock.withLock {
            asked.append(command)
            guard reachable else { throw Unreachable() }
            return answers[command] ?? GuestShellOutput(text: "", exitCode: 127)
        }
    }
}

struct AndroidAppReadinessTests {
    @Test func androidIsReadyWhenEverythingHolds() async {
        let shell = AnswerShell()
        let state = await AndroidAppReadiness.check(shell: shell, storageReady: { true })
        #expect(state == .ready)
    }

    @Test func anAppInFrontIsReadyOnceHomeWasSeenInTheRun() async {
        let settings = """
            topResumedActivity=ActivityRecord{7f u0 com.android.settings/.Settings t9}
            ResumedActivity: ActivityRecord{7f u0 com.android.settings/.Settings t9}
            """
        // Before the run saw Home, an app in front does not say the start is over.
        let before = AnswerShell(resumed: settings)
        #expect(await AndroidAppReadiness.check(shell: before, storageReady: { true }, homeReached: false) == .notYet)
        // After it, it is the normal state of a running Android, and Home's resume is not asked again.
        let after = AnswerShell(resumed: settings)
        #expect(await AndroidAppReadiness.check(shell: after, storageReady: { true }, homeReached: true) == .ready)
        #expect(!after.asked.contains(AndroidAppReadiness.resumedCommand))
        // Every other question is still asked and still answered.
        let booting = AnswerShell(boot: "0", resumed: settings)
        #expect(await AndroidAppReadiness.check(shell: booting, storageReady: { true }, homeReached: true) == .notYet)
        let noStorage = AnswerShell(resumed: settings)
        #expect(await AndroidAppReadiness.check(shell: noStorage, storageReady: { false }, homeReached: true) == .notYet)
        let noHome = AnswerShell(home: "", resumed: settings)
        #expect(await AndroidAppReadiness.check(shell: noHome, storageReady: { true }, homeReached: true) == .notYet)
        let unfinished = AnswerShell(setup: "0", resumed: settings)
        #expect(await AndroidAppReadiness.check(shell: unfinished, storageReady: { true }, homeReached: true) == .setupUnfinished)
        let unknown = AnswerShell(setup: nil, resumed: settings)
        unknown.set(AndroidAppReadiness.setupCommand, GuestShellOutput(text: "null\n", exitCode: 0))
        #expect(await AndroidAppReadiness.check(shell: unknown, storageReady: { true }, homeReached: true) == .notYet)
    }

    @Test func theSetupIsAskedForTheUserInTheForeground() {
        #expect(AndroidAppReadiness.setupCommand == "settings --user current get secure user_setup_complete")
    }

    @Test func onlyAnAffirmativeSetupAnswerIsReady() async {
        // A query that fails, says nothing, says null, or says anything but 1 or 0 is not "ready".
        for answer in [("", 255), ("", 0), ("null\n", 0), ("2\n", 0), ("Error: no such user\n", 1), ("1\n", 1)] {
            let shell = AnswerShell(setup: nil)
            shell.set(AndroidAppReadiness.setupCommand, GuestShellOutput(text: answer.0, exitCode: answer.1))
            #expect(await AndroidAppReadiness.check(shell: shell, storageReady: { true }) == .notYet, "\(answer)")
        }
        let unanswered = AnswerShell(setup: nil)
        #expect(await AndroidAppReadiness.check(shell: unanswered, storageReady: { true }) == .notYet)
    }

    @Test func anUnfinishedSetupIsSaid() async {
        let shell = AnswerShell(setup: "0")
        #expect(await AndroidAppReadiness.check(shell: shell, storageReady: { true }) == .setupUnfinished)
    }

    @Test func eachMissingPieceIsNotYet() async {
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(boot: "0"), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(boot: ""), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(home: "No activity found"), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(), storageReady: { false }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(), storageReady: { nil }) == .notYet)
        let down = AnswerShell()
        down.reachable = false
        #expect(await AndroidAppReadiness.check(shell: down, storageReady: { true }) == .notYet)
    }

    @Test func theHomeScreenMustHaveResumedAndItsResumeIsObserved() async {
        // Another app, or nothing, resumed: not yet. The home activity resumed: ready.
        let other = "topResumedActivity=ActivityRecord{1 u0 com.android.settings/.Settings t9}"
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(resumed: other), storageReady: { true }) == .notYet)
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(resumed: nil), storageReady: { true }) == .notYet)
        // A package that only starts like the home's is not the home.
        let lookalike = "ResumedActivity: ActivityRecord{1 u0 com.android.launcher3x/.Main t9}"
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(resumed: lookalike), storageReady: { true }) == .notYet)
        // A line that is not about the resumed activity does not count: a stopped launcher is not resumed.
        let stopped = "  * ActivityRecord{1 u0 com.android.launcher3/.Main t5} state=STOPPED"
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(resumed: stopped), storageReady: { true }) == .notYet)
        // Also in the older form of the line.
        let old = "  mResumedActivity: ActivityRecord{1 u0 com.android.launcher3/.Main t5}"
        #expect(await AndroidAppReadiness.check(shell: AnswerShell(resumed: old), storageReady: { true }) == .ready)
    }

    @Test func theHomePackageIsTheLastLineOfTheResolveAnswer() {
        #expect(AndroidAppReadiness.homePackage(in: AnswerShell.homeAnswer) == "com.android.launcher3")
        #expect(AndroidAppReadiness.homePackage(in: "No activity found") == nil)
        #expect(AndroidAppReadiness.homePackage(in: "") == nil)
        #expect(AndroidAppReadiness.homePackage(in: "x; id/y") == nil)
    }

    @Test func theSetupIsOnlyAskedOnceTheRestHolds() async {
        let shell = AnswerShell(boot: "0", setup: "0")
        _ = await AndroidAppReadiness.check(shell: shell, storageReady: { true })
        #expect(shell.asked == [AndroidAppReadiness.bootCompletedCommand])
    }
}

/// A scripted sequence of looks at Android, and a clock that only the waits
/// between them advance.
private final class Script: @unchecked Sendable {
    private let lock = NSLock()
    private var states: [AndroidAppReadiness.State]
    private var clock = Duration.zero
    private var sleeps: [Duration] = []

    init(_ states: [AndroidAppReadiness.State]) { self.states = states }

    var slept: [Duration] { lock.withLock { sleeps } }
    func next() -> AndroidAppReadiness.State { lock.withLock { states.isEmpty ? .notYet : states.removeFirst() } }
    func now() -> Duration { lock.withLock { clock } }
    func sleep(_ interval: Duration) { lock.withLock { clock += interval; sleeps.append(interval) } }
}

struct AndroidAppReadinessWaitTests {
    private func run(_ states: [AndroidAppReadiness.State], limit: Duration = .seconds(30)) async -> (AndroidAppReadiness.Outcome, [Duration]) {
        let script = Script(states)
        let outcome = await AndroidAppReadiness.wait(
            check: { script.next() }, now: { script.now() }, sleep: { script.sleep($0) }, limit: limit)
        return (outcome, script.slept)
    }

    @Test func readyEndsTheWaitAtOnceNothingIsSleptForAfterIt() async {
        let (outcome, slept) = await run([.notYet, .notYet, .ready])
        #expect(outcome == .ready)
        #expect(slept == [AndroidAppReadiness.pollInterval, AndroidAppReadiness.pollInterval])
    }

    @Test func anUnfinishedSetupEndsTheWaitAtOnce() async {
        let (outcome, slept) = await run([.notYet, .setupUnfinished])
        #expect(outcome == .setupUnfinished)
        #expect(slept == [AndroidAppReadiness.pollInterval])
    }

    @Test func theWaitEndsWhenTheGuestsRunningTimeIsUp() async {
        let (outcome, slept) = await run([], limit: .seconds(2))
        #expect(outcome == .timedOut)
        // Five looks, 400 ms apart, bring the clock to the limit.
        #expect(slept.count == 5)
        let (late, _) = await run([.notYet, .notYet, .notYet, .notYet, .ready], limit: .seconds(30))
        #expect(late == .ready)
    }

    @Test func aCancelledWaitIsTimedOut() async {
        let task = Task {
            await AndroidAppReadiness.wait(
                check: { .notYet }, now: { .zero }, sleep: { _ in try? await Task.sleep(for: .milliseconds(5)) },
                limit: .seconds(10_000))
        }
        task.cancel()
        #expect(await task.value == .timedOut)
    }
}

struct AndroidAppSearchPreferenceTests {
    private func defaults() -> UserDefaults {
        let name = "andarium-search-pref-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    @Test func searchIsOnUntilTurnedOff() {
        let defaults = defaults()
        let preference = AndroidAppSearchPreference(defaults: defaults)
        #expect(preference.isOn)
        preference.keep(false)
        #expect(!AndroidAppSearchPreference(defaults: defaults).isOn)
        preference.keep(true)
        #expect(AndroidAppSearchPreference(defaults: defaults).isOn)
    }

    @Test func whatTheIndexWasLastMadeToHoldIsKept() {
        let defaults = defaults()
        let preference = AndroidAppSearchPreference(defaults: defaults)
        #expect(preference.indexed == nil)
        preference.keepIndexed("on:abc")
        #expect(AndroidAppSearchPreference(defaults: defaults).indexed == "on:abc")
        preference.keepIndexed(nil)
        #expect(AndroidAppSearchPreference(defaults: defaults).indexed == nil)
    }
}
