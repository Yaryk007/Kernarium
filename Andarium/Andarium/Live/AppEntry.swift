// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Metal
import os
import TipKit
import UIKit

/// The device application's entry: the guest on disk, this device and the
/// system Andarium carries, and what a harness launch asks for in the
/// environment (`Andarium/README.md#automation`).
struct AppEntry {
    var overrides: LaunchOverrides
    /// Whether this launch is the person's own, the only kind that announces
    /// the release notes with the ribbon and keeps their record. A launch
    /// that builds an entry without `fromArguments`, as a test does, is not.
    var announcesWhatsNew = false
    /// The build this launch runs (`CFBundleVersion`); nil when the bundle
    /// states none as a number.
    var build = AppEntry.bundleBuild()
    /// Where the release notes' record is kept.
    var defaults = UserDefaults.standard

    /// The live application opens no screen directly.
    var hints: ScreenHints { ScreenHints() }

    static func fromArguments() -> AppEntry {
        let info = ProcessInfo.processInfo
        let overrides = LaunchOverrides(
            environment: info.environment,
            processorCount: GuestProcessorLimit.vCPUs(physicalProcessorCount: info.activeProcessorCount)
        )
        return AppEntry(
            overrides: overrides,
            announcesWhatsNew: Self.isPersonsLaunch(overrides: overrides, environment: info.environment)
        )
    }

    /// The adb key of the guest this launch runs.
    var adbKeyStore: AdbKeyStore {
        return AdbKeyStore(instance: overrides.instance)
    }

