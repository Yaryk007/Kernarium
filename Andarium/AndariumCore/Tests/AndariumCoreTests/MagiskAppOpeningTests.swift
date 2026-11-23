// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A guest that answers as the image's shell does, as measured on it: user 0
/// is unlocked after some asks, the resolver names the launcher activity, `am
/// start` takes or refuses it, and the focus check names the task in focus.
final class MagiskGuest: GuestShell, @unchecked Sendable {
    struct Script: Sendable {
        /// The answer to the package list.
        var presence = ""
        /// Asks of user 0's state answered `RUNNING_LOCKED` before the first
        /// `RUNNING_UNLOCKED` (`Int.max`: never).
        var lockedAsks = 0
        /// Asks of the resolver answered "No activity found".
        var unresolvedAsks = 0
        /// Starts answered with an error (`Int.max`: always).
        var refusedStarts = 0
        /// Focus checks answered with no task of the app.
        var unfocusedAsks = 0
        /// Every command of the opening throws, as a guest that cannot be asked.
        var unreachable = false
    }

    static let resolved =
        "priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=false\n"
        + "com.topjohnwu.magisk/.ui.MainActivity\n"
    static let started =
        "Starting: Intent { act=android.intent.action.MAIN cat=[android.intent.category.LAUNCHER] "
        + "cmp=com.topjohnwu.magisk/.ui.MainActivity }\n"
    static let refused =
        "Starting: Intent { act=android.intent.action.MAIN cat=[android.intent.category.LAUNCHER] "
        + "cmp=com.topjohnwu.magisk/.ui.MainActivity }\nError type 3\n"
        + "Error: Activity class {com.topjohnwu.magisk/com.topjohnwu.magisk.ui.MainActivity} does not exist.\n"
    static let focused = "  topDisplayFocusedRootTask=Task{4125ccd #6 type=standard A=10126:com.topjohnwu.magisk}\n"

    private struct Unreachable: Error {}
    private let lock = NSLock()
    private var script: Script
    private var asked: [String] = []

    init(_ script: Script = Script()) { self.script = script }

    var commands: [String] { lock.withLock { asked } }

    /// How often `command` was asked.
    func count(_ command: String) -> Int { commands.filter { $0 == command }.count }

    func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
        try lock.withLock {
            asked.append(command)
            if command == MagiskApp.probeCommand {
                return GuestShellOutput(text: script.presence, exitCode: 0)
            }
            if script.unreachable { throw Unreachable() }
            switch command {
            case MagiskApp.userStateCommand:
                if script.lockedAsks > 0 {
                    if script.lockedAsks != .max { script.lockedAsks -= 1 }
                    return GuestShellOutput(text: "RUNNING_LOCKED\n", exitCode: 0)
                }
                return GuestShellOutput(text: "RUNNING_UNLOCKED\n", exitCode: 0)
            case MagiskApp.resolveCommand:
                if script.unresolvedAsks > 0 {
                    script.unresolvedAsks -= 1
                    return GuestShellOutput(text: "No activity found\n", exitCode: 0)
                }
                return GuestShellOutput(text: Self.resolved, exitCode: 0)
            case MagiskApp.focusCommand:
                if script.unfocusedAsks > 0 {
                    script.unfocusedAsks -= 1
                    return GuestShellOutput(text: "", exitCode: 1)
                }
                return GuestShellOutput(text: Self.focused, exitCode: 0)
            default:
                guard command.hasPrefix("am start") else { return GuestShellOutput(text: "", exitCode: 0) }
                if script.refusedStarts > 0 {
                    if script.refusedStarts != .max { script.refusedStarts -= 1 }
                    return GuestShellOutput(text: Self.refused, exitCode: 1)
                }
                return GuestShellOutput(text: Self.started, exitCode: 0)
            }
        }
    }
}

/// The waits a test asked for instead of making them.
actor RecordedSleeps {
    private(set) var waits: [Duration] = []
    func add(_ duration: Duration) { waits.append(duration) }
}

/// Opening the Magisk app after the install: the commands, what each answer
/// means, and the bounded wait and retries around them.
struct MagiskAppOpeningTests {
    private let component = "com.topjohnwu.magisk/.ui.MainActivity"
    private let standard = MagiskAppOpening.Policy.standard

    private func output(_ text: String, exit: Int? = 0) -> GuestShellOutput { GuestShellOutput(text: text, exitCode: exit) }

