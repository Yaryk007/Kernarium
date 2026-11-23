// SPDX-License-Identifier: Apache-2.0
//
// What the vibrator HAL offers the framework and how each call becomes a
// timeline of haptics_protocol.hpp events. The numbers are those of
// android.hardware.vibrator (CompositePrimitive, Effect, EffectStrength), so
// the file needs no AIDL header and is tested on the host.
//
// The durations are the guest's timing contract: they are what perform() and
// getPrimitiveDuration() report and what the completion callbacks wait for. The
// shape that fills each duration belongs to the host application, which can be
// tuned in an app update without a new Android image.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "haptics_protocol.hpp"

namespace andarium::haptics {
struct Entry {
    int32_t code;
    uint32_t duration_ms;
};

// CompositePrimitive numbers.
namespace primitive {
constexpr int32_t noop = 0, click = 1, thud = 2, spin = 3, quick_rise = 4, slow_rise = 5,
                  quick_fall = 6, light_tick = 7, low_tick = 8;
}  // namespace primitive

// Effect numbers. The ringtone effects (6 through 20) are not offered: the
// framework plays its own waveform in their place.
namespace effect {
constexpr int32_t click = 0, double_click = 1, tick = 2, thud = 3, pop = 4, heavy_click = 5,
                  texture_tick = 21;
}  // namespace effect

// EffectStrength numbers: LIGHT 0, MEDIUM 1, STRONG 2.
constexpr int32_t strongest_effect_strength = 2;

inline constexpr std::array<Entry, 9> primitives{{
    {primitive::noop, 0},
    {primitive::click, 12},
    {primitive::thud, 60},
    {primitive::spin, 140},
    {primitive::quick_rise, 120},
    {primitive::slow_rise, 400},
    {primitive::quick_fall, 80},
    {primitive::light_tick, 8},
    {primitive::low_tick, 12},
}};

inline constexpr std::array<Entry, 7> effects{{
    {effect::click, 12},
    {effect::double_click, 112},
    {effect::tick, 8},
    {effect::thud, 60},
    {effect::pop, 12},
    {effect::heavy_click, 16},
    {effect::texture_tick, 8},
}};

constexpr std::optional<uint32_t> duration_of(std::span<const Entry> table, int32_t code) {
    for (const auto& entry : table) {
        if (entry.code == code) return entry.duration_ms;
    }
    return std::nullopt;
}

constexpr std::optional<uint32_t> primitive_duration_ms(int32_t code) {
    return duration_of(primitives, code);
}
constexpr std::optional<uint32_t> effect_duration_ms(int32_t code) {
    return duration_of(effects, code);
}

// A composition holds at most 128 elements, which keeps a frame under 2.6 KB,
// and a delay of at most one second before an element.
constexpr std::size_t composition_size_max = 128;
constexpr int32_t composition_delay_max_ms = 1000;
static_assert(composition_size_max == maximum_events);
// The amplitude a vibration starts at: setAmplitude is cleared by off().
constexpr float default_amplitude = 1.0f;
// A tone's sharpness. The HAL has no frequency control, so it is fixed.
constexpr float tone_sharpness = 0.5f;

// How a call ends, in the framework's terms.
enum class Outcome {
    ok,
    // EX_ILLEGAL_ARGUMENT: a value outside what the interface allows.
    illegal_argument,
    // EX_UNSUPPORTED_OPERATION: an effect, primitive or strength the HAL does not offer.
    unsupported,
};

// What a call plays: the events of its frame and when it ends, from the start
// of the frame. A call that plays nothing (a composition of delays only, a tone
// of no length) has no events.
struct Timeline {
    std::vector<Event> events;
    uint32_t total_ms = 0;
};

struct Built {
    Outcome outcome = Outcome::ok;
    Timeline timeline;
};

// IVibrator.on: a tone for `timeout_ms` at `amplitude`. A negative time is refused.
inline Built build_tone(int32_t timeout_ms, float amplitude) {
    if (timeout_ms < 0 || !(amplitude > 0 && amplitude <= 1)) return {Outcome::illegal_argument, {}};
    Built built;
    built.timeline.total_ms = uint32_t(timeout_ms);
    if (timeout_ms > 0) {
        built.timeline.events.push_back(Event{Kind::tone, 0, 0, 0, uint32_t(timeout_ms), amplitude, tone_sharpness});
    }
    return built;
}

// IVibrator.perform: one predefined effect. An effect or a strength the HAL does
// not offer is unsupported, as the VTS expects of a value outside the enum too.
inline Built build_effect(int32_t effect_code, int32_t strength) {
    const auto duration = effect_duration_ms(effect_code);
    if (!duration || strength < 0 || strength > strongest_effect_strength) {
        return {Outcome::unsupported, {}};
    }
    Built built;
    built.timeline.total_ms = *duration;
    built.timeline.events.push_back(
        Event{Kind::effect, uint8_t(effect_code), uint8_t(strength), 0, *duration, 1.0f, 0.0f});
    return built;
}

// One element of IVibrator.compose: wait `delay_ms`, then play `primitive`
// at `scale`.
struct CompositionStep {
    int32_t delay_ms;
    int32_t primitive;
    float scale;
};

// IVibrator.compose: the elements one after another. An element's start is the
// end of the one before plus its own delay. NOOP only advances the clock. More
// elements than the HAL offers, a delay beyond its maximum and a scale outside
// [0, 1] are illegal; a primitive it does not offer is unsupported.
inline Built build_composition(std::span<const CompositionStep> steps) {
    if (steps.size() > composition_size_max) return {Outcome::illegal_argument, {}};
    for (const auto& step : steps) {
        if (step.delay_ms < 0 || step.delay_ms > composition_delay_max_ms ||
            !(step.scale >= 0 && step.scale <= 1)) {
            return {Outcome::illegal_argument, {}};
        }
    }
    for (const auto& step : steps) {
        if (!primitive_duration_ms(step.primitive)) return {Outcome::unsupported, {}};
    }
    Built built;
    uint32_t cursor = 0;
    for (const auto& step : steps) {
        const uint32_t duration = *primitive_duration_ms(step.primitive);
        const uint32_t start = cursor + uint32_t(step.delay_ms);
        if (step.primitive != primitive::noop) {
            built.timeline.events.push_back(Event{Kind::primitive, uint8_t(step.primitive), 0, start,
                                                  duration, step.scale, 0.0f});
        }
        cursor = start + duration;
    }
    built.timeline.total_ms = cursor;
    return built;
}
}  // namespace andarium::haptics
