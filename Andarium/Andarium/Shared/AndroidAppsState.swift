// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AppIntents
import CoreSpotlight
import CryptoKit
import Foundation
import ImageIO
import Observation
import UIKit

/// Where Android's apps come from and how one is opened, for Add to Home
/// Screen: the runtime asks the tool in Android's image (`AndroidAppsCommands`)
/// and waits for Android to be ready for an app, and the Simulator preview
/// plays a list. Everything the screens know of the tool is `AndariumCore`'s
/// `AndroidApp`, its icon and the answers of `AndroidAppsTool`.
@MainActor
protocol AndroidAppsSource: AnyObject {
    /// The tool for this run; nil where the run cannot ask it (no adb, Recovery).
    var androidAppsTool: (any AndroidAppsTool)? { get }
    /// Waits until Android is ready for an app to be opened: its boot complete,
    /// its storage up, its home screen resumed. The wait ends early when
    /// Android's own first-run setup is on the screen, and by itself after
    /// `AndroidAppReadiness.limit` of the guest's running time. `homeReached`
    /// says that the home screen's resume is not waited for: the request
    /// neither began the run's boot nor arrived during it, so another app in
    /// front is the normal state.
    func awaitAndroidReady(homeReached: Bool) async -> AndroidAppReadiness.Outcome
}

/// The guest is asked for one listing at a time: the run's refresh, the
/// chooser's, a launch's own look at the apps after a missing-app answer. A
/// second asker waits its turn, in order, and the answer it gets is its own.
/// A task that is cancelled while it waits (the run ended) leaves the queue
/// at once and holds no turn.
@MainActor
final class ListingLane {
    private var held = false
    private var waiting: [(id: Int, turn: CheckedContinuation<Bool, Never>)] = []
    private var issued = 0

    /// Waits for the turn; false where the task was cancelled, and the turn is not held.
    func enter() async -> Bool {
        if Task.isCancelled { return false }
        guard held else {
            held = true
            return true
        }
        issued += 1
        let id = issued
        return await withTaskCancellationHandler {
            await withCheckedContinuation { waiting.append((id, $0)) }
        } onCancel: {
            Task { @MainActor in self.leaveQueue(id) }
        }
    }

    func leave() {
        if waiting.isEmpty {
            held = false
        } else {
            waiting.removeFirst().turn.resume(returning: true)
        }
    }

    private func leaveQueue(_ id: Int) {
        guard let index = waiting.firstIndex(where: { $0.id == id }) else { return }
        waiting.remove(at: index).turn.resume(returning: false)
    }
}

/// The tool, with its digest and listing asked through the lane. The turn is
/// held for one ask, and an ask whose task ended while it waited sends nothing.
nonisolated struct OneListingAtATimeTool: AndroidAppsTool {
    let base: any AndroidAppsTool
    let lane: ListingLane

    func digest(limit: Duration) async -> AndroidAppDigestAnswer {
        guard await lane.enter() else { return .failed }
        var answer = AndroidAppDigestAnswer.failed
        if !Task.isCancelled { answer = await base.digest(limit: limit) }
        await lane.leave()
        return answer
    }

    func list(limit: Duration) async -> AndroidAppListing {
        guard await lane.enter() else { return .failed }
        var answer = AndroidAppListing.failed
        if !Task.isCancelled { answer = await base.list(limit: limit) }
        await lane.leave()
        return answer
    }

    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] {
        await base.icons(of: apps, limit: limit)
    }

    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer {
        await base.open(component, limit: limit)
    }
}

