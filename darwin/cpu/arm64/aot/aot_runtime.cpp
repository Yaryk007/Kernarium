// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/aot/aot_runtime.hpp"

#include "cpu/arm64/aot/shared_verdicts.hpp"
#include "cpu/arm64/detail/memory_copy_set.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <arm_acle.h>

#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <sys/mman.h>

#if UML_AOT_DIAGNOSTICS
#include "cpu/arm64/aot/aot_diagnostics.hpp"
#include "memory/count_audit.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
#endif

/* The framework's blob (aot_blob.S). */
extern "C" const std::uint8_t uml_aot_blob[];

namespace uml::cpu::arm64::aot {
namespace {

constexpr std::uint64_t page_size = 4096;
/* How many library instances (a library of an address space at a load bias)
 * one executor thread's binder holds before it replaces the oldest: 2048
 * unless UML_AOT_INSTANCE_CAPACITY in CMake says otherwise. */
#ifndef UML_AOT_INSTANCE_CAPACITY
#define UML_AOT_INSTANCE_CAPACITY 2048
#endif
constexpr std::size_t max_instances = UML_AOT_INSTANCE_CAPACITY;
static_assert(max_instances >= 1 && max_instances <= 16384);
/* Instances are found by hash chains over this many buckets. */
constexpr std::size_t index_buckets = 2 * max_instances;
/* The page states cached by (page, address space): 8 per instance, at least
 * 2048. */
constexpr std::size_t page_slots =
	max_instances * 8 > 2048 ? max_instances * 8 : 2048;
/* The host's page: each table of a binder starts on one. */
constexpr std::size_t host_page = 16384;
/* The verdicts of every address space of the process (shared_verdicts.hpp).
 * A verdict names a physical-page record that is never reused, so an entry
 * can outlive the address space that filed it and never be answered wrongly. */
VerdictTable g_verdicts;
constexpr char blob_magic[8] = {'U', 'M', 'L', 'A', 'O', 'T', '1', '6'};
/* The magic names the layout, and the word after it the version of that
 * layout: a change an older runtime would misread changes the magic, and an
 * addition it can ignore raises the version, which a runtime accepts at or
 * above the one it was built for (tools/aot/rewrite.py: BLOB_VERSION). */
constexpr std::size_t header_version = 8;
constexpr std::uint32_t blob_version = 3;
/* The stored hash of a text page: SHA-256 (tools/aot/rewrite.py:
 * page_hashes). */
constexpr std::size_t page_hash_size = CC_SHA256_DIGEST_LENGTH;

/* The generator's layout (tools/aot/rewrite.py: build_blob). */
constexpr std::size_t header_library_count = 16;
constexpr std::size_t header_records = 20;
constexpr std::size_t header_enter = 24;
constexpr std::size_t header_hash = 28;
constexpr std::size_t header_slots = 32;
/* How many libraries the rewriter left to the engine: their entry table
 * could not be formed (version 2). */
constexpr std::size_t header_refused = 36;
/* The release the blob was made from (version 3): its ID, then the SHA-256 of
 * its manifest and of its system partition. */
constexpr std::size_t header_release_id = 0x30;
constexpr std::size_t release_id_bytes = 96;
constexpr std::size_t header_manifest_sha256 = 0x90;
constexpr std::size_t header_system_sha256 = 0xb0;
constexpr std::size_t record_size = 256;
constexpr std::size_t record_table_base = 152;
constexpr std::size_t record_table_end = 160;
constexpr std::size_t record_table_offset = 168;
constexpr std::size_t record_table_words = 172;
constexpr std::size_t record_pages = 180;
constexpr std::size_t record_unit_offset = 200;
constexpr std::size_t record_page_hashes = 204;

template<typename T>
[[nodiscard]] T read(std::size_t offset) noexcept
{
	T value;
	std::memcpy(&value, uml_aot_blob + offset, sizeof(T));
	return value;
}

struct Library final {
	std::uint64_t table_base;
	std::uint64_t table_end;
	std::size_t table_offset;
	std::size_t table_words;
	std::size_t pages;
	std::size_t unit_offset;
	/* The SHA-256 of each text page the library was built from. */
	std::size_t page_hashes;
};

/* The unit offset of the native code of the source word `word` of a
 * library, or 0 for a word without. The table holds 16 bits per word and,
 * from the next 4 KiB, one signed 32-bit anchor per page, relative to that
 * array (tools/aot/rewrite.py: Library.fill_entry16). */
[[nodiscard]] std::uint32_t native_entry(const Library &library,
	std::uint64_t word) noexcept
{
	const auto entry =
		read<std::uint16_t>(library.table_offset + 2 * word);
	if (entry == 0)
		return 0;
	const std::size_t anchors = library.table_offset +
		((2 * library.table_words + page_size - 1) & ~(page_size - 1));
	const auto relative = read<std::int32_t>(anchors + 4 * (word / 1024));
	return static_cast<std::uint32_t>(
		static_cast<std::int64_t>(anchors) + relative + 4 * entry -
		static_cast<std::int64_t>(library.unit_offset));
}

struct Directory final {
	bool valid{};
	std::uint32_t libraries{};
	std::size_t records{};
	std::size_t enter{};
	std::size_t hash{};
	std::uint32_t slots{};
	std::uint32_t refused{};

