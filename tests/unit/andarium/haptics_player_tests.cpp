// SPDX-License-Identifier: GPL-2.0-or-later
//
// The vibrator HAL's state and timing (the player and its completion timer)
// against a fake link: what is sent for each call, that every completion
// callback runs once and not before the vibration ends, and that the host's
// absence changes nothing the framework sees.
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "haptics_player.hpp"
#include "require.hpp"

using namespace andarium::haptics;
using namespace std::chrono_literals;
using uml::testing::require;
using Clock = std::chrono::steady_clock;

namespace {
// A link that keeps the frames it is given and can be told the host is gone.
struct FakeLink {
    std::mutex mutex;
    std::vector<std::vector<uint8_t>> frames;
    bool connected = true;

    bool send(std::span<const uint8_t> frame) {
        std::lock_guard lock(mutex);
        if (!connected) return false;
        frames.emplace_back(frame.begin(), frame.end());
        return true;
    }
    std::vector<std::vector<uint8_t>> taken() {
        std::lock_guard lock(mutex);
        return frames;
    }
    void set_connected(bool value) {
        std::lock_guard lock(mutex);
        connected = value;
    }
};

uint16_t type_of(const std::vector<uint8_t>& frame) { return uint16_t(frame[4] | (frame[5] << 8)); }
uint32_t word(const std::vector<uint8_t>& frame, std::size_t offset) {
    return uint32_t(frame[offset]) | (uint32_t(frame[offset + 1]) << 8) | (uint32_t(frame[offset + 2]) << 16) |
           (uint32_t(frame[offset + 3]) << 24);
}
float real(const std::vector<uint8_t>& frame, std::size_t offset) { return std::bit_cast<float>(word(frame, offset)); }
// The sequence number of a frame of any type: the fifth word of its payload
// after the 8-byte header is 4 bytes in, for PLAY, INTENSITY and STOP alike.
uint32_t sequence_of(const std::vector<uint8_t>& frame) { return word(frame, 12); }

// A completion callback that counts its runs and when the first one happened.
struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    int runs = 0;
    Clock::time_point first;
    std::thread::id thread;

    Player::Done done() {
        return [this] {
            std::lock_guard lock(mutex);
            if (runs++ == 0) first = Clock::now();
            thread = std::this_thread::get_id();
            changed.notify_all();
        };
    }
    // Waits for a first run, at most `limit`.
    bool wait(std::chrono::milliseconds limit = 5000ms) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, [this] { return runs > 0; });
    }
    int count() {
        std::lock_guard lock(mutex);
        return runs;
    }
};

struct Rig {
    FakeLink link;
    Player player{[this](std::span<const uint8_t> frame) { return link.send(frame); }};
};

