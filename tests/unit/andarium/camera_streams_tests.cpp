// SPDX-License-Identifier: GPL-2.0-or-later
//
// When the camera HAL closes the host stream of an output
// (android/lineage-24/overlay/camera/hal/camera_streams.hpp). The model keeps
// the bookkeeping that V4l2FrameSource::BeginRequest keeps, request by request:
// the count of requests that took no still, which one of them last used each
// stream, when it was last used, and the question put to the rule at the start
// of a request for every stream that the request does not use. A request holds
// the pipeline for as long as it takes, and the streams it uses are used again
// when it ends, as the HAL stamps a stream when it queues a buffer and when it
// collects it.
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "camera_streams.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

constexpr std::int64_t ms = 1'000'000;
constexpr std::int64_t second = 1'000 * ms;

// The streams the HAL has: a video output has an id from 1 on, and the host's
// stills (a JPEG, and a RAW) are 100 and 101. A request that has one of those
// takes a still.
constexpr int preview = 1;
constexpr int recording = 2;
constexpr int yuv = 3;
constexpr int jpeg = 100;
constexpr int raw = 101;

bool is_output(int id)
{
    return id < jpeg;
}

bool takes_a_still(const std::set<int>& uses)
{
    return uses.count(jpeg) != 0 || uses.count(raw) != 0;
}

// The rules the HAL had before: time alone, whatever the requests were; and
// the count of every request, still or not.
bool closes_by_time(bool video_output, std::uint64_t, std::int64_t idle_ns)
{
    return idle_ns > (video_output ? idle_output_limit_ns : idle_stream_limit_ns);
}

using Rule = std::function<bool(bool, std::uint64_t, std::int64_t)>;

class Hal {
public:
    // `count_stills`: whether a request that takes a still advances the count
    // (the rule of the first version of the change, which a queue of stills
    // defeated); `skip_used`: whether a stream the request uses is asked about;
    // `restart`: whether a request that takes a still starts the count again for
    // every open video output (the rule of the second version, which a mixed
    // queue of captures defeated).
    explicit Hal(Rule rule = closes_when_idle, bool skip_used = true, bool count_stills = false, bool restart = true)
        : rule_(std::move(rule)), skip_used_(skip_used), count_stills_(count_stills), restart_(restart) {}

    struct Stream {
        std::uint64_t last_request = 0;
        std::int64_t last_used_ns = 0;
    };

    // A request that uses `uses` begins at `now` and holds the pipeline for
    // `hold`; the time of the next request's earliest begin is returned.
    std::int64_t request(std::int64_t now, const std::set<int>& uses, std::int64_t hold)
    {
        for (auto it = streams_.begin(); it != streams_.end();) {
            const bool used = uses.count(it->first) != 0;
            const std::uint64_t since_use = begun_ - it->second.last_request;
            if ((!skip_used_ || !used) && rule_(is_output(it->first), since_use, now - it->second.last_used_ns)) {
                closed_.push_back({it->first, now});
                it = streams_.erase(it);
            } else {
                ++it;
            }
        }
        if (count_stills_ || !takes_a_still(uses)) {
            ++begun_;
        } else if (restart_) {
            for (auto& [id, stream] : streams_)
                if (is_output(id)) stream.last_request = begun_;
        }
        for (int id : uses) {
            if (streams_.count(id) == 0) opened_.push_back({id, now});
            streams_[id].last_request = begun_;
            streams_[id].last_used_ns = now + hold;
        }
        return now + hold;
    }

    // A client that sends a request every `period` from `from` until `until`;
    // the time after the last one is returned.
    std::int64_t preview_requests(std::int64_t from, std::int64_t until, std::int64_t period,
                                  const std::set<int>& uses = {preview})
    {
        std::int64_t now = from;
        while (now < until) now = request(now, uses, 0) + period;
        return now;
    }

    // `count` requests that each take a still of `kind` (or, with `pairs`, a
    // JPEG and then a RAW), queued: each begins when the one before it ended.
    std::int64_t burst(std::int64_t from, int count, std::int64_t hold, bool pairs = false)
    {
        std::int64_t now = from;
        for (int i = 0; i < count; ++i) now = request(now, {pairs && i % 2 == 1 ? raw : jpeg}, hold);
        return now;
    }

