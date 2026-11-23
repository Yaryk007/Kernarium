// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The memory pressure response (runtime/memory_pressure_response): the policy
 * against a clock and actions of the test's own, and the system's side of it,
 * the memory pressure source, against a source this binary substitutes: the
 * binary defines dispatch_source_create, so the response's call binds here and
 * a memory pressure source becomes one the test can raise (a real event needs
 * a privileged tool). The process makes that source once, on the first watch.
 */
#include "memory_pressure_response.hpp"
#include "named_tests.hpp"
#include "platform/gpu_cache_reclaim.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

/* The memory pressure source the response made, as a source the test merges
 * data into: DISPATCH_SOURCE_TYPE_DATA_OR delivers the merged bits to the
 * handler as the status flags a memory pressure source delivers. */
std::atomic<dispatch_source_t> pressure_source{nullptr};
/* The queue the source's handler runs on: a call on it from the test, once an
 * event was merged, returns after the handler that event started. The handler
 * only records the delivery. */
std::atomic<dispatch_queue_t> pressure_queue{nullptr};
/* The queue the policy handles the deliveries on, and the source that signals
 * it (made by the response, a data source). */
std::atomic<dispatch_queue_t> policy_queue{nullptr};
std::atomic<dispatch_source_t> deliveries_source{nullptr};
/* While set, the process's nothrow allocations fail, as they may when the
 * system is short of memory, which is what a pressure response runs in. */
std::atomic<bool> nothrow_allocation_fails{false};

} // namespace

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	if (nothrow_allocation_fails.load())
		return nullptr;
	try {
		return ::operator new(size);
	} catch (...) {
		return nullptr;
	}
}

/* The ordinary allocations of the thread that is counting, as the process's
 * own operator new makes them: the count of a thread that does not count stays
 * as it was. */
thread_local bool counting_allocations = false;
thread_local std::size_t allocations_counted = 0;

void *operator new(std::size_t size)
{
	if (counting_allocations)
		++allocations_counted;
	if (void *const memory = std::malloc(size == 0 ? 1 : size))
		return memory;
	throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept
{
	std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
	std::free(memory);
}

namespace {

/* The handlers the response gave the pressure source and the signal of the
 * policy queue, which run through a counting wrapper: the ordinary allocations
 * of one event's whole handling, recording and policy, each on the thread that
 * runs it, added up. */
std::atomic<dispatch_function_t> pressure_handler{nullptr};
std::atomic<dispatch_function_t> deliveries_handler{nullptr};
std::atomic<std::size_t> event_allocations{0};

void countingPressureHandler(void *context)
{
	allocations_counted = 0;
	counting_allocations = true;
	pressure_handler.load()(context);
	counting_allocations = false;
	event_allocations.fetch_add(allocations_counted);
}

void countingDeliveriesHandler(void *context)
{
	allocations_counted = 0;
	counting_allocations = true;
	deliveries_handler.load()(context);
	counting_allocations = false;
	event_allocations.fetch_add(allocations_counted);
}

/* Holds the first read of the pressure source's flags until the test lets it
 * go: a source callback the scheduler stops after it began, before it read
 * what the system raised. */
struct FlagReadHold final {
	std::mutex gate;
	std::condition_variable changed;
	bool armed = false;
	bool inside = false;
	bool release = false;

	void arm()
	{
		const std::lock_guard lock{gate};
		armed = true;
		inside = false;
		release = false;
	}

	/* Called by the read: waits when armed, once. */
	void hold() noexcept
	{
		std::unique_lock lock{gate};
		if (!armed)
			return;
		armed = false;
		inside = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return release; });
	}

	bool wait_inside()
	{
		std::unique_lock lock{gate};
		return changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return inside; });
	}

	void let_go()
	{
		const std::lock_guard lock{gate};
		release = true;
		changed.notify_all();
	}
} flag_read_hold;

} // namespace

extern "C" uintptr_t dispatch_source_get_data(dispatch_source_t source)
{
	using Get = uintptr_t (*)(dispatch_source_t);
	static const auto real = reinterpret_cast<Get>(
		dlsym(RTLD_NEXT, "dispatch_source_get_data"));
	if (source == pressure_source.load())
		flag_read_hold.hold();
	return real(source);
}

extern "C" void dispatch_source_set_event_handler_f(
	dispatch_source_t source, dispatch_function_t handler)
{
	using Set = void (*)(dispatch_source_t, dispatch_function_t);
	static const auto real = reinterpret_cast<Set>(
		dlsym(RTLD_NEXT, "dispatch_source_set_event_handler_f"));
	if (handler != nullptr && source == pressure_source.load()) {
		pressure_handler.store(handler);
		real(source, &countingPressureHandler);
		return;
	}
	if (handler != nullptr && source == deliveries_source.load()) {
		deliveries_handler.store(handler);
		real(source, &countingDeliveriesHandler);
		return;
	}
	real(source, handler);
}

extern "C" dispatch_source_t dispatch_source_create(
	dispatch_source_type_t type, uintptr_t handle, uintptr_t mask,
	dispatch_queue_t queue)
{
	using Create = dispatch_source_t (*)(
		dispatch_source_type_t, uintptr_t, uintptr_t, dispatch_queue_t);
	static const auto real = reinterpret_cast<Create>(
		dlsym(RTLD_NEXT, "dispatch_source_create"));
	if (type == DISPATCH_SOURCE_TYPE_DATA_OR) {
		/* The response's signal for its policy queue. */
		const dispatch_source_t source = real(type, handle, mask, queue);
		deliveries_source.store(source);
		policy_queue.store(queue);
		return source;
	}
	if (type != DISPATCH_SOURCE_TYPE_MEMORYPRESSURE)
		return real(type, handle, mask, queue);
	const dispatch_source_t source =
		real(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
	pressure_source.store(source);
	pressure_queue.store(queue);
	return source;
}

namespace {

using uml::runtime::MemoryPressureResponse;
using uml::runtime::PressureActions;
using uml::runtime::PressureFigures;
using uml::runtime::PressureLevel;
using uml::runtime::PressureReaction;
using uml::runtime::PressureReport;
using uml::runtime::PressureTiming;
using uml::runtime::TrimKind;
using uml::runtime::TrimOutcome;
using uml::testing::require;
using uml::testing::waitUntil;
using Outcome = MemoryPressureResponse::Outcome;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr std::uint64_t second = 1'000'000'000ULL;
constexpr std::uint64_t millisecond = 1'000'000ULL;

/* The memory pressure source the first watch made. */
dispatch_source_t raisable()
{
	const dispatch_source_t source = pressure_source.load();
	require(source != nullptr, "no memory pressure source was made");
	return source;
}

/* Returns once the source's handler has recorded what was merged into it and
 * the policy queue has handled the deliveries signalled so far: both are serial
 * queues, so a call on each, made after the merge, waits for what is ahead. */
void settle()
{
	dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	dispatch_sync_f(policy_queue.load(), nullptr, [](void *) {});
	dispatch_sync_f(policy_queue.load(), nullptr, [](void *) {});
}

/* Raises `flags` on the source and returns once its response has been handled
 * (for a single delivery). */
void raiseAndWait(std::uint64_t flags)
{
	dispatch_source_merge_data(raisable(), flags);
	settle();
}

/* A clock, a process and a guest the test moves by hand. */
class FakeActions final : public PressureActions {
public:
	std::uint64_t now_ns() noexcept override
	{
		const std::lock_guard lock{mutex};
		return now;
	}

	PressureFigures figures() noexcept override
	{
		const std::lock_guard lock{mutex};
		return current;
	}

	std::uint64_t reclaim_gpu() noexcept override
	{
		const std::lock_guard lock{mutex};
		calls.emplace_back("gpu");
		current.graphics -= std::min(gpu_bytes, current.graphics);
		current.footprint -= std::min(gpu_bytes, current.footprint);
		return gpu_freed;
	}

	/* The gradual trimmer's requests are one short message each, with an
	 * outcome of their own. The trim of all the cache is recorded as "trim"
	 * and is never what the response asks for: every test that reaches a
	 * level asserts that no such call was made. */
	TrimOutcome trim_page_cache(TrimKind kind) noexcept override
	{
		std::function<void()> inside;
		{
			const std::lock_guard lock{mutex};
			calls.emplace_back(kind == TrimKind::once ? "trim" :
				kind == TrimKind::gradual_on ? "trim-on" : "trim-off");
			inside = inside_trim;
		}
		if (inside)
			inside();
		const std::lock_guard lock{mutex};
		if (kind == TrimKind::once)
			return TrimOutcome::done;
		/* The scripted answers come first, then the standing ones. */
		if (script_next < gradual_script.size())
			return gradual_script[script_next++];
		if (kind == TrimKind::gradual_off && off_outcome)
			return *off_outcome;
		return gradual_outcome;
	}

	void log(std::string_view line) noexcept override
	{
		const std::lock_guard lock{mutex};
		lines.emplace_back(line);
	}

	/* What the application heard, and how much the response had done when it
	 * heard it. */
	struct Heard final {
		PressureReport report;
		std::size_t calls_before;
		std::size_t lines_before;
	};

	void report(const PressureReport &heard) noexcept override
	{
		const std::lock_guard lock{mutex};
		reports.push_back({heard, calls.size(), lines.size()});
	}

	[[nodiscard]] std::vector<Heard> reports_taken()
	{
		const std::lock_guard lock{mutex};
		return reports;
	}

	void advance(std::uint64_t nanoseconds)
	{
		const std::lock_guard lock{mutex};
		now += nanoseconds;
	}

	[[nodiscard]] std::vector<std::string> calls_taken()
	{
		const std::lock_guard lock{mutex};
		return calls;
	}

	[[nodiscard]] std::vector<std::string> lines_taken()
	{
		const std::lock_guard lock{mutex};
		return lines;
	}

	/* The trims of all the cache: none, as the response never asks for one. */
	[[nodiscard]] std::size_t trims()
	{
		return calls_named("trim");
	}

	/* Every request to the guest, of any kind. */
	[[nodiscard]] std::size_t requests()
	{
		return calls_named("trim") + calls_named("trim-on") +
			calls_named("trim-off");
	}

	[[nodiscard]] std::size_t calls_named(const char *name)
	{
		const std::lock_guard lock{mutex};
		std::size_t count = 0;
		for (const auto &call : calls)
			count += call == name ? 1U : 0U;
		return count;
	}

	std::mutex mutex;
	std::uint64_t now = 100 * second;
	PressureFigures current{
		.footprint = 2026 * mib,
		.guest_held = 1500 * mib,
		.graphics = 150 * mib,
		.wire_failures = 16,
	};
	std::uint64_t gpu_bytes = 12 * mib;
	std::uint64_t gpu_freed = 3;
	/* What the gradual trimmer's requests answer. */
	TrimOutcome gradual_outcome = TrimOutcome::done;
	/* The answers of the next requests of the gradual trimmer, in order, of
	 * either kind; and what a request to turn it off answers when it is not
	 * the same as turning it on. */
	std::vector<TrimOutcome> gradual_script;
	std::size_t script_next = 0;
	std::optional<TrimOutcome> off_outcome;
	std::function<void()> inside_trim;
	std::vector<std::string> calls;
	std::vector<std::string> lines;
	std::vector<Heard> reports;
};

[[nodiscard]] bool contains(const std::string &text, const char *part)
{
	return text.find(part) != std::string::npos;
}

void theLevelsOfTheSourceAreTheMostSeriousFlag()
{
	using uml::runtime::pressure_level_from_dispatch;
	require(pressure_level_from_dispatch(0) == PressureLevel::normal);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_NORMAL) ==
		PressureLevel::normal);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_WARN) ==
		PressureLevel::warn);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_CRITICAL) ==
		PressureLevel::critical);
	/* Events the handler had not yet run for arrive together. */
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_NORMAL |
			DISPATCH_MEMORYPRESSURE_WARN) == PressureLevel::warn);
	require(pressure_level_from_dispatch(DISPATCH_MEMORYPRESSURE_WARN |
			DISPATCH_MEMORYPRESSURE_CRITICAL) == PressureLevel::critical);
}

