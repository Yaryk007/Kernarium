// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Home's "120 Hz" in Performance: whether the next Android start asks
/// for a 120 Hz display. Off is the default and the display is what it has
/// always been. The choice lasts across launches. A display that is 120 Hz is
/// fixed at the start, so a change applies when Android next starts.
public struct HighRefreshPreference {
    /// The stored value's key in the defaults database; a missing value is the
    /// default, off.
    public static let key = "andarium.highRefresh"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last value kept; off until one is.
    public var isOn: Bool { defaults.object(forKey: Self.key) as? Bool ?? false }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// How a launch begins.
    public struct Launch: Equatable, Sendable {
        /// Whether the setting is on from the start.
        public var isOn: Bool
        /// Whether what the person sets from here on is kept. A harness's
        /// launch decides for its run alone, so what the person chose stays.
        public var keepsChanges: Bool
    }

    /// The start of a launch. One that states the value starts as stated. Any
    /// other harness launch starts with the default, so that a measured run
    /// does not inherit the person's choice; only a launch of the person's own
    /// restores the kept value.
    public func launch(automated: Bool, stated: Bool? = nil) -> Launch {
        if let stated { return Launch(isOn: stated, keepsChanges: false) }
        return automated ? Launch(isOn: false, keepsChanges: false) : Launch(isOn: isOn, keepsChanges: true)
    }
}

/// What the 120 Hz setting asks of the boot, from the device and the choice.
/// The runtime decides once, at the start, whether a run has the 120 Hz
/// display (`kernarium_runtime_statistics.display_refresh_hz`), and the
/// application uses what it granted; these are the conditions the offer and
/// the request go by, the same ones the runtime checks.
public enum HighRefresh {
    /// The slowest screen that can show the 120 Hz display: the display link
    /// asks for no more than 60 on one below it (the runtime's
    /// `highStepMinimumCeiling`), so a screen that shows 61 to 99 is not
    /// offered it.
    public static let minimumFramesPerSecond = 100

    /// Whether a display of `width` x `height` pixels can state 120 Hz in the
    /// guest's EDID: the one detailed timing the device writes holds its pixel
    /// clock in two bytes of tens of kilohertz, which the 12.9-inch and
    /// 13-inch iPad Pro at their native size overflow. The runtime's
    /// `edidCarries120Hz` is this rule; one table checks both.
    public static func carries(width: Int, height: Int) -> Bool {
        guard width > 0, height > 0, width <= maximumEdidDimension, height <= maximumEdidDimension else { return false }
        let horizontalBlank = min(maximumEdidDimension, max(160, width / 5))
        let verticalBlank = min(maximumEdidDimension, max(30, height / 20))
        let clock = ((width + horizontalBlank) * (height + verticalBlank) * 120 + 5000) / 10000
        return clock <= maximumDetailedTimingClock
    }

    private static let maximumEdidDimension = 4095
    private static let maximumDetailedTimingClock = 65535

    /// Whether Home offers the setting: the screen shows at least 100
    /// frames a second, the host is an iPhone or an iPad (not an iPad app on a
    /// Mac or on a Vision Pro), and the display the next start gives the guest
    /// is a size whose 120 Hz mode the EDID can state. A size not known yet
    /// leaves the question to the screen and the host; the runtime decides it
    /// at the start.
    public static func isOffered(
        maximumFramesPerSecond: Int, displaySize: (width: Int, height: Int)? = nil, hostIsMacOrVision: Bool = false
    ) -> Bool {
        guard !hostIsMacOrVision, maximumFramesPerSecond >= minimumFramesPerSecond else { return false }
        guard let displaySize else { return true }
        return carries(width: displaySize.width, height: displaySize.height)
    }

    /// Whether the boot asks for the 120 Hz display: the setting is on, and
    /// the offer holds. A device that cannot never asks, whatever a launch
    /// states.
    public static func asksAtBoot(
        isOn: Bool, maximumFramesPerSecond: Int, displaySize: (width: Int, height: Int)? = nil,
        hostIsMacOrVision: Bool = false
    ) -> Bool {
        isOn
            && isOffered(
                maximumFramesPerSecond: maximumFramesPerSecond, displaySize: displaySize,
                hostIsMacOrVision: hostIsMacOrVision)
    }

    /// Whether Android is held to the 60 Hz rate: the run was granted the 120
    /// Hz display, and what the screen shows is 60 at the most, because iOS is
    /// in Low Power Mode or the screen the window is on (an external display
    /// after a move) shows less than 100. A run with the 60 Hz display needs
    /// no hold.
    public static func holdsAtSixty(
        displayIs120: Bool, lowPowerMode: Bool, screenMaximumFramesPerSecond: Int
    ) -> Bool {
        displayIs120 && (lowPowerMode || screenMaximumFramesPerSecond < minimumFramesPerSecond)
    }

    /// "on" while the next start asks for the 120 Hz display, "off" while it
    /// does not, in the fixed English the session record and the feedback
    /// information use.
    public static func state(isOn: Bool) -> String { isOn ? "on" : "off" }
}
