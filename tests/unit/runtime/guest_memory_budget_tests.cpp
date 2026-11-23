// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What the guest side of the process holds beside the guest's RAM as the vCPU
 * count grows, the most RAM a process limit leaves a guest and the RAM a new
 * guest gets: the cache cap and burst per vCPU count, the slot tags, E(v, G),
 * G_max(v), the conservative line and the default at the iPad mini 6's 2,867
 * MiB and other limits, under the launch set and the maximum set, and the host
 * GPU's budget B the default keeps where a guest may have 4 GiB. The expected
 * rows were derived on their own, in integer bytes, from the measured ceilings
 * (see the table comment) and by the searches in
 * theConservativeDefaultIsTheLargestStepThatLeavesTheFloor and
 * onADeviceThatOffers4GiBTheDefaultKeepsAGibibyteForTheGpu. The range has no
 * top of 4 GiB, and B has no cap of half the guest's RAM:
 * aLargerCeilingAndNoHalfCapChangeNoDefaultAndNoSizeUnder4GiB holds every
 * default, and every size below 4 GiB, to the rules of the range that ended
 * there and of the cap, copied below.
 */
#include "named_tests.hpp"
#include "require.hpp"
#include "runtime/guest_memory_budget.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

using uml::testing::require;
namespace budget = uml::runtime::guest_memory;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t ipad_limit = 2867 * mib;

/* The set that counts none of A, E_rest and S, and the conservative launch
 * set, as (A, E_rest, S) in MiB: S is a transient of 45, 40 past the GPU's
 * pool and 64 of growth. */
constexpr auto no_terms = budget::launch_memory_mib(0, 0, 0);
constexpr auto conservative = budget::launch_memory_mib(64, 320, 45 + 40 + 64);
constexpr auto conservative_application = conservative.application;
constexpr auto conservative_engine_rest = conservative.engine_rest;
constexpr auto conservative_headroom = conservative.headroom;
/* Three other sets, to hold the formula to its shape for any terms. */
constexpr auto light_set = budget::launch_memory_mib(56, 563, 64);
constexpr auto middle_set = budget::launch_memory_mib(64, 604, 128);
constexpr auto heavy_set = budget::launch_memory_mib(80, 644, 192);
constexpr std::array all_sets{no_terms, conservative, light_set, middle_set,
	heavy_set};

/* The worst gadget-cache ceiling of any measured session at 1..8 vCPUs, MiB. */
constexpr std::array<double, 8> measured_ceiling_mib{
	135.3, 173.6, 208.0, 244.9, 262.9, 267.5, 299.3, 298.3};

/* Seven eighths of the ceiling rounded up to 16 MiB, and 32 MiB more. */
constexpr std::array<std::uint32_t, 8> expected_cap_mib{
	160, 192, 224, 256, 272, 272, 304, 304};

/* The most the cache's charge exceeded its budget in an Android guest's boot,
 * first launches and a simultaneous start of several applications at 1..8
 * vCPUs (charge-time high-water marks), rounded up to 1 MiB. */
constexpr std::array<std::uint32_t, 8> expected_burst_mib{
	1, 1, 1, 1, 1, 1, 1, 2};

using Row = std::array<std::uint32_t, 8>;

Row maximumRow(std::uint64_t limit, const budget::LaunchMemorySet &set)
{
	Row row{};
	for (unsigned executors = 1; executors <= row.size(); ++executors)
		row[executors - 1] =
			budget::maximum_guest_memory_mib(limit, executors, set);
	return row;
}

void theCapIsSevenEighthsOfTheMeasuredCeilingRoundedUpAndThirtyTwoMore()
{
	for (unsigned executors = 1; executors <= 8; ++executors) {
		const double ceiling = measured_ceiling_mib[executors - 1];
		const double seven_eighths = ceiling * 7.0 / 8.0;
		auto rounded = static_cast<std::uint32_t>(seven_eighths);
		if (static_cast<double>(rounded) < seven_eighths)
			++rounded;
		const std::uint32_t sixteens = (rounded + 15U) / 16U * 16U;
		require(sixteens + 32U == expected_cap_mib[executors - 1],
			"cap = 7/8 of the ceiling up to 16 MiB, and 32 MiB more");
		require(budget::gadget_cache_cap(executors) ==
			expected_cap_mib[executors - 1] * mib);
		const auto at_ceiling =
			static_cast<std::size_t>(ceiling * static_cast<double>(mib));
		require(budget::gadget_cache_cap(executors) >= at_ceiling,
			"at or above the measured ceiling, so no measured load reaches it");
	}
}

void aCountOfZeroIsOneAndFurtherVCPUsAddSixteenEach()
{
	require(budget::gadget_cache_cap(0) == budget::gadget_cache_cap(1));
	require(budget::gadget_cache_cap(9) == 320 * mib);
	require(budget::gadget_cache_cap(10) == 336 * mib);
	require(budget::gadget_cache_cap(64) == (304 + 16 * 56) * mib);
	for (unsigned executors = 1; executors < 64; ++executors)
		require(budget::gadget_cache_cap(executors) <=
			budget::gadget_cache_cap(executors + 1), "never falls");
}

/* x(v) is small against the cap it exceeds, a count of zero is one and a count
 * above the last measured one has the last one's. */
void theBurstIsTheMeasuredExcessOfTheChargeOverTheBudget()
{
	for (unsigned executors = 1; executors <= 8; ++executors) {
		require(budget::gadget_cache_burst(executors) ==
			expected_burst_mib[executors - 1] * mib);
		require(budget::gadget_cache_burst(executors) * 16 <
			budget::gadget_cache_cap(executors),
			"a burst is a few MiB, far under the cap");
	}
	require(budget::gadget_cache_burst(0) == budget::gadget_cache_burst(1));
	require(budget::gadget_cache_burst(9) == budget::gadget_cache_burst(8));
	require(budget::gadget_cache_burst(64) == budget::gadget_cache_burst(8));
	/* It is no part of the budget the cache sheds to. */
	require(budget::gadget_cache_budget(4096 * mib, std::size_t{64} << 30, 8) ==
		304 * mib);
}

void theSlotTagsAreFourMebibytesEach()
{
	require(budget::slot_tag_bytes_per_executor == 4 * mib);
	require(budget::slot_tag_bytes(1) == 4 * mib);
	require(budget::slot_tag_bytes(6) == 24 * mib);
	require(budget::slot_tag_bytes(0) == 4 * mib, "zero counts as one");
}

/* The cap bounds the cache only where the host sets a limit. */
void theCacheBudgetIsCappedOnlyUnderALimit()
{
	constexpr std::size_t gib = std::size_t{1} << 30;
	/* No limit: a quarter of the guest, uncapped, as before the cap. */
	require(budget::gadget_cache_budget(8 * gib, 0, 1) == 2 * gib);
	require(budget::gadget_cache_budget(512 * mib, 0, 8) == 128 * mib);
	/* A roomy limit leaves the cap as the bound: 1024 MiB at 4096 MiB. */
	require(budget::gadget_cache_budget(4096 * mib, 64 * gib, 1) == 160 * mib);
	require(budget::gadget_cache_budget(4096 * mib, 64 * gib, 8) == 304 * mib);
	/* An eighth of 2867 MiB is 358.375, above the cap up to 8 vCPUs. */
	for (unsigned executors = 1; executors <= 8; ++executors)
		require(budget::gadget_cache_budget(2240 * mib, ipad_limit,
				executors) == expected_cap_mib[executors - 1] * mib);
	/* The cap is no floor: a small guest keeps its 128 MiB at any count, a
	 * tight limit its eighth. */
	require(budget::gadget_cache_budget(512 * mib, ipad_limit, 8) == 128 * mib);
	require(budget::gadget_cache_budget(512 * mib, ipad_limit, 1) == 128 * mib);
	require(budget::gadget_cache_budget(2048 * mib, 800 * mib, 8) == 100 * mib);
	/* More vCPUs than the table measured raise the cap, then the eighth of
	 * the limit bounds the cache. */
	require(budget::gadget_cache_budget(4096 * mib, 64 * gib, 12) ==
		(304 + 16 * 4) * mib);
}

