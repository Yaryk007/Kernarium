// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_shared_translations.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

using namespace uml::emulator::tests;

/*
 * A budget too small for a shared translation tier leaves none: executors that
 * run the same guest code at the same time each compile it and keep their own
 * validation words, and the tier is never looked in, published to, registered
 * with, locked or swept. Every one of those goes through
 * SharedTranslationCache::instance(), so the structural proof is that the
 * process tier is never constructed, observed without constructing it. The
 * tier's own behaviour is in arm64-jit-cache_validation, whose budget funds
 * one.
 */

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitExecutor;
using uml::memory::StaticGuestMemory;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr unsigned long generation = 9301;
constexpr std::uint64_t base = 0x140'0000;

[[nodiscard]] constexpr std::uint32_t
move_immediate(unsigned destination, unsigned immediate) noexcept
{
	return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target) noexcept
{
	const auto displacement = (static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source)) / 4;
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

void run_to_system_call(JitExecutor &executor, StaticGuestMemory &memory,
	arm64::CpuState &cpu)
{
	cpu.pc = base;
	cpu.registers[8] = 10;
	cpu.registers[9] = 20;
	for (unsigned step = 0; step < 8; ++step) {
		require(executor.begin_execution(generation, cpu.pc));
		std::size_t executed = 0;
		const auto event =
			executor.run_block(cpu, memory, generation, executed);
		executor.end_execution();
		if (event == ::uml::cpu::EngineEvent::system_call)
			return;
		require(event == ::uml::cpu::EngineEvent::continued);
	}
	require(false);
}

void the_budget_funds_no_tier()
{
	require(arm64::derive_shared_translation_cache_budget(
			256U * 1024U * 1024U, 2) == 0 &&
		arm64::jit_shared_translation_cache_budget() == 0);
	arm64::JitCacheExecutorLease lease;
	require(lease.admitted() && !lease.shared_translations());
}

/* Two blocks, the first ending in a direct branch, so the translation has a
 * link pointer a tier would have to relocate.
 */
void executors_alive_together_share_nothing()
{
	constexpr auto target = base + 0x40;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, 0x9100'0508U); // add x8, x8, #1
	backing.put32(8, 0x9100'0929U); // add x9, x9, #2
	backing.put32(12, branch(base + 12, target));
	backing.put32(target - base, move_immediate(1, 9));
	backing.put32(target - base + 4, 0x8b09'0108U); // add x8, x8, x9
	backing.put32(target - base + 8, system_call);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;

	/* Nothing before this point reached the tier: budgets, leases and the
	 * guard test read geometry only. */
	require(!arm64::SharedTranslationCache::instance_exists());
	std::uint64_t installed = 0;
	{
		/* Two live executors are what publication needs when there is a
		 * tier. */
		JitExecutor first;
		JitExecutor second;
		for (auto *executor : {&first, &second}) {
			for (unsigned pass = 0; pass < 2; ++pass) {
				arm64::CpuState cpu{};
				run_to_system_call(*executor, memory, cpu);
				require(cpu.registers[0] == 7 && cpu.registers[1] == 9);
				require(cpu.registers[8] == 33 && cpu.registers[9] == 22);
			}
			const auto diagnostics = executor->cache_validation_diagnostics();
			require(diagnostics.translations > 0);
			require(diagnostics.private_validation_word_installs ==
				diagnostics.translations);
			require(diagnostics.shared_validation_word_installs == 0);
			installed += diagnostics.translations;
		}
		/* Registration, lookups and publications of the compiles above would
		 * each have constructed it. */
		require(!arm64::SharedTranslationCache::instance_exists());
	}
	/* The destructors unregister only from a tier that exists. */
	require(!arm64::SharedTranslationCache::instance_exists());
	std::fprintf(stderr,
		"JIT without a shared tier: %llu blocks installed privately by two "
		"executors, the process tier never constructed\n",
		static_cast<unsigned long long>(installed));
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	constexpr std::size_t mib = std::size_t{1024} * 1024U;
	/* 128 MiB each: all of it theirs, nothing left for a tier. */
	require(arm64::set_jit_cache_budget(256U * mib, 2));
	the_budget_funds_no_tier();
	executors_alive_together_share_nothing();
	return 0;
}
