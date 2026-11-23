// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

private func component(_ package: String, _ activity: String = "com.example.Main") -> AndroidComponent {
    AndroidComponent(packageName: package, activityClass: activity)!
}

private func app(_ package: String, _ label: String, builtIn: Bool = false, tag: String = "1", activity: String = "com.example.Main")
    -> AndroidApp
{
    // The tool's icon key is 16 lowercase hex digits; a short tag stands for the key it pads to.
    let key = String(repeating: "0", count: max(0, 16 - tag.count)) + tag
    return AndroidApp(component: component(package, activity), label: label, builtIn: builtIn, iconTag: key)
}

/// A PNG as the store keeps one: the signature, a header chunk and the end
/// chunk, with `padding` more bytes between them; `mark` tells one from another.
private func wholePNG(_ padding: Int = 0, mark: UInt8 = 0) -> Data {
    var data = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13])
    data.append(contentsOf: Array("IHDR".utf8))
    data.append(Data(repeating: mark, count: 13 + 4 + padding))
    data.append(contentsOf: [0, 0, 0, 0, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82])
    return data
}

private func makeDirectory() -> URL {
    FileManager.default.temporaryDirectory.appendingPathComponent("andarium-apps-tests-\(UUID().uuidString)")
}

/// A tool that answers as scripted and counts what it was asked.
private final class FakeTool: AndroidAppsTool, @unchecked Sendable {
    private let lock = NSLock()
    var listing: AndroidAppListing
    var digestAnswer: AndroidAppDigestAnswer?
    var icons: [String: AndroidAppIconAnswer] = [:]
    var opens: [AndroidComponent: AndroidAppOpenAnswer] = [:]
    /// The most icons one ask is answered for; a bigger ask gets no answer (the tool ran out of time).
    var answersOnlyUpTo: Int?
    /// Icons answered `deferred` the first time they are asked for.
    var deferredOnce: Set<String> = []
    /// What happens while an ask is under way (an erase, the run ending).
    var duringList: (@Sendable () -> Void)?
    var duringIcons: (@Sendable () -> Void)?
    private(set) var iconAsks: [[String]] = []
    private(set) var openAsks: [AndroidComponent] = []
    private(set) var listAsks = 0
    private(set) var digestAsks = 0

    init(_ listing: AndroidAppListing) { self.listing = listing }

    var askedIcons: [String] { iconAsks.flatMap { $0 } }

    func digest(limit: Duration) async -> AndroidAppDigestAnswer {
        lock.withLock { digestAsks += 1 }
        if let digestAnswer { return digestAnswer }
        switch listing {
        case let .apps(list): return .digest(list.digest ?? "none")
        case .unavailable: return .unavailable
        case .failed: return .failed
        }
    }

    func list(limit: Duration) async -> AndroidAppListing {
        lock.withLock { listAsks += 1 }
        duringList?()
        return listing
    }

    func icons(of apps: [AndroidApp], limit: Duration) async -> [String: AndroidAppIconAnswer] {
        let asked = lock.withLock { () -> [String] in
            iconAsks.append(apps.map(\.id))
            return iconAsks.dropLast().flatMap { $0 }
        }
        duringIcons?()
        if let answersOnlyUpTo, apps.count > answersOnlyUpTo { return [:] }
        return Dictionary(uniqueKeysWithValues: apps.compactMap { app in
            if deferredOnce.contains(app.id), !asked.contains(app.id) { return (app.id, .deferred) }
            return icons[app.id].map { (app.id, $0) }
        })
    }

    func open(_ component: AndroidComponent, limit: Duration) async -> AndroidAppOpenAnswer {
        lock.withLock { openAsks.append(component) }
        return opens[component] ?? .failed
    }
}

private final class AskCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    var value: Int { lock.withLock { count } }
    func add() { lock.withLock { count += 1 } }
}

struct AndroidComponentTests {
    @Test func theIdentifierIsThePackageAndTheClass() {
        let c = component("com.example.app", "com.example.app.Main$Launcher")
        #expect(c.identifier == "com.example.app/com.example.app.Main$Launcher")
        #expect(AndroidComponent(identifier: c.identifier) == c)
    }

    @Test func aPackageNameIsAtLeastTwoSegmentsOfIdentifiers() {
        for good in ["com.example", "a.b", "com.example.app_1", "Com.Example2"] {
            #expect(AndroidComponent.isPackageName(good), "\(good)")
        }
        for bad in [
            "", "example", "com..example", ".com.example", "com.example.", "com.1example", "com.exa mple", "com.example;id",
            "-n.example", "com.exämple", "com.example'", "com.example\n", "com/example", String(repeating: "a.", count: 200) + "b",
        ] {
            #expect(!AndroidComponent.isPackageName(bad), "\(bad)")
        }
    }

    @Test func aClassNameIsJavaIdentifiersWithAnOptionalLeadingDot() {
        for good in ["com.example.Main", "com.example.Main", "com.example.Main$Launcher", "Main", "com.example.$Hidden", "_a._b"] {
            #expect(AndroidComponent.isActivityClass(good), "\(good)")
        }
        for bad in [
            "", ".", ".Main", "..Main", "com.example.", "com..Main", "-n", "--es", "com.example.Main'", "com.example.Main\"", "a;b",
            "a|b", "a&b", "a`b", "a$(id)", "a b", "a\nb", "a/b", "a\\b", "a<b", "a>b", "(a)", "Mäin", "1Main",
            String(repeating: "a", count: 256),
        ] {
            #expect(!AndroidComponent.isActivityClass(bad), "\(bad)")
        }
    }

