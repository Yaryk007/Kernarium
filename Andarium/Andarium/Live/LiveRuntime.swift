// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import GuestHostCore
import Observation
import UIKit

/// The guest console, read on the runtime's thread for the boot's milestones.
nonisolated final class ConsoleSink: @unchecked Sendable {
    private let lock = NSLock()
    private var scanner = ConsoleScanner()
    private let report: @Sendable ([BootMilestone], Bool, Bool) -> Void

    /// `report` gets the milestones a chunk completes, whether Android
    /// reported `sys.boot_completed` in it and whether the recovery image
    /// started in it.
    init(report: @escaping @Sendable ([BootMilestone], Bool, Bool) -> Void) {
        self.report = report
    }

    /// The guest restarted itself in place (`LiveRuntime.guestRestarted`): the
    /// text that follows is another boot's.
    func restarted() {
        lock.withLock { scanner.beginBoot() }
    }

    func feed(_ data: Data) {
        let (found, completed, recovery) = lock.withLock { () -> ([BootMilestone], Bool, Bool) in
            let before = (scanner.bootCompletedCount, scanner.recoveryStartedCount)
            let found = scanner.feed(data)
            return (found, scanner.bootCompletedCount > before.0, scanner.recoveryStartedCount > before.1)
        }
        if !found.isEmpty || completed || recovery { report(found, completed, recovery) }
    }
}

/// A run the model asked for, waiting for the disk and the display.
private struct PendingRun {
    var guest: Guest
    var mode: StartMode
    var kind: BootKind
}

/// The device application behind the screens: the guest's storage, and the
/// runtime that runs it. Each run has a thread of its own,
/// into the display view the screens keep alive.
@MainActor
final class LiveRuntime: RuntimeDriver {
    /// The loader's flavour for the Android runtime.
    private static let androidFlavor: UInt32 = 1
    /// Android's power key held this long raises its power menu.
    nonisolated private static let powerMenuHold = 0.8