void theEngineIsTheCacheItsBurstTheSlotTagsAndTheRest()
{
	const auto burst = [](unsigned executors) {
		return std::uint64_t{expected_burst_mib[executors - 1]} * mib;
	};
	require(budget::engine_memory(300 * mib, 4, 0) ==
		316 * mib + burst(4));
	require(budget::engine_memory(300 * mib, 4, 604 * mib) ==
		920 * mib + burst(4));
	require(budget::engine_memory(0, 8, 0) == 32 * mib + burst(8));
	require(budget::engine_memory(0, 0, 0) == 4 * mib + burst(1),
		"a count of zero is one");
	constexpr auto unlimited = std::numeric_limits<std::uint64_t>::max();
	require(budget::engine_memory(unlimited, 2, 1) == unlimited);
	require(budget::engine_memory(1, 2, unlimited) == unlimited);
	require(budget::engine_memory(unlimited - 4 * mib, 1, 0) == unlimited);
	require(budget::engine_memory(unlimited - 4 * mib - burst(1) + 1, 1, 0) ==
		unlimited, "the burst saturates with the rest");
}

/* The launch set is the conservative one the default and the GPU's budget are
 * measured with; the maximum set counts none of A, E_rest and S. */
void theLaunchSetIsConservativeAndTheMaximumSetCountsNone()
{
	require(budget::launch_memory_set.application == 64 * mib);
	require(budget::launch_memory_set.engine_rest == 320 * mib);
	require(budget::launch_memory_set.headroom == 149 * mib);
	require(budget::maximum_memory_set.application == 0);
	require(budget::maximum_memory_set.engine_rest == 0);
	require(budget::maximum_memory_set.headroom == 0);
}

void theMaximumOnAnIPadFollowsTheVCPUCountForEverySet()
{
	require(maximumRow(ipad_limit, no_terms) ==
		Row{2304, 2304, 2240, 2240, 2176, 2176, 2176, 2176}, "zero set");
	require(maximumRow(ipad_limit, conservative) ==
		Row{1792, 1728, 1728, 1664, 1664, 1664, 1600, 1600},
		"launch set");
	require(maximumRow(ipad_limit, light_set) ==
		Row{1664, 1600, 1536, 1536, 1536, 1472, 1472, 1472}, "light");
	require(maximumRow(ipad_limit, middle_set) ==
		Row{1536, 1472, 1472, 1408, 1408, 1408, 1344, 1344}, "middle");
	require(maximumRow(ipad_limit, heavy_set) ==
		Row{1408, 1344, 1344, 1280, 1280, 1280, 1216, 1216}, "heavy");
	/* The maximum a person may choose counts none of A, E_rest and S, so
	 * it is no lower than the launch set's and the default argument. */
	require(budget::maximum_guest_memory_mib(ipad_limit, 6) ==
		budget::maximum_guest_memory_mib(ipad_limit, 6, no_terms));
	require(budget::maximum_guest_memory_mib(ipad_limit, 6) == 2176,
		"2048 MiB stays selectable on an iPad mini 6");
	require(budget::maximum_guest_memory_mib(ipad_limit, 6) >
		budget::maximum_guest_memory_mib(ipad_limit, 6,
			budget::launch_memory_set));
	/* The same evaluation as a constant expression. */
	static_assert(budget::maximum_guest_memory_mib(ipad_limit, 2,
			middle_set) == 1472);
	static_assert(budget::maximum_guest_memory_mib(ipad_limit, 6) == 2176);
}

/* Every maximum leaves the host GPU its floor at its own size and not at the
 * next one up, the invariant G_max is defined by. */
void theMaximumIsTheLargestSizeThatLeavesTheFloor()
{
	for (const auto &set : all_sets)
		for (unsigned executors = 1; executors <= 8; ++executors) {
			const auto fits = [&](std::uint32_t guest_mib) {
				const std::uint64_t guest = guest_mib * mib;
				return !uml::platform::compute_gpu_budget({
					.process_limit = ipad_limit,
					.guest_memory = guest,
					.application = set.application,
					.engine = budget::engine_memory(
						budget::gadget_cache_budget(guest,
							ipad_limit, executors),
						executors, set.engine_rest),
					.headroom = set.headroom,
				}).short_of_floor;
			};
			const auto maximum = budget::maximum_guest_memory_mib(
				ipad_limit, executors, set);
			require(maximum % 64U == 0);
			require(fits(maximum));
			if (maximum < budget::device_window_guest_memory_maximum_mib)
				require(!fits(maximum + 64U),
					"the next size up is short of the floor");
		}
}

void aMoreVCPUsNeverRaiseTheMaximum()
{
	for (const auto &set : all_sets)
		for (std::uint64_t limit = 1024; limit <= 6144; limit += 256)
			for (unsigned executors = 1; executors < 16; ++executors)
				require(budget::maximum_guest_memory_mib(
						limit * mib, executors + 1, set) <=
					budget::maximum_guest_memory_mib(
						limit * mib, executors, set));
}

/* Without a limit nothing is short, so the offer is the host's ceiling: the
 * iOS device's window unless the caller states another. */
void noLimitOffersTheWholeRange()
{
	for (const auto &set : all_sets)
		for (unsigned executors : {0U, 1U, 4U, 8U, 64U}) {
			require(budget::maximum_guest_memory_mib(0, executors,
					set) == 9152);
			for (const std::uint32_t ceiling : {512U, 4096U, 65536U})
				require(budget::maximum_guest_memory_mib(0,
						executors, set, ceiling) == ceiling);
		}
}

/* A limit that leaves even the smallest guest less than the floor still
 * offers the smallest; the launch warns, as it does today. */
void aTightLimitOffersTheSmallestGuest()
{
	require(budget::maximum_guest_memory_mib(512 * mib, 4) == 512);
	require(budget::maximum_guest_memory_mib(1, 1) == 512);
	require(budget::maximum_guest_memory_mib(1024 * mib, 8,
			heavy_set) == 512);
	/* A roomier limit offers more, and steps down with the vCPU count. */
	require(budget::maximum_guest_memory_mib(4096 * mib, 1) == 3520);
	require(budget::maximum_guest_memory_mib(4096 * mib, 8) == 3392);
}

using DefaultRow = std::array<std::uint32_t, 8>;

DefaultRow defaultRow(std::uint64_t limit)
{
	DefaultRow row{};
	for (unsigned executors = 1; executors <= row.size(); ++executors)
		row[executors - 1] = budget::default_guest_memory_mib(limit, executors);
	return row;
}

DefaultRow conservativeRow(std::uint64_t limit)
{
	DefaultRow row{};
	for (unsigned executors = 1; executors <= row.size(); ++executors)
		row[executors - 1] =
			budget::conservative_guest_memory_mib(limit, executors);
	return row;
}

/* B in MiB of the guest the default gives, by vCPU count. */
DefaultRow budgetAtDefaultRow(std::uint64_t limit)
{
	DefaultRow row{};
	for (unsigned executors = 1; executors <= row.size(); ++executors)
		row[executors - 1] = static_cast<std::uint32_t>(
			budget::gpu_budget(limit,
				budget::default_guest_memory_mib(limit, executors),
				executors).bytes / mib);
	return row;
}

/* An iPad mini 6 starts a guest at 1.5 GiB at every vCPU count: the launch
 * set's G_max(v), 1792 down to 1600 MiB, rounds down to 1536 and the range's
 * bottom holds it there. The guest of 2048 MiB the maximum still offers
 * (2176 at six vCPUs) is above it. */
void theDefaultOnAnIPadMiniIsOneAndAHalfGiBAtEveryVCPUCount()
{
	require(defaultRow(ipad_limit) ==
		DefaultRow{1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536});
	for (unsigned executors = 1; executors <= 8; ++executors) {
		require(budget::default_guest_memory_mib(ipad_limit, executors) <
			2048, "2048 MiB is above the default");
		require(budget::maximum_guest_memory_mib(ipad_limit, executors) >=
			2048, "and still selectable");
		require(budget::default_guest_memory_mib(ipad_limit, executors,
				conservative) ==
			budget::default_guest_memory_mib(ipad_limit, executors),
			"the launch set is the default argument");
	}
	static_assert(budget::default_guest_memory_mib(ipad_limit, 6) == 1536);
}

/* Other limits, with the advertised limit as the only input: the launch set's
 * G_max(v) rounded down to 512 MiB, held within 1536 to 4096, and never above
 * the maximum the settings offer; where a guest may have 4096 MiB also the
 * most that keeps 992 MiB for the host GPU (5120 MiB is such a limit, and the
 * only one here that the second bound lowers). */
