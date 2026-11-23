// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// Android's screen reader in the session: the switch in Settings shows what
/// Android reports and turns it on and off through the driver; a read that a
/// change overtook never overwrites it; every read and change belongs to the
/// session it began in; and nothing turns the screen reader on but the person,
/// on the switch or on the card that asks while VoiceOver runs.
@MainActor
struct TalkBackSwitchTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
    private let talkBack = ScreenReader.services[0]
    private let other = "com.example.reader/com.example.reader.Service"

    /// A guest that ran `plan`'s start to its end, with VoiceOver as given.
    private func started(
        plan: StartPlan = .normal, voiceOver: Bool = false, mode: StartMode = .normal,
        guestState: ScreenReaderState? = nil, interval: Duration = .seconds(3)
    ) -> (AppModel, StubDriver) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: plan)
        if let guestState { driver.screenReaderState = guestState }
        model.driver = driver
        model.voiceOverRunning = { voiceOver }
        model.screenReaderWatchInterval = interval
        model.refreshPlan()
        model.start(mode)
        model.finishBoot(model.bootKind(mode))
        return (model, driver)
    }

    /// What VoiceOver is told of the model's own accord.
    private final class Said {
        var lines: [String] = []
        var queued: [Bool] = []
        var high: [Bool] = []
    }

    /// What a run's start asks VoiceOver to say, and the end of it.
    private final class RunStartSpeech {
        var lines: [String] = []
        var finish: (@MainActor @Sendable (Bool) -> Void)?
    }

    /// Listens to what the model tells VoiceOver. Call it before anything awaits:
    /// the first read of a start comes as soon as the test suspends.
    private func listen(to model: AppModel) -> Said {
        let said = Said()
        model.announcer = { text, queued, high in
            said.lines.append(text)
            said.queued.append(queued)
            said.high.append(high)
        }
        return said
    }

    private func reading(_ services: [String], installed: String? = nil) -> ScreenReaderState {
        ScreenReaderState(installedService: installed ?? talkBack, enabledServices: services)
    }

    /// Waits, within a bound, until `done` holds, and records a failure where
    /// it does not: a wait is the assertion of what it waited for.
    private func settle(sourceLocation: SourceLocation = #_sourceLocation, _ done: () -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(5)
        while !done() {
            guard ContinuousClock.now < deadline else {
                Issue.record("What the test waited for did not happen within 5 seconds.", sourceLocation: sourceLocation)
                return
            }
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    /// Reads once, as Settings does, and returns what the switch shows.
    private func read(_ model: AppModel) async -> ScreenReaderRow {
        await model.refreshScreenReader()
        return model.screenReaderRow
    }

    // MARK: The switch

    @Test func theSwitchShowsWhatAndroidReports() async throws {
        let (model, driver) = started()
        #expect(model.screenReaderRow == .checking)
        #expect(await read(model) == .off)
        // A change made in Android's own settings reaches the switch.
        driver.screenReaderState = reading([talkBack])
        #expect(await read(model) == .on)
        #expect(model.session.reader.isOn)
    }

    @Test func aReadThatGetsNoAnswerIsUnknownNotTheLastState() async throws {
        let (model, driver) = started()
        #expect(await read(model) == .off)
        driver.screenReaderState = reading([talkBack])
        #expect(await read(model) == .on)
        driver.screenReaderState = nil
        #expect(await read(model) == .couldntCheck)
        #expect(!model.session.reader.isOn)
        #expect(!model.screenReaderCanChange)
        // Try Again reads again.
        driver.screenReaderState = reading([])
        #expect(await read(model) == .off)
        #expect(model.screenReaderCanChange)
    }

    @Test func theSwitchTurnsTheScreenReaderOnAndOffNamingTheService() async throws {
        let (model, driver) = started()
        _ = await read(model)
        model.setScreenReader(true)
        try await settle { model.session.reader.changing == nil && model.screenReaderRow == .on }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
        // Asking for the state it is in already changes nothing.
        model.setScreenReader(true)
        #expect(driver.screenReaderChanges.count == 1)
        model.setScreenReader(false)
        try await settle { model.session.reader.changing == nil && model.screenReaderRow == .off }
        #expect(driver.screenReaderChanges.map(\.on) == [true, false])
        // No alert closes Settings: a failure is told where the person is.
        #expect(model.alert == nil)
    }

    @Test func aSystemWithoutAScreenReaderOffersNoSwitchToTurn() async throws {
        let (model, driver) = started(guestState: ScreenReaderState(installedService: nil, enabledServices: []))
        #expect(await read(model) == .unavailable)
        #expect(!model.screenReaderCanChange)
        model.setScreenReader(true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.screenReaderChanges.isEmpty)
    }

    @Test func recoveryHasNoScreenReader() async throws {
        let (model, driver) = started(voiceOver: true, mode: .recovery)
        #expect(!model.offersScreenReader)
        await model.refreshScreenReader()
        model.setScreenReader(true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(driver.screenReaderReads == 0 && driver.screenReaderChanges.isEmpty)
        #expect(model.session.reader.card == nil)
    }

    @Test func aFileTransferHoldsTheSwitchAndSaysUntilWhen() async throws {
        let (model, driver) = started()
        #expect(await read(model) == .off)
        model.busy = BusyState(title: "Working", detail: "Detail", progress: 0)
        #expect(model.screenReaderRow == .waitsForTransfer)
        #expect(!model.screenReaderCanChange)
        model.setScreenReader(true)
        await model.refreshScreenReader()
        #expect(driver.screenReaderChanges.isEmpty)
        #expect(driver.screenReaderReads == 1)
        model.busy = nil
        #expect(model.screenReaderRow == .off)
    }

    // MARK: Failures

    @Test func aChangeThatFailsSaysSoOnTheRowAndReadsAgain() async throws {
        let (model, driver) = started()
        _ = await read(model)
        let reason = "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader."
        driver.screenReaderError = AdbFailure(message: reason)
        let reads = driver.screenReaderReads
        model.setScreenReader(true)
        try await settle { model.session.reader.failure != nil && driver.screenReaderReads > reads }
        #expect(model.screenReaderRow == .failed(turningOn: true, reason: reason))
        #expect(model.screenReaderRow.line == ScreenReaderWords.didntChange(turningOn: true))
        #expect(model.alert == nil)
        #expect(!model.session.reader.isOn)
        // Try Again after the cause is gone.
        driver.screenReaderError = nil
        model.setScreenReader(true)
        try await settle { model.screenReaderRow == .on }
        #expect(model.session.reader.failure == nil)
    }

    // MARK: Reads and changes that overlap

    @Test func aReadThatAnsweredAfterAChangeNeverOverwritesIt() async throws {
        let (model, driver) = started()
        _ = await read(model)
        // The next read leaves before the change and answers after it ended.
        driver.screenReaderReadDelays = [.zero, .milliseconds(400)]
        let late = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == 2 }
        model.setScreenReader(true)
        try await settle { model.session.reader.changing == nil && model.session.reader.isOn }
        await late.value
        #expect(model.screenReaderRow == .on)
        #expect(model.session.reader.isOn)
    }

    /// The same in the other direction: a read that left while it was on and
    /// answered after it was turned off.
    @Test func aReadThatAnsweredAfterTurningItOffNeverOverwritesIt() async throws {
        let (model, driver) = started(guestState: reading([talkBack]))
        _ = await read(model)
        #expect(model.screenReaderRow == .on)
        driver.screenReaderReadDelays = [.zero, .milliseconds(400)]
        let late = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == 2 }
        model.setScreenReader(false)
        try await settle { model.session.reader.changing == nil && !model.session.reader.isOn }
        await late.value
        #expect(model.screenReaderRow == .off)
        #expect(!model.session.reader.isOn)
        #expect(driver.screenReaderState?.enabledServices == [])
    }

    /// A read that began while a change ran saw Android half way: it never
    /// applies, whatever it answers.
    @Test func aReadThatBeganDuringAChangeNeverApplies() {
        let reader = ScreenReaderSession()
        let before = reading([])
        #expect(reader.apply(read: before, reader.beginRead()))
        #expect(reader.beginChange(turningOn: true))
        let during = reader.beginRead()
        #expect(!reader.apply(read: nil, during))
        #expect(!reader.apply(read: reading([talkBack]), during))
        #expect(reader.state == before)
        #expect(!reader.readFailed)
        reader.finishChange(reading([talkBack]))
        #expect(reader.isOn)
    }

    /// While a change runs the model does not ask Android at all: a read would
    /// wait behind it on the same channel.
    @Test func nothingIsReadWhileAChangeRuns() async throws {
        let (model, driver) = started()
        _ = await read(model)
        driver.screenReaderChangeDelay = .milliseconds(300)
        let reads = driver.screenReaderReads
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        await model.refreshScreenReader()
        await model.refreshScreenReader()
        #expect(driver.screenReaderReads == reads, "a read began while the change ran")
        #expect(model.screenReaderRow == .switching(turningOn: true))
        try await settle { model.session.reader.changing == nil }
        #expect(model.screenReaderRow == .on)
    }

    @Test func aNewerReadIsNotOverwrittenByAnOlderOne() async throws {
        let (model, driver) = started()
        _ = await read(model)
        // The older read answers last, with what Android held before.
        driver.screenReaderReadDelays = [.zero, .milliseconds(300)]
        let older = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == 2 }
        driver.screenReaderState = reading([talkBack])
        await model.refreshScreenReader()
        #expect(model.screenReaderRow == .on)
        await older.value
        #expect(model.screenReaderRow == .on)
    }

    // MARK: Changes that end half way

    private func guestOn(setting: Bool = true) -> ScreenReaderState {
        ScreenReaderState(installedService: talkBack, enabledServices: [talkBack], accessibilitySetting: setting)
    }

    /// The list was written and `accessibility_enabled` was not: Android shows
    /// the screen reader off while it holds the list that was asked for, and
    /// what a later read finds there is not the change that was asked for.
    @Test func aChangeThatWroteTheListThenFailedStaysUnconfirmedWhenItReadsAgain() async throws {
        let (model, driver) = started(guestState: guestOn())
        _ = await read(model)
        #expect(model.screenReaderRow == .on)
        driver.screenReaderChangeFailsAfterListWrite = true
        model.setScreenReader(false)
        // The read after the failure finds the list empty and the switch on.
        try await settle { model.session.reader.failure != nil && model.session.reader.state?.enabledServices == [] }
        #expect(driver.screenReaderState == ScreenReaderState(
            installedService: talkBack, enabledServices: [], accessibilitySetting: true))
        guard case .failed(let on, _) = model.screenReaderRow else {
            Issue.record("The row is \(model.screenReaderRow), not a failure.")
            return
        }
        #expect(!on)
        #expect(model.screenReaderRow.offersTryAgain)
    }

    /// The change's own report that the list and the switch disagree is not a
    /// change that happened, whichever way it went.
    @Test func aReportWhoseListAndSwitchDisagreeIsNotAChange() async throws {
        let (model, driver) = started(guestState: guestOn())
        let said = listen(to: model)
        _ = await read(model)
        driver.screenReaderChangeReport = ScreenReaderState(
            installedService: talkBack, enabledServices: [], accessibilitySetting: true)
        model.setScreenReader(false)
        try await settle { model.session.reader.failure != nil }
        #expect(model.screenReaderRow == .failed(turningOn: false, reason: ScreenReaderWords.notReady))
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: false, reason: ScreenReaderWords.notReady)])
        #expect(model.session.reader.changing == nil)
    }

    /// A list that names the screen reader while Android's switch is off is not
    /// "on": Android does not run it.
    @Test func aListThatNamesItWhileAccessibilityIsOffIsOff() async throws {
        let (model, _) = started(guestState: guestOn(setting: false))
        #expect(await read(model) == .off)
        #expect(!model.session.reader.isOn)
        #expect(!(GuestLayer.accessibility(of: model)?.directTouch ?? false))
    }

    /// The service a change writes is the one it was given: a guest whose
    /// system now has the other TalkBack does not show the screen reader on.
    @Test func aServiceTheGuestNoLongerHasIsNotTheScreenReaderOn() async throws {
        let (model, driver) = started()
        _ = await read(model)
        driver.screenReaderState = ScreenReaderState(installedService: ScreenReader.services[1], enabledServices: [])
        model.setScreenReader(true)
        try await settle { model.session.reader.failure != nil }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
        #expect(driver.screenReaderState?.enabledServices == [talkBack])
        #expect(model.screenReaderRow == .failed(turningOn: true, reason: ScreenReaderWords.notReady))
    }

    // MARK: A session's own

    /// How a run ends: the guest powers off, or Android restarts in place.
    /// Either way the session is replaced.
    enum RunsEnd: CaseIterable, Sendable {
        case poweredOff, restarted
    }

    private func end(_ model: AppModel, _ how: RunsEnd) {
        switch how {
        case .poweredOff: model.powerOff()
        case .restarted: model.restart()
        }
    }

    /// Nothing of the session that ended is in the one that follows: no state,
    /// card, failure or alert, nothing begun, and nothing said to VoiceOver.
    private func expectNothingCarriedOver(_ model: AppModel, _ said: Said, sourceLocation: SourceLocation = #_sourceLocation) {
        let reader = model.session.reader
        #expect(reader.state == nil, sourceLocation: sourceLocation)
        #expect(reader.changing == nil && reader.failure == nil, sourceLocation: sourceLocation)
        #expect(reader.card == nil && reader.cardFailure == nil, sourceLocation: sourceLocation)
        #expect(!reader.toldOn && reader.outstandingOperations == 0, sourceLocation: sourceLocation)
        #expect(model.alert == nil, sourceLocation: sourceLocation)
        #expect(said.lines.isEmpty, "\(said.lines)", sourceLocation: sourceLocation)
    }

    /// The session's operations are its own: they end with it, and nothing
    /// begins in a session that ended.
    @Test func aSessionThatEndedCancelsItsOperationsAndStartsNone() async throws {
        final class Flags { var cancelled = false; var started = false }
        let flags = Flags()
        let reader = ScreenReaderSession()
        reader.run {
            do { try await Task.sleep(for: .seconds(30)) } catch { flags.cancelled = true }
        }
        #expect(reader.outstandingOperations == 1)
        reader.end()
        #expect(reader.outstandingOperations == 0 && reader.ended)
        try await settle { flags.cancelled }
        reader.run { flags.started = true }
        try await Task.sleep(for: .milliseconds(50))
        #expect(!flags.started && reader.outstandingOperations == 0)
    }

    @Test func anOperationThatFinishedIsNoLongerKept() async throws {
        let reader = ScreenReaderSession()
        reader.run {}
        try await settle { reader.outstandingOperations == 0 }
        #expect(!reader.ended)
    }

    /// The window the review found: the session ends between the person's
    /// action and the task that carries it out, which has not begun.
    @Test(arguments: RunsEnd.allCases)
    func aChangeNotYetBegunNeverReachesAndroid(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        _ = await read(model)
        let old = model.session
        model.setScreenReader(true)
        #expect(old.reader.outstandingOperations == 1)
        #expect(driver.screenReaderChanges.isEmpty)
        end(model, how)
        #expect(old.reader.outstandingOperations == 0)
        try await Task.sleep(for: .milliseconds(100))
        #expect(driver.screenReaderChanges.isEmpty, "the change reached Android after its run ended")
        #expect(driver.screenReaderState == ScreenReaderState(installedService: talkBack, enabledServices: []))
        expectNothingCarriedOver(model, said)
    }

    @Test(arguments: RunsEnd.allCases)
    func openingSettingsNotYetBegunNeverReachesAndroid(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        let old = model.session
        model.openAccessibilitySettings()
        #expect(old.reader.outstandingOperations == 1)
        end(model, how)
        try await Task.sleep(for: .milliseconds(100))
        #expect(driver.accessibilitySettingsCalls == 0, "Settings opened on Android after its run ended")
        expectNothingCarriedOver(model, said)
    }

    /// A change that waits for Android's connection when its run ends is
    /// cancelled there: nothing is written, and nothing is said of it.
    @Test(arguments: RunsEnd.allCases)
    func aChangeWaitingForAndroidIsCancelledWithItsRun(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        _ = await read(model)
        driver.screenReaderChangeDelay = .seconds(30)
        let old = model.session
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        #expect(old.reader.outstandingOperations == 1)
        end(model, how)
        try await settle { driver.screenReaderCancelled == 1 }
        #expect(driver.screenReaderState == ScreenReaderState(installedService: talkBack, enabledServices: []))
        expectNothingCarriedOver(model, said)
    }

    @Test(arguments: RunsEnd.allCases)
    func openingSettingsWaitingForAndroidIsCancelledWithItsRun(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        driver.accessibilitySettingsDelay = .seconds(30)
        model.openAccessibilitySettings()
        try await settle { driver.accessibilitySettingsCalls == 1 }
        end(model, how)
        try await settle { driver.screenReaderCancelled == 1 }
        #expect(driver.accessibilitySettingsOpened == 0)
        expectNothingCarriedOver(model, said)
    }

    /// A change that reaches Android and answers after its run ended, as a
    /// connection that does not stop would: the guest has changed, and the
    /// next session is told nothing.
    @Test(arguments: RunsEnd.allCases)
    func aChangeThatAnswersAfterItsRunLeavesTheNextSessionAlone(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        _ = await read(model)
        driver.screenReaderRepliesAfterCancel = true
        driver.screenReaderChangeDelay = .milliseconds(200)
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        end(model, how)
        try await settle { driver.screenReaderState?.enabledServices == [talkBack] }
        try await Task.sleep(for: .milliseconds(100))
        expectNothingCarriedOver(model, said)
    }

    @Test(arguments: RunsEnd.allCases)
    func aFailedChangeAfterTheRunSaysNothingToTheNextSession(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        _ = await read(model)
        driver.screenReaderRepliesAfterCancel = true
        driver.screenReaderChangeDelay = .milliseconds(200)
        driver.screenReaderError = AdbFailure(.io(EIO), activity: .screenReader)
        model.setScreenReader(true)
        try await settle { driver.screenReaderChanges.count == 1 }
        end(model, how)
        try await settle { driver.screenReaderErrorsThrown == 1 }
        try await Task.sleep(for: .milliseconds(100))
        expectNothingCarriedOver(model, said)
    }

    @Test(arguments: RunsEnd.allCases)
    func aLateErrorOpeningSettingsIsNotTheNextSessions(_ how: RunsEnd) async throws {
        let (model, driver) = started()
        let said = listen(to: model)
        driver.screenReaderRepliesAfterCancel = true
        driver.accessibilitySettingsDelay = .milliseconds(200)
        driver.accessibilitySettingsError = AdbFailure(message: "The guest refused.")
        model.openAccessibilitySettings()
        try await settle { driver.accessibilitySettingsCalls == 1 }
        end(model, how)
        try await settle { driver.accessibilitySettingsErrorsThrown == 1 }
        try await Task.sleep(for: .milliseconds(100))
        expectNothingCarriedOver(model, said)
    }

    /// The same error in the session that asked is shown, so the row above is
    /// not an alert that never comes.
    @Test func anErrorOpeningSettingsInItsOwnSessionIsShown() async throws {
        let (model, driver) = started()
        driver.accessibilitySettingsError = AdbFailure(message: "The guest refused.")
        model.openAccessibilitySettings()
        try await settle { model.alert != nil }
        #expect(model.alert?.title == ScreenReaderWords.androidSettings)
        #expect(model.alert?.message == "The guest refused.")
        #expect(driver.accessibilitySettingsOpened == 0)
    }

    @Test(arguments: RunsEnd.allCases)
    func aReadThatAnswersAfterItsRunLeavesTheNextSessionAlone(_ how: RunsEnd) async throws {
        let (model, driver) = started(voiceOver: true, guestState: reading([talkBack]), interval: .seconds(60))
        let said = listen(to: model)
        try await settle { model.session.reader.isOn }
        said.lines.removeAll()
        said.queued.removeAll()
        said.high.removeAll()
        driver.screenReaderReadDelays = Array(repeating: .zero, count: driver.screenReaderReads) + [.milliseconds(300)]
        let reads = driver.screenReaderReads
        let late = Task { await model.refreshScreenReader() }
        try await settle { driver.screenReaderReads == reads + 1 }
        end(model, how)
        await late.value
        expectNothingCarriedOver(model, said)
    }

    @Test func theLoopThatReadsEndsWithTheSession() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { driver.screenReaderReads >= 3 }
        let old = model.session
        #expect(old.reader.watcher != nil)
        model.powerOff()
        #expect(old.reader.watcher == nil && old.reader.ended)
        let reads = driver.screenReaderReads
        try await Task.sleep(for: .milliseconds(200))
        #expect(driver.screenReaderReads <= reads + 1)
    }

    /// Reads begin with VoiceOver, stop when it stops, begin again when it
    /// does, and stop with the run: the count of reads shows each.
    @Test func theLoopStartsStopsAndStartsAgain() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { driver.screenReaderReads >= 3 }
        #expect(model.session.reader.watcher != nil)
        var running = true
        model.voiceOverRunning = { running }
        running = false
        model.voiceOverStatusChanged()
        #expect(model.session.reader.watcher == nil)
        let stopped = driver.screenReaderReads
        try await Task.sleep(for: .milliseconds(150))
        #expect(driver.screenReaderReads <= stopped + 1, "reads went on after VoiceOver stopped")
        running = true
        model.voiceOverStatusChanged()
        let restarted = driver.screenReaderReads
        try await settle { driver.screenReaderReads >= restarted + 3 }
        #expect(model.session.reader.watcher != nil)
        let old = model.session
        model.powerOff()
        #expect(old.reader.watcher == nil)
        let ended = driver.screenReaderReads
        try await Task.sleep(for: .milliseconds(150))
        #expect(driver.screenReaderReads <= ended + 1, "reads went on after the run ended")
    }

    // MARK: Reading only when it is needed

    @Test func withoutVoiceOverNothingReadsUntilSettingsOpens() async throws {
        let (model, driver) = started(voiceOver: false, interval: .milliseconds(20))
        #expect(model.offersScreenReader && model.session.reader.watcher == nil)
        try await Task.sleep(for: .milliseconds(150))
        #expect(driver.screenReaderReads == 0)
        let watcher = Task { await model.watchSettings() }
        defer { watcher.cancel() }
        try await settle { driver.screenReaderReads > 0 }
        #expect(driver.screenReaderReads > 0)
        #expect(model.session.reader.settingsOpen == 1)
        watcher.cancel()
        await watcher.value
        #expect(model.session.reader.settingsOpen == 0)
    }

    @Test func underVoiceOverTheDisplayFollowsAChangeMadeInAndroid() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.state != nil }
        driver.screenReaderState = reading([talkBack])
        try await settle { model.session.reader.isOn }
        #expect(model.session.reader.isOn)
        #expect(ScreenReaderWords.displayHint(model.session.reader, edge: nil)
            == ScreenReaderWords.sentences(ScreenReaderWords.displayHintDirect, ScreenReaderWords.wayBack(nil)))
    }

    /// The display is a direct-touch area exactly while Android's screen reader
    /// is on, so VoiceOver speaks and touches reach Android only then; a read
    /// that gets no answer leaves VoiceOver in charge.
    @Test func theDisplaysDirectTouchFollowsTheState() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.state != nil }
        #expect(GuestLayer.accessibility(of: model)?.directTouch == false)
        driver.screenReaderState = reading([talkBack])
        try await settle { model.session.reader.isOn }
        let on = GuestLayer.accessibility(of: model)
        #expect(on?.directTouch == true)
        #expect(on?.value == ScreenReaderWords.displayValue(model.session.reader))
        driver.screenReaderState = nil
        try await settle { model.session.reader.readFailed }
        #expect(GuestLayer.accessibility(of: model)?.directTouch == false)
        driver.screenReaderState = reading([other])
        try await settle { model.session.reader.state != nil }
        #expect(GuestLayer.accessibility(of: model)?.directTouch == false)
        // The display is described only while the session shows it.
        model.phase = .home
        #expect(GuestLayer.accessibility(of: model) == nil)
    }

    /// What a run's start tells VoiceOver: the sentence that says what the run
    /// is and where the side keys are goes as an announcement of its own (as the
    /// argument of the screen change it is not spoken; its priority is the
    /// model's `runStartAnnouncer`, which a test replaces), and the display's
    /// direct-touch element takes VoiceOver's focus only once it has been spoken.
    @Test func theRunStartSentenceIsAnnouncedAndTheDisplayWaitsForItsEnd() throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]))
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.announceRunStart(.running(.normal))
        #expect(spoken.lines == [try #require(RootView.spoken(.running(.normal)))])
        #expect(model.session.reader.runStartAnnouncing)
        #expect(!model.displayMayTakeFocus)
        spoken.finish?(false)
        #expect(!model.session.reader.runStartAnnouncing)
        #expect(model.displayMayTakeFocus)
        model.announceRunStart(.running(.recovery))
        #expect(spoken.lines.last == RootView.spoken(.running(.recovery)))
        #expect(spoken.lines.count == 2)
    }

    /// The display says nothing of the screen reader's state while the run's
    /// sentence is spoken and Android has not been read: VoiceOver composes
    /// what it says on landing there now and speaks it after the sentence, so
    /// "checking" would be stale. The value comes with the read.
    @Test func theDisplayHasNoValueWhileTheSentenceIsSpokenAndTheStateIsUnread() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.announceRunStart(.running(.normal))
        #expect(model.session.reader.state == nil)
        #expect(GuestLayer.accessibility(of: model)?.value == nil)
        #expect(GuestLayer.accessibility(of: model)?.label == ScreenReaderWords.displayLabel)
        // Once Android has been read the value is no longer stale, though the
        // sentence is still being spoken.
        try await settle { model.session.reader.state != nil }
        #expect(GuestLayer.accessibility(of: model)?.value != nil)
        spoken.finish?(true)
        #expect(GuestLayer.accessibility(of: model)?.value != nil)
    }

    /// The card that asks waits for the run-start sentence's end: its focus
    /// would cut the sentence off. Where the sentence ends, the card rises at
    /// once.
    @Test func theCardWaitsForTheRunStartSentenceAndRisesAtItsEnd() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        // Before the first read: the card would rise as soon as Android answers.
        model.announceRunStart(.running(.normal))
        try await settle { model.session.reader.state != nil }
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.card == nil)
        #expect(model.session.reader.runStartAnnouncing)
        spoken.finish?(true)
        #expect(model.session.reader.card == .ask)
        #expect(!model.session.reader.runStartAnnouncing)
    }

    /// Where VoiceOver never says the sentence is done, the card and the
    /// display go on after the bound.
    @Test func theCardAndTheDisplayGoOnAfterTheBoundWhereNoEndComes() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        model.runStartBound = .seconds(1)
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.announceRunStart(.running(.normal))
        try await settle { model.session.reader.state != nil }
        try await Task.sleep(for: .milliseconds(100))
        #expect(model.session.reader.card == nil)
        try await settle { model.session.reader.card == .ask }
        #expect(!model.session.reader.runStartAnnouncing)
        // An end that comes late changes nothing.
        spoken.finish?(true)
        #expect(model.session.reader.card == .ask)
        model.session.reader.card = nil
        #expect(model.displayMayTakeFocus)
    }

    /// The news that the screen reader is on is at high priority, so the
    /// display's own speech does not cut it off, and at a run start where
    /// something covers the display it waits for the run's sentence rather
    /// than over it.
    @Test func theNewsThatTheScreenReaderIsOnIsHighPriorityAndWaitsForTheRunStartSentence() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        let said = listen(to: model)
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.alert = AppAlert(title: "Title", message: "Message")
        model.announceRunStart(.running(.normal))
        try await settle { model.session.reader.isOn }
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.isEmpty)
        spoken.finish?(true)
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: model.session.keysEdge)])
        #expect(said.high == [true])
    }

    /// At a run start where the screen reader was on and nothing covers the
    /// display, the sentence is all VoiceOver is told: the display, which then
    /// takes the focus, carries the state and the way back.
    @Test func aRunStartWhereTheScreenReaderIsOnSaysNoNewsAfterTheSentence() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        let said = listen(to: model)
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.announceRunStart(.running(.normal))
        try await settle { model.session.reader.isOn }
        try await Task.sleep(for: .milliseconds(100))
        #expect(!model.session.reader.toldOn)
        #expect(!model.displayMayTakeFocus)
        spoken.finish?(true)
        #expect(model.session.reader.toldOn)
        #expect(model.displayMayTakeFocus)
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.isEmpty)
        #expect(spoken.lines.count == 1)
    }

    /// A change that ends while the run's sentence is spoken says nothing over
    /// it and sets nothing: the sentence's end reads again and tells it, once.
    @Test func aChangeThatEndsWhileTheRunStartSentenceIsSpokenIsToldAfterIt() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let said = listen(to: model)
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.announceRunStart(.running(.normal))
        try await settle { model.session.reader.state != nil }
        model.setScreenReader(true)
        try await settle { model.session.reader.isOn && model.session.reader.changing == nil }
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.isEmpty)
        #expect(!model.session.reader.toldOn)
        spoken.finish?(true)
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: model.session.keysEdge)])
        #expect(said.high == [true])
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.count == 1)
    }

    @Test func aScreenThatSpeaksForItselfAnnouncesNothingAtItsStart() throws {
        let (model, _) = started(voiceOver: true)
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        for phase in [Phase.home, .booting(.normal), .failed] {
            model.announceRunStart(phase)
        }
        #expect(spoken.lines.isEmpty)
        #expect(!model.session.reader.runStartAnnouncing)
    }

    @Test func withoutVoiceOverTheRunStartAnnouncesNothingAndWaitsForNothing() throws {
        let (model, _) = started(voiceOver: false)
        let spoken = RunStartSpeech()
        model.runStartAnnouncer = { text, finished in
            spoken.lines.append(text)
            spoken.finish = finished
        }
        model.announceRunStart(.running(.normal))
        #expect(spoken.lines.isEmpty)
        #expect(!model.session.reader.runStartAnnouncing)
        #expect(model.displayMayTakeFocus)
    }

    /// Something over the display keeps VoiceOver's focus from going to it: the
    /// card that asks, a Settings sheet, a busy or low-space screen, an alert.
    @Test func theDisplayTakesNoFocusFromWhatCoversIt() throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]))
        #expect(model.displayMayTakeFocus)
        model.session.reader.card = .ask
        #expect(!model.displayMayTakeFocus)
        model.session.reader.card = nil
        let reader = model.settingsOpened()
        #expect(!model.displayMayTakeFocus)
        model.settingsClosed(reader)
        #expect(model.displayMayTakeFocus)
        model.alert = AppAlert(title: "A", message: "B")
        #expect(!model.displayMayTakeFocus)
        model.alert = nil
        model.phase = .home
        #expect(!model.displayMayTakeFocus)
    }

    @Test func theDisplayAsksForFocusOnlyWhileTheScreenReaderIsOnAndNothingCoversIt() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.state != nil }
        #expect(GuestLayer.accessibility(of: model)?.takesFocus == false)
        driver.screenReaderState = reading([talkBack])
        try await settle { model.session.reader.isOn }
        #expect(GuestLayer.accessibility(of: model)?.takesFocus == true)
        model.session.reader.runStartAnnouncing = true
        #expect(GuestLayer.accessibility(of: model)?.takesFocus == false)
    }

    /// The hint VoiceOver reads on the direct-touch display ends with the way
    /// back, naming the edge the side keys are on.
    @Test func theHintOnTheDirectTouchDisplayEndsWithTheWayBack() async throws {
        let (model, _) = started(guestState: reading([talkBack]))
        model.session.keysEdge = .right
        _ = await read(model)
        let right = try #require(GuestLayer.accessibility(of: model)?.hint)
        #expect(right.hasSuffix(ScreenReaderWords.wayBack(.right)))
        #expect(right.hasPrefix(ScreenReaderWords.displayHintDirect))
        model.session.keysEdge = .left
        let left = try #require(GuestLayer.accessibility(of: model)?.hint)
        #expect(left.hasSuffix(ScreenReaderWords.wayBack(.left)))
        #expect(left != right)
        model.session.keysEdge = nil
        #expect(GuestLayer.accessibility(of: model)?.hint?.hasSuffix(ScreenReaderWords.wayBack(nil)) == true)
    }

    // MARK: Nothing turns on by itself

    @Test func aFirstStartUnderVoiceOverAsksAndTurnsNothingOn() async throws {
        let (model, driver) = started(plan: .firstStart, voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        try await Task.sleep(for: .milliseconds(100))
        #expect(model.session.reader.card == .ask)
        #expect(driver.screenReaderChanges.isEmpty)
        #expect(!model.session.reader.isOn)
    }

    @Test func aFirstStartWithoutVoiceOverShowsNoCardAndLeavesItOff() async throws {
        let (model, driver) = started(plan: .firstStart, voiceOver: false)
        #expect(await read(model) == .off)
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.isEmpty)
    }

    @Test func theCardsTurnOnTurnsItOnAndGoes() async throws {
        let (model, driver) = started(voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
        #expect(model.session.reader.card == nil)
    }

    @Test func aTurnOnThatFailsKeepsTheCardWithTheReason() async throws {
        let (model, driver) = started(voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        let reason = "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader."
        driver.screenReaderError = AdbFailure(message: reason)
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.cardFailure != nil }
        #expect(model.session.reader.card == .ask)
        #expect(model.session.reader.cardFailure == reason)
        // Try Again, once the cause is gone.
        driver.screenReaderError = nil
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(model.session.reader.card == nil)
        #expect(model.session.reader.cardFailure == nil)
    }

    @Test func notNowKeepsTheCardAwayForTheRestOfTheSession() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        try await settle { model.session.reader.card == .ask }
        model.declineScreenReaderCard()
        #expect(model.session.reader.card == nil)
        let reads = driver.screenReaderReads
        try await settle { driver.screenReaderReads > reads + 2 }
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.isEmpty)
        // A new session asks again.
        model.powerOff()
        #expect(!model.session.reader.declined)
    }

    @Test func noCardWhereTheScreenReaderIsAlreadyOn() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]))
        try await settle { model.session.reader.isOn }
        try await Task.sleep(for: .milliseconds(50))
        #expect(model.session.reader.card == nil)
    }

    @Test func aSystemWithoutOneIsToldOnceAndNeverAskedToTurnItOn() async throws {
        let (model, driver) = started(
            voiceOver: true, guestState: ScreenReaderState(installedService: nil, enabledServices: []),
            interval: .milliseconds(20))
        try await settle { model.session.reader.card == .unavailable }
        model.dismissScreenReaderCard()
        let reads = driver.screenReaderReads
        try await settle { driver.screenReaderReads > reads + 2 }
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.isEmpty)
    }

    @Test func theCardDoesNotRiseOverSettings() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let reader = model.settingsOpened()
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.state != nil)
        #expect(model.session.reader.card == nil)
        model.settingsClosed(reader)
        #expect(model.session.reader.card == .ask)
    }

    @Test func theCardGoesWhenVoiceOverTurnsOff() async throws {
        let (model, driver) = started(voiceOver: true)
        try await settle { model.session.reader.card == .ask }
        var running = false
        model.voiceOverRunning = { running }
        model.voiceOverStatusChanged()
        #expect(model.session.reader.card == nil)
        #expect(model.session.reader.watcher == nil)
        #expect(driver.screenReaderChanges.isEmpty)
        // And back on: the loop reads again and the card follows.
        running = true
        model.voiceOverStatusChanged()
        #expect(model.session.reader.watcher != nil)
        try await settle { model.session.reader.card == .ask }
        #expect(model.session.reader.card == .ask)
    }

    // MARK: The way back

    @Test func theWayBackNamesTheEdgeTheKeysAreOn() {
        let right = ScreenReaderWords.wayBack(.right)
        let left = ScreenReaderWords.wayBack(.left)
        let none = ScreenReaderWords.wayBack(nil)
        #expect(Set([right, left, none]).count == 3)
        #expect(right.contains("right") && left.contains("left") && !none.contains("edge"))
        // The footer says it too, and follows the edge as the keys move.
        #expect(ScreenReaderWords.footer(.right).hasSuffix(right))
        #expect(ScreenReaderWords.footer(.left).hasSuffix(left))
        #expect(ScreenReaderWords.announcement(on: true, edge: .left).hasSuffix(left))
        #expect(ScreenReaderWords.announcement(on: false, edge: .left) == ScreenReaderWords.announcement(on: false, edge: .right))
    }

    // MARK: What VoiceOver is told

    @Test func turningItOnSaysSoOnceWithTheWayBackAndTurningItOffSaysSoOnce() async throws {
        let (model, _) = started(voiceOver: true)
        let said = listen(to: model)
        model.session.keysEdge = .right
        try await settle { model.session.reader.card == .ask }
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.isOn }
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .right)])
        #expect(said.lines.first?.hasSuffix(ScreenReaderWords.wayBack(.right)) == true)
        // The loop reads on and finds it on: nothing more is said.
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.count == 1)
        model.setScreenReader(false)
        try await settle { !model.session.reader.isOn && model.session.reader.changing == nil }
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .right), ScreenReaderWords.announcement(on: false, edge: .right)])
    }

    @Test func aChangeThatFailsSaysSoOnce() async throws {
        let (model, driver) = started(voiceOver: true)
        let said = listen(to: model)
        try await settle { model.session.reader.card == .ask }
        let reason = "The virtual machine couldn\u{2019}t change Android\u{2019}s screen reader."
        driver.screenReaderError = AdbFailure(message: reason)
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.cardFailure != nil }
        try await Task.sleep(for: .milliseconds(50))
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: true, reason: reason)])
    }

    @Test func theCardsTurnOnNeverDoesNothingSilently() async throws {
        let (model, driver) = started(voiceOver: true)
        let said = listen(to: model)
        try await settle { model.session.reader.card == .ask }
        // A transfer holds the channel: the card says so in Settings' words.
        model.busy = BusyState(title: "Working", detail: "Detail", progress: 0)
        model.turnOnScreenReaderFromCard()
        #expect(model.session.reader.cardFailure == ScreenReaderWords.waitsForTransfer)
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: true, reason: ScreenReaderWords.waitsForTransfer)])
        model.busy = nil
        // The last read got no answer: it says so and reads again.
        driver.screenReaderState = nil
        await model.refreshScreenReader()
        #expect(model.session.reader.state == nil)
        let reads = driver.screenReaderReads
        said.lines.removeAll()
        driver.screenReaderState = reading([])
        model.turnOnScreenReaderFromCard()
        #expect(model.session.reader.cardFailure == ScreenReaderWords.couldntCheck)
        #expect(said.lines == [ScreenReaderWords.changeFailure(turningOn: true, reason: ScreenReaderWords.couldntCheck)])
        try await settle { driver.screenReaderReads > reads }
        #expect(driver.screenReaderChanges.isEmpty)
        // The read answered: the note goes, and the next press turns it on.
        try await settle { model.session.reader.state != nil }
        #expect(model.session.reader.cardFailure == nil)
        driver.screenReaderChangeDelay = .milliseconds(300)
        said.lines.removeAll()
        model.turnOnScreenReaderFromCard()
        try await settle { model.session.reader.changing == true && driver.screenReaderChanges.count == 1 }
        // A press while the change runs says "Turning on…" and starts no second change.
        model.turnOnScreenReaderFromCard()
        model.turnOnScreenReaderFromCard()
        #expect(said.lines == [ScreenReaderWords.turningOn, ScreenReaderWords.turningOn])
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
        try await settle { model.session.reader.isOn }
        #expect(driver.screenReaderChanges == [.init(on: true, service: talkBack)])
    }

    // MARK: A start where the screen reader is on already

    /// Where the display is about to take VoiceOver's focus, its own words
    /// carry the state and the way back: nothing is announced, and nothing
    /// is asked.
    @Test func aStartWhereTheScreenReaderIsOnLeavesTheNewsToTheDisplayThatTakesTheFocus() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        let said = listen(to: model)
        model.session.keysEdge = .right
        try await settle { model.session.reader.toldOn }
        #expect(model.displayMayTakeFocus)
        // Reads go on every few milliseconds; Settings opens and closes.
        try await Task.sleep(for: .milliseconds(150))
        let reader = model.settingsOpened()
        try await Task.sleep(for: .milliseconds(60))
        model.settingsClosed(reader)
        try await Task.sleep(for: .milliseconds(60))
        #expect(said.lines.isEmpty)
        #expect(model.session.reader.card == nil)
    }

    /// Where something covers the display at that start, VoiceOver is told,
    /// once, at high priority, with the way back.
    @Test func aStartWhereTheScreenReaderIsOnAndSomethingCoversTheDisplayTellsVoiceOverOnceWithTheWayBack() async throws {
        let (model, _) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        let said = listen(to: model)
        model.session.keysEdge = .right
        model.alert = AppAlert(title: "Title", message: "Message")
        try await settle { !said.lines.isEmpty }
        // Reads go on every few milliseconds; the cover goes, Settings opens and closes.
        try await Task.sleep(for: .milliseconds(100))
        model.alert = nil
        let reader = model.settingsOpened()
        try await Task.sleep(for: .milliseconds(60))
        model.settingsClosed(reader)
        try await Task.sleep(for: .milliseconds(60))
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .right)])
        #expect(said.lines.first?.hasSuffix(ScreenReaderWords.wayBack(.right)) == true)
        #expect(said.high == [true])
        #expect(model.session.reader.card == nil)
    }

    /// Turned on in Android's own settings after a start that found it off, it
    /// is told, with nothing over the display too.
    @Test func aScreenReaderTurnedOnInAndroidAfterTheStartIsToldWithTheWayBack() async throws {
        let (model, driver) = started(voiceOver: true, interval: .milliseconds(20))
        let said = listen(to: model)
        model.session.keysEdge = .left
        try await settle { model.session.reader.card == .ask }
        #expect(said.lines.isEmpty)
        driver.screenReaderState = reading([talkBack])
        try await settle { !said.lines.isEmpty }
        try await Task.sleep(for: .milliseconds(100))
        #expect(model.displayMayTakeFocus)
        #expect(said.lines == [ScreenReaderWords.announcement(on: true, edge: .left)])
        #expect(said.high == [true])
    }

    @Test func aStartWhereTheScreenReaderIsOffTellsVoiceOverNothing() async throws {
        let (model, _) = started(voiceOver: true, interval: .milliseconds(20))
        let said = listen(to: model)
        try await settle { model.session.reader.card == .ask }
        try await Task.sleep(for: .milliseconds(100))
        #expect(said.lines.isEmpty)
    }

    @Test func withoutVoiceOverNothingIsSaid() async throws {
        let (model, _) = started(voiceOver: false, guestState: reading([talkBack]))
        let said = listen(to: model)
        _ = await read(model)
        #expect(model.session.reader.isOn)
        #expect(said.lines.isEmpty)
    }

    // MARK: Turning it off is an answer

    @Test func turningItOffInSettingsKeepsTheCardAwayForTheSession() async throws {
        let (model, driver) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        try await settle { model.session.reader.isOn }
        let reader = model.settingsOpened()
        model.setScreenReader(false)
        try await settle { model.session.reader.changing == nil && !model.session.reader.isOn }
        model.settingsClosed(reader)
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.card == nil)
        #expect(driver.screenReaderChanges.map(\.on) == [false])
    }

    @Test func turningItOffInAndroidKeepsTheCardAwayForTheSession() async throws {
        let (model, driver) = started(voiceOver: true, guestState: reading([talkBack]), interval: .milliseconds(20))
        try await settle { model.session.reader.isOn }
        driver.screenReaderState = reading([])
        try await settle { model.session.reader.state != nil && !model.session.reader.isOn }
        try await Task.sleep(for: .milliseconds(150))
        #expect(model.session.reader.card == nil)
        // A session that never had it on asks, as before; and so does the next.
        model.powerOff()
        #expect(!model.session.reader.declined)
    }

    // MARK: The words

    @Test func theDisplaySaysWhatItHasAndOffersNoHintWhereThereIsNoScreenReader() async throws {
        let (model, driver) = started()
        let reader = model.session.reader
        #expect(ScreenReaderWords.displayValue(reader) == String(localized: "Checking Android\u{2019}s screen reader\u{2026}"))
        #expect(ScreenReaderWords.displayHint(reader, edge: .right) == nil)
        _ = await read(model)
        #expect(ScreenReaderWords.displayHint(reader, edge: .right) == ScreenReaderWords.displayHintOff)
        driver.screenReaderState = ScreenReaderState(installedService: nil, enabledServices: [])
        _ = await read(model)
        #expect(ScreenReaderWords.displayHint(reader, edge: .right) == nil)
        #expect(ScreenReaderWords.displayValue(reader) == String(localized: "No screen reader for Android in this system image"))
    }

    @Test func theWordsKeepTalkBackToTheTitle() {
        let edges: [KeysEdge?] = [.right, .left, nil]
        let explanations = edges.flatMap { edge in
            [
                ScreenReaderWords.footer(edge), ScreenReaderWords.wayBack(edge),
                ScreenReaderWords.announcement(on: true, edge: edge), ScreenReaderWords.announcement(on: false, edge: edge),
            ]
        } + [
            ScreenReaderWords.cantRead, ScreenReaderWords.canRead,
            ScreenReaderWords.noReader, ScreenReaderWords.displayHintDirect, ScreenReaderWords.displayHintOff,
            ScreenReaderWords.changeFailure(turningOn: true, reason: ""), ScreenReaderWords.notReady,
        ]
        for text in explanations {
            #expect(!text.contains("TalkBack"), "\(text)")
            #expect(!text.localizedCaseInsensitiveContains("guest"), "\(text)")
        }
        #expect(ScreenReaderWords.name == "TalkBack")
        #expect(ScreenReaderWords.askTitle.contains("TalkBack"))
    }
}
