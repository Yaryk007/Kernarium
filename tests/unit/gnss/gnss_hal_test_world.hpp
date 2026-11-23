// SPDX-License-Identifier: GPL-2.0-or-later
//
// The operating system as the guest GNSS HAL's tests see it (the overlay's
// gnss_engine.hpp and gnss_hal.hpp): a pipe for the receiver, a wakelock that
// counts, a clock the test moves, and a sink that records what the framework
// would hear. This test support is GPL-2.0-or-later and includes the overlay's
// Apache-2.0 headers; the tests are built and run by the test suite only and
// are never distributed.
#pragma once

#include "gnss_engine.hpp"
#include "nmea_writer.hpp"
#include "require.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gnsshaltest {

using andarium::gnss::Env;
using andarium::gnss::Location;
using andarium::gnss::Sink;
using andarium::gnss::Status;
using uml::gnss::Fix;
using uml::testing::require;
using namespace std::chrono_literals;

// 2026-11-10 13:45:30.000 UTC.
constexpr std::int64_t baseNs = 1'794'318'330'000'000'000;
constexpr std::int64_t second = 1'000'000'000;
constexpr auto limit = 10s;

/* The operating system as the engine sees it: a pipe for the receiver, a
 * wakelock that counts, and a clock the test moves. */
struct World {
    std::mutex mutex;
    std::condition_variable changed;

    // The receiver. The test writes to `feed`; the engine reads `receiver`.
    int receiver = -1;
    int feed = -1;
    int opens = 0;
    bool deviceMissing = false;

    int wakelocksHeld = 0;
    int wakelocksTaken = 0;
    bool wakelockUnavailable = false;

    std::atomic<std::int64_t> realtime{baseNs};
    // The guest's uptime. Each read advances it by `boottimeStep`, so a test
    // that writes several epochs at once can say how much time passes between
    // the epochs as the engine handles them.
    std::atomic<std::int64_t> boottime{3600 * second};
    std::atomic<std::int64_t> boottimeStep{0};

    // What the engine reported. `log` has every call in the order it was made:
    // "ready", "on", "begin", "end", "off", "location", "nmea" and "sv".
    std::vector<std::string> log;
    int readies = 0;
    std::vector<Status> statuses;
    std::vector<Location> locations;
    std::vector<std::string> sentences;
    std::vector<std::int64_t> sentenceTimes;
    int svStatuses = 0;
    std::vector<std::string> logs;  // what the engine said about itself (Env::log)

    /* What the sinks reach the world through. The reporter delivers what it
     * still holds after the engine is gone, so a call may come after the case
     * that made the world has ended: it finds the world gone and is ignored. */
    struct Alive {
        std::mutex mutex;
        World* world = nullptr;
    };
    std::shared_ptr<Alive> alive = std::make_shared<Alive>();

    World() { alive->world = this; }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    static const char* statusName(Status status)
    {
        switch (status) {
        case Status::session_begin:
            return "begin";
        case Status::session_end:
            return "end";
        case Status::engine_on:
            return "on";
        case Status::engine_off:
            return "off";
        }
        return "?";
    }

    Env env()
    {
        Env result;
        result.open_device = [this] {
            std::lock_guard lock(mutex);
            if (deviceMissing)
                return -2;
            int fds[2];
            require(pipe(fds) == 0);
            fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
            fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
            if (feed >= 0)
                ::close(feed);
            receiver = fds[0];
            feed = fds[1];
            ++opens;
            return fds[0];
        };
        result.hold_wakelock = [this]() -> std::shared_ptr<void> {
            std::lock_guard lock(mutex);
            if (wakelockUnavailable)
                return nullptr;
            ++wakelocksHeld;
            ++wakelocksTaken;
            return std::shared_ptr<void>(this, [](World* world) {
                std::lock_guard guard(world->mutex);
                --world->wakelocksHeld;
                world->changed.notify_all();
            });
        };
        result.realtime_ns = [this] { return realtime.load(); };
        result.boottime_ns = [this] { return boottime.fetch_add(boottimeStep.load()); };
        result.log = [this](const std::string& message) {
            std::lock_guard lock(mutex);
            logs.push_back(message);
            changed.notify_all();
        };
        return result;
    }

    Sink sink()
    {
        // Each call holds `alive` while it records, which takes no lock of
        // another sink, so the world cannot go while a call is in it.
        const auto reach = [alive = alive](auto record) {
            return [alive, record](auto&&... arguments) {
                std::lock_guard guard(alive->mutex);
                if (alive->world == nullptr)
                    return;
                World& world = *alive->world;
                std::lock_guard lock(world.mutex);
                record(world, arguments...);
                world.changed.notify_all();
            };
        };
        Sink result;
        result.ready = reach([](World& world) {
            ++world.readies;
            world.log.push_back("ready");
        });
        result.status = reach([](World& world, Status status) {
            world.statuses.push_back(status);
            world.log.push_back(statusName(status));
        });
        result.location = reach([](World& world, const Location& location) {
            world.locations.push_back(location);
            world.log.push_back("location");
        });
        result.nmea = reach([](World& world, std::int64_t at, const std::string& sentence) {
            world.sentenceTimes.push_back(at);
            world.sentences.push_back(sentence);
            world.log.push_back("nmea");
        });
        result.sv_status = reach([](World& world) {
            ++world.svStatuses;
            world.log.push_back("sv");
        });
        return result;
    }

    /* Waits until `done` holds, at most `limit`. */
    template<typename Predicate>
    bool wait(Predicate done)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, done);
    }

    void write(const std::string& text)
    {
        std::lock_guard lock(mutex);
        require(feed >= 0);
        require(::write(feed, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    }

    /* The receiver dies: the writing end closes. */
    void hangUp()
    {
        std::lock_guard lock(mutex);
        if (feed >= 0)
            ::close(feed);
        feed = -1;
    }

    /* True when the engine closed its end of the pipe. */
    bool receiverClosed()
    {
        std::lock_guard lock(mutex);
        if (feed < 0)
            return true;
        // Writing to a pipe whose reader is gone fails with EPIPE (SIGPIPE is
        // ignored by the test's main). A line feed is nothing to the parser.
        const char byte = '\n';
        return ::write(feed, &byte, 1) < 0 && errno == EPIPE;
    }

    /* Waits until the engine has read everything written to the receiver. */
    bool drained()
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            int count = -1;
            {
                std::lock_guard lock(mutex);
                require(receiver >= 0 && ioctl(receiver, FIONREAD, &count) == 0);
            }
            if (count == 0)
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    ~World()
    {
        {
            std::lock_guard guard(alive->mutex);
            alive->world = nullptr;
        }
        if (feed >= 0)
            ::close(feed);
    }
};

inline Fix preciseFix(std::int64_t utcNs, double latitude = 35.6895)
{
    Fix fix;
    fix.utcNs = utcNs;
    fix.latitude = latitude;
    fix.longitude = 139.6917;
    fix.altitude = 40.0;
    fix.ellipsoidalAltitude = 80.5;
    fix.horizontalAccuracy = 8.0;
    fix.verticalAccuracy = 12.0;
    fix.speed = 1.5;
    fix.course = 90.0;
    return fix;
}

inline std::string epoch(const Fix& fix)
{
    return uml::gnss::nmeaEpoch(fix, fix.utcNs);
}

inline void heldWakelocks(World& world, int expected)
{
    std::lock_guard lock(world.mutex);
    require(world.wakelocksHeld == expected);
}

} // namespace gnsshaltest
