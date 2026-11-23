// SPDX-License-Identifier: GPL-2.0-or-later

import XCTest

/// The notice that Android has no Google apps, on purpose, in the real
/// application.
///
/// The first start: the launch gives the application a guest instance of its
/// own (`ANDARIUM_GUEST_INSTANCE`, a folder in the Simulator's container that
/// no other launch uses), so there is no virtual machine; the test sets one
/// up, starts it, finds the panel last in the boot screen's column, below its
/// first-run note, while the real runtime boots Android on the Simulator's
/// software renderer, and waits for the boot to end, which takes the screen and
/// the panel away.
@MainActor
final class NoGoogleNoticeUITests: XCTestCase {
    private let reason = "Google licenses its apps and Google Play services only to makers of certified devices."

    /// The elements whose label holds `text`, of any kind: SwiftUI combines a panel into one.
    private func labelled(_ text: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS %@", text)).firstMatch
    }

    func testAFirstStartShowsThePanelAndTheBootEndsWithoutIt() {
        continueAfterFailure = false
        let app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_GUEST_INSTANCE"] = "no-google-notice-\(UUID().uuidString.prefix(8))"
        app.launch()

        // No virtual machine yet: set one up, and start it.
        let setUp = app.buttons["Set up virtual machine"]
        XCTAssertTrue(setUp.waitForExistence(timeout: 120), "there is no Welcome screen")
        setUp.tap()
        let create = app.buttons["Create"]
        XCTAssertTrue(create.waitForExistence(timeout: 30), "the create sheet did not open")
        create.tap()
        // Home's button says "Start and set up" until the first start has happened.
        let start = app.buttons["Start and set up"]
        XCTAssertTrue(start.waitForExistence(timeout: 600), "the virtual machine was not created")
        start.tap()

        // The first start's boot screen: its own note, and below it the panel.
        let panel = labelled("No Google apps", in: app)
        XCTAssertTrue(panel.waitForExistence(timeout: 120), "the first start has no panel")
        XCTAssertTrue(panel.label.contains(reason), "the panel says \(panel.label)")
        XCTAssertTrue(
            labelled("The first-run setup may take a few minutes.", in: app).exists, "the boot's own note is not there")
        // The status line is one element with the elapsed time.
        XCTAssertTrue(labelled("First start", in: app).exists, "this is not the first start's screen")

        // Android's first boot ends: the session replaces the screen, and the panel goes with it.
        XCTAssertTrue(
            panel.waitForNonExistence(timeout: 1500), "the panel stayed: the boot did not end, or the panel outlived it")
        XCTAssertFalse(labelled("First start", in: app).exists, "the boot screen stayed")
    }
}
