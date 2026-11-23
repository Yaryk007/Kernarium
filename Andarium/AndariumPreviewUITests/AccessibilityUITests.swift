// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// The Preview UI tests read English labels. Every launch carries its own
/// language, so that no test depends on the language a Simulator was created
/// with (a new one takes the Mac's).
let previewTestLanguage = ["-AppleLanguages", "(en)", "-AppleLocale", "en_US"]

/// The Simulator preview driven as VoiceOver drives it: the screens listed
/// here pass Apple's audit for element descriptions and traits (the audit
/// waits for the sheet or card a screen opens on), the switch for Android's
/// screen reader sits last in the session's Settings with every state told,
/// and under VoiceOver a card asks before the screen reader is turned on. The
/// Simulator has no VoiceOver: a test drives the interface, not the speech.
/// `-screen` names a screen of `AppEntry`.
final class AccessibilityUITests: XCTestCase {
    /// Each screen with what it shows once it is up, which the audit waits for.
    private static let screens: [(name: String, ready: String)] = [
        ("welcome", ""), ("home", ""), ("home-new", ""), ("home-update", ""), ("home-update-failed", ""),
        ("home-blocked", ""), ("home-ended", ""), ("home-memory-warning", ""), ("home-mic-denied", ""),
        ("home-aot", ""), ("home-aot-off", ""), ("home-aot-jit", ""),
        ("create", ""), ("resize", ""), ("system-edit", ""), ("info", ""), ("info-acknowledgements", ""),
        ("info-license", ""), ("boot", ""), ("boot-first", ""), ("boot-recovery", ""),
        ("session", "side-keys"), ("session-open", "side-keys-power"), ("session-stats", ""),
        ("session-graphics-notice", ""), ("settings", "Settings"),
        ("settings-accessibility", "settings-talkback"), ("settings-adb", "Settings"),
        ("install-done", ""), ("install-failed", ""), ("space-pause", "space-pause"),
        ("space-pause-running", "space-pause"), ("recovery-open", "side-keys-power"), ("failed", ""), ("failed-memory", ""),
    ]

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

    /// Waits, within `timeout`, until `element`'s value is `value`.
    private func expect(_ element: XCUIElement, value: String, timeout: TimeInterval = 10) {
        let holds = expectation(for: NSPredicate(format: "value == %@", value), evaluatedWith: element)
        wait(for: [holds], timeout: timeout)
    }

    /// Settings' switch for Android's screen reader, scrolled to if the sheet
    /// opened above it: it is the last section of a long list, which draws a
    /// row only once it is near the screen.
    @MainActor
    private func talkBackSwitch(in app: XCUIApplication) -> XCUIElement {
        let toggle = app.switches["settings-talkback"]
        var swipes = 0
        while !(toggle.waitForExistence(timeout: swipes == 0 ? 10 : 1) && toggle.isHittable), swipes < 12 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertTrue(toggle.exists && toggle.isHittable, "Settings has no switch for the screen reader on screen")
        return toggle
    }

    /// Flips a SwiftUI toggle: its row's own switch, not the label beside it.
    @MainActor
    private func flip(_ toggle: XCUIElement) {
        let control = toggle.switches.firstMatch
        (control.exists ? control : toggle).tap()
    }

