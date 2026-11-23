// SPDX-License-Identifier: GPL-2.0-or-later

import CryptoKit
import Foundation

/// What Android last said of its launchable apps, kept on the device so that
/// the Home Screen's icons, Shortcuts, Search and Siri can name them while
/// Android is not running.
public struct AndroidAppCatalog: Equatable, Sendable, Codable {
    public static let currentSchema = 1

    public var schema = AndroidAppCatalog.currentSchema
    /// When Android last answered the listing that this one is.
    public var listedAt: Date
    public var digest: String?
    /// By name, as the person reads them.
    public var apps: [AndroidApp]

    public init(listedAt: Date, digest: String?, apps: [AndroidApp]) {
        self.listedAt = listedAt
        self.digest = digest
        self.apps = apps
    }

    public func app(withID id: String) -> AndroidApp? { apps.first { $0.id == id } }
}

/// What the store keeps of one app after an erase: enough to say, for an icon
/// that outlived its Android, which app it was.
public struct AndroidAppRecord: Equatable, Sendable, Codable, Identifiable {
    public var id: String
    public var label: String
    public var builtIn: Bool
    public var seen: Date
    /// How many launcher activities the app's package had in the listing that
    /// last held this one; nil where that is not known. An icon whose activity
    /// is gone follows the package to its one other launcher only where this
    /// is 1 (the app renamed its launcher): with more, which of them the icon
    /// was for is not known.
    public var launchers: Int?

    public init(id: String, label: String, builtIn: Bool, seen: Date, launchers: Int? = nil) {
        self.id = id
        self.label = label
        self.builtIn = builtIn
        self.seen = seen
        self.launchers = launchers
    }

    public var component: AndroidComponent? { AndroidComponent(identifier: id) }
}

/// The launchable apps on disk, in one folder of the application:
///
/// - `Catalog/catalog.json` and `Catalog/Icons/` hold the last listing and the
///   icons as PNG files; they belong to the Android that was listed and go
///   when it is erased (`erase`).
/// - `records.json` holds a small record per app ever listed (its identifier,
///   its name and when it was seen), which an erase keeps, so that a Home
///   Screen icon whose Android is gone can still say which app it is, and
///   works again once the app is back in a new Android.
///
/// Everything is excluded from backup. A store is safe to read from any
/// thread: the intents' queries read it while the application is not
/// running, and the model writes it.
public final class AndroidAppStore: @unchecked Sendable {
    public let directory: URL
    private let lock = NSLock()
    private var loaded = false
    private var catalogCache: AndroidAppCatalog?
    private var recordCache: [String: AndroidAppRecord] = [:]
    private var generation = 0
    private var artwork = 0

    /// The most a catalog file and a records file may hold before they are read.
    static let largestFile = 4 * 1_024 * 1_024

    private let fileManager = FileManager.default

    public init(directory: URL) {
        self.directory = directory
    }

    private var catalogDirectory: URL { directory.appendingPathComponent("Catalog", isDirectory: true) }
    private var catalogFile: URL { catalogDirectory.appendingPathComponent("catalog.json") }
    private var iconDirectory: URL { catalogDirectory.appendingPathComponent("Icons", isDirectory: true) }
    private var recordsFile: URL { directory.appendingPathComponent("records.json") }

    // MARK: Reading

    /// The last listing; nil before Android has been listed and after an erase.
    public func catalog() -> AndroidAppCatalog? {
        lock.withLock {
            loadIfNeeded()
            return catalogCache
        }
    }

    /// The icon kept for the app as it is now listed (its tag included), as PNG data.
    public func icon(of app: AndroidApp) -> Data? {
        lock.withLock { iconData(for: app) }
    }

    /// The icon kept for the app whatever its tag, for the entity of an app
    /// that is listed.
    public func icon(forID id: String) -> Data? {
        lock.withLock {
            loadIfNeeded()
            return catalogCache?.app(withID: id).flatMap { iconData(for: $0) }
        }
    }

    public func record(forID id: String) -> AndroidAppRecord? {
        lock.withLock {
            loadIfNeeded()
            return recordCache[id]
        }
    }

