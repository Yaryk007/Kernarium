// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest GNSS HAL's session (the overlay's gnss_engine.hpp) over a pipe that
// stands in for /dev/gnss0, with the host writer's epochs written into it: the
// framework's calls in order, the wakelock held for exactly the session, a
// receiver that dies, and that nothing is reported after stop(). This test is
// GPL-2.0-or-later and includes the overlay's Apache-2.0 headers; the binary is
// built and run by the test suite only and is never distributed.

#include "gnss_engine.hpp"
#include "gnss_hal_test_world.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <csignal>
#include <optional>

namespace {

using andarium::gnss::Engine;
using gnsshaltest::Fix;
using gnsshaltest::World;
using gnsshaltest::baseNs;
using gnsshaltest::epoch;
using gnsshaltest::heldWakelocks;
using gnsshaltest::preciseFix;
using gnsshaltest::second;
using andarium::gnss::Status;
using uml::testing::require;
using namespace std::chrono_literals;

const std::vector<Status> beganSession{Status::engine_on, Status::session_begin};
const std::vector<Status> endedSession{Status::engine_on, Status::session_begin, Status::session_end,
                                       Status::engine_off};

std::vector<Status> reportedStatuses(World& world)
{
    std::lock_guard lock(world.mutex);
    return world.statuses;
}

bool reported(World& world, const std::vector<Status>& expected)
{
    return world.wait([&] { return world.statuses == expected; });
}

/* The framework hears the engine in order, so a new session's begin is a
 * barrier: when it arrives, everything posted before it has been delivered, and
 * `expected` is all there was. Starts a session, which the caller may stop. */
void nothingMoreBefore(World& world, Engine& engine, std::vector<Status> expected)
{
    require(engine.start());
    expected.push_back(Status::engine_on);
    expected.push_back(Status::session_begin);
    require(reported(world, expected), "nothing is said between the sessions");
}

void beginsAndEndsASession()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(!engine.running());
    require(engine.start());
    require(engine.running());
    require(reported(world, beganSession));
    heldWakelocks(world, 1);

    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return !world.locations.empty(); }));
    {
        std::lock_guard lock(world.mutex);
        require(world.locations.size() == 1);
        require(world.locations[0].horizontal_accuracy == 8.0);
        require(world.locations[0].timestamp_ms == baseNs / 1'000'000);
    }
    heldWakelocks(world, 1);
    require(!world.receiverClosed());

    engine.stop();
    require(!engine.running());
    require(reported(world, endedSession));
    heldWakelocks(world, 0);
    require(world.receiverClosed());
    // A second stop is nothing.
    engine.stop();
    nothingMoreBefore(world, engine, endedSession);
    engine.stop();
}

/* The decision: the lock is held from start() to stop() across any number of
 * epochs, and not a moment less or more. */
void holdsTheWakelockForTheWholeSession()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    for (int index = 0; index < 5; ++index) {
        world.boottime += second;
        world.write(epoch(preciseFix(baseNs + index * second)));
        require(world.wait([&] { return world.locations.size() == static_cast<std::size_t>(index) + 1; }));
        heldWakelocks(world, 1);
    }
    {
        std::lock_guard lock(world.mutex);
        require(world.wakelocksTaken == 1, "one lock for the session, not one per epoch");
    }
    engine.stop();
    heldWakelocks(world, 0);
}

void takesNoWakelockBeforeStartOrAfterStop()
{
    World world;
    Engine engine(world.env(), world.sink());
    heldWakelocks(world, 0);
    require(engine.start());
    engine.stop();
    heldWakelocks(world, 0);
    require(world.wakelocksTaken == 1);
    // A restart is a new session with its own lock.
    require(engine.start());
    heldWakelocks(world, 1);
    engine.stop();
    heldWakelocks(world, 0);
    require(world.wakelocksTaken == 2);
}

void releasesTheWakelockWhenTheReceiverDies()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return !world.locations.empty(); }));
    world.hangUp();
    // A session with no receiver behind it keeps nothing awake.
    require(world.wait([&] { return world.wakelocksHeld == 0; }));
    require(world.wait([&] { return !world.logs.empty(); }));
    // The framework's stop() still works, and has nothing left to say.
    require(reported(world, endedSession));
    engine.stop();
    heldWakelocks(world, 0);
    nothingMoreBefore(world, engine, endedSession);
    engine.stop();
}

