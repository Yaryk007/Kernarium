// SPDX-License-Identifier: GPL-2.0-or-later

import CoreGraphics
import Foundation
import Testing

@testable import AndariumCore

struct ResolutionChoicesTests {
    private func scales(_ nativeScale: Double) -> [Double] {
        ResolutionChoices(nativeScale: nativeScale).scales
    }

    @Test func theChoicesRunFromOnePointToNativeInHalfSteps() {
        #expect(scales(3) == [1, 1.5, 2, 2.5, 3])
        #expect(scales(2) == [1, 1.5, 2])
        #expect(scales(2.3) == [1, 1.5, 2, 2.3])
        #expect(scales(3.5) == [1, 1.5, 2, 2.5, 3, 3.5])
    }

    @Test func aStepNearNativeIsDropped() {
        // 2.5 is under a quarter from 2.61.
        #expect(scales(2.61) == [1, 1.5, 2, 2.61])
        // A step exactly a quarter below native stays.
        #expect(scales(2.25) == [1, 1.5, 2, 2.25])
        #expect(scales(1.75) == [1, 1.5, 1.75])
        #expect(scales(2.24) == [1, 1.5, 2.24])
    }

    @Test func oneIsAlwaysOfferedAboveOneAndNativeAloneBelow() {
        #expect(scales(1) == [1])
        #expect(scales(1.2) == [1, 1.2])
        #expect(scales(1.5) == [1, 1.5])
        #expect(scales(0.75) == [0.75])
        #expect(scales(0) == [1])
        #expect(scales(.nan) == [1])
        #expect(scales(-2) == [1])
    }

    @Test func theLastChoiceIsNativeAndTheOthersAreFactors() {
        let choices = ResolutionChoices(nativeScale: 2.3)
        #expect(choices.count == 4 && choices.hasChoice)
        #expect(choices.scale(at: 0) == .factor(1))
        #expect(choices.scale(at: 2) == .factor(2))
        #expect(choices.scale(at: 3) == .native)
        #expect(choices.nativeIndex == 3)
        let single = ResolutionChoices(nativeScale: 1)
        #expect(!single.hasChoice && single.scale(at: 0) == .native)
    }

    @Test func theEndsAreNamedOnlyWhereThereIsAChoice() {
        let choices = ResolutionChoices(nativeScale: 3)
        #expect(choices.end(at: 0) == .fastest)
        #expect(choices.end(at: 1) == nil && choices.end(at: 3) == nil)
        #expect(choices.end(at: 4) == .native)
        #expect(ResolutionChoices(nativeScale: 1).end(at: 0) == nil)
        #expect(ResolutionChoices(nativeScale: 0.5).end(at: 0) == nil)
        #expect(ResolutionChoices(nativeScale: 2).end(at: 1) == nil)
    }

    @Test func aNumberThatIsNoChoiceGoesToTheNearestOne() {
        let choices = ResolutionChoices(nativeScale: 3)
        #expect(choices.resolve(.factor(1.5)) == .factor(1.5))
        #expect(choices.resolve(.factor(1.7)) == .factor(1.5))
        #expect(choices.resolve(.factor(1.8)) == .factor(2))
        #expect(choices.resolve(.factor(0.4)) == .factor(1))
        // Above native, and a number equal to native, are native.
        #expect(choices.resolve(.factor(6)) == .native)
        #expect(choices.resolve(.factor(3)) == .native)
        #expect(choices.resolve(.native) == .native)
        // Another screen's choice on this one.
        let twice = ResolutionChoices(nativeScale: 2)
        #expect(twice.resolve(.factor(2.5)) == .native)
        #expect(twice.resolve(.factor(2.61)) == .native)
        #expect(ResolutionChoices(nativeScale: 2.61).resolve(.factor(2.5)) == .native)
        #expect(ResolutionChoices(nativeScale: 2.61).resolve(.factor(2.2)) == .factor(2))
    }

    @Test func theLowerChoiceWinsATie() {
        // 1.25 is as near 1 as it is 1.5.
        #expect(ResolutionChoices(nativeScale: 3).resolve(.factor(1.25)) == .factor(1))
        #expect(ResolutionChoices(nativeScale: 3).resolve(.factor(2.25)) == .factor(2))
    }

