// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

/// Add to Home Screen in the real application: Settings > Apps > the row, the
/// chooser and the guide, and Apple's own Shortcuts button, which opens
/// Apple's Shortcuts app on Andarium's page. The launch asks for the input
/// probe with the played list of made-up Android apps in place of the image's
/// tool (`ANDARIUM_ANDROID_APPS_PROBE`, see the Andarium README), which also
/// puts the apps in the App Shortcuts, so Shortcuts shows their tiles. The
/// second test needs the Shortcuts app, which a Simulator has.
@MainActor
final class AddToHomeUITests: XCTestCase {
    private let comet = "Add \u{201C}Comet Hop\u{201D} to Home Screen"

    /// The built application, in the language of the run, with its Settings open. The elements are
    /// found by identifier, or by the word Settings in each of Andarium's languages, so that a run
    /// in another language (`xcodebuild -testLanguage`) gets as far.
    private func launch() -> XCUIApplication {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_ANDROID_APPS_PROBE"] = "1"
        app.launch()
        let keys = app.buttons["side-keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 90), "the session did not open")
        keys.tap()
        let settings = app.buttons.matching(NSPredicate(format: "label IN %@", ["Settings", "\u{8BBE}\u{7F6E}", "\u{8A2D}\u{5B9A}"])).firstMatch
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars.firstMatch.waitForExistence(timeout: 30), "Settings did not open")
        return app
    }

    private func requireShortcutTiles() throws {
        if #unavailable(iOS 27.0) { throw XCTSkip("Add to Home Screen is offered from iOS 27") }
    }

