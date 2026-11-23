// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Who owns the guest's display rotation.
///
/// Where the interface turns with the device (an iPad, and a display that
/// rotates like one) the host owns it throughout a session: the guest is held
/// at rotation zero, its display follows the scene and the sensor axes follow
/// the interface orientation. Where the interface is portrait-only (an
/// iPhone) the guest owns it. A session never asks iPadOS to lock the
/// interface, so no presentation, window or scene change flips the policy.
public struct SessionOrientationPolicy: Equatable, Sendable {
    public let hostDrivesGuestRotation: Bool

    /// `interfaceRotates` is whether the interface of the display the scene
    /// is on turns with the device; the caller decides it for each display.
    public init(phase: Phase, interfaceRotates: Bool) {
        let session: Bool = switch phase {
        case .booting, .running: true
        case .home, .failed: false
        }
        hostDrivesGuestRotation = session && interfaceRotates
    }
}
