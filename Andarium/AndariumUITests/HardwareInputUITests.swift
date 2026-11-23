// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit
import XCTest

/// The hardware keyboard and mouse in the real application. The launch asks for
/// the input probe (`ANDARIUM_INPUT_PROBE`): the session opens with no guest
/// behind it, and what the guest's keyboard and mouse would receive goes to a
/// log the session shows as one accessibility element. XCUITest's synthetic
/// keys and pointer events are the hardware.
///
/// The probe's guest registers each device it is plugged, as a kernel with
/// the hot-plug topic does; one test launches the other kind, which never
/// registers (a kernel without it).
///
/// XCUITest has no pointer on an iPhone ("Pointer events are not supported for
/// this device"), so the iPhone's tests are the keys and the absence of the
/// iPad's mouse setting.
@MainActor
final class HardwareInputUITests: XCTestCase {
    private var app: XCUIApplication!

    override func setUp() async throws {
        continueAfterFailure = false
        app = XCUIApplication()
        try launch(probe: "1")
    }

    /// Launches the application with the input probe: `1` is a guest that
    /// registers its devices, `unregistered` one that never does.
    private func launch(probe mode: String) throws {
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = mode
        app.launch()
        XCTAssertTrue(probe.waitForExistence(timeout: 90), "the session did not open")
        // The display takes the keyboard focus once it is in a window.
        sleep(2)
        // And it is not the software keyboard that it takes it with, before
        // any hardware key has been typed either.
        XCTAssertFalse(app.keyboards.firstMatch.exists, "the guest's focus raised the software keyboard at launch")
    }

    // MARK: Reading the guest's end

    private var probe: XCUIElement { app.descendants(matching: .any)["inputProbeLog"] }

    private var lines: [String] {
        probe.label.split(separator: "\n").map(String.init)
    }

