// SPDX-License-Identifier: GPL-2.0-or-later

/* The host GPU gives the guest a budget the pool enforces in bytes, and the
 * ledger says whose bytes they are. A Virgl resource is credited to every
 * context it is attached to and counted once in the total, a blob to the
 * context that made it, and a Venus context's device memory to the context
 * the renderer reports it for, without counting a Venus context's blobs
 * again. The guest kernel names the process that created a context, and the
 * snapshot adds up a process's contexts, leaves out the holders that are
 * active or small, and ranks the rest. The test drives the ledger with its
 * own clock and with the command sequences a guest sends. */

#include "context_memory_ledger.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using uml::gpu::ContextMemoryLedger;
using uml::gpu::parseContextOwner;
using uml::testing::require;
using Clock = ContextMemoryLedger::Clock;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint64_t mebibyte = 1U << 20;

const ContextMemoryLedger::Holder* find(
    const ContextMemoryLedger::Snapshot& snapshot, std::uint32_t pid)
{
    for (const auto& holder : snapshot.holders) {
        if (holder.pid == pid)
            return &holder;
    }
    return nullptr;
}

/* The device reserves a resource's room, then tells the ledger how big the
 * renderer made it. */
void made(
    ContextMemoryLedger& ledger, std::uint32_t resource, std::uint64_t bytes)
{
    ledger.reserveResource(resource);
    ledger.setResourceBytes(resource, bytes);
}

void madeBlob(
    ContextMemoryLedger& ledger,
    std::uint32_t resource,
    std::uint32_t context,
    std::uint64_t bytes)
{
    ledger.reserveResource(resource);
    ledger.blobMade(resource, context, bytes);
}

/* The debug name a kernel with the topic gives a context of process `pid`. */
std::string owned(std::uint32_t pid, const char* comm,
    const char* thread = "RenderThread")
{
    return std::string(thread) + "|pid=" + std::to_string(pid) +
        "|comm=" + comm;
}

bool attach(
    ContextMemoryLedger& ledger, std::uint32_t context, std::uint32_t resource)
{
    return ledger.resourceAttached(context, resource);
}

ContextMemoryLedger::Snapshot all(
    const ContextMemoryLedger& ledger, Clock::time_point now)
{
    return ledger.snapshot(now, 100, Clock::duration::zero(), 0);
}

void namesAreParsedOnlyWhenTheKernelGaveAProcess()
{
    const auto named = parseContextOwner("RenderThread|pid=1234|comm=org.example");
    require(named && named->pid == 1234 && named->comm == "org.example",
        "a name with a process was not parsed");
    const auto bare = parseContextOwner("|pid=7|comm=x");
    require(bare && bare->pid == 7 && bare->comm == "x",
        "a name cut to nothing was not parsed");
    const auto colons = parseContextOwner("a|pid=462|comm=binder:462_1");
    require(colons && colons->pid == 462 && colons->comm == "binder:462_1",
        "a command name with a colon was cut");
    const auto spaces = parseContextOwner("t|pid=5|comm=two words");
    require(spaces && spaces->comm == "two words",
        "a command name with a space was cut");

    /* The kernel's suffix is the last one: a name that imitates it, whether
     * the context's own or a thread's, does not move the process. */
    const auto forged = parseContextOwner(
        "|pid=1|comm=victim|pid=2222|comm=real");
    require(forged && forged->pid == 2222 && forged->comm == "real",
        "an imitation before the suffix beat the suffix");

    /* Every name a kernel without the topic gives is a thread's name, and a
     * suffix that is not complete names no process. */
    for (const char* thread : {"RenderThread", "binder:462_1",
             "Chrome_InProcGp", "GrallocUploadTh", "", "10:allocator",
             "|pid=5", "|comm=x", "|pid=|comm=x", "|pid=5|comm=",
             "|pid=0|comm=x", "|pid=x5|comm=y", "|pid=-5|comm=y",
             "|pid=99999999999|comm=y", "|pid=4294967296|comm=y",
             "|comm=x|pid=5", "pid=5|comm=x"}) {
        require(!parseContextOwner(thread).has_value(),
            std::string("a thread name was taken for a process: ") + thread);
    }
    require(parseContextOwner("t|pid=4294967295|comm=max")->pid ==
            4294967295U,
        "the largest process ID was refused");
}

