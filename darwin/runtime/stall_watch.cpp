// SPDX-License-Identifier: GPL-2.0-or-later
#include "stall_watch.hpp"

#include "carrier_activity.hpp"
#include "kernel_threads.hpp"
#include "platform/host_state_figures.hpp"
#include "session_record.hpp"
#include "stall_record.hpp"
#include "vcpu_carrier_name.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/task.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace uml::runtime {
namespace {

using namespace ::uml::runtime::stall;
using ::uml::platform::HostThermalLevel;
using ::uml::platform::HostThermalProvider;
using ::uml::platform::HostThermalState;

static_assert(carrier_capacity == max_vcpus);

constexpr std::uint64_t nanoseconds_per_millisecond = 1'000'000;
constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
/* How often the host's state is read and the main queue asked while a stall
 * is suspected, and how often the watch looks at what the observer left; the
 * process's counters are sampled every context_period_ms. */
constexpr std::uint64_t host_poll_ms = 250;
constexpr std::uint64_t context_period_ms = 1000;
/* The watch goes to sleep after this many ticks in a row with every carrier
 * in its idle wait and nothing suspected. */
constexpr unsigned park_after_quiet_ticks = 2;
/* An event that is open is written this often. */
constexpr std::uint64_t open_write_ms = 1000;
/* The context ring covers the seconds before an event. */
constexpr std::size_t context_ring = 8;
constexpr std::uint64_t before_window_ns = 5 * nanoseconds_per_second;
/* A thread that has no name yet is looked at again this often. */
constexpr std::uint64_t unnamed_retry_ms = 200;

mach_timebase_info_data_t timebase() noexcept
{
	static const mach_timebase_info_data_t base = [] {
		mach_timebase_info_data_t value{};
		(void)mach_timebase_info(&value);
		if (value.numer == 0 || value.denom == 0)
			value = {1, 1};
		return value;
	}();
	return base;
}

std::uint64_t ticks_to_ns(std::uint64_t ticks) noexcept
{
	const auto base = timebase();
	return ticks * base.numer / base.denom;
}

/* The continuous clock keeps counting while the system sleeps, as the wall
 * clock and the guest's own do, so a time in the record is the base plus its
 * offset whatever the session slept through. */
std::uint64_t continuous_ns() noexcept
{
	return ticks_to_ns(mach_continuous_time());
}

std::int64_t wall_seconds() noexcept
{
	timespec time{};
	(void)clock_gettime(CLOCK_REALTIME, &time);
	return static_cast<std::int64_t>(time.tv_sec);
}

/* ---- The main queue's lateness. ----
 * The callbacks outlive any watch, so they keep to process-wide state. */
struct MainQueuePing final {
	std::atomic<bool> outstanding{false};
	std::atomic<std::uint64_t> sent_ns{0};
	/* The worst answer since the last reading, -1 for none. */
	std::atomic<std::int64_t> worst_ms{-1};
	/* Whether the main queue has ever answered: a process that runs no main
	 * queue, as a command-line run, never does, and then it says nothing. */
	std::atomic<bool> serviced{false};
	/* The watch wants the main queue asked: an event is open or a stall is
	 * being confirmed. */
	std::atomic<bool> wanted{false};
};

MainQueuePing main_queue_ping;

void main_queue_answered(void *) noexcept
{
	const std::uint64_t sent = main_queue_ping.sent_ns.load(
		std::memory_order_acquire);
	const auto late = static_cast<std::int64_t>(
		(continuous_ns() - sent) / nanoseconds_per_millisecond);
	std::int64_t worst = main_queue_ping.worst_ms.load(std::memory_order_relaxed);
	while (late > worst && !main_queue_ping.worst_ms.compare_exchange_weak(
		       worst, late, std::memory_order_relaxed)) {
	}
	main_queue_ping.serviced.store(true, std::memory_order_release);
	main_queue_ping.outstanding.store(false, std::memory_order_release);
}

/* Asks the main queue, if the watch wants it asked and no ask is out. Only
 * the host observer's queue calls it: dispatch_async_f is libdispatch, which
 * the watch's own thread, a thread the fence can hold, never calls. */
void send_main_queue_ping(std::uint64_t now_ns) noexcept
{
	if (!main_queue_ping.wanted.load(std::memory_order_relaxed) ||
	    main_queue_ping.outstanding.load(std::memory_order_acquire))
		return;
	main_queue_ping.sent_ns.store(now_ns, std::memory_order_release);
	main_queue_ping.outstanding.store(true, std::memory_order_release);
	dispatch_async_f(dispatch_get_main_queue(), nullptr, &main_queue_answered);
}

/* The worst lateness of the main queue since the last call, -1 when it is
 * not serviced. */
std::int32_t main_queue_lateness_ms(std::uint64_t now_ns) noexcept
{
	if (!main_queue_ping.serviced.load(std::memory_order_acquire))
		return -1;
	std::int64_t worst = main_queue_ping.worst_ms.exchange(-1,
		std::memory_order_relaxed);
	if (main_queue_ping.outstanding.load(std::memory_order_acquire)) {
		const std::uint64_t sent = main_queue_ping.sent_ns.load(
			std::memory_order_acquire);
		if (now_ns > sent)
			worst = std::max<std::int64_t>(worst,
				static_cast<std::int64_t>((now_ns - sent) /
					nanoseconds_per_millisecond));
	}
	return static_cast<std::int32_t>(std::min<std::int64_t>(worst, 0x7fffffff));
}

/* ---- The memory pressure level. ----
 * What the watch observes of the host is published process-wide
 * (platform/host_state_figures.hpp), where the session record's checkpoint
 * reads it too. */
void memory_pressure_changed(void *context) noexcept
{
	const auto source = static_cast<dispatch_source_t>(context);
	const auto flags = dispatch_source_get_data(source);
	const std::uint8_t level =
		(flags & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0 ? 2 :
		(flags & DISPATCH_MEMORYPRESSURE_WARN) != 0 ? 1 : 0;
	const bool changed = ::uml::platform::publishedMemoryPressure() != level;
	::uml::platform::publishMemoryPressure(level);
	/* A watch that sleeps wakes to record the change with the time it was
	 * seen. */
	if (changed)
		watch_poke();
}

/* The write end of the running watch's request pipe, for a reading of the
 * host's state asked from outside (request_host_state_reading). The lock keeps
 * a descriptor the watch has closed from being written. */
std::mutex request_lock;
int request_write_descriptor = -1;

/* ---- What the process's counters read at one moment. ---- */
struct Context final {
	std::uint64_t at_ns{};
	std::uint64_t cpu_ns{};
	std::uint64_t performance_ns{};
	std::uint64_t energy_nj{};
	std::uint64_t involuntary{};
	std::uint64_t footprint{};
	std::int64_t left{-1};
	std::uint64_t compressed{};
	std::uint32_t pageins{};
	std::uint32_t faults{};
	std::uint64_t system_pageins{};
	std::uint64_t system_decompressions{};
};

std::uint16_t hundredths(std::uint64_t amount, std::uint64_t wall_ns) noexcept
{
	if (wall_ns == 0)
		return 0;
	return static_cast<std::uint16_t>(std::min<std::uint64_t>(
		amount * 100 / wall_ns, 9999));
}

std::uint32_t mebibytes(std::uint64_t bytes) noexcept
{
	return static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes >> 20,
		0xffffffffU));
}

