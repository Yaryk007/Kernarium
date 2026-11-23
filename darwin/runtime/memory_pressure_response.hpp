// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace uml::runtime {

/*
 * What the host gives back when the system says it is short of memory.
 *
 * The system's memory pressure source reports three levels. They describe the
 * whole device, not this process's own headroom, and a warning is frequent, so
 * only the critical level responds, and it asks the guest for its own
 * page-cache trimmer (TrimKind::gradual_on), never for one trim of all the cold
 * cache, which stalls frames:
 *
 *   - a critical level turns the guest's trimmer on: it trims the cold file
 *     cache in small passes a few seconds apart and backs off while the pages
 *     it took are read back. The host sends the request and does not wait for
 *     the guest's answer. It also frees the host GPU's cached driver
 *     allocations (platform/gpu_cache_reclaim.hpp) each time.
 *   - a warning and the normal level free nothing and start no response. They
 *     turn off the trimmer a critical level turned on, unless the application
 *     asked for it (uml_emulator_set_page_cache_trimming).
 *
 * The trimmer is on, for the pressure, exactly while the level held is
 * critical: a critical level turns it on, and a warning, the normal level and
 * the quiet period's return turn it off at once. Each request is one short
 * message, and none is sent while the guest already runs as asked, so levels
 * that alternate are followed as they come.
 *
 * The system need not say when the pressure ends (an iPad's source never
 * delivered the normal level after a critical one). The level the response
 * holds therefore returns to normal by itself when the source has delivered
 * no level other than normal for `PressureTiming::quiet_period_ns`, on the
 * clock of PressureActions::now_ns (which stops while the device sleeps, not
 * while the process alone is suspended): on_retry() makes the return, the same
 * way the normal level does, and says so on its level line. A process
 * suspended for longer than the period meets the return at its resume, which
 * asks the trimmer off and nothing else; a level still present is delivered
 * again. A delivered normal level ends the period at once.
 *
 * A critical level always starts a response, because it frees the GPU's caches
 * each time and the trimmer request is free where the guest runs as asked. No
 * response starts while one is running: the event is dropped and counted, and
 * a level that eases meanwhile ends the trimmer when the response returns. The
 * translation caches, the guest's RAM and every camera or other session are
 * not touched.
 *
 * The guest requests are skipped, with no retry, for a guest the host has
 * quiesced (it trimmed the guest when it did, and a request would wake it; the
 * trimmer's state is kept for the resume). A request that finds no console
 * (TrimOutcome::unreachable) or another lifecycle call of the guest in progress
 * (TrimOutcome::busy) is kept pending and tried again after
 * `PressureTiming::retry_interval_ns`, up to `retry_attempts` times: on_retry()
 * runs it, and the next response (which tries for itself) or a level that no
 * longer asks for it replaces or ends it.
 *
 * Each event the system raises is also reported to the application, once and
 * as soon as the response has decided what it does with it (PressureReport):
 * the level, and whether a response starts or is already running. The report is
 * made by the call that handles the event, before that response's work, and the
 * system's watch makes those calls one at a time in the order the events
 * arrived. A warning and the normal level are reported too, so that an
 * application that shows the pressure can show its end. It carries no figures.
 *
 * Each change of the level and each critical response (or the last retry of a
 * request) is one line on stderr (the console of both applications). A level
 * line carries the level and the footprint; a response line the footprint
 * before and after, the bytes of graphics the GPU release freed and the state of
 * the gradual trimmer. Every line carries the count of guest RAM wiring
 * operations that failed (the first sign of a system that is short). Each kind
 * of line is rate limited, so that a storm of events cannot flood the console;
 * the next line that passes says how many were left out.
 *
 * MemoryPressureResponse is the policy and holds no thread, clock or system
 * call: it reads and acts through PressureActions, so a test drives it with a
 * clock and actions of its own. start_memory_pressure_response() composes it
 * with the system's source and actions.
 */

enum class PressureLevel : std::uint8_t {
	normal = 0,
	warn = 1,
	critical = 2,
};

/* The name the lines use. */
[[nodiscard]] std::string_view pressure_level_name(PressureLevel level) noexcept;

