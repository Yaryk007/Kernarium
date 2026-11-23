// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// A driver that answers with a fixed plan and records what the model asks.
@MainActor
final class StubDriver: RuntimeDriver {
    var plan: StartPlan?
    var begun: [(mode: StartMode, kind: BootKind)] = []
    /// The guest each start handed over, and the guest saved by then.
    private(set) var begunGuests: [Guest] = []
    private(set) var savedWhenBegun: [Guest?] = []
    var restored = 0
    var restoreFails = false

    init(plan: StartPlan?) { self.plan = plan }

    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest { guest }
    private(set) var erased = 0
    var eraseFails = false
    func erase(progress: @escaping @Sendable (Double) -> Void) async throws {
        if eraseFails { throw InstanceError.refused("Nothing could be deleted.") }
        erased += 1
    }
    /// Makes the next saves fail, as a full disk does.
    var saveError: (any Error)?
    func save(_ guest: Guest) throws {
        if let saveError { throw saveError }
        savedGuest = guest
    }
    func startPlan() -> StartPlan? { plan }
    /// Whether the guest's first Android boot has not been seen to complete.
    var firstBootIsPending = false
    func firstBootPending() -> Bool { firstBootIsPending }
    /// What the data disk holds now; a test sets it as a run would.
    var diskUsedBytes: UInt64 = 0
    func measuredUsedBytes() -> UInt64 { diskUsedBytes }
    /// How many times the model asked to stop the start under way.
    private(set) var guestStops = 0
    func stopGuest() { guestStops += 1 }
    func begin(guest: Guest, mode: StartMode, kind: BootKind) {
        begun.append((mode, kind))
        begunGuests.append(guest)
        savedWhenBegun.append(savedGuest)
        // The runtime begins the run's record with the ramdisk the boot
        // uses; a test that plays a boot that never began, or one that
        // restarted into recovery, sets `sessionRecord` itself.
        if recordsTheBoot {
            let ramdisk = mode == .recovery ? "recovery" : guest.root.startRooted ? "root" : "stock"
            sessionRecord = SessionRecord(text: """
                uml-session 8
                state running
                end running
                status 0
                pid 1
                started \(Int(Date().timeIntervalSince1970))
                boot unobserved
                restarts 0
                ramdisk \(ramdisk)

                """)
        }
    }
    /// Whether `begin` leaves the record of the boot it began, as the runtime does.
    var recordsTheBoot = true
    func restoreUpdate() async throws {
        if restoreFails { throw InstanceError.refused("No update is pending.") }
        restored += 1
        // The files now say the update waits for the next start.
        plan = .update(from: "r1", to: "r2")
    }
    var savedGuest: Guest?
    /// What the carried release holds for root, and the guest's shell, as a
    /// test with root plays them (`RootStartTests`); none for any other.
    var rootRelease: RootRelease?
    var rootApkURL: URL?
    var guestShell: (any GuestShell)?
    /// The runtime's record of the run that was begun, as the test plays it.
    var sessionRecord: SessionRecord?
    /// The runtime's own record files, for a test that plays a relaunch
    /// after a run; with none, `sessionRecord` is the record.
    var runRecord: RunRecord?
    func currentSessionRecord() -> SessionRecord? {
        if let runRecord { return runRecord.read() }
        return sessionRecord
    }
    /// How many times the model settled a run's record.
    private(set) var retiredRunRecords = 0
    /// Whether the driver says the earlier run's record is this process's own.
    var retiredAnEarlierRun = false
    func retireRunRecord() {
        retiredRunRecords += 1
        sessionRecord = nil
        runRecord?.retire()
    }
    /// The denials of the network the model told the driver, in order; nil is none.
    private(set) var networkNotices: [NetworkDenial?] = []
    func setNetworkNotice(_ denial: NetworkDenial?) { networkNotices.append(denial) }
    /// What Settings' "When hot" told the driver, in order.
    private(set) var heatLimits: [Bool] = []
    func setHeatLimit(_ on: Bool) { heatLimits.append(on) }
    /// What the Haptics switch told the driver.
    var haptics = true
    func setHaptics(_ on: Bool) { haptics = on }
    /// What "Use the pointer as touch" and the pointer lock told the driver.
    var pointerAsTouch = false
    var pointerLocked = false
    func setPointerAsTouch(_ on: Bool) { pointerAsTouch = on }
    func pointerLockDidChange(_ locked: Bool) { pointerLocked = locked }
    private(set) var geometryChanges = 0
    func sceneGeometryDidChange() { geometryChanges += 1 }

