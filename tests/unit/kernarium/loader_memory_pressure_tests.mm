// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The last part of the generated source of kernarium-loader-memory-pressure:
 * the loader's file-local forwarders are exercised by compiling the loader's
 * own source into the same translation unit, between the head and this file
 * (cmake/concatenate_sources.cmake joins them; nothing includes an
 * implementation file). A fake runtime table stands in for the framework.
 *
 *   loader_memory_pressure_tests          what the loader delivers, and to whom
 *   loader_memory_pressure_tests --exit   a report forwarded after exit() has
 *                                         destroyed ordinary statics reaches
 *                                         the handler that was set
 *   loader_memory_pressure_tests --table  which runtime tables the loader takes
 *   loader_memory_pressure_tests --settle what the settling calls report: the
 *                                         runtime's envelope and handle, and
 *                                         the loader's forwarding of them
 */
#include "KernariumMemoryPressure.hpp"
#include "KernariumSettle.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

kernarium::memory_pressure::Receiver receiver;
bool exitMode = false;
/* What the handler of the exit mode heard: atomics, which have no destructor
 * to run before the hook below. */
std::atomic<int> exitHeard {0};
std::atomic<int> exitLevel {-1};
std::atomic<int> exitResponse {-1};

void fakeSet(kernarium_runtime_memory_pressure_callback callback, void* context)
{
    receiver.set(callback, context);
}

[[noreturn]] void fail(const char* message)
{
    std::fprintf(stderr, "loader memory pressure test failed: %s\n", message);
    std::fflush(stderr);
    std::_Exit(1);
}

void check(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

/* The runtime's thread, as it makes the reports, while the main queue is
 * blocked by the caller: they queue. */
void publishFromRuntimeThread(int count, uml::runtime::PressureLevel level)
{
    std::thread runtime([count, level] {
        for (int index = 0; index < count; ++index)
            receiver.publish({level, uml::runtime::PressureReaction::responding});
    });
    runtime.join();
}

void deliveryTests()
{
    __block std::vector<std::pair<int, int>> heard;
    __block bool onMainQueue = true;
    kernarium_runtime_loader_set_memory_pressure_handler(^(int level, int response) {
        onMainQueue = onMainQueue && NSThread.isMainThread;
        heard.emplace_back(level, response);
    });

    /* The reports reach the handler on the main queue, in order, and none
     * before the queue runs. */
    receiver.publish({uml::runtime::PressureLevel::warn, uml::runtime::PressureReaction::none});
    receiver.publish({uml::runtime::PressureLevel::critical, uml::runtime::PressureReaction::responding});
    receiver.publish({uml::runtime::PressureLevel::critical, uml::runtime::PressureReaction::underway});
    receiver.publish({uml::runtime::PressureLevel::normal, uml::runtime::PressureReaction::none});
    check(heard.empty(), "a report reached the handler before the main queue ran");

    dispatch_async(dispatch_get_main_queue(), ^{
        const std::vector<std::pair<int, int>> expected {
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING, KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE},
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED},
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_CRITICAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_UNDER_WAY},
            {KERNARIUM_RUNTIME_MEMORY_PRESSURE_NORMAL, KERNARIUM_RUNTIME_MEMORY_RESPONSE_NONE},
        };
        check(heard == expected, "the reports did not arrive in order, once each");
        check(onMainQueue, "a report reached the handler off the main queue");

        /* One queued block per report, and clearing or replacing the handler
         * does not cancel them: while the main queue is busy (here, this
         * block), a thousand reports queue and all reach the handler they were
         * made for. */
        __block int oldHeard = 0;
        __block int newHeard = 0;
        kernarium_runtime_loader_set_memory_pressure_handler(^(int, int) { ++oldHeard; });
        constexpr int queued = 1000;
        publishFromRuntimeThread(queued, uml::runtime::PressureLevel::warn);
        kernarium_runtime_loader_set_memory_pressure_handler(nil);
        kernarium_runtime_loader_set_memory_pressure_handler(^(int, int) { ++newHeard; });
        check(oldHeard == 0, "a queued report ran while the main queue was busy");
        dispatch_async(dispatch_get_main_queue(), ^{
            check(oldHeard == queued, "a queued report was lost or doubled by the replacement");
            check(newHeard == 0, "a report queued for the old handler reached the new one");
            publishFromRuntimeThread(5, uml::runtime::PressureLevel::critical);
            dispatch_async(dispatch_get_main_queue(), ^{
                check(newHeard == 5, "a report after the replacement missed the new handler");
                check(oldHeard == queued, "a report after the replacement reached the old handler");
                kernarium_runtime_loader_set_memory_pressure_handler(nil);
                receiver.publish({uml::runtime::PressureLevel::critical, uml::runtime::PressureReaction::responding});
                std::puts("loader memory pressure: delivery ok");
                std::fflush(stdout);
                std::_Exit(0);
            });
        });
    });
}

