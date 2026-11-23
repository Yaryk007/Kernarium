// SPDX-License-Identifier: Apache-2.0
//
// What the framework may be told about a position, and when. The rules are the
// project's, not the receiver's, so they live here and not in the stream:
//
//  - A fix the host marked approximate (iOS's reduced accuracy) reaches an
//    application as a GNSS fix that is at least kApproximateAccuracyFloorM
//    uncertain, with no altitude, speed or bearing, whatever the stream
//    carries. The NMEA sentences an application can listen to are held to the
//    same rule.
//  - A precise fix keeps what the host knew. A field the host did not know is
//    absent, never zero. The altitude is the ellipsoidal one Android defines
//    (GGA's sea level altitude plus its geoid separation); with only one of the
//    two the fix has no altitude, since the datum would be wrong.
//  - A fix is reported once, when its time is newer than the last one reported
//    and the interval the framework asked for has passed. The receiver repeats a
//    held fix with its own time, so a repeat is not a new fix.
//
// The header has no Android dependency, so a host test compiles it.
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nmea.hpp"

namespace andarium::gnss {

// Q1 of the location design: iOS describes an approximate position as about
// 5 km; Android's own coarse location is 2 km. Never a smaller circle.
constexpr double kApproximateAccuracyFloorM = 2000.0;
// The uncertainty of the fix's time against the guest's clock, which is the
// host's: the stream's hundredths of a second.
constexpr double kTimeUncertaintyNs = 10'000'000.0;
// A report may come this much early: the host's epochs are a period apart but
// not to the millisecond.
constexpr double kIntervalTolerance = 0.9;

// The flags of android.hardware.gnss.GnssLocation and ElapsedRealtime. The AIDL
// is frozen, and Gnss.cpp checks each value against the generated constants.
enum LocationFlag : int32_t {
    kHasLatLong = 0x0001,
    kHasAltitude = 0x0002,
    kHasSpeed = 0x0004,
    kHasBearing = 0x0008,
    kHasHorizontalAccuracy = 0x0010,
    kHasVerticalAccuracy = 0x0020,
    kHasSpeedAccuracy = 0x0040,
    kHasBearingAccuracy = 0x0080,
};
enum ElapsedRealtimeFlag : int32_t {
    kHasTimestampNs = 1 << 0,
    kHasTimeUncertaintyNs = 1 << 1,
};

struct Location {
    int32_t flags = 0;
    double latitude = 0;
    double longitude = 0;
    double altitude = 0;
    double speed = 0;
    double bearing = 0;
    double horizontal_accuracy = 0;
    double vertical_accuracy = 0;
    double speed_accuracy = 0;
    double bearing_accuracy = 0;
    int64_t timestamp_ms = 0;
    int32_t elapsed_flags = 0;
    int64_t elapsed_realtime_ns = 0;
    double elapsed_uncertainty_ns = 0;
};

namespace detail {

inline std::optional<double> usable(const std::optional<double>& value) {
    if (!value || !std::isfinite(*value) || *value < 0) return std::nullopt;
    return value;
}

inline std::string fixed(double value, int decimals) {
    char buffer[48];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                            std::chars_format::fixed, decimals);
    return error == std::errc() ? std::string(buffer, end) : std::string();
}

// `value` rounded up to `decimals` places, for a bound that must hold after
// the rounding.
inline std::string fixed_up(double value, int decimals) {
    double scale = 1;
    for (int place = 0; place < decimals; ++place) scale *= 10;
    return fixed(std::ceil(value * scale) / scale, decimals);
}

// Sets `fields[index]` to the larger of its number and `minimum`, when the field
// holds a number. An empty field stays empty: the stream does not claim an
// accuracy the application could then compare.
inline void raise_to(std::vector<std::string>& fields, std::size_t index, double minimum, int decimals) {
    if (index >= fields.size()) return;
    const auto current = nmea::number(fields[index]);
    if (current && *current < minimum) fields[index] = fixed_up(minimum, decimals);
}

inline void clear(std::vector<std::string>& fields, std::initializer_list<std::size_t> indices) {
    for (const std::size_t index : indices) {
        if (index < fields.size()) fields[index].clear();
    }
}

}  // namespace detail

