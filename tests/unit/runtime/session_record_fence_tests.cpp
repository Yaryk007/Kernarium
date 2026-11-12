// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The session record's memory checkpoint thread against the thread fence. The
 * thread is named uml-*, so every quiesce must hold it, and the fence defers
 * a thread inside libsystem_c: one that waits there between its checkpoints
 * (nanosleep, usleep) is deferred on every pass and no hold can succeed. The
 * checkpoints must also keep their cadence.
 */
#include "scratch_directory.hpp"
#include "session_record.hpp"
#include "thread_fence.hpp"
#include "thread_nap.hpp"
#include "require.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

namespace {

using uml::runtime::ThreadFence;
using uml::testing::nap;
using uml::testing::require;

/* The time of the checkpoint the record's first slot holds, or 0. */
[[nodiscard]] long checkpoint_time(const std::string &path)
{
	long at = 0;
	if (std::FILE *file = std::fopen(path.c_str(), "r"); file != nullptr) {
		char line[64]{};
		if (std::fgets(line, sizeof line, file) != nullptr &&
		    std::fgets(line, sizeof line, file) != nullptr)
			(void)std::sscanf(line, "memory at %ld", &at);
		(void)std::fclose(file);
	}
	return at;
}

/* Holds the fence, or stops the test saying why it could not: the only thread
 * it can find in this process is the checkpoint thread. */
void hold_or_stop(ThreadFence &fence, const char *when)
{
	if (fence.hold(1000))
		return;
	const ThreadFence::Report &report = fence.report();
	std::fprintf(stderr, "%s: failure %d after %u passes, %u deferred\n",
		when, report.failure, report.passes, report.deferred);
	require(false, "the checkpoint thread could not be held");
}

void the_checkpoint_thread_is_held_at_any_moment_of_its_wait()
{
	const uml::testing::ScratchSubdirectory directory {
		"uml-session-fence.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	require(uml::runtime::begin_session_record(path, {}), "begin the record");
	ThreadFence fence;
	/* The thread names itself as it starts. */
	for (int attempt = 0; attempt < 300; ++attempt) {
		hold_or_stop(fence, "waiting for the checkpoint thread");
		const bool found = fence.report().named != 0;
		fence.release();
		if (found)
			break;
		nap(10000);
	}
	/* The interval is longer than the budget, so a thread that waits where
	 * the fence defers it cannot be caught at the end of its wait. */
	for (const unsigned offset_ms : {0U, 700U, 1300U, 1900U}) {
		nap(offset_ms * 1000U);
		hold_or_stop(fence, "holding the checkpoint thread");
		require(fence.report().named == 1 && fence.report().held == 1,
			"the checkpoint thread is the one thread the fence holds");
		fence.release();
	}
	uml::runtime::reset_session_record();
}

void the_checkpoints_keep_their_cadence()
{
	const uml::testing::ScratchSubdirectory directory {
		"uml-session-cadence.XXXXXX"};
	const std::string path = (directory.path() / "session").string();
	uml::runtime::reset_session_record();
	require(uml::runtime::begin_session_record(path, {}), "begin the record");
	std::set<long> times;
	for (int look = 0; look < 160 && times.size() < 4; ++look) {
		times.insert(checkpoint_time(path));
		nap(50000);
	}
	times.erase(0);
	require(times.size() >= 3, "a record checkpoints about every " +
		std::to_string(uml::runtime::memory_checkpoint_interval_ms) +
		" ms, and this one wrote " + std::to_string(times.size()) +
		" times in eight seconds");
	uml::runtime::reset_session_record();
}

} // namespace

int main()
{
	the_checkpoint_thread_is_held_at_any_moment_of_its_wait();
	the_checkpoints_keep_their_cadence();
	std::puts("session record fence tests passed");
	return 0;
}
