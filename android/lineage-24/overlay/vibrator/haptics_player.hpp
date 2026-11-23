// SPDX-License-Identifier: Apache-2.0
//
// The state of the guest's vibrator and the frames it sends to the host. The
// binder side (Vibrator.cpp) converts the AIDL types and the link side
// (HostLink.cpp) moves bytes, so what the framework sees is decided here and
// is tested on the host with a fake link.
//
// Every call builds its frame, offers it to the link without waiting, and arms
// the completion callback for the moment the vibration ends. The framework's
// view of the vibrator never depends on the host: with no host connected a
// frame is dropped and the callbacks still complete on time. An ACK is for
// diagnostics only; nothing waits for one, and one for a frame the player keeps
// no timing for is counted and ignored.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "completion_timer.hpp"
#include "haptics_effects.hpp"
#include "haptics_protocol.hpp"

namespace andarium::haptics {
// What the player counts, for the HAL's dump.
struct Stats {
    uint64_t plays = 0;
    uint64_t intensities = 0;
    uint64_t stops = 0;
    // Frames the link took whole, frames it could not (no host, or a send that
    // did not complete) and frames the encoder refused (a defect).
    uint64_t sent = 0;
    uint64_t dropped = 0;
    uint64_t refused = 0;
    uint64_t acks = 0;
    uint64_t ack_played = 0;
    uint64_t ack_not_in_front = 0;
    uint64_t ack_engine_failed = 0;
    uint64_t ack_other = 0;
    uint64_t ack_unmatched = 0;
    // From the last 256 ACKs the player kept the timing of.
    uint64_t round_trips = 0;
    uint32_t round_trip_p50_us = 0;
    uint32_t round_trip_max_us = 0;
    uint32_t host_max_us = 0;
};

class Player {
public:
    using Clock = std::chrono::steady_clock;
    // Offers one frame to the host. False when it was not sent whole.
    using Sender = std::function<bool(std::span<const uint8_t>)>;
    using Done = std::function<void()>;
    using Microseconds = std::function<uint64_t()>;

    // A STOP is sent for a vibration that ended this recently as well, because
    // the host may still be playing its last element.
    static constexpr auto stop_grace = std::chrono::milliseconds(200);

    explicit Player(Sender sender, Microseconds now_us = steady_microseconds)
        : sender_(std::move(sender)), now_us_(std::move(now_us)) {}

    // IVibrator.on. `done` runs when the tone has lasted `timeout_ms`.
    Outcome on(int32_t timeout_ms, Done done) {
        std::lock_guard lock(mutex_);
        auto built = build_tone(timeout_ms, amplitude_);
        if (built.outcome != Outcome::ok) return built.outcome;
        begin(built.timeline, std::move(done), true);
        return Outcome::ok;
    }

    // IVibrator.perform. `length_ms` is what the call reports to the framework.
    Outcome perform(int32_t effect, int32_t strength, Done done, int32_t* length_ms) {
        std::lock_guard lock(mutex_);
        auto built = build_effect(effect, strength);
        if (built.outcome != Outcome::ok) return built.outcome;
        *length_ms = int32_t(built.timeline.total_ms);
        begin(built.timeline, std::move(done), false);
        return Outcome::ok;
    }

    // IVibrator.compose.
    Outcome compose(std::span<const CompositionStep> steps, Done done) {
        std::lock_guard lock(mutex_);
        auto built = build_composition(steps);
        if (built.outcome != Outcome::ok) return built.outcome;
        begin(built.timeline, std::move(done), false);
        return Outcome::ok;
    }

    // IVibrator.setAmplitude: the level of a tone, now and until off(). It
    // changes a tone that is playing and is kept for the next one.
    Outcome set_amplitude(float amplitude) {
        if (!(amplitude > 0 && amplitude <= 1)) return Outcome::illegal_argument;
        std::lock_guard lock(mutex_);
        amplitude_ = amplitude;
        if (delivered_ && Clock::now() < tone_end_) {
            const auto sequence = next_sequence();
            offer(encode_intensity(device_target, sequence, amplitude), sequence, stats_.intensities);
        }
        return Outcome::ok;
    }

    // IVibrator.off: stops what plays, clears the amplitude, and completes the
    // callback of the vibration that was cut.
    void off() {
        std::lock_guard lock(mutex_);
        amplitude_ = default_amplitude;
        stop_if_playing(Clock::now());
        timer_.complete_scheduled();
    }

    // The host's ACK of a frame, from the link's reading thread.
    void on_ack(const Ack& ack) {
        std::lock_guard lock(mutex_);
        ++stats_.acks;
        switch (AckResult(ack.result)) {
            case AckResult::played: ++stats_.ack_played; break;
            case AckResult::not_in_front: ++stats_.ack_not_in_front; break;
            case AckResult::engine_failed: ++stats_.ack_engine_failed; break;
            default: ++stats_.ack_other; break;
        }
        stats_.host_max_us = std::max(stats_.host_max_us, ack.microseconds);
        auto& flight = flights_[ack.sequence % flights_.size()];
        if (!flight.valid || flight.sequence != ack.sequence) {
            ++stats_.ack_unmatched;
            return;
        }
        flight.valid = false;
        const uint64_t elapsed = now_us_() - flight.sent_us;
        const auto round_trip = uint32_t(std::min<uint64_t>(elapsed, UINT32_MAX));
        window_[stats_.round_trips % window_.size()] = round_trip;
        ++stats_.round_trips;
        stats_.round_trip_max_us = std::max(stats_.round_trip_max_us, round_trip);
    }

