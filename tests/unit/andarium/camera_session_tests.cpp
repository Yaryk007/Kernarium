// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the camera HAL does when the host ends a camera's video
// (android/lineage-24/overlay/camera/hal/camera_session.hpp): a session with a
// stream whose consumer is a video encoder is over and is told once, any other
// session closes and reopens its video streams and is told nothing, and the state of
// a session that is over is the opened session's own. The rows hold the session kinds
// (an encoder stream present or absent, a mixed session, a session reconfigured from
// one kind to the other), the once-only announcement under concurrent callers, and the
// reopening of a camera's streams: once per loss, never waiting, bounded in time when
// the host ends the video again at once.
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "camera_session.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

// The usage bits of a preview, a still and a recording as the framework states them
// (GRALLOC_USAGE_HW_TEXTURE, GRALLOC_USAGE_HW_COMPOSER, GRALLOC_USAGE_HW_VIDEO_ENCODER).
constexpr std::uint64_t texture = 0x100;
constexpr std::uint64_t composer = 0x800;
constexpr std::uint64_t encoder = 0x10000;

constexpr std::int64_t ms = 1'000'000;

bool encoder_session(const std::vector<StreamUse>& streams)
{
    return has_encoder_stream(streams.data(), streams.size());
}

void check_which_streams_feed_an_encoder()
{
    require(encoder == usage_video_encoder, "the encoder bit is the gralloc one");
    require(feeds_video_encoder({false, encoder}), "an output with the encoder usage");
    require(feeds_video_encoder({false, encoder | texture}), "among other bits");
    require(!feeds_video_encoder({false, texture | composer}), "a preview is not");
    require(!feeds_video_encoder({false, 0}), "nor is a stream that states no usage");
    require(!feeds_video_encoder({true, encoder}), "an input stream is not an encoder's feed");
}

void check_the_kinds_of_session()
{
    const StreamUse preview{false, texture | composer};
    const StreamUse still{false, 0};
    const StreamUse record{false, encoder};
    require(!encoder_session({preview}), "a preview alone");
    require(!encoder_session({preview, still}), "a preview with a still");
    require(encoder_session({record}), "a recording alone");
    require(encoder_session({preview, still, record}), "a mixed session is an encoder session");
    require(encoder_session({record, preview}), "wherever the encoder stream is listed");
    require(!encoder_session({}), "no streams");
    require(!has_encoder_stream(nullptr, 3), "no list");
    require(!encoder_session({StreamUse{true, encoder}, preview}), "an encoder usage on the input of a reprocess does not count");
    require(loss_response(true) == LossResponse::device_error, "an encoder session is told the device is gone");
    require(loss_response(false) == LossResponse::reopen_streams, "any other session reopens its streams");
}

void check_a_session_reconfigured_from_one_kind_to_the_other()
{
    SessionHealth health;
    require(!health.encoder_stream() && health.response() == LossResponse::reopen_streams, "a new session has no encoder stream");
    health.set_encoder_stream(encoder_session({{false, texture}}));
    require(health.response() == LossResponse::reopen_streams, "a preview configuration");
    health.set_encoder_stream(encoder_session({{false, texture}, {false, encoder}}));
    require(health.response() == LossResponse::device_error, "reconfigured with a recording");
    health.set_encoder_stream(encoder_session({{false, texture}}));
    require(health.response() == LossResponse::reopen_streams, "and back to a preview");
}

void check_the_state_of_a_session_that_is_over_is_the_sessions()
{
    SessionHealth health;
    require(!health.lost() && !health.claim_device_error(), "a healthy session announces nothing");
    health.set_encoder_stream(true);
    health.latch_lost();
    require(health.lost(), "latched");
    require(health.claim_device_error(), "the first request after the latch tells the framework");
    require(!health.claim_device_error() && !health.claim_device_error(), "and no other request does");
    // A reconfiguration, whatever it turns the session into, neither clears the state nor tells again.
    health.set_encoder_stream(false);
    require(health.lost() && !health.claim_device_error(), "a reconfiguration to a preview does not clear it");
    health.set_encoder_stream(true);
    health.latch_lost();
    require(health.lost() && !health.claim_device_error(), "and latching again does not announce again");
    // A session opened afterwards is a new one.
    SessionHealth fresh;
    require(!fresh.lost(), "a fresh session is healthy");
    fresh.latch_lost();
    require(fresh.claim_device_error(), "and tells once itself");
}

void check_the_announcement_is_made_once_under_concurrent_callers()
{
    for (int round = 0; round < 50; ++round) {
        SessionHealth health;
        health.latch_lost();
        std::atomic<int> told{0};
        std::array<std::thread, 4> workers;
        for (auto& worker : workers) {
            worker = std::thread([&] {
                for (int i = 0; i < 20; ++i) {
                    if (health.claim_device_error()) ++told;
                }
            });
        }
        for (auto& worker : workers) worker.join();
        require(told == 1, "exactly one caller tells the framework");
    }
}

void check_the_reopening_of_a_cameras_streams()
{
    ReopenGate gate;
    require(!gate.pending() && !gate.take(0), "nothing to do without a loss");
    gate.note_loss();
    require(gate.pending(), "a loss is pending");
    require(gate.take(10 * ms), "the first loss is acted on at once");
    require(!gate.pending() && !gate.take(11 * ms), "once per loss");
    // The host ends the video again right after the streams were opened: not before the interval.
    gate.note_loss();
    gate.note_loss();
    require(gate.pending(), "a second loss is pending, however often it is noted");
    require(!gate.take(10 * ms + reopen_interval_ns - 1), "not sooner than the interval after the last reopening");
    require(gate.pending(), "and it stays pending, nothing waits for it");
    require(gate.take(10 * ms + reopen_interval_ns), "then it is due");
    require(!gate.take(10 * ms + reopen_interval_ns + 1), "one reopening for the two notes");
    // A loss long after the last one is acted on at once.
    gate.note_loss();
    require(gate.take(60'000 * ms), "a later loss is acted on at once");
    // Requests that ask for nothing cost nothing.
    for (std::int64_t at = 61'000 * ms; at < 62'000 * ms; at += 33 * ms) require(!gate.take(at), "no loss, no reopening");
    // A host that ends the video again every time bounds the reopenings by time, not by the number of requests.
    ReopenGate stubborn;
    int reopened = 0;
    for (std::int64_t at = 0; at < 2'000 * ms; at += 33 * ms) {
        stubborn.note_loss();
        if (stubborn.take(at)) ++reopened;
    }
    require(reopened >= 7 && reopened <= 8, "a loss at every request reopens at most four times a second");
}

} // namespace

int main()
{
    check_which_streams_feed_an_encoder();
    check_the_kinds_of_session();
    check_a_session_reconfigured_from_one_kind_to_the_other();
    check_the_state_of_a_session_that_is_over_is_the_sessions();
    check_the_announcement_is_made_once_under_concurrent_callers();
    check_the_reopening_of_a_cameras_streams();
    return 0;
}
