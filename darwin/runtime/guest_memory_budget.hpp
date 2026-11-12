// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the guest side of the process holds besides the guest's RAM, as a
 * function of the vCPU count, the most RAM a process limit leaves a guest, and
 * the RAM a new guest gets. The gadget-cache budget a run sets, the E of the
 * host GPU's budget (platform/gpu_budget.hpp), the bound the application's
 * settings put on the guest's RAM and the default it starts from all come from
 * here, so none of them states the formula itself. The vCPU count the
 * applications default to and offer comes from here too
 * (application_vcpu_limit), since this header holds the count the budget is
 * measured for.
 *
 *     E(v, G) = c(v, G) + t * v + E_rest
 *     c(v, G) = min(derive_budget(G, L), cap(v))      (the cap only where L != 0)
 *     G_max(v) = the largest G in {512, 576, ..., C} MiB with
 *                L - G - A - E(v, G) - S >= 384 MiB; C where L is zero
 *     conservative(v) = G_max(v) under the launch set, in 512 MiB steps and
 *                  at least 1536 MiB, and never above G_max(v) under the
 *                  maximum set
 *     default(v) = conservative(v) held to 4096 MiB, and on a device whose
 *                  G_max(1) under the maximum set reaches 4096 MiB also no
 *                  more than the largest 512 MiB step of G with B(G, v) >=
 *                  1024 MiB under the launch set
 *
 * L is the process's memory limit (platform::process_memory_limit_bytes), zero
 * for none. cap(v) is seven eighths of the cache's measured worst ceiling at v
 * vCPUs, rounded up to 16 MiB. t is the slot tags each executor keeps outside
 * the cache's ledger. A, E_rest and S come from a LaunchMemorySet.
 *
 * C is the host's ceiling (guest_memory_ceiling_mib): the most RAM the host
 * admits whatever the budget says. Nothing else bounds G_max(v): the offered
 * maximum is what the budget gives the device at the vCPU count. The default
 * is held to 4096 MiB (default_guest_memory_maximum_mib) however much is
 * offered, and its rules are the ones that held for a range ending there.
 *
 * Header-only and standard library only, so the application's loader includes
 * it too and answers the settings without the runtime, which is loaded once
 * per process for the flavour chosen at launch. The includes are relative for
 * the same reason: the application's project has no search path into darwin/.
 */

#include "../cpu/gadget_cache_budget.hpp"
#include "../platform/gpu_budget.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace uml::runtime::guest_memory {

inline constexpr std::uint64_t mebibyte = std::uint64_t{1} << 20;

/* The guest RAM a launch offers: 512 MiB up to the host's ceiling, in 64 MiB
 * steps. */
inline constexpr std::uint32_t guest_memory_minimum_mib = 512;
inline constexpr std::uint32_t guest_memory_step_mib = 64;
/* The top of any range: 1 TiB, the most a memory limit may state
 * (platform::parse_memory_limit_mib). No host's ceiling is above it. */
inline constexpr std::uint32_t guest_memory_range_maximum_mib =
	std::uint32_t{1} << 20;

/* The most guest RAM an iOS device or the iOS Simulator admits: 9152 MiB. The
 * native kernel window is packed into the signed 14 GiB envelope
 * (runtime/native_kernel_layout.hpp), and it holds the RAM, an auxiliary
 * allowance of the same size up to 4 GiB, and a fixed span of 1 GiB and 44
 * KiB for the shared-memory aperture and the stub pages, so the RAM that fits
 * is 14 GiB - 4 GiB - 1 GiB - 44 KiB, taken down to the 64 MiB steps. The
 * layout tests pin it. A macOS window (a Mac, or an iOS application on one)
 * sits in the 128 TiB range and has no such bound. */
inline constexpr std::uint32_t device_window_guest_memory_maximum_mib = 9152;

/* The default guest RAM of a new guest: a multiple of 512 MiB within this
 * range (default_guest_memory_mib). Its top is no cap on what is offered: it
 * is the largest default, and the line the rule that keeps a gibibyte for the
 * host GPU applies from. */
