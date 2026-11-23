// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The counters reporter starts a thread with every host signal blocked, so
 * that the guest's interrupts never reach it, and gives the caller's mask back
 * afterwards. The caller is a translating thread: a start that fails (the
 * path or the thread's own state cannot be allocated) must leave it with the
 * mask it had, or its timer and wake signals stay blocked for good.
 *
 * Each run is one process, as the start happens once, and fails one step
 * the start makes, named by the argument:
 *   1   the path's copy, which is made before the signals are blocked;
 *   2   the thread's state allocation, with the signals blocked;
 *   3   pthread_create(), which answers EAGAIN, with the signals blocked.
 * The caller starts with SIGUSR1 blocked and the rest not, and its mask must
 * be exactly that afterwards: the one it had, not an empty or a full one.
 */
#include "allocation_failure.hpp"
#include "code_holders.hpp"
#include "require.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>

namespace {
bool create_refused;
}

/* The row that refuses the thread: the reporter's std::thread asks for it
 * here and gets no thread. */
extern "C" int pthread_create(pthread_t *, const pthread_attr_t *,
	void *(*)(void *), void *)
{
	create_refused = true;
	return EAGAIN;
}

int main(int argc, char **argv)
{
	using uml::testing::require;
	require(argc == 2);
	const auto failing = std::strtoul(argv[1], nullptr, 10);
	require(failing >= 1 && failing <= 3);

	/* Longer than a short string keeps, so that copying it allocates, and
	 * naming nothing, so that a reporter that does start finds no file. */
	require(setenv("DARWIN_UML_CODE_HOLDER_COUNTERS",
		"/nonexistent-directory/code-holder-counters-reporter-test", 1) == 0);

	sigset_t before;
	sigemptyset(&before);
	sigaddset(&before, SIGUSR1);
	require(pthread_sigmask(SIG_SETMASK, &before, nullptr) == 0);

	if (failing <= 2)
		uml::testing::fail_after_allocations(failing - 1);
	uml::memory::code_holders::report_counters_once();
	const bool consumed = failing == 3 ?
		create_refused : uml::testing::last_allocation_failure().observed;
	(void)uml::testing::cancel_pending_allocation_failure();

	sigset_t after;
	require(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
	int differences = 0;
	for (int signal = 1; signal < NSIG; ++signal) {
		if (sigismember(&before, signal) != sigismember(&after, signal)) {
			std::fprintf(stderr,
				"failed reporter start changed the mask of signal %d\n",
				signal);
			++differences;
		}
	}
	require(consumed);
	return differences == 0 ? 0 : 1;
}
