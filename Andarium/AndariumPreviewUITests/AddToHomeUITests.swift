// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// Add to Home Screen in the Simulator preview, where the tool's answers are
/// played: the row in Settings' Apps section is there only while Android runs
/// and has a list to offer, the chooser lists the apps with the label VoiceOver
/// reads, searches them and shows each state of its list, the guide carries
/// Apple's Shortcuts button, and a request shows the cards, the capsule and the
/// banners. The Simulator has no VoiceOver: a test drives the interface and
/// reads the labels and the order of the elements, not the speech.
/// `-screen` names a screen of `AppEntry` (`AddToHomePreview`).
final class AddToHomeUITests: XCTestCase {
    private let cometLabel = "Add \u{201C}Comet Hop\u{201D} to Home Screen"

    override func setUp() {
        continueAfterFailure = false
    }

    @MainActor
    private func launch(_ screen: String, _ arguments: String...) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments = previewTestLanguage + ["-screen", screen] + arguments
        app.launch()
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20), "\(screen) did not open")
        return app
    }

    private func element(_ app: XCUIApplication, _ identifier: String) -> XCUIElement {
        app.descendants(matching: .any)[identifier].firstMatch
    }

    private func text(_ app: XCUIApplication, _ label: String) -> XCUIElement {
        app.staticTexts[label].firstMatch
    }

    /// Add to Home Screen is offered from iOS 27, where Shortcuts has a tile for each app. A test of
    /// the row, the chooser or the guide runs there; before it nothing offers them
    /// (`testBefore27NothingOffersAddToHomeScreen`).
    private func requireShortcutTiles() throws {
        if #unavailable(iOS 27.0) { throw XCTSkip("Add to Home Screen is offered from iOS 27") }
    }

    // MARK: The row

    @MainActor
    func testTheRowIsInTheAppsSectionOnlyWhileAndroidRunsWithAList() throws {
        try requireShortcutTiles()
        let offered = launch("add-home-row")
        let row = element(offered, "settings-add-to-home-screen")
        XCTAssertTrue(row.waitForExistence(timeout: 20), "the row is not in Settings while Android runs")
        XCTAssertEqual(row.label.contains("Add to Home Screen"), true, row.label)
        XCTAssertTrue(row.label.contains("Shortcuts"), "the row's detail is Shortcuts: \(row.label)")
        offered.terminate()

        // The ordinary preview's Android runs too, but with no list to offer: no row.
        let without = launch("settings")
        XCTAssertTrue(without.navigationBars["Settings"].waitForExistence(timeout: 20))
        XCTAssertFalse(element(without, "settings-add-to-home-screen").waitForExistence(timeout: 3))
    }

    /// Before iOS 27 Shortcuts has no tile for an app of Andarium's, so Settings has no row and nothing
    /// opens the chooser or the guide, even where the launch asks for them.
    @MainActor
    func testBefore27NothingOffersAddToHomeScreen() throws {
        if #available(iOS 27.0, *) { throw XCTSkip("Add to Home Screen is offered from iOS 27") }
        let offered = launch("add-home-row")
        XCTAssertTrue(offered.navigationBars["Settings"].waitForExistence(timeout: 20))
        XCTAssertFalse(element(offered, "settings-add-to-home-screen").waitForExistence(timeout: 3))
        offered.terminate()

        let page = launch("add-home-page")
        XCTAssertTrue(page.navigationBars["Settings"].waitForExistence(timeout: 20))
        XCTAssertFalse(page.navigationBars["Add to Home Screen"].waitForExistence(timeout: 3))
        XCTAssertFalse(page.buttons[cometLabel].exists)
    }

    @MainActor
    func testTheRowOpensTheChooser() throws {
        try requireShortcutTiles()
        let app = launch("add-home-row")
        let row = element(app, "settings-add-to-home-screen")
        XCTAssertTrue(row.waitForExistence(timeout: 20))
        row.tap()
        XCTAssertTrue(app.navigationBars["Add to Home Screen"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.buttons[cometLabel].waitForExistence(timeout: 10))
    }

    // MARK: The chooser

    @MainActor
    func testTheChooserListsInstalledAppsThenBuiltInOnesWithTheLabelsVoiceOverReads() throws {
        try requireShortcutTiles()
        let app = launch("add-home-page")
        XCTAssertTrue(app.navigationBars["Add to Home Screen"].waitForExistence(timeout: 20))
        let comet = app.buttons[cometLabel]
        XCTAssertTrue(comet.waitForExistence(timeout: 10), "no row for Comet Hop")
        // Every row reads as the command it leads to, and in the order it is shown.
        let rows = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Add \u{201C}'"))
        // The rows on screen: iOS 26's rows are taller, so an iPad shows seven of them where iOS 27 shows eight.
        if #available(iOS 27.0, *) {
            XCTAssertGreaterThanOrEqual(rows.count, 8)
        } else {
            XCTAssertGreaterThanOrEqual(rows.count, 7)
        }
        let first = rows.element(boundBy: 0)
        XCTAssertEqual(first.label, cometLabel)
        let frames = (0..<min(rows.count, 8)).map { rows.element(boundBy: $0).frame.minY }
        XCTAssertEqual(frames, frames.sorted(), "the rows are not in reading order")
        // The sections are headings that come before their apps.
        let installed = app.staticTexts["Installed"].firstMatch
        XCTAssertTrue(installed.exists)
        XCTAssertLessThan(installed.frame.minY, comet.frame.minY)
        // The icon and the package name are not read apart from the row.
        XCTAssertFalse(app.images.matching(NSPredicate(format: "label CONTAINS 'Comet Hop'")).firstMatch.exists)
    }

    @MainActor
    func testSearchNarrowsTheListAndSaysSoWhenNothingMatches() throws {
        try requireShortcutTiles()
        let app = launch("add-home-page")
        let search = app.searchFields["Search Android apps"]
        XCTAssertTrue(search.waitForExistence(timeout: 20), "the chooser has no search field")
        search.tap()
        search.typeText("Moon")
        XCTAssertTrue(app.buttons["Add \u{201C}Moonclock\u{201D} to Home Screen"].waitForExistence(timeout: 5))
        XCTAssertFalse(app.buttons[cometLabel].exists)
        search.clearAndTypeText("Zebra")
        XCTAssertTrue(app.staticTexts["No Results for \u{201C}Zebra\u{201D}"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["No app in Android has this name."].exists)
    }

    @MainActor
    func testTheSearchSwitchIsAtTheFootAndOnByDefault() throws {
        try requireShortcutTiles()
        let app = launch("add-home-page")
        let toggle = element(app, "android-apps-search-switch")
        var swipes = 0
        while !(toggle.waitForExistence(timeout: 1) && toggle.isHittable), swipes < 8 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertTrue(toggle.exists, "the chooser has no Search switch")
        XCTAssertEqual(toggle.label, "Show in Search")
        XCTAssertEqual(toggle.value as? String, "1", "Search is on by default")
        XCTAssertTrue(app.staticTexts["Android apps appear in Search while this is on. They stay available in Shortcuts and Siri when it’s off."].exists)
    }

    @MainActor
    func testTheSearchSectionIsInViewOfAShortList() throws {
        try requireShortcutTiles()
        let app = launch("add-home-page-search")
        let toggle = element(app, "android-apps-search-switch")
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        XCTAssertTrue(toggle.isHittable, "the switch for Search is in view without scrolling")
        XCTAssertTrue(app.staticTexts["Android apps appear in Search while this is on. They stay available in Shortcuts and Siri when it’s off."].exists)
    }

    @MainActor
    func testTurnedOffTheSwitchShowsOffAndTurnsOnAgain() throws {
        try requireShortcutTiles()
        let app = launch("add-home-page-search-off")
        let toggle = element(app, "android-apps-search-switch")
        var swipes = 0
        while !(toggle.waitForExistence(timeout: 1) && toggle.isHittable), swipes < 8 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertEqual(toggle.value as? String, "0")
        (toggle.switches.firstMatch.exists ? toggle.switches.firstMatch : toggle).tap()
        let on = expectation(for: NSPredicate(format: "value == '1'"), evaluatedWith: toggle)
        wait(for: [on], timeout: 5)
    }

    @MainActor
    func testEachStateOfTheList() throws {
        try requireShortcutTiles()
        // A list that is being read from Android, with the remembered one shown meanwhile.
        var app = launch("add-home-page-refreshing")
        XCTAssertTrue(element(app, "android-apps-refreshing").waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons[cometLabel].exists, "the remembered list is shown while Android is asked")
        app.terminate()

        // No list yet and none remembered: only the note.
        app = launch("add-home-page-loading")
        XCTAssertTrue(element(app, "android-apps-refreshing").waitForExistence(timeout: 20))
        XCTAssertTrue(app.staticTexts["Reading apps from Android\u{2026}"].exists)
        app.terminate()

        // Android did not answer: the remembered list and Try again.
        app = launch("add-home-page-failed")
        XCTAssertTrue(element(app, "android-apps-remembered").waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["Try again"].exists)
        XCTAssertTrue(app.buttons[cometLabel].exists, "the remembered list is kept when Android does not answer")
        app.terminate()

        // Android did not answer and no list is remembered: the title says the apps could not be read.
        app = launch("add-home-page-failed-empty")
        XCTAssertTrue(app.staticTexts["Couldn\u{2019}t read Android\u{2019}s apps"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["Try again"].exists)
        XCTAssertFalse(app.staticTexts["No Android apps yet"].exists, "Android has apps; only the reading failed")
        app.terminate()

        // Android lists no app.
        app = launch("add-home-page-empty")
        XCTAssertTrue(app.staticTexts["No Android apps yet"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.staticTexts["Android has no app with an icon to add."].exists)
        app.terminate()

        // A search with no match.
        app = launch("add-home-page-search-none")
        XCTAssertTrue(app.staticTexts["No Results for \u{201C}Zebra\u{201D}"].waitForExistence(timeout: 20))
    }

    // MARK: The guide

    @MainActor
    func testTheGuideCarriesTheStepsAndApplesButton() throws {
        try requireShortcutTiles()
        let app = launch("add-home-guide")
        let guide = element(app, "add-to-home-screen-guide")
        XCTAssertTrue(guide.waitForExistence(timeout: 20), "the guide is not up")
        XCTAssertTrue(app.staticTexts["Comet Hop"].exists)
        XCTAssertTrue(app.staticTexts["Open Shortcuts"].exists)
        XCTAssertTrue(app.staticTexts["Touch and hold \u{201C}Comet Hop\u{201D}"].exists)
        XCTAssertTrue(app.staticTexts["Tap Add to Home Screen"].exists)
        XCTAssertTrue(app.staticTexts["Tap the check mark"].exists)
        // The four steps are in order on the page.
        let steps = ["Open Shortcuts", "Touch and hold \u{201C}Comet Hop\u{201D}", "Tap Add to Home Screen", "Tap the check mark"]
        let positions = steps.map { app.staticTexts[$0].frame.minY }
        XCTAssertEqual(positions, positions.sorted(), "the steps are not in order")
        // Apple's button, and no other way into Shortcuts.
        let link = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR (label CONTAINS[c] 'shortcuts' AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        XCTAssertTrue(link.waitForExistence(timeout: 10), "Apple's Shortcuts button is missing: \(app.debugDescription)")
    }

    @MainActor
    func testAnAppOfTheChooserLeadsToItsGuide() throws {
        try requireShortcutTiles()
        let app = launch("add-home-page")
        let comet = app.buttons[cometLabel]
        XCTAssertTrue(comet.waitForExistence(timeout: 20))
        comet.tap()
        XCTAssertTrue(element(app, "add-to-home-screen-guide").waitForExistence(timeout: 10))
        XCTAssertTrue(app.staticTexts["Touch and hold \u{201C}Comet Hop\u{201D}"].exists)
    }

    // MARK: A request

    @MainActor
    func testTheBootScreenCardOffersDontOpen() {
        let app = launch("add-home-boot")
        XCTAssertTrue(element(app, "app-open-boot-card").waitForExistence(timeout: 20))
        XCTAssertTrue(app.staticTexts["Opening \u{201C}Comet Hop\u{201D}"].exists)
        let drop = app.buttons["Don\u{2019}t open"]
        XCTAssertTrue(drop.exists)
        drop.tap()
        let gone = expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: element(app, "app-open-boot-card"))
        wait(for: [gone], timeout: 5)
    }

    @MainActor
    func testTheWaitingCardsSitAboveHomesNotice() {
        var app = launch("add-home-wait-update")
        let card = element(app, "app-open-waiting-card")
        XCTAssertTrue(card.waitForExistence(timeout: 20), "no waiting card for the update")
        XCTAssertTrue(app.staticTexts["\u{201C}Comet Hop\u{201D} is waiting"].exists)
        XCTAssertTrue(app.staticTexts["System image update didn\u{2019}t finish"].exists, "Home's notice is still there")
        XCTAssertLessThan(card.frame.minY, app.staticTexts["System image update didn\u{2019}t finish"].frame.minY)
        app.terminate()

        app = launch("add-home-wait-space")
        XCTAssertTrue(element(app, "app-open-waiting-card").waitForExistence(timeout: 20), "no waiting card for the space")
        XCTAssertTrue(app.staticTexts["\u{201C}Comet Hop\u{201D} is waiting for space"].exists)
    }

    @MainActor
    func testTheCapsuleAndTheBannersOfAFailedOpen() {
        var app = launch("add-home-pill")
        XCTAssertTrue(element(app, "app-open-pill").waitForExistence(timeout: 20), "no capsule while the app is opened")
        app.terminate()

        app = launch("add-home-banner-gone")
        let banner = element(app, "app-open-banner")
        XCTAssertTrue(banner.waitForExistence(timeout: 20))
        XCTAssertTrue(app.staticTexts["\u{201C}Comet Hop\u{201D} isn\u{2019}t in Android"].exists)
        XCTAssertFalse(app.buttons["Try again"].exists, "an app that is gone has no Try again")
        app.buttons["Dismiss"].tap()
        let closed = expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: banner)
        wait(for: [closed], timeout: 5)
        app.terminate()

        app = launch("add-home-banner-slow")
        XCTAssertTrue(app.staticTexts["\u{201C}Comet Hop\u{201D} didn\u{2019}t open"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["Try again"].exists)
        app.terminate()

        app = launch("add-home-banner-recovery")
        XCTAssertTrue(app.staticTexts["Android is in recovery mode"].waitForExistence(timeout: 20))
        app.terminate()

        app = launch("add-home-banner-setup")
        XCTAssertTrue(app.staticTexts["Android is still being set up"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.buttons["Try again"].exists)
        app.terminate()

        // An Android without the tool: the short title, the approved text, no Try again.
        app = launch("add-home-banner-tool")
        XCTAssertTrue(app.staticTexts["Can\u{2019}t open \u{201C}Comet Hop\u{201D}"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.staticTexts["This version of Android is too old to open apps directly. Update Andarium, then open the app again."].exists)
        XCTAssertFalse(app.buttons["Try again"].exists, "an Android without the tool has no Try again")
    }

    @MainActor
    func testAnIconUsedAfterAnEraseSaysTheAppIsNotInAndroid() {
        let app = launch("add-home-alert-no-android")
        let alert = app.alerts.firstMatch
        XCTAssertTrue(alert.waitForExistence(timeout: 20))
        XCTAssertTrue(alert.staticTexts["\u{201C}Comet Hop\u{201D} isn\u{2019}t in Android"].exists)
        XCTAssertTrue(alert.staticTexts.matching(NSPredicate(format: "label CONTAINS 'There is no virtual machine'")).firstMatch.exists)
    }

    // MARK: Accessibility

    /// The new screens pass Apple's audit for element descriptions and traits
    /// once what they open on is up.
    @MainActor
    func testTheScreensHaveDescriptionsAndTraits() throws {
        continueAfterFailure = true
        let screens: [(String, String)] = [
            ("add-home-row", "settings-add-to-home-screen"), ("add-home-page", "android-app-com.example.comethop/com.example.comethop.MainActivity"),
            ("add-home-page-failed", "android-apps-remembered"), ("add-home-page-empty", ""),
            ("add-home-guide", "add-to-home-screen-guide"), ("add-home-boot", "app-open-boot-card"),
            ("add-home-wait-update", "app-open-waiting-card"), ("add-home-pill", "app-open-pill"),
            ("add-home-banner-slow", "app-open-banner"),
        ]
        for (screen, ready) in screens {
            // The row, the chooser and the guide are offered from iOS 27 (`testBefore27NothingOffersAddToHomeScreen`).
            if #unavailable(iOS 27.0), ["add-home-row", "add-home-page", "add-home-guide"].contains(where: screen.hasPrefix) { continue }
            let app = XCUIApplication()
            app.launchArguments = previewTestLanguage + ["-screen", screen]
            app.launch()
            XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20), "\(screen) did not open")
            if ready.isEmpty {
                _ = app.descendants(matching: .any).firstMatch.waitForExistence(timeout: 10)
            } else {
                XCTAssertTrue(element(app, ready).waitForExistence(timeout: 20), "\(screen) has no \(ready)")
            }
            try app.performAccessibilityAudit(for: [.sufficientElementDescription, .trait]) { issue in
                let element = issue.element.map { "\($0.label) (\($0.identifier))" } ?? "no element"
                XCTFail("\(screen): \(issue.compactDescription): \(element)")
                return true
            }
            app.terminate()
        }
    }
}

private extension XCUIElement {
    /// Replaces the text of a search field.
    func clearAndTypeText(_ text: String) {
        guard let current = value as? String, !current.isEmpty else {
            typeText(text)
            return
        }
        tap()
        typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: current.count) + text)
    }
}
