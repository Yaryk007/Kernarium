// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// A defaults database of its own for one test, removed after it.
private func withDefaults<Result>(_ body: (UserDefaults) throws -> Result) rethrows -> Result {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-whatsnew-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let name = directory.appendingPathComponent("defaults").path
    let defaults = UserDefaults(suiteName: name)!
    defer {
        defaults.removePersistentDomain(forName: name)
        try? FileManager.default.removeItem(at: directory)
    }
    return try body(defaults)
}

private func release(_ build: Int, _ ids: [String]) -> WhatsNewRelease {
    WhatsNewRelease(
        build: build, version: "1.0 (\(build))",
        entries: ids.map { WhatsNewEntry(id: $0, title: $0, text: "T", symbol: "star") })
}

/// Releases 4 and 5 stand for earlier ones, 7 for a later one, and 4 for one
/// with nothing to announce.
private let releases = [release(6, ["a", "b", "c"]), release(5, ["d"]), release(4, []), release(7, ["z"])]
private let all: @Sendable (WhatsNewEntry) -> Bool = { _ in true }

private func host(maximumMiB: Int, hasCompass: Bool = false, hasCamera: Bool = false) -> HostDevice {
    HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [maximumMiB]),
        freeStorageBytes: 40_000_000_000, hasCompass: hasCompass, hasCamera: hasCamera)
}

private func device(_ idiom: HostDevice.Idiom, hasHaptics: Bool = false) -> HostDevice {
    HostDevice(
        name: "\(idiom)", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [3072]),
        freeStorageBytes: 40_000_000_000, idiom: idiom, hasHaptics: hasHaptics, hasCompass: false, hasCamera: false)
}

private let iPhone = device(.phone, hasHaptics: true)
private let iPhoneWithoutHaptics = device(.phone)
private let iPad = device(.pad)
/// An iPad application running on a Mac.
private let mac = device(.mac)

