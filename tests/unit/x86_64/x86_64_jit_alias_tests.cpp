// SPDX-License-Identifier: GPL-2.0-or-later
/* The x86-64 JIT over a frame that more than one address maps, on the real
 * static MMU. A frame is compiled at one address (X) and written through
 * another (W); the rows read what the guest can see and the counters the
 * engine and the MMU keep for what must not happen.
 *
 *   store      a store through W reaches X's code: the unit that compiled X
 *              registered its frame first, W's page publishes though it is
 *              not executable, and the next run of X executes the new bytes.
 *   running    a store through W into the unit that is running leaves the
 *              unit before the store, which then runs as a unit of its own:
 *              the guest sees the new immediate.
 *   decoder    a page the engine compiled registers its frame once while its
 *              decoder range stands, and again after the page's holder was
 *              removed and the page mapped back.
 *   failure    a registration that fails reports the fetch fault the kernel
 *              resolves, retires nothing and caches nothing; the run goes on
 *              once a registration succeeds, and 64 failures in a row end the
 *              space. */
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "cpu/execution_types.hpp"
#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "cpu/x86_64/segments.hpp"
#include "frame_accounting.hpp"
#include "memory/static_guest_memory.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <sys/mman.h>
#include <thread>
#include <vector>

/* The next allocation of exactly this many bytes fails: the registry's page
 * counts are the only array of this size a space makes. */
std::atomic<std::size_t> failing_array_size{0};

void *operator new[](std::size_t size)
{
	auto armed = failing_array_size.load(std::memory_order_relaxed);
	if (armed != 0 && size == armed &&
	    failing_array_size.compare_exchange_strong(armed, 0))
		throw std::bad_alloc{};
	return ::operator new(size);
}

namespace {

namespace x86_64 = uml::cpu::x86_64;
namespace jit = uml::cpu::x86_64::jit;
using uml::memory::FrameAccounting;
using uml::memory::StaticGuestMemory;
using uml::memory::WriterLocation;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rw = PROT_READ | PROT_WRITE;
constexpr int rx = PROT_READ | PROT_EXEC;

[[nodiscard]] bool generation_live(unsigned long generation) noexcept
{
	return darwin_uml_static_vm_generation_live(generation) != 0;
}

/* One address space and one engine on it, run as the runtime runs one: the
 * execution context entered, the engine handed what changed, then the run. */
class Space final {
public:
	/* `owner` is false for a second vCPU of a generation another Space made
	 * and will destroy. */
	explicit Space(unsigned long generation, bool owner = true)
		: generation_{generation},
		  owner_{owner},
		  context_{darwin_uml_static_vm_execution_context_create(generation)},
		  engine_(uml::platform::apple::probed_host_capabilities(),
			  jit::FexEngineOptions{.generation_live = &generation_live})
	{
		require(context_ != nullptr);
		memory_.set_generation(generation);
	}

	~Space()
	{
		if (owner_)
			(void)darwin_uml_static_vm_destroy(generation_);
		darwin_uml_static_vm_execution_context_destroy(context_);
	}

	Space(const Space &) = delete;
	Space &operator=(const Space &) = delete;

	void map(std::uint64_t address, ScratchBacking &backing,
		unsigned long first_page, int protection, int guest_code)
	{
		require(darwin_uml_static_vm_map_generation(generation_,
			reinterpret_cast<void *>(address), backing.descriptor(),
			first_page * page, page, protection, guest_code, 1) == 0,
			"a mapping was refused");
	}

	void unmap(std::uint64_t address)
	{
		require(darwin_uml_static_vm_unmap_generation(generation_,
			reinterpret_cast<void *>(address), page) == 0);
	}

	/* What the runtime hands the engine before each run. */
	void sync(const darwin_uml_static_sequences &sequences)
	{
		const auto mode = x86_64::ExecutionMode::long64;
		engine_.settle_stuck_participants(generation_, mode);
		jit::AddressSpaceChanges changes{};
		changes.mapping_sequence = sequences.mapping_changes;
		changes.code_sequence = sequences.code_changes;
		std::uint64_t mapping_seen = 0;
		std::uint64_t code_seen = 0;
		if (!engine_.consumed_sequences(generation_, mode, mapping_seen,
			code_seen)) {
			engine_.apply_changes(generation_, mode, changes, false);
			return;
		}
		if (mapping_seen == sequences.mapping_changes &&
			code_seen == sequences.code_changes)
			return;
		std::array<darwin_uml_static_mapping_change, 256> mapped;
		std::array<jit::GuestRange, 256> mapping_ranges;
		if (mapping_seen != sequences.mapping_changes) {
			std::uint64_t current = sequences.mapping_changes;
			const int count = darwin_uml_static_vm_mapping_changes(generation_,
				mapping_seen, mapped.data(), mapped.size(), &current);
			if (count < 0) {
				changes.all_mappings = true;
			} else {
				for (int index = 0; index < count; ++index)
					mapping_ranges[index] = {mapped[index].address,
						mapped[index].length};
				changes.mapping_ranges = {mapping_ranges.data(),
					static_cast<std::size_t>(count)};
				changes.mapping_sequence = current;
			}
		}
		std::array<darwin_uml_static_code_change, 256> coded;
		std::array<jit::GuestRange, 256> code_ranges;
		if (code_seen != sequences.code_changes) {
			std::uint64_t current = sequences.code_changes;
			const int count =
				darwin_uml_static_vm_execution_context_code_change_ranges(
					context_, code_seen, coded.data(), coded.size(), &current);
			if (count < 0) {
				changes.all_code = true;
			} else {
				for (int index = 0; index < count; ++index)
					code_ranges[index] = {coded[index].address,
						coded[index].length};
				changes.code_ranges = {code_ranges.data(),
					static_cast<std::size_t>(count)};
				changes.code_sequence = current;
			}
		}
		engine_.apply_changes(generation_, mode, changes, false);
	}

