// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The stall watch against real threads (runtime/stall_watch): carriers the
 * kernel thread registry lists, named as the kernel names them, spend their
 * time in marked waits; the watch writes what it sees into the stall slot of
 * a session record file. A stall here is a carrier that stays in one marked
 * wait, which needs no load on the host.
 */
#include "carrier_activity.hpp"
#include "kernel_threads.hpp"
#include "kernel_waits.hpp"
#include "platform/host_state_figures.hpp"
#include "platform/host_thermal.hpp"
#include "platform/host_wait.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "session_record.hpp"
#include "stall_record.hpp"
#include "stall_watch.hpp"
#include "thread_fence.hpp"
#include "thread_nap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/* The main queue's ping is a call of dispatch_async_f. This binary defines
 * the function, so the watch's call binds here and not to libdispatch: it
 * counts the callers by thread, answers the ping at once, and the test
 * requires that no thread of the process's own (uml-*), which the thread fence
 * can hold, ever sends one. The watch's notices to its observer queue (a
 * change of the thermal state) bind here too, run at once on the caller's
 * thread, and are not counted as pings. */
namespace {
std::atomic<int> pings_from_held_threads{0};
std::atomic<int> pings_from_other_threads{0};
} // namespace

extern "C" void dispatch_async_f(dispatch_queue_t queue, void *context,
	dispatch_function_t work)
{
	char name[64]{};
	(void)pthread_getname_np(pthread_self(), name, sizeof(name));
	if (std::strncmp(name, "uml-", 4) == 0)
		++pings_from_held_threads;
	else if (queue == dispatch_get_main_queue())
		++pings_from_other_threads;
	work(context);
}

namespace {

using uml::platform::HostThermalLevel;
using uml::platform::HostThermalState;
using uml::runtime::session_memory_slot_bytes;
using uml::runtime::session_stall_slot_bytes;
using uml::testing::nap;
using uml::testing::require;

/* A thermal source the test moves. It counts the readings made from a thread
 * of the process's own (uml-*), which the thread fence can hold: the watch
 * thread must never read it. */
struct Thermal final : uml::platform::HostThermalProvider {
	static inline std::atomic<int> level{0};
	static inline std::atomic<int> processors{6};
	static inline std::atomic<int> from_held_threads{0};
	static inline std::atomic<int> readings{0};
	/* The watch's notice of a change, as the system would call it. */
	static inline std::atomic<void (*)(void *)> notice{nullptr};
	static inline std::atomic<void *> notice_context{nullptr};

	void observe(void (*changed)(void *), void *context) noexcept override
	{
		notice_context = context;
		notice = changed;
	}

	/* Moves the thermal state and says so, from the calling thread. */
	static void change_to(int new_level)
	{
		level = new_level;
		if (void (*const changed)(void *) = notice.load())
			changed(notice_context.load());
	}

