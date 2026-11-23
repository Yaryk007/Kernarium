// SPDX-License-Identifier: GPL-2.0-or-later

// The profiles of a plain HID gamepad and of the Xbox Wireless Controller.
//
// The Xbox profile's force feedback report is transcribed from the Linux
// kernel's drivers/hid/hid-microsoft.c (SPDX GPL-2.0-or-later), the driver
// that the guest binds to that device: the report's ID, size and field offsets
// are the ones the driver writes. This file keeps the driver's licence
// identifier for that reason.

import Foundation

/// The report of a plain HID gamepad (report 1) and the descriptor that
/// declares it, built from one list of fields so that the two cannot drift
/// apart. The guest's hid-generic turns the descriptor into an input device,
/// and Android's stock `Generic.kl` maps its codes, so the usages are chosen
/// for those codes. Only the elements the controller has are listed, which
/// makes Android's answer to "has this key" true. Every field is whole bytes:
///
/// - a stick axis: 16 bits, 0 through 65535, centered at 32768, y down;
/// - a trigger: 10 bits, 0 through 1023, in 16 bits (Brake is the left
///   trigger, Accelerator the right one);
/// - the hat switch: 4 bits, 0 for neutral and 1 through 8 clockwise from up,
///   in 8 bits;
/// - the buttons: one bit each in the order of `buttonUsages`, padded to whole
///   bytes.
struct GamepadGenericLayout: Equatable, Sendable {
    enum Field: Equatable, Sendable {
        case leftX, leftY, rightX, rightY
        case leftTrigger, rightTrigger
        case hat
        case buttons([GamepadButtons])
    }

    /// The button page's usage of each button. A usage becomes the evdev code
    /// `BTN_GAMEPAD + usage - 1`, and `Generic.kl` names the codes: A, B, X, Y,
    /// L1, R1, L2, R2, Select, Start, Mode, Thumb L, Thumb R. Usages 3 and 6
    /// stay empty, as on an Xbox controller, so that X and Y are the codes Android
    /// calls BUTTON_X and BUTTON_Y.
    static let buttonUsages: [(button: GamepadButtons, usage: Int)] = [
        (.south, 1), (.east, 2), (.west, 4), (.north, 5), (.leftShoulder, 7), (.rightShoulder, 8),
        (.leftTrigger, 9), (.rightTrigger, 10), (.select, 11), (.start, 12), (.mode, 13),
        (.leftThumbstick, 14), (.rightThumbstick, 15),
    ]

    /// The report ID of the report the layout describes.
    static let reportID: UInt8 = 1

    let fields: [Field]

    init(elements: GamepadElements) {
        var fields = [Field]()
        if elements.leftStick { fields += [.leftX, .leftY] }
        if elements.rightStick { fields += [.rightX, .rightY] }
        if elements.triggers { fields += [.leftTrigger, .rightTrigger] }
        if elements.directionPad { fields.append(.hat) }
        let present = Self.buttonUsages.map(\.button).filter { elements.buttons.contains($0) }
        if !present.isEmpty { fields.append(.buttons(present)) }
        self.fields = fields
    }

    /// The report's size in bytes, with its ID.
    var reportSize: Int {
        1 + fields.reduce(0) { total, field in
            switch field {
            case .leftX, .leftY, .rightX, .rightY, .leftTrigger, .rightTrigger: total + 2
            case .hat: total + 1
            case .buttons(let buttons): total + (buttons.count + 7) / 8
            }
        }
    }

