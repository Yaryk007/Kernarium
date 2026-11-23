// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Observation
import SwiftUI

/// A view that shows the guest's statistics.
enum StatisticsViewer: Hashable {
    /// The capsule over the session.
    case pill
    /// The Stats rows of the session's Settings.
    case settings
}

enum SheetKind: String, Identifiable, Hashable, CaseIterable {
    case create, resize, info, system, resolution, whatsNew, snapshots, appUpdate

    var id: String { rawValue }
}

/// What a launch argument asks the session to show first.
struct SessionPreset: Equatable {
    /// Android's volume panel, as after a volume key press.
    var volumePanel = false
    var settings = false
    /// Settings opens at full height instead of half.
    var settingsLarge = false
    /// Settings scrolls to the section with this identifier ("stats" or
    /// "capabilities").
    var settingsScroll: String?
    /// Settings opens on the Old app compatibility page (the preview).
    var oldAppsPage = false
    /// Settings opens on the Add to Home Screen page (the preview), and on the
    /// guide of the app with this identifier when one is stated.
    var addToHomePage = false
    var addToHomeGuide: String?
    /// The text the Add to Home Screen page's search starts with (the preview).
    var addToHomeQuery: String?
    var powerMenu = false
    var leftEdge = false
    var guestScreen: GuestScreen = .home
}

enum GuestScreen: Equatable {
    /// Android's home screen.
    case home
    /// The launcher's search with Android's own keyboard up.
    case search
    /// Android's first-run setup.
    case setup
    /// Android's screen asleep.
    case off
}

/// What the running guest offers through Settings (ADB and the stats
/// overlay) and whether its side keys are open. It lasts for one run.
@Observable
final class SessionState {
    /// ADB over network is on: the listener runs.
    var adb = false
    /// The device's address on the local network while `adb` is on; nil
    /// without one.
    var adbAddress: String?
    /// Why ADB over network did not start, in words.
    var adbProblem: String?
    /// The guest's USB debugging setting as last read; nil until it has
    /// been. ADB over network needs it on.
    var usbDebugging: Bool?
    /// Whether the guest's internal storage is up, as last read; nil until it
    /// has been. Android ends its boot some seconds before it, and the file
    /// browser waits for it.
    var storageReady: Bool?
    /// What the session knows of Old app compatibility (`OldApps.swift`).
    let oldApps = OldAppsState()
    var showStats = false
    var readout = Readout()
    /// The side keys are open. The session owns it because a touch on the
    /// guest's display, which is another view, closes them.
    var sideKeysOpen = false
    /// The edge the side keys are on, which they report as they appear and
    /// move; nil where they are not on an edge (the key rail). The words that
    /// send a person back to the keys name it.
    var keysEdge: KeysEdge?
    /// Android's screen reader in this session: what Android last said of it,
    /// a change under way and the card that asks about it.
    let reader = ScreenReaderSession()
    /// The text message being written in the bar over Android, and how the
    /// last send went. It lasts for this run.
    let textMessage = TextMessageState()

    /// The address the preview shows, from the range reserved for examples.
    static let previewAddress = "198.51.100.23"

    /// Puts the side keys away. A touch that begins on the guest's display
    /// calls it, so the touch both reaches Android and closes the keys.
    func closeSideKeys() {
        guard sideKeysOpen else { return }
        withAnimation(SideKeysOverlay.motion) { sideKeysOpen = false }
    }
}

/// What the screens read and change. The guest's life (create, start, boot,
/// erase, how a run ends) is `Lifecycle` in AndariumCore; this adds what only
/// a screen holds: the open sheet, display options and the session's state.
@Observable
final class AppModel {
    var lifecycle: Lifecycle {
        // The readings run while the guest does.
        didSet {
            if lifecycle.phase != oldValue.phase {
                // The notice belongs to the boot screen: any other screen has
                // no start under way to call slow.
                if case .booting = lifecycle.phase {} else {
                    slowStart.finish()
                    confirmingStop = false
                }
                refreshStatistics()
                refreshGraphicsNotice()
                showThermalNoticeIfWaiting()
            }
            if (lifecycle.guest == nil) != (oldValue.guest == nil) { refreshNetworkWatch() }
        }
    }
    var host: HostDevice
    /// Whether the iPad app runs on a Mac or on an Apple Vision Pro, and what
    /// its system offers: this process's.
    private let processPadAppHost = IPadAppHost.system
    var padAppHost: IPadAppHost {
        if let playedPadAppHost { return playedPadAppHost }
        return processPadAppHost
    }
    /// The guest's files, when the runtime behind the screens can reach them.
    var fileBrowser: FileBrowserModel? {
        didSet { fileBrowser?.isAvailable = { [weak self] in self?.filesAvailable ?? false } }
    }
    /// The system this build of Andarium carries.
    var system: SystemInfo
    /// What the runtime behind the screens can do; a screen shows only the
    /// controls whose capability exists.
    var capabilities: Capabilities
    /// Free space a new guest needs, which the storage behind the screens
    /// decides.
    var createRequirementBytes: UInt64
    /// The largest data disk the carried system accepts, in GiB; the storage
    /// rulers stop there when it is below `Limits.capacityRangeGiB`.
    var releaseMaximumGiB = Limits.capacityRangeGiB.upperBound
    /// The sizes the storage rulers offer, in steps of 4 GiB.
    var capacityStopsGiB: [Int] { Limits.capacityStopsGiB(releaseMaximumGiB: releaseMaximumGiB) }
    var sheet: SheetKind?
    var startBlocked = false
    /// Erase guest is asking to be confirmed.
    var confirmingErase = false
    /// What a start would be, from the guest's files: Home words Start and
    /// shows the update notices by it.
    var plan: StartPlan = .normal
    /// The release the running boot updates from, where it is known.
    var bootFrom: String?
    /// Whether the guest's first Android boot had not been seen to complete
    /// when the start under way began (`InstanceFiles.firstBootPending`): an
    /// ordinary start of such a guest is its first Android boot still.
    private(set) var bootStartedUnfinished = false
    /// The preview's plan after Restore; a device reads it from the files.
    var planAfterRestore: StartPlan?

    // App-wide display option, set on the home screen: how densely the guest
    // draws, from one guest pixel to a point to one to each pixel of the
    // screen. The guest's display always follows the window.
    /// What the person chose for the resolution; nil until they do. Until
    /// then it follows the device (`DisplayOptions.defaultScale`: 240 dpi on a
    /// low-memory one, else 320), and once they have the default never
    /// replaces it. A Retina mode an earlier version stored is no choice.
    var chosenScale: GuestDisplayScale?
    /// The resolution the launch states (automation): that run's alone, so it
    /// is not kept and what the person chose stays stored.
    var launchScale: GuestDisplayScale?
    /// What the system holds this application's memory to (`MemoryLimit`),
    /// which the default resolution follows (`DisplayOptions.isLowMemory`);
    /// zero where it sets none.
    var memoryLimitBytes: UInt64 = 0
    /// The native scale of the screen the window is on, as last read.
    var screenNativeScale = 1.0
    /// The size of the display the next start gives the guest, in points: the
    /// window less the session's safe area (`SessionDisplayArea`). Nil until
    /// Home has laid out and read it.
    var displayPoints: CGSize?
    /// What this screen offers.
    var resolutionChoices: ResolutionChoices { ResolutionChoices(nativeScale: screenNativeScale) }
    /// The resolution the guest draws at: the launch's, else the person's
    /// choice, else the default, as a choice of this screen (a number that is
    /// none goes to the nearest).
    var resolution: GuestDisplayScale {
        let choices = resolutionChoices
        return launchScale.map(choices.resolve)
            ?? displayOptions.scale(memoryLimitBytes: memoryLimitBytes, choices: choices)
    }
    /// The guest's display as the next start lays it out at the choice at
    /// `index` of `resolutionChoices`; nil until the display's size is known.
    func resolutionLayout(at index: Int) -> GuestDisplayLayout? {
        let choices = resolutionChoices
        return displayPoints.flatMap {
            GuestDisplayLayout(scale: choices.scale(at: index), points: $0, nativeScale: screenNativeScale)
        }
    }
    /// The index of `resolution` among the choices.
    var resolutionIndex: Int { resolutionChoices.index(nearest: resolution) }
    /// The guest's display as the next start lays it out.
    var resolutionLayout: GuestDisplayLayout? { resolutionLayout(at: resolutionIndex) }
    /// A screen and a display the Simulator preview states to show other
    /// devices; nothing in the application does.
    var statedNativeScale: Double?
    var statedDisplayPoints: CGSize?
    var statedMaximumFramesPerSecond: Int?
    /// What the scene reads of the screen the window is on. A caller that
    /// does not read the frame rate leaves it as it was.
    func screenDidChange(nativeScale: Double, maximumFramesPerSecond: Int? = nil) {
        let scale = statedNativeScale ?? nativeScale
        if screenNativeScale != scale { screenNativeScale = scale }
        if let rate = statedMaximumFramesPerSecond ?? maximumFramesPerSecond,
           screenMaximumFramesPerSecond != rate {
            screenMaximumFramesPerSecond = rate
        }
    }
    /// What Home reads of the window: the display's size as the guest will
    /// get it.
    func displayPointsDidChange(_ points: CGSize) {
        let points = statedDisplayPoints ?? points
        if displayPoints != points { displayPoints = points }
    }
    /// The person's choice of resolution on Home.
    func chooseResolution(_ scale: GuestDisplayScale) {
        chosenScale = scale
    }
    /// What is kept of the display options across launches.
    var displayOptions: DisplayOptions { DisplayOptions(chosenScale: chosenScale) }
    /// The display options to keep; nil where the launch stated the
    /// resolution, so what the person chose stays stored.
    var displayOptionsToKeep: DisplayOptions? { launchScale != nil ? nil : displayOptions }
    /// The host owns the guest's display rotation: in an iPad session the guest
    /// is held at rotation zero and its display follows the scene. The scene's
    /// root sets it (`SessionOrientationPolicy`). Never a saved guest setting.
    var hostDrivesGuestRotation = false
    /// The guest follows the device's orientation, as iOS reports it: the device
    /// is an iPhone, whose interface stays portrait while the guest turns with
    /// the device, or the launch states it (a Mac test). Where the interface
    /// turns (a folding iPhone's inner display) the host drives the guest
    /// instead. Set once, when the model is made (`AppEntry`).
    var followsDeviceOrientation = false
    /// What the system says of the folding display the scene is on (the iPhone
    /// Duo); nil on every device that reports no hinge, which keeps every
    /// layout it has. The scene's root reads it (`FoldSceneReader`) and the
    /// layout decisions are `AndariumCore`'s (`FoldLayout.swift`).
    var fold: FoldReading?
    /// Where the scene is: `.plain` unless the display folds.
    var foldPose: FoldPose { fold?.pose ?? .plain }
    /// The session's layout on a folding display; nil elsewhere.
    var sessionPlan: SessionPlan? { fold.flatMap(SessionPlan.init) }

