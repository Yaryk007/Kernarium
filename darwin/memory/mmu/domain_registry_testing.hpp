// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

#ifndef UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_ALLOCATION_TESTING 0
#endif

#if UML_MEMORY_ALLOCATION_TESTING
namespace uml::memory::testing {

using SoftwareAddressSpaceAcquiredHook = void (*)() noexcept;

[[nodiscard]] std::size_t software_domain_count() noexcept;
[[nodiscard]] std::size_t native_domain_count() noexcept;
void set_software_address_space_acquired_hook(
	SoftwareAddressSpaceAcquiredHook hook) noexcept;

/* Recounts, for every software domain, the executable guest-code words of
 * every live generation per physical-page record and compares each record's
 * count with the recount. Returns the number of records that disagree (a
 * record that can no longer count is not compared). For a test that has
 * stopped changing mappings. */
[[nodiscard]] std::size_t audit_executable_counts() noexcept;

} // namespace uml::memory::testing
#endif
