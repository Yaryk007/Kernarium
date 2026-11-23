// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::gpu {

/* The slowest screen that can show the 120 step: a ceiling at or above it asks
 * for 120 (iOS gives a 100 Hz screen its 100), and one below it can ask for no
 * more than 60, so a screen that shows 61 to 99 would be given a 120 Hz mode
 * that it cannot pace. The 120 Hz display is granted only to a screen at or
 * above it, and the application offers the setting only for such a screen. */
inline constexpr double highStepMinimumCeiling = 100.0;

/* What a display link asks the system for, in frames per second: the
 * minimum, the maximum and the preferred rate of a CAFrameRateRange. */
struct LinkFrameRate {
    float minimum {};
    float maximum {};
    float preferred {};

    [[nodiscard]] bool operator==(const LinkFrameRate&) const = default;
};

/* What the draws of one frame may still wait for a drawable. The budget
 * belongs to the frame a draw claims, which is the newest one at the moment of
 * its claim (a frame that arrives after the tick took its flag, and before the
 * claim, is the frame drawn): `frame` is the frame the waits so far were for,
 * `left` what is left of its budget and `whole` the budget of a frame that no
 * draw has tried. The draw asks the layer for a drawable only while the
 * allowance for the frame it claimed is above zero. */
struct DrawableBudget {
    std::uint64_t frame {};
    double left {};
    double whole {};

    [[nodiscard]] double allowanceFor(std::uint64_t claimed) const noexcept
    {
        return claimed == frame ? left : whole;
    }

    /* A draw that nobody paces asks as it always did. */
    [[nodiscard]] static constexpr DrawableBudget unlimited() noexcept
    {
        return {0, 0.0, 1e9};
    }
};

/* The decisions of the display link that paces the presents of a 120 Hz
 * display (native_display_ios.mm): what the link asks iOS for, whether a tick
 * presents, and when the link sleeps. It is a state machine with no clock and
 * no platform call: the caller gives the time (seconds, monotonic) and the
 * guest's frame counter, and does what the answer says.
 *
 * The link follows the rate the guest renders at, in two steps, 120 and 60
 * frames a second: a guest that renders at 60 on a 120 Hz mode, which is what
 * Android does for most content, would keep the display at 120 for ticks that
 * carry no frame, so the link asks for 60 for it. A tick shows only the newest
 * frame, so a link on the 60 step shows at most 60: a guest above it (the
 * setting off shows every frame up to the 60 of its mode) rises to the 120
 * step, judged over a second. The 120 step is left only when the rise measure
 * no longer supports it and a fall measure over 2 s, which lags it, is below
 * it too: a guest that alternates across the threshold for less than that
 * stays on the 120 step. A step changes no sooner than the dwell (2 s) after
 * the last change since the link woke (a wake forgets it, and a ceiling that
 * forbids the step lowers it at once), whatever the measures say, so that
 * frames that arrive early and late (which move the count of a second by two
 * between ticks, over a gap of 1.5 frames a second between a rise at 62.5 and
 * a hold under 61) or a rate that wobbles about a threshold change the step
 * at most once in the dwell. Once the link has settled a steady guest of 60 or
 * less, or 63 or more, makes no change; one between them never changes it
 * twice within the dwell, and changes it at most once when its frames are
 * early or late by up to 4 ms. With 4 ms of jitter in the interval between its
 * frames it can change it up to five times in 12 s, at rates from about 60.05
 * to 62.05. The time that a rule states (the second of a rise, the 2 s of a
 * fall and of the dwell) is a time plus one tick: the link decides at its
 * ticks. A link that wakes asks for the fastest rate the ceiling allows,
 * because the first frame of a burst should wait for the shortest tick, and
 * settles after the first second of the burst. The ceiling is the fastest rate
 * the host allows now (the mode's rate, or less when the screen can show less,
 * as in Low Power Mode): the 120 step is for a ceiling of 100 or more, and any
 * lower ceiling, a mode of 47 Hz included, is the 60 step.
 *
 * The rate is judged on the frames the display could show, not on the frames
 * the guest offered: a tick counts no more of the frames that arrived since the
 * tick before than its time holds at 120 Hz (`Parameters::
 * displayFramesPerSecond`), and two at least (`Parameters::
 * framesCountedPerTick`), the same at the 60 step's ticks as at the 120
 * step's. A burst that arrives within one on-time tick counts two at most; one
 * that straddles two ticks, or arrives at a late tick, counts as many as the
 * time of that tick or those ticks holds at 120 Hz. A guest at 60 with 20
 * frames at once at one point of each second, or at 63 with 3 to 12 of them
 * bunched at one point of each second, counts 62 or fewer: it does not take
 * the 120 step from the 60 step, and keeps the 120 step it has (after a wake)
 * while its count is 61 or more. A guest whose bursts come at different points
 * of the seconds, so that a window of a second holds two of them, counts more
 * and takes the 120 step. A finer tick never counts a stream less than a
 * coarser one, so a stream that takes the 120 step is not below the fall's
 * measure on it: a guest whose bursts of two or more frames, each within one
 * tick, come 31 or more times a second takes the 120 step and keeps it.
 * The time from a change of the guest's rate to the link following it is the
 * later of the dwell since the last change (2 s) and the window after the
 * change (1 s for a rise, 2 s for a fall), plus one tick, for any guest whose
 * counted frames pass a threshold in every window of a second, however they
 * bunch; just over the rise threshold the rise can come later (at 63 counted
 * a second by up to half a second, at 64 by about a tenth, at 65 or more by
 * none). A guest whose counted frames stay a frame under a threshold (63 a
 * second with three of them bunched at one point counts 62) holds the old step
 * for as long as they are under, and one that hovers about a threshold changes
 * the step at most once in the dwell.
 *
 * The link sleeps when no frame has been waiting for idleSeconds. Whoever
 * runs the link keeps one flag, whether a frame waits, and clears it when a
 * tick presents. A tick that presents draws, and tells the pacer when the
 * drawable was not there (`drew`, with the frame the draw claimed): the frame
 * is then drawn again on the next ticks, up to maximumDrawRetries of them and
 * until its draws have waited maximumDrawableWaitSeconds for a drawable, so
 * that a transient failure does not leave the newest frame unshown while the
 * link sleeps, and a stalled one does not hold the thread that draws. The
 * draw asks for a drawable once, and only while the frame has budget left
 * (`drawableBudget`), so the waits of a frame add up to under the budget plus
 * one wait of the layer (a second): under 1.9 s, whatever the draws and the
 * flags that ask for them. */
