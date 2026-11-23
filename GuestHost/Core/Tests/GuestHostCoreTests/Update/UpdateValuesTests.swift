// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

/// The values of the update notice: iOS versions, release days, version
/// strings and builds, each made only from text that passes its rule.
struct UpdateValuesTests {
    // MARK: iOS versions

    @Test func anIOSVersionIsOneToThreeNumbers() {
        #expect(UpdateOSVersion("26") == UpdateOSVersion(major: 26))
        #expect(UpdateOSVersion("26.0") == UpdateOSVersion(major: 26, minor: 0))
        #expect(UpdateOSVersion("26.2.1") == UpdateOSVersion(major: 26, minor: 2, patch: 1))
        for text in ["", ".", "26.", ".26", "26..1", "26.0.0.1", "a", "26.x", "-1", "+1", " 26", "26 ", "26.0 ", "٢٦", "12345", "1e3"] {
            #expect(UpdateOSVersion(text) == nil, "\(text)")
        }
    }

    @Test func iOSVersionsCompareAsDottedNumbers() {
        #expect(UpdateOSVersion(major: 26) == UpdateOSVersion("26.0.0"))
        #expect(UpdateOSVersion("26.0")! < UpdateOSVersion("26.2.1")!)
        #expect(UpdateOSVersion("26.2")! < UpdateOSVersion("26.10")!)
        #expect(UpdateOSVersion("9.9")! < UpdateOSVersion("26.0")!)
        #expect(UpdateOSVersion("26.2.1")! < UpdateOSVersion("26.2.10")!)
        #expect(!(UpdateOSVersion("27.0")! < UpdateOSVersion("26.9.9")!))
    }

    @Test func anIOSVersionIsWrittenWithItsMinorAndOnlyANeededPatch() {
        #expect(UpdateOSVersion("26")!.description == "26.0")
        #expect(UpdateOSVersion("26.0.0")!.description == "26.0")
        #expect(UpdateOSVersion("26.2.1")!.description == "26.2.1")
    }

    // MARK: Release days

    @Test func aDayIsAFullDateOrAFullDateWithTimeAndZone() {
        #expect(UpdateDay("2026-11-20") == UpdateDay(year: 2026, month: 11, day: 20))
        #expect(UpdateDay("2026-11-20T09:30:00Z") == UpdateDay(year: 2026, month: 11, day: 20))
        #expect(UpdateDay("2026-11-20T23:30:00-08:00") == UpdateDay(year: 2026, month: 11, day: 20))
        #expect(UpdateDay("2026-11-20T00:30:00+09:00") == UpdateDay(year: 2026, month: 11, day: 20))
        #expect(UpdateDay("2028-02-29") == UpdateDay(year: 2028, month: 2, day: 29))
        #expect(UpdateDay("2026-11-20T09:30:00.123Z") == UpdateDay(year: 2026, month: 11, day: 20))
        #expect(UpdateDay("2026-11-20T09:30:00+0800") == UpdateDay(year: 2026, month: 11, day: 20))
    }

    @Test func textThatIsNoDayIsRefused() {
        for text in ["", "tomorrow", "2026-11-31", "2026-02-29", "2026-13-01", "2026-00-10", "2026-11-00", "20261120",
            "2026-11-20 09:30", "2026-11-20T09:30:00", "11/20/2026", "2026-11", "2026-11-20junk", "٢٠٢٦-١١-٢٠", "2026-11-20T", "2026-11-20T09:30Z", "2026-11-20T25:00:00Z", "2026-11-20T09:60:00Z",
            "2026-11-20T09:30:00Zjunk", "2026-11-20T09:30:00+8", "2026-11-20T09:30:00+25:00", "2026-11-20T09:30:00.Z",
            "2026-11-20t09:30:00z", "2026-11-20T09:30:00 +08:00", "2026-11-20\n", "2026/11/20", "2026.11.20", "2026-1120", "202611-20", "2026 11 20", "2026-11/20", "2026/11-20", "2026_11_20",
            String(repeating: "2", count: 200)] {
            #expect(UpdateDay(text) == nil, "\(text)")
        }
    }

