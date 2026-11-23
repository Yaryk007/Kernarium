// SPDX-License-Identifier: GPL-2.0-or-later

import CoreHaptics
import Foundation
import Testing
@testable import AndariumCore

/// How each element of a score feels, against the design's table, and that
/// every shape is bounded by the duration the guest reported and builds a
/// Core Haptics pattern (which needs no engine, so a Mac can check it).
struct HapticShapesTests {
    /// The durations the guest's HAL reports, which are its timing contract
    /// (`getPrimitiveDuration`, `perform`).
    private static let primitiveDurations: [HapticPrimitive: UInt32] = [
        .noop: 0, .click: 12, .thud: 60, .spin: 140, .quickRise: 120, .slowRise: 400, .quickFall: 80,
        .lightTick: 8, .lowTick: 12,
    ]
    private static let effectDurations: [HapticEffect: UInt32] = [
        .click: 12, .doubleClick: 112, .tick: 8, .thud: 60, .pop: 12, .heavyClick: 16, .textureTick: 8,
    ]

    private func primitiveShape(_ primitive: HapticPrimitive, scale: Float = 1) -> HapticShape? {
        HapticShapes.shape(
            for: .primitive(primitive, scale: scale, durationMilliseconds: Self.primitiveDurations[primitive]!))
    }

    private func effectShape(_ effect: HapticEffect, _ strength: HapticStrength = .medium) -> HapticShape? {
        HapticShapes.shape(
            for: .effect(effect, strength: strength, durationMilliseconds: Self.effectDurations[effect]!))
    }

    private func transient(_ shape: HapticShape?) -> HapticShape.Piece? {
        guard let shape, shape.pieces.count == 1, shape.pieces[0].kind == .transient else { return nil }
        return shape.pieces[0]
    }

    // MARK: The table

    @Test func theTransientPrimitivesAreTaps() {
        let click = transient(primitiveShape(.click))
        #expect(click?.intensity == 0.9 && click?.sharpness == 0.65)
        let light = transient(primitiveShape(.lightTick))
        #expect(light?.intensity == 0.5 && light?.sharpness == 0.9)
        let low = transient(primitiveShape(.lowTick))
        #expect(low?.intensity == 0.6 && low?.sharpness == 0.2)
    }

    @Test func aPrimitivesScaleScalesItsIntensity() {
        #expect(transient(primitiveShape(.click, scale: 0.5))?.intensity == 0.45)
        #expect(transient(primitiveShape(.click, scale: 0))?.intensity == 0)
        // A scale beyond 1 is held to it.
        #expect(transient(primitiveShape(.click, scale: 7))?.intensity == 0.9)
    }

    @Test func theThudDecaysInsteadOfHolding() throws {
        let piece = try #require(primitiveShape(.thud, scale: 0.5)?.pieces.first)
        #expect(piece.kind == .continuous && piece.duration == 0.06)
        #expect(piece.intensity == 0.5 && piece.sharpness == 0.1)
        #expect(!piece.sustained && piece.decay > 0)
    }

    @Test func theRampsRiseFallAndTurn() throws {
        let rise = try #require(primitiveShape(.quickRise)?.pieces.first)
        #expect(rise.duration == 0.12 && rise.intensity == 1)
        #expect(rise.intensityRamp == [.init(time: 0, value: 0), .init(time: 0.12, value: 1)])
        #expect(rise.sharpness == 0.4)
        #expect(rise.sharpnessRamp.last?.value.isApproximately(0.4) == true)

        let slow = try #require(primitiveShape(.slowRise)?.pieces.first)
        #expect(slow.duration == 0.4 && slow.sharpness == 0.3)
        #expect(slow.sharpnessRamp.last?.value.isApproximately(0.2) == true)

        let fall = try #require(primitiveShape(.quickFall)?.pieces.first)
        #expect(fall.intensityRamp == [.init(time: 0, value: 1), .init(time: 0.08, value: 0)])
        #expect(fall.sharpness == 0.8)
        #expect(fall.sharpnessRamp.last?.value.isApproximately(-0.4) == true)

        let spin = try #require(primitiveShape(.spin, scale: 0.5)?.pieces.first)
        #expect(spin.intensity == 0.5 && spin.duration == 0.14)
        #expect(spin.intensityRamp.map(\.value) == [0, 1, 0])
        #expect(spin.intensityRamp.map(\.time).map { ($0 * 1000).rounded() } == [0, 70, 140])
    }

