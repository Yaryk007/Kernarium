// SPDX-License-Identifier: Apache-2.0
//
// The wire format of the private gamepad stream between the host application
// and the guest's gamepad relay: vsock port 5558, the relay listening and the
// host connecting from CID 2, as for the motion, haptics and thermal streams.
// All fields are little endian. Every frame is an 8-byte header, the magic
// "AGP1", a uint16 type and a uint16 payload length of at most 4400, and the
// payload.
//
// Host to guest:
//   CREATE (1)            uint8 slot (0 through 7), uint8 flags, uint16 bus,
//                         uint32 vendor, uint32 product, uint32 version,
//                         uint32 country, char name[128], char uniq[64],
//                         uint16 descriptor length (1 through 4096), then the
//                         HID report descriptor. A CREATE for a used slot
//                         destroys that slot's device first.
//   DESTROY (2)           uint8 slot, three reserved bytes.
//   INPUT (3)             uint8 slot, one reserved byte, uint16 length (1
//                         through 4096), then the input report with its report
//                         ID.
//   GET_REPORT_REPLY (4)  uint8 slot, one reserved byte, uint16 error (0 or an
//                         errno), uint32 id, uint16 length (0 through 4096),
//                         then the report.
//   SET_REPORT_REPLY (5)  uint8 slot, one reserved byte, uint16 error (0 or an
//                         errno), uint32 id.
// Guest to host:
//   HELLO (16)            uint16 protocol version (1), uint8 slot count (8),
//                         one reserved byte. The relay sends it on accept, and
//                         the host sends nothing before it.
//   CREATED (17)          uint8 slot, one reserved byte, uint16 reserved,
//                         int32 result (0, or the negated errno of the open or
//                         the create of the uhid device).
//   EVENT (18)            uint8 slot, uint8 kind (0 start, 1 stop, 2 open,
//                         3 close: uhid's own events), uint16 reserved.
//   OUTPUT (19)           uint8 slot, uint8 report type, uint16 length (0
//                         through 4096), then the report the kernel driver
//                         sent to the device.
//   GET_REPORT (20)       uint8 slot, uint8 report type, uint8 report number,
//                         one reserved byte, uint32 id.
//   SET_REPORT (21)       uint8 slot, uint8 report type, uint8 report number,
//                         one reserved byte, uint32 id, uint16 length (0
//                         through 4096), then the report.
//
// The relay closes the connection on a header whose magic, type or payload
// length is wrong, on a payload whose fields break the ranges above (a slot of
// 8 or more, a length that is zero where one is required or does not fill the
// payload), and on a type that only the guest sends. The flags and every
// reserved byte are ignored on reading and zero on writing, so that the host
// can grow the format without an image. The golden frames in
// tests/unit/andarium/fixtures/gamepad-golden-frames.txt are the shared
// contract, and Andarium/AndariumCore GamepadProtocol.swift is the host side.
//
// The relay knows no controller family: the identity, the descriptor and the
// meaning of every report belong to the host.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace andarium::gamepad {
constexpr uint32_t port = 5558;
constexpr std::size_t header_size = 8;
constexpr std::size_t maximum_payload = 4400;
constexpr std::size_t slot_count = 8;
constexpr std::size_t maximum_descriptor = 4096;
constexpr std::size_t maximum_report = 4096;
constexpr std::size_t name_size = 128;
constexpr std::size_t uniq_size = 64;
constexpr uint16_t protocol_version = 1;
constexpr std::array<uint8_t, 4> magic{'A', 'G', 'P', '1'};

// Where each payload's fixed part ends: the variable part (a descriptor or a
// report) follows it, and the length field that says how long it is lies inside
// it.
constexpr std::size_t create_fixed_size = 214;
constexpr std::size_t destroy_payload_size = 4;
constexpr std::size_t input_fixed_size = 4;
constexpr std::size_t get_report_reply_fixed_size = 10;
constexpr std::size_t set_report_reply_payload_size = 8;
constexpr std::size_t hello_payload_size = 4;
constexpr std::size_t created_payload_size = 8;
constexpr std::size_t event_payload_size = 4;
constexpr std::size_t output_fixed_size = 4;
constexpr std::size_t get_report_payload_size = 8;
constexpr std::size_t set_report_fixed_size = 10;

enum class Type : uint16_t {
    create = 1,
    destroy = 2,
    input = 3,
    get_report_reply = 4,
    set_report_reply = 5,
    hello = 16,
    created = 17,
    event = 18,
    output = 19,
    get_report = 20,
    set_report = 21,
};

// What uhid reports about a device (UHID_START, UHID_STOP, UHID_OPEN and
// UHID_CLOSE), in the order of its event numbers.
enum class EventKind : uint8_t { start = 0, stop = 1, open = 2, close = 3 };