    private weak var model: AppModel?
    private let storage: TwoDiskStorage
    /// Android reached `sys.boot_completed` in the boot under way, which a
    /// clean end records. Recovery never does.
    private var reachedBootCompleted = false
    private let pause: PauseController
    /// The effects of the pause, on the one serial queue of the runtime calls
    /// that wait: a stop is delivered on it too.
    private let pauseEffects: LivePauseEffects
    /// Counts the time the guest runs, for the limits of adb operations and
    /// the boot screen's notice that a start is slow: it stops while the
    /// application holds the guest paused, and while it is away before the
    /// runtime runs (`holdStartClock`).
    private let clock = GuestRunClock()
    /// The clock stopped for the application's span in the background before
    /// the runtime ran.
    private var startClockHeld = false
    private let keyStore: AdbKeyStore
    /// Where the runtime records how each session ends, for the next launch.
    private let sessionRecord: URL?
    /// Where the colour data a Bayer still teaches the camera is kept: beside
    /// the session records, outside the guest's disks, so an erase leaves it.
    private let cameraDirectory: URL?
    /// The guest's adb key, made before the start that boots with it.
    private var adbIdentity: AdbKeyStore.Identity?
    /// What this run puts in the names of its imports' partial files, new at
    /// every start and at every restart of the guest inside the run
    /// (`GuestRunMark`).
    private(set) var runMark = GuestRunMark.random()
    /// Whether the guest's storage may hold partial files that imports left,
    /// on disk beside the guest's other files: the sweep below is made only
    /// when it does.
    let partials: PartialSweepLedger
    let textMessageNotice: TextMessageNotice
    /// The sweep of the partial files earlier runs left in the guest, which a
    /// normal run makes once per boot that owes it, after Android has booted.
    private(set) var partialSweep: PartialFileSweep?
    private let motion = MotionSensors()
    /// The guest's hardware keyboard and mouse: plugged while iOS has the
    /// hardware, and fed by the display's view (`GuestLayer`).
    let keyboardMouse: KeyboardMouse
    /// The runtime reported that this run has started, so the keyboard and the
    /// mouse exist in it and can be plugged.
    private var inputRunReady = false
    /// The device's haptics for the guest's vibrator. A device without a Taptic
    /// Engine never runs it: the guest finds no vibrator.
    private let hapticsAvailable = DeviceHaptics.isAvailable
    /// One player for the device's own actuator and every game controller's,
    /// each target with the engine its own source makes.
    private let controllerEngines = GameControllerEngines()
    private lazy var hapticPlayer = CoreHapticsPlayer(
        source: HapticEngineSources(device: DeviceHapticEngines(), controllers: controllerEngines))
    private lazy var haptics = HapticsSession(opener: HapticsConnector(), player: hapticPlayer)
    /// The game controllers iOS has connected, as HID devices in the guest.
    /// Their rumble plays on `hapticPlayer`, which a controller has whether or
    /// not the device has a Taptic Engine.
    private lazy var gameControllers = GameControllers(player: hapticPlayer, engines: controllerEngines)
    /// Tells the guest's thermal HAL how hot the device is; Android lowers
    /// its frame rate for it, and the session says so once. Settings' "When
    /// hot" decides whether Android is told (`setHeatLimit`); the session
    /// tells the model when the device is held hot either way.
    private lazy var thermal = ThermalSession(
        opener: thermalOpener ?? ThermalConnector(),
        hotChanged: { [weak self] hot in
            // In order, whatever the thread the session calls from.
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.model?.deviceHeatChanged(hot: hot) } }
        },
        // A run begins in the state iOS is in then, never the last run's: the
        // monitor's own first reading comes after the run has begun.
        present: { ThermalSource.current() },
        capStarted: { [weak self] in
            Task { @MainActor [weak self] in self?.thermalCapStarted() }
        })
    /// The guest's end of the thermal connection where a UI test's probe
    /// stands in for it; nil is the real one, over vsock.
    private let thermalOpener: (any GuestStreamOpener)?
    /// A UI test's probe session runs the thermal session as a normal run
    /// does (`beginThermalProbe`).
    private var thermalProbe = false
    private lazy var thermalMonitor = ThermalMonitor { [weak self] state in self?.thermal.observe(state) }
    /// Whether the run in progress has the 120 Hz display, as the runtime
    /// granted it (it decides once at the start, whatever the start asked), and
    /// so whether Low Power Mode, or a screen that shows less than 100 frames a
    /// second, holds its render rate to 60 (`HighRefresh.holdsAtSixty`). False
    /// until the runtime has said (`learnDisplayGrant`).
    private var runHasHighRefresh = false
    private var displayGrantKnown = false
    private var lowPowerModeOn = false
    private lazy var lowPowerMonitor = LowPowerMonitor { [weak self] on in
        guard let self else { return }
        lowPowerModeOn = on
        updateSixtyHertzHold()
    }
    /// The device's position for the guest's GNSS receiver, read from Core
    /// Location only while the person's Location switch, iOS, the foreground,
    /// a normal run and the guest's own receiver all say so
    /// (`LocationSession`).
    private let hostLocation = HostLocation()
    private lazy var location = LocationSession(updates: hostLocation, sink: RuntimeLocationSink())
    /// The interface orientation the sensors' axes are mounted to.
    private var motionMount = MotionMount()
    /// The device's orientation, observed while a session is in front on a
    /// device whose guest follows it (an iPhone), and given to the guest's
    /// device-orientation sensor over the motion connection.
    private lazy var deviceOrientation = DeviceOrientationObserver { [weak self] in
        self?.deviceOrientationChanged($0)
    }
    private lazy var rotation = GuestRotationController(
        apply: { [weak self] policy in
            guard let self else { throw CancellationError() }
            let result = try await operations.shell(policy.command, limit: .seconds(15))
            guard result.exitCode == 0 else {
                print("Andarium: display rotation command failed: \(result.text)")
                throw AdbFailure(message: String(localized: "The virtual machine’s screen could not follow this window. Close and reopen Andarium to try again."))
            }
        }, changed: { [weak self] in
            self?.updateMotionSensors()
        }, failure: { [weak self] error in
            print("Andarium: display rotation unavailable: \(error)")
            guard let model = self?.model, model.alert == nil else { return }
            model.alert = AppAlert(
                title: String(localized: "Display rotation unavailable"),
                message: String(localized: "The virtual machine’s screen could not follow this window. Close and reopen Andarium to try again."))
        })
    private var normalRun = false
    /// The device body's turn last given to the camera in this run
    /// (`updateCameraRotation`).
    private var cameraRotationSent: UInt32?
    /// The runtime runs a guest, from its start until its run returns.
    private var runActive = false { didSet { publishSnapshotActivity() } }
    private var active = UIApplication.shared.applicationState == .active
    /// The space a write can use on the device (`HostStorage`), which the run
    /// watches and every screen shows. The application's own reading unless a
    /// test supplies another.
    private let freeSpace: @Sendable () -> UInt64
    private var spaceWatchTask: Task<Void, Never>?
    /// Tells a watch that ended from the one that replaced it.
    private var spaceWatchGeneration = 0
    /// The person asked to shut a guest paused for space down: Android is
    /// not asked anything else, and the space is not watched, as it goes.
    private var shuttingDown = false
    /// The screen over the session says the space is short, whether Android
    /// is paused or still runs behind it: the space is not watched, and the
    /// guest is asked nothing, until the person answers it.
    private var spaceAlertOpen = false
    private var observers: [NSObjectProtocol] = []
    /// Writes what the person sets, for the next launch.
    private let preferences: PreferenceKeeper

    private var pending: PendingRun? { didSet { publishSnapshotActivity() } }
    /// Which start the preparation in hand belongs to: a start the person
    /// stopped before the runtime ran moves it on, and the preparation, which
    /// goes on to its end, then drops its launch.
    private var launchGeneration = 0
    /// The preparation of the start in hand, from the first file it touches
    /// until it hands the run to the runtime or ends. It owns the guest's files
    /// for that span, and goes on after the person stops the start: a start
    /// asked for since waits for it (`startIfReady`), and so does whatever else
    /// writes the files (`settleGuestFiles`).
    private var preparation: Task<Void, Never>?
    /// The changes of the guest's files from Home (`withGuestFiles`: setting
    /// the guest up, erasing it, the update's Restore) from the moment they are
    /// asked for until they end. A start asked for meanwhile waits for them
    /// (`startIfReady`): one that waited behind a preparation must not be let
    /// in ahead of a change that was asked for before it.
    private var fileChanges = 0
    /// The start `preparation` belongs to, so that the end of one that was
    /// stopped does not clear its successor.
    private var preparationGeneration = -1
    /// The person's stop of the run in hand (`RunStop`), from the launch until
    /// the run's end is reported.
    private var runStop: RunStop?
    #if DEBUG
    /// A test's stand-in for the preparation of the files, where it holds a
    /// start.
    var prepareFilesOverride: (@MainActor (Guest) async throws -> Void)?
    #endif
    /// The person stopped this start (`stopGuest`); the run's end says so.
    private var userStopped = false
    private var launching = false { didSet { publishSnapshotActivity() } }
    private var preparing = false { didSet { publishSnapshotActivity() } }
    /// The snapshot operation under way (`SnapshotDriver`). It clears the
    /// field itself before it ends, so what waits for it never sees it set.
    private var snapshotOperation: Task<Void, Never>?
    /// What that operation is.
    private var snapshotWork: SnapshotWork? { didSet { publishSnapshotActivity() } }
    /// A guest is being created or erased.
    private var storageBusy = false { didSet { publishSnapshotActivity() } }
    /// The operations finished so far (`SnapshotActivity.revision`).
    private var snapshotRevision = 0 { didSet { publishSnapshotActivity() } }
    /// What the snapshot driver is doing, as the model has it.
    private(set) var activity = SnapshotActivity()
    var activityChanged: (@MainActor (SnapshotActivity) -> Void)?
    /// What stopped the last attempt in this process to finish a restore that
    /// did not finish; the listing carries it.
    private var rollForwardFailure: SnapshotFailure?
    private(set) var view: GuestMetalView?
    private var geometry: GuestDisplayGeometry?
    private var bootStartedAt = Date()
    private(set) var runStartedAt: Date?
    /// The memory, in MiB, the run in progress was started with; what a
    /// refusal of it by the host names.
    private var runMemoryMiB = 0
    private var thermalObserver: NSObjectProtocol?
    /// The sink of the run's console; a test sets its own.
    var console: ConsoleSink?

    init(
        model: AppModel, storage: TwoDiskStorage, marker: PauseMarker, keyStore: AdbKeyStore = AdbKeyStore(),
        sessionRecord: URL? = nil, cameraDirectory: URL? = nil,
        freeSpace: @escaping @Sendable () -> UInt64 = { AppEntry.freeStorageBytes() },
        keyboardMouse: KeyboardMouse? = nil, thermalOpener: (any GuestStreamOpener)? = nil
    ) {
        self.thermalOpener = thermalOpener
        self.model = model
        self.keyboardMouse = keyboardMouse ?? KeyboardMouse.live(supportsMouse: model.host.supportsPointer) {
            [weak model] wanted in model?.pointerLockWanted = wanted
        }
        self.preferences = PreferenceKeeper(model: model)
        self.storage = storage
        partials = PartialSweepLedger(directory: storage.locations.directory)
        textMessageNotice = TextMessageNotice(directory: storage.locations.directory)
        self.keyStore = keyStore
        self.sessionRecord = sessionRecord
        self.cameraDirectory = cameraDirectory
        self.freeSpace = freeSpace
        let effects = LivePauseEffects()
        pauseEffects = effects
        pause = PauseController(effects: effects, marker: marker, runClock: clock)
        // The boot screen counts a start by the guest's running time, not by
        // the wall clock: neither a span in the background nor a correction of
        // the date moves it.
        model.clock = { [clock] in clock.seconds }
        effects.statistics = { [weak self] in await self?.readStatistics() }
        model.microphone = MicrophoneController(model: model, source: SystemMicrophoneSource()) {
            // The runtime is told the setting and never asks the system
            // itself; a guest records silence while access is out of reach.
            _ = kernarium_runtime_loader_set_microphone($0 ? 1 : 0)
        }
        // iOS's denial of the network, from the system's path while a watch is
        // wanted (the model decides when); the path monitor calls back for a
        // change of the path alone.
        model.networkWatcher = NetworkAccessWatcher(source: SystemNetworkPathSource())
        hostLocation.deliver = { [weak self] fix in self?.location.deliver(fix) }
        model.location = LocationController(model: model, source: hostLocation.access) { [weak self] in
            self?.updateLocation()
        }
        // The guest's receiver powers up when an Android app wants a position
        // and again when it stops; the loader delivers each event on the main
        // queue.
        kernarium_runtime_loader_set_location_receiver_handler { @Sendable [weak self] powered in
            MainActor.assumeIsolated { self?.location.receiverPower(powered) }
        }
        // The camera is read once, when the guest starts; the runtime says
        // what the host is doing for it (the notice for other apps sharing the
        // screen), delivered on the main queue.
        model.camera = CameraController(model: model, source: SystemCameraSource())
        kernarium_runtime_loader_set_camera_state_handler { @Sendable [weak model] state in
            MainActor.assumeIsolated { model?.cameraCaptureChanged(Self.cameraCapture(runtimeState: state)) }
        }
        model.hapticsAvailable = hapticsAvailable
        self.keyboardMouse.setPointerAsTouch(model.pointerAsTouch)
        observeLifecycle()
        observeRotationPolicy()
        observeOverlay()
        preferences.start()
    }

    // MARK: Storage

    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest {
        defer { model?.host.freeStorageBytes = AppEntry.freeStorageBytes() }
        return try await withGuestFiles {
            storageBusy = true
            defer {
                snapshotRevision += 1
                storageBusy = false
            }
            return try await storage.create(guest, progress: progress)
        }
    }

    func erase(progress: @escaping @Sendable (Double) -> Void) async throws {
        defer { model?.host.freeStorageBytes = AppEntry.freeStorageBytes() }
        // The snapshots go with the guest: an operation on them finishes first,
        // and so does the preparation of a start that was stopped.
        try await withGuestFiles {
            storageBusy = true
            defer {
                snapshotRevision += 1
                storageBusy = false
            }
            try await storage.erase(progress: progress)
            partials.diskErased()
            // The next guest is a new one: it boots with a key of its own.
            keyStore.delete()
            adbIdentity = nil
        }
    }

    /// Changes the guest's files from Home. The change owns them from the
    /// moment it is asked for, before its first wait, so a start asked for
    /// after it does not begin until it has ended; it then waits until nothing
    /// else writes them, and refuses if a start or the run took them first (a
    /// start that was under way when it was asked for, and went on to the
    /// runtime): the end of a preparation does not say that the files are idle.
    private func withGuestFiles<Value>(_ change: () async throws -> Value) async throws -> Value {
        fileChanges += 1
        defer {
            fileChanges -= 1
            startIfReady()
        }
        await settleGuestFiles()
        guard !SnapshotRules.runtimeActive(running: runActive, pending: false, launching: launching, preparing: preparing)
        else { throw GuestFilesTaken() }
        return try await change()
    }

    /// What a change of the guest's files says when a start or the run holds
    /// them: the snapshots' own words.
    private struct GuestFilesTaken: LocalizedError {
        var errorDescription: String? { String(localized: "Not while the virtual machine is starting or running.") }
    }

    func save(_ guest: Guest) throws {
        try storage.save(guest)
    }

    // MARK: Starting

    func startPlan() -> StartPlan? {
        storage.startPlan()
    }

    func firstBootPending() -> Bool {
        storage.firstBootPending()
    }

    func measuredUsedBytes() -> UInt64 {
        storage.measuredUsedBytes()
    }

    /// The update's Restore: after the snapshot operation under way, as a
    /// start is. It changes the guest's files (it completes a snapshot
    /// restore that did not finish first), so what a screen has listed is stale.
    func restoreUpdate() async throws {
        defer { model?.host.freeStorageBytes = AppEntry.freeStorageBytes() }
        try await withGuestFiles {
            // A guest being set up or erased has no update to restore.
            guard !storageBusy else { return }
            defer { snapshotRevision += 1 }
            do {
                try storage.restoreUpdate()
                rollForwardFailure = nil
            } catch let unfinished as SnapshotRestoreUnfinished {
                rollForwardFailure = SnapshotFailure(unfinished.cause)
                throw unfinished
            }
        }
    }

    /// A restore that began in an earlier process and could not be finished
    /// at launch (`TwoDiskStorage.recoverSnapshots`): the listing says so.
    func snapshotRecoveryFailed(_ error: any Error) {
        let cause = (error as? SnapshotRestoreUnfinished)?.cause ?? error
        rollForwardFailure = SnapshotFailure(cause)
    }

    func begin(guest: Guest, mode: StartMode, kind: BootKind) {
        // The record of the run before moves aside, so that the one at its
        // place from here on is this run's (`RunRecord`).
        runRecord?.retire()
        // The previous display is removed on return to Home. Wait for the
        // new boot's view to lay out before starting another runtime.
        view = nil
        geometry = nil
        launchGeneration += 1
        userStopped = false
        // A preparation of a start that was stopped is still the owner of
        // `launching` and of the guest's files: this start waits for it to end
        // (`startIfReady`, and the end of that preparation).
        pending = PendingRun(guest: guest, mode: mode, kind: kind)
        startIfReady()
    }

    /// The person stopped the start. The application states the intent at once
    /// and asks nothing of the runtime on the main actor: the guest is asked to
    /// write its storage back and then stopped, through the queue of the
    /// runtime's other waiting calls, and the stop is made again until the run
    /// ends (`RunStop`).
    func stopGuest() {
        userStopped = true
        if runActive, let stop = runStop {
            // The run returns, and `finished` reports it as the person's.
            if stop.request() { pauseEffects.deliverStop(stop) }
            return
        }
        // The runtime has not run: the start ends here. A preparation under
        // way goes on to its end, since it is the guest's files that it
        // writes, and drops its launch.
        launchGeneration += 1
        pending = nil
        userStopped = false
        model?.runEnded(RunEnd(result: 0, stoppedByUser: true), uptimeSeconds: nil)
    }

    /// The display view has a size. The first layout after a run is wanted
    /// starts it.
    func viewDidLayout(_ view: GuestMetalView, _ geometry: GuestDisplayGeometry) {
        // Removing the guest screen can lay it out once more. A completed
        // run must not take ownership of that retired view again.
        guard pending != nil || launching else { return }
        self.view = view
        self.geometry = geometry
        startIfReady()
    }

    private func startIfReady() {
        guard let run = pending, !launching, fileChanges == 0, view != nil, geometry != nil else { return }
        pending = nil
        launching = true
        let generation = launchGeneration
        preparationGeneration = generation
        preparation = Task {
            await prepareAndLaunch(run, generation: generation)
            // A start asked for meanwhile has its own preparation by now.
            if preparationGeneration == generation { preparation = nil }
        }
    }

    /// The person stopped the start that this preparation is for.
    private func launchWasStopped(_ generation: Int) -> Bool {
        guard generation != launchGeneration else { return false }
        preparing = false
        launching = false
        model?.preparingDisk = false
        model?.diskProgress = nil
        // A start asked for since waits for this preparation to end.
        startIfReady()
        return true
    }

    private func prepareAndLaunch(_ run: PendingRun, generation: Int) async {
        preparing = true
        do {
            try await prepareFiles(for: run.guest) { fraction in
                Task { @MainActor in self.preparingDisk(fraction) }
            }
        } catch {
            if launchWasStopped(generation) { return }
            preparing = false
            end(startFailure: "the disk could not be prepared: \(error.localizedDescription)")
            return
        }
        if launchWasStopped(generation) { return }
        // The guest's adb key, made here when there is none: generating it
        // takes a moment and the main thread does not wait for it. Without
        // one the guest still boots; its adb features then say why they fail.
        let store = keyStore
        adbIdentity = await Task.detached(priority: .userInitiated) { () -> AdbKeyStore.Identity? in
            do {
                return try store.loadOrCreate()
            } catch {
                print("Andarium: the adb key could not be made: \(error)")
                return nil
            }
        }.value
        if launchWasStopped(generation) { return }
        preparing = false
        model?.preparingDisk = false
        let jit = await prepareJit(for: run)
        if launchWasStopped(generation) { return }
        // The row as it stands at this start: the runtime states the release for
        // the translated libraries or leaves it out.
        let extras = StartExtras(jit: jit, translatedLibraries: Self.translatedLibrariesStart(of: model, jitInUse: jit))
        guard let view, let geometry else {
            end(startFailure: "the display is not ready")
            return
        }
        launch(run, view: view, geometry: geometry, extras: extras)
    }

    /// The guest's files, ready for the run: a snapshot operation under way
    /// finishes first, and the update rules then apply to what it left (a
    /// restore that did not finish completes here).
    func prepareFiles(for guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws {
        #if DEBUG
        if let prepareFilesOverride {
            try await prepareFilesOverride(guest)
            return
        }
        #endif
        await settleSnapshotOperation()
        do {
            try await storage.prepareStart(guest, progress: progress)
            rollForwardFailure = nil
        } catch let unfinished as SnapshotRestoreUnfinished {
            rollForwardFailure = SnapshotFailure(unfinished.cause)
            throw unfinished
        }
    }

    /// Whether the guest boots on the JIT (`AppModel.startOnJit`).
    private func prepareJit(for run: PendingRun) async -> Bool {
        guard let model, model.jitAvailable else { return false }
        return await model.startOnJit(
            JitShape(memoryMiB: run.guest.memoryMiB, processorCount: run.guest.cores))
    }

    func jitDebuggerAttached() -> Bool {
        JitDebugger.attached()
    }

    func prepareJit(wanted: Bool, shape: JitShape) async -> JitAnswer {
        await JitDebugger.prepare(wanted: wanted, shape: shape, flavor: Self.androidFlavor)
    }

    private func preparingDisk(_ fraction: Double) {
        guard preparing, let model else { return }
        model.preparingDisk = true
        model.diskProgress = fraction
    }

    private func end(startFailure: String, memoryRefusedMiB: Int? = nil) {
        print("Andarium: cannot start the guest: \(startFailure)")
        // The start is over: nothing owns the launch any more.
        preparing = false
        launching = false
        model?.runEnded(
            RunEnd(result: -1, startFailure: startFailure, memoryRefusedMiB: memoryRefusedMiB), uptimeSeconds: nil)
    }

    /// What a start decides before the runtime runs, beyond the guest: on a
    /// device, whether the guest boots on the JIT engine.
    private struct StartExtras: Sendable {
        var jit = false
        /// What the start does about the translated libraries. It has no
        /// default, so that a start cannot forget to ask the row.
        var translatedLibraries: TranslatedLibrariesStart
    }

    /// What a start does about the translated libraries, from the row as it
    /// stands; a start with no model (none runs) has no framework to use.
    static func translatedLibrariesStart(of model: AppModel?, jitInUse: Bool) -> TranslatedLibrariesStart {
        model?.translatedLibrariesStart(jitInUse: jitInUse)
            ?? TranslatedLibrariesStart(buildCarries: false, optionOn: true, jitInUse: jitInUse)
    }

    /// What the boot request says of the translated libraries
    /// (`android_aot_disabled`): 1 keeps the release out of the launch, where a
    /// build with the framework has the switch off, and 0 states it.
    nonisolated static func aotDisabled(for start: TranslatedLibrariesStart) -> UInt32 {
        start.statesRelease ? 0 : 1
    }

    private func launch(
        _ run: PendingRun, view: GuestMetalView, geometry: GuestDisplayGeometry, extras: StartExtras
    ) {
        guard kernarium_runtime_loader_select(Self.androidFlavor) == 0 else {
            end(startFailure: "the runtime could not be loaded")
            return
        }
        // The runtime refuses a size this host does not admit, and the
        // settings never offer one, so this is a safety net: say why rather
        // than end on the runtime's bare result.
        guard kernarium_runtime_loader_guest_memory_admitted(UInt32(clamping: run.guest.memoryMiB)) != 0 else {
            end(
                startFailure: MemoryRefusal.reason(
                    memoryMiB: run.guest.memoryMiB,
                    ceilingMiB: Int(kernarium_runtime_loader_guest_memory_ceiling_mib())),
                memoryRefusedMiB: run.guest.memoryMiB)
            return
        }
        // A denial of the network the notice is up for began before this run
        // loaded the runtime; its record notes it from the start.
        model?.reportNetworkDenial()
        // The runtime boots a guest that restarts itself again inside the
        // run, and says when. The loader delivers each event on the main
        // queue.
        kernarium_runtime_loader_set_restart_handler { @Sendable [weak self] event, restartCount in
            guard let restart = GuestRestartEvent(event: event, restartCount: restartCount) else { return }
            MainActor.assumeIsolated { self?.guestRestarted(restart) }
        }
        let sink = makeConsoleSink()
        console = sink
        kernarium_runtime_loader_set_console_output_handler { data in sink.feed(data) }
        // A Microphone setting the person turned on earlier follows the
        // guest while iOS still allows it; otherwise the guest hears nothing.
        model?.microphone?.guestStarting()

        let stop = RunStop()
        runStop = stop
        pause.guestDidStart()
        runActive = true
        shuttingDown = false
        bootStartedAt = Date()
        runStartedAt = Date()
        startThermalObservation()
        let size = GuestInputCoordinateSpace.boundedDisplaySize(geometry.pixelSize)
        let width = UInt32(size.width.rounded(.down))
        let height = UInt32(size.height.rounded(.down))
        view.setGuestDisplay(width: width, height: height)
        UIApplication.shared.isIdleTimerDisabled = true

        let dataDiskPath = storage.dataDiskPath
        let releasePath = storage.releaseDirectory.path
        let dataGiB = storage.dataGiB(for: run.guest)
        reachedBootCompleted = false
        normalRun = run.mode == .normal
        beginPartialSweep()
        // The 120 Hz display is asked for at the start, for the size it boots
        // at, and the runtime decides once whether the run has it: the run
        // has what it granted, which is read as soon as the guest's console
        // answers (`learnDisplayGrant`). It holds until the guest powers off;
        // Low Power Mode, or a screen that shows less, then holds what the
        // guest renders to what that screen shows.
        let asksForHighRefresh = model?.asksForHighRefresh(
            displaySize: (Int(width), Int(height))) == true
        runHasHighRefresh = false
        displayGrantKnown = false
        model?.runStarted(withHighRefresh: false)
        if normalRun { lowPowerMonitor.start() }
        updateThermal()
        if normalRun { thermalMonitor.start() }
        motionMount.begin(interface: interfaceMotionAxes)
        cameraRotationSent = nil
        updateCameraRotation(policy: rotationPolicy, starting: true)
        updateHaptics()
        updateGameControllers()
        updateLocation()
        inputRunReady = false
        updateKeyboardMouse()
        let hostFeatures = Self.hostFeatures(
            hapticsAvailable: hapticsAvailable, followsDeviceOrientation: model?.followsDeviceOrientation == true,
            compassAvailable: motion.compassAvailable, highRefresh: asksForHighRefresh)
        let cameraMode = cameraBootMode()
        let cameraPath = cameraDirectory.map { Self.prepareCalibrationDirectory($0) }?.path
        let bootMode: UInt32 = run.mode == .recovery ? 2 : 1
        // Root: the release's Magisk ramdisk for every normal boot of this run;
        // the model decided it before the start and wrote it with the guest.
        let rootBoot = run.mode == .normal && run.guest.root.startRooted && rootRelease != nil
        let memoryMiB = UInt32(run.guest.memoryMiB)
        runMemoryMiB = run.guest.memoryMiB
        let processorCount = UInt32(run.guest.cores)
        let dotsPerInch = geometry.dotsPerInch
        let adbKeyLine = adbIdentity?.publicKeyLine
        // The zone the host is in now, read at every start: the guest follows
        // it while its own automatic time zone is on.
        let timeZone = GuestTimeZone.current()
        let sessionRecordPath = sessionRecord?.path
        // What this start chose, beside the runtime's record of the session, for
        // a bug report to say.
        if let sessionRecord {
            StartRecord(
                translatedLibraries: extras.translatedLibraries.word, processID: Int(getpid()), at: Date())
                .write(to: sessionRecord.deletingLastPathComponent().appendingPathComponent(BugReportAttachments.startRecord))
        }
        print("Andarium: AOT \(extras.translatedLibraries.word.rawValue)")
        // The run occupies its thread for the guest's whole life and the GPU
        // service reaches the main queue while it starts, so it must not be
        // the main thread. It owns a thread outright rather than a place on
        // the cooperative pool it would block for hours.
        let worker = Thread {
            // A stop asked for since the launch began keeps the run out of the
            // runtime: the runtime drops a stop from before its run begins.
            guard stop.enter() else {
                stop.end()
                Task { @MainActor in self.finished(result: 0) }
                return
            }
            let result = Self.runGuest(
                view: view, dataDiskPath: dataDiskPath, releasePath: releasePath, dataGiB: dataGiB, bootMode: bootMode,
                width: width, height: height, dotsPerInch: dotsPerInch, memoryMiB: memoryMiB,
                processorCount: processorCount, adbKeyLine: adbKeyLine, timeZone: timeZone,
                sessionRecordPath: sessionRecordPath, hostFeatures: hostFeatures, cameraMode: cameraMode,
                cameraPath: cameraPath, extras: extras, rootBoot: rootBoot
            )
            stop.end()
            Task { @MainActor in self.finished(result: result) }
        }
        worker.name = "andarium-runtime"
        worker.start()
    }

    private nonisolated static func runGuest(
        view: GuestMetalView, dataDiskPath: String, releasePath: String, dataGiB: UInt32, bootMode: UInt32,
        width: UInt32, height: UInt32, dotsPerInch: UInt32, memoryMiB: UInt32,
        processorCount: UInt32, adbKeyLine: String?, timeZone: String?, sessionRecordPath: String?,
        hostFeatures: UInt32, cameraMode: UInt32, cameraPath: String?, extras: StartExtras, rootBoot: Bool
    ) -> Int32 {
        dataDiskPath.withCString { dataDisk in
            releasePath.withCString { release in
                withOptionalCString(adbKeyLine) { adbKey in
                    withOptionalCString(timeZone) { zone in
                        withOptionalCString(sessionRecordPath) { record in
                            withOptionalCString(cameraPath) { calibration in
                                var request = kernarium_runtime_boot()
                                request.struct_size = UInt32(MemoryLayout<kernarium_runtime_boot>.size)
                                request.width = width
                                request.height = height
                                request.dots_per_inch = dotsPerInch
                                request.guest_memory_mib = memoryMiB
                                request.guest_processor_count = processorCount
                                request.android_boot_mode = bootMode
                                // The guest's display always follows the window.
                                request.auto_resize_guest = 1
                                request.android_data_disk_path = dataDisk
                                request.android_release_path = release
                                request.android_data_gib = dataGiB
                                // The conservative launch parameters the default memory was computed with.
                                let parameters = kernarium_runtime_loader_launch_memory_set()
                                request.gpu_budget_application_mib = parameters.application_mib
                                request.gpu_budget_engine_mib = parameters.engine_mib
                                request.gpu_budget_headroom_mib = parameters.headroom_mib
                                // The key adbd authorizes for this boot, or none.
                                request.android_adb_public_key = adbKey
                                // The host's time zone, which the guest applies while its
                                // own automatic time zone is on, or none.
                                request.android_time_zone = zone
                                // Where the runtime records how the session ends.
                                request.session_record_path = record
                                // What the host has that the guest is to find.
                                request.android_host_features = hostFeatures
                                // What the cameras do for the whole run, and where the
                                // colour data a Bayer still teaches is kept.
                                request.android_camera_mode = cameraMode
                                request.android_camera_calibration_path = calibration
                                // The translated libraries need the release stated to the
                                // emulator; a person who turned them off keeps it out.
                                request.android_aot_disabled = aotDisabled(for: extras.translatedLibraries)
                                // The JIT engine, once its memory is prepared; gadgets otherwise.
                                request.use_jit = extras.jit ? 1 : 0
                                // The release's Magisk-patched ramdisk, or its own.
                                request.android_root = rootBoot ? 1 : 0
                                return withUnsafePointer(to: &request) { kernarium_runtime_loader_run(view, $0) }
                            }
                        }
                    }
                }
            }
        }
    }

    private nonisolated static func withOptionalCString<Output>(
        _ text: String?, _ body: (UnsafePointer<CChar>?) -> Output
    ) -> Output {
        guard let text else { return body(nil) }
        return text.withCString(body)
    }

    /// What the boot request tells the guest the host has
    /// (`android_host_features`): a vibrator, on a device with haptics, a
    /// device orientation to follow, where the guest follows it, a magnetic
    /// field, on a device that can give one, and a 120 Hz display, where the
    /// person asked for it and the screen can show it.
    nonisolated static func hostFeatures(
        hapticsAvailable: Bool, followsDeviceOrientation: Bool = false, compassAvailable: Bool = false,
        highRefresh: Bool = false
    ) -> UInt32 {
        (hapticsAvailable ? UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS) : 0)
            | (followsDeviceOrientation ? UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION) : 0)
            | (compassAvailable ? UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS) : 0)
            | (highRefresh ? UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH) : 0)
    }

    /// The camera mode the boot request states. iOS may have changed the
    /// camera's access since Home read it: the mode is what it allows at this
    /// moment, and it holds until the guest powers off.
    func cameraBootMode() -> UInt32 {
        model?.camera?.refresh()
        return Self.cameraRuntimeMode(model?.cameraMode ?? .blocked)
    }

    /// Where the colour data a Bayer still teaches is kept. It is derived from
    /// this device's own sensors and learned again at the next photo, so it
    /// stays out of iCloud and computer backups.
    nonisolated static func prepareCalibrationDirectory(_ directory: URL) -> URL {
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var target = directory
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        try? target.setResourceValues(excluded)
        return directory
    }

    /// The policy with the memory a live camera session of this host holds out
    /// of each budget, as the runtime composes it before the boot: the layout
    /// of the cameras it discovers (which asks iOS for nothing and opens no
    /// session) and the colour data in `calibration`, which the boot reads.
    /// The runtime discovers the cameras for the first budget and keeps them
    /// for the rest. It is the loader's ceiling (which bounds the system
    /// cameras), and not exact, where no runtime can be loaded or it cannot
    /// discover the cameras. Off the main
    /// thread: loading the runtime and discovering the cameras are not quick.
    nonisolated static func cameraReservePlan(for policy: MemoryPolicy, calibration: URL?) -> CameraReservePlan {
        _ = kernarium_runtime_loader_select(androidFlavor)
        var exact = true
        var first: UInt32 = 1
        let planned = withOptionalCString(calibration?.path) { path in
            policy.withCameraReserve { budgetMiB in
                var figureIsExact: UInt32 = 0
                let reserve = kernarium_runtime_loader_plan_camera_reserve_mib(
                    UInt32(clamping: budgetMiB), path, first, &figureIsExact)
                first = 0
                exact = exact && figureIsExact != 0
                return Int(reserve)
            }
        }
        return CameraReservePlan(policy: planned, exact: exact)
    }

    /// The runtime's word for the session's camera mode (KernariumRuntime.h).
    nonisolated static func cameraRuntimeMode(_ mode: CameraMode) -> UInt32 {
        switch mode {
        case .blocked: UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_BLOCKED)
        case .live: UInt32(KERNARIUM_RUNTIME_ANDROID_CAMERA_LIVE)
        }
    }

    /// What the runtime's camera state (KernariumRuntime.h) says the host is
    /// doing. A state this application does not know counts as an interruption,
    /// the safe reading: a notice follows only the multitasking one and a blocked
    /// guest's open of a camera.
    nonisolated static func cameraCapture(runtimeState state: Int32) -> CameraCapture {
        switch state {
        case Int32(KERNARIUM_RUNTIME_CAMERA_STATE_IDLE): .idle
        case Int32(KERNARIUM_RUNTIME_CAMERA_STATE_STREAMING): .streaming
        case Int32(KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_MULTITASKING): .interruptedByMultitasking
        case Int32(KERNARIUM_RUNTIME_CAMERA_STATE_INTERRUPTED_BY_SYSTEM_PRESSURE): .interruptedBySystemPressure
        case Int32(KERNARIUM_RUNTIME_CAMERA_STATE_BLOCKED_IN_USE): .blockedInUse
        default: .interrupted
        }
    }

    // MARK: Android operations

    /// What adb does for the screens, with the key this boot carries.
    private var operations: AdbGuestOperations {
        AdbGuestOperations(
            identity: adbIdentity, clock: clock, device: model?.host.name ?? "device", hostFree: freeSpace,
            run: runMark)
    }

    /// A new run has its own mark, and a normal one sweeps the partial files
    /// that earlier runs left once Android is up, when the record says there
    /// may be some (`PartialFileSweep`). A recovery boot starts no Android to
    /// ask, and a guest that booted without the application's adb key cannot
    /// be asked either.
    private func beginPartialSweep() {
        partialSweep?.runEnded()
        runMark = GuestRunMark.random()
        guard normalRun, adbIdentity != nil else {
            partialSweep = nil
            return
        }
        // Each step reads the operations when it runs: the mark it spares is
        // the one current then, which a restart of the guest has renewed.
        partialSweep = PartialFileSweep(
            owed: { [partials] in partials.isOwed },
            storageReady: { [weak self] in await self?.storageReady() },
            sweep: { [weak self] in try await self?.sweepPartialFiles() ?? 0 })
    }

    /// The sweep, with the mark that is current when it runs. `walk` is what
    /// removes the leftovers in the guest and returns how many.
    func sweepPartialFiles(
        using walk: @Sendable (AdbGuestOperations) async throws -> Int = { try await $0.sweepPartialFiles() }
    ) async throws -> Int {
        let guest = operations
        return try await partials.sweeping(excluding: guest.run) { try await walk(guest) }
    }

    /// The file browser's backend: it reads the operations for each call, as
    /// the key changes with each boot.
    func makeGuestFiles(hostFree: @escaping @Sendable () -> UInt64) -> LiveGuestFiles {
        LiveGuestFiles(operations: { [unowned self] in operations }, partials: partials, hostFree: hostFree)
    }

    func createBugReport(update: @escaping @Sendable (BusyUpdate) -> Void) async throws -> URL {
        try await operations.createBugReport(update: update)
    }

    func guestStallFiles() async -> [BugReportArchive.Entry] {
        await operations.guestProcFiles()
    }

    /// The root the carried release holds, read at launch; nil for a release
    /// with none. The run asks the runtime for the release's own Magisk ramdisk
    /// only while `Guest.root.startRooted` says so.
    var rootRelease: RootRelease?

    var rootApkURL: URL? { rootRelease?.apkURL(in: storage.releaseDirectory) }

    var guestShell: (any GuestShell)? { AdbGuestShell(operations: operations) }

    /// The records of this process's runs, where the runtime writes them.
    private lazy var runRecord: RunRecord? = {
        guard let sessionRecord, sessionRecord.lastPathComponent == BugReportAttachments.sessionRecord else { return nil }
        return RunRecord(in: sessionRecord.deletingLastPathComponent())
    }()

    func currentSessionRecord() -> SessionRecord? { runRecord?.read() }

    func retireRunRecord() { runRecord?.retire() }

    var retiredAnEarlierRun: Bool { runRecord?.retiredAnEarlierRun ?? false }

    func installRootApp() async throws {
        guard let release = rootRelease else { throw CancellationError() }
        let folder = storage.releaseDirectory
        try await operations.installReleaseApk(
            name: (release.apkPath as NSString).lastPathComponent,
            open: { try release.openApk(in: folder, capacity: ApkInstallOperation.chunkBytes) },
            update: { _ in })
    }

    func installApk(
        at url: URL, bypassLowTargetSdkBlock: Bool, update: @escaping @Sendable (BusyUpdate) -> Void
    ) async throws -> ApkInstallOutcome {
        try await operations.installApk(
            at: url, bypassLowTargetSdkBlock: bypassLowTargetSdkBlock, partials: partials, update: update)
    }

    func usbDebugging() async -> Bool? {
        await operations.usbDebugging()
    }

    func storageReady() async -> Bool? {
        await operations.storageReady()
    }

    // MARK: Old app compatibility

    /// The image's `kernarium_oldapp` command, over the application's own adb channel.
    private var oldApps: OldAppCompatibility { OldAppCompatibility(shell: AdbGuestShell(operations: operations)) }

    func listOldApps(limit: Duration?) async -> OldAppListing {
        await oldApps.list(limit: limit ?? OldAppCompatibility.listLimit)
    }

    func setOldApp(_ package: String, on: Bool) async -> OldAppWrite {
        await oldApps.set(package: package, on: on)
    }

    func isOldAppOpen(_ package: String) async -> Bool? {
        await oldApps.isOpen(package: package)
    }

    // MARK: Add to Home Screen

    /// The image's `kernarium_apps` tool, over the application's own adb channel.
    var androidAppsTool: (any AndroidAppsTool)? {
        AndroidAppsCommands(shell: AdbGuestShell(operations: operations))
    }

    /// Android is ready for an app when its boot is complete, its storage is up,
    /// a home activity resolves and its first-run setup is not on the screen.
    /// The guest is asked only while it is awake (a guest being resumed is not
    /// asked), and the wait counts the guest's running time, so the time the
    /// application spends in the background does not run it out.
    func awaitAndroidReady(homeReached: Bool) async -> AndroidAppReadiness.Outcome {
        let shell = AdbGuestShell(operations: operations)
        let clock = clock
        return await AndroidAppReadiness.wait(
            check: { [weak self] in
                guard let self, pause.guestIsAwake else { return .notYet }
                return await AndroidAppReadiness.check(
                    shell: shell, storageReady: { await self.storageReady() }, homeReached: homeReached)
            },
            now: { clock.now },
            sleep: { try? await Task.sleep(for: $0) })
    }

    func sendSms(_ request: GuestSmsRequest) async -> GuestSmsChannel.Outcome {
        await GuestSmsChannel(connector: TextMessageConnector()).send(request.payload)
    }

    // MARK: Android's screen reader

    func readScreenReader() async -> ScreenReaderState? {
        await operations.screenReader()
    }

    func changeScreenReader(_ on: Bool, service: String) async throws -> ScreenReaderState {
        try await operations.changeScreenReader(on, service: service)
    }

    func openAccessibilitySettings() async throws {
        try await operations.openAccessibilitySettings()
    }

    // MARK: ADB over network

    /// The listener on the network addresses, whose every connection is
    /// joined to a fresh stream to the guest's adbd.
    private let network = AdbNetworkBridge(opener: VsockConnector())

    func startAdbOverNetwork() throws {
        try network.start()
    }

    func stopAdbOverNetwork() {
        network.stop()
    }

    func localNetworkAddress() -> String? {
        LanAddress.current()
    }

    // MARK: Events

    /// The sink of a run's console, which the loader feeds on the main queue in
    /// the order the runtime wrote it. It applies what it finds there and
    /// then, synchronously, so that a report of an old boot cannot overtake the
    /// restart that follows it: the loader delivers the console and the
    /// restart events on the same queue, and any hop of the report's own
    /// would run after the restart's.
    func makeConsoleSink() -> ConsoleSink {
        ConsoleSink { [weak self] milestones, bootCompleted, recovery in
            MainActor.assumeIsolated {
                self?.reached(milestones, bootCompleted: bootCompleted, recoveryStarted: recovery)
            }
        }
    }

    /// The application is away while a start is being prepared, before the
    /// runtime runs (a guest that runs is held by `PauseController`, which
    /// stops the clock): the span is not the start's.
    private func holdStartClock() {
        guard !runActive, pending != nil || launching else { return }
        startClockHeld = true
        clock.pause()
    }

    private func releaseStartClock() {
        guard startClockHeld else { return }
        startClockHeld = false
        clock.resume()
    }

    private func reached(_ milestones: [BootMilestone], bootCompleted: Bool, recoveryStarted: Bool) {
        guard let model else { return }
        // The guest's display exists, so the runtime has decided what it
        // granted the run: before the screen the run is shown on is.
        learnDisplayGrant()
        // The guest console answers, so the emulator exists and takes requests
        // for the keyboard and the mouse.
        if !inputRunReady {
            inputRunReady = true
            updateKeyboardMouse()
        }
        let seconds = Int(Date().timeIntervalSince(bootStartedAt))
        // Before the milestones: the boot screen ends at `.ready` and shows
        // what the guest turned out to be.
        if recoveryStarted { model.bootTracker.noteRecovery() }
        if bootCompleted {
            reachedBootCompleted = true
            partialSweep?.guestBooted()
            updateThermal()
            updateGuestRotation()
            updateSpaceWatch()
            // An update counts as applied once its start has booted, whatever
            // happens to the application after that.
            do { try storage.bootCompleted() } catch { print("Andarium: the guest's record was not updated: \(error.localizedDescription)") }
        }
        for milestone in milestones where model.bootTracker.reach(milestone, atSeconds: seconds) {
            print("Andarium: boot reached \(milestone) at \(seconds) s")
        }
    }

    func guestRestarted(_ event: GuestRestartEvent) {
        print("Andarium: \(event)")
        guard case .began = event, let model else { return }
        // The boot that follows is another one: it ends an update only by
        // reaching sys.boot_completed itself, and its console is read afresh.
        reachedBootCompleted = false
        console?.restarted()
        // No import outlives the restart (the browser stops its transfer
        // below and the guest's adb connections end with it), so the files of
        // the run so far are leftovers to the boot that follows. Only the
        // removal of a cut-off import's own partial file waits for that boot,
        // and it names its file.
        runMark = GuestRunMark.random()
        partialSweep?.guestRestarting()
        spaceAlertOpen = false
        motion.stop()
        haptics.restart()
        gameControllers.restart()
        thermal.restart()
        location.guestRestarted()
        keyboardMouse.guestRestarted()
        updateThermal()
        updateGuestRotation()
        updateSpaceWatch()
        switch model.phase {
        case .running, .booting:
            bootStartedAt = Date()
            model.restart()
        default:
            break
        }
    }

    private func finished(result: Int32) {
        partialSweep?.runEnded()
        partialSweep = nil
        normalRun = false
        runActive = false
        inputRunReady = false
        shuttingDown = false
        spaceAlertOpen = false
        motion.stop()
        updateGuestRotation()
        updateSpaceWatch()
        updateThermal()
        thermalMonitor.stop()
        // A run that has ended holds nothing: the hold goes with the grant.
        runHasHighRefresh = false
        displayGrantKnown = false
        lowPowerMonitor.stop()
        stopThermalObservation()
        kernarium_runtime_loader_set_restart_handler(nil)
        kernarium_runtime_loader_set_console_output_handler(nil)
        console = nil
        pause.guestDidEnd()
        runStop = nil
        view = nil
        geometry = nil
        launching = false
        UIApplication.shared.isIdleTimerDisabled = false
        print(ConsoleReport.runtimeReturned(result))
        // A start that reached sys.boot_completed and ended with Android's own
        // power-off is a completed one (`lastCompleted`); the pending update it
        // may have started ended when the start booted.
        if result == 0, reachedBootCompleted {
            do { try storage.completeRun() } catch { print("Andarium: the guest's record was not updated: \(error.localizedDescription)") }
        }
        let uptime = runStartedAt.map { Int(Date().timeIntervalSince($0)) }
        runStartedAt = nil
        // The run wrote to the data disk, so the device has less free space
        // than before it.
        model?.host.freeStorageBytes = freeSpace()
        let stopped = userStopped
        userStopped = false
        model?.runEnded(
            Self.runEnd(result: result, memoryMiB: runMemoryMiB, stoppedByUser: stopped), uptimeSeconds: uptime)
    }

    /// How a run that returned `result` ended, as the screens tell it.
    /// `memoryMiB` is the size the run asked for. The runtime says the host
    /// would not give the guest's memory with a result of its own: the run
    /// never started its kernel, and a smaller size is what starts. A bare
    /// out-of-memory status of the library (-2) is not that.
    static func runEnd(result: Int32, memoryMiB: Int, stoppedByUser: Bool) -> RunEnd {
        RunEnd(
            result: result,
            restartedInALoop: result == Int32(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP),
            stoppedByUser: stoppedByUser,
            memoryRefusedMiB: result == Int32(KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED) ? memoryMiB : nil)
    }

    // MARK: Keys

    func press(_ key: GuestKey) {
        guard !holdsGuestStill else { return }
        kernarium_runtime_loader_send_key(key.rawValue, 1)
        kernarium_runtime_loader_send_key(key.rawValue, 0)
    }

    /// Holds the key down for `seconds`, as Power is for Android's power menu.
    func hold(_ key: GuestKey, seconds: Double = LiveRuntime.powerMenuHold) {
        guard !holdsGuestStill else { return }
        kernarium_runtime_loader_send_key(key.rawValue, 1)
        Task {
            try? await Task.sleep(for: .seconds(seconds))
            kernarium_runtime_loader_send_key(key.rawValue, 0)
        }
    }

    // MARK: Motion sensors

    /// The interface orientation of the guest's window; nil while the display
    /// is not in a scene, which leaves the mount as it is.
    private var interfaceMotionAxes: MotionSample.Axes? {
        guard let scene = view?.window?.windowScene else { return nil }
        return switch scene.effectiveGeometry.interfaceOrientation {
        case .landscapeLeft: .landscapeLeft
        case .landscapeRight: .landscapeRight
        case .portraitUpsideDown: .portraitUpsideDown
        default: .portrait
        }
    }

    private var rotationPolicy: GuestRotationPolicy {
        model?.hostDrivesGuestRotation == true ? .host : .guest
    }

    /// The scene's geometry changed, which may include its interface
    /// orientation. The hosting controller coalesces its triggers into one call
    /// per turn, and the mount restarts the sensors only when it moves.
    func sceneGeometryDidChange() {
        guard normalRun else { return }
        // The scene may have moved to another screen, which shows a different
        // number of frames a second than the last one did.
        updateSixtyHertzHold()
        updateGuestRotation()
    }

    private func updateGuestRotation() {
        let policy = rotationPolicy
        // Where the guest owns rotation (an iPhone, which is portrait-only) a
        // new period mounts the virtual device to the orientation UIKit holds
        // and keeps that frame; where the host does (an iPad session, in a
        // window or full screen) the guest is drawn upright in the scene and
        // the mount follows the scene's orientation. Never remap per sensor
        // sample.
        if motionMount.update(policy: policy, interface: interfaceMotionAxes) { motion.restart() }
        updateCameraRotation(policy: policy)
        rotation.update(policy: policy, ready: normalRun && reachedBootCompleted && active && !holdsGuestStill)
        updateMotionSensors()
        updateHaptics()
        updateGameControllers()
        updateLocation()
        updateKeyboardMouse()
    }

    /// Tells the camera the turn of the device body: where the host owns the
    /// rotation (an iPad window) the window's orientation says it, else it is
    /// zero (`CameraRotation`). The guest hears the turn at its start, for each
    /// camera's SENSOR_ORIENTATION, and a later change turns the picture. The
    /// runtime keeps the figure across runs, so a run that starts without a
    /// scene to read says zero rather than leave an earlier run's turn; only a
    /// change is sent.
    private func updateCameraRotation(policy: GuestRotationPolicy, starting: Bool = false) {
        guard runActive else { return }
        let degrees: UInt32
        if let axes = interfaceMotionAxes {
            degrees = CameraRotation.degrees(policy: policy, interface: axes)
        } else if starting {
            degrees = 0
        } else {
            return
        }
        guard degrees != cameraRotationSent else { return }
        if kernarium_runtime_loader_set_camera_rotation(degrees) == 0 { cameraRotationSent = degrees }
    }

    func setNetworkNotice(_ denial: NetworkDenial?) {
        // The runtime has the value only once a run has loaded it; the run
        // says it again then (`launch`).
        _ = kernarium_runtime_loader_set_network_notice(Self.networkNoticeCause(denial))
    }

    /// The runtime's word for the denial in force (KernariumRuntime.h).
    static func networkNoticeCause(_ denial: NetworkDenial?) -> Int32 {
        switch denial {
        case nil: Int32(KERNARIUM_RUNTIME_NETWORK_NOTICE_NONE)
        case .cellular?: Int32(KERNARIUM_RUNTIME_NETWORK_NOTICE_CELLULAR_DENIED)
        case .wifi?: Int32(KERNARIUM_RUNTIME_NETWORK_NOTICE_WIFI_DENIED)
        }
    }

    /// The sensors are the guest's wherever the device has them, while the
    /// guest is in front; a sensor is read only while an application in the
    /// guest uses it (`MotionSession`).
    private func updateMotionSensors() {
        let foreground = normalRun && reachedBootCompleted && active && !holdsGuestStill
            && rotation.applied == rotationPolicy
        // Where the interface does not turn and the guest owns rotation, Android
        // follows the device's orientation as iOS reports it, which already
        // includes the person's Portrait Orientation Lock. That is the display's
        // own behaviour, not a reading an application takes, so the sensors do
        // not decide it.
        let follows = model?.followsDeviceOrientation == true && rotationPolicy == .guest
        motion.update(
            foreground: foreground,
            axes: motionMount.axes,
            followsOrientation: follows
        )
        deviceOrientation.observe(follows && foreground)
    }

    /// The device's orientation changed, or nil once it is no longer observed.
    /// The guest is given it if its image takes orientations.
    func deviceOrientationChanged(_ orientation: GuestOrientation?) {
        motion.setOrientation(orientation)
    }

    // MARK: Keyboard and mouse

    /// The guest's keyboard and mouse follow the run and the foreground: they
    /// exist from the first word the guest's console says to its end, and the
    /// display's keys and pointer reach them while the application is in front
    /// and the guest is not held still.
    private func updateKeyboardMouse() {
        keyboardMouse.update(
            running: runActive && inputRunReady, foreground: active && !holdsGuestStill,
            overlaid: !(model?.guestHasKeys ?? true))
    }

    /// A sheet, an alert, Settings or the share sheet over the guest takes the
    /// keys, and with them the pointer lock the guest's capture asked for.
    private func observeOverlay() {
        guard let model else { return }
        withObservationTracking {
            _ = model.guestHasKeys
        } onChange: {
            Task { @MainActor in
                guard self.model != nil else { return }
                self.updateKeyboardMouse()
                self.observeOverlay()
            }
        }
    }

    func setPointerAsTouch(_ on: Bool) {
        keyboardMouse.setPointerAsTouch(on)
    }

    /// A UI test's launch (`InputProbeLaunch`) has no guest: the session is
    /// treated as one whose run has started, so the guest's keyboard and mouse
    /// can be exercised. Nothing else of a run (rotation, sensors, haptics)
    /// starts, because nothing answers it.
    func beginInputProbe() {
        runActive = true
        inputRunReady = true
        updateKeyboardMouse()
    }

    /// A probe launch that states iOS's thermal state (`ThermalOverride`)
    /// runs the thermal session as a normal run does, against the probe's own
    /// stand-in for the guest's HAL (`ThermalProbeHal`): the policy, the
    /// notice, Settings' choice and what Android would be told all work.
    func beginThermalProbe() {
        runActive = true
        thermalProbe = true
        updateThermal()
        thermalMonitor.start()
    }

    func pointerLockDidChange(_ locked: Bool) {
        keyboardMouse.pointerLockChanged(locked)
    }

    // MARK: Haptics

    /// The guest's vibrator plays while a normal run is in front and holds
    /// the guest running; the connection is made from the run's start, because
    /// the guest's HAL starts early. Android's own settings decide what it
    /// asks for, and the Haptics switch (or a launch that silences them,
    /// `HapticsLaunch`) whether the device plays it: the guest keeps its
    /// vibrator either way, and while they are off the session holds no
    /// connection and the player no engine for the device. A game controller's
    /// rumble is not governed by either.
    private func updateHaptics() {
        haptics.update(
            available: hapticsAvailable, enabled: model?.hapticsPlays ?? true, running: normalRun,
            booted: reachedBootCompleted, foreground: active && !holdsGuestStill)
    }

    func setHaptics(_ on: Bool) {
        updateHaptics()
    }

    // MARK: Game controllers

    /// The game controllers are the guest's while a normal run is in front and
    /// holds the guest running, and are looked for from the run's start to its
    /// end. The guest's relay starts early, so the connection is made at once.
    private func updateGameControllers() {
        gameControllers.update(
            running: normalRun, booted: reachedBootCompleted, foreground: active && !holdsGuestStill)
    }

    // MARK: Location

    /// Sets the location session to what is wanted now: the switch as the
    /// screens show it, what iOS allows, whether Andarium is in front and not
    /// holding the guest still, and whether a normal run goes on. The guest's
    /// own receiver decides the rest, and the session ends it all the moment
    /// a run does.
    private func updateLocation() {
        guard let model else { return }
        location.update(
            enabled: model.locationShownOn, authorization: model.locationAuthorization,
            foreground: active && !holdsGuestStill, running: normalRun)
    }

    // MARK: Microphone

    // MARK: Statistics

    /// Both readings are made on one serial queue of their own: the memory
    /// request waits for the guest and neither runs on the main thread or on
    /// the cooperative pool, and two never overlap.
    private let statsQueue = DispatchQueue(label: "com.imxieyi.andarium.stats", qos: .utility)

    func readStatistics() async -> StatsSample? {
        await withCheckedContinuation { continuation in
            statsQueue.async { continuation.resume(returning: Self.statisticsReading()) }
        }
    }

    func readGraphicsRefusals() async -> UInt64? {
        await withCheckedContinuation { continuation in
            statsQueue.async { continuation.resume(returning: Self.graphicsRefusalsReading()) }
        }
    }

    func readGraphicsMemory(minimumIdle: Duration, minimumBytes: UInt64) async -> GraphicsMemoryReading? {
        let (seconds, attoseconds) = minimumIdle.components
        let idle = UInt32(clamping: max(seconds, 0) * 1000 + max(attoseconds, 0) / 1_000_000_000_000_000)
        return await withCheckedContinuation { continuation in
            statsQueue.async {
                continuation.resume(
                    returning: Self.graphicsMemoryReading(minimumIdleMilliseconds: idle, minimumBytes: minimumBytes))
            }
        }
    }

    func reclaimGraphicsMemory(_ reading: GraphicsMemoryReading, using policy: GraphicsReclaim) async
        -> GraphicsReclaimResult
    {
        await policy.reclaim(from: reading, shell: AdbGuestShell(operations: operations))
    }

    func readGuestMemory() async -> GuestMemorySample? {
        await withCheckedContinuation { continuation in
            statsQueue.async { continuation.resume(returning: Self.guestMemoryReading()) }
        }
    }

    private nonisolated static func graphicsMemoryReading(
        minimumIdleMilliseconds: UInt32, minimumBytes: UInt64
    ) -> GraphicsMemoryReading? {
        var reading = kernarium_runtime_graphics_memory()
        reading.struct_size = UInt32(MemoryLayout<kernarium_runtime_graphics_memory>.size)
        reading.minimum_idle_ms = minimumIdleMilliseconds
        reading.minimum_holder_bytes = minimumBytes
        guard kernarium_runtime_loader_read_graphics_memory(&reading) == 0 else { return nil }
        let count = Int(min(reading.holder_count, UInt32(KERNARIUM_RUNTIME_GRAPHICS_HOLDERS)))
        let holders = withUnsafeBytes(of: &reading.holders) { raw -> [GraphicsHolder] in
            let table = raw.bindMemory(to: kernarium_runtime_graphics_holder.self)
            return (0..<count).map { index in
                var entry = table[index]
                let name = withUnsafeBytes(of: &entry.name) { bytes in
                    String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
                }
                return GraphicsHolder(
                    pid: entry.pid, contexts: entry.context_count, bytes: entry.bytes,
                    idle: .milliseconds(entry.idle_ms), name: name)
            }
        }
        return GraphicsMemoryReading(
            budgetBytes: reading.budget_bytes, usageBytes: reading.usage_bytes, contextBytes: reading.context_bytes,
            refusals: reading.memory_refusals, guestReclaims: reading.guest_reclaims != 0, holders: holders)
    }

    private nonisolated static func graphicsRefusalsReading() -> UInt64? {
        var reading = kernarium_runtime_statistics()
        reading.struct_size = UInt32(MemoryLayout<kernarium_runtime_statistics>.size)
        guard kernarium_runtime_loader_read_statistics(&reading) == 0 else { return nil }
        return reading.gpu_memory_refusals
    }

    private nonisolated static func statisticsReading() -> StatsSample? {
        var reading = kernarium_runtime_statistics()
        reading.struct_size = UInt32(MemoryLayout<kernarium_runtime_statistics>.size)
        guard kernarium_runtime_loader_read_statistics(&reading) == 0 else { return nil }
        return StatsSample(
            bootIndex: reading.boot_index, processorCount: reading.processor_count,
            timestampNanoseconds: reading.timestamp_ns, processorTimeNanoseconds: reading.processor_time_ns,
            presentedFrames: reading.presented_frames, ramBytes: reading.memory_bytes,
            ramHeldBytes: reading.memory_held_bytes, gpuMemoryBudgetBytes: reading.gpu_memory_budget_bytes,
            gpuMemoryUsedBytes: reading.gpu_memory_used_bytes
        )
    }

    private nonisolated static func guestMemoryReading() -> GuestMemorySample? {
        var reading = kernarium_runtime_guest_memory()
        reading.struct_size = UInt32(MemoryLayout<kernarium_runtime_guest_memory>.size)
        guard kernarium_runtime_loader_read_guest_memory(&reading) == 0 else { return nil }
        return GuestMemorySample(totalBytes: reading.total_bytes, availableBytes: reading.available_bytes)
    }

    // MARK: Thermal state

    // Two readers of the thermal state. The guest's thermal HAL is told how
    // hot the device is in every build (`ThermalSession`). The state and the
    // uptime also go to the console for the iPad harness
    // (`Shared/ConsoleReport.swift`).

    /// Reads what the runtime granted the run's display, once: the 120 Hz
    /// display (the screen of the display's own view, the host and the size
    /// were checked at the start, once, for everything that depends on it) or
    /// the 60 Hz one. The run has that, whatever the start asked, and so does
    /// the hold.
    private func learnDisplayGrant() {
        guard runActive, !displayGrantKnown, let rate = Self.displayRefreshReading() else { return }
        displayGrantKnown = true
        runHasHighRefresh = rate >= 120
        model?.runStarted(withHighRefresh: runHasHighRefresh)
        updateSixtyHertzHold()
    }

    /// Android is held to 60 Hz while the run has the 120 Hz display and what
    /// the screen shows is 60 at the most: iOS in Low Power Mode, or a screen
    /// the window moved to that shows less than 100 frames a second.
    private func updateSixtyHertzHold() {
        thermal.setSixtyHertzHold(
            HighRefresh.holdsAtSixty(
                displayIs120: runHasHighRefresh, lowPowerMode: lowPowerModeOn,
                screenMaximumFramesPerSecond: model?.screenMaximumFramesPerSecond ?? 60))
    }

    /// The rate the runtime granted the run's display, nil while there is no
    /// display (before the guest's kernel starts).
    private nonisolated static func displayRefreshReading() -> UInt32? {
        var reading = kernarium_runtime_statistics()
        reading.struct_size = UInt32(MemoryLayout<kernarium_runtime_statistics>.size)
        guard kernarium_runtime_loader_read_statistics(&reading) == 0, reading.display_refresh_hz != 0 else {
            return nil
        }
        return reading.display_refresh_hz
    }

    /// The guest's thermal HAL is told while a normal run goes on; Recovery
    /// has none.
    private func updateThermal() {
        thermal.setLimit(model?.heatLimitOn ?? true)
        if thermalProbe {
            thermal.update(running: true, booted: true)
            return
        }
        thermal.update(running: normalRun, booted: reachedBootCompleted)
    }

    /// Settings' "When hot" changed: Android is told nothing hot at once, or
    /// the policy's severity at once.
    func setHeatLimit(_ on: Bool) {
        updateThermal()
    }

    /// Android starts to lower its frame rate: the first time in the run that
    /// the device is hot enough and the HAL has heard of it.
    private func thermalCapStarted() {
        guard runActive, normalRun || thermalProbe else { return }
        model?.thermalCapStarted()
    }

    private func reportThermalState(_ phase: ConsoleReport.ThermalPhase) {
        print(ConsoleReport.thermal(
            phase: phase, state: ProcessInfo.processInfo.thermalState,
            uptime: ProcessInfo.processInfo.systemUptime, heatLimitOn: model?.heatLimitOn ?? true
        ))
    }

    private func startThermalObservation() {
        reportThermalState(.start)
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.reportThermalState(.change) }
        }
    }

    private func stopThermalObservation() {
        reportThermalState(.finish)
        if let thermalObserver {
            NotificationCenter.default.removeObserver(thermalObserver)
            self.thermalObserver = nil
        }
    }

    // MARK: Free space

    /// A guest paused for space, or going down after it, is asked nothing: a
    /// request to it would wait, or fail as it goes. Nor is one the screen
    /// says the space is short for: the person decides what happens to it.
    private var holdsGuestStill: Bool { pause.isPausedForSpace || shuttingDown || spaceAlertOpen }

    /// The space is watched while Android runs a normal start it has booted
    /// and the application holds it in the foreground: before it has booted
    /// the start check stands for the space, and a guest the application
    /// paused for the background or for space needs no watching.
    private var wantsSpaceWatch: Bool {
        runActive && normalRun && reachedBootCompleted && !holdsGuestStill && pause.phase == .foreground
    }

    private func updateSpaceWatch() {
        guard wantsSpaceWatch else {
            spaceWatchTask?.cancel()
            spaceWatchTask = nil
            return
        }
        guard spaceWatchTask == nil else { return }
        spaceWatchGeneration += 1
        let generation = spaceWatchGeneration
        spaceWatchTask = Task { [weak self] in
            await self?.watchSpace()
            if let self, self.spaceWatchGeneration == generation { self.spaceWatchTask = nil }
        }
    }

    /// Reads the space now and then, as often as the guest could write what
    /// is left before the pause (`RunSpace.nextCheck`), and stops once it
    /// has paused the guest.
    private func watchSpace() async {
        while !Task.isCancelled {
            guard wantsSpaceWatch else { return }
            let free = freeSpace()
            model?.host.freeStorageBytes = free
            if RunSpace.verdict(freeBytes: free) == .pause {
                pauseForSpace(freeBytes: free)
                return
            }
            try? await Task.sleep(for: RunSpace.nextCheck(freeBytes: free))
        }
    }

    private func pauseForSpace(freeBytes: UInt64) {
        guard pause.beginSpacePause() else { return }
        print("Andarium: the guest is pausing: \(Formats.size(bytes: freeBytes)) of the device's space is free")
        spaceAlertOpen = true
        motion.stop()
        updateGuestRotation()
        model?.guestPausingForSpace(freeBytes: freeBytes)
        Task { [weak self] in await self?.settleSpacePause() }
    }

    /// Suspends the guest and says what came of it: the screen offers Resume
    /// over a guest that holds still, and Shut down over one that did not.
    private func settleSpacePause() async {
        guard let outcome = await pause.settleSpacePause() else { return }
        switch outcome {
        case .held:
            print("Andarium: the guest is paused")
            model?.guestPausedForSpace()
        case .running:
            print("Andarium: the guest could not be paused")
            model?.guestNotPausedForSpace()
        }
    }

    /// The person asks to check the space and pause again, over a guest that
    /// did not pause. Returns whether there is enough space for it to run on,
    /// which takes the screen away; otherwise the guest is paused again.
    func checkSpaceAfterPauseFailed() async -> Bool {
        let free = freeSpace()
        model?.host.freeStorageBytes = free
        guard !RunSpace.allowsResume(freeBytes: free) else {
            spaceAlertOpen = false
            updateGuestRotation()
            updateSpaceWatch()
            return true
        }
        guard pause.beginSpacePause() else { return false }
        await settleSpacePause()
        return false
    }

    func resumeAfterSpacePause() async -> Bool {
        let free = freeSpace()
        model?.host.freeStorageBytes = free
        guard RunSpace.allowsResume(freeBytes: free) else { return false }
        await wakeGuest()
        guard !pause.isPausedForSpace else { return false }
        print("Andarium: the guest resumes: \(Formats.size(bytes: free)) of the device's space is free")
        spaceAlertOpen = false
        updateGuestRotation()
        updateSpaceWatch()
        return true
    }

    func shutDownAfterSpacePause() async throws {
        shuttingDown = true
        await wakeGuest()
        guard !pause.isPausedForSpace else {
            shuttingDown = false
            throw AdbFailure(message: String(localized: "Android is still paused."))
        }
        do {
            let result = try await operations.shell("reboot -p", limit: .seconds(30))
            if let code = result.exitCode, code != 0 {
                throw AdbFailure(message: String(localized: "Android refused to power off (\(code))."))
            }
            // Android powers off after the command returns, which ends the
            // run; the screen goes with it. Until it has, it is going down.
            try await waitForRunToEnd(.seconds(120))
        } catch {
            // The shell's stream can end because the guest is already going
            // down: the run ending says so.
            if runActive { try? await waitForRunToEnd(.seconds(5)) }
            guard runActive else { return }
            shuttingDown = false
            spaceAlertOpen = false
            updateGuestRotation()
            updateSpaceWatch()
            throw error
        }
    }

    private func waitForRunToEnd(_ limit: Duration) async throws {
        let deadline = ContinuousClock.now + limit
        while runActive {
            guard ContinuousClock.now < deadline else {
                throw AdbFailure(message: String(localized: "Android did not power off."))
            }
            try await Task.sleep(for: .milliseconds(250))
        }
    }

    /// Wakes the guest the pause holds and returns once it is back.
    private func wakeGuest() async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            if !pause.resumeFromSpacePause(finished: { continuation.resume() }) { continuation.resume() }
        }
    }

    // MARK: Lifecycle

    private func observeLifecycle() {
        let edges: [(Notification.Name, @MainActor (LiveRuntime) -> Void)] = [
            (UIApplication.willResignActiveNotification, {
                print(ConsoleReport.application(active: false))
                $0.active = false
                $0.model?.applicationActive = false
                $0.motion.stop()
                $0.updateGuestRotation()
                $0.pause.willResignActive()
            }),
            (UIApplication.didEnterBackgroundNotification, {
                $0.pause.didEnterBackground()
                $0.holdStartClock()
                $0.updateSpaceWatch()
            }),
            (UIApplication.willEnterForegroundNotification, {
                $0.pause.willEnterForeground()
                $0.releaseStartClock()
                $0.updateSpaceWatch()
            }),
            (UIApplication.willTerminateNotification, { $0.pause.willTerminate() }),
            (UIApplication.didBecomeActiveNotification, { $0.didBecomeActive() }),
        ]
        observers = edges.map { name, edge in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    if let self { edge(self) }
                }
            }
        }
    }

    /// The person may have changed the permissions or the storage while
    /// Andarium was away.
    private func didBecomeActive() {
        print(ConsoleReport.application(active: true))
        active = true
        updateGuestRotation()
        if normalRun {
            thermalMonitor.refresh()
            lowPowerMonitor.refresh()
        }
        guard let model else { return }
        model.applicationActive = true
        let free = freeSpace()
        model.host.freeStorageBytes = free
        // A guest paused for space waits for the person, who may have made
        // room while Andarium was away: the screen says what is free now.
        if model.spacePause != nil { model.spacePause?.freeBytes = free }
        model.microphone?.refresh()
        model.location?.refresh()
        model.camera?.refresh()
    }

    private func observeRotationPolicy() {
        guard let model else { return }
        withObservationTracking {
            _ = model.hostDrivesGuestRotation
        } onChange: {
            Task { @MainActor in
                guard self.model != nil else { return }
                self.updateGuestRotation()
                self.observeRotationPolicy()
            }
        }
    }
}

