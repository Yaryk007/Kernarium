// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The loop guard of an application that boots a guest again after it
 * restarted itself: a restart less than 15 s after its boot started is early,
 * and so is one before the boot finished where the guest's console shows its
 * init and says when the boot ends. Five restarts in a row within the window
 * end the loop; eight early ones in a row end it too, and a restart that is
 * not early resets both counts.
 */
#include "require.hpp"
#include "restart_loop_guard.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using uml::emulator::BootCompletion;
using uml::emulator::RestartLoopGuard;
using uml::testing::require;

const RestartLoopGuard::Clock::time_point epoch{};

/* Boots at `start` and restarts after `lasted`; returns the guard's answer
 * and leaves the clock at the restart. */
RestartLoopGuard::Clock::time_point restart_after(RestartLoopGuard &guard,
	RestartLoopGuard::Clock::time_point start,
	std::chrono::milliseconds lasted, bool expected_answer,
	BootCompletion completion = BootCompletion::unobserved)
{
	guard.boot_started(start);
	const auto restart = start + lasted;
	require(guard.may_restart(restart, completion) == expected_answer,
		"the guard answered another way than the policy says");
	return restart;
}

void test_the_policy_is_fifteen_seconds_five_and_eight_restarts()
{
	/* Applications and their documentation quote these numbers. */
	require(uml::emulator::early_restart_window == 15s,
		"an early restart is not one under 15 s");
	require(uml::emulator::early_restart_limit == 5U,
		"the loop does not end on the fifth restart within the window");
	require(uml::emulator::unfinished_restart_limit == 8U,
		"the loop does not end on the eighth early restart");
	require(uml::emulator::early_restart_exit_status == 3,
		"the launchers' guard exit status is not 3");
}

void test_a_boot_that_lasts_the_window_is_not_early()
{
	RestartLoopGuard guard;
	for (int index = 0; index != 100; ++index)
		(void)restart_after(guard, epoch + std::chrono::seconds(index * 20),
			15s, true);
	require(guard.early_restarts() == 0U,
		"a boot of 15 s counted as early");
	(void)restart_after(guard, epoch, 14999ms, true);
	require(guard.early_restarts() == 1U,
		"a boot just under 15 s did not count as early");
}

void test_the_fifth_early_restart_in_a_row_ends_the_loop()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (unsigned restart = 1; restart != uml::emulator::early_restart_limit;
	     ++restart) {
		now = restart_after(guard, now, 2s, true);
		require(guard.early_restarts() == restart,
			"the count did not follow the early restarts");
	}
	(void)restart_after(guard, now, 2s, false);
	require(guard.early_restarts() == uml::emulator::early_restart_limit,
		"the ending restart was not counted");
}

void test_a_long_boot_resets_the_count()
{
	RestartLoopGuard guard;
	auto now = epoch;
	/* Four early restarts, a boot that lasted, then four more early ones:
	 * eight early restarts, never five in a row. */
	for (int round = 0; round != 3; ++round) {
		for (int early = 0; early != 4; ++early)
			now = restart_after(guard, now, 1s, true);
		now = restart_after(guard, now, 15s, true);
		require(guard.early_restarts() == 0U,
			"a boot of 15 s did not reset the count");
	}
}

void test_a_guest_that_never_boots_ends_after_five_boots()
{
	RestartLoopGuard guard;
	auto now = epoch;
	unsigned boots = 0;
	for (;;) {
		guard.boot_started(now);
		++boots;
		now += 3s;
		if (!guard.may_restart(now, BootCompletion::unobserved))
			break;
		require(boots < 100U, "a guest that restarts at once never ended");
	}
	require(boots == uml::emulator::early_restart_limit,
		"the loop ended after another number of boots than the limit");
}

