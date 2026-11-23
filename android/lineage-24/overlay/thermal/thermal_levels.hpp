// SPDX-License-Identifier: Apache-2.0
//
// What the thermal HAL reports for each severity the host sends. The HAL has
// one SKIN sensor, and the host's state moves it between the bands that the
// sensor's thresholds define. The framework derives the throttling status from
// the HAL's own `throttlingStatus` and the headroom from the temperature and
// the SEVERE threshold, so the temperature of every severity sits inside its
// band: the middle of it, so that no rounding moves it out. CTS wants
// the SEVERE threshold's headroom to be exactly 1.0 and no threshold below it
// to be above 1.0, and VTS wants one SKIN sensor whose thresholds from LIGHT
// to SHUTDOWN are set and do not decrease.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "thermal_protocol.hpp"

namespace andarium::thermal {
// The sensor's name, in the temperature and in the threshold alike.
constexpr char sensor_name[] = "skin";

// ThrottlingSeverity NONE through SHUTDOWN.
constexpr std::size_t severity_count = 7;

constexpr float no_threshold = std::numeric_limits<float>::quiet_NaN();

// hotThrottlingThresholds of the sensor, in degrees Celsius, indexed by
// ThrottlingSeverity. NONE has no threshold.
constexpr std::array<float, severity_count> hot_thresholds{
    no_threshold, 34.0f, 37.0f, 40.0f, 43.0f, 46.0f, 49.0f};

// coldThrottlingThresholds: the sensor has none.
constexpr std::array<float, severity_count> cold_thresholds{
    no_threshold, no_threshold, no_threshold, no_threshold, no_threshold, no_threshold, no_threshold};

// The ThrottlingSeverity to report. EMERGENCY and SHUTDOWN make Android close
// apps and shut the guest down, so the HAL never reports them, whatever it is
// given: a value beyond CRITICAL reports CRITICAL.
constexpr int32_t throttling_status(Severity severity) {
    return int32_t(std::min(uint8_t(severity), highest_severity));
}

// The skin temperature to report, in degrees Celsius: the middle of the band
// from the severity's threshold to the next one (NONE: 3 degrees under LIGHT's).
constexpr float skin_temperature(Severity severity) {
    switch (std::min(uint8_t(severity), highest_severity)) {
        case uint8_t(Severity::none):
            return 31.0f;
        case uint8_t(Severity::light):
            return 35.5f;
        case uint8_t(Severity::moderate):
            return 38.5f;
        case uint8_t(Severity::severe):
            return 41.5f;
        default:
            return 44.5f;
    }
}
}  // namespace andarium::thermal
