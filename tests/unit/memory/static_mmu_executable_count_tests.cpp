// SPDX-License-Identifier: GPL-2.0-or-later
/* The executable-mapping count of a physical page's record, and the epoch it
 * moves. A record counts the guest-code mappings that grant execute and name
 * its page, in every address space of the domain; the epoch moves when the
 * last of them departs and at a publication, and at no other time. These rows
 * hold the counts through every way a mapping appears, changes and goes: the
 * map calls, a batch, protect, unmap, a whole-space flush and destroy, an
 * allocation failure at any step, and mappings of one frame from several
 * threads. After each, an audit recounts the executable words of every
 * generation and compares them with the records. */
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "allocation_fault.hpp"
#include "memory/count_audit.hpp"
#include "domain_registry_testing.hpp"
#include "mapped_backing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace {

using uml::memory::MappedBacking;
using uml::testing::ScratchBacking;
using uml::testing::require;
namespace testing = uml::memory::testing;

constexpr std::string_view backing_name = "uml-static-mmu-count-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;
constexpr int rx = PROT_READ | PROT_EXEC;
constexpr int rw = PROT_READ | PROT_WRITE;

void *at(unsigned long address)
{
	return reinterpret_cast<void *>(address);
}

int map(unsigned long generation, unsigned long address,
	ScratchBacking &backing, unsigned long offset, unsigned long pages,
	int protection, int code)
{
	return darwin_uml_static_vm_map_generation(generation, at(address),
		backing.descriptor(), offset, pages * page, protection, code, 1);
}

void must_map(unsigned long generation, unsigned long address,
	ScratchBacking &backing, unsigned long offset, unsigned long pages,
	int protection, int code)
{
	require(map(generation, address, backing, offset, pages, protection,
		code) == 0);
}

void unmap(unsigned long generation, unsigned long address,
	unsigned long pages)
{
	require(darwin_uml_static_vm_unmap_generation(generation, at(address),
		pages * page) == 0);
}

int protect(unsigned long generation, unsigned long address,
	unsigned long pages, int protection)
{
	require(darwin_uml_static_vm_activate(generation) == 0);
	return darwin_uml_static_vm_protect(at(address), pages * page, protection);
}

/* The record of the page mapped at `address`, which must be executable. */
void *record_of(unsigned long generation, unsigned long address)
{
	void *handle = nullptr;
	darwin_uml_static_code_page_stamp stamp{};
	require(darwin_uml_static_vm_code_page(generation, address, &handle,
		&stamp) == DARWIN_UML_STATIC_TRANSLATED);
	return handle;
}

darwin_uml_static_code_page_stamp stamp_of(void *record)
{
	darwin_uml_static_code_page_stamp stamp{};
	require(darwin_uml_static_vm_code_page_stamp(record, &stamp) == 0);
	return stamp;
}

std::uint64_t count_of(void *record)
{
	return MappedBacking::executable_mappings(record);
}

/* Anomalies a row provoked on purpose. */
std::size_t expected_anomalies = 0;

/* Every count equals its recount and nothing counted wrongly. */
void audit()
{
	require(testing::audit_executable_counts() == 0);
	require(testing::count_anomalies() == expected_anomalies);
}