/* The level a memory pressure source's data word states: the most serious flag
 * set (DISPATCH_MEMORYPRESSURE_CRITICAL 0x4, _WARN 0x2, _NORMAL 0x1), normal
 * for none. */
[[nodiscard]] PressureLevel pressure_level_from_dispatch(
	std::uint64_t flags) noexcept;

/* What the response does about a level the system has just reported, decided
 * before it does it. */
enum class PressureReaction : std::uint8_t {
	/* A warning or the normal level: no response starts (the gradual trimmer a
	 * critical level turned on is asked off). */
	none = 0,
	/* A critical level: a response starts, which turns the guest's gradual
	 * trimmer on and frees the graphics caches. */
	responding = 1,
	/* A response is running (or being tried again) and this event starts none
	 * of its own. The system delivers its notifications one at a time, each
	 * after the response of the one before, so this does not arise from it. */
	underway = 3,
};

/* The name the lines use. */
[[nodiscard]] std::string_view pressure_reaction_name(
	PressureReaction reaction) noexcept;

/* One event of the system's memory pressure source, as the application hears
 * it. */
struct PressureReport final {
	PressureLevel level{PressureLevel::normal};
	PressureReaction reaction{PressureReaction::none};
};

/* How the guest's page-cache trim ended. */
enum class TrimOutcome : std::uint8_t {
	/* The guest trimmed, or already ran as asked. */
	done,
	/* Sent, and its answer was not waited for (the gradual trimmer's two
	 * short requests). */
	sent,
	/* Sent, and the guest had not finished within the bound. */
	unanswered,
	/* Sent, and the guest refused it, as a kernel without the command does. */
	refused,
	/* Not sent: there is no console to send it to. */
	unreachable,
	/* Not sent: the host has quiesced the guest, so the guest was trimmed
	 * when it was suspended and a request would wake it. */
	quiesced,
	/* Not sent: another lifecycle call of the guest was in progress. The
	 * only outcome the response tries again. */
	busy,
};

[[nodiscard]] std::string_view trim_outcome_name(TrimOutcome outcome) noexcept;

/* What a guest trim asks of the guest. */
enum class TrimKind : std::uint8_t {
	/* All of the cold page cache and the free memory, now, and the reply is
	 * waited for, bounded ("trim once"). The response never asks for it: it
	 * stalls frames, and the system's level is the device's, not this process's
	 * headroom. */
	once,
	/* The guest's periodic trimmer on ("trim on"): the request is one short
	 * message that is not waited for, and the trimming is the guest's, in small
	 * passes that back off while what they took is read back. */
	gradual_on,
	/* The periodic trimmer off ("trim off"). */
	gradual_off,
};

/* What the response reads of the process at one moment. */
struct PressureFigures final {
	/* What the system holds the process to (phys_footprint). */
	std::uint64_t footprint{};
	/* The part of the guest's RAM the host holds; 0 without a guest. */
	std::uint64_t guest_held{};
	/* The footprint's graphics ledger, compressed pages included. */
	std::uint64_t graphics{};
	/* Guest RAM wiring operations that failed, since the guest began (the
	 * count is of operations, each over a range of pages). */
	std::uint64_t wire_failures{};
};

/* Everything the policy reads and does. Implementations answer on the thread
 * that calls and never wait on one another. */
class PressureActions {
public:
	virtual ~PressureActions() = default;

	/* A clock that does not go backwards, in nanoseconds. */
	[[nodiscard]] virtual std::uint64_t now_ns() noexcept = 0;
	[[nodiscard]] virtual PressureFigures figures() noexcept = 0;
	/* Frees the host GPU's cached driver allocations; returns how many it
	 * freed, 0 with no GPU. */
	[[nodiscard]] virtual std::uint64_t reclaim_gpu() noexcept = 0;
	/* Asks the guest for `kind` of trim. "Once" waits for the trim, bounded;
	 * the gradual trimmer's requests, the only ones the response makes, wait for
	 * nothing. */
	[[nodiscard]] virtual TrimOutcome trim_page_cache(TrimKind kind) noexcept = 0;
	/* One line, without its newline. */
	virtual void log(std::string_view line) noexcept = 0;
	/* Tells the application what the system reported and what the host does
	 * about it. Called once for each event, as soon as that is decided and
	 * before the response's work, on the thread that handles the event; it
	 * returns promptly. */
	virtual void report(const PressureReport &report) noexcept = 0;
};

