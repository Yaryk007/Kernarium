// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace andarium::motion {
constexpr uint32_t port = 5556;
constexpr size_t record_size = 20;
// Android's sensor type of the device-orientation sensor (SensorType
// DEVICE_ORIENTATION), whose record the host sends once the guest has said it
// has the sensor (control frame "ANC2"). The record carries the orientation in
// the first value: 0 the default portrait, 1 the device turned 90 degrees
// counter-clockwise, 2 turned 180 degrees, 3 turned 90 degrees clockwise, which
// are Surface.ROTATION_* and the numbers of the sensor's own events. The other
// two values are zero.
constexpr uint32_t orientation_type = 27;
constexpr int orientation_count = 4;
// The launch property that makes the guest list the sensor: the host's
// "androidboot.kernarium.orientation=1" kernel argument. Without it the sub-HAL
// lists the accelerometer and the gyroscope alone and sends "ANC1" frames, as
// an image without the sensor does.
constexpr const char* orientation_property = "ro.boot.kernarium.orientation";
// Android's sensor types of the three sensors the host measures (SensorType
// ACCELEROMETER, MAGNETIC_FIELD and GYROSCOPE), in m/s^2, microtesla and rad/s,
// in the device's frame. The magnetic field is the one sensor whose
// record carries an accuracy: byte 6 of the record, 0 to 3, is Android's
// SensorStatus (0 unreliable, 1 low, 2 medium, 3 high). Byte 6 is 0 for every
// other type, and byte 7 is 0 for all of them. An image lists the magnetic
// field sensor, and takes its records, only for a host that says it has a
// magnetometer: the "androidboot.kernarium.compass=1" kernel argument, which
// the guest reads as the property below. Without it the image is the older one
// in every respect: no sensor, no "ANC3" frame, and a type-2 record ends the
// connection like any record it does not take.
constexpr uint32_t accelerometer_type = 1;
constexpr uint32_t magnetic_field_type = 2;
constexpr uint32_t gyroscope_type = 4;
constexpr uint8_t highest_magnetic_status = 3;
constexpr const char* compass_property = "ro.boot.kernarium.compass";
struct Sample {
    uint32_t type;
    std::array<float, 3> values;
    // Android's SensorStatus; 0 for every sensor but the magnetic field.
    uint8_t status = 0;
};
inline std::optional<Sample> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() != record_size || bytes[0] != 'A' || bytes[1] != 'N' ||
        bytes[2] != 'S' || bytes[3] != '1') return std::nullopt;
    const auto word = [&bytes](size_t offset) {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    };
    // The type is a uint16, then the status byte and a byte that is always 0.
    Sample sample{uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8), {}, bytes[6]};
    if (bytes[7] != 0) return std::nullopt;
    if (sample.type == magnetic_field_type) {
        if (sample.status > highest_magnetic_status) return std::nullopt;
    } else if ((sample.type != accelerometer_type && sample.type != gyroscope_type &&
                sample.type != orientation_type) || sample.status != 0) {
        return std::nullopt;
    }
    for (size_t i = 0; i < sample.values.size(); ++i) {
        sample.values[i] = std::bit_cast<float>(word(8 + i * 4));
        if (!std::isfinite(sample.values[i])) return std::nullopt;
    }
    if (sample.type == orientation_type &&
        (sample.values[0] < 0 || sample.values[0] >= orientation_count ||
         sample.values[0] != std::floor(sample.values[0]) || sample.values[1] != 0 ||
         sample.values[2] != 0)) {
        return std::nullopt;
    }
    return sample;
}