    Stats stats() const {
        std::lock_guard lock(mutex_);
        Stats snapshot = stats_;
        const auto count = std::size_t(std::min<uint64_t>(stats_.round_trips, window_.size()));
        if (count > 0) {
            std::vector<uint32_t> sorted(window_.begin(), window_.begin() + std::ptrdiff_t(count));
            const auto middle = sorted.begin() + std::ptrdiff_t(count / 2);
            std::nth_element(sorted.begin(), middle, sorted.end());
            snapshot.round_trip_p50_us = *middle;
        }
        return snapshot;
    }

    // The text of the service's dump.
    std::string dump() const {
        const auto s = stats();
        float amplitude;
        {
            std::lock_guard lock(mutex_);
            amplitude = amplitude_;
        }
        char text[640];
        std::snprintf(
            text, sizeof(text),
            "Andarium vibrator: amplitude=%.2f\n"
            "  frames: play=%llu intensity=%llu stop=%llu sent=%llu dropped=%llu refused=%llu\n"
            "  acks: %llu (played %llu, not in front %llu, engine failed %llu, other %llu, "
            "unmatched %llu)\n"
            "  round trip: p50=%u us max=%u us over %llu ACKs; host start max=%u us\n",
            double(amplitude), (unsigned long long)s.plays, (unsigned long long)s.intensities,
            (unsigned long long)s.stops, (unsigned long long)s.sent, (unsigned long long)s.dropped,
            (unsigned long long)s.refused, (unsigned long long)s.acks,
            (unsigned long long)s.ack_played, (unsigned long long)s.ack_not_in_front,
            (unsigned long long)s.ack_engine_failed, (unsigned long long)s.ack_other,
            (unsigned long long)s.ack_unmatched, s.round_trip_p50_us, s.round_trip_max_us,
            (unsigned long long)s.round_trips, s.host_max_us);
        return text;
    }

private:
    struct Flight {
        uint32_t sequence = 0;
        uint64_t sent_us = 0;
        bool valid = false;
    };

    static uint64_t steady_microseconds() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                            Clock::now().time_since_epoch())
                            .count());
    }

    uint32_t next_sequence() { return ++sequence_; }

    // Offers a frame to the link and keeps the timing of its ACK. Called with
    // mutex_ held; the link never waits. Returns whether the frame was sent.
    bool offer(const std::optional<std::vector<uint8_t>>& frame, uint32_t sequence, uint64_t& counter) {
        ++counter;
        if (!frame) {
            ++stats_.refused;
            return false;
        }
        auto& flight = flights_[sequence % flights_.size()];
        flight = {sequence, now_us_(), true};
        if (sender_(*frame)) {
            ++stats_.sent;
            return true;
        }
        ++stats_.dropped;
        flight.valid = false;
        return false;
    }

    // Starts a vibration, replacing the one before it: the host stops what it
    // played when a PLAY arrives, and the callback of the vibration that was
    // replaced completes at once. Called with mutex_ held.
    void begin(const Timeline& timeline, Done done, bool tone) {
        const auto now = Clock::now();
        const auto end = now + std::chrono::milliseconds(timeline.total_ms);
        timer_.complete_scheduled();
        if (timeline.events.empty()) {
            stop_if_playing(now);
        } else {
            const auto sequence = next_sequence();
            delivered_ = offer(encode_play(device_target, sequence, timeline.events), sequence, stats_.plays);
            play_end_ = end;
            tone_end_ = tone ? end : Clock::time_point{};
        }
        if (done) timer_.schedule(std::move(done), end);
    }

    // Sends a STOP when the host may be playing something, and forgets it.
    void stop_if_playing(Clock::time_point now) {
        if (delivered_ && now < play_end_ + stop_grace) {
            const auto sequence = next_sequence();
            offer(encode_stop(device_target, sequence), sequence, stats_.stops);
        }
        delivered_ = false;
        play_end_ = {};
        tone_end_ = {};
    }

    Sender sender_;
    Microseconds now_us_;
    mutable std::mutex mutex_;
    float amplitude_ = default_amplitude;
    uint32_t sequence_ = 0;
    // Whether the host took the frame of the vibration in progress, and when
    // it ends. `tone_end_` is set only while that vibration is a tone.
    bool delivered_ = false;
    Clock::time_point play_end_{};
    Clock::time_point tone_end_{};
    std::array<Flight, 64> flights_{};
    std::array<uint32_t, 256> window_{};
    Stats stats_;
    // Last, so its thread is joined before the state its callbacks outlive.
    CompletionTimer timer_;
};
}  // namespace andarium::haptics