	[[nodiscard]] Library library(std::uint32_t index) const noexcept
	{
		const auto base = records + record_size * index;
		return {read<std::uint64_t>(base + record_table_base),
			read<std::uint64_t>(base + record_table_end),
			read<std::uint32_t>(base + record_table_offset),
			read<std::uint32_t>(base + record_table_words),
			read<std::uint32_t>(base + record_pages),
			read<std::uint32_t>(base + record_unit_offset),
			read<std::uint32_t>(base + record_page_hashes)};
	}
};

[[nodiscard]] const Directory &directory() noexcept
{
	static const Directory value = []() noexcept {
		Directory d;
		if (std::memcmp(uml_aot_blob, blob_magic, sizeof(blob_magic)) != 0)
			return d;
		if (read<std::uint32_t>(header_version) < blob_version)
			return d;
		d.libraries = read<std::uint32_t>(header_library_count);
		d.records = read<std::uint32_t>(header_records);
		d.enter = read<std::uint32_t>(header_enter);
		d.hash = read<std::uint32_t>(header_hash);
		d.slots = read<std::uint32_t>(header_slots);
		d.refused = read<std::uint32_t>(header_refused);
		d.valid = d.libraries != 0 && d.slots != 0 &&
			(d.slots & (d.slots - 1)) == 0;
		return d;
	}();
	return value;
}

/* The release the blob names, which the layout ties to the system a launch
 * must boot (tools/aot/rewrite.py: build_blob). */
[[nodiscard]] ::uml::cpu::AotRelease header_release() noexcept
{
	::uml::cpu::AotRelease release;
	const auto *id = reinterpret_cast<const char *>(
		uml_aot_blob + header_release_id);
	release.id = std::string_view{id, strnlen(id, release_id_bytes)};
	std::memcpy(release.manifest_sha256.data(),
		uml_aot_blob + header_manifest_sha256,
		release.manifest_sha256.size());
	std::memcpy(release.system_sha256.data(),
		uml_aot_blob + header_system_sha256, release.system_sha256.size());
	return release;
}

/* Whether the blob was made from the release a launch boots: the ID and both
 * digests are the ones its header names. */
[[nodiscard]] bool made_from(const ::uml::cpu::AotRelease &booted) noexcept
{
	const auto own = header_release();
	return !own.id.empty() && own.id == booted.id &&
		own.manifest_sha256 == booted.manifest_sha256 &&
		own.system_sha256 == booted.system_sha256;
}

/* The identity of one 4 KiB page (tools/aot/rewrite.py: page_digest). */
[[nodiscard]] std::uint64_t page_digest(const std::byte *page) noexcept
{
	std::uint32_t low = 0;
	std::uint32_t high = 0x9e3779b9U;
	for (std::size_t offset = 0; offset < page_size; offset += 8) {
		std::uint64_t word;
		std::memcpy(&word, page + offset, sizeof(word));
		low = __crc32cd(low, word);
		high = __crc32cd(high, word);
	}
	const auto digest = (std::uint64_t{high} << 32) | low;
	return digest != 0 ? digest : 1;
}


std::atomic<int> g_state{-1};

/* What Binder::find answered: an entry, or why it found none. */
enum class FindResult : unsigned {
	hit,
	/* The page is not a page of a covered library (any other code, or
	 * data). */
	uncovered,
	/* The page could not be read. */
	not_ready,
	/* The library instance holds bytes other than the framework's. */
	poisoned,
	/* The instance is not verified (a page unreadable, a retry pending). */
	unverified,
	/* A verified instance, but not an instruction the framework has an
	 * entry for. */
	no_entry,
	/* The instance the page was bound to was replaced. */
	stale,
	/* The page's mapping does not carry the kernel's immutable fact: no
	 * entry is given for it. */
	mutable_page,
};

#if UML_AOT_DIAGNOSTICS
std::atomic<bool> g_refill_stress{false};
std::atomic<void (*)(void *)> g_run_hook{nullptr};
std::atomic<void *> g_run_hook_context{nullptr};
std::atomic<std::uint64_t> g_learning_epoch{0};

/* Leave every general and vector register the caller could rely on holding
 * something else: the refill thunk saves x0-x17, x30, NZCV and q0-q31 around
 * the call and must hand them back as they were. Callee-saved registers are
 * the compiler's to restore (the upper halves of v8-v15 are not). */
[[gnu::noinline]] void scramble_registers() noexcept
{
	asm volatile(
		"movz x0, #0xdead\n movk x0, #0xbeef, lsl #16\n"
		"mov x1, x0\n mov x2, x0\n mov x3, x0\n mov x4, x0\n"
		"mov x5, x0\n mov x6, x0\n mov x7, x0\n mov x8, x0\n"
		"mov x9, x0\n mov x10, x0\n mov x11, x0\n mov x12, x0\n"
		"mov x13, x0\n mov x14, x0\n mov x15, x0\n mov x16, x0\n"
		"mov x17, x0\n"
		"movi v0.16b, #0xa5\n movi v1.16b, #0xa5\n movi v2.16b, #0xa5\n"
		"movi v3.16b, #0xa5\n movi v4.16b, #0xa5\n movi v5.16b, #0xa5\n"
		"movi v6.16b, #0xa5\n movi v7.16b, #0xa5\n movi v8.16b, #0xa5\n"
		"movi v9.16b, #0xa5\n movi v10.16b, #0xa5\n movi v11.16b, #0xa5\n"
		"movi v12.16b, #0xa5\n movi v13.16b, #0xa5\n movi v14.16b, #0xa5\n"
		"movi v15.16b, #0xa5\n movi v16.16b, #0xa5\n movi v17.16b, #0xa5\n"
		"movi v18.16b, #0xa5\n movi v19.16b, #0xa5\n movi v20.16b, #0xa5\n"
		"movi v21.16b, #0xa5\n movi v22.16b, #0xa5\n movi v23.16b, #0xa5\n"
		"movi v24.16b, #0xa5\n movi v25.16b, #0xa5\n movi v26.16b, #0xa5\n"
		"movi v27.16b, #0xa5\n movi v28.16b, #0xa5\n movi v29.16b, #0xa5\n"
		"movi v30.16b, #0xa5\n movi v31.16b, #0xa5\n"
		"msr nzcv, x0\n"
		:
		:
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		  "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "v0",
		  "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",
		  "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19",
		  "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28",
		  "v29", "v30", "v31", "cc");
}

/* The binders alive (one per executor thread), for the report's measure of
 * their tables. Never destroyed, like the report's own lock. */
[[nodiscard]] std::mutex &binders_lock() noexcept
{
	static std::mutex &lock = *new std::mutex;
	return lock;
}

[[nodiscard]] std::vector<const Binder *> &binders() noexcept
{
	static std::vector<const Binder *> &list = *new std::vector<const Binder *>;
	return list;
}

[[nodiscard]] const char *find_result_name(std::size_t kind) noexcept
{
	constexpr const char *names[] = {"hit", "uncovered", "not_ready",
		"poisoned", "unverified", "no_entry", "stale", "mutable_page"};
	return names[kind];
}

namespace counters {
std::atomic<std::uint64_t> binds{0}, verifications{0}, pages_hashed{0},
	unreadable{0}, poisoned{0}, enters{0}, steps{0}, retired{0}, refills{0},
	refill_failures{0}, cross_hits{0}, cross_misses{0}, max_retired{0},
	verify_nanos{0}, step_nanos{0}, steps_run{0}, code_bumps{0},
	history_queries{0}, mops_slow{0}, mops_fault{0}, mops_illegal{0},
	shared_hits{0}, mutable_pages{0}, epoch_rehashes{0}, stamp_queries{0},
	instances_created{0}, instance_evictions{0}, instance_rebinds{0},
	instance_page_bytes{0}, instance_page_bytes_peak{0}, binder_bytes{0},
	page_lookups{0}, page_slot_fills{0}, page_slot_conflicts{0},
	page_slots_filled{0};
std::array<std::atomic<std::uint64_t>, 5> refusals{};
/* The guest addresses AOT code stepped at most: library << 40 | offset. */
constexpr std::size_t step_slots = 32768;
std::array<std::atomic<std::uint64_t>, step_slots> step_key{};
std::array<std::atomic<std::uint64_t>, step_slots> step_count{};
std::array<std::atomic<std::uint64_t>, 64> library_enters{};
std::array<std::atomic<std::uint64_t>, 64> library_steps{};
/* Pages of a library compared with their stored hash, and pages whose
 * verification the shared table answered. */
std::array<std::atomic<std::uint64_t>, 64> library_verifications{};
std::array<std::atomic<std::uint64_t>, 64> library_shared_hits{};
/* Page verifications refused for lack of the kernel's immutable fact. */
std::array<std::atomic<std::uint64_t>, 64> library_mutable_pages{};
/* Page verifications that the epoch of a frame's record forced (see
 * Stats::epoch_rehashes). */
std::array<std::atomic<std::uint64_t>, 64> library_epoch_rehashes{};
/* Instances made of a library, and the ones made again (see
 * Stats::instance_rebinds); `instance_seen` holds a hash of every
 * (binder, address space, library, bias) that was made. */
std::array<std::atomic<std::uint64_t>, 64> library_instances{};
std::array<std::atomic<std::uint64_t>, 64> library_rebinds{};
constexpr std::size_t instance_seen_slots = 65536;
std::array<std::atomic<std::uint64_t>, instance_seen_slots> instance_seen{};
/* What find() answered: an entry, or why not (FindResult), in all and per
 * library where the answer names one; and what a cross-library lookup that
 * found no entry looked at (CrossMiss). */
std::array<std::atomic<std::uint64_t>, 8> find_results{};
std::array<std::array<std::atomic<std::uint64_t>, 8>, 64> library_finds{};
std::array<std::atomic<std::uint64_t>, 3> cross_miss_kinds{};
std::array<std::atomic<std::uint64_t>, 64> library_cross_misses{};
/* The dispatches that ended with the engine running the block, by the
 * reason no entry was taken (diagnostics::EngineReason), the guest
 * instructions the engine then ran (on a page the binder knows as a library
 * page, on another, or one it cannot classify), the dispatches in all and the
 * ones that took an entry, and how many lookups a dispatch made. */
constexpr std::size_t engine_reason_count =
	static_cast<std::size_t>(diagnostics::EngineReason::count);
std::array<std::atomic<std::uint64_t>, engine_reason_count> engine_dispatches{},
	engine_covered{}, engine_other{}, engine_unknown{};
std::atomic<std::uint64_t> dispatches{0}, dispatches_entered{0};
std::array<std::atomic<std::uint64_t>, 8> dispatch_finds{};
/* Refills by what the TLB entry held when the thunk asked: nothing at all
 * (a first touch, or a flush), the page the other kind holds, a page of the
 * same kind, a page of the other kind, the page itself already, a crossing of
 * a page boundary; and, per site, how many of each. The second index tells
 * whether the TLB's generation or change count had moved since this thread's
 * previous refill. */
constexpr std::size_t refill_cause_count = 6;
std::array<std::array<std::atomic<std::uint64_t>, 2>, refill_cause_count>
	refill_causes{};
std::array<std::atomic<std::uint64_t>, 2> refill_kinds{};
constexpr std::size_t refill_pc_slots = 32768;
std::array<std::atomic<std::uint64_t>, refill_pc_slots> refill_pc_key{};
std::array<std::array<std::atomic<std::uint64_t>, refill_cause_count>,
	refill_pc_slots> refill_pc_count{};
/* The last conflicting refill of each site: the page it asked for and the
 * page the entry held, one pair at a time: a sequence number (odd while a
 * writer holds the slot) lets a reader take both of one refill, and a writer
 * that finds the slot held drops its sample. */
std::array<std::array<std::atomic<std::uint64_t>, 2>, refill_pc_slots>
	refill_pc_last{};
std::array<std::atomic<std::uint64_t>, refill_pc_slots> refill_pc_seq{};
std::atomic<bool> reporter_started{false};
/* The audit of the executable-mapping counts of the memory layer, run by the
 * reporter thread: audits that held every address space at once, those that
 * could not, the records the last one compared, and the records whose count
 * differed from the recount in all of them and in the last. */
std::atomic<std::uint64_t> audit_runs{0}, audit_skipped{0}, audit_records{0},
	audit_mismatches{0}, audit_last_mismatches{0};
/* The time the exact audits took, in all and the longest: one holds every
 * address space's mapping changes (fork, exit, page faults that map) still
 * for the length of its scan, so the timing of a diagnostics run is read
 * with it. */
std::atomic<std::uint64_t> audit_nanos_total{0}, audit_nanos_max{0};
/* Audits that have begun, and the sequence number of the latest exact audit
 * that finished: a reader that saw `started` equal to n when the guest's
 * work was done knows that an exact audit with a number above n began after
 * it. */
std::atomic<std::uint64_t> audit_started{0}, audit_last_exact{0};
/* What the last audit that found a mismatch saw: its generations, the
 * executable words that named no record, and the first mismatches as the
 * record's count and the recount. */
std::atomic<std::uint64_t> audit_detail_generations{0}, audit_detail_unnamed{0};
std::array<std::atomic<std::uint64_t>, 4> audit_detail_count{},
	audit_detail_expected{};
/* The least stack any sampled thread had left at the points where the
 * runtime is deepest (entry, refill, page verification), and the stack size
 * of the thread that had it. */
std::atomic<std::uint64_t> stack_min_free{~std::uint64_t{0}};
std::atomic<std::uint64_t> stack_size_at_min{0};
std::atomic<std::uint64_t> stack_kind_at_min{0};
/* The same measure at the start of each run, in the engine's own frame
 * before any runtime call: the least over all threads, and the most any
 * sample site was deeper than the start of its own run on its own thread
 * (the stack the runtime adds to the engine's). */
std::atomic<std::uint64_t> stack_engine_free{~std::uint64_t{0}};
std::atomic<std::uint64_t> stack_max_growth{0};
} // namespace counters

/* A Linux task stack, in guest memory (never a host stack), aligned to its
 * size like every page-order allocation: the size the kernel configuration
 * gives (CONFIG_KERNEL_STACK_ORDER at 4 KiB pages), which the build derives
 * from the same file the kernel is configured from. The guest kernel's
 * threads run the executor on one. */
#ifndef UML_AOT_KERNEL_STACK_BYTES
#error "the build names the kernel's task stack size"
#endif
constexpr std::uint64_t kernel_stack_bytes = UML_AOT_KERNEL_STACK_BYTES;
static_assert((kernel_stack_bytes & (kernel_stack_bytes - 1)) == 0,
	"a task stack is a power-of-two page order");

/* Record how much of the stack the caller's frame is on is left. A frame on
 * the thread's own host stack or its signal stack measures against that
 * stack; any other is a Linux task stack, whose base is the frame rounded
 * down to the stack size. The report names which (stack_kind): 0 a task
 * stack, 1 the host stack, 2 the signal stack. */
#define UML_AOT_SAMPLE_STACK() sample_stack(__builtin_frame_address(0), false)
#define UML_AOT_SAMPLE_STACK_AT(lowest) sample_stack(lowest, false)
#define UML_AOT_SAMPLE_ENGINE_STACK() \
	sample_stack(__builtin_frame_address(0), true)

/* The engine's own depth at the start of the run this thread is in, and
 * the kind of stack it was on (a sample on the signal stack is not measured
 * against it). */
thread_local std::uint64_t t_run_start_left = ~std::uint64_t{0};
thread_local std::uint64_t t_run_start_kind = 0;

[[gnu::noinline]] void sample_stack(const void *frame, bool run_start) noexcept
{
	struct HostStacks {
		std::uintptr_t low[2];
		std::uintptr_t high[2];
	};
	thread_local const HostStacks host = [] {
		HostStacks stacks{};
		const auto self = pthread_self();
		const auto top = reinterpret_cast<std::uintptr_t>(
			pthread_get_stackaddr_np(self));
		stacks.high[0] = top;
		stacks.low[0] = top - pthread_get_stacksize_np(self);
		stack_t alternate;
		if (sigaltstack(nullptr, &alternate) == 0 &&
		    (alternate.ss_flags & SS_DISABLE) == 0) {
			stacks.low[1] = reinterpret_cast<std::uintptr_t>(
				alternate.ss_sp);
			stacks.high[1] = stacks.low[1] + alternate.ss_size;
		}
		return stacks;
	}();
	const auto here = reinterpret_cast<std::uintptr_t>(frame);
	std::uint64_t left = here & (kernel_stack_bytes - 1);
	std::uint64_t size = kernel_stack_bytes;
	std::uint64_t kind = 0;
	for (int i = 0; i < 2; ++i) {
		if (here >= host.low[i] && here < host.high[i]) {
			left = here - host.low[i];
			size = host.high[i] - host.low[i];
			kind = 1 + static_cast<std::uint64_t>(i);
			break;
		}
	}
	if (run_start) {
		t_run_start_left = left;
		t_run_start_kind = kind;
		auto engine = counters::stack_engine_free.load(
			std::memory_order_relaxed);
		while (left < engine &&
		       !counters::stack_engine_free.compare_exchange_weak(
		           engine, left, std::memory_order_relaxed)) {
		}
		return;
	}
	if (t_run_start_left != ~std::uint64_t{0} && t_run_start_kind == kind &&
	    t_run_start_left > left) {
		const auto growth = t_run_start_left - left;
		auto most = counters::stack_max_growth.load(
			std::memory_order_relaxed);
		while (growth > most &&
		       !counters::stack_max_growth.compare_exchange_weak(
		           most, growth, std::memory_order_relaxed)) {
		}
	}
	auto least = counters::stack_min_free.load(std::memory_order_relaxed);
	while (left < least) {
		if (counters::stack_min_free.compare_exchange_weak(
		        least, left, std::memory_order_relaxed)) {
			counters::stack_size_at_min.store(size,
				std::memory_order_relaxed);
			counters::stack_kind_at_min.store(kind,
				std::memory_order_relaxed);
			break;
		}
	}
}

#define UML_AOT_COUNT(name, amount) \
	counters::name.fetch_add((amount), std::memory_order_relaxed)
#define UML_AOT_COUNT_LIBRARY(name, library) \
	do { \
		if ((library) < 64) \
			counters::name[(library)].fetch_add(1, \
				std::memory_order_relaxed); \
	} while (0)

constexpr std::uint32_t no_library = 0xffffffffU;

/* The reason the last lookup of this thread that found no entry gave, and the
 * dispatch whose engine run is still to be counted (note_engine_run). */
thread_local diagnostics::EngineReason t_find_reason =
	diagnostics::EngineReason::no_entry;
struct PendingEngineRun final {
	bool set;
	std::uint8_t cover;
	diagnostics::EngineReason reason;
};
thread_local PendingEngineRun t_pending{};

[[nodiscard]] diagnostics::EngineReason engine_reason_of(
	FindResult result) noexcept
{
	switch (result) {
	case FindResult::not_ready:
		return diagnostics::EngineReason::not_ready;
	case FindResult::uncovered:
		return diagnostics::EngineReason::uncovered;
	case FindResult::poisoned:
		return diagnostics::EngineReason::poisoned;
	case FindResult::unverified:
		return diagnostics::EngineReason::unverified;
	case FindResult::stale:
		return diagnostics::EngineReason::stale;
	case FindResult::mutable_page:
		return diagnostics::EngineReason::mutable_page;
	case FindResult::hit:
	case FindResult::no_entry:
		break;
	}
	return diagnostics::EngineReason::no_entry;
}

void note_find(FindResult result, std::uint32_t library) noexcept
{
	t_find_reason = engine_reason_of(result);
	counters::find_results[static_cast<std::size_t>(result)].fetch_add(
		1, std::memory_order_relaxed);
	if (library < 64)
		counters::library_finds[library][static_cast<std::size_t>(result)]
			.fetch_add(1, std::memory_order_relaxed);
}

/* A binder made an instance, in the place of a live one or not. Whether it
 * had made this one before is told by a table of hashes that never
 * allocates (the report thread and the vCPU threads share it): a full table
 * stops telling, and the count of rebinds is then a lower bound. */
void note_instance(const void *binder, unsigned long generation,
	std::uint32_t library, std::uint64_t bias, bool evicted,
	std::size_t freed, std::size_t added) noexcept
{
	UML_AOT_COUNT(instances_created, 1);
	if (evicted)
		UML_AOT_COUNT(instance_evictions, 1);
	/* The page-state bytes the live instances of all binders hold, and the
	 * most they held at once. */
	counters::instance_page_bytes.fetch_sub(freed, std::memory_order_relaxed);
	const auto now = counters::instance_page_bytes.fetch_add(
		added, std::memory_order_relaxed) + added;
	auto peak = counters::instance_page_bytes_peak.load(
		std::memory_order_relaxed);
	while (now > peak && !counters::instance_page_bytes_peak
	       .compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
	}
	UML_AOT_COUNT_LIBRARY(library_instances, library);
	std::uint64_t key = reinterpret_cast<std::uintptr_t>(binder);
	for (const std::uint64_t part :
	     {static_cast<std::uint64_t>(generation), std::uint64_t{library}, bias}) {
		key ^= part + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
		key *= 0xff51afd7ed558ccdULL;
		key ^= key >> 33;
	}
	/* The last avalanche: parts that differ in high bits only (biases a
	 * few GiB apart) must still spread over the table. */
	key *= 0xc4ceb9fe1a85ec53ULL;
	key ^= key >> 29;
	key |= 1;
	std::size_t slot = key >> 20;
	for (std::size_t probe = 0; probe < 64; ++probe) {
		const auto at = (slot + probe) % counters::instance_seen_slots;
		auto stored =
			counters::instance_seen[at].load(std::memory_order_relaxed);
		if (stored == 0 &&
		    counters::instance_seen[at].compare_exchange_strong(
			    stored, key, std::memory_order_relaxed))
			return;
		if (stored == key) {
			UML_AOT_COUNT(instance_rebinds, 1);
			UML_AOT_COUNT_LIBRARY(library_rebinds, library);
			return;
		}
	}
}

/* The binder whose table the running vCPU thread's rewritten code uses. */
thread_local const Binder *g_cross_owner = nullptr;
thread_local unsigned long g_cross_generation = 0;

/* A cross-library lookup found no entry: inside a verified library but
 * not at an instruction it has one for (2), in a library instance the
 * table does not hold (1; unverified, or not verified at this sequence),
 * or in no instance this thread knows (0). */
void note_cross_miss(const CrossTable *table, std::uint64_t address) noexcept
{
	std::size_t kind = 0;
	std::uint32_t library = no_library;
	for (std::uint64_t index = 0; index < table->count; ++index) {
		const auto &entry = table->entries[index];
		if (address >= entry.low && address < entry.high) {
			kind = 2;
			library = entry.library;
			break;
		}
	}
	std::uint64_t offset = 0;
	if (kind == 0 && g_cross_owner != nullptr &&
	    g_cross_owner->locate(g_cross_generation, address, library, offset))
		kind = 1;
	counters::cross_miss_kinds[kind].fetch_add(1, std::memory_order_relaxed);
	if (library < 64)
		counters::library_cross_misses[library].fetch_add(
			1, std::memory_order_relaxed);
}

void note_step(std::uint64_t key) noexcept
{
	std::size_t slot = (key * 0x9e3779b97f4a7c15ULL) >> 49;
	for (std::size_t probe = 0; probe < 32; ++probe) {
		const auto at = (slot + probe) % counters::step_slots;
		auto seen = counters::step_key[at].load(std::memory_order_relaxed);
		if (seen == 0)
			counters::step_key[at].compare_exchange_strong(seen, key);
		if (counters::step_key[at].load(std::memory_order_relaxed) == key) {
			counters::step_count[at].fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
}

/* One audit of the memory layer's executable-mapping counts: every record
 * holds exactly the executable guest-code words that name it, and no
 * departure found nothing to count out. A mismatch of an audit that held every
 * address space at once is a bug in the accounting, never a race, so the guest
 * run that sees one fails (tests/integration/aot/run_aot_guest.py). */
void run_count_audit() noexcept
{
	const auto sequence =
		counters::audit_started.fetch_add(1, std::memory_order_relaxed) + 1;
	const auto started = std::chrono::steady_clock::now();
	const auto audit = ::uml::memory::audit_executable_counts();
	const auto nanos = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - started).count());
	if (!audit.exact) {
		counters::audit_skipped.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	counters::audit_runs.fetch_add(1, std::memory_order_relaxed);
	counters::audit_nanos_total.fetch_add(nanos, std::memory_order_relaxed);
	auto longest = counters::audit_nanos_max.load(std::memory_order_relaxed);
	while (nanos > longest &&
	       !counters::audit_nanos_max.compare_exchange_weak(longest, nanos,
			       std::memory_order_relaxed)) {
	}
	counters::audit_records.store(audit.records, std::memory_order_relaxed);
	counters::audit_last_mismatches.store(audit.mismatches,
		std::memory_order_relaxed);
	counters::audit_mismatches.fetch_add(audit.mismatches,
		std::memory_order_relaxed);
	if (audit.mismatches != 0) {
		counters::audit_detail_generations.store(audit.generations,
			std::memory_order_relaxed);
		counters::audit_detail_unnamed.store(audit.unnamed,
			std::memory_order_relaxed);
		for (std::size_t i = 0; i < audit.sample_count.size(); ++i) {
			counters::audit_detail_count[i].store(audit.sample_count[i],
				std::memory_order_relaxed);
			counters::audit_detail_expected[i].store(
				audit.sample_expected[i], std::memory_order_relaxed);
		}
	}
	/* Last, so that a report that shows this audit as finished shows what
	 * it found. */
	auto newest = counters::audit_last_exact.load(std::memory_order_relaxed);
	while (sequence > newest &&
	       !counters::audit_last_exact.compare_exchange_weak(newest, sequence,
			       std::memory_order_release, std::memory_order_relaxed)) {
	}
}

/* Written from a thread of its own: it allocates and does file I/O. The
 * file is made beside the report and renamed over it, so that a reader (the
 * runner reads it a few times a second, around each row) sees the whole of
 * one report and never the front of a new one. */
void write_report() noexcept
{
	/* Never destroyed: the reporter's thread goes on writing until the
	 * process is gone, and a process that leaves through exit() runs the
	 * static destructors while it does. */
	static std::mutex &writing = *new std::mutex;
	const std::lock_guard lock{writing};
	const char *path = std::getenv("DARWIN_UML_AOT_REPORT");
	if (path == nullptr)
		return;
	const std::string part = std::string{path} + ".part";
	std::FILE *file = std::fopen(part.c_str(), "w");
	if (file == nullptr)
		return;
	/* The instant the counters below were read, on the clock a reader on
	 * this host reads: taken before the first of them, so a report stamped
	 * later than the end of a piece of work holds only what the work left.
	 * The stamp, not the order in which reports appear, is the evidence: a
	 * report can be published late. */
	std::fprintf(file, "report at %llu\n", static_cast<unsigned long long>(
		clock_gettime_nsec_np(CLOCK_UPTIME_RAW)));
	auto get = [](const std::atomic<std::uint64_t> &value) {
		return static_cast<unsigned long long>(
			value.load(std::memory_order_relaxed));
	};
	std::fprintf(file,
		"binds %llu verifications %llu pages_hashed %llu unreadable %llu "
		"poisoned %llu enters %llu steps %llu retired %llu refills %llu "
		"refill_failures %llu cross_hits %llu cross_misses %llu\n",
		get(counters::binds), get(counters::verifications),
		get(counters::pages_hashed), get(counters::unreadable),
		get(counters::poisoned), get(counters::enters), get(counters::steps),
		get(counters::retired), get(counters::refills),
		get(counters::refill_failures), get(counters::cross_hits),
		get(counters::cross_misses));
	std::fprintf(file,
		"max_retired %llu verify_nanos %llu step_nanos %llu steps_run %llu "
		"code_bumps %llu history_queries %llu mops_slow %llu mops_fault %llu "
		"mops_illegal %llu shared_hits %llu mutable_pages %llu "
		"epoch_rehashes %llu stamp_queries %llu instances_created %llu "
		"instance_evictions %llu instance_rebinds %llu "
		"instance_page_bytes_peak %llu binder_bytes %llu\n",
		get(counters::max_retired), get(counters::verify_nanos),
		get(counters::step_nanos), get(counters::steps_run),
		get(counters::code_bumps), get(counters::history_queries),
		get(counters::mops_slow), get(counters::mops_fault),
		get(counters::mops_illegal), get(counters::shared_hits),
		get(counters::mutable_pages), get(counters::epoch_rehashes),
		get(counters::stamp_queries), get(counters::instances_created),
		get(counters::instance_evictions), get(counters::instance_rebinds),
		get(counters::instance_page_bytes_peak), get(counters::binder_bytes));
	{
		/* The binders' tables, reserved and in memory, the page states the
		 * live instances hold now, and how the page slots were used:
		 * lookups, binds into an empty slot, binds that replaced another
		 * page's slot, and the slots filled now. */
		unsigned long long count = 0, reserved = 0, resident = 0;
		const std::lock_guard binders_guard{binders_lock()};
		for (const auto *binder : binders()) {
			++count;
			reserved += binder->tables_bytes();
			resident += binder->tables_resident_bytes();
		}
		std::fprintf(file, "binders %llu binder_tables_bytes %llu "
			"binder_tables_resident_bytes %llu instance_page_bytes %llu "
			"page_lookups %llu page_slot_fills %llu page_slot_conflicts %llu "
			"page_slots_filled %llu\n",
			count, reserved, resident, get(counters::instance_page_bytes),
			get(counters::page_lookups), get(counters::page_slot_fills),
			get(counters::page_slot_conflicts),
			get(counters::page_slots_filled));
	}
	std::fprintf(file, "stack_free %llu stack_size %llu stack_kind %llu "
		"stack_task_bytes %llu stack_engine_free %llu stack_growth %llu\n",
		get(counters::stack_min_free), get(counters::stack_size_at_min),
		get(counters::stack_kind_at_min),
		static_cast<unsigned long long>(kernel_stack_bytes),
		get(counters::stack_engine_free), get(counters::stack_max_growth));
	std::fprintf(file, "libraries %u libraries_refused %u\n",
		directory().libraries, directory().refused);
	std::fprintf(file, "count_audit runs %llu skipped %llu records %llu "
		"mismatches %llu last_mismatches %llu anomalies %llu "
		"nanos_total %llu nanos_max %llu started %llu last_exact %llu\n",
		get(counters::audit_runs), get(counters::audit_skipped),
		get(counters::audit_records), get(counters::audit_mismatches),
		get(counters::audit_last_mismatches),
		static_cast<unsigned long long>(::uml::memory::count_anomalies()),
		get(counters::audit_nanos_total), get(counters::audit_nanos_max),
		get(counters::audit_started), get(counters::audit_last_exact));
	std::fprintf(file, "count_audit_detail generations %llu unnamed %llu "
		"c0 %llu e0 %llu c1 %llu e1 %llu c2 %llu e2 %llu c3 %llu e3 %llu\n",
		get(counters::audit_detail_generations),
		get(counters::audit_detail_unnamed),
		get(counters::audit_detail_count[0]),
		get(counters::audit_detail_expected[0]),
		get(counters::audit_detail_count[1]),
		get(counters::audit_detail_expected[1]),
		get(counters::audit_detail_count[2]),
		get(counters::audit_detail_expected[2]),
		get(counters::audit_detail_count[3]),
		get(counters::audit_detail_expected[3]));
	std::fprintf(file, "refused tbi %llu sp %llu exclusive %llu fp %llu "
		"short %llu\n",
		get(counters::refusals[0]), get(counters::refusals[1]),
		get(counters::refusals[2]), get(counters::refusals[3]),
		get(counters::refusals[4]));
	std::fprintf(file, "find");
	for (std::size_t kind = 0; kind < counters::find_results.size(); ++kind)
		std::fprintf(file, " %s %llu", find_result_name(kind),
			get(counters::find_results[kind]));
	std::fprintf(file, "\ncross_miss uncovered %llu unbound %llu no_entry "
		"%llu\n", get(counters::cross_miss_kinds[0]),
		get(counters::cross_miss_kinds[1]),
		get(counters::cross_miss_kinds[2]));
	std::fprintf(file, "dispatch total %llu entered %llu finds0 %llu "
		"finds1 %llu finds2 %llu finds3 %llu finds4 %llu finds5_8 %llu "
		"finds9_16 %llu finds17p %llu\n", get(counters::dispatches),
		get(counters::dispatches_entered),
		get(counters::dispatch_finds[0]), get(counters::dispatch_finds[1]),
		get(counters::dispatch_finds[2]), get(counters::dispatch_finds[3]),
		get(counters::dispatch_finds[4]), get(counters::dispatch_finds[5]),
		get(counters::dispatch_finds[6]), get(counters::dispatch_finds[7]));
	{
		constexpr const char *reasons[] = {"retry", "exclusive_monitor",
			"tbi_disabled", "sp_misaligned", "fp_environment",
			"short_entry", "not_ready", "uncovered", "poisoned",
			"unverified", "no_entry", "stale", "mutable_page",
			"no_progress"};
		for (std::size_t reason = 0; reason < counters::engine_reason_count;
		     ++reason)
			std::fprintf(file, "engine_dispatch %s dispatches %llu "
				"covered %llu other %llu unknown %llu\n", reasons[reason],
				get(counters::engine_dispatches[reason]),
				get(counters::engine_covered[reason]),
				get(counters::engine_other[reason]),
				get(counters::engine_unknown[reason]));
		constexpr const char *causes[] = {"empty", "upgrade", "conflict",
			"conflict_other_kind", "already", "crossing"};
		for (std::size_t cause = 0; cause < counters::refill_cause_count;
		     ++cause)
			std::fprintf(file, "refill_cause %s steady %llu moved %llu\n",
				causes[cause], get(counters::refill_causes[cause][0]),
				get(counters::refill_causes[cause][1]));
		std::fprintf(file, "refill_kind read %llu write %llu\n",
			get(counters::refill_kinds[0]), get(counters::refill_kinds[1]));
	}
	for (std::size_t library = 0; library < 64; ++library) {
		const auto enters = get(counters::library_enters[library]);
		unsigned long long finds = 0;
		for (const auto &counter : counters::library_finds[library])
			finds += get(counter);
		const auto cross = get(counters::library_cross_misses[library]);
		const auto hashed = get(counters::library_verifications[library]);
		const auto shared = get(counters::library_shared_hits[library]);
		const auto refused = get(counters::library_mutable_pages[library]);
		const auto forced = get(counters::library_epoch_rehashes[library]);
		const auto made = get(counters::library_instances[library]);
		const auto again = get(counters::library_rebinds[library]);
		if (enters == 0 && finds == 0 && cross == 0 && hashed == 0 &&
		    shared == 0 && refused == 0 && forced == 0 && made == 0)
			continue;
		std::fprintf(file, "library %zu enters %llu steps %llu cross_misses "
			"%llu verified %llu shared %llu mutable %llu rehash %llu "
			"instances %llu rebound %llu",
			library, enters, get(counters::library_steps[library]), cross,
			hashed, shared, refused, forced, made, again);
		for (std::size_t kind = 0;
		     kind < counters::library_finds[library].size(); ++kind)
			std::fprintf(file, " %s %llu", find_result_name(kind),
				get(counters::library_finds[library][kind]));
		std::fprintf(file, "\n");
	}
	for (std::size_t slot = 0; slot < counters::step_slots; ++slot) {
		const auto count = get(counters::step_count[slot]);
		if (count != 0) {
			const auto key = get(counters::step_key[slot]);
			std::fprintf(file, "step %llu %llx %llu\n", key >> 40,
				key & ((1ULL << 40) - 1), count);
		}
	}
	if (std::fclose(file) != 0 || std::rename(part.c_str(), path) != 0)
		(void)std::remove(part.c_str());
}

/* The per-site refill histogram, to the file DARWIN_UML_AOT_REPORT_PC names
 * (when it is set), made beside it and renamed over it like the report: one
 * line per site, `refill_site <library> <guest offset in hex> <total> <the
 * six causes of counters::refill_causes> last <page asked for> <page held>`,
 * the last two from the site's last conflicting refill. */
void write_refill_sites() noexcept
{
	const char *path = std::getenv("DARWIN_UML_AOT_REPORT_PC");
	if (path == nullptr)
		return;
	const std::string part = std::string{path} + ".part";
	std::FILE *file = std::fopen(part.c_str(), "w");
	if (file == nullptr)
		return;
	for (std::size_t slot = 0; slot < counters::refill_pc_slots; ++slot) {
		const auto key = counters::refill_pc_key[slot].load(
			std::memory_order_relaxed);
		if (key == 0)
			continue;
		unsigned long long total = 0, each[counters::refill_cause_count];
		for (std::size_t cause = 0; cause < counters::refill_cause_count;
		     ++cause) {
			each[cause] = counters::refill_pc_count[slot][cause].load(
				std::memory_order_relaxed);
			total += each[cause];
		}
		unsigned long long wanted = 0, held = 0;
		for (int attempt = 0; attempt < 8; ++attempt) {
			const auto before = counters::refill_pc_seq[slot].load(
				std::memory_order_acquire);
			if ((before & 1U) != 0)
				continue;
			const auto a = counters::refill_pc_last[slot][0].load(
				std::memory_order_relaxed);
			const auto b = counters::refill_pc_last[slot][1].load(
				std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_acquire);
			if (counters::refill_pc_seq[slot].load(
				    std::memory_order_relaxed) == before) {
				wanted = a;
				held = b;
				break;
			}
		}
		std::fprintf(file, "refill_site %llu %llx %llu %llu %llu %llu %llu "
			"%llu %llu last %llx %llx\n", (key >> 40) - 1,
			static_cast<unsigned long long>(key & ((1ULL << 40) - 1)), total,
			each[0], each[1], each[2], each[3], each[4], each[5], wanted,
			held);
	}
	if (std::fclose(file) != 0 || std::rename(part.c_str(), path) != 0)
		(void)std::remove(part.c_str());
}

void start_reporter() noexcept
{
	if (std::getenv("DARWIN_UML_AOT_REPORT") == nullptr ||
	    counters::reporter_started.exchange(true))
		return;
	try {
		std::thread([] {
			for (unsigned tick = 0;; ++tick) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				/* The audit stops the address spaces' mapping changes for
				 * the length of a scan, so it runs every few ticks. */
				if (tick % 4 == 0)
					run_count_audit();
				write_report();
				if (tick % 8 == 7)
					write_refill_sites();
			}
		}).detach();
	} catch (...) {
	}
}
#else
#define UML_AOT_COUNT(name, amount) ((void)0)
inline void note_find(FindResult, std::uint32_t) noexcept {}
inline void note_instance(const void *, unsigned long, std::uint32_t,
	std::uint64_t, bool, std::size_t, std::size_t) noexcept {}
#define UML_AOT_COUNT_LIBRARY(name, library) ((void)0)
#define UML_AOT_SAMPLE_STACK() ((void)0)
#define UML_AOT_SAMPLE_STACK_AT(lowest) ((void)0)
#define UML_AOT_SAMPLE_ENGINE_STACK() ((void)0)
constexpr std::uint32_t no_library = 0xffffffffU;
#endif

/* On unless DARWIN_UML_AOT is "0": the component is the default wherever a
 * product carries it, and the environment is the switch a test or a
 * measurement uses to compare against the engine alone. */
[[nodiscard]] bool environment_enabled() noexcept
{
	const char *value = std::getenv("DARWIN_UML_AOT");
	const bool off = value != nullptr && value[0] == '0' && value[1] == '\0';
	return !off && directory().valid;
}

/* Rewritten code asks for the TLB entry of a page it missed (see the refill
 * thunk in tools/aot/rewrite.py): fill it as the engine does and say whether
 * the entry now holds the page. A fault is not recorded for the guest here;
 * the instruction steps and the engine takes it. An access that crosses a
 * page also steps: the probe found it as a miss. This is noexcept although
 * Tlb::translate is not: the thunk that calls it has no unwind information,
 * so an exception could not be handled there, and one that escaped the fill
 * would end the process (std::terminate) rather than reach the guest. */
std::uint64_t refill(void *tlb_pointer, std::uint64_t address,
	std::uint64_t code) noexcept
{
	UML_AOT_COUNT(refills, 1);
	UML_AOT_SAMPLE_STACK();
	auto &tlb = *static_cast<::uml::memory::Tlb *>(tlb_pointer);
	const auto kind = (code & 0xff) == 1 ? ::uml::memory::Access::write :
		::uml::memory::Access::read;
	const auto bytes = code >> 8;
	if (bytes > 1)
		address -= bytes - 1;
	if ((address & (page_size - 1)) + bytes > page_size) {
		UML_AOT_COUNT(refill_failures, 1);
		return 0;
	}
	if (tlb.translate(address, kind) == nullptr) {
		UML_AOT_COUNT(refill_failures, 1);
		return 0;
	}
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	const auto &entry = tlb.entries[page & (::uml::memory::tlb_entry_count - 1)];
	const bool filled = (kind == ::uml::memory::Access::write ?
		entry.write_page : entry.read_page) == page;
#if UML_AOT_DIAGNOSTICS
	if (filled && g_refill_stress.load(std::memory_order_relaxed)) {
		/* Leave only this access's entry in the TLB. */
		for (auto &other : tlb.entries)
			other = ::uml::memory::TlbEntry{};
		if (tlb.translate(address, kind) == nullptr)
			return 0;
		scramble_registers();
	}
#endif
	return filled ? 1 : 0;
}

/* The epilogue of a memory copy or set that the thunk of its registers did
 * not carry out (tools/aot/rewrite.py: emit_mops_thunks): the engine's own
 * routine on the registers the prologue formatted. */
std::uint64_t mops(CpuState *cpu, ::uml::memory::Tlb *tlb,
	std::uint64_t *registers, std::uint64_t kind) noexcept
{
	using namespace ::uml::cpu::arm64::detail;
	UML_AOT_COUNT(mops_slow, 1);
	cpu->nzcv = static_cast<std::uint32_t>(registers[3]);
	MopsOutcome outcome;
	switch (kind) {
	case 0:
		outcome = mops_epilogue<MopsFamily::copy_forward>(*cpu, *tlb,
			registers[0], registers[1], registers[2]);
		break;
	case 1:
		outcome = mops_epilogue<MopsFamily::copy_general>(*cpu, *tlb,
			registers[0], registers[1], registers[2]);
		break;
	default:
		outcome = mops_epilogue<MopsFamily::set>(*cpu, *tlb,
			registers[0], registers[1], registers[2]);
		break;
	}
	if (outcome == MopsOutcome::fault)
		UML_AOT_COUNT(mops_fault, 1);
	else if (outcome != MopsOutcome::complete)
		UML_AOT_COUNT(mops_illegal, 1);
	return outcome == MopsOutcome::complete ? 1 :
		outcome == MopsOutcome::fault ? 0 : 2;
}

#if UML_AOT_DIAGNOSTICS
/* The library whose unit holds the code at `blob_offset` (units lie in
 * library order), or no_library. */
[[nodiscard]] std::uint32_t library_of_unit_offset(
	std::size_t blob_offset) noexcept
{
	const auto &dir = directory();
	std::uint32_t low = 0, high = dir.libraries;
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		if (dir.library(middle).unit_offset <= blob_offset)
			low = middle + 1;
		else
			high = middle;
	}
	return low == 0 ? no_library : low - 1;
}

/* What the TLB entry holds when a refill is asked for, as a cause (see
 * counters::refill_causes). `site` is the instruction's byte offset in its
 * library's entry table, the thunk's T1. */
void note_refill(const void *tlb_pointer, std::uint64_t address,
	std::uint64_t code, std::uint64_t site, const void *caller) noexcept
{
	const auto &tlb = *static_cast<const ::uml::memory::Tlb *>(tlb_pointer);
	const auto kind = code & 0xff;
	const auto bytes = code >> 8;
	if (bytes > 1)
		address -= bytes - 1;
	std::size_t cause;
	std::uint64_t held = 0;
	if ((address & (page_size - 1)) + bytes > page_size) {
		cause = 5;
	} else {
		const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
		const auto &entry =
			tlb.entries[page & (::uml::memory::tlb_entry_count - 1)];
		const auto same = kind == 1 ? entry.write_page : entry.read_page;
		const auto other = kind == 1 ? entry.read_page : entry.write_page;
		constexpr auto empty = ::uml::memory::tlb_empty_key;
		if (same == page)
			cause = 4;
		else if (same == empty)
			cause = other == empty ? 0 : other == page ? 1 : 3;
		else {
			cause = 2;
			held = same;
		}
	}
	thread_local const void *last_tlb = nullptr;
	thread_local std::uint64_t last_generation = 0, last_changes = 0;
	const std::size_t moved = &tlb != last_tlb ||
		tlb.generation != last_generation || tlb.changes != last_changes;
	last_tlb = &tlb;
	last_generation = tlb.generation;
	last_changes = tlb.changes;
	counters::refill_causes[cause][moved].fetch_add(1, std::memory_order_relaxed);
	counters::refill_kinds[kind & 1].fetch_add(1, std::memory_order_relaxed);
	const auto library = library_of_unit_offset(
		static_cast<std::size_t>(static_cast<const std::uint8_t *>(caller) -
			uml_aot_blob));
	if (library == no_library)
		return;
	const std::uint64_t key = ((std::uint64_t{library} + 1) << 40) |
		(directory().library(library).table_base + site);
	std::size_t slot = (key * 0x9e3779b97f4a7c15ULL) >> 49;
	for (std::size_t probe = 0; probe < 32; ++probe) {
		const auto at = (slot + probe) % counters::refill_pc_slots;
		auto seen = counters::refill_pc_key[at].load(std::memory_order_relaxed);
		if (seen == 0)
			counters::refill_pc_key[at].compare_exchange_strong(seen, key);
		if (counters::refill_pc_key[at].load(std::memory_order_relaxed) == key) {
			counters::refill_pc_count[at][cause].fetch_add(
				1, std::memory_order_relaxed);
			if (cause == 2) {
				auto seq = counters::refill_pc_seq[at].load(
					std::memory_order_relaxed);
				if ((seq & 1U) == 0 &&
				    counters::refill_pc_seq[at].compare_exchange_strong(
					    seq, seq + 1, std::memory_order_acquire)) {
					counters::refill_pc_last[at][0].store(
						address >> DARWIN_UML_GUEST_PAGE_SHIFT,
						std::memory_order_relaxed);
					counters::refill_pc_last[at][1].store(
						held, std::memory_order_relaxed);
					counters::refill_pc_seq[at].store(
						seq + 2, std::memory_order_release);
				}
			}
			return;
		}
	}
}
#endif

/* Rewritten code branches to an address outside its own library: answer the
 * native code of the verified instance that holds it, or none. */
Target lookup(const CrossTable *table, std::uint64_t address) noexcept
{
	for (std::uint64_t index = 0; index < table->count; ++index) {
		const auto &entry = table->entries[index];
		if (address < entry.low || address >= entry.high)
			continue;
		const auto library = directory().library(entry.library);
		const std::uint64_t offset = address - entry.bias;
		if ((offset & 3U) != 0)
			break;
		/* The target's text page must have matched its stored hash; a
		 * page not yet verified ends the entry at the branch, and the
		 * host verifies it before the next one. */
		if (entry.pages[(offset - library.table_base) / page_size] == 0)
			break;
		const auto native =
			native_entry(library, (offset - library.table_base) / 4);
		if (native == 0)
			break;
		UML_AOT_COUNT(cross_hits, 1);
		return {uml_aot_blob + library.unit_offset + native, &entry};
	}
	UML_AOT_COUNT(cross_misses, 1);
#if UML_AOT_DIAGNOSTICS
	note_cross_miss(table, address);
#endif
	return {};
}

} // namespace

