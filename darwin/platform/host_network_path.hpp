// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace uml::platform {

/*
 * Whether the host has a network path, as the system last said. A reader on
 * any thread asks at the moment it needs the answer; a source of reports
 * (the monitor below, or a test) writes it.
 *
 * A state no one has reported to is usable: a monitor that has not spoken yet
 * must not turn a working network into an outage.
 */
class HostNetworkPathState final {
public:
	/* A report's observer: called after each report, on the thread that
	 * reported, with the context it was set with. It must not block, report
	 * or set an observer. */
	using Observer = void (*)(void *context) noexcept;

	/* False only while the last report said the host has no path at all. A
	 * path that needs a connection first (an on-demand VPN) is usable: the
	 * attempt itself brings it up. */
	[[nodiscard]] bool usable() const noexcept
	{
		return usable_.load(std::memory_order_acquire);
	}
	/* Whether the last report said an interface carries the path: the system
	 * has a path it can use now. False for no path at all, and for one that
	 * needs a connection first, which no interface carries yet. A state no
	 * one has reported to carries one, as it is usable. */
	[[nodiscard]] bool carried() const noexcept
	{
		return carried_.load(std::memory_order_acquire);
	}
	/* How many reports said that no interface carries the path. A reader
	 * that compares it with the count it saw at its last look learns of an
	 * outage that began and ended between the two looks, which `carried()`
	 * alone cannot show. */
	[[nodiscard]] std::uint64_t uncarried_reports() const noexcept
	{
		return uncarried_reports_.load(std::memory_order_acquire);
	}
	/* Every report sets the state, so a path that is back clears the
	 * outage. A report that gives only whether the path is usable says that
	 * an interface carries it exactly when it is. */
	void report(bool usable) noexcept
	{
		report(usable, usable);
	}
	void report(bool usable, bool carried) noexcept
	{
		usable_.store(usable, std::memory_order_release);
		carried_.store(carried, std::memory_order_release);
		/* Before the total: a reader that sees a report counted sees what
		 * it said. */
		if (!carried)
			uncarried_reports_.fetch_add(1, std::memory_order_release);
		reports_.fetch_add(1, std::memory_order_release);
		std::lock_guard guard{observer_lock_};
		if (observer_ != nullptr)
			observer_(observer_context_);
	}
	/* Reports so far; a source that has reported once has said what the
	 * system's path is. */
	[[nodiscard]] std::uint64_t reports() const noexcept
	{
		return reports_.load(std::memory_order_acquire);
	}
	/* The one observer, which is how a reader that waits learns of a report
	 * without polling; none with a null function. Once this returns, no call
	 * of the old observer is running or will start. */
	void set_observer(Observer observer, void *context) noexcept
	{
		std::lock_guard guard{observer_lock_};
		observer_ = observer;
		observer_context_ = context;
	}

private:
	std::atomic<bool> usable_{true};
	std::atomic<bool> carried_{true};
	std::atomic<std::uint64_t> reports_{0};
	std::atomic<std::uint64_t> uncarried_reports_{0};
	std::mutex observer_lock_;
	Observer observer_{};
	void *observer_context_{};
};

/* The system's path monitor, reporting into a state while it lives. It is
 * event driven: the system reports the current path once at the start and
 * then each change, from a thread of its own, and nothing else wakes the
 * process. It opens no connection. The monitor makes its own thread for the
 * system's start-up, so that no Mach port of the process is tied to the
 * thread that asked for it; its owner keeps it for as long as it wants the
 * state, which a guest's restart does not end. */
class HostNetworkPathMonitor {
public:
	virtual ~HostNetworkPathMonitor() = default;
	/* True once the system has given no monitor: this one never reports and
	 * never will. Its owner drops it and makes another at its next try; one
	 * that has not failed is kept. */
	[[nodiscard]] virtual bool failed() const noexcept = 0;
};

/* Starts a monitor that reports into `state`, which must outlive it. Once the
 * monitor's destructor returns, no report is running or will start. A system
 * that gives no monitor leaves the state as it is, and the monitor says so
 * (failed()). Throws std::bad_alloc or std::system_error when its storage or
 * its thread cannot be had; the caller cleans up as for any other resource it
 * could not get. */
[[nodiscard]] std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state);

} // namespace uml::platform
