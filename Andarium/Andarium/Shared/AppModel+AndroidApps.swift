// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// Add to Home Screen: the list of Android's apps, and a request to open one.
///
/// The list is the cache's (`AndroidAppStore`), brought up to date from the
/// tool in Android's image while Android runs. A request comes from an App
/// Intent (a Home Screen icon, Shortcuts, Search, Siri) and follows the Start
/// button's own rules (`AppOpenPlanner`): a running Android opens the app once
/// it is ready for it, a stopped one is started as Start starts it, and a start
/// that needs the person's answer first waits for it at Home. The request lives
/// in memory only.
extension AppModel {
    // MARK: What is offered

    /// Where the list and the opening come from: the runtime, or the list a
    /// preview or a UI test's probe session plays.
    var androidAppsSource: (any AndroidAppsSource)? {
        if let scriptedAndroidApps { return scriptedAndroidApps }
        return driver
    }

    /// The tool can be asked now, for the list: a normal session, not paused
    /// for space, on a host that offers Add to Home Screen (a Mac or a Vision
    /// Pro has no use for the list, so Android is not asked for it unprompted).
    var canAskAndroidApps: Bool { hostOffersAddToHomeScreen && canReachAndroidAppsTool }

    /// The tool can be reached now, whatever the host: a normal session, not paused for space.
    private var canReachAndroidAppsTool: Bool {
        guard case .running(.normal) = phase, spacePause == nil else { return false }
        return androidAppsSource?.androidAppsTool != nil
    }

    /// The host has a Home Screen to add to, and Shortcuts shows the tile for
    /// each app (iOS 27 and later). A Mac and a Vision Pro have no Home Screen,
    /// and an earlier iOS has no tile: nothing of this feature is offered
    /// there, neither the row nor the entities, the Spotlight index or the
    /// shortcut tiles.
    var hostOffersAddToHomeScreen: Bool {
        host.idiom != .mac && padAppHost.offersAddToHomeScreen
    }

    /// Settings' Apps section offers Add to Home Screen: Android runs, and its
    /// image has a list of apps to offer.
    var offersAddToHomeScreen: Bool {
        guard case .running(.normal) = phase, androidAppsSource != nil, !androidApps.unavailable else { return false }
        return hostOffersAddToHomeScreen
    }

    // MARK: The list

    /// Brings the cache up to what Android lists now: asks whether the apps
    /// changed, lists them if they did and fetches the icons that are missing.
    /// A refresh already under way is joined; with `afterAChange` another
    /// follows it, made by the refresh's owner once it has cleared the one that
    /// finished, because what changed may have come after the listing it
    /// captured. Returns whether one was asked. `forAnOpen` is the listing a
    /// host with no Home Screen takes before it opens an app by name: whatever
    /// the digest says, and no icons.
    @discardableResult
    func refreshAndroidApps(afterAChange: Bool = false, forAnOpen: Bool = false) async -> Bool {
        let state = androidApps
        guard forAnOpen ? canReachAndroidAppsTool : canAskAndroidApps, !state.unavailable,
              let tool = androidAppsSource?.androidAppsTool
        else {
            // A change announced while the guest is paused for space is listed once it resumes (`listTheChangeOwed`).
            if afterAChange, hostOffersAddToHomeScreen, !state.unavailable, case .running(.normal) = phase, spacePause != nil {
                state.refreshAgain = true
            }
            return false
        }
        if let running = state.inFlight {
            if afterAChange { state.refreshAgain = true }
            _ = await running.value
            return true
        }
        let run = session
        let flat = FlatteningAppsTool(base: OneListingAtATimeTool(base: tool, lane: state.listingLane))
        let store = state.store
        let token = state.begin()
        let task = Task.detached {
            await AndroidAppRefresh.run(tool: flat, store: store, forAnOpen: forAnOpen) { catalog, left in
                Task { @MainActor in
                    // The progress of a refresh that was ended is not heard.
                    if state.owns(token) { state.progressed(catalog, iconsLeft: left) }
                }
            }
        }
        state.inFlight = task
        let result = await task.value
        // The run ended or the cache changed meanwhile: that refresh is gone.
        guard session === run, state.owns(token), state.inFlight == task else { return true }
        state.inFlight = nil
        state.finished(result)
        switch result {
        case .refreshed:
            await syncAndroidAppsIndex()
        case .unavailable:
            // The image has no tool: what an earlier image listed is not offered.
            state.withdrawCatalog()
            await syncAndroidAppsIndex()
        case .failed:
            break
        }
        // A change was announced while this refresh was under way: it is listed
        // now, or, where Android cannot be asked (paused for space), once the
        // run can be asked again (`listTheChangeOwed`).
        if state.refreshAgain, session === run, state.owns(token) {
            guard canAskAndroidApps else { return true }
            state.refreshAgain = false
            return await refreshAndroidApps()
        }
        return true
    }

