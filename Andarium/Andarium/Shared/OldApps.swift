// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation

/// Where the old apps come from: the runtime's driver asks the image's
/// `kernarium_oldapp` command through `OldAppCompatibility`, and the Simulator
/// preview plays a list. Everything the screens know of the command is
/// `AndariumCore`'s `OldApp`, `OldAppListing` and `OldAppWrite`.
@MainActor
protocol OldAppsSource: AnyObject {
    /// The installed apps Old app compatibility can apply to, within `limit`
    /// (the command's own when nil).
    func listOldApps(limit: Duration?) async -> OldAppListing
    /// Switches it on or off for one app, which the image force-stops.
    func setOldApp(_ package: String, on: Bool) async -> OldAppWrite
    /// Whether the app has a process now; nil when that cannot be told.
    func isOldAppOpen(_ package: String) async -> Bool?
}

/// What the Old app compatibility page asks the person before it changes a
/// switch, and the one thing it says when the image would not.
enum OldAppsPrompt: Identifiable, Equatable {
    /// Turning it on is asked every time. `open` adds that the app closes now.
    case turnOn(OldApp, open: Bool)
    /// Turning it off is asked only for an app that is open.
    case turnOff(OldApp)
    /// The virtual machine did not accept the change.
    case notChanged

    var id: String {
        switch self {
        case let .turnOn(app, open): "on-\(app.package)-\(open)"
        case let .turnOff(app): "off-\(app.package)"
        case .notChanged: "not-changed"
        }
    }
}

/// What the session knows of Old app compatibility: the last listing and the
/// change under way. It lasts for one run: the image's apps and the switch
/// belong to the virtual machine that is running.
@Observable
final class OldAppsState {
    /// The apps of the last listing that answered, by name.
    private(set) var apps: [OldApp] = []
    /// A listing has answered this run (an empty one included).
    private(set) var listed = false
    /// The image has no listing command, or one this Andarium cannot read.
    private(set) var unavailable = false
    /// The last listing did not get an answer.
    private(set) var failed = false
    /// A listing is under way.
    private(set) var listing = false
    /// The package whose switch is being asked about or written.
    var busy: String?
    var prompt: OldAppsPrompt?
    /// The listing under way, which a second asker joins.
    @ObservationIgnored var inFlight: Task<OldAppListing, Never>?
    /// Counts what changes the apps from outside a listing: each listing that
    /// was applied and each change that finished. A listing asked before
    /// either is older than the state and is not applied over it.
    @ObservationIgnored private(set) var generation = 0
    /// Which showing of the page asks. An answer that comes after the page was
    /// closed raises no alert for a page that is gone.
    @ObservationIgnored private(set) var showing = 0
    /// The run's first listing, which waits for Android's storage and is asked
    /// again a few times if it got no answer, then ends.
    @ObservationIgnored var first: Task<Void, Never>?
    /// The spacing of the first listing's attempts: each waits twice as long as the one before.
    @ObservationIgnored var retryInterval = Duration.seconds(5)
    /// How many times the run's first listing is asked: at the start, then
    /// after 5, 10, 20 and 40 s, and last after 80 s.
    static let startAttempts = 6

    /// The Settings row is offered: the last listing found an app.
    var offered: Bool { !unavailable && !apps.isEmpty }
    /// The switch is on for at least one app.
    var anyOn: Bool { apps.contains(where: \.isOn) }

    func begin() {
        listing = true
        failed = false
    }

    func apply(_ answer: OldAppListing) {
        listing = false
        switch answer {
        case let .apps(found):
            apps = found
            listed = true
            unavailable = false
            failed = false
            answered()
        case .unavailable:
            apps = []
            listed = true
            unavailable = true
            failed = false
            answered()
        case .failed:
            failed = true
        }
    }

    /// The first listing has its answer: nothing more is asked at the start.
    private func answered() {
        generation &+= 1
        first?.cancel()
        first = nil
    }

    /// A listing's answer is older than the state and is not applied.
    func dropped() {
        listing = false
    }

    /// A change finished: a listing asked before it may not have it.
    func changeFinished() {
        generation &+= 1
    }

    /// The app's switch, as the image last answered or the person last changed it.
    func record(_ package: String, on: Bool, at time: Int) {
        guard let index = apps.firstIndex(where: { $0.package == package }) else { return }
        apps[index].isOn = on
        apps[index].turnedOn = on ? time : 0
    }

    /// The page opens: an alert left over from before is gone, and the switches
    /// answer to this showing.
    func pageOpened() {
        prompt = nil
        showing &+= 1
    }

    /// The page closes: it asks nothing, and nothing that was asked of it
    /// raises an alert later.
    func pageClosed() {
        prompt = nil
        showing &+= 1
    }

    func reset() {
        inFlight?.cancel()
        inFlight = nil
        first?.cancel()
        first = nil
        apps = []
        listed = false
        unavailable = false
        failed = false
        listing = false
        busy = nil
        prompt = nil
    }
}

