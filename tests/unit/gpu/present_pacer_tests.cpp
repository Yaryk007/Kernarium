// SPDX-License-Identifier: GPL-2.0-or-later

#include "present_pacer.hpp"
#include "require.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using uml::gpu::LinkFrameRate;
using uml::gpu::PresentPacer;
using uml::testing::require;

constexpr LinkFrameRate high {80.0F, 120.0F, 120.0F};
constexpr LinkFrameRate normal {60.0F, 60.0F, 60.0F};

/* The link as the display runs it, on a clock the test moves a millisecond at
 * a time: a guest that offers frames at a rate that can change, a link that
 * sleeps until a frame arrives, wakes at the pacer's fastest rate and ticks
 * at the rate the pacer last asked for (a tick is when iOS gives the link a
 * turn, so the first tick after a wake is one interval later). */
class Link {
public:
    explicit Link(double ceiling = 120.0, PresentPacer::Parameters parameters = {})
        : pacer_(parameters)
        , ceiling_(ceiling)
    {
    }

    void setGuestRate(double framesPerSecond) { guestRate_ = framesPerSecond; }
    void setCeiling(double ceiling) { ceiling_ = ceiling; }
    /* The peak-to-peak jitter of a guest frame's time (1 ms by default; none
     * makes a guest as even as a fake clock can). */
    void setJitter(double seconds) { jitter_ = seconds; }
    /* The next `count` draws find no drawable (a transient failure of the
     * view's drawable or render pass), and show nothing. */
    void failNextDraws(unsigned count) { failures_ = count; }
    /* Each draw that finds no drawable has waited this long for it, on the
     * thread that draws (a layer waits up to a second for a drawable that the
     * display holds). */
    void setDrawableWait(double seconds) { drawableWait_ = seconds; }
    /* What each request for a drawable waits, in the order the draws make
     * them (the last repeats), whether the request finds one or not: the
     * waits of one frame that no single wait stands for. */
    void setRequestWaits(std::vector<double> seconds)
    {
        requestWaits_ = std::move(seconds);
        requestsMade_ = 0;
    }
    /* A frame arrives on the next tick that presents, after the tick took its
     * flag and before the draw claims the newest frame: the draw is of that
     * frame, and the flag it left set is the next tick's. */
    void arriveBeforeNextClaim() { arriveBeforeClaim_ = true; }
    /* The link is made again, as when the scene it was on disconnects and
     * another activates before the next tick: the pacer wakes at once, with
     * whatever the display owes the newest frame. */
    void rebind()
    {
        asleep_ = false;
        ++wakes_;
        request(pacer_.wake(now_, frames_, ceiling_));
        nextTick_ = now_ + 1.0 / tickRate();
    }
    /* One frame, now, whatever the guest rate. */
    void offer()
    {
        ++frames_;
        arrived();
        wake();
    }

    /* Runs `seconds` more. */
    void run(double seconds)
    {
        const double end = now_ + seconds;
        while (now_ < end) {
            now_ += step;
            guestFrame();
            if (!asleep_ && now_ + 1e-9 >= nextTick_)
                tick();
        }
    }

    [[nodiscard]] double now() const { return now_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }
    /* Draws that showed a frame, and draws made (a failed one included). */
    [[nodiscard]] std::uint64_t presents() const { return presents_; }
    [[nodiscard]] std::uint64_t draws() const { return draws_; }
    /* The requests the draws made of the layer for a drawable. */
    [[nodiscard]] std::uint64_t requestsMade() const { return requestsMade_; }
    /* The time the draws spent waiting for a drawable, all on the thread that
     * draws. */
    [[nodiscard]] double mainThreadSeconds() const { return mainThread_; }
    /* A frame has arrived that no draw has shown. */
    [[nodiscard]] bool frameWaiting() const { return unshown_; }
    [[nodiscard]] std::uint64_t ticks() const { return ticks_; }
    [[nodiscard]] std::uint64_t wakes() const { return wakes_; }
    [[nodiscard]] bool asleep() const { return asleep_; }
    [[nodiscard]] unsigned rate() const { return pacer_.rate(); }
    [[nodiscard]] const std::vector<LinkFrameRate>& requests() const
    {
        return requests_;
    }
    /* When the link last asked for a range other than the one before. */
    [[nodiscard]] double lastRequestAt() const { return lastRequestAt_; }
    [[nodiscard]] double longestWait() const { return longestWait_; }

private:
    static constexpr double step = 0.001;

    void guestFrame()
    {
        if (guestRate_ <= 0.0)
            return;
        /* Frame times with a deterministic jitter of up to a millisecond. */
        if (nextFrame_ <= 0.0)
            nextFrame_ = now_;
        while (nextFrame_ <= now_) {
            ++frames_;
            arrived();
            wake();
            seed_ = seed_ * 1103515245U + 12345U;
            const double jitter =
                (static_cast<double>((seed_ >> 16U) % 1000U) / 1000.0 - 0.5) *
                jitter_;
            nextFrame_ += 1.0 / guestRate_ + jitter;
        }
    }

    /* The producer's side: the flag the tick takes, and the frame that stays
     * unshown until a draw shows it. */
    void arrived()
    {
        if (!unshown_)
            waitingSince_ = now_;
        unshown_ = true;
        waiting_ = true;
    }

    void wake()
    {
        if (!asleep_)
            return;
        asleep_ = false;
        ++wakes_;
        const auto range = pacer_.wake(now_, frames_, ceiling_);
        request(range);
        nextTick_ = now_ + 1.0 / tickRate();
    }

    void request(LinkFrameRate range)
    {
        if (requests_.empty() || !(requests_.back() == range)) {
            requests_.push_back(range);
            lastRequestAt_ = now_;
        }
        preferred_ = range.preferred;
    }

    [[nodiscard]] double tickRate() const { return preferred_; }

    /* What the next request waits: the plan, or the wait of a request that
     * finds no drawable. */
    double requestWait(bool fails)
    {
        ++requestsMade_;
        if (!requestWaits_.empty()) {
            return requestWaits_[std::min<std::size_t>(requestsMade_ - 1, requestWaits_.size() - 1)];
        }
        return fails ? drawableWait_ : 0.0;
    }

    void tick()
    {
        ++ticks_;
        /* The tick takes the flag before it decides, as the display does. */
        const bool waiting = std::exchange(waiting_, false);
        const auto decision = pacer_.tick(now_, frames_, waiting, ceiling_);
        if (decision.range)
            request(*decision.range);
        if (decision.present) {
            ++draws_;
            if (arriveBeforeClaim_) {
                arriveBeforeClaim_ = false;
                ++frames_;
                arrived();
            }
            /* The draw claims the newest frame, and asks the layer for a
             * drawable once, only while that frame has budget left. */
            const std::uint64_t claimed = frames_;
            bool unavailable = true;
            double waited = 0.0;
            if (pacer_.drawableBudget().allowanceFor(claimed) > 0.0) {
                const bool fails = failures_ > 0;
                waited = requestWait(fails);
                mainThread_ += waited;
                if (fails)
                    --failures_;
                unavailable = fails;
            }
            pacer_.drew(unavailable, unavailable ? waited : 0.0, claimed);
            if (!unavailable) {
                ++presents_;
                longestWait_ = std::max(longestWait_, now_ - waitingSince_);
                unshown_ = false;
            }
        }
        if (decision.sleep) {
            asleep_ = true;
            return;
        }
        nextTick_ += 1.0 / tickRate();
        if (nextTick_ <= now_)
            nextTick_ = now_ + 1.0 / tickRate();
    }

    PresentPacer pacer_;
    double ceiling_;
    double now_ {};
    double guestRate_ {};
    double nextFrame_ {};
    double nextTick_ {};
    double preferred_ {120.0};
    double lastRequestAt_ {};
    double waitingSince_ {};
    double longestWait_ {};
    double jitter_ {0.001};
    std::uint32_t seed_ {1};
    std::uint64_t frames_ {};
    std::uint64_t presents_ {};
    std::uint64_t draws_ {};
    unsigned failures_ {};
    double drawableWait_ {};
    std::vector<double> requestWaits_;
    std::uint64_t requestsMade_ {};
    bool arriveBeforeClaim_ {};
    double mainThread_ {};
    std::uint64_t ticks_ {};
    std::uint64_t wakes_ {};
    bool waiting_ {};
    bool unshown_ {};
    bool asleep_ {true};
    std::vector<LinkFrameRate> requests_;
};

void aLinkThatWakesAsksForTheFastestRateTheCeilingAllows()
{
    PresentPacer pacer;
    require(pacer.wake(1.0, 0, 120.0) == high, "a 120 Hz ceiling");
    require(pacer.rate() == 120);
    require(pacer.wake(2.0, 0, 90.0) == normal, "a 90 Hz ceiling is a 60 step");
    /* The 120 step needs a screen that shows 100 or more: the display is
     * granted the 120 Hz mode only on one (uml::gpu::highStepMinimumCeiling),
     * so a ceiling under it is never the mode's. */
    require(uml::gpu::highStepMinimumCeiling == 100.0);
    require(pacer.wake(2.5, 0, 99.0) == normal, "a 99 Hz ceiling is a 60 step");
    require(pacer.wake(2.6, 0, 100.0) == high, "a 100 Hz ceiling is the 120 step");
    require(pacer.wake(3.0, 0, 60.0) == normal, "a 60 Hz ceiling");
    /* A display that reports neither 60 nor 120 gets the step it fits: the
     * 120 step from 100 up, the 60 step for any lower rate, a mode of 47 Hz
     * (a size too large for a 60 Hz timing), 30 Hz or none included. */
    for (const double ceiling : {47.0, 31.0, 30.0, 1.0, 0.0})
        require(pacer.wake(4.0, 0, ceiling) == normal, "under 100 is the 60 step");
    for (const double ceiling : {100.0, 119.0, 120.0, 144.0})
        require(pacer.wake(5.0, 0, ceiling) == high, "100 and over is the 120 step");
}

void aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink()
{
    Link link;
    link.setGuestRate(60.0);
    link.run(3.0);
    require(link.rate() == 60, "the link settled at 60");
    require(link.requests().size() == 2 && link.requests()[0] == high &&
                link.requests()[1] == normal,
        "one wake at 120 and one step down");
    require(link.lastRequestAt() < 1.3, "settled on the first second's evidence");
    /* Nearly every frame is presented (two that arrive between ticks are
     * one present), and none later than one tick of the settled rate after
     * it arrived (the first waits for the 120 Hz link). */
    require(link.presents() * 10U >= link.frames() * 9U, "almost every frame");
    require(link.longestWait() < 0.0185, "no frame waits more than a tick");
    /* After settling the link ticks at 60 a second, not 120. */
    const auto ticksBefore = link.ticks();
    link.run(2.0);
    const auto ticks = link.ticks() - ticksBefore;
    require(ticks >= 118 && ticks <= 122, "60 ticks a second at the 60 step");
}

void aOneHundredTwentyHertzGuestKeepsTheFastLink()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(4.0);
    require(link.rate() == 120);
    require(link.requests().size() == 1 && link.requests()[0] == high,
        "no change of range at all");
    require(link.presents() * 10U >= link.frames() * 9U, "almost every frame");
    require(link.longestWait() < 0.0105, "a frame waits at most a tick");
}

void aGuestThatRendersFasterMakesTheLinkRiseAtOnce()
{
    Link link;
    link.setGuestRate(60.0);
    /* Settled at 60 after the first second, and 2 s after that change. */
    link.run(4.0);
    require(link.rate() == 60);
    const double at = link.now();
    link.setGuestRate(120.0);
    link.run(0.5);
    require(link.rate() == 120, "the link rose");
    require(link.lastRequestAt() - at < 0.25, "a doubled rate lifts the window at once");
    require(link.requests().back() == high);
}

void aGuestThatSlowsDownMakesTheLinkFallAfterASustainedStretch()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(1.0);
    require(link.rate() == 120);
    const double at = link.now();
    link.setGuestRate(60.0);
    link.run(1.5);
    require(link.rate() == 120, "not before a sustained stretch below");
    link.run(1.0);
    require(link.rate() == 60, "fell to 60");
    require(link.requests().back() == normal);
    require(link.lastRequestAt() - at < 2.2, "within the fall window");
    /* A slower guest stays on the 60 step, which is the lowest. */
    link.setGuestRate(24.0);
    link.run(6.0);
    require(link.rate() == 60);
    require(link.requests().back() == normal);
}

/* Where a link starts: asleep, so that it wakes at the fastest step ("high"),
 * or settled on the 60 step by a guest of 45 frames a second ("mid"). */
enum class Start { high, mid };
constexpr Start allStarts[] = {Start::high, Start::mid};

void putOnStep(Link& link, Start start)
{
    if (start == Start::high)
        return;
    link.setGuestRate(45.0);
    link.run(4.0);
    require(link.rate() == 60, "the middle phase is on the 60 step");
}

/* The rule the steps follow. With the setting on, a guest that holds a rate
 * is shown at no fewer frames a second than with the setting off, which
 * shows every frame up to the 60 of its mode: a guest of 60 or less is on the
 * 60 step, which shows 60, and one at 63 or more has the 120 step. What a
 * step shows is checked for a guest well under it. */
void aSteadyGuestIsNeverShownFewerFramesThanTheSettingOffPathBeyondTwo()
{
    for (const Start start : allStarts) {
        for (const double guest : {20.0, 30.0, 31.0, 33.0, 35.0, 45.0, 55.0,
                 59.0, 60.0, 63.0, 65.0, 70.0, 80.0, 100.0, 119.0, 120.0}) {
            Link link;
            link.setJitter(0.0);
            putOnStep(link, start);
            link.setGuestRate(guest);
            link.run(6.0);
            if (guest <= 60.0)
                require(link.rate() == 60, "a guest of 60 or less is on the 60 step");
            if (guest >= 63.0)
                require(link.rate() == 120, "63 or more has the 120 step");
            /* A guest well under its step is shown whole. One near it can have
             * two frames in a tick and none in the next, whatever the step is,
             * so only its step is checked. */
            if (guest > 0.8 * static_cast<double>(link.rate()))
                continue;
            const auto frames = link.frames();
            const auto presents = link.presents();
            link.run(2.0);
            const double shown =
                static_cast<double>(link.presents() - presents) / 2.0;
            const double offered =
                static_cast<double>(link.frames() - frames) / 2.0;
            if (shown < offered - 2.0)
                std::fprintf(stderr, "guest %.0f from start %d is shown %.1f of %.1f\n",
                    guest, static_cast<int>(start), shown, offered);
            require(shown >= offered - 2.0, "an even guest is shown whole, less 2");
        }
    }
}

/* A guest that speeds up to 63 or more, from any step the link has settled
 * on, has the 120 step within 2 s and one tick (1/60 s on the 60 step) of the
 * speed-up. */
void aGuestThatSpeedsUpToSixtyThreeOrMoreReachesTheHighStepWithinTwoSecondsAndATick()
{
    for (const double before : {24.0, 45.0, 58.0}) {
        for (const double guest : {63.0, 64.0, 65.0, 70.0, 90.0, 119.0}) {
            Link link;
            link.setGuestRate(before);
            link.run(4.0);
            link.setGuestRate(guest);
            link.run(2.0 + 1.0 / 60.0);
            if (link.rate() != 120)
                std::fprintf(stderr, "guest %.0f after %.0f is on the %u step\n",
                    guest, before, link.rate());
            require(link.rate() == 120, "the 120 step within 2 s and a tick");
        }
    }
}

/* A steady speed-up changes the rate once: the rise measure supports the
 * step it has risen to, so the fall measure (which lags it) is not asked. */
void aSteadySpeedUpChangesTheRateOnce()
{
    struct Change {
        double before;
        double after;
        unsigned step;
    };
    for (const Change change : {Change {50.0, 70.0, 120}, Change {25.0, 63.0, 120},
             Change {30.0, 70.0, 120}, Change {55.0, 65.0, 120},
             Change {58.0, 63.0, 120}, Change {45.0, 64.0, 120},
             Change {60.0, 120.0, 120}, Change {20.0, 40.0, 60}}) {
        Link link;
        link.setGuestRate(change.before);
        link.run(6.0);
        const auto requests = link.requests().size();
        link.setGuestRate(change.after);
        link.run(8.0);
        const auto changes = link.requests().size() - requests;
        if (changes != (change.step == 120 ? 1U : 0U))
            std::fprintf(stderr, "%.0f -> %.0f made %zu changes\n", change.before,
                change.after, changes);
        require(changes == (change.step == 120 ? 1U : 0U),
            "one change for a speed-up over the threshold, none under it");
        require(link.rate() == change.step);
    }
}

/* A guest that alternates across the threshold changes the rate a bounded
 * number of times: at most once in each half of a cycle, and one more (the
 * step it settles on), whatever the pair, so that a fall measure and a rise
 * measure that disagree cannot take the link up and down on every tick. The
 * reviewers counted 169 changes in 12 s for 57 and 64 before the fall waited
 * for the rise measure. */
void aGuestThatAlternatesAcrossTheThresholdChangesTheRateAtMostOncePerHalfCycle()
{
    struct Pair {
        double slow;
        double fast;
    };
    for (const Pair pair : {Pair {57.0, 64.0}, Pair {58.0, 66.0}, Pair {50.0, 70.0},
             Pair {40.0, 70.0}, Pair {20.0, 40.0}, Pair {60.0, 80.0},
             Pair {62.0, 82.0}, Pair {60.0, 120.0}, Pair {30.0, 80.0}}) {
        for (const double half : {0.25, 0.5, 1.0, 1.5, 2.0}) {
            for (const Start start : allStarts) {
                Link link;
                putOnStep(link, start);
                const auto alternate = [&](double seconds) {
                    for (double done = 0.0; done < seconds; done += 2.0 * half) {
                        link.setGuestRate(pair.slow);
                        link.run(half);
                        link.setGuestRate(pair.fast);
                        link.run(half);
                    }
                };
                alternate(6.0);
                const auto requests = link.requests().size();
                alternate(12.0);
                const auto changes = link.requests().size() - requests;
                const auto bound = static_cast<std::size_t>(12.0 / half) + 1U;
                if (changes > bound)
                    std::fprintf(stderr,
                        "%.0f/%.0f half %.2f s from start %d: %zu changes in 12 s, bound %zu\n",
                        pair.slow, pair.fast, half, static_cast<int>(start), changes, bound);
                require(changes <= bound, "more than one change per half cycle");
            }
        }
    }
}

/* A guest that alternates across the threshold in halves of a second or
 * less, with its mean over it, stays on the 120 step and makes no change
 * once the link has settled. */
void aGuestThatAlternatesAboveTheThresholdOnAverageStaysOnTheHighStep()
{
    for (const auto [slow, fast] : {std::pair {60.0, 80.0}, std::pair {62.0, 82.0},
             std::pair {60.0, 120.0}, std::pair {70.0, 74.0}}) {
        for (const double half : {0.25, 0.5, 1.0}) {
            for (const Start start : allStarts) {
                Link link;
                putOnStep(link, start);
                const auto alternate = [&](double seconds) {
                    for (double done = 0.0; done < seconds; done += 2.0 * half) {
                        link.setGuestRate(slow);
                        link.run(half);
                        link.setGuestRate(fast);
                        link.run(half);
                    }
                };
                alternate(6.0);
                const auto requests = link.requests().size();
                alternate(8.0);
                if (link.requests().size() != requests || link.rate() != 120)
                    std::fprintf(stderr, "%.0f<->%.0f half %.2f s from start %d changed\n",
                        slow, fast, half, static_cast<int>(start));
                require(link.requests().size() == requests);
                require(link.rate() == 120, "the 120 step");
            }
        }
    }
}

