// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// What a start does about root, as the model plays it against a driver that
/// records: the ramdisk the guest is written with before the runtime is
/// handed the start, and the boot screen's Magisk app step.
@MainActor
final class RootStartTests {
    // The model of a test adopts the process's App Intent hub, which the suites
    // of Add to Home Screen replace: these take turns with them (`AndroidAppsHubLock`).
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let release = RootRelease(versionName: "v31.1", versionCode: 31100, apkPath: "root/Magisk-v31.1.apk")
    private let apk = URL(fileURLWithPath: "/release/root/Magisk-v31.1.apk")

    /// What the image answers to the commands that open the Magisk app once it
    /// is in: user 0 unlocked, the launcher activity, a start the activity
    /// manager takes and the app's task in focus. Nil for any other command.
    private nonisolated static func openingAnswer(_ command: String) -> GuestShellOutput? {
        if command.hasPrefix("am get-started-user-state") { return GuestShellOutput(text: "RUNNING_UNLOCKED\n", exitCode: 0) }
        if command.hasPrefix("cmd package resolve-activity") {
            return GuestShellOutput(text: "priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=false\ncom.topjohnwu.magisk/.ui.MainActivity\n", exitCode: 0)
        }
        if command.hasPrefix("am start") {
            return GuestShellOutput(text: "Starting: Intent { cmp=com.topjohnwu.magisk/.ui.MainActivity }\n", exitCode: 0)
        }
        if command.hasPrefix("dumpsys activity activities") {
            return GuestShellOutput(text: "  topDisplayFocusedRootTask=Task{4125ccd #6 type=standard A=10126:com.topjohnwu.magisk}\n", exitCode: 0)
        }
        return nil
    }

