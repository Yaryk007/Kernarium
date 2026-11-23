// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest's end of the haptics wire (vsock 5557) against the golden frames
// that Andarium's HapticsProtocol.swift checks too, and the timelines the
// vibrator HAL builds from the framework's calls. argv[1] is the fixture,
// tests/unit/andarium/fixtures/haptics-golden-frames.txt.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "haptics_effects.hpp"
#include "haptics_protocol.hpp"
#include "require.hpp"

using namespace andarium::haptics;
using uml::testing::fail;
using uml::testing::require;
using Bytes = std::vector<uint8_t>;

namespace {
Bytes parse_hex(const std::string& text) {
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
std::map<std::string, Bytes> read_fixture(const char* path) {
    std::ifstream file(path);
    require(file.is_open(), "the golden-frame fixture opens");
    std::map<std::string, Bytes> frames;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto colon = line.find(':');
        require(colon != std::string::npos, "a fixture line is name: bytes");
        const auto name = line.substr(0, colon);
        require(frames.emplace(name, parse_hex(line.substr(colon + 1))).second, "fixture names are unique");
    }
    return frames;
}

uint32_t word(const Bytes& bytes, std::size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
           (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}
uint16_t half(const Bytes& bytes, std::size_t offset) { return uint16_t(bytes[offset] | (bytes[offset + 1] << 8)); }
float real(const Bytes& bytes, std::size_t offset) { return std::bit_cast<float>(word(bytes, offset)); }

// The host's rules for a frame the guest sends (HapticsProtocol.swift), written
// out here independently of the encoder's own checks: the oracle for what the
// encoder produces and for the fixture's `reject-` frames.
bool host_accepts(const Bytes& frame) {
    if (frame.size() < 8 || frame[0] != 'A' || frame[1] != 'H' || frame[2] != 'V' || frame[3] != '1') return false;
    const auto type = half(frame, 4);
    const std::size_t length = half(frame, 6);
    if (type < 1 || type > 3 || length > 4096) return false;
    if (type == 1) {
        if (length <= 8 || (length - 8) % 20 != 0 || (length - 8) / 20 > 128) return false;
    } else if (type == 2 && length != 12) {
        return false;
    } else if (type == 3 && length != 8) {
        return false;
    }
    if (frame.size() != 8 + length) return false;
    const Bytes payload(frame.begin() + 8, frame.end());
    if (type == 1) {
        const std::size_t count = half(payload, 2);
        if (payload[1] != 0 || count < 1 || count > 128 || payload.size() != 8 + count * 20) return false;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t at = 8 + i * 20;
            const auto kind = payload[at];
            const auto code = payload[at + 1];
            const auto strength = payload[at + 2];
            const auto intensity = real(payload, at + 12);
            const auto sharpness = real(payload, at + 16);
            if (kind > 2 || payload[at + 3] != 0) return false;
            if (!std::isfinite(intensity) || !std::isfinite(sharpness)) return false;
            if (kind == 0 && !(code == 0 && strength == 0 && intensity > 0 && intensity <= 1 &&
                               sharpness >= 0 && sharpness <= 1)) {
                return false;
            }
            if (kind == 1 && !(strength == 0 && intensity >= 0 && intensity <= 1 && sharpness == 0)) return false;
            if (kind == 2 && !(strength <= 2 && sharpness == 0)) return false;
        }
        return true;
    }
    if (payload[1] != 0 || payload[2] != 0 || payload[3] != 0) return false;
    if (type == 2) {
        const auto intensity = real(payload, 8);
        return std::isfinite(intensity) && intensity > 0 && intensity <= 1;
    }
    return true;
}

Bytes play_of(const Built& built, uint32_t sequence, uint8_t target = device_target) {
    require(built.outcome == Outcome::ok, "the call is accepted");
    const auto frame = encode_play(target, sequence, built.timeline.events);
    require(frame.has_value(), "the timeline encodes");
    return *frame;
}

void check_fixture_frames(const std::map<std::string, Bytes>& fixture) {
    const auto frame = [&](const char* name) -> const Bytes& {
        const auto found = fixture.find(name);
        require(found != fixture.end(), name);
        return found->second;
    };

    // Each guest frame is what its call produces.
    const CompositionStep click{0, primitive::click, 1.0f};
    require(play_of(build_composition(std::span(&click, 1)), 7) == frame("play-primitive-click"),
            "play-primitive-click");
    require(play_of(build_tone(500, 1.0f), 1) == frame("play-tone"), "play-tone");
    require(play_of(build_effect(effect::double_click, 1), 2) == frame("play-effect-double-click"),
            "play-effect-double-click");
    require(play_of(build_effect(effect::texture_tick, 0), 3) == frame("play-effect-texture-tick"),
            "play-effect-texture-tick");
    const std::vector<CompositionStep> composition{{0, primitive::click, 0.5f}, {50, primitive::thud, 1.0f}};
    require(play_of(build_composition(composition), 4) == frame("play-composition"), "play-composition");
    const std::vector<Event> second{Event{Kind::tone, 0, 0, 0, 100, 0.25f, 0.8f}};
    require(*encode_play(1, 5, second) == frame("play-second-target"), "play-second-target");
    require(*encode_intensity(device_target, 6, 0.5f) == frame("intensity-half"), "intensity-half");
    require(encode_stop(device_target, 8) == frame("stop"), "stop");

    // The ACKs the host sends.
    const auto played = decode_ack(frame("ack-played"));
    require(played && played->sequence == 7 && played->microseconds == 300 &&
                played->result == uint8_t(AckResult::played),
            "ack-played");
    const auto unseen = decode_ack(frame("ack-not-in-front"));
    require(unseen && unseen->sequence == 9 && unseen->microseconds == 0 &&
                unseen->result == uint8_t(AckResult::not_in_front),
            "ack-not-in-front");
    const auto failed = decode_ack(frame("ack-engine-failed"));
    require(failed && failed->sequence == 10 && failed->microseconds == 1500 &&
                failed->result == uint8_t(AckResult::engine_failed),
            "ack-engine-failed");

    // The host's rules accept what the guest sends and refuse each `reject-`
    // frame, so the oracle above and the fixture agree.
    for (const auto& [name, bytes] : fixture) {
        if (name.starts_with("play-") || name.starts_with("intensity-") || name == "stop") {
            require(host_accepts(bytes), "the host accepts " + name);
        } else if (name.starts_with("reject-")) {
            require(!host_accepts(bytes), "the host refuses " + name);
        } else {
            require(name.starts_with("ack-"), "an unknown fixture frame " + name);
            require(!host_accepts(bytes), "an ACK is not a guest frame: " + name);
        }
    }
    require(fixture.size() >= 20, "the fixture holds the guest's frames, the ACKs and the rejects");
}

void check_ack_reader(const std::map<std::string, Bytes>& fixture) {
    Bytes all;
    for (const char* name : {"ack-played", "ack-not-in-front", "ack-engine-failed"}) {
        all.insert(all.end(), fixture.at(name).begin(), fixture.at(name).end());
    }
    // The three frames arrive whole, one byte at a time, and in uneven pieces.
    AckReader whole;
    const auto together = whole.feed(all);
    require(!together.failed && together.acks.size() == 3 && together.acks[2].sequence == 10, "whole frames");
    AckReader single;
    std::vector<Ack> seen;
    for (const auto byte : all) {
        const auto output = single.feed(std::span(&byte, 1));
        require(!output.failed, "one byte at a time");
        seen.insert(seen.end(), output.acks.begin(), output.acks.end());
    }
    require(seen.size() == 3 && seen[0].sequence == 7 && seen[1].sequence == 9 && seen[2].microseconds == 1500,
            "one byte at a time");
    AckReader pieces;
    seen.clear();
    for (std::size_t at = 0, size = 5; at < all.size(); at += size, size = size % 11 + 3) {
        const auto piece = std::span(all).subspan(at, std::min(size, all.size() - at));
        const auto output = pieces.feed(piece);
        require(!output.failed, "uneven pieces");
        seen.insert(seen.end(), output.acks.begin(), output.acks.end());
    }
    require(seen.size() == 3, "uneven pieces");

    // A result a newer host defines still reads, as a number.
    Bytes newer = fixture.at("ack-played");
    newer[16] = 9;
    require(decode_ack(newer) && decode_ack(newer)->result == 9, "an unknown result is kept");

    // Anything that is not an ACK ends the stream, and the ACKs before it count.
    for (const auto& [label, edit] : std::map<std::string, std::pair<std::size_t, uint8_t>>{
             {"magic", {0, 'B'}}, {"type", {4, 1}}, {"length", {6, 8}}}) {
        Bytes bad = fixture.at("ack-played");
        bad[edit.first] = edit.second;
        AckReader reader;
        require(reader.feed(bad).failed, "a bad " + label);
        require(!decode_ack(bad), "a bad " + label);
    }
    Bytes trailing = fixture.at("ack-played");
    trailing.insert(trailing.end(), fixture.at("play-tone").begin(), fixture.at("play-tone").begin() + 17);
    AckReader reader;
    const auto output = reader.feed(trailing);
    require(output.acks.size() == 1 && output.failed, "a frame after an ACK that is not one");
    require(!decode_ack(std::span(fixture.at("ack-played")).first(16)), "a short ACK");
    // The header check does not wait for the payload.
    AckReader early;
    require(early.feed(std::span(fixture.at("play-tone")).first(8)).failed, "a PLAY header is refused at once");
}

void check_encoders_refuse() {
    const Event tone{Kind::tone, 0, 0, 0, 10, 1.0f, 0.5f};
    require(!encode_play(0, 1, {}), "no event");
    require(!encode_play(0, 1, std::vector<Event>(maximum_events + 1, tone)), "129 events");
    const auto largest = encode_play(0, 1, std::vector<Event>(maximum_events, tone));
    require(largest && largest->size() == header_size + play_header_size + maximum_events * event_size &&
                largest->size() <= header_size + maximum_payload && host_accepts(*largest),
            "128 events fit the frame");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const Event& bad : {
             Event{Kind::tone, 0, 0, 0, 10, 0.0f, 0.5f},       // no intensity
             Event{Kind::tone, 0, 0, 0, 10, 1.5f, 0.5f},       // above 1
             Event{Kind::tone, 0, 0, 0, 10, nan, 0.5f},        // not a number
             Event{Kind::tone, 0, 0, 0, 10, 1.0f, infinity},   // not finite
             Event{Kind::tone, 0, 0, 0, 10, 1.0f, -0.1f},      // below 0
             Event{Kind::tone, 1, 0, 0, 10, 1.0f, 0.5f},       // a tone has no code
             Event{Kind::primitive, 1, 1, 0, 10, 1.0f, 0.0f},  // a primitive has no strength
             Event{Kind::primitive, 1, 0, 0, 10, 1.1f, 0.0f},  // a scale above 1
             Event{Kind::primitive, 1, 0, 0, 10, 1.0f, 0.5f},  // a primitive has no sharpness
             Event{Kind::effect, 1, 3, 0, 10, 1.0f, 0.0f},     // strength 3
             Event{Kind::effect, 1, 0, 0, 10, 1.0f, 0.2f},     // an effect has no sharpness
         }) {
        require(!encode_play(0, 1, std::span(&bad, 1)), "the encoder refuses what the host refuses");
    }
    for (const float bad : {0.0f, -0.5f, 1.01f, nan, infinity}) {
        require(!encode_intensity(0, 1, bad), "an intensity outside (0, 1]");
    }
    require(encode_intensity(0, 1, 1.0f) && encode_intensity(0, 1, 0.001f), "intensities in (0, 1]");
}