struct WhatsNewRulesTests {
    @Test func aNewInstallAnnouncesNothingAndRecordsThisBuild() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: nil, currentBuild: 6, hasGuest: false, offers: all)
        #expect(launch.pending.isEmpty)
        #expect(launch.record == WhatsNewRecord(seenBuild: 6))
    }

    @Test func anInstallWithAGuestAndNoRecordIsAnUpdateFromTheLastBuildBeforeRecords() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: nil, currentBuild: 6, hasGuest: true, offers: all)
        #expect(WhatsNewRules.buildBeforeRecords == 5)
        #expect(launch.pending.map(\.build) == [6])
        #expect(launch.record == WhatsNewRecord(seenBuild: 5, ribbonBuild: 6, ribbonLaunches: 1))
    }

    @Test func skippedUpdatesAreGatheredNewestFirstWithoutEmptyOrLaterReleases() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: WhatsNewRecord(seenBuild: 3), currentBuild: 6, hasGuest: true, offers: all)
        #expect(launch.pending.map(\.build) == [6, 5])
    }

    @Test func notesWrittenForALaterBuildWaitForItAndTheRecordMovesOn() {
        let later = [release(12, ["x", "y"])]
        let eleven = WhatsNewRules.launch(
            releases: later, record: WhatsNewRecord(seenBuild: 5), currentBuild: 11, hasGuest: true, offers: all)
        #expect(eleven.pending.isEmpty)
        #expect(eleven.record == WhatsNewRecord(seenBuild: 11))

        // Build 12 then offers them, after build 11 and straight from build 5.
        let afterEleven = WhatsNewRules.launch(
            releases: later, record: eleven.record, currentBuild: 12, hasGuest: true, offers: all)
        #expect(afterEleven.pending.map(\.build) == [12])
        #expect(afterEleven.pending.first?.entries.map(\.id) == ["x", "y"])
        #expect(afterEleven.record == WhatsNewRecord(seenBuild: 11, ribbonBuild: 12, ribbonLaunches: 1))
        let fromFive = WhatsNewRules.launch(
            releases: later, record: nil, currentBuild: 12, hasGuest: true, offers: all)
        #expect(fromFive.pending.map(\.build) == [12])
        #expect(fromFive.record == WhatsNewRecord(seenBuild: 5, ribbonBuild: 12, ribbonLaunches: 1))
    }

    @Test func theRibbonIsOfferedOnThreeLaunchesThenGoesAsIfPutAway() {
        var record = WhatsNewRecord(seenBuild: 5)
        for number in 1...4 {
            let launch = WhatsNewRules.launch(
                releases: releases, record: record, currentBuild: 6, hasGuest: true, offers: all)
            record = launch.record
            #expect(launch.pending.isEmpty == (number > WhatsNewRules.ribbonLaunchLimit), "launch \(number)")
        }
        #expect(WhatsNewRules.ribbonLaunchLimit == 3)
        #expect(record == WhatsNewRecord(seenBuild: 6))
    }

    /// A list with no release announces nothing on any install. (The
    /// application keeps no record for it either: `AppEntry.announceWhatsNew`.)
    @Test func notesWithNoReleaseAnnounceNothing() {
        let installs: [(WhatsNewRecord?, Bool)] = [
            (nil, false), (nil, true), (WhatsNewRecord(seenBuild: 5), true),
            (WhatsNewRecord(seenBuild: 9, ribbonBuild: 12, ribbonLaunches: 2), true),
        ]
        for (record, hasGuest) in installs {
            let launch = WhatsNewRules.launch(
                releases: [], record: record, currentBuild: 12, hasGuest: hasGuest, offers: all)
            #expect(launch.pending.isEmpty)
        }
    }

    /// Without a record, an install that has a guest is an update from the
    /// first public release, so notes that come to a track later reach an
    /// install that ran its earlier, empty builds.
    @Test func aTrackThatGainsNotesLaterOffersThemToAnInstallThatKeptNoRecord() {
        let later = [release(14, ["a"])]
        let launch = WhatsNewRules.launch(
            releases: later, record: nil, currentBuild: 14, hasGuest: true, offers: all)
        #expect(launch.pending.map(\.build) == [14])
        #expect(launch.record == WhatsNewRecord(seenBuild: 5, ribbonBuild: 14, ribbonLaunches: 1))
        // A build numbered below the notes offers none of them yet.
        let before = WhatsNewRules.launch(
            releases: later, record: nil, currentBuild: 13, hasGuest: true, offers: all)
        #expect(before.pending.isEmpty)
        #expect(before.record == WhatsNewRecord(seenBuild: 13))
    }

    @Test func aBuildOlderThanTheLastSeenAnnouncesNothingAndKeepsTheRecord() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: WhatsNewRecord(seenBuild: 6), currentBuild: 5, hasGuest: true, offers: all)
        #expect(launch.pending.isEmpty)
        #expect(launch.record == WhatsNewRecord(seenBuild: 6))
    }

    @Test func theSameBuildAgainAfterItsNotesWereSeenAnnouncesNothing() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: WhatsNewRecord(seenBuild: 6), currentBuild: 6, hasGuest: true, offers: all)
        #expect(launch.pending.isEmpty)
        #expect(launch.record == WhatsNewRecord(seenBuild: 6))
    }

    @Test func aNewBuildCountsItsLaunchesAgain() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: WhatsNewRecord(seenBuild: 5, ribbonBuild: 6, ribbonLaunches: 2),
            currentBuild: 7, hasGuest: true, offers: all)
        #expect(launch.pending.map(\.build) == [7, 6])
        #expect(launch.record.ribbonLaunches == 1)
    }

    @Test func whenNoEntryFitsTheDeviceNothingIsAnnouncedAndTheRecordMovesOn() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: WhatsNewRecord(seenBuild: 5), currentBuild: 6, hasGuest: true,
            offers: { $0.id == "none" })
        #expect(launch.pending.isEmpty)
        #expect(launch.record == WhatsNewRecord(seenBuild: 6))
    }

    @Test func onlyTheEntriesTheDeviceOffersAreAnnounced() {
        let launch = WhatsNewRules.launch(
            releases: releases, record: WhatsNewRecord(seenBuild: 5), currentBuild: 6, hasGuest: true,
            offers: { $0.id != "b" })
        #expect(launch.pending.first?.entries.map(\.id) == ["a", "c"])
    }

    @Test func aRequirementOfMemoryNeedsMoreThanThatManyMiBAtTheVCPUCount() {
        let requirement = WhatsNewRequirement(memoryAboveMiB: 4096)
        #expect(!requirement.isMet(by: host(maximumMiB: 4096), vCPUs: 6))
        #expect(requirement.isMet(by: host(maximumMiB: 4608), vCPUs: 6))
        #expect(WhatsNewRequirement(memoryAboveMiB: 4096).isMet(by: host(maximumMiB: 8192), vCPUs: 1))
    }

    @Test func aPadRequirementIsMetByAnIPadAndNothingElse() {
        let requirement = WhatsNewRequirement(device: .pad)
        #expect(requirement.isMet(by: iPad, vCPUs: 6))
        #expect(!requirement.isMet(by: iPhone, vCPUs: 6))
        #expect(!requirement.isMet(by: iPhoneWithoutHaptics, vCPUs: 6))
        #expect(!requirement.isMet(by: mac, vCPUs: 6))
    }

    @Test func aHapticsRequirementIsMetByADeviceThatReportsHaptics() {
        let requirement = WhatsNewRequirement(device: .haptics)
        #expect(requirement.isMet(by: iPhone, vCPUs: 6))
        #expect(!requirement.isMet(by: iPhoneWithoutHaptics, vCPUs: 6))
        #expect(!requirement.isMet(by: iPad, vCPUs: 6))
        #expect(!requirement.isMet(by: mac, vCPUs: 6))
    }

    @Test func aRequirementOfSeveralThingsNeedsAllOfThem() {
        let requirement = WhatsNewRequirement(memoryAboveMiB: 4096, device: .pad)
        #expect(!requirement.isMet(by: iPad, vCPUs: 6))
        #expect(!requirement.isMet(by: device(.phone, hasHaptics: true), vCPUs: 6))
        var big = iPad
        big.memory = MemoryPolicy(maximumsMiB: [8192])
        #expect(requirement.isMet(by: big, vCPUs: 6))
        var bigPhone = iPhone
        bigPhone.memory = MemoryPolicy(maximumsMiB: [8192])
        #expect(!requirement.isMet(by: bigPhone, vCPUs: 6))
    }

    @Test func aDeviceTheNotesDoNotNameIsOfferedAnEntryWithoutARequirement() {
        let entry = WhatsNewEntry(id: "a", title: "A", text: "T", symbol: "star")
        for host in [iPhone, iPhoneWithoutHaptics, iPad, mac] {
            #expect(entry.requirement?.isMet(by: host, vCPUs: 6) ?? true)
        }
    }

    /// An update from build 8: each device is told its own entries, in the
    /// order they are written, and never another kind's.
    @Test func eachKindOfDeviceIsAnnouncedItsOwnEntries() {
        let entries = [
            WhatsNewEntry(id: "everyone", title: "E", text: "T", symbol: "star"),
            WhatsNewEntry(
                id: "pad", title: "P", text: "T", symbol: "star", requirement: WhatsNewRequirement(device: .pad)),
            WhatsNewEntry(
                id: "haptics", title: "H", text: "T", symbol: "star",
                requirement: WhatsNewRequirement(device: .haptics)),
            WhatsNewEntry(id: "last", title: "L", text: "T", symbol: "star"),
        ]
        let notes = [WhatsNewRelease(build: 9, version: "1.0 (9)", entries: entries)]
        func announced(to host: HostDevice) -> [String] {
            WhatsNewRules.launch(
                releases: notes, record: WhatsNewRecord(seenBuild: 8), currentBuild: 9, hasGuest: true,
                offers: { $0.requirement?.isMet(by: host, vCPUs: 6) ?? true }
            ).pending.flatMap { $0.entries.map(\.id) }
        }
        #expect(announced(to: iPhone) == ["everyone", "haptics", "last"])
        #expect(announced(to: iPad) == ["everyone", "pad", "last"])
        #expect(announced(to: iPhoneWithoutHaptics) == ["everyone", "last"])
        #expect(announced(to: mac) == ["everyone", "last"])
    }
}

