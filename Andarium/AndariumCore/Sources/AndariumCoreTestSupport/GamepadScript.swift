// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// A timed run of controllers for something that is not a person: connects,
/// states, battery levels and disconnects, played against a `GamepadSession`.
/// It is simulated input: the unit tests and the test-only probe tool use it,
/// and the application does not link this target.
package struct GamepadScript: Sendable {
    package enum Step: Sendable {
        case connect(GamepadController, GamepadState)
        case submit(id: Int, GamepadState)
        case battery(id: Int, GamepadBattery)
        case disconnect(id: Int)
        case wait(milliseconds: Int)
    }

    package var steps: [Step]

    package init(_ steps: [Step]) { self.steps = steps }

    /// Plays the steps in order against `session`; `sleep` is asked for each wait.
    package func play(
        on session: GamepadSession, sleep: (Int) -> Void = { usleep(UInt32(max(0, $0)) * 1000) }
    ) {
        for step in steps {
            switch step {
            case .connect(let controller, let state): session.connect(controller, state: state)
            case .submit(let id, let state): session.submit(state, for: id)
            case .battery(let id, let battery): session.updateBattery(battery, for: id)
            case .disconnect(let id): session.disconnect(id: id)
            case .wait(let milliseconds): sleep(milliseconds)
            }
        }
    }

    // MARK: Built-in scripts

    /// One controller of each family the guest tells apart.
    package static let controllers: [GamepadController] = [
        GamepadController(id: 1, family: .dualSense, name: "DualSense Wireless Controller", haptics: .handles),
        GamepadController(id: 2, family: .dualShock4, name: "DualShock 4", haptics: .handles),
        GamepadController(id: 3, family: .xbox, name: "Xbox Wireless Controller", haptics: .handles),
        GamepadController(id: 4, family: .generic, name: "Probe Pad", haptics: .single),
    ]

    /// The names a probe tool can ask for.
    package static let names = ["identity", "buttons", "hotplug"]

    package static func named(_ name: String) -> GamepadScript? {
        switch name {
        case "identity": identity
        case "buttons": buttons
        case "hotplug": hotPlug
        default: nil
        }
    }

    /// The four controllers connect and stay, at rest.
    package static var identity: GamepadScript {
        GamepadScript(controllers.map { .connect($0, .neutral) } + [.wait(milliseconds: 500)])
    }

    /// Every button, each stick direction, each trigger and each direction of
    /// the pad, one at a time, on every controller.
    package static var buttons: GamepadScript {
        var steps: [Step] = controllers.map { .connect($0, .neutral) }
        steps.append(.wait(milliseconds: 500))
        let singles: [GamepadState] =
            GamepadButtons.standard.elements.map { GamepadState(buttons: $0) }
            + [GamepadButtons.record].map { GamepadState(buttons: $0) }
            + [GamepadDirections.up, .down, .left, .right].map { GamepadState(directions: $0) }
            + [
                GamepadState(leftStick: GamepadStick(x: 1, y: 0)), GamepadState(leftStick: GamepadStick(x: -1, y: 0)),
                GamepadState(leftStick: GamepadStick(x: 0, y: 1)), GamepadState(leftStick: GamepadStick(x: 0, y: -1)),
                GamepadState(rightStick: GamepadStick(x: 1, y: 0)), GamepadState(rightStick: GamepadStick(x: 0, y: 1)),
                GamepadState(leftTrigger: 1), GamepadState(rightTrigger: 1),
                GamepadState(leftTrigger: 0.5), GamepadState(rightTrigger: 0.5),
            ]
        for controller in controllers {
            for state in singles {
                steps.append(.submit(id: controller.id, state))
                steps.append(.wait(milliseconds: 40))
                steps.append(.submit(id: controller.id, .neutral))
                steps.append(.wait(milliseconds: 40))
            }
        }
        return GamepadScript(steps)
    }

    /// Controllers come and go, three times over, with the third slot's
    /// controller replaced by another family each time.
    package static var hotPlug: GamepadScript {
        var steps: [Step] = [
            .connect(controllers[0], .neutral), .connect(controllers[1], .neutral), .wait(milliseconds: 300),
        ]
        for round in 0..<3 {
            let family = controllers[2 + round % 2]
            let pad = GamepadController(id: 10 + round, family: family.family, name: family.name, haptics: family.haptics)
            steps += [.connect(pad, .neutral), .wait(milliseconds: 300), .disconnect(id: pad.id), .wait(milliseconds: 300)]
        }
        steps.append(.disconnect(id: 1))
        steps.append(.wait(milliseconds: 200))
        return GamepadScript(steps)
    }
}

private extension GamepadButtons {
    /// The buttons one at a time, in bit order.
    var elements: [GamepadButtons] {
        (0..<32).map { GamepadButtons(rawValue: 1 << UInt32($0)) }.filter { contains($0) }
    }
}