/* A steady guest makes no request after the link has settled. */
void aSteadyGuestMakesNoLinkFlip()
{
    for (const Start start : allStarts) {
        for (const double guest : {20.0, 30.0, 45.0, 59.0, 60.0, 61.0, 62.0,
                 63.0, 66.0, 80.0, 120.0}) {
            Link link;
            putOnStep(link, start);
            link.setGuestRate(guest);
            link.run(6.0);
            const auto requests = link.requests().size();
            link.run(8.0);
            if (link.requests().size() != requests)
                std::fprintf(stderr, "guest %.0f from start %d changed the range\n",
                    guest, static_cast<int>(start));
            require(link.requests().size() == requests,
                "a steady guest changed the range");
        }
    }
}

/* The step changes of a guest given as the times its frames arrive, driven
 * as the display link runs: a tick on a 120 Hz grid, every other one on the
 * 60 step, and a wake at the start. Returns the seconds after `from` of each
 * change in (from, from + length]. */
std::vector<double> stepChanges(
    const std::vector<double>& arrivals, double from, double length)
{
    PresentPacer pacer;
    (void)pacer.wake(0.0, 0, 120.0);
    std::size_t offered = 0;
    std::vector<double> changes;
    const auto last = static_cast<unsigned>((from + length) * 120.0 + 0.5);
    for (unsigned tick = 1; tick <= last;) {
        const double now = static_cast<double>(tick) / 120.0;
        const auto before = offered;
        while (offered < arrivals.size() && arrivals[offered] <= now + 1e-11)
            ++offered;
        const auto decision = pacer.tick(now, offered, offered != before, 120.0);
        if (decision.present)
            pacer.drew(false);
        if (decision.range && now > from)
            changes.push_back(now - from);
        tick += pacer.rate() == 120 ? 1U : 2U;
    }
    return changes;
}

/* A uniform pseudo-random number in [-1, 1) from a seed that advances. */
double nextOffset(std::uint32_t& seed)
{
    seed = seed * 1103515245U + 12345U;
    return static_cast<double>((seed >> 16U) % 2000U) / 1000.0 - 1.0;
}

/* Frame times of a guest whose rate is `rateAt(time)` (none for 0), each
 * `jitter` seconds early or late at most, from `phase`, to `seconds`. */
template <typename RateAt>
std::vector<double> jitteredArrivals(
    RateAt rateAt, double seconds, double jitter, double phase, std::uint32_t seed)
{
    std::vector<double> arrivals;
    double ideal = phase;
    while (ideal < seconds) {
        const double rate = rateAt(ideal);
        if (rate <= 0.0) {
            ideal += 0.001;
            continue;
        }
        arrivals.push_back(ideal + jitter * nextOffset(seed));
        ideal += 1.0 / rate;
    }
    std::sort(arrivals.begin(), arrivals.end());
    return arrivals;
}

/* The pattern two reviewers used: 4 s at 45 frames a second, then halves of
 * `half` seconds alternating `slow` and `fast`, the frames of a half placed in
 * groups, early and late by `amplitude` seconds a frame in a cosine of
 * `slowCycles` or `fastCycles` cycles from an eighth-turn `slowShift` or
 * `fastShift`, and the last of a half on its boundary. */
struct Groups {
    int slow;
    int fast;
    int half;
    int slowCycles;
    int fastCycles;
    int slowShift;
    int fastShift;
    double amplitude;
};

std::vector<double> groupedArrivals(const Groups& g)
{
    constexpr double pi = 3.141592653589793;
    std::vector<double> arrivals;
    for (int i = 1; i <= 180; ++i)
        arrivals.push_back(i / 45.0);
    for (int half = 0; half < 24 / g.half; ++half) {
        const double origin = 4.0 + half * g.half;
        double time = origin;
        const int rate = half % 2 != 0 ? g.fast : g.slow;
        const int frames = rate * g.half;
        const int cycles = half % 2 != 0 ? g.fastCycles : g.slowCycles;
        const int shift = half % 2 != 0 ? g.fastShift : g.slowShift;
        for (int frame = 0; frame < frames; ++frame) {
            const int slot = (frame * cycles + frames * shift / 8) % frames;
            double factor = slot < frames / 2 ? -1.0 : slot >= frames - frames / 2 ? 1.0 : 0.0;
            if (frames % cycles != 0)
                factor = std::cos(2.0 * pi * ((frame + 0.5) * cycles / frames + shift / 8.0));
            time += 1.0 / rate + g.amplitude * factor;
            arrivals.push_back(frame + 1 == frames ? origin + g.half : time);
        }
    }
    return arrivals;
}

/* The changes of a grouped guest in the 12 s that follow its settling. */
std::size_t changesOfGroups(const Groups& g)
{
    return stepChanges(groupedArrivals(g), 4.0 + (g.half == 2 ? 8.0 : 6.0), 12.0).size();
}

/* A step changes at most once in the dwell (2 s) whatever the guest does, so
 * a guest that alternates in halves of 2 s or less changes it at most once
 * per half cycle and one more, as the rows above bound it, and a source whose
 * frames arrive early and late in groups is no different. With the rise
 * measure of 62.5 and the hold under 61 (1.5 apart) alone deciding, such a
 * source made the step flip up to 44 times in 12 s, because early and late
 * frames move the count of a second by two between ticks. The first patterns
 * are those two reviewers found; the rest is a scan around them. */
void aGuestWhoseFramesArriveInGroupsAlternatesAtMostOncePerHalfCycle()
{
    struct Found {
        double amplitude;
        int half, slow, fast, slowCycles, fastCycles, slowShift, fastShift;
    };
    for (const Found f : {Found {0.001, 2, 57, 64, 1, 1, 0, 2}, Found {0.002, 2, 57, 64, 1, 1, 5, 2},
             Found {0.002, 2, 58, 66, 3, 1, 3, 3}, Found {0.002, 1, 57, 64, 1, 1, 5, 2},
             Found {0.003, 2, 57, 64, 1, 1, 6, 2}, Found {0.003, 2, 58, 66, 1, 1, 6, 2},
             Found {0.003, 2, 50, 70, 1, 1, 0, 2}, Found {0.003, 2, 40, 70, 1, 1, 0, 2},
             Found {0.003, 2, 60, 80, 2, 1, 6, 4}, Found {0.003, 1, 57, 64, 1, 1, 6, 2},
             Found {0.004, 2, 57, 64, 3, 3, 2, 7}, Found {0.004, 2, 58, 66, 1, 1, 6, 2},
             Found {0.004, 2, 50, 70, 1, 1, 5, 3}, Found {0.004, 2, 40, 70, 1, 1, 0, 2},
             Found {0.004, 2, 60, 80, 3, 1, 0, 0}, Found {0.004, 2, 62, 82, 3, 1, 5, 0},
             Found {0.004, 2, 30, 80, 1, 1, 0, 2}, Found {0.004, 1, 57, 64, 3, 1, 6, 2},
             Found {0.004, 1, 50, 70, 1, 1, 5, 2}}) {
        const Groups g {f.slow, f.fast, f.half, f.slowCycles, f.fastCycles,
            f.slowShift, f.fastShift, f.amplitude};
        const auto changes = changesOfGroups(g);
        const auto bound = static_cast<std::size_t>(12 / g.half) + 1U;
        if (changes > bound)
            std::fprintf(stderr, "%d/%d half %d s amplitude %.3f: %zu changes, bound %zu\n",
                g.slow, g.fast, g.half, g.amplitude, changes, bound);
        require(changes <= bound, "a found pattern changed the step too often");
    }
    for (const double amplitude : {0.0, 0.001, 0.002, 0.003, 0.004}) {
        for (const int half : {2, 1}) {
            for (const auto [slow, fast] : {std::pair {57, 64}, std::pair {58, 66},
                     std::pair {50, 70}, std::pair {40, 70}, std::pair {60, 80},
                     std::pair {62, 82}, std::pair {60, 120}, std::pair {30, 80}}) {
                for (const int slowCycles : {1, 3}) {
                    for (const int fastCycles : {1, 3}) {
                        for (const int slowShift : {0, 6}) {
                            for (const int fastShift : {0, 2}) {
                                const Groups g {slow, fast, half, slowCycles, fastCycles,
                                    slowShift, fastShift, amplitude};
                                const auto changes = changesOfGroups(g);
                                const auto bound = static_cast<std::size_t>(12 / half) + 1U;
                                if (changes > bound)
                                    std::fprintf(stderr,
                                        "%d/%d half %d s amplitude %.3f cycles %d/%d shifts %d/%d: %zu changes, bound %zu\n",
                                        slow, fast, half, amplitude, slowCycles, fastCycles,
                                        slowShift, fastShift, changes, bound);
                                require(changes <= bound, "a grouped guest changed the step too often");
                            }
                        }
                    }
                }
            }
        }
    }
}

/* The same bound for a guest whose frames are each early or late by up to 0,
 * 1, 2 or 4 ms around the ideal times of a rate that alternates, among them
 * pairs that straddle the threshold closely (60.5 and 62.5). */
