// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine over a frame that more than one address maps.
 *
 * A frame is translated at one address (X) and written through another (W),
 * or at an address of another address space. The rows run the engine over
 * the real static MMU and read what the guest can see: the value a block
 * returns after the store, the tag a write entry carries, and the counters
 * the engine and the MMU keep for what must not happen. Each row names the
 * behaviour it holds; the counters that must stay 0 are read at the end of
 * the run, whatever a row did.
 *
 *   store      a store through W reaches X's translation: the write entry is
 *              untagged while the frame has no holder, retired by X's
 *              registration, and refilled tagged; a frame with no holder
 *              stays untagged.
 *   running    a store through W into the block that is running leaves the
 *              block before it runs the old bytes, for the ordinary store, a
 *              push and the baked entries alike.
 *   stack      the stack twins (the baked [rsp + displacement] entries) over
 *              an alias: a store through W as the stack, whose write entry
 *              carries the tag or has been retired, reaches X's translation
 *              and leaves the block that is running.
 *   serial     an address space's flush leaves its bodies' stamps untouched
 *              yet makes the body compare its source once and register its
 *              frame again; another address space's body on the same frame
 *              keeps its stamp hit.
 *   transient  a registration that fails (allocation) runs the instruction
 *              as a one-instruction transient program and registers on the
 *              next dispatch; no translation becomes runnable unregistered.
 *   helpers    a store that a helper makes and publishes -- x87, extended
 *              state, string, ENTER, the segment model's, across a page --
 *              leaves the running block as the ordinary store does.
 *   enter,     a later store of an instruction that already reached the
 *   stos       running block (ENTER's next push, the next element of a
 *              repeated STOS) publishes for itself.
 *   writer     an executor with no registry tags every write fill, so a
 *              frame translated afterwards, at an address of another
 *              generation, still sees its stores.
 *   registry   a registry that could not be made is made again by the next
 *              prepare of its generation.
 *   memo       an executor does not ask the address space again for a page
 *              whose registration settled, until another holder is added to
 *              the frame, its writers become unknown, the flush serial moves
 *              or the mapping changes (`memo-unknown` runs the row of the
 *              unknown writers alone); bytes written through another address
 *              run after a registration the memo answered (`memo-written`).
 */
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/cpu_state.hpp"
#include "cpu/x86_64/gadgets/executor.hpp"
#include "cpu/x86_64/gadgets/native_baked.hpp"
#include "cpu/x86_64/gadgets/program.hpp"
#include "cpu/x86_64/software_tlb.hpp"
#include "frame_accounting.hpp"
#include "memory/static_guest_memory.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <sys/mman.h>
#include <utility>
#include <vector>

/* The next allocation of exactly this many bytes fails: the registry's page
 * counts are the only array of this size an executor makes. */
std::atomic<std::size_t> failing_array_size{0};
constexpr std::size_t registry_page_counts = std::size_t{1} << 18;

void *operator new[](std::size_t size)
{
	auto armed = failing_array_size.load(std::memory_order_relaxed);
	if (armed != 0 && size == armed &&
	    failing_array_size.compare_exchange_strong(armed, 0))
		throw std::bad_alloc{};
	return ::operator new(size);
}

namespace {

using namespace uml::cpu::x86_64;
using uml::cpu::EngineEvent;
using uml::memory::FrameAccounting;
using uml::memory::StaticGuestMemory;
using uml::memory::WriterLocation;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rw = PROT_READ | PROT_WRITE;
constexpr int rx = PROT_READ | PROT_EXEC;
constexpr std::uint64_t write_tag_mask = 0xfff;
constexpr std::uint64_t write_code_tag = UML_X86_64_TLB_WRITE_CODE;

[[nodiscard]] gadgets::ProgramCacheSequences cache_sequences(
	void *context) noexcept
{
	const auto sequences =
		darwin_uml_static_vm_execution_context_sequences(context);
	return {sequences.mapping_changes, sequences.code_changes,
		sequences.execution_interrupt != 0};
}

[[nodiscard]] int cache_code_changes(void *context, std::uint64_t sequence,
	gadgets::ProgramCodeChange *changes, std::size_t capacity,
	std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, changes, capacity, current_sequence);
}