extension AppModel {
    /// Where the listing comes from: the runtime, or the list a preview or a
    /// UI test's probe session plays.
    private var oldAppsSource: (any OldAppsSource)? {
        if let scriptedOldApps { return scriptedOldApps }
        return driver
    }

    /// A listing can be asked now: a normal session whose runtime installs
    /// apps (Recovery runs no adb), not paused for space. The page's switches
    /// are disabled while it is not so.
    var canAskOldApps: Bool {
        guard capabilities.apkInstall, case .running(.normal) = phase, spacePause == nil else { return false }
        return oldAppsSource != nil
    }

    /// Lists the apps. A listing already under way is joined, one that found no
    /// command is not asked again, and nil says the run does not allow one.
    /// A change that finishes while a listing is asked makes its answer older
    /// than the page: it is dropped and the virtual machine is asked again.
    @discardableResult
    func refreshOldApps() async -> OldAppListing? {
        let state = session.oldApps
        guard canAskOldApps, let source = oldAppsSource else { return nil }
        if state.unavailable { return .unavailable }
        if let running = state.inFlight { return await running.value }
        for _ in 0..<3 {
            let mark = state.generation
            let task = Task { await source.listOldApps(limit: nil) }
            state.inFlight = task
            state.begin()
            let answer = await task.value
            // The run ended meanwhile: its session, and its apps, are gone.
            guard session.oldApps === state, !task.isCancelled else { return nil }
            state.inFlight = nil
            if state.generation == mark {
                state.apply(answer)
                return answer
            }
            state.dropped()
        }
        return nil
    }

    /// A listing for the install's result, which waits for it: within a short
    /// limit, so that a virtual machine that is slow costs a few seconds and a
    /// hint, nothing else. An answer that did not come in time, or that is older
    /// than the page, is nil and leaves the page as it was.
    private func listOldAppsBriefly() async -> OldAppListing? {
        let state = session.oldApps
        guard canAskOldApps, let source = oldAppsSource else { return nil }
        if state.unavailable { return .unavailable }
        let mark = state.generation
        let answer = await source.listOldApps(limit: OldAppCompatibility.installListLimit)
        guard session.oldApps === state, state.generation == mark, answer != .failed else { return nil }
        state.apply(answer)
        return answer
    }

    /// The first listing of a run, once Android's storage is up, and again
    /// while Settings is open for as long as the virtual machine has not
    /// answered one.
    func listOldAppsOnce() async {
        guard session.storageReady == true, !session.oldApps.listed else { return }
        await refreshOldApps()
    }

    /// Android is up (the boot screen ends): the run lists the old apps once,
    /// with no need for Settings to be open, because the image clears a switch
    /// whose app is gone or was installed again at its next listing. The
    /// storage is read as the file browser reads it (Android reports its boot
    /// done before it is mounted), and a listing that got no answer is asked
    /// again a few times, each after twice the wait of the one before; then the
    /// task ends, as it does at the first answer, so that nothing keeps waking
    /// an idle run. Recovery and an image without adb are not asked
    /// (`canAskOldApps`).
    func listOldAppsAtStart() {
        let state = session.oldApps
        guard state.first == nil, !state.listed, canAskOldApps else { return }
        state.first = Task { [weak self, weak state] in
            for attempt in 0..<OldAppsState.startAttempts {
                if attempt > 0, let wait = state?.retryInterval {
                    try? await Task.sleep(for: wait * (1 << (attempt - 1)))
                }
                guard let self, let state, !Task.isCancelled, self.session.oldApps === state else { return }
                if let driver = self.driver, await driver.storageReady() == true {
                    await self.refreshOldApps()
                }
                if state.listed { break }
            }
            state?.first = nil
        }
    }

    /// The person moved an app's switch. The switch itself stays where the
    /// image has it until the change is written. Turning it on is always
    /// asked; turning it off only when the app is open. Nothing is asked of a
    /// virtual machine that cannot be asked, and nothing is raised for a page
    /// that was closed meanwhile.
    func requestOldApp(_ package: String, on: Bool) async {
        let state = session.oldApps
        guard canAskOldApps, state.busy == nil, state.prompt == nil, let source = oldAppsSource,
              let app = state.apps.first(where: { $0.package == package }), app.isOn != on
        else { return }
        let showing = state.showing
        state.busy = package
        let open = await source.isOldAppOpen(package) == true
        guard session.oldApps === state, state.busy == package else { return }
        guard state.showing == showing else {
            state.busy = nil
            return
        }
        if on {
            state.prompt = .turnOn(app, open: open)
            state.busy = nil
        } else if open {
            state.prompt = .turnOff(app)
            state.busy = nil
        } else {
            await writeOldApp(app, on: false, showing: showing)
        }
    }