void unusedEntry() {}

/* A table of the given version with every entry set. */
kernarium_runtime_api fullTable(uint32_t version)
{
    kernarium_runtime_api api {};
    api.abi_version = version;
    api.struct_size = sizeof(api);
    api.guest_page_size = 4096U;
    api.guest_flavor = KERNARIUM_RUNTIME_GUEST_LINUX;
    void (*entry)() = unusedEntry;
    constexpr std::size_t first = offsetof(kernarium_runtime_api, run);
    check((sizeof(api) - first) % sizeof(entry) == 0, "the table has an entry that is no function pointer");
    for (std::size_t offset = first; offset < sizeof(api); offset += sizeof(entry))
        std::memcpy(reinterpret_cast<unsigned char*>(&api) + offset, &entry, sizeof(entry));
    return api;
}

/* The loader takes the table of its own version and refuses one of any other,
 * a table too short for the entries it reads, and one that lacks an entry
 * added since version 39. */
void tableTests()
{
    kernarium_runtime_api api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    check(validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "the table of the loader's version was refused");
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_ANDROID), "a table of another guest flavor was taken");
    check(!validApi(nullptr, KERNARIUM_RUNTIME_GUEST_LINUX), "a null table was taken");

    for (const uint32_t version : {39U, 40U, 41U, 42U, 43U, 44U, 45U, KERNARIUM_RUNTIME_API_VERSION + 1U}) {
        api = fullTable(version);
        check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table of another version was taken");
    }

    api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    api.struct_size = offsetof(kernarium_runtime_api, set_memory_pressure_callback);
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table without the last entry was taken");

    api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    api.plan_camera_reserve = nullptr;
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table without plan_camera_reserve was taken");

    api = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    api.set_memory_pressure_callback = nullptr;
    check(!validApi(&api, KERNARIUM_RUNTIME_GUEST_LINUX), "a table without set_memory_pressure_callback was taken");

    std::puts("loader table: ok");
    std::fflush(stdout);
}

/* ---- The calls that write the guest's storage back. ---- */

/* A handle the fake library is given and never dereferences. */
uml_emulator* fakeHandle()
{
    return reinterpret_cast<uml_emulator*>(static_cast<std::uintptr_t>(0x1000));
}

constexpr uint32_t untouched = 0xdeadU;

kernarium_runtime_settle blankReport()
{
    return {
        .struct_size = sizeof(kernarium_runtime_settle),
        .reserved = untouched,
        .guest_storage = 99U,
        .guest_suspension = 99U,
    };
}

bool reportIsUntouched(const kernarium_runtime_settle& report)
{
    return report.struct_size == sizeof(kernarium_runtime_settle) && report.reserved == untouched &&
        report.guest_storage == 99U && report.guest_suspension == 99U;
}

bool reportIsNoGuest(const kernarium_runtime_settle& report)
{
    return report.reserved == 0U && report.guest_storage == KERNARIUM_RUNTIME_GUEST_STORAGE_NO_GUEST &&
        report.guest_suspension == KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED;
}

