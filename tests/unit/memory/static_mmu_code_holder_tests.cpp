// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Which generations hold a translation of a frame of guest code, and what a
 * code synchronization or a kernel write tells them. A translator registers
 * its generation in the frame's holder word before it reads the frame's
 * bytes; a publication takes the word and publishes a whole-space code change
 * in each generation the word named (mmu/code_holders.hpp).
 *
 * The rows:
 *   words        a frame's word fills two slots, then reads "every generation";
 *                a generation that is gone is dropped on the way; a generation
 *                a slot cannot hold registers as every one.
 *   interleaving a reader that has copied the frame's bytes (held there by the
 *                fetch observer) is published to by a writer that stores and
 *                synchronizes the frame meanwhile, from another process.
 *   dead holder  a holder whose generation was destroyed is skipped, and the
 *                publication does not bring its generation back.
 *   own frame    a frame held by its writer alone, or by nobody, is published
 *                with no exchange of its word; one held by another is taken
 *                once, and a second publication finds it empty.
 *   no leaf      a backing that cannot make a leaf of its table registers
 *                nothing, and a publication on it goes to every generation.
 *   kernel write the kernel writes a frame through another process's view
 *                (darwin_uml_vm_invalidate_code): a process whose table has no
 *                entry for the page publishes to every generation, and one
 *                that maps it without execute still tells the holders.
 *   named write the kernel names the frame it wrote
 *                (darwin_uml_vm_invalidate_code_frame): the holders of that
 *                frame hear of it, whatever the process maps at the address
 *                and whether or not it has an entry for the page, and no other
 *                generation does; a frame the kernel cannot name goes to every
 *                generation.
 *   remap        the target replaces the page the kernel pinned with another
 *                frame before the write: the holders of the frame written
 *                hear of it, and those of the frame mapped now do not.
 *   x86 beside   (x86-64 guest builds) one kernel write reaches an arm64 reader
 *                named in the word and an x86 holder of the frame, each by its
 *                own mechanism, by address and by frame.
 *   overlap      a publisher has taken a frame's word and not yet published;
 *                a second synchronization of the frame meets an empty word
 *                (or a word of its own) and must still reach the first one's
 *                holders, with and without a holder that registered between.
 *   own views    a generation that holds a frame is told of a change made
 *                through another of its own views of it (one that cannot
 *                execute, an executable alias, or none at all) at every
 *                address, as it ran or verified the frame at another; one
 *                that holds nothing is told of the range written.
 *
 * Not a row: the pairing between a publisher's stamp step and a verifier's
 * registration (Binder::verify_page). The publisher steps the frame's stamp,
 * fences, then reads the holder word; the verifier adds its generation to the
 * word, fences, then reads the stamp. Store buffering: the word read sees the
 * registration or the stamp read sees the step, never neither. The architecture
 * allows neither without the publisher's fence (step_stamp in
 * mmu/address_space.cpp), but no two-thread row can fail without it on Apple
 * cores, so the pairing is held by that fence and by review, not by a row
 * that cannot be red.
 */
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "allocation_fault.hpp"
#include "code_holders.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/static_guest_memory_testing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace {

using uml::memory::StaticGuestMemory;
using uml::testing::ScratchBacking;
using uml::testing::require;
namespace holders = uml::memory::code_holders;
namespace memory_testing = uml::memory::testing;

constexpr std::string_view backing_name = "uml-code-holder-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr unsigned long code_address = 0x4a00'0000;
constexpr unsigned long writer_address = 0x4b00'0000;
constexpr std::uint32_t old_word = 0xd280'0020U; // movz x0, #1
constexpr std::uint32_t new_word = 0xd280'0040U; // movz x0, #2
constexpr std::uint32_t third_word = 0xd280'0060U; // movz x0, #3

void map(unsigned long generation, unsigned long address,
	ScratchBacking &backing, int protection, int code)
{
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address), backing.descriptor(), 0, page,
		protection, code, (protection & PROT_WRITE) != 0 ? 1 : 0) == 0);
}

void map_code(unsigned long generation, ScratchBacking &backing)
{
	map(generation, code_address, backing, PROT_READ | PROT_EXEC, 1);
}

