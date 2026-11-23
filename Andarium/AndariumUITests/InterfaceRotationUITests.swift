// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit
import XCTest

/// The interface in a session turns with the device on an iPad and stays
/// portrait on an iPhone. The launch asks for the input probe
/// (`ANDARIUM_INPUT_PROBE`): the session opens at once, with no guest behind
/// it, and the scene root, its orientation preferences and its policy are the
/// application's own. The guest's side of a turn (its display following the
/// scene) needs a guest: `RotationOverVsockTests` is that check.
///
/// XCUIDevice turns the Simulator through the four orientations. An element's
/// frame is in the interface's own space, so a window is landscape when the
/// interface is.
@MainActor
final class InterfaceRotationUITests: XCTestCase {
    private var app: XCUIApplication!

    private var isPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    override func setUp() async throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        app = XCUIApplication()
        app.launchEnvironment["ANDARIUM_INPUT_PROBE"] = "1"
        app.launch()
        XCTAssertTrue(probe.waitForExistence(timeout: 90), "the session did not open")
        // The side keys' tip may be showing: the interface turns with it too.
        sleep(2)
    }

    override func tearDown() async throws {
        XCUIDevice.shared.orientation = .portrait
    }

    private var probe: XCUIElement { app.descendants(matching: .any)["inputProbeLog"] }
    private var window: XCUIElement { app.windows.firstMatch }

    private func isLandscape(_ frame: CGRect) -> Bool { frame.width > frame.height }

    /// Waits for the window to have the shape the turn should give it, and
    /// answers its frame, which is its last one at the deadline.
    private func waitForShape(landscape: Bool, timeout: TimeInterval = 20) -> CGRect {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let frame = window.frame
            if isLandscape(frame) == landscape { return frame }
            usleep(200_000)
        }
        return window.frame
    }

    func testTheInterfaceFollowsTheDeviceOnAnIPadAndStaysPortraitOnAnIPhone() {
        let upright = window.frame
        XCTAssertFalse(isLandscape(upright), "the session did not open upright: \(upright)")
        let turns: [(String, UIDeviceOrientation)] = [
            ("landscape left", .landscapeLeft), ("portrait", .portrait), ("landscape right", .landscapeRight),
            ("portrait upside down", .portraitUpsideDown), ("portrait", .portrait),
        ]
        for (name, orientation) in turns {
            XCUIDevice.shared.orientation = orientation
            let landscape = isPad && orientation.isLandscape
            let turned = waitForShape(landscape: landscape)
            // Let a late turn arrive: an interface that was held, or that
            // turned back, shows here.
            sleep(2)
            let settled = window.frame
            let shape = landscape ? "landscape" : "portrait"
            XCTAssertEqual(isLandscape(settled), landscape, "\(name): the window is not \(shape): \(turned), then \(settled)")
            if !landscape {
                XCTAssertEqual(settled.size.width, upright.size.width, accuracy: 1, "\(name): \(settled)")
                XCTAssertEqual(settled.size.height, upright.size.height, accuracy: 1, "\(name): \(settled)")
            } else {
                XCTAssertEqual(settled.size.width, upright.size.height, accuracy: 1, "\(name): \(settled)")
                XCTAssertEqual(settled.size.height, upright.size.width, accuracy: 1, "\(name): \(settled)")
            }
            // The session is still open, and laid out inside the turned window.
            XCTAssertTrue(probe.exists, "\(name): the session closed")
            XCTAssertTrue(
                settled.insetBy(dx: -1, dy: -1).contains(probe.frame),
                "\(name): the session is outside the window: \(probe.frame) in \(settled)")
            let note = XCTAttachment(string: "\(name): window \(settled), probe \(probe.frame)")
            note.name = "turn \(name)"
            note.lifetime = .keepAlways
            add(note)
        }
    }
}
