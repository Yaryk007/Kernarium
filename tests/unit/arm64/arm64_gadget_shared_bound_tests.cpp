// SPDX-License-Identifier: GPL-2.0-or-later
/* A shared address space bounds the freed arena it accumulates. Once the arena
 * holds at least as many bytes no body uses as its bodies do, the space is
 * closed: it leaves the registry, every executor attached lets go of it at the
 * start of its next entry into translated code, and the next to want the
 * address space gets a new one, so the old one dies with its last executor and
 * takes its arena, its link records and its freed bodies with it.
 *
 * These tests run against the unit-test variant of the gadget executor
 * (uml_arm64_gadget_sweep_stress_executor), which renews on a schedule and lets
 * a test lower the floor and read what a space holds. */
#include "allocation_failure.hpp"
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/engine.hpp"
#include "cpu/gadget_cache_budget.hpp"
#include "static_mmu_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace stress = gadgets::stress;
namespace cache = uml::cpu::gadget_cache;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

/* `blocks` blocks of `width` instructions, each a run of moves ending in a
 * branch to the next, then a system call. */
void put_chain(JitBacking &backing, std::size_t blocks,
	std::size_t width = 1)
{
	for (std::size_t block = 0; block < blocks; ++block) {
		const auto first = block * width * 4;
		for (std::size_t move = 0; move + 1 < width; ++move)
			backing.put32(first + move * 4, move_immediate(2, 7));
		backing.put32(first + (width - 1) * 4,
			branch(code_base + first + (width - 1) * 4,
				code_base + first + width * 4));
	}
	backing.put32(blocks * width * 4, system_call);
}

/* The unit-test variant of the executor holds 1024 translations, 128 sets of
 * eight, so a few thousand blocks of another address space evict a space's
 * slots.
 *
 * The address space counts the bytes of its arena and of the bodies some slot
 * holds, which is what decides when it is renewed. */
