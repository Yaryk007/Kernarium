// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/host_network_path.hpp"
#include "host_network_path_status.hpp"
#if UML_HOST_NETWORK_PATH_TESTING
#include "host_network_path_attempt.hpp"
#endif

#import <Network/Network.h>

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace uml::platform {

bool hostNetworkPathIsOutage(nw_path_status_t status,
			     nw_path_unsatisfied_reason_t reason) noexcept
{
	return status == nw_path_status_unsatisfied &&
		reason != nw_path_unsatisfied_reason_local_network_denied;
}

bool hostNetworkPathIsCarried(nw_path_status_t status) noexcept
{
	return status == nw_path_status_satisfied;
}

namespace {

/* What the monitor's thread and its updates share. It outlives the monitor
 * that made it, so that an update in flight when the monitor ends, and the
 * thread on its way out, find a lock and no state, not freed memory. */
struct Shared final {
	std::mutex lock;
	std::condition_variable wake;
	HostNetworkPathState *state{};
	bool ended{};
	/* The system gave no monitor: this one will never report. */
	std::atomic<bool> failed{};

	void deliver(bool usable, bool carried) noexcept
	{
		std::lock_guard guard{lock};
		if (state != nullptr)
			state->report(usable, carried);
	}
};

#if UML_HOST_NETWORK_PATH_TESTING
/* The test variant asks a function before each call to the system's creation
 * of a monitor, so that a test has the system give none; the product has no
 * such function. */
using Gate = HostNetworkPathAttempt;

nw_path_monitor_t createMonitor(Gate gate)
{
	return (gate == nullptr || gate()) ? nw_path_monitor_create() : nil;
}
#else
struct Gate {};

nw_path_monitor_t createMonitor(Gate)
{
	return nw_path_monitor_create();
}
#endif

/*
 * The first Network.framework monitor of a process starts the framework up,
 * and the thread that makes it keeps a Mach port of its own (a send and
 * receive right) until that thread ends. A guest's restart ends the thread
 * that booted it, so a monitor made on that thread leaves the process one port
 * short of its first boot's, which the restart audit (guest-restart) reports.
 * The monitor is therefore made on a thread of its own, which lives as long as
 * the monitor does and waits without waking for anything.
 *
 * When the system gives no monitor the thread says so and ends: nothing waits
 * for what will not come, and the owner makes another monitor at its next try.
 */
void run(std::shared_ptr<Shared> shared, Gate gate) noexcept
{
	(void)::pthread_setname_np("uml-network-path");
	nw_path_monitor_t monitor = createMonitor(gate);
	if (monitor == nil) {
		shared->failed.store(true, std::memory_order_release);
		return;
	}
	nw_path_monitor_set_queue(monitor, dispatch_queue_create(
		"uml.network-path",
		dispatch_queue_attr_make_with_qos_class(
			DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)));
	/* On the system's queue: a block that does no more than write the
	 * state and run its observer, which only wakes a reader. */
	nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
		shared->deliver(!hostNetworkPathIsOutage(
					nw_path_get_status(path),
					nw_path_get_unsatisfied_reason(path)),
				hostNetworkPathIsCarried(
					nw_path_get_status(path)));
	});
	nw_path_monitor_start(monitor);
	{
		std::unique_lock guard{shared->lock};
		shared->wake.wait(guard, [&] { return shared->ended; });
	}
	nw_path_monitor_cancel(monitor);
}

class AppleHostNetworkPathMonitor final : public HostNetworkPathMonitor {
public:
	AppleHostNetworkPathMonitor(HostNetworkPathState &state, Gate gate)
	{
		shared_->state = &state;
		/* The thread holds the shared part alone: it may outlive this
		 * object by the time it takes to cancel the monitor. */
		const std::shared_ptr<Shared> shared = shared_;
		std::thread{[shared, gate] { run(shared, gate); }}.detach();
	}

	~AppleHostNetworkPathMonitor() override
	{
		{
			std::lock_guard guard{shared_->lock};
			shared_->state = nullptr;
			shared_->ended = true;
		}
		shared_->wake.notify_all();
	}

	[[nodiscard]] bool failed() const noexcept override
	{
		return shared_->failed.load(std::memory_order_acquire);
	}

private:
	std::shared_ptr<Shared> shared_{std::make_shared<Shared>()};
};

} // namespace

#if UML_HOST_NETWORK_PATH_TESTING
std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state,
				HostNetworkPathAttempt attempt)
{
	return std::make_unique<AppleHostNetworkPathMonitor>(state, attempt);
}
#endif

std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state)
{
	return std::make_unique<AppleHostNetworkPathMonitor>(state, Gate{});
}

} // namespace uml::platform
