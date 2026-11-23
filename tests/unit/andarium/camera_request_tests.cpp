// SPDX-License-Identifier: GPL-2.0-or-later
//
// How a request of the camera HAL ends (android/lineage-24/overlay/camera/hal):
// one budget that the buffer waits of a request share, and the notify the
// framework gets when some or all of the outputs failed. The model below
// collects a request's buffers the way the sensor does, each wait against what
// is left of the request's clock, and reads the end off the rule the HAL uses.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "camera_request.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

constexpr std::int64_t ms = 1'000'000;
constexpr std::int64_t second = 1'000 * ms;

enum class Kind { preview, record, jpeg, raw };
enum class Fate { arrives, refused, never };

// A buffer's fate: the device completes it at `at` (nanoseconds after the
// request began), with the error flag for a refusal, or does not complete it.
// `work` is what the HAL spends on the buffer once it was dequeued (the wait for
// its metadata, at most 200 ms); it is outside the clock's budget.
struct Output {
    Kind kind;
    Fate fate;
    std::int64_t at;
    std::int64_t work = 0;
};

struct Told {
    RequestEnd end = RequestEnd::complete;
    std::int64_t elapsed = 0;
    std::size_t failed = 0;
    std::vector<bool> filled;

    // What the sensor sends, by the rule of the end.
    [[nodiscard]] bool shutter() const { return end != RequestEnd::request_error; }
    [[nodiscard]] bool results() const { return end != RequestEnd::request_error; }
    [[nodiscard]] std::size_t error_buffers() const { return end == RequestEnd::buffer_errors ? failed : 0; }
};

// The sensor's loop over a request: every buffer waits for what is left of the
// request's budget. A wait takes a buffer that completed before it began, even
// when the budget is spent (V4l2Stream::Wait dequeues before it looks at the
// time), and a wait under way at the deadline gives up and takes the stream's
// buffers back.
Told collect(const std::vector<Output>& outputs, bool with_still = false)
{
    for (const auto& output : outputs) with_still = with_still || output.kind == Kind::jpeg || output.kind == Kind::raw;
    RequestClock clock;
    clock.begin(0, with_still);
    Told told;
    std::int64_t now = 0;
    for (const auto& output : outputs) {
        const std::int64_t limit = now + clock.remaining(now);
        bool filled = false;
        if (output.fate == Fate::never || output.at > limit) {
            now = limit; // the wait ends at the deadline and the stream starts over
        } else {
            now = std::max(now, output.at) + output.work;
            filled = output.fate == Fate::arrives;
        }
        told.filled.push_back(filled);
        if (!filled) ++told.failed;
    }
    told.elapsed = now;
    told.end = request_end(outputs.size(), told.failed);
    return told;
}

// What the sensor sends to the framework for a request, in its order (the
// shutter, then the buffer errors as the buffers go back, then the results), and
// what the application's callbacks make of each message.
struct Message {
    std::uint32_t frame;
    std::string callback; // what the application's capture callback sees
};

std::vector<Message> sent(std::uint32_t frame, const Told& told)
{
    std::vector<Message> messages;
    if (told.shutter()) messages.push_back({frame, "onCaptureStarted"});
    if (told.end == RequestEnd::request_error) messages.push_back({frame, "onCaptureFailed"});
    for (std::size_t i = 0; i < told.error_buffers(); ++i) messages.push_back({frame, "onCaptureBufferLost"});
    if (told.results()) messages.push_back({frame, "onCaptureCompleted"});
    return messages;
}

std::vector<std::string> callbacks_of(const std::vector<Message>& messages, std::uint32_t frame)
{
    std::vector<std::string> result;
    for (const auto& message : messages)
        if (message.frame == frame) result.push_back(message.callback);
    return result;
}

void check_rule()
{
    require(request_end(0, 0) == RequestEnd::complete, "a request without outputs is complete");
    require(request_end(3, 0) == RequestEnd::complete, "every output filled is complete");
    require(request_end(3, 1) == RequestEnd::buffer_errors, "one of three failed is a buffer error");
    require(request_end(3, 2) == RequestEnd::buffer_errors, "two of three failed is buffer errors");
    require(request_end(3, 3) == RequestEnd::request_error, "every output failed is a request error");
    require(request_end(1, 1) == RequestEnd::request_error, "the only output failed is a request error");
}

