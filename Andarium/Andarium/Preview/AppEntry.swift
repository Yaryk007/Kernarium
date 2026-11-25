// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import TipKit
import UIKit

/// The Simulator preview's entry. `-screen <name>` opens any screen or state
/// directly with mock data, so a simulator driven only by `simctl` reaches
/// every flow. The arguments are the keys `fromArguments()` reads.
struct AppEntry {
    var screen: String
    /// Boot screens animate and continue into the session instead of holding.
    var play: Bool
    var lowSpace: Bool
    var resetTips: Bool
    /// The boot step a frozen boot screen shows, from 0; the screen's own
    /// when absent.
    var bootStage: Int?
    /// The Microphone switch asks the Simulator's own permission instead of
    /// taking the access as given, so `simctl privacy` and the system's
    /// alert drive it as they drive the device's.
    var systemMicrophone: Bool
    /// The Location switch asks the Simulator's own permission, as
    /// `systemMicrophone` does for the microphone: `simctl privacy` sets it
    /// and the system's alert asks.
    var systemLocation: Bool
    /// The Camera switch asks the Simulator's own permission, as
    /// `systemMicrophone` does for the microphone. The Simulator has no camera,
    /// so the preview takes it that the device has one: `simctl privacy` sets
    /// the access and the system's alert asks.
    var systemCamera: Bool
    /// How many CPU readings the stats overlay and Settings' Stats rows
    /// start with, and hold: 0 is a session before its first reading. Absent,
    /// the mock readings run on.
    var statsReadings: Int?
    /// Graphics memory in use, in MiB, held with `statsReadings`: a figure
    /// near the budget turns its bar orange.
    var statsVramMiB: Int?
    /// The mock device: "iphone-18-pro-max", "ipad-mini-6" or "mac-64-gib"
    /// (a ruler of about a thousand positions); absent, the idiom's own mock.
    var device: String?
    /// The guest's memory, in MiB, instead of the default a new guest gets.
    var memoryMiB: Int?
    /// "light" or "dark" draws the preview in that appearance whatever the
    /// Simulator's own, so a capture leaves the device's setting alone.
    var colorScheme: String?
    /// The resolution chosen, "native" or a number, instead of the default.
    var displayScale: String?
    /// The screen's native scale and the display's size in points, "402x778",
    /// instead of the Simulator's own, to show another device's choices.
    var displayNative: String?
    var displayPoints: String?
    /// What the system holds the application's memory to, in MiB, which the
    /// default resolution follows ("2867" is an iPad mini 6's); absent, no
    /// limit, as on a Mac.
    var memoryLimitMiB: Int?
    /// The model takes VoiceOver as running, which the Simulator never has,
    /// so a run shows the card that asks about Android's screen reader.
    var voiceOver: Bool
    /// The mock guest's screen reader: "on", "missing" for a system without
    /// one, "unreadable" for a guest whose answer never comes, "fails" for one
    /// that refuses a change, "slow" for a change that takes long and
    /// "transfer" for a file transfer that holds the channel; absent, the
    /// system carries it and it is off.
    var talkBack: String?
    /// What the preview does of its own once the mock guest has been read:
    /// "change" turns the screen reader on from Settings' switch, "card" from
    /// the card's Turn On, so a capture shows a change under way or failed.
    var talkBackAct: String?

    static func fromArguments() -> AppEntry {
        let defaults = UserDefaults.standard
        return AppEntry(
            screen: defaults.string(forKey: "screen") ?? "home",
            play: defaults.bool(forKey: "play"),
            lowSpace: defaults.bool(forKey: "lowSpace"),
            resetTips: defaults.bool(forKey: "resetTips"),
            bootStage: defaults.string(forKey: "bootStage").flatMap { Int($0) },
            systemMicrophone: defaults.bool(forKey: "systemMicrophone"),
            systemLocation: defaults.bool(forKey: "systemLocation"),
            systemCamera: defaults.bool(forKey: "systemCamera"),
            statsReadings: defaults.string(forKey: "statsReadings").flatMap { Int($0) },
            statsVramMiB: defaults.string(forKey: "statsVramMiB").flatMap { Int($0) },
            device: defaults.string(forKey: "device"),
            memoryMiB: defaults.string(forKey: "memoryMiB").flatMap { Int($0) },
            colorScheme: defaults.string(forKey: "colorScheme"),
            displayScale: defaults.string(forKey: "displayScale"),
            displayNative: defaults.string(forKey: "displayNative"),
            displayPoints: defaults.string(forKey: "displayPoints"),
            memoryLimitMiB: defaults.string(forKey: "memoryLimitMiB").flatMap { Int($0) },
            voiceOver: defaults.bool(forKey: "voiceOver"),
            talkBack: defaults.string(forKey: "talkBack"),
            talkBackAct: defaults.string(forKey: "talkBackAct")
        )
    }