[[nodiscard]] std::vector<PressureReport> reportsOf(FakeActions &actions);
[[nodiscard]] bool sameReports(const std::vector<PressureReport> &got,
	const std::vector<PressureReport> &wanted);

void theNormalLevelTakesNothing()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_taken().empty());
	require(actions.lines_taken().empty(), "no change, no line");

	/* A warning takes nothing either, of the guest or the GPU: the change is
	 * its one line, and the way back to normal is another. */
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_taken().empty());
	const auto lines = actions.lines_taken();
	require(lines.size() == 2, "warn change, normal change");
	require(contains(lines[1], "memory pressure normal (was warn)"), lines[1]);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.lines_taken().size() == 2);
	require(actions.requests() == 0 && response.counts().responses == 0);
}

void aWarningAsksTheGuestForNothing()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(actions.calls_taken().empty(), "a warning took the guest or the GPU");
	require(response.retry_delay_ns() == 0);
	const auto lines = actions.lines_taken();
	require(lines.size() == 1, "the change, and no response");
	require(lines[0] ==
		"uml: memory pressure warn (was normal): footprint 2026 MiB, "
		"16 guest RAM wire failures", lines[0]);
	/* However often it comes. */
	for (int event = 0; event < 20; ++event) {
		actions.advance(second);
		require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	}
	require(actions.calls_taken().empty());
	const auto counts = response.counts();
	require(counts.responses == 0 && counts.busy == 0 && counts.retries == 0);
	/* The application hears each one, with no response behind it. */
	const auto heard = reportsOf(actions);
	require(heard.size() == 21);
	for (const auto &report : heard)
		require(report.level == PressureLevel::warn &&
			report.reaction == PressureReaction::none);
}

void aCriticalLevelFreesTheGpuCachesAndAsksForTheGradualTrimmerNeverATrimOfAllTheCache()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	/* The GPU's caches and the trimmer, and no trim of all the cache. */
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on"}));
	require(actions.trims() == 0, "a critical level trimmed all the cache");
	const auto lines = actions.lines_taken();
	require(lines.size() == 2, "the change and the response");
	require(lines[0] ==
		"uml: memory pressure critical (was normal): footprint 2026 MiB, "
		"16 guest RAM wire failures", lines[0]);
	/* 2026 - 12 graphics = 2014; the guest's RAM is not touched. */
	require(lines[1] ==
		"uml: memory pressure critical: footprint 2026 -> 2014 MiB; "
		"released 12 MiB graphics (3 cached allocations); guest page cache "
		"trimmer on (done); 16 guest RAM wire failures",
		lines[1]);
	require(actions.figures().guest_held == 1500 * mib);

	/* The next one asks the trimmer on again (the guest side answers that it
	 * already is), so that a request that was lost is not owed for ever. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 2 && actions.calls_named("trim-on") == 2);
	require(actions.trims() == 0);

	/* And the way back to normal ends it. */
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 1);
	require(actions.trims() == 0);

	/* A critical level whose trimmer request found the guest busy still frees
	 * the GPU's caches, and says what became of the request. */
	FakeActions busy;
	busy.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse waiting{busy};
	require(waiting.on_level(PressureLevel::critical) == Outcome::responded);
	require((busy.calls_taken() == std::vector<std::string>{"gpu", "trim-on"}));
	require(contains(busy.lines_taken().back(),
		"; guest page cache trimmer on (busy); "), busy.lines_taken().back());
}

void aCriticalLevelSaysWhatBecameOfTheTrimmerAndWhatTheGpuFreed()
{
	/* Each ending has its own word, and the GPU's part is said beside it. */
	const struct {
		TrimOutcome outcome;
		const char *words;
	} endings[] = {
		{TrimOutcome::unreachable, "trimmer on (no console)"},
		{TrimOutcome::quiesced, "trimmer on (guest quiesced)"},
		{TrimOutcome::refused, "trimmer on (refused)"},
		{TrimOutcome::busy, "trimmer on (busy)"},
		{TrimOutcome::unanswered, "trimmer on (unanswered)"},
		{TrimOutcome::sent, "trimmer on (sent)"},
		{TrimOutcome::done, "trimmer on (done)"},
	};
	for (const auto &ending : endings) {
		FakeActions actions;
		actions.gradual_outcome = ending.outcome;
		actions.gpu_freed = 0;
		actions.gpu_bytes = 0;
		MemoryPressureResponse response{actions};
		require(response.on_level(PressureLevel::critical) ==
			Outcome::responded);
		const auto lines = actions.lines_taken();
		require(contains(lines.back(),
			"released 0 MiB graphics (0 cached allocations); guest page cache "),
			lines.back());
		require(contains(lines.back(), ending.words), lines.back());
		require(actions.trims() == 0);
	}

	/* A response that frees little says so in kibibytes under a mebibyte. */
	FakeActions small;
	small.gpu_bytes = 300 * 1024;
	small.gpu_freed = 1;
	MemoryPressureResponse little{small};
	require(little.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(small.lines_taken().back(),
		"released 300 KiB graphics (1 cached allocations); "),
		small.lines_taken().back());
}

void aCriticalLevelAlwaysStartsOne()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 1);
	/* No interval stands in its way, a millisecond after the last. */
	actions.advance(millisecond);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 2);
	/* Nor does a warning between two of them. */
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 3);
	require(actions.calls_named("trim-on") == 3);
	require(actions.trims() == 0);
}

void aCriticalLevelAfterAWarningAndAWarningAfterACriticalLevel()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	/* A warning alone takes nothing: the critical level after it frees the
	 * GPU's caches and turns the trimmer on. */
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(actions.calls_taken().empty());
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on"}));
	/* The warning after it is the pressure easing: the trimmer is turned off,
	 * once, and the GPU is left alone. */
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on", "trim-off"}));
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(actions.calls_taken().size() == 3, "a second warning asked again");
	/* A critical level after that turns it on again. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 2 && actions.calls_named("trim-on") == 2);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_taken().back() == "trim-off");
	require(actions.calls_named("trim-off") == 2 && actions.trims() == 0);
}

/* ---- The trimmer is on exactly while the level held is critical. ---- */

void aLevelThatEasesFromCriticalAsksTheTrimmerOffAtOnce()
{
	for (const auto easing : {PressureLevel::warn, PressureLevel::normal}) {
		FakeActions actions;
		MemoryPressureResponse response{actions};
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		require(response.retry_delay_ns() == 0);
		actions.advance(second);
		require(response.on_level(easing) == Outcome::nothing);
		/* At once, with nothing pending and the GPU left alone. */
		require((actions.calls_taken() ==
			std::vector<std::string>{"gpu", "trim-on", "trim-off"}));
		require(response.retry_delay_ns() == 0 && response.counts().retries == 0);
		require(response.on_retry() == Outcome::nothing);
		/* The level lines said what the system did, and nothing more is
		 * written. */
		require(actions.lines_taken().size() == 3,
			"the critical change and response, and the easing level's change");
		require(actions.trims() == 0);
	}
}

void levelsThatAlternateAreFollowedAsTheyCome()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	/* Critical, warning, critical: on, off, on. */
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{
		"gpu", "trim-on", "trim-off", "gpu", "trim-on"}));
	/* And so on, a second apart: each critical level frees the GPU's caches and
	 * asks the trimmer on, each warning asks it off, and nothing is pending
	 * for a timer. */
	for (int cycle = 0; cycle < 100; ++cycle) {
		actions.advance(second);
		require(response.on_level(PressureLevel::warn) == Outcome::nothing);
		require(response.retry_delay_ns() == 0);
		actions.advance(second);
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		require(response.retry_delay_ns() == 0);
	}
	require(actions.calls_named("trim-on") == 102 &&
		actions.calls_named("trim-off") == 101 &&
		actions.calls_named("gpu") == 102);
	require(actions.trims() == 0);
	/* The last word is the level the response holds: critical keeps the
	 * trimmer on, and the easing after it ends it. */
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_taken().back() == "trim-off");
	require(actions.calls_named("trim-off") == 102);
}

void aWarningThatMeetsABusyGuestKeepsTheEndPendingUntilACriticalLevelTakesIt()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	/* The end found the guest busy: it is tried again, as a normal level's is. */
	require(response.retry_delay_ns() == PressureTiming{}.retry_interval_ns);
	require(response.on_retry() == Outcome::retried);
	require(actions.calls_named("trim-off") == 2);
	/* The pressure comes back before it went through: the request that is
	 * pending is no longer wanted, and the critical level asks for itself. */
	actions.gradual_outcome = TrimOutcome::done;
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() == 0);
	require(actions.calls_taken().back() == "trim-on");
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 2 && actions.trims() == 0);
}

void aResponseThatIsRunningIsNotJoined()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	std::vector<Outcome> met;
	actions.inside_trim = [&] {
		/* Other events, from a thread that is not the one that runs. */
		met.push_back(response.on_level(PressureLevel::warn));
		met.push_back(response.on_level(PressureLevel::critical));
	};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.inside_trim = nullptr;
	/* A warning starts no response of its own; the critical level is not
	 * joined, and is the last word: the trimmer stays on. */
	require(met == (std::vector<Outcome>{Outcome::nothing, Outcome::busy}));
	require(actions.requests() == 1 && actions.calls_named("gpu") == 1);
	require(response.counts().busy == 1);
	require(response.retry_delay_ns() == 0, "the warning's end was not taken back");
	/* Once it ended, a critical level is answered, and says what it skipped. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(actions.lines_taken().back(),
		"1 events skipped since the last response"),
		actions.lines_taken().back());
	require(actions.trims() == 0);
}

void aLevelThatEasesDuringAResponseEndsTheTrimmerWhenItReturns()
{
	for (const auto easing : {PressureLevel::warn, PressureLevel::normal}) {
		FakeActions actions;
		MemoryPressureResponse response{actions};
		/* The pressure eases from another thread while the response's request
		 * is in the guest's hands: that response ends the trimmer when it
		 * returns. */
		actions.inside_trim = [&] {
			actions.inside_trim = nullptr;
			require(response.on_level(easing) == Outcome::nothing);
		};
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		require((actions.calls_taken() ==
			std::vector<std::string>{"gpu", "trim-on", "trim-off"}));
		require(response.retry_delay_ns() == 0);
	}
}