    var sessionPreset = SessionPreset()
    var session = SessionState() {
        // The new session shows no figures: the next reading is published
        // whatever it draws. The old one's reader ends, and with it the work it began.
        didSet {
            statisticsGate.reset()
            if oldValue !== session { oldValue.reader.end() }
        }
    }
    /// The release notes: Info's history and the ribbon above Start after an
    /// update.
    var whatsNew = WhatsNewModel()
    /// The notice that a newer version of Andarium exists, in a build that
    /// has an address for its updates and a launch that is the person's own;
    /// nil elsewhere, where Home and Info show nothing of it.
    var appUpdate: UpdateNotice?
    /// The launch decided whether the statistics show (automation, see
    /// `StatsPreference.launch`): the value belongs to that run and is not kept.
    var statsDecidedByLaunch = false
    /// What is kept of Settings' "Show on screen" across launches
    /// (`StatsPreference`); nil where the launch decided it.
    var showStatsToKeep: Bool? { statsDecidedByLaunch ? nil : session.showStats }
    // AOT (Home's AOT row): Android's system libraries run from the code
    // translated from them ahead of time. The switch applies to the next start,
    // and only a build that carries the translated-code framework has it.
    /// This build carries the translated-code framework (what Acknowledgements
    /// reads, `Acknowledgements.carries`), so Home shows the row.
    var translatedLibrariesAvailable = false
    /// Whether the person wants them. On is the default. Kept for the install
    /// (`TranslatedLibrariesPreference`).
    var translatedLibrariesOn = true
    /// The launch decided it (automation): the value belongs to that run and
    /// is not kept.
    var translatedLibrariesDecidedByLaunch = false
    /// What is kept of the row across launches; nil where the launch decided
    /// it.
    var translatedLibrariesToKeep: Bool? { translatedLibrariesDecidedByLaunch ? nil : translatedLibrariesOn }
    /// The person turns the translated libraries on or off. It changes nothing
    /// until Android next starts: the choice is read at each start.
    func setTranslatedLibraries(_ on: Bool) {
        translatedLibrariesOn = on
    }
    /// The note under Performance's panel: what JIT does at the next start
    /// (`JitStatus.footer`). The ready sentence shows only while the JIT row is
    /// on; the sentences that say how to enable it, that the guest changed
    /// after it was prepared, or that a start ran without it show whatever the
    /// row says.
    var jitNote: String? {
        let status = jitStatus
        return status.readyForNextStart && !jitOn ? nil : status.footer
    }
    /// The next start runs on the JIT engine, which does not use the translated
    /// libraries: the JIT is on and the note under its row says it is ready
    /// (its memory is, or can be, prepared for the guest, and no earlier start
    /// fell back), so that this row and that note agree.
    var jitInUseForNextStart: Bool {
        jitAvailable && jitOn && jitStatus.readyForNextStart
    }
    /// What a start does about the translated libraries, for the runtime's boot
    /// request and the start record. `jitInUse` is whether this start runs on
    /// the JIT engine.
    func translatedLibrariesStart(jitInUse: Bool) -> TranslatedLibrariesStart {
        TranslatedLibrariesStart(
            buildCarries: translatedLibrariesAvailable, optionOn: translatedLibrariesOn, jitInUse: jitInUse)
    }
    // JIT (Home's JIT row): the guest runs on the JIT engine when the person
    // turned it on and a debugger they attached has prepared the executable
    // memory.
    /// This build on this device can run the guest on the JIT engine: a build
    /// on a device, Debug or Release. The Simulator and the preview offer it
    /// only as a mock.
    var jitAvailable = false
    /// Whether the person wants the JIT. It applies to the next start.
    var jitOn = false
    /// The launch decided it (automation): the value belongs to that run and
    /// is not kept.
    var jitDecidedByLaunch = false
    /// What is kept of the JIT row across launches (`JitPreference`); nil
    /// where the launch decided it.
    var jitToKeep: Bool? { jitDecidedByLaunch ? nil : jitOn }
    /// A debugger is attached to Andarium, as last read.
    var jitDebuggerAttached = false
    /// What the executable memory was prepared for, once a start has done it.
    var jitPrepared: JitShape?
    /// How the last start answered, when it ran without the JIT it was asked
    /// for.
    var jitLastStart: JitAnswer?
    /// The debugger is preparing the executable memory, which the boot screen
    /// says while it takes the seconds it takes.
    var preparingJit = false
    /// How the attempt made when Andarium opened ended, when it left the
    /// memory unprepared (`prepareJitAtLaunch`).
    var jitLaunch: JitAnswer?
    /// The attempt made when Andarium opened, which a start that comes while
    /// it runs waits for.
    @ObservationIgnored private var jitLaunchWork: Task<Void, Never>?
    @ObservationIgnored private var jitLaunchAsked = false
    /// What Home says under the JIT row.
    var jitStatus: JitStatus {
        JitStatus(
            debuggerAttached: jitDebuggerAttached, prepared: jitPrepared,
            guest: guest.map { JitShape(memoryMiB: $0.memoryMiB, processorCount: $0.cores) },
            lastStart: jitLastStart, launch: jitLaunch)
    }
    /// Asks the debugger attached to Andarium for what the JIT row says now,
    /// once per process, as the application opens: StikDebug spins while it
    /// waits for the request and the system ends it within a minute. With the
    /// row on, the executable memory is prepared for the guest (`standard`
    /// when there is none yet, or when `replacingGuest` says the launch is
    /// about to make another); with the row off, the debugger is told to go.
    /// The work is off the main thread, Home does not wait for it, and a start
    /// that comes while it runs waits for it.
    func prepareJitAtLaunch(standard: JitShape, replacingGuest: Bool = false) {
        guard !jitLaunchAsked else { return }
        jitLaunchAsked = true
        guard jitAvailable, let driver else { return }
        refreshJit()
        let current = replacingGuest
            ? nil : guest.map { JitShape(memoryMiB: $0.memoryMiB, processorCount: $0.cores) }
        let wanted = jitOn
        guard let request = JitLaunchRequest.atLaunch(
            wanted: wanted, debuggerAttached: jitDebuggerAttached, guest: current, standard: standard)
        else { return }
        let shape: JitShape
        switch request {
        case .prepare(let target): shape = target
        case .detach: shape = current ?? standard
        }
        jitLaunchWork = Task { [weak self] in
            let answer = await driver.prepareJit(wanted: wanted, shape: shape)
            self?.jitLaunchEnded(answer, shape: shape)
        }
    }
    /// Waits for the attempt made at launch, if it is still under way.
    func waitForJitLaunch() async {
        await jitLaunchWork?.value
    }
    private func jitLaunchEnded(_ answer: JitAnswer, shape: JitShape) {
        switch answer {
        case .ready:
            jitPrepared = shape
            print("Andarium: JIT prepared at launch for \(shape.memoryMiB) MiB, \(shape.processorCount) vCPUs")
        case .disabled:
            print("Andarium: JIT is off; the debugger was told to detach at launch")
        case .noDebugger, .failed, .otherSettings, .unsupported:
            jitLaunch = answer
            print("Andarium: JIT was not prepared at launch (\(answer))")
        }
        refreshJit()
    }
    /// What a start does about the JIT for a guest of `shape`; true when the
    /// guest boots on it. The memory was prepared for this guest when
    /// Andarium opened, or is prepared here when no attempt was made then. A
    /// start that comes while the attempt at launch runs waits for it, and the
    /// boot says "Preparing JIT" meanwhile. A JIT that is off lets an attached
    /// debugger go, so that the guest does not run under it. A start that
    /// cannot have the JIT runs on gadgets and says why; it never fails.
    func startOnJit(_ shape: JitShape) async -> Bool {
        let wanted = jitOn
        if wanted { preparingJit = true }
        await waitForJitLaunch()
        let answer: JitAnswer
        if wanted, jitPrepared == shape {
            answer = .ready
        } else if let driver {
            answer = await driver.prepareJit(wanted: wanted, shape: shape)
        } else {
            answer = .unsupported
        }
        preparingJit = false
        refreshJit()
        switch answer {
        case .ready:
            jitPrepared = shape
            jitLastStart = nil
            print("Andarium: JIT ready")
            return true
        case .disabled:
            jitLastStart = nil
            return false
        case .noDebugger, .failed, .otherSettings, .unsupported:
            // A fall-back is only worth a sentence to a person who asked for the JIT.
            jitLastStart = wanted ? answer : nil
            if wanted { print("Andarium: \(answer.fallbackLog ?? "started without JIT")") }
            return false
        }
    }
    /// The person turns the JIT on or off. It changes nothing until a start.
    func setJit(_ on: Bool) {
        jitOn = on
    }
    /// Reads whether a debugger is attached now. The preview plays one.
    func refreshJit() {
        jitDebuggerAttached = driver?.jitDebuggerAttached() ?? previewJitDebugger
    }
    /// Whether the preview, which has no runtime, plays a debugger.
    var previewJitDebugger = true
    /// How the last run ended, for the screen that says so.
    var runEnd = RunEnd(result: 0)
    var runUptimeSeconds: Int?
    /// Where Resize opens; nil opens at the current capacity.
    var resizePreset: Int?
    /// Holds the boot screen at one stage instead of animating (screenshots).
    var frozenBootStage: Int?
    /// Seconds the boot screen shows when it is held.
    var frozenElapsed = 9

    /// The launch's automation work (create, erase, start) has run; a scene
    /// that connects to this model again does not repeat it.
    var launchApplied = false

