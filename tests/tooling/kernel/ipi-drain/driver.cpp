// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the Darwin port's real idle wait (time.c) and IPI code (smp.c) on this
// thread as CPU0, with the host calls that matter counted, and checks what an
// idle cycle costs and what it does with the IPI wake pipe: one kevent() for a
// wake by the deadline or the stop descriptor and nothing else, no look at the
// pipe when the wait found it empty and no vector is pending, one drain at
// most, and no vector lost or delayed when a sender races the wait.
#include <atomic>
#include <cassert>
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/select.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "irq.h"
void os_init_smp(void);
void os_idle_sleep(void);
int os_send_ipi(int cpu, int vector);
void os_raise_local_ipi(int vector);
int os_timer_create(void);
int os_timer_one_shot(int cpu, unsigned long long nsecs);
void os_timer_disable(int cpu);
long long os_nsecs(void);
}

// The host calls an idle cycle makes, counted while the tracked thread runs.
// A wait is a kevent() that may block: it asks for events.
static thread_local bool tracked = false;
static std::atomic<int> polls{0}, reads{0}, selects{0}, sigmasks{0}, kevents{0}, waits{0};
// The last wait's change list and timeout, as it was called.
struct Wait { int changes; bool timer_add, timer_delete, timeout; long long timeout_ns; };
static Wait last_wait;
// Run once, on the tracked thread, as the next wait returns or as the next
// vectors are handed over: a sender that races exactly there.
static std::function<void()> after_wait, after_exchange;
// Make the next wait fail with EINTR before it looks at anything, or make the
// host refuse the deadline timer it is asked to add.
static bool inject_eintr = false, inject_timer_error = false;

template <typename T> static T next_symbol(const char *name) {
	return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}
extern "C" {
int poll(struct pollfd *fds, nfds_t count, int timeout) {
	static auto real = next_symbol<int (*)(struct pollfd *, nfds_t, int)>("poll");
	if (tracked) polls++;
	return real(fds, count, timeout);
}
ssize_t read(int fd, void *buffer, size_t size) {
	static auto real = next_symbol<ssize_t (*)(int, void *, size_t)>("read");
	if (tracked) reads++;
	return real(fd, buffer, size);
}
int pselect(int count, fd_set *in, fd_set *out, fd_set *error, const struct timespec *timeout, const sigset_t *mask) {
	static auto real = next_symbol<int (*)(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *)>("pselect");
	if (tracked) selects++;
	return real(count, in, out, error, timeout, mask);
}
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
	static auto real = next_symbol<int (*)(int, const sigset_t *, sigset_t *)>("pthread_sigmask");
	if (tracked) sigmasks++;
	return real(how, set, old);
}
int kevent(int queue, const struct kevent *changes, int nchanges, struct kevent *events, int nevents, const struct timespec *timeout) {
	static auto real = next_symbol<int (*)(int, const struct kevent *, int, struct kevent *, int, const struct timespec *)>("kevent");
	if (!tracked) return real(queue, changes, nchanges, events, nevents, timeout);
	kevents++;
	if (nevents <= 0) return real(queue, changes, nchanges, events, nevents, timeout);
	waits++;
	Wait seen{nchanges, false, false, timeout != nullptr, timeout ? timeout->tv_sec * 1000000000LL + timeout->tv_nsec : 0};
	for (int i = 0; i < nchanges; i++)
		if (changes[i].filter == EVFILT_TIMER) {
			seen.timer_add |= (changes[i].flags & EV_ADD) != 0;
			seen.timer_delete |= (changes[i].flags & EV_DELETE) != 0;
		}
	last_wait = seen;
	int result;
	if (inject_eintr) {
		inject_eintr = false;
		errno = EINTR;
		result = -1;
	} else if (inject_timer_error && seen.timer_add) {
		// The host refused the timer: the call returns at once with the error.
		EV_SET(&events[0], 0, EVFILT_TIMER, EV_ERROR, 0, EINVAL, nullptr);
		result = 1;
	} else {
		result = real(queue, changes, nchanges, events, nevents, timeout);
	}
	if (after_wait) {
		int saved = errno;
		auto hook = std::move(after_wait);
		after_wait = nullptr;
		hook();
		errno = saved;
	}
	return result;
}
}
// A sender held between setting its vector and writing its byte: it parks in
// its write until released, which the test does once the idle exit has run.
static thread_local bool hold_write = false;
static std::atomic<bool> held{false}, release{false};
extern "C" ssize_t write(int fd, const void *buffer, size_t size) {
	static auto real = next_symbol<ssize_t (*)(int, const void *, size_t)>("write");
	if (hold_write) {
		hold_write = false;
		held = true;
		while (!release) std::this_thread::yield();
	}
	return real(fd, buffer, size);
}
static ssize_t raw_read(int fd, void *buffer, size_t size) {
	static auto real = next_symbol<ssize_t (*)(int, void *, size_t)>("read");
	return real(fd, buffer, size);
}