// The host (or the driver) refuses the picture: the framework hears of a failed
// request, not of a lost buffer, and the next picture is a new request.
void check_refused_still()
{
    const auto refused = collect({{Kind::jpeg, Fate::refused, 2 * second}});
    require(refused.end == RequestEnd::request_error, "a refused still is a request error");
    require(!refused.shutter() && !refused.results(), "a failed request has no shutter and no results");
    require(refused.error_buffers() == 0, "its buffer goes back marked as failed, with no buffer error of its own");
    require(refused.elapsed == 2 * second, "the refusal ends the request when it comes");

    const auto next = collect({{Kind::jpeg, Fate::arrives, 800 * ms}});
    require(next.end == RequestEnd::complete && next.shutter() && next.results(), "the next still is a request of its own");
}

// The device says nothing: the budget ends the waits, and it is one limit
// whatever the number of buffers.
void check_deadline()
{
    const auto still = collect({{Kind::jpeg, Fate::never, 0}});
    require(still.end == RequestEnd::request_error, "a still that never arrives is a request error");
    require(still.elapsed == still_timeout_ns, "it ends at the still deadline");

    const auto frame = collect({{Kind::preview, Fate::never, 0}});
    require(frame.end == RequestEnd::request_error && frame.elapsed == frame_timeout_ns, "a frame ends at the frame deadline");

    const auto many = collect({{Kind::jpeg, Fate::never, 0}, {Kind::raw, Fate::never, 0}, {Kind::preview, Fate::never, 0}});
    require(many.elapsed == still_timeout_ns, "three buffers share one budget");
    require(many.end == RequestEnd::request_error, "and fail together");

    // A wait under way at the deadline gives up: a picture that comes after it is too late.
    const auto late = collect({{Kind::jpeg, Fate::arrives, still_timeout_ns + 1}});
    require(late.end == RequestEnd::request_error, "a picture that arrives after the budget, during its wait, is too late");

    // A buffer that completed before its wait began is taken even when the budget is spent, and what the HAL does
    // with a buffer after it was dequeued is outside the budget: the request can end after the limit.
    const auto taken = collect({{Kind::preview, Fate::arrives, 14'950 * ms, 200 * ms}, {Kind::record, Fate::arrives, 15'050 * ms}},
                               true);
    require(taken.end == RequestEnd::complete && taken.filled[1], "a buffer that completed before its wait is taken after the budget");
    require(taken.elapsed == 15'150 * ms, "and the request ends after the limit, by what the first buffer's collecting took");

    RequestClock clock;
    clock.begin(5 * second, true);
    require(clock.remaining(5 * second) == still_timeout_ns, "a request starts with its whole limit");
    require(clock.remaining(5 * second + still_timeout_ns + 1) == 0, "no time is left after the deadline, and never less than none");
    clock.begin(100 * second, false);
    require(clock.remaining(100 * second) == frame_timeout_ns, "a request without a still has the frame limit");
}

// The RAW picture is refused and the JPEG is made: the request is not failed,
// and the framework hears only of the RAW buffer.
void check_partial_still()
{
    const auto raw_refused = collect({{Kind::jpeg, Fate::arrives, 900 * ms}, {Kind::raw, Fate::refused, 900 * ms}});
    require(raw_refused.end == RequestEnd::buffer_errors, "a refused RAW leg is a buffer error");
    require(raw_refused.shutter() && raw_refused.results(), "the JPEG's shutter and results are delivered");
    require(raw_refused.error_buffers() == 1 && raw_refused.filled[0] && !raw_refused.filled[1], "only the RAW buffer is lost");

    const auto jpeg_refused = collect({{Kind::jpeg, Fate::refused, 900 * ms}, {Kind::raw, Fate::arrives, 900 * ms}});
    require(jpeg_refused.end == RequestEnd::buffer_errors && jpeg_refused.error_buffers() == 1,
            "a refused JPEG leg with a RAW leg is a buffer error too");
}

// Preview and a recording run while a picture is refused: the picture's request
// fails and the frames' requests go on, and a snapshot that loses its JPEG does
// not take a frame from the recording.
void check_with_streams_running()
{
    const std::int64_t frame = 33 * ms;
    for (int i = 0; i < 3; ++i) {
        const auto frames = collect({{Kind::preview, Fate::arrives, frame}, {Kind::record, Fate::arrives, frame}});
        require(frames.end == RequestEnd::complete && frames.shutter(), "the frames of the streams are requests of their own");
    }
    const auto refused = collect({{Kind::jpeg, Fate::refused, 2 * second}});
    require(refused.end == RequestEnd::request_error, "the refused picture fails as a request");
    for (int i = 0; i < 3; ++i) {
        const auto frames = collect({{Kind::preview, Fate::arrives, frame}, {Kind::record, Fate::arrives, frame}});
        require(frames.end == RequestEnd::complete && frames.error_buffers() == 0, "the streams are not disturbed");
    }

    const auto snapshot = collect({{Kind::preview, Fate::arrives, frame}, {Kind::record, Fate::arrives, frame}, {Kind::jpeg, Fate::refused, 2 * second}});
    require(snapshot.end == RequestEnd::buffer_errors, "a snapshot that loses its JPEG loses that buffer only");
    require(snapshot.shutter() && snapshot.results() && snapshot.filled[0] && snapshot.filled[1], "the recording's frame is delivered");

    const auto stalled = collect({{Kind::preview, Fate::never, 0}, {Kind::record, Fate::never, 0}});
    require(stalled.end == RequestEnd::request_error && stalled.elapsed == frame_timeout_ns, "frames that never arrive fail the request within the frame deadline");
}

// What an application sees. A still that the host refuses, between preview
// requests that keep completing: onCaptureFailed for the still, and the preview
// requests before and after it complete, untouched and in order. A refused RAW
// leg beside a JPEG that is made: the capture started, the RAW buffer is lost
// (Camera2 only; CameraX ignores it) and the capture completed with the JPEG.
void check_what_the_application_sees()
{
    const std::int64_t frame = 33 * ms;
    const std::vector<Output> preview = {{Kind::preview, Fate::arrives, frame}};
    std::vector<Message> seen;
    const auto add = [&seen](std::uint32_t number, const Told& told) {
        for (const auto& message : sent(number, told)) seen.push_back(message);
    };
    add(10, collect(preview));
    add(11, collect({{Kind::jpeg, Fate::refused, 2 * second}}));
    add(12, collect(preview));
    add(13, collect(preview));

    const std::vector<std::string> good = {"onCaptureStarted", "onCaptureCompleted"};
    require(callbacks_of(seen, 10) == good, "the preview request before the still completes");
    require(callbacks_of(seen, 11) == std::vector<std::string>({"onCaptureFailed"}),
            "the refused still gets onCaptureFailed, and nothing else");
    require(callbacks_of(seen, 12) == good && callbacks_of(seen, 13) == good, "the preview requests after it complete");
    std::vector<std::uint32_t> order;
    for (const auto& message : seen)
        if (order.empty() || order.back() != message.frame) order.push_back(message.frame);
    require(order == std::vector<std::uint32_t>({10, 11, 12, 13}), "and the frames stay in order");

    // The shot after the refused one is a request like any other.
    require(callbacks_of(sent(14, collect({{Kind::jpeg, Fate::arrives, 900 * ms}})), 14) == good, "the next still completes");

    const auto raw_refused = collect({{Kind::jpeg, Fate::arrives, 900 * ms}, {Kind::raw, Fate::refused, 900 * ms}});
    require(callbacks_of(sent(20, raw_refused), 20) ==
                std::vector<std::string>({"onCaptureStarted", "onCaptureBufferLost", "onCaptureCompleted"}),
            "a refused RAW leg with a JPEG that is made: started, the RAW buffer lost, completed");
}

// The host ends a camera (the application left the foreground while the guest
// had a video stream open, wire README): the buffers of its video streams fail
// and its newest record says black and interrupted. That is the camera's loss,
// which the HAL then answers by the session's streams (camera_session.hpp); a
// failed video buffer under any other record, and a failed still, are not.
void check_the_hosts_end_of_a_camera()
{
    namespace flag = abi::frame_flag;
    const std::uint32_t both = flag::black | flag::interrupted;
    require(camera_lost(true, true, true, both), "a failed video buffer under a black, interrupted record is the end");
    require(camera_lost(true, true, true, both | flag::resuming), "whatever else the record says");
    require(!camera_lost(true, false, true, both), "a buffer that did not fail is a frame, black or not");
    require(!camera_lost(false, true, true, both), "a failed still is a failed still");
    require(!camera_lost(true, true, false, both), "with no record, a failed buffer is a failed buffer");
    require(!camera_lost(true, true, true, 0), "under a live record, too");
    require(!camera_lost(true, true, true, flag::black | flag::blocked), "and a blocked session's black is no loss");
    require(!camera_lost(true, true, true, flag::interrupted), "an interruption alone is a dark gap");
    require(!camera_lost(true, true, true, flag::black | flag::resuming), "and so is a restart");
}

} // namespace

int main()
{
    check_the_hosts_end_of_a_camera();
    check_rule();
    check_refused_still();
    check_deadline();
    check_partial_still();
    check_with_streams_running();
    check_what_the_application_sees();
    return 0;
}