    @Test func aNoopAndTheEffectsTheFrameworkPlaysItselfMakeNothing() {
        #expect(primitiveShape(.noop) == nil)
        for ringtone in 6...20 {
            #expect(HapticShapes.shape(for: HapticEvent(kind: .effect, code: UInt8(ringtone), durationMilliseconds: 100)) == nil)
        }
        // A number the table does not have is silence, never a crash.
        #expect(HapticShapes.shape(for: HapticEvent(kind: .primitive, code: 200, durationMilliseconds: 50)) == nil)
        #expect(HapticShapes.shape(for: HapticEvent(kind: .effect, code: 99, durationMilliseconds: 50)) == nil)
        // A continuous element with no duration has nothing to fill.
        #expect(HapticShapes.shape(for: .primitive(.thud, durationMilliseconds: 0)) == nil)
        #expect(HapticShapes.shape(for: .primitive(.spin, durationMilliseconds: 0)) == nil)
        #expect(HapticShapes.shape(for: .tone(durationMilliseconds: 0)) == nil)
    }

    @Test func theEffectsAreTapsThatTheStrengthScales() {
        let expectations: [(HapticEffect, Float, Float)] = [
            (.click, 0.9, 0.65), (.tick, 0.55, 0.9), (.textureTick, 0.35, 1.0), (.heavyClick, 1.0, 0.45),
            (.pop, 0.8, 0.85),
        ]
        for (effect, intensity, sharpness) in expectations {
            for strength in HapticStrength.allCases {
                let piece = transient(effectShape(effect, strength))
                #expect(piece?.intensity.isApproximately(intensity * strength.factor) == true, "\(effect) \(strength)")
                #expect(piece?.sharpness == sharpness, "\(effect)")
            }
        }
        #expect(HapticStrength.light.factor == 0.55 && HapticStrength.medium.factor == 0.8)
        #expect(HapticStrength.strong.factor == 1)
    }

    @Test func aStrengthTheTableDoesNotKnowPlaysAsMedium() {
        let odd = HapticEvent(kind: .effect, code: HapticEffect.click.rawValue, strength: 9, durationMilliseconds: 12)
        #expect(HapticShapes.shape(for: odd)?.pieces.first?.intensity.isApproximately(0.9 * 0.8) == true)
    }

    @Test func aDoubleClickIsTwoTapsAHundredMillisecondsApart() throws {
        let pieces = try #require(effectShape(.doubleClick, .strong)?.pieces)
        #expect(pieces.map(\.kind) == [.transient, .transient])
        #expect(pieces.map(\.start) == [0, 0.1])
        #expect(pieces.allSatisfy { $0.intensity.isApproximately(0.9) && $0.sharpness == 0.65 })
    }

    @Test func theThudEffectIsTheThudAtTheEffectsStrength() throws {
        let strong = try #require(effectShape(.thud, .strong)?.pieces.first)
        #expect(strong.intensity == 1 && strong.duration == 0.06 && !strong.sustained)
        let light = try #require(effectShape(.thud, .light)?.pieces.first)
        #expect(light.intensity == 0.55)
    }

    // MARK: Tones

    @Test func aToneHoldsAtFullIntensityAndCarriesItsLevelAsADynamicIntensity() throws {
        let tone = try #require(HapticShapes.shape(for: .tone(durationMilliseconds: 500, intensity: 0.4, sharpness: 0.5)))
        #expect(tone.toneIntensity == 0.4)
        let piece = try #require(tone.pieces.first)
        #expect(tone.pieces.count == 1 && piece.kind == .continuous && piece.sustained)
        #expect(piece.intensity == 1 && piece.sharpness == 0.5 && piece.duration == 0.5)
        #expect(piece.intensityRamp.isEmpty && piece.sharpnessRamp.isEmpty)
    }

    @Test func aToneThatTheGuestHoldsForLongerThanCoreHapticsAllowsIsCut() throws {
        let long = try #require(HapticShapes.shape(for: .tone(durationMilliseconds: 600_000)))
        #expect(long.end == 30)
        let exact = try #require(HapticShapes.shape(for: .tone(durationMilliseconds: 30_000)))
        #expect(exact.end == 30)
        let short = try #require(HapticShapes.shape(for: .tone(durationMilliseconds: 29_999)))
        #expect(short.end == 29.999)
    }