    /// What the statistics readings answer, in turn, the last one repeating;
    /// nothing until a test scripts them.
    var samples: [StatsSample?] = []
    var memories: [GuestMemorySample?] = []
    private(set) var statisticsAsked = 0
    private(set) var memoryAsked = 0
    func readStatistics() async -> StatsSample? {
        defer { statisticsAsked += 1 }
        guard !samples.isEmpty else { return nil }
        return samples[min(statisticsAsked, samples.count - 1)]
    }
    func readGuestMemory() async -> GuestMemorySample? {
        defer { memoryAsked += 1 }
        guard !memories.isEmpty else { return nil }
        return memories[min(memoryAsked, memories.count - 1)]
    }
    var debuggerAttached = false
    func jitDebuggerAttached() -> Bool { debuggerAttached }
    /// What the JIT preparation answers and what it was asked, in order. A
    /// debugger that serves the request has left once the answer is ready or
    /// disabled. A test can hold the preparation to have a start catch it.
    var jitAnswer = JitAnswer.ready
    private(set) var jitAsked: [(wanted: Bool, shape: JitShape)] = []
    var holdsJit = false
    private var heldJit: CheckedContinuation<Void, Never>?
    func prepareJit(wanted: Bool, shape: JitShape) async -> JitAnswer {
        jitAsked.append((wanted, shape))
        if holdsJit { await withCheckedContinuation { heldJit = $0 } }
        let answer = wanted ? jitAnswer : .disabled
        if answer == .ready || answer == .disabled { debuggerAttached = false }
        return answer
    }
    func releaseJit() {
        heldJit?.resume()
        heldJit = nil
    }

    /// What the refusal-count readings answer, in turn, the last one
    /// repeating; nothing until a test scripts them.
    var refusalCounts: [UInt64?] = []
    private(set) var refusalsAsked = 0
    func readGraphicsRefusals() async -> UInt64? {
        defer { refusalsAsked += 1 }
        guard !refusalCounts.isEmpty else { return nil }
        return refusalCounts[min(refusalsAsked, refusalCounts.count - 1)]
    }

    /// What the graphics memory readings answer, in turn, the last one
    /// repeating; nothing until a test scripts them. A try to free memory
    /// records the reading it was given and answers `reclaimAnswer`.
    var graphicsMemories: [GraphicsMemoryReading?] = []
    private(set) var graphicsMemoryAsked = 0
    private(set) var graphicsMemoryFilters: [(idle: Duration, bytes: UInt64)] = []
    func readGraphicsMemory(minimumIdle: Duration, minimumBytes: UInt64) async -> GraphicsMemoryReading? {
        defer { graphicsMemoryAsked += 1 }
        graphicsMemoryFilters.append((minimumIdle, minimumBytes))
        guard !graphicsMemories.isEmpty else { return nil }
        return graphicsMemories[min(graphicsMemoryAsked, graphicsMemories.count - 1)]
    }
    var reclaimAnswer = GraphicsReclaimResult.nothingToEnd
    private(set) var reclaims: [GraphicsMemoryReading] = []
    func reclaimGraphicsMemory(_ reading: GraphicsMemoryReading, using policy: GraphicsReclaim) async
        -> GraphicsReclaimResult
    {
        reclaims.append(reading)
        return reclaimAnswer
    }

    /// What a bug report or an install does when the model asks.
    var bugReport: (@Sendable (@Sendable (BusyUpdate) -> Void) async throws -> URL) = { _ in
        throw CancellationError()
    }
    func createBugReport(update: @escaping @Sendable (BusyUpdate) -> Void) async throws -> URL {
        try await bugReport(update)
    }
    var install: (@Sendable (@Sendable (BusyUpdate) -> Void) async throws -> Void) = { _ in }
    private(set) var installed = 0
    /// Whether each attempt carried the flag, in order.
    private(set) var installAttempts: [Bool] = []
    /// The file each attempt was asked to install, in order.
    private(set) var installURLs: [URL] = []
    /// What an install does for the attempt with or without the flag; set by a test
    /// that tells the two apart, otherwise `install` answers both.
    var installAttempt: (@Sendable (Bool, @Sendable (BusyUpdate) -> Void) async throws -> Void)?
    /// How an install that does not throw ends.
    var installOutcome = ApkInstallOutcome.installed
    func installApk(
        at url: URL, bypassLowTargetSdkBlock: Bool, update: @escaping @Sendable (BusyUpdate) -> Void
    ) async throws -> ApkInstallOutcome {
        installed += 1
        installAttempts.append(bypassLowTargetSdkBlock)
        installURLs.append(url)
        if let installAttempt {
            try await installAttempt(bypassLowTargetSdkBlock, update)
        } else {
            try await install(update)
        }
        return installOutcome
    }

