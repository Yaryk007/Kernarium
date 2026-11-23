// SPDX-License-Identifier: GPL-2.0-or-later

#include "present_pacer.hpp"

namespace uml::gpu {

PresentPacer::PresentPacer(Parameters parameters)
    : parameters_(parameters)
{
}

unsigned PresentPacer::rateForCeiling(double ceiling) noexcept
{
    /* Any ceiling under the 120 step's is the 60 step: a mode that states 47
     * (a size too large for even a 60 Hz timing) is shown whole by it. */
    return ceiling >= highStepMinimumCeiling ? 120 : 60;
}

LinkFrameRate PresentPacer::rangeFor(unsigned rate) noexcept
{
    /* The top step asks for a range that cannot be met by 60, so that iOS
     * gives a ProMotion screen its fastest rate (Apple's guidance for
     * content that wants it); the 60 step states one rate. */
    if (rate == 120)
        return {80.0F, 120.0F, 120.0F};
    return {60.0F, 60.0F, 60.0F};
}

unsigned PresentPacer::rate() const noexcept
{
    return rate_;
}

LinkFrameRate PresentPacer::wake(
    double now, std::uint64_t guestFrames, double ceiling) noexcept
{
    first_ = 0;
    count_ = 0;
    /* The evidence and the last change are those of the burst before. */
    changedAt_.reset();
    remember(now, guestFrames);
    rate_ = rateForCeiling(ceiling);
    lastPresent_ = now;
    return rangeFor(rate_);
}

void PresentPacer::drew(
    bool unavailable, double drawableWaitSeconds, std::uint64_t frame) noexcept
{
    if (!unavailable) {
        retryOwed_ = false;
        retries_ = 0;
        drawableWait_ = 0.0;
        budgetFrame_ = 0;
        return;
    }
    /* The limits belong to the frame the draw claimed: a draw of another
     * frame starts them again, and a flag that a frame's own arrival left
     * set does not. */
    if (frame != budgetFrame_) {
        budgetFrame_ = frame;
        retries_ = 0;
        drawableWait_ = 0.0;
    }
    if (drawableWaitSeconds > 0.0)
        drawableWait_ += drawableWaitSeconds;
    /* A frame is drawn again while its retries and the waits of its draws for
     * a drawable are within their limits. */
    retryOwed_ = retries_ < parameters_.maximumDrawRetries &&
        drawableWait_ < parameters_.maximumDrawableWaitSeconds;
    if (retryOwed_)
        ++retries_;
}

bool PresentPacer::retryOwed() const noexcept
{
    return retryOwed_;
}

DrawableBudget PresentPacer::drawableBudget() const noexcept
{
    const double left = parameters_.maximumDrawableWaitSeconds - drawableWait_;
    return {budgetFrame_, left > 0.0 ? left : 0.0, parameters_.maximumDrawableWaitSeconds};
}

bool PresentPacer::arrangeRetry() noexcept
{
    if (!retryOwed_ || retryArranged_)
        return false;
    retryArranged_ = true;
    return true;
}

void PresentPacer::retryDelayRan() noexcept
{
    retryArranged_ = false;
}

void PresentPacer::remember(double now, std::uint64_t guestFrames) noexcept
{
    if (count_ == 0) {
        counted_ = 0;
    } else if (guestFrames > lastGuestFrames_) {
        /* A tick counts no more of the frames that arrived since the tick
         * before than the display could have shown in that time, and two at
         * least: the same two at the 60 step's ticks and at the 120 step's,
         * so a finer tick never counts a stream less than a coarser one and
         * a stream that takes the 120 step is not below the fall's measure
         * on it. However a guest bunches its frames, the count of a stretch
         * of ticks moves by at most this for each of them, so a rate judged
         * on it does not depend on how many frames come at once. */
        const double slots = (now - lastTime_) * parameters_.displayFramesPerSecond + 0.75;
        const double least = static_cast<double>(parameters_.framesCountedPerTick);
        const std::uint64_t most = slots > least ? static_cast<std::uint64_t>(slots)
                                                 : parameters_.framesCountedPerTick;
        const std::uint64_t arrived = guestFrames - lastGuestFrames_;
        counted_ += arrived < most ? arrived : most;
    }
    lastGuestFrames_ = guestFrames;
    lastTime_ = now;
    if (count_ == capacity) {
        first_ = (first_ + 1U) % capacity;
        --count_;
    }
    samples_[(first_ + count_) % capacity] = {now, counted_};
    ++count_;
}

std::optional<double> PresentPacer::framesPerSecond(
    double now, double window, double minimumSpan) const noexcept
{
    if (count_ < 2)
        return std::nullopt;
    const Sample newest = samples_[(first_ + count_ - 1U) % capacity];
    std::size_t oldest = count_ - 1U;
    while (oldest > 0) {
        const Sample& earlier = samples_[(first_ + oldest - 1U) % capacity];
        if (earlier.time < now - window)
            break;
        --oldest;
    }
    if (oldest == count_ - 1U)
        return std::nullopt;
    const Sample start = samples_[(first_ + oldest) % capacity];
    const double span = newest.time - start.time;
    /* Fewer samples than the minimum cannot say a rate: a frame more or less
     * is 1 / span frames a second. */
    if (span < minimumSpan || span <= 0.0)
        return std::nullopt;
    return static_cast<double>(newest.frames - start.frames) / span;
}

PresentPacer::Decision PresentPacer::tick(
    double now,
    std::uint64_t guestFrames,
    bool frameWaiting,
    double ceiling) noexcept
{
    remember(now, guestFrames);
    Decision decision;
    unsigned next = rate_;
    /* The rise measure decides the step, and the fall test runs only where
     * the rise measure no longer supports it: a fall measure over 2 s lags the
     * one over a second, and would otherwise take the link down as it rises,
     * tick after tick. The step is supported down to the fall threshold, which
     * is under the rise one by more than the measure's frame. */
    const auto rise = framesPerSecond(
        now, parameters_.riseWindowSeconds, parameters_.riseMinimumSpanSeconds);
    if (rise && *rise >= parameters_.riseToHigh) {
        next = 120;
    } else if (rate_ == 120 && (!rise || *rise < parameters_.fallFromHigh)) {
        if (const auto fall = framesPerSecond(
                now, parameters_.fallWindowSeconds, parameters_.fallMinimumSpanSeconds)) {
            if (*fall < parameters_.fallFromHigh)
                next = 60;
        }
    }
    /* A step change waits until the dwell has passed since the last one,
     * whatever the measures say: a guest whose rate wobbles about a
     * threshold changes the step at most once in the dwell. The ceiling is
     * not a measure: a step it forbids is left at once. */
    const unsigned cap = rateForCeiling(ceiling);
    if (next != rate_ && next <= cap) {
        if (changedAt_ && now - *changedAt_ < parameters_.minimumDwellSeconds)
            next = rate_;
        else
            changedAt_ = now;
    }
    if (next > cap)
        next = cap;
    if (next != rate_) {
        rate_ = next;
        decision.range = rangeFor(next);
    }
    if (frameWaiting || retryOwed_) {
        decision.present = true;
        lastPresent_ = now;
        retryOwed_ = false;
    } else if (now - lastPresent_ >= parameters_.idleSeconds) {
        decision.sleep = true;
    }
    return decision;
}

} // namespace uml::gpu
