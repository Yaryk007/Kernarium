// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "block/android_boot.hpp"

#include <cstddef>

namespace kernarium::androidroot {

/* Whether an Android launch asks for the root boot (runtime API 46): the
 * release's Magisk-patched ramdisk in place of its own. The ramdisk is the
 * release's file and nothing a request names, so a request carries one word. */

/* What a request states before the word was added to it. */
inline constexpr std::size_t bootSizeBeforeRoot =
    offsetof(kernarium_runtime_boot, android_root);

/* The mode `boot` asks for: root when its struct_size covers the word and the
 * word is not zero. An older caller, whose request ends before the word, and
 * one that states zero get the release's own ramdisk. */
[[nodiscard]] inline uml::android::RootMode rootMode(
    const kernarium_runtime_boot& boot) noexcept
{
    return boot.struct_size >= bootSizeBeforeRoot + sizeof(boot.android_root) &&
                   boot.android_root != 0U
               ? uml::android::RootMode::on
               : uml::android::RootMode::off;
}

} // namespace kernarium::androidroot