void aGuestWhoseFramesAreEarlyAndLateAlternatesAtMostOncePerHalfCycle()
{
    for (const auto [slow, fast] : {std::pair {57.0, 64.0}, std::pair {58.0, 66.0},
             std::pair {50.0, 70.0}, std::pair {40.0, 70.0}, std::pair {20.0, 40.0},
             std::pair {60.0, 80.0}, std::pair {62.0, 82.0}, std::pair {60.0, 120.0},
             std::pair {30.0, 80.0}, std::pair {60.0, 63.0}, std::pair {61.0, 63.0},
             std::pair {60.0, 62.6}, std::pair {59.0, 64.0}, std::pair {55.0, 68.0},
             std::pair {60.5, 62.5}}) {
        for (const double half : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0}) {
            for (const double jitter : {0.0, 0.001, 0.002, 0.004}) {
                for (unsigned phase = 0; phase < 3; ++phase) {
                    for (const double lead : {0.0, 50.0}) {
                        const auto arrivals = jitteredArrivals(
                            [&](double time) {
                                if (time < 4.0 && lead > 0.0)
                                    return lead;
                                return std::fmod(time, 2.0 * half) < half ? slow : fast;
                            },
                            20.0, jitter, phase * 0.0037, 11U + phase);
                        const auto changes = stepChanges(arrivals, 8.0, 12.0).size();
                        const auto bound = static_cast<std::size_t>(12.0 / half) + 1U;
                        if (changes > bound)
                            std::fprintf(stderr,
                                "%.1f/%.1f half %.2f s jitter %.3f phase %u lead %.0f: %zu changes, bound %zu\n",
                                slow, fast, half, jitter, phase, lead, changes, bound);
                        require(changes <= bound, "an early and late guest changed the step too often");
                    }
                }
            }
        }
    }
}

/* A steady guest of 59 to 63 frames a second, whose frames are early or late
 * by up to 0, 1, 2 or 4 ms (which moves the count of a second by two between
 * ticks), makes no change after the link has settled when it renders 60 or
 * less or 63 or more, and one at most between them (a rise where the rise
 * measure is reached, never left again, or a fall). */
void aSteadyGuestOfFiftyNineToSixtyThreeChangesTheStepAtMostOnceAfterSettling()
{
    for (const double jitter : {0.0, 0.001, 0.002, 0.004}) {
        for (int tenths = 590; tenths <= 630; ++tenths) {
            const double guest = tenths / 10.0;
            for (unsigned phase = 0; phase < 12; ++phase) {
                for (const double lead : {0.0, 50.0}) {
                    const auto arrivals = jitteredArrivals(
                        [&](double time) { return time < 4.0 && lead > 0.0 ? lead : guest; },
                        24.0, jitter, phase * 0.00113, 101U + phase);
                    const auto changes = stepChanges(arrivals, 10.0, 12.0).size();
                    const bool between = guest > 60.0 && guest < 63.0;
                    if (changes > (between ? 1U : 0U))
                        std::fprintf(stderr, "guest %.1f jitter %.3f phase %u lead %.0f: %zu changes\n",
                            guest, jitter, phase, lead, changes);
                    require(changes <= (between ? 1U : 0U), "a steady guest changed the step");
                }
            }
        }
    }
}

/* The same through the link of the other rows, whose jitter is in the interval
 * between frames, so that the frame times drift: with up to 2 ms a guest of 59
 * to 63 changes the step at most once in the 12 s after it settles, and not at
 * all at 60 or less or 63 or more. */
void aSteadyGuestOfFiftyNineToSixtyThreeWithIntervalJitterChangesTheStepAtMostOnce()
{
    for (const double jitter : {0.0, 0.001, 0.002}) {
        for (int tenths = 590; tenths <= 630; ++tenths) {
            const double guest = tenths / 10.0;
            for (const Start start : allStarts) {
                Link link;
                link.setJitter(jitter);
                putOnStep(link, start);
                link.setGuestRate(guest);
                link.run(6.0);
                const auto requests = link.requests().size();
                link.run(12.0);
                const auto changes = link.requests().size() - requests;
                const bool between = guest > 60.0 && guest < 63.0;
                if (changes > (between ? 1U : 0U))
                    std::fprintf(stderr, "guest %.1f jitter %.3f from start %d: %zu changes\n",
                        guest, jitter, static_cast<int>(start), changes);
                require(changes <= (between ? 1U : 0U), "a steady guest changed the step");
            }
        }
    }
}

/* With 4 ms in the interval between frames (the frame times of a second drift
 * by a tenth of a frame, and the count of a second by two) a steady guest
 * between 60 and 63 can still change the step more than once in 12 s: the
 * guarantee is the dwell, no two changes within 2 s of each other, where the
 * reviewers' fixture changed it five times, two of them 100 ms apart. A
 * guest of 60 or less, or 63 or more, makes no change after it settles. */
void aSteadyGuestWithFourMillisecondsOfIntervalJitterNeverChangesTheStepTwiceInTheDwell()
{
    for (int tenths = 590; tenths <= 630; ++tenths) {
        const double guest = tenths / 10.0;
        for (const Start start : allStarts) {
            Link link;
            link.setJitter(0.004);
            putOnStep(link, start);
            link.setGuestRate(guest);
            link.run(6.0);
            std::size_t requests = link.requests().size();
            double last = -1.0;
            unsigned changes = 0;
            for (double done = 0.0; done < 12.0; done += 0.01) {
                link.run(0.01);
                if (link.requests().size() == requests)
                    continue;
                requests = link.requests().size();
                ++changes;
                if (last >= 0.0 && link.lastRequestAt() - last < 2.0 - 1e-9)
                    std::fprintf(stderr, "guest %.1f from start %d: changes %.3f s apart\n",
                        guest, static_cast<int>(start), link.lastRequestAt() - last);
                require(last < 0.0 || link.lastRequestAt() - last >= 2.0 - 1e-9,
                    "two changes within the dwell");
                last = link.lastRequestAt();
            }
            const bool between = guest > 60.0 && guest < 63.0;
            require(between || changes == 0, "a steady guest outside 60 to 63 changed the step");
        }
    }
}

/* A step change waits for 2 s after the last one. A guest of 40 frames a
 * second settles the link on the 60 step, 90 takes it up as soon as the rise
 * measure is reached, and 20 right after takes it down no sooner than 2 s
 * after that rise (the measures would take it down in a second); 90 again
 * takes it up no sooner than 2 s after the fall. A ceiling under the step is
 * not waited for. */
void aStepChangeWaitsForTheDwellSinceTheLastOne()
{
    const auto runUntil = [](Link& link, unsigned step) {
        for (double waited = 0.0; link.rate() != step && waited < 6.0; waited += 0.01)
            link.run(0.01);
        require(link.rate() == step, "the step was not reached");
    };
    Link link;
    link.setJitter(0.0);
    link.setGuestRate(40.0);
    link.run(5.0);
    require(link.rate() == 60, "settled on the 60 step");
    link.setGuestRate(90.0);
    runUntil(link, 120);
    const double rose = link.lastRequestAt();
    link.setGuestRate(20.0);
    link.run(1.5);
    require(link.rate() == 120, "held by the dwell");
    runUntil(link, 60);
    const double fell = link.lastRequestAt();
    require(fell - rose >= 2.0 - 1e-9, "no sooner than 2 s after the rise");
    link.setGuestRate(90.0);
    link.run(1.5);
    require(link.rate() == 60, "a rise waits for the dwell since the fall too");
    runUntil(link, 120);
    require(link.lastRequestAt() - fell >= 2.0 - 1e-9, "no sooner than 2 s after the fall");

    /* A ceiling that falls is not a measure: the step goes at once. */
    link.setCeiling(60.0);
    link.run(0.05);
    require(link.rate() == 60, "a ceiling under the step is left at once");
}

/* A draw that finds no drawable shows nothing: with no later frame to ask
 * again, the link must try the same frame at its next ticks, and sleep only
 * when it is shown. */
void aDrawThatFindsNoDrawableIsRetriedUntilTheFrameIsShown()
{
    Link link;
    link.offer();
    link.failNextDraws(5);
    link.run(0.3);
    require(link.presents() == 1, "the frame was shown once");
    require(link.draws() == 6, "five failed draws and the one that showed it");
    require(!link.frameWaiting());
    link.run(0.3);
    require(link.asleep(), "the link sleeps after the frame is shown");
    require(link.draws() == 6, "no further draws");
}

/* A drawable that never comes costs a bounded number of draws, and the link
 * sleeps after them: an idle link does not tick for ever. */
void aDrawThatNeverFindsADrawableIsRetriedAtMostThirtyTimes()
{
    Link link;
    link.offer();
    link.failNextDraws(1000);
    link.run(1.0);
    require(link.draws() == 31, "the first draw and thirty retries");
    require(link.presents() == 0);
    require(link.asleep(), "the link slept once the retries were spent");
    /* A new frame has its own retries, and a drawable that is back shows it
     * at the next tick. */
    link.failNextDraws(2);
    link.offer();
    link.run(0.2);
    require(link.presents() == 1, "the next frame was shown");
    require(link.draws() == 31 + 3);
    link.run(0.3);
    require(link.asleep());
}

/* A draw that waited out a layer's own timeout (a second for a drawable that
 * the display holds) is not drawn again, and the waits of a frame's draws are
 * bounded in all: with every draw waiting a second, the thirty retries held
 * the thread that draws for 31 s (the first draw and thirty retries); now the
 * first draw is the only one. A wait that is shorter ends the retries when
 * the waits add up to 0.9 s, so a frame costs under 0.9 s and one wait more,
 * and a draw that fails at once keeps its thirty retries. */
