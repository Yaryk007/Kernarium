// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IC IVAU names an address, and hardware faults it as it would a load: an
 * address the process has no translation for takes a translation fault, the
 * kernel maps the page, and the instruction runs again, so it always reaches
 * the frame it means. A software page table the kernel fills at faults has no
 * entry for a view the process never touched, such as the executable view of a
 * page another process also runs. Both engines report that fault, at the
 * instruction and not after it, and publish the code change once the page is
 * mapped.
 *
 * Cases:
 *   unmapped    IC IVAU at an address nothing maps faults for a read at that
 *               address, at the instruction, which has not retired; once the
 *               page is mapped, the instruction runs and the guest goes on.
 *   mapped      A page mapped without execute, as a writable view is, does not
 *               fault.
 *   null page   IC IVAU at the null page faults like any other unmapped page:
 *               a privileged guest can map code there, as a lazy view, and the
 *               kernel's answer to the fault (a mapping or a signal) is the
 *               architecture's.
 *   lazy-view   A writes the code through its writable view and invalidates the
 *               instruction cache at the executable view, which A has not
 *               mapped yet. The fault maps the view; B, which translated the
 *               page, then runs the new instruction.
 *   note        The helper both engines call leaves a note for the entry loop
 *               when the address is unmapped, which an executor that runs on
 *               after an instruction can see without taking it.
 *   entered     The generation an entered execution context holds is the one
 *               IC IVAU is resolved in, through nested contexts and back out
 *               of them, and a context destroyed while entered leaves nothing
 *               of it behind.
 *   other-active
 *               Each case above again on a thread whose active generation is
 *               another process's, as a vCPU's is after it ran that process's
 *               task: the instruction belongs to the generation the engine
 *               runs, so a page that generation maps does not fault and one it
 *               does not map does, whatever the other generation maps, and the
 *               change is published to the running generation.
 */
#include "arm64_cross_generation_test_support.hpp"

#include "cpu/arm64/code_synchronization.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