#if UML_AOT_DIAGNOSTICS
/* The refill thunk's call, with the instruction's slot (the thunk's T1,
 * x27, which a callee keeps) as a fourth argument: the trampoline below puts
 * it in x3 and branches, so the return address stays the thunk's. */
extern "C" std::uint64_t uml_aot_refill_site(void *tlb, std::uint64_t address,
	std::uint64_t code, std::uint64_t site) noexcept;
asm(".text\n"
    ".p2align 2\n"
    ".globl _uml_aot_refill_diag\n"
    "_uml_aot_refill_diag:\n"
    "\tmov x3, x27\n"
    "\tb _uml_aot_refill_site\n");
extern "C" std::uint64_t uml_aot_refill_diag(void *tlb, std::uint64_t address,
	std::uint64_t code) noexcept;
extern "C" std::uint64_t uml_aot_refill_site(void *tlb, std::uint64_t address,
	std::uint64_t code, std::uint64_t site) noexcept
{
	note_refill(tlb, address, code, site, __builtin_return_address(0));
	return refill(tlb, address, code);
}
#endif

bool enabled() noexcept
{
	auto state = g_state.load(std::memory_order_relaxed);
	if (state < 0) {
#if UML_AOT_DIAGNOSTICS
		/* A test that drives executors without a launch has no release to
		 * name; only a build that carries the test hooks answers for it. */
		state = environment_enabled() ? 1 : 0;
#else
		state = 0;
#endif
		g_state.store(state, std::memory_order_relaxed);
#if UML_AOT_DIAGNOSTICS
		if (state != 0)
			start_reporter();
#endif
	}
	return state != 0;
}