    /// How many times the model asked for the release's own Magisk app to be
    /// put in; the same `install` answers it.
    private(set) var rootInstalls = 0
    func installRootApp() async throws {
        rootInstalls += 1
        try await install { _ in }
    }

    /// Android's screen reader as the stub plays it: what the guest holds
    /// (every service it has on, and `accessibility_enabled`), nil while it
    /// cannot be asked, how long each reply takes, and the calls the model
    /// made. The system carries TalkBack, off.
    var screenReaderState: ScreenReaderState? = ScreenReaderState(
        installedService: ScreenReader.services[0], enabledServices: [])
    var screenReaderError: Error?
    /// Each read's delay in turn; a read past the list answers at once.
    var screenReaderReadDelays: [Duration] = []
    /// How long a change, and the opening of Android's settings, wait to reach
    /// Android, as a connection that is being established does: a task that
    /// is cancelled meanwhile ends there (`CancellationError`) and Android is
    /// left as it was. Where `screenReaderRepliesAfterCancel` is set the wait
    /// goes on, and the operation reaches Android and answers late.
    var screenReaderChangeDelay = Duration.zero
    var screenReaderRepliesAfterCancel = false
    /// The change writes the list and then fails, leaving `accessibility_enabled`
    /// as it was: a write that stopped half way.
    var screenReaderChangeFailsAfterListWrite = false
    /// What opening Android's settings does besides counting: wait, then fail.
    var accessibilitySettingsDelay = Duration.zero
    var accessibilitySettingsError: Error?
    struct ScreenReaderCall: Equatable {
        var on: Bool
        var service: String
    }
    private(set) var screenReaderReads = 0
    private(set) var screenReaderChanges: [ScreenReaderCall] = []
    private(set) var accessibilitySettingsOpened = 0
    private(set) var accessibilitySettingsCalls = 0
    /// How many operations were cancelled while they waited to reach Android,
    /// and how many errors the stub delivered (to a session that may be over).
    private(set) var screenReaderCancelled = 0
    private(set) var screenReaderErrorsThrown = 0
    private(set) var accessibilitySettingsErrorsThrown = 0
    /// What the change's command reports instead of what it wrote: a guest
    /// whose report disagrees with the request.
    var screenReaderChangeReport: ScreenReaderState?
    func readScreenReader() async -> ScreenReaderState? {
        let index = screenReaderReads
        screenReaderReads += 1
        // The guest answers with what it holds when asked, however late the
        // reply reaches the model.
        let answer = screenReaderState
        if index < screenReaderReadDelays.count { try? await Task.sleep(for: screenReaderReadDelays[index]) }
        return answer
    }
    /// Waits as the connection to Android would; false when the task was
    /// cancelled meanwhile and the operation never reached it.
    private func reachAndroid(after delay: Duration) async -> Bool {
        guard delay > .zero else { return true }
        if screenReaderRepliesAfterCancel {
            await Task.detached { try? await Task.sleep(for: delay) }.value
            return true
        }
        do {
            try await Task.sleep(for: delay)
            return true
        } catch {
            screenReaderCancelled += 1
            return false
        }
    }
    func changeScreenReader(_ on: Bool, service: String) async throws -> ScreenReaderState {
        screenReaderChanges.append(ScreenReaderCall(on: on, service: service))
        guard await reachAndroid(after: screenReaderChangeDelay) else { throw CancellationError() }
        if let screenReaderError, !screenReaderChangeFailsAfterListWrite {
            screenReaderErrorsThrown += 1
            throw screenReaderError
        }
        if let screenReaderChangeReport {
            screenReaderState = screenReaderChangeReport
            return screenReaderChangeReport
        }
        // The change edits what the guest holds now, as the command does, so
        // a service turned on meanwhile stays on; it writes the service it
        // was given, which is the guest's only if the guest still has that
        // variant.
        guard let current = screenReaderState else { throw AdbFailure(.io(EIO), activity: .screenReader) }
        var enabled = current.enabledServices.filter { !ScreenReader.services.contains($0) }
        if on { enabled.append(service) }
        if screenReaderChangeFailsAfterListWrite {
            screenReaderState = ScreenReaderState(
                installedService: current.installedService, enabledServices: enabled,
                accessibilitySetting: current.accessibilitySetting ?? !current.enabledServices.isEmpty)
            screenReaderErrorsThrown += 1
            throw screenReaderError ?? AdbFailure(.io(EIO), activity: .screenReader)
        }
        let after = ScreenReaderState(
            installedService: current.installedService, enabledServices: enabled, accessibilitySetting: !enabled.isEmpty)
        screenReaderState = after
        return after
    }
    func openAccessibilitySettings() async throws {
        accessibilitySettingsCalls += 1
        guard await reachAndroid(after: accessibilitySettingsDelay) else { throw CancellationError() }
        if let accessibilitySettingsError {
            accessibilitySettingsErrorsThrown += 1
            throw accessibilitySettingsError
        }
        accessibilitySettingsOpened += 1
    }