    @Test func theLongestTokenTheToolListsIsAComponentAndOneByteMoreOnEitherSideIsNot() {
        let package = String(repeating: "a", count: 253) + ".x"
        let activity = String(repeating: "b", count: 255)
        let longest = AndroidComponent(packageName: package, activityClass: activity)
        #expect(longest?.identifier.utf8.count == 511)
        #expect(AndroidComponent(identifier: longest?.identifier ?? "") == longest)
        #expect(AndroidComponent(packageName: package + "y", activityClass: activity) == nil)
        #expect(AndroidComponent(packageName: package, activityClass: activity + "c") == nil)
    }

    @Test func anIdentifierWithAnythingElseIsNoComponent() {
        for bad in ["", "com.example", "com.example/", "/com.example.Main", "com.example/.Main", "com.example/com.example.Main/extra", "com.example/com.example.Main;id"] {
            #expect(AndroidComponent(identifier: bad) == nil, "\(bad)")
        }
    }
}

struct AndroidAppTextTests {
    @Test func aLabelIsMadeSafeToShow() {
        #expect(AndroidAppText.label("  Comet\tHop \u{202E}\n", fallback: "x") == "Comet Hop")
        #expect(AndroidAppText.label("a\u{2066}b\u{2069}", fallback: "x") == "ab")
        #expect(AndroidAppText.label("   ", fallback: "com.example") == "com.example")
        // Characters that draw nothing and are no format character: the Hangul fillers and the blank braille.
        #expect(AndroidAppText.label("\u{3164}\u{115F}\u{2800}\u{FFA0}\u{1160}", fallback: "com.example") == "com.example")
        #expect(AndroidAppText.label("\u{3164}Comet\u{2800}Hop\u{3164}", fallback: "x") == "Comet Hop")
    }

    @Test func theToolsOwnFilterIsAppliedAgain() {
        // Line and paragraph separators, and every space character, are spaces; a run is one space.
        #expect(AndroidAppText.label("a\u{2028}b\u{2029}c\u{00A0}d\u{3000}e\u{85}f   g", fallback: "x") == "a b c d e f g")
        // Every format character goes but the two joiners: bidirectional marks, zero width, word joiner, BOM, tags.
        for dropped in ["\u{061C}", "\u{200E}", "\u{200F}", "\u{202A}", "\u{202B}", "\u{202C}", "\u{202D}", "\u{202E}",
                        "\u{2066}", "\u{2067}", "\u{2068}", "\u{2069}", "\u{200B}", "\u{2060}", "\u{FEFF}", "\u{180E}",
                        "\u{E0041}", "\u{E007F}", "\u{2061}", "\u{2064}", "\u{00AD}"] {
            #expect(AndroidAppText.label("a\(dropped)b", fallback: "x") == "ab", "U+\(String(dropped.unicodeScalars.first!.value, radix: 16))")
        }
        // The joiners stay, so that emoji sequences and scripts that need them are whole.
        #expect(AndroidAppText.label("\u{1F468}\u{200D}\u{1F469}", fallback: "x") == "\u{1F468}\u{200D}\u{1F469}")
        #expect(AndroidAppText.label("a\u{200C}b", fallback: "x") == "a\u{200C}b")
        // Noncharacters are the replacement character.
        let nonCharacters = String(String.UnicodeScalarView([Unicode.Scalar(0x61)!, Unicode.Scalar(0xFFFF)!, Unicode.Scalar(0x62)!, Unicode.Scalar(0xFDD0)!]))
        #expect(AndroidAppText.label(nonCharacters, fallback: "x") == "a\u{FFFD}b\u{FFFD}")
        // A label of nothing that shows is the package name.
        #expect(AndroidAppText.label("\u{200E}\u{2028}\u{FEFF}", fallback: "com.example.app") == "com.example.app")
    }

    @Test func aLabelIsBoundedByScalarsAndBytesBeforeAnyGraphemeWork() {
        let limits = (scalars: AndroidAppLimits.longestLabelScalars, bytes: AndroidAppLimits.longestLabelBytes)
        // Eighty scalars are left alone, the tool's own cut with its ellipsis included.
        let eighty = String(repeating: "a", count: 79) + "\u{2026}"
        #expect(AndroidAppText.label(eighty, fallback: "x") == eighty)
        // A longer one is cut at a scalar and ends with the ellipsis, within both bounds.
        let long = AndroidAppText.label(String(repeating: "é", count: 1_000), fallback: "x")
        #expect(long.unicodeScalars.count == limits.scalars)
        #expect(long.hasSuffix("\u{2026}") && long.utf8.count <= limits.bytes)
        // The widest scalars stay inside both bounds with the ellipsis.
        let wide = AndroidAppText.label(String(repeating: "\u{1F600}", count: 200), fallback: "x")
        #expect(wide.utf8.count <= limits.bytes && wide.hasSuffix("\u{2026}"))
        #expect(wide.unicodeScalars.count == limits.scalars)
        // One base and a very long run of combining marks is one grapheme and 400,000 bytes: it is cut as scalars.
        let stack = "a" + String(repeating: "\u{0301}", count: 200_000)
        let cut = AndroidAppText.label(stack, fallback: "x")
        #expect(cut.unicodeScalars.count == limits.scalars && cut.utf8.count <= limits.bytes)
        // Text of invisible characters in front does not get past the cut of what is looked at.
        let padded = AndroidAppText.label(String(repeating: "\u{200B}", count: 5_000) + "Comet", fallback: "x")
        #expect(padded == "x")
        // The fallback is held to the same bounds.
        #expect(AndroidAppText.label("", fallback: String(repeating: "a", count: 200)).unicodeScalars.count == limits.scalars)
    }

