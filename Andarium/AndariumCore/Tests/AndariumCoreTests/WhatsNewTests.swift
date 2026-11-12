// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

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

private func host(maximumMiB: Int) -> HostDevice {
    HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [maximumMiB]),
        freeStorageBytes: 40_000_000_000)
}

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
        let later = [release(9, ["x", "y"])]
        let eight = WhatsNewRules.launch(
            releases: later, record: WhatsNewRecord(seenBuild: 5), currentBuild: 8, hasGuest: true, offers: all)
        #expect(eight.pending.isEmpty)
        #expect(eight.record == WhatsNewRecord(seenBuild: 8))

        // Build 9 then offers them, after build 8 and straight from build 5.
        let afterEight = WhatsNewRules.launch(
            releases: later, record: eight.record, currentBuild: 9, hasGuest: true, offers: all)
        #expect(afterEight.pending.map(\.build) == [9])
        #expect(afterEight.pending.first?.entries.map(\.id) == ["x", "y"])
        #expect(afterEight.record == WhatsNewRecord(seenBuild: 8, ribbonBuild: 9, ribbonLaunches: 1))
        let fromFive = WhatsNewRules.launch(
            releases: later, record: nil, currentBuild: 9, hasGuest: true, offers: all)
        #expect(fromFive.pending.map(\.build) == [9])
        #expect(fromFive.record == WhatsNewRecord(seenBuild: 5, ribbonBuild: 9, ribbonLaunches: 1))
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
