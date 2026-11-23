// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Which actuator a haptic score plays on: target 0 is the device's own
/// Taptic Engine, and a larger number names another actuator of the host (a
/// game controller's handle, when the application plays one). A target the
/// application has no actuator for is dropped.
public struct HapticTarget: Hashable, Sendable {
    public let rawValue: UInt8
    public init(rawValue: UInt8) { self.rawValue = rawValue }
    /// The device's own actuator.
    public static let device = HapticTarget(rawValue: 0)

    /// Where a game controller's actuators of a locality begin: the guest's
    /// vibrator never asks for these, only the gamepad session plays them.
    static let firstControllerTarget: UInt8 = 16

    /// Where on a game controller a target plays.
    public enum ControllerLocality: UInt8, Sendable, CaseIterable {
        /// iOS's default locality: both handles on most controllers, the one
        /// engine of a controller that has no handles of its own.
        case standard = 0
        case leftHandle = 1
        case rightHandle = 2
    }

    /// The actuator of a locality on the controller that holds `slot`.
    public static func controller(slot: Int, locality: ControllerLocality) -> HapticTarget {
        precondition((0..<GamepadProtocol.slotCount).contains(slot), "a controller has a slot")
        return HapticTarget(rawValue: firstControllerTarget + UInt8(slot) * 4 + locality.rawValue)
    }

    /// The controller and locality of a target, or nil for another actuator.
    public var controller: (slot: Int, locality: ControllerLocality)? {
        guard rawValue >= Self.firstControllerTarget else { return nil }
        let offset = Int(rawValue - Self.firstControllerTarget)
        guard offset / 4 < GamepadProtocol.slotCount, let locality = ControllerLocality(rawValue: UInt8(offset % 4)) else {
            return nil
        }
        return (offset / 4, locality)
    }
}

/// The composition primitives of Android's vibrator HAL
/// (`android.hardware.vibrator.CompositePrimitive`), by the number the HAL
/// sends.
public enum HapticPrimitive: UInt8, Sendable, CaseIterable {
    case noop = 0, click = 1, thud = 2, spin = 3, quickRise = 4, slowRise = 5
    case quickFall = 6, lightTick = 7, lowTick = 8
}

/// The predefined effects of Android's vibrator HAL
/// (`android.hardware.vibrator.Effect`) that the host plays, by the number the
/// HAL sends. The ringtone effects (6 through 20) are not played: the
/// framework plays its own waveform in their place.
public enum HapticEffect: UInt8, Sendable, CaseIterable {
    case click = 0, doubleClick = 1, tick = 2, thud = 3, pop = 4, heavyClick = 5
    case textureTick = 21
}

/// An effect's strength (`android.hardware.vibrator.EffectStrength`).
public enum HapticStrength: UInt8, Sendable, CaseIterable {
    case light = 0, medium = 1, strong = 2

    /// How much of the effect's intensity this strength plays.
    public var factor: Float {
        switch self {
        case .light: 0.55
        case .medium: 0.8
        case .strong: 1.0
        }
    }
}

/// One element of a haptic score: what to play and when, as the guest's
/// vibrator HAL asks for it. The element ends at `startMilliseconds +
/// durationMilliseconds` at the latest, which is the duration the HAL reports
/// to the framework; the host plays nothing past it.
public struct HapticEvent: Equatable, Sendable {
    public enum Kind: UInt8, Sendable {
        /// A buzz of the given intensity and sharpness for the duration
        /// (`IVibrator.on` and `setAmplitude`).
        case tone = 0
        /// A composition primitive (`HapticPrimitive`), `intensity` being its scale.
        case primitive = 1
        /// A predefined effect (`HapticEffect`) of a `HapticStrength`.
        case effect = 2
    }

    public var kind: Kind
    /// The primitive's or the effect's number; 0 for a tone.
    public var code: UInt8
    /// An effect's `HapticStrength`; 0 otherwise.
    public var strength: UInt8
    /// From the start of the score.
    public var startMilliseconds: UInt32
    public var durationMilliseconds: UInt32
    /// A tone's intensity, a primitive's scale; 1 for an effect.
    public var intensity: Float
    /// A tone's sharpness; 0 otherwise.
    public var sharpness: Float

    public init(
        kind: Kind, code: UInt8 = 0, strength: UInt8 = 0, startMilliseconds: UInt32 = 0,
        durationMilliseconds: UInt32, intensity: Float = 1, sharpness: Float = 0
    ) {
        self.kind = kind
        self.code = code
        self.strength = strength
        self.startMilliseconds = startMilliseconds
        self.durationMilliseconds = durationMilliseconds
        self.intensity = intensity
        self.sharpness = sharpness
    }

    public static func tone(
        durationMilliseconds: UInt32, intensity: Float = 1, sharpness: Float = 0.5, startMilliseconds: UInt32 = 0
    ) -> HapticEvent {
        HapticEvent(
            kind: .tone, startMilliseconds: startMilliseconds, durationMilliseconds: durationMilliseconds,
            intensity: intensity, sharpness: sharpness)
    }

    public static func primitive(
        _ primitive: HapticPrimitive, scale: Float = 1, startMilliseconds: UInt32 = 0, durationMilliseconds: UInt32
    ) -> HapticEvent {
        HapticEvent(
            kind: .primitive, code: primitive.rawValue, startMilliseconds: startMilliseconds,
            durationMilliseconds: durationMilliseconds, intensity: scale)
    }

    public static func effect(
        _ effect: HapticEffect, strength: HapticStrength = .medium, startMilliseconds: UInt32 = 0,
        durationMilliseconds: UInt32
    ) -> HapticEvent {
        HapticEvent(
            kind: .effect, code: effect.rawValue, strength: strength.rawValue, startMilliseconds: startMilliseconds,
            durationMilliseconds: durationMilliseconds)
    }

    /// When the element ends, from the start of the score.
    public var endMilliseconds: UInt64 { UInt64(startMilliseconds) + UInt64(durationMilliseconds) }
}

/// What the guest asked a target to play (a `PLAY` frame): the elements in
/// time, replacing whatever the target played before.
public struct HapticScore: Equatable, Sendable {
    public var target: HapticTarget
    /// The guest's number for the frame, which the acknowledgement repeats.
    public var sequence: UInt32
    public var events: [HapticEvent]

    public init(target: HapticTarget = .device, sequence: UInt32, events: [HapticEvent]) {
        self.target = target
        self.sequence = sequence
        self.events = events
    }

    /// When the last element ends, from the start of the score.
    public var endMilliseconds: UInt64 { events.map(\.endMilliseconds).max() ?? 0 }

    /// Whether a tone is playing `elapsedMilliseconds` after the score began.
    public func playsTone(atMilliseconds elapsedMilliseconds: UInt64) -> Bool {
        events.contains {
            $0.kind == .tone && UInt64($0.startMilliseconds) <= elapsedMilliseconds
                && elapsedMilliseconds < $0.endMilliseconds
        }
    }
}
