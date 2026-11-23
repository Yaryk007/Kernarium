// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#ifndef UML_STATIC_GUEST_MEMORY_TESTING
#define UML_STATIC_GUEST_MEMORY_TESTING 0
#endif

#if UML_STATIC_GUEST_MEMORY_TESTING
namespace uml::memory::testing {

/* Runs on the fetching thread, in StaticGuestMemory::fetch(), right after the
 * bytes of one page of an instruction fetch were copied for a memory that
 * registers its generation as a holder, and before the next page or the
 * return. A test holds the reader there while another process writes and
 * synchronizes the frame. It must not call back into the memory under test.
 * Only the test variant of the object (uml_static_guest_memory_test_variant)
 * has it; the product has neither the pointer nor the call. */
using FetchObserver = void (*)(std::uint64_t address) noexcept;
void set_fetch_observer(FetchObserver observer) noexcept;

} // namespace uml::memory::testing
#endif
