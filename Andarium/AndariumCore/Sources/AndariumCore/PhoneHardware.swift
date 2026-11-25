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

/// The cutout Android lays its status bar and its applications out around, and
/// where the clock and the icons stand beside it. The image's own tall cutout
/// overlay needs no root, but is one size; the overlays made here fit the phone
/// and only root may make them. Once made they stay in the guest, and the shell
/// may switch them in a start without root too.
public struct DisplayCutoutEmulation: Equatable, Sendable {
    public static let stockOverlay = "com.android.internal.display.cutout.emulation.tall"
    static let cutoutName = "AndariumNotchCutout"
    static let paddingName = "AndariumNotchPadding"
    /// The guest's own note of what the overlays were made for.
    static let marker = "andarium_notch"

    let housing: PhoneHardware.Housing
    /// The height of the housing's row in points, which are guest dp.
    let height: Int

    public init(housing: PhoneHardware.Housing, height: Int) {
        self.housing = housing
        self.height = min(max(height, 24), 100)
    }

    /// The housing's width, in dp.
    var width: Int {
        switch housing {
        case .wideNotch: 210
        case .notch: 162
        case .island: 126
        case .smallIsland: 90
        }
    }

    /// How far the clock and the icons stand from the screen's edges, in dp:
    /// close to them beside the wide notch, and nearer the housing the more
    /// room there is beside it.
    var padding: Int {
        switch housing {
        case .wideNotch: 12
        case .notch, .island: 24
        case .smallIsland: 34
        }
    }

    private var signature: String { "1-\(width)-\(height)-\(padding)" }

    /// What the shell runs at each start with the notch space: the overlays
    /// made for this phone where the guest has them, else the stock cutout.
    public var enableCommand: String {
        "{ [ \"$(settings get global \(Self.marker))\" = '\(signature)' ] && \(Self.enable)"
            + " && cmd overlay disable --user 0 \(Self.stockOverlay); } >/dev/null 2>&1"
            + " || cmd overlay enable-exclusive --user 0 --category \(Self.stockOverlay)"
    }

    /// What the shell runs at each start without it.
    public static var disableCommand: String {
        let overlays = ["com.android.shell:\(cutoutName)", "com.android.shell:\(paddingName)", stockOverlay]
        return "{ " + overlays.map { "cmd overlay disable --user 0 \($0); " }.joined() + "true; } >/dev/null 2>&1"
    }

    /// Whether the guest already has the overlays made for this phone.
    public var checkCommand: String { "[ \"$(settings get global \(Self.marker))\" = '\(signature)' ]" }

    /// What a rooted start runs when it does not. Magisk asks the person
    /// whether the shell may have root, and waits for the answer.
    public var rootCommand: String {
        let half = width / 2
        let path = "M -\(half),0 L -\(half),\(height) L \(half),\(height) L \(half),0 Z @dp"
        // TypedValue's complex form of whole dp: the mantissa, radix 23p0, dp.
        let dimension = "0x" + String(padding << 8 | 1, radix: 16)
        let commands = [
            "cmd overlay fabricate --target android --name \(Self.cutoutName) "
                + "android:string/config_mainBuiltInDisplayCutout 0x03 \"\(path)\"",
            "cmd overlay fabricate --target android --name \(Self.paddingName) "
                + "android:dimen/rounded_corner_content_padding 0x05 \(dimension)",
            "cmd overlay disable --user 0 \(Self.stockOverlay)",
            Self.enable,
            "settings put global \(Self.marker) \(signature)",
        ]
        return "su -c '" + commands.joined(separator: " && ") + "'"
    }

    private static let enable =
        "cmd overlay enable --user 0 com.android.shell:\(cutoutName)"
        + " && cmd overlay enable --user 0 com.android.shell:\(paddingName)"
}