void theDefaultFollowsTheLimitInHalfGiBSteps()
{
	for (const auto &[limit_mib, expected] : {
		     std::pair{3584U, DefaultRow{2048, 2048, 2048, 2048, 2048,
				     2048, 2048, 2048}},
		     std::pair{4096U, DefaultRow{2560, 2560, 2560, 2560, 2560,
				     2560, 2560, 2560}},
		     std::pair{5120U, DefaultRow{3072, 3072, 3072, 3072, 3072,
				     3072, 3072, 3072}},
		     std::pair{6144U, DefaultRow{4096, 4096, 4096, 4096, 4096,
				     4096, 4096, 4096}},
		     std::pair{8192U, DefaultRow{4096, 4096, 4096, 4096, 4096,
				     4096, 4096, 4096}},
		     /* The range's bottom holds 1792..1600 down at 1536. */
		     std::pair{2304U, DefaultRow{1536, 1536, 1536, 1536, 1536,
				     1536, 1536, 1536}},
		     /* A limit that leaves less than the bottom of the range:
		      * the maximum the settings offer wins, on 64 MiB steps. */
		     std::pair{2048U, DefaultRow{1472, 1472, 1408, 1408, 1408,
				     1408, 1408, 1344}},
		     std::pair{1536U, DefaultRow{960, 960, 960, 960, 960, 960,
				     960, 896}},
	     })
		require(defaultRow(limit_mib * mib) == expected,
			std::to_string(limit_mib).c_str());
	/* No limit: the whole range, whatever the vCPU count. */
	for (unsigned executors : {0U, 1U, 4U, 8U, 64U})
		require(budget::default_guest_memory_mib(0, executors) == 4096);
}

/* The conservative line is the largest multiple of 512 MiB for which the
 * launch set leaves the host GPU its floor, searched here from the sums in
 * bytes rather than from G_max, then held to at least 1536 and under the
 * maximum, which is searched the same way with the set that counts none. Both
 * searches run to the ceiling: the iOS device's here, and 4096 MiB, the range
 * that ended there, where the line is also held to 4096. */
void theConservativeDefaultIsTheLargestStepThatLeavesTheFloor()
{
	const auto fits = [](std::uint64_t limit, unsigned executors,
				  std::uint32_t guest_mib,
				  const budget::LaunchMemorySet &set) {
		const std::uint64_t guest = guest_mib * mib;
		return !uml::platform::compute_gpu_budget({
			.process_limit = limit,
			.guest_memory = guest,
			.application = set.application,
			.engine = budget::engine_memory(
				budget::gadget_cache_budget(guest, limit, executors),
				executors, set.engine_rest),
			.headroom = set.headroom,
		}).short_of_floor;
	};
	for (const std::uint32_t ceiling : {4096U, 9152U})
		for (std::uint64_t limit_mib = 1024; limit_mib <= 16384;
		     limit_mib += 128)
			for (unsigned executors = 1; executors <= 8; ++executors) {
				const std::uint64_t limit = limit_mib * mib;
				std::uint32_t stepped = 512;
				for (std::uint32_t guest = 512; guest <= ceiling;
				     guest += 512)
					if (fits(limit, executors, guest, conservative))
						stepped = guest;
				std::uint32_t most = 512;
				for (std::uint32_t guest = 512; guest <= ceiling;
				     guest += 64)
					if (fits(limit, executors, guest, no_terms))
						most = guest;
				const auto expected = std::min(
					std::max(stepped, 1536U), most);
				const auto actual = budget::conservative_guest_memory_mib(
					limit, executors, budget::launch_memory_set, ceiling);
				require(actual == expected,
					std::to_string(limit_mib).c_str());
				require(actual <= budget::maximum_guest_memory_mib(
						limit, executors, budget::maximum_memory_set,
						ceiling));
				require(actual % 64U == 0, "on the settings' steps");
				if (actual < most)
					require(actual % 512U == 0,
						"whole half gibibytes wherever the maximum did not bind");
				if (stepped >= 1536)
					require(fits(limit, executors, actual, conservative),
						"the launch set leaves the GPU its floor at the line");
			}
}

/* More vCPUs hold more of the process, so the conservative line never rises
 * with them. */
void aMoreVCPUsNeverRaiseTheConservativeDefault()
{
	for (std::uint64_t limit = 1024; limit <= 8192; limit += 128)
		for (unsigned executors = 1; executors < 16; ++executors)
			require(budget::conservative_guest_memory_mib(limit * mib,
					executors + 1) <=
				budget::conservative_guest_memory_mib(limit * mib,
					executors));
}

/* Nor does the default, at any limit and for vCPU counts to 15: the second
 * bound applies at every count of a device that offers 4096 MiB at one vCPU,
 * and both bounds fall with the count, so the default does too even where the
 * maximum itself leaves 4096 (a limit of 4612 to 4783 MiB offers 4096 at one
 * vCPU only). */
void aMoreVCPUsNeverRaiseTheDefault()
{
	for (std::uint64_t limit = 1024; limit <= 12288; ++limit)
		for (unsigned executors = 1; executors < 16; ++executors)
			require(budget::default_guest_memory_mib(limit * mib,
					executors + 1) <=
				budget::default_guest_memory_mib(limit * mib, executors),
				std::to_string(limit).c_str());
}

/* On a device whose limit lets a guest have 4096 MiB the default keeps the
 * host GPU 1 GiB. The rows are for the iPad mini 6's limit and the limits
 * assumed for an iPhone 18 Pro Max, at 1 through 8 vCPUs. */
void theDefaultKeepsAGibibyteForTheGpuAtTheTopOfTheRange()
{
	/* No second bound below the top of the range: the iPad mini 6 and the
	 * limits whose maximum is under 4096. */
	for (const std::uint64_t limit_mib : {2867U, 3584U, 4096U}) {
		const auto limit = limit_mib * mib;
		require(defaultRow(limit) == conservativeRow(limit),
			std::to_string(limit_mib).c_str());
		for (unsigned executors = 1; executors <= 8; ++executors)
			require(budget::maximum_guest_memory_mib(limit, executors) <
				4096);
	}
	require(conservativeRow(2867 * mib) ==
		DefaultRow{1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536});
	require(conservativeRow(3584 * mib) ==
		DefaultRow{2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048});
	require(conservativeRow(4096 * mib) ==
		DefaultRow{2560, 2560, 2560, 2560, 2560, 2560, 2560, 2560});
	/* 5120 MiB offers 4096 at every count: 3584 MiB leaves the GPU 665 to
	 * 838 MiB, so the default is 3072 MiB and the GPU 1177 to 1350. */
	require(conservativeRow(5120 * mib) ==
		DefaultRow{3584, 3584, 3584, 3584, 3584, 3584, 3584, 3584});
	require(defaultRow(5120 * mib) ==
		DefaultRow{3072, 3072, 3072, 3072, 3072, 3072, 3072, 3072});
	require(budgetAtDefaultRow(5120 * mib) ==
		DefaultRow{1350, 1314, 1278, 1242, 1222, 1218, 1182, 1177});
	for (unsigned executors = 1; executors <= 8; ++executors)
		require(budget::gpu_budget(5120 * mib, 3584, executors).bytes <
			budget::default_gpu_budget_minimum_mib * mib,
			"the conservative line leaves the GPU less");
	/* 4700 MiB offers 4096 at one and two vCPUs and less after, and the
	 * second bound still holds at every count: 2560 MiB throughout, where
	 * the conservative line is 3584 at one and two and 3072 after. */
	require(conservativeRow(4700 * mib) ==
		DefaultRow{3584, 3584, 3072, 3072, 3072, 3072, 3072, 3072});
	require(defaultRow(4700 * mib) ==
		DefaultRow{2560, 2560, 2560, 2560, 2560, 2560, 2560, 2560});
	/* 6144 MiB leaves a 4096 MiB guest 1177 to 1350 MiB already. The
	 * default stops at 4096; the line the process is likely to be ended
	 * above does not. */
	require(defaultRow(6144 * mib) ==
		DefaultRow{4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096});
	require(conservativeRow(6144 * mib) ==
		DefaultRow{4608, 4608, 4608, 4608, 4608, 4608, 4608, 4608});
	require(budgetAtDefaultRow(6144 * mib) ==
		DefaultRow{1350, 1314, 1278, 1242, 1222, 1218, 1182, 1177});
	/* No limit: the whole range, as before, and B is the guest's RAM. */
	for (unsigned executors : {0U, 1U, 4U, 8U})
		require(budget::default_guest_memory_mib(0, executors) == 4096);
	require(budget::gpu_budget(0, 4096, 8).bytes == 4096 * mib);
	require(!budget::gpu_budget(0, 4096, 8).short_of_floor);
}

