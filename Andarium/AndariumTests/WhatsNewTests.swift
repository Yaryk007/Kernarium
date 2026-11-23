// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import CoreHaptics
import Foundation
import Testing
import UIKit
@testable import Andarium

private extension AppEntry {
    /// The launch's notes for a six-vCPU guest.
    @MainActor
    func announce(on model: AppModel, hasGuest: Bool) {
        announceWhatsNew(on: model, hasGuest: hasGuest, vCPUs: 6)
    }
}

/// The bundled release notes and how a launch announces them: the person's
/// own launch gets the ribbon and keeps its record, any other shows no
/// ribbon and keeps none. Each test keeps into a defaults database of its own.
@MainActor
struct WhatsNewTests {
    private func withDefaults(_ body: (UserDefaults) throws -> Void) rethrows {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        try body(defaults)
    }

    /// An iPhone with haptics and a camera, unless said otherwise.
    private func host(
        maximumMiB: Int, idiom: HostDevice.Idiom = .phone, hasHaptics: Bool = true, hasCamera: Bool = true
    ) -> HostDevice {
        HostDevice(
            name: idiom == .pad ? "iPad" : "iPhone", processorCount: 6,
            memory: MemoryPolicy(maximumsMiB: [maximumMiB]), freeStorageBytes: 40_000_000_000,
            idiom: idiom, hasHaptics: hasHaptics, hasCompass: false, hasCamera: hasCamera)
    }

    private func withDefaultsReturning<Result>(_ body: (UserDefaults) -> Result) -> Result {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        return body(defaults)
    }