    /// The pause for space ended: a refresh that a change owed and the pause
    /// kept from being made is made now, unless one under way will make it.
    private func listTheChangeOwed() {
        let state = androidApps
        guard state.refreshAgain, state.inFlight == nil, canAskAndroidApps else { return }
        state.refreshAgain = false
        Task { await refreshAndroidApps() }
    }

    /// Android is up (the boot screen ends, which is Android's own report that
    /// its boot completed): the run's first listing is owed. It is made
    /// `startDelay` after that, so that the guest has settled and nothing is
    /// started in it during the busiest part of the boot, and again a few times
    /// if it got no answer, each after twice the wait of the one before; then
    /// the task ends, as it does at the first answer, so that nothing keeps
    /// waking an idle run. A listing made meanwhile (the chooser opened) is the
    /// run's, and the wait asks nothing more. `listNeeded` says that the request
    /// that began the boot needs the list, and the first ask is made at once,
    /// with the request's app still opening. A host with no Home Screen asks
    /// nothing here.
    func refreshAndroidAppsAtStart(listNeeded: Bool = false) {
        let state = androidApps
        guard hostOffersAddToHomeScreen, !state.listed else { return }
        state.firstOwed = true
        state.firstNeedsList = listNeeded
        startOwedFirstListing()
    }

    /// Starts the task that makes the run's first listing, if one is owed and
    /// Android can be asked now: at the boot's end, and again when a pause for
    /// space that kept it from starting, or from asking, is over. The task
    /// belongs to one run and holds a number: after each wait it looks again
    /// at whether it is still the owner, the run is still its own and the list
    /// is still wanted, and it clears its handle only if the handle is its own.
    private func startOwedFirstListing() {
        let state = androidApps
        guard state.firstOwed, !state.listed, state.first == nil, canAskAndroidApps else { return }
        let run = session
        let token = state.beginFirst()
        let listNeeded = state.firstNeedsList
        state.first = Task { [weak self, weak state] in
            defer { state?.endFirst(token) }
            // An ask made, or one that storage not being up kept from being made, is one of the
            // attempts. An app being opened is not: it has Android's attention first, and the wait
            // starts over without spending an attempt.
            var attempt = 0
            var holdingForAnOpen = false
            while attempt < AndroidAppsState.startAttempts {
                guard let state else { return }
                let wait: Duration
                if holdingForAnOpen {
                    wait = state.retryInterval
                } else if attempt == 0 {
                    wait = listNeeded ? .zero : state.startDelay
                } else {
                    wait = state.retryInterval * (1 << (attempt - 1))
                }
                holdingForAnOpen = false
                if wait > .zero { await state.sleep(wait) }
                guard let self, self.ownsFirstListing(token, in: run) else { return }
                if state.listed { break }
                // Android paused for space: the listing stays owed, and the pause ending makes it.
                guard self.canAskAndroidApps else { return }
                // Except where the app is the reason the list is wanted (`listNeeded`).
                switch state.open.pending?.stage {
                case .opening?, .launching?:
                    if !listNeeded {
                        holdingForAnOpen = true
                        continue
                    }
                default: break
                }
                let storageUp = if let driver = self.driver { await driver.storageReady() == true } else { true }
                guard self.ownsFirstListing(token, in: run) else { return }
                if state.listed { break }
                guard self.canAskAndroidApps else { return }
                if storageUp { await self.refreshAndroidApps() }
                guard self.ownsFirstListing(token, in: run) else { return }
                if state.listed { break }
                attempt += 1
            }
            // Asked as often as it is asked: nothing is owed any more.
            if let self, self.ownsFirstListing(token, in: run) { state?.firstOwed = false }
        }
    }

    /// The task holding `token` is still the run's first listing: not cancelled, the run
    /// it began in still on, and no newer task or the run's end took its place.
    private func ownsFirstListing(_ token: Int, in run: SessionState) -> Bool {
        !Task.isCancelled && session === run && androidApps.ownsFirst(token)
    }

    /// Tells the system what Android's apps are now: the tiles in Shortcuts, and
    /// Spotlight while the switch is on, through the one ordered path to the
    /// index (`AndroidAppsIndexWriter`), which judges it as it runs.
    func syncAndroidAppsIndex() async {
        await AndroidAppsHub.shared.writer.reconcile()
    }

    /// The Search switch. It governs Spotlight only: the tiles, the entities
    /// and the Siri phrase do not follow it.
    func setAndroidAppsInSearch(_ on: Bool) {
        let state = androidApps
        guard state.searchOn != on else { return }
        state.searchOn = on
        AndroidAppsHub.shared.preference.keep(on)
        Task { await syncAndroidAppsIndex() }
    }

