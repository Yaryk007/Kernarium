// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/* A guest's instruction-cache invalidation by address (IC IVAU), called from
 * both engines at the instruction, which then returns to the entry loop at
 * the next one. It publishes the code change to every address space that
 * translated the frame the address names. */
extern "C" void
uml_arm64_guest_code_synchronize(std::uint64_t address) noexcept;

namespace uml::cpu::arm64 {

/* IC IVAU is handled as a load is for translation: an address the address
 * space maps nothing at faults, and the kernel maps the page. The helper
 * cannot report a fault from inside translated code, so it notes the address
 * and the entry loop that regains control takes the note. */
[[nodiscard]] bool take_unmapped_code_synchronization(
	std::uint64_t &address) noexcept;
/* Whether a note is waiting. An executor that runs more than one instruction
 * between two returns to the entry loop tests it after each instruction it
 * takes the engine's way, as the entry loop would at the end of a block: the
 * instruction that made the note is the last that may run before the loop
 * takes it, because the loop reports the fault at the instruction before the
 * PC it finds. */
[[nodiscard]] bool has_unmapped_code_synchronization() noexcept;
/* Forgets a note nothing took (a caller that runs an executor outside the
 * entry loop), so a later run does not report an old instruction's fault. */
void discard_unmapped_code_synchronization() noexcept;

} // namespace uml::cpu::arm64