namespace {

using namespace uml::testing::cross;
using uml::testing::require;

constexpr std::uint32_t ic_ivau_x1 = 0xd50b'7521U;

int failures;

/* Runs `cpu` in `generation` with `active` the thread's active generation,
 * which `generation` is not in the cases that matter: the engine takes the
 * guest's generation from the request. */
uml::cpu::ExecutionResult run_with_active(uml::cpu::CpuEngine &engine,
	unsigned long generation, unsigned long active, arm64::CpuState &cpu)
{
	require(darwin_uml_static_vm_activate(active) == 0);
	auto memory = std::make_unique<uml::memory::StaticGuestMemory>();
	return run_with_active_generation_as_is(
		engine, *memory, generation, cpu);
}

void expect(bool condition, const char *engine, const char *what)
{
	if (condition)
		return;
	++failures;
	std::printf("  %s: FAIL %s\n", engine, what);
}

void unmapped_and_mapped(uml::cpu::EngineKind kind, unsigned long &next)
{
	const char *engine_label = engine_name(kind);
	const auto generation = next++;
	require(darwin_uml_static_vm_activate(generation) == 0);
	{
		uml::testing::ScratchBacking code{"uml-icfault-code.XXXXXX", page};
		uml::testing::ScratchBacking data{"uml-icfault-data.XXXXXX", page};
		const std::uint32_t words[] = {ic_ivau_x1, movz_x0(7), svc0};
		code.put(0, std::as_bytes(std::span{words}));
		const Map code_map{generation, shared_base, code.descriptor(),
			PROT_READ | PROT_EXEC, true};
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr);

		arm64::CpuState cpu{};
		cpu.pc = shared_base;
		cpu.registers[1] = alias_base + 0x40;
		const auto fault = run(*engine, generation, cpu);
		expect(fault.reason == uml::cpu::ExitReason::memory_fault, engine_label,
			"an unmapped IC IVAU did not fault");
		expect(fault.fault.address == alias_base + 0x40 &&
			fault.fault.access == uml::memory::MemoryAccess::read,
			engine_label, "the fault is not a read of the address");
		expect(cpu.pc == shared_base, engine_label,
			"the fault is not at the instruction");
		expect(fault.retired_instructions == 0, engine_label,
			"the instruction that faulted retired");

		/* The kernel maps the page, and the instruction runs again. */
		const Map data_map{generation, alias_base, data.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const auto again = run(*engine, generation, cpu);
		expect(again.reason == uml::cpu::ExitReason::system_call &&
			cpu.registers[0] == 7, engine_label,
			"the instruction did not run once its page was mapped");

		/* A page mapped without execute, as a writable view is, is a page. */
		arm64::CpuState writable{};
		writable.pc = shared_base;
		writable.registers[1] = alias_base;
		const auto quiet = run(*engine, generation, writable);
		expect(quiet.reason == uml::cpu::ExitReason::system_call &&
			writable.registers[0] == 7, engine_label,
			"an IC IVAU at a writable view faulted");

		/* The null page is a page like another: the kernel decides whether
		 * something maps it. */
		arm64::CpuState null_page{};
		null_page.pc = shared_base;
		null_page.registers[1] = 0;
		const auto null_run = run(*engine, generation, null_page);
		expect(null_run.reason == uml::cpu::ExitReason::memory_fault &&
			null_run.fault.address == 0 &&
			null_run.fault.access == uml::memory::MemoryAccess::read,
			engine_label, "an IC IVAU at the null page did not fault for a read");
		expect(null_page.pc == shared_base &&
			null_run.retired_instructions == 0, engine_label,
			"the null-page fault is not at the instruction, unretired");
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void another_generation_active(uml::cpu::EngineKind kind, unsigned long &next)
{
	const char *engine_label = engine_name(kind);
	const auto app = next++;
	const auto other = next++;
	require(darwin_uml_static_vm_activate(app) == 0);
	require(darwin_uml_static_vm_activate(other) == 0);
	{
		uml::testing::ScratchBacking code{"uml-icfault-code.XXXXXX", page};
		uml::testing::ScratchBacking data{"uml-icfault-data.XXXXXX", page};
		const std::uint32_t words[] = {ic_ivau_x1, movz_x0(7), svc0};
		code.put(0, std::as_bytes(std::span{words}));
		const Map code_map{app, shared_base, code.descriptor(),
			PROT_READ | PROT_EXEC, true};
		auto engine = uml::cpu::make_cpu_engine(kind);
		require(engine != nullptr);

		/* The page is the running guest's and not the other's. */
		{
			const Map app_view{app, alias_base, data.descriptor(),
				PROT_READ | PROT_WRITE, false};
			const auto app_before = darwin_uml_static_vm_code_changes(app);
			const auto other_before =
				darwin_uml_static_vm_code_changes(other);
			arm64::CpuState cpu{};
			cpu.pc = shared_base;
			cpu.registers[1] = alias_base + 0x40;
			const auto result = run_with_active(*engine, app, other, cpu);
			expect(result.reason == uml::cpu::ExitReason::system_call &&
				cpu.registers[0] == 7, engine_label,
				"an IC IVAU at a page the guest maps faulted, "
				"another generation being active");
			expect(darwin_uml_static_vm_code_changes(app) > app_before,
				engine_label,
				"the guest's generation did not hear of its IC IVAU");
			expect(darwin_uml_static_vm_code_changes(other) == other_before,
				engine_label,
				"the active generation heard of another guest's IC IVAU");
		}

		/* The page is the other's and not the running guest's: the guest
		 * faults, and the kernel maps it for the guest. */
		{
			const Map other_view{other, alias_base, data.descriptor(),
				PROT_READ | PROT_WRITE, false};
			const auto other_before =
				darwin_uml_static_vm_code_changes(other);
			arm64::CpuState cpu{};
			cpu.pc = shared_base;
			cpu.registers[1] = alias_base + 0x40;
			const auto fault = run_with_active(*engine, app, other, cpu);
			expect(fault.reason == uml::cpu::ExitReason::memory_fault,
				engine_label,
				"an IC IVAU at a page only the active generation maps "
				"did not fault");
			expect(fault.fault.address == alias_base + 0x40 &&
				fault.fault.access == uml::memory::MemoryAccess::read,
				engine_label, "the fault is not a read of the address");
			expect(cpu.pc == shared_base && fault.retired_instructions == 0,
				engine_label,
				"the fault is not at the instruction, unretired");
			expect(darwin_uml_static_vm_code_changes(other) == other_before,
				engine_label,
				"the active generation heard of another guest's IC IVAU");
			const Map app_view{app, alias_base, data.descriptor(),
				PROT_READ | PROT_WRITE, false};
			/* The faulting run already published to the guest's generation
			 * (an IC IVAU at an unmapped page still is a code change), so
			 * the retry is measured from here. */
			const auto retry_before = darwin_uml_static_vm_code_changes(app);
			const auto again = run_with_active(*engine, app, other, cpu);
			expect(again.reason == uml::cpu::ExitReason::system_call &&
				cpu.registers[0] == 7, engine_label,
				"the instruction did not run once the guest mapped its "
				"page");
			expect(darwin_uml_static_vm_code_changes(app) > retry_before,
				engine_label,
				"the guest's generation did not hear of its IC IVAU");
		}
	}
	require(darwin_uml_static_vm_destroy(other) == 0);
	require(darwin_uml_static_vm_destroy(app) == 0);
}

/* What the bridge answers for the page `alias_base`, which only `app` maps,
 * with `other` the thread's active generation. */
int synchronize_alias()
{
	return darwin_uml_static_vm_synchronize_code(
		static_cast<unsigned long>(alias_base + 0x40));
}

void note_for_the_entry_loop(unsigned long &next)
{
	const auto app = next++;
	require(darwin_uml_static_vm_activate(app) == 0);
	{
		uml::testing::ScratchBacking data{"uml-icfault-data.XXXXXX", page};
		void *context = darwin_uml_static_vm_execution_context_create(app);
		require(context != nullptr);
		require(darwin_uml_static_vm_execution_context_begin(context, 0) == 0);
		namespace cpu = uml::cpu::arm64;
		std::uint64_t address = 0;
		uml::cpu::arm64::discard_unmapped_code_synchronization();
		expect(!cpu::has_unmapped_code_synchronization(), "helper",
			"a note was waiting before any instruction");
		/* An address nothing maps: the note is there to see, as often as
		 * asked, until the loop takes it. */
		uml_arm64_guest_code_synchronize(alias_base + 0x40);
		expect(cpu::has_unmapped_code_synchronization() &&
			cpu::has_unmapped_code_synchronization(), "helper",
			"an IC IVAU at an unmapped address left no note");
		expect(cpu::take_unmapped_code_synchronization(address) &&
			address == alias_base + 0x40, "helper",
			"the note does not name the address");
		expect(!cpu::has_unmapped_code_synchronization(), "helper",
			"a taken note is still waiting");
		require(darwin_uml_static_vm_execution_context_end(context) == 0);
		/* A mapped one leaves none. A mapping waits for the runs that are
		 * entered, so the kernel's map call is made between them. */
		const Map view{app, alias_base, data.descriptor(),
			PROT_READ | PROT_WRITE, false};
		require(darwin_uml_static_vm_execution_context_begin(context, 0) == 0);
		uml_arm64_guest_code_synchronize(alias_base + 0x40);
		expect(!cpu::has_unmapped_code_synchronization(), "helper",
			"an IC IVAU at a mapped address left a note");
		require(darwin_uml_static_vm_execution_context_end(context) == 0);
		darwin_uml_static_vm_execution_context_destroy(context);
	}
	require(darwin_uml_static_vm_destroy(app) == 0);
}

void entered_context_binding(unsigned long &next)
{
	const auto app = next++;
	const auto other = next++;
	require(darwin_uml_static_vm_activate(app) == 0);
	require(darwin_uml_static_vm_activate(other) == 0);
	{
		uml::testing::ScratchBacking data{"uml-icfault-data.XXXXXX", page};
		const Map app_view{app, alias_base, data.descriptor(),
			PROT_READ | PROT_WRITE, false};
		void *in_app = darwin_uml_static_vm_execution_context_create(app);
		void *in_other = darwin_uml_static_vm_execution_context_create(other);
		require(in_app != nullptr && in_other != nullptr);

		expect(synchronize_alias() == 1, "bridge",
			"outside a context the active generation did not decide");
		require(darwin_uml_static_vm_execution_context_begin(in_app, 0) == 0);
		expect(synchronize_alias() == 0, "bridge",
			"an entered context did not decide");
		/* A run inside the run: the innermost is the guest running. */
		require(darwin_uml_static_vm_execution_context_begin(
			in_other, 0) == 0);
		expect(synchronize_alias() == 1, "bridge",
			"the innermost entered context did not decide");
		require(darwin_uml_static_vm_execution_context_end(in_other) == 0);
		expect(synchronize_alias() == 0, "bridge",
			"leaving the inner context did not give the outer back");
		require(darwin_uml_static_vm_execution_context_end(in_app) == 0);
		expect(synchronize_alias() == 1, "bridge",
			"a left context still decided");

		/* A context destroyed while entered is gone from the thread. */
		require(darwin_uml_static_vm_execution_context_begin(in_app, 0) == 0);
		darwin_uml_static_vm_execution_context_destroy(in_app);
		expect(synchronize_alias() == 1, "bridge",
			"a destroyed context still decided");
		darwin_uml_static_vm_execution_context_destroy(in_other);
	}
	require(darwin_uml_static_vm_destroy(other) == 0);
	require(darwin_uml_static_vm_destroy(app) == 0);
}

void lazy_view(uml::cpu::EngineKind kind, bool second_vcpu,
	bool other_active, unsigned long &next)
{
	const char *engine_label = engine_name(kind);
	const auto b = next++;
	const auto a = next++;
	const auto other = next++;
	require(darwin_uml_static_vm_activate(b) == 0);
	require(darwin_uml_static_vm_activate(a) == 0);
	require(darwin_uml_static_vm_activate(other) == 0);
	/* The writer's runs on the thread of the vCPU: with the other generation
	 * active there, or its own as a kernel that just switched to it leaves
	 * it. */
	const auto writer_run = [&](uml::cpu::CpuEngine &engine,
	                            arm64::CpuState &cpu) {
		return run_with_active(engine, a, other_active ? other : a, cpu);
	};
	bool fresh = false;
	{
		uml::testing::ScratchBacking shared{"uml-icfault-shared.XXXXXX", page};
		uml::testing::ScratchBacking writer{"uml-icfault-writer.XXXXXX", page};
		const std::uint32_t old_words[] = {movz_x0(1), svc0};
		shared.put(0, std::as_bytes(std::span{old_words}));
		writer.put(0, std::as_bytes(std::span{writer_program_other_view}));
		auto engine_b = uml::cpu::make_cpu_engine(kind);
		require(engine_b != nullptr);

		const Map b_code{b, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		const Map a_view{a, shared_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map a_writer{a, writer_base, writer.descriptor(),
			PROT_READ | PROT_EXEC, true};
		require(call(*engine_b, b, shared_base) == 1);
		require(call(*engine_b, b, shared_base) == 1);

		on_vcpu(kind, *engine_b, second_vcpu, [&](uml::cpu::CpuEngine &engine) {
			arm64::CpuState cpu{};
			cpu.pc = writer_base;
			cpu.registers[1] = movz_x0(2);
			cpu.registers[2] = shared_base;
			cpu.registers[3] = alias_base;
			const auto first = writer_run(engine, cpu);
			expect(first.reason == uml::cpu::ExitReason::memory_fault &&
				first.fault.address == alias_base, engine_label,
				"the writer's IC IVAU at an unmapped view did not fault");
			/* The kernel maps the executable view; the writer goes on. */
			const Map a_alias{a, alias_base, shared.descriptor(),
				PROT_READ | PROT_EXEC, true};
			const auto second = writer_run(engine, cpu);
			expect(second.reason == uml::cpu::ExitReason::system_call,
				engine_label, "the writer did not finish");
		});
		fresh = call(*engine_b, b, shared_base) == 2;
	}
	require(darwin_uml_static_vm_destroy(other) == 0);
	require(darwin_uml_static_vm_destroy(a) == 0);
	require(darwin_uml_static_vm_destroy(b) == 0);
	const char *what = second_vcpu ?
		(other_active ? "B kept the old code (two vCPUs, other active)" :
			"B kept the old code (two vCPUs)") :
		(other_active ? "B kept the old code (one vCPU, other active)" :
			"B kept the old code (one vCPU)");
	expect(fresh, engine_label, what);
}

} // namespace

int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	unsigned long generation = 0x6c00;
	entered_context_binding(generation);
	note_for_the_entry_loop(generation);
	for_each_engine(argc > 1 ? argv[1] : "", [&](uml::cpu::EngineKind kind) {
		unmapped_and_mapped(kind, generation);
		another_generation_active(kind, generation);
		for (const bool second_vcpu : {false, true})
			for (const bool other_active : {false, true})
				lazy_view(kind, second_vcpu, other_active, generation);
	});
	std::printf("arm64-code-synchronize-fault: %d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