inline constexpr std::uint32_t default_guest_memory_minimum_mib = 1536;
inline constexpr std::uint32_t default_guest_memory_maximum_mib = 4096;
inline constexpr std::uint32_t default_guest_memory_step_mib = 512;

/* The host GPU's budget B a new guest keeps where the device lets a guest have
 * the top of the range (default_guest_memory_mib). */
inline constexpr std::uint64_t default_gpu_budget_minimum_mib = 1024;

/* The most the arm64 gadget cache may hold at 1 through 8 vCPUs: seven eighths
 * of the worst ceiling any session reached in the Android and KDE loads that
 * were measured on the Mac and the iPad, rounded up to 16 MiB. The cap sits
 * under those loads' worst ceilings, so the budget's own shedding
 * (cpu/gadget_cache_budget.hpp) bounds the cache there. Geekbench's multi-core
 * peaks at 8 vCPUs, 71 to 94 MiB, stay far below it, and forcing the budget to
 * 64 MiB there cost no measurable score. */
inline constexpr std::array<std::uint32_t, 8> gadget_cache_cap_mib{
	128, 160, 192, 224, 240, 240, 272, 272};
/* Each vCPU above the last measured one adds this to the cap. */
inline constexpr std::uint32_t gadget_cache_cap_further_executor_mib = 16;

/* The most vCPUs the table above, and with it G_max(v), is measured for. */
inline constexpr unsigned measured_vcpu_maximum =
	static_cast<unsigned>(gadget_cache_cap_mib.size());

/* The vCPU count the applications (Kernarium, Andarium and the macOS
 * launchers) default to and offer at most, on a host with `physical_processors`
 * physical cores: every core, and never more than the count the memory budget
 * is measured for. An explicit ncpus= on a launcher keeps the runtime's own
 * range; a test states its own count. A count of zero is one. */
[[nodiscard]] constexpr unsigned application_vcpu_limit(
	unsigned physical_processors) noexcept
{
	return std::clamp(physical_processors, 1U, measured_vcpu_maximum);
}

/* cap(v), in bytes; a count of zero is one. */
[[nodiscard]] constexpr std::size_t gadget_cache_cap(unsigned executors) noexcept
{
	const std::size_t measured = measured_vcpu_maximum;
	const std::size_t count = std::max<std::size_t>(executors, 1U);
	const std::size_t mib = count <= measured ?
		gadget_cache_cap_mib[count - 1U] :
		gadget_cache_cap_mib.back() +
			gadget_cache_cap_further_executor_mib * (count - measured);
	return mib * static_cast<std::size_t>(mebibyte);
}

/* The tags of an executor's 262,144 slots, 16 bytes each: 4 MiB, charged to no
 * account because the slot count fixes them. It is the default slot count
 * (UML_GADGET_CACHE_SLOTS); a build that chooses another does not change what
 * the settings assume. */
inline constexpr std::size_t slot_tag_bytes_per_executor =
	std::size_t{262144} * 16U;

/* t * v, in bytes; a count of zero is one. */
[[nodiscard]] constexpr std::size_t slot_tag_bytes(unsigned executors) noexcept
{
	return slot_tag_bytes_per_executor * std::max(executors, 1U);
}

/* c(v, G): the budget the gadget cache gets for a guest of `guest_memory`
 * bytes under `memory_limit` bytes (zero for none) at `executors` vCPUs. Where
 * the host sets no limit the budget derives from the guest alone and is not
 * capped, so a host without a limit behaves as it did before the cap existed. */
[[nodiscard]] constexpr std::size_t gadget_cache_budget(
	std::size_t guest_memory, std::size_t memory_limit,
	unsigned executors) noexcept
{
	auto budget = ::uml::cpu::gadget_cache::derive_budget(
		guest_memory, memory_limit);
	if (memory_limit != 0)
		budget = std::min(budget, gadget_cache_cap(executors));
	return budget;
}

/* E(v, G): the gadget cache in effect, the slot tags of `executors` vCPUs and
 * `engine_rest`, saturating. */
