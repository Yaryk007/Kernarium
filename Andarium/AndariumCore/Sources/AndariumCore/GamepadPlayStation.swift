// SPDX-License-Identifier: GPL-2.0-or-later

// The profiles of Sony's DualSense, DualSense Edge and DualShock 4.
//
// The report layouts below are transcribed from the Linux kernel's
// drivers/hid/hid-playstation.c (SPDX GPL-2.0-or-later, Copyright (c)
// 2020-2022 Sony Interactive Entertainment), the driver that the guest binds
// to these devices: the sizes, field offsets and flag bits of its input,
// output and feature reports are the ones it reads and writes, so a change
// to either side has to follow the other. This file keeps the driver's
// licence identifier for that reason.
//
// Each device is the USB variant, which the driver reads without the
// Bluetooth CRC, and never presents a Bluetooth address to Android. The
// motion and touchpad fields of the input reports are inactive (the driver
// still creates the controller's sensor and touchpad devices, which stay
// silent).

import Foundation

/// The parts of the PlayStation reports that both generations share.
enum PlayStationReports {
    /// The device version the driver ORs 0x8000 into, which Android's key
    /// layouts for these controllers match.
    static let version: UInt32 = 0x0100

    // The first button byte: hat switch in the low nibble, then the four face buttons.
    static let hatNeutral: UInt8 = 8
    static let square: UInt8 = 1 << 4
    static let cross: UInt8 = 1 << 5
    static let circle: UInt8 = 1 << 6
    static let triangle: UInt8 = 1 << 7
    // The second button byte.
    static let leftShoulder: UInt8 = 1 << 0
    static let rightShoulder: UInt8 = 1 << 1
    static let leftTrigger: UInt8 = 1 << 2
    static let rightTrigger: UInt8 = 1 << 3
    static let create: UInt8 = 1 << 4
    static let options: UInt8 = 1 << 5
    static let leftThumbstick: UInt8 = 1 << 6
    static let rightThumbstick: UInt8 = 1 << 7
    // The third button byte.
    static let home: UInt8 = 1 << 0

    /// The hat switch value of a direction pad: 0 through 7 clockwise from
    /// up, and 8 for neutral.
    static func hat(_ directions: GamepadDirections) -> UInt8 {
        let value = directions.hat
        return value == 0 ? hatNeutral : value - 1
    }

    /// The three button bytes of both generations' reports.
    static func buttons(_ state: GamepadState) -> [UInt8] {
        var first = hat(state.directions)
        var second: UInt8 = 0
        var third: UInt8 = 0
        let buttons = state.buttons
        if buttons.contains(.west) { first |= square }
        if buttons.contains(.south) { first |= cross }
        if buttons.contains(.east) { first |= circle }
        if buttons.contains(.north) { first |= triangle }
        if buttons.contains(.leftShoulder) { second |= leftShoulder }
        if buttons.contains(.rightShoulder) { second |= rightShoulder }
        if buttons.contains(.leftTrigger) { second |= leftTrigger }
        if buttons.contains(.rightTrigger) { second |= rightTrigger }
        if buttons.contains(.select) { second |= create }
        if buttons.contains(.start) { second |= options }
        if buttons.contains(.leftThumbstick) { second |= leftThumbstick }
        if buttons.contains(.rightThumbstick) { second |= rightThumbstick }
        if buttons.contains(.mode) { third |= home }
        return [first, second, third]
    }

    /// A stick or trigger axis as 0 through 255, 128 centered for a stick.
    static func axis(_ value: Float) -> UInt8 { UInt8(((value.isNaN ? 0 : min(1, max(-1, value))) + 1) * 127.5 + 0.5) }
    static func trigger(_ value: Float) -> UInt8 { UInt8((value.isNaN ? 0 : min(1, max(0, value))) * 255 + 0.5) }

    /// The battery's tenths, 0 through 10 (a unit is 10%, 0 meaning 0-9%).
    static func tenths(_ level: Float) -> UInt8 { UInt8(min(10, max(0, (level * 10).rounded(.down)))) }

    /// The pairing address a slot's device presents: locally administered
    /// (its first octet is 0x02), different in every slot, and never a real
    /// one. It is in the report's byte order, the driver's little endian.
    static func address(slot: Int) -> [UInt8] {
        [0x40 + UInt8(slot), 0xa1, 0x11, 0x00, 0x00, 0x02]
    }

    /// The motors' levels of an output report, as the driver's two bytes.
    static func rumble(strong: UInt8, weak: UInt8) -> GamepadOutputEffects.Rumble {
        .init(strong: Float(strong) / 255, weak: Float(weak) / 255)
    }