class PresentPacer final {
public:
    struct Parameters {
        /* How long without a frame before the link sleeps. */
        double idleSeconds {0.1};
        /* The windows a rise and a fall are judged over, and the least span
         * of samples that says a rate: a window the link has not lived yet
         * (it woke a moment ago) waits for it. */
        double riseWindowSeconds {1.0};
        double riseMinimumSpanSeconds {0.5};
        double fallWindowSeconds {2.0};
        double fallMinimumSpanSeconds {1.0};
        /* A rate in frames a second: at or above this the guest renders
         * faster than the 60 step shows, and the link rises to the 120 step.
         * It stays there while the rise measure holds this ... */
        double riseToHigh {62.5};
        /* ... and falls from it when that measure does not and the fall
         * measure is below this. */
        double fallFromHigh {61.0};
        /* The frames a second the display can show at most: the 120 step. A
         * tick counts no more of the frames that arrived since the one before
         * it than its time holds at this rate, and no fewer than
         * `framesCountedPerTick` of them (see `remember`). */
        double displayFramesPerSecond {120.0};
        unsigned framesCountedPerTick {2};
        /* The least time between two step changes. */
        double minimumDwellSeconds {2.0};
        /* How many times a frame is drawn again after a draw that found no
         * drawable, at the next ticks: 30 is a quarter of a second at the
         * 120 step and half a second at the 60 step. */
        unsigned maximumDrawRetries {30};
        /* How long the draws of one frame may have waited for a drawable, in
         * all, before no draw asks again: a layer waits a second for a
         * drawable that the display holds, so a draw that waited it out ends
         * the retries, where 30 of them would hold the thread that draws for
         * half a minute. Only the wait for the drawable counts, and a draw
         * asks only while less than this has been waited, so the last wait
         * may end past it by a layer's own second at most. */
        double maximumDrawableWaitSeconds {0.9};
    };