std::optional<::uml::cpu::AotRelease> embedded_release() noexcept
{
	if (!directory().valid)
		return std::nullopt;
	return header_release();
}

bool select(::uml::cpu::AotSelection selection,
	const ::uml::cpu::AotRelease *release) noexcept
{
	using ::uml::cpu::AotSelection;
	/* A blob that is not valid, or was made from another release than the
	 * one booted, is never entered: not under `automatic`, and a launch that
	 * insists on it does not start. */
	const bool fits = directory().valid && release != nullptr &&
		made_from(*release);
	bool wanted = false;
	switch (selection) {
	case AotSelection::automatic:
		wanted = fits && environment_enabled();
		break;
	case AotSelection::on:
		if (!fits)
			return false;
		wanted = true;
		break;
	case AotSelection::off:
		break;
	}
	/* The decision of this run, taken before any executor of it exists (the
	 * executors read it once, when they are made, so a run's own executors
	 * all agree). A run that restarts in the process decides again: one the
	 * blob no longer fits is off, never entering it. A session that ran
	 * without the component does not turn it on later (end_session() starts
	 * the next one afresh): a launch under
	 * `automatic` then runs on the engine, which is always correct, and one
	 * that insists on it does not start. */
	auto state = g_state.load(std::memory_order_relaxed);
	for (;;) {
		if (state < 0) {
			if (!g_state.compare_exchange_weak(state, wanted ? 1 : 0,
				    std::memory_order_relaxed))
				continue;
#if UML_AOT_DIAGNOSTICS
			if (wanted)
				start_reporter();
#endif
			return true;
		}
		if ((state != 0) == wanted)
			return true;
		if (wanted)
			return selection != AotSelection::on;
		if (g_state.compare_exchange_weak(state, 0,
			    std::memory_order_relaxed))
			return true;
	}
}