namespace detail {
inline void put(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
}
inline void put(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}
inline void put_header(std::vector<uint8_t>& out, Type type, std::size_t length) {
    out.insert(out.end(), magic.begin(), magic.end());
    put(out, uint16_t(type));
    put(out, uint16_t(length));
}
inline uint16_t get16(std::span<const uint8_t> bytes, std::size_t offset) {
    return uint16_t(bytes[offset] | (bytes[offset + 1] << 8));
}
inline uint32_t get32(std::span<const uint8_t> bytes, std::size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
           (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}
}  // namespace detail

// ---- Host to guest

// The smallest and the largest payload the host may send in a frame of `type`,
// or nothing for a type the host never sends.
struct PayloadLimits {
    std::size_t minimum;
    std::size_t maximum;
};
constexpr std::optional<PayloadLimits> host_payload_limits(uint16_t type) {
    switch (Type(type)) {
        case Type::create:
            return PayloadLimits{create_fixed_size + 1, create_fixed_size + maximum_descriptor};
        case Type::destroy:
            return PayloadLimits{destroy_payload_size, destroy_payload_size};
        case Type::input:
            return PayloadLimits{input_fixed_size + 1, input_fixed_size + maximum_report};
        case Type::get_report_reply:
            return PayloadLimits{get_report_reply_fixed_size, get_report_reply_fixed_size + maximum_report};
        case Type::set_report_reply:
            return PayloadLimits{set_report_reply_payload_size, set_report_reply_payload_size};
        default:
            return std::nullopt;
    }
}

struct Header {
    Type type;
    uint16_t length;
};

// The header at the start of `bytes`, or nothing when the bytes are not the
// header of a frame the host may send: a wrong magic, a type that is not a
// host type, or a payload length outside what the type allows. The header
// alone decides, so a frame that claims more than the limit is refused before
// its payload is read.
inline std::optional<Header> parse_host_header(std::span<const uint8_t> bytes) {
    if (bytes.size() < header_size || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        return std::nullopt;
    }
    const uint16_t type = detail::get16(bytes, 4);
    const uint16_t length = detail::get16(bytes, 6);
    const auto limits = host_payload_limits(type);
    if (!limits || length > maximum_payload || length < limits->minimum || length > limits->maximum) {
        return std::nullopt;
    }
    return Header{Type(type), length};
}

// The frames of the host out of a byte stream that arrives in any pieces. A
// header that is not one the host may send ends the stream's use: feed()
// returns false, the bytes after it are dropped and the owner closes the
// connection. The frames that were whole before the bad header are handled
// first.
class FrameReader {
public:
    // Calls `handle(Type, payload)` for every whole frame, in order; the
    // payload is valid until `handle` returns. `handle` returns false to end
    // the stream's use, as for a payload that breaks the rules.
    template <typename Handler>
    bool feed(std::span<const uint8_t> bytes, Handler&& handle) {
        if (failed_) return false;
        while (!bytes.empty()) {
            if (used_ < header_size) {
                const auto count = std::min(header_size - used_, bytes.size());
                std::memcpy(buffer_.data() + used_, bytes.data(), count);
                used_ += count;
                bytes = bytes.subspan(count);
                if (used_ < header_size) break;
                const auto header = parse_host_header(std::span(buffer_).first(header_size));
                if (!header) return fail();
                type_ = header->type;
                length_ = header->length;
            }
            const auto total = header_size + length_;
            const auto count = std::min(total - used_, bytes.size());
            std::memcpy(buffer_.data() + used_, bytes.data(), count);
            used_ += count;
            bytes = bytes.subspan(count);
            if (used_ < total) break;
            used_ = 0;
            if (!handle(type_, std::span<const uint8_t>(buffer_).subspan(header_size, length_))) return fail();
        }
        return true;
    }

    void reset() {
        used_ = 0;
        failed_ = false;
    }

private:
    bool fail() {
        used_ = 0;
        failed_ = true;
        return false;
    }

