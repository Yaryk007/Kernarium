// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace uml::runtime {

/*
 * What the host can say about a running guest from the system's own
 * accounting, for an application that shows statistics. Nothing here runs
 * until it is called: there is no sampler, timer or counter of its own, and a
 * call asks the guest nothing. The system keeps every thread's CPU time
 * whether or not anyone reads it, and the commitment of the guest's RAM keeps
 * the count of pages the guest released for its own sake.
 */

/* Whether `name` is a vCPU carrier's, uml-vcpu-N-carrier with N in decimal.
 * The kernel names its host threads (kernel/common/README.md), and the
 * carriers are the only ones that run guest code. */
[[nodiscard]] bool is_vcpu_carrier_name(std::string_view name) noexcept;

struct GuestCpuTime final {
	/* The carriers found: the guest's vCPUs that have come up. */
	unsigned carriers{};
	/* The user and system time those threads have used, in nanoseconds. A
	 * carrier that has ended takes its time with it, so the sum of a guest
	 * that restarted starts again from zero. */
	std::uint64_t nanoseconds{};
	/* False when the process's threads could not be listed. */
	bool listed{};
};

/* The CPU time of the process's vCPU carriers. It lists the process's threads
 * and reads each one's name and times, about 40 to 90 microseconds for the
 * 40 to 90 threads of a running guest. Any thread. */
[[nodiscard]] GuestCpuTime guest_cpu_time() noexcept;

struct GuestRamUsage final {
	/* The guest's RAM, kernel image included. */
	std::uint64_t ram_bytes{};
	/* The part of it the host holds now: everything the guest has committed
	 * and not released, and the kernel image, which is never released. */
	std::uint64_t held_bytes{};
	/* The failed wiring operations on the guest's RAM (a commit or a
	 * readmission of a range of pages), since the guest began
	 * (memory/guest_ram_commitment.hpp). */
	std::uint64_t wire_failures{};
};

/* The guest's RAM and the part of it the host holds, or nothing before the
 * guest's kernel has claimed its RAM, while the guest is being handed back to
 * the process and after. Any thread; it never waits: it reads two counters
 * unless a GuestRamRetirementScope is held. */
[[nodiscard]] std::optional<GuestRamUsage> guest_ram_usage() noexcept;

/*
 * The parts of the process's footprint the memory budgets count
 * (guest_memory_budget.hpp), in bytes, for the session record's memory
 * checkpoint (session_record.hpp), so that a record of a process the system
 * ended says which part had grown. Every figure but malloc's is a counter
 * something keeps anyway.
 */
struct MemoryTerms final {
	/* guest_ram_usage()'s held bytes; 0 without a guest. */
	std::uint64_t guest_held{};
	/* The host GPU's budget B and what the GPU held against it when its
	 * device last counted, and how long ago that was (-1: never); all 0
	 * without a budget (platform/gpu_memory_figures.hpp). */
	std::uint64_t gpu_budget{};
	std::uint64_t gpu_held{};
	std::int64_t gpu_age_ms{-1};
	/* The footprint's graphics and media ledgers, compressed pages
	 * included. */
	std::uint64_t graphics{};
	std::uint64_t media{};
	/* What the gadget caches hold, the accounts asked to shed included
	 * (cpu/gadget_cache_budget.hpp). */
	std::uint64_t cache{};
	/* The engine's domain heaps, committed (memory/domain_heap.hpp). */
	std::uint64_t heaps{};
	/* Every malloc zone's bytes, allocated to the zone and in use; 0 unless
	 * asked for. */
	std::uint64_t malloc_allocated{};
	std::uint64_t malloc_in_use{};
	/* The footprint's anonymous memory and its compressed share. */
	std::uint64_t internal{};
	std::uint64_t compressed{};
};

/*
 * What the session record's checkpoint writes besides the memory terms, for a
 * report that must say why a guest's frames dropped: how many frames the guest
 * flushed and the longest gap between two, the host's thermal state and
 * memory pressure, what the gadget cache did, and how much CPU the process and
 * the vCPU carriers used and how it ran. Each counter is a total, so that the
 * record differences two readings; the others are states at the moment of the
 * reading. A source that has nothing to say leaves its bit out of `sources`,
 * and its fields are zero.
 */
struct RunFigures final {
	/* The sources that answered. */
	static constexpr std::uint32_t frames = 1U << 0;
	static constexpr std::uint32_t host = 1U << 1;
	static constexpr std::uint32_t cache = 1U << 2;
	static constexpr std::uint32_t largest = 1U << 3;
	static constexpr std::uint32_t cpu = 1U << 4;
	static constexpr std::uint32_t carriers = 1U << 5;
	static constexpr std::uint32_t usage = 1U << 6;
	static constexpr std::uint32_t cycle_count = 1U << 7;
	std::uint32_t sources{};

	/* Scanout flushes since the process began (platform/frame_figures.hpp),
	 * and the longest gap between two since the previous reading. */
	std::uint64_t flushes{};
	std::uint64_t longest_gap_ns{};
	/* The stall watch's last observation (platform/host_state_figures.hpp):
	 * the thermal state, 0 nominal to 3 critical, and the memory pressure
	 * level, 0 normal to 2 critical. */
	std::uint8_t thermal{};
	std::uint8_t pressure{};
	/* The gadget cache ledger's relief counters since the process began, and
	 * the bytes of its largest open account (cpu/gadget_cache_budget.hpp). */
	std::uint64_t reliefs{};
	std::uint64_t shed_accounts{};
	std::uint64_t renewals{};
	std::uint64_t largest_open{};
	/* The process's user and system time, and the carriers' (guest_cpu_time),
	 * in nanoseconds. */
	std::uint64_t cpu_ns{};
	std::uint64_t carrier_ns{};
	/* From the system's power information of the task (task_info,
	 * TASK_POWER_INFO_V2, on every platform): the part of the process's CPU
	 * time spent on performance cores, and its energy in nanojoules. */
	std::uint64_t performance_ns{};
	std::uint64_t energy_nj{};
	/* The CPU cycles the process has run, from proc_pid_rusage where
	 * <libproc.h> exists (macOS); iOS has no such source. */
	std::uint64_t cycles{};
};

/* The figures now; any thread, and it never waits on the guest. `session_begins`
 * starts the frame gaps afresh for a guest that is about to run. It costs the
 * reading of the carriers' times (guest_cpu_time) and a system call each for
 * the process's usage and, on macOS, its cycles, and holds no lock. */
[[nodiscard]] RunFigures run_figures(bool session_begins) noexcept;

/* The terms now; any thread, and it never waits on the guest. With
 * `read_malloc`, also the malloc zones' statistics: a walk of the zones that
 * holds their locks while it runs, so every thread that allocates or frees
 * meanwhile waits for it, about 4 ms with 900 MiB of malloc. The caller must
 * hold no zone's lock. */
[[nodiscard]] MemoryTerms memory_terms(bool read_malloc) noexcept;

/*
 * Held by whoever retires the guest's RAM commitment (the hand-back of a
 * guest that restarted), so that a reader of guest_ram_usage() never meets
 * the object while it is deleted: a reader that finds it held reports
 * nothing. The commitment is deleted once per restart, never while a guest
 * runs; this exists for the reader that arrives between a guest's end and
 * the next one's start.
 */
class GuestRamRetirementScope final {
public:
	GuestRamRetirementScope() noexcept;
	~GuestRamRetirementScope();

	GuestRamRetirementScope(const GuestRamRetirementScope &) = delete;
	GuestRamRetirementScope &operator=(
		const GuestRamRetirementScope &) = delete;
};

} // namespace uml::runtime
