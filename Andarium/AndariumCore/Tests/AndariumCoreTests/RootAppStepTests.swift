// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The boot screen's Magisk app step: what it asks the guest, when it
/// installs, and the 45 second bound on the check and the install together.
struct RootAppStepTests {
    private let release = RootRelease(versionName: "v31.1", versionCode: 31_100, apkPath: "root/Magisk-v31.1.apk")
    private var rooted: RootState {
        var state = RootState()
        state.on = true
        return state
    }

    /// A wait that a cancel does not end and that holds no thread, so that a
    /// suite running beside others is not starved by it.
    private static func waitThatACancelDoesNotEnd(seconds: Double) async {
        await withCheckedContinuation { continuation in
            DispatchQueue.global().asyncAfter(deadline: .now() + seconds) { continuation.resume() }
        }
    }

    /// What the opening does when it has to try again: no waits are made.
    private let noWait: @Sendable (Duration) async -> Void = { _ in }

    /// Whether `entered` was bumped within `seconds` of real time: a step that
    /// never reaches the command fails its test promptly instead of waiting for ever.
    private static func waitForEntry(_ entered: RootCounter, seconds: Double = 10) async -> Bool {
        let end = ContinuousClock.now.advanced(by: .seconds(seconds))
        while ContinuousClock.now < end {
            if await entered.value > 0 { return true }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return await entered.value > 0
    }

    private func shell(_ probe: String, exit: Int = 0) -> RootScriptedShell {
        RootScriptedShell { command in
            switch command {
            case MagiskApp.probeCommand: return GuestShellOutput(text: probe, exitCode: exit)
            case MagiskApp.userStateCommand: return GuestShellOutput(text: "RUNNING_UNLOCKED\n", exitCode: 0)
            case MagiskApp.resolveCommand: return GuestShellOutput(text: MagiskGuest.resolved, exitCode: 0)
            case MagiskApp.focusCommand: return GuestShellOutput(text: MagiskGuest.focused, exitCode: 0)
            default:
                return command.hasPrefix("am start")
                    ? GuestShellOutput(text: MagiskGuest.started, exitCode: 0) : GuestShellOutput(text: "", exitCode: 0)
            }
        }
    }

    @Test func anAbsentAppIsInstalledAndOpened() async {
        let shell = shell("")
        let showed = RootCounter()
        let installed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted,
            installing: { await showed.bump() }, install: { await installed.bump() })
        guard case .installed(let state) = outcome else { Issue.record("not installed: \(outcome)"); return }
        #expect(state.appVersionCode == 31_100 && state.appEverHandled && state.on)
        let shown = await showed.value
        let done = await installed.value
        #expect(shown == 1 && done == 1)
        #expect(state.appInstall == .init(reason: .absent, installed: true, opened: true))
        #expect(shell.commands == [
            MagiskApp.probeCommand, MagiskApp.userStateCommand, MagiskApp.resolveCommand,
            MagiskApp.startCommand(component: "com.topjohnwu.magisk/.ui.MainActivity"), MagiskApp.focusCommand,
        ])
    }

    @Test func anAppThatDoesNotOpenIsRecordedAndIsNoFailure() async {
        let guest = MagiskGuest(.init(refusedStarts: .max))
        let sleeps = RecordedSleeps()
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: rooted, installing: {}, install: {},
            sleep: { await sleeps.add($0) })
        guard case .installed(let state) = outcome else { Issue.record("an installed app was reported as \(outcome)"); return }
        #expect(!outcome.isFailure)
        #expect(state.appVersionCode == 31_100 && state.appEverHandled && state.on, "the install is recorded whatever the open did")
        #expect(state.appInstall == .init(reason: .absent, installed: true, opened: false))
        #expect(guest.count(MagiskApp.startCommand(component: "com.topjohnwu.magisk/.ui.MainActivity")) == MagiskAppOpening.Policy.standard.attempts)
        let waits = await sleeps.waits
        #expect(waits.count == MagiskAppOpening.Policy.standard.attempts - 1)
    }

    @Test func aLauncherActivityTheGuestCannotNameIsARecordedFailedOpenAndNothingIsStarted() async {
        // A package whose launcher the resolver does not know: the open fails, the install stands, the start goes on.
        let guest = MagiskGuest(.init(unresolvedAsks: .max))
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: rooted, installing: {}, install: {}, sleep: noWait)
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(!outcome.isFailure)
        #expect(state.appInstall == .init(reason: .absent, installed: true, opened: false))
        #expect(!guest.commands.contains { $0.hasPrefix("am start") })
    }

    @Test func aHiddenAppIsNeitherInstalledAgainNorOpenedWithinThePeriod() async {
        // "Hide the Magisk app" renames the package: the exact package is absent, and the period's record says it was put in.
        var state = rooted
        state.appVersionCode = 31_100
        let guest = MagiskGuest(.init(presence: "package:com.example.hidden versionCode:31100\n"))
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: state, installing: { Issue.record("installed again") },
            install: { Issue.record("installed again") }, sleep: noWait)
        #expect(outcome == .notNeeded)
        #expect(guest.commands == [MagiskApp.probeCommand], "nothing is started or waited for")
    }

    @Test func anAppThatOpensOnlyAfterTheUnlockIsOpened() async {
        let guest = MagiskGuest(.init(lockedAsks: 4, refusedStarts: 1))
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: rooted, installing: {}, install: {}, sleep: noWait)
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appInstall?.opened == true)
        #expect(guest.count(MagiskApp.userStateCommand) == 5)
    }

    @Test func thePolicyTheStepIsGivenIsTheOneTheOpeningUses() async {
        let guest = MagiskGuest(.init(refusedStarts: .max))
        let few = MagiskAppOpening.Policy(
            unlockPolls: 1, pollInterval: .milliseconds(1), attempts: 2, retryInterval: .milliseconds(1),
            commandLimit: .seconds(1))
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: rooted, installing: {}, install: {}, opening: few, sleep: noWait)
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appInstall?.opened == false)
        #expect(guest.count(MagiskApp.resolveCommand) == 2)
    }

    @Test func anAppTheApplicationPutInForThisReleaseIsLeftAlone() async {
        var state = rooted
        state.appVersionCode = 31_100
        let shell = shell("package:com.topjohnwu.magisk versionCode:31100\n")
        let showed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: state,
            installing: { await showed.bump() }, install: { Issue.record("installed a current app") })
        #expect(outcome == .notNeeded)
        let shown = await showed.value
        #expect(shown == 0, "no step is shown for a boot that installs nothing")
        #expect(shell.commands == [MagiskApp.probeCommand])
    }

    @Test func aPackageTheStateDoesNotSayWePutInIsInstalledOverNotRecordedAsOurs() async {
        // The same package name and version code as the release's: only its
        // signature says whose it is, and the package manager checks that.
        for line in ["package:com.topjohnwu.magisk versionCode:31100\n"] {
            let shell = shell(line)
            let installed = RootCounter()
            let outcome = await RootAppStep.run(
                shell: shell, release: release, state: rooted,
                installing: {}, install: { await installed.bump() })
            guard case .installed(let state) = outcome else { Issue.record("\(line): \(outcome)"); continue }
            let done = await installed.value
            #expect(done == 1, Comment(rawValue: line))
            #expect(state.appInstall == .init(reason: .notRecorded, installed: true, opened: true), Comment(rawValue: line))
            #expect(state.appVersionCode == 31_100)
            #expect(!shell.commands.contains { $0.contains("uninstall") }, "the person's app is never removed")
        }
    }

    @Test func aNewerPackageTheStateDoesNotRecordIsLeftAloneAtEveryStart() async {
        // The person's newer official app, after root was turned off and on or a snapshot was restored:
        // Android would refuse the downgrade, so no install is tried and no alert would follow.
        for boot in 1...2 {
            let shell = shell("package:com.topjohnwu.magisk versionCode:31200\n")
            let showed = RootCounter()
            let outcome = await RootAppStep.run(
                shell: shell, release: release, state: rooted,
                installing: { await showed.bump() }, install: { Issue.record("a newer app was installed over") })
            #expect(outcome == .notNeeded, "boot \(boot)")
            let shown = await showed.value
            #expect(shown == 0, "boot \(boot)")
            #expect(shell.commands == [MagiskApp.probeCommand], "boot \(boot)")
        }
    }

    @Test func aSignatureMismatchFailsTheInstallAndTheReasonIsKept() async {
        struct Incompatible: Error {}
        let outcome = await RootAppStep.run(
            shell: shell("package:com.topjohnwu.magisk versionCode:31100\n"), release: release, state: rooted,
            installing: {}, install: { throw Incompatible() })
        guard case .failed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appInstall == .init(reason: .notRecorded, installed: false))
        #expect(state.appVersionCode == nil, "a failed install records no version")
    }

    @Test func anOlderAppIsUpdated() async {
        let outcome = await RootAppStep.run(
            shell: shell("package:com.topjohnwu.magisk versionCode:27000\n"), release: release, state: rooted,
            installing: {}, install: {})
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appVersionCode == 31_100)
        #expect(state.appInstall == .init(reason: .older, installed: true, opened: true))
    }

    @Test func anAppThePersonRemovedStaysRemovedWithinThePeriod() async {
        var state = rooted
        state.appVersionCode = 31_100
        let showed = RootCounter()
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: state,
            installing: { await showed.bump() }, install: { Issue.record("installed again") })
        #expect(outcome == .notNeeded)
        let shown = await showed.value
        #expect(shown == 0)
    }

    @Test func aFailedInstallEndsTheStepAsAFailure() async {
        struct Refused: Error {}
        let shell = shell("")
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted, installing: {}, install: { throw Refused() })
        #expect(outcome.isFailure)
        #expect(!shell.commands.contains { $0.hasPrefix("am start") }, "nothing is opened after a failure")
        #expect(!shell.commands.contains(MagiskApp.userStateCommand), "and no wait for the unlock is made")
    }

    @Test func theBoundEndsAnInstallThatNeverFinishes() async {
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: rooted, installing: {},
            install: { try await Task.sleep(for: .seconds(60)) }, limit: .milliseconds(200))
        #expect(outcome.isFailure)
        #expect(ContinuousClock.now - started < .seconds(10))
    }

    @Test func theBoundIsReleasedEvenWhenTheWorkIgnoresItsCancellation() async {
        // The waits are long against the bound and the checks are loose: a
        // suite that runs beside others may hold the pool for seconds.
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: shell(""), release: release, state: rooted, installing: {},
            install: { await Self.waitThatACancelDoesNotEnd(seconds: 40) },
            limit: .milliseconds(200))
        #expect(outcome.isFailure)
        #expect(ContinuousClock.now - started < .seconds(20))
    }

    @Test func openingTheAppIsInsideTheBoundAndSaysNothingOfTheInstall() async {
        // The guest is unlocked and names the launcher, so the start is reached and then hangs. The bound is
        // counted on a clock the test moves once the start is entered, so no load on this machine can end it
        // before the start is reached.
        let clock = ScriptedClock()
        let entered = RootCounter()
        let hungStart = RootScriptedShell { command in
            switch command {
            case MagiskApp.userStateCommand: return GuestShellOutput(text: "RUNNING_UNLOCKED\n", exitCode: 0)
            case MagiskApp.resolveCommand: return GuestShellOutput(text: MagiskGuest.resolved, exitCode: 0)
            default:
                if command.hasPrefix("am start") {
                    await entered.bump()
                    await Self.waitThatACancelDoesNotEnd(seconds: 40)
                }
                return GuestShellOutput(text: "", exitCode: 0)
            }
        }
        let release = release
        let state = rooted
        let clocking = clock.clocking
        let task = Task {
            await RootAppStep.run(
                shell: hungStart, release: release, state: state, installing: {}, install: {}, clocking: clocking)
        }
        let reached = await Self.waitForEntry(entered)
        if !reached { Issue.record("the hung start was never entered") }
        clock.advance(to: RootAppStep.limit)
        let started = ContinuousClock.now
        let outcome = await task.value
        guard case .installed(let learned) = outcome else { Issue.record("an installed app was reported as \(outcome)"); return }
        let enteredTimes = await entered.value
        #expect(enteredTimes == 1, "the hung start was entered once")
        #expect(learned.appInstall?.opened == false, "an open the bound ended is not an open")
        #expect(ContinuousClock.now - started < .seconds(20), "the open waited past the bound")
    }

    @Test func everyTimerOfTheStepEndsAtTheOriginalDeadlineWhateverTheClockDidMeanwhile() async {
        // Every read of the time costs a second, as a timer that starts late would find it.
        let clock = ScriptedClock(tick: .seconds(1))
        let outcome = await RootAppStep.run(
            shell: MagiskGuest(), release: release, state: rooted, installing: {}, install: {}, sleep: noWait,
            clocking: clock.clocking)
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appInstall?.opened == true)
        let deadline = clock.instant(at: .seconds(45))
        // Both timer tasks register their wait when they first run, which may be after the step returned.
        let asked = await clock.requested(atLeast: 2)
        #expect(asked.count == 2, "one timer for the check and the install, one for the opening")
        #expect(asked.allSatisfy { $0 == deadline }, "a timer was set for another instant than the step's deadline")
    }

    @Test func aSilentGuestIsReleasedWhenTheClockReachesTheDeadlineAndNotBefore() async {
        // The install takes 40 of the 45 seconds; the guest then never answers the start.
        let clock = ScriptedClock()
        let entered = RootCounter()
        let hungStart = RootScriptedShell { command in
            switch command {
            case MagiskApp.userStateCommand: return GuestShellOutput(text: "RUNNING_UNLOCKED\n", exitCode: 0)
            case MagiskApp.resolveCommand: return GuestShellOutput(text: MagiskGuest.resolved, exitCode: 0)
            default:
                if command.hasPrefix("am start") {
                    await entered.bump()
                    await Self.waitThatACancelDoesNotEnd(seconds: 40)
                }
                return GuestShellOutput(text: "", exitCode: 0)
            }
        }
        let finished = RootCounter()
        let release = release
        let state = rooted
        let clocking = clock.clocking
        let task = Task { () -> RootAppStep.Outcome in
            let outcome = await RootAppStep.run(
                shell: hungStart, release: release, state: state, installing: {},
                install: { clock.advance(to: .seconds(40)) }, clocking: clocking)
            await finished.bump()
            return outcome
        }
        guard await Self.waitForEntry(entered) else {
            Issue.record("the hung start was never entered")
            clock.advance(to: .seconds(45))
            _ = await task.value
            return
        }
        clock.advance(to: .seconds(44))
        try? await Task.sleep(for: .milliseconds(150))
        let early = await finished.value
        #expect(early == 0, "the step ended before its deadline")
        clock.advance(to: .seconds(45))
        let outcome = await task.value
        guard case .installed(let learned) = outcome else { Issue.record("\(outcome)"); return }
        #expect(learned.appInstall?.opened == false)
    }

    @Test func theWaitsOfTheOpeningAreInsideTheBoundToo() async {
        // Every try is refused and the policy waits a minute between tries: the step still ends with the bound.
        let guest = MagiskGuest(.init(lockedAsks: .max, refusedStarts: .max))
        let slow = MagiskAppOpening.Policy(
            unlockPolls: 100, pollInterval: .seconds(60), attempts: 100, retryInterval: .seconds(60),
            commandLimit: .seconds(1))
        let started = ContinuousClock.now
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: rooted, installing: {}, install: {}, limit: .seconds(1), opening: slow)
        guard case .installed(let state) = outcome else { Issue.record("\(outcome)"); return }
        #expect(state.appInstall?.opened == false)
        #expect(ContinuousClock.now - started < .seconds(20))
    }

    @Test func aBoundThatEndsDuringTheInstallOpensNothing() async {
        let guest = MagiskGuest()
        let outcome = await RootAppStep.run(
            shell: guest, release: release, state: rooted, installing: {},
            install: { try? await Task.sleep(for: .milliseconds(300)) }, limit: .milliseconds(100))
        // The install itself ran past the bound: a failure, and nothing is opened.
        #expect(outcome.isFailure)
        #expect(!guest.commands.contains { $0.hasPrefix("am start") })
    }

    @Test func aCancelledCallerIsReleasedBeforeTheWorkEnds() async {
        let answered = RootCounter()
        let slowAnswer = RootScriptedShell { _ in
            await Self.waitThatACancelDoesNotEnd(seconds: 40)
            await answered.bump()
            return GuestShellOutput(text: "", exitCode: 0)
        }
        let release = release
        let state = rooted
        let task = Task {
            await RootAppStep.run(shell: slowAnswer, release: release, state: state, installing: {}, install: {})
        }
        while slowAnswer.commands.isEmpty { try? await Task.sleep(for: .milliseconds(5)) }
        task.cancel()
        let outcome = await task.value
        let answeredBefore = await answered.value
        #expect(outcome.isFailure)
        #expect(answeredBefore == 0, "a cancelled step kept its caller until the guest answered")
    }

    @Test func aCancelledStepInstallsNothingWhenTheGuestAnswersLate() async {
        let gate = RootCounter()
        let answered = RootCounter()
        let slowAnswer = RootScriptedShell { _ in
            while await gate.value == 0 { try? await Task.sleep(for: .milliseconds(5)) }
            await answered.bump()
            return GuestShellOutput(text: "", exitCode: 0)
        }
        let installed = RootCounter()
        let release = release
        let state = rooted
        let task = Task {
            await RootAppStep.run(
                shell: slowAnswer, release: release, state: state, installing: {}, install: { await installed.bump() })
        }
        while slowAnswer.commands.isEmpty { try? await Task.sleep(for: .milliseconds(5)) }
        task.cancel()
        _ = await task.value
        await gate.bump()
        var waited = 0
        while await answered.value == 0, waited < 4_000 { try? await Task.sleep(for: .milliseconds(5)); waited += 5 }
        try? await Task.sleep(for: .milliseconds(200))
        let done = await installed.value
        #expect(done == 0)
    }

    @Test func theBoundIsFortyFiveSeconds() {
        #expect(RootAppStep.limit == .seconds(45))
    }

    @Test func aGuestThatCannotBeAskedGetsTheInstallAndItsFailureIsReported() async {
        struct Unreachable: Error {}
        let shell = RootScriptedShell { _ in throw Unreachable() }
        let outcome = await RootAppStep.run(
            shell: shell, release: release, state: rooted, installing: {}, install: { throw Unreachable() })
        #expect(outcome.isFailure)
    }

    @Test func theBootScreenHasAFifthStepOnlyForTheInstall() {
        #expect(BootSteps.count == 4 && BootSteps.countWithRootApp == 5)
        #expect(BootSteps.reportName(ofStep: BootSteps.rootAppStep, recovery: false) == "Magisk app")
        #expect(BootSteps.reportName(ofStep: 3, recovery: false) == "Startup")
    }
}