/// The tool, with every icon made one the host drew itself: the picture is
/// decoded whole, at the size the tool says it has, and kept as a PNG this
/// process rendered on an opaque white square, whatever the tool said of its
/// alpha. iOS paints a transparent Home Screen icon black, and a picture that
/// does not decode is no icon (it is asked for again, not kept). The listing
/// and the launching are the tool's.
nonisolated struct FlatteningAppsTool: AndroidAppsTool {
    let base: any AndroidAppsTool

    func digest(limit: Duration) async -> AndroidAppDigestAnswer { await base.digest(limit: limit) }
    func list(limit: Duration) async -> AndroidAppListing { await base.list(limit: limit) }
    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer {
        await base.open(component, limit: limit)
    }

    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] {
        var answers = await base.icons(of: apps, limit: limit)
        for (id, answer) in answers {
            guard case let .png(data, _) = answer else { continue }
            answers[id] = Self.flattened(data).map { .png($0, hasAlpha: false) } ?? .error
        }
        return answers
    }

    /// The most pixels on a side of a picture the host decodes: the tool's largest size.
    static let largestSide = 192

    /// The picture, decoded whole, drawn on an opaque white square of its own
    /// size and made a PNG again; nil where it is not a whole, square picture
    /// of a size the tool makes.
    static func flattened(_ data: Data) -> Data? {
        guard AndroidAppStore.isWholePNG(data),
              let source = CGImageSourceCreateWithData(data as CFData, nil), CGImageSourceGetCount(source) == 1,
              CGImageSourceGetStatus(source) == .statusComplete,
              let decoded = CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary),
              CGImageSourceGetStatusAtIndex(source, 0) == .statusComplete,
              decoded.width == decoded.height, (1...largestSide).contains(decoded.width)
        else { return nil }
        let side = CGFloat(decoded.width)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        // 8 bits of sRGB whatever the display: the extended range a wide-colour
        // device draws in by default is 16 bits a channel, and a PNG of that is
        // not one the store keeps (`AndroidAppLimits.largestIcon`).
        format.preferredRange = .standard
        let renderer = UIGraphicsImageRenderer(size: CGSize(width: side, height: side), format: format)
        return renderer.image { context in
            UIColor.white.setFill()
            context.fill(CGRect(x: 0, y: 0, width: side, height: side))
            UIImage(cgImage: decoded, scale: 1, orientation: .up).draw(in: CGRect(x: 0, y: 0, width: side, height: side))
        }.pngData()
    }
}

/// What the session knows of Android's apps: the list the cache holds, a
/// refresh under way, the request to open an app and the banner of how the last
/// one ended. The list is the cache's, so the chooser and the App Intent have
/// it while Android is off; the rest lasts for one run.
@Observable
final class AndroidAppsState {
    /// The apps of the last listing that was kept, in the order the person reads them.
    private(set) var apps: [AndroidApp] = []
    /// When the cached list was made (Android's listing), for the page's note.
    private(set) var listedAt: Date?
    /// A refresh is under way.
    private(set) var refreshing = false
    /// Icons the refresh is still to fetch.
    private(set) var iconsLeft = 0
    /// A listing answered this run (an empty one included).
    private(set) var listed = false
    /// The image has no listing tool, or speaks a version of it this Andarium
    /// does not know: nothing is offered.
    private(set) var unavailable = false
    /// The last refresh did not get an answer.
    private(set) var failed = false
    /// Changes whenever an icon arrives, so the screens read the pictures again.
    private(set) var iconRevision = 0
    /// The request to open an app and the banner of how the last one ended.
    var open = AppOpenState()
    /// Whether Android's apps are offered to Search (the Spotlight index): the
    /// switch, as the hub keeps it.
    var searchOn = true
    /// A refresh's own number: the progress of a refresh that stop, erase or a
    /// newer one ended does not reach the screens.
    @ObservationIgnored private(set) var refreshToken = 0
    /// A snapshot restore is under way, and the release of the requests it
    /// held waits for the model to have adopted what it restored.
    @ObservationIgnored var restoring = false
    /// The request that began this run's boot, or arrived while it ran: the
    /// launcher may not be up when the boot ends, so only that request waits
    /// for Android's home screen to resume. A request that comes once the boot
    /// is over does not, whatever is in front. Lasts for the run.
    @ObservationIgnored var bootRequest: AppOpenRequest?
    /// A change (an install) was announced while a refresh was under way: the
    /// refresh's owner makes one more after it, because what changed may have
    /// come after the listing it captured.
    @ObservationIgnored var refreshAgain = false
    /// The cache: the catalog, the icons, and the record kept per icon.
    @ObservationIgnored var store: AndroidAppStore
    /// The refresh under way, which a second asker joins.
    @ObservationIgnored var inFlight: Task<AndroidAppRefresh.Result, Never>?
    /// The run's first refresh, asked again a few times if it got no answer.
    @ObservationIgnored var first: Task<Void, Never>?
    /// Whose `first` is: each task made takes the next number, and only the
    /// one that holds the current number clears the handle or goes on after a wait.
    @ObservationIgnored private(set) var firstToken = 0
    /// The run's first listing is still to be made: Android was paused for
    /// space before the boot ended, or while the task waited, and the pause
    /// ending starts it again. Cleared by the answer and by the run's end.
    @ObservationIgnored var firstOwed = false
    /// A request that began the boot needs the list: the first listing is not made to wait.
    @ObservationIgnored var firstNeedsList = false
    /// The request that began this boot (Start's own boot is no request's).
    @ObservationIgnored var bootStartedBy: AppOpenRequest?
    /// Whose turn it is to ask the guest for a listing.
    @ObservationIgnored let listingLane = ListingLane()
    /// Android being made ready for the request, then the app being opened.
    @ObservationIgnored var opening: Task<Void, Never>?
    /// The wait for the next request or banner to run out.
    @ObservationIgnored var expiry: Task<Void, Never>?
    /// The preview keeps a waiting request and a banner as they are, for a
    /// screen that must stay still.
    @ObservationIgnored var holdsForever = false
    /// The first refresh waits this long after Android reports its boot
    /// complete, so that it starts nothing in the guest while the guest settles
    /// from its boot.
    @ObservationIgnored var startDelay = Duration.seconds(60)
    /// The first refresh's attempts wait twice as long as the one before.
    @ObservationIgnored var retryInterval = Duration.seconds(5)
    /// How the first refresh waits: a test holds the wait and lets it go.
    @ObservationIgnored var sleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    /// How many times the run's first refresh is asked.
    static let startAttempts = 5