// What the launch gave the guest: the sensors the sub-HAL lists besides the
// accelerometer and the gyroscope.
struct Features {
    bool orientation = false;
    bool compass = false;
    constexpr bool operator==(const Features&) const = default;
};
// The handles of the sensors. They are fixed, so a sensor keeps its handle
// whichever of the others the launch asked for; the list holds the handles of
// the sensors that exist, which are not contiguous when a host has a
// magnetometer and no orientation sensor.
constexpr int32_t accelerometer_handle = 1;
constexpr int32_t gyroscope_handle = 2;
constexpr int32_t orientation_handle = 3;
constexpr int32_t magnetometer_handle = 4;
struct Listing {
    std::array<int32_t, 4> handles{};
    size_t count = 0;
    constexpr bool contains(int32_t handle) const {
        for (size_t i = 0; i < count; ++i) if (handles[i] == handle) return true;
        return false;
    }
};
// The sensors the sub-HAL lists, in list order.
constexpr Listing listing(const Features& features) {
    Listing list;
    list.handles[list.count++] = accelerometer_handle;
    list.handles[list.count++] = gyroscope_handle;
    if (features.orientation) list.handles[list.count++] = orientation_handle;
    if (features.compass) list.handles[list.count++] = magnetometer_handle;
    return list;
}
// The sensor whose records have this type, empty for a type no record has.
constexpr std::optional<int32_t> handle_of_type(uint32_t type) {
    switch (type) {
    case accelerometer_type: return accelerometer_handle;
    case magnetic_field_type: return magnetometer_handle;
    case gyroscope_type: return gyroscope_handle;
    case orientation_type: return orientation_handle;
    default: return std::nullopt;
    }
}
// Whether the guest takes a record of this type. A record it does not take
// ends the connection: an image that does not list the sensor never told the
// host that it could send one.
constexpr bool takes_record(const Features& features, uint32_t type) {
    const auto handle = handle_of_type(type);
    return handle && listing(features).contains(*handle);
}

