// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import CoreSpotlight
import Foundation
import Testing
@testable import Andarium

/// What the application's index is told, in order, instead of the system's.
/// A write can be held until the test lets it go, and made to fail.
@MainActor
final class RecordingAppsIndex: AndroidAppsSearchIndexing {
    private(set) var offered: [[String]] = []
    private(set) var added: [[String]] = []
    private(set) var withdrawn = 0
    private(set) var shortcutUpdates = 0
    /// Every write and every tile update, in the order the system took them.
    private(set) var events: [String] = []
    /// Whether the system takes what it is given.
    var accepts = true
    /// The next offer waits here until `letGo()`.
    var holdsNextOffer = false
    private var held: CheckedContinuation<Void, Never>?
    /// The next withdrawal and the next selective add wait until `letGoWithdraw()` and `letGoAdd()`.
    var holdsNextWithdraw = false
    var holdsNextAdd = false
    private var heldWithdraw: CheckedContinuation<Void, Never>?
    private var heldAdd: CheckedContinuation<Void, Never>?

    func offer(_ apps: [AndroidApp]) async -> Bool {
        if holdsNextOffer {
            holdsNextOffer = false
            events.append("offer-started")
            await withCheckedContinuation { held = $0 }
        }
        guard accepts else { events.append("offer-failed"); return false }
        offered.append(apps.map(\.id))
        events.append("offer")
        return true
    }

    func add(_ apps: [AndroidApp]) async -> Bool {
        if holdsNextAdd {
            holdsNextAdd = false
            events.append("add-started")
            await withCheckedContinuation { heldAdd = $0 }
        }
        guard accepts else { return false }
        added.append(apps.map(\.id))
        events.append("add")
        return true
    }

    func withdraw() async -> Bool {
        if holdsNextWithdraw {
            holdsNextWithdraw = false
            events.append("withdraw-started")
            await withCheckedContinuation { heldWithdraw = $0 }
        }
        guard accepts else { events.append("withdraw-failed"); return false }
        withdrawn += 1
        events.append("withdraw")
        return true
    }

    func shortcutsChanged() {
        shortcutUpdates += 1
        events.append("tiles")
    }

    func letGo() {
        held?.resume()
        held = nil
    }

    func letGoWithdraw() {
        heldWithdraw?.resume()
        heldWithdraw = nil
    }

    func letGoAdd() {
        heldAdd?.resume()
        heldAdd = nil
    }
}

/// The waits of the run's first listing, held until a test lets them go: nothing of it depends on a clock.
/// A wait that is cancelled ends at once.
nonisolated final class HeldWaits: @unchecked Sendable {
    private let lock = NSLock()
    private var waiters: [Int: CheckedContinuation<Void, Never>] = [:]
    private var cancelled: Set<Int> = []
    private var asked: [Duration] = []
    private var next = 0

    /// Every wait asked for, in order, and how many are still held.
    var durations: [Duration] { lock.withLock { asked } }
    var holding: Int { lock.withLock { waiters.count } }

    func wait(_ duration: Duration) async {
        let id = lock.withLock { () -> Int in
            next += 1
            asked.append(duration)
            return next
        }
        await withTaskCancellationHandler {
            await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
                let endsAtOnce = lock.withLock { () -> Bool in
                    if cancelled.remove(id) != nil { return true }
                    waiters[id] = continuation
                    return false
                }
                if endsAtOnce { continuation.resume() }
            }
        } onCancel: {
            let held = lock.withLock { () -> CheckedContinuation<Void, Never>? in
                if let held = waiters.removeValue(forKey: id) { return held }
                cancelled.insert(id)
                return nil
            }
            held?.resume()
        }
    }

    /// Ends every wait that is held now.
    func releaseAll() {
        let held = lock.withLock { () -> [CheckedContinuation<Void, Never>] in
            defer { waiters = [:] }
            return Array(waiters.values)
        }
        held.forEach { $0.resume() }
    }
}

/// Add to Home Screen as the model plays it: the list a refresh keeps and what
/// it tells the system, a request made as an App Intent makes it while Android
/// runs, while it starts and while it is off, what a request that must wait
/// waits for, how each failure ends, and what an erase leaves. The runtime is
/// a stub and the tool is the played one (`ScriptedAndroidApps`).
@Suite(.serialized)
@MainActor
final class AndroidAppsModelTests {
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
    private let comet = ScriptedAndroidApps.installedApps[0]
    private let inkleaf = ScriptedAndroidApps.installedApps[1]

    private struct Rig {
        let model: AppModel
        let driver: StubDriver
        let scripted: ScriptedAndroidApps
        let index: RecordingAppsIndex
        let directory: URL
        /// The run's first listing waits here, until a test lets it go.
        let waits: HeldWaits
        var state: AndroidAppsState { model.androidApps }
        var tool: ScriptedAppsTool { scripted.tool }

        /// Ends what the test left waiting (an hour-long wait would outlive it).
        func stop() {
            state.first?.cancel()
            state.opening?.cancel()
            waits.releaseAll()
        }
    }

    /// A model over a stub runtime, a cache of its own and the played tool.
    /// Nothing is asked slowly: each answer takes a millisecond.
    private func rig(phase: Phase = .home, plan: StartPlan = .normal, guest: Bool = true) throws -> Rig {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-apps-test-\(UUID().uuidString)", isDirectory: true)
        let model = AppModel(
            guest: guest ? Guest.fresh(on: host) : nil, host: host, system: system, capabilities: .device,
            createRequirementBytes: 1)
        let driver = StubDriver(plan: plan)
        model.driver = driver
        model.plan = plan
        model.phase = phase
        model.session.storageReady = true
        let scripted = ScriptedAndroidApps()
        scripted.tool.delay = .milliseconds(1)
        scripted.readyDelay = .milliseconds(1)
        model.scriptedAndroidApps = scripted
        // The host the rows are about is an iPhone of iOS 27, whatever system the tests run on.
        model.playedPadAppHost = IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false)
        AndroidAppsHub.shared.configure(directory: directory)
        AndroidAppsHub.shared.attach(model)
        let index = RecordingAppsIndex()
        AndroidAppsHub.shared.searchIndex = index
        let state = model.androidApps
        state.store = AndroidAppsHub.shared.store
        AndroidAppsHub.shared.preference = AndroidAppSearchPreference(
            defaults: try #require(UserDefaults(suiteName: "andarium-apps-test-\(UUID().uuidString)")))
        state.searchOn = true
        state.adoptCache()
        let waits = HeldWaits()
        state.sleep = { await waits.wait($0) }
        return Rig(model: model, driver: driver, scripted: scripted, index: index, directory: directory, waits: waits)
    }

    private func eventually(_ seconds: Double = 10, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return condition()
    }

    /// Whether `task` ends within `seconds`; one that does not is left to `rig.stop()`.
    private func ends(_ task: Task<Void, Never>, within seconds: Double = 5) async -> Bool {
        final class Ended { var value = false }
        let ended = Ended()
        Task {
            await task.value
            ended.value = true
        }
        return await eventually(seconds) { ended.value }
    }

    // MARK: The list

    @Test func aRefreshKeepsTheListAndItsIconsAndTellsTheSystem() async throws {
        let rig = try rig(phase: .running(.normal))
        #expect(await rig.model.refreshAndroidApps())
        let state = rig.state
        #expect(state.listed && !state.failed && !state.refreshing)
        #expect(state.apps.count == ScriptedAndroidApps.sampleApps.count)
        #expect(state.store.icon(forID: comet.id) != nil)
        // The cache answers without a model: the App Intent's query reads it.
        #expect(AndroidAppsHub.shared.listedApps().map(\.id) == state.apps.map(\.id))
        #expect(rig.index.offered.count == 1 && rig.index.offered[0] == state.apps.map(\.id))
        #expect(rig.index.shortcutUpdates == 1)
    }