	HostThermalState state() noexcept override
	{
		char name[64]{};
		(void)pthread_getname_np(pthread_self(), name, sizeof(name));
		if (std::strncmp(name, "uml-", 4) == 0)
			++from_held_threads;
		++readings;
		HostThermalState result;
		result.level = static_cast<HostThermalLevel>(level.load());
		result.activeProcessors = static_cast<std::uint8_t>(processors.load());
		return result;
	}
};

/* A carrier as the kernel makes one: a registered thread that names itself. */
struct Carrier final {
	pthread_t thread{};
	unsigned cpu{};
	std::function<void()> body;
	std::atomic<bool> stop{false};
	/* Named through the loader's wrapper, as the kernel's carriers are, so
	 * that the activity record knows it; a carrier named directly reports
	 * nothing there and keeps the watch awake. */
	bool reports{};
	/* Not a carrier at all: this name. */
	const char *other_name{};
};

void *carrier_main(void *argument)
{
	auto *carrier = static_cast<Carrier *>(argument);
	char name[32];
	std::snprintf(name, sizeof(name), "uml-vcpu-%u-carrier", carrier->cpu);
	using SetName = int (*)(const char *);
	if (carrier->reports) {
		const auto set_name = reinterpret_cast<SetName>(
			uml::runtime::kernel_wait_import("pthread_setname_np"));
		require(set_name != nullptr);
		(void)set_name(carrier->other_name != nullptr ? carrier->other_name : name);
	} else {
		(void)pthread_setname_np(carrier->other_name != nullptr ?
			carrier->other_name : name);
	}
	carrier->body();
	return nullptr;
}

void start(Carrier &carrier, unsigned cpu, std::function<void()> body,
	bool reports = false)
{
	carrier.cpu = cpu;
	carrier.body = std::move(body);
	carrier.reports = reports;
	require(uml::runtime::kernel_pthread_create(&carrier.thread, nullptr,
		carrier_main, &carrier) == 0);
}

/* A thread of the kernel's that is no carrier. */
void start_other(Carrier &thread, const char *name, std::function<void()> body)
{
	thread.other_name = name;
	start(thread, 0, std::move(body), true);
}

/* The idle wait of a vCPU, through the loader's wrapper: 20 ms at a time. */
void idle_until_stopped(const Carrier &carrier)
{
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	const auto wait = reinterpret_cast<Pselect>(
		uml::runtime::kernel_wait_import("_pselect$DARWIN_EXTSN"));
	require(wait != nullptr);
	while (!carrier.stop.load(std::memory_order_acquire)) {
		const timespec period{0, 20L * 1000L * 1000L};
		(void)wait(0, nullptr, nullptr, nullptr, &period, nullptr);
	}
}

/* The idle wait of a vCPU that wakes now and then: `period_ms` at a time. */
void blink_until_stopped(const Carrier &carrier, long period_ms)
{
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	const auto wait = reinterpret_cast<Pselect>(
		uml::runtime::kernel_wait_import("_pselect$DARWIN_EXTSN"));
	require(wait != nullptr);
	while (!carrier.stop.load(std::memory_order_acquire)) {
		const timespec period{period_ms / 1000, (period_ms % 1000) * 1000L * 1000L};
		(void)wait(0, nullptr, nullptr, nullptr, &period, nullptr);
	}
}

/* An idle wait as the kernel makes it: a kevent() on the vCPU's own queue,
 * whose one-shot timer is in the change list and which has no timeout. With
 * `kernel_shape` false the timer is in microseconds, a shape no caller makes,
 * which the wrapper does not count as idle. */
void kevent_idle_until_stopped(const Carrier &carrier, long period_ms,
	bool kernel_shape)
{
	using Kevent = int (*)(int, const struct kevent *, int, struct kevent *, int,
		const timespec *);
	const auto wait = reinterpret_cast<Kevent>(
		uml::runtime::kernel_wait_import("kevent"));
	require(wait != nullptr);
	const int queue = kqueue();
	require(queue >= 0);
	while (!carrier.stop.load(std::memory_order_acquire)) {
		struct kevent timer;
		if (kernel_shape)
			EV_SET(&timer, 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
				NOTE_NSECONDS | NOTE_CRITICAL, period_ms * 1'000'000, nullptr);
		else
			EV_SET(&timer, 0, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS,
				period_ms * 1000, nullptr);
		struct kevent result[2];
		(void)wait(queue, &timer, 1, result, 2, nullptr);
	}
	close(queue);
}

/* An idle wait that ends when the descriptor is readable. */
void idle_until_readable(int descriptor)
{
	using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
		const sigset_t *);
	const auto wait = reinterpret_cast<Pselect>(
		uml::runtime::kernel_wait_import("_pselect$DARWIN_EXTSN"));
	require(wait != nullptr);
	fd_set readable;
	FD_ZERO(&readable);
	FD_SET(descriptor, &readable);
	(void)wait(descriptor + 1, &readable, nullptr, nullptr, nullptr, nullptr);
}

/* One wait for a device service's answer, as long as the test says. */
void wait_for_a_device_service(int descriptor)
{
	using Poll = int (*)(pollfd *, nfds_t, int);
	const auto wait = reinterpret_cast<Poll>(
		uml::runtime::kernel_wait_import("poll"));
	require(wait != nullptr);
	pollfd entry{descriptor, POLLIN, 0};
	(void)wait(&entry, 1, 10'000);
}

std::string stall_slot(const std::string &path)
{
	std::ifstream file{path, std::ios::binary};
	const std::string content{std::istreambuf_iterator<char>{file},
		std::istreambuf_iterator<char>{}};
	require(content.size() >= session_memory_slot_bytes + session_stall_slot_bytes,
		"the record is shorter than its slots");
	return content.substr(session_memory_slot_bytes, session_stall_slot_bytes);
}

/* The events that name a vCPU. A host so loaded that the watch's own tick runs
 * 150 ms late records an event of the whole process as well, which names none
 * and says nothing of the carriers. */
std::size_t vcpu_events(const std::string &slot)
{
	std::size_t found = 0;
	std::size_t at = 0;
	while ((at = slot.find("\nstall ", at)) != std::string::npos) {
		++at;
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		/* stall SEQ START DUR[+] CAUSES ... */
		std::size_t field = 0;
		std::size_t from = 0;
		for (; field < 4; ++field)
			from = line.find(' ', from) + 1;
		const std::string causes = line.substr(from, line.find(' ', from) - from);
		if (causes.find_first_not_of('.') != std::string::npos)
			++found;
	}
	return found;
}

/* The events whose watch lateness, the eighth field, is at least `late_ms`. */
std::size_t late_events(const std::string &slot, unsigned late_ms)
{
	std::size_t found = 0;
	std::size_t at = 0;
	while ((at = slot.find("\nstall ", at)) != std::string::npos) {
		++at;
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		std::size_t from = 0;
		for (unsigned field = 0; field < 7; ++field)
			from = line.find(' ', from) + 1;
		if (std::strtoul(line.c_str() + from, nullptr, 10) >= late_ms)
			++found;
	}
	return found;
}

/* The first event line of the slot that names `cause` for vCPU `vcpu`, empty
 * for none. The line is "stall SEQ START DUR[+] CAUSES ...", and the causes
 * hold one letter per vCPU, a dot for one not stalled. */
std::string event_of_carrier(const std::string &slot, std::size_t vcpu, char cause)
{
	std::size_t at = 0;
	while ((at = slot.find("\nstall ", at)) != std::string::npos) {
		++at;
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		std::size_t from = 0;
		for (unsigned field = 0; field < 4; ++field)
			from = line.find(' ', from) + 1;
		const std::string causes = line.substr(from, line.find(' ', from) - from);
		if (causes.size() > vcpu && causes[vcpu] == cause)
			return line;
	}
	return {};
}

/* The cores the process used in the seconds before the event of this line, in
 * hundredths (the field after the active core counts); -1 for no line. */
long cores_before_of(const std::string &line)
{
	std::size_t from = 0;
	for (unsigned field = 0; field < 11; ++field) {
		from = line.find(' ', from);
		if (from == std::string::npos)
			return -1;
		++from;
	}
	return std::strtol(line.c_str() + from, nullptr, 10);
}

/* The CPU the whole process has used, in nanoseconds. */
std::uint64_t process_cpu_ns()
{
	rusage usage{};
	require(getrusage(RUSAGE_SELF, &usage) == 0);
	return (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) +
		static_cast<std::uint64_t>(usage.ru_stime.tv_sec)) * 1'000'000'000ULL +
		(static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
		 static_cast<std::uint64_t>(usage.ru_stime.tv_usec)) * 1000ULL;
}