[[nodiscard]] int cache_mapping_changes(void *, std::uint64_t generation,
	std::uint64_t sequence, gadgets::ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept
{
	return darwin_uml_static_vm_mapping_changes(generation, sequence,
		changes, capacity, current_sequence);
}

struct Run final {
	EngineEvent event;
	std::size_t retired;
	std::uint64_t rax;
	std::uint64_t rip;
};

/* One address space and one executor on it, run as the runtime runs it: the
 * execution context entered, the executor prepared from its sequences, then
 * a block selected and executed. */
class Space final {
public:
	explicit Space(unsigned long generation)
		: generation_{generation},
		  context_{darwin_uml_static_vm_execution_context_create(generation)}
	{
		require(context_ != nullptr);
		memory_.set_generation(generation);
	}

	~Space()
	{
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

	/* Prepares the executor for another generation's address space, as a
	 * vCPU that moves between processes does. */
	void prepare_as(unsigned long generation)
	{
		require(darwin_uml_static_vm_execution_context_begin(context_, 0) == 0);
		const auto sequences =
			darwin_uml_static_vm_execution_context_sequences(context_);
		executor_.prepare(memory_, generation, sequences.mapping_changes,
			sequences.code_changes, context_, cache_sequences,
			cache_code_changes, cache_mapping_changes);
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
	}

	/* The registry the executor participates in, null when it could not
	 * make one. */
	[[nodiscard]] CodePageRegistry *registry() const noexcept
	{
		return executor_.code_page_registry();
	}

	/* The dispatches the last run_to_system_call made. */
	[[nodiscard]] unsigned last_dispatches() const noexcept
	{
		return last_dispatches_;
	}

	/* One dispatch from `cpu`: the block at its RIP, run to its exit. The
	 * block's stream goes to `stream` when it is given. */
	[[nodiscard]] Run step(CpuState &cpu,
		std::vector<std::uintptr_t> *stream = nullptr)
	{
		require(darwin_uml_static_vm_execution_context_begin(context_,
			cpu.rip) == 0, "the execution context was not entered");
		const auto sequences =
			darwin_uml_static_vm_execution_context_sequences(context_);
		memory_.prepare(generation_, sequences.mapping_changes);
		executor_.prepare(memory_, generation_, sequences.mapping_changes,
			sequences.code_changes, context_, cache_sequences,
			cache_code_changes, cache_mapping_changes);
		const auto selected = executor_.select_program(
			memory_, cpu.rip, ExecutionMode::long64, 64);
		require(selected.status == gadgets::BuildStatus::built &&
			selected.program != nullptr, "a block did not build");
		if (stream != nullptr) {
			const auto words = selected.program->native_stream();
			stream->assign(words.begin(), words.end());
		}
		const auto result = gadgets::execute(
			*selected.program, cpu, memory_, executor_, 64);
		require(darwin_uml_static_vm_execution_context_end(context_) == 0);
		return {result.event, result.retired_instructions,
			cpu.gpr(Register::rax), cpu.rip};
	}

	[[nodiscard]] Run run(std::uint64_t address, std::uint64_t rbx = 0,
		std::uint64_t rdx = 0)
	{
		CpuState cpu{};
		cpu.rip = address;
		cpu.gpr(Register::rbx) = rbx;
		cpu.gpr(Register::rdx) = rdx;
		return step(cpu);
	}

	/* Dispatches until the block makes its system call, as the runtime does
	 * across a block that leaves early. */
	[[nodiscard]] Run run_to_system_call(std::uint64_t address,
		std::uint64_t rbx = 0, std::uint64_t rdx = 0)
	{
		CpuState cpu{};
		cpu.rip = address;
		cpu.gpr(Register::rbx) = rbx;
		cpu.gpr(Register::rdx) = rdx;
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			const auto result = step(cpu);
			last_dispatches_ = dispatch + 1;
			if (result.event == EngineEvent::system_call)
				return result;
		}
		require(false, "the block never reached its system call");
		return {};
	}

	/* The write entry of the page at `address`: its tags, or ~0 when the
	 * entry holds another page. */
	[[nodiscard]] std::uint64_t write_tags(std::uint64_t address)
	{
		const auto &entry =
			executor_.tlb_entries()[native_tlb_index(address)];
		const auto held = entry.write_page;
		if ((held & ~write_tag_mask) != (address & ~write_tag_mask))
			return ~std::uint64_t{0};
		return held & write_tag_mask;
	}

	/* Whether the table holds the page at `address`'s write entry with the
	 * code bit set: an entry of another page, or an empty one, is not one. */
	[[nodiscard]] bool write_tagged(std::uint64_t address)
	{
		const auto tags = write_tags(address);
		return tags != ~std::uint64_t{0} && (tags & write_code_tag) != 0;
	}

	[[nodiscard]] const gadgets::ProgramCacheDiagnostics &diagnostics() const
		noexcept
	{
		return executor_.cache_diagnostics();
	}

	[[nodiscard]] unsigned long generation() const noexcept
	{
		return generation_;
	}

private:
	unsigned long generation_;
	void *context_;
	unsigned last_dispatches_{};
	StaticGuestMemory memory_;
	gadgets::Executor executor_;
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

std::vector<WriterLocation> holders_of(
	const std::shared_ptr<uml::memory::MappedBacking> &backing,
	unsigned long frame)
{
	std::vector<WriterLocation> holders;
	FrameAccounting::holders_for_testing(*backing, frame * page, holders);
	return holders;
}

/* Whether the generation holds the frame at `address`: the holder names the
 * generation by its state's alias id. */
[[nodiscard]] bool held_at(const std::vector<WriterLocation> &holders,
	unsigned long generation, std::uint64_t address)
{
	const auto state = uml::memory::testing::generation_state(generation);
	if (state == nullptr)
		return false;
	for (const auto &holder : holders)
		if (holder.page == address && holder.alias == state->accounting.alias)
			return true;
	return false;
}

/* The accounting errors the rows made on purpose. */
std::uint64_t forced_accounting_errors{};

/* A row leaves the counters that must stay 0 where it found them: no state
 * was destroyed with its locations still in the accounting, no holder
 * outlived its state, and no removal found its location missing. */
struct Clean final {
	FrameAccounting::Diagnostics start{FrameAccounting::diagnostics()};

	~Clean()
	{
		const auto now = FrameAccounting::diagnostics();
		require(now.nonempty_state_destructions ==
			start.nonempty_state_destructions,
			"a state was destroyed without its walk");
		require(now.holder_leaks == start.holder_leaks, "a holder leaked");
		require(now.errors == start.errors, "an accounting error was counted");
	}
};

/* A store through W reaches the translation at X, and the tag of W's write
 * entry follows the frame's holders. */
void test_store()
{
	const Clean clean;
	/* W and the writer are a page and two pages past a multiple of 256 MiB
	 * from X, which the table's direct index gives entries of their own: at
	 * an exact multiple, X's own retires would clear W's entry whether or
	 * not the registration retires the writer's. */
	constexpr std::uint64_t executable = 0x1000'0000;
	constexpr std::uint64_t alias = 0x2000'1000;
	constexpr std::uint64_t unheld = 0x2000'4000;
	constexpr std::uint64_t writer = 0x3000'2000;
	ScratchBacking frame{"uml-x86-alias-store-XXXXXX", page};
	ScratchBacking other{"uml-x86-alias-other-XXXXXX", page};
	ScratchBacking code{"uml-x86-alias-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9101};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	space.map(unheld, other, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);

	/* The frame has no holder: a store through W fills an untagged entry. */
	require(space.run(writer, 1, alias + 1).event == EngineEvent::system_call);
	require(space.write_tags(alias) == 0,
		"a write entry for a frame with no holder was tagged");

	/* X translates, which registers its frame and retires the untagged
	 * entry W's store left. */
	const auto first = space.run(executable);
	require(first.event == EngineEvent::system_call && first.rax == 1);
	require(held_at(holders_of(backing, 0), space.generation(), executable),
		"X's translation did not register its frame");
	require(space.write_tags(alias) == ~std::uint64_t{0},
		"the registration did not retire W's untagged entry");

	/* The store through W refills the entry tagged, and X sees the byte. */
	require(space.run(writer, 2, alias + 1).event == EngineEvent::system_call);
	require(space.write_tagged(alias),
		"a write entry for a frame with a holder was not tagged");
	require(space.run(executable).rax == 2,
		"a store through another address did not reach the translation");

	/* A frame no executor translated stays untagged. */
	require(space.run(writer, 3, unheld).event == EngineEvent::system_call);
	require(space.write_tags(unheld) == 0,
		"a write entry for an unheld frame was tagged");
	require(space.diagnostics().unheld_runnable == 0);
}

/* A block that stores through W into its own following bytes leaves before it
 * runs them: the guest sees the new immediate, not the old. */
void test_running_block()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1100'0000;
	constexpr std::uint64_t alias = 0x2100'1000;
	ScratchBacking frame{"uml-x86-alias-running-XXXXXX", page};
	/* mov [rdx], bl ; mov eax, imm32 ; syscall: the store names the
	 * immediate's low byte. */
	put(frame, 0, {0x88, 0x1a, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	Space space{9102};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	require(space.run_to_system_call(executable, 1, alias + 3).rax == 1);
	require(space.run_to_system_call(executable, 2, alias + 3).rax == 2,
		"the running block ran its old bytes after a store through W");
	require(space.diagnostics().unheld_runnable == 0);
}

/* The same through a push: the stack is W, and the push writes eight bytes
 * over the block that runs it. */
void test_running_push()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1400'0000;
	constexpr std::uint64_t alias = 0x2400'1000;
	ScratchBacking frame{"uml-x86-alias-push-XXXXXX", page};
	/* push rbx ; mov eax, imm32 ; syscall */
	put(frame, 0, {0x53, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	Space space{9106};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	/* RBX holds the bytes of the same block with the immediate changed. */
	const auto same = std::uint64_t{0x050f'0000'0001'b853};
	const auto changed = std::uint64_t{0x050f'0000'0002'b853};
	for (const auto [rbx, expected] : {std::pair{same, 1U}, {changed, 2U}}) {
		CpuState cpu{};
		cpu.rip = executable;
		cpu.gpr(Register::rbx) = rbx;
		cpu.gpr(Register::rsp) = alias + 8;
		Run result{};
		for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
			result = space.step(cpu);
			if (result.event == EngineEvent::system_call)
				break;
		}
		require(result.event == EngineEvent::system_call);
		require(result.rax == expected,
			"the running block ran its old bytes after a push through W");
	}
	require(space.diagnostics().unheld_runnable == 0);
}

/* The stack twins are the baked bank's last group, behind the unrecorded
 * twins, whose last entry is XOR's at 64 bits from an immediate into R15. */
[[nodiscard]] bool is_stack_twin(std::uintptr_t word) noexcept
{
	return word >
		gadgets::baked::uml_x86_64_baked_xor_64_ri_unrecorded[15];
}

/* The stack twins, which take a store of [rsp + displacement] only through
 * the stack's own write entry when its tag is exact and the run's
 * `write_code` is clear, over a frame that another address translates.
 * Every run starts at the entry of RSP's page, and here RSP is W's.
 *
 * Running: the store names the immediate of the `mov` after it. The entry W's
 * first store filled is tagged, so every later run reaches the twin with a
 * tag and a clear `write_code`, and the twin must still send the store to
 * the ordinary path, which publishes: the block leaves before it runs the
 * old bytes, and the run that follows leaves its own stack twin's load
 * through the table x22 holds.
 *
 * Follows: the entry is filled before the frame has a holder, so the twin
 * hits it, and X's registration retires it; the next store takes the
 * ordinary path, refills it tagged and reaches X, and the run after that
 * starts with the tagged entry and a clear `write_code`. */
void test_stack_twins_over_an_alias()
{
	const Clean clean;
	{
		/* W is a page past a multiple of 256 MiB from X: the table's index
		 * keeps the low 15 bits of the page number, so the rows above, whose
		 * addresses are exact multiples apart, share one entry between X's
		 * page and W's, and X's translation retires W's entry whatever
		 * names it. Here the two have entries of their own. */
		constexpr std::uint64_t executable = 0x1e00'0000;
		constexpr std::uint64_t alias = 0x2e00'1000;
		ScratchBacking frame{"uml-x86-alias-twin-XXXXXX", page};
		/* mov [rsp-59], bl ; mov eax, imm32 ; add eax, [rsp] ; syscall.
		 * RSP is W+64, so the store names the immediate's low byte at
		 * offset 5, and the load reads the word at offset 64. */
		put(frame, 0, {0x88, 0x5c, 0x24, 0xc5, 0xb8, 0x01, 0x00, 0x00,
			0x00, 0x03, 0x04, 0x24, 0x0f, 0x05});
		put(frame, 64, {0x00, 0x01, 0x00, 0x00});
		Space space{9800};
		space.map(executable, frame, 0, rx, 1);
		space.map(alias, frame, 0, rw, 0);
		for (const std::uint64_t immediate : {1U, 2U, 3U, 2U}) {
			CpuState cpu{};
			cpu.rip = executable;
			cpu.gpr(Register::rbx) = immediate;
			cpu.gpr(Register::rsp) = alias + 64;
			std::vector<std::uintptr_t> stream;
			const auto first = space.step(cpu, &stream);
			require(stream.size() > 3 && is_stack_twin(stream[0]),
				"the store did not take its stack twin");
			require(first.event == EngineEvent::continued &&
				first.retired == 1 && first.rip == executable + 4,
				"a stack twin's store through W did not leave the block");
			Run result{};
			for (unsigned dispatch = 0; dispatch < 8; ++dispatch) {
				result = space.step(cpu);
				if (result.event == EngineEvent::system_call)
					break;
			}
			require(result.event == EngineEvent::system_call);
			require(result.rax == 0x100 + immediate,
				"the running block ran its old bytes after a stack "
				"twin's store through W");
		}
		require(space.diagnostics().unheld_runnable == 0);
	}
	{
		constexpr std::uint64_t executable = 0x1f00'0000;
		constexpr std::uint64_t alias = 0x2f00'1000;
		constexpr std::uint64_t writer = 0x3f00'2000;
		ScratchBacking frame{"uml-x86-alias-twin-frame-XXXXXX", page};
		ScratchBacking code{"uml-x86-alias-twin-code-XXXXXX", page};
		load_return(frame, 1);
		/* mov [rsp], bl ; mov [rsp], bl ; syscall: the first store fills W's
		 * write entry and the second hits it as the stack's own. */
		put(code, 0, {0x88, 0x1c, 0x24, 0x88, 0x1c, 0x24, 0x0f, 0x05});
		Space space{9801};
		space.map(writer, code, 0, rx, 1);
		space.map(executable, frame, 0, rx, 1);
		space.map(alias, frame, 0, rw, 0);
		const auto store = [&](std::uint64_t value) {
			CpuState cpu{};
			cpu.rip = writer;
			cpu.gpr(Register::rbx) = value;
			cpu.gpr(Register::rsp) = alias + 1;
			std::vector<std::uintptr_t> stream;
			const auto result = space.step(cpu, &stream);
			require(stream.size() > 3 && is_stack_twin(stream[0]) &&
					is_stack_twin(stream[2]),
				"the stores did not take their stack twins");
			return result;
		};
		require(store(1).event == EngineEvent::system_call);
		require(space.write_tags(alias) == 0,
			"a write entry for a frame with no holder was tagged");
		const auto first = space.run(executable);
		require(first.event == EngineEvent::system_call && first.rax == 1);
		require(space.write_tags(alias) == ~std::uint64_t{0},
			"the registration did not retire the stack's write entry");
		for (const std::uint64_t value : {2U, 3U}) {
			require(store(value).event == EngineEvent::system_call);
			require(space.write_tagged(alias),
				"a write entry for a frame with a holder was not tagged");
			require(space.run(executable).rax == value,
				"a stack twin's store did not reach the translation");
		}
		require(space.diagnostics().unheld_runnable == 0);
	}
}

/* An address space's flush moves its flush serial and no stamp: its bodies
 * compare their source once and register again, while the body of another
 * address space on the same frame keeps its stamp hit. */
void test_flush_serial()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1200'0000;
	ScratchBacking frame{"uml-x86-alias-serial-XXXXXX", page};
	load_return(frame, 7);
	Space first{9103};
	Space second{9104};
	first.map(executable, frame, 0, rx, 1);
	second.map(executable, frame, 0, rx, 1);
	const auto backing = uml::memory::testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	require(first.run(executable).rax == 7 && second.run(executable).rax == 7);
	require(first.run(executable).rax == 7 && second.run(executable).rax == 7);
	require(held_at(holders_of(backing, 0), first.generation(), executable) &&
		held_at(holders_of(backing, 0), second.generation(), executable));

	const auto serials = FrameAccounting::diagnostics().serial_bumps;
	const auto first_before = first.diagnostics();
	const auto second_before = second.diagnostics();
	require(darwin_uml_static_vm_flush_generation(first.generation()) == 0);
	require(FrameAccounting::diagnostics().serial_bumps == serials + 1,
		"the flush did not move the serial once");
	require(!held_at(holders_of(backing, 0), first.generation(), executable),
		"the flush left a holder of the generation");
	require(held_at(holders_of(backing, 0), second.generation(), executable));
	/* The page-table walk the kernel makes next republishes the mapping. */
	first.map(executable, frame, 0, rx, 1);

	require(first.run(executable).rax == 7 && second.run(executable).rax == 7);
	const auto &first_after = first.diagnostics();
	const auto &second_after = second.diagnostics();
	require(first_after.flush_serial_misses ==
		first_before.flush_serial_misses + 1,
		"the flushed generation's body did not miss on the serial");
	require(first_after.validation_stamp_hits ==
		first_before.validation_stamp_hits,
		"the flushed generation's body hit its stamp");
	require(first_after.holder_registrations ==
		first_before.holder_registrations + 1,
		"the flushed generation's body did not register again");
	require(held_at(holders_of(backing, 0), first.generation(), executable));
	require(second_after.flush_serial_misses ==
		second_before.flush_serial_misses,
		"another generation's body missed on the serial");
	require(second_after.holder_registrations ==
			second_before.holder_registrations &&
		second_after.translations == second_before.translations &&
		second_after.validation_mismatches ==
			second_before.validation_mismatches,
		"another generation's body registered or translated again");
	require(first_after.validation_hits == first_before.validation_hits + 1,
		"the flushed generation's body did not match its source");
	require(first_after.unheld_runnable == 0 && second_after.unheld_runnable == 0);
}

/* The registration memo: a block built on a page its executor has already
 * registered does not ask the address space again. What ends the memo's
 * answer: a holder another generation adds to the frame, a move of the flush
 * serial (here with no change of mapping), and a change of the mapping that
 * took the page's holder with it. Each asks, and the holder is there after. */
void test_a_remembered_registration_is_skipped_until_something_moves()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t x = 0x1800'0000;
	ScratchBacking frame{"uml-x86-alias-memo-XXXXXX", page};
	/* mov eax, 10 + n ; syscall, one block every 0x100 bytes. */
	for (std::uint8_t block = 0; block < 6; ++block)
		put(frame, block * 0x100,
			{0xb8, static_cast<std::uint8_t>(10 + block), 0x00, 0x00, 0x00,
				0x0f, 0x05});
	Space space{9120};
	Space other{9121};
	space.map(x, frame, 0, rx, 1);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	const auto registered = [&](const gadgets::ProgramCacheDiagnostics &before,
				    const gadgets::ProgramCacheDiagnostics &after,
				    std::uint64_t asked, std::uint64_t answered) {
		return after.holder_registrations == before.holder_registrations + asked &&
			after.registration_memo_hits ==
				before.registration_memo_hits + answered;
	};

	auto before = space.diagnostics();
	require(space.run(x).rax == 10);
	auto after = space.diagnostics();
	require(registered(before, after, 1, 0),
		"the first block of a page did not register it");
	require(held_at(holders_of(backing, 0), space.generation(), x));

	before = after;
	require(space.run(x + 0x100).rax == 11);
	after = space.diagnostics();
	require(registered(before, after, 0, 1),
		"a block on a page already registered asked the address space again");

	/* Another generation holds the frame now: its addition moved the frame's
	 * change count. */
	other.map(x, frame, 0, rx, 1);
	require(other.run(x).rax == 10);
	require(held_at(holders_of(backing, 0), other.generation(), x));
	before = after;
	require(space.run(x + 0x200).rax == 12);
	after = space.diagnostics();
	require(registered(before, after, 1, 0),
		"a holder added by another generation did not end the memo's answer");
	/* What that registration left is remembered in turn. */
	before = after;
	require(space.run(x + 0x300).rax == 13);
	after = space.diagnostics();
	require(registered(before, after, 0, 1),
		"the registration made after another holder's was not remembered");

	/* The generation's serial moves, its mapping and holders do not. */
	FrameAccounting::bump_flush_serial(*testing::generation_state(space.generation()));
	before = after;
	require(space.run(x + 0x400).rax == 14);
	after = space.diagnostics();
	require(registered(before, after, 1, 0),
		"a move of the flush serial did not end the memo's answer");
	require(held_at(holders_of(backing, 0), space.generation(), x));

	/* The page goes and comes back: its holder is out of the frame until a
	 * block registers it again. */
	space.unmap(x);
	require(!held_at(holders_of(backing, 0), space.generation(), x),
		"an unmap left the page's holder");
	space.map(x, frame, 0, rx, 1);
	before = after;
	require(space.run(x + 0x500).rax == 15);
	after = space.diagnostics();
	require(registered(before, after, 1, 0),
		"a change of mapping did not end the memo's answer");
	require(held_at(holders_of(backing, 0), space.generation(), x),
		"a block ran on a page whose holder was not registered again");
	require(after.unheld_runnable == 0 && other.diagnostics().unheld_runnable == 0);
}

/* A body whose page stamp moved without its bytes changing validates by its
 * source, and the registration that validation makes is the memo's too: a
 * store past the block's bytes, through another address, moves the stamp, and
 * the validation that follows asks the address space nothing. */
void test_a_revalidation_uses_the_memo()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t executable = 0x1900'0000;
	constexpr std::uint64_t alias = 0x2900'2000;
	/* Not in the memo's slot of the executable page. */
	constexpr std::uint64_t writer = 0x3900'1000;
	ScratchBacking frame{"uml-x86-alias-memo-v-XXXXXX", page};
	ScratchBacking code{"uml-x86-alias-memo-w-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9122};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	require(space.run(executable).rax == 1);
	require(held_at(holders_of(backing, 0), space.generation(), executable));

	/* A store through another address, past the block's bytes: it moves the
	 * page's stamp and leaves the body's code as it was. */
	require(space.run(writer, 1, alias + 0x800).event == EngineEvent::system_call);
	const auto before = space.diagnostics();
	require(space.run(executable).rax == 1);
	const auto after = space.diagnostics();
	require(after.validation_hits == before.validation_hits + 1 &&
			after.validation_stamp_hits == before.validation_stamp_hits &&
			after.translations == before.translations,
		"the body was not validated by its source after the store");
	require(after.holder_registrations == before.holder_registrations,
		"a validation asked the address space for a registration the executor had");
	require(after.registration_memo_hits == before.registration_memo_hits + 1,
		"a validation did not use the executor's memo");
	require(after.unheld_runnable == 0);
}

/* A store through another address, published to a holder whose registration
 * the memo answered: the block is built again from the new bytes, runs them,
 * and asks the address space nothing. */
void test_newly_written_bytes_run_after_a_memo_hit()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t executable = 0x1a00'0000;
	constexpr std::uint64_t alias = 0x2a00'2000;
	/* Not in the memo's slot of the executable page. */
	constexpr std::uint64_t writer = 0x3a00'1000;
	ScratchBacking frame{"uml-x86-alias-memo-n-XXXXXX", page};
	ScratchBacking code{"uml-x86-alias-memo-m-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space space{9123};
	space.map(writer, code, 0, rx, 1);
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	require(space.run(executable).rax == 1);
	require(held_at(holders_of(backing, 0), space.generation(), executable));

	/* The new immediate, through the alias: it reaches the holder. */
	require(space.run(writer, 2, alias + 1).event == EngineEvent::system_call);
	const auto before = space.diagnostics();
	require(space.run(executable).rax == 2,
		"bytes written through another address did not run after a memo hit");
	const auto after = space.diagnostics();
	require(after.translations == before.translations + 1,
		"the block was not translated again from the new bytes");
	require(after.holder_registrations == before.holder_registrations &&
			after.registration_memo_hits == before.registration_memo_hits + 1,
		"the translation from the new bytes was not a memo hit");
	require(after.unheld_runnable == 0);
}

/* When a frame's writers become unknown the registration of the frame is the
 * global one, and an executor's memo of an earlier registration must not stand
 * in for it: the next block asks, is answered globally, and so is the one
 * after it. The unknown state is forced the way the accounting reports it, by
 * the removal of a writer location the frame never had. */
void test_the_unknown_writer_state_ends_the_memo()
{
	namespace testing = uml::memory::testing;
	const auto start = FrameAccounting::diagnostics();
	constexpr std::uint64_t x = 0x1b00'0000;
	constexpr std::uint64_t alias = 0x2b00'1000;
	ScratchBacking frame{"uml-x86-alias-memo-u-XXXXXX", page};
	for (std::uint8_t block = 0; block < 5; ++block)
		put(frame, block * 0x100,
			{0xb8, static_cast<std::uint8_t>(20 + block), 0x00, 0x00, 0x00,
				0x0f, 0x05});
	{
		Space space{9124};
		space.map(x, frame, 0, rx, 1);
		/* The alias makes the frame writable: its writers are known. */
		space.map(alias, frame, 0, rw, 0);
		const auto backing = testing::mapped_backing(frame.descriptor());
		require(backing != nullptr);
		const auto registered = [&](const gadgets::ProgramCacheDiagnostics &before,
					    const gadgets::ProgramCacheDiagnostics &after,
					    std::uint64_t asked, std::uint64_t answered) {
			return after.holder_registrations ==
					before.holder_registrations + asked &&
				after.registration_memo_hits ==
					before.registration_memo_hits + answered;
		};

		auto before = space.diagnostics();
		require(space.run(x).rax == 20);
		auto after = space.diagnostics();
		require(registered(before, after, 1, 0));
		before = after;
		require(space.run(x + 0x100).rax == 21);
		after = space.diagnostics();
		require(registered(before, after, 0, 1),
			"the memo did not answer before the writers became unknown");

		const auto errors = FrameAccounting::diagnostics().errors;
		FrameAccounting::remove_writer(*backing, 0, {777'777, x});
		require(FrameAccounting::diagnostics().errors == errors + 1,
			"a removal of a writer the frame never had was not an error");

		auto globals = FrameAccounting::diagnostics().global_holds;
		before = after;
		require(space.run(x + 0x200).rax == 22);
		after = space.diagnostics();
		require(registered(before, after, 1, 0),
			"the memo answered for a frame whose writers became unknown");
		require(FrameAccounting::diagnostics().global_holds == globals + 1,
			"a frame whose writers are unknown was not answered globally");
		/* The global answer is no registration to remember. */
		globals = FrameAccounting::diagnostics().global_holds;
		before = after;
		require(space.run(x + 0x300).rax == 23);
		after = space.diagnostics();
		require(registered(before, after, 1, 0),
			"a global answer was remembered");
		require(FrameAccounting::diagnostics().global_holds == globals + 1,
			"a second registration of the unknown frame was not global");
		require(after.unheld_runnable == 0);
	}
	const auto now = FrameAccounting::diagnostics();
	require(now.nonempty_state_destructions == start.nonempty_state_destructions,
		"a state was destroyed without its walk");
	require(now.holder_leaks == start.holder_leaks, "a holder leaked");
	/* The one error this row made. */
	require(now.errors == start.errors + 1, "another accounting error was counted");
	forced_accounting_errors += 1;
}

/* E7: units that link to each other stay safe when the frame of one of them
 * is written through W. The chain W -> A -> B is armed first: W stores past
 * B's code (no store of the warm-up touches B), jumps to A, which jumps to
 * B, and a round is one dispatch. Then W makes the one store into B's frame
 * with the chain armed, and B returns the new immediate: the store cools
 * the link A holds to B. A store that touches none of B's code leaves the
 * cached units and their links alone. */
void test_links_follow_alias_stores()
{
	const Clean clean;
	constexpr std::uint64_t a = 0x1600'0000;
	constexpr std::uint64_t b = a + 0x1000;
	constexpr std::uint64_t w = a + 0x2000;
	constexpr std::uint64_t alias = 0x2600'0000;
	ScratchBacking frame_a{"uml-x86-alias-link-a-XXXXXX", page};
	ScratchBacking frame_b{"uml-x86-alias-link-b-XXXXXX", page};
	ScratchBacking frame_w{"uml-x86-alias-link-w-XXXXXX", page};
	/* jmp B */
	put(frame_a, 0, {0xe9, 0xfb, 0x0f, 0x00, 0x00});
	load_return(frame_b, 1);
	/* mov [rdx], bl ; jmp A */
	put(frame_w, 0, {0x88, 0x1a, 0xe9, 0xf9, 0xdf, 0xff, 0xff});
	Space space{9106};
	space.map(w, frame_w, 0, rx, 1);
	space.map(a, frame_a, 0, rx, 1);
	space.map(b, frame_b, 0, rx, 1);
	space.map(alias, frame_b, 0, rw, 0);
	/* The warm-up stores past B's code: nothing invalidates B, and the
	 * links form. */
	for (unsigned round = 0; round < 8; ++round)
		require(space.run_to_system_call(w, 1, alias + 0x400).rax == 1);
	require(space.last_dispatches() == 1,
		"the chain W -> A -> B was not armed before the store");
	/* The one store into B's code, through W, with the chain armed. */
	require(space.run_to_system_call(w, 2, alias + 1).rax == 2,
		"a store through W did not reach a unit another unit links to");
	/* Past B's code again: the cached units and their links are kept. */
	const auto built = space.diagnostics().translations;
	require(space.run_to_system_call(w, 9, alias + 0x400).rax == 2);
	require(space.diagnostics().translations == built,
		"a store that touched no code translated the units again");
	require(space.diagnostics().unheld_runnable == 0);
}

/* E10: after a holder is removed at a page, no link reaches a body that
 * covers it before the body is validated again and registers its frame, and
 * a store through W then reaches the body. */
void test_a_removed_holder_is_registered_again_before_a_link_runs()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t a = 0x1700'0000;
	constexpr std::uint64_t b = a + 0x1000;
	constexpr std::uint64_t w = a + 0x2000;
	constexpr std::uint64_t alias = 0x2700'0000;
	ScratchBacking frame_a{"uml-x86-alias-gone-a-XXXXXX", page};
	ScratchBacking frame_b{"uml-x86-alias-gone-b-XXXXXX", page};
	ScratchBacking frame_w{"uml-x86-alias-gone-w-XXXXXX", page};
	put(frame_a, 0, {0xe9, 0xfb, 0x0f, 0x00, 0x00});
	load_return(frame_b, 1);
	put(frame_w, 0, {0x88, 0x1a, 0xe9, 0xf9, 0xdf, 0xff, 0xff});
	Space space{9107};
	space.map(w, frame_w, 0, rx, 1);
	space.map(a, frame_a, 0, rx, 1);
	space.map(b, frame_b, 0, rx, 1);
	space.map(alias, frame_b, 0, rw, 0);
	const auto backing = testing::mapped_backing(frame_b.descriptor());
	require(backing != nullptr);
	for (unsigned round = 0; round < 4; ++round)
		require(space.run_to_system_call(a).rax == 1);
	require(held_at(holders_of(backing, 0), space.generation(), b));

	/* The page goes and comes back: its holder is out of the frame until
	 * the body that covers it registers again. */
	space.unmap(b);
	require(!held_at(holders_of(backing, 0), space.generation(), b),
		"an unmap left the page's holder");
	space.map(b, frame_b, 0, rx, 1);
	require(!held_at(holders_of(backing, 0), space.generation(), b),
		"a mapping registered a holder nobody asked for");
	const auto before = space.diagnostics();
	require(space.run_to_system_call(a).rax == 1);
	const auto after = space.diagnostics();
	require(after.validation_attempts > before.validation_attempts,
		"a link reached a body whose page had been removed without validating it");
	require(after.holder_registrations > before.holder_registrations,
		"the body ran before its frame was registered again");
	require(held_at(holders_of(backing, 0), space.generation(), b));
	require(after.unheld_runnable == 0);

	/* And a store through W now reaches it. */
	require(space.run_to_system_call(w, 2, alias + 1).rax == 2,
		"a store through W did not reach a body registered again");
	require(space.diagnostics().unheld_runnable == 0);
}

void test_registration_failure()
{
	const Clean clean;
	namespace testing = uml::memory::testing;
	constexpr std::uint64_t executable = 0x1300'0000;
	ScratchBacking frame{"uml-x86-alias-failure-XXXXXX", page};
	load_return(frame, 5);
	Space space{9105};
	space.map(executable, frame, 0, rx, 1);
	const auto backing = testing::mapped_backing(frame.descriptor());
	require(backing != nullptr);
	const auto before = space.diagnostics();
	/* The frame's record exists from the mapping, so the registration's own
	 * allocation, the list of holders, is the one that fails. */
	testing::fail_allocation_at(testing::AllocationSite::holders, 1);
	const auto failed = space.run(executable);
	testing::clear_allocation_failure();
	require(failed.event == EngineEvent::continued && failed.retired == 1,
		"a failed registration ran more than its one instruction");
	require(space.diagnostics().unregistered_transients ==
		before.unregistered_transients + 1,
		"a failed registration was not counted");
	require(!held_at(holders_of(backing, 0), space.generation(), executable),
		"a failed registration left a holder");
	/* The next dispatch registers and runs the whole block. */
	const auto settled = space.run_to_system_call(executable);
	require(settled.rax == 5);
	require(held_at(holders_of(backing, 0), space.generation(), executable));
	require(space.diagnostics().unheld_runnable == 0);
	require(space.diagnostics().walked_refusals == 0);
}


/* Every helper that stores, one instruction each: the program is the store,
 * nops up to offset 64, then `mov eax, 1 ; syscall`, and the store writes
 * through W starting at that `mov`, so it lands in the block that is running.
 * The block leaves after the store -- one retired instruction, no system
 * call -- whatever helper stored, because the writing thread's next
 * instruction sees the new bytes. A store the engine publishes through a
 * helper and then forgets reaching the running block runs the old `mov` and
 * the system call in this dispatch.
 *
 *   x87 stores and x87 environment saves (bulk helper, same page), the
 *   extended-state saves (bulk helper, always leave), the segment model's
 *   SLDT and STR, the string stores (single and repeated, the element and
 *   the bulk path), ENTER, and the cross-page scalar and vector stores below.
 */
struct StoreForm final {
	const char *name;
	std::vector<std::uint8_t> code;
	void (*prepare)(CpuState &, std::uint64_t target);
};

void prepare_pointer(CpuState &cpu, std::uint64_t target)
{
	cpu.gpr(Register::rdx) = target;
	cpu.gpr(Register::rbx) = target;
}

/* The x87 store of ST(0): the value's ten bytes are `mov eax, 2 ; syscall`. */
void prepare_x87(CpuState &cpu, std::uint64_t target)
{
	prepare_pointer(cpu, target);
	cpu.x87_top = 0;
	cpu.x87_tag = 0xfffc;
	cpu.x87[0].significand = 0x0005'0f00'0000'02b8ULL;
	cpu.x87[0].sign_exponent = 0;
}

void prepare_x87_zero(CpuState &cpu, std::uint64_t target)
{
	prepare_x87(cpu, target);
	cpu.x87[0].significand = 0;
}

void prepare_extended(CpuState &cpu, std::uint64_t target)
{
	prepare_pointer(cpu, target);
	cpu.gpr(Register::rdx) = 0;
	cpu.gpr(Register::rax) = 3;
}

void prepare_string(CpuState &cpu, std::uint64_t target)
{
	prepare_pointer(cpu, target);
	cpu.gpr(Register::rdi) = target;
	cpu.gpr(Register::rsi) = target + 0x400;
	cpu.gpr(Register::rcx) = 64;
	cpu.gpr(Register::rax) = 0;
}

void prepare_enter(CpuState &cpu, std::uint64_t target)
{
	prepare_pointer(cpu, target);
	cpu.gpr(Register::rsp) = target + 8;
	cpu.gpr(Register::rbp) = 0x1122'3344'5566'7788ULL;
}

void test_helper_stores_leave_the_block()
{
	const Clean clean;
	const std::vector<StoreForm> forms{
		{"fstp tbyte", {0xdb, 0x3a}, prepare_x87},
		{"fbstp", {0xdf, 0x32}, prepare_x87_zero},
		{"fnstenv (28)", {0xd9, 0x32}, prepare_x87},
		{"fnstenv (14)", {0x66, 0xd9, 0x32}, prepare_x87},
		{"fnsave (108)", {0xdd, 0x32}, prepare_x87},
		{"fnsave (94)", {0x66, 0xdd, 0x32}, prepare_x87},
		{"fxsave", {0x0f, 0xae, 0x03}, prepare_extended},
		{"fxsave64", {0x48, 0x0f, 0xae, 0x03}, prepare_extended},
		{"xsave", {0x0f, 0xae, 0x23}, prepare_extended},
		{"sldt", {0x0f, 0x00, 0x02}, prepare_pointer},
		{"str", {0x0f, 0x00, 0x0a}, prepare_pointer},
		{"stosb", {0xaa}, prepare_string},
		{"rep stosb", {0xf3, 0xaa}, prepare_string},
		{"rep movsb", {0xf3, 0xa4}, prepare_string},
		{"enter", {0xc8, 0x08, 0x00, 0x00}, prepare_enter},
	};
	unsigned long generation = 9300;
	/* Every form runs before the row fails, so a failure names them all. */
	unsigned failures = 0;
	for (const auto &form : forms) {
		constexpr std::uint64_t executable = 0x1500'0000;
		constexpr std::uint64_t alias = 0x2500'0000;
		ScratchBacking frame{"uml-x86-alias-helper-XXXXXX", page};
		std::vector<std::uint8_t> bytes = form.code;
		bytes.resize(64, 0x90);
		for (const auto byte : {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05})
			bytes.push_back(static_cast<std::uint8_t>(byte));
		std::vector<std::byte> data;
		for (const auto byte : bytes)
			data.push_back(static_cast<std::byte>(byte));
		frame.put(0, data);
		Space space{generation++};
		space.map(executable, frame, 0, rx, 1);
		space.map(alias, frame, 0, rw, 0);
		CpuState cpu{};
		cpu.rip = executable;
		form.prepare(cpu, alias + 64);
		const auto first = space.step(cpu);
		if (first.event != EngineEvent::continued || first.retired != 1) {
			std::fprintf(stderr, "%s: event %d after %zu instructions\n",
				form.name, static_cast<int>(first.event), first.retired);
			++failures;
			continue;
		}
		require(space.diagnostics().unheld_runnable == 0);
		require(first.rip == executable + form.code.size(),
			"the block left with RIP at another instruction");
	}
	require(failures == 0, "a helper store did not leave the running block");
}

/* The value the next instruction runs is the one the store wrote: the x87
 * store of ten bytes and a store across a page, scalar and vector. */
void test_helper_stores_are_seen_by_the_next_instruction()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1600'0000;
	constexpr std::uint64_t alias = 0x2600'0000;
	ScratchBacking frame{"uml-x86-alias-seen-XXXXXX", page};
	ScratchBacking next{"uml-x86-alias-next-XXXXXX", page};
	std::vector<std::byte> bytes(page, std::byte{0x90});
	const auto set = [&](std::size_t at, std::initializer_list<std::uint8_t> v) {
		for (const auto byte : v)
			bytes[at++] = static_cast<std::byte>(byte);
	};
	/* fstp tbyte [rdx] ; mov eax, 1 ; syscall */
	set(0, {0xdb, 0x3a});
	set(64, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05});
	frame.put(0, bytes);
	Space space{9320};
	space.map(executable, frame, 0, rx, 1);
	space.map(alias, frame, 0, rw, 0);
	CpuState cpu{};
	cpu.rip = executable;
	prepare_x87(cpu, alias + 64);
	Run result{};
	for (unsigned dispatch = 0; dispatch < 4; ++dispatch) {
		result = space.step(cpu);
		if (result.event == EngineEvent::system_call)
			break;
	}
	require(result.event == EngineEvent::system_call && result.rax == 2,
		"the instruction after an x87 store ran its old bytes");
}

/* A store across the end of W's page: the part in W is the next instruction
 * of the block at the end of X's page. The page after W is another frame. */
void test_cross_page_stores_leave_the_block()
{
	const Clean clean;
	struct Case final {
		const char *name;
		std::vector<std::uint8_t> code;
		bool vector;
	};
	const std::vector<Case> cases{
		{"mov [rdx], rbx", {0x48, 0x89, 0x1a}, false},
		{"movups [rdx], xmm0", {0x0f, 0x11, 0x02}, true},
	};
	unsigned long generation = 9330;
	for (const auto &item : cases) {
		constexpr std::uint64_t executable = 0x1700'0000;
		constexpr std::uint64_t alias = 0x2700'0000;
		ScratchBacking frame{"uml-x86-alias-cross-XXXXXX", page};
		ScratchBacking beyond{"uml-x86-alias-beyond-XXXXXX", page};
		std::vector<std::byte> bytes(page, std::byte{0x90});
		std::size_t at = 0xff0;
		for (const auto byte : item.code)
			bytes[at++] = static_cast<std::byte>(byte);
		/* mov al, 1 ; syscall, the last four bytes of the page */
		at = 0xffc;
		for (const auto byte : {0xb0, 0x01, 0x0f, 0x05})
			bytes[at++] = static_cast<std::byte>(byte);
		frame.put(0, bytes);
		Space space{generation++};
		space.map(executable, frame, 0, rx, 1);
		space.map(alias, frame, 0, rw, 0);
		space.map(alias + page, beyond, 0, rw, 0);
		CpuState cpu{};
		cpu.rip = executable + 0xff0;
		cpu.gpr(Register::rdx) = alias + 0xffc;
		cpu.gpr(Register::rbx) = 0x050f'02b0ULL;
		const std::uint32_t changed = 0x050f'02b0U;
		std::memcpy(cpu.vectors[0].data(), &changed, sizeof(changed));
		const auto first = space.step(cpu);
		if (first.event != EngineEvent::continued || first.retired != 1) {
			std::fprintf(stderr, "%s: event %d after %zu instructions\n",
				item.name, static_cast<int>(first.event), first.retired);
			require(false, "a store across a page did not leave the block");
		}
		Run result{};
		for (unsigned dispatch = 0; dispatch < 4; ++dispatch) {
			result = space.step(cpu);
			if (result.event == EngineEvent::system_call)
				break;
		}
		require(result.event == EngineEvent::system_call &&
			(result.rax & 0xff) == 2,
			"the instruction after a store across a page ran its old bytes");
	}
}

/* A later store of an instruction that has already reached the running block
 * publishes for itself. ENTER with nesting level 2 pushes three times through
 * W: the first push, into the page of the block that runs it, reaches the
 * block; the second, into the page below, which another frame backs, writes
 * the bytes of a block of that frame cached at another address (X2). The
 * instruction leaves, and X2 runs what the push wrote. */
void test_enter_pushes_publish_each()
{
	const Clean clean;
	constexpr std::uint64_t running = 0x1900'0000;
	constexpr std::uint64_t cached = 0x1a00'0000;
	constexpr std::uint64_t stack = 0x2900'0000;
	constexpr std::uint64_t data = 0x2a00'0000;
	ScratchBacking first{"uml-x86-alias-enter-1-XXXXXX", page};
	ScratchBacking second{"uml-x86-alias-enter-2-XXXXXX", page};
	ScratchBacking value{"uml-x86-alias-enter-v-XXXXXX", page};
	std::vector<std::byte> bytes(page, std::byte{0x90});
	/* enter 0, 2 */
	for (const auto [at, byte] : {std::pair{0, 0xc8}, {1, 0x00}, {2, 0x00},
		{3, 0x02}})
		bytes[at] = static_cast<std::byte>(byte);
	first.put(0, bytes);
	/* The block X2 runs: mov al, 1 ; syscall, at the page's last eight
	 * bytes. */
	std::vector<std::byte> tail(page, std::byte{0x90});
	std::size_t at = 0xff8;
	for (const auto byte : {0xb0, 0x01, 0x0f, 0x05})
		tail[at++] = static_cast<std::byte>(byte);
	second.put(0, tail);
	/* What the second push writes: mov al, 2 ; syscall. */
	std::vector<std::byte> pushed(page, std::byte{0x90});
	at = 0;
	for (const auto byte : {0xb0, 0x02, 0x0f, 0x05})
		pushed[at++] = static_cast<std::byte>(byte);
	value.put(0, pushed);
	Space space{9430};
	space.map(running, first, 0, rx, 1);
	space.map(cached, second, 0, rx, 1);
	/* The stack grows down from the top of the first frame's page, through
	 * W, into the second frame's. */
	space.map(stack, second, 0, rw, 0);
	space.map(stack + page, first, 0, rw, 0);
	space.map(data, value, 0, rw, 0);
	const auto backing = uml::memory::testing::mapped_backing(second.descriptor());
	require(backing != nullptr);
	/* X2 is translated and cached before the instruction runs. */
	for (unsigned round = 0; round < 2; ++round)
		require((space.run_to_system_call(cached + 0xff8).rax & 0xff) == 1);
	CpuState cpu{};
	cpu.rip = running;
	cpu.gpr(Register::rsp) = stack + page + 8;
	/* ENTER's nesting level loads the word below RBP. */
	cpu.gpr(Register::rbp) = data + 8;
	const auto entered = space.step(cpu);
	require(entered.event == EngineEvent::continued && entered.retired == 1,
		"the instruction that stored over the running block did not leave it");
	require((space.run_to_system_call(cached + 0xff8).rax & 0xff) == 2,
		"a later push of ENTER did not reach a block cached at another address");
	require(space.diagnostics().unheld_runnable == 0);
}

/* The same for the elements of a repeated STOS: the first sixteen bytes land
 * in the frame of the block that runs it, the next four in the first bytes of
 * the frame after W's page, whose block is cached at another address. */
void test_repeated_stores_publish_each()
{
	const Clean clean;
	constexpr std::uint64_t running = 0x1b00'0000;
	constexpr std::uint64_t cached = 0x1c00'0000;
	constexpr std::uint64_t stack = 0x2b00'0000;
	ScratchBacking first{"uml-x86-alias-stos-1-XXXXXX", page};
	ScratchBacking second{"uml-x86-alias-stos-2-XXXXXX", page};
	std::vector<std::byte> bytes(page, std::byte{0x90});
	/* rep stosb, at the end of the page */
	bytes[0xff0] = std::byte{0xf3};
	bytes[0xff1] = std::byte{0xaa};
	first.put(0, bytes);
	/* mov al, 1 ; syscall ; syscall. The four bytes written over it make
	 * `mov al, 0xb0 ; mov al, 0xb0`, and the syscall after them runs. */
	std::vector<std::byte> head(page, std::byte{0x90});
	std::size_t at = 0;
	for (const auto byte : {0xb0, 0x01, 0x0f, 0x05, 0x0f, 0x05})
		head[at++] = static_cast<std::byte>(byte);
	second.put(0, head);
	Space space{9431};
	space.map(running, first, 0, rx, 1);
	space.map(cached, second, 0, rx, 1);
	space.map(stack, first, 0, rw, 0);
	space.map(stack + page, second, 0, rw, 0);
	for (unsigned round = 0; round < 2; ++round)
		require((space.run_to_system_call(cached).rax & 0xff) == 1);
	CpuState cpu{};
	cpu.rip = running + 0xff0;
	cpu.gpr(Register::rdi) = stack + 0xff0;
	cpu.gpr(Register::rcx) = 20;
	cpu.gpr(Register::rax) = 0xb0;
	const auto stored = space.step(cpu);
	require(stored.event == EngineEvent::continued,
		"the repeated store did not leave the block it wrote");
	require(cpu.gpr(Register::rcx) == 0, "the repeated store did not finish");
	require((space.run_to_system_call(cached).rax & 0xff) == 0xb0,
		"a later element of a repeated store did not reach a block cached at "
		"another address");
	require(space.diagnostics().unheld_runnable == 0);
}

/* A writer whose executor has no registry (its allocation failed) is not
 * named by a registration that retires write translations, so every write
 * entry it fills is tagged: W's store, made before any frame holder exists,
 * must still reach the block that X, of another generation, translates
 * afterwards. */
void test_a_writer_without_a_registry()
{
	const Clean clean;
	constexpr std::uint64_t executable = 0x1d00'0000;
	constexpr std::uint64_t alias = 0x2d00'1000;
	constexpr std::uint64_t writer = 0x3d00'2000;
	ScratchBacking frame{"uml-x86-alias-bare-XXXXXX", page};
	ScratchBacking code{"uml-x86-alias-bare-code-XXXXXX", page};
	load_return(frame, 1);
	load_writer(code);
	Space w{9440};
	Space x{9441};
	w.map(writer, code, 0, rx, 1);
	w.map(alias, frame, 0, rw, 0);
	x.map(executable, frame, 0, rx, 1);
	failing_array_size.store(registry_page_counts);
	/* W's first store: no holder of the frame exists yet. */
	require(w.run(writer, 1, alias + 1).event == EngineEvent::system_call);
	require(failing_array_size.load() == 0 && w.registry() == nullptr,
		"the writer's registry was made");
	/* X translates, which registers its frame. */
	require((x.run_to_system_call(executable).rax & 0xff) == 1);
	/* W stores again through the entry it filled before X existed. */
	require(w.run(writer, 2, alias + 1).event == EngineEvent::system_call);
	require((x.run_to_system_call(executable).rax & 0xff) == 2,
		"a store of an executor with no registry did not reach a block "
		"translated after its first store");
	require(w.write_tagged(alias),
		"a write entry of an executor with no registry was not tagged");
	require(x.diagnostics().unheld_runnable == 0);
}

/* A registry that could not be made is made again: the executor prepared for
 * a generation whose registry failed, then for another, and again for the
 * first, finds nothing left from the failure. */
void test_a_failed_registry_is_made_again()
{
	const Clean clean;
	Space space{9450};
	failing_array_size.store(registry_page_counts);
	space.prepare_as(9451);
	require(failing_array_size.load() == 0,
		"the registry's allocation was not the one that failed");
	require(space.registry() == nullptr);
	space.prepare_as(9452);
	require(space.registry() != nullptr);
	space.prepare_as(9451);
	require(space.registry() != nullptr,
		"the registry of a generation that failed once was not made again");
}

/* The kernel stores into another process's page (ptrace, /proc/pid/mem,
 * process_vm_writev) and names that process's generation: a translation of
 * the frame in another generation runs the new bytes, whatever the target's
 * own view of the frame is. The target maps it as data (a writable alias of
 * a frame that another process executes), maps nothing at the page (the
 * kernel fills the software table at faults, so it has not yet), or maps it
 * as code (the control). */
void test_a_remote_kernel_write_reaches_the_reader()
{
	const Clean clean;
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	constexpr std::uint64_t reading = 0x3a00'0000;
	constexpr std::uint64_t writing = 0x3b00'0000;
	enum class View { data, absent, code };
	unsigned long generation = 9500;
	/* Every view is run, so one run names each that fails. */
	unsigned failed = 0;
	for (const auto view : {View::data, View::absent, View::code}) {
		const char *name = view == View::data ? "data view" :
			view == View::absent ? "page the target does not map" :
			"code view";
		ScratchBacking frame{"uml-x86-alias-remote-XXXXXX", page};
		load_return(frame, 1);
		Space reader{generation++};
		Space target{generation++};
		reader.map(reading, frame, 0, rx, 1);
		if (view == View::data)
			target.map(writing, frame, 0, rw, 0);
		else if (view == View::code)
			target.map(writing, frame, 0, rx, 1);
		require(reader.run_to_system_call(reading).rax == 1);
		load_return(frame, 2);
		require(darwin_uml_static_vm_invalidate_code_generation(
			target.generation(), writing, page) == 0);
		if (reader.run_to_system_call(reading).rax != 2) {
			std::fprintf(stderr, "a remote write into a %s did not reach "
				"a translation of the frame\n", name);
			++failed;
		}
	}
	require(failed == 0, "a remote kernel write left a translation running");
}

/* A kernel write that names no frame is published to every generation as
 * the whole space (a code change of length 0), and a unit that links to
 * another keeps running the callee it was linked to unless the executor
 * answers that publication by resetting every link record. The caller A
 * jumps to the callee B, the pair is armed (one dispatch per call), then B's
 * frame is written and the write is published: the next call to A must run
 * the new callee. The routes are the remote hook for a page the target maps
 * as data (a described range, the control) and for one it does not map, and
 * the ordinary kernel write whose buffer the host table does not resolve. */
void test_a_whole_space_publication_resets_the_links()
{
	const Clean clean;
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	constexpr std::uint64_t a = 0x3c00'0000;
	constexpr std::uint64_t b = a + 0x1000;
	constexpr std::uint64_t writing = 0x3d00'0000;
	enum class Route { hook_data, hook_absent, kernel_write_unresolved };
	unsigned long generation = 9600;
	unsigned failed = 0;
	for (const auto route : {Route::hook_data, Route::hook_absent,
		Route::kernel_write_unresolved}) {
		const char *name = route == Route::hook_data ?
			"remote write into a data view" :
			route == Route::hook_absent ?
			"remote write into a page the target does not map" :
			"kernel write through a buffer no frame is named by";
		ScratchBacking frame_a{"uml-x86-alias-whole-a-XXXXXX", page};
		ScratchBacking frame_b{"uml-x86-alias-whole-b-XXXXXX", page};
		/* jmp B */
		put(frame_a, 0, {0xe9, 0xfb, 0x0f, 0x00, 0x00});
		load_return(frame_b, 1);
		Space reader{generation++};
		Space target{generation++};
		reader.map(a, frame_a, 0, rx, 1);
		reader.map(b, frame_b, 0, rx, 1);
		if (route == Route::hook_data)
			target.map(writing, frame_b, 0, rw, 0);
		for (unsigned round = 0; round < 8; ++round)
			require(reader.run_to_system_call(a).rax == 1);
		require(reader.last_dispatches() == 1,
			"the pair A -> B was not armed before the write");
		load_return(frame_b, 2);
		if (route == Route::kernel_write_unresolved) {
			std::array<unsigned char, 8> loose{};
			require(darwin_uml_static_vm_activate(target.generation()) == 0);
			require(darwin_uml_static_vm_copy_kernel_write(
				writing, loose.data(), loose.size()) == 0);
		} else {
			require(darwin_uml_static_vm_invalidate_code_generation(
				target.generation(), writing, page) == 0);
		}
		if (reader.run_to_system_call(a).rax != 2) {
			std::fprintf(stderr, "a %s left the caller running the old "
				"callee\n", name);
			++failed;
		}
		require(reader.diagnostics().unheld_runnable == 0);
	}
	require(failed == 0, "a whole-space publication left a link running");
}

/* The kernel pins a frame to write it and names it. Between the pin and the
 * write the target replaces what is mapped at the address with another frame,
 * so the call meets a page that is not the frame written, and the reader of
 * the frame written must still hear of the write. The reader runs once more
 * after the replacement, so no publication of the replacement can account for
 * what it hears. */
void test_a_remote_write_after_the_target_remapped()
{
	const Clean clean;
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	constexpr std::uint64_t reading = 0x3e00'0000;
	constexpr std::uint64_t writing = 0x3f00'0000;
	enum class Replacement { data, code };
	unsigned long generation = 9700;
	unsigned failed = 0;
	for (const auto replacement : {Replacement::data, Replacement::code}) {
		const char *name = replacement == Replacement::data ?
			"a data view of another frame" : "a code view of another frame";
		ScratchBacking pinned{"uml-x86-alias-remap-p-XXXXXX", page};
		ScratchBacking other{"uml-x86-alias-remap-o-XXXXXX", page};
		load_return(pinned, 1);
		load_return(other, 3);
		Space reader{generation++};
		Space target{generation++};
		reader.map(reading, pinned, 0, rx, 1);
		target.map(writing, pinned, 0, rw, 0);
		require(reader.run_to_system_call(reading).rax == 1);
		/* The pin, then the replacement. */
		target.unmap(writing);
		if (replacement == Replacement::data)
			target.map(writing, other, 0, rw, 0);
		else
			target.map(writing, other, 0, rx, 1);
		require(reader.run_to_system_call(reading).rax == 1);
		/* The store reaches the pinned frame, which the call names. */
		load_return(pinned, 2);
		require(darwin_uml_static_vm_invalidate_code_frame(
			target.generation(), writing, page, pinned.descriptor(), 0) == 0);
		if (reader.run_to_system_call(reading).rax != 2) {
			std::fprintf(stderr, "a remote write after the target replaced "
				"its mapping with %s did not reach a translation of the "
				"frame\n", name);
			++failed;
		}
	}
	require(failed == 0, "a remote write after a remap left a translation "
		"running");
}

} // namespace

int main(int argc, char **argv)
{
	/* A row's name as the first argument runs that row alone. */
	const char *only = argc > 1 ? argv[1] : nullptr;
	const auto selected = [&](const char *name) {
		return only == nullptr || std::strcmp(only, name) == 0;
	};
	const auto start = FrameAccounting::diagnostics();
	if (selected("store")) {
		test_store();
		std::puts("store ok");
	}
	if (selected("running")) {
		test_running_block();
		std::puts("running ok");
	}
	if (selected("push")) {
		test_running_push();
		std::puts("push ok");
	}
	if (selected("stack")) {
		test_stack_twins_over_an_alias();
		std::puts("stack twins ok");
	}
	if (selected("serial")) {
		test_flush_serial();
		std::puts("serial ok");
	}
	if (selected("links")) {
		test_links_follow_alias_stores();
		std::puts("links ok");
	}
	if (selected("removed")) {
		test_a_removed_holder_is_registered_again_before_a_link_runs();
		std::puts("removed ok");
	}
	if (selected("transient")) {
		test_registration_failure();
		std::puts("transient ok");
	}
	if (selected("helpers")) {
		test_helper_stores_leave_the_block();
		std::puts("helper stores ok");
		test_helper_stores_are_seen_by_the_next_instruction();
		std::puts("helper store value ok");
		test_cross_page_stores_leave_the_block();
		std::puts("cross-page stores ok");
	}
	if (selected("enter")) {
		test_enter_pushes_publish_each();
		std::puts("enter pushes ok");
	}
	if (selected("stos")) {
		test_repeated_stores_publish_each();
		std::puts("repeated stores ok");
	}
	if (selected("writer")) {
		test_a_writer_without_a_registry();
		std::puts("writer without a registry ok");
	}
	if (selected("registry")) {
		test_a_failed_registry_is_made_again();
		std::puts("failed registry ok");
	}
	if (selected("memo")) {
		test_a_remembered_registration_is_skipped_until_something_moves();
		std::puts("registration memo ok");
		test_a_revalidation_uses_the_memo();
		std::puts("revalidation memo ok");
	}
	if (selected("memo") || selected("memo-written")) {
		test_newly_written_bytes_run_after_a_memo_hit();
		std::puts("memo and written bytes ok");
	}
	if (selected("memo") || selected("memo-unknown")) {
		test_the_unknown_writer_state_ends_the_memo();
		std::puts("memo and unknown writers ok");
	}
	if (selected("remote")) {
		test_a_remote_kernel_write_reaches_the_reader();
		std::puts("remote kernel write ok");
	}
	if (selected("remote") || selected("whole-space")) {
		test_a_whole_space_publication_resets_the_links();
		std::puts("whole-space publication and links ok");
	}
	if (selected("remote") || selected("remapped")) {
		test_a_remote_write_after_the_target_remapped();
		std::puts("remote write after a remap ok");
	}
	const auto now = FrameAccounting::diagnostics();
	require(now.holder_leaks == start.holder_leaks, "a holder leaked");
	require(now.unquiesced_holder_removals == start.unquiesced_holder_removals);
	require(now.unmarked_holder_removals == start.unmarked_holder_removals);
	require(now.walked_refusals == start.walked_refusals);
	require(now.errors == start.errors + forced_accounting_errors,
		"an accounting error was counted");
	std::puts("x86-64 gadget alias tests passed");
	return EXIT_SUCCESS;
}
