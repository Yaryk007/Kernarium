// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRestartLoop.hpp"
#include "camera/background_video.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <type_traits>
#include <utility>

namespace kernarium::gpu_hold {

/*
 * What the application's hold of the guest's GPU requests does
 * (kernarium_runtime_set_gpu_hold), apart from the globals it works on: the
 * GPU service that a guest registers, the mutex that guards it, and the run
 * loop's Lifecycle belong to the runtime, which hands them in. The unit tests
 * hand in their own, so the sequence runs as the runtime runs it.
 */

/* The values of `held`. A value of 3 or more is the hold of a caller that has a
 * deadline: the milliseconds its deadline leaves. */
inline constexpr int released = 0;
inline constexpr int withCamera = 1;
inline constexpr int atOnce = 2;

struct Timing final {
    /* The longest the gate waits for a GPU request that is running. */
    std::chrono::milliseconds drain {1000};
    /* The longest a hold waits for the guest to stop its camera video. */
    std::chrono::nanoseconds camera {uml::camera::backgroundVideoBound};
};

/* How long a hold of `held` waits for the guest's camera video: the camera's
 * own bound, or, for a caller whose deadline leaves `held` milliseconds, what
 * is left of them after the second the gate's own wait for a running request
 * may take. None when nothing is left. The deadline limits this wait and not
 * that one: a hold always has its drain's second, so a deadline that leaves
 * less than a second is passed by the drain. */
[[nodiscard]] constexpr std::chrono::nanoseconds cameraWait(
    int held, const Timing& timing) noexcept
{
    if (held < 3)
        return timing.camera;
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::milliseconds(held) - timing.drain);
    return std::clamp(left, std::chrono::nanoseconds::zero(), timing.camera);
}

/* The hold on the guest's GPU service. `Gpu` is the in-process GPU service
 * (`holdRequests`, `releaseRequests`); `mutex` guards `gpu`, which is null when
 * no guest runs. */
template <class Gpu>
class Gate final {
public:
    Gate(std::mutex& mutex, Gpu*& gpu, restart::Lifecycle& lifecycle,
         Timing timing)
        : mutex_(mutex)
        , gpu_(gpu)
        , lifecycle_(lifecycle)
        , timing_(timing)
    {
    }

    /* Holds the guest's GPU requests: 1 when it is held, 0 when a request that
     * was running outlasted the drain, -1 when no guest runs (the hold then
     * waits for the next one). A gate that is held already was closed by a
     * hold that waited for its requests under this mutex, which this call
     * waited for: it does not wait for them again, so two holds that meet
     * cost the drain once, and it answers what that drain found, until the
     * requests are given back. */
    int close()
    {
        std::lock_guard lock(mutex_);
        lifecycle_.setGpuHoldRequested(true);
        if (gpu_ == nullptr)
            return -1;
        if (gpu_->requestsHeld())
            return timedOut_ == gpu_ ? 0 : 1;
        const bool drained = gpu_->holdRequests(timing_.drain);
        timedOut_ = drained ? nullptr : gpu_;
        return drained ? 1 : 0;
    }

    int release()
    {
        std::lock_guard lock(mutex_);
        lifecycle_.setGpuHoldRequested(false);
        timedOut_ = nullptr;
        if (gpu_ != nullptr)
            gpu_->releaseRequests();
        /* A restarted guest waits for this. */
        lifecycle_.changed();
        return gpu_ != nullptr ? 1 : -1;
    }

    /* `video` is the camera's way to end the guest's video: a
     * `uml::camera::BackgroundVideo*` (null for none), or a null pointer
     * constant for a guest with no camera. `log` takes what the camera's wait
     * has to say. */
    template <class Video, class Log>
    int set(int held, Video video, Log&& log)
    {
        static_cast<void>(log);
        if (held == released)
            return release();
        if (held == atOnce) {
            /* The bound's hold, when the edge is already late: no wait for the
             * camera, and a hold that waits for it meanwhile is of no use once
             * this has landed, and ends. */
            const int result = close();
            if constexpr (!std::is_null_pointer_v<Video>) {
                if (video != nullptr)
                    video->abandon();
            }
            return result;
        }
        /* The application has asked: the next boot waits from here on, though
         * the gate closes only after the camera's wait. */
        lifecycle_.setGpuHoldRequested(true);
        if constexpr (std::is_null_pointer_v<Video>) {
            return close();
        } else {
            const auto wait = cameraWait(held, timing_);
            if (video == nullptr || wait <= std::chrono::nanoseconds::zero())
                return close();
            return uml::camera::holdAfterVideoEnds(
                       *video, [this] { return close(); }, wait,
                       std::forward<Log>(log))
                .held;
        }
    }

private:
    std::mutex& mutex_;
    Gpu*& gpu_;
    restart::Lifecycle& lifecycle_;
    const Timing timing_;
    /* The service whose drain a request outlasted, while its gate stays held:
     * what a later hold of it answers. Guarded by `mutex_`. */
    const Gpu* timedOut_ {};
};

} // namespace kernarium::gpu_hold