    @Test func theAppsAreInTheOrderThePersonReads() {
        let apps = [app("com.b.app", "banana"), app("com.a.app", "Apple"), app("com.c.app", "apple"), app("com.d.app", "10 Ten"), app("com.e.app", "9 Nine")]
        let ordered = apps.sorted(by: AndroidAppText.inOrder).map(\.label)
        #expect(ordered.map { $0.lowercased() } == ["9 nine", "10 ten", "apple", "apple", "banana"])
    }
}

struct AndroidAppStoreTests {
    @Test func invalidatingMakesTheNextRefreshListAgain() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let tool = FakeTool(.apps(AndroidAppList(apps: [comet], digest: "d1")))
        tool.icons[comet.id] = .noIcon
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 1))
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 2))
        #expect(tool.listAsks == 1)
        store.invalidate()
        #expect(store.catalog()?.digest == nil)
        #expect(store.catalog()?.apps == [comet])
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 3))
        #expect(tool.listAsks == 2)
        #expect(AndroidAppStore(directory: directory).catalog()?.digest == "d1")
    }

    @Test func aListingForAnOpenAsksNeitherTheDigestNorTheIconsAndAnyOtherRefreshAsksBoth() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let tool = FakeTool(.apps(AndroidAppList(apps: [comet], digest: "d1")))
        tool.icons[comet.id] = .noIcon
        guard case .refreshed = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 1), forAnOpen: true)
        else {
            Issue.record("the listing was not kept")
            return
        }
        #expect(tool.listAsks == 1 && tool.digestAsks == 0 && store.catalog()?.apps == [comet])
        // The apps are the same, and the next open lists them again.
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 2), forAnOpen: true)
        #expect(tool.listAsks == 2 && tool.digestAsks == 0 && tool.askedIcons.isEmpty)
        // Any other refresh asks the digest, and the icons that are missing.
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 3))
        #expect(tool.listAsks == 2 && tool.digestAsks == 1, "the digest matched: nothing is listed")
        #expect(tool.askedIcons == [comet.id], "the next refresh asks for what is missing")
    }

    @Test func theCurrentCatalogsRecordsAreKeptBeforeTheHistoryAtTheBound() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        // A full history dated after the listing that follows (the clock went back).
        let history = (0..<AndroidAppLimits.mostRecords).map { app("com.example.history\($0)", "History \($0)") }
        try store.apply(AndroidAppList(apps: history, digest: "h"), at: Date(timeIntervalSince1970: 1_000))
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet], digest: "d1"), at: Date(timeIntervalSince1970: 1))
        #expect(store.records().count == AndroidAppLimits.mostRecords)
        #expect(store.record(forID: comet.id)?.launchers == 1, "the listed app's record is not the one to go")
        #expect(AndroidAppStore(directory: directory).record(forID: comet.id)?.launchers == 1)
    }

    @Test func aListingIsKeptAndReadBackByAnotherStore() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        #expect(store.catalog() == nil)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet, app("com.android.settings", "Settings", builtIn: true)], digest: "d1"), at: Date(timeIntervalSince1970: 100))
        store.setIcon(wholePNG(mark: 3), for: comet)

        let again = AndroidAppStore(directory: directory)
        let catalog = try #require(again.catalog())
        #expect(catalog.apps.map(\.label) == ["Comet Hop", "Settings"])
        #expect(catalog.digest == "d1")
        #expect(catalog.listedAt == Date(timeIntervalSince1970: 100))
        #expect(again.icon(forID: comet.id) == wholePNG(mark: 3))
        #expect(again.icon(forID: "com.android.settings/com.example.Main") == nil)
        #expect(again.hasIcon(of: comet))
        #expect(!again.hasIcon(of: app("com.android.settings", "Settings", builtIn: true)))
    }

    @Test func anIconOfAnotherTagIsNotTheIconOfTheApp() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let old = app("com.example.comet", "Comet Hop", tag: "1")
        try store.apply(AndroidAppList(apps: [old], digest: nil), at: Date())
        store.setIcon(wholePNG(mark: 1), for: old)
        let changed = app("com.example.comet", "Comet Hop", tag: "2")
        #expect(store.icon(of: changed) == nil)
        try store.apply(AndroidAppList(apps: [changed], digest: nil), at: Date())
        // The old picture went with the tag it belonged to.
        #expect(store.icon(of: old) == nil)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: [changed], digest: nil)) == [changed])
    }

    @Test func anIconThatIsEmptyEnormousOrNotAWholePNGIsNotKept() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet], digest: nil), at: Date())
        store.setIcon(Data(), for: comet)
        #expect(store.icon(of: comet) == nil)
        store.setIcon(wholePNG(AndroidAppLimits.largestIcon), for: comet)
        #expect(store.icon(of: comet) == nil)
        store.setIcon(wholePNG(AndroidAppLimits.largestIcon - 45), for: comet)
        #expect(store.icon(of: comet) != nil)
    }

    @Test func aTruncatedOrUnreadablePictureCountsAsMissing() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let list = AndroidAppList(apps: [comet], digest: nil)
        try store.apply(list, at: Date())
        store.setIcon(wholePNG(mark: 1), for: comet)
        #expect(store.appsMissingIcons(in: list).isEmpty)
        // The file is cut short on disk (a write that did not finish, or a failing disk).
        let file = directory.appendingPathComponent("Catalog/Icons/\(store.iconFileName(for: comet))")
        try wholePNG(mark: 1).prefix(30).write(to: file)
        #expect(store.icon(of: comet) == nil)
        #expect(store.appsMissingIcons(in: list) == [comet])
        // Bytes that are no picture are not kept in the first place.
        #expect(store.setIcon(Data(repeating: 7, count: 100), for: comet) == false)
        #expect(store.icon(of: comet) == nil)
    }

    @Test func aStoreRemembersHowManyLaunchersAPackageHadWhenAnAppWasLastListed() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let one = app("com.example.solo", "Solo")
        let a = app("com.example.pair", "Pair A", activity: "com.example.A")
        let b = app("com.example.pair", "Pair B", activity: "com.example.B")
        try store.apply(AndroidAppList(apps: [one, a, b], digest: nil), at: Date(timeIntervalSince1970: 1))
        #expect(store.record(forID: one.id)?.launchers == 1)
        #expect(store.record(forID: a.id)?.launchers == 2)
        // A is disabled: B is alone now, and A's record still says what was true when A was last listed.
        try store.apply(AndroidAppList(apps: [one, b], digest: nil), at: Date(timeIntervalSince1970: 2))
        #expect(store.record(forID: a.id)?.launchers == 2)
        #expect(store.record(forID: b.id)?.launchers == 1)
        #expect(AndroidAppStore(directory: directory).record(forID: a.id)?.launchers == 2)
    }

    @Test func aRefreshThatTheRunEndingOrAnEraseEndedStoresNothing() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let tool = FakeTool(.apps(AndroidAppList(apps: [comet], digest: "d")))
        tool.icons[comet.id] = .png(wholePNG(mark: 1), hasAlpha: false)
        // The erase comes while the listing is still on its way: its answer is not kept, on this store or the disk.
        tool.duringList = { store.erase() }
        let progress = ProgressLog()
        let result = await AndroidAppRefresh.run(tool: tool, store: store) { catalog, left in progress.add([], left, 0) }
        #expect(result == .failed)
        #expect(store.catalog() == nil)
        #expect(AndroidAppStore(directory: directory).catalog() == nil)
        #expect(progress.entries.isEmpty)
        #expect(tool.askedIcons.isEmpty)
    }

    @Test func aRefreshEndedBetweenTwoIconBatchesStoresNoMoreIcons() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<10).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        for app in apps { tool.icons[app.id] = .png(wholePNG(mark: 1), hasAlpha: false) }
        // The run stops while the first batch is being rendered; its answer arrives all the same.
        tool.duringIcons = { store.invalidateRefreshes() }
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.iconAsks.count == 1)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).count == 10)
    }

    @Test func aCancelledRefreshStoresNoListing() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let tool = FakeTool(.apps(AndroidAppList(apps: [comet], digest: "d")))
        let task = Task {
            // The cancel lands before the listing is applied.
            withUnsafeCurrentTask { $0?.cancel() }
            return await AndroidAppRefresh.run(tool: tool, store: store)
        }
        #expect(await task.value == .failed)
        #expect(store.catalog() == nil)
    }

    @Test func aStaleGenerationIsRefusedByEveryChange() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        let generation = store.refreshGeneration
        try store.apply(AndroidAppList(apps: [comet], digest: "d"), at: Date(), generation: generation)
        #expect(store.setIcon(wholePNG(mark: 1), for: comet, generation: generation))
        store.invalidateRefreshes()
        #expect(throws: AndroidAppStore.StaleRefresh.self) {
            try store.apply(AndroidAppList(apps: [], digest: "e"), at: Date(), generation: generation)
        }
        #expect(store.setIcon(wholePNG(mark: 2), for: comet, generation: generation) == false)
        #expect(store.markNoIcon(for: comet, generation: generation) == false)
        #expect(store.touch(at: Date(), generation: generation) == false)
        #expect(store.catalog()?.digest == "d")
        for ending in [{ store.erase() }, { store.invalidate() }, { store.eraseRecords() }] {
            let before = store.refreshGeneration
            ending()
            #expect(store.refreshGeneration != before)
        }
    }

    @Test func aCatalogFromDiskIsHeldToWhatAListingIs() throws {
        func write(_ json: String, records: String = "[]") throws -> AndroidAppStore {
            let directory = makeDirectory()
            let folder = directory.appendingPathComponent("Catalog", isDirectory: true)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try Data(json.utf8).write(to: folder.appendingPathComponent("catalog.json"))
            try Data(records.utf8).write(to: directory.appendingPathComponent("records.json"))
            return AndroidAppStore(directory: directory)
        }
        func catalog(_ apps: String) -> String { #"{"schema":1,"listedAt":1,"digest":null,"apps":[\#(apps)]}"# }
        func entry(package: String = "com.example.comet", activity: String = "com.example.Main", label: String = "Comet", tag: String = "0123456789abcdef") -> String {
            #"{"component":{"packageName":"\#(package)","activityClass":"\#(activity)"},"label":"\#(label)","builtIn":false,"iconTag":"\#(tag)"}"#
        }
        #expect(try write(catalog(entry())).catalog()?.apps.count == 1)
        // A component that is no component is not read: the launch quoting relies on the grammar.
        #expect(try write(catalog(entry(package: "a.b'; reboot; echo '"))).catalog() == nil)
        #expect(try write(catalog(entry(activity: "x y"))).catalog() == nil)
        // Nor is an icon key that is not the tool's shape, or an app twice.
        #expect(try write(catalog(entry(tag: "short"))).catalog() == nil)
        #expect(try write(catalog(entry() + "," + entry())).catalog() == nil)
        // A label is made again what a screen may show.
        let label = try write(catalog(entry(label: "Comet\\u2028Hop\\u200E"))).catalog()?.apps.first?.label
        #expect(label == "Comet Hop")
        // More apps than a listing holds is not a listing.
        let many = (0...AndroidAppLimits.mostApps).map { entry(package: "com.example.a\($0)") }.joined(separator: ",")
        #expect(try write(catalog(many)).catalog() == nil)
    }

    @Test func recordsFromDiskThatAreNoComponentAreDroppedAndTheRestAreBounded() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var records = [#"{"id":"not a component","label":"X","builtIn":false,"seen":1}"#,
                       #"{"id":"com.example.ok/com.example.Main","label":"Fine\u2028Name","builtIn":false,"seen":2}"#]
        records += (0..<(AndroidAppLimits.mostRecords + 10)).map {
            #"{"id":"com.example.n\#($0)/com.example.Main","label":"N","builtIn":false,"seen":\#(100 + $0)}"#
        }
        try Data("[\(records.joined(separator: ","))]".utf8).write(to: directory.appendingPathComponent("records.json"))
        let store = AndroidAppStore(directory: directory)
        #expect(store.record(forID: "not a component") == nil)
        #expect(store.records().count == AndroidAppLimits.mostRecords)
        // The oldest went first: the one of seen 2 is among them.
        #expect(store.record(forID: "com.example.ok/com.example.Main") == nil)
        #expect(store.record(forID: "com.example.n\(AndroidAppLimits.mostRecords + 9)/com.example.Main")?.label == "N")
    }

    @Test func aFileLargerThanAStoreFileCanBeIsNotRead() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let folder = directory.appendingPathComponent("Catalog", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let big = Data(repeating: 0x20, count: AndroidAppStore.largestFile + 1)
        try big.write(to: folder.appendingPathComponent("catalog.json"))
        try big.write(to: directory.appendingPathComponent("records.json"))
        let store = AndroidAppStore(directory: directory)
        #expect(store.catalog() == nil)
        #expect(store.records().isEmpty)
    }

    @Test func theArtworkRevisionMovesWhenAPictureIsKeptOrTheStoreIsErased() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet], digest: nil), at: Date())
        let first = store.artworkRevision
        store.setIcon(wholePNG(mark: 1), for: comet)
        #expect(store.artworkRevision != first)
        let second = store.artworkRevision
        store.markNoIcon(for: comet)
        #expect(store.artworkRevision != second)
    }

    @Test func anEraseTakesTheAppsAndKeepsTheRecords() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let comet = app("com.example.comet", "Comet Hop")
        try store.apply(AndroidAppList(apps: [comet], digest: "d"), at: Date(timeIntervalSince1970: 5))
        store.setIcon(wholePNG(mark: 9), for: comet)

        store.erase()
        #expect(store.catalog() == nil)
        #expect(store.icon(forID: comet.id) == nil)
        let record = try #require(store.record(forID: comet.id))
        #expect(record.label == "Comet Hop")
        #expect(record.component == comet.component)

        // The records survive a new process too, and a reinstall lists the app again.
        let later = AndroidAppStore(directory: directory)
        #expect(later.catalog() == nil)
        #expect(later.record(forID: comet.id)?.label == "Comet Hop")
        try later.apply(AndroidAppList(apps: [app("com.example.comet", "Comet Hop 2")], digest: "e"), at: Date(timeIntervalSince1970: 9))
        #expect(later.record(forID: comet.id)?.label == "Comet Hop 2")
        #expect(later.catalog()?.apps.count == 1)
    }

    @Test func theRecordsAreBoundedAndTheOldestGoFirst() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let first = (0..<600).map { app("com.example.a\($0)", "A\($0)") }
        try store.apply(AndroidAppList(apps: first, digest: nil), at: Date(timeIntervalSince1970: 1))
        let second = (0..<600).map { app("com.example.b\($0)", "B\($0)") }
        try store.apply(AndroidAppList(apps: second, digest: nil), at: Date(timeIntervalSince1970: 2))
        #expect(store.records().count == AndroidAppLimits.mostRecords)
        #expect(store.record(forID: "com.example.b0/com.example.Main") != nil)
        // The oldest went, which no listing names any more: 1,200 records, 1,024 kept.
        #expect(store.records().filter { $0.id.hasPrefix("com.example.a") }.count == 600 - (1_200 - AndroidAppLimits.mostRecords))
    }

    @Test func aListingLongerThanTheBoundIsCut() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let many = (0..<(AndroidAppLimits.mostApps + 50)).map { app("com.example.a\($0)", "A\($0)") }
        try store.apply(AndroidAppList(apps: many, digest: nil), at: Date())
        #expect(store.catalog()?.apps.count == AndroidAppLimits.mostApps)
    }

    @Test func aCatalogOfAnotherSchemaOrNoJSONReadsAsNone() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let catalogFolder = directory.appendingPathComponent("Catalog", isDirectory: true)
        try FileManager.default.createDirectory(at: catalogFolder, withIntermediateDirectories: true)
        try Data("not json".utf8).write(to: catalogFolder.appendingPathComponent("catalog.json"))
        try Data("[]".utf8).write(to: directory.appendingPathComponent("records.json"))
        let store = AndroidAppStore(directory: directory)
        #expect(store.catalog() == nil)
        #expect(store.records().isEmpty)
    }

    @Test func theStoreIsLeftOutOfTheBackup() throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        try store.apply(AndroidAppList(apps: [app("com.example.comet", "Comet Hop")], digest: nil), at: Date())
        let values = try directory.resourceValues(forKeys: [.isExcludedFromBackupKey])
        #expect(values.isExcludedFromBackup == true)
    }
}

