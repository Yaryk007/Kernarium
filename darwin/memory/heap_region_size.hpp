// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace uml::memory {

/* The address space the process's heap region (HeapRegion) reserves on an iOS
 * device and in the Simulator, where the address space is small and the guest's
 * native window takes most of it; a macOS process reserves more. Standard
 * library only, so the budget of the guest's memory, which the applications'
 * loader includes, counts it too. */
inline constexpr std::size_t device_heap_region_bytes = std::size_t{1} << 30;

} // namespace uml::memory
