// SPDX-License-Identifier: GPL-2.0-or-later
/* How the x86-64 JIT meets x86's protocol for code that another processor
 * changes: the writer stores the code and then a flag, and a processor that
 * saw the flag and then executes a serializing instruction runs the new
 * code. The runtime hands an engine the address space's code changes only
 * between runs, so
 *
 * - a store into code that a translation holds publishes the change only
 *   once the store has landed: another vCPU that took the publication and
 *   translated the code again in between would keep the old bytes with
 *   nothing left to tell it. The address space here records the bytes of
 *   the function at every publication, for a plain store, a locked one and
 *   a repeated one;
 * - CPUID and IRET, the serializing instructions user code has, end the
 *   run. Here the load of a flag stands for another vCPU, which rewrites the
 *   function and leaves its change for the next run's hand-over;
 * - a vCPU that stays inside the engine for longer than a registration waits
 *   may still land a store that publishes nothing. The code compiled while it
 *   was stuck is dropped at each hand-over until it has left.
 *
 * Each case calls a function "mov $1, %eax; ret" so the engine translates
 * it, changes it to return 2, calls it again and makes a system call, at
 * which EAX must be 2. */
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/cpuid.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "cpu/x86_64/segments.hpp"
#include "memory/guest_address_space.hpp"
#include "platform/apple/host_features.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace x86_64 = uml::cpu::x86_64;
using uml::memory::MemoryAccess;

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t code_page = 0x1'0000;
/* The function, "mov $1, %eax; ret", alone on an executable page that the
 * program also writes. */
constexpr std::uint64_t function_page = 0x1'1000;
/* The flag another vCPU's change stands behind. */
constexpr std::uint64_t flag_page = 0x2'0000;
constexpr std::uint64_t stack_page = 0x3'0000;