    init(store: AndroidAppStore? = nil) {
        self.store = store ?? Self.defaultStore()
        searchOn = AndroidAppsHub.shared.preference.isOn
        adoptCache()
    }

    /// The process's cache (what the App Intent's queries read). A hosted test
    /// builds many models, and a model that ends its run ends the refresh of
    /// every model that shares its cache: each gets a cache of its own, and the
    /// tests of Add to Home Screen put the hub's (`AndroidAppsHub.configure`) in theirs.
    private static func defaultStore() -> AndroidAppStore {
        guard ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil else { return AndroidAppsHub.shared.store }
        return AndroidAppStore(
            directory: FileManager.default.temporaryDirectory
                .appendingPathComponent("andarium-apps-model-\(UUID().uuidString)", isDirectory: true))
    }

    /// The cached list, as the store has it now.
    func adoptCache() {
        let catalog = store.catalog()
        apps = catalog?.apps ?? []
        listedAt = catalog?.listedAt
    }

    var installed: [AndroidApp] { apps.filter { !$0.builtIn } }
    var builtIn: [AndroidApp] { apps.filter(\.builtIn) }

    /// A refresh starts: the number its progress and its end are told by.
    func begin() -> Int {
        refreshToken &+= 1
        refreshing = true
        failed = false
        return refreshToken
    }

    func owns(_ token: Int) -> Bool { refreshToken == token }

    /// A task for the run's first listing starts: the number it holds.
    func beginFirst() -> Int {
        firstToken &+= 1
        return firstToken
    }

    func ownsFirst(_ token: Int) -> Bool { firstToken == token }

    /// The task that holds `token` ends: the handle is cleared only if it is still that task's.
    func endFirst(_ token: Int) {
        if firstToken == token { first = nil }
    }

    /// A catalog is in the store (the listing, then each batch of icons).
    func progressed(_ catalog: AndroidAppCatalog, iconsLeft left: Int) {
        if apps != catalog.apps { apps = catalog.apps }
        listedAt = catalog.listedAt
        iconsLeft = left
        iconRevision &+= 1
    }

    func finished(_ result: AndroidAppRefresh.Result) {
        refreshing = false
        iconsLeft = 0
        switch result {
        case let .refreshed(catalog):
            progressed(catalog, iconsLeft: 0)
            listed = true
            firstOwed = false
            unavailable = false
            failed = false
        case .unavailable:
            unavailable = true
            listed = true
            firstOwed = false
        case .failed:
            failed = true
        }
    }

    /// Dropped for a run that ended meanwhile.
    func abandonedRefresh() {
        refreshing = false
        iconsLeft = 0
    }