    /// Opens the session's Settings through the side keys, as a person does.
    @MainActor
    private func openSettings(in app: XCUIApplication) {
        let keys = app.descendants(matching: .any)["side-keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 90), "the session did not start")
        // The keys slide in with the session; a tap before they land misses.
        let landed = expectation(for: NSPredicate(format: "hittable == true"), evaluatedWith: keys)
        wait(for: [landed], timeout: 10)
        keys.tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 10), "the side keys have no Settings")
        settings.tap()
    }

    @MainActor
    func testEveryScreenHasDescriptionsAndTraits() throws {
        continueAfterFailure = true
        let cases = Self.screens.map { ($0.name, $0.ready, [String]()) }
            + [("session", "screen-reader-card", ["-voiceOver", "YES"])]
        for (screen, ready, arguments) in cases {
            let app = XCUIApplication()
            app.launchArguments = previewTestLanguage + ["-screen", screen] + arguments
            app.launch()
            XCTAssertTrue(app.wait(for: .runningForeground, timeout: 20), "\(screen) did not open")
            // The audit reads what the screen shows once it is up: the sheet or
            // the card it opens on, else the first element.
            if ready.isEmpty {
                _ = app.descendants(matching: .any).firstMatch.waitForExistence(timeout: 10)
            } else {
                XCTAssertTrue(
                    app.descendants(matching: .any)[ready].waitForExistence(timeout: 20), "\(screen) has no \(ready)")
            }
            try app.performAccessibilityAudit(for: [.sufficientElementDescription, .trait]) { issue in
                let element = issue.element.map { "\($0.label) (\($0.identifier))" } ?? "no element"
                XCTFail("\(screen): \(issue.compactDescription): \(element)")
                return true
            }
            app.terminate()
        }
    }

    @MainActor
    func testTheSideKeysCarryNoAccessibilityKey() {
        let app = launch("session-open")
        XCTAssertTrue(app.descendants(matching: .any)["side-keys-power"].waitForExistence(timeout: 10))
        XCTAssertFalse(app.descendants(matching: .any)["side-keys-accessibility"].exists)
        XCTAssertTrue(app.buttons["Settings"].exists)
    }

    @MainActor
    func testTheSwitchTurnsTheScreenReaderOnAndOff() {
        let app = launch("settings-accessibility")
        let toggle = talkBackSwitch(in: app)
        XCTAssertEqual(toggle.value as? String, "0")
        XCTAssertTrue(toggle.isEnabled)
        flip(toggle)
        expect(toggle, value: "1")
        flip(toggle)
        expect(toggle, value: "0")
    }

    @MainActor
    func testTheSwitchShowsTheScreenReaderOnWhereAndroidHasItOn() {
        let app = launch("settings-accessibility", "-talkBack", "on")
        expect(talkBackSwitch(in: app), value: "1")
    }

    @MainActor
    func testASystemWithoutAScreenReaderShowsTheSwitchUnavailableWithItsReason() {
        let app = launch("settings-accessibility", "-talkBack", "missing")
        let toggle = talkBackSwitch(in: app)
        XCTAssertFalse(toggle.isEnabled)
        XCTAssertEqual(toggle.value as? String, "0")
        XCTAssertTrue(app.staticTexts["Not in this system image"].exists)
    }

    @MainActor
    func testAReadThatGetsNoAnswerSaysSoAndOffersTryAgain() {
        let app = launch("settings-accessibility", "-talkBack", "unreadable")
        let toggle = talkBackSwitch(in: app)
        XCTAssertTrue(app.staticTexts["Couldn\u{2019}t check"].waitForExistence(timeout: 10))
        XCTAssertFalse(toggle.isEnabled)
        XCTAssertTrue(app.buttons["settings-talkback-try-again"].exists)
    }

    @MainActor
    func testAChangeThatFailsSaysSoOnTheRowAndKeepsSettingsOpen() {
        let app = launch("settings-accessibility", "-talkBack", "fails")
        let toggle = talkBackSwitch(in: app)
        flip(toggle)
        XCTAssertTrue(app.staticTexts["Didn\u{2019}t turn on"].waitForExistence(timeout: 10))
        XCTAssertTrue(app.buttons["settings-talkback-try-again"].exists)
        XCTAssertEqual(toggle.value as? String, "0")
        // No alert took Settings away.
        XCTAssertTrue(toggle.exists)
    }

    @MainActor
    func testAChangeUnderWayDimsTheSwitchAndSaysSo() {
        let app = launch("settings-accessibility", "-talkBack", "slow")
        let toggle = talkBackSwitch(in: app)
        flip(toggle)
        XCTAssertTrue(app.staticTexts["Turning on\u{2026}"].waitForExistence(timeout: 10))
        XCTAssertFalse(toggle.isEnabled)
    }

    @MainActor
    func testAFileTransferHoldsTheSwitchAndSaysUntilWhen() {
        let app = launch("settings-accessibility", "-talkBack", "transfer")
        let toggle = talkBackSwitch(in: app)
        XCTAssertTrue(app.staticTexts["Available when the file transfer ends"].waitForExistence(timeout: 10))
        XCTAssertFalse(toggle.isEnabled)
    }

    @MainActor
    func testAndroidsAccessibilitySettingsOpenOnlyWhileTheScreenReaderIsOn() {
        let off = launch("settings-accessibility")
        _ = talkBackSwitch(in: off)
        XCTAssertFalse(off.buttons["settings-accessibility-settings"].isEnabled)
        off.terminate()
        let app = launch("settings-accessibility", "-talkBack", "on")
        let toggle = talkBackSwitch(in: app)
        let button = app.buttons["settings-accessibility-settings"]
        XCTAssertTrue(button.isEnabled)
        XCTAssertEqual(button.label, "Android\u{2019}s accessibility settings")
        button.tap()
        let gone = expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: toggle)
        wait(for: [gone], timeout: 10)
    }

    // MARK: The card

    @MainActor
    func testUnderVoiceOverACardAsksBeforeTheScreenReaderIsTurnedOn() {
        let app = launch("session", "-voiceOver", "YES")
        let turnOn = app.buttons["screen-reader-card-turn-on"]
        XCTAssertTrue(turnOn.waitForExistence(timeout: 20), "no card asks")
        XCTAssertTrue(app.buttons["screen-reader-card-not-now"].exists)
        turnOn.tap()
        let gone = expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: turnOn)
        wait(for: [gone], timeout: 10)
        openSettings(in: app)
        expect(talkBackSwitch(in: app), value: "1")
    }

    @MainActor
    func testNotNowLeavesTheScreenReaderOffAndTheCardAway() {
        let app = launch("session", "-voiceOver", "YES")
        let notNow = app.buttons["screen-reader-card-not-now"]
        XCTAssertTrue(notNow.waitForExistence(timeout: 20), "no card asks")
        notNow.tap()
        let gone = expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: notNow)
        wait(for: [gone], timeout: 10)
        openSettings(in: app)
        XCTAssertEqual(talkBackSwitch(in: app).value as? String, "0")
    }

    @MainActor
    func testACardThatCannotTurnItOnKeepsTheReasonAndTryAgain() {
        let app = launch("session", "-voiceOver", "YES", "-talkBack", "fails")
        let turnOn = app.buttons["screen-reader-card-turn-on"]
        XCTAssertTrue(turnOn.waitForExistence(timeout: 20), "no card asks")
        turnOn.tap()
        XCTAssertTrue(app.staticTexts["screen-reader-card-failure"].waitForExistence(timeout: 10))
        XCTAssertEqual(turnOn.label, "Try Again")
    }

    @MainActor
    func testTheCardStartsWithItsTitleAndEndsWithItsButtons() {
        let app = launch("session", "-voiceOver", "YES")
        let card = app.descendants(matching: .any)["screen-reader-card"]
        XCTAssertTrue(card.waitForExistence(timeout: 20), "no card asks")
        // VoiceOver lands on a modal's first element and reads on in order.
        let texts = card.descendants(matching: .any).allElementsBoundByIndex
            .filter { $0.elementType == .staticText || $0.elementType == .button }
        XCTAssertEqual(texts.first?.label, "Turn on TalkBack?", "the card does not start with its title")
        XCTAssertEqual(texts.last?.label, "Turn On")
    }

    /// The footer under Settings' screen reader row, scrolled to; it is the
    /// last text of the last section.
    @MainActor
    private func footer(in app: XCUIApplication, saying words: String) -> XCUIElement {
        _ = talkBackSwitch(in: app)
        let text = app.staticTexts.containing(NSPredicate(format: "label CONTAINS %@", words)).firstMatch
        var swipes = 0
        while !(text.waitForExistence(timeout: 1) && text.isHittable), swipes < 6 {
            app.swipeUp()
            swipes += 1
        }
        return text
    }

    @MainActor
    func testTheCardAndTheFooterNameTheEdgeTheKeysAreOn() {
        var app = launch("session", "-voiceOver", "YES")
        XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS %@", "side keys on the right edge"))
            .firstMatch.waitForExistence(timeout: 20), "the card does not name the right edge")
        app = launch("session-closed-left", "-voiceOver", "YES")
        XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS %@", "side keys on the left edge"))
            .firstMatch.waitForExistence(timeout: 20), "the card does not name the left edge")
        app = launch("settings-accessibility")
        XCTAssertTrue(footer(in: app, saying: "side keys on the right edge").exists, "the footer does not name the right edge")
        app = launch("settings-accessibility-left")
        XCTAssertTrue(footer(in: app, saying: "side keys on the left edge").exists, "the footer does not name the left edge")
    }

    /// The keys are dragged across, as a person moves them; the way back
    /// follows them to the other edge.
    @MainActor
    func testMovingTheKeysMovesTheWayBack() {
        let app = launch("session")
        let keys = app.descendants(matching: .any)["side-keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 90), "the session did not start")
        let landed = expectation(for: NSPredicate(format: "hittable == true"), evaluatedWith: keys)
        wait(for: [landed], timeout: 10)
        keys.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).press(
            forDuration: 0.1, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.1, dy: 0.4)))
        openSettings(in: app)
        XCTAssertTrue(footer(in: app, saying: "side keys on the left edge").exists, "the way back did not follow the keys")
    }

    /// Turning the screen reader off is the person's answer: the card that asks
    /// does not come back when Settings closes.
    @MainActor
    func testTurningTheScreenReaderOffInSettingsDoesNotBringTheCardBack() {
        let app = launch("session", "-voiceOver", "YES", "-talkBack", "on")
        openSettings(in: app)
        let toggle = talkBackSwitch(in: app)
        expect(toggle, value: "1")
        flip(toggle)
        expect(toggle, value: "0")
        app.buttons["Done"].tap()
        XCTAssertTrue(app.descendants(matching: .any)["side-keys"].waitForExistence(timeout: 10))
        XCTAssertFalse(app.buttons["screen-reader-card-turn-on"].waitForExistence(timeout: 5), "the card asked again")
    }

    @MainActor
    func testASystemWithoutAScreenReaderIsToldSoOnTheCard() {
        let app = launch("session", "-voiceOver", "YES", "-talkBack", "missing")
        let ok = app.buttons["screen-reader-card-ok"]
        XCTAssertTrue(ok.waitForExistence(timeout: 20))
        XCTAssertFalse(app.buttons["screen-reader-card-turn-on"].exists)
    }

    @MainActor
    func testWithoutVoiceOverThereIsNoCard() {
        let app = launch("session")
        XCTAssertTrue(app.descendants(matching: .any)["side-keys"].waitForExistence(timeout: 20))
        XCTAssertFalse(app.buttons["screen-reader-card-turn-on"].waitForExistence(timeout: 3))
    }

    @MainActor
    func testAFirstStartUnderVoiceOverAsksFirstAndTurnsNothingOn() {
        let app = launch("home-new", "-voiceOver", "YES")
        let start = app.buttons["Start and set up"]
        XCTAssertTrue(start.waitForExistence(timeout: 10))
        start.tap()
        let notNow = app.buttons["screen-reader-card-not-now"]
        XCTAssertTrue(notNow.waitForExistence(timeout: 90), "no card asks")
        notNow.tap()
        openSettings(in: app)
        XCTAssertEqual(talkBackSwitch(in: app).value as? String, "0")
    }

    @MainActor
    func testAFirstStartWithoutVoiceOverLeavesTheScreenReaderOff() {
        let app = launch("home-new")
        let start = app.buttons["Start and set up"]
        XCTAssertTrue(start.waitForExistence(timeout: 10))
        start.tap()
        openSettings(in: app)
        XCTAssertEqual(talkBackSwitch(in: app).value as? String, "0")
        XCTAssertFalse(app.buttons["screen-reader-card-turn-on"].exists)
    }
}
