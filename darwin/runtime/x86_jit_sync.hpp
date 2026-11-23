// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What a vCPU hands the x86-64 JIT before each run. It is a template over
 * the engine and the execution scope so that a unit test can drive the order
 * of its calls with stubs; the runtime instantiates it with FexEngine and
 * X86StaticExecutionScope. The engine names the types it takes (`Changes`
 * and `Range`), so this header includes no engine. */

#include "cpu/x86_64/cpu_state.hpp"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::runtime {

namespace x86_jit_sync_detail {

/* The most changes of either kind the engine is handed as ranges; a reader
 * further behind retires everything instead. A vCPU that was inside the
 * guest kernel while it mapped a large buffer is a few thousand mapping
 * changes behind, none of which touches a translation, so each capacity
 * covers what the address space remembers of its kind. */
constexpr std::size_t mapping_change_capacity = 4096;
constexpr std::size_t code_change_capacity = 1024;

/* The first `count` records of `records` as the engine's ranges, in
 * `ranges`. */
template<typename Range, typename Record, std::size_t Capacity>
[[gnu::always_inline]] std::span<const Range> guest_ranges(
	const std::array<Record, Capacity> &records, std::size_t count,
	std::array<Range, Capacity> &ranges) noexcept
{
	for (std::size_t index = 0; index < count; ++index)
		ranges[index] = {records[index].address, records[index].length};
	return {ranges.data(), count};
}

} // namespace x86_jit_sync_detail

/* Hands the engine what changed in the address space since it last ran in
 * it: the ranges while the address space can still name them, everything
 * once it cannot. */
template<typename Engine, typename Scope>
void sync_x86_jit_engine(Engine &engine, const Scope &execution,
	unsigned long generation, ::uml::cpu::x86_64::ExecutionMode mode,
	const darwin_uml_static_sequences &sequences,
	bool orders_stores) noexcept
{
	/* First, before either early return below: it concerns code that no
	 * address-space change describes. */
	engine.settle_stuck_participants(generation, mode);
	typename Engine::Changes changes{};
	changes.mapping_sequence = sequences.mapping_changes;
	changes.code_sequence = sequences.code_changes;
	std::uint64_t mapping_seen = 0;
	std::uint64_t code_seen = 0;
	if (!engine.consumed_sequences(generation, mode, mapping_seen,
		code_seen)) {
		engine.apply_changes(generation, mode, changes, orders_stores);
		return;
	}
	if (mapping_seen == sequences.mapping_changes &&
		code_seen == sequences.code_changes)
		return;
	thread_local std::array<darwin_uml_static_mapping_change,
		x86_jit_sync_detail::mapping_change_capacity>
		mapped;
	thread_local std::array<typename Engine::Range,
		x86_jit_sync_detail::mapping_change_capacity>
		mapping_ranges;
	if (mapping_seen != sequences.mapping_changes) {
		std::uint64_t current = sequences.mapping_changes;
		const int count = darwin_uml_static_vm_mapping_changes(generation,
			mapping_seen, mapped.data(), mapped.size(), &current);
		if (count < 0) {
			changes.all_mappings = true;
		} else {
			changes.mapping_ranges = x86_jit_sync_detail::guest_ranges(mapped,
				static_cast<std::size_t>(count), mapping_ranges);
			changes.mapping_sequence = current;
		}
	}
	thread_local std::array<darwin_uml_static_code_change,
		x86_jit_sync_detail::code_change_capacity>
		coded;
	thread_local std::array<typename Engine::Range,
		x86_jit_sync_detail::code_change_capacity>
		code_ranges;
	if (code_seen != sequences.code_changes) {
		std::uint64_t current = sequences.code_changes;
		const int count =
			darwin_uml_static_vm_execution_context_code_change_ranges(
				execution.context(), code_seen, coded.data(),
				coded.size(), &current);
		if (count < 0) {
			changes.all_code = true;
		} else {
			changes.code_ranges = x86_jit_sync_detail::guest_ranges(coded,
				static_cast<std::size_t>(count), code_ranges);
			changes.code_sequence = current;
		}
	}
	engine.apply_changes(generation, mode, changes, orders_stores);
}

} // namespace uml::runtime
