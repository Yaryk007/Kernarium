// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "block/android_release_identity.hpp"
#include "options.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace kernarium::aot {

/* Whether an Android launch states the release it boots to the emulator
 * (runtime API 45). The ahead-of-time component runs only with the release it
 * was made from, and a launch that states none runs without it
 * (docs/architecture.md, "Ahead-of-time code"), so the boot request's
 * android_aot_disabled is how an application keeps the component out of one
 * start. */

/* What a request states before the word was added to it. */
inline constexpr std::size_t bootSizeBeforeAotDisabled =
    offsetof(kernarium_runtime_boot, android_aot_disabled);

/* Whether `boot` leaves the release out: its struct_size covers the word and
 * the word is not zero. An older caller, whose request ends before the word,
 * and one that states zero get today's launch. */
[[nodiscard]] inline bool aotDisabled(
    const kernarium_runtime_boot& boot) noexcept
{
    return boot.struct_size >=
               bootSizeBeforeAotDisabled + sizeof(boot.android_aot_disabled) &&
           boot.android_aot_disabled != 0U;
}

/* The emulator argument that states the release a start boots, from the
 * source of its identity (Storage::releaseIdentity): none when the launch
 * keeps the component out, which does not read the release at all, and none
 * for a release that states no identity the emulator accepts. This is the
 * whole of what the request's android_aot_disabled changes about a start. */
template <typename Release>
[[nodiscard]] std::string releaseArgument(
    bool disabled, const Release& release)
{
    if (disabled)
        return {};
    const std::optional<uml::android::ReleaseIdentity> identity =
        release.releaseIdentity();
    if (!identity)
        return {};
    return uml::emulator::aot_release_argument(
        identity->id, identity->manifestSha256, identity->systemSha256);
}

} // namespace kernarium::aot