    /// An iPhone or iPad running the application, as iOS 27 has it.
    private static let iPadApp = IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false)
    /// The same on an iOS before 27, which has no Shortcuts tile for Add to Home
    /// Screen.
    private static let iPadAppBeforeIOS27 = IPadAppHost(
        isiOSAppOnMac: false, isiOSAppOnVision: false, hasAppTilesInShortcuts: false)

    /// A model for `host`, on a screen that shows `framesPerSecond` at the most
    /// and a system that runs the application as `padApp` says: a ProMotion
    /// screen on iOS 27 unless the test says otherwise, so that Settings offers
    /// Add to Home Screen.
    private func model(
        on host: HostDevice, framesPerSecond: Int = 120, padApp: IPadAppHost = Self.iPadApp
    ) -> AppModel {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(
                version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1.1 (16)"),
            capabilities: .device, createRequirementBytes: 1)
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: framesPerSecond)
        model.playedPadAppHost = padApp
        return model
    }

    private func entry(announces: Bool, build: Int? = 16, defaults: UserDefaults) -> AppEntry {
        var entry = AppEntry(overrides: LaunchOverrides())
        entry.announcesWhatsNew = announces
        entry.build = build
        entry.defaults = defaults
        return entry
    }

    // MARK: The bundled notes

    /// Build 16 as written.
    private static let buildSixteenIDs = [
        "root-access", "aot", "high-refresh", "add-to-home-screen", "voiceover-talkback",
        "frame-rate-when-hot", "text-messages", "update-notice", "build-16-fixes",
    ]

    /// Build 16 as a device without Add to Home Screen is told it.
    private static let buildSixteenIDsWithoutAddToHomeScreen = buildSixteenIDs.filter { $0 != "add-to-home-screen" }

    /// Build 12 as written.
    private static let buildTwelveIDs = ["old-app-compatibility", "fixes-and-improvements"]

    /// Build 11 as written, and as an iPhone with haptics and a camera and an
    /// iPad with a camera are told it.
    private static let buildElevenIDs = [
        "snapshots", "resolution", "ipad-rotation", "keyboard-mouse-controllers", "location", "camera", "haptics",
        "legacy-apk", "more-fixes",
    ]
    private static let buildElevenIDsOnAnIPhone = buildElevenIDs.filter { $0 != "ipad-rotation" }
    private static let buildElevenIDsOnAnIPad = buildElevenIDs.filter { $0 != "haptics" }
    private static let buildNineIDs = ["file-browser", "more-memory", "stability-fixes"]

    /// The notes every build bundles, and so what this test run has.
    @Test func theBundledNotesDecodeAndStartWithBuildSixteen() throws {
        let url = try #require(Bundle.main.url(forResource: "WhatsNew", withExtension: "json"))
        let releases = try WhatsNewCatalog.decode(Data(contentsOf: url))
        #expect(releases.map(\.build) == [16, 12, 11, 9])
        #expect(releases.map(\.version) == ["1.1 (16)", "1.0 (12)", "1.0 (11)", "1.0 (9)"])
        #expect(WhatsNewCatalog.load() == releases)
        let sixteen = try #require(releases.first?.entries)
        #expect(sixteen.map(\.id) == Self.buildSixteenIDs)
        #expect(sixteen.first?.title == "Root access")
        // The four entries about a row of Settings say where it is found.
        #expect(
            sixteen.filter { $0.place == .sessionSettings }.map(\.id)
                == ["add-to-home-screen", "voiceover-talkback", "frame-rate-when-hot", "text-messages"])
        // The fixes are five points under a title, with no sentence of their own.
        let sixteenFixes = try #require(sixteen.last)
        #expect(
            sixteenFixes.title == "Fixes and improvements" && sixteenFixes.text == nil
                && sixteenFixes.points.count == 5)
        #expect(sixteenFixes.place == nil && sixteenFixes.requirement == nil)
        // Build 12's notes are kept as they were.
        let twelve = try #require(releases.dropFirst().first?.entries)
        #expect(twelve.map(\.id) == Self.buildTwelveIDs)
        #expect(twelve.first?.title == "Old app compatibility")
        // The old-app switch is found in Settings.
        #expect(twelve.first?.place == .sessionSettings && twelve.first?.requirement == nil)
        let twelveFixes = try #require(twelve.last)
        #expect(
            twelveFixes.title == "Fixes and improvements" && twelveFixes.text == nil
                && twelveFixes.points.count == 5)
        #expect(twelveFixes.place == nil && twelveFixes.requirement == nil)
        // So are build 11's.
        let eleven = try #require(releases.dropFirst(2).first?.entries)
        #expect(eleven.map(\.id) == Self.buildElevenIDs)
        #expect(eleven.first?.title == "Snapshots")
        let elevenFixes = try #require(eleven.last)
        #expect(elevenFixes.title == "More fixes" && elevenFixes.text == nil && elevenFixes.points.count == 6)
        #expect(elevenFixes.place == nil && elevenFixes.requirement == nil)
        // And build 9's.
        let nine = try #require(releases.last?.entries)
        #expect(nine.map(\.id) == Self.buildNineIDs)
        #expect(nine.first?.title == "File browser")
        let nineFixes = try #require(nine.last)
        #expect(nineFixes.title == "Stability fixes" && nineFixes.text == nil && nineFixes.points.count == 6)
        #expect(nineFixes.place == nil && nineFixes.requirement == nil)
    }

    /// The notes are read from the bundle given, and from nothing else.
    @Test func theNotesAreReadFromTheBundleGiven() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-whatsnew-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let note = #"[{"build": 14, "version": "1.0 (14)", "entries": [{"id": "a", "title": "A", "text": "T", "symbol": "star"}]}]"#
        try Data(note.utf8).write(to: directory.appendingPathComponent("WhatsNew.json"))
        let bundle = try #require(Bundle(url: directory))
        #expect(WhatsNewCatalog.load(from: bundle).map(\.build) == [14])
    }

    @Test func everyEntryHasWordsASymbolAndARequirementTheCodeKnows() {
        for release in WhatsNewCatalog.load() {
            #expect(!release.version.isEmpty)
            for entry in release.entries {
                #expect(!entry.title.isEmpty && (entry.text != nil || !entry.points.isEmpty), "\(entry.id)")
                #expect(UIImage(systemName: entry.symbol) != nil, "\(entry.id): \(entry.symbol)")
            }
        }
        // A requirement the code does not know fails to decode, so the notes
        // would not be here; these five entries are the ones that have any.
        let requirements = WhatsNewCatalog.load().flatMap(\.entries).compactMap { entry in
            entry.requirement.map { (entry.id, $0) }
        }
        #expect(Dictionary(uniqueKeysWithValues: requirements) == [
            "more-memory": WhatsNewRequirement(memoryAboveMiB: 4096),
            "ipad-rotation": WhatsNewRequirement(device: .pad),
            "camera": WhatsNewRequirement(camera: true),
            "haptics": WhatsNewRequirement(device: .haptics),
            "add-to-home-screen": WhatsNewRequirement(feature: .addToHomeScreen),
        ])
    }

    @Test func theRibbonNamesTheBundledEntriesAndCountsTheRest() throws {
        let releases = WhatsNewCatalog.load()
        let sixteenTitles = try #require(releases.first).entries.map(\.title)
        #expect(sixteenTitles == [
            "Root access", "AOT", "120 Hz", "Add to Home Screen", "VoiceOver and TalkBack",
            "Frame rate when hot", "Text messages", "Update notice", "Fixes and improvements",
        ])
        let twelveTitles = try #require(releases.first { $0.build == 12 }).entries.map(\.title)
        #expect(twelveTitles == ["Old app compatibility", "Fixes and improvements"])
        let titles = try #require(releases.first { $0.build == 11 }).entries.map(\.title)
        #expect(titles == [
            "Snapshots", "Resolution", "Rotation on iPad", "Keyboard, mouse and controllers", "Location", "Camera",
            "Haptics", "Install older Android apps", "More fixes",
        ])
        // The dot keeps to the end of its line: a no-break space before it.
        let separator = "\u{00A0}\u{00B7} "
        #expect(WhatsNewRibbon.summary(titles, naming: 9, language: "en") == titles.joined(separator: separator))
        #expect(
            WhatsNewRibbon.summary(titles, naming: 2, language: "en")
                == "Snapshots\(separator)Resolution\(separator)7 more")
        #expect(WhatsNewRibbon.summary(titles, naming: 0) == "9 new features")
        #expect(
            WhatsNewRibbon.summary(sixteenTitles, naming: 3)
                == "Root access\(separator)AOT\(separator)120 Hz\(separator)6 more")
        #expect(WhatsNewRibbon.summary(sixteenTitles, naming: 0) == "9 new features")
        #expect(WhatsNewRibbon.summary(twelveTitles, naming: 0) == "2 new features")
    }

    /// A language that breaks between any two characters gets plain spaces: a
    /// no-break space would pull the last character of a name onto the next
    /// line with its dot.
    @Test func theSeparatorIsNoBreakWhereALanguageBreaksAtSpaces() {
        #expect(WhatsNewRibbon.separator(inLanguage: "en") == "\u{00A0}\u{00B7} ")
        for language in ["zh-Hans", "zh-Hant", "ja", "zh-Hant-TW"] {
            #expect(WhatsNewRibbon.separator(inLanguage: language) == " \u{00B7} ", "\(language)")
        }
        #expect(WhatsNewRibbon.summary(["A", "B", "C"], naming: 1, language: "ja") == "A \u{00B7} 2 more")
    }

    /// The ribbon lists what the device is told, in the order written: nineteen
    /// entries each (an update from build 9 carries builds 16, 12 and 11), but
    /// not the same nineteen.
    @Test func theRibbonNamesWhatAnIPhoneAndAnIPadAreTold() {
        func ribbon(on host: HostDevice, naming named: Int) -> String {
            withDefaultsReturning { defaults in
                WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
                let model = model(on: host)
                entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
                return WhatsNewRibbon.summary(model.whatsNew.pendingEntries.map(\.title), naming: named)
            }
        }
        let separator = "\u{00A0}\u{00B7} "
        let sixteen = [
            "Root access", "AOT", "120 Hz", "Add to Home Screen", "VoiceOver and TalkBack",
            "Frame rate when hot", "Text messages", "Update notice", "Fixes and improvements",
        ]
        let twelve = ["Old app compatibility", "Fixes and improvements"]
        #expect(ribbon(on: host(maximumMiB: 4096), naming: 14) == (sixteen + twelve + [
            "Snapshots", "Resolution", "Keyboard, mouse and controllers", "5 more",
        ]).joined(separator: separator))
        #expect(ribbon(on: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false), naming: 14) == (sixteen + twelve + [
            "Snapshots", "Resolution", "Rotation on iPad", "5 more",
        ]).joined(separator: separator))
        #expect(ribbon(on: host(maximumMiB: 4096), naming: 0) == "19 new features")
        #expect(ribbon(on: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false), naming: 0) == "19 new features")
    }

    @Test func eachKindOfDeviceIsToldItsOwnEntries() {
        func told(by host: HostDevice, padApp: IPadAppHost = Self.iPadApp) -> [String] {
            withDefaultsReturning { defaults in
                WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
                let model = model(on: host, padApp: padApp)
                entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
                // Info lists the notes as written, whatever the device.
                #expect(model.whatsNew.releases.first?.entries.map(\.id) == Self.buildSixteenIDs)
                #expect(model.whatsNew.releases.dropFirst().first?.entries.map(\.id) == Self.buildTwelveIDs)
                #expect(model.whatsNew.releases.dropFirst(2).first?.entries.map(\.id) == Self.buildElevenIDs)
                return model.whatsNew.pendingEntries.map(\.id)
            }
        }
        let withoutDeviceEntries = Self.buildElevenIDs.filter { $0 != "ipad-rotation" && $0 != "haptics" }
        let withoutACamera = { (ids: [String]) in ids.filter { $0 != "camera" } }
        let newer = Self.buildSixteenIDs + Self.buildTwelveIDs
        // A Mac and a Vision Pro show no Add to Home Screen.
        let newerWithoutFeatures = Self.buildSixteenIDsWithoutAddToHomeScreen + Self.buildTwelveIDs
        #expect(told(by: host(maximumMiB: 4096)) == newer + Self.buildElevenIDsOnAnIPhone)
        #expect(
            told(by: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false))
                == newer + Self.buildElevenIDsOnAnIPad)
        #expect(told(by: host(maximumMiB: 4096, hasHaptics: false)) == newer + withoutDeviceEntries)
        // A device with no camera (a Mac running the iPad app has none) is not
        // told about the Camera row, which would not be there.
        #expect(
            told(by: host(maximumMiB: 4096, hasCamera: false))
                == withoutACamera(newer + Self.buildElevenIDsOnAnIPhone))
        #expect(
            told(
                by: host(maximumMiB: 4096, idiom: .mac, hasHaptics: false, hasCamera: false),
                padApp: IPadAppHost(isiOSAppOnMac: true, isiOSAppOnVision: false))
                == withoutACamera(newerWithoutFeatures + withoutDeviceEntries))
        // The iPad app on a Vision Pro has none either, whatever it finds.
        let vision = CameraPlatform(isiOSAppOnMac: false, isiOSAppOnVision: true, hasFrontCamera: { true })
        #expect(
            told(
                by: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false, hasCamera: vision.hasCamera),
                padApp: IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: true))
                == withoutACamera(newerWithoutFeatures + Self.buildElevenIDsOnAnIPad))
    }

    // MARK: The entry about a feature that only some devices show

    /// What an update from build 12 announces to `host`, and the checks that
    /// hold on every device: the ribbon and the notes name the same entries, and
    /// Info lists build 16 as written.
    private func idsAnnounced(
        to host: HostDevice, framesPerSecond: Int = 120, padApp: IPadAppHost = Self.iPadApp
    ) -> [String] {
        withDefaultsReturning { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 12))
            let model = model(on: host, framesPerSecond: framesPerSecond, padApp: padApp)
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            let ids = model.whatsNew.pendingEntries.map(\.id)
            #expect(model.whatsNew.announced.flatMap { $0.entries.map(\.id) } == ids)
            #expect(model.whatsNew.showsRibbon == !ids.isEmpty)
            #expect(model.whatsNew.releases.first?.entries.map(\.id) == Self.buildSixteenIDs)
            return ids
        }
    }

    @Test func aDeviceThatShowsAddToHomeScreenIsToldEveryEntry() {
        #expect(idsAnnounced(to: host(maximumMiB: 4096)) == Self.buildSixteenIDs)
        #expect(idsAnnounced(to: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false)) == Self.buildSixteenIDs)
    }

    /// An iOS before 27 has no Shortcuts tile for the Add to Home Screen guide,
    /// so Settings offers none.
    @Test func aSystemBeforeIOS27IsNotToldAboutAddToHomeScreen() {
        #expect(
            idsAnnounced(to: host(maximumMiB: 4096), padApp: Self.iPadAppBeforeIOS27)
                == Self.buildSixteenIDsWithoutAddToHomeScreen)
    }

    @Test func theIPadAppOnAMacOrAVisionProIsNotToldAboutAddToHomeScreen() {
        #expect(
            idsAnnounced(
                to: host(maximumMiB: 4096, idiom: .mac, hasHaptics: false, hasCamera: false),
                padApp: IPadAppHost(isiOSAppOnMac: true, isiOSAppOnVision: false))
                == Self.buildSixteenIDsWithoutAddToHomeScreen)
        #expect(
            idsAnnounced(
                to: host(maximumMiB: 4096, idiom: .pad, hasHaptics: false, hasCamera: false),
                padApp: IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: true))
                == Self.buildSixteenIDsWithoutAddToHomeScreen)
    }

    /// The entry about 120 Hz is for every device, whatever its screen shows:
    /// it names the setting, and a device without the switch has no other
    /// reason to be left out of the news.
    @Test func the120HzEntryIsToldWhateverTheScreenShows() {
        for framesPerSecond in [60, 120] {
            #expect(idsAnnounced(to: host(maximumMiB: 4096), framesPerSecond: framesPerSecond) == Self.buildSixteenIDs)
            #expect(
                idsAnnounced(
                    to: host(maximumMiB: 4096), framesPerSecond: framesPerSecond, padApp: Self.iPadAppBeforeIOS27)
                    == Self.buildSixteenIDsWithoutAddToHomeScreen)
        }
        #expect(Self.buildSixteenIDsWithoutAddToHomeScreen.contains("high-refresh"))
    }

    /// The entries a device does not show are not in the ribbon's count or names
    /// either.
    @Test func theRibbonCountsAndNamesOnlyTheEntriesTheDeviceShows() {
        func ribbon(framesPerSecond: Int, padApp: IPadAppHost, naming named: Int) -> String {
            withDefaultsReturning { defaults in
                WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 12))
                let model = model(on: host(maximumMiB: 4096), framesPerSecond: framesPerSecond, padApp: padApp)
                entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
                return WhatsNewRibbon.summary(model.whatsNew.pendingEntries.map(\.title), naming: named)
            }
        }
        let separator = "\u{00A0}\u{00B7} "
        for framesPerSecond in [60, 120] {
            #expect(ribbon(framesPerSecond: framesPerSecond, padApp: Self.iPadApp, naming: 0) == "9 new features")
            #expect(
                ribbon(framesPerSecond: framesPerSecond, padApp: Self.iPadAppBeforeIOS27, naming: 0)
                    == "8 new features")
        }
        // Where Add to Home Screen is not shown, the names skip it.
        #expect(
            ribbon(framesPerSecond: 60, padApp: Self.iPadAppBeforeIOS27, naming: 5)
                == ["Root access", "AOT", "120 Hz", "VoiceOver and TalkBack", "Frame rate when hot", "3 more"]
                .joined(separator: separator))
    }

    /// The launch offers the whole release, and the notes ask the feature's own
    /// rule each time they are drawn.
    @Test func theNotesAskTheFeaturesRuleEachTimeTheyAreDrawn() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 12))
            let model = model(on: host(maximumMiB: 4096), padApp: Self.iPadAppBeforeIOS27)
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pending.flatMap { $0.entries.map(\.id) } == Self.buildSixteenIDs)
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildSixteenIDsWithoutAddToHomeScreen)
            model.playedPadAppHost = Self.iPadApp
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildSixteenIDs)
            model.playedPadAppHost = Self.iPadAppBeforeIOS27
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildSixteenIDsWithoutAddToHomeScreen)
            // The record is the launch's: what the notes show does not touch it.
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 12, ribbonBuild: 16, ribbonLaunches: 1))
        }
    }

    /// No second rule: Add to Home Screen is shown exactly where Settings offers
    /// it, whatever the device, and the 120 Hz entry is shown everywhere.
    @Test func theEntryAboutAddToHomeScreenIsShownExactlyWhereSettingsOffersIt() {
        let hosts = [
            host(maximumMiB: 4096), host(maximumMiB: 4096, idiom: .pad, hasHaptics: false),
            host(maximumMiB: 4096, idiom: .mac, hasHaptics: false, hasCamera: false),
        ]
        let systems = [
            Self.iPadApp,
            Self.iPadAppBeforeIOS27,
            IPadAppHost(isiOSAppOnMac: true, isiOSAppOnVision: false),
            IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: true),
        ]
        var answers = Set<Bool>()
        for host in hosts {
            for padApp in systems {
                for framesPerSecond in [60, 120] {
                    withDefaults { defaults in
                        WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 12))
                        let model = model(on: host, framesPerSecond: framesPerSecond, padApp: padApp)
                        entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
                        let ids = model.whatsNew.pendingEntries.map(\.id)
                        let label = "\(host.name) \(padApp) at \(framesPerSecond)"
                        #expect(ids.contains("add-to-home-screen") == model.hostOffersAddToHomeScreen, "\(label)")
                        #expect(ids.contains("high-refresh"), "\(label)")
                        answers.insert(model.hostOffersAddToHomeScreen)
                    }
                }
            }
        }
        // Both answers occur in the cases above, so neither check is vacuous.
        #expect(answers == [true, false])
    }

    /// A release whose every entry the device hides shows no ribbon, though the
    /// launch offered it.
    @Test func aReleaseWhoseEntriesAreAllHiddenShowsNoRibbon() {
        let hidden = WhatsNewRelease(build: 16, version: "1.1 (16)", entries: [
            WhatsNewEntry(
                id: "a", title: "A", text: "T", symbol: "star",
                requirement: WhatsNewRequirement(feature: .addToHomeScreen)),
        ])
        let model = model(on: host(maximumMiB: 4096), padApp: Self.iPadAppBeforeIOS27)
        model.whatsNew.launch(
            releases: [hidden], record: WhatsNewRecord(seenBuild: 12), currentBuild: 16, hasGuest: true,
            offers: { _ in true })
        #expect(model.whatsNew.pending.map(\.build) == [16])
        #expect(model.whatsNew.announced.isEmpty && model.whatsNew.pendingEntries.isEmpty)
        #expect(!model.whatsNew.showsRibbon)
        model.playedPadAppHost = Self.iPadApp
        #expect(model.whatsNew.showsRibbon && model.whatsNew.pendingEntries.map(\.id) == ["a"])
    }

    @Test func aLaunchThatIsNotThePersonsListsTheNotesWithoutARibbon() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: false, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
            #expect(model.whatsNew.releases.map(\.build) == [16, 12, 11, 9])
            #expect(WhatsNewStore(defaults: defaults).record == nil)
        }
    }

    /// The host the application builds reports what the entries ask about, and
    /// not what a default would say: asked of the platform itself, so that an
    /// iPad Simulator (which is not an iPhone) tells a missing report from a
    /// right one. A Simulator has no Taptic Engine, so the haptics report is
    /// the platform's `false`; that it is read at all is a device check.
    @Test func theHostReportsItsKindAndWhetherItHasHaptics() {
        let host = AppEntry.hostDevice()
        let platform = UIDevice.current.userInterfaceIdiom
        if ProcessInfo.processInfo.isiOSAppOnMac {
            #expect(host.idiom == .mac && host.name == "Mac")
        } else if platform == .pad {
            #expect(host.idiom == .pad && host.name == "iPad")
        } else {
            #expect(platform == .phone && host.idiom == .phone && host.name == "iPhone")
        }
        #expect(host.hasHaptics == CHHapticEngine.capabilitiesForHardware().supportsHaptics)
        #expect(host.hasCamera == SystemCameraSource.hasCamera)
    }

    /// The camera a host reports follows the platform it is given: an iPad app
    /// on a Vision Pro has none, whatever cameras it finds, and a device that
    /// finds a front camera has one.
    @Test func aHostThatRunsTheIPadAppOnAVisionProHasNoCamera() {
        let vision = CameraPlatform(isiOSAppOnMac: false, isiOSAppOnVision: true, hasFrontCamera: { true })
        #expect(!AppEntry.hostDevice(camera: vision).hasCamera)
        let elsewhere = CameraPlatform(isiOSAppOnMac: false, isiOSAppOnVision: false, hasFrontCamera: { true })
        #expect(AppEntry.hostDevice(camera: elsewhere).hasCamera)
    }

    // MARK: The person's launch

    @Test func anUpdateFromBuildFiveAnnouncesTheNotesAndKeepsTheRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let host = host(maximumMiB: 6144)
            let model = model(on: host)
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.showsRibbon)
            // Build 16's notes first, then builds 12, 11 and 9 the person skipped.
            #expect(model.whatsNew.pending.map(\.build) == [16, 12, 11, 9])
            #expect(model.whatsNew.pendingEntries.map(\.id)
                == Self.buildSixteenIDs + Self.buildTwelveIDs + Self.buildElevenIDsOnAnIPhone + Self.buildNineIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 5, ribbonBuild: 16, ribbonLaunches: 1))
            #expect(model.whatsNew.releases.map(\.build) == [16, 12, 11, 9])
        }
    }

    /// The update of the public release before this one: only build 16's notes.
    @Test func anUpdateFromBuildTwelveAnnouncesOnlyBuildSixteensNotes() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 12))
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pending.map(\.build) == [16])
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildSixteenIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 12, ribbonBuild: 16, ribbonLaunches: 1))
            // Info lists every release.
            #expect(model.whatsNew.releases.map(\.build) == [16, 12, 11, 9])
        }
    }

    @Test func anUpdateFromBuildElevenAnnouncesBuildsTwelveAndSixteen() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 11))
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pending.map(\.build) == [16, 12])
            #expect(model.whatsNew.pendingEntries.map(\.id) == Self.buildSixteenIDs + Self.buildTwelveIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 11, ribbonBuild: 16, ribbonLaunches: 1))
        }
    }

    @Test func anUpdateFromBuildNineAnnouncesBuildsElevenTwelveAndSixteen() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 9))
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pending.map(\.build) == [16, 12, 11])
            #expect(model.whatsNew.pendingEntries.map(\.id)
                == Self.buildSixteenIDs + Self.buildTwelveIDs + Self.buildElevenIDsOnAnIPhone)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 9, ribbonBuild: 16, ribbonLaunches: 1))
            #expect(model.whatsNew.releases.map(\.build) == [16, 12, 11, 9])
        }
    }

    @Test func anInstallWithAGuestAndNoRecordIsAnUpdateFromBuildFive() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.showsRibbon)
        }
    }

    @Test func aDeviceThatOffersNoMoreThanFourGiBIsNotToldAboutMoreMemory() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 4096))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(model.whatsNew.pendingEntries.map(\.id)
                == Self.buildSixteenIDs + Self.buildTwelveIDs + Self.buildElevenIDsOnAnIPhone
                    + ["file-browser", "stability-fixes"])
            // Info lists the notes as written.
            #expect(model.whatsNew.releases.map(\.entries.count) == [9, 2, 9, 3])
        }
    }

    @Test func aNewInstallAnnouncesNothingAndRecordsThisBuild() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: false)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 16))
        }
    }

    /// Notes that cannot be read, or none: the launch shows no ribbon and no
    /// history, and keeps no record, whatever the install had. (Info lists the
    /// history only when the model has releases.)
    @Test func noNotesShowNothingAndKeepNoRecord() {
        let installs: [(record: WhatsNewRecord?, hasGuest: Bool)] = [
            (nil, false), (nil, true), (WhatsNewRecord(seenBuild: 5), true), (WhatsNewRecord(seenBuild: 11), true),
        ]
        for install in installs {
            withDefaults { defaults in
                install.record.map { WhatsNewStore(defaults: defaults).keep($0) }
                let model = model(on: host(maximumMiB: 6144))
                entry(announces: true, build: 16, defaults: defaults)
                    .announceWhatsNew(on: model, hasGuest: install.hasGuest, vCPUs: 6, notes: [])
                #expect(!model.whatsNew.showsRibbon)
                #expect(model.whatsNew.pendingEntries.isEmpty)
                #expect(model.whatsNew.releases.isEmpty)
                #expect(WhatsNewStore(defaults: defaults).record == install.record)
                model.whatsNew.read()
                model.whatsNew.putAway()
                #expect(WhatsNewStore(defaults: defaults).record == install.record)
            }
        }
    }

    @Test func buildFiveAndEarlierAnnounceNothing() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 4))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 5, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
        }
    }

    /// A build numbered 8 that bundles the notes written for builds 9, 11, 12
    /// and 16 offers none of them and records itself; build 9 then offers only
    /// its own, build 11 only its own after the person has read them, build 12
    /// only its own after the person has read those, and build 16 the same.
    @Test func buildEightAnnouncesNothingThenEachBuildItsOwnAfterTheLastWasRead() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let eight = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 8, defaults: defaults).announce(on: eight, hasGuest: true)
            #expect(!eight.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 8))
            // Info lists the notes as bundled.
            #expect(eight.whatsNew.releases.map(\.build) == [16, 12, 11, 9])

            let nine = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 9, defaults: defaults).announce(on: nine, hasGuest: true)
            #expect(nine.whatsNew.pending.map(\.build) == [9])
            #expect(nine.whatsNew.pendingEntries.map(\.id) == Self.buildNineIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 8, ribbonBuild: 9, ribbonLaunches: 1))
            nine.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 9))

            let eleven = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 11, defaults: defaults).announce(on: eleven, hasGuest: true)
            #expect(eleven.whatsNew.pending.map(\.build) == [11])
            #expect(eleven.whatsNew.pendingEntries.map(\.id) == Self.buildElevenIDsOnAnIPhone)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 9, ribbonBuild: 11, ribbonLaunches: 1))
            eleven.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 11))

            let twelve = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: 12, defaults: defaults).announce(on: twelve, hasGuest: true)
            #expect(twelve.whatsNew.pending.map(\.build) == [12])
            #expect(twelve.whatsNew.pendingEntries.map(\.id) == Self.buildTwelveIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 11, ribbonBuild: 12, ribbonLaunches: 1))
            twelve.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 12))

            let sixteen = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: sixteen, hasGuest: true)
            #expect(sixteen.whatsNew.pending.map(\.build) == [16])
            #expect(sixteen.whatsNew.pendingEntries.map(\.id) == Self.buildSixteenIDs)
            #expect(WhatsNewStore(defaults: defaults).record
                == WhatsNewRecord(seenBuild: 12, ribbonBuild: 16, ribbonLaunches: 1))
        }
    }

    @Test func aBuildWithoutANumberAnnouncesNothingAndKeepsNothing() {
        withDefaults { defaults in
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, build: nil, defaults: defaults).announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == nil)
        }
    }

    @Test func readingTheNotesAndPuttingTheRibbonAwayAreKept() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: model, hasGuest: true)
            model.whatsNew.read()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 16))
            #expect(model.whatsNew.showsRibbon)
            model.whatsNew.putAway()
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 16))

            // The next launch of the same build offers nothing.
            let next = self.model(on: host(maximumMiB: 6144))
            entry(announces: true, defaults: defaults).announce(on: next, hasGuest: true)
            #expect(!next.whatsNew.showsRibbon)
        }
    }

    // MARK: A harness's launch

    @Test func aLaunchThatIsNotThePersonsShowsNoRibbonAndKeepsNoRecord() {
        withDefaults { defaults in
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 5))
            let model = model(on: host(maximumMiB: 6144))
            let launch = entry(announces: false, defaults: defaults)
            launch.announce(on: model, hasGuest: true)
            #expect(!model.whatsNew.showsRibbon)
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 5))
            model.whatsNew.read()
            model.whatsNew.putAway()
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 5))
            // Info still lists the notes.
            #expect(model.whatsNew.releases.map(\.build) == [16, 12, 11, 9])

            // The record of an install that has none stays none.
            withDefaults { fresh in
                let model = self.model(on: host(maximumMiB: 6144))
                self.entry(announces: false, defaults: fresh).announce(on: model, hasGuest: true)
                #expect(WhatsNewStore(defaults: fresh).record == nil)
            }
        }
    }

    @Test func onlyALaunchWithNoSettingAndOutsideATestAnnounces() {
        func announces(_ environment: [String: String]) -> Bool {
            AppEntry.isPersonsLaunch(
                overrides: LaunchOverrides(environment: environment, processorCount: 6), environment: environment)
        }
        #expect(announces([:]))
        #expect(announces(["HOME": "/"]))
        #expect(!announces(["ANDARIUM_START": "normal"]))
        #expect(!announces(["ANDARIUM_GUEST_INSTANCE": "x"]))
        #expect(!announces(["KERNARIUM_VCPU_COUNT": "2"]))
        #expect(!announces(["ANDARIUM_STATS": "0"]))
        #expect(!announces(["ANDARIUM_JIT": "0"]))
        #expect(!announces(["XCTestConfigurationFilePath": "/tmp/x"]))
    }

    /// The process's own model is the real `makeModel`'s: Info lists the notes
    /// and the test run's launch announces none. (A test does not build a
    /// model of its own here: a `LiveRuntime` that is released takes Core
    /// Motion's device down with it, which crashes a Mac.)
    @Test func theTestRunsOwnLaunchIsAHarnessLaunch() {
        #expect(!AppEntry.fromArguments().announcesWhatsNew)
        let model = AndariumProcess.shared.model
        #expect(!model.whatsNew.showsRibbon)
        #expect(model.whatsNew.releases.map(\.build) == [16, 12, 11, 9])
    }
}
