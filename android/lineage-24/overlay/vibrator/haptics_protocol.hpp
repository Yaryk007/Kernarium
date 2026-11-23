// SPDX-License-Identifier: Apache-2.0
//
// The wire format of the private haptics stream between the guest's vibrator
// HAL and the host application: vsock port 5557, the HAL listening and the
// host connecting from CID 2, as for the motion stream. All fields are little
// endian. Every frame is an 8-byte header, the magic "AHV1", a uint16 type and
// a uint16 payload length of at most 4096, and the payload.
//
// Guest to host:
//   PLAY (1)      uint8 target, uint8 flags (0), uint16 count (1 through 128),
//                 uint32 sequence, then `count` events of 20 bytes (Event).
//   INTENSITY (2) uint8 target, three reserved bytes, uint32 sequence, float32
//                 intensity in (0, 1].
//   STOP (3)      uint8 target, three reserved bytes, uint32 sequence.
// Host to guest:
//   ACK (16)      uint32 sequence, uint32 microseconds from receipt to the
//                 player's start (0 for a dropped frame), uint8 result.
//
// The host closes the connection on a frame that breaks these rules, so the
// encoders refuse what the host would refuse. The golden frames in
// tests/unit/andarium/fixtures/haptics-golden-frames.txt are the shared
// contract, and Andarium/AndariumCore HapticsProtocol.swift is the host side.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace andarium::haptics {
constexpr uint32_t port = 5557;
constexpr std::size_t header_size = 8;
constexpr std::size_t maximum_payload = 4096;
constexpr std::size_t maximum_events = 128;
constexpr std::size_t event_size = 20;
constexpr std::size_t play_header_size = 8;
constexpr std::size_t intensity_payload_size = 12;
constexpr std::size_t stop_payload_size = 8;
constexpr std::size_t ack_payload_size = 9;
constexpr std::size_t ack_frame_size = header_size + ack_payload_size;
constexpr std::array<uint8_t, 4> magic{'A', 'H', 'V', '1'};
// The device's own actuator. A larger number names another actuator of the
// host, which the vibrator HAL never plays.
constexpr uint8_t device_target = 0;

enum class Type : uint16_t { play = 1, intensity = 2, stop = 3, ack = 16 };
enum class Kind : uint8_t { tone = 0, primitive = 1, effect = 2 };
// What the host reports in an ACK. It is for diagnostics: the HAL counts the
// results and never waits for one.
enum class AckResult : uint8_t { played = 0, not_in_front = 1, engine_failed = 2 };

// One element of a PLAY frame. `start_ms` and `duration_ms` are measured from
// the start of the frame, and the host plays nothing past start + duration.
//   tone       a buzz: code 0, strength 0, `intensity` in (0, 1], `sharpness`
//              in [0, 1];
//   primitive  a CompositePrimitive: `code` is its number, `intensity` its
//              scale in [0, 1], sharpness 0;
//   effect     a predefined Effect: `code` is its number, `strength` an
//              EffectStrength (0 through 2), `intensity` 1, sharpness 0.
struct Event {
    Kind kind = Kind::tone;
    uint8_t code = 0;
    uint8_t strength = 0;
    uint32_t start_ms = 0;
    uint32_t duration_ms = 0;
    float intensity = 1;
    float sharpness = 0;
};

// Whether the host accepts the event.
inline bool valid(const Event& event) {
    if (!std::isfinite(event.intensity) || !std::isfinite(event.sharpness)) return false;
    switch (event.kind) {
        case Kind::tone:
            return event.code == 0 && event.strength == 0 && event.intensity > 0 &&
                   event.intensity <= 1 && event.sharpness >= 0 && event.sharpness <= 1;
        case Kind::primitive:
            return event.strength == 0 && event.intensity >= 0 && event.intensity <= 1 &&
                   event.sharpness == 0;
        case Kind::effect:
            return event.strength <= 2 && event.sharpness == 0;
    }
    return false;
}

