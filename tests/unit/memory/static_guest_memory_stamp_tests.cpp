// SPDX-License-Identifier: GPL-2.0-or-later
/* What StaticGuestMemory reports as the stamp of a code page: the identity,
 * the version, the epoch, and whether the page may be shared -- the kernel's
 * immutable fact for the mapping the page is fetched through, on a record
 * that still counts exactly. The stamp the translation caches keep stays
 * the two words it was. */
#include "memory/static_guest_memory.hpp"
#include "cpu/arm64/aot/shared_verdicts.hpp"
#include "allocation_fault.hpp"
#include "mapped_backing.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <sys/mman.h>

namespace {

using uml::memory::CodePageStamp;
using uml::memory::ExtendedCodePageStamp;
using uml::memory::StaticGuestMemory;
using uml::testing::ScratchBacking;
using uml::testing::require;

constexpr std::string_view backing_name = "uml-static-guest-stamp-XXXXXX";
constexpr unsigned long page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr int rwx = PROT_READ | PROT_WRITE | PROT_EXEC;

void map(unsigned long generation, unsigned long address,
	ScratchBacking &backing, unsigned long offset, int protection, int code)
{
	require(darwin_uml_static_vm_map_generation(generation,
		reinterpret_cast<void *>(address), backing.descriptor(), offset,
		page, protection, code, 1) == 0);
}

[[nodiscard]] bool stamp_of(StaticGuestMemory &memory, unsigned long address,
	ExtendedCodePageStamp &stamp)
{
	return memory.extended_code_page_stamp(address, nullptr, stamp);
}

void test_the_stamp_says_whether_the_page_may_be_shared()
{
	constexpr unsigned long generation = 601;
	constexpr unsigned long base = 0x1000'0000;
	ScratchBacking backing{backing_name, 4 * page};
	map(generation, base, backing, 0, rwx, 3);
	map(generation, base + page, backing, 0, rwx, 1);
	map(generation, base + 2 * page, backing, page, PROT_READ | PROT_EXEC, 3);
	/* Immutable, but not a page a translator can hold. */
	map(generation, base + 3 * page, backing, 2 * page,
		PROT_READ | PROT_WRITE, 3);
	require(darwin_uml_static_vm_activate(generation) == 0);

	StaticGuestMemory memory;
	memory.set_generation(generation);
	std::array<ExtendedCodePageStamp, 3> stamps{};
	for (std::size_t index = 0; index < stamps.size(); ++index)
		require(stamp_of(memory, base + index * page, stamps[index]));
	require(stamps[0].shareable && !stamps[1].shareable && stamps[2].shareable);
	/* The fact itself is what admits rewritten code to a page; sharing needs
	 * more. */
	require(stamps[0].immutable && !stamps[1].immutable && stamps[2].immutable);

	/* The two mappings of one frame name the same page at the same version
	 * and the same epoch; the fact is the mapping's. */
	require(stamps[0].stamp == stamps[1].stamp &&
		stamps[0].epoch == stamps[1].epoch);
	require(!(stamps[0].stamp == stamps[2].stamp));

	/* The stamp the translation caches keep is the same two words. */
	CodePageStamp plain{};
	require(memory.code_page_stamp(base, nullptr, plain));
	require(plain == stamps[0].stamp);
	require(sizeof(CodePageStamp) == 2 * sizeof(std::uint64_t));

	ExtendedCodePageStamp unused{};
	require(!stamp_of(memory, base + 3 * page, unused));

	/* A publication moves the version and the epoch; the stamp taken again
	 * through the cached entry still carries the mapping's fact. */
	void *handle = nullptr;
	darwin_uml_static_code_page_stamp raw{};
	require(darwin_uml_static_vm_code_page(generation, base, &handle, &raw) ==
		DARWIN_UML_STATIC_TRANSLATED);
	darwin_uml_static_vm_invalidate_code_page(handle);
	ExtendedCodePageStamp after{};
	require(stamp_of(memory, base, after));
	require(after.stamp.identity == stamps[0].stamp.identity &&
		after.stamp.version == stamps[0].stamp.version + 1 &&
		after.epoch == stamps[0].epoch + 1 && after.shareable);
	ExtendedCodePageStamp alias{};
	require(stamp_of(memory, base + page, alias));
	require(alias.stamp == after.stamp && alias.epoch == after.epoch &&
		!alias.shareable && !alias.immutable);

	/* Flipping the fact is a mapping change, which the executor consumes
	 * before it asks again. */
	map(generation, base + page, backing, 0, rwx, 3);
	memory.set_generation(generation);
	require(stamp_of(memory, base + page, alias));
	require(alias.shareable && alias.immutable);
	map(generation, base, backing, 0, rwx, 1);
	memory.set_generation(generation);
	require(stamp_of(memory, base, after));
	require(!after.shareable && !after.immutable);
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* How the host before the fact read the kernel's `guest_code`, as that host's
 * source has it (darwin/memory/mmu/address_space.cpp at 872c64c7, in
 * darwin_uml_static_vm_map and _map_generation: `guest_code != 0`). Nothing
 * here runs that host or a kernel of that time or of this: the rule is a
 * reading of the earlier source, kept as a function so that this host's
 * decoding of every value is held against it. */
constexpr bool earlier_host_reads_guest_code(int value)
{
	return value != 0;
}

/* The values a kernel before the fact passes (0 and 1) and the ones a kernel
 * with it passes (also 3), and the others the ABI's bit set allows. */
constexpr bool kernel_before_the_fact_passes(int value)
{
	return value == 0 || value == 1;
}

/* A kernel of another version of the bridge meets this host. The ABI's
 * `guest_code` is a bit set, and a host and a kernel of different versions
 * agree on every value either may pass. A kernel from before the fact passes
 * only 0 and 1 and a kernel with it also 3: what is guest code is what the
 * value says, whatever bit 1 and the bits above it say (an earlier host, which
 * reads any nonzero value as guest code, agrees), and the fact is bit 1 with
 * bit 0 and nothing else. So the older kernel on this host is never told a
 * page is immutable, and the newer kernel on the earlier host loses nothing it
 * had: its pages are code, and the fact it adds is not read. This row is this
 * host's decoding judged against the earlier host's rule as that source reads;
 * a mixed pair of host and kernel is not run. */
void test_kernels_of_other_versions_agree_with_this_host_on_every_value()
{
	constexpr unsigned long generation = 604;
	constexpr unsigned long base = 0x1400'0000;
	ScratchBacking backing{backing_name, 8 * page};
	for (int code = 0; code < 8; ++code)
		map(generation, base + code * page, backing, code * page, rwx, code);
	require(darwin_uml_static_vm_activate(generation) == 0);
	StaticGuestMemory memory;
	memory.set_generation(generation);
	for (int code = 0; code < 8; ++code) {
		ExtendedCodePageStamp stamp{};
		const bool found = stamp_of(memory, base + code * page, stamp);
		/* The earlier host's reading: any nonzero value is guest code. */
		require(found == earlier_host_reads_guest_code(code));
		/* This host's fact: bit 1 with bit 0. Kernels that never set bit 1
		 * (the values 0 and 1) never produce it. */
		const bool immutable = found && stamp.immutable;
		require(immutable == ((code & 3) == 3));
		if (kernel_before_the_fact_passes(code))
			require(!immutable && !(found && stamp.shareable));
		std::printf("guest_code %d: code %d immutable %d\n", code,
			found ? 1 : 0, immutable ? 1 : 0);
	}
	require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* A verdict filed for a page is answered to the next address space until the
 * page's epoch moves; a record that ran out of count or of epoch is marked
 * never-share for good and answers none, whatever the epoch field says
 * after later publications. Through the table's own entry point and the
 * stamps the memory layer reports, the sequence that clears the mark in an
 * unconditional epoch step: the count overflows with the epoch at its
 * largest, every mapping goes, the frame is mapped again and a change is
 * published. */
void test_an_exhausted_record_answers_no_old_verdict()
{
	namespace aot = uml::cpu::arm64::aot;
	constexpr unsigned long first = 801, second = 802;
	constexpr unsigned long base = 0x1100'0000;
	constexpr int rx = PROT_READ | PROT_EXEC;
	ScratchBacking backing{backing_name, 2 * page};
	/* A mapping that is not code keeps the frame's record while the
	 * address spaces unmap it. */
	map(803, 0x1000, backing, 0, PROT_READ, 0);
	map(first, base, backing, 0, rx, 3);
	map(second, base, backing, 0, rx, 3);
	require(darwin_uml_static_vm_activate(first) == 0);
	StaticGuestMemory one, two;
	one.set_generation(first);
	two.set_generation(second);
	auto table = std::make_unique<aot::VerdictTable>();
	std::size_t hashed = 0;
	const auto ask = [&](StaticGuestMemory &memory) {
		return aot::verify_through_table(*table,
			[&](std::uint64_t at, aot::PageStamp &stamp) noexcept {
				ExtendedCodePageStamp seen;
				if (!memory.extended_code_page_stamp(at, nullptr, seen))
					return false;
				stamp = {seen.stamp.identity, seen.epoch, seen.shareable};
				return true;
			},
			[&]() noexcept {
				++hashed;
				return aot::SharedOutcome::match;
			},
			[]() noexcept { return false; },
			base, aot::VerdictTable::where(1, 0));
	};
	require(ask(one) == aot::SharedOutcome::match && hashed == 1);
	require(ask(two) == aot::SharedOutcome::shared_hit && hashed == 1);

	void *record = nullptr;
	darwin_uml_static_code_page_stamp raw{};
	require(darwin_uml_static_vm_code_page(first, base, &record, &raw) ==
		DARWIN_UML_STATIC_TRANSLATED);
	uml::memory::MappedBacking::preset_for_testing(record,
		(std::uint64_t{1} << 24) - 1, (std::uint64_t{1} << 39) - 1);
	uml::memory::MappedBacking::executable_mapping_added(record);
	require(darwin_uml_static_vm_unmap_generation(first,
		reinterpret_cast<void *>(base), page) == 0);
	require(darwin_uml_static_vm_unmap_generation(second,
		reinterpret_cast<void *>(base), page) == 0);
	map(second, base, backing, 0, rx, 3);
	darwin_uml_static_vm_invalidate_code_page(record);
	two.set_generation(second);
	ExtendedCodePageStamp after{};
	require(stamp_of(two, base, after));
	require(!after.shareable && after.immutable);
	/* The old verdict stays unanswered, and so does whatever is filed. */
	require(ask(two) == aot::SharedOutcome::match && hashed == 2);
	require(ask(two) == aot::SharedOutcome::match && hashed == 3);
	require(darwin_uml_static_vm_destroy(first) == 0);
	require(darwin_uml_static_vm_destroy(second) == 0);
	require(darwin_uml_static_vm_destroy(803) == 0);
}

/* Once the accounting has been seen to fail anywhere, no stamp says
 * shareable, whichever frame it names; the kernel's fact is still the
 * mapping's. */
void test_an_accounting_anomaly_leaves_nothing_shareable()
{
	namespace testing = uml::memory::testing;
	constexpr unsigned long generation = 804;
	constexpr unsigned long base = 0x1200'0000;
	constexpr int rx = PROT_READ | PROT_EXEC;
	ScratchBacking good{backing_name, 2 * page}, bad{backing_name, 2 * page};
	map(805, 0x1000, good, 0, PROT_READ, 0);
	map(806, 0x1000, bad, 0, PROT_READ, 0);
	map(generation, base, good, 0, rx, 3);
	map(generation, base + page, bad, 0, rx, 3);
	require(darwin_uml_static_vm_activate(generation) == 0);
	StaticGuestMemory memory;
	memory.set_generation(generation);
	ExtendedCodePageStamp stamp{};
	require(stamp_of(memory, base, stamp) && stamp.shareable && stamp.immutable);
	void *record = nullptr;
	darwin_uml_static_code_page_stamp raw{};
	require(darwin_uml_static_vm_code_page(generation, base + page, &record,
		&raw) == DARWIN_UML_STATIC_TRANSLATED);
	uml::memory::MappedBacking::preset_for_testing(record, 0, 1);
	require(darwin_uml_static_vm_unmap_generation(generation,
		reinterpret_cast<void *>(base + page), page) == 0);
	require(uml::memory::sharing_latched_off());
	require(stamp_of(memory, base, stamp) && !stamp.shareable && stamp.immutable);
	testing::clear_sharing_latch();
	require(stamp_of(memory, base, stamp) && stamp.shareable);
	require(darwin_uml_static_vm_destroy(generation) == 0);
	require(darwin_uml_static_vm_destroy(805) == 0);
	require(darwin_uml_static_vm_destroy(806) == 0);
}

} // namespace

int main()
{
	test_the_stamp_says_whether_the_page_may_be_shared();
	test_kernels_of_other_versions_agree_with_this_host_on_every_value();
	test_an_exhausted_record_answers_no_old_verdict();
	test_an_accounting_anomaly_leaves_nothing_shareable();
	std::puts("Static guest memory stamp tests passed");
	return 0;
}
