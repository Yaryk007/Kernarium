// SPDX-License-Identifier: GPL-2.0-or-later

/* The pool's reclaim hook waits for the graphics footprint to fall only when
 * memory was freed since it last ran, so a refusal with nothing to give back
 * fails at once. The watch says whether it was: the guest released device
 * memory, the host drivers' caches released an allocation, or the resident
 * heaps fell. A free is not hidden by an allocation made after it, which is
 * what the net readings cannot promise. */

#include "freed_memory_watch.hpp"
#include "require.hpp"

#include <cstdint>
#include <thread>

using uml::gpu::FreedMemoryWatch;
using uml::testing::require;

namespace {

constexpr std::uint64_t mebibyte = 1U << 20;

void firstCallReportsAFree()
{
    FreedMemoryWatch watch;
    require(watch.freedSinceLastCall(0, 0),
        "the first call has nothing to compare with and reports a free");
    require(!watch.freedSinceLastCall(0, 0),
        "a second call with nothing freed reports none");
    watch.reset();
    require(watch.freedSinceLastCall(0, 0), "a reset watch reports a free again");
}

void nothingFreedIsNotAFree()
{
    FreedMemoryWatch watch;
    (void)watch.freedSinceLastCall(0, 300 * mebibyte);
    for (int refusal = 0; refusal < 100; ++refusal) {
        require(!watch.freedSinceLastCall(0, 300 * mebibyte),
            "a refusal with nothing freed since the last one reports none");
    }
    require(!watch.freedSinceLastCall(0, 400 * mebibyte),
        "heaps made since the last call are no free");
}

void aGuestFreeIsReportedOnce()
{
    FreedMemoryWatch watch;
    (void)watch.freedSinceLastCall(0, 300 * mebibyte);
    watch.noteFreed(288 * mebibyte);
    require(watch.freedSinceLastCall(0, 300 * mebibyte),
        "memory the guest freed is reported");
    require(!watch.freedSinceLastCall(0, 300 * mebibyte),
        "the free is reported once");
}

void anAllocationDoesNotHideAFree()
{
    // A guest process ends and another allocates the same amount: the resident
    // heaps are where they were at the last call, and the free is still seen.
    FreedMemoryWatch watch;
    (void)watch.freedSinceLastCall(0, 300 * mebibyte);
    watch.noteFreed(288 * mebibyte);
    require(watch.freedSinceLastCall(0, 300 * mebibyte),
        "a free is reported when the heaps ended where they began");
    watch.noteFreed(288 * mebibyte);
    require(watch.freedSinceLastCall(0, 600 * mebibyte),
        "a free is reported when the heaps grew");
}

void theHostsOwnFreesAreReported()
{
    FreedMemoryWatch watch;
    (void)watch.freedSinceLastCall(0, 300 * mebibyte);
    require(watch.freedSinceLastCall(3, 300 * mebibyte),
        "allocations the drivers' caches released are a free");
    require(!watch.freedSinceLastCall(0, 300 * mebibyte), "and reported once");
    require(watch.freedSinceLastCall(0, 200 * mebibyte),
        "a heap the driver freed for itself is a free");
    require(!watch.freedSinceLastCall(0, 200 * mebibyte), "and reported once");
}

void freesFromAnyThreadAreCounted()
{
    FreedMemoryWatch watch;
    (void)watch.freedSinceLastCall(0, 0);
    std::thread guest([&watch] {
        for (int index = 0; index < 1000; ++index)
            watch.noteFreed(mebibyte);
    });
    guest.join();
    require(watch.freedSinceLastCall(0, 0), "frees from another thread are seen");
    require(!watch.freedSinceLastCall(0, 0), "and once");
}

} // namespace

int main()
{
    firstCallReportsAFree();
    nothingFreedIsNotAFree();
    aGuestFreeIsReportedOnce();
    anAllocationDoesNotHideAFree();
    theHostsOwnFreesAreReported();
    freesFromAnyThreadAreCounted();
    return 0;
}