RunState run_state(int value) noexcept
{
	switch (value) {
	case TH_STATE_RUNNING:
		return RunState::running;
	case TH_STATE_STOPPED:
		return RunState::stopped;
	case TH_STATE_UNINTERRUPTIBLE:
		return RunState::uninterruptible;
	case TH_STATE_HALTED:
		return RunState::halted;
	default:
		return RunState::waiting;
	}
}

/* A registry thread's identity: which vCPU it carries, if any. */
struct Identity final {
	mach_port_t port{MACH_PORT_NULL};
	/* >= 0 the vCPU; -1 not a carrier; -2 not named yet. */
	int cpu{-1};
	std::uint64_t retry_ns{};
};

/* What a reading of the carriers says of whether the watch may sleep. */
struct CarrierView final {
	/* Bit v: the carrier of vCPU v was read. */
	std::uint32_t present{};
	/* Bit v: and it sat in its idle wait. */
	std::uint32_t idle{};
	/* A thread is yet to be named or to be started, and may be a carrier. */
	bool undecided{};
};

} // namespace

struct StallWatch::State final {
	std::unique_ptr<HostThermalProvider> thermal;
	StallRecord record;
	StallDetector detector{record};
	KernelThreadSnapshot snapshot{};
	Identity known[kernel_thread_capacity]{};
	char slot[session_stall_slot_bytes]{};