void end_session() noexcept
{
	g_state.store(-1, std::memory_order_relaxed);
}

int choice() noexcept
{
	return g_state.load(std::memory_order_relaxed);
}

void restore_choice(int choice) noexcept
{
	g_state.store(choice, std::memory_order_relaxed);
}

void note_refusal(Refusal reason) noexcept
{
#if UML_AOT_DIAGNOSTICS
	counters::refusals[static_cast<std::size_t>(reason)].fetch_add(
		1, std::memory_order_relaxed);
#else
	(void)reason;
#endif
}

void note_run_start() noexcept
{
#if UML_AOT_DIAGNOSTICS
	UML_AOT_SAMPLE_ENGINE_STACK();
	if (const auto hook = g_run_hook.load(std::memory_order_acquire))
		hook(g_run_hook_context.load(std::memory_order_relaxed));
#endif
}

std::uint64_t learning_epoch() noexcept
{
#if UML_AOT_DIAGNOSTICS
	return g_learning_epoch.load(std::memory_order_relaxed);
#else
	return 0;
#endif
}

std::uint64_t step_begin() noexcept
{
#if UML_AOT_DIAGNOSTICS
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
#else
	return 0;
#endif
}

void step_end(std::uint64_t began) noexcept
{
#if UML_AOT_DIAGNOSTICS
	counters::step_nanos.fetch_add(step_begin() - began,
		std::memory_order_relaxed);
	counters::steps_run.fetch_add(1, std::memory_order_relaxed);
#else
	(void)began;
#endif
}

