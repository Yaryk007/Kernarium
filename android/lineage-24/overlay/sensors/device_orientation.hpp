// SPDX-License-Identifier: Apache-2.0
//
// The state of the device-orientation sensor that the sub-HAL lists for a host
// that feeds it (motion_protocol.hpp). The sub-HAL converts the framework's
// calls and the host's records; what to report, and when, is decided here, and
// is tested on the host.
//
// The sensor is on-change. The host says an orientation when iOS's changes,
// whether or not anything in the guest uses the sensor, so the last value is
// kept, and a framework client that enables the sensor is given it at once
// (the window manager waits for the first event and keeps its rotation until
// it has one). Nothing is reported while the sensor is off, a value the
// framework already has is not reported again, and before the host says
// anything the sensor stays silent: Android keeps the rotation it has.
#pragma once

#include <optional>

#include "motion_protocol.hpp"

namespace andarium::motion {
class DeviceOrientation {
public:
    // Whether the framework has the sensor enabled.
    bool active() const { return active_; }
    // What the host last said (0-3); empty until it says anything.
    std::optional<int> latest() const {
        return latest_ ? std::optional<int>(*latest_) : std::nullopt;
    }

    // The framework enabled or disabled the sensor. Answers the value to
    // report: the host's last one, when the sensor was off and the host has
    // said one.
    std::optional<int> activate(bool enabled) {
        if (enabled == active_) return std::nullopt;
        active_ = enabled;
        reported_.reset();
        if (!enabled || !latest_) return std::nullopt;
        reported_ = latest_;
        return latest();
    }

    // The host said the orientation. Answers the value to report: one the
    // framework has not been given since it enabled the sensor. A value out of
    // range is not an orientation and changes nothing (the decoder refuses it).
    std::optional<int> host_value(int value) {
        if (value < 0 || value >= orientation_count) return std::nullopt;
        latest_ = value;
        if (!active_ || reported_ == latest_) return std::nullopt;
        reported_ = latest_;
        return latest();
    }

    // The framework restarted and every subscription ended; what the host
    // said stays.
    void reset() {
        active_ = false;
        reported_.reset();
    }

private:
    std::optional<int> latest_;
    std::optional<int> reported_;
    bool active_ = false;
};
} // namespace andarium::motion