[[nodiscard]] constexpr std::uint64_t engine_memory(std::uint64_t cache_budget,
	unsigned executors, std::uint64_t engine_rest) noexcept
{
	constexpr auto unlimited = std::numeric_limits<std::uint64_t>::max();
	const std::uint64_t cache_and_tags =
		cache_budget > unlimited - slot_tag_bytes(executors) ?
		unlimited : cache_budget + slot_tag_bytes(executors);
	return cache_and_tags > unlimited - engine_rest ? unlimited :
		cache_and_tags + engine_rest;
}

/* A, E_rest and S of the host GPU's budget, in bytes: the launch parameters.
 * E_rest is the guest side's memory beyond the cache and the slot tags. */
struct LaunchMemorySet final {
	std::uint64_t application;
	std::uint64_t engine_rest;
	std::uint64_t headroom;
};

[[nodiscard]] constexpr LaunchMemorySet launch_memory_mib(
	std::uint64_t application, std::uint64_t engine_rest,
	std::uint64_t headroom) noexcept
{
	return {application * mebibyte, engine_rest * mebibyte,
		headroom * mebibyte};
}

/* The two sets of A, E_rest and S, in MiB, and which uses which:
 *
 *   launch_memory_set   conservative: the default guest RAM of a new guest
 *                       (default_guest_memory_mib), the line above which the
 *                       process is likely to be ended
 *                       (conservative_guest_memory_mib) and the host GPU's
 *                       budget B of every launch that states no set of its
 *                       own.
 *   maximum_memory_set  counts none of them: only the most RAM a person may
 *                       choose (maximum_guest_memory_mib). A guest above the
 *                       conservative line but within this maximum is allowed;
 *                       the applications warn that the process is then likely
 *                       to be ended.
 *
 * A is the application's own ceiling. E_rest is the guest side's memory beyond
 * the cache and the slot tags at its worst. S is the headroom: the largest
 * transient (45), B's overshoot past its pool (40) and a long session's growth
 * (64). */
inline constexpr LaunchMemorySet launch_memory_set =
	launch_memory_mib(64, 320, 45 + 40 + 64);
inline constexpr LaunchMemorySet maximum_memory_set =
	launch_memory_mib(0, 0, 0);

/* B: the host GPU's budget a launch of `guest_mib` MiB of guest RAM gets at
 * `executors` vCPUs under `memory_limit` bytes (zero for none, which gives the
 * guest's RAM) with `set`, and whether the limit leaves it less than its
 * floor. It is the evaluation the run makes (emulator/src/gpu_budget_plan.hpp)
 * when the launch states no gadget cache budget of its own, so the settings can
 * show what a guest will get. */
[[nodiscard]] constexpr ::uml::platform::GpuBudget gpu_budget(
	std::uint64_t memory_limit, std::uint32_t guest_mib, unsigned executors,
	const LaunchMemorySet &set = launch_memory_set) noexcept
{
	const std::uint64_t guest = std::uint64_t{guest_mib} * mebibyte;
	const auto cache = gadget_cache_budget(static_cast<std::size_t>(guest),
		static_cast<std::size_t>(memory_limit), executors);
	return ::uml::platform::compute_gpu_budget({
		.process_limit = memory_limit,
		.guest_memory = guest,
		.application = set.application,
		.engine = engine_memory(cache, executors, set.engine_rest),
		.headroom = set.headroom,
	});
}

/* C: the most guest RAM, in MiB, the host admits whatever the budget says,
 * taken down to the 64 MiB steps. `window_maximum_mib` is what the product's
 * native kernel window holds (device_window_guest_memory_maximum_mib on an iOS
 * device or the Simulator, guest_memory_range_maximum_mib where the window is
 * a macOS one); `physical_memory` is the host's RAM in bytes, zero when
 * unknown. A host never admits more RAM than it has. It never admits less than
 * default_guest_memory_maximum_mib, the range every host offered before
 * there was a ceiling, so the default is always one of the sizes on offer. */
