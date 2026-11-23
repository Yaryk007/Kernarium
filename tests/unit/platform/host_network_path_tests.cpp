// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host's network path (platform/host_network_path.hpp). The state starts
 * usable, and every report replaces it, so a path that is back clears an
 * outage. The system's monitor reports the current path once as it starts,
 * whatever the path is, and again at each start; a path change itself cannot
 * be made here, so the report is checked as an event and the stand-in state
 * of the resolver's tests carries the two directions. What the system's
 * status and reason mean as an outage is a function of the two values, checked
 * for each of them. The monitor ties no Mach port of the process to the thread
 * that made it, whichever thread that is: the system's start-up would, and a
 * guest's restart ends the thread that booted it (guest-restart). A system
 * that gives no monitor leaves a monitor that says so, on a thread of its own,
 * and one that works does not. A monitor that has ended leaves nothing behind
 * that writes to the state.
 *
 * Besides usable, a report says whether an interface carries the path (only a
 * satisfied status does), and an observer learns of each report without
 * polling, until it is removed.
 */
#include "platform/host_network_path.hpp"
#include "host_network_path_attempt.hpp"
#include "host_network_path_status.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <mach/mach.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

namespace {

using uml::platform::HostNetworkPathMonitor;
using uml::platform::HostNetworkPathState;
using uml::testing::require;
using uml::testing::waitUntil;

/* The rights the process holds that are both send and receive and name no
 * kernel object: what a run loop or a framework's start-up makes for a thread.
 */
std::size_t portsOfThreadKind()
{
	mach_port_name_array_t names = nullptr;
	mach_msg_type_number_t nameCount = 0;
	mach_port_type_array_t types = nullptr;
	mach_msg_type_number_t typeCount = 0;
	require(mach_port_names(mach_task_self(), &names, &nameCount, &types,
				&typeCount) == KERN_SUCCESS,
		"the process's ports cannot be listed");
	std::size_t count = 0;
	for (mach_msg_type_number_t index = 0; index != typeCount; ++index) {
		ipc_info_object_type_t type{};
		mach_vm_address_t address = 0;
		const bool noObject =
			mach_port_kobject(mach_task_self(), names[index], &type,
					  &address) != KERN_SUCCESS ||
			static_cast<int>(type) == -1;
		if (noObject && types[index] ==
				(MACH_PORT_TYPE_SEND | MACH_PORT_TYPE_RECEIVE))
			++count;
	}
	(void)vm_deallocate(mach_task_self(),
			    reinterpret_cast<vm_address_t>(names),
			    nameCount * sizeof(mach_port_name_t));
	(void)vm_deallocate(mach_task_self(),
			    reinterpret_cast<vm_address_t>(types),
			    typeCount * sizeof(mach_port_type_t));
	return count;
}

/* Where the system's creation was asked for, and how often. */
std::atomic<std::thread::id> askedOn{};
std::atomic<int> asked{};

bool theSystemGivesNoMonitor()
{
	askedOn.store(std::this_thread::get_id());
	asked.fetch_add(1);
	return false;
}

/* The first monitor of the process, made on a thread that ends while the
 * monitor lives, as a guest's restart ends the thread that booted it: the
 * process holds the same ports after the thread is gone as while it ran. This
 * runs before every other test, because the system's start-up happens once. */
void the_first_monitor_ties_no_port_to_the_thread_that_made_it()
{
	HostNetworkPathState state;
	std::unique_ptr<HostNetworkPathMonitor> monitor;
	std::size_t whileTheThreadRuns = 0;
	std::thread maker{[&] {
		monitor = uml::platform::makeAppleHostNetworkPathMonitor(state);
		require(monitor != nullptr, "no monitor");
		require(waitUntil([&] { return state.reports() >= 1; },
				  std::chrono::seconds{10}),
			"the system did not report the path");
		whileTheThreadRuns = portsOfThreadKind();
	}};
	maker.join();
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	require(portsOfThreadKind() == whileTheThreadRuns,
		"the process lost a port when the thread that made the monitor "
		"ended");
}

void a_state_nobody_reported_to_is_usable_and_each_report_replaces_it()
{
	HostNetworkPathState state;
	require(state.usable(), "a state no one reported to is an outage");
	require(state.reports() == 0, "a report was counted before any");

	state.report(false);
	require(!state.usable(), "a report of no path left the path usable");
	state.report(false);
	require(!state.usable(), "a second report of no path cleared the outage");
	state.report(true);
	require(state.usable(), "a report of a path did not clear the outage");
	state.report(false);
	state.report(true);
	require(state.usable() && state.reports() == 5,
		"the transitions were not each counted");
}

void only_an_unsatisfied_path_is_an_outage_and_a_local_network_denial_is_not()
{
	using uml::platform::hostNetworkPathIsOutage;
	const nw_path_unsatisfied_reason_t reasons[] = {
		nw_path_unsatisfied_reason_not_available,
		nw_path_unsatisfied_reason_cellular_denied,
		nw_path_unsatisfied_reason_wifi_denied,
		nw_path_unsatisfied_reason_local_network_denied,
		nw_path_unsatisfied_reason_vpn_inactive};
	for (const auto reason : reasons) {
		/* The reason means something only for an unsatisfied path. */
		require(!hostNetworkPathIsOutage(nw_path_status_satisfied, reason),
			"a satisfied path is an outage");
		require(!hostNetworkPathIsOutage(nw_path_status_satisfiable, reason),
			"a path that needs a connection first is an outage");
		require(!hostNetworkPathIsOutage(nw_path_status_invalid, reason),
			"an invalid path is an outage");
		require(hostNetworkPathIsOutage(nw_path_status_unsatisfied, reason) ==
				(reason != nw_path_unsatisfied_reason_local_network_denied),
			"an unsatisfied path is not an outage, or a local network "
			"denial alone is one");
	}
	require(hostNetworkPathIsOutage(nw_path_status_unsatisfied,
					nw_path_unsatisfied_reason_not_available),
		"no network is not an outage");
	require(!hostNetworkPathIsOutage(
			nw_path_status_unsatisfied,
			nw_path_unsatisfied_reason_local_network_denied),
		"a local network denial alone is an outage");
}

void a_report_says_whether_an_interface_carries_the_path()
{
	HostNetworkPathState state;
	require(state.carried(), "a state no one reported to has no carrying path");

	/* A report that gives only the usable flag says carried when usable. */
	state.report(false);
	require(!state.carried(), "no path is carried");
	state.report(true);
	require(state.carried(), "a usable path is not carried");
	/* A path can be usable and not yet carried: it needs a connection. */
	state.report(true, false);
	require(state.usable() && !state.carried(),
		"a usable path that needs a connection first is carried");
	state.report(false, false);
	require(!state.usable() && !state.carried(), "an outage is carried");
	state.report(true, true);
	require(state.usable() && state.carried() && state.reports() == 5,
		"the reports were not each counted");
	/* A look that compares the count with its last one sees an outage that
	 * began and ended in between, which the flag alone has forgotten. */
	require(state.uncarried_reports() == 3,
		"the reports that said no interface carries the path were not "
		"each counted");
	HostNetworkPathState quiet;
	quiet.report(true);
	require(quiet.uncarried_reports() == 0,
		"a carried report counted as an outage");
}

void only_a_satisfied_path_is_carried()
{
	using uml::platform::hostNetworkPathIsCarried;
	require(hostNetworkPathIsCarried(nw_path_status_satisfied),
		"a satisfied path is not carried");
	require(!hostNetworkPathIsCarried(nw_path_status_satisfiable),
		"a path that needs a connection first is carried");
	require(!hostNetworkPathIsCarried(nw_path_status_unsatisfied),
		"an unsatisfied path is carried");
	require(!hostNetworkPathIsCarried(nw_path_status_invalid),
		"an invalid path is carried");
}

void an_observer_hears_each_report_until_it_is_removed()
{
	struct Counter {
		int calls{};
		std::uint64_t reports_seen{};
		const HostNetworkPathState *state{};
	} counter;
	HostNetworkPathState state;
	counter.state = &state;
	const auto observe = [](void *context) noexcept {
		auto &seen = *static_cast<Counter *>(context);
		seen.calls++;
		/* The state already holds the report when the observer runs. */
		seen.reports_seen = seen.state->reports();
	};
	state.report(true);
	state.set_observer(observe, &counter);
	state.report(false);
	require(counter.calls == 1 && counter.reports_seen == 2,
		"the observer did not hear a report once, after it was counted");
	state.report(true, false);
	require(counter.calls == 2, "the observer missed a report");
	state.set_observer(nullptr, nullptr);
	state.report(true);
	require(counter.calls == 2, "a removed observer still heard a report");
}

void a_removed_observer_is_not_running_when_set_returns()
{
	/* A report in flight on another thread finishes first; none starts after.
	 * The observer waits on a flag the test sets once removal has begun, so
	 * removal can only return after that call has ended: the test records
	 * whether it had, at the instant removal returned. */
	struct Gate {
		std::atomic<bool> inside{false};
		std::atomic<bool> release{false};
		std::atomic<bool> done{false};
		std::atomic<bool> removing{false};
		std::atomic<bool> removed{false};
		std::atomic<bool> done_when_removed{false};
	} gate;
	HostNetworkPathState state;
	state.set_observer(
		[](void *context) noexcept {
			auto &shared = *static_cast<Gate *>(context);
			shared.inside.store(true);
			while (!shared.release.load())
				std::this_thread::yield();
			shared.done.store(true);
		},
		&gate);
	std::thread reporter{[&] { state.report(false); }};
	require(waitUntil([&] { return gate.inside.load(); },
			  std::chrono::seconds{10}),
		"the observer was not called");
	std::thread remover{[&] {
		gate.removing.store(true);
		state.set_observer(nullptr, nullptr);
		gate.done_when_removed.store(gate.done.load());
		gate.removed.store(true);
	}};
	require(waitUntil([&] { return gate.removing.load(); },
			  std::chrono::seconds{10}),
		"the removal did not begin");
	/* Removal that waits for the call has not returned while it runs. */
	require(!waitUntil([&] { return gate.removed.load(); },
			   std::chrono::milliseconds{200}),
		"the observer was removed while a call of it was running");
	gate.release.store(true);
	remover.join();
	require(gate.done_when_removed.load(),
		"the removal returned before the running call ended");
	reporter.join();
}

void the_monitor_reports_the_path_at_each_start()
{
	HostNetworkPathState state;
	for (std::uint64_t start = 1; start <= 3; ++start) {
		std::unique_ptr<HostNetworkPathMonitor> monitor =
			uml::platform::makeAppleHostNetworkPathMonitor(state);
		require(monitor != nullptr, "no monitor");
		require(waitUntil([&] { return state.reports() >= start; },
				  std::chrono::seconds{10}),
			"the system did not report the path at a start");
		require(!monitor->failed(), "a monitor that reports is failed");
	}
}

/* The system's call that makes a monitor returns null when it cannot: the
 * monitor says so, its thread asks on a thread other than the requester's, and
 * ends the monitor's wait for a report that will not come. Nothing is reported
 * and the state stays as it was. */
void a_system_that_gives_no_monitor_leaves_a_monitor_that_says_so()
{
	HostNetworkPathState state;
	std::unique_ptr<HostNetworkPathMonitor> monitor =
		uml::platform::makeAppleHostNetworkPathMonitor(
			state, theSystemGivesNoMonitor);
	require(monitor != nullptr, "no monitor");
	require(waitUntil([&] { return monitor->failed(); },
			  std::chrono::seconds{10}),
		"a system that gave no monitor was not told");
	require(asked.load() == 1, "the system was asked more than once");
	require(askedOn.load() != std::this_thread::get_id(),
		"the system's monitor was made on the thread that asked for it");
	std::this_thread::sleep_for(std::chrono::milliseconds{100});
	require(state.reports() == 0 && state.usable(),
		"a monitor the system did not give wrote to the state");
	monitor.reset();
}

void a_monitor_that_ended_writes_nothing()
{
	/* Each state ends right after its monitor, before or after the first
	 * report: a report in flight must find a lock and no state. */
	for (int round = 0; round < 200; ++round) {
		auto state = std::make_unique<HostNetworkPathState>();
		auto monitor =
			uml::platform::makeAppleHostNetworkPathMonitor(*state);
		require(monitor != nullptr, "no monitor");
		if (round % 2 == 1)
			(void)waitUntil([&] { return state->reports() >= 1; },
					std::chrono::seconds{10});
		monitor.reset();
		state.reset();
	}

	HostNetworkPathState state;
	{
		auto monitor =
			uml::platform::makeAppleHostNetworkPathMonitor(state);
		require(waitUntil([&] { return state.reports() >= 1; },
				  std::chrono::seconds{10}),
			"the system did not report the path");
	}
	const auto reports = state.reports();
	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	require(state.reports() == reports,
		"a monitor that ended still wrote to the state");
}

} // namespace

int main()
{
	the_first_monitor_ties_no_port_to_the_thread_that_made_it();
	a_state_nobody_reported_to_is_usable_and_each_report_replaces_it();
	only_an_unsatisfied_path_is_an_outage_and_a_local_network_denial_is_not();
	a_report_says_whether_an_interface_carries_the_path();
	only_a_satisfied_path_is_carried();
	an_observer_hears_each_report_until_it_is_removed();
	a_removed_observer_is_not_running_when_set_returns();
	the_monitor_reports_the_path_at_each_start();
	a_system_that_gives_no_monitor_leaves_a_monitor_that_says_so();
	a_monitor_that_ended_writes_nothing();
	std::puts("host network path tests passed");
	return 0;
}