    @Test func aSingleChoiceTakesAnyScale() {
        let single = ResolutionChoices(nativeScale: 1)
        for scale in [GuestDisplayScale.native, .points, .factor(2.5), .factor(0.3)] {
            #expect(single.resolve(scale) == .native)
        }
        #expect(ResolutionChoices(nativeScale: 0.75).resolve(.points) == .native)
    }
}

/// What the build before the Resolution control stored under `DisplayOptions.storageKey`:
/// `DisplayOptions` as it was at commit 61ce7518, the last commit to touch Launch.swift while it still had the
/// Retina switch (its file is the one build 9, tag 1.0b9-beta, shipped, and the one commit c0633f13 replaced).
/// Its encoder wrote both keys whatever the value, with the plain JSONEncoder the store used.
private struct ReleasedDisplayOptions: Encodable {
    /// What the person set the Retina switch to; nil until they did.
    var chosenRetina: Bool?

    private enum CodingKeys: String, CodingKey { case retina, chosen }

    func encode(to encoder: any Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(chosenRetina ?? false, forKey: .retina)
        try values.encode(chosenRetina != nil, forKey: .chosen)
    }
}

/// The form before it (commit 5c080a09 added `chosen`): the synthesized Codable of a struct with the one field.
private struct EarlierDisplayOptions: Encodable {
    var retina: Bool
}

struct DisplayOptionsTests {
    private let gib = UInt64(1) << 30

    private func defaultScale(native: Double, limit: UInt64) -> GuestDisplayScale {
        DisplayOptions.defaultScale(memoryLimitBytes: limit, choices: .init(nativeScale: native))
    }

    /// A device with more than 4 GiB, or none to state: 320 dpi, the choice nearest 2.0.
    @Test func aPersonWhoNeverChoseGetsTheChoiceNearestTwoWhereTheLimitIsAboveFourGiB() {
        let expected: [(native: Double, scale: GuestDisplayScale)] = [
            (1.0, .native), (1.2, .native), (1.5, .native), (1.75, .native),
            // 2.0 is a choice of its own, and is native where the screen is.
            (2.0, .native), (2.25, .factor(2)), (2.3, .factor(2)), (2.61, .factor(2)), (3.0, .factor(2)),
        ]
        // A Mac sets no limit.
        for limit in [0, 4 * gib + 1, 6 * gib, 8 * gib] {
            for (native, scale) in expected {
                #expect(defaultScale(native: native, limit: limit) == scale, "native \(native), limit \(limit)")
            }
        }
    }

    /// A device held to 4 GiB or less: 240 dpi, the choice nearest 1.5.
    @Test func aLowMemoryDeviceGetsTheChoiceNearestOnePointFive() {
        let expected: [(native: Double, scale: GuestDisplayScale)] = [
            (1.0, .native), (1.2, .native),
            // 1.5 is native on a 1.5 screen.
            (1.5, .native),
            // The nearest choice of 1.75 is 1.5, and never one below it where one is above.
            (1.75, .factor(1.5)), (2.0, .factor(1.5)), (2.25, .factor(1.5)), (2.3, .factor(1.5)),
            (2.61, .factor(1.5)), (3.0, .factor(1.5)),
        ]
        // An iPad mini 6 holds the application to 2,867 MiB.
        for limit in [1, UInt64(2867) << 20, 3 * gib, 4 * gib] {
            for (native, scale) in expected {
                #expect(defaultScale(native: native, limit: limit) == scale, "native \(native), limit \(limit)")
            }
        }
        #expect(defaultScale(native: 0.75, limit: UInt64(2867) << 20) == .native)
    }

    @Test func theThresholdIsFourGiBItselfAndNoLimitIsNotLow() {
        #expect(DisplayOptions.lowMemoryAtMostBytes == 4 * gib)
        #expect(DisplayOptions.isLowMemory(4 * gib))
        #expect(!DisplayOptions.isLowMemory(4 * gib + 1))
        #expect(DisplayOptions.isLowMemory(1))
        #expect(!DisplayOptions.isLowMemory(0))
        #expect(defaultScale(native: 3, limit: 4 * gib) == .factor(1.5))
        #expect(defaultScale(native: 3, limit: 4 * gib + 1) == .factor(2))
    }

