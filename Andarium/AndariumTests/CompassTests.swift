// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CoreMotion
import Foundation
import Testing
@testable import Andarium

/// What the application does about the magnetic field where the hardware, or the
/// person's motion permission, may be missing: the boot request states the
/// compass only for a device that can give a field, the gate that says so, and
/// what Core Motion's handler may call a failure. The field's path through the
/// session is `MotionCompassTests` (AndariumCore); the readings of a real
/// device are checked on one.
struct CompassHostTests {
    @Test func theBootRequestStatesTheCompassOnlyForADeviceThatCanGiveAField() {
        let haptics = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS)
        let orientation = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION)
        let compass = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS)
        #expect(compass == 4)
        // A request that does not say the compass is today's.
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, followsDeviceOrientation: true) == orientation)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true, followsDeviceOrientation: true) == haptics | orientation)
        // Each of the three alone, and every combination.
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, compassAvailable: true) == compass)
        for h in [false, true] {
            for o in [false, true] {
                for c in [false, true] {
                    let expected = (h ? haptics : 0) | (o ? orientation : 0) | (c ? compass : 0)
                    #expect(
                        LiveRuntime.hostFeatures(hapticsAvailable: h, followsDeviceOrientation: o, compassAvailable: c)
                            == expected, "haptics \(h), orientation \(o), compass \(c)")
                }
            }
        }
    }

    /// Counts what an assessment reads, in order.
    private final class Reads: @unchecked Sendable {
        var order: [String] = []
    }

    private func assessed(
        _ magnetometer: Bool = true, _ deviceMotion: Bool = true, _ northFrame: Bool = true,
        _ authorization: CMAuthorizationStatus? = nil, reads: Reads = Reads()
    ) -> CompassSource.Assessment {
        CompassSource.assess(
            magnetometer: magnetometer, deviceMotion: deviceMotion,
            northFrame: {
                reads.order.append("frames")
                return northFrame
            },
            authorization: {
                reads.order.append("permission")
                return authorization
            })
    }

    @Test func theGuestIsOfferedAMagnetometerOnlyWhereEverythingItNeedsIsThere() {
        #expect(assessed().available)
        // The sensor, device motion and the magnetic-north frame are each needed.
        #expect(!assessed(false).available)
        #expect(!assessed(true, false).available)
        #expect(!assessed(true, true, false).available)
        // Before iOS 27.2 there is no permission to ask about.
        #expect(assessed(true, true, true, nil).available)
        #expect(assessed().permission == .notStated)
        // From 27.2: refused by the person or the system ends the offer; not yet
        // asked, or allowed, leaves it. Nothing is requested here either way.
        #expect(assessed(true, true, true, .notDetermined).available)
        #expect(assessed(true, true, true, .authorized).available)
        #expect(!assessed(true, true, true, .denied).available)
        #expect(!assessed(true, true, true, .restricted).available)
        #expect(assessed(true, true, true, .denied).permission == .status(.denied))
        // A refusal does not matter to a device that could not give a field anyway.
        #expect(!assessed(false, true, true, .authorized).available)
        for status in [CMAuthorizationStatus.notDetermined, .restricted, .denied, .authorized] {
            #expect(CompassSource.permits(status) == (status == .notDetermined || status == .authorized))
        }
    }

    @Test func aDeviceThatCannotHaveAFieldIsNotAskedAboutTheFramesOrThePermission() {
        // iOS on a Mac does not state either (the permission status is marked
        // unavailable on macOS): the question ends at the first two answers.
        for (magnetometer, deviceMotion) in [(false, false), (false, true), (true, false)] {
            let reads = Reads()
            let assessment = assessed(magnetometer, deviceMotion, reads: reads)
            #expect(reads.order.isEmpty, "magnetometer \(magnetometer), device motion \(deviceMotion)")
            #expect(assessment == CompassSource.Assessment())
            #expect(assessment.permission == .notRead && assessment.northFrame == nil)
        }
    }

    @Test func thePermissionIsReadOnlyAfterTheFramesAndOnlyIfTheyAreThere() {
        let reads = Reads()
        _ = assessed(true, true, true, .authorized, reads: reads)
        #expect(reads.order == ["frames", "permission"])
        let withoutFrame = Reads()
        let assessment = assessed(true, true, false, .authorized, reads: withoutFrame)
        #expect(withoutFrame.order == ["frames"])
        #expect(!assessment.available && assessment.northFrame == false && assessment.permission == .notRead)
    }

    @Test func theStatusHasWordsForALog() {
        #expect(CompassSource.name(.notRead) == "not read")
        #expect(CompassSource.name(.notStated) == "not stated before iOS 27.2")
        #expect(CompassSource.name(.status(.notDetermined)) == "notDetermined")
        #expect(CompassSource.name(.status(.restricted)) == "restricted")
        #expect(CompassSource.name(.status(.denied)) == "denied")
        #expect(CompassSource.name(.status(.authorized)) == "authorized")
    }

    @Test func onlyTheMovementNoticeIsNotAFailure() {
        // Core Motion's "the device must move to calibrate" (101), once, in the handler.
        let notice = NSError(domain: CMErrorDomain, code: Int(CMErrorDeviceRequiresMovement.rawValue))
        #expect(notice.code == 101)
        #expect(CompassSource.isCalibrationNotice(notice))
        // Every other code of Core Motion, and the same number from another domain, is one.
        for code in [CMErrorNULL, CMErrorTrueNorthNotAvailable, CMErrorUnknown, CMErrorNotAvailable,
                     CMErrorNotAuthorized, CMErrorNilData] {
            #expect(!CompassSource.isCalibrationNotice(NSError(domain: CMErrorDomain, code: Int(code.rawValue))), "\(code)")
        }
        #expect(!CompassSource.isCalibrationNotice(NSError(domain: NSPOSIXErrorDomain, code: 101)))
        #expect(!CompassSource.isCalibrationNotice(CancellationError()))
    }

    /// A device that has a field, or not, and nothing else.
    private final class FieldOnly: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = false
        let hasGyroscope = false
        let hasMagnetometer: Bool
        init(_ hasMagnetometer: Bool) { self.hasMagnetometer = hasMagnetometer }
        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) {}
        func stop(_ sensor: MotionSample.Sensor) {}
    }

    private struct NoGuest: GuestStreamOpener {
        func openStream() throws -> Int32 { throw POSIXError(.ECONNREFUSED) }
    }

    @Test func theApplicationOffersTheCompassWhereItsDeviceHasAField() {
        #expect(MotionSensors(opener: NoGuest(), device: FieldOnly(true)).compassAvailable)
        #expect(!MotionSensors(opener: NoGuest(), device: FieldOnly(false)).compassAvailable)
    }

    /// The Simulator and an iPad app on a Mac have no magnetometer: no bit, so no
    /// sensor the guest could list and nothing feeds.
    @Test func theSimulatorAndAMacAreNotOfferedACompass() {
        #if targetEnvironment(simulator)
        #expect(!MotionSensors().compassAvailable, "The Simulator has no magnetometer")
        #else
        if ProcessInfo.processInfo.isiOSAppOnMac { #expect(!MotionSensors().compassAvailable, "A Mac has no magnetometer") }
        #endif
    }
}