/// Pausing the guest on the real runtime. The runtime calls run on one serial
/// queue, in the order asked, so a quick return to the app resumes behind a
/// quiesce still in flight.
@MainActor
final class LivePauseEffects: PauseEffects {
    private let queue = DispatchQueue(label: "com.imxieyi.andarium.guest-sync", qos: .userInitiated)

    func holdGuestGPU() {
        let result = kernarium_runtime_loader_set_gpu_hold(1)
        print("Andarium: guest GPU requests held on background (\(result))")
    }

    func releaseGuestGPU() {
        let result = kernarium_runtime_loader_set_gpu_hold(0)
        print("Andarium: guest GPU requests released (\(result))")
    }

    func flushGuest(until deadline: DispatchTime?) {
        settle(reason: "inactive", quiesce: false, until: deadline)
    }

    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        settle(reason: reason, quiesce: true, until: deadline)
    }

    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        queue.async {
            kernarium_runtime_loader_resume()
            Task { @MainActor in finished() }
        }
    }

    /// Asks the runtime to stop the run `stop` belongs to, off the main actor:
    /// the request may wait for a guest being restored or for the runtime's
    /// own lock. It goes through the queue of the other runtime calls that
    /// wait, so it comes after a hold that was asked for before it, and is made
    /// again until the run ends, with the guest's write-back before the first
    /// that reaches one (`GuestStopDelivery`).
    ///
    /// `settle` and `request` are the runtime's, and `interval` how long after
    /// a request it is made again; `settle` returns whether it reached a guest.
    /// A test supplies its own.
    func deliverStop(
        _ stop: RunStop, every interval: DispatchTimeInterval = GuestStopDelivery.interval,
        settle: (@Sendable () -> Bool)? = nil,
        request: @escaping @Sendable () -> Void = { kernarium_runtime_loader_request_stop() }
    ) {
        let settle = settle ?? { Self.settleBeforeStop(for: stop) }
        GuestStopDelivery.deliver(stop, on: queue, every: interval, settle: settle, request: request)
    }

    /// The runtime's write-back of the guest's storage, for the Stop of the run
    /// `stop` belongs to. Returns whether a guest was reached.
    nonisolated static func settleBeforeStop(for stop: RunStop) -> Bool {
        GuestStopDelivery.settleBeforeStop(for: stop, logPrefix: "Andarium: ")
    }

    /// Writes the guest's storage back and suspends it with its RAM left in
    /// the process: the device is short of the space an image of it takes.
    func suspendGuestResident(reason: String, finished: @escaping @MainActor () -> Void) {
        let assertion = PauseTaskAssertion(named: "guest-sync")
        queue.async {
            let settle = GuestSettle.quiesceResident()
            Task { @MainActor in
                print("Andarium: " + settle.logLine(reason: reason, kept: "RAM kept"))
                assertion.end()
                finished()
            }
        }
    }

    /// Set by the runtime that owns this: where the guest's statistics come
    /// from, on their own queue.
    var statistics: (@MainActor () async -> StatsSample?)?

    func guestProcessorTime() async -> UInt64? {
        await statistics?()?.processorTimeNanoseconds
    }

    /// Writes the guest's storage back, and suspends the guest when asked,
    /// under an assertion that keeps the process scheduled until it is done.
    private func settle(reason: String, quiesce: Bool, until deadline: DispatchTime?) {
        let assertion = PauseTaskAssertion(named: "guest-sync")
        let finished = DispatchSemaphore(value: 0)
        queue.async {
            let settle = quiesce ? GuestSettle.quiesce() : GuestSettle.sync()
            finished.signal()
            Task { @MainActor in
                print("Andarium: " + settle.logLine(reason: reason))
                assertion.end()
            }
        }
        if let deadline {
            _ = finished.wait(timeout: deadline)
        }
    }
}

