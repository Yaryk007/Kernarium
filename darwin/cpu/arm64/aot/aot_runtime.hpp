// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/aot_selection.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "memory/static_guest_memory.hpp"
#include "memory/tlb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

/* Ahead-of-time native code for the Android image's own arm64 libraries
 * (docs/architecture.md, "Ahead-of-time code"). The framework holds one
 * generated blob (tools/aot/rewrite.py): the entry routine, one unit of
 * rewritten code per library with its entry table and page digests. This
 * header is the whole boundary the gadget executor sees. Counters, reports
 * and test switches are in aot_diagnostics.hpp and exist only in builds that
 * set UML_AOT_DIAGNOSTICS. */
namespace uml::cpu::arm64::aot {

/* Whether AOT code may be entered. Decided by the select() of each run, before
 * its first executor, which reads the answer once when it is made; a process
 * whose launch selected nothing has it off (a diagnostics build, which tests
 * drive without a launch, takes the environment's answer: on unless
 * DARWIN_UML_AOT=0). Always off when the framework's blob is not valid. */
[[nodiscard]] bool enabled() noexcept;

/* The release the framework's blob was made from, as its header names it
 * (the ID points into the blob); empty when the blob is not valid. */
[[nodiscard]] std::optional<::uml::cpu::AotRelease> embedded_release() noexcept;

/* A launch's own word (--aot=) and the release it boots (null for a guest
 * that is not an Android release). The blob runs only with the release it
 * was made from: `automatic` is on (unless DARWIN_UML_AOT=0) only for that
 * release, and `on` fails for any other release or none. The decision is made
 * before any executor, so a launch the blob does not fit allocates nothing
 * of it. A run that starts again in the process (a guest restart) decides
 * again: a release the blob no longer fits is off, and a session that ran
 * without the component does not turn it on, so `automatic` runs on the
 * engine. False when it cannot be kept: `on` for a framework whose blob is not
 * valid, for a release it does not fit, or in a session that already runs
 * without it. */
[[nodiscard]] bool select(::uml::cpu::AotSelection selection,
	const ::uml::cpu::AotRelease *release) noexcept;

/* The end of a session: Android's clean power-off, whose guest was released to
 * the process, so that no executor of it exists. The next session's select()
 * chooses again, which a start without the component would otherwise forbid
 * for the rest of the process (the loader reuses it). A restart inside a
 * session is no end and keeps what its session chose. Nothing about a release
 * is remembered: select() validates it at every boot. */
void end_session() noexcept;

/* The session's choice as it stands, and putting it back: a start that took
 * the choice and then failed before its guest ran leaves the process as it
 * was. The value means nothing but to restore_choice(). */
[[nodiscard]] int choice() noexcept;
void restore_choice(int choice) noexcept;

/* Why an entry was refused, for the diagnostics build's counters. */
enum class Refusal : unsigned {
	tbi_disabled,
	sp_misaligned,
	exclusive_monitor,
	fp_environment,
	short_entry,
	count,
};
void note_refusal(Refusal reason) noexcept;
/* Where a diagnostics build's test hook runs: after the dispatcher read the
 * address space's sequences, before anything is looked up (nothing in a
 * production build). */
void note_run_start() noexcept;
/* A number that changes when a diagnostics build's tests switch the
 * component on or off: the engine forgets what it learned about instructions
 * that faulted and about short entries (always 0 in a production build). */
[[nodiscard]] std::uint64_t learning_epoch() noexcept;
/* Bracket the engine's execution of one instruction rewritten code left to
 * it (counted and timed in a diagnostics build only). */
[[nodiscard]] std::uint64_t step_begin() noexcept;
void step_end(std::uint64_t began) noexcept;

/* Why AOT code returned; the numbers are the generator's. */
enum class Reason : std::uint64_t {
	budget = 0,
	branch = 1,
	step = 2,
};

/* `retired` is the budget the entry used up. In the product layout charge
 * points pay for the run that follows them in advance, so it is not a count
 * of instructions (a single RET reports 33); a build rewritten with
 * UML_AOT_COUNT_RETIRED reports the exact count. */
struct EnterResult final {
	Reason reason;
	std::uint64_t retired;
};

/* One guest instruction the rewritten code can be entered at. `pages` is
 * the state of the text pages of the instance it is in: byte p is 1 once
 * text page p matched its stored hash (rewritten code reads it
 * where it leaves a page; the binder alone writes it, between entries). */
struct Hit final {
	const void *native{};
	std::uint64_t bias{};
	std::uint32_t instance{};
	std::uint32_t library{};
	const std::uint8_t *pages{};
};

/* A library instance rewritten code may branch to. The thunk generated for
 * a cross-library branch reads `bias` and `pages` (tools/aot/rewrite.py:
 * CROSS_BIAS, CROSS_PAGES). */
struct CrossEntry final {
	std::uint64_t low;
	std::uint64_t high;
	std::uint64_t bias;
	const std::uint8_t *pages;
	std::uint32_t library;
	std::uint32_t reserved;
};
static_assert(offsetof(CrossEntry, bias) == 16 &&
	offsetof(CrossEntry, pages) == 24);

/* The answer of a cross-library lookup: the native code of a guest address
 * in another library instance, and that instance's entry. */
struct Target final {
	const void *native;
	const CrossEntry *entry;
};

/* What rewritten code asks the host for, and the libraries it may branch to
 * without leaving: the instances of the entered address space. The first
 * three members are called by the generated refill, lookup and MOPS
 * thunks (tools/aot/rewrite.py: XTAB_*). */
struct CrossTable final {
	static constexpr std::size_t capacity = 64;
	using Entry = CrossEntry;
	/* Fill the TLB entry of an access: `address` is the address the probe
	 * formed (the last byte of an access of more than one byte) and `code`
	 * is 0 or 1 (read or write) | bytes << 8. Answers whether the entry now
	 * holds the page and the access fits in it. */
	std::uint64_t (*refill)(void *tlb, std::uint64_t address,
		std::uint64_t code) noexcept;
	Target (*lookup)(const CrossTable *table, std::uint64_t address) noexcept;
	/* The epilogue of a memory copy or set (FEAT_MOPS) when the thunk's own
	 * fast path does not apply. `registers` holds the destination, count and
	 * source (the fill value of a set) and the flags, in that order; the
	 * three are updated as the engine's epilogue does. `kind` is 0 for CPYF,
	 * 1 for CPY and 2 for SET. Answers 1 when the epilogue completed, 0 when
	 * it stopped at a fault (the registers are committed to the region the
	 * fault is in) and 2 when the registers cannot have come from the
	 * prologue. */
	std::uint64_t (*mops)(CpuState *cpu, ::uml::memory::Tlb *tlb,
		std::uint64_t *registers, std::uint64_t kind) noexcept;
	std::uint64_t count{};
	std::array<Entry, capacity> entries{};
};

/* The libraries an address space has mapped, identified by the digest of
 * their code pages. Each text page is hashed (SHA-256) and the hash compared
 * with the one stored for the page before any rewritten instruction of it
 * runs (when find() is asked for an entry in it, and when rewritten code
 * leaves another page for it: it tests the page's state byte), and again
 * after the address space's code-change history names it. An instance with a
 * page whose hash differs is never entered again in that address space
 * generation; a page that cannot be read yet is tried again later. One
 * binder belongs to one executor thread. */
class Binder final {
public:
	Binder() noexcept;
	~Binder();
	Binder(const Binder &) = delete;
	Binder &operator=(const Binder &) = delete;