/* The bound is the search the second rule is held to: for every limit that
 * offers 4096 MiB at one vCPU, at every vCPU count the default is the lesser
 * of the conservative line and the largest multiple of 512 MiB that leaves B
 * at 992 MiB, searched here from compute_gpu_budget, and is never under 2048
 * MiB (such a limit leaves a guest of 2048 more than 992 MiB). With the
 * launch set's terms the closed form L - A - E_rest - cap(v) - x(v) - 4 MiB * v
 * - S - 992 stands for the search, since the cache is then at its cap. */
void onADeviceThatOffers4GiBTheDefaultKeepsAGibibyteForTheGpu()
{
	unsigned limited = 0;
	for (std::uint64_t limit_mib = 1024; limit_mib <= 12288; limit_mib += 16)
		for (unsigned executors = 1; executors <= 8; ++executors) {
			const std::uint64_t limit = limit_mib * mib;
			const auto actual =
				budget::default_guest_memory_mib(limit, executors);
			const auto conservative = budget::conservative_guest_memory_mib(
				limit, executors, budget::launch_memory_set, 4096);
			if (budget::maximum_guest_memory_mib(limit, 1,
					budget::maximum_memory_set, 4096) < 4096) {
				require(actual == conservative);
				continue;
			}
			std::uint32_t largest = 0;
			for (std::uint32_t guest = 512; guest <= 4096; guest += 512) {
				const auto plan = uml::platform::compute_gpu_budget({
					.process_limit = limit,
					.guest_memory = guest * mib,
					.application = conservative_application,
					.engine = budget::engine_memory(
						budget::gadget_cache_budget(
							guest * mib, limit, executors),
						executors, conservative_engine_rest),
					.headroom = conservative_headroom,
				});
				if (plan.bytes >= budget::default_gpu_budget_minimum_mib * mib)
					largest = guest;
			}
			require(largest >= 2048,
				"2048 MiB leaves the GPU 992 MiB wherever 4096 is offered at one vCPU");
			require(budget::graphics_guest_memory_mib(limit, executors) ==
				largest);
			require(actual == std::min(conservative, largest));
			require(actual >= 2048 && actual % 512U == 0);
			require(budget::gpu_budget(limit, actual, executors).bytes >=
				budget::default_gpu_budget_minimum_mib * mib,
				"B is 992 MiB or more at the default");
			if (actual < conservative)
				++limited;
			/* The cache is at its cap, so the closed form holds. */
			require(budget::gadget_cache_budget(2048 * mib, limit,
					executors) == budget::gadget_cache_cap(executors));
			const std::uint64_t spare = limit_mib - 64U - 320U -
				(budget::gadget_cache_cap(executors) +
				 budget::gadget_cache_burst(executors) +
				 budget::slot_tag_bytes(executors)) / mib -
				(45U + 40U + 64U) - 992U;
			require(largest == std::min<std::uint64_t>(
				spare / 512U * 512U, 4096U));
		}
	require(limited > 0, "some limits are lowered");
}

/* The applications warn about graphics memory above a default the GPU budget
 * rule lowered: default_guest_memory says so from the same choice that makes
 * the default, so no second definition of the rule exists. It holds where the
 * rule picked a size below the conservative one, and is false where the
 * default is the conservative size, the cap of 4096 MiB held it included. */
void theGpuBudgetRuleIsFlaggedOnlyWhereItLowersTheDefault()
{
	unsigned flagged = 0;
	unsigned unflagged = 0;
	for (std::uint64_t limit_mib = 0; limit_mib <= 20480; limit_mib += 16)
		for (unsigned executors = 1; executors <= 8; ++executors) {
			const std::uint64_t limit = limit_mib * mib;
			const auto choice = budget::default_guest_memory(limit, executors);
			require(choice.mib ==
				budget::default_guest_memory_mib(limit, executors));
			const auto conservative = budget::conservative_guest_memory_mib(
				limit, executors, budget::launch_memory_set, 4096);
			if (!choice.holds_gpu_budget) {
				require(choice.mib == conservative);
				++unflagged;
				continue;
			}
			++flagged;
			require(limit != 0 && choice.mib < conservative);
			require(choice.mib ==
				budget::graphics_guest_memory_mib(limit, executors));
			require(budget::gpu_budget(limit, choice.mib, executors).bytes >=
				budget::default_gpu_budget_minimum_mib * mib);
			require(budget::gpu_budget(limit, choice.mib + 512U, executors)
					.bytes < budget::default_gpu_budget_minimum_mib * mib,
				"B is under 992 MiB from the next step above the default");
		}
	require(flagged > 0 && unflagged > 0);
	/* The limits the applications know. Where the cap of 4096 MiB holds the
	 * default (6144 and above) or none lets 4096 be offered (4096 and
	 * below), nothing is said of B. */
	for (unsigned executors = 1; executors <= 8; ++executors) {
		for (const std::uint64_t limit_mib : {4700U, 5120U})
			require(budget::default_guest_memory(limit_mib * mib, executors)
					.holds_gpu_budget,
				"a limit that offers 4096 and a default under it");
		for (const std::uint64_t limit_mib :
			{0U, 2867U, 3584U, 4096U, 6144U, 8192U, 12288U})
			require(!budget::default_guest_memory(limit_mib * mib, executors)
					 .holds_gpu_budget);
	}
	static_assert(budget::default_guest_memory(5120 * mib, 6).mib == 3072);
	static_assert(budget::default_guest_memory(6144 * mib, 6).mib == 4096);
}

/* A set that leaves no guest in the default range 992 MiB for the GPU
 * cannot lower the default: the second bound is then left out and the default
 * is the conservative line. */
void aSetThatLeavesNoGibibyteLeavesTheConservativeDefault()
{
	constexpr auto crowded = budget::launch_memory_mib(4000, 0, 0);
	const auto limit = 6144 * mib;
	require(budget::maximum_guest_memory_mib(limit, 4,
			budget::maximum_memory_set, 4096) == 4096);
	require(budget::graphics_guest_memory_mib(limit, 4, crowded) == 0);
	require(budget::default_guest_memory_mib(limit, 4, crowded) ==
		budget::conservative_guest_memory_mib(limit, 4, crowded, 4096));
	require(budget::graphics_guest_memory_mib(512 * mib, 4) == 0);
}

/* B is the evaluation the run makes: the guest's RAM without a limit, the
 * larger of the floor and L - A - G - E - S otherwise, and the sums are those
 * of the run. */
void theBudgetIsTheEvaluationTheRunMakes()
{
	/* An iPad mini 6 at six vCPUs and the default 1536 MiB. */
	const auto plain = budget::gpu_budget(ipad_limit, 1536, 6);
	const std::uint64_t cache = budget::gadget_cache_budget(
		1536 * mib, ipad_limit, 6);
	const std::uint64_t engine = cache + 6 * 4 * mib +
		expected_burst_mib[5] * mib + 320 * mib;
	require(plain.available == ipad_limit - 64 * mib - 1536 * mib - engine -
		149 * mib);
	require(plain.bytes == 501 * mib);
	require(!plain.short_of_floor);
	/* The most the settings offer leaves the floor and the process short. */
	const auto top = budget::gpu_budget(ipad_limit, 2176, 6);
	require(top.bytes == 352 * mib && top.short_of_floor);
	/* A small guest is no bound: what the limit leaves is all of it. */
	const auto small = budget::gpu_budget(ipad_limit, 512, 1);
	require(small.available ==
		(2867 - 64 - 512 - (128 + 4 + 1 + 320) - 149) * mib);
	require(small.bytes == small.available);
	require(small.bytes == 1689 * mib && !small.short_of_floor);
	/* Without a limit it is the guest's RAM, whatever the terms. */
	require(budget::gpu_budget(0, 1536, 6).bytes == 1536 * mib);
	require(budget::gpu_budget(0, 1536, 6, heavy_set).bytes == 1536 * mib);
	require(budget::gpu_budget(0, 9152, 1).bytes == 9152 * mib);
	/* The set is an argument, the launch set the default. */
	require(budget::gpu_budget(ipad_limit, 1536, 6).bytes ==
		budget::gpu_budget(ipad_limit, 1536, 6, conservative).bytes);
	require(budget::gpu_budget(ipad_limit, 1536, 6, no_terms).bytes >
		plain.bytes);
}

/* The warning threshold the applications use is the default itself: a guest
 * above it is the one the process is likely to be ended with. On an iPad mini 6
 * that is every size from 1600 MiB up, 2048 MiB included, and none of the
 * sizes up to the default. */
