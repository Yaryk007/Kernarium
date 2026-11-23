// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The application's hold of the guest's GPU requests (kernarium_runtime_set_gpu_hold) as the runtime runs it
 * (KernariumGpuHold.hpp), over the real request gate, the real camera control (`BackgroundVideo`) and the real run loop's
 * Lifecycle: how long a hold waits for the guest's camera when its caller's deadline leaves little, what happens when the
 * edge's own bound and the hold that waits for the camera meet, and that a guest that restarts while a hold is pending
 * does not boot behind the gate.
 *
 * The times are the application's, halved, with the edge's own: a drain of 0.5 s for a request that runs, a camera bound
 * of 1 s, an edge budget of 2 s, and the bound that holds a kept-running job's GPU 0.75 s after the edge. Every row's
 * clock starts at the edge, not at the call of the hold.
 */
#include "KernariumGpuHold.hpp"
#include "KernariumRestartLoop.hpp"
#include "camera/background_video.hpp"
#include "require.hpp"
#include "vhost_user/request_gate.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kernarium::gpu_hold::Gate;
using kernarium::gpu_hold::Timing;
using uml::camera::BackgroundVideo;
using uml::testing::require;
using Clock = BackgroundVideo::Clock;

constexpr std::chrono::milliseconds edgeBudget {2000};
constexpr std::chrono::milliseconds boundAfterTheEdge {750};
constexpr Timing halfTiming {500ms, 1000ms};

std::chrono::milliseconds since(Clock::time_point began)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began);
}

/* The in-process GPU service as the gate uses it, over the real request gate. `beforeClosing` lets a row decide when the
 * call that closes the gate gets to do it. */
class FakeGpu final {
public:
    bool holdRequests(std::chrono::milliseconds budget) noexcept
    {
        if (beforeClosing) beforeClosing();
        std::int64_t unset = -1;
        closedAt.compare_exchange_strong(unset, Clock::now().time_since_epoch().count());
        return gate.hold(budget);
    }
    void releaseRequests() noexcept { gate.release(); }
    [[nodiscard]] bool requestsHeld() const noexcept { return gate.held(); }

    uml::gpu::RequestGate gate;
    std::function<void()> beforeClosing;
    /* When the first call that closes the gate began, in the clock's own count; -1 before. */
    std::atomic<std::int64_t> closedAt {-1};

    [[nodiscard]] std::chrono::milliseconds closedAfter(Clock::time_point edge) const
    {
        const auto at = closedAt.load();
        require(at >= 0, "the gate was never closed");
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point(Clock::duration(at)) - edge);
    }
};

/* What the runtime keeps and the gate works on. */
struct Runtime final {
    explicit Runtime(Timing timing = {}) : gate(mutex, active, lifecycle, timing) { active = &gpu; }
    std::mutex mutex;
    FakeGpu gpu;
    FakeGpu* active {};
    kernarium::restart::Lifecycle lifecycle;
    Gate<FakeGpu> gate;
};

/* The guest's camera layer for a recording that never stops: it answers the request to end the video, and leaves the
 * streams running. */