    /* What a tick is to do. */
    struct Decision {
        /* Present the newest frame. */
        bool present {};
        /* Pause the link: nothing has waited for idleSeconds. */
        bool sleep {};
        /* The range to ask for from now on, when it changed. */
        std::optional<LinkFrameRate> range;
    };

    explicit PresentPacer(Parameters parameters);
    PresentPacer() : PresentPacer(Parameters {}) {}

    /* The link wakes from sleep (or starts) at `now`, with `guestFrames`
     * frames offered so far: forgets the evidence of the last burst, keeps a
     * retry that is owed, and returns the range to ask for, the fastest the
     * ceiling allows. */
    [[nodiscard]] LinkFrameRate wake(
        double now, std::uint64_t guestFrames, double ceiling) noexcept;

    /* A tick at `now`. `guestFrames` is the count of frames the guest has
     * offered (never decreasing) and `frameWaiting` whether one has not been
     * presented. */
    [[nodiscard]] Decision tick(
        double now,
        std::uint64_t guestFrames,
        bool frameWaiting,
        double ceiling) noexcept;

    /* A draw has been made (a tick that presented, or the display's own draw
     * when it has no link) for `frame`, the frame it claimed: `unavailable`
     * says it found no drawable or render pass, which owes the frame another
     * draw at the next ticks, up to the limits (the draws, and the time they
     * waited for a drawable: `drawableWaitSeconds`, the acquisition's own
     * time); a draw that shows the frame ends them. The limits are the
     * frame's: a draw of another frame than the last starts them again. */
    void drew(bool unavailable, double drawableWaitSeconds = 0.0, std::uint64_t frame = 0) noexcept;

    /* Whether the newest frame is owed another draw. The debt, and what is
     * left of the limit, outlive a wake: a link made again (the scene it was
     * on went) goes on with the frame that was not shown, and only a draw of
     * another frame renews the limit. */
    [[nodiscard]] bool retryOwed() const noexcept;

    /* What the next draw may wait for a drawable (`DrawableBudget`). */
    [[nodiscard]] DrawableBudget drawableBudget() const noexcept;

    /* For a display with no link to tick, which draws a frame itself and
     * asks again after a delay: whether the caller is to arrange that delay
     * now, which is when a retry is owed and none is arranged already.
     * There is at most one, whatever frames arrive; `retryDelayRan` says it
     * ran. */
    [[nodiscard]] bool arrangeRetry() noexcept;
    void retryDelayRan() noexcept;

    /* The rate the link now asks for, 120 or 60. */
    [[nodiscard]] unsigned rate() const noexcept;

    [[nodiscard]] static LinkFrameRate rangeFor(unsigned rate) noexcept;

private:
    struct Sample {
        double time {};
        std::uint64_t frames {};
    };
    /* The newest samples, oldest first: a tick at 120 Hz over the longest
     * window (2 s) needs 240; the rest is room for a link that ticks
     * faster. */
    static constexpr std::size_t capacity = 512;

    [[nodiscard]] static unsigned rateForCeiling(double ceiling) noexcept;
    void remember(double now, std::uint64_t guestFrames) noexcept;
    /* Frames a second over the samples of the last `window` seconds: the
     * frames counted over the span they cover, when it is at least
     * `minimumSpan`. */
    [[nodiscard]] std::optional<double> framesPerSecond(
        double now, double window, double minimumSpan) const noexcept;

    Parameters parameters_;
    unsigned rate_ {60};
    /* The counter and time of the sample before, and the frames counted so
     * far: what the samples hold in place of the guest's own counter. */
    std::uint64_t lastGuestFrames_ {};
    double lastTime_ {};
    std::uint64_t counted_ {};
    double lastPresent_ {};
    /* A draw owed to the newest frame, and how many have been tried since a
     * frame last arrived. */
    bool retryOwed_ {};
    unsigned retries_ {};
    double drawableWait_ {};
    /* The frame the retries and the waits count for. */
    std::uint64_t budgetFrame_ {};
    bool retryArranged_ {};
    /* When the step last changed since the last wake. */
    std::optional<double> changedAt_;
    std::array<Sample, capacity> samples_ {};
    std::size_t first_ {};
    std::size_t count_ {};
};

} // namespace uml::gpu
