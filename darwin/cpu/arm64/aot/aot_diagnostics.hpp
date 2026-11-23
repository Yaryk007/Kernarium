// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#if !UML_AOT_DIAGNOSTICS
#error "aot_diagnostics.hpp is for builds that set UML_AOT_DIAGNOSTICS"
#endif

/* The counters, the test switches and the report of a diagnostics build of
 * the AOT runtime. Production builds do not set UML_AOT_DIAGNOSTICS and carry
 * none of this. The counters are fixed atomics bumped by the vCPU thread; the
 * report is written by a thread of its own to DARWIN_UML_AOT_REPORT
 * (a few times a second), so no vCPU thread does file I/O or allocates. */
namespace uml::cpu::arm64::aot {
class Binder;
} // namespace uml::cpu::arm64::aot

namespace uml::cpu::arm64::aot::diagnostics {

/* The number of library instances a binder (one per executor thread) holds
 * before it replaces the oldest, as the runtime was built: tests that need
 * more instances than that ask it. */
[[nodiscard]] std::size_t instance_capacity() noexcept;

struct Stats final {
	std::uint64_t binds{};
	/* Full verifications (every page) and pages hashed in all. */
	std::uint64_t verifications{};
	std::uint64_t pages_hashed{};
	/* Verifications that found a page unreadable. */
	std::uint64_t unreadable{};
	/* Instances whose pages differ from the ones the code stands for. */
	std::uint64_t poisoned{};
	std::uint64_t enters{};
	std::uint64_t steps{};
	/* Budget used up by entries: instructions only in a counting build
	 * (UML_AOT_COUNT_RETIRED), otherwise the prepaid charge. */
	std::uint64_t retired{};
	std::uint64_t refills{};
	std::uint64_t refill_failures{};
	std::uint64_t cross_hits{};
	std::uint64_t cross_misses{};
	std::array<std::uint64_t, 5> refusals{};
	/* Instructions the largest single entry ran (measured by counting each
	 * one, not estimated from the budget) and nanoseconds spent verifying
	 * instances. */
	std::uint64_t max_retired{};
	std::uint64_t verify_nanos{};
	/* Instructions the engine ran for rewritten code, and the nanoseconds
	 * that took in all (the engine's side of a step, entry to return). */
	std::uint64_t step_nanos{};
	std::uint64_t steps_run{};
	/* How many times the code-change sequence moved between lookups, and
	 * how many times a lookup asked the history what changed. */
	std::uint64_t code_bumps{};
	std::uint64_t history_queries{};
	/* Memory copy and set epilogues the generated thunk left to the host
	 * routine (a size above 64 bytes, a backward copy, a page crossing or
	 * a TLB miss). */
	std::uint64_t mops_slow{};
	std::uint64_t mops_fault{};
	std::uint64_t mops_illegal{};
	/* What binder lookups answered: hit, uncovered, not_ready, poisoned,
	 * unverified, no_entry, stale, mutable_page. */
	std::array<std::uint64_t, 8> finds{};
	/* Pages whose verification the shared table answered: nothing was
	 * fetched or hashed for them, so they are not in `verifications`. */
	std::uint64_t shared_hits{};
	/* Page verifications refused because the page's mapping lacks the
	 * kernel's immutable fact (each retry after a backoff counts again). */
	std::uint64_t mutable_pages{};
	/* Verifications that hashed a page because its frame's epoch moved: the
	 * table held a verdict of the same frame for the same library page
	 * under an older epoch. A frame given back to the page it held, or one
	 * whose code was published, shows here. */
	std::uint64_t epoch_rehashes{};
	/* Queries of a page's stamp that verifying pages made: one to admit the
	 * page, and a second after a hash that matched. */
	std::uint64_t stamp_queries{};
	/* Instances the binders made (a library of an address space at a bias,
	 * in a vCPU's table), how many took the place of a live one (the
	 * table is first in, first out), and how many were made again for a
	 * (vCPU, address space, library, bias) the table had held before: each
	 * of those costs the instance's page states and a verification of every
	 * page the code touches again. */
	std::uint64_t instances_created{};
	std::uint64_t instance_evictions{};
	std::uint64_t instance_rebinds{};
	/* The most page-state bytes the live instances of all binders held at
	 * once, and the size of one Binder object (its tables are mapped apart:
	 * the report's binder_tables_bytes). */
	std::uint64_t instance_page_bytes_peak{};
	std::uint64_t binder_bytes{};
	/* The page-state bytes the live instances of all binders hold now, and
	 * the page slots the live binders have filled. */
	std::uint64_t instance_page_bytes{};
	std::uint64_t page_slots_filled{};
	/* The stack the runtime measured (see the report's stack_ fields): the
	 * least left at a sample site (all ones: none sampled), the size of the
	 * stack that was on, its kind (0 task, 1 host, 2 signal), the task stack
	 * size the runtime assumes, the least left at the start of a run, and
	 * the most a sample site was deeper than the start of its own run. */
	std::uint64_t stack_min_free{};
	std::uint64_t stack_size_at_min{};
	std::uint64_t stack_kind_at_min{};
	std::uint64_t stack_task_bytes{};
	std::uint64_t stack_engine_free{};
	std::uint64_t stack_max_growth{};
	/* The libraries the rewriter left to the engine because their entry
	 * table could not be formed (the blob header's count). */
	std::uint64_t libraries_refused{};
};

/* Why the engine, not rewritten code, ran the block of a dispatch: an
 * instruction the dispatcher would enter was not entered. The first five are
 * the refusals and the retry rule of the executor, the next seven what the
 * binder's lookup answered (FindResult), then an entry that retired nothing. */
enum class EngineReason : unsigned {
	retry,
	exclusive_monitor,
	tbi_disabled,
	sp_misaligned,
	fp_environment,
	short_entry,
	not_ready,
	uncovered,
	poisoned,
	unverified,
	no_entry,
	stale,
	mutable_page,
	no_progress,
	count,
};

/* The dispatch that starts at `pc` ends with the engine running the block
 * there, for `reason`; the guest instructions it runs are counted by
 * note_engine_run, on the page's cover as the binder knows it now (a long
 * block chain may leave the page). The executor calls both when the
 * diagnostics build compiled it with the hooks. */
void note_engine_dispatch(const Binder &binder, unsigned long generation,
	std::uint64_t pc, EngineReason reason) noexcept;
void note_engine_run(std::uint64_t instructions) noexcept;
/* A dispatch ended: how many lookups it made and how many entries it took. */
void note_dispatch(unsigned finds, unsigned enters) noexcept;
/* The reason the last lookup of this thread that found no entry gave. */
[[nodiscard]] EngineReason find_reason() noexcept;

/* Override DARWIN_UML_AOT (nullopt: read the environment again). */
void set_enabled(std::optional<bool> state) noexcept;
/* Make every refill flush the TLB first (so that the next access to any
 * other page or kind misses too) and leave every register that the refill
 * thunk must preserve scrambled when the callback returns. */
void set_refill_stress(bool on) noexcept;
/* Call `hook(context)` whenever a run of the executor reaches AOT, after
 * it read the address space's sequences (nullptr: none). The hook may
 * publish a code change, as another vCPU would at that moment. */
void set_run_hook(void (*hook)(void *), void *context) noexcept;
/* Forget every verdict the address spaces share. */
void clear_shared_verdicts() noexcept;
[[nodiscard]] Stats stats() noexcept;
void reset_stats() noexcept;

} // namespace uml::cpu::arm64::aot::diagnostics