/// The development build's compass readout: asked for by the launch, one line
/// a period, and the line the compass check's driver reads.
struct CompassReadoutTests {
    @Test func aLaunchAsksForTheReadoutWithOneAndOnlyWithOne() {
        #expect(CompassReadoutLaunch.requested(in: ["ANDARIUM_COMPASS_READOUT": "1"]))
        #expect(!CompassReadoutLaunch.requested(in: [:]))
        #expect(!CompassReadoutLaunch.requested(in: ["ANDARIUM_COMPASS_READOUT": "0"]))
        #expect(CompassReadout.requested(environment: [:]) == nil)
        #expect(CompassReadout.requested(environment: ["ANDARIUM_COMPASS_READOUT": "1"]) != nil)
    }

    @Test func theReadoutPrintsTheFirstSampleAndThenOneAPeriodLater() {
        let readout = CompassReadout(period: 0.5)
        let field = CMMagneticField(x: 1, y: 2, z: 3)
        #expect(readout.observe(field, at: 100.0) != nil)
        #expect(readout.observe(field, at: 100.2) == nil)
        #expect(readout.observe(field, at: 100.499) == nil)
        #expect(readout.observe(field, at: 100.5) != nil)
        #expect(readout.observe(field, at: 100.6) == nil)
    }

    @Test func theSpanCoversEverySampleSinceTheLastLineAndStartsAgainAfterIt() {
        let readout = CompassReadout(period: 0.5)
        // The first line's extrema are its own sample.
        #expect(
            readout.observe(CMMagneticField(x: 1, y: -2, z: -40), at: 100.0)
                == CompassReadout.Extrema(count: 1, low: [1, -2, -40], high: [1, -2, -40]))
        // Samples between two lines are forwarded to the guest and not printed, but they are taken into the span.
        #expect(readout.observe(CMMagneticField(x: 3, y: -2.5, z: -39), at: 100.1) == nil)
        #expect(readout.observe(CMMagneticField(x: 0.5, y: -1, z: -41), at: 100.3) == nil)
        // The next line covers those two and its own sample, component by component.
        #expect(
            readout.observe(CMMagneticField(x: 2, y: -2, z: -40), at: 100.5)
                == CompassReadout.Extrema(count: 3, low: [0.5, -2.5, -41], high: [3, -1, -39]))
        // The span started again with that line: a sample after it is the whole of the next one.
        #expect(readout.observe(CMMagneticField(x: 9, y: 9, z: 9), at: 100.7) == nil)
        #expect(
            readout.observe(CMMagneticField(x: 8, y: 7, z: 6), at: 101.0)
                == CompassReadout.Extrema(count: 2, low: [8, 7, 6], high: [9, 9, 9]))
    }

    @Test func theLineCarriesTheFieldTheGravityTheAttitudeTheSpanAndTheMount() {
        let identity = CMRotationMatrix(m11: 1, m12: 0, m13: 0, m21: 0, m22: 1, m23: 0, m31: 0, m32: 0, m33: 1)
        let line = CompassReadout.line(
            timestamp: 12.5, mount: .landscapeLeft, accuracy: 2,
            field: CMMagneticField(x: 1.5, y: -2.25, z: -40),
            extrema: CompassReadout.Extrema(count: 1, low: [1.5, -2.25, -40], high: [1.5, -2.25, -40]),
            gravity: CMAcceleration(x: 0, y: 0, z: -1), attitude: identity, heading: 123.456)
        #expect(
            line == "Andarium: compass-check: t=12.500 mount=landscapeLeft acc=2 field=1.500,-2.250,-40.000 "
                + "grav=0.00000,0.00000,-1.00000 att=1.00000,0.00000,0.00000,0.00000,1.00000,0.00000,0.00000,0.00000,1.00000 "
                + "heading=123.46 fmin=1.500,-2.250,-40.000 fmax=1.500,-2.250,-40.000 n=1")
        // Nine different numbers, so that a transposed or shuffled matrix, or a swapped gravity axis, cannot print the same line.
        let rows = CMRotationMatrix(m11: 0.1, m12: 0.2, m13: 0.3, m21: 0.4, m22: 0.5, m23: 0.6, m31: 0.7, m32: 0.8, m33: 0.9)
        let distinct = CompassReadout.line(
            timestamp: 0.25, mount: .portrait, accuracy: -1,
            field: CMMagneticField(x: -7.125, y: 3.5, z: 21.75),
            extrema: CompassReadout.Extrema(count: 7, low: [-8, 3, 20.5], high: [-7, 4.25, 22]),
            gravity: CMAcceleration(x: 0.25, y: -0.5, z: -0.75), attitude: rows, heading: -1)
        #expect(
            distinct == "Andarium: compass-check: t=0.250 mount=portrait acc=-1 field=-7.125,3.500,21.750 "
                + "grav=0.25000,-0.50000,-0.75000 att=0.10000,0.20000,0.30000,0.40000,0.50000,0.60000,0.70000,0.80000,0.90000 "
                + "heading=-1.00 fmin=-8.000,3.000,20.500 fmax=-7.000,4.250,22.000 n=7")
    }
}
