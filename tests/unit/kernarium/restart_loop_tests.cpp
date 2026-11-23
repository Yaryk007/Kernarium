// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The run loop of a Kernarium launch, with a recording machine in place of the
 * guest: the order of the steps of a restart, the retry of a refused run, the
 * stop flag, the hold on the next boot, the loop guard (the early window and
 * the boots that never finished) and the runtime's restart events.
 */
#include "KernariumRestartLoop.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kernarium::restart::Lifecycle;
using kernarium::restart::Machine;
using kernarium::restart::RunEnd;
using kernarium::restart::Timing;
using uml::testing::require;
using Clock = uml::emulator::RestartLoopGuard::Clock;

/* One scripted boot: what its run returns, how it ended, and how long it
 * lasted on the fake clock. */
struct Boot final {
    int status {0};
    RunEnd end {RunEnd::Kind::powerOff, ""};
    std::chrono::seconds lasted {60};
    /* The library marked the run as refused for want of memory. */
    bool refusedForMemory {false};
};

Boot powersOff(std::chrono::seconds lasted = 60s)
{
    return {0, {RunEnd::Kind::powerOff, ""}, lasted};
}

Boot restarts(std::string command = "", std::chrono::seconds lasted = 60s)
{
    return {2, {RunEnd::Kind::restart, std::move(command)}, lasted};
}

using uml::emulator::BootCompletion;

/* A restart of an Android guest, whose console says whether its boot had
 * finished. */
Boot restartsAndroid(BootCompletion completion, std::chrono::seconds lasted)
{
    return {2, {RunEnd::Kind::restart, "", completion}, lasted};
}

/* The machine the loop drives. Every step is recorded in `calls`, in order,
 * from whichever thread made it. */
class FakeMachine final : public Machine {
public:
    /* The boots in order; the last one repeats when the script runs out. */
    std::deque<Boot> script;
    /* A restarted boot's first `refusals` runs are refused. */
    unsigned refusals {};
    /* Building the boot numbered this fails (-1: none does). */
    int failStart {-1};
    bool endUnknown {};
    std::atomic_bool stop {};
    std::atomic_bool held {};
    /* The console answers from this poll on; never when 0. */
    unsigned answerFromPoll {};
    /* A restarted boot's run lasts until its console event arrives, or this
     * long. */
    bool runWaitsForBooted {};
    /* Runs after the call is recorded; a test uses it to act at a moment. */
    std::function<void(FakeMachine&, const std::string&)> onCall;