    /// The descriptor, with the Record button's report of its own when `record`.
    func descriptor(record: Bool) -> [UInt8] {
        var d = HIDDescriptor()
        d.usagePage(0x01)
        d.usage(0x05)
        d.beginApplication()
        d.reportID(Int(Self.reportID))
        for field in fields {
            switch field {
            case .leftX: axis(&d, usage: 0x30)
            case .leftY: axis(&d, usage: 0x31)
            case .rightX: axis(&d, usage: 0x32)
            case .rightY: axis(&d, usage: 0x35)
            case .leftTrigger: trigger(&d, usage: 0xc5)
            case .rightTrigger: trigger(&d, usage: 0xc4)
            case .hat:
                d.usagePage(0x01)
                d.usage(0x39)
                d.logicalMinimum(1)
                d.logicalMaximum(8)
                d.physicalMinimum(0)
                d.physicalMaximum(315)
                d.unit(0x14)
                d.reportSize(4)
                d.reportCount(1)
                d.input([.value, .nullState])
                d.unit(0)
                d.paddingInput(bits: 4)
            case .buttons(let buttons):
                d.usagePage(0x09)
                for button in buttons { d.usage(Self.usage(of: button)) }
                d.logicalMinimum(0)
                d.logicalMaximum(1)
                d.reportSize(1)
                d.reportCount(buttons.count)
                d.input(.value)
                d.paddingInput(bits: (8 - buttons.count % 8) % 8)
            }
        }
        d.endCollection()
        if record {
            // The Record button, as the Consumer page's own usage in a report
            // of its own, which hid-input turns into KEY_RECORD.
            d.usagePage(0x0c)
            d.usage(0x01)
            d.beginApplication()
            d.reportID(Int(GamepadGenericLayout.recordReportID))
            d.logicalMinimum(0)
            d.logicalMaximum(1)
            d.usage(0xb2)
            d.reportSize(1)
            d.reportCount(1)
            d.input(.value)
            d.paddingInput(bits: 7)
            d.endCollection()
        }
        return d.bytes
    }

    static let recordReportID: UInt8 = 2

    private func axis(_ d: inout HIDDescriptor, usage: Int) {
        d.usagePage(0x01)
        d.usage(usage)
        d.logicalMinimum(0)
        d.logicalMaximum(65535)
        d.reportSize(16)
        d.reportCount(1)
        d.input(.value)
    }

    private func trigger(_ d: inout HIDDescriptor, usage: Int) {
        d.usagePage(0x02)
        d.usage(usage)
        d.logicalMinimum(0)
        d.logicalMaximum(1023)
        d.reportSize(10)
        d.reportCount(1)
        d.input(.value)
        d.paddingInput(bits: 6)
    }

    private static func usage(of button: GamepadButtons) -> Int {
        buttonUsages.first { $0.button == button }!.usage
    }

    /// The report for `state`, with its ID.
    func report(for state: GamepadState) -> [UInt8] {
        var report = [UInt8]()
        report.reserveCapacity(reportSize)
        report.append(Self.reportID)
        for field in fields {
            switch field {
            case .leftX: append(axis: state.leftStick.x, to: &report)
            case .leftY: append(axis: -state.leftStick.y, to: &report)
            case .rightX: append(axis: state.rightStick.x, to: &report)
            case .rightY: append(axis: -state.rightStick.y, to: &report)
            case .leftTrigger: append(trigger: state.leftTrigger, to: &report)
            case .rightTrigger: append(trigger: state.rightTrigger, to: &report)
            case .hat: report.append(state.directions.hat)
            case .buttons(let buttons):
                var bytes = [UInt8](repeating: 0, count: (buttons.count + 7) / 8)
                for (index, button) in buttons.enumerated() where state.buttons.contains(button) {
                    bytes[index / 8] |= 1 << UInt8(index % 8)
                }
                report.append(contentsOf: bytes)
            }
        }
        return report
    }

    private func append(axis value: Float, to report: inout [UInt8]) {
        let scaled = UInt16(((value + 1) / 2 * 65535).rounded())
        report.append(UInt8(scaled & 0xff))
        report.append(UInt8(scaled >> 8))
    }

    private func append(trigger value: Float, to report: inout [UInt8]) {
        let scaled = UInt16((value * 1023).rounded())
        report.append(UInt8(scaled & 0xff))
        report.append(UInt8(scaled >> 8))
    }
}