    /// The refresh under way is no longer anyone's: it stops, its progress is
    /// not heard, and the store refuses what it still writes.
    private func endRefreshUnderWay() {
        refreshToken &+= 1
        store.invalidateRefreshes()
        inFlight?.cancel()
        inFlight = nil
        refreshAgain = false
        refreshing = false
        iconsLeft = 0
    }

    /// The run is over: nothing is under way, and what this run listed is not
    /// known of the next. The cached list stays.
    func runEnded() {
        endRefreshUnderWay()
        first?.cancel()
        first = nil
        firstToken &+= 1
        firstOwed = false
        firstNeedsList = false
        bootStartedBy = nil
        opening?.cancel()
        opening = nil
        open.runEnded()
        open.dismissBanner()
        listed = false
        failed = false
        unavailable = false
        bootRequest = nil
        expiry?.cancel()
        expiry = nil
    }

    /// Android was erased: the cache is gone, the record of each icon is kept.
    func erased() {
        runEnded()
        store.erase()
        adoptCache()
        iconRevision &+= 1
    }

    /// The disk may not be what was listed (a restored snapshot or update):
    /// the next refresh lists again, and a refresh under way is no longer the store's.
    /// A refresh that a change owed stays owed: the new listing it asks for may be kept
    /// from being made (Android paused for space), and the change is still in the disk.
    func invalidated() {
        let owed = refreshAgain
        endRefreshUnderWay()
        refreshAgain = owed
        store.invalidate()
        listed = false
    }

    /// The image has no tool (and none will answer this run): what an earlier
    /// image listed is not offered. The records stay.
    func withdrawCatalog() {
        endRefreshUnderWay()
        unavailable = true
        listed = true
        store.erase()
        adoptCache()
        iconRevision &+= 1
    }

    /// Sleeps until the earliest time a waiting request or a banner runs out,
    /// then lets `expire` drop what has, and looks again.
    func scheduleExpiry(_ changed: @escaping @MainActor () -> Void) {
        expiry?.cancel()
        expiry = nil
        guard let deadline = nextDeadline() else { return }
        expiry = Task { [weak self] in
            let wait = max(deadline.timeIntervalSinceNow, 0) + 0.05
            try? await Task.sleep(for: .seconds(wait))
            guard let self, !Task.isCancelled else { return }
            if open.expire(now: Date()) { changed() }
            scheduleExpiry(changed)
        }
    }

    private func nextDeadline() -> Date? {
        guard !holdsForever else { return nil }
        var deadlines: [Date] = []
        if let pending = open.pending, case let .waiting(_, since) = pending.stage {
            deadlines.append(since.addingTimeInterval(AppOpenState.holdLimit))
        }
        if let banner = open.banner {
            let limit = banner.failure.canRetry ? AppOpenState.retryBannerLimit : AppOpenState.bannerLimit
            deadlines.append(banner.shownAt.addingTimeInterval(limit))
        }
        return deadlines.min()
    }
}

/// How the app list reaches Spotlight (Search) and the App Shortcuts, so a test
/// can watch it without the system's index. Each write says whether the system
/// took it, so that one that did not is tried again.
@MainActor
protocol AndroidAppsSearchIndexing: AnyObject {
    /// Offers these apps to Search, in place of what was offered.
    func offer(_ apps: [AndroidApp]) async -> Bool
    /// Adds these apps to what is offered (the system asked for them again).
    func add(_ apps: [AndroidApp]) async -> Bool
    /// Takes every app back out of Search.
    func withdraw() async -> Bool
    /// Tells the system that the apps the shortcut tiles are made of changed.
    func shortcutsChanged()
}

/// The system's index. The apps are entities from iOS 27; before it there is
/// nothing to write, and no host there asks (`AndroidAppsHub.hostOffers`).
@MainActor
final class SpotlightAppsIndex: AndroidAppsSearchIndexing {
    func offer(_ apps: [AndroidApp]) async -> Bool {
        guard #available(iOS 27.0, *) else { return true }
        let index = CSSearchableIndex.default()
        do {
            try await index.deleteAppEntities(ofType: AndroidAppEntity.self)
            if !apps.isEmpty { try await index.indexAppEntities(apps.map(AndroidAppEntity.init)) }
            return true
        } catch {
            return false
        }
    }

