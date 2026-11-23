// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_wait.hpp"

#include <cstddef>
#include <cstdint>

namespace uml::runtime::stall {

/*
 * The stall record: when a guest's vCPUs stopped running on the host, and
 * what was true of the host then.
 *
 * A vCPU is stalled when, for at least stall_threshold_ms, its carrier thread
 * was runnable and got less than starved_below_percent of the time (starved),
 * or sat in an uninterruptible wait (fault) or in one host wait (blocked: for
 * a device service's answer, a disk, guest RAM, or in no wait the loader
 * marks) and used less than starved_below_percent of the time: a carrier that
 * ran between two samples was not held in the wait both found it in. A carrier
 * in its idle wait is not stalled. The watch (stall_watch.hpp) feeds this
 * file one Tick every tick_ms while it looks; what comes out is a
 * StallRecord, a ring of events and three rings of transitions, which
 * format_stall_slot writes as text into the session record's fixed slot. The
 * rings and the slot never grow: a ring that is full forgets its oldest entry
 * and the slot is cut to its size by dropping whole old events.
 *
 * Everything here is arithmetic on its arguments: it reads no clock, takes no
 * lock and allocates nothing, so the classification and the format are tested
 * with synthetic samples.
 */

inline constexpr unsigned max_vcpus = 16;
/* The watch's period, and how long a stall must last to be recorded. */
inline constexpr unsigned tick_ms = 50;
inline constexpr unsigned stall_threshold_ms = 100;
/* A runnable carrier that got less than this share of the time is starved. */
inline constexpr unsigned starved_below_percent = 20;
/* A tick later than this by the watch's own clock is an event even when no
 * carrier shows a stall: the process was not running. */
inline constexpr unsigned process_late_ms = 150;
/* A gap this long is the process being suspended, not a stall. */
inline constexpr unsigned suspension_gap_ms = 5000;
/* An event ends after this many ticks with nothing stalled, or at this age. */
inline constexpr unsigned quiet_ticks_to_close = 2;
inline constexpr unsigned max_event_ms = 60'000;

inline constexpr std::size_t event_capacity = 32;
inline constexpr std::size_t transition_capacity = 8;
/* The session record's stall slot. */
inline constexpr std::size_t slot_bytes = 4096;

/* A carrier's run state as the system reports it. */
enum class RunState : std::uint8_t {
	running,
	stopped,
	waiting,
	uninterruptible,
	halted,
};

struct VcpuSample final {
	bool present{};
	RunState state{RunState::waiting};
	/* The carrier's user and system time so far. */
	std::uint64_t cpu_ns{};
	/* The carrier's published wait (platform/host_wait.hpp). */
	std::uint64_t wait_word{};
};

struct Tick final {
	/* A monotonic clock. */
	std::uint64_t now_ns{};
	VcpuSample vcpus[max_vcpus]{};
	/* The threads were held, or the process was, since the last tick, and
	 * what they show says nothing of the host. */
	bool disturbed{};
	/* The watch slept since the last tick, with every carrier in its idle
	 * wait (stall_watch.hpp). A carrier that has run since used a sliver of
	 * the sleep, which says nothing of whether it was starved, so the
	 * interval is not measured: every carrier is seen as for the first time,
	 * and the next tick is the first to measure. */
	bool rebaseline{};
	/* How late the watch woke against its own deadline, which leaves out the
	 * watch's work between two waits: a slow write of the slot is not the
	 * process being stopped. */
	std::uint32_t late_ms{};
	/* The main queue's lateness at this tick, -1 where it is not serviced. */
	std::int32_t ui_late_ms{-1};
};

/* Why a vCPU is stalled. */
enum class Cause : std::uint8_t {
	none,
	starved,
	fault,
	/* Blocked in a host wait the loader does not mark. */
	other,
	answer,
	receive,
	send,
	disk_read,
	disk_write,
	flush,
	memory,
	count,
};

/* The letter the slot spells a cause with; '.' for none. */
[[nodiscard]] constexpr char cause_letter(Cause cause) noexcept
{
	constexpr char letters[] = {'.', 's', 'f', 'o', 'a', 'r', 'n', 'd', 'w',
		'y', 'm'};
	static_assert(sizeof(letters) == static_cast<std::size_t>(Cause::count));
	return letters[static_cast<std::size_t>(cause)];
}

/* What one event records. Measured by the detector: the first six fields.
 * Filled by the watch from the system: the rest. */
struct StallEvent final {
	std::uint32_t sequence{};
	/* Since the record's base. */
	std::uint32_t start_ms{};
	std::uint32_t duration_ms{};
	bool open{};
	std::uint32_t mask{};
	/* Per vCPU, the cause that lasted longest and the share of a core it got
	 * meanwhile, in percent. */
	Cause causes[max_vcpus]{};
	std::uint8_t share[max_vcpus]{};
	/* The worst lateness of the main queue (-1 not serviced) and of the
	 * watch's own tick during the event. */
	std::int32_t ui_late_ms{-1};
	std::uint32_t watch_late_ms{};