void destroy(unsigned long generation)
{
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* What a vCPU's fetch does: this generation's memory, registering, reads the
 * instruction at the page's first word. */
std::uint32_t fetch(StaticGuestMemory &memory, unsigned long generation)
{
	memory.set_code_holder_registration(true);
	memory.prepare(generation, darwin_uml_static_vm_changes(generation));
	std::uint32_t word = 0;
	require(memory.fetch(code_address,
		std::as_writable_bytes(std::span{&word, 1})));
	return word;
}

std::uint32_t fetch(unsigned long generation)
{
	auto memory = std::make_unique<StaticGuestMemory>();
	return fetch(*memory, generation);
}

/* The holder word of the frame that `generation` maps at the code address. */
holders::Word &word_of(unsigned long generation)
{
	void *host = nullptr;
	void *holder = nullptr;
	require(darwin_uml_static_vm_translate_execute(generation, code_address,
		&host, &holder) == DARWIN_UML_STATIC_TRANSLATED);
	require(holder != nullptr);
	return *static_cast<holders::Word *>(holder);
}

std::uint64_t code_changes(unsigned long generation)
{
	void *context = darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	const auto changes =
		darwin_uml_static_vm_execution_context_sequences(context).code_changes;
	darwin_uml_static_vm_execution_context_destroy(context);
	return changes;
}

/* The code changes `generation` was told of after `sequence`; a change of
 * the whole space is {0, 0}. */
std::vector<darwin_uml_static_code_change> changes_after(
	unsigned long generation, std::uint64_t sequence)
{
	void *context = darwin_uml_static_vm_execution_context_create(generation);
	require(context != nullptr);
	std::array<darwin_uml_static_code_change, 64> ranges{};
	std::uint64_t current = 0;
	const auto count = darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, ranges.data(), ranges.size(), &current);
	darwin_uml_static_vm_execution_context_destroy(context);
	require(count >= 0, "the code change ring no longer described the span");
	return {ranges.begin(), ranges.begin() + count};
}

bool told_of_the_whole_space(
	const std::vector<darwin_uml_static_code_change> &changes)
{
	for (const auto &change : changes)
		if (change.length == 0)
			return true;
	return false;
}

bool told_of_the_view(const std::vector<darwin_uml_static_code_change> &changes)
{
	for (const auto &change : changes)
		if (change.address == writer_address && change.length == page)
			return true;
	return false;
}

/* A guest code synchronization by `writer` at its own view of the frame. */
void synchronize(unsigned long writer)
{
	require(darwin_uml_static_vm_activate(writer) == 0);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	require(darwin_uml_static_vm_synchronize_code(writer_address) == 0);
}

/* A writer's mappings: a writable view of the frame, the way another
 * process holds it. */
void map_writer(unsigned long writer, ScratchBacking &backing)
{
	map(writer, writer_address, backing, PROT_READ | PROT_WRITE, 0);
}

holders::Counters since(const holders::Counters &before)
{
	const auto now = holders::counters();
	return {now.registrations - before.registrations,
		now.overflows - before.overflows, now.exchanges - before.exchanges,
		now.publications - before.publications,
		now.all_publications - before.all_publications,
		now.generations_published - before.generations_published,
		now.leaves - before.leaves,
		now.unknown_backings - before.unknown_backings,
		now.overlap_widenings - before.overlap_widenings,
		now.absent_kernel_pages - before.absent_kernel_pages,
		now.kernel_exchanges - before.kernel_exchanges};
}

void test_the_word_fills_two_slots_then_reads_every_generation()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long first = 1001, second = 1002, third = 1003;
	for (const auto generation : {first, second, third})
		map_code(generation, backing);
	const auto before = holders::counters();

	require(fetch(first) == old_word);
	auto &word = word_of(first);
	require(word.load() == holders::join(first, 0));
	/* The same generation again changes nothing. */
	require(fetch(first) == old_word);
	require(word.load() == holders::join(first, 0));
	require(fetch(second) == old_word);
	require(word.load() == holders::join(first, second));
	require(fetch(third) == old_word);
	require(word.load() == holders::all);
	/* Every generation holds, and registers nothing further. */
	require(fetch(first) == old_word);
	require(word.load() == holders::all);

	const auto delta = since(before);
	require(delta.registrations == 3 && delta.overflows == 1);
	/* One leaf for the region, however many frames of it are asked. */
	require(delta.leaves == 1);
	for (const auto generation : {first, second, third})
		destroy(generation);
}

