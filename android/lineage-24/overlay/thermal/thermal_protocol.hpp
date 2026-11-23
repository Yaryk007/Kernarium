// SPDX-License-Identifier: Apache-2.0
//
// The wire format of the private thermal stream between the host application
// and the guest's thermal HAL: vsock port 5559, the HAL listening and the host
// connecting from CID 2, as for the motion and haptics streams. It carries one
// direction only, from host to guest.
//
// Every record is 8 bytes: the magic "ATH1", one byte with the Android
// severity the host wants reported (0 NONE, 1 LIGHT, 2 MODERATE, 3 SEVERE,
// 4 CRITICAL) and three reserved bytes that are zero. The host sends one
// record when it connects and one whenever the severity changes. Android's
// EMERGENCY and SHUTDOWN have no value: a record that names one is refused,
// and the host application never sends one.
//
// The HAL closes the connection on a record that breaks these rules, and keeps
// the severity it had. The golden frames in
// tests/unit/andarium/fixtures/thermal-golden-frames.txt are the shared
// contract, and Andarium/AndariumCore ThermalProtocol.swift is the host side.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace andarium::thermal {
constexpr uint32_t port = 5559;
constexpr std::size_t record_size = 8;
constexpr std::array<uint8_t, 4> magic{'A', 'T', 'H', '1'};

// The values are android.hardware.thermal ThrottlingSeverity's, up to CRITICAL.
enum class Severity : uint8_t { none = 0, light = 1, moderate = 2, severe = 3, critical = 4 };
constexpr uint8_t highest_severity = uint8_t(Severity::critical);

// Whether `byte` may stand at `position` of a record. The check is per byte so
// that a record that is wrong is known as soon as its wrong byte arrives.
constexpr bool acceptable_byte(std::size_t position, uint8_t byte) {
    if (position < magic.size()) return byte == magic[position];
    if (position == magic.size()) return byte <= highest_severity;
    return byte == 0;
}

// The severity in one whole record, or nothing when the bytes are not a record.
inline std::optional<Severity> decode_record(std::span<const uint8_t> record) {
    if (record.size() != record_size) return std::nullopt;
    for (std::size_t position = 0; position < record_size; ++position) {
        if (!acceptable_byte(position, record[position])) return std::nullopt;
    }
    return Severity(record[magic.size()]);
}

// The records out of a byte stream that arrives in any pieces. A byte that
// breaks the rules ends the stream's use: `failed` is set, the bytes after it
// are dropped and the owner closes the connection. The records that arrived
// before the bad byte still count.
class RecordReader {
public:
    struct Output {
        std::vector<Severity> records;
        bool failed = false;
    };

    Output feed(std::span<const uint8_t> bytes) {
        Output output;
        for (const auto byte : bytes) {
            if (!acceptable_byte(used_, byte)) {
                used_ = 0;
                output.failed = true;
                return output;
            }
            buffer_[used_++] = byte;
            if (used_ == record_size) {
                used_ = 0;
                output.records.push_back(Severity(buffer_[magic.size()]));
            }
        }
        return output;
    }

    void reset() { used_ = 0; }

private:
    std::array<uint8_t, record_size> buffer_{};
    std::size_t used_ = 0;
};
}  // namespace andarium::thermal
