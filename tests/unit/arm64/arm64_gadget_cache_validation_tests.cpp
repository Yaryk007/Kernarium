// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "cpu/gadget_cache_geometry.hpp"
#include "static_mmu_test_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#ifndef UML_GADGET_CACHE_SLOTS
#error "gadget cache validation test needs the configured cache geometry"
#endif

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

void write_mapped32(unsigned long generation, std::uint64_t address,
	std::uint32_t value)
{
	void *destination = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, address, DARWIN_UML_STATIC_WRITE, &destination) ==
		DARWIN_UML_STATIC_TRANSLATED);
	std::memcpy(destination, &value, sizeof(value));
}

void unchanged_words_avoid_retranslation()
{
	constexpr unsigned long generation = 920;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);
	JitMapping mapping{generation, code_base, backing, 2};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);

	darwin_uml_static_vm_invalidate_code(
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);
	const auto diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);
}

void changed_words_rebuild_and_republish_links()
{
	constexpr unsigned long generation = 921;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, branch(code_base, target));
	backing.put32(target - code_base, move_immediate(0, 1));
	backing.put32(target - code_base + 4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == 1);

	write_mapped32(generation, target, move_immediate(0, 2));
	darwin_uml_static_vm_invalidate_code(target);
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(executor, memory, generation, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	const auto rebuilt = run_once(executor, memory, generation, cpu);
	require(rebuilt.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);
	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(executor, memory, generation, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == 2);

	const auto diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == 3);
	require(diagnostics.validation_attempts == 2);
	require(diagnostics.validation_hits == 1);
	require(diagnostics.validation_mismatches == 1);
}

/* Linux writes a traced process's text through its own view of the frame --
 * ptrace inserting a breakpoint -- so no translated store publishes it. The
 * kernel then names the process's generation, the written range and, for a
 * ptrace or process_vm_writev(), the frame, or only the range, for a flush by
 * address (an AArch32 cacheflush()), through darwin_uml_vm_invalidate_code()
 * or darwin_uml_vm_invalidate_code_frame(); the page stamp must move and the
 * next run must execute the new instruction. */
void a_kernel_text_write_republishes_through_the_bridge()
{
	constexpr unsigned long generation = 922;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 1));
	backing.put32(4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 1);

	struct darwin_uml_static_code_page_stamp before{};
	void *handle = nullptr;
	require(darwin_uml_static_vm_code_page(generation, code_base, &handle,
		&before) == DARWIN_UML_STATIC_TRANSLATED);
	backing.put32(0, move_immediate(0, 2));
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, code_base, 4) == 0);
	struct darwin_uml_static_code_page_stamp after{};
	require(darwin_uml_static_vm_code_page(generation, code_base, &handle,
		&after) == DARWIN_UML_STATIC_TRANSLATED);
	require(after.identity == before.identity &&
		after.version != before.version);

	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);

	/* process_vm_writev() reports its data transfers the same way; a page
	 * mapped without execute permission holds no translation to retire. */
	constexpr auto data_base = code_base + 0x10'0000;
	JitBacking data{1};
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(data_base),
		data.descriptor(), 0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	const auto code_changes = darwin_uml_static_vm_code_changes(generation);
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, data_base, DARWIN_UML_GUEST_PAGE_SIZE) == 0);
	require(darwin_uml_static_vm_code_changes(generation) == code_changes);
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, code_base, 4) == 0);
	require(darwin_uml_static_vm_code_changes(generation) != code_changes);

	/* The same two writes with the frame named, as ptrace and
	 * process_vm_writev() make them: the data page publishes nothing, and
	 * the code page changes. */
	const auto named_changes = darwin_uml_static_vm_code_changes(generation);
	require(darwin_uml_static_vm_invalidate_code_frame(generation, data_base,
		DARWIN_UML_GUEST_PAGE_SIZE, data.descriptor(), 0) == 0);
	require(darwin_uml_static_vm_code_changes(generation) == named_changes);
	backing.put32(0, move_immediate(0, 3));
	require(darwin_uml_static_vm_invalidate_code_frame(generation, code_base, 4,
		backing.descriptor(), 0) == 0);
	require(darwin_uml_static_vm_code_changes(generation) != named_changes);
	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 3);
}

/* A code change resets every activation, not only the ones it touched.
 *
 * The guest code-change sequence carries no range, so a change anywhere has to
 * be treated as a change everywhere. This invalidates a page that holds none of
 * the executed code and leaves every word intact, then requires the successor
 * link to have been dropped all the same: the linked run must fall back to
 * exiting at its successor instead of continuing into it.
 *
 * The reset walks only the records that hold an activation rather than every
 * record ever created, so this also pins that the narrower walk still reaches a
 * record the change did not name.
 */
void an_unrelated_code_change_resets_an_untouched_link()
{
	constexpr unsigned long generation = 922;
	constexpr auto target = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, branch(code_base, target));
	backing.put32(target - code_base, move_immediate(0, 5));
	backing.put32(target - code_base + 4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	arm64::CpuState cpu{};
	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == 5);

	darwin_uml_static_vm_invalidate_code(
		code_base + DARWIN_UML_GUEST_PAGE_SIZE);
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(executor, memory, generation, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	/* The words never changed, so both blocks revalidate and relink rather
	 * than retranslating.
	 */
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(executor, memory, generation, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == 5);
	const auto diagnostics = executor.cache_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_hits == diagnostics.validation_attempts);
	require(diagnostics.validation_mismatches == 0);
}

/* Retiring the least recently current address space drops its translations and
 * keeps everybody else's.
 *
 * Retirement walks the slot list the retired address space kept while it was
 * storing rather than the whole cache, so a slot it names but no longer owns
 * must be left alone, and a slot it owns must not be missed. Running one more
 * address space than the residency cap forces exactly one retirement, and the
 * surviving spaces then have to answer from the cache while the retired one has
 * to translate again.
 */
