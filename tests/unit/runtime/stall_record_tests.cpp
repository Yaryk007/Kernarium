// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The stall record (runtime/stall_record): which vCPUs are stalled and why,
 * from synthetic samples with no real stall; the rings that never grow; and
 * the text of the session record's stall slot, which Andarium's parser reads
 * from the same golden file (tests/unit/andarium/fixtures/stall-slot-golden.txt).
 */
#include "stall_record.hpp"
#include "require.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using namespace uml::runtime::stall;
using uml::platform::HostWait;
using uml::testing::require;

constexpr std::uint64_t millisecond = 1'000'000;
constexpr std::uint64_t start_ns = 7'000'000'000ULL;

/* A word as a WaitCell publishes it. */
constexpr std::uint64_t word(HostWait kind, std::uint64_t sequence) noexcept
{
	return sequence << 8 | static_cast<std::uint8_t>(kind);
}

/* A run of ticks over `vcpus` carriers. A carrier's CPU time is advanced
 * by `share` percent of each interval unless a test sets it itself. */
class Scenario final {
public:
	explicit Scenario(unsigned vcpus) : vcpus_{vcpus}, detector_{record}
	{
		detector_.reset(now_);
		for (unsigned cpu = 0; cpu < vcpus; ++cpu) {
			sample(cpu, RunState::waiting, 0, word(HostWait::idle, 1));
		}
	}

	void sample(unsigned cpu, RunState state, unsigned share,
		std::uint64_t wait_word)
	{
		samples_[cpu] = {true, state, samples_[cpu].cpu_ns, wait_word};
		shares_[cpu] = share;
	}

	/* Advances `interval_ms` and observes. The watch woke late by what the
	 * interval has over a tick unless the test says its wake-up was on time
	 * (`late_ms`), as when a tick's own work took the time. */
	StallDetector::Change tick(unsigned interval_ms = tick_ms,
		bool disturbed = false, int late_ms = -1, bool rebaseline = false)
	{
		now_ += interval_ms * millisecond;
		Tick current;
		current.now_ns = now_;
		current.disturbed = disturbed;
		current.rebaseline = rebaseline;
		current.late_ms = late_ms >= 0 ? static_cast<std::uint32_t>(late_ms) :
			interval_ms > tick_ms ? interval_ms - tick_ms : 0;
		for (unsigned cpu = 0; cpu < vcpus_; ++cpu) {
			samples_[cpu].cpu_ns += static_cast<std::uint64_t>(interval_ms) *
				millisecond * shares_[cpu] / 100;
			current.vcpus[cpu] = samples_[cpu];
		}
		return detector_.observe(current);
	}

	void ticks(unsigned count)
	{
		for (unsigned index = 0; index < count; ++index)
			(void)tick();
	}

	StallRecord record;
	[[nodiscard]] const StallDetector &detector() const { return detector_; }

private:
	unsigned vcpus_;
	StallDetector detector_;
	std::uint64_t now_{start_ns};
	VcpuSample samples_[max_vcpus]{};
	unsigned shares_[max_vcpus]{};
};

void idle_and_busy_carriers_are_not_stalled()
{
	Scenario scenario{6};
	for (unsigned cpu = 0; cpu < 3; ++cpu)
		scenario.sample(cpu, RunState::running, 100, 0);
	/* A carrier that wakes from its idle wait again and again is a new wait
	 * each time, and a held one is nobody's stall. */
	scenario.sample(3, RunState::stopped, 0, word(HostWait::disk_read, 9));
	for (unsigned index = 0; index < 2000; ++index) {
		scenario.sample(4, RunState::waiting, 0,
			word(HostWait::idle, 10 + index));
		scenario.sample(5, index % 7 == 0 ? RunState::running : RunState::waiting,
			index % 7 == 0 ? 5 : 0, word(HostWait::idle, 5000 + index));
		require(scenario.tick() == StallDetector::Change::none);
	}
	require(scenario.record.events.total() == 0);
}

