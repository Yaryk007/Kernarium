// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Settings' "When hot" under Performance: whether Andarium tells Android how
/// hot the device is, so that Android limits the frame rate while it is hot
/// (`ThermalSession`). On, "Limit frame rate", is the default; off, "Full
/// frame rate", tells Android nothing hot at any state of iOS's. The choice
/// lasts across launches until the person turns the limit back on.
public struct HeatLimitPreference {
    /// The stored value's key in the defaults database; a missing value is the
    /// default, on.
    public static let key = "andarium.heatLimit"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; on until one is.
    public var isOn: Bool { defaults.object(forKey: Self.key) as? Bool ?? true }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    public struct Launch: Equatable, Sendable {
        /// Whether the frame rate limit is on from the start.
        public var isOn: Bool
        /// Whether what the person sets from here on is kept. A harness's
        /// launch decides for its run alone, so what the person chose stays.
        public var keepsChanges: Bool
    }

    /// The start of a launch. One that states the value starts as stated. Any
    /// other harness launch starts with the default, so that a run measured
    /// under heat does not inherit the person's choice; only a launch of the
    /// person's own restores the kept value.
    public func launch(automated: Bool, stated: Bool? = nil) -> Launch {
        if let stated { return Launch(isOn: stated, keepsChanges: false) }
        return automated ? Launch(isOn: true, keepsChanges: false) : Launch(isOn: isOn, keepsChanges: true)
    }
}

/// What the heat facts of a session say about the option, in the fixed English
/// the session record uses.
public enum HeatLimitWords {
    /// "on" while Android is told how hot the device is, "off" while it is not.
    public static func state(isOn: Bool) -> String { isOn ? "on" : "off" }
}