void test_a_generation_that_is_gone_is_dropped_on_the_way_to_every_one()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long first = 1011, second = 1012, third = 1013;
	for (const auto generation : {first, second, third})
		map_code(generation, backing);
	(void)fetch(first);
	(void)fetch(second);
	auto &word = word_of(second);
	require(word.load() == holders::join(first, second));

	destroy(first);
	(void)fetch(third);
	require(word.load() == holders::join(third, second));
	destroy(second);
	destroy(third);
}

void test_a_generation_a_slot_cannot_hold_registers_as_every_one()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long wide = holders::max_generation + 5;
	map_code(wide, backing);
	(void)fetch(wide);
	require(word_of(wide).load() == holders::all);
	destroy(wide);
}

/* The reader is held by the fetch observer after it copied the frame's bytes
 * and before it returns: a writer from another process stores new bytes and
 * synchronizes them there. The reader copied the old word, so the
 * synchronization must reach its generation, which registered before it
 * copied. A reader that registers after its copy is not yet a holder when the
 * writer looks, and is never told. */
std::atomic<bool> reader_copied;
std::atomic<bool> writer_done;

void hold_the_reader(std::uint64_t) noexcept
{
	reader_copied.store(true, std::memory_order_release);
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!writer_done.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit)
			uml::testing::fail("the writer never finished");
		std::this_thread::yield();
	}
}

void test_a_publication_between_the_reader_s_copy_and_its_return_reaches_it()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long reader = 1021, writer = 1022;
	map_code(reader, backing);
	map_writer(writer, backing);
	require(darwin_uml_static_vm_activate(reader) == 0);
	const auto changes_before = code_changes(reader);

	reader_copied.store(false);
	writer_done.store(false);
	memory_testing::set_fetch_observer(&hold_the_reader);
	std::uint32_t copied = 0;
	std::thread fetching{[&] { copied = fetch(reader); }};
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!reader_copied.load(std::memory_order_acquire)) {
		require(std::chrono::steady_clock::now() < limit,
			"the reader never copied the frame");
		std::this_thread::yield();
	}
	backing.put32(0, new_word);
	synchronize(writer);
	writer_done.store(true, std::memory_order_release);
	fetching.join();
	memory_testing::set_fetch_observer(nullptr);

	require(copied == old_word, "the reader copied after the write");
	require(code_changes(reader) > changes_before,
		"the synchronization did not reach the reader");
	destroy(reader);
	destroy(writer);
}

void test_a_holder_that_is_gone_is_skipped_and_not_brought_back()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long reader = 1031, other = 1032, writer = 1033;
	map_code(reader, backing);
	map_code(other, backing);
	map_writer(writer, backing);
	(void)fetch(reader);
	(void)fetch(other);
	destroy(reader);
	require(darwin_uml_static_vm_generation_live(reader) == 0);
	const auto others_before = code_changes(other);

	synchronize(writer);
	require(darwin_uml_static_vm_generation_live(reader) == 0,
		"the publication made the dead generation again");
	require(code_changes(other) > others_before);
	destroy(other);
	destroy(writer);
	require(darwin_uml_static_vm_generation_live(reader) == 0);
}

void test_a_frame_held_by_its_writer_alone_is_published_without_an_exchange()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long writer = 1041, reader = 1042;
	/* The writer also runs the page it writes: a writable and executable
	 * mapping, which is its own translator and the frame's only holder. */
	map(writer, code_address, backing, PROT_READ | PROT_WRITE | PROT_EXEC, 1);
	map(writer, writer_address, backing, PROT_READ | PROT_WRITE, 0);
	map_code(reader, backing);
	(void)fetch(writer);
	auto &word = word_of(writer);
	require(word.load() == holders::join(writer, 0));

	/* The word is empty or the writer's own: nothing is taken. */
	auto before = holders::counters();
	synchronize(writer);
	synchronize(writer);
	require(since(before).exchanges == 0 && since(before).publications == 0);
	require(word.load() == holders::join(writer, 0),
		"the writer's own registration was taken");

	/* Another generation holds it too: the first publication takes the word
	 * and tells the other; the second finds it empty of the other. */
	(void)fetch(reader);
	const auto reader_before = code_changes(reader);
	before = holders::counters();
	synchronize(writer);
	require(since(before).exchanges == 1 && since(before).publications == 1);
	require(since(before).kernel_exchanges == 0,
		"a guest synchronization's take was counted as the kernel's");
	require(since(before).overlap_widenings == 0,
		"a publication that overlapped nothing widened");
	require(code_changes(reader) > reader_before);
	before = holders::counters();
	synchronize(writer);
	require(since(before).exchanges == 0,
		"a second publication took a word that was already taken");
	destroy(writer);
	destroy(reader);
}

