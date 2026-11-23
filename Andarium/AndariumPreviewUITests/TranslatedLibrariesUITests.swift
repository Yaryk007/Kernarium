// SPDX-License-Identifier: GPL-2.0-or-later
import XCTest

/// Home's AOT row in the Simulator preview: shown, between "120 Hz" and JIT in
/// the Performance section, in a build that carries the translated-code
/// framework (the preview plays one with `-screen home-aot`), absent from one
/// that does not (`home`), on by default, turned off and on again, and dimmed
/// with its reason while the next start runs on the JIT engine, which does not
/// use the translated code.
final class TranslatedLibrariesUITests: XCTestCase {
    private static let identifier = "home-aot-switch"
    private static let title = "AOT"
    private static let note = "Translated system libraries run faster. Turn off only to troubleshoot."
    private static let jitNote = "Not used while JIT is on."

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

    /// Scrolls Home until `element` is on screen: a row below the fold of a
    /// phone is drawn from the first frame, but a swipe brings it into reach.
    @MainActor
    private func reveal(_ element: XCUIElement, in app: XCUIApplication, file: StaticString = #filePath, line: UInt = #line) {
        var swipes = 0
        while !(element.waitForExistence(timeout: swipes == 0 ? 10 : 1) && element.isHittable), swipes < 10 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertTrue(element.exists && element.isHittable, "\(element) is not on screen", file: file, line: line)
    }

    /// A text by its whole label: an identifier query is bounded to 128
    /// characters.
    private func text(_ label: String, in app: XCUIApplication) -> XCUIElement {
        app.staticTexts.matching(NSPredicate(format: "label == %@", label)).firstMatch
    }

    private func value(_ element: XCUIElement) -> String {
        (element.value as? String) ?? "none"
    }

    /// Taps where the row's control is, at its trailing end: the row reads as
    /// one switch for VoiceOver, and a touch on its words is not the control's.
    private func tapControl(of row: XCUIElement) {
        row.coordinate(withNormalizedOffset: CGVector(dx: 0.88, dy: 0.5)).tap()
    }

    /// Waits, within `timeout`, until the row's value is `value`.
    private func expect(_ row: XCUIElement, value: String, _ message: String, timeout: TimeInterval = 10) {
        let holds = expectation(for: NSPredicate(format: "value == %@", value), evaluatedWith: row)
        wait(for: [holds], timeout: timeout)
        XCTAssertEqual(self.value(row), value, message)
    }

    @MainActor
    func testTheSwitchIsOnByDefaultAndTurnsOffAndOnAgain() {
        let app = launch("home-aot")
        let toggle = app.switches[Self.identifier].firstMatch
        reveal(toggle, in: app)
        XCTAssertEqual(value(toggle), "1", "AOT starts on")
        // VoiceOver reads the title and then the note, both as the label.
        XCTAssertEqual(toggle.label, "\(Self.title), \(Self.note)", "the switch's label is \(toggle.label)")
        XCTAssertTrue(text(Self.note, in: app).exists, "the note under the title is missing")
        XCTAssertFalse(text(Self.jitNote, in: app).exists, "the JIT line shows while JIT is off")

        tapControl(of: toggle)
        expect(toggle, value: "0", "the first tap did not turn AOT off")
        tapControl(of: toggle)
        expect(toggle, value: "1", "the second tap did not turn it back on")
    }

    @MainActor
    func testTheRowIsBetweenTheHundredAndTwentyHertzRowAndJit() {
        let app = launch("home-aot")
        let toggle = app.switches[Self.identifier].firstMatch
        reveal(toggle, in: app)
        let refresh = app.switches["home-high-refresh-switch"].firstMatch
        let jit = app.switches["JIT"].firstMatch
        XCTAssertTrue(refresh.exists, "the 120 Hz row is missing")
        XCTAssertTrue(jit.exists, "the JIT row is missing")
        XCTAssertLessThan(refresh.frame.maxY, toggle.frame.minY, "the AOT row is not below 120 Hz")
        XCTAssertLessThan(toggle.frame.maxY, jit.frame.minY, "the AOT row is not above the JIT row")
        // Each setting is its own group, with its note beneath the panel.
        let refreshNote = text("Smoother motion, more battery use.", in: app)
        let aotNote = text(Self.note, in: app)
        XCTAssertTrue(refreshNote.exists && aotNote.exists, "a note is missing")
        XCTAssertTrue(
            refresh.frame.maxY < refreshNote.frame.minY && refreshNote.frame.maxY < toggle.frame.minY,
            "the 120 Hz note is not between its switch and the AOT switch")
        XCTAssertTrue(
            toggle.frame.maxY < aotNote.frame.minY && aotNote.frame.maxY < jit.frame.minY,
            "the AOT note is not between its switch and the JIT switch")
        // One section: a single Performance heading above the three rows.
        XCTAssertEqual(app.staticTexts.matching(identifier: "Performance").count, 1)
    }

    @MainActor
    func testTheSwitchShowsOffWhenItIsOff() {
        let app = launch("home-aot-off")
        let toggle = app.switches[Self.identifier].firstMatch
        reveal(toggle, in: app)
        XCTAssertEqual(value(toggle), "0")
        XCTAssertFalse(text(Self.jitNote, in: app).exists)
    }

    @MainActor
    func testABuildWithoutTheFrameworkHasNoRow() {
        let app = launch("home")
        XCTAssertTrue(app.staticTexts["Virtual machine"].waitForExistence(timeout: 20), "Home did not open")
        XCTAssertFalse(app.switches[Self.identifier].firstMatch.exists, "a build without the libraries shows the row")
        XCTAssertFalse(text(Self.note, in: app).exists)
    }

    @MainActor
    func testWhileTheNextStartRunsOnTheJitTheRowIsDimmedAndSaysWhy() {
        let app = launch("home-aot-jit")
        let toggle = app.switches[Self.identifier].firstMatch
        reveal(toggle, in: app)
        XCTAssertTrue(text(Self.jitNote, in: app).waitForExistence(timeout: 10), "no word says AOT is not used")
        XCTAssertFalse(text(Self.note, in: app).exists, "the note shows beside the reason")
        // The reason is heard with the title, not as a hint that hints-off drops.
        XCTAssertEqual(toggle.label, "\(Self.title), \(Self.jitNote)", "the dimmed switch's label is \(toggle.label)")
        // A touch on the dimmed switch changes nothing.
        tapControl(of: toggle)
        Thread.sleep(forTimeInterval: 1)
        XCTAssertEqual(value(toggle), "1", "the switch worked while the start runs on the JIT")

        // Turning JIT off hands the start to the engine that uses them.
        let jit = app.switches["JIT"].firstMatch
        reveal(jit, in: app)
        tapControl(of: jit)
        expect(jit, value: "0", "JIT did not turn off")
        let gone = expectation(for: NSPredicate(format: "exists == false"), evaluatedWith: text(Self.jitNote, in: app))
        wait(for: [gone], timeout: 10)
        XCTAssertTrue(text(Self.note, in: app).waitForExistence(timeout: 10), "the note did not come back")
        XCTAssertEqual(toggle.label, "\(Self.title), \(Self.note)", "the label did not follow: \(toggle.label)")
        // The switch works again.
        tapControl(of: toggle)
        expect(toggle, value: "0", "the switch stayed dimmed after JIT went off")
    }
}
