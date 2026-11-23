// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// How Core Motion's field becomes the guest's: the status, and which fields
/// can be sent. The field itself is the device's, as the SDK states it.
struct MagneticFieldTests {
    @Test func calibrationAccuracyBecomesAndroidsStatusPlusOne() {
        // Core Motion: uncalibrated -1, low 0, medium 1, high 2.
        // Android: unreliable 0, low 1, medium 2, high 3.
        #expect(MagneticField.status(accuracyRawValue: -1) == 0)
        #expect(MagneticField.status(accuracyRawValue: 0) == 1)
        #expect(MagneticField.status(accuracyRawValue: 1) == 2)
        #expect(MagneticField.status(accuracyRawValue: 2) == 3)
        // A value the SDK does not have is brought into the range.
        #expect(MagneticField.status(accuracyRawValue: -7) == 0)
        #expect(MagneticField.status(accuracyRawValue: 3) == 3)
        #expect(MagneticField.status(accuracyRawValue: Int.max - 1) == 3)
    }

    @Test func aFieldCanBeSentOnlyIfItsComponentsAreFiniteOnTheWire() {
        #expect(MagneticField.isUsable(x: 52.68, y: 0.62, z: -44.58))
        #expect(MagneticField.isUsable(x: 0, y: -0.0, z: 1e30))
        for bad in [Double.nan, .infinity, -.infinity, 1e39, -1e39, .greatestFiniteMagnitude] {
            #expect(!MagneticField.isUsable(x: bad, y: 1, z: 1), "\(bad)")
            #expect(!MagneticField.isUsable(x: 1, y: bad, z: 1), "\(bad)")
            #expect(!MagneticField.isUsable(x: 1, y: 1, z: bad), "\(bad)")
        }
    }

    @Test func aReadingKnowsWhetherItCanBeSent() {
        #expect(MotionSample(sensor: .magnetometer, x: 1, y: 2, z: 3, status: 3).isUsable)
        #expect(!MotionSample(sensor: .magnetometer, x: .nan, y: 2, z: 3).isUsable)
        #expect(!MotionSample(sensor: .magnetometer, x: 1e39, y: 2, z: 3).isUsable)
        #expect(MotionSample(sensor: .magnetometer, x: .nan, y: 2, z: 3).encoded == nil)
    }
}
