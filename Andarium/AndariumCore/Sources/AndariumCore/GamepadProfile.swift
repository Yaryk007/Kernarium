// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Which kind of controller a guest device stands for. The guest's kernel
/// binds its own driver to each: hid-playstation to the three PlayStation
/// controllers, hid-microsoft to the Xbox one, hid-generic to the rest.
public enum GamepadFamily: Equatable, Hashable, Sendable {
    case dualSense
    case dualSenseEdge
    case dualShock4
    case xbox
    /// Any other controller with a gamepad profile (MFi, Switch, arcade
    /// sticks): a plain HID gamepad that lists only what the controller has.
    case generic
}

/// What a kernel driver asked of the controller in an output report.
public struct GamepadOutputEffects: Equatable, Sendable {
    /// The two rumble motors, each 0 through 1.
    public struct Rumble: Equatable, Sendable {
        /// The strong, low frequency motor: the left handle.
        public var strong: Float
        /// The weak, high frequency motor: the right handle.
        public var weak: Float
        public init(strong: Float, weak: Float) {
            self.strong = strong
            self.weak = weak
        }
    }

    public struct Light: Equatable, Sendable {
        public var red: UInt8
        public var green: UInt8
        public var blue: UInt8
        public init(red: UInt8, green: UInt8, blue: UInt8) {
            self.red = red
            self.green = green
            self.blue = blue
        }
    }

    public enum Player: Equatable, Sendable {
        /// iOS's player index, 0 through 3.
        case set(Int)
        case unset
    }

    /// Nil when the report does not say.
    public var rumble: Rumble?
    public var light: Light?
    public var player: Player?

    public init(rumble: Rumble? = nil, light: Light? = nil, player: Player? = nil) {
        self.rumble = rumble
        self.light = light
        self.player = player
    }

    public var isEmpty: Bool { rumble == nil && light == nil && player == nil }
}

/// Turns a controller's states into the input reports of its family, keeping
/// whatever a family counts in them (a sequence number).
public protocol GamepadReportEncoder: Sendable {
    /// The reports to send for `state`, each with its report ID. `previous` is
    /// the state last sent, or nil for the first report, which a family with a
    /// report of its own for an element sends whatever it holds.
    mutating func reports(for state: GamepadState, after previous: GamepadState?) -> [[UInt8]]
}

/// What one family of controllers is to the guest: the device the kernel
/// binds its driver to, the reports it answers and sends, and what the
/// driver's output reports ask for.
public protocol GamepadProfile: Sendable {
    var family: GamepadFamily { get }
    /// The `CREATE` frame of the controller that takes `slot`: the same bytes
    /// every time for a slot, so a controller is the same device each session.
    func create(slot: Int) -> GamepadCreate
    /// The report the driver asks for as a feature report, with its report
    /// ID; nil for one the device does not have.
    func featureReport(number: UInt8, slot: Int) -> [UInt8]?
    func makeEncoder() -> any GamepadReportEncoder
    /// What an output report the driver sent asks of the controller. A report
    /// that is not the family's, or short of its fields, asks nothing.
    func decodeOutput(_ report: [UInt8]) -> GamepadOutputEffects
}

/// The profile of a family. `name` is the controller's own, which only a
/// generic profile shows to the guest, and `elements` what it has.
public enum GamepadProfiles {
    public static func profile(family: GamepadFamily, name: String, elements: GamepadElements) -> any GamepadProfile {
        switch family {
        case .dualSense: DualSenseProfile(edge: false)
        case .dualSenseEdge: DualSenseProfile(edge: true)
        case .dualShock4: DualShock4Profile()
        case .xbox: XboxProfile()
        case .generic: GenericProfile(name: name, elements: elements)
        }
    }
}

/// The errno a request of the driver for something the device has not is
/// answered with.
public enum GamepadErrno {
    public static let invalid: UInt16 = 22
}

/// uhid's report types, as the relay passes them.
public enum GamepadReportType {
    public static let feature: UInt8 = 0
    public static let output: UInt8 = 1
    public static let input: UInt8 = 2
}

// MARK: A HID report descriptor

/// A report descriptor built item by item, with each value in the fewest
/// bytes that hold it.
struct HIDDescriptor {
    private(set) var bytes: [UInt8] = []

    private enum Main: UInt8 { case input = 0x80, output = 0x90, feature = 0xb0, collection = 0xa0, endCollection = 0xc0 }
    private enum Global: UInt8 {
        case usagePage = 0x04, logicalMinimum = 0x14, logicalMaximum = 0x24, physicalMinimum = 0x34
        case physicalMaximum = 0x44, unit = 0x64, reportSize = 0x74, reportID = 0x84, reportCount = 0x94
    }
    private enum Local: UInt8 { case usage = 0x08, usageMinimum = 0x18, usageMaximum = 0x28 }

