// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The system's side of the memory pressure response (memory_pressure_response.
 * hpp): the memory pressure source, and the clock, figures and actions the
 * policy reads and takes from the process.
 */
#include "memory_pressure_response.hpp"

#include "guest_statistics.hpp"
#include "platform/gpu_cache_reclaim.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <time.h>
#include <unistd.h>

namespace uml::runtime {

namespace {

/* The longest line the console gets in one write. */
constexpr std::size_t line_limit = 1000;

/* The process's one observer of the reports. A report is delivered with the
 * mutex held, which is what makes clearing the observer wait for a call in
 * progress, and a report allocates nothing: it is made when the system is short
 * of memory. The pressure queue outlives static destruction (a guest still runs
 * when the process exits), so the slot is never destroyed: an event that comes
 * after exit() has run the other destructors still finds a live mutex. */
struct ObserverSlot final {
	std::mutex mutex;
	PressureObserver observer{};
	void *context{};
};

[[clang::no_destroy]] constinit ObserverSlot observer_slot;

class SystemActions final : public PressureActions {
public:
	SystemActions(GuestTrim trim, void *context) noexcept
		: trim_{trim}, context_{context}
	{
	}

	/* The clock of the quiet period and the lines' windows: monotonic, and
	 * stopped while the system sleeps, so a long sleep does not end a period. */
	std::uint64_t now_ns() noexcept override
	{
		return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	}

	PressureFigures figures() noexcept override
	{
		PressureFigures figures;
		task_vm_info_data_t information{};
		mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_VM_INFO,
			    reinterpret_cast<task_info_t>(&information), &count) ==
		    KERN_SUCCESS) {
			figures.footprint = information.phys_footprint;
			if (count >= TASK_VM_INFO_REV3_COUNT)
				figures.graphics = static_cast<std::uint64_t>(
					std::max<std::int64_t>(
						information.ledger_tag_graphics_footprint +
							information.ledger_tag_graphics_footprint_compressed,
						0));
		}
		if (const auto ram = guest_ram_usage()) {
			figures.guest_held = ram->held_bytes;
			figures.wire_failures = ram->wire_failures;
		}
		return figures;
	}

	std::uint64_t reclaim_gpu() noexcept override
	{
		const auto reclaim = ::uml::platform::publishedGpuCacheReclaimer();
		return reclaim == nullptr ? 0U : reclaim();
	}

	TrimOutcome trim_page_cache(TrimKind kind) noexcept override
	{
		return trim_ == nullptr ? TrimOutcome::unreachable :
			trim_(context_, kind);
	}

	void log(std::string_view line) noexcept override
	{
		char buffer[line_limit + 1];
		const std::size_t size = std::min(line.size(), line_limit);
		std::memcpy(buffer, line.data(), size);
		buffer[size] = '\n';
		/* One write, so that the line is not split among the console's
		 * other writers. */
		(void)write(STDERR_FILENO, buffer, size + 1);
	}

	void report(const PressureReport &report) noexcept override
	{
		const std::lock_guard lock{observer_slot.mutex};
		if (observer_slot.observer != nullptr)
			observer_slot.observer(observer_slot.context, report);
	}

private:
	GuestTrim trim_;
	void *context_;
};

/*
 * The process's memory pressure source and the two serial queues that serve it,
 * made on the first watch and kept. A run binds its state to the hub and
 * unbinds before it returns. They are never cancelled or released: cancelling a
 * source leaves a libdispatch worker thread behind (still there seconds later),
 * and a run that ends leaves no thread of its own. A source that nothing is
 * bound to drops what it delivers.
 *
 * The source's handler runs on the source queue and never waits: it records
 * the delivery in a ring and signals the policy queue, which handles the
 * deliveries one at a time in the order they arrived. The policy's work can
 * take seconds (a trim waits for the guest), and the system merges the events
 * raised while a handler runs into one, so a handler that did that work would
 * hide the order of the events raised meanwhile. Recording takes a mutex held
 * for a few instructions and allocates nothing: it runs when the system is
 * short of memory.
 */
struct Hub final {
	/* The source's queue: the handler that records a delivery. */
	dispatch_queue_t queue{};
	dispatch_source_t source{};
	/* The policy's queue: the deliveries' handling and the retries, one at a
	 * time. A trim waits for the guest here, and what else a shared queue
	 * serves must not wait behind it. */
	dispatch_queue_t policy_queue{};
	/* Signals the policy queue that the ring holds a delivery; a data source
	 * so that signalling allocates nothing. */
	dispatch_source_t deliveries{};
	/* The timer of the retries, made with the hub so that arming a retry
	 * allocates nothing of its own: a retry is wanted when the system is
	 * short of memory. One-shot, set again for each retry and disarmed at
	 * an unbind. It is resumed by the first retry: a resumed timer source
	 * costs the process a thread of libdispatch's, and a process that
	 * never needs a retry keeps the one thread of the pressure source. */
	dispatch_source_t retry_timer{};
	bool retry_timer_resumed{};

