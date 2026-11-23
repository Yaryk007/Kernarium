// SPDX-License-Identifier: GPL-2.0-or-later
//
// The entry points the framework's binder threads call on the guest GNSS HAL
// (the overlay's gnss_hal.hpp behind Gnss.cpp), over the same pipe-backed
// receiver as the engine tests: what each call does to the session, and that
// the position mode and the toggles outlive one callback. This test is
// GPL-2.0-or-later and includes the overlay's Apache-2.0 headers; the binary is
// built and run by the test suite only and is never distributed.

#include "gnss_hal.hpp"
#include "gnss_hal_test_world.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <csignal>

namespace {

using andarium::gnss::Hal;
using andarium::gnss::Status;
using gnsshaltest::World;
using gnsshaltest::baseNs;
using gnsshaltest::epoch;
using gnsshaltest::heldWakelocks;
using gnsshaltest::preciseFix;
using gnsshaltest::second;
using uml::testing::require;

const std::vector<Status> beganSession{Status::engine_on, Status::session_begin};

std::vector<Status> statusesOf(World& world)
{
    std::lock_guard lock(world.mutex);
    return world.statuses;
}

void refusesAStartBeforeAnyCallback()
{
    World world;
    Hal hal(world.env());
    require(hal.start() == Hal::StartResult::no_callback);
    hal.stop();
    hal.close();
    require(!hal.last_fix());
    heldWakelocks(world, 0);
    require(world.opens == 0);
}

void startsAndStopsTheSessionOfTheCallback()
{
    World world;
    Hal hal(world.env());
    hal.set_callback(world.sink());
    require(hal.start() == Hal::StartResult::started);
    require(world.wait([&] { return world.statuses == beganSession; }));
    heldWakelocks(world, 1);
    world.boottime += second;
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return world.locations.size() == 1; }));
    require(hal.last_fix().has_value());
    hal.stop();
    require(world.wait([&] { return world.statuses.size() == 4; }));
    heldWakelocks(world, 0);
    require(world.receiverClosed());
    // A stop with no session is nothing.
    hal.stop();
    require(statusesOf(world).size() == 4);
}

void reportsAMissingReceiver()
{
    World world;
    world.deviceMissing = true;
    Hal hal(world.env());
    hal.set_callback(world.sink());
    require(hal.start() == Hal::StartResult::no_receiver);
    heldWakelocks(world, 0);
    require(statusesOf(world).empty());
}

void closeEndsTheSessionSilently()
{
    World world;
    Hal hal(world.env());
    hal.set_callback(world.sink());
    require(hal.start() == Hal::StartResult::started);
    require(world.wait([&] { return world.statuses == beganSession; }));
    hal.close();
    heldWakelocks(world, 0);
    require(world.receiverClosed());
    require(!hal.last_fix());
    require(hal.start() == Hal::StartResult::no_callback);
    // The framework closed the HAL: it is told nothing more.
    require(statusesOf(world) == beganSession);
}

void aNewCallbackReplacesTheSession()
{
    World first;
    World other;
    Hal hal(first.env());
    hal.set_callback(first.sink());
    require(hal.start() == Hal::StartResult::started);
    require(first.wait([&] { return first.statuses == beganSession; }));
    hal.set_callback(other.sink());
    require(other.wait([&] { return other.readies == 1; }), "the new callback is told what the receiver is");
    // The old session is gone, silently, with its receiver and its lock.
    heldWakelocks(first, 0);
    require(first.receiverClosed());
    require(statusesOf(first) == beganSession);
    // The new callback has no session until it asks for one.
    require(statusesOf(other).empty());
    require(!hal.last_fix());
}

void theModeAndTheTogglesOutliveOneCallback()
{
    World world;
    Hal hal(world.env());
    // Set before any callback, applied to the first.
    require(hal.set_position_mode(1000));
    hal.set_nmea(true);
    hal.set_sv_status(true);
    hal.set_callback(world.sink());
    require(hal.start() == Hal::StartResult::started);
    world.boottime += second;
    world.write(epoch(preciseFix(baseNs)));
    require(world.wait([&] { return world.locations.size() == 1; }));
    {
        std::lock_guard lock(world.mutex);
        require(world.sentences.size() == 6, "sentences were asked for before the callback");
        require(world.svStatuses == 1);
    }
    // Turned off while the session runs, and kept off by the next callback.
    hal.set_nmea(false);
    hal.set_sv_status(false);
    hal.close();
    World again;
    hal.set_callback(again.sink());
    require(hal.start() == Hal::StartResult::started);
    // The receiver and the clock are the first world's; the callback is the second's.
    world.boottime += second;
    world.write(epoch(preciseFix(baseNs)));
    require(again.wait([&] { return again.locations.size() == 1; }));
    {
        std::lock_guard lock(again.mutex);
        require(again.sentences.empty() && again.svStatuses == 0);
    }
    hal.close();
}

void refusesAnIntervalThatIsNotPositive()
{
    World world;
    Hal hal(world.env());
    require(!hal.set_position_mode(0));
    require(!hal.set_position_mode(-5));
    require(hal.set_position_mode(1));
}

void announcesOncePerCallback()
{
    World world;
    Hal hal(world.env());
    hal.set_callback(world.sink());
    require(world.wait([&] { return world.readies == 1; }));
    require(hal.start() == Hal::StartResult::started);
    hal.stop();
    require(world.wait([&] { return world.statuses.size() == 4; }));
    hal.close();
    std::lock_guard lock(world.mutex);
    require(world.readies == 1);
    require(world.log.front() == "ready");
}

} // namespace

int main()
{
    // The receiver pipe's reader closes while the test may still write.
    signal(SIGPIPE, SIG_IGN);
    static constexpr uml::testing::NamedTest tests[] = {
        {"refuses-a-start-before-any-callback", refusesAStartBeforeAnyCallback},
        {"starts-and-stops-the-session-of-the-callback", startsAndStopsTheSessionOfTheCallback},
        {"reports-a-missing-receiver", reportsAMissingReceiver},
        {"close-ends-the-session-silently", closeEndsTheSessionSilently},
        {"a-new-callback-replaces-the-session", aNewCallbackReplacesTheSession},
        {"the-mode-and-the-toggles-outlive-one-callback", theModeAndTheTogglesOutliveOneCallback},
        {"refuses-an-interval-that-is-not-positive", refusesAnIntervalThatIsNotPositive},
        {"announces-once-per-callback", announcesOncePerCallback},
    };
    return uml::testing::runNamedTests(tests);
}