    /// What the resume of a guest paused for space answers, and what Shut
    /// down does; a test sets them as the runtime would behave.
    var resumeAnswers = true
    private(set) var spaceResumes = 0
    func resumeAfterSpacePause() async -> Bool {
        spaceResumes += 1
        return resumeAnswers
    }
    /// What checking again over a guest that did not pause answers: whether
    /// the space is enough now.
    var checkAnswers = false
    private(set) var spaceChecks = 0
    func checkSpaceAfterPauseFailed() async -> Bool {
        spaceChecks += 1
        return checkAnswers
    }
    var shutDownError: Error?
    private(set) var spaceShutDowns = 0
    func shutDownAfterSpacePause() async throws {
        spaceShutDowns += 1
        if let shutDownError { throw shutDownError }
    }

    /// ADB over the network as the stub plays it.
    var debugging: Bool? = true
    var address: String? = "192.0.2.23"
    var startError: Error?
    private(set) var listening = false
    private(set) var starts = 0
    private(set) var stops = 0
    /// Computers connected through the listener, which a stop closes.
    var connectedComputers = 0
    private(set) var closedComputers = 0
    /// While set, reading the setting does not answer, as while adbd restarts.
    var holdsUsbDebuggingReads = false
    func usbDebugging() async -> Bool? {
        while holdsUsbDebuggingReads, !Task.isCancelled { try? await Task.sleep(for: .milliseconds(10)) }
        return debugging
    }
    /// What Android's storage reads as; a test sets it as a boot would.
    var storage: Bool? = true
    /// How many times the model asked, and a test can hold the answer back.
    private(set) var storageReads = 0
    var holdsStorageReads = false
    func storageReady() async -> Bool? {
        storageReads += 1
        while holdsStorageReads, !Task.isCancelled { try? await Task.sleep(for: .milliseconds(10)) }
        return storage
    }
    /// Each text message request the model handed over, in order.
    private(set) var smsSent: [GuestSmsRequest] = []
    /// How the stub answers a text message: a test sets it to hold the answer
    /// back or to say what the channel would. Absent, Android took it.
    var smsAnswer: (@MainActor (GuestSmsRequest) async -> SmsReply)?
    func sendSms(_ request: GuestSmsRequest) async -> GuestSmsChannel.Outcome {
        smsSent.append(request)
        if let smsAnswer { return await smsAnswer(request).outcome(for: request) }
        return SmsReply.handedOver(1).outcome(for: request)
    }
    /// The notice's files, which a test reads and fails, and the notice over them.
    var noticeFiles = RecordingNoticeFiles()
    lazy var textMessageNotice = TextMessageNotice(
        directory: URL(fileURLWithPath: "/instance"), files: noticeFiles)
    func startAdbOverNetwork() throws {
        if let startError { throw startError }
        listening = true
        starts += 1
    }
    func stopAdbOverNetwork() {
        listening = false
        stops += 1
        closedComputers += connectedComputers
        connectedComputers = 0
    }
    func localNetworkAddress() -> String? { address }
}