    /// The runtime and storage behind the screens; nil plays mock data.
    var driver: (any RuntimeDriver)? {
        didSet { reportNetworkDenial() }
    }
    /// The guest's data snapshots behind the screens: the runtime's, or the
    /// preview's seeded ones (`SnapshotDriver`).
    var snapshotDriver: (any SnapshotDriver)? {
        didSet {
            snapshotDriver?.activityChanged = { [weak self] in self?.snapshotActivity = $0 }
            snapshotActivity = snapshotDriver?.activity ?? SnapshotActivity()
        }
    }
    /// What the snapshot driver is doing, which it sets around each
    /// operation: the controls of a screen follow `snapshotGate`.
    var snapshotActivity = SnapshotActivity() {
        didSet {
            snapshots.activityChanged(from: oldValue, to: snapshotActivity)
            // A request to open an Android app that waited for it goes on.
            if oldValue.holdsGuestFiles, !snapshotActivity.holdsGuestFiles { androidAppsSnapshotOperationEnded() }
        }
    }
    /// What the snapshot screens show and do (`SnapshotsModel`).
    let snapshots = SnapshotsModel()
    /// Android's apps, the cache of them and the request to open one
    /// (`AppModel+AndroidApps.swift`).
    let androidApps = AndroidAppsState()
    /// Home's notice after a snapshot was restored, until the guest starts or
    /// the person puts it away.
    var restoredSnapshot: RestoredSnapshotNotice? {
        // The data is what the snapshot held: the apps may differ from the cache's list.
        didSet { if restoredSnapshot != nil { androidApps.invalidated() } }
    }
    /// A long task the screens wait for.
    var busy: BusyState? {
        didSet { if busy == nil { busyOverSheet = false } }
    }
    /// A sheet that stands over the screens draws the overlay of the task under
    /// way itself, and the screens do not: set for the task a sheet starts,
    /// and cleared with it.
    var busyOverSheet = false
    #if FOLLOW_X
    /// The handle Follow developer on X opens, from Info.plist, which only a
    /// build that names one has; a test replaces it.
    @ObservationIgnored var followXHandle: () -> String? = { FollowXProfile.handle }
    #endif
    /// The update's Restore is under way (waiting for a snapshot operation, or running).
    private var restoringUpdate = false
    /// The sender of the last text message, kept until the virtual machine
    /// is erased.
    @ObservationIgnored var smsSender = GuestSms.SenderMemory()
    /// Says what a text message came to aloud, for VoiceOver, and tells whether
    /// the announcement was spoken to its end; a test collects the words and
    /// reports the end itself.
    @ObservationIgnored var textMessageAnnouncer: @MainActor (String, @escaping @MainActor @Sendable (Bool) -> Void) -> Void = {
        VoiceOver.announce($0, finished: $1)
    }
    /// VoiceOver runs. A line counts as told only when VoiceOver has said it.
    @ObservationIgnored var textMessageVoiceOver: @MainActor () -> Bool = { UIAccessibility.isVoiceOverRunning }
    /// The scene is active: a line counts as told only while it is (the bar reports it).
    @ObservationIgnored var textMessageSceneActive = true
    /// The notice of a model with no guest (the preview): it lives in memory.
    @ObservationIgnored let previewTextMessageNotice = TextMessageNotice(
        directory: URL(fileURLWithPath: "/"), files: MemoryNoticeFiles())
    /// Waits for Settings to go before the bar comes up over Android; a test
    /// paces it by hand.
    @ObservationIgnored var textMessageSleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    /// The guest is paused because the device is nearly out of space; the
    /// screen over the session asks what to do.
    var spacePause: SpacePause? {
        // The pause for space that held a request to open an Android app is over.
        didSet {
            if oldValue != nil, spacePause == nil { androidAppsHoldEnded(.space) }
            if oldValue == nil, spacePause != nil { androidAppsSpacePaused() }
        }
    }
    var alert: AppAlert?
    /// What a Files picker's failure says, over Settings (`documentPickerFailed`).
    var documentPickerProblem: AppAlert?
    /// Settings' Install APK asks whether to install an app Android refused for
    /// the version it was built for; the screens' alert shows it while the
    /// install waits behind the overlay (`LegacyInstallFlow`).
    var legacyInstallQuestion: LegacyInstallQuestion?
    @ObservationIgnored private var legacyInstallAnswer: CheckedContinuation<Bool?, Never>?
    /// Which attempt of an install the overlay listens to: what an attempt that is over reports
    /// late (the guest's last words arrive on the main actor after the attempt has thrown) is
    /// dropped, so it cannot write over the question's or the retry's overlay.
    @ObservationIgnored private var installAttemptTag = 0
    /// How far the current boot has got, from the guest's console.
    var bootTracker = BootTracker()
    /// When the start under way began and whether the boot screen says it is
    /// slow (`SlowStart`).
    var slowStart = SlowStart()
    /// The notice's Stop asked first (`askToStop`): the dialog is up.
    var confirmingStop = false
    /// The preview's frozen boot screen shows the start's own clock, which runs.
    @ObservationIgnored var previewClockRuns = false
    /// The time the boot screen counts a start by, in seconds: a monotonic
    /// reading that does not run while the guest is held (a background span, a
    /// pause for space). A runtime supplies its pause-aware clock; a test
    /// replaces it. The default stands still while the device sleeps, as the
    /// system uptime it replaces did (`MonotonicClock`).
    @ObservationIgnored var clock: () -> TimeInterval = { MonotonicClock.suspendingSeconds() }
    /// The guest's disk is being written before the run can begin.
    var preparingDisk = false
    var diskProgress: Double?
    /// The device has haptics (a Taptic Engine) for the guest's vibrator to
    /// play on. The runtime states it; an iPad, a Mac and the Simulator have
    /// none, and Capabilities then has no Haptics row.
    var hapticsAvailable = false
    /// The Android release this copy carries holds root (`RootRelease`): Home
    /// shows the Root section only then, and a start boots Magisk only then.
    /// The device application reads it from the staged release; nothing else
    /// sets it.
    var rootAvailable = false
    /// The alert that asks before root is turned on is up (`Model+Root.swift`).
    var rootWarningShowing = false
    /// The boot screen's Magisk app step is under way: Android has booted with
    /// root and the app is being put in the guest (`runRootAppStep`). The
    /// screen adds its fifth step only while this is set.
    var rootAppInstalling = false
    /// Counts the starts, restarts and run ends of the guest: a Magisk app
    /// step belongs to the boot it began in and changes nothing of Home or
    /// the boot screen once this number has moved (`runRootAppStep`).
    private(set) var bootRun = 0
    /// Counts the changes of the Root switch and of what pauses it: a result
    /// of a Magisk app step belongs to the period of the switch it began in,
    /// as well as to its boot (`runRootAppStep`).
    @ObservationIgnored
    var rootPeriod = 0
    /// A fallback decided at launch, before a driver could save the guest.
    @ObservationIgnored
    var rootStateUnsaved = false
    /// The previous session's record is left where the runtime wrote it while
    /// a decision made from it (a pause of root) is not saved, so that a
    /// launch that ends first reads it again (`PreviousSession`).
    @ObservationIgnored
    var previousSessionHeld = false
    /// The record of a run that ended is left at its place while the pause
    /// decided from it is not saved, and moved aside once it is
    /// (`keepRunRecordIfDecided`): a launch that comes after "Try with root
    /// again" does not read it again.
    @ObservationIgnored
    var runRecordHeld = false
    /// The ramdisk the last boot before the run under way used, as its record
    /// said: the previous session's, or a run of this process whose record
    /// was moved aside.
    @ObservationIgnored
    var previousBootRamdisk: SessionRecord.BootRamdisk?
    /// Settings' "Use the pointer as touch": the system pointer's clicks are the
    /// guest's touches and the guest has no mouse. Kept for the install
    /// (`PointerPreference`); off, Android sees a mouse with its own cursor.
    var pointerAsTouch = false
    /// The launch decided it (automation): the value belongs to that run and
    /// is not kept.
    var pointerAsTouchDecidedByLaunch = false
    /// What is kept of the setting across launches; nil where the launch
    /// decided it.
    var pointerAsTouchToKeep: Bool? { pointerAsTouchDecidedByLaunch ? nil : pointerAsTouch }
    /// Settings is open over the session. A sheet has the keys, so the guest
    /// does not hold the keyboard focus meanwhile.
    var sessionSettingsOpen = false
    /// The share sheet of a bug report is up over the session.
    var shareSheetUp = false
    /// The guest has the hardware keys unless something is over it that needs
    /// them (Settings, the text message bar, a sheet, an alert, the share
    /// sheet): the sheet takes the keys, and the guest takes them back when
    /// it goes. The pointer lock
    /// goes with them.
    var guestHasKeys: Bool {
        !sessionSettingsOpen && !session.textMessage.isOpen && sheet == nil && alert == nil
            && legacyInstallQuestion == nil && !shareSheetUp && !askingToTurnOffHeatLimit
    }
    /// An Android application captured the pointer and the guest should have
    /// the pointer lock; the scene's root asks iOS for it.
    var pointerLockWanted = false
    /// A UI test's launch: the guest's keyboard and mouse send to a log the
    /// test reads, and Settings carries a text field to take the keys.
    var inputProbe: InputProbe?
    var microphoneState = MicrophoneState.notAsked
    /// How far iOS lets Andarium use the device's cameras, and `unavailable`
    /// for a device that has none.
    var cameraAccess = CameraAccess.unavailable
    /// The person went to Settings to allow the camera: when Andarium is
    /// active again with access, the switch they tapped turns on.
    var cameraAwaitsSettings = false
    /// The Camera switch is on and iOS's question about it is on screen, or
    /// about to be. A switch that is on while iOS has not been asked and no
    /// question is under way is a choice made under access that has ended, and
    /// it goes off.
    private var cameraAsking = false
    /// Asks iOS about the camera. The device application and the Simulator
    /// preview's system mode have one; a preview without it takes the access
    /// as given.
    @ObservationIgnored var camera: CameraController?
    /// Asks the runtime what a live camera session of this host holds out of
    /// the GPU's budget, for every budget the memory policy has
    /// (`MemoryPolicy.withCameraReserve`). The device application has one, and
    /// so do the preview states that show a camera reserve; without one the
    /// model keeps the policy's own figure, the ceiling that bounds the system
    /// cameras.
    @ObservationIgnored var cameraReservePlanner: (@Sendable (MemoryPolicy) -> CameraReservePlan)?
    /// The last question to the runtime for the camera reserve, which a test
    /// waits for.
    @ObservationIgnored private(set) var cameraReserveTask: Task<Void, Never>?
    /// What the host is doing for the guest's cameras, from the runtime, and
    /// whether the person put the notice it raised away.
    private(set) var cameraNotice = CameraNoticeState()
    var cameraCapture: CameraCapture { cameraNotice.capture }
    /// Waits out the time the Camera-is-off notice stays after the guest closed
    /// its camera; a test paces it by hand.
    @ObservationIgnored var cameraNoticeSleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    /// The moment a report is taken at; a test sets its own.
    @ObservationIgnored var cameraNoticeClock: @MainActor () -> ContinuousClock.Instant = { .now }
    @ObservationIgnored private var cameraNoticeExpiry: Task<Void, Never>?
    /// The question about a permission the person is being asked, if any.
    var permissionPrompt: PermissionPrompt?
    /// The person went to Settings to allow the microphone: when Andarium is
    /// active again with access, the switch they tapped turns on.
    var microphoneAwaitsSettings = false
    /// Asks iOS about the microphone and tells the guest. The device
    /// application and the Simulator preview's system mode have one; a preview
    /// without it takes the access as given.
    @ObservationIgnored var microphone: MicrophoneController?
    var locationAuthorization = LocationAuthorization.notDetermined
    /// The person went to Settings to allow the location: when Andarium is
    /// active again with access, the switch they tapped turns on.
    var locationAwaitsSettings = false
    /// The Location switch is on and iOS's question about it is on screen, or
    /// about to be. A switch that is on while iOS has not been asked and no
    /// question is under way is a choice made under access that has ended
    /// (iOS's "Allow Once"), and it goes off.
    private var locationAsking = false
    /// Asks iOS about the location and sets the guest's session to match. The
    /// device application and the Simulator preview's system mode have one; a
    /// preview without it takes the access as given.
    @ObservationIgnored var location: LocationController?
    /// Opens Andarium's page in the system's Settings. A test puts its own here.
    @ObservationIgnored var openSettings: @MainActor () -> Void = {
        if let url = URL(string: UIApplication.openSettingsURLString) {
            UIApplication.shared.open(url)
        }
    }
    private var persistTask: Task<Void, Never>?
    /// The Android operation under way (a bug report, an install), which the
    /// overlay's Cancel stops, and the run it belongs to.
    private var operation: Task<Void, Never>?
    private var operationEpoch = 0
    private var operationCancelled = false
    /// Looks after ADB over network while it is on.
    private var adbWatcher: Task<Void, Never>?
    private var readingUsbDebugging = false
    private var readingStorage = false
    /// How often the watcher looks while ADB over network is on: the address
    /// every time, the USB debugging setting every fifth. A test shortens it.
    var adbWatchInterval = Duration.seconds(2)
    /// Whether VoiceOver is running; a test answers for it.
    @ObservationIgnored var voiceOverRunning: @MainActor () -> Bool = { UIAccessibility.isVoiceOverRunning }
    /// What VoiceOver is told of its own accord; a test collects it.
    @ObservationIgnored var announcer: @MainActor (_ text: String, _ queued: Bool, _ highPriority: Bool) -> Void = {
        VoiceOver.announce($0, queued: $1, priority: $2 ? .high : nil)
    }
    /// Says what a run is as it begins, at high priority (VoiceOver's own
    /// speech of the element it lands on after the screen change would cut a
    /// queued or default announcement off), and tells whether the sentence was
    /// spoken to its end; a test collects the words and reports the end itself.
    @ObservationIgnored var runStartAnnouncer: @MainActor (String, @escaping @MainActor @Sendable (Bool) -> Void) -> Void = {
        VoiceOver.announce($0, priority: .high, finished: $1)
    }
    /// How long the card and the display wait for that sentence's end before
    /// they go on without it, where VoiceOver never reports one. The sentence
    /// takes about seven seconds at VoiceOver's default rate; the bound leaves
    /// room for slower speech and longer languages.
    @ObservationIgnored var runStartBound: Duration = .seconds(15)
    /// The preview's mock guest screen reader, where no runtime is behind the
    /// screens; nil in the application.
    @ObservationIgnored var screenReaderStandIn: (any ScreenReaderStandIn)?
    /// How often the loop reads while VoiceOver runs; a test shortens it.
    @ObservationIgnored var screenReaderWatchInterval = Duration.seconds(3)
    @ObservationIgnored var voiceOverObserver: NSObjectProtocol?

    // MARK: Statistics state

    /// The views that show the statistics. Nothing is read while there are
    /// none.
    private(set) var statisticsViewers: Set<StatisticsViewer> = []
    /// Whether the application is the active one. The readings stop when it
    /// is not, and start again with a new baseline.
    var applicationActive = true {
        didSet {
            if applicationActive != oldValue {
                refreshStatistics()
                refreshGraphicsNotice()
                refreshNetworkWatch()
                refreshJit()
            }
        }
    }
    /// The figures behind the screens, from the driver's readings.
    private(set) var statisticsMeter = StatsMeter()
    /// Holds back the readings that draw what the views show already.
    @ObservationIgnored private var statisticsGate = StatsPublishGate<Readout.Drawn>()
    /// A launch for automation prints each reading.
    var printsStatistics = false
    /// Waits between readings; a test paces them by hand.
    @ObservationIgnored var statisticsSleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    @ObservationIgnored private var statisticsTask: Task<Void, Never>?
    /// Tells a loop that ended from the one that replaced it.
    @ObservationIgnored private var statisticsGeneration = 0

    // MARK: Graphics notice state

    /// Whether the session says Android ran out of graphics memory, from the
    /// runtime's count of the refusals the guest was answered with.
    private(set) var graphicsNotice = GraphicsNotice()
    /// Waits between readings of that count; a test paces them by hand.
    @ObservationIgnored var graphicsNoticeSleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    /// The moment a reading is taken at; a test sets its own.
    @ObservationIgnored var graphicsNoticeClock: @MainActor () -> ContinuousClock.Instant = { .now }
    @ObservationIgnored private var graphicsNoticeTask: Task<Void, Never>?
    @ObservationIgnored private var graphicsNoticeGeneration = 0
    /// When to end the cached app that holds the most graphics memory, from
    /// the same readings; nothing of it shows.
    @ObservationIgnored private(set) var graphicsReclaim = GraphicsReclaim()

    // MARK: Thermal notice state

    /// Whether the session says the frame rate is lowered while the device is
    /// hot, once in a run, from the runtime's thermal policy.
    private(set) var thermalNotice = ThermalNotice()
    /// The policy has announced the cap and the session screen is not up yet.
    @ObservationIgnored private var thermalCapWaiting = false
    /// Waits out the notice's time; a test paces it by hand.
    @ObservationIgnored var thermalNoticeSleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    @ObservationIgnored private var thermalNoticeTask: Task<Void, Never>?
    /// Settings' "When hot" under Performance: whether Android is told how hot
    /// the device is, so that it limits the frame rate while the device is
    /// hot. On is the default. Kept for the install (`HeatLimitPreference`).
    var heatLimitOn = true
    /// The launch decided it (automation): the value belongs to that run and
    /// is not kept.
    var heatLimitDecidedByLaunch = false
    /// What is kept of the option across launches; nil where the launch
    /// decided it.
    var heatLimitToKeep: Bool? { heatLimitDecidedByLaunch ? nil : heatLimitOn }
    /// The most frames a second the screen the window is on can show: 60 until
    /// the scene says (`screenDidChange`, on connection and on every move
    /// between screens), and then 120 on a ProMotion iPhone or iPad.
    private(set) var screenMaximumFramesPerSecond = 60
    /// The host can show the 120 Hz display: an iPhone or an iPad. An iPad app
    /// on a Mac or on a Vision Pro has none that the display link can pace, so
    /// nothing of the setting is offered or asked there (the gate the list of
    /// Android's apps uses).
    var hostOffersHighRefresh: Bool {
        host.idiom != .mac && padAppHost.hasHomeScreen
    }
    /// Whether Home offers "120 Hz": the screen shows at least 100 frames a
    /// second, the host is an iPhone or an iPad, and the display the next
    /// start gives the guest is a size whose 120 Hz mode the EDID can state (a
    /// 12.9- or 13-inch iPad Pro at its native size is not, and is offered the
    /// setting again at a smaller resolution).
    var highRefreshOffered: Bool {
        HighRefresh.isOffered(
            maximumFramesPerSecond: screenMaximumFramesPerSecond, displaySize: nextDisplaySize,
            hostIsMacOrVision: !hostOffersHighRefresh)
    }
    /// The size in pixels of the display the next start lays out, once Home has
    /// read the window.
    private var nextDisplaySize: (width: Int, height: Int)? {
        resolutionLayout.map { ($0.width, $0.height) }
    }
    /// Home's "120 Hz" in Performance: whether the next Android start asks
    /// for a 120 Hz display. Off is the default. Kept for the install
    /// (`HighRefreshPreference`).
    var highRefreshOn = false
    /// The launch decided it (automation): the value belongs to that run and
    /// is not kept.
    var highRefreshDecidedByLaunch = false
    /// What is kept of the setting across launches; nil where the launch
    /// decided it.
    var highRefreshToKeep: Bool? { highRefreshDecidedByLaunch ? nil : highRefreshOn }
    /// Whether a start now asks for the 120 Hz display: the setting is on and
    /// the offer holds. The runtime decides at the start (the screen of the
    /// display's own view and the size it boots at) and the run has what it
    /// granted (`runStarted`).
    var asksForHighRefresh: Bool { asksForHighRefresh(displaySize: nextDisplaySize) }
    /// The same for a start whose display size is known: the size it boots at.
    func asksForHighRefresh(displaySize: (width: Int, height: Int)?) -> Bool {
        HighRefresh.asksAtBoot(
            isOn: highRefreshOn, maximumFramesPerSecond: screenMaximumFramesPerSecond,
            displaySize: displaySize, hostIsMacOrVision: !hostOffersHighRefresh)
    }
    /// Whether the run in progress has the 120 Hz display, as the runtime
    /// granted it, which no change of the setting alters until Android next
    /// starts. False until the runtime has said.
    private(set) var runHasHighRefresh = false
    /// Home's "120 Hz". It applies at the next start (Home shows only while no
    /// guest runs).
    func setHighRefresh(_ on: Bool) {
        guard on != highRefreshOn else { return }
        highRefreshOn = on
    }
    /// What this run's display is: the runtime granted the 120 Hz display, or
    /// did not (it checks the screen, the host and the size once at the start,
    /// whatever the start asked).
    func runStarted(withHighRefresh granted: Bool) {
        runHasHighRefresh = granted
    }

