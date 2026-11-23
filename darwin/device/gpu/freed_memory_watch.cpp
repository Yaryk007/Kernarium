// SPDX-License-Identifier: GPL-2.0-or-later

#include "freed_memory_watch.hpp"

namespace uml::gpu {

bool FreedMemoryWatch::freedSinceLastCall(
    std::uint64_t cachedFreed, std::uint64_t residentHeapBytes) noexcept
{
    const std::uint64_t freed = freed_.load(std::memory_order_acquire);
    const bool owed = !primed_ || cachedFreed != 0 || freed != seenFreed_ ||
        residentHeapBytes < seenHeapBytes_;
    primed_ = true;
    seenFreed_ = freed;
    seenHeapBytes_ = residentHeapBytes;
    return owed;
}

} // namespace uml::gpu
