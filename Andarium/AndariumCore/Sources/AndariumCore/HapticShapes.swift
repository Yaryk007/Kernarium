// SPDX-License-Identifier: GPL-2.0-or-later

import CoreHaptics
import Foundation

/// What the host plays for one element of a score, in Core Haptics' own terms
/// and independent of the engine: events with their intensity and sharpness,
/// and the ramps a continuous event follows. `HapticShapes` decides it from the
/// element, and `makePattern()` turns it into the pattern a player plays; the
/// pattern builds without an engine, so every shape is tested on any Mac.
///
/// A shape belongs to one pattern and one player. A ramp is a parameter curve,
/// which Core Haptics applies to every event of its pattern, so an element
/// with a ramp stands alone in its pattern.
public struct HapticShape: Equatable, Sendable {
    public struct Point: Equatable, Sendable {
        /// Seconds from the start of the piece.
        public var time: Double
        public var value: Float
        public init(time: Double, value: Float) {
            self.time = time
            self.value = value
        }
    }

    /// One Core Haptics event.
    public struct Piece: Equatable, Sendable {
        public enum Kind: Sendable { case transient, continuous }

        public var kind: Kind
        /// Seconds from the start of the element.
        public var start: Double
        /// Seconds; 0 for a transient.
        public var duration: Double
        public var intensity: Float
        public var sharpness: Float
        /// Whether a continuous event holds for its duration. One that does not
        /// decays after its attack, over `decay` (Core Haptics' 0 through 1).
        public var sustained: Bool
        public var decay: Float
        /// A multiplier of the event's intensity over time (0 through 1).
        public var intensityRamp: [Point]
        /// An offset of the event's sharpness over time (-1 through 1).
        public var sharpnessRamp: [Point]

        public init(
            kind: Kind, start: Double = 0, duration: Double = 0, intensity: Float, sharpness: Float,
            sustained: Bool = true, decay: Float = 0, intensityRamp: [Point] = [], sharpnessRamp: [Point] = []
        ) {
            self.kind = kind
            self.start = start
            self.duration = duration
            self.intensity = intensity
            self.sharpness = sharpness
            self.sustained = sustained
            self.decay = decay
            self.intensityRamp = intensityRamp
            self.sharpnessRamp = sharpnessRamp
        }

        /// When the piece ends, in seconds from the start of the element.
        public var end: Double { start + duration }
    }

    public var pieces: [Piece]
    /// The intensity a tone starts at, applied as the player's dynamic intensity
    /// (the event itself is at full intensity so that `INTENSITY` frames can
    /// raise it again). Nil for everything but a tone.
    public var toneIntensity: Float?

    public init(pieces: [Piece], toneIntensity: Float? = nil) {
        self.pieces = pieces
        self.toneIntensity = toneIntensity
    }

    /// When the last piece ends, in seconds from the start of the element.
    public var end: Double { pieces.map(\.end).max() ?? 0 }
}

/// How each element of a score feels. The table is the host's: the wire
/// carries which effect or primitive the guest asked for and its duration, and
/// this decides what fills the duration, so the feel is tuned in an app update
/// and never needs a new Android image. The values are starting points to
/// tune on a device.
public enum HapticShapes {
    /// Core Haptics plays a continuous event for at most 30 seconds; a tone
    /// the guest holds for longer plays this long.
    public static let maximumToneMilliseconds: UInt32 = 30_000

    /// The shape of `event`, or nil when it makes no haptic (a delay, an
    /// effect the framework plays itself, a continuous element with no
    /// duration).
    public static func shape(for event: HapticEvent) -> HapticShape? {
        let duration = Double(event.durationMilliseconds) / 1000
        switch event.kind {
        case .tone:
            guard event.durationMilliseconds > 0 else { return nil }
            let held = Double(min(event.durationMilliseconds, maximumToneMilliseconds)) / 1000
            return HapticShape(
                pieces: [.init(kind: .continuous, duration: held, intensity: 1, sharpness: clamp(event.sharpness))],
                toneIntensity: clamp(event.intensity))
        case .primitive:
            guard let primitive = HapticPrimitive(rawValue: event.code) else { return nil }
            return shape(of: primitive, scale: clamp(event.intensity), duration: duration)
        case .effect:
            guard let effect = HapticEffect(rawValue: event.code) else { return nil }
            let strength = HapticStrength(rawValue: event.strength) ?? .medium
            return shape(of: effect, factor: strength.factor, duration: duration)
        }
    }