    /// iOS's player index for the pattern the driver lights on the
    /// DualSense's five LEDs (a player's number is centered across them).
    static func player(leds: UInt8) -> GamepadOutputEffects.Player {
        switch leds & 0x1f {
        case 0b00100: .set(0)
        case 0b01010: .set(1)
        case 0b10101: .set(2)
        case 0b11011: .set(3)
        default: .unset
        }
    }

    /// Gyroscope and accelerometer calibration that makes the driver's scale
    /// the nominal one, for the feature report both generations send.
    static func putCalibration(into report: inout [UInt8]) {
        func put(_ value: Int16, at offset: Int) {
            let bits = UInt16(bitPattern: value)
            report[offset] = UInt8(bits & 0xff)
            report[offset + 1] = UInt8(bits >> 8)
        }
        // Biases, then the plus and minus end of each gyro axis, then the speed.
        for axis in 0..<3 { put(0, at: 1 + 2 * axis) }
        for axis in 0..<3 {
            put(16384, at: 7 + 4 * axis)
            put(-16383, at: 9 + 4 * axis)
        }
        put(1024, at: 19)
        put(1024, at: 21)
        // Plus and minus 1 g on each accelerometer axis.
        for axis in 0..<3 {
            put(8192, at: 23 + 4 * axis)
            put(-8192, at: 25 + 4 * axis)
        }
    }
}

/// The DualSense, and the DualSense Edge that only its product number
/// tells apart.
///
/// Reports of hid-playstation's DualSense, over USB: input report 1 of 64
/// bytes, output report 2 of 63, and the feature reports 0x05 (calibration),
/// 0x09 (pairing address) and 0x20 (firmware) the driver reads once.
public struct DualSenseProfile: GamepadProfile {
    public let edge: Bool

    public init(edge: Bool) { self.edge = edge }

    public var family: GamepadFamily { edge ? .dualSenseEdge : .dualSense }

    static let inputReportSize = 64
    static let outputReportID: UInt8 = 0x02
    static let calibrationSize = 41
    static let pairingSize = 20
    static let firmwareSize = 64

    public func create(slot: Int) -> GamepadCreate {
        GamepadCreate(
            slot: slot, bus: 3, vendor: 0x054c, product: edge ? 0x0df2 : 0x0ce6, version: PlayStationReports.version,
            name: edge ? "DualSense Edge Wireless Controller" : "DualSense Wireless Controller", uniq: "",
            descriptor: Self.descriptor)
    }

    /// The descriptor the driver parses and nothing else reads: it declares the
    /// reports' IDs and sizes.
    static let descriptor: [UInt8] = {
        var d = HIDDescriptor()
        d.usagePage(0x01)
        d.usage(0x05)
        d.beginApplication()
        d.reportID(1)
        for usage in [0x30, 0x31, 0x32, 0x35, 0x33, 0x34] { d.usage(usage) }
        d.logicalMinimum(0)
        d.logicalMaximum(255)
        d.reportSize(8)
        d.reportCount(6)
        d.input(.value)
        d.vendorInput(page: 0xff00, usage: 0x20, bytes: 1)
        // The hat switch and the 14 buttons, with 6 more bits.
        d.usagePage(0x01)
        d.usage(0x39)
        d.logicalMinimum(0)
        d.logicalMaximum(7)
        d.physicalMinimum(0)
        d.physicalMaximum(315)
        d.unit(0x14)
        d.reportSize(4)
        d.reportCount(1)
        d.input([.value, .nullState])
        d.unit(0)
        d.usagePage(0x09)
        d.usageRange(1, 14)
        d.logicalMinimum(0)
        d.logicalMaximum(1)
        d.reportSize(1)
        d.reportCount(14)
        d.input(.value)
        d.usagePage(0xff00)
        d.usage(0x21)
        d.logicalMinimum(0)
        d.logicalMaximum(63)
        d.reportSize(6)
        d.reportCount(1)
        d.input(.value)
        d.vendorInput(page: 0xff00, usage: 0x22, bytes: inputReportSize - 1 - 6 - 1 - 3)
        d.reportID(Int(outputReportID))
        d.vendorOutput(page: 0xff00, usage: 0x23, bytes: 62)
        d.reportID(0x05)
        d.vendorFeature(page: 0xff00, usage: 0x24, bytes: calibrationSize - 1)
        d.reportID(0x09)
        d.vendorFeature(page: 0xff00, usage: 0x25, bytes: pairingSize - 1)
        d.reportID(0x20)
        d.vendorFeature(page: 0xff00, usage: 0x26, bytes: firmwareSize - 1)
        d.endCollection()
        return d.bytes
    }()

