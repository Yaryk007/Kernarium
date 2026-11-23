// SPDX-License-Identifier: GPL-2.0-or-later
//
// The state of the motion sub-HAL's device-orientation sensor
// (android/lineage-24/overlay/sensors/device_orientation.hpp): what the
// framework is given and when, against the sequences the window manager and the
// host produce. Silent until the host says something, the host's last value at
// activation, a change while on, nothing twice, nothing while off.
#include <optional>
#include <vector>

#include "device_orientation.hpp"
#include "require.hpp"

using andarium::motion::DeviceOrientation;
using uml::testing::require;
using Reported = std::optional<int>;

int main()
{
    // Before the host says anything the sensor is silent, enabled or not: the
    // window manager keeps the rotation Android has.
    {
        DeviceOrientation sensor;
        require(!sensor.active() && !sensor.latest());
        require(sensor.activate(true) == Reported());
        require(sensor.active() && !sensor.latest());
        require(sensor.activate(false) == Reported());
        require(!sensor.active());
    }

    // The host's first value after the sensor was enabled is reported, and a
    // value the framework already has is not reported again.
    {
        DeviceOrientation sensor;
        require(sensor.activate(true) == Reported());
        require(sensor.host_value(1) == Reported(1));
        require(sensor.host_value(1) == Reported());
        require(sensor.host_value(0) == Reported(0));
        require(sensor.host_value(3) == Reported(3));
        require(sensor.host_value(2) == Reported(2));
        require(sensor.host_value(2) == Reported());
        require(sensor.latest() == Reported(2));
    }

    // A value that arrives while the sensor is off is kept, and the framework
    // is given it as it enables the sensor, however long afterwards.
    {
        DeviceOrientation sensor;
        require(sensor.host_value(3) == Reported());
        require(sensor.host_value(1) == Reported());
        require(sensor.latest() == Reported(1));
        require(sensor.activate(true) == Reported(1));
        require(sensor.activate(true) == Reported());
        require(sensor.host_value(1) == Reported());
        require(sensor.activate(false) == Reported());
        // Nothing while off, though the host goes on; the last value waits.
        require(sensor.host_value(2) == Reported());
        require(sensor.host_value(0) == Reported());
        require(sensor.activate(true) == Reported(0));
    }

    // A second activation after a deactivation is given the same value again
    // (a new client holds no event of the earlier one), and the same value
    // from the host afterwards is not a change.
    {
        DeviceOrientation sensor;
        require(sensor.host_value(2) == Reported());
        require(sensor.activate(true) == Reported(2));
        require(sensor.activate(false) == Reported());
        require(sensor.activate(true) == Reported(2));
        require(sensor.host_value(2) == Reported());
    }

    // A value out of range is no orientation: nothing is reported, and what the
    // sensor knew stays.
    {
        DeviceOrientation sensor;
        require(sensor.activate(true) == Reported());
        require(sensor.host_value(1) == Reported(1));
        for (const int value : {-1, 4, 100, -1000}) {
            require(sensor.host_value(value) == Reported());
            require(sensor.latest() == Reported(1));
        }
        require(sensor.host_value(1) == Reported());
    }

    // The framework restarting ends every subscription and keeps what the host
    // said; the host's next value is not reported until the sensor is enabled.
    {
        DeviceOrientation sensor;
        require(sensor.activate(true) == Reported());
        require(sensor.host_value(3) == Reported(3));
        sensor.reset();
        require(!sensor.active() && sensor.latest() == Reported(3));
        require(sensor.host_value(1) == Reported());
        require(sensor.activate(true) == Reported(1));
        sensor.reset();
        require(sensor.activate(true) == Reported(1));
    }

    // The iPhone's whole turn, as the host sends it: upright, left, upright,
    // upside down, right. Each change is one event, in order.
    {
        DeviceOrientation sensor;
        std::vector<int> events;
        const auto record = [&events](Reported value) {
            if (value) events.push_back(*value);
        };
        record(sensor.host_value(0));
        record(sensor.activate(true));
        for (const int value : {0, 1, 1, 0, 2, 3, 3}) record(sensor.host_value(value));
        require((events == std::vector<int>{0, 1, 0, 2, 3}));
    }
}