	[[nodiscard]] jit::JitRunResult run(x86_64::CpuState &cpu)
	{
		require(darwin_uml_static_vm_execution_context_begin(context_,
			cpu.rip) == 0, "the execution context was not entered");
		const auto sequences =
			darwin_uml_static_vm_execution_context_sequences(context_);
		memory_.prepare(generation_, sequences.mapping_changes);
		sync(sequences);
		uml::cpu::InterruptToken interrupt{};
		const auto result = engine_.run(cpu, memory_, {
			.generation = generation_,
			.instruction_budget = 1000,
			.execution_context = context_,
			.interrupt = &interrupt,
			.orders_stores = false,
		});
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
		return result;
	}

	/* A fresh CPU at `address` with RBX and RDX set. */
	[[nodiscard]] static x86_64::CpuState cpu_at(std::uint64_t address,
		std::uint64_t rbx = 0, std::uint64_t rdx = 0)
	{
		x86_64::CpuState cpu{};
		cpu.segment_selectors = {0, 0x33, 0x2b, 0, 0, 0};
		(void)x86_64::enter_segments(cpu);
		cpu.rip = address;
		cpu.rflags = 0x202;
		cpu.gpr(x86_64::Register::rbx) = rbx;
		cpu.gpr(x86_64::Register::rdx) = rdx;
		return cpu;
	}

	/* RAX at the block's system call, running on after the exits that leave
	 * for a store into the block. */
	[[nodiscard]] std::uint64_t to_system_call(std::uint64_t address,
		std::uint64_t rbx = 0, std::uint64_t rdx = 0)
	{
		auto cpu = cpu_at(address, rbx, rdx);
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			const auto result = run(cpu);
			if (result.execution.reason == uml::cpu::ExitReason::system_call)
				return cpu.gpr(x86_64::Register::rax);
		}
		require(false, "the block never reached its system call");
		return 0;
	}

	[[nodiscard]] jit::FexEngine &engine() noexcept { return engine_; }
	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

private:
	unsigned long generation_;
	bool owner_;
	void *context_;
	StaticGuestMemory memory_;
	jit::FexEngine engine_;
};

void put(ScratchBacking &backing, std::size_t offset,
	std::initializer_list<std::uint8_t> bytes)
{
	std::vector<std::byte> data;
	for (const auto byte : bytes)
		data.push_back(static_cast<std::byte>(byte));
	backing.put(offset, data);
}

/* mov eax, imm32 ; syscall */
void load_return(ScratchBacking &backing, std::uint8_t value)
{
	put(backing, 0, {0xb8, value, 0x00, 0x00, 0x00, 0x0f, 0x05});
}

/* mov [rdx], bl ; syscall */
void load_writer(ScratchBacking &backing)
{
	put(backing, 0, {0x88, 0x1a, 0x0f, 0x05});
}

[[nodiscard]] bool held_at(
	const std::shared_ptr<uml::memory::MappedBacking> &backing,
	unsigned long generation, std::uint64_t address)
{
	std::vector<WriterLocation> holders;
	FrameAccounting::holders_for_testing(*backing, 0, holders);
	const auto state = uml::memory::testing::generation_state(generation);
	if (state == nullptr)
		return false;
	for (const auto &holder : holders)
		if (holder.page == address && holder.alias == state->accounting.alias)
			return true;
	return false;
}

/* A row leaves the counters that must stay 0 where it found them: no state
 * was destroyed with its locations still in the accounting, no holder
 * outlived its state, and no removal found its location missing. */
struct Clean final {
	FrameAccounting::Diagnostics start{FrameAccounting::diagnostics()};
	/* The accounting errors a row causes on purpose. */
	std::uint64_t errors{};

	~Clean()
	{
		const auto now = FrameAccounting::diagnostics();
		require(now.nonempty_state_destructions ==
			start.nonempty_state_destructions,
			"a state was destroyed without its walk");
		require(now.holder_leaks == start.holder_leaks, "a holder leaked");
		require(now.errors == start.errors + errors,
			"an accounting error was counted");
	}
};

void test_store()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1000'0000;
	constexpr std::uint64_t alias = 0x2000'0000;
	constexpr std::uint64_t writer = 0x3000'0000;
	ScratchBacking frame{"uml-x86-jit-alias-XXXXXX", page};
	ScratchBacking code{"uml-x86-jit-alias-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9201};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	require(space.to_system_call(executable) == 1);
	require(held_at(backing, space.generation(), executable),
		"the unit that compiled X did not register its frame");
	(void)space.to_system_call(writer, 2, alias + 1);
	require(space.to_system_call(executable) == 2,
		"a store through another address did not reach the compiled code");
	require(space.engine().diagnostics().registration_failures == 0);
}

/* The kernel stores into another process's page (ptrace, /proc/pid/mem,
 * process_vm_writev) and names that process's generation: the code another
 * generation compiled runs the new bytes, whatever the target's own view of
 * the frame is. The target maps it as data, maps nothing at the page, or
 * maps it as code (the control). */