void aResourceIsCreditedToEveryContextThatHoldsIt()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "allocator"), false, origin);
    ledger.contextCreated(3, owned(20, "app"), false, origin);
    ledger.contextCreated(4, owned(30, "compositor"), false, origin);

    made(ledger, 100, 4 * mebibyte);
    require(ledger.totalBytes() == 0,
        "a resource no context holds counted in the total");
    attach(ledger, 2, 100);
    attach(ledger, 3, 100);
    attach(ledger, 3, 100);
    attach(ledger, 4, 100);
    auto snapshot = all(ledger, origin);
    require(ledger.totalBytes() == 4 * mebibyte &&
            snapshot.totalBytes == 4 * mebibyte,
        "a resource three contexts hold was counted more than once");
    for (const std::uint32_t pid : {10U, 20U, 30U}) {
        const auto* holder = find(snapshot, pid);
        require(holder && holder->bytes == 4 * mebibyte,
            "a context that holds the resource was not credited");
    }

    ledger.resourceDetached(2, 100);
    snapshot = all(ledger, origin);
    require(find(snapshot, 10) == nullptr,
        "a context that let go of a resource kept the credit");
    require(ledger.totalBytes() == 4 * mebibyte,
        "a resource held by two contexts left the total");
    ledger.resourceDetached(3, 100);
    ledger.resourceDetached(4, 100);
    require(ledger.totalBytes() == 0,
        "a resource no context holds stayed in the total");
    ledger.resourceDetached(4, 100);
    require(ledger.totalBytes() == 0, "a second detach changed the total");

    attach(ledger, 3, 100);
    ledger.resourceReleased(100);
    require(ledger.totalBytes() == 0 && all(ledger, origin).holders.empty(),
        "an unreferenced resource was still credited");
    attach(ledger, 3, 100);
    require(ledger.totalBytes() == 0,
        "a resource the guest destroyed was attached again");
}

void anAttachSaysWhetherItMadeTheContextAHolder()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "a"), false, origin);
    ledger.contextCreated(3, owned(20, "b"), false, origin);
    made(ledger, 100, mebibyte);

    require(attach(ledger, 2, 100), "the first attach did not hold");
    require(!attach(ledger, 2, 100), "a repeated attach held again");
    require(attach(ledger, 3, 100), "a second holder was refused");
    require(!attach(ledger, 3, 100), "a repeated second attach held again");
    require(!attach(ledger, 4, 100), "an unknown context held a resource");
    require(!attach(ledger, 2, 101), "an unknown resource was held");
    ledger.resourceDetached(3, 100);
    require(attach(ledger, 3, 100), "a detached context could not attach");
    require(ledger.totalBytes() == mebibyte, "an attach counted twice");
}

void storageThatArrivesAfterTheAttachIsCredited()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "a"), false, origin);
    ledger.contextCreated(3, owned(20, "b"), false, origin);
    ledger.reserveResource(100);
    ledger.reserveResource(100);
    require(attach(ledger, 2, 100) && attach(ledger, 3, 100), "setup");
    require(ledger.totalBytes() == 0, "a resource of no size counted");

    ledger.setResourceBytes(100, 6 * mebibyte);
    require(ledger.totalBytes() == 6 * mebibyte &&
            find(all(ledger, origin), 10)->bytes == 6 * mebibyte &&
            find(all(ledger, origin), 20)->bytes == 6 * mebibyte,
        "the size of a held resource was not credited to its holders");
    ledger.setResourceBytes(100, 2 * mebibyte);
    require(ledger.totalBytes() == 2 * mebibyte &&
            find(all(ledger, origin), 10)->bytes == 2 * mebibyte,
        "a new size was not rebased");
    ledger.setResourceBytes(101, 5);
    require(ledger.totalBytes() == 2 * mebibyte,
        "the size of an unknown resource counted");
    ledger.resourceReleased(100);
    require(ledger.totalBytes() == 0 && all(ledger, origin).holders.empty(),
        "a released resource kept its credit");

    /* A reservation the command gave back leaves nothing. */
    ledger.reserveResource(102);
    ledger.resourceReleased(102);
    require(!attach(ledger, 2, 102) && ledger.totalBytes() == 0,
        "a reservation that was given back held something");
    ledger.blobMade(102, 2, mebibyte);
    require(ledger.totalBytes() == 0, "a blob nobody reserved counted");
    madeBlob(ledger, 103, 2, mebibyte);
    ledger.blobMade(103, 3, 4 * mebibyte);
    require(ledger.totalBytes() == mebibyte,
        "a blob that was held already was made again");
}