    /// A guest shell that answers the package list with `listing` and
    /// remembers what it was asked.
    private final class Shell: GuestShell, @unchecked Sendable {
        private let lock = NSLock()
        private var asked: [String] = []
        let listing: String
        init(listing: String) { self.listing = listing }
        var commands: [String] { lock.withLock { asked } }
        func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
            lock.withLock { asked.append(command) }
            if let answer = RootStartTests.openingAnswer(command) { return answer }
            return GuestShellOutput(text: command.hasPrefix("pm list") ? listing : "", exitCode: 0)
        }
    }

    /// A guest shell whose answer waits until the test lets it go, and that
    /// a cancel does not end: a package list that is slow to come.
    private final class GatedShell: GuestShell, @unchecked Sendable {
        private let lock = NSLock()
        private var asked = 0
        private var open = false
        func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
            lock.withLock { asked += 1 }
            while !lock.withLock({ open }) { try? await Task.sleep(for: .milliseconds(5)) }
            return GuestShellOutput(text: "", exitCode: 0)
        }
        func release() { lock.withLock { open = true } }
        func waitUntilAsked() async {
            while lock.withLock({ asked }) == 0 { try? await Task.sleep(for: .milliseconds(5)) }
        }
    }

    /// A guest shell with no Magisk app whose command to open it waits until
    /// the test lets it go: the app has been installed, and the step is
    /// still inside its bound.
    private final class OpenGatedShell: GuestShell, @unchecked Sendable {
        private let lock = NSLock()
        private var opening = false
        private var open = false
        func run(_ command: String, limit: Duration) async throws -> GuestShellOutput {
            if command.hasPrefix("am start") {
                lock.withLock { opening = true }
                while !lock.withLock({ open }) { try? await Task.sleep(for: .milliseconds(5)) }
            }
            return RootStartTests.openingAnswer(command) ?? GuestShellOutput(text: "", exitCode: 0)
        }
        func release() { lock.withLock { open = true } }
        func waitUntilOpening() async {
            while !lock.withLock({ opening }) { try? await Task.sleep(for: .milliseconds(5)) }
        }
    }

    private func model(
        rootOn: Bool, available: Bool = true, shell: (any GuestShell)? = nil, carries: Bool = true
    ) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        var guest = Guest.fresh(on: host)
        guest.root.on = rootOn
        let model = AppModel(
            guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        if carries {
            driver.rootRelease = release
            driver.rootApkURL = apk
        }
        driver.guestShell = shell
        model.driver = driver
        model.rootAvailable = available
        model.refreshPlan()
        return (model, driver)
    }

    @Test func aStartWithRootOnAsksTheRuntimeForTheMagiskRamdisk() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        #expect(driver.begunGuests.first?.root.startRooted == true)
        #expect(model.guest?.root.startRooted == true)
    }

    @Test func noStartBootsRootedWithoutTheSwitchOrTheReleaseOrInRecovery() {
        let (off, offDriver) = model(rootOn: false)
        off.start(.normal)
        #expect(offDriver.begunGuests.first?.root.startRooted == false)

        let (without, withoutDriver) = model(rootOn: true, available: false)
        without.start(.normal)
        #expect(withoutDriver.begunGuests.first?.root.startRooted == false)

        let (recovery, recoveryDriver) = model(rootOn: true)
        recovery.start(.recovery)
        #expect(recoveryDriver.begunGuests.first?.root.startRooted == false)

        let (paused, pausedDriver) = model(rootOn: true)
        paused.guest?.root.attention = .failedStart
        paused.start(.normal)
        #expect(pausedDriver.begunGuests.first?.root.startRooted == false)
    }

    @Test func theStepPutsTheAppInWhenTheGuestHasNoneAndRemembersIt() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        var installing = 0
        await model.runRootAppStep(kind: .normal) { installing += 1 }
        #expect(installing == 1)
        #expect(driver.rootInstalls == 1)
        #expect(model.rootAppInstalling)
        #expect(model.guest?.root.appVersionCode == 31100)
        #expect(model.guest?.root.appInstall?.opened == true)
        #expect(shell.commands.contains { $0.hasPrefix("am start") && $0.contains(" -n 'com.topjohnwu.magisk/") })
        #expect(model.alert == nil)
        // The next start's boot, with the app there at the carried version, installs nothing more.
        var again = 0
        driver.guestShell = Shell(listing: "package:com.topjohnwu.magisk versionCode:31100\n")
        model.rootAppInstalling = false
        await model.runRootAppStep(kind: .normal) { again += 1 }
        #expect(again == 0)
        #expect(driver.rootInstalls == 1)
        #expect(!model.rootAppInstalling)
    }

    @Test func aFailedInstallIsAnAlertAndTheBootGoesOnWithTheStateUnchanged() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        driver.install = { _ in throw InstanceError.refused("No.") }
        model.start(.normal)
        await model.runRootAppStep(kind: .normal) {}
        #expect(model.alert?.title == RootWords.appFailedTitle)
        #expect(model.guest?.root.appVersionCode == nil)
        #expect(model.guest?.root.on == true)
        // The fifth step is gone with the failure: it is never shown ticked.
        #expect(!model.rootAppInstalling)
    }

    @Test func aBootThatTheRecordSaysUsedRecoveryNeverInstalls() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        // Android restarted in place into recovery: what the start asked for is not what booted.
        driver.sessionRecord = SessionRecord(text: record(end: "restarted", boot: "unfinished", ramdisk: "recovery"))
        await model.runRootAppStep(kind: .normal) {}
        #expect(driver.rootInstalls == 0)
        #expect(shell.commands.isEmpty)
    }

    @Test func aStartThatNeverReachedTheRuntimeInstallsNothing() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        driver.recordsTheBoot = false
        model.start(.normal)
        await model.runRootAppStep(kind: .normal) {}
        #expect(driver.rootInstalls == 0)
        #expect(shell.commands.isEmpty)
    }

    @Test func anAnswerThatArrivesAfterTheSwitchWasTurnedOffDoesNotTurnItBackOn() async {
        let shell = GatedShell()
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        let task = Task { await model.runRootAppStep(kind: .normal) {} }
        await shell.waitUntilAsked()
        // The boot screen's task ends and the person turns root off on Home.
        task.cancel()
        model.setRoot(false)
        shell.release()
        await task.value
        try? await Task.sleep(for: .milliseconds(100))
        #expect(model.guest?.root.on == false)
        #expect(model.alert == nil && !model.rootAppInstalling)
        #expect(driver.rootInstalls == 0)
    }

    @Test func aRestoreThatReplacedTheDiskAndCouldNotFinishForgetsTheAppBeforeTheNextInstallDecision() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        model.guest?.root.appVersionCode = 31100
        model.guest?.root.appEverHandled = true
        let snapshot = SnapshotSummary(
            id: SnapshotID(), name: "before root", created: Date(), releaseID: "20261109-los24-r20", buildNumber: 1,
            diskGiB: 16, restore: .asIs, kept: SnapshotFigure(bytes: 4096), freesWhenDeleted: SnapshotFigure(bytes: 4096))
        let fake = FakeSnapshotDriver(items: [snapshot])
        fake.restoreLeavesUnfinished = true
        model.snapshotDriver = fake
        let result = await model.restoreSnapshot(snapshot.id)
        #expect(result.refusal == .restoreUnfinished)
        // The data is the snapshot's, which has no app: nothing is remembered of one, here or on disk.
        #expect(model.guest?.root.appVersionCode == nil && model.guest?.root.appEverHandled == false)
        #expect(driver.savedGuest?.root.appVersionCode == nil && driver.savedGuest?.root.on == true)
        // The next start installs it.
        model.start(.normal)
        await model.runRootAppStep(kind: .normal) {}
        #expect(driver.rootInstalls == 1)
    }

    @Test func anInstallThatSucceededBeforeAStopDuringTheOpenWritesNothingAfterOffAndOn() async {
        let shell = OpenGatedShell()
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        let task = Task { await model.runRootAppStep(kind: .normal) {} }
        await shell.waitUntilOpening()
        // The app is in; the person stops the start, turns root off and on again.
        model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 10)
        model.setRoot(false)
        model.confirmRoot()
        shell.release()
        await task.value
        #expect(driver.rootInstalls == 1)
        // The old result neither records a version nor that the app was ever put in: the next install is not suppressed.
        #expect(model.guest?.root.appVersionCode == nil)
        #expect(model.guest?.root.appEverHandled == false)
        #expect(model.guest?.root.appInstall == nil)
        #expect(model.guest?.root.on == true)
    }

    @Test func aResultThatFindsTheSwitchChangedInTheSameBootWritesNothing() async {
        let shell = OpenGatedShell()
        let (model, _) = model(rootOn: true, shell: shell)
        model.start(.normal)
        let task = Task { await model.runRootAppStep(kind: .normal) {} }
        await shell.waitUntilOpening()
        // The same boot, the same run: only the period of the switch moved.
        model.setRoot(false)
        model.confirmRoot()
        shell.release()
        await task.value
        #expect(model.guest?.root.appVersionCode == nil && model.guest?.root.appEverHandled == false)
    }

    @Test func aStepThatEndsAfterItsBootWasStoppedRaisesNoAlertAndNoStepInALaterBoot() async {
        let shell = GatedShell()
        let (model, driver) = model(rootOn: true, shell: shell)
        driver.install = { _ in throw InstanceError.refused("No.") }
        model.start(.normal)
        let task = Task { await model.runRootAppStep(kind: .normal) {} }
        await shell.waitUntilAsked()
        model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 10)
        model.start(.normal)
        shell.release()
        await task.value
        try? await Task.sleep(for: .milliseconds(100))
        #expect(model.alert == nil, "a stopped boot's install told the person of its failure")
        #expect(!model.rootAppInstalling, "a stopped boot's install added a step to the next boot")
    }

    @Test func aStartWithoutRootAsksTheGuestNothing() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: false, shell: shell)
        model.start(.normal)
        await model.runRootAppStep(kind: .normal) {}
        #expect(shell.commands.isEmpty)
        #expect(driver.rootInstalls == 0)
        #expect(model.alert == nil)
    }

    @Test func recoveryHasNoMagiskAppStep() async {
        let shell = Shell(listing: "")
        let (model, driver) = model(rootOn: true, shell: shell)
        model.start(.normal)
        await model.runRootAppStep(kind: .recovery) {}
        #expect(shell.commands.isEmpty)
        #expect(driver.rootInstalls == 0)
    }

    // MARK: A start with root that did not finish

    private func diagnostics(record: String) throws -> URL {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("root-start-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try Data(record.utf8).write(to: directory.appendingPathComponent(BugReportAttachments.sessionRecord))
        return directory
    }

    private func record(
        end: String, status: Int = 0, boot: String = "unfinished", ramdisk: String? = "root"
    ) -> String {
        var text = "uml-session 8\nstate ended\nend \(end)\nstatus \(status)\npid 1\nboot \(boot)\nrestarts 0\n"
        if let ramdisk { text += "ramdisk \(ramdisk)\n" }
        return text
    }

    @Test func aPanicBeforeAndroidBootedPausesRootAtLaunchAndSavesIt() throws {
        let (model, driver) = model(rootOn: true)
        // Nothing of the last start is saved with the guest: the record alone says it booted Magisk.
        let directory = try diagnostics(record: record(end: "guest-panic"))
        defer { try? FileManager.default.removeItem(at: directory) }
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(model.guest?.root.startRooted == false)
        #expect(model.guest?.root.on == true)
        #expect(driver.savedGuest?.root.attention == .failedStart)
        // The next start boots without root until the person chooses.
        model.start(.normal)
        #expect(driver.begunGuests.last?.root.startRooted == false)
    }

    @Test func aPanicBeforeInitSaidAnythingPausesRootToo() throws {
        for end in ["guest-panic", "kernel-abort"] {
            let (model, driver) = model(rootOn: true)
            let directory = try diagnostics(record: record(end: end, boot: "unobserved"))
            defer { try? FileManager.default.removeItem(at: directory) }
            model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
            #expect(model.guest?.root.attention == .failedStart, "\(end)")
            #expect(driver.savedGuest?.root.attention == .failedStart, "\(end)")
        }
    }

    @Test func aLaunchBeforeTheDriverExistsSavesTheFallbackOnceItDoes() throws {
        let (model, driver) = model(rootOn: true)
        model.driver = nil
        let directory = try diagnostics(record: record(end: "kernel-abort"))
        defer { try? FileManager.default.removeItem(at: directory) }
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.savedGuest == nil)
        // The pause is not saved, so the record has not moved.
        let kept = directory.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        #expect(!FileManager.default.fileExists(atPath: kept.path))
        model.driver = driver
        model.saveRootStateIfPending()
        #expect(driver.savedGuest?.root.attention == .failedStart)
        #expect(FileManager.default.fileExists(atPath: kept.path))
    }

    @Test func aPanicAfterAndroidBootedOrOfAnotherRamdiskChangesNothing() throws {
        // Booted; a stock boot; recovery after an automatic restart; a record that names no ramdisk.
        for (ramdisk, boot) in [("root", "completed"), ("stock", "unfinished"), ("recovery", "unobserved")] as [(String?, String)]
            + [(nil, "unfinished")]
        {
            let (model, driver) = model(rootOn: true)
            let directory = try diagnostics(record: record(end: "guest-panic", boot: boot, ramdisk: ramdisk))
            defer { try? FileManager.default.removeItem(at: directory) }
            model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
            #expect(model.guest?.root.attention == nil, "ramdisk \(ramdisk ?? "none"), boot \(boot)")
            #expect(driver.savedGuest == nil)
        }
    }

    @Test func aRestartLoopBeforeBootPausesRootAtTheEndOfTheRun() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        model.runEnded(RunEnd(result: -100, restartedInALoop: true), uptimeSeconds: 4)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.savedGuest?.root.attention == .failedStart)
    }

    @Test func aKernelThatExitedWithAnErrorBeforeBootPausesRootAtTheEndOfTheRun() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        // The process goes on after the kernel's exit, and the record of the run says how the boot ended.
        driver.sessionRecord = SessionRecord(text: record(end: "kernel-exit", status: 2, boot: "unobserved"))
        model.runEnded(RunEnd(result: 2), uptimeSeconds: 3)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.savedGuest?.root.attention == .failedStart)
    }

    @Test func aRestartLoopOfRecoveryBootsIsNoFailedStartWithRoot() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        // The start asked for root, but the boot that ended used recovery's ramdisk.
        driver.sessionRecord = SessionRecord(text: record(end: "restarted", boot: "unfinished", ramdisk: "recovery"))
        model.runEnded(RunEnd(result: -100, restartedInALoop: true), uptimeSeconds: 4)
        #expect(model.guest?.root.attention == nil)
    }

    @Test func aStopOfAStartThatNeverReachedTheRuntimeOffersNothing() {
        let (model, driver) = model(rootOn: true)
        driver.recordsTheBoot = false
        model.start(.normal)
        model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 1)
        #expect(model.guest?.root.attention == nil)
    }

    @Test func aStopBeforeAndroidBootedOffersAStartWithoutRoot() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        model.runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 200)
        #expect(model.guest?.root.attention == .stoppedStart)
        #expect(model.guest?.root.on == true)
        #expect(driver.savedGuest?.root.attention == .stoppedStart)
    }

    @Test func aRunThatBootedAndThenEndedChangesNothing() {
        let (model, _) = model(rootOn: true)
        model.start(.normal)
        model.bootTracker.reach(.ready, atSeconds: 30)
        model.runEnded(RunEnd(result: -100, restartedInALoop: true), uptimeSeconds: 600)
        #expect(model.guest?.root.attention == nil)
    }

    @Test func tryAgainEndsThePauseAndStartsAndroidWithRoot() {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.attention = .failedStart
        model.tryRootAgain()
        #expect(model.guest?.root.attention == nil)
        #expect(model.guest?.root.on == true)
        // The same start as the Start button's: one run, with the Magisk ramdisk.
        #expect(driver.begun.count == 1)
        #expect(driver.begunGuests.last?.root.startRooted == true)
        #expect(driver.savedWhenBegun.last.flatMap { $0 }?.root.attention == nil)

        let (stopped, stoppedDriver) = self.model(rootOn: true)
        stopped.guest?.root.attention = .stoppedStart
        stopped.start(.normal, withoutRoot: true)
        #expect(stoppedDriver.begunGuests.last?.root.startRooted == false)
        #expect(stopped.guest?.root.attention == nil)
        #expect(stopped.guest?.root.on == true)
    }

    @Test func aTryAgainThatCannotBeginAStartStillEndsThePauseAndSaysWhatStartSays() {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.attention = .failedStart
        // Too little room on the device: Start would say so, and so does the card's button.
        model.host = HostDevice(
            name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 0)
        model.tryRootAgain()
        #expect(model.guest?.root.attention == nil && model.guest?.root.on == true)
        #expect(driver.begun.isEmpty)
        #expect(model.startBlocked)
    }

    @Test func aTryAgainWhoseSettingsCannotBeSavedBeginsWithoutRootAsStartDoes() {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.attention = .failedStart
        driver.saveError = InstanceError.refused("The disk is full.")
        model.tryRootAgain()
        #expect(driver.begunGuests.last?.root.startRooted == false)
        #expect(model.alert?.title == "Settings not saved")
    }

    @Test func turnOffRootFromTheCardEndsThePauseAndTheSwitch() {
        let (model, driver) = model(rootOn: true)
        model.guest?.root.attention = .failedStart
        model.setRoot(false)
        #expect(model.guest?.root.on == false)
        #expect(model.guest?.root.attention == nil)
        #expect(driver.savedGuest?.root.on == false)
    }

    @Test func thePreviousRecordStaysReadableUntilThePauseIsSaved() throws {
        let (model, driver) = model(rootOn: true)
        driver.saveError = InstanceError.refused("The disk is full.")
        let directory = try diagnostics(record: record(end: "guest-panic", boot: "unobserved"))
        defer { try? FileManager.default.removeItem(at: directory) }
        let current = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        let kept = directory.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        // The pause stands in memory and was not saved: the evidence has not moved.
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.savedGuest == nil)
        #expect(FileManager.default.fileExists(atPath: current.path) && !FileManager.default.fileExists(atPath: kept.path))
        #expect(model.alert?.title == "Settings not saved")
        // A launch that ends here reads the same record again; and no run begins meanwhile.
        #expect(PreviousSession.read(in: directory, endedOutOfForeground: false)?.record.ramdisk == .root)
        model.alert = nil
        model.start(.normal)
        #expect(driver.begunGuests.isEmpty)
        // Once the pause can be saved the next start saves it, keeps the record, and boots without root.
        driver.saveError = nil
        model.start(.normal)
        #expect(driver.savedGuest?.root.attention == .failedStart)
        #expect(!FileManager.default.fileExists(atPath: current.path) && FileManager.default.fileExists(atPath: kept.path))
        #expect(driver.begunGuests.last?.root.startRooted == false)
    }

    @Test func aStartWithRootWhoseSettingsCannotBeSavedBeginsWithoutRoot() {
        let (model, driver) = model(rootOn: true)
        driver.saveError = InstanceError.refused("The disk is full.")
        model.start(.normal)
        #expect(driver.begunGuests.first?.root.startRooted == false)
        #expect(model.alert?.title == "Settings not saved")
    }

    @Test func theBugReportStillSaysTheFailedBootUsedTheMagiskRamdisk() throws {
        let (model, _) = model(rootOn: true)
        let directory = try diagnostics(record: record(end: "guest-panic", boot: "unobserved"))
        defer { try? FileManager.default.removeItem(at: directory) }
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        // The pause asks for no root at the next start, and the report still says what booted.
        let text = String(decoding: try #require(model.rootReportEntry()).data, as: UTF8.self)
        #expect(text.contains("booted the Magisk ramdisk: yes"))
        #expect(text.contains("paused: a start with root ended before Android booted"))
    }

    // MARK: The record of a run that ended

    @Test func aFailedRootedRunsRecordMovesAsideOnceItsPauseIsSaved() throws {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        driver.sessionRecord = SessionRecord(text: record(end: "kernel-exit", status: 2, boot: "unobserved"))
        model.runEnded(RunEnd(result: 2), uptimeSeconds: 3)
        #expect(driver.savedGuest?.root.attention == .failedStart)
        #expect(driver.retiredRunRecords == 1)
        // The bug report still says what the failed boot used, though its record is aside.
        let text = String(decoding: try #require(model.rootReportEntry()).data, as: UTF8.self)
        #expect(text.contains("booted the Magisk ramdisk: yes"))
    }

    @Test func aRunsRecordStaysUntilThePauseDecidedFromItIsSaved() {
        let (model, driver) = model(rootOn: true)
        model.start(.normal)
        driver.sessionRecord = SessionRecord(text: record(end: "kernel-exit", status: 2, boot: "unobserved"))
        driver.saveError = InstanceError.refused("The disk is full.")
        model.runEnded(RunEnd(result: 2), uptimeSeconds: 3)
        #expect(model.guest?.root.attention == .failedStart)
        #expect(driver.retiredRunRecords == 0)
        // Once the pause can be saved, the next start saves it and the record moves aside.
        driver.saveError = nil
        model.start(.normal)
        #expect(driver.savedGuest?.root.attention == .failedStart)
        #expect(driver.retiredRunRecords == 1)
    }

    @Test func tryAgainFollowedByARelaunchDoesNotPauseRootFromTheSameRecordAgain() throws {
        let directory = try diagnostics(record: record(end: "kernel-exit", status: 2, boot: "unobserved"))
        defer { try? FileManager.default.removeItem(at: directory) }
        let (model, driver) = model(rootOn: true)
        driver.runRecord = RunRecord(in: directory)
        model.start(.normal)
        model.runEnded(RunEnd(result: 2), uptimeSeconds: 3)
        #expect(model.guest?.root.attention == .failedStart)
        model.tryRootAgain()
        #expect(model.guest?.root.attention == nil)
        // The app is relaunched before any start: the launch finds no record to decide from.
        let (relaunched, relaunchedDriver) = self.model(rootOn: true)
        relaunched.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(relaunched.guest?.root.attention == nil)
        #expect(relaunchedDriver.savedGuest == nil)
    }

    // MARK: An earlier run a process before left

    @Test func aFailedStartThatWasPausedAndLeftIsThePreviousSessionAtTheNextLaunchAndDecidesNothing() throws {
        // The process before ended with a failed start whose pause was saved and whose record
        // was moved aside; there is no record at the run's place.
        let directory = try diagnostics(record: record(end: "powered-off", ramdisk: "stock"))
        defer { try? FileManager.default.removeItem(at: directory) }
        let place = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        let earlier = directory.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        let kept = directory.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        try FileManager.default.moveItem(at: place, to: kept)
        try Data(record(end: "kernel-exit", status: 2, boot: "unobserved").utf8).write(to: earlier)
        let (model, driver) = model(rootOn: true)
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        // Its pause is saved already: nothing is decided from it again.
        #expect(model.guest?.root.attention == nil)
        #expect(driver.savedGuest == nil)
        // It is the previous session's now, and the process before that is not.
        #expect(SessionRecord.read(at: kept)?.end == "kernel-exit")
        #expect(!FileManager.default.fileExists(atPath: earlier.path))
    }

    @Test func aLaunchDropsTheEarlierRunTheProcessBeforeLeftWhenItLeftARecordAtTheRunsPlace() throws {
        let directory = try diagnostics(record: record(end: "guest-panic", boot: "unobserved"))
        defer { try? FileManager.default.removeItem(at: directory) }
        let earlier = directory.appendingPathComponent(BugReportAttachments.earlierRunRecord)
        try Data(record(end: "powered-off", ramdisk: "stock").utf8).write(to: earlier)
        let (model, _) = model(rootOn: true)
        model.adoptDiagnostics(in: directory, containers: [], endedOutOfForeground: false)
        #expect(!FileManager.default.fileExists(atPath: earlier.path))
        // The record at the run's place is the previous session's, as before.
        #expect(model.guest?.root.attention == .failedStart)
        let kept = directory.appendingPathComponent(BugReportAttachments.previousSessionRecord)
        #expect(SessionRecord.read(at: kept)?.end == "guest-panic")
    }

    // MARK: The record of a run that is asked for

    /// The real runtime over empty storage, whose session record is the file
    /// `diagnostics(record:)` wrote. A start handed to it waits for a display,
    /// so nothing runs: only the record's place is looked at.
    private func liveRuntime(in directory: URL) -> (LiveRuntime, AppModel) {
        let identity = ReleaseIdentity(
            releaseID: "20261031-los24-r4", buildFingerprint: "fp", buildNumber: 100, securityPatch: "2026-10-01",
            systemDiskSHA256: String(repeating: "a", count: 64), dataLayout: 1, minimumGiB: 1, defaultGiB: 1,
            maximumGiB: 8)
        let storage = TwoDiskStorage(
            locations: GuestLocations(directory: directory.appendingPathComponent("Guest")),
            release: identity, releaseDirectory: directory.appendingPathComponent("system"))
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        let runtime = LiveRuntime(
            model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")),
            sessionRecord: directory.appendingPathComponent(BugReportAttachments.sessionRecord))
        return (runtime, model)
    }

    @Test func asARunIsAskedForTheRecordOfTheRunBeforeMovesAside() throws {
        let directory = try diagnostics(record: record(end: "kernel-exit", status: 2, boot: "unobserved"))
        defer { try? FileManager.default.removeItem(at: directory) }
        let (runtime, model) = liveRuntime(in: directory)
        withExtendedLifetime(model) {
            #expect(runtime.currentSessionRecord()?.ramdisk == .root)
            runtime.begin(guest: Guest.fresh(on: host), mode: .normal, kind: .normal)
            // A start stopped during its preparation never began a record of its own.
            #expect(runtime.currentSessionRecord() == nil)
            let kept = directory.appendingPathComponent(BugReportAttachments.earlierRunRecord)
            #expect(SessionRecord.read(at: kept)?.ramdisk == .root)
        }
    }

    @Test(.enabled(if: geteuid() != 0))
    func aRecordThatCannotBeMovedAsideIsNeverTakenForTheRunsOwn() throws {
        let directory = try diagnostics(record: record(end: "kernel-exit", status: 2, boot: "unobserved"))
        let current = directory.appendingPathComponent(BugReportAttachments.sessionRecord)
        defer {
            try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: directory.path)
            try? FileManager.default.removeItem(at: directory)
        }
        let (runtime, model) = liveRuntime(in: directory)
        try withExtendedLifetime(model) {
            // The record can be neither moved nor emptied, as one in a file that cannot be replaced.
            try FileManager.default.setAttributes([.posixPermissions: 0o400], ofItemAtPath: current.path)
            try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: directory.path)
            runtime.begin(guest: Guest.fresh(on: host), mode: .normal, kind: .normal)
            #expect(SessionRecord.read(at: current)?.ramdisk == .root)
            #expect(runtime.currentSessionRecord() == nil)
        }
    }

    @Test func theBugReportCarriesTheRootLineOnlyWhereRootIsOffered() {
        let (offered, _) = model(rootOn: true)
        #expect(offered.rootReportEntry()?.name == "andarium/root.txt")
        let (hidden, _) = model(rootOn: false, available: false)
        #expect(hidden.rootReportEntry() == nil)
    }
}