[[nodiscard]] constexpr std::uint32_t guest_memory_ceiling_mib(
	std::uint64_t window_maximum_mib,
	std::uint64_t physical_memory) noexcept
{
	std::uint64_t top = std::min<std::uint64_t>(window_maximum_mib,
		guest_memory_range_maximum_mib);
	if (physical_memory != 0)
		top = std::min(top, physical_memory / mebibyte);
	top = std::max<std::uint64_t>(top, default_guest_memory_maximum_mib);
	return static_cast<std::uint32_t>(guest_memory_minimum_mib +
		(top - guest_memory_minimum_mib) / guest_memory_step_mib *
			guest_memory_step_mib);
}

namespace detail {

/* `mib` taken up into the steps' range and down onto its steps. */
[[nodiscard]] constexpr std::uint32_t on_guest_memory_steps(
	std::uint64_t mib) noexcept
{
	mib = std::clamp<std::uint64_t>(mib, guest_memory_minimum_mib,
		guest_memory_range_maximum_mib);
	return static_cast<std::uint32_t>(guest_memory_minimum_mib +
		(mib - guest_memory_minimum_mib) / guest_memory_step_mib *
			guest_memory_step_mib);
}

/* The largest guest RAM up to `ceiling_mib` for which `memory_limit` bytes
 * (zero for none) leave the host GPU at least its floor with `set`: the
 * ceiling where there is no limit, the smallest guest where none leaves it. A
 * larger guest leaves the GPU less, so the search starts at the ceiling, or at
 * the limit less the floor where that is lower, since no term of the sum is
 * negative. */
[[nodiscard]] constexpr std::uint32_t largest_fitting_guest_memory_mib(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set, std::uint32_t ceiling_mib) noexcept
{
	const auto top = on_guest_memory_steps(ceiling_mib);
	if (memory_limit == 0)
		return top;
	constexpr std::uint64_t floor_mib =
		::uml::platform::gpu_budget_floor_bytes / mebibyte;
	const std::uint64_t limit_mib = memory_limit / mebibyte;
	auto start = top;
	if (limit_mib < std::uint64_t{top} + floor_mib)
		start = limit_mib < guest_memory_minimum_mib + floor_mib ?
			guest_memory_minimum_mib :
			on_guest_memory_steps(limit_mib - floor_mib);
	for (auto mib = start; mib > guest_memory_minimum_mib;
	     mib -= guest_memory_step_mib)
		if (!gpu_budget(memory_limit, mib, executors, set)
				.short_of_floor)
			return mib;
	return guest_memory_minimum_mib;
}

} // namespace detail

/* G_max(v), in MiB: the largest guest RAM of the offered range, up to the
 * host's ceiling (guest_memory_ceiling_mib), for which `memory_limit` bytes
 * (zero for none) leave the host GPU at least its floor once the guest, A,
 * E(v, G) and S of `set` are counted. Where the host sets no limit it is the
 * ceiling. Where even the smallest guest leaves less than the floor it is the
 * smallest, and the launch says the process is likely to be ended. Without a
 * set it is the most a person may choose (maximum_memory_set). Without a
 * ceiling it is the one an iOS device admits. */
[[nodiscard]] constexpr std::uint32_t maximum_guest_memory_mib(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set = maximum_memory_set,
	std::uint32_t ceiling_mib =
		device_window_guest_memory_maximum_mib) noexcept
{
	return detail::largest_fitting_guest_memory_mib(
		memory_limit, executors, set, ceiling_mib);
}

/* The most RAM a guest may have before the process is likely to be ended:
 * G_max(v) under `set`, taken down to a multiple of 512 MiB and no lower than
 * 1536 MiB. A limit that leaves less than that does not take it above what the
 * settings offer (G_max(v) under maximum_memory_set), so it stays on the
 * picker's steps and inside its range. Where the host sets no limit nothing is
 * likely to end the process, and it is the whole offered range. Without a set
 * it is the conservative launch set, the one the host GPU's budget uses. A
 * guest above this is likely to be ended by the system. */
