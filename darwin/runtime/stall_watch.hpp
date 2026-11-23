// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_thermal.hpp"

#include <cstdint>
#include <memory>

namespace uml::runtime {

/* The name of the watch's thread, and the name it takes while it sleeps, so
 * that a sample of the process shows which it is in. The sleeping name is set
 * once the watch has decided to sleep, after its last look, and the awake
 * name again when a timer of the carriers wakes it. */
inline constexpr char stall_watch_thread_name[] = "uml-stall-watch";
inline constexpr char stall_watch_asleep_name[] = "uml-stall-watch (asleep)";

/*
 * The watch that records why a guest's vCPUs stopped running.
 *
 * Every 50 ms (below, while a carrier is outside its idle wait) a thread of
 * the process, `uml-stall-watch`, at the user interactive quality of service,
 * reads each vCPU carrier's run state and CPU time from the system
 * (thread_info, on the carriers the kernel thread registry lists) and the
 * wait the carrier says it is in (platform/
 * host_wait.hpp). A carrier that is runnable and gets little of the time, or
 * sits in an uninterruptible wait or one host wait and used little of the
 * time, for 100 ms or more is stalled, and the stalls form events (runtime/
 * stall_record.hpp).
 *
 * The thread sleeps, with no wake-up of its own, while every carrier is in its
 * idle wait and nothing is suspected: the carriers report their idle waits
 * (runtime/carrier_activity.hpp), and one that stays outside its wait for
 * 50 ms, or is due to wake from a timed wait and has not, or that the host
 * wakes, wakes the watch. It sleeps after two looks in a row with every
 * carrier idle and nothing suspected, counted afresh after each sleep (the
 * first look after a wake, which starts the measurement over so that the sleep
 * is nobody's starvation and measures nothing, is the first of the two). While
 * some carrier is outside its idle wait, or does not report its idle waits, it
 * looks every 50 ms.
 *
 * Each event carries what the host looked like around it:
 * the thermal state, Low Power Mode and the active core count; the process's
 * cores, performance cores and power in the 5 s before and during it; its
 * memory, paging and involuntary context switches; the lateness of the
 * watch's own wake-up and of the main queue, which tell a stall of the whole
 * process from one of the carriers. Changes of the thermal state, of Low Power
 * Mode and the core count, and of the memory pressure level are recorded with
 * the time they were seen.
 *
 * The times are on the continuous clock (stall_clock_ns), which counts the
 * time the system sleeps; the record's base is the wall clock at the start.
 *
 * The record is the stall slot of the session record (session_record.hpp),
 * rewritten when an event opens or ends, once a second while one is open, when
 * a transition is seen and when the first carrier is seen in an idle wait.
 * Nothing is written otherwise. The watch never suspends a thread, takes no
 * lock a carrier takes and reads no guest memory; it uses only public system
 * interfaces, in every build.
 *
 * The watch thread calls no framework, and waits in a kevent, a system call of
 * libsystem_kernel alone. A fence that holds the kernel's threads
 * (thread_fence.hpp) holds this thread too, and a hold inside a system
 * library's lock would last until the release. What needs Foundation or
 * libdispatch (the thermal state, Low Power Mode and the core count, the
 * memory pressure level, the main queue's answer) is observed on a serial
 * queue that the system's worker threads run and the fence never holds, and
 * the watch reads it from atomics, which are published process-wide
 * (platform/host_state_figures.hpp) for the session record's checkpoint to read
 * too. The system's notices of a change of the thermal state or Low Power Mode
 * and the memory pressure source publish a change and wake the watch to record
 * it; nothing polls them. While an event is open or a stall is being confirmed
 * the watch has the observer read the host's state, the core count included,
 * every 250 ms and ask the main queue; it asks by a byte in a pipe. The first
 * tick after a hold starts the measurement over.
 */
class StallWatch final {
public:
	/* Starts the thread. `thermal` is the host's thermal source, the
	 * system's when null. */
	explicit StallWatch(
		std::unique_ptr<::uml::platform::HostThermalProvider> thermal);
	/* Ends the thread, writes the record a last time and closes an event
	 * that is open. */
	~StallWatch();

	StallWatch(const StallWatch &) = delete;
	StallWatch &operator=(const StallWatch &) = delete;

private:
	struct State;
	std::unique_ptr<State> state_;
};

/* The clock of every time in the record: the continuous clock, in
 * nanoseconds. */
[[nodiscard]] std::uint64_t stall_clock_ns() noexcept;

/* Asks the running watch, if any, for one reading of the host's state, thermal
 * state, Low Power Mode and core count included, so that a notice the process
 * missed (it was suspended) does not leave the published state stale. The host
 * calls it when the app returns to the foreground. Any thread. */
void request_host_state_reading() noexcept;

/* Starts the watch of the session that is open on a record file
 * (session_record_open); null when none is. */
[[nodiscard]] std::unique_ptr<StallWatch> start_stall_watch(
	std::unique_ptr<::uml::platform::HostThermalProvider> thermal = nullptr);

} // namespace uml::runtime
