// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// The per-family devices a guest sees: each profile's descriptor is read back
/// by a test-only HID parser and every report the encoder writes is read the
/// way the guest's kernel reads it, so the descriptor, the reports and the
/// numbers the kernel drivers expect cannot drift apart. The driver numbers
/// (bit positions, sizes, offsets) are written out here on purpose and not
/// shared with the code under test.
struct GamepadProfileTests {
    private static let all: [(String, any GamepadProfile)] = [
        ("DualSense", DualSenseProfile(edge: false)),
        ("DualSense Edge", DualSenseProfile(edge: true)),
        ("DualShock 4", DualShock4Profile()),
        ("Xbox", XboxProfile()),
        ("generic", GenericProfile(name: "Pad", elements: .extended)),
    ]

    private func encode(_ profile: any GamepadProfile, _ state: GamepadState, previous: GamepadState? = nil) -> [UInt8] {
        var encoder = profile.makeEncoder()
        return encoder.reports(for: state, after: previous)[0]
    }

    // MARK: Identity

    @Test func everyProfileIsTheSameDeviceInASlotEveryTime() {
        for (name, profile) in Self.all {
            #expect(profile.create(slot: 3) == profile.create(slot: 3), "\(name)")
            #expect(profile.create(slot: 3).slot == 3)
        }
    }

    @Test func thePlayStationDevicesHaveTheIdsAndVersionTheDriverMatches() {
        let ds = DualSenseProfile(edge: false).create(slot: 0)
        let edge = DualSenseProfile(edge: true).create(slot: 0)
        let ds4 = DualShock4Profile().create(slot: 0)
        for create in [ds, edge, ds4] {
            // USB, Sony, and 0x0100 which the guest's driver ORs 0x8000 into.
            #expect(create.bus == 3)
            #expect(create.vendor == 0x054c)
            #expect(create.version == 0x0100)
            #expect(create.uniq.isEmpty)
        }
        #expect(ds.product == 0x0ce6)
        #expect(edge.product == 0x0df2)
        #expect(ds4.product == 0x09cc)
        #expect(DualSenseProfile(edge: false).family == .dualSense)
        #expect(DualSenseProfile(edge: true).family == .dualSenseEdge)
        #expect(DualShock4Profile().family == .dualShock4)
    }

    @Test func theXboxDeviceIsTheBluetoothOneWithANonAddressUniq() {
        let create = XboxProfile().create(slot: 5)
        #expect(create.bus == 5)
        #expect(create.vendor == 0x045e)
        #expect(create.product == 0x0b13)
        #expect(create.name == "Xbox Wireless Controller")
        // No Bluetooth address, so Android never looks the device up.
        #expect(create.uniq == "kernarium-gamepad-5")
        #expect(!create.uniq.contains(":"))
    }

    @Test func aGenericDeviceHasNoVendorOrProductAndTheControllersName() {
        let create = GenericProfile(name: "  Backbone One\u{0007}  ", elements: .extended).create(slot: 1)
        #expect(create.vendor == 0)
        #expect(create.product == 0)
        #expect(create.name == "Backbone One")
        #expect(GenericProfile(name: "", elements: .extended).create(slot: 0).name == "Game Controller")
        #expect(GenericProfile(name: String(repeating: "x", count: 300), elements: .extended).create(slot: 0).name.utf8.count == 127)
    }

    @Test func profilesAreChosenByFamily() {
        for family in [GamepadFamily.dualSense, .dualSenseEdge, .dualShock4, .xbox, .generic] {
            #expect(GamepadProfiles.profile(family: family, name: "x", elements: .extended).family == family)
        }
    }

    // MARK: Descriptors

    @Test func everyDescriptorParsesAndTheReportsMatchIt() throws {
        for (name, profile) in Self.all {
            let create = profile.create(slot: 0)
            let model = try HIDDescriptorModel(create.descriptor)
            #expect(model.collections >= 1, "\(name)")
            let inputSize = try #require(model.reportSize(.input, id: 1), "\(name)")
            #expect(encode(profile, .neutral).count == inputSize, "\(name)")
        }
    }

