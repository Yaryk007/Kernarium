// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"

#if DARWIN_UML_X86_64_GUEST

#include "mapped_backing.hpp"
#include "uml_darwin_static_mmu.h"
#include "writer_location.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace uml::memory {

struct GenerationState;

/* The frame a fill index word maps, as far as the accounting needs it. */
struct FrameRef final {
	MappedBacking *backing{};
	std::uint64_t offset{};
	bool writable{};
	/* `FrameAccounting::translatable` of the word's fields. */
	bool translatable{};
};

/* Per 4 KiB frame of a backing, the exact locations at which a generation's
 * fill index maps it writable. A translator that registers a frame needs
 * them to retire the write translations that may predate its registration.
 *
 * A location exists for exactly as long as the writable word it names: a
 * mapping primitive adds it before the word is installed (a location without
 * a word costs a spurious retire at most) and removes it after the word is
 * gone, with execution of the generation quiesced. The four primitives that
 * change a word are `commit_planned_pages`, `protect`, `unmap_locked`'s
 * erase and `clear_mappings`; each calls here.
 *
 * Every change of a frame's writers happens under that frame's stripe lock.
 * Nothing allocates or waits under it: what an addition may need (the index
 * leaf, and for a second writer the frame's code-page record with room for
 * the list) is made before the lock is taken.
 *
 * Compiled only for the x86-64 guest, the one guest whose translators see a
 * store through any alias of a frame at once.
 */
class FrameAccounting final {
public:
	static constexpr std::uint32_t maximum_alias =
		FrameAccountingDomain::maximum_alias;

	/* Whether a word with these fields can hold a translation: a guest-code
	 * page that grants execute. The one predicate of every site that adds or
	 * removes a holder (registration, `apply`, the walks, `protect`), so that
	 * a holder exists only where a translatable word does. */
	[[nodiscard]] static constexpr bool translatable(
		bool guest_code, bool executable) noexcept
	{
		return guest_code && executable;
	}

	/* Only a page an entered context can fill is a location: the fill never
	 * serves a page at or above the guest address limit. */
	[[nodiscard]] static constexpr bool tracks(std::uint64_t page) noexcept
	{
		return page < std::uint64_t{DARWIN_UML_GUEST_ADDRESS_LIMIT};
	}

	/* Adds `location` to the writers of the frame at `offset` unless it is
	 * already one; true if this call added it. Throws std::bad_alloc, with
	 * nothing changed, when memory for it cannot be had. A frame whose
	 * accounting is in error ("unknown") records nothing. */
	[[nodiscard]] static bool add_writer(MappedBacking &backing,
		std::uint64_t offset, const WriterLocation &location);
	/* Removes `location`. A removal that finds no such location is an
	 * accounting error: the frame becomes "unknown" and the error is
	 * counted. */
	static void remove_writer(MappedBacking &backing, std::uint64_t offset,
		const WriterLocation &location) noexcept;

	/* Reserves the writer location of a page that becomes `final`. For a
	 * holder of the generation's mapping lock; see WriterReservation. */
	static void reserve(GenerationState &state, std::uint64_t page,
		const FrameRef &final);
	/* Applies the change of one committed page from `before` to `after`:
	 * removes the writer location of `before` unless the page stays writable
	 * to the same frame, and its holder unless the page stays translatable to
	 * the same frame. The holder step does not depend on the page being
	 * writable: a read-execute page is the common holder. Called after the
	 * word changed and before the backing slot of `before` is given back.
	 * Allocates nothing. */
	static void apply(GenerationState &state, std::uint64_t page,
		const FrameRef &before, const FrameRef &after) noexcept;
	/* The locations of the fill index word `word` of `page`, which is going
	 * away: its writer location if it is writable, and its holder if it is
	 * translatable. */
	static void release_word(GenerationState &state, std::uint64_t page,
		std::uintptr_t word) noexcept;
	/* Only the writer location of `word`, for a word that stays but stops
	 * being writable (`protect`). */
	static void release_writer(GenerationState &state, std::uint64_t page,
		std::uintptr_t word) noexcept;
	/* Only the holder of `word`, for a word that stays but stops being
	 * translatable (`protect` without execute). */
	static void release_holder(GenerationState &state, std::uint64_t page,
		std::uintptr_t word) noexcept;

	/* The change of the primitive that reserved took effect or did not: the
	 * locations it added stay, or are taken back. Idempotent. */
	static void commit(GenerationState &state) noexcept;
	static void settle(GenerationState &state) noexcept;