    /// The thermal policy holds the device hot (iOS's critical state, the
    /// policy's hold after it included), whether or not Android is told.
    /// Settings' footnote reads it.
    private(set) var deviceIsHot = false
    /// The alert that asks to turn the limit off is up, over the notice.
    private(set) var askingToTurnOffHeatLimit = false

    // MARK: Network notice state

    /// Whether iOS denies Andarium the network, from the path watcher. The
    /// words and the Settings button are `NetworkWords`; nothing here is a
    /// request of Andarium's own.
    private(set) var networkNotice = NetworkNotice()
    /// Watches the system's network path while a watch is wanted. The device
    /// application has one over the system's path monitor; a test gives its
    /// own, and the Simulator preview has none.
    @ObservationIgnored var networkWatcher: NetworkAccessWatcher? {
        didSet {
            oldValue?.setWanted(false)
            networkWatcher?.changed = { [weak self] in self?.networkWatcherChanged() }
            refreshNetworkWatch()
        }
    }
    init(
        guest: Guest?, host: HostDevice, system: SystemInfo, capabilities: Capabilities,
        createRequirementBytes: UInt64
    ) {
        lifecycle = Lifecycle(guest: guest)
        self.host = host
        self.system = system
        self.capabilities = capabilities
        self.createRequirementBytes = createRequirementBytes
        snapshots.app = self
        // The notes show an entry about a feature only where the feature's own
        // rule offers it on this device.
        whatsNew.showsFeature = { [weak self] feature in
            guard let self else { return true }
            return switch feature {
            case .addToHomeScreen: hostOffersAddToHomeScreen
            }
        }
    }

    var guest: Guest? {
        get { lifecycle.guest }
        set {
            lifecycle.guest = newValue
            persistSoon()
        }
    }

    var phase: Phase {
        get { lifecycle.phase }
        set { lifecycle.phase = newValue }
    }

    var notice: HomeNotice? {
        get { lifecycle.notice }
        set { lifecycle.notice = newValue }
    }

    /// Where the session records and MetricKit's reports are kept: a bug
    /// report carries them (`BugReportAttachments`).
    var diagnosticsDirectory: URL?
    /// The application's container paths, which never leave in a bug report.
    var diagnosticsContainers: [String] = []

    /// At launch: keeps `diagnostics` for bug reports and reads how the
    /// previous session ended (`PreviousSession`), before the next boot writes
    /// its own record. A bad end of a guest that still exists says so on Home.
    func adoptDiagnostics(in diagnostics: URL, containers: [String], endedOutOfForeground: Bool) {
        diagnosticsDirectory = diagnostics
        diagnosticsContainers = containers
        // An earlier run a process before left there is not this process's.
        PreviousSession.settleEarlierRun(in: diagnostics)
        guard let previous = PreviousSession.read(in: diagnostics, endedOutOfForeground: endedOutOfForeground)
        else {
            // No session record to hold: the start's own record still goes
            // aside for the bug report.
            PreviousSession.keep(in: diagnostics)
            return
        }
        print("Andarium: \(previous.record.summary)")
        if guest != nil, let notice = previous.notice { self.notice = notice }
        previousBootRamdisk = previous.record.ramdisk
        // A start with root that ended before Android booted is told by the
        // record alone, which names the ramdisk of the boot that ended: a
        // guest kernel's panic ends the process, so no run end is seen
        // (`RootFallbackRule`). The record stays where it is until the pause
        // is saved.
        if guest?.root.on == true, RootFallbackRule.endedBeforeBoot(previous.record) {
            previousSessionHeld = true
            applyRootFallback()
            keepPreviousSessionIfDecided()
        } else {
            PreviousSession.keep(in: diagnostics)
        }
    }

    var canCreate: Bool { host.hasRoom(for: createRequirementBytes) }
    /// Less free space than Android's first start wants: Home says so before
    /// a start would fail.
    var lowSpace: Bool { !host.hasRoom(for: Limits.firstStartReserveBytes) }

    /// What a start of the guest would be, for the words on Start.
    func bootKind(_ mode: StartMode) -> BootKind {
        guest == nil ? .normal : plan.bootKind(mode)
    }

    /// Reads the plan from the guest's files again: after the guest is made,
    /// erased or has run, which change what a start is.
    func refreshPlan() {
        guard let driver else { return }
        // A record that cannot be read has no plan; the start then fails
        // with the reason.
        plan = driver.startPlan() ?? .normal
        // A run may have taught the store a lens's colour data, which changes
        // the layout the cameras make and so what a session holds.
        refreshCameraReserve()
    }

    /// Reads the memory a live camera session holds from the runtime again, for
    /// the sheet's graphics memory: after the Camera choice or the access to the
    /// cameras changes, after a run, and when the System sheet opens, because
    /// the cameras the host has and the colour data they have learned decide
    /// the layout. It matters only
    /// while the Camera choice is on, so that is when the runtime is asked; the
    /// policy keeps its own figure meanwhile. Questions are answered in order,
    /// and the result is the task that ends with this one.
    @discardableResult
    func refreshCameraReserve() -> Task<Void, Never> {
        let before = cameraReserveTask
        guard let planner = cameraReservePlanner, guest?.camera == true else { return before ?? Task {} }
        let task = Task { [weak self] in
            await before?.value
            guard let self, self.guest?.camera == true else { return }
            let current = self.host.memory
            let plan = await Task.detached(priority: .utility) { planner(current) }.value
            if self.host.memory == current, plan.policy != current { self.host.memory = plan.policy }
        }
        cameraReserveTask = task
        return task
    }