void aContextThatGoesTakesItsCreditWithIt()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "a"), false, origin);
    ledger.contextCreated(3, owned(20, "b"), false, origin);
    made(ledger, 100, 8 * mebibyte);
    made(ledger, 101, 2 * mebibyte);
    attach(ledger, 2, 100);
    attach(ledger, 3, 100);
    attach(ledger, 2, 101);

    ledger.contextDestroyed(2);
    auto snapshot = all(ledger, origin);
    require(snapshot.totalBytes == 8 * mebibyte,
        "the resource only the destroyed context held stayed counted");
    require(find(snapshot, 20) && find(snapshot, 20)->bytes == 8 * mebibyte &&
            !find(snapshot, 10),
        "the survivor lost its credit or the dead context kept its own");
    ledger.contextDestroyed(3);
    require(ledger.totalBytes() == 0, "no context held anything");
    ledger.contextDestroyed(3);

    /* A context's ID may come back with another process. */
    ledger.contextCreated(2, owned(11, "c"), false, origin);
    require(all(ledger, origin).holders.empty(),
        "a new context started with the old one's credit");
}

void aBlobBelongsToItsCreatorUnlessTheCreatorIsVenus()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "gl"), false, origin);
    ledger.contextCreated(3, owned(20, "vk"), true, origin);

    madeBlob(ledger, 200, 2, 6 * mebibyte);
    require(ledger.totalBytes() == 6 * mebibyte &&
            find(all(ledger, origin), 10)->bytes == 6 * mebibyte,
        "a blob was not credited to the context that made it");

    madeBlob(ledger, 201, 3, 16 * mebibyte);
    require(ledger.totalBytes() == 6 * mebibyte,
        "a Venus context's blob was counted besides its device memory");
    ledger.venusMemory(3, static_cast<std::int64_t>(16 * mebibyte));
    require(ledger.totalBytes() == 22 * mebibyte,
        "the Venus context's device memory was not counted");

    /* A GL context that imports the Venus blob holds none of its bytes. */
    attach(ledger, 2, 201);
    require(find(all(ledger, origin), 10)->bytes == 6 * mebibyte,
        "an importer of a Venus blob was credited with its size");
    ledger.resourceReleased(200);
    ledger.resourceReleased(201);
    require(ledger.totalBytes() == 16 * mebibyte,
        "released blobs left bytes behind");
}