[[nodiscard]] constexpr std::uint32_t conservative_guest_memory_mib(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set = launch_memory_set,
	std::uint32_t ceiling_mib =
		device_window_guest_memory_maximum_mib) noexcept
{
	const auto maximum = maximum_guest_memory_mib(
		memory_limit, executors, maximum_memory_set, ceiling_mib);
	if (memory_limit == 0)
		return maximum;
	const auto safe = detail::largest_fitting_guest_memory_mib(
		memory_limit, executors, set, ceiling_mib);
	const auto stepped = safe / default_guest_memory_step_mib *
		default_guest_memory_step_mib;
	return std::min(std::max(stepped, default_guest_memory_minimum_mib),
		maximum);
}

/* The most guest RAM, a multiple of 512 MiB within the range a new guest's RAM
 * is held to (default_guest_memory_minimum_mib through
 * default_guest_memory_maximum_mib), for which `set` leaves the host GPU a
 * budget of at least default_gpu_budget_minimum_mib; zero where no such RAM
 * exists. */
[[nodiscard]] constexpr std::uint32_t graphics_guest_memory_mib(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set = launch_memory_set) noexcept
{
	for (auto mib = default_guest_memory_maximum_mib;
	     mib >= default_guest_memory_minimum_mib;
	     mib -= default_guest_memory_step_mib)
		if (gpu_budget(memory_limit, mib, executors, set).bytes >=
			default_gpu_budget_minimum_mib * mebibyte)
			return mib;
	return 0;
}

/* The RAM a new guest gets, and whether the GPU budget rule below is what set
 * it. Never above default_guest_memory_maximum_mib however much the host
 * offers. On a device whose limit lets a guest have that much (G_max(1) under
 * maximum_memory_set is 4096 MiB when the range ends there), a guest of that
 * size runs the host GPU short long before it runs the process short, so at
 * every vCPU count the default is also held to what keeps B at
 * default_gpu_budget_minimum_mib: the lesser of conservative_guest_memory_mib
 * and graphics_guest_memory_mib. Both fall with the vCPU count, so the default
 * does too. That bound is never under 2048 MiB with the launch set, since
 * every limit that offers 4096 MiB at one vCPU leaves a guest of 2048 more
 * than 1024 for the GPU at every count the applications offer; for a set that
 * leaves none in the range, or for a limit that offers less than 4096 MiB or
 * none at all, the default is the conservative one. A guest above the
 * conservative one is likely to be ended by the system; one between the two is
 * likely to run out of graphics memory. The rules read the range as ending at
 * 4096 MiB, so a larger ceiling changes none of the defaults.
 *
 * holds_gpu_budget is true where the graphics rule is what lowered the
 * default, below the conservative size the default would be otherwise. Above
 * such a default B is under default_gpu_budget_minimum_mib from the next
 * multiple of 512 MiB, which is what the applications warn about. Where the
 * default is the conservative size (a ceiling of 4096 MiB held it, or the
 * limit lets no guest have that much) it says nothing of B. */
struct DefaultGuestMemory {
	std::uint32_t mib;
	bool holds_gpu_budget;
};

[[nodiscard]] constexpr DefaultGuestMemory default_guest_memory(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set = launch_memory_set) noexcept
{
	constexpr auto top = default_guest_memory_maximum_mib;
	const auto conservative = conservative_guest_memory_mib(
		memory_limit, executors, set, top);
	const auto reach = maximum_guest_memory_mib(
		memory_limit, 1, maximum_memory_set, top);
	if (memory_limit == 0 || reach != top)
		return {conservative, false};
	const auto graphics =
		graphics_guest_memory_mib(memory_limit, executors, set);
	if (graphics == 0 || graphics >= conservative)
		return {conservative, false};
	return {graphics, true};
}

[[nodiscard]] constexpr std::uint32_t default_guest_memory_mib(
	std::uint64_t memory_limit, unsigned executors,
	const LaunchMemorySet &set = launch_memory_set) noexcept
{
	return default_guest_memory(memory_limit, executors, set).mib;
}

} // namespace uml::runtime::guest_memory
