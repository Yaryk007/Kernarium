// SPDX-License-Identifier: GPL-2.0-or-later
//
// The motion wire (vsock 5556) from the guest's side: the sensor records the
// host sends, the control frames the sub-HAL sends and the rule that says which
// sensors it lists, all against the golden frames that AndariumCore checks too
// (MotionOrientationFixture.swift). argv[1] is the fixture,
// tests/unit/andarium/fixtures/motion-orientation-golden-frames.txt.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "motion_protocol.hpp"
#include "require.hpp"

using uml::testing::require;
using Bytes = std::vector<uint8_t>;

namespace {
Bytes parse_hex(const std::string& text)
{
    Bytes bytes;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        require(token.size() == 2, "a fixture byte is two hex digits");
        bytes.push_back(uint8_t(std::stoul(token, nullptr, 16)));
    }
    return bytes;
}

// `name: <hex bytes>` per line; a line that starts with # is a comment.
std::map<std::string, Bytes> read_fixture(const char* path)
{
    std::ifstream file(path);
    require(file.is_open(), "the golden-frame fixture opens");
    std::map<std::string, Bytes> frames;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto colon = line.find(':');
        require(colon != std::string::npos, "a fixture line is name: bytes");
        require(frames.emplace(line.substr(0, colon), parse_hex(line.substr(colon + 1))).second,
                "fixture names are unique");
    }
    return frames;
}

bool starts_with(const std::string& text, const char* prefix)
{
    return text.rfind(prefix, 0) == 0;
}

// The oracle for the orientation records, written out here independently of
// the decoder: the host sends 20 bytes "ANS1", type 27 and one of four whole
// numbers, then two zeros.
bool host_may_send_orientation(const Bytes& frame)
{
    if (frame.size() != 20 || frame[0] != 'A' || frame[1] != 'N' || frame[2] != 'S' || frame[3] != '1' ||
        frame[4] != 27 || frame[5] != 0 || frame[6] != 0 || frame[7] != 0) {
        return false;
    }
    static const Bytes orientations[] = {{0, 0, 0, 0}, {0, 0, 0x80, 0x3f}, {0, 0, 0, 0x40}, {0, 0, 0x40, 0x40}};
    bool known = false;
    for (const auto& orientation : orientations) known |= Bytes(frame.begin() + 8, frame.begin() + 12) == orientation;
    return known && Bytes(frame.begin() + 12, frame.end()) == Bytes(8, 0);
}

// The oracle for every other record, written out here independently of the
// decoder: 20 bytes "ANS1", the type as a uint16 of 1, 2 or 4, a status byte
// that is 0 to 3 for the magnetic field and 0 for the others, a byte that is
// 0, and three finite float32 values.
bool finite_word(const Bytes& frame, size_t offset)
{
    // An exponent of all ones is infinity or NaN.
    return (frame[offset + 3] & 0x7f) != 0x7f || (frame[offset + 2] & 0x80) == 0;
}

bool host_may_send_sensor(const Bytes& frame)
{
    if (frame.size() != 20 || frame[0] != 'A' || frame[1] != 'N' || frame[2] != 'S' || frame[3] != '1' ||
        frame[5] != 0 || frame[7] != 0) {
        return false;
    }
    const auto type = frame[4];
    if (type == 27) return host_may_send_orientation(frame);
    if (type != 1 && type != 2 && type != 4) return false;
    if (frame[6] > (type == 2 ? 3 : 0)) return false;
    return finite_word(frame, 8) && finite_word(frame, 12) && finite_word(frame, 16);
}

float float_at(const Bytes& frame, size_t offset)
{
    return std::bit_cast<float>(uint32_t(frame[offset]) | (uint32_t(frame[offset + 1]) << 8) |
                                (uint32_t(frame[offset + 2]) << 16) | (uint32_t(frame[offset + 3]) << 24));
}