    [[nodiscard]] bool open(int id) const { return streams_.count(id) != 0; }
    [[nodiscard]] std::size_t closes(int id) const { return count(closed_, id); }
    [[nodiscard]] std::size_t opens(int id) const { return count(opened_, id); }
    // When the stream was closed the n-th time (from 0), in ns.
    [[nodiscard]] std::int64_t closed_at(int id, std::size_t n = 0) const
    {
        for (const auto& [stream, when] : closed_)
            if (stream == id && n-- == 0) return when;
        return -1;
    }
    [[nodiscard]] std::int64_t last_used(int id) const { return streams_.at(id).last_used_ns; }

private:
    using Events = std::vector<std::pair<int, std::int64_t>>;
    static std::size_t count(const Events& events, int id)
    {
        std::size_t n = 0;
        for (const auto& event : events) n += event.first == id ? 1 : 0;
        return n;
    }

    Rule rule_;
    bool skip_used_;
    bool count_stills_;
    bool restart_;
    std::uint64_t begun_ = 0;
    std::map<int, Stream> streams_;
    Events closed_, opened_;
};

// The rule's table: a video output needs both an absence of still-free requests
// and the time.
static_assert(!closes_when_idle(true, 0, 10 * second), "a stream the last request used is not closed, however long ago");
static_assert(!closes_when_idle(true, idle_output_still_free_requests - 1, 10 * second), "one request without it is not enough");
static_assert(closes_when_idle(true, idle_output_still_free_requests, idle_output_limit_ns + 1), "two requests and the time close it");
static_assert(!closes_when_idle(true, idle_output_still_free_requests, idle_output_limit_ns), "the limit itself is not yet over it");
static_assert(!closes_when_idle(true, 1000, idle_output_limit_ns), "requests without time do not close it");
static_assert(!closes_when_idle(false, 1000, idle_stream_limit_ns), "a still's stream takes its own limit, whatever the requests");
static_assert(closes_when_idle(false, 0, idle_stream_limit_ns + 1), "and closes after it, as the requests had nothing to do with it");
static_assert(idle_output_limit_ns == 500 * ms && idle_stream_limit_ns == 5 * second && idle_output_still_free_requests == 2,
              "the limits of the HAL: half a second and two still-free requests for an output, five seconds for a still");

// The finding: a still holds the pipeline for longer than the limit, the
// preview's requests wait behind it, and the first of them used to find its
// stream closed (and so did the host its capture session). The preview is kept
// now, and the rule the HAL had would have closed it (the model proves that the
// scenario is the one that closed it).
void check_preview_behind_a_still()
{
    for (const bool old_rule : {true, false}) {
        Hal hal(old_rule ? Rule(closes_by_time) : Rule(closes_when_idle), !old_rule);
        std::int64_t now = hal.preview_requests(0, 3 * second, 33 * ms);
        require(hal.open(preview), "the preview runs");
        // The shutter: one request that has no preview and holds the pipeline for the host's picture.
        now = hal.request(now, {jpeg}, 1900 * ms) + 33 * ms;
        now = hal.preview_requests(now, now + 2 * second, 33 * ms);
        require(hal.open(jpeg), "the still's stream is kept for its own limit");
        if (old_rule) {
            require(hal.closes(preview) == 1 && hal.opens(preview) == 2,
                    "the rule of time alone closed the preview's stream behind the still and opened it again");
        } else {
            require(hal.closes(preview) == 0 && hal.opens(preview) == 1, "the preview's stream stays open behind the still");
        }
    }

    // Shot after shot, as the tests of the finding took them (the shutter tapped every 2 seconds).
    Hal hal;
    std::int64_t now = hal.preview_requests(0, second, 33 * ms);
    for (int shot = 0; shot < 20; ++shot) {
        now = hal.request(now, {jpeg}, 1900 * ms) + 33 * ms;
        now = hal.preview_requests(now, now + 100 * ms, 33 * ms);
    }
    require(hal.closes(preview) == 0 && hal.opens(preview) == 1, "twenty stills in a row do not restart the preview");

    // The preview and the picture in one request (a client that keeps the preview in its capture) are no absence.
    Hal together;
    now = together.preview_requests(0, second, 33 * ms);
    now = together.request(now, {preview, jpeg}, 1900 * ms) + 33 * ms;
    together.preview_requests(now, now + second, 33 * ms);
    require(together.closes(preview) == 0, "a still requested with the preview closes nothing");
}