void held_bytes_follow_the_slots()
{
	constexpr unsigned long generation = 980;
	constexpr std::size_t blocks = 700;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	const auto vcpu = std::make_unique<Vcpu>();
	auto &memory = vcpu->memory;
	auto &executor = vcpu->executor;
	auto &cpu = vcpu->cpu;

	const auto empty = stress::open_space_bytes(generation);
	require(empty.spaces == 0);
	require(run_to_system_call(executor, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	const auto full = stress::open_space_bytes(generation);
	require(full.spaces == 1);
	/* Every body takes at least its header and a stream word, in sixteen-byte
	 * granules; a few sets overflow and drop theirs. */
	require(full.held >= (blocks - blocks / 4) * 32 && full.held <= full.arena);

	/* Revalidating a block after a code change replaces its body, and the
	 * old one stops being held: the same bytes are held afterwards. */
	backing.put32(0, branch(code_base, code_base + 8));
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, code_base, 4) == 0);
	require(run_to_system_call(executor, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	const auto again = stress::open_space_bytes(generation);
	require(again.spaces == 1 && again.held == full.held);
}

enum class Leave { window, eviction };

/* Executor A fills a space with a long chain of blocks while B holds one of
 * them, then moves on until it has let go of every one of A's: either its
 * window retires the space, or the blocks of another address space take its
 * slots, which stores nothing in the first. What is left is the arena A's
 * bodies took and one body of B's; once that is more than the floor the space
 * is closed, and B, which kept it, lets go of it at its next entry and
 * translates its block again. Under a floor the arena does not reach, B keeps
 * its slot. */
void a_mostly_free_space_is_renewed(unsigned long generation, Leave leave,
	bool renewed)
{
	constexpr std::size_t blocks = 700;
	/* Wide enough that its bodies take more of the arena than the narrow
	 * ones of the blocks that evict them, and odd, so that the blocks spread
	 * over the cache's sets, which are indexed by the address in words. */
	constexpr std::size_t width = 31;
	constexpr std::size_t evicting_blocks = 6000;
	/* The body arena maps its chunks 64 KiB at a time. */
	constexpr std::size_t arena_chunk = std::size_t{64} << 10;
	/* The chain must fill more of them than the floor and B's pins leave. */
	constexpr std::size_t renewal_probe_arena = 4 * arena_chunk;
	/* Fills the space's arena, which is read before the floor is set. */
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks * width + 1) * 4)};
	put_chain(backing, blocks, width);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks * width + 1) * 4)};
	std::vector<std::unique_ptr<JitBacking>> others;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	if (leave == Leave::window) {
		for (unsigned long other = 0;
		     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other) {
			others.push_back(std::make_unique<JitBacking>(1));
			put_program(*others.back(), 1, 2);
			mappings.push_back(std::make_unique<JitMapping>(
				generation + 1 + other, code_base, *others.back(), 1));
		}
	} else {
		others.push_back(std::make_unique<JitBacking>(
			pages_for((evicting_blocks + 1) * 4)));
		put_chain(*others.back(), evicting_blocks);
		mappings.push_back(std::make_unique<JitMapping>(generation + 1,
			code_base, *others.back(), pages_for((evicting_blocks + 1) * 4)));
	}
	const auto a_vcpu = std::make_unique<Vcpu>();
	const auto b_vcpu = std::make_unique<Vcpu>();
	auto &a = a_vcpu->executor;
	auto &b = b_vcpu->executor;
	auto &a_memory = a_vcpu->memory;
	auto &b_memory = b_vcpu->memory;
	auto &cpu = a_vcpu->cpu;

	require(run_to_system_call(a, a_memory, generation, cpu, code_base,
		blocks + 4) != 0);
	/* B takes the entry block and follows A's links through the rest, which
	 * a run does for a few thousand instructions before it stops and the next
	 * adopts the block it stopped at. */
	require(run_to_system_call(b, b_memory, generation, cpu, code_base,
		blocks) != 0);
	require(b.cache_diagnostics().adoptions >= 1 &&
		b.cache_diagnostics().adoptions < blocks / 16 &&
		b.cache_diagnostics().translations == 0);
	/* The arena returns a chunk of 64 KiB when none of its bodies is live, so
	 * what A leaves behind is the chunks that bodies of B still pin. B holds
	 * a body at the entry and three more spread along the chain, each in a
	 * chunk of its own. */
	for (const auto at : {blocks / 4, blocks / 2, blocks * 3 / 4}) {
		cpu = {};
		cpu.pc = code_base + at * width * 4;
		(void)run_once(b, b_memory, generation, cpu);
	}
	const auto full = stress::open_space_bytes(generation);
	require(full.spaces == 1 && full.held <= full.arena &&
		full.arena >= renewal_probe_arena);
	/* Two chunks of idle arena are more than any other space here has to
	 * spare, and less than this one has once A lets go. */
	stress::set_renewal_floor(renewed ? 2 * arena_chunk :
		std::numeric_limits<std::size_t>::max());
	const auto closes = stress::closes();

	if (leave == Leave::window) {
		/* The window retires the space once no executor is running it;
		 * B stopped, so a pause shows that. */
		let_activity_lapse();
		for (unsigned long other = 0;
		     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other)
			require(run_to_system_call(a, a_memory,
				generation + 1 + other, cpu, code_base) == 4);
	} else {
		require(run_to_system_call(a, a_memory, generation + 1, cpu,
			code_base, evicting_blocks + 4) != 0);
	}
	const auto left = stress::open_space_bytes(generation);
	if (renewed) {
		/* No new executor would be given it. */
		require(left.spaces == 0 && stress::closes() == closes + 1);
	} else {
		require(left.spaces == 1 && stress::closes() == closes);
		require(left.held < full.held / 8);
	}

	const auto translations = b.cache_diagnostics().translations;
	const auto alive = stress::live_spaces();
	cpu = {};
	cpu.pc = code_base;
	const auto outcome = run_once(b, b_memory, generation, cpu);
	if (renewed) {
		/* The old space went with B, its last executor, as the new one came
		 * into being. */
		require(stress::live_spaces() == alive);
		require(b.cache_diagnostics().translations == translations + 1);
		require(outcome.event == ::uml::cpu::EngineEvent::continued &&
			outcome.executed == width);
		const auto fresh = stress::open_space_bytes(generation);
		require(fresh.spaces == 1 && fresh.arena < full.arena / 2 &&
			fresh.held != 0);
	} else {
		require(b.cache_diagnostics().translations == translations);
	}
}

void a_mostly_free_space_is_renewed_and_a_busy_one_is_not()
{
	a_mostly_free_space_is_renewed(1000, Leave::window, true);
	a_mostly_free_space_is_renewed(1020, Leave::window, false);
	/* The space gets no store after A leaves it: its executors let go of
	 * bodies, and the one that did closes it. */
	a_mostly_free_space_is_renewed(1040, Leave::eviction, true);
}

/* What retiring the space frees is what the other executor is running:
 * while B enters it, A's window leaves it, retires the oldest address space
 * nobody entered lately instead, and does not close it, though A's bodies are
 * most of the arena. B goes on through the links A armed and translates
 * nothing. A new address space looks entered until its history is its own, so
 * A fills its window with the spaces to go before the activity lapses, and
 * only B's entries after that keep this one in use. Once B has stopped for a
 * window of activity, A's window retires it as it would any other. */