// What the kernel provides around the two files.
struct Handled { long long ns; unsigned mask; };
static std::vector<Handled> handled;
static std::atomic<bool> timer_fired{false};
static std::jmp_buf init_escape;
static bool initializing = false;
static int stop_pipe[2] = {-1, -1};
extern "C" {
int uml_ncpus = 2;
void panic(const char *format, ...) {
	// os_init_smp() ends by asking the carrier machinery, which this test
	// does not start, to name CPU0. Everything it sets up has been done by then.
	if (initializing && strstr(format, "fixed carrier")) std::longjmp(init_escape, 1);
	std::fprintf(stderr, "panic: %s\n", format);
	std::abort();
}
void os_warn(const char *format, ...) { std::fprintf(stderr, "warning: %s", format); }
int um_set_signals(int) { return 1; }
int um_get_signals(void) { return 1; }
void um_trace_signals_on(void) {}
void um_trace_signals_off(void) {}
void unblock_signals(void) {}
int darwin_timers_cooperative(void) { return 1; }
void timer_handler(int, void *, struct uml_pt_regs *) { timer_fired = true; }
int darwin_uml_stop_fd(void) { return stop_pipe[0]; }
// A stop request is a byte on the host's stop descriptor; the checkpoint takes it.
void darwin_host_stop_checkpoint(void) {
	char byte;
	if (stop_pipe[0] >= 0) (void)!raw_read(stop_pipe[0], &byte, 1);
}
int darwin_task_shutdown_requested(void) { return 0; }
int darwin_reserve_embedded_thread(void) { return 0; }
void darwin_release_embedded_thread(void) {}
void darwin_timer_kernel_checkpoint(void) {}
int darwin_irq_idle_begin(int) { return 0; }
void darwin_irq_idle_end(void) {}
// The dispatch before the wait is a call's first in a cycle, the one after the
// wait its second. A result of DARWIN_IRQ_HANDLED says it collected an edge.
static std::atomic<int> dispatch_calls{0};
static int first_dispatch_result = 1;
int darwin_irq_dispatch(int, unsigned long long, struct uml_pt_regs *) {
	return dispatch_calls++ == 0 ? first_dispatch_result : 1;
}
int os_set_fd_block(int fd, int blocking) {
	int flags = fcntl(fd, F_GETFL);
	return fcntl(fd, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}
int os_set_exec_close(int fd) { return fcntl(fd, F_SETFD, FD_CLOEXEC); }
void set_sigstack(void *, int) {}
void new_thread(void *, jmp_buf *, void (*)(void)) {}
void new_thread_handler(void) {}
void uml_finishsetup(void) {}
void uml_start_secondary(void *) {}
// The idle exit hands the vectors it found to the kernel here, after it took
// them from the pending mask.
void uml_ipi_handler_mask(unsigned mask) {
	handled.push_back({os_nsecs(), mask});
	if (after_exchange) {
		auto hook = std::move(after_exchange);
		after_exchange = nullptr;
		hook();
	}
}
}

struct Cycle {
	double ms;
	int polls, reads, kevents, waits, selects, sigmasks;
	bool timer;
	unsigned mask;
	Wait wait;
};
static Cycle run_cycle() {
	handled.clear();
	timer_fired = false;
	polls = reads = selects = sigmasks = kevents = waits = 0;
	dispatch_calls = 0;
	long long start = os_nsecs();
	tracked = true;
	os_idle_sleep();
	tracked = false;
	Cycle result{(double)(os_nsecs() - start) / 1e6, polls, reads, kevents, waits, selects, sigmasks, timer_fired, 0, last_wait};
	for (const Handled &entry : handled) result.mask |= entry.mask;
	return result;
}
// One idle cycle with a clock deadline `deadline_ms` away.
static Cycle cycle(double deadline_ms) {
	os_timer_one_shot(0, (unsigned long long)(deadline_ms * 1e6));
	return run_cycle();
}
// One idle cycle with no clock deadline, as in a suspend or with nothing queued.
static Cycle cycle_without_deadline() {
	os_timer_disable(0);
	return run_cycle();
}
// Woken by the clock only: nothing drained, nothing handled, and the one
// kevent() that armed the timer and waited was the whole cycle.
static bool quiet(const Cycle &c, double deadline_ms) {
	return c.timer && c.polls == 0 && c.reads == 0 && c.mask == 0 && c.ms >= deadline_ms * 0.5 &&
	       c.kevents == 1 && c.waits == 1 && c.selects == 0 && c.sigmasks == 0 && c.wait.changes == 1 &&
	       c.wait.timer_add && !c.wait.timeout;
}
// Whatever ended the wait, it cost one kevent() and no other host call but
// the counted ones.
static bool one_wait(const Cycle &c) { return c.kevents == 1 && c.waits == 1 && c.selects == 0 && c.sigmasks == 0; }
static void sleep_us(long us) { timespec t{us / 1000000, (us % 1000000) * 1000L}; nanosleep(&t, nullptr); }
// A cycle with a clock deadline that an IPI sent 2 ms into it ends. A host that
// stalls the sender past half the deadline lets the clock end the wait, which is
// not the case under test: the late vector is collected and the cycle is tried
// again. With the sender on time the clock may win only because the host held
// this thread, and then the wait reported the pipe and made no poll; a poll
// there means the byte, written long before, was not in the wait's result.
static Cycle cycle_ended_by_ipi(double deadline_ms, unsigned vector) {
	for (int attempt = 0; attempt < 20; attempt++) {
		std::atomic<long long> sent{0};
		long long start = os_nsecs();
		std::thread sender([vector, &sent] { sleep_us(2000); os_send_ipi(0, vector); sent = os_nsecs(); });
		Cycle c = cycle(deadline_ms);
		sender.join();
		if (c.mask == 1u << vector && !c.timer) return c;
		assert(c.polls == 0 || sent - start >= (long long)(deadline_ms * 5e5));
		(void)cycle(2);
		(void)cycle(2);
	}
	assert(!"an IPI ends a wait before the clock does");
	return Cycle{};
}

int main() {
	assert(pipe(stop_pipe) == 0 && os_set_fd_block(stop_pipe[0], 0) == 0);
	initializing = true;
	if (setjmp(init_escape) == 0) os_init_smp();
	initializing = false;
	assert(os_timer_create() == 0);
	pthread_t self = pthread_self();

	// The first wait creates the CPU's queue; later ones only use it.
	cycle(2);

	// A wake by the clock is one kevent() that adds the timer and waits. It
	// finds the pipe empty and, with no vector pending, looks at it no further.
	for (int i = 0; i < 20; i++) {
		Cycle c = cycle(2);
		if (i == 0)
			std::printf("deadline wake: %.2f ms, kevents %d, reads %d, polls %d, other %d\n", c.ms, c.kevents, c.reads, c.polls, c.selects + c.sigmasks);
		assert(quiet(c, 2));
	}
	// A dispatch before the wait that ran the handlers of an edge may have made
	// a task runnable on this CPU, and the idle loop reads the flag that says
	// so only when the wait returns: the wait must not sleep. One that found
	// nothing leaves the wait to sleep until its deadline.
	first_dispatch_result = DARWIN_IRQ_HANDLED;
	{
		Cycle c = cycle(300);
		std::printf("handled edge before the wait: %.2f ms\n", c.ms);
		assert(c.ms < 150 && !c.timer);
	}
	first_dispatch_result = 1;
	assert(quiet(cycle(2), 2));
	{
		Cycle c = cycle(100);
		assert(c.timer && c.ms >= 50);
	}
	// A CPU that comes back online keeps its queue: still one call.
	assert(os_timer_create() == 0);
	assert(quiet(cycle(2), 2));

	// So does a wake by the host's stop descriptor, which is not the pipe.
	assert(write(stop_pipe[1], "s", 1) == 1);
	{
		Cycle c = cycle(5000);
		std::printf("stop descriptor wake: %.2f ms, kevents %d, reads %d, polls %d, other %d\n", c.ms, c.kevents, c.reads, c.polls, c.selects + c.sigmasks);
		assert(c.ms < 2500 && !c.timer && c.polls == 0 && c.reads == 0 && c.mask == 0 && one_wait(c));
		assert(quiet(cycle(2), 2));
	}

	// A vector sent before the wait: the wait must not sleep, reads the byte
	// once with no poll and hands the vector over once.
	assert(os_send_ipi(0, 3) == 0);
	{
		Cycle c = cycle(5000);
		std::printf("vector sent before the wait: %.2f ms, kevents %d, reads %d, polls %d, other %d\n", c.ms, c.kevents, c.reads, c.polls, c.selects + c.sigmasks);
		assert(c.ms < 2500 && c.mask == 1u << 3 && c.polls == 0 && c.reads == 1 && one_wait(c));
		assert(quiet(cycle(2), 2));
	}
	// A pending vector which has no byte behind it (a local one) ends the wait
	// at once as well; the wait found the pipe empty, so the poll that hands
	// the vector over drains it, once, and finds nothing.
	os_raise_local_ipi(11);
	{
		Cycle c = cycle(5000);
		std::printf("local vector: %.2f ms, kevents %d, reads %d, polls %d, other %d\n", c.ms, c.kevents, c.reads, c.polls, c.selects + c.sigmasks);
		assert(c.ms < 2500 && c.mask == 1u << 11 && c.polls == 1 && c.reads == 0 && one_wait(c));
		assert(quiet(cycle(2), 2));
	}

	// A vector sent during the wait ends it, and the byte is read.
	{
		std::thread sender([] { sleep_us(20000); os_send_ipi(0, 5); });
		Cycle c = cycle(5000);
		sender.join();
		std::printf("vector sent during the wait: %.2f ms, kevents %d, reads %d, polls %d, other %d\n", c.ms, c.kevents, c.reads, c.polls, c.selects + c.sigmasks);
		assert(c.ms < 2500 && c.mask == 1u << 5 && c.polls == 0 && c.reads == 1 && one_wait(c));
		assert(quiet(cycle(2), 2));
	}

	// A vector sent after the wait looked at the pipe and before the exit took
	// the pending mask is not lost: the exit hands it over, and drains its byte
	// with the poll that finds the vector pending, so nothing is left behind.
	after_wait = [] { assert(os_send_ipi(0, 7) == 0); };
	{
		Cycle c = cycle(2);
		std::printf("sent after the wait: mask %#x, polls %d, reads %d\n", c.mask, c.polls, c.reads);
		assert(c.mask == 1u << 7 && c.polls == 2 && c.reads == 1 && one_wait(c));
		assert(quiet(cycle(2), 2));
	}
	// A sender held between its vector and its byte until the exit has taken
	// the mask: the vector is handed over, and the byte which follows ends the
	// next wait at once and is read there: one spurious cycle, then quiet.
	{
		std::thread sender([] { hold_write = true; os_send_ipi(0, 6); });
		for (int spin = 0; spin < 5000 && !held; spin++) sleep_us(1000);
		assert(held);
		Cycle c = cycle(2);
		assert(c.mask == 1u << 6 && c.polls == 1 && c.reads == 0 && one_wait(c));
		release = true;
		sender.join();
		Cycle spurious = cycle(5000);
		std::printf("byte after the exit: spurious cycle %.2f ms, kevents %d, polls %d, reads %d\n", spurious.ms, spurious.kevents, spurious.polls, spurious.reads);
		assert(spurious.ms < 2500 && spurious.mask == 0 && !spurious.timer && spurious.reads == 1 && spurious.polls == 0 && one_wait(spurious));
		assert(quiet(cycle(2), 2));
	}
	// A vector sent after the exit took the mask is not lost either: the next
	// wait finds it pending and the byte readable.
	after_exchange = [] { assert(os_send_ipi(0, 9) == 0); };
	os_raise_local_ipi(1);
	{
		Cycle c = cycle(5000);
		assert(c.ms < 2500 && c.mask == 1u << 1);
		Cycle next = cycle(5000);
		assert(next.ms < 2500 && next.mask == 1u << 9 && next.reads == 1 && next.polls == 0);
		assert(quiet(cycle(2), 2));
	}

	// A signal ends the wait with an error, which leaves the result set
	// undefined, so that cycle drains as it always did.
	{
		std::atomic<bool> finished{false};
		std::thread sender([&] { while (!finished) { pthread_kill(self, SIGUSR1); sleep_us(5000); } });
		Cycle c = cycle(5000);
		finished = true;
		sender.join();
		std::printf("signal: %.2f ms, kevents %d, polls %d, reads %d\n", c.ms, c.kevents, c.polls, c.reads);
		assert(c.ms < 2500 && c.polls == 1 && c.mask == 0 && !c.timer && one_wait(c));
		assert(quiet(cycle(2), 2));
	}
	// An SME run's carrier keeps the IPI signal blocked through its wait, so
	// the pipe alone ends it; a signal sent meanwhile stays pending and ends
	// no wait, since the call has no mask to open for it.
	{
		sigset_t only, previous;
		sigemptyset(&only);
		sigaddset(&only, SIGUSR1);
		assert(pthread_sigmask(SIG_BLOCK, &only, &previous) == 0);
		std::thread sender([self] { sleep_us(20000); os_send_ipi(0, 6); pthread_kill(self, SIGUSR1); });
		Cycle c = cycle(5000);
		sender.join();
		std::printf("signal blocked: %.2f ms, kevents %d, reads %d, polls %d\n", c.ms, c.kevents, c.reads, c.polls);
		assert(c.ms < 2500 && c.mask == 1u << 6 && !c.timer && c.polls == 0 && c.reads == 1 && one_wait(c));
		Cycle later = cycle(30);
		assert(later.ms >= 25 && quiet(later, 30));
		assert(pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0);
	}
	// Even with a byte in the pipe: the wait that failed drains it in full, with
	// the vector it stands for handed over once.
	assert(os_send_ipi(0, 4) == 0);
	inject_eintr = true;
	{
		Cycle c = cycle(5000);
		std::printf("failed wait with a byte: kevents %d, polls %d, reads %d\n", c.kevents, c.polls, c.reads);
		assert(c.mask == 1u << 4 && c.polls == 2 && c.reads == 1 && one_wait(c));
		assert(quiet(cycle(2), 2));
	}

	// An IPI ends a wait and leaves its timer in the queue. The timer fires
	// while nobody waits; a delivery that nobody collected must not end the
	// next wait, whose own deadline is later.
	{
		Cycle c = cycle_ended_by_ipi(100, 2);
		assert(one_wait(c));
		sleep_us(150000);
		Cycle later = cycle(80);
		std::printf("stale timer: next wait %.2f ms for a deadline of 80, kevents %d, reads %d, polls %d\n", later.ms, later.kevents, later.reads, later.polls);
		assert(later.ms >= 70 && quiet(later, 80));
	}
	// An IPI-woken wait, then one with no deadline at all: only an IPI ends it.
	// The timer left in the queue is deleted by the call that waits, whether
	// it has fired or not, and the wait after that has nothing to delete.
	for (int fired = 0; fired < 2; fired++) {
		Cycle c = cycle_ended_by_ipi(100, 2);
		assert(one_wait(c));
		if (fired) sleep_us(150000);
		std::thread late([] { sleep_us(120000); os_send_ipi(0, 8); });
		Cycle idle = cycle_without_deadline();
		late.join();
		std::printf("no deadline after an IPI wake (timer %s): %.2f ms, kevents %d, changes %d, delete %d\n", fired ? "fired" : "armed", idle.ms, idle.kevents, idle.wait.changes, idle.wait.timer_delete);
		assert(idle.ms >= 100 && idle.mask == 1u << 8 && !idle.timer && one_wait(idle));
		assert(idle.wait.changes == 1 && idle.wait.timer_delete && !idle.wait.timeout);
		std::thread again([] { sleep_us(20000); os_send_ipi(0, 8); });
		Cycle next = cycle_without_deadline();
		again.join();
		assert(next.mask == 1u << 8 && next.ms >= 15 && one_wait(next) && next.wait.changes == 0 && !next.wait.timeout);
		assert(quiet(cycle(2), 2));
	}

	// Sends at every offset against a wait which ends by the clock about then:
	// each is handed over promptly, whichever side of the wait it lands on, and
	// costs at most one spurious cycle, so that a lost wake (which would cost
	// the whole deadline) and a pile of stale bytes both show.
	{
		std::atomic<int> request{0};
		std::atomic<bool> stop{false};
		std::atomic<long> offset_ns{0};
		std::thread sender([&] {
			int served = 0;
			while (!stop) {
				if (request.load() == served) { std::this_thread::yield(); continue; }
				served = request.load();
				for (long long until = os_nsecs() + offset_ns; os_nsecs() < until;) {}
				os_send_ipi(0, 1 + served % 8);
			}
		});
		int spurious = 0, rounds = 400;
		unsigned seed = 7;
		for (int round = 1; round <= rounds; round++) {
			seed = seed * 1103515245u + 12345u;
			offset_ns = (long)((seed >> 16) % 300) * 1000;
			unsigned want = 1u << (1 + round % 8);
			long long start = os_nsecs();
			request = round;
			int cycles = 0;
			unsigned got = 0;
			// The first wait ends about when the send lands, by the clock.
			double deadline = (double)((seed >> 5) % 400) / 1000;
			while (!(got & want)) {
				Cycle c = cycle(deadline);
				deadline = 1000;
				got |= c.mask;
				assert(++cycles <= 3);
				// A wake by neither the clock nor a vector: a stale byte.
				if (!c.mask && !c.timer) spurious++;
			}
			assert(os_nsecs() - start < 500000000);
		}
		stop = true;
		sender.join();
		std::printf("%d sends against the wait: %d spurious cycles\n", rounds, spurious);
		assert(spurious <= rounds);
		// Whatever was left behind is gone after at most one more cycle.
		Cycle c = cycle(2);
		if (c.mask || !c.timer) c = cycle(2);
		assert(quiet(c, 2));
	}

	// The host refuses the deadline timer: the call that added it returns at
	// once with the error, and the waits after it time out instead, a little
	// late, with no change list. This leaves the CPU in that state for good.
	// A refused change ends the call before it looks at the pipe, so the exit
	// does not take the pipe for empty: a byte left with no vector behind it,
	// as a sender held between its vector and its byte leaves, is drained there.
	inject_timer_error = true;
	{
		held = false;
		release = false;
		std::thread sender([] { hold_write = true; os_send_ipi(0, 6); });
		for (int spin = 0; spin < 5000 && !held; spin++) sleep_us(1000);
		assert(held);
		inject_timer_error = false;
		assert((cycle(2).mask & (1u << 6)) != 0);
		release = true;
		sender.join();
		inject_timer_error = true;
		Cycle refused = cycle(300);
		std::printf("refused timer: first wait %.2f ms, polls %d, reads %d\n", refused.ms, refused.polls, refused.reads);
		assert(refused.ms < 150 && !refused.timer && refused.mask == 0 && one_wait(refused));
		assert(refused.polls >= 1 && refused.reads == 1);
		Cycle c = cycle(300);
		std::printf("refused timer: next wait %.2f ms, kevents %d, changes %d\n", c.ms, c.kevents, c.wait.changes);
		assert(c.timer && c.ms >= 299 && c.ms < 3000 && one_wait(c) && c.wait.changes == 0 && c.wait.timeout);
		assert(c.polls == 0 && c.reads == 0 && c.mask == 0);
		// A pending vector still wins over the timeout.
		os_raise_local_ipi(12);
		Cycle vector = cycle(5000);
		assert(vector.ms < 2500 && vector.mask == 1u << 12 && one_wait(vector));
	}
	inject_timer_error = false;
	std::puts("ipi drain ok");
	return 0;
}