void envelopeTests()
{
    using namespace kernarium::settle;

    /* The report is usable when none is asked for, or when it is large enough
     * for what the runtime writes. */
    auto report = blankReport();
    check(usable(nullptr), "no report was refused");
    check(usable(&report), "a report of the structure's size was refused");
    report.struct_size = sizeof(report) + 8U;
    check(usable(&report), "a larger report was refused");
    report.struct_size = sizeof(report) - 1U;
    check(!usable(&report), "a smaller report was taken");

    /* No guest: the report says so, and nothing is called. */
    int calls = 0;
    const auto library = [&](uml_emulator*, uml_emulator_settle*) {
        ++calls;
        return 0;
    };
    report = blankReport();
    check(run(&report, nullptr, library) == 0, "no guest did not return 0");
    check(reportIsNoGuest(report) && calls == 0, "no guest was not reported as such, or the library was called");
    check(run(nullptr, nullptr, library) == 0 && calls == 0, "no guest and no report failed");

    /* A report too small is refused and left alone, and nothing is called. */
    report = blankReport();
    report.struct_size = sizeof(report) - 1U;
    check(run(&report, fakeHandle(), library) == -EINVAL, "a report too small was not refused");
    check(run(&report, nullptr, library) == -EINVAL, "a report too small was taken with no guest");
    check(calls == 0 && report.reserved == untouched && report.guest_storage == 99U,
        "a refused call called the library or wrote its report");

    /* The library is given a report of its own structure's size, and what it
     * does not write is nothing, never a confirmation. */
    uint32_t sizeSeen = 0U;
    uint32_t storageSeen = 99U;
    report = blankReport();
    check(run(&report, fakeHandle(),
              [&](uml_emulator* emulator, uml_emulator_settle* given) {
                  check(emulator == fakeHandle(), "the library was given another handle");
                  sizeSeen = given->struct_size;
                  storageSeen = static_cast<uint32_t>(given->guest_storage);
                  return 0;
              }) == 0,
        "a call that wrote nothing failed");
    check(sizeSeen == sizeof(uml_emulator_settle), "the library's report was not its structure's size");
    check(storageSeen == UML_EMULATOR_GUEST_STORAGE_NOT_REPORTED, "the library's report did not start as not reported");
    check(report.guest_storage == KERNARIUM_RUNTIME_GUEST_STORAGE_NOT_REPORTED &&
            report.guest_suspension == KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_ASKED && report.reserved == 0U,
        "a report the library never wrote claimed something");

    /* What the library reports is copied word for word, with its count. */
    report = blankReport();
    check(run(&report, fakeHandle(),
              [](uml_emulator*, uml_emulator_settle* given) {
                  given->guest_storage = UML_EMULATOR_GUEST_STORAGE_UNCONFIRMED;
                  given->guest_suspension = UML_EMULATOR_GUEST_SUSPENSION_NOT_SUSPENDED;
                  return 3;
              }) == 3,
        "the library's count was not returned");
    check(report.guest_storage == KERNARIUM_RUNTIME_GUEST_STORAGE_UNCONFIRMED &&
            report.guest_suspension == KERNARIUM_RUNTIME_GUEST_SUSPENSION_NOT_SUSPENDED && report.reserved == 0U,
        "the library's report was not copied");

    /* A failure writes nothing, whatever the library left in its report. */
    report = blankReport();
    check(run(&report, fakeHandle(),
              [](uml_emulator*, uml_emulator_settle* given) {
                  given->guest_storage = UML_EMULATOR_GUEST_STORAGE_CONFIRMED;
                  return -UML_EMULATOR_INVALID_ARGUMENT;
              }) == -UML_EMULATOR_INVALID_ARGUMENT,
        "a failure was not returned");
    check(reportIsUntouched(report), "a failed call wrote the report");

    /* No report asked for: the library is still called. */
    calls = 0;
    check(run(nullptr, fakeHandle(), library) == 0 && calls == 1, "a call without a report did not reach the library");
}

