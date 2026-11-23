// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kernarium::hostfeatures {

/* What an Android guest is told about the host at boot: the bits of the boot
 * request's android_host_features (the KERNARIUM_RUNTIME_ANDROID_HOST_ enum of
 * KernariumRuntime.h) and the kernel argument each becomes. Android's init
 * imports an androidboot. argument as a ro.boot property. The launcher owns
 * the arguments: a release's command-line file cannot set one (each prefix is a
 * launcher prefix). android/lineage-24/README.md owns what the guest does with
 * them.
 *
 * This table is the one place a feature's argument is composed, and a feature
 * is added by adding a row here and a bit to the enum. */
struct Feature {
    std::string_view name;
    std::uint32_t bit;
    /* Through the '='. */
    std::string_view argumentPrefix;
    std::string_view argumentValue;
};

inline constexpr Feature features[] = {
    /* Android's per-SKU manifest of the ODM axis declares the vibrator HAL
     * only for this SKU, so a device without haptics runs no HAL and finds no
     * vibrator. */
    {"haptics", KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS,
     "androidboot.product.hardware.sku=", "haptics"},
    /* The guest's motion sub-HAL lists its device-orientation sensor only when
     * ro.boot.kernarium.orientation is true, because Android's window manager
     * follows such a sensor instead of judging the accelerometer and so must
     * not find one that nothing feeds. */
    {"orientation", KERNARIUM_RUNTIME_ANDROID_HOST_ORIENTATION,
     "androidboot.kernarium.orientation=", "1"},
    /* The guest's motion sub-HAL lists a magnetometer, and the compass feature
     * is declared, only when ro.boot.kernarium.compass is true: Android's
     * features must match its sensors, and a host that has no magnetometer
     * (a Mac, the Simulator) must not offer one that nothing feeds. */
    {"compass", KERNARIUM_RUNTIME_ANDROID_HOST_COMPASS,
     "androidboot.kernarium.compass=", "1"},
    /* The display is a 120 Hz one (the EDID's one mode), so the guest's frame
     * scheduler should treat it as such: ro.boot.kernarium.high_refresh is
     * true only on a host that advertises it, and a 60 Hz host has no such
     * property and runs as it always has. */
    {"high-refresh", KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH,
     "androidboot.kernarium.high_refresh=", "1"},
};

[[nodiscard]] constexpr bool singleBit(std::uint32_t value) noexcept
{
    return value != 0 && (value & (value - 1U)) == 0;
}

/* Whether every row of `table` has a bit of its own, exactly one bit wide, and
 * a name and a kernel argument prefix no other row has: a bit shared by two
 * features would turn one into the other, and one argument written twice would
 * leave the guest the last. */
[[nodiscard]] constexpr bool distinctRows(std::span<const Feature> table) noexcept
{
    for (std::size_t row = 0; row < table.size(); ++row) {
        const Feature& feature = table[row];
        if (!singleBit(feature.bit) || feature.name.empty() ||
            !feature.argumentPrefix.ends_with('=') ||
            feature.argumentValue.empty())
            return false;
        for (std::size_t earlier = 0; earlier < row; ++earlier) {
            if (table[earlier].bit == feature.bit ||
                table[earlier].name == feature.name ||
                table[earlier].argumentPrefix == feature.argumentPrefix)
                return false;
        }
    }
    return true;
}

static_assert(distinctRows(features),
    "two host features share a bit, a name or a kernel argument");

/* The bits `table` names. */
[[nodiscard]] constexpr std::uint32_t knownBits(
    std::span<const Feature> table = features) noexcept
{
    std::uint32_t known = 0;
    for (const Feature& feature : table)
        known |= feature.bit;
    return known;
}

/* The bits of `requested` the runtime has no row for. */
[[nodiscard]] constexpr std::uint32_t unknownBits(
    std::uint32_t requested, std::span<const Feature> table = features) noexcept
{
    return requested & ~knownBits(table);
}

/* The kernel arguments for the bits of `requested` that `table` names, in
 * table order. */
[[nodiscard]] inline std::vector<std::string> kernelArguments(
    std::uint32_t requested, std::span<const Feature> table = features)
{
    std::vector<std::string> arguments;
    for (const Feature& feature : table) {
        if ((requested & feature.bit) != 0)
            arguments.push_back(
                std::string {feature.argumentPrefix} +
                std::string {feature.argumentValue});
    }
    return arguments;
}

/* Every prefix the table's arguments start with: the launcher prefixes a
 * release's command-line file may not carry. */
[[nodiscard]] inline std::vector<std::string_view> launcherPrefixes(
    std::span<const Feature> table = features)
{
    std::vector<std::string_view> prefixes;
    for (const Feature& feature : table)
        prefixes.push_back(feature.argumentPrefix);
    return prefixes;
}

} // namespace kernarium::hostfeatures