void test_a_remote_kernel_write_reaches_the_compiled_code()
{
	const Clean clean;
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	constexpr std::uint64_t executable = 0x1a00'0000;
	constexpr std::uint64_t target_address = 0x2a00'0000;
	enum class View { data, absent, code };
	unsigned long generation = 9250;
	/* Every view is run, so one run names each that fails. */
	unsigned failed = 0;
	for (const auto view : {View::data, View::absent, View::code}) {
		const char *name = view == View::data ? "data view" :
			view == View::absent ? "page the target does not map" :
			"code view";
		ScratchBacking frame{"uml-x86-jit-alias-remote-XXXXXX", page};
		load_return(frame, 1);
		Space reader{generation++};
		Space target{generation++};
		reader.map(executable, frame, 0, rx, 1);
		if (view == View::data)
			target.map(target_address, frame, 0, rw, 0);
		else if (view == View::code)
			target.map(target_address, frame, 0, rx, 1);
		require(reader.to_system_call(executable) == 1);
		load_return(frame, 2);
		require(darwin_uml_static_vm_invalidate_code_generation(
			target.generation(), target_address, page) == 0);
		if (reader.to_system_call(executable) != 2) {
			std::fprintf(stderr, "a remote write into a %s did not reach "
				"the compiled code\n", name);
			++failed;
		}
	}
	require(failed == 0, "a remote kernel write left compiled code running");
}

/* The kernel pins a frame to write it and names it. Between the pin and the
 * write the target replaces what is mapped at the address with another frame,
 * so the call meets a page that is not the frame written, and the code compiled
 * from the frame written must still hear of the write. The reader runs once
 * more after the replacement, so no publication of the replacement can account
 * for what it hears. */
void test_a_remote_write_after_the_target_remapped()
{
	const Clean clean;
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	constexpr std::uint64_t executable = 0x1b00'0000;
	constexpr std::uint64_t target_address = 0x2b00'0000;
	enum class Replacement { data, code };
	unsigned long generation = 9350;
	unsigned failed = 0;
	for (const auto replacement : {Replacement::data, Replacement::code}) {
		const char *name = replacement == Replacement::data ?
			"a data view of another frame" : "a code view of another frame";
		ScratchBacking pinned{"uml-x86-jit-alias-remap-p-XXXXXX", page};
		ScratchBacking other{"uml-x86-jit-alias-remap-o-XXXXXX", page};
		load_return(pinned, 1);
		load_return(other, 3);
		Space reader{generation++};
		Space target{generation++};
		reader.map(executable, pinned, 0, rx, 1);
		target.map(target_address, pinned, 0, rw, 0);
		require(reader.to_system_call(executable) == 1);
		/* The pin, then the replacement. */
		target.unmap(target_address);
		if (replacement == Replacement::data)
			target.map(target_address, other, 0, rw, 0);
		else
			target.map(target_address, other, 0, rx, 1);
		require(reader.to_system_call(executable) == 1);
		/* The store reaches the pinned frame, which the call names. */
		load_return(pinned, 2);
		require(darwin_uml_static_vm_invalidate_code_frame(
			target.generation(), target_address, page, pinned.descriptor(),
			0) == 0);
		if (reader.to_system_call(executable) != 2) {
			std::fprintf(stderr, "a remote write after the target replaced "
				"its mapping with %s did not reach the compiled code\n",
				name);
			++failed;
		}
	}
	require(failed == 0, "a remote write after a remap left compiled code "
		"running");
}

void test_running_block()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1100'0000;
	constexpr std::uint64_t alias = 0x2100'0000;
	ScratchBacking frame{"uml-x86-jit-running-XXXXXX", page};
	/* mov [rdx], bl ; mov eax, imm32 ; syscall */
	put(frame, 0, {0x88, 0x1a, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	Space space{9202};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	require(space.to_system_call(executable, 1, alias + 3) == 1);
	require(space.to_system_call(executable, 2, alias + 3) == 2,
		"the running unit ran its old bytes after a store through W");
}

void test_decoder_range()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1200'0000;
	ScratchBacking frame{"uml-x86-jit-range-XXXXXX", page};
	ScratchBacking other{"uml-x86-jit-range-other-XXXXXX", page};
	load_return(frame, 3);
	load_return(other, 4);
	Space space{9203};
	space.map(executable, frame, 0, rx, 1);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	require(space.to_system_call(executable) == 3);
	const auto first = space.engine().diagnostics().frame_registrations;
	require(first >= 1 && held_at(backing, space.generation(), executable));
	/* Compiled again with nothing changed, it asks no more. */
	require(space.to_system_call(executable) == 3);
	require(space.engine().diagnostics().frame_registrations == first,
		"an unchanged page registered its frame again");
	/* The page goes and comes back as another frame: its holder is removed,
	 * and the next compile asks for the page again and registers the new
	 * frame before it reads a byte of it. A decoder range that survived the
	 * unmap would run the old bytes. */
	space.unmap(executable);
	require(!held_at(backing, space.generation(), executable),
		"the unmap left the page's holder");
	space.map(executable, other, 0, rx, 1);
	const auto replacement =
		uml::memory::testing::mapped_backing(other.descriptor());
	require(replacement != nullptr);
	require(!held_at(replacement, space.generation(), executable),
		"a mapping registered a frame nobody compiled");
	require(space.to_system_call(executable) == 4,
		"a decoder range survived the page being replaced");
	require(space.engine().diagnostics().frame_registrations == first + 1,
		"a page mapped back was compiled without registering its frame");
	require(held_at(replacement, space.generation(), executable),
		"the replacement frame was compiled without a holder");
	require(!held_at(backing, space.generation(), executable),
		"the old frame was registered again");
}