    /// Android was erased: the cache and the apps offered are gone; the record
    /// of each icon stays, so a leftover icon can say its app is not in Android.
    func androidAppsErased() {
        androidApps.erased()
        Task { await syncAndroidAppsIndex() }
    }

    // MARK: A request to open an app

    /// What is true now, for the planner.
    private var androidAppOpenSituation: AppOpenSituation {
        AppOpenSituation(
            phase: phase, plan: plan, verdict: lifecycle.startVerdict(.normal, host: host, plan: plan),
            holdsGuestFiles: snapshotActivity.holdsGuestFiles, spacePaused: spacePause != nil, guestBusy: guestIsBusy,
            restoring: androidApps.restoring)
    }

    /// The guest is being erased (or another task works on its files behind
    /// the overlay): a start would race it.
    private var guestIsBusy: Bool {
        guard let busy else { return false }
        return busy.result == nil
    }

    /// A request to open `component`, from an App Intent or Try Again. The
    /// name is the one the request knew (the entity's), else the cache's.
    func requestAndroidApp(_ component: AndroidComponent, label: String? = nil) {
        let state = androidApps
        let known = label ?? state.store.catalog()?.app(withID: component.identifier)?.label
            ?? state.store.record(forID: component.identifier)?.label
        let name = AndroidAppText.label(known ?? "", fallback: component.packageName)
        // What covers Android goes: the app opens where the person looks.
        if case .running = phase {
            sessionSettingsOpen = false
            session.sideKeysOpen = false
        }
        let effect = state.open.request(component, label: name, in: androidAppOpenSituation, now: Date())
        performAndroidAppEffect(effect)
    }

    /// The request as an App Intent hands it over: its entity's identifier. A
    /// value that is no component is no request.
    @discardableResult
    func requestAndroidApp(identifier: String, label: String? = nil) -> Bool {
        guard let component = AndroidComponent(identifier: identifier) else { return false }
        requestAndroidApp(component, label: label)
        return true
    }

    private func performAndroidAppEffect(_ effect: AppOpenEffect) {
        switch effect {
        case .none:
            break
        case .startAndroid:
            // This request began the boot; Start's own boot is no request's.
            androidApps.bootStartedBy = androidApps.open.pending?.request
            startAndroidForRequest()
        case let .open(request):
            beginOpeningAndroidApp(request)
        case let .alert(refusal, label):
            switch refusal {
            case .needsNewerChip:
                alert = AppAlert(title: DeviceRequirement.title, message: DeviceRequirement.explanation(deviceName: host.name))
            default:
                alert = AppAlert(
                    title: AndroidAppsWords.notInAndroidTitle(label), message: AndroidAppsWords.noAndroidText)
            }
        }
        androidApps.scheduleExpiry { }
    }

    /// Starts Android as the Start button does. A start that is refused after
    /// all drops the request: nothing is left waiting for a boot that is not coming.
    private func startAndroidForRequest() {
        start(.normal)
        if case .booting = phase { return }
        androidApps.bootStartedBy = nil
        androidApps.open.drop()
    }

    /// Android is up for a request: waits until it is ready for the app, then
    /// opens it. The run ending, a pause for space, or another request, ends the
    /// wait; the situation is looked at again right before the launch.
    private func beginOpeningAndroidApp(_ request: AppOpenRequest) {
        let state = androidApps
        state.opening?.cancel()
        state.opening = Task { [weak self] in
            guard let self else { return }
            let run = session
            let source = androidAppsSource
            // Only the request that began the boot, or arrived during it, waits for Home.
            let outcome = await source?.awaitAndroidReady(homeReached: state.bootRequest != request) ?? .timedOut
            guard session === run, !Task.isCancelled, state.open.pending?.request == request else { return }
            switch outcome {
            case .setupUnfinished:
                state.open.setupUnfinished(for: request, now: Date())
            case .timedOut:
                state.open.opened(.failed, for: request, now: Date())
            case .ready:
                // A host with no Home Screen asks for the list only to open an app by name: each
                // open takes one listing before it resolves, so that what it needs to know of a
                // renamed launcher is on record (`launchersWhenSeen`).
                if !hostOffersAddToHomeScreen {
                    await refreshAndroidApps(forAnOpen: true)
                    guard session === run, !Task.isCancelled, state.open.pending?.request == request else { return }
                }
                // The wait took seconds: the run may have ended, or paused for space.
                guard state.open.readyToLaunch(request, in: androidAppOpenSituation, now: Date()) else {
                    state.scheduleExpiry { }
                    return
                }
                let result: AndroidAppOpenOutcome
                if let tool = source?.androidAppsTool {
                    let launchers = state.store.record(forID: request.component.identifier)?.launchers
                    // A renamed launcher's replacement is a command not yet sent when the apps are listed
                    // again: it goes only where the request, the run and the pause still allow it.
                    let ordered = OneListingAtATimeTool(base: tool, lane: state.listingLane)
                    result = await ordered.open(request.component, launchersWhenSeen: launchers) {
                        await MainActor.run {
                            state.open.mayReplaceLaunch(request, in: self.androidAppOpenSituation, now: Date())
                        }
                    }
                } else {
                    result = .failed
                }
                guard session === run, !Task.isCancelled else { return }
                state.open.opened(result, for: request, now: Date())
                switch result {
                case .notInAndroid:
                    // An app that is gone means the cached list is old.
                    state.invalidated()
                    Task { await self.refreshAndroidApps(afterAChange: true) }
                case .toolMissing:
                    // The image has no tool: nothing it listed is offered.
                    state.withdrawCatalog()
                    Task { await self.syncAndroidAppsIndex() }
                case .opened, .openedRenamed, .failed:
                    break
                }
            }
            state.scheduleExpiry { }
        }
    }

