// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

/// Old app compatibility in the real application: Settings > Apps > the page,
/// its switches and the alerts that ask first. The launch asks for the input
/// probe with a scripted list of two made-up apps in place of the image's
/// command (`ANDARIUM_OLD_APPS_PROBE`, see the Andarium README): Pixel Knights
/// is off, Rally 2011 is on and open.
@MainActor
final class OldAppsUITests: XCTestCase {
    private let pixel = "old-app-com.example.pixelknights"
    private let rally = "old-app-com.example.rally"

    /// Settings of a probe session with the scripted list `script` ("apps", "refuse" or "none").
    private func settings(_ script: String) -> XCUIApplication {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_OLD_APPS_PROBE"] = script
        app.launch()
        let keys = app.buttons["Side keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 90), "the session did not open")
        keys.tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 30), "Settings did not open")
        return app
    }

    private func openThePage(_ app: XCUIApplication) {
        let row = app.buttons["settings-old-apps"]
        XCTAssertTrue(row.waitForExistence(timeout: 30), "Settings has no Old app compatibility row")
        row.tap()
        XCTAssertTrue(app.navigationBars["Old app compatibility"].waitForExistence(timeout: 20), "the page did not open")
    }

    /// A switch moves from its trailing end; a tap in the middle of its row would only hit the name.
    private func flip(_ identifier: String, in app: XCUIApplication) {
        let toggle = app.switches[identifier]
        XCTAssertTrue(toggle.waitForExistence(timeout: 20), "\(identifier) is not on the page")
        toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.92, dy: 0.5)).tap()
    }

    private func value(_ identifier: String, in app: XCUIApplication) -> String {
        (app.switches[identifier].value as? String) ?? "none"
    }

    private func waitForValue(_ expected: String, of identifier: String, in app: XCUIApplication) -> Bool {
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline {
            if value(identifier, in: app) == expected { return true }
            usleep(200_000)
        }
        return value(identifier, in: app) == expected
    }

    private func quoted(_ name: String) -> String { "\u{201C}\(name)\u{201D}" }

    func testTheRowOpensThePageAndTurningOnAsksFirst() {
        let app = settings("apps")
        let row = app.buttons["settings-old-apps"]
        XCTAssertTrue(row.waitForExistence(timeout: 30), "Settings has no Old app compatibility row")
        // Rally 2011 is on, so the row says it.
        XCTAssertTrue(row.label.contains("Old app compatibility"), "the row says \(row.label)")
        XCTAssertTrue(row.label.contains("On"), "the row says \(row.label)")
        openThePage(app)
        XCTAssertTrue(app.staticTexts["Pixel Knights"].exists)
        XCTAssertTrue(app.staticTexts["com.example.pixelknights"].exists)
        XCTAssertEqual(value(pixel, in: app), "0")
        XCTAssertEqual(value(rally, in: app), "1")

        // Turning on asks; Cancel leaves the switch where it was.
        flip(pixel, in: app)
        let ask = app.alerts["Turn on for \(quoted("Pixel Knights"))?"]
        XCTAssertTrue(ask.waitForExistence(timeout: 20), "turning on asked nothing")
        XCTAssertTrue(ask.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "You can turn it off again here.")).firstMatch.exists)
        XCTAssertFalse(ask.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "will close now")).firstMatch.exists)
        ask.buttons["Cancel"].tap()
        XCTAssertTrue(ask.waitForNonExistence(timeout: 10))
        XCTAssertEqual(value(pixel, in: app), "0")

        // Turn on writes it.
        flip(pixel, in: app)
        XCTAssertTrue(ask.waitForExistence(timeout: 20), "turning on asked nothing the second time")
        ask.buttons["Turn on"].tap()
        XCTAssertTrue(waitForValue("1", of: pixel, in: app), "the switch did not turn on")
        XCTAssertEqual(value(rally, in: app), "1")
    }

    func testTurningOffAnOpenAppAsksAndTheRowStaysWhileOneIsOn() {
        let app = settings("apps")
        openThePage(app)
        flip(rally, in: app)
        let ask = app.alerts["Turn off for \(quoted("Rally 2011"))?"]
        XCTAssertTrue(ask.waitForExistence(timeout: 20), "turning off an open app asked nothing")
        XCTAssertTrue(
            ask.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "\(quoted("Rally 2011")) is open and will close now.")).firstMatch.exists,
            "the alert does not say the app closes")
        ask.buttons["Turn off"].tap()
        XCTAssertTrue(waitForValue("0", of: rally, in: app), "the switch did not turn off")
        // None is on now: back in Settings the row stays, and no longer says On.
        app.navigationBars["Old app compatibility"].buttons.firstMatch.tap()
        let row = app.buttons["settings-old-apps"]
        XCTAssertTrue(row.waitForExistence(timeout: 20), "the row went")
        XCTAssertFalse(row.label.contains("On"), "the row says \(row.label)")
    }

    func testAChangeTheVirtualMachineRefusesIsSaid() {
        let app = settings("refuse")
        openThePage(app)
        flip(pixel, in: app)
        let ask = app.alerts["Turn on for \(quoted("Pixel Knights"))?"]
        XCTAssertTrue(ask.waitForExistence(timeout: 20))
        ask.buttons["Turn on"].tap()
        let said = app.alerts["Old app compatibility wasn\u{2019}t changed"]
        XCTAssertTrue(said.waitForExistence(timeout: 20), "a refused change said nothing")
        XCTAssertTrue(
            said.staticTexts["The virtual machine didn\u{2019}t accept the change. Try again."].exists, "the alert says something else")
        said.buttons["OK"].tap()
        XCTAssertTrue(said.waitForNonExistence(timeout: 10))
        XCTAssertEqual(value(pixel, in: app), "0")
    }

    func testWithoutTheCommandSettingsHasNoRow() {
        let app = settings("none")
        // The list has been asked once the session is up; the rest of the Apps section is there.
        XCTAssertTrue(app.buttons.matching(NSPredicate(format: "label CONTAINS %@", "Install APK")).firstMatch.waitForExistence(timeout: 30))
        sleep(3)
        XCTAssertFalse(app.buttons["settings-old-apps"].exists, "Settings offers Old app compatibility with no command")
    }
}
