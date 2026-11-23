// SPDX-License-Identifier: GPL-2.0-or-later
// What the application does when it leaves the foreground with a camera video running (`BackgroundVideo`, and
// `holdAfterVideoEnds`, which orders it before the hold of the guest's GPU requests): the guest's video ends, the
// application waits (bounded) until the guest has stopped its video streams, and only then holds the GPU. The device that
// ends the video and sees the guest stop it has its own rows (camera_device_tests.cpp); here the device is a thread that
// answers as it does, and the hold and the video are fakes.

#include "background_video.hpp"
#include "require.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using uml::camera::BackgroundVideo;
using uml::testing::require;
using Clock = BackgroundVideo::Clock;

std::chrono::milliseconds since(Clock::time_point began)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began);
}

/* The device's worker, as far as `BackgroundVideo` can tell: it takes the request when it runs, answers after `answerAfter`
 * with `streams` to stop, and lets the guest stop them one by one `stopAfter` after the answer (never, if it is zero). */
class FakeDevice final {
public:
    FakeDevice(BackgroundVideo& control, std::chrono::milliseconds answerAfter, std::uint32_t streams, std::chrono::milliseconds stopAfter)
        : control_(control)
    {
        control_.attach([this] { requested_.store(true); });
        thread_ = std::thread([=, this] {
            std::uint64_t request = 0;
            while (!quit_.load()) {
                if (requested_.exchange(false) && control_.take(request)) {
                    std::this_thread::sleep_for(answerAfter);
                    control_.ended(request, streams);
                    answered_.store(true);
                    if (stopAfter.count() != 0) {
                        for (std::uint32_t left = streams; left > 0; --left) {
                            std::this_thread::sleep_for(stopAfter);
                            control_.remaining(left - 1);
                        }
                    }
                }
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~FakeDevice()
    {
        quit_.store(true);
        thread_.join();
        control_.detach();
    }
    bool answered() const { return answered_.load(); }

private:
    BackgroundVideo& control_;
    std::atomic<bool> requested_ {}, quit_ {}, answered_ {};
    std::thread thread_;
};

void withNoDeviceTheCallEndsAndWaitsForNothing()
{
    BackgroundVideo control;
    const auto began = Clock::now();
    const auto outcome = control.end(began + 5s);
    require(outcome.answered && outcome.stopped && outcome.streams == 0, "no device: nothing ended");
    require(since(began) < 200ms, "and no wait");
}

void theCallReturnsWhenTheGuestHasStoppedItsStreams()
{
    BackgroundVideo control;
    FakeDevice device(control, 20ms, 2, 40ms);
    const auto began = Clock::now();
    const auto outcome = control.end(began + 5s);
    require(outcome.answered && outcome.stopped && outcome.streams == 2, "two streams were ended and then stopped");
    require(since(began) >= 100ms, "the call waited for both stops (20 ms, then two of 40 ms)");
    require(since(began) < 2s, "and not for the deadline");
}

void aGuestWithNoRunningVideoIsNotWaitedFor()
{
    BackgroundVideo control;
    FakeDevice device(control, 0ms, 0, 0ms);
    const auto began = Clock::now();
    const auto outcome = control.end(began + 5s);
    require(outcome.answered && outcome.stopped && outcome.streams == 0, "nothing runs");
    require(since(began) < 1s, "the call does not wait for the deadline");
}

void aGuestThatDoesNotStopHoldsTheCallForTheDeadlineOnly()
{
    BackgroundVideo control;
    FakeDevice device(control, 0ms, 1, 0ms);
    const auto began = Clock::now();
    const auto outcome = control.end(began + 300ms);
    require(outcome.answered && !outcome.stopped && outcome.streams == 1, "the stream was not stopped");
    require(since(began) >= 280ms && since(began) < 1500ms, "the call took the deadline");
}

/* The answer and the stop share one deadline: a device that answers late leaves the wait for the stop what is left. */
void theAnswerAndTheStopShareTheDeadline()
{
    BackgroundVideo control;
    FakeDevice device(control, 150ms, 1, 0ms);
    const auto began = Clock::now();
    const auto outcome = control.end(began + 300ms);
    require(outcome.answered && !outcome.stopped && outcome.streams == 1, "answered, not stopped");
    require(since(began) >= 280ms && since(began) < 420ms, "within the one deadline, not after a second wait of its own");
}

void aDeviceThatDoesNotAnswerIsSaid()
{
    BackgroundVideo control;
    control.attach([] {});
    const auto began = Clock::now();
    const auto outcome = control.end(began + 200ms);
    require(!outcome.answered && !outcome.stopped && outcome.streams == 0, "an unanswered request");
    require(since(began) >= 180ms && since(began) < 1500ms, "at the deadline");
    control.detach();
}

void aDeviceThatGoesAwayEndsTheWait()
{
    BackgroundVideo control;
    std::thread away;
    {
        FakeDevice device(control, 0ms, 1, 0ms);
        away = std::thread([&] {
            std::this_thread::sleep_for(150ms);
            control.detach();
        });
        const auto began = Clock::now();
        const auto outcome = control.end(began + 5s);
        require(outcome.stopped, "nothing is left to wait for");
        require(since(began) < 2s, "the wait ended with the device");
        away.join();
    }
}

void callsSerialize()
{
    BackgroundVideo control;
    FakeDevice device(control, 30ms, 1, 30ms);
    std::atomic<unsigned> stopped {};
    std::vector<std::thread> callers;
    for (unsigned index = 0; index < 3; ++index)
        callers.emplace_back([&] {
            const auto outcome = control.end(Clock::now() + 5s);
            if (outcome.answered && outcome.stopped) ++stopped;
        });
    for (auto& caller : callers) caller.join();
    require(stopped.load() == 3, "every call came back");
}

/* A hold that lands while a call waits ends the wait: the guest cannot act on the video once its GPU requests are held. */
void anAbandonedWaitReturnsAtOnce()
{
    BackgroundVideo control;
    FakeDevice device(control, 10ms, 1, 0ms);
    BackgroundVideo::Outcome outcome;
    const auto began = Clock::now();
    std::thread caller([&] { outcome = control.end(Clock::now() + 8s); });
    while (!device.answered()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(50ms);
    control.abandon();
    caller.join();
    require(outcome.abandoned && !outcome.stopped && outcome.answered && outcome.streams == 1, "the wait was cut short");
    require(since(began) < 2s, "at once, not at the deadline");
}

/* An abandon with no call in progress does nothing, and a call that comes later waits as any does. */
void anAbandonWithNoCallInProgressChangesNothing()
{
    BackgroundVideo control;
    FakeDevice device(control, 5ms, 1, 40ms);
    control.abandon();
    const auto outcome = control.end(Clock::now() + 5s);
    require(!outcome.abandoned && outcome.answered && outcome.stopped && outcome.streams == 1, "the next call waits for the stop");
}

/* The edge's main path waits for the guest to stop its video, the bound's hold lands first (the edge is already late), and the
 * wait ends: the main path's own hold then holds what is held, and nothing is told. */
void aHoldThatLandsFirstEndsThePendingWaitHarmlessly()
{
    BackgroundVideo control;
    FakeDevice device(control, 5ms, 2, 0ms);
    std::mutex mutex;
    std::vector<std::string> events;
    const auto note = [&](const char* what) {
        std::lock_guard lock(mutex);
        events.emplace_back(what);
    };
    std::vector<std::string> log;
    std::atomic<bool> held {};
    const auto began = Clock::now();
    BackgroundVideo::Outcome outcome;
    std::thread mainPath([&] {
        const auto result = uml::camera::holdAfterVideoEnds(
            control, [&] { note("hold"); return held.load() ? 1 : 0; }, 8s, [&](const std::string& line) { log.push_back(line); });
        outcome = result.video;
    });
    while (!device.answered()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(50ms);
    {
        // The other path: the hold at the bound lands at once, and the pending wait is told.
        note("late hold");
        held.store(true);
        control.abandon();
    }
    mainPath.join();
    require((events == std::vector<std::string> {"late hold", "hold"}), "the late hold first, the main path's hold after it");
    require(outcome.abandoned && outcome.streams == 2, "the main path's wait was abandoned");
    require(log.empty(), "and says nothing");
    require(since(began) < 3s, "without waiting for the bound");
}

/* The ordering of the edge, with a fake video and a fake hold. */
struct FakeVideo {
    std::vector<std::string>* events {};
    BackgroundVideo::Outcome outcome;
    /* How long `end` takes: until the deadline when it is negative. */
    std::chrono::milliseconds takes {0};
    std::vector<Clock::time_point> deadlines;
    BackgroundVideo::Outcome end(Clock::time_point deadline)
    {
        deadlines.push_back(deadline);
        events->push_back("end");
        if (takes.count() < 0) std::this_thread::sleep_until(deadline);
        else std::this_thread::sleep_for(takes);
        events->push_back("ended");
        return outcome;
    }
};

struct EdgeRun {
    std::vector<std::string> events;
    std::vector<std::string> log;
    Clock::time_point began, held;
    int answer {};
    BackgroundVideo::Outcome video;
    std::vector<Clock::time_point> deadlines;
};

EdgeRun runTheEdge(BackgroundVideo::Outcome outcome, std::chrono::milliseconds takes, std::chrono::milliseconds bound)
{
    EdgeRun run;
    FakeVideo video {&run.events, outcome, takes, {}};
    run.began = Clock::now();
    const auto result = uml::camera::holdAfterVideoEnds(
        video,
        [&] {
            run.held = Clock::now();
            run.events.push_back("hold");
            return 7;
        },
        bound, [&](const std::string& line) { run.log.push_back(line); });
    run.answer = result.held;
    run.video = result.video;
    run.deadlines = video.deadlines;
    return run;
}

void aRunningVideoIsEndedAndWaitedForBeforeTheHold()
{
    const auto run = runTheEdge({1, true, true}, 60ms, 2s);
    require((run.events == std::vector<std::string> {"end", "ended", "hold"}), "end, wait, hold: in that order");
    require(run.answer == 7 && run.video.streams == 1, "the hold's answer and the video's are given back");
    require(run.log.empty(), "nothing to say");
    require(std::chrono::duration_cast<std::chrono::milliseconds>(run.held - run.began) >= 50ms, "the hold came after the wait");
}

void noVideoMeansTheHoldAtOnce()
{
    const auto run = runTheEdge({0, true, true}, 0ms, 2s);
    require((run.events == std::vector<std::string> {"end", "ended", "hold"}), "the hold follows at once");
    require(std::chrono::duration_cast<std::chrono::milliseconds>(run.held - run.began) < 500ms, "with no wait");
    require(run.log.empty(), "and no line");
}

void aBoundThatRunsOutHoldsAndSaysSoOnce()
{
    const auto stopped = runTheEdge({2, false, true}, -1ms, 200ms);
    require((stopped.events == std::vector<std::string> {"end", "ended", "hold"}), "the GPU is held all the same");
    require(stopped.log.size() == 1 && stopped.log[0].find("2 video stream(s)") != std::string::npos &&
                stopped.log[0].find("200 ms") != std::string::npos,
            "one line, with the streams and the bound");
    const auto silent = runTheEdge({0, false, false}, -1ms, 200ms);
    require((silent.events == std::vector<std::string> {"end", "ended", "hold"}) && silent.log.size() == 1, "an unanswered request too: one line");
}

/* The wait is within the edge's own budget, whose clock starts at the edge, not at the call: a display wait of 150 ms comes
 * first, the bound is what a 400 ms edge budget leaves after it and after the 100 ms the hold may spend on a running
 * request, and the hold follows the wait without a second one. A guest whose video never stops holds it to the bound. */
void theWaitIsWithinTheBoundAndNeverAddedToIt()
{
    constexpr auto budget = 400ms;
    constexpr auto drain = 100ms;
    const auto edge = Clock::now();
    std::this_thread::sleep_for(150ms);
    BackgroundVideo control;
    FakeDevice device(control, 0ms, 1, 0ms);
    const auto bound = budget - since(edge) - drain;
    Clock::time_point held;
    std::vector<std::string> log;
    const auto result = uml::camera::holdAfterVideoEnds(
        control, [&] { held = Clock::now(); return 7; }, bound, [&](const std::string& line) { log.push_back(line); });
    require(result.video.answered && !result.video.stopped && result.video.streams == 1, "the stream never stopped");
    require(log.size() == 1, "and it is said once");
    const auto after = std::chrono::duration_cast<std::chrono::milliseconds>(held - edge);
    require(after >= budget - drain - 30ms, "the hold waited for the video to the end of the bound");
    require(after <= budget - drain + 60ms, "and landed there at the latest, so the drain ends inside the budget");
}

/* A call that is queued behind another keeps its own deadline: the one before it may be waiting for a device that does not
 * answer, and the caller of the later one has less time than that. */
void aCallQueuedBehindAnotherEndsAtItsOwnDeadline()
{
    BackgroundVideo control;
    std::atomic<bool> asked {};
    control.attach([&] { asked.store(true); });
    std::thread first([&] { (void)control.end(Clock::now() + 600ms); });
    const auto until = Clock::now() + 5s;
    while (!asked.load() && Clock::now() < until) std::this_thread::sleep_for(1ms);
    require(asked.load(), "the first call asked the device");
    const auto began = Clock::now();
    bool held = false;
    const auto result = uml::camera::holdAfterVideoEnds(
        control, [&] { held = true; return 1; }, 50ms, [](const std::string&) {});
    const auto& outcome = result.video;
    require(!outcome.answered && !outcome.stopped && outcome.streams == 0, "the queued call was not answered");
    require(since(began) >= 40ms && since(began) < 300ms, "and returned at its own deadline, not after the first call's");
    require(held && result.held == 1, "and its caller held the gate, as it does after any wait that ran out");
    first.join();
    control.detach();
}

} // namespace

int main(int argc, char** argv)
{
    /* A row's name on the command line runs that row alone. */
    const char* const only = argc > 1 ? argv[1] : nullptr;
#define ROW(name) \
    if (only == nullptr || std::string(only) == #name) name()
    ROW(withNoDeviceTheCallEndsAndWaitsForNothing);
    ROW(theCallReturnsWhenTheGuestHasStoppedItsStreams);
    ROW(aGuestWithNoRunningVideoIsNotWaitedFor);
    ROW(aGuestThatDoesNotStopHoldsTheCallForTheDeadlineOnly);
    ROW(theAnswerAndTheStopShareTheDeadline);
    ROW(aDeviceThatDoesNotAnswerIsSaid);
    ROW(aDeviceThatGoesAwayEndsTheWait);
    ROW(callsSerialize);
    ROW(anAbandonedWaitReturnsAtOnce);
    ROW(anAbandonWithNoCallInProgressChangesNothing);
    ROW(aHoldThatLandsFirstEndsThePendingWaitHarmlessly);
    ROW(aRunningVideoIsEndedAndWaitedForBeforeTheHold);
    ROW(noVideoMeansTheHoldAtOnce);
    ROW(aBoundThatRunsOutHoldsAndSaysSoOnce);
    ROW(theWaitIsWithinTheBoundAndNeverAddedToIt);
    ROW(aCallQueuedBehindAnotherEndsAtItsOwnDeadline);
#undef ROW
    return 0;
}
