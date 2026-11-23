// SPDX-License-Identifier: GPL-2.0-or-later

/* The sizes whose 120 Hz mode the EDID can state, against the table that the
 * application's offer (Andarium/AndariumCore HighRefresh.swift) reads too, and
 * the rate of the mode of a size for each rate a display can be granted. */

#include "edid_timing.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace {

using uml::testing::require;

void everySizeOfTheTableCarriesOrDoesNot(const char* path)
{
    std::ifstream file(path);
    require(file.is_open(), "the size table opens");
    std::string line;
    unsigned rows = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        unsigned carries = 2;
        require(static_cast<bool>(row >> width >> height >> carries) &&
                    carries <= 1,
            "a row is width, height and 0 or 1");
        if (uml::gpu::edidCarries120Hz(width, height) != (carries == 1))
            std::fprintf(stderr, "%ux%u\n", width, height);
        require(uml::gpu::edidCarries120Hz(width, height) == (carries == 1),
            "a size's 120 Hz mode differs from the table");
        ++rows;
    }
    require(rows >= 10, "the table has its rows");
}

void theModeRateFollowsTheGrantAndTheSize()
{
    using uml::gpu::modeRefreshRate;
    require(modeRefreshRate(860, 1678, 120) == 120, "granted and it fits");
    require(modeRefreshRate(860, 1678, 60) == 60, "not granted");
    require(modeRefreshRate(860, 1678, 0) == 60, "no grant");
    require(modeRefreshRate(860, 1678, 90) == 60, "only 120 is a grant");
    require(modeRefreshRate(2752, 2064, 120) == 60,
        "granted but the size cannot state it");
    require(modeRefreshRate(2752, 2064, 60) == 60);
}

void theStatedRateIsWhatTheClockCarries()
{
    using uml::gpu::statedRefreshRate;
    require(statedRefreshRate(860, 1678, 120) == 120);
    require(statedRefreshRate(860, 1678, 60) == 60);
    require(statedRefreshRate(2752, 2064, 120) == 60,
        "a size that cannot state 120 states 60");
    /* A size whose 60 Hz clock overflows the field too (13.9 million pixels
     * with their blanking) states what the largest clock makes of it: the
     * EDID is main's, and the display paces by that. */
    require(statedRefreshRate(3840, 2880, 120) == 47);
    require(statedRefreshRate(3840, 2880, 60) == 47);
    /* The clock's rounding never takes a size that fits under its rate. */
    for (const auto [width, height] :
        {std::pair {640U, 480U}, std::pair {1280U, 720U}, std::pair {860U, 1678U},
            std::pair {1668U, 2388U}, std::pair {2732U, 2048U},
            std::pair {2752U, 2064U}, std::pair {3000U, 2000U}}) {
        require(statedRefreshRate(width, height, 60) == 60,
            "a size that fits states its rate");
    }
}

void theClockOfTheSixtyHertzModeIsWhatTheDeviceAlwaysStated()
{
    /* A phone's guest display, 860 x 1678: a 1032 x 1761 total at 60 Hz is a
     * clock of 10904 tens of kilohertz. */
    const auto blanking = uml::gpu::edidBlanking(860, 1678);
    require(blanking.horizontal == 172 && blanking.vertical == 83);
    require(uml::gpu::detailedTimingClock(860, 1678, blanking, 60) == 10904);
    require(uml::gpu::detailedTimingClock(860, 1678, blanking, 120) == 21808);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "usage: edid_timing_tests <edid-120hz-sizes.txt>");
        everySizeOfTheTableCarriesOrDoesNot(argv[1]);
        theModeRateFollowsTheGrantAndTheSize();
        theStatedRateIsWhatTheClockCarries();
        theClockOfTheSixtyHertzModeIsWhatTheDeviceAlwaysStated();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "edid timing test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