/// The input reports of a plain HID gamepad: its one report, and for an
/// Xbox controller the Record button's, sent when that button changes.
struct GamepadGenericEncoder: GamepadReportEncoder {
    var layout: GamepadGenericLayout
    var record: Bool

    mutating func reports(for state: GamepadState, after previous: GamepadState?) -> [[UInt8]] {
        var reports = [layout.report(for: state)]
        if record, previous == nil || previous!.buttons.contains(.record) != state.buttons.contains(.record) {
            reports.append([GamepadGenericLayout.recordReportID, state.buttons.contains(.record) ? 1 : 0])
        }
        return reports
    }
}

/// Any controller with a gamepad profile that is not a PlayStation or Xbox
/// one: a USB HID gamepad with no vendor or product, which the guest's
/// hid-generic binds and Android maps with `Generic.kl`. Its guest name is
/// the controller's own.
public struct GenericProfile: GamepadProfile {
    public let name: String
    public let elements: GamepadElements

    public init(name: String, elements: GamepadElements) {
        self.name = name
        self.elements = elements
    }

    public var family: GamepadFamily { .generic }

    private var layout: GamepadGenericLayout { GamepadGenericLayout(elements: elements) }

    public func create(slot: Int) -> GamepadCreate {
        GamepadCreate(
            slot: slot, bus: 3, vendor: 0, product: 0, version: 0, name: GamepadNames.deviceName(name),
            uniq: "kernarium-gamepad-\(slot)", descriptor: layout.descriptor(record: false))
    }

    public func featureReport(number: UInt8, slot: Int) -> [UInt8]? { nil }

    public func makeEncoder() -> any GamepadReportEncoder {
        GamepadGenericEncoder(layout: layout, record: false)
    }

    public func decodeOutput(_ report: [UInt8]) -> GamepadOutputEffects { GamepadOutputEffects() }
}

/// An Xbox Wireless Controller (Model 1914) on Bluetooth, which the guest's
/// hid-microsoft binds: it adds force feedback to what hid-input maps from
/// the descriptor, so Android sees the stock mapping, with Share as Record
/// and rumble in the guest.
///
/// The force feedback report is transcribed from the Linux kernel's
/// drivers/hid/hid-microsoft.c (GPL-2.0-or-later), whose driver sends it: one
/// output report, ID 3, of 9 bytes, `enable` (bit 0 weak, bit 1 strong), four
/// magnitudes (the two triggers' motors, then strong, then weak, each 0
/// through 100), a duration, a start delay and a loop count.
public struct XboxProfile: GamepadProfile {
    public init() {}

    public var family: GamepadFamily { .xbox }

    private var layout: GamepadGenericLayout { GamepadGenericLayout(elements: .extended) }

    public func create(slot: Int) -> GamepadCreate {
        // The Bluetooth bus and the product hid-microsoft lists. The uniq is
        // text that is no Bluetooth address, so that Android never looks the
        // device up in a Bluetooth stack the image does not have.
        GamepadCreate(
            slot: slot, bus: 5, vendor: 0x045e, product: 0x0b13, version: 0x0100, name: "Xbox Wireless Controller",
            uniq: "kernarium-gamepad-\(slot)", descriptor: layout.descriptor(record: true))
    }

    public func featureReport(number: UInt8, slot: Int) -> [UInt8]? { nil }

    public func makeEncoder() -> any GamepadReportEncoder {
        GamepadGenericEncoder(layout: layout, record: true)
    }

    public func decodeOutput(_ report: [UInt8]) -> GamepadOutputEffects {
        guard report.count >= 6, report[0] == 3 else { return GamepadOutputEffects() }
        let enable = report[1]
        let strong = enable & 0x02 != 0 ? Float(min(report[4], 100)) / 100 : 0
        let weak = enable & 0x01 != 0 ? Float(min(report[5], 100)) / 100 : 0
        return GamepadOutputEffects(rumble: .init(strong: strong, weak: weak))
    }
}