void aGuestAboveTheDefaultIsWhatTheApplicationsWarnAbout()
{
	for (unsigned executors = 1; executors <= 8; ++executors) {
		const auto warn_from =
			budget::default_guest_memory_mib(ipad_limit, executors);
		const auto maximum =
			budget::maximum_guest_memory_mib(ipad_limit, executors);
		require(warn_from == 1536);
		require(maximum > warn_from, "there are sizes to warn about");
		require(2048 > warn_from && 2048 <= maximum);
	}
}

/* The rules of the range that ended at 4096 MiB, as they were before the
 * range lost its top, and of B as it was while half the guest's RAM bounded
 * it, copied so the sweep below holds the lifted rules to them rather than to
 * themselves. */
namespace before_the_range_lost_its_top {

constexpr std::uint32_t top = 4096;

/* B in bytes while half the guest's RAM bounded it: the remainder between the
 * floor and G / 2, and G / 2 without a limit. */
std::uint64_t budgetBytes(std::uint64_t limit, std::uint32_t guest_mib,
	unsigned executors, const budget::LaunchMemorySet &set)
{
	const std::uint64_t half = std::uint64_t{guest_mib} * mib / 2U;
	if (limit == 0)
		return half;
	const auto available =
		budget::gpu_budget(limit, guest_mib, executors, set).available;
	return std::min(std::max(available,
				 uml::platform::gpu_budget_floor_bytes), half);
}

std::uint32_t maximum(std::uint64_t limit, unsigned executors,
	const budget::LaunchMemorySet &set)
{
	if (limit == 0)
		return top;
	for (auto size = top; size > 512; size -= 64)
		if (!budget::gpu_budget(limit, size, executors, set).short_of_floor)
			return size;
	return 512;
}

std::uint32_t conservative(std::uint64_t limit, unsigned executors,
	const budget::LaunchMemorySet &set)
{
	const auto safe = maximum(limit, executors, set);
	const auto stepped = safe / 512U * 512U;
	const auto held = std::clamp(stepped, 1536U, top);
	return std::min(held, maximum(limit, executors, budget::maximum_memory_set));
}

std::uint32_t graphics(std::uint64_t limit, unsigned executors,
	const budget::LaunchMemorySet &set)
{
	for (auto size = top; size >= 1024U * 2U; size -= 512)
		if (budgetBytes(limit, size, executors, set) >=
			budget::default_gpu_budget_minimum_mib * mib)
			return size;
	return 0;
}

bool holdsGpuBudget(std::uint64_t limit, unsigned executors,
	const budget::LaunchMemorySet &set)
{
	if (limit == 0 ||
	    maximum(limit, 1, budget::maximum_memory_set) != top)
		return false;
	const auto graphic = graphics(limit, executors, set);
	return graphic != 0 && graphic < conservative(limit, executors, set);
}

std::uint32_t defaults(std::uint64_t limit, unsigned executors,
	const budget::LaunchMemorySet &set)
{
	const auto line = conservative(limit, executors, set);
	if (limit == 0 ||
	    maximum(limit, 1, budget::maximum_memory_set) != top)
		return line;
	const auto graphic = graphics(limit, executors, set);
	return graphic == 0 ? line : std::min(line, graphic);
}

} // namespace before_the_range_lost_its_top

/* The default does not change on any device, and neither does any size the
 * range offered: for every limit from none through 20 GiB, every vCPU count
 * and the sets, the default, whether the graphics rule set it, and the
 * 4096-capped maximum and line equal the old rules' for every ceiling, so a
 * larger ceiling only adds sizes above them. The graphics bound is the old one
 * wherever there was one; the one size the cap kept out of its range, 1536
 * MiB, may now qualify where nothing above it did, and no default uses it
 * there (a limit that offers 4096 MiB leaves 2048 its 992 MiB). */
void aLargerCeilingAndNoHalfCapChangeNoDefaultAndNoSizeUnder4GiB()
{
	namespace old = before_the_range_lost_its_top;
	constexpr std::array<std::uint32_t, 5> ceilings{
		4096, 4160, 9152, 65536, budget::guest_memory_range_maximum_mib};
	for (std::uint64_t limit_mib = 0; limit_mib <= 20480; ++limit_mib) {
		const std::uint64_t limit = limit_mib * mib;
		/* Every set at a coarse step, the two the product uses at every
		 * limit. */
		const bool every_set = limit_mib % 17 == 0;
		for (unsigned executors = 1; executors <= 8; ++executors) {
			for (std::size_t index = 0; index < all_sets.size(); ++index) {
				/* all_sets starts with none and the launch set. */
				if (!every_set && index > 1)
					continue;
				const auto &set = all_sets[index];
				require(budget::default_guest_memory_mib(limit, executors,
						set) == old::defaults(limit, executors, set),
					std::to_string(limit_mib).c_str());
				require(budget::default_guest_memory(limit, executors,
						set).holds_gpu_budget ==
					old::holdsGpuBudget(limit, executors, set),
					std::to_string(limit_mib).c_str());
				const auto graphic = budget::graphics_guest_memory_mib(
					limit, executors, set);
				const auto before = old::graphics(limit, executors, set);
				require(before != 0 ? graphic == before :
						graphic == 0 || graphic == 1536,
					std::to_string(limit_mib).c_str());
				for (const auto ceiling : ceilings) {
					const auto offered = budget::maximum_guest_memory_mib(
						limit, executors, set, ceiling);
					const auto line = budget::conservative_guest_memory_mib(
						limit, executors, set, ceiling);
					require(std::min(offered, 4096U) ==
							old::maximum(limit, executors, set),
						"the offer, below 4 GiB, is the old maximum");
					require(std::min(line, 4096U) ==
							old::conservative(limit, executors, set),
						"the line, below 4 GiB, is the old line");
					require(offered <= ceiling);
					require(line <= budget::maximum_guest_memory_mib(limit,
							executors, budget::maximum_memory_set,
							ceiling),
						"the line is within the most a person may choose");
					require(offered % 64U == 0 && offered >= 512U);
					require(budget::default_guest_memory_mib(limit,
							executors, set) <= std::max(offered, 1536U),
						"a new guest is one of the sizes on offer");
				}
			}
			const auto exact = budget::conservative_guest_memory_mib(limit,
				executors, budget::launch_memory_set, 4096);
			require(exact == old::conservative(limit, executors,
					budget::launch_memory_set));
			require(budget::maximum_guest_memory_mib(limit, executors,
					budget::maximum_memory_set, 4096) ==
				old::maximum(limit, executors, budget::maximum_memory_set));
		}
	}
}

/* Above 4096 the offer is what the budget gives, searched here from the sums,
 * and stops at the ceiling; no limit offers the ceiling and nothing to warn
 * about. A Mac with 64 GiB of RAM and 63 GiB of limit, an iPad Pro with a
 * 12 GiB limit and the iPad mini 6 are the cases. */
void aLimitOffersWhatTheBudgetGivesUpToTheCeiling()
{
	const auto fits = [](std::uint64_t limit, unsigned executors,
				  std::uint32_t guest_mib,
				  const budget::LaunchMemorySet &set) {
		return !budget::gpu_budget(limit, guest_mib, executors, set)
				.short_of_floor;
	};
	for (const std::uint32_t ceiling :
	     {9152U, 65536U, budget::guest_memory_range_maximum_mib})
		for (const std::uint64_t limit_mib :
		     {2867ULL, 6144ULL, 12288ULL, 16384ULL, 65536ULL - 1024ULL,
		      1ULL << 20}) {
			const std::uint64_t limit = limit_mib * mib;
			for (unsigned executors = 1; executors <= 8; ++executors) {
				std::uint32_t most = 512;
				const auto searched = std::min<std::uint64_t>(
					ceiling, limit_mib);
				for (std::uint64_t guest = 512; guest <= searched;
				     guest += 64)
					if (fits(limit, executors,
						    static_cast<std::uint32_t>(guest),
						    no_terms))
						most = static_cast<std::uint32_t>(guest);
				require(budget::maximum_guest_memory_mib(limit, executors,
						no_terms, ceiling) == most,
					std::to_string(limit_mib).c_str());
				if (most < ceiling)
					require(!fits(limit, executors, most + 64U, no_terms),
						"the next size up is short of the floor");
			}
		}
	/* The cases by name. */
	require(budget::maximum_guest_memory_mib(12288 * mib, 6) ==
		9152, "an iPad Pro's limit is more than a device window admits");
	require(budget::maximum_guest_memory_mib(12288 * mib, 6,
			budget::maximum_memory_set, 65536) < 12288 &&
		budget::maximum_guest_memory_mib(12288 * mib, 6,
			budget::maximum_memory_set, 65536) > 11264,
		"a Mac stating the same limit is held by the budget alone");
	require(budget::maximum_guest_memory_mib(63 * 1024 * mib, 8,
			budget::maximum_memory_set, 65536) < 63 * 1024);
	require(budget::maximum_guest_memory_mib(0, 8,
			budget::maximum_memory_set, 65536) == 65536);
	/* The line follows the ceiling too, and without a limit it is the
	 * whole offer. */
	require(budget::conservative_guest_memory_mib(0, 8, budget::launch_memory_set,
			9152) == 9152);
	require(budget::conservative_guest_memory_mib(12288 * mib, 6,
			budget::launch_memory_set, 65536) > 4096);
	require(budget::conservative_guest_memory_mib(12288 * mib, 6,
			budget::launch_memory_set, 65536) <
		budget::maximum_guest_memory_mib(12288 * mib, 6,
			budget::maximum_memory_set, 65536));
}