void check_tone_and_amplitude() {
    Rig rig;
    Completion completion;
    const auto start = Clock::now();
    require(rig.player.on(250, completion.done()) == Outcome::ok, "on");
    auto frames = rig.link.taken();
    require(frames.size() == 1 && type_of(frames[0]) == 1 && real(frames[0], 8 + 8 + 12) == 1.0f &&
                word(frames[0], 8 + 8 + 8) == 250,
            "on(250) sends one PLAY: a tone for 250 ms at the default amplitude");
    require(completion.wait(), "the callback runs");
    require(completion.first - start >= 250ms, "the callback is never early");
    std::this_thread::sleep_for(150ms);
    require(completion.count() == 1, "the callback runs once");
    require(completion.thread != std::this_thread::get_id(), "the callback runs on the timer's thread");

    // An amplitude set before a tone is the tone's level; one set during it is
    // an INTENSITY frame; one set after it only waits for the next tone.
    Rig second;
    require(second.player.set_amplitude(0.25f) == Outcome::ok, "setAmplitude");
    require(second.link.taken().empty(), "no frame while nothing plays");
    const auto tone_start = Clock::now();
    require(second.player.on(400, {}) == Outcome::ok, "on");
    frames = second.link.taken();
    require(frames.size() == 1 && real(frames[0], 8 + 8 + 12) == 0.25f, "the tone starts at the set amplitude");
    require(second.player.set_amplitude(0.75f) == Outcome::ok, "setAmplitude during a tone");
    frames = second.link.taken();
    // Unless the host stalled the test for most of the tone.
    if (Clock::now() - tone_start < 300ms) {
        require(frames.size() == 2 && type_of(frames[1]) == 2 && real(frames[1], 16) == 0.75f &&
                    sequence_of(frames[1]) == sequence_of(frames[0]) + 1,
                "an INTENSITY frame follows, with the next sequence number");
    }
    std::this_thread::sleep_for(450ms);
    const auto sent_so_far = second.link.taken().size();
    require(second.player.set_amplitude(0.5f) == Outcome::ok && second.link.taken().size() == sent_so_far,
            "no INTENSITY once the tone is over");
    require(second.player.on(100, {}) == Outcome::ok, "on");
    frames = second.link.taken();
    require(type_of(frames.back()) == 1 && real(frames.back(), 8 + 8 + 12) == 0.5f,
            "the amplitude stays for the next tone");
    // off() clears it, as the interface requires.
    second.player.off();
    require(second.player.on(100, {}) == Outcome::ok, "on");
    frames = second.link.taken();
    require(real(frames.back(), 8 + 8 + 12) == 1.0f, "off() resets the amplitude");

    for (const float bad : {0.0f, -1.0f, 1.1f, std::numeric_limits<float>::quiet_NaN()}) {
        require(second.player.set_amplitude(bad) == Outcome::illegal_argument, "an amplitude outside (0, 1]");
    }
}

void check_off_and_replacement() {
    Rig rig;
    Completion tone;
    require(rig.player.on(5000, tone.done()) == Outcome::ok, "on");
    const auto before = Clock::now();
    rig.player.off();
    auto frames = rig.link.taken();
    require(frames.size() == 2 && type_of(frames[1]) == 3 && frames[1].size() == 16, "off() sends STOP");
    require(tone.wait(1000ms), "off() completes the callback of the tone it cut");
    require(tone.first - before < 1000ms, "and at once");
    std::this_thread::sleep_for(100ms);
    require(tone.count() == 1, "once");

    // A second off() with nothing playing sends nothing.
    rig.player.off();
    require(rig.link.taken().size() == 2, "off() with nothing playing sends no STOP");

    // A vibration replaces the one before it: no STOP, the first callback
    // completes at once and the second at its own end.
    Rig second;
    Completion first;
    Completion next;
    require(second.player.on(5000, first.done()) == Outcome::ok, "on");
    int32_t length = 0;
    require(second.player.perform(effect::click, 1, next.done(), &length) == Outcome::ok && length == 12, "perform");
    require(first.wait(1000ms), "the replaced vibration completes at once");
    require(next.wait(), "the new one completes");
    frames = second.link.taken();
    require(frames.size() == 2 && type_of(frames[0]) == 1 && type_of(frames[1]) == 1, "a PLAY replaces a PLAY");
    require(first.count() == 1 && next.count() == 1, "each callback ran once");

    // A STOP follows a vibration that ended only just now (the host may be
    // playing its last element), and not one that ended long ago.
    Rig third;
    const auto ended = Clock::now();
    require(third.player.perform(effect::tick, 0, {}, &length) == Outcome::ok && length == 8, "perform");
    third.player.off();
    if (Clock::now() - ended < Player::stop_grace - 50ms) {
        require(third.link.taken().size() == 2, "a STOP for a vibration that just ended");
    }
    const auto frames_before = third.link.taken().size();
    require(third.player.perform(effect::tick, 0, {}, &length) == Outcome::ok, "perform");
    std::this_thread::sleep_for(Player::stop_grace + 150ms);
    const auto before_second_off = third.link.taken().size();
    third.player.off();
    require(third.link.taken().size() == before_second_off, "no STOP for one that ended long ago");
    require(before_second_off == frames_before + 1, "the second perform sent one PLAY");
}