void anEventOnAnotherThreadMeetsTheRunningTrim()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	std::mutex gate;
	std::condition_variable changed;
	bool inside = false;
	bool release = false;
	actions.inside_trim = [&] {
		std::unique_lock lock{gate};
		inside = true;
		changed.notify_all();
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return release; }), "the test never released the trim");
	};
	std::thread running{[&] {
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
	}};
	{
		std::unique_lock lock{gate};
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return inside; }), "the trim never began");
	}
	/* It returns at once and starts nothing. */
	require(response.on_level(PressureLevel::critical) == Outcome::busy);
	require(actions.requests() == 1 && actions.calls_named("gpu") == 1);
	{
		const std::lock_guard lock{gate};
		release = true;
	}
	changed.notify_all();
	running.join();
	require(actions.requests() == 1);
	actions.inside_trim = nullptr;
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(actions.calls_named("gpu") == 2 && actions.requests() == 2);
	require(actions.trims() == 0);
}

/* ---- A trimmer request that met another lifecycle call is tried again. ---- */

void aBusyTrimmerRequestIsTriedAgain()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.retry_delay_ns() == 0, "nothing pending yet");
	require(response.on_retry() == Outcome::nothing);
	require(actions.requests() == 0);

	/* A critical level whose request found the guest busy keeps one pending;
	 * the GPU's caches were freed at once. */
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(actions.lines_taken().back(), "trimmer on (busy)"),
		actions.lines_taken().back());
	require(response.retry_delay_ns() == timing.retry_interval_ns);
	require(actions.requests() == 1 && actions.calls_named("gpu") == 1);

	/* The guest is free when the retry comes: it sends the request and the
	 * response's last line says what it got. */
	actions.gradual_outcome = TrimOutcome::done;
	require(response.on_retry() == Outcome::retried);
	require(actions.requests() == 2);
	require(response.retry_delay_ns() == 0, "one pending request, tried once");
	require(actions.lines_taken().back() ==
		"uml: memory pressure critical (retry 1 of 4): guest page cache "
		"trimmer on (done); 16 guest RAM wire failures",
		actions.lines_taken().back());
	require(response.on_retry() == Outcome::nothing);
	require(actions.requests() == 2);
	require(response.counts().retries == 1);
	/* The retry freed nothing more and asked for no trim of all the cache. */
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on", "trim-on"}));
}

void aRetryThatIsStillBusyStopsAfterItsAttempts()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	PressureTiming timing;
	timing.retry_attempts = 3;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	const auto lines_before = actions.lines_taken().size();
	for (unsigned attempt = 1; attempt <= 3; ++attempt) {
		require(response.retry_delay_ns() == timing.retry_interval_ns,
			std::to_string(attempt));
		require(response.on_retry() == Outcome::retried);
	}
	/* Three tries and no more, one line for the last. */
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.requests() == 4);
	const auto lines = actions.lines_taken();
	require(lines.size() == lines_before + 1, "a line for each try");
	require(contains(lines.back(), "(retry 3 of 3)") &&
		contains(lines.back(), "trimmer on (busy)"),
		lines.back());

	/* No retries configured, none pending. */
	FakeActions never;
	never.gradual_outcome = TrimOutcome::busy;
	PressureTiming none;
	none.retry_attempts = 0;
	MemoryPressureResponse once{never, none};
	require(once.on_level(PressureLevel::critical) == Outcome::responded);
	require(once.retry_delay_ns() == 0);
}

void aRetryIsNotForAGuestTheHostHasQuiesced()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
	/* The guest was quiesced meanwhile: the request said so, and that is the
	 * end of it. */
	actions.gradual_outcome = TrimOutcome::quiesced;
	require(response.on_retry() == Outcome::retried);
	require(response.retry_delay_ns() == 0);
	require(actions.requests() == 2);
	require(contains(actions.lines_taken().back(),
		"trimmer on (guest quiesced)"), actions.lines_taken().back());
	require(response.on_retry() == Outcome::nothing);
	require(actions.requests() == 2);

	/* Neither does a request that was refused or went unanswered come
	 * again. */
	for (const auto ending : {TrimOutcome::refused, TrimOutcome::unanswered,
		     TrimOutcome::quiesced}) {
		FakeActions other;
		other.gradual_outcome = ending;
		MemoryPressureResponse once{other};
		require(once.on_level(PressureLevel::critical) == Outcome::responded);
		require(once.retry_delay_ns() == 0);
	}
}

void aLaterResponseOrAnEasingLevelTakesThePendingRequest()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);

	/* A warning is the pressure easing: the request for the trimmer on is no
	 * longer wanted, and the trimmer is asked off instead. */
	actions.gradual_outcome = TrimOutcome::done;
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on", "trim-off"}));

	/* A response of its own tries for itself: that request replaces the pending
	 * one, and when it finds the guest free nothing is left to retry. */
	actions.gradual_script = {TrimOutcome::busy};
	actions.script_next = 0;
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-on") == 3);

	/* The way back to normal ends the pressure and the retry with it. */
	actions.gradual_script.push_back(TrimOutcome::busy);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.retry_delay_ns() != 0);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.trims() == 0);

	/* A response that is busy again starts a fresh set of attempts. */
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.on_retry() == Outcome::retried);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	for (unsigned attempt = 0; attempt < 4; ++attempt)
		require(response.on_retry() == Outcome::retried);
	require(response.on_retry() == Outcome::nothing);
}

void anEasingLevelDuringAResponseLeavesNoRequestPending()
{
	for (const auto easing : {PressureLevel::normal, PressureLevel::warn}) {
		FakeActions actions;
		MemoryPressureResponse response{actions};
		/* The pressure eases from another thread while the response's request
		 * is in the guest's hands, and the request then reports busy. */
		actions.gradual_script = {TrimOutcome::busy};
		actions.inside_trim = [&] {
			actions.inside_trim = nullptr;
			require(response.on_level(easing) == Outcome::nothing);
		};
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		require(response.retry_delay_ns() == 0,
			"a request was left for a pressure that ended");
		require(response.on_retry() == Outcome::nothing);
		/* The GPU's caches, the request, and the end that the response owed
		 * once it returned. */
		require((actions.calls_taken() ==
			std::vector<std::string>{"gpu", "trim-on", "trim-off"}));

		/* Without it the same busy request is kept for a retry. */
		actions.advance(second);
		actions.gradual_script.push_back(TrimOutcome::busy);
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		require(response.retry_delay_ns() != 0);
	}
}

void aBusyGuestKeepsTheEndOfTheGradualTrimmerPending()
{
	for (const auto easing : {PressureLevel::normal, PressureLevel::warn}) {
		FakeActions actions;
		MemoryPressureResponse response{actions};
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		actions.gradual_outcome = TrimOutcome::busy;
		actions.advance(second);
		require(response.on_level(easing) == Outcome::nothing);
		require(response.retry_delay_ns() != 0);
		/* The guest is free when the retry comes. */
		actions.gradual_outcome = TrimOutcome::done;
		require(response.on_retry() == Outcome::retried);
		require(response.retry_delay_ns() == 0);
		const std::string line = std::string{"uml: memory pressure "} +
			std::string{uml::runtime::pressure_level_name(easing)} +
			" (retry 1 of 4): guest page cache trimmer off (done); 16 guest "
			"RAM wire failures";
		require(actions.lines_taken().back() == line,
			actions.lines_taken().back());
		require((actions.calls_taken() == std::vector<std::string>{
			"gpu", "trim-on", "trim-off", "trim-off"}));
		/* Ended: the next level asks no more. */
		require(response.on_level(easing) == Outcome::nothing);
		require(actions.requests() == 3);
	}

	/* A pressure that came back first takes the request: it asks for the
	 * trimmer on itself. */
	FakeActions other;
	MemoryPressureResponse again{other};
	require(again.on_level(PressureLevel::critical) == Outcome::responded);
	other.gradual_outcome = TrimOutcome::busy;
	require(again.on_level(PressureLevel::normal) == Outcome::nothing);
	require(again.retry_delay_ns() != 0);
	other.gradual_outcome = TrimOutcome::done;
	other.advance(second);
	require(again.on_level(PressureLevel::critical) == Outcome::responded);
	require(again.retry_delay_ns() == 0);
	require(again.on_retry() == Outcome::nothing);
	require((other.calls_taken() == std::vector<std::string>{
		"gpu", "trim-on", "trim-off", "gpu", "trim-on"}));
}

void aRetryIsNotJoinedToAResponseThatIsRunning()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	std::vector<Outcome> met;
	actions.inside_trim = [&] {
		met.push_back(response.on_retry());
		met.push_back(response.on_level(PressureLevel::critical));
	};
	require(response.on_retry() == Outcome::retried);
	actions.inside_trim = nullptr;
	require(met == (std::vector<Outcome>{Outcome::busy, Outcome::busy}));
	require(actions.requests() == 2);
	require(actions.calls_named("gpu") == 1 && actions.trims() == 0);
}

/* ---- The trimmer follows the level; its requests are retried within a
 * finite allowance. ---- */

void aCriticalLevelAsksForTheTrimmerAfterAnEndThatWasNeverSent()
{
	FakeActions actions;
	actions.off_outcome = TrimOutcome::unreachable;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	/* The end could not be sent: the policy still holds the trimmer as asked
	 * on, and the guest side holds what the application and the response last
	 * wanted. */
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	actions.advance(second);
	/* The pressure is back: the request for the trimmer to be on is made
	 * again, so that what the guest side wanted of it is on again, whatever it
	 * recorded for the end. */
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{
		"gpu", "trim-on", "trim-off", "gpu", "trim-on"}));
	require(response.retry_delay_ns() == 0, "the end was kept for a retry");
}

void aCriticalLevelTakesAPendingEndOfTheTrimmer()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.retry_delay_ns() == timing.retry_interval_ns,
		"the end of the trimmer is pending");
	/* The pressure is back: the end that was never sent is no longer wanted,
	 * and what is pending now is the request for the trimmer to be on. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() == std::vector<std::string>{
		"gpu", "trim-on", "trim-off", "gpu", "trim-on"}));
	actions.gradual_outcome = TrimOutcome::done;
	require(response.retry_delay_ns() == timing.retry_interval_ns);
	require(response.on_retry() == Outcome::retried);
	require(actions.calls_taken().back() == "trim-on" &&
		actions.calls_named("trim-off") == 1,
		"the end of the trimmer outlived the pressure");
	require(response.retry_delay_ns() == 0);
}

void aBusyEndOfTheTrimmerIsTriedAgainAtMostFourTimes()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 1);
	for (unsigned retry = 1; retry <= 4; ++retry) {
		require(response.retry_delay_ns() == timing.retry_interval_ns,
			std::to_string(retry));
		require(response.on_retry() == Outcome::retried);
		/* One request for each call of the timer, and no second set of
		 * attempts behind it. */
		require(actions.calls_named("trim-off") == 1U + retry,
			std::to_string(retry));
	}
	require(response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 5);
	require(response.counts().retries == 4);
	require(contains(actions.lines_taken().back(), "(retry 4 of 4)") &&
		contains(actions.lines_taken().back(), "trimmer off (busy)"),
		actions.lines_taken().back());
}