void a_window_leaves_a_space_another_executor_is_running(
	unsigned long generation)
{
	constexpr std::size_t blocks = 700;
	constexpr std::size_t width = 31;
	constexpr std::size_t arena_chunk = std::size_t{64} << 10;
	constexpr unsigned long places = UML_GADGET_RESIDENT_GENERATIONS;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks * width + 1) * 4)};
	put_chain(backing, blocks, width);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks * width + 1) * 4)};
	OtherSpaces old;
	OtherSpaces fresh;
	OtherSpaces later;
	const auto a_vcpu = std::make_unique<Vcpu>();
	const auto b_vcpu = std::make_unique<Vcpu>();
	auto &a = a_vcpu->executor;
	auto &b = b_vcpu->executor;
	auto &a_memory = a_vcpu->memory;
	auto &b_memory = b_vcpu->memory;
	auto &cpu = a_vcpu->cpu;

	require(run_to_system_call(a, a_memory, generation, cpu, code_base,
		blocks + 4) != 0);
	require(run_to_system_call(b, b_memory, generation, cpu, code_base,
		blocks) != 0);
	require(b.cache_diagnostics().translations == 0);
	/* B holds a body in four chunks of the arena, so that what A lets go of
	 * would leave it mostly free. */
	for (const auto at : {blocks / 4, blocks / 2, blocks * 3 / 4}) {
		cpu = {};
		cpu.pc = code_base + at * width * 4;
		(void)run_once(b, b_memory, generation, cpu);
	}
	const auto full = stress::open_space_bytes(generation);
	require(full.spaces == 1 && full.held <= full.arena &&
		full.arena >= 4 * arena_chunk);
	/* A renewal would follow the retirement, as in the test above. */
	stress::set_renewal_floor(2 * arena_chunk);
	const auto closes = stress::closes();

	/* A's window is full: this space and the ones to go. */
	for (unsigned long other = 1; other < places; ++other) {
		old.add(generation + other);
		require(run_to_system_call(a, a_memory, generation + other,
			cpu, code_base) == 4);
	}
	let_activity_lapse();

	/* A new space is admitted on each turn of A's, and the window goes past
	 * this one every time. */
	auto stamp = activity_stamp();
	cpu = {};
	cpu.pc = code_base;
	(void)run_once(b, b_memory, generation, cpu);
	for (unsigned long other = 1; other < places; ++other) {
		fresh.add(generation + places + other);
		require(run_to_system_call(a, a_memory,
			generation + places + other, cpu, code_base) == 4);
		require_activity_held(stamp);
		/* B enters the space on every turn of A's. */
		stamp = activity_stamp();
		cpu = {};
		cpu.pc = code_base;
		(void)run_once(b, b_memory, generation, cpu);
	}
	const auto kept = stress::open_space_bytes(generation);
	require(kept.spaces == 1 && stress::closes() == closes);
	require(kept.held >= full.held * 9 / 10);
	const auto translations = b.cache_diagnostics().translations;
	const auto adoptions = a.cache_diagnostics().adoptions;
	require(run_to_system_call(b, b_memory, generation, cpu, code_base,
		blocks) != 0);
	require(b.cache_diagnostics().translations == translations);
	/* A still holds its bodies: it finds them in its own slots. */
	require(run_to_system_call(a, a_memory, generation, cpu, code_base,
		blocks + 4) != 0);
	require(a.cache_diagnostics().adoptions == adoptions);

	/* B has gone quiet. A turn of A's through other address spaces lets the
	 * window retire this one, and what it frees closes the space. */
	let_activity_lapse();
	for (unsigned long other = 0; other <= places; ++other) {
		later.add(generation + 2 * places + other);
		require(run_to_system_call(a, a_memory,
			generation + 2 * places + other, cpu, code_base) == 4);
	}
	require(stress::open_space_bytes(generation).spaces == 0 &&
		stress::closes() == closes + 1);
}

/* A window that leaves a running space alone does not keep it from the
 * budget, and when the budget demands the space, relief wins: it asks any open
 * account to shed, the running ones last, and every executor holding a
 * shedding account lets go of it at its next entry whatever its window would
 * do. The window spares a space only while it is neither closed nor being shed
 * (gadget_cache::place_use()), and once the budget has asked, no window takes
 * it back. The state is that of the test above, B having entered the space on
 * every turn of A's through the window. The budget then falls under what the
 * caches hold and a store of A's passes it: the space is asked to shed though
 * it is running. A lets go of it at its next entry, B, which has not entered
 * since, at its own, and the last of them frees it; B then translates its
 * block afresh in a new space, not in the one it let go of. */
