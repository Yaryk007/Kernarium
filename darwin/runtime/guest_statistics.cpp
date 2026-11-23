// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_statistics.hpp"

#include "cpu/gadget_cache_budget.hpp"
#include "memory/domain_heap.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "platform/frame_figures.hpp"
#include "platform/gpu_memory_figures.hpp"
#include "platform/host_state_figures.hpp"
#include "vcpu_carrier_name.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <malloc/malloc.h>
#include <mutex>

/* The cycles a process has run come from proc_pid_rusage, which the iOS SDK
 * does not publish: libproc.h exists on macOS only, and no iOS product
 * declares the call itself. */
#if __has_include(<libproc.h>)
#include <libproc.h>
#include <unistd.h>
#define UML_PROCESS_CYCLES 1
#else
#define UML_PROCESS_CYCLES 0
#endif

namespace uml::runtime {

namespace {

/* Leaf: nothing is taken under it. */
std::mutex ram_retirement_mutex;

} // namespace

bool is_vcpu_carrier_name(std::string_view name) noexcept
{
	return vcpu_carrier_index(name) >= 0;
}

GuestCpuTime guest_cpu_time() noexcept
{
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return {};
	GuestCpuTime total;
	total.listed = true;
	for (mach_msg_type_number_t index = 0; index < count; ++index) {
		thread_extended_info_data_t information {};
		mach_msg_type_number_t information_count = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(threads[index], THREAD_EXTENDED_INFO,
			    reinterpret_cast<thread_info_t>(&information),
			    &information_count) == KERN_SUCCESS &&
		    is_vcpu_carrier_name(std::string_view {information.pth_name,
			    ::strnlen(information.pth_name,
				    sizeof(information.pth_name))})) {
			++total.carriers;
			total.nanoseconds +=
				information.pth_user_time + information.pth_system_time;
		}
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(thread_act_t));
	return total;
}

std::optional<GuestRamUsage> guest_ram_usage() noexcept
{
	/* A hand-back in progress holds the lock: the RAM is being given back,
	 * and the reader does not wait for it. */
	const std::unique_lock lock {ram_retirement_mutex, std::try_to_lock};
	if (!lock.owns_lock())
		return std::nullopt;
	const ::uml::memory::GuestRamCommitment *const commitment =
		::uml::memory::guest_ram_commitment();
	if (commitment == nullptr)
		return std::nullopt;
	const std::uint64_t ram = commitment->size();
	const std::uint64_t released = commitment->released_bytes();
	return GuestRamUsage {.ram_bytes = ram,
		.held_bytes = released < ram ? ram - released : 0U,
		.wire_failures = commitment->wire_failures()};
}

GuestRamRetirementScope::GuestRamRetirementScope() noexcept
{
	ram_retirement_mutex.lock();
}

GuestRamRetirementScope::~GuestRamRetirementScope()
{
	ram_retirement_mutex.unlock();
}

namespace {

/* The process's usage as the system keeps it, with its times in nanoseconds.
 * False when the system does not answer. */
struct ProcessUsage final {
	std::uint64_t cpu_ns{};
	std::uint64_t performance_ns{};
	std::uint64_t energy_nj{};
};

/* TASK_POWER_INFO_V2 (a public interface on every Apple platform): the user
 * and system time and the time on performance cores in mach ticks, and the
 * energy the system estimates in nanojoules. */
bool process_usage(ProcessUsage &usage) noexcept
{
	task_power_info_v2_data_t info{};
	mach_msg_type_number_t count = TASK_POWER_INFO_V2_COUNT;
	if (task_info(mach_task_self(), TASK_POWER_INFO_V2,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
	    count < TASK_POWER_INFO_V2_COUNT)
		return false;
	static const mach_timebase_info_data_t base = [] {
		mach_timebase_info_data_t value{};
		(void)mach_timebase_info(&value);
		if (value.numer == 0 || value.denom == 0)
			value = {1, 1};
		return value;
	}();
	const auto nanoseconds = [](std::uint64_t ticks) {
		return ticks * base.numer / base.denom;
	};
	usage.cpu_ns = nanoseconds(
		info.cpu_energy.total_user + info.cpu_energy.total_system);
	usage.performance_ns = nanoseconds(info.task_ptime);
	usage.energy_nj = info.task_energy;
	return true;
}

/* The CPU cycles the process has run. macOS only: false elsewhere. */
bool process_cycles(std::uint64_t &cycles) noexcept
{
#if UML_PROCESS_CYCLES
	rusage_info_v6 info{};
	if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6,
		    reinterpret_cast<rusage_info_t *>(&info)) != 0)
		return false;
	cycles = info.ri_cycles;
	return true;
#else
	(void)cycles;
	return false;
#endif
}

} // namespace

