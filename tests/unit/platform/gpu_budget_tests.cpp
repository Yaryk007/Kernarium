// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_budget.hpp"
#include "named_tests.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace {

using uml::testing::require;
namespace platform = uml::platform;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;

/* The floor, 352 MiB: the 384 MiB of the launch set less the 32 MiB the gadget
 * cache's cap gained. */
constexpr std::uint64_t floor_mib = 352;
static_assert(platform::gpu_budget_floor_bytes == floor_mib * mib);

platform::GpuBudget budgetOf(std::uint64_t limit, std::uint64_t guest,
	std::uint64_t application = 0, std::uint64_t engine = 0,
	std::uint64_t headroom = 0)
{
	return platform::compute_gpu_budget({
		.process_limit = limit * mib,
		.guest_memory = guest * mib,
		.application = application * mib,
		.engine = engine * mib,
		.headroom = headroom * mib,
	});
}

/* L - A - G - E - S at or above the floor is the budget itself. */
void theRemainderIsTheBudgetAboveTheFloor()
{
	/* iPad mini 6: 2867 MiB, a 1536 MiB guest, a 358 MiB gadget cache. */
	const auto ipad = budgetOf(2867, 1536, 0, 358, 0);
	require(ipad.available == 973 * mib);
	require(ipad.bytes == 973 * mib, "the guest's size does not bound it");
	require(!ipad.short_of_floor);

	const auto tight = budgetOf(2867, 1536, 300, 358, 400);
	require(tight.available == 273 * mib);
	require(tight.bytes == floor_mib * mib, "the floor is the least");
	require(tight.short_of_floor);

	const auto middle = budgetOf(2867, 1536, 300, 358, 100);
	require(middle.available == 573 * mib);
	require(middle.bytes == 573 * mib, "the remainder");
	require(!middle.short_of_floor);

	require(budgetOf(2867, 1536, 0, 358, 573).bytes == 400 * mib);
	require(budgetOf(2867, 1536, 0, 358, 621).bytes == floor_mib * mib);
	require(!budgetOf(2867, 1536, 0, 358, 621).short_of_floor,
		"exactly the floor is not short of it");
	require(budgetOf(2867, 1536, 0, 358, 622).short_of_floor);
}

/* A small guest on a large limit gets all that the limit leaves, and a tiny
 * one at least the floor: the guest's size is no bound. */
void aSmallGuestGetsWhatTheLimitLeavesAndTheFloorAtLeast()
{
	const auto small = budgetOf(2867, 512);
	require(small.bytes == 2355 * mib);
	require(small.available == 2355 * mib);
	require(!small.short_of_floor);
	require(budgetOf(2867, 704).bytes == 2163 * mib);
	require(budgetOf(2867, 768).bytes == 2099 * mib);
	require(budgetOf(6144, 64).bytes == 6080 * mib, "a tiny guest");
	require(budgetOf(6144, 512, 64, 320 + 128 + 4, 149).bytes == 4967 * mib);
	require(budgetOf(2867, 256).bytes == 2611 * mib,
		"a guest under the floor is no bound either");
	const auto tight = budgetOf(1024, 256, 0, 700, 0);
	require(tight.available == 68 * mib);
	require(tight.bytes == floor_mib * mib, "the floor, however small the guest");
	require(tight.short_of_floor);
	/* The warning follows the sum alone, and so does the budget above the
	 * floor. */
	const auto starved = budgetOf(1024, 512, 0, 400, 0);
	require(starved.available == 112 * mib);
	require(starved.short_of_floor);
	require(starved.bytes == floor_mib * mib);
	require(budgetOf(1024, 64, 0, 400, 0).bytes == 560 * mib);
}

/* Under a limit the budget is the larger of the floor and the sum's remainder
 * whatever the guest's size, checked here against the arithmetic alone. */
void theGuestsSizeNeverBoundsTheBudgetUnderALimit()
{
	for (std::uint64_t limit : {1024U, 2867U, 4096U, 6144U, 16384U})
		for (std::uint64_t guest = 64; guest <= 16384; guest += 64)
			for (std::uint64_t terms : {0U, 149U, 700U}) {
				const auto budget = budgetOf(limit, guest, terms, 300, 50);
				const std::uint64_t committed = guest + terms + 300 + 50;
				const std::uint64_t left =
					limit > committed ? limit - committed : 0;
				require(budget.available == left * mib);
				require(budget.bytes == std::max<std::uint64_t>(left, floor_mib) * mib);
				require(budget.short_of_floor == (left < floor_mib));
			}
}

/* Without a limit there is nothing to derive a budget from: the budget is the
 * guest's RAM and nothing is short, whatever A, E and S state. */
void noLimitIsTheGuest()
{
	const auto none = budgetOf(0, 2048, 100, 500, 200);
	require(none.bytes == 2048 * mib);
	require(none.available == 2048 * mib);
	require(!none.short_of_floor);
	require(budgetOf(0, 128).bytes == 128 * mib, "under the floor too");
	require(budgetOf(0, 9152).bytes == 9152 * mib);
	require(budgetOf(0, 0).bytes == 0);
}

