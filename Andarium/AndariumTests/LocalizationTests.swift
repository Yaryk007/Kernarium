// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The application ships its String Catalogs in every language. Each lookup
/// names its language, so the result does not depend on the language of the
/// device or Simulator the tests run on, and each reads the bundle the
/// application ships. `tests/tooling/andarium` checks the catalogs' contents;
/// these show the build carries them.
struct LocalizationTests {
    private static let languages = ["ja", "zh-Hans", "zh-Hant"]

    private func text(_ key: String.LocalizationValue, table: String? = nil, in language: String) -> String {
        String(localized: LocalizedStringResource(
            key, table: table, locale: Locale(identifier: language), bundle: .main))
    }

    /// The scheme's Test action pins English, so the tests that read the words
    /// the application says do not depend on the language of the device. When
    /// this fails, so will they: the pin is gone or not taken.
    @Test func theTestsRunInEnglish() {
        #expect(Bundle.main.preferredLocalizations.first == "en", "\(Bundle.main.preferredLocalizations)")
        #expect(Locale.preferredLanguages.first?.hasPrefix("en") == true, "\(Locale.preferredLanguages)")
    }

    @Test func theApplicationShipsEveryLanguage() {
        let shipped = Set(Bundle.main.localizations)
        for language in ["en"] + Self.languages {
            #expect(shipped.contains(language), "\(language) is not in \(shipped.sorted())")
        }
    }

    @Test(arguments: LocalizationTests.languages)
    func aScreenIsTranslated(language: String) {
        #expect(text("Virtual machine", in: "en") == "Virtual machine")
        #expect(text("Virtual machine", in: language) != "Virtual machine", "\(language) says it in English")
    }

    /// Each word of the Camera setting reads in the language, not in English:
    /// the switch's reasons, its alerts and the notes under Capabilities.
    @Test(arguments: LocalizationTests.languages)
    func theCameraWordsAreTranslated(language: String) {
        let words: [String.LocalizationValue] = [
            "Camera", "Not allowed in Settings", "Restricted on this device",
            "This device has no camera to share.",
            "Camera paused while other apps are on screen", "Allow camera access",
            "iOS is not allowing Andarium to use the camera. Turn on Camera for Andarium in Settings, then come back to use it.",
            "Camera is restricted",
            "This device\u{2019}s settings, such as Screen Time or a profile, do not let apps use the camera, so Andarium cannot share it.",
            "Andarium is not allowed to use the camera. Turning this on offers to open Settings.",
            "The camera is restricted on this device. Turning this on explains why.",
        ]
        for word in words {
            let english = text(word, in: "en")
            #expect(text(word, in: language) != english, "\(language) shows \"\(english)\" in English")
        }
    }

    @Test func aCountIsPluralInEnglishAndInOneFormInTheOthers() {
        #expect(text("\(1) snapshots", in: "en") == "1 snapshot")
        #expect(text("\(3) snapshots", in: "en") == "3 snapshots")
        for language in Self.languages {
            let one = text("\(1) snapshots", in: language), three = text("\(3) snapshots", in: language)
            #expect(one.contains("1") && three.contains("3"), "\(language): \(one), \(three)")
            #expect(one.replacingOccurrences(of: "1", with: "3") == three, "\(language) has a form for one")
        }
    }

    /// The notes that every language writes as English does: a term and a unit.
    /// `Localization/english-only.json` lists them as identical (the check of
    /// `scripts/localization.py` refuses any other that is).
    private static let notesEveryLanguageKeeps: Set<String> = ["AOT", "120 Hz"]

    @Test(arguments: LocalizationTests.languages)
    func everyNoteOfTheReleasesIsTranslated(language: String) throws {
        let releases = WhatsNewCatalog.load()
        let notes = releases.flatMap(\.entries).flatMap { entry in
            [entry.title] + [entry.text].compactMap { $0 } + entry.points
        }
        try #require(!notes.isEmpty)
        for note in notes {
            let translated = text(String.LocalizationValue(note), table: "WhatsNew", in: language)
            if Self.notesEveryLanguageKeeps.contains(note) {
                #expect(translated == note, "\(language) shows \"\(translated)\" for \"\(note)\"")
            } else {
                #expect(translated != note, "\(language) shows \"\(note)\" in English")
            }
        }
        // The terms are notes of the releases, so a list that outlives them is found.
        #expect(Self.notesEveryLanguageKeeps.isSubset(of: Set(notes)))
    }

    /// Each permission text of the Info.plist reads differently in each
    /// language: the Info.plist's own value is the English text, and the
    /// language's `InfoPlist.strings` must hold another.
    @Test(arguments: LocalizationTests.languages)
    func thePermissionTextsAreTranslated(language: String) throws {
        let path = try #require(Bundle.main.path(
            forResource: "InfoPlist", ofType: "strings", inDirectory: nil, forLocalization: language))
        let strings = try #require(NSDictionary(contentsOfFile: path) as? [String: String])
        let info = try #require(Bundle.main.infoDictionary)
        let keys = info.keys.filter { $0.hasPrefix("NS") && $0.hasSuffix("UsageDescription") }.sorted()
        try #require(!keys.isEmpty)
        for key in keys {
            let english = try #require(info[key] as? String, "\(key) is no text")
            let value = try #require(strings[key], "\(language) has no \(key)")
            #expect(!value.isEmpty && value != english, "\(language) shows \(key) in English")
        }
    }

    /// Words the store screenshot review settled: Traditional Chinese says
    /// 影格速率 for the frame rate and 匯入 for importing, and Japanese
    /// writes a label and its figure with no colon between them (iOS sets the
    /// gap between Japanese and Latin text itself).
    @Test func theReviewedWordsAreSaid() {
        #expect(text("Frame rate", in: "zh-Hant") == "影格速率")
        #expect(text("Import", in: "zh-Hant") == "匯入")
        #expect(text("Folders and packages can\u{2019}t be imported.", in: "zh-Hant") == "無法匯入檔案夾和套件。")
        let items = "13項目", free = "7.5 GiB"
        #expect(text("\(items) \u{00B7} \(free) free", in: "ja") == "13項目 \u{00B7} 空き容量7.5 GiB")
        #expect(text("\(free) free on this \("iPhone")", in: "ja") == "このiPhoneの空き容量7.5 GiB")
        #expect(text("\(free) on this \("iPhone")", in: "ja") == "このiPhoneでの使用量7.5 GiB")
        #expect(text("From \(free)", in: "ja") == "元のサイズ7.5 GiB")
    }

    /// A number interpolated into a localized string takes the language's
    /// grouping (5,555), so a port is passed as text and shows its digits.
    @Test func aPortIsWrittenWithoutGrouping() {
        let message = AdbNetworkMessages.cannotStart(AdbError.io(EADDRINUSE))
        #expect(message.contains("5555"), "\(message)")
    }
}