	/* The deliveries not yet handled, oldest first. Each carries the epoch it
	 * arrived in, so that one that arrived before a bind or an unbind is
	 * dropped. When the ring is full (a policy call held for a long time while
	 * the system raised many events) the oldest delivery is dropped. */
	struct Delivery final {
		std::uint64_t flags{};
		std::uint64_t epoch{};
	};
	static constexpr std::size_t ring_capacity = 64;
	std::mutex ring_mutex;
	std::array<Delivery, ring_capacity> ring{};
	std::size_t ring_first{};
	std::size_t ring_count{};
	/* Counts every bind and unbind. */
	std::atomic<std::uint64_t> epoch{};

	/* The following are touched only on the policy queue. */
	/* The run that listens. */
	MemoryPressureWatch::State *current{};
	/* The run and the epoch the timer was last set for, so that a retry that
	 * fires after its run ended finds it. */
	MemoryPressureWatch::State *retry_state{};
	std::uint64_t retry_epoch{};

	/* `seen` is the epoch the callback sampled before it read the flags. */
	void record(std::uint64_t flags, std::uint64_t seen) noexcept
	{
		const std::lock_guard lock{ring_mutex};
		if (ring_count == ring.size()) {
			ring_first = (ring_first + 1U) % ring.size();
			--ring_count;
		}
		ring[(ring_first + ring_count) % ring.size()] = {flags, seen};
		++ring_count;
	}

	/* The oldest delivery, and whether more follow it. */
	bool take(Delivery &delivery, bool &more) noexcept
	{
		const std::lock_guard lock{ring_mutex};
		if (ring_count == 0U)
			return false;
		delivery = ring[ring_first];
		ring_first = (ring_first + 1U) % ring.size();
		--ring_count;
		more = ring_count != 0U;
		return true;
	}

	void clear() noexcept
	{
		const std::lock_guard lock{ring_mutex};
		ring_count = 0;
	}
};

} // namespace

struct MemoryPressureWatch::State final {
	State(Hub &hub_, GuestTrim trim, void *context,
		PressureTiming timing) noexcept
		: hub{hub_}, actions{trim, context}, response{actions, timing}
	{
	}

	~State();

	State(const State &) = delete;
	State &operator=(const State &) = delete;

	Hub &hub;
	SystemActions actions;
	MemoryPressureResponse response;
	/* The timer is set, for a retry or for the end of the quiet period; only
	 * the policy queue touches these. */
	bool retry_armed{};
	bool timer_for_retry{};
};

namespace {

void retry_due(void *context) noexcept;

/* On the policy queue, after anything that may have left a guest trim
 * pending or moved the end of the quiet period. A retry comes first: it sets
 * the timer even when it is set for the quiet period. The quiet period's own
 * timer is not set again for each delivery: it fires at the first deadline,
 * on_retry() finds the period not over, and the next call sets it for what is
 * left. */
void arm_retry(Hub &hub, MemoryPressureWatch::State &state) noexcept
{
	std::uint64_t delay = state.response.retry_delay_ns();
	const bool retry = delay != 0U;
	if (retry) {
		if (state.retry_armed && state.timer_for_retry)
			return;
	} else {
		if (state.retry_armed)
			return;
		delay = state.response.quiet_delay_ns();
		if (delay == 0U)
			return;
	}
	state.retry_armed = true;
	state.timer_for_retry = retry;
	hub.retry_state = &state;
	hub.retry_epoch = hub.epoch.load();
	dispatch_source_set_timer(hub.retry_timer,
		dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(delay)),
		DISPATCH_TIME_FOREVER, delay / 10U);
	if (!hub.retry_timer_resumed) {
		hub.retry_timer_resumed = true;
		dispatch_resume(hub.retry_timer);
	}
}

void disarm_retry(Hub &hub) noexcept
{
	hub.retry_state = nullptr;
	if (hub.retry_timer_resumed)
		dispatch_source_set_timer(hub.retry_timer, DISPATCH_TIME_FOREVER,
			DISPATCH_TIME_FOREVER, 0);
}

/* On the policy queue, when the timer fires. A retry that was set for a run
 * that has since ended finds the hub bound elsewhere, or bound again. */
void retry_due(void *context) noexcept
{
	Hub &hub = *static_cast<Hub *>(context);
	MemoryPressureWatch::State *const state = hub.current;
	if (state == nullptr || hub.retry_state != state ||
	    hub.retry_epoch != hub.epoch.load())
		return;
	state->retry_armed = false;
	state->timer_for_retry = false;
	(void)state->response.on_retry();
	arm_retry(hub, *state);
}

/* On the source's queue. The source's data is the status flags the system
 * raised since the last call: one event, or the events that came closer
 * together than this handler takes to run (a few instructions, as it only
 * records), which the system merged. A merged mask is resolved by its most
 * serious flag (pressure_level_from_dispatch), and that residual is all that is
 * left of the ambiguity: the normal flag merged with a higher one reads as the
 * higher level, and the next event corrects it. The delivery is recorded, and
 * the policy queue handles it in its turn. The epoch is sampled before the
 * flags are read, so that a callback the scheduler stops between the two, across
 * a stop and a new start of the watch, hands its old notification to nobody. */