/* A receiver that goes away ends the session by itself: the framework is told
 * once, from the session's thread, and the stop() that follows adds nothing. */
void reportsTheEndWhenTheReceiverDiesOnItsOwn()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return !world.locations.empty(); }));
    world.hangUp();
    require(reported(world, endedSession));
    require(!engine.running(), "a session that reported its end is not running");
    heldWakelocks(world, 0);
    {
        std::lock_guard lock(world.mutex);
        require(world.opens == 1, "the receiver is not opened again");
    }
    engine.stop();
    require(world.receiverClosed());
    engine.stop();
    nothingMoreBefore(world, engine, endedSession);
    engine.stop();
}

/* The same for a session that never received an epoch. */
void reportsTheEndOfASessionThatReadNothing()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.hangUp();
    require(reported(world, endedSession));
    // Destroying the engine after the end is silent too.
}

/* A restart after the end is a new session: it reports its own beginning, and
 * its end is reported again. */
void restartsAfterTheEnd()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.hangUp();
    require(reported(world, endedSession));
    require(engine.start());
    require(engine.running());
    require(reported(world, {Status::engine_on, Status::session_begin, Status::session_end, Status::engine_off,
                             Status::engine_on, Status::session_begin}));
    heldWakelocks(world, 1);
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return !world.locations.empty(); }));
    engine.stop();
    require(world.wait([&] { return world.statuses.size() == 8; }));
    require(reportedStatuses(world).back() == Status::engine_off);
    std::lock_guard lock(world.mutex);
    require(world.opens == 2);
}

/* stop() and the receiver dying at the same moment report the end once, from
 * whichever gets there first. */
void reportsTheEndOnceWhenStopRacesTheReceiverDying()
{
    for (int round = 0; round < 300; ++round) {
        World world;
        Engine engine(world.env(), world.sink());
        require(engine.start());
        std::thread hang([&] { world.hangUp(); });
        engine.stop();
        hang.join();
        require(reported(world, endedSession), "one end per session");
        heldWakelocks(world, 0);
        nothingMoreBefore(world, engine, endedSession);
        engine.stop();
    }
}

void runsWithoutAWakelockWhenThereIsNone()
{
    World world;
    world.wakelockUnavailable = true;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return !world.locations.empty(); }));
    {
        std::lock_guard lock(world.mutex);
        require(world.logs.size() == 1, "the missing lock is logged once");
    }
    engine.stop();
}

void failsWhenTheReceiverCannotBeOpened()
{
    World world;
    world.deviceMissing = true;
    Engine engine(world.env(), world.sink());
    require(!engine.start());
    require(!engine.running());
    require(world.statuses.empty(), "no session began");
    // The lock taken first is not kept.
    heldWakelocks(world, 0);
    require(!world.logs.empty());
    // And the engine starts when the receiver appears.
    world.deviceMissing = false;
    require(engine.start());
    engine.stop();
}

void reportsNothingAfterStop()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    std::atomic<bool> writing{true};
    // A receiver that never stops writing epochs of new fixes.
    std::thread writer([&] {
        for (int index = 0; writing; ++index) {
            world.boottime += 2 * second;
            const auto text = epoch(preciseFix(baseNs + index * 2 * second));
            bool full = false;
            {
                std::lock_guard lock(world.mutex);
                if (world.feed < 0)
                    break;
                if (::write(world.feed, text.data(), text.size()) < 0) {
                    if (errno == EPIPE)
                        break;
                    full = true;
                }
            }
            if (full)
                std::this_thread::sleep_for(1ms);
        }
    });
    require(world.wait([&] { return world.locations.size() >= 3; }));
    engine.stop();
    require(world.wait([&] { return world.statuses.size() == 4; }));
    writing = false;
    writer.join();
    // The calls were made in the order they were posted: every location of the
    // session came before its end, and none was made after it.
    std::lock_guard lock(world.mutex);
    require(world.log.back() == "off" && world.log[world.log.size() - 2] == "end");
    std::size_t end = 0;
    for (std::size_t index = 0; index < world.log.size(); ++index)
        if (world.log[index] == "end")
            end = index;
    for (std::size_t index = end; index < world.log.size(); ++index)
        require(world.log[index] != "location", "no location after the end of the session");
}