void relief_sheds_a_space_the_window_leaves(unsigned long generation)
{
	/* The space is much larger than the little ones of the window's turn, so
	 * that a budget under it is not under what the rest hold. */
	constexpr std::size_t blocks = 2800;
	constexpr std::size_t width = 31;
	constexpr unsigned long places = UML_GADGET_RESIDENT_GENERATIONS;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks * width + 1) * 4)};
	put_chain(backing, blocks, width);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks * width + 1) * 4)};
	OtherSpaces old;
	OtherSpaces fresh;
	OtherSpaces store;
	const auto store_generation = generation + 2 * places;
	const auto a_vcpu = std::make_unique<Vcpu>();
	const auto b_vcpu = std::make_unique<Vcpu>();
	auto &a = a_vcpu->executor;
	auto &b = b_vcpu->executor;
	auto &a_memory = a_vcpu->memory;
	auto &b_memory = b_vcpu->memory;
	auto &cpu = a_vcpu->cpu;

	require(run_to_system_call(a, a_memory, generation, cpu, code_base,
		blocks + 4) != 0);
	require(run_to_system_call(b, b_memory, generation, cpu, code_base,
		blocks) != 0);
	for (const auto at : {blocks / 4, blocks / 2, blocks * 3 / 4}) {
		cpu = {};
		cpu.pc = code_base + at * width * 4;
		(void)run_once(b, b_memory, generation, cpu);
	}
	const auto full = stress::open_space_bytes(generation);
	require(full.spaces == 1);
	const auto closes = stress::closes();
	for (unsigned long other = 1; other < places; ++other) {
		old.add(generation + other);
		require(run_to_system_call(a, a_memory, generation + other,
			cpu, code_base) == 4);
	}
	let_activity_lapse();
	auto stamp = activity_stamp();
	cpu = {};
	cpu.pc = code_base;
	(void)run_once(b, b_memory, generation, cpu);
	for (unsigned long other = 1; other < places; ++other) {
		fresh.add(generation + places + other);
		require(run_to_system_call(a, a_memory,
			generation + places + other, cpu, code_base) == 4);
		require_activity_held(stamp);
		stamp = activity_stamp();
		cpu = {};
		cpu.pc = code_base;
		(void)run_once(b, b_memory, generation, cpu);
	}
	/* The window went past the running space: the blocks of the others
	 * took a few of its slots, not the space. */
	const auto kept = stress::open_space_bytes(generation);
	require(kept.spaces == 1 && kept.held >= full.held * 9 / 10 &&
		stress::closes() == closes);

	/* The budget falls under the space, and A stores past it. */
	store.add(store_generation);
	const auto charged = cache::ledger_state();
	require(charged.shedding == 0 && charged.total > kept.arena);
	cache::set_budget(kept.arena * 3 / 4);
	require(run_to_system_call(a, a_memory, store_generation, cpu,
		code_base) == 4);
	const auto asked = cache::ledger_state();
	require(asked.reliefs > charged.reliefs);
	/* The space, not the little ones, was asked, and B still holds it. */
	require(asked.shed_accounts == charged.shed_accounts + 1 &&
		asked.shedding >= kept.held);
	/* B's next entry lets go of it as the last holder, and the space it then
	 * runs is a new one holding the block B translated in it. */
	cpu = {};
	cpu.pc = code_base;
	(void)run_once(b, b_memory, generation, cpu);
	require(cache::ledger_state().shedding == 0);
	const auto fresh_space = stress::open_space_bytes(generation);
	require(fresh_space.spaces == 1 && fresh_space.held < kept.held / 8);
	cache::set_budget(cache::fallback_budget);
}

/* The returns from translated code a run has seen, and the one after which
 * the budget is told to shed, as a store that passed it would. */
struct Returns final {
	std::size_t seen{};
	std::size_t shed_at{};
};

[[nodiscard]] bool shed_at_return(arm64::CpuState &, void *context) noexcept
{
	auto &returns = *static_cast<Returns *>(context);
	if (++returns.seen == returns.shed_at) {
		cache::set_budget(1);
		cache::relieve();
	}
	return false;
}

/* A holder in the middle of a run lets go of a space the budget has shed at
 * its next return from translated code, not a dispatch later. The program is
 * a long chain of blocks the executor translates one after another, each
 * return to the run loop is a boundary, and the budget falls under what is
 * held and relieves it after the fifth. The run ends there, as a spent
 * budget ends one: the executor, which still holds the space, is not asked to
 * grow it, and the next run starts by letting go of it, as the last holder,
 * and goes on in a new space. Without the exit the run translates the rest of
 * the chain into the space it was asked to shed and ends at the system call. */
void a_running_holder_leaves_a_shed_space_at_its_next_return(
	unsigned long generation)
{
	constexpr std::size_t blocks = 400;
	constexpr std::size_t shed_at = 5;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	const auto vcpu = std::make_unique<Vcpu>();
	auto &executor = vcpu->executor;
	auto &memory = vcpu->memory;
	auto &cpu = vcpu->cpu;
	const auto base = cache::ledger_state();
	Returns returns{.shed_at = shed_at};
	cpu = {};
	cpu.pc = code_base;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = memory,
		.translated_memory = &memory,
		.generation = generation,
		.instruction_budget = 100000,
		.boundary = &shed_at_return,
		.boundary_context = &returns,
	};
	const auto first = arm64::detail::run_translated(executor, request);
	cache::set_budget(cache::fallback_budget);
	require(first.reason == uml::cpu::ExitReason::instruction_budget &&
		returns.seen == shed_at,
		"a run did not end at the first return after its space was shed");
	require(first.retired_instructions < blocks);
	const auto held = cache::ledger_state();
	require(held.shedding > base.shedding,
		"the budget did not ask the running space to shed");

	/* The next run lets go of it, as the last holder, and finishes the
	 * chain in a space of its own. */
	request.boundary = nullptr;
	request.boundary_context = nullptr;
	const auto second = arm64::detail::run_translated(executor, request);
	require(second.reason == uml::cpu::ExitReason::system_call,
		"the run after a shed space was let go of did not finish");
	require(cache::ledger_state().shedding == base.shedding,
		"the next run did not free the space it was asked to let go of");
}