void test_a_backing_without_a_leaf_publishes_to_every_generation()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long reader = 1051, bystander = 1052, writer = 1053;
	map_code(reader, backing);
	map_code(bystander, backing);
	map_writer(writer, backing);
	require(darwin_uml_static_vm_activate(bystander) == 0);
	require(darwin_uml_static_vm_activate(reader) == 0);
	const auto reader_before = code_changes(reader);
	const auto before = holders::counters();

	memory_testing::fail_next_allocation(
		memory_testing::AllocationSite::holder_leaf);
	/* The reader's fetch finds no leaf and cannot make one: it registers
	 * nothing, and the backing is unknown from now on. */
	require(fetch(reader) == old_word);
	memory_testing::clear_allocation_failure();
	require(since(before).unknown_backings == 1);

	backing.put32(0, new_word);
	synchronize(writer);
	require(code_changes(reader) > reader_before,
		"a backing without a table did not publish to the reader");
	require(since(before).all_publications == 1);
	destroy(reader);
	destroy(bystander);
	destroy(writer);
}

/* The first publisher has taken the frame's word, so the reader that held it
 * is no longer named there, and has not yet published to it (the observer
 * holds it at the start of its publication). A second synchronization of the
 * same frame then finds the word empty, or holding only a generation that
 * registered after the take. The reader must hear of the second change before
 * the first publisher goes on: a completed synchronization cannot leave a
 * holder that another publisher consumed unannounced. */
std::atomic<bool> publisher_paused;
std::atomic<bool> publisher_released;
thread_local bool holds_at_publication;

void hold_the_publisher(memory_testing::AllocationSite site) noexcept
{
	if (site != memory_testing::AllocationSite::holder_publication ||
	    !holds_at_publication)
		return;
	publisher_paused.store(true, std::memory_order_release);
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!publisher_released.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit)
			uml::testing::fail("the held publisher was never released");
		std::this_thread::yield();
	}
}

/* The second publisher is a guest synchronization by another process, or a
 * kernel write or a guest synchronization through the reader's own read-write
 * view of the frame, the reader being the target of the call: the reader hears
 * of it at every address, as the word the first publisher took was its own. */
enum class Second { another_process, kernel_on_reader, sync_on_reader };

void two_publishers(bool late_holder, Second second_kind, unsigned long base)
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	const unsigned long reader = base, late = base + 1, first = base + 2,
		second = base + 3;
	map_code(reader, backing);
	map_code(late, backing);
	map_writer(first, backing);
	map_writer(second, backing);
	if (second_kind != Second::another_process)
		map_writer(reader, backing);
	(void)fetch(reader);
	const auto changes_before = code_changes(reader);
	const auto before = holders::counters();

	publisher_paused.store(false);
	publisher_released.store(false);
	memory_testing::set_allocation_observer(&hold_the_publisher);
	std::thread first_writer{[&] {
		holds_at_publication = true;
		backing.put32(0, new_word);
		synchronize(first);
	}};
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!publisher_paused.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit) {
			publisher_released.store(true);
			first_writer.join();
			uml::testing::fail("the first publisher never took the word");
		}
		std::this_thread::yield();
	}
	std::uint64_t late_before = 0;
	if (late_holder) {
		(void)fetch(late);
		late_before = code_changes(late);
	}
	backing.put32(0, third_word);
	if (second_kind == Second::kernel_on_reader) {
		require(darwin_uml_static_vm_invalidate_code_generation(reader,
			writer_address, 4) == 0);
	} else if (second_kind == Second::sync_on_reader) {
		synchronize(reader);
	} else {
		synchronize(second);
	}
	const bool heard = code_changes(reader) > changes_before;
	const bool told_everywhere = second_kind == Second::another_process ||
		told_of_the_whole_space(changes_after(reader, changes_before));
	const bool late_heard =
		!late_holder || code_changes(late) > late_before;
	publisher_released.store(true, std::memory_order_release);
	first_writer.join();
	memory_testing::set_allocation_observer(nullptr);

	require(heard, second_kind != Second::another_process ?
		"a write through the reader's own view missed the reader "
		"the first publisher had consumed" : late_holder ?
		"a publication that took a late holder missed the reader the "
		"first publisher had consumed" :
		"a publication that found the word empty missed the reader the "
		"first publisher had consumed");
	require(told_everywhere, "a write through the reader's own view told the "
		"reader the first publisher had consumed of the written range only");
	require(late_heard,
		"a publication that took a late holder missed that holder");
	require(since(before).overlap_widenings == 1,
		"the overlap did not widen exactly one publication");
	for (const auto generation : {reader, late, first, second})
		destroy(generation);
}

