// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The orientation of the device as Android's device-orientation sensor
/// reports it: the number of `Surface.ROTATION_*`, which the window manager
/// takes as the rotation to propose. The cases say how the device is turned
/// from its default (upright portrait) position, which is what the sensor's
/// definition says (types.hal, DEVICE_ORIENTATION): 1 has the device's x axis
/// vertical and pointing up, 3 has it pointing down.
public enum GuestOrientation: UInt8, CaseIterable, Sendable {
    case upright = 0
    case turnedCounterClockwise = 1
    case upsideDown = 2
    case turnedClockwise = 3

    /// The orientation iOS reports, from `UIDeviceOrientation`'s own numbers
    /// (`DeviceOrientationReading`). Face up, face down and unknown are no
    /// orientation: the guest keeps the last one it was given.
    ///
    /// iOS names a landscape orientation by where the home indicator is, so
    /// `landscapeLeft` is the device turned counter-clockwise (the top
    /// edge pointing left, gravity along -x in Core Motion's device frame) and
    /// `landscapeRight` the device turned clockwise (gravity along +x). That is
    /// the opposite of the interface orientation of the same name, which the
    /// interface takes to keep the content upright; the reported value is
    /// the device's own, as the sensor's definition is.
    public init?(_ reading: DeviceOrientationReading) {
        switch reading {
        case .portrait: self = .upright
        case .landscapeLeft: self = .turnedCounterClockwise
        case .portraitUpsideDown: self = .upsideDown
        case .landscapeRight: self = .turnedClockwise
        case .unknown, .faceUp, .faceDown: return nil
        }
    }
}

/// `UIDeviceOrientation`, which `AndariumCore` cannot import: the case numbers
/// are UIKit's (hosted tests check them against it).
public enum DeviceOrientationReading: Int, CaseIterable, Sendable {
    case unknown = 0
    case portrait = 1
    case portraitUpsideDown = 2
    case landscapeLeft = 3
    case landscapeRight = 4
    case faceUp = 5
    case faceDown = 6
}

/// The record that carries an orientation to the guest on the motion
/// connection (vsock 5556): 20 bytes, little endian, "ANS1", the Android sensor
/// type 27 (DEVICE_ORIENTATION), the orientation as a float32 (0, 1, 2 or 3),
/// and two zero float32 values. The guest takes one only after it said it has
/// the sensor (`MotionDemand.acceptsOrientation`); an image without the sensor
/// ends the connection on one.
public enum OrientationRecord {
    public static let sensorType: UInt32 = 27

    public static func encoded(_ orientation: GuestOrientation) -> Data {
        var data = Data([0x41, 0x4e, 0x53, 0x31])
        for word in [sensorType, Float(orientation.rawValue).bitPattern, 0, 0] as [UInt32] {
            var little = word.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        return data
    }
}