    func add(_ apps: [AndroidApp]) async -> Bool {
        guard !apps.isEmpty else { return true }
        guard #available(iOS 27.0, *) else { return true }
        do {
            try await CSSearchableIndex.default().indexAppEntities(apps.map(AndroidAppEntity.init))
            return true
        } catch {
            return false
        }
    }

    func withdraw() async -> Bool {
        guard #available(iOS 27.0, *) else { return true }
        do {
            try await CSSearchableIndex.default().deleteAppEntities(ofType: AndroidAppEntity.self)
            return true
        } catch {
            return false
        }
    }

    func shortcutsChanged() {
        // Before iOS 27 there are no tiles to tell of the apps.
        if #available(iOS 27.0, *) {
            AndroidAppShortcuts.updateAppShortcutParameters()
        }
    }
}

/// The one path every write to Spotlight takes: the refresh, the switch, the
/// launch's reconcile and the system's own reindex requests queue behind one
/// another, and each is judged against the preference and the catalog as they
/// are when it runs, not as they were when it was asked. An app is offered
/// only while it is listed; the switch off keeps every app out; a write that
/// did not succeed is tried again by the next call (the launch makes one).
/// Where the host has no Home Screen to add to, nothing is written at all.
@MainActor
final class AndroidAppsIndexWriter {
    private unowned let hub: AndroidAppsHub
    private var held = false
    private var waiters: [CheckedContinuation<Void, Never>] = []
    private var asked = 0
    private var judged = 0
    private var tilesKey: String

    init(hub: AndroidAppsHub) {
        self.hub = hub
        tilesKey = Self.key(of: [], store: hub.store)
    }

    /// Makes Spotlight and the shortcut tiles what the catalog and the switch
    /// say now. A call whose answer an earlier, later-started pass already
    /// gave is not judged again.
    func reconcile() async {
        asked += 1
        let mine = asked
        await acquire()
        defer { release() }
        guard judged < mine else { return }
        let covering = asked
        await judge()
        judged = covering
    }

    /// A write the index did not take: the system is told, and asks again.
    struct IndexWriteFailed: Error {}

    /// The system asks for some apps again (nil: all of them). A write the
    /// index did not take is an error to the system.
    func reindex(_ identifiers: [String]?) async throws {
        await acquire()
        defer { release() }
        guard hub.hostOffers, hub.preference.isOn else { return }
        let apps = hub.listedApps()
        let index = hub.searchIndex
        if let identifiers {
            let wanted = Set(identifiers)
            let chosen = apps.filter { wanted.contains($0.id) }
            // An add changes what Spotlight holds whatever the saved state says
            // ("off" included): from here it is not known, and the next
            // reconcile writes what the switch says, offer or withdrawal.
            if !chosen.isEmpty { hub.preference.keepIndexed(nil) }
            guard await index.add(chosen) else { throw IndexWriteFailed() }
        } else {
            // What is offered and the fingerprint recorded for it come from the
            // same instant: artwork that arrives during the offer is not in it.
            let wanted = "on:" + Self.key(of: apps, store: hub.store)
            // From the delete on, what Spotlight holds is not known: an offer
            // that fails or is cut short leaves the next reconcile to repair it.
            hub.preference.keepIndexed(nil)
            guard await index.offer(apps) else { throw IndexWriteFailed() }
            hub.preference.keepIndexed(wanted)
        }
    }

    private func judge() async {
        guard hub.hostOffers else { return }
        let index = hub.searchIndex
        let apps = hub.listedApps()
        let key = Self.key(of: apps, store: hub.store)
        if key != tilesKey {
            tilesKey = key
            index.shortcutsChanged()
        }
        let on = hub.preference.isOn
        let wanted = on ? "on:" + key : "off"
        guard wanted != hub.preference.indexed else { return }
        // Unknown from here to the acknowledgement: a launch that finds it so
        // writes again, whichever way the switch is by then.
        hub.preference.keepIndexed(nil)
        let took = on ? await index.offer(apps) : await index.withdraw()
        if took { hub.preference.keepIndexed(wanted) }
    }