    [[nodiscard]] std::vector<std::string> calls() const
    {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    [[nodiscard]] std::vector<std::string> said() const
    {
        std::lock_guard lock(mutex_);
        return said_;
    }

    [[nodiscard]] bool called(const std::string& call) const
    {
        for (const auto& made : calls())
            if (made == call)
                return true;
        return false;
    }

    [[nodiscard]] std::size_t count(const std::string& prefix) const
    {
        std::size_t total = 0;
        for (const auto& made : calls())
            if (made.starts_with(prefix))
                ++total;
        return total;
    }

    [[nodiscard]] unsigned polls() const { return polls_.load(); }

    bool startBoot(unsigned restarts) override
    {
        record("start " + std::to_string(restarts));
        restarts_ = restarts;
        entered_.store(false);
        return static_cast<int>(restarts) != failStart;
    }

    int enter() override
    {
        record("enter");
        entered_.store(true);
        if (restarts_ != 0 && refused_ < refusals) {
            ++refused_;
            return -UML_EMULATOR_RUNTIME_ERROR;
        }
        {
            std::lock_guard lock(mutex_);
            current_ = script.front();
            if (script.size() > 1U)
                script.pop_front();
            clock_ += current_.lasted;
        }
        if (runWaitsForBooted && restarts_ != 0) {
            std::unique_lock lock(mutex_);
            booted_.wait_for(lock, 10s, [this] { return bootedSeen_; });
        }
        return current_.status;
    }

    bool runEnd(RunEnd& end) override
    {
        record("end");
        end = current_.end;
        return !endUnknown;
    }

    bool startRefusedForMemory() noexcept override
    {
        return current_.refusedForMemory;
    }

    void stopBoot() noexcept override { record("stop-services"); }
    void resetLink() noexcept override { record("reset-link"); }
    bool stopRequested() noexcept override { return stop.load(); }
    bool bootHeld() noexcept override { return held.load(); }

    void waitWhileHeld(std::chrono::milliseconds) override
    {
        record("wait-held");
        std::this_thread::sleep_for(1ms);
    }

    void pause(std::chrono::milliseconds delay) override
    {
        record("pause " + std::to_string(delay.count()));
    }

    Clock::time_point now() noexcept override
    {
        std::lock_guard lock(mutex_);
        return clock_;
    }

    void say(const std::string& message) noexcept override
    {
        std::lock_guard lock(mutex_);
        said_.push_back(message);
    }

    void restarting(unsigned restarts) noexcept override
    {
        record("began " + std::to_string(restarts));
    }

    bool consoleAnswers() noexcept override
    {
        // A guest answers only once its run is under way.
        if (!entered_.load())
            return false;
        const unsigned poll = ++polls_;
        return answerFromPoll != 0U && poll >= answerFromPoll;
    }

    void booted(unsigned restarts, bool answered) noexcept override
    {
        record("booted " + std::to_string(restarts) +
            (answered ? " answered" : " unanswered"));
        {
            std::lock_guard lock(mutex_);
            bootedSeen_ = true;
        }
        booted_.notify_all();
    }

private:
    void record(const std::string& call)
    {
        {
            std::lock_guard lock(mutex_);
            calls_.push_back(call);
        }
        if (onCall)
            onCall(*this, call);
    }

    mutable std::mutex mutex_;
    std::condition_variable booted_;
    std::vector<std::string> calls_;
    std::vector<std::string> said_;
    Boot current_;
    Clock::time_point clock_;
    unsigned restarts_ {};
    unsigned refused_ {};
    std::atomic_uint polls_ {};
    std::atomic_bool entered_ {};
    bool bootedSeen_ {};
};

const Timing quickly {
    .consolePoll = 1ms, .consoleBound = 60s, .heldPoll = 1ms};

[[nodiscard]] bool contains(const std::vector<std::string>& lines,
                            const std::string& line)
{
    for (const auto& candidate : lines)
        if (candidate == line)
            return true;
    return false;
}

using Calls = std::vector<std::string>;

void aGuestThatPowersOffIsBootedOnce()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    require(kernarium::restart::run(machine, quickly) == 0,
            "a power-off did not return the guest's status");
    require((machine.calls() == Calls {"start 0", "enter", "end"}),
            "a power-off ran more than the one boot");
}

void everyEndButARestartEndsTheLaunchWithTheRunsStatus()
{
    for (const auto& boot : {
             Boot {0, {RunEnd::Kind::powerOff, ""}, 60s},
             Boot {0, {RunEnd::Kind::stopped, ""}, 60s},
             Boot {7, {RunEnd::Kind::failed, ""}, 60s},
             Boot {5, {RunEnd::Kind::none, ""}, 60s},
         }) {
        FakeMachine machine;
        machine.script = {boot};
        require(kernarium::restart::run(machine, quickly) == boot.status,
                "an end that is not a restart did not return the status");
        require((machine.calls() == Calls {"start 0", "enter", "end"}),
                "an end that is not a restart went on");
    }
    FakeMachine unknown;
    unknown.script = {restarts()};
    unknown.endUnknown = true;
    require(kernarium::restart::run(unknown, quickly) == 2,
            "a run whose end the library cannot say did not end the loop");
    require((unknown.calls() == Calls {"start 0", "enter", "end"}),
            "a run of unknown end was restarted");
}

void aRestartStopsTheServicesResetsTheLinkAndBootsAgainInThisOrder()
{
    FakeMachine machine;
    machine.script = {restarts("recovery"), powersOff()};
    require(kernarium::restart::run(machine, quickly) == 0,
            "the restarted guest's power-off did not end the launch");
    // The services stop before the library hands the RAM back, which its next
    // run does; the link resets before the next guest's network opens; the
    // event comes after both and before the next boot is built.
    require((machine.calls() == Calls {"start 0", "enter", "end",
                "stop-services", "reset-link", "began 1", "start 1",
                "enter", "end"}),
            "the steps of a restart are not in the loop's order");
    require(contains(machine.said(),
                "guest restarting (command 'recovery', restart 1)"),
            "the restart was not logged with its command");
}

void theRestartCommandIsLoggedSanitized()
{
    FakeMachine machine;
    machine.script = {restarts("x'\nguest restarting"), powersOff()};
    (void)kernarium::restart::run(machine, quickly);
    require(contains(machine.said(),
                "guest restarting (command 'x\\'\\x0aguest restarting', "
                "restart 1)"),
            "a restart command broke the log line");
}