void a_starved_carrier_is_a_stall_after_two_ticks()
{
	Scenario scenario{6};
	scenario.sample(0, RunState::running, 100, 0);
	scenario.sample(1, RunState::running, 100, 0);
	scenario.ticks(4);
	/* Both runnable and getting 3% and 10%. */
	scenario.sample(0, RunState::running, 3, 0);
	scenario.sample(1, RunState::running, 10, 0);
	require(scenario.tick() == StallDetector::Change::none);
	require(scenario.tick() == StallDetector::Change::opened);
	require(scenario.record.events.size() == 1);
	scenario.ticks(8);
	scenario.sample(0, RunState::running, 100, 0);
	scenario.sample(1, RunState::running, 100, 0);
	require(scenario.tick() == StallDetector::Change::updated);
	require(scenario.tick() == StallDetector::Change::closed);
	require(!scenario.detector().open());

	const StallEvent &event = scenario.record.events.back();
	require(!event.open);
	require(event.mask == 0x3);
	require(event.causes[0] == Cause::starved && event.causes[1] == Cause::starved);
	require(event.causes[2] == Cause::none);
	require(event.share[0] == 3 && event.share[1] == 10);
	/* It starts at the beginning of the interval the first low-share sample
	 * covers, and ends at the first full one. */
	require(event.start_ms == 4 * tick_ms);
	require(event.duration_ms == 11 * tick_ms);
	require(event.ui_late_ms == -1);
}

void a_blocked_carrier_must_stay_in_the_same_wait()
{
	Scenario scenario{4};
	/* A disk read of 40 ms after another, 100 times: never one wait of 100. */
	for (unsigned index = 0; index < 100; ++index) {
		scenario.sample(1, RunState::waiting, 0,
			word(HostWait::disk_read, 100 + index));
		require(scenario.tick() == StallDetector::Change::none);
	}
	/* One read that stays: first seen, then 50 ms later, then 100. */
	scenario.sample(1, RunState::waiting, 0, word(HostWait::disk_read, 500));
	require(scenario.tick() == StallDetector::Change::none);
	require(scenario.tick() == StallDetector::Change::none);
	require(scenario.tick() == StallDetector::Change::opened);
	scenario.ticks(3);
	scenario.sample(1, RunState::waiting, 0, word(HostWait::idle, 501));
	require(scenario.tick() == StallDetector::Change::updated);
	require(scenario.tick() == StallDetector::Change::closed);
	const StallEvent &event = scenario.record.events.back();
	require(event.mask == 0x2 && event.causes[1] == Cause::disk_read);
	require(event.duration_ms >= 250 && event.duration_ms <= 300);
}

void every_wait_kind_has_a_cause_and_an_unmarked_wait_is_other()
{
	struct Case {
		HostWait kind;
		Cause cause;
		RunState state;
	};
	const Case cases[] = {
		{HostWait::answer, Cause::answer, RunState::waiting},
		{HostWait::receive, Cause::receive, RunState::waiting},
		{HostWait::send, Cause::send, RunState::waiting},
		{HostWait::disk_read, Cause::disk_read, RunState::waiting},
		{HostWait::disk_write, Cause::disk_write, RunState::waiting},
		{HostWait::flush, Cause::flush, RunState::waiting},
		{HostWait::memory, Cause::memory, RunState::waiting},
		{HostWait::none, Cause::other, RunState::waiting},
		{HostWait::none, Cause::fault, RunState::uninterruptible},
	};
	for (const Case &entry : cases) {
		Scenario scenario{2};
		scenario.sample(1, entry.state, 0, word(entry.kind, 77));
		scenario.ticks(3);
		require(scenario.record.events.size() == 1);
		const StallEvent &event = scenario.record.events.back();
		require(event.open && event.mask == 0x2 &&
			event.causes[1] == entry.cause);
	}
}