void check_composition_completion() {
    Rig rig;
    Completion completion;
    const std::vector<CompositionStep> steps{{0, primitive::click, 0.5f}, {50, primitive::thud, 1.0f}};
    const auto start = Clock::now();
    require(rig.player.compose(steps, completion.done()) == Outcome::ok, "compose");
    require(completion.wait(), "the composition completes");
    require(completion.first - start >= 122ms, "after delays and primitives: 12 + 50 + 60 ms");
    require(rig.link.taken().size() == 1, "one PLAY for the composition");

    // A composition of delays alone plays nothing and completes after them.
    Rig quiet;
    Completion waited;
    const std::vector<CompositionStep> delays{{80, primitive::noop, 0.0f}};
    const auto quiet_start = Clock::now();
    require(quiet.player.compose(delays, waited.done()) == Outcome::ok, "compose of NOOP");
    require(waited.wait() && waited.first - quiet_start >= 80ms, "completes after the delay");
    require(quiet.link.taken().empty(), "nothing is sent for it");

    // A rejected call arms no callback and sends nothing.
    Rig bad;
    Completion never;
    const std::vector<CompositionStep> unsupported{{0, 99, 1.0f}};
    int32_t length = 0;
    require(bad.player.compose(unsupported, never.done()) == Outcome::unsupported, "an unsupported primitive");
    require(bad.player.perform(effect::click, 3, never.done(), &length) == Outcome::unsupported, "an unsupported strength");
    require(bad.player.perform(7, 1, never.done(), &length) == Outcome::unsupported, "a ringtone");
    require(bad.player.on(-1, never.done()) == Outcome::illegal_argument, "a negative time");
    std::this_thread::sleep_for(100ms);
    require(never.count() == 0 && bad.link.taken().empty(), "a refused call does nothing");
}

void check_without_host() {
    // With no host the frames are dropped and the callbacks run on time.
    Rig rig;
    rig.link.set_connected(false);
    Completion completion;
    int32_t length = 0;
    const auto start = Clock::now();
    require(rig.player.perform(effect::double_click, 2, completion.done(), &length) == Outcome::ok && length == 112,
            "perform without a host");
    require(completion.wait() && completion.first - start >= 112ms, "the callback still runs on time");
    require(rig.player.set_amplitude(0.5f) == Outcome::ok, "setAmplitude without a host");
    rig.player.off();
    auto stats = rig.player.stats();
    require(stats.plays == 1 && stats.sent == 0 && stats.dropped == 1 && stats.stops == 0 && stats.intensities == 0,
            "a dropped PLAY is counted, and nothing else is offered for it");
    require(rig.link.taken().empty(), "nothing reached the host");

    // A host that connects later gets the frames of later calls.
    rig.link.set_connected(true);
    require(rig.player.perform(effect::click, 1, {}, &length) == Outcome::ok, "perform with a host");
    stats = rig.player.stats();
    require(stats.sent == 1 && rig.link.taken().size() == 1, "and it is sent");
}