    /// Where Home scrolls to for a screen that shows a lower section.
    private static let scrollTargets: [String: String] = [
        "home-options": "erase", "home-erase": "erase", "erase": "erase",
        "home-resolution": "resolution",
        "home-capabilities": "capabilities", "home-mic-on": "capabilities", "home-mic-denied": "capabilities",
        "home-haptics-off": "capabilities",
        "home-mic-alert": "capabilities",
        "home-location-on": "capabilities", "home-location-denied": "capabilities",
        "home-location-approximate": "capabilities", "home-location-restricted": "capabilities",
        "home-location-services-off": "capabilities", "home-location-alert": "capabilities",
        "home-location-approximate-alert": "capabilities", "home-location-restricted-alert": "capabilities",
        "home-location-services-off-alert": "capabilities", "home-location-ask": "capabilities",
        "home-camera-on": "capabilities", "home-camera-denied": "capabilities",
        "home-camera-restricted": "capabilities", "home-camera-unavailable": "capabilities",
        "home-camera-alert": "capabilities", "home-camera-restricted-alert": "capabilities",
        "home-camera-ask": "capabilities",
        "home-backup": "backup",
        "home-jit": "performance", "home-jit-no-debugger": "performance", "home-jit-fallback": "performance",
        "home-performance": "performance", "home-performance-on": "performance",
        "home-aot": "performance", "home-aot-off": "performance", "home-aot-jit": "performance",
        "home-aot-60hz": "performance",
    ]

    /// Where Settings scrolls to for a screen that shows a lower section.
    private static let settingsScrolls: [String: String] = [
        "settings-stats": "stats",
        "settings-text-message": "developer",
        "settings-accessibility": "accessibility",
        "settings-accessibility-left": "accessibility",
        "settings-performance": "stats", "settings-performance-menu": "stats", "settings-performance-off": "stats",
        "settings-performance-hot": "stats", "settings-performance-hot-off": "stats",
        "settings-performance-120": "stats",
    ]

    /// The page of Info a screen opens on.
    private static let infoPages: [String: InfoPage] = [
        "info-acknowledgements": .acknowledgements,
        "info-license": .license("mesa"),
        "info-license-gpl": .license("linux"),
        "info-acknowledgements-root": .acknowledgements,
        "info-license-magisk": .license("magisk"),
        "whatsnew-history": .whatsNew,
    ]

    var hints: ScreenHints {
        ScreenHints(
            scrollTarget: Self.scrollTargets[screen] ?? SnapshotPreview.scrollTargets[screen]
                ?? RootPreview.scrollTargets[screen],
            infoPage: Self.infoPages[screen]
        )
    }

    /// The side-keys tip. A screenshot of the tip shows it whatever the
    /// datastore says; other session screenshots leave it out.
    func configureTips() {
        if resetTips { try? Tips.resetDatastore() }
        // The legacy install, played, shows the tip as a real session may.
        if screen == "session-tip" || screen == "session-tip-left" || (screen == "install-legacy" && play) {
            Tips.showAllTipsForTesting()
        } else if screen.hasPrefix("session") || screen.hasPrefix("settings") || screen.hasPrefix("files")
            || screen.hasPrefix("recovery") || screen.hasPrefix("boot")
            || screen.hasPrefix("space-pause") || screen.hasPrefix("sms-") || screen.hasPrefix("add-home")
            || Self.operationScreens.contains(screen) {
            Tips.hideAllTipsForTesting()
        }
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    func makeModel() -> AppModel {
        CaptureState.begin(screen: screen)
        applyColorScheme()
        let lowSpace = lowSpace || ["create-low-space", "start-low-space", "home-low-space", "add-home-wait-space"].contains(screen)
        let host = WhatsNewPreview.host(for: screen) ?? SnapshotPreview.host(for: screen)
            ?? PreviewData.host(lowSpace: lowSpace, device: device)
        var guest: Guest? = PreviewData.sampleGuest(on: host)
        if let memoryMiB { guest?.memoryMiB = memoryMiB }
        if ["welcome", "welcome-menu", "create", "create-low-space"].contains(screen) { guest = nil }
        let model = AppModel(
            guest: guest, host: host, system: PreviewData.system, capabilities: .preview,
            createRequirementBytes: PreviewData.createRequirementBytes
        )
        applyDisplay(to: model)
        let files = MockGuestFiles()
        let browser = FileBrowserModel(backend: files)
        MockFolderProbe.browser = browser
        model.fileBrowser = browser
        let freeze = !play
        model.jitAvailable = true
        // The preview carries no translated-code framework, so Home has no AOT
        // row unless a screen plays a build that does (`home-aot`,
        // `home-aot-off`, `home-aot-jit`).
        model.translatedLibrariesAvailable = screen.hasPrefix("home-aot")
        // Only an iPhone has a Taptic Engine for the guest's haptics: an iPad's
        // preview, like the device, has no Haptics row.
        model.hapticsAvailable = host.hasHaptics
        // The mock device has a camera that iOS has not been asked about; a
        // screen states another access (and `-systemCamera` reads the
        // Simulator's own).
        model.cameraAccess = .notDetermined
        if voiceOver {
            model.voiceOverRunning = { true }
            // The Simulator's VoiceOver is off and reports no end of a sentence.
            model.runStartAnnouncer = { _, finished in finished(true) }
        }
        let mockReader = PreviewScreenReader()
        model.screenReaderStandIn = mockReader
        switch talkBack {
        case "on":
            mockReader.state.enabledServices = [ScreenReader.services[0]]
        case "missing":
            mockReader.state = ScreenReaderState(installedService: nil, enabledServices: [])
        case "unreadable": mockReader.fault = .unreadable
        case "fails": mockReader.fault = .refusesChange
        case "slow": mockReader.fault = .slow
        case "transfer": mockReader.fault = .transfer
        case "checking": mockReader.fault = .pending
        default:
            break
        }

        func boot(_ kind: BootKind, stage: Int, seconds: Int) {
            model.frozenBootStage = freeze ? (bootStage ?? stage) : nil
            model.frozenElapsed = seconds
            model.phase = .booting(kind)
        }

        /// A start that has run `seconds`: the model's own clock says so, so
        /// the notice appears as it does for a real one, and Stop (played)
        /// ends the run as a host stop does.
        func slowBoot(_ kind: BootKind = .normal, stage: Int, seconds: Int, stopping: Bool = false) {
            boot(kind, stage: stage, seconds: seconds)
            let now = model.clock()
            model.slowStart.begin(at: now - TimeInterval(seconds), kind: kind)
            _ = model.slowStart.update(at: now)
            if stopping { _ = model.slowStart.requestStop() }
        }

        /// A tap on Stop comes long after the screen's first frame; a dialog
        /// asked for in it is presented before the screen has put its
        /// appearance on the window, and takes the system's.
        func asksToStopOnceSettled() {
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(1600))
                model.confirmingStop = true
            }
        }