void gateTests()
{
    using namespace kernarium::settle;
    Gate gate;
    int calls = 0;
    uml_emulator* handleSeen = nullptr;
    const auto library = [&](uml_emulator* emulator, uml_emulator_settle* given) {
        ++calls;
        handleSeen = emulator;
        given->guest_storage = UML_EMULATOR_GUEST_STORAGE_CONFIRMED;
        return 0;
    };

    /* Nothing runs: no guest. */
    auto report = blankReport();
    check(sync(gate, &report, library) == 0 && reportIsNoGuest(report) && calls == 0, "a gate with no emulator was not no guest");

    /* The emulator that runs is the one the call reaches. */
    gate.open(fakeHandle());
    report = blankReport();
    check(sync(gate, &report, library) == 0 && calls == 1 && handleSeen == fakeHandle(), "the call did not reach the emulator");
    check(report.guest_storage == KERNARIUM_RUNTIME_GUEST_STORAGE_CONFIRMED, "the call's report was lost");
    report.struct_size = sizeof(report) - 1U;
    check(sync(gate, &report, library) == -EINVAL && calls == 1, "a report too small reached the library");

    /* A sync in flight keeps the handle: the registration that ends the guest
     * waits for it, and a call that comes meanwhile finds no guest. */
    std::mutex mutex;
    std::condition_variable changed;
    bool inside = false;
    bool leave = false;
    std::atomic<bool> closed {false};
    std::thread syncing([&] {
        auto own = blankReport();
        (void)sync(gate, &own, [&](uml_emulator*, uml_emulator_settle*) {
            std::unique_lock lock(mutex);
            inside = true;
            changed.notify_all();
            changed.wait(lock, [&] { return leave; });
            return 0;
        });
    });
    {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, std::chrono::seconds(10), [&] { return inside; }), "the sync never started");
    }
    std::thread closing([&] {
        gate.close();
        closed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    check(!closed.load(), "the guest ended while a sync still used its handle");
    report = blankReport();
    int before = calls;
    check(sync(gate, &report, library) == 0 && reportIsNoGuest(report) && calls == before,
        "a sync that came while the guest was ending reached the library");
    {
        const std::lock_guard lock(mutex);
        leave = true;
    }
    changed.notify_all();
    syncing.join();
    closing.join();
    check(closed.load(), "the guest never ended once the sync returned");

    /* A guest that runs after another has ended is a new registration. */
    gate.open(fakeHandle());
    report = blankReport();
    check(sync(gate, &report, library) == 0 && calls == before + 1, "a new registration was not reached");
    gate.close();
}

void settleTests()
{
    envelopeTests();
    gateTests();

    /* The loader, with no runtime loaded: no guest to settle, so a report that
     * fits says so and the call returns -1; one that does not fit is refused as
     * the runtime refuses it, untouched; none asked for is fine. */
    selectedApi.store(nullptr);
    using Call = int (*)(kernarium_runtime_settle*);
    const std::pair<const char*, Call> loaderCalls[] = {
        {"sync", kernarium_runtime_loader_sync},
        {"quiesce", kernarium_runtime_loader_quiesce},
        {"quiesce_resident", kernarium_runtime_loader_quiesce_resident},
    };
    for (const auto& [name, call] : loaderCalls) {
        auto report = blankReport();
        check(call(&report) == -1 && reportIsNoGuest(report), name);
        report = blankReport();
        report.struct_size = sizeof(report) + 4U;
        check(call(&report) == -1 && report.guest_storage == KERNARIUM_RUNTIME_GUEST_STORAGE_NO_GUEST, name);
        report = blankReport();
        report.struct_size = sizeof(report) - 1U;
        check(call(&report) == -EINVAL, name);
        check(report.reserved == untouched && report.guest_storage == 99U && report.guest_suspension == 99U,
            "a report too small was written by the loader");
        check(call(nullptr) == -1, name);
    }

    /* With a table: each call reaches its own entry, with the caller's pointer
     * and the entry's answer. */
    static kernarium_runtime_settle* seenBySync;
    static kernarium_runtime_settle* seenByQuiesce;
    static kernarium_runtime_settle* seenByResident;
    static kernarium_runtime_api table;
    table = fullTable(KERNARIUM_RUNTIME_API_VERSION);
    table.sync = [](kernarium_runtime_settle* settle) {
        seenBySync = settle;
        return 7;
    };
    table.quiesce = [](kernarium_runtime_settle* settle) {
        seenByQuiesce = settle;
        return 8;
    };
    table.quiesce_resident = [](kernarium_runtime_settle* settle) {
        seenByResident = settle;
        return 9;
    };
    selectedApi.store(&table);
    auto report = blankReport();
    check(kernarium_runtime_loader_sync(&report) == 7 && seenBySync == &report, "the table's sync did not get the caller's report");
    check(kernarium_runtime_loader_quiesce(&report) == 8 && seenByQuiesce == &report, "the table's quiesce did not get the caller's report");
    check(kernarium_runtime_loader_quiesce_resident(&report) == 9 && seenByResident == &report,
        "the table's quiesce_resident did not get the caller's report");
    check(reportIsUntouched(report), "the loader wrote a report that the runtime owns");
    check(kernarium_runtime_loader_sync(nullptr) == 7 && seenBySync == nullptr, "no report was not forwarded as none");
    selectedApi.store(nullptr);

    std::puts("settle plumbing: ok");
    std::fflush(stdout);
}

} // namespace