    /// The boot reached Android: a request that was starting opens now.
    func androidAppsBootFinished(recovery: Bool) {
        let state = androidApps
        // The request that began this boot, if it is still the one waiting for it.
        let starter = state.bootStartedBy
        state.bootStartedBy = nil
        let startedByRequest = starter != nil && starter == state.open.pending?.request && state.open.pending?.stage == .starting
        // The wait for Home's first resume is for the request that began the
        // boot, or arrived while it ran, when the launcher may not be up yet.
        // The boot is over for every request that comes later (with some app
        // in front, the ordinary case), whatever became of that one.
        if !recovery { state.bootRequest = state.open.pending?.stage == .starting ? state.open.pending?.request : nil }
        let effect = state.open.androidIsUp(recovery: recovery, now: Date())
        performAndroidAppEffect(effect)
        if !recovery {
            // The request that began this boot, for an app the cached list does not
            // name, needs the list: the wait for the guest to settle is not kept for it.
            let needsList = starter.map { startedByRequest && state.store.catalog()?.app(withID: $0.component.identifier) == nil } ?? false
            refreshAndroidAppsAtStart(listNeeded: needsList)
        }
    }

    /// The person answered what Home asked (Start, Update and start): a
    /// request that waited is starting now.
    func androidAppsPersonStarted() {
        androidApps.open.personStarted(now: Date())
    }

    /// A snapshot operation ended. A restore holds the release of the requests
    /// that waited for it until the model has adopted the guest it restored and
    /// worked out what a start now is (`androidAppsRestoreFinished`).
    func androidAppsSnapshotOperationEnded() {
        guard !androidApps.restoring else { return }
        androidAppsHoldEnded(.snapshot)
    }

    /// The model adopted what the restore left (or the restore ended without
    /// changing anything); the requests held for it are decided now. A request
    /// made while the restore was under way, unlock to adoption included, is
    /// one of them.
    func androidAppsRestoreFinished() {
        androidApps.restoring = false
        androidAppsHoldEnded(.snapshot)
    }

    /// The guest was paused for want of space: a request that was being opened
    /// waits for the person's answer, as one made during the pause does.
    func androidAppsSpacePaused() {
        let state = androidApps
        guard state.open.spacePaused(now: Date()) else { return }
        state.opening?.cancel()
        state.opening = nil
        state.scheduleExpiry { }
    }

    /// What held a request ended: a snapshot operation, or the pause for space.
    func androidAppsHoldEnded(_ hold: AppOpenHold) {
        let effect = androidApps.open.releaseHold(hold, in: androidAppOpenSituation, now: Date())
        performAndroidAppEffect(effect)
        if hold == .space {
            listTheChangeOwed()
            startOwedFirstListing()
        }
    }

    /// "Don't open": the request is dropped, and Android keeps starting.
    func dontOpenAndroidApp() {
        androidApps.opening?.cancel()
        androidApps.opening = nil
        androidApps.open.drop()
    }

    /// The failure banner's close button.
    func dismissAndroidAppBanner() {
        androidApps.open.dismissBanner()
    }

    /// The failure banner's Try Again: the request is made again.
    func retryAndroidAppOpen() {
        guard let banner = androidApps.open.banner, let component = banner.component else { return }
        androidApps.open.dismissBanner()
        requestAndroidApp(component, label: banner.label)
    }

    /// The run is over or Android restarts: nothing waits for it, and what the
    /// run listed is not known of the next.
    func androidAppsRunEnded() {
        androidApps.runEnded()
    }
}