/* An executor that lets go of a space that nobody will run again does not tell
 * the space of each body: the space dies with its last holder, which frees the
 * bodies with the arena, and a body queued for the limbo is a lock, a probe and
 * a push each, behind every other executor doing the same. Such a space is one
 * the budget shed, one closed for renewal, one private to the cache and one
 * whose generation the MMU destroyed; the open one of an executor whose window
 * retires it, which other executors may go on running and renew, is let go of
 * body by body (a_mostly_free_space_is_renewed). The ledger gets a shed space's
 * bytes back with its last holder as ever. */
enum class Doomed { shed, closed };

void a_doomed_space_is_let_go_of_without_telling_it_of_each_body(
	unsigned long generation, Doomed how)
{
	constexpr std::size_t blocks = 200;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	const auto vcpu = std::make_unique<Vcpu>();
	auto &executor = vcpu->executor;
	auto &memory = vcpu->memory;
	auto &cpu = vcpu->cpu;
	const auto base = cache::ledger_state();
	require(run_to_system_call(executor, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	const auto built = executor.cache_diagnostics().translations;
	require(built >= blocks);
	const auto pushes = stress::limbo_pushes();
	if (how == Doomed::shed) {
		cache::set_budget(1);
		cache::relieve();
		cache::set_budget(cache::fallback_budget);
		require(cache::ledger_state().shedding > base.shedding);
	} else {
		require(stress::close_spaces(generation) == 1);
	}

	/* The next entry lets go of the space as its last holder, and builds the
	 * chain again in a new one. */
	require(run_to_system_call(executor, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	require(cache::ledger_state().shedding == base.shedding,
		"the space was not freed with its last holder");
	require(executor.cache_diagnostics().translations >= built + blocks,
		"the executor did not translate in a space of its own again");
	require(stress::limbo_pushes() == pushes,
		"letting go of a doomed space queued its bodies one by one");
}

/* A space the budget shed and nothing has closed yet (no holder has been
 * through its next entry) is doomed by the shed alone: an executor that ends
 * while it holds one, as a vCPU's executor does when its guest exits, empties
 * its slots without telling the space and takes the space with it as the last
 * holder. */
void a_shed_space_goes_with_the_executor_that_ends_holding_it(
	unsigned long generation)
{
	constexpr std::size_t blocks = 200;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	auto vcpu = std::make_unique<Vcpu>();
	const auto base = cache::ledger_state();
	const auto closes = stress::closes();
	require(run_to_system_call(vcpu->executor, vcpu->memory, generation,
		vcpu->cpu, code_base, blocks + 4) != 0);
	const auto pushes = stress::limbo_pushes();
	cache::set_budget(1);
	cache::relieve();
	cache::set_budget(cache::fallback_budget);
	require(cache::ledger_state().shedding > base.shedding);
	require(stress::closes() == closes,
		"the space was closed before the executor ended");

	vcpu.reset();
	require(cache::ledger_state().shedding == base.shedding,
		"the shed space did not go with the executor that held it");
	require(stress::limbo_pushes() == pushes,
		"an executor that ended queued the bodies of a shed space");
}

/* A private cache's window retires its space, whichever way it is used: no
 * other executor holds it. */
void a_private_space_is_let_go_of_without_telling_it_of_each_body()
{
	constexpr unsigned long generation = 1260;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{1};
	put_program(backing, 4, 5);
	JitMapping mapping{generation, code_base, backing, 1};
	std::vector<std::unique_ptr<JitBacking>> others;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (unsigned long other = 0;
	     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other) {
		others.push_back(std::make_unique<JitBacking>(1));
		put_program(*others.back(), 1, 2);
		mappings.push_back(std::make_unique<JitMapping>(
			generation + 1 + other, code_base, *others.back(), 1));
	}
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1) == 0);
	const auto vcpu = std::make_unique<Vcpu>();
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	auto &memory = vcpu->memory;
	auto &executor = vcpu->executor;
	auto &cpu = vcpu->cpu;
	require(run_to_system_call(executor, memory, generation, cpu,
		code_base) == 4);
	const auto spaces = stress::live_spaces();
	const auto pushes = stress::limbo_pushes();
	for (unsigned long other = 0; other <= UML_GADGET_RESIDENT_GENERATIONS;
	     ++other)
		require(run_to_system_call(executor, memory,
			generation + 1 + other, cpu, code_base) == 4);
	/* The window went past it, and the oldest of the others: those spaces
	 * went, and queued nothing. */
	require(stress::live_spaces() ==
		spaces + UML_GADGET_RESIDENT_GENERATIONS - 1,
		"the window did not retire the private space");
	require(stress::limbo_pushes() == pushes,
		"retiring a private space queued its bodies one by one");
}

enum class Ends { next_run, teardown };

/* Executors A and B hold a space, B in the middle of a run, and the budget
 * sheds it. A lets go of it at its next entry and B does not yet, which is
 * what a running holder does until its next return to the run loop, or one
 * that sits in a host wait for much longer. B goes on in the space A let go
 * of, and what A held must still be there for it: its bodies are indexed and
 * linked, so B chains into them and takes the one at its entry without
 * translating; the count of holders of a body keeps A's hold, so that B
 * releasing it (its code changed under it) does not queue it for freeing
 * while another executor, or B itself in a chain, may run it. Then B lets go
 * as the last holder, at its next run or as it ends. None of this queues a
 * body for the limbo, and the ledger gets the space's bytes back. */
void a_holder_runs_on_in_a_space_another_holder_let_go_of(
	unsigned long generation, Ends ends)
{
	constexpr std::size_t blocks = 300;
	constexpr std::size_t entry = 5;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	auto a_vcpu = std::make_unique<Vcpu>();
	auto b_vcpu = std::make_unique<Vcpu>();
	auto &a = a_vcpu->executor;
	auto &a_memory = a_vcpu->memory;
	auto &a_cpu = a_vcpu->cpu;
	auto &b = b_vcpu->executor;
	auto &b_memory = b_vcpu->memory;
	auto &b_cpu = b_vcpu->cpu;
	const auto base = cache::ledger_state();

	/* A builds the chain. B is inside a run, holding the one block of it its
	 * run began at, and follows the links the rest of the way. */
	require(run_to_system_call(a, a_memory, generation, a_cpu, code_base,
		blocks + 4) != 0);
	const auto entry_pc = code_base + entry * 4;
	b_cpu = {};
	b_cpu.pc = entry_pc;
	require(b.begin_execution(generation, b_cpu.pc));
	std::size_t executed = 0;
	require(b.run_block(b_cpu, b_memory, generation, executed) ==
		EngineEvent::system_call);
	require(b.cache_diagnostics().translations == 0 &&
		b.cache_diagnostics().adoptions == 1,
		"B did not take the block it entered at from A");

	cache::set_budget(1);
	cache::relieve();
	cache::set_budget(cache::fallback_budget);
	require(cache::ledger_state().shedding > base.shedding);
	require(a.renewal_pending() && b.renewal_pending(),
		"the budget did not ask both holders to let go");

	/* A lets go at its next entry, B is still in its run: the space stays,
	 * with what A held in it. */
	const auto pushes = stress::limbo_pushes();
	a_cpu = {};
	a_cpu.pc = code_base;
	(void)run_once(a, a_memory, generation, a_cpu);
	require(stress::limbo_pushes() == pushes,
		"a holder that let go of a shed space queued its bodies");
	require(cache::ledger_state().shedding > base.shedding,
		"the space went while a holder still held it");

	/* B follows the links into bodies only A held, without translating. */
	b_cpu = {};
	b_cpu.pc = entry_pc;
	require(b.run_block(b_cpu, b_memory, generation, executed) ==
		EngineEvent::system_call,
		"B did not reach the system call through the bodies A held");
	require(b.cache_diagnostics().translations == 0,
		"B translated what A had held in the space");

	/* B's code changes under it: the block it holds is stale, B lets go of
	 * that one through the space, which must not queue the body, and takes
	 * the rest of the chain from A's bodies, one miss at a time. */
	backing.put32(entry * 4, branch(entry_pc, entry_pc + 8));
	require(darwin_uml_static_vm_invalidate_code_generation(
		generation, entry_pc, 4) == 0);
	b_cpu = {};
	b_cpu.pc = entry_pc;
	EngineEvent event = EngineEvent::continued;
	for (std::size_t run = 0; run < blocks && event != EngineEvent::system_call;
	     ++run)
		event = b.run_block(b_cpu, b_memory, generation, executed);
	require(event == EngineEvent::system_call,
		"B did not finish the chain after its code changed");
	require(b.cache_diagnostics().translations == 1 &&
		b.cache_diagnostics().adoptions > 1,
		"B did not translate just the block that changed");
	require(stress::limbo_pushes() == pushes,
		"B released a body A had held and it was queued for freeing");
	require(cache::ledger_state().shedding > base.shedding);
	b.end_execution();

	/* B lets go as the last holder: at its next run, or as it ends. */
	if (ends == Ends::next_run) {
		require(run_to_system_call(b, b_memory, generation, b_cpu,
			code_base, blocks + 4) != 0);
	} else {
		b_vcpu.reset();
	}
	require(stress::limbo_pushes() == pushes,
		"the last holder queued the bodies of the space it let go of");
	require(cache::ledger_state().shedding == base.shedding,
		"the shed space was not freed with its last holder");
}

/* A let-go that runs out of memory leaves the ask standing, and the runs go
 * on as they did: the executor does not end every run at its first block
 * because a flag it cannot clear is set. Whichever allocation of the let-go
 * fails, no run is asked to end early afterwards, and the first let-go that
 * succeeds frees the space. */
void a_failed_let_go_does_not_end_runs_early(unsigned long generation)
{
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{1};
	put_program(backing, 1, 2);
	JitMapping mapping{generation, code_base, backing, 1};
	bool stood = false;
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 256, "the failure point never moved past the "
			"end of letting go of a shed space");
		const auto vcpu = std::make_unique<Vcpu>();
		auto &executor = vcpu->executor;
		auto &memory = vcpu->memory;
		auto &cpu = vcpu->cpu;
		const auto base = cache::ledger_state();
		require(run_to_system_call(executor, memory, generation, cpu,
			code_base) == 4);
		cache::set_budget(1);
		cache::relieve();
		cache::set_budget(cache::fallback_budget);
		require(executor.renewal_pending(),
			"the budget did not ask the executor to let go");
		require(cache::ledger_state().shedding > base.shedding);

		cpu = {};
		cpu.pc = code_base;
		uml::testing::fail_after_allocations(allowed);
		require(executor.begin_execution(generation, cpu.pc));
		executor.end_execution();
		/* True once the let-go made fewer allocations than allowed. */
		const bool done =
			uml::testing::cancel_pending_allocation_failure();
		require(!executor.renewal_pending(),
			"an executor that could not let go asks every run to end");
		const bool held =
			cache::ledger_state().shedding > base.shedding;
		stood = stood || held;
		/* Whichever way it went, the next entry lets go. */
		require(executor.begin_execution(generation, cpu.pc));
		executor.end_execution();
		require(cache::ledger_state().shedding == base.shedding,
			"the space was not let go of at the next entry");
		if (done) {
			require(stood, "no allocation of the let-go failed");
			return;
		}
	}
}