/// The update, interrupted-update (Start retries, Restore, Erase) and
/// blocked-start flows against a stub runtime, including a clean shutdown
/// returning Home with a fresh session and a failed run refusing another
/// Start.
@MainActor
final class UpdateFlowTests {
    // The model of a test adopts the process's App Intent hub, which the suites
    // of Add to Home Screen replace: these take turns with them (`AndroidAppsHubLock`).
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(plan: StartPlan) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: plan)
        model.driver = driver
        model.refreshPlan()
        return (model, driver)
    }

    @Test func anUpdateStartsAsAnUpgradeAndRemembersWhereFrom() {
        let (model, driver) = model(plan: .update(from: "r1", to: "r2"))
        #expect(model.bootKind(.normal) == .upgrade)
        model.start(.normal)
        #expect(model.phase == .booting(.upgrade))
        #expect(model.bootFrom == "r1")
        #expect(driver.begun.map(\.kind) == [.upgrade])
    }

    @Test func startOnAnUnfinishedUpdateTriesItAgain() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        #expect(model.bootKind(.normal) == .upgrade)
        model.start(.normal)
        #expect(model.phase == .booting(.upgrade))
        #expect(driver.begun.map(\.kind) == [.upgrade])
        #expect(driver.restored == 0)
    }

    @Test func restoreOnlyPutsTheDataBackAndHomeShowsThePendingUpdate() async {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        await model.restoreUpdate()
        #expect(driver.restored == 1)
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.plan == .update(from: "r1", to: "r2"))
        #expect(model.alert == nil)
        // The next start runs the update again.
        model.start(.normal)
        #expect(model.phase == .booting(.upgrade))
        #expect(model.bootFrom == "r1")
    }

    @Test func aRestoreThatFailsSaysSoAndStaysAtHome() async {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        driver.restoreFails = true
        await model.restoreUpdate()
        #expect(model.alert?.title == "The virtual machine could not be restored")
        #expect(model.phase == .home)
        #expect(model.plan == .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
    }

    @Test func anAndroidThatCannotRetryWaitsForRestoreOrEraseWithoutAMessage() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: false, canRestore: true))
        #expect(!model.plan.allowsStart)
        for mode in [StartMode.normal, .recovery] {
            #expect(!model.plan.allowsStart(mode))
            model.start(mode)
        }
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.alert == nil)
    }

    @Test func recoveryWaitsForTheUpdateToFinishWithoutAMessage() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        #expect(model.plan.allowsStart(.normal) && !model.plan.allowsStart(.recovery))
        model.start(.recovery)
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.alert == nil)
    }

    @Test func aBlockedGuestIsNotStartedAndNothingIsSaid() {
        let (model, driver) = model(plan: .blocked(.olderAndroid))
        for mode in [StartMode.normal, .recovery] {
            #expect(!model.plan.allowsStart(mode))
            model.start(mode)
        }
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.alert == nil && !model.startBlocked)
    }

    @Test func lowSpaceStillAsksBeforeAStart() {
        let (model, driver) = model(plan: .normal)
        model.host.freeStorageBytes = 1_000_000
        model.start(.normal)
        #expect(model.startBlocked)
        #expect(driver.begun.isEmpty)
    }

    // MARK: Erase

    private func eventually(_ condition: () -> Bool) async -> Bool {
        for _ in 0..<400 {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    @Test func eraseDeletesTheGuestAndReturnsToTheFirstLaunchScreen() async {
        let (model, driver) = model(plan: .normal)
        model.notice = .closedInBackground
        model.resizePreset = 24
        model.erase()
        #expect(model.busy?.title == "Erasing virtual machine")
        #expect(await eventually { model.busy == nil })
        #expect(driver.erased == 1)
        // No guest: Home shows the first-launch screen, with no notice that
        // the guest was erased.
        #expect(model.guest == nil)
        #expect(model.notice == nil)
        #expect(model.resizePreset == nil)
        #expect(model.phase == .home)
        #expect(model.alert == nil)
        #expect(model.plan == .normal)
    }

    @Test func aFailedEraseKeepsTheGuestAndSaysSo() async {
        let (model, driver) = model(plan: .normal)
        driver.eraseFails = true
        model.erase()
        #expect(await eventually { model.alert != nil })
        #expect(model.alert?.title == "The virtual machine could not be erased")
        #expect(model.guest != nil)
        #expect(await eventually { model.busy == nil })
    }

    @Test func aChoiceMadeJustBeforeEraseIsNotSavedBack() async {
        let (model, driver) = model(plan: .normal)
        model.guest?.cores = 2
        model.erase()
        #expect(await eventually { model.busy == nil })
        try? await Task.sleep(for: .milliseconds(500))
        #expect(driver.savedGuest == nil)
    }

    @Test func theEndOfARunReadsThePlanAgain() {
        let (model, driver) = model(plan: .update(from: "r1", to: "r2"))
        model.start(.normal)
        // The update completed: the files now say an ordinary start.
        driver.plan = .normal
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.plan == .normal)
        // A record that cannot be read leaves the start to report why.
        driver.plan = nil
        model.refreshPlan()
        #expect(model.plan == .normal)
    }

    /// Home states the storage the guest holds. It is measured when the guest
    /// loads, so the end of a run measures it again, however the run ended.
    @Test func theEndOfARunMeasuresTheStorageAgain() {
        let (model, driver) = model(plan: .normal)
        model.guest?.usedBytes = 2 * Limits.bytesPerGiB
        model.start(.normal)
        model.finishBoot(.normal)
        // Android wrote to its data disk while the guest ran.
        driver.diskUsedBytes = 5 * Limits.bytesPerGiB + Limits.bytesPerGiB / 2
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.phase == .home)
        #expect(model.guest?.usedBytes == driver.diskUsedBytes)
        #expect(model.guest?.freeBytes == 8 * Limits.bytesPerGiB - driver.diskUsedBytes)

        let (failed, other) = self.model(plan: .normal)
        failed.start(.normal)
        other.diskUsedBytes = 3 * Limits.bytesPerGiB
        failed.runEnded(RunEnd(result: -4), uptimeSeconds: 60)
        #expect(failed.phase == .failed)
        #expect(failed.guest?.usedBytes == 3 * Limits.bytesPerGiB)
    }

    @Test func theFirstStartLastsUntilTheRunHasCreatedTheDataDisk() {
        let (model, driver) = model(plan: .firstStart)
        #expect(model.plan == .firstStart)
        model.start(.normal)
        model.finishBoot(.firstStart)
        // The runtime created the data disk during the start.
        driver.plan = .normal
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.plan == .normal)
    }

    @Test func cleanShutdownReturnsHomeAndStartsANewSession() {
        let (model, driver) = model(plan: .normal)
        model.start(.normal)
        model.finishBoot(.normal)
        let guest = model.guest
        model.session.usbDebugging = true
        model.session.showStats = true
        model.sessionPreset.settings = true
        model.sheet = .info
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.phase == .home)
        #expect(model.guest == guest)
        #expect(model.sheet == nil)
        #expect(model.session.usbDebugging == nil)
        #expect(model.session.showStats)
        #expect(model.sessionPreset == SessionPreset())
        model.start(.recovery)
        #expect(model.phase == .booting(.recovery))
        #expect(driver.begun.map(\.mode) == [.normal, .recovery])
    }

    @Test func uncleanShutdownCannotStartAgainInThisProcess() {
        let (model, driver) = model(plan: .normal)
        model.start(.normal)
        model.runEnded(RunEnd(result: -4), uptimeSeconds: 60)
        #expect(model.phase == .failed)
        model.start(.normal)
        #expect(model.phase == .failed)
        #expect(driver.begun.count == 1)
    }
}

