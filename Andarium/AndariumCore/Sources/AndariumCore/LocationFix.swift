// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How far the system lets Andarium use this device's location, in the terms
/// the Location row and the session need. The application reads it from iOS.
public enum LocationAuthorization: Equatable, Sendable {
    /// iOS has not asked the person yet.
    case notDetermined
    /// Location Services are off for the whole device.
    case servicesOff
    /// Something other than the person forbids it (Screen Time, a profile).
    case restricted
    /// The person refused it, or took it away in Settings.
    case denied
    /// Allowed while Andarium is in use, with the position as it is.
    case precise
    /// Allowed while Andarium is in use, but Precise Location is off for it:
    /// iOS reduces every position to an area.
    case approximate

    /// Positions can be read.
    public var allowsLocating: Bool { self == .precise || self == .approximate }

    /// Positions are reduced to an area.
    public var isApproximate: Bool { self == .approximate }

    /// Nothing can be read and the person has no question left to answer.
    public var refuses: Bool { self == .servicesOff || self == .restricted || self == .denied }
}

/// A position the system read, in the system's own terms: a negative accuracy,
/// speed or course says the value is not valid.
public struct LocationReading: Equatable, Sendable {
    public var timestamp: Date
    public var latitude: Double
    public var longitude: Double
    /// Meters above sea level.
    public var altitude: Double
    /// Meters above the WGS 84 ellipsoid.
    public var ellipsoidalAltitude: Double
    /// A negative value says the latitude and longitude are not valid.
    public var horizontalAccuracy: Double
    /// A negative value says the altitudes are not valid.
    public var verticalAccuracy: Double
    public var speed: Double
    public var speedAccuracy: Double
    public var course: Double
    public var courseAccuracy: Double

    public init(
        timestamp: Date, latitude: Double, longitude: Double, altitude: Double, ellipsoidalAltitude: Double,
        horizontalAccuracy: Double, verticalAccuracy: Double, speed: Double, speedAccuracy: Double,
        course: Double, courseAccuracy: Double
    ) {
        self.timestamp = timestamp
        self.latitude = latitude
        self.longitude = longitude
        self.altitude = altitude
        self.ellipsoidalAltitude = ellipsoidalAltitude
        self.horizontalAccuracy = horizontalAccuracy
        self.verticalAccuracy = verticalAccuracy
        self.speed = speed
        self.speedAccuracy = speedAccuracy
        self.course = course
        self.courseAccuracy = courseAccuracy
    }
}

/// One position for the guest's GNSS receiver. A field the system did not
/// give, or gave as invalid, is absent: it is left out of the guest's stream
/// and never sent as zero.
public struct LocationFix: Equatable, Sendable {
    /// The position's own time: UTC, in nanoseconds since 1970-01-01.
    public var utcNanoseconds: Int64
    /// WGS 84, in degrees.
    public var latitude: Double
    public var longitude: Double
    /// iOS reduced the position to an area (Precise Location is off); the
    /// guest's receiver says so in its stream.
    public var approximate: Bool
    public var altitude: Double?
    public var ellipsoidalAltitude: Double?
    public var horizontalAccuracy: Double?
    public var verticalAccuracy: Double?
    public var speed: Double?
    public var speedAccuracy: Double?
    public var course: Double?
    public var courseAccuracy: Double?

    public init(
        utcNanoseconds: Int64, latitude: Double, longitude: Double, approximate: Bool,
        altitude: Double? = nil, ellipsoidalAltitude: Double? = nil, horizontalAccuracy: Double? = nil,
        verticalAccuracy: Double? = nil, speed: Double? = nil, speedAccuracy: Double? = nil,
        course: Double? = nil, courseAccuracy: Double? = nil
    ) {
        self.utcNanoseconds = utcNanoseconds
        self.latitude = latitude
        self.longitude = longitude
        self.approximate = approximate
        self.altitude = altitude
        self.ellipsoidalAltitude = ellipsoidalAltitude
        self.horizontalAccuracy = horizontalAccuracy
        self.verticalAccuracy = verticalAccuracy
        self.speed = speed
        self.speedAccuracy = speedAccuracy
        self.course = course
        self.courseAccuracy = courseAccuracy
    }

    /// The fix a reading makes, tagged `approximate` when iOS's accuracy
    /// authorization was reduced as the reading was delivered; nil when the
    /// reading holds no usable position (its horizontal accuracy is negative
    /// or a coordinate is out of range). Each other field is kept only
    /// while it is valid.
    public init?(_ reading: LocationReading, approximate: Bool) {
        guard reading.horizontalAccuracy >= 0, reading.horizontalAccuracy.isFinite,
            reading.latitude.isFinite, abs(reading.latitude) <= 90,
            reading.longitude.isFinite, abs(reading.longitude) <= 180
        else { return nil }
        let seconds = reading.timestamp.timeIntervalSince1970
        guard seconds.isFinite, abs(seconds) < 9e9 else { return nil }
        // Whole seconds and the fraction apart: the product of the two would
        // lose the low digits of a time this large.
        let whole = seconds.rounded(.down)
        let nanoseconds = Int64(whole) * 1_000_000_000 + Int64(((seconds - whole) * 1e9).rounded())
        self.init(
            utcNanoseconds: nanoseconds, latitude: reading.latitude,
            longitude: reading.longitude, approximate: approximate,
            horizontalAccuracy: reading.horizontalAccuracy)
        if reading.verticalAccuracy >= 0, reading.verticalAccuracy.isFinite {
            verticalAccuracy = reading.verticalAccuracy
            altitude = Self.finite(reading.altitude)
            ellipsoidalAltitude = Self.finite(reading.ellipsoidalAltitude)
        }
        if reading.speed >= 0, reading.speed.isFinite {
            speed = reading.speed
            speedAccuracy = Self.valid(reading.speedAccuracy)
        }
        if reading.course >= 0, reading.course.isFinite {
            course = reading.course
            courseAccuracy = Self.valid(reading.courseAccuracy)
        }
    }

    private static func finite(_ value: Double) -> Double? { value.isFinite ? value : nil }

    private static func valid(_ accuracy: Double) -> Double? { accuracy >= 0 && accuracy.isFinite ? accuracy : nil }
}
