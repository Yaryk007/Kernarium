// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

/* Set by the build for the memory layer's audited compilation, which only a
 * diagnostics build of the AOT runtime links (darwin/memory/CMakeLists.txt);
 * the memory layer's test variant has it as well. The compilation every
 * product links never does, and the packaging check refuses a binary that
 * carries the audit. */
#ifndef UML_MEMORY_COUNT_AUDIT
#if defined(UML_MEMORY_ALLOCATION_TESTING) && UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_COUNT_AUDIT 1
#else
#define UML_MEMORY_COUNT_AUDIT 0
#endif
#endif

#if UML_MEMORY_COUNT_AUDIT
/* The executable-mapping counts of the physical-page records, audited in a
 * diagnostics build of a real guest run. Each record counts the guest-code
 * mappings that grant execute and name its page; the audit recounts the
 * executable words of every generation of every software address space and
 * compares them with the records. A production build carries none of this. */
namespace uml::memory {

struct CountAuditResult final {
	/* The audit saw every generation of a domain at one instant: a change
	 * cannot fall between two of them, so `mismatches` is a fact. False when
	 * a generation could not be held without waiting, and then the audit
	 * says nothing. */
	bool exact{};
	/* Records compared (never-share records are not), and the number whose
	 * count differs from the recount. */
	std::uint64_t records{};
	std::uint64_t mismatches{};
	/* What the audit held and what a mismatch looked like: the generations
	 * held, the executable words that named no record, and for the first few
	 * mismatching records the count it had and the recount. */
	std::uint64_t generations{};
	std::uint64_t unnamed{};
	std::uint64_t samples{};
	std::array<std::uint64_t, 4> sample_count{};
	std::array<std::uint64_t, 4> sample_expected{};
};

/* One audit of every software address space of the process. */
[[nodiscard]] CountAuditResult audit_executable_counts() noexcept;

/* Departures that found nothing to count out, and counts that named no
 * record, since the process began: each is a bug in the accounting. */
[[nodiscard]] std::uint64_t count_anomalies() noexcept;
void note_count_anomaly() noexcept;

} // namespace uml::memory
#endif