/* An address space only one executor holds is retired in its turn, however
 * lately that executor ran it: nobody else could be in the code it frees. */
void a_window_retires_a_space_no_other_executor_holds()
{
	constexpr unsigned long generation = 1160;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{1};
	put_program(backing, 4, 5);
	JitMapping mapping{generation, code_base, backing, 1};
	std::vector<std::unique_ptr<JitBacking>> others;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (unsigned long other = 0;
	     other <= UML_GADGET_RESIDENT_GENERATIONS; ++other) {
		others.push_back(std::make_unique<JitBacking>(1));
		put_program(*others.back(), 1, 2);
		mappings.push_back(std::make_unique<JitMapping>(
			generation + 1 + other, code_base, *others.back(), 1));
	}
	const auto vcpu = std::make_unique<Vcpu>();
	auto &memory = vcpu->memory;
	auto &executor = vcpu->executor;
	auto &cpu = vcpu->cpu;
	require(run_to_system_call(executor, memory, generation, cpu,
		code_base) == 4);
	require(stress::open_space_bytes(generation).spaces == 1);
	for (unsigned long other = 0; other <= UML_GADGET_RESIDENT_GENERATIONS;
	     ++other)
		require(run_to_system_call(executor, memory,
			generation + 1 + other, cpu, code_base) == 4);
	/* Its only executor retired it, so it went. */
	require(stress::open_space_bytes(generation).spaces == 0);
}