#if UML_AOT_DIAGNOSTICS
namespace diagnostics {

void set_enabled(std::optional<bool> state) noexcept
{
	g_learning_epoch.fetch_add(1, std::memory_order_relaxed);
	g_state.store(!state.has_value() ? -1 :
		(*state && directory().valid ? 1 : 0), std::memory_order_relaxed);
}

void set_refill_stress(bool on) noexcept
{
	g_refill_stress.store(on, std::memory_order_relaxed);
}

void set_run_hook(void (*hook)(void *), void *context) noexcept
{
	g_run_hook_context.store(context, std::memory_order_relaxed);
	g_run_hook.store(hook, std::memory_order_release);
}

void clear_shared_verdicts() noexcept
{
	g_verdicts.clear();
}

EngineReason find_reason() noexcept
{
	return t_find_reason;
}

void note_engine_dispatch(const Binder &binder, unsigned long generation,
	std::uint64_t pc, EngineReason reason) noexcept
{
	/* The lookup's own answers say whether the page is a library page; the
	 * refusals and the retry rule come before any lookup, so they ask the
	 * binder, through a small cache of this thread's answers. */
	std::uint8_t cover = 2;
	switch (reason) {
	case EngineReason::not_ready:
	case EngineReason::uncovered:
		cover = 0;
		break;
	case EngineReason::poisoned:
	case EngineReason::unverified:
	case EngineReason::no_entry:
	case EngineReason::stale:
	case EngineReason::mutable_page:
		cover = 1;
		break;
	default: {
		struct Known final {
			std::uint64_t page;
			unsigned long generation;
			bool covered;
		};
		thread_local std::array<Known, 512> known{};
		const auto page = pc >> 12;
		auto &slot = known[page % known.size()];
		/* Only a page found to be covered is kept: a page asked about before
		 * the binder discovered its library is asked again, so a later refusal
		 * on it is charged to the library. */
		if (slot.page != page + 1 || slot.generation != generation ||
		    !slot.covered) {
			std::uint32_t library;
			std::uint64_t offset;
			slot = {page + 1, generation,
				binder.locate(generation, pc, library, offset)};
		}
		cover = slot.covered ? 1 : 0;
		break;
	}
	}
	t_pending = {true, cover, reason};
	counters::engine_dispatches[static_cast<std::size_t>(reason)].fetch_add(
		1, std::memory_order_relaxed);
}

void note_engine_run(std::uint64_t instructions) noexcept
{
	if (!t_pending.set)
		return;
	t_pending.set = false;
	auto &row = t_pending.cover == 1 ? counters::engine_covered :
		t_pending.cover == 0 ? counters::engine_other :
		counters::engine_unknown;
	row[static_cast<std::size_t>(t_pending.reason)].fetch_add(
		instructions, std::memory_order_relaxed);
}

void note_dispatch(unsigned finds, unsigned enters) noexcept
{
	counters::dispatches.fetch_add(1, std::memory_order_relaxed);
	if (enters != 0)
		counters::dispatches_entered.fetch_add(1, std::memory_order_relaxed);
	const std::size_t bucket = finds <= 4 ? finds : finds <= 8 ? 5 :
		finds <= 16 ? 6 : 7;
	counters::dispatch_finds[bucket].fetch_add(1, std::memory_order_relaxed);
}

std::size_t instance_capacity() noexcept
{
	return max_instances;
}

Stats stats() noexcept
{
	auto get = [](const std::atomic<std::uint64_t> &value) {
		return value.load(std::memory_order_relaxed);
	};
	Stats result{get(counters::binds), get(counters::verifications),
		get(counters::pages_hashed), get(counters::unreadable),
		get(counters::poisoned), get(counters::enters), get(counters::steps),
		get(counters::retired), get(counters::refills),
		get(counters::refill_failures), get(counters::cross_hits),
		get(counters::cross_misses), {}};
	for (std::size_t i = 0; i < result.refusals.size(); ++i)
		result.refusals[i] = get(counters::refusals[i]);
	result.max_retired = get(counters::max_retired);
	result.verify_nanos = get(counters::verify_nanos);
	result.step_nanos = get(counters::step_nanos);
	result.steps_run = get(counters::steps_run);
	result.code_bumps = get(counters::code_bumps);
	result.history_queries = get(counters::history_queries);
	result.mops_slow = get(counters::mops_slow);
	result.mops_fault = get(counters::mops_fault);
	result.mops_illegal = get(counters::mops_illegal);
	for (std::size_t i = 0; i < result.finds.size(); ++i)
		result.finds[i] = get(counters::find_results[i]);
	result.shared_hits = get(counters::shared_hits);
	result.mutable_pages = get(counters::mutable_pages);
	result.epoch_rehashes = get(counters::epoch_rehashes);
	result.stamp_queries = get(counters::stamp_queries);
	result.instances_created = get(counters::instances_created);
	result.instance_evictions = get(counters::instance_evictions);
	result.instance_rebinds = get(counters::instance_rebinds);
	result.instance_page_bytes_peak = get(counters::instance_page_bytes_peak);
	result.binder_bytes = get(counters::binder_bytes);
	result.instance_page_bytes = get(counters::instance_page_bytes);
	result.page_slots_filled = get(counters::page_slots_filled);
	result.stack_min_free = get(counters::stack_min_free);
	result.stack_size_at_min = get(counters::stack_size_at_min);
	result.stack_kind_at_min = get(counters::stack_kind_at_min);
	result.stack_task_bytes = kernel_stack_bytes;
	result.stack_engine_free = get(counters::stack_engine_free);
	result.stack_max_growth = get(counters::stack_max_growth);
	result.libraries_refused = directory().refused;
	return result;
}

void reset_stats() noexcept
{
	for (auto *counter : {&counters::binds, &counters::verifications,
		     &counters::pages_hashed, &counters::unreadable,
		     &counters::poisoned, &counters::enters, &counters::steps,
		     &counters::retired, &counters::refills,
		     &counters::refill_failures, &counters::cross_hits,
		     &counters::cross_misses, &counters::max_retired,
		     &counters::verify_nanos, &counters::step_nanos,
		     &counters::steps_run, &counters::code_bumps,
		     &counters::history_queries, &counters::mops_slow,
		     &counters::mops_fault, &counters::mops_illegal,
		     &counters::shared_hits, &counters::mutable_pages,
		     &counters::epoch_rehashes, &counters::stamp_queries,
		     &counters::instances_created, &counters::instance_evictions,
		     &counters::instance_rebinds, &counters::instance_page_bytes_peak})
		counter->store(0, std::memory_order_relaxed);
	for (auto &counter : counters::library_instances)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::library_rebinds)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::instance_seen)
		counter.store(0, std::memory_order_relaxed);
	for (auto *counter : {&counters::stack_min_free, &counters::stack_engine_free})
		counter->store(~std::uint64_t{0}, std::memory_order_relaxed);
	for (auto *counter : {&counters::stack_size_at_min,
		     &counters::stack_kind_at_min, &counters::stack_max_growth})
		counter->store(0, std::memory_order_relaxed);
	for (auto &counter : counters::refusals)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::find_results)
		counter.store(0, std::memory_order_relaxed);
	for (auto &row : counters::library_finds)
		for (auto &counter : row)
			counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::cross_miss_kinds)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::library_cross_misses)
		counter.store(0, std::memory_order_relaxed);
	for (auto *row : {&counters::engine_dispatches, &counters::engine_covered,
		     &counters::engine_other, &counters::engine_unknown})
		for (auto &counter : *row)
			counter.store(0, std::memory_order_relaxed);
	for (auto *counter : {&counters::dispatches, &counters::dispatches_entered})
		counter->store(0, std::memory_order_relaxed);
	for (auto &counter : counters::dispatch_finds)
		counter.store(0, std::memory_order_relaxed);
	for (auto &row : counters::refill_causes)
		for (auto &counter : row)
			counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::refill_kinds)
		counter.store(0, std::memory_order_relaxed);
	for (auto &counter : counters::refill_pc_key)
		counter.store(0, std::memory_order_relaxed);
	for (auto &row : counters::refill_pc_count)
		for (auto &counter : row)
			counter.store(0, std::memory_order_relaxed);
}

} // namespace diagnostics
#endif

Binder::Binder() noexcept
{
#if UML_AOT_DIAGNOSTICS
	cross_.refill = &uml_aot_refill_diag;
#else
	cross_.refill = &refill;
#endif
	cross_.lookup = &lookup;
	cross_.mops = &mops;
	const auto whole = [](std::size_t bytes) {
		return (bytes + host_page - 1) & ~(host_page - 1);
	};
	const auto instances_bytes = whole(max_instances * sizeof(Instance));
	const auto heads_bytes = whole(index_buckets * sizeof(std::uint16_t));
	const auto size = instances_bytes + heads_bytes +
		whole(page_slots * sizeof(PageSlot));
	void *const mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mapping != MAP_FAILED) {
		auto *const base = static_cast<std::byte *>(mapping);
		tables_ = mapping;
		tables_size_ = size;
		instances_ = reinterpret_cast<Instance *>(base);
		heads_ = reinterpret_cast<std::uint16_t *>(base + instances_bytes);
		pages_ = reinterpret_cast<PageSlot *>(
			base + instances_bytes + heads_bytes);
	}
#if UML_AOT_DIAGNOSTICS
	counters::binder_bytes.store(sizeof(Binder), std::memory_order_relaxed);
	const std::lock_guard guard{binders_lock()};
	binders().push_back(this);
#endif
}

Binder::~Binder()
{
#if UML_AOT_DIAGNOSTICS
	/* The page states the live instances held leave the count with them. */
	std::size_t held = 0;
	for (std::size_t at = 0; at < used_; ++at)
		held += instances_[at].page_count;
	counters::instance_page_bytes.fetch_sub(held, std::memory_order_relaxed);
	/* So do the page slots it filled. */
	std::size_t filled = 0;
	if (pages_ != nullptr)
		for (std::size_t at = 0; at < page_slots; ++at)
			if (pages_[at].page != 0)
				++filled;
	counters::page_slots_filled.fetch_sub(filled, std::memory_order_relaxed);
	const std::lock_guard guard{binders_lock()};
	std::erase(binders(), this);
#endif
	for (std::size_t at = 0; at < used_; ++at)
		std::free(instances_[at].pages);
	if (tables_ != nullptr)
		munmap(tables_, tables_size_);
}

