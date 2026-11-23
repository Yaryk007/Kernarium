// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace uml::audio {

/* An engine that is stale, or whose output node reports no format, is not
 * started as it is. */
[[nodiscard]] constexpr bool mustReplaceEngine(bool stale, bool routeUp) noexcept
{
    return stale || !routeUp;
}

/* Whether the host output's engine may be started as it is. An AVAudioEngine
 * is bound to the output route it was made on. While the app is in the
 * background with its session inactive, iOS takes that route away: the
 * engine's output node then reports no format, starting it fails for the rest
 * of the run, and nothing done to that engine (reconnecting it, preparing it
 * again, taking the session again) brings it back. Only an engine made after
 * the route is there again works, so an engine that was caught by the loss is
 * replaced, never started.
 *
 * Releasing a stream and taking the session back is routine in the
 * foreground, and does not harm the engine, so it does not replace it; the
 * evidence does: the output node has no format, an engine configuration change
 * arrived while the sink held no session, or a start failed. */
class EngineHealth final {
public:
    /* The engine is replaced before it is started. `routeUp` is whether its
     * output node reports a format. */
    [[nodiscard]] bool mustReplace(bool routeUp) const noexcept
    {
        return mustReplaceEngine(stale_, routeUp);
    }

    [[nodiscard]] bool stale() const noexcept { return stale_; }

    void invalidate() noexcept { stale_ = true; }

    /* A fresh engine took the place of the old one. */
    void replaced() noexcept { stale_ = false; }

    /* The engine, or the audio route under it, changed. `fromEngine` is true
     * for the engine's own configuration-change notification and false for a
     * route change of the session; `sessionHeld` is whether the sink held the
     * session when it arrived. An engine's notification without the session
     * means the route went with it, and an output node without a format
     * means the route is gone, whoever said so. */
    void configurationChanged(
        bool fromEngine, bool sessionHeld, bool routeUp) noexcept
    {
        if (!routeUp || (fromEngine && !sessionHeld))
            stale_ = true;
    }

private:
    bool stale_ {};
};

/* How long a sink whose output cannot start yet keeps asking, and how often.
 * The route comes back some seconds after the app returns, and nothing
 * announces it, so a sink that starts into a missing route plays muted (the
 * stream keeps its clock) and starts the output again with a fresh engine every
 * `routeRetryInterval`, on the poll that already asks whether another app let
 * go of the session. The same wait covers a session the system would not
 * activate at a start that is not an interruption (the app is not yet active
 * when the guest resumes). After `routeWaitLimit` the output has failed and the
 * usual failure notice is given; only a later change of the route or a new
 * start asks again. */
inline constexpr std::chrono::seconds routeRetryInterval(1);
inline constexpr std::chrono::seconds routeWaitLimit(15);

/* What the sink waits for. It names the cause in its notices, so that a log
 * tells a refused activation from a missing route. */
enum class WaitCause : std::uint8_t {
    // The output node has no route.
    route,
    // The system would not activate the session.
    session,
};

/* The deadline of that wait. A wait ends when the output runs, when the stream
 * stops, is released or is configured again, and when an interruption takes
 * the stream: the interruption owns the mute then, has its own way back, and
 * the route is asked about afresh when it ends, with a full limit. */
class RouteWait final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto limit = routeWaitLimit;

    /* Starts waiting at `now`, unless a wait is under way: asking again does
     * not extend it, but says what is awaited now. */
    void begin(Clock::time_point now, WaitCause cause) noexcept
    {
        cause_ = cause;
        if (!waiting_) {
            waiting_ = true;
            since_ = now;
        }
    }

    void end() noexcept { waiting_ = false; }

    [[nodiscard]] bool waiting() const noexcept { return waiting_; }

    [[nodiscard]] WaitCause cause() const noexcept { return cause_; }

    [[nodiscard]] bool expired(Clock::time_point now) const noexcept
    {
        return waiting_ && now - since_ >= limit;
    }

private:
    bool waiting_ {};
    WaitCause cause_ {WaitCause::route};
    Clock::time_point since_ {};
};

/* How an attempt to take the session and the engine back, made on another
 * thread, ended. */
enum class ResumeResult : std::uint8_t {
    // The system would not activate the session: another app still holds it.
    sessionRefused,
    // The session is active and the output has no route.
    noRoute,
    engineFailed,
    // The engine runs.
    ready,
};

enum class HandbackAction : std::uint8_t {
    // Stay muted and ask again after the next poll.
    stayMuted,
    // Stay muted and ask again at once: what the attempt started is obsolete.
    askAgain,
    // The session is ours; wait for the route, with the deadline.
    waitForRoute,
    fail,
    play,
};

/* What the device thread does with the attempt's result. A result is obsolete
 * when an interruption began since the attempt started (the interruption owns
 * the stream again, whatever the attempt found) or, for an engine that runs,
 * when the engine's configuration or the route changed since (the engine the
 * attempt started may be gone already). */
[[nodiscard]] constexpr HandbackAction handback(
    ResumeResult result, bool interruptedSince, bool changedSince) noexcept
{
    if (interruptedSince)
        return HandbackAction::stayMuted;
    switch (result) {
    case ResumeResult::sessionRefused:
        return HandbackAction::stayMuted;
    case ResumeResult::noRoute:
        return HandbackAction::waitForRoute;
    case ResumeResult::engineFailed:
        return HandbackAction::fail;
    case ResumeResult::ready:
        return changedSince ? HandbackAction::askAgain : HandbackAction::play;
    }
    return HandbackAction::fail;
}

} // namespace uml::audio