	std::atomic<bool> stopping{false};
	pthread_t thread{};
	bool started{};

	mach_port_t host{MACH_PORT_NULL};
	/* The host observer: a serial queue of the system's worker threads, which
	 * the fence never holds. It takes the memory pressure and the system's
	 * notices of the thermal state and Low Power Mode, and, while a stall is
	 * suspected, reads the thermal source every host_poll_ms and asks the main
	 * queue; the watch asks for that by writing a byte to a pipe. The watch
	 * thread calls into no framework, and waits in a kevent, which lies in
	 * libsystem_kernel alone because the thread fence defers a thread inside
	 * libsystem_c on every pass. It reads what the observer left in atomics,
	 * and the observer wakes it when the host's state moved. */
	dispatch_queue_t observer_queue{};
	dispatch_source_t pressure_source{};
	dispatch_source_t request_source{};
	int request_pipe[2]{-1, -1};

	/* Thread-confined state. */
	std::uint32_t fence_epoch{};
	HostThermalState host_state{};
	std::uint8_t pressure{};
	std::uint64_t next_poll_ns{};
	std::uint64_t next_observe_ns{};
	std::uint64_t next_context_ns{};
	std::uint64_t next_write_ns{};
	bool idle_seen{};
	/* Ticks in a row with every carrier idle and nothing suspected. */
	unsigned quiet_ticks{};
	Context recent[context_ring]{};
	std::size_t recent_count{};
	Context opened_at{};

	explicit State(std::unique_ptr<HostThermalProvider> source)
		: thermal{std::move(source)}
	{
	}

	/* ---- What the host observer saw. ---- */

	[[nodiscard]] static HostThermalState observed() noexcept
	{
		const auto published = ::uml::platform::publishedHostState();
		return published ? published->thermal : HostThermalState{};
	}

	/* On the observer's queue. A reading that moved wakes a watch that
	 * sleeps, which records the change with the time it was seen. */
	static void observe_host(void *argument) noexcept
	{
		auto *self = static_cast<State *>(argument);
		const HostThermalState reading = self->thermal->state();
		const bool moved = observed() != reading;
		/* Published before the poke, so that the watch, woken by it, reads
		 * the new state and not the old. */
		::uml::platform::publishHostThermal(reading);
		if (moved)
			watch_poke();
		send_main_queue_ping(continuous_ns());
	}

	/* On the observer's queue: the watch asked for a reading. */
	static void observe_requested(void *argument) noexcept
	{
		auto *self = static_cast<State *>(argument);
		char bytes[16];
		while (read(self->request_pipe[0], bytes, sizeof(bytes)) > 0) {
		}
		observe_host(argument);
	}

	/* The system's notice of a change of the thermal state or of Low Power
	 * Mode, on a thread of the system's. */
	static void host_state_notified(void *argument) noexcept
	{
		auto *self = static_cast<State *>(argument);
		dispatch_async_f(self->observer_queue, self, &State::observe_host);
	}

	static void request_cancelled(void *argument) noexcept
	{
		auto *self = static_cast<State *>(argument);
		(void)close(self->request_pipe[0]);
		self->request_pipe[0] = -1;
	}

	/* ---- Reading the system. ---- */

