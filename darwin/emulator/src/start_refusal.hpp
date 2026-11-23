// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

struct uml_emulator;

namespace uml::emulator {

/* Whether the last run of `emulator` was refused for want of memory, before
 * its kernel started: the heap region, the window or the guest's RAM could not
 * be had. `-UML_EMULATOR_OUT_OF_MEMORY` alone does not say it: a failed
 * allocation of the library's own (the argument vector, the handle) returns
 * the same result with no window reserved, and a guest's own exit status can
 * be the same number. The mark belongs to the handle and to its run: the run
 * clears it when it begins, so that a caller who reads it after that run reads
 * what that run did, and a run of another handle cannot change it. Any thread. */
[[nodiscard]] bool start_refused_for_memory(
	const uml_emulator *emulator) noexcept;

} // namespace uml::emulator
