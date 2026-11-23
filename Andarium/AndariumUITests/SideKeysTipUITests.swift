// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

/// The side keys with their tip up, in the real application. The launch asks
/// for the input probe with the tip left on (`ANDARIUM_INPUT_PROBE_TIP`): the
/// session opens at once with no guest behind it, and the tip's popover shows
/// beside the closed keys. UIKit's popover takes the first touch outside its
/// card to dismiss itself; the first tap on the keys has to open them all the
/// same.
@MainActor
final class SideKeysTipUITests: XCTestCase {
    func testTheFirstTapOnTheKeysOpensThemWhileTheirTipIsUp() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launchEnvironment["ANDARIUM_INPUT_PROBE_TIP"] = "1"
        app.launch()
        // The tip's title is the keys' own name; the keys are the button.
        XCTAssertTrue(app.staticTexts["Side keys"].waitForExistence(timeout: 90), "the tip did not show")
        let keys = app.buttons["Side keys"]
        XCTAssertTrue(keys.waitForExistence(timeout: 20), "the side keys are not on screen")
        // The popover is settled once its text is on screen for a moment.
        sleep(2)
        keys.tap()
        XCTAssertTrue(app.buttons["Power"].waitForExistence(timeout: 5), "the first tap did not open the side keys")
    }
}