struct AndroidAppRefreshTests {
    @Test func theNamesAreKeptFirstAndTheIconsFollowInBatches() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<10).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps.reversed(), digest: "d")))
        for app in apps { tool.icons[app.id] = .png(wholePNG(mark: 1), hasAlpha: false) }
        let seen = ProgressLog()
        let result = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 7)) { catalog, left in
            seen.add(catalog.apps.map(\.label), left, store.appsMissingIcons(in: AndroidAppList(apps: catalog.apps, digest: nil)).count)
        }
        guard case let .refreshed(catalog) = result else { Issue.record("not refreshed"); return }
        #expect(catalog.apps.map(\.label) == ["A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9"])
        // Eight icons to an ask, then the two that are left.
        #expect(tool.iconAsks.map(\.count) == [8, 2])
        #expect(seen.entries.map(\.left) == [10, 2, 0])
        // Each progress call saw the store as it was then: names first, a batch more each time.
        #expect(seen.entries.map(\.stored) == [10, 2, 0])
        #expect(store.icon(forID: apps[0].id) == wholePNG(mark: 1))
    }

    @Test func anUnchangedDigestAsksForNoListingAndNoIconThatItHas() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let a = app("com.example.aaa", "Aaa")
        let tool = FakeTool(.apps(AndroidAppList(apps: [a], digest: "d")))
        tool.icons = [a.id: .png(wholePNG(mark: 1), hasAlpha: false)]
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 1))
        _ = await AndroidAppRefresh.run(tool: tool, store: store, now: Date(timeIntervalSince1970: 2))
        #expect(tool.listAsks == 1)
        #expect(tool.askedIcons.count == 1)
        #expect(tool.digestAsks == 2)
        // The time of the check is the list's time.
        #expect(store.catalog()?.listedAt == Date(timeIntervalSince1970: 2))
        // A new digest lists again and a new tag asks for the icon again.
        tool.listing = .apps(AndroidAppList(apps: [app("com.example.aaa", "Aaa", tag: "2")], digest: "e"))
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.listAsks == 2)
        #expect(tool.askedIcons.count == 2)
    }

    @Test func anIconOwedAtTheLastRefreshIsAskedForEvenWhenTheDigestHeld() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let a = app("com.example.aaa", "Aaa")
        let tool = FakeTool(.apps(AndroidAppList(apps: [a], digest: "d")))
        tool.icons = [a.id: .error]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(store.icon(forID: a.id) == nil)
        tool.icons = [a.id: .png(wholePNG(mark: 1), hasAlpha: false)]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.listAsks == 1)
        #expect(store.icon(forID: a.id) == wholePNG(mark: 1))
    }

    @Test func anAppWithNoIconIsNotAskedForAgain() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let a = app("com.example.aaa", "Aaa")
        let tool = FakeTool(.apps(AndroidAppList(apps: [a], digest: "d")))
        tool.icons = [a.id: .noIcon]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.askedIcons.count == 1)
        #expect(store.declaresNoIcon(of: a))
        #expect(store.icon(forID: a.id) == nil)
        // A new tag is a new icon.
        tool.listing = .apps(AndroidAppList(apps: [app("com.example.aaa", "Aaa", tag: "2")], digest: "e"))
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.askedIcons.count == 2)
    }

    @Test func aDeferredOrMissingOrUnansweredIconIsAskedForAtTheNextRefresh() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = [app("com.example.a", "A"), app("com.example.b", "B"), app("com.example.c", "C")]
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        tool.icons = [apps[0].id: .deferred, apps[1].id: .missing]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).count == 3)
    }

    @Test func aBatchThatGotNoAnswerIsAskedAgainInSmallerBatches() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<8).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        for app in apps { tool.icons[app.id] = .png(wholePNG(mark: 1), hasAlpha: false) }
        tool.answersOnlyUpTo = 2
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        // Eight, then four, then pairs: every icon arrived in this refresh.
        #expect(tool.iconAsks.map(\.count) == [8, 4, 2, 2, 2, 2])
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).isEmpty)
    }

    @Test func aToolThatAnswersOneAppAtATimeGivesEveryIconInOneRefresh() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<8).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        for app in apps { tool.icons[app.id] = .png(wholePNG(mark: 1), hasAlpha: false) }
        tool.answersOnlyUpTo = 1
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        #expect(tool.iconAsks.map(\.count) == [8, 4, 2, 1, 1, 1, 1, 1, 1, 1, 1])
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).isEmpty)
    }

    @Test func aToolThatNeverAnswersIsGivenUpOnAfterThreeAsksAtOneApp() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = (0..<8).map { app("com.example.a\($0)", "A\($0)") }
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        tool.answersOnlyUpTo = 0
        let result = await AndroidAppRefresh.run(tool: tool, store: store)
        // The halving reaches one app first; only a silence at one app counts against the refresh.
        #expect(tool.iconAsks.map(\.count) == [8, 4, 2, 1, 1, 1])
        guard case .refreshed = result else { Issue.record("the names are kept even when no icon comes: \(result)"); return }
        #expect(store.catalog()?.apps.count == 8)
        #expect(store.appsMissingIcons(in: AndroidAppList(apps: apps, digest: nil)).count == 8)
    }

    @Test func anIconTheToolDeferredIsAskedAgainWithinTheRefreshButNotWithoutEnd() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        let apps = [app("com.example.a", "A"), app("com.example.b", "B")]
        let tool = FakeTool(.apps(AndroidAppList(apps: apps, digest: "d")))
        tool.icons = [apps[0].id: .png(wholePNG(mark: 1), hasAlpha: false), apps[1].id: .deferred]
        tool.deferredOnce = [apps[0].id]
        _ = await AndroidAppRefresh.run(tool: tool, store: store)
        // A came back deferred once and arrived at the second ask; B is deferred every time and is asked three times.
        #expect(store.icon(forID: apps[0].id) == wholePNG(mark: 1))
        #expect(tool.askedIcons.filter { $0 == apps[1].id }.count == 1 + AndroidAppLimits.deferredRetries)
        #expect(store.icon(forID: apps[1].id) == nil)
    }

    @Test func theLimitOfOneIconsAskIsFortySeconds() {
        #expect(AndroidAppLimits.iconLimit == .seconds(40))
    }

    @Test func aListingThatFailsOrIsMissingChangesNothing() async throws {
        let directory = makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let store = AndroidAppStore(directory: directory)
        try store.apply(AndroidAppList(apps: [app("com.example.aaa", "Aaa")], digest: "d"), at: Date())
        let tool = FakeTool(.failed)
        #expect(await AndroidAppRefresh.run(tool: tool, store: store) == .failed)
        tool.listing = .unavailable
        #expect(await AndroidAppRefresh.run(tool: tool, store: store) == .unavailable)
        // A digest that answers but a listing that does not.
        tool.listing = .failed
        tool.digestAnswer = .digest("other")
        #expect(await AndroidAppRefresh.run(tool: tool, store: store) == .failed)
        #expect(store.catalog()?.apps.map(\.label) == ["Aaa"])
    }
}