/// A clock a test moves. Each read of the time can cost `tick`, and a wait
/// ends when the test moves the time to its instant or the waiter is cancelled.
final class ScriptedClock: @unchecked Sendable {
    private struct Waiter {
        var id: Int
        var deadline: ContinuousClock.Instant
        var continuation: CheckedContinuation<Void, Never>
    }

    private let lock = NSLock()
    private let base = ContinuousClock.now
    private let tick: Duration
    private var offset = Duration.zero
    private var waiters: [Waiter] = []
    private var nextID = 0
    private var asked: [ContinuousClock.Instant] = []

    init(tick: Duration = .zero) { self.tick = tick }

    /// The instants every wait was set for, once `count` waits have registered
    /// or `seconds` of real time have passed.
    func requested(atLeast count: Int, seconds: Double = 5) async -> [ContinuousClock.Instant] {
        let end = ContinuousClock.now.advanced(by: .seconds(seconds))
        while ContinuousClock.now < end, lock.withLock({ asked.count }) < count {
            try? await Task.sleep(for: .milliseconds(5))
        }
        return lock.withLock { asked }
    }

    func instant(at time: Duration) -> ContinuousClock.Instant { base.advanced(by: time) }

    func now() -> ContinuousClock.Instant {
        lock.withLock {
            let value = base.advanced(by: offset)
            offset += tick
            return value
        }
    }