void venusMemoryFollowsTheRenderersReports()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(5, owned(40, "game"), true, origin);
    ledger.contextCreated(6, owned(41, "other"), true, origin);
    ledger.venusMemory(5, 64 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(5, 32 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(6, 8 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(7, 500 * static_cast<std::int64_t>(mebibyte));
    require(ledger.totalBytes() == 104 * mebibyte,
        "a report for a context the ledger never saw was counted");

    ledger.venusMemory(5, -64 * static_cast<std::int64_t>(mebibyte));
    require(find(all(ledger, origin), 40)->bytes == 32 * mebibyte,
        "freed device memory was not subtracted");
    ledger.venusMemory(5, -1000 * static_cast<std::int64_t>(mebibyte));
    require(find(all(ledger, origin), 40) == nullptr &&
            ledger.totalBytes() == 8 * mebibyte,
        "a free larger than the holding went below zero");
    ledger.venusMemory(5, INT64_MIN);
    require(ledger.totalBytes() == 8 * mebibyte,
        "the most negative delta was not clamped");

    ledger.venusMemory(6, 100 * static_cast<std::int64_t>(mebibyte));
    ledger.contextDestroyed(6);
    require(ledger.totalBytes() == 0,
        "a destroyed Venus context's device memory stayed counted");
    ledger.venusMemory(6, -8 * static_cast<std::int64_t>(mebibyte));
    require(ledger.totalBytes() == 0,
        "a free that arrived after its context went was counted");
}

void aProcessIsTheSumOfItsContexts()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(77, "app"), false, origin);
    ledger.contextCreated(3, owned(77, "app"), true, origin);
    ledger.contextCreated(4, "RenderThread", false, origin);
    ledger.contextCreated(5, "RenderThread", false, origin);
    made(ledger, 100, 10 * mebibyte);
    made(ledger, 101, 5 * mebibyte);
    made(ledger, 102, 3 * mebibyte);
    made(ledger, 103, 2 * mebibyte);
    attach(ledger, 2, 100);
    attach(ledger, 3, 101);
    ledger.venusMemory(3, 20 * static_cast<std::int64_t>(mebibyte));
    attach(ledger, 4, 102);
    attach(ledger, 5, 103);

    const auto snapshot = all(ledger, origin);
    require(snapshot.holders.size() == 3,
        "a process and two unnamed contexts were not three holders");
    const auto* app = find(snapshot, 77);
    require(app && app->contexts == 2 && app->bytes == 35 * mebibyte &&
            app->name == "app" && app->contextId == 3,
        "a process was not the sum of its contexts");
    require(snapshot.holders[0].pid == 77 &&
            snapshot.holders[1].bytes == 3 * mebibyte &&
            snapshot.holders[1].pid == 0 && snapshot.holders[1].contextId == 4 &&
            snapshot.holders[1].name == "RenderThread" &&
            snapshot.holders[2].bytes == 2 * mebibyte,
        "the holders were not ranked by bytes with unnamed contexts apart");
    require(snapshot.totalBytes == 40 * mebibyte, "the total was wrong");
}

void aContextNamesItsProcessUntilTheProcessIsGone()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(77, "app"), false, origin);
    ledger.contextCreated(3, owned(77, "app"), true, origin);
    ledger.contextCreated(4, "RenderThread", false, origin);
    require(ledger.processOf(2) == 77 && ledger.processOf(3) == 77,
        "a named context did not name its process");
    require(ledger.processOf(4) == 0 && ledger.processOf(99) == 0,
        "an unnamed or unknown context named a process");
    require(ledger.hasProcess(77) && !ledger.hasProcess(78),
        "the process's contexts were not found");
    ledger.contextDestroyed(2);
    require(ledger.hasProcess(77), "one context's end took its process");
    ledger.contextDestroyed(3);
    require(!ledger.hasProcess(77) && ledger.processOf(3) == 0,
        "a process outlived its contexts");
}

void aHolderIsIdleWhenItsMostRecentContextIs()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(50, "a"), false, origin);
    ledger.contextCreated(3, owned(50, "a"), false, origin);
    ledger.contextCreated(4, owned(51, "b"), false, origin);
    for (std::uint32_t id : {2U, 3U, 4U}) {
        made(ledger, 100 + id, 4 * mebibyte);
        attach(ledger, id, 100 + id);
    }
    ledger.contextActive(3, origin + 8s);
    ledger.contextActive(2, origin + 3s);
    ledger.contextActive(3, origin + 6s);
    ledger.contextActive(99, origin + 6s);

    const auto now = origin + 10s;
    const auto snapshot = ledger.snapshot(now, 10, 5s, 0);
    require(snapshot.holders.size() == 1 && snapshot.holders[0].pid == 51,
        "an active process was offered as idle");
    require(snapshot.holders[0].idle == 10s, "the idle time was wrong");
    const auto unfiltered = all(ledger, now);
    require(find(unfiltered, 50)->idle == 2s,
        "a process was not as idle as its most recent context");
    require(ledger.snapshot(now, 10, 2s, 0).holders.size() == 2 &&
            ledger.snapshot(now, 10, 11s, 0).holders.empty(),
        "the idle bound did not include exactly the quiet holders");
    require(ledger.snapshot(origin, 10, 0s, 0).holders.size() == 2 &&
            find(ledger.snapshot(origin, 10, 0s, 0), 50)->idle == 0s,
        "a clock that ran behind made a negative idle time");
}