    /// The flag bits of an input, output or feature item.
    struct Flags: OptionSet {
        let rawValue: UInt8
        /// A constant: padding.
        static let constant = Flags(rawValue: 1 << 0)
        /// A variable, not an array.
        static let variable = Flags(rawValue: 1 << 1)
        static let relative = Flags(rawValue: 1 << 2)
        static let nullState = Flags(rawValue: 1 << 6)
        static let data: Flags = []
        static let value: Flags = [.variable]
        static let padding: Flags = [.constant, .variable]
    }

    private mutating func item(_ prefix: UInt8, _ value: Int?) {
        guard let value else {
            bytes.append(prefix)
            return
        }
        switch value {
        case -128...127:
            bytes.append(prefix | 1)
            bytes.append(UInt8(truncatingIfNeeded: value))
        case -32768...32767:
            bytes.append(prefix | 2)
            bytes.append(UInt8(truncatingIfNeeded: value))
            bytes.append(UInt8(truncatingIfNeeded: value >> 8))
        default:
            bytes.append(prefix | 3)
            for shift in stride(from: 0, to: 32, by: 8) { bytes.append(UInt8(truncatingIfNeeded: value >> shift)) }
        }
    }

    mutating func usagePage(_ page: Int) { item(Global.usagePage.rawValue, page) }
    mutating func usage(_ usage: Int) { item(Local.usage.rawValue, usage) }
    mutating func usageRange(_ minimum: Int, _ maximum: Int) {
        item(Local.usageMinimum.rawValue, minimum)
        item(Local.usageMaximum.rawValue, maximum)
    }
    mutating func logicalMinimum(_ value: Int) { item(Global.logicalMinimum.rawValue, value) }
    mutating func logicalMaximum(_ value: Int) { item(Global.logicalMaximum.rawValue, value) }
    mutating func physicalMinimum(_ value: Int) { item(Global.physicalMinimum.rawValue, value) }
    mutating func physicalMaximum(_ value: Int) { item(Global.physicalMaximum.rawValue, value) }
    mutating func unit(_ value: Int) { item(Global.unit.rawValue, value) }
    mutating func reportSize(_ bits: Int) { item(Global.reportSize.rawValue, bits) }
    mutating func reportCount(_ count: Int) { item(Global.reportCount.rawValue, count) }
    mutating func reportID(_ id: Int) { item(Global.reportID.rawValue, id) }
    mutating func input(_ flags: Flags) { item(Main.input.rawValue, Int(flags.rawValue)) }
    mutating func output(_ flags: Flags) { item(Main.output.rawValue, Int(flags.rawValue)) }
    mutating func feature(_ flags: Flags) { item(Main.feature.rawValue, Int(flags.rawValue)) }
    /// An application collection.
    mutating func beginApplication() { item(Main.collection.rawValue, 1) }
    mutating func endCollection() { item(Main.endCollection.rawValue, nil) }

    /// `count` padding bits, as a constant input; nothing for none.
    mutating func paddingInput(bits: Int) {
        guard bits > 0 else { return }
        reportSize(bits)
        reportCount(1)
        input(.padding)
    }

    /// `count` bytes of a vendor defined input (`page` 0xff00 and up).
    mutating func vendorInput(page: Int, usage usageID: Int, bytes count: Int) {
        usagePage(page)
        usage(usageID)
        logicalMinimum(0)
        logicalMaximum(255)
        reportSize(8)
        reportCount(count)
        input(.value)
    }

    mutating func vendorOutput(page: Int, usage usageID: Int, bytes count: Int) {
        usagePage(page)
        usage(usageID)
        logicalMinimum(0)
        logicalMaximum(255)
        reportSize(8)
        reportCount(count)
        output(.value)
    }

    mutating func vendorFeature(page: Int, usage usageID: Int, bytes count: Int) {
        usagePage(page)
        usage(usageID)
        logicalMinimum(0)
        logicalMaximum(255)
        reportSize(8)
        reportCount(count)
        feature(.value)
    }
}

// MARK: Names

enum GamepadNames {
    /// A controller's own name as a guest device's: printable, without
    /// leading or trailing space, at most 127 bytes of UTF-8, and "Game
    /// Controller" when nothing is left.
    static func deviceName(_ name: String) -> String {
        var cleaned = ""
        for scalar in name.unicodeScalars where !scalar.properties.generalCategory.isControlOrFormat {
            cleaned.unicodeScalars.append(scalar)
        }
        cleaned = cleaned.trimmingCharacters(in: .whitespaces)
        var result = ""
        var size = 0
        for character in cleaned {
            let width = String(character).utf8.count
            guard size + width <= GamepadProtocol.nameSize - 1 else { break }
            result.append(character)
            size += width
        }
        return result.isEmpty ? "Game Controller" : result
    }
}

private extension Unicode.GeneralCategory {
    var isControlOrFormat: Bool {
        switch self {
        case .control, .format, .surrogate, .privateUse, .unassigned, .lineSeparator, .paragraphSeparator: true
        default: false
        }
    }
}