    public func records() -> [AndroidAppRecord] {
        lock.withLock {
            loadIfNeeded()
            return recordCache.values.sorted { $0.id < $1.id }
        }
    }

    /// The apps whose icon the store does not have as listed. An app that
    /// declares no icon (`markNoIcon`) is not missing one.
    public func appsMissingIcons(in list: AndroidAppList) -> [AndroidApp] {
        lock.withLock { list.apps.filter { iconData(for: $0) == nil && !declaresNoIcon($0) } }
    }

    /// Whether a picture is kept for the app as listed: a look at the file,
    /// not a read of it, for a figure over a whole catalog.
    public func hasIcon(of app: AndroidApp) -> Bool {
        lock.withLock { fileManager.fileExists(atPath: iconDirectory.appendingPathComponent(iconFileName(for: app)).path) }
    }

    /// Whether Android said the app declares no icon, as listed.
    public func declaresNoIcon(of app: AndroidApp) -> Bool {
        lock.withLock { declaresNoIcon(app) }
    }

    /// Changes whenever a picture is kept or told to be none, so that what is
    /// published to the system (the artwork of the tiles and of Search) can be
    /// told from what was published before.
    public var artworkRevision: Int { lock.withLock { artwork } }

    // MARK: Refresh ownership

    /// A refresh takes the generation when it starts and hands it back with
    /// every change it makes; a change of a generation that is no longer the
    /// store's is refused. Stopping the run, an erase, a restored snapshot and
    /// `invalidateRefreshes` end the generation of a refresh that is under way.
    public var refreshGeneration: Int { lock.withLock { generation } }

    public func invalidateRefreshes() {
        lock.withLock { generation += 1 }
    }

    /// A change of a refresh that was ended.
    public struct StaleRefresh: Error, Equatable {}

    // MARK: Writing

    /// Keeps a listing. Icons of apps no longer listed, or listed with a new
    /// tag, are removed; the records learn every listed app's name. A listing
    /// that is the same as the stored one (digest and apps) changes nothing
    /// but the time it was taken. With a `generation`, a listing of a refresh
    /// that ended meanwhile is refused (`StaleRefresh`).
    public func apply(_ list: AndroidAppList, at date: Date, generation expected: Int? = nil) throws {
        try lock.withLock {
            guard expected == nil || expected == generation else { throw StaleRefresh() }
            loadIfNeeded()
            try prepareDirectories()
            let apps = Array(list.apps.prefix(AndroidAppLimits.mostApps))
            let catalog = AndroidAppCatalog(listedAt: date, digest: list.digest, apps: apps)
            try write(catalog, to: catalogFile)
            catalogCache = catalog
            let keep = Set(apps.flatMap { [iconFileName(for: $0), noIconFileName(for: $0)] })
            if let files = try? fileManager.contentsOfDirectory(atPath: iconDirectory.path) {
                for file in files where !keep.contains(file) {
                    try? fileManager.removeItem(at: iconDirectory.appendingPathComponent(file))
                }
            }
            var launchers: [String: Int] = [:]
            for app in apps { launchers[app.component.packageName, default: 0] += 1 }
            for app in apps {
                recordCache[app.id] = AndroidAppRecord(
                    id: app.id, label: app.label, builtIn: app.builtIn, seen: date,
                    launchers: launchers[app.component.packageName])
            }
            trimRecords()
            try writeRecords()
        }
    }

    /// Keeps one app's icon (PNG data) as listed. A picture that is not a
    /// whole PNG of a size worth keeping is not kept. Returns whether it was
    /// (a refresh that ended meanwhile keeps nothing).
    @discardableResult
    public func setIcon(_ data: Data, for app: AndroidApp, generation expected: Int? = nil) -> Bool {
        lock.withLock {
            guard expected == nil || expected == generation else { return false }
            guard Self.isWholePNG(data) else { return false }
            guard (try? prepareDirectories()) != nil else { return false }
            guard (try? data.write(to: iconDirectory.appendingPathComponent(iconFileName(for: app)), options: .atomic)) != nil
            else { return false }
            artwork += 1
            return true
        }
    }