/* The response's pacing; a test shortens the retry. */
struct PressureTiming final {
	/* How long after a request that found the guest busy the next try is due. */
	std::uint64_t retry_interval_ns = 1'000'000'000ULL;
	/* How many times a request that did not go through is tried again; 0 for
	 * never. */
	unsigned retry_attempts = 4;
	/* How long the source must deliver no level but the normal one for the
	 * response to return to normal itself; 0 for never. Three times the longest
	 * gap seen between two critical levels of one episode (78 s, on an iPad,
	 * where the normal level never came). */
	std::uint64_t quiet_period_ns = 240'000'000'000ULL;
};

class MemoryPressureResponse final {
public:
	/* Each kind of line: at most `log_burst` in `log_window_ns`. */
	static constexpr std::uint64_t log_window_ns = 10'000'000'000ULL;
	static constexpr unsigned log_burst = 4;

	/* What a call to on_level() did. */
	enum class Outcome : std::uint8_t {
		/* A warning or the normal level (the trimmer was asked off, when it was
		 * on). */
		nothing,
		/* A critical level's response ran. */
		responded,
		/* A response is running. */
		busy,
		/* on_retry() ran the pending guest request again. */
		retried,
		/* on_retry() returned the level to normal after the quiet period. */
		quiet,
	};

	explicit MemoryPressureResponse(PressureActions &actions,
		PressureTiming timing = {}) noexcept
		: actions_{actions}, timing_{timing}
	{
	}

	MemoryPressureResponse(const MemoryPressureResponse &) = delete;
	MemoryPressureResponse &operator=(const MemoryPressureResponse &) = delete;

	/* The system reported `level`. Runs the response, when there is one (a
	 * critical level), on the calling thread and returns once it has ended; any
	 * thread, and a call that meets a response running returns at once. */
	Outcome on_level(PressureLevel level) noexcept;

	/* How long from now the pending guest request is due, 0 when none is: a
	 * retry after `retry_interval_ns`. The caller runs on_retry() that long after
	 * each call that may leave one. */
	[[nodiscard]] std::uint64_t retry_delay_ns() const noexcept;

	/* How long from now the quiet period ends, 0 when the level held is
	 * normal (or the period is off). The caller runs on_retry() that long after
	 * each call that may have moved it. */
	[[nodiscard]] std::uint64_t quiet_delay_ns() const noexcept;

	/* The pending guest request is tried again, on the calling thread. Nothing
	 * happens when none is pending (a later response or a level that no longer
	 * asks for it took it), and a call that meets a response running returns
	 * busy and leaves it pending. When the quiet period has ended it returns the level to normal
	 * instead, whatever is pending (the normal level takes it). */
	Outcome on_retry() noexcept;

	/* The response's counters, for a test or a report. */
	struct Counts final {
		std::uint64_t responses{};
		std::uint64_t busy{};
		std::uint64_t retries{};
		std::uint64_t lines_dropped{};
	};
	[[nodiscard]] Counts counts() const noexcept;

private:
	/* A fixed window of lines. Called under the response's mutex. */
	class LineBudget final {
	public:
		/* Whether a line may be written at `now_ns`; the lines left out since
		 * the last one is returned through `dropped`. */
		[[nodiscard]] bool admit(std::uint64_t now_ns,
			std::uint64_t &dropped) noexcept;

	private:
		std::uint64_t window_start_{};
		unsigned used_{};
		bool started_{};
		std::uint64_t dropped_{};
	};