    /// The alert's answer. Cancel changes nothing.
    func answerOldApps(_ prompt: OldAppsPrompt, confirmed: Bool) async {
        let state = session.oldApps
        state.prompt = nil
        guard confirmed else { return }
        let showing = state.showing
        switch prompt {
        case let .turnOn(app, _): await writeOldApp(app, on: true, showing: showing)
        case let .turnOff(app): await writeOldApp(app, on: false, showing: showing)
        case .notChanged: break
        }
    }

    /// Writes one switch. The image's answer decides what the page shows: a
    /// change it took is recorded, and any other answer is said and the list
    /// is read again, since the app may be gone or no longer eligible.
    private func writeOldApp(_ app: OldApp, on: Bool, showing: Int) async {
        let state = session.oldApps
        guard let source = oldAppsSource else { return }
        state.busy = app.package
        let answer = await source.setOldApp(app.package, on: on)
        guard session.oldApps === state else { return }
        state.busy = nil
        // A listing asked before this is older than what the image now has.
        state.changeFinished()
        switch answer {
        case .applied:
            state.record(app.package, on: on, at: Int(Date().timeIntervalSince1970))
        case .notEligible, .failed:
            // The page that asked may be gone by now: it is not told of it.
            if state.showing == showing { state.prompt = .notChanged }
            await refreshOldApps()
        }
    }

    /// After an Install anyway that succeeded for an app built for an Android
    /// before 4.0: whether the listing now names an app the install added or
    /// updated, which is the one Old app compatibility can help. `before` is
    /// the listing taken as the file was sent again.
    func installAddedOldApp(listedBefore before: [OldApp]) async -> Bool {
        guard case let .apps(after)? = await listOldAppsBriefly() else { return false }
        return OldAppInstallHint.installedApp(listedBefore: before, listedAfter: after) != nil
    }

    /// The apps as the listing has them now, taken afresh within the install's
    /// short limit: the baseline of an install that may add one. Nil when the
    /// virtual machine cannot say in time.
    func oldAppsBeforeInstall() async -> [OldApp]? {
        guard case let .apps(apps)? = await listOldAppsBriefly() else { return nil }
        return apps
    }
}

/// The words of Old app compatibility. "Virtual machine", never "VM" or
/// "guest"; no promise that an app works; the app's own name and package
/// name are shown as they are.
nonisolated enum OldAppsWords {
    static let title = String(localized: "Old app compatibility")
    static let on = String(localized: "On")
    static let loading = String(localized: "Looking for old apps\u{2026}")
    static let sectionHeader = String(localized: "Built for Android before 4.0")
    static let empty = String(localized: "No app built for Android before 4.0 is installed.")
    static let failed = String(localized: "The virtual machine didn\u{2019}t answer. Try again.")
    static let tryAgain = String(localized: "Try again")
    static let turnOn = String(localized: "Turn on")
    static let turnOff = String(localized: "Turn off")
    static let cancel = String(localized: "Cancel")
    static let ok = String(localized: "OK")

    /// The page's footer: what the switch is for, then when to use it.
    static var footer: String {
        String(localized: "Some apps this old crash because they rely on how Android used to work. Old app compatibility makes Android run an app more like older versions did.")
            + "\n\n"
            + String(localized: "Turn it on only for an app that crashes: it can make an app that works today behave differently. Turning it on or off closes the app if it\u{2019}s open. Only apps it can apply to are listed.")
    }

    /// What VoiceOver reads for an app's switch.
    static func switchLabel(_ label: String) -> String {
        String(
            localized: "\(label), Old app compatibility",
            comment: "What VoiceOver reads for an app's switch on the Old app compatibility page: the app's name, then the feature's name.")
    }

    static func title(for prompt: OldAppsPrompt) -> String {
        switch prompt {
        case let .turnOn(app, _): String(localized: "Turn on for \u{201C}\(app.label)\u{201D}?")
        case let .turnOff(app): String(localized: "Turn off for \u{201C}\(app.label)\u{201D}?")
        case .notChanged: String(localized: "Old app compatibility wasn\u{2019}t changed")
        }
    }

    static func message(for prompt: OldAppsPrompt) -> String {
        switch prompt {
        case let .turnOn(app, open):
            let message = String(
                localized: "Android will run this app more like older versions did. This may help an app that crashes, but it can also make the app behave differently. You can turn it off again here.")
            return open ? Sentences.join(message, closesNow(app.label)) : message
        case let .turnOff(app):
            return Sentences.join(closesNow(app.label), String(localized: "It will open without Old app compatibility next time."))
        case .notChanged:
            return String(localized: "The virtual machine didn\u{2019}t accept the change. Try again.")
        }
    }

    private static func closesNow(_ label: String) -> String {
        String(localized: "\u{201C}\(label)\u{201D} is open and will close now.")
    }

    /// The two sentences after Install anyway installed an old app: what it
    /// was built for, and where the switch is.
    static let installHint = String(
        localized: "It was built for an Android version before 4.0. If it crashes, open Settings from the side keys and choose Old app compatibility.")
}