void check_fixture(const std::map<std::string, Bytes>& fixture)
{
    using andarium::motion::Features;
    constexpr int64_t ms = 1'000'000;
    // The guest's state behind each control frame of the fixture. The periods
    // are the accelerometer's, the gyroscope's and the magnetometer's; those of
    // a sensor the image does not list say nothing.
    struct State {
        Features features;
        std::array<bool, 3> active;
        std::array<int64_t, 3> period;
        bool orientation_active;
    };
    const std::map<std::string, State> controls = {
        {"control-v1-idle", {{false, false}, {false, false, false}, {10 * ms, 10 * ms, 20 * ms}, false}},
        {"control-v1-motion", {{false, false}, {true, true, false}, {20 * ms, 1'000 * ms, 20 * ms}, false}},
        {"control-v2-idle", {{true, false}, {false, false, false}, {10 * ms, 10 * ms, 20 * ms}, false}},
        {"control-v2-orientation", {{true, false}, {false, false, false}, {10 * ms, 10 * ms, 20 * ms}, true}},
        {"control-v2-all", {{true, false}, {true, true, false}, {20 * ms, 1'000 * ms, 20 * ms}, true}},
        {"control-v3-idle", {{false, true}, {false, false, false}, {10 * ms, 10 * ms, 20 * ms}, false}},
        {"control-v3-magnetometer", {{false, true}, {false, false, true}, {10 * ms, 10 * ms, 50 * ms}, false}},
        {"control-v3-orientation-idle", {{true, true}, {false, false, false}, {10 * ms, 10 * ms, 20 * ms}, false}},
        {"control-v3-orientation", {{true, true}, {false, false, false}, {10 * ms, 10 * ms, 20 * ms}, true}},
        {"control-v3-all", {{true, true}, {true, true, true}, {20 * ms, 1'000 * ms, 40 * ms}, true}},
    };
    size_t records = 0, rejected_records = 0, magnetic = 0, rejected_magnetic = 0, others = 0, rejected_others = 0;
    size_t frames = 0, rejected_frames = 0;
    for (const auto& [name, bytes] : fixture) {
        if (starts_with(name, "orientation-")) {
            ++records;
            require(host_may_send_orientation(bytes), name);
            const auto sample = andarium::motion::decode(bytes);
            require(sample.has_value(), name);
            require(sample->type == andarium::motion::orientation_type, name);
            require(sample->values[0] == float(name.back() - '0'), name);
            require(sample->values[1] == 0 && sample->values[2] == 0 && sample->status == 0, name);
        } else if (starts_with(name, "reject-orientation-")) {
            ++rejected_records;
            require(!host_may_send_orientation(bytes), name);
            require(!andarium::motion::decode(bytes).has_value(), name);
        } else if (starts_with(name, "magnetic-status-")) {
            ++magnetic;
            require(host_may_send_sensor(bytes) && bytes[4] == 2, name);
            const auto sample = andarium::motion::decode(bytes);
            require(sample.has_value(), name);
            require(sample->type == andarium::motion::magnetic_field_type, name);
            require(sample->status == name.back() - '0', name);
            require(sample->values == std::array<float, 3>{0.0f, 25.0f, -40.0f}, name);
        } else if (starts_with(name, "reject-magnetic-")) {
            ++rejected_magnetic;
            require(bytes.size() == 20 && bytes[4] == 2, name);
            require(!host_may_send_sensor(bytes), name);
            require(!andarium::motion::decode(bytes).has_value(), name);
        } else if (starts_with(name, "accelerometer-") || starts_with(name, "gyroscope-")) {
            ++others;
            require(host_may_send_sensor(bytes), name);
            const auto sample = andarium::motion::decode(bytes);
            require(sample.has_value() && sample->status == 0, name);
            require(sample->type == bytes[4], name);
            for (size_t i = 0; i < 3; ++i) require(sample->values[i] == float_at(bytes, 8 + 4 * i), name);
        } else if (starts_with(name, "reject-accelerometer-") || starts_with(name, "reject-gyroscope-") ||
                   starts_with(name, "reject-type-")) {
            ++rejected_others;
            require(!host_may_send_sensor(bytes), name);
            require(!andarium::motion::decode(bytes).has_value(), name);
        } else if (starts_with(name, "control-")) {
            ++frames;
            const auto state = controls.find(name);
            require(state != controls.end(), name);
            const auto& want = state->second;
            const auto frame = andarium::motion::control_frame(
                want.features, want.active, want.period, want.orientation_active);
            require(frame.size == bytes.size(), name);
            require(Bytes(frame.bytes.begin(), frame.bytes.begin() + frame.size) == bytes, name);
        } else {
            // The guest never encodes a rejected control frame, whichever
            // sensors it lists and whatever is in use.
            require(starts_with(name, "reject-control-"), name);
            ++rejected_frames;
            for (const bool orientation : {false, true}) {
                for (const bool compass : {false, true}) {
                    for (unsigned mask = 0; mask < 8; ++mask) {
                        for (const bool orientation_active : {false, true}) {
                            for (const int64_t magnetometer : {20 * ms, 50 * ms}) {
                                const auto frame = andarium::motion::control_frame(
                                    {orientation, compass}, {bool(mask & 1), bool(mask & 2), bool(mask & 4)},
                                    {10 * ms, 10 * ms, magnetometer}, orientation_active);
                                require(Bytes(frame.bytes.begin(), frame.bytes.begin() + frame.size) != bytes, name);
                            }
                        }
                    }
                }
            }
        }
    }
    require(records == 4 && rejected_records == 7, "the fixture holds the orientation records");
    require(magnetic == 4 && rejected_magnetic == 6, "the fixture holds the magnetic field records");
    require(others == 2 && rejected_others == 5, "the fixture holds the other records");
    require(frames == controls.size() && rejected_frames == 8, "the fixture holds every control frame");
}
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2, "usage: motion_protocol_tests <motion-orientation-golden-frames.txt>");
    check_fixture(read_fixture(argv[1]));

    // Independent wire fixture: +1, -2, +0.5 radians/second.
    std::array<uint8_t, 20> bytes{
        'A', 'N', 'S', '1', 4, 0, 0, 0,
        0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0, 0x3f};
    const auto sample = andarium::motion::decode(bytes);
    require(sample.has_value());
    require(sample->type == 4);
    require(sample->values == std::array<float, 3>{1, -2, 0.5f});
    require(!andarium::motion::decode(std::span(bytes).first(19)));
    bytes[0] = 0;
    require(!andarium::motion::decode(bytes));
    bytes[0] = 'A';
    bytes[4] = 99;
    require(!andarium::motion::decode(bytes));
    bytes[4] = 1;
    require(andarium::motion::decode(bytes).has_value());
    bytes[10] = 0x80;
    bytes[11] = 0x7f; // Infinity is never a physical measurement.
    require(!andarium::motion::decode(bytes));
    bytes[10] = 0xc0; // Neither is NaN.
    require(!andarium::motion::decode(bytes));

    // Independent control frame fixtures: the accelerometer alone at 20 ms
    // (the gyroscope's period is carried and not read), then both, at 66 667
    // us and 1 s.
    using andarium::motion::encode_control;
    require(encode_control({true, false}, {20'000'000, 10'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 1, 0, 0, 0,
                                    0x20, 0x4e, 0, 0, 0x10, 0x27, 0, 0});
    require(encode_control({true, true}, {66'667'000, 1'000'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 3, 0, 0, 0,
                                    0x6b, 0x04, 0x01, 0, 0x40, 0x42, 0x0f, 0});
    // Nothing in use, and periods the sensor framework would never ask for,
    // which the frame brings into 10 ms to 1 s.
    require(encode_control({false, false}, {0, 5'000'000'000}) ==
            std::array<uint8_t, 16>{'A', 'N', 'C', '1', 0, 0, 0, 0,
                                    0x10, 0x27, 0, 0, 0x40, 0x42, 0x0f, 0});
    require(encode_control({false, true}, {10'000'000, 10'000'000})[4] == 2);

    // An image with the orientation sensor says so in the magic and the third
    // bit, which the sensors of the older frame leave as they were; the
    // sensor's own period does not exist.
    const auto with_sensor = encode_control({true, false}, {20'000'000, 10'000'000}, false);
    require(with_sensor[2] == 'C' && with_sensor[3] == '2' && with_sensor[4] == 1);
    require(encode_control({true, true}, {20'000'000, 10'000'000}, true)[4] == 7);
    require(encode_control({false, false}, {20'000'000, 10'000'000}, true)[4] == 4);
    require(encode_control({false, false}, {20'000'000, 10'000'000})[3] == '1');

    // The orientation record: the first value alone, one of four whole
    // numbers, in the 20 bytes of every record. An independent fixture of
    // landscape 90 degrees counter-clockwise (1.0), then each way it is wrong.
    std::array<uint8_t, 20> turned{
        'A', 'N', 'S', '1', 27, 0, 0, 0, 0, 0, 0x80, 0x3f, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto orientation = andarium::motion::decode(turned);
    require(orientation.has_value() && orientation->type == 27 && orientation->values[0] == 1.0f);
    for (const float value : {0.0f, 1.0f, 2.0f, 3.0f}) {
        const auto word = std::bit_cast<uint32_t>(value);
        for (size_t i = 0; i < 4; ++i) turned[8 + i] = uint8_t(word >> (8 * i));
        require(andarium::motion::decode(turned).has_value());
    }
    const auto refuse = [&turned](size_t offset, float value) {
        auto bytes = turned;
        const auto word = std::bit_cast<uint32_t>(value);
        for (size_t i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(word >> (8 * i));
        return !andarium::motion::decode(bytes).has_value();
    };
    require(refuse(8, 4.0f) && refuse(8, -1.0f) && refuse(8, 0.5f) && refuse(8, 1e9f));
    require(refuse(8, std::nanf("")) && refuse(8, INFINITY));
    require(refuse(12, 1.0f) && refuse(16, 1.0f));
    // A record is exactly 20 bytes: a valid one with a byte more, or with a byte
    // less, is not a record.
    std::array<uint8_t, 21> overlong{};
    std::copy(turned.begin(), turned.end(), overlong.begin());
    require(!andarium::motion::decode(overlong));
    require(!andarium::motion::decode(std::span(turned).first(19)));
    require(andarium::motion::orientation_count == 4);
    require(std::string(andarium::motion::orientation_property) == "ro.boot.kernarium.orientation");

    // The magnetic field record: type 2, then the status byte, the byte that
    // is always 0 and three values in microtesla. An independent fixture of
    // (0, 25, -40) at high accuracy, then each way it is wrong.
    std::array<uint8_t, 20> field{
        'A', 'N', 'S', '1', 2, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0xc8, 0x41, 0, 0, 0x20, 0xc2};
    const auto magnetic = andarium::motion::decode(field);
    require(magnetic.has_value() && magnetic->type == andarium::motion::magnetic_field_type);
    require(magnetic->status == 3 && magnetic->values == std::array<float, 3>{0.0f, 25.0f, -40.0f});
    for (uint8_t status = 0; status <= 3; ++status) {
        field[6] = status;
        const auto sample = andarium::motion::decode(field);
        require(sample.has_value() && sample->status == status);
    }
    for (const uint8_t status : {4, 5, 127, 128, 255}) {
        field[6] = status;
        require(!andarium::motion::decode(field));
    }
    field[6] = 2;
    field[7] = 1;
    require(!andarium::motion::decode(field));
    field[7] = 0;
    // The type is a uint16: a type whose high byte is not 0 is no sensor, and
    // 0x0102 is not 2.
    field[5] = 1;
    require(!andarium::motion::decode(field));
    field[5] = 0;
    require(andarium::motion::decode(field).has_value());
    // Only the magnetic field has a status; the other records say 0.
    for (const uint8_t type : {1, 4, 27}) {
        auto record = turned;
        record[4] = type;
        require(andarium::motion::decode(record).has_value());
        record[6] = 1;
        require(!andarium::motion::decode(record));
    }
    // Types the guest never takes are no records at all.
    for (const uint8_t type : {0, 3, 5, 11, 14, 20, 28}) {
        field[4] = type;
        require(!andarium::motion::decode(field));
    }
    field[4] = 2;
    require(andarium::motion::decode(field).has_value());
    for (const size_t offset : {8, 12, 16}) {
        auto bad = field;
        bad[offset + 2] = 0x80;
        bad[offset + 3] = 0x7f; // Infinity is never a measurement of the field.
        require(!andarium::motion::decode(bad));
        bad[offset + 2] = 0xc0; // Neither is NaN.
        require(!andarium::motion::decode(bad));
    }

    // Which sensors the sub-HAL lists, from the two properties: none of the
    // optional ones for an image booted without them, which is the older
    // image in every respect. The handles are fixed, so a host with a
    // magnetometer and no orientation sensor has the handles 1, 2 and 4.
    using andarium::motion::Features;
    using andarium::motion::listing;
    using andarium::motion::takes_record;
    const auto handles = [](const Features& features) {
        const auto list = listing(features);
        return std::vector<int32_t>(list.handles.begin(), list.handles.begin() + list.count);
    };
    require(handles({false, false}) == std::vector<int32_t>{1, 2});
    require(handles({true, false}) == std::vector<int32_t>{1, 2, 3});
    require(handles({false, true}) == std::vector<int32_t>{1, 2, 4});
    require(handles({true, true}) == std::vector<int32_t>{1, 2, 3, 4});
    for (const auto& features : {Features{false, false}, Features{true, false}, Features{false, true}, Features{true, true}}) {
        const auto list = listing(features);
        require(list.contains(1) && list.contains(2));
        require(list.contains(3) == features.orientation && list.contains(4) == features.compass);
        for (const int32_t handle : {-1, 0, 5, 7, 255}) require(!list.contains(handle));
        // A record is taken exactly for a sensor that is listed.
        require(takes_record(features, 1) && takes_record(features, 4));
        require(takes_record(features, andarium::motion::orientation_type) == features.orientation);
        require(takes_record(features, andarium::motion::magnetic_field_type) == features.compass);
        for (const uint32_t type : {0u, 3u, 5u, 11u, 14u, 20u, 28u, 0x10002u}) require(!takes_record(features, type));
    }
    // Without the compass property: no magnetometer, no magnetic field record,
    // whatever else the launch asked for.
    require(!listing({false, false}).contains(andarium::motion::magnetometer_handle));
    require(!takes_record({true, false}, 2));
    require(std::string(andarium::motion::compass_property) == "ro.boot.kernarium.compass");
    for (const uint32_t type : {1u, 2u, 4u, 27u}) {
        require(andarium::motion::handle_of_type(type).has_value());
    }
    require(*andarium::motion::handle_of_type(2) == 4 && *andarium::motion::handle_of_type(27) == 3);
    require(!andarium::motion::handle_of_type(3).has_value());

    // Each sensor is listed with its own limits, which a period is brought into.
    using andarium::motion::clamp_period;
    require(clamp_period(1, 5'000'000) == 10'000'000 && clamp_period(2, 0) == 10'000'000);
    require(clamp_period(1, 5'000'000'000) == 1'000'000'000);
    require(clamp_period(4, 10'000'000) == 20'000'000 && clamp_period(4, 19'999'999) == 20'000'000);
    require(clamp_period(4, 20'000'000) == 20'000'000 && clamp_period(4, 66'667'000) == 66'667'000);
    require(clamp_period(4, 5'000'000'000) == 1'000'000'000);

    // The "ANC3" frame, from an independent fixture: the accelerometer at
    // 20 ms and the magnetometer at 40 ms in use, no orientation sensor.
    using andarium::motion::encode_compass_control;
    require(encode_compass_control({true, false, true}, {20'000'000, 10'000'000, 40'000'000}) ==
            std::array<uint8_t, 20>{'A', 'N', 'C', '3', 9, 0, 0, 0, 0x20, 0x4e, 0, 0,
                                    0x10, 0x27, 0, 0, 0x40, 0x9c, 0, 0});
    // The orientation sensor listed and enabled is bit 4 and bit 2; listed
    // alone is bit 4; absent is neither.
    require(encode_compass_control({false, false, false}, {10'000'000, 10'000'000, 20'000'000}, true)[4] == 20);
    require(encode_compass_control({false, false, false}, {10'000'000, 10'000'000, 20'000'000}, false)[4] == 16);
    require(encode_compass_control({false, false, false}, {10'000'000, 10'000'000, 20'000'000})[4] == 0);
    require(encode_compass_control({true, true, true}, {10'000'000, 10'000'000, 20'000'000}, true)[4] == 31);
    // The magnetometer's period is brought into 20 ms to 1 s, the others' into
    // 10 ms to 1 s.
    require(encode_compass_control({true, true, true}, {0, 5'000'000'000, 0}) ==
            std::array<uint8_t, 20>{'A', 'N', 'C', '3', 11, 0, 0, 0, 0x10, 0x27, 0, 0,
                                    0x40, 0x42, 0x0f, 0, 0x20, 0x4e, 0, 0});
    require(encode_compass_control({false, false, true}, {10'000'000, 10'000'000, 5'000'000'000})[16] == 0x40);
    // The frame a guest sends follows its features, and an image without the
    // magnetometer ignores the magnetometer's state.
    using andarium::motion::control_frame;
    const std::array<int64_t, 3> periods{20'000'000, 10'000'000, 40'000'000};
    const auto older = control_frame({false, false}, {true, false, true}, periods, true);
    require(older.size == 16 && older.bytes[3] == '1' && older.bytes[4] == 1);
    const auto second = control_frame({true, false}, {true, false, true}, periods, true);
    require(second.size == 16 && second.bytes[3] == '2' && second.bytes[4] == 5);
    const auto third = control_frame({false, true}, {true, false, true}, periods, true);
    require(third.size == 20 && third.bytes[3] == '3' && third.bytes[4] == 9);
    const auto all = control_frame({true, true}, {true, false, true}, periods, true);
    require(all.size == 20 && all.bytes[3] == '3' && all.bytes[4] == 29);

    // What the sensor service is told about the magnetometer is within what
    // the compatibility definition (7.3.2) asks of one: a range of at least
    // 900 uT, a resolution of at most 0.6 uT and a rate of at least 10 Hz, a
    // delay range that is the frame's and a power that is positive.
    const auto& attributes = andarium::motion::magnetometer_attributes;
    require(attributes.max_range >= 900.0f && attributes.resolution > 0 && attributes.resolution <= 0.6f);
    require(attributes.min_delay_us == 20'000 && attributes.min_delay_us <= 100'000);
    require(attributes.max_delay_us == 1'000'000 && attributes.power_ma > 0);
    // The sensor service rewrites a range that is not a multiple of the resolution and rounds every value to an
    // eighth of it, so both must be exact in a float: a power of two, and the range a whole number of steps.
    const float step = attributes.resolution;
    int exponent = 0;
    require(std::frexp(step, &exponent) == 0.5f);
    require(std::fmod(attributes.max_range, step) == 0.0f);
    require(std::fmod(25.0f, step / 8) == 0.0f && std::fmod(40.0f, step / 8) == 0.0f);
}