    @Test func aDayIsWrittenInTheReadersLanguageWhateverTheirTimeZone() {
        let day = UpdateDay(year: 2026, month: 11, day: 20)
        #expect(day.formatted(locale: Locale(identifier: "en_US")) == "November 20, 2026")
        #expect(day.formatted(locale: Locale(identifier: "en_GB")) == "20 November 2026")
        #expect(day.formatted(locale: Locale(identifier: "ja_JP")) == "2026年11月20日")
        #expect(day.formatted(locale: Locale(identifier: "zh_CN")) == "2026年11月20日")
        #expect(day.formatted(locale: Locale(identifier: "zh_TW")) == "2026年11月20日")
        // The first and last day of a year do not slip across a zone.
        #expect(UpdateDay(year: 2027, month: 1, day: 1).formatted(locale: Locale(identifier: "en_US")) == "January 1, 2027")
        #expect(UpdateDay(year: 2026, month: 12, day: 31).formatted(locale: Locale(identifier: "en_US")) == "December 31, 2026")
    }

    // MARK: Versions and builds

    @Test func aVersionIsLettersDigitsAndDotsUpToSixteen() {
        for text in ["1", "1.1", "1.0.12", "2.0b3", "A", String(repeating: "1", count: 16)] {
            #expect(UpdateVersion.isWellFormed(version: text), "\(text)")
        }
        for text in ["", String(repeating: "1", count: 17), "1 1", "1-1", "1_1", "1.1\n", "é", "1.1<b>", "1/1", "\u{202E}1", "１.１"] {
            #expect(!UpdateVersion.isWellFormed(version: text), "\(text)")
        }
    }

    @Test func aBuildIsOneToNineDigitsAboveZero() {
        #expect(UpdateVersion.build(from: "1") == 1)
        #expect(UpdateVersion.build(from: "16") == 16)
        #expect(UpdateVersion.build(from: "016") == 16)
        #expect(UpdateVersion.build(from: "999999999") == 999_999_999)
        for text in ["", "0", "00", "-1", "+1", "1.0", "1e3", " 1", "1 ", "1000000000", "abc", "１６", "0x10"] {
            #expect(UpdateVersion.build(from: text) == nil, "\(text)")
        }
    }

    @Test func aVersionIsLabelledAsInfoWritesIt() {
        let version = UpdateVersion(
            version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 1,
            appFile: URL(string: "https://example.com/a.ipa")!)
        // A no-break space keeps the version and its build on one line.
        #expect(version.label == "1.1\u{A0}(16)")
        #expect(version.label.contains(UpdateVersion.buildSeparator))
        #expect(!version.label.contains(" "))
    }

    @Test func aVersionSurvivesTheRecordItIsKeptIn() throws {
        let version = UpdateVersion(
            version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 1_390_000_000,
            minimumOS: UpdateOSVersion("26.0"), maximumOS: UpdateOSVersion("27.9"),
            appFile: URL(string: "https://example.com/a.ipa")!, notes: URL(string: "https://example.com/n")!)
        let record = UpdateRecord(attempted: Date(timeIntervalSince1970: 1_700_000_000), found: version, hiddenBuild: 15)
        let data = try JSONEncoder().encode(record)
        #expect(try JSONDecoder().decode(UpdateRecord.self, from: data) == record)
    }

    // MARK: A kept record is held to the rules again

    private static let kept = UpdateVersion(
        version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 1_390_000_000,
        minimumOS: UpdateOSVersion("26.0"), appFile: URL(string: "https://example.com/a.ipa")!)
    private static let stamp = Date(timeIntervalSince1970: 1_700_000_000)

    /// The record as the application keeps it, with `edit` applied to its JSON
    /// text, which is what someone changing the device's defaults does.
    private func edited(_ edit: (String) -> String) throws -> UpdateRecord {
        let record = UpdateRecord(attempted: Self.stamp, found: Self.kept, hiddenBuild: 15)
        let text = String(decoding: try JSONEncoder().encode(record), as: UTF8.self)
        let changed = edit(text)
        #expect(changed != text, "the edit did not apply to \(text)")
        return try JSONDecoder().decode(UpdateRecord.self, from: Data(changed.utf8))
    }