	/* Whether a line of `budget` may be written now (the clock is read under
	 * the mutex, so that calls from several threads see it in order);
	 * `dropped` receives the lines of that kind left out since the last
	 * one. */
	[[nodiscard]] bool admit_line(LineBudget &budget,
		std::uint64_t &dropped) noexcept;
	void log_level_change(PressureLevel from, PressureLevel to,
		bool quiet) noexcept;
	/* on_level(), and on_retry()'s return to normal (`quiet`), which does
	 * nothing unless the period has ended. */
	Outcome handle_level(PressureLevel level, bool quiet) noexcept;
	/* Whether the period has ended. Under the mutex, with the clock read
	 * under it. */
	[[nodiscard]] bool quiet_due(std::uint64_t now) const noexcept;
	/* A critical level's response. */
	void respond(std::uint64_t events_skipped) noexcept;
	/* Asks for the gradual trimmer off, after a level that no longer asks for
	 * it, and keeps the request pending when it could not be sent. */
	void end_gradual_trim() noexcept;
	/* Asks for the gradual trimmer on, for a critical level that met a retry
	 * running, and keeps the request pending when it could not be sent. */
	void arm_gradual_trim() noexcept;
	/* A gradual request of `kind`, noted. */
	[[nodiscard]] TrimOutcome ask_gradual(TrimKind kind) noexcept;
	/* Records what became of a request of `kind`: an ON that was asked is
	 * owed an OFF until one has been taken. Under the mutex. */
	void note_request(TrimKind kind, TrimOutcome outcome) noexcept;
	/* Leaves the request pending after a request that did not go through, for
	 * `level`, with the full attempts. Under the mutex. */
	void keep_pending(PressureLevel level, TrimKind kind) noexcept;

	PressureActions &actions_;
	const PressureTiming timing_;
	mutable std::mutex mutex_;
	PressureLevel level_{PressureLevel::normal};
	/* When the source last delivered a level other than normal. */
	std::uint64_t last_level_ns_{};
	bool running_{};
	std::uint64_t skipped_{};
	/* The one pending guest request: the level of the response that left it,
	 * what it asks (the trimmer on or off) and the tries it has left. */
	bool retry_pending_{};
	PressureLevel retry_level_{PressureLevel::normal};
	TrimKind retry_kind_{TrimKind::gradual_on};
	unsigned retry_attempts_left_{};
	/* The gradual trimmer was asked on and no request to turn it off has been
	 * taken since: a level that no longer asks for it owes it an off. */
	bool gradual_on_{};
	Counts counts_{};
	LineBudget level_lines_;
	LineBudget response_lines_;
};

/* The step the runtime cannot take itself: the host's requests to the guest
 * (the kinds of TrimKind), which the emulator layer owns. Called on the
 * response's thread, never with a lock the caller holds. */
using GuestTrim = TrimOutcome (*)(void *context, TrimKind kind) noexcept;

/* The response of a run, listening to the system's memory pressure source until
 * it is destroyed. The destructor returns once a response that is running has
 * ended, and no response, retry or call of the run's `GuestTrim` starts after
 * it. The source, its queue and the retry timer are the process's, made on the
 * first watch and kept: cancelling a source leaves a libdispatch worker thread
 * behind, and a run leaves no thread of its own. The timer starts, and takes
 * a thread of libdispatch's, with the first retry. */
class MemoryPressureWatch final {
public:
	~MemoryPressureWatch();

	MemoryPressureWatch(const MemoryPressureWatch &) = delete;
	MemoryPressureWatch &operator=(const MemoryPressureWatch &) = delete;

	struct State;

private:
	friend std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
		GuestTrim, void *, PressureTiming);
	explicit MemoryPressureWatch(std::unique_ptr<State> state) noexcept;

	std::unique_ptr<State> state_;
};

/* Receives the reports of the run's response (PressureReport), on the thread
 * that handles the system's event, so it returns promptly. */
using PressureObserver = void (*)(void *context,
	const PressureReport &report) noexcept;

/* Sets, or with a null observer clears, the process's one receiver of the
 * reports. It outlives a run and hears the reports of every run after it was
 * set, none from before. Once this returns, no call to the previous observer
 * is running or will start, so its context may be released; the observer must
 * not call this. */
void set_pressure_observer(PressureObserver observer, void *context) noexcept;

/* Starts the response of the run that is beginning. `trim` and `context` are
 * the guest's page-cache trim. Null when the system's source cannot be made;
 * the run goes on without the response. */
[[nodiscard]] std::unique_ptr<MemoryPressureWatch> start_memory_pressure_response(
	GuestTrim trim, void *context, PressureTiming timing = {});

} // namespace uml::runtime