	/* Removes every writer location and every holder of the generation, before
	 * its words go (`clear_mappings`), and moves the generation's flush
	 * serial, so that no translation recorded before the walk is taken for
	 * current. The bump follows the last holder removal and sits in this
	 * function, so every caller gets it. The generation stays usable. */
	static void walk(GenerationState &state) noexcept;
	/* The generation is gone: marks its state walked and gives back its alias
	 * id (retiring it for good if a holder still names it). Later
	 * reservations and registrations on it are refused. */
	static void finish(GenerationState &state) noexcept;
	/* The retire walk: the same for a state whose words stay as they are. */
	static void retire(GenerationState &state) noexcept;

	/* What the accounting has done in this process. The counters are always
	 * compiled in and move on cold paths only. `errors` and
	 * `nonempty_state_destructions` must stay 0. */
	struct Diagnostics final {
		/* Removals that found no such location. */
		std::uint64_t errors;
		/* States destroyed while still holding an alias id: their walk did
		 * not run. */
		std::uint64_t nonempty_state_destructions;
		std::uint64_t writer_additions;
		std::uint64_t writer_removals;
		/* Reservations taken back because their change did not complete. */
		std::uint64_t rollbacks;
		/* Times a reservation made room for more entries: a plan of N pages
		 * takes O(log N), never N. */
		std::uint64_t reservation_growths;
		std::uint64_t holder_additions;
		std::uint64_t holder_removals;
		/* Flush serial moves: one per walk. */
		std::uint64_t serial_bumps;
		/* Alias ids retired for good because a holder still named them when
		 * their state was walked for the last time; must stay 0. */
		std::uint64_t holder_leaks;
		/* Holder removals made while the generation could still run, other
		 * than the retire walk's; must stay 0. */
		std::uint64_t unquiesced_holder_removals;
		/* Holder removals for a change the code-change predicate does not
		 * name; must stay 0. */
		std::uint64_t unmarked_holder_removals;
		/* Registrations refused because the state was walked. */
		std::uint64_t walked_refusals;
		/* Registrations that failed (memory, an unknown frame, too much
		 * debt) and changed nothing. */
		std::uint64_t registration_failures;
		/* Publications of stores and kernel writes at the holders of a frame. */
		std::uint64_t publications;
		/* Kernel writes whose buffer named no frame. */
		std::uint64_t unresolved_kernel_writes;
		/* Direct ranges the table had no slot for. */
		std::uint64_t direct_range_drops;
		/* Holds answered `global`: the writers were too many for the
		 * buffer or not known by kind. */
		std::uint64_t global_holds;
		/* Kernel writes into guest frames (page cache, memfd or anonymous)
		 * that were named to the host
		 * (darwin_uml_vm_invalidate_code_frames()), whatever came of
		 * them. The kernel names only frames a guest-code mapping was
		 * queued for, so a guest that never maps code executable shows 0. */
		std::uint64_t kernel_frame_writes;
	};
	[[nodiscard]] static Diagnostics diagnostics() noexcept;
	static void note_nonempty_state_destruction() noexcept;

	enum class HoldResult : std::uint8_t {
		/* The holder stands and nothing is owed. */
		settled,
		/* The holder stands; `owed` locations were written to the caller's
		 * buffer, and their write translations are to be retired and the
		 * participants waited for. */
		owed,
		/* The page is not translatable here, or the state is gone. */
		refused,
		/* Nothing changed: memory ran out, or the frame has no record
		 * index. */
		failed,
		/* Nothing changed: the debt is `owed` locations, more than the
		 * caller's buffer holds. The caller asks again with room for them. */
		crowded,
		/* The holder stands, and the debt is every write translation of every
		 * participant: the frame's writers are not known (an accounting
		 * error), or the caller took the fallback for a debt it had no room
		 * for. `owed` is how many locations the debt names, if known. The
		 * caller retires them all and waits for every participant. */
		global,
	};
	using DebtLocation = darwin_uml_static_frame_location;
	struct HoldReport final {
		HoldResult result;
		/* The frame record's identity and the debt sequence the owed
		 * locations belong to, for clear_debt. */
		std::uint64_t record_identity;
		std::uint64_t debt_sequence;
		std::size_t owed;
		/* The frame's record and its holder change count as this call left
		 * them, under the frame's lock: a caller that read the same count
		 * later knows no holder was added since. Null when nothing stands. */
		const void *record;
		std::uint64_t alias_changes;
	};
	/* Registers the page as a holder of its frame before the caller reads a
	 * byte of it. For a holder of the generation's mapping lock shared, who
	 * keeps it for the whole call: the word is read once, and the record and
	 * the room the list needs are made before the stripe lock, never under
	 * it. A debt larger than `capacity` is reported as `crowded` unless
	 * `global_ok`, which registers the holder and reports `global`. */
	[[nodiscard]] static HoldReport hold(GenerationState &state,
		std::uint64_t page, DebtLocation *owed, std::size_t capacity,
		bool global_ok = false) noexcept;
	/* A completed retirement covers the debt up to `debt_sequence` of the
	 * record `record_identity`, if the page still maps that record. For a
	 * holder of the mapping lock shared. */
	static void clear_debt(GenerationState &state, std::uint64_t page,
		std::uint64_t record_identity, std::uint64_t debt_sequence) noexcept;

