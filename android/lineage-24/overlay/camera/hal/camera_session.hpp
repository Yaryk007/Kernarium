// SPDX-License-Identifier: Apache-2.0
//
// What the camera HAL does when the host ends a camera's video (the application
// left the foreground while the guest had a video stream open; wire README of
// kernel/common/video), and what the framework is told. The host ends the video of
// any camera that has a video stream, and the HAL decides by the session's streams:
//
//  - a session with a stream whose consumer is a video encoder (the stream's usage
//    carries GRALLOC_USAGE_HW_VIDEO_ENCODER) is over: the HAL fails the request that
//    met the loss as a request (ERROR_REQUEST: the framework returns the buffers of
//    a request only when it was told that the request failed) and tells the
//    framework ERROR_DEVICE besides, once;
//  - any other session (a preview, a scanner, a video call) is told nothing: the HAL
//    closes the camera's own video streams, which ends the host's loss, fails the
//    requests of the gap as it does for any interruption, and opens the streams again
//    with the next request.
//
// The state of a session that is over belongs to the opened device session, not to
// the frame source or the sensor a reconfiguration replaces: it answers every later
// request, flush and configuration with the internal error, drains what it accepted
// before with error results, and a close succeeds. The tests hold these rules where
// no Android library is needed.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace andarium::camera {

// GRALLOC_USAGE_HW_VIDEO_ENCODER: the consumer of the stream is a video encoder.
constexpr std::uint64_t usage_video_encoder = std::uint64_t{1} << 16;

// One stream of a configuration, as the framework states it.
struct StreamUse {
    bool input = false;
    std::uint64_t usage = 0;
};

// Whether a stream feeds a video encoder: an output whose usage says so.
[[nodiscard]] constexpr bool feeds_video_encoder(const StreamUse& stream) noexcept
{
    return !stream.input && (stream.usage & usage_video_encoder) != 0;
}

// Whether any stream of a configuration feeds a video encoder: one such stream makes the
// session an encoder session, whatever the others are.
[[nodiscard]] constexpr bool has_encoder_stream(const StreamUse* streams, std::size_t count) noexcept
{
    if (streams == nullptr) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (feeds_video_encoder(streams[i])) return true;
    }
    return false;
}

enum class LossResponse {
    // The session is over: ERROR_REQUEST for the request that met the loss, and
    // ERROR_DEVICE besides, once.
    device_error,
    // The session goes on: the HAL closes its video streams and opens them again.
    reopen_streams,
};

[[nodiscard]] constexpr LossResponse loss_response(bool encoder_stream) noexcept
{
    return encoder_stream ? LossResponse::device_error : LossResponse::reopen_streams;
}

// The reopening of one camera's video streams after the host ended them. It never
// waits: the HAL asks at the start of each request whether a reopening is due, and a
// loss that comes while the streams are closed, or again right after they were opened,
// waits for the next due time; the requests in between fail as they do in any gap.
constexpr std::int64_t reopen_interval_ns = 250'000'000;

class ReopenGate {
public:
    // The host ended the camera's video.
    void note_loss() noexcept { pending_ = true; }

    [[nodiscard]] bool pending() const noexcept { return pending_; }

    // Whether the streams are to be closed now: once per loss, and not sooner than
    // reopen_interval_ns after the last time.
    [[nodiscard]] bool take(std::int64_t now_ns) noexcept
    {
        if (!pending_) return false;
        if (reopened_ && now_ns - last_ns_ < reopen_interval_ns) return false;
        pending_ = false;
        reopened_ = true;
        last_ns_ = now_ns;
        return true;
    }

private:
    bool pending_ = false;
    bool reopened_ = false;
    std::int64_t last_ns_ = 0;
};

// The health of one opened device session. Callable from any thread.
class SessionHealth {
public:
    // The streams of the configuration now in force; a reconfiguration may change the
    // kind of session, never the state of a session that is over.
    void set_encoder_stream(bool present) noexcept { encoder_.store(present, std::memory_order_relaxed); }

    [[nodiscard]] bool encoder_stream() const noexcept { return encoder_.load(std::memory_order_relaxed); }

    [[nodiscard]] LossResponse response() const noexcept { return loss_response(encoder_stream()); }

    // The session's device is gone. It stays so for the session.
    void latch_lost() noexcept { lost_.store(true, std::memory_order_release); }

    [[nodiscard]] bool lost() const noexcept { return lost_.load(std::memory_order_acquire); }

    // Whether this call is the one that tells the framework ERROR_DEVICE besides its
    // request error: true for the first call after the session was latched, false for
    // every other. Every request of a session that is over fails as ERROR_REQUEST, the
    // one that claims the device error too.
    [[nodiscard]] bool claim_device_error() noexcept
    {
        return lost() && !told_.exchange(true, std::memory_order_acq_rel);
    }

private:
    std::atomic<bool> encoder_{false};
    std::atomic<bool> lost_{false};
    std::atomic<bool> told_{false};
};

} // namespace andarium::camera