/* The longest of the events that name a vCPU, in milliseconds. On a host
 * loaded enough to starve a busy thread for 100 ms the watch records that
 * truthfully; a sleep wrongly counted as starvation is seconds long. */
unsigned long longest_vcpu_event_ms(const std::string &slot)
{
	unsigned long longest = 0;
	std::size_t at = 0;
	while ((at = slot.find("\nstall ", at)) != std::string::npos) {
		++at;
		const std::size_t end = slot.find('\n', at);
		const std::string line = slot.substr(at, end - at);
		/* stall SEQ START DUR[+] CAUSES ... */
		std::size_t from = 0;
		for (unsigned field = 0; field < 3; ++field)
			from = line.find(' ', from) + 1;
		const unsigned long duration = std::strtoul(line.c_str() + from, nullptr, 10);
		from = line.find(' ', from) + 1;
		const std::string causes = line.substr(from, line.find(' ', from) - from);
		if (causes.find_first_not_of('.') != std::string::npos)
			longest = std::max(longest, duration);
	}
	return longest;
}

/* The count after `word` in the slot's header. */
unsigned long header_count(const std::string &slot, const std::string &word)
{
	const std::size_t at = slot.find(" " + word + " ");
	return at == std::string::npos ? 0 :
		std::strtoul(slot.c_str() + at + word.size() + 2, nullptr, 10);
}

void sleep_ms(unsigned milliseconds)
{
	nap(milliseconds * 1000U);
}

void a_stalled_carrier_is_recorded_and_idle_ones_are_not()
{
	const uml::testing::ScratchSubdirectory directory{"uml-stall-watch.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	require(uml::runtime::begin_session_record(path, {}), "begin the record");
	require(uml::runtime::session_record_open());

	int pipe_fds[2];
	require(pipe(pipe_fds) == 0);
	Carrier idle0;
	Carrier idle1;
	Carrier stalled;
	start(idle0, 0, [&] { idle_until_stopped(idle0); });
	start(idle1, 1, [&] { idle_until_stopped(idle1); });
	Thermal::level = 0;
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr, "a watch without a record");

	/* Only idle carriers: no event, and the watch has seen them in the idle
	 * wait the loader's wrapper marks, which no load on the host changes. */
	sleep_ms(400);
	require(vcpu_events(stall_slot(path)) == 0, "idle carriers stalled:\n" + stall_slot(path));
	require(header_count(stall_slot(path), "idle") >= 1,
		"no carrier was seen in a marked idle wait:\n" + stall_slot(path));

	/* A carrier starts a wait that lasts, and the thermal state and the core
	 * count move. */
	start(stalled, 2, [&] {
		wait_for_a_device_service(pipe_fds[0]);
		idle_until_stopped(stalled);
	});
	sleep_ms(200);
	Thermal::level = 2;
	Thermal::processors = 4;
	sleep_ms(1000);
	const std::string during = stall_slot(path);
	require(vcpu_events(during) == 1, "one event is open:\n" + during);
	require(during.find("+ ..a ") != std::string::npos,
		"the third carrier is stalled on a device service's answer:\n" + during);
	require(during.find("\nthermal ") != std::string::npos &&
		during.find("\npower ") != std::string::npos, "transitions:\n" + during);
	/* What the watch observed is what the session record's checkpoint reads
	 * (platform/host_state_figures.hpp): the same observation, not a second
	 * one. */
	const auto published = uml::platform::publishedHostState();
	require(published && published->thermal.level == HostThermalLevel::serious &&
		published->thermal.activeProcessors == 4,
		"the watch's observation was not published");

	/* The service answers: the event closes. */
	require(write(pipe_fds[1], "x", 1) == 1);
	sleep_ms(500);
	const std::string after = stall_slot(path);
	require(vcpu_events(after) == 1 && after.find("+ ..a ") == std::string::npos &&
		after.find(" ..a ") != std::string::npos,
		"the event is closed:\n" + after);

	/* Ending the watch leaves a valid slot of exactly its size, and no state
	 * that is observed no more. */
	watch.reset();
	require(!uml::platform::publishedHostState(),
		"a host state was published after the watch ended");
	const std::string last = stall_slot(path);
	require(last.size() == session_stall_slot_bytes && last.back() == '\n');
	/* The thermal source was read, and never from a thread the fence holds. */
	require(Thermal::readings.load() > 0, "the thermal source was never read");
	require(Thermal::from_held_threads.load() == 0,
		"the thermal source was read from a thread of the process's own");
	/* The main queue was asked while the stall was open, and never from a
	 * thread the fence holds. */
	require(pings_from_other_threads.load() > 0,
		"the main queue was never asked during the stall");
	require(pings_from_held_threads.load() == 0,
		"the main queue was asked from a thread of the process's own");

	for (Carrier *carrier : {&idle0, &idle1, &stalled})
		carrier->stop.store(true, std::memory_order_release);
	for (Carrier *carrier : {&idle0, &idle1, &stalled})
		require(pthread_join(carrier->thread, nullptr) == 0);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
	uml::runtime::reset_session_record();
}