/* A cached application that trims its memory runs commands that free it, and
 * stays as idle as it was: freeing is not activity. A context that draws, or
 * that allocates, is active, and a command after a release is credited again.
 * The renderer reports memory after the command ran, and a command may both
 * free and allocate. */
void freeingIsNotActivity()
{
    const auto idleOf = [](const ContextMemoryLedger& ledger,
                            Clock::time_point now, std::uint32_t pid) {
        const auto snapshot = all(ledger, now);
        const auto* holder = find(snapshot, pid);
        require(holder != nullptr, "the holder was not in the snapshot");
        return holder->idle;
    };

    ContextMemoryLedger ledger;
    /* A venus application, a GL one that holds a 4 MiB resource, and one that
     * draws. */
    ledger.contextCreated(2, owned(50, "venus"), true, origin);
    ledger.contextCreated(3, owned(51, "gl"), false, origin);
    ledger.contextCreated(4, owned(52, "draws"), false, origin);
    ledger.venusMemory(2, 8 * static_cast<std::int64_t>(mebibyte));
    made(ledger, 101, 4 * mebibyte);
    require(attach(ledger, 3, 101), "the attach was refused");
    made(ledger, 102, mebibyte);
    require(attach(ledger, 4, 102), "the attach was refused");
    /* What the two keep, so that they stay holders. */
    made(ledger, 120, mebibyte);
    require(attach(ledger, 3, 120), "the attach was refused");
    made(ledger, 121, mebibyte);
    require(attach(ledger, 4, 121), "the attach was refused");

    /* Trimming: a command that frees a Venus allocation, one that releases a
     * resource, and one that shrinks it. */
    ledger.contextActive(2, origin + 20s);
    require(idleOf(ledger, origin + 21s, 50) == 1s,
        "a command was not activity until the renderer said what it did");
    ledger.venusMemory(2, -static_cast<std::int64_t>(mebibyte));
    require(idleOf(ledger, origin + 30s, 50) == 30s,
        "a command that freed memory reset the idle time");
    ledger.contextActive(3, origin + 21s);
    ledger.setResourceBytes(101, 3 * mebibyte);
    require(idleOf(ledger, origin + 30s, 51) == 30s,
        "a command that shrank a resource reset the idle time");
    ledger.contextActive(3, origin + 22s);
    ledger.resourceDetached(3, 101);
    require(idleOf(ledger, origin + 30s, 51) == 30s,
        "a command that dropped a resource reset the idle time");

    /* Drawing is activity, whatever it frees on the way. */
    ledger.contextActive(4, origin + 28s);
    require(idleOf(ledger, origin + 30s, 52) == 2s,
        "a context that draws was not active");
    ledger.contextActive(4, origin + 29s);
    require(idleOf(ledger, origin + 30s, 52) == 1s, "drawing was not credited");
    ledger.resourceReleased(102);
    ledger.contextActive(4, origin + 30s);
    require(idleOf(ledger, origin + 30s, 52) == 0s,
        "a command after a release was not credited");

    /* A command that allocates is activity, in either order with a release. */
    ledger.contextActive(2, origin + 40s);
    ledger.venusMemory(2, 2 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(2, -static_cast<std::int64_t>(mebibyte));
    require(idleOf(ledger, origin + 45s, 50) == 5s,
        "a command that allocated and freed was not activity");
    ledger.contextActive(2, origin + 50s);
    ledger.venusMemory(2, -static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(2, 2 * static_cast<std::int64_t>(mebibyte));
    require(idleOf(ledger, origin + 55s, 50) == 5s,
        "a command that freed and then allocated was not activity");
    /* What a release withdraws is the credit since the last allocation, not
     * the one the allocation made good. */
    ledger.venusMemory(2, -static_cast<std::int64_t>(mebibyte));
    require(idleOf(ledger, origin + 60s, 50) == 10s,
        "a release took back an allocation's activity");

    /* A blob and an attach are allocations. */
    ledger.contextActive(3, origin + 70s);
    made(ledger, 103, 2 * mebibyte);
    require(attach(ledger, 3, 103), "the attach was refused");
    require(idleOf(ledger, origin + 75s, 51) == 5s,
        "a command that attached a resource was not activity");
    ledger.contextActive(3, origin + 80s);
    madeBlob(ledger, 104, 3, mebibyte);
    require(idleOf(ledger, origin + 85s, 51) == 5s,
        "a command that made a blob was not activity");

    /* A release that follows no command, or a zero-byte one, changes
     * nothing, and a context that is quiet only grows idler. */
    ledger.resourceReleased(104);
    require(idleOf(ledger, origin + 90s, 51) == 10s,
        "a release after an allocation took the allocation's activity");
    made(ledger, 105, 0);
    require(attach(ledger, 3, 105), "the attach was refused");
    ledger.contextActive(3, origin + 95s);
    ledger.resourceReleased(105);
    require(idleOf(ledger, origin + 100s, 51) == 5s,
        "a resource without bytes was a release");
}

void theSnapshotKeepsTheLargestHoldersThatAreBigEnough()
{
    ContextMemoryLedger ledger;
    for (std::uint32_t id = 1; id <= 6; ++id) {
        ledger.contextCreated(id, owned(id * 10, "p"), false, origin);
        made(ledger, id, id * mebibyte);
        attach(ledger, id, id);
    }
    ledger.contextCreated(9, owned(90, "empty"), false, origin);

    auto snapshot = ledger.snapshot(origin, 3, 0s, 0);
    require(snapshot.holders.size() == 3 && snapshot.holders[0].pid == 60 &&
            snapshot.holders[1].pid == 50 && snapshot.holders[2].pid == 40,
        "the largest holders were not kept in order");
    snapshot = ledger.snapshot(origin, 10, 0s, 4 * mebibyte);
    require(snapshot.holders.size() == 3 &&
            snapshot.holders.back().bytes == 4 * mebibyte,
        "a holder below the size bound was offered");
    require(snapshot.totalBytes == 21 * mebibyte,
        "the filters changed the total");
    require(ledger.snapshot(origin, 0, 0s, 0).holders.empty(),
        "a request for no holders returned some");
}

void clearForgetsTheGuest()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "a"), true, origin);
    made(ledger, 100, mebibyte);
    attach(ledger, 2, 100);
    ledger.venusMemory(2, 9);
    require(ledger.totalBytes() == mebibyte + 9, "setup");
    ledger.clear();
    require(ledger.totalBytes() == 0 && all(ledger, origin).holders.empty(),
        "a cleared ledger remembered a context");
    attach(ledger, 2, 100);
    ledger.venusMemory(2, 9);
    require(ledger.totalBytes() == 0, "a cleared ledger remembered a resource");
}

