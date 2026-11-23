// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The kernel's blocking imports, marked (runtime/kernel_waits): each wrapper
 * publishes what the calling thread waits for in its registry slot for the
 * length of the call, makes the system's call and leaves the slot as it was
 * but for the count of marks. A call that cannot block is not marked.
 */
#include "carrier_activity.hpp"
#include "kernel_threads.hpp"
#include "kernel_waits.hpp"
#include "platform/host_wait.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "thread_nap.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/select.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::platform::HostWait;
using uml::runtime::kernel_wait_import;
using uml::testing::nap;
using uml::testing::require;

struct Job final {
	std::function<void()> run;
	std::atomic<mach_port_t> port{MACH_PORT_NULL};
	std::atomic<bool> finished{false};
};

void *job_main(void *argument)
{
	auto *job = static_cast<Job *>(argument);
	job->port.store(mach_thread_self(), std::memory_order_release);
	job->run();
	job->finished.store(true, std::memory_order_release);
	return nullptr;
}

/* What the thread whose port this is says it waits for, or 0. */
std::uint64_t word_of(mach_port_t port)
{
	uml::runtime::KernelThreadSnapshot snapshot{};
	uml::runtime::snapshot_kernel_threads(snapshot);
	for (std::size_t index = 0; index < snapshot.thread_count; ++index)
		if (snapshot.threads[index] == port)
			return snapshot.waits[index];
	return 0;
}

bool within(std::chrono::milliseconds limit, const std::function<bool()> &done)
{
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (!done()) {
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		nap(2000);
	}
	return true;
}

/* Runs `call` on a registered thread and returns its port, once it has one. */
struct Running final {
	Job job;
	pthread_t thread{};

	explicit Running(std::function<void()> call)
	{
		job.run = std::move(call);
		require(uml::runtime::kernel_pthread_create(&thread, nullptr, job_main,
			&job) == 0);
		require(within(std::chrono::seconds{3}, [this] {
			return job.port.load(std::memory_order_acquire) != MACH_PORT_NULL;
		}));
	}

	[[nodiscard]] mach_port_t port() const
	{
		return job.port.load(std::memory_order_acquire);
	}

	void join()
	{
		require(pthread_join(thread, nullptr) == 0);
	}
};

/* A blocking call is marked with `kind` while it blocks, and the thread
 * leaves the call with no mark but one more in its count. */
void marks_while_blocked(HostWait kind, const std::function<void()> &call,
	const std::function<void()> &release)
{
	Running running{call};
	require(within(std::chrono::seconds{3}, [&] {
		return uml::platform::wait_kind(word_of(running.port())) == kind;
	}), "the call was never seen waiting");
	const auto during = word_of(running.port());
	require(uml::platform::wait_sequence(during) >= 1);
	release();
	require(within(std::chrono::seconds{3}, [&] {
		return running.job.finished.load(std::memory_order_acquire);
	}), "the call never ended");
	running.join();
	const auto after = word_of(running.port());
	require(uml::platform::wait_kind(after) == HostWait::none &&
		uml::platform::wait_sequence(after) ==
		uml::platform::wait_sequence(during));
}

template<typename Function>
Function import_of(const char *name)
{
	void *const wrapper = kernel_wait_import(name);
	require(wrapper != nullptr, name);
	return reinterpret_cast<Function>(wrapper);
}

