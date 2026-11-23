// SPDX-License-Identifier: GPL-2.0-or-later
#include "code_synchronization.hpp"

#include "uml_darwin_static_mmu.h"

#include <atomic>

namespace {

/* One thread runs the helper and the loop that takes the note. */
thread_local bool unmapped_pending;
thread_local std::uint64_t unmapped_address;

} // namespace

extern "C" [[gnu::noinline]] void
uml_arm64_guest_code_synchronize(std::uint64_t address) noexcept
{
	/* Guest data and instruction views share one physical backing, but
	 * translated programs still need an explicit sequence change at the
	 * architectural synchronization point. The fence orders the guest's
	 * stores before the holders of the frame are read. */
	std::atomic_thread_fence(std::memory_order_seq_cst);
	if (darwin_uml_static_vm_synchronize_code(
		    static_cast<unsigned long>(address)) != 0) {
		unmapped_address = address;
		unmapped_pending = true;
	}
}

namespace uml::cpu::arm64 {

bool take_unmapped_code_synchronization(std::uint64_t &address) noexcept
{
	if (!unmapped_pending)
		return false;
	unmapped_pending = false;
	address = unmapped_address;
	return true;
}

bool has_unmapped_code_synchronization() noexcept
{
	return unmapped_pending;
}

void discard_unmapped_code_synchronization() noexcept
{
	unmapped_pending = false;
}

} // namespace uml::cpu::arm64