void aCriticalLevelsTrimmerRequestIsTriedAgainWithinItsAllowance()
{
	/* A busy or unreachable guest: the request is asked once and four times
	 * more, and no more, and the GPU's caches are freed once, by the response. */
	for (const auto ending : {TrimOutcome::busy, TrimOutcome::unreachable}) {
		FakeActions actions;
		actions.gradual_outcome = ending;
		MemoryPressureResponse response{actions};
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		for (unsigned retry = 1; retry <= 4; ++retry) {
			require(response.retry_delay_ns() != 0, std::to_string(retry));
			require(response.on_retry() == Outcome::retried);
		}
		require(response.retry_delay_ns() == 0);
		require(response.on_retry() == Outcome::nothing);
		require(actions.calls_named("trim-on") == 5);
		require(actions.calls_named("gpu") == 1);
		require(actions.trims() == 0, "a retry trimmed all the cache");
		/* The guest is free again and the pressure is still there: the next
		 * critical level asks once more, and nothing is left pending. */
		actions.gradual_outcome = TrimOutcome::done;
		actions.advance(second);
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
		require(actions.calls_named("trim-on") == 6 &&
			response.retry_delay_ns() == 0);
	}
}

void anEndOfTheTrimmerThatCouldNotBeSentStaysOwedAndARefusedOneDoesNot()
{
	FakeActions actions;
	actions.off_outcome = TrimOutcome::unreachable;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	for (unsigned retry = 1; retry <= 4; ++retry)
		require(response.on_retry() == Outcome::retried);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 5);
	/* The guest was never told: the next level that eases asks again. */
	actions.off_outcome = TrimOutcome::done;
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 6);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(actions.calls_named("trim-off") == 6, "asked again after it was taken");

	/* A kernel without the command has nothing to turn off. */
	FakeActions refused;
	refused.off_outcome = TrimOutcome::refused;
	MemoryPressureResponse without{refused};
	require(without.on_level(PressureLevel::critical) == Outcome::responded);
	refused.advance(second);
	require(without.on_level(PressureLevel::normal) == Outcome::nothing);
	require(without.retry_delay_ns() == 0);
	require(without.on_level(PressureLevel::normal) == Outcome::nothing);
	require(refused.calls_named("trim-off") == 1);
}

void aCriticalLevelThatMeetsTheRetriedEndOfTheTrimmerTurnsItOnAgain()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.gradual_outcome = TrimOutcome::busy;
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	actions.gradual_outcome = TrimOutcome::done;
	Outcome met = Outcome::nothing;
	/* The pressure comes back while the retried end is in the guest's hands:
	 * the event finds a retry running and starts nothing. */
	actions.inside_trim = [&] {
		actions.inside_trim = nullptr;
		met = response.on_level(PressureLevel::critical);
	};
	require(response.on_retry() == Outcome::retried);
	require(met == Outcome::busy);
	require((actions.calls_taken() == std::vector<std::string>{
		"gpu", "trim-on", "trim-off", "trim-off", "trim-on"}));
	require(response.retry_delay_ns() == 0);
}

/* ---- The level the response holds returns to normal when the source is
 * quiet. The clock is the one the policy reads, the actions' now_ns (in the
 * host CLOCK_UPTIME_RAW, which stops while the device sleeps, not while the
 * process alone is suspended). ---- */

void theLevelReturnsToNormalWhenTheSourceStaysQuiet()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.quiet_delay_ns() == 0, "a normal level has no period");
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(response.quiet_delay_ns() == timing.quiet_period_ns);
	/* A timer that fires early finds the period not over, and says when. */
	actions.advance(timing.quiet_period_ns - second);
	require(response.on_retry() == Outcome::nothing);
	require(response.quiet_delay_ns() == second);
	actions.advance(second);
	require(response.on_retry() == Outcome::quiet);
	/* The way a delivered normal level goes: nothing asked of the guest, one
	 * line, one report. */
	require(actions.calls_taken().empty());
	require(contains(actions.lines_taken().back(),
		"uml: memory pressure normal (was warn)") &&
		contains(actions.lines_taken().back(), "no level for 240 s"),
		actions.lines_taken().back());
	require(sameReports(reportsOf(actions), {
		{PressureLevel::warn, PressureReaction::none},
		{PressureLevel::normal, PressureReaction::none},
	}));
	require(response.quiet_delay_ns() == 0 && response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing,
		"the return to normal was made twice");
	actions.advance(timing.quiet_period_ns);
	require(response.on_retry() == Outcome::nothing);
	/* The next warning is a new episode. */
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(response.quiet_delay_ns() == timing.quiet_period_ns);
}

void aCriticalLevelReturnsToNormalWhenTheSourceStaysQuiet()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.quiet_delay_ns() == timing.quiet_period_ns);
	actions.advance(timing.quiet_period_ns);
	require(response.on_retry() == Outcome::quiet);
	/* The GPU's caches and the trimmer on at the level; the trimmer off at the
	 * return, as a delivered normal level asks. */
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on", "trim-off"}));
	require(actions.trims() == 0);
	require(contains(actions.lines_taken().back(),
		"uml: memory pressure normal (was critical)") &&
		contains(actions.lines_taken().back(), "no level for 240 s"),
		actions.lines_taken().back());
	require(sameReports(reportsOf(actions), {
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::normal, PressureReaction::none},
	}));
	require(response.quiet_delay_ns() == 0 && response.retry_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
}

void aLevelDeliveredInsideThePeriodMovesItsEnd()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(timing.quiet_period_ns - 40 * second);
	require(response.quiet_delay_ns() == 40 * second);
	/* Another level, from the source, starts the period again. */
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	/* The period runs from the delivery, and the response that follows it
	 * waits for nothing. */
	const std::uint64_t left = response.quiet_delay_ns();
	require(left == timing.quiet_period_ns);
	actions.advance(left - second);
	require(response.on_retry() == Outcome::nothing,
		"the period ran from the first level");
	require(actions.calls_named("trim-off") == 0);
	actions.advance(second);
	require(response.on_retry() == Outcome::quiet);
	require(actions.calls_named("trim-off") == 1);
	require(contains(actions.lines_taken().back(),
		"normal (was critical)"), actions.lines_taken().back());
}

void aDeliveredNormalLevelEndsThePeriod()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.quiet_delay_ns() == 0, "the period outlived the level");
	actions.advance(2 * timing.quiet_period_ns);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_taken().empty());
	require(sameReports(reportsOf(actions), {
		{PressureLevel::warn, PressureReaction::none},
		{PressureLevel::normal, PressureReaction::none},
	}), "the timer said normal a second time");
}

/* A guest the host holds is not woken for the end of the trimmer, and a timer
 * that fires after the resume asks for nothing more. */
void aHeldGuestIsNotTrimmedBecauseOfAStaleTimer()
{
	FakeActions actions;
	const PressureTiming timing;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	/* A timer that fires before the clock has reached the end of the period
	 * (the device slept meanwhile, and the clock stopped) finds it not over. */
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 0);
	/* The period ends with the guest held, as it does for a process suspended
	 * for longer than the period when it resumes: the return asks the trimmer
	 * off, the host records the end for the guest's resume, and the response
	 * keeps nothing pending. */
	actions.gradual_outcome = TrimOutcome::quiesced;
	actions.advance(timing.quiet_period_ns);
	require(response.on_retry() == Outcome::quiet);
	require(actions.calls_named("trim-off") == 1);
	require(response.retry_delay_ns() == 0 && response.quiet_delay_ns() == 0);
	/* The resume, and a timer that was left set. */
	actions.gradual_outcome = TrimOutcome::done;
	actions.advance(2 * timing.quiet_period_ns);
	require(response.on_retry() == Outcome::nothing);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on", "trim-off"}),
		"a stale timer asked the guest for something");
}

void aQuietPeriodOfZeroNeverReturnsTheLevel()
{
	FakeActions actions;
	PressureTiming timing;
	timing.quiet_period_ns = 0;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(1000 * second);
	require(response.quiet_delay_ns() == 0);
	require(response.on_retry() == Outcome::nothing);
	require(actions.calls_named("trim-off") == 0);
}

/* ---- What the host reads of the system's level. ---- */

/* A clock that holds its first reader, as a thread the scheduler stops between
 * reading the time and using it would. */
class HeldClock final : public PressureActions {
public:
	std::uint64_t now_ns() noexcept override
	{
		std::unique_lock lock{mutex};
		const unsigned reader = ++readers;
		++inside;
		widest = std::max(widest, inside);
		changed.notify_all();
		if (reader == 1U)
			(void)changed.wait_for(lock, std::chrono::seconds{20},
				[&] { return released; });
		--inside;
		return now += reader == 1U ? 0U : 2 * second;
	}

	PressureFigures figures() noexcept override { return {}; }
	std::uint64_t reclaim_gpu() noexcept override { return 0; }
	TrimOutcome trim_page_cache(TrimKind) noexcept override
	{
		trims.fetch_add(1);
		return TrimOutcome::done;
	}
	void log(std::string_view) noexcept override {}
	void report(const PressureReport &) noexcept override {}

	std::mutex mutex;
	std::condition_variable changed;
	std::uint64_t now = 100 * second;
	unsigned readers = 0;
	unsigned inside = 0;
	unsigned widest = 0;
	bool released = false;
	std::atomic<unsigned> trims{0};
};

void theClockIsReadInTheOrderOfTheMutex()
{
	HeldClock clock;
	MemoryPressureResponse response{clock};
	std::thread first{[&] {
		require(response.on_level(PressureLevel::critical) == Outcome::responded);
	}};
	{
		std::unique_lock lock{clock.mutex};
		require(clock.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return clock.inside == 1U; }), "the first call never read");
	}
	/* A second event meets the first one's reading of the clock: it waits for
	 * the policy's mutex instead of reading an older or newer time, and finds
	 * the first one's response started when it gets the mutex. */
	Outcome second_outcome = Outcome::nothing;
	std::thread second_call{[&] {
		second_outcome = response.on_level(PressureLevel::critical);
	}};
	require(!waitUntil([&] {
		const std::lock_guard lock{clock.mutex};
		return clock.readers > 1U;
	}, std::chrono::milliseconds{300}),
		"a second thread read the clock while the first held the mutex");
	{
		const std::lock_guard lock{clock.mutex};
		clock.released = true;
	}
	clock.changed.notify_all();
	first.join();
	second_call.join();
	require(clock.widest == 1U, "two threads read the clock at once");
	/* Busy when it got the mutex while the response ran, and a response of its
	 * own once that had ended. */
	require(second_outcome == Outcome::busy ||
		second_outcome == Outcome::responded);
	/* The first response's request for the trimmer, and the second's. */
	require(clock.trims == (second_outcome == Outcome::responded ? 2U : 1U));
}