    std::array<uint8_t, header_size + maximum_payload> buffer_{};
    std::size_t used_ = 0;
    std::size_t length_ = 0;
    Type type_ = Type::destroy;
    bool failed_ = false;
};

// The payloads, each read from exactly the bytes of one frame. A payload whose
// slot or length breaks the ranges is nothing. The spans point into `payload`.
struct Create {
    uint8_t slot;
    uint16_t bus;
    uint32_t vendor;
    uint32_t product;
    uint32_t version;
    uint32_t country;
    std::span<const uint8_t> name;        // name_size bytes
    std::span<const uint8_t> uniq;        // uniq_size bytes
    std::span<const uint8_t> descriptor;  // 1 through maximum_descriptor bytes
};

inline std::optional<Create> decode_create(std::span<const uint8_t> payload) {
    if (payload.size() <= create_fixed_size || payload[0] >= slot_count) return std::nullopt;
    const auto length = detail::get16(payload, 212);
    if (length == 0 || length > maximum_descriptor || payload.size() != create_fixed_size + length) {
        return std::nullopt;
    }
    return Create{payload[0],
                  detail::get16(payload, 2),
                  detail::get32(payload, 4),
                  detail::get32(payload, 8),
                  detail::get32(payload, 12),
                  detail::get32(payload, 16),
                  payload.subspan(20, name_size),
                  payload.subspan(20 + name_size, uniq_size),
                  payload.subspan(create_fixed_size, length)};
}

struct Destroy {
    uint8_t slot;
};

inline std::optional<Destroy> decode_destroy(std::span<const uint8_t> payload) {
    if (payload.size() != destroy_payload_size || payload[0] >= slot_count) return std::nullopt;
    return Destroy{payload[0]};
}

struct Input {
    uint8_t slot;
    std::span<const uint8_t> report;  // 1 through maximum_report bytes
};

inline std::optional<Input> decode_input(std::span<const uint8_t> payload) {
    if (payload.size() <= input_fixed_size || payload[0] >= slot_count) return std::nullopt;
    const auto length = detail::get16(payload, 2);
    if (length == 0 || length > maximum_report || payload.size() != input_fixed_size + length) return std::nullopt;
    return Input{payload[0], payload.subspan(input_fixed_size, length)};
}

struct GetReportReply {
    uint8_t slot;
    uint16_t error;
    uint32_t id;
    std::span<const uint8_t> data;  // 0 through maximum_report bytes
};

inline std::optional<GetReportReply> decode_get_report_reply(std::span<const uint8_t> payload) {
    if (payload.size() < get_report_reply_fixed_size || payload[0] >= slot_count) return std::nullopt;
    const auto length = detail::get16(payload, 8);
    if (length > maximum_report || payload.size() != get_report_reply_fixed_size + length) return std::nullopt;
    return GetReportReply{payload[0], detail::get16(payload, 2), detail::get32(payload, 4),
                          payload.subspan(get_report_reply_fixed_size, length)};
}

struct SetReportReply {
    uint8_t slot;
    uint16_t error;
    uint32_t id;
};

inline std::optional<SetReportReply> decode_set_report_reply(std::span<const uint8_t> payload) {
    if (payload.size() != set_report_reply_payload_size || payload[0] >= slot_count) return std::nullopt;
    return SetReportReply{payload[0], detail::get16(payload, 2), detail::get32(payload, 4)};
}

// ---- Guest to host

inline std::vector<uint8_t> encode_hello() {
    std::vector<uint8_t> frame;
    frame.reserve(header_size + hello_payload_size);
    detail::put_header(frame, Type::hello, hello_payload_size);
    detail::put(frame, protocol_version);
    frame.push_back(uint8_t(slot_count));
    frame.push_back(0);
    return frame;
}

inline std::vector<uint8_t> encode_created(uint8_t slot, int32_t result) {
    std::vector<uint8_t> frame;
    frame.reserve(header_size + created_payload_size);
    detail::put_header(frame, Type::created, created_payload_size);
    frame.push_back(slot);
    frame.push_back(0);
    detail::put(frame, uint16_t(0));
    detail::put(frame, uint32_t(result));
    return frame;
}

inline std::vector<uint8_t> encode_event(uint8_t slot, EventKind kind) {
    std::vector<uint8_t> frame;
    frame.reserve(header_size + event_payload_size);
    detail::put_header(frame, Type::event, event_payload_size);
    frame.push_back(slot);
    frame.push_back(uint8_t(kind));
    detail::put(frame, uint16_t(0));
    return frame;
}

// An OUTPUT frame, or nothing for a report over maximum_report bytes.
inline std::optional<std::vector<uint8_t>> encode_output(uint8_t slot, uint8_t report_type,
                                                         std::span<const uint8_t> report) {
    if (report.size() > maximum_report) return std::nullopt;
    std::vector<uint8_t> frame;
    frame.reserve(header_size + output_fixed_size + report.size());
    detail::put_header(frame, Type::output, output_fixed_size + report.size());
    frame.push_back(slot);
    frame.push_back(report_type);
    detail::put(frame, uint16_t(report.size()));
    frame.insert(frame.end(), report.begin(), report.end());
    return frame;
}

inline std::vector<uint8_t> encode_get_report(uint8_t slot, uint8_t report_type, uint8_t number, uint32_t id) {
    std::vector<uint8_t> frame;
    frame.reserve(header_size + get_report_payload_size);
    detail::put_header(frame, Type::get_report, get_report_payload_size);
    frame.push_back(slot);
    frame.push_back(report_type);
    frame.push_back(number);
    frame.push_back(0);
    detail::put(frame, id);
    return frame;
}

// A SET_REPORT frame, or nothing for a report over maximum_report bytes.
inline std::optional<std::vector<uint8_t>> encode_set_report(uint8_t slot, uint8_t report_type, uint8_t number,
                                                             uint32_t id, std::span<const uint8_t> report) {
    if (report.size() > maximum_report) return std::nullopt;
    std::vector<uint8_t> frame;
    frame.reserve(header_size + set_report_fixed_size + report.size());
    detail::put_header(frame, Type::set_report, set_report_fixed_size + report.size());
    frame.push_back(slot);
    frame.push_back(report_type);
    frame.push_back(number);
    frame.push_back(0);
    detail::put(frame, id);
    detail::put(frame, uint16_t(report.size()));
    frame.insert(frame.end(), report.begin(), report.end());
    return frame;
}
}  // namespace andarium::gamepad