void test_a_publication_that_overlaps_an_unfinished_one_reaches_its_holders()
{
	two_publishers(false, Second::another_process, 1061);
	two_publishers(true, Second::another_process, 1071);
	two_publishers(false, Second::kernel_on_reader, 1081);
	two_publishers(true, Second::kernel_on_reader, 1091);
	two_publishers(false, Second::sync_on_reader, 1111);
	two_publishers(true, Second::sync_on_reader, 1121);
}

/* A kernel write for `target`, which maps the frame read-write only or not at
 * all: the reader that translated the frame must still hear of it. */
void kernel_write(bool target_maps_frame, unsigned long base)
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	const unsigned long reader = base, target = base + 1;
	map_code(reader, backing);
	require(darwin_uml_static_vm_activate(target) == 0);
	if (target_maps_frame)
		map(target, writer_address, backing, PROT_READ | PROT_WRITE, 0);
	(void)fetch(reader);
	const auto changes_before = code_changes(reader);
	const auto before = holders::counters();

	backing.put32(0, new_word);
	require(darwin_uml_static_vm_invalidate_code_generation(
		target, writer_address, 4) == 0);
	require(code_changes(reader) > changes_before, target_maps_frame ?
		"a kernel write through a view without execute missed the holder" :
		"a kernel write for a process with no entry for the page missed "
		"the holder");
	const auto delta = since(before);
	require(delta.absent_kernel_pages == (target_maps_frame ? 0U : 1U),
		"the absent pages were not counted as the write names them");
	require(delta.kernel_exchanges == (target_maps_frame ? 1U : 0U) &&
		delta.exchanges == delta.kernel_exchanges,
		"the kernel path's take was not counted as the kernel's");
	require(delta.all_publications == (target_maps_frame ? 0U : 1U),
		target_maps_frame ? "a write through a mapped page went to every "
		"generation" : "a write for an absent page did not go to every "
		"generation");
	destroy(reader);
	destroy(target);
}

void test_a_kernel_write_reaches_the_holders_whatever_the_target_maps()
{
	kernel_write(false, 1081);
	kernel_write(true, 1091);
}

/* How a change reaches the generation: the reader's own code
 * synchronization, the kernel's write naming the address, or the kernel's
 * write naming the frame it holds. */
enum class Route { code_sync, kernel_address, kernel_frame };

/* A change made through another view of the frame in the generation that
 * runs it. The reader maps the frame executable at one address and the
 * writer's view at another: a view that cannot execute, an executable alias,
 * or (for the kernel's write only, which its software page table may not
 * hold yet) none. A reader that fetched the frame holds it, and its
 * translation stands at the executable view's address, which a range of the
 * written view does not name: it is told of the whole space. A generation that
 * maps the frame and never fetched it holds nothing, and is told of the view
 * it wrote or of nothing. A write that names no address-table entry but names
 * the frame (the unmapped view) is told of the whole space when the generation
 * holds the frame, and of nothing when it does not: the frame is known, and
 * is not the generation's. */
