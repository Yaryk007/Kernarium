// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The buttons of a game controller, by position, in the neutral vocabulary
/// the profiles translate into each family's reports. The face buttons follow
/// iOS's own: A south, B east, X west, Y north, whatever the controller prints
/// on them.
public struct GamepadButtons: OptionSet, Hashable, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    /// A, Cross.
    public static let south = GamepadButtons(rawValue: 1 << 0)
    /// B, Circle.
    public static let east = GamepadButtons(rawValue: 1 << 1)
    /// X, Square.
    public static let west = GamepadButtons(rawValue: 1 << 2)
    /// Y, Triangle.
    public static let north = GamepadButtons(rawValue: 1 << 3)
    public static let leftShoulder = GamepadButtons(rawValue: 1 << 4)
    public static let rightShoulder = GamepadButtons(rawValue: 1 << 5)
    /// The left trigger as a button: pressed far enough for iOS to say so.
    public static let leftTrigger = GamepadButtons(rawValue: 1 << 6)
    public static let rightTrigger = GamepadButtons(rawValue: 1 << 7)
    /// iOS's Options button: Create, Share or View.
    public static let select = GamepadButtons(rawValue: 1 << 8)
    /// iOS's Menu button: Options on PlayStation, Menu on Xbox.
    public static let start = GamepadButtons(rawValue: 1 << 9)
    /// iOS's Home button: the PlayStation or Xbox logo.
    public static let mode = GamepadButtons(rawValue: 1 << 10)
    public static let leftThumbstick = GamepadButtons(rawValue: 1 << 11)
    public static let rightThumbstick = GamepadButtons(rawValue: 1 << 12)
    /// iOS's Share button on an Xbox controller, which Android reads as Record.
    public static let record = GamepadButtons(rawValue: 1 << 13)

    /// Every button a standard controller has except Record.
    public static let standard: GamepadButtons = [
        .south, .east, .west, .north, .leftShoulder, .rightShoulder, .leftTrigger, .rightTrigger,
        .select, .start, .mode, .leftThumbstick, .rightThumbstick,
    ]
}

/// The direction pad, as the four directions iOS reports.
public struct GamepadDirections: OptionSet, Hashable, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }

    public static let up = GamepadDirections(rawValue: 1 << 0)
    public static let down = GamepadDirections(rawValue: 1 << 1)
    public static let left = GamepadDirections(rawValue: 1 << 2)
    public static let right = GamepadDirections(rawValue: 1 << 3)

    /// The pad as a hat switch: 0 for neutral, then 1 for up and on clockwise
    /// through 8 for up and left. Opposite directions cancel.
    public var hat: UInt8 {
        let horizontal = (contains(.right) ? 1 : 0) - (contains(.left) ? 1 : 0)
        let vertical = (contains(.down) ? 1 : 0) - (contains(.up) ? 1 : 0)
        switch (horizontal, vertical) {
        case (0, -1): return 1
        case (1, -1): return 2
        case (1, 0): return 3
        case (1, 1): return 4
        case (0, 1): return 5
        case (-1, 1): return 6
        case (-1, 0): return 7
        case (-1, -1): return 8
        default: return 0
        }
    }
}

/// A thumbstick: -1 through 1 on each axis, with y up as iOS reports it.
public struct GamepadStick: Equatable, Hashable, Sendable {
    public var x: Float
    public var y: Float

    public init(x: Float = 0, y: Float = 0) {
        self.x = Self.clamp(x)
        self.y = Self.clamp(y)
    }

    public static let centered = GamepadStick()

    fileprivate static func clamp(_ value: Float) -> Float {
        value.isNaN ? 0 : min(1, max(-1, value))
    }
}

/// A controller's battery as iOS reports it.
public struct GamepadBattery: Equatable, Hashable, Sendable {
    public enum State: Sendable { case discharging, charging, full, unknown }

    /// 0 through 1.
    public var level: Float
    public var state: State

    public init(level: Float, state: State) {
        self.level = level.isNaN ? 0 : min(1, max(0, level))
        self.state = state
    }

    /// What a controller with no battery says: full.
    public static let none = GamepadBattery(level: 1, state: .full)
}

/// Everything about a controller's inputs that Android can see, at one moment.
public struct GamepadState: Equatable, Hashable, Sendable {
    public var buttons: GamepadButtons
    public var directions: GamepadDirections
    public var leftStick: GamepadStick
    public var rightStick: GamepadStick
    /// 0 through 1.
    public var leftTrigger: Float
    public var rightTrigger: Float
    public var battery: GamepadBattery

    public init(
        buttons: GamepadButtons = [], directions: GamepadDirections = [], leftStick: GamepadStick = .centered,
        rightStick: GamepadStick = .centered, leftTrigger: Float = 0, rightTrigger: Float = 0,
        battery: GamepadBattery = .none
    ) {
        self.buttons = buttons
        self.directions = directions
        self.leftStick = leftStick
        self.rightStick = rightStick
        self.leftTrigger = leftTrigger.isNaN ? 0 : min(1, max(0, leftTrigger))
        self.rightTrigger = rightTrigger.isNaN ? 0 : min(1, max(0, rightTrigger))
        self.battery = battery
    }

    /// Nothing pressed, everything centered; the battery is kept as given.
    public static let neutral = GamepadState()

    /// This state with the controls let go: what a guest is told when the
    /// application leaves the front.
    public var released: GamepadState { GamepadState(battery: battery) }

    /// Whether a button, the direction pad or a trigger's press differs. A
    /// change in any of these is sent at once; one in an analog value or the
    /// battery only soon.
    public func digitalDiffers(from other: GamepadState) -> Bool {
        buttons != other.buttons || directions != other.directions
    }

    /// The buttons that this state holds down and `other` does not.
    public func newlyPressed(since other: GamepadState) -> GamepadButtons {
        buttons.subtracting(other.buttons)
    }
}

/// What a controller has, which a generic profile's report descriptor lists
/// and nothing more, so that Android's answer to "has this key" is true.
public struct GamepadElements: Equatable, Hashable, Sendable {
    public var buttons: GamepadButtons
    public var leftStick: Bool
    public var rightStick: Bool
    /// Analog triggers (their buttons are `buttons`).
    public var triggers: Bool
    public var directionPad: Bool

    public init(
        buttons: GamepadButtons = [], leftStick: Bool = false, rightStick: Bool = false, triggers: Bool = false,
        directionPad: Bool = false
    ) {
        self.buttons = buttons
        self.leftStick = leftStick
        self.rightStick = rightStick
        self.triggers = triggers
        self.directionPad = directionPad
    }

    /// An extended gamepad: everything but Record.
    public static let extended = GamepadElements(
        buttons: .standard, leftStick: true, rightStick: true, triggers: true, directionPad: true)
}
