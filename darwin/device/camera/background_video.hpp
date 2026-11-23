// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace uml::camera {

/* The application's way to a camera device when it leaves the foreground, from a thread of its own.
 *
 * Leaving the foreground ends the guest's camera video (`VirtioCameraDevice`'s lost video), and the application holds the
 * guest's GPU requests a moment later. A guest whose camera pipeline has not acted on the lost video when the hold lands
 * stalls in its next GPU request until the application is back, and a recording that runs then stops only at the return.
 * `end` has the device end the video now, as the platform's notice of the departure would, and waits until the guest has
 * stopped the video streams it was running (the guest's camera layer stops them when it has seen the loss), so that the
 * application can hold the GPU afterwards.
 *
 * The control is given to the device through `CameraDeviceOptions::background`, and outlives it. A device attaches while
 * it exists; `end` with none attached ends nothing and waits for nothing. Every method is safe to call from any thread. */
class BackgroundVideo final {
public:
    using Clock = std::chrono::steady_clock;

    struct Outcome {
        /* The video streams of the guest that were running and that the guest has to stop (every stream of a camera whose
         * video has just ended, and of one whose video ended a moment ago and is still being acted on). None: no video runs,
         * and nothing was waited for. */
        std::uint32_t streams {};
        /* The guest stopped all of them (also true with none, and when the device went away meanwhile). False: the deadline
         * passed first. */
        bool stopped {true};
        /* The device took the request: false when it did not within the deadline (the video may be ended late, and `streams`
         * is not known). */
        bool answered {true};
        /* The wait was cut short by `abandon`: the application's GPU requests were held meanwhile (by a caller that had
         * no time to wait), so the guest cannot act on the lost video any more. `stopped` is then not known. */
        bool abandoned {};
    };

    /* Ends the guest's running video and waits until the guest has stopped its streams, or until `deadline`: one deadline
     * for the device's answer and the wait together, so the call never takes longer than it. Calls serialize, and the wait
     * for an earlier call is under the same deadline: a call that is still queued behind another at its deadline returns
     * unanswered (`answered` false), and the caller holds as it would after any wait that ran out. */
    [[nodiscard]] Outcome end(Clock::time_point deadline);

    /* Ends the wait of a call of `end` that is in progress, as the application's other way to hold the GPU does when it lands
     * first (the hold that the edge's bound takes when the edge is already late): the guest cannot act on the video once its
     * GPU requests are held, so waiting for it is of no use. The call returns at once with `abandoned` set. With no call in
     * progress it does nothing: a call that comes later waits as any does (each call counts the abandons from its own
     * start). Any thread. */
    void abandon();

    /* The device's side. */

    /* The device exists and `wake` makes its worker run `take` (a call that does nothing but signal the worker's event
     * descriptor); `detach` is the device going away, which lets a waiting `end` return. */
    void attach(std::function<void()> wake);
    void detach() noexcept;

    /* On the worker: whether a request waits, and the number of the request, which `ended` answers. Cheap when none does. */
    [[nodiscard]] bool take(std::uint64_t& request);

    /* On the worker: the request is handled, and `streams` video streams of the guest are left to stop. */
    void ended(std::uint64_t request, std::uint32_t streams);

    /* On the worker: the streams the guest has yet to stop are now `streams` (a stop of one, or a reset, brings it down). */
    void remaining(std::uint32_t streams);

private:
    std::timed_mutex callMutex_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::function<void()> wake_;
    bool attached_ {};
    std::atomic<bool> requested_ {};
    std::uint64_t asked_ {};
    std::uint64_t abandoned_ {};
    std::uint64_t answered_ {};
    std::uint32_t streams_ {};
    std::uint32_t remaining_ {};
};

/* How long the application's departure waits for the guest to stop its video (`BackgroundVideo::end`). It is part of the
 * application's edge budget (the time its leaving the foreground may take), never added to it. */
inline constexpr std::chrono::milliseconds backgroundVideoBound {2000};

/* What the application does when it leaves the foreground, before it holds the guest's GPU requests: `video.end` ends the
 * guest's running camera video and waits for the guest to stop it, within `bound`, and then `hold` runs, whatever the wait
 * found. A wait that ran out is told once to `log` (the message, without a prefix); one that was abandoned is not, and the
 * hold that follows it holds what is held already. `Video` is a `BackgroundVideo`. */
template <class Hold> struct HeldAfterVideo {
    BackgroundVideo::Outcome video;
    std::invoke_result_t<Hold&> held;
};

template <class Video, class Hold, class Log>
[[nodiscard]] HeldAfterVideo<Hold> holdAfterVideoEnds(Video& video, Hold&& hold, std::chrono::nanoseconds bound, Log&& log)
{
    const auto outcome = video.end(BackgroundVideo::Clock::now() + bound);
    if (outcome.abandoned) {
        // Another caller held the GPU first: nothing to tell.
    } else if (!outcome.answered)
        log("the camera did not answer the end of the guest's video within " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(bound).count()) + " ms");
    else if (!outcome.stopped)
        log("the guest did not stop " + std::to_string(outcome.streams) + " video stream(s) within " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(bound).count()) + " ms");
    return {outcome, hold()};
}

} // namespace uml::camera
