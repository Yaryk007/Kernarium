// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The scene root's update runs only when one of its inputs differs from the
/// last run's, so each one is part of the comparison and nothing else is.
struct SceneUpdateInputsTests {
    private final class Token {}
    private let scene = Token()
    private let otherScene = Token()
    private let driver = Token()
    private let otherDriver = Token()

    private func inputs(
        scene: Token? = nil, driver: Token?? = nil, size: CGSize = CGSize(width: 393, height: 852),
        screen: CGSize = CGSize(width: 393, height: 852), orientation: Int = 1,
        phase: Phase = .running(.normal)
    ) -> SceneUpdateInputs {
        SceneUpdateInputs(
            scene: ObjectIdentifier(scene ?? self.scene),
            driver: (driver ?? self.driver).map(ObjectIdentifier.init), sceneSize: size, screenSize: screen,
            interfaceOrientation: orientation, phase: phase)
    }

    @Test func inputsThatAreTheSameAreEqual() {
        #expect(inputs() == inputs())
        #expect(inputs(driver: .some(nil)) == inputs(driver: .some(nil)))
    }

    @Test func everyInputIsPartOfTheComparison() {
        let base = inputs()
        let changes: [(String, SceneUpdateInputs)] = [
            ("scene", inputs(scene: otherScene)),
            ("driver", inputs(driver: otherDriver)),
            ("no driver", inputs(driver: .some(nil))),
            ("scene width", inputs(size: CGSize(width: 320, height: 852))),
            ("scene height", inputs(size: CGSize(width: 393, height: 600))),
            ("screen width", inputs(screen: CGSize(width: 430, height: 852))),
            ("screen height", inputs(screen: CGSize(width: 393, height: 932))),
            ("interface orientation", inputs(orientation: 4)),
            ("phase", inputs(phase: .home)),
            ("mode of the phase", inputs(phase: .running(.recovery))),
        ]
        for (name, changed) in changes {
            #expect(changed != base, "\(name)")
        }
    }
}