/* `reports`: the carrier reports its idle waits, so the watch sleeps while the
 * fence holds it. */
void a_hold_of_the_threads_is_no_stall(bool reports)
{
	const uml::testing::ScratchSubdirectory directory{"uml-stall-fence.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	require(uml::runtime::begin_session_record(path, {}), "begin the record");

	Carrier idle0;
	start(idle0, 0, [&] { idle_until_stopped(idle0); }, reports);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	sleep_ms(300);

	/* The fence holds every thread named uml-*, the watch's included, and the
	 * registered carrier; it wakes late. */
	uml::runtime::ThreadFence fence;
	require(fence.hold(2000), "the watch thread could not be held");
	sleep_ms(600);
	fence.release();
	sleep_ms(400);

	/* No event names a vCPU, and none is as late as the hold was; a loaded
	 * host may still make a short lateness of its own. */
	const std::string slot = stall_slot(path);
	require(vcpu_events(slot) == 0 && late_events(slot, 400) == 0,
		"a hold of the threads was recorded as a stall:\n" + slot);

	watch.reset();
	idle0.stop.store(true, std::memory_order_release);
	require(pthread_join(idle0.thread, nullptr) == 0);
	uml::runtime::reset_session_record();
}

/* ---- A watch that sleeps while every carrier is idle. ---- */

/* A session record in a scratch directory, and a thermal source at rest. */
struct Session final {
	explicit Session(const char *pattern)
		: directory{pattern}, path{(directory.path() / "session").string()}
	{
		uml::runtime::reset_session_record();
		require(uml::runtime::begin_session_record(path, {}), "begin the record");
		Thermal::level = 0;
		Thermal::processors = 6;
		Thermal::notice = nullptr;
	}

	~Session()
	{
		uml::runtime::reset_session_record();
	}

	uml::testing::ScratchSubdirectory directory;
	std::string path;
};

/* The level the newest thermal transition of the slot went to, -1 for none. */
int newest_thermal_level(const std::string &slot)
{
	const std::size_t at = slot.rfind("\nthermal ");
	if (at == std::string::npos)
		return -1;
	const std::size_t end = slot.find('\n', at + 1);
	const std::string line = slot.substr(at + 1, end - at - 1);
	return std::atoi(line.c_str() + line.rfind(' ') + 1);
}

double milliseconds_since(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - start).count();
}

/* Moves the thermal state, as the system's notice does, which wakes a watch
 * that sleeps to record it, and returns how many looks at the carriers the
 * slot says the watch has made: the carriers' idle and unmarked waits it has
 * counted, one for each carrier at each look. The watch makes none while it
 * sleeps, so the count does not move between two calls then. */
unsigned long looks_after(const std::string &path, int thermal_level)
{
	Thermal::change_to(thermal_level);
	const auto began = std::chrono::steady_clock::now();
	for (;;) {
		const std::string slot = stall_slot(path);
		if (newest_thermal_level(slot) == thermal_level)
			return header_count(slot, "idle") + header_count(slot, "unmarked");
		require(milliseconds_since(began) < 5000,
			"a change of the thermal state was not recorded:\n" + slot);
		sleep_ms(5);
	}
}

/* Whether the watch's thread has named itself asleep: it has made its last
 * look and makes no more until a timer of the carriers wakes it. */
bool the_watch_is_asleep()
{
	thread_act_array_t threads;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return false;
	bool asleep = false;
	for (mach_msg_type_number_t index = 0; index < count; ++index) {
		char name[64]{};
		const pthread_t thread = pthread_from_mach_thread_np(threads[index]);
		if (thread != nullptr)
			(void)pthread_getname_np(thread, name, sizeof(name));
		asleep = asleep ||
			std::strcmp(name, uml::runtime::stall_watch_asleep_name) == 0;
		(void)mach_port_deallocate(mach_task_self(), threads[index]);
	}
	(void)vm_deallocate(mach_task_self(),
		reinterpret_cast<vm_address_t>(threads),
		count * sizeof(thread_act_t));
	return asleep;
}

/* Returns once the watch has gone to sleep, however long a loaded host takes
 * to give it the two quiet looks it needs; the bound only ends a watch that
 * never sleeps. */