	std::uint8_t thermal_start{};
	std::uint8_t thermal_end{};
	bool low_power{};
	std::uint8_t processors_start{};
	std::uint8_t processors_end{};
	/* Cores used, of them on performance cores, and watts, times 100: the
	 * five seconds before the event and the event. */
	std::uint16_t cores_before{};
	std::uint16_t performance_before{};
	std::uint16_t watts_before{};
	std::uint16_t cores_during{};
	std::uint16_t performance_during{};
	std::uint16_t watts_during{};
	/* The process's involuntary context switches during the event. */
	std::uint32_t involuntary_switches{};
	/* MiB. `left` is what the process's limit still allows, -1 for none. */
	std::uint32_t footprint_mib{};
	std::int32_t left_mib{-1};
	std::int32_t compressed_mib{};
	/* Page-ins and faults of the process, and of the system, during it. */
	std::uint32_t pageins{};
	std::uint32_t faults{};
	std::uint32_t system_pageins{};
	std::uint32_t system_decompressions{};
	/* 0 normal, 1 warning, 2 critical. */
	std::uint8_t pressure{};
};

/* A change of the host's state: the thermal state, Low Power Mode with the
 * active processor count, or the memory pressure level. */
struct Transition final {
	std::uint32_t at_ms{};
	std::uint8_t first{};
	std::uint8_t second{};
};

template<typename Entry, std::size_t Capacity>
class Ring final {
public:
	void push(const Entry &entry) noexcept
	{
		entries_[total_ % Capacity] = entry;
		++total_;
	}

	/* Entries held now, oldest first at 0. */
	[[nodiscard]] std::size_t size() const noexcept
	{
		return total_ < Capacity ? total_ : Capacity;
	}

	/* Entries ever pushed. */
	[[nodiscard]] std::uint32_t total() const noexcept { return total_; }

	[[nodiscard]] Entry &at(std::size_t index) noexcept
	{
		return entries_[(total_ - size() + index) % Capacity];
	}

	[[nodiscard]] const Entry &at(std::size_t index) const noexcept
	{
		return entries_[(total_ - size() + index) % Capacity];
	}

	[[nodiscard]] Entry &back() noexcept { return at(size() - 1); }