    private func opened(_ script: MagiskGuest.Script) async -> (Bool, MagiskGuest, [Duration]) {
        let guest = MagiskGuest(script)
        let sleeps = RecordedSleeps()
        let result = await MagiskAppOpening.run(shell: guest, sleep: { await sleeps.add($0) })
        return (result, guest, await sleeps.waits)
    }

    // MARK: The commands

    @Test func theAppIsStartedByItsComponentAndNeverByTheLauncherIntent() {
        // `am start` resolves default activities only and the launcher filter has no default category.
        #expect(MagiskApp.resolveCommand.hasPrefix("cmd package resolve-activity --brief --user 0"))
        #expect(MagiskApp.resolveCommand.hasSuffix(" com.topjohnwu.magisk"))
        let start = MagiskApp.startCommand(component: component)
        #expect(start == "am start --user 0 -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n 'com.topjohnwu.magisk/.ui.MainActivity'")
        #expect(!start.contains(" -p "))
        #expect(MagiskApp.userStateCommand == "am get-started-user-state 0")
    }

    @Test func theLauncherActivityIsTheLastLineOfTheResolversAnswerForThisPackageOnly() {
        #expect(MagiskApp.launcherComponent(in: output(MagiskGuest.resolved)) == component)
        #expect(MagiskApp.launcherComponent(in: output("com.topjohnwu.magisk/com.topjohnwu.magisk.ui.MainActivity\r\n"))
            == "com.topjohnwu.magisk/com.topjohnwu.magisk.ui.MainActivity")
        #expect(MagiskApp.launcherComponent(in: output("com.topjohnwu.magisk/.ui.Main$Inner\n")) == "com.topjohnwu.magisk/.ui.Main$Inner")
        for bad in [
            "No activity found\n", "", "\n",
            "com.example.other/.Main\n", "com.topjohnwu.magisk.stub/.Main\n", "com.topjohnwu.magisk\n",
            "com.topjohnwu.magisk/\n", "com.topjohnwu.magisk/.\n", "com.topjohnwu.magisk/..Main\n",
            "com.topjohnwu.magisk/.Main.\n", "com.topjohnwu.magisk/.Ma in\n", "com.topjohnwu.magisk/.Main;id\n",
            "com.topjohnwu.magisk/.Main'\n", "com.topjohnwu.magisk/1Main\n", "com.topjohnwu.magisk/.a/b\n",
            "com.topjohnwu.magisk/-Main\n",
        ] {
            #expect(MagiskApp.launcherComponent(in: output(bad)) == nil, Comment(rawValue: bad))
        }
        #expect(MagiskApp.launcherComponent(in: output(MagiskGuest.resolved, exit: 1)) == nil)
        #expect(MagiskApp.launcherComponent(in: output(MagiskGuest.resolved, exit: nil)) == nil)
        let long = "com.topjohnwu.magisk/." + String(repeating: "a", count: 300) + "\n"
        #expect(MagiskApp.launcherComponent(in: output(long)) == nil)
    }

    @Test func aStartIsTakenOnlyWhenTheCommandEndedWellAndPrintedNoError() {
        #expect(MagiskApp.startTaken(output(MagiskGuest.started)))
        // The activity manager brought a task that was there to the front.
        #expect(MagiskApp.startTaken(output(MagiskGuest.started + "Warning: Activity not started, its current task has been brought to the front\n")))
        #expect(!MagiskApp.startTaken(output(MagiskGuest.refused, exit: 1)))
        #expect(!MagiskApp.startTaken(output(MagiskGuest.refused, exit: 0)), "the words decide as well as the status")
        #expect(!MagiskApp.startTaken(output("Starting: Intent { act=android.intent.action.MAIN }\nError: Activity not started, unable to resolve Intent { act=android.intent.action.MAIN }\n", exit: 1)))
        #expect(!MagiskApp.startTaken(output("Starting: Intent { cmp=com.topjohnwu.magisk/.ui.MainActivity }\nException occurred while executing 'start':\njava.lang.IllegalStateException: Too early to start activity.\n")))
        #expect(!MagiskApp.startTaken(output(MagiskGuest.started, exit: 255)))
        #expect(!MagiskApp.startTaken(output(MagiskGuest.started, exit: nil)))
    }

    @Test func theFocusCheckNamesThePackageOfTheTaskInFocusAndNoOtherPackage() {
        #expect(MagiskApp.hasFocus(output(MagiskGuest.focused)))
        #expect(MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{4125ccd #6 type=standard A=10126:com.topjohnwu.magisk U=0}\n")))
        #expect(MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{4125ccd #6 type=standard A=10126:com.topjohnwu.magisk")))
        #expect(!MagiskApp.hasFocus(output("")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #2 type=home A=1000:com.android.launcher3}\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #2 type=home}\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #2 type=standard A=10126:com.topjohnwu.magisk.stub}\n")))
        // The package alone in a line that is not the focus line (a task list) is no focus.
        #expect(!MagiskApp.hasFocus(output("  * Task{a891373 #9 type=standard A=10126:com.topjohnwu.magisk U=0 visible=true}\n")))
        #expect(!MagiskApp.hasFocus(output("Can't find service: activity\n", exit: 1)))
    }

    @Test func aStaleLineBesideTheCurrentRecordIsNoFocus() {
        let home = "topDisplayFocusedRootTask=Task{1 #1 type=home}\n"
        let stale = "previous_topDisplayFocusedRootTask=Task{2 #9 type=standard A=10126:com.topjohnwu.magisk}\n"
        // Home has the focus whatever order the lines come in.
        #expect(!MagiskApp.hasFocus(output(home + stale)))
        #expect(!MagiskApp.hasFocus(output(stale + home)))
        // A stale line is not a current record and counts for nothing: the app's own record is the one.
        #expect(MagiskApp.hasFocus(output(MagiskGuest.focused + "previous_topDisplayFocusedRootTask=Task{1 #1 type=home}\n")))
        #expect(!MagiskApp.hasFocus(output(stale)), "a stale line alone names no focus")
    }

    @Test func theAppsFieldMustBeInsideTheCurrentRecordsOwnValue() {
        // One line: Home is current, and the exact token belongs to the previous task after the current value closed.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #1 type=home} previous_topDisplayFocusedRootTask=Task{2 #9 A=10126:com.topjohnwu.magisk}\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #1 type=home}\tA=10126:com.topjohnwu.magisk\n")))
        #expect(!MagiskApp.hasFocus(output("  topDisplayFocusedRootTask=Task{1 #1 type=home} A=10126:com.topjohnwu.magisk}\n")))
        // The same line the other way round: the app's own value closed first, a stale field follows.
        #expect(MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 #9 type=standard A=10126:com.topjohnwu.magisk} previous_topDisplayFocusedRootTask=Task{1 #1 type=home}\n")))
        // A nested brace does not end the value early.
        #expect(MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 #9 x={1} type=standard A=10126:com.topjohnwu.magisk}\n")))
        // A value that never closes counts only when no other current key follows in it.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #1 type=home previous_topDisplayFocusedRootTask=Task{2 #9 A=10126:com.topjohnwu.magisk\n")))
        // The record is a Task value.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=A=10126:com.topjohnwu.magisk\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask= Task{2 #9 A=10126:com.topjohnwu.magisk}\n")))
        // Leading spaces and tabs, as the dump indents.
        #expect(MagiskApp.hasFocus(output("\t  \ttopDisplayFocusedRootTask=Task{2 #9 type=standard\tA=10126:com.topjohnwu.magisk}\n")))
    }

    @Test func onlyAFieldAtTheRecordsOwnDepthCountsAndNeverOneInsideANestedValue() {
        // The launcher is current; the app's token sits in a nested value that belongs to another task.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #1 type=home A=1000:com.android.launcher3 previous=Task{2 #9 A=10126:com.topjohnwu.magisk U=0}}\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #1 type=home previous=Task{2 #9 A=10126:com.topjohnwu.magisk}}\n")))
        // The real line a guest printed for the app in front.
        #expect(MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{a38ded #9 type=standard A=10126:com.topjohnwu.magisk}\n")))
        // A nested value first, then the app's own token at the record's depth.
        #expect(MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 previous=Task{1 #1 type=home A=1000:com.android.launcher3} #9 type=standard A=10126:com.topjohnwu.magisk}\n")))
        // A token that touches a nested value is no field of this record.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 #9 A=10126:com.topjohnwu.magisk{x}}\n")))
        // Two fields at the record's depth are ambiguous whatever they name, so the app is not taken to be in front.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 #9 A=1000:com.android.launcher3 A=10126:com.topjohnwu.magisk}\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 #9 A=10126:com.topjohnwu.magisk A=10126:com.topjohnwu.magisk}\n")))
        // A field of another package at the record's depth and the app's only inside a nested value.
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{2 #9 A=1000:com.android.launcher3 x=Task{3 A=10126:com.topjohnwu.magisk}}\n")))
    }

    @Test func aQueryThatDidNotSucceedIsNoFocus() {
        #expect(!MagiskApp.hasFocus(output(MagiskGuest.focused, exit: 1)))
        #expect(!MagiskApp.hasFocus(output(MagiskGuest.focused, exit: 255)))
        #expect(!MagiskApp.hasFocus(output(MagiskGuest.focused, exit: nil)))
    }

    @Test func moreThanOneCurrentRecordIsAmbiguousAndNoFocus() {
        let magisk = MagiskGuest.focused
        #expect(!MagiskApp.hasFocus(output(magisk + magisk)))
        #expect(!MagiskApp.hasFocus(output(magisk + "topDisplayFocusedRootTask=Task{1 #1 type=home}\n")))
        #expect(!MagiskApp.hasFocus(output("topDisplayFocusedRootTask=Task{1 #1 type=home}\n" + magisk)))
    }

    @Test func theAppsFieldIsTheExactUidAndPackageField() {
        for bad in [
            "topDisplayFocusedRootTask=Task{1 #1 type=standard A=abc:com.topjohnwu.magisk}\n",
            "topDisplayFocusedRootTask=Task{1 #1 type=standard A=:com.topjohnwu.magisk}\n",
            "topDisplayFocusedRootTask=Task{1 #1 type=standard XA=10126:com.topjohnwu.magisk}\n",
            "topDisplayFocusedRootTask=Task{1 #1 type=standard A=10126:other:com.topjohnwu.magisk}\n",
            "topDisplayFocusedRootTask=Task{1 #1 type=standard B=10126:com.topjohnwu.magisk}\n",
            "topDisplayFocusedRootTask=Task{1 #1 type=standard A=10126:Com.topjohnwu.magisk}\n",
            "topDisplayFocusedRootTask=Task{1 #1 type=standard com.topjohnwu.magisk}\n",
        ] {
            #expect(!MagiskApp.hasFocus(output(bad)), Comment(rawValue: bad))
        }
    }

    @Test func userZeroIsUnlockedOnlyOnItsOwnWord() {
        #expect(MagiskApp.isUnlocked(output("RUNNING_UNLOCKED\n")))
        #expect(MagiskApp.isUnlocked(output("RUNNING_UNLOCKED\r\n")))
        for other in ["RUNNING_LOCKED\n", "RUNNING_UNLOCKING\n", "BOOTING\n", "", "Error: Unknown command: get-started-user-state\n",
            "cmd: Can't find service: activity\n"] {
            #expect(!MagiskApp.isUnlocked(output(other)), Comment(rawValue: other))
        }
        #expect(!MagiskApp.isUnlocked(output("RUNNING_UNLOCKED\n", exit: 1)))
        #expect(!MagiskApp.isUnlocked(output("RUNNING_UNLOCKED\n", exit: nil)))
    }

    // MARK: The rule

    @Test func anUnlockedGuestIsAskedOnceThenTheAppIsStartedAndFoundInFocus() async {
        let (result, guest, sleeps) = await opened(.init())
        #expect(result)
        #expect(guest.commands == [
            MagiskApp.userStateCommand, MagiskApp.resolveCommand, MagiskApp.startCommand(component: component),
            MagiskApp.focusCommand,
        ])
        #expect(sleeps.isEmpty, "nothing waited for an unlocked guest")
    }

    @Test func aGuestNotUnlockedYetIsWaitedForAtItsPollIntervalAndOnlyThenOpened() async {
        let (result, guest, sleeps) = await opened(.init(lockedAsks: 3))
        #expect(result)
        #expect(sleeps == Array(repeating: standard.pollInterval, count: 3))
        let commands = guest.commands
        #expect(commands.prefix(4) == Array(repeating: MagiskApp.userStateCommand, count: 4)[...])
        #expect(commands.dropFirst(4).first == MagiskApp.resolveCommand, "nothing is opened before the unlock")
        #expect(guest.count(MagiskApp.userStateCommand) == 4)
    }

    @Test func anUnlockThatNeverComesIsWaitedForABoundedTimeThenTheOpenIsStillTried() async {
        // The tries run even then: a start while locked fails and is recorded, or succeeds. A start by
        // component already works in the gap before the unlock is reported (measured).
        let (result, guest, sleeps) = await opened(.init(lockedAsks: .max))
        #expect(result)
        #expect(guest.count(MagiskApp.userStateCommand) == standard.unlockPolls)
        #expect(sleeps == Array(repeating: standard.pollInterval, count: standard.unlockPolls - 1))
        #expect(guest.count(MagiskApp.startCommand(component: component)) == 1)
    }

    @Test func aRefusedStartIsRetriedWithinTheBoundThenGivenUpWithoutMoreAsks() async {
        let (result, guest, sleeps) = await opened(.init(refusedStarts: .max))
        #expect(!result)
        #expect(guest.count(MagiskApp.startCommand(component: component)) == standard.attempts)
        #expect(guest.count(MagiskApp.resolveCommand) == standard.attempts)
        #expect(guest.count(MagiskApp.focusCommand) == 0, "a start the guest refused is not checked for focus")
        #expect(sleeps == Array(repeating: standard.retryInterval, count: standard.attempts - 1))
        #expect(guest.count(MagiskApp.userStateCommand) == 1)
    }

    @Test func aStartThatTheGuestTakesOnTheRetryOpensTheApp() async {
        let (result, guest, sleeps) = await opened(.init(refusedStarts: 2))
        #expect(result)
        #expect(guest.count(MagiskApp.startCommand(component: component)) == 3)
        #expect(sleeps == [standard.retryInterval, standard.retryInterval])
    }

    @Test func anActivityTheResolverDoesNotKnowYetIsRetriedWithoutStartingAnything() async {
        let (result, guest, sleeps) = await opened(.init(unresolvedAsks: 2))
        #expect(result)
        #expect(guest.count(MagiskApp.resolveCommand) == 3)
        #expect(guest.count(MagiskApp.startCommand(component: component)) == 1)
        #expect(sleeps == [standard.retryInterval, standard.retryInterval])
    }

    @Test func aResolverThatNeverKnowsTheActivityGivesUpAfterTheAttempts() async {
        let (result, guest, _) = await opened(.init(unresolvedAsks: .max))
        #expect(!result)
        #expect(guest.count(MagiskApp.resolveCommand) == standard.attempts)
        #expect(!guest.commands.contains { $0.hasPrefix("am start") })
    }

    @Test func aStartThatTheGuestTookButNoTaskOfTheAppHasInFocusIsTriedAgain() async {
        let (result, guest, sleeps) = await opened(.init(unfocusedAsks: 1))
        #expect(result)
        #expect(guest.count(MagiskApp.startCommand(component: component)) == 2)
        #expect(guest.count(MagiskApp.focusCommand) == 2)
        #expect(sleeps == [standard.retryInterval])
    }

    @Test func anAppThatNeverComesToFocusIsNotOpened() async {
        let (result, guest, _) = await opened(.init(unfocusedAsks: .max))
        #expect(!result)
        #expect(guest.count(MagiskApp.focusCommand) == standard.attempts)
    }

    @Test func aGuestThatCannotBeAskedGivesUpAfterTheBoundedAsksAndDoesNotThrow() async {
        let (result, guest, sleeps) = await opened(.init(unreachable: true))
        #expect(!result)
        #expect(guest.count(MagiskApp.userStateCommand) == standard.unlockPolls)
        #expect(guest.count(MagiskApp.resolveCommand) == standard.attempts)
        #expect(sleeps.count == standard.unlockPolls - 1 + standard.attempts - 1)
    }

    @Test func aCancelledCallerAsksNothing() async {
        let guest = MagiskGuest(.init(refusedStarts: .max))
        let task = Task { () -> Bool in
            withUnsafeCurrentTask { $0?.cancel() }
            return await MagiskAppOpening.run(shell: guest, sleep: { _ in })
        }
        let result = await task.value
        #expect(!result)
        #expect(guest.commands.isEmpty)
    }

    @Test func aCancelDuringTheWaitEndsItAtTheNextLook() async {
        let guest = MagiskGuest(.init(lockedAsks: .max))
        let task = Task { () -> Bool in
            await MagiskAppOpening.run(
                shell: guest, sleep: { _ in withUnsafeCurrentTask { $0?.cancel() } })
        }
        let result = await task.value
        #expect(!result)
        #expect(guest.count(MagiskApp.userStateCommand) == 1)
        #expect(!guest.commands.contains { $0.hasPrefix("am start") })
    }

    @Test func theStandardBoundsLeaveRoomInsideTheStepsBound() {
        let policy = MagiskAppOpening.Policy.standard
        #expect(policy.unlockPolls == 15 && policy.pollInterval == .seconds(1))
        #expect(policy.attempts == 5 && policy.retryInterval == .seconds(2))
        let waits = policy.pollInterval * (policy.unlockPolls - 1) + policy.retryInterval * (policy.attempts - 1)
        #expect(waits == .seconds(22))
        #expect(waits < RootAppStep.limit / 2, "the waits are well inside the step's bound")
    }
}
