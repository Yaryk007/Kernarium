// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <algorithm>
#include <cstdint>

namespace uml::gpu {

/* The one detailed timing the virtio-GPU device states in its EDID: its
 * blanking, which grows with the mode's size, and the pixel clock that
 * blanking and a refresh rate make. The clock is two bytes of tens of
 * kilohertz (655.35 MHz at most), so a large mode cannot state a fast rate.
 * The 120 Hz decisions of the display (what rate the mode has, and whether a
 * run is granted the 120 Hz display at all) read it here, so that the
 * advertised mode, the host-feature bit, the display link's pacing and the
 * application's offer all come from the one rule. */
struct EdidBlanking {
    std::uint32_t horizontal {};
    std::uint32_t vertical {};
};

[[nodiscard]] constexpr EdidBlanking edidBlanking(
    std::uint32_t width, std::uint32_t height) noexcept
{
    return {
        .horizontal = std::min<std::uint32_t>(
            protocol::maximumEdidDimension,
            std::max<std::uint32_t>(160U, width / 5U)),
        .vertical = std::min<std::uint32_t>(
            protocol::maximumEdidDimension,
            std::max<std::uint32_t>(30U, height / 20U)),
    };
}

/* The pixel clock of the detailed timing, in tens of kilohertz as its two
 * bytes state it, for a mode of `refreshRate` hertz. */
inline constexpr std::uint64_t maximumDetailedTimingClock = 65535U;

[[nodiscard]] constexpr std::uint64_t detailedTimingClock(
    std::uint32_t width,
    std::uint32_t height,
    EdidBlanking blanking,
    std::uint64_t refreshRate) noexcept
{
    return ((static_cast<std::uint64_t>(width) + blanking.horizontal) *
                (static_cast<std::uint64_t>(height) + blanking.vertical) *
                refreshRate +
            5000U) /
        10000U;
}

/* Whether a mode of this size can state 120 Hz. */
[[nodiscard]] constexpr bool edidCarries120Hz(
    std::uint32_t width, std::uint32_t height) noexcept
{
    return detailedTimingClock(
               width, height, edidBlanking(width, height), 120U) <=
        maximumDetailedTimingClock;
}

/* The rate of the mode of this size for a display that is granted
 * `grantedRefreshRate` (Display::refreshRate): 120 only for a display
 * granted it and a size that can state it, else the 60 Hz mode that a size
 * whose 120 Hz clock overflows has always been. */
[[nodiscard]] constexpr std::uint32_t modeRefreshRate(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t grantedRefreshRate) noexcept
{
    return grantedRefreshRate == 120U && edidCarries120Hz(width, height) ? 120U
                                                                         : 60U;
}

/* The rate the detailed timing of a mode of this size states, for a display
 * granted `grantedRefreshRate`: `modeRefreshRate`, or less for a size so large
 * that even its 60 Hz clock overflows the field (the clock is then the
 * field's largest, which is about 47 Hz at 3840 x 2880, and the EDID has
 * always been that). The display paces by the rate the guest is told. */
[[nodiscard]] constexpr std::uint32_t statedRefreshRate(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t grantedRefreshRate) noexcept
{
    const std::uint64_t rate =
        modeRefreshRate(width, height, grantedRefreshRate);
    const EdidBlanking blanking = edidBlanking(width, height);
    const std::uint64_t pixels =
        (static_cast<std::uint64_t>(width) + blanking.horizontal) *
        (static_cast<std::uint64_t>(height) + blanking.vertical);
    const std::uint64_t clock = std::clamp<std::uint64_t>(
        detailedTimingClock(width, height, blanking, rate),
        1U,
        maximumDetailedTimingClock);
    const std::uint64_t stated = (clock * 10000U + pixels / 2U) / pixels;
    return static_cast<std::uint32_t>(std::min(rate, stated));
}

} // namespace uml::gpu