void aDrawWhoseDrawableWaitRanOutTheTimeoutIsNotDrawnAgain()
{
    /* Before: no limit on the wait. */
    PresentPacer::Parameters unlimited;
    unlimited.maximumDrawableWaitSeconds = 1e9;
    Link before(120.0, unlimited);
    before.setDrawableWait(1.0);
    before.offer();
    before.failNextDraws(1000);
    before.run(1.0);
    require(before.draws() == 31, "thirty retries without the limit");
    require(std::abs(before.mainThreadSeconds() - 31.0) < 1e-9, "31 s of the thread that draws");

    Link link;
    link.setDrawableWait(1.0);
    link.offer();
    link.failNextDraws(1000);
    link.run(1.0);
    require(link.draws() == 1, "a draw that waited out the timeout is the only one");
    require(std::abs(link.mainThreadSeconds() - 1.0) < 1e-9, "one wait of the thread that draws");
    require(link.presents() == 0);
    require(link.asleep(), "the link slept");

    /* A frame that arrives has its own budget, and a drawable that is back
     * shows it. */
    link.failNextDraws(1);
    link.offer();
    link.run(0.2);
    require(link.draws() == 2 && link.mainThreadSeconds() < 2.0 + 1e-9);
    link.failNextDraws(0);
    link.offer();
    link.run(0.2);
    require(link.presents() == 1, "the next frame was shown");

    /* Waits that are shorter add up: the time of a frame's retries is under
     * the 0.9 s limit and one wait more, whatever the wait. */
    for (const double wait : {0.05, 0.25, 0.4, 0.89, 0.95, 1.0}) {
        Link slow;
        slow.setDrawableWait(wait);
        slow.offer();
        slow.failNextDraws(1000);
        slow.run(1.0);
        if (!(slow.mainThreadSeconds() < 0.9 + wait + 1e-9))
            std::fprintf(stderr, "waits of %.2f s cost %.2f s\n", wait, slow.mainThreadSeconds());
        require(slow.mainThreadSeconds() < 0.9 + wait + 1e-9, "the waits are bounded");
        require(slow.mainThreadSeconds() < 2.0, "under two seconds for a frame");
        require(slow.asleep(), "the link slept");
    }

    /* A draw that fails at once has no wait to count: its thirty retries. */
    Link quick;
    quick.setDrawableWait(0.0);
    quick.offer();
    quick.failNextDraws(1000);
    quick.run(1.0);
    require(quick.draws() == 31, "a draw that fails at once keeps its retries");
}

/* The waits of a frame are not renewed by a link made again: the scene went and
 * another activated, which asks for the frame once more, not for its wait. */
void aLinkMadeAgainDoesNotRenewTheDrawableWait()
{
    Link link;
    link.setDrawableWait(1.0);
    link.offer();
    link.failNextDraws(1000);
    link.run(0.05);
    for (unsigned made = 0; made < 5; ++made) {
        link.rebind();
        link.run(0.05);
    }
    require(link.draws() == 1, "no draw after the wait was spent");
    require(std::abs(link.mainThreadSeconds() - 1.0) < 1e-9, "one wait in all");
}

/* The waits of one frame, whatever each request waits. A draw asks the layer
 * for a drawable once, and only while its frame has budget left (0.9 s), so
 * the waits of a frame add up to under the budget and one wait of the layer (a
 * second): 1.9 s. A frame whose requests wait 0.375, 0.375, 0.95 and 0.95 s
 * stops after the third, at 1.7 s; a draw that asked twice, as the draws did
 * (the view's render pass, then its drawable), cost 2.66 s in four waits.
 * The row drives the test's own model of the draw, so it checks the pacer's
 * side (the budget it hands out and the waits it records); that a draw asks
 * once is `metal_presentation.mm`'s, which no unit test runs. */
void theWaitsOfAFrameAreBoundedWhateverEachRequestWaits()
{
    Link link;
    link.setRequestWaits({0.375, 0.375, 0.95, 0.95});
    link.offer();
    link.failNextDraws(1000);
    link.run(1.0);
    require(link.requestsMade() == 3, "the fourth request is not made");
    require(link.draws() == 3);
    require(std::abs(link.mainThreadSeconds() - 1.7) < 1e-9, "1.7 s in three waits");
    require(link.asleep(), "the link slept");

    /* Any plan of waits of up to a second costs a frame under 1.9 s. */
    std::uint32_t seed = 7;
    double worst = 0.0;
    for (int plan = 0; plan < 200; ++plan) {
        std::vector<double> waits;
        for (int request = 0; request < 40; ++request) {
            seed = seed * 1103515245U + 12345U;
            waits.push_back(static_cast<double>((seed >> 16U) % 1000U + 1U) / 1000.0);
        }
        Link swept;
        swept.setRequestWaits(waits);
        swept.offer();
        swept.failNextDraws(1000);
        swept.run(1.0);
        worst = std::max(worst, swept.mainThreadSeconds());
        require(swept.mainThreadSeconds() < 1.9 + 1e-9, "a frame costs under 1.9 s");
    }
    require(worst > 1.0, "the sweep reaches past a second");
}

/* A frame that arrives after the tick took its flag and before the draw claims
 * the newest frame is the frame that is drawn, and its arrival leaves the flag
 * set for the next tick: that tick draws the same frame again, and its budget
 * is not renewed by the flag. With requests that wait 0.99, 0.79 and 0.95 s the
 * frame cost 2.73 s in three draws; now one request, and the next tick asks
 * nothing. */
void aFrameThatArrivesBetweenTheFlagAndTheClaimHasOneBudget()
{
    Link link;
    link.setRequestWaits({0.99, 0.79, 0.95});
    link.offer();
    link.arriveBeforeNextClaim();
    link.failNextDraws(1000);
    link.run(1.0);
    require(link.requestsMade() == 1, "the layer was asked once for it");
    require(std::abs(link.mainThreadSeconds() - 0.99) < 1e-9, "one wait of the thread that draws");
    require(link.draws() == 2, "the tick that the flag left set drew it again");
    require(link.presents() == 0 && link.asleep());

    /* The next frame has a budget of its own, and a drawable shows it. */
    link.setRequestWaits({});
    link.failNextDraws(0);
    link.offer();
    link.run(0.2);
    require(link.presents() == 1, "the next frame was shown");
}

/* A drawable that is in hand is drawn into, whatever the request cost: the
 * request whose wait spent the budget may be the one that returns it. The row
 * drives the test's own model of the draw, so it checks the pacer's side (the
 * link sleeps and wakes right after a shown draw); that the draw does not
 * discard the drawable is `metal_presentation.mm`'s, which no unit test runs. */
void aDrawableThatArrivesAfterALongWaitShowsTheFrame()
{
    Link link;
    link.setRequestWaits({0.75, 0.96});
    link.offer();
    link.failNextDraws(1);
    link.run(1.0);
    require(link.requestsMade() == 2);
    require(link.presents() == 1, "the frame was shown by the drawable that came late");
    require(std::abs(link.mainThreadSeconds() - 1.71) < 1e-9);
    require(!link.frameWaiting() && link.asleep());
}

/* The pacer's side: the budget is the frame's that was drawn. */
void theDrawableBudgetBelongsToTheFrameThatWasDrawn()
{
    PresentPacer pacer;
    require(std::abs(pacer.drawableBudget().allowanceFor(5) - 0.9) < 1e-9, "a frame no draw tried has the whole budget");
    pacer.drew(true, 0.6, 5);
    require(pacer.drawableBudget().frame == 5);
    require(std::abs(pacer.drawableBudget().allowanceFor(5) - 0.3) < 1e-9, "0.3 s of the frame's are left");
    require(std::abs(pacer.drawableBudget().allowanceFor(6) - 0.9) < 1e-9, "another frame has the whole of its own");
    require(pacer.retryOwed());
    pacer.drew(true, 0.5, 5);
    require(pacer.drawableBudget().allowanceFor(5) == 0.0, "none is left");
    require(!pacer.retryOwed());
    /* The frame that was drawn after it starts again, and the first frame is
     * not remembered. */
    pacer.drew(true, 0.5, 6);
    require(pacer.retryOwed());
    require(std::abs(pacer.drawableBudget().allowanceFor(6) - 0.4) < 1e-9);
    pacer.drew(false, 0.0, 6);
    require(pacer.drawableBudget().frame == 0 && !pacer.retryOwed());
    require(std::abs(pacer.drawableBudget().allowanceFor(6) - 0.9) < 1e-9, "a shown frame owes nothing");
}

/* The pacer's side of it, with no link: a retry is a present that no frame
 * asked for, a shown draw ends the debt, and a frame renews it. */
void aRetryIsAPresentNoFrameAskedFor()
{
    PresentPacer pacer;
    (void)pacer.wake(0.0, 0, 120.0);
    require(pacer.tick(0.0083, 1, true, 120.0).present);
    pacer.drew(true);
    const auto retry = pacer.tick(0.0166, 1, false, 120.0);
    require(retry.present && !retry.sleep, "the retry presents");
    pacer.drew(false);
    const auto done = pacer.tick(0.0249, 1, false, 120.0);
    require(!done.present && !done.sleep, "a shown frame owes nothing");
    /* Never asleep while a retry is owed, though no frame has waited. */
    PresentPacer owing;
    (void)owing.wake(0.0, 0, 120.0);
    require(owing.tick(0.01, 1, true, 120.0).present);
    owing.drew(true);
    require(owing.tick(0.5, 1, false, 120.0).present, "owed after 0.5 s");
    owing.drew(false);
    require(owing.tick(0.51, 1, false, 120.0).sleep == false);
    require(owing.tick(0.7, 1, false, 120.0).sleep);
}

/* A link made again while the newest frame is owed another draw (its scene
 * disconnected and another activated before the next tick) goes on with that
 * frame: the debt is the display's, not the link's. */
void aLinkMadeAgainWhileAFrameIsOwedStillShowsIt()
{
    Link link;
    link.offer();
    link.failNextDraws(1);
    link.run(0.010);
    require(link.draws() == 1 && link.presents() == 0, "one draw, which failed");
    require(link.frameWaiting());
    link.rebind();
    link.run(0.3);
    require(link.presents() == 1, "the frame was shown by the new link");
    require(!link.frameWaiting());
    require(link.draws() == 2, "no more than the one retry");
    link.run(0.3);
    require(link.asleep());
}

/* The limit that is left goes with it: a link made again and again with no
 * new frame never owes more than the frame's own draws. */
void aLinkMadeAgainAndAgainDoesNotRenewTheRetries()
{
    Link link;
    link.offer();
    link.failNextDraws(1000);
    for (int round = 0; round < 40; ++round) {
        link.run(0.020);
        link.rebind();
    }
    link.run(1.0);
    require(link.draws() == 31, "the first draw and thirty retries");
    require(link.presents() == 0);
    require(link.asleep(), "the link slept once they were spent");
    /* Only a frame renews them. */
    link.rebind();
    link.run(0.3);
    require(link.draws() == 31, "a link made again owes nothing more");
    link.failNextDraws(0);
    link.offer();
    link.run(0.2);
    require(link.presents() == 1 && link.draws() == 32);
}