void destroy(unsigned long generation)
{
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A mapping of the backing that is not executable. The domain keeps a
 * backing, and the records of its pages, only while some generation maps it,
 * so a test that reads a record after its last mapping went away holds one. */
class Pin final {
public:
	Pin(unsigned long generation, ScratchBacking &backing)
		: generation_{generation}
	{
		must_map(generation_, 0x1000, backing, 0, 1, rw, 0);
	}
	~Pin() { destroy(generation_); }
	Pin(const Pin &) = delete;
	Pin &operator=(const Pin &) = delete;

private:
	unsigned long generation_;
};

void test_the_last_departure_moves_the_epoch()
{
	constexpr unsigned long generation = 701;
	constexpr unsigned long base = 0x2000'0000;
	ScratchBacking backing{backing_name, 2 * page};
	Pin pin1{1001, backing};
	must_map(generation, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(generation, base);
	const auto before = stamp_of(record);
	require(count_of(record) == 1);
	audit();
	unmap(generation, base, 1);
	const auto after = stamp_of(record);
	require(count_of(record) == 0);
	require(after.epoch == before.epoch + 1 && after.version > before.version);
	audit();
	destroy(generation);
}

/* Two address spaces map one frame: the count is the sum, and only the
 * departure that leaves none moves the epoch. */
void test_mappings_in_two_spaces_hold_one_count()
{
	constexpr unsigned long first = 702;
	constexpr unsigned long second = 703;
	constexpr unsigned long base = 0x2100'0000;
	ScratchBacking backing{backing_name, 2 * page};
	Pin pin2{1002, backing};
	must_map(first, base, backing, 0, 1, rwx, 1);
	must_map(second, base + 8 * page, backing, 0, 1, rx, 3);
	auto *record = record_of(first, base);
	require(record_of(second, base + 8 * page) == record);
	const auto start = stamp_of(record);
	require(count_of(record) == 2);

	unmap(first, base, 1);
	auto now = stamp_of(record);
	require(count_of(record) == 1 && now.epoch == start.epoch &&
		now.version > start.version);
	audit();
	unmap(second, base + 8 * page, 1);
	now = stamp_of(record);
	require(count_of(record) == 0 && now.epoch == start.epoch + 1);
	audit();
	/* Counting in again does not move the epoch back or on. */
	must_map(first, base, backing, 0, 1, rwx, 1);
	require(count_of(record) == 1 && stamp_of(record).epoch == start.epoch + 1);
	destroy(first);
	destroy(second);
	require(count_of(record) == 0 && stamp_of(record).epoch == start.epoch + 2);
	audit();
}

void test_protect_counts_both_ways()
{
	constexpr unsigned long first = 704;
	constexpr unsigned long second = 705;
	constexpr unsigned long base = 0x2200'0000;
	ScratchBacking backing{backing_name, 2 * page};
	Pin pin3{1003, backing};
	must_map(first, base, backing, 0, 1, rwx, 1);
	must_map(second, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(first, base);
	const auto start = stamp_of(record);

	require(protect(first, base, 1, rw) == 0);
	require(count_of(record) == 1 && stamp_of(record).epoch == start.epoch);
	audit();
	/* Write protection that keeps execute counts nothing. */
	require(protect(second, base, 1, rx) == 0);
	require(count_of(record) == 1 && stamp_of(record).epoch == start.epoch);
	require(protect(second, base, 1, rwx) == 0);
	require(count_of(record) == 1 && stamp_of(record).epoch == start.epoch);
	/* Gaining execute counts in. */
	require(protect(first, base, 1, rwx) == 0);
	require(count_of(record) == 2 && stamp_of(record).epoch == start.epoch);
	require(protect(first, base, 1, rw) == 0);
	require(protect(second, base, 1, rw) == 0);
	require(count_of(record) == 0 && stamp_of(record).epoch == start.epoch + 1);
	audit();
	/* A page that is not guest code never counts, whatever it gains. */
	must_map(first, base + page, backing, page, 1, rw, 0);
	require(protect(first, base + page, 1, rwx) == 0);
	audit();
	destroy(first);
	destroy(second);
	audit();
}

/* A replayed map, a change of the immutable fact and of the protection that
 * keep execute and the bytes leave the count alone, so it never passes
 * through zero on the way. */
void test_a_change_that_keeps_the_record_counts_nothing()
{
	constexpr unsigned long generation = 706;
	constexpr unsigned long base = 0x2300'0000;
	ScratchBacking backing{backing_name, 2 * page};
	Pin pin4{1004, backing};
	must_map(generation, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(generation, base);
	const auto start = stamp_of(record);
	for (const auto [protection, code] : {std::pair{rwx, 1}, std::pair{rwx, 3},
		     std::pair{rx, 3}, std::pair{rx, 1}, std::pair{rwx, 3}}) {
		must_map(generation, base, backing, 0, 1, protection, code);
		require(count_of(record) == 1 &&
			stamp_of(record).epoch == start.epoch);
		audit();
	}
	unmap(generation, base, 1);
	require(count_of(record) == 0 && stamp_of(record).epoch == start.epoch + 1);
	audit();
	destroy(generation);
}

void test_a_page_that_cannot_hold_code_counts_nothing()
{
	constexpr unsigned long generation = 707;
	constexpr unsigned long base = 0x2400'0000;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin5{1005, backing};
	must_map(generation, base, backing, 0, 1, rwx, 0);
	must_map(generation, base + page, backing, page, 1, rw, 1);
	must_map(generation, base + 2 * page, backing, 2 * page, 1, PROT_READ, 3);
	audit();
	unmap(generation, base, 3);
	audit();
	destroy(generation);
}

/* Remapping a page to other bytes counts the old record out and the new
 * one in. */
void test_remapping_moves_the_count_to_the_new_frame()
{
	constexpr unsigned long generation = 708;
	constexpr unsigned long base = 0x2500'0000;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin6{1006, backing};
	must_map(generation, base, backing, 0, 1, rwx, 1);
	auto *old_record = record_of(generation, base);
	const auto old_start = stamp_of(old_record);
	must_map(generation, base, backing, 2 * page, 1, rwx, 1);
	auto *new_record = record_of(generation, base);
	require(new_record != old_record);
	require(count_of(old_record) == 0 && count_of(new_record) == 1);
	require(stamp_of(old_record).epoch == old_start.epoch + 1);
	const auto new_start = stamp_of(new_record);
	/* Now to a page that does not grant execute. */
	must_map(generation, base, backing, 2 * page, 1, rw, 1);
	require(count_of(new_record) == 0 &&
		stamp_of(new_record).epoch == new_start.epoch + 1);
	audit();
	destroy(generation);
}

/* A batch is judged by what it started from and what it leaves. */
void test_a_batch_counts_its_net_effect()
{
	constexpr unsigned long generation = 709;
	constexpr unsigned long base = 0x2600'0000;
	ScratchBacking backing{backing_name, 8 * page};
	Pin pin7{1007, backing};
	must_map(generation, base, backing, 0, 2, rwx, 1);
	auto *kept = record_of(generation, base);
	auto *dropped = record_of(generation, base + page);
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	/* A page mapped, replaced and unmapped inside the batch leaves nothing;
	 * one mapped, unmapped and mapped again leaves its last mapping. */
	must_map(generation, base + 4 * page, backing, 4 * page, 1, rwx, 1);
	must_map(generation, base + 4 * page, backing, 5 * page, 1, rwx, 1);
	unmap(generation, base + 4 * page, 1);
	must_map(generation, base + 5 * page, backing, 6 * page, 1, rwx, 1);
	unmap(generation, base + 5 * page, 1);
	must_map(generation, base + 5 * page, backing, 6 * page, 1, rwx, 1);
	/* An existing page is replaced by one of the same frame (kept), and
	 * another is unmapped. */
	must_map(generation, base, backing, 0, 1, rx, 3);
	unmap(generation, base + page, 1);
	require(darwin_uml_static_vm_batch_end() == 0);
	require(count_of(kept) == 1 && count_of(dropped) == 0);
	auto *added = record_of(generation, base + 5 * page);
	require(count_of(added) == 1);
	audit();
	destroy(generation);
	require(count_of(kept) == 0 && count_of(added) == 0);
	audit();
}

/* Exit flushes a generation and destroy follows: every mapping is counted
 * out once. */
void test_flush_and_destroy_count_once()
{
	constexpr unsigned long generation = 710;
	constexpr unsigned long base = 0x2700'0000;
	ScratchBacking backing{backing_name, 8 * page};
	Pin pin8{1008, backing};
	must_map(generation, base, backing, 0, 6, rwx, 1);
	must_map(generation, base + 8 * page, backing, 0, 2, rwx, 1);
	std::array<void *, 6> records{};
	std::array<std::uint64_t, 6> epochs{};
	for (unsigned long index = 0; index < records.size(); ++index) {
		records[index] = record_of(generation, base + index * page);
		epochs[index] = stamp_of(records[index]).epoch;
		require(count_of(records[index]) == (index < 2 ? 2u : 1u));
	}
	require(darwin_uml_static_vm_flush_generation(generation) == 0);
	for (unsigned long index = 0; index < records.size(); ++index) {
		require(count_of(records[index]) == 0);
		require(stamp_of(records[index]).epoch == epochs[index] + 1);
	}
	audit();
	destroy(generation);
	for (unsigned long index = 0; index < records.size(); ++index)
		require(stamp_of(records[index]).epoch == epochs[index] + 1);
	audit();

	/* Destroy without a flush counts out as well. */
	constexpr unsigned long other = 711;
	must_map(other, base, backing, 0, 3, rwx, 1);
	for (unsigned long index = 0; index < 3; ++index)
		epochs[index] = stamp_of(records[index]).epoch;
	destroy(other);
	for (unsigned long index = 0; index < 3; ++index)
		require(stamp_of(records[index]).epoch == epochs[index] + 1);
	audit();
}

/* A change that fails at a step that allocates changes no mapping and no
 * count, and the same change succeeds afterwards. */
void test_an_allocation_failure_changes_no_count()
{
	constexpr unsigned long generation = 712;
	constexpr unsigned long base = 0x2800'0000;
	constexpr unsigned long pages = 48;
	ScratchBacking backing{backing_name, pages * page};
	Pin pin9{1009, backing};
	/* Each attempt maps frames no record exists for yet, so that making one
	 * is a step that can fail. */
	unsigned long next = 0;
	for (const auto site : {testing::AllocationSite::reserve,
		     testing::AllocationSite::code_record}) {
		bool failed_any = false;
		for (std::size_t occurrence = 1; occurrence < 40; ++occurrence) {
			require(next + 3 <= pages);
			testing::fail_allocation_at(site, occurrence);
			const int result = map(generation,
				base + next * page, backing, next * page, 3, rwx, 1);
			testing::clear_allocation_failure();
			audit();
			if (result == 0) {
				/* Past the last checkpoint: everything counted. */
				require(count_of(record_of(generation, base + next * page)) == 1);
				next += 3;
				break;
			}
			failed_any = true;
			void *handle = nullptr;
			darwin_uml_static_code_page_stamp stamp{};
			require(darwin_uml_static_vm_code_page(generation,
				base + next * page, &handle, &stamp) ==
				DARWIN_UML_STATIC_UNMAPPED);
			/* The retry counts. */
			must_map(generation, base + next * page, backing, next * page, 3,
				rwx, 1);
			audit();
			unmap(generation, base + next * page, 3);
			audit();
			next += 3;
		}
		require(failed_any);
	}
	destroy(generation);
	audit();
}

/* A page that gains execute has its record made before the change is
 * applied; if that fails the page is as it was. */
void test_protect_that_cannot_make_a_record_changes_nothing()
{
	constexpr unsigned long generation = 713;
	constexpr unsigned long base = 0x2900'0000;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin10{1010, backing};
	must_map(generation, base, backing, 0, 2, rw, 1);
	testing::fail_next_allocation(testing::AllocationSite::code_record);
	require(protect(generation, base, 2, rwx) != 0);
	testing::clear_allocation_failure();
	void *handle = nullptr;
	darwin_uml_static_code_page_stamp stamp{};
	require(darwin_uml_static_vm_code_page(generation, base, &handle, &stamp) ==
		DARWIN_UML_STATIC_PERMISSION);
	audit();
	require(protect(generation, base, 2, rwx) == 0);
	require(count_of(record_of(generation, base)) == 1);
	audit();
	destroy(generation);
}

/* A record that would count past its width stops counting for good, and a
 * departure that finds nothing to count out is contained and seen. */
void test_the_edges_of_the_word()
{
	constexpr unsigned long generation = 714;
	constexpr unsigned long base = 0x2a00'0000;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin11{1011, backing};
	must_map(generation, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(generation, base);

	/* Full count: the next mapping marks the record never-share. */
	MappedBacking::preset_for_testing(record, (std::uint64_t{1} << 24) - 1, 7);
	must_map(generation, base + page, backing, 0, 1, rwx, 1);
	darwin_uml_static_code_page_stamp stamp{};
	void *again = nullptr;
	require(darwin_uml_static_vm_code_page(generation, base + page, &again,
		&stamp) == DARWIN_UML_STATIC_TRANSLATED);
	require((stamp.flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	const auto frozen = stamp.epoch;
	unmap(generation, base, 2);
	/* It counts no more and the epoch stays. */
	stamp = stamp_of(record);
	require((stamp.flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0 &&
		stamp.epoch == frozen);
	audit();

	/* The largest epoch: one more step is the same mark. */
	ScratchBacking other{backing_name, 2 * page};
	Pin pin12{1012, other};
	must_map(generation, base + 4 * page, other, 0, 1, rwx, 1);
	auto *fresh = record_of(generation, base + 4 * page);
	MappedBacking::preset_for_testing(fresh, 1, (std::uint64_t{1} << 39) - 1);
	require((stamp_of(fresh).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0);
	darwin_uml_static_vm_invalidate_code_page(fresh);
	require((stamp_of(fresh).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	unmap(generation, base + 4 * page, 1);
	audit();

	/* A change that keeps the record is no count at all: at the largest
	 * count it marks nothing. */
	ScratchBacking fourth{backing_name, 2 * page};
	Pin pin16{1016, fourth};
	must_map(generation, base + 12 * page, fourth, 0, 1, rwx, 1);
	auto *kept = record_of(generation, base + 12 * page);
	MappedBacking::preset_for_testing(kept, (std::uint64_t{1} << 24) - 1, 3);
	must_map(generation, base + 12 * page, fourth, 0, 1, rwx, 3);
	must_map(generation, base + 12 * page, fourth, 0, 1, rx, 3);
	require((stamp_of(kept).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0 &&
		count_of(kept) == (std::uint64_t{1} << 24) - 1 &&
		stamp_of(kept).epoch == 3);
	MappedBacking::preset_for_testing(kept, 1, 3);
	unmap(generation, base + 12 * page, 1);
	require(count_of(kept) == 0 && stamp_of(kept).epoch == 4);
	audit();

	/* A departure at zero: the count stays zero, the epoch moves, and the
	 * anomaly is counted. */
	ScratchBacking third{backing_name, 2 * page};
	Pin pin13{1013, third};
	must_map(generation, base + 8 * page, third, 0, 1, rwx, 1);
	auto *wrong = record_of(generation, base + 8 * page);
	MappedBacking::preset_for_testing(wrong, 0, 5);
	require(testing::count_anomalies() == expected_anomalies);
	unmap(generation, base + 8 * page, 1);
	require(count_of(wrong) == 0 && stamp_of(wrong).epoch == 6);
	require(testing::count_anomalies() == expected_anomalies + 1);
	++expected_anomalies;
	/* It also stopped all sharing, which the next row looks at. */
	testing::clear_sharing_latch();
	destroy(generation);
}

/* An accounting anomaly latches sharing off for the whole process, in every
 * build: the counts are not exact any more, so no record answers a verdict,
 * whichever frame it names. */
void test_an_accounting_anomaly_stops_all_sharing()
{
	constexpr unsigned long generation = 718;
	constexpr unsigned long base = 0x2f00'0000;
	ScratchBacking one{backing_name, 2 * page}, other{backing_name, 2 * page};
	Pin pin19{1019, one}, pin20{1020, other};
	must_map(generation, base, one, 0, 1, rwx, 3);
	must_map(generation, base + page, other, 0, 1, rwx, 3);
	auto *spoiled = record_of(generation, base);
	auto *innocent = record_of(generation, base + page);
	require((stamp_of(spoiled).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0);
	require((stamp_of(innocent).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0);
	require(!uml::memory::sharing_latched_off());
	MappedBacking::preset_for_testing(spoiled, 0, 2);
	unmap(generation, base, 1);
	++expected_anomalies;
	require(uml::memory::sharing_latched_off());
	require((stamp_of(spoiled).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	require((stamp_of(innocent).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	/* A page mapped afterwards is just as refused. */
	ScratchBacking later{backing_name, 2 * page};
	Pin pin21{1021, later};
	must_map(generation, base + 2 * page, later, 0, 1, rwx, 3);
	require((stamp_of(record_of(generation, base + 2 * page)).flags &
		DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	testing::clear_sharing_latch();
	require((stamp_of(innocent).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0);
	destroy(generation);
	audit();
}

/* "Never share" is for good. A record that cannot count exactly any more
 * (its count overflowed, or its epoch ran out) answers no verdict, so
 * nothing may clear the bit again: a publication moves the legacy version of
 * the page and nothing else, and the mapping counts stay out of it. 2^39
 * publications are out of reach of a test, so the rows set the whole word at
 * the edge: the epoch at its largest with the bit set, which an unconditional
 * step carries out of the word and clears. */
void test_never_share_survives_publications_and_the_exhausted_epoch()
{
	constexpr unsigned long generation = 715;
	constexpr unsigned long base = 0x2d00'0000;
	constexpr std::uint64_t never_share = std::uint64_t{1} << 63;
	constexpr std::uint64_t count_mask = (std::uint64_t{1} << 24) - 1;
	constexpr std::uint64_t epoch_mask =
		((std::uint64_t{1} << 39) - 1) << 24;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin17{1017, backing};
	must_map(generation, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(generation, base);

	/* After the count overflowed: publications leave the bit and the epoch. */
	MappedBacking::preset_for_testing(record, count_mask, 7);
	must_map(generation, base + page, backing, 0, 1, rwx, 1);
	require((stamp_of(record).flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	const auto marked = MappedBacking::word_for_testing(record);
	auto version = stamp_of(record).version;
	for (int step = 0; step < 3; ++step) {
		darwin_uml_static_vm_invalidate_code_page(record);
		require(MappedBacking::word_for_testing(record) == marked);
		require(stamp_of(record).version == ++version);
	}
	unmap(generation, base + page, 1);
	require(MappedBacking::word_for_testing(record) == marked);

	/* After the epoch ran out: bit set, epoch at its largest. */
	const auto exhausted = never_share | epoch_mask | count_mask;
	MappedBacking::preset_word_for_testing(record, exhausted);
	for (int step = 0; step < 3; ++step) {
		darwin_uml_static_vm_invalidate_code_page(record);
		require(MappedBacking::word_for_testing(record) == exhausted);
		require((stamp_of(record).flags &
			DARWIN_UML_STATIC_STAMP_NEVER_SHARE) != 0);
	}
	must_map(generation, base + page, backing, 0, 1, rwx, 1);
	unmap(generation, base + page, 1);
	require(MappedBacking::word_for_testing(record) == exhausted);

	/* Running out of epoch is itself the mark: one publication at the
	 * largest epoch sets the bit, and the next ones leave it. */
	MappedBacking::preset_word_for_testing(record, epoch_mask);
	darwin_uml_static_vm_invalidate_code_page(record);
	require(MappedBacking::word_for_testing(record) == never_share);
	darwin_uml_static_vm_invalidate_code_page(record);
	require(MappedBacking::word_for_testing(record) == never_share);
	MappedBacking::preset_word_for_testing(record, 1);
	unmap(generation, base, 1);
	require(MappedBacking::word_for_testing(record) == (std::uint64_t{1} << 24));
	destroy(generation);
	audit();
}

/* The audit a diagnostics build runs on a real guest (count_audit.hpp): it
 * recounts the executable words of every generation, holding all of them at
 * once, and says whether it could. A wrong count is a mismatch it reports,
 * and a departure that found nothing to count out is an anomaly it counts,
 * with no test-only machinery behind it. */
void test_the_audit_of_a_guest_run_sees_a_wrong_count()
{
	constexpr unsigned long first = 716, second = 717;
	constexpr unsigned long base = 0x2e00'0000;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin18{1018, backing};
	must_map(first, base, backing, 0, 2, rwx, 3);
	must_map(second, base, backing, 0, 2, rx, 3);
	auto *record = record_of(first, base);
	const auto before = uml::memory::audit_executable_counts();
	require(before.exact && before.mismatches == 0 && before.records >= 2);
	require(uml::memory::count_anomalies() == expected_anomalies);

	/* A count that is too high, then one that is too low: one record each. */
	MappedBacking::preset_for_testing(record, 5, 0);
	auto audit_now = uml::memory::audit_executable_counts();
	require(audit_now.exact && audit_now.mismatches == 1);
	MappedBacking::preset_for_testing(record, 1, 0);
	audit_now = uml::memory::audit_executable_counts();
	require(audit_now.exact && audit_now.mismatches == 1);
	MappedBacking::preset_for_testing(record, 2, 0);
	audit_now = uml::memory::audit_executable_counts();
	require(audit_now.exact && audit_now.mismatches == 0);

	/* A departure at zero is contained by the count and counted as an
	 * anomaly: each of the two address spaces leaves it once. */
	MappedBacking::preset_for_testing(record, 0, 4);
	unmap(first, base, 2);
	unmap(second, base, 2);
	expected_anomalies += 2;
	require(uml::memory::count_anomalies() == expected_anomalies);
	testing::clear_sharing_latch();
	destroy(first);
	destroy(second);
	audit();
}

/* Several address spaces, one frame, mapped and unmapped from several
 * threads at once: nothing is lost and nothing is counted twice. */
void test_threads_mapping_one_frame_keep_the_count_exact()
{
	constexpr unsigned long threads = 6;
	constexpr unsigned long rounds = 400;
	constexpr unsigned long base = 0x2b00'0000;
	ScratchBacking backing{backing_name, 2 * page};
	Pin pin14{1014, backing};
	must_map(800, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(800, base);
	std::atomic<bool> go{false};
	std::atomic<unsigned long> unmaps{};
	std::vector<std::thread> workers;
	for (unsigned long index = 0; index < threads; ++index)
		workers.emplace_back([&, index] {
			const unsigned long generation = 810 + index;
			while (!go.load(std::memory_order_acquire)) {}
			for (unsigned long round = 0; round < rounds; ++round) {
				must_map(generation, base, backing, 0, 1,
					round % 2 ? rx : rwx, round % 3 ? 1 : 3);
				if (round % 5 == 0)
					require(protect(generation, base, 1, rw) == 0);
				unmap(generation, base, 1);
				unmaps.fetch_add(1, std::memory_order_relaxed);
			}
			require(darwin_uml_static_vm_destroy(generation) == 0);
		});
	const auto start = stamp_of(record).epoch;
	go.store(true, std::memory_order_release);
	for (auto &worker : workers)
		worker.join();
	/* The resident mapping held the count above zero throughout. */
	require(count_of(record) == 1 && stamp_of(record).epoch == start);
	audit();
	unmap(800, base, 1);
	require(count_of(record) == 0 && stamp_of(record).epoch == start + 1);
	destroy(800);
	audit();
}

/* The same without a resident mapping: the count returns to zero and the
 * epoch moved, at most once per departure. */
void test_threads_that_come_and_go_return_the_count_to_zero()
{
	constexpr unsigned long threads = 6;
	constexpr unsigned long rounds = 400;
	constexpr unsigned long base = 0x2c00'0000;
	ScratchBacking backing{backing_name, 2 * page};
	Pin pin15{1015, backing};
	must_map(900, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(900, base);
	unmap(900, base, 1);
	const auto start = stamp_of(record).epoch;
	std::atomic<bool> go{false};
	std::vector<std::thread> workers;
	for (unsigned long index = 0; index < threads; ++index)
		workers.emplace_back([&, index] {
			const unsigned long generation = 910 + index;
			while (!go.load(std::memory_order_acquire)) {}
			for (unsigned long round = 0; round < rounds; ++round) {
				must_map(generation, base, backing, 0, 1, rwx, 1);
				unmap(generation, base, 1);
			}
			require(darwin_uml_static_vm_destroy(generation) == 0);
		});
	go.store(true, std::memory_order_release);
	for (auto &worker : workers)
		worker.join();
	const auto end = stamp_of(record).epoch;
	require(count_of(record) == 0);
	require(end > start && end <= start + threads * rounds);
	audit();
	destroy(900);
}

/* An audit that says it held every generation is a fact while generations
 * come and go: a generation made after the audit began cannot count a mapping
 * in that the recount missed. One thread makes generations, maps the frames
 * of one backing into each and destroys the older ones; another audits
 * without pause, and every audit that was exact finds nothing. */
void test_an_exact_audit_is_exact_while_generations_come_and_go()
{
	constexpr unsigned long rounds = 3000;
	constexpr unsigned long window = 6;
	constexpr unsigned long frames = 48;
	constexpr unsigned long base = 0x2d00'0000;
	ScratchBacking backing{backing_name, frames * page};
	Pin pin19{1019, backing};
	std::atomic<bool> finished{false};
	std::atomic<unsigned long> exact_audits{};
	std::atomic<unsigned long> wrong_audits{};
	std::atomic<unsigned long> first_wrong{};
	std::thread auditor{[&] {
		while (!finished.load(std::memory_order_acquire)) {
			const auto result = uml::memory::audit_executable_counts();
			if (!result.exact)
				continue;
			exact_audits.fetch_add(1, std::memory_order_relaxed);
			if (result.mismatches != 0) {
				unsigned long none = 0;
				first_wrong.compare_exchange_strong(none,
					result.mismatches);
				wrong_audits.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}};
	/* At least `rounds` generations, and on until the audit has been exact
	 * enough times to mean something (a busy mapper can starve it). */
	unsigned long made = 0;
	while (made < rounds || (exact_audits.load() < 16 && made < 20 * rounds)) {
		must_map(50000 + made, base, backing, 0, frames, rx, 3);
		if (made >= window)
			destroy(50000 + made - window);
		++made;
	}
	for (unsigned long round = made - window; round < made; ++round)
		destroy(50000 + round);
	finished.store(true, std::memory_order_release);
	auditor.join();
	std::printf("exact audits %lu, with a mismatch %lu (first: %lu records)\n",
		exact_audits.load(), wrong_audits.load(), first_wrong.load());
	require(exact_audits.load() >= 16);
	require(wrong_audits.load() == 0);
	audit();
}


std::atomic<int> destroy_window{};
constexpr int window_open = 1;
constexpr int window_held = 2;
constexpr int window_released = 3;

/* Holds the first destroy() that gets here, between the clearing of its
 * generation and the retirement of the state, until the test lets it go. */
void hold_destroy_in_the_window(testing::AllocationSite site) noexcept
{
	if (site != testing::AllocationSite::destroy_cleared)
		return;
	int open = window_open;
	if (!destroy_window.compare_exchange_strong(open, window_held))
		return;
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds{20};
	while (destroy_window.load(std::memory_order_acquire) != window_released &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
}

/* A walker batch that began before destroy() and commits while destroy has
 * cleared its generation and not yet retired it must be refused: its words
 * would count in and the state, with them, be erased, leaving the frame's
 * count high for good. */
void test_a_batch_cannot_commit_into_a_generation_being_destroyed()
{
	constexpr unsigned long generation = 726;
	constexpr unsigned long other = 727;
	constexpr unsigned long base = 0x3000'0000;
	ScratchBacking backing{backing_name, 4 * page};
	Pin pin{1026, backing};
	must_map(other, base, backing, 0, 1, rwx, 1);
	auto *record = record_of(other, base);
	require(count_of(record) == 1);

	require(darwin_uml_static_vm_walker_lock(generation) == 0);
	require(darwin_uml_static_vm_batch_begin(generation) == 0);
	must_map(generation, base, backing, 0, 1, rwx, 1);

	destroy_window.store(window_open, std::memory_order_release);
	testing::set_allocation_observer(hold_destroy_in_the_window);
	std::thread destroyer{[&] { destroy(generation); }};
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds{20};
	while (destroy_window.load(std::memory_order_acquire) != window_held &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	require(destroy_window.load(std::memory_order_acquire) == window_held);
	const int result = darwin_uml_static_vm_batch_end();
	destroy_window.store(window_released, std::memory_order_release);
	destroyer.join();
	testing::set_allocation_observer(nullptr);
	require(darwin_uml_static_vm_walker_unlock(generation) == 0);

	require(result == -EAGAIN);
	require(count_of(record) == 1);
	audit();
	destroy(other);
	require(count_of(record) == 0);
	audit();
}


std::atomic<int> immediate_window{};

/* Holds the first immediate map, protect or unmap that gets here, after it has
 * taken its generation's state and before it takes the state's lock, until
 * the test lets it go. */
void hold_immediate_change(testing::AllocationSite site) noexcept
{
	if (site != testing::AllocationSite::immediate_change)
		return;
	int open = window_open;
	if (!immediate_window.compare_exchange_strong(open, window_held))
		return;
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds{20};
	while (immediate_window.load(std::memory_order_acquire) != window_released &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
}

/* Runs `change` on a thread that is held after it took the state of
 * `generation`, destroys the generation meanwhile, lets the change go on, and
 * returns what it returned. */
template <typename Change>
int change_across_a_destroy(unsigned long generation, Change change)
{
	int result = -1;
	immediate_window.store(window_open, std::memory_order_release);
	testing::set_allocation_observer(hold_immediate_change);
	std::thread changer{[&] { result = change(); }};
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds{20};
	while (immediate_window.load(std::memory_order_acquire) != window_held &&
	       std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	require(immediate_window.load(std::memory_order_acquire) == window_held);
	destroy(generation);
	immediate_window.store(window_released, std::memory_order_release);
	changer.join();
	testing::set_allocation_observer(nullptr);
	return result;
}

/* A map, protect or unmap that took its generation's state before destroy()
 * retired it takes the state's lock after: it must leave the state alone and
 * return what it would have returned. A map that wrote there would count its
 * words into a state that is no longer in the table, which no audit walks and
 * nothing flushes, and leave the frame's count high for good. The protect and
 * unmap arms pass without their guards (destroy has already emptied the
 * state): they are regression cover, not proof that those guards ran. */
void test_an_immediate_change_cannot_write_into_a_destroyed_generation()
{
	/* A generation is never used again once destroyed: one for each change. */
	constexpr unsigned long mapper = 728;
	constexpr unsigned long protector = 730;
	constexpr unsigned long unmapper = 731;
	constexpr unsigned long later = 732;
	constexpr unsigned long other = 729;
	constexpr unsigned long base = 0x3300'0000;
	ScratchBacking backing{backing_name, 6 * page};
	Pin pin{1051, backing};
	must_map(other, base, backing, 0, 6, rwx, 1);
	std::array<void *, 6> records{};
	for (unsigned long index = 0; index < records.size(); ++index)
		records[index] = record_of(other, base + index * page);

	const auto stage = [&](unsigned long generation) {
		must_map(generation, base, backing, 0, 1, rwx, 1);
		must_map(generation, base + page, backing, page, 1, rw, 1);
		require(count_of(records[0]) == 2 && count_of(records[1]) == 1);
	};

	stage(mapper);
	require(change_across_a_destroy(mapper, [&] {
		return map(mapper, base + 2 * page, backing, 2 * page, 1, rwx, 1);
	}) == 0);
	require(count_of(records[0]) == 1);
	require(count_of(records[2]) == 1);
	audit();

	stage(protector);
	require(change_across_a_destroy(protector, [&] {
		return protect(protector, base + page, 1, rwx);
	}) == 0);
	require(count_of(records[0]) == 1 && count_of(records[1]) == 1);
	audit();

	stage(unmapper);
	require(change_across_a_destroy(unmapper, [&] {
		return darwin_uml_static_vm_unmap_generation(unmapper, at(base),
			page);
	}) == 0);
	require(count_of(records[0]) == 1);
	audit();

	/* A generation that was not destroyed is counted as before. */
	must_map(later, base + 3 * page, backing, 3 * page, 1, rwx, 1);
	require(count_of(records[3]) == 2);
	audit();
	destroy(later);
	require(count_of(records[3]) == 1);
	destroy(other);
	for (void *record : records)
		require(count_of(record) == 0);
	audit();
}


/* A whole-space flush and destroy count every executable mapping out,
 * whatever the number of pages: the sizes around one leaf of the fill index
 * (512 words) and past several of them. The mappings of a second generation
 * to half of the pages keep their counts through it. */
void test_a_large_space_counts_every_mapping_out()
{
	constexpr unsigned long base = 0x3200'0000;
	unsigned long round = 0;
	for (const unsigned long pages : {511ul, 512ul, 513ul, 1024ul}) {
		const unsigned long generation = 740 + round;
		const unsigned long other = 760 + round;
		ScratchBacking backing{backing_name, pages * page};
		Pin pin{1040 + round, backing};
		must_map(generation, base, backing, 0, pages, rwx, 1);
		must_map(other, base, backing, 0, pages / 2, rwx, 1);
		std::vector<void *> records(pages);
		std::vector<std::uint64_t> epochs(pages);
		for (unsigned long index = 0; index < pages; ++index) {
			records[index] = record_of(generation, base + index * page);
			epochs[index] = stamp_of(records[index]).epoch;
			require(count_of(records[index]) == (index < pages / 2 ? 2u : 1u));
		}
		audit();
		/* The flush departs this generation's mappings and no others. */
		require(darwin_uml_static_vm_flush_generation(generation) == 0);
		for (unsigned long index = 0; index < pages; ++index) {
			require(count_of(records[index]) == (index < pages / 2 ? 1u : 0u));
			require(stamp_of(records[index]).epoch ==
				epochs[index] + (index < pages / 2 ? 0u : 1u));
		}
		audit();
		/* Destroy of a generation that was not flushed counts the same. */
		must_map(generation, base, backing, 0, pages, rwx, 1);
		destroy(generation);
		for (unsigned long index = 0; index < pages; ++index)
			require(count_of(records[index]) == (index < pages / 2 ? 1u : 0u));
		audit();
		destroy(other);
		for (unsigned long index = 0; index < pages; ++index) {
			require(count_of(records[index]) == 0);
			require(stamp_of(records[index]).epoch > epochs[index]);
		}
		audit();
		++round;
	}
}

} // namespace

int main()
{
	test_the_last_departure_moves_the_epoch();
	test_mappings_in_two_spaces_hold_one_count();
	test_protect_counts_both_ways();
	test_a_change_that_keeps_the_record_counts_nothing();
	test_a_page_that_cannot_hold_code_counts_nothing();
	test_remapping_moves_the_count_to_the_new_frame();
	test_a_batch_counts_its_net_effect();
	test_flush_and_destroy_count_once();
	test_an_allocation_failure_changes_no_count();
	test_protect_that_cannot_make_a_record_changes_nothing();
	test_the_edges_of_the_word();
	test_never_share_survives_publications_and_the_exhausted_epoch();
	test_the_audit_of_a_guest_run_sees_a_wrong_count();
	test_an_accounting_anomaly_stops_all_sharing();
	test_threads_mapping_one_frame_keep_the_count_exact();
	test_threads_that_come_and_go_return_the_count_to_zero();
	test_an_exact_audit_is_exact_while_generations_come_and_go();
	test_a_batch_cannot_commit_into_a_generation_being_destroyed();
	test_an_immediate_change_cannot_write_into_a_destroyed_generation();
	test_a_large_space_counts_every_mapping_out();
	std::puts("Static MMU executable count tests passed");
	return 0;
}
