// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory_pressure_response.hpp"

#include "fixed_text.hpp"

#include <utility>

namespace uml::runtime {

namespace {

constexpr std::uint64_t kibibyte = 1024ULL;
constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

/* The mebibytes of `bytes`, or its kibibytes under one. */
void size_text(FixedText &out, std::uint64_t bytes) noexcept
{
	if (bytes >= mebibyte || bytes == 0U)
		out.number(static_cast<std::int64_t>(bytes / mebibyte)).text(" MiB");
	else
		out.number(static_cast<std::int64_t>(bytes / kibibyte)).text(" KiB");
}

void footprint_text(FixedText &out, std::uint64_t bytes) noexcept
{
	out.number(static_cast<std::int64_t>(bytes / mebibyte)).text(" MiB");
}

[[nodiscard]] std::uint64_t fell_by(std::uint64_t before,
	std::uint64_t after) noexcept
{
	return before > after ? before - after : 0U;
}

/* The clock reads of two threads may cross; time does not run backwards. */
[[nodiscard]] std::uint64_t elapsed(std::uint64_t start,
	std::uint64_t end) noexcept
{
	return end > start ? end - start : 0U;
}

/* The tail every line carries when lines before it were left out. */
void dropped_text(FixedText &out, std::uint64_t dropped) noexcept
{
	if (dropped == 0U)
		return;
	out.text("; ").number(static_cast<std::int64_t>(dropped))
		.text(" earlier lines left out");
}

/* What the application is told of a call's outcome. */
[[nodiscard]] PressureReaction reaction_of(
	MemoryPressureResponse::Outcome outcome) noexcept
{
	switch (outcome) {
	case MemoryPressureResponse::Outcome::responded:
		return PressureReaction::responding;
	case MemoryPressureResponse::Outcome::busy:
		return PressureReaction::underway;
	case MemoryPressureResponse::Outcome::nothing:
	case MemoryPressureResponse::Outcome::retried:
	case MemoryPressureResponse::Outcome::quiet:
		break;
	}
	return PressureReaction::none;
}

/* The request reached the guest, or will when the guest is resumed. */
[[nodiscard]] bool taken(TrimOutcome outcome) noexcept
{
	return outcome == TrimOutcome::done || outcome == TrimOutcome::sent ||
	       outcome == TrimOutcome::unanswered ||
	       outcome == TrimOutcome::quiesced;
}

/* A request that did not go through and may the next time: another call held
 * the guest or its console, or the message could not be written. */
[[nodiscard]] bool transient(TrimOutcome outcome) noexcept
{
	return outcome == TrimOutcome::busy || outcome == TrimOutcome::unreachable;
}

} // namespace

std::string_view pressure_level_name(PressureLevel level) noexcept
{
	switch (level) {
	case PressureLevel::normal:
		return "normal";
	case PressureLevel::warn:
		return "warn";
	case PressureLevel::critical:
		return "critical";
	}
	return "unknown";
}

PressureLevel pressure_level_from_dispatch(std::uint64_t flags) noexcept
{
	constexpr std::uint64_t warn = 0x2;
	constexpr std::uint64_t critical = 0x4;
	if ((flags & critical) != 0U)
		return PressureLevel::critical;
	if ((flags & warn) != 0U)
		return PressureLevel::warn;
	return PressureLevel::normal;
}

std::string_view pressure_reaction_name(PressureReaction reaction) noexcept
{
	switch (reaction) {
	case PressureReaction::none:
		return "none";
	case PressureReaction::responding:
		return "responding";
	case PressureReaction::underway:
		return "underway";
	}
	return "unknown";
}

std::string_view trim_outcome_name(TrimOutcome outcome) noexcept
{
	switch (outcome) {
	case TrimOutcome::done:
		return "done";
	case TrimOutcome::unanswered:
		return "unanswered";
	case TrimOutcome::refused:
		return "refused";
	case TrimOutcome::unreachable:
		return "no console";
	case TrimOutcome::quiesced:
		return "guest quiesced";
	case TrimOutcome::busy:
		return "busy";
	case TrimOutcome::sent:
		return "sent";
	}
	return "unknown";
}

bool MemoryPressureResponse::LineBudget::admit(std::uint64_t now_ns,
	std::uint64_t &dropped) noexcept
{
	if (!started_ || elapsed(window_start_, now_ns) >= log_window_ns) {
		started_ = true;
		window_start_ = now_ns;
		used_ = 0;
	}
	if (used_ >= log_burst) {
		++dropped_;
		return false;
	}
	++used_;
	dropped = std::exchange(dropped_, 0U);
	return true;
}

MemoryPressureResponse::Counts MemoryPressureResponse::counts() const noexcept
{
	const std::lock_guard lock{mutex_};
	return counts_;
}

bool MemoryPressureResponse::admit_line(LineBudget &budget,
	std::uint64_t &dropped) noexcept
{
	const std::lock_guard lock{mutex_};
	if (budget.admit(actions_.now_ns(), dropped))
		return true;
	++counts_.lines_dropped;
	return false;
}

void MemoryPressureResponse::log_level_change(PressureLevel from,
	PressureLevel to, bool quiet) noexcept
{
	std::uint64_t dropped = 0;
	if (!admit_line(level_lines_, dropped))
		return;
	const PressureFigures now = actions_.figures();
	char buffer[320];
	FixedText line{buffer, sizeof(buffer)};
	line.text("uml: memory pressure ").text(pressure_level_name(to))
		.text(" (was ").text(pressure_level_name(from)).text("): footprint ");
	footprint_text(line, now.footprint);
	line.text(", ").number(static_cast<std::int64_t>(now.wire_failures))
		.text(" guest RAM wire failures");
	if (quiet)
		line.text("; no level for ")
			.number(static_cast<std::int64_t>(
				timing_.quiet_period_ns / 1'000'000'000ULL))
			.text(" s");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
}

bool MemoryPressureResponse::quiet_due(std::uint64_t now) const noexcept
{
	return timing_.quiet_period_ns != 0U && level_ != PressureLevel::normal &&
	       !running_ && elapsed(last_level_ns_, now) >= timing_.quiet_period_ns;
}

std::uint64_t MemoryPressureResponse::quiet_delay_ns() const noexcept
{
	const std::lock_guard lock{mutex_};
	if (timing_.quiet_period_ns == 0U || level_ == PressureLevel::normal)
		return 0;
	const std::uint64_t quiet = elapsed(last_level_ns_, actions_.now_ns());
	/* Due, and held back by a response that is running: look again soon. */
	return quiet >= timing_.quiet_period_ns ? timing_.retry_interval_ns :
		timing_.quiet_period_ns - quiet;
}

MemoryPressureResponse::Outcome MemoryPressureResponse::on_level(
	PressureLevel level) noexcept
{
	return handle_level(level, false);
}

MemoryPressureResponse::Outcome MemoryPressureResponse::handle_level(
	PressureLevel level, bool quiet) noexcept
{
	PressureLevel previous = PressureLevel::normal;
	Outcome outcome = Outcome::nothing;
	std::uint64_t events_skipped = 0;
	bool end_gradual = false;
	{
		const std::lock_guard lock{mutex_};
		/* Read under the mutex: calls from several threads then see the
		 * clock in the order the mutex gives them. */
		const std::uint64_t now = actions_.now_ns();
		if (quiet && !quiet_due(now))
			return Outcome::nothing;
		previous = std::exchange(level_, level);
		if (level != PressureLevel::normal)
			last_level_ns_ = now;
		if (level != PressureLevel::critical) {
			/* The pressure that asks for the trimmer is over, or has eased: a
			 * request still pending is not wanted, but the trimmer is ended,
			 * unless a response that is running ends it when it returns. */
			retry_pending_ = false;
			end_gradual = gradual_on_ && !running_;
			outcome = quiet ? Outcome::quiet : Outcome::nothing;
		} else {
			/* The pressure is back: an end of the gradual trimmer that is
			 * still pending is no longer wanted. */
			if (retry_pending_ && retry_kind_ == TrimKind::gradual_off)
				retry_pending_ = false;
			if (running_) {
				++counts_.busy;
				++skipped_;
				outcome = Outcome::busy;
			} else {
				running_ = true;
				events_skipped = std::exchange(skipped_, 0U);
				/* This response's own request replaces a pending one. */
				retry_pending_ = false;
				outcome = Outcome::responded;
			}
		}
	}
	/* Before anything that reads the process or waits: the application hears
	 * the event at once, and the response's work follows. */
	actions_.report({level, reaction_of(outcome)});
	if (level != previous)
		log_level_change(previous, level, quiet);
	if (end_gradual)
		end_gradual_trim();
	if (outcome == Outcome::responded)
		respond(events_skipped);
	return outcome;
}

std::uint64_t MemoryPressureResponse::retry_delay_ns() const noexcept
{
	const std::lock_guard lock{mutex_};
	return retry_pending_ ? timing_.retry_interval_ns : 0U;
}

void MemoryPressureResponse::note_request(TrimKind kind,
	TrimOutcome outcome) noexcept
{
	/* Whatever became of an ON, the guest or the host may hold it. */
	if (kind == TrimKind::gradual_on)
		gradual_on_ = true;
	/* An OFF that was taken ends it, and so does one the guest refused: a
	 * kernel without the command has no trimmer to turn off. One that could
	 * not be sent leaves the end owed. */
	else if (kind == TrimKind::gradual_off &&
		 (taken(outcome) || outcome == TrimOutcome::refused))
		gradual_on_ = false;
}

void MemoryPressureResponse::keep_pending(PressureLevel level,
	TrimKind kind) noexcept
{
	if (timing_.retry_attempts == 0U)
		return;
	retry_pending_ = true;
	retry_level_ = level;
	retry_kind_ = kind;
	retry_attempts_left_ = timing_.retry_attempts;
}

TrimOutcome MemoryPressureResponse::ask_gradual(TrimKind kind) noexcept
{
	const TrimOutcome outcome = actions_.trim_page_cache(kind);
	const std::lock_guard lock{mutex_};
	note_request(kind, outcome);
	return outcome;
}

void MemoryPressureResponse::end_gradual_trim() noexcept
{
	const TrimOutcome outcome = ask_gradual(TrimKind::gradual_off);
	const std::lock_guard lock{mutex_};
	/* The pressure came back meanwhile: that response asks for itself. */
	if (transient(outcome) && level_ != PressureLevel::critical && !running_)
		keep_pending(level_, TrimKind::gradual_off);
}

void MemoryPressureResponse::arm_gradual_trim() noexcept
{
	const TrimOutcome outcome = ask_gradual(TrimKind::gradual_on);
	bool end = false;
	{
		const std::lock_guard lock{mutex_};
		if (level_ != PressureLevel::critical)
			end = !running_ && gradual_on_;
		else if (transient(outcome) && !running_ && !retry_pending_)
			keep_pending(level_, TrimKind::gradual_on);
	}
	/* The level eased meanwhile and found nothing to end. */
	if (end)
		end_gradual_trim();
}

void MemoryPressureResponse::respond(std::uint64_t events_skipped) noexcept
{
	const PressureFigures before = actions_.figures();
	/* The GPU's caches are the critical level's: a rendering client takes the
	 * storage back within a second. */
	const std::uint64_t gpu_freed = actions_.reclaim_gpu();
	const PressureFigures after_gpu = actions_.figures();
	/* The gradual trimmer is on while the pressure lasts: one trim of all the
	 * cache stalls frames, and the system's level is the device's, not this
	 * process's headroom. */
	const TrimOutcome gradual = ask_gradual(TrimKind::gradual_on);
	const PressureFigures after = actions_.figures();
	bool end_after = false;
	{
		const std::lock_guard lock{mutex_};
		running_ = false;
		++counts_.responses;
		/* Not for a pressure that eased during the request: the level that
		 * replaced it ends the trimmer. */
		if (level_ == PressureLevel::critical) {
			if (transient(gradual))
				keep_pending(PressureLevel::critical, TrimKind::gradual_on);
		} else {
			end_after = gradual_on_;
		}
	}
	if (end_after)
		end_gradual_trim();

	std::uint64_t dropped = 0;
	if (!admit_line(response_lines_, dropped))
		return;
	char buffer[448];
	FixedText line{buffer, sizeof(buffer)};
	line.text("uml: memory pressure critical: footprint ")
		.number(static_cast<std::int64_t>(before.footprint / mebibyte))
		.text(" -> ");
	footprint_text(line, after.footprint);
	line.text("; released ");
	size_text(line, fell_by(before.graphics, after_gpu.graphics));
	line.text(" graphics (").number(static_cast<std::int64_t>(gpu_freed))
		.text(" cached allocations); guest page cache trimmer on (")
		.text(trim_outcome_name(gradual)).text(")");
	line.text("; ").number(static_cast<std::int64_t>(after.wire_failures))
		.text(" guest RAM wire failures");
	if (events_skipped != 0U)
		line.text("; ").number(static_cast<std::int64_t>(events_skipped))
			.text(" events skipped since the last response");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
}

MemoryPressureResponse::Outcome MemoryPressureResponse::on_retry() noexcept
{
	PressureLevel level = PressureLevel::normal;
	TrimKind kind = TrimKind::gradual_on;
	unsigned attempt = 0;
	bool quiet = false;
	{
		const std::lock_guard lock{mutex_};
		if (quiet_due(actions_.now_ns())) {
			/* The source has delivered nothing but the normal level for the
			 * quiet period: the level returns to normal as if it had been
			 * delivered (the call below, with the lock released, checks again). */
			quiet = true;
		} else {
			if (!retry_pending_)
				return Outcome::nothing;
			if (running_)
				return Outcome::busy;
			running_ = true;
			level = retry_level_;
			kind = retry_kind_;
			--retry_attempts_left_;
			attempt = timing_.retry_attempts - retry_attempts_left_;
			++counts_.retries;
		}
	}
	if (quiet)
		return handle_level(PressureLevel::normal, true);
	const TrimOutcome trim = ask_gradual(kind);
	const PressureFigures after = actions_.figures();
	bool again = false;
	bool end_after = false;
	bool arm_after = false;
	{
		const std::lock_guard lock{mutex_};
		running_ = false;
		/* A level that no longer asks for the request (which takes every
		 * pending one) or the pressure coming back (which takes a pending
		 * end) may have taken the pending request meanwhile. */
		const bool pending = retry_pending_;
		retry_pending_ = false;
		if (pending && transient(trim) && retry_attempts_left_ != 0U) {
			retry_pending_ = true;
			again = true;
		}
		if (level_ != PressureLevel::critical)
			/* The pressure eased while the request was in the guest's hands:
			 * an off is owed, but one that was itself retried is not asked a
			 * second time here. */
			end_after = kind != TrimKind::gradual_off && gradual_on_;
		else if (kind == TrimKind::gradual_off && !again)
			/* It came back while the end was being retried, and the event
			 * found a retry running. */
			arm_after = true;
	}
	if (end_after)
		end_gradual_trim();
	if (arm_after)
		arm_gradual_trim();
	/* A retry that failed says nothing until its last. */
	if (again)
		return Outcome::retried;
	std::uint64_t dropped = 0;
	if (!admit_line(response_lines_, dropped))
		return Outcome::retried;
	char buffer[448];
	FixedText line{buffer, sizeof(buffer)};
	line.text("uml: memory pressure ").text(pressure_level_name(level))
		.text(" (retry ").number(static_cast<std::int64_t>(attempt))
		.text(" of ")
		.number(static_cast<std::int64_t>(timing_.retry_attempts))
		.text("): guest page cache trimmer ")
		.text(kind == TrimKind::gradual_on ? "on" : "off")
		.text(" (").text(trim_outcome_name(trim)).text(")");
	line.text("; ").number(static_cast<std::int64_t>(after.wire_failures))
		.text(" guest RAM wire failures");
	dropped_text(line, dropped);
	actions_.log({buffer, line.size()});
	return Outcome::retried;
}

} // namespace uml::runtime