void startingTwiceRestartsTheSession()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    require(engine.start());
    // The restart ends the first session without telling the framework, and
    // begins the second.
    require(reported(world, {Status::engine_on, Status::session_begin, Status::engine_on, Status::session_begin}));
    {
        std::lock_guard lock(world.mutex);
        require(world.opens == 2);
        require(world.wakelocksHeld == 1 && world.wakelocksTaken == 2);
    }
    engine.stop();
    heldWakelocks(world, 0);
}

void destroysARunningSession()
{
    World world;
    {
        Engine engine(world.env(), world.sink());
        require(engine.start());
        world.write(epoch(preciseFix(baseNs)));
        require(world.wait([&] { return !world.locations.empty(); }));
    }
    heldWakelocks(world, 0);
    require(world.receiverClosed());
    // The destructor tells the framework nothing: the HAL's close() has already
    // dropped its callback.
    std::lock_guard lock(world.mutex);
    require(world.statuses.size() == 2);
}

void deliversSentencesAndSatelliteStatusOnlyWhenAsked()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.boottime += second;
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return world.locations.size() == 1; }));
    {
        std::lock_guard lock(world.mutex);
        require(world.sentences.empty() && world.svStatuses == 0, "off by default");
    }
    engine.set_nmea(true);
    engine.set_sv_status(true);
    world.boottime += second;
    world.write(epoch(preciseFix(baseNs + second)));
    require(world.wait([&] { return world.locations.size() == 2; }));
    {
        std::lock_guard lock(world.mutex);
        require(world.sentences.size() == 6);
        require(world.sentences[0].rfind("$GNGGA,", 0) == 0);
        require(world.svStatuses == 1);
        // Stamped with the fix's own time.
        for (const auto at : world.sentenceTimes)
            require(at == (baseNs + second) / 1'000'000);
    }
    engine.set_nmea(false);
    engine.set_sv_status(false);
    world.boottime += second;
    world.write(epoch(preciseFix(baseNs + 2 * second)));
    require(world.wait([&] { return world.locations.size() == 3; }));
    {
        std::lock_guard lock(world.mutex);
        require(world.sentences.size() == 6 && world.svStatuses == 1);
    }
    engine.stop();
}

void reportsNoFixSentencesAndNoLocation()
{
    World world;
    Engine engine(world.env(), world.sink());
    engine.set_nmea(true);
    engine.set_sv_status(true);
    require(engine.start());
    world.write(uml::gnss::nmeaEpoch(std::nullopt, baseNs));
    require(world.wait([&] { return world.svStatuses == 1 && world.sentences.size() == 6; }));
    {
        std::lock_guard lock(world.mutex);
        require(world.locations.empty());
        require(world.sentenceTimes[0] == baseNs / 1'000'000, "stamped with the time the sentences carry");
    }
    require(!engine.last_fix());
    engine.stop();
}

void reportsAHeldFixOnce()
{
    World world;
    world.boottimeStep = second;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    // The host repeats a held fix with its own time for ten seconds, a second
    // apart.
    for (int index = 0; index < 4; ++index)
        world.write(epoch(preciseFix(baseNs)));
    // A newer fix follows; it is the proof that the repeats were read.
    world.write(epoch(preciseFix(baseNs + 5 * second, 35.7)));
    require(world.wait([&] { return world.locations.size() == 2; }));
    std::lock_guard lock(world.mutex);
    require(world.locations.size() == 2);
    require(world.locations[1].latitude > 35.69);
}

void reportsAtTheFrameworksInterval()
{
    World world;
    world.boottimeStep = second;
    Engine engine(world.env(), world.sink());
    engine.set_min_interval_ms(3000);
    engine.set_sv_status(true);
    require(engine.start());
    // A new fix each second, a second of uptime between epochs: one in three goes.
    for (int index = 0; index < 7; ++index)
        world.write(epoch(preciseFix(baseNs + index * second)));
    // Every epoch makes a satellite status, so seven means all were handled; the
    // locations before it were delivered first, and the seventh epoch's follows.
    require(world.wait([&] { return world.svStatuses == 7 && world.locations.size() >= 3; }));
    engine.stop();
    std::lock_guard lock(world.mutex);
    require(world.locations.size() == 3, "fixes 0, 3 and 6 of seven");
}