    /// Records the guest's choices a moment after the last change, so a
    /// drag on a ruler writes once.
    private func persistSoon() {
        guard driver != nil else { return }
        persistTask?.cancel()
        persistTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            self?.persistNow()
        }
    }

    /// Writes the guest's choices now; whether they were written. A failure
    /// tells the person, and a caller whose decision must outlast the process
    /// (`applyRootFallback`) does not go on as if it were saved.
    @discardableResult
    func persistNow() -> Bool {
        persistTask?.cancel()
        guard let driver, let guest else { return false }
        do {
            try driver.save(guest)
            return true
        } catch {
            alert = AppAlert(title: String(localized: "Settings not saved"), message: error.localizedDescription)
            return false
        }
    }

    func create(_ guest: Guest) {
        guard let driver else {
            lifecycle.create(guest)
            plan = .firstStart
            forgetSnapshots()
            return
        }
        runBusy(
            BusyState(
                title: String(localized: "Creating virtual machine"),
                detail: String(localized: "Setting up Andarium with \(Formats.size(gib: guest.effectiveCapacityGiB))."), progress: 0
            ),
            failure: String(localized: "The virtual machine could not be created")
        ) { progress in
            let made = try await driver.create(guest, progress: progress)
            self.lifecycle.create(made)
            self.forgetSnapshots()
            self.refreshPlan()
        }
    }

    /// Restore: the data goes back to what the update that did not finish
    /// started from, and the guest does not start. Home then shows the
    /// ordinary pending update. A snapshot operation under way finishes first.
    func restoreUpdate() async {
        guard let driver else {
            if let planAfterRestore { plan = planAfterRestore }
            return
        }
        guard !restoringUpdate else { return }
        restoringUpdate = true
        defer { restoringUpdate = false }
        do {
            try await driver.restoreUpdate()
            // The data is what the update started from: its apps may differ.
            androidApps.invalidated()
        } catch {
            alert = AppAlert(title: String(localized: "The virtual machine could not be restored"), message: error.localizedDescription)
        }
        refreshPlan()
    }

    /// Erase guest: everything the guest stored is deleted, its choices
    /// included, and Andarium returns to the first-launch screen.
    func erase() {
        guard let driver, guest != nil else {
            lifecycle.erase()
            smsSender.forget()
            textMessageNotice.reset()
            plan = .firstStart
            forgetSnapshots()
            androidAppsErased()
            return
        }
        runBusy(
            BusyState(title: String(localized: "Erasing virtual machine"), detail: String(localized: "Deleting everything the virtual machine stored."), progress: 0),
            failure: String(localized: "The virtual machine could not be erased")
        ) { progress in
            // A choice saved a moment after the last change must not come
            // back after the files are gone.
            self.persistTask?.cancel()
            try await driver.erase(progress: progress)
            self.lifecycle.erase()
            self.smsSender.forget()
            self.textMessageNotice.reset()
            self.forgetSnapshots()
            self.androidAppsErased()
            self.resizePreset = nil
            self.sheet = nil
            self.refreshPlan()
        }
    }

    /// Runs `work` behind the busy overlay, with a failure alert if it throws.
    private func runBusy(
        _ state: BusyState, failure: String,
        _ work: @escaping @MainActor (@escaping @Sendable (Double) -> Void) async throws -> Void
    ) {
        guard busy == nil else { return }
        busy = state
        Task { [weak self] in
            guard let self else { return }
            do {
                try await work { fraction in
                    Task { @MainActor in self.busy?.progress = fraction }
                }
            } catch {
                alert = AppAlert(title: failure, message: error.localizedDescription)
            }
            busy = nil
        }
    }

    // MARK: Low space

    /// The device is down to `freeBytes` and the runtime is pausing the guest.
    /// What needs the guest is let go (an operation, ADB over network), the
    /// session closes its Settings, and the screen over it says Android is
    /// being paused, offering nothing until it is.
    func guestPausingForSpace(freeBytes: UInt64) {
        host.freeStorageBytes = freeBytes
        abandonOperation()
        fileBrowser?.reset()
        stopAdbOverNetwork()
        spacePause = SpacePause(freeBytes: freeBytes, stage: .pausing)
    }

    /// The guest holds still: the screen offers Resume and Shut down.
    func guestPausedForSpace() {
        guard var pause = spacePause, pause.stage == .pausing else { return }
        pause.stage = .paused
        pause.freeBytes = host.freeStorageBytes
        spacePause = pause
    }

    /// The guest did not hold still after every attempt and runs on: the
    /// screen says so, and offers Shut down and a new attempt.
    func guestNotPausedForSpace() {
        guard var pause = spacePause, pause.stage == .pausing else { return }
        pause.stage = .notPaused
        pause.freeBytes = host.freeStorageBytes
        spacePause = pause
    }

    /// Check again, over a guest that did not pause: the driver reads the
    /// space and pauses the guest again, or lets the screen go when there is
    /// room for the guest to run on.
    func checkSpaceAfterPauseFailed() {
        guard var pause = spacePause, pause.stage == .notPaused, pause.working == nil else { return }
        guard let driver else {
            spacePause = nil
            return
        }
        pause.stage = .pausing
        spacePause = pause
        Task { [weak self] in
            let enough = await driver.checkSpaceAfterPauseFailed()
            guard let self, self.spacePause?.stage == .pausing, enough else { return }
            self.spacePause = nil
        }
    }

    /// Resume: the driver reads the space again and wakes the guest when
    /// there is enough; otherwise the screen says what is free now.
    func resumeAfterSpacePause() {
        guard var pause = spacePause, pause.stage == .paused, pause.working == nil else { return }
        guard let driver else {
            // The preview plays the pause away.
            spacePause = nil
            return
        }
        pause.working = .resuming
        spacePause = pause
        Task { [weak self] in
            let resumed = await driver.resumeAfterSpacePause()
            // A guest that paused again meanwhile has a screen of its own.
            guard let self, var pause = self.spacePause, pause.working == .resuming else { return }
            if resumed {
                self.spacePause = nil
            } else {
                pause.freeBytes = self.host.freeStorageBytes
                pause.stillShort = true
                pause.working = nil
                self.spacePause = pause
            }
        }
    }

    /// Shut down: Android is woken and asked to power off, which writes what
    /// it has and ends the run. The run ending takes the screen away.
    func shutDownAfterSpacePause() {
        guard var pause = spacePause, pause.stage != .pausing, pause.working == nil else { return }
        guard let driver else {
            powerOff()
            return
        }
        pause.working = .shuttingDown
        spacePause = pause
        Task { [weak self] in
            do {
                try await driver.shutDownAfterSpacePause()
            } catch {
                guard let self, self.spacePause?.working == .shuttingDown else { return }
                // The guest runs on, and its space is watched again: the
                // screen comes back with the new figure if it is still short.
                self.spacePause = nil
                self.alert = AppAlert(
                    title: String(localized: "Android could not be shut down"),
                    message: String(localized: "It could not be asked to power off. Try again, or close Andarium and open it again."))
            }
        }
    }

    // MARK: ADB over network

    /// ADB over network may be switched on: the guest's USB debugging is on.
    /// The preview has no guest to ask and says yes unless a screen says no.
    var adbOverNetworkAvailable: Bool {
        driver == nil ? session.usbDebugging != false : session.usbDebugging == true
    }

    /// The file browser may list and move files: Android has its storage up.
    /// The guest's USB debugging setting does not matter, since the browser
    /// uses the application's own channel. The preview has no guest to ask
    /// and says yes unless a screen says no.
    var filesAvailable: Bool {
        driver == nil ? session.storageReady != false : session.storageReady == true
    }

    /// The toggle in Settings. Turning it on opens the listener on the
    /// device's network addresses, which the guest's USB debugging being on
    /// allows; turning it off, USB debugging going off, or the run ending
    /// closes it. It lasts for this run only.
    func setAdbOverNetwork(_ on: Bool) {
        guard capabilities.adbOverNetwork else { return }
        guard on else {
            stopAdbOverNetwork()
            return
        }
        guard !session.adb, case .running(.normal) = phase, adbOverNetworkAvailable else { return }
        do {
            try driver?.startAdbOverNetwork()
        } catch {
            session.adbProblem = AdbNetworkMessages.cannotStart(error)
            return
        }
        withAnimation(.snappy) {
            session.adb = true
            session.adbProblem = nil
            session.adbAddress = driver?.localNetworkAddress() ?? (driver == nil ? SessionState.previewAddress : nil)
        }
        watchAdbOverNetwork()
    }

    /// Closes the listener and switches the toggle off.
    func stopAdbOverNetwork() {
        adbWatcher?.cancel()
        adbWatcher = nil
        driver?.stopAdbOverNetwork()
        guard session.adb || session.adbAddress != nil else { return }
        withAnimation(.snappy) {
            session.adb = false
            session.adbAddress = nil
        }
    }

    /// The run is over or starts again: nothing about the guest's settings
    /// holds for the next one.
    private func endAdbOverNetwork() {
        stopAdbOverNetwork()
        session.usbDebugging = nil
        session.storageReady = nil
        session.adbProblem = nil
        session.oldApps.reset()
    }

    /// The address a computer connects to, which changes with the network.
    /// The preview keeps the one it was given.
    func refreshAdbAddress() {
        guard session.adb, let driver else { return }
        let address = driver.localNetworkAddress()
        if session.adbAddress != address { session.adbAddress = address }
    }

    /// Reads the guest's USB debugging setting. ADB over network ends when
    /// it is off.
    func refreshUsbDebugging() async {
        // A guest paused for space is not asked: a connection waits for it.
        guard capabilities.adbOverNetwork, let driver, case .running(.normal) = phase,
              spacePause == nil, !readingUsbDebugging else { return }
        readingUsbDebugging = true
        defer { readingUsbDebugging = false }
        let current = session
        let value = await driver.usbDebugging()
        guard session === current, case .running(.normal) = phase, let value else { return }
        if current.usbDebugging != value { current.usbDebugging = value }
        if !value, current.adb { stopAdbOverNetwork() }
    }

    /// Reads whether Android has its internal storage up, until it has: once
    /// it is, it stays up for the run.
    func refreshStorage() async {
        guard let driver, case .running(.normal) = phase, spacePause == nil, !readingStorage,
              session.storageReady != true else { return }
        readingStorage = true
        defer { readingStorage = false }
        let current = session
        let value = await driver.storageReady()
        guard session === current, case .running(.normal) = phase, let value else { return }
        if current.storageReady != value { current.storageReady = value }
        // The file browser's check is the moment Android is up for the apps too.
        await listOldAppsOnce()
    }

    /// While Settings is open: the address and the settings, read again every
    /// few seconds until the sheet goes and the task is cancelled. The two
    /// readings are independent, so the file browser's storage check never
    /// waits for the USB debugging one.
    func watchSettings() async {
        let reader = settingsOpened()
        defer { settingsClosed(reader) }
        while !Task.isCancelled {
            refreshAdbAddress()
            async let debugging: Void = refreshUsbDebugging()
            async let storage: Void = refreshStorage()
            // A listing that got no answer is asked again while Settings is open.
            async let oldApps: Void = listOldAppsOnce()
            // The screen reader's switch follows a change made in Android meanwhile.
            async let screenReader: Void = refreshScreenReader()
            _ = await (debugging, storage, oldApps, screenReader)
            try? await Task.sleep(for: .seconds(3))
        }
    }

    /// While ADB over network is on and Settings is closed, the address is
    /// read every few seconds and the setting every ten, so that turning
    /// USB debugging off in the guest turns this off too.
    private func watchAdbOverNetwork() {
        adbWatcher?.cancel()
        guard driver != nil else { return }
        adbWatcher = Task { [weak self] in
            var tick = 0
            while !Task.isCancelled {
                try? await Task.sleep(for: self?.adbWatchInterval ?? .seconds(2))
                guard let self, !Task.isCancelled else { return }
                tick += 1
                refreshAdbAddress()
                if tick % 5 == 0 { await refreshUsbDebugging() }
            }
        }
    }

    // MARK: Android operations

    /// Runs one of Android's operations behind the busy overlay: Settings
    /// has dismissed, the overlay covers the screens, and a Cancel button
    /// stops it. `work` reports through the update closure and throws
    /// `CancellationError` when the person cancelled; any other error is an
    /// alert. `finished` runs once the overlay has gone. With `ending`, the
    /// overlay stays up instead and says how the operation ended (its words
    /// on success; `failure` and the error's on failure) until the person
    /// closes it; cancelling takes the overlay away, except that an operation
    /// which returns its outcome after the cancel (an install whose app was in
    /// by then) shows that outcome.
    ///
    /// `failed` says how a failure ends instead of the alert, for a task
    /// started from a sheet, where an alert of the screens could not show.
    /// `finished` does not run for a cancelled operation.
    func runOperation<Output: Sendable>(
        _ state: BusyState, failure: String,
        ending: ((Output) -> BusyWords)? = nil,
        failed: (@MainActor (String) -> Void)? = nil,
        _ work: @escaping @MainActor (@escaping @Sendable (BusyUpdate) -> Void) async throws -> Output,
        finished: @escaping @MainActor (Output) -> Void = { _ in }
    ) {
        guard busy == nil else { return }
        operationEpoch += 1
        let epoch = operationEpoch
        operationCancelled = false
        var state = state
        state.cancel = BusyCancel { [weak self] in self?.cancelOperation() }
        busy = state
        operation = Task { [weak self] in
            guard let self else { return }
            var result: Output?
            var failureWords: String?
            do {
                result = try await work { update in
                    Task { @MainActor in self.apply(update, epoch: epoch) }
                }
            } catch is CancellationError {
            } catch {
                if operationEpoch == epoch {
                    if ending == nil {
                        if let failed {
                            failed(error.localizedDescription)
                        } else {
                            alert = AppAlert(title: failure, message: error.localizedDescription)
                        }
                    } else {
                        failureWords = error.localizedDescription
                    }
                }
            }
            // The run ending, or a restart, took the operation over already.
            guard operationEpoch == epoch else { return }
            operation = nil
            if let ending, let result {
                let words = ending(result)
                busy = ended(title: words.title, detail: words.detail, style: words.style)
            } else if ending != nil, let failureWords {
                busy = ended(title: failure, detail: failureWords, failed: true)
            } else {
                busy = nil
            }
            // A cancelled operation hands nothing on, even when its work
            // finished before it saw the cancel.
            if ending == nil, let result, !operationCancelled { finished(result) }
        }
    }

    /// The overlay of an operation that is over: how it ended, and the button
    /// that closes it.
    private func ended(title: String, detail: String, failed: Bool) -> BusyState {
        ended(title: title, detail: detail, style: failed ? .failure : .success)
    }

    private func ended(title: String, detail: String, style: BusyResult.Style) -> BusyState {
        BusyState(
            title: title, detail: detail,
            result: BusyResult(style: style, dismiss: BusyCancel { [weak self] in self?.busy = nil })
        )
    }

    private func apply(_ update: BusyUpdate, epoch: Int) {
        guard epoch == operationEpoch, !operationCancelled, busy != nil else { return }
        // The new value is worked out before the write: reading `busy` while
        // writing through it is an overlapping access.
        if let progress = update.progress, let current = busy {
            busy?.progress = max(current.progress ?? 0, progress)
        }
        if let detail = update.detail { busy?.detail = detail }
        if update.cancellable == false { busy?.cancel = nil }
        if update.cancellable == true, busy?.cancel == nil, busy?.result == nil {
            busy?.cancel = BusyCancel { [weak self] in self?.cancelOperation() }
        }
    }

    /// The overlay's Cancel: the operation stops and cleans up while the
    /// overlay stays, so the session returns with nothing left behind.
    private func cancelOperation() {
        guard operation != nil, !operationCancelled else { return }
        operationCancelled = true
        busy?.cancel = nil
        busy?.detail = String(localized: "Cancelling\u{2026}")
        operation?.cancel()
    }

    /// The guest ended or restarted under an operation: its stream is gone
    /// with the boot, so the overlay goes and no alert is raised over the
    /// screen that says the guest stopped.
    private func abandonOperation() {
        // The words of a finished operation belong to the run that ended.
        if operation == nil, busy?.result != nil { busy = nil }
        guard operation != nil else { return }
        operationEpoch += 1
        operation?.cancel()
        operation = nil
        finishLegacyInstallQuestion(nil)
        busy = nil
    }

    /// Create bug report: Settings has dismissed. Android collects its report
    /// (minutes, under emulation) and the finished zip goes to the share
    /// sheet, where Save to Files is one of the actions.
    func createBugReport() {
        guard canCreateBugReport else { return }
        runOperation(
            Self.bugReportState, failure: Self.bugReportFailure, bugReportWork(),
            finished: { [weak self] file in self?.share(file) }
        )
    }

    /// The overlay of a bug report under way, the same wherever it is asked for.
    static let bugReportState = BusyState(
        title: String(localized: "Creating bug report"),
        detail: String(localized: "The virtual machine is collecting its logs and state. This can take several minutes."), progress: 0
    )
    static let bugReportFailure = String(localized: "Bug report not created")

    /// A normal session of a runtime that can make a bug report, with the
    /// guest's one adb channel free.
    var canCreateBugReport: Bool {
        capabilities.bugReport && fileBrowser?.busy != true && {
            if case .running(.normal) = phase { return true }
            return false
        }()
    }

    /// What making a bug report does, whoever asks for it: Android's report,
    /// and the host's records and the guest's counters inside it.
    func bugReportWork() -> @MainActor (@escaping @Sendable (BusyUpdate) -> Void) async throws -> URL {
        let root = rootReportEntry()
        return { [driver, diagnosticsDirectory, diagnosticsContainers, plays = playsBugReport] update in
            let file = if let driver, !plays {
                try await driver.createBugReport(update: update)
            } else {
                try await Self.playBugReport(update)
            }
            // The host's records and the guest's counters travel inside
            // Android's own report.
            if let diagnosticsDirectory {
                let stalls = await driver?.guestStallFiles() ?? []
                let guest = root.map { stalls + [$0] } ?? stalls
                let earlierRun = driver?.retiredAnEarlierRun ?? false
                _ = await Task.detached(priority: .userInitiated) {
                    BugReportAttachments.attach(
                        to: file, from: diagnosticsDirectory, containers: diagnosticsContainers, guest: guest,
                        includingEarlierRun: earlierRun)
                }.value
            }
            // A cancel that came as the report finished: nobody takes the file.
            if Task.isCancelled {
                try? FileManager.default.removeItem(at: file)
                throw CancellationError()
            }
            return file
        }
    }

    /// Opens the share sheet on a finished report; false when there is no
    /// window to show it. A test puts its own here.
    var presentShare: (URL, @escaping @MainActor () -> Void) -> Bool = { file, finished in
        BugReportSharing.present(file, finished: finished)
    }

    /// The report is deleted on every outcome of the sheet.
    private func share(_ file: URL) {
        shareSheetUp = true
        let shown = presentShare(file) { [weak self] in
            self?.shareSheetUp = false
            try? FileManager.default.removeItem(at: file)
        }
        if !shown {
            shareSheetUp = false
            try? FileManager.default.removeItem(at: file)
        }
    }

    /// The preview's report: the same steps on the same overlay, ending in
    /// the share sheet with a small file.
    static func playBugReport(_ update: @Sendable (BusyUpdate) -> Void) async throws -> URL {
        for step in 1...36 {
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(progress: Double(step) / 36 * 0.9))
        }
        update(BusyUpdate(progress: 0.9, detail: String(localized: "Copying the report\u{2026}")))
        for step in 1...8 {
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(progress: 0.9 + 0.1 * Double(step) / 8))
        }
        try FileManager.default.createDirectory(at: BugReportStaging.directory, withIntermediateDirectories: true)
        let file = BugReportStaging.directory.appendingPathComponent("bugreport-preview.zip")
        // An empty zip: its end-of-central-directory record.
        try Data([0x50, 0x4B, 0x05, 0x06] + [UInt8](repeating: 0, count: 18)).write(to: file)
        return file
    }

    /// What the hosted tests put in place of the picked file's access, to see
    /// when it is held.
    @ObservationIgnored
    var accessPickedFileStub: (@MainActor (URL) -> (@MainActor () -> Void))?

    /// Starts the access that a picked file's URL carries and returns what
    /// ends it.
    private func accessPickedFile(_ url: URL) -> @MainActor () -> Void {
        if let accessPickedFileStub { return accessPickedFileStub(url) }
        guard url.startAccessingSecurityScopedResource() else { return {} }
        return { url.stopAccessingSecurityScopedResource() }
    }

    /// Install APK: `url` is what the Files picker returned. Settings has
    /// dismissed and the install runs behind the overlay, which stays up at the
    /// end with how it went until the person closes it. A file that is not an
    /// .apk, .apks, .xapk or .apkm, or is an .apk that Android cannot take by
    /// its size, is refused on the same overlay, at once; an archive of an
    /// app's parts is read, and refused or installed, by the operation. The
    /// file's security scope is held to the end of the install. `staged` is the
    /// folder of ours that holds a copy the picker made: it is removed at each
    /// refusal and when the install is over, across Install anyway too.
    func installPicked(_ url: URL, staged: PickedFolder? = nil) {
        guard busy == nil else { return }
        guard fileBrowser?.busy != true else {
            alert = AppAlert(
                title: Self.installFailure, message: String(localized: "Wait for the file transfer to finish, then install the app."))
            return
        }
        let name = url.lastPathComponent
        if let refusal = InstallRefusal(fileName: name) {
            busy = ended(title: Self.installFailure, detail: refusal.message, failed: true)
            return
        }
        let endAccess = accessPickedFile(url)
        // The size here is what the picker can say: an iCloud file that is not
        // down has none yet, and the install reads the real one when it has
        // the file.
        let size = UInt64(max((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0, 0))
        // The size limit is an .apk's: an archive may be larger, and each APK
        // inside it is held to the limit by the operation.
        if InstallRefusal.isPlainApk(fileName: name), let refusal = InstallRefusal(sizeBytes: size) {
            endAccess()
            busy = ended(title: Self.installFailure, detail: refusal.message, failed: true)
            return
        }
        let detail = size > 0 ? "\(name) \u{00B7} \(Formats.size(bytes: size))" : name
        runOperation(
            BusyState(title: String(localized: "Installing app"), detail: detail, progress: 0),
            failure: Self.installFailure,
            ending: { (result: ApkInstallResult) in result.outcome.words(fileName: name, oldAppHint: result.oldAppHint) },
            { [driver] update in
                defer {
                    endAccess()
                    staged?.discard()
                }
                let end = try await self.installAskingAboutLegacyApps(detail: detail) { bypass, update in
                    if let driver {
                        return try await driver.installApk(at: url, bypassLowTargetSdkBlock: bypass, update: update)
                    } else {
                        try await self.playInstall(bypassLowTargetSdkBlock: bypass, update)
                        return self.previewInstallOutcome
                    }
                }
                // The app is in. Old app compatibility lists it if it applies; after an
                // Install anyway of an app built for an Android before 4.0 the result
                // says where the switch is, when the listing names the app the install added.
                var hint = false
                if let before = end.oldAppsBefore {
                    hint = await self.installAddedOldApp(listedBefore: before)
                } else {
                    Task { await self.refreshOldApps() }
                }
                // The app is in Android's list now: the apps Andarium offers follow it.
                Task { await self.refreshAndroidApps(afterAChange: true) }
                // 100% for a moment, then the result. Nothing is left to stop, so
                // the Cancel that came back for an app's data goes.
                update(BusyUpdate(progress: 1, cancellable: false))
                try? await Task.sleep(for: .milliseconds(500))
                return ApkInstallResult(outcome: end.outcome, oldAppHint: hint)
            }
        )
    }

    /// The install, as `LegacyInstallFlow` has it: the ordinary attempt first,
    /// which is exactly what Install APK always did. When Android refuses the
    /// app for the version it was built for, and only then, the person is
    /// asked; "Install anyway" sends the same file again with the package
    /// manager's flag, once, and Cancel ends the install as a cancelled one.
    /// The file's security scope is the caller's and spans the question; each
    /// attempt reads the file and checks the room afresh.
    private func installAskingAboutLegacyApps(
        detail: String,
        attempt: (_ bypassLowTargetSdkBlock: Bool, _ update: @escaping @Sendable (BusyUpdate) -> Void) async throws -> ApkInstallOutcome
    ) async throws -> LegacyInstallEnd {
        let epoch = operationEpoch
        var flow = LegacyInstallFlow()
        var outcome = ApkInstallOutcome.installed
        var oldAppsBefore: [OldApp]?
        while true {
            switch flow.step {
            case let .attempt(bypass):
                installAttemptTag += 1
                let tag = installAttemptTag
                let update: @Sendable (BusyUpdate) -> Void = { update in
                    Task { @MainActor in
                        guard self.installAttemptTag == tag else { return }
                        self.apply(update, epoch: epoch)
                    }
                }
                do {
                    outcome = try await attempt(bypass, update)
                    flow.attemptEnded(.installed)
                } catch let refused as LegacyApkRefused {
                    flow.attemptEnded(.tooOldTarget(refused.target))
                }
            case let .ask(target):
                // The file is not being sent while the person thinks: the
                // overlay says which file. Its Cancel stays, though the alert
                // covers it: an alert that cannot present (another one, or
                // a popover, is in its way) must not leave the install with
                // no way out. That Cancel closes the question too.
                installAttemptTag += 1
                busy?.cancel = BusyCancel { [weak self] in self?.cancelOperation() }
                busy?.detail = detail
                guard let installAnyway = await askAboutLegacyInstall(target) else {
                    flow.stopped()
                    continue
                }
                flow.answered(installAnyway: installAnyway)
                if installAnyway {
                    // The bar starts over for the second attempt.
                    busy?.progress = 0
                    // An app built for an Android before 4.0 may be one Old app
                    // compatibility lists: the listing as the file goes again is what
                    // the listing after it is compared with.
                    if OldAppInstallHint.isOldTarget(target) { oldAppsBefore = await oldAppsBeforeInstall() }
                }
            case let .finished(ending):
                switch ending {
                case .installed: return LegacyInstallEnd(outcome: outcome, oldAppsBefore: oldAppsBefore)
                case .cancelled: throw CancellationError()
                // Today only a second too-old refusal ends this way, and
                // `ApkInstall` words one of those as an ordinary failure
                // itself, so these are the same words. A later route to
                // `.failed` for another cause needs words of its own: these
                // would name the wrong one.
                case .failed: throw AdbFailure(message: PackageInstallMessages.message(forCode: PackageInstallAnswer.tooOldCode))
                }
            }
        }
    }

    /// Opens the question and waits for the answer: true for Install anyway,
    /// false for Cancel, nil when the install was stopped meanwhile (the guest
    /// ended, or the task was cancelled), which takes the question away.
    private func askAboutLegacyInstall(_ target: LegacyTarget) async -> Bool? {
        guard !Task.isCancelled else { return nil }
        return await withTaskCancellationHandler {
            await withCheckedContinuation { (continuation: CheckedContinuation<Bool?, Never>) in
                finishLegacyInstallQuestion(nil)
                if Task.isCancelled {
                    continuation.resume(returning: nil)
                    return
                }
                legacyInstallAnswer = continuation
                legacyInstallQuestion = LegacyInstallQuestion(target: target)
            }
        } onCancel: {
            Task { @MainActor [weak self] in self?.finishLegacyInstallQuestion(nil) }
        }
    }

    /// The alert's buttons.
    func answerLegacyInstall(installAnyway: Bool) {
        finishLegacyInstallQuestion(installAnyway)
    }

    private func finishLegacyInstallQuestion(_ answer: Bool?) {
        legacyInstallQuestion = nil
        let continuation = legacyInstallAnswer
        legacyInstallAnswer = nil
        continuation?.resume(returning: answer)
    }

    /// The title of an install that did not happen.
    static let installFailure = String(localized: "App not installed")

    /// The Files picker of Install APK or of the file browser's Import failed:
    /// the person is told, where nothing would otherwise happen. Settings,
    /// which both pickers are in, shows it over itself (`SessionSettingsSheet`):
    /// an alert of the screens beneath it would close the sheet to be shown.
    func documentPickerFailed(_ error: Error, title: String) {
        documentPickerProblem = AppAlert(title: title, message: error.localizedDescription)
    }

    /// What one attempt of the preview's install does instead of
    /// `playInstall`, for a preview that plays how the install can end.
    @ObservationIgnored
    var previewInstallAttempt: (@Sendable (_ bypassLowTargetSdkBlock: Bool, _ update: @Sendable (BusyUpdate) -> Void) async throws -> Void)?
    /// How the preview's install ends; an install ends as installed by default.
    @ObservationIgnored
    var previewInstallOutcome = ApkInstallOutcome.installed
    /// The list of old apps the preview and a UI test's probe session play,
    /// in place of the runtime's (`ScriptedOldApps`).
    @ObservationIgnored
    var scriptedOldApps: (any OldAppsSource)?
    /// Android's apps and the way to open one that a preview or a UI test's
    /// probe session plays in place of the runtime's (`ScriptedAndroidApps`).
    @ObservationIgnored
    var scriptedAndroidApps: (any AndroidAppsSource)?
    /// The iPad-app host a test plays in place of this process's
    /// (`padAppHost`).
    @ObservationIgnored
    var playedPadAppHost: IPadAppHost?
    /// A UI test's probe session makes the preview's bug report (`playBugReport`) though a runtime is
    /// there: its guest is none.
    @ObservationIgnored
    var playsBugReport = false

    private func playInstall(bypassLowTargetSdkBlock: Bool, _ update: @Sendable (BusyUpdate) -> Void) async throws {
        if let previewInstallAttempt {
            try await previewInstallAttempt(bypassLowTargetSdkBlock, update)
            return
        }
        try await Self.playInstall(update)
    }

    /// The preview's install: the file goes over, the guest takes its time.
    private static func playInstall(_ update: @Sendable (BusyUpdate) -> Void) async throws {
        for step in 1...20 {
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(progress: Double(step) / 20 * 0.8))
        }
        update(BusyUpdate(detail: String(localized: "The virtual machine is installing the app\u{2026}"), cancellable: false))
        try await Task.sleep(for: .milliseconds(1200))
    }

    /// Starts the guest in `mode`. Start on an update that did not finish
    /// tries the update again. The start carries any storage growth the
    /// person chose: the lifecycle applies it as the start begins, and the
    /// guest is saved with the new capacity before the runtime, which grows
    /// the data disk in every kind of start, is handed it. A snapshot
    /// operation under way is not a reason to say anything: the start is
    /// ignored, as the screens offer none while one runs (`snapshotGate`); a
    /// rename is not one, and the start waits for it.
    func start(_ mode: StartMode, withoutRoot: Bool = false) {
        guard !snapshotActivity.holdsGuestFiles else { return }
        // A pause of root decided from the previous session's record and not
        // saved yet is settled first: the run's own record replaces that one.
        // Without it saved, no run begins (the alert says why).
        saveRootStateIfPending()
        guard !rootStateUnsaved else { return }
        switch lifecycle.start(mode, host: host, plan: plan) {
        case let .allowed(kind):
            // A request to open an Android app that waited for this answer starts with it.
            androidAppsPersonStarted()
            restoredSnapshot = nil
            snapshots.cancelReads()
            frozenBootStage = nil
            bootTracker = BootTracker()
            bootStartedUnfinished = driver?.firstBootPending() ?? false
            slowStart.begin(at: clock(), kind: kind, firstBootPending: bootStartedUnfinished)
            preparingDisk = false
            diskProgress = nil
            jitLastStart = nil
            preparingJit = false
            if case let .update(from, _) = plan { bootFrom = from } else { bootFrom = nil }
            rootAppInstalling = false
            bootRun &+= 1
            // Whether this start asks for the Magisk ramdisk. Nothing of it is
            // saved: which ramdisk a boot used is the runtime's record of that
            // boot (`SessionRecord.ramdisk`).
            if let current = guest {
                guest?.root.startRooted = current.bootsRooted(
                    mode: mode, releaseCarriesRoot: rootAvailable, withoutRootOnce: withoutRoot)
                // "Start without root" is the person's answer to a stopped start.
                if withoutRoot, current.root.attention == .stoppedStart { guest?.root.attention = nil }
            }
            if let driver, var begun = guest {
                // The choices are written before the runtime is handed the
                // start. A start with root whose write failed begins without
                // root (the alert says the settings were not saved).
                if !persistNow(), begun.root.startRooted {
                    begun.root.startRooted = false
                    guest?.root.startRooted = false
                }
                driver.begin(guest: begun, mode: mode, kind: kind)
            }
        case .lowSpace: startBlocked = true
        // The screens offer no start the plan refuses (`StartPlan.allowsStart`),
        // so one that arrives anyway, from a keyboard or a launch, is ignored
        // without a message: Home already says what the guest waits for.
        case .unfinishedUpdate, .blocked: break
        case .needsNewerChip:
            alert = AppAlert(title: DeviceRequirement.title, message: DeviceRequirement.explanation(deviceName: host.name))
        case .noGuest: break
        case .relaunchRequired: break
        }
    }

    func finishBoot(_ started: BootKind) {
        // A guest that booted its recovery image on its own (a reboot into
        // recovery from the shell, Android's rescue after a loop of failed
        // boots) is the session the launch's recovery mode makes; its way
        // back, the recovery's reboot, is a restart into a normal session.
        let kind = bootTracker.recovery ? BootKind.recovery : started
        let firstStart = plan == .firstStart
        lifecycle.finishBoot(kind)
        rootBootFinished(kind)
        // The display lets touches through to Android's screen reader only
        // while it is on, so a run under VoiceOver reads it as soon as Android
        // is up and goes on reading; without VoiceOver Settings reads it when
        // it opens. Whether to turn it on is the person's to say, on the card.
        if kind != .recovery { screenReaderBootFinished() }
        if driver != nil {
            persistNow()
            // Android is up: the old apps are listed once, with Settings closed.
            listOldAppsAtStart()
        } else if kind != .recovery {
            // The preview plays the instance too: the first start sets
            // Android up, and an update or a retry ends.
            plan = .normal
            if firstStart {
                sessionPreset.guestScreen = .setup
                guest?.usedBytes = UInt64(2.1 * Double(Limits.bytesPerGiB))
            }
        }
        // A request to open an Android app that started this boot opens now,
        // and the app list is brought up to date once the guest has settled.
        androidAppsBootFinished(recovery: kind == .recovery)
    }

    func powerOff() {
        runEnded(RunEnd(result: 0), uptimeSeconds: nil)
    }

    func fail(_ end: RunEnd = RunEnd(result: -11), uptimeSeconds: Int? = 2530) {
        runEnd = end
        runUptimeSeconds = uptimeSeconds
        lifecycle.fail()
    }

    /// The runtime's run returned: the guest powered off, or stopped for a
    /// reason the failure screen reports.
    func runEnded(_ end: RunEnd, uptimeSeconds: Int?) {
        androidAppsRunEnded()
        abandonOperation()
        fileBrowser?.reset()
        endAdbOverNetwork()
        endTextMessageSession()
        spacePause = nil
        graphicsNotice = GraphicsNotice()
        graphicsReclaim.reset()
        thermalCapWaiting = false
        dismissThermalNotice()
        deviceIsHot = false
        cameraNoticeExpiry?.cancel()
        cameraNoticeExpiry = nil
        cameraNotice.runEnded()
        runEnd = end
        runUptimeSeconds = uptimeSeconds
        rootAppInstalling = false
        bootRun &+= 1
        if end.poweredOff {
            sheet = nil
            sessionPreset = SessionPreset()
            let showStats = session.showStats
            session = SessionState()
            session.showStats = showStats
        }
        lifecycle.phase = end.phase
        lifecycle.grewFromGiB = nil
        // The run changed the data disk: what Home states of the storage is
        // what the disk holds now, not what it held when the guest loaded.
        if let driver { lifecycle.guest?.usedBytes = driver.measuredUsedBytes() }
        rootRunEnded(end)
        persistNow()
        refreshPlan()
    }

    /// Android's own Restart boots the guest again in place. ADB and the
    /// install list belong to the run that ended.
    func restart() {
        androidAppsRunEnded()
        abandonOperation()
        fileBrowser?.reset()
        endAdbOverNetwork()
        endTextMessageSession()
        spacePause = nil
        graphicsNotice = GraphicsNotice()
        graphicsReclaim.reset()
        // The notice was for the screen that restarts, and it was announced
        // for the run, which goes on.
        dismissThermalNotice()
        frozenBootStage = nil
        sessionPreset = SessionPreset()
        let showStats = session.showStats
        session = SessionState()
        session.showStats = showStats
        statisticsMeter = StatsMeter()
        bootTracker = BootTracker()
        rootAppInstalling = false
        bootRun &+= 1
        // A restart before the start finished is the same start; a running
        // session's restart begins a boot of its own.
        var unfinished = false
        if case .booting = phase { unfinished = true }
        lifecycle.restart()
        slowStart.guestRestarted(at: clock(), startUnfinished: unfinished)
    }

    // MARK: Slow start

    /// The boot screen looked at the clock: puts the notice up once a start
    /// has run for `SlowStart.threshold`. Returns whether it just appeared.
    @discardableResult
    func updateSlowStart() -> Bool {
        guard case .booting = phase else { return false }
        return slowStart.update(at: clock())
    }

    /// The seconds the start under way has run, as the status line and the
    /// notice both show them.
    func bootElapsedSeconds() -> Int {
        slowStart.elapsedSeconds(at: clock())
    }

    /// The notice's Stop button: asks first, because a stop ends the virtual
    /// machine for this launch of the app (`stopStart` does it on the answer).
    func askToStop() {
        guard case .booting = phase, slowStart.began != nil, !slowStart.stopping else { return }
        confirmingStop = true
    }

    /// Ends the start under way: the answer to the dialog. The guest is asked
    /// to write back first and the runtime then ends the run, and `runEnded`
    /// follows; the notice says Stopping the whole time. The
    /// preview, with no runtime, ends it at once as a host stop does.
    func stopStart() {
        confirmingStop = false
        guard case .booting = phase, slowStart.requestStop() else { return }
        if let driver {
            driver.stopGuest()
        } else {
            runEnded(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: bootElapsedSeconds())
        }
    }

    func factoryReset() {
        lifecycle.factoryReset()
        androidApps.invalidated()
        // Only the preview knows the data is empty; on a device the data
        // disk stays and Android sets itself up again by itself.
        if driver == nil { plan = .firstStart }
        persistNow()
    }

    /// Settings' "Use the pointer as touch".
    func setPointerAsTouch(_ on: Bool) {
        pointerAsTouch = on
        driver?.setPointerAsTouch(on)
    }

    /// The Haptics switch as the screens show it: the guest's choice, which
    /// is on until the person turns it off.
    var hapticsOn: Bool { guest?.haptics ?? true }

    /// The launch plays none of the guest's haptics on the device, whatever
    /// the switch says (`HapticsLaunch`): the switch and the stored choice stay
    /// the person's, and the guest keeps its vibrator.
    var hapticsSilencedByLaunch = false

    /// Whether the device plays what the guest's vibrator asks for: the
    /// switch, unless the launch silences it.
    var hapticsPlays: Bool {
        if hapticsSilencedByLaunch { return false }
        return hapticsOn
    }

    /// Whether the application's own touch feedback (the side keys' click, the
    /// rulers' tick) plays. The Haptics switch does not govern it; only a
    /// launch that silences haptics does.
    var interfaceFeedbackPlays: Bool {
        if hapticsSilencedByLaunch { return false }
        return true
    }

    /// The Haptics switch. It asks for nothing and applies at once, in a run
    /// too: the guest keeps its vibrator either way.
    func setHaptics(_ on: Bool) {
        guard guest != nil else { return }
        guest?.haptics = on
        persistNow()
        driver?.setHaptics(on)
    }

    /// The Microphone switch as the screens show it: the guest's choice is on
    /// and iOS lets Andarium record. Access that was refused or taken away
    /// shows it off, and the guest hears nothing then.
    var microphoneShownOn: Bool { guest?.microphone == true && microphoneState != .denied }

    /// The Microphone switch. Turning it on asks iOS for access at once, the
    /// first time. After a refusal there is nothing to ask: the person is told
    /// and offered Andarium's page in Settings.
    func setMicrophone(_ on: Bool) {
        guard guest != nil else { return }
        guard on else {
            microphoneAwaitsSettings = false
            guest?.microphone = false
            persistNow()
            microphone?.setOn(false)
            return
        }
        switch microphoneState {
        case .denied:
            permissionPrompt = .microphone
        case .notAsked:
            guest?.microphone = true
            persistNow()
            // The controller asks iOS and reports the answer to
            // `microphoneAccessChanged`. A preview without one has no system
            // prompt to show and takes it as given.
            if let microphone { microphone.setOn(true) } else { microphoneAccessChanged(.granted) }
        case .granted:
            guest?.microphone = true
            persistNow()
            microphone?.setOn(true)
        }
    }

    /// iOS answered the microphone question, or the person changed Andarium's
    /// access in Settings. A refusal takes the choice back with it; access the
    /// person went to Settings to give turns the switch they tapped on.
    func microphoneAccessChanged(_ state: MicrophoneState) {
        microphoneState = state
        switch state {
        case .denied:
            if guest?.microphone == true {
                guest?.microphone = false
                persistNow()
            }
        case .granted:
            if microphoneAwaitsSettings, guest != nil, guest?.microphone != true {
                guest?.microphone = true
                persistNow()
            }
        case .notAsked:
            break
        }
        if state != .notAsked { microphoneAwaitsSettings = false }
    }

    /// The alert's Open Settings.
    func openMicrophoneSettings() {
        microphoneAwaitsSettings = true
        openSettings()
    }

    /// The Location switch as the screens show it: the guest's choice is on
    /// and iOS lets Andarium read the location, or has not been asked yet and
    /// the question is under way. Access that was refused or taken away, or
    /// Location Services off, shows it off, and the guest gets no position.
    var locationShownOn: Bool {
        guard guest?.location == true else { return false }
        return locationAuthorization.allowsLocating || locationAuthorization == .notDetermined
    }

    /// The Location switch. Turning it on asks iOS for access at once, the
    /// first time. Where there is nothing to ask (refused, restricted, Location
    /// Services off) the person is told why and, where Settings has the way
    /// out, offered Andarium's page there. It applies at once, in a run too.
    func setLocation(_ on: Bool) {
        guard guest != nil else { return }
        guard on else {
            locationAwaitsSettings = false
            locationAsking = false
            guest?.location = false
            persistNow()
            location?.setOn(false)
            return
        }
        switch locationAuthorization {
        case .servicesOff, .restricted, .denied:
            permissionPrompt = .location
        case .notDetermined:
            guest?.location = true
            locationAsking = true
            persistNow()
            // The controller asks iOS and reports the answer to
            // `locationAccessChanged`. A preview without one has no system
            // prompt to show and takes it as given.
            if let location { location.setOn(true) } else { locationAccessChanged(.precise) }
        case .precise, .approximate:
            guest?.location = true
            persistNow()
            location?.setOn(true)
        }
    }

    /// iOS answered the location question, or the person changed Andarium's
    /// access, Location Services or Precise Location. A refusal takes the
    /// choice back with it; access the person went to Settings to give turns
    /// on the switch they tapped.
    func locationAccessChanged(_ state: LocationAuthorization) {
        locationAuthorization = state
        switch state {
        case .servicesOff, .restricted, .denied:
            if guest?.location == true {
                guest?.location = false
                persistNow()
            }
        case .precise, .approximate:
            if locationAwaitsSettings, guest != nil, guest?.location != true {
                guest?.location = true
                persistNow()
            }
        case .notDetermined:
            if !locationAsking, guest?.location == true {
                guest?.location = false
                persistNow()
            }
        }
        if state != .notDetermined {
            locationAwaitsSettings = false
            locationAsking = false
        }
    }

    /// The alert's Open Settings: for a refusal, the switch the person tapped
    /// turns on when Andarium is active again with access.
    func openLocationSettings() {
        locationAwaitsSettings = locationAuthorization.refuses
        openSettings()
    }

    // MARK: Camera

    /// The Camera switch as the screens show it: the guest's choice is on and
    /// iOS lets Andarium capture, or has not been asked yet and the question is
    /// under way. Access that was refused or taken away, or a device without
    /// cameras, shows it off.
    var cameraShownOn: Bool {
        guard guest?.camera == true else { return false }
        return cameraAccess == .authorized || cameraAccess == .notDetermined
    }

    /// What the cameras do in the run that starts now: live only while the
    /// switch is on and iOS lets Andarium capture. The runtime reads it at the
    /// start and nothing changes it until the guest powers off.
    var cameraMode: CameraMode {
        CameraSession.mode(choice: guest?.camera == true, access: cameraAccess)
    }

    /// The Camera switch. It exists on Home, so it can only change between
    /// runs. Turning it on asks iOS for access at once, the first time. After a
    /// refusal there is nothing to ask: the person is told why and, where
    /// Settings has the way out, offered Andarium's page there.
    func setCamera(_ on: Bool) {
        guard guest != nil else { return }
        guard on else {
            cameraAwaitsSettings = false
            cameraAsking = false
            guest?.camera = false
            persistNow()
            return
        }
        switch cameraAccess {
        case .unavailable:
            return
        case .restricted, .denied:
            permissionPrompt = .camera
        case .notDetermined:
            guest?.camera = true
            cameraAsking = true
            persistNow()
            refreshCameraReserve()
            // The controller asks iOS and reports the answer to
            // `cameraAccessChanged`. A preview without one has no system
            // prompt to show and takes it as given.
            if let camera { camera.setOn(true) } else { cameraAccessChanged(.authorized) }
        case .authorized:
            guest?.camera = true
            persistNow()
            refreshCameraReserve()
        }
    }

    /// iOS answered the camera question, or the person changed Andarium's
    /// access in Settings. A refusal takes the choice back with it, and a
    /// device without a camera keeps it; access the person went to Settings to
    /// give turns the switch they tapped on.
    func cameraAccessChanged(_ access: CameraAccess) {
        cameraAccess = access
        switch access {
        case .unavailable:
            // No camera to share is not a choice taken back: a Continuity
            // camera that is away, or a device the guest moves to, leaves the
            // person's switch as it was. The row shows it off and disabled,
            // and the session is blocked (`cameraShownOn`, `cameraMode`).
            break
        case .restricted, .denied:
            if guest?.camera == true {
                guest?.camera = false
                persistNow()
            }
        case .authorized:
            if cameraAwaitsSettings, guest != nil, guest?.camera != true {
                guest?.camera = true
                persistNow()
            }
        case .notDetermined:
            if !cameraAsking, guest?.camera == true {
                guest?.camera = false
                persistNow()
            }
        }
        if access != .notDetermined {
            cameraAwaitsSettings = false
            cameraAsking = false
        }
        // The cameras a host has can change with the access to them (a
        // Continuity camera that comes or goes).
        refreshCameraReserve()
    }

    /// The alert's Open Settings: for a refusal, the switch the person tapped
    /// turns on when Andarium is active again with access.
    func openCameraSettings() {
        cameraAwaitsSettings = cameraAccess.refuses
        openSettings()
    }

    /// The runtime reports what the host is doing for the guest's cameras.
    func cameraCaptureChanged(_ capture: CameraCapture) {
        var next = cameraNotice
        next.captureChanged(capture, at: cameraNoticeClock())
        guard next != cameraNotice else { return }
        cameraNotice = next
        cameraNoticeExpiry?.cancel()
        cameraNoticeExpiry = nil
        guard next.lingers else { return }
        cameraNoticeExpiry = Task { [weak self] in
            await self?.cameraNoticeSleep(CameraNoticeState.lingerFor)
            guard !Task.isCancelled else { return }
            self?.expireCameraNotice()
        }
    }

    /// The guest closed its camera `lingerFor` ago and has not opened one
    /// since: the notice goes.
    private func expireCameraNotice() {
        cameraNoticeExpiry = nil
        var next = cameraNotice
        next.expire(at: cameraNoticeClock())
        if next != cameraNotice { cameraNotice = next }
    }

    /// The notice the session shows about the camera: that it is paused
    /// because other apps share the screen, or that a guest app opened a
    /// camera while Camera is off. The paused notice goes when the
    /// interruption ends; Camera off goes a moment after the guest closes its
    /// camera and returns at a later open. A dismissed Camera-is-off notice
    /// stays away for the rest of the run. Camera off is said only where the
    /// row can be turned on: not on a device with no camera to share, and not
    /// when the switch is on.
    var cameraNoticeKind: CameraNotice? {
        cameraNotice.showing(canTurnOn: cameraAccess.offered && !cameraShownOn)
    }

    var cameraNoticeShowing: Bool { cameraNoticeKind != nil }

    func dismissCameraNotice() {
        cameraNotice.dismiss()
    }

    /// The Simulator preview plays a camera notice: it has no runtime to
    /// interrupt, and its guest has no camera to open.
    func previewCameraNotice(_ capture: CameraCapture = .interruptedByMultitasking) {
        cameraCaptureChanged(capture)
    }

    // MARK: Statistics

    /// A view that shows the statistics appears or goes. The readings run
    /// while some view shows them and everything else allows
    /// (`StatsSampling.isWanted`), and one loop serves every view.
    func statistics(_ viewer: StatisticsViewer, visible: Bool) {
        if visible {
            statisticsViewers.insert(viewer)
        } else {
            statisticsViewers.remove(viewer)
        }
        refreshStatistics()
    }

    private var statisticsWanted: Bool {
        var running = false
        if case .running = phase { running = true }
        return StatsSampling.isWanted(
            viewers: statisticsViewers.count, applicationActive: applicationActive,
            capable: capabilities.statistics, hasDriver: driver != nil, running: running
        )
    }

    /// Starts the loop when the readings are wanted and none runs, and stops
    /// it when they are not.
    func refreshStatistics() {
        if statisticsWanted {
            guard statisticsTask == nil else { return }
            statisticsGeneration += 1
            let generation = statisticsGeneration
            statisticsTask = Task { [weak self] in
                await self?.runStatisticsLoop()
                if let self, self.statisticsGeneration == generation { self.statisticsTask = nil }
            }
        } else if let task = statisticsTask {
            task.cancel()
            statisticsTask = nil
            statisticsGeneration += 1
        }
    }

    /// Two readings a moment apart, so figures show within about a third of
    /// a second, and the guest's memory after the second. Then a reading
    /// every second and the memory with every other one. A result that
    /// arrives after the loop was stopped is dropped.
    private func runStatisticsLoop() async {
        guard let driver else { return }
        statisticsMeter.pause()
        var readings = 0
        while !Task.isCancelled, statisticsWanted {
            if let sample = await driver.readStatistics(), !Task.isCancelled {
                statisticsMeter.record(sample)
                readings += 1
                if readings.isMultiple(of: StatsSampling.memoryEvery) {
                    if let memory = await driver.readGuestMemory(), !Task.isCancelled {
                        statisticsMeter.record(memory: memory)
                    }
                }
                publishStatistics()
            }
            guard !Task.isCancelled else { return }
            await statisticsSleep(readings == 1 ? StatsSampling.firstGap : StatsSampling.interval)
        }
    }

    /// Hands the reading to the views when it draws something other than they
    /// show. A published readout redraws the capsule and Settings' rows and has
    /// iOS composite a frame, so a steady guest, whose figures and bars stay
    /// where they are, costs none of that each second. The reading itself
    /// stays as it is: the loop samples at the same rate and the console
    /// report below prints every reading.
    private func publishStatistics() {
        let readout = Readout(statisticsMeter)
        // The size the views draw the shares against.
        let memoryMiB = guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound
        if statisticsGate.admits(readout.drawn(memoryMiB: memoryMiB)) {
            session.readout = readout
        }
        if printsStatistics {
            print(ConsoleReport.statistics(statisticsMeter, guestMemoryMiB: guest?.memoryMiB ?? 0))
        }
    }

    // MARK: Graphics notice

    /// The count is read while a normal session runs in the foreground, with
    /// or without a view of the statistics: the notice is for a person who
    /// is using the guest. Recovery runs no app that could lose its
    /// graphics.
    private var graphicsNoticeWanted: Bool {
        guard case .running(.normal) = phase else { return false }
        return applicationActive && capabilities.statistics && driver != nil
    }

    /// Starts reading the refusal count when it is wanted and stops when it
    /// is not. The count is the runtime's for the whole launch, so a reading
    /// after a pause finds what was refused meanwhile.
    func refreshGraphicsNotice() {
        if graphicsNoticeWanted {
            guard graphicsNoticeTask == nil else { return }
            graphicsNoticeGeneration += 1
            let generation = graphicsNoticeGeneration
            graphicsNoticeTask = Task { [weak self] in
                await self?.runGraphicsNoticeLoop()
                if let self, self.graphicsNoticeGeneration == generation { self.graphicsNoticeTask = nil }
            }
        } else if let task = graphicsNoticeTask {
            task.cancel()
            graphicsNoticeTask = nil
            graphicsNoticeGeneration += 1
        }
    }

    private func runGraphicsNoticeLoop() async {
        guard let driver else { return }
        while !Task.isCancelled, graphicsNoticeWanted {
            if let refusals = await driver.readGraphicsRefusals(), !Task.isCancelled {
                // An in-place mutation tells the views even when it changes
                // nothing, and the views' redraw lays the scene's root out;
                // a reading that finds the same count and the same notice
                // up is not published.
                var next = graphicsNotice
                next.record(refusals: refusals, at: graphicsNoticeClock())
                if next != graphicsNotice { graphicsNotice = next }
            }
            guard !Task.isCancelled else { return }
            await reclaimGraphicsMemory(with: driver)
            guard !Task.isCancelled else { return }
            await graphicsNoticeSleep(GraphicsNotice.pollInterval)
        }
    }

    /// One look at the graphics memory budget: when it stays nearly full or
    /// the guest was refused, ends the cached app that holds the most of it.
    /// Silent, and nothing at all while the kernel names no process.
    private func reclaimGraphicsMemory(with driver: RuntimeDriver) async {
        guard
            let reading = await driver.readGraphicsMemory(
                minimumIdle: GraphicsReclaim.minimumIdle, minimumBytes: GraphicsReclaim.minimumHolderBytes),
            !Task.isCancelled
        else { return }
        guard graphicsReclaim.observe(reading, at: graphicsNoticeClock()) else { return }
        let result = await driver.reclaimGraphicsMemory(reading, using: graphicsReclaim)
        graphicsReclaim.record(result, at: graphicsNoticeClock())
    }

    /// The person dismisses the notice.
    func dismissGraphicsNotice() {
        graphicsNotice.dismiss()
    }

    /// The Simulator preview plays a refusal: it has no runtime to count one.
    func previewGraphicsNotice() {
        let now = graphicsNoticeClock()
        graphicsNotice.record(refusals: 0, at: now)
        graphicsNotice.record(refusals: 1, at: now)
    }

    // MARK: Thermal notice

    /// The runtime's policy says Android starts to lower the frame rate. The
    /// notice shows once the session screen is up, and the policy says this
    /// once in a run, so nothing here counts.
    func thermalCapStarted() {
        // No notice while the limit is off, at any heat state: a cap announced
        // just before the person turned the limit off is not shown.
        guard heatLimitOn else { return }
        thermalCapWaiting = true
        showThermalNoticeIfWaiting()
    }

    fileprivate func showThermalNoticeIfWaiting() {
        guard thermalCapWaiting, heatLimitOn, case .running(.normal) = phase else { return }
        thermalCapWaiting = false
        thermalNotice.show()
        waitOutThermalNotice()
    }

    private func waitOutThermalNotice() {
        thermalNoticeTask?.cancel()
        thermalNoticeTask = Task { [weak self] in
            await self?.thermalNoticeSleep(ThermalNotice.visibleFor)
            guard !Task.isCancelled else { return }
            self?.thermalNotice.dismiss()
        }
    }

    /// The person dismisses the notice, or it has run its time. It changes
    /// nothing else, and ends a question about the limit that was open.
    func dismissThermalNotice() {
        thermalNoticeTask?.cancel()
        thermalNoticeTask = nil
        thermalNotice.dismiss()
        askingToTurnOffHeatLimit = false
    }

    // MARK: Heat limit

    /// The thermal policy says the device is hot now, or no longer.
    func deviceHeatChanged(hot: Bool) {
        deviceIsHot = hot
    }

    /// Settings' "When hot", and the alert's Turn Off. Off, the notice goes at
    /// once and none comes, and the runtime tells Android nothing hot; on, the
    /// runtime tells it the policy's severity at once.
    func setHeatLimit(_ on: Bool) {
        guard on != heatLimitOn else { return }
        heatLimitOn = on
        if !on {
            thermalCapWaiting = false
            dismissThermalNotice()
        }
        driver?.setHeatLimit(on)
    }

    /// The person taps the notice's words: the alert asks before the limit
    /// goes. The notice waits meanwhile.
    func askToTurnOffHeatLimit() {
        guard thermalNotice.isShowing, heatLimitOn else { return }
        thermalNoticeTask?.cancel()
        thermalNoticeTask = nil
        askingToTurnOffHeatLimit = true
    }

    /// The alert's Cancel, or an alert that could not be shown: the notice is
    /// as it was.
    func cancelTurnOffHeatLimit() {
        guard askingToTurnOffHeatLimit else { return }
        askingToTurnOffHeatLimit = false
        if thermalNotice.isShowing { waitOutThermalNotice() }
    }

    /// The alert's Turn Off: the one tap.
    func confirmTurnOffHeatLimit() {
        askingToTurnOffHeatLimit = false
        setHeatLimit(false)
    }

    /// The Simulator preview plays the notice: it has no runtime to start a cap.
    func previewThermalNotice() {
        thermalNotice.show()
    }

    // MARK: App update notice

    /// Whether the update notice may read its file now: Home is showing, the
    /// application is the active one and no start is pending (an app that
    /// Shortcuts or an icon asked to open waits for one).
    var appUpdateReadAllowed: Bool {
        phase == .home && applicationActive && androidApps.open.pending == nil
    }

    /// Home's task: reads the version file when one is due, a few seconds
    /// after Home shows. A start that begins meanwhile cancels the task, and
    /// nothing of it ever stands before Start.
    func readAppUpdateWhenHomeShows() async {
        guard let appUpdate else { return }
        await appUpdate.readWhenHomeShows(moment: { self.appUpdateReadAllowed })
    }

    // MARK: Network notice

    /// The path is watched while Andarium is the active application and has a
    /// guest, on Home or in a session: only then does the notice matter, and
    /// the system's monitor calls back for a change of the path alone.
    private var networkWatchWanted: Bool {
        applicationActive && lifecycle.guest != nil
    }

    /// Starts the watch when it is wanted and stops it when it is not.
    func refreshNetworkWatch() {
        networkWatcher?.setWanted(networkWatchWanted)
    }

    private func networkWatcherChanged() {
        guard let watcher = networkWatcher else { return }
        networkNotice = watcher.notice
        reportNetworkDenial()
    }

    /// The denial the notice is up for now, which Home and a session show;
    /// nil when there is none, the person has dismissed it, or there is no
    /// guest to be denied a network.
    var networkDenialShowing: NetworkDenial? {
        lifecycle.guest == nil ? nil : networkNotice.showing
    }

    /// Tells the runtime which denial is in force, dismissed or not, for the
    /// session record: a bug report then says whether the person was told.
    /// The runtime keeps it for the next session too, but exists only once a
    /// run has loaded it, so the run says it again then.
    func reportNetworkDenial() {
        driver?.setNetworkNotice(networkNotice.denial)
    }

    /// The person dismisses the notice: on Home or in the session, for the
    /// same cause, until the network has been allowed again.
    func dismissNetworkNotice() {
        networkWatcher?.dismiss()
    }

    /// The Simulator cannot be denied the network, so the preview plays a
    /// denial.
    func previewNetworkNotice(_ denial: NetworkDenial) {
        networkNotice.apply(.denied(denial))
    }

    /// The debugger is preparing the JIT, which keeps the boot at its first step.
    private var preparingJitNow: Bool {
        preparingJit
    }

    /// The step of the boot under way, from the runtime's milestones.
    func liveBootStage() -> Int {
        BootSteps.current(preparingDisk: preparingDisk || preparingJitNow, milestone: bootTracker.milestone)
    }

    /// The failure screen's copyable report.
    var runReport: String {
        runEnd.report(
            appVersion: system.appVersion, system: system, guest: guest, uptimeSeconds: runUptimeSeconds
        )
    }
}