	/* The AOT entry for `pc`, or an empty Hit. `context` is the executor's
	 * execution context (it names the code-change history); `code_changes`
	 * is the sequence the dispatcher read. */
	[[nodiscard]] Hit find(::uml::memory::StaticGuestMemory &memory,
		void *context, unsigned long generation, std::uint64_t code_changes,
		std::uint64_t pc) noexcept;
	/* The entry for `pc` in an instance find() returned, or an empty Hit. */
	[[nodiscard]] Hit entry(std::uint32_t instance,
		std::uint64_t pc) const noexcept;
	/* The instances rewritten code may branch to: those usable in
	 * `generation`, current as of the sequence find() was given (call it
	 * after find()). */
	[[nodiscard]] const CrossTable &cross(unsigned long generation,
		std::uint64_t code_changes) noexcept;
	/* The library and offset a guest address belongs to, for reports. */
	[[nodiscard]] bool locate(unsigned long generation, std::uint64_t address,
		std::uint32_t &library, std::uint64_t &offset) const noexcept;
	/* A diagnostics build's measure of the tables (instances, index and page
	 * slots): their size, and how much of it the host holds in memory. */
	[[nodiscard]] std::size_t tables_bytes() const noexcept;
	[[nodiscard]] std::size_t tables_resident_bytes() const noexcept;

private:
	/* A library of an address space at a load bias. A place that has never
	 * held one is all zero bytes. */
	struct Instance final {
		unsigned long generation;
		std::uint64_t bias;
		std::uint64_t state_changes;
		/* `page_count` bytes, one per text page: 1 = its mapping carried
		 * the kernel's immutable fact, it matched its stored hash and no
		 * code change names it since (a mapping that loses the fact is
		 * one); as of `state_changes`. Rewritten code reads them. */
		std::uint8_t *pages;
		std::uint32_t library;
		std::uint32_t page_count;
		/* The next instance of the same hash bucket, plus one (0: none). */
		std::uint16_t chain;
		bool poisoned;
		bool used;
	};
	static_assert(std::is_trivial_v<Instance> && sizeof(Instance) == 48);
	/* What is known of a text page of an address space: the instance it is
	 * in and the code-change sequence that was current when it was bound. A
	 * slot of all zero bytes is empty. */
	struct PageSlot final {
		/* The page's address with bit 0 set. */
		std::uint64_t page{};
		std::uint64_t stamp{};
		/* An unreadable page, or a page of a mapping without the kernel's
		 * immutable fact, is not tried again before this many lookups. */
		std::uint64_t retry_at{};
		/* The low 32 bits of the address space's generation. A kernel
		 * generation is a 32-bit counter (memory/mmu/code_holders.hpp);
		 * the instance a slot names holds, and is checked for, the whole
		 * value. */
		std::uint32_t generation{};
		/* The instance the page is in, or -1: not in a library. */
		std::int16_t instance{-1};
		/* The wait before a retry doubles from 64 lookups to 65536: 0 is
		 * none, n is 64 << (n - 1). */
		std::uint8_t backoff{};
		/* The retry waits for a mapping that lacks the fact. */
		bool mutable_page{};
	};
	static_assert(sizeof(PageSlot) == 32);