void aStormOfLevelChangesCannotFloodTheConsole()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	for (int flip = 0; flip < 10; ++flip) {
		(void)response.on_level(PressureLevel::warn);
		(void)response.on_level(PressureLevel::normal);
	}
	/* Twenty changes in one window: the burst's worth of lines. */
	std::size_t changes = 0;
	for (const auto &line : actions.lines_taken())
		changes += contains(line, "(was ") &&
			!contains(line, "released") ? 1U : 0U;
	require(changes == MemoryPressureResponse::log_burst,
		std::to_string(changes));
	require(response.counts().lines_dropped >= 16);
	/* The next window's first line says how many it left out. */
	actions.advance(MemoryPressureResponse::log_window_ns);
	(void)response.on_level(PressureLevel::warn);
	const auto lines = actions.lines_taken();
	const auto &last_change = *std::find_if(lines.rbegin(), lines.rend(),
		[](const std::string &line) { return contains(line, "(was "); });
	require(contains(last_change, "earlier lines left out"), last_change);
}

void aStormOfCriticalLevelsCannotFloodTheConsole()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	for (int event = 0; event < 12; ++event)
		require(response.on_level(PressureLevel::critical) ==
			Outcome::responded);
	/* Every one freed the GPU's caches and asked for the trimmer, and a few
	 * were written. */
	require(actions.calls_named("gpu") == 12 && actions.calls_named("trim-on") == 12);
	require(actions.trims() == 0);
	std::size_t responses = 0;
	for (const auto &line : actions.lines_taken())
		responses += contains(line, "released") ? 1U : 0U;
	require(responses == MemoryPressureResponse::log_burst,
		std::to_string(responses));
	actions.advance(MemoryPressureResponse::log_window_ns);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(contains(actions.lines_taken().back(),
		"8 earlier lines left out"), actions.lines_taken().back());
}

/* ---- The application hears each event, before the response's work. ---- */

[[nodiscard]] std::vector<PressureReport> reportsOf(FakeActions &actions)
{
	std::vector<PressureReport> reports;
	for (const auto &entry : actions.reports_taken())
		reports.push_back(entry.report);
	return reports;
}

[[nodiscard]] bool sameReports(const std::vector<PressureReport> &got,
	const std::vector<PressureReport> &wanted)
{
	if (got.size() != wanted.size())
		return false;
	for (std::size_t index = 0; index < got.size(); ++index) {
		if (got[index].level != wanted[index].level ||
		    got[index].reaction != wanted[index].reaction)
			return false;
	}
	return true;
}

void everyLevelIsReportedTheNormalLevelToo()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::normal) == Outcome::nothing);
	/* A warning and the way back are reports with nothing done for them; a
	 * normal level that changes nothing is reported as well, and the
	 * application keeps what it needs from it. */
	require(sameReports(reportsOf(actions), {
		{PressureLevel::normal, PressureReaction::none},
		{PressureLevel::warn, PressureReaction::none},
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::normal, PressureReaction::none},
	}));
}

void aReportComesBeforeTheWorkOfTheResponse()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on"}));
	const auto reports = actions.reports_taken();
	require(reports.size() == 1);
	/* Nothing was freed, nothing was sent to the guest and no line was
	 * written when the application heard it. */
	require(reports[0].calls_before == 0, "the report waited for the work");
	require(reports[0].lines_before == 0, "the report waited for a line");
	require(reports[0].report.level == PressureLevel::critical);
	require(reports[0].report.reaction == PressureReaction::responding);

	/* The same for the next one, whose work comes after what the first did. */
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	const auto both = actions.reports_taken();
	require(both.size() == 2);
	require(both[1].calls_before == 2, "the report waited for the work");
	require(both[1].lines_before == both[0].lines_before + 2,
		"the report waited for a line");
	require(both[1].report.reaction == PressureReaction::responding);
	require(actions.trims() == 0);
}

void aWarningIsReportedAsNoResponse()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	actions.advance(second);
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	actions.advance(second);
	require(response.on_level(PressureLevel::warn) == Outcome::nothing);
	require(sameReports(reportsOf(actions), {
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::warn, PressureReaction::none},
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::warn, PressureReaction::none},
	}));
	/* Each critical level freed the GPU's caches and asked for the trimmer on;
	 * each warning asked it off and nothing more. */
	require(actions.trims() == 0 && actions.calls_named("gpu") == 2 &&
		actions.calls_named("trim-on") == 2 &&
		actions.calls_named("trim-off") == 2,
		"a level asked the guest for more than the trimmer");
}

void aLevelMetWhileTheResponseRunsIsReportedAsUnderway()
{
	FakeActions actions;
	MemoryPressureResponse response{actions};
	std::vector<Outcome> met;
	std::vector<PressureReport> during;
	actions.inside_trim = [&] {
		actions.inside_trim = nullptr;
		/* The policy is concurrent: a level met on another thread while the
		 * request runs is reported at once, a critical one as underway. (The
		 * system delivers its events one at a time on one queue, so this is
		 * the policy's path and not what the system's source produces.) */
		met.push_back(response.on_level(PressureLevel::critical));
		met.push_back(response.on_level(PressureLevel::warn));
		during = reportsOf(actions);
	};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(met == (std::vector<Outcome>{Outcome::busy, Outcome::nothing}));
	require(sameReports(during, {
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::critical, PressureReaction::underway},
		{PressureLevel::warn, PressureReaction::none},
	}));
	/* The warning that came last ends the trimmer when the response returns. */
	require((actions.calls_taken() ==
		std::vector<std::string>{"gpu", "trim-on", "trim-off"}));
}

void aRetryReportsNothing()
{
	FakeActions actions;
	actions.gradual_outcome = TrimOutcome::busy;
	PressureTiming timing;
	timing.retry_attempts = 2;
	MemoryPressureResponse response{actions, timing};
	require(response.on_level(PressureLevel::critical) == Outcome::responded);
	require(response.on_retry() == Outcome::retried);
	require(response.on_retry() == Outcome::retried);
	require(actions.requests() == 3);
	require(sameReports(reportsOf(actions), {
		{PressureLevel::critical, PressureReaction::responding},
	}), "a retry is the same pressure, heard once");
}

void theNamesOfTheReactionsAreTheLinesWords()
{
	using uml::runtime::pressure_reaction_name;
	require(pressure_reaction_name(PressureReaction::none) == "none");
	require(pressure_reaction_name(PressureReaction::responding) == "responding");
	require(pressure_reaction_name(PressureReaction::underway) == "underway");
}

/* ---- The system's side, against a source the test raises. ---- */

/* Stderr, captured through a pipe for the case that follows. */
class StderrCapture final {
public:
	StderrCapture()
	{
		int descriptors[2];
		require(::pipe(descriptors) == 0, "pipe");
		read_ = descriptors[0];
		write_ = descriptors[1];
		(void)::fcntl(read_, F_SETFL, O_NONBLOCK);
		saved_ = ::dup(STDERR_FILENO);
		require(saved_ >= 0, "dup");
		require(::dup2(write_, STDERR_FILENO) >= 0, "dup2");
	}

	~StderrCapture()
	{
		restore();
		::close(read_);
		::close(write_);
	}

	StderrCapture(const StderrCapture &) = delete;
	StderrCapture &operator=(const StderrCapture &) = delete;

	void restore()
	{
		if (saved_ >= 0) {
			::dup2(saved_, STDERR_FILENO);
			::close(saved_);
			saved_ = -1;
		}
	}

	/* What has been written since the last call. */
	[[nodiscard]] std::string take()
	{
		char buffer[512];
		for (;;) {
			const auto got = ::read(read_, buffer, sizeof buffer);
			if (got <= 0)
				break;
			text_.append(buffer, static_cast<std::size_t>(got));
		}
		return text_;
	}

private:
	int read_{-1};
	int write_{-1};
	int saved_{-1};
	std::string text_;
};

std::atomic<int> gpu_reclaims{0};
std::atomic<int> guest_trims{0};
std::atomic<int> gradual_ons{0};
std::atomic<int> gradual_offs{0};

std::uint64_t countedGpuReclaim() noexcept
{
	gpu_reclaims.fetch_add(1);
	return 5;
}

uml::runtime::TrimOutcome countedGuestTrim(void *context,
	TrimKind kind) noexcept
{
	switch (kind) {
	case TrimKind::once:
		guest_trims.fetch_add(1);
		*static_cast<int *>(context) += 1;
		break;
	case TrimKind::gradual_on:
		gradual_ons.fetch_add(1);
		break;
	case TrimKind::gradual_off:
		gradual_offs.fetch_add(1);
		break;
	}
	return TrimOutcome::done;
}