void check_acknowledgements() {
    uint64_t microseconds = 1000;
    FakeLink link;
    Player player([&link](std::span<const uint8_t> frame) { return link.send(frame); },
                  [&microseconds] { return microseconds; });
    int32_t length = 0;
    // Three PLAYs, answered 100, 300 and 200 microseconds after they were sent.
    const uint64_t delays[] = {100, 300, 200};
    for (int i = 0; i < 3; ++i) {
        microseconds = 1000 * uint64_t(i + 1);
        require(player.perform(effect::click, 1, {}, &length) == Outcome::ok, "perform");
    }
    const auto frames = link.taken();
    require(frames.size() == 3, "three PLAYs");
    for (int i = 0; i < 3; ++i) {
        microseconds = 1000 * uint64_t(i + 1) + delays[i];
        player.on_ack(Ack{sequence_of(frames[i]), 40, uint8_t(AckResult::played)});
    }
    // An ACK for a frame the player keeps no timing for, one it already
    // answered, and results of every kind.
    player.on_ack(Ack{9999, 5, uint8_t(AckResult::played)});
    player.on_ack(Ack{sequence_of(frames[0]), 5, uint8_t(AckResult::played)});
    player.on_ack(Ack{9998, 0, uint8_t(AckResult::not_in_front)});
    player.on_ack(Ack{9997, 70, uint8_t(AckResult::engine_failed)});
    player.on_ack(Ack{9996, 0, 7});
    const auto stats = player.stats();
    require(stats.acks == 8 && stats.ack_played == 5 && stats.ack_not_in_front == 1 && stats.ack_engine_failed == 1 &&
                stats.ack_other == 1 && stats.ack_unmatched == 5,
            "ACKs are counted by result, matched or not");
    require(stats.round_trips == 3 && stats.round_trip_p50_us == 200 && stats.round_trip_max_us == 300 &&
                stats.host_max_us == 70,
            "the round trip's median and maximum");
    const auto text = player.dump();
    require(text.find("p50=200 us max=300 us") != std::string::npos && text.find("amplitude=1.00") != std::string::npos,
            "the dump reports them");

    // More ACKs than the player keeps flights for: the old ones are forgotten.
    FakeLink busy;
    Player flood([&busy](std::span<const uint8_t> frame) { return busy.send(frame); });
    for (int i = 0; i < 100; ++i) require(flood.perform(effect::tick, 0, {}, &length) == Outcome::ok, "perform");
    const auto sent = busy.taken();
    flood.on_ack(Ack{sequence_of(sent.front()), 1, 0});
    flood.on_ack(Ack{sequence_of(sent.back()), 1, 0});
    require(flood.stats().ack_unmatched == 1 && flood.stats().round_trips == 1, "an ACK for a forgotten frame");
}

void check_timer() {
    // A callback does not run before its deadline.
    {
        CompletionTimer timer;
        std::atomic<int64_t> ran_at{-1};
        const auto start = Clock::now();
        timer.schedule([&] { ran_at = (Clock::now() - start) / 1ms; }, start + 120ms);
        const auto limit = Clock::now() + 5s;
        while (ran_at < 0 && Clock::now() < limit) std::this_thread::sleep_for(10ms);
        require(ran_at >= 120, "a callback runs, and not before its deadline");
    }
    // A new callback replaces the one armed before it, which is due at once;
    // the armed one waits for its time, or for complete_scheduled, and runs once.
    CompletionTimer timer;
    std::mutex mutex;
    std::vector<int> order;
    const auto record = [&](int id) {
        return [&, id] {
            std::lock_guard lock(mutex);
            order.push_back(id);
        };
    };
    const auto waited = [&](std::size_t count) {
        const auto limit = Clock::now() + 5s;
        while (Clock::now() < limit) {
            {
                std::lock_guard lock(mutex);
                if (order.size() >= count) return;
            }
            std::this_thread::sleep_for(10ms);
        }
    };
    timer.schedule(record(1), Clock::now() + 3600s);
    timer.schedule(record(2), Clock::now() + 3600s);
    waited(1);
    {
        std::lock_guard lock(mutex);
        require(order == std::vector<int>{1}, "the replaced callback ran, and the armed one waits for its time");
    }
    timer.complete_scheduled();
    timer.complete_scheduled();
    waited(2);
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard lock(mutex);
        require(order == std::vector<int>{1, 2}, "complete_scheduled runs the armed callback once");
    }
    // A timer destroyed with a callback armed far ahead ends at once, and does
    // not run it.
    const auto destroyed = Clock::now();
    {
        CompletionTimer pending;
        pending.schedule(record(3), Clock::now() + 3600s);
    }
    require(Clock::now() - destroyed < 2s, "destroying a timer does not wait for its deadline");
    std::lock_guard lock(mutex);
    require(order.size() == 2, "and does not run it");
}
}  // namespace

int main()
{
    check_timer();
    check_tone_and_amplitude();
    check_off_and_replacement();
    check_composition_completion();
    check_without_host();
    check_acknowledgements();
    return EXIT_SUCCESS;
}
