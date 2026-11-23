// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The session's one notice that the frame rate is lowered while the device is
/// hot. `ThermalPolicy` decides when (once in a run, at the moment the cap
/// starts); this is what the screen shows.
///
/// It stays until the person dismisses it or `visibleFor` has passed, as the
/// notice that Android ran out of graphics memory does.
public struct ThermalNotice: Equatable, Sendable {
    /// How long a notice stays up when nobody dismisses it.
    public static let visibleFor = Duration.seconds(30)

    /// The words of the notice.
    public static let text = String(localized: "Frame rate lowered while this device is hot", bundle: .module)
    public static let dismissLabel = String(localized: "Dismiss", bundle: .module)

    /// A notice is up.
    public private(set) var isShowing = false

    public init() {}

    public mutating func show() { isShowing = true }

    public mutating func dismiss() { isShowing = false }
}
