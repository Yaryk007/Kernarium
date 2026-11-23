// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A host whose sound worker stops for a while.
 *
 * A test runner inserts this library into the launcher it starts
 * (DYLD_INSERT_LIBRARIES) and names a directory in UML_AUDIO_HOLD_DIRECTORY.
 * When the file "hold" appears in that directory, a thread of the process
 * removes it, suspends the thread that serves the Virtio sound device, and
 * resumes it after the number of milliseconds the file held (one decimal
 * number), which is what a busy host does to the worker that the device's
 * capture timeline runs on while the guest itself keeps running. The worker
 * is found by its name: the in-process service that hosts the device names its
 * thread "uml-audio". A runner writes the request to another name and renames
 * it to "hold", so the library never reads a file half written.
 *
 * After each hold the thread appends a line "<n> <held nanoseconds> <begin>
 * <threads>" to the file "holds": the time from the suspend call to the end of
 * the resume call, when the suspend call began (both on CLOCK_MONOTONIC), and
 * how many threads had the worker's name. Without exactly one, nothing is
 * suspended and the held time is 0, so a runner can tell a hold that did not
 * happen from one that did. The library does nothing in any process that is
 * not a launcher, or without the directory, and nothing in the product loads
 * it.
 */
#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr char worker_name[] = "uml-audio";
constexpr char thread_name[] = "uml-audio-hold";
constexpr unsigned poll_milliseconds = 5;
constexpr unsigned longest_hold_milliseconds = 60000;

const std::string *directory = nullptr;

std::uint64_t now_ns()
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC);
}

void sleep_ns(std::uint64_t nanoseconds)
{
	timespec time{};
	time.tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
	time.tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);
	while (nanosleep(&time, &time) != 0) {
	}
}

/* The Mach port of the thread named `worker_name`, or MACH_PORT_NULL unless
 * exactly one thread has the name. `found` counts the threads that do. The
 * caller owns the right it returns. */
mach_port_t find_worker(unsigned &found)
{
	found = 0;
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	mach_port_t worker = MACH_PORT_NULL;
	for (mach_msg_type_number_t index = 0; index != count; ++index) {
		thread_extended_info_data_t info{};
		mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(threads[index], THREAD_EXTENDED_INFO,
				reinterpret_cast<thread_info_t>(&info), &size) ==
			    KERN_SUCCESS &&
		    std::strcmp(info.pth_name, worker_name) == 0) {
			++found;
			if (worker == MACH_PORT_NULL) {
				worker = threads[index];
				continue;
			}
		}
		mach_port_deallocate(mach_task_self(), threads[index]);
	}
	vm_deallocate(mach_task_self(),
		      reinterpret_cast<vm_address_t>(threads),
		      count * sizeof(threads[0]));
	if (found != 1 && worker != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), worker);
		worker = MACH_PORT_NULL;
	}
	return worker;
}

bool take_request(const std::string &path, unsigned &hold_ms)
{
	std::FILE *file = std::fopen(path.c_str(), "r");
	if (file == nullptr)
		return false;
	const int fields = std::fscanf(file, "%u", &hold_ms);
	std::fclose(file);
	(void)std::remove(path.c_str());
	return fields == 1 && hold_ms != 0 &&
	       hold_ms <= longest_hold_milliseconds;
}

void serve()
{
	pthread_setname_np(thread_name);
	const std::string request = *directory + "/hold";
	const std::string log = *directory + "/holds";
	unsigned long count = 0;
	for (;;) {
		unsigned hold_ms = 0;
		if (!take_request(request, hold_ms)) {
			sleep_ns(poll_milliseconds * 1000000ULL);
			continue;
		}
		unsigned threads = 0;
		const mach_port_t worker = find_worker(threads);
		/* Nothing between the suspend and the resume may allocate or take
		 * a lock the worker could hold. */
		const std::uint64_t begin = now_ns();
		std::uint64_t held = 0;
		if (worker != MACH_PORT_NULL) {
			const kern_return_t suspended = thread_suspend(worker);
			sleep_ns(hold_ms * 1000000ULL);
			if (suspended == KERN_SUCCESS) {
				(void)thread_resume(worker);
				held = now_ns() - begin;
			}
			mach_port_deallocate(mach_task_self(), worker);
		}
		if (std::FILE *file = std::fopen(log.c_str(), "a")) {
			std::fprintf(file, "%lu %llu %llu %u\n", ++count,
				     static_cast<unsigned long long>(held),
				     static_cast<unsigned long long>(begin),
				     threads);
			std::fclose(file);
		}
	}
}

__attribute__((constructor)) void start()
{
	const char *const target = getprogname();
	const char *const where = std::getenv("UML_AUDIO_HOLD_DIRECTORY");
	if (where == nullptr || target == nullptr ||
	    std::strncmp(target, "uml-darwin", 10) != 0)
		return;
	directory = new std::string(where);
	/* The launcher blocks the guest timer on the threads it makes, and so
	 * does this one: ITIMER_REAL must reach the designated CPU only. */
	sigset_t all{};
	sigfillset(&all);
	sigset_t previous{};
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	try {
		std::thread{serve}.detach();
	} catch (...) {
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

} // namespace