    @Test func aRecordThatIsNotTamperedWithIsKeptWhole() throws {
        let record = try edited { $0.replacingOccurrences(of: "\"hiddenBuild\":15", with: "\"hiddenBuild\":15 ") }
        #expect(record == UpdateRecord(attempted: Self.stamp, found: Self.kept, hiddenBuild: 15))
    }

    @Test(arguments: [
        ("a sentence for a version", "\"version\":\"1.1\"", "\"version\":\"Visit evil.test now for the update!\""),
        ("a version too long", "\"version\":\"1.1\"", "\"version\":\"1.1.1.1.1.1.1.1.1\""),
        ("an empty version", "\"version\":\"1.1\"", "\"version\":\"\""),
        ("a month 13", "\"month\":11", "\"month\":13"),
        ("a day 45", "\"day\":20", "\"day\":45"),
        ("the 31st of November", "\"day\":20", "\"day\":31"),
        ("a year of Int.max", "\"year\":2026", "\"year\":9223372036854775807"),
        ("a negative year", "\"year\":2026", "\"year\":-5"),
        ("a build of 0", "\"build\":16", "\"build\":0"),
        ("a negative build", "\"build\":16", "\"build\":-16"),
        ("a build over nine digits", "\"build\":16", "\"build\":1000000000"),
        ("a size of 0", "\"sizeBytes\":1390000000", "\"sizeBytes\":0"),
        ("a size over a tebibyte", "\"sizeBytes\":1390000000", "\"sizeBytes\":1099511627777"),
        ("an iOS version out of range", "\"major\":26", "\"major\":99999"),
    ])
    func aVersionTheRulesWouldNotKeepIsDroppedAndTheRestOfTheRecordStays(
        name: String, from: String, to: String
    ) throws {
        let record = try edited { $0.replacingOccurrences(of: from, with: to) }
        #expect(record.found == nil, "\(name)")
        #expect(record.hiddenBuild == 15, "\(name)")
        #expect(record.attempted == Self.stamp, "\(name)")
    }

    @Test func aVersionThatIsTheWrongTypeIsDroppedToo() throws {
        let record = try edited { $0.replacingOccurrences(of: "\"build\":16", with: "\"build\":\"sixteen\"") }
        #expect(record.found == nil)
        #expect(record.hiddenBuild == 15)
        let noVersion = try edited { $0.replacingOccurrences(of: "\"found\":{", with: "\"unknown\":{") }
        #expect(noVersion.found == nil)
    }

    @Test func aTamperedValueNeverReachesTheScreen() throws {
        let record = try edited {
            $0.replacingOccurrences(of: "\"version\":\"1.1\"", with: "\"version\":\"Visit evil.test now for the update!\"")
        }
        #expect(record.found == nil)
    }

    @MainActor
    @Test func theStoreReadsATamperedRecordAsOneWithoutAVersion() throws {
        let suite = "UpdateValuesTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = DefaultsUpdateStore(defaults: defaults)
        store.record = UpdateRecord(attempted: Self.stamp, found: Self.kept, hiddenBuild: 15)
        #expect(store.record.found == Self.kept)
        let text = String(decoding: try #require(defaults.data(forKey: DefaultsUpdateStore.recordKey)), as: UTF8.self)
        defaults.set(
            Data(text.replacingOccurrences(of: "\"month\":11", with: "\"month\":13").utf8),
            forKey: DefaultsUpdateStore.recordKey)
        #expect(store.record.found == nil)
        #expect(store.record.hiddenBuild == 15)
        // What is not JSON at all is an empty record.
        defaults.set(Data([0xFF, 0x7B]), forKey: DefaultsUpdateStore.recordKey)
        #expect(store.record == UpdateRecord())
    }
}
