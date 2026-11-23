// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * What the rows about code that one guest process changes for another share:
 * generations (guest address spaces) that map one backing, a guest program
 * that writes and synchronizes code the way a guest does, and a call that runs
 * a function through an engine in a generation.
 */
#include "jit_test_support.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"
#include "scratch_backing.hpp"
#include "static_mmu_test_support.hpp"

#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_static_mmu.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace uml::testing::cross {

namespace arm64 = ::uml::cpu::arm64;

inline constexpr std::uint64_t page = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t shared_base = 0x4f00'0000U;
inline constexpr std::uint64_t alias_base = 0x5300'0000U;
inline constexpr std::uint64_t writer_base = 0x5100'0000U;

[[nodiscard]] constexpr std::uint32_t movz_x0(std::uint32_t value) noexcept
{
	return 0xd280'0000U | value << 5;
}
inline constexpr std::uint32_t svc0 = 0xd400'0001U;

/* x1 = the new word, x2 = where to write it in the writer's view. */
inline const std::vector<std::uint32_t> writer_program{
	0xb900'0041U, // str w1, [x2]
	0xd50b'7b22U, // dc cvau, x2
	0xd503'3b9fU, // dsb ish
	0xd50b'7522U, // ic ivau, x2
	0xd503'3b9fU, // dsb ish
	0xd503'3fdfU, // isb
	svc0,
};

/* The same, but the instruction cache is invalidated at x3, another view of
 * the frame than the one x2 stores through. */
inline const std::vector<std::uint32_t> writer_program_other_view{
	0xb900'0041U, // str w1, [x2]
	0xd50b'7b22U, // dc cvau, x2
	0xd503'3b9fU, // dsb ish
	0xd50b'7523U, // ic ivau, x3
	0xd503'3b9fU, // dsb ish
	0xd503'3fdfU, // isb
	svc0,
};

/* One page of a backing mapped in a generation. Unmapped when it goes. */
struct Map final {
	unsigned long generation;
	std::uint64_t address;
	int fd;
	int protection;
	bool code;
	Map(unsigned long g, std::uint64_t a, int f, int p, bool c)
		: generation{g}, address{a}, fd{f}, protection{p}, code{c}
	{
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(address), fd, 0, page, protection,
			code ? 1 : 0, (protection & PROT_WRITE) != 0 ? 1 : 0) == 0);
	}
	~Map()
	{
		require(darwin_uml_static_vm_unmap_generation(generation,
			reinterpret_cast<void *>(address), page) == 0);
	}
	Map(const Map &) = delete;
	Map &operator=(const Map &) = delete;
};

/* Runs `cpu` in `generation` on `engine`, over `memory`, until the guest
 * leaves for a reason other than its budget, and leaves the thread's active
 * generation as it is. The kernel selects a generation for its own calls, and
 * a vCPU thread that ran another process's task last still holds that one when
 * it enters this guest: a row that must show the engine runs the guest in the
 * generation it was given activates another one first. */
inline ::uml::cpu::ExecutionResult run_with_active_generation_as_is(
	::uml::cpu::CpuEngine &engine, ::uml::memory::StaticGuestMemory &memory,
	unsigned long generation, arm64::CpuState &cpu)
{
	::uml::cpu::ExecutionResult result{};
	for (int attempt = 0; attempt < 64; ++attempt) {
		memory.prepare(generation,
			darwin_uml_static_vm_sequences(generation)
				.mapping_changes);
		::uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = memory,
			.translated_memory = &memory,
			.generation = generation,
			.instruction_budget = std::size_t{1} << 16,
		};
		result = engine.run(request);
		if (result.reason != ::uml::cpu::ExitReason::instruction_budget &&
		    result.reason != ::uml::cpu::ExitReason::interrupt)
			break;
	}
	return result;
}

/* The same, with `generation` the thread's active generation: the kernel's
 * state of a vCPU that last ran this guest. */
inline ::uml::cpu::ExecutionResult run(::uml::cpu::CpuEngine &engine,
	::uml::memory::StaticGuestMemory &memory, unsigned long generation,
	arm64::CpuState &cpu)
{
	require(darwin_uml_static_vm_activate(generation) == 0);
	return run_with_active_generation_as_is(engine, memory, generation, cpu);
}