private final class ProgressLog: @unchecked Sendable {
    struct Entry { var labels: [String]; var left: Int; var stored: Int }
    private let lock = NSLock()
    private var all: [Entry] = []
    var entries: [Entry] { lock.withLock { all } }
    func add(_ labels: [String], _ left: Int, _ stored: Int) { lock.withLock { all.append(Entry(labels: labels, left: left, stored: stored)) } }
}

struct AndroidAppOpenTests {
    @Test func anAppThatOpensIsOpened() async {
        let c = component("com.example.comet")
        let tool = FakeTool(.failed)
        tool.opens[c] = .opened
        #expect(await tool.open(c) == .opened)
        #expect(tool.listAsks == 0)
    }

    @Test func aFailedCommandIsAFailureAndNeverAGuess() async {
        let c = component("com.example.comet")
        let tool = FakeTool(.apps(AndroidAppList(apps: [], digest: nil)))
        tool.opens[c] = .failed
        #expect(await tool.open(c) == .failed)
        #expect(tool.listAsks == 0)
    }

    @Test func aLauncherThatMovedIsFollowedWhenThePackageHadOneAndHasOne() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let new = component("com.example.comet", "com.example.NewMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.NewMain")], digest: nil)))
        tool.opens[old] = .notFound
        tool.opens[new] = .opened
        #expect(await tool.open(old, launchersWhenSeen: 1) == .openedRenamed(new))
        #expect(tool.openAsks == [old, new])
    }

    @Test func aReplacementLaunchIsSentOnlyWhereTheAskerStillWantsIt() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let new = component("com.example.comet", "com.example.NewMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.NewMain")], digest: nil)))
        tool.opens[old] = .notFound
        tool.opens[new] = .opened
        // Asked once, after the listing and before the replacement; an answer of no sends nothing.
        let asked = AskCounter()
        #expect(await tool.open(old, launchersWhenSeen: 1, mayReplace: { asked.add(); return false }) == .failed)
        #expect(asked.value == 1)
        #expect(tool.openAsks == [old], "the first command was sent; the replacement was not")
        #expect(tool.listAsks == 1)
        #expect(await tool.open(old, launchersWhenSeen: 1, mayReplace: { asked.add(); return true }) == .openedRenamed(new))
        #expect(asked.value == 2)
        #expect(tool.openAsks == [old, old, new])
        // A launch that needs no replacement never asks.
        let direct = component("com.example.direct")
        tool.opens[direct] = .opened
        #expect(await tool.open(direct, launchersWhenSeen: 1, mayReplace: { asked.add(); return false }) == .opened)
        #expect(asked.value == 2)
    }

    @Test func anIconIsNeverGivenAnotherLauncherOfAPackageThatHadTwoOrWhoseCountIsUnknown() async {
        // The icon was for A while A and B were both offered; A is disabled now and B is all that is left.
        let a = component("com.example.comet", "com.example.A")
        let b = component("com.example.comet", "com.example.B")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.B")], digest: nil)))
        tool.opens[a] = .notFound
        tool.opens[b] = .opened
        #expect(await tool.open(a, launchersWhenSeen: 2) == .notInAndroid)
        #expect(await tool.open(a, launchersWhenSeen: nil) == .notInAndroid)
        #expect(await tool.open(a) == .notInAndroid)
        #expect(tool.openAsks == [a, a, a])
    }

    @Test func aPackageWithTwoLauncherActivitiesIsNotGuessedAt() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [
            app("com.example.comet", "Comet", activity: "com.example.A"), app("com.example.comet", "Comet 2", activity: "com.example.B"),
        ], digest: nil)))
        tool.opens[old] = .notFound
        #expect(await tool.open(old, launchersWhenSeen: 1) == .notInAndroid)
        #expect(tool.openAsks == [old])
    }

    @Test func aPackageThatIsGoneIsNotInAndroid() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.other", "Other")], digest: nil)))
        tool.opens[old] = .notFound
        #expect(await tool.open(old, launchersWhenSeen: 1) == .notInAndroid)
    }

    @Test func aMissingListingAfterARefusalIsAFailureNotAMissingApp() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let tool = FakeTool(.failed)
        tool.opens[old] = .notFound
        #expect(await tool.open(old, launchersWhenSeen: 1) == .failed)
        tool.listing = .unavailable
        #expect(await tool.open(old, launchersWhenSeen: 1) == .failed)
    }

    @Test func theSameLauncherListedAgainIsNotARename() async {
        let old = component("com.example.comet", "com.example.Main")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.Main")], digest: nil)))
        tool.opens[old] = .notFound
        #expect(await tool.open(old, launchersWhenSeen: 1) == .notInAndroid)
        #expect(tool.openAsks == [old])
    }

    @Test func aRenamedLauncherThatDoesNotOpenEndsAsTheAnswerSays() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let new = component("com.example.comet", "com.example.NewMain")
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.NewMain")], digest: nil)))
        tool.opens[old] = .notFound
        tool.opens[new] = .notFound
        #expect(await tool.open(old, launchersWhenSeen: 1) == .notInAndroid)
        tool.opens[new] = .failed
        #expect(await tool.open(old, launchersWhenSeen: 1) == .failed)
    }

    @Test func aStartThatFailedWithoutAReasonIsExplainedByTheListing() async {
        let old = component("com.example.comet", "com.example.OldMain")
        let new = component("com.example.comet", "com.example.NewMain")
        // Still listed: a failure, and nothing else is opened.
        let tool = FakeTool(.apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.OldMain")], digest: nil)))
        tool.opens[old] = .startFailed
        #expect(await tool.open(old, launchersWhenSeen: 1) == .failed)
        #expect(tool.listAsks == 1)
        // Not listed any more: the app is gone, or it renamed its launcher.
        tool.listing = .apps(AndroidAppList(apps: [], digest: nil))
        #expect(await tool.open(old, launchersWhenSeen: 1) == .notInAndroid)
        tool.listing = .apps(AndroidAppList(apps: [app("com.example.comet", "Comet", activity: "com.example.NewMain")], digest: nil))
        tool.opens[new] = .opened
        #expect(await tool.open(old, launchersWhenSeen: 1) == .openedRenamed(new))
        // A relist that fails is a failure.
        tool.listing = .failed
        #expect(await tool.open(old, launchersWhenSeen: 1) == .failed)
    }

    @Test func anImageWithoutTheToolIsToldSoAndNothingIsRelisted() async {
        let c = component("com.example.comet")
        let tool = FakeTool(.failed)
        tool.opens[c] = .unavailable
        #expect(await tool.open(c, launchersWhenSeen: 1) == .toolMissing)
        #expect(tool.listAsks == 0)
    }
}
