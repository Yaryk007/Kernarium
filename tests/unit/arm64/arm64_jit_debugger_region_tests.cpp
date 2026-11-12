// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_arena.hpp"
#include "cpu/arm64/jit/jit_linking.hpp"
#include "cpu/jit_memory/executable_memory.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

/* The region an iOS device's debugger prepares, exercised without a debugger:
 * its mapping, the alias instructions are written through, the lowest-first
 * allocation of whole granules, and the arena that writes through the alias
 * and runs from the executable mapping. */
namespace {

namespace jit_memory = ::uml::cpu::jit_memory;
namespace jit = ::uml::cpu::arm64::jit::detail;
using uml::testing::require;

constexpr std::size_t kib = 1024U;
constexpr std::size_t granule = 16U * kib;
constexpr std::uintptr_t tib = std::uintptr_t{1} << 40U;

[[nodiscard]] std::uint32_t call(const void *code)
{
	return reinterpret_cast<std::uint32_t (*)()>(const_cast<void *>(code))();
}

/* `mov w0, #value; ret`. */
[[nodiscard]] std::array<std::uint32_t, 2> constant(std::uint16_t value)
{
	return {0x52800000U | (std::uint32_t{value} << 5U), 0xd65f03c0U};
}

void nothing_is_available_before_a_region_is_prepared()
{
	require(!jit_memory::available(), "JIT memory before a region");
	require(jit_memory::allocate(granule) == nullptr,
		"allocation before a region");
	require(jit_memory::prepared_region_bytes() == 0, "bytes before a region");
	int local = 0;
	require(jit_memory::writable_alias(&local) == &local,
		"an address outside the region has an alias");
	require(jit_memory::debugger_route(), "the variant has no route");
}

void a_region_is_prepared_once_and_only_grows_by_a_new_process()
{
	require(jit_memory::prepare_debugger_region(0) ==
			jit_memory::RegionStatus::failed,
		"an empty region was prepared");
	require(jit_memory::prepare_debugger_region(100 * kib) ==
			jit_memory::RegionStatus::ready,
		"the region was not prepared");
	require(jit_memory::available(), "JIT memory after the region");
	require(jit_memory::prepared_region_bytes() == 112 * kib,
		"the region is not whole granules");
	require(jit_memory::prepare_debugger_region(16 * kib) ==
			jit_memory::RegionStatus::ready,
		"a smaller request after the region was not ready");
	require(jit_memory::prepare_debugger_region(112 * kib) ==
			jit_memory::RegionStatus::ready,
		"the same request after the region was not ready");
	require(jit_memory::prepare_debugger_region(112 * kib + 1) ==
			jit_memory::RegionStatus::smaller,
		"a larger request after the region was not refused");
}

void allocation_serves_whole_granules_lowest_first()
{
	/* Seven granules in all: one, three, one, and the two that remain. */
	void *first = jit_memory::allocate(granule);
	void *second = jit_memory::allocate(2 * granule + 1);
	void *third = jit_memory::allocate(granule);
	require(first != nullptr && second != nullptr && third != nullptr,
		"allocation failed");
	const auto a = reinterpret_cast<std::uintptr_t>(first);
	const auto b = reinterpret_cast<std::uintptr_t>(second);
	const auto c = reinterpret_cast<std::uintptr_t>(third);
	require(a % granule == 0 && b == a + granule && c == b + 3 * granule,
		"allocations are not consecutive whole granules");
	require(a + 7 * granule <= tib, "the region is above 1 TiB");

	void *rest = jit_memory::allocate(2 * granule);
	require(rest == reinterpret_cast<void *>(c + granule),
		"the rest of the region was not served");
	require(jit_memory::allocate(granule) == nullptr,
		"a full region served more");
	require(jit_memory::allocate(0) == nullptr, "an empty allocation");

	jit_memory::release(second, 2 * granule + 1);
	require(jit_memory::allocate(4 * granule) == nullptr,
		"a hole too small was served");
	void *again = jit_memory::allocate(2 * granule);
	require(again == second, "the lowest hole was not served first");
	jit_memory::release(again, 2 * granule);
	jit_memory::release(rest, 2 * granule);
	jit_memory::release(first, granule);
	jit_memory::release(third, granule);
	require(jit_memory::allocate(7 * granule) == first,
		"released granules were not served again as one run");
	jit_memory::release(first, 7 * granule);
}

void code_written_through_the_alias_runs_from_the_executable_mapping()
{
	void *code = jit_memory::allocate(granule);
	require(code != nullptr, "allocation failed");
	auto *alias = static_cast<std::byte *>(jit_memory::writable_alias(code));
	require(alias != code, "the alias is the executable mapping");
	/* One delta for the whole region. */
	void *other = jit_memory::allocate(granule);
	require(static_cast<std::byte *>(jit_memory::writable_alias(other)) -
				static_cast<std::byte *>(other) ==
			alias - static_cast<std::byte *>(code),
		"the alias is not one offset from the region");
	for (std::uint16_t value : {7, 8, 9}) {
		const auto words = constant(value);
		std::memcpy(alias, words.data(), sizeof words);
		jit_memory::set_writable(false);
		jit_memory::invalidate_instruction_cache(code, sizeof words);
		require(std::memcmp(code, words.data(), sizeof words) == 0,
			"the executable mapping does not show the written words");
		require(call(code) == value, "written code did not run");
	}
	jit_memory::release(other, granule);
	jit_memory::release(code, granule);
}

void the_arena_writes_through_the_alias()
{
	jit::ExecutableArena arena{3 * granule + 100};
	require(arena.valid() && jit::ExecutableArena::available(),
		"the arena has no memory");
	const auto first = constant(21);
	const auto second = constant(22);
	void *one = arena.store(first);
	void *two = arena.store(second);
	require(one != nullptr && two != nullptr && one != two,
		"the arena stored nothing");
	require(call(one) == 21 && call(two) == 22,
		"arena code did not run");

	/* A block with a literal patched into its words, through the link table. */
	::uml::cpu::arm64::JitBlockLinkTable links{16};
	::uml::cpu::arm64::JitBlockLink empty{.guest_pc = 1};
	const std::array<std::uint32_t, 4> words{
		0x52800000U | (30U << 5U), 0xd65f03c0U, 0, 0};
	const std::array<::uml::cpu::arm64::JitRelocation, 1> relocations{
		::uml::cpu::arm64::JitRelocation{
			.word = 2,
			.kind = ::uml::cpu::arm64::JitRelocationKind::indirect_link}};
	const std::array<std::uintptr_t, 1> values{0x1122334455667788ULL};
	const auto stored = arena.store_block(
		0x4000, words, relocations, values, links, empty);
	require(static_cast<bool>(stored), "the arena stored no block");
	require(call(stored.code) == 30, "block code did not run");
	std::uint64_t literal = 0;
	std::memcpy(&literal, static_cast<const std::byte *>(stored.code) + 8,
		sizeof literal);
	require(literal == 0x1122334455667788ULL,
		"the literal was not patched into the executable mapping");
	arena.reset();
	require(call(arena.store(constant(23))) == 23,
		"a reset arena did not run new code");
}

} // namespace

int main()
{
	try {
		nothing_is_available_before_a_region_is_prepared();
		a_region_is_prepared_once_and_only_grows_by_a_new_process();
		allocation_serves_whole_granules_lowest_first();
		code_written_through_the_alias_runs_from_the_executable_mapping();
		the_arena_writes_through_the_alias();
		std::puts("JIT debugger region tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "JIT debugger region test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