    @Test func dualSenseReportsHaveTheSizesTheDriverChecks() throws {
        let profile = DualSenseProfile(edge: false)
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        #expect(model.reportSize(.input, id: 1) == 64)
        #expect(model.reportSize(.output, id: 2) == 63)
        #expect(model.reportSize(.feature, id: 0x05) == 41)
        #expect(model.reportSize(.feature, id: 0x09) == 20)
        #expect(model.reportSize(.feature, id: 0x20) == 64)
        #expect(profile.featureReport(number: 0x05, slot: 0)?.count == 41)
        #expect(profile.featureReport(number: 0x09, slot: 0)?.count == 20)
        #expect(profile.featureReport(number: 0x20, slot: 0)?.count == 64)
        #expect(profile.featureReport(number: 0x02, slot: 0) == nil)
    }

    @Test func dualShock4ReportsHaveTheSizesTheDriverChecks() throws {
        let profile = DualShock4Profile()
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        #expect(model.reportSize(.input, id: 1) == 64)
        #expect(model.reportSize(.output, id: 5) == 32)
        #expect(model.reportSize(.feature, id: 0x02) == 37)
        #expect(model.reportSize(.feature, id: 0x12) == 16)
        #expect(model.reportSize(.feature, id: 0xa3) == 49)
        #expect(profile.featureReport(number: 0x02, slot: 0)?.count == 37)
        #expect(profile.featureReport(number: 0x12, slot: 0)?.count == 16)
        #expect(profile.featureReport(number: 0xa3, slot: 0)?.count == 49)
        #expect(profile.featureReport(number: 0x05, slot: 0) == nil)
    }

    @Test func aFeatureReportStartsWithItsOwnNumber() {
        for (name, profile) in Self.all {
            for number in 0...255 {
                guard let report = profile.featureReport(number: UInt8(number), slot: 2) else { continue }
                #expect(report.first == UInt8(number), "\(name) feature \(number)")
            }
        }
    }

    @Test func aMalformedDescriptorIsRefusedByTheTestParser() {
        // A collection that is never closed, and an item that runs off the end.
        #expect(throws: HIDDescriptorModel.Invalid.self) { _ = try HIDDescriptorModel([0x05, 0x01, 0x09, 0x05, 0xa1, 0x01]) }
        #expect(throws: HIDDescriptorModel.Invalid.self) { _ = try HIDDescriptorModel([0x06, 0x00]) }
    }

    // MARK: Pairing addresses

    @Test func eachSlotsPairingAddressIsLocallyAdministeredAndDifferent() throws {
        var seen = Set<[UInt8]>()
        for profile in [DualSenseProfile(edge: false) as any GamepadProfile, DualShock4Profile()] {
            let number: UInt8 = profile.family == .dualShock4 ? 0x12 : 0x09
            for slot in 0..<8 {
                let report = try #require(profile.featureReport(number: number, slot: slot))
                let address = Array(report[1..<7])
                // Little endian: the first octet is the last byte. Bit 1 is
                // locally administered and bit 0 clear is unicast.
                #expect(address[5] & 0x02 != 0)
                #expect(address[5] & 0x01 == 0)
                seen.insert(address + [UInt8(profile.family == .dualShock4 ? 1 : 0)])
            }
        }
        #expect(seen.count == 16)
    }

    @Test func theFirmwareReportMakesTheDriverRumbleTheWayTheConsoleDoes() throws {
        let report = try #require(DualSenseProfile(edge: false).featureReport(number: 0x20, slot: 0))
        // update_version at offset 44 (little endian) is 0x0215: 2.21.
        #expect(Int(report[44]) | Int(report[45]) << 8 >= 0x0224 - 0x0f)
        #expect(Int(report[44]) | Int(report[45]) << 8 >= 0x0215)
    }

    // MARK: PlayStation input reports

    private func dualSense(_ state: GamepadState) -> [UInt8] { encode(DualSenseProfile(edge: false), state) }
    private func dualShock(_ state: GamepadState) -> [UInt8] { encode(DualShock4Profile(), state) }

    @Test func aNeutralDualSenseReportIsCenteredWithNothingPressed() {
        let report = dualSense(.neutral)
        #expect(report[0] == 1)
        #expect(Array(report[1...4]) == [128, 128, 128, 128])
        #expect(Array(report[5...6]) == [0, 0])
        // Hat neutral is 8, and no button.
        #expect(Array(report[8...10]) == [8, 0, 0])
    }