void write_through_own_view(Route route, bool executable_view,
	bool unmapped_view, bool holds, unsigned long base)
{
	const bool kernel = route != Route::code_sync;
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	const unsigned long reader = base;
	map_code(reader, backing);
	if (!unmapped_view)
		map(reader, writer_address, backing, executable_view ?
			PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE,
			executable_view ? 1 : 0);
	require(darwin_uml_static_vm_activate(reader) == 0);
	if (holds)
		(void)fetch(reader);
	const auto before = code_changes(reader);
	const auto counters = holders::counters();

	backing.put32(0, new_word);
	if (route == Route::kernel_address)
		require(darwin_uml_static_vm_invalidate_code_generation(reader,
			writer_address, 4) == 0);
	else if (route == Route::kernel_frame)
		require(darwin_uml_static_vm_invalidate_code_frame(reader,
			writer_address, 4, backing.descriptor(), 0) == 0);
	else
		synchronize(reader);
	const auto changes = changes_after(reader, before);
	const bool nothing_published = unmapped_view &&
		route == Route::kernel_frame && !holds;
	if (holds || (unmapped_view && route == Route::kernel_address)) {
		/* A page the table does not map names no frame, and every
		 * generation hears of it; so does a holder of the frame. */
		require(told_of_the_whole_space(changes), holds ?
			"a generation that holds the frame was not told of a change "
			"through another of its own views at every address" :
			"a write through a page nothing maps did not tell the whole "
			"space");
	} else if (nothing_published) {
		require(changes.empty(),
			"a named write told a generation that holds nothing of the frame "
			"and maps it nowhere");
	} else {
		require(!told_of_the_whole_space(changes),
			"a generation that holds nothing of the frame was told of the "
			"whole space");
		/* What its view of the frame says it wrote, a range of that view. */
		require(!(kernel && !executable_view) || changes.empty(),
			"a kernel write through a view that cannot execute told a "
			"generation that holds nothing of a range");
		require(!(!kernel || executable_view) || told_of_the_view(changes),
			"the written view's range was not published");
	}
	require(since(counters).exchanges == 0,
		"a frame held by the reader alone was taken");
	destroy(reader);
}

void test_a_change_through_another_of_the_holders_own_views_tells_it_everywhere()
{
	unsigned long base = 1131;
	for (const auto route : {Route::code_sync, Route::kernel_address,
		     Route::kernel_frame})
		for (const bool holds : {true, false}) {
			write_through_own_view(route, false, false, holds, base);
			base += 10;
			write_through_own_view(route, true, false, holds, base);
			base += 10;
		}
	for (const auto route : {Route::kernel_address, Route::kernel_frame})
		for (const bool holds : {true, false}) {
			write_through_own_view(route, false, true, holds, base);
			base += 10;
		}
}

/* The same write with the frame named (darwin_uml_vm_invalidate_code_frame):
 * the reader that translated the frame hears of it whether the target maps it
 * without execute or has no entry for the page, and no other generation does:
 * nothing goes to every generation, and no page is counted as absent. */
void kernel_write_frame(bool target_maps_frame, unsigned long base)
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	const unsigned long reader = base, target = base + 1;
	map_code(reader, backing);
	require(darwin_uml_static_vm_activate(target) == 0);
	if (target_maps_frame)
		map(target, writer_address, backing, PROT_READ | PROT_WRITE, 0);
	(void)fetch(reader);
	const auto changes_before = code_changes(reader);
	const auto before = holders::counters();

	backing.put32(0, new_word);
	require(darwin_uml_static_vm_invalidate_code_frame(
		target, writer_address, 4, backing.descriptor(), 0) == 0);
	require(code_changes(reader) > changes_before, target_maps_frame ?
		"a named kernel write through a view without execute missed the "
		"holder" : "a named kernel write for a process with no entry for the "
		"page missed the holder");
	const auto delta = since(before);
	require(delta.absent_kernel_pages == 0,
		"a named kernel write was counted as a write to an absent page");
	require(delta.kernel_exchanges == 1 &&
		delta.exchanges == delta.kernel_exchanges,
		"the kernel path's take was not counted as the kernel's");
	require(delta.all_publications == 0,
		"a named kernel write went to every generation");
	destroy(reader);
	destroy(target);
}

/* A write that names no frame (the kernel cannot name it) is told to every
 * generation, as a page the process has no entry for was. */
void kernel_write_unnamed(unsigned long base)
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	const unsigned long reader = base, target = base + 1;
	map_code(reader, backing);
	require(darwin_uml_static_vm_activate(target) == 0);
	(void)fetch(reader);
	const auto changes_before = code_changes(reader);
	const auto before = holders::counters();

	backing.put32(0, new_word);
	require(darwin_uml_static_vm_invalidate_code_frame(
		target, writer_address, 4, -1, 0) == 0);
	require(code_changes(reader) > changes_before,
		"a kernel write that named no frame missed the holder");
	const auto delta = since(before);
	require(delta.absent_kernel_pages == 1 && delta.all_publications == 1,
		"a kernel write that named no frame did not go to every generation");
	destroy(reader);
	destroy(target);
}

/* A named frame that nobody translated publishes nothing: a generation that
 * translated another frame does not hear of it, whether the frame has a
 * backing (a data mapping of it) or none (nothing has mapped it). */
