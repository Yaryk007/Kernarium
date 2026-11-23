// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreLocation
import Foundation

/// The device's position, read from Core Location for the guest's GNSS
/// receiver. It reads only while the session says so (`LocationSession`), at
/// the best accuracy and with no distance filter, in the foreground, and never
/// asks for background access: no Always authorization, no background mode and
/// no background activity session.
@MainActor
final class HostLocation: LocationUpdates {
    let access: SystemLocationAccess
    /// Where each position goes (the session's `deliver`).
    var deliver: (@MainActor (LocationFix) -> Void)?

    init(access: SystemLocationAccess? = nil) {
        let access = access ?? SystemLocationAccess()
        self.access = access
        let manager = access.manager
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.distanceFilter = kCLDistanceFilterNone
        // A device that stands still must not stop the positions: the guest
        // repeats a precise one for ten seconds only.
        manager.pausesLocationUpdatesAutomatically = false
        access.delivered = { [weak self] locations in
            // The newest position stands for the batch.
            guard let self, let newest = locations.last else { return }
            let reduced = self.access.manager.accuracyAuthorization == .reducedAccuracy
            if let fix = Self.fix(from: newest, approximate: reduced) { deliver?(fix) }
        }
    }

    func start() {
        access.manager.startUpdatingLocation()
    }

    func stop() {
        access.manager.stopUpdatingLocation()
    }

    /// The fix a position makes, tagged approximate when iOS had reduced the
    /// accuracy as it delivered it.
    static func fix(from location: CLLocation, approximate: Bool) -> LocationFix? {
        LocationFix(
            LocationReading(
                timestamp: location.timestamp, latitude: location.coordinate.latitude,
                longitude: location.coordinate.longitude, altitude: location.altitude,
                ellipsoidalAltitude: location.ellipsoidalAltitude,
                horizontalAccuracy: location.horizontalAccuracy, verticalAccuracy: location.verticalAccuracy,
                speed: location.speed, speedAccuracy: location.speedAccuracy, course: location.course,
                courseAccuracy: location.courseAccuracy),
            approximate: approximate)
    }
}

/// The guest's GNSS receiver in the runtime: each fix goes in whole, and a
/// clear drops what the receiver holds. The runtime answers -ENODEV when no
/// guest with a receiver runs, which is not an error here.
@MainActor
final class RuntimeLocationSink: LocationSink {
    func report(_ fix: LocationFix) {
        var runtime = Self.runtimeFix(fix)
        _ = kernarium_runtime_loader_report_location(&runtime)
    }

    func clear() {
        _ = kernarium_runtime_loader_report_location(nil)
    }

    /// The runtime's fix for a position: a flag for each field that is valid.
    static func runtimeFix(_ fix: LocationFix) -> kernarium_runtime_location_fix {
        var runtime = kernarium_runtime_location_fix()
        runtime.struct_size = UInt32(MemoryLayout<kernarium_runtime_location_fix>.size)
        runtime.utc_ns = fix.utcNanoseconds
        runtime.latitude = fix.latitude
        runtime.longitude = fix.longitude
        var flags: UInt32 = fix.approximate ? UInt32(KERNARIUM_RUNTIME_LOCATION_APPROXIMATE) : 0
        func set(_ value: Double?, _ flag: Int, _ field: inout Double) {
            guard let value else { return }
            field = value
            flags |= UInt32(flag)
        }
        set(fix.altitude, Int(KERNARIUM_RUNTIME_LOCATION_HAS_ALTITUDE), &runtime.altitude)
        set(fix.ellipsoidalAltitude, Int(KERNARIUM_RUNTIME_LOCATION_HAS_ELLIPSOIDAL_ALTITUDE), &runtime.ellipsoidal_altitude)
        set(fix.horizontalAccuracy, Int(KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY), &runtime.horizontal_accuracy)
        set(fix.verticalAccuracy, Int(KERNARIUM_RUNTIME_LOCATION_HAS_VERTICAL_ACCURACY), &runtime.vertical_accuracy)
        set(fix.speed, Int(KERNARIUM_RUNTIME_LOCATION_HAS_SPEED), &runtime.speed)
        set(fix.speedAccuracy, Int(KERNARIUM_RUNTIME_LOCATION_HAS_SPEED_ACCURACY), &runtime.speed_accuracy)
        set(fix.course, Int(KERNARIUM_RUNTIME_LOCATION_HAS_COURSE), &runtime.course)
        set(fix.courseAccuracy, Int(KERNARIUM_RUNTIME_LOCATION_HAS_COURSE_ACCURACY), &runtime.course_accuracy)
        runtime.flags = flags
        return runtime
    }
}
