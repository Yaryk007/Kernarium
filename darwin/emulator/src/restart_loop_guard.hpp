/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace uml::emulator {

/*
 * The loop guard of an application that boots a guest again after the guest
 * restarted itself.
 *
 * A restart is early when it comes less than `early_restart_window` after its
 * boot started. For a guest whose console says when its boot ends (an Android
 * guest, uml_emulator_run_end), a restart is also early when the boot had not
 * finished, however long it ran, so a guest that restarts itself every 20 s
 * and never gets as far as booting is stopped like one that restarts at once.
 * A restart after a finished boot that lasted the window is not early,
 * however often it follows: a reboot test suite restarts its guest without
 * limit. Two counts decide:
 *
 *  - `early_restart_limit` restarts in a row within the window end the loop.
 *    Any restart that comes after the window resets this count. This is the
 *    rule for a guest whose console says nothing of its boot, and for any
 *    guest that restarts at once.
 *  - `unfinished_restart_limit` early restarts in a row, within the window or
 *    before the boot finished, end the loop. Only a restart that is not early
 *    resets this count. It is higher because Android's own boot-loop rescue
 *    acts at five failed boots in ten minutes (a warm reboot, then recovery's
 *    factory-reset prompt): a slow loop is left to reach it, and the host
 *    ends only a loop that goes past it.
 *
 * These constants are the one place the policy lives; every application that
 * runs the loop uses them.
 */
inline constexpr std::chrono::seconds early_restart_window{15};
inline constexpr unsigned early_restart_limit = 5;
inline constexpr unsigned unfinished_restart_limit = 8;

/* The macOS launchers' exit status when the guard ended the loop. 0 is a
 * power-off and 1 a launch error; a guest never ends with 3. */
inline constexpr int early_restart_exit_status = 3;

/* The library refuses to hand a restarted guest back, with
 * -UML_EMULATOR_RUNTIME_ERROR and no side effect, while a kernel thread of
 * the old guest is still ending. The refusal can be retried: an application
 * runs again up to this many times, this far apart, before it ends the launch
 * on the refusal. */
inline constexpr unsigned restart_run_retries = 3;
inline constexpr std::chrono::milliseconds restart_run_retry_delay{500};

/* The restart command is the guest's raw bytes, control characters and
 * quotes included. This is what an application may log or show: printable
 * ASCII as it is, a backslash or a single quote with a backslash before it,
 * and any other byte as \xNN, so the log line it sits in cannot be broken or
 * made to look like another. */
[[nodiscard]] inline std::string printable_restart_command(
	std::string_view command)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string printable;
	printable.reserve(command.size());
	for (const char character : command) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte == '\\' || byte == '\'') {
			printable.push_back('\\');
			printable.push_back(character);
		} else if (byte >= 0x20 && byte < 0x7f) {
			printable.push_back(character);
		} else {
			printable += "\\x";
			printable.push_back(digits[byte >> 4]);
			printable.push_back(digits[byte & 0xf]);
		}
	}
	return printable;
}

/* What the application knows of the boot that just ended. */
enum class BootCompletion {
	/* The guest's console shows nothing of when its boot ends (a Linux
	 * desktop guest, or an Android console the host does not read): only the
	 * early window counts. */
	unobserved,
	/* The console showed init's output, and the boot had not finished by the
	 * restart. */
	unfinished,
	/* The console said that the boot finished. */
	completed,
};

/* What an application tells the guard of a boot that ended: `reports` is
 * whether the guest's kind says when its boot ends at all, `init_observed`
 * and `completed` the run's uml_emulator_run_end fields. A boot of such a
 * guest is unfinished only when init's output was seen and did not end it. */
[[nodiscard]] constexpr BootCompletion boot_completion(bool reports,
	bool init_observed, bool completed) noexcept
{
	return !reports ? BootCompletion::unobserved :
		completed ? BootCompletion::completed :
		init_observed ? BootCompletion::unfinished :
		BootCompletion::unobserved;
}

class RestartLoopGuard final {
public:
	using Clock = std::chrono::steady_clock;

	/* A boot starts at `now`. */
	void boot_started(Clock::time_point now) noexcept { started_ = now; }

	/* The guest asked to restart at `now`, and its boot ended as
	 * `completion` says. Whether the application may boot it again: false
	 * once `early_restart_limit` restarts within the window, or
	 * `unfinished_restart_limit` early ones, followed each other. */
	[[nodiscard]] bool may_restart(Clock::time_point now,
		BootCompletion completion) noexcept
	{
		const bool within = now - started_ < early_restart_window;
		const bool early = within || completion == BootCompletion::unfinished;
		in_window_in_a_row_ = within ? in_window_in_a_row_ + 1 : 0;
		early_in_a_row_ = early ? early_in_a_row_ + 1 : 0;
		ended_by_window_ = in_window_in_a_row_ >= early_restart_limit;
		return !ended_by_window_ && early_in_a_row_ < unfinished_restart_limit;
	}

	/* The restarts in a row within the window, and the early ones (within
	 * the window or before the boot finished) in a row. */
	[[nodiscard]] unsigned restarts_within_window() const noexcept
	{
		return in_window_in_a_row_;
	}
	[[nodiscard]] unsigned early_restarts() const noexcept
	{
		return early_in_a_row_;
	}

	/* What an application says when the guard ended the loop, after "the
	 * guest ": the restarts in a row, and why they count, for the limit that
	 * ended it. */
	[[nodiscard]] std::string end_message() const
	{
		const std::string window =
			std::to_string(early_restart_window.count()) + " s of booting";
		if (ended_by_window_)
			return "restarted " + std::to_string(in_window_in_a_row_) +
				" times in a row within " + window;
		return "restarted " + std::to_string(early_in_a_row_) +
			" times in a row, each before it finished booting or within " +
			window;
	}

private:
	Clock::time_point started_{};
	unsigned in_window_in_a_row_{};
	unsigned early_in_a_row_{};
	bool ended_by_window_{};
};

} // namespace uml::emulator