    /// Keeps that Android said the app declares no icon, so it is not asked for again.
    @discardableResult
    public func markNoIcon(for app: AndroidApp, generation expected: Int? = nil) -> Bool {
        lock.withLock {
            guard expected == nil || expected == generation else { return false }
            guard (try? prepareDirectories()) != nil else { return false }
            guard (try? Data().write(to: iconDirectory.appendingPathComponent(noIconFileName(for: app)), options: .atomic)) != nil
            else { return false }
            artwork += 1
            return true
        }
    }

    /// The listing is the one the store has: only the time it was taken changes.
    @discardableResult
    public func touch(at date: Date, generation expected: Int? = nil) -> Bool {
        lock.withLock {
            guard expected == nil || expected == generation else { return false }
            loadIfNeeded()
            guard var catalog = catalogCache else { return false }
            catalog.listedAt = date
            guard (try? write(catalog, to: catalogFile)) != nil else { return false }
            catalogCache = catalog
            return true
        }
    }

    /// What was listed may not be what the disk now has (a snapshot or an
    /// update was restored): the next refresh lists again, whatever the digest,
    /// and a refresh under way is no longer the store's.
    public func invalidate() {
        lock.withLock {
            generation += 1
            loadIfNeeded()
            guard var catalog = catalogCache, catalog.digest != nil else { return }
            catalog.digest = nil
            guard (try? write(catalog, to: catalogFile)) != nil else { return }
            catalogCache = catalog
        }
    }

    /// Android was erased or reset: its apps are none. The records stay.
    public func erase() {
        lock.withLock {
            generation += 1
            loadIfNeeded()
            try? fileManager.removeItem(at: catalogDirectory)
            catalogCache = nil
            artwork += 1
        }
    }

    /// Forgets everything, the records included (for a test, and for the person
    /// who wants the leftovers gone).
    public func eraseRecords() {
        lock.withLock {
            generation += 1
            loadIfNeeded()
            try? fileManager.removeItem(at: catalogDirectory)
            try? fileManager.removeItem(at: recordsFile)
            catalogCache = nil
            recordCache = [:]
            artwork += 1
        }
    }

    // MARK: Files

    private func loadIfNeeded() {
        guard !loaded else { return }
        loaded = true
        if let data = readBounded(catalogFile), let decoded = try? JSONDecoder.store.decode(AndroidAppCatalog.self, from: data) {
            catalogCache = Self.validated(decoded)
        }
        if let data = readBounded(recordsFile), let decoded = try? JSONDecoder.store.decode([AndroidAppRecord].self, from: data) {
            let records = decoded.compactMap(Self.validated)
            recordCache = Dictionary(records.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
            trimRecords()
        }
    }

    /// The file's bytes, unless it is larger than a file of the store can be.
    private func readBounded(_ url: URL) -> Data? {
        guard let size = (try? fileManager.attributesOfItem(atPath: url.path))?[.size] as? Int, size <= Self.largestFile
        else { return nil }
        return try? Data(contentsOf: url)
    }

    /// A catalog read from disk is held to what a listing is: the current
    /// schema, at most `mostApps` apps, each with an icon key of the tool's
    /// shape and an identifier no other has, and each name made again what the
    /// screens are allowed to show. Anything else is not read.
    static func validated(_ catalog: AndroidAppCatalog) -> AndroidAppCatalog? {
        guard catalog.schema == AndroidAppCatalog.currentSchema, catalog.apps.count <= AndroidAppLimits.mostApps else { return nil }
        var seen = Set<String>()
        var apps: [AndroidApp] = []
        for app in catalog.apps {
            guard isIconKey(app.iconTag), seen.insert(app.id).inserted else { return nil }
            var kept = app
            kept.label = AndroidAppText.label(app.label, fallback: app.component.packageName)
            apps.append(kept)
        }
        var checked = catalog
        checked.apps = apps
        return checked
    }

    private static func validated(_ record: AndroidAppRecord) -> AndroidAppRecord? {
        guard let component = record.component else { return nil }
        var kept = record
        kept.label = AndroidAppText.label(record.label, fallback: component.packageName)
        if let launchers = kept.launchers, !(1...AndroidAppLimits.mostApps).contains(launchers) { kept.launchers = nil }
        return kept
    }

    private static func isIconKey(_ text: String) -> Bool {
        text.utf8.count == 16 && text.utf8.allSatisfy { ($0 >= 48 && $0 <= 57) || ($0 >= 97 && $0 <= 102) }
    }

    /// A PNG that is all there: the signature, the header chunk first and the
    /// end chunk last, and of a size the store keeps. A truncated file or
    /// bytes that are no picture are not artwork.
    public static func isWholePNG(_ data: Data) -> Bool {
        let signature: [UInt8] = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]
        let end: [UInt8] = [0, 0, 0, 0, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82]
        guard data.count >= 8 + 25 + 12, data.count <= AndroidAppLimits.largestIcon else { return false }
        let bytes = Array(data)
        return Array(bytes[0..<8]) == signature && Array(bytes[12..<16]) == Array("IHDR".utf8)
            && Array(bytes[(bytes.count - 12)...]) == end
    }