/// A background-task assertion that keeps the process scheduled until it ends:
/// the caller ends it, or iOS expires it. Ending it twice is harmless.
@MainActor
final class PauseTaskAssertion {
    private var identifier = UIBackgroundTaskIdentifier.invalid

    init(named name: String) {
        identifier = UIApplication.shared.beginBackgroundTask(withName: name) { [weak self] in
            MainActor.assumeIsolated { self?.end() }
        }
    }

    func end() {
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }
}

// MARK: Snapshots

/// The guest's data snapshots. The application's own state is the lock on the
/// disk: no file lock says on iOS that a guest holds it, so an operation runs
/// only while the guest is stopped (`SnapshotRules.gate`), one at a time, off
/// the main actor, and a start, an erase or the update's Restore waits for the
/// one under way.
extension LiveRuntime: SnapshotDriver {
    var snapshotGate: SnapshotRefusal? {
        guard let model else { return .guestNotStopped }
        return SnapshotRules.gate(
            phase: model.phase, runtimeActive: activity.runtimeActive, operationUnderWay: activity.operation != nil)
    }

    /// Tells the model what the driver is doing when it changed.
    fileprivate func publishSnapshotActivity() {
        let now = SnapshotActivity(
            runtimeActive: SnapshotRules.runtimeActive(
                running: runActive, pending: pending != nil, launching: launching, preparing: preparing),
            operation: snapshotWork ?? (storageBusy ? .guestStorage : nil), revision: snapshotRevision)
        guard now != activity else { return }
        activity = now
        activityChanged?(now)
    }