    private static func shape(of primitive: HapticPrimitive, scale: Float, duration: Double) -> HapticShape? {
        switch primitive {
        case .noop:
            return nil
        case .click:
            return transient(0.9 * scale, 0.65)
        case .lightTick:
            return transient(0.5 * scale, 0.9)
        case .lowTick:
            return transient(0.6 * scale, 0.2)
        case .thud:
            return thud(scale, duration: duration)
        case .spin:
            return ramped(
                duration: duration, intensity: scale, intensityRamp: [(0, 0), (0.5, 1), (1, 0)], sharpness: (0.3, 0.7))
        case .quickRise:
            return ramped(duration: duration, intensity: scale, intensityRamp: [(0, 0), (1, 1)], sharpness: (0.4, 0.8))
        case .slowRise:
            return ramped(duration: duration, intensity: scale, intensityRamp: [(0, 0), (1, 1)], sharpness: (0.3, 0.5))
        case .quickFall:
            return ramped(duration: duration, intensity: scale, intensityRamp: [(0, 1), (1, 0)], sharpness: (0.8, 0.4))
        }
    }

    private static func shape(of effect: HapticEffect, factor: Float, duration: Double) -> HapticShape? {
        switch effect {
        case .click:
            return transient(0.9 * factor, 0.65)
        case .tick:
            return transient(0.55 * factor, 0.9)
        case .textureTick:
            return transient(0.35 * factor, 1.0)
        case .heavyClick:
            return transient(1.0 * factor, 0.45)
        case .pop:
            return transient(0.8 * factor, 0.85)
        case .doubleClick:
            return HapticShape(pieces: [
                .init(kind: .transient, start: 0, intensity: 0.9 * factor, sharpness: 0.65),
                .init(kind: .transient, start: 0.1, intensity: 0.9 * factor, sharpness: 0.65),
            ])
        case .thud:
            return thud(factor, duration: duration)
        }
    }

    private static func transient(_ intensity: Float, _ sharpness: Float) -> HapticShape {
        HapticShape(pieces: [.init(kind: .transient, intensity: clamp(intensity), sharpness: sharpness)])
    }

    private static func thud(_ intensity: Float, duration: Double) -> HapticShape? {
        guard duration > 0 else { return nil }
        return HapticShape(pieces: [
            .init(
                kind: .continuous, duration: duration, intensity: intensity, sharpness: 0.1, sustained: false,
                decay: 0.3)
        ])
    }

    /// A continuous event whose intensity follows `intensityRamp` (fractions of
    /// the duration and multipliers of `intensity`) and whose sharpness moves
    /// from one value to another.
    private static func ramped(
        duration: Double, intensity: Float, intensityRamp: [(Double, Float)], sharpness: (Float, Float)
    ) -> HapticShape? {
        guard duration > 0 else { return nil }
        return HapticShape(pieces: [
            .init(
                kind: .continuous, duration: duration, intensity: intensity, sharpness: sharpness.0,
                intensityRamp: intensityRamp.map { .init(time: $0.0 * duration, value: $0.1) },
                sharpnessRamp: [
                    .init(time: 0, value: 0), .init(time: duration, value: sharpness.1 - sharpness.0),
                ])
        ])
    }

    private static func clamp(_ value: Float) -> Float { min(max(value, 0), 1) }
}

extension HapticShape {
    /// The Core Haptics pattern that plays the shape. It needs no engine to
    /// build; it throws what Core Haptics throws for a value it refuses.
    public func makePattern() throws -> CHHapticPattern {
        var events: [CHHapticEvent] = []
        var curves: [CHHapticParameterCurve] = []
        for piece in pieces {
            var parameters = [
                CHHapticEventParameter(parameterID: .hapticIntensity, value: piece.intensity),
                CHHapticEventParameter(parameterID: .hapticSharpness, value: piece.sharpness),
            ]
            switch piece.kind {
            case .transient:
                events.append(CHHapticEvent(eventType: .hapticTransient, parameters: parameters, relativeTime: piece.start))
            case .continuous:
                if !piece.sustained {
                    parameters.append(CHHapticEventParameter(parameterID: .sustained, value: 0))
                    parameters.append(CHHapticEventParameter(parameterID: .decayTime, value: piece.decay))
                }
                events.append(
                    CHHapticEvent(
                        eventType: .hapticContinuous, parameters: parameters, relativeTime: piece.start,
                        duration: piece.duration))
            }
            if !piece.intensityRamp.isEmpty {
                curves.append(Self.curve(.hapticIntensityControl, piece.intensityRamp, start: piece.start))
            }
            if !piece.sharpnessRamp.isEmpty {
                curves.append(Self.curve(.hapticSharpnessControl, piece.sharpnessRamp, start: piece.start))
            }
        }
        return try CHHapticPattern(events: events, parameterCurves: curves)
    }

    private static func curve(
        _ parameter: CHHapticDynamicParameter.ID, _ points: [Point], start: Double
    ) -> CHHapticParameterCurve {
        CHHapticParameterCurve(
            parameterID: parameter,
            controlPoints: points.map { .init(relativeTime: $0.time, value: $0.value) },
            relativeTime: start)
    }
}
