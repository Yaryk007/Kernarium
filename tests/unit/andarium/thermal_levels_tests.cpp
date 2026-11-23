// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the thermal HAL reports for each severity the host sends: the status,
// the skin temperature and the thresholds, against the rules that the
// framework's thermal manager and CTS and VTS apply to them.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

#include "require.hpp"
#include "thermal_levels.hpp"

using namespace andarium::thermal;
using uml::testing::require;

namespace {
constexpr Severity wire_severities[] = {Severity::none, Severity::light, Severity::moderate, Severity::severe,
                                        Severity::critical};

// ThermalManagerService.TemperatureWatcher.normalizeTemperature: 1.0 is the
// SEVERE threshold's temperature, and 0.0 is 30 degrees under it.
float headroom(float temperature) {
    const float severe = hot_thresholds[3];
    const float zero = severe - 30.0f;
    return temperature <= zero ? 0.0f : (temperature - zero) / 30.0f;
}

// The status the framework would see for a temperature: the highest threshold
// the temperature has reached (a throttling happens when temperature >= threshold).
int status_of(float temperature) {
    int status = 0;
    for (std::size_t severity = 1; severity < severity_count; ++severity) {
        if (temperature >= hot_thresholds[severity]) status = int(severity);
    }
    return status;
}

bool near(float left, float right) { return std::fabs(left - right) < 1e-5f; }

void check_status() {
    for (const auto severity : wire_severities) {
        require(throttling_status(severity) == int32_t(severity), "the status is the severity");
    }
    // The HAL never reports EMERGENCY (5) or SHUTDOWN (6), whatever it is given.
    for (unsigned value = 5; value < 256; ++value) {
        require(throttling_status(Severity(value)) == 4, "a value beyond CRITICAL reports CRITICAL");
        require(skin_temperature(Severity(value)) == skin_temperature(Severity::critical),
                "and its temperature is CRITICAL's");
        require(status_of(skin_temperature(Severity(value))) == 4, "which the framework reads as CRITICAL");
    }
}

void check_temperatures() {
    require(skin_temperature(Severity::none) == 31.0f, "NONE 31.0");
    require(skin_temperature(Severity::light) == 35.5f, "LIGHT 35.5");
    require(skin_temperature(Severity::moderate) == 38.5f, "MODERATE 38.5");
    require(skin_temperature(Severity::severe) == 41.5f, "SEVERE 41.5");
    require(skin_temperature(Severity::critical) == 44.5f, "CRITICAL 44.5");
    for (const auto severity : wire_severities) {
        const float temperature = skin_temperature(severity);
        require(!std::isnan(temperature), "the value is never NaN");
        // The temperature sits inside its severity's band, so the status the
        // HAL reports and the status the temperature implies agree.
        require(status_of(temperature) == throttling_status(severity), "the temperature is in its band");
        const std::size_t index = std::size_t(throttling_status(severity));
        if (index > 0) require(temperature >= hot_thresholds[index], "at or above its threshold");
        require(temperature < hot_thresholds[index + 1], "under the next threshold");
        // The middle of the band, away from either edge.
        if (index > 0) require(temperature - hot_thresholds[index] >= 1.0f, "a degree above the threshold");
        require(hot_thresholds[index + 1] - temperature >= 1.0f, "a degree under the next threshold");
    }
    for (std::size_t i = 1; i < std::size(wire_severities); ++i) {
        require(skin_temperature(wire_severities[i]) > skin_temperature(wire_severities[i - 1]),
                "a higher severity is hotter");
    }
}

// What VtsHalThermalTargetTest SkinTemperatureThresholdsTest checks.
void check_thresholds() {
    require(hot_thresholds.size() == severity_count && severity_count == 7, "one threshold per ThrottlingSeverity");
    require(cold_thresholds.size() == severity_count, "and as many cold ones");
    require(std::isnan(hot_thresholds[0]), "NONE has no threshold");
    for (std::size_t severity = 1; severity < severity_count; ++severity) {
        require(!std::isnan(hot_thresholds[severity]), "LIGHT through SHUTDOWN are set");
        if (severity > 1) require(hot_thresholds[severity] >= hot_thresholds[severity - 1], "and do not decrease");
    }
    for (const auto threshold : cold_thresholds) require(std::isnan(threshold), "no cold threshold");
    require(hot_thresholds[1] == 34.0f && hot_thresholds[2] == 37.0f && hot_thresholds[3] == 40.0f &&
                hot_thresholds[4] == 43.0f && hot_thresholds[5] == 46.0f && hot_thresholds[6] == 49.0f,
            "the thresholds");
    require(std::string(sensor_name) == "skin", "the sensor and its threshold are both named skin");
}

// What the framework derives for ADPF headroom (CTS: the SEVERE threshold's
// headroom is exactly 1.0, nothing below it is above 1.0, the values are in
// [0, 10) and do not decrease) and the numbers the design lists.
void check_headroom() {
    const float expected_thresholds[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f};
    float last = 0;
    for (std::size_t severity = 1; severity < severity_count; ++severity) {
        const float value = headroom(hot_thresholds[severity]);
        require(near(value, expected_thresholds[severity - 1]), "the headroom thresholds");
        require(value >= last && value >= 0 && value < 10, "non-decreasing, in [0, 10)");
        last = value;
    }
    require(headroom(hot_thresholds[3]) == 1.0f, "SEVERE is exactly 1.0");
    require(headroom(hot_thresholds[1]) < 1.0f && headroom(hot_thresholds[2]) < 1.0f, "nothing below SEVERE is above 1.0");

    require(near(headroom(skin_temperature(Severity::none)), 0.70f), "NONE headroom 0.70");
    require(near(headroom(skin_temperature(Severity::light)), 0.85f), "LIGHT headroom 0.85");
    require(near(headroom(skin_temperature(Severity::moderate)), 0.95f), "MODERATE headroom 0.95");
    require(near(headroom(skin_temperature(Severity::severe)), 1.05f), "SEVERE headroom 1.05");
    require(near(headroom(skin_temperature(Severity::critical)), 1.15f), "CRITICAL headroom 1.15");
    // Only SEVERE and CRITICAL put an application at or over 1.0: they are the
    // states in which the host asks Android to hold back.
    require(headroom(skin_temperature(Severity::moderate)) < 1.0f, "MODERATE leaves headroom");
    require(headroom(skin_temperature(Severity::severe)) >= 1.0f, "SEVERE has none");
}
}  // namespace

int main() {
    check_status();
    check_temperatures();
    check_thresholds();
    check_headroom();
    std::puts("thermal levels: ok");
    return 0;
}
