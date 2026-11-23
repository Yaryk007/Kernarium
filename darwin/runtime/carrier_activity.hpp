// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <time.h>

namespace uml::runtime {

/*
 * Which vCPU carriers are outside an idle wait, so that the stall watch
 * (runtime/stall_watch.hpp) can sleep while all of them are inside one.
 *
 * The watch samples the carriers because the system pushes nothing when a
 * runnable thread is starved, faults, or sits in a wait the loader does not
 * mark. It needs to sample only while some carrier is not in its idle wait,
 * and a carrier says so itself: the loader's idle wrappers (kernel_waits.hpp)
 * report each carrier's entry to and exit from an idle wait here. Threads that
 * are not carriers (the kernel starts others, and all of them have a wait
 * cell) are ignored.
 *
 * The carriers hold a timer of the watch's own kqueue for it:
 *
 *   a carrier leaves its idle wait, none was outside one    armed for 50 ms
 *   the last carrier enters an idle wait                    removed, or pushed
 *                                                           past the earliest
 *                                                           deadline of a
 *                                                           timed wait
 *
 * so a carrier that stays out of its idle wait for 50 ms wakes the watch, and
 * one that only blinks (a guest's hrtimer wake lasts under a millisecond)
 * never does. A carrier that is due to wake from a timed wait and does not run
 * to clear its idle mark, because the system has not scheduled it, is found
 * by the pushed timer 50 ms after its deadline. A host that wakes the guest
 * (note_host_wake) arms the watch for the same reason. The watch's own wait
 * is a kevent, a system call of libsystem_kernel alone, which the thread
 * fence can hold.
 *
 * What is not covered. A carrier woken before the limit of its idle wait (a
 * timed one included) by a guest wake source (another carrier's IPI, or the
 * kernel's notification thread, which alone wakes CPU0 for a host descriptor)
 * is seen from its first instruction on, not while the system keeps it from
 * running, when every other carrier is idle. The kernel's fallback, which
 * wakes an idle secondary when CPU0 is late for a device, arms the watch and
 * so records such a stall, later and shorter than before. The case stays open
 * for a guest of one vCPU, a guest whose secondaries are offline (a suspended
 * one), a carrier woken by an IPI from one that goes idle again within the
 * bound, and a stall shorter than the fallback needs.
 *
 * Two more cases follow from the bound itself. A carrier that stays blocked
 * past its limit and the bound is looked at once and then left alone, so one
 * that wakes after that and is then starved is seen from its first
 * instruction too. A carrier that is due, is starved, and runs before the
 * watch has collected the timer's firing takes the firing back when it arms or
 * removes the timer, and the watch does not look.
 *
 * The deadlines and the timers run on the absolute clock, which the carriers'
 * waits run on, and stops while the host sleeps; the stall record's times are
 * on the continuous clock.
 */

/* How many vCPU carriers are tracked; a higher vCPU number is ignored here
 * and by the watch. */
inline constexpr unsigned carrier_capacity = 16;
/* How long a carrier may stay outside its idle wait before the watch wakes,
 * and how long past its deadline a carrier due to wake from a timed idle
 * wait is given to mark itself. */
inline constexpr unsigned carrier_arm_ms = 50;

/* ---- The carriers' side. All of these act on the calling thread. ---- */

/* The calling thread has named itself the carrier of vCPU `vcpu`
 * (uml-vcpu-N-carrier); it starts outside an idle wait. The loader's wrapper
 * of pthread_setname_np calls it. */
void carrier_named(unsigned vcpu) noexcept;

/* The calling carrier is in an idle host wait for the length of the scope;
 * a thread that is not a carrier does nothing. */
class CarrierIdle final {
public:
	/* The clock of a time limit. */
	enum class Limit : std::uint8_t {
		/* The time to wait, as pselect takes it. */
		relative,
		/* A time on the realtime clock, as pthread_cond_timedwait takes
		 * it. */
		realtime,
	};

	/* A wait with no time limit. */
	CarrierIdle() noexcept;
	/* A wait that ends by `limit`, none when null. */
	CarrierIdle(const timespec *limit, Limit clock) noexcept;
	~CarrierIdle();

	CarrierIdle(const CarrierIdle &) = delete;
	CarrierIdle &operator=(const CarrierIdle &) = delete;

private:
	bool counted_;
};

/* The host wakes a guest that may be asleep (a wake, a resume, a console
 * request, which ends a suspend the guest took by itself): the watch is armed,
 * so a wake that the system does not schedule is seen. Any thread. */
void note_host_wake() noexcept;

/* ---- The watch's side. ---- */

/* What woke the watch. */
inline constexpr unsigned watch_woken_by_timer = 1;
inline constexpr unsigned watch_woken_by_poke = 2;
inline constexpr unsigned watch_woken_by_stop = 4;

/* Opens the process's wake queue (once; it stays for the life of the process,
 * as the carriers may arm it after the watch ends), clears what an earlier
 * watch left in it and lets the carriers arm it. False without a queue. */
[[nodiscard]] bool watch_queue_open() noexcept;
/* The carriers stop arming it. */
void watch_queue_close() noexcept;
/* Blocks until the carriers' timer fires, the queue is poked or told to stop,
 * or `timeout_ns` (nullptr: no limit) passes; the bits above, 0 for a
 * timeout. Only the watch's thread calls it. */
[[nodiscard]] unsigned watch_queue_wait(const std::uint64_t *timeout_ns)
	noexcept;
/* Wakes the watch's wait, for it to read the host's state. Any thread. */
void watch_poke() noexcept;
/* Wakes the watch's wait for good. Any thread. */
void watch_stop() noexcept;

/* The carriers outside an idle wait, and the carriers that report to this
 * module, one bit per vCPU. The watch sleeps only while it sees every carrier
 * it samples in the second set and none in the first. */
[[nodiscard]] std::uint32_t carriers_outside_idle() noexcept;
[[nodiscard]] std::uint32_t carriers_reporting() noexcept;
/* Sets the timer for the carriers that are all idle now: removed, or pushed
 * past the earliest deadline of a timed wait. The watch calls it before it
 * sleeps, having just seen every carrier in its idle wait, and so spends the
 * deadlines it has looked past: a carrier still blocked after its deadline and
 * the bound does not wake the watch again. A carrier that enters its idle wait
 * last does the same without spending. */
void watch_settle() noexcept;

} // namespace uml::runtime