    @Test func aDualSenseReportCarriesTheSticksTriggersAndTheYAxisDown() {
        let state = GamepadState(
            leftStick: GamepadStick(x: 1, y: 1), rightStick: GamepadStick(x: -1, y: -1), leftTrigger: 1, rightTrigger: 0.5)
        let report = dualSense(state)
        #expect(report[1] == 255)  // Left x
        #expect(report[2] == 0)  // Left y: iOS up is the report's 0
        #expect(report[3] == 0)  // Right x
        #expect(report[4] == 255)  // Right y: iOS down is the report's 255
        #expect(report[5] == 255)
        #expect(report[6] == 128)
    }

    @Test func eachButtonSetsTheBitTheDriverReadsInBothPlayStationGenerations() {
        // The bit each button has in the three button bytes of the DualSense
        // (bytes 8 through 10) and the DualShock 4 (5 through 7).
        let expected: [(GamepadButtons, Int, UInt8)] = [
            (.west, 0, 0x10), (.south, 0, 0x20), (.east, 0, 0x40), (.north, 0, 0x80),
            (.leftShoulder, 1, 0x01), (.rightShoulder, 1, 0x02), (.leftTrigger, 1, 0x04), (.rightTrigger, 1, 0x08),
            (.select, 1, 0x10), (.start, 1, 0x20), (.leftThumbstick, 1, 0x40), (.rightThumbstick, 1, 0x80),
            (.mode, 2, 0x01),
        ]
        for (button, byte, bit) in expected {
            var neutralBytes: [UInt8] = [8, 0, 0]
            neutralBytes[byte] |= bit
            #expect(Array(dualSense(GamepadState(buttons: button))[8...10]) == neutralBytes, "DualSense \(button)")
            #expect(Array(dualShock(GamepadState(buttons: button))[5...7]) == neutralBytes, "DualShock 4 \(button)")
        }
    }

    @Test func theDirectionPadIsTheHatTheDriverReads() {
        // 0 is up, then clockwise; 8 is neutral.
        let cases: [(GamepadDirections, UInt8)] = [
            ([], 8), (.up, 0), ([.up, .right], 1), (.right, 2), ([.down, .right], 3), (.down, 4),
            ([.down, .left], 5), (.left, 6), ([.up, .left], 7), ([.up, .down], 8), ([.left, .right], 8),
        ]
        for (directions, hat) in cases {
            #expect(dualSense(GamepadState(directions: directions))[8] & 0x0f == hat, "\(directions)")
            #expect(dualShock(GamepadState(directions: directions))[5] & 0x0f == hat, "\(directions)")
        }
    }

    @Test func theDualShockReportHasItsOwnOffsets() {
        let state = GamepadState(
            leftStick: GamepadStick(x: 1, y: 0), rightStick: GamepadStick(x: 0, y: 1), leftTrigger: 1, rightTrigger: 0.25)
        let report = dualShock(state)
        #expect(Array(report[1...4]) == [255, 128, 128, 0])
        #expect(report[8] == 255)
        #expect(report[9] == 64)
    }

    @Test func theDualSenseSequenceNumberCountsAndTheTouchpadIsInactive() {
        var encoder = DualSenseProfile(edge: false).makeEncoder()
        let first = encoder.reports(for: .neutral, after: nil)[0]
        let second = encoder.reports(for: .neutral, after: .neutral)[0]
        #expect(second[7] == first[7] &+ 1)
        // Both contacts have the "no finger" bit set: no phantom touch.
        #expect(first[33] & 0x80 != 0)
        #expect(first[37] & 0x80 != 0)
    }