void retiring_an_address_space_keeps_the_others()
{
	constexpr unsigned long first = 940;
	constexpr auto resident = UML_GADGET_RESIDENT_GENERATIONS;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);

	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	auto memory = std::make_unique<StaticGuestMemory>();
	std::vector<std::unique_ptr<JitMapping>> mappings;
	const auto visit = [&](unsigned long generation) {
		arm64::CpuState cpu{};
		cpu.pc = code_base;
		const auto outcome =
			run_once(*executor, *memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 7);
	};

	/* Fill residency exactly, then run them all again: everything is a hit,
	 * so nothing has been retired yet. */
	for (unsigned long index = 0; index < resident; ++index) {
		mappings.push_back(std::make_unique<JitMapping>(
			first + index, code_base, backing, 1));
		visit(first + index);
	}
	const auto after_fill = executor->cache_diagnostics().translations;
	require(after_fill == resident);
	for (unsigned long index = 0; index < resident; ++index)
		visit(first + index);
	require(executor->cache_diagnostics().translations == after_fill);

	/* One more address space retires the least recently current, which is
	 * the one that has been idle longest -- `first`. */
	mappings.push_back(std::make_unique<JitMapping>(
		first + resident, code_base, backing, 1));
	visit(first + resident);
	require(executor->cache_diagnostics().translations == after_fill + 1);

	/* Every space except the retired one still answers from the cache. */
	for (unsigned long index = 1; index < resident; ++index)
		visit(first + index);
	require(executor->cache_diagnostics().translations == after_fill + 1);

	/* The retired one has to be translated again, and retires another. */
	visit(first);
	require(executor->cache_diagnostics().translations == after_fill + 2);
}

/* A block that stopped at a word the builder does not translate keeps where
 * it stopped in a tail of its stored body, and an empty one reports that
 * address as the illegal exit's PC. Both come back from the cache the same as
 * from the build that stored them. */
void a_stopped_block_keeps_its_stop_in_the_cache()
{
	constexpr unsigned long generation = 1300;
	/* An exception-class word no allocation gives a meaning: the builder
	 * stops before it without retiring it. */
	constexpr std::uint32_t unimplemented = 0xd400'0000U;
	constexpr auto stopped = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, unimplemented);
	backing.put32(stopped - code_base, move_immediate(0, 7));
	backing.put32(stopped - code_base + 4, unimplemented);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	for (unsigned pass = 0; pass < 3; ++pass) {
		arm64::CpuState empty{};
		empty.pc = code_base;
		require(run_once(executor, memory, generation, empty).event ==
			::uml::cpu::EngineEvent::illegal_instruction);
		require(empty.pc == code_base);

		arm64::CpuState cpu{};
		cpu.pc = stopped;
		require(run_once(executor, memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::illegal_instruction);
		require(cpu.pc == stopped + 4);
		require(cpu.registers[0] == 7);
		require(executor.cache_diagnostics().translations == 2);
	}
}

/* A slot names its address space by an id the cache hands out and takes back
 * when the space is retired, so the id of a retired space is the id of the
 * next to arrive. Every translation of the retired space must be gone by then
 * however many times the ids go round, or the newcomer would run the old
 * space's code at the same PC.
 *
 * Each space maps code of its own at the same address, and the visits cycle
 * through more spaces than are resident, so every visit retires one space and
 * reuses its id.
 */
void a_recycled_address_space_id_serves_no_old_translation()
{
	constexpr unsigned long first = 1100;
	constexpr auto resident = UML_GADGET_RESIDENT_GENERATIONS;
	constexpr unsigned long spaces = 3 * resident + 1;
	std::vector<std::unique_ptr<JitBacking>> backings;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (unsigned long index = 0; index < spaces; ++index) {
		backings.push_back(std::make_unique<JitBacking>(1));
		backings.back()->put32(0, move_immediate(0, 100 + index));
		backings.back()->put32(4, system_call);
		mappings.push_back(std::make_unique<JitMapping>(
			first + index, code_base, *backings.back(), 1));
	}
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	std::size_t translations = 0;
	for (unsigned pass = 0; pass < 3; ++pass) {
		for (unsigned long index = 0; index < spaces; ++index) {
			arm64::CpuState cpu{};
			cpu.pc = code_base;
			require(run_once(executor, memory, first + index, cpu)
					.event == ::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == 100 + index);
			/* Cycling through more spaces than the cache keeps, each
			 * visit finds its space gone. */
			require(executor.cache_diagnostics().translations ==
				++translations);
			cpu = {};
			cpu.pc = code_base;
			require(run_once(executor, memory, first + index, cpu)
					.event == ::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == 100 + index);
			require(executor.cache_diagnostics().translations ==
				translations);
		}
	}
}

/* A slot is current when the epoch it carries is its space's, and the epoch
 * counts code changes in sixteen bits. A slot the space has not run for a whole
 * epoch cycle must not be taken for current when the count comes round to its
 * own again: the cache clears the epochs a wrap would repeat.
 *
 * Block A is run once and its word rewritten behind the cache's back, which
 * moves no code-change sequence. Block B then runs through every one of the
 * changes that returns the epoch to A's, and A, never run in between, must be
 * validated against the guest's words and rebuilt instead of served stale.
 */
void a_validation_does_not_outlive_the_epochs_it_counts()
{
	constexpr unsigned long generation = 1200;
	constexpr auto other = code_base + 0x100;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);
	backing.put32(other - code_base, move_immediate(1, 1));
	backing.put32(other - code_base + 4, system_call);
	JitMapping mapping{generation, code_base, backing, 1};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 7);

	write_mapped32(generation, code_base, move_immediate(0, 9));
	constexpr unsigned changes = 65'535;
	for (unsigned change = 0; change < changes; ++change) {
		darwin_uml_static_vm_invalidate_code(
			code_base + DARWIN_UML_GUEST_PAGE_SIZE);
		cpu = {};
		cpu.pc = other;
		require(run_once(executor, memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::system_call);
	}

	const auto before = executor.cache_diagnostics();
	cpu = {};
	cpu.pc = code_base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 9);
	const auto after = executor.cache_diagnostics();
	require(after.validation_attempts == before.validation_attempts + 1);
	require(after.validation_mismatches == before.validation_mismatches + 1);
	require(after.translations == before.translations + 1);
}

