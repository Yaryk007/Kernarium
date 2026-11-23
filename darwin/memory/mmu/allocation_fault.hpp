// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

#ifndef UML_MEMORY_ALLOCATION_TESTING
#define UML_MEMORY_ALLOCATION_TESTING 0
#endif
#include "../count_audit.hpp"

namespace uml::memory {

/* The executable-mapping accounting found a bug: a departure with nothing to
 * count out, or a count that names no record. Counts are not exact any more,
 * and a verdict kept on their strength may be wrong, so from then on the
 * process shares none: every stamp the memory layer reports says
 * never-share. The latch is one relaxed flag, set in every build and never
 * cleared (but by a test). */
void latch_sharing_off() noexcept;
[[nodiscard]] bool sharing_latched_off() noexcept;

} // namespace uml::memory

#if UML_MEMORY_ALLOCATION_TESTING
namespace uml::memory::testing {

enum class AllocationSite : std::uint8_t {
	none,
	backing,
	batch,
	batch_commit,
	batch_index,
	code_record,
	destroy_cleared,
	generation,
	history,
	immediate_change,
	holder_leaf,
	holder_publication,
	reserve,
	software_domain_registry,
	native_after_mmap,
	native_backing_index,
	native_direct_record,
	native_domain_registry,
	native_mapping,
	native_operation,
	native_owner_record,
	native_owner_registry,
	native_slot,
	writers,
	holders,
	records,
	count,
};

void fail_next_allocation(AllocationSite site) noexcept;
void fail_allocation_at(AllocationSite site, std::size_t occurrence) noexcept;
void clear_allocation_failure() noexcept;
void allocation_checkpoint(AllocationSite site);
void reset_allocation_attempt_counts() noexcept;
[[nodiscard]] std::size_t allocation_attempt_count(
	AllocationSite site) noexcept;

/* Runs at every checkpoint, before any injected failure, on the thread that
 * reached it and under whatever locks that thread holds. It lets a test see
 * the state an operation has at that point, so it must not call back into the
 * memory domain. */
using AllocationObserver = void (*)(AllocationSite site) noexcept;
void set_allocation_observer(AllocationObserver observer) noexcept;

/* The executable-mapping counts of the physical-page records are exact by
 * construction. A departure with nothing to count out, or a count that names
 * no record, is a bug in the accounting; the production code survives it (the
 * count clamps at zero and the epoch moves) and a test must see it. */
void count_anomaly() noexcept;
[[nodiscard]] std::size_t count_anomalies() noexcept;
/* Lets a test that provoked an anomaly on purpose share again. */
void clear_sharing_latch() noexcept;
/* The process-wide count of publications that name no frame
 * (MappedBacking::note_unnamed_publication), which a test sets near its limit
 * and reads back. The limit is the count at which the stamp reports
 * never-share and the count stops. */
void set_unnamed_publication_count(std::uint64_t count) noexcept;
[[nodiscard]] std::uint64_t unnamed_publication_count() noexcept;
[[nodiscard]] std::uint64_t unnamed_publication_limit() noexcept;
/* The frame accounting holds a frame's stripe lock between these two calls on
 * the thread that holds it; a checkpoint reached in between is an allocation
 * under a stripe lock, which is counted. */
void stripe_lock_entered() noexcept;
void stripe_lock_left() noexcept;
/* An allocator the frame accounting's metadata uses, or a test's replacement
 * of the global operator new, reports here; one made under a stripe lock is
 * counted. */
/* A walk that ran while the address space's table of generations was locked,
 * which a test reads (retire_walks_under_lock): the retire walk takes the
 * state's mapping lock and the frames' stripe locks, so it must not nest in
 * the table's. */
void retire_walk_under_lock() noexcept;
[[nodiscard]] std::size_t retire_walks_under_lock() noexcept;
bool allocator_probe() noexcept;
/* While set, every allocation the calling thread makes through a probed
 * allocator or a checkpoint is refused and counted: a test shows that a path
 * needs no memory by running it so. */
void refuse_allocations(bool refuse) noexcept;
[[nodiscard]] std::size_t refused_allocations() noexcept;
/* The bytes the frame accounting's metadata holds now (frame_heap.hpp). */
void frame_bytes_changed(std::ptrdiff_t delta) noexcept;
[[nodiscard]] std::size_t frame_metadata_bytes() noexcept;
[[nodiscard]] std::size_t allocations_under_stripe_lock() noexcept;

void backing_mapping_created() noexcept;
void backing_mapping_destroyed() noexcept;
[[nodiscard]] std::size_t active_backing_mappings() noexcept;

} // namespace uml::memory::testing

#define UML_MEMORY_ALLOCATION_CHECKPOINT(site) \
	::uml::memory::testing::allocation_checkpoint(site)
#define UML_MEMORY_COUNT_ANOMALY() \
	do { \
		::uml::memory::testing::count_anomaly(); \
		::uml::memory::latch_sharing_off(); \
	} while (0)
#define UML_MEMORY_ALLOCATOR_PROBE() \
	do { \
		if (::uml::memory::testing::allocator_probe()) \
			throw std::bad_alloc{}; \
	} while (false)
#define UML_MEMORY_FRAME_BYTES(delta) \
	::uml::memory::testing::frame_bytes_changed(delta)
#define UML_MEMORY_RETIRE_WALK_UNLOCKED(mutex) \
	do { \
		if ((mutex).try_lock_shared()) \
			(mutex).unlock_shared(); \
		else \
			::uml::memory::testing::retire_walk_under_lock(); \
	} while (false)
#define UML_MEMORY_STRIPE_LOCK_ENTERED() \
	::uml::memory::testing::stripe_lock_entered()
#define UML_MEMORY_STRIPE_LOCK_LEFT() \
	::uml::memory::testing::stripe_lock_left()
#define UML_MEMORY_BACKING_MAPPING_CREATED() \
	::uml::memory::testing::backing_mapping_created()
#define UML_MEMORY_BACKING_MAPPING_DESTROYED() \
	::uml::memory::testing::backing_mapping_destroyed()
#else
/* Production expands these probes away before semantic analysis. The
 * allocation-site enum and test functions therefore create no ordinary
 * product symbol, load, branch, or call even in an unoptimized build. The
 * one that sees an accounting bug latches sharing off in every build, and the
 * diagnostics build of a guest run counts it as well (count_audit.hpp reports
 * it). */
#define UML_MEMORY_ALLOCATION_CHECKPOINT(...) ((void)0)
#if UML_MEMORY_COUNT_AUDIT
#define UML_MEMORY_COUNT_ANOMALY() \
	do { \
		::uml::memory::note_count_anomaly(); \
		::uml::memory::latch_sharing_off(); \
	} while (0)
#else
#define UML_MEMORY_COUNT_ANOMALY() ::uml::memory::latch_sharing_off()
#endif
#define UML_MEMORY_ALLOCATOR_PROBE() ((void)0)
#define UML_MEMORY_FRAME_BYTES(...) ((void)0)
#define UML_MEMORY_RETIRE_WALK_UNLOCKED(mutex) ((void)0)
#define UML_MEMORY_STRIPE_LOCK_ENTERED() ((void)0)
#define UML_MEMORY_STRIPE_LOCK_LEFT() ((void)0)
#define UML_MEMORY_BACKING_MAPPING_CREATED() ((void)0)
#define UML_MEMORY_BACKING_MAPPING_DESTROYED() ((void)0)
#endif