void wait_until_the_watch_sleeps()
{
	const auto began = std::chrono::steady_clock::now();
	while (!the_watch_is_asleep()) {
		require(milliseconds_since(began) < 60000, "the watch did not go to sleep");
		sleep_ms(5);
	}
}

void the_watch_sleeps_while_every_carrier_is_idle()
{
	Session session{"uml-stall-sleep.XXXXXX"};
	int quiet[2];
	require(pipe(quiet) == 0);
	Carrier idle0;
	Carrier idle1;
	Carrier notifier;
	/* Two carriers that wake now and then, as a guest's timers wake them for
	 * a moment, and the kernel's notification thread: it has a wait cell
	 * like every thread the kernel starts, waits in a call that is not marked,
	 * and is no carrier. */
	start(idle0, 0, [&] { blink_until_stopped(idle0, 30); }, true);
	start(idle1, 1, [&] { blink_until_stopped(idle1, 45); }, true);
	start_other(notifier, "uml-irq-notify", [&] {
		char byte;
		(void)!read(quiet[0], &byte, 1);
	});
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr, "a watch without a record");
	/* Two quiet looks, and it sleeps. */
	wait_until_the_watch_sleeps();
	const unsigned long first = looks_after(session.path, 1);
	require(first >= 2, "the watch made no look at the idle carriers");
	/* A second of carriers waking every 30 and 45 ms: no look at all. */
	sleep_ms(1000);
	Thermal::processors = 4;
	const unsigned long second = looks_after(session.path, 2);
	require(second == first, "the watch looked at carriers that were idle: " +
		std::to_string(first) + " then " + std::to_string(second));
	const std::string slot = stall_slot(session.path);
	require(slot.find("\npower ") != std::string::npos,
		"a change that woke the watch was not recorded:\n" + slot);
	require(vcpu_events(slot) == 0, "idle carriers stalled:\n" + slot);

	const auto stopping = std::chrono::steady_clock::now();
	watch.reset();
	require(milliseconds_since(stopping) < 2000, "a sleeping watch did not stop");
	require(Thermal::from_held_threads.load() == 0,
		"the thermal source was read from a thread of the process's own");
	require(pings_from_held_threads.load() == 0);

	for (Carrier *carrier : {&idle0, &idle1})
		carrier->stop.store(true, std::memory_order_release);
	(void)!write(quiet[1], "x", 1);
	for (Carrier *carrier : {&idle0, &idle1, &notifier})
		require(pthread_join(carrier->thread, nullptr) == 0);
	close(quiet[0]);
	close(quiet[1]);
}

/* The kernel's own idle wait, a kevent whose deadline is in its change list,
 * is an idle wait to the watch as pselect is: it sleeps through carriers
 * that wake now and then. */
void the_watch_sleeps_while_carriers_wait_in_the_kernels_kevent()
{
	Session session{"uml-stall-kevent.XXXXXX"};
	Carrier idle0;
	Carrier idle1;
	start(idle0, 0, [&] { kevent_idle_until_stopped(idle0, 30, true); }, true);
	start(idle1, 1, [&] { kevent_idle_until_stopped(idle1, 45, true); }, true);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr, "a watch without a record");
	sleep_ms(800);
	const unsigned long first = looks_after(session.path, 1);
	require(first >= 2, "the watch made no look at the idle carriers");
	sleep_ms(1000);
	const unsigned long second = looks_after(session.path, 2);
	require(second == first, "the watch looked at carriers in the kernel's "
		"kevent wait: " + std::to_string(first) + " then " + std::to_string(second));
	require(vcpu_events(stall_slot(session.path)) == 0, "idle carriers stalled");
	watch.reset();
	for (Carrier *carrier : {&idle0, &idle1})
		carrier->stop.store(true, std::memory_order_release);
	for (Carrier *carrier : {&idle0, &idle1})
		require(pthread_join(carrier->thread, nullptr) == 0);
}

/* A timer of any other shape is not read: the carrier stays counted outside
 * its wait and the watch keeps sampling it. */
void a_timer_of_another_shape_keeps_the_watch_looking()
{
	Session session{"uml-stall-shape.XXXXXX"};
	Carrier idle0;
	start(idle0, 0, [&] { kevent_idle_until_stopped(idle0, 30, false); }, true);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	sleep_ms(500);
	const unsigned long first = looks_after(session.path, 1);
	sleep_ms(1000);
	const unsigned long second = looks_after(session.path, 2);
	require(second >= first + 12, "the watch slept for a timer it cannot read: " +
		std::to_string(first) + " then " + std::to_string(second));
	watch.reset();
	idle0.stop.store(true, std::memory_order_release);
	require(pthread_join(idle0.thread, nullptr) == 0);
}

void a_carrier_that_reports_nothing_keeps_the_watch_looking()
{
	Session session{"uml-stall-awake.XXXXXX"};
	Carrier idle0;
	start(idle0, 0, [&] { blink_until_stopped(idle0, 30); }, false);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	sleep_ms(500);
	const unsigned long first = looks_after(session.path, 1);
	sleep_ms(1000);
	const unsigned long second = looks_after(session.path, 2);
	/* 20 looks a second, as before the watch learned to sleep. */
	require(second >= first + 12, "the watch slept for a carrier it cannot hear: " +
		std::to_string(first) + " then " + std::to_string(second));
	watch.reset();
	idle0.stop.store(true, std::memory_order_release);
	require(pthread_join(idle0.thread, nullptr) == 0);
}