/* Registered first by the head: runs after the loader's statics were
 * destroyed, when the pressure queue may still call into the loader. The
 * report must still reach the handler the application set, so the hook
 * serves the main queue (this is the main thread) until the handler has run:
 * a handler or a lock that the loader kept in an ordinary global is gone by
 * now, and nothing runs. */
extern "C" void loaderTestAfterStaticDestruction(void)
{
    if (!exitMode)
        return;
    std::fputs("forwarding a report after static destruction\n", stdout);
    std::fflush(stdout);
    forwardMemoryPressure(
        nullptr,
        KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING,
        KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (exitHeard.load() == 0 && std::chrono::steady_clock::now() < deadline)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    if (exitHeard.load() != 1 ||
        exitLevel.load() != KERNARIUM_RUNTIME_MEMORY_PRESSURE_WARNING ||
        exitResponse.load() != KERNARIUM_RUNTIME_MEMORY_RESPONSE_STARTED) {
        std::fputs("loader memory pressure test failed: the handler did not hear the report after static destruction\n", stderr);
        std::fflush(stderr);
        std::_Exit(3);
    }
    std::fputs("loader memory pressure: exit ok\n", stdout);
    std::fflush(stdout);
    std::_Exit(0);
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        if (argc > 1 && std::strcmp(argv[1], "--table") == 0) {
            tableTests();
            return 0;
        }
        if (argc > 1 && std::strcmp(argv[1], "--settle") == 0) {
            settleTests();
            return 0;
        }
        static kernarium_runtime_api api {};
        api.set_memory_pressure_callback = fakeSet;
        selectedApi.store(&api);
        if (argc > 1 && std::strcmp(argv[1], "--exit") == 0) {
            exitMode = true;
            int captured = 42;
            kernarium_runtime_loader_set_memory_pressure_handler(^(int level, int response) {
                (void)captured;
                exitLevel.store(level);
                exitResponse.store(response);
                exitHeard.fetch_add(1);
            });
            /* Exits with the handler set: the status is the hook's, 0 when the
             * handler heard the report, 3 when it did not, and 2 if the hook
             * never ran. */
            std::exit(2);
        }
        deliveryTests();
    }
    dispatch_main();
}
