// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <mach/mach_time.h>

namespace uml::platform {

/* The scanout flushes the guest makes, for a reader that must not wait on the
 * GPU device: the session record's checkpoint (runtime/session_record.hpp)
 * says how many frames the guest flushed and the longest gap between two of
 * them since the checkpoint before. The device notes each flush of the
 * guest's primary scanout, whichever display shows it, with one counter
 * increment and one reading of the host's clock (mach_absolute_time, about
 * 6 ns): no lock, no thread and no timer. The device has one thread that
 * handles its control queue, which is the only writer of the gap; the reader is
 * any other thread. One copy per image, and the runtime and the GPU device
 * share an image. Times are mach ticks until they are read. */
struct FrameFigures final {
    /* Scanout flushes since the process began. */
    std::uint64_t flushes {};
    /* The longest time in nanoseconds between two flushes since the last
     * take, or from the last flush to the moment of the take when that is
     * longer: a guest that stopped flushing shows the time it has been
     * silent. 0 before the guest's first flush. */
    std::uint64_t longestGapNs {};
};

namespace detail {
inline std::atomic<std::uint64_t> frameFlushes {0};
/* The clock reading of the last flush; 0 for none. */
inline std::atomic<std::uint64_t> frameLastFlushTicks {0};
inline std::atomic<std::uint64_t> frameLongestGapTicks {0};
} // namespace detail

/* The host's clock, in mach ticks: what the flush path reads. */
[[nodiscard]] inline std::uint64_t frameClockTicks() noexcept
{
    return mach_absolute_time();
}

/* Nanoseconds in a mach tick, as a ratio. */
[[nodiscard]] inline mach_timebase_info_data_t frameTimebase() noexcept
{
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t value {};
        (void)mach_timebase_info(&value);
        if (value.numer == 0 || value.denom == 0)
            value = {1, 1};
        return value;
    }();
    return base;
}

[[nodiscard]] inline std::uint64_t frameTicksToNs(
    std::uint64_t ticks) noexcept
{
    const auto base = frameTimebase();
    return ticks * base.numer / base.denom;
}

[[nodiscard]] inline std::uint64_t frameNsToTicks(std::uint64_t ns) noexcept
{
    const auto base = frameTimebase();
    return ns * base.denom / base.numer;
}

/* A flush at `nowTicks` on frameClockTicks()'s clock. */
inline void noteScanoutFlushAt(std::uint64_t nowTicks) noexcept
{
    detail::frameFlushes.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t before =
        detail::frameLastFlushTicks.load(std::memory_order_relaxed);
    detail::frameLastFlushTicks.store(nowTicks, std::memory_order_relaxed);
    if (before == 0 || nowTicks <= before)
        return;
    const std::uint64_t gap = nowTicks - before;
    std::uint64_t longest =
        detail::frameLongestGapTicks.load(std::memory_order_relaxed);
    while (gap > longest &&
        !detail::frameLongestGapTicks.compare_exchange_weak(
            longest, gap, std::memory_order_relaxed)) {
    }
}

inline void noteScanoutFlush() noexcept
{
    noteScanoutFlushAt(frameClockTicks());
}

/* The figures at `nowTicks`; the longest gap starts again from here. The
 * session record's checkpoint is the one reader. */
[[nodiscard]] inline FrameFigures takeFrameFigures(
    std::uint64_t nowTicks) noexcept
{
    FrameFigures figures;
    figures.flushes = detail::frameFlushes.load(std::memory_order_relaxed);
    std::uint64_t longest =
        detail::frameLongestGapTicks.exchange(0, std::memory_order_relaxed);
    const std::uint64_t last =
        detail::frameLastFlushTicks.load(std::memory_order_relaxed);
    if (last != 0 && nowTicks > last && nowTicks - last > longest)
        longest = nowTicks - last;
    figures.longestGapNs = frameTicksToNs(longest);
    return figures;
}

/* A guest begins: its first flush has no gap behind it, and what the gap
 * measured so far is dropped. */
inline void forgetFrameGaps() noexcept
{
    detail::frameLastFlushTicks.store(0, std::memory_order_relaxed);
    detail::frameLongestGapTicks.store(0, std::memory_order_relaxed);
}

} // namespace uml::platform
