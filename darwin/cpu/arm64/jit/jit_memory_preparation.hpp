// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/jit_memory_status.hpp"
#include "jit_cache_geometry.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

using ::uml::cpu::JitMemoryStatus;

/* Prepares the region the whole-process budget `total_bytes` over `executors`
 * needs (the arenas of every executor derive_jit_cache_geometry gives it) and
 * publishes the budget as the process's, so a run that sets the same pair
 * finds both ready. Blocks for the seconds the debugger takes: call it from a
 * worker thread, before any executor exists.
 */
[[nodiscard]] JitMemoryStatus prepare_jit_memory(std::size_t total_bytes,
	unsigned executors,
	std::size_t per_executor_bytes =
		default_jit_cache_budget_per_executor) noexcept;

/* The region's bytes a budget needs, zero for an invalid pair. */
[[nodiscard]] std::size_t jit_memory_region_bytes(std::size_t total_bytes,
	unsigned executors,
	std::size_t per_executor_bytes =
		default_jit_cache_budget_per_executor) noexcept;

[[nodiscard]] bool jit_debugger_attached() noexcept;

/* Tells an attached debugger to detach, for a process that will not use JIT. */
void release_jit_debugger() noexcept;

/* The bytes of executable memory a debugger prepared for this process, zero
 * before one did. They stay wired for the life of the process, used by a JIT
 * run or not. */
[[nodiscard]] std::size_t prepared_jit_region_bytes() noexcept;

} // namespace uml::cpu::arm64