void a_late_tick_is_a_stall_of_the_process_and_a_long_gap_is_not()
{
	/* A gap of a second with the carriers idle: no carrier shows it, the
	 * watch itself was late. */
	Scenario late{6};
	late.ticks(5);
	require(late.tick(1000) == StallDetector::Change::opened);
	require(late.tick() == StallDetector::Change::updated);
	require(late.tick() == StallDetector::Change::closed);
	const StallEvent &process = late.record.events.back();
	require(process.mask == 0 && process.watch_late_ms == 950);
	/* From the wake-up the watch missed to the one it made. */
	require(process.start_ms == 6 * tick_ms);
	require(process.duration_ms >= 950 && process.duration_ms <= 1100);

	/* The same gap with a runnable carrier is a starved carrier at once. */
	Scenario starved{6};
	starved.sample(2, RunState::running, 100, 0);
	starved.ticks(5);
	starved.sample(2, RunState::running, 2, 0);
	require(starved.tick(1000) == StallDetector::Change::opened);
	require(starved.record.events.back().mask == 0x4);
	starved.ticks(3);
	require(starved.record.events.back().causes[2] == Cause::starved);

	/* Seconds of nothing at all is the process suspended. */
	Scenario suspended{6};
	suspended.sample(2, RunState::running, 100, 0);
	suspended.ticks(5);
	suspended.sample(2, RunState::running, 0, 0);
	require(suspended.tick(6000) == StallDetector::Change::none);
	suspended.sample(2, RunState::running, 100, 0);
	suspended.ticks(5);
	require(suspended.record.events.total() == 0);
}

/* A watch that slept while every carrier was idle (stall_watch.hpp) wakes
 * 50 ms after one left its idle wait. The sleep is not an interval to measure:
 * a carrier that ran since used a sliver of it. */
void a_watch_that_slept_sees_every_carrier_as_for_the_first_time()
{
	/* Without the flag, a woken carrier that used 1% of a 2 s gap is starved at
	 * once; this is what the flag is for. */
	Scenario unflagged{3};
	unflagged.ticks(4);
	unflagged.sample(1, RunState::running, 1, 0);
	require(unflagged.tick(2000, false, 0) == StallDetector::Change::opened);

	/* With it, nothing is judged by the sleep, and the carrier is measured from
	 * the next look: 50 ms starved, then 100 ms is a stall. */
	Scenario slept{3};
	slept.ticks(4);
	slept.sample(1, RunState::running, 1, 0);
	require(slept.tick(2000, false, 0, true) == StallDetector::Change::none);
	require(slept.record.events.total() == 0 && !slept.detector().suspicious());
	require(slept.tick() == StallDetector::Change::none);
	require(slept.detector().suspicious());
	require(slept.tick() == StallDetector::Change::opened);
	require(slept.record.events.back().causes[1] == Cause::starved);

	/* A carrier found in a wait that does not end is counted from that look,
	 * as one found so at the watch's start is, and is an event 100 ms on. */
	Scenario wedged{3};
	wedged.ticks(4);
	wedged.sample(2, RunState::waiting, 0, word(HostWait::answer, 7));
	require(wedged.tick(2000, false, 0, true) == StallDetector::Change::none);
	require(wedged.detector().suspicious());
	require(wedged.tick() == StallDetector::Change::none);
	require(wedged.tick() == StallDetector::Change::opened);
	require(wedged.record.events.back().causes[2] == Cause::answer);

	/* The look counts the carriers' idle waits, and a carrier that is busy is
	 * nothing to it. */
	Scenario counted{3};
	counted.sample(1, RunState::running, 100, 0);
	const std::uint32_t idle_before = counted.record.idle_samples;
	require(counted.tick(2000, false, 0, true) == StallDetector::Change::none);
	require(counted.record.idle_samples == idle_before + 2);
	counted.ticks(8);
	require(counted.record.events.total() == 0);

	/* An event cannot be open across a sleep; if one is, it ends there. */
	Scenario open{3};
	open.sample(1, RunState::waiting, 0, word(HostWait::send, 3));
	open.ticks(4);
	require(open.detector().open());
	require(open.tick(2000, false, 0, true) == StallDetector::Change::closed);
	require(!open.detector().open() && !open.record.events.back().open);
}

void a_hold_ends_an_event_and_restarts_the_measurement()
{
	Scenario scenario{3};
	scenario.sample(1, RunState::waiting, 0, word(HostWait::answer, 3));
	scenario.ticks(4);
	require(scenario.detector().open());
	require(scenario.tick(tick_ms, true) == StallDetector::Change::closed);
	require(!scenario.record.events.back().open);
	/* The wait carries on; the measurement starts again and needs its own
	 * 100 ms before it is an event. */
	require(scenario.tick() == StallDetector::Change::none);
	require(scenario.tick() == StallDetector::Change::none);
	require(scenario.tick() == StallDetector::Change::opened);
	require(scenario.record.events.total() == 2);
}

