// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The words of Add to Home Screen are in the build in every language: each
/// string of its screens, its App Intent and its errors, and the phrases Siri
/// and Search understand. Each lookup reads one language's bundle, so the
/// result does not depend on the language of the device or Simulator the tests
/// run on. `tests/tooling/andarium` checks the catalogs' contents; these show
/// the build carries them.
struct AndroidAppsWordsTests {
    private static let languages = ["ja", "zh-Hans", "zh-Hant"]

    /// Every string the feature adds to the Localizable table, but the two
    /// that every language writes as English does.
    private static let keys = [
        "Add to Home Screen",
        "Shortcuts",
        "Search Android apps",
        "Installed",
        "Built into Android",
        "Reading apps from Android…",
        "No Android apps yet",
        "Couldn’t read Android’s apps",
        "Android has no app with an icon to add.",
        "No Results for “%@”",
        "No app in Android has this name.",
        "Android didn’t answer. Showing the list from %@.",
        "Add “%@” to Home Screen",
        "Show in Search",
        "Android apps appear in Search while this is on. They stay available in Shortcuts and Siri when it’s off.",
        "Android app",
        "In the Shortcuts app",
        "Open Shortcuts",
        "It opens on Andarium’s page.",
        "Touch and hold “%@”",
        "Its tile has the app’s own icon.",
        "Tap Add to Home Screen",
        "The icon and name are already filled in.",
        "Tap the check mark",
        "The icon appears on your Home Screen.",
        "A tap on the icon opens “%@” in Android, and starts Android first if it isn’t running.",
        "Opens Andarium’s page in Shortcuts.",
        "New Shortcut",
        "Home Screen",
        "Open Android App",
        "Android App",
        "App",
        "Opens an app in Android.",
        "Open ${target}",
        "This isn’t an Android app that Andarium can open.",
        "Opening “%@”",
        "It opens as soon as Android has started.",
        "Don’t open",
        "Opening “%@”…",
        "“%@” is waiting",
        "Choose what to do with the unfinished update below. The app opens once Android has started.",
        "“%@” is waiting for space",
        "Free up space on this %@, then tap Start. The app opens once Android has started.",
        "“%@” isn’t in Android",
        "It may have been removed from Android. You can remove its icon from the Home Screen.",
        "There is no virtual machine. The icon works again once the app is installed in a new virtual machine.",
        "“%@” didn’t open",
        "Android took too long. Try again, or open it from Android’s home screen.",
        "Android is in recovery mode",
        "Restart Android normally to open “%@”.",
        "Android is still being set up",
        "Finish setting up Android, then open “%@” again.",
        "Can’t open “%@”",
        "This version of Android is too old to open apps directly. Update Andarium, then open the app again.",
    ]

    private func bundle(_ language: String) throws -> Bundle {
        let path = try #require(Bundle.main.path(forResource: language, ofType: "lproj"), "no \(language) in the bundle")
        return try #require(Bundle(path: path))
    }

    @Test(arguments: AndroidAppsWordsTests.languages)
    func everyStringIsTranslated(language: String) throws {
        let bundle = try bundle(language)
        for key in Self.keys {
            let value = bundle.localizedString(forKey: key, value: "\u{0}missing", table: nil)
            #expect(value != "\u{0}missing", "\(language) has no entry for \(key)")
            #expect(value != key, "\(language) says \(key) in English")
            // Each place a name goes stays a place for one.
            #expect(
                value.components(separatedBy: "%@").count == key.components(separatedBy: "%@").count,
                "\(language): \(value) does not keep the places of \(key)")
        }
    }

    @Test(arguments: AndroidAppsWordsTests.languages)
    func theNameOfAnAppAndOfAndroidAreNotTranslated(language: String) throws {
        let bundle = try bundle(language)
        #expect(bundle.localizedString(forKey: "%@", value: nil, table: nil) == "%@")
        #expect(bundle.localizedString(forKey: "Android", value: nil, table: nil) == "Android")
    }

    @Test(arguments: AndroidAppsWordsTests.languages)
    func theSiriPhrasesAreTranslatedAndKeepTheirParts(language: String) throws {
        let bundle = try bundle(language)
        let table = try #require(
            bundle.url(forResource: "AppShortcuts", withExtension: "strings").flatMap { NSDictionary(contentsOf: $0) as? [String: String] },
            "\(language) has no AppShortcuts table")
        let phrases = table.filter { $0.key.hasPrefix("#!SET#!_Open ${target} in ${applicationName}[") }
            .sorted { $0.key < $1.key }.map(\.value)
        #expect(phrases.count == 2, "\(phrases)")
        for phrase in phrases {
            // The system finds the phrase by the app's name in it: without it the phrase is dropped.
            #expect(phrase.contains("${applicationName}"), "\(phrase)")
            #expect(!phrase.hasPrefix("Open "), "\(language) says a phrase in English: \(phrase)")
        }
        #expect(phrases.first?.contains("${target}") == true, "the first phrase names the app: \(phrases)")
    }

    /// An app's name goes between quotation marks in a sentence, and stays as
    /// Android gives it, whatever its characters.
    @Test func anAppsNameIsQuotedAndNeverReflowed() {
        #expect(AndroidAppsWords.openingTitle("Comet Hop") == "Opening \u{201C}Comet Hop\u{201D}")
        #expect(AndroidAppsWords.waitingSpaceTitle("A \u{201C}b\u{201D} %@") == "\u{201C}A \u{201C}b\u{201D} %@\u{201D} is waiting for space")
        #expect(AndroidAppsWords.addLabel("Wavelet") == "Add \u{201C}Wavelet\u{201D} to Home Screen")
        #expect(AndroidAppsWords.waitingSpaceText(device: "iPad").contains("this iPad"))
    }

    @Test func theBannerIsAnnouncedToVoiceOverAsItsTitleAndItsText() throws {
        let component = try #require(AndroidComponent(packageName: "com.example.comet", activityClass: "com.example.Main"))
        for failure in [AppOpenFailure.notInAndroid, .tookTooLong, .recovery, .setupUnfinished, .toolMissing] {
            let banner = AppOpenBanner(
                serial: 1, failure: failure, label: "Comet Hop", app: component,
                component: failure.canRetry ? component : nil, shownAt: Date())
            let words = AndroidAppsWords.banner(failure, app: "Comet Hop")
            #expect(AppOpenFailureBanner.announcement(for: banner) == "\(words.title). \(words.text)", "\(failure)")
            // Every failure names the app, in its title or its text.
            #expect((words.title + words.text).contains("Comet Hop"), "\(failure)")
        }
    }

    @Test func eachFailureHasItsOwnWordsAndOnlyTheRetriableOnesTalkOfTryingAgain() {
        let failures: [AppOpenFailure] = [.notInAndroid, .tookTooLong, .recovery, .setupUnfinished, .toolMissing]
        let titles = failures.map { AndroidAppsWords.banner($0, app: "Comet Hop").title }
        #expect(Set(titles).count == failures.count, "\(titles)")
        #expect(AndroidAppsWords.banner(.notInAndroid, app: "Comet Hop").title == "\u{201C}Comet Hop\u{201D} isn\u{2019}t in Android")
        #expect(AndroidAppsWords.banner(.recovery, app: "Comet Hop").text.contains("Comet Hop"))
        // An image without the tool is not helped by another try: the words do not offer one.
        let missing = AndroidAppsWords.banner(.toolMissing, app: "Comet Hop")
        #expect(missing.title.contains("Comet Hop"))
        #expect(!missing.text.localizedCaseInsensitiveContains("try again"))
        #expect(!AppOpenFailure.toolMissing.canRetry)
    }
}