// The framework's calls become timelines; the checks are the VTS's and CTS's.
void check_effects() {
    // Every offered effect and strength builds a frame the host accepts, and
    // reports the duration its callback waits for.
    require(effects.size() == 7, "seven predefined effects");
    for (const auto& entry : effects) {
        require(entry.duration_ms > 0 && entry.code >= 0 && entry.code <= 21 && (entry.code < 6 || entry.code == 21),
                "an effect is a non-ringtone effect with a length");
        for (int32_t strength = 0; strength <= 2; ++strength) {
            const auto built = build_effect(entry.code, strength);
            require(built.outcome == Outcome::ok && built.timeline.total_ms == entry.duration_ms &&
                        built.timeline.events.size() == 1,
                    "an effect builds");
            const auto& event = built.timeline.events[0];
            require(event.kind == Kind::effect && event.code == entry.code && event.strength == strength &&
                        event.start_ms == 0 && event.duration_ms == entry.duration_ms && event.intensity == 1.0f,
                    "an effect's event");
            require(host_accepts(play_of(built, 1)), "an effect's frame");
        }
    }
    // Everything else is unsupported: the ringtones, values outside the enum
    // and strengths outside it (the VTS tries them all).
    for (int32_t code = -2; code <= 24; ++code) {
        for (int32_t strength = -2; strength <= 4; ++strength) {
            const auto built = build_effect(code, strength);
            const bool offered = effect_duration_ms(code).has_value() && strength >= 0 && strength <= 2;
            require(built.outcome == (offered ? Outcome::ok : Outcome::unsupported), "an effect or strength not offered");
        }
    }

    // Primitives: the VTS requires these five, each with a length, and NOOP has none.
    for (const int32_t required : {primitive::click, primitive::light_tick, primitive::quick_rise,
                                   primitive::slow_rise, primitive::quick_fall}) {
        require(primitive_duration_ms(required).value_or(0) > 0, "a required primitive");
    }
    require(primitive_duration_ms(primitive::noop) == 0u, "NOOP is a delay");
    require(primitives.size() == 9, "nine primitives");
    for (const auto& entry : primitives) {
        require(entry.code == primitive::noop || entry.duration_ms > 0, "a primitive has a length");
    }
    require(!primitive_duration_ms(-1) && !primitive_duration_ms(9), "values outside the enum");

    // A composition: the golden one, then the rules of compose().
    const std::vector<CompositionStep> golden{{0, primitive::click, 0.5f}, {50, primitive::thud, 1.0f}};
    auto built = build_composition(golden);
    require(built.outcome == Outcome::ok && built.timeline.total_ms == 122 && built.timeline.events.size() == 2 &&
                built.timeline.events[1].start_ms == 62 && built.timeline.events[1].duration_ms == 60,
            "an element starts when the one before ends, plus its delay");
    // NOOP only advances the clock and adds no event.
    const std::vector<CompositionStep> padded{{100, primitive::noop, 1.0f}, {20, primitive::click, 1.0f}};
    built = build_composition(padded);
    require(built.outcome == Outcome::ok && built.timeline.events.size() == 1 &&
                built.timeline.events[0].start_ms == 120 && built.timeline.total_ms == 132,
            "NOOP and a delay");
    const std::vector<CompositionStep> only_delays{{30, primitive::noop, 0.0f}, {40, primitive::noop, 0.0f}};
    built = build_composition(only_delays);
    require(built.outcome == Outcome::ok && built.timeline.events.empty() && built.timeline.total_ms == 70,
            "a composition of delays plays nothing");
    require(build_composition({}).outcome == Outcome::ok && build_composition({}).timeline.total_ms == 0,
            "an empty composition");

    const auto step = [](int32_t delay, int32_t code, float scale) { return std::vector<CompositionStep>{{delay, code, scale}}; };
    require(build_composition(step(composition_delay_max_ms, primitive::click, 1.0f)).outcome == Outcome::ok,
            "the longest delay");
    require(build_composition(step(composition_delay_max_ms + 1, primitive::click, 1.0f)).outcome ==
                Outcome::illegal_argument,
            "a delay beyond the maximum");
    require(build_composition(step(-1, primitive::click, 1.0f)).outcome == Outcome::illegal_argument, "a negative delay");
    require(build_composition(step(0, primitive::click, 0.0f)).outcome == Outcome::ok, "scale 0");
    require(build_composition(step(0, primitive::click, std::nextafter(0.0f, -1.0f))).outcome ==
                Outcome::illegal_argument,
            "a scale below 0");
    require(build_composition(step(0, primitive::click, std::nextafter(1.0f, 2.0f))).outcome ==
                Outcome::illegal_argument,
            "a scale above 1");
    require(build_composition(step(0, primitive::click, std::numeric_limits<float>::quiet_NaN())).outcome ==
                Outcome::illegal_argument,
            "a scale that is not a number");
    require(build_composition(step(0, -1, 1.0f)).outcome == Outcome::unsupported, "a primitive below the enum");
    require(build_composition(step(0, 9, 1.0f)).outcome == Outcome::unsupported, "a primitive above the enum");

    // The most a composition holds still fits one frame the host accepts; one
    // more is illegal.
    std::vector<CompositionStep> longest(composition_size_max, CompositionStep{1, primitive::click, 1.0f});
    built = build_composition(longest);
    require(built.outcome == Outcome::ok && built.timeline.events.size() == 128, "128 elements");
    require(host_accepts(play_of(built, 1)), "128 elements make one frame");
    longest.push_back({1, primitive::click, 1.0f});
    require(build_composition(longest).outcome == Outcome::illegal_argument, "129 elements");

    // Every primitive at several scales, alone and in a mix, ends within its
    // total and builds an accepted frame; times never go backwards.
    std::vector<CompositionStep> mix;
    for (const auto& entry : primitives) {
        for (const float scale : {0.0f, 0.3f, 1.0f}) {
            const auto alone = build_composition(step(7, entry.code, scale));
            require(alone.outcome == Outcome::ok && alone.timeline.total_ms == 7 + entry.duration_ms, "a primitive alone");
            if (!alone.timeline.events.empty()) require(host_accepts(play_of(alone, 3)), "a primitive's frame");
            mix.push_back({int32_t(entry.duration_ms % 50), entry.code, scale});
        }
    }
    built = build_composition(mix);
    require(built.outcome == Outcome::ok, "a mix of every primitive");
    uint32_t previous_end = 0;
    for (const auto& event : built.timeline.events) {
        require(event.start_ms >= previous_end, "an element starts after the one before ends");
        previous_end = event.start_ms + event.duration_ms;
        require(previous_end <= built.timeline.total_ms, "every element ends within the total");
    }
    require(host_accepts(play_of(built, 4)), "a mix is one frame");

    // A tone.
    auto tone = build_tone(2000, 0.4f);
    require(tone.outcome == Outcome::ok && tone.timeline.total_ms == 2000 && tone.timeline.events.size() == 1 &&
                tone.timeline.events[0].kind == Kind::tone && tone.timeline.events[0].duration_ms == 2000 &&
                tone.timeline.events[0].intensity == 0.4f && tone.timeline.events[0].sharpness == tone_sharpness,
            "a tone");
    require(host_accepts(play_of(tone, 1)), "a tone's frame");
    tone = build_tone(0, 1.0f);
    require(tone.outcome == Outcome::ok && tone.timeline.events.empty() && tone.timeline.total_ms == 0, "no time, no tone");
    require(build_tone(-1, 1.0f).outcome == Outcome::illegal_argument, "a negative time");
    for (const float bad : {0.0f, -1.0f, 1.1f, std::numeric_limits<float>::quiet_NaN()}) {
        require(build_tone(100, bad).outcome == Outcome::illegal_argument, "an amplitude outside (0, 1]");
    }
    tone = build_tone(std::numeric_limits<int32_t>::max(), 1.0f);
    require(tone.outcome == Outcome::ok && tone.timeline.total_ms == uint32_t(std::numeric_limits<int32_t>::max()),
            "the longest tone");
    require(port == 5557, "the haptics port");
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) fail("usage: haptics_protocol_tests <haptics-golden-frames.txt>");
    const auto fixture = read_fixture(argv[1]);
    check_fixture_frames(fixture);
    check_ack_reader(fixture);
    check_encoders_refuse();
    check_effects();
    return EXIT_SUCCESS;
}
