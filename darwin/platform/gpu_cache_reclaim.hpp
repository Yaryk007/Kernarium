// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

namespace uml::platform {

/* The one step of the host GPU's reclaim the runtime may take on its own: the
 * function that frees what the host's drivers cache for reuse and returns how
 * many cached allocations it freed (the Virgl renderer's
 * reclaimCachedMemory). The GPU device publishes it while a renderer with a
 * memory budget exists and withdraws it (null) when the renderer goes, so the
 * runtime's response to the system's memory pressure
 * (runtime/memory_pressure_response.hpp) reaches the device without a
 * dependency on it. The function may be called from any thread and does
 * nothing once the renderer is gone. One copy per image, as the other
 * published figures. */
using GpuCacheReclaimer = std::uint64_t (*)() noexcept;

namespace detail {
inline std::atomic<GpuCacheReclaimer> gpuCacheReclaimer {nullptr};
} // namespace detail

inline void publishGpuCacheReclaimer(GpuCacheReclaimer reclaimer) noexcept
{
    detail::gpuCacheReclaimer.store(reclaimer, std::memory_order_release);
}

/* Null while no renderer with a budget runs. */
[[nodiscard]] inline GpuCacheReclaimer publishedGpuCacheReclaimer() noexcept
{
    return detail::gpuCacheReclaimer.load(std::memory_order_acquire);
}

} // namespace uml::platform
