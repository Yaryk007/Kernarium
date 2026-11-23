// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumMemoryPressure.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace kernarium::memory_pressure;
using uml::runtime::PressureLevel;
using uml::runtime::PressureReaction;
using uml::runtime::PressureReport;
using uml::testing::require;
using uml::testing::waitUntil;

struct Reports final {
    std::vector<std::pair<int, int>> heard;
    int callbacks = 0;
};

void record(void* context, int level, int response)
{
    auto& reports = *static_cast<Reports*>(context);
    ++reports.callbacks;
    reports.heard.emplace_back(level, response);
}

void theValuesAreTheApplicationsWords()
{
    require(levelValue(PressureLevel::normal) == KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL);
    require(levelValue(PressureLevel::warn) == KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING);
    require(levelValue(PressureLevel::critical) == KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL);
    // The numbers an application built against runtime API 41 reads.
    require(KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL == 0 && KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING == 1
            && KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL == 2,
        "the levels");
    require(responseValue(PressureReaction::none) == KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE);
    require(responseValue(PressureReaction::responding) == KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED);
    require(responseValue(PressureReaction::underway) == KERNARIUM_RUNTIME_MEMORY_RESPONSE_UNDER_WAY);
    require(KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE == 0 && KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED == 1
            && KERNARIUM_RUNTIME_MEMORY_RESPONSE_WITHHELD == 2 && KERNARIUM_RUNTIME_MEMORY_RESPONSE_UNDER_WAY == 3,
        "the responses");
}

void theReceiverPassesEachReportInOrder()
{
    Receiver receiver;
    Reports reports;
    receiver.publish({PressureLevel::warn, PressureReaction::responding});
    require(reports.callbacks == 0, "no callback is set yet");

    receiver.set(record, &reports);
    receiver.publish({PressureLevel::critical, PressureReaction::responding});
    receiver.publish({PressureLevel::warn, PressureReaction::none});
    receiver.publish({PressureLevel::critical, PressureReaction::underway});
    receiver.publish({PressureLevel::normal, PressureReaction::none});
    require((reports.heard
                == std::vector<std::pair<int, int>> {
                    {KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED},
                    {KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING, KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE},
                    {KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_UNDER_WAY},
                    {KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE},
                }),
        "every report, repeats included, in the order they arrived");
}

void aReplacedOrClearedCallbackHearsNothingMore()
{
    Receiver receiver;
    Reports first;
    Reports second;
    receiver.set(record, &first);
    receiver.publish({PressureLevel::warn, PressureReaction::responding});
    receiver.set(record, &second);
    receiver.publish({PressureLevel::critical, PressureReaction::responding});
    receiver.set(nullptr, nullptr);
    receiver.publish({PressureLevel::normal, PressureReaction::none});
    require(first.callbacks == 1 && second.callbacks == 1);
}

void clearingWaitsForACallInProgress()
{
    struct Slow final {
        std::mutex gate;
        std::condition_variable changed;
        bool inside = false;
        bool release = false;
        std::atomic<bool> finished {false};
    } slow;
    Receiver receiver;
    receiver.set(
        [](void* context, int, int) {
            auto& slow = *static_cast<Slow*>(context);
            std::unique_lock lock {slow.gate};
            slow.inside = true;
            slow.changed.notify_all();
            (void)slow.changed.wait_for(lock, std::chrono::seconds {20}, [&] { return slow.release; });
            lock.unlock();
            slow.finished.store(true);
        },
        &slow);
    std::thread publishing {[&] { receiver.publish({PressureLevel::critical, PressureReaction::responding}); }};
    {
        std::unique_lock lock {slow.gate};
        require(slow.changed.wait_for(lock, std::chrono::seconds {20}, [&] { return slow.inside; }),
            "the callback never ran");
    }
    std::atomic<bool> cleared {false};
    std::thread clearing {[&] {
        receiver.set(nullptr, nullptr);
        require(slow.finished.load(), "the clear returned inside the call");
        cleared.store(true);
    }};
    require(!waitUntil([&] { return cleared.load(); }, std::chrono::milliseconds {300}), "the clear did not wait");
    {
        const std::lock_guard lock {slow.gate};
        slow.release = true;
    }
    slow.changed.notify_all();
    clearing.join();
    publishing.join();
    require(cleared.load());
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] {
        {"the-values-are-the-applications-words", theValuesAreTheApplicationsWords},
        {"the-receiver-passes-each-report-in-order", theReceiverPassesEachReportInOrder},
        {"a-replaced-or-cleared-callback-hears-nothing-more", aReplacedOrClearedCallbackHearsNothingMore},
        {"clearing-waits-for-a-call-in-progress", clearingWaitsForACallInProgress},
    };
    return uml::testing::runNamedTests(tests);
}
