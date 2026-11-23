// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "gnss/fix.hpp"

#include <cerrno>
#include <cmath>
#include <optional>

namespace kernarium::location {

namespace detail {

[[nodiscard]] inline std::optional<double> optionalField(
    const kernarium_runtime_location_fix& source,
    std::uint32_t flag,
    double value,
    double minimum,
    double limit)
{
    if ((source.flags & flag) == 0 || !std::isfinite(value) || value < minimum ||
        value >= limit)
        return std::nullopt;
    return value;
}

} // namespace detail

/* The runtime's fix as the GNSS device's. Returns 0, or -EINVAL for a struct
 * too small to hold the fields of this version, a time that is not after the
 * epoch, or a position outside the globe. A flagged optional field that
 * cannot be true (not a number, a negative accuracy or speed, a course
 * outside [0, 360)) is left out, never zeroed. */
[[nodiscard]] inline int toFix(
    const kernarium_runtime_location_fix& source,
    uml::gnss::Fix& fix)
{
    using detail::optionalField;
    constexpr double unbounded = 1e300;
    if (source.struct_size < sizeof(source) || source.utc_ns <= 0 ||
        !std::isfinite(source.latitude) || !std::isfinite(source.longitude) ||
        std::fabs(source.latitude) > 90.0 || std::fabs(source.longitude) > 180.0)
        return -EINVAL;
    fix = {};
    fix.utcNs = source.utc_ns;
    fix.latitude = source.latitude;
    fix.longitude = source.longitude;
    fix.approximate = (source.flags & KERNARIUM_RUNTIME_LOCATION_APPROXIMATE) != 0;
    fix.altitude = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_ALTITUDE, source.altitude,
        -unbounded, unbounded);
    fix.ellipsoidalAltitude = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_ELLIPSOIDAL_ALTITUDE,
        source.ellipsoidal_altitude, -unbounded, unbounded);
    fix.horizontalAccuracy = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_HORIZONTAL_ACCURACY,
        source.horizontal_accuracy, 0.0, unbounded);
    fix.verticalAccuracy = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_VERTICAL_ACCURACY,
        source.vertical_accuracy, 0.0, unbounded);
    fix.speed = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_SPEED, source.speed, 0.0,
        unbounded);
    fix.speedAccuracy = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_SPEED_ACCURACY,
        source.speed_accuracy, 0.0, unbounded);
    fix.course = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_COURSE, source.course, 0.0, 360.0);
    fix.courseAccuracy = optionalField(
        source, KERNARIUM_RUNTIME_LOCATION_HAS_COURSE_ACCURACY,
        source.course_accuracy, 0.0, unbounded);
    return 0;
}

} // namespace kernarium::location