    func snapshotListing(measured: Bool) async -> SnapshotListing {
        let storage = self.storage
        while true {
            // A caller that was cancelled wants no answer: it gets what the
            // files say now, unmeasured, without waiting or reading again.
            // Waiting on a cancelled task would return at once and spin here.
            let cancelled = Task.isCancelled
            if !cancelled { await settleSnapshotOperation() }
            // A guest being set up or erased changes the files under a listing.
            while storageBusy, !Task.isCancelled { try? await Task.sleep(for: .milliseconds(50)) }
            let revision = snapshotRevision
            let quick = cancelled || Task.isCancelled
            // A cancellation reaches the measure, which stops between two
            // files and answers unmeasured.
            let reading = Task.detached(priority: .utility) { storage.snapshotListing(measured: measured && !quick) }
            var listing = await withTaskCancellationHandler { await reading.value } onCancel: { reading.cancel() }
            // An operation that started or finished while this one read makes
            // it older than the files: read again.
            guard quick || (snapshotOperation == nil && !storageBusy && snapshotRevision == revision) else { continue }
            listing.revision = revision
            listing.whileNotStopped = snapshotGate
            if var unfinished = listing.unfinishedRestore {
                unfinished.lastFailure = rollForwardFailure
                listing.unfinishedRestore = unfinished
            }
            return listing
        }
    }

