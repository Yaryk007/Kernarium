// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The iPhone Andarium runs on, from the hardware's own name: what Android
/// says the device is, and how much of the status bar's row its sensor housing
/// takes.
public struct PhoneHardware: Equatable, Sendable {
    public enum Housing: String, Sendable {
        /// iPhone X through 12: the wide notch.
        case wideNotch
        /// iPhone 13 and 14, 16e and 17e: the narrower notch.
        case notch
        /// iPhone 14 Pro through 17 and Air.
        case island
        /// iPhone 18 Pro and Pro Max.
        case smallIsland
    }

    public let name: String
    public let chip: String
    /// Nil on a phone with a home button.
    public let housing: Housing?

    /// The phone `uname` calls `machine` ("iPhone18,1"); nil for an iPad, the
    /// Simulator and a phone this table does not know, which keep Android's
    /// own name.
    public init?(machine: String) {
        guard let phone = Self.phones[machine] else { return nil }
        self = phone
    }

    private init(_ name: String, _ chip: String, _ housing: Housing?) {
        self.name = name
        self.chip = chip
        self.housing = housing
    }

    /// What Android's properties say of the device, as the runtime takes them
    /// (`android_properties`). With the notch space the painted cutout goes:
    /// the phone's own housing is there.
    public func properties(notchSpace: Bool) -> String {
        var lines = [
            "ro.product.manufacturer=Apple",
            "ro.product.model=\(name)",
            "ro.soc.manufacturer=Apple",
            "ro.soc.model=\(chip)",
        ]
        if notchSpace { lines.append("debug.disable_screen_decorations=true") }
        return lines.map { $0 + "\n" }.joined()
    }

    /// Names the device in Android after the phone, unless the person gave it
    /// a name of their own.
    public var deviceNameCommand: String {
        "case \"$(settings get global device_name)\" in "
            + "null|''|'Andarium Virtual Device') settings put global device_name '\(name)';; esac"
    }

    private static let phones: [String: PhoneHardware] = {
        var all: [String: PhoneHardware] = [:]
        func add(_ name: String, _ chip: String, _ housing: Housing?, _ machines: String...) {
            for machine in machines { all["iPhone" + machine] = PhoneHardware(name, chip, housing) }
        }
        add("iPhone X", "A11 Bionic", .wideNotch, "10,3", "10,6")
        add("iPhone XS", "A12 Bionic", .wideNotch, "11,2")
        add("iPhone XS Max", "A12 Bionic", .wideNotch, "11,4", "11,6")
        add("iPhone XR", "A12 Bionic", .wideNotch, "11,8")
        add("iPhone 11", "A13 Bionic", .wideNotch, "12,1")
        add("iPhone 11 Pro", "A13 Bionic", .wideNotch, "12,3")
        add("iPhone 11 Pro Max", "A13 Bionic", .wideNotch, "12,5")
        add("iPhone SE (2nd generation)", "A13 Bionic", nil, "12,8")
        add("iPhone 12 mini", "A14 Bionic", .wideNotch, "13,1")
        add("iPhone 12", "A14 Bionic", .wideNotch, "13,2")
        add("iPhone 12 Pro", "A14 Bionic", .wideNotch, "13,3")
        add("iPhone 12 Pro Max", "A14 Bionic", .wideNotch, "13,4")
        add("iPhone 13 mini", "A15 Bionic", .notch, "14,4")
        add("iPhone 13", "A15 Bionic", .notch, "14,5")
        add("iPhone 13 Pro", "A15 Bionic", .notch, "14,2")
        add("iPhone 13 Pro Max", "A15 Bionic", .notch, "14,3")
        add("iPhone SE (3rd generation)", "A15 Bionic", nil, "14,6")
        add("iPhone 14", "A15 Bionic", .notch, "14,7")
        add("iPhone 14 Plus", "A15 Bionic", .notch, "14,8")
        add("iPhone 14 Pro", "A16 Bionic", .island, "15,2")
        add("iPhone 14 Pro Max", "A16 Bionic", .island, "15,3")
        add("iPhone 15", "A16 Bionic", .island, "15,4")
        add("iPhone 15 Plus", "A16 Bionic", .island, "15,5")
        add("iPhone 15 Pro", "A17 Pro", .island, "16,1")
        add("iPhone 15 Pro Max", "A17 Pro", .island, "16,2")
        add("iPhone 16", "A18", .island, "17,3")
        add("iPhone 16 Plus", "A18", .island, "17,4")
        add("iPhone 16 Pro", "A18 Pro", .island, "17,1")
        add("iPhone 16 Pro Max", "A18 Pro", .island, "17,2")
        add("iPhone 16e", "A18", .notch, "17,5")
        add("iPhone 17", "A19", .island, "18,3")
        add("iPhone 17 Pro", "A19 Pro", .island, "18,1")
        add("iPhone 17 Pro Max", "A19 Pro", .island, "18,2")
        add("iPhone Air", "A19 Pro", .island, "18,4")
        add("iPhone 17e", "A19", .notch, "18,5")
        add("iPhone 18 Pro", "A20 Pro", .smallIsland, "19,2")
        add("iPhone 18 Pro Max", "A20 Pro", .smallIsland, "19,3", "19,7")
        return all
    }()
}

/// Home's "Notch space": Android's display also takes the row of the sensor
/// housing, and Android is told of a cutout there. Off until the person turns
/// it on; it applies when Android next starts.
public struct NotchSpacePreference {
    public static let key = "andarium.notchSpace"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public var isOn: Bool { defaults.bool(forKey: Self.key) }

    public func keep(_ on: Bool) { defaults.set(on, forKey: Self.key) }
}

/// The cutout Android lays its status bar and its applications out around: the
/// image's own tall cutout overlay, which the shell may switch. Its 48 dp are
/// the status bar's height too.
public enum DisplayCutoutEmulation {
    public static let overlay = "com.android.internal.display.cutout.emulation.tall"

    public static func command(on: Bool) -> String {
        on
            ? "cmd overlay enable-exclusive --user 0 --category \(overlay)"
            : "cmd overlay disable --user 0 \(overlay)"
    }
}