// Android runs queued capture requests back to back, so a client that takes
// several pictures at once sends a queue of stills with no preview request
// between them, and the preview's requests wait behind all of them. The count
// of absences must not advance for a request that takes a still: a burst of
// any length, of any speed, leaves the preview its stream.
void check_a_burst_of_stills()
{
    for (const int count : {3, 4, 5, 8}) {
        for (const std::int64_t hold : {250 * ms, 300 * ms, 1000 * ms}) {
            for (const bool pairs : {false, true}) {
                Hal hal;
                std::int64_t now = hal.preview_requests(0, second, 33 * ms);
                now = hal.burst(now + 33 * ms, count, hold, pairs);
                now = hal.preview_requests(now, now + second, 33 * ms);
                require(hal.closes(preview) == 0 && hal.opens(preview) == 1,
                        "a burst of stills leaves the preview its stream, however many and however slow");
                require(hal.open(preview), "the preview still runs after the burst");
            }
        }
    }

    // The rule of the first version of the change counted every request; the same bursts closed the preview then (three
    // at 0.3 s, five at 0.25 s, eight at 1 s), which is what the model has to show for the test to mean something.
    for (const auto& [count, hold] : {std::pair<int, std::int64_t>{3, 300 * ms}, {5, 250 * ms}, {8, 1000 * ms}}) {
        Hal counted(closes_when_idle, true, true, false);
        std::int64_t now = counted.preview_requests(0, second, 33 * ms);
        now = counted.burst(now + 33 * ms, count, hold);
        counted.preview_requests(now, now + second, 33 * ms);
        require(counted.closes(preview) >= 1, "counting every request, a burst closed the preview");
    }

    // A preview request between the stills, or a still with the preview, is still-free or no absence either way.
    Hal mixed;
    std::int64_t now = mixed.preview_requests(0, second, 33 * ms);
    for (int i = 0; i < 6; ++i) {
        now = mixed.request(now, {jpeg}, 400 * ms);
        now = mixed.request(now, {preview}, 0) + 33 * ms;
    }
    require(mixed.closes(preview) == 0, "stills with preview requests between them close nothing");
}

// A legal burst can mix captures that take no still (two YUV image reader captures) with JPEG or RAW ones, while the
// repeating preview waits behind the whole list. The two YUV captures are absences of the preview, and the second
// of the stills 0.9 s after the first used to close the preview's stream: the absences earned before a request that takes
// a still start again from zero when it begins.
void check_a_mixed_burst()
{
    for (const int still : {jpeg, raw}) {
        for (const std::int64_t hold : {300 * ms, 900 * ms}) {
            Hal restarting;
            Hal carrying(closes_when_idle, true, false, false);
            for (Hal* hal : {&restarting, &carrying}) {
                std::int64_t now = hal->preview_requests(0, second, 33 * ms);
                now = hal->request(now, {yuv}, 0);
                now = hal->request(now + 33 * ms, {yuv}, 0);
                now = hal->request(now + 33 * ms, {still}, hold);
                now = hal->request(now, {still}, hold);
                hal->preview_requests(now, now + second, 33 * ms);
            }
            require(restarting.closes(preview) == 0 && restarting.opens(preview) == 1,
                    "a mixed burst of two YUV and two stills leaves the preview its stream");
            if (hold == 900 * ms) {
                require(carrying.closes(preview) == 1, "carrying the absences over the stills, it closed the preview behind them");
            }
        }
    }

    // The stream of a dropped recording still goes at the second still-free request after the half second, when
    // stills and captures without one come before it in any order.
    Hal hal;
    std::int64_t now = hal.preview_requests(0, 2 * second, 33 * ms, {preview, recording});
    now = hal.request(now, {yuv}, 0);
    now = hal.request(now + 33 * ms, {jpeg}, 900 * ms);
    now = hal.request(now, {raw}, 300 * ms);
    require(hal.open(recording) && hal.open(preview), "the burst closed nothing");
    hal.request(now, {preview}, 0);
    hal.request(now + 33 * ms, {preview}, 0);
    require(hal.open(recording), "two still-free requests after the burst are not enough");
    hal.request(now + 66 * ms, {preview}, 0);
    require(!hal.open(recording) && hal.closes(recording) == 1 && hal.closes(preview) == 0,
            "the dropped recording goes at the third, the preview stays");
}

// The edge of the rule: one still-free request without the stream is tolerated,
// and so is the second, however long they hold the pipeline; the third request
// without it closes a stream whose time is over.
void check_boundary()
{
    Hal hal;
    std::int64_t now = hal.preview_requests(0, second, 33 * ms);
    // Requests that take no still but have no preview either (another stream of the camera, say).
    now = hal.request(now, {recording}, 900 * ms);
    require(hal.closes(preview) == 0, "the first request without the preview closes nothing");
    now = hal.request(now, {recording}, 900 * ms);
    require(hal.closes(preview) == 0, "nor does the second: only one request had passed when it began");
    now = hal.request(now, {recording}, 900 * ms);
    require(hal.closes(preview) == 1, "the third finds the preview absent from two requests and idle for over half a second");

    // A stalled pipeline (the client paused, the host was suspended) and then a request that uses the stream:
    // the stream is not closed for the request that needs it, whatever the stall was.
    Hal stalled;
    now = stalled.preview_requests(0, second, 33 * ms);
    stalled.request(now, {recording}, 0);
    stalled.request(now + 20 * ms, {recording}, 0);
    stalled.request(now + 40 * ms, {recording}, 0);
    now = stalled.request(now + 60 * ms + 3 * second, {preview}, 0);
    require(stalled.closes(preview) == 0 && stalled.opens(preview) == 1, "the request that uses the stream keeps it");
}

