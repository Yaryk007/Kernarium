// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Code another process changed and synchronized runs in every process that
 * translated the page. Two guest processes (two generations) share one page of
 * code, a MAP_SHARED file or memfd page. Process B runs a function from the
 * page, so the engine translates it. Process A rewrites the function through
 * its own mapping of the page and synchronizes it with the architectural
 * sequence (STR; DC CVAU; DSB ISH; IC IVAU; DSB ISH; ISB), a guest program run
 * through the engine. B then runs the function again. IC IVAU is broadcast
 * and works on the physical address, so B runs the new instruction. Each case
 * reports what B ran, on both engines and on one vCPU or two.
 *
 * Cases:
 *   cross-rw     A maps the page RW (the writable view of a dual mapping),
 *                B maps it RX.
 *   cross-rwx    A maps the page RWX at the same address, B maps it RX.
 *   ptrace-third B and C map the page RX. The kernel writes it for B (as a
 *                ptrace POKETEXT of a shared mapping does) and calls
 *                darwin_uml_vm_invalidate_code_frame, naming the frame, for
 *                B's generation only; C runs.
 *   kernel-write-lazy-target
 *                B maps the page RX and runs it; the kernel writes the frame
 *                for a process A whose software page table has no entry for it
 *                yet (a table the kernel fills at faults) and calls
 *                darwin_uml_vm_invalidate_code_frame for A's generation.
 *   kernel-write-data-alias
 *                The same, with A holding the frame read-write only: a
 *                process_vm_writev() into a writable view of a dual mapping.
 *   kernel-write-own-alias
 *                B maps the page RX and runs it, and RW at another address;
 *                the kernel writes the frame through the RW view and calls
 *                darwin_uml_vm_invalidate_code_frame for B's generation at
 *                that address: a ptrace or process_vm_writev() into a
 *                dual-view JIT.
 *   cacheflush-own-alias
 *                The same write, told by address only, as an AArch32
 *                cacheflush() of the writable view does
 *                (darwin_uml_vm_invalidate_code).
 *   same-alias   Control: one process, the page RX at one address and RW at
 *                another; the store and the IC IVAU go to the RW alias.
 *   one-vcpu / two-vcpus: whether one engine object (one vCPU) runs both
 *                processes, or each has its own.
 */
#include "arm64_cross_generation_test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