void theSystemSourceReachesTheGpuAndTheGuest()
{
	gpu_reclaims = 0;
	guest_trims = 0;
	gradual_ons = 0;
	gradual_offs = 0;
	uml::platform::publishGpuCacheReclaimer(&countedGpuReclaim);
	int context = 0;
	StderrCapture capture;
	{
		const auto watch =
			uml::runtime::start_memory_pressure_response(&countedGuestTrim,
				&context);
		require(watch != nullptr);

		/* Normal: nothing. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL);
		require(capture.take().empty(), capture.take());
		require(gpu_reclaims == 0 && guest_trims == 0 && gradual_ons == 0 &&
			gradual_offs == 0);

		/* A warning takes nothing of the guest or the GPU, and says so on
		 * stderr with its level line alone. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		require(gpu_reclaims == 0 && guest_trims == 0 && context == 0 &&
			gradual_ons == 0 && gradual_offs == 0,
			"a warning took the GPU's caches or asked the guest for anything");
		const std::string text = capture.take();
		require(contains(text, "memory pressure warn (was normal): footprint "),
			text);
		/* The figures are the process's own. */
		const auto footprint = text.find("footprint ");
		require(footprint != std::string::npos &&
			std::stoull(text.substr(footprint + 10)) > 0, text);
		require(!contains(text, " -> ") && !contains(text, "trimmer"), text);
		require(contains(text, "guest RAM wire failures"), text);

		/* And so does a second one. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		require(gpu_reclaims == 0 && guest_trims == 0 && gradual_ons == 0 &&
			gradual_offs == 0, "a second warning acted");

		/* A critical level always acts: the GPU's caches and the gradual
		 * trimmer, and never a trim of all the cache. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(gpu_reclaims == 1, "the critical level freed no GPU caches");
		require(guest_trims == 0 && context == 0,
			"the critical level trimmed all the cache");
		require(gradual_ons == 1);
		const std::string critical = capture.take();
		require(contains(critical, "(5 cached allocations)"), critical);
		require(contains(critical, "guest page cache trimmer on (done)"), critical);
		require(!contains(critical, "page cache trim "), critical);

		/* A warning after it is the pressure easing: the gradual trimmer is
		 * ended once, and the GPU is left alone. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		require(gradual_offs == 1 && gpu_reclaims == 1);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL);
		require(gradual_offs == 1, "the way back to normal asked again");
	}
	/* The watch has ended; the source stays with the process and answers
	 * nothing. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	require(gpu_reclaims == 1 && guest_trims == 0 && gradual_ons == 1 &&
		gradual_offs == 1, "a source with no watch acted");
	capture.restore();
	uml::platform::publishGpuCacheReclaimer(nullptr);
}

void aWatchWithNoGpuAndNoGuestStillAnswers()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	StderrCapture capture;
	const auto watch =
		uml::runtime::start_memory_pressure_response(nullptr, nullptr);
	require(watch != nullptr);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	const std::string text = capture.take();
	require(contains(text, "trimmer on (no console)"), text);
	require(contains(text, "(0 cached allocations)"), text);
}

void aWatchEndsWhileItsResponseIsRunning()
{
	std::mutex gate;
	std::condition_variable changed;
	bool inside = false;
	struct Hold final {
		std::mutex *gate;
		std::condition_variable *changed;
		bool *inside;
	} hold{&gate, &changed, &inside};
	StderrCapture capture;
	auto watch = uml::runtime::start_memory_pressure_response(
		[](void *context, TrimKind kind) noexcept {
			if (kind != TrimKind::gradual_on)
				return TrimOutcome::done;
			auto &hold = *static_cast<Hold *>(context);
			{
				const std::lock_guard lock{*hold.gate};
				*hold.inside = true;
			}
			hold.changed->notify_all();
			std::this_thread::sleep_for(std::chrono::milliseconds{300});
			return TrimOutcome::done;
		}, &hold);
	require(watch != nullptr);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	{
		std::unique_lock lock{gate};
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return inside; }), "the trim never began");
	}
	/* The watch goes with a trim still running: the destructor returns once
	 * it has ended, and what the trim reads (the hold) is alive for it. */
	const auto begun = std::chrono::steady_clock::now();
	watch.reset();
	require(std::chrono::steady_clock::now() - begun >=
		std::chrono::milliseconds{100}, "the watch did not wait");
	capture.restore();
	require(contains(capture.take(), "trimmer on (done)"));
}

/* What the request for the gradual trimmer counts when it finds the guest busy
 * a few times; the request to turn it off is always taken and not counted. */
struct BusyThenFree final {
	std::atomic<int> calls{0};
	int busy_calls;
};

TrimOutcome busyThenFree(void *context, TrimKind kind) noexcept
{
	if (kind != TrimKind::gradual_on)
		return TrimOutcome::done;
	auto &trim = *static_cast<BusyThenFree *>(context);
	return trim.calls.fetch_add(1) < trim.busy_calls ? TrimOutcome::busy
							: TrimOutcome::done;
}

void aBusyTrimmerRequestIsTriedAgainByTheWatch()
{
	BusyThenFree trim;
	trim.busy_calls = 2;
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 20 * millisecond;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&busyThenFree, &trim, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		/* The response's own try and two retries found the guest busy or
		 * free; nothing comes after the one that was done. */
		require(waitUntil([&] { return trim.calls == 3; },
			std::chrono::seconds{20}), "the busy request was not retried");
		require(waitUntil([&] {
			return contains(capture.take(), "(retry 2 of 4)");
		}, std::chrono::seconds{20}), capture.take());
		require(!waitUntil([&] { return trim.calls > 3; },
			std::chrono::milliseconds{300}), "a request that was done came again");
	}
	capture.restore();
	const std::string text = capture.take();
	require(contains(text, "trimmer on (busy)"), text);
	require(contains(text, "(retry 2 of 4)") &&
		contains(text, "trimmer on (done)"), text);
	/* The first retry found it busy still and wrote nothing. */
	require(!contains(text, "(retry 1 of 4)"), text);
}

/* The guest is busy at the first try, and until the retry the process cannot
 * allocate. */
TrimOutcome busyWhileAllocationFails(void *context, TrimKind kind) noexcept
{
	if (kind != TrimKind::gradual_on)
		return TrimOutcome::done;
	auto &calls = *static_cast<std::atomic<int> *>(context);
	if (calls.fetch_add(1) == 0) {
		nothrow_allocation_fails.store(true);
		return TrimOutcome::busy;
	}
	nothrow_allocation_fails.store(false);
	return TrimOutcome::done;
}

void aRetryNeedsNoAllocationToBeArmed()
{
	std::atomic<int> calls{0};
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 20 * millisecond;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&busyWhileAllocationFails, &calls, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		const bool retried = waitUntil([&] { return calls == 2; },
			std::chrono::seconds{20});
		nothrow_allocation_fails.store(false);
		require(retried, "a retry was lost for want of memory to arm it");
	}
	capture.restore();
	require(contains(capture.take(), "(retry 1 of 4)"));
}

void aPendingRetryEndsWithTheWatch()
{
	BusyThenFree trim;
	trim.busy_calls = 1000;
	BusyThenFree later;
	later.busy_calls = 0;
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 150 * millisecond;
	{
		auto watch = uml::runtime::start_memory_pressure_response(
			&busyThenFree, &trim, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(trim.calls == 1);
		/* The watch ends with a retry pending, and another watch of the same
		 * process begins before the timer would have fired. */
		watch.reset();
		const auto next = uml::runtime::start_memory_pressure_response(
			&busyThenFree, &later, timing);
		require(next != nullptr);
		std::this_thread::sleep_for(std::chrono::milliseconds{500});
		require(trim.calls == 1, "a retry came after its watch ended");
		require(later.calls == 0, "a retry of an earlier watch reached the next");
	}
	/* The process goes on, and a watch with no retry pending sees none. */
	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	require(trim.calls == 1 && later.calls == 0);
}

/* What the guest trim a watch makes was asked, by kind. */
struct KindCounts final {
	std::atomic<int> on{0};
	std::atomic<int> off{0};
	std::atomic<int> once{0};
	/* `busy_on` requests for the trimmer on find the guest busy, from the
	 * `skip_on`th. */
	int skip_on = 0;
	int busy_on = 0;
};

TrimOutcome countKinds(void *context, TrimKind kind) noexcept
{
	auto &counts = *static_cast<KindCounts *>(context);
	switch (kind) {
	case TrimKind::gradual_on: {
		const int index = counts.on.fetch_add(1);
		return index >= counts.skip_on && index < counts.skip_on + counts.busy_on ?
			TrimOutcome::busy : TrimOutcome::done;
	}
	case TrimKind::gradual_off:
		counts.off.fetch_add(1);
		return TrimOutcome::done;
	case TrimKind::once:
		counts.once.fetch_add(1);
		break;
	}
	return TrimOutcome::done;
}

/* The system's source delivers a warning and then nothing: the watch's own
 * timer ends the level, as a delivered normal level would. */
void aWatchReturnsToNormalWhenTheSourceStaysQuiet()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	KindCounts counts;
	StderrCapture capture;
	PressureTiming timing;
	timing.quiet_period_ns = 100 * millisecond;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countKinds, &counts, timing);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(waitUntil([&] { return counts.off.load() == 1; },
			std::chrono::seconds{20}), "the level never returned to normal");
		require(counts.on.load() == 1 && counts.once.load() == 0);
		require(!waitUntil([&] { return counts.off.load() > 1; },
			std::chrono::milliseconds{300}),
			"the return to normal was made twice");
	}
	capture.restore();
	const std::string text = capture.take();
	require(contains(text, "normal (was critical)") &&
		contains(text, "no level for"), text);
}

/* Levels that alternate are followed as they come: each critical level asks
 * the trimmer on and each warning off, and the watch's timer has nothing to
 * ask. */
void aWatchFollowsLevelsThatAlternate()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	KindCounts counts;
	StderrCapture capture;
	/* A retry interval far shorter than the wait below, so that a retry the
	 * watch wrongly kept pending would come due inside it. */
	PressureTiming timing;
	timing.retry_interval_ns = 20 * millisecond;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countKinds, &counts, timing);
		require(watch != nullptr);
		for (int cycle = 0; cycle < 3; ++cycle) {
			raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
			raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		}
		require(counts.on.load() == 3 && counts.off.load() == 3,
			"a level was not followed");
		require(!waitUntil([&] { return counts.on.load() > 3 ||
				counts.off.load() > 3; },
			std::chrono::milliseconds{300}), "a timer asked again");
		require(counts.once.load() == 0);
	}
	capture.restore();
}

/* A retry that comes due before the end of the quiet period sets the timer
 * itself: it does not wait for the period. */
void aRetryDoesNotWaitForTheQuietPeriod()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	KindCounts counts;
	/* The critical level's request and the first retry find the guest busy. */
	counts.skip_on = 0;
	counts.busy_on = 2;
	StderrCapture capture;
	PressureTiming timing;
	timing.retry_interval_ns = 20 * millisecond;
	timing.quiet_period_ns = 600 * second;
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countKinds, &counts, timing);
		require(watch != nullptr);
		/* The warning sets the timer for the quiet period. */
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		require(waitUntil([&] { return counts.on.load() == 3; },
			std::chrono::seconds{20}),
			"the retry waited for the quiet period's timer");
		require(counts.once.load() == 0);
	}
	capture.restore();
}

/* ---- The observer of the reports, against the system's source. ---- */

/* What an observer heard, in order, and the guest trims counted by the time it
 * heard each. */
struct Heard final {
	std::mutex mutex;
	std::vector<PressureReport> reports;
	std::vector<int> trims_before;
	std::atomic<int> *trims{};
};

void observeInto(void *context, const PressureReport &report) noexcept
{
	auto &heard = *static_cast<Heard *>(context);
	const std::lock_guard lock{heard.mutex};
	heard.reports.push_back(report);
	heard.trims_before.push_back(heard.trims == nullptr ? 0 : heard.trims->load());
}

std::atomic<int> observed_trims{0};

TrimOutcome countObservedTrim(void *, TrimKind) noexcept
{
	observed_trims.fetch_add(1);
	return TrimOutcome::done;
}

void theObserverHearsTheLevelsTheSystemRaises()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	observed_trims = 0;
	Heard heard;
	heard.trims = &observed_trims;
	StderrCapture capture;
	uml::runtime::set_pressure_observer(&observeInto, &heard);
	{
		const auto watch = uml::runtime::start_memory_pressure_response(
			&countObservedTrim, nullptr);
		require(watch != nullptr);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
		raiseAndWait(DISPATCH_MEMORYPRESSURE_NORMAL);
	}
	/* A watch that ended drops what the source delivers, and so reports none. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	const std::lock_guard lock{heard.mutex};
	require(sameReports(heard.reports, {
		{PressureLevel::warn, PressureReaction::none},
		{PressureLevel::warn, PressureReaction::none},
		{PressureLevel::critical, PressureReaction::responding},
		{PressureLevel::normal, PressureReaction::none},
	}));
	/* Each was heard before its own request: the warnings asked for nothing,
	 * the critical level's trimmer on, the normal level's trimmer off. */
	require((heard.trims_before == std::vector<int>{0, 0, 0, 1}),
		"a report followed the request it announced");
	require(observed_trims == 2);
	capture.restore();
}