RunFigures run_figures(bool session_begins) noexcept
{
	RunFigures figures;
	if (session_begins)
		::uml::platform::forgetFrameGaps();
	const auto frames = ::uml::platform::takeFrameFigures(
		::uml::platform::frameClockTicks());
	figures.sources |= RunFigures::frames;
	figures.flushes = frames.flushes;
	figures.longest_gap_ns = frames.longestGapNs;
	if (const auto host = ::uml::platform::publishedHostState()) {
		figures.sources |= RunFigures::host;
		figures.thermal = static_cast<std::uint8_t>(host->thermal.level);
		figures.pressure = host->memoryPressure;
	}
	const auto &book = ::uml::cpu::gadget_cache::ledger();
	figures.sources |= RunFigures::cache;
	figures.reliefs = book.reliefs.load(std::memory_order_relaxed);
	figures.shed_accounts =
		book.shed_accounts.load(std::memory_order_relaxed);
	figures.renewals = book.renewals.load(std::memory_order_relaxed);
	std::size_t largest = 0;
	if (::uml::cpu::gadget_cache::largest_open_bytes(largest)) {
		figures.sources |= RunFigures::largest;
		figures.largest_open = largest;
	}
	const GuestCpuTime carriers = guest_cpu_time();
	if (carriers.listed) {
		figures.sources |= RunFigures::carriers;
		figures.carrier_ns = carriers.nanoseconds;
	}
	if (ProcessUsage usage; process_usage(usage)) {
		figures.sources |= RunFigures::cpu | RunFigures::usage;
		figures.cpu_ns = usage.cpu_ns;
		figures.performance_ns = usage.performance_ns;
		figures.energy_nj = usage.energy_nj;
	}
	if (std::uint64_t cycles; process_cycles(cycles)) {
		figures.sources |= RunFigures::cycle_count;
		figures.cycles = cycles;
	}
	return figures;
}

MemoryTerms memory_terms(bool read_malloc) noexcept
{
	MemoryTerms terms;
	if (const auto ram = guest_ram_usage())
		terms.guest_held = ram->held_bytes;
	if (const auto gpu = ::uml::platform::publishedGpuMemory();
	    gpu.budgetBytes != 0) {
		terms.gpu_budget = gpu.budgetBytes;
		terms.gpu_held = gpu.heldBytes;
		if (gpu.heldAtMs != 0)
			terms.gpu_age_ms = std::max<std::int64_t>(
				::uml::platform::gpuMemoryClockMs() - gpu.heldAtMs, 0);
	}
	const auto &cache = ::uml::cpu::gadget_cache::ledger();
	terms.cache = cache.open.load(std::memory_order_relaxed) +
		cache.shedding.load(std::memory_order_relaxed);
	terms.heaps = ::uml::memory::HeapRegion::instance().committed();
	if (read_malloc) {
		malloc_statistics_t zones{};
		malloc_zone_statistics(nullptr, &zones);
		terms.malloc_allocated = zones.size_allocated;
		terms.malloc_in_use = zones.size_in_use;
	}
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
		const auto positive = [](std::int64_t value) {
			return static_cast<std::uint64_t>(std::max<std::int64_t>(value, 0));
		};
		terms.internal = info.internal;
		terms.compressed = info.compressed;
		if (count >= TASK_VM_INFO_REV3_COUNT) {
			terms.graphics = positive(info.ledger_tag_graphics_footprint +
				info.ledger_tag_graphics_footprint_compressed);
			terms.media = positive(info.ledger_tag_media_footprint +
				info.ledger_tag_media_footprint_compressed);
		}
	}
	return terms;
}

} // namespace uml::runtime