void kernel_write_frame_nobody_translated(bool backing_is_alive, unsigned long base)
{
	ScratchBacking bystander_backing{backing_name, page};
	ScratchBacking written{backing_name, page};
	bystander_backing.put32(0, old_word);
	const unsigned long bystander = base, target = base + 1;
	map_code(bystander, bystander_backing);
	require(darwin_uml_static_vm_activate(target) == 0);
	if (backing_is_alive)
		map(target, writer_address, written, PROT_READ | PROT_WRITE, 0);
	(void)fetch(bystander);
	const auto changes_before = code_changes(bystander);
	const auto before = holders::counters();

	require(darwin_uml_static_vm_invalidate_code_frame(
		target, writer_address, 4, written.descriptor(), 0) == 0);
	require(code_changes(bystander) == changes_before,
		"a named write to a frame nobody translated published a code change");
	const auto delta = since(before);
	require(delta.publications == 0 && delta.all_publications == 0,
		"a named write to a frame nobody translated published");
	destroy(bystander);
	destroy(target);
}

/* The kernel pins a frame to write it. Between the pin and the write the
 * target replaces what is mapped at the address with another frame, so the
 * call meets a page that is not the frame written, and the reader named in
 * the word of the frame the kernel did write must still hear of the write
 * while the generation that translated the frame mapped now does not. The
 * reader fetches once more after the replacement, so the replacement cannot
 * account for what it hears. */
void kernel_write_frame_after_a_remap(bool replacement_is_code, unsigned long base)
{
	ScratchBacking pinned{backing_name, page};
	ScratchBacking other{backing_name, page};
	pinned.put32(0, old_word);
	other.put32(0, third_word);
	const unsigned long reader = base, target = base + 1, bystander = base + 2;
	map_code(reader, pinned);
	map_code(bystander, other);
	require(darwin_uml_static_vm_activate(target) == 0);
	map(target, writer_address, pinned, PROT_READ | PROT_WRITE, 0);
	require(fetch(reader) == old_word);
	require(fetch(bystander) == third_word);
	require(darwin_uml_static_vm_unmap_generation(target,
		reinterpret_cast<void *>(writer_address), page) == 0);
	if (replacement_is_code)
		map(target, writer_address, other, PROT_READ | PROT_EXEC, 1);
	else
		map(target, writer_address, other, PROT_READ | PROT_WRITE, 0);
	require(fetch(reader) == old_word);
	const auto changes_before = code_changes(reader);
	const auto bystander_before = code_changes(bystander);
	const auto before = holders::counters();

	pinned.put32(0, new_word);
	require(darwin_uml_static_vm_invalidate_code_frame(
		target, writer_address, 4, pinned.descriptor(), 0) == 0);
	require(code_changes(reader) > changes_before, replacement_is_code ?
		"a kernel write after the target replaced its page with code "
		"missed the holder of the frame written" :
		"a kernel write after the target replaced its page with data "
		"missed the holder of the frame written");
	require(code_changes(bystander) == bystander_before,
		"a kernel write published the frame the target maps now");
	const auto delta = since(before);
	require(delta.kernel_exchanges == 1 && delta.all_publications == 0,
		"a named kernel write after a remap was not narrow");
	destroy(reader);
	destroy(target);
	destroy(bystander);
}

void test_a_named_kernel_write_reaches_the_holders_of_its_frame()
{
	kernel_write_frame(false, 1151);
	kernel_write_frame(true, 1161);
	kernel_write_unnamed(1171);
	kernel_write_frame_nobody_translated(false, 1181);
	kernel_write_frame_nobody_translated(true, 1191);
}

void test_a_kernel_write_after_the_target_remapped_reaches_the_holders()
{
	kernel_write_frame_after_a_remap(false, 1111);
	kernel_write_frame_after_a_remap(true, 1121);
}

#if DARWIN_UML_X86_64_GUEST
/* The kernel writes a frame that two translators hold, each in its own
 * generation: an arm64 reader, named in the frame's holder word, and an x86
 * translator, registered with the frame accounting (mmu/frame_accounting.hpp).
 * The target maps the frame with execute and never translated it. One
 * invalidate_code() carries both publications, so both holders hear of the
 * write, each by its own mechanism: the word is taken (once, as the kernel's),
 * and the frame's x86 holders are told through the accounting. Once the x86
 * contract is on, for the life of the process, so this row runs last. */