void aClearedObserverHearsNothingMore()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	Heard first;
	Heard second_observer;
	StderrCapture capture;
	const auto watch = uml::runtime::start_memory_pressure_response(
		nullptr, nullptr);
	require(watch != nullptr);
	uml::runtime::set_pressure_observer(&observeInto, &first);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	uml::runtime::set_pressure_observer(&observeInto, &second_observer);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	capture.restore();
	require(first.reports.size() == 1, "the replaced observer heard on");
	require(second_observer.reports.size() == 1,
		"the cleared observer heard on");
}

/* An observer that records into storage of its own: it allocates nothing. */
struct FixedHeard final {
	std::array<PressureReport, 8> reports{};
	std::atomic<int> count{0};
};

void observeFixed(void *context, const PressureReport &report) noexcept
{
	auto &heard = *static_cast<FixedHeard *>(context);
	const int index = heard.count.load();
	if (index < static_cast<int>(heard.reports.size()))
		heard.reports[static_cast<std::size_t>(index)] = report;
	heard.count.store(index + 1);
}

/* A report is made when the system is short of memory, so it allocates
 * nothing: the same event is handled with and without an observer, and the
 * ordinary allocations of its whole handling (recording it, and the policy's
 * turn) are the same count (an allocation the report added would show in the difference),
 * and an event still reaches the observer while the nothrow allocations fail. */
void aReportNeedsNoAllocation()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	StderrCapture capture;
	const auto watch = uml::runtime::start_memory_pressure_response(
		nullptr, nullptr);
	require(watch != nullptr);
	require(pressure_handler.load() != nullptr, "the handler was not counted");
	/* The first events make what the handling makes once. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	event_allocations.store(0);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	const std::size_t without_observer = event_allocations.load();

	FixedHeard heard;
	uml::runtime::set_pressure_observer(&observeFixed, &heard);
	event_allocations.store(0);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	const std::size_t with_observer = event_allocations.load();
	nothrow_allocation_fails.store(true);
	raiseAndWait(DISPATCH_MEMORYPRESSURE_CRITICAL);
	nothrow_allocation_fails.store(false);
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();
	require(heard.count.load() == 2, "a report was lost for want of memory");
	require(heard.reports[0].level == PressureLevel::critical);
	require(with_observer == without_observer,
		"the report allocated: " + std::to_string(with_observer) + " ordinary allocations with the observer, "
			+ std::to_string(without_observer) + " without");
}

/* The system raises levels while a policy call is held (a trim waits for the
 * guest). The source's handler only records each delivery, so it returns at
 * once and each level is delivered on its own; the policy handles them one at
 * a time in the order they came, each reported before its own work, and ends
 * in the state of the last. */

/* A guest trim that records the requests it gets, in order, and holds the
 * first one until the test lets it go. */
struct HeldPolicy final {
	std::mutex mutex;
	std::condition_variable changed;
	bool inside = false;
	bool release = false;
	std::vector<TrimKind> kinds;
	bool held_once = false;

	[[nodiscard]] std::vector<TrimKind> taken()
	{
		const std::lock_guard lock{mutex};
		return kinds;
	}

	void let_go()
	{
		{
			const std::lock_guard lock{mutex};
			release = true;
		}
		changed.notify_all();
	}

	void wait_inside()
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return inside; }), "the policy call never began");
	}
};

TrimOutcome heldTrim(void *context, TrimKind kind) noexcept
{
	auto &held = *static_cast<HeldPolicy *>(context);
	std::unique_lock lock{held.mutex};
	held.kinds.push_back(kind);
	if (!std::exchange(held.held_once, true)) {
		held.inside = true;
		held.changed.notify_all();
		(void)held.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return held.release; });
	}
	return TrimOutcome::done;
}

[[nodiscard]] std::vector<PressureReport> heardReports(Heard &heard)
{
	const std::lock_guard lock{heard.mutex};
	return heard.reports;
}

void deliveriesAfterAHeldPolicyCallAreHandledInTheOrderTheyCame(
	std::uint64_t first, std::uint64_t then, const char *name)
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	HeldPolicy held;
	Heard heard;
	StderrCapture capture;
	uml::runtime::set_pressure_observer(&observeInto, &heard);
	const auto watch = uml::runtime::start_memory_pressure_response(
		&heldTrim, &held);
	require(watch != nullptr);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	held.wait_inside();

	/* The policy is held. Each of the next two is delivered on its own, and
	 * the source's handler does not wait for the held call. */
	const auto started = std::chrono::steady_clock::now();
	for (const std::uint64_t flags : {first, then}) {
		dispatch_source_merge_data(raisable(), flags);
		dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	}
	require(std::chrono::steady_clock::now() - started <
		std::chrono::seconds{2}, std::string{name} +
		": the source's handler waited for the policy");
	/* Only the critical level that was being handled has been reported. */
	require(heardReports(heard).size() == 1, name);

	held.let_go();
	require(waitUntil([&] { return heardReports(heard).size() == 3; },
		std::chrono::seconds{20}), std::string{name} +
		": the deliveries were not all handled");
	settle();
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();

	/* One report for each delivery, in the order they came. */
	const auto reports = heardReports(heard);
	require(reports.size() == 3, name);
	const auto level = [](std::uint64_t flags) {
		return flags == DISPATCH_MEMORYPRESSURE_CRITICAL ?
			PressureLevel::critical : PressureLevel::normal;
	};
	require(reports[0].level == PressureLevel::critical &&
		reports[0].reaction == PressureReaction::responding, name);
	require(reports[1].level == level(first), name);
	require(reports[2].level == level(then), name);
	require(reports[1].reaction == (first == DISPATCH_MEMORYPRESSURE_CRITICAL ?
		PressureReaction::responding : PressureReaction::none), name);
	require(reports[2].reaction == (then == DISPATCH_MEMORYPRESSURE_CRITICAL ?
		PressureReaction::responding : PressureReaction::none), name);

	/* And the requests to the guest in that order: the first critical level's
	 * trimmer on; then the critical level's trimmer on and the normal level's
	 * trimmer off, each where it came. The trimmer ends as the last delivery
	 * says. */
	std::vector<TrimKind> expected{TrimKind::gradual_on};
	const auto add = [&](std::uint64_t flags) {
		expected.push_back(flags == DISPATCH_MEMORYPRESSURE_CRITICAL ?
			TrimKind::gradual_on : TrimKind::gradual_off);
	};
	add(first);
	add(then);
	require(held.taken() == expected, std::string{name} +
		": the requests were not in the order the levels came");
}

void aCriticalThenANormalLevelRaisedDuringAHeldPolicyCallEndTheTrimmerOff()
{
	deliveriesAfterAHeldPolicyCallAreHandledInTheOrderTheyCame(
		DISPATCH_MEMORYPRESSURE_CRITICAL, DISPATCH_MEMORYPRESSURE_NORMAL,
		"critical then normal");
}

void aNormalThenACriticalLevelRaisedDuringAHeldPolicyCallEndTheTrimmerOn()
{
	deliveriesAfterAHeldPolicyCallAreHandledInTheOrderTheyCame(
		DISPATCH_MEMORYPRESSURE_NORMAL, DISPATCH_MEMORYPRESSURE_CRITICAL,
		"normal then critical");
}

/* A mask the system itself merged (the events came closer together than the
 * handler takes to record them) is the most serious flag it holds. A suspended
 * source stands for that: what is merged meanwhile arrives as one. */
void aMergedMaskTakesItsMostSeriousFlag()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	HeldPolicy held;
	held.held_once = true;
	Heard heard;
	StderrCapture capture;
	uml::runtime::set_pressure_observer(&observeInto, &heard);
	const auto watch = uml::runtime::start_memory_pressure_response(
		&heldTrim, &held);
	require(watch != nullptr);
	dispatch_suspend(raisable());
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_NORMAL);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	dispatch_resume(raisable());
	settle();
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();
	const auto reports = heardReports(heard);
	require(reports.size() == 1,
		"a merged mask was reported more than once");
	require(reports[0].level == PressureLevel::critical &&
		reports[0].reaction == PressureReaction::responding);
	require((held.taken() == std::vector<TrimKind>{TrimKind::gradual_on}));
}

/* The watch ends while a policy call is held and deliveries wait behind it:
 * the end waits for the call and no more is handled. */
void aWatchEndsWithDeliveriesQueuedBehindAHeldPolicyCall()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	HeldPolicy held;
	Heard heard;
	StderrCapture capture;
	uml::runtime::set_pressure_observer(&observeInto, &heard);
	auto watch = uml::runtime::start_memory_pressure_response(&heldTrim, &held);
	require(watch != nullptr);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	held.wait_inside();
	for (const std::uint64_t flags : {DISPATCH_MEMORYPRESSURE_CRITICAL,
		     DISPATCH_MEMORYPRESSURE_NORMAL,
		     DISPATCH_MEMORYPRESSURE_CRITICAL}) {
		dispatch_source_merge_data(raisable(), flags);
		dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	}
	std::atomic<bool> ended{false};
	std::thread stopping{[&] {
		watch.reset();
		ended.store(true);
	}};
	require(!waitUntil([&] { return ended.load(); },
		std::chrono::milliseconds{300}),
		"the watch ended while its policy call was running");
	held.let_go();
	stopping.join();
	require(ended.load());
	settle();
	/* Nothing after the call that was running: the queued deliveries were
	 * dropped, and no later report or request reached the application. */
	require(held.taken() == std::vector<TrimKind>{TrimKind::gradual_on});
	require(heardReports(heard).size() == 1);
	/* A later watch of the process starts clean: the dropped deliveries do
	 * not reach it. */
	HeldPolicy next;
	next.held_once = true;
	const auto later = uml::runtime::start_memory_pressure_response(
		&heldTrim, &next);
	require(later != nullptr);
	settle();
	require(next.taken().empty(), "a dropped delivery reached the next watch");
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();
}

/* A source callback that was running while the watch ended and another began
 * is a callback of the earlier watch: what it read reaches no one. */
void aCallbackThatMeetsAStopAndAStartHandsItsNotificationToNoOne()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	observed_trims = 0;
	Heard first;
	Heard second;
	StderrCapture capture;
	uml::runtime::set_pressure_observer(&observeInto, &first);
	auto watch = uml::runtime::start_memory_pressure_response(
		&countObservedTrim, nullptr);
	require(watch != nullptr);
	flag_read_hold.arm();
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	require(flag_read_hold.wait_inside(), "the callback did not run");
	/* The callback is held at its read; the watch ends and a new one begins. */
	watch.reset();
	uml::runtime::set_pressure_observer(&observeInto, &second);
	const auto next = uml::runtime::start_memory_pressure_response(
		&countObservedTrim, nullptr);
	require(next != nullptr);
	flag_read_hold.let_go();
	settle();
	require(heardReports(first).empty());
	require(heardReports(second).empty(),
		"the old watch's notification reached the new watch");
	require(observed_trims.load() == 0,
		"the old watch's notification started a response in the new one");
	/* The new watch hears what the system raises now, once. */
	raiseAndWait(DISPATCH_MEMORYPRESSURE_WARN);
	require(sameReports(heardReports(second), {
		{PressureLevel::warn, PressureReaction::none}}));
	uml::runtime::set_pressure_observer(nullptr, nullptr);
	capture.restore();
}