class StubbornCamera final {
public:
    explicit StubbornCamera(BackgroundVideo& control) : control_(control)
    {
        control_.attach([this] { requested_.store(true); });
        thread_ = std::thread([this] {
            std::uint64_t request = 0;
            while (!quit_.load()) {
                if (requested_.exchange(false) && control_.take(request)) control_.ended(request, 1);
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~StubbornCamera()
    {
        quit_.store(true);
        thread_.join();
        control_.detach();
    }

private:
    BackgroundVideo& control_;
    std::atomic<bool> requested_ {};
    std::atomic<bool> quit_ {};
    std::thread thread_;
};

/* A camera control that answers at once, and records what it was asked. */
struct RecordingVideo final {
    BackgroundVideo::Outcome end(Clock::time_point deadline)
    {
        deadlines.push_back(deadline);
        return {1, true, true};
    }
    void abandon() { ++abandons; }
    std::vector<Clock::time_point> deadlines;
    int abandons {};
};

const auto saysNothing = [](const std::string&) {};

void sleepUntil(Clock::time_point when) { std::this_thread::sleep_until(when); }

/* ---- What the camera's wait is. */

void aDeadlineLeavesTheCameraWhatIsLeftAfterTheDrain()
{
    const Timing timing {1000ms, 2000ms};
    const auto wait = [&](int held) { return std::chrono::duration_cast<std::chrono::milliseconds>(kernarium::gpu_hold::cameraWait(held, timing)); };
    require(wait(1) == 2000ms, "a hold with no deadline waits for the camera's own bound");
    require(wait(3) == 0ms && wait(1000) == 0ms, "a deadline that leaves no more than the drain leaves the camera nothing");
    require(wait(1100) == 100ms, "and one that leaves a little more, a little");
    require(wait(2500) == 1500ms, "2.5 s left: the drain's second, and 1.5 s for the camera");
    require(wait(3000) == 2000ms && wait(4000) == 2000ms, "and never more than the camera's bound");
}

void aHoldWithADeadlineWaitsForTheCameraOnlyWithinIt()
{
    Runtime runtime {Timing {1000ms, 2000ms}};
    RecordingVideo video;
    auto began = Clock::now();
    (void)runtime.gate.set(2500, &video, saysNothing);
    require(video.deadlines.size() == 1, "the video was asked once");
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(video.deadlines[0] - began);
    require(wait >= 1500ms && wait < 1700ms, "within what the deadline leaves after the drain: 1.5 s, not the camera's 2 s");
    require(runtime.gpu.requestsHeld(), "and the gate was held");

    RecordingVideo bounded;
    began = Clock::now();
    (void)runtime.gate.set(1, &bounded, saysNothing);
    wait = std::chrono::duration_cast<std::chrono::milliseconds>(bounded.deadlines.at(0) - began);
    require(wait >= 2000ms && wait < 2200ms, "a hold with no deadline waits for the camera's own bound");

    RecordingVideo late;
    (void)runtime.gate.set(900, &late, saysNothing);
    require(late.deadlines.empty() && late.abandons == 0, "a deadline with nothing left after the drain does not wait for the camera");
    require(runtime.gpu.requestsHeld() && runtime.lifecycle.gpuHoldRequested(), "and holds");

    RecordingVideo bound;
    (void)runtime.gate.set(kernarium::gpu_hold::atOnce, &bound, saysNothing);
    require(bound.deadlines.empty() && bound.abandons == 1, "the bound's hold waits for nothing and ends a wait that is pending");

    /* A deadline that leaves less than the drain does not shorten the drain: on a gate that is not held yet, with a request
     * that runs, the gate waits its whole drain and the camera is not asked. */
    Runtime fresh {Timing {500ms, 1000ms}};
    std::optional<uml::gpu::RequestGate::Entry> stuck;
    stuck.emplace(fresh.gpu.gate.enter());
    require(bool(*stuck), "a request is running");
    RecordingVideo none;
    const auto freshBegan = Clock::now();
    const int outcome = fresh.gate.set(300, &none, saysNothing);
    const auto took = since(freshBegan);
    require(none.deadlines.empty(), "nothing is left for the camera");
    require(outcome == 0 && fresh.gpu.requestsHeld(), "the gate is held, with the request still running");
    require(took >= 450ms && took < 800ms, "after the whole drain, not after the 300 ms the deadline left");
}

/* A guest with no camera (the desktop Linux guest) is held and released as any, with no wait. */
void aGuestWithNoCameraIsHeldAndReleasedWithoutAWait()
{
    Runtime runtime {Timing {1000ms, 2000ms}};
    const auto began = Clock::now();
    require(runtime.gate.set(kernarium::gpu_hold::withCamera, nullptr, saysNothing) == 1, "the hold is in effect");
    require(runtime.gpu.requestsHeld() && runtime.lifecycle.gpuHoldRequested(), "the gate is held and the hold is asked for");
    require(since(began) < 500ms, "with no wait for a camera");
    require(runtime.gate.set(kernarium::gpu_hold::atOnce, nullptr, saysNothing) == 1, "a hold at once of a held gate is in effect");
    require(runtime.gate.set(kernarium::gpu_hold::released, nullptr, saysNothing) == 1, "the requests are given back");
    require(!runtime.gpu.requestsHeld() && !runtime.lifecycle.gpuHoldRequested(), "the gate is open and nothing is asked for");
    runtime.active = nullptr;
    require(runtime.gate.set(kernarium::gpu_hold::withCamera, nullptr, saysNothing) == -1, "no guest runs: the hold waits for the next");
    require(runtime.lifecycle.gpuHoldRequested(), "and is kept");
    require(runtime.gate.set(kernarium::gpu_hold::released, nullptr, saysNothing) == -1 && !runtime.lifecycle.gpuHoldRequested(),
            "and given back");
}

/* Two holds that meet wait for a request that runs once, and the second is told what the first's wait found: the request
 * outlasted the drain (0), and still does. */
void aSecondHoldOfAGateThatDidNotDrainIsToldSo()
{
    Runtime runtime {Timing {300ms, 1000ms}};
    std::optional<uml::gpu::RequestGate::Entry> stuck;
    stuck.emplace(runtime.gpu.gate.enter());
    require(bool(*stuck), "a request is running");

    int first = -2;
    int second = -2;
    const auto began = Clock::now();
    std::thread closer([&] { first = runtime.gate.set(kernarium::gpu_hold::atOnce, nullptr, saysNothing); });
    std::this_thread::sleep_for(60ms);
    require(runtime.gpu.requestsHeld(), "the first hold has closed the gate and waits for the request");
    std::thread meeting([&] { second = runtime.gate.set(kernarium::gpu_hold::atOnce, nullptr, saysNothing); });
    closer.join();
    meeting.join();
    require(first == 0, "the first hold found the request still running after the drain");
    require(second == 0, "and the hold that met it is told the same, not that the requests are drained");
    require(since(began) < 300ms + 250ms, "after one wait for the request, not two");
    require(runtime.gate.set(kernarium::gpu_hold::atOnce, nullptr, saysNothing) == 0, "a later hold of the held gate is told so as well");

    /* The requests are given back and the request ends: the next hold finds nothing to wait for. */
    (void)runtime.gate.set(kernarium::gpu_hold::released, nullptr, saysNothing);
    stuck.reset();
    require(runtime.gate.set(kernarium::gpu_hold::atOnce, nullptr, saysNothing) == 1, "a gate that drained is held with 1");
    require(runtime.gate.set(kernarium::gpu_hold::atOnce, nullptr, saysNothing) == 1, "and so is a hold that meets it");
}

/* ---- The edge's own bound and the hold that waits for the camera. A job whose display stays on, a video that never
 * stops and one GPU request that never ends: the clock starts at the edge. */

struct EdgeRun final {
    std::chrono::milliseconds closed {};
    std::chrono::milliseconds returned {};
};

/* `timerAt` and `mainAt`: when, after the edge, the bound's hold and the main path's hold begin (the main path has waited
 * for the display, a time at most the bound's). A negative time leaves the call out. */
EdgeRun theEdge(std::chrono::milliseconds timerAt, std::chrono::milliseconds mainAt)
{
    Runtime runtime {halfTiming};
    BackgroundVideo video;
    StubbornCamera camera {video};
    auto stuck = runtime.gpu.gate.enter();
    require(bool(stuck), "a request is running");

    const auto edge = Clock::now();
    std::thread bound;
    if (timerAt >= 0ms) {
        bound = std::thread([&] {
            sleepUntil(edge + timerAt);
            (void)runtime.gate.set(kernarium::gpu_hold::atOnce, &video, saysNothing);
        });
    }
    EdgeRun run;
    if (mainAt >= 0ms) {
        sleepUntil(edge + mainAt);
        const auto left = edgeBudget - since(edge);
        (void)runtime.gate.set(static_cast<int>(left.count()), &video, saysNothing);
        run.returned = since(edge);
    }
    if (bound.joinable()) bound.join();
    run.closed = runtime.gpu.closedAfter(edge);
    require(runtime.gpu.requestsHeld() && runtime.lifecycle.gpuHoldRequested(), "the gate is held and the hold is asked for");
    return run;
}

void theBoundHoldsByItsTimeAndTheMainPathEndsInsideTheEdge()
{
    /* The bound fires first, and the main path meets its hold already in progress. */
    const auto timerFirst = theEdge(boundAfterTheEdge, boundAfterTheEdge + 20ms);
    require(timerFirst.closed <= boundAfterTheEdge + 150ms, "the bound closes the gate at its time, not after another camera wait");
    require(timerFirst.returned < edgeBudget, "and the main path is back inside the edge's deadline");
    require(timerFirst.returned <= boundAfterTheEdge + halfTiming.drain + 200ms, "as soon as the bound's drain ends: a drain that is waited for once");
    /* The main path takes its hold first: the bound's lands while it waits for the camera, and ends that wait. */
    const auto mainFirst = theEdge(boundAfterTheEdge, boundAfterTheEdge - 20ms);
    require(mainFirst.closed <= boundAfterTheEdge + 150ms, "the bound closes the gate at its time while the main path waits");
    require(mainFirst.returned < edgeBudget, "and the main path, whose wait it ended, is back inside the edge's deadline");
    require(mainFirst.returned <= boundAfterTheEdge + halfTiming.drain + 200ms, "as soon as the bound's drain ends, not when its own wait would have");
}

void aMainPathThatCameLateWaitsForTheCameraOnlyWhatTheEdgeLeaves()
{
    /* No bound fires (it was withdrawn, or its thread has not run): the main path alone, after a display wait of the
     * bound's length. What the edge leaves after the drain is 1.25 s - 0.5 s = 0.75 s, not the camera's second. */
    const auto run = theEdge(-1ms, boundAfterTheEdge);
    require(run.closed >= boundAfterTheEdge + 600ms, "the main path waited for the camera");
    require(run.closed <= edgeBudget - 500ms + 150ms, "but only until the edge budget less the drain, so the drain ends inside the edge");
    /* The allowance is nominal: the camera's wait and the drain together use what the edge left, so the hold is back at
     * the deadline, to within the wake-ups of its timers. */
    require(run.returned <= edgeBudget + 150ms, "and the hold is back at the edge's deadline");
    require(run.returned <= run.closed + halfTiming.drain + 200ms, "after the one drain the request that runs costs");
}

/* ---- A guest that restarts while the hold waits for the camera does not boot behind the gate. */

class RestartingGuest final : public kernarium::restart::Machine {
public:
    /* When the application's hold arrives: while the old guest runs (its camera is still to answer), or while the
     * restarted guest's boot is being built. `gateClosesBeforeAdmission` is for the second: the hold's close has landed
     * by the time the loop would run the guest. */
    enum class When { whileTheOldGuestRuns, whileTheNextBootIsBuilt };

    explicit RestartingGuest(When when = When::whileTheOldGuestRuns, bool gateClosesBeforeAdmission = false)
        : when_(when), gateClosesBeforeAdmission_(gateClosesBeforeAdmission)
    {
        runtime.gpu.beforeClosing = [this] {
            const auto until = Clock::now() + 5s;
            while (!letTheGateClose_.load() && Clock::now() < until) std::this_thread::sleep_for(1ms);
        };
    }
    ~RestartingGuest() override
    {
        letTheGateClose_.store(true);
        if (hold_.joinable()) hold_.join();
        video.detach();
    }

    /* The application asks for the hold; it waits for the guest's camera first. */
    void askForTheHold()
    {
        hold_ = std::thread([this] { (void)runtime.gate.set(kernarium::gpu_hold::withCamera, &video, saysNothing); });
    }
    void letTheGateClose()
    {
        letTheGateClose_.store(true);
        if (hold_.joinable()) hold_.join();
    }

    bool startBoot(unsigned number) override
    {
        if (when_ == When::whileTheNextBootIsBuilt && number == 1) {
            /* The application leaves the foreground while the restarted guest's services are being made: the old
             * guest's wait is over and the loop's guard has looked already. */
            askForTheHold();
            const auto until = Clock::now() + 5s;
            while (!runtime.lifecycle.gpuHoldRequested() && Clock::now() < until) std::this_thread::sleep_for(1ms);
            require(runtime.lifecycle.gpuHoldRequested(), "the hold is asked for while the boot is built");
            if (gateClosesBeforeAdmission_) letTheGateClose();
        }
        started.store(number + 1);
        return true;
    }
    int enter() override
    {
        entered.fetch_add(1);
        if (started.load() == 1 && when_ == When::whileTheNextBootIsBuilt) return 0;
        if (started.load() == 1) {
            video.attach([] {});
            askForTheHold();
            /* The guest's camera layer answers, and the guest restarts. */
            std::uint64_t request = 0;
            const auto until = Clock::now() + 5s;
            while (!video.take(request) && Clock::now() < until) std::this_thread::sleep_for(1ms);
            require(request != 0, "the hold ended the guest's video");
            video.ended(request, 1);
            return 0;
        }
        /* The restarted guest's first GPU request. */
        enteredWhilePending = runtime.lifecycle.gpuHoldRequested();
        enteredWithGateHeld = runtime.gpu.requestsHeld();
        enteredAdmitted = bool(runtime.gpu.gate.enter());
        return 0;
    }
    bool runEnd(kernarium::restart::RunEnd& end) override
    {
        end.kind = started.load() == 1 ? kernarium::restart::RunEnd::Kind::restart : kernarium::restart::RunEnd::Kind::powerOff;
        return true;
    }
    bool startRefusedForMemory() noexcept override { return false; }
    /* The old guest's camera goes with it, which ends the wait that was for it. */
    void stopBoot() noexcept override { video.detach(); }
    void resetLink() noexcept override {}
    bool stopRequested() noexcept override { return false; }
    bool bootHeld() noexcept override { return runtime.lifecycle.held(); }
    void waitWhileHeld(std::chrono::milliseconds slice) override
    {
        waits.fetch_add(1);
        runtime.lifecycle.waitWhileHeld(slice);
    }
    void pause(std::chrono::milliseconds delay) override { std::this_thread::sleep_for(delay); }
    kernarium::restart::Clock::time_point now() noexcept override { return kernarium::restart::Clock::now(); }
    void say(const std::string&) noexcept override {}
    void restarting(unsigned) noexcept override {}
    bool consoleAnswers() noexcept override { return true; }
    void booted(unsigned, bool) noexcept override {}

    Runtime runtime {halfTiming};
    BackgroundVideo video;
    std::atomic<unsigned> started {};
    std::atomic<unsigned> entered {};
    std::atomic<unsigned> waits {};
    bool enteredWithGateHeld {};
    bool enteredWhilePending {};
    bool enteredAdmitted {};

private:
    const When when_;
    const bool gateClosesBeforeAdmission_;
    std::thread hold_;
    std::atomic<bool> letTheGateClose_ {};
};

void aGuestThatRestartsWhileTheHoldWaitsForTheCameraBootsAfterTheRelease()
{
    RestartingGuest guest;
    int result = -1;
    std::thread loop([&] {
        result = kernarium::restart::run(guest, {.consolePoll = 1ms, .consoleBound = 20ms, .heldPoll = 1ms});
    });
    const auto until = Clock::now() + 5s;
    /* The loop has the guest restarted and waits at the boot guard, with the hold asked for and the gate still open. */
    while (guest.waits.load() < 3 && Clock::now() < until) std::this_thread::sleep_for(1ms);
    require(guest.waits.load() >= 3, "the restarted guest's boot waits for the hold that is pending");
    require(!guest.runtime.gpu.requestsHeld(), "the gate has not closed yet: the hold waits for the old guest's camera");
    /* The hold's camera wait ended with the old guest's camera; its close lands now, while the boot is held back. */
    guest.letTheGateClose();
    require(guest.runtime.gpu.requestsHeld(), "the gate is closed");
    require(guest.started.load() == 1, "and the restarted guest has not begun to boot behind it");
    /* The application returns. */
    (void)guest.runtime.gate.set(kernarium::gpu_hold::released, &guest.video, saysNothing);
    loop.join();
    require(result == 0, "the launch ends");
    require(guest.started.load() == 2, "the restarted guest boots once the requests are given back");
    require(!guest.enteredWithGateHeld && guest.enteredAdmitted, "and its GPU requests are answered");
}

/* The review's interleaving: the old guest ended and the loop's guard found no hold; the application leaves while the next
 * boot is built, and the loop is about to run the guest. It must not: a guest that boots behind a closed gate (or one that
 * is closing) runs without 3D for the whole boot. `gateClosesBeforeAdmission` is the hold whose close has landed; without
 * it the close still waits for the guest's requests, and only the request for the hold is there. */
void aHoldThatArrivesWhileTheNextBootIsBuiltKeepsTheRestartedGuestOutUntilTheRelease(bool gateClosesBeforeAdmission)
{
    RestartingGuest guest {RestartingGuest::When::whileTheNextBootIsBuilt, gateClosesBeforeAdmission};
    int result = -1;
    std::thread loop([&] {
        result = kernarium::restart::run(guest, {.consolePoll = 1ms, .consoleBound = 20ms, .heldPoll = 1ms});
    });
    const auto until = Clock::now() + 5s;
    while ((guest.started.load() < 2 || (guest.waits.load() < 3 && guest.entered.load() < 2)) && Clock::now() < until)
        std::this_thread::sleep_for(1ms);
    require(guest.started.load() == 2, "the restarted guest's boot was built");
    require(guest.entered.load() == 1 && guest.waits.load() >= 3,
            "the loop waits for the hold that arrived while the boot was built, and has not run the restarted guest");
    require(guest.runtime.gpu.requestsHeld() == gateClosesBeforeAdmission, "the gate is as the hold has left it");
    guest.letTheGateClose();
    require(guest.runtime.gpu.requestsHeld(), "the hold's close lands");
    std::this_thread::sleep_for(30ms);
    require(guest.entered.load() == 1, "and the restarted guest is still not run behind it");
    /* The application returns. */
    (void)guest.runtime.gate.set(kernarium::gpu_hold::released, &guest.video, saysNothing);
    loop.join();
    require(result == 0, "the launch ends");
    require(guest.entered.load() == 2, "the restarted guest runs once the requests are given back");
    require(!guest.enteredWhilePending && !guest.enteredWithGateHeld && guest.enteredAdmitted,
            "with no hold pending, and its GPU requests answered");
}

void aHoldThatArrivesWhileTheNextBootIsBuiltAndHasClosedTheGate()
{
    aHoldThatArrivesWhileTheNextBootIsBuiltKeepsTheRestartedGuestOutUntilTheRelease(true);
}

void aHoldThatArrivesWhileTheNextBootIsBuiltAndHasNotClosedTheGateYet()
{
    aHoldThatArrivesWhileTheNextBootIsBuiltKeepsTheRestartedGuestOutUntilTheRelease(false);
}

} // namespace

int main(int argc, char** argv)
{
    /* A row's name on the command line runs that row alone. */
    const char* const only = argc > 1 ? argv[1] : nullptr;
#define ROW(name) \
    if (only == nullptr || std::string(only) == #name) name()
    ROW(aDeadlineLeavesTheCameraWhatIsLeftAfterTheDrain);
    ROW(aHoldWithADeadlineWaitsForTheCameraOnlyWithinIt);
    ROW(aGuestWithNoCameraIsHeldAndReleasedWithoutAWait);
    ROW(aSecondHoldOfAGateThatDidNotDrainIsToldSo);
    ROW(theBoundHoldsByItsTimeAndTheMainPathEndsInsideTheEdge);
    ROW(aMainPathThatCameLateWaitsForTheCameraOnlyWhatTheEdgeLeaves);
    ROW(aGuestThatRestartsWhileTheHoldWaitsForTheCameraBootsAfterTheRelease);
    ROW(aHoldThatArrivesWhileTheNextBootIsBuiltAndHasClosedTheGate);
    ROW(aHoldThatArrivesWhileTheNextBootIsBuiltAndHasNotClosedTheGateYet);
#undef ROW
    return 0;
}