/* The renderer's threads report Venus memory while the transport thread
 * works the command stream and a statistics thread reads. */
void threadsMeetOnOneLedger()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, owned(10, "a"), true, origin);
    ledger.contextCreated(3, owned(11, "b"), false, origin);
    std::atomic_bool stop {false};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto snapshot = all(ledger, origin);
            require(snapshot.totalBytes % mebibyte == 0,
                "a reader saw half of an update");
        }
    });
    std::thread renderer([&] {
        for (int index = 0; index < 20000; ++index) {
            ledger.venusMemory(2, mebibyte);
            ledger.venusMemory(2, -static_cast<std::int64_t>(mebibyte));
        }
    });
    for (std::uint32_t id = 100; id < 3100; ++id) {
        made(ledger, id, mebibyte);
        attach(ledger, 3, id);
        ledger.contextActive(3, origin);
        if (id % 2 == 0)
            ledger.resourceReleased(id);
        else
            ledger.resourceDetached(3, id);
    }
    renderer.join();
    stop.store(true);
    reader.join();
    require(ledger.totalBytes() == 0, "the threads left bytes behind");
}

/* The device hears of a change of the total through an observer, for a guest
 * that asked to be told, with no lock held and only for a change. */
void anObserverHearsOfEveryChangeOfTheTotal()
{
    ContextMemoryLedger ledger;
    int calls = 0;
    std::uint64_t seen = 0;
    ledger.contextCreated(1, "t|pid=100|comm=a", true, origin);
    ledger.contextCreated(2, "t|pid=200|comm=b", false, origin);
    ledger.venusMemory(1, 10 * mebibyte);
    require(calls == 0, "an observer that was not set was called");

    ledger.setObserver([&] {
        ++calls;
        /* The ledger is not locked: the observer may read it. */
        seen = ledger.totalBytes();
    });
    ledger.venusMemory(1, 5 * mebibyte);
    require(calls == 1 && seen == 15 * mebibyte, "growth was not reported with its total");
    ledger.venusMemory(1, 0);
    require(calls == 1, "a change of nothing was reported");
    ledger.venusMemory(99, mebibyte);
    require(calls == 1, "a report for an unknown context was reported");
    ledger.contextActive(1, origin);
    ledger.reserveResource(7);
    require(calls == 1, "activity or an empty reservation was reported");

    ledger.setResourceBytes(7, 4 * mebibyte);
    require(calls == 1, "a resource no context holds was reported");
    attach(ledger, 2, 7);
    require(calls == 2 && seen == 19 * mebibyte, "a resource that gained a holder was not reported");
    attach(ledger, 1, 7);
    require(calls == 2, "a second holder of a counted resource was reported");
    ledger.resourceDetached(1, 7);
    require(calls == 2, "a holder that left a held resource was reported");
    ledger.resourceReleased(7);
    require(calls == 3 && seen == 15 * mebibyte, "a released resource was not reported");
    ledger.contextDestroyed(1);
    require(calls == 4 && seen == 0, "a context that took memory with it was not reported");
    ledger.contextDestroyed(1);
    require(calls == 4, "a context that was gone was reported");

    ledger.notify();
    require(calls == 5, "notify did not call the observer");
    ledger.venusMemory(2, 2 * mebibyte);
    require(calls == 6 && seen == 2 * mebibyte, "growth after a destroy was not reported");
    ledger.clear();
    require(calls == 7 && seen == 0, "clear was not reported");
    ledger.clear();
    require(calls == 7, "clearing nothing was reported");

    ledger.setObserver({});
    ledger.contextCreated(3, "t|pid=300|comm=c", true, origin);
    ledger.venusMemory(3, mebibyte);
    ledger.notify();
    require(calls == 7, "a cleared observer was called");
}

