// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// The update notice of a sideloaded build in the Simulator preview, whose
/// version file is a made-up one read by the real rules (`UpdatePreview`): the
/// card first on Home, its close button, the detail its button opens, Skip
/// This Version, the detail without tool rows of an iPad app on a Mac, and
/// Info's Updates section with its switch. `-screen` names a screen of
/// `AppEntry`.
final class UpdateNoticeUITests: XCTestCase {
    override func setUp() {
        continueAfterFailure = false
    }

    @MainActor
    private func launch(_ screen: String) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments = previewTestLanguage + ["-screen", screen]
        app.launch()
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20), "\(screen) did not open")
        return app
    }

    private func element(_ app: XCUIApplication, _ identifier: String) -> XCUIElement {
        app.descendants(matching: .any)[identifier].firstMatch
    }

    @MainActor
    func testTheCardIsOnHomeAndItsCloseButtonPutsItAway() {
        let app = launch("update-card")
        let card = element(app, "home-update-notice")
        XCTAssertTrue(card.waitForExistence(timeout: 20), "the card is not on Home")
        XCTAssertTrue(app.staticTexts["Update available"].exists)
        XCTAssertTrue(app.staticTexts["Andarium 1.1\u{A0}(16) is out. You have 1.0\u{A0}(15)."].exists)
        app.buttons["Later"].firstMatch.tap()
        XCTAssertTrue(card.waitForNonExistence(timeout: 5), "the card stays after its close button")
    }

    @MainActor
    func testAPutAwayCardLeavesHomeAsItAlwaysIs() {
        let app = launch("update-card-closed")
        XCTAssertTrue(app.staticTexts["Virtual machine"].waitForExistence(timeout: 20))
        XCTAssertFalse(element(app, "home-update-notice").exists)
    }

    @MainActor
    func testTheButtonOpensTheDetailWithTheInstalledToolsAndSkipPutsTheVersionAway() {
        let app = launch("update-card")
        // The card is one element that carries the button's identifier too: the button is found by its words.
        let button = app.buttons["Get Update"].firstMatch
        XCTAssertTrue(button.waitForExistence(timeout: 20))
        button.tap()
        XCTAssertTrue(element(app, "update-with-altstore").waitForExistence(timeout: 10), "no AltStore row")
        XCTAssertTrue(element(app, "update-with-sidestore").exists, "no SideStore row")
        XCTAssertTrue(element(app, "update-app-file").exists)
        XCTAssertTrue(element(app, "update-release-page").exists)
        XCTAssertTrue(app.staticTexts["Released"].exists)
        let skip = element(app, "update-skip")
        if !skip.isHittable { app.swipeUp() }
        XCTAssertTrue(skip.waitForExistence(timeout: 5))
        skip.tap()
        XCTAssertTrue(element(app, "update-with-altstore").waitForNonExistence(timeout: 5), "the sheet stays")
        XCTAssertFalse(element(app, "home-update-notice").exists, "the skipped version's card stays")
    }

    @MainActor
    func testOnlyTheInstalledToolHasARow() {
        let app = launch("update-detail-altstore")
        XCTAssertTrue(element(app, "update-with-altstore").waitForExistence(timeout: 20))
        XCTAssertFalse(element(app, "update-with-sidestore").exists)
    }

    @MainActor
    func testWithNoToolTheStepsStandInAndOnAMacToo() {
        for screen in ["update-detail-steps", "update-detail-mac"] {
            let app = launch(screen)
            XCTAssertTrue(app.staticTexts["How to update"].waitForExistence(timeout: 20), screen)
            XCTAssertFalse(element(app, "update-with-altstore").exists, screen)
            XCTAssertFalse(element(app, "update-with-sidestore").exists, screen)
            // The rows under the steps are below the sheet's fold.
            let file = element(app, "update-app-file")
            if !file.exists { app.swipeUp() }
            XCTAssertTrue(file.waitForExistence(timeout: 5), screen)
            app.terminate()
        }
    }

    @MainActor
    func testInfoShowsTheUpdateAndTheSwitchTurnsTheCheckOff() {
        let app = launch("update-info")
        let row = element(app, "update-info-row")
        XCTAssertTrue(row.waitForExistence(timeout: 20), "no update row in Info")
        // The row carries the switch's identifier; the switch itself is its child.
        let toggle = app.switches["update-check-switch"].firstMatch
        XCTAssertTrue(toggle.exists, "no Check for Updates switch")
        XCTAssertEqual(toggle.value as? String, "1")
        toggle.switches.firstMatch.tap()
        XCTAssertTrue(row.waitForNonExistence(timeout: 5), "the row stays with the check off")
        XCTAssertEqual(toggle.value as? String, "0")
    }

    @MainActor
    func testInfoWithNothingNewerHasTheSwitchAlone() {
        let app = launch("update-info-current")
        XCTAssertTrue(app.switches["update-check-switch"].firstMatch.waitForExistence(timeout: 20))
        XCTAssertFalse(element(app, "update-info-row").exists)
    }

    @MainActor
    func testInfoWithTheSwitchOffHasNoUpdateRow() {
        let app = launch("update-info-off")
        let toggle = app.switches["update-check-switch"].firstMatch
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        XCTAssertEqual(toggle.value as? String, "0")
        XCTAssertFalse(element(app, "update-info-row").exists)
    }
}
