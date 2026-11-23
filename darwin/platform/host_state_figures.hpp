// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "host_thermal.hpp"

#include <atomic>
#include <cstdint>
#include <optional>

namespace uml::platform {

/* What the host's own state was when it was last observed: the thermal state,
 * Low Power Mode and the active core count, and the memory pressure level
 * (0 normal, 1 warning, 2 critical). The stall watch (runtime/stall_watch.hpp)
 * observes them for its record of why the vCPUs stopped, when the system says
 * they changed and every 250 ms while it suspects a stall, and publishes what
 * it saw here, so that the session record's checkpoint
 * (runtime/session_record.hpp) reads the same observation instead of making a
 * second one. Nothing is published before the watch starts or after it ends.
 * One copy per image, and the runtime shares it with the watch. */
struct HostStateFigures final {
    HostThermalState thermal;
    std::uint8_t memoryPressure {};
};

namespace detail {
inline constexpr std::uint32_t hostStateObserved = 0x80000000U;
inline std::atomic<std::uint32_t> hostThermalWord {0};
inline std::atomic<std::uint8_t> hostMemoryPressure {0};
} // namespace detail

/* The thermal word the watch keeps: the level, Low Power Mode and the active
 * core count in one atomic value. */
[[nodiscard]] inline std::uint32_t packHostThermal(
    const HostThermalState& state) noexcept
{
    return static_cast<std::uint32_t>(state.level) |
        (state.lowPowerMode ? 0x100U : 0U) |
        static_cast<std::uint32_t>(state.activeProcessors) << 16;
}

[[nodiscard]] inline HostThermalState unpackHostThermal(
    std::uint32_t word) noexcept
{
    HostThermalState state;
    state.level = static_cast<HostThermalLevel>(word & 0xffU);
    state.lowPowerMode = (word & 0x100U) != 0;
    state.activeProcessors = static_cast<std::uint8_t>(word >> 16 & 0xffU);
    return state;
}

inline void publishHostThermal(const HostThermalState& state) noexcept
{
    detail::hostThermalWord.store(
        packHostThermal(state) | detail::hostStateObserved,
        std::memory_order_relaxed);
}

inline void publishMemoryPressure(std::uint8_t level) noexcept
{
    detail::hostMemoryPressure.store(level, std::memory_order_relaxed);
}

/* The watch ends, or a new one begins: nothing is known until it observes. */
inline void withdrawHostState() noexcept
{
    detail::hostThermalWord.store(0, std::memory_order_relaxed);
    detail::hostMemoryPressure.store(0, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint8_t publishedMemoryPressure() noexcept
{
    return detail::hostMemoryPressure.load(std::memory_order_relaxed);
}

/* The state the watch last published, or nothing while none is observing. */
[[nodiscard]] inline std::optional<HostStateFigures>
publishedHostState() noexcept
{
    const std::uint32_t word =
        detail::hostThermalWord.load(std::memory_order_relaxed);
    if ((word & detail::hostStateObserved) == 0)
        return std::nullopt;
    return HostStateFigures {
        .thermal = unpackHostThermal(word),
        .memoryPressure = publishedMemoryPressure(),
    };
}

} // namespace uml::platform