	/* What the write fill reads, without a lock: whether the frame the page
	 * maps has a holder other than the page, and the words whose change tells
	 * the fill to start over. */
	struct FillState final {
		const void *record;
		std::uint64_t alias_changes;
		bool other_holder;
		/* The frame, so that the fill can ask again whether the two words
		 * moved. */
		MappedBacking *backing;
		std::uint64_t offset;
	};
	[[nodiscard]] static FillState fill_state(const GenerationState &state,
		std::uint64_t page) noexcept;
	/* The same from the page's record handle, which the caller's own cache
	 * holds: no lookup, no lock. The record then cannot appear later, and
	 * only its change count tells the fill to start over. */
	[[nodiscard]] static FillState fill_state(const GenerationState &state,
		std::uint64_t page, const void *code_page) noexcept;
	/* Whether the frame still has the record and alias count `was` read. For
	 * a caller holding the generation's execution gate, with no lock. */
	[[nodiscard]] static bool fill_unchanged(const FillState &was) noexcept;
	/* What a store publication did. */
	struct Publication final {
		/* The frame has a holder other than the writer's page and the
		 * store was published at every holder. Otherwise nothing was
		 * done and the caller publishes as it always did. */
		bool handled;
		/* The generation's holder addresses did not fit the caller's
		 * buffer: the caller must cool everything. */
		bool overflow;
		/* The holder addresses in the writer's own generation, and how
		 * many ring records the call added there. */
		std::size_t own_count;
		std::uint64_t own_records;
	};
	/* Publishes the store of `length` bytes at `in_page` of the page the
	 * writer maps at `page`, after its bytes were written. For a holder of
	 * the mapping lock shared. */
	[[nodiscard]] static Publication publish_store(GenerationState &writer,
		std::uint64_t page, std::uint64_t in_page, std::uint64_t length,
		std::uint64_t *own, std::size_t capacity) noexcept;
	/* The holder addresses in the writer's own generation that a store of
	 * `length` bytes at `in_page` would publish at, before the store: the
	 * same answer as publish_store's, with nothing published. */
	[[nodiscard]] static Publication own_holders(GenerationState &writer,
		std::uint64_t page, std::uint64_t in_page, std::uint64_t length,
		std::uint64_t *own, std::size_t capacity) noexcept;
	/* The same for a write the kernel made through its own alias of the
	 * frame. */
	static void publish_kernel_write(MappedBacking &backing,
		std::uint64_t frame_offset, std::uint64_t in_page,
		std::uint64_t length) noexcept;
	/* The generation's flush serial; see walk. */
	[[nodiscard]] static std::uint64_t flush_serial(
		const GenerationState &state) noexcept;
	/* Moves the flush serial to a value no state has held. The walk does it
	 * after its last holder removal; a write that names no frame does it for
	 * every generation, because a stamp does not move for it. */
	static void bump_flush_serial(GenerationState &state) noexcept;
	/* The host ranges the kernel maps shared from a descriptor, so that a
	 * kernel write through its alias names its frame. */
	static void note_direct_map(const void *address, std::uint64_t length,
		int fd, std::uint64_t offset) noexcept;
	static void note_direct_unmap(const void *address,
		std::uint64_t length) noexcept;
	[[nodiscard]] static bool resolve_direct(const void *address, int &fd,
		std::uint64_t &offset) noexcept;
	static void note_unresolved_kernel_write() noexcept;
	static void note_kernel_frame_write() noexcept;
#if UML_MEMORY_ALLOCATION_TESTING
	/* The locations of the frame at `offset`; false when its accounting is in
	 * error. For a test that excludes every change. */
	[[nodiscard]] static bool locations_for_testing(MappedBacking &backing,
		std::uint64_t offset, std::vector<WriterLocation> &locations);
	/* The holders of the frame at `offset`, and the count the alias table
	 * keeps for `alias` in the backing's domain. */
	static void holders_for_testing(MappedBacking &backing,
		std::uint64_t offset, std::vector<WriterLocation> &holders);
	[[nodiscard]] static std::uint64_t alias_holders_for_testing(
		const MappedBacking &backing, std::uint32_t alias) noexcept;
#endif
};