    /// Moves the time to `time` from the start and ends every wait it reaches.
    func advance(to time: Duration) {
        let due: [Waiter] = lock.withLock {
            offset = time
            let reached = base.advanced(by: time)
            let ready = waiters.filter { $0.deadline <= reached }
            waiters.removeAll { $0.deadline <= reached }
            return ready
        }
        due.forEach { $0.continuation.resume() }
    }

    func wait(until deadline: ContinuousClock.Instant) async {
        let id = lock.withLock { () -> Int in
            asked.append(deadline)
            nextID += 1
            return nextID
        }
        await withTaskCancellationHandler {
            await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
                lock.lock()
                // A cancel that came before the waiter was registered finds nobody to end, so it is seen here.
                if Task.isCancelled || deadline <= base.advanced(by: offset) {
                    lock.unlock()
                    continuation.resume()
                } else {
                    waiters.append(Waiter(id: id, deadline: deadline, continuation: continuation))
                    lock.unlock()
                }
            }
        } onCancel: {
            let waiter = lock.withLock { () -> Waiter? in
                guard let index = waiters.firstIndex(where: { $0.id == id }) else { return nil }
                return waiters.remove(at: index)
            }
            waiter?.continuation.resume()
        }
    }

    var clocking: RootAppStep.Clocking {
        RootAppStep.Clocking(now: { self.now() }, wait: { await self.wait(until: $0) })
    }
}