/* A, E and S at zero leave the limit, the guest and the cache in the sum. */
void zeroParametersLeaveOnlyTheGuestAndTheCache()
{
	require(budgetOf(4096, 2048).bytes == 2048 * mib);
	require(budgetOf(2867, 2048).bytes == 819 * mib);
	require(budgetOf(2867, 2048).available == 819 * mib);
	require(budgetOf(2867, 4096).bytes == floor_mib * mib,
		"the guest alone passes the limit");
	require(budgetOf(2867, 4096).short_of_floor);
	require(budgetOf(2867, 4096).available == 0);
}

/* The conservative launch set (runtime/guest_memory_budget.hpp) on an iPad
 * mini 6: 64 MiB for the application, 320 for the rest of the engine and 149
 * of headroom, with the default guest of 1536 MiB and six vCPUs' cache (272
 * MiB) and slot tags (24 MiB). The GPU gets what is left, 502 MiB, where the
 * sum without the terms would leave it 1035 MiB. */
void theConservativeSumLeavesTheGPUWhatIsLeft()
{
	const auto conservative = budgetOf(2867, 1536, 64, 272 + 24 + 320, 149);
	require(conservative.available == 502 * mib);
	require(conservative.bytes == 502 * mib);
	require(!conservative.short_of_floor);
	require(budgetOf(2867, 1536, 0, 272 + 24, 0).bytes == 1035 * mib);
	/* A guest of 2048 MiB with the same terms is short of the floor. */
	const auto above = budgetOf(2867, 2048, 64, 272 + 24 + 320, 149);
	require(above.available == 0);
	require(above.bytes == floor_mib * mib);
	require(above.short_of_floor);
}

/* A live camera session holds host memory outside the GPU (the camera
 * device's reserve, 197 MiB at the tier every host is offered, for the use
 * with the most in it: camera_tiers.hpp), and the GPU's pool is B less it, taken after B is clamped
 * to the floor. */
void aLiveCameraTakesItsReserveOutOfTheBudget()
{
	constexpr std::uint64_t reserve = 197 * mib;
	/* An iPad mini 6 with the default guest: B is 502 MiB. */
	const auto mini = budgetOf(2867, 1536, 64, 272 + 24 + 320, 149);
	require(mini.bytes == 502 * mib);
	require(platform::gpu_pool_bytes(mini, 0) == 502 * mib,
		"blocked cameras and none take nothing");
	require(platform::gpu_pool_bytes(mini, reserve) == 305 * mib);
	require(mini.bytes == 502 * mib, "B itself is unchanged");
	/* A budget at the floor gives the pool less than the floor: the reserve is
	 * taken after the clamp. */
	const auto floor = budgetOf(2867, 2048, 64, 272 + 24 + 320, 149);
	require(floor.bytes == floor_mib * mib && floor.short_of_floor);
	require(platform::gpu_pool_bytes(floor, reserve) == 155 * mib);
	/* Without a limit the budget is the guest's RAM. */
	require(platform::gpu_pool_bytes(budgetOf(0, 2048), reserve) == (2048 - 197) * mib);
	/* The pool is never negative. */
	require(platform::gpu_pool_bytes(floor, floor.bytes) == 0);
	require(platform::gpu_pool_bytes(floor, floor.bytes + 1) == 0);
	require(platform::gpu_pool_bytes(budgetOf(0, 0), reserve) == 0);
	/* A pool of zero is no limit to the renderer, so a reserve that takes the
	 * whole budget is told apart from one that leaves a pool: the launch is
	 * refused. */
	require(!platform::gpu_pool_exhausted(mini, 0), "no camera holds nothing");
	require(!platform::gpu_pool_exhausted(mini, reserve));
	require(!platform::gpu_pool_exhausted(mini, mini.bytes - 1));
	require(platform::gpu_pool_exhausted(mini, mini.bytes) &&
		platform::gpu_pool_exhausted(mini, mini.bytes + 1));
	require(platform::gpu_pool_exhausted(floor, 598 * mib),
		"the large tier's most on a budget at the floor");
}

void theSumSaturates()
{
	constexpr auto top = std::numeric_limits<std::uint64_t>::max();
	const auto huge = platform::compute_gpu_budget({
		.process_limit = top,
		.guest_memory = 2048 * mib,
		.application = top,
		.engine = top,
		.headroom = top,
	});
	require(huge.available == 0);
	require(huge.bytes == floor_mib * mib);
	require(huge.short_of_floor);
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-remainder-is-the-budget-above-the-floor",
			theRemainderIsTheBudgetAboveTheFloor},
		{"a-small-guest-gets-what-the-limit-leaves-and-the-floor-at-least",
			aSmallGuestGetsWhatTheLimitLeavesAndTheFloorAtLeast},
		{"the-guests-size-never-bounds-the-budget-under-a-limit",
			theGuestsSizeNeverBoundsTheBudgetUnderALimit},
		{"no-limit-is-the-guest", noLimitIsTheGuest},
		{"zero-parameters-leave-only-the-guest-and-the-cache",
			zeroParametersLeaveOnlyTheGuestAndTheCache},
		{"the-conservative-sum-leaves-the-gpu-what-is-left",
			theConservativeSumLeavesTheGPUWhatIsLeft},
		{"a-live-camera-takes-its-reserve-out-of-the-budget",
			aLiveCameraTakesItsReserveOutOfTheBudget},
		{"the-sum-saturates", theSumSaturates},
	};
	return uml::testing::runNamedTests(tests);
}