/* E12: an instruction that spans the end of a page is decoded from two
 * frames, and the unit registers both before the decoder reads a byte of
 * them: a store through an alias of the second frame reaches the compiled
 * unit. The instruction is `mov eax, imm32` with its last two immediate bytes
 * on the second page. */
void test_decoder_across_two_pages()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1800'0000;
	constexpr std::uint64_t alias = 0x2800'0000;
	constexpr std::uint64_t writer = 0x3800'0000;
	ScratchBacking first{"uml-x86-jit-span-first-XXXXXX", page};
	ScratchBacking second{"uml-x86-jit-span-second-XXXXXX", page};
	ScratchBacking code{"uml-x86-jit-span-code-XXXXXX", page};
	/* 0xffd: mov eax, imm32 (b8 01 00 | 00 00) ; syscall */
	put(first, 0xffd, {0xb8, 0x01, 0x00});
	put(second, 0, {0x00, 0x00, 0x0f, 0x05});
	load_writer(code);
	Space space{9210};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, first, 0, rx, 1);
	space.map(executable + page, second, 0, rx, 1);
	space.map(alias, second, 0, rw, 0);
	const auto first_backing =
		uml::memory::testing::mapped_backing(first.descriptor());
	const auto second_backing =
		uml::memory::testing::mapped_backing(second.descriptor());
	require(first_backing != nullptr && second_backing != nullptr);
	require(space.to_system_call(executable + 0xffd) == 1);
	require(held_at(first_backing, space.generation(), executable),
		"the first page of a spanning instruction was decoded unregistered");
	require(held_at(second_backing, space.generation(), executable + page),
		"the second page of a spanning instruction was decoded unregistered");
	/* The byte at the start of the second page becomes the immediate's
	 * third byte. */
	(void)space.to_system_call(writer, 2, alias);
	require(space.to_system_call(executable + 0xffd) == 0x2'0001,
		"a store through the second frame's alias did not reach the unit");
	require(space.engine().diagnostics().registration_failures == 0);
}

/* Holds each registering thread at the point it records a holder, until the
 * other has reached it too, so that both are inside the registration (and so
 * inside the engine) when either one retires the writers' translations. */
std::atomic<unsigned> registering{0};
thread_local bool hold_at_registration = false;

void wait_for_the_other_registration(
	uml::memory::testing::AllocationSite site) noexcept
{
	using namespace std::chrono_literals;
	if (site != uml::memory::testing::AllocationSite::holders ||
	    !hold_at_registration)
		return;
	hold_at_registration = false;
	registering.fetch_add(1, std::memory_order_acq_rel);
	const auto deadline = std::chrono::steady_clock::now() + 3s;
	while (registering.load(std::memory_order_acquire) < 2 &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
}

/* NF-1: two vCPUs of one generation compile frames that each owe a writer's
 * write translation, at the same moment. A registration waits for the
 * participants inside the engine, so a vCPU that waits with its own epoch odd
 * is waited for by the other, and both come back at the deadline with the
 * page unsettled. Both must settle at once and neither record a stuck
 * participant. */
void test_two_spaces_registering_owed_frames()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	using namespace std::chrono_literals;
	constexpr unsigned long generation = 9211;
	constexpr std::uint64_t executable_a = 0x1900'0000;
	constexpr std::uint64_t alias_a = 0x2900'0000;
	constexpr std::uint64_t executable_b = 0x1a00'0000;
	constexpr std::uint64_t alias_b = 0x2a00'0000;
	ScratchBacking frame_a{"uml-x86-jit-twin-a-XXXXXX", page};
	ScratchBacking frame_b{"uml-x86-jit-twin-b-XXXXXX", page};
	load_return(frame_a, 1);
	load_return(frame_b, 2);
	Space first{generation};
	Space second{generation, false};
	first.map(executable_a, frame_a, 0, rx, 1);
	first.map(alias_a, frame_a, 0, rw, 0);
	first.map(executable_b, frame_b, 0, rx, 1);
	first.map(alias_b, frame_b, 0, rw, 0);

	registering.store(0);
	testing::set_allocation_observer(&wait_for_the_other_registration);
	std::uint64_t result_a = 0;
	std::uint64_t result_b = 0;
	const auto start = std::chrono::steady_clock::now();
	std::thread thread_a{[&] {
		hold_at_registration = true;
		result_a = first.to_system_call(executable_a);
	}};
	std::thread thread_b{[&] {
		hold_at_registration = true;
		result_b = second.to_system_call(executable_b);
	}};
	thread_a.join();
	thread_b.join();
	const auto elapsed = std::chrono::steady_clock::now() - start;
	testing::set_allocation_observer(nullptr);
	require(registering.load() == 2, "the two registrations did not overlap");
	require(result_a == 1 && result_b == 2);
	require(elapsed < 600ms,
		"two vCPUs registering owed frames waited for each other");
	for (auto *space : {&first, &second}) {
		const auto &diagnostics = space->engine().diagnostics();
		require(diagnostics.escalation_invalidations == 0,
			"a registration left a participant counted stuck");
		require(diagnostics.registration_failures == 0);
	}
}