/* The ceiling is the window's bound and the host's RAM, whichever is less,
 * never under the range the default is held to, on the 64 MiB steps. */
void theCeilingIsTheWindowAndTheHostsRAMWhicheverIsLess()
{
	constexpr auto device = budget::device_window_guest_memory_maximum_mib;
	constexpr auto range = budget::guest_memory_range_maximum_mib;
	static_assert(device == 9152);
	static_assert(budget::guest_memory_ceiling_mib(device, 0) == device,
		"unknown RAM leaves the window's bound");
	static_assert(budget::guest_memory_ceiling_mib(device, 16384 * mib) ==
		device);
	static_assert(budget::guest_memory_ceiling_mib(device, 8192 * mib) ==
		8192);
	static_assert(budget::guest_memory_ceiling_mib(range, 64 * 1024 * mib) ==
		65536, "a Mac with 64 GiB");
	static_assert(budget::guest_memory_ceiling_mib(range, 0) == range);
	static_assert(budget::guest_memory_ceiling_mib(range, 1ULL << 50) == range,
		"a terabyte is the most any range holds");
	static_assert(budget::guest_memory_ceiling_mib(range, 3072 * mib) == 4096,
		"never under the range the default is held to");
	static_assert(budget::guest_memory_ceiling_mib(range, 0x7FFFFFFFFULL) ==
		budget::guest_memory_minimum_mib + (0x7FFFFFFFFULL / mib -
			budget::guest_memory_minimum_mib) / 64U * 64U,
		"onto the steps");
	for (std::uint64_t window : {4096ULL, device * 1ULL, range * 1ULL})
		for (std::uint64_t physical = 0; physical <= 300; ++physical) {
			const auto ceiling = budget::guest_memory_ceiling_mib(
				window, physical * 977 * mib);
			require(ceiling >= 4096 && ceiling <= range);
			require((ceiling - budget::guest_memory_minimum_mib) %
					budget::guest_memory_step_mib == 0);
			require(ceiling <= std::max<std::uint64_t>(window, 4096));
		}
}

/* The applications default to and offer every physical core, at most the
 * count the cache cap and G_max are measured for. */
void theApplicationLimitIsThePhysicalCoresAtMostTheMeasuredCount()
{
	static_assert(budget::measured_vcpu_maximum == 8);
	static_assert(budget::measured_vcpu_maximum ==
		budget::gadget_cache_cap_mib.size());
	/* An iPad mini 6, an eight-core host, the ten-core devices and a Mac
	 * with twenty. */
	require(budget::application_vcpu_limit(6) == 6);
	require(budget::application_vcpu_limit(8) == 8);
	require(budget::application_vcpu_limit(10) == 8);
	require(budget::application_vcpu_limit(20) == 8);
	require(budget::application_vcpu_limit(1) == 1);
	require(budget::application_vcpu_limit(0) == 1, "zero counts as one");
	require(budget::application_vcpu_limit(
			std::numeric_limits<unsigned>::max()) == 8);
	for (unsigned physical = 0; physical <= 64; ++physical) {
		const auto limit = budget::application_vcpu_limit(physical);
		require(limit >= 1 && limit <= budget::measured_vcpu_maximum);
		require(limit <= std::max(physical, 1U));
		/* No limit lies beyond a measured row of the cap and of G_max. */
		require(budget::gadget_cache_cap(limit) ==
			budget::gadget_cache_cap_mib[limit - 1] * mib);
	}
}

/* A run starts exactly the sizes from 512 MiB through the ceiling in 64 MiB
 * steps: the runtime refuses a size by guest_memory_admitted under
 * process_guest_memory_ceiling_mib, as the loader answers the applications.
 * The ceiling is the first size past it that is refused. */
void aRunStartsTheSizesFromTheMinimumThroughTheCeilingOnTheSteps()
{
	static_assert(budget::guest_memory_admitted(512, 4096));
	static_assert(budget::guest_memory_admitted(4096, 4096));
	static_assert(budget::guest_memory_admitted(4160, 9152),
		"a size past 4 GiB is no longer refused");
	static_assert(budget::guest_memory_admitted(9152, 9152));
	static_assert(!budget::guest_memory_admitted(9216, 9152));
	static_assert(!budget::guest_memory_admitted(4160, 4096));
	static_assert(!budget::guest_memory_admitted(0, 9152));
	static_assert(!budget::guest_memory_admitted(448, 9152));
	static_assert(!budget::guest_memory_admitted(513, 9152));
	static_assert(!budget::guest_memory_admitted(4097, 9152));
	static_assert(!budget::guest_memory_admitted(
		std::numeric_limits<std::uint64_t>::max(), 9152));
	/* A process: the device window's ceiling (the iPad mini 6's RAM is
	 * under 4 GiB, a 16 GB iPad's over the window) and a macOS window's. */
	static_assert(budget::process_guest_memory_ceiling_mib(false, 0) == 9152);
	static_assert(budget::process_guest_memory_ceiling_mib(false,
		16384 * mib) == 9152);
	static_assert(budget::process_guest_memory_ceiling_mib(false,
		3862 * mib) == 4096);
	static_assert(budget::process_guest_memory_ceiling_mib(true,
		65536 * mib) == 65536);
	static_assert(budget::process_guest_memory_ceiling_mib(true, 0) ==
		budget::guest_memory_range_maximum_mib);
	constexpr std::uint32_t ceiling = 9152;
	for (std::uint64_t size = 0; size <= ceiling + 256; ++size) {
		const bool on_steps = size >= 512 && size <= ceiling &&
			(size - 512) % 64 == 0;
		require(budget::guest_memory_admitted(size, ceiling) == on_steps,
			std::to_string(size) + " MiB");
	}
}

/* The settings offer no size the runtime refuses: for each window, the host's
 * RAM (the iPad mini 6, 6, 8, 12 and 16 GB devices, Macs, unknown), limit
 * (none through 20 GiB) and vCPU count, the offered maximum, the conservative
 * size and the default are sizes the run admits under the same ceiling, the
 * ceiling is the largest the run admits, and the step past it is refused. */
void everyOfferedSizeIsOneTheRunAdmits()
{
	constexpr std::array<std::uint64_t, 9> rams{
		3862, 6144, 8192, 12288, 16384, 24576, 65536, 196608, 0};
	for (const bool macos_window : {false, true})
		for (const auto ram : rams) {
			const auto ceiling = budget::process_guest_memory_ceiling_mib(
				macos_window, ram * mib);
			require(budget::guest_memory_admitted(ceiling, ceiling));
			require(!budget::guest_memory_admitted(
				ceiling + budget::guest_memory_step_mib, ceiling));
			require(!budget::guest_memory_admitted(ceiling - 1U, ceiling));
			for (std::uint64_t limit_mib = 0; limit_mib <= 20480;
			     limit_mib += limit_mib == 0 ? 1536 : 61) {
				const auto limit = limit_mib * mib;
				for (unsigned vcpus = 1; vcpus <= 8; ++vcpus) {
					const auto maximum = budget::maximum_guest_memory_mib(
						limit, vcpus, budget::maximum_memory_set, ceiling);
					require(budget::guest_memory_admitted(maximum, ceiling),
						"the offered maximum " + std::to_string(maximum) +
							" MiB at " + std::to_string(limit_mib) +
							" MiB and " + std::to_string(vcpus) +
							" vCPUs is refused under " +
							std::to_string(ceiling));
					require(budget::guest_memory_admitted(
						budget::conservative_guest_memory_mib(limit, vcpus,
							budget::launch_memory_set, ceiling),
						ceiling));
					require(budget::guest_memory_admitted(
						budget::default_guest_memory_mib(limit, vcpus,
							budget::launch_memory_set, ceiling),
						ceiling));
				}
			}
		}
}