// Guest-to-host control frame, sent on the same connection when the host
// connects and at every change of what applications use. 16 bytes, little
// endian: "ANC1", a uint32 mask of the active sensors (bit 0 accelerometer,
// bit 1 gyroscope), then the uint32 sampling period in microseconds of the
// accelerometer and of the gyroscope. The host reads a sensor only while it is
// active; a host that has not seen a frame reads both continuously.
//
// An image that has the device-orientation sensor sends "ANC2" instead: the
// same layout, with bit 2 of the mask for that sensor (it has no period). The
// frame also tells the host that the guest accepts orientation records, which
// an image without the sensor would take for a bad record and answer by ending
// the connection, so a host sends one only after an "ANC2" frame.
//
// An image that has the magnetic field sensor sends "ANC3", 20 bytes: the
// mask has bit 0 and bit 1 as above, bit 2 for the device-orientation sensor
// being enabled, bit 3 for the magnetometer being active and bit 4 for the
// device-orientation sensor being listed at all (bit 2 without bit 4 is no
// state). The periods follow, and a third, the magnetometer's, which a frame
// brings into 20 ms to 1 s. The frame tells the host that the guest takes
// magnetic field records, which an older image would answer by ending the
// connection, so a host sends one only while the latest frame has bit 3 set.
constexpr size_t control_size = 16;
constexpr size_t compass_control_size = 20;
constexpr int64_t minimum_period_ns = 10'000'000;
constexpr int64_t maximum_period_ns = 1'000'000'000;
constexpr int64_t minimum_magnetometer_period_ns = 20'000'000;
// The shortest period a sensor is read at, and the longest.
constexpr int64_t minimum_period_of(int32_t handle) {
    return handle == magnetometer_handle ? minimum_magnetometer_period_ns : minimum_period_ns;
}
constexpr int64_t clamp_period(int32_t handle, int64_t period_ns) {
    return std::clamp(period_ns, minimum_period_of(handle), maximum_period_ns);
}
// `orientation` is empty for a guest without the sensor, which sends "ANC1",
// and otherwise whether the sensor is active, which sends "ANC2".
inline std::array<uint8_t, control_size> encode_control(
        const std::array<bool, 2>& active, const std::array<int64_t, 2>& period_ns,
        std::optional<bool> orientation = std::nullopt) {
    std::array<uint8_t, control_size> frame{'A', 'N', 'C', uint8_t(orientation ? '2' : '1')};
    const auto put = [&frame](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) frame[offset + i] = uint8_t(value >> (8 * i));
    };
    put(4, (active[0] ? 1u : 0u) | (active[1] ? 2u : 0u) | (orientation.value_or(false) ? 4u : 0u));
    for (size_t i = 0; i < period_ns.size(); ++i) {
        const auto period = std::clamp(period_ns[i], minimum_period_ns, maximum_period_ns);
        put(8 + i * 4, uint32_t(period / 1000));
    }
    return frame;
}
// The "ANC3" frame of a guest that lists the magnetometer: `active` and
// `period_ns` are the accelerometer's, the gyroscope's and the magnetometer's.
// `orientation` is empty when the guest does not list the orientation sensor
// and otherwise whether it is enabled.
inline std::array<uint8_t, compass_control_size> encode_compass_control(
        const std::array<bool, 3>& active, const std::array<int64_t, 3>& period_ns,
        std::optional<bool> orientation = std::nullopt) {
    std::array<uint8_t, compass_control_size> frame{'A', 'N', 'C', '3'};
    const auto put = [&frame](size_t offset, uint32_t value) {
        for (size_t i = 0; i < 4; ++i) frame[offset + i] = uint8_t(value >> (8 * i));
    };
    put(4, (active[0] ? 1u : 0u) | (active[1] ? 2u : 0u) | (orientation.value_or(false) ? 4u : 0u) |
           (active[2] ? 8u : 0u) | (orientation ? 16u : 0u));
    const std::array<int32_t, 3> handles{accelerometer_handle, gyroscope_handle, magnetometer_handle};
    for (size_t i = 0; i < period_ns.size(); ++i) put(8 + i * 4, uint32_t(clamp_period(handles[i], period_ns[i]) / 1000));
    return frame;
}
// The frame a guest with these features sends: "ANC1" with neither optional
// sensor, "ANC2" with the orientation sensor alone and "ANC3" with the
// magnetometer, whatever else it has. `active` and `period_ns` are the
// accelerometer's, the gyroscope's and the magnetometer's (which an image
// without the sensor has not, and which then say nothing).
struct ControlFrame {
    std::array<uint8_t, compass_control_size> bytes{};
    size_t size = 0;
};
inline ControlFrame control_frame(
        const Features& features, const std::array<bool, 3>& active,
        const std::array<int64_t, 3>& period_ns, bool orientation_active) {
    ControlFrame frame;
    const std::optional<bool> orientation = features.orientation ? std::optional<bool>(orientation_active)
                                                                  : std::nullopt;
    if (features.compass) {
        frame.bytes = encode_compass_control(active, period_ns, orientation);
        frame.size = compass_control_size;
    } else {
        const auto older = encode_control({active[0], active[1]}, {period_ns[0], period_ns[1]}, orientation);
        std::copy(older.begin(), older.end(), frame.bytes.begin());
        frame.size = control_size;
    }
    return frame;
}

// What the sensor service is told about the magnetometer. Core Motion states
// neither the range nor the resolution of the field it fuses, so these declare
// figures within the compatibility definition (section 7.3.2: a range of at
// least 900 uT, a resolution of at most 0.6 uT, a rate of at least 10 Hz);
// whether the hardware behind Core Motion meets its other requirements is
// Apple's and unmeasured. The resolution is a power of two, 1/8 uT, so that
// the range (4912 uT, 39296 steps) is an exact multiple of it, which the
// sensor service checks. The service rounds every event value to an eighth of
// the resolution it holds for the sensor, so a test that compares a value
// reads that resolution from the service. The delays are the ones a frame
// brings a period into; the power is not measured, like the other host
// sensors'.
struct SensorAttributes {
    float max_range;
    float resolution;
    float power_ma;
    int32_t min_delay_us;
    int32_t max_delay_us;
};
constexpr SensorAttributes magnetometer_attributes{
    4912.0f, 0.125f, 0.5f, int32_t(minimum_magnetometer_period_ns / 1000), int32_t(maximum_period_ns / 1000)};
} // namespace andarium::motion