    func createSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary> {
        await performSnapshot(.create, gated: true) { try $0.createSnapshot(named: name) }
    }

    func restoreSnapshot(_ id: SnapshotID, guest: Guest) async -> SnapshotResult<Guest> {
        await performSnapshot(.restore, gated: true) { try $0.restoreSnapshot(id, guest: guest) }
    }

    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        await performSnapshot(.delete, gated: true) { try $0.deleteSnapshot(id) }
    }

    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void> {
        await performSnapshot(.rename, gated: false) { try $0.renameSnapshot(id, to: name) }
    }

    /// Waits for the snapshot operation under way, if any.
    fileprivate func settleSnapshotOperation() async {
        while let operation = snapshotOperation { await operation.value }
    }

    /// Waits for the preparation of a start, if one is in hand: it writes the
    /// guest's files, and goes on to its end after the person stopped the
    /// start. Not for the preparation's own use (it settles the snapshot
    /// operation itself).
    fileprivate func settlePreparation() async {
        while let task = preparation { await task.value }
    }

    /// Waits until nothing else writes the guest's files: the preparation of a
    /// start and the snapshot operation under way, which may follow one
    /// another. What changes the files, besides a start, begins after it.
    fileprivate func settleGuestFiles() async {
        while preparation != nil || snapshotOperation != nil {
            await settlePreparation()
            await settleSnapshotOperation()
        }
    }

    /// Runs `body` on the storage off the main actor as the one operation
    /// under way. A gated one refuses unless the guest is stopped and idle;
    /// one that is not (Rename) waits its turn, and refuses only while a guest
    /// is being set up or erased.
    private func performSnapshot<Value: Sendable>(
        _ work: SnapshotWork, gated: Bool, _ body: @escaping @Sendable (TwoDiskStorage) throws -> Value
    ) async -> SnapshotResult<Value> {
        if gated {
            if let refusal = snapshotGate { return .refused(refusal) }
        } else {
            await settleGuestFiles()
            if storageBusy { return .refused(.busy) }
        }
        let storage = self.storage
        let working = Task.detached(priority: .userInitiated) { () -> SnapshotResult<Value> in
            do {
                return .done(try body(storage))
            } catch let unfinished as SnapshotRestoreUnfinished {
                // The data is in place and the intent stands: the caller is told as a value, and the cause is kept.
                return .failed(unfinished)
            } catch let refusal as SnapshotRefusal {
                return .refused(refusal)
            } catch {
                return .failed(error)
            }
        }
        // The operation clears itself before it ends, so what waits for it
        // never sees it set once it has finished.
        let operation = Task { [weak self] in
            _ = await working.value
            self?.snapshotWork = nil
            self?.snapshotRevision += 1
            self?.snapshotOperation = nil
        }
        snapshotWork = work
        snapshotOperation = operation
        var result = await working.value
        await operation.value
        if case .failed(let error) = result, let unfinished = error as? SnapshotRestoreUnfinished {
            rollForwardFailure = SnapshotFailure(unfinished.cause)
            result = .refused(.restoreUnfinished)
        } else if work == .restore, result.value != nil {
            rollForwardFailure = nil
        }
        // A restore or a delete changes what the device has free.
        model?.host.freeStorageBytes = AppEntry.freeStorageBytes()
        return result
    }
}