// What the 0.5 second rule is for: the stream of an output that stays
// configured but leaves the requests (a recording that stopped; the preview of
// a client that moved to other outputs) goes soon after, and a client that sends
// requests slowly does not keep it for long. A reconfiguration is not this
// case: it makes a new frame source.
void check_an_output_that_leaves_the_requests()
{
    Hal hal;
    std::int64_t now = hal.preview_requests(0, 2 * second, 33 * ms, {preview, recording});
    const std::int64_t last = hal.last_used(recording);
    hal.preview_requests(now, now + 2 * second, 33 * ms);
    require(!hal.open(recording) && hal.closes(recording) == 1, "the recording's stream is closed once it left the requests");
    const std::int64_t gone = hal.closed_at(recording) - last;
    require(gone > idle_output_limit_ns, "not before its half second is over");
    require(gone <= idle_output_limit_ns + 2 * 33 * ms, "and at the first request after it");
    require(hal.open(preview) && hal.closes(preview) == 0, "the outputs that stay are untouched");

    // The same, with a burst of stills right after the recording stopped: the stills count nothing, so the stream
    // goes at the second still-free request after the half second, and the preview that waited behind the burst stays.
    Hal after_burst;
    now = after_burst.preview_requests(0, 2 * second, 33 * ms, {preview, recording});
    now = after_burst.burst(now, 5, 300 * ms);
    require(after_burst.open(recording), "the stills closed nothing");
    const std::int64_t resumed = now;
    after_burst.request(now, {preview}, 0);
    require(after_burst.open(recording), "one still-free request without it is not enough");
    after_burst.request(now + 33 * ms, {preview}, 0);
    require(after_burst.open(recording), "nor two: the third finds it absent from two");
    after_burst.request(now + 66 * ms, {preview}, 0);
    require(!after_burst.open(recording) && after_burst.closes(recording) == 1, "the stream goes once still-free requests resume");
    require(after_burst.closed_at(recording) >= resumed, "not before the burst ended");
    require(after_burst.closes(preview) == 0, "the preview stayed");

    // A slow client (a request every 400 ms): the stream goes at the first request that has the requests and the time.
    Hal slow;
    now = slow.preview_requests(0, 2 * second, 400 * ms, {preview, recording});
    const std::int64_t slow_last = slow.last_used(recording);
    slow.preview_requests(now, now + 4 * second, 400 * ms);
    require(slow.closes(recording) == 1, "a dropped stream closes with slow requests too");
    require(slow.closed_at(recording) - slow_last <= 3 * 400 * ms, "after the two requests that the rule asks for");

    // The stream of a still that the client stopped asking for goes after its five seconds.
    Hal stills;
    now = stills.preview_requests(0, second, 33 * ms);
    now = stills.request(now, {jpeg}, 300 * ms) + 33 * ms;
    const std::int64_t still_last = stills.last_used(jpeg);
    stills.preview_requests(now, now + 10 * second, 33 * ms);
    require(stills.closes(jpeg) == 1, "an unused still's stream is closed");
    require(stills.closed_at(jpeg) - still_last > idle_stream_limit_ns, "after five seconds");
    require(stills.closed_at(jpeg) - still_last <= idle_stream_limit_ns + 33 * ms, "and at the first request after them");
}

// Two outputs that need one size have a stream each: dropping one leaves the other.
void check_two_outputs()
{
    Hal hal;
    std::int64_t now = hal.preview_requests(0, 2 * second, 33 * ms, {preview, recording});
    hal.preview_requests(now, now + 2 * second, 33 * ms, {recording});
    require(hal.closes(preview) == 1 && hal.closes(recording) == 0, "the dropped output's stream goes, the other's stays");
    require(hal.open(recording) && !hal.open(preview), "the recording goes on");
}

} // namespace

int main()
{
    check_preview_behind_a_still();
    check_a_burst_of_stills();
    check_a_mixed_burst();
    check_boundary();
    check_an_output_that_leaves_the_requests();
    check_two_outputs();
    return 0;
}