    @Test func onlyATonePlaysAtADynamicIntensity() {
        #expect(primitiveShape(.click)?.toneIntensity == nil)
        #expect(primitiveShape(.spin)?.toneIntensity == nil)
        #expect(effectShape(.doubleClick)?.toneIntensity == nil)
    }

    // MARK: Every shape is bounded and builds

    /// Every element a HAL can ask for, at every scale and strength.
    private func everyElement() -> [HapticEvent] {
        var events: [HapticEvent] = []
        for (primitive, duration) in Self.primitiveDurations {
            for scale in [Float(0), 0.01, 0.5, 1] {
                events.append(.primitive(primitive, scale: scale, durationMilliseconds: duration))
            }
        }
        for (effect, duration) in Self.effectDurations {
            for strength in HapticStrength.allCases { events.append(.effect(effect, strength: strength, durationMilliseconds: duration)) }
        }
        for duration: UInt32 in [1, 10, 500, 29_999, 30_000, 31_000, UInt32.max] {
            for intensity in [Float(0.01), 0.5, 1] {
                for sharpness in [Float(0), 0.5, 1] {
                    events.append(.tone(durationMilliseconds: duration, intensity: intensity, sharpness: sharpness))
                }
            }
        }
        return events
    }

    @Test func noShapePlaysPastTheDurationTheGuestReported() throws {
        var checked = 0
        for event in everyElement() {
            guard let shape = HapticShapes.shape(for: event) else { continue }
            checked += 1
            let reported = Double(event.durationMilliseconds) / 1000
            let limit = event.kind == .tone ? min(reported, 30) : reported
            for piece in shape.pieces {
                #expect(piece.start >= 0 && piece.start <= reported + 1e-9, "\(event)")
                if piece.kind == .continuous {
                    #expect(piece.duration > 0 && piece.end <= limit + 1e-9, "\(event)")
                } else {
                    #expect(piece.duration == 0, "\(event)")
                }
                // The ramps stay inside the piece.
                for point in piece.intensityRamp + piece.sharpnessRamp {
                    #expect(point.time >= 0 && point.time <= piece.duration + 1e-9, "\(event)")
                }
            }
        }
        #expect(checked > 100)
    }

    @Test func everyShapeKeepsItsValuesInCoreHapticsRanges() {
        for event in everyElement() {
            guard let shape = HapticShapes.shape(for: event) else { continue }
            for piece in shape.pieces {
                #expect((0...1).contains(piece.intensity) && (0...1).contains(piece.sharpness), "\(event)")
                #expect((0...1).contains(piece.decay), "\(event)")
                #expect(piece.intensityRamp.allSatisfy { (0...1).contains($0.value) }, "\(event)")
                #expect(piece.sharpnessRamp.allSatisfy { (-1...1).contains($0.value) }, "\(event)")
            }
            if let level = shape.toneIntensity { #expect((0...1).contains(level), "\(event)") }
        }
    }

    @Test func everyShapeBuildsACoreHapticsPatternWithoutAnEngine() throws {
        var built = 0
        for event in everyElement() {
            guard let shape = HapticShapes.shape(for: event) else { continue }
            let pattern = try shape.makePattern()
            built += 1
            #expect(pattern.duration <= shape.end + 0.05, "\(event)")
        }
        #expect(built > 100)
    }

    @Test func aPatternHasTheShapesEventsAndCurves() throws {
        let spin = try #require(primitiveShape(.spin))
        let pattern = try spin.makePattern()
        // The same through the pattern's own dictionary form.
        let dictionary = try pattern.exportDictionary()
        let list = try #require(dictionary[.pattern] as? [[CHHapticPattern.Key: Any]])
        let events = list.filter { $0[.event] != nil }
        let curves = list.filter { $0[.parameterCurve] != nil }
        #expect(events.count == 1)
        #expect(curves.count == 2)

        let double = try #require(effectShape(.doubleClick)).makePattern()
        let doubleList = try #require(try double.exportDictionary()[.pattern] as? [[CHHapticPattern.Key: Any]])
        #expect(doubleList.filter { $0[.event] != nil }.count == 2)
        #expect(doubleList.filter { $0[.parameterCurve] != nil }.isEmpty)
    }
}

private extension Float {
    func isApproximately(_ other: Float) -> Bool { abs(self - other) < 1e-5 }
}