/* The same over memory of its own for each call. */
inline ::uml::cpu::ExecutionResult run(::uml::cpu::CpuEngine &engine,
	unsigned long generation, arm64::CpuState &cpu)
{
	auto memory = std::make_unique<::uml::memory::StaticGuestMemory>();
	return run(engine, *memory, generation, cpu);
}

[[nodiscard]] inline std::uint64_t result_of(
	const ::uml::cpu::ExecutionResult &result, const arm64::CpuState &cpu)
{
	if (result.reason != ::uml::cpu::ExitReason::system_call)
		std::printf("  exit reason %d at pc %#llx\n",
			static_cast<int>(result.reason),
			static_cast<unsigned long long>(cpu.pc));
	require(result.reason == ::uml::cpu::ExitReason::system_call);
	return cpu.registers[0];
}

/* Runs the function at `pc` in `generation`: what it put in x0. */
[[nodiscard]] inline std::uint64_t call(::uml::cpu::CpuEngine &engine,
	unsigned long generation, std::uint64_t pc)
{
	arm64::CpuState cpu{};
	cpu.pc = pc;
	const auto result = run(engine, generation, cpu);
	return result_of(result, cpu);
}

/* A vCPU: an engine and the memory it runs over, which keeps its TLB from one
 * call to the next, as a carrier thread's does. */
struct Vcpu final {
	explicit Vcpu(::uml::cpu::EngineKind kind)
		: engine{::uml::cpu::make_cpu_engine(kind)},
		  memory{std::make_unique<::uml::memory::StaticGuestMemory>()}
	{
		require(engine != nullptr);
	}

	/* Runs the function at `pc` in `generation` on this vCPU. */
	[[nodiscard]] std::uint64_t call(unsigned long generation,
		std::uint64_t pc)
	{
		arm64::CpuState cpu{};
		cpu.pc = pc;
		const auto result = run(*engine, *memory, generation, cpu);
		return result_of(result, cpu);
	}

	std::unique_ptr<::uml::cpu::CpuEngine> engine;
	std::unique_ptr<::uml::memory::StaticGuestMemory> memory;
};

/* The writer's guest program: store `word` at `target` in its own view and
 * synchronize it. */
inline void write_and_synchronize(::uml::cpu::CpuEngine &engine,
	unsigned long generation, std::uint64_t target, std::uint32_t word)
{
	arm64::CpuState cpu{};
	cpu.pc = writer_base;
	cpu.registers[1] = word;
	cpu.registers[2] = target;
	const auto result = run(engine, generation, cpu);
	require(result.reason == ::uml::cpu::ExitReason::system_call);
}

/* Runs `work` on this vCPU's engine, or on a second vCPU: its own thread and
 * engine. */
template<typename Work>
void on_vcpu(::uml::cpu::EngineKind kind, ::uml::cpu::CpuEngine &own,
	bool second_vcpu, const Work &work)
{
	if (!second_vcpu) {
		work(own);
		return;
	}
	std::thread vcpu{[&] {
		auto engine = ::uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr);
		work(*engine);
	}};
	vcpu.join();
}

[[nodiscard]] inline const char *engine_name(::uml::cpu::EngineKind kind)
{
	return kind == ::uml::cpu::EngineKind::jit ? "jit" : "gadgets";
}

/* Calls `run_case(kind)` for each engine this host has, the JIT inside the
 * pool session the product runs it in (two standalone JIT executors cannot
 * coexist in one process); `only_engine` ("gadgets", "jit" or empty) narrows
 * it. */
template<typename RunKind>
void for_each_engine(std::string_view only_engine, const RunKind &run_kind)
{
	for (const auto kind : {::uml::cpu::EngineKind::gadgets,
		     ::uml::cpu::EngineKind::jit}) {
		const std::string_view engine = engine_name(kind);
		if (!only_engine.empty() && only_engine != engine)
			continue;
		if (!::uml::cpu::cpu_engine_available(kind)) {
			std::printf("%.*s: unavailable on this host; skipped\n",
				static_cast<int>(engine.size()), engine.data());
			continue;
		}
		const bool session = kind == ::uml::cpu::EngineKind::jit;
		if (session)
			require(arm64::acquire_jit_cache_run_session());
		run_kind(kind);
		if (session)
			arm64::release_jit_cache_run_session();
	}
}

} // namespace uml::testing::cross