    public func featureReport(number: UInt8, slot: Int) -> [UInt8]? {
        switch number {
        case 0x09:
            var report = [UInt8](repeating: 0, count: Self.pairingSize)
            report[0] = 0x09
            report.replaceSubrange(1..<7, with: PlayStationReports.address(slot: slot))
            return report
        case 0x20:
            // Hardware and firmware versions, and the update version 2.21 or
            // later, which makes the driver rumble the way the console does.
            var report = [UInt8](repeating: 0, count: Self.firmwareSize)
            report[0] = 0x20
            report.replaceSubrange(24..<28, with: [0x00, 0x01, 0x00, 0x00])
            report.replaceSubrange(28..<32, with: [0x00, 0x00, 0x00, 0x01])
            report.replaceSubrange(44..<46, with: [0x15, 0x02])
            return report
        case 0x05:
            var report = [UInt8](repeating: 0, count: Self.calibrationSize)
            report[0] = 0x05
            PlayStationReports.putCalibration(into: &report)
            return report
        default:
            return nil
        }
    }

    public func makeEncoder() -> any GamepadReportEncoder { DualSenseEncoder() }

    public func decodeOutput(_ report: [UInt8]) -> GamepadOutputEffects {
        // report ID 2, then valid_flag0, valid_flag1, motor_right, motor_left
        // (1 through 4), valid_flag2 (39), player_leds (44), lightbar RGB (45
        // through 47).
        guard report.count >= 48, report[0] == Self.outputReportID else { return GamepadOutputEffects() }
        let flag0 = report[1]
        let flag1 = report[2]
        let flag2 = report[39]
        var effects = GamepadOutputEffects()
        // Select classic rumble and enable it, in either generation of firmware.
        if flag0 & 0x02 != 0, flag0 & 0x01 != 0 || flag2 & 0x04 != 0 {
            effects.rumble = PlayStationReports.rumble(strong: report[4], weak: report[3])
        }
        if flag1 & 0x04 != 0 {
            effects.light = .init(red: report[45], green: report[46], blue: report[47])
        }
        if flag1 & 0x10 != 0 {
            effects.player = PlayStationReports.player(leds: report[44])
        }
        return effects
    }
}

struct DualSenseEncoder: GamepadReportEncoder {
    var sequence: UInt8 = 0

    mutating func reports(for state: GamepadState, after previous: GamepadState?) -> [[UInt8]] {
        var report = [UInt8](repeating: 0, count: DualSenseProfile.inputReportSize)
        report[0] = 0x01
        report[1] = PlayStationReports.axis(state.leftStick.x)
        report[2] = PlayStationReports.axis(-state.leftStick.y)
        report[3] = PlayStationReports.axis(state.rightStick.x)
        report[4] = PlayStationReports.axis(-state.rightStick.y)
        report[5] = PlayStationReports.trigger(state.leftTrigger)
        report[6] = PlayStationReports.trigger(state.rightTrigger)
        report[7] = sequence
        sequence &+= 1
        report.replaceSubrange(8..<11, with: PlayStationReports.buttons(state))
        // The touchpad's two contacts are inactive (the high bit of a contact).
        report[33] = 0x80
        report[37] = 0x80
        // status[0]: the battery in tenths, and what it is doing in the high nibble.
        let tenths = PlayStationReports.tenths(state.battery.level)
        switch state.battery.state {
        case .charging: report[53] = 0x10 | tenths
        case .full: report[53] = 0x20 | 10
        case .discharging, .unknown: report[53] = tenths
        }
        return [report]
    }
}

/// The DualShock 4 (the second generation's USB product).
///
/// Reports of hid-playstation's DualShock 4, over USB: input report 1 of 64
/// bytes, output report 5 of 32, and the feature reports 0x02 (calibration),
/// 0x12 (pairing address) and 0xa3 (firmware).
public struct DualShock4Profile: GamepadProfile {
    public init() {}

    public var family: GamepadFamily { .dualShock4 }

    static let inputReportSize = 64
    static let outputReportID: UInt8 = 0x05
    static let calibrationSize = 37
    static let pairingSize = 16
    static let firmwareSize = 49

    public func create(slot: Int) -> GamepadCreate {
        GamepadCreate(
            slot: slot, bus: 3, vendor: 0x054c, product: 0x09cc, version: PlayStationReports.version,
            name: "Wireless Controller", uniq: "", descriptor: Self.descriptor)
    }