    private func prepareDirectories() throws {
        try fileManager.createDirectory(at: iconDirectory, withIntermediateDirectories: true)
        var url = directory
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? url.setResourceValues(values)
    }

    private func write<Value: Encodable>(_ value: Value, to url: URL) throws {
        try JSONEncoder.store.encode(value).write(to: url, options: .atomic)
    }

    private func writeRecords() throws {
        try write(recordCache.values.sorted { $0.id < $1.id }, to: recordsFile)
    }

    /// The oldest records go first once there are more than the bound, and the
    /// records of the apps of the catalog go last, whatever their dates say.
    private func trimRecords() {
        guard recordCache.count > AndroidAppLimits.mostRecords else { return }
        let listed = Set(catalogCache?.apps.map(\.id) ?? [])
        let oldest = recordCache.values.filter { !listed.contains($0.id) }
            .sorted { ($0.seen, $0.id) < ($1.seen, $1.id) }
            .prefix(recordCache.count - AndroidAppLimits.mostRecords)
        for record in oldest { recordCache[record.id] = nil }
    }

    /// The picture kept for the app as listed, when it is a whole PNG; a file
    /// that cannot be read, or was cut short, is no picture.
    private func iconData(for app: AndroidApp) -> Data? {
        let url = iconDirectory.appendingPathComponent(iconFileName(for: app))
        guard let size = (try? fileManager.attributesOfItem(atPath: url.path))?[.size] as? Int,
              size <= AndroidAppLimits.largestIcon, let data = try? Data(contentsOf: url), Self.isWholePNG(data)
        else { return nil }
        return data
    }

    private func declaresNoIcon(_ app: AndroidApp) -> Bool {
        fileManager.fileExists(atPath: iconDirectory.appendingPathComponent(noIconFileName(for: app)).path)
    }

    func noIconFileName(for app: AndroidApp) -> String {
        String(iconFileName(for: app).dropLast(".png".count)) + ".none"
    }

    /// A file name that no app's own text can shape: the hash of the identifier and the tag.
    func iconFileName(for app: AndroidApp) -> String {
        let digest = SHA256.hash(data: Data("\(app.id)\n\(app.iconTag)".utf8))
        return digest.map { String(format: "%02x", $0) }.joined() + ".png"
    }
}

extension JSONEncoder {
    fileprivate static var store: JSONEncoder {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        encoder.outputFormatting = [.sortedKeys]
        return encoder
    }
}

extension JSONDecoder {
    fileprivate static var store: JSONDecoder {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return decoder
    }
}

/// Brings the store up to what Android lists now, for the model and its tests.
public enum AndroidAppRefresh {
    /// How a refresh ended.
    public enum Result: Equatable, Sendable {
        /// The store has Android's list (changed or not).
        case refreshed(AndroidAppCatalog)
        case unavailable
        case failed
    }