void blocking_calls_are_marked_for_as_long_as_they_block()
{
	using Read = ssize_t (*)(int, void *, size_t);
	using Poll = int (*)(pollfd *, nfds_t, int);
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	using Recvfrom = ssize_t (*)(int, void *, size_t, int, sockaddr *,
		socklen_t *);
	using CondWait = int (*)(pthread_cond_t *, pthread_mutex_t *);
	using Kevent = int (*)(int, const struct kevent *, int, struct kevent *, int,
		const timespec *);

	int pipe_fds[2];
	require(pipe(pipe_fds) == 0);
	const auto wake = [&] { (void)!write(pipe_fds[1], "x", 1); };

	/* The same wrapper under the names the kernel uses for it. */
	require(kernel_wait_import("read") == kernel_wait_import("_read"));
	require(kernel_wait_import("pselect$DARWIN_EXTSN") ==
		kernel_wait_import("_pselect$DARWIN_EXTSN"));

	marks_while_blocked(HostWait::receive, [&] {
		char byte;
		(void)import_of<Read>("_read")(pipe_fds[0], &byte, 1);
	}, wake);

	marks_while_blocked(HostWait::answer, [&] {
		pollfd descriptor{pipe_fds[0], POLLIN, 0};
		(void)import_of<Poll>("poll")(&descriptor, 1, -1);
	}, wake);
	/* A poll leaves what it waited for in the pipe. */
	char leftover;
	(void)!read(pipe_fds[0], &leftover, 1);

	marks_while_blocked(HostWait::idle, [&] {
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(pipe_fds[0], &readable);
		(void)import_of<Pselect>("_pselect$DARWIN_EXTSN")(pipe_fds[0] + 1,
			&readable, nullptr, nullptr, nullptr, nullptr);
	}, wake);
	(void)!read(pipe_fds[0], &leftover, 1);

	/* A vCPU's idle wait: a kevent() that adds its deadline timer in the
	 * change list and has no timeout of its own. */
	const int queue = kqueue();
	require(queue >= 0);
	struct kevent watch;
	EV_SET(&watch, static_cast<uintptr_t>(pipe_fds[0]), EVFILT_READ, EV_ADD, 0,
		0, nullptr);
	require(kevent(queue, &watch, 1, nullptr, 0, nullptr) == 0);
	marks_while_blocked(HostWait::idle, [&] {
		struct kevent timer;
		EV_SET(&timer, 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
			NOTE_NSECONDS | NOTE_CRITICAL, 60'000'000'000, nullptr);
		struct kevent result[2];
		require(import_of<Kevent>("kevent")(queue, &timer, 1, result, 2,
			nullptr) == 1);
	}, wake);
	(void)!read(pipe_fds[0], &leftover, 1);
	close(queue);

	int pair[2];
	require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	marks_while_blocked(HostWait::receive, [&] {
		char byte;
		(void)import_of<Recvfrom>("recvfrom")(pair[0], &byte, 1, 0, nullptr,
			nullptr);
	}, [&] { (void)!write(pair[1], "y", 1); });

	/* A condition wait is how a parked vCPU idles. */
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
	static std::atomic<bool> signalled{false};
	signalled = false;
	marks_while_blocked(HostWait::idle, [&] {
		pthread_mutex_lock(&mutex);
		while (!signalled.load())
			(void)import_of<CondWait>("pthread_cond_wait")(&condition, &mutex);
		pthread_mutex_unlock(&mutex);
	}, [&] {
		pthread_mutex_lock(&mutex);
		signalled = true;
		pthread_cond_broadcast(&condition);
		pthread_mutex_unlock(&mutex);
	});
	close(pair[0]);
	close(pair[1]);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
}

/* A call that waits for nothing, or cannot be seen waiting, still counts one
 * mark, except an immediate poll or select. */
void other_calls_are_marked_and_immediate_ones_are_not()
{
	using Pread = ssize_t (*)(int, void *, size_t, off_t);
	using Pwrite = ssize_t (*)(int, const void *, size_t, off_t);
	using Fsync = int (*)(int);
	using Poll = int (*)(pollfd *, nfds_t, int);
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	using Kevent = int (*)(int, const struct kevent *, int, struct kevent *, int,
		const timespec *);

	const uml::testing::ScratchSubdirectory directory{"uml-kernel-waits.XXXXXX"};
	const std::string path = (directory.path() / "file").string();
	const int file = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	require(file >= 0);
	(void)unlink(path.c_str());

	std::atomic<std::uint64_t> sequence_after_marked{0};
	std::atomic<std::uint64_t> sequence_after_immediate{0};
	std::atomic<std::uint64_t> sequence_before{0};
	Running running{[&] {
		const auto word = [] {
			return uml::platform::current_wait_cell->word();
		};
		sequence_before = uml::platform::wait_sequence(word());
		char data[16] = "0123456789abcde";
		require(import_of<Pwrite>("pwrite")(file, data, 16, 0) == 16);
		char back[16];
		require(import_of<Pread>("pread")(file, back, 16, 0) == 16);
		require(std::memcmp(data, back, 16) == 0);
		require(import_of<Fsync>("fsync")(file) == 0);
		sequence_after_marked = uml::platform::wait_sequence(word());
		pollfd descriptor{file, POLLIN, 0};
		require(import_of<Poll>("poll")(&descriptor, 1, 0) >= 0);
		const timespec none{0, 0};
		require(import_of<Pselect>("pselect")(0, nullptr, nullptr, nullptr,
			&none, nullptr) == 0);
		/* A kevent that only changes the queue, and one that polls it. */
		const int queue = kqueue();
		require(queue >= 0);
		struct kevent timer;
		struct kevent result[1];
		EV_SET(&timer, 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
			NOTE_NSECONDS | NOTE_CRITICAL, 60'000'000'000, nullptr);
		require(import_of<Kevent>("kevent")(queue, &timer, 1, nullptr, 0,
			nullptr) == 0);
		require(import_of<Kevent>("kevent")(queue, nullptr, 0, result, 1,
			&none) == 0);
		(void)close(queue);
		sequence_after_immediate = uml::platform::wait_sequence(word());
	}};
	require(within(std::chrono::seconds{3}, [&] {
		return running.job.finished.load(std::memory_order_acquire);
	}));
	running.join();
	require(sequence_after_marked == sequence_before + 3,
		"pwrite, pread and fsync are one mark each");
	require(sequence_after_immediate == sequence_after_marked,
		"an immediate poll or select, or a kevent that cannot wait, was marked");
	(void)close(file);
}

void calls_the_loader_does_not_hand_over_are_not_wrapped()
{
	require(kernel_wait_import("malloc") == nullptr);
	require(kernel_wait_import("write") == nullptr);
	require(kernel_wait_import("recvmsg") == nullptr);
	require(kernel_wait_import("no_such_call") == nullptr);
	require(kernel_wait_import(nullptr) == nullptr);
}

/* A thread the registry does not list marks nothing and is not harmed. */
void a_thread_without_a_cell_marks_nothing()
{
	using Read = ssize_t (*)(int, void *, size_t);
	require(uml::platform::current_wait_cell == nullptr);
	int fds[2];
	require(pipe(fds) == 0);
	(void)!write(fds[1], "z", 1);
	char byte = 0;
	require(import_of<Read>("read")(fds[0], &byte, 1) == 1 && byte == 'z');
	close(fds[0]);
	close(fds[1]);
}

/* The call that names a thread tells the activity record the stall watch
 * sleeps by (runtime/carrier_activity) that a vCPU's carrier exists, and its
 * idle waits are then reported there; the kernel's other threads are not. */
void a_carrier_is_reported_by_its_name_and_its_idle_waits()
{
	using SetName = int (*)(const char *);
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	require(kernel_wait_import("pthread_setname_np") != nullptr);
	require(kernel_wait_import("_pthread_setname_np") ==
		kernel_wait_import("pthread_setname_np"));
	require(uml::runtime::carriers_outside_idle() == 0 &&
		uml::runtime::carriers_reporting() == 0);

	int pipe_fds[2];
	require(pipe(pipe_fds) == 0);
	std::atomic<bool> named{false};
	std::atomic<std::uint32_t> after_naming{0};
	std::atomic<std::uint32_t> after_waking{0};
	Running carrier{[&] {
		const auto set_name = import_of<SetName>("pthread_setname_np");
		require(set_name("uml-vcpu-5-carrier") == 0);
		char name[64]{};
		require(pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 &&
			std::strcmp(name, "uml-vcpu-5-carrier") == 0, "the name is not set");
		after_naming = uml::runtime::carriers_outside_idle();
		named = true;
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(pipe_fds[0], &readable);
		(void)import_of<Pselect>("pselect")(pipe_fds[0] + 1, &readable, nullptr,
			nullptr, nullptr, nullptr);
		after_waking = uml::runtime::carriers_outside_idle();
	}};
	require(within(std::chrono::seconds{3}, [&] { return named.load(); }));
	/* It starts outside an idle wait, enters one, and is outside again after. */
	require(after_naming == 1U << 5);
	require(uml::runtime::carriers_reporting() == 1U << 5);
	require(within(std::chrono::seconds{3}, [&] {
		return uml::platform::wait_kind(word_of(carrier.port())) == HostWait::idle;
	}));
	require(within(std::chrono::seconds{3}, [&] {
		return uml::runtime::carriers_outside_idle() == 0;
	}), "a carrier in its idle wait is counted outside it");
	(void)!write(pipe_fds[1], "x", 1);
	require(within(std::chrono::seconds{3}, [&] {
		return carrier.job.finished.load(std::memory_order_acquire);
	}));
	carrier.join();
	require(after_waking == 1U << 5);
	/* A thread that ends reports no more. */
	require(uml::runtime::carriers_outside_idle() == 0 &&
		uml::runtime::carriers_reporting() == 0);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
}

/* The kernel starts a notification thread and an alarm helper beside its
 * carriers; they wait in calls of their own, and the idle waits among them
 * must not count, whatever the thread is named. */
void other_threads_of_the_kernel_are_not_carriers()
{
	using SetName = int (*)(const char *);
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	for (const char *name : {"uml-irq-notify", "uml-rtc-alarm",
		     "uml-vcpu-16-carrier", "uml-vcpu-x-carrier", "uml-vcpu--carrier"}) {
		int pipe_fds[2];
		require(pipe(pipe_fds) == 0);
		std::atomic<bool> waiting{false};
		Running thread{[&] {
			require(import_of<SetName>("pthread_setname_np")(name) == 0);
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(pipe_fds[0], &readable);
			waiting = true;
			(void)import_of<Pselect>("pselect")(pipe_fds[0] + 1, &readable,
				nullptr, nullptr, nullptr, nullptr);
		}};
		require(within(std::chrono::seconds{3}, [&] {
			return waiting.load() && uml::platform::wait_kind(
				word_of(thread.port())) == HostWait::idle;
		}));
		require(uml::runtime::carriers_outside_idle() == 0 &&
			uml::runtime::carriers_reporting() == 0, name);
		(void)!write(pipe_fds[1], "x", 1);
		thread.join();
		require(uml::runtime::carriers_outside_idle() == 0 &&
			uml::runtime::carriers_reporting() == 0, name);
		close(pipe_fds[0]);
		close(pipe_fds[1]);
	}
}

/* ---- What the kernel's idle kevent reports to the stall watch. ----
 *
 * The kernel's vCPU waits in one kevent() whose one-shot timer is in the
 * change list and which has no timeout. The wrapper counts the carrier as idle
 * with the limit that timer (or the call's timeout, whichever ends first)
 * gives, which the watch's own timer follows: 50 ms past the deadline, once
 * every carrier is idle. A carrier the system does not run stays in its wait
 * past its deadline; the tests hold one with thread_suspend to see that. */

using WatchClock = std::chrono::steady_clock;
constexpr unsigned idle_vcpu = 3;
constexpr unsigned idle_bit = 1u << idle_vcpu;

struct Woken final {
	unsigned bits{};
	double ms{};
};

/* Waits as the watch does, for at most `limit_ms`. */
Woken watch_wakes(unsigned limit_ms, WatchClock::time_point since)
{
	const std::uint64_t timeout = static_cast<std::uint64_t>(limit_ms) * 1'000'000;
	Woken woken;
	woken.bits = uml::runtime::watch_queue_wait(&timeout);
	woken.ms = std::chrono::duration<double, std::milli>(
		WatchClock::now() - since).count();
	return woken;
}

void drain_watch()
{
	for (;;) {
		const std::uint64_t none = 0;
		if (uml::runtime::watch_queue_wait(&none) == 0)
			return;
	}
}

/* The process's watch queue, empty, for the length of a case. */
struct WatchScope final {
	WatchScope()
	{
		require(uml::runtime::carriers_outside_idle() == 0 &&
			uml::runtime::carriers_reporting() == 0, "carriers left behind");
		require(uml::runtime::watch_queue_open(), "the watch's queue");
		drain_watch();
	}

	~WatchScope()
	{
		uml::runtime::watch_queue_close();
	}
};

struct timespec_ms final {
	timespec value;
	explicit timespec_ms(long milliseconds)
		: value{milliseconds / 1000, (milliseconds % 1000) * 1'000'000L} {}
};

struct kevent timer_change(std::uint16_t flags, std::uint32_t fflags,
	std::int64_t data)
{
	struct kevent change;
	EV_SET(&change, 0, EVFILT_TIMER, flags, fflags, data, nullptr);
	return change;
}

/* The kernel's own timer change: EV_ADD | EV_ONESHOT, NOTE_NSECONDS |
 * NOTE_CRITICAL, the time left in nanoseconds. */
struct kevent kernel_timer(long milliseconds)
{
	return timer_change(EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL,
		static_cast<std::int64_t>(milliseconds) * 1'000'000);
}

/* A thread named `name` that waits in one kevent() call: its queue holds a
 * pipe's read end (a byte ends the wait), and the call is made with `changes`
 * and `timeout`. */
class KeventWait final {
public:
	KeventWait(const char *name, std::vector<struct kevent> changes,
		const timespec *timeout, bool timer_in_queue = false)
		: changes_{std::move(changes)}
	{
		using Kevent = int (*)(int, const struct kevent *, int, struct kevent *,
			int, const timespec *);
		using SetName = int (*)(const char *);
		require(pipe(pipe_) == 0);
		queue_ = kqueue();
		require(queue_ >= 0);
		struct kevent watch;
		EV_SET(&watch, static_cast<uintptr_t>(pipe_[0]), EVFILT_READ, EV_ADD, 0, 0,
			nullptr);
		require(kevent(queue_, &watch, 1, nullptr, 0, nullptr) == 0);
		if (timer_in_queue) {
			const struct kevent far = kernel_timer(60'000);
			require(kevent(queue_, &far, 1, nullptr, 0, nullptr) == 0);
		}
		timeout_ = timeout != nullptr ? *timeout : timespec{};
		const bool has_timeout = timeout != nullptr;
		carrier_ = std::make_unique<Running>([=, this] {
			require(import_of<SetName>("pthread_setname_np")(name) == 0);
			struct kevent result[2];
			returned_ = import_of<Kevent>("kevent")(queue_, changes_.data(),
				static_cast<int>(changes_.size()), result, 2,
				has_timeout ? &timeout_ : nullptr);
			outside_after_ = uml::runtime::carriers_outside_idle();
		});
	}

	KeventWait(const KeventWait &) = delete;
	KeventWait &operator=(const KeventWait &) = delete;

	~KeventWait()
	{
		finish();
		close(queue_);
		close(pipe_[0]);
		close(pipe_[1]);
	}

	/* The call is marked idle and, for a carrier, counted inside its wait. */
	void wait_until_blocked(bool counted = true)
	{
		require(within(std::chrono::seconds{3}, [&] {
			return uml::platform::wait_kind(word_of(carrier_->port())) ==
				HostWait::idle && (!counted ||
				uml::runtime::carriers_outside_idle() == 0);
		}), "the call was never seen in its idle wait");
	}

	/* Keeps the thread from returning, as a system that does not run it. */
	void hold()
	{
		require(thread_suspend(carrier_->port()) == KERN_SUCCESS);
		held_ = true;
	}

	[[nodiscard]] bool returned() const
	{
		return carrier_->job.finished.load(std::memory_order_acquire);
	}

	/* Ends the call by a byte on the pipe and joins the thread. */
	void finish()
	{
		if (done_)
			return;
		done_ = true;
		if (held_)
			require(thread_resume(carrier_->port()) == KERN_SUCCESS);
		(void)!write(pipe_[1], "x", 1);
		require(within(std::chrono::seconds{3}, [&] { return returned(); }),
			"the call never ended");
		carrier_->join();
	}

	[[nodiscard]] int result() const { return returned_.load(); }
	[[nodiscard]] std::uint32_t outside_after() const { return outside_after_.load(); }

private:
	int pipe_[2]{};
	int queue_{-1};
	timespec timeout_{};
	std::vector<struct kevent> changes_;
	std::atomic<int> returned_{-99};
	std::atomic<std::uint32_t> outside_after_{~0u};
	std::unique_ptr<Running> carrier_;
	bool held_{};
	bool done_{};
};

/* The kernel's wait is counted idle for as long as it blocks, with the limit
 * its timer gives, and outside again after. */
void a_kernel_wait_is_idle_for_the_watch_until_its_deadline()
{
	const WatchScope scope;
	const auto began = WatchClock::now();
	KeventWait wait{"uml-vcpu-3-carrier", {kernel_timer(700)}, nullptr};
	wait.wait_until_blocked();
	drain_watch();
	wait.hold();
	/* The timer is the carrier's 700 ms and 50: a deadline the call is late for
	 * is found; a wait with no limit would remove the timer and never wake. */
	const Woken woken = watch_wakes(3000, began);
	require(woken.bits == uml::runtime::watch_woken_by_timer,
		"the deadline of the kernel's wait was lost");
	require(woken.ms >= 740, "the watch woke before the deadline and 50 ms: " +
		std::to_string(woken.ms));
	require(!wait.returned(), "the carrier left its wait before it was held");
	require(uml::runtime::carriers_outside_idle() == 0);
	wait.finish();
	require(wait.result() >= 1);
	/* And it is outside its wait again after. */
	require(wait.outside_after() == idle_bit);
}

/* A timer and a timeout in one call: the earlier ends the wait. */
void the_earlier_of_the_timer_and_the_timeout_is_the_limit()
{
	const WatchScope scope;
	const timespec_ms soon{500};
	const auto began = WatchClock::now();
	KeventWait wait{"uml-vcpu-3-carrier", {kernel_timer(30'000)}, &soon.value};
	wait.wait_until_blocked();
	drain_watch();
	wait.hold();
	Woken woken = watch_wakes(3000, began);
	require(woken.bits == uml::runtime::watch_woken_by_timer &&
		woken.ms >= 540 && woken.ms < 2500,
		"the timeout, earlier than the timer, was not the limit: " +
		std::to_string(woken.ms));
	wait.finish();
}

/* A timer the host refused leaves a call with its timeout only. */
void a_timeout_alone_is_the_limit()
{
	const WatchScope scope;
	const timespec_ms soon{500};
	const auto began = WatchClock::now();
	KeventWait wait{"uml-vcpu-3-carrier", {}, &soon.value};
	wait.wait_until_blocked();
	drain_watch();
	wait.hold();
	const Woken woken = watch_wakes(3000, began);
	require(woken.bits == uml::runtime::watch_woken_by_timer && woken.ms >= 540,
		"a timeout alone was not the limit: " + std::to_string(woken.ms));
	wait.finish();
}

/* A call that deletes the timer (the untimed and the suspended wait) has no
 * limit: the watch's timer is removed, and a carrier asleep for as long as it
 * is idle wakes nothing. */
void a_call_that_deletes_the_timer_has_no_limit()
{
	const WatchScope scope;
	KeventWait wait{"uml-vcpu-3-carrier",
		{timer_change(EV_DELETE, 0, 0)}, nullptr, true};
	wait.wait_until_blocked();
	drain_watch();
	const auto began = WatchClock::now();
	require(watch_wakes(800, began).bits == 0,
		"an untimed wait left the watch's timer set");
	require(uml::runtime::carriers_outside_idle() == 0);
	wait.finish();
}

/* A timer of any shape but the kernel's is not read: the carrier stays
 * counted outside its wait, which the watch keeps sampling. */
void a_timer_of_another_shape_leaves_the_carrier_counted_outside()
{
	timespec now{};
	clock_gettime(CLOCK_REALTIME, &now);
	const std::int64_t later = (static_cast<std::int64_t>(now.tv_sec) + 60) *
		1'000'000'000 + now.tv_nsec;
	const struct kevent shapes[] = {
		timer_change(EV_ADD | EV_ONESHOT, NOTE_ABSOLUTE | NOTE_NSECONDS, later),
		timer_change(EV_ADD | EV_ONESHOT, NOTE_USECONDS, 60'000'000),
		timer_change(EV_ADD | EV_ONESHOT, NOTE_SECONDS, 60),
		timer_change(EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_MACH_CONTINUOUS_TIME,
			60'000'000'000),
	};
	for (const struct kevent &shape : shapes) {
		const WatchScope scope;
		KeventWait wait{"uml-vcpu-3-carrier", {shape}, nullptr};
		wait.wait_until_blocked(false);
		nap(150'000);
		require(uml::runtime::carriers_outside_idle() == idle_bit,
			"a timer the wrapper cannot read was counted as idle");
		wait.finish();
	}
}

/* The notification thread waits in the same call and is no carrier. */
void the_notification_thread_in_the_same_call_changes_nothing()
{
	const WatchScope scope;
	KeventWait wait{"uml-irq-notify", {kernel_timer(500)}, nullptr};
	wait.wait_until_blocked(false);
	require(uml::runtime::carriers_outside_idle() == 0 &&
		uml::runtime::carriers_reporting() == 0);
	require(watch_wakes(800, WatchClock::now()).bits == 0,
		"a thread that is no carrier armed the watch");
	wait.finish();
}

/* The trigger (a change list, no event wanted) and the idle poll are no
 * wait: neither is marked nor counted. */
void a_kevent_that_waits_for_nothing_is_not_counted()
{
	using Kevent = int (*)(int, const struct kevent *, int, struct kevent *, int,
		const timespec *);
	using SetName = int (*)(const char *);
	const WatchScope scope;
	const int queue = kqueue();
	require(queue >= 0);
	std::atomic<bool> unchanged{false};
	std::atomic<std::uint32_t> outside{0};
	Running carrier{[&] {
		require(import_of<SetName>("pthread_setname_np")("uml-vcpu-3-carrier") == 0);
		const auto before = uml::platform::wait_sequence(word_of(mach_thread_self()));
		const struct kevent change = kernel_timer(60'000);
		require(import_of<Kevent>("kevent")(queue, &change, 1, nullptr, 0,
			nullptr) == 0);
		struct kevent result[2];
		const timespec none{0, 0};
		require(import_of<Kevent>("kevent")(queue, nullptr, 0, result, 2,
			&none) == 0);
		outside = uml::runtime::carriers_outside_idle();
		unchanged = uml::platform::wait_sequence(
			word_of(mach_thread_self())) == before;
	}};
	require(within(std::chrono::seconds{3}, [&] {
		return carrier.job.finished.load(std::memory_order_acquire);
	}));
	carrier.join();
	close(queue);
	require(unchanged.load(), "a kevent that waits for nothing was marked");
	require(outside == idle_bit, "a kevent that waits for nothing was counted");
}

} // namespace

/* A signal handler's marked write inside a wait it interrupted gives the
 * thread back to that wait, as a new sighting of it. */
void a_mark_inside_another_gives_the_thread_back_to_the_outer_wait()
{
	using uml::platform::ScopedHostWait;
	using uml::platform::WaitCell;
	using uml::platform::wait_kind;
	using uml::platform::wait_sequence;
	WaitCell cell;
	uml::platform::current_wait_cell = &cell;
	{
		const ScopedHostWait outer{HostWait::idle};
		require(wait_kind(cell.word()) == HostWait::idle);
		const std::uint64_t outer_sequence = wait_sequence(cell.word());
		{
			const ScopedHostWait inner{HostWait::send};
			require(wait_kind(cell.word()) == HostWait::send);
			{
				const ScopedHostWait innermost{HostWait::disk_write};
				require(wait_kind(cell.word()) == HostWait::disk_write);
			}
			require(wait_kind(cell.word()) == HostWait::send);
		}
		require(wait_kind(cell.word()) == HostWait::idle);
		/* The sequence only goes forward, or a reader would take a later wait
		 * for the one it saw. */
		require(wait_sequence(cell.word()) == outer_sequence + 2);
	}
	require(wait_kind(cell.word()) == HostWait::none);
	uml::platform::current_wait_cell = nullptr;
}

int main()
{
	a_mark_inside_another_gives_the_thread_back_to_the_outer_wait();
	a_thread_without_a_cell_marks_nothing();
	calls_the_loader_does_not_hand_over_are_not_wrapped();
	blocking_calls_are_marked_for_as_long_as_they_block();
	a_carrier_is_reported_by_its_name_and_its_idle_waits();
	other_threads_of_the_kernel_are_not_carriers();
	other_calls_are_marked_and_immediate_ones_are_not();
	a_kernel_wait_is_idle_for_the_watch_until_its_deadline();
	the_earlier_of_the_timer_and_the_timeout_is_the_limit();
	a_timeout_alone_is_the_limit();
	a_call_that_deletes_the_timer_has_no_limit();
	a_timer_of_another_shape_leaves_the_carrier_counted_outside();
	the_notification_thread_in_the_same_call_changes_nothing();
	a_kevent_that_waits_for_nothing_is_not_counted();
	std::puts("kernel waits tests passed");
	return 0;
}
