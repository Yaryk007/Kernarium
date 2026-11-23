// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Add to Home Screen's screens in the Simulator preview (`-screen <name>`):
/// the tool's answers are played (`ScriptedAndroidApps`), the cache is a
/// folder of its own, and a request is made as an App Intent makes it.
@MainActor
enum AddToHomePreview {
    /// Android runs and Settings is open: the row, the chooser (a state of its
    /// list), and the guide.
    private static let settingsScreens: Set<String> = [
        "add-home-row", "add-home-page", "add-home-page-refreshing", "add-home-page-loading",
        "add-home-page-failed", "add-home-page-empty", "add-home-page-search-none",
        "add-home-page-search-off", "add-home-page-search", "add-home-page-failed-empty", "add-home-guide",
    ]
    /// A request to open an app, and where it is.
    private static let requestScreens: Set<String> = [
        "add-home-boot", "add-home-pill", "add-home-wait-update", "add-home-wait-space",
        "add-home-banner-gone", "add-home-banner-slow", "add-home-banner-recovery", "add-home-banner-setup",
        "add-home-banner-tool",
        "add-home-alert-no-android", "add-home-flow-cold", "add-home-flow-warm", "add-home-flow-gone",
    ]

    static func handles(_ screen: String) -> Bool {
        settingsScreens.contains(screen) || requestScreens.contains(screen)
    }

    /// The app the screens are about.
    static let app = ScriptedAndroidApps.installedApps[0]

    /// Sets the model up for a screen: the played tool, a cache of its own, and
    /// the state the screen starts in.
    static func prepare(screen: String, model: AppModel, freeze: Bool) {
        // The screen of the Search section has a short list, so that the foot of the page is in view.
        let scripted = screen == "add-home-page-search"
            ? ScriptedAndroidApps(tool: ScriptedAppsTool(apps: Array(ScriptedAndroidApps.sampleApps.prefix(3))))
            : ScriptedAndroidApps()
        model.scriptedAndroidApps = scripted
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-add-home-\(UUID().uuidString)", isDirectory: true)
        AndroidAppsHub.shared.configure(directory: directory)
        let state = model.androidApps
        state.store = AndroidAppsHub.shared.store
        AndroidAppsHub.shared.preference = AndroidAppSearchPreference(
            defaults: UserDefaults(suiteName: "andarium-add-home-preview") ?? .standard)
        AndroidAppsHub.shared.preference.keep(screen != "add-home-page-search-off")
        state.searchOn = AndroidAppsHub.shared.preference.isOn
        state.adoptCache()
        AndroidAppsHub.shared.searchIndex = SilentAppsIndex()

        if screen == "add-home-page-loading" {
            // The list never comes, and there is none remembered.
            scripted.tool.delay = .seconds(3600)
        } else if screen == "add-home-page-failed-empty" {
            // Android does not answer and nothing is remembered.
            scripted.tool.answer(listing: .failed, digest: .failed)
        } else {
            remember(scripted, in: state)
        }
        switch screen {
        case "add-home-page-refreshing":
            scripted.tool.delay = .seconds(3600)
        case "add-home-page-failed":
            scripted.tool.answer(listing: .failed, digest: .failed)
        case "add-home-page-empty":
            state.store.erase()
            state.adoptCache()
            scripted.tool.answer(listing: .apps(AndroidAppList(apps: [], digest: "empty")), digest: .digest("empty"))
            state.finished(.refreshed(AndroidAppCatalog(listedAt: Date(), digest: "empty", apps: [])))
        default:
            break
        }

        if settingsScreens.contains(screen) {
            model.phase = .running(.normal)
            model.sessionPreset = SessionPreset(
                settings: true, settingsLarge: true,
                addToHomePage: screen != "add-home-row",
                addToHomeGuide: screen == "add-home-guide" ? app.id : nil,
                addToHomeQuery: screen == "add-home-page-search-none" ? "Zebra" : nil)
            return
        }

        // A request: its readiness takes as long as the screen needs. A banner
        // is the end of its screen, so it comes at once; a screen that rests in
        // a state holds it, and a flow plays out only when the run is played.
        state.holdsForever = freeze
        switch screen {
        case "add-home-flow-cold", "add-home-flow-warm", "add-home-flow-gone":
            scripted.readyDelay = freeze ? .seconds(3600) : .milliseconds(1800)
        case "add-home-banner-gone", "add-home-banner-slow", "add-home-banner-setup", "add-home-banner-tool":
            scripted.readyDelay = .milliseconds(300)
        default:
            scripted.readyDelay = .seconds(3600)
        }
        switch screen {
        case "add-home-boot":
            model.requestAndroidApp(app.component)
            model.frozenBootStage = freeze ? 2 : nil
            model.frozenElapsed = 9
        case "add-home-wait-update":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: true, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "add-home-wait-space":
            break
        case "add-home-alert-no-android":
            model.guest = nil
            model.plan = .firstStart
        case "add-home-banner-recovery":
            model.phase = .running(.recovery)
        case "add-home-banner-setup":
            scripted.readiness = .setupUnfinished
            scripted.readyDelay = .milliseconds(300)
            model.phase = .running(.normal)
        case "add-home-banner-gone", "add-home-flow-gone":
            scripted.tool.remove(app.id)
            model.phase = .running(.normal)
        case "add-home-banner-slow":
            scripted.tool.answer(open: app.component, .failed)
            model.phase = .running(.normal)
        case "add-home-banner-tool":
            // The image has no tool that opens apps (an older one): no Try again.
            scripted.tool.answer(open: app.component, .unavailable)
            model.phase = .running(.normal)
        case "add-home-pill", "add-home-flow-warm":
            model.phase = .running(.normal)
        default:
            break
        }
    }

    /// What the screen does once the first frame is up, as a tap on an icon would.
    static func applyDeferred(screen: String, model: AppModel) async {
        guard requestScreens.contains(screen), screen != "add-home-boot" else { return }
        try? await Task.sleep(for: .milliseconds(500))
        model.requestAndroidApp(app.component)
    }

    /// The list Android gave the last time, in the cache and on the page.
    private static func remember(_ scripted: ScriptedAndroidApps, in state: AndroidAppsState) {
        let list = AndroidAppList(apps: scripted.tool.apps, digest: "scripted-1")
        try? state.store.apply(list, at: Date().addingTimeInterval(-3600))
        for app in scripted.tool.apps {
            if let data = ScriptedAppIcons.png(for: app) {
                state.store.setIcon(data, for: app)
            } else {
                state.store.markNoIcon(for: app)
            }
        }
        state.adoptCache()
        if let catalog = state.store.catalog() {
            state.finished(.refreshed(catalog))
        }
    }
}

/// The preview takes nothing of the system's index.
@MainActor
final class SilentAppsIndex: AndroidAppsSearchIndexing {
    func offer(_ apps: [AndroidApp]) async -> Bool { true }
    func add(_ apps: [AndroidApp]) async -> Bool { true }
    func withdraw() async -> Bool { true }
    func shortcutsChanged() {}
}
