// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How Core Motion's calibrated magnetic field becomes the reading the guest's
/// magnetometer reports (Android's `TYPE_MAGNETIC_FIELD`: microtesla, in the
/// device's own axes, calibrated, with a status).
///
/// The field is `CMDeviceMotion.magneticField` of device motion in the
/// magnetic-north frame, never the raw magnetometer, which carries the device's
/// own magnets, and never a heading. The SDK states it is the field "with
/// respect to the device", so it is the guest's as it is; the mount's
/// rotation is `MotionSample`'s.
public enum MagneticField {
    /// Android's `SensorStatus` for Core Motion's calibration accuracy, whose
    /// raw values are -1 (uncalibrated), 0 (low), 1 (medium) and 2 (high):
    /// unreliable 0, low 1, medium 2, high 3. Anything else is clamped.
    public static func status(accuracyRawValue: Int) -> UInt8 {
        UInt8(min(max(accuracyRawValue + 1, 0), 3))
    }

    /// Whether a field can be sent: each component is finite as the wire's
    /// float32 (a larger value is not a field).
    public static func isUsable(x: Double, y: Double, z: Double) -> Bool {
        Float(x).isFinite && Float(y).isFinite && Float(z).isFinite
    }
}
