// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How a controller's actuators are offered by iOS.
public enum GamepadHaptics: Equatable, Sendable {
    /// None: the guest's rumble is dropped, as a controller with a dead motor does.
    case none
    /// One engine for the whole controller (iOS's default locality only).
    case single
    /// A left and a right handle, which play the two motors.
    case handles
}

/// What a rumble report becomes in the haptics' vocabulary. A report sets the
/// strong and the weak motor to a level and holds it until the next one, so
/// each motor is a tone that plays at that level, is changed in place when
/// the level changes, and stops at level 0. The sharpness and the split are
/// starting points to tune on a controller.
enum GamepadRumble {
    /// The longest a single force feedback effect lasts (its length is a
    /// `uint16` of milliseconds), which bounds every tone. The player cuts a
    /// tone shorter (`HapticShapes.maximumToneMilliseconds`) and the session
    /// starts it again when a report arrives after that.
    static let toneMilliseconds: UInt32 = 65_535

    /// The strong motor's tone: a low rumble.
    static let strongSharpness: Float = 0.2
    /// The weak motor's tone: a high buzz.
    static let weakSharpness: Float = 0.8

    struct Tone: Equatable {
        var target: HapticTarget
        /// 0 through 1; 0 says the target stops.
        var intensity: Float
        var sharpness: Float
    }

    /// The tones of a slot's controller for the motors' levels.
    static func tones(slot: Int, haptics: GamepadHaptics, rumble: GamepadOutputEffects.Rumble) -> [Tone] {
        switch haptics {
        case .none:
            return []
        case .handles:
            return [
                Tone(
                    target: .controller(slot: slot, locality: .leftHandle), intensity: rumble.strong,
                    sharpness: strongSharpness),
                Tone(
                    target: .controller(slot: slot, locality: .rightHandle), intensity: rumble.weak,
                    sharpness: weakSharpness),
            ]
        case .single:
            // One engine plays both: the stronger level, as sharp as the two
            // motors' mix.
            let total = rumble.strong + rumble.weak
            let sharpness = total > 0
                ? (rumble.strong * strongSharpness + rumble.weak * weakSharpness) / total : strongSharpness
            return [
                Tone(
                    target: .controller(slot: slot, locality: .standard), intensity: max(rumble.strong, rumble.weak),
                    sharpness: sharpness)
            ]
        }
    }
}