    @Test func theDualShockTouchContactsAreInactive() {
        let report = dualShock(.neutral)
        for contact in [35, 39, 44, 48, 53, 57] { #expect(report[contact] & 0x80 != 0, "contact at \(contact)") }
    }

    @Test func theDualSenseBatteryIsInTenthsWithItsCableState() {
        func status(_ battery: GamepadBattery) -> UInt8 { dualSense(GamepadState(battery: battery))[53] }
        #expect(status(.init(level: 0.55, state: .discharging)) == 5)
        #expect(status(.init(level: 1, state: .discharging)) == 10)
        #expect(status(.init(level: 0, state: .unknown)) == 0)
        #expect(status(.init(level: 0.42, state: .charging)) == 0x10 | 4)
        // Full: the driver's "complete" state, 2, at 100%.
        #expect(status(.init(level: 1, state: .full)) == 0x20 | 10)
        #expect(status(.none) == 0x20 | 10)
    }

    @Test func theDualShockBatteryIsInTenthsAndFullIsOnACable() {
        func status(_ battery: GamepadBattery) -> UInt8 { dualShock(GamepadState(battery: battery))[30] }
        #expect(status(.init(level: 0.55, state: .discharging)) == 5)
        #expect(status(.init(level: 0.99, state: .charging)) == 0x10 | 9)
        #expect(status(.init(level: 1, state: .full)) == 0x10 | 11)
    }

    // MARK: PlayStation output reports

    private func dualSenseOutput(
        flag0: UInt8 = 0, flag1: UInt8 = 0, flag2: UInt8 = 0, right: UInt8 = 0, left: UInt8 = 0, leds: UInt8 = 0,
        color: (UInt8, UInt8, UInt8) = (0, 0, 0)
    ) -> [UInt8] {
        var report = [UInt8](repeating: 0, count: 63)
        report[0] = 2
        report[1] = flag0
        report[2] = flag1
        report[3] = right
        report[4] = left
        report[39] = flag2
        report[44] = leds
        (report[45], report[46], report[47]) = color
        return report
    }

    @Test func aDualSenseRumbleReportIsTheTwoMotorsFromEitherGenerationOfFirmware() {
        let profile = DualSenseProfile(edge: false)
        // Haptics select (bit 1) with the compatible vibration flag of the first or the second flag byte.
        let one = profile.decodeOutput(dualSenseOutput(flag0: 0x03, right: 51, left: 255))
        #expect(one.rumble == .init(strong: 1, weak: 0.2))
        let two = profile.decodeOutput(dualSenseOutput(flag0: 0x02, flag2: 0x04, right: 0, left: 102))
        #expect(two.rumble == .init(strong: 0.4, weak: 0))
        // Without the select bit, or without either enable, the report says nothing about rumble.
        #expect(profile.decodeOutput(dualSenseOutput(flag0: 0x01, right: 9, left: 9)).rumble == nil)
        #expect(profile.decodeOutput(dualSenseOutput(flag0: 0x02, right: 9, left: 9)).rumble == nil)
    }

    @Test func aDualSenseLightbarAndPlayerLedsAreDecodedWhenTheirFlagsAreSet() {
        let profile = DualSenseProfile(edge: true)
        let effects = profile.decodeOutput(dualSenseOutput(flag1: 0x04 | 0x10, leds: 0b01010, color: (1, 2, 3)))
        #expect(effects.light == .init(red: 1, green: 2, blue: 3))
        #expect(effects.player == .set(1))
        #expect(effects.rumble == nil)
        #expect(profile.decodeOutput(dualSenseOutput(leds: 0b01010, color: (1, 2, 3))).isEmpty)
    }

    @Test func thePlayerLedPatternsAreIOSPlayersOneThroughFour() {
        let profile = DualSenseProfile(edge: false)
        let patterns: [(UInt8, GamepadOutputEffects.Player)] = [
            (0b00100, .set(0)), (0b01010, .set(1)), (0b10101, .set(2)), (0b11011, .set(3)), (0, .unset), (0b11111, .unset),
        ]
        for (leds, player) in patterns {
            #expect(profile.decodeOutput(dualSenseOutput(flag1: 0x10, leds: leds)).player == player, "leds \(leds)")
        }
    }

    @Test func aDualShockReportCarriesRumbleAndLightbarByItsFlags() {
        let profile = DualShock4Profile()
        var report = [UInt8](repeating: 0, count: 32)
        report[0] = 5
        report[1] = 0x01 | 0x02
        report[4] = 255  // Right (weak) motor
        report[5] = 51  // Left (strong) motor
        (report[6], report[7], report[8]) = (10, 20, 30)
        let effects = profile.decodeOutput(report)
        #expect(effects.rumble == .init(strong: 0.2, weak: 1))
        #expect(effects.light == .init(red: 10, green: 20, blue: 30))
        report[1] = 0x02
        #expect(profile.decodeOutput(report).rumble == nil)
        report[1] = 0x01
        #expect(profile.decodeOutput(report).light == nil)
    }

    @Test func anOutputReportThatIsNotTheFamilysOrIsShortAsksNothing() {
        #expect(DualSenseProfile(edge: false).decodeOutput([]).isEmpty)
        #expect(DualSenseProfile(edge: false).decodeOutput([5] + [UInt8](repeating: 0xff, count: 62)).isEmpty)
        #expect(DualSenseProfile(edge: false).decodeOutput([2, 0xff, 0xff, 0xff]).isEmpty)
        #expect(DualShock4Profile().decodeOutput([2] + [UInt8](repeating: 0xff, count: 31)).isEmpty)
        #expect(DualShock4Profile().decodeOutput([5, 3]).isEmpty)
        #expect(XboxProfile().decodeOutput([5, 3, 0, 0, 100, 100]).isEmpty)
        #expect(GenericProfile(name: "x", elements: .extended).decodeOutput([3, 3, 0, 0, 100, 100, 0, 0, 0]).isEmpty)
    }

    // MARK: Xbox output

    @Test func anXboxRumbleReportCarriesStrongAndWeakInPercent() {
        let profile = XboxProfile()
        // report ID 3, enable (bit 0 weak, bit 1 strong), four magnitudes (the
        // two trigger motors, strong, weak), duration, delay, loop.
        let both = profile.decodeOutput([3, 0x03, 0, 0, 100, 50, 255, 0, 0])
        #expect(both.rumble == .init(strong: 1, weak: 0.5))
        let weakOnly = profile.decodeOutput([3, 0x01, 0, 0, 100, 40, 255, 0, 0])
        #expect(weakOnly.rumble == .init(strong: 0, weak: 0.4))
        let strongOnly = profile.decodeOutput([3, 0x02, 0, 0, 25, 40, 255, 0, 0])
        #expect(strongOnly.rumble == .init(strong: 0.25, weak: 0))
        #expect(profile.decodeOutput([3, 0x03, 0, 0, 0, 0, 0, 0, 0]).rumble == .init(strong: 0, weak: 0))
        // A magnitude past 100 is 100.
        #expect(profile.decodeOutput([3, 0x03, 0, 0, 200, 0, 0, 0, 0]).rumble == .init(strong: 1, weak: 0))
    }

    // MARK: Generic and Xbox input, read by the descriptor

    private let hidPage = 0x09

    private func button(_ usage: Int, _ model: HIDDescriptorModel, _ report: [UInt8]) -> Int? {
        model.value(page: hidPage, usage: usage, in: report)
    }

    @Test func aGenericReportIsReadBackThroughItsDescriptor() throws {
        let profile = GenericProfile(name: "Pad", elements: .extended)
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        let state = GamepadState(
            buttons: [.south, .rightShoulder, .mode], directions: [.down, .left], leftStick: GamepadStick(x: 1, y: 1),
            rightStick: GamepadStick(x: -1, y: -1), leftTrigger: 1, rightTrigger: 0)
        let report = encode(profile, state)
        #expect(report.count == model.reportSize(.input, id: 1))
        #expect(model.value(page: 1, usage: 0x30, in: report) == 65535)
        #expect(model.value(page: 1, usage: 0x31, in: report) == 0)
        #expect(model.value(page: 1, usage: 0x32, in: report) == 0)
        #expect(model.value(page: 1, usage: 0x35, in: report) == 65535)
        // Brake is the left trigger and Accelerator the right one, 10 bits.
        #expect(model.value(page: 2, usage: 0xc5, in: report) == 1023)
        #expect(model.value(page: 2, usage: 0xc4, in: report) == 0)
        // The hat is 1 through 8 clockwise from up; down and left is 6.
        #expect(model.value(page: 1, usage: 0x39, in: report) == 6)
        #expect(button(1, model, report) == 1)  // South: BTN_SOUTH
        #expect(button(8, model, report) == 1)  // Right shoulder: BTN_TR
        #expect(button(13, model, report) == 1)  // Mode
        #expect(button(2, model, report) == 0)
    }

    @Test func aCenteredGenericReportIsAtTheMiddleOfEachAxis() throws {
        let profile = GenericProfile(name: "Pad", elements: .extended)
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        let report = encode(profile, .neutral)
        for usage in [0x30, 0x31, 0x32, 0x35] {
            let value = try #require(model.value(page: 1, usage: usage, in: report))
            #expect(abs(value - 32768) <= 1)
        }
        #expect(model.value(page: 1, usage: 0x39, in: report) == 0)
        #expect(model.value(page: 2, usage: 0xc5, in: report) == 0)
    }

    @Test func eachGenericButtonIsItsOwnUsageOnTheButtonPage() throws {
        let profile = GenericProfile(name: "Pad", elements: .extended)
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        // The usages become BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, L1, R1, L2, R2,
        // Select, Start, Mode, Thumb L, Thumb R; 3 and 6 stay empty.
        let usages: [(GamepadButtons, Int)] = [
            (.south, 1), (.east, 2), (.west, 4), (.north, 5), (.leftShoulder, 7), (.rightShoulder, 8),
            (.leftTrigger, 9), (.rightTrigger, 10), (.select, 11), (.start, 12), (.mode, 13),
            (.leftThumbstick, 14), (.rightThumbstick, 15),
        ]
        for (pressed, usage) in usages {
            let report = encode(profile, GamepadState(buttons: pressed))
            for (_, other) in usages {
                #expect(button(other, model, report) == (other == usage ? 1 : 0), "\(pressed) reads usage \(other)")
            }
        }
        #expect(button(3, model, encode(profile, GamepadState(buttons: .standard))) == nil)
        #expect(button(6, model, encode(profile, GamepadState(buttons: .standard))) == nil)
    }

    @Test func aGenericDeviceListsOnlyWhatTheControllerHas() throws {
        let elements = GamepadElements(buttons: [.south, .east], leftStick: true, directionPad: true)
        let profile = GenericProfile(name: "Micro", elements: elements)
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        #expect(model.field(page: 1, usage: 0x30) != nil)
        #expect(model.field(page: 1, usage: 0x32) == nil)
        #expect(model.field(page: 2, usage: 0xc5) == nil)
        #expect(model.field(page: 1, usage: 0x39) != nil)
        #expect(model.field(page: 9, usage: 1) != nil)
        #expect(model.field(page: 9, usage: 4) == nil)
        let report = encode(profile, GamepadState(buttons: [.south, .north], directions: .up, leftStick: GamepadStick(x: 0.5, y: 0)))
        #expect(report.count == model.reportSize(.input, id: 1))
        #expect(button(1, model, report) == 1)
        #expect(button(2, model, report) == 0)
        #expect(model.value(page: 1, usage: 0x39, in: report) == 1)
        let x = try #require(model.value(page: 1, usage: 0x30, in: report))
        #expect(abs(x - 49151) <= 1)
    }

    @Test func aGenericDeviceWithNoElementsStillHasAnInputReport() throws {
        // Nothing listed: the descriptor declares no field, and the encoder keeps up.
        let profile = GenericProfile(name: "None", elements: GamepadElements())
        let report = encode(profile, GamepadState(buttons: .standard))
        #expect(report == [1])
    }

    @Test func theXboxRecordButtonIsItsOwnReportSentWhenItChanges() throws {
        let profile = XboxProfile()
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        var encoder = profile.makeEncoder()
        let first = encoder.reports(for: .neutral, after: nil)
        // The first report of a session sends the Record report whatever it holds.
        #expect(first.count == 2)
        #expect(first[1] == [2, 0])
        let held = GamepadState(buttons: .record)
        let pressed = encoder.reports(for: held, after: .neutral)
        #expect(pressed.count == 2)
        #expect(model.value(page: 0x0c, usage: 0xb2, in: pressed[1], reportID: 2) == 1)
        #expect(model.reportSize(.input, id: 2) == 2)
        // Record unchanged: the report of the pad alone.
        let moved = encoder.reports(for: GamepadState(buttons: .record, leftTrigger: 0.5), after: held)
        #expect(moved.count == 1)
        let released = encoder.reports(for: .neutral, after: held)
        #expect(released.count == 2)
        #expect(released[1] == [2, 0])
    }

    @Test func theXboxPadReportHasNoRecordButtonInIt() throws {
        let profile = XboxProfile()
        let model = try HIDDescriptorModel(profile.create(slot: 0).descriptor)
        var encoder = profile.makeEncoder()
        let report = encoder.reports(for: GamepadState(buttons: .record), after: nil)[0]
        #expect(report.count == model.reportSize(.input, id: 1))
        for usage in 1...15 { #expect(button(usage, model, report) != 1) }
    }
}