void a_wedge_after_a_wake_is_found_within_a_quarter_of_a_second()
{
	Session session{"uml-stall-wedge.XXXXXX"};
	int wake[2];
	int answer[2];
	require(pipe(wake) == 0 && pipe(answer) == 0);
	Carrier idle0;
	Carrier wedger;
	Carrier spinner;
	start(idle0, 0, [&] { blink_until_stopped(idle0, 40); }, true);
	/* A thread of the process's that is no carrier. It naps while the watch
	 * sleeps and keeps a core busy from the host's wake to the event: the
	 * event looks back over the time since the wake, in which the spinner
	 * used a core, and not over the sleep, in which it used none. */
	std::atomic<bool> spinning{false};
	start_other(spinner, "uml-test-spinner", [&] {
		while (!spinner.stop.load(std::memory_order_acquire)) {
			if (!spinning.load(std::memory_order_acquire))
				sleep_ms(1);
		}
	});
	/* Idle until the host wakes it, then waits on a device service that does not
	 * answer. */
	start(wedger, 2, [&] {
		while (!wedger.stop.load(std::memory_order_acquire)) {
			idle_until_readable(wake[0]);
			char byte;
			(void)!read(wake[0], &byte, 1);
			if (wedger.stop.load(std::memory_order_acquire))
				break;
			wait_for_a_device_service(answer[0]);
			(void)!read(answer[0], &byte, 1);
		}
	}, true);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	/* Longer than the five seconds an event looks back over. */
	sleep_ms(5600);
	/* The wedger's own event is the one that is timed: an unrelated starvation
	 * of the idle carrier on a loaded host is not. */
	const std::uint64_t cpu_at_wake = process_cpu_ns();
	const auto woken = std::chrono::steady_clock::now();
	spinning.store(true, std::memory_order_release);
	require(write(wake[1], "w", 1) == 1);
	double found = -1;
	std::string slot;
	std::string line;
	std::uint64_t cpu_at_found = 0;
	while (milliseconds_since(woken) < 5000) {
		slot = stall_slot(session.path);
		line = event_of_carrier(slot, 2, 'a');
		if (!line.empty()) {
			found = milliseconds_since(woken);
			cpu_at_found = process_cpu_ns();
			break;
		}
		sleep_ms(5);
	}
	spinning.store(false, std::memory_order_release);
	require(found >= 0, "a wedge after a wake was never found:\n" + slot);
	std::printf("a wedge after a wake was recorded %.0f ms after the wake\n", found);
	/* The timer 50 ms after the wake, then the 100 ms a stall must last. */
	require(found >= 100, "a wedge was recorded before it had lasted 100 ms: " +
		std::to_string(found) + " ms");
	require(found <= 250, "a wedge after a wake was found late: " +
		std::to_string(found) + " ms");
	require(line.find("+ ") != std::string::npos,
		"the wedger's event has ended:\n" + line);
	/* The watch slept, yet the event says what the process was doing in the
	 * seconds before it: since the wake, when the watch first looked, and not
	 * over the sleep. The spinner used a core from the wake and none during
	 * the sleep, so the figure is the process's use over the span the test
	 * measures from the wake, within a factor of two (and a couple of
	 * hundredths of a core for the rounding of two small figures). A figure
	 * of nothing, and the sleep's average, which is the spinner's use over
	 * five seconds of a nap, both fail it. */
	const long before = cores_before_of(line);
	const double measured = static_cast<double>(cpu_at_found - cpu_at_wake) /
		1'000'000.0 / found * 100.0;
	const std::string figures = "the process's use before the event is " +
		std::to_string(before) + " hundredths of a core; measured over the "
		"wake to the event: " + std::to_string(measured) + "\n" + line;
	std::printf("the process's use before that event: %ld hundredths of a "
		"core, measured from the wake: %.0f\n", before, measured);
	/* A host too loaded to let the spinner run says nothing: run it alone. */
	require(measured >= 20, "the spinner did not get a fifth of a core, rerun "
		"this test alone: " + figures);
	require(before >= 0 && before * 2.0 + 2.0 >= measured &&
		before <= measured * 2.0 + 2.0, figures);

	spinner.stop.store(true, std::memory_order_release);
	wedger.stop.store(true, std::memory_order_release);
	idle0.stop.store(true, std::memory_order_release);
	require(write(answer[1], "a", 1) == 1);
	require(write(wake[1], "w", 1) == 1);
	watch.reset();
	for (Carrier *carrier : {&idle0, &wedger, &spinner})
		require(pthread_join(carrier->thread, nullptr) == 0);
	for (const int descriptor : {wake[0], wake[1], answer[0], answer[1]})
		close(descriptor);
}

/* A carrier that wakes after a sleep of seconds has used a sliver of them,
 * and the watch must not take that for starvation. */