    @Test func aRefreshOfAnUnchangedListAsksOnlyTheDigest() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let listings = rig.tool.listingsAsked
        await rig.model.refreshAndroidApps()
        #expect(rig.tool.listingsAsked == listings)
        #expect(rig.tool.digestsAsked >= 2)
        #expect(rig.index.offered.count == 1, "nothing changed, so nothing is offered again")
    }

    @Test func anInstalledAppReachesTheListAndTheSystem() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let extra = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.newcomer", activityClass: "com.example.newcomer.Main")),
            label: "Newcomer", builtIn: false, iconTag: "0000000000000001")
        rig.tool.install(extra)
        await rig.model.refreshAndroidApps()
        #expect(rig.state.apps.contains { $0.id == extra.id })
        #expect(rig.index.offered.count == 2 && rig.index.offered[1].contains(extra.id))
    }

    @Test func noRefreshIsAskedWhereTheToolCannotBe() async throws {
        let home = try rig(phase: .home)
        #expect(await home.model.refreshAndroidApps() == false)
        let recovery = try rig(phase: .running(.recovery))
        #expect(await recovery.model.refreshAndroidApps() == false)
        let paused = try rig(phase: .running(.normal))
        paused.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        #expect(await paused.model.refreshAndroidApps() == false)
        #expect(paused.tool.digestsAsked == 0)
    }

    @Test func anImageWithoutTheToolOffersNothing() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        #expect(!rig.state.apps.isEmpty)
        // An earlier image listed apps; this one has no tool. What it listed is withdrawn everywhere.
        rig.tool.answer(listing: .unavailable, digest: .unavailable)
        rig.state.invalidated()
        await rig.model.refreshAndroidApps()
        #expect(rig.state.unavailable)
        #expect(!rig.model.offersAddToHomeScreen)
        #expect(rig.state.apps.isEmpty && rig.state.store.catalog() == nil)
        #expect(AndroidAppsHub.shared.listedApps().isEmpty)
        #expect(rig.index.offered.last == [], "Spotlight holds none of them")
        #expect(rig.index.events.last != "offer-failed")
    }

    @Test func aRefreshThatGotNoAnswerKeepsWhatWasCached() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let before = rig.state.apps
        rig.tool.install(try #require(ScriptedAndroidApps.installedApps.first))  // the digest changes
        rig.tool.answer(listing: .failed, digest: .digest("changed"))
        await rig.model.refreshAndroidApps()
        #expect(rig.state.failed && rig.state.apps == before)
        #expect(rig.state.listedAt != nil)
    }

    // MARK: Search

    @Test func theSwitchWithdrawsTheAppsAndOffersThemAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.model.setAndroidAppsInSearch(false)
        #expect(await eventually { rig.index.withdrawn == 1 })
        #expect(!rig.state.searchOn && !AndroidAppsHub.shared.preference.isOn)
        // The list still reaches the shortcut tiles, which are the route to the Home Screen.
        #expect(AndroidAppsHub.shared.listedApps().count == rig.state.apps.count)
        rig.model.setAndroidAppsInSearch(true)
        #expect(await eventually { rig.index.offered.count == 2 })
        #expect(AndroidAppsHub.shared.preference.isOn)
    }

    @Test func aRefreshWhileTheSwitchIsOffOffersNothing() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.state.searchOn = false
        AndroidAppsHub.shared.preference.keep(false)
        rig.tool.install(try #require(ScriptedAndroidApps.installedApps.last))
        await rig.model.refreshAndroidApps()
        #expect(rig.index.offered.isEmpty && rig.index.withdrawn == 1)
    }

    // MARK: A request while Android runs

    @Test func aRequestWhileAndroidRunsOpensTheAppAndClosesWhatCoversIt() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.model.sessionSettingsOpen = true
        rig.model.session.sideKeysOpen = true
        rig.model.requestAndroidApp(comet.component)
        #expect(!rig.model.sessionSettingsOpen && !rig.model.session.sideKeysOpen)
        #expect(rig.state.open.pending?.stage == .opening)
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(await eventually { rig.state.open.pending == nil })
        #expect(rig.state.open.banner == nil)
        #expect(rig.driver.begun.isEmpty, "Android runs: nothing is started")
        #expect(rig.scripted.readinessAsked == 1)
    }

    @Test func aRequestNamesTheAppAsItsEntityDid() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .seconds(3600)
        #expect(rig.model.requestAndroidApp(identifier: comet.id, label: "Comet Hop"))
        #expect(rig.state.open.pending?.request.label == "Comet Hop")
        #expect(rig.state.open.pending?.request.component == comet.component)
        rig.model.dontOpenAndroidApp()
    }

    @Test func aValueThatIsNoComponentIsNoRequest() throws {
        let rig = try rig(phase: .running(.normal))
        #expect(!rig.model.requestAndroidApp(identifier: "not-a-component"))
        #expect(!rig.model.requestAndroidApp(identifier: ""))
        #expect(rig.state.open.pending == nil)
    }

    @Test func aNewerRequestReplacesAnOlderOne() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .milliseconds(200)
        rig.model.requestAndroidApp(comet.component)
        rig.model.requestAndroidApp(inkleaf.component)
        #expect(await eventually { rig.state.open.pending == nil })
        #expect(rig.tool.opened == [inkleaf.component])
    }

    @Test func dontOpenDropsTheRequestAndNothingIsOpenedLater() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .milliseconds(100)
        rig.model.requestAndroidApp(comet.component)
        rig.model.dontOpenAndroidApp()
        #expect(rig.state.open.pending == nil)
        try await Task.sleep(for: .milliseconds(300))
        #expect(rig.tool.opened.isEmpty)
    }

    @Test func theRunEndingDropsTheRequestAndTheBanner() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .seconds(3600)
        rig.model.requestAndroidApp(comet.component)
        rig.model.runEnded(RunEnd(result: 0), uptimeSeconds: nil)
        #expect(rig.state.open.pending == nil && rig.state.open.banner == nil)
        #expect(!rig.state.listed)
        #expect(rig.tool.opened.isEmpty)
    }

    // MARK: A request while Android is off

    @Test func aRequestWhileAndroidIsOffStartsItAsStartDoesAndOpensTheAppWhenItIsUp() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        rig.model.requestAndroidApp(comet.component)
        #expect(rig.driver.begun.count == 1 && rig.driver.begun.first?.mode == .normal)
        #expect(rig.model.phase == .booting(.normal))
        #expect(rig.state.open.pending?.stage == .starting)
        #expect(rig.tool.opened.isEmpty)
        rig.model.phase = .running(.normal)
        rig.model.finishBoot(.normal)
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(await eventually { rig.state.open.pending == nil })
    }

    @Test func aRequestWhileAndroidIsStartingJoinsTheStart() throws {
        let rig = try rig(phase: .booting(.normal))
        rig.model.requestAndroidApp(comet.component)
        #expect(rig.driver.begun.isEmpty, "no second start")
        #expect(rig.state.open.pending?.stage == .starting)
    }

    @Test func aRequestWaitsAtHomeForAnUnfinishedUpdateAndStartsWithTheAnswer() async throws {
        let rig = try rig(phase: .home, plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        rig.model.requestAndroidApp(comet.component)
        guard case .waiting(.update, _)? = rig.state.open.pending?.stage else {
            Issue.record("the request should wait for the answer to the update: \(String(describing: rig.state.open.pending))")
            return
        }
        #expect(rig.driver.begun.isEmpty)
        // The person answers: Update and start.
        rig.model.plan = .update(from: "r1", to: "r2")
        rig.model.start(.normal)
        #expect(rig.driver.begun.count == 1)
        #expect(rig.state.open.pending?.stage == .starting)
    }

    @Test func aRequestWaitsAtHomeWhileThereIsNoRoomToStart() throws {
        var tight = host
        tight.freeStorageBytes = 1_000_000
        let model = AppModel(guest: Guest.fresh(on: tight), host: tight, system: system, capabilities: .device, createRequirementBytes: 1)
        model.driver = StubDriver(plan: .normal)
        model.plan = .normal
        model.scriptedAndroidApps = ScriptedAndroidApps()
        AndroidAppsHub.shared.configure(
            directory: FileManager.default.temporaryDirectory.appendingPathComponent("andarium-apps-test-\(UUID().uuidString)"))
        model.androidApps.store = AndroidAppsHub.shared.store
        model.requestAndroidApp(comet.component)
        guard case .waiting(.space, _)? = model.androidApps.open.pending?.stage else {
            Issue.record("the request should wait for space: \(String(describing: model.androidApps.open.pending))")
            return
        }
    }

    @Test func aRequestWithNoVirtualMachineSaysTheAppIsNotInAndroid() throws {
        let rig = try rig(phase: .home, plan: .firstStart, guest: false)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.model.alert?.title == AndroidAppsWords.notInAndroidTitle("Comet Hop"))
        #expect(rig.model.alert?.message == AndroidAppsWords.noAndroidText)
        #expect(rig.state.open.pending == nil)
    }

    @Test func aRequestInRecoveryIsRefusedWithABanner() throws {
        let rig = try rig(phase: .running(.recovery))
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.banner?.failure == .recovery)
        #expect(rig.state.open.pending == nil && rig.tool.opened.isEmpty)
    }

    // MARK: How an open ends

    @Test func anAppThatIsGoneEndsWithABannerAndAListAskedAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.tool.remove(comet.id)
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .notInAndroid })
        #expect(rig.state.open.banner?.app == comet.component)
        #expect(rig.state.open.banner?.component == nil, "no Try Again for an app that is not there")
        // The cache was dropped and read again, so the app is out of the list.
        #expect(await eventually { !rig.state.apps.contains { $0.id == comet.id } && rig.state.listed })
    }

    @Test func androidTakingTooLongEndsWithABannerAndTryAgainOpensTheApp() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.answer(open: comet.component, .failed)
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .tookTooLong })
        rig.tool.answer(open: comet.component, .opened)
        rig.model.retryAndroidAppOpen()
        #expect(rig.state.open.banner == nil)
        #expect(await eventually { rig.tool.opened.count == 2 && rig.state.open.pending == nil })
        #expect(rig.state.open.banner == nil)
    }

    @Test func androidNeverBecomingReadyEndsWithTheSameBanner() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readiness = .timedOut
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .tookTooLong })
        #expect(rig.tool.opened.isEmpty)
    }

    @Test func androidsUnfinishedSetupEndsWithABannerThatOffersTryAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readiness = .setupUnfinished
        rig.model.requestAndroidApp(comet.component)
        #expect(await eventually { rig.state.open.banner?.failure == .setupUnfinished })
        #expect(rig.state.open.banner?.component == comet.component)
        #expect(rig.tool.opened.isEmpty)
        rig.model.dismissAndroidAppBanner()
        #expect(rig.state.open.banner == nil)
    }

    // MARK: Erase and the entities

    @Test func anEraseDropsTheCacheAndKeepsTheRecordOfEachIcon() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        #expect(rig.state.store.record(forID: comet.id)?.label == "Comet Hop")
        rig.model.androidAppsErased()
        #expect(rig.state.apps.isEmpty && rig.state.store.catalog() == nil)
        #expect(AndroidAppsHub.shared.listedApps().isEmpty)
        #expect(rig.state.store.record(forID: comet.id)?.label == "Comet Hop")
        // What was offered to Search is replaced by nothing.
        #expect(await eventually { rig.index.offered.last == [] })
    }

    @available(iOS 27.0, *)
    @Test func anIconAfterAnEraseStillResolvesToItsAppAndWorksAfterAReinstall() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.model.androidAppsErased()
        let query = AndroidAppQuery()
        let leftover = try await query.entities(for: [comet.id])
        #expect(leftover.map(\.name) == ["Comet Hop"], "the leftover icon keeps its app's name")
        // The app is installed in a new Android: the same identifier opens it again.
        rig.model.phase = .running(.normal)
        rig.model.session.storageReady = true
        await rig.model.refreshAndroidApps()
        let again = try await query.entities(for: [comet.id])
        #expect(again.map(\.id) == [comet.id])
        #expect(rig.model.requestAndroidApp(identifier: again[0].id, label: again[0].name))
        #expect(await eventually { rig.tool.opened == [comet.component] })
    }

    @available(iOS 27.0, *)
    @Test func theQueryAnswersFromTheCacheWhileAndroidIsOff() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.model.phase = .home
        let query = AndroidAppQuery()
        let all = try await query.allEntities()
        #expect(all.count == ScriptedAndroidApps.sampleApps.count)
        #expect(all.map(\.id) == rig.state.apps.map(\.id))
        #expect(try await query.suggestedEntities().count == all.count)
        // An identifier that is no component names nothing.
        #expect(try await query.entities(for: ["nothing"]).isEmpty)
    }

    @available(iOS 27.0, *)
    @Test func anEntityCarriesTheAppsNameItsIdentifierAndItsIcon() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let entity = try #require(AndroidAppsHub.shared.entities().first { $0.id == comet.id })
        #expect(entity.name == "Comet Hop")
        #expect(entity.id == "com.example.comethop/com.example.comethop.MainActivity")
        #expect(entity.attributeSet.title == "Comet Hop")
        #expect(entity.attributeSet.thumbnailData != nil)
    }

    // MARK: The row

    @Test func theRowIsOfferedOnlyWhileAndroidRuns() throws {
        let running = try rig(phase: .running(.normal))
        #expect(running.model.offersAddToHomeScreen)
        for phase in [Phase.home, .booting(.normal), .running(.recovery), .failed] {
            let other = try rig(phase: phase)
            #expect(!other.model.offersAddToHomeScreen, "no row in \(phase)")
        }
    }

    @Test func aMacHasNoHomeScreenToAddTo() throws {
        let rig = try rig(phase: .running(.normal))
        rig.model.host.idiom = .mac
        #expect(!rig.model.offersAddToHomeScreen)
    }

    /// Nothing of Add to Home Screen is offered to `host`: no row, and the apps are neither entities
    /// nor written to Spotlight.
    private func expectNothingOffered(to host: IPadAppHost, _ rig: Rig) async {
        rig.model.playedPadAppHost = host
        #expect(!rig.model.hostOffersAddToHomeScreen)
        #expect(!rig.model.offersAddToHomeScreen)
        await rig.model.refreshAndroidApps()
        let writes = rig.index.events.count
        AndroidAppsHub.shared.attach(rig.model)
        #expect(!AndroidAppsHub.shared.hostOffers)
        #expect(AndroidAppsHub.shared.listedApps().isEmpty)
        await rig.model.syncAndroidAppsIndex()
        #expect(rig.index.events.count == writes, "nothing is offered to Spotlight: \(rig.index.events)")
    }

    @Test func aVisionProOn26HasNoHomeScreenToAddTo() async throws {
        let rig = try rig(phase: .running(.normal))
        // visionOS 26.0 does not say it runs the iPad app; the hardware's name does.
        await expectNothingOffered(
            to: IPadAppHost(isiOSAppOnMac: false, visionAnswer: nil, machine: "RealityDevice14,1"), rig)
    }

    @Test func anIOSBefore27OffersNoAddToHomeScreen() async throws {
        let rig = try rig(phase: .running(.normal))
        // A Home Screen, and no tile in Shortcuts for the guide to send the person to.
        await expectNothingOffered(
            to: IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false, hasAppTilesInShortcuts: false), rig)
    }

    @Test func theSystemsOwnHostOffersAddToHomeScreenFromIOS27() {
        var tilesExpected = false
        if #available(iOS 27.0, *) { tilesExpected = true }
        let system = IPadAppHost.system
        #expect(system.hasAppTilesInShortcuts == tilesExpected)
        #expect(system.offersAddToHomeScreen == (system.hasHomeScreen && tilesExpected))
    }

    // MARK: Where the feature is not offered

    @Test func aMacWritesNothingToSpotlightAndHasNoEntities() async throws {
        let rig = try rig(phase: .running(.normal))
        // The cache holds apps (a listing made when the hub was set up elsewhere), and the host offers none.
        await rig.model.refreshAndroidApps()
        let writes = rig.index.events.count
        rig.model.host.idiom = .mac
        AndroidAppsHub.shared.attach(rig.model)
        #expect(!AndroidAppsHub.shared.hostOffers)
        #expect(AndroidAppsHub.shared.listedApps().isEmpty)
        if #available(iOS 27.0, *) {
            let query = AndroidAppQuery()
            #expect(try await query.allEntities().isEmpty)
            #expect(try await query.suggestedEntities().isEmpty)
            #expect(try await query.entities(for: [comet.id]).isEmpty)
        }
        await rig.model.syncAndroidAppsIndex()
        if #available(iOS 27.0, *) {
            let query = AndroidAppQuery()
            try await query.reindexAllEntities(indexDescription: CSSearchableIndexDescription())
            try await query.reindexEntities(for: [comet.id], indexDescription: CSSearchableIndexDescription())
        }
        #expect(rig.index.events.count == writes, "no index write, no tile update: \(rig.index.events)")
        // And a refresh on such a host tells the system nothing.
        rig.model.setAndroidAppsInSearch(false)
        await rig.model.syncAndroidAppsIndex()
        #expect(rig.index.events.count == writes)
    }

    // MARK: The switch is for Search only

    @Test func theSwitchOffLeavesTheEntitiesAndTheTilesAndEmptiesSpotlight() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.model.setAndroidAppsInSearch(false)
        await rig.model.refreshAndroidApps()
        #expect(rig.state.apps.count == ScriptedAndroidApps.sampleApps.count)
        // Spotlight holds none and was never given any.
        #expect(rig.index.offered.isEmpty)
        #expect(rig.index.events.contains("withdraw"))
        // The entities, and the parameters of the App Shortcut that are made of them, are as with the switch on.
        #expect(AndroidAppsHub.shared.listedApps().count == rig.state.apps.count)
        if #available(iOS 27.0, *) {
            #expect(try await AndroidAppQuery().suggestedEntities().count == rig.state.apps.count)
        }
        #expect(rig.index.shortcutUpdates >= 1, "the tiles are told of the apps")
        // An icon added while the switch is off reaches the tiles.
        let before = rig.index.shortcutUpdates
        rig.tool.install(AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.newcomer", activityClass: "com.example.newcomer.Main")),
            label: "Newcomer", builtIn: false, iconTag: "0000000000000042"))
        await rig.model.refreshAndroidApps()
        #expect(rig.index.shortcutUpdates == before + 1)
        #expect(rig.index.offered.isEmpty)
        #expect(AndroidAppsHub.shared.listedApps().contains { $0.label == "Newcomer" })
    }

    @available(iOS 27.0, *)
    @Test func theSystemsReindexAsksAreJudgedByTheSwitchAndOfferOnlyListedApps() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let query = AndroidAppQuery()
        let off = CSSearchableIndexDescription()
        // On: the listed app is added; an identifier the cache no longer lists is not.
        let gone = "com.example.gone/com.example.gone.Main"
        try await query.reindexEntities(for: [comet.id, gone], indexDescription: off)
        #expect(rig.index.added.last == [comet.id])
        // Off: nothing is added, whatever the system asks.
        rig.model.setAndroidAppsInSearch(false)
        let adds = rig.index.added.count
        let offers = rig.index.offered.count
        try await query.reindexEntities(for: [comet.id], indexDescription: off)
        try await query.reindexAllEntities(indexDescription: off)
        #expect(rig.index.added.count == adds && rig.index.offered.count == offers)
    }

    @Test func aWriteAskedBeforeTheSwitchWasTurnedOffNeverLandsAfterIt() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.index.holdsNextOffer = true
        // The refresh's offer is under way at the system and held there.
        let refreshing = Task { await rig.model.refreshAndroidApps() }
        #expect(await eventually { rig.index.events.contains("offer-started") })
        rig.model.setAndroidAppsInSearch(false)
        try await Task.sleep(for: .milliseconds(50))
        rig.index.letGo()
        await refreshing.value
        #expect(await eventually { rig.index.events.last == "withdraw" })
        // In order: the offer that was under way first, then the withdrawal; never an offer after it.
        let writes = rig.index.events.filter { ["offer", "withdraw"].contains($0) }
        #expect(writes == ["offer", "withdraw"], "\(rig.index.events)")
    }

    @Test func aWithdrawalThatFailedIsFinishedAtTheNextLaunch() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.index.accepts = false
        rig.model.setAndroidAppsInSearch(false)
        #expect(await eventually { rig.index.events.contains("withdraw-failed") })
        #expect(rig.index.withdrawn == 0)
        // The next launch reconciles Spotlight with the switch's last word.
        rig.index.accepts = true
        await AndroidAppsHub.shared.writer.reconcile()
        #expect(rig.index.withdrawn == 1)
        // And once it took, a launch writes nothing again.
        let events = rig.index.events.count
        await AndroidAppsHub.shared.writer.reconcile()
        #expect(rig.index.events.count == events)
    }

    @Test func artworkThatArrivedAfterTheRowsIsToldToShortcutsAndSpotlight() async throws {
        let rig = try rig(phase: .running(.normal))
        let app = AndroidApp(component: comet.component, label: "Comet Hop", builtIn: false, iconTag: "00000000000000aa")
        try rig.state.store.apply(AndroidAppList(apps: [app], digest: "d"), at: Date())
        rig.state.adoptCache()
        await rig.model.syncAndroidAppsIndex()
        let offers = rig.index.offered.count
        let tiles = rig.index.shortcutUpdates
        // The same rows, and now a picture.
        let png = try #require(ScriptedAppIcons.png(for: app))
        #expect(rig.state.store.setIcon(png, for: app))
        await rig.model.syncAndroidAppsIndex()
        #expect(rig.index.offered.count == offers + 1)
        #expect(rig.index.shortcutUpdates == tiles + 1)
        // Nothing changed since: nothing is written.
        await rig.model.syncAndroidAppsIndex()
        #expect(rig.index.offered.count == offers + 1 && rig.index.shortcutUpdates == tiles + 1)
    }

    @Test func aSwitchTurnedOffBeforeTheOfferWasAcknowledgedIsWithdrawnAtTheNextLaunch() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        // Search was off and the index was told so: the saved state is "withdrawn".
        rig.model.setAndroidAppsInSearch(false)
        #expect(await eventually { rig.index.withdrawn == 1 })
        // The person turns it on; the offer is under way...
        rig.index.holdsNextOffer = true
        rig.model.setAndroidAppsInSearch(true)
        #expect(await eventually { rig.index.events.last == "offer-started" })
        // ...and the person turns it off again before the offer is acknowledged and the process ends.
        rig.model.setAndroidAppsInSearch(false)
        // The next launch: a fresh writer over the same saved preference.
        AndroidAppsHub.shared.searchIndex = rig.index
        await AndroidAppsHub.shared.writer.reconcile()
        #expect(rig.index.withdrawn == 2, "Spotlight may hold what the lost offer wrote: the launch withdraws it")
        // The abandoned offer ends with the test, not after it.
        rig.index.letGo()
        try await Task.sleep(for: .milliseconds(150))
    }

    @available(iOS 27.0, *)
    @Test func aFullReindexThatDeletedAndFailedToInsertIsRepairedAtTheNextLaunch() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let query = AndroidAppQuery()
        // The system asks for every app again; the index takes the delete and not the insert.
        rig.index.accepts = false
        var reported = false
        do {
            try await query.reindexAllEntities(indexDescription: CSSearchableIndexDescription())
        } catch {
            reported = true
        }
        #expect(reported, "the failure reaches the system, which asks again")
        rig.index.accepts = true
        let offers = rig.index.offered.count
        await AndroidAppsHub.shared.writer.reconcile()
        #expect(rig.index.offered.count == offers + 1, "the next launch offers the apps again: Spotlight holds none")
    }

    @available(iOS 27.0, *)
    @Test func aSelectiveAddQueuedBehindAWithdrawalIsWithdrawnWhenTheSwitchEndsOff() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        // Search goes off: the withdrawal is under way at the system and held there.
        rig.index.holdsNextWithdraw = true
        rig.model.setAndroidAppsInSearch(false)
        #expect(await eventually { rig.index.events.last == "withdraw-started" })
        // The system asks for the app again meanwhile (queued behind the withdrawal), and the person turns Search on.
        let query = AndroidAppQuery()
        let asking = Task { try? await query.reindexEntities(for: [comet.id], indexDescription: CSSearchableIndexDescription()) }
        try await Task.sleep(for: .milliseconds(50))
        rig.model.setAndroidAppsInSearch(true)
        rig.index.holdsNextAdd = true
        rig.index.letGoWithdraw()
        #expect(await eventually { rig.index.events.last == "add-started" })
        // The add is under way when the person turns Search off again.
        rig.model.setAndroidAppsInSearch(false)
        rig.index.letGoAdd()
        _ = await asking.value
        // Spotlight ends without the app: what the saved state said before the add is not what it holds now.
        #expect(await eventually { rig.index.events.last == "withdraw" }, "\(rig.index.events)")
    }

    @available(iOS 27.0, *)
    @Test func artworkThatArrivesWhileAFullReindexIsOfferedIsOfferedAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        let app = AndroidApp(component: comet.component, label: "Comet Hop", builtIn: false, iconTag: "00000000000000aa")
        try rig.state.store.apply(AndroidAppList(apps: [app], digest: "d"), at: Date())
        rig.state.adoptCache()
        await rig.model.syncAndroidAppsIndex()
        // The system asks for every app again; the entities are made without a picture...
        rig.index.holdsNextOffer = true
        let query = AndroidAppQuery()
        let reindexing = Task { try? await query.reindexAllEntities(indexDescription: CSSearchableIndexDescription()) }
        #expect(await eventually { rig.index.events.last == "offer-started" })
        // ...and the picture arrives while the offer is on its way.
        let png = try #require(ScriptedAppIcons.png(for: app))
        #expect(rig.state.store.setIcon(png, for: app))
        rig.index.letGo()
        _ = await reindexing.value
        // What Spotlight holds has no picture, so the next pass offers the apps again.
        let offers = rig.index.offered.count
        await rig.model.syncAndroidAppsIndex()
        #expect(rig.index.offered.count == offers + 1)
    }

    // MARK: Erase and restore

    @Test func aTapDuringAnEraseStartsNothingAndWritesNothing() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        rig.model.busy = BusyState(title: "Erasing virtual machine", detail: "Deleting.", progress: 0)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.driver.begun.isEmpty, "no start races the erase")
        #expect(rig.state.open.pending == nil && rig.state.open.banner == nil)
        #expect(rig.model.alert == nil)
        // The erase ends; the tap is not remembered and nothing starts by itself.
        rig.model.busy = nil
        rig.model.refreshPlan()
        try await Task.sleep(for: .milliseconds(50))
        #expect(rig.driver.begun.isEmpty && rig.state.open.pending == nil)
        // A tap after the erase finds no virtual machine and says so as ever.
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .starting)
    }

    @Test func aRequestHeldForARestoreStartsOnlyOnceTheRestoredGuestIsAdopted() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        let item = SnapshotSummary(
            id: SnapshotID(), name: "Before", created: Date(), releaseID: "20261109-los24-r20", buildNumber: 1, diskGiB: 16,
            restore: .asIs, kept: SnapshotFigure(bytes: 7_000_000_000), freesWhenDeleted: SnapshotFigure(bytes: 4096))
        let fake = FakeSnapshotDriver(items: [item])
        rig.model.snapshotDriver = fake
        fake.holding = .restore
        let restoring = Task { await rig.model.restoreSnapshot(item.id) }
        #expect(await eventually { fake.activity.operation == .restore })
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        guard case .waiting(.snapshot, _)? = rig.state.open.pending?.stage else {
            Issue.record("the request should wait for the restore: \(String(describing: rig.state.open.pending))")
            return
        }
        // The files say what a start is now (the restore left an update to apply).
        fake.onRestore = { rig.driver.plan = .update(from: "r1", to: "r2") }
        fake.release()
        _ = await restoring.value
        #expect(rig.driver.begun.count == 1, "the held request starts Android once")
        #expect(rig.driver.begunGuests.first?.usedBytes == 7_000_000_000, "with the guest the restore left")
        #expect(rig.model.plan == .update(from: "r1", to: "r2"), "and the plan the files now say")
        #expect(rig.model.lifecycle.guest?.usedBytes == 7_000_000_000)
    }

    @Test func aRestoreThatRefusesStillReleasesTheHeldRequest() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        let item = SnapshotSummary(
            id: SnapshotID(), name: "Before", created: Date(), releaseID: "20261109-los24-r20", buildNumber: 1, diskGiB: 16,
            restore: .asIs, kept: SnapshotFigure(bytes: 4096), freesWhenDeleted: SnapshotFigure(bytes: 4096))
        let fake = FakeSnapshotDriver(items: [item])
        rig.model.snapshotDriver = fake
        fake.holding = .restore
        fake.nextRefusal = .missing
        let restoring = Task { await rig.model.restoreSnapshot(item.id) }
        #expect(await eventually { fake.activity.operation == .restore })
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        fake.release()
        _ = await restoring.value
        #expect(rig.driver.begun.count == 1)
        #expect(!rig.state.restoring)
    }

    @Test func aTapBetweenTheUnlockAndTheAdoptionOfARestoredGuestWaitsForTheAdoption() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        let item = SnapshotSummary(
            id: SnapshotID(), name: "Before", created: Date(), releaseID: "20261109-los24-r20", buildNumber: 1, diskGiB: 16,
            restore: .asIs, kept: SnapshotFigure(bytes: 7_000_000_000), freesWhenDeleted: SnapshotFigure(bytes: 4096))
        let fake = FakeSnapshotDriver(items: [item])
        rig.model.snapshotDriver = fake
        // The runtime has unlocked the guest's files and the result is on its way to the model: the tap lands now.
        var atTheTap: AppOpenStage?
        let component = comet.component
        fake.atUnlock = { [weak model = rig.model] in
            model?.requestAndroidApp(component, label: "Comet Hop")
            atTheTap = model?.androidApps.open.pending?.stage
        }
        fake.onRestore = { rig.driver.plan = .update(from: "r1", to: "r2") }
        let result = await rig.model.restoreSnapshot(item.id)
        guard case .done = result else { Issue.record("the restore should be done: \(result)"); return }
        if case .waiting(.snapshot, _)? = atTheTap {} else { Issue.record("the tap should wait for the adoption: \(String(describing: atTheTap))") }
        #expect(rig.driver.begun.count == 1, "the held request starts Android once")
        #expect(rig.driver.begunGuests.first?.usedBytes == 7_000_000_000, "with the guest the restore left, not the one before it")
        #expect(rig.model.plan == .update(from: "r1", to: "r2"), "and under the plan the files now say")
    }

    // MARK: Space and readiness

    @Test func aPauseForSpaceWhileAndroidIsMadeReadyMakesTheRequestWaitForTheAnswer() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .milliseconds(300)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .opening)
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        guard case .waiting(.space, _)? = rig.state.open.pending?.stage else {
            Issue.record("the request should wait for space: \(String(describing: rig.state.open.pending))")
            return
        }
        try await Task.sleep(for: .milliseconds(450))
        #expect(rig.tool.opened.isEmpty, "nothing is launched into a paused guest")
        // The person makes room and the guest resumes: the request is decided again and opens.
        rig.model.spacePause = nil
        #expect(await eventually { rig.tool.opened == [comet.component] })
    }

    @Test func aRunThatPausedBeforeTheLaunchIsNotLaunchedInto() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.scripted.readyDelay = .milliseconds(200)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        // The pause starts without the model's own hook (the readiness wait is what it raced).
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .paused)
        try await Task.sleep(for: .milliseconds(400))
        #expect(rig.tool.opened.isEmpty)
        #expect(rig.state.open.pending != nil)
    }

    @Test func aSecondAppOpensWhileTheFirstIsInFrontWithoutWaitingForHomeAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.scripted.requiresHomeResumed = true
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually(3) { rig.tool.opened == [comet.component] })
        // The first app is resumed now: Home is not the activity in front.
        rig.scripted.homeInFront = false
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually(3) { rig.tool.opened == [comet.component, inkleaf.component] }, "one launch of the second app")
        #expect(rig.state.open.banner == nil, "no timeout")
        // A new run's boot is begun by a request: that request waits for Home, and an app in front keeps it from coming.
        rig.model.androidAppsRunEnded()
        rig.model.phase = .home
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually(3) { rig.state.open.banner?.failure == .tookTooLong })
        #expect(rig.tool.opened == [comet.component, inkleaf.component])
    }

    @Test func theFirstOpenOfARunPastItsBootDoesNotWaitForHome() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.scripted.requiresHomeResumed = true
        // An app is in front, and no request of this run has asked anything: the run's boot completed, nobody waited for it.
        rig.scripted.homeInFront = false
        rig.model.androidAppsBootFinished(recovery: false)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually(3) { rig.tool.opened == [comet.component] }, "the first open launches without waiting for Home")
        #expect(rig.state.open.banner == nil)
        #expect(rig.scripted.homeReachedAsked == [true])
    }

    @Test func aRequestThatStartedTheBootStillWaitsForHomesFirstResume() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        defer { rig.stop() }
        rig.scripted.requiresHomeResumed = true
        rig.scripted.homeInFront = false
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .starting)
        // The boot it started completes; the launcher is not up (something else is in front).
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually(3) { rig.state.open.banner?.failure == .tookTooLong }, "the look for Home was kept for it")
        #expect(rig.tool.opened.isEmpty)
        #expect(rig.scripted.homeReachedAsked == [false])
        // The boot is over for the requests that come later (Try Again is one), with Home in front or not.
        rig.scripted.homeInFront = true
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually(3) { rig.tool.opened == [comet.component] })
        rig.scripted.homeInFront = false
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually(3) { rig.tool.opened == [comet.component, inkleaf.component] })
        #expect(rig.scripted.homeReachedAsked == [false, true, true])
    }

    @Test func aRequestAfterTheBootDoesNotWaitForHomeWhenTheRequestThatBeganItStoppedAtSetup() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        defer { rig.stop() }
        rig.scripted.requiresHomeResumed = true
        rig.scripted.readiness = .setupUnfinished
        // Icon A starts Android; the boot completes and Android's own setup is still on the screen.
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .starting)
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually(3) { rig.state.open.banner?.failure == .setupUnfinished })
        // The setup is finished, with another app in front. Icon B opens at once: the boot is over.
        rig.scripted.readiness = .ready
        rig.scripted.homeInFront = false
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually(3) { rig.tool.opened == [inkleaf.component] }, "icon B launches without waiting for Home")
        #expect(rig.state.open.banner == nil)
        #expect(rig.scripted.homeReachedAsked == [false, true])
    }

    @Test func aLaunchAlreadySentIsNotSentAgainByAPauseAndAResume() async throws {
        let rig = try rig(phase: .running(.normal))
        // The command is out and its reply is slow.
        rig.tool.delay = .milliseconds(300)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually(3) { rig.tool.opened == [comet.component] }, "the tool received the command")
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        try await Task.sleep(for: .milliseconds(450))
        rig.model.spacePause = nil
        try await Task.sleep(for: .milliseconds(450))
        #expect(rig.tool.opened == [comet.component], "exactly one launch")
        #expect(rig.state.open.pending == nil, "the reply ended the request")
        #expect(rig.state.open.banner == nil)
    }

    // MARK: The run's first listing

    @Test func theRunsFirstListingWaitsForTheGuestToSettleAfterItsBootCompleted() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        // The one number: the guest is given a minute after Android reports its boot done.
        #expect(rig.state.startDelay == .seconds(60))
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(rig.tool.digestsAsked == 0, "nothing is asked as the boot ends")
        #expect(await eventually { rig.waits.durations == [.seconds(60)] }, "the first ask waits the minute")
        #expect(rig.tool.digestsAsked == 0)
        rig.waits.releaseAll()
        #expect(await eventually { rig.state.listed })
        #expect(rig.tool.digestsAsked == 1)
    }

    @Test func aHostWithNoHomeScreenNeverAsksAndroidForTheApps() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(rig.state.first == nil, "no task waits to ask")
        // No other asker gets an answer either: an install, a request that found its app gone, a page.
        #expect(await rig.model.refreshAndroidApps() == false)
        #expect(await rig.model.refreshAndroidApps(afterAChange: true) == false)
        #expect(rig.tool.digestsAsked == 0 && rig.tool.listingsAsked == 0)
        #expect(!rig.state.listed && !rig.state.refreshing)
        #expect(rig.waits.durations.isEmpty)
    }

    /// An iOS before 27 has a Home Screen and no tile in Shortcuts to add an app to it from, so it
    /// takes the path of a host with no Home Screen (`hostOffersAddToHomeScreen` is false).
    @Test func anIOSBefore27NeverAsksAndroidForTheApps() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.playedPadAppHost = IPadAppHost(
            isiOSAppOnMac: false, isiOSAppOnVision: false, hasAppTilesInShortcuts: false)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(rig.state.first == nil, "no task waits to ask")
        #expect(await rig.model.refreshAndroidApps() == false)
        #expect(await rig.model.refreshAndroidApps(afterAChange: true) == false)
        #expect(rig.tool.digestsAsked == 0 && rig.tool.listingsAsked == 0)
        #expect(!rig.state.listed && !rig.state.refreshing)
        #expect(rig.waits.durations.isEmpty)
    }

    @Test func anIPadAsksAsAnIPhoneDoes() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .pad
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually { rig.waits.holding == 1 })
        rig.waits.releaseAll()
        #expect(await eventually(5) { rig.state.listed })
    }

    @Test func theChoosersListingDoesNotWaitForTheGuestToSettle() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.androidAppsBootFinished(recovery: false)
        // The person opens Add to Home Screen while the guest is still settling: it is asked at once.
        #expect(await rig.model.refreshAndroidApps())
        #expect(rig.state.listed && rig.state.apps.count == ScriptedAndroidApps.sampleApps.count)
        #expect(rig.tool.digestsAsked >= 1)
    }

    @Test func aListingTheChooserMadeIsNotAskedForAgainWhenTheWaitIsOver() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.androidAppsBootFinished(recovery: false)
        // The run's first listing is the task's: take its handle before anything yields.
        let waiting = try #require(rig.state.first)
        #expect(await rig.model.refreshAndroidApps())
        let asked = rig.tool.digestsAsked
        #expect(await eventually { rig.waits.holding == 1 })
        rig.waits.releaseAll()
        #expect(await ends(waiting), "the wait ends")
        #expect(rig.tool.digestsAsked == asked, "the run's list was answered; the wait asks nothing more")
        #expect(rig.state.first == nil)
    }

    @Test func anEarlierRunsTaskDoesNotClearTheNextRunsTimerNorAskItsGuest() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        // Run A: its wait is over and its listing is under way, held at the write to the index.
        rig.index.holdsNextOffer = true
        rig.model.androidAppsBootFinished(recovery: false)
        let a = try #require(rig.state.first)
        #expect(await eventually { rig.waits.holding == 1 })
        rig.waits.releaseAll()
        #expect(await eventually { rig.index.events.contains("offer-started") })
        // Run B begins while A's reply is held; B's chooser lists.
        rig.model.androidAppsRunEnded()
        rig.model.session = SessionState()
        rig.model.phase = .running(.normal)
        rig.model.session.storageReady = true
        rig.model.androidAppsBootFinished(recovery: false)
        let b = try #require(rig.state.first)
        #expect(await eventually { rig.waits.holding == 1 })
        // (The write to the index is ordered behind A's, which is held: B's listing is kept, its write waits.)
        let chooser = Task { await rig.model.refreshAndroidApps() }
        #expect(await eventually { rig.state.listed })
        // A's reply comes: A ends, and the timer is still B's.
        rig.index.letGo()
        #expect(await ends(a), "A's task ends")
        _ = await chooser.value
        #expect(rig.state.first == b, "A cleared the handle of B's timer")
        // Run C begins during B's wait: B's wait ends with B, and C's guest is asked by C's timer alone.
        rig.model.androidAppsRunEnded()
        rig.model.session = SessionState()
        rig.model.phase = .running(.normal)
        rig.model.session.storageReady = true
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually(3) { rig.waits.holding == 1 && rig.waits.durations.count == 3 }, "waits: \(rig.waits.holding) of \(rig.waits.durations)")
    }

    @Test func aListingMadeDuringTheStartupStorageCheckIsNotAskedAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.driver.holdsStorageReads = true
        rig.model.androidAppsBootFinished(recovery: false)
        let first = try #require(rig.state.first)
        #expect(await eventually { rig.waits.holding == 1 })
        rig.waits.releaseAll()
        #expect(await eventually { rig.driver.storageReads > 0 }, "the task is at the storage check")
        // The person opens Add to Home Screen meanwhile.
        #expect(await rig.model.refreshAndroidApps())
        let asked = rig.tool.digestsAsked
        rig.driver.holdsStorageReads = false
        #expect(await ends(first), "the task ends")
        #expect(rig.tool.digestsAsked == asked, "the run's list was answered during the check")
    }

    @Test func anUncachedShortcutThatStartedAndroidAsksForTheListAtOnceWhileItsAppOpens() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        defer { rig.stop() }
        rig.scripted.readyDelay = .milliseconds(300)
        // The cache has no list: nothing says that Comet Hop is in Android.
        #expect(rig.state.store.catalog() == nil)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .starting)
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(rig.state.open.pending?.stage == .opening)
        // The list is asked for, with the app still opening and no wait let go.
        #expect(await eventually(3) { rig.tool.digestsAsked > 0 }, "waits held: \(rig.waits.durations)")
        #expect(!rig.waits.durations.contains(rig.state.startDelay))
        #expect(await eventually(5) { rig.tool.opened == [comet.component] })
        #expect(await eventually(5) { rig.state.listed }, "the list the shortcut needed was made")
    }

    @Test func anUncachedStartersFirstListingSurvivesStorageThatWasDownAndAnOpenStillUnderWay() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        defer { rig.stop() }
        // The app stays opening for the whole test, and the guest's storage is not up at the first check.
        rig.scripted.readyDelay = .seconds(30)
        rig.driver.storage = false
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(rig.state.open.pending?.stage == .opening)
        #expect(await eventually { rig.driver.storageReads >= 1 }, "the first check was made")
        #expect(rig.tool.digestsAsked == 0, "storage was down")
        // Storage comes up; every retry finds the app still opening, and the list is still owed.
        rig.driver.storage = true
        let asked = await eventually(4) {
            rig.waits.releaseAll()
            return rig.tool.digestsAsked > 0
        }
        #expect(asked, "the list the request needs is asked at a retry, with the app still opening")
        #expect(rig.state.open.pending?.stage == .opening)
        #expect(await eventually { rig.state.listed })
    }

    @Test func anOpenUnderWayAtEveryRetryDoesNotSpendTheFirstListing() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.scripted.readyDelay = .seconds(1)
        rig.model.androidAppsBootFinished(recovery: false)
        // An app is opened while the guest settles, and takes longer than every retry of the listing together.
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .opening)
        let listed = await eventually(5) {
            rig.waits.releaseAll()
            return rig.state.listed
        }
        #expect(listed, "the listing the open held back is made once the open is over")
        #expect(rig.tool.opened == [comet.component])
    }

    @Test func theListingAMissingAppsOpenMakesDoesNotOverlapTheStartersCatalogListing() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        defer { rig.stop() }
        let gone = try #require(AndroidComponent(packageName: "com.example.gone", activityClass: "com.example.gone.Main"))
        rig.tool.delay = .milliseconds(120)
        rig.scripted.readyDelay = .milliseconds(40)
        // The cache has no list, and the app the request began the boot for is not in Android.
        rig.model.requestAndroidApp(gone, label: "Gone")
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually(8) { rig.state.open.banner != nil }, "the missing-app answer arrived")
        #expect(await eventually(8) { rig.state.listed })
        #expect(rig.tool.mostListingsAtOnce == 1, "the guest is asked for one listing at a time")
    }

    @Test func aShortcutDuringABootTheStartButtonBeganWaitsForTheGuestToSettle() async throws {
        let rig = try rig(phase: .home, plan: .normal)
        defer { rig.stop() }
        rig.model.start(.normal)
        // Android is starting, and a shortcut for an app the list lacks arrives during the boot.
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .starting)
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually { rig.waits.durations == [rig.state.startDelay] }, "waits: \(rig.waits.durations)")
        #expect(rig.tool.digestsAsked == 0)
    }

    @Test func aShortcutForAnAppTheListHasLeavesTheListForTheEndOfTheWait() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        await rig.model.refreshAndroidApps()
        let baseline = rig.tool.digestsAsked
        // A new run: the cache keeps its list, and this run has listed nothing yet.
        rig.model.androidAppsRunEnded()
        rig.model.phase = .home
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(rig.state.open.pending?.stage == .starting)
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(await eventually(5) { rig.tool.opened == [comet.component] })
        #expect(await eventually { rig.waits.durations == [rig.state.startDelay] })
        #expect(rig.tool.digestsAsked == baseline, "the list the cache holds is enough until the wait is over")
        rig.waits.releaseAll()
        #expect(await eventually { rig.tool.digestsAsked > baseline })
    }

    @Test func aPauseForSpaceBeforeTheBootScreenEndsStillOwesTheFirstListing() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(rig.state.first == nil, "nothing is asked while Android is paused")
        rig.model.spacePause = nil
        #expect(await eventually { rig.waits.durations == [rig.state.startDelay] }, "the pause ending starts the wait: \(rig.waits.durations)")
        rig.waits.releaseAll()
        #expect(await eventually { rig.state.listed })
    }

    @Test func aPauseForSpaceDuringTheWaitKeepsTheFirstListingOwed() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.androidAppsBootFinished(recovery: false)
        let first = try #require(rig.state.first)
        #expect(await eventually { rig.waits.holding == 1 })
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        rig.waits.releaseAll()
        #expect(await ends(first), "the task ends when Android is paused")
        #expect(rig.tool.digestsAsked == 0, "Android is paused when the wait ends")
        rig.model.spacePause = nil
        #expect(await eventually { rig.waits.durations == [rig.state.startDelay, rig.state.startDelay] }, "waits: \(rig.waits.durations)")
        rig.waits.releaseAll()
        #expect(await eventually { rig.state.listed })
    }

    @Test func aRefreshOwedForAnInstallSurvivesAMissingAppAnswerDuringAPause() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        await rig.model.refreshAndroidApps()
        let gone = try #require(AndroidComponent(packageName: "com.example.gone", activityClass: "com.example.gone.Main"))
        // The launch of an app that is not there is under way, listing the apps to learn why.
        rig.tool.delay = .milliseconds(150)
        rig.model.requestAndroidApp(gone, label: "Gone")
        #expect(await eventually { rig.tool.opened == [gone] })
        // Android is paused for space, and an install is announced meanwhile.
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        #expect(await rig.model.refreshAndroidApps(afterAChange: true) == false)
        #expect(await eventually(5) { rig.state.open.banner != nil }, "the missing-app answer arrived")
        rig.tool.delay = .milliseconds(1)
        let asked = rig.tool.digestsAsked
        rig.model.spacePause = nil
        #expect(await eventually(5) { rig.tool.digestsAsked > asked }, "the resume lists what the install changed")
    }

    @Test func anExplicitOpenOnAHostWithNoHomeScreenTakesOneListingWithNoIconsEachTime() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(rig.tool.listingsAsked == 1 && rig.tool.digestsAsked == 0 && rig.tool.iconsAsked == 0, "one listing, no digest, no icons")
        #expect(rig.state.store.record(forID: comet.id)?.launchers == 1, "the listing is recorded")
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually { rig.tool.opened == [comet.component, inkleaf.component] })
        #expect(rig.tool.listingsAsked == 2 && rig.tool.digestsAsked == 0, "one listing at every open")
        #expect(rig.waits.durations.isEmpty, "nothing is asked at the boot")
    }

    @Test func anExplicitOpenOnAnIOSBefore27TakesOneListingWithNoIconsEachTime() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.playedPadAppHost = IPadAppHost(
            isiOSAppOnMac: false, isiOSAppOnVision: false, hasAppTilesInShortcuts: false)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(rig.tool.listingsAsked == 1 && rig.tool.digestsAsked == 0 && rig.tool.iconsAsked == 0, "one listing, no digest, no icons")
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually { rig.tool.opened == [comet.component, inkleaf.component] })
        #expect(rig.tool.listingsAsked == 2 && rig.tool.digestsAsked == 0, "one listing at every open")
        #expect(rig.waits.durations.isEmpty, "nothing is asked at the boot")
    }

    @Test func anAppThatRenamedItsLauncherIsStillOpenedOnAHostWithNoHomeScreen() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] && rig.state.open.pending == nil })
        // The app renames its only launcher.
        let renamed = AndroidApp(
            component: try #require(AndroidComponent(packageName: comet.component.packageName, activityClass: "com.example.comethop.Renamed")),
            label: "Comet Hop", builtIn: false, iconTag: "0000000000000001")
        rig.tool.remove(comet.id)
        rig.tool.install(renamed)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened.last == renamed.component }, "opened: \(rig.tool.opened)")
        #expect(rig.state.open.banner == nil)
    }

    @Test func anAppInstalledAfterTheRunsListingKeepsItsLauncherHistoryOnAHostWithNoHomeScreen() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] && rig.state.open.pending == nil })
        // An app is installed after the run's listing, and the person opens it by name.
        let newcomer = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.newcomer", activityClass: "com.example.newcomer.Main")),
            label: "Newcomer", builtIn: false, iconTag: "0000000000000001")
        rig.tool.install(newcomer)
        rig.model.requestAndroidApp(newcomer.component, label: "Newcomer")
        #expect(await eventually { rig.tool.opened.last == newcomer.component && rig.state.open.pending == nil })
        #expect(rig.state.store.record(forID: newcomer.id)?.launchers == 1, "its launcher is on record")
        // It renames its only launcher, and the original intent is run again.
        let renamed = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.newcomer", activityClass: "com.example.newcomer.Renamed")),
            label: "Newcomer", builtIn: false, iconTag: "0000000000000001")
        rig.tool.remove(newcomer.id)
        rig.tool.install(renamed)
        rig.model.requestAndroidApp(newcomer.component, label: "Newcomer")
        #expect(await eventually { rig.tool.opened.last == renamed.component }, "opened: \(rig.tool.opened)")
        #expect(rig.state.open.banner == nil)
    }

    @Test func aListingThatGotNoAnswerForAnOpenIsNotCountedAsDone() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        rig.tool.answer(listing: .failed)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] && rig.state.open.pending == nil })
        #expect(rig.tool.listingsAsked == 1 && rig.state.failed && rig.state.store.record(forID: comet.id) == nil)
        // The tool answers again, and the next open lists.
        rig.tool.answer(listing: nil)
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually { rig.tool.opened == [comet.component, inkleaf.component] && rig.state.open.pending == nil })
        #expect(rig.tool.listingsAsked == 2, "the next open lists again")
        #expect(rig.state.store.record(forID: inkleaf.id)?.launchers == 1)
    }

    @Test func aReplacementOpenWaitsForTheListingTheOpenItReplacedBegan() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        let gone = try #require(AndroidComponent(packageName: "com.example.gone", activityClass: "com.example.gone.Main"))
        rig.tool.delay = .milliseconds(120)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.state.inFlight != nil }, "the first open's listing is under way")
        // Another request replaces it, for an app that is not in Android: its launch lists the apps to learn why.
        rig.model.requestAndroidApp(gone, label: "Gone")
        #expect(await eventually(8) { rig.state.open.banner != nil }, "the missing-app answer arrived")
        #expect(rig.tool.mostListingsAtOnce == 1, "the guest is asked for one listing at a time")
    }

    @Test func aReinstalledAppsLauncherCountIsItsOwnAndNotTheRetainedOne() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        func twin(_ activity: String) throws -> AndroidApp {
            AndroidApp(
                component: try #require(AndroidComponent(packageName: "com.example.twin", activityClass: "com.example.twin." + activity)),
                label: "Twin", builtIn: false, iconTag: "0000000000000001")
        }
        let main = try twin("Main")
        let second = try twin("Second")
        // The app is listed with two launchers, then uninstalled, and Android restarts.
        rig.tool.install(main)
        rig.tool.install(second)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] && rig.state.open.pending == nil })
        #expect(rig.state.store.record(forID: main.id)?.launchers == 2)
        rig.tool.remove(main.id)
        rig.tool.remove(second.id)
        rig.model.androidAppsRunEnded()
        // Another app is opened, so the current catalog omits it; the record of the old one is retained.
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component, comet.component] && rig.state.open.pending == nil })
        #expect(rig.state.store.catalog()?.app(withID: main.id) == nil)
        #expect(rig.state.store.record(forID: main.id)?.launchers == 2, "the retained record")
        // It is reinstalled with one launcher and opened by name, then renames that launcher.
        rig.tool.install(main)
        rig.model.requestAndroidApp(main.component, label: "Twin")
        #expect(await eventually { rig.tool.opened.last == main.component && rig.state.open.pending == nil })
        #expect(rig.state.store.record(forID: main.id)?.launchers == 1, "its count is the catalog's now")
        let renamed = try twin("Renamed")
        rig.tool.remove(main.id)
        rig.tool.install(renamed)
        rig.model.requestAndroidApp(main.component, label: "Twin")
        #expect(await eventually { rig.tool.opened.last == renamed.component }, "opened: \(rig.tool.opened)")
        #expect(rig.state.open.banner == nil)
    }

    @Test func aFailedRecordsWriteIsMadeAgainByTheNextOpenOnAHostWithNoHomeScreen() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        // The records cannot be written (a folder is where the file goes), and the store lives on.
        let records = rig.directory.appendingPathComponent("records.json")
        try FileManager.default.createDirectory(at: records, withIntermediateDirectories: true)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] && rig.state.open.pending == nil })
        #expect(rig.state.failed, "the write failed")
        try FileManager.default.removeItem(at: records)
        // The next open lists again, and the records reach the disk.
        rig.model.requestAndroidApp(inkleaf.component, label: "Inkleaf Notes")
        #expect(await eventually { rig.tool.opened == [comet.component, inkleaf.component] && rig.state.open.pending == nil })
        #expect(AndroidAppStore(directory: rig.directory).record(forID: comet.id)?.launchers == 1, "the count is on disk")
        // The app renames its only launcher, and the original intent still finds it.
        let renamed = AndroidApp(
            component: try #require(AndroidComponent(packageName: comet.component.packageName, activityClass: "com.example.comethop.Renamed")),
            label: "Comet Hop", builtIn: false, iconTag: "0000000000000001")
        rig.tool.remove(comet.id)
        rig.tool.install(renamed)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened.last == renamed.component }, "opened: \(rig.tool.opened)")
        #expect(rig.state.open.banner == nil)
    }

    @Test func anOpenOnAHostWithNoHomeScreenResolvesAnAppOfTheCurrentCatalogWhateverTheHistoryHolds() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        rig.model.host.idiom = .mac
        // The history is at its bound and dated after now (the clock went back).
        let history = try (0..<AndroidAppLimits.mostRecords).map { index in
            AndroidApp(
                component: try #require(AndroidComponent(packageName: "com.example.history\(index)", activityClass: "com.example.history\(index).Main")),
                label: "History \(index)", builtIn: false, iconTag: "0000000000000001")
        }
        try rig.state.store.apply(AndroidAppList(apps: history, digest: "history"), at: Date().addingTimeInterval(86_400))
        #expect(rig.state.store.records().count == AndroidAppLimits.mostRecords)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened == [comet.component] && rig.state.open.pending == nil })
        #expect(rig.state.open.banner == nil)
        #expect(rig.state.store.record(forID: comet.id)?.launchers == 1, "an app of the current catalog keeps its record")
        #expect(rig.state.store.records().count == AndroidAppLimits.mostRecords)
    }

    @Test func aCancelledAskWaitingForTheLaneIsReleasedWhenTheRunEnds() async throws {
        let rig = try rig(phase: .running(.normal))
        let gone = try #require(AndroidComponent(packageName: "com.example.gone", activityClass: "com.example.gone.Main"))
        defer {
            rig.tool.holdsDigests = false
            rig.stop()
        }
        // The run's catalog listing is under way, and the guest's reply to it is held.
        rig.tool.holdsDigests = true
        let catalog = Task { await rig.model.refreshAndroidApps() }
        #expect(await eventually { rig.tool.digestsAsked == 1 })
        // A launch of an app that is not there lists the apps to learn why: that ask waits its turn.
        rig.model.requestAndroidApp(gone, label: "Gone")
        #expect(await eventually { rig.tool.opened == [gone] })
        try await Task.sleep(for: .milliseconds(100))
        let launch = try #require(rig.state.opening)
        // The run ends with the reply still held: the ask that waited is released at once.
        rig.model.androidAppsRunEnded()
        #expect(await ends(launch, within: 3), "the cancelled ask stayed in the queue")
        rig.tool.holdsDigests = false
        _ = await catalog.value
    }

    @Test func theCachedListAndRecordsAnswerBeforeTheRunsFirstListing() async throws {
        let rig = try rig(phase: .running(.normal))
        defer { rig.stop() }
        await rig.model.refreshAndroidApps()
        // Android restarts, and then Android is erased: the cache goes and the record of each icon stays.
        rig.model.androidAppsRunEnded()
        rig.model.androidAppsErased()
        rig.model.phase = .running(.normal)
        rig.model.androidAppsBootFinished(recovery: false)
        #expect(!rig.state.listed, "the run's first listing is still to come")
        if #available(iOS 27.0, *) {
            let query = AndroidAppQuery()
            #expect(try await query.entities(for: [comet.id]).map(\.name) == ["Comet Hop"])
        }
        #expect(rig.model.requestAndroidApp(identifier: comet.id, label: "Comet Hop"))
        #expect(await eventually { rig.tool.opened == [comet.component] })
        #expect(rig.state.open.banner == nil)
    }

    // MARK: Refresh ownership

    @Test func anEraseDuringARefreshLeavesNothingOfIt() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.delay = .milliseconds(40)
        let refreshing = Task { await rig.model.refreshAndroidApps() }
        #expect(await eventually { rig.state.refreshing })
        rig.model.androidAppsErased()
        _ = await refreshing.value
        try await Task.sleep(for: .milliseconds(300))
        #expect(rig.state.apps.isEmpty && rig.state.store.catalog() == nil)
        #expect(!rig.state.refreshing && rig.state.iconsLeft == 0)
        #expect(AndroidAppsHub.shared.listedApps().isEmpty)
    }

    @Test func aRefreshAfterAnInstallWaitsForTheOneUnderWayAndListsAgain() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.delay = .milliseconds(30)
        let first = Task { await rig.model.refreshAndroidApps() }
        #expect(await eventually { rig.state.inFlight != nil })
        let extra = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.newcomer", activityClass: "com.example.newcomer.Main")),
            label: "Newcomer", builtIn: false, iconTag: "0000000000000042")
        rig.tool.install(extra)
        await rig.model.refreshAndroidApps(afterAChange: true)
        _ = await first.value
        #expect(rig.state.apps.contains { $0.id == extra.id })
    }

    @Test func anInstallThatLandsAfterTheListWasCapturedIsListedByTheRefreshThatFollows() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.delay = .milliseconds(40)
        let first = Task { await rig.model.refreshAndroidApps() }
        // The refresh has captured the list it will keep...
        #expect(await eventually { rig.tool.listingsAsked >= 1 })
        // ...and then an app is installed, and the install asks for a refresh after it.
        let extra = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.latecomer", activityClass: "com.example.latecomer.Main")),
            label: "Latecomer", builtIn: false, iconTag: "0000000000000043")
        rig.tool.install(extra)
        await rig.model.refreshAndroidApps(afterAChange: true)
        _ = await first.value
        #expect(await eventually { rig.state.apps.contains { $0.id == extra.id } && !rig.state.refreshing })
        #expect(AndroidAppsHub.shared.listedApps().contains { $0.id == extra.id })
    }

    @Test func anInstallThatLandsAfterTheListWasCapturedIsListedOnceAPauseForSpaceEnds() async throws {
        let rig = try rig(phase: .running(.normal))
        rig.tool.delay = .milliseconds(40)
        // The system takes the first refresh's offer slowly.
        rig.index.holdsNextOffer = true
        let first = Task { await rig.model.refreshAndroidApps() }
        #expect(await eventually { rig.tool.listingsAsked >= 1 })
        // An app is installed after the list was captured, and the install asks for a refresh after it.
        let extra = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.latecomer", activityClass: "com.example.latecomer.Main")),
            label: "Latecomer", builtIn: false, iconTag: "0000000000000043")
        rig.tool.install(extra)
        await rig.model.refreshAndroidApps(afterAChange: true)
        // All guest work of the refresh is done; the offer is under way when Android pauses for space.
        #expect(await eventually { rig.index.events.contains("offer-started") })
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .paused)
        rig.index.letGo()
        _ = await first.value
        try await Task.sleep(for: .milliseconds(150))
        #expect(!rig.state.apps.contains { $0.id == extra.id }, "nothing is asked of a paused guest")
        #expect(rig.tool.listingsAsked == 1)
        // The person makes room and the guest resumes: the refresh that was owed is made.
        rig.model.spacePause = nil
        #expect(await eventually(3) { rig.state.apps.contains { $0.id == extra.id } && !rig.state.refreshing }, "the install is listed")
        #expect(AndroidAppsHub.shared.listedApps().contains { $0.id == extra.id })
    }

    @Test func anInstallCommittedWhileAndroidIsPausedForSpaceIsListedOnceItResumes() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        // No refresh is under way: Android pauses for space, and an install that was committed asks for a refresh.
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .paused)
        let extra = AndroidApp(
            component: try #require(AndroidComponent(packageName: "com.example.latecomer", activityClass: "com.example.latecomer.Main")),
            label: "Latecomer", builtIn: false, iconTag: "0000000000000043")
        rig.tool.install(extra)
        let asked = await rig.model.refreshAndroidApps(afterAChange: true)
        #expect(!asked, "nothing is asked of a paused guest")
        let listings = rig.tool.listingsAsked
        // The person makes room and the guest resumes: the refresh that was owed is made.
        rig.model.spacePause = nil
        #expect(await eventually(3) { rig.state.apps.contains { $0.id == extra.id } && !rig.state.refreshing }, "the install is listed")
        #expect(rig.tool.listingsAsked == listings + 1)
        #expect(AndroidAppsHub.shared.listedApps().contains { $0.id == extra.id })
    }

    // MARK: How an open ends, more

    @Test func anImageWithoutTheToolEndsAnOpenWithItsOwnBannerAndWithdrawsTheApps() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        rig.tool.answer(open: comet.component, .unavailable)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.state.open.banner?.failure == .toolMissing })
        #expect(rig.state.open.banner?.component == nil, "no Try Again")
        #expect(await eventually { rig.state.unavailable && rig.state.apps.isEmpty })
        #expect(AndroidAppsHub.shared.listedApps().isEmpty)
    }

    @Test func aRenamedLauncherOpensOnlyWhereThePackageHadExactlyOne() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        #expect(rig.state.store.record(forID: comet.id)?.launchers == 1)
        // The app renamed its launcher activity: the tool no longer finds the saved one.
        let renamed = AndroidApp(
            component: try #require(AndroidComponent(packageName: comet.component.packageName, activityClass: "com.example.comethop.NewMain")),
            label: "Comet Hop", builtIn: false, iconTag: "0000000000000099")
        rig.tool.remove(comet.id)
        rig.tool.install(renamed)
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        #expect(await eventually { rig.tool.opened.contains(renamed.component) })
        #expect(rig.state.open.banner == nil)
    }

    @Test func aReplacementLaunchNotYetSentWaitsForTheSpaceNoticeToClear() async throws {
        let rig = try rig(phase: .running(.normal))
        await rig.model.refreshAndroidApps()
        let renamed = AndroidApp(
            component: try #require(AndroidComponent(packageName: comet.component.packageName, activityClass: "com.example.comethop.NewMain")),
            label: "Comet Hop", builtIn: false, iconTag: "0000000000000099")
        rig.tool.remove(comet.id)
        rig.tool.install(renamed)
        rig.tool.delay = .milliseconds(300)
        let listings = rig.tool.listingsAsked
        rig.model.requestAndroidApp(comet.component, label: "Comet Hop")
        // The first command was refused and the apps are listed again to learn why...
        #expect(await eventually(3) { rig.tool.listingsAsked == listings + 1 })
        // ...when Android is low on space, the pause does not take, and the space notice stays.
        rig.model.spacePause = SpacePause(freeBytes: 0, stage: .pausing)
        try await Task.sleep(for: .milliseconds(450))
        #expect(rig.tool.opened == [comet.component], "no replacement launch is sent under the notice")
        #expect(rig.state.open.banner == nil)
        // The person makes room and the notice clears: the request is decided again and opens the renamed app.
        rig.tool.delay = .milliseconds(1)
        rig.model.spacePause = nil
        #expect(await eventually(3) { rig.tool.opened.contains(renamed.component) })
        #expect(rig.state.open.banner == nil)
    }
}