        switch screen {
        case "home-new":
            model.guest = Guest.fresh(on: host)
            model.plan = .firstStart
        case "home-grow":
            model.guest?.pendingCapacityGiB = 24
        case "resize":
            model.resizePreset = 24
        case "home-max":
            model.guest?.capacityGiB = Limits.capacityRangeGiB.upperBound
        case "home-update":
            model.plan = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-update-failed":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: true, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-update-failed-restore":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: false, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "home-blocked":
            model.plan = .blocked(.olderAndroid)
        case "home-closed":
            model.notice = .closedInBackground
        case "home-network-cellular":
            model.previewNetworkNotice(.cellular)
        case "home-network-wifi":
            model.previewNetworkNotice(.wifi)
        case "home-ended":
            // The card's own words, from the runtime's record of a kernel panic.
            if let notice = SessionRecord(text: """
                uml-session 1
                state ended
                end guest-panic
                status 6
                pid 1
                message Attempted to kill init! exitcode=0x0000000b

                """)?.notice(endedOutOfForeground: false) {
                model.notice = .sessionEnded(notice)
            }
        case "home-memory-warning", "system-memory-warning":
            model.guest = PreviewData.highMemoryGuest(on: host)
        case "home-options", "home-display", "home-capabilities", "home-backup", "system-edit",
             "home-location-ask", "home-camera-ask", "home-performance", "home-performance-on":
            break
        case "home-aot", "home-aot-60hz":
            break
        case "home-aot-off":
            model.translatedLibrariesOn = false
        case "home-aot-jit":
            // JIT is on and a debugger is attached, so the next start runs on it.
            model.jitOn = true
        case "home-jit":
            model.jitOn = true
        case "home-jit-no-debugger":
            model.jitOn = true
            model.previewJitDebugger = false
        case "home-jit-fallback":
            model.jitOn = true
            model.previewJitDebugger = false
            model.jitLastStart = .noDebugger
        case "home-mic-on":
            model.guest?.microphone = true
            model.microphoneState = .granted
        case "home-mic-denied", "home-mic-alert":
            model.microphoneState = .denied
        case "home-haptics-off":
            model.guest?.haptics = false
        case "home-location-on":
            model.guest?.location = true
            model.locationAuthorization = .precise
        case "home-location-approximate", "home-location-approximate-alert":
            model.guest?.location = true
            model.locationAuthorization = .approximate
        case "home-location-denied", "home-location-alert":
            model.locationAuthorization = .denied
        case "home-location-restricted", "home-location-restricted-alert":
            model.locationAuthorization = .restricted
        case "home-location-services-off", "home-location-services-off-alert":
            model.locationAuthorization = .servicesOff
        case "home-camera-on", "system-edit-camera":
            model.guest?.camera = true
            model.cameraAccess = .authorized
        case "home-camera-denied", "home-camera-alert":
            model.cameraAccess = .denied
        case "home-camera-restricted", "home-camera-restricted-alert":
            model.cameraAccess = .restricted
        case "home-camera-unavailable":
            model.cameraAccess = .unavailable
        case "busy-create":
            model.busy = BusyState(title: String(localized: "Creating virtual machine"), detail: String(localized: "Setting up Andarium with \(Formats.size(gib: 16))."), progress: 0.42)
        case "busy-erase":
            model.busy = BusyState(title: String(localized: "Erasing virtual machine"), detail: String(localized: "Deleting everything the virtual machine stored."), progress: 0.65)
        case "boot":
            // The start applied the growth as it began.
            model.guest?.setCapacity(24)
            model.lifecycle.grewFromGiB = model.guest?.applyPendingGrowth()
            boot(.normal, stage: 3, seconds: 9)
        case "boot-single-vcpu":
            model.guest?.cores = 1
            boot(.normal, stage: 3, seconds: 9)
        case "boot-first":
            model.guest = model.guest?.erased
            model.plan = .firstStart
            boot(.firstStart, stage: 3, seconds: 74)
        case "boot-upgrade":
            model.bootFrom = PreviewData.previousSystemRelease
            boot(.upgrade, stage: 3, seconds: 51)
        case "boot-recovery":
            boot(.recovery, stage: 3, seconds: 7)
        case "boot-restart":
            boot(.restart, stage: 3, seconds: 8)
        case "boot-slow":
            slowBoot(stage: 3, seconds: 197)
        case "boot-slow-stopping":
            slowBoot(stage: 3, seconds: 203, stopping: true)
        case "boot-slow-first":
            // A first start eight minutes in: the later notice, with a plain title.
            model.guest = model.guest?.erased
            model.plan = .firstStart
            slowBoot(.firstStart, stage: 3, seconds: 497)
        case "boot-slow-upgrade":
            model.bootFrom = PreviewData.previousSystemRelease
            slowBoot(.upgrade, stage: 3, seconds: 491)
        case "boot-slow-recovery":
            slowBoot(.recovery, stage: 3, seconds: 197)
        case "boot-slow-live":
            // The steps held, the clock running: the status line and the notice count one time.
            slowBoot(stage: 3, seconds: 197)
            model.previewClockRuns = true
        case "boot-slow-stop":
            // Stop chosen: the dialog that asks first.
            slowBoot(stage: 3, seconds: 197)
            asksToStopOnceSettled()
        case "boot-slow-recovery-stop":
            // The same dialog over Recovery's own, light screen.
            slowBoot(.recovery, stage: 3, seconds: 197)
            asksToStopOnceSettled()
        case "boot-slow-storage":
            // The same notice over an earlier step, as a start stuck on the disk shows it.
            slowBoot(stage: 2, seconds: 184)
        case "boot-jit":
            model.jitOn = true
            model.preparingJit = true
            boot(.normal, stage: 0, seconds: 3)
        case "boot-jit-fallback":
            model.jitOn = true
            model.jitLastStart = .noDebugger
            boot(.normal, stage: 1, seconds: 4)
        case "session", "session-closed-left", "session-tip", "session-tip-left", "session-open", "session-keyboard", "session-stats",
             "session-power", "session-setup", "session-off", "session-left", "session-volume",
             "session-graphics-notice", "session-thermal-notice", "session-network-cellular", "session-network-wifi",
             "session-camera-notice", "session-camera-off-notice", "session-thermal-alert", "session-thermal-off":
            model.session.showStats = screen == "session-stats"
            model.session.sideKeysOpen = ["session-open", "session-off", "session-left", "session-volume"].contains(screen)
            model.sessionPreset = SessionPreset(
                volumePanel: screen == "session-volume",
                powerMenu: screen == "session-power",
                leftEdge: ["session-left", "session-closed-left", "session-volume", "session-tip-left"].contains(screen),
                guestScreen: {
                    switch screen {
                    case "session-setup": .setup
                    case "session-off": .off
                    case "session-keyboard": .search
                    default: .home
                    }
                }()
            )
            model.phase = .running(.normal)
            if screen == "session-graphics-notice" { model.previewGraphicsNotice() }
            if screen == "session-thermal-notice" || screen == "session-thermal-alert" { model.previewThermalNotice() }
            if screen == "session-network-cellular" { model.previewNetworkNotice(.cellular) }
            if screen == "session-network-wifi" { model.previewNetworkNotice(.wifi) }
            if screen == "session-camera-notice" { model.previewCameraNotice() }
            if screen == "session-camera-off-notice" { model.previewCameraNotice(.blockedInUse) }
        case "sms-bar", "sms-bar-typing", "sms-bar-near-limit", "sms-bar-sending", "sms-bar-sent",
             "sms-bar-not-confirmed", "sms-bar-delivered-earlier", "sms-bar-not-sent", "sms-bar-uncertain",
             "sms-bar-cut-off", "sms-bar-waiting", "sms-bar-bad-sender", "sms-bar-long-sender",
             "sms-bar-no-sender", "sms-bar-too-long", "sms-bar-unsendable", "sms-bar-paused":
            model.phase = .running(.normal)
            // `-androidScreen light` puts the bar over a light Android screen (the launcher's search).
            if UserDefaults.standard.string(forKey: "androidScreen") == "light" {
                model.sessionPreset = SessionPreset(guestScreen: .search)
            }
            model.previewTextMessage(screen)
        case "settings", "settings-medium", "settings-adb-off", "settings-adb-no-address", "settings-adb", "settings-stats",
             "settings-text-message", "settings-accessibility", "settings-accessibility-left",
             "settings-performance", "settings-performance-menu", "settings-performance-off",
             "settings-performance-hot", "settings-performance-hot-off", "settings-performance-120",
             "install-apk", "install-parts", "install-legacy", "install-done", "install-failed", "install-not-apk",
             "install-app-bundle", "bug-report", "install-data", "install-done-without-data", "install-done-data-not-placed", "install-done-container",
             "install-encrypted", "install-damaged-or-encrypted", "install-no-app", "install-damaged", "install-missing-split",
             "install-part-too-large", "install-legacy-hint", "install-legacy-hint-data",
             "settings-old-apps", "settings-old-apps-page", "settings-old-apps-turn-on", "settings-old-apps-turn-on-open",
             "settings-old-apps-turn-off", "settings-old-apps-refused", "settings-old-apps-empty",
             "settings-old-apps-failed", "settings-old-apps-loading":
            let session = model.session
            switch screen {
            case "settings-adb":
                session.adb = true
                session.adbAddress = SessionState.previewAddress
            case "settings-adb-no-address":
                session.adb = true
            case "settings-adb-off":
                session.usbDebugging = false
            default:
                break
            }
            Self.playOldApps(screen: screen, in: model, play: play)
            // The guest operations play on the full-screen overlay with
            // Settings already gone.
            let operation = Self.operationScreens.contains(screen)
            model.sessionPreset = SessionPreset(
                settings: !operation,
                settingsLarge: Self.settingsScrolls.keys.contains(screen) || screen == "settings",
                settingsScroll: Self.settingsScrolls[screen],
                oldAppsPage: screen.hasPrefix("settings-old-apps-"),
                leftEdge: screen == "settings-accessibility-left"
            )
            model.phase = .running(.normal)
            // The real operation lasts a few seconds, too short for a
            // screenshot to catch: hold its overlay part way unless `-play`.
            // A result is what the real operation leaves up, so it always
            // shows as it is.
            if operation && (freeze || Self.resultScreens.contains(screen)) {
                model.busy = Self.operationOverlay(for: screen, model: model)
            }
        case "space-pause", "space-pause-short", "space-pause-shutting-down", "space-pause-pausing",
            "space-pause-running":
            model.phase = .running(.normal)
            model.spacePause = switch screen {
            case "space-pause-short": SpacePause(freeBytes: 1_200 * Limits.bytesPerMiB, stillShort: true)
            case "space-pause-shutting-down": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, working: .shuttingDown)
            case "space-pause-pausing": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .pausing)
            case "space-pause-running": SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .notPaused)
            default: SpacePause(freeBytes: 820 * Limits.bytesPerMiB)
            }
        case let name where name.hasPrefix("files"):
            MockGuestFiles.present(name, browser: browser, files: files, model: model)
        case "recovery", "recovery-open":
            model.session.sideKeysOpen = screen == "recovery-open"
            model.phase = .running(.recovery)
        case "failed":
            // The failure path a real run takes: a crash after 42 minutes.
            model.fail()
        case "failed-memory":
            // What a run ends with where the host would not map the guest's
            // memory: it never began. The size is the guest's own, as the
            // report's second line states it. The result is the runtime's
            // memory-refused one (-101); the screen shows the size.
            model.fail(
                RunEnd(
                    result: -101,
                    memoryRefusedMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound),
                uptimeSeconds: nil)
        case "stopped":
            // What Stop on the slow-start notice leaves: the run ended by the person.
            model.fail(RunEnd(result: -5, stoppedByUser: true), uptimeSeconds: 203)
        case let name where AddToHomePreview.handles(name):
            AddToHomePreview.prepare(screen: name, model: model, freeze: freeze)
        default:
            break
        }
        HeatLimitPreview.apply(screen: screen, to: model)
        WhatsNewPreview.apply(screen: screen, to: model)
        UpdatePreview.apply(screen: screen, to: model)
        RootPreview.apply(screen: screen, to: model, freeze: freeze)
        if let statsReadings {
            model.session.readout = Readout(
                previewReadings: statsReadings, memoryMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound
            )
            if let statsVramMiB { model.session.readout.vramUsedBytes = UInt64(statsVramMiB) * Limits.bytesPerMiB }
        }
        if systemMicrophone { useSystemMicrophone(in: model) }
        if systemLocation { useSystemLocation(in: model) }
        if systemCamera { useSystemCamera(in: model) }
        SnapshotPreview.apply(screen: screen, to: model)
        // A run in the preview reads the mock guest's screen reader as a run
        // does; under VoiceOver the card follows.
        if case .running = model.phase { model.screenReaderBootFinished() }
        return model
    }

    /// Old app compatibility's screens: the image's list the preview plays, and
    /// the state the page starts in.
    @MainActor
    private static func playOldApps(screen: String, in model: AppModel, play: Bool) {
        guard screen.hasPrefix("settings-old-apps") else { return }
        let scripted = ScriptedOldApps(open: ["com.example.rally"])
        model.scriptedOldApps = scripted
        let state = model.session.oldApps
        switch screen {
        case "settings-old-apps-empty":
            scripted.listingAnswer = .apps([])
            state.apply(.apps([]))
        case "settings-old-apps-failed":
            scripted.listingAnswer = .failed
            state.apply(.apps(ScriptedOldApps.sampleApps))
            state.apply(.failed)
        case "settings-old-apps-loading":
            // The list never comes: the page keeps saying it is looking.
            scripted.delay = .seconds(3600)
        case "settings-old-apps-refused":
            scripted.writeAnswer = .failed
            state.apply(.apps(ScriptedOldApps.sampleApps))
        default:
            state.apply(.apps(ScriptedOldApps.sampleApps))
        }
    }

    /// The screens that show a guest operation on the full-screen overlay.
    private static let operationScreens: Set<String> = [
        "bug-report", "install-apk", "install-parts", "install-legacy", "install-done", "install-failed", "install-not-apk",
        "install-app-bundle", "install-data", "install-done-without-data", "install-done-data-not-placed", "install-done-container", "install-encrypted",
        "install-damaged-or-encrypted", "install-no-app", "install-damaged", "install-missing-split", "install-part-too-large",
        "install-legacy-hint", "install-legacy-hint-data",
    ]
    /// The ones that show how an install ended.
    private static let resultScreens: Set<String> = [
        "install-done", "install-failed", "install-not-apk", "install-app-bundle", "install-done-without-data",
        "install-done-data-not-placed", "install-encrypted", "install-damaged-or-encrypted", "install-no-app", "install-damaged",
        "install-missing-split", "install-part-too-large", "install-legacy-hint", "install-legacy-hint-data",
    ]

    /// A guest operation's overlay as it looks part way, or at its end: what
    /// `createBugReport` and `installPicked` show, held still.
    private static func operationOverlay(for screen: String, model: AppModel) -> BusyState {
        let cancel = BusyCancel {}
        let dismiss = BusyCancel { model.busy = nil }
        let file = PreviewData.sampleApkName
        let container = PreviewData.sampleContainerName
        switch screen {
        case "bug-report":
            return BusyState(
                title: String(localized: "Creating bug report"),
                detail: String(localized: "The virtual machine is collecting its logs and state. This can take several minutes."),
                progress: 0.6, cancel: cancel
            )
        case "install-parts":
            // An app in parts, part way through being sent.
            return BusyState(
                title: String(localized: "Installing app"),
                detail: String(localized: "Sending part \(2) of \(3)\u{2026}"),
                progress: 0.45, cancel: cancel
            )
        case "install-data":
            // The app is in; its data files are being copied, and Cancel is back for the copy.
            return BusyState(
                title: String(localized: "Installing app"),
                detail: String(localized: "Copying data file \(1) of \(2)\u{2026}"),
                progress: 0.6, cancel: cancel
            )
        case "install-legacy-hint", "install-legacy-hint-data":
            // After Install anyway of an app built for an Android before 4.0 that
            // Old app compatibility lists: the card says where its switch is.
            let outcome: ApkInstallOutcome = screen == "install-legacy-hint" ? .installed : .installedWithoutData
            let words = outcome.words(fileName: "pixel-knights.apk", oldAppHint: true)
            return BusyState(
                title: words.title, detail: words.detail, result: BusyResult(style: words.style, dismiss: dismiss)
            )
        case "install-done-without-data", "install-done-data-not-placed", "install-done-container":
            // What the install's own card says for each of these endings.
            let outcome: ApkInstallOutcome = switch screen {
            case "install-done-without-data": .installedWithoutData
            case "install-done-data-not-placed": .installedDataNotPlaced
            default: .installed
            }
            let words = outcome.words(fileName: container)
            return BusyState(
                title: words.title, detail: words.detail, result: BusyResult(style: words.style, dismiss: dismiss)
            )
        case "install-encrypted", "install-damaged-or-encrypted", "install-no-app", "install-damaged", "install-part-too-large":
            let refusal: InstallRefusal = switch screen {
            case "install-encrypted": .encrypted
            case "install-damaged-or-encrypted": .damagedOrEncrypted
            case "install-no-app": .noApp
            case "install-part-too-large": .partTooLarge
            default: .damaged
            }
            return BusyState(
                title: AppModel.installFailure, detail: refusal.message, result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-missing-split":
            return BusyState(
                title: AppModel.installFailure, detail: PackageInstallMessages.message(forCode: "FAILED_MISSING_SPLIT"),
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-legacy":
            // The first attempt is over and the question is up: the overlay
            // keeps its Cancel, which the alert covers.
            return BusyState(
                title: String(localized: "Installing app"),
                detail: "\(file) \u{00B7} \(Formats.size(bytes: PreviewData.sampleApkSizeBytes))",
                progress: 0.8, cancel: cancel
            )
        case "install-done":
            return BusyState(
                title: String(localized: "App installed"), detail: String(localized: "\(file) is installed in the virtual machine."),
                result: BusyResult(failed: false, dismiss: dismiss)
            )
        case "install-failed":
            return BusyState(
                title: AppModel.installFailure,
                detail: PackageInstallMessages.message(forCode: "FAILED_NO_MATCHING_ABIS"),
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-not-apk":
            return BusyState(
                title: AppModel.installFailure, detail: InstallRefusal.notApk.message,
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        case "install-app-bundle":
            return BusyState(
                title: AppModel.installFailure, detail: InstallRefusal.appBundle.message,
                result: BusyResult(failed: true, dismiss: dismiss)
            )
        default:
            return BusyState(
                title: String(localized: "Installing app"),
                detail: "\(file) \u{00B7} \(Formats.size(bytes: PreviewData.sampleApkSizeBytes))",
                progress: 0.45, cancel: cancel
            )
        }
    }

    /// Every window of the process takes `colorScheme`'s appearance as it
    /// becomes key, before its first frame.
    private func applyColorScheme() {
        let style: UIUserInterfaceStyle
        switch colorScheme {
        case "dark": style = .dark
        case "light": style = .light
        default: return
        }
        NotificationCenter.default.addObserver(
            forName: UIWindow.didBecomeKeyNotification, object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated {
                for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
                    for window in scene.windows { window.overrideUserInterfaceStyle = style }
                }
            }
        }
    }

    /// The device's microphone conversation against the Simulator's
    /// permission, which is read again whenever the app becomes active.
    private func useSystemMicrophone(in model: AppModel) {
        model.microphone = MicrophoneController(model: model, source: SimulatorMicrophoneSource()) { _ in }
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.microphone?.refresh() }
        }
    }

    /// The device's location conversation against the Simulator's
    /// permission, which is read again whenever the app becomes active.
    private func useSystemLocation(in model: AppModel) {
        model.location = LocationController(model: model, source: SystemLocationAccess()) {}
        model.location?.refresh()
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.location?.refresh() }
        }
    }

    /// The device's camera conversation against the Simulator's permission,
    /// which is read again whenever the app becomes active. The Simulator has
    /// no camera and the conversation does not need one.
    private func useSystemCamera(in model: AppModel) {
        model.camera = CameraController(model: model, source: SystemCameraSource(requiresCamera: false))
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.camera?.refresh() }
        }
    }

    /// The resolution the launch chooses, the memory limit and the screen it
    /// states: the preview shows a 2.3 screen on any Simulator.
    @MainActor
    private func applyDisplay(to model: AppModel) {
        model.chosenScale = displayScale.flatMap { GuestDisplayScale(text: $0) }
        model.memoryLimitBytes = memoryLimitMiB.map { UInt64(max($0, 0)) << 20 } ?? 0
        if let native = displayNative.flatMap({ Double($0) }) {
            model.statedNativeScale = native
            model.screenDidChange(nativeScale: native)
        }
        if let displayPoints {
            let parts = displayPoints.lowercased().split(separator: "x").compactMap { Double($0) }
            if parts.count == 2 { model.statedDisplayPoints = CGSize(width: parts[0], height: parts[1]) }
        }
    }

    /// Sheets and dialogs open after the first frame so SwiftUI presents them
    /// the way a tap would.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        try? await Task.sleep(for: .milliseconds(450))
        if let talkBackAct {
            for _ in 0..<40 where model.session.reader.state == nil {
                try? await Task.sleep(for: .milliseconds(100))
            }
            switch talkBackAct {
            case "change": model.setScreenReader(true)
            case "card": model.turnOnScreenReaderFromCard()
            default: break
            }
        }
        await AddToHomePreview.applyDeferred(screen: screen, model: model)
        UpdatePreview.applyDeferred(screen: screen, model: model)
        switch screen {
        case "create", "create-low-space":
            model.sheet = .create
        case "resize":
            model.sheet = .resize
        case "system-edit", "system-memory-warning":
            model.sheet = .system
        case "system-edit-camera":
            // The sheet shows what the runtime answers for the mock's cameras
            // when it opens, as the device's does after launch.
            let reserve = PreviewData.cameraReserveMiB(device: device)
            model.cameraReservePlanner = { policy in
                CameraReservePlan(policy: policy.withCameraReserve { _ in reserve }, exact: true)
            }
            await model.refreshCameraReserve().value
            model.sheet = .system
        case "resolution-sheet":
            model.sheet = .resolution
        case "home-mic-alert":
            model.setMicrophone(true)
        case "home-location-alert", "home-location-restricted-alert", "home-location-services-off-alert",
             "home-location-ask":
            model.setLocation(true)
        case "home-location-approximate-alert":
            model.permissionPrompt = .location
        case "home-camera-alert", "home-camera-restricted-alert", "home-camera-ask":
            model.setCamera(true)
        case "erase":
            model.confirmingErase = true
        case "info", "info-acknowledgements", "info-license", "info-license-gpl", "whatsnew-info",
             "whatsnew-history", "info-acknowledgements-root", "info-license-magisk":
            model.sheet = .info
        case "whatsnew-open", "whatsnew-skipped-open", "whatsnew-unsupported-open":
            model.sheet = .whatsNew
        case "start-low-space":
            model.start(.normal)
        case "files-review":
            try? await Task.sleep(for: .milliseconds(1100))
            model.fileBrowser?.reviewingSelection = true
        case "files-share":
            try? await Task.sleep(for: .milliseconds(1100))
            if let browser = model.fileBrowser, let transfer = browser.transfer { browser.share(transfer) }
        case "alert-error":
            model.alert = AppAlert(
                title: String(localized: "Settings not saved"),
                message: CocoaError(.fileWriteOutOfSpace).localizedDescription
            )
        case "bug-report" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.createBugReport()
        case "install-apk" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.installPicked(PreviewData.sampleApk())
        case "settings-old-apps-turn-on", "settings-old-apps-turn-on-open", "settings-old-apps-turn-off",
             "settings-old-apps-refused":
            // The page is up behind the alert, as a tap on a switch leaves it.
            try? await Task.sleep(for: .milliseconds(1400))
            let pixel = ScriptedOldApps.sampleApps[0]
            let rally = ScriptedOldApps.sampleApps[1]
            model.session.oldApps.prompt = switch screen {
            case "settings-old-apps-turn-on": .turnOn(pixel, open: false)
            case "settings-old-apps-turn-on-open": .turnOn(pixel, open: true)
            case "settings-old-apps-turn-off": .turnOff(rally)
            default: .notChanged
            }
        case "install-legacy" where play:
            // The whole install: Android takes its time over the first
            // attempt (`-legacyRefusalSeconds`, 8 by default: long enough to
            // unfold a folding iPhone meanwhile) and refuses the app for its
            // version; the answer decides the rest.
            let refusalSeconds = UserDefaults.standard.string(forKey: "legacyRefusalSeconds").flatMap { Double($0) } ?? 8
            // The image lists one old app already; the one installed anyway joins it.
            let scripted = ScriptedOldApps(apps: [ScriptedOldApps.sampleApps[1]])
            model.scriptedOldApps = scripted
            model.previewInstallAttempt = { bypass, update in
                for step in 1...10 {
                    try await Task.sleep(for: .milliseconds(100))
                    update(BusyUpdate(progress: Double(step) / 10 * 0.8))
                }
                guard bypass else {
                    update(BusyUpdate(detail: String(localized: "The virtual machine is installing the app\u{2026}"), cancellable: false))
                    try await Task.sleep(for: .seconds(refusalSeconds))
                    throw LegacyApkRefused(target: LegacyTarget(appLevel: 7, requiredLevel: 24))
                }
                try await Task.sleep(for: .milliseconds(1200))
                // The app is in, and the image's list has it now.
                await scripted.install(
                    OldApp(package: "com.example.pixelknights", label: "Pixel Knights", isOn: false, lastUpdate: Int(Date().timeIntervalSince1970)))
            }
            try? await Task.sleep(for: .milliseconds(450))
            model.installPicked(PreviewData.sampleApk())
        case "install-legacy":
            // What Android's refusal of an app built for API level 7 asks.
            try? await Task.sleep(for: .milliseconds(450))
            model.legacyInstallQuestion = LegacyInstallQuestion(target: LegacyTarget(appLevel: 7, requiredLevel: 24))
        default:
            await SnapshotPreview.applyDeferred(screen: screen, to: model)
        }
    }
}

extension AppEntry {
    func sweepInstallLeftovers() {}

    @MainActor
    func presentSystemSetup(in window: UIWindow, onInstall: @escaping () -> Void) -> Bool { false }
}
