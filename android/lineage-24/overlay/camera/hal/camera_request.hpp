// SPDX-License-Identifier: Apache-2.0
//
// How a capture request of the camera HAL ends. The HAL collects the output
// buffers of a request one after another, their waits sharing one budget, and the
// framework learns of the end by the camera HAL's notify contract: a request
// whose outputs all failed is an ERROR_REQUEST (a failed capture, after which the
// application may shoot again), while ERROR_BUFFER belongs to a request that lost
// only some of its outputs. A camera the host has ended fails the request as a
// whole: when the session has a video encoder stream the framework is told
// ERROR_REQUEST for it and, once per session, ERROR_DEVICE besides
// (camera_session.hpp); any other session is told nothing of it. The tests hold
// the rules where no Android library is needed.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "camera_abi.hpp"

namespace andarium::camera {

// What the device is given for the buffers of a request. A frame is due at once;
// a request with a still waits for the device to finish its capture (the device
// gives a started still ten seconds) and for the rest of the group's pictures
// (two seconds more), so the HAL's limit is the device's own and some more.
constexpr std::int64_t frame_timeout_ns = 3'000'000'000;
constexpr std::int64_t still_timeout_ns = 15'000'000'000;

// What the framework is told when the HAL has collected a request.
enum class RequestEnd {
    // Every output is filled: the shutter, the results and the buffers.
    complete,
    // Some outputs failed: the shutter, the results and the buffers as above,
    // and ERROR_BUFFER for each failed one. The filled outputs are real frames,
    // such as a recording's that a refused snapshot must not interrupt.
    buffer_errors,
    // No output could be made: ERROR_REQUEST, no shutter and no results, and
    // the buffers go back marked as failed (with no ERROR_BUFFER of their own).
    request_error,
};

[[nodiscard]] constexpr RequestEnd request_end(std::size_t outputs, std::size_t failed) noexcept
{
    if (outputs == 0 || failed == 0) return RequestEnd::complete;
    return failed >= outputs ? RequestEnd::request_error : RequestEnd::buffer_errors;
}

// Whether a buffer that completed with the error flag is the host's end of the
// camera: it belongs to a video stream, and the camera's newest metadata record
// says the camera is black and interrupted (the host fails the buffers of a video
// it ended, and records them so, for as long as the guest keeps the stream; wire
// README of kernel/common/video). A failed video buffer under any other record,
// or with no record, and a failed still, are a failed buffer. The dequeue flags
// of the wire do not reach the HAL, which is why the record is the second half
// of the news.
[[nodiscard]] constexpr bool camera_lost(bool video_stream, bool buffer_failed, bool has_record,
                                         std::uint32_t record_flags) noexcept
{
    constexpr std::uint32_t ended = abi::frame_flag::black | abi::frame_flag::interrupted;
    return video_stream && buffer_failed && has_record && (record_flags & ended) == ended;
}

// The one budget that the buffer waits of a request share. Every buffer waits
// for what is left of it, so the waits of any number of outputs add up to one
// limit, and a wait that starts after the budget is spent ends at once, taking
// a buffer that has completed by then. The time the HAL spends on a buffer after
// it was dequeued (its metadata wait, scaling, encoding) is outside the budget.
class RequestClock {
public:
    void begin(std::int64_t now_ns, bool with_still) noexcept
    {
        deadline_ns_ = now_ns + (with_still ? still_timeout_ns : frame_timeout_ns);
    }

    [[nodiscard]] std::int64_t remaining(std::int64_t now_ns) const noexcept
    {
        return std::max<std::int64_t>(deadline_ns_ - now_ns, 0);
    }

private:
    std::int64_t deadline_ns_ = 0;
};

} // namespace andarium::camera