struct WhatsNewRequirementTests {
    @Test func aRequirementOfACameraIsMetOnlyWhereTheDeviceHasOne() {
        let requirement = WhatsNewRequirement(camera: true)
        func device(hasCamera: Bool) -> HostDevice {
            HostDevice(
                name: "d", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]),
                freeStorageBytes: 40_000_000_000, hasCamera: hasCamera)
        }
        #expect(requirement.isMet(by: device(hasCamera: true), vCPUs: 6))
        #expect(!requirement.isMet(by: device(hasCamera: false), vCPUs: 6))
        // A device has no camera unless it says so.
        #expect(!requirement.isMet(by: host(maximumMiB: 8192), vCPUs: 6))
        // Every requirement must hold: a camera is not a compass.
        let both = WhatsNewRequirement(compass: true, camera: true)
        #expect(!both.isMet(by: device(hasCamera: true), vCPUs: 6))
        #expect(both.isMet(by: host(maximumMiB: 4096, hasCompass: true, hasCamera: true), vCPUs: 6))
    }

    /// An entry about a feature is left out where the application's own rule for
    /// the feature does not offer it. The rule is asked of the application each
    /// time the notes are drawn, so a launch's question (`isMet`) does not
    /// include it.
    @Test func aFeatureRequirementIsAskedWhenTheNotesAreDrawnAndNotWhenALaunchDecides() {
        let requirement = WhatsNewRequirement(feature: .addToHomeScreen)
        for host in [iPhone, iPhoneWithoutHaptics, iPad, mac] { #expect(requirement.isMet(by: host, vCPUs: 6)) }
        #expect(requirement.isShown(by: { $0 == .addToHomeScreen }))
        #expect(!requirement.isShown(by: { _ in false }))
        // A requirement that names no feature is shown whatever the features say.
        #expect(WhatsNewRequirement(device: .pad).isShown(by: { _ in false }))
        // The device and the feature each have their say.
        let both = WhatsNewRequirement(device: .pad, feature: .addToHomeScreen)
        #expect(both.isMet(by: iPad, vCPUs: 6) && !both.isMet(by: iPhone, vCPUs: 6))
        #expect(both.isShown(by: { $0 == .addToHomeScreen }) && !both.isShown(by: { _ in false }))
    }

    /// The ribbon's launch keeps an entry about a feature, whatever the device
    /// is: only the notes drawn later leave it out.
    @Test func aLaunchKeepsAnEntryAboutAFeatureOnEveryKindOfDevice() {
        let entries = [
            WhatsNewEntry(id: "everyone", title: "E", text: "T", symbol: "star"),
            WhatsNewEntry(
                id: "feature", title: "F", text: "T", symbol: "star",
                requirement: WhatsNewRequirement(feature: .addToHomeScreen)),
        ]
        let notes = [WhatsNewRelease(build: 9, version: "1.0 (9)", entries: entries)]
        for host in [iPhone, iPad, mac] {
            let launch = WhatsNewRules.launch(
                releases: notes, record: WhatsNewRecord(seenBuild: 8), currentBuild: 9, hasGuest: true,
                offers: { $0.requirement?.isMet(by: host, vCPUs: 6) ?? true })
            #expect(launch.pending.flatMap { $0.entries.map(\.id) } == ["everyone", "feature"])
        }
    }

    @Test func aRequirementOfACompassIsMetOnlyWhereTheDeviceCanGiveOne() {
        let requirement = WhatsNewRequirement(compass: true)
        #expect(requirement.isMet(by: host(maximumMiB: 4096, hasCompass: true), vCPUs: 6))
        #expect(!requirement.isMet(by: host(maximumMiB: 4096, hasCompass: false), vCPUs: 6))
        // A device is not given a compass unless it says so.
        #expect(!requirement.isMet(by: host(maximumMiB: 8192), vCPUs: 6))
        // Both requirements must hold.
        let both = WhatsNewRequirement(memoryAboveMiB: 4096, compass: true)
        #expect(both.isMet(by: host(maximumMiB: 8192, hasCompass: true), vCPUs: 6))
        #expect(!both.isMet(by: host(maximumMiB: 4096, hasCompass: true), vCPUs: 6))
        #expect(!both.isMet(by: host(maximumMiB: 8192, hasCompass: false), vCPUs: 6))
    }
}