	/* resolve() answers an instance index, none (not a library page, or a
	 * poisoned instance: remembered), or not_ready (do not remember). */
	static constexpr std::int32_t none = -1;
	static constexpr std::int32_t not_ready = -2;
	/* `mutable_page`: the mapping does not carry the kernel's immutable fact,
	 * so nothing says the bytes stay what they were hashed as. */
	enum class Verdict { match, unreadable, mutable_page, mismatch };
	[[nodiscard]] std::int32_t resolve(
		::uml::memory::StaticGuestMemory &memory, void *context,
		unsigned long generation, std::uint64_t code_changes,
		std::uint64_t page) noexcept;
	/* Bring the page states of every instance of `generation` up to
	 * `code_changes`: pages the code-change history names since an
	 * instance's last sequence are cleared. */
	void sync(void *context, unsigned long generation,
		std::uint64_t code_changes) noexcept;
	/* Hash text page `index` of the instance and compare it with the stored
	 * hash. */
	[[nodiscard]] Verdict verify_page(
		::uml::memory::StaticGuestMemory &memory, void *context,
		Instance &instance, std::uint64_t index) noexcept;
	/* Whether a code change after `sequence` touches [low, high). */
	[[nodiscard]] static bool changed_since(void *context,
		std::uint64_t sequence, std::uint64_t low, std::uint64_t high,
		std::uint64_t &current) noexcept;

	[[nodiscard]] static std::size_t bucket_of(unsigned long generation,
		std::uint32_t library, std::uint64_t bias) noexcept;
	[[nodiscard]] Instance *find_instance(unsigned long generation,
		std::uint32_t library, std::uint64_t bias) noexcept;
	void unlink_instance(std::size_t index) noexcept;
	void link_instance(std::size_t index) noexcept;
	/* The tables are one mapping of fresh pages: the instances, the heads of
	 * the index's hash chains (an instance's place plus one; 0: none) and
	 * the page slots, each from pages of its own. All zero bytes is the
	 * empty state of each, so the host backs the pages a binder has used
	 * and no more, and the capacity costs memory only as it is reached. A
	 * mapping the host refuses leaves the binder without tables: it finds
	 * nothing and the engine runs everything. */
	void *tables_{};
	std::size_t tables_size_{};
	Instance *instances_{};
	std::uint16_t *heads_{};
	PageSlot *pages_{};
	/* Places [0, used_) of `instances_` have held an instance: the ones
	 * that are searched. */
	std::size_t used_{};
	/* The place the next instance takes, first in, first out. */
	std::size_t next_instance_{};
	std::uint64_t ticks_{};
	/* The last (generation, sequence) sync() brought the instances to. */
	unsigned long sync_generation_{};
	std::uint64_t sync_changes_{};
	/* The last code-change sequence find() was given (a diagnostics build
	 * counts how often it moves). The layout is the same in both
	 * compilations of the runtime, which share the executor's object. */
	[[maybe_unused]] std::uint64_t last_changes_{};
	/* Moves whenever the set of usable instances changes. */
	std::uint64_t epoch_{1};
	/* The instances of one address space (at most a library each, a few more
	 * where a library is mapped twice): one the table has no room for is
	 * left to the engine, never written past it. */
	CrossTable cross_{};
	unsigned long cross_generation_{};
	std::uint64_t cross_epoch_{};
	std::uint64_t cross_changes_{};
};

/* Run rewritten code from `hit` until it leaves. `budget` bounds the
 * instructions it may retire before it returns. The guest registers, vectors
 * and flags are committed in `cpu` on every return. */
[[nodiscard]] EnterResult enter(CpuState &cpu, ::uml::memory::Tlb &tlb,
	const Hit &hit, std::uint64_t budget, const CrossTable &table) noexcept;

} // namespace uml::cpu::arm64::aot
