// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_memory_preparation.hpp"
#include "runtime/guest_memory_budget.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

/* The executable memory an iOS device's debugger prepares is sized from the
 * JIT's cache geometry, inside the budget the gadget cache would have had, so
 * the memory a guest side holds does not change with the engine. This host
 * has no debugger route: the preparation says so and publishes nothing. */
namespace {

namespace arm64 = ::uml::cpu::arm64;
namespace guest_memory = ::uml::runtime::guest_memory;
using uml::testing::require;

constexpr std::size_t kib = 1024U;
constexpr std::size_t mib = kib * kib;
constexpr std::size_t granule = 16U * kib;

[[nodiscard]] constexpr std::size_t whole_granules(std::size_t bytes) noexcept
{
	return (bytes + granule - 1U) / granule * granule;
}

void an_executors_arenas_are_whole_granules()
{
	for (const auto budget : {96U * mib, 128U * mib, 160U * mib, 272U * mib}) {
		const auto geometry = arm64::derive_jit_cache_geometry(budget, 1);
		require(geometry.valid(), "a budget of the gadget cache had no geometry");
		const auto bytes = geometry.executable_bytes();
		require(bytes % granule == 0, "arenas are not whole granules");
		require(bytes ==
				whole_granules(geometry.arena_bytes) +
					whole_granules(geometry.tracked_store_arena_bytes) +
					whole_granules(16U * kib),
			"the executable bytes are not the three arenas");
		require(bytes >= geometry.arena_bytes +
				geometry.tracked_store_arena_bytes,
			"the arenas do not fit their bytes");
	}
	require(!arm64::derive_jit_cache_geometry(0, 1).valid() &&
			arm64::jit_memory_region_bytes(0, 1) == 0 &&
			arm64::jit_memory_region_bytes(mib, 0) == 0,
		"an invalid pair has a region size");
}

void the_region_fits_the_gadget_budget_of_every_device_setting()
{
	/* An iPad mini 6 (2867 MiB), an iPhone with 6 GiB and a device with no
	 * limit measured: every vCPU count and every guest size the settings
	 * offer. */
	for (const std::size_t limit : {std::size_t{2867} * mib,
		     std::size_t{4096} * mib, std::size_t{6144} * mib}) {
		for (unsigned vcpus = 1; vcpus <= guest_memory::measured_vcpu_maximum;
		     ++vcpus) {
			for (std::size_t guest = 1536; guest <= 4096; guest += 512) {
				const auto budget = guest_memory::gadget_cache_budget(
					guest * mib, limit, vcpus);
				const auto region =
					arm64::jit_memory_region_bytes(budget, vcpus);
				require(region != 0 && region % granule == 0,
					"a setting has no region");
				require(region < budget,
					"the region exceeds the gadget cache's budget");
				const auto geometry =
					arm64::derive_jit_cache_geometry(budget, vcpus);
				require(region <= geometry.total_bytes() * vcpus,
					"the region exceeds what the geometry charges");
			}
		}
	}
}

void a_budget_is_publishable_until_another_is_published()
{
	require(arm64::jit_cache_budget_publishable(160U * mib, 2),
		"nothing was published, yet a valid pair was refused");
	require(!arm64::jit_cache_budget_publishable(0, 2) &&
			!arm64::jit_cache_budget_publishable(160U * mib, 0),
		"an invalid pair was publishable");
	require(arm64::prepare_jit_memory(160U * mib, 2) ==
			arm64::JitMemoryStatus::unsupported,
		"a host without the route prepared JIT memory");
	require(!arm64::jit_debugger_attached(),
		"a host without the route reports a debugger");
	arm64::release_jit_debugger();
	require(arm64::jit_cache_budget_publishable(256U * mib, 4),
		"the unsupported preparation published a budget");

	require(arm64::set_jit_cache_budget(160U * mib, 2),
		"a valid pair was not published");
	require(arm64::jit_cache_budget_publishable(160U * mib, 2),
		"the published pair is not publishable again");
	require(!arm64::jit_cache_budget_publishable(160U * mib, 3) &&
			!arm64::jit_cache_budget_publishable(256U * mib, 2),
		"another pair was publishable after one was published");
	require(!arm64::jit_cache_budget_publishable(
			160U * mib, 2, 32U * mib),
		"another private cap was publishable");
}

} // namespace

int main()
{
	try {
		an_executors_arenas_are_whole_granules();
		the_region_fits_the_gadget_budget_of_every_device_setting();
		a_budget_is_publishable_until_another_is_published();
		std::puts("JIT memory preparation tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "JIT memory preparation test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