/* The locations one mapping change added, so that a change that fails or is
 * abandoned takes back exactly those and no location that was already there.
 * Each primitive that changes words holds the generation's mapping lock
 * exclusively from its first reservation to its commit or settle, so the
 * object is used by one thread at a time. */
class WriterReservation final {
public:
	/* Adds `page`'s location at the frame, remembering it if this call added
	 * it. Throws std::bad_alloc, or when the state is walked. */
	void add(GenerationState &state, MappedBacking &backing,
		std::uint64_t offset, std::uint64_t page);
	/* The change took effect: what was added stays. */
	void commit() noexcept { release(); }
	/* The change did not: removes what this reservation added. */
	void settle(std::uint32_t alias) noexcept;

private:
	struct Added final {
		MappedBacking *backing;
		std::uint64_t offset;
		std::uint64_t page;
	};

	void release() noexcept;

	/* The plan's entries are temporaries of one operation on one thread, so
	 * they live in that thread's scratch arena, which this scope keeps from
	 * the first entry to the end of the plan. */
	std::optional<ScratchScope> scope_;
	Added *items_{};
	std::size_t size_{};
	std::size_t capacity_{};
};

/* Takes back the locations a primitive reserved if it did not commit them. */
class WriterSettler final {
public:
	explicit WriterSettler(GenerationState &state) noexcept : state_{state} {}
	~WriterSettler();
	WriterSettler(const WriterSettler &) = delete;
	WriterSettler &operator=(const WriterSettler &) = delete;

private:
	GenerationState &state_;
};

/* A generation state's part of the accounting. */
struct StateAccounting final {
	/* Holds the domain's alias table for as long as the state exists. */
	const std::shared_ptr<FrameAccountingDomain> domain;
	/* The id the state's locations name: allocated when the state is made, the
	 * same until it is destroyed, never 0. After `finish` the id is free for
	 * another state, and this state no longer uses it. */
	const std::uint32_t alias;
	/* Set once the generation is gone; a walked state refuses reservations
	 * and registrations. */
	bool walked{};
	/* Moves at every walk; see FrameAccounting::walk. Every value, the first
	 * included, comes from one process-wide 64-bit source, so no two states
	 * and no two moments of one state share one. */
	std::atomic_uint64_t flush_serial{0};
	/* A location may name the id: the state has reserved one. Registrations
	 * set it under the mapping lock held shared, so two may at once. */
	std::atomic_bool used{false};
	WriterReservation reservation;

	/* Throws std::bad_alloc, with nothing taken, when no id can be had. */
	StateAccounting(std::shared_ptr<FrameAccountingDomain> frame_domain,
		GenerationState &state, std::uint64_t generation);
	StateAccounting(const StateAccounting &) = delete;
	StateAccounting &operator=(const StateAccounting &) = delete;
	~StateAccounting();
};

} // namespace uml::memory

#if UML_MEMORY_ALLOCATION_TESTING
#include <memory>
#include <tuple>
#include <utility>

namespace uml::memory::testing {

/* What the writer accounting holds against what the generations' fill
 * indexes say, for the current domain. `equal` is true when every writable
 * word of every live state has exactly its location and no frame holds any
 * other. */
struct FrameWriterAudit final {
	std::size_t state_locations{};
	std::size_t frame_locations{};
	std::size_t unknown_frames{};
	bool equal{};
};

/* One writer location of one frame, for comparing two ways of counting. */
struct FrameWriterSite final {
	const MappedBacking *backing;
	std::uint64_t frame;
	std::uint32_t alias;
	std::uint64_t page;

	[[nodiscard]] bool operator<(const FrameWriterSite &other) const noexcept
	{
		return std::tie(backing, frame, alias, page) <
			std::tie(other.backing, other.frame, other.alias, other.page);
	}
	[[nodiscard]] bool operator==(const FrameWriterSite &) const noexcept = default;
};

[[nodiscard]] FrameWriterAudit audit_frame_writers() noexcept;
/* The live state of `generation` in the current domain; null when there is
 * none. The caller's reference outlives the generation. */
[[nodiscard]] std::shared_ptr<GenerationState> generation_state(
	std::uint64_t generation);
/* The backing that `fd` names in the current domain, if it has one. */
[[nodiscard]] std::shared_ptr<MappedBacking> mapped_backing(int fd);
/* {generation, page} of each writer of the frame at `offset` of the backing
 * `fd` names in the current domain. */
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>>
frame_writers(int fd, std::uint64_t offset);

} // namespace uml::memory::testing
#endif

#endif