    @Test func theDefaultsAreTheDensitiesTheyAreNamedFor() {
        // The guest's density is 160 dpi to each unit of the choice.
        #expect(GuestDisplayLayout.dotsPerInch(factor: ResolutionChoices.lowMemoryDefault) == 240)
        #expect(GuestDisplayLayout.dotsPerInch(factor: ResolutionChoices.highMemoryDefault) == 320)
    }

    @Test func aChoiceIsNeverOverriddenByTheDefault() {
        let choices = ResolutionChoices(nativeScale: 3)
        let small = UInt64(2) << 30
        #expect(DisplayOptions().scale(memoryLimitBytes: 8 * gib, choices: choices) == .factor(2))
        #expect(DisplayOptions().scale(memoryLimitBytes: 0, choices: choices) == .factor(2))
        #expect(DisplayOptions().scale(memoryLimitBytes: small, choices: choices) == .factor(1.5))
        #expect(DisplayOptions(chosenScale: .points).scale(memoryLimitBytes: 8 * gib, choices: choices) == .factor(1))
        #expect(DisplayOptions(chosenScale: .native).scale(memoryLimitBytes: small, choices: choices) == .native)
        #expect(DisplayOptions(chosenScale: .native).scale(memoryLimitBytes: 8 * gib, choices: choices) == .native)
        #expect(DisplayOptions(chosenScale: .factor(2)).scale(memoryLimitBytes: 0, choices: choices) == .factor(2))
        #expect(DisplayOptions(chosenScale: .factor(1.5)).scale(memoryLimitBytes: 0, choices: choices) == .factor(1.5))
    }