// The location the framework gets for `position`, or nothing when the position
// is no fix or cannot be reported. `realtime_ns` and `boottime_ns` are the
// guest's CLOCK_REALTIME and CLOCK_BOOTTIME now, read together: the fix's age
// is the difference between its UTC time and the first, laid on the second.
inline std::optional<Location> make_location(const nmea::Position& position, int64_t realtime_ns,
                                             int64_t boottime_ns) {
    using nmea::FixClass;
    if (position.fix_class == FixClass::none || !std::isfinite(position.latitude) ||
        !std::isfinite(position.longitude)) {
        return std::nullopt;
    }
    Location location;
    location.latitude = position.latitude;
    location.longitude = position.longitude;
    location.flags = kHasLatLong;

    const auto horizontal = detail::usable(position.horizontal_accuracy);
    if (position.fix_class == FixClass::approximate) {
        location.horizontal_accuracy = std::max(horizontal.value_or(0.0), kApproximateAccuracyFloorM);
        location.flags |= kHasHorizontalAccuracy;
    } else {
        // The framework refuses a location without an accuracy.
        if (!horizontal) return std::nullopt;
        location.horizontal_accuracy = *horizontal;
        location.flags |= kHasHorizontalAccuracy;
        if (position.altitude && position.geoid_separation && std::isfinite(*position.altitude) &&
            std::isfinite(*position.geoid_separation)) {
            location.altitude = *position.altitude + *position.geoid_separation;
            location.flags |= kHasAltitude;
            if (const auto vertical = detail::usable(position.vertical_accuracy)) {
                location.vertical_accuracy = *vertical;
                location.flags |= kHasVerticalAccuracy;
            }
        }
        if (const auto speed = detail::usable(position.speed)) {
            location.speed = *speed;
            location.flags |= kHasSpeed;
            if (const auto accuracy = detail::usable(position.speed_accuracy)) {
                location.speed_accuracy = *accuracy;
                location.flags |= kHasSpeedAccuracy;
            }
        }
        if (position.course && std::isfinite(*position.course) && *position.course >= 0 &&
            *position.course < 360.0) {
            location.bearing = *position.course;
            location.flags |= kHasBearing;
            if (const auto accuracy = detail::usable(position.course_accuracy)) {
                location.bearing_accuracy = *accuracy;
                location.flags |= kHasBearingAccuracy;
            }
        }
    }

    location.timestamp_ms = position.utc_ms;
    // The guest's wall clock is the host's, so the fix's age is how far its time
    // lies behind the clock. A fix from the future (a clock that stepped back)
    // is as old as nothing, and a fix older than the guest's uptime starts at the
    // first nanosecond: no field is zero for "unknown".
    int64_t age_ns = realtime_ns - position.utc_ms * 1'000'000;
    if (age_ns < 0) age_ns = 0;
    location.elapsed_realtime_ns = std::max<int64_t>(boottime_ns - age_ns, 1);
    location.elapsed_flags = kHasTimestampNs | kHasTimeUncertaintyNs;
    location.elapsed_uncertainty_ns = kTimeUncertaintyNs;
    return location;
}

// The sentences an NMEA listener may see for `epoch`. A precise epoch and a
// no-fix epoch pass as written. For an approximate one each sentence is
// rewritten to what make_location would report (no altitude, speed or course,
// accuracies at the floor, fix type 2), and a sentence this code does not know
// is left out, because it cannot be vouched for.
inline std::vector<std::string> listener_sentences(const nmea::Epoch& epoch) {
    if (epoch.position.fix_class != nmea::FixClass::approximate) return epoch.sentences;
    std::vector<std::string> out;
    for (const std::string& sentence : epoch.sentences) {
        const auto body = nmea::sentence_body(sentence);
        if (!body) continue;
        const auto views = nmea::detail::split(*body);
        std::vector<std::string> fields(views.begin(), views.end());
        const std::string& id = fields[0];
        const std::string type = id.size() == 5 ? id.substr(2) : std::string();
        // The kind of fix is the approximate one wherever a sentence names it.
        const auto name_estimate = [&fields](std::size_t index, const char* letter) {
            if (index < fields.size() && !fields[index].empty()) fields[index] = letter;
        };
        if (id == "PKRNF") {
            name_estimate(1, "A");
            detail::raise_to(fields, 2, kApproximateAccuracyFloorM, 1);
            detail::clear(fields, {3, 4, 5});
        } else if (type == "GGA") {
            name_estimate(6, "6");
            detail::clear(fields, {9, 10, 11, 12});
        } else if (type == "RMC") {
            name_estimate(12, "E");
            detail::clear(fields, {7, 8, 10, 11});
        } else if (type == "GSA") {
            if (fields.size() > 2 && fields[2] == "3") fields[2] = "2";
        } else if (type == "GST") {
            // Each axis carries the radial error over the square root of two.
            const double axis = kApproximateAccuracyFloorM / std::sqrt(2.0);
            for (const std::size_t index : {2, 3, 4, 6, 7}) {
                detail::raise_to(fields, index, axis, 2);
            }
            detail::clear(fields, {8});
        } else if (type == "VTG") {
            name_estimate(9, "E");
            detail::clear(fields, {1, 3, 5, 7});
        } else {
            continue;
        }
        std::string joined;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (index != 0) joined.push_back(',');
            joined += fields[index];
        }
        out.push_back(nmea::sentence_text(joined));
    }
    return out;
}

// Decides which epochs become reports.
class ReportPacer {
public:
    // True when a position with `utc_ms` is to be reported at `boottime_ns`,
    // given the framework's `interval_ms`. The decision is recorded.
    bool admit(int64_t utc_ms, int64_t boottime_ns, int interval_ms) {
        if (reported_ && utc_ms <= last_utc_ms_) return false;
        if (reported_ &&
            static_cast<double>(boottime_ns - last_boottime_ns_) <
                kIntervalTolerance * static_cast<double>(interval_ms) * 1e6) {
            return false;
        }
        reported_ = true;
        last_utc_ms_ = utc_ms;
        last_boottime_ns_ = boottime_ns;
        return true;
    }

    void reset() { reported_ = false; }

private:
    bool reported_ = false;
    int64_t last_utc_ms_ = 0;
    int64_t last_boottime_ns_ = 0;
};

}  // namespace andarium::gnss