void one_event_holds_every_carrier_stalled_meanwhile_and_two_quiet_ticks_end_it()
{
	Scenario scenario{4};
	scenario.sample(1, RunState::waiting, 0, word(HostWait::send, 1));
	scenario.ticks(2);
	scenario.sample(2, RunState::waiting, 0, word(HostWait::disk_write, 1));
	scenario.ticks(2);
	require(scenario.record.events.total() == 1);
	/* The first carrier recovers and the second stays: still one event. */
	scenario.sample(1, RunState::waiting, 0, word(HostWait::idle, 2));
	scenario.ticks(5);
	require(scenario.record.events.total() == 1 && scenario.detector().open());
	scenario.sample(2, RunState::waiting, 0, word(HostWait::idle, 2));
	require(scenario.tick() == StallDetector::Change::updated);
	require(scenario.tick() == StallDetector::Change::closed);
	const StallEvent &event = scenario.record.events.back();
	require(event.mask == 0x6 && event.causes[1] == Cause::send &&
		event.causes[2] == Cause::disk_write);
	/* A new stall after the end is a new event. */
	scenario.sample(3, RunState::waiting, 0, word(HostWait::flush, 9));
	scenario.ticks(3);
	require(scenario.record.events.total() == 2);
}

void an_event_ends_at_its_age_limit_and_the_next_starts_from_there()
{
	Scenario scenario{2};
	scenario.sample(0, RunState::waiting, 0, word(HostWait::flush, 1));
	unsigned closed = 0;
	for (unsigned index = 0; index < (max_event_ms / tick_ms) * 3 / 2; ++index)
		if (scenario.tick() == StallDetector::Change::closed)
			++closed;
	require(closed == 1);
	require(scenario.record.events.total() == 2);
	const StallEvent &first = scenario.record.events.at(0);
	require(first.duration_ms >= max_event_ms - tick_ms &&
		first.duration_ms <= max_event_ms + tick_ms);
	const StallEvent &second = scenario.record.events.at(1);
	require(second.start_ms >= first.start_ms + first.duration_ms - tick_ms);
	/* Not the whole stall again. */
	require(second.duration_ms < max_event_ms + tick_ms * 20);
}

void a_carrier_that_ran_is_not_held_in_the_waits_it_was_seen_in()
{
	/* A busy carrier that every sample finds in a wait: an unmarked one, an
	 * uninterruptible one, or one marked wait that the sequence says is the
	 * same. It used 85% of each interval, so it was not held. */
	struct Case {
		RunState state;
		std::uint64_t wait_word;
	};
	const Case cases[] = {
		{RunState::waiting, 0},
		{RunState::uninterruptible, 0},
		{RunState::waiting, word(HostWait::disk_read, 41)},
	};
	for (const Case &entry : cases) {
		Scenario scenario{3};
		scenario.sample(1, entry.state, 85, entry.wait_word);
		for (unsigned index = 0; index < 400; ++index)
			require(scenario.tick() == StallDetector::Change::none);
		require(scenario.record.events.total() == 0);
		/* Seen at 85% of one interval and at nothing in the next, the wait
		 * began in between. The interval it began in is not counted, so the
		 * stall counts from the next sample. */
		scenario.sample(1, entry.state, 0, entry.wait_word);
		scenario.ticks(4);
		require(scenario.record.events.total() == 1);
		require(scenario.record.events.back().share[1] == 0);
	}
	/* A carrier in a wait that is mostly idle but runs a few percent is held. */
	Scenario held{2};
	held.sample(1, RunState::waiting, 15, word(HostWait::answer, 5));
	held.ticks(4);
	require(held.record.events.total() == 1 &&
		held.record.events.back().share[1] == 15);
}

void a_slow_tick_is_not_a_late_one()
{
	/* The watch's own work took 150 ms between two waits, and its wake-ups
	 * were on time: nobody stopped the process. */
	Scenario scenario{4};
	scenario.ticks(5);
	for (unsigned index = 0; index < 10; ++index)
		require(scenario.tick(200, false, 0) == StallDetector::Change::none);
	require(scenario.record.events.total() == 0);
	/* The same interval with a wake-up that was 150 ms late is. */
	require(scenario.tick(200, false, 150) == StallDetector::Change::opened);
	require(scenario.record.events.back().mask == 0 &&
		scenario.record.events.back().watch_late_ms == 150);
}