void clearingTheObserverWaitsForACallInProgress()
{
	uml::platform::publishGpuCacheReclaimer(nullptr);
	struct Slow final {
		std::mutex gate;
		std::condition_variable changed;
		bool inside = false;
		bool release = false;
		std::atomic<bool> finished{false};
	} slow;
	StderrCapture capture;
	uml::runtime::set_pressure_observer([](void *context,
		const PressureReport &) noexcept {
		auto &slow = *static_cast<Slow *>(context);
		std::unique_lock lock{slow.gate};
		slow.inside = true;
		slow.changed.notify_all();
		(void)slow.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return slow.release; });
		lock.unlock();
		slow.finished.store(true);
	}, &slow);
	const auto watch = uml::runtime::start_memory_pressure_response(
		nullptr, nullptr);
	require(watch != nullptr);
	dispatch_source_merge_data(raisable(), DISPATCH_MEMORYPRESSURE_CRITICAL);
	{
		std::unique_lock lock{slow.gate};
		require(slow.changed.wait_for(lock, std::chrono::seconds{20},
			[&] { return slow.inside; }), "the observer was never called");
	}
	std::atomic<bool> cleared{false};
	std::thread clearing{[&] {
		uml::runtime::set_pressure_observer(nullptr, nullptr);
		/* The call had ended when this returned, so its context could go. */
		require(slow.finished.load(), "the clear returned inside the call");
		cleared.store(true);
	}};
	require(!waitUntil([&] { return cleared.load(); },
		std::chrono::milliseconds{300}), "the clear did not wait");
	{
		const std::lock_guard lock{slow.gate};
		slow.release = true;
	}
	slow.changed.notify_all();
	clearing.join();
	require(cleared.load());
	/* Let the response behind the call end before the watch goes. */
	dispatch_sync_f(pressure_queue.load(), nullptr, [](void *) {});
	capture.restore();
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"the-levels-of-the-source-are-the-most-serious-flag",
			theLevelsOfTheSourceAreTheMostSeriousFlag},
		{"the-normal-level-takes-nothing", theNormalLevelTakesNothing},
		{"a-warning-asks-the-guest-for-nothing", aWarningAsksTheGuestForNothing},
		{"a-critical-level-frees-the-gpu-caches-and-asks-for-the-gradual-"
			"trimmer-never-a-trim-of-all-the-cache",
			aCriticalLevelFreesTheGpuCachesAndAsksForTheGradualTrimmerNeverATrimOfAllTheCache},
		{"a-critical-level-says-what-became-of-the-trimmer-and-what-the-"
			"gpu-freed",
			aCriticalLevelSaysWhatBecameOfTheTrimmerAndWhatTheGpuFreed},
		{"a-critical-level-always-starts-one", aCriticalLevelAlwaysStartsOne},
		{"a-critical-level-after-a-warning-and-a-warning-after-a-critical-"
			"level",
			aCriticalLevelAfterAWarningAndAWarningAfterACriticalLevel},
		{"a-level-that-eases-from-critical-asks-the-trimmer-off-at-once",
			aLevelThatEasesFromCriticalAsksTheTrimmerOffAtOnce},
		{"levels-that-alternate-are-followed-as-they-come",
			levelsThatAlternateAreFollowedAsTheyCome},
		{"a-warning-that-meets-a-busy-guest-keeps-the-end-pending-until-a-"
			"critical-level-takes-it",
			aWarningThatMeetsABusyGuestKeepsTheEndPendingUntilACriticalLevelTakesIt},
		{"a-response-that-is-running-is-not-joined",
			aResponseThatIsRunningIsNotJoined},
		{"a-level-that-eases-during-response-ends-the-trimmer-when-it-"
			"returns",
			aLevelThatEasesDuringAResponseEndsTheTrimmerWhenItReturns},
		{"an-event-on-another-thread-meets-the-running-trim",
			anEventOnAnotherThreadMeetsTheRunningTrim},
		{"a-busy-trimmer-request-is-tried-again", aBusyTrimmerRequestIsTriedAgain},
		{"a-retry-that-is-still-busy-stops-after-its-attempts",
			aRetryThatIsStillBusyStopsAfterItsAttempts},
		{"a-retry-is-not-for-a-guest-the-host-has-quiesced",
			aRetryIsNotForAGuestTheHostHasQuiesced},
		{"a-later-response-or-an-easing-level-takes-the-pending-request",
			aLaterResponseOrAnEasingLevelTakesThePendingRequest},
		{"an-easing-level-during-response-leaves-no-request-pending",
			anEasingLevelDuringAResponseLeavesNoRequestPending},
		{"a-busy-guest-keeps-the-end-of-the-gradual-trimmer-pending",
			aBusyGuestKeepsTheEndOfTheGradualTrimmerPending},
		{"a-retry-is-not-joined-to-a-response-that-is-running",
			aRetryIsNotJoinedToAResponseThatIsRunning},
		{"a-critical-level-asks-for-the-trimmer-after-an-end-that-was-"
			"never-sent",
			aCriticalLevelAsksForTheTrimmerAfterAnEndThatWasNeverSent},
		{"a-critical-level-takes-pending-end-of-the-trimmer",
			aCriticalLevelTakesAPendingEndOfTheTrimmer},
		{"a-busy-end-of-the-trimmer-is-tried-again-at-most-four-times",
			aBusyEndOfTheTrimmerIsTriedAgainAtMostFourTimes},
		{"a-critical-levels-trimmer-request-is-tried-again-within-its-"
			"allowance",
			aCriticalLevelsTrimmerRequestIsTriedAgainWithinItsAllowance},
		{"an-end-of-the-trimmer-that-could-not-be-sent-stays-owed-and-a-"
			"refused-one-does-not",
			anEndOfTheTrimmerThatCouldNotBeSentStaysOwedAndARefusedOneDoesNot},
		{"a-critical-level-that-meets-the-retried-end-of-the-trimmer-"
			"turns-it-on-again",
			aCriticalLevelThatMeetsTheRetriedEndOfTheTrimmerTurnsItOnAgain},
		{"the-level-returns-to-normal-when-the-source-stays-quiet",
			theLevelReturnsToNormalWhenTheSourceStaysQuiet},
		{"a-critical-level-returns-to-normal-when-the-source-stays-quiet",
			aCriticalLevelReturnsToNormalWhenTheSourceStaysQuiet},
		{"a-level-delivered-inside-the-period-moves-its-end",
			aLevelDeliveredInsideThePeriodMovesItsEnd},
		{"a-delivered-normal-level-ends-the-period",
			aDeliveredNormalLevelEndsThePeriod},
		{"a-held-guest-is-not-trimmed-because-of-a-stale-timer",
			aHeldGuestIsNotTrimmedBecauseOfAStaleTimer},
		{"a-quiet-period-of-zero-never-returns-the-level",
			aQuietPeriodOfZeroNeverReturnsTheLevel},
		{"the-clock-is-read-in-the-order-of-the-mutex",
			theClockIsReadInTheOrderOfTheMutex},
		{"a-storm-of-level-changes-cannot-flood-the-console",
			aStormOfLevelChangesCannotFloodTheConsole},
		{"a-storm-of-critical-levels-cannot-flood-the-console",
			aStormOfCriticalLevelsCannotFloodTheConsole},
		{"every-level-is-reported-the-normal-level-too",
			everyLevelIsReportedTheNormalLevelToo},
		{"a-report-comes-before-the-work-of-the-response",
			aReportComesBeforeTheWorkOfTheResponse},
		{"a-warning-is-reported-as-no-response", aWarningIsReportedAsNoResponse},
		{"a-level-met-while-the-response-runs-is-reported-as-underway",
			aLevelMetWhileTheResponseRunsIsReportedAsUnderway},
		{"a-retry-reports-nothing", aRetryReportsNothing},
		{"the-names-of-the-reactions-are-the-lines-words",
			theNamesOfTheReactionsAreTheLinesWords},
		{"the-system-source-reaches-the-gpu-and-the-guest",
			theSystemSourceReachesTheGpuAndTheGuest},
		{"a-watch-with-no-gpu-and-no-guest-still-answers",
			aWatchWithNoGpuAndNoGuestStillAnswers},
		{"a-watch-ends-while-its-response-is-running",
			aWatchEndsWhileItsResponseIsRunning},
		{"a-busy-trimmer-request-is-tried-again-by-the-watch",
			aBusyTrimmerRequestIsTriedAgainByTheWatch},
		{"a-retry-needs-no-allocation-to-be-armed", aRetryNeedsNoAllocationToBeArmed},
		{"a-pending-retry-ends-with-the-watch", aPendingRetryEndsWithTheWatch},
		{"a-watch-returns-to-normal-when-the-source-stays-quiet",
			aWatchReturnsToNormalWhenTheSourceStaysQuiet},
		{"a-watch-follows-levels-that-alternate", aWatchFollowsLevelsThatAlternate},
		{"a-retry-does-not-wait-for-the-quiet-period",
			aRetryDoesNotWaitForTheQuietPeriod},
		{"the-observer-hears-the-levels-the-system-raises",
			theObserverHearsTheLevelsTheSystemRaises},
		{"a-cleared-observer-hears-nothing-more", aClearedObserverHearsNothingMore},
		{"a-report-needs-no-allocation", aReportNeedsNoAllocation},
		{"a-critical-then-a-normal-level-raised-during-a-held-policy-call-"
			"end-the-trimmer-off",
			aCriticalThenANormalLevelRaisedDuringAHeldPolicyCallEndTheTrimmerOff},
		{"a-normal-then-a-critical-level-raised-during-a-held-policy-call-"
			"end-the-trimmer-on",
			aNormalThenACriticalLevelRaisedDuringAHeldPolicyCallEndTheTrimmerOn},
		{"a-merged-mask-takes-its-most-serious-flag",
			aMergedMaskTakesItsMostSeriousFlag},
		{"a-watch-ends-with-deliveries-queued-behind-a-held-policy-call",
			aWatchEndsWithDeliveriesQueuedBehindAHeldPolicyCall},
		{"a-callback-that-meets-a-stop-and-a-start-hands-its-notification-"
			"to-no-one",
			aCallbackThatMeetsAStopAndAStartHandsItsNotificationToNoOne},
		{"clearing-the-observer-waits-for-a-call-in-progress",
			clearingTheObserverWaitsForACallInProgress},
	};
	return uml::testing::runNamedTests(tests);
}