/* The display with no link retries after a delay it arranges: one delay at a
 * time, whatever frames arrive, and none when nothing is owed. */
void theDelayedRetryOfADisplayWithNoLinkIsOneAtATime()
{
    PresentPacer pacer;
    require(!pacer.arrangeRetry(), "nothing owed, nothing arranged");
    pacer.drew(true, 0.0, 1);
    require(pacer.retryOwed());
    require(pacer.arrangeRetry(), "the first is arranged");
    /* More frames, more failed draws: still the one. */
    for (std::uint64_t frame = 2; frame < 102; ++frame) {
        pacer.drew(true, 0.0, frame);
        require(!pacer.arrangeRetry(), "a delay is pending already");
    }
    pacer.retryDelayRan();
    require(pacer.arrangeRetry(), "the next after the first ran");
    pacer.retryDelayRan();
    pacer.drew(false);
    require(!pacer.arrangeRetry(), "a frame shown owes nothing");
    /* The limit of a frame is thirty, however many delays it takes, and
     * delays alone do not renew it. */
    PresentPacer spent;
    unsigned draws = 0;
    for (int tries = 0; tries < 100; ++tries) {
        spent.drew(true, 0.0, 1);
        ++draws;
        if (!spent.retryOwed())
            break;
    }
    require(draws == 31, "the draw and thirty retries");
    spent.retryDelayRan();
    require(!spent.retryOwed() && !spent.arrangeRetry(), "nothing more is owed");
    spent.drew(true, 0.0, 1);
    require(!spent.retryOwed(), "the same frame has no more");
    spent.drew(true, 0.0, 2);
    require(spent.retryOwed(), "a frame that is drawn has its own");
}

void theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime()
{
    Link link;
    link.setGuestRate(60.0);
    link.run(1.0);
    require(!link.asleep());
    link.setGuestRate(0.0);
    link.run(0.05);
    require(!link.asleep(), "not after 50 ms");
    link.run(0.2);
    require(link.asleep(), "after 250 ms");

    /* Exactly: the tick that finds the last frame presented 100 ms ago
     * sleeps; one a fraction of a millisecond earlier does not. */
    PresentPacer pacer;
    (void)pacer.wake(10.0, 0, 120.0);
    require(pacer.tick(10.0083, 1, true, 120.0).present);
    const auto before = pacer.tick(10.108, 1, false, 120.0);
    require(!before.sleep && !before.present, "99.7 ms after the present");
    const auto after = pacer.tick(10.1084, 1, false, 120.0);
    require(after.sleep && !after.present, "100.1 ms after the present");
}

void aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake()
{
    Link link;
    /* One frame every 90 ms: the link never sleeps, though only one tick in
     * many has a frame. */
    link.setGuestRate(1.0 / 0.09);
    link.run(2.0);
    require(link.wakes() == 1, "one wake");
    require(!link.asleep());
    /* One frame every 150 ms: the link sleeps between them and wakes for
     * each. */
    Link sparse;
    sparse.setGuestRate(1.0 / 0.15);
    sparse.run(3.0);
    require(sparse.wakes() >= 15, "a wake for most frames");
    require(sparse.presents() >= sparse.frames() - 1U);
    /* The first frame after a wake waits for the first tick of the fastest
     * rate, which is at most 8.4 ms. */
    require(sparse.longestWait() < 0.0105);
}

void aTickPresentsOnlyWhenAFrameWaits()
{
    PresentPacer pacer;
    (void)pacer.wake(0.0, 0, 120.0);
    const auto empty = pacer.tick(0.0083, 0, false, 120.0);
    require(!empty.present && !empty.sleep);
    const auto waiting = pacer.tick(0.0166, 1, true, 120.0);
    require(waiting.present && !waiting.sleep);
}

void theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises()
{
    Link link;
    link.setGuestRate(120.0);
    link.run(1.0);
    require(link.rate() == 120);
    link.setCeiling(60.0);
    link.run(0.05);
    require(link.rate() == 60, "capped at the next tick");
    require(link.requests().back() == normal);
    /* The guest still renders 120: the link stays at the cap. */
    link.run(2.0);
    require(link.rate() == 60);
    link.setCeiling(120.0);
    link.run(0.4);
    require(link.rate() == 120, "rose when the cap lifted");
    /* A link that wakes under a cap asks for the cap. */
    Link capped {60.0};
    capped.setGuestRate(120.0);
    capped.run(1.0);
    require(capped.requests().front() == normal);
    require(capped.rate() == 60);
}

void aWakeForgetsTheEvidenceOfTheLastBurst()
{
    Link link;
    link.setGuestRate(45.0);
    link.run(1.5);
    require(link.rate() == 60);
    link.setGuestRate(0.0);
    link.run(0.5);
    require(link.asleep());
    /* The next burst is at 120, and the first request is the fastest step
     * again, not the 60 the last burst ended on. */
    const auto before = link.requests().size();
    link.setGuestRate(120.0);
    link.run(0.2);
    require(link.requests().size() == before + 1);
    require(link.requests().back() == high);
    require(link.rate() == 120);
}

/* The link as the display runs it on a 120 Hz vsync grid, driven by the times
 * a guest's frames arrive (sorted): it sleeps when no frame waits, wakes at
 * the next frame, and ticks at the step it asked for (every vsync at the 120
 * step, every second one at the 60 step). Returns when the step changed and to
 * what, the first request included. With `vsyncsPerTick`, a link that ticks that
 * many vsyncs apart at either step, as one does when its thread is busy. */
struct StepChange {
    double at;
    unsigned to;
};

std::vector<StepChange> stepChangesOf(
    const std::vector<double>& frames, double seconds, double vsyncsPerTick = 0.0)
{
    constexpr double vsync = 1.0 / 120.0;
    std::vector<StepChange> changes;
    PresentPacer pacer;
    std::uint64_t offered = 0;
    std::size_t next = 0;
    bool awake = false;
    bool waiting = false;
    double now = 0.0;
    double asked = 60.0;
    unsigned noted = 0;
    const auto note = [&](double at, unsigned rate) {
        if (noted != 0 && rate != noted)
            changes.push_back({at, rate});
        noted = rate;
    };
    while (true) {
        if (!awake) {
            if (next >= frames.size() || frames[next] >= seconds)
                break;
            const double arrival = frames[next];
            while (next < frames.size() && frames[next] <= arrival) {
                ++offered;
                ++next;
            }
            waiting = true;
            asked = pacer.wake(arrival, offered, 120.0).preferred;
            note(arrival, pacer.rate());
            awake = true;
            now = std::ceil(arrival / vsync + 1e-9) * vsync;
        }
        if (now >= seconds)
            break;
        while (next < frames.size() && frames[next] <= now) {
            ++offered;
            ++next;
            waiting = true;
        }
        const bool had = waiting;
        waiting = false;
        const auto decision = pacer.tick(now, offered, had, 120.0);
        if (decision.range)
            asked = decision.range->preferred;
        note(now, pacer.rate());
        if (decision.present)
            pacer.drew(false);
        if (decision.sleep) {
            awake = false;
            continue;
        }
        now += vsyncsPerTick > 0.0 ? vsyncsPerTick * vsync : std::round(120.0 / asked) * vsync;
    }
    return changes;
}

/* When the step first became `to` after `after`, or nothing. */
std::optional<double> firstChangeTo(
    const std::vector<StepChange>& changes, unsigned to, double after)
{
    for (const StepChange& change : changes)
        if (change.at > after && change.to == to)
            return change.at;
    return std::nullopt;
}

/* A guest of 30 frames a second, which leaves the link on the 60 step, and the
 * time that step was reached. */
std::vector<double> slowLead(double seconds)
{
    std::vector<double> frames;
    for (int i = 1; i / 30.0 < seconds; ++i)
        frames.push_back(i / 30.0);
    return frames;
}

/* `perSecond` frames in each calendar second from `epoch`, of which `bunched`
 * arrive within three milliseconds of the second's boundaries (each boundary
 * holds one early frame fewer and one late one more, until none is early), and
 * the others are evenly spaced: the frames of a guest that catches up in one
 * go once a second. */
void addBoundaryBunchedFrames(
    std::vector<double>& frames, double epoch, int seconds, int perSecond, int bunched)
{
    for (int s = 0; s < seconds; ++s) {
        const double start = epoch + s;
        const int evenly = perSecond - bunched;
        for (int j = 0; j < evenly; ++j)
            frames.push_back(start + (j + 0.5) / evenly);
        const int early = std::max(0, bunched - s);
        for (int i = 0; i < early; ++i)
            frames.push_back(start + 0.0001 + 0.0007 * i);
        for (int i = 0; i < bunched - early; ++i)
            frames.push_back(start + 0.9999 - 0.0007 * i);
    }
    std::sort(frames.begin(), frames.end());
}

/* A tick counts two of the frames that arrive within it at most, at the 60
 * step's ticks as at the 120 step's. A guest of 63 frames a second with 3 to
 * 12 of them within three milliseconds of a second's boundary offers the
 * display 62 or fewer, and a guest at 60 that catches up with a burst once a
 * second offers it 61: neither takes the 120 step. The count that held them on
 * the 60 step for 4 to 7 s before the burst was over, and took the 120 step in
 * the end, counted every frame. */