/* A private cache is not renewed, whatever its arena holds: its window
 * retires it. */
void a_private_cache_is_never_renewed()
{
	constexpr unsigned long generation = 1060;
	constexpr std::size_t blocks = 700;
	stress::set_renewal_period(1);
	stress::set_renewal_floor(0);
	JitBacking backing{pages_for((blocks + 1) * 4)};
	put_chain(backing, blocks);
	JitMapping mapping{generation, code_base, backing,
		pages_for((blocks + 1) * 4)};
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "0", 1) == 0);
	const auto vcpu = std::make_unique<Vcpu>();
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	auto &memory = vcpu->memory;
	auto &alone = vcpu->executor;
	auto &cpu = vcpu->cpu;
	const auto closes = stress::closes();
	require(run_to_system_call(alone, memory, generation, cpu, code_base,
		blocks + 4) != 0);
	require(alone.cache_diagnostics().translations == blocks + 1);
	require(stress::closes() == closes);
	stress::set_renewal_period(0);
}

/* A closes the space while it is in a bracket, so it goes on in the old one;
 * B goes to another address space and back, and so enters the successor first
 * and builds only the entry block; A's next bracket lets go of the old space,
 * which dies with it, takes the entry block from the successor and builds the
 * rest there, where it is linked, so that after that one run reaches the
 * system call. A cache that did not prepare the successor would bind to it at
 * the code-change sequence zero, which the gate B set does not accept:
 * nothing A built would be linked, and every run would stop at the second
 * block. (B has to prepare the successor itself for the gate to be set, as an
 * executor does when it comes back from elsewhere.) */
