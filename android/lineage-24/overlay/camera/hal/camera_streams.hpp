// SPDX-License-Identifier: Apache-2.0
//
// When the HAL gives up the host stream of an output. Each video output of a
// request has a stream on the camera's V4L2 node, and the host's capture
// session lives as long as a video stream of some camera runs, a still is in
// flight or the torch is on. Closing a camera's last video stream, with
// nothing else of that kind going on, closes the session, and the next stream
// opens it again with black stand-in frames until the platform delivers. So a
// stream must not be closed merely because the request pipeline was held up: a
// still holds it for as long as the host takes to make the picture, and the
// requests of the preview wait behind it, behind one still or a queue of them.
//
// An output that leaves the configuration needs no rule here: the framework's
// reconfiguration makes a new frame source. The rule is for an output that
// stays configured but leaves the requests (a recording that stopped): the host
// runs the camera at the largest size of the streams that are open, so the
// stream of an output that went must go soon, and the restart for a smaller
// size belongs to that moment and not to a recording started a few seconds
// later.
//
// The HAL asks at the start of every request, for each stream the request does
// not use. A video output's stream is closed only when at least
// `idle_output_still_free_requests` requests that took no still began since its
// last use and it has been unused for `idle_output_limit_ns`. A request that
// takes a still is not counted, however long it lasts and however many follow
// one another, and the absences that every open video output earned before such
// a request start again from zero when it begins (the idle time is left as it
// is): the requests that wait behind a still wait behind all of it, so a queue of
// stills, or of YUV and JPEG captures mixed, never closes a preview that waits
// behind it. Any other stream (a still's, or a picture the HAL makes from a
// frame) is closed after `idle_stream_limit_ns` unused.
//
// Two limits, neither of which gives a wrong picture, because such requests
// cannot be told, request by request, from an output the client dropped. A
// recording output that was dropped while every request that follows takes a
// still (a repeating preview and RAW request, say) stays open until a request
// without a still or a reconfiguration. Sixteen or more consecutive captures
// that leave the preview out and take no still close the preview's stream.
#pragma once

#include <cstdint>

namespace andarium::camera {

constexpr std::int64_t idle_stream_limit_ns = 5'000'000'000;
constexpr std::int64_t idle_output_limit_ns = 500'000'000;
constexpr std::uint64_t idle_output_still_free_requests = 2;

// Whether the stream that the request about to begin does not use is closed.
// `still_free_requests_since_use` counts the requests without a still that began
// after the stream's last use and before this one; `idle_ns` is the time since
// that use.
[[nodiscard]] constexpr bool closes_when_idle(bool video_output, std::uint64_t still_free_requests_since_use,
                                              std::int64_t idle_ns) noexcept
{
    if (!video_output) return idle_ns > idle_stream_limit_ns;
    return still_free_requests_since_use >= idle_output_still_free_requests && idle_ns > idle_output_limit_ns;
}

} // namespace andarium::camera