void aGuestWhoseExtraFramesBunchInOneTickStaysOnTheSixtyStep()
{
    for (const int bunched : {3, 4, 6, 9, 12}) {
        for (const double phase : {0.0, 0.002, 0.004, 0.006}) {
            std::vector<double> frames = slowLead(3.0);
            const auto lead = stepChangesOf(frames, 3.0);
            const auto down = firstChangeTo(lead, 60, 0.0);
            require(down.has_value(), "the slow lead did not reach the 60 step");
            const double epoch = *down + phase;
            frames.erase(std::remove_if(frames.begin(), frames.end(),
                             [&](double at) { return at >= epoch; }),
                frames.end());
            addBoundaryBunchedFrames(frames, epoch, 20, 63, bunched);
            const auto changes = stepChangesOf(frames, epoch + 20.0);
            const auto rose = firstChangeTo(changes, 120, epoch);
            if (rose)
                std::fprintf(stderr, "bunched %d phase %.3f: the 120 step at %.3f s\n",
                    bunched, phase, *rose - epoch);
            require(!rose.has_value(), "a burst of frames in one tick took the 120 step");
        }
    }
    /* A guest at 60 that catches up with 20 frames in one tick each second. */
    std::vector<double> frames = slowLead(3.0);
    const auto down = firstChangeTo(stepChangesOf(frames, 3.0), 60, 0.0);
    require(down.has_value());
    const double epoch = *down + 0.002;
    frames.erase(std::remove_if(frames.begin(), frames.end(),
                     [&](double at) { return at >= epoch; }),
        frames.end());
    addBoundaryBunchedFrames(frames, epoch, 20, 80, 20);
    require(!firstChangeTo(stepChangesOf(frames, epoch + 20.0), 120, epoch).has_value(),
        "a guest at 60 with a burst of 20 frames in one tick took the 120 step");
}

/* Frames spread over ticks are followed as before: a guest of 63, 64 or 66
 * frames a second evenly spaced takes the 120 step as soon as the dwell since
 * the fall has passed, as its second of frames has been counted by then. */
void aGuestWhoseFramesAreSpreadOverTicksRisesAtTheDwell()
{
    for (const int perSecond : {63, 64, 66}) {
        for (const double phase : {0.0, 0.003, 0.006}) {
            std::vector<double> frames = slowLead(3.0);
            const auto down = firstChangeTo(stepChangesOf(frames, 3.0), 60, 0.0);
            require(down.has_value());
            const double epoch = *down + phase;
            frames.erase(std::remove_if(frames.begin(), frames.end(),
                             [&](double at) { return at >= epoch; }),
                frames.end());
            for (int s = 0; s < 8; ++s)
                for (int j = 0; j < perSecond; ++j)
                    frames.push_back(epoch + s + (j + 0.5) / perSecond);
            std::sort(frames.begin(), frames.end());
            const auto rose = firstChangeTo(stepChangesOf(frames, epoch + 8.0), 120, epoch);
            require(rose.has_value(), "an evenly spaced guest above 62.5 did not rise");
            if (*rose - epoch > 2.1)
                std::fprintf(stderr, "%d a second, phase %.3f: the 120 step at %.3f s\n",
                    perSecond, phase, *rose - epoch);
            require(*rose - epoch <= 2.1, "the rise took longer than the dwell");
        }
    }
}

/* A guest that speeds up from 30 to 90 or 120 frames a second with 3 to 12 of
 * them within three milliseconds of each second's boundary is followed within
 * the dwell and the window, as an evenly spaced one is: a burst is two frames
 * at most to the count, and the rest of the guest's frames are far over the
 * threshold. */
void aGuestThatSpeedsUpWithBunchedFramesIsFollowedWithinTheDwellAndTheWindow()
{
    for (const int perSecond : {90, 120}) {
        for (const int bunched : {3, 6, 12}) {
            for (const double phase : {0.0, 0.003, 0.006}) {
                std::vector<double> frames = slowLead(3.0);
                const auto down = firstChangeTo(stepChangesOf(frames, 3.0), 60, 0.0);
                require(down.has_value());
                const double epoch = *down + phase;
                frames.erase(std::remove_if(frames.begin(), frames.end(),
                                 [&](double at) { return at >= epoch; }),
                    frames.end());
                addBoundaryBunchedFrames(frames, epoch, 8, perSecond, bunched);
                const auto rose = firstChangeTo(stepChangesOf(frames, epoch + 8.0), 120, epoch);
                require(rose.has_value(), "a guest of 90 or more with a burst did not rise");
                if (*rose - epoch > 2.1)
                    std::fprintf(stderr,
                        "%d a second, %d bunched, phase %.3f: the 120 step at %.3f s\n",
                        perSecond, bunched, phase, *rose - epoch);
                require(*rose - epoch <= 2.1, "the rise took longer than the dwell");
            }
        }
    }
}

/* A link that ticks several vsyncs apart (as one does while its thread is busy)
 * counts the frames of the time each tick holds: a tick that follows 4 vsyncs
 * counts 4 frames of a guest at 120, where a count of two a tick (or of what a
 * 60 Hz display shows) takes the guest for one at 60, drops it to the 60 step
 * from a wake and holds it there. The count is rounded up from a quarter of a
 * vsync: a guest whose frames come in lumps of ten every second tick of a link
 * that ticks 5.3, 6.3 or 8.3 vsyncs apart counts 6, 7 or 9 a lump, 68, 67 or 65
 * a second, and a count rounded down from the half or the whole counts 5, 6 or
 * 8 a lump, 57 or 58, and does not rise or falls. */
void aSlowLinkCountsTheFramesOfTheTimeEachTickHolds()
{
    /* A guest evenly spaced at 90 or 120 frames a second: the 120 step from a
     * wake, and from the 60 step within the dwell and the window. */
    for (const double vsyncs : {4.0, 6.0}) {
        for (const double perSecond : {90.0, 120.0}) {
            std::vector<double> frames;
            for (int i = 0; 1.0 + i / perSecond < 9.0; ++i)
                frames.push_back(1.0 + i / perSecond);
            require(stepChangesOf(frames, 9.0, vsyncs).empty(),
                "a guest of 90 or 120 on a slow link did not keep the 120 step from a wake");
            frames = slowLead(3.0);
            const auto down = firstChangeTo(stepChangesOf(frames, 3.0, vsyncs), 60, 0.0);
            require(down.has_value(), "the slow lead did not reach the 60 step");
            const double epoch = *down + 0.002;
            frames.erase(std::remove_if(frames.begin(), frames.end(),
                             [&](double at) { return at >= epoch; }),
                frames.end());
            for (int i = 0; epoch + i / perSecond < epoch + 8.0; ++i)
                frames.push_back(epoch + i / perSecond);
            const auto rose = firstChangeTo(stepChangesOf(frames, epoch + 8.0, vsyncs), 120, epoch);
            require(rose.has_value(), "a guest of 90 or 120 on a slow link did not rise");
            if (*rose - epoch > 2.2)
                std::fprintf(stderr, "%.0f vsyncs, %.0f a second: the 120 step at %.3f s\n",
                    vsyncs, perSecond, *rose - epoch);
            require(*rose - epoch <= 2.2, "the rise on a slow link took longer than the dwell");
        }
    }
    /* A guest whose frames come in lumps of ten every second tick. */
    for (const double vsyncs : {5.3, 6.3, 8.3}) {
        const double tick = vsyncs / 120.0;
        std::vector<double> frames;
        for (int i = 0; 1.0 + 2 * i * tick < 9.0; ++i)
            for (int j = 0; j < 10; ++j)
                frames.push_back(1.0 + 2 * i * tick + 0.0001 * j);
        require(stepChangesOf(frames, 9.0, vsyncs).empty(),
            "a lumpy guest on a slow link did not keep the 120 step from a wake");
        frames = slowLead(3.0);
        const auto down = firstChangeTo(stepChangesOf(frames, 3.0, vsyncs), 60, 0.0);
        require(down.has_value(), "the slow lead did not reach the 60 step");
        const double epoch = *down;
        frames.erase(std::remove_if(frames.begin(), frames.end(),
                         [&](double at) { return at >= epoch; }),
            frames.end());
        for (int i = 0; epoch + (2 * i + 0.5) * tick < epoch + 8.0; ++i)
            for (int j = 0; j < 10; ++j)
                frames.push_back(epoch + (2 * i + 0.5) * tick + 0.0001 * j);
        const auto rose = firstChangeTo(stepChangesOf(frames, epoch + 8.0, vsyncs), 120, epoch);
        require(rose.has_value(), "a lumpy guest on a slow link did not rise");
        if (*rose - epoch > 2.5)
            std::fprintf(stderr, "%.1f vsyncs, lumps: the 120 step at %.3f s\n", vsyncs,
                *rose - epoch);
        require(*rose - epoch <= 2.5, "the rise of a lumpy guest on a slow link took too long");
    }
}

/* A guest that falls from 120 to 60 with 3 of its frames within a millisecond
 * of each second's boundary is followed within the dwell and the window, for
 * every tick length the display can have: the count that held it on the 120
 * step for five seconds counted the frames of the bunch as frames of the
 * display, which only two of the three per tick are. */
