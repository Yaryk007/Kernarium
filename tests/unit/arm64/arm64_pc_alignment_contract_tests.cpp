// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "require.hpp"

#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <signal.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace {

using ::uml::cpu::EngineKind;
using ::uml::emulator::tests::JitBacking;
using ::uml::emulator::tests::JitMapping;

constexpr std::uint64_t code_base = 0x5d00'0000;
constexpr std::uint64_t predecessor_source = code_base + 0x40;
constexpr std::uint32_t indirect_branch_x0 = 0xd61f'0000U;
constexpr std::uint32_t system_call = 0xd400'0001U;

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] EngineKind parse_engine(std::string_view name)
{
	if (name == "gadgets")
		return EngineKind::gadgets;
	if (name == "jit")
		return EngineKind::jit;
	fail("unknown child engine");
}

[[nodiscard]] const char *engine_name(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::gadgets:
		return "gadgets";
	case EngineKind::jit:
		return "jit";
	}
	return "unknown";
}

using EntryPoint = int (*)(void *, void *, unsigned long, unsigned long,
	unsigned long *);

[[nodiscard]] EntryPoint bridge_for(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::gadgets:
		return &darwin_uml_static_enter_guest;
	case EngineKind::jit:
		return &darwin_uml_jit_enter_guest;
	}
	return nullptr;
}

[[nodiscard]] ::uml::cpu::ExecutionResult run(
	::uml::cpu::CpuEngine &engine, ::uml::cpu::arm64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory, unsigned long generation)
{
	::uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = 8,
	};
	return engine.run(request);
}

void require_alignment_result(
	const ::uml::cpu::ExecutionResult &result,
	const ::uml::cpu::arm64::CpuState &cpu, std::uint64_t address,
	std::size_t retired)
{
	require(result.reason == ::uml::cpu::ExitReason::pc_alignment,
		"engine did not report a PC-alignment exit");
	require(result.retired_instructions == retired,
		"PC-alignment exit lost its precise retired prefix");
	require(cpu.pc == address,
		"PC-alignment exit changed the faulting instruction address");
	require(result.fault.address == address,
		"PC-alignment exit reported the wrong address");
	require(result.fault.access == ::uml::memory::MemoryAccess::execute,
		"PC-alignment exit lost its instruction-access direction");
}