void test_the_restart_command_is_printable()
{
	using uml::emulator::printable_restart_command;
	require(printable_restart_command("") == "", "an empty command changed");
	require(printable_restart_command("recovery") == "recovery",
		"a plain command changed");
	require(printable_restart_command("a b-c_d/e.f") == "a b-c_d/e.f",
		"printable ASCII changed");
	/* A newline would end the log line, and a quote would end the command's
	 * own quotes. */
	require(printable_restart_command("a\nb") == "a\\x0ab",
		"a newline was kept");
	require(printable_restart_command("x'); rm") == "x\\'); rm",
		"a quote was kept as it came");
	require(printable_restart_command("back\\slash") == "back\\\\slash",
		"a backslash was kept as it came");
	require(printable_restart_command(std::string_view("a\0b", 3)) == "a\\x00b",
		"a NUL ended the command");
	require(printable_restart_command("\x1b[31m\x7f\xff") ==
		"\\x1b[31m\\x7f\\xff", "a control or high byte was kept");
	/* Whatever it was, a command that holds a quote cannot be taken for
	 * another by a reader that stops at the first unescaped one. */
	const auto hostile = printable_restart_command("recovery'\nguest restarting");
	require(hostile.find('\n') == std::string::npos,
		"the command still holds a line break");
	require(hostile.find("recovery'") == std::string::npos,
		"the command still closes its quotes early");
}

void test_the_retry_policy_is_bounded()
{
	require(uml::emulator::restart_run_retries == 3U,
		"the refused hand-back is not retried three times");
	require(uml::emulator::restart_run_retry_delay == 500ms,
		"the retry delay is not half a second");
}

void test_a_restart_with_no_time_at_all_is_early()
{
	RestartLoopGuard guard;
	guard.boot_started(epoch);
	require(guard.may_restart(epoch, BootCompletion::unobserved),
		"the first early restart was refused");
	require(guard.early_restarts() == 1U, "an instant restart was not early");
}

/* A guest whose console reports its boot's end, restarting every 20 s and
 * never getting there, is the loop the 15 s window alone let through. Android
 * is left to its own boot-loop rescue (five failed boots, a warm reboot, then
 * recovery's prompt) before the host ends the loop at the eighth. */
void test_a_boot_that_never_finished_counts_however_long_it_ran()
{
	RestartLoopGuard guard;
	auto now = epoch;
	unsigned boots = 0;
	for (;;) {
		guard.boot_started(now);
		++boots;
		now += 20s;
		if (!guard.may_restart(now, BootCompletion::unfinished))
			break;
		require(boots < 100U, "a guest that never boots never ended");
	}
	require(boots == uml::emulator::unfinished_restart_limit,
		"an unfinished boot of 20 s did not count toward the limit of eight");
	require(guard.early_restarts() == uml::emulator::unfinished_restart_limit &&
		guard.restarts_within_window() == 0U,
		"the ending restart was not counted as an unfinished one");

	/* However long it ran: an hour is no better than twenty seconds. */
	RestartLoopGuard slow;
	for (unsigned boot = 1; boot != uml::emulator::unfinished_restart_limit;
	     ++boot)
		now = restart_after(slow, now, std::chrono::hours{1}, true,
			BootCompletion::unfinished);
	(void)restart_after(slow, now, std::chrono::hours{1}, false,
		BootCompletion::unfinished);
}

/* Reboot tests and a user's own restarts follow boots that finished. */
void test_restarts_after_finished_boots_are_never_limited()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (int reboot = 0; reboot != 200; ++reboot)
		now = restart_after(guard, now, 25s, true, BootCompletion::completed);
	require(guard.early_restarts() == 0U &&
		guard.restarts_within_window() == 0U,
		"restarts after finished boots were counted");
}

void test_a_finished_boot_resets_the_count()
{
	RestartLoopGuard guard;
	auto now = epoch;
	/* Seven boots that did not finish, one that did, seven more: fourteen
	 * unfinished restarts, never eight in a row. */
	for (int round = 0; round != 3; ++round) {
		for (int early = 0; early != 7; ++early)
			now = restart_after(guard, now, 30s, true,
				BootCompletion::unfinished);
		now = restart_after(guard, now, 30s, true,
			BootCompletion::completed);
		require(guard.early_restarts() == 0U,
			"a finished boot did not reset the count");
	}
}