/* The retire-all sentinel at the JIT's own callback: a frame whose writers
 * are not known is registered with every write translation of every
 * participant retired, and the JIT retires its own through the callback
 * `retire_all_and_wait` calls with `all_pages`. W's store entry, filled before
 * the frame had a holder (so that it publishes nothing), is the one entry
 * that can only go that way: a store through W after the registration must
 * reach the unit at X. */
void test_retire_all_reaches_the_jit_callback()
{
	Clean clean;
	constexpr std::uint64_t executable = 0x1b00'0000;
	/* A different TLB index from the unit's, so that only the retire-all can
	 * retire W's entry. */
	constexpr std::uint64_t alias = 0x2b00'5000;
	constexpr std::uint64_t writer = 0x3b00'0000;
	ScratchBacking frame{"uml-x86-jit-all-XXXXXX", page};
	ScratchBacking code{"uml-x86-jit-all-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9212};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	/* W's entry is filled while the frame has no holder. */
	(void)space.to_system_call(writer, 1, alias + 1);
	/* The writers of the frame are no longer known (an accounting error the
	 * row causes). */
	clean.errors = 1;
	FrameAccounting::remove_writer(*backing, 0, {777'777, alias});

	require(space.to_system_call(executable) == 1);
	require(held_at(backing, space.generation(), executable));
	(void)space.to_system_call(writer, 2, alias + 1);
	require(space.to_system_call(executable) == 2,
		"the JIT did not retire its write translations for the retire-all "
		"sentinel");
}

/* E7: a store through W into a unit that other units link to reaches the
 * link: the units W -> A -> B run within one dispatch chain, and B's frame
 * is rewritten through W by the first of them. */
void test_links_follow_alias_stores()
{
	const Clean clean;
	constexpr std::uint64_t a = 0x1600'0000;
	constexpr std::uint64_t b = a + 0x1000;
	constexpr std::uint64_t w = a + 0x2000;
	constexpr std::uint64_t alias = 0x2600'0000;
	ScratchBacking frame_a{"uml-x86-jit-link-a-XXXXXX", page};
	ScratchBacking frame_b{"uml-x86-jit-link-b-XXXXXX", page};
	ScratchBacking frame_w{"uml-x86-jit-link-w-XXXXXX", page};
	/* jmp B */
	put(frame_a, 0, {0xe9, 0xfb, 0x0f, 0x00, 0x00});
	load_return(frame_b, 1);
	/* mov [rdx], bl ; jmp A */
	put(frame_w, 0, {0x88, 0x1a, 0xe9, 0xf9, 0xdf, 0xff, 0xff});
	Space space{9206};
	space.map(w, frame_w, 0, rx, 1);
	space.map(a, frame_a, 0, rx, 1);
	space.map(b, frame_b, 0, rx, 1);
	space.map(alias, frame_b, 0, rw, 0);
	for (unsigned round = 0; round < 4; ++round)
		require(space.to_system_call(w, 1, alias + 1) == 1);
	require(space.to_system_call(w, 2, alias + 1) == 2,
		"a store through W did not reach a unit another unit links to");
	/* Past B's code: the units keep running as they did. (FEXCore asks for a
	 * page again after any range invalidation, so the registrations do not
	 * say whether a unit was compiled again; the gadget row counts those.) */
	require(space.to_system_call(w, 9, alias + 0x400) == 2);
}

/* E7, the case the explicit cooling of the writer's own units catches: one
 * dispatch, with no hand-over between, stores through W into a unit it has
 * already compiled and linked to, and branches into it again. The unit P
 * stores CL through W over the immediate of X's `mov eax, imm32`, then jumps
 * to X, which counts and jumps back once:
 *
 *	P:	88 0a			mov %cl, (%rdx)
 *		e9 rel32		jmp X
 *	X:	b8 00 00 00 00		mov $0, %eax	(immediate patched)
 *		ff c1			inc %ecx
 *		83 f9 02		cmp $2, %ecx
 *		0f 8c rel32		jl P
 *		0f 05			syscall
 *
 * The second round stores 1, and the second run of X must see it. The
 * ring record the store makes is applied at the next hand-over, which this
 * dispatch does not reach. */
void test_a_store_then_a_branch_into_a_linked_unit_in_one_dispatch()
{
	const Clean clean;
	constexpr std::uint64_t p = 0x3c00'0000;
	constexpr std::uint64_t x = 0x1c00'0000;
	constexpr std::uint64_t alias = 0x2c00'0000;
	ScratchBacking frame_p{"uml-x86-jit-loop-p-XXXXXX", page};
	ScratchBacking frame_x{"uml-x86-jit-loop-x-XXXXXX", page};
	const auto rel32 = [](std::uint64_t from, std::uint64_t to) {
		const auto delta = static_cast<std::int32_t>(to - from);
		return std::array<std::uint8_t, 4>{
			static_cast<std::uint8_t>(delta), static_cast<std::uint8_t>(delta >> 8),
			static_cast<std::uint8_t>(delta >> 16), static_cast<std::uint8_t>(delta >> 24)};
	};
	const auto to_x = rel32(p + 7, x);
	put(frame_p, 0, {0x88, 0x0a, 0xe9, to_x[0], to_x[1], to_x[2], to_x[3]});
	const auto to_p = rel32(x + 16, p);
	put(frame_x, 0, {0xb8, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc1, 0x83, 0xf9, 0x02,
		0x0f, 0x8c, to_p[0], to_p[1], to_p[2], to_p[3], 0x0f, 0x05});
	Space space{9213};
	space.map(p, frame_p, 0, rx, 1);
	space.map(x, frame_x, 0, rx, 1);
	space.map(alias, frame_x, 0, rw, 0);
	auto cpu = Space::cpu_at(p, 0, alias + 1);
	const auto result = space.run(cpu);
	require(result.execution.reason == uml::cpu::ExitReason::system_call,
		"the loop did not reach its system call in one dispatch");
	require(cpu.gpr(x86_64::Register::rax) == 1,
		"a branch into a linked unit ran the code a store through W had "
		"replaced in the same dispatch");
}

/* E4j: a participant of another generation stays inside the engine while
 * this space compiles a frame it writes: the space drops the frame's code at
 * each hand-over until the participant has left, and then once more. */
void test_a_stuck_writer_of_another_generation_drops_the_code()
{
	using namespace std::chrono_literals;
	const Clean clean;
	constexpr std::uint64_t executable = 0x1700'0000;
	constexpr std::uint64_t alias = 0x2700'0000;
	constexpr unsigned long writer_generation = 9402;
	ScratchBacking frame{"uml-x86-jit-stuck-XXXXXX", page};
	load_return(frame, 1);
	/* Another address space writes the frame through its own mapping, and a
	 * vCPU of it stays inside the engine. */
	Space other{writer_generation};
	other.map(alias, frame, 0, rw, 0);
	const auto registry = uml::cpu::x86_64::CodePageRegistry::acquire(
		writer_generation, {.slice = 10ms, .slices = 2});
	uml::cpu::x86_64::CodePageRegistry::Participant stuck;
	registry->join(stuck);
	stuck.engine_epoch.store(1, std::memory_order_release);

	Space space{9401};
	space.map(executable, frame, 0, rx, 1);
	require(space.to_system_call(executable) == 1);
	const auto compiled = space.engine().diagnostics().escalation_invalidations;
	require(space.to_system_call(executable) == 1);
	require(space.engine().diagnostics().escalation_invalidations == compiled + 1,
		"a hand-over did not drop the code that depends on a stuck participant");
	/* The participant leaves the engine: the next hand-over drops the code
	 * once more, and the record with it. */
	stuck.engine_epoch.store(2, std::memory_order_release);
	require(space.to_system_call(executable) == 1);
	require(space.engine().diagnostics().escalation_invalidations == compiled + 2,
		"the hand-over after the participant left did not drop the code once");
	require(space.to_system_call(executable) == 1);
	require(space.engine().diagnostics().escalation_invalidations == compiled + 2,
		"the record of a participant that left was kept");
	registry->leave(stuck);
}

/* E5: a space whose registry could not be made still registers the frames it
 * compiles from, so that a store through another mapping of one reaches its
 * code. The space refuses to compile a page until a holder stands: the first
 * registration is made to fail, and the dispatch reports a fetch fault with
 * nothing retired and nothing run; only the next one, whose registration
 * settles, compiles. */
void test_without_a_registry()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1500'0000;
	constexpr std::uint64_t alias = 0x2500'0000;
	constexpr std::uint64_t writer = 0x3500'0000;
	constexpr std::size_t page_counts = std::size_t{1} << 18;
	ScratchBacking frame{"uml-x86-jit-bare-XXXXXX", page};
	ScratchBacking code{"uml-x86-jit-bare-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9205};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	namespace testing = uml::memory::testing;
	failing_array_size.store(page_counts);
	auto cpu = Space::cpu_at(executable);
	testing::fail_allocation_at(testing::AllocationSite::holders, 1);
	const auto refused = space.run(cpu);
	testing::clear_allocation_failure();
	require(failing_array_size.load() == 0 &&
		uml::cpu::x86_64::CodePageRegistry::find(space.generation()) == nullptr,
		"the registry of the space was made");
	require(refused.execution.reason == uml::cpu::ExitReason::memory_fault &&
		refused.execution.retired_instructions == 0 && cpu.rip == executable,
		"a space with no registry compiled a page no holder stood for");
	require(space.engine().diagnostics().registration_failures == 1);
	require(space.to_system_call(executable) == 1);
	require(held_at(backing, space.generation(), executable),
		"a space without a registry compiled from a frame it had not registered");
	(void)space.to_system_call(writer, 2, alias + 1);
	require(space.to_system_call(executable) == 2,
		"a store through another address did not reach a space with no registry");
	require(space.engine().diagnostics().registration_failures == 1);
}

/* The writer's side of E5: a space that could not make its registry is not
 * named by a registration that retires write translations, so it cannot be
 * retired from a holder's registration. Every store it makes publishes, the
 * stores to a non-executable page included: W's store, made before any
 * holder of the frame exists, must still reach the code that X, another
 * generation, compiles afterwards. */
void test_a_writer_without_a_registry()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1c00'0000;
	constexpr std::uint64_t alias = 0x2c00'0000;
	constexpr std::uint64_t writer = 0x3c00'0000;
	constexpr std::size_t page_counts = std::size_t{1} << 18;
	ScratchBacking frame{"uml-x86-jit-bare-writer-XXXXXX", page};
	ScratchBacking code{"uml-x86-jit-bare-writer-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space w{9217};
	Space x{9218};
	w.map(writer, code, 0, rx, 1);
	w.map(alias, frame, 0, rw, 0);
	x.map(executable, frame, 0, rx, 1);
	failing_array_size.store(page_counts);
	/* W's first store, before any holder of the frame exists. */
	(void)w.to_system_call(writer, 1, alias + 1);
	require(failing_array_size.load() == 0 &&
		uml::cpu::x86_64::CodePageRegistry::find(w.generation()) == nullptr,
		"the registry of the writer was made");
	require(x.to_system_call(executable) == 1);
	/* W stores again, through whatever its first store left. */
	(void)w.to_system_call(writer, 2, alias + 1);
	require(x.to_system_call(executable) == 2,
		"a store of a space with no registry did not reach code compiled "
		"after its first store");
}

/* Fails every registration that reaches the holder list, as memory that
 * stays exhausted would. */
void fail_every_holder_list(uml::memory::testing::AllocationSite site) noexcept
{
	if (site == uml::memory::testing::AllocationSite::holders)
		uml::memory::testing::fail_next_allocation(site);
}

void test_registration_failure()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t executable = 0x1300'0000;
	ScratchBacking frame{"uml-x86-jit-failure-XXXXXX", page};
	load_return(frame, 5);
	Space space{9204};
	space.map(executable, frame, 0, rx, 1);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	/* One failure: the fetch fault the kernel resolves, nothing retired. */
	auto cpu = Space::cpu_at(executable);
	testing::fail_allocation_at(testing::AllocationSite::holders, 1);
	auto result = space.run(cpu);
	testing::clear_allocation_failure();
	require(result.execution.reason == uml::cpu::ExitReason::memory_fault &&
		result.execution.fault.access == uml::memory::MemoryAccess::execute,
		"a failed registration did not report a fetch fault");
	require(result.execution.retired_instructions == 0 &&
		cpu.rip == executable, "a failed registration retired an instruction");
	/* FEXCore asks for the page again as it reports the fault, and that
	 * query registers it: a holder without code is no harm. */
	require(space.engine().diagnostics().registration_failures == 1);
	/* The kernel resumes the instruction; the registration now succeeds. */
	require(space.to_system_call(executable) == 5);
	require(space.engine().diagnostics().consecutive_registration_failures == 0,
		"a registration that succeeded left the failures counted");
	require(held_at(backing, space.generation(), executable));

	/* Failures in a row end the space. */
	ScratchBacking other{"uml-x86-jit-limit-XXXXXX", page};
	load_return(other, 6);
	constexpr std::uint64_t unlucky = 0x1400'0000;
	space.map(unlucky, other, 0, rx, 1);
	auto again = Space::cpu_at(unlucky);
	bool fatal = false;
	testing::set_allocation_observer(&fail_every_holder_list);
	testing::fail_next_allocation(testing::AllocationSite::holders);
	for (unsigned attempt = 0; attempt < jit::FexEngine::registration_failure_limit + 2;
		++attempt) {
		const auto outcome = space.run(again);
		if (outcome.execution.reason == uml::cpu::ExitReason::fatal) {
			fatal = true;
			break;
		}
		require(outcome.execution.reason == uml::cpu::ExitReason::memory_fault);
	}
	testing::set_allocation_observer(nullptr);
	testing::clear_allocation_failure();
	require(fatal, "the failures in a row did not end the space");
	require(space.engine().diagnostics().failed_spaces == 1);
	require(space.engine().diagnostics().registration_failures ==
		1 + jit::FexEngine::registration_failure_limit);
}

} // namespace

/* The next instruction sees what a helper-shaped store wrote through W: an
 * x87 store of ten bytes, a string store, ENTER's push, SLDT, and a scalar and
 * a vector store across the end of W's page. Each writes over the `mov eax,
 * imm32 ; syscall` that follows it in the unit, at offset 64 of the frame
 * (the last four bytes of the page for the stores across it), and the system
 * call returns the value the store wrote. The unit leaves before it runs the
 * overwritten bytes, whichever form stored; the extended-state and
 * environment saves write structured bytes over everything after them, so
 * they cannot return a value and are not rows here. */
struct StoreForm final {
	const char *name;
	std::vector<std::uint8_t> code;
	std::size_t at;
	std::uint64_t expected;
	void (*prepare)(x86_64::CpuState &, std::uint64_t alias);
};

void prepare_x87(x86_64::CpuState &cpu, std::uint64_t alias)
{
	cpu.gpr(x86_64::Register::rdx) = alias + 64;
	cpu.x87_top = 0;
	cpu.x87_tag = 0xfffc;
	cpu.x87[0].significand = 0x0005'0f00'0000'02b8ULL;
	cpu.x87[0].sign_exponent = 0;
}

void prepare_string(x86_64::CpuState &cpu, std::uint64_t alias)
{
	cpu.gpr(x86_64::Register::rdi) = alias + 65;
	cpu.gpr(x86_64::Register::rcx) = 1;
	cpu.gpr(x86_64::Register::rax) = 2;
}

void prepare_enter(x86_64::CpuState &cpu, std::uint64_t alias)
{
	cpu.gpr(x86_64::Register::rsp) = alias + 64 + 8;
	cpu.gpr(x86_64::Register::rbp) = 0x0005'0f00'0000'02b8ULL;
}

void prepare_sldt(x86_64::CpuState &cpu, std::uint64_t alias)
{
	cpu.gpr(x86_64::Register::rdx) = alias + 65;
}

void prepare_across(x86_64::CpuState &cpu, std::uint64_t alias)
{
	cpu.gpr(x86_64::Register::rdx) = alias + 0xffc;
	cpu.gpr(x86_64::Register::rbx) = 0x050f'02b0ULL;
	const std::uint32_t changed = 0x050f'02b0U;
	std::memcpy(cpu.vectors[0].data(), &changed, sizeof(changed));
}

void test_helper_stores_reach_the_next_instruction()
{
	const Clean clean;
	const std::vector<StoreForm> forms{
		{"fstp tbyte", {0xdb, 0x3a}, 64, 2, prepare_x87},
		{"rep stosb", {0xf3, 0xaa}, 64, 2, prepare_string},
		{"enter", {0xc8, 0x08, 0x00, 0x00}, 64, 2, prepare_enter},
		{"sldt", {0x0f, 0x00, 0x02}, 64, 0, prepare_sldt},
		{"mov across a page", {0x48, 0x89, 0x1a}, 0xffc, 2, prepare_across},
		{"movups across a page", {0x0f, 0x11, 0x02}, 0xffc, 2, prepare_across},
	};
	unsigned long generation = 9340;
	unsigned failures = 0;
	for (const auto &form : forms) {
		constexpr std::uint64_t executable = 0x1500'0000;
		constexpr std::uint64_t alias = 0x2500'0000;
		ScratchBacking frame{"uml-x86-jit-helper-XXXXXX", page};
		ScratchBacking beyond{"uml-x86-jit-beyond-XXXXXX", page};
		std::vector<std::byte> bytes(page, std::byte{0x90});
		const std::size_t start = form.at == 64 ? 0 : 0xff0;
		std::size_t at = start;
		for (const auto byte : form.code)
			bytes[at++] = static_cast<std::byte>(byte);
		if (form.at == 64) {
			at = 64;
			for (const auto byte : {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05})
				bytes[at++] = static_cast<std::byte>(byte);
		} else {
			at = 0xffc;
			for (const auto byte : {0xb0, 0x01, 0x0f, 0x05})
				bytes[at++] = static_cast<std::byte>(byte);
		}
		frame.put(0, bytes);
		Space space{generation++};
		space.map(executable, frame, 0, rx, 1);
		space.map(alias, frame, 0, rw, 0);
		space.map(alias + page, beyond, 0, rw, 0);
		auto cpu = Space::cpu_at(executable + start);
		form.prepare(cpu, alias);
		std::uint64_t result = ~std::uint64_t{0};
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			if (space.run(cpu).execution.reason ==
				uml::cpu::ExitReason::system_call) {
				result = cpu.gpr(x86_64::Register::rax) & 0xff;
				break;
			}
		}
		if (result != form.expected) {
			std::fprintf(stderr, "%s: the unit returned %llu\n", form.name,
				static_cast<unsigned long long>(result));
			++failures;
		}
	}
	require(failures == 0,
		"the instruction after a helper-shaped store ran its old bytes");
}