class Memory final : public uml::memory::GuestAddressSpace {
public:
	explicit Memory(std::initializer_list<std::uint8_t> program)
	{
		std::size_t offset = 0;
		for (const auto byte : program)
			code_[offset++] = std::byte{byte};
		const std::uint8_t function[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
		std::memcpy(function_, function, sizeof(function));
	}

	/* The first access to the flag page rewrites the function, as another
	 * vCPU would, and leaves the change for the next hand-over. */
	bool change_at_flag{};
	bool change_pending{};
	/* Each publication of a change to the function's page, and whether the
	 * function already returned 2 when it was made. */
	unsigned publications{};
	unsigned early_publications{};
	/* How often the engine asked for the function's bytes to translate
	 * them. */
	unsigned function_fetches{};

	[[nodiscard]] bool read(std::uint64_t address,
		std::span<std::byte> destination) override
	{
		auto *source = locate(address, destination.size(), MemoryAccess::read);
		if (source == nullptr)
			return false;
		std::memcpy(destination.data(), source, destination.size());
		return true;
	}

	[[nodiscard]] bool write(std::uint64_t address,
		std::span<const std::byte> source) override
	{
		auto *target = locate(address, source.size(), MemoryAccess::write);
		if (target == nullptr)
			return false;
		std::memcpy(target, source.data(), source.size());
		return true;
	}

	[[nodiscard]] bool validate_access(std::uint64_t address, std::size_t size,
		MemoryAccess access) override
	{
		return locate(address, size, access) != nullptr;
	}

	[[nodiscard]] std::byte *atomic_pointer(std::uint64_t address,
		std::size_t size, MemoryAccess access) override
	{
		auto *pointer = locate(address, size, access);
		if (pointer != nullptr && access == MemoryAccess::execute &&
			(address & ~(page_size - 1)) == function_page)
			++function_fetches;
		if (pointer != nullptr && change_at_flag && !change_pending &&
			(address & ~(page_size - 1)) == flag_page) {
			function_[1] = std::byte{2};
			change_at_flag = false;
			change_pending = true;
		}
		return pointer;
	}

	/* The function's bytes, for a store that lands without any publication. */
	[[nodiscard]] std::byte *function_bytes() noexcept { return function_; }

	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override
	{
		const auto page = address & ~(page_size - 1);
		return page == code_page || page == function_page;
	}

	void invalidate_code(std::uint64_t address) override
	{
		if ((address & ~(page_size - 1)) != function_page)
			return;
		++publications;
		if (function_[1] != std::byte{2})
			++early_publications;
	}

private:
	[[nodiscard]] std::byte *locate(std::uint64_t address, std::size_t size,
		MemoryAccess access) noexcept
	{
		const auto page = address & ~(page_size - 1);
		const auto offset = address - page;
		if (size > page_size - offset)
			return nullptr;
		switch (page) {
		case code_page:
			return access == MemoryAccess::write ? nullptr : code_ + offset;
		case function_page:
			return function_ + offset;
		case flag_page:
			return access == MemoryAccess::execute ? nullptr : flag_ + offset;
		case stack_page:
			return access == MemoryAccess::execute ? nullptr : stack_ + offset;
		default:
			return nullptr;
		}
	}

	alignas(page_size) std::byte code_[page_size]{};
	alignas(page_size) std::byte function_[page_size]{};
	alignas(page_size) std::byte flag_[page_size]{};
	alignas(page_size) std::byte stack_[page_size]{};
};

int failures;

void expect(bool condition, std::string_view test, const char *what)
{
	if (condition)
		return;
	std::printf("FAIL %.*s: %s\n", static_cast<int>(test.size()), test.data(), what);
	++failures;
}

/* Runs the program from its first byte to its system call as the runtime
 * does: before each run the engine is handed the changes made since the
 * last, here the function's page once the flag has been read. Returns EAX at
 * the system call, and in `runs` how many runs that took. */
std::uint64_t run_to_system_call(Memory &memory, unsigned long generation,
	unsigned &runs, bool &finished, x86_64::CpuState *at_end = nullptr)
{
	x86_64::jit::FexEngine engine{uml::platform::apple::probed_host_capabilities()};
	const x86_64::jit::GuestRange function{function_page, page_size};
	std::uint64_t sequence = 1;
	engine.apply_changes(generation, x86_64::ExecutionMode::long64,
		{.mapping_sequence = 1, .code_sequence = sequence}, false);
	x86_64::CpuState cpu{};
	cpu.segment_selectors = {0, 0x33, 0x2b, 0, 0, 0};
	(void)x86_64::enter_segments(cpu);
	cpu.rip = code_page;
	cpu.rflags = 0x202;
	cpu.gpr(x86_64::Register::rsp) = stack_page + page_size;
	uml::cpu::InterruptToken interrupt{};
	finished = false;
	for (runs = 1; runs <= 16; ++runs) {
		if (memory.change_pending) {
			memory.change_pending = false;
			engine.apply_changes(generation, x86_64::ExecutionMode::long64,
				{.mapping_sequence = 1, .code_sequence = ++sequence,
					.code_ranges = {&function, 1}},
				false);
		}
		const auto result = engine.run(cpu, memory, {
			.generation = generation,
			.instruction_budget = 1000,
			.execution_context = nullptr,
			.interrupt = &interrupt,
			.orders_stores = false,
		});
		if (result.execution.reason == uml::cpu::ExitReason::system_call) {
			finished = true;
			break;
		}
		if (result.execution.reason != uml::cpu::ExitReason::instruction_budget)
			break;
	}
	if (at_end != nullptr)
		*at_end = cpu;
	return cpu.gpr(x86_64::Register::rax);
}

/* A serializing instruction between the flag and the second call. */
void test_serializing(std::string_view name, unsigned long generation,
	std::initializer_list<std::uint8_t> program, x86_64::CpuState *at_end = nullptr)
{
	Memory memory{program};
	memory.change_at_flag = true;
	unsigned runs = 0;
	bool finished = false;
	const auto eax = run_to_system_call(memory, generation, runs, finished, at_end);
	expect(finished, name, "the program did not reach its system call");
	expect(!memory.change_at_flag, name, "the program did not read the flag");
	expect(eax == 2, name, "the call after the serializing instruction ran the old code");
	std::printf("%s %.*s: eax=%llu runs=%u\n", eax == 2 ? "ok" : "FAIL",
		static_cast<int>(name.size()), name.data(),
		static_cast<unsigned long long>(eax), runs);
}

/* The program's own store into the function. */
void test_store(std::string_view name, unsigned long generation,
	std::initializer_list<std::uint8_t> program)
{
	Memory memory{program};
	unsigned runs = 0;
	bool finished = false;
	const auto eax = run_to_system_call(memory, generation, runs, finished);
	expect(finished, name, "the program did not reach its system call");
	expect(memory.publications != 0, name, "the store published no change");
	expect(memory.early_publications == 0, name,
		"a change was published before its store landed");
	expect(eax == 2, name, "the call after the store ran the old code");
	std::printf("%s %.*s: eax=%llu publications=%u early=%u\n",
		memory.early_publications == 0 && eax == 2 ? "ok" : "FAIL",
		static_cast<int>(name.size()), name.data(),
		static_cast<unsigned long long>(eax), memory.publications,
		memory.early_publications);
}

/* A vCPU of another process stays inside the engine for longer than a
 * registration waits, and a store it translated lands afterwards without
 * publishing anything. The runtime's hand-over drops the code compiled while
 * the vCPU was stuck, so the call after the store runs the new code, and the
 * code is cached again once the vCPU has left.
 *
 * mov $0x11000, %esi; call *%rsi; five times with "xor %eax, %eax; xor %ecx,
 * %ecx; cpuid" between the calls, each of which ends the run; syscall. */
void test_stuck_participant(unsigned long generation)
{
	using namespace std::chrono_literals;
	constexpr std::string_view name = "stuck";
	Memory memory{{0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6,
		0x31, 0xc0, 0x31, 0xc9, 0x0f, 0xa2, 0xff, 0xd6,
		0x31, 0xc0, 0x31, 0xc9, 0x0f, 0xa2, 0xff, 0xd6,
		0x31, 0xc0, 0x31, 0xc9, 0x0f, 0xa2, 0xff, 0xd6,
		0x31, 0xc0, 0x31, 0xc9, 0x0f, 0xa2, 0xff, 0xd6, 0x0f, 0x05}};
	/* The registry of the generation, made before the engine's space joins
	 * it, with a wait short enough for a test. */
	const auto registry = x86_64::CodePageRegistry::acquire(
		generation, {.slice = 10ms, .slices = 3});
	x86_64::CodePageRegistry::Participant stuck;
	registry->join(stuck);
	stuck.engine_epoch.store(1, std::memory_order_release);

	x86_64::jit::FexEngine engine{uml::platform::apple::probed_host_capabilities()};
	engine.apply_changes(generation, x86_64::ExecutionMode::long64,
		{.mapping_sequence = 1, .code_sequence = 1}, false);
	x86_64::CpuState cpu{};
	cpu.segment_selectors = {0, 0x33, 0x2b, 0, 0, 0};
	(void)x86_64::enter_segments(cpu);
	cpu.rip = code_page;
	cpu.rflags = 0x202;
	cpu.gpr(x86_64::Register::rsp) = stack_page + page_size;
	uml::cpu::InterruptToken interrupt{};
	unsigned fetches[6]{};
	bool finished = false;
	for (unsigned run = 1; run <= 5; ++run) {
		if (run > 1) {
			if (run == 2)
				/* The store that lands late, publishing nothing. */
				memory.function_bytes()[1] = std::byte{2};
			if (run == 4)
				stuck.engine_epoch.store(2, std::memory_order_release);
			engine.settle_stuck_participants(generation, x86_64::ExecutionMode::long64);
		}
		const auto result = engine.run(cpu, memory, {
			.generation = generation,
			.instruction_budget = 1000,
			.execution_context = nullptr,
			.interrupt = &interrupt,
			.orders_stores = false,
		});
		fetches[run] = memory.function_fetches;
		finished = result.execution.reason == uml::cpu::ExitReason::system_call;
		if (!finished && result.execution.reason != uml::cpu::ExitReason::instruction_budget &&
			run == 5)
			break;
	}
	registry->leave(stuck);
	expect(registry->leaves_inside_engine() == 0, name,
		"a participant left the registry inside the engine");
	/* The program's page and the function's page both compiled beside the
	 * stuck participant; each is dropped at the three hand-overs that found
	 * the record, the last of which dropped it. */
	expect(engine.diagnostics().escalation_invalidations == 6, name,
		"the hand-overs did not drop each stuck page once");
	expect(finished, name, "the program did not reach its system call");
	expect(cpu.gpr(x86_64::Register::rax) == 2, name,
		"the call after the late store ran the old code");
	expect(fetches[1] == 1, name, "the first call did not translate the function");
	expect(fetches[2] > fetches[1] && fetches[3] > fetches[2], name,
		"code compiled while a vCPU was stuck survived a hand-over");
	expect(fetches[4] > fetches[3], name,
		"the hand-over after the vCPU left did not drop the code");
	expect(fetches[5] == fetches[4], name,
		"the code was not cached once the vCPU had left");
	std::printf("%s %.*s: eax=%llu fetches=%u,%u,%u,%u,%u invalidations=%llu\n",
		failures == 0 ? "ok" : "FAIL", static_cast<int>(name.size()), name.data(),
		static_cast<unsigned long long>(cpu.gpr(x86_64::Register::rax)),
		fetches[1], fetches[2], fetches[3], fetches[4], fetches[5],
		static_cast<unsigned long long>(
			engine.diagnostics().escalation_invalidations));
}

} // namespace