/* The two rules apply together: a boot that finished and restarted within the
 * window is early, as it was before the console said anything, and five of
 * them in a row end the loop. */
void test_the_window_still_counts_a_finished_boot()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (unsigned boot = 1; boot != uml::emulator::early_restart_limit; ++boot)
		now = restart_after(guard, now, 2s, true, BootCompletion::completed);
	require(guard.early_restarts() ==
		uml::emulator::early_restart_limit - 1U,
		"a finished boot that restarted at once did not count as early");
	(void)restart_after(guard, now, 2s, false, BootCompletion::completed);
}

/* A guest whose console says nothing keeps the 15 s rule alone: its boots
 * never finish as far as the host can tell, and may not be limited for it. */
void test_a_guest_with_no_signal_keeps_the_window_alone()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (int reboot = 0; reboot != 50; ++reboot)
		now = restart_after(guard, now, 20s, true,
			BootCompletion::unobserved);
	require(guard.early_restarts() == 0U,
		"an unobserved boot of 20 s was counted");
	for (unsigned boot = 1; boot != uml::emulator::early_restart_limit; ++boot)
		now = restart_after(guard, now, 5s, true,
			BootCompletion::unobserved);
	(void)restart_after(guard, now, 5s, false, BootCompletion::unobserved);
}

/* Four restarts within the window, then slow ones that never finished: the
 * window's count resets at the first slow restart and the eight count goes on
 * from the four. */
void test_fast_restarts_and_slow_ones_count_together_toward_eight()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (int fast = 0; fast != 4; ++fast)
		now = restart_after(guard, now, 2s, true, BootCompletion::unfinished);
	require(guard.restarts_within_window() == 4U &&
		guard.early_restarts() == 4U, "four fast restarts were counted wrong");
	for (unsigned slow = 5; slow != uml::emulator::unfinished_restart_limit;
	     ++slow) {
		now = restart_after(guard, now, 20s, true,
			BootCompletion::unfinished);
		require(guard.restarts_within_window() == 0U &&
			guard.early_restarts() == slow,
			"a slow restart did not reset the window's count alone");
	}
	(void)restart_after(guard, now, 20s, false, BootCompletion::unfinished);
	require(guard.early_restarts() == 8U,
		"the eighth early restart did not end the loop");
	require(guard.end_message() ==
		"restarted 8 times in a row, each before it finished booting or "
		"within 15 s of booting", "the late loop's message is wrong: " +
		guard.end_message());
}

/* Seven slow restarts, then a boot that finished: the loop never ends. */
void test_seven_slow_restarts_and_a_finished_boot_never_end_the_loop()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (int round = 0; round != 5; ++round) {
		for (int slow = 0; slow != 7; ++slow)
			now = restart_after(guard, now, 20s, true,
				BootCompletion::unfinished);
		now = restart_after(guard, now, 20s, true, BootCompletion::completed);
	}
}

/* Five within the window end the loop even when slow, unfinished boots came
 * before them: the window's own count is five in a row. */
void test_five_within_the_window_end_the_loop_after_slow_ones()
{
	RestartLoopGuard guard;
	auto now = epoch;
	now = restart_after(guard, now, 20s, true, BootCompletion::unfinished);
	now = restart_after(guard, now, 20s, true, BootCompletion::unfinished);
	for (unsigned fast = 1; fast != uml::emulator::early_restart_limit; ++fast)
		now = restart_after(guard, now, 2s, true, BootCompletion::unfinished);
	(void)restart_after(guard, now, 2s, false, BootCompletion::unfinished);
	require(guard.end_message() ==
		"restarted 5 times in a row within 15 s of booting",
		"the early loop's message is wrong: " + guard.end_message());
}