void aRefusedHandBackIsRetriedThreeTimesFiveHundredMillisecondsApart()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.refusals = 3;
    require(kernarium::restart::run(machine, quickly) == 0,
            "a hand-back that succeeded on the third retry failed the launch");
    require(machine.count("pause 500") == 3U,
            "the refusal was not retried 500 ms apart three times");
    require(machine.count("enter") == 5U,
            "the refused run was not entered again each time");

    FakeMachine hopeless;
    hopeless.script = {restarts(), powersOff()};
    hopeless.refusals = 4;
    require(kernarium::restart::run(hopeless, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a refusal past the retries did not end the launch with it");
    require(hopeless.count("pause 500") == 3U,
            "the retries were not bounded by three");
    require(contains(hopeless.said(),
                "unable to boot the restarted guest: -4"),
            "the refusal was not reported");
}

void theFirstBootIsNeverRetried()
{
    FakeMachine machine;
    machine.script = {Boot {-UML_EMULATOR_RUNTIME_ERROR,
        {RunEnd::Kind::none, ""}, 1s}};
    require(kernarium::restart::run(machine, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a first run's error was not returned");
    require(machine.count("pause") == 0U,
            "a first run's error was retried as a restart's refusal");
}

void aStopThatArrivesAtAnyStepOfARestartEndsTheLaunchCleanly()
{
    struct Case {
        const char* at;
        bool secondEnter;
        bool secondStart;
        bool servicesStopped;
    };
    // The guest ended in a restart as the user quit; the gap after its
    // services stopped; between building the next boot and running it.
    for (const Case& stopCase : {
             Case {"end", false, false, false},
             Case {"began 1", false, false, true},
             Case {"start 1", false, true, true},
         }) {
        FakeMachine machine;
        machine.script = {restarts(), powersOff()};
        machine.onCall = [&](FakeMachine& self, const std::string& call) {
            if (call == stopCase.at)
                self.stop.store(true);
        };
        require(kernarium::restart::run(machine, quickly) == 0,
                "a stop did not end the launch cleanly");
        require(machine.called("start 1") == stopCase.secondStart,
                "a stop did not keep the next boot from being built");
        require(machine.count("enter") == (stopCase.secondEnter ? 2U : 1U),
                "a stop did not keep the next guest from running");
        require(machine.called("stop-services") == stopCase.servicesStopped,
                "a stop changed which services the loop stopped");
    }
}

void aStopThatEndsTheRetriesIsNotAFailure()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.refusals = 10;
    machine.onCall = [](FakeMachine& self, const std::string& call) {
        if (call == "pause 500")
            self.stop.store(true);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop during the retries was reported as an error");
    require(machine.count("pause") == 1U,
            "the retries went on after the stop");
}

void aStopBeforeTheFirstRunKeepsTheGuestFromRunning()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.stop.store(true);
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop before the first run did not end the launch");
    require(!machine.called("enter"),
            "a stop before the first run still ran the guest");
}

/* The loop's side only: the machine's `held` is the test's own switch. What makes the real machine held (a hold asked for
 * with the gate still open) is the Lifecycle's, in aHoldAskedForHoldsTheNextBootBeforeTheGateCloses, and the restart that
 * meets a hold that waits for the guest's camera is kernarium-gpu-hold. */
void theNextBootWaitsWhileTheApplicationHoldsIt()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.held.store(true);
    machine.onCall = [](FakeMachine& self, const std::string&) {
        if (self.count("wait-held") == 3U)
            self.held.store(false);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a released hold did not let the guest boot");
    const auto calls = machine.calls();
    // The restart is announced, then the loop waits with nothing built, and
    // the boot follows the release.
    require((calls == Calls {"start 0", "enter", "end", "stop-services",
                "reset-link", "began 1", "wait-held", "wait-held",
                "wait-held", "start 1", "enter", "end"}),
            "the hold did not keep the next boot back");
    require(contains(machine.said(),
                "the restarted guest may boot"),
            "the end of the hold was not logged");
}

void theFirstBootIsNeverHeld()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.held.store(true);
    require(kernarium::restart::run(machine, quickly) == 0,
            "a hold kept the first boot back");
    require(machine.count("wait-held") == 0U,
            "the first boot waited for a hold");
}

/* A hold that arrives while the next boot is built: the loop's guard found none before it, and the guest must not be run
 * behind the hold either. What remains by design is the hold that lands after the guest runs. */
void aHoldThatArrivesWhileTheNextBootIsBuiltKeepsTheGuestFromRunningUntilItEnds()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.onCall = [](FakeMachine& self, const std::string& call) {
        if (call == "start 1")
            self.held.store(true);
        if (self.count("wait-held") == 3U)
            self.held.store(false);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a released hold did not let the guest run");
    require((machine.calls() == Calls {"start 0", "enter", "end", "stop-services",
                "reset-link", "began 1", "start 1", "wait-held", "wait-held",
                "wait-held", "enter", "end"}),
            "the hold that arrived while the boot was built did not keep the guest from running");
    require(contains(machine.said(), "the restarted guest may boot"),
            "the end of the hold was not logged");
}

void aStopEndsTheWaitForAHoldThatArrivedWhileTheNextBootWasBuilt()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.onCall = [](FakeMachine& self, const std::string& call) {
        if (call == "start 1")
            self.held.store(true);
        if (self.count("wait-held") == 2U)
            self.stop.store(true);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop during the hold did not end the launch");
    require(machine.count("enter") == 1U,
            "a stop during the hold still ran the restarted guest");
}

/* The first boot is run as it is asked for: a hold that arrives while it is built is applied to the GPU the guest has
 * (the runtime's registration), and the guest runs, as it does when the hold was there before. */
void aHoldThatArrivesWhileTheFirstBootIsBuiltDoesNotKeepItBack()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.onCall = [](FakeMachine& self, const std::string& call) {
        if (call == "start 0")
            self.held.store(true);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a hold kept the first boot back");
    require(machine.count("wait-held") == 0U && machine.count("enter") == 1U,
            "the first boot waited for a hold that arrived while it was built");
}

void aStopEndsTheWaitForTheHold()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.held.store(true);
    machine.onCall = [](FakeMachine& self, const std::string&) {
        if (self.count("wait-held") == 2U)
            self.stop.store(true);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop during the hold did not end the launch");
    require(!machine.called("start 1"),
            "a stop during the hold still built the next boot");
}

void fiveEarlyRestartsInARowEndTheLoopWithItsOwnResult()
{
    FakeMachine machine;
    machine.script = {restarts("", 3s)};
    require(kernarium::restart::run(machine, quickly) ==
                KERNARIUM_RUNTIME_RESULT_RESTART_LOOP,
            "a guest that never boots did not end with the loop result");
    require(machine.count("enter") == 5U,
            "the loop guard did not end the loop at five boots");
    // The fifth restart is refused: its events never happen.
    require(machine.count("began") == 4U,
            "a restart the guard refused was announced");
    require(machine.count("stop-services") == 5U &&
                machine.count("reset-link") == 5U,
            "the services of the last guest were left running");
    require(contains(machine.said(),
                "the guest restarted 5 times in a row within 15 s of "
                "booting; not booting it again"),
            "the loop guard's end was not logged");
    require(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP < -UML_EMULATOR_RUNTIME_ERROR,
            "the loop result collides with a uml_emulator error");
    /* The applications name the host's refusal of the guest's memory by its
     * own constant, which no uml_emulator error, the loop result or a status
     * the emulator returns shares. */
    require(KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED < -UML_EMULATOR_RUNTIME_ERROR &&
                KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED !=
                    KERNARIUM_RUNTIME_RESULT_RESTART_LOOP,
            "the memory-refused result collides with another result");
}

/* The status the library returns for a run the host refused for want of
 * memory, and the one it returns for a failed allocation of its own, which
 * is the same number. */
Boot refusedForMemory()
{
    return {-UML_EMULATOR_OUT_OF_MEMORY, {RunEnd::Kind::none, ""}, 0s, true};
}

Boot outOfMemoryWithoutARefusal()
{
    return {-UML_EMULATOR_OUT_OF_MEMORY, {RunEnd::Kind::none, ""}, 0s, false};
}

void aRunTheHostRefusedForMemoryHasAResultOfItsOwn()
{
    FakeMachine machine;
    machine.script = {refusedForMemory()};
    require(kernarium::restart::run(machine, quickly) ==
                KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED,
            "a run refused for want of memory did not return its own result");
    require(machine.count("enter") == 1U, "the refused run was entered again");
}

void aBareOutOfMemoryIsNotARefusalOfTheGuestsSize()
{
    /* The library's own allocation failed (the argument vector, say): the
     * run returned the same status with no window and no mark. */
    FakeMachine bare;
    bare.script = {outOfMemoryWithoutARefusal()};
    require(kernarium::restart::run(bare, quickly) == -UML_EMULATOR_OUT_OF_MEMORY,
            "an allocation failure of the library read as a refusal of the "
            "guest's size");

    /* A guest that ran and ended with the same number as its status. */
    FakeMachine guest;
    guest.script = {{-UML_EMULATOR_OUT_OF_MEMORY,
        {RunEnd::Kind::failed, ""}, 30s, false}};
    require(kernarium::restart::run(guest, quickly) == -UML_EMULATOR_OUT_OF_MEMORY,
            "a guest's own status of the same number read as a refusal");
}

void aRestartedBootTheHostRefusedForMemoryEndsTheLaunchWithTheResult()
{
    FakeMachine machine;
    machine.script = {restarts(), refusedForMemory()};
    require(kernarium::restart::run(machine, quickly) ==
                KERNARIUM_RUNTIME_RESULT_MEMORY_REFUSED,
            "a restarted boot refused for want of memory did not return its "
            "own result");
    require(contains(machine.said(), "unable to boot the restarted guest: -2"),
            "the refused restart was not logged");

    FakeMachine bare;
    bare.script = {restarts(), outOfMemoryWithoutARefusal()};
    require(kernarium::restart::run(bare, quickly) == -UML_EMULATOR_OUT_OF_MEMORY,
            "a restarted boot's allocation failure read as a refusal of the "
            "guest's size");
}

void aBootThatLastsFifteenSecondsResetsTheCount()
{
    FakeMachine machine;
    machine.script = {restarts("", 3s), restarts("", 3s), restarts("", 3s),
        restarts("", 3s), restarts("", 15s), restarts("", 3s),
        restarts("", 3s), restarts("", 3s), restarts("", 3s), powersOff(3s)};
    require(kernarium::restart::run(machine, quickly) == 0,
            "a boot of fifteen seconds did not reset the early count");
    require(machine.count("enter") == 10U, "the script did not run through");
}

void aBootThatNeverFinishedCountsHoweverLongItRan()
{
    FakeMachine machine;
    // The loop the 15 s window let through: a restart every 20 s, before the
    // boot ever finished. Android's own boot-loop rescue gets its five boots
    // first; the loop ends at the eighth.
    machine.script = {restartsAndroid(BootCompletion::unfinished, 20s)};
    require(kernarium::restart::run(machine, quickly) ==
                KERNARIUM_RUNTIME_RESULT_RESTART_LOOP,
            "a guest that never finishes booting did not end with the loop "
            "result");
    require(machine.count("enter") == 8U,
            "the loop guard did not end the late loop at eight boots");
    require(machine.count("began") == 7U,
            "a restart the guard refused was announced");
    require(machine.count("stop-services") == 8U &&
                machine.count("reset-link") == 8U,
            "the services of the last guest were left running");
    require(contains(machine.said(),
                "the guest restarted 8 times in a row, each before it "
                "finished booting or within 15 s of booting; not booting it "
                "again"),
            "the late loop's end was not logged with its reason");
}

void anAndroidGuestRestartingAtOnceEndsAtFive()
{
    FakeMachine machine;
    machine.script = {restartsAndroid(BootCompletion::unfinished, 3s)};
    require(kernarium::restart::run(machine, quickly) ==
                KERNARIUM_RUNTIME_RESULT_RESTART_LOOP,
            "a guest that restarts at once did not end with the loop result");
    require(machine.count("enter") == 5U,
            "the loop guard did not end the early loop at five boots");
    require(contains(machine.said(),
                "the guest restarted 5 times in a row within 15 s of "
                "booting; not booting it again"),
            "the early loop's end was not logged with its reason");
}

void aConsoleThatShowsNothingOfInitIsNotAnUnfinishedBoot()
{
    FakeMachine machine;
    // A healthy Android guest on a console the host does not read: boots of
    // 20 s are no loop.
    machine.script.assign(12, restartsAndroid(BootCompletion::unobserved, 20s));
    machine.script.push_back(powersOff(20s));
    require(kernarium::restart::run(machine, quickly) == 0,
            "boots of 20 s on an unread console ended the loop");
}

void restartsAfterFinishedBootsAreNeverLimited()
{
    FakeMachine machine;
    // A reboot suite: every boot finished and lasted, twelve times over.
    machine.script.assign(12, restartsAndroid(BootCompletion::completed, 40s));
    machine.script.push_back(powersOff(40s));
    require(kernarium::restart::run(machine, quickly) == 0,
            "restarts after finished boots ended the loop");
    require(machine.count("enter") == 13U, "the script did not run through");
    require(machine.count("began") == 12U, "a restart was not announced");
}

void aFinishedBootResetsTheCountOfUnfinishedOnes()
{
    FakeMachine machine;
    // Seven boots that did not finish, one that did, seven more: never eight
    // in a row.
    for (int round = 0; round != 2; ++round) {
        for (int slow = 0; slow != 7; ++slow)
            machine.script.push_back(
                restartsAndroid(BootCompletion::unfinished, 30s));
        machine.script.push_back(
            restartsAndroid(BootCompletion::completed, 30s));
    }
    machine.script.push_back(powersOff(30s));
    require(kernarium::restart::run(machine, quickly) == 0,
            "a finished boot did not reset the count of unfinished ones");
    require(machine.count("enter") == 17U, "the script did not run through");
}

void aGuestWhoseConsoleSaysNothingKeepsTheWindowAlone()
{
    FakeMachine machine;
    // A Linux guest: boots of 20 s are no loop, whatever the host can tell.
    machine.script.assign(12, restarts("", 20s));
    machine.script.push_back(powersOff(20s));
    require(kernarium::restart::run(machine, quickly) == 0,
            "boots of 20 s with no console signal ended the loop");
}

void aBootThatCannotBeBuiltFailsTheLaunch()
{
    FakeMachine first;
    first.script = {powersOff()};
    first.failStart = 0;
    require(kernarium::restart::run(first, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a first boot that could not be built did not fail the launch");
    require(!first.called("enter"), "a boot that failed to build ran");

    FakeMachine restarted;
    restarted.script = {restarts(), powersOff()};
    restarted.failStart = 1;
    require(kernarium::restart::run(restarted, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a restarted boot that could not be built did not fail");
    require(restarted.count("enter") == 1U,
            "a restarted boot that failed to build ran");
}

void theBootedEventFollowsTheNewGuestsConsole()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.answerFromPoll = 3;
    machine.runWaitsForBooted = true;
    {
        uml::testing::Watchdog guard {"the restart events", 30s};
        require(kernarium::restart::run(machine, quickly) == 0,
                "the restarted guest did not end the launch");
    }
    const auto calls = machine.calls();
    // BEGAN comes before the new boot is built, BOOTED while its run is
    // under way, and the first boot sends neither.
    require((calls == Calls {"start 0", "enter", "end", "stop-services",
                "reset-link", "began 1", "start 1", "enter",
                "booted 1 answered", "end"}),
            "the restart events are not in the order of the restart");
    require(machine.polls() >= 3U,
            "the event did not wait for the console to answer");
}

void theFirstBootAsksNothingOfTheConsole()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.answerFromPoll = 1;
    (void)kernarium::restart::run(machine, quickly);
    require(machine.polls() == 0U && machine.count("booted") == 0U,
            "the first boot was probed for a restart event");
}

void theBootedEventIsSentWithoutAnAnswerAfterTheBound()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.answerFromPoll = 0;
    machine.runWaitsForBooted = true;
    const Timing brief {.consolePoll = 1ms, .consoleBound = 30ms,
        .heldPoll = 1ms};
    {
        uml::testing::Watchdog guard {"the unanswered restart event", 30s};
        require(kernarium::restart::run(machine, brief) == 0,
                "the launch did not end");
    }
    require(machine.called("booted 1 unanswered"),
            "a console that never answers was not reported after the bound");
}

void noBootedEventIsSentWhenTheGuestEndsFirst()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.answerFromPoll = 0;
    require(kernarium::restart::run(machine, quickly) == 0,
            "the launch did not end");
    require(machine.count("booted") == 0U,
            "an event was sent for a guest that had ended");
}

void aStoppedAndQuiescedLifecycleHoldsTheNextBoot()
{
    Lifecycle lifecycle;
    require(!lifecycle.held(), "a fresh lifecycle held the boot");

    // A paused guest.
    require(!lifecycle.quiesced(), "a fresh lifecycle was quiesced");
    lifecycle.quiesce();
    require(lifecycle.held(), "a quiesce between boots did not hold the boot");
    require(lifecycle.quiesced(), "a quiesce was not reported");
    lifecycle.resume();
    require(!lifecycle.held(), "a resume did not release the boot");
    require(!lifecycle.quiesced(), "a resume left the guest quiesced");

    // The GPU's request gate.
    std::atomic_bool gate {};
    lifecycle.setGpuHeld([&] { return gate.load(); });
    require(!lifecycle.held(), "an open GPU gate held the boot");
    gate.store(true);
    require(lifecycle.held(), "held GPU requests did not hold the boot");
    require(!lifecycle.quiesced(),
        "held GPU requests are not a quiesced guest");
    gate.store(false);
    require(!lifecycle.held(), "released GPU requests held the boot");

    // Either holds; both must end.
    lifecycle.quiesce();
    gate.store(true);
    gate.store(false);
    require(lifecycle.held(), "a quiesce was forgotten when the GPU opened");
    lifecycle.resume();
    gate.store(true);
    require(lifecycle.held(), "the GPU's hold was forgotten at a resume");
    lifecycle.setGpuHeld({});
    require(!lifecycle.held(), "an unregistered GPU still held the boot");
}

void aHoldAskedForHoldsTheNextBootBeforeTheGateCloses()
{
    Lifecycle lifecycle;
    std::atomic_bool gate {};
    lifecycle.setGpuHeld([&] { return gate.load(); });
    require(!lifecycle.gpuHoldRequested(), "a fresh lifecycle had a hold asked for");

    // The application has asked, and the gate is still open: it waits for the
    // guest's camera first, and a guest that boots meanwhile would find the gate
    // closed behind it.
    lifecycle.setGpuHoldRequested(true);
    require(lifecycle.gpuHoldRequested(), "the hold asked for was not kept");
    require(lifecycle.held(), "a hold asked for with the gate open did not hold the boot");
    require(!lifecycle.quiesced(), "a hold asked for is not a quiesced guest");

    // It holds with no GPU service registered, between two boots.
    lifecycle.setGpuHeld({});
    require(lifecycle.held(), "a hold asked for was forgotten when the GPU service went away");

    // The requests given back release the boot, whatever the gate does.
    lifecycle.setGpuHoldRequested(false);
    require(!lifecycle.held(), "a hold given back still held the boot");
    gate.store(true);
    lifecycle.setGpuHeld([&] { return gate.load(); });
    require(lifecycle.held(), "a closed gate stopped holding the boot");
}

/* Calls `nudge` until `done`, so that a wake-up sent before the waiter began
 * to wait is sent again. */
template<typename Nudge, typename Done>
void nudgeUntil(Nudge nudge, Done done)
{
    while (!done()) {
        nudge();
        std::this_thread::sleep_for(1ms);
    }
}

void aWaitingBootWakesWhenTheHoldEnds()
{
    Lifecycle lifecycle;
    std::atomic_bool gate {true};
    lifecycle.setGpuHeld([&] { return gate.load(); });
    lifecycle.quiesce();
    std::atomic_uint waits {};
    // A long slice: only a wake-up ends a wait early.
    std::thread waiter {[&] {
        while (lifecycle.held()) {
            ++waits;
            lifecycle.waitWhileHeld(60s);
        }
    }};
    uml::testing::Watchdog guard {"the waiting boot", 30s};
    nudgeUntil([] {}, [&] { return waits.load() >= 1U; });
    // The GPU opens but the guest is still paused: it wakes, looks, and
    // waits again.
    gate.store(false);
    nudgeUntil([&] { lifecycle.changed(); },
               [&] { return waits.load() >= 2U; });
    require(lifecycle.held(), "a released GPU gate released a paused guest");
    lifecycle.resume();
    waiter.join();
    require(!lifecycle.held(), "the boot was not released");
}

void aStopWakesAWaitingBootAndIsNotInheritedByTheNextRun()
{
    Lifecycle lifecycle;
    lifecycle.quiesce();
    std::atomic_bool woke {};
    std::thread waiter {[&] {
        lifecycle.waitWhileHeld(60s);
        woke.store(true);
    }};
    {
        uml::testing::Watchdog guard {"the stop wake-up", 30s};
        nudgeUntil([&] { lifecycle.requestStop(); },
                   [&] { return woke.load(); });
        waiter.join();
    }
    require(lifecycle.stopRequested(),
            "a stop did not wake the waiting boot");
    lifecycle.begin();
    require(!lifecycle.stopRequested(),
            "a stop asked for before a run was carried into it");
}

const uml::testing::NamedTest tests[] {
    {"a-guest-that-powers-off-is-booted-once", aGuestThatPowersOffIsBootedOnce},
    {"every-end-but-a-restart-ends-the-launch",
        everyEndButARestartEndsTheLaunchWithTheRunsStatus},
    {"a-restart-runs-its-steps-in-order",
        aRestartStopsTheServicesResetsTheLinkAndBootsAgainInThisOrder},
    {"the-restart-command-is-logged-sanitized",
        theRestartCommandIsLoggedSanitized},
    {"a-refused-hand-back-is-retried",
        aRefusedHandBackIsRetriedThreeTimesFiveHundredMillisecondsApart},
    {"the-first-boot-is-never-retried", theFirstBootIsNeverRetried},
    {"a-stop-at-any-step-of-a-restart",
        aStopThatArrivesAtAnyStepOfARestartEndsTheLaunchCleanly},
    {"a-stop-during-the-retries-is-not-a-failure",
        aStopThatEndsTheRetriesIsNotAFailure},
    {"a-stop-before-the-first-run", aStopBeforeTheFirstRunKeepsTheGuestFromRunning},
    {"the-next-boot-waits-while-held", theNextBootWaitsWhileTheApplicationHoldsIt},
    {"the-first-boot-is-never-held", theFirstBootIsNeverHeld},
    {"a-stop-ends-the-wait-for-the-hold", aStopEndsTheWaitForTheHold},
    {"a-hold-that-arrives-while-the-next-boot-is-built-keeps-the-guest-from-running",
        aHoldThatArrivesWhileTheNextBootIsBuiltKeepsTheGuestFromRunningUntilItEnds},
    {"a-stop-ends-the-wait-for-a-hold-that-arrived-while-the-next-boot-was-built",
        aStopEndsTheWaitForAHoldThatArrivedWhileTheNextBootWasBuilt},
    {"a-hold-that-arrives-while-the-first-boot-is-built-does-not-keep-it-back",
        aHoldThatArrivesWhileTheFirstBootIsBuiltDoesNotKeepItBack},
    {"five-early-restarts-end-the-loop",
        fiveEarlyRestartsInARowEndTheLoopWithItsOwnResult},
    {"a-fifteen-second-boot-resets-the-count",
        aBootThatLastsFifteenSecondsResetsTheCount},
    {"an-android-guest-restarting-at-once-ends-at-five",
        anAndroidGuestRestartingAtOnceEndsAtFive},
    {"a-console-that-shows-nothing-of-init-is-not-an-unfinished-boot",
        aConsoleThatShowsNothingOfInitIsNotAnUnfinishedBoot},
    {"an-unfinished-boot-counts-however-long-it-ran",
        aBootThatNeverFinishedCountsHoweverLongItRan},
    {"restarts-after-finished-boots-are-unlimited",
        restartsAfterFinishedBootsAreNeverLimited},
    {"a-finished-boot-resets-the-unfinished-count",
        aFinishedBootResetsTheCountOfUnfinishedOnes},
    {"a-guest-with-no-console-signal-keeps-the-window",
        aGuestWhoseConsoleSaysNothingKeepsTheWindowAlone},
    {"a-boot-that-cannot-be-built", aBootThatCannotBeBuiltFailsTheLaunch},
    {"a-run-the-host-refused-for-memory", aRunTheHostRefusedForMemoryHasAResultOfItsOwn},
    {"a-bare-out-of-memory-is-no-refusal", aBareOutOfMemoryIsNotARefusalOfTheGuestsSize},
    {"a-restarted-boot-refused-for-memory", aRestartedBootTheHostRefusedForMemoryEndsTheLaunchWithTheResult},
    {"the-booted-event-follows-the-console",
        theBootedEventFollowsTheNewGuestsConsole},
    {"the-first-boot-asks-nothing-of-the-console",
        theFirstBootAsksNothingOfTheConsole},
    {"the-booted-event-after-the-bound",
        theBootedEventIsSentWithoutAnAnswerAfterTheBound},
    {"no-booted-event-when-the-guest-ends-first",
        noBootedEventIsSentWhenTheGuestEndsFirst},
    {"the-lifecycle-holds-the-next-boot",
        aStoppedAndQuiescedLifecycleHoldsTheNextBoot},
    {"a-hold-asked-for-holds-the-next-boot-before-the-gate-closes",
        aHoldAskedForHoldsTheNextBootBeforeTheGateCloses},
    {"a-waiting-boot-wakes-when-the-hold-ends", aWaitingBootWakesWhenTheHoldEnds},
    {"a-stop-wakes-a-waiting-boot",
        aStopWakesAWaitingBootAndIsNotInheritedByTheNextRun},
};

} // namespace

int main()
{
    const uml::testing::Watchdog bound {
        "the Kernarium restart loop tests", 120s};
    return uml::testing::runNamedTests(tests);
}
