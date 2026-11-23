// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private final class Anchor {}

/// The package's words in each language of Andarium. The lookup names the
/// language itself, so the result does not depend on the language of the
/// machine the tests run on, and it reads the resource bundle the package
/// ships: a language whose catalog did not make it into the package fails
/// here.
private enum Say {
    static let languages = ["en", "ja", "zh-Hans", "zh-Hant"]

    /// The package's resource bundle: SwiftPM and Xcode both put it beside the
    /// test bundle.
    static let resources: URL = Bundle(for: Anchor.self).bundleURL
        .deletingLastPathComponent().appendingPathComponent("AndariumCore_AndariumCore.bundle")

    static func text(_ value: String.LocalizationValue, in language: String) -> String {
        String(localized: LocalizedStringResource(
            value, locale: Locale(identifier: language), bundle: .atURL(resources)))
    }

    /// A string that has a name for its key and the English as its default.
    static func named(_ key: StaticString, _ value: String.LocalizationValue, in language: String) -> String {
        String(localized: LocalizedStringResource(
            key, defaultValue: value, locale: Locale(identifier: language), bundle: .atURL(resources)))
    }
}

@Suite struct LocalizationTests {
    @Test func theResourceBundleCarriesEveryLanguage() throws {
        let bundle = try #require(Bundle(url: Say.resources))
        let found = Set(bundle.localizations)
        for language in Say.languages where language != "en" {
            #expect(found.contains(language), "\(language) is not in \(found.sorted())")
        }
    }

    @Test(arguments: ["ja", "zh-Hans", "zh-Hant"])
    func aWordOfTheModelIsTranslated(language: String) {
        let english = Say.text("This virtual machine cannot start", in: "en")
        #expect(english == "This virtual machine cannot start")
        let translated = Say.text("This virtual machine cannot start", in: language)
        #expect(translated != english, "\(language) says it in English")
        #expect(!translated.isEmpty)
    }

    @Test func englishCountsVcpusInOneForm() {
        #expect(Say.text("\(1) vCPUs", in: "en") == "1 vCPU")
        #expect(Say.text("\(6) vCPUs", in: "en") == "6 vCPUs")
    }

    @Test func japaneseAndChineseCountVcpusWithoutPluralForms() {
        #expect(Say.text("\(1) vCPUs", in: "ja") == "1 vCPU")
        #expect(Say.text("\(6) vCPUs", in: "ja") == "6 vCPU")
        #expect(Say.text("\(1) vCPUs", in: "zh-Hans") == "1个vCPU")
        #expect(Say.text("\(6) vCPUs", in: "zh-Hans") == "6个vCPU")
        #expect(Say.text("\(6) vCPUs", in: "zh-Hant") == "6個vCPU")
    }

    @Test func sentencesAreSeparatedAsTheLanguageWritesThem() {
        let first = "First.", second = "Second."
        #expect(Say.named("sentence.join", "\(first) \(second)", in: "en") == "First. Second.")
        for language in ["ja", "zh-Hans", "zh-Hant"] {
            #expect(Say.named("sentence.join", "\(first) \(second)", in: language) == "First.Second.", "\(language)")
        }
    }

    @Test func spokenListsAreSeparatedAsTheLanguageWritesThem() {
        let first = "One", second = "Two"
        #expect(Say.named("list.join", "\(first), \(second)", in: "en") == "One, Two")
        #expect(Say.named("list.join", "\(first), \(second)", in: "ja") == "One、Two")
        #expect(Say.named("list.join", "\(first), \(second)", in: "zh-Hans") == "One、Two")
        #expect(Say.named("list.join", "\(first), \(second)", in: "zh-Hant") == "One、Two")
    }

    @Test func joiningNothingOrOneSentenceAddsNothing() {
        #expect(Sentences.join([]) == "")
        #expect(Sentences.join("Only.") == "Only.")
        #expect(ListWords.join([]) == "")
        #expect(ListWords.join(["Only"]) == "Only")
    }
}