void direct_engine_contract(EngineKind kind, unsigned long generation)
{
	JitBacking backing{1};
	backing.put32(0, indirect_branch_x0);
	backing.put32(4, system_call);
	backing.put32(predecessor_source - code_base,
		0x9100'0508U); // add x8, x8, #1
	backing.put32(predecessor_source - code_base + 4,
		0x9100'0929U); // add x9, x9, #2
	backing.put32(predecessor_source - code_base + 8,
		indirect_branch_x0);
	JitMapping mapping{generation, code_base, backing, 1,
		PROT_READ | PROT_EXEC};
	::uml::memory::StaticGuestMemory memory;
	memory.set_generation(generation);
	auto engine = ::uml::cpu::make_cpu_engine(kind);
	require(engine != nullptr && engine->kind() == kind,
		"factory substituted a different engine");

	::uml::cpu::arm64::CpuState cpu;
	cpu.pc = code_base + 2;
	const auto entry = run(*engine, cpu, memory, generation);
	require_alignment_result(entry, cpu, code_base + 2, 0);

	/* Reuse the exact engine after the exceptional entry. */
	cpu.pc = code_base + 4;
	const auto entry_retry = run(*engine, cpu, memory, generation);
	require(entry_retry.reason == ::uml::cpu::ExitReason::system_call &&
		entry_retry.retired_instructions == 1 &&
		cpu.pc == code_base + 8,
		"engine was not reusable after a misaligned entry");

	/* The indirect transfer retires, then the next-instruction alignment
	 * check reports its target without fetching from that address. */
	cpu = {};
	cpu.pc = code_base;
	cpu.registers[0] = code_base + 2;
	const auto branch = run(*engine, cpu, memory, generation);
	require_alignment_result(branch, cpu, code_base + 2, 1);

	cpu = {};
	cpu.pc = code_base;
	cpu.registers[0] = code_base + 4;
	const auto branch_retry = run(*engine, cpu, memory, generation);
	require(branch_retry.reason == ::uml::cpu::ExitReason::system_call &&
		branch_retry.retired_instructions == 2 &&
		cpu.pc == code_base + 8,
		"engine cache/link state was not reusable after a bad target");

	/* Immediate branches cannot create a misaligned AArch64 PC, so only a
	 * BR reaches the alignment check after earlier instructions of its block
	 * have written registers. The exit must report those x8/x9 writes. */
	cpu = {};
	cpu.pc = predecessor_source;
	cpu.registers[0] = code_base + 2;
	cpu.registers[8] = 10;
	cpu.registers[9] = 20;
	const auto predecessor = run(*engine, cpu, memory, generation);
	require_alignment_result(predecessor, cpu, code_base + 2, 3);
	require(cpu.registers[8] == 11 && cpu.registers[9] == 22,
		"PC-alignment exit lost the block's x8/x9 writes");
}

void bridge_contract(EngineKind kind, unsigned long generation)
{
	JitBacking backing{1};
	backing.put32(0, indirect_branch_x0);
	backing.put32(4, system_call);
	backing.put32(predecessor_source - code_base,
		0x9100'0508U); // add x8, x8, #1
	backing.put32(predecessor_source - code_base + 4,
		0x9100'0929U); // add x9, x9, #2
	backing.put32(predecessor_source - code_base + 8,
		indirect_branch_x0);
	JitMapping mapping{generation, code_base, backing, 1,
		PROT_READ | PROT_EXEC};
	const auto enter = bridge_for(kind);
	require(enter != nullptr, "engine bridge is unavailable");
	uml::testing::GuestFpTail fp{};
	unsigned long tls = 0;

	darwin_uml_guest_registers registers{};
	registers.pc = code_base + 2;
	const auto entry = enter(&registers, &fp, sizeof(fp), generation, &tls);
	require(entry == DARWIN_UML_EVENT_PC_ALIGNMENT,
		"C bridge did not export the PC-alignment event");
	require(registers.pc == code_base + 2 &&
		registers.fault.address == code_base + 2 &&
		registers.fault.error_code == 0 &&
		registers.fault.trap_number == 0,
		"C bridge changed PC-alignment metadata");

	registers.pc = code_base + 4;
	const auto entry_retry = enter(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(entry_retry == DARWIN_UML_EVENT_SYSCALL &&
		registers.pc == code_base + 8,
		"C bridge was not reusable after a misaligned entry");

	registers = {};
	registers.pc = code_base;
	registers.x[0] = code_base + 2;
	const auto branch = enter(&registers, &fp, sizeof(fp), generation, &tls);
	require(branch == DARWIN_UML_EVENT_PC_ALIGNMENT &&
		registers.pc == code_base + 2 &&
		registers.fault.address == code_base + 2,
		"C bridge lost a misaligned indirect target");

	registers = {};
	registers.pc = code_base;
	registers.x[0] = code_base + 4;
	const auto branch_retry = enter(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(branch_retry == DARWIN_UML_EVENT_SYSCALL &&
		registers.pc == code_base + 8,
		"C bridge cache/link state was not reusable after a bad target");

	registers = {};
	registers.pc = predecessor_source;
	registers.x[0] = code_base + 2;
	registers.x[8] = 30;
	registers.x[9] = 40;
	const auto predecessor = enter(
		&registers, &fp, sizeof(fp), generation, &tls);
	require(predecessor == DARWIN_UML_EVENT_PC_ALIGNMENT &&
		registers.pc == code_base + 2 &&
		registers.fault.address == code_base + 2 &&
		registers.x[8] == 31 && registers.x[9] == 42,
		"C bridge lost the block's x8/x9 writes at a PC-alignment exit");
}

void data_alignment_contract(EngineKind kind, unsigned long generation)
{
	JitBacking backing{1};
	backing.put32(0, 0x9100'0508U); // add x8, x8, #1
	backing.put32(4, 0x9940'00f5U); // ldapur w21, [x7] (FEX TSO load)
	backing.put32(8, system_call);
	backing.put32(12, 0x9900'00f5U); // stlur w21, [x7]
	backing.put32(16, system_call);
	backing.put32(0x100, 0x1234'5678U);
	JitMapping mapping{generation, code_base, backing, 1};
	::uml::memory::StaticGuestMemory memory;
	memory.set_generation(generation);
	auto engine = ::uml::cpu::make_cpu_engine(kind);
	/* The misaligned address crosses a 16-byte block, so the ordered
	 * access faults whether or not the host implements FEAT_LSE2. */
	for (const bool write : {false, true}) {
		::uml::cpu::arm64::CpuState cpu;
		cpu.pc = code_base + (write ? 12 : 0);
		cpu.registers[7] = code_base + 0x10e;
		cpu.registers[8] = 40;
		cpu.registers[21] = 0xaabb'ccdd;
		const auto result = run(*engine, cpu, memory, generation);
		require(result.reason == ::uml::cpu::ExitReason::memory_fault &&
			result.fault.alignment &&
			result.fault.address == code_base + 0x10e &&
			result.fault.access == (write ?
				::uml::memory::MemoryAccess::write :
				::uml::memory::MemoryAccess::read),
			"engine lost data-alignment cause, address, or direction");
		require(cpu.pc == code_base + (write ? 12 : 4) &&
			result.retired_instructions == (write ? 0U : 1U) &&
			cpu.registers[21] == 0xaabb'ccdd &&
			cpu.registers[8] == (write ? 40U : 41U),
			"data-alignment fault changed the precise architectural state");
		cpu.registers[7] = code_base + 0x100;
		const auto retry = run(*engine, cpu, memory, generation);
		require(retry.reason == ::uml::cpu::ExitReason::system_call,
			"aligned retry failed after a data-alignment fault");
		cpu.pc = code_base + 4;
		cpu.registers[7] = code_base + 2 * DARWIN_UML_GUEST_PAGE_SIZE;
		const auto missing = run(*engine, cpu, memory, generation);
		require(missing.reason == ::uml::cpu::ExitReason::memory_fault &&
			!missing.fault.alignment,
			"ordinary page fault inherited a stale alignment cause");
	}
	const auto enter = bridge_for(kind);
	uml::testing::GuestFpTail fp{};
	unsigned long tls = 0;
	darwin_uml_guest_registers registers{};
	registers.pc = code_base;
	registers.x[7] = code_base + 0x10e;
	registers.x[21] = 0xaabb'ccdd;
	engine.reset();
	const auto event = enter(&registers, &fp, sizeof(fp), generation, &tls);
	require(event == DARWIN_UML_EVENT_DATA_ALIGNMENT &&
		registers.pc == code_base + 4 &&
		registers.fault.address == code_base + 0x10e &&
		registers.x[21] == 0xaabb'ccdd,
		"bridge did not preserve the FEX alignment-fault contract");
}

/* A cached return to an address the guest cannot execute from. The sites
 * below hold two other targets in both of their cells and the gadget
 * engine's return ring has no record for the target, which is the state a
 * guest reaches by calling any function twice through different callers and
 * then returning to 0xffffffffffffffff. The engine must give the guest the
 * fault it gives for the same target reached by a cold site, never a host
 * signal. */
struct ReturnForm final {
	const char *name;
	std::uint64_t site;
	std::uint64_t cold_site;
	std::size_t retired;
	/* The instruction before the form's site in the popped-ring scenario
	 * that sets the target. */
	std::uint32_t pop_setup;
};

constexpr std::uint32_t ret_x15 = 0xd65f'01e0U;
constexpr std::uint32_t ret_x30 = 0xd65f'03c0U;
constexpr std::uint32_t ldp_x29_x30_post = 0xa8c1'7bfdU;
constexpr std::uint32_t movn_x15 = 0x9280'000fU;
constexpr std::uint32_t movn_x30 = 0x9280'001eU;
constexpr std::uint32_t nop = 0xd503'201fU;

constexpr std::uint64_t target_one = code_base + 0x10;
constexpr std::uint64_t target_two = code_base + 0x20;
constexpr std::uint64_t frames = 0x800;
constexpr std::uint64_t all_ones = ~std::uint64_t{};
/* N and C set: a return that faults must leave the flags as they were. */
constexpr std::uint32_t seeded_nzcv = 0xa000'0000U;

constexpr std::array return_forms{
	ReturnForm{"RET X15", code_base + 0x000, code_base + 0x080, 1, movn_x15},
	ReturnForm{"RET", code_base + 0x030, code_base + 0x0b0, 1, movn_x30},
	/* LDP X29, X30, [SP], #16 ; RET is one fused epilogue head. */
	ReturnForm{"LDP X29, X30, [SP], #16 ; RET", code_base + 0x040,
		code_base + 0x0c0, 2, nop},
};

constexpr std::uint64_t call_and_exit = code_base + 0x100;
constexpr std::uint64_t exit_callee = code_base + 0x120;
constexpr std::uint64_t popper_ret = code_base + 0x1a0;
constexpr std::uint64_t popped_sequence = code_base + 0x200;

[[nodiscard]] constexpr std::uint32_t branch_to(
	std::uint64_t from, std::uint64_t to, bool link) noexcept
{
	const auto words = static_cast<std::int64_t>(to - from) / 4;
	return (link ? 0x9400'0000U : 0x1400'0000U) |
		(static_cast<std::uint32_t>(words) & 0x03ff'ffffU);
}

void put_return_code(JitBacking &backing)
{
	backing.put32(target_one - code_base, system_call);
	backing.put32(target_two - code_base, system_call);
	for (const auto &form : return_forms) {
		for (const auto site : {form.site, form.cold_site}) {
			if (form.retired == 2) {
				backing.put32(site - code_base, ldp_x29_x30_post);
				backing.put32(site - code_base + 4, ret_x30);
			} else {
				backing.put32(site - code_base,
					&form == &return_forms[0] ? ret_x15 : ret_x30);
			}
		}
	}
	/* A call that leaves a return record in the ring and exits through a
	 * guest system call. */
	backing.put32(call_and_exit - code_base,
		branch_to(call_and_exit, exit_callee, true));
	backing.put32(call_and_exit - code_base + 4, system_call);
	backing.put32(exit_callee - code_base, system_call);
	/* A polymorphic return site that pops the record a call pushed, then
	 * the form's site with the ring empty again, all in one run. */
	backing.put32(popper_ret - code_base, ret_x30);
	for (std::size_t index = 0; index < return_forms.size(); ++index) {
		const auto &form = return_forms[index];
		const auto base = popped_sequence + 0x20 * index;
		backing.put32(base - code_base, branch_to(base, popper_ret, true));
		backing.put32(base - code_base + 4, form.pop_setup);
		backing.put32(base - code_base + 8,
			branch_to(base + 8, form.site, false));
	}
}

[[nodiscard]] ::uml::cpu::arm64::CpuState return_state(const ReturnForm &form,
	JitBacking &backing, std::uint64_t site, std::uint64_t target,
	unsigned slot, bool tbi)
{
	::uml::cpu::arm64::CpuState cpu;
	cpu.pc = site;
	cpu.nzcv = seeded_nzcv;
	cpu.tbi_enabled = tbi;
	if (&form == &return_forms[0]) {
		cpu.registers[15] = target;
	} else if (&form == &return_forms[1]) {
		cpu.registers[30] = target;
	} else {
		cpu.sp = code_base + frames + 16 * slot;
		backing.put64(frames + 16 * slot, 0);
		backing.put64(frames + 16 * slot + 8, target);
	}
	return cpu;
}

[[nodiscard]] ::uml::cpu::ExecutionResult run_budget(
	::uml::cpu::CpuEngine &engine, ::uml::cpu::arm64::CpuState &cpu,
	::uml::memory::StaticGuestMemory &memory, unsigned long generation,
	std::size_t budget)
{
	::uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = budget,
	};
	return engine.run(request);
}

void require_same_exit(const ::uml::cpu::ExecutionResult &actual,
	const ::uml::cpu::arm64::CpuState &actual_cpu,
	const ::uml::cpu::ExecutionResult &cold,
	const ::uml::cpu::arm64::CpuState &cold_cpu, std::size_t extra_retired)
{
	require(actual.reason == cold.reason &&
		actual.retired_instructions ==
			cold.retired_instructions + extra_retired &&
		actual.fault.address == cold.fault.address &&
		actual.fault.access == cold.fault.access &&
		actual.fault.alignment == cold.fault.alignment &&
		actual.syndrome == cold.syndrome &&
		actual_cpu.pc == cold_cpu.pc,
		"a trained return site exited differently from a cold one");
	/* A fetch fault has no syndrome, and the flags are the guest's own. */
	require(actual.syndrome == 0 && cold.syndrome == 0,
		"a return fault reported a syndrome");
	require(actual_cpu.nzcv == seeded_nzcv && cold_cpu.nzcv == seeded_nzcv,
		"a return fault changed the flags");
}

/* One world per case, so that no site, table or ring survives from another.
 * `popped` reaches the site with the ring emptied by the return before it;
 * `after_call` first leaves a record in the ring and exits through a guest
 * system call, the next entry resetting the ring. */
enum class RingState { initial, after_call, popped };

void cached_return_case(EngineKind kind, unsigned long generation,
	JitBacking &backing, std::size_t form_index, RingState ring,
	std::uint64_t target, bool tbi)
{
	const auto &form = return_forms[form_index];
	JitMapping mapping{generation, code_base, backing, 1,
		PROT_READ | PROT_EXEC};
	::uml::memory::StaticGuestMemory memory;
	memory.set_generation(generation);
	auto engine = ::uml::cpu::make_cpu_engine(kind);
	require(engine != nullptr && engine->kind() == kind,
		"factory substituted a different engine");

	/* The cold twin gives the exit the architecture and this engine
	 * give for the target without any cache state. */
	auto cold_cpu = return_state(form, backing, form.cold_site, target, 3, tbi);
	const auto cold = run_budget(*engine, cold_cpu, memory, generation, 8);
	require(cold.reason == ::uml::cpu::ExitReason::pc_alignment ||
		cold.reason == ::uml::cpu::ExitReason::memory_fault,
		"a return to an unmapped target did not fault");

	/* {one, two, one} fills both cells of the site with other targets. */
	const std::array<std::uint64_t, 3> trained{
		target_one, target_two, target_one};
	for (unsigned slot = 0; slot < trained.size(); ++slot) {
		auto cpu = return_state(
			form, backing, form.site, trained[slot], slot, false);
		const auto result = run_budget(*engine, cpu, memory, generation, 8);
		require(result.reason == ::uml::cpu::ExitReason::system_call,
			"a return site did not reach its training target");
	}

	auto cpu = return_state(form, backing, form.site, target, 3, tbi);
	std::size_t extra = 0;
	if (ring == RingState::after_call) {
		auto caller = return_state(form, backing, call_and_exit, 0, 0, false);
		const auto exited = run_budget(*engine, caller, memory, generation, 8);
		require(exited.reason == ::uml::cpu::ExitReason::system_call,
			"the call did not exit through its system call");
		/* The same CPU state enters again, the ring still holding the
		 * call's record until this entry resets it. */
		caller.pc = form.site;
		caller.tbi_enabled = tbi;
		caller.sp = cpu.sp;
		caller.registers[15] = cpu.registers[15];
		caller.registers[30] = cpu.registers[30];
		cpu = caller;
	} else if (ring == RingState::popped) {
		/* Train the popper on two other targets so that it consults the
		 * ring, which holds the record of the call that reached it. */
		for (unsigned slot = 0; slot < trained.size(); ++slot) {
			::uml::cpu::arm64::CpuState popper;
			popper.pc = popper_ret;
			popper.registers[30] = trained[slot];
			const auto result =
				run_budget(*engine, popper, memory, generation, 8);
			require(result.reason == ::uml::cpu::ExitReason::system_call,
				"the popping return did not reach its training target");
		}
		cpu.pc = popped_sequence + 0x20 * form_index;
		extra = 4;
	}
	const auto actual = run_budget(*engine, cpu, memory, generation, 16);
	require_same_exit(actual, cpu, cold, cold_cpu, extra);
	if (target == all_ones) {
		require(actual.reason == ::uml::cpu::ExitReason::pc_alignment &&
			actual.retired_instructions == extra + form.retired &&
			cpu.pc == all_ones &&
			actual.fault.address == all_ones &&
			actual.fault.access == ::uml::memory::MemoryAccess::execute,
			"a cached return to ~0 did not report a PC-alignment exit");
		if (form_index == 2)
			require(cpu.registers[30] == all_ones &&
				cpu.registers[29] == 0 &&
				cpu.sp == code_base + frames + 16 * 3 + 16,
				"the epilogue's restores were not retired exactly");
	}
}

void cached_return_contract(EngineKind kind, unsigned long first_generation)
{
	JitBacking backing{1};
	put_return_code(backing);
	constexpr std::array<std::uint64_t, 12> targets{
		all_ones,
		/* Near all ones: one word below is aligned, the rest are not. */
		0xffff'ffff'ffff'fffcULL,
		0xffff'ffff'ffff'fffdULL,
		0xffff'ffff'ffff'fff0ULL,
		/* The values the JIT's and the tables' empty cells use. */
		0,
		1,
		0x1002,
		0xdead'beef'0000'1000ULL,
		/* Tagged addresses. */
		0xb100'0000'0000'1000ULL,
		0xff00'ffff'ffff'ffffULL,
		0xaaff'ffff'ffff'ffffULL,
		0x0000'0000'dead'0000ULL,
	};
	unsigned long generation = first_generation;
	for (std::size_t form = 0; form < return_forms.size(); ++form)
		for (const auto ring : {RingState::initial, RingState::after_call,
			     RingState::popped})
			for (const auto target : targets) {
				/* The popped sequence is the guard for the
				 * crashing target only. */
				if (ring == RingState::popped && target != all_ones)
					continue;
				for (const bool tbi : {false, true})
					cached_return_case(kind, generation++, backing,
						form, ring, target, tbi);
			}
}

int run_child(EngineKind kind)
{
	if (!::uml::cpu::cpu_engine_available(kind))
		return 77;
	const auto ordinal = static_cast<unsigned long>(kind);
	/* First: a JIT engine made after the bridge contract reports fatal. */
	cached_return_contract(kind, 0x9000 + ordinal * 0x400);
	direct_engine_contract(kind, 0x8d00 + ordinal * 2);
	data_alignment_contract(kind, 0x8e00 + ordinal);
	bridge_contract(kind, 0x8d01 + ordinal * 2);
	return EXIT_SUCCESS;
}

void run_subprocess(const char *executable, EngineKind kind)
{
	const auto *name = engine_name(kind);
	std::array arguments{
		const_cast<char *>(executable),
		const_cast<char *>("--child"),
		const_cast<char *>(name),
		static_cast<char *>(nullptr),
	};
	pid_t child{};
	const auto spawned = posix_spawnp(
		&child, executable, nullptr, nullptr, arguments.data(), environ);
	require(spawned == 0, "cannot spawn isolated engine child");

	int status{};
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds{20};
	for (;;) {
		const auto waited = waitpid(child, &status, WNOHANG);
		if (waited == child)
			break;
		require(waited == 0 || (waited < 0 && errno == EINTR),
			"cannot wait for isolated engine child");
		if (std::chrono::steady_clock::now() >= deadline) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, &status, 0);
			fail("isolated engine child timed out");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
		return;
	require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
		"isolated engine child terminated or failed");
}

} // namespace

int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	if (argc == 3 && std::string_view{argv[1]} == "--child")
		return run_child(parse_engine(argv[2]));
	require(argc >= 1 && argv[0] != nullptr,
		"test executable path is unavailable");
	for (const auto kind : {EngineKind::gadgets, EngineKind::jit})
		run_subprocess(argv[0], kind);
	std::puts("arm64-pc-alignment-contract: passed");
	return EXIT_SUCCESS;
}