	void clear() noexcept { total_ = 0; }

private:
	Entry entries_[Capacity]{};
	std::uint32_t total_{};
};

/* The record of one session: all the memory it ever uses. */
struct StallRecord final {
	Ring<StallEvent, event_capacity> events;
	Ring<Transition, transition_capacity> thermal;
	Ring<Transition, transition_capacity> power;
	Ring<Transition, transition_capacity> pressure;
	/* The wall clock, in seconds, at the base of every time in the record.
	 * The times are on the continuous clock, which counts the time the
	 * system sleeps, so a time is the base plus its offset. */
	std::int64_t base_unix{};
	/* Observations of a vCPU carrier in the idle wait the loader marks, and
	 * in a wait nothing marks. A running guest shows the first, which proves
	 * the markers reach the wrappers. */
	std::uint32_t idle_samples{};
	std::uint32_t unmarked_samples{};
};

/* How the slot spells a record, a text of at most slot_bytes: the header
 *   stalls 1 base B at T total N shown M
 * then, oldest first, the newest events that fit
 *   stall SEQ START DUR[+] CAUSES SHARES UI WATCH THERMAL LPM CPUS BEFORE
 *     DURING SWITCHES FOOTPRINT LEFT COMPRESSED PAGEINS FAULTS SYSPAGEINS
 *     SYSDECOMP PRESSURE
 * (one line), and the transitions
 *   thermal AT FROM TO / power AT LOWPOWER PROCESSORS / pressure AT LEVEL.
 * Times are milliseconds after B except T, the wall clock at the write. The
 * text is padded with spaces to slot_bytes, the last of which is a line feed.
 * Returns the bytes written before the padding. */
std::size_t format_stall_slot(char *slot, std::size_t size,
	const StallRecord &record, std::int64_t now_unix) noexcept;

/* The line of one event, without its line feed, in at most max_event_line
 * bytes, and its length. */
inline constexpr std::size_t max_event_line = 256;
std::size_t format_stall_event(char *line, std::size_t size,
	const StallEvent &event) noexcept;

/* Decides from a stream of ticks when the stalls begin and end, and keeps the
 * record's events. One thread. */
class StallDetector final {
public:
	enum class Change : std::uint8_t {
		none,
		/* A new event is the record's last. */
		opened,
		/* The open event moved on. */
		updated,
		/* The last event is finished. */
		closed,
	};

	explicit StallDetector(StallRecord &record) noexcept : record_{record} {}

	/* Forgets everything but the record and starts counting time from
	 * `now_ns`. */
	void reset(std::uint64_t now_ns) noexcept;

	[[nodiscard]] Change observe(const Tick &tick) noexcept;

	[[nodiscard]] bool open() const noexcept { return open_; }
	/* Whether an event is open or a stall is being confirmed: the only time
	 * the main queue is asked how late it is. */
	[[nodiscard]] bool suspicious() const noexcept;
	/* Milliseconds since the base for a clock reading. */
	[[nodiscard]] std::uint32_t since_base_ms(std::uint64_t now_ns) const
		noexcept;

private:
	struct Track final {
		bool seen{};
		std::uint64_t cpu_ns{};
		/* The candidate stall: its cause, the wait it started in, how long it
		 * has lasted and what the carrier ran meanwhile. */
		Cause cause{};
		std::uint64_t sequence{};
		std::uint32_t pending_ms{};
		std::uint64_t pending_cpu_ns{};
		std::uint64_t pending_wall_ns{};
		/* How much of that the open event has been given. */
		std::uint32_t credited_ms{};
		std::uint64_t credited_cpu_ns{};
		std::uint64_t credited_wall_ns{};
		/* The open event's account. */
		std::uint32_t cause_ms[static_cast<std::size_t>(Cause::count)]{};
		std::uint64_t event_cpu_ns{};
		std::uint64_t event_wall_ns{};
	};

	void begin_event(const Tick &tick, std::uint64_t first_stalled_ns,
		bool continued) noexcept;
	void summarize(StallEvent &event) const noexcept;

	StallRecord &record_;
	std::uint64_t base_ns_{};
	std::uint64_t last_ns_{};
	bool started_{};
	bool open_{};
	/* The last event ended at its age limit with a stall still going. */
	bool continued_{};
	std::uint64_t event_start_ns_{};
	std::uint64_t quiet_since_ns_{};
	unsigned quiet_ticks_{};
	std::uint32_t sequence_{};
	Track tracks_[max_vcpus]{};
};

} // namespace uml::runtime::stall