    @Test func aStoredNumberThatIsNoChoiceOfThisDeviceGoesToTheNearestOne() {
        // 2.5 chosen on a 3.0 phone, restored on a 2.0 iPad.
        let kept = DisplayOptions(chosenScale: .factor(2.5))
        #expect(kept.scale(memoryLimitBytes: 0, choices: .init(nativeScale: 2)) == .native)
        #expect(kept.scale(memoryLimitBytes: 0, choices: .init(nativeScale: 3)) == .factor(2.5))
        #expect(DisplayOptions(chosenScale: .factor(1.5)).scale(memoryLimitBytes: 0, choices: .init(nativeScale: 1))
            == .native)
    }

    @Test func nativeIsStoredAsTheSymbolAndFollowsTheScreen() throws {
        let stored = try JSONEncoder().encode(DisplayOptions(chosenScale: .native))
        #expect(String(decoding: stored, as: UTF8.self) == #"{"scale":"native"}"#)
        let back = try JSONDecoder().decode(DisplayOptions.self, from: stored)
        // The same stored choice is the sharpest on whichever screen reads it.
        #expect(back.scale(memoryLimitBytes: 0, choices: .init(nativeScale: 3)) == .native)
        #expect(back.scale(memoryLimitBytes: 0, choices: .init(nativeScale: 2.3)) == .native)
        #expect(ResolutionChoices(nativeScale: 2.3).scale(at: 3) == .native)
    }

    @Test func aNumberIsStoredAsTheNumber() throws {
        let stored = try JSONEncoder().encode(DisplayOptions(chosenScale: .factor(1.5)))
        #expect(String(decoding: stored, as: UTF8.self) == #"{"scale":1.5}"#)
        #expect(String(decoding: try JSONEncoder().encode(DisplayOptions()), as: UTF8.self) == "{}")
    }

    @Test func aChosenValueSurvivesTheStoredForm() throws {
        let all = [
            DisplayOptions(), DisplayOptions(chosenScale: .native), DisplayOptions(chosenScale: .points),
            DisplayOptions(chosenScale: .factor(1.5)), DisplayOptions(chosenScale: .factor(2.61)),
        ]
        for options in all {
            let data = try JSONEncoder().encode(options)
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: data) == options)
        }
    }

    @Test func aStoredScaleThatIsNoScaleIsNoChoice() throws {
        for bad in [#"{"scale":"sharpest"}"#, #"{"scale":0}"#, #"{"scale":-1}"#, #"{"scale":true}"#, #"{"scale":null}"#] {
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: Data(bad.utf8)) == DisplayOptions(), "\(bad)")
        }
    }

    private func read(_ json: String) throws -> DisplayOptions {
        try JSONDecoder().decode(DisplayOptions.self, from: Data(json.utf8))
    }

    /// Everyone who comes from the build with the Retina switch gets the new default, whether or not they
    /// had set the switch.
    @Test func theRetinaFormTheReleasedBuildWroteIsNoChoice() throws {
        // The released encoder's own bytes, in the order it happened to write them and the other.
        let cases: [(chosenRetina: Bool?, keys: [String])] = [
            (true, [#""retina":true"#, #""chosen":true"#]),
            (false, [#""retina":false"#, #""chosen":true"#]),
            (nil, [#""retina":false"#, #""chosen":false"#]),
        ]
        for (chosenRetina, keys) in cases {
            let written = try JSONEncoder().encode(ReleasedDisplayOptions(chosenRetina: chosenRetina))
            let text = String(decoding: written, as: UTF8.self)
            for key in keys { #expect(text.contains(key), "\(text)") }
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: written) == DisplayOptions(), "\(text)")
            let sorted = JSONEncoder()
            sorted.outputFormatting = .sortedKeys
            let bytes = try sorted.encode(ReleasedDisplayOptions(chosenRetina: chosenRetina))
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: bytes) == DisplayOptions())
        }
        // Retina on with `chosen`, retina off with `chosen`, and never chosen, as literal bytes in both key orders.
        for json in [
            #"{"retina":true,"chosen":true}"#, #"{"chosen":true,"retina":true}"#,
            #"{"retina":false,"chosen":true}"#, #"{"chosen":true,"retina":false}"#,
            #"{"retina":false,"chosen":false}"#, #"{"chosen":false,"retina":false}"#,
            #"{"retina":true,"chosen":false}"#,
        ] {
            #expect(try read(json) == DisplayOptions(), "\(json)")
        }
    }

    @Test func theEarlierRetinaFormWithoutChosenIsNoChoiceToo() throws {
        for retina in [true, false] {
            let written = try JSONEncoder().encode(EarlierDisplayOptions(retina: retina))
            #expect(String(decoding: written, as: UTF8.self) == #"{"retina":\#(retina)}"#)
            #expect(try JSONDecoder().decode(DisplayOptions.self, from: written) == DisplayOptions())
        }
        // The form before it also carried a value this version ignores.
        #expect(try read(#"{"retina":true,"followWindow":false}"#) == DisplayOptions())
        #expect(try read(#"{"retina":false,"followWindow":true}"#) == DisplayOptions())
    }

    @Test func nothingStoredIsNoChoice() throws {
        #expect(try read("{}") == DisplayOptions())
        #expect(DisplayOptions().chosenScale == nil)
    }

    @Test func aPersonFromTheRetinaFormGetsTheDeviceDefaultOnBothSidesOfTheThreshold() throws {
        let choices = ResolutionChoices(nativeScale: 3)
        for json in [#"{"retina":true,"chosen":true}"#, #"{"retina":false,"chosen":true}"#, #"{"retina":false}"#] {
            let options = try read(json)
            #expect(options.scale(memoryLimitBytes: 8 * gib, choices: choices) == .factor(2), "\(json)")
            #expect(options.scale(memoryLimitBytes: 0, choices: choices) == .factor(2), "\(json)")
            #expect(options.scale(memoryLimitBytes: 2 * gib, choices: choices) == .factor(1.5), "\(json)")
        }
    }

    /// What the Resolution control writes is the person's choice, whatever the default is.
    @Test func aStoredScaleStaysTheChoice() throws {
        let choices = ResolutionChoices(nativeScale: 3)
        let expected: [(json: String, scale: GuestDisplayScale)] = [
            (#"{"scale":"native"}"#, .native), (#"{"scale":1}"#, .points), (#"{"scale":2.5}"#, .factor(2.5)),
        ]
        for (json, scale) in expected {
            let options = try read(json)
            #expect(options == DisplayOptions(chosenScale: scale), "\(json)")
            for limit in [0, 2 * gib, 8 * gib] {
                #expect(options.scale(memoryLimitBytes: limit, choices: choices) == choices.resolve(scale), "\(json)")
            }
        }
    }

    @Test func theScaleWinsOverAnOldFormBesideIt() throws {
        let both = Data(#"{"scale":2,"retina":true,"chosen":true}"#.utf8)
        #expect(try JSONDecoder().decode(DisplayOptions.self, from: both) == DisplayOptions(chosenScale: .factor(2)))
        let off = Data(#"{"scale":"native","retina":false,"chosen":true}"#.utf8)
        #expect(try JSONDecoder().decode(DisplayOptions.self, from: off) == DisplayOptions(chosenScale: .native))
    }
}

struct DisplayScaleLaunchTests {
    private func launch(_ settings: [LaunchOverrides.Setting: String]) -> LaunchOverrides {
        LaunchOverrides(processorCount: 6) { settings[$0] }
    }

    @Test func theLaunchStatesNativeOrANumber() {
        #expect(launch([.displayScale: "native"]).displayScale == .native)
        #expect(launch([.displayScale: "Native"]).displayScale == .native)
        #expect(launch([.displayScale: "1"]).displayScale == .factor(1))
        #expect(launch([.displayScale: "1.5"]).displayScale == .factor(1.5))
        #expect(launch([.displayScale: "2.61"]).displayScale == .factor(2.61))
        #expect(launch([:]).displayScale == nil)
    }

    @Test func aValueThatIsNoScaleIsIgnored() {
        for bad in ["", "0", "-1", "sharpest", "1,5", "99", "nan"] {
            #expect(launch([.displayScale: bad]).displayScale == nil, "\(bad)")
            #expect(launch([.displayScale: bad]).isEmpty, "\(bad)")
        }
    }

    @Test func theEarlierRetinaNameIsReadWhereTheScaleIsNotStated() {
        #expect(launch([.retinaMode: "1"]).displayScale == .native)
        #expect(launch([.retinaMode: "0"]).displayScale == .points)
        #expect(launch([.retinaMode: "2"]).displayScale == nil)
        // The scale, where stated, is the one.
        #expect(launch([.displayScale: "2", .retinaMode: "1"]).displayScale == .factor(2))
        // A scale that is unreadable leaves the earlier name's value.
        #expect(launch([.displayScale: "x", .retinaMode: "0"]).displayScale == .points)
    }

    @Test func aLaunchThatStatesTheScaleIsAHarnessLaunch() {
        #expect(!launch([.displayScale: "native"]).isEmpty)
        #expect(!launch([.retinaMode: "1"]).isEmpty)
    }
}

struct SessionDisplayAreaTests {
    @Test func anIPhoneKeepsItsSensorHousingAndAnIPadGivesTheBarsRowToTheDisplay() {
        // iPhone 18 Pro: 402 x 874 pt; the island is 62 and the home indicator 34.
        let phone = SessionDisplayArea.size(
            home: CGSize(width: 402, height: 778), topInset: 62, bottomInset: 34, isPhone: true)
        #expect(phone == CGSize(width: 402, height: 778))
        // iPad Pro 11-inch in portrait: the status bar's 24 points go to the display.
        let pad = SessionDisplayArea.size(
            home: CGSize(width: 834, height: 1166), topInset: 24, bottomInset: 20, isPhone: false)
        #expect(pad == CGSize(width: 834, height: 1190))
    }

    @Test func aWindowHasNoBarToGive() {
        let window = SessionDisplayArea.size(
            home: CGSize(width: 600, height: 760), topInset: 0, bottomInset: 0, isPhone: false)
        #expect(window == CGSize(width: 600, height: 760))
    }

    @Test func aPhoneWithAHomeButtonGivesItsBarToTheDisplay() {
        let phone = SessionDisplayArea.size(
            home: CGSize(width: 375, height: 647), topInset: 20, bottomInset: 0, isPhone: true)
        #expect(phone == CGSize(width: 375, height: 667))
    }
}