void level_changed(void *context) noexcept
{
	Hub &hub = *static_cast<Hub *>(context);
	const std::uint64_t seen = hub.epoch.load();
	hub.record(dispatch_source_get_data(hub.source), seen);
	dispatch_source_merge_data(hub.deliveries, 1);
}

/* On the policy queue, once for each signal: the oldest delivery is handled
 * (its report comes from the policy, before that delivery's work, here and not
 * on the source's queue, because the reaction it reports is decided in the
 * order the deliveries are handled), and the next is signalled for. Handling
 * one delivery for each run lets a bind or an unbind, which wait on this
 * queue, go between two deliveries instead of behind all of them. */
void delivery_due(void *context) noexcept
{
	Hub &hub = *static_cast<Hub *>(context);
	Hub::Delivery delivery;
	bool more = false;
	if (!hub.take(delivery, more))
		return;
	MemoryPressureWatch::State *const state = hub.current;
	if (state != nullptr && delivery.epoch == hub.epoch.load()) {
		(void)state->response.on_level(
			pressure_level_from_dispatch(delivery.flags));
		arm_retry(hub, *state);
	}
	if (more)
		dispatch_source_merge_data(hub.deliveries, 1);
}

Hub *make_hub() noexcept
{
	auto *const hub = new (std::nothrow) Hub;
	if (hub == nullptr)
		return nullptr;
	hub->queue = dispatch_queue_create("uml.memory-pressure",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
			QOS_CLASS_USER_INITIATED, 0));
	hub->policy_queue = dispatch_queue_create("uml.memory-pressure.policy",
		dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
			QOS_CLASS_USER_INITIATED, 0));
	if (hub->queue == nullptr || hub->policy_queue == nullptr)
		return nullptr;
	hub->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
		DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN |
			DISPATCH_MEMORYPRESSURE_CRITICAL,
		hub->queue);
	if (hub->source == nullptr)
		return nullptr;
	hub->deliveries = dispatch_source_create(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0,
		hub->policy_queue);
	if (hub->deliveries == nullptr)
		return nullptr;
	hub->retry_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
		hub->policy_queue);
	if (hub->retry_timer == nullptr)
		return nullptr;
	dispatch_set_context(hub->deliveries, hub);
	dispatch_source_set_event_handler_f(hub->deliveries, &delivery_due);
	dispatch_resume(hub->deliveries);
	dispatch_set_context(hub->source, hub);
	dispatch_source_set_event_handler_f(hub->source, &level_changed);
	dispatch_resume(hub->source);
	dispatch_set_context(hub->retry_timer, hub);
	dispatch_source_set_event_handler_f(hub->retry_timer, &retry_due);
	return hub;
}

Hub *shared_hub() noexcept
{
	static Hub *const hub = make_hub();
	return hub;
}

struct Binding final {
	Hub *hub;
	MemoryPressureWatch::State *state;
};

/* Both wait, on the policy queue, for the delivery or retry that is running
 * (a trim in flight): the policy's calls never wait for the emulator's
 * lifecycle mutex, so the wait is bounded by the guest's reply. The deliveries
 * still queued are dropped: the epoch moves on, and the ring is emptied. */
void bind(Hub &hub, MemoryPressureWatch::State *state) noexcept
{
	Binding binding{&hub, state};
	dispatch_sync_f(hub.policy_queue, &binding, [](void *context) {
		auto &binding = *static_cast<Binding *>(context);
		binding.hub->current = binding.state;
		binding.hub->epoch.fetch_add(1);
		binding.hub->clear();
	});
}

void unbind(Hub &hub, MemoryPressureWatch::State *state) noexcept
{
	Binding binding{&hub, state};
	dispatch_sync_f(hub.policy_queue, &binding, [](void *context) {
		auto &binding = *static_cast<Binding *>(context);
		if (binding.hub->current == binding.state) {
			binding.hub->current = nullptr;
			binding.hub->epoch.fetch_add(1);
			binding.hub->clear();
			disarm_retry(*binding.hub);
		}
	});
}

} // namespace

/* A response that is running ends before the state goes, and none, nor any
 * retry, starts after the unbind. */
MemoryPressureWatch::State::~State()
{
	unbind(hub, this);
}

MemoryPressureWatch::MemoryPressureWatch(std::unique_ptr<State> state) noexcept
	: state_{std::move(state)}
{
}

MemoryPressureWatch::~MemoryPressureWatch() = default;

void set_pressure_observer(PressureObserver observer, void *context) noexcept
{
	const std::lock_guard lock{observer_slot.mutex};
	observer_slot.observer = observer;
	observer_slot.context = context;
}

std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
	GuestTrim trim, void *context, PressureTiming timing)
{
	Hub *const hub = shared_hub();
	if (hub == nullptr)
		return nullptr;
	auto state = std::unique_ptr<MemoryPressureWatch::State>{
		new (std::nothrow) MemoryPressureWatch::State{*hub, trim, context, timing}};
	if (state == nullptr)
		return nullptr;
	bind(*hub, state.get());
	return std::unique_ptr<MemoryPressureWatch>{
		new (std::nothrow) MemoryPressureWatch{std::move(state)}};
}

} // namespace uml::runtime