namespace {

using namespace uml::testing::cross;
using uml::testing::require;

enum class Case {
	cross_rw, cross_rwx, ptrace_third, kernel_lazy_target,
	kernel_data_alias, kernel_own_alias, cacheflush_own_alias, same_alias
};

[[nodiscard]] const char *name(Case c) noexcept
{
	switch (c) {
	case Case::cross_rw:
		return "cross-rw";
	case Case::cross_rwx:
		return "cross-rwx";
	case Case::ptrace_third:
		return "ptrace-third";
	case Case::kernel_lazy_target:
		return "kernel-write-lazy-target";
	case Case::kernel_data_alias:
		return "kernel-write-data-alias";
	case Case::kernel_own_alias:
		return "kernel-write-own-alias";
	case Case::cacheflush_own_alias:
		return "cacheflush-own-alias";
	case Case::same_alias:
		return "same-alias";
	}
	return "?";
}

/* Returns true when B ran the new instruction. */
bool run_case(uml::cpu::EngineKind kind, Case c, bool two_engines,
	unsigned long &next_generation)
{
	const auto b = next_generation++;
	const auto a = next_generation++;
	bool fresh = false;
	require(darwin_uml_static_vm_activate(b) == 0);
	require(darwin_uml_static_vm_activate(a) == 0);
	{
		uml::testing::ScratchBacking shared{"uml-gstale-code.XXXXXX", page};
		uml::testing::ScratchBacking writer{"uml-gstale-writer.XXXXXX",
			page};
		const std::uint32_t old_words[] = {movz_x0(1), svc0};
		shared.put(0, std::as_bytes(std::span{old_words}));
		writer.put(0, std::as_bytes(std::span{writer_program}));

		auto engine_b = uml::cpu::make_cpu_engine(kind);
		require(engine_b != nullptr);
		/* The other side's work: on this vCPU, or on a second vCPU (its
		 * own thread and engine). */
		const auto other_side = [&](const auto &work) {
			if (!two_engines) {
				work(*engine_b);
				return;
			}
			std::thread vcpu{[&] {
				auto engine = uml::cpu::make_cpu_engine(kind);
				require(engine != nullptr);
				work(*engine);
			}};
			vcpu.join();
		};

		const Map b_code{b, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		std::unique_ptr<Map> a_view;
		std::unique_ptr<Map> a_writer;
		std::unique_ptr<Map> b_alias;
		std::unique_ptr<Map> b_writer;
		switch (c) {
		case Case::cross_rw:
			a_view = std::make_unique<Map>(a, shared_base,
				shared.descriptor(), PROT_READ | PROT_WRITE, false);
			a_writer = std::make_unique<Map>(a, writer_base,
				writer.descriptor(), PROT_READ | PROT_EXEC, true);
			break;
		case Case::cross_rwx:
			a_view = std::make_unique<Map>(a, shared_base,
				shared.descriptor(),
				PROT_READ | PROT_WRITE | PROT_EXEC, true);
			a_writer = std::make_unique<Map>(a, writer_base,
				writer.descriptor(), PROT_READ | PROT_EXEC, true);
			break;
		case Case::ptrace_third:
			/* Generation a is the third process C, which only runs. */
			a_view = std::make_unique<Map>(a, shared_base,
				shared.descriptor(), PROT_READ | PROT_EXEC, true);
			break;
		case Case::kernel_lazy_target:
			/* Generation a maps nothing at the address. */
			break;
		case Case::kernel_data_alias:
			a_view = std::make_unique<Map>(a, shared_base,
				shared.descriptor(), PROT_READ | PROT_WRITE, false);
			break;
		case Case::kernel_own_alias:
		case Case::cacheflush_own_alias:
		case Case::same_alias:
			b_alias = std::make_unique<Map>(b, alias_base,
				shared.descriptor(), PROT_READ | PROT_WRITE, false);
			if (c == Case::same_alias)
				b_writer = std::make_unique<Map>(b, writer_base,
					writer.descriptor(), PROT_READ | PROT_EXEC, true);
			break;
		}

		/* Warm: B translates the function. */
		require(call(*engine_b, b, shared_base) == 1);
		require(call(*engine_b, b, shared_base) == 1);

		std::uint64_t third = 0;
		switch (c) {
		case Case::cross_rw:
		case Case::cross_rwx:
			other_side([&](uml::cpu::CpuEngine &engine) {
				write_and_synchronize(engine, a, shared_base,
					movz_x0(2));
			});
			break;
		case Case::ptrace_third:
			other_side([&](uml::cpu::CpuEngine &engine) {
				require(call(engine, a, shared_base) == 1);
				require(call(engine, a, shared_base) == 1);
				/* The kernel's view of the frame, then the bridge
				 * call flush_icache_user_page() makes for the
				 * traced mm, B, naming the frame. */
				shared.put32(0, movz_x0(2));
				require(darwin_uml_static_vm_invalidate_code_frame(
					b, shared_base, 4, shared.descriptor(), 0) == 0);
				third = call(engine, a, shared_base);
			});
			break;
		case Case::kernel_lazy_target:
		case Case::kernel_data_alias:
			other_side([&](uml::cpu::CpuEngine &) {
				/* The kernel's view of the frame, then the bridge
				 * call for the mm it wrote through, A, naming the
				 * frame. */
				shared.put32(0, movz_x0(2));
				require(darwin_uml_static_vm_invalidate_code_frame(
					a, shared_base, 4, shared.descriptor(), 0) == 0);
			});
			break;
		case Case::kernel_own_alias:
			other_side([&](uml::cpu::CpuEngine &) {
				shared.put32(0, movz_x0(2));
				require(darwin_uml_static_vm_invalidate_code_frame(
					b, alias_base, 4, shared.descriptor(), 0) == 0);
			});
			break;
		case Case::cacheflush_own_alias:
			other_side([&](uml::cpu::CpuEngine &) {
				shared.put32(0, movz_x0(2));
				require(darwin_uml_static_vm_invalidate_code_generation(
					b, alias_base, 4) == 0);
			});
			break;
		case Case::same_alias:
			other_side([&](uml::cpu::CpuEngine &engine) {
				write_and_synchronize(engine, b, alias_base,
					movz_x0(2));
			});
			break;
		}

		const auto after = call(*engine_b, b, shared_base);
		const char *engine_name =
			kind == uml::cpu::EngineKind::jit ? "jit" : "gadgets";
		const char *mode = two_engines ? "two-vcpus" : "one-vcpu";
		if (c == Case::ptrace_third) {
			fresh = third == 2;
			std::printf("  %s %s %s: traced B x0=%llu, third C x0=%llu (%s)\n",
				engine_name, name(c), mode,
				static_cast<unsigned long long>(after),
				static_cast<unsigned long long>(third),
				fresh ? "new instruction" : "STALE");
		} else {
			fresh = after == 2;
			std::printf("  %s %s %s: B x0=%llu (%s)\n", engine_name,
				name(c), mode, static_cast<unsigned long long>(after),
				fresh ? "new instruction" : "STALE");
		}
	}
	require(darwin_uml_static_vm_destroy(a) == 0);
	require(darwin_uml_static_vm_destroy(b) == 0);
	return fresh;
}

} // namespace

int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	const std::string_view only_engine = argc > 1 ? argv[1] : "";
	unsigned long generation = 0x6a00;
	int stale = 0;
	for (const auto kind : {uml::cpu::EngineKind::gadgets,
		     uml::cpu::EngineKind::jit}) {
		const std::string_view engine =
			kind == uml::cpu::EngineKind::jit ? "jit" : "gadgets";
		if (!only_engine.empty() && only_engine != engine)
			continue;
		if (!uml::cpu::cpu_engine_available(kind)) {
			std::printf("%.*s: unavailable on this host; skipped\n",
				static_cast<int>(engine.size()), engine.data());
			continue;
		}
		/* The product runs the JIT from a pool session; two
		 * standalone JIT executors cannot coexist in one process. */
		const bool session = kind == uml::cpu::EngineKind::jit;
		if (session)
			require(arm64::acquire_jit_cache_run_session());
		for (const auto c : {Case::same_alias, Case::cross_rw,
			     Case::cross_rwx, Case::ptrace_third,
			     Case::kernel_lazy_target, Case::kernel_data_alias,
			     Case::kernel_own_alias, Case::cacheflush_own_alias})
			for (const bool two : {false, true})
				if (!run_case(kind, c, two, generation))
					++stale;
		if (session)
			arm64::release_jit_cache_run_session();
	}
	std::printf("arm64-cross-generation-code-change: %d stale case(s)\n",
		stale);
	return stale == 0 ? 0 : 1;
}