void test_a_kernel_write_reaches_an_arm64_reader_and_an_x86_holder()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long arm = 1101, x86 = 1102, target = 1103;
	map_code(arm, backing);
	map_code(x86, backing);
	map_code(target, backing);
	require(darwin_uml_static_vm_activate(target) == 0);
	(void)fetch(arm);
	std::array<darwin_uml_static_frame_location, 16> owed{};
	darwin_uml_static_frame_hold hold{};
	const auto held = darwin_uml_static_vm_hold_code_frame(nullptr, x86,
		code_address, owed.data(), owed.size(), &hold);
	require(held == DARWIN_UML_STATIC_FRAME_SETTLED ||
		held == DARWIN_UML_STATIC_FRAME_OWED,
		"the x86 translator's registration was refused");
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	const auto arm_before = code_changes(arm);
	const auto x86_before = code_changes(x86);
	const auto before = holders::counters();

	backing.put32(0, new_word);
	require(darwin_uml_static_vm_invalidate_code_generation(
		target, code_address, 4) == 0);
	require(code_changes(arm) > arm_before,
		"the arm64 reader named in the word missed a kernel write");
	require(code_changes(x86) > x86_before,
		"the x86 holder of the frame missed a kernel write");
	const auto delta = since(before);
	require(delta.kernel_exchanges == 1 && delta.all_publications == 0,
		"the kernel path's take widened because an x86 holder stood");
	destroy(arm);
	destroy(x86);
	destroy(target);
}

/* The same, the frame named: the arm64 word is taken once as the kernel's and
 * the x86 holders are told through the accounting, with nothing published to
 * every generation. */
void test_a_named_kernel_write_reaches_an_arm64_reader_and_an_x86_holder()
{
	ScratchBacking backing{backing_name, page};
	backing.put32(0, old_word);
	constexpr unsigned long arm = 1201, x86 = 1202, target = 1203;
	map_code(arm, backing);
	map_code(x86, backing);
	map_code(target, backing);
	require(darwin_uml_static_vm_activate(target) == 0);
	(void)fetch(arm);
	std::array<darwin_uml_static_frame_location, 16> owed{};
	darwin_uml_static_frame_hold hold{};
	const auto held = darwin_uml_static_vm_hold_code_frame(nullptr, x86,
		code_address, owed.data(), owed.size(), &hold);
	require(held == DARWIN_UML_STATIC_FRAME_SETTLED ||
		held == DARWIN_UML_STATIC_FRAME_OWED,
		"the x86 translator's registration was refused");
	darwin_uml_static_vm_require_coherent_kernel_code_writes();
	const auto arm_before = code_changes(arm);
	const auto x86_before = code_changes(x86);
	const auto before = holders::counters();

	backing.put32(0, new_word);
	require(darwin_uml_static_vm_invalidate_code_frame(
		target, code_address, 4, backing.descriptor(), 0) == 0);
	require(code_changes(arm) > arm_before,
		"the arm64 reader named in the word missed a named kernel write");
	require(code_changes(x86) > x86_before,
		"the x86 holder of the frame missed a named kernel write");
	const auto delta = since(before);
	require(delta.kernel_exchanges == 1 && delta.all_publications == 0,
		"the named kernel path's take widened because an x86 holder stood");
	destroy(arm);
	destroy(x86);
	destroy(target);
}
#endif

} // namespace

int main()
{
	test_the_word_fills_two_slots_then_reads_every_generation();
	test_a_generation_that_is_gone_is_dropped_on_the_way_to_every_one();
	test_a_generation_a_slot_cannot_hold_registers_as_every_one();
	test_a_publication_between_the_reader_s_copy_and_its_return_reaches_it();
	test_a_holder_that_is_gone_is_skipped_and_not_brought_back();
	test_a_frame_held_by_its_writer_alone_is_published_without_an_exchange();
	test_a_backing_without_a_leaf_publishes_to_every_generation();
	test_a_publication_that_overlaps_an_unfinished_one_reaches_its_holders();
	test_a_kernel_write_reaches_the_holders_whatever_the_target_maps();
	test_a_change_through_another_of_the_holders_own_views_tells_it_everywhere();
	test_a_named_kernel_write_reaches_the_holders_of_its_frame();
	test_a_kernel_write_after_the_target_remapped_reaches_the_holders();
#if DARWIN_UML_X86_64_GUEST
	test_a_kernel_write_reaches_an_arm64_reader_and_an_x86_holder();
	test_a_named_kernel_write_reaches_an_arm64_reader_and_an_x86_holder();
#endif
	std::puts("static-mmu-code-holder: ok");
	return 0;
}