    private func attach(_ name: String, of element: XCUIElement) {
        let attachment = XCTAttachment(screenshot: element.screenshot())
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    private func openTheGuide(_ app: XCUIApplication) {
        let row = app.buttons["settings-add-to-home-screen"]
        XCTAssertTrue(row.waitForExistence(timeout: 30), "Settings has no Add to Home Screen row")
        attach("settings-row", of: app)
        row.tap()
        let app1 = app.buttons["android-app-com.example.comethop/com.example.comethop.MainActivity"]
        XCTAssertTrue(app1.waitForExistence(timeout: 30), "the list the tool gave is not on the page")
        attach("chooser", of: app)
        app1.tap()
        XCTAssertTrue(app.descendants(matching: .any)["add-to-home-screen-guide"].waitForExistence(timeout: 20), "the guide did not open")
    }

    /// Add to Home Screen is offered from iOS 27, where Shortcuts has a tile for each app of
    /// Andarium's. Before it Settings has no row, and the test says so (the others are skipped).
    func testBefore27SettingsHasNoAddToHomeScreenRow() throws {
        if #available(iOS 27.0, *) { throw XCTSkip("Add to Home Screen is offered from iOS 27") }
        let app = launch()
        // The Apps section is on screen (the row would follow Install APK).
        XCTAssertTrue(app.staticTexts["Install APK\u{2026}"].firstMatch.waitForExistence(timeout: 30), "the Apps section is not on screen")
        XCTAssertFalse(app.buttons["settings-add-to-home-screen"].waitForExistence(timeout: 5), "Settings has an Add to Home Screen row")
    }

    /// Shortcuts' own list of actions, searched for Andarium's. The entity, its query, the action and
    /// its automatic shortcut are declared @available(iOS 27.0, *), so Shortcuts lists "Open Android
    /// App" and the Find the system makes for the entity from iOS 27, and no Android action before
    /// it. The test runs Andarium once, which is when the system learns its actions, then
    /// launches Shortcuts, which runs in the test's language, English. The new shortcut's editor is
    /// opened by its URL; its search of actions is the search field that says "Action", or, from
    /// iOS 27, the only one, behind the Editor button of the panel that describes a shortcut. The
    /// words searched are "Safari", which proves the search works, and "Android". A result is a
    /// button or a cell. The extracted metadata says the same of the build
    /// (`andarium-open-intent-availability`).
    func testShortcutsOffersOpenAndroidAppFromIOS27Only() throws {
        var offered = false
        if #available(iOS 27.0, *) { offered = true }
        continueAfterFailure = false
        // The system learns what an app's actions are when the app first runs, and Shortcuts lists
        // none of them before.
        let own = XCUIApplication()
        own.launch()
        XCTAssertTrue(own.wait(for: .runningForeground, timeout: 60), "Andarium did not start")
        sleep(10)
        own.terminate()
        let shortcuts = XCUIApplication(bundleIdentifier: "com.apple.shortcuts")
        shortcuts.launch()
        func attachScreen(_ name: String) {
            let text = XCTAttachment(string: shortcuts.debugDescription)
            text.name = name
            text.lifetime = .keepAlways
            add(text)
            let picture = XCTAttachment(screenshot: shortcuts.screenshot())
            picture.name = name + "-picture"
            picture.lifetime = .keepAlways
            add(picture)
        }
        // The search of actions: the one that says Action, or, once the editor is open, the only one
        // (iOS 27 says Search; the list of shortcuts has a search of its own beside the panel that
        // describes a shortcut, and none beside the editor).
        var editorOpen = false
        func actionsSearch() -> XCUIElement {
            let named = shortcuts.searchFields.matching(NSPredicate(format: "placeholderValue CONTAINS[c] 'Action'")).firstMatch
            return named.exists || !editorOpen ? named : shortcuts.searchFields.firstMatch
        }
        func waitForSearch(_ seconds: TimeInterval) -> Bool {
            let found = NSPredicate { _, _ in actionsSearch().exists && actionsSearch().isHittable }
            return XCTWaiter().wait(for: [expectation(for: found, evaluatedWith: nil)], timeout: seconds) == .completed
        }
        XCUIDevice.shared.system.open(try XCTUnwrap(URL(string: "shortcuts://create-shortcut")))
        attachScreen("opened")
        var found = waitForSearch(15)
        if !found {
            // Shortcuts' own introduction of what is new, then the panel that describes the shortcut,
            // whose Editor button opens the editor with the actions beside it.
            let proceed = shortcuts.buttons.matching(NSPredicate(format: "label ==[c] 'Continue'")).firstMatch
            if proceed.waitForExistence(timeout: 3) { proceed.tap() }
            let editor = shortcuts.buttons["Editor"]
            if editor.waitForExistence(timeout: 20) { editor.tap() }
            editorOpen = true
            attachScreen("editor-toggled")
            found = waitForSearch(40)
        }
        attachScreen("editor")
        XCTAssertTrue(found, "the new shortcut's editor has no search of actions: \(shortcuts.debugDescription)")
        actionsSearch().tap()
        func results(naming word: String) -> XCUIElementQuery {
            let rows = shortcuts.cells.matching(NSPredicate(format: "label CONTAINS[c] %@", word))
            let buttons = shortcuts.buttons.matching(NSPredicate(format: "label CONTAINS[c] %@", word))
            return rows.count > 0 ? rows : buttons
        }
        func forget(_ typed: String) {
            actionsSearch().typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: typed.count))
        }
        actionsSearch().typeText("Safari")
        let control = NSPredicate { _, _ in results(naming: "Safari").count > 0 }
        let proved = XCTWaiter().wait(for: [expectation(for: control, evaluatedWith: nil)], timeout: 40)
        attachScreen("search-control")
        XCTAssertEqual(proved, .completed, "the search of actions found no Safari: \(shortcuts.debugDescription)")
        forget("Safari")
        actionsSearch().typeText("Android")
        // From iOS 27 Shortcuts lists Open and the Find it makes for the app entity.
        func opens() -> Bool {
            let found = results(naming: "Android")
            return found.count >= 2 || found.matching(NSPredicate(format: "label CONTAINS[c] 'Open'")).count > 0
        }
        if offered {
            let present = NSPredicate { _, _ in opens() }
            let outcome = XCTWaiter().wait(for: [expectation(for: present, evaluatedWith: nil)], timeout: 40)
            XCTAssertEqual(outcome, .completed, "Shortcuts does not offer Open Android App: \(shortcuts.debugDescription)")
        } else {
            // The search of "Safari" listed its results, so the list of actions is searched and loaded.
            sleep(15)
            XCTAssertEqual(results(naming: "Android").count, 0, "Shortcuts lists an Android action: \(shortcuts.debugDescription)")
        }
        attachScreen("search-android")
    }

    func testTheRowTheChooserAndTheGuideOfTheBuiltApplication() throws {
        try requireShortcutTiles()
        let app = launch()
        openTheGuide(app)
        XCTAssertTrue(app.staticTexts["Touch and hold \u{201C}Comet Hop\u{201D}"].exists)
        attach("guide", of: app)
        // Apple's button, drawn by the system.
        let link = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR (label CONTAINS[c] 'shortcuts' AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        XCTAssertTrue(link.waitForExistence(timeout: 10), "Apple's Shortcuts button is missing")
    }

    /// What Apple's Shortcuts button reads in the language the run is in
    /// (`xcodebuild -testLanguage ja -testRegion JP`): its label, its value and
    /// its children, written where the run's owner reads them
    /// (`ANDARIUM_LINK_REPORT`, a file), and the picture of the foot of the guide.
    func testApplesShortcutsButtonReadsInTheLanguageOfTheRun() throws {
        try requireShortcutTiles()
        let app = launch()
        openTheGuide(app)
        // Apple's button, by its identifier or by the word Shortcuts in each of the languages.
        let link = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR ((label CONTAINS[c] 'shortcuts' OR label CONTAINS '\u{30B7}\u{30E7}\u{30FC}\u{30C8}\u{30AB}\u{30C3}\u{30C8}' OR label CONTAINS '\u{5FEB}\u{6377}\u{6307}\u{4EE4}' OR label CONTAINS '\u{6377}\u{5F91}') AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        let found = link.waitForExistence(timeout: 10)
        attach("guide-foot", of: app)
        if !found {
            let buttons = app.buttons.allElementsBoundByIndex.map { "\($0.identifier)|\($0.label)" }
            try? "language=\(Locale.preferredLanguages.first ?? "?")\nNOT FOUND buttons=\(buttons)\n".write(
                toFile: ProcessInfo.processInfo.environment["ANDARIUM_LINK_REPORT"] ?? "/dev/null", atomically: true, encoding: .utf8)
        }
        XCTAssertTrue(found, "Apple's Shortcuts button is missing")
        let language = Locale.preferredLanguages.first ?? "?"
        let atOnce = link.label
        // The system fills the button's name in itself: read it again after a wait.
        sleep(12)
        attach("guide-foot-later", of: app)
        let afterWait = link.label
        // Once Shortcuts has been opened from the button and the application is back in front.
        link.tap()
        let shortcuts = XCUIApplication(bundleIdentifier: "com.apple.shortcuts")
        _ = shortcuts.wait(for: .runningForeground, timeout: 60)
        sleep(6)
        app.activate()
        sleep(4)
        let after = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR ((label CONTAINS[c] 'shortcuts' OR label CONTAINS '\u{30B7}\u{30E7}\u{30FC}\u{30C8}\u{30AB}\u{30C3}\u{30C8}' OR label CONTAINS '\u{5FEB}\u{6377}\u{6307}\u{4EE4}' OR label CONTAINS '\u{6377}\u{5F91}') AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        attach("guide-foot-after-shortcuts", of: app)
        let lines = [
            "language=\(language)",
            "label-at-once=\(atOnce)",
            "label-after-12s=\(afterWait)",
            "label-after-shortcuts=\(after.label)",
            "label=\(link.label)",
            "value=\(String(describing: link.value))",
            "identifier=\(link.identifier)",
            "type=\(link.elementType.rawValue)",
            "children=\(link.descendants(matching: .any).allElementsBoundByIndex.map { "\($0.elementType.rawValue):\($0.label)" })",
        ]
        XCTContext.runActivity(named: lines.joined(separator: " | ")) { _ in }
        let report = XCTAttachment(string: lines.joined(separator: "\n"))
        report.name = "link-report"
        report.lifetime = .keepAlways
        add(report)
        if let path = ProcessInfo.processInfo.environment["ANDARIUM_LINK_REPORT"] {
            let text = lines.joined(separator: "\n") + "\n\n"
            if let handle = FileHandle(forWritingAtPath: path) {
                handle.seekToEndOfFile()
                handle.write(Data(text.utf8))
                try? handle.close()
            } else {
                try? text.write(toFile: path, atomically: true, encoding: .utf8)
            }
        }
    }

    func testApplesShortcutsButtonOpensShortcutsOnAndariumsPage() throws {
        try requireShortcutTiles()
        let app = launch()
        openTheGuide(app)
        let link = app.descendants(matching: .any).matching(NSPredicate(
            format: "identifier == 'shortcuts-link' OR (label CONTAINS[c] 'shortcuts' AND (elementType == %d OR elementType == %d))",
            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue)).firstMatch
        XCTAssertTrue(link.waitForExistence(timeout: 10), "Apple's Shortcuts button is missing")
        link.tap()
        let shortcuts = XCUIApplication(bundleIdentifier: "com.apple.shortcuts")
        XCTAssertTrue(shortcuts.wait(for: .runningForeground, timeout: 60), "Shortcuts did not open")
        // The first launch may show what is new.
        sleep(4)
        attach("shortcuts-1", of: shortcuts)
        let next = shortcuts.buttons["Continue"]
        if next.waitForExistence(timeout: 3) {
            next.tap()
            sleep(2)
        }
        attach("shortcuts-2", of: shortcuts)
        // Andarium's page: its name, then a tile for each of its apps. The
        // tiles are asked of the application and can come a moment later.
        XCTAssertTrue(
            shortcuts.staticTexts["Andarium"].firstMatch.waitForExistence(timeout: 30),
            "Shortcuts does not show Andarium's page: \(shortcuts.debugDescription)")
        attach("shortcuts-page", of: shortcuts)
        let tile = shortcuts.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS[c] 'Comet Hop'")).firstMatch
        let found = tile.waitForExistence(timeout: 60)
        attach("shortcuts-3", of: shortcuts)
        XCTAssertTrue(found, "Andarium's page has no tile for Comet Hop: \(shortcuts.debugDescription)")
    }
}