/* The native kernel window of a guest is its RAM, an allowance of the same
 * size held between 1 and 4 GiB and the 1 GiB aperture with the stub pages:
 * the arithmetic the budget keeps for the address space, which the layout's own
 * derivation is held to in emulator_options_tests. */
void theNativeWindowIsTheRamItsAllowanceAndTheFixedSpan()
{
	constexpr std::uint64_t gib = 1024 * mib;
	constexpr std::uint64_t kib = 1024;
	static_assert(budget::native_window_fixed_bytes == gib + 44 * kib);
	static_assert(budget::native_window_bytes(0) == 0);
	static_assert(budget::native_window_bytes(512 * mib) ==
		512 * mib + gib + gib + 44 * kib,
		"a small guest keeps the 1 GiB allowance");
	static_assert(budget::native_window_bytes(1536 * mib) ==
		4 * gib + 44 * kib, "the allowance tracks the RAM");
	static_assert(budget::native_window_bytes(2048 * mib) ==
		5 * gib + 44 * kib);
	static_assert(budget::native_window_bytes(4096 * mib) ==
		9 * gib + 44 * kib, "the allowance stops at 4 GiB");
	static_assert(budget::native_window_bytes(8192 * mib) ==
		13 * gib + 44 * kib);
	static_assert(budget::native_window_bytes(1) ==
		gib + 4 * kib + gib + 44 * kib,
		"the RAM is rounded up to the guest page");
	std::uint64_t previous = 0;
	for (std::uint64_t size = 512; size <= 16384; size += 64) {
		const auto window = budget::native_window_bytes(size * mib);
		require(window > previous, "a larger guest has a larger window");
		previous = window;
	}
	/* The largest guest an iOS device admits fills the signed 14 GiB envelope
	 * to within one step. */
	const auto largest = budget::native_window_bytes(
		std::uint64_t{budget::device_window_guest_memory_maximum_mib} * mib);
	require(largest <= 14 * gib && largest + 64 * mib > 14 * gib,
		"the device bound is the envelope's fill");
}

/* The most RAM a run of address space holds a window of, with the reserve left
 * over: the iPad mini 6 gives a process signed without Extended Virtual
 * Addressing a run of 6 GiB above its guard region. */
void theAddressSpaceBoundsTheRamByTheWindowItNeeds()
{
	constexpr std::uint64_t gib = 1024 * mib;
	static_assert(budget::address_space_reserve_bytes == 1024 * mib + 64 * mib,
		"the heap region's 1 GiB and a margin");
	static_assert(budget::address_space_guest_memory_mib(0) ==
		budget::guest_memory_range_maximum_mib,
		"a host that cannot say has no bound");
	static_assert(budget::address_space_guest_memory_mib(6 * gib) == 1984,
		"1984 MiB needs 4992 MiB and 44 KiB, 2048 MiB needs 5120 MiB and "
		"44 KiB, and 6 GiB less the reserve is 5056 MiB");
	static_assert(budget::address_space_guest_memory_mib(5 * gib) == 1472,
		"1536 MiB needs 4096 MiB and 44 KiB");
	static_assert(budget::address_space_guest_memory_mib(4 * gib) == 896,
		"960 MiB needs 3008 MiB and 44 KiB, the allowance being 1 GiB at least");
	static_assert(budget::address_space_guest_memory_mib(2 * gib) == 0,
		"not even the smallest guest fits");
	static_assert(budget::address_space_guest_memory_mib(
		budget::address_space_reserve_bytes) == 0);
	static_assert(budget::address_space_guest_memory_mib(1) == 0);
	/* The step above never fits and the step itself does. */
	for (std::uint64_t free = 0; free <= 80 * gib; free += 17 * mib + 4096) {
		const auto size = budget::address_space_guest_memory_mib(free);
		if (free == 0) {
			require(size == budget::guest_memory_range_maximum_mib);
			continue;
		}
		if (size == 0) {
			require(budget::native_window_bytes(512 * mib) +
				budget::address_space_reserve_bytes > free);
			continue;
		}
		require(size >= 512 && (size - 512) % 64 == 0);
		require(budget::native_window_bytes(size * mib) +
				budget::address_space_reserve_bytes <= free,
			"the window and the reserve fit the run");
		if (size < budget::guest_memory_range_maximum_mib)
			require(budget::native_window_bytes(
					(size + 64ULL) * mib) +
					budget::address_space_reserve_bytes > free,
				"the next step does not");
	}
	/* More address space never offers less. */
	std::uint32_t previous = 0;
	for (std::uint64_t free = 1; free <= 80 * gib; free += 64 * mib) {
		const auto size = budget::address_space_guest_memory_mib(free);
		require(size >= previous);
		previous = size;
	}
}

/* A process signed without the entitlements, on a device whose limit leaves a
 * default of 2 GiB or more, still gets a default, an offered maximum and a
 * conservative size its window fits: the ceiling follows the address space, and
 * the settings answer from it. */
void anAddressSpaceHoldsTheCeilingTheDefaultAndEverySizeOffered()
{
	constexpr std::uint64_t gib = 1024 * mib;
	/* No entitlement on the iPad mini 6 (6 GiB), the phone without the memory
	 * entitlements (a run between 5 and 6 GiB), Extended Virtual Addressing
	 * (64 GiB), and a host that could not measure. */
	for (const std::uint64_t free :
	     {4 * gib, 5 * gib, 5 * gib + 512 * mib, 6 * gib, 8 * gib, 10 * gib,
		     16 * gib, 64 * gib, std::uint64_t{0}}) {
		const auto ceiling = budget::process_guest_memory_ceiling_mib(
			false, 8192 * mib, free);
		require(ceiling >= budget::guest_memory_minimum_mib &&
			ceiling <= budget::device_window_guest_memory_maximum_mib);
		require((ceiling - budget::guest_memory_minimum_mib) %
				budget::guest_memory_step_mib == 0);
		if (free != 0 && ceiling > budget::guest_memory_minimum_mib)
			require(budget::native_window_bytes(ceiling * mib) +
					budget::address_space_reserve_bytes <= free,
				"the largest size the process admits has a window that fits");
		for (const std::uint64_t limit_mib : {2867ULL, 3376ULL, 4096ULL,
			     6144ULL, 8192ULL, 0ULL})
			for (unsigned vcpus = 1; vcpus <= 8; ++vcpus) {
				const auto limit = limit_mib * mib;
				const auto maximum = budget::maximum_guest_memory_mib(limit,
					vcpus, budget::maximum_memory_set, ceiling);
				const auto safe = budget::conservative_guest_memory_mib(limit,
					vcpus, budget::launch_memory_set, ceiling);
				const auto standard = budget::default_guest_memory(limit,
					vcpus, budget::launch_memory_set, ceiling);
				const std::string what = std::to_string(free / mib) +
					" MiB of address space, limit " +
					std::to_string(limit_mib) + " MiB, " +
					std::to_string(vcpus) + " vCPUs";
				require(maximum <= ceiling, "offered maximum: " + what);
				require(safe <= ceiling, "conservative: " + what);
				require(standard.mib <= ceiling, "default: " + what);
				require(budget::guest_memory_admitted(maximum, ceiling) &&
					budget::guest_memory_admitted(safe, ceiling) &&
					budget::guest_memory_admitted(standard.mib, ceiling),
					what);
				if (free != 0)
					require(budget::native_window_bytes(maximum * mib) +
							budget::address_space_reserve_bytes <= free ||
						maximum == budget::guest_memory_minimum_mib,
						"the window of the offered maximum fits: " + what);
			}
	}
	/* The phone without the memory entitlements: 3376 MiB leaves a default of
	 * 2048 MiB at 6 vCPUs on a device with room, and 1536 MiB in a 5 GiB run. */
	static_assert(budget::default_guest_memory_mib(3376 * mib, 6) == 2048);
	static_assert(budget::default_guest_memory_mib(3376 * mib, 6,
		budget::launch_memory_set, 1984) == 1536);
	static_assert(budget::default_guest_memory_mib(3376 * mib, 6,
		budget::launch_memory_set, 1792) == 1536);
	static_assert(budget::default_guest_memory_mib(3376 * mib, 6,
		budget::launch_memory_set, 2240) == 2048);
	static_assert(budget::default_guest_memory_mib(6144 * mib, 6,
		budget::launch_memory_set, 9152) == 4096,
		"the entitled device keeps its default");
	/* An address space with the entitlement is no bound at all. */
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		64 * gib) == 8192);
	static_assert(budget::process_guest_memory_ceiling_mib(false, 0,
		64 * gib) == 9152);
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		6 * gib) == 1984);
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		0) == 8192, "an address space that was not measured bounds nothing");
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		2 * gib) == 0,
		"a measured run that holds no window admits no size");
	static_assert(budget::process_guest_memory_ceiling_mib(true, 8192 * mib,
		6 * gib) == 8192, "a macOS window is not placed in what is left");
}