void aGuestThatFallsToSixtyWithBunchedFramesIsFollowedWithinTheDwellAndTheWindow()
{
    for (const double highTick : {1.0 / 120.00001, 1.0 / 120.0, 1.0 / 119.88, 1.0 / 119.98}) {
        for (const double phase : {0.002, 0.004, 0.006}) {
            PresentPacer pacer;
            (void)pacer.wake(0.0, 0, 120.0);
            std::vector<double> frames;
            for (int i = 1; i <= 120; ++i)
                frames.push_back(i / 30.0);
            for (int i = 1; i <= 120; ++i)
                frames.push_back(4.0 + i / 120.0);
            std::size_t count = 0;
            double epoch = -1.0;
            double low = -1.0;
            int k = 1;
            while (k * highTick < 13.0) {
                const double now = k * highTick;
                const std::size_t before = count;
                while (count < frames.size() && frames[count] <= now)
                    ++count;
                const auto decision = pacer.tick(now, count, count != before, 120.0);
                if (decision.present)
                    pacer.drew(false);
                if (epoch > 0.0 && decision.range && pacer.rate() == 60) {
                    low = now;
                    break;
                }
                if (now > 4.0 && epoch < 0.0 && decision.range && pacer.rate() == 120) {
                    epoch = now + phase;
                    frames.resize(count);
                    for (int s = 0; s < 8; ++s) {
                        const double start = epoch + s;
                        for (int j = 0; j < 57; ++j)
                            frames.push_back(start + (j + 0.5) / 57.0);
                        const int early = s < 2 ? 0 : std::min(3, s - 1);
                        for (int i = 0; i < early; ++i)
                            frames.push_back(start + 0.0001 + 0.0007 * i);
                        for (int i = 0; i < 3 - early; ++i)
                            frames.push_back(start + 0.9999 - 0.0007 * i);
                    }
                    std::sort(frames.begin() + static_cast<std::ptrdiff_t>(count), frames.end());
                }
                k += pacer.rate() == 120 ? 1 : 2;
            }
            require(epoch > 0.0, "the guest never reached the 120 step");
            require(low > 0.0, "the step never fell");
            if (low - epoch > 2.1)
                std::fprintf(stderr, "tick %.9f phase %.3f: the 60 step after %.3f s\n",
                    highTick, phase, low - epoch);
            require(low - epoch <= 2.1, "the fall took longer than the dwell and a window");
        }
    }
}

/* A guest whose bursts of two or more frames, each within a millisecond, come
 * 31 or more times a second keeps the step it has: a tick counts two of a burst
 * at the 60 step's ticks and at the 120 step's, so the stream that rises at the
 * 60 step is not below the fall's measure at the 120 step. A count that took
 * one of a burst at the 120 step's ticks and two at the 60 step's changed the
 * step every four seconds. */
void aGuestWhoseBurstsComeThirtyOneOrMoreTimesASecondKeepsTheHighStep()
{
    for (const int burst : {2, 3, 4, 6}) {
        for (const double perSecond : {31.5, 35.0, 40.0, 45.0, 50.0, 55.0, 60.0}) {
            std::vector<double> frames;
            const double period = 1.0 / perSecond;
            for (int n = 0; n * period < 30.0; ++n)
                for (int k = 0; k < burst; ++k)
                    frames.push_back(1.0 + n * period + 0.0003 * k);
            const auto changes = stepChangesOf(frames, 31.0);
            if (!changes.empty())
                std::fprintf(stderr,
                    "bursts of %d, %.1f a second: %zu changes, the first at %.2f s\n", burst,
                    perSecond, changes.size(), changes[0].at);
            require(changes.empty(), "a steady stream of bursts changed the step");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    /* With a row's name as the argument, that row alone. */
    struct Row {
        const char* name;
        void (*run)();
    };
    const Row rows[] = {
        {"aLinkThatWakesAsksForTheFastestRateTheCeilingAllows",
            aLinkThatWakesAsksForTheFastestRateTheCeilingAllows},
        {"aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink",
            aSixtyHertzGuestSettlesAtSixtyAndKeepsNoFasterLink},
        {"aOneHundredTwentyHertzGuestKeepsTheFastLink",
            aOneHundredTwentyHertzGuestKeepsTheFastLink},
        {"aGuestThatRendersFasterMakesTheLinkRiseAtOnce",
            aGuestThatRendersFasterMakesTheLinkRiseAtOnce},
        {"aGuestThatSlowsDownMakesTheLinkFallAfterASustainedStretch",
            aGuestThatSlowsDownMakesTheLinkFallAfterASustainedStretch},
        {"aSteadyGuestIsNeverShownFewerFramesThanTheSettingOffPathBeyondTwo",
            aSteadyGuestIsNeverShownFewerFramesThanTheSettingOffPathBeyondTwo},
        {"aGuestThatSpeedsUpToSixtyThreeOrMoreReachesTheHighStepWithinTwoSecondsAndATick",
            aGuestThatSpeedsUpToSixtyThreeOrMoreReachesTheHighStepWithinTwoSecondsAndATick},
        {"aSteadySpeedUpChangesTheRateOnce", aSteadySpeedUpChangesTheRateOnce},
        {"aGuestThatAlternatesAcrossTheThresholdChangesTheRateAtMostOncePerHalfCycle",
            aGuestThatAlternatesAcrossTheThresholdChangesTheRateAtMostOncePerHalfCycle},
        {"aGuestThatAlternatesAboveTheThresholdOnAverageStaysOnTheHighStep",
            aGuestThatAlternatesAboveTheThresholdOnAverageStaysOnTheHighStep},
        {"aSteadyGuestMakesNoLinkFlip", aSteadyGuestMakesNoLinkFlip},
        {"aGuestWhoseFramesArriveInGroupsAlternatesAtMostOncePerHalfCycle",
            aGuestWhoseFramesArriveInGroupsAlternatesAtMostOncePerHalfCycle},
        {"aGuestWhoseFramesAreEarlyAndLateAlternatesAtMostOncePerHalfCycle",
            aGuestWhoseFramesAreEarlyAndLateAlternatesAtMostOncePerHalfCycle},
        {"aSteadyGuestOfFiftyNineToSixtyThreeChangesTheStepAtMostOnceAfterSettling",
            aSteadyGuestOfFiftyNineToSixtyThreeChangesTheStepAtMostOnceAfterSettling},
        {"aSteadyGuestOfFiftyNineToSixtyThreeWithIntervalJitterChangesTheStepAtMostOnce",
            aSteadyGuestOfFiftyNineToSixtyThreeWithIntervalJitterChangesTheStepAtMostOnce},
        {"aSteadyGuestWithFourMillisecondsOfIntervalJitterNeverChangesTheStepTwiceInTheDwell",
            aSteadyGuestWithFourMillisecondsOfIntervalJitterNeverChangesTheStepTwiceInTheDwell},
        {"aStepChangeWaitsForTheDwellSinceTheLastOne",
            aStepChangeWaitsForTheDwellSinceTheLastOne},
        {"aDrawThatFindsNoDrawableIsRetriedUntilTheFrameIsShown",
            aDrawThatFindsNoDrawableIsRetriedUntilTheFrameIsShown},
        {"aDrawThatNeverFindsADrawableIsRetriedAtMostThirtyTimes",
            aDrawThatNeverFindsADrawableIsRetriedAtMostThirtyTimes},
        {"aDrawWhoseDrawableWaitRanOutTheTimeoutIsNotDrawnAgain",
            aDrawWhoseDrawableWaitRanOutTheTimeoutIsNotDrawnAgain},
        {"aLinkMadeAgainDoesNotRenewTheDrawableWait",
            aLinkMadeAgainDoesNotRenewTheDrawableWait},
        {"theWaitsOfAFrameAreBoundedWhateverEachRequestWaits",
            theWaitsOfAFrameAreBoundedWhateverEachRequestWaits},
        {"aFrameThatArrivesBetweenTheFlagAndTheClaimHasOneBudget",
            aFrameThatArrivesBetweenTheFlagAndTheClaimHasOneBudget},
        {"aDrawableThatArrivesAfterALongWaitShowsTheFrame",
            aDrawableThatArrivesAfterALongWaitShowsTheFrame},
        {"theDrawableBudgetBelongsToTheFrameThatWasDrawn",
            theDrawableBudgetBelongsToTheFrameThatWasDrawn},
        {"aRetryIsAPresentNoFrameAskedFor", aRetryIsAPresentNoFrameAskedFor},
        {"aLinkMadeAgainWhileAFrameIsOwedStillShowsIt",
            aLinkMadeAgainWhileAFrameIsOwedStillShowsIt},
        {"aLinkMadeAgainAndAgainDoesNotRenewTheRetries",
            aLinkMadeAgainAndAgainDoesNotRenewTheRetries},
        {"theDelayedRetryOfADisplayWithNoLinkIsOneAtATime",
            theDelayedRetryOfADisplayWithNoLinkIsOneAtATime},
        {"theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime",
            theLinkSleepsWhenNoFrameHasWaitedForTheIdleTime},
        {"aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake",
            aFrameThatArrivesBeforeTheIdleTimeKeepsTheLinkAwake},
        {"aTickPresentsOnlyWhenAFrameWaits", aTickPresentsOnlyWhenAFrameWaits},
        {"theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises",
            theCeilingCapsTheLinkAndItRisesAgainWhenTheCeilingRises},
        {"aWakeForgetsTheEvidenceOfTheLastBurst",
            aWakeForgetsTheEvidenceOfTheLastBurst},
        {"aGuestWhoseExtraFramesBunchInOneTickStaysOnTheSixtyStep",
            aGuestWhoseExtraFramesBunchInOneTickStaysOnTheSixtyStep},
        {"aGuestWhoseFramesAreSpreadOverTicksRisesAtTheDwell",
            aGuestWhoseFramesAreSpreadOverTicksRisesAtTheDwell},
        {"aGuestThatSpeedsUpWithBunchedFramesIsFollowedWithinTheDwellAndTheWindow",
            aGuestThatSpeedsUpWithBunchedFramesIsFollowedWithinTheDwellAndTheWindow},
        {"aSlowLinkCountsTheFramesOfTheTimeEachTickHolds",
            aSlowLinkCountsTheFramesOfTheTimeEachTickHolds},
        {"aGuestThatFallsToSixtyWithBunchedFramesIsFollowedWithinTheDwellAndTheWindow",
            aGuestThatFallsToSixtyWithBunchedFramesIsFollowedWithinTheDwellAndTheWindow},
        {"aGuestWhoseBurstsComeThirtyOneOrMoreTimesASecondKeepsTheHighStep",
            aGuestWhoseBurstsComeThirtyOneOrMoreTimesASecondKeepsTheHighStep},
    };
    try {
        for (const Row& row : rows) {
            if (argc > 1 && std::string_view(argv[1]) != row.name)
                continue;
            row.run();
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "present pacer test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
