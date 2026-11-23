// SPDX-License-Identifier: GPL-2.0-or-later
#include "stall_record.hpp"

#include "fixed_text.hpp"

#include <algorithm>
#include <cstring>

namespace uml::runtime::stall {
namespace {

constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000;

using ::uml::platform::HostWait;

template<typename Value>
constexpr std::int64_t clamped(Value value, std::int64_t maximum) noexcept
{
	return std::min<std::int64_t>(static_cast<std::int64_t>(value), maximum);
}

/* The cause a wait of this kind makes. */
constexpr Cause blocked_cause(HostWait kind) noexcept
{
	switch (kind) {
	case HostWait::answer:
		return Cause::answer;
	case HostWait::receive:
		return Cause::receive;
	case HostWait::send:
		return Cause::send;
	case HostWait::disk_read:
		return Cause::disk_read;
	case HostWait::disk_write:
		return Cause::disk_write;
	case HostWait::flush:
		return Cause::flush;
	case HostWait::memory:
		return Cause::memory;
	case HostWait::none:
	case HostWait::idle:
	case HostWait::count:
		break;
	}
	return Cause::other;
}

/* What one sample says about a carrier over the `wall_ns` it covers, with
 * `share` the percent of that time the carrier ran. */
constexpr Cause sample_cause(const VcpuSample &sample, unsigned share) noexcept
{
	switch (sample.state) {
	case RunState::stopped:
	case RunState::halted:
		/* Held by a fence, or ended. */
		return Cause::none;
	case RunState::uninterruptible:
		return Cause::fault;
	case RunState::waiting: {
		const HostWait kind = ::uml::platform::wait_kind(sample.wait_word);
		return kind == HostWait::idle ? Cause::none : blocked_cause(kind);
	}
	case RunState::running:
		return share < starved_below_percent ? Cause::starved : Cause::none;
	}
	return Cause::none;
}

constexpr bool measured_over_the_interval(Cause cause) noexcept
{
	/* A starved carrier's share covers the whole interval; the other
	 * causes are a state seen at an instant. */
	return cause == Cause::starved;
}

FixedText &field(FixedText &text, std::int64_t value) noexcept
{
	return text.text(" ").number(value);
}

FixedText &transitions(FixedText &text, std::string_view name,
	const Ring<Transition, transition_capacity> &ring, bool two) noexcept
{
	for (std::size_t index = 0; index < ring.size(); ++index) {
		const Transition &entry = ring.at(index);
		text.text(name).text(" ").number(entry.at_ms)
			.text(" ").number(entry.first);
		if (two)
			text.text(" ").number(entry.second);
		text.text("\n");
	}
	return text;
}

} // namespace

std::size_t format_stall_event(char *line, std::size_t size,
	const StallEvent &event) noexcept
{
	FixedText text{line, size};
	text.text("stall").text(" ").number(clamped(event.sequence, 999'999));
	field(text, clamped(event.start_ms, 999'999'999));
	field(text, clamped(event.duration_ms, 999'999));
	if (event.open)
		text.text("+");
	/* The causes, up to the last vCPU that has one; then the shares of those
	 * vCPUs, two digits each. */
	text.text(" ");
	std::size_t spelled = 1;
	for (std::size_t cpu = 0; cpu < max_vcpus; ++cpu)
		if (event.causes[cpu] != Cause::none)
			spelled = cpu + 1;
	for (std::size_t cpu = 0; cpu < spelled; ++cpu) {
		const char letter = cause_letter(event.causes[cpu]);
		text.text({&letter, 1});
	}
	text.text(" ");
	bool any = false;
	for (std::size_t cpu = 0; cpu < max_vcpus; ++cpu) {
		if (event.causes[cpu] == Cause::none)
			continue;
		const std::int64_t share = std::min<std::int64_t>(event.share[cpu], 99);
		if (share < 10)
			text.text("0");
		text.number(share);
		any = true;
	}
	if (!any)
		text.text("-");
	if (event.ui_late_ms < 0)
		text.text(" -");
	else
		field(text, clamped(event.ui_late_ms, 99'999));
	field(text, clamped(event.watch_late_ms, 99'999));
	text.text(" ").number(std::min<unsigned>(event.thermal_start, 9))
		.number(std::min<unsigned>(event.thermal_end, 9));
	field(text, event.low_power ? 1 : 0);
	field(text, std::min<unsigned>(event.processors_start, 99));
	text.text(">").number(std::min<unsigned>(event.processors_end, 99));
	text.text(" ").number(clamped(event.cores_before, 9999))
		.text(",").number(clamped(event.performance_before, 9999))
		.text(",").number(clamped(event.watts_before, 9999));
	text.text(" ").number(clamped(event.cores_during, 9999))
		.text(",").number(clamped(event.performance_during, 9999))
		.text(",").number(clamped(event.watts_during, 9999));
	field(text, clamped(event.involuntary_switches, 9'999'999));
	field(text, clamped(event.footprint_mib, 99'999));
	if (event.left_mib < 0)
		text.text(" -");
	else
		field(text, clamped(event.left_mib, 99'999));
	field(text, std::clamp<std::int64_t>(event.compressed_mib, -99'999, 99'999));
	field(text, clamped(event.pageins, 9'999'999));
	field(text, clamped(event.faults, 99'999'999));
	field(text, clamped(event.system_pageins, 99'999'999));
	field(text, clamped(event.system_decompressions, 99'999'999));
	field(text, std::min<unsigned>(event.pressure, 9));
	return text.size();
}

std::size_t format_stall_slot(char *slot, std::size_t size,
	const StallRecord &record, std::int64_t now_unix) noexcept
{
	if (size < 2)
		return 0;
	/* The transitions first: they are small and always shown. */
	char changes[1024];
	FixedText changed{changes, sizeof(changes)};
	transitions(changed, "thermal", record.thermal, true);
	transitions(changed, "power", record.power, true);
	transitions(changed, "pressure", record.pressure, false);

	constexpr std::size_t header_reserve = 128;
	std::size_t remaining = size - 1;
	remaining -= std::min(remaining, header_reserve + changed.size());
	std::size_t shown = 0;
	char line[max_event_line];
	for (std::size_t newest = record.events.size(); newest != 0; --newest) {
		const std::size_t length = format_stall_event(line, sizeof(line),
			record.events.at(newest - 1));
		if (length + 1 > remaining)
			break;
		remaining -= length + 1;
		++shown;
	}

	FixedText text{slot, size - 1};
	text.text("stalls 1 base ").number(record.base_unix)
		.text(" at ").number(now_unix)
		.text(" total ").number(record.events.total())
		.text(" shown ").number(static_cast<std::int64_t>(shown))
		.text(" idle ").number(record.idle_samples)
		.text(" unmarked ").number(record.unmarked_samples)
		.text("\n");
	text.text({changes, changed.size()});
	for (std::size_t index = record.events.size() - shown;
	     index < record.events.size(); ++index) {
		const std::size_t length = format_stall_event(line, sizeof(line),
			record.events.at(index));
		text.text({line, length}).text("\n");
	}
	const std::size_t used = text.size();
	std::memset(slot + used, ' ', size - 1 - used);
	slot[size - 1] = '\n';
	return used;
}

void StallDetector::reset(std::uint64_t now_ns) noexcept
{
	for (Track &track : tracks_)
		track = {};
	base_ns_ = now_ns;
	last_ns_ = now_ns;
	started_ = true;
	open_ = false;
	quiet_ticks_ = 0;
}

bool StallDetector::suspicious() const noexcept
{
	if (open_)
		return true;
	for (const Track &track : tracks_)
		if (track.cause != Cause::none)
			return true;
	return false;
}

std::uint32_t StallDetector::since_base_ms(std::uint64_t now_ns) const noexcept
{
	const std::uint64_t since = now_ns > base_ns_ ? now_ns - base_ns_ : 0;
	return static_cast<std::uint32_t>(std::min<std::uint64_t>(
		since / nanoseconds_per_millisecond, 0xffffffffU));
}

void StallDetector::summarize(StallEvent &event) const noexcept
{
	event.mask = 0;
	for (std::size_t cpu = 0; cpu < max_vcpus; ++cpu) {
		const Track &track = tracks_[cpu];
		Cause worst = Cause::none;
		std::uint32_t longest = 0;
		for (std::size_t cause = 1;
		     cause < static_cast<std::size_t>(Cause::count); ++cause) {
			if (track.cause_ms[cause] > longest) {
				longest = track.cause_ms[cause];
				worst = static_cast<Cause>(cause);
			}
		}
		event.causes[cpu] = worst;
		event.share[cpu] = 0;
		if (worst == Cause::none)
			continue;
		event.mask |= 1U << cpu;
		if (track.event_wall_ns != 0)
			event.share[cpu] = static_cast<std::uint8_t>(std::min<std::uint64_t>(
				track.event_cpu_ns * 100 / track.event_wall_ns, 100));
	}
}

void StallDetector::begin_event(const Tick &tick,
	std::uint64_t first_stalled_ns, bool continued) noexcept
{
	for (Track &track : tracks_) {
		std::fill(std::begin(track.cause_ms), std::end(track.cause_ms), 0U);
		track.event_cpu_ns = 0;
		track.event_wall_ns = 0;
		/* A stall that goes on from the last event is the new event's from
		 * here only. */
		track.credited_ms = continued ? track.pending_ms : 0;
		track.credited_cpu_ns = continued ? track.pending_cpu_ns : 0;
		track.credited_wall_ns = continued ? track.pending_wall_ns : 0;
	}
	StallEvent event;
	event.sequence = ++sequence_;
	event_start_ns_ = continued ? tick.now_ns :
		std::max(first_stalled_ns, base_ns_);
	event.start_ms = since_base_ms(event_start_ns_);
	event.open = true;
	event.ui_late_ms = tick.ui_late_ms;
	record_.events.push(event);
	open_ = true;
	quiet_ticks_ = 0;
}

StallDetector::Change StallDetector::observe(const Tick &tick) noexcept
{
	if (!started_)
		reset(tick.now_ns);
	const std::uint64_t previous_ns = last_ns_;
	const std::uint64_t wall_ns = tick.now_ns > previous_ns ?
		tick.now_ns - previous_ns : 0;
	last_ns_ = tick.now_ns;
	const std::uint64_t wall_ms = wall_ns / nanoseconds_per_millisecond;
	const auto interval_ms = static_cast<std::uint32_t>(
		std::min<std::uint64_t>(wall_ms, 0xffffU * 1000U));

	/* Threads that were held, or a process that was not running for
	 * seconds, say nothing about the host: the counters start again and an
	 * event in progress ends where it was last seen. A watch that slept
	 * starts again the same way, and goes on to read the carriers at once, as
	 * it does at its start: none of them has an interval to judge. */
	if (tick.disturbed || tick.rebaseline || wall_ms >= suspension_gap_ms) {
		Change change = Change::none;
		if (open_) {
			StallEvent &event = record_.events.back();
			summarize(event);
			event.duration_ms = since_base_ms(previous_ns) - event.start_ms;
			event.open = false;
			open_ = false;
			change = Change::closed;
		}
		for (Track &track : tracks_)
			track = {};
		continued_ = false;
		if (!tick.rebaseline || tick.disturbed || change != Change::none)
			return change;
	}

	const std::uint32_t late_ms = tick.late_ms;
	bool any_stalled = false;
	std::uint64_t first_stalled_ns = tick.now_ns;
	for (std::size_t cpu = 0; cpu < max_vcpus; ++cpu) {
		Track &track = tracks_[cpu];
		const VcpuSample &sample = tick.vcpus[cpu];
		if (!sample.present) {
			track = {};
			continue;
		}
		/* A carrier seen for the first time, or whose time went backwards
		 * (a new guest), has no interval to judge. */
		const bool measured = track.seen && sample.cpu_ns >= track.cpu_ns &&
			wall_ns != 0;
		const std::uint64_t used_ns = measured ? sample.cpu_ns - track.cpu_ns : 0;
		const unsigned share = measured ?
			static_cast<unsigned>(std::min<std::uint64_t>(
				used_ns * 100 / wall_ns, 100)) : 100;
		track.seen = true;
		track.cpu_ns = sample.cpu_ns;

		if (sample.state == RunState::waiting) {
			const HostWait kind = ::uml::platform::wait_kind(sample.wait_word);
			if (kind == HostWait::idle)
				++record_.idle_samples;
			else if (kind == HostWait::none)
				++record_.unmarked_samples;
		}
		Cause cause = sample_cause(sample, share);
		/* A carrier that ran between two samples was not held in the wait
		 * they both found it in: it counts toward a stall of a wait only
		 * when it used little of the interval. The interval a busy carrier
		 * began to block in still shows its busy share, so the wait counts
		 * from the next one: a wait shorter than about 200 ms is not
		 * recorded, and a longer one is recorded 50 ms late and short. */
		if (cause != Cause::none && !measured_over_the_interval(cause) &&
		    measured && share >= starved_below_percent)
			cause = Cause::none;
		const std::uint64_t sequence =
			::uml::platform::wait_sequence(sample.wait_word);
		if (cause == Cause::none) {
			track.cause = Cause::none;
			track.pending_ms = 0;
			track.pending_cpu_ns = 0;
			track.pending_wall_ns = 0;
			track.credited_ms = 0;
			track.credited_cpu_ns = 0;
			track.credited_wall_ns = 0;
		} else {
			const bool same = track.cause == cause && track.sequence == sequence;
			if (!same) {
				track.cause = cause;
				track.sequence = sequence;
				track.pending_ms = 0;
				track.pending_cpu_ns = 0;
				track.pending_wall_ns = 0;
				track.credited_ms = 0;
				track.credited_cpu_ns = 0;
				track.credited_wall_ns = 0;
			}
			if (same || measured_over_the_interval(cause)) {
				track.pending_ms += interval_ms;
				track.pending_cpu_ns += used_ns;
				track.pending_wall_ns += wall_ns;
			}
		}
		if (track.cause == Cause::none ||
		    track.pending_ms < stall_threshold_ms)
			continue;
		any_stalled = true;
		const std::uint64_t pending_ns =
			static_cast<std::uint64_t>(track.pending_ms) *
			nanoseconds_per_millisecond;
		first_stalled_ns = std::min(first_stalled_ns,
			tick.now_ns > pending_ns ? tick.now_ns - pending_ns : 0);
	}

	const bool process_late = late_ms >= process_late_ms;
	Change change = Change::updated;
	if (!open_) {
		if (!any_stalled && !process_late) {
			continued_ = false;
			return Change::none;
		}
		/* A watch that woke late was away from its deadline on. */
		const std::uint64_t late_ns = std::min<std::uint64_t>(
			static_cast<std::uint64_t>(late_ms) * nanoseconds_per_millisecond,
			tick.now_ns);
		begin_event(tick, any_stalled ? first_stalled_ns :
			tick.now_ns - late_ns, continued_);
		continued_ = false;
		change = Change::opened;
	}

	StallEvent &event = record_.events.back();
	for (Track &track : tracks_) {
		if (track.cause == Cause::none ||
		    track.pending_ms < stall_threshold_ms)
			continue;
		track.cause_ms[static_cast<std::size_t>(track.cause)] +=
			track.pending_ms - track.credited_ms;
		track.event_cpu_ns += track.pending_cpu_ns - track.credited_cpu_ns;
		track.event_wall_ns += track.pending_wall_ns - track.credited_wall_ns;
		track.credited_ms = track.pending_ms;
		track.credited_cpu_ns = track.pending_cpu_ns;
		track.credited_wall_ns = track.pending_wall_ns;
	}
	event.watch_late_ms = std::max(event.watch_late_ms, late_ms);
	event.ui_late_ms = std::max(event.ui_late_ms, tick.ui_late_ms);

	if (any_stalled || process_late) {
		quiet_ticks_ = 0;
	} else if (++quiet_ticks_ == 1) {
		quiet_since_ns_ = tick.now_ns;
	}
	const std::uint64_t age_ms =
		(tick.now_ns - event_start_ns_) / nanoseconds_per_millisecond;
	const bool quiet = quiet_ticks_ >= quiet_ticks_to_close;
	if (quiet || age_ms >= max_event_ms) {
		const std::uint64_t end_ns = quiet ? quiet_since_ns_ : tick.now_ns;
		summarize(event);
		event.duration_ms = since_base_ms(end_ns) - event.start_ms;
		event.open = false;
		open_ = false;
		continued_ = !quiet;
		return Change::closed;
	}
	summarize(event);
	event.duration_ms = since_base_ms(tick.now_ns) - event.start_ms;
	return change;
}

} // namespace uml::runtime::stall