#if UML_AOT_DIAGNOSTICS
std::size_t Binder::tables_bytes() const noexcept
{
	return tables_size_;
}

std::size_t Binder::tables_resident_bytes() const noexcept
{
	if (tables_ == nullptr)
		return 0;
	std::vector<char> in_core(tables_size_ / host_page);
	if (mincore(tables_, tables_size_, in_core.data()) != 0)
		return 0;
	std::size_t count = 0;
	for (const char page : in_core)
		if ((page & MINCORE_INCORE) != 0)
			++count;
	return count * host_page;
}
#endif

std::size_t Binder::bucket_of(unsigned long generation, std::uint32_t library,
	std::uint64_t bias) noexcept
{
	std::uint64_t key = generation;
	for (const std::uint64_t part : {std::uint64_t{library}, bias}) {
		key ^= part + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
		key *= 0xff51afd7ed558ccdULL;
		key ^= key >> 33;
	}
	return static_cast<std::size_t>(key % index_buckets);
}

Binder::Instance *Binder::find_instance(unsigned long generation,
	std::uint32_t library, std::uint64_t bias) noexcept
{
	for (auto at = heads_[bucket_of(generation, library, bias)]; at != 0;
	     at = instances_[at - 1].chain) {
		auto &candidate = instances_[at - 1];
		if (candidate.generation == generation &&
		    candidate.library == library && candidate.bias == bias)
			return &candidate;
	}
	return nullptr;
}

void Binder::unlink_instance(std::size_t index) noexcept
{
	auto &victim = instances_[index];
	auto *at = &heads_[bucket_of(victim.generation, victim.library,
		victim.bias)];
	while (*at != 0 && static_cast<std::size_t>(*at - 1) != index)
		at = &instances_[*at - 1].chain;
	if (*at != 0)
		*at = victim.chain;
	victim.chain = 0;
}

void Binder::link_instance(std::size_t index) noexcept
{
	auto &added = instances_[index];
	auto &head = heads_[bucket_of(added.generation, added.library,
		added.bias)];
	added.chain = head;
	head = static_cast<std::uint16_t>(index + 1);
}

bool Binder::changed_since(void *context, std::uint64_t sequence,
	std::uint64_t low, std::uint64_t high, std::uint64_t &current) noexcept
{
	if (context == nullptr)
		return true;
	UML_AOT_COUNT(history_queries, 1);
	darwin_uml_static_code_change ranges[32];
	const int count = darwin_uml_static_vm_execution_context_code_change_ranges(
		context, sequence, ranges, 32, &current);
	if (count < 0)
		return true;
	for (int index = 0; index < count; ++index) {
		/* A range of length zero names the whole address space. */
		if (ranges[index].length == 0)
			return true;
		if (ranges[index].address < high &&
		    ranges[index].address + ranges[index].length > low)
			return true;
	}
	return false;
}

Binder::Verdict Binder::verify_page(
	::uml::memory::StaticGuestMemory &memory, void *context,
	Instance &instance, std::uint64_t index) noexcept
{
	const auto library = directory().library(instance.library);
	const auto address =
		instance.bias + library.table_base + index * page_size;
	/* The bytes fetched below are the frame the stamps name only while the
	 * address space's mappings stand still, which the execution gate this
	 * runs inside keeps true; the sequence is read to check it, not to rely
	 * on it. It is read before the first stamp, as the stamps are before the
	 * fetch. */
	const auto mapping_sequence = [context]() noexcept -> std::uint64_t {
		return context == nullptr ? 0 :
			darwin_uml_static_vm_execution_context_sequences(context)
				.mapping_changes;
	};
	const auto sequence_at_start = mapping_sequence();
	/* Registers this generation as a holder of the page's frame, before any
	 * stamp is read and whichever way the page is then judged: a verdict of
	 * the table reads no byte of the page, so the registration that the
	 * fetch of a hash makes is not there for it. The word is consumed by a
	 * publication, so the registration is made at every verification, not
	 * once.
	 *
	 * The pairing: this registration is a compare-exchange on the frame's
	 * word followed by a full fence (StaticGuestMemory::register_holder_slow),
	 * and the stamp is read after both. A publisher steps the frame's stamp,
	 * issues a full fence and then reads the word (AddressSpace::step_stamp,
	 * before take_holders). Each side stores, fences and loads what the other
	 * stored (the store-buffering shape), so either the publisher's read of
	 * the word sees this registration and tells the generation, or the stamp
	 * read below shows the step. Both fences are dmb ish, whatever the host's
	 * code generation is for the loads and the exchanges around them
	 * (memory/mmu/code_holders.hpp).
	 *
	 * resolve() and the engine's own fetches of the page register the
	 * generation too, and a publication that makes a slot stale tells that
	 * generation, so no known interleaving leaves this call the only
	 * registration. It is kept so that the stamp read below never depends on
	 * a registration made elsewhere. The one-byte fetch makes the page's TLB
	 * slot a tagged holder word, so the stamp read that follows looks the
	 * page up again: two slow lookups per verification, not one. */
	{
		std::byte registered;
		if (!memory.fetch(address, std::span{&registered, 1}))
			return Verdict::unreadable;
	}
	/* Rewritten code runs only for a page whose mapping carries the kernel's
	 * immutable fact. A page without it can be rewritten through another
	 * mapping of the frame with no publication this address space sees, and
	 * rewritten code would go on running the bytes it was hashed as. A page
	 * that cannot be stamped is one nothing can be fetched from either. This
	 * stamp is also the one the verification holds before the fetch. */
	::uml::memory::ExtendedCodePageStamp admission;
	UML_AOT_COUNT(stamp_queries, 1);
	if (!memory.extended_code_page_stamp(address, nullptr, admission))
		return Verdict::unreadable;
	if (!admission.immutable) {
		UML_AOT_COUNT(mutable_pages, 1);
		UML_AOT_COUNT_LIBRARY(library_mutable_pages, instance.library);
		return Verdict::mutable_page;
	}
	[[maybe_unused]] bool admission_unused = true;
	const auto outcome = verify_through_table(g_verdicts,
		[&](std::uint64_t at, PageStamp &stamp) noexcept {
#if !UML_AOT_SHARED_VERDICTS
			(void)at;
			(void)stamp;
			return false;
#else
			/* The first stamp asked for is the one that admitted the page,
			 * read after the sequence above and before any fetch, which is
			 * where the stamp before the fetch is read; the second, after a
			 * hash that matched, is read afresh. */
			if (admission_unused && at == address) {
				admission_unused = false;
				stamp = {admission.stamp.identity, admission.epoch,
					admission.shareable};
				return true;
			}
			::uml::memory::ExtendedCodePageStamp seen;
			UML_AOT_COUNT(stamp_queries, 1);
			if (!memory.extended_code_page_stamp(at, nullptr, seen))
				return false;
			stamp = {seen.stamp.identity, seen.epoch, seen.shareable};
			return true;
#endif
		},
		[&]() noexcept {
			alignas(8) std::array<std::byte, page_size> page;
			if (!memory.fetch(address, page))
				return SharedOutcome::unreadable;
			UML_AOT_COUNT(verifications, 1);
			UML_AOT_COUNT_LIBRARY(library_verifications, instance.library);
			UML_AOT_SAMPLE_STACK_AT(page.data());
			/* The stored page is a SHA-256 of the bytes the code was built
			 * from: rewritten code runs for other bytes only on a collision
			 * of the hash. CC_SHA256 is CommonCrypto's one-shot digest,
			 * part of libSystem on macOS, iOS and the iOS Simulator, and
			 * runs on the SHA-2 instructions of the host. */
			std::array<std::uint8_t, page_hash_size> hash;
			CC_SHA256(page.data(), static_cast<CC_LONG>(page_size),
				hash.data());
			return std::memcmp(hash.data(), uml_aot_blob +
				library.page_hashes + index * page_hash_size,
				page_hash_size) == 0 ? SharedOutcome::match :
				SharedOutcome::mismatch;
		},
		[&]() noexcept { return mapping_sequence() != sequence_at_start; },
		address, VerdictTable::where(instance.library,
			static_cast<std::uint32_t>(index))
#if UML_AOT_DIAGNOSTICS
		, [&]() noexcept {
			UML_AOT_COUNT(epoch_rehashes, 1);
			UML_AOT_COUNT_LIBRARY(library_epoch_rehashes, instance.library);
		}
#endif
		);
	switch (outcome) {
	case SharedOutcome::unreadable:
		return Verdict::unreadable;
	case SharedOutcome::mismatch:
		return Verdict::mismatch;
	case SharedOutcome::shared_hit:
		UML_AOT_COUNT(shared_hits, 1);
		UML_AOT_COUNT_LIBRARY(library_shared_hits, instance.library);
		break;
	case SharedOutcome::match:
		break;
	}
	instance.pages[index] = 1;
	return Verdict::match;
}

void Binder::sync(void *context, unsigned long generation,
	std::uint64_t code_changes) noexcept
{
	if (sync_generation_ == generation && sync_changes_ == code_changes)
		return;
	darwin_uml_static_code_change ranges[32];
	std::uint64_t ranges_from = 0;
	int count = 0;
	bool have = false;
	for (std::size_t at = 0; at < used_; ++at) {
		auto &instance = instances_[at];
		if (!instance.used || instance.generation != generation ||
		    instance.poisoned || instance.state_changes == code_changes)
			continue;
		/* The history answers what was published after a sequence, up to
		 * the present, so a change newer than code_changes clears a page
		 * early and never late. A sequence from the future or a history
		 * that no longer reaches back clears everything. */
		const auto from = instance.state_changes;
		if (!have || ranges_from != from) {
			std::uint64_t current = 0;
			count = context == nullptr || from > code_changes ? -1 :
				darwin_uml_static_vm_execution_context_code_change_ranges(
					context, from, ranges, 32, &current);
			ranges_from = from;
			have = true;
			UML_AOT_COUNT(history_queries, 1);
		}
		const auto library = directory().library(instance.library);
		const std::uint64_t low = instance.bias + library.table_base;
		const std::uint64_t high = instance.bias + library.table_end;
		if (count < 0) {
			std::fill_n(instance.pages, instance.page_count, 0);
		} else {
			for (int index = 0; index < count; ++index) {
				std::uint64_t first = ranges[index].address;
				std::uint64_t last = first + ranges[index].length;
				/* A range of length zero names the whole address space. */
				if (ranges[index].length == 0) {
					first = low;
					last = high;
				}
				if (first >= high || last <= low)
					continue;
				first = std::max(first, low);
				last = std::min(last, high);
				for (auto page = (first - low) / page_size;
				     page < (last - low + page_size - 1) / page_size; ++page)
					instance.pages[page] = 0;
			}
		}
		instance.state_changes = code_changes;
	}
	sync_generation_ = generation;
	sync_changes_ = code_changes;
}