    /// Waits for a line that satisfies `matches`; false at the deadline.
    @discardableResult
    private func waitForLine(timeout: TimeInterval = 20, _ matches: (String) -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if lines.contains(where: matches) { return true }
            usleep(200_000)
        }
        return lines.contains(where: matches)
    }

    private func waitForLine(containing text: String, timeout: TimeInterval = 20) -> Bool {
        waitForLine(timeout: timeout) { $0.contains(text) }
    }

    /// The pointer lines as (x, y, buttons, inRange).
    private var pointerReports: [(x: Int, y: Int, buttons: Int, inRange: Bool)] {
        lines.compactMap { line in
            let words = line.split(separator: " ")
            guard words.count == 5, words[0] == "pointer", let x = Int(words[1]), let y = Int(words[2]),
                  words[3].hasPrefix("b"), let buttons = Int(words[3].dropFirst()) else { return nil }
            return (x, y, buttons, words[4] == "in")
        }
    }

    /// The mouse lines as (dx, dy, buttons, wheel, horizontalWheel).
    private var mouseReports: [(dx: Int, dy: Int, buttons: Int, wheel: Int, horizontalWheel: Int)] {
        lines.compactMap { line in
            let words = line.split(separator: " ")
            guard words.count == 6, words[0] == "mouse", let dx = Int(words[1]), let dy = Int(words[2]),
                  let buttons = Int(words[3].dropFirst()), let wheel = Int(words[4].dropFirst()),
                  let horizontal = Int(words[5].dropFirst()) else { return nil }
            return (dx, dy, buttons, wheel, horizontal)
        }
    }

    private func index(of line: String) -> Int? { lines.firstIndex(of: line) }

    /// Does `action` until a line that satisfies `matches` is in the log, up to
    /// `attempts` times. The Simulator reports its keyboard and its mouse only
    /// around each synthetic event, and the first event after a launch can come
    /// before that report and reach no view (the display's own log shows no
    /// press at all); a person would press again.
    ///
    /// How many attempts it took goes to the results as an attachment, so that
    /// a test that has come to need many shows it.
    @discardableResult
    private func perform(
        _ action: () -> Void, until matches: (String) -> Bool, attempts: Int = 4, in function: String = #function
    ) -> Bool {
        for attempt in 1...attempts {
            action()
            if waitForLine(timeout: 8, matches) {
                noteAttempts(attempt, of: attempts, in: function)
                return true
            }
        }
        noteAttempts(attempts, of: attempts, in: function, succeeded: false)
        return false
    }

    /// The same for a condition that is not in the log.
    private func perform(
        _ action: () -> Void, untilTrue condition: () -> Bool, attempts: Int = 4, in function: String = #function
    ) -> Bool {
        for attempt in 1...attempts {
            action()
            let deadline = Date().addingTimeInterval(8)
            while Date() < deadline {
                if condition() {
                    noteAttempts(attempt, of: attempts, in: function)
                    return true
                }
                usleep(200_000)
            }
        }
        noteAttempts(attempts, of: attempts, in: function, succeeded: false)
        return false
    }

    private func noteAttempts(_ used: Int, of attempts: Int, in function: String, succeeded: Bool = true) {
        let note = XCTAttachment(string: "\(function): \(used) of \(attempts) attempts\(succeeded ? "" : ", failed")")
        note.name = "attempts \(function)"
        note.lifetime = .keepAlways
        add(note)
    }

    private func waitForValue(of element: XCUIElement, _ value: String, timeout: TimeInterval = 15) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if element.value as? String == value { return true }
            usleep(200_000)
        }
        return element.value as? String == value
    }

    private var isPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    // MARK: Settings

    private func openSettings() {
        let keys = app.buttons["Side keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 40), "the side keys are not on screen")
        keys.tap()
        let settings = app.buttons["Settings"]
        XCTAssertTrue(settings.waitForExistence(timeout: 20), "the side keys did not open")
        settings.tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 30), "Settings did not open")
    }

    /// The probe's text field in the open Settings. An iPhone opens Settings
    /// at its medium detent, whose rows end above the field, and the list
    /// makes no row it does not show, so the sheet is drawn up until the
    /// field is there.
    private func probeField() -> XCUIElement {
        let field = app.textFields["inputProbeField"]
        for _ in 0..<3 where !field.waitForExistence(timeout: 5) {
            app.collectionViews.firstMatch.swipeUp()
        }
        return field
    }

    /// The pointer row of the open Settings. The list makes no row it does not
    /// show, and the Performance section above the row pushes it below what an
    /// iPad's sheet builds at first, so the sheet is drawn up until the row is
    /// there (as `probeField` does for the field).
    private func pointerSwitch() -> XCUIElement {
        let toggle = app.switches["pointerAsTouch"]
        for _ in 0..<5 where !(toggle.waitForExistence(timeout: 3) && toggle.isHittable) {
            app.collectionViews.firstMatch.swipeUp()
        }
        return toggle
    }

    private func closeSettings() {
        // A tap can be lost while the host is loaded: tap Done again if the
        // sheet is still there.
        let bar = app.navigationBars["Settings"]
        var taps = 0
        for _ in 0..<3 where bar.exists {
            app.buttons["Done"].tap()
            taps += 1
            if bar.waitForNonExistence(timeout: 10) { break }
        }
        let note = XCTAttachment(string: "closing Settings took \(taps) of 3 taps on Done")
        note.name = "attempts closeSettings"
        note.lifetime = .keepAlways
        add(note)
        XCTAssertTrue(bar.waitForNonExistence(timeout: 20), "Settings did not close")
        sleep(1)
    }

    // MARK: Keys

    func testKeysReachTheGuestAsPositionsAfterTheKeyboardIsPlugged() {
        XCTAssertTrue(perform({ app.typeKey("a", modifierFlags: []) }, until: { $0 == "key 0x4 up" }), "log: \(lines)")
        let plug = index(of: "connect keyboard 1")
        let down = index(of: "key 0x4 down")
        let up = index(of: "key 0x4 up")
        XCTAssertNotNil(plug, "log: \(lines)")
        XCTAssertNotNil(down, "log: \(lines)")
        XCTAssertNotNil(up, "log: \(lines)")
        if let plug, let down, let up {
            XCTAssertLessThan(plug, down)
            XCTAssertLessThan(down, up)
        }
    }

    func testModifiersAreTheirOwnKeysAroundTheKey() {
        XCTAssertTrue(
            perform({ app.typeKey("c", modifierFlags: .command) }, until: { $0 == "key 0xe3 up" }), "log: \(lines)")
        // Whatever attempt was the first to arrive, a whole press is four lines.
        let keys = lines.filter { $0.hasPrefix("key ") }
        XCTAssertEqual(keys.suffix(4), ["key 0xe3 down", "key 0x6 down", "key 0x6 up", "key 0xe3 up"])
        let beforeShiftControl = keys.count

        app.typeKey("a", modifierFlags: [.shift, .control])
        XCTAssertTrue(waitForLine(containing: "key 0x4 up"), "log: \(lines)")
        let more = Array(lines.filter { $0.hasPrefix("key ") }.dropFirst(beforeShiftControl))
        // Both modifiers go down before the key and come up after it.
        XCTAssertEqual(more.count, 6, "log: \(lines)")
        XCTAssertEqual(Set(more.prefix(2)), ["key 0xe0 down", "key 0xe1 down"])
        XCTAssertEqual(Array(more[2...3]), ["key 0x4 down", "key 0x4 up"])
        XCTAssertEqual(Set(more.suffix(2)), ["key 0xe0 up", "key 0xe1 up"])
    }

    func testTheKeysThatAreCombinationsOfTheSystemReachTheGuestToo() {
        // Command-K, Command-W and Control-Space are the guest's.
        perform({ app.typeKey("k", modifierFlags: .command) }, until: { $0 == "key 0xe3 up" })
        app.typeKey("w", modifierFlags: .command)
        app.typeKey(XCUIKeyboardKey.space.rawValue, modifierFlags: .control)
        XCTAssertTrue(waitForLine(containing: "key 0x2c up"), "log: \(lines)")
        let downs = lines.filter { $0.hasSuffix("down") }
        XCTAssertTrue(downs.contains("key 0xe3 down"))
        XCTAssertTrue(downs.contains("key 0xe0 down"))
        XCTAssertTrue(downs.contains("key 0xe down"))
        XCTAssertTrue(downs.contains("key 0x1a down"))
        XCTAssertTrue(downs.contains("key 0x2c down"))
    }

    func testNoSoftwareKeyboardAppearsWhenTheGuestTakesTheKeys() {
        XCTAssertTrue(perform({ app.typeKey("a", modifierFlags: []) }, until: { $0 == "key 0x4 up" }), "log: \(lines)")
        sleep(1)
        XCTAssertFalse(app.keyboards.firstMatch.exists, "the guest's focus raised the software keyboard")
    }

    // MARK: A sheet has the keys

    func testASheetsTextFieldTakesTheKeysWhileItIsOpenAndTheGuestTakesThemBack() {
        XCTAssertTrue(perform({ app.typeKey("z", modifierFlags: []) }, until: { $0 == "key 0x1d up" }), "log: \(lines)")
        let before = lines.filter { $0.hasPrefix("key ") }.count

        openSettings()
        let field = probeField()
        XCTAssertTrue(field.waitForExistence(timeout: 10), "Settings has no probe field")
        field.tap()
        let text = { (field.value as? String) ?? "" }
        XCTAssertTrue(
            perform({ app.typeKey("h", modifierFlags: []) }, untilTrue: { text().contains("h") }),
            "the field took no key: \(text())")
        app.typeKey("i", modifierFlags: [])
        XCTAssertTrue(
            perform({}, untilTrue: { text().hasSuffix("i") }, attempts: 1), "the field took no second key: \(text())")
        sleep(1)
        XCTAssertEqual(
            lines.filter { $0.hasPrefix("key ") }.count, before, "the guest received the keys the sheet took: \(lines)")

        closeSettings()
        XCTAssertTrue(
            perform({ app.typeKey("c", modifierFlags: []) }, until: { $0 == "key 0x6 up" }),
            "the guest did not take the keys back: \(lines)")
    }

    func testASheetWithNoFieldTakesNoKeyFromTheGuestEither() {
        // The guest does take a key while no sheet is up: the negative below
        // is not a guest that takes none.
        XCTAssertTrue(perform({ app.typeKey("d", modifierFlags: []) }, until: { $0 == "key 0x7 up" }), "log: \(lines)")
        let before = lines.filter { $0.hasPrefix("key ") }.count

        openSettings()
        // The field is not focused: no text view has the key, and the sheet
        // does not give it to the guest behind it.
        app.typeKey("d", modifierFlags: [])
        sleep(2)
        XCTAssertEqual(
            lines.filter { $0.hasPrefix("key ") }.count, before, "the guest received a key the sheet took: \(lines)")
        closeSettings()
        XCTAssertTrue(
            perform({ app.typeKey("e", modifierFlags: []) }, until: { $0 == "key 0x8 up" }),
            "the guest did not take the keys back: \(lines)")
    }

    // MARK: Pointer (iPad)

    func testThePointerHoversClicksDragsAndScrollsAsAMouse() throws {
        try XCTSkipUnless(isPad, "XCUITest has no pointer on an iPhone")
        let window = app.windows.firstMatch
        let a = window.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.3))
        let b = window.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.5))

        XCTAssertTrue(
            perform({ a.hover(); b.hover() }, until: { $0.hasPrefix("pointer ") && $0.hasSuffix(" in") }),
            "the mouse was not plugged: \(lines)")
        XCTAssertTrue(lines.contains("connect mouse 1"), "log: \(lines)")
        // Where the pointer is on the display, on the touchscreen's range.
        let hovered = try XCTUnwrap(pointerReports.last { $0.inRange && $0.buttons == 0 })
        XCTAssertEqual(Double(hovered.x), 0.6 * 32767, accuracy: 0.15 * 32767)
        XCTAssertEqual(Double(hovered.y), 0.5 * 32767, accuracy: 0.15 * 32767)

        b.click()
        XCTAssertTrue(waitForLine { $0.hasPrefix("pointer ") && $0.contains(" b1 ") }, "a click: \(lines)")
        XCTAssertTrue(waitForLine { $0.hasPrefix("pointer ") && $0.contains(" b0 in") }, "the button's release: \(lines)")

        b.rightClick()
        XCTAssertTrue(waitForLine { $0.hasPrefix("pointer ") && $0.contains(" b2 ") }, "a right click: \(lines)")

        let reportsBeforeDrag = pointerReports.count
        a.click(forDuration: 0.2, thenDragTo: b)
        XCTAssertTrue(waitForLine(timeout: 20) { _ in self.pointerReports.count > reportsBeforeDrag + 2 }, "log: \(lines)")
        let drag = pointerReports.dropFirst(reportsBeforeDrag).filter { $0.buttons == 1 }
        XCTAssertGreaterThanOrEqual(Set(drag.map { $0.x }).count, 2, "a drag moves: \(drag)")

        XCTAssertTrue(
            perform(
                { b.scroll(byDeltaX: 0, deltaY: 60) },
                until: { $0.hasPrefix("mouse 0 0 b0 w") && !$0.hasPrefix("mouse 0 0 b0 w0 ") }),
            "scroll: \(lines)")
        XCTAssertTrue(
            perform(
                { b.scroll(byDeltaX: 40, deltaY: 0) },
                until: { $0.hasPrefix("mouse 0 0 b0 w0 h") && !$0.hasSuffix(" h0") }),
            "scroll: \(lines)")

        // The wheel goes on the guest's mouse with no buttons of its own, and
        // the system pointer is never also a touch of the touchscreen.
        XCTAssertTrue(mouseReports.allSatisfy { $0.buttons == 0 && $0.dx == 0 && $0.dy == 0 })
        // Record what the synthetic scroll was, for the hardware check.
        let scrolls = mouseReports.map { "w\($0.wheel) h\($0.horizontalWheel)" }
        let note = XCTAttachment(string: "scrolls for deltaY 60 then deltaX 40: \(scrolls)")
        note.lifetime = .keepAlways
        add(note)
    }

    func testUsingThePointerAsTouchKeepsTheMouseOffTheGuest() throws {
        try XCTSkipUnless(isPad, "XCUITest has no pointer on an iPhone")
        openSettings()
        let toggle = pointerSwitch()
        XCTAssertTrue(toggle.waitForExistence(timeout: 10), "Settings has no pointer row")
        XCTAssertEqual(toggle.value as? String, "0", "the setting starts off")
        // The switch is at the row's trailing end.
        toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.94, dy: 0.5)).tap()
        XCTAssertTrue(waitForValue(of: toggle, "1"), "the setting did not turn on: \(String(describing: toggle.value))")
        closeSettings()

        let window = app.windows.firstMatch
        let a = window.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.3))
        let b = window.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.5))
        // The click is the touchscreen's: a finger down on the guest, and no
        // mouse. (Without the touch the absence of the mouse proves nothing.)
        XCTAssertTrue(
            perform({ a.hover(); b.hover(); b.click() }, until: { $0.hasPrefix("touch ") && $0.hasSuffix(" down") }),
            "the click was not a touch: \(lines)")
        sleep(2)
        XCTAssertFalse(lines.contains { $0.hasPrefix("connect mouse") || $0.hasPrefix("pointer ") }, "log: \(lines)")

        openSettings()
        let again = pointerSwitch()
        XCTAssertTrue(again.exists, "Settings has no pointer row the second time")
        again.coordinate(withNormalizedOffset: CGVector(dx: 0.94, dy: 0.5)).tap()
        XCTAssertTrue(waitForValue(of: again, "0"), "the setting did not turn off: \(String(describing: again.value))")
        closeSettings()
        XCTAssertTrue(
            perform({ a.hover(); b.hover() }, until: { $0 == "connect mouse 1" }), "the mouse did not return: \(lines)")
        // The click is the mouse's now, and no longer a finger.
        let touchesBefore = lines.filter { $0.hasPrefix("touch ") }.count
        XCTAssertTrue(
            perform({ b.click() }, until: { $0.hasPrefix("pointer ") && $0.contains(" b1 ") }),
            "the click was not the mouse's: \(lines)")
        XCTAssertEqual(
            lines.filter { $0.hasPrefix("touch ") }.count, touchesBefore, "the mouse's click was also a touch: \(lines)")
    }

    func testOnAGuestThatNeverRegistersTheMouseAClickStaysATouchAndStillPlugsTheMouse() throws {
        try XCTSkipUnless(isPad, "XCUITest has no pointer on an iPhone")
        // A kernel without the hot-plug topic follows no configuration change:
        // the mouse is plugged and never registered, and the pointer must behave
        // as it does with no mouse.
        app.terminate()
        try launch(probe: "unregistered")
        let window = app.windows.firstMatch
        let a = window.coordinate(withNormalizedOffset: CGVector(dx: 0.3, dy: 0.3))
        let b = window.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.5))
        XCTAssertTrue(
            perform({ a.hover(); b.hover(); b.click() }, until: { $0.hasPrefix("touch ") && $0.hasSuffix(" down") }),
            "the click was not a touch: \(lines)")
        // Using the mouse plugs it, the click stays a tap, and the guest
        // hears nothing from the pointer.
        XCTAssertTrue(waitForLine(containing: "connect mouse 1"), "the mouse was not plugged: \(lines)")
        b.scroll(byDeltaX: 0, deltaY: 60)
        sleep(2)
        XCTAssertFalse(lines.contains { $0.hasPrefix("pointer ") || $0.hasPrefix("mouse ") }, "log: \(lines)")
    }

    // MARK: iPhone

    func testAnIPhoneHasNoMouseSettingAndNoMouse() throws {
        try XCTSkipIf(isPad, "an iPad has the pointer setting")
        XCTAssertTrue(perform({ app.typeKey("a", modifierFlags: []) }, until: { $0 == "key 0x4 up" }), "log: \(lines)")
        openSettings()
        XCTAssertTrue(probeField().waitForExistence(timeout: 10))
        XCTAssertFalse(app.switches["pointerAsTouch"].exists, "an iPhone has no pointer to give the guest")
        closeSettings()
        XCTAssertFalse(lines.contains { $0.hasPrefix("connect mouse") })
    }
}