    func configureTips() {
        // The input probe's UI tests tap the side keys and Settings; a tip's
        // popover over them is no part of what they check, but for the one
        // that checks the first tap on the keys with the tip up.
        let environment = ProcessInfo.processInfo.environment
        if InputProbeLaunch.showsTip(in: environment) {
            try? Tips.resetDatastore()
            Tips.showTipsForTesting([SideKeysTip.self])
        } else if InputProbeLaunch.requested(in: environment) {
            Tips.hideAllTipsForTesting()
        }
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    /// The model for this launch. A copy of Andarium without its system, or
    /// with one it cannot read, still opens, and says so.
    @MainActor
    /// `freeSpace` is the space a run watches: the application's own reading
    /// (`freeStorageBytes`) unless a test supplies another.
    /// `environment` is the launch's unless a test supplies another.
    func makeModel(
        freeSpace: @escaping @Sendable () -> UInt64 = { AppEntry.freeStorageBytes() },
        environment: [String: String] = ProcessInfo.processInfo.environment
    ) -> AppModel {
        let launch = overrides
        let bundle = Bundle.main
        let version = Self.appVersion(bundle)
        let systemFolder = SystemInstallation.installed
            ?? bundle.resourceURL?.appendingPathComponent(SystemFolder.name)
        let host = Self.hostDevice()
        // A UI test of the hardware keyboard and mouse opens the session with
        // no guest behind it.
        if InputProbeLaunch.requested(in: environment) {
            return Self.makeInputProbeModel(host: host, version: version, environment: environment)
        }

        var problem: String?
        var system = SystemInfo(
            version: "", release: "unknown", securityPatch: nil,
            kernel: "", architecture: "", appVersion: version
        )
        var carried: SystemFolder.Release?
        if let systemFolder {
            do {
                let manifest = try Data(contentsOf: systemFolder.appendingPathComponent(SystemFolder.manifest))
                system = try SystemInfo(manifest: manifest, appVersion: version)
                carried = try SystemFolder.release(in: systemFolder)
            } catch {
                problem = String(localized: "This copy of Andarium does not carry a usable system: \(error.localizedDescription)")
            }
        } else {
            problem = String(localized: "This copy of Andarium does not carry a system.")
        }

        let selectedSystem = systemFolder.flatMap(SystemInstallation.selection(in:))
        let locations = GuestLocations(directory: Self.guestDirectory(
            instance: launch.instance, system: selectedSystem
        ))
        // A custom or test guest keeps its own list of Android apps beside its disks.
        let isCustomSystem = selectedSystem.map { $0 != .lineage } ?? false
        if launch.instance != nil || isCustomSystem {
            AndroidAppsHub.shared.configure(directory: locations.directory.appendingPathComponent("AndroidApps", isDirectory: true))
        }
        let storage = TwoDiskStorage(
            locations: locations,
            release: carried?.identity ?? ReleaseIdentity(
                releaseID: "unavailable", buildFingerprint: "unavailable", buildNumber: BuildNumber.oldest, securityPatch: "",
                systemDiskSHA256: String(repeating: "0", count: 64), dataLayout: 1, minimumGiB: 4, defaultGiB: 8,
                maximumGiB: 64
            ),
            releaseDirectory: systemFolder ?? locations.directory
        )
        // What the last process left half done in the snapshots (a restore, a
        // create, a delete) is completed before the guest is read.
        var snapshotRecoveryFailure: (any Error)?
        do {
            try storage.recoverSnapshots()
        } catch {
            snapshotRecoveryFailure = error
            print("Andarium: the snapshots could not be recovered: \(error.localizedDescription)")
        }
        var guest: Guest?
        do {
            guest = try storage.load()
        } catch {
            problem = problem ?? String(localized: "The virtual machine\u{2019}s settings could not be read: \(error.localizedDescription)")
        }
        // A stored memory size above the bound of its vCPU count comes down to
        // it; a size the launch states stays as stated.
        guest = guest.map {
            launch.applied(to: $0, on: host, capacityMaximumGiB: storage.release.maximumGiB)
        }

        let model = AppModel(
            guest: guest, host: host, system: system, capabilities: Capabilities.device,
            createRequirementBytes: storage.createRequirementBytes
        )
        // An iPhone's guest follows the device's orientation; a harness launch
        // may state it for another device.
        model.followsDeviceOrientation = launch.followsDeviceOrientation
            ?? (UIDevice.current.userInterfaceIdiom == .phone)
        #if !targetEnvironment(simulator)
        // A build on a device, Debug or Release, can run the guest on the JIT
        // engine when a debugger prepares its memory.
        model.jitAvailable = true
        #endif
        // The rulers offer no size the carried system's data disk would refuse.
        model.releaseMaximumGiB = storage.release.maximumGiB
        // The resolution is the person's choice where they made one (a Retina
        // mode an earlier version stored is none), else the choice nearest 1.5
        // where the system holds this application to 4 GiB or less and the one
        // nearest 2.0 where it does not. A value a launch states is that run's,
        // and what the person chose stays stored.
        model.chosenScale = DisplayOptionsStore.load().chosenScale
        model.launchScale = launch.displayScale
        model.memoryLimitBytes = MemoryLimit.bytes()
        // The statistics show from the start where the person left them,
        // unless the launch is a harness's, which decides for its run alone
        // (`StatsPreference.launch`).
        let stats = StatsPreference().launch(for: launch)
        model.session.showStats = stats.showOnScreen
        model.statsDecidedByLaunch = !stats.keepsChanges
        // "Use the pointer as touch" is the person's last choice, unless the
        // launch is a harness's and decides for its run alone.
        let pointer = PointerPreference().launch(automated: !launch.isEmpty)
        model.pointerAsTouch = pointer.isOn
        model.pointerAsTouchDecidedByLaunch = !pointer.keepsChanges
        // Settings' "When hot" is the person's last choice, unless the launch
        // is a harness's and decides for its run alone (`HeatLimitPreference`).
        let heat = HeatLimitPreference().launch(
            automated: !launch.isEmpty || ThermalOverride.stated(in: environment) != nil,
            stated: HeatLimitLaunch.stated(in: environment))
        model.heatLimitOn = heat.isOn
        model.heatLimitDecidedByLaunch = !heat.keepsChanges
        // Home's "120 Hz" is the person's last choice, off at first, unless the
        // launch is a harness's and decides for its run alone
        // (`HighRefreshPreference`).
        let refresh = HighRefreshPreference().launch(
            automated: !launch.isEmpty || ThermalOverride.stated(in: environment) != nil,
            stated: HighRefreshLaunch.stated(in: environment))
        model.highRefreshOn = refresh.isOn
        model.highRefreshDecidedByLaunch = !refresh.keepsChanges
        // LineageOS presents itself as this iPhone, and can draw beside its
        // sensor housing; a harness launch keeps the image's own identity.
        if launch.isEmpty, selectedSystem == .lineage || selectedSystem == .lineageGApps {
            model.phoneHardware = PhoneHardware(machine: IPadAppHost.hardwareMachine())
        }
        model.notchSpaceOn = launch.isEmpty && NotchSpacePreference().isOn
        model.notchSpaceDecidedByLaunch = !launch.isEmpty
        // Home's AOT row exists where the build carries the translated-code
        // framework (what Acknowledgements reads), and is the person's last
        // choice unless the launch is a harness's, whichever way it is told.
        model.translatedLibrariesAvailable = !isCustomSystem && Acknowledgements.carries(Acknowledgements.aotFramework)
        let translated = Self.translatedLibrariesLaunch(overrides: launch, environment: environment)
        model.translatedLibrariesOn = translated.isOn
        model.translatedLibrariesDecidedByLaunch = !translated.keepsChanges
        model.printsStatistics = launch.statistics == true
        // The JIT row follows the person's last choice, unless the launch is a
        // harness's and decides for its run alone (`JitPreference.launch`).
        let jit = JitPreference().launch(
            stated: JitPreference.stated(in: ProcessInfo.processInfo.environment),
            hasOtherSettings: !launch.isEmpty)
        model.jitOn = jit.isOn
        model.jitDecidedByLaunch = !jit.keepsChanges
        // A harness launch can keep the device's haptics silent for its run
        // (`HapticsLaunch`); the person's switch stays as stored.
        model.hapticsSilencedByLaunch = HapticsLaunch.silences(in: environment)

        let markerDirectory = launch.instance == nil ? Self.supportDirectory() : locations.directory
        let marker = PauseMarker(url: markerDirectory.appendingPathComponent("pause.json"))
        let closedInBackground = marker.consume()
        if closedInBackground, guest != nil {
            model.notice = .closedInBackground
            print("Andarium: the previous process ended out of the foreground; the guest starts again from boot")
        }
        // The session records and MetricKit's reports live beside the marker.
        let sessionRecord = markerDirectory.appendingPathComponent(BugReportAttachments.sessionRecord)
        model.adoptDiagnostics(
            in: markerDirectory, containers: Self.containerPaths(), endedOutOfForeground: closedInBackground)
        ExitReports.shared.start(keepingIn: markerDirectory)
        if carried != nil {
            // A harness launch of an isolated instance may state the free space.
            let watched = launch.instance == nil
                ? freeSpace : Self.harnessFreeSpace(in: locations.directory, fallback: freeSpace)
            if launch.instance != nil {
                Self.answerSpaceScreenFromHarness(model: model, directory: locations.directory)
            }
            // The colour data's folder: the boot reads it and the planner asks the runtime
            // about it, and the planner's figure is the boot's only while they are one value.
            let calibration = markerDirectory.appendingPathComponent("Camera", isDirectory: true)
            let runtime = LiveRuntime(
                model: model, storage: storage, marker: marker, keyStore: adbKeyStore,
                sessionRecord: sessionRecord,
                cameraDirectory: calibration,
                freeSpace: watched)
            // Root is offered only by a release that holds it; with none the
            // row is not shown and a start boots the release's own ramdisk.
            runtime.rootRelease = carried?.root
            model.rootAvailable = carried?.root != nil
            // An installed system can be given the root files it came without;
            // one in the application's bundle cannot be written.
            if launch.isEmpty, carried != nil, carried?.root == nil, let installed = SystemInstallation.installed {
                model.rootSetup = { [weak model, weak runtime] phase in
                    let root = try await MagiskInstaller.install(in: installed) { progress in
                        switch progress {
                        case .working(let label): phase(label)
                        case .downloading(let label, let received, let total, _):
                            guard let total, total > 0 else { return phase(label) }
                            phase("\(label) \(Int(Double(received) / Double(total) * 100))%")
                        }
                    }
                    runtime?.rootRelease = root
                    model?.rootAvailable = true
                }
            }
            model.driver = runtime
            // A start with root that the last session's record shows ended
            // before Android booted was found before there was a driver.
            model.saveRootStateIfPending()
            model.snapshotDriver = runtime
            model.cameraReservePlanner = { policy in
                LiveRuntime.cameraReservePlan(for: policy, calibration: calibration)
            }
            // The listing says why a restore that began last time is still not finished.
            if let snapshotRecoveryFailure { runtime.snapshotRecoveryFailed(snapshotRecoveryFailure) }
            model.fileBrowser = FileBrowserModel(backend: runtime.makeGuestFiles(hostFree: watched))
            model.refreshPlan()
            // Access the person took away while Andarium was closed takes the
            // Microphone choice with it.
            model.microphone?.refresh()
            model.location?.refresh()
            model.camera?.refresh()
            // A debugger attached at launch (StikDebug) is asked for what the
            // JIT row says at once: it spins until it is served, and the
            // system ends it within a minute. A guest the launch is about to
            // make is the one the memory is prepared for.
            let offered = Self.launchGuest(launch, on: model)
            model.prepareJitAtLaunch(
                standard: JitShape(memoryMiB: offered.memoryMiB, processorCount: offered.cores),
                replacingGuest: launch.erase)
        }
        announceWhatsNew(
            on: model, hasGuest: guest != nil, vCPUs: guest?.cores ?? host.processorCount)
        model.appUpdate = makeAppUpdate(on: model)
        model.alert = problem.map { AppAlert(title: String(localized: "Andarium is incomplete"), message: $0) }
        return model
    }

    /// The model of a UI test's launch of the hardware keyboard and mouse
    /// (`InputProbeLaunch`): a guest that exists only in memory and a session
    /// that is open as if its run had booted, so the display takes the keys
    /// and the pointer. The guest's keyboard and mouse send to the probe's log
    /// instead of the runtime, and nothing is read from or written to the
    /// application's own guest or settings.
    @MainActor
    private static func makeInputProbeModel(
        host: HostDevice, version: String, environment: [String: String]
    ) -> AppModel {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("InputProbe", isDirectory: true)
        let locations = GuestLocations(directory: directory)
        let storage = TwoDiskStorage(
            locations: locations,
            release: ReleaseIdentity(
                releaseID: "probe", buildFingerprint: "probe", buildNumber: BuildNumber.oldest, securityPatch: "",
                systemDiskSHA256: String(repeating: "0", count: 64), dataLayout: 1, minimumGiB: 4, defaultGiB: 8,
                maximumGiB: 64
            ),
            releaseDirectory: directory
        )
        let system = SystemInfo(
            version: "", release: "probe", securityPatch: nil, kernel: "", architecture: "", appVersion: version)
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: Capabilities.device,
            createRequirementBytes: 0
        )
        // The launch decides the setting for its own run.
        model.pointerAsTouch = PointerPreference().launch(automated: true).isOn
        model.pointerAsTouchDecidedByLaunch = true
        let heat = HeatLimitPreference().launch(automated: true, stated: HeatLimitLaunch.stated(in: environment))
        model.heatLimitOn = heat.isOn
        model.heatLimitDecidedByLaunch = true
        model.highRefreshOn = HighRefreshPreference().launch(
            automated: true, stated: HighRefreshLaunch.stated(in: environment)).isOn
        model.highRefreshDecidedByLaunch = true
        model.translatedLibrariesDecidedByLaunch = true
        // The side keys play their feedback here too: a launch that keeps the
        // phone silent does so on this path as on the ordinary one.
        model.hapticsSilencedByLaunch = HapticsLaunch.silences(in: environment)
        let probe = InputProbe()
        model.inputProbe = probe
        // A probe that states iOS's thermal state runs the thermal session
        // against a stand-in for the guest's HAL, which says what Android
        // would be told in the probe's log.
        let thermalStated = ThermalOverride.stated(in: environment) != nil
        let hal = thermalStated
            ? ThermalProbeHal { [weak probe] severity in
                DispatchQueue.main.async { MainActor.assumeIsolated { probe?.record("heat told \(severity)") } }
            } : nil
        let runtime = LiveRuntime(
            model: model, storage: storage, marker: PauseMarker(url: directory.appendingPathComponent("pause.json")),
            keyboardMouse: KeyboardMouse.probe(
                probe, supportsMouse: host.supportsPointer,
                guestRegisters: InputProbeLaunch.guestRegisters(in: environment)
            ) { [weak model] wanted in model?.pointerLockWanted = wanted },
            thermalOpener: hal)
        model.driver = runtime
        model.phase = .running(.normal)
        runtime.beginInputProbe()
        if thermalStated { runtime.beginThermalProbe() }
        if let script = OldAppsProbeLaunch.script(in: environment) {
            let scripted = ScriptedOldApps(open: ["com.example.rally"])
            switch script {
            case .apps: break
            case .refuse: scripted.writeAnswer = .failed
            case .none: scripted.listingAnswer = .unavailable
            }
            model.scriptedOldApps = scripted
            // What the first listing of a run does once Android's storage is up.
            Task { await model.refreshOldApps() }
        }
        if AndroidAppsProbeLaunch.requested(in: environment) {
            model.scriptedAndroidApps = ScriptedAndroidApps()
            // What the first listing of a run does once Android's storage is up.
            Task { await model.refreshAndroidApps() }
        }
        return model
    }