/* A slow restart in the middle of fast ones starts the window's count again:
 * four fast, a slow, four fast is no five in a row. */
void test_a_slow_restart_breaks_the_windows_run_of_five()
{
	RestartLoopGuard guard;
	auto now = epoch;
	for (int fast = 0; fast != 4; ++fast)
		now = restart_after(guard, now, 2s, true, BootCompletion::completed);
	now = restart_after(guard, now, 20s, true, BootCompletion::completed);
	for (int fast = 0; fast != 4; ++fast)
		now = restart_after(guard, now, 2s, true, BootCompletion::completed);
}

void test_the_completion_follows_the_guest_kind()
{
	using uml::emulator::boot_completion;
	/* A guest whose kind says nothing of its boot is never anything else. */
	for (const bool init : {false, true})
		for (const bool done : {false, true})
			require(boot_completion(false, init, done) ==
				BootCompletion::unobserved,
				"a guest whose console says nothing was observed");
	require(boot_completion(true, true, true) == BootCompletion::completed &&
		boot_completion(true, false, true) == BootCompletion::completed,
		"a finished boot was not completed");
	require(boot_completion(true, true, false) == BootCompletion::unfinished,
		"a boot whose init was seen and did not finish was not unfinished");
	/* An Android console that shows nothing of init, as one the host does
	 * not read does on a healthy boot, is not evidence of an unfinished boot. */
	require(boot_completion(true, false, false) == BootCompletion::unobserved,
		"a boot with no init output was taken for an unfinished one");
}

/* Healthy boots on a console the host does not read: only the window counts,
 * as for a Linux guest. */
void test_healthy_boots_on_an_unread_console_are_never_limited()
{
	RestartLoopGuard guard;
	auto now = epoch;
	const auto none = uml::emulator::boot_completion(true, false, false);
	for (int reboot = 0; reboot != 50; ++reboot)
		now = restart_after(guard, now, 20s, true, none);
	require(guard.early_restarts() == 0U,
		"restarts on an unread console were counted");
}

void test_the_end_is_worded_for_its_limit()
{
	/* The words existing logs and tests quote, for a Linux guest. */
	RestartLoopGuard linux_guest;
	auto now = epoch;
	for (unsigned boot = 1; boot != uml::emulator::early_restart_limit; ++boot)
		now = restart_after(linux_guest, now, 2s, true);
	(void)restart_after(linux_guest, now, 2s, false);
	require(linux_guest.end_message() ==
		"restarted 5 times in a row within 15 s of booting",
		"the early loop's message changed: " + linux_guest.end_message());
}

} // namespace

int main()
{
	test_the_policy_is_fifteen_seconds_five_and_eight_restarts();
	test_a_boot_that_lasts_the_window_is_not_early();
	test_the_fifth_early_restart_in_a_row_ends_the_loop();
	test_a_long_boot_resets_the_count();
	test_a_guest_that_never_boots_ends_after_five_boots();
	test_a_restart_with_no_time_at_all_is_early();
	test_a_boot_that_never_finished_counts_however_long_it_ran();
	test_restarts_after_finished_boots_are_never_limited();
	test_a_finished_boot_resets_the_count();
	test_the_window_still_counts_a_finished_boot();
	test_a_guest_with_no_signal_keeps_the_window_alone();
	test_fast_restarts_and_slow_ones_count_together_toward_eight();
	test_seven_slow_restarts_and_a_finished_boot_never_end_the_loop();
	test_five_within_the_window_end_the_loop_after_slow_ones();
	test_a_slow_restart_breaks_the_windows_run_of_five();
	test_the_completion_follows_the_guest_kind();
	test_healthy_boots_on_an_unread_console_are_never_limited();
	test_the_end_is_worded_for_its_limit();
	test_the_restart_command_is_printable();
	test_the_retry_policy_is_bounded();
	std::puts("restart loop guard tests passed");
	return 0;
}
