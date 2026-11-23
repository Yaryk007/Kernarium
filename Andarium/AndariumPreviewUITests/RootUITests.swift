// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// The root switch in the Simulator preview: the Root section is on Home only
/// where the release carries root, the switch asks before it turns root on and
/// turns it off at once, and the line under it says a pause. `-screen` names
/// a screen of `AppEntry` (`RootPreview`).
final class RootUITests: XCTestCase {
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

    private func rootSwitch(_ app: XCUIApplication) -> XCUIElement {
        app.switches["home-root-switch"].firstMatch
    }

    /// The row carries the switch's identifier; the control is the switch itself or its child.
    private func flip(_ toggle: XCUIElement) {
        let control = toggle.switches.firstMatch
        (control.exists ? control : toggle).tap()
    }

    @MainActor
    func testAReleaseWithoutRootHasNoRootSection() {
        let app = launch("home")
        XCTAssertTrue(app.staticTexts["Virtual machine"].waitForExistence(timeout: 20))
        XCTAssertFalse(rootSwitch(app).exists, "the Root row is on Home for a release that has no root")
        XCTAssertFalse(app.staticTexts["Root access"].exists)
    }

    @MainActor
    func testTheSectionSitsBetweenCapabilitiesAndBackupAndStartsOff() {
        let app = launch("home-root")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20), "no Root row")
        XCTAssertEqual(toggle.value as? String, "0")
        XCTAssertTrue(app.staticTexts["Apps you allow in Magisk can run as root. Takes effect at the next start."].exists)
        XCTAssertFalse(app.staticTexts["Paused after a failed start"].exists)
        // Capabilities is above it and Backup below it, in the order VoiceOver reads.
        let capabilities = app.staticTexts["Capabilities"].firstMatch
        let root = app.staticTexts["Root"].firstMatch
        let backup = app.staticTexts["Backup"].firstMatch
        XCTAssertTrue(capabilities.exists && root.exists && backup.exists)
        XCTAssertLessThan(capabilities.frame.minY, root.frame.minY)
        XCTAssertLessThan(root.frame.minY, backup.frame.minY)
    }

    @MainActor
    func testTurningRootOnAsksFirstAndCancelLeavesItOff() {
        let app = launch("home-root")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        flip(toggle)
        XCTAssertTrue(app.alerts["Turn on root access?"].waitForExistence(timeout: 10), "no warning")
        XCTAssertTrue(app.alerts.staticTexts.containing(NSPredicate(format: "label CONTAINS 'The official Magisk app is installed at the next start'")).firstMatch.exists)
        app.alerts.buttons["Cancel"].tap()
        XCTAssertTrue(app.alerts.firstMatch.waitForNonExistence(timeout: 5))
        XCTAssertEqual(toggle.value as? String, "0", "Cancel turned root on")
    }

    @MainActor
    func testTurnOnTurnsRootOnAndTheSwitchTurnsItOffAtOnce() {
        let app = launch("home-root")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        flip(toggle)
        XCTAssertTrue(app.alerts["Turn on root access?"].waitForExistence(timeout: 10))
        app.alerts.buttons["Turn On"].firstMatch.tap()
        XCTAssertTrue(app.alerts.firstMatch.waitForNonExistence(timeout: 5))
        XCTAssertEqual(toggle.value as? String, "1", "Turn On left root off")
        // Off applies at once, with nothing asked.
        flip(toggle)
        XCTAssertFalse(app.alerts.firstMatch.waitForExistence(timeout: 2), "turning root off asked something")
        XCTAssertEqual(toggle.value as? String, "0")
    }

    @MainActor
    func testARootStateOpensWithTheSwitchOn() {
        let app = launch("home-root-on")
        XCTAssertEqual(rootSwitch(app).waitForExistence(timeout: 20) ? rootSwitch(app).value as? String : nil, "1")
    }

    @MainActor
    func testTheWarningIsTheOneThePreviewAsksFor() {
        let app = launch("home-root-warning")
        XCTAssertTrue(app.alerts["Turn on root access?"].waitForExistence(timeout: 20))
        XCTAssertTrue(app.alerts.buttons["Turn On"].exists && app.alerts.buttons["Cancel"].exists)
    }

    @MainActor
    func testAPausedRootSaysSoUnderTheSwitchWhichStaysOn() {
        let app = launch("home-root-paused")
        let toggle = rootSwitch(app)
        XCTAssertTrue(toggle.waitForExistence(timeout: 20))
        XCTAssertEqual(toggle.value as? String, "1")
        XCTAssertTrue(app.staticTexts["Paused after a failed start"].exists)
    }

    @MainActor
    func testTheBootScreenOfAStartThatInstallsTheAppHasAFifthStep() {
        let app = launch("boot-root")
        let step = app.staticTexts["Magisk app"].firstMatch
        XCTAssertTrue(step.waitForExistence(timeout: 20), "no Magisk app step")
        XCTAssertTrue(app.staticTexts["Installs the official Magisk app"].exists)
        // The step comes after Startup, the last of the four.
        let startup = app.staticTexts["Startup"].firstMatch
        XCTAssertTrue(startup.exists)
        XCTAssertLessThan(startup.frame.minY, step.frame.minY)
    }

    @MainActor
    func testAStartWithoutRootKeepsTheFourSteps() {
        let app = launch("boot")
        XCTAssertTrue(app.staticTexts["Startup"].firstMatch.waitForExistence(timeout: 20))
        XCTAssertFalse(app.staticTexts["Magisk app"].exists, "a start without root has the Magisk app step")
    }

    @MainActor
    func testTheAlertSaysTheMagiskAppCouldNotBeInstalled() {
        let app = launch("home-root-app-alert")
        let alert = app.alerts["Magisk app not installed"]
        XCTAssertTrue(alert.waitForExistence(timeout: 20), "no alert")
        XCTAssertTrue(alert.staticTexts.containing(NSPredicate(format: "label CONTAINS 'tries again at the next start'")).firstMatch.exists)
        alert.buttons["OK"].tap()
        XCTAssertTrue(app.alerts.firstMatch.waitForNonExistence(timeout: 5))
    }

    @MainActor
    func testThePausedCardOffersTryAgainAndTurnOffAndTryAgainStartsAndroid() {
        let app = launch("home-root-notice-paused")
        XCTAssertTrue(app.staticTexts["Root is paused"].firstMatch.waitForExistence(timeout: 20), "no paused card")
        XCTAssertTrue(app.buttons["Turn off root"].firstMatch.exists)
        app.buttons["Try with root again"].firstMatch.tap()
        // The pause ends and the start leaves Home for the boot screen, as Start does.
        XCTAssertTrue(app.staticTexts["Root is paused"].firstMatch.waitForNonExistence(timeout: 10), "the card stayed")
        XCTAssertTrue(app.staticTexts["Startup"].firstMatch.waitForExistence(timeout: 20), "Android did not start")
    }

    /// The card shows its text and both buttons as one block; the second
    /// button must still be a control of its own that an assistive technology
    /// can reach and activate, with its own name.
    @MainActor
    func testThePausedCardsSecondButtonIsAControlOfItsOwnWithItsOwnLabel() {
        let app = launch("home-root-notice-paused")
        XCTAssertTrue(app.staticTexts["Root is paused"].firstMatch.waitForExistence(timeout: 20), "no paused card")
        let tryAgain = app.buttons["Try with root again"].firstMatch
        let turnOff = app.buttons["Turn off root"].firstMatch
        XCTAssertTrue(tryAgain.exists, "no Try with root again button")
        XCTAssertTrue(turnOff.exists, "no Turn off root button")
        XCTAssertTrue(tryAgain.isHittable, "Try with root again cannot be reached")
        XCTAssertTrue(turnOff.isHittable, "Turn off root cannot be reached")
        XCTAssertEqual(turnOff.label, "Turn off root")
        XCTAssertNotEqual(tryAgain.label, turnOff.label, "the two buttons share one name")
        XCTAssertNotEqual(tryAgain.frame, turnOff.frame, "the two buttons are one control")
    }

    @MainActor
    func testTurnOffRootOnThePausedCardTurnsTheSwitchOff() {
        let app = launch("home-root-notice-paused")
        XCTAssertTrue(app.staticTexts["Root is paused"].firstMatch.waitForExistence(timeout: 20))
        app.buttons["Turn off root"].firstMatch.tap()
        XCTAssertTrue(app.staticTexts["Root is paused"].firstMatch.waitForNonExistence(timeout: 5))
        let toggle = rootSwitch(app)
        app.swipeUp()
        XCTAssertTrue(toggle.waitForExistence(timeout: 10))
        XCTAssertEqual(toggle.value as? String, "0")
    }

    @MainActor
    func testTheTurnedOffCardInformsAndCanBeDismissed() {
        let app = launch("home-root-notice-turned-off")
        let title = app.staticTexts["Root turned off"].firstMatch
        XCTAssertTrue(title.waitForExistence(timeout: 20), "no card")
        XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'turn it on again under Root'")).firstMatch.exists)
        app.buttons["Dismiss"].firstMatch.tap()
        XCTAssertTrue(title.waitForNonExistence(timeout: 5))
    }

    @MainActor
    func testAStoppedStartOffersAStartWithoutRootWhichStartsTheMachine() {
        let app = launch("home-root-notice-stopped")
        XCTAssertTrue(app.staticTexts["Start was stopped"].firstMatch.waitForExistence(timeout: 20), "no card")
        app.buttons["Start without root"].firstMatch.tap()
        // The start leaves Home, and a start without root has no Magisk app step.
        XCTAssertTrue(app.staticTexts["Start was stopped"].firstMatch.waitForNonExistence(timeout: 10), "the card stayed")
        XCTAssertFalse(app.staticTexts["Magisk app"].exists)
    }

    /// A row of the acknowledgements list, found by its name whatever the row
    /// puts together with it (the license beneath).
    private func acknowledgement(_ name: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH %@", name)).firstMatch
    }

    @MainActor
    func testTheAcknowledgementsListMagiskAndBusyBoxOnlyWhereTheReleaseCarriesRoot() {
        let with = launch("info-acknowledgements-root")
        XCTAssertTrue(acknowledgement("Linux", in: with).waitForExistence(timeout: 20), "the list did not open")
        XCTAssertTrue(acknowledgement("Magisk", in: with).exists, "no Magisk where the release carries root")
        XCTAssertTrue(acknowledgement("BusyBox", in: with).exists, "no BusyBox where the release carries root")
        with.terminate()
        let without = launch("info-acknowledgements")
        XCTAssertTrue(acknowledgement("Linux", in: without).waitForExistence(timeout: 20), "the list did not open")
        XCTAssertFalse(acknowledgement("Magisk", in: without).exists, "Magisk is listed for a release without root")
        XCTAssertFalse(acknowledgement("BusyBox", in: without).exists, "BusyBox is listed for a release without root")
    }
}