std::int32_t Binder::resolve(::uml::memory::StaticGuestMemory &memory,
	void *, unsigned long generation, std::uint64_t code_changes,
	std::uint64_t page_va) noexcept
{
	const auto &dir = directory();
	alignas(8) std::array<std::byte, page_size> page;
	if (!memory.fetch(page_va, page))
		return not_ready;
	UML_AOT_COUNT(pages_hashed, 1);
	const auto digest = page_digest(page.data());
	std::uint32_t slot = static_cast<std::uint32_t>(digest) & (dir.slots - 1);
	std::uint32_t library_index = 0;
	std::uint32_t page_index = 0;
	for (;;) {
		const auto at = dir.hash + 16 * static_cast<std::size_t>(slot);
		const auto stored = read<std::uint64_t>(at);
		if (stored == 0)
			return none;
		if (stored == digest) {
			library_index = read<std::uint32_t>(at + 8);
			page_index = read<std::uint32_t>(at + 12);
			break;
		}
		slot = (slot + 1) & (dir.slots - 1);
	}
	const auto library = dir.library(library_index);
	const std::uint64_t bias =
		page_va - (library.table_base + page_index * page_size);
	Instance *found = find_instance(generation, library_index, bias);
	if (found == nullptr) {
		/* The page states first: with none, nothing changes. */
		auto *const states = static_cast<std::uint8_t *>(
			std::calloc(library.pages != 0 ? library.pages : 1, 1));
		if (states == nullptr)
			return not_ready;
		const auto victim = next_instance_;
		next_instance_ = (next_instance_ + 1) % max_instances;
		note_instance(this, generation, library_index, bias,
			instances_[victim].used, instances_[victim].page_count,
			library.pages);
		if (instances_[victim].used) {
			unlink_instance(victim);
			/* No page slot may name the instance that takes this place;
			 * a place that never held one is named by none. */
			for (std::size_t at = 0; at < page_slots; ++at)
				if (pages_[at].page != 0 &&
				    pages_[at].instance == static_cast<std::int16_t>(victim)) {
					pages_[at] = PageSlot{};
#if UML_AOT_DIAGNOSTICS
					counters::page_slots_filled.fetch_sub(1,
						std::memory_order_relaxed);
#endif
				}
			std::free(instances_[victim].pages);
		}
		used_ = std::max(used_, victim + 1);
		found = &instances_[victim];
		*found = Instance{};
		found->generation = generation;
		found->library = library_index;
		found->bias = bias;
		found->used = true;
		found->pages = states;
		found->page_count = library.pages;
		found->state_changes = code_changes;
		link_instance(victim);
		++epoch_;
	}
	return found->poisoned ? none :
		static_cast<std::int32_t>(found - instances_);
}

Hit Binder::find(::uml::memory::StaticGuestMemory &memory, void *context,
	unsigned long generation, std::uint64_t code_changes,
	std::uint64_t pc) noexcept
{
	if (!directory().valid || (pc & 3U) != 0 || tables_ == nullptr)
		return {};
	++ticks_;
	UML_AOT_COUNT(page_lookups, 1);
#if UML_AOT_DIAGNOSTICS
	if (code_changes != last_changes_) {
		UML_AOT_COUNT(code_bumps, 1);
		last_changes_ = code_changes;
	}
#endif
	/* Every instance of the address space is current before any is used. */
	sync(context, generation, code_changes);
	const auto page_va = pc & ~(page_size - 1);
	auto &slot = pages_[((page_va >> 12) ^ generation) % page_slots];
	const auto tag = page_va | 1;
	const auto slot_generation = static_cast<std::uint32_t>(generation);
	bool fresh = slot.page == tag && slot.generation == slot_generation;
	if (fresh && slot.stamp != code_changes) {
		std::uint64_t current = 0;
		if (changed_since(context, slot.stamp, page_va, page_va + page_size,
			    current))
			fresh = false;
		else
			slot.stamp = code_changes;
	}
	if (!fresh) {
		UML_AOT_COUNT(binds, 1);
		const auto answer =
			resolve(memory, context, generation, code_changes, page_va);
		if (answer == not_ready) {
			note_find(FindResult::not_ready, no_library);
			return {};
		}
#if UML_AOT_DIAGNOSTICS
		if (slot.page == 0) {
			UML_AOT_COUNT(page_slot_fills, 1);
			UML_AOT_COUNT(page_slots_filled, 1);
		} else if (slot.page != tag || slot.generation != slot_generation) {
			UML_AOT_COUNT(page_slot_conflicts, 1);
		}
#endif
		/* The retry state is of the page, not of the slot. */
		slot = PageSlot{.page = tag, .stamp = code_changes,
			.generation = slot_generation,
			.instance = static_cast<std::int16_t>(answer)};
	}
	if (slot.instance < 0) {
		note_find(FindResult::uncovered, no_library);
		return {};
	}
	auto &instance = instances_[static_cast<std::size_t>(slot.instance)];
	if (!instance.used || instance.poisoned ||
	    instance.generation != generation) {
		note_find(instance.poisoned ? FindResult::poisoned :
			FindResult::stale, instance.poisoned ? instance.library :
			no_library);
#if UML_AOT_DIAGNOSTICS
		counters::page_slots_filled.fetch_sub(1, std::memory_order_relaxed);
#endif
		slot = PageSlot{};
		return {};
	}
	/* The page of `pc` must have matched its stored hash. */
	const auto record = directory().library(instance.library);
	const std::uint64_t page_index =
		(pc - instance.bias - record.table_base) / page_size;
	if (pc - instance.bias < record.table_base ||
	    page_index >= instance.page_count) {
		note_find(FindResult::no_entry, instance.library);
		return {};
	}
	if (instance.pages[page_index] == 0) {
		if (ticks_ < slot.retry_at) {
			note_find(slot.mutable_page ? FindResult::mutable_page :
				FindResult::unverified, instance.library);
			return {};
		}
#if UML_AOT_DIAGNOSTICS
		const auto began = std::chrono::steady_clock::now();
#endif
		const auto verdict = verify_page(memory, context, instance,
			page_index);
#if UML_AOT_DIAGNOSTICS
		counters::verify_nanos.fetch_add(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - began).count()),
			std::memory_order_relaxed);
#endif
		switch (verdict) {
		case Verdict::match:
			slot.backoff = 0;
			slot.mutable_page = false;
			break;
		case Verdict::unreadable:
			UML_AOT_COUNT(unreadable, 1);
			slot.backoff = std::min<std::uint8_t>(slot.backoff + 1, 11);
			slot.retry_at = ticks_ + (std::uint64_t{64} << (slot.backoff - 1));
			slot.mutable_page = false;
			note_find(FindResult::unverified, instance.library);
			return {};
		case Verdict::mutable_page:
			slot.backoff = std::min<std::uint8_t>(slot.backoff + 1, 11);
			slot.retry_at = ticks_ + (std::uint64_t{64} << (slot.backoff - 1));
			slot.mutable_page = true;
			note_find(FindResult::mutable_page, instance.library);
			return {};
		case Verdict::mismatch:
			instance.poisoned = true;
			UML_AOT_COUNT(poisoned, 1);
			++epoch_;
			note_find(FindResult::poisoned, instance.library);
			return {};
		}
	}
	const auto found =
		entry(static_cast<std::uint32_t>(slot.instance), pc);
	note_find(found.native != nullptr ? FindResult::hit :
		FindResult::no_entry, instance.library);
	return found;
}

Hit Binder::entry(std::uint32_t instance_index,
	std::uint64_t pc) const noexcept
{
	const auto &instance = instances_[instance_index];
	if (!instance.used || instance.poisoned)
		return {};
	const auto library = directory().library(instance.library);
	const std::uint64_t offset = pc - instance.bias;
	if (offset < library.table_base || offset >= library.table_end ||
	    (offset & 3U) != 0)
		return {};
	const auto word = (offset - library.table_base) / 4;
	const auto native = native_entry(library, word);
	if (native == 0)
		return {};
	return {uml_aot_blob + library.unit_offset + native, instance.bias,
		instance_index, instance.library, instance.pages};
}

const CrossTable &Binder::cross(unsigned long generation,
	std::uint64_t code_changes) noexcept
{
#if UML_AOT_DIAGNOSTICS
	g_cross_owner = this;
	g_cross_generation = generation;
#endif
	if (cross_generation_ == generation && cross_epoch_ == epoch_ &&
	    cross_changes_ == code_changes)
		return cross_;
	cross_.count = 0;
	for (std::size_t at = 0; at < used_; ++at) {
		const auto &instance = instances_[at];
		if (!instance.used || instance.poisoned ||
		    instance.generation != generation ||
		    instance.state_changes != code_changes)
			continue;
		/* The table holds the instances of one address space; one it has
		 * no room for is left to the engine, never written past its
		 * entries. */
		if (cross_.count >= CrossTable::capacity)
			break;
		const auto library = directory().library(instance.library);
		cross_.entries[cross_.count++] = {
			instance.bias + library.table_base,
			instance.bias + library.table_end, instance.bias,
			instance.pages, instance.library, 0};
	}
	std::sort(cross_.entries.begin(), cross_.entries.begin() +
		static_cast<std::ptrdiff_t>(cross_.count),
		[](const CrossTable::Entry &a, const CrossTable::Entry &b) {
			return a.low < b.low;
		});
	cross_generation_ = generation;
	cross_epoch_ = epoch_;
	cross_changes_ = code_changes;
	return cross_;
}

bool Binder::locate(unsigned long generation, std::uint64_t address,
	std::uint32_t &library, std::uint64_t &offset) const noexcept
{
	for (std::size_t at = 0; at < used_; ++at) {
		const auto &instance = instances_[at];
		if (!instance.used || instance.generation != generation)
			continue;
		const auto record = directory().library(instance.library);
		if (address >= instance.bias + record.table_base &&
		    address < instance.bias + record.table_end) {
			library = instance.library;
			offset = address - instance.bias;
			return true;
		}
	}
	return false;
}

EnterResult enter(CpuState &cpu, ::uml::memory::Tlb &tlb, const Hit &hit,
	std::uint64_t budget, const CrossTable &table) noexcept
{
	using Entry = EnterResult (*)(CpuState *, void *, std::uint64_t,
		const void *, std::uint64_t, const CrossTable *,
		const std::uint8_t *);
	/* The probe indexes the entries from the pointer the refill gets. */
	if (static_cast<const void *>(tlb.entries.data()) !=
	    static_cast<const void *>(&tlb))
		std::abort();
	const auto function = reinterpret_cast<Entry>(
		const_cast<std::uint8_t *>(uml_aot_blob) + directory().enter);
	UML_AOT_COUNT(enters, 1);
	UML_AOT_SAMPLE_STACK();
#if UML_AOT_DIAGNOSTICS
	if (hit.library < 64)
		counters::library_enters[hit.library].fetch_add(
			1, std::memory_order_relaxed);
#endif
	const auto result = function(&cpu, &tlb, hit.bias, hit.native, budget,
		&table, hit.pages);
	UML_AOT_COUNT(retired, result.retired);
#if UML_AOT_DIAGNOSTICS
	{
		auto seen = counters::max_retired.load(std::memory_order_relaxed);
		while (seen < result.retired &&
		       !counters::max_retired.compare_exchange_weak(
			       seen, result.retired, std::memory_order_relaxed)) {
		}
	}
	if (result.reason == Reason::step) {
		counters::steps.fetch_add(1, std::memory_order_relaxed);
		for (std::uint64_t index = 0; index < table.count; ++index) {
			const auto &entry = table.entries[index];
			if (cpu.pc >= entry.low && cpu.pc < entry.high) {
				if (entry.library < 64)
					counters::library_steps[entry.library].fetch_add(
						1, std::memory_order_relaxed);
				note_step((std::uint64_t{entry.library} << 40) |
					(cpu.pc - entry.bias));
				break;
			}
		}
	}
#endif
	return result;
}

} // namespace uml::cpu::arm64::aot