    /// Asks the tool whether the apps changed (its digest against the stored
    /// one) and lists them only if they did; the listing is stored first, so
    /// every name is known at once, and the icons that are missing follow, a
    /// few to an ask, each stored as it arrives. `progress` is told after the
    /// listing and after each batch of icons, with how many are still to come.
    /// An unchanged digest with every icon in hand asks for nothing more.
    ///
    /// A refresh owns the store only while it is the current one: it takes the
    /// store's generation at its start and hands it back with each change, so
    /// a refresh that the run ending, an erase or a restore ended stores
    /// nothing, whatever the tool still answers, and tells nothing.
    ///
    /// `forAnOpen` is the listing a host with no Home Screen takes before it opens an app by name:
    /// the apps are listed whatever the digest says, and no icon is asked for.
    public static func run(
        tool: any AndroidAppsTool, store: AndroidAppStore, now: Date = Date(), forAnOpen: Bool = false,
        progress: @Sendable (AndroidAppCatalog, _ iconsLeft: Int) -> Void = { _, _ in }
    ) async -> Result {
        let generation = store.refreshGeneration
        var remote: AndroidAppDigestAnswer?
        if !forAnOpen { remote = await tool.digest(limit: AndroidAppLimits.digestLimit) }
        switch remote {
        case .unavailable?: return .unavailable
        case .failed?: return .failed
        default: break
        }
        guard !Task.isCancelled, store.refreshGeneration == generation else { return .failed }
        if case let .digest(digest)? = remote, let kept = store.catalog(), kept.digest == digest {
            // Nothing that is cached is out of date; an icon that never arrived is still owed.
            guard store.touch(at: now, generation: generation), let catalog = store.catalog() else { return .failed }
            await fetchIcons(tool: tool, store: store, generation: generation, catalog: catalog, progress: progress)
            return .refreshed(catalog)
        }
        switch await tool.list(limit: AndroidAppLimits.listLimit) {
        case .unavailable:
            return .unavailable
        case .failed:
            return .failed
        case let .apps(list):
            guard !Task.isCancelled else { return .failed }
            let sorted = AndroidAppList(apps: list.apps.sorted(by: AndroidAppText.inOrder), digest: list.digest)
            do { try store.apply(sorted, at: now, generation: generation) } catch { return .failed }
            guard let catalog = store.catalog() else { return .failed }
            if !forAnOpen {
                await fetchIcons(tool: tool, store: store, generation: generation, catalog: catalog, progress: progress)
            }
            return .refreshed(catalog)
        }
    }

    private static func fetchIcons(
        tool: any AndroidAppsTool, store: AndroidAppStore, generation: Int, catalog: AndroidAppCatalog,
        progress: @Sendable (AndroidAppCatalog, Int) -> Void
    ) async {
        let list = AndroidAppList(apps: catalog.apps, digest: catalog.digest)
        var missing = store.appsMissingIcons(in: list)
        progress(catalog, missing.count)
        // `error` and `missing` are asked again at the next refresh. A batch the
        // tool gave no answer to (it ran out of time) is asked again in
        // halves, down to one app, and only a silence at one app counts
        // against the refresh: after `silentAsksBeforeGivingUp` of them in a
        // row it stops asking for icons (the next refresh asks again). An
        // icon the tool answered `deferred` (it ran out of time on the one
        // before) is asked a few more times, all within this refresh.
        var size = AndroidAppLimits.iconsPerAsk
        var silentAtOne = 0
        var asked: [String: Int] = [:]
        while !missing.isEmpty {
            if Task.isCancelled || store.refreshGeneration != generation { break }
            let batch = Array(missing.prefix(size))
            missing.removeFirst(batch.count)
            let answers = await tool.icons(of: batch, limit: AndroidAppLimits.iconLimit)
            if Task.isCancelled || store.refreshGeneration != generation { break }
            if answers.isEmpty {
                if batch.count > 1 {
                    size = max(1, batch.count / 2)
                    missing.insert(contentsOf: batch, at: 0)
                } else {
                    silentAtOne += 1
                    if silentAtOne >= AndroidAppLimits.silentAsksBeforeGivingUp { break }
                }
                progress(catalog, missing.count)
                continue
            }
            silentAtOne = 0
            for app in batch {
                switch answers[app.id] {
                case let .png(data, _)?: store.setIcon(data, for: app, generation: generation)
                case .noIcon?: store.markNoIcon(for: app, generation: generation)
                case .deferred?:
                    asked[app.id, default: 0] += 1
                    if asked[app.id, default: 0] <= AndroidAppLimits.deferredRetries { missing.append(app) }
                case .missing?, .error?, nil: break
                }
            }
            progress(catalog, missing.count)
        }
    }
}