    static let descriptor: [UInt8] = {
        var d = HIDDescriptor()
        d.usagePage(0x01)
        d.usage(0x05)
        d.beginApplication()
        d.reportID(1)
        for usage in [0x30, 0x31, 0x32, 0x35] { d.usage(usage) }
        d.logicalMinimum(0)
        d.logicalMaximum(255)
        d.reportSize(8)
        d.reportCount(4)
        d.input(.value)
        d.usagePage(0x01)
        d.usage(0x39)
        d.logicalMinimum(0)
        d.logicalMaximum(7)
        d.physicalMinimum(0)
        d.physicalMaximum(315)
        d.unit(0x14)
        d.reportSize(4)
        d.reportCount(1)
        d.input([.value, .nullState])
        d.unit(0)
        d.usagePage(0x09)
        d.usageRange(1, 14)
        d.logicalMinimum(0)
        d.logicalMaximum(1)
        d.reportSize(1)
        d.reportCount(14)
        d.input(.value)
        d.usagePage(0xff00)
        d.usage(0x20)
        d.logicalMinimum(0)
        d.logicalMaximum(63)
        d.reportSize(6)
        d.reportCount(1)
        d.input(.value)
        d.usagePage(0x01)
        d.usage(0x33)
        d.usage(0x34)
        d.logicalMinimum(0)
        d.logicalMaximum(255)
        d.reportSize(8)
        d.reportCount(2)
        d.input(.value)
        d.vendorInput(page: 0xff00, usage: 0x21, bytes: inputReportSize - 1 - 4 - 3 - 2)
        d.reportID(Int(outputReportID))
        d.vendorOutput(page: 0xff00, usage: 0x22, bytes: 31)
        d.reportID(0x02)
        d.vendorFeature(page: 0xff00, usage: 0x23, bytes: calibrationSize - 1)
        d.reportID(0x12)
        d.vendorFeature(page: 0xff00, usage: 0x24, bytes: pairingSize - 1)
        d.reportID(0xa3)
        d.vendorFeature(page: 0xff00, usage: 0x25, bytes: firmwareSize - 1)
        d.endCollection()
        return d.bytes
    }()

    public func featureReport(number: UInt8, slot: Int) -> [UInt8]? {
        switch number {
        case 0x12:
            var report = [UInt8](repeating: 0, count: Self.pairingSize)
            report[0] = 0x12
            report.replaceSubrange(1..<7, with: PlayStationReports.address(slot: slot))
            return report
        case 0xa3:
            var report = [UInt8](repeating: 0, count: Self.firmwareSize)
            report[0] = 0xa3
            report.replaceSubrange(35..<37, with: [0x00, 0x01])
            report.replaceSubrange(41..<43, with: [0x00, 0x01])
            return report
        case 0x02:
            var report = [UInt8](repeating: 0, count: Self.calibrationSize)
            report[0] = 0x02
            PlayStationReports.putCalibration(into: &report)
            return report
        default:
            return nil
        }
    }

    public func makeEncoder() -> any GamepadReportEncoder { DualShock4Encoder() }

    public func decodeOutput(_ report: [UInt8]) -> GamepadOutputEffects {
        // report ID 5, then valid_flag0 (1), motor_right and motor_left (4,
        // 5) and the lightbar RGB (6 through 8). The driver sends rumble and
        // lightbar together, whichever one changed.
        guard report.count >= 9, report[0] == Self.outputReportID else { return GamepadOutputEffects() }
        let flag0 = report[1]
        var effects = GamepadOutputEffects()
        if flag0 & 0x01 != 0 {
            effects.rumble = PlayStationReports.rumble(strong: report[5], weak: report[4])
        }
        if flag0 & 0x02 != 0 {
            effects.light = .init(red: report[6], green: report[7], blue: report[8])
        }
        return effects
    }
}

struct DualShock4Encoder: GamepadReportEncoder {
    mutating func reports(for state: GamepadState, after previous: GamepadState?) -> [[UInt8]] {
        var report = [UInt8](repeating: 0, count: DualShock4Profile.inputReportSize)
        report[0] = 0x01
        report[1] = PlayStationReports.axis(state.leftStick.x)
        report[2] = PlayStationReports.axis(-state.leftStick.y)
        report[3] = PlayStationReports.axis(state.rightStick.x)
        report[4] = PlayStationReports.axis(-state.rightStick.y)
        report.replaceSubrange(5..<8, with: PlayStationReports.buttons(state))
        report[8] = PlayStationReports.trigger(state.leftTrigger)
        report[9] = PlayStationReports.trigger(state.rightTrigger)
        // status[0]: the battery in tenths, with the cable's state in bit 4: on
        // a cable, 11 is full. The touch reports are none, and their contacts inactive.
        let tenths = PlayStationReports.tenths(state.battery.level)
        switch state.battery.state {
        case .charging: report[30] = 0x10 | tenths
        case .full: report[30] = 0x10 | 11
        case .discharging, .unknown: report[30] = tenths
        }
        for contact in [35, 39, 44, 48, 53, 57] { report[contact] = 0x80 }
        return [report]
    }
}