namespace detail {
inline void put(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
}
inline void put(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(uint8_t(value >> shift));
}
inline void put(std::vector<uint8_t>& out, float value) {
    put(out, std::bit_cast<uint32_t>(value));
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

// A PLAY frame, or nothing when `events` is empty, holds more than 128
// elements or holds one the host would refuse.
inline std::optional<std::vector<uint8_t>> encode_play(uint8_t target, uint32_t sequence,
                                                       std::span<const Event> events) {
    if (events.empty() || events.size() > maximum_events) return std::nullopt;
    for (const auto& event : events) {
        if (!valid(event)) return std::nullopt;
    }
    std::vector<uint8_t> frame;
    const std::size_t length = play_header_size + events.size() * event_size;
    frame.reserve(header_size + length);
    detail::put_header(frame, Type::play, length);
    frame.push_back(target);
    frame.push_back(0);
    detail::put(frame, uint16_t(events.size()));
    detail::put(frame, sequence);
    for (const auto& event : events) {
        frame.push_back(uint8_t(event.kind));
        frame.push_back(event.code);
        frame.push_back(event.strength);
        frame.push_back(0);
        detail::put(frame, event.start_ms);
        detail::put(frame, event.duration_ms);
        detail::put(frame, event.intensity);
        detail::put(frame, event.sharpness);
    }
    return frame;
}

// An INTENSITY frame, or nothing for an intensity outside (0, 1].
inline std::optional<std::vector<uint8_t>> encode_intensity(uint8_t target, uint32_t sequence,
                                                            float intensity) {
    if (!(intensity > 0 && intensity <= 1)) return std::nullopt;
    std::vector<uint8_t> frame;
    frame.reserve(header_size + intensity_payload_size);
    detail::put_header(frame, Type::intensity, intensity_payload_size);
    frame.push_back(target);
    frame.insert(frame.end(), 3, 0);
    detail::put(frame, sequence);
    detail::put(frame, intensity);
    return frame;
}

inline std::vector<uint8_t> encode_stop(uint8_t target, uint32_t sequence) {
    std::vector<uint8_t> frame;
    frame.reserve(header_size + stop_payload_size);
    detail::put_header(frame, Type::stop, stop_payload_size);
    frame.push_back(target);
    frame.insert(frame.end(), 3, 0);
    detail::put(frame, sequence);
    return frame;
}

// The host's answer to a frame. `result` is kept as the host sent it: a value
// beyond AckResult is a newer host's, and counts as such.
struct Ack {
    uint32_t sequence = 0;
    uint32_t microseconds = 0;
    uint8_t result = 0;
};

// Whether the first 8 bytes are an ACK's header.
inline bool is_ack_header(std::span<const uint8_t> header) {
    if (header.size() < header_size) return false;
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (header[i] != magic[i]) return false;
    }
    return detail::get16(header, 4) == uint16_t(Type::ack) &&
           detail::get16(header, 6) == ack_payload_size;
}

// The ACK in one whole frame (17 bytes), or nothing when the bytes are not one.
inline std::optional<Ack> decode_ack(std::span<const uint8_t> frame) {
    if (frame.size() != ack_frame_size || !is_ack_header(frame)) return std::nullopt;
    return Ack{detail::get32(frame, 8), detail::get32(frame, 12), frame[16]};
}

// The host's ACKs out of a byte stream that arrives in any pieces. A frame that
// is not an ACK ends the stream's use: `failed` is set, and the owner closes
// the connection. The ACKs that arrived before the bad frame still count.
class AckReader {
public:
    struct Output {
        std::vector<Ack> acks;
        bool failed = false;
    };

    Output feed(std::span<const uint8_t> bytes) {
        Output output;
        while (!bytes.empty()) {
            const auto take = std::min(bytes.size(), buffer_.size() - used_);
            std::copy_n(bytes.begin(), take, buffer_.begin() + std::ptrdiff_t(used_));
            used_ += take;
            bytes = bytes.subspan(take);
            if (used_ >= header_size && !is_ack_header(buffer_)) {
                used_ = 0;
                output.failed = true;
                return output;
            }
            if (used_ < buffer_.size()) break;
            const auto ack = decode_ack(buffer_);
            used_ = 0;
            if (!ack) {
                output.failed = true;
                return output;
            }
            output.acks.push_back(*ack);
        }
        return output;
    }

    void reset() { used_ = 0; }

private:
    std::array<uint8_t, ack_frame_size> buffer_{};
    std::size_t used_ = 0;
};
}  // namespace andarium::haptics