/// What the guest's service answers to a text message request, as a test says
/// it: the channel's outcome for the request it was asked.
enum SmsReply: Sendable {
    case handedOver(Int), handedOverEarlier(Int)
    case notHandedOver(String), uncertain(String), unsettled
    case refusedVersion
    case notSent, notTaken, noAnswer
    /// Bytes as they come, whatever they are.
    case bytes(String)

    func outcome(for request: GuestSmsRequest) -> GuestSmsChannel.Outcome {
        let id = request.identifier
        switch self {
        case .handedOver(let parts): return .answered(Data("2.handed-over.\(id).\(parts)".utf8))
        case .handedOverEarlier(let parts): return .answered(Data("2.handed-over-earlier.\(id).\(parts)".utf8))
        case .notHandedOver(let reason): return .answered(Data("2.not-handed-over.\(id).\(reason)".utf8))
        case .uncertain(let reason): return .answered(Data("2.uncertain.\(id).\(reason)".utf8))
        case .unsettled: return .answered(Data("2.unsettled.\(id).store".utf8))
        case .refusedVersion: return .answered(Data("2.refused-version".utf8))
        case .notSent: return .notSent
        case .notTaken: return .notTaken
        case .noAnswer: return .noAnswer
        case .bytes(let text): return .answered(Data(text.utf8))
        }
    }
}