void holdsTheApproximateLineEndToEnd()
{
    World world;
    Engine engine(world.env(), world.sink());
    engine.set_nmea(true);
    require(engine.start());
    Fix fix = preciseFix(baseNs);
    fix.approximate = true;
    fix.horizontalAccuracy = 100.0;
    world.write(epoch(fix));
    require(world.wait([&] { return !world.locations.empty() && world.sentences.size() == 6; }));
    {
        std::lock_guard lock(world.mutex);
        const auto& location = world.locations[0];
        require(location.flags == (andarium::gnss::kHasLatLong | andarium::gnss::kHasHorizontalAccuracy));
        require(location.horizontal_accuracy == 2000.0);
        for (const auto& sentence : world.sentences) {
            require(sentence.find("40.0") == std::string::npos && sentence.find("80.5") == std::string::npos);
        }
    }
    const auto debug = engine.last_fix();
    require(debug && debug->position.fix_class == andarium::gnss::nmea::FixClass::approximate);
    engine.stop();
}

void restartsCleanAfterAStop()
{
    World world;
    Engine engine(world.env(), world.sink());
    require(engine.start());
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return world.locations.size() == 1; }));
    // The first half of an epoch is written and read, and the session ends.
    const auto full = epoch(preciseFix(baseNs + second, 35.7));
    std::size_t half = 0;
    for (int sentences = 0; sentences < 2; ++sentences)
        half = full.find("\r\n", half) + 2;
    world.write(full.substr(0, half));
    require(world.drained());
    engine.stop();
    // The same fix in a new session is reported again, since the pacer started
    // over; the second half of the old epoch finds no first half to complete.
    require(engine.start());
    world.write(full.substr(half));
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return world.locations.size() == 2; }));
    engine.stop();
    std::lock_guard lock(world.mutex);
    require(world.locations.size() == 2);
    require(world.locations[1].latitude < 35.69, "the new session's fix, not the old half-epoch's");
}

} // namespace

int main()
{
    // The receiver pipe's reader closes while the test may still write.
    signal(SIGPIPE, SIG_IGN);
    static constexpr uml::testing::NamedTest tests[] = {
        {"begins-and-ends-a-session", beginsAndEndsASession},
        {"holds-the-wakelock-for-the-whole-session", holdsTheWakelockForTheWholeSession},
        {"takes-no-wakelock-outside-a-session", takesNoWakelockBeforeStartOrAfterStop},
        {"releases-the-wakelock-when-the-receiver-dies", releasesTheWakelockWhenTheReceiverDies},
        {"reports-the-end-when-the-receiver-dies", reportsTheEndWhenTheReceiverDiesOnItsOwn},
        {"reports-the-end-of-a-session-that-read-nothing", reportsTheEndOfASessionThatReadNothing},
        {"restarts-after-the-end", restartsAfterTheEnd},
        {"reports-the-end-once-when-stop-races", reportsTheEndOnceWhenStopRacesTheReceiverDying},
        {"runs-without-a-wakelock-when-there-is-none", runsWithoutAWakelockWhenThereIsNone},
        {"fails-when-the-receiver-cannot-be-opened", failsWhenTheReceiverCannotBeOpened},
        {"reports-nothing-after-stop", reportsNothingAfterStop},
        {"starting-twice-restarts-the-session", startingTwiceRestartsTheSession},
        {"destroys-a-running-session", destroysARunningSession},
        {"delivers-sentences-and-satellites-only-when-asked", deliversSentencesAndSatelliteStatusOnlyWhenAsked},
        {"reports-no-fix-sentences-and-no-location", reportsNoFixSentencesAndNoLocation},
        {"reports-a-held-fix-once", reportsAHeldFixOnce},
        {"reports-at-the-frameworks-interval", reportsAtTheFrameworksInterval},
        {"holds-the-approximate-line-end-to-end", holdsTheApproximateLineEndToEnd},
        {"restarts-clean-after-a-stop", restartsCleanAfterAStop},
    };
    return uml::testing::runNamedTests(tests);
}