    /// What the system is told of: each app's identifier, name, icon key and
    /// whether its picture is there. It changes when the artwork does though
    /// the rows are the same.
    static func key(of apps: [AndroidApp], store: AndroidAppStore) -> String {
        var hasher = SHA256()
        for app in apps {
            hasher.update(data: Data("\(app.id)\t\(app.label)\t\(app.iconTag)\t\(store.hasIcon(of: app) ? 1 : 0)\n".utf8))
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    private func acquire() async {
        guard held else { held = true; return }
        await withCheckedContinuation { waiters.append($0) }
    }

    private func release() {
        if waiters.isEmpty { held = false } else { waiters.removeFirst().resume() }
    }
}

/// What the App Intent, its entity and its queries share with the application:
/// the cache (which answers while Android is off, and in a process the system
/// started for a query, before any screen or model exists), the model a
/// request goes to, the switch for Search, and the index.
nonisolated final class AndroidAppsHub: @unchecked Sendable {
    static let shared = AndroidAppsHub()

    private let lock = NSLock()
    private var kept: AndroidAppStore?
    private var preferenceKept = AndroidAppSearchPreference()
    private var hostOffersKept = AndroidAppsHub.processOffersAddToHomeScreen
    private var writerKept: AndroidAppsIndexWriter?

    /// The host has a Home Screen to add to (an iPhone or an iPad) and
    /// Shortcuts has the tile for each app (iOS 27 and later): a Mac, a Vision
    /// Pro and an earlier iOS run the application but offer none of this
    /// feature.
    static var processOffersAddToHomeScreen: Bool {
        IPadAppHost.system.offersAddToHomeScreen
    }

    /// Whether the apps are entities, in Spotlight and in the tiles.
    var hostOffers: Bool {
        get { lock.withLock { hostOffersKept } }
        set { lock.withLock { hostOffersKept = newValue } }
    }

    /// The switch for Search, as it is kept.
    var preference: AndroidAppSearchPreference {
        get { lock.withLock { preferenceKept } }
        set { lock.withLock { preferenceKept = newValue } }
    }

    /// The cache, in the application's support folder unless a launch put it elsewhere.
    var store: AndroidAppStore {
        lock.withLock {
            if let kept { return kept }
            let store = AndroidAppStore(directory: Self.defaultDirectory())
            kept = store
            return store
        }
    }

    /// Puts the cache somewhere else (an isolated test guest, a hosted test,
    /// the preview). Done before the first use; a model made afterwards adopts it.
    func configure(directory: URL) {
        lock.withLock {
            kept = AndroidAppStore(directory: directory)
            writerKept = nil
        }
    }

    static func defaultDirectory() -> URL {
        let support = (try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
        return support.appendingPathComponent("AndroidApps", isDirectory: true)
    }

    private weak var attachedModel: AppModel?
    private var indexKept: (any AndroidAppsSearchIndexing)?

    /// The model a request is handed to. The process's own, unless a test
    /// attached another.
    @MainActor var model: AppModel {
        if let attached = lock.withLock({ attachedModel }) { return attached }
        return AndariumProcess.shared.model
    }

    @MainActor func attach(_ model: AppModel?) {
        lock.withLock { attachedModel = model }
        if let model { hostOffers = model.hostOffersAddToHomeScreen }
    }

    /// Where the apps are offered to the system; a test or the preview puts another.
    @MainActor var searchIndex: any AndroidAppsSearchIndexing {
        get {
            lock.withLock {
                if let indexKept { return indexKept }
                let index = SpotlightAppsIndex()
                indexKept = index
                return index
            }
        }
        set {
            lock.withLock {
                indexKept = newValue
                writerKept = nil
            }
        }
    }

    /// The one ordered path to the index.
    @MainActor var writer: AndroidAppsIndexWriter {
        if let writer = lock.withLock({ writerKept }) { return writer }
        let writer = AndroidAppsIndexWriter(hub: self)
        lock.withLock { writerKept = writer }
        return writer
    }

    /// The apps of the cache, the ones the system is told of: none where the
    /// host offers none.
    func listedApps() -> [AndroidApp] {
        hostOffers ? (store.catalog()?.apps ?? []) : []
    }

    /// The entities of the cache, as the query answers.
    @available(iOS 27.0, *)
    func entities() -> [AndroidAppEntity] {
        listedApps().map(AndroidAppEntity.init)
    }
}
