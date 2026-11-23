// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import UIKit

/// The device's orientation, from `UIDevice`, for as long as it is wanted.
///
/// It follows iOS's own rule: `UIDevice.orientation` turns with the device
/// while the person's Portrait Orientation Lock is off and stays portrait
/// while it is on, whatever the application supports, and the value always
/// agrees with its notification. So the guest turns when iOS would, and the
/// observer needs no sensor of its own.
///
/// `UIDevice` generates its notifications (and runs the accelerometer for them)
/// only between `begin` and `end`. The observer begins them when the
/// orientation is wanted (a session in front on a device whose guest follows
/// the orientation) and ends them when it is not, so nothing runs at Home or in
/// the background. The value is `unknown` until the first notification, which
/// arrives within moments of `begin`. Face up, face down and unknown are no
/// orientation: nothing is reported for them, and the guest keeps the last it
/// was given. When the observer stops it reports `nil`, so that what was seen
/// before is not given again as if it were current.
@MainActor
final class DeviceOrientationObserver {
    /// What the observer asks of the device and the notification centre; the
    /// application's is `.system`.
    struct Source {
        var begin: @MainActor () -> Void
        var end: @MainActor () -> Void
        var reading: @MainActor () -> DeviceOrientationReading
        var center: NotificationCenter
        var notification: Notification.Name

        static let system = Source(
            begin: { UIDevice.current.beginGeneratingDeviceOrientationNotifications() },
            end: { UIDevice.current.endGeneratingDeviceOrientationNotifications() },
            reading: { DeviceOrientationReading(rawValue: UIDevice.current.orientation.rawValue) ?? .unknown },
            center: .default,
            notification: UIDevice.orientationDidChangeNotification)
    }

    private let source: Source
    private let changed: @MainActor (GuestOrientation?) -> Void
    private var token: NSObjectProtocol?

    init(source: Source = .system, changed: @escaping @MainActor (GuestOrientation?) -> Void) {
        self.source = source
        self.changed = changed
    }

    var isObserving: Bool { token != nil }

    /// Starts or stops observing. A call that changes nothing does nothing.
    func observe(_ wanted: Bool) {
        guard wanted != isObserving else { return }
        if wanted {
            source.begin()
            token = source.center.addObserver(forName: source.notification, object: nil, queue: .main) {
                [weak self] _ in
                MainActor.assumeIsolated { self?.report() }
            }
            // The device may know its orientation already.
            report()
        } else {
            if let token { source.center.removeObserver(token) }
            token = nil
            source.end()
            changed(nil)
        }
    }

    private func report() {
        guard let orientation = GuestOrientation(source.reading()) else { return }
        changed(orientation)
    }
}
