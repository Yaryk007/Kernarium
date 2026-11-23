// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import GuestHostCore

private final class Anchor {}

/// The notice's words in each language of the applications. Each lookup names
/// its language, so the result does not depend on the machine's, and reads the
/// resource bundle the package ships: a language whose catalog did not make it
/// into the package fails here.
struct UpdateWordsTests {
    private static let languages = ["ja", "zh-Hans", "zh-Hant"]

    private static let resources: URL = Bundle(for: Anchor.self).bundleURL
        .deletingLastPathComponent().appendingPathComponent("GuestHostCore_GuestHostCore.bundle")

    private func text(_ value: String.LocalizationValue, in language: String) -> String {
        String(localized: LocalizedStringResource(
            value, locale: Locale(identifier: language), bundle: .atURL(Self.resources)))
    }

    @Test func theResourceBundleCarriesEveryLanguage() throws {
        let bundle = try #require(Bundle(url: Self.resources))
        let found = Set(bundle.localizations)
        for language in Self.languages { #expect(found.contains(language), "\(language) is not in \(found.sorted())") }
    }

    /// The sentences as English reads them, whatever the machine's language is.
    private static func inEnglish<Result>(_ read: () throws -> Result) rethrows -> Result {
        try UpdateWords.$language.withValue(Locale(identifier: "en"), operation: read)
    }

    @Test func englishIsTheKeyAndThePlainWords() {
        Self.inEnglish {
            #expect(UpdateWords.cardTitle == "Update available")
            #expect(UpdateWords.cardButton == "Get Update")
            #expect(UpdateWords.cardClose == "Later")
            #expect(UpdateWords.cardLine(app: "Andarium", label: "1.1 (16)", installed: "1.0 (15)")
                == "Andarium 1.1 (16) is out. You have 1.0 (15).")
            #expect(UpdateWords.detailTitle(app: "Andarium", version: "1.1") == "Andarium 1.1")
            #expect(UpdateWords.versionLine("1.1 (16)") == "Version 1.1 (16)")
            #expect(UpdateWords.released == "Released")
            #expect(UpdateWords.size == "Size")
            #expect(UpdateWords.requires == "Requires")
            #expect(UpdateWords.installed == "Installed")
            #expect(UpdateWords.requiresValue(UpdateOSVersion(major: 26)) == "iOS 26.0 or later")
            #expect(UpdateWords.updateWith(.altStore) == "Update with AltStore")
            #expect(UpdateWords.updateWith(.sideStore) == "Update with SideStore")
            #expect(UpdateWords.downloadAppFile == "Download App File")
            #expect(UpdateWords.openReleasePage == "Open Release Page")
            #expect(UpdateWords.sameAppleID
                == "Install it with the same Apple ID and the same app as before, so it replaces this version and keeps your virtual machine.")
            #expect(UpdateWords.stepsTitle == "How to update")
            #expect(UpdateWords.steps == [
                "Download the app file.", "Install it with the same app or computer and the same Apple ID as before.",
            ])
            #expect(UpdateWords.skip == "Skip This Version")
            #expect(UpdateWords.done == "Done")
            #expect(UpdateWords.infoHeader == "Updates")
            #expect(UpdateWords.infoRow == "Update Available")
            #expect(UpdateWords.checkSwitch == "Check for Updates")
            #expect(UpdateWords.checkFootnote
                == "Checks once a day for a newer version. Sends no account, identifier or usage data.")
        }
    }

    @Test func everyAppSentenceSaysVirtualMachineNeverGuest() {
        Self.inEnglish {
            #expect(UpdateWords.sameAppleID.contains("virtual machine"))
            for sentence in [UpdateWords.sameAppleID, UpdateWords.checkFootnote] + UpdateWords.steps {
                #expect(!sentence.lowercased().contains("guest"))
            }
        }
    }

    /// With no language named, the words are the ones the bundle negotiates for
    /// the process: one of the four the application ships.
    @Test func theMachinesOwnLanguageIsOneOfTheFour() {
        let shipped = ["en", "ja", "zh-Hans", "zh-Hant"].map { language in
            UpdateWords.$language.withValue(Locale(identifier: language)) { UpdateWords.cardTitle }
        }
        #expect(Set(shipped).count == 4)
        #expect(shipped.contains(UpdateWords.cardTitle), "\(UpdateWords.cardTitle)")
    }

    @Test(arguments: UpdateWordsTests.languages)
    func aNamedLanguageIsTheOneTheWordsAreReadIn(language: String) {
        let named = UpdateWords.$language.withValue(Locale(identifier: language)) {
            (UpdateWords.cardTitle, UpdateWords.sameAppleID, UpdateWords.steps, UpdateWords.checkFootnote)
        }
        #expect(named.0 == text("Update available", in: language))
        #expect(named.1 == text("Install it with the same Apple ID and the same app as before, so it replaces this version and keeps your virtual machine.", in: language))
        #expect(named.2.count == 2)
        #expect(named.3 == text("Checks once a day for a newer version. Sends no account, identifier or usage data.", in: language))
    }

    @Test func theSizeIsWrittenAsEverySizeIs() {
        let version = UpdateVersion(
            version: "1.1", build: 16, released: UpdateDay(year: 2026, month: 11, day: 20), sizeBytes: 1_390_000_000,
            appFile: URL(string: "https://example.com/a.ipa")!)
        #expect(UpdateWords.sizeText(version) == "1.29 GiB")
    }

    @Test(arguments: UpdateWordsTests.languages)
    func theCardIsTranslated(language: String) {
        for word in [
            "Update available", "Get Update", "Later", "Skip This Version", "Done", "Updates", "Update Available",
            "Check for Updates", "Install it with the same Apple ID and the same app as before, so it replaces this version and keeps your virtual machine.",
            "Install it with the same app or computer and the same Apple ID as before.",
            "Checks once a day for a newer version. Sends no account, identifier or usage data.",
        ] as [String.LocalizationValue] {
            let english = text(word, in: "en")
            #expect(text(word, in: language) != english, "\(language) says \"\(english)\" in English")
        }
    }

    @Test func theCardLineKeepsItsThreeValuesInEveryLanguage() {
        let line = { (language: String) in
            self.text("\("Andarium") \("1.1 (16)") is out. You have \("1.0 (15)").", in: language)
        }
        #expect(line("en") == "Andarium 1.1 (16) is out. You have 1.0 (15).")
        #expect(line("ja") == "Andarium 1.1 (16)が公開されました。現在のバージョンは1.0 (15)です。")
        #expect(line("zh-Hans") == "Andarium 1.1 (16)已推出。你当前的版本为1.0 (15)。")
        #expect(line("zh-Hant") == "Andarium 1.1 (16)已推出。你目前的版本為1.0 (15)。")
    }

    @Test(arguments: UpdateWordsTests.languages)
    func theOtherSentencesKeepTheirValues(language: String) {
        let version = text("Version \("1.1 (16)")", in: language)
        #expect(version.contains("1.1 (16)"))
        let ios = text("iOS \("26.0") or later", in: language)
        #expect(ios.contains("iOS 26.0"))
        let tool = text("Update with \("AltStore")", in: language)
        #expect(tool.contains("AltStore"))
        let footnote = text("Checks once a day for a newer version. Sends no account, identifier or usage data.", in: language)
        #expect(!footnote.contains("%@"))
        // Chinese and Japanese write no space between their text and a Latin name or a number.
        for sentence in [version, ios, tool, footnote] {
            #expect(!sentence.contains(" Andarium"), "\(sentence)")
        }
    }
}
