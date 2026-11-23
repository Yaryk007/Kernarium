// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A host that does not run a guest vCPU for a while.
 *
 * A test runner inserts this library into the launcher it starts
 * (DYLD_INSERT_LIBRARIES) and names a directory in UML_CPU_HOLD_DIRECTORY.
 * While the file "hold" exists in that directory, a thread of the process
 * suspends one vCPU's carrier thread for a while, resumes it, and repeats,
 * which is what a loaded or throttled host does to a carrier. The file holds
 * three decimal numbers:
 *
 *   <vcpu> <hold milliseconds> <period milliseconds>
 *
 * A hold starts every period, as long as the file is there. After each one
 * the thread appends a line "<n> <held nanoseconds> <begin>" to the file
 * "holds": the time from the suspend call to the end of the resume call, and
 * when the suspend call began, both on CLOCK_MONOTONIC, so a runner can check
 * that the carrier really stood still for as long as it asked, and which of
 * its own measurements the hold overlapped. The library does nothing in any process that
 * is not a launcher, or without the directory, and nothing in the product
 * loads it.
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

constexpr char thread_name[] = "uml-cpu-hold";
constexpr unsigned poll_milliseconds = 5;

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

/* The Mach port of the carrier of `vcpu`, or MACH_PORT_NULL. The caller owns
 * the right it returns. */
mach_port_t find_carrier(int vcpu)
{
	char wanted[40];
	std::snprintf(wanted, sizeof(wanted), "uml-vcpu-%d-carrier", vcpu);
	thread_act_array_t threads = nullptr;
	mach_msg_type_number_t count = 0;
	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	mach_port_t found = MACH_PORT_NULL;
	for (mach_msg_type_number_t index = 0; index != count; ++index) {
		thread_extended_info_data_t info{};
		mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
		if (found == MACH_PORT_NULL &&
		    thread_info(threads[index], THREAD_EXTENDED_INFO,
				reinterpret_cast<thread_info_t>(&info), &size) ==
			    KERN_SUCCESS &&
		    std::strcmp(info.pth_name, wanted) == 0) {
			found = threads[index];
			continue;
		}
		mach_port_deallocate(mach_task_self(), threads[index]);
	}
	vm_deallocate(mach_task_self(),
		      reinterpret_cast<vm_address_t>(threads),
		      count * sizeof(threads[0]));
	return found;
}

bool read_request(const std::string &path, int &vcpu, unsigned &hold_ms,
		  unsigned &period_ms)
{
	std::FILE *file = std::fopen(path.c_str(), "r");
	if (file == nullptr)
		return false;
	const int fields = std::fscanf(file, "%d %u %u", &vcpu, &hold_ms,
				       &period_ms);
	std::fclose(file);
	return fields == 3 && vcpu >= 0 && hold_ms != 0 && period_ms >= hold_ms;
}

void serve()
{
	pthread_setname_np(thread_name);
	const std::string request = *directory + "/hold";
	const std::string log = *directory + "/holds";
	unsigned long count = 0;
	std::uint64_t next = 0;
	for (;;) {
		int vcpu = 0;
		unsigned hold_ms = 0;
		unsigned period_ms = 0;
		if (!read_request(request, vcpu, hold_ms, period_ms)) {
			next = 0;
			sleep_ns(poll_milliseconds * 1000000ULL);
			continue;
		}
		const std::uint64_t period = period_ms * 1000000ULL;
		if (next == 0)
			next = now_ns() + period;
		const std::uint64_t now = now_ns();
		if (now < next) {
			sleep_ns(std::min<std::uint64_t>(
				next - now, poll_milliseconds * 1000000ULL));
			continue;
		}
		next += period;
		const mach_port_t carrier = find_carrier(vcpu);
		if (carrier == MACH_PORT_NULL)
			continue;
		/* Nothing between the suspend and the resume may allocate or take
		 * a lock the carrier could hold. */
		const std::uint64_t begin = now_ns();
		const kern_return_t suspended = thread_suspend(carrier);
		sleep_ns(hold_ms * 1000000ULL);
		if (suspended == KERN_SUCCESS)
			(void)thread_resume(carrier);
		const std::uint64_t held = now_ns() - begin;
		mach_port_deallocate(mach_task_self(), carrier);
		if (suspended != KERN_SUCCESS)
			continue;
		if (std::FILE *file = std::fopen(log.c_str(), "a")) {
			std::fprintf(file, "%lu %llu %llu\n", ++count,
				     static_cast<unsigned long long>(held),
				     static_cast<unsigned long long>(begin));
			std::fclose(file);
		}
	}
}

__attribute__((constructor)) void start()
{
	const char *const target = getprogname();
	const char *const where = std::getenv("UML_CPU_HOLD_DIRECTORY");
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