void renewal_reaches_every_executor()
{
	constexpr unsigned long generation = 1080;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	JitBacking backing{1};
	put_program(backing, 3, 4);
	JitMapping mapping{generation, code_base, backing, 1};
	JitBacking elsewhere{1};
	put_program(elsewhere, 1, 2);
	JitMapping elsewhere_mapping{generation + 1, code_base, elsewhere, 1};
	const auto spaces = stress::live_spaces();
	{
		const auto a_vcpu = std::make_unique<Vcpu>();
		const auto b_vcpu = std::make_unique<Vcpu>();
		auto &a = a_vcpu->executor;
		auto &b = b_vcpu->executor;
		auto &a_memory = a_vcpu->memory;
		auto &b_memory = b_vcpu->memory;
		auto &cpu = a_vcpu->cpu;
		require(run_to_system_call(a, a_memory, generation, cpu,
			code_base) == 4);
		/* A code change, so the sequence is not zero. */
		backing.put32(target - code_base, move_immediate(1, 5));
		require(darwin_uml_static_vm_invalidate_code_generation(
			generation, target, 4) == 0);
		require(darwin_uml_static_vm_code_changes(generation) != 0);
		require(run_to_system_call(a, a_memory, generation, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		require(run_to_system_call(b, b_memory, generation, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		require(b.cache_diagnostics().adoptions == 1);
		require(stress::live_spaces() == spaces + 1);

		/* The space is closed while A is inside it. */
		cpu = {};
		cpu.pc = code_base;
		require(a.begin_execution(generation, cpu.pc));
		require(stress::close_spaces(generation) == 1);
		require(stress::open_space_bytes(generation).spaces == 0);
		std::size_t executed = 0;
		const auto event = a.run_block(cpu, a_memory, generation, executed);
		a.end_execution();
		require(event == ::uml::cpu::EngineEvent::system_call &&
			executed == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		require(stress::live_spaces() == spaces + 1);

		/* B leaves the old space as it goes elsewhere, and on coming back
		 * builds the entry block of the new one. */
		require(run_to_system_call(b, b_memory, generation + 1, cpu,
			code_base) == 4);
		require(stress::live_spaces() == spaces + 2);
		cpu = {};
		cpu.pc = code_base;
		const auto b_translations = b.cache_diagnostics().translations;
		auto outcome = run_once(b, b_memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued &&
			outcome.executed == 2 && cpu.pc == target);
		require(b.cache_diagnostics().translations == b_translations + 1);
		require(stress::open_space_bytes(generation).spaces == 1);
		require(stress::live_spaces() == spaces + 3);

		/* A's next bracket lets go of the old space, which is destroyed with
		 * its last executor, and adopts what B built. */
		cpu = {};
		cpu.pc = code_base;
		const auto a_translations = a.cache_diagnostics().translations;
		const auto a_adoptions = a.cache_diagnostics().adoptions;
		outcome = run_once(a, a_memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::continued &&
			outcome.executed == 2 && cpu.pc == target);
		require(a.cache_diagnostics().translations == a_translations);
		require(a.cache_diagnostics().adoptions == a_adoptions + 1);
		require(stress::live_spaces() == spaces + 2);

		/* A builds the second block in the new space, which links it. */
		outcome = run_once(a, a_memory, generation, cpu);
		require(outcome.event == ::uml::cpu::EngineEvent::system_call &&
			outcome.executed == 2);
		require(a.cache_diagnostics().translations == a_translations + 1);
		require(cpu.registers[1] == 5);

		/* From here one run of either executor reaches the system call
		 * through the links. */
		for (auto *executor : {&a, &b}) {
			cpu = {};
			cpu.pc = code_base;
			outcome = run_once(*executor,
				executor == &a ? a_memory : b_memory, generation, cpu);
			require(outcome.event ==
					::uml::cpu::EngineEvent::system_call &&
				outcome.executed == 4);
			require(cpu.registers[0] == 3 && cpu.registers[1] == 5);
		}
		require(stress::open_space_bytes(generation).spaces == 1);
	}
	require(stress::live_spaces() == spaces);
}

/* Several vCPUs run, link, retire, revalidate and renew the same address
 * spaces while another rewrites their code. Every run must see one of the
 * values the code has held, and once the writer stops, the last ones; and when
 * the executors are gone so is every space, the closed ones included. */
void concurrent_executors_follow_renewals()
{
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	stress::set_renewal_period(8);
	const auto alive = stress::live_spaces();
	const auto closed = stress::closes();
	code_change_storm(1100, UML_GADGET_RESIDENT_GENERATIONS + 2, 6);
	/* The count varies with how the threads interleave (about 120 to 190 on
	 * an idle host); renewals that never happen leave it at zero. */
	require(stress::closes() > closed + 20);
	require(stress::live_spaces() == alive);
	stress::set_renewal_period(0);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", "1", 1) == 0);
	held_bytes_follow_the_slots();
	a_mostly_free_space_is_renewed_and_a_busy_one_is_not();
	a_window_leaves_a_space_another_executor_is_running(1120);
	relief_sheds_a_space_the_window_leaves(1200);
	a_running_holder_leaves_a_shed_space_at_its_next_return(1210);
	a_failed_let_go_does_not_end_runs_early(1220);
	a_doomed_space_is_let_go_of_without_telling_it_of_each_body(1230,
		Doomed::shed);
	a_doomed_space_is_let_go_of_without_telling_it_of_each_body(1235,
		Doomed::closed);
	a_shed_space_goes_with_the_executor_that_ends_holding_it(1237);
	a_private_space_is_let_go_of_without_telling_it_of_each_body();
	a_holder_runs_on_in_a_space_another_holder_let_go_of(1240,
		Ends::next_run);
	a_holder_runs_on_in_a_space_another_holder_let_go_of(1250,
		Ends::teardown);
	a_window_retires_a_space_no_other_executor_holds();
	a_private_cache_is_never_renewed();
	renewal_reaches_every_executor();
	concurrent_executors_follow_renewals();
	return 0;
}