int main()
{
	if (!x86_64::jit::fex_engine_available(
		uml::platform::apple::probed_host_capabilities())) {
		std::puts("SKIP the x86-64 JIT cannot run on this host");
		return 0;
	}
	/* mov $0x11000, %esi; call *%rsi; movb $2, 1(%rsi); call *%rsi;
	 * syscall */
	test_store("store", 1, {0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6, 0xc6,
		0x46, 0x01, 0x02, 0xff, 0xd6, 0x0f, 0x05});
	/* The same with movl $2, 1(%rsi), four bytes across the immediate. */
	test_store("store-4", 2, {0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6, 0xc7,
		0x46, 0x01, 0x02, 0x00, 0x00, 0x00, 0xff, 0xd6, 0x0f, 0x05});
	/* The same with lock xorb $3, 1(%rsi). */
	test_store("locked", 3, {0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6, 0xf0,
		0x80, 0x76, 0x01, 0x03, 0xff, 0xd6, 0x0f, 0x05});
	/* The same with lea 1(%rsi), %rdi; mov $2, %eax; mov $1, %ecx; cld;
	 * rep stosb. */
	test_store("rep-stosb", 4, {0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6, 0x48,
		0x8d, 0x7e, 0x01, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xb9, 0x01, 0x00, 0x00,
		0x00, 0xfc, 0xf3, 0xaa, 0xff, 0xd6, 0x0f, 0x05});
	/* mov $0x11000, %esi; call *%rsi; mov 0x20000, %ebx; xor %eax, %eax;
	 * xor %ecx, %ecx; cpuid; call *%rsi; syscall. CPUID's own results
	 * survive its exit (RCX is SYSCALL's). */
	x86_64::CpuState after_cpuid{};
	test_serializing("cpuid", 6, {0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6,
		0x8b, 0x1c, 0x25, 0x00, 0x00, 0x02, 0x00, 0x31, 0xc0, 0x31, 0xc9,
		0x0f, 0xa2, 0xff, 0xd6, 0x0f, 0x05}, &after_cpuid);
	const auto vendor = x86_64::cpuid(0, 0);
	expect(after_cpuid.gpr(x86_64::Register::rbx) == vendor.ebx &&
		after_cpuid.gpr(x86_64::Register::rdx) == vendor.edx,
		"cpuid", "CPUID's results did not survive its exit");
	/* mov $0x11000, %esi; call *%rsi; mov 0x20000, %ebx; mov %ss, %eax;
	 * push %rax; push %rsp; addq $8, (%rsp); pushfq; mov %cs, %eax;
	 * push %rax; lea 1f(%rip), %rax; push %rax; iretq; 1: call *%rsi;
	 * syscall */
	test_serializing("iret", 5, {0xbe, 0x00, 0x10, 0x01, 0x00, 0xff, 0xd6,
		0x8b, 0x1c, 0x25, 0x00, 0x00, 0x02, 0x00, 0x8c, 0xd0, 0x50, 0x54,
		0x48, 0x83, 0x04, 0x24, 0x08, 0x9c, 0x8c, 0xc8, 0x50, 0x48, 0x8d,
		0x05, 0x03, 0x00, 0x00, 0x00, 0x50, 0x48, 0xcf, 0xff, 0xd6, 0x0f,
		0x05});
	test_stuck_participant(7);
	std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