struct WhatsNewStoreTests {
    @Test func theRecordIsKeptAcrossLaunches() {
        withDefaults { defaults in
            #expect(WhatsNewStore(defaults: defaults).record == nil)
            let record = WhatsNewRecord(seenBuild: 5, ribbonBuild: 6, ribbonLaunches: 2)
            WhatsNewStore(defaults: defaults).keep(record)
            #expect(WhatsNewStore(defaults: defaults).record == record)
            WhatsNewStore(defaults: defaults).keep(WhatsNewRecord(seenBuild: 6))
            #expect(WhatsNewStore(defaults: defaults).record == WhatsNewRecord(seenBuild: 6))
        }
    }

    @Test func aRecordThatCannotBeReadIsNoRecord() {
        withDefaults { defaults in
            defaults.set(Data("not a record".utf8), forKey: WhatsNewStore.key)
            #expect(WhatsNewStore(defaults: defaults).record == nil)
        }
    }
}

struct WhatsNewCatalogTests {
    private func decode(_ json: String) throws -> [WhatsNewRelease] {
        try WhatsNewCatalog.decode(Data(json.utf8))
    }

    private func notes(
        build: Int = 6, date: String? = nil, entry: String = #""id": "a", "title": "A", "text": "T", "symbol": "star""#
    ) -> String {
        let day = date.map { #", "date": "\#($0)""# } ?? ""
        return #"[{"build": \#(build), "version": "1.0 (\#(build))"\#(day), "entries": [{\#(entry)}]}]"#
    }

    @Test func aReleaseDecodesWithItsEntriesPlaceAndRequirement() throws {
        let decoded = try decode(
            notes(
                date: "2026-11-12",
                entry: #"""
                "id": "file-browser", "title": "File browser", "text": "T", "symbol": "folder.fill",
                "place": "sessionSettings", "requirement": {"memoryAboveMiB": 4096}
                """#))
        let release = try #require(decoded.first)
        #expect(release.build == 6 && release.version == "1.0 (6)" && release.date == "2026-11-12")
        #expect(release.entries.first?.title == "File browser")
        #expect(release.entries.first?.place == .sessionSettings)
        #expect(release.entries.first?.requirement == WhatsNewRequirement(memoryAboveMiB: 4096))
    }

    private let entry = #""id": "a", "title": "A", "text": "T", "symbol": "star""#

    @Test func aDeviceRequirementDecodesAlone() throws {
        for (word, device) in [("pad", WhatsNewDevice.pad), ("haptics", .haptics)] {
            let decoded = try decode(notes(entry: entry + #", "requirement": {"device": "\#(word)"}"#))
            #expect(decoded.first?.entries.first?.requirement == WhatsNewRequirement(device: device))
        }
    }

    @Test func aDeviceRequirementDecodesBesideMemory() throws {
        let decoded = try decode(
            notes(entry: entry + #", "requirement": {"memoryAboveMiB": 4096, "device": "pad"}"#))
        #expect(
            decoded.first?.entries.first?.requirement == WhatsNewRequirement(memoryAboveMiB: 4096, device: .pad))
    }

    /// A requirement the code does not know refuses the notes: a feature it
    /// no longer has is one.
    @Test func aRequirementFieldTheCodeDoesNotKnowRefusesTheNotes() {
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"feature": "sendFeedback"}"#))
        }
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"device": "pad", "mail": true}"#))
        }
    }

    @Test func aFeatureRequirementDecodesAloneOrBesideAnotherAndOnlyAsAKnownFeature() throws {
        let alone = try decode(notes(entry: entry + #", "requirement": {"feature": "addToHomeScreen"}"#))
        #expect(alone.first?.entries.first?.requirement == WhatsNewRequirement(feature: .addToHomeScreen))
        let beside = try decode(
            notes(entry: entry + #", "requirement": {"device": "pad", "feature": "addToHomeScreen"}"#))
        #expect(
            beside.first?.entries.first?.requirement
                == WhatsNewRequirement(device: .pad, feature: .addToHomeScreen))
        // A word that is not a feature, and one that no longer is, refuse the notes.
        for feature in [#""Pad""#, #""120Hz""#, #""highRefresh""#, #""""#, "3", "null", #"["addToHomeScreen"]"#, "true"] {
            #expect(throws: (any Error).self, "\(feature)") {
                try decode(notes(entry: entry + #", "requirement": {"feature": \#(feature)}"#))
            }
        }
    }

    /// A key that is present holds a value, or the notes are refused: `null`
    /// beside another field would otherwise drop the requirement unseen.
    @Test func aRequirementFieldSetToNullIsRefused() {
        for requirement in [
            #"{"memoryAboveMiB": 4096, "device": null}"#, #"{"device": "pad", "memoryAboveMiB": null}"#,
            #"{"device": null}"#, #"{"memoryAboveMiB": null}"#,
        ] {
            #expect(throws: (any Error).self, "\(requirement)") {
                try decode(notes(entry: entry + #", "requirement": \#(requirement)"#))
            }
        }
    }

    /// The notes file, where the application keeps it.
    private func notesFile() -> URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("Andarium/Shared/WhatsNew.json")
    }

    /// The application's own notes, read where they are, so a mistake in them
    /// fails `swift test` and does not wait for the hosted run: a release
    /// archive runs no test, and unreadable notes show no ribbon and an empty
    /// Info.
    @Test func theBundledNotesDecode() throws {
        let releases = try WhatsNewCatalog.decode(Data(contentsOf: notesFile()))
        #expect(releases.map(\.build) == releases.map(\.build).sorted(by: >))
        #expect(releases.allSatisfy { !$0.entries.isEmpty })
        // Every device the notes name is one the code knows: that is what
        // decoding them proves.
        let named = releases.flatMap(\.entries).compactMap(\.requirement)
        #expect(named.contains { $0.device == .pad })
        // The entry about a feature that only some devices show.
        #expect(Set(named.compactMap(\.feature)) == Set(WhatsNewFeature.allCases))
    }

    @Test func anEmptyListIsNotes() throws {
        #expect(try decode("[]").isEmpty)
    }

    /// Notes and the code that reads them ship in one build, so a device the
    /// code does not know cannot come from a newer build. It is a mistake in
    /// the notes, caught by the test that reads the bundled file: the notes are
    /// refused whole, as for any field the code does not know, and the entries
    /// beside it are not announced either.
    @Test func aDeviceTheCodeDoesNotKnowRefusesTheNotes() {
        for device in [#""watch""#, #""Pad""#, #""""#, "3", "null", #"["pad"]"#] {
            #expect(throws: (any Error).self, "\(device)") {
                try decode(notes(entry: entry + #", "requirement": {"device": \#(device)}"#))
            }
        }
        let other = #""id": "b", "title": "B", "text": "T", "symbol": "star", "requirement": {"device": "watch"}"#
        #expect(throws: (any Error).self) {
            try decode(#"[{"build": 6, "version": "1.0 (6)", "entries": [{\#(entry)}, {\#(other)}]}]"#)
        }
    }

    @Test func anEntryHasTextPointsOrBoth() throws {
        let entry = #""id": "a", "title": "A", "symbol": "star""#
        let text = try #require(decode(notes(entry: entry + #", "text": "T""#)).first?.entries.first)
        #expect(text.text == "T" && text.points.isEmpty)
        let points = try #require(decode(notes(entry: entry + #", "points": ["One", "Two"]"#)).first?.entries.first)
        #expect(points.text == nil && points.points == ["One", "Two"])
        let both = try #require(
            decode(notes(entry: entry + #", "text": "T", "points": ["One"]"#)).first?.entries.first)
        #expect(both.text == "T" && both.points == ["One"])
    }

    @Test func anEntryWithNeitherTextNorPointsIsRefused() {
        #expect(throws: (any Error).self) { try decode(notes(entry: #""id": "a", "title": "A", "symbol": "star""#)) }
    }

    @Test func anEmptyStringIsRefusedInEveryField() {
        func entry(id: String = "a", title: String = "A", text: String = "T", symbol: String = "s", more: String = "")
            -> String
        {
            #""id": "\#(id)", "title": "\#(title)", "text": "\#(text)", "symbol": "\#(symbol)"\#(more)"#
        }
        #expect(throws: Never.self) { try decode(notes(entry: entry())) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry(id: ""))) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry(title: ""))) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry(text: ""))) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry(symbol: ""))) }
        // An empty text is no text, even beside points.
        #expect(throws: (any Error).self) { try decode(notes(entry: entry(text: "", more: #", "points": ["One"]"#))) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry(more: #", "points": ["One", ""]"#))) }
    }

    @Test func aListOfPointsThatStatesNothingIsRefused() {
        let entry = #""id": "a", "title": "A", "symbol": "star""#
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "points": []"#)) }
        // Not even beside text.
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "text": "T", "points": []"#)) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "points": "One""#)) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "points": [1]"#)) }
    }

    @Test func theDayIsOptional() throws {
        let release = try #require(decode(notes()).first)
        #expect(release.date == nil && release.day == nil)
    }

    @Test func theDayIsAGregorianDayWhateverTheReadersCalendar() throws {
        let release = try #require(decode(notes(date: "2026-11-12")).first)
        let parts = Calendar(identifier: .gregorian).dateComponents([.year, .month, .day], from: try #require(release.day))
        #expect(parts.year == 2026 && parts.month == 11 && parts.day == 12)
    }

    @Test func aDayThatIsNotADayIsRefused() {
        for date in ["2026-13-01", "2026-02-30", "11-12-2026", "2026-1-2", "soon"] {
            #expect(throws: (any Error).self, "\(date)") { try decode(notes(date: date)) }
        }
    }

    @Test func aFieldTheCodeDoesNotKnowIsRefusedWhereverItIs() {
        let entry = #""id": "a", "title": "A", "text": "T", "symbol": "star""#
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "action": "editSystem""#)) }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "requirement": {"diskGiB": 8}"#)) }
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"memoryAboveMiB": 4096, "diskGiB": 8}"#))
        }
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "place": "home""#)) }
        #expect(throws: (any Error).self) {
            try decode(#"[{"build": 6, "version": "1.0 (6)", "summary": "x", "entries": [{\#(entry)}]}]"#)
        }
    }

    @Test func aCompassRequirementDecodesAloneOrWithMemoryAndOnlyAsABoolean() throws {
        let entry = #""id": "a", "title": "A", "text": "T", "symbol": "star""#
        let alone = try decode(notes(entry: entry + #", "requirement": {"compass": true}"#))
        #expect(alone.first?.entries.first?.requirement == WhatsNewRequirement(compass: true))
        let both = try decode(notes(entry: entry + #", "requirement": {"memoryAboveMiB": 4096, "compass": true}"#))
        #expect(both.first?.entries.first?.requirement == WhatsNewRequirement(memoryAboveMiB: 4096, compass: true))
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"compass": "yes"}"#))
        }
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"compass": true, "gyroscope": true}"#))
        }
    }

    @Test func aCompassRequirementOfFalseIsRefusedLikeAnyOtherInvalidOne() {
        // `false` would read as "only on a device without a compass": no entry says that.
        let entry = #""id": "a", "title": "A", "text": "T", "symbol": "star""#
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"compass": false}"#))
        }
        #expect(throws: (any Error).self) {
            try decode(notes(entry: entry + #", "requirement": {"memoryAboveMiB": 4096, "compass": false}"#))
        }
    }

    @Test func aCameraRequirementDecodesLikeACompassOneAndFalseOrNullIsRefused() throws {
        let entry = #""id": "a", "title": "A", "text": "T", "symbol": "star""#
        let alone = try decode(notes(entry: entry + #", "requirement": {"camera": true}"#))
        #expect(alone.first?.entries.first?.requirement == WhatsNewRequirement(camera: true))
        let both = try decode(notes(entry: entry + #", "requirement": {"memoryAboveMiB": 4096, "camera": true}"#))
        #expect(both.first?.entries.first?.requirement == WhatsNewRequirement(memoryAboveMiB: 4096, camera: true))
        for requirement in [
            #"{"camera": false}"#, #"{"camera": "yes"}"#, #"{"camera": null}"#, #"{"compass": null}"#,
            #"{"memoryAboveMiB": 4096, "camera": null}"#, #"{"camera": true, "microphone": true}"#,
        ] {
            #expect(throws: (any Error).self, "\(requirement)") {
                try decode(notes(entry: entry + #", "requirement": \#(requirement)"#))
            }
        }
    }

    @Test func aRequirementThatStatesNothingIsRefused() {
        let entry = #""id": "a", "title": "A", "text": "T", "symbol": "star""#
        #expect(throws: (any Error).self) { try decode(notes(entry: entry + #", "requirement": {}"#)) }
    }

    @Test func aMissingFieldIsRefused() {
        #expect(throws: (any Error).self) { try decode(notes(entry: #""id": "a", "text": "T", "symbol": "star""#)) }
        #expect(throws: (any Error).self) { try decode(notes(entry: #""title": "A", "text": "T", "symbol": "star""#)) }
        #expect(throws: (any Error).self) { try decode(notes(entry: #""id": "a", "title": "A", "text": "T""#)) }
    }

    @Test func noReleaseAtOrBeforeTheLastBuildWithoutARecordHasNotes() throws {
        for build in [1, 4, 5] {
            #expect(throws: WhatsNewCatalog.Problem.releaseBeforeNotes(build: build)) { try decode(notes(build: build)) }
        }
        #expect(try decode(notes(build: 6)).count == 1)
    }

    @Test func aBuildAnEntryAndAReleaseAreWrittenOnce() {
        let entry = #"{"id": "a", "title": "A", "text": "T", "symbol": "star"}"#
        let other = #"{"id": "b", "title": "B", "text": "T", "symbol": "star"}"#
        func release(_ build: Int, _ entries: String) -> String {
            #"{"build": \#(build), "version": "v", "entries": [\#(entries)]}"#
        }
        #expect(throws: WhatsNewCatalog.Problem.duplicateBuild(6)) {
            try decode("[\(release(6, entry)), \(release(6, other))]")
        }
        #expect(throws: WhatsNewCatalog.Problem.duplicateEntry("a")) {
            try decode("[\(release(6, entry)), \(release(7, entry))]")
        }
        #expect(throws: WhatsNewCatalog.Problem.duplicateEntry("a")) {
            try decode("[\(release(6, entry + ", " + entry))]")
        }
        #expect(throws: WhatsNewCatalog.Problem.emptyRelease(build: 6)) { try decode("[\(release(6, ""))]") }
    }

    @Test func theNotesAreNotAnArray() {
        #expect(throws: (any Error).self) { try decode("{}") }
    }
}