	[[nodiscard]] Context read_context(std::uint64_t now_ns) const noexcept
	{
		Context context;
		context.at_ns = now_ns;
		rusage usage{};
		if (getrusage(RUSAGE_SELF, &usage) == 0) {
			context.cpu_ns = (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) +
				static_cast<std::uint64_t>(usage.ru_stime.tv_sec)) *
				nanoseconds_per_second +
				(static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
				 static_cast<std::uint64_t>(usage.ru_stime.tv_usec)) * 1000;
			context.involuntary = static_cast<std::uint64_t>(usage.ru_nivcsw);
		}
		task_power_info_v2_data_t power{};
		mach_msg_type_number_t power_count = TASK_POWER_INFO_V2_COUNT;
		if (task_info(mach_task_self(), TASK_POWER_INFO_V2,
			    reinterpret_cast<task_info_t>(&power), &power_count) ==
		    KERN_SUCCESS) {
			context.energy_nj = power.task_energy;
			context.performance_ns = ticks_to_ns(power.task_ptime);
		}
		task_vm_info_data_t memory{};
		mach_msg_type_number_t memory_count = TASK_VM_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_VM_INFO,
			    reinterpret_cast<task_info_t>(&memory), &memory_count) ==
		    KERN_SUCCESS) {
			context.footprint = memory.phys_footprint;
			context.compressed = memory.compressed;
			/* Zero where the host sets no limit, as on a Mac. */
			if (memory_count >= TASK_VM_INFO_COUNT &&
			    memory.limit_bytes_remaining != 0)
				context.left = static_cast<std::int64_t>(
					memory.limit_bytes_remaining);
		}
		task_events_info_data_t events{};
		mach_msg_type_number_t events_count = TASK_EVENTS_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_EVENTS_INFO,
			    reinterpret_cast<task_info_t>(&events), &events_count) ==
		    KERN_SUCCESS) {
			context.pageins = static_cast<std::uint32_t>(events.pageins);
			context.faults = static_cast<std::uint32_t>(events.faults);
		}
		if (host != MACH_PORT_NULL) {
			vm_statistics64_data_t system{};
			mach_msg_type_number_t system_count = HOST_VM_INFO64_COUNT;
			if (host_statistics64(host, HOST_VM_INFO64,
				    reinterpret_cast<host_info64_t>(&system),
				    &system_count) == KERN_SUCCESS) {
				context.system_pageins = system.pageins;
				context.system_decompressions = system.decompressions;
			}
		}
		return context;
	}

	void sample_context(std::uint64_t now_ns) noexcept
	{
		if (recent_count == context_ring) {
			std::copy(recent + 1, recent + context_ring, recent);
			--recent_count;
		}
		recent[recent_count++] = read_context(now_ns);
	}

	/* ---- The threads. ---- */

	void resolve(Identity &identity, mach_port_t port,
		std::uint64_t now_ns) noexcept
	{
		identity.port = port;
		thread_extended_info_data_t information{};
		mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(port, THREAD_EXTENDED_INFO,
			    reinterpret_cast<thread_info_t>(&information),
			    &count) != KERN_SUCCESS) {
			identity.cpu = -1;
			return;
		}
		const std::string_view name{information.pth_name,
			::strnlen(information.pth_name, sizeof(information.pth_name))};
		if (name.empty()) {
			identity.cpu = -2;
			identity.retry_ns = now_ns + unnamed_retry_ms *
				nanoseconds_per_millisecond;
			return;
		}
		const int cpu = vcpu_carrier_index(name);
		identity.cpu = cpu < static_cast<int>(max_vcpus) ? cpu : -1;
	}

	[[nodiscard]] CarrierView read_carriers(Tick &tick) noexcept
	{
		CarrierView view;
		snapshot_kernel_threads(snapshot);
		/* A thread in the middle of its creation, or with no name yet, may
		 * be a carrier; a table that overflowed lists no carrier it missed. */
		view.undecided = snapshot.creator_count != 0 || snapshot.overflowed;
		for (std::size_t index = 0; index < snapshot.thread_count; ++index) {
			const mach_port_t port = snapshot.threads[index];
			Identity &identity = known[index];
			if (identity.port != port ||
			    (identity.cpu == -2 && tick.now_ns >= identity.retry_ns))
				resolve(identity, port, tick.now_ns);
			if (identity.cpu == -2)
				view.undecided = true;
			/* The kernel's other threads (the notification and alarm
			 * helpers) are not carriers whatever they wait in. */
			if (identity.cpu < 0)
				continue;
			thread_extended_info_data_t information{};
			mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
			if (thread_info(port, THREAD_EXTENDED_INFO,
				    reinterpret_cast<thread_info_t>(&information),
				    &count) != KERN_SUCCESS)
				continue;
			VcpuSample &sample = tick.vcpus[identity.cpu];
			sample.present = true;
			sample.state = run_state(information.pth_run_state);
			sample.cpu_ns = information.pth_user_time +
				information.pth_system_time;
			sample.wait_word = snapshot.waits[index];
			const std::uint32_t bit = std::uint32_t{1} << identity.cpu;
			view.present |= bit;
			if (sample.state == RunState::waiting &&
			    ::uml::platform::wait_kind(sample.wait_word) ==
				    ::uml::platform::HostWait::idle)
				view.idle |= bit;
		}
		return view;
	}

	/* Whether the watch has nothing to watch: every carrier it sees reports its
	 * idle waits, sits in one, and none is outside it. */
	[[nodiscard]] bool carriers_all_idle(const CarrierView &view) const noexcept
	{
		return view.present != 0 && view.idle == view.present &&
			!view.undecided &&
			(view.present & ~carriers_reporting()) == 0 &&
			carriers_outside_idle() == 0;
	}

	/* ---- The host's state. ---- */

	void write_slot() noexcept
	{
		(void)format_stall_slot(slot, sizeof(slot), record, wall_seconds());
		write_session_stall_slot({slot, sizeof(slot)});
	}

	void poll_host(std::uint64_t now_ns) noexcept
	{
		bool changed = false;
		const HostThermalState state = observed();
		const std::uint32_t at = detector.since_base_ms(now_ns);
		if (state.level != host_state.level) {
			record.thermal.push({at,
				static_cast<std::uint8_t>(host_state.level),
				static_cast<std::uint8_t>(state.level)});
			changed = true;
		}
		if (state.lowPowerMode != host_state.lowPowerMode ||
		    state.activeProcessors != host_state.activeProcessors) {
			record.power.push({at, static_cast<std::uint8_t>(
				state.lowPowerMode ? 1 : 0), state.activeProcessors});
			changed = true;
		}
		host_state = state;
		const std::uint8_t level = ::uml::platform::publishedMemoryPressure();
		if (level != pressure) {
			record.pressure.push({at, level, pressure});
			pressure = level;
			changed = true;
		}
		if (changed)
			write_slot();
	}

	/* ---- Events. ---- */

	void begin_context(StallEvent &event, std::uint64_t now_ns) noexcept
	{
		opened_at = read_context(now_ns);
		const HostThermalState state = observed();
		event.thermal_start = static_cast<std::uint8_t>(state.level);
		event.low_power = state.lowPowerMode;
		event.processors_start = state.activeProcessors;
		event.pressure = ::uml::platform::publishedMemoryPressure();
		/* The five seconds before this reading: from the oldest sample the
		 * ring holds within five seconds of it. A watch that slept took its
		 * first sample at the wake, so the window then is the time since
		 * the wake, and the field means what it says. */
		if (recent_count >= 1 &&
		    opened_at.at_ns - recent[recent_count - 1].at_ns <=
			    before_window_ns) {
			const Context &last = opened_at;
			std::size_t first = recent_count - 1;
			while (first > 0 &&
			       last.at_ns - recent[first - 1].at_ns <= before_window_ns)
				--first;
			const Context &from = recent[first];
			const std::uint64_t wall = last.at_ns - from.at_ns;
			event.cores_before = hundredths(last.cpu_ns - from.cpu_ns, wall);
			event.performance_before = hundredths(
				last.performance_ns - from.performance_ns, wall);
			event.watts_before = hundredths(
				last.energy_nj - from.energy_nj, wall);
		}
	}

	void end_context(StallEvent &event, std::uint64_t now_ns) noexcept
	{
		const Context closed = read_context(now_ns);
		const std::uint64_t wall = closed.at_ns - opened_at.at_ns;
		event.cores_during = hundredths(closed.cpu_ns - opened_at.cpu_ns, wall);
		event.performance_during = hundredths(
			closed.performance_ns - opened_at.performance_ns, wall);
		event.watts_during = hundredths(
			closed.energy_nj - opened_at.energy_nj, wall);
		event.involuntary_switches = static_cast<std::uint32_t>(
			closed.involuntary - opened_at.involuntary);
		event.footprint_mib = mebibytes(closed.footprint);
		event.left_mib = closed.left < 0 ? -1 :
			static_cast<std::int32_t>(std::min<std::uint64_t>(
				mebibytes(static_cast<std::uint64_t>(closed.left)), 0x7fffffff));
		event.compressed_mib = static_cast<std::int32_t>(std::clamp<std::int64_t>(
			(static_cast<std::int64_t>(closed.compressed) -
			 static_cast<std::int64_t>(opened_at.compressed)) / (1 << 20),
			-0x7fffffff, 0x7fffffff));
		event.pageins = closed.pageins - opened_at.pageins;
		event.faults = closed.faults - opened_at.faults;
		event.system_pageins = static_cast<std::uint32_t>(std::min<std::uint64_t>(
			closed.system_pageins - opened_at.system_pageins, 0xffffffffU));
		event.system_decompressions = static_cast<std::uint32_t>(
			std::min<std::uint64_t>(closed.system_decompressions -
				opened_at.system_decompressions, 0xffffffffU));
		const HostThermalState state = observed();
		event.thermal_end = static_cast<std::uint8_t>(state.level);
		event.processors_end = state.activeProcessors;
		event.low_power = event.low_power || state.lowPowerMode;
		event.pressure = std::max(event.pressure,
			::uml::platform::publishedMemoryPressure());
	}

	void handle(StallDetector::Change change, std::uint64_t now_ns) noexcept
	{
		switch (change) {
		case StallDetector::Change::none:
			return;
		case StallDetector::Change::opened:
			begin_context(record.events.back(), now_ns);
			write_slot();
			next_write_ns = now_ns + open_write_ms * nanoseconds_per_millisecond;
			return;
		case StallDetector::Change::updated:
			if (now_ns >= next_write_ns) {
				end_context(record.events.back(), now_ns);
				write_slot();
				next_write_ns = now_ns +
					open_write_ms * nanoseconds_per_millisecond;
			}
			return;
		case StallDetector::Change::closed:
			end_context(record.events.back(), now_ns);
			write_slot();
			return;
		}
	}

	/* One look at the carriers. `rebaseline`: the watch slept since the last. */
	void tick(std::uint64_t now_ns, std::uint32_t late_ms,
		bool rebaseline) noexcept
	{
		Tick tick;
		tick.now_ns = now_ns;
		tick.late_ms = late_ms;
		tick.rebaseline = rebaseline;
		const std::uint32_t epoch = kernel_thread_fence_epoch();
		tick.disturbed = kernel_thread_reclaim_inhibited() || epoch != fence_epoch;
		fence_epoch = epoch;
		tick.ui_late_ms = main_queue_lateness_ms(now_ns);
		const CarrierView view = read_carriers(tick);
		handle(detector.observe(tick), now_ns);
		const bool suspicious = detector.suspicious();
		main_queue_ping.wanted.store(suspicious, std::memory_order_relaxed);
		quiet_ticks = !suspicious && carriers_all_idle(view) ? quiet_ticks + 1 : 0;
		/* While a stall is suspected the host's state is read, and the main
		 * queue asked, every host_poll_ms: the first time at once. */
		if (suspicious && now_ns >= next_observe_ns) {
			request_observation();
			next_observe_ns = now_ns + host_poll_ms * nanoseconds_per_millisecond;
		}
		/* The first sight of a carrier in a marked idle wait says the
		 * markers reach the wrappers; it is written once, then rides with
		 * the next write. */
		if (!idle_seen && record.idle_samples != 0) {
			idle_seen = true;
			write_slot();
		}
		if (now_ns >= next_context_ns) {
			sample_context(now_ns);
			next_context_ns = now_ns + context_period_ms *
				nanoseconds_per_millisecond;
		}
		if (now_ns >= next_poll_ns) {
			poll_host(now_ns);
			next_poll_ns = now_ns + host_poll_ms * nanoseconds_per_millisecond;
		}
	}

	/* Asks the host observer for a reading, by a write to a pipe that is a
	 * system call and no framework's. */
	void request_observation() noexcept
	{
		if (request_pipe[1] >= 0)
			(void)write(request_pipe[1], "r", 1);
	}

	/* Puts the watch to sleep if it still may. The carriers' timer is set
	 * for what the idle carriers have left (watch_settle), and a carrier that
	 * left its idle wait since the last reading arms it, so the watch is
	 * woken whichever came first; this returns false when one has. */
	[[nodiscard]] bool try_to_park() noexcept
	{
		watch_settle();
		return carriers_outside_idle() == 0;
	}

	void run() noexcept
	{
		(void)pthread_setname_np(stall_watch_thread_name);
		/* This thread's local storage exists before anything marks a wait. */
		::uml::platform::current_wait_cell = nullptr;
		/* The set-up is done before the measurement starts, so its time is
		 * nobody's lateness. */
		host_state = observed();
		sample_context(continuous_ns());
		std::uint64_t now_ns = continuous_ns();
		detector.reset(now_ns);
		record.base_unix = wall_seconds();
		next_context_ns = now_ns + context_period_ms * nanoseconds_per_millisecond;
		next_poll_ns = now_ns;
		next_observe_ns = now_ns;
		write_slot();
		constexpr std::uint64_t period_ns = tick_ms * nanoseconds_per_millisecond;
		/* The watch wakes at a deadline of its own and measures how late it
		 * woke; the work of a tick (a slow write of the slot, say) is not
		 * part of that. */
		std::uint64_t deadline_ns = continuous_ns() + period_ns;
		bool parked = false;
		while (!stopping.load(std::memory_order_acquire)) {
			now_ns = continuous_ns();
			unsigned woken = 0;
			if (parked) {
				woken = watch_queue_wait(nullptr);
			} else if (now_ns < deadline_ns) {
				const std::uint64_t wait_ns = deadline_ns - now_ns;
				woken = watch_queue_wait(&wait_ns);
			}
			now_ns = continuous_ns();
			if (stopping.load(std::memory_order_acquire) ||
			    (woken & watch_woken_by_stop) != 0)
				break;
			/* The host's state moved: it is recorded with this time. */
			if ((woken & watch_woken_by_poke) != 0) {
				poll_host(now_ns);
				next_poll_ns = now_ns + host_poll_ms * nanoseconds_per_millisecond;
			}
			bool rebaseline = false;
			if (parked) {
				/* Only a carrier's timer ends the sleep. */
				if ((woken & watch_woken_by_timer) == 0)
					continue;
				parked = false;
				(void)pthread_setname_np(stall_watch_thread_name);
				rebaseline = true;
				deadline_ns = now_ns;
			} else if (now_ns < deadline_ns) {
				/* Woken before the tick is due, by a timer or a poke. */
				continue;
			}
			const std::uint64_t late_ns =
				now_ns > deadline_ns ? now_ns - deadline_ns : 0;
			tick(now_ns, static_cast<std::uint32_t>(std::min<std::uint64_t>(
				late_ns / nanoseconds_per_millisecond, 0xffffffffU)), rebaseline);
			deadline_ns = continuous_ns() + period_ns;
			if (quiet_ticks >= park_after_quiet_ticks && try_to_park()) {
				parked = true;
				(void)pthread_setname_np(stall_watch_asleep_name);
				/* Two looks again after every sleep, the one after a wake
				 * (which measures nothing) the first of them. */
				quiet_ticks = 0;
			}
		}
		/* An event in progress ends where it was last seen. */
		Tick last;
		last.now_ns = continuous_ns();
		last.disturbed = true;
		handle(detector.observe(last), last.now_ns);
		write_slot();
	}

	static void *entry(void *argument) noexcept
	{
		static_cast<State *>(argument)->run();
		return nullptr;
	}

	void start_observer() noexcept
	{
		::uml::platform::publishHostThermal(thermal->state());
		observer_queue = dispatch_queue_create("uml.stall-watch.host",
			DISPATCH_QUEUE_SERIAL);
		if (observer_queue == nullptr)
			return;
		pressure_source = dispatch_source_create(
			DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
			DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
				DISPATCH_MEMORYPRESSURE_CRITICAL,
			observer_queue);
		if (pressure_source != nullptr) {
			dispatch_set_context(pressure_source, pressure_source);
			dispatch_source_set_event_handler_f(pressure_source,
				&memory_pressure_changed);
			dispatch_resume(pressure_source);
		}
		if (pipe(request_pipe) == 0) {
			for (const int descriptor : request_pipe) {
				(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
				(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);
			}
			request_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
				static_cast<uintptr_t>(request_pipe[0]), 0, observer_queue);
		}
		if (request_source != nullptr) {
			/* Asked for from outside only once the read end has a source. */
			{
				const std::lock_guard guard{request_lock};
				request_write_descriptor = request_pipe[1];
			}
			dispatch_set_context(request_source, this);
			dispatch_source_set_event_handler_f(request_source,
				&State::observe_requested);
			dispatch_source_set_cancel_handler_f(request_source,
				&State::request_cancelled);
			dispatch_resume(request_source);
		} else {
			for (int &descriptor : request_pipe) {
				if (descriptor >= 0)
					(void)close(descriptor);
				descriptor = -1;
			}
		}
		thermal->observe(&State::host_state_notified, this);
	}

	void stop_observer() noexcept
	{
		/* No notice is running or will start; the observer's queue then ends
		 * what it has. */
		thermal->observe(nullptr, nullptr);
		/* Before the source goes, whose cancel handler closes the read end: a
		 * request from outside then cannot write to a pipe with no reader. */
		{
			const std::lock_guard guard{request_lock};
			request_write_descriptor = -1;
		}
		if (request_source != nullptr) {
			dispatch_source_cancel(request_source);
			dispatch_release(request_source);
			request_source = nullptr;
		}
		if (pressure_source != nullptr) {
			dispatch_source_cancel(pressure_source);
			dispatch_release(pressure_source);
			pressure_source = nullptr;
		}
		if (observer_queue != nullptr) {
			/* A handler that is running ends before the state goes. */
			dispatch_sync_f(observer_queue, nullptr, [](void *) {});
			dispatch_release(observer_queue);
			observer_queue = nullptr;
		}
		if (request_pipe[1] >= 0) {
			(void)close(request_pipe[1]);
			request_pipe[1] = -1;
		}
		::uml::platform::withdrawHostState();
	}

	void start() noexcept
	{
		if (!watch_queue_open())
			return;
		host = mach_host_self();
		::uml::platform::withdrawHostState();
		start_observer();
		/* The thread takes no signal: the kernel's process-directed signals
		 * must never pick it. */
		sigset_t all;
		sigset_t previous;
		sigfillset(&all);
		if (pthread_sigmask(SIG_SETMASK, &all, &previous) != 0)
			return;
		pthread_attr_t attributes;
		if (pthread_attr_init(&attributes) == 0) {
			(void)pthread_attr_setstacksize(&attributes, 128 * 1024);
			(void)pthread_attr_set_qos_class_np(&attributes,
				QOS_CLASS_USER_INTERACTIVE, 0);
			started = pthread_create(&thread, &attributes, &State::entry,
				this) == 0;
			(void)pthread_attr_destroy(&attributes);
		}
		(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	}

	void stop() noexcept
	{
		if (started) {
			stopping.store(true, std::memory_order_release);
			watch_stop();
			(void)pthread_join(thread, nullptr);
			started = false;
		}
		watch_queue_close();
		stop_observer();
		if (host != MACH_PORT_NULL) {
			(void)mach_port_deallocate(mach_task_self(), host);
			host = MACH_PORT_NULL;
		}
	}
};

StallWatch::StallWatch(std::unique_ptr<HostThermalProvider> thermal)
	: state_{std::make_unique<State>(
		  thermal != nullptr ? std::move(thermal) :
		  ::uml::platform::makeAppleHostThermalProvider())}
{
	state_->start();
}

StallWatch::~StallWatch()
{
	state_->stop();
}

std::uint64_t stall_clock_ns() noexcept
{
	return continuous_ns();
}

void request_host_state_reading() noexcept
{
	const std::lock_guard guard{request_lock};
	if (request_write_descriptor >= 0)
		(void)write(request_write_descriptor, "r", 1);
}

std::unique_ptr<StallWatch> start_stall_watch(
	std::unique_ptr<HostThermalProvider> thermal)
{
	if (!session_record_open())
		return nullptr;
	return std::make_unique<StallWatch>(std::move(thermal));
}

} // namespace uml::runtime