    /// An automation launch creates the guest if there is none, erases it
    /// when asked, and starts it; a normal launch stops at Home.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        let launch = overrides
        guard launch.start != nil || launch.erase else { return }
        if model.guest == nil {
            model.create(Self.launchGuest(launch, on: model))
            guard await Self.idle(model) else { return }
        }
        if launch.erase {
            model.erase()
            guard await Self.idle(model) else { return }
            // Erasing leaves no guest; a launch that starts one sets up a new
            // guest first.
            if launch.start != nil, model.guest == nil {
                model.create(Self.launchGuest(launch, on: model))
                guard await Self.idle(model) else { return }
            }
        }
        if let mode = launch.start, model.alert == nil {
            model.start(mode)
        }
    }

    /// A new guest as the launch asks for it, within the carried system's data
    /// disk.
    @MainActor
    private static func launchGuest(_ launch: LaunchOverrides, on model: AppModel) -> Guest {
        launch.applied(
            to: Guest.fresh(on: model.host), on: model.host, capacityMaximumGiB: model.releaseMaximumGiB
        )
    }

    /// Waits for the busy overlay to go, within a bound; false if it did not.
    @MainActor
    private static func idle(_ model: AppModel) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(900)
        while model.busy != nil {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(200))
        }
        return model.alert == nil
    }

    // MARK: Device

    /// The build number, which orders the releases whose notes the ribbon
    /// announces.
    static func bundleBuild(_ bundle: Bundle = .main) -> Int? {
        (bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String).flatMap { Int($0) }
    }

    private static func appVersion(_ bundle: Bundle) -> String {
        let short = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0"
        let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "1"
        return "\(short) (\(build))"
    }

    /// `Library/Guest/`. The disk's path goes to Linux in a kernel argument
    /// that cannot carry a space, so the guest does not live in `Application
    /// Support`; the library directory's name is safe on a device and on a Mac.
    /// A harness launch's isolated `instance` lives beside it.
    static func guestDirectory(instance: String? = nil, system: GuestSystemChoice? = nil) -> URL {
        let library = (try? FileManager.default.url(
            for: .libraryDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
        if let instance {
            return library.appendingPathComponent("TestGuests", isDirectory: true)
                .appendingPathComponent(instance, isDirectory: true)
        }
        if let system, system != .lineage {
            return library.appendingPathComponent("CustomGuests", isDirectory: true)
                .appendingPathComponent(system.storageName, isDirectory: true)
        }
        return library.appendingPathComponent("Guest", isDirectory: true)
    }

    /// `Library/Application Support/`, for the application's own small files.
    /// The paths that name this install of Andarium, which leave the device
    /// in no bug report.
    static func containerPaths() -> [String] {
        [
            NSHomeDirectory(),
            Bundle.main.bundleURL.deletingLastPathComponent().path,
            FileManager.default.temporaryDirectory.path,
        ]
    }

    static func supportDirectory() -> URL {
        (try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
    }

    @MainActor
    static func hostDevice(camera: CameraPlatform? = nil) -> HostDevice {
        let info = ProcessInfo.processInfo
        let name: String
        let idiom: HostDevice.Idiom
        if info.isiOSAppOnMac {
            name = "Mac"
            idiom = .mac
        } else if UIDevice.current.userInterfaceIdiom == .pad {
            name = "iPad"
            idiom = .pad
        } else {
            name = "iPhone"
            idiom = .phone
        }
        // A guest has every processor, at most the count the memory budget is
        // measured for; the memory policy has a row for each of those counts.
        let limit = GuestProcessorLimit.vCPUs(physicalProcessorCount: info.activeProcessorCount)
        return HostDevice(
            name: name, processorCount: limit,
            memory: memoryPolicy(processorCount: limit),
            freeStorageBytes: freeStorageBytes(),
            runsGuestGraphics: drawsGuestGraphics(),
            supportsPointer: UIDevice.current.userInterfaceIdiom != .phone,
            idiom: idiom, hasHaptics: DeviceHaptics.isAvailable,
            hasCompass: MotionSensors().compassAvailable, hasCamera: (camera ?? .system).hasCamera
        )
    }

    /// The guest is drawn with Metal 4 (the A14 and later). The Simulator
    /// draws it with the CPU and has no Metal 4 device, so it always can.
    static func drawsGuestGraphics() -> Bool {
        #if targetEnvironment(simulator)
        return true
        #else
        return MTLCreateSystemDefaultDevice()?.supportsFamily(.metal4) ?? false
        #endif
    }

    /// The most guest RAM at each vCPU count, the default a new guest gets, the
    /// size above which iOS is likely to end the application and the host
    /// GPU's budget a guest would launch with: the runtime's own budget
    /// formula, answered by the loader as it is for Kernarium, against the
    /// memory limit the system sets this process (none on a Mac). The maximum
    /// counts none of the launch allowances; the others count the conservative
    /// launch set the host GPU's budget uses too.
    static func memoryPolicy(processorCount: Int) -> MemoryPolicy {
        let maximumSet = kernarium_runtime_loader_maximum_memory_set()
        let launchSet = kernarium_runtime_loader_launch_memory_set()
        return MemoryPolicy(
            processorCount: processorCount,
            bound: { count in
                Int(kernarium_runtime_loader_maximum_guest_memory_mib(UInt32(count), maximumSet))
            },
            defaultMemory: { count in
                Int(kernarium_runtime_loader_default_guest_memory_mib(UInt32(count), launchSet))
            },
            safeMemory: { count in
                Int(kernarium_runtime_loader_conservative_guest_memory_mib(UInt32(count), launchSet))
            },
            defaultHoldsGraphics: { count in
                kernarium_runtime_loader_default_guest_memory_holds_gpu_budget(UInt32(count), launchSet) != 0
            },
            graphicsMemory: { memoryMiB, count in
                Int(kernarium_runtime_loader_gpu_budget_mib(UInt32(memoryMiB), UInt32(count), launchSet))
            },
            cameraReserve: { budgetMiB in
                Int(kernarium_runtime_loader_camera_reserve_mib(UInt32(clamping: budgetMiB)))
            }
        )
    }

    /// The space a write can use on the device, the one figure the run's
    /// watch, the start checks and every screen use. It touches no state of
    /// the entry, so a `@Sendable` source can hold it.
    nonisolated static func freeStorageBytes() -> UInt64 {
        HostStorage.freeBytes()
    }
}

/// What the system holds this application's memory to: the footprint plus what
/// the system lets it grow by, which is the limit iOS enforces (jetsam) and
/// 2,867 MiB on an iPad mini 6. Zero where the system sets none (a Mac, or an
/// iOS application on one). The same figure as `uml::platform::
/// os_memory_limit_bytes` (`darwin/platform/apple/os_memory_limit.hpp`), which
/// the runtime holds the guest's memory to; this application does not link
/// that header.
enum MemoryLimit {
    static func bytes() -> UInt64 {
        var info = task_vm_info_data_t()
        let words = MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size
        var count = mach_msg_type_number_t(words)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: words) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS, Int(count) >= words, info.limit_bytes_remaining != 0 else { return 0 }
        return info.phys_footprint + info.limit_bytes_remaining
    }
}

/// The display option, kept across launches.
enum DisplayOptionsStore {
    private static let key = DisplayOptions.storageKey

    static func load(from defaults: UserDefaults = .standard) -> DisplayOptions {
        guard let data = defaults.data(forKey: key),
              let options = try? JSONDecoder().decode(DisplayOptions.self, from: data)
        else { return DisplayOptions() }
        return options
    }

    static func save(_ options: DisplayOptions, to defaults: UserDefaults = .standard) {
        defaults.set(try? JSONEncoder().encode(options), forKey: key)
    }
}

/// An installed Android release, or the equivalent directory carried by an
/// older build. It holds `release-manifest.json`,
/// `boot/kernel-command-line.txt`, `guest/system.img` (the read-only system
/// disk) and `guest/partition-layout.json`. The runtime reads the directory
/// itself; the application reads the release's identity.
nonisolated enum SystemFolder {
    static let name = "andarium-system"
    static let manifest = "release-manifest.json"
    static let commandLine = "boot/kernel-command-line.txt"
    static let layout = "guest/partition-layout.json"
    static let systemDisk = "guest/system.img"

    enum Problem: LocalizedError, Equatable {
        case missing(String)
        case sizeDiffers(declared: UInt64, actual: UInt64)

        var errorDescription: String? {
            switch self {
            case .missing(let file): "\(file) is missing"
            case .sizeDiffers(let declared, let actual):
                "the system disk is \(actual) bytes, not the \(declared) its layout declares"
            }
        }
    }

    struct Release {
        var identity: ReleaseIdentity
        /// What the release holds for root (`RootRelease`), nil for none: the
        /// manifest's `root` object with both files in `root/`.
        var root: RootRelease?
    }

    static func release(in folder: URL) throws -> Release {
        let manager = FileManager.default
        for file in [manifest, commandLine, layout, systemDisk]
        where !manager.fileExists(atPath: folder.appendingPathComponent(file).path) {
            throw Problem.missing(file)
        }
        let layoutData = try Data(contentsOf: folder.appendingPathComponent(layout))
        let identity = try ReleaseIdentity(
            manifest: Data(contentsOf: folder.appendingPathComponent(manifest)), layout: layoutData
        )
        guard let object = try JSONSerialization.jsonObject(with: layoutData) as? [String: Any],
              let system = object["system_disk"] as? [String: Any],
              let declared = (system["size_bytes"] as? NSNumber)?.uint64Value, declared > 0
        else { throw Problem.missing("the system disk's size in \(layout)") }
        let actual = UInt64((try? folder.appendingPathComponent(systemDisk).resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        guard actual == declared else { throw Problem.sizeDiffers(declared: declared, actual: actual) }
        let manifestData = try Data(contentsOf: folder.appendingPathComponent(manifest))
        return Release(identity: identity, root: RootRelease.carried(in: folder, manifest: manifestData))
    }
}