/* Clearing the observer returns once no call of it is running, so what the
 * observer touches can go. */
void clearingTheObserverWaitsForARunningCall()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(1, "t|pid=100|comm=a", true, origin);
    std::atomic<bool> inside {false};
    std::atomic<bool> leave {false};
    std::atomic<bool> finished {false};
    ledger.setObserver([&] {
        inside.store(true);
        while (!leave.load())
            std::this_thread::sleep_for(1ms);
        finished.store(true);
    });
    std::thread changing([&] { ledger.venusMemory(1, mebibyte); });
    while (!inside.load())
        std::this_thread::sleep_for(1ms);
    std::thread releasing([&] {
        std::this_thread::sleep_for(50ms);
        leave.store(true);
    });
    ledger.setObserver({});
    require(finished.load(), "the observer was cleared while a call of it ran");
    releasing.join();
    changing.join();
}

} // namespace

int main()
{
    namesAreParsedOnlyWhenTheKernelGaveAProcess();
    aResourceIsCreditedToEveryContextThatHoldsIt();
    anAttachSaysWhetherItMadeTheContextAHolder();
    storageThatArrivesAfterTheAttachIsCredited();
    aContextThatGoesTakesItsCreditWithIt();
    aBlobBelongsToItsCreatorUnlessTheCreatorIsVenus();
    venusMemoryFollowsTheRenderersReports();
    aProcessIsTheSumOfItsContexts();
    aContextNamesItsProcessUntilTheProcessIsGone();
    aHolderIsIdleWhenItsMostRecentContextIs();
    freeingIsNotActivity();
    theSnapshotKeepsTheLargestHoldersThatAreBigEnough();
    clearForgetsTheGuest();
    threadsMeetOnOneLedger();
    anObserverHearsOfEveryChangeOfTheTotal();
    clearingTheObserverWaitsForARunningCall();
    return 0;
}