/* A process the system leaves no room for even the smallest guest's window has
 * a ceiling of zero, and a ceiling of zero is carried to every answer the
 * loader gives the applications: no size is admitted, offered or defaulted, at
 * any limit, vCPU count or set. It is never raised to the smallest size, which
 * is a size the reservation of its window would then fail. */
void aProcessWithNoRoomForAWindowAdmitsNoSizeAndOffersNone()
{
	static_assert(budget::address_space_guest_memory_mib(1) == 0);
	static_assert(budget::guest_memory_ceiling_mib(
		budget::device_window_guest_memory_maximum_mib, 8192 * mib, 0) == 0);
	static_assert(budget::guest_memory_ceiling_mib(
		budget::device_window_guest_memory_maximum_mib, 0, 0) == 0);
	static_assert(budget::process_guest_memory_ceiling_mib(false, 0, 1) == 0,
		"a measured run of one byte, the loader's measure of none");
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		budget::address_space_reserve_bytes) == 0);
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		budget::native_window_bytes(512 * mib) +
			budget::address_space_reserve_bytes - 1) == 0,
		"a byte short of the smallest guest's window and the reserve");
	static_assert(budget::process_guest_memory_ceiling_mib(false, 8192 * mib,
		budget::native_window_bytes(512 * mib) +
			budget::address_space_reserve_bytes) ==
			budget::guest_memory_minimum_mib,
		"room for exactly the smallest guest");
	/* A host that measured nothing is not one that measured none. */
	static_assert(budget::process_guest_memory_ceiling_mib(false, 0, 0) == 9152);
	for (std::uint32_t size = 0; size <= 9152 + 64; ++size)
		require(!budget::guest_memory_admitted(size, 0),
			"a size was admitted where no window fits");
	for (const std::uint64_t limit_mib : {2867ULL, 3376ULL, 4096ULL, 6144ULL,
		     8192ULL, 0ULL})
		for (unsigned vcpus = 0; vcpus <= 8; ++vcpus) {
			const auto limit = limit_mib * mib;
			const std::string what = "limit " + std::to_string(limit_mib) +
				" MiB, " + std::to_string(vcpus) + " vCPUs";
			require(budget::maximum_guest_memory_mib(limit, vcpus,
					budget::maximum_memory_set, 0) == 0,
				"offered maximum: " + what);
			require(budget::conservative_guest_memory_mib(limit, vcpus,
					budget::launch_memory_set, 0) == 0,
				"conservative size: " + what);
			const auto standard = budget::default_guest_memory(limit, vcpus,
				budget::launch_memory_set, 0);
			require(standard.mib == 0 && !standard.holds_gpu_budget,
				"default: " + what);
		}
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-cap-is-seven-eighths-of-the-measured-ceiling-rounded-up-and-sixteen-more",
			theCapIsSevenEighthsOfTheMeasuredCeilingRoundedUpAndThirtyTwoMore},
		{"a-count-of-zero-is-one-and-further-vcpus-add-sixteen-each",
			aCountOfZeroIsOneAndFurtherVCPUsAddSixteenEach},
		{"the-slot-tags-are-four-mebibytes-each",
			theSlotTagsAreFourMebibytesEach},
		{"the-cache-budget-is-capped-only-under-a-limit",
			theCacheBudgetIsCappedOnlyUnderALimit},
		{"the-burst-is-the-measured-excess-of-the-charge-over-the-budget",
			theBurstIsTheMeasuredExcessOfTheChargeOverTheBudget},
		{"the-engine-is-the-cache-its-burst-the-slot-tags-and-the-rest",
			theEngineIsTheCacheItsBurstTheSlotTagsAndTheRest},
		{"the-launch-set-is-conservative-and-the-maximum-set-counts-none",
			theLaunchSetIsConservativeAndTheMaximumSetCountsNone},
		{"the-maximum-on-an-ipad-follows-the-vcpu-count-for-every-set",
			theMaximumOnAnIPadFollowsTheVCPUCountForEverySet},
		{"the-maximum-is-the-largest-size-that-leaves-the-floor",
			theMaximumIsTheLargestSizeThatLeavesTheFloor},
		{"more-vcpus-never-raise-the-maximum", aMoreVCPUsNeverRaiseTheMaximum},
		{"the-default-on-an-ipad-mini-is-one-and-a-half-gib-at-every-vcpu-count",
			theDefaultOnAnIPadMiniIsOneAndAHalfGiBAtEveryVCPUCount},
		{"the-default-follows-the-limit-in-half-gib-steps",
			theDefaultFollowsTheLimitInHalfGiBSteps},
		{"the-conservative-default-is-the-largest-step-that-leaves-the-floor",
			theConservativeDefaultIsTheLargestStepThatLeavesTheFloor},
		{"more-vcpus-never-raise-the-conservative-default",
			aMoreVCPUsNeverRaiseTheConservativeDefault},
		{"more-vcpus-never-raise-the-default", aMoreVCPUsNeverRaiseTheDefault},
		{"the-default-keeps-a-gibibyte-for-the-gpu-at-the-top-of-the-range",
			theDefaultKeepsAGibibyteForTheGpuAtTheTopOfTheRange},
		{"on-a-device-that-offers-4-gib-the-default-keeps-a-gibibyte-for-the-gpu",
			onADeviceThatOffers4GiBTheDefaultKeepsAGibibyteForTheGpu},
		{"the-gpu-budget-rule-is-flagged-only-where-it-lowers-the-default",
			theGpuBudgetRuleIsFlaggedOnlyWhereItLowersTheDefault},
		{"a-set-that-leaves-no-gibibyte-leaves-the-conservative-default",
			aSetThatLeavesNoGibibyteLeavesTheConservativeDefault},
		{"the-budget-is-the-evaluation-the-run-makes",
			theBudgetIsTheEvaluationTheRunMakes},
		{"a-guest-above-the-default-is-what-the-applications-warn-about",
			aGuestAboveTheDefaultIsWhatTheApplicationsWarnAbout},
		{"no-limit-offers-the-whole-range", noLimitOffersTheWholeRange},
		{"a-tight-limit-offers-the-smallest-guest",
			aTightLimitOffersTheSmallestGuest},
		{"a-larger-ceiling-and-no-half-cap-change-no-default-and-no-size-under-4-gib",
			aLargerCeilingAndNoHalfCapChangeNoDefaultAndNoSizeUnder4GiB},
		{"a-limit-offers-what-the-budget-gives-up-to-the-ceiling",
			aLimitOffersWhatTheBudgetGivesUpToTheCeiling},
		{"the-ceiling-is-the-window-and-the-hosts-ram-whichever-is-less",
			theCeilingIsTheWindowAndTheHostsRAMWhicheverIsLess},
		{"the-application-limit-is-the-physical-cores-at-most-the-measured-count",
			theApplicationLimitIsThePhysicalCoresAtMostTheMeasuredCount},
		{"a-run-starts-the-sizes-from-the-minimum-through-the-ceiling-on-the-steps",
			aRunStartsTheSizesFromTheMinimumThroughTheCeilingOnTheSteps},
		{"every-offered-size-is-one-the-run-admits",
			everyOfferedSizeIsOneTheRunAdmits},
		{"the-native-window-is-the-ram-its-allowance-and-the-fixed-space",
			theNativeWindowIsTheRamItsAllowanceAndTheFixedSpan},
		{"the-address-space-bounds-the-ram-by-the-window-it-needs",
			theAddressSpaceBoundsTheRamByTheWindowItNeeds},
		{"an-address-space-holds-the-ceiling-the-default-and-every-size-offered",
			anAddressSpaceHoldsTheCeilingTheDefaultAndEverySizeOffered},
		{"a-process-with-no-room-for-a-window-admits-no-size-and-offers-none",
			aProcessWithNoRoomForAWindowAdmitsNoSizeAndOffersNone},
	};
	return uml::testing::runNamedTests(tests);
}