/* Mirrors ProgramCache::locate(): the set a translation lands in folds the
 * address space into the index, so the only way to aim several address spaces
 * at one set of ways from outside is to choose generations that collide there.
 */
constexpr std::size_t cache_ways = ::uml::cpu::gadget_cache::ways;
constexpr std::size_t cache_sets = UML_GADGET_CACHE_SLOTS / cache_ways;

[[nodiscard]] constexpr std::size_t cache_set(
	unsigned long generation, std::uint64_t pc) noexcept
{
	const auto block = (pc >> 2) ^
		(static_cast<std::uint64_t>(generation) *
			0x9e37'79b9'7f4a'7c15ULL);
	const auto mixed = block ^ (block >> 12) ^ (block >> 24);
	return static_cast<std::size_t>(mixed) & (cache_sets - 1);
}

/* A slot handed to another address space must not keep the previous owner's
 * link record.
 *
 * A slot memoises the record its translation publishes into, and all
 * activate() can ask of that memo is the record's guest PC -- which does
 * not change when the slot is taken by a *different*
 * address space at the same guest PC. Reusing it there publishes the new
 * occupant's translation into the previous owner's record, so the previous
 * owner's own code branches straight into another address space's
 * translation, against G160; and once that owner retires, the record its map
 * held is destroyed while the slot still names it, which is a live executor
 * dereferencing freed storage on its next activation.
 *
 * Two address spaces are chosen so that one guest PC lands in the same set in
 * both, and so that the predecessor block lands elsewhere and survives the set
 * filling up. The owner stores its successor there first; the other address
 * space fills the remaining ways with blocks of its own and then stores at the
 * same guest PC, taking the owner's way, the set's round-robin replacement
 * having come back around to it. The owner then has to find its successor link
 * reset rather than pointing at the other's translation.
 */
void a_replaced_slot_drops_the_previous_owner_link()
{
	constexpr auto target = code_base + 0x100;
	constexpr unsigned owner_value = 7;
	constexpr unsigned other_value = 9;

	/* Both have to stay resident: retiring the owner would release its
	 * slots and destroy its records, which is the sequence this is here to
	 * keep unreachable. */
	static_assert(UML_GADGET_RESIDENT_GENERATIONS >= 2);

	unsigned long owner = 1000;
	while (cache_set(owner, target) == cache_set(owner, code_base))
		++owner;
	const auto set = cache_set(owner, target);
	unsigned long other = owner + 1;
	while (cache_set(other, target) != set) {
		++other;
		require(other < 8'000'000UL);
	}

	/* The other address space's own blocks for the rest of the set, each a
	 * move and a call two words long, apart from each other and from the
	 * successor. */
	std::vector<std::uint64_t> fillers;
	for (auto pc = target + 2 * sizeof(std::uint32_t);
	     fillers.size() + 1 < cache_ways; pc += sizeof(std::uint32_t)) {
		require(pc - code_base < (std::uint64_t{1} << 26));
		if (cache_set(other, pc) == set) {
			fillers.push_back(pc);
			pc += sizeof(std::uint32_t);
		}
	}
	const auto extent = (fillers.empty() ? target : fillers.back()) +
		2 * sizeof(std::uint32_t) - code_base;
	const std::size_t pages = (extent + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE;

	JitBacking owner_backing{1};
	owner_backing.put32(0, branch(code_base, target));
	owner_backing.put32(target - code_base, move_immediate(0, owner_value));
	owner_backing.put32(target - code_base + 4, system_call);
	JitBacking other_backing{pages};
	other_backing.put32(target - code_base, move_immediate(0, other_value));
	other_backing.put32(target - code_base + 4, system_call);
	for (const auto pc : fillers) {
		other_backing.put32(pc - code_base, move_immediate(0, other_value));
		other_backing.put32(pc - code_base + 4, system_call);
	}

	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	auto memory = std::make_unique<StaticGuestMemory>();
	std::vector<std::unique_ptr<JitMapping>> mappings;
	mappings.push_back(std::make_unique<JitMapping>(
		owner, code_base, owner_backing, 1));
	mappings.push_back(std::make_unique<JitMapping>(
		other, code_base, other_backing, pages));

	const auto run_at = [&](unsigned long generation, std::uint64_t pc,
				unsigned value) {
		arm64::CpuState cpu{};
		cpu.pc = pc;
		const auto outcome =
			run_once(*executor, *memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == value);
	};
	const auto run_successor = [&](unsigned long generation, unsigned value) {
		run_at(generation, target, value);
	};

	/* The owner translates its successor and the predecessor that links to
	 * it, and the live link carries the predecessor straight through. */
	run_successor(owner, owner_value);
	arm64::CpuState cpu{};
	cpu.pc = code_base;
	const auto linked = run_once(*executor, *memory, owner, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 3);
	require(cpu.registers[0] == owner_value);

	/* Fill the remaining ways, then take the owner's. */
	for (const auto pc : fillers)
		run_at(other, pc, other_value);
	run_successor(other, other_value);
	const auto before = executor->cache_diagnostics().translations;
	require(before == cache_ways + 2);

	/* The owner's successor went with its way, so the predecessor has to
	 * exit at the successor address. Continuing here would mean the link
	 * record still names a translation -- and the only translation left at
	 * that address belongs to the address space that took the way. */
	cpu = {};
	cpu.pc = code_base;
	const auto reset_link = run_once(*executor, *memory, owner, cpu);
	require(reset_link.event == ::uml::cpu::EngineEvent::continued);
	require(reset_link.executed == 1);
	require(cpu.pc == target);

	/* Rebuilding the successor is what proves the owner's way really had
	 * been taken, and republishes the owner's own record. */
	const auto rebuilt = run_once(*executor, *memory, owner, cpu);
	require(rebuilt.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == owner_value);
	require(executor->cache_diagnostics().translations == before + 1);

	cpu = {};
	cpu.pc = code_base;
	const auto relinked = run_once(*executor, *memory, owner, cpu);
	require(relinked.event == ::uml::cpu::EngineEvent::system_call);
	require(relinked.executed == 3);
	require(cpu.registers[0] == owner_value);

	/* No guest word and no code-change sequence moved anywhere here, so
	 * nothing revalidated. That is worth pinning: a code change would reset
	 * the owner's links on its own and make the check above pass whatever
	 * the replacement did with the record.
	 */
	require(executor->cache_diagnostics().validation_attempts == 0);
}

/* An evicted block is gone from its slot, and the link that named it is reset.
 *
 * The test above takes a slot across address spaces; this takes one inside a
 * single address space, by capacity. One block more than a set has ways is
 * chosen to share a set, so the last store finds every way occupied and
 * replaces the first one's. The evicted block must then be translated again
 * rather than found in the slot it left, its predecessor must stop following
 * the link to it, and the blocks that kept their ways must still answer from
 * the cache. The block that the re-translation replaces in turn proves that
 * the replacement pointer moved on and that the slot is refilled instead of
 * skipped.
 */
void an_evicted_block_misses_and_its_predecessor_link_is_reset()
{
	constexpr auto target = code_base + 0x100;
	constexpr std::size_t blocks = cache_ways + 1;

	/* The predecessor must not compete for the target's set. */
	unsigned long generation = 960;
	while (cache_set(generation, code_base) == cache_set(generation, target))
		++generation;

	std::vector<std::uint64_t> pcs{target};
	for (std::uint64_t word = 0x80; pcs.size() < blocks; ++word) {
		const auto pc = code_base + word * sizeof(std::uint32_t);
		if (cache_set(generation, pc) == cache_set(generation, target))
			pcs.push_back(pc);
		require(word < (1U << 22));
	}
	const auto extent = pcs.back() + 2 * sizeof(std::uint32_t) - code_base;
	const std::size_t pages = (extent + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE;

	JitBacking backing{pages};
	backing.put32(0, branch(code_base, target));
	for (std::size_t index = 0; index < pcs.size(); ++index) {
		backing.put32(pcs[index] - code_base,
			move_immediate(0, static_cast<unsigned>(index + 1)));
		backing.put32(pcs[index] - code_base + 4, system_call);
	}
	JitMapping mapping{generation, code_base, backing, pages};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	const auto run_block = [&](std::size_t index) {
		arm64::CpuState cpu{};
		cpu.pc = pcs[index];
		const auto outcome = run_once(executor, memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == index + 1);
	};
	const auto translations = [&] {
		return executor.cache_diagnostics().translations;
	};
	const auto run_predecessor = [&] {
		arm64::CpuState cpu{};
		cpu.pc = code_base;
		return std::pair{run_once(executor, memory, generation, cpu), cpu};
	};

	/* The target and its predecessor are translated, and the live link
	 * carries the predecessor straight through. */
	run_block(0);
	require(translations() == 1);
	{
		const auto [linked, cpu] = run_predecessor();
		require(linked.event == ::uml::cpu::EngineEvent::system_call);
		require(linked.executed == 3);
		require(cpu.registers[0] == 1);
	}
	require(translations() == 2);

	/* The next ones fill the set; each is a miss that finds a free way. */
	for (std::size_t index = 1; index < cache_ways; ++index)
		run_block(index);
	require(translations() == 2 + cache_ways - 1);

	/* The last takes the target's way. */
	run_block(cache_ways);
	const auto full = translations();
	require(full == 2 + cache_ways);

	/* The target went with its way: the predecessor has to exit at its
	 * address instead of continuing into a translation that is gone. */
	{
		const auto [reset, cpu] = run_predecessor();
		require(reset.event == ::uml::cpu::EngineEvent::continued);
		require(reset.executed == 1);
		require(cpu.pc == target);
	}

	/* The blocks that kept their ways still answer from the cache. */
	for (std::size_t index = 1; index <= cache_ways; ++index)
		run_block(index);
	require(translations() == full);

	/* The evicted one is translated again, taking the next way in turn,
	 * and the predecessor links to it once more. */
	run_block(0);
	require(translations() == full + 1);
	{
		const auto [relinked, cpu] = run_predecessor();
		require(relinked.event == ::uml::cpu::EngineEvent::system_call);
		require(relinked.executed == 3);
		require(cpu.registers[0] == 1);
	}
	run_block(1);
	require(translations() == full + 2);

	/* Nothing here changed a guest word or the code-change sequence, so
	 * no lookup revalidated: every miss above was an empty or evicted
	 * slot, not a discarded one. */
	require(executor.cache_diagnostics().validation_attempts == 0);
}

/* Address spaces that leave the cache free their translations' storage for
 * translations of any size, and the survivors keep theirs.
 *
 * Retiring an address space frees all its translations at once, and the arena
 * merges what they leave into free blocks that other translations are cut
 * from. Blocks of five lengths that differ between address spaces, enough of
 * them to fill several chunks, more address spaces than the cache keeps, and
 * blocks chosen so that no set ever overflows: then every miss below is a
 * retirement's, the retranslations are cut from holes of other sizes around
 * the survivors' translations, and each block's own value proves which
 * translation ran.
 */
void retired_address_spaces_release_and_the_others_keep_running()
{
	constexpr unsigned long first = 980;
	constexpr auto resident =
		static_cast<std::size_t>(UML_GADGET_RESIDENT_GENERATIONS);
	constexpr std::size_t spaces = resident + 2;
	constexpr std::size_t blocks = 1024;
	/* Instructions a block may hold: up to four moves into x1, the move
	 * into x0 its value is read from, and the call that ends it. */
	constexpr std::size_t longest = 6;
	const auto value = [](std::size_t space, std::size_t block) {
		return static_cast<unsigned>((block * 7U + space * 131U) & 0xffffU);
	};
	const auto moves = [](std::size_t space, std::size_t block) {
		return (block + space) % (longest - 1);
	};

	std::vector<std::size_t> load(cache_sets);
	std::vector<std::uint64_t> pcs;
	for (std::uint64_t pc = code_base; pcs.size() < blocks;) {
		bool fits = true;
		for (std::size_t space = 0; space < spaces; ++space)
			fits = fits && load[cache_set(first + space, pc)] < cache_ways;
		if (!fits) {
			pc += sizeof(std::uint32_t);
			continue;
		}
		for (std::size_t space = 0; space < spaces; ++space)
			++load[cache_set(first + space, pc)];
		pcs.push_back(pc);
		pc += longest * sizeof(std::uint32_t);
	}
	const auto extent =
		pcs.back() + longest * sizeof(std::uint32_t) - code_base;
	const std::size_t pages = (extent + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE;

	std::vector<std::unique_ptr<JitBacking>> backings;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (std::size_t space = 0; space < spaces; ++space) {
		backings.push_back(std::make_unique<JitBacking>(pages));
		for (std::size_t block = 0; block < blocks; ++block) {
			auto at = pcs[block] - code_base;
			for (std::size_t move = 0; move < moves(space, block);
			     ++move, at += sizeof(std::uint32_t))
				backings.back()->put32(at, move_immediate(1,
					static_cast<unsigned>(move)));
			backings.back()->put32(at,
				move_immediate(0, value(space, block)));
			backings.back()->put32(at + sizeof(std::uint32_t),
				system_call);
		}
		mappings.push_back(std::make_unique<JitMapping>(
			first + space, code_base, *backings.back(), pages));
	}

	auto executor = std::make_unique<gadgets::GadgetExecutor>();
	auto memory = std::make_unique<StaticGuestMemory>();
	const auto visit = [&](std::size_t space) {
		for (std::size_t block = 0; block < blocks; ++block) {
			arm64::CpuState cpu{};
			cpu.pc = pcs[block];
			const auto outcome = run_once(
				*executor, *memory, first + space, cpu);
			require(outcome.event ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == value(space, block));
		}
	};
	const auto translations = [&] {
		return executor->cache_diagnostics().translations;
	};

	/* In order: the last two retire the first two. */
	for (std::size_t space = 0; space < spaces; ++space)
		visit(space);
	require(translations() == spaces * blocks);

	/* Newest first, so the newest becomes the least recently current:
	 * every resident space still answers from the cache. */
	for (std::size_t space = spaces; space-- > 2;)
		visit(space);
	require(translations() == spaces * blocks);

	/* The retired two come back, retiring the two newest. */
	visit(1);
	visit(0);
	require(translations() == (spaces + 2) * blocks);
	for (std::size_t space = 0; space < resident; ++space)
		visit(space);
	require(translations() == (spaces + 2) * blocks);
	visit(spaces - 1);
	require(translations() == (spaces + 3) * blocks);
	require(executor->cache_diagnostics().validation_attempts == 0);
}

[[nodiscard]] constexpr std::uint32_t branch_register(unsigned source) noexcept
{
	return 0xd61f'0000U | ((source & 31U) << 5U);
}

/* A link sweep releases the records only evicted blocks named, and keeps
 * every one something still reaches.
 *
 * Thousands of one-branch blocks are crowded into a few sets, so most are
 * evicted soon after they are stored, and with them the only name for their
 * own record and their target's. Those records pile up past the sweep floor
 * and a store sweeps them. Meanwhile an indirect site that stays resident is
 * trained on targets whose blocks are long gone, so its cells and the target
 * table name records nothing else does: the sweep releases those too, and
 * must empty the cells, because the site is asked for the same target again
 * right after. Throughout, a resident pair whose link was armed before any
 * sweep keeps following it, and a resident block nothing links to keeps its
 * own record armed: a hit does not activate, so a block translated after the
 * sweeps can only link to it through that record. No guest word and no
 * code-change sequence moves, so nothing but the sweep resets a link.
 */
void a_link_sweep_releases_what_only_evicted_blocks_named()
{
	constexpr unsigned long generation = 980;
	constexpr std::size_t crowded_sets = 256;
	constexpr std::size_t blocks = 4000;
	constexpr std::size_t lag = 1000;
	constexpr auto keeper = code_base + 0x100;
	constexpr auto predecessor = code_base + 0x200;
	constexpr auto site = code_base + 0x300;
	constexpr auto loner = code_base + 0x400;
	constexpr auto late = code_base + 0x500;
	constexpr std::uint64_t crowd_base = code_base + 0x10000;

	const auto crowded = [&](std::uint64_t pc) {
		return cache_set(generation, pc) % (cache_sets / crowded_sets) == 0;
	};
	for (const auto pc : {keeper, predecessor, site, loner, late})
		require(!crowded(pc));
	std::vector<std::uint64_t> pcs;
	for (auto pc = crowd_base; pcs.size() < blocks; pc += 8)
		if (crowded(pc))
			pcs.push_back(pc);
	const auto extent = pcs.back() + 8 - code_base;
	const std::size_t pages = (extent + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE;

	/* Each crowded block branches to the word behind it, which no block
	 * starts at: its target is never translated. */
	JitBacking backing{pages};
	backing.put32(keeper - code_base, move_immediate(0, 77));
	backing.put32(keeper - code_base + 4, system_call);
	backing.put32(predecessor - code_base, branch(predecessor, keeper));
	backing.put32(site - code_base, branch_register(1));
	backing.put32(loner - code_base, move_immediate(0, 78));
	backing.put32(loner - code_base + 4, system_call);
	backing.put32(late - code_base, branch(late, loner));
	for (const auto pc : pcs)
		backing.put32(pc - code_base, branch(pc, pc + 4));
	JitMapping mapping{generation, code_base, backing, pages};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;

	const auto run = [&](std::uint64_t pc, std::uint64_t x1 = 0) {
		arm64::CpuState cpu{};
		cpu.pc = pc;
		cpu.registers[1] = x1;
		const auto outcome = run_once(executor, memory, generation, cpu);
		return std::pair{outcome, cpu};
	};
	const auto linked_pair = [&] {
		const auto [outcome, cpu] = run(predecessor);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(outcome.executed == 3);
		require(cpu.registers[0] == 77);
	};
	const auto indirect = [&](std::uint64_t target) {
		const auto [outcome, cpu] = run(site, target);
		require(outcome.event == ::uml::cpu::EngineEvent::continued);
		require(cpu.pc == target);
	};

	require(run(keeper).first.event == ::uml::cpu::EngineEvent::system_call);
	require(run(loner).first.event == ::uml::cpu::EngineEvent::system_call);
	linked_pair();
	for (std::size_t index = 0; index < blocks; ++index) {
		const auto [outcome, cpu] = run(pcs[index]);
		require(outcome.event == ::uml::cpu::EngineEvent::continued);
		require(outcome.executed == 1);
		require(cpu.pc == pcs[index] + 4);
		if (index <= lag)
			continue;
		/* Asked again for the target it learned last round, which a
		 * sweep in this round's store may have released. */
		indirect(pcs[index - lag - 1] + 4);
		indirect(pcs[index - lag] + 4);
	}
	linked_pair();
	{
		const auto [outcome, cpu] = run(late);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call);
		require(outcome.executed == 3);
		require(cpu.registers[0] == 78);
	}

	const auto &diagnostics = executor.cache_diagnostics();
	require(diagnostics.link_sweeps != 0);
	require(diagnostics.released_links >= blocks / 2);
	require(diagnostics.released_sites == 0);
	require(diagnostics.validation_attempts == 0);
}

/* Only a block that leaves the cache can leave a record nothing names, so an
 * address space whose blocks all stay resident never sweeps, however many
 * records it makes. */
void an_address_space_that_only_grows_does_not_sweep()
{
	constexpr unsigned long generation = 981;
	constexpr std::size_t blocks = 6000;
	constexpr std::uint64_t base = code_base + 0x10000;

	/* One block per set, so no store evicts another. */
	std::vector<std::uint64_t> pcs;
	std::vector<bool> taken(cache_sets);
	for (auto pc = base; pcs.size() < blocks; pc += 8) {
		const auto set = cache_set(generation, pc);
		if (taken[set])
			continue;
		taken[set] = true;
		pcs.push_back(pc);
	}
	const auto extent = pcs.back() + 8 - code_base;
	const std::size_t pages = (extent + DARWIN_UML_GUEST_PAGE_SIZE - 1) /
		DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{pages};
	for (const auto pc : pcs)
		backing.put32(pc - code_base, branch(pc, pc + 4));
	JitMapping mapping{generation, code_base, backing, pages};
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
	for (const auto pc : pcs) {
		arm64::CpuState cpu{};
		cpu.pc = pc;
		const auto outcome = run_once(executor, memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued);
		require(cpu.pc == pc + 4);
	}
	/* Each block made its own record and its target's. */
	require(executor.cache_diagnostics().translations == blocks);
	require(executor.cache_diagnostics().link_sweeps == 0);
}

/* An A64 key at or above the guest address limit (2^47) has no compact tag
 * form, so no slot can name it: its translation runs once, uncached, and takes
 * no way from the set it hashes to. A guest's own mappings stay below the
 * limit, but the static MMU admits code above it to a direct caller, so the
 * cache has to meet such a key with every slot's owner id still one an address
 * space holds. A slot that carried one would make a lookup of another such key
 * run the wrong program, and its eviction, a code change's sweep and the
 * cache's teardown read the owner table past its end. */
constexpr std::uint64_t uncacheable_pc = DARWIN_UML_GUEST_ADDRESS_LIMIT;
static_assert((uncacheable_pc >> 47) != 0);

/* Whether the pass run_all() is in lets executors share translations: main()
 * flips DARWIN_UML_GADGET_SHARED_BODIES between its two. */
bool sharing_pass = false;

/* `mov x0, #value; svc #0` at `offset`. */
void put_block(JitBacking &backing, std::uint64_t offset, std::uint32_t value)
{
	backing.put32(offset, move_immediate(0, value));
	backing.put32(offset + 4, system_call);
}

[[nodiscard]] std::uint64_t run_block_at(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, std::uint64_t pc)
{
	arm64::CpuState cpu{};
	cpu.pc = pc;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	return cpu.registers[0];
}

/* The first page-aligned address from `from` whose block lands in `set`. */
[[nodiscard]] std::uint64_t uncacheable_in_set(unsigned long generation,
	std::size_t set, std::uint64_t from)
{
	auto pc = from;
	while (cache_set(generation, pc) != set) {
		pc += DARWIN_UML_GUEST_PAGE_SIZE;
		require(pc - from < (std::uint64_t{1} << 36));
	}
	return pc;
}

/* The rows below stay out of line: an executor is large, and rows
 * inlined into run_all() would add their frames to its, past the main thread's
 * stack under the sanitizers. */

/* Two uncacheable blocks in one set each run their own code, however often
 * they are entered: neither is ever found in a slot the other left. A block
 * that stopped at a word the builder does not translate, and an empty one,
 * report their stop as a stored block does. */
[[gnu::noinline]] void uncacheable_blocks_in_one_set_run_their_own_code()
{
	constexpr unsigned long generation = 990;
	const auto set = cache_set(generation, uncacheable_pc);
	const auto other_pc = uncacheable_in_set(generation, set,
		uncacheable_pc + DARWIN_UML_GUEST_PAGE_SIZE);

	/* An exception-class word no allocation gives a meaning: the builder
	 * stops before it without retiring it. */
	constexpr std::uint32_t unimplemented = 0xd400'0000U;
	const auto stopped_pc = other_pc + 0x100;
	const auto empty_pc = other_pc + 0x200;

	JitBacking anchor{1}, first{1}, second{1};
	put_block(first, 0, 11);
	put_block(second, 0, 22);
	second.put32(0x100, move_immediate(0, 7));
	second.put32(0x104, unimplemented);
	second.put32(0x200, unimplemented);
	JitMapping mapping{generation, code_base, anchor, 1};
	const uml::emulator::tests::GenerationMapping first_mapping{generation,
		uncacheable_pc, first, 1, PROT_READ | PROT_EXEC, true};
	const uml::emulator::tests::GenerationMapping second_mapping{generation,
		other_pc, second, 1, PROT_READ | PROT_EXEC, true};

	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor{sharing_options()};
	for (int round = 0; round < 2; ++round) {
		require(run_block_at(executor, memory, generation,
			uncacheable_pc) == 11);
		require(run_block_at(executor, memory, generation, other_pc) == 22);

		arm64::CpuState stopped{};
		stopped.pc = stopped_pc;
		require(run_once(executor, memory, generation, stopped).event ==
			::uml::cpu::EngineEvent::illegal_instruction);
		require(stopped.pc == stopped_pc + 4);
		require(stopped.registers[0] == 7);
		arm64::CpuState empty{};
		empty.pc = empty_pc;
		require(run_once(executor, memory, generation, empty).event ==
			::uml::cpu::EngineEvent::illegal_instruction);
		require(empty.pc == empty_pc);
	}
	/* Nothing was stored, so every entry translated. */
	require(executor.cache_diagnostics().translations == 8);
	require(executor.cache_diagnostics().validation_attempts == 0);
}

/* A second executor of the address space finds no translation of an
 * uncacheable block to adopt, though it adopts a cacheable one where
 * translations are shared. */
[[gnu::noinline]] void a_peer_executor_does_not_adopt_an_uncacheable_block()
{
	constexpr unsigned long generation = 991;
	JitBacking anchor{1}, uncached{1};
	put_block(anchor, 0, 33);
	put_block(uncached, 0, 11);
	JitMapping mapping{generation, code_base, anchor, 1};
	const uml::emulator::tests::GenerationMapping uncached_mapping{generation,
		uncacheable_pc, uncached, 1, PROT_READ | PROT_EXEC, true};

	StaticGuestMemory memory, peer_memory;
	gadgets::GadgetExecutor executor{sharing_options()};
	gadgets::GadgetExecutor peer{sharing_options()};
	require(run_block_at(executor, memory, generation, uncacheable_pc) == 11);
	require(run_block_at(peer, peer_memory, generation, uncacheable_pc) == 11);
	require(peer.cache_diagnostics().adoptions == 0);
	require(peer.cache_diagnostics().translations == 1);

	/* The control: the peer does adopt a block that can be cached. */
	require(run_block_at(executor, memory, generation, code_base) == 33);
	require(run_block_at(peer, peer_memory, generation, code_base) == 33);
	require(peer.cache_diagnostics().adoptions == (sharing_pass ? 1U : 0U));
	require(peer.cache_diagnostics().translations ==
		(sharing_pass ? 1U : 2U));
}

/* An uncacheable block takes no way from its set: a set's worth of cacheable
 * blocks fits beside it without one of them being translated again, and the
 * block after them evicts one of those, whose owner is an address space the
 * cache holds. */
[[gnu::noinline]] void an_uncacheable_block_takes_no_way_from_its_set()
{
	constexpr unsigned long generation = 992;
	const auto set = cache_set(generation, uncacheable_pc);
	std::vector<std::uint64_t> pcs;
	for (auto pc = code_base; pcs.size() < cache_ways + 1;
	     pc += 2 * sizeof(std::uint32_t))
		if (cache_set(generation, pc) == set)
			pcs.push_back(pc);
	const std::size_t pages = (pcs.back() + 8 - code_base +
		DARWIN_UML_GUEST_PAGE_SIZE - 1) / DARWIN_UML_GUEST_PAGE_SIZE;

	JitBacking low{pages}, uncached{1};
	for (const auto pc : pcs)
		put_block(low, pc - code_base, 33);
	put_block(uncached, 0, 11);
	JitMapping mapping{generation, code_base, low, pages};
	const uml::emulator::tests::GenerationMapping uncached_mapping{generation,
		uncacheable_pc, uncached, 1, PROT_READ | PROT_EXEC, true};

	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor{sharing_options()};
	require(run_block_at(executor, memory, generation, uncacheable_pc) == 11);
	for (std::size_t index = 0; index < cache_ways; ++index)
		require(run_block_at(executor, memory, generation, pcs[index]) ==
			33);
	require(executor.cache_diagnostics().translations == 1 + cache_ways);
	for (std::size_t index = 0; index < cache_ways; ++index)
		require(run_block_at(executor, memory, generation, pcs[index]) ==
			33);
	require(executor.cache_diagnostics().translations == 1 + cache_ways);

	require(run_block_at(executor, memory, generation, pcs[cache_ways]) ==
		33);
	require(executor.cache_diagnostics().translations ==
		2 + cache_ways);
	require(run_block_at(executor, memory, generation, uncacheable_pc) == 11);
	for (const auto pc : pcs)
		require(run_block_at(executor, memory, generation, pc) == 33);
}

/* A cache that ran an uncacheable block ends with no slot to walk for it. */
[[gnu::noinline]] void a_cache_that_ran_an_uncacheable_block_tears_down()
{
	constexpr unsigned long generation = 993;
	JitBacking anchor{1}, uncached{1};
	put_block(uncached, 0, 11);
	JitMapping mapping{generation, code_base, anchor, 1};
	const uml::emulator::tests::GenerationMapping uncached_mapping{generation,
		uncacheable_pc, uncached, 1, PROT_READ | PROT_EXEC, true};
	StaticGuestMemory memory;
	{
		gadgets::GadgetExecutor executor{sharing_options()};
		require(run_block_at(executor, memory, generation,
			uncacheable_pc) == 11);
	}
}

/* An uncacheable block is built unlinked, so it leaves no link record or
 * indirect site in its address space. Nothing would collect them: a sweep
 * follows a block's departure from a slot, and the budget is relieved by a
 * store. A direct exit and an indirect one each leave the block with its PC and
 * its one instruction retired, and a thousand distinct entries leave the
 * address space's charged bytes as they were. */
[[gnu::noinline]] void uncacheable_blocks_leave_no_link_state(bool za)
{
	const unsigned long generation = za ? 995 : 994;
	constexpr std::size_t entries = 1024;
	constexpr std::uint32_t nop = 0xd503'201fU;
	const std::uint64_t exits = uncacheable_pc;
	const std::uint64_t direct = exits;
	const std::uint64_t indirect = exits + 0x100;
	const std::uint64_t target = exits + 0x200;
	const std::uint64_t run_base = exits + DARWIN_UML_GUEST_PAGE_SIZE;

	JitBacking anchor{1}, code{1}, nops{1};
	code.put32(direct - exits, branch(direct, target));
	code.put32(indirect - exits, branch_register(1));
	put_block(code, target - exits, 5);
	for (std::size_t index = 0; index < entries; ++index)
		nops.put32(index * sizeof(std::uint32_t), nop);
	JitMapping mapping{generation, code_base, anchor, 1};
	const uml::emulator::tests::GenerationMapping code_mapping{generation,
		exits, code, 1, PROT_READ | PROT_EXEC, true};
	const uml::emulator::tests::GenerationMapping nop_mapping{generation,
		run_base, nops, 1, PROT_READ | PROT_EXEC, true};

	/* ZA mode runs the same code through the ZA-mode cache, whose blocks are
	 * built by their own path; any host runs it through an injected SME
	 * profile. */
	auto options = sharing_options();
	if (za) {
		for (const auto feature : {uml::cpu::ArmFeature::sme,
			     uml::cpu::ArmFeature::sme_f64f64,
			     uml::cpu::ArmFeature::sme_i16i64})
			options.host_capabilities.supported.set(
				static_cast<std::size_t>(feature));
		options.host_capabilities.sme_svl_bytes = 64;
		options.host_capabilities.sme_hardware = false;
	}
	const auto file = arm64::SmeRegisterFile::for_host(
		options.host_capabilities);
	std::vector<std::byte> task(za ? arm64::sme_storage_size(file.svl()) : 0);
	require(!za || file.data() != nullptr);
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor{options};
	const auto cpu = std::make_unique<arm64::CpuState>();
	const auto enter = [&](std::uint64_t pc, std::uint64_t x1 = 0) {
		*cpu = {};
		if (za) {
			cpu->bind_sme_registers(file);
			cpu->defer_sme_load(task.data(), arm64::svcr_za, 0);
		}
		cpu->pc = pc;
		cpu->registers[1] = x1;
		return run_once(executor, memory, generation, *cpu);
	};

	require(enter(target).event == ::uml::cpu::EngineEvent::system_call);
	require(cpu->registers[0] == 5);
	require(cpu->svcr == (za ? arm64::svcr_za : 0));
	const auto charged = ::uml::cpu::gadget_cache::ledger_state().total;

	const auto direct_exit = enter(direct);
	require(direct_exit.event == ::uml::cpu::EngineEvent::continued);
	require(direct_exit.executed == 1);
	require(cpu->pc == target);

	const auto indirect_exit = enter(indirect, target);
	require(indirect_exit.event == ::uml::cpu::EngineEvent::continued);
	require(indirect_exit.executed == 1);
	require(cpu->pc == target);

	for (std::size_t index = 0; index < entries; ++index)
		require(enter(run_base + index * sizeof(std::uint32_t)).executed !=
			0);
	require(::uml::cpu::gadget_cache::ledger_state().total == charged);
}

/* The same cases with each address space's translations private to its
 * executor and then shared among them: the cache's slots and bodies keep
 * their own packing in both. */
void run_all()
{
	unchanged_words_avoid_retranslation();
	changed_words_rebuild_and_republish_links();
	a_kernel_text_write_republishes_through_the_bridge();
	an_unrelated_code_change_resets_an_untouched_link();
	retiring_an_address_space_keeps_the_others();
	a_stopped_block_keeps_its_stop_in_the_cache();
	a_recycled_address_space_id_serves_no_old_translation();
	a_validation_does_not_outlive_the_epochs_it_counts();
	a_replaced_slot_drops_the_previous_owner_link();
	an_evicted_block_misses_and_its_predecessor_link_is_reset();
	retired_address_spaces_release_and_the_others_keep_running();
	uncacheable_blocks_in_one_set_run_their_own_code();
	a_peer_executor_does_not_adopt_an_uncacheable_block();
	an_uncacheable_block_takes_no_way_from_its_set();
	a_cache_that_ran_an_uncacheable_block_tears_down();
	uncacheable_blocks_leave_no_link_state(false);
	uncacheable_blocks_leave_no_link_state(true);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1) == 0);
	sharing_pass = false;
	run_all();
	/* A shared address space is never swept, so the sweep's cases run
	 * with private translations only (arm64_gadget_shared_bodies_tests
	 * checks the shared skip). */
	a_link_sweep_releases_what_only_evicted_blocks_named();
	an_address_space_that_only_grows_does_not_sweep();
	/* Sharing is the default. */
	require(unsetenv("DARWIN_UML_GADGET_SHARED_BODIES") == 0);
	sharing_pass = true;
	run_all();
	return 0;
}