int main(int argc, char **argv)
{
	/* A row's name as the first argument runs that row alone: "writer",
	 * "remote". */
	if (argc > 1 && std::strcmp(argv[1], "writer") == 0) {
		test_a_writer_without_a_registry();
		std::puts("writer without a registry ok");
		return EXIT_SUCCESS;
	}
	if (argc > 1 && std::strcmp(argv[1], "remote") == 0) {
		test_a_remote_kernel_write_reaches_the_compiled_code();
		std::puts("remote kernel write ok");
		return EXIT_SUCCESS;
	}
	if (argc > 1 && std::strcmp(argv[1], "remapped") == 0) {
		test_a_remote_write_after_the_target_remapped();
		std::puts("remote write after a remap ok");
		return EXIT_SUCCESS;
	}
	const auto start = FrameAccounting::diagnostics();
	test_store();
	std::puts("store ok");
	test_running_block();
	std::puts("running ok");
	test_decoder_range();
	std::puts("decoder ok");
	test_decoder_across_two_pages();
	std::puts("decoder spanning ok");
	test_two_spaces_registering_owed_frames();
	std::puts("twin spaces ok");
	test_retire_all_reaches_the_jit_callback();
	std::puts("retire all ok");
	test_links_follow_alias_stores();
	std::puts("links ok");
	test_a_store_then_a_branch_into_a_linked_unit_in_one_dispatch();
	std::puts("one dispatch ok");
	test_a_stuck_writer_of_another_generation_drops_the_code();
	std::puts("stuck ok");
	test_registration_failure();
	std::puts("failure ok");
	test_without_a_registry();
	std::puts("no registry ok");
	test_a_writer_without_a_registry();
	std::puts("writer without a registry ok");
	test_helper_stores_reach_the_next_instruction();
	std::puts("helper stores ok");
	test_a_remote_kernel_write_reaches_the_compiled_code();
	std::puts("remote kernel write ok");
	test_a_remote_write_after_the_target_remapped();
	std::puts("remote write after a remap ok");
	const auto now = FrameAccounting::diagnostics();
	require(now.holder_leaks == start.holder_leaks, "a holder leaked");
	require(now.unquiesced_holder_removals == start.unquiesced_holder_removals);
	require(now.unmarked_holder_removals == start.unmarked_holder_removals);
	/* The retire-all row makes one on purpose. */
	require(now.errors == start.errors + 1, "an accounting error was counted");
	std::puts("x86-64 JIT alias tests passed");
	return EXIT_SUCCESS;
}