void the_times_run_through_a_sleep_of_the_system()
{
	/* The clock the watch feeds counts the time the system sleeps. An hour
	 * of sleep is a gap, not an event, and the next event's time is its time
	 * since the base, the hour included, so base plus offset is the wall
	 * clock. */
	Scenario scenario{3};
	scenario.ticks(5);
	require(scenario.tick(3'600'000) == StallDetector::Change::none);
	scenario.sample(1, RunState::waiting, 0, word(HostWait::answer, 3));
	scenario.ticks(3);
	require(scenario.record.events.total() == 1);
	const StallEvent &event = scenario.record.events.back();
	const std::uint32_t slept = 3'600'000;
	require(event.start_ms >= slept && event.start_ms <= slept + 8 * tick_ms);
	/* The slot says the same: the base, and a time that includes the hour. */
	char slot[slot_bytes];
	scenario.record.base_unix = 1'794'114'300;
	(void)format_stall_slot(slot, sizeof(slot), scenario.record, 1'794'118'000);
	require(std::string_view{slot, sizeof(slot)}.find("base 1794114300") !=
		std::string_view::npos);
}

void the_markers_are_counted_and_a_hold_is_not()
{
	Scenario scenario{3};
	scenario.sample(1, RunState::waiting, 0, 0);
	scenario.ticks(2);
	/* Three carriers in a marked idle wait, one of them unmarked, for two
	 * ticks. */
	require(scenario.record.idle_samples == 4 /* carriers 0 and 2 */);
	require(scenario.record.unmarked_samples == 2 /* carrier 1 */);
	/* A held tick says nothing about the waits. */
	scenario.sample(2, RunState::running, 100, 0);
	(void)scenario.tick(tick_ms, true);
	require(scenario.record.idle_samples == 4 && scenario.record.unmarked_samples == 2);
	(void)scenario.tick();
	require(scenario.record.idle_samples == 5 && scenario.record.unmarked_samples == 3);
}

void the_main_queue_is_asked_only_while_something_is_suspicious()
{
	Scenario scenario{3};
	scenario.ticks(20);
	require(!scenario.detector().suspicious());
	scenario.sample(1, RunState::waiting, 0, word(HostWait::send, 4));
	require(scenario.tick() == StallDetector::Change::none);
	/* The first sight of a stall being confirmed. */
	require(scenario.detector().suspicious());
	require(scenario.tick() == StallDetector::Change::none);
	require(scenario.tick() == StallDetector::Change::opened);
	require(scenario.detector().suspicious());
	scenario.sample(1, RunState::waiting, 0, word(HostWait::idle, 5));
	scenario.ticks(2);
	require(!scenario.detector().open() && !scenario.detector().suspicious());
}

void the_rings_keep_their_size()
{
	Scenario scenario{2};
	for (unsigned event = 0; event < event_capacity * 3; ++event) {
		scenario.sample(1, RunState::waiting, 0,
			word(HostWait::answer, 1000 + event * 10));
		scenario.ticks(4);
		scenario.sample(1, RunState::waiting, 0,
			word(HostWait::idle, 1001 + event * 10));
		scenario.ticks(3);
	}
	require(scenario.record.events.total() == event_capacity * 3);
	require(scenario.record.events.size() == event_capacity);
	require(scenario.record.events.at(0).sequence == event_capacity * 2 + 1);
	require(scenario.record.events.back().sequence == event_capacity * 3);

	Ring<Transition, transition_capacity> ring;
	for (std::uint32_t index = 0; index < 100; ++index)
		ring.push({index, 0, 0});
	require(ring.size() == transition_capacity && ring.total() == 100);
	require(ring.at(0).at_ms == 100 - transition_capacity);
	require(ring.back().at_ms == 99);
}

/* The worst an event can say: every figure at its largest, every vCPU. */
StallEvent largest_event(std::uint32_t sequence)
{
	StallEvent event;
	event.sequence = 4'000'000'000U + sequence;
	event.start_ms = 4'000'000'000U;
	event.duration_ms = 4'000'000'000U;
	event.open = true;
	for (unsigned cpu = 0; cpu < max_vcpus; ++cpu) {
		event.causes[cpu] = Cause::memory;
		event.share[cpu] = 255;
	}
	event.mask = 0xffff;
	event.ui_late_ms = 0x7fffffff;
	event.watch_late_ms = 0xffffffffU;
	event.thermal_start = event.thermal_end = 255;
	event.low_power = true;
	event.processors_start = event.processors_end = 255;
	event.cores_before = event.performance_before = event.watts_before = 65535;
	event.cores_during = event.performance_during = event.watts_during = 65535;
	event.involuntary_switches = 0xffffffffU;
	event.footprint_mib = 0xffffffffU;
	event.left_mib = 0x7fffffff;
	event.compressed_mib = -0x7fffffff;
	event.pageins = event.faults = 0xffffffffU;
	event.system_pageins = event.system_decompressions = 0xffffffffU;
	event.pressure = 255;
	return event;
}

StallRecord largest_record()
{
	StallRecord record;
	record.base_unix = 9'999'999'999;
	record.idle_samples = 0xffffffffU;
	record.unmarked_samples = 0xffffffffU;
	for (std::uint32_t index = 0; index < event_capacity * 4; ++index)
		record.events.push(largest_event(index));
	for (std::uint32_t index = 0; index < transition_capacity * 4; ++index) {
		const Transition change{4'000'000'000U, 255, 255};
		record.thermal.push(change);
		record.power.push(change);
		record.pressure.push(change);
	}
	return record;
}

bool is_text(std::string_view slot)
{
	for (const char character : slot)
		if (character != '\n' && (character < ' ' || character >= 0x7f))
			return false;
	return true;
}

void the_slot_never_exceeds_its_size_whatever_the_figures()
{
	/* No figure, however large, makes a line longer than the bound. */
	char line[max_event_line * 2];
	const std::size_t longest = format_stall_event(line, sizeof(line),
		largest_event(0));
	require(longest <= max_event_line);

	const StallRecord record = largest_record();
	std::string slot(slot_bytes + 64, '#');
	const std::size_t used = format_stall_slot(slot.data(), slot_bytes, record,
		9'999'999'999);
	require(used < slot_bytes);
	/* Past the slot nothing was written. */
	require(slot.find_first_not_of('#', slot_bytes) == std::string::npos);
	require(slot[slot_bytes - 1] == '\n');
	require(is_text({slot.data(), slot_bytes}));
	/* The padding is spaces up to the last byte. */
	for (std::size_t index = used; index + 1 < slot_bytes; ++index)
		require(slot[index] == ' ');
	/* Whole lines only: the slot ends a line, and the events it shows are the
	 * newest. */
	std::size_t shown = 0;
	for (std::size_t at = slot.find("\nstall ");
	     at != std::string::npos && at < slot_bytes;
	     at = slot.find("\nstall ", at + 1))
		++shown;
	require(shown >= 10 && shown < event_capacity);
	char header[160];
	std::snprintf(header, sizeof(header), "shown %zu idle ", shown);
	require(slot.find(header) != std::string::npos);
	require(slot.find("total 128 shown") != std::string::npos);

	/* An empty record is a header. */
	StallRecord empty;
	empty.base_unix = 5;
	std::string blank(slot_bytes, '#');
	const std::size_t blank_used = format_stall_slot(blank.data(), slot_bytes,
		empty, 6);
	require(std::string_view{blank.data(), blank_used} ==
		"stalls 1 base 5 at 6 total 0 shown 0 idle 0 unmarked 0\n");
}

/* Fixed figures every platform makes the same: the golden slot. */
StallRecord golden_record()
{
	StallRecord record;
	record.base_unix = 1'794'114'300;
	record.idle_samples = 123'456;
	record.unmarked_samples = 789;
	/* More events than the ring holds, so that the total says so. */
	for (std::uint32_t index = 1; index <= event_capacity + 3; ++index) {
		StallEvent event;
		event.sequence = index;
		event.start_ms = 1000 * index;
		event.duration_ms = 100 + index;
		event.causes[0] = Cause::starved;
		event.share[0] = 4;
		event.mask = 1;
		record.events.push(event);
	}
	{
		StallEvent event;
		event.sequence = event_capacity + 4;
		event.start_ms = 3'988'180;
		event.duration_ms = 962;
		event.mask = 0x3f;
		const Cause causes[6] = {Cause::starved, Cause::starved, Cause::starved,
			Cause::starved, Cause::starved, Cause::starved};
		const std::uint8_t shares[6] = {3, 5, 0, 0, 2, 1};
		for (unsigned cpu = 0; cpu < 6; ++cpu) {
			event.causes[cpu] = causes[cpu];
			event.share[cpu] = shares[cpu];
		}
		event.ui_late_ms = 940;
		event.watch_late_ms = 955;
		event.thermal_start = 1;
		event.thermal_end = 2;
		event.low_power = false;
		event.processors_start = 6;
		event.processors_end = 6;
		event.cores_before = 587;
		event.performance_before = 198;
		event.watts_before = 344;
		event.cores_during = 212;
		event.performance_during = 198;
		event.watts_during = 255;
		event.involuntary_switches = 12;
		event.footprint_mib = 5752;
		event.left_mib = 380;
		event.compressed_mib = 12;
		event.pageins = 41;
		event.faults = 18000;
		event.system_pageins = 90;
		event.system_decompressions = 3300;
		event.pressure = 1;
		record.events.push(event);
	}
	{
		StallEvent event;
		event.sequence = event_capacity + 5;
		event.start_ms = 4'120'000;
		event.duration_ms = 5230;
		event.open = true;
		event.mask = 0x14;
		event.causes[2] = Cause::disk_write;
		event.share[2] = 0;
		event.causes[4] = Cause::answer;
		event.share[4] = 0;
		event.thermal_start = 2;
		event.thermal_end = 2;
		event.low_power = true;
		event.processors_start = 6;
		event.processors_end = 4;
		event.footprint_mib = 5801;
		event.left_mib = -1;
		event.compressed_mib = -20;
		record.events.push(event);
	}
	record.thermal.push({1200, 0, 1});
	record.thermal.push({3'990'000, 1, 2});
	record.power.push({50'000, 1, 6});
	record.power.push({4'121'000, 1, 4});
	record.pressure.push({2'000'000, 1, 0});
	record.pressure.push({4'100'000, 0, 1});
	return record;
}

std::string golden_text()
{
	char slot[slot_bytes];
	const StallRecord record = golden_record();
	const std::size_t used = format_stall_slot(slot, sizeof(slot), record,
		1'794'118'430);
	return {slot, used};
}

void the_slot_matches_the_golden_file(const char *path)
{
	const std::string text = golden_text();
	std::ifstream file{path};
	require(file.is_open(), "the golden file opens");
	const std::string golden{std::istreambuf_iterator<char>{file},
		std::istreambuf_iterator<char>{}};
	if (golden != text) {
		std::fprintf(stderr, "golden:\n%s\nslot:\n%s\n", golden.c_str(),
			text.c_str());
		require(false, "the slot differs from the golden file");
	}
	/* The ring keeps its newest 32 of the 37 and says how many there were. */
	require(text.find("total 37 shown 32 idle ") != std::string::npos);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--print-golden") == 0) {
		std::fputs(golden_text().c_str(), stdout);
		return 0;
	}
	idle_and_busy_carriers_are_not_stalled();
	a_starved_carrier_is_a_stall_after_two_ticks();
	a_blocked_carrier_must_stay_in_the_same_wait();
	every_wait_kind_has_a_cause_and_an_unmarked_wait_is_other();
	a_late_tick_is_a_stall_of_the_process_and_a_long_gap_is_not();
	a_carrier_that_ran_is_not_held_in_the_waits_it_was_seen_in();
	a_slow_tick_is_not_a_late_one();
	the_times_run_through_a_sleep_of_the_system();
	the_markers_are_counted_and_a_hold_is_not();
	the_main_queue_is_asked_only_while_something_is_suspicious();
	a_hold_ends_an_event_and_restarts_the_measurement();
	a_watch_that_slept_sees_every_carrier_as_for_the_first_time();
	one_event_holds_every_carrier_stalled_meanwhile_and_two_quiet_ticks_end_it();
	an_event_ends_at_its_age_limit_and_the_next_starts_from_there();
	the_rings_keep_their_size();
	the_slot_never_exceeds_its_size_whatever_the_figures();
	if (argc == 2)
		the_slot_matches_the_golden_file(argv[1]);
	static_assert(sizeof(StallRecord) <= 16 * 1024,
		"the record is a few KiB, whatever the session does");
	std::puts("stall record tests passed");
	return 0;
}