void a_two_second_idle_is_no_starvation()
{
	Session session{"uml-stall-nap.XXXXXX"};
	Carrier sleeper;
	Carrier other;
	start(other, 1, [&] { blink_until_stopped(other, 40); }, true);
	start(sleeper, 0, [&] {
		using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
			const sigset_t *);
		const auto wait = reinterpret_cast<Pselect>(
			uml::runtime::kernel_wait_import("_pselect$DARWIN_EXTSN"));
		const timespec long_idle{2, 200L * 1000L * 1000L};
		(void)wait(0, nullptr, nullptr, nullptr, &long_idle, nullptr);
		/* Awake and busy for a moment, as the guest is when it wakes. */
		const auto busy_until = std::chrono::steady_clock::now() +
			std::chrono::milliseconds{120};
		while (std::chrono::steady_clock::now() < busy_until) {
		}
		blink_until_stopped(sleeper, 40);
	}, true);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	sleep_ms(3500);
	watch.reset();
	const std::string slot = stall_slot(session.path);
	require(longest_vcpu_event_ms(slot) < 1500,
		"a sleep was counted as starvation:\n" + slot);
	for (Carrier *carrier : {&sleeper, &other})
		carrier->stop.store(true, std::memory_order_release);
	for (Carrier *carrier : {&sleeper, &other})
		require(pthread_join(carrier->thread, nullptr) == 0);
}

/* A carrier is due to wake, and the system does not run it: it never clears
 * its idle mark, and the watch hears of it 50 ms after its deadline. */
void a_carrier_that_is_due_and_does_not_run_wakes_the_watch()
{
	Session session{"uml-stall-due.XXXXXX"};
	using Steady = std::chrono::steady_clock;
	constexpr auto limit_ms = std::chrono::milliseconds{1500};
	Carrier due;
	std::atomic<bool> waiting{false};
	std::atomic<Steady::rep> entered{0};
	start(due, 0, [&] {
		const uml::platform::ScopedHostWait mark{uml::platform::HostWait::idle};
		const timespec limit{1, 500L * 1000L * 1000L};
		const uml::runtime::CarrierIdle idle{&limit,
			uml::runtime::CarrierIdle::Limit::relative};
		entered = Steady::now().time_since_epoch().count();
		waiting = true;
		/* Held well past the deadline: the carrier is blocked, not due and
		 * running. */
		sleep_ms(3500);
	}, true);
	/* The carrier is in its wait before the watch exists: the watch sets the
	 * timer for it as it goes to sleep. */
	while (!waiting.load())
		sleep_ms(1);
	const Steady::time_point deadline{Steady::duration{entered.load()} + limit_ms};
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	sleep_ms(600);
	const unsigned long before = looks_after(session.path, 1);
	sleep_ms(400);
	const unsigned long still = looks_after(session.path, 2);
	require(still == before, "the watch looked before the carrier was due");

	/* The first look past the deadline comes with the timer, 50 ms on. The
	 * watch is poked to write its slot (each poke is a reading of the host's
	 * state, no look), and the looks it counts are read from the slot. */
	int level = 1;
	unsigned long counted = still;
	double first_look_ms = -1;
	while (Steady::now() < deadline + std::chrono::milliseconds{1000}) {
		counted = looks_after(session.path, level);
		level = level == 1 ? 2 : 1;
		if (counted > still) {
			first_look_ms = std::chrono::duration<double, std::milli>(
				Steady::now() - deadline).count();
			break;
		}
		sleep_ms(2);
	}
	std::printf("the first look past the deadline was %.0f ms after it\n",
		first_look_ms);
	require(first_look_ms >= 0,
		"the watch did not look for a carrier past its deadline");
	require(first_look_ms >= 40 && first_look_ms <= 300,
		"the first look past the deadline came " + std::to_string(first_look_ms) +
		" ms after it, not within 40 to 300 ms");

	/* And then it sleeps again: the carrier is still blocked in its wait, and a
	 * deadline the watch has looked past must not wake it again. */
	sleep_ms(400);
	const unsigned long later = looks_after(session.path, level);
	require(Steady::now() < deadline + std::chrono::milliseconds{1900},
		"the test ran past the carrier's hold");
	require(later - counted <= 2,
		"the watch looked " + std::to_string(later - counted) +
		" more times in 400 ms while the carrier stayed blocked, "
		"first look " + std::to_string(first_look_ms) + " ms past the deadline");
	/* Two looks after a wake, the first of which measures nothing, before it
	 * sleeps: counted afresh, not carried over from before the sleep. */
	require(later - still >= 2,
		"the watch slept after " + std::to_string(later - still) +
		" look after its wake, not two");
	watch.reset();
	require(pthread_join(due.thread, nullptr) == 0);
}

/* A change of the host's state that no notice announced (the process was
 * suspended when it happened) is recorded when the host asks for a reading. */
