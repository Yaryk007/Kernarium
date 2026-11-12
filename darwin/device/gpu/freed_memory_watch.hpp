// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace uml::gpu {

/* Tells the pool's reclaim hook whether memory was freed since it last ran,
 * so that it waits for the graphics footprint to follow a free only when there
 * is one: a refusal with nothing to give back fails at once.
 *
 * The footprint of a resident heap stays until the process next asks Metal to
 * make an allocation resident (`retireFreedGpuMemory`), and the hook makes
 * that request on every refusal. It waits for the footprint to fall only when
 * one of these says a free happened since its previous call:
 *   - the guest released device memory (`noteFreed`: a freed Venus allocation,
 *     a destroyed resource or context). The total only grows, so an
 *     allocation made after a free cannot hide it, which the net readings
 *     below cannot promise: a guest process that ends and another that
 *     allocates leave Metal's counter and the resident-heap tally where they
 *     were;
 *   - the host drivers' caches released an allocation (`cachedFreed`);
 *   - the resident heaps fell (`residentHeapBytes`), which covers a heap the
 *     driver freed for itself and never falls without a free;
 *   - this is the first call, which has nothing to compare with.
 * Any thread may call `noteFreed`; `freedSinceLastCall` is called by one
 * thread at a time, as the pool runs its hook. */
class FreedMemoryWatch {
public:
    void noteFreed(std::uint64_t bytes) noexcept
    {
        freed_.fetch_add(bytes, std::memory_order_release);
    }

    /* Forgets what was seen: the next call reports a free. */
    void reset() noexcept { primed_ = false; }

    [[nodiscard]] bool freedSinceLastCall(
        std::uint64_t cachedFreed, std::uint64_t residentHeapBytes) noexcept;

private:
    std::atomic<std::uint64_t> freed_ {};
    std::uint64_t seenFreed_ {};
    std::uint64_t seenHeapBytes_ {};
    bool primed_ {};
};

} // namespace uml::gpu