void a_requested_reading_records_a_change_no_notice_announced()
{
	Session session{"uml-stall-reading.XXXXXX"};
	Carrier idle0;
	start(idle0, 0, [&] { blink_until_stopped(idle0, 40); }, true);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	/* The watch sleeps, and the state moves with no notice. */
	sleep_ms(600);
	Thermal::level = 2;
	sleep_ms(400);
	require(newest_thermal_level(stall_slot(session.path)) == -1,
		"a change nobody announced was recorded by itself");
	uml::runtime::request_host_state_reading();
	const auto asked = std::chrono::steady_clock::now();
	std::string slot;
	while (milliseconds_since(asked) < 5000) {
		slot = stall_slot(session.path);
		if (newest_thermal_level(slot) == 2)
			break;
		sleep_ms(5);
	}
	require(newest_thermal_level(slot) == 2,
		"a requested reading did not record the change:\n" + slot);
	idle0.stop.store(true, std::memory_order_release);
	watch.reset();
	require(pthread_join(idle0.thread, nullptr) == 0);
	/* With no watch the request does nothing. */
	uml::runtime::request_host_state_reading();
}

/* Wedges found among carriers that come and go. */
void every_wedge_among_waking_carriers_is_found()
{
	Session session{"uml-stall-race.XXXXXX"};
	int wake[2];
	int answer[2];
	require(pipe(wake) == 0 && pipe(answer) == 0);
	Carrier churn1;
	Carrier churn2;
	Carrier wedger;
	const auto churn = [](Carrier &carrier, unsigned seed) {
		using Pselect = int (*)(int, fd_set *, fd_set *, fd_set *, const timespec *,
			const sigset_t *);
		const auto wait = reinterpret_cast<Pselect>(
			uml::runtime::kernel_wait_import("_pselect$DARWIN_EXTSN"));
		std::mt19937 random{seed};
		while (!carrier.stop.load(std::memory_order_acquire)) {
			const timespec period{0, (1L + random() % 30) * 1000L * 1000L};
			(void)wait(0, nullptr, nullptr, nullptr, &period, nullptr);
			nap(random() % 200);
		}
	};
	start(churn1, 0, [&] { churn(churn1, 1); }, true);
	start(churn2, 1, [&] { churn(churn2, 2); }, true);
	start(wedger, 2, [&] {
		while (!wedger.stop.load(std::memory_order_acquire)) {
			idle_until_readable(wake[0]);
			char byte;
			(void)!read(wake[0], &byte, 1);
			if (wedger.stop.load(std::memory_order_acquire))
				break;
			wait_for_a_device_service(answer[0]);
			(void)!read(answer[0], &byte, 1);
		}
	}, true);
	auto watch = uml::runtime::start_stall_watch(std::make_unique<Thermal>());
	require(watch != nullptr);
	std::mt19937 random{0xd0ce};
	for (std::size_t wedge = 1; wedge <= 8; ++wedge) {
		sleep_ms(400 + random() % 500);
		const auto woken = std::chrono::steady_clock::now();
		require(write(wake[1], "w", 1) == 1);
		while (vcpu_events(stall_slot(session.path)) < wedge) {
			require(milliseconds_since(woken) < 3000,
				"wedge " + std::to_string(wedge) + " was not found:\n" +
				stall_slot(session.path));
			sleep_ms(5);
		}
		/* Past the 100 ms a wedge must last, then let it go. */
		sleep_ms(50);
		require(write(answer[1], "a", 1) == 1);
	}
	for (Carrier *carrier : {&churn1, &churn2, &wedger})
		carrier->stop.store(true, std::memory_order_release);
	require(write(answer[1], "a", 1) == 1);
	require(write(wake[1], "w", 1) == 1);
	watch.reset();
	for (Carrier *carrier : {&churn1, &churn2, &wedger})
		require(pthread_join(carrier->thread, nullptr) == 0);
	for (const int descriptor : {wake[0], wake[1], answer[0], answer[1]})
		close(descriptor);
}

/* The record's times are on the clock that counts the system's sleep. */
void the_record_is_on_the_continuous_clock()
{
	const auto continuous = static_cast<long long>(
		clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
	const auto ours = static_cast<long long>(uml::runtime::stall_clock_ns());
	const auto uptime = static_cast<long long>(
		clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
	require(ours - continuous < 50'000'000 && continuous - ours < 50'000'000,
		"the record's clock is not the continuous one");
	/* The continuous clock is the uptime and the time spent asleep. */
	require(ours + 50'000'000 >= uptime);
}

void no_session_means_no_watch()
{
	uml::runtime::reset_session_record();
	require(uml::runtime::start_stall_watch() == nullptr);
}

} // namespace

int main()
{
	no_session_means_no_watch();
	the_record_is_on_the_continuous_clock();
	a_stalled_carrier_is_recorded_and_idle_ones_are_not();
	a_hold_of_the_threads_is_no_stall(false);
	a_hold_of_the_threads_is_no_stall(true);
	the_watch_sleeps_while_every_carrier_is_idle();
	the_watch_sleeps_while_carriers_wait_in_the_kernels_kevent();
	a_timer_of_another_shape_keeps_the_watch_looking();
	a_carrier_that_reports_nothing_keeps_the_watch_looking();
	a_wedge_after_a_wake_is_found_within_a_quarter_of_a_second();
	a_two_second_idle_is_no_starvation();
	a_carrier_that_is_due_and_does_not_run_wakes_the_watch();
	a_requested_reading_records_a_change_no_notice_announced();
	every_wedge_among_waking_carriers_is_found();
	std::puts("stall watch tests passed");
	return 0;
}
