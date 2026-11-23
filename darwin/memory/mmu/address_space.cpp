// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_static_mmu.h"
#include "uml_darwin_guest.h"
#include "../domain_contract.hpp"
#include "../process_singleton.hpp"
#include "address_layout.hpp"
#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "../count_audit.hpp"
#include "bridge_call.hpp"
#include "code_holders.hpp"
#include "domain_registry_testing.hpp"
#include "execution_gate.hpp"
#include "frame_accounting.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <shared_mutex>
#include <span>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace uml::memory {
namespace {

constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
using AddressLayout = ConfiguredAddressLayout;

struct BackingKey final {
	std::uint64_t device;
	std::uint64_t inode;

	bool operator==(const BackingKey &) const = default;
};

struct BackingKeyHash final {
	[[nodiscard]] std::size_t operator()(const BackingKey &key) const noexcept
	{
		auto value = key.inode ^ (key.device * 0x9e37'79b9'7f4a'7c15ULL);
		value ^= value >> 33;
		return static_cast<std::size_t>(value);
	}
};

/* One non-executable shared mapping covers the complete UML physical-memory
 * file.  Besides avoiding a host mapping per guest page, this is the
 * invariant that lets a TLB hit become pointer arithmetic.
 */
MappingBatch &mapping_batch()
{
	static thread_local MappingBatch batch;
	return batch;
}

void reset_mapping_batch() noexcept
{
	auto &batch = mapping_batch();
	batch.operations.clear();
	batch.backings.clear();
	batch.address_space = nullptr;
	batch.depth = 0;
	batch.generation = 0;
	batch.invalidation_epoch = 0;
	batch.state.reset();
}

ActiveWalker &active_walker()
{
	static thread_local ActiveWalker active;
	return active;
}

constexpr std::size_t maximum_mapping_history = 4096;

/* The protection bits a mapping keeps. */
constexpr int protection_mask = PROT_READ | PROT_WRITE | PROT_EXEC;

/* What a generation maps at one page, as its fill index word and backing slot
 * say. The backing is the slot's, so the record is good for as long as the
 * caller holds the generation's lock. */
struct PageMapping final {
	MappedBacking *backing;
	std::uint64_t offset;
	int fd;
	int protection;
	bool guest_code;
	bool guest_writable;
	/* The kernel's immutable fact for this mapping (BackingSlots keeps it in
	 * the slot's key, since a word has no bit for it). */
	bool immutable;
};

[[nodiscard]] int protection_of(const FillIndex::Fields &fields) noexcept
{
	return (fields.read ? PROT_READ : 0) | (fields.write ? PROT_WRITE : 0) |
		(fields.exec ? PROT_EXEC : 0);
}

/* The record of a page whose word is `word` (0: none), or nothing when the
 * page is absent. */
[[nodiscard]] std::optional<PageMapping> decode_page(
	const GenerationState &state, std::uint64_t page,
	std::uintptr_t word) noexcept
{
	if (word == 0)
		return std::nullopt;
	const auto fields = FillIndex::decode(word);
	const auto entry = state.backings.entry(fields.slot, page);
	if (entry.backing == nullptr)
		return std::nullopt;
	return PageMapping{entry.backing,
		fields.host - reinterpret_cast<std::uintptr_t>(entry.backing->base()),
		entry.fd, protection_of(fields), fields.guest_code,
		fields.guest_writable, entry.immutable};
}

/* What the generation maps at `page`. For a holder of its lock. */
[[nodiscard]] std::optional<PageMapping> find_page(
	const GenerationState &state, std::uint64_t page) noexcept
{
	return decode_page(state, page, state.fill_index.lookup(page));
}

/* Whether a translator can hold code for the page: it fetches only from a
 * guest-code page that grants execute. */
[[nodiscard]] bool can_hold_translation(const PageMapping &mapping) noexcept
{
#if DARWIN_UML_X86_64_GUEST
	/* The one predicate of every holder site (frame_accounting.hpp). */
	return FrameAccounting::translatable(
		mapping.guest_code, (mapping.protection & PROT_EXEC) != 0);
#else
	return mapping.guest_code && (mapping.protection & PROT_EXEC) != 0;
#endif
}

/* The stamp of a record, as the bridge reports it. */
[[nodiscard]] bool load_stamp(const void *handle,
	darwin_uml_static_code_page_stamp &stamp) noexcept
{
	MappedBacking::RecordStamp record;
	if (!MappedBacking::load_code_page_stamp(handle, record))
		return false;
	stamp.identity = record.identity;
	stamp.version = record.version;
	stamp.epoch = record.epoch;
	/* A record that can no longer count exactly shares no verdict, and nor
	 * does any record once the accounting has been seen to fail. */
	stamp.flags = record.never_share || sharing_latched_off() ?
		DARWIN_UML_STATIC_STAMP_NEVER_SHARE : 0;
	return true;
}

#if DARWIN_UML_X86_64_GUEST
/* The frame a page mapping names, for the writer accounting. */
[[nodiscard]] FrameRef frame_of(
	const std::optional<PageMapping> &mapping) noexcept
{
	if (!mapping)
		return {};
	return {mapping->backing, mapping->offset,
		(mapping->protection & PROT_WRITE) != 0,
		can_hold_translation(*mapping)};
}
#endif

/* Raised once for the process, before the first x86 translation exists. x86
 * code sees every store at once, the kernel's included, so a kernel write into
 * a guest-code page must publish a code change itself. arm64 and AArch32 code
 * see such a write only after their own IC IVAU or cacheflush(), which
 * publishes it, and a process that runs nothing else does not pay for a
 * publication per uaccess. */
std::atomic_bool &coherent_kernel_code_writes()
{
	static std::atomic_bool required{};
	return required;
}

/* Whether a kernel write just copied into a guest-code page must publish.
 * The fence pairs with the one after the flag is raised: either this load
 * sees the flag, or the first x86 translator fetches the bytes just written. */
[[nodiscard]] bool kernel_code_write_publishes() noexcept
{
	std::atomic_thread_fence(std::memory_order_seq_cst);
	return coherent_kernel_code_writes().load(std::memory_order_relaxed);
}

/* Whether replacing `before` with `after` can leave a translation of the
 * page wrong. A translation exists only for a page that could hold one, so
 * a page that could not changes no code, whatever it becomes. One that
 * could changes code when it stops being able to or when its bytes may
 * differ, which is another backing or another place in it; a change of
 * protection or of who may write that keeps execute and the bytes changes
 * none. A store into the page publishes for itself (a translation's page is
 * in the code-page registry); a frame's other writers reach its other
 * holders through the frame's holder word for arm64 holders
 * (code_holders.hpp), and through the frame accounting for x86 holders, where
 * a tagged store publishes at each (frame_accounting.hpp), not here. */
[[nodiscard]] bool changes_translatable_code(
	const std::optional<PageMapping> &before,
	const std::optional<PageMapping> &after) noexcept
{
	if (!before || !can_hold_translation(*before))
		return false;
	if (!after || !can_hold_translation(*after))
		return true;
	return before->backing != after->backing ||
		before->offset != after->offset;
}

/* Whether the page keeps its place and its execute right but stops carrying
 * the kernel's immutable fact. What a consumer kept about its bytes on the
 * strength of the fact (rewritten code, bound to the page by a hash) no
 * longer holds, since another mapping of the frame can change them with no
 * word to this address space. */
[[nodiscard]] bool revokes_immutable_fact(
	const std::optional<PageMapping> &before,
	const std::optional<PageMapping> &after) noexcept
{
	return before && after && can_hold_translation(*before) &&
		can_hold_translation(*after) && before->immutable &&
		!after->immutable;
}

/* The pages of one mapping change whose code changes, as the smallest span
 * covering them, or the whole address space. */
struct CodeSpan final {
	std::uint64_t first{~std::uint64_t{0}};
	std::uint64_t end{};
	bool whole{};

	void add(std::uint64_t page) noexcept
	{
		first = std::min(first, page);
		end = std::max(end, page + page_size);
	}

	[[nodiscard]] bool empty() const noexcept
	{
		return !whole && end <= first;
	}
};

/* Notes that the page at `address`, which was `before` and becomes `after`,
 * changes code, when it does. A page stamp names the bytes a translation was
 * validated against, and the stores that move it are those a translator sees
 * land on a page it holds -- a page mapped without execute, or reached by
 * another alias, is not one. So a page leaving the translatable state moves
 * its own stamp as it is published: whatever later returns to it compares
 * its saved source instead of trusting a stamp that nothing could move. The
 * stamp exists only once something has translated the page, and a page
 * nothing translated has none to move. */
void note_code_change(CodeSpan &code, std::uint64_t address,
	const std::optional<PageMapping> &before,
	const std::optional<PageMapping> &after) noexcept
{
	if (!changes_translatable_code(before, after)) {
		/* Translations of the page are right still (the bytes are the
		 * same), so its stamp stays: the change is published for what
		 * relied on the fact. */
		if (revokes_immutable_fact(before, after))
			code.add(address);
		return;
	}
	code.add(address);
	before->backing->retire_code_page(before->offset);
}

struct MappingChangePlan final {
	std::uint64_t address;
	std::uint64_t length;
	CodeSpan code;
};

class PendingMappingChanges final {
public:
	PendingMappingChanges(GenerationState &state,
		std::span<const MappingChangePlan> changes)
		: state_{state}
	{
		const auto first = state.mapping_changes.load(
			std::memory_order_acquire) + 1;
		try {
			for (std::size_t index = 0; index < changes.size(); ++index) {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::history);
				state.mapping_history.push_back({first + index,
					changes[index].address, changes[index].length});
				++added_;
				const auto &code = changes[index].code;
				if (code.empty())
					continue;
				/* A whole-space change names the range {0, 0}. */
				if (code_changes_ < code_ranges_.size())
					code_ranges_[code_changes_] = code.whole ?
						CodeChangeRange{0, 0} :
						CodeChangeRange{code.first,
							code.end - code.first};
				++code_changes_;
			}
		} catch (...) {
			rollback_history();
			throw;
		}
	}

	~PendingMappingChanges()
	{
		if (!committed_)
			rollback_history();
	}

	PendingMappingChanges(const PendingMappingChanges &) = delete;
	PendingMappingChanges &operator=(const PendingMappingChanges &) = delete;

	void commit() noexcept
	{
		if (added_ != 0)
			state_.mapping_changes.fetch_add(
				added_, std::memory_order_release);
		if (code_changes_ > code_ranges_.size())
			publish_untracked_code_changes(state_, code_changes_);
		else
			publish_code_changes(state_,
				{code_ranges_.data(), code_changes_});
		while (state_.mapping_history.size() > maximum_mapping_history)
			state_.mapping_history.pop_front();
		committed_ = true;
	}

private:
	void rollback_history() noexcept
	{
		while (added_ != 0) {
			state_.mapping_history.pop_back();
			--added_;
		}
	}

	GenerationState &state_;
	/* One batch rarely changes code in more than a few of its ranges; a wider
	 * one is published without a history rather than allocating here, where a
	 * throw would have to unwind a partly recorded mapping change. */
	std::array<CodeChangeRange, 16> code_ranges_{};
	std::size_t added_{};
	std::size_t code_changes_{};
	bool committed_{};
};

[[nodiscard]] bool same_mapping(
	const PageMapping &left, const PageMapping &right) noexcept
{
	return left.backing == right.backing && left.offset == right.offset &&
		left.fd == right.fd && left.protection == right.protection &&
		left.guest_code == right.guest_code &&
		left.guest_writable == right.guest_writable &&
		left.immutable == right.immutable;
}

[[nodiscard]] bool same_mapping(
	const std::optional<PageMapping> &left,
	const std::optional<PageMapping> &right) noexcept
{
	return left.has_value() == right.has_value() &&
		(!left || same_mapping(*left, *right));
}

/* The fill index word of a mapped page that lies in backing slot `slot`: its
 * host address and what the guest may do there, as translate_in() judges it.
 * map() has checked that the address is one a word can hold. */
[[nodiscard]] std::uintptr_t fill_word(
	const PageMapping &mapping, unsigned slot) noexcept
{
	return FillIndex::encode({
		reinterpret_cast<std::uintptr_t>(
			mapping.backing->at(mapping.offset)),
		(mapping.protection & PROT_READ) != 0,
		(mapping.protection & PROT_WRITE) != 0,
		(mapping.protection & PROT_EXEC) != 0,
		mapping.guest_code, mapping.guest_writable, slot});
}

/* A departing word: counts one executable mapping out of its record when
 * the word is one a translator can hold code for. For a caller that has not
 * yet let go of the word's backing. */
void count_executable_departure(const GenerationState &state,
	std::uint64_t page, std::uintptr_t word) noexcept
{
	const auto fields = FillIndex::decode(word);
	if (!fields.guest_code || !fields.exec)
		return;
	const auto mapping = decode_page(state, page, word);
	auto *record = mapping ?
		mapping->backing->find_code_page(mapping->offset) : nullptr;
	if (record != nullptr)
		MappedBacking::executable_mapping_removed(record);
	else
		UML_MEMORY_COUNT_ANOMALY();
}

struct PlannedMappingPage final {
	std::uint64_t address;
	std::optional<PageMapping> original;
	std::optional<PageMapping> final;
	/* Who owns `final`'s backing: the operation that maps it. */
	const std::shared_ptr<MappedBacking> *owner;
	/* The backing slot `final` lies in, which the plan claimed. */
	unsigned slot;
	/* The record of the physical page `final` maps when a translator can
	 * hold code for it, made by the reserve step so that counting it in
	 * cannot fail. */
	void *record{};
};

/* The step of a mapping change that can fail: the nodes, and the overflow
 * room, that every page the change maps will live in. A page's word is
 * committed only once all of them exist, so a change that fails here has
 * changed nothing but the empty nodes it made. */
void reserve_planned_pages(GenerationState &state,
	std::span<PlannedMappingPage> pages)
{
	for (auto &page : pages) {
		if (!page.final || same_mapping(page.original, page.final))
			continue;
		UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::reserve);
		if (!state.fill_index.reserve(page.address))
			throw std::bad_alloc{};
		if (page.slot == BackingSlots::overflow)
			state.backings.reserve_overflow(page.address);
		if (can_hold_translation(*page.final))
			page.record = page.final->backing->obtain_code_page(
				page.final->offset);
#if DARWIN_UML_X86_64_GUEST
		FrameAccounting::reserve(state, page.address, frame_of(page.final));
#endif
	}
}

/* The executable-mapping count of the records a page's change touches: one
 * more on the record of the page it becomes when a translator can hold code
 * for it, one fewer on the record of the page it was. A page that stays on
 * one record (the protection or the immutable fact moved, the bytes did not)
 * changes nothing, so the count never passes through zero for it. The record
 * of the old page exists, since counting it in made it; it is found before
 * the plan lets go of the old backing. */
void count_executable_change(const PlannedMappingPage &page) noexcept
{
	const bool had = page.original && can_hold_translation(*page.original);
	const bool has = page.final && can_hold_translation(*page.final);
	if (had && has && page.original->backing == page.final->backing &&
	    page.original->offset == page.final->offset)
		return;
	if (has)
		MappedBacking::executable_mapping_added(page.record);
	if (had) {
		if (auto *record = page.original->backing->find_code_page(
			    page.original->offset);
		    record != nullptr)
			MappedBacking::executable_mapping_removed(record);
		else
			UML_MEMORY_COUNT_ANOMALY();
	}
}

/* Writes the planned pages' words, counts them in and out of their slots, and
 * frees the leaves a removal emptied. The reserve step made room for all of
 * it, so nothing here allocates or fails. A removal is for the caller to have
 * quiesced execution for. */
void commit_planned_pages(GenerationState &state,
	std::span<const PlannedMappingPage> pages) noexcept
{
	for (const auto &page : pages) {
		if (same_mapping(page.original, page.final))
			continue;
		count_executable_change(page);
		const auto old_word = state.fill_index.lookup(page.address);
		const bool had = old_word != 0;
		const auto old_slot = FillIndex::decode(old_word).slot;
#if DARWIN_UML_X86_64_GUEST
		/* A page of the overflow table that maps another backing lets go of
		 * the old one when its entry is overwritten; the accounting below
		 * still takes the old frame's location out of it. */
		std::shared_ptr<MappedBacking> retained;
		if (had && page.final && old_slot == BackingSlots::overflow &&
		    page.slot == BackingSlots::overflow)
			retained = state.backings.owner(old_slot, page.address);
#endif
		if (page.final) {
			state.backings.add(page.slot, page.address, *page.owner,
				page.final->fd, page.final->immutable);
			state.fill_index.store(page.address,
				fill_word(*page.final, page.slot));
			if (!had)
				++state.mapped_pages;
		} else {
			state.fill_index.store(page.address, 0);
			if (had)
				--state.mapped_pages;
		}
#if DARWIN_UML_X86_64_GUEST
		FrameAccounting::apply(state, page.address,
			frame_of(page.original), frame_of(page.final));
#endif
		/* A page that stays in the overflow table was just given its new
		 * entry; dropping the old one would drop that. */
		if (had && !(page.final && old_slot == BackingSlots::overflow &&
			     page.slot == BackingSlots::overflow))
			state.backings.remove(old_slot, page.address);
	}
	auto trimmed = ~std::uint64_t{0};
	for (const auto &page : pages) {
		if (page.final || !page.original ||
		    FillIndex::leaf_of(page.address) == trimmed)
			continue;
		trimmed = FillIndex::leaf_of(page.address);
		state.fill_index.trim(page.address);
	}
#if DARWIN_UML_X86_64_GUEST
	FrameAccounting::commit(state);
#endif
}

/* Empties a generation of its pages. */
void clear_mappings(GenerationState &state) noexcept
{
	/* Every executable mapping departs with the generation's words. */
	(void)state.fill_index.scan(0, std::numeric_limits<std::uint64_t>::max(),
		[&](std::uint64_t page, std::uintptr_t word) {
			count_executable_departure(state, page, word);
			return false;
		});
#if DARWIN_UML_X86_64_GUEST
	FrameAccounting::walk(state);
#endif
	state.fill_index.clear();
	state.backings.clear();
	state.mapped_pages = 0;
}

/* Releases the backing slots a change claimed, and the overflow room it
 * reserved, once the change is committed or abandoned. */
class SlotSettler final {
public:
	explicit SlotSettler(GenerationState &state) noexcept : state_{state} {}
	~SlotSettler()
	{
		state_.backings.settle();
#if DARWIN_UML_X86_64_GUEST
		FrameAccounting::settle(state_);
#endif
	}
	SlotSettler(const SlotSettler &) = delete;
	SlotSettler &operator=(const SlotSettler &) = delete;

private:
	GenerationState &state_;
};

ActiveGeneration &active_thread_generation()
{
	static thread_local ActiveGeneration active;
	return active;
}

/* The execution context this thread has entered, innermost first, or null.
 * The thread's active generation is what the kernel last selected for its
 * mapping-management calls, and a vCPU thread that ran another process's task
 * since may hold that process's. A guest instruction that reaches the address
 * space without naming a generation (IC IVAU) belongs to the generation being
 * run, which is the entered context's. Only an entered context is ever here:
 * begin links it and end, or its destruction while entered, unlinks it. */
ExecutionContext *&entered_thread_context() noexcept
{
	static thread_local ExecutionContext *entered{};
	return entered;
}

class AddressSpace;

std::atomic_uint64_t &domain_retirement_sequence_storage()
{
	static std::atomic_uint64_t sequence{};
	return sequence;
}

std::atomic_uint64_t &lifetime_retirement_epoch()
{
	static std::atomic_uint64_t epoch{};
	return epoch;
}

/* Generations destroyed in this process, in any domain: what a holder of
 * per-generation state loads to learn that some generation may have died
 * since it last asked generation_live(). Every executor loads it at each
 * entry into translated code, so it has a cache line of its own: a neighbour
 * another thread writes would make each of those loads a miss. */
std::atomic_uint64_t &generation_retirement_sequence()
{
	struct alignas(128) Sequence final {
		std::atomic_uint64_t value{};
	};
	static Sequence sequence;
	return sequence.value;
}

struct ResolvedAddressSpace final {
	std::uint64_t domain{};
	std::uint64_t retirement_sequence{};
	std::shared_ptr<AddressSpace> owner;
};

/* Everything address_space() needs from its thread, behind one thread-local
 * lookup: every resolution is a TLB miss or a mapping call on a vCPU thread.
 *
 * pruned_epoch is the lifetime-retirement epoch at which this thread last
 * found none of its bindings retired. AddressSpace::retire() is the only
 * writer of the lifetime flag that pruning tests, and it advances the epoch
 * after setting it, so a thread that reads an unchanged epoch has nothing to
 * prune.
 *
 * borrowed_accesses counts the AddressSpaceAccess objects on this thread that
 * borrow resolved.owner instead of holding their own reference. While any is
 * alive, resolution does not replace resolved, so the borrowed space outlives
 * every access to it without a shared reference-count round trip, which is
 * contended by every vCPU of one guest process. */
struct ThreadAddressSpaceCache final {
	ResolvedAddressSpace resolved;
	std::uint64_t pruned_epoch{~std::uint64_t{0}};
	unsigned borrowed_accesses{};
};

ThreadAddressSpaceCache &thread_address_space_cache()
{
	static thread_local ThreadAddressSpaceCache cache;
	return cache;
}

void prune_retired_thread_state(ThreadAddressSpaceCache &cache) noexcept
{
	const auto epoch =
		lifetime_retirement_epoch().load(std::memory_order_acquire);
	if (cache.pruned_epoch == epoch)
		return;
	const auto domain_retired = [](const auto &state) {
		return state != nullptr && state->address_space_lifetime &&
			state->address_space_lifetime->retired.load(
				std::memory_order_acquire);
	};
	auto &walker = active_walker();
	if (domain_retired(walker.state.get())) {
		walker.state.reset();
		walker.address_space = nullptr;
		walker.generation = 0;
	}
	auto &batch = mapping_batch();
	if (domain_retired(batch.state.get()))
		reset_mapping_batch();
	auto &active = active_thread_generation();
	if (domain_retired(active.state.get())) {
		active.address_space = nullptr;
		active.generation = 0;
		active.state.reset();
	}
	cache.pruned_epoch = epoch;
}

class AddressSpaceAccess;

/* Retirement has to wait for in-flight operations, and it used to do that with a
 * shared_mutex lease taken by every AddressSpaceAccess and held for the whole
 * call. That lock is per address space, so all ten vCPU threads of one guest
 * process serialize on it - libc++'s lock_shared takes a real internal
 * std::mutex - and G120 measured removing it as worth about 31% of the
 * miss-heavy probe, more than the process-global registry lock G101 removed.
 *
 * The replacement is the pattern jit_publication.hpp already uses for
 * jit_store_participants: each thread publishes the address space it is
 * operating on into its own slot, and teardown scans the slots instead of
 * excluding everyone.
 *
 * This is a store-buffer problem and the fences are the design, not a detail.
 * An access stores its slot and then reads `retired_`; retirement stores
 * `retired_` and then reads the slots. Both sides are seq_cst, so the two
 * stores are totally ordered and at least one side observes the other: an
 * access that is not seen by the scan is guaranteed to see the retirement and
 * back out. Relaxing either side reintroduces exactly the window the lease
 * closed. */
struct ActiveAccessSlot final {
	std::atomic<const void *> space{};
};

/* Never destroyed, under the exit contract in docs/architecture.md: a thread
 * can leave its slot, and a domain can retire, after exit() has begun
 * destroying static storage. Both are constant-initialized. */
std::mutex &active_access_registry_mutex()
{
	[[clang::no_destroy]] static std::mutex mutex;
	return mutex;
}

std::vector<ActiveAccessSlot *> &active_access_registry()
{
	[[clang::no_destroy]] static std::vector<ActiveAccessSlot *> slots;
	return slots;
}

ActiveAccessSlot &active_access_slot()
{
	/* Registered on first use and unregistered at thread exit. The scan below
	 * holds the registry mutex, and a thread in the middle of an access clears
	 * its slot from its destructor without needing that mutex, so the scan
	 * always makes progress even while an exiting thread waits to unregister. */
	struct Registration final {
		Registration()
		{
			const std::scoped_lock lock{active_access_registry_mutex()};
			active_access_registry().push_back(&slot);
		}

		~Registration()
		{
			const std::scoped_lock lock{active_access_registry_mutex()};
			auto &slots = active_access_registry();
			slots.erase(std::remove(slots.begin(), slots.end(), &slot),
				slots.end());
		}

		ActiveAccessSlot slot;
	};
	static thread_local Registration registration;
	return registration.slot;
}

void wait_for_active_accesses(const void *space) noexcept
{
	const std::scoped_lock lock{active_access_registry_mutex()};
	for (auto *slot : active_access_registry())
		while (slot->space.load(std::memory_order_seq_cst) == space)
			std::this_thread::yield();
}

class AddressSpace final {
public:
	int map(std::uint64_t generation, std::uint64_t address, int fd,
		std::uint64_t offset,
		std::uint64_t length, int protection, bool guest_code,
		bool guest_writable, bool guest_immutable)
	{
		if ((address | offset | length) % page_size != 0 || length == 0 ||
		    !stays_in_generation(address, length))
			return -1;
		auto backing = backing_for(fd);
		if (!backing || offset > backing->size() ||
		    length > backing->size() - offset)
			return -1;
		/* A page's word holds the host address beside its flags. */
		if (!FillIndex::representable(
			    reinterpret_cast<std::uintptr_t>(backing->at(offset))))
			return -1;
		protection &= protection_mask;
		generation = generation_for(address, generation);
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.address_space == this &&
		    batch.generation == generation) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch);
			batch.operations.push_back({
				true, address, length,
				Mapping{std::move(backing), offset, fd, protection,
					guest_code, guest_writable, guest_immutable}});
			return 0;
		}
		auto state = state_for(generation);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::immediate_change);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		/* A destroy() that ran between taking the state and its lock has
		 * cleared it, retired it and dropped it from the table. The
		 * generation is gone, so nothing observable is lost by leaving it
		 * alone; a write here would count words into a state that no audit
		 * walks and nothing clears. */
		if (state->retired.load(std::memory_order_acquire))
			return 0;
		map_locked(*state, address, length,
			Mapping{std::move(backing), offset, fd, protection,
				guest_code, guest_writable, guest_immutable});
		return 0;
	}

	int protect(std::uint64_t address, std::uint64_t length, int protection)
	{
		if ((address | length) % page_size != 0 || length == 0 ||
		    !stays_in_generation(address, length))
			return -1;
		const auto &active = active_thread_generation();
		const auto generation = generation_for(address,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		protection &= protection_mask;
		auto state = state_for(generation);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::immediate_change);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		/* Retired by a destroy() since the state was taken: see map(). */
		if (state->retired.load(std::memory_order_acquire))
			return 0;
#if DARWIN_UML_X86_64_GUEST
		WriterSettler writers{*state};
#endif
		bool changed = false;
		CodeSpan code;
		(void)state->fill_index.scan(address, address + length,
			[&](std::uint64_t page, std::uintptr_t word) {
				const auto fields = FillIndex::decode(word);
				if (protection_of(fields) == protection)
					return false;
				changed = true;
#if DARWIN_UML_X86_64_GUEST
				if ((protection & PROT_WRITE) != 0 && !fields.write)
					if (const auto mapping = decode_page(*state, page, word))
						FrameAccounting::reserve(*state, page,
							{mapping->backing, mapping->offset, true});
#endif
				/* The backing and the bytes stay; only losing execute
				 * leaves a translation wrong. */
				if (fields.guest_code && fields.exec &&
				    (protection & PROT_EXEC) == 0) {
					code.add(page);
					if (const auto mapping = decode_page(
						    *state, page, word))
						mapping->backing->retire_code_page(
							mapping->offset);
				}
				/* A page that gains execute is counted in below, which
				 * cannot fail: its record is made first. */
				if (fields.guest_code && !fields.exec &&
				    (protection & PROT_EXEC) != 0) {
					if (const auto mapping = decode_page(
						    *state, page, word))
						(void)mapping->backing->obtain_code_page(
							mapping->offset);
				}
				return false;
			});
		if (!changed)
			return 0;
		const std::array changes{
			MappingChangePlan{address, length, code}};
		PendingMappingChanges pending{*state, changes};
		(void)state->fill_index.scan(address, address + length,
			[&](std::uint64_t page, std::uintptr_t word) {
				auto fields = FillIndex::decode(word);
				if (fields.guest_code &&
				    fields.exec != ((protection & PROT_EXEC) != 0)) {
					if (fields.exec) {
						count_executable_departure(*state, page, word);
					} else if (const auto mapping = decode_page(
							   *state, page, word)) {
						if (auto *record = mapping->backing->find_code_page(
							    mapping->offset);
						    record != nullptr)
							MappedBacking::executable_mapping_added(record);
						else
							UML_MEMORY_COUNT_ANOMALY();
					}
				}
				fields.read = (protection & PROT_READ) != 0;
				fields.write = (protection & PROT_WRITE) != 0;
				fields.exec = (protection & PROT_EXEC) != 0;
				state->fill_index.store(page, FillIndex::encode(fields));
#if DARWIN_UML_X86_64_GUEST
				const auto before = FillIndex::decode(word);
				if (before.write && (protection & PROT_WRITE) == 0)
					FrameAccounting::release_writer(*state, page, word);
				if (FrameAccounting::translatable(
					    before.guest_code, before.exec) &&
				    !FrameAccounting::translatable(
					    before.guest_code, (protection & PROT_EXEC) != 0))
					FrameAccounting::release_holder(*state, page, word);
#endif
				return false;
			});
		pending.commit();
#if DARWIN_UML_X86_64_GUEST
		FrameAccounting::commit(*state);
#endif
		return 0;
	}

	int unmap(std::uint64_t generation, std::uint64_t address,
		  std::uint64_t length)
	{
		if (length == 0)
			return 0;
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return -1;
		const auto end = address + length;
		if (end > std::numeric_limits<std::uint64_t>::max() -
				(page_size - 1))
			return -1;
		const auto first = address & ~(page_size - 1);
		const auto rounded_end = (end + page_size - 1) & ~(page_size - 1);
		if (AddressLayout::classify(first, rounded_end - first) ==
		    AddressDomain::invalid)
			return -1;
		address = first;
		length = rounded_end - first;
		generation = generation_for(address, generation);
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.address_space == this &&
		    batch.generation == generation) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch);
			batch.operations.push_back(
				{false, address, length, Mapping{}});
			return 0;
		}
		auto state = state_for(generation);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::immediate_change);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		/* Retired by a destroy() since the state was taken: see map(). */
		if (state->retired.load(std::memory_order_acquire))
			return 0;
		/* Linux invalidates after it changes a PTE and before it frees the
		 * page.  A walker which sampled the old PTE may not have published it
		 * yet, so the range can be absent here and the snapshot still stale:
		 * reject every batch that began before this point, whether or not a
		 * mapping was removed. */
		(void)unmap_locked(*state, address, length);
		state->invalidation_epoch.fetch_add(1, std::memory_order_release);
		return 0;
	}

	int flush_generation(std::uint64_t generation)
	{
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		if (state->mapped_pages == 0) {
			/* A batch which started before this flush must not publish an old
			 * page-table snapshot after the flush returns, even when no mapping
			 * had reached the host cache yet. */
			state->invalidation_epoch.fetch_add(
				1, std::memory_order_release);
			return 0;
		}
		/* A zero-length history entry tells every executor to discard its
		 * complete private TLB.  Whole-mm Linux invalidation cannot retain a
		 * narrower range, and clearing the generation here replaces one host
		 * execution quiesce per VMA with one generation-wide boundary. */
		const std::array changes{
			MappingChangePlan{0, 0, {.whole = true}}};
		PendingMappingChanges pending{*state, changes};
		state->invalidation_epoch.fetch_add(
			1, std::memory_order_release);
		clear_mappings(*state);
		pending.commit();
		return 0;
	}

	int activate(std::uint64_t generation)
	{
		/* A UML process is a Darwin thread, so the selected Linux mm is a
		 * thread property. A process-global selector lets a sibling switch mm
		 * between activate() and map(), installing a page in the wrong guest.
		 * Translation already carries an explicit generation; activation only
		 * supplies the implicit generation used by mapping-management calls.
		 */
		auto &active = active_thread_generation();
		if (active.address_space == this && active.generation == generation &&
		    active.state &&
		    !active.state->retired.load(std::memory_order_acquire))
			return 0;
		active.address_space = this;
		active.generation = generation;
		active.state = state_for(generation);
		return 0;
	}

	/* Whether the generation still has an address space: destroying one
	 * removes its state, and a generation never comes back. */
	int generation_live(std::uint64_t generation)
	{
		if (generation == 0)
			return 1;
		std::shared_lock lock{states_mutex_};
		const auto found = states_.find(generation);
		return found != states_.end() &&
			!found->second->retired.load(std::memory_order_acquire);
	}

	int destroy(std::uint64_t generation)
	{
		if (generation == 0)
			return 0;
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		{
			std::lock_guard lock{state->mappings_mutex};
			const std::array changes{
				MappingChangePlan{0, 0, {.whole = true}}};
			PendingMappingChanges pending{*state, changes};
			clear_mappings(*state);
			pending.commit();
#if DARWIN_UML_X86_64_GUEST
			FrameAccounting::finish(*state);
#endif
			/* Under the lock that cleared the generation. A batch that
			 * began earlier takes this lock to commit, and sees either the
			 * mappings it was made against or a generation that is gone;
			 * were the store after the unlock, it could count its words in
			 * between and have them erased with the state, leaving a
			 * frame's count too high for good. */
			state->retired.store(true, std::memory_order_release);
		}
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::destroy_cleared);
		{
			std::lock_guard lock{states_mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end() && found->second == state)
				states_.erase(found);
		}
		/* After the erasure, so a holder that loads the new count and then
		 * asks generation_live() is told the generation is gone. */
		generation_retirement_sequence().fetch_add(
			1, std::memory_order_release);
		auto &active = active_thread_generation();
		if (active.address_space == this && active.generation == generation) {
			active.state.reset();
			active.address_space = nullptr;
		}
		return 0;
	}

	void retire() noexcept
	{
		if (retired_.exchange(true, std::memory_order_seq_cst))
			return;
		/* Ordered against the slot store in AddressSpaceAccess: an access this
		 * scan does not observe is guaranteed to observe the retirement. */
		wait_for_active_accesses(this);
		lifetime_->retired.store(true, std::memory_order_release);
		lifetime_retirement_epoch().fetch_add(1, std::memory_order_acq_rel);
#if DARWIN_UML_X86_64_GUEST
		decltype(states_) gone;
#endif
		{
			std::lock_guard lock{states_mutex_};
			for (auto &[generation, state] : states_) {
				(void)generation;
				state->retired.store(true, std::memory_order_release);
			}
#if DARWIN_UML_X86_64_GUEST
			gone = std::move(states_);
#endif
			states_.clear();
		}
#if DARWIN_UML_X86_64_GUEST
		/* After states_mutex_ is released: the walk takes each state's
		 * mapping lock and the frames' stripe locks. */
		UML_MEMORY_RETIRE_WALK_UNLOCKED(states_mutex_);
		for (auto &[generation, state] : gone) {
			(void)generation;
			std::lock_guard lock{state->mappings_mutex};
			FrameAccounting::retire(*state);
		}
#endif
		generation_retirement_sequence().fetch_add(
			1, std::memory_order_release);
		auto &active = active_thread_generation();
		if (active.address_space == this) {
			active.address_space = nullptr;
			active.generation = 0;
			active.state.reset();
		}
		auto &batch = mapping_batch();
		if (batch.address_space == this) {
			batch.operations.clear();
			batch.backings.clear();
			batch.address_space = nullptr;
			batch.depth = 0;
			batch.generation = 0;
			batch.invalidation_epoch = 0;
			batch.state.reset();
		}
		auto &walker = active_walker();
		if (walker.address_space == this && walker.state) {
			walker.state.reset();
			walker.address_space = nullptr;
			walker.generation = 0;
		}
	}

	int discard_if_stale(std::uint64_t address, int fd, std::uint64_t offset)
	{
		address &= ~(page_size - 1);
		const auto &active = active_thread_generation();
		const auto generation = generation_for(address,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		const auto names_other_backing = [&](const GenerationState &state) {
			const auto mapping = find_page(state, address);
			return mapping && (mapping->fd != fd || mapping->offset != offset);
		};
		{
			/* Every kernel uaccess asks, and nearly every one finds the page
			 * absent or already right; neither is worth stopping the
			 * generation's executors for.  The active generation is
			 * borrowed without a reference, as a translation borrows it.
			 * The caller's backing is only compared, never installed, so it
			 * is never looked up. */
			auto reference = reference_for(generation);
			std::shared_lock lock{reference.state->mappings_mutex};
			if (!names_other_backing(*reference.state))
				return 0;
		}
		auto state = state_for(generation);
		ExecutionQuiesce quiesce{state, generation};
		std::lock_guard lock{state->mappings_mutex};
		if (!names_other_backing(*state))
			return 0;
		/* The caller pinned this backing from a PTE it no longer holds the
		 * lock of, so the disagreement says only that one of the two is
		 * stale, not which.  Installing the caller's page here is how a
		 * uaccess that raced a copy-on-write pointed every sibling back at
		 * the page Linux had just replaced, writable, and their stores were
		 * lost to it.  An absent page is always sound: the next access
		 * faults and a walker replays the PTE under the invalidation epoch.
		 * This is an invalidation like any other, so older batches go too. */
		(void)unmap_locked(*state, address, page_size);
		state->invalidation_epoch.fetch_add(1, std::memory_order_release);
		return 0;
	}

	/* A kernel write (`kernel_write`) publishes a code change only when
	 * kernel_code_write_publishes() says x86 code may hold the page. The
	 * plain copy publishes only the range of its own mapping, so it is not
	 * for a runtime client's write to a code page that another mapping of
	 * the frame may hold. */
	int copy_from_buffer(std::uint64_t address, const void *buffer,
			     std::uint64_t length, bool kernel_write)
	{
		if (length == 0)
			return 0;
		if (buffer == nullptr || address + length < address ||
		    length > page_size - (address & (page_size - 1)))
			return -1;
		const auto page = address & ~(page_size - 1);
		const auto in_page = address & (page_size - 1);
		/* The mapping must remain stable while the written range is copied,
		 * but neither the mapping table nor generation metadata is structurally
		 * changed.  Let unrelated page copies and translations proceed in
		 * parallel.  Copying only the bytes changed by uaccess is essential:
		 * a sibling guest thread may update another byte in the same guest page
		 * through its Darwin 16 KiB host-page shadow.
		 */
		const auto &active = active_thread_generation();
		const auto generation = generation_for(page,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
#if DARWIN_UML_X86_64_GUEST
		/* A kernel write reaches the frame through the kernel's own alias of
		 * it, which `buffer` is. Name the frame before any generation lock is
		 * taken: the table the host keeps of those aliases has a lock of its
		 * own, and the descriptor table another. */
		std::shared_ptr<MappedBacking> alias_backing;
		std::uint64_t alias_offset = 0;
		if (kernel_write) {
			int fd = -1;
			if (FrameAccounting::resolve_direct(buffer, fd, alias_offset))
				alias_backing = existing_backing(fd);
		}
#endif
		auto reference = reference_for(generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto word = reference.state->fill_index.lookup(page);
		if (word != 0) {
			std::memcpy(reinterpret_cast<void *>(
					    (word & FillIndex::address_mask) + in_page),
				buffer, length);
			if ((word & FillIndex::guest_code_page) != 0 &&
			    (!kernel_write || kernel_code_write_publishes())) {
				if (const auto mapping = decode_page(
					    *reference.state, page, word))
					MappedBacking::invalidate_code_page(
						mapping->backing->code_page_handle(
							mapping->offset));
				::uml::memory::publish_code_change(
					*reference.state, page, page_size);
			}
		}
#if DARWIN_UML_X86_64_GUEST
		/* The bytes are in the shared backing at the write, so the
		 * publication follows them: at every holder of the frame, whichever
		 * generation and address registered it. The frame is the one the
		 * user word names when it is present, and the one the kernel's alias
		 * names when the table resolves it: normally one frame, and each is
		 * published when they differ. */
		if (kernel_write && kernel_code_write_publishes()) {
			constexpr auto frame_mask = ~(page_size - 1);
			MappedBacking *user_backing = nullptr;
			std::uint64_t user_offset = 0;
			if (const auto mapping = decode_page(*reference.state, page, word)) {
				user_backing = mapping->backing;
				user_offset = mapping->offset & frame_mask;
			}
			const auto alias_frame = alias_offset & frame_mask;
			const bool same_frame = user_backing != nullptr &&
				user_backing == alias_backing.get() &&
				user_offset == alias_frame;
			if (user_backing != nullptr && !same_frame)
				FrameAccounting::publish_kernel_write(*user_backing,
					user_offset, in_page, length);
			if (alias_backing) {
				FrameAccounting::publish_kernel_write(*alias_backing,
					alias_frame, alias_offset & (page_size - 1), length);
			} else {
				/* The buffer names no frame, whether or not the user word
				 * does: every generation reads every translation again. */
				FrameAccounting::note_unresolved_kernel_write();
				lock.unlock();
				publish_all_generations();
			}
		}
#endif
		return 0;
	}

	int translate(std::uint64_t generation, std::uint64_t address,
		      darwin_uml_static_access access, void **result)
	{
		if (result == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*result = nullptr;
		const auto page = address & ~(page_size - 1);
		const auto resolved_generation = generation_for(page, generation);
		/* The execution gate excludes mapping mutation in this generation.
		 * Keep each generation's mapping lookup independent so unrelated guest
		 * processes neither share a hash table nor touch the same lock cacheline.
		 */
		auto reference = reference_for(resolved_generation);
		return translate_in(*reference.state, address, access, result);
	}

	/* translate() for an entered execution context of `generation`: the
	 * context holds the generation's state, so a guest address needs neither
	 * the thread's address space nor its active generation. Anything else
	 * answers execution_context_fallback for the general path. */
	static constexpr int execution_context_fallback = -1000;
	[[nodiscard]] static int execution_context_translate(
		ExecutionContext *context, std::uint64_t generation,
		std::uint64_t address, darwin_uml_static_access access,
		void **result)
	{
		if (context == nullptr || !context->entered ||
		    context->generation != generation || result == nullptr)
			return execution_context_fallback;
		const auto page = address & ~(page_size - 1);
		if (!AddressLayout::is_guest(page) ||
		    context->state->retired.load(std::memory_order_acquire))
			return execution_context_fallback;
		*result = nullptr;
		const auto wanted = access == DARWIN_UML_STATIC_EXECUTE ?
			FillIndex::executable :
			access == DARWIN_UML_STATIC_WRITE ?
				FillIndex::writable : FillIndex::readable;
		const auto entry = context->state->fill_index.find(page);
		if ((entry & wanted) != 0) {
			*result = reinterpret_cast<void *>(
				(entry & FillIndex::address_mask) +
				(address & (page_size - 1)));
			return DARWIN_UML_STATIC_TRANSLATED;
		}
		return translate_in(*context->state, address, access, result);
	}

	[[nodiscard]] static int translate_in(GenerationState &state,
		std::uint64_t address, darwin_uml_static_access access,
		void **result)
	{
		const auto page = address & ~(page_size - 1);
		std::shared_lock lock{state.mappings_mutex};
		const auto word = state.fill_index.lookup(page);
		if (word == 0)
			return DARWIN_UML_STATIC_UNMAPPED;
		const auto wanted = access == DARWIN_UML_STATIC_EXECUTE ?
			FillIndex::executable :
			access == DARWIN_UML_STATIC_WRITE ?
				FillIndex::writable : FillIndex::readable;
		if ((word & wanted) == 0)
			return DARWIN_UML_STATIC_PERMISSION;
		*result = reinterpret_cast<void *>(
			(word & FillIndex::address_mask) + (address & (page_size - 1)));
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	/* translate() for an execute access that also answers the holder word of
	 * the frame it translated, in which the caller registers its generation
	 * before it reads the frame's bytes (code_holders.hpp). A null word says
	 * the frame's backing cannot make one: its publications go to every
	 * generation, and there is nothing to register. */
	int translate_execute(std::uint64_t generation, std::uint64_t address,
		void **result, void **holder)
	{
		if (result == nullptr || holder == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*result = nullptr;
		*holder = nullptr;
		const auto page = address & ~(page_size - 1);
		auto reference = reference_for(generation_for(page, generation));
		auto &state = *reference.state;
		std::shared_lock lock{state.mappings_mutex};
		const auto word = state.fill_index.lookup(page);
		if (word == 0)
			return DARWIN_UML_STATIC_UNMAPPED;
		if ((word & FillIndex::executable) == 0)
			return DARWIN_UML_STATIC_PERMISSION;
		if (const auto mapping = decode_page(state, page, word))
			*holder = mapping->backing->holder_word(mapping->offset);
		*result = reinterpret_cast<void *>(
			(word & FillIndex::address_mask) + (address & (page_size - 1)));
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	int code_page(std::uint64_t generation, std::uint64_t address,
		void **handle, darwin_uml_static_code_page_stamp *stamp)
	{
		if (handle == nullptr || stamp == nullptr)
			return DARWIN_UML_STATIC_HOST_ERROR;
		*handle = nullptr;
		*stamp = {};
		const auto page = address & ~(page_size - 1);
		const auto resolved_generation = generation_for(page, generation);
		auto reference = reference_for(resolved_generation);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto mapping = find_page(*reference.state, page);
		if (!mapping)
			return DARWIN_UML_STATIC_UNMAPPED;
		if (!can_hold_translation(*mapping))
			return DARWIN_UML_STATIC_PERMISSION;
		auto *code_page = mapping->backing->code_page_handle(mapping->offset);
		if (code_page == nullptr ||
		    !load_stamp(code_page, *stamp))
			return DARWIN_UML_STATIC_HOST_ERROR;
		if (mapping->immutable)
			stamp->flags |= DARWIN_UML_STATIC_STAMP_IMMUTABLE;
		*handle = code_page;
		return DARWIN_UML_STATIC_TRANSLATED;
	}

	[[nodiscard]] std::uint64_t changes(std::uint64_t generation)
	{
		auto &active = active_thread_generation();
		/* The active generation owns this state for the lifetime of the
		 * thread's activation.  Read its counters in place: copying the
		 * shared_ptr here made every translated block contend on the same
		 * reference count in multi-process workloads.
		 */
		if (active.address_space == this &&
		    active.generation == generation && active.state &&
		    !active.state->retired.load(std::memory_order_acquire))
			return active.state->mapping_changes.load(
				std::memory_order_acquire);
		return state_for(generation)->mapping_changes.load(
			std::memory_order_acquire);
	}

	int mapping_changes(std::uint64_t generation, std::uint64_t sequence,
		struct darwin_uml_static_mapping_change *output,
		std::size_t capacity, std::uint64_t *current_sequence)
	{
		if (current_sequence == nullptr || (capacity != 0 && output == nullptr))
			return -1;
		/* A vCPU asks after every mapping change it notices, so this runs
		 * as often as the misses do: resolve the state the way translate()
		 * does, through the thread's active generation, not through the
		 * address space's state table and its lock. */
		auto reference = reference_for(generation);
		auto *state = reference.state;
		std::shared_lock lock{state->mappings_mutex};
		const auto current = state->mapping_changes.load(
			std::memory_order_acquire);
		*current_sequence = current;
		if (sequence >= current)
			return 0;
		if (state->mapping_history.empty() ||
		    sequence + 1 < state->mapping_history.front().sequence)
			return -1;
		const auto first_sequence =
			state->mapping_history.front().sequence;
		const auto first_index = sequence < first_sequence ? 0 :
			static_cast<std::size_t>(sequence - first_sequence + 1);
		if (state->mapping_history.size() - first_index > capacity)
			return -1;
		std::size_t count = 0;
		for (auto index = first_index;
		     index < state->mapping_history.size(); ++index) {
			const auto &change = state->mapping_history[index];
			if (change.length == 0)
				return -1;
			output[count++] = {change.address, change.length};
		}
		return static_cast<int>(count);
	}

	/* A guest code synchronization (IC IVAU) at `address`, in the generation
	 * the thread is running when it has entered an execution context of this
	 * address space, else in the thread's active generation. False when the
	 * address names a guest page that the generation maps nothing at: the
	 * instruction would have faulted on hardware, and which frame it meant is
	 * unknown. */
	bool invalidate_code(std::uint64_t address)
	{
		if (AddressLayout::is_guest(address)) {
			if (const auto *entered = entered_thread_context();
			    entered != nullptr && entered->state &&
			    entered->state->address_space_lifetime == lifetime_) {
				/* A generation destroyed under its run has nothing to
				 * publish to, and the active generation is not a
				 * substitute for it. */
				if (entered->state->retired.load(
					    std::memory_order_acquire))
					return true;
				return synchronize_code(
					*entered->state, entered->generation, address);
			}
		}
		const auto &active = active_thread_generation();
		/* Translated execution normally publishes through its cached context.
		 * Keep this routed fallback coherent with the same physical-page stamp,
		 * including callers which invalidate code outside translated execution.
		 */
		if (AddressLayout::is_guest(address) &&
		    active.address_space == this && active.state &&
		    !active.state->retired.load(std::memory_order_acquire)) {
			return synchronize_code(
				*active.state, active.generation, address);
		}
		const auto generation = generation_for(address,
			active.address_space == this && active.state &&
				!active.state->retired.load(std::memory_order_acquire) ?
				active.generation : 0);
		auto state = state_for(generation);
		return synchronize_code(*state, generation, address);
	}

	/* Linux wrote a user page through its own mapping of the frame -- a
	 * ptrace, /proc/pid/mem or process_vm_writev() store into another
	 * process's memory -- so no translated store saw it. Move each code
	 * page's stamp and publish the range in that process's generation, as a
	 * guest code synchronization would, and in every other generation that
	 * translated one of the frames: whatever the page's permissions in the
	 * process written, since another process's view of the frame may be the
	 * executable one. A page the process has no entry for yet (a software
	 * table the kernel fills at faults) names no frame, so every generation
	 * hears of the change. Data transfers publish nothing unless some
	 * generation translated the frame, or the page is one the process never
	 * touched.
	 *
	 * The x86-64 holders of a frame (frame_accounting.hpp) do not read the
	 * holder word. Once x86 code may run (kernel_code_write_publishes()),
	 * the frame of every page the process maps is published at all of them,
	 * whatever the process's view of it permits, and a page with no entry
	 * moves every generation's flush serial, as no record's stamp does for
	 * a frame nobody named (the arm64 verdicts' epochs move through the
	 * count of such writes instead, MappedBacking::note_unnamed_publication). */
	int invalidate_code(std::uint64_t generation, std::uint64_t address,
			    std::uint64_t length)
	{
		if (length == 0)
			return 0;
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return -1;
		const auto end = address + length;
		if (end > std::numeric_limits<std::uint64_t>::max() -
				(page_size - 1))
			return -1;
		const auto first = address & ~(page_size - 1);
		const auto rounded_end = (end + page_size - 1) & ~(page_size - 1);
		if (AddressLayout::classify(first, rounded_end - first) ==
		    AddressDomain::invalid)
			return -1;
		const auto target = generation_for(first, generation);
		auto state = state_for(target);
		std::uint64_t publish_first = rounded_end;
		std::uint64_t publish_end = first;
		/* Whether the target must hear of the change at every address: it
		 * holds a frame written (its translations and verified pages may
		 * stand at another address than the one written), or the frame is
		 * unknown. */
		bool whole_space = false;
		code_holders::HolderList others;
#if DARWIN_UML_X86_64_GUEST
		/* Whether x86 code may hold the frames written, and whether some
		 * page named none. */
		const bool x86_holders = kernel_code_write_publishes();
		bool unknown_frame = false;
#endif
		/* The kernel's bytes are in the frame already; the holders are read
		 * after them, as a code synchronization's fence orders it. */
		std::atomic_thread_fence(std::memory_order_seq_cst);
		{
			std::shared_lock lock{state->mappings_mutex};
			for (auto page = first; page < rounded_end;
			     page += page_size) {
				const auto mapping = find_page(*state, page);
				if (!mapping) {
					/* No frame is named, so no stamp moves; a verdict
					 * filed under any of them must not be answered
					 * again. */
					MappedBacking::note_unnamed_publication();
					others.add_every();
					code_holders::count(&code_holders::detail::
						CounterStore::absent_kernel_pages);
#if DARWIN_UML_X86_64_GUEST
					unknown_frame = true;
#endif
				} else {
					step_stamp(*mapping);
					const bool own = take_holders(*mapping, target,
						others, holder_takes_in_flight_, true);
					whole_space = whole_space || own;
#if DARWIN_UML_X86_64_GUEST
					/* The page's frame may be translated at other
					 * addresses and in other generations, whether or not
					 * this view of it is code or an arm64 holder: the
					 * holder word says nothing of x86 holders. */
					if (x86_holders)
						FrameAccounting::publish_kernel_write(
							*mapping->backing, mapping->offset, 0, page_size);
#endif
					if (!mapping->guest_code && !own)
						continue;
				}
				publish_first = std::min(publish_first, page);
				publish_end = page + page_size;
			}
			/* A list that goes to every generation, because an unfinished
			 * publisher has consumed holders (take_holders) or for any other
			 * reason, leaves the target out of it: the target is published
			 * here, whole, whatever its pages map, as the holder an
			 * unfinished publisher took may be the target itself. */
			if (others.every())
				whole_space = true;
		}
		/* The target's own view of the frame is not the only one it may
		 * run or have verified: another mapping of the frame (an
		 * executable alias, or the one the write reached through the
		 * kernel's page table) is at an address the written range does not
		 * name. A generation that holds the frame, or any frame, hears of
		 * a change at every address, like the generations that hold it; a
		 * generation that holds none hears of the range it wrote. */
		if (whole_space)
			::uml::memory::publish_code_change(*state, 0, 0);
		else if (publish_first < publish_end)
			::uml::memory::publish_code_change(*state, publish_first,
				publish_end - publish_first);
#if DARWIN_UML_X86_64_GUEST
		/* A page that names no frame may be any frame an x86 body holds, and
		 * the stamp of none moved: every generation compares its bodies
		 * again. After the mapping lock, as the table of generations is
		 * taken. */
		if (unknown_frame && x86_holders)
			publish_all_generations();
#endif
		publish_to_holders(target, others);
		return 0;
	}

	/* Linux wrote [address, address + length), within one guest page, through
	 * its own alias of the frame at `fd` and `offset`, which it still holds:
	 * a ptrace, /proc/pid/mem or process_vm_writev() store into another
	 * process's memory. That process may have mapped another frame at the
	 * address since Linux found the page, so the change is published at the
	 * holders of the frame written, whatever the generation maps there now,
	 * and in the generation when its view of the page is that frame and code,
	 * or it holds the frame. The frame mapped now is not written, and is
	 * published only when it is the frame named.
	 *
	 * No generation maps a frame that has no backing here, so none holds it.
	 * A negative `fd` names no frame: it takes the route of a page the
	 * process has no entry for, whatever the process maps (invalidate_code()).
	 * The x86-64 holders of a frame do not read the holder word; once x86
	 * code may run (kernel_code_write_publishes()), the frame written is
	 * published at all of them. */
	int invalidate_code_frame(std::uint64_t generation, std::uint64_t address,
				  std::uint64_t length, int fd,
				  std::uint64_t offset)
	{
		if (length == 0)
			return 0;
		const auto in_page = address & (page_size - 1);
		if (length > page_size - in_page)
			return -1;
		if (fd >= 0 && (offset & (page_size - 1)) != 0)
			return -1;
		const auto page = address & ~(page_size - 1);
		if (AddressLayout::classify(page, page_size) == AddressDomain::invalid)
			return -1;
		const auto target = generation_for(page, generation);
		auto state = state_for(target);
		/* Before any generation lock: the descriptor table has its own. */
		const auto backing = fd >= 0 ? existing_backing(fd) :
			std::shared_ptr<MappedBacking>{};
		code_holders::HolderList others;
		/* Whether the target must hear of the change at every address, as in
		 * invalidate_code(): it holds the frame written, or the frame is
		 * unknown. */
		bool whole_space = false;
		bool publish_target = false;
#if DARWIN_UML_X86_64_GUEST
		const bool x86_holders = kernel_code_write_publishes();
		bool unknown_frame = false;
#else
		constexpr bool x86_holders = false;
#endif
		/* The kernel's bytes are in the frame already; the holders are read
		 * after them, as a code synchronization's fence orders it. */
		std::atomic_thread_fence(std::memory_order_seq_cst);
		{
			std::shared_lock lock{state->mappings_mutex};
			const auto mapping = find_page(*state, page);
			if (fd < 0) {
				/* No frame is named, so no stamp moves; a verdict filed
				 * under any of them must not be answered again
				 * (invalidate_code()). */
				MappedBacking::note_unnamed_publication();
				others.add_every();
				code_holders::count(&code_holders::detail::
					CounterStore::absent_kernel_pages);
#if DARWIN_UML_X86_64_GUEST
				unknown_frame = true;
#endif
			}
			if (mapping && (fd < 0 || (backing &&
				    mapping->backing == backing.get() &&
				    mapping->offset == offset))) {
				/* The frame the generation maps, written or taken
				 * for it: invalidate_code()'s page. */
				step_stamp(*mapping);
				const bool own = take_holders(*mapping, target,
					others, holder_takes_in_flight_, true);
				whole_space = own;
#if DARWIN_UML_X86_64_GUEST
				if (x86_holders)
					FrameAccounting::publish_kernel_write(
						*mapping->backing, mapping->offset, 0,
						page_size);
#endif
				publish_target = mapping->guest_code || own;
			} else if (backing) {
				/* The generation maps no page, or another, where the
				 * kernel wrote this one: the stamp of the frame moves
				 * if a translator asked for one, before the word is
				 * read, and the frame's readers hear of the change. */
				retire_written_frame(*backing, offset, x86_holders);
				whole_space = take_holders(*backing, offset, target,
					others, holder_takes_in_flight_, true);
				publish_target = whole_space;
			}
			/* A list that goes to every generation leaves the target out
			 * of it: the target is published here (invalidate_code()). */
			if (others.every())
				whole_space = true;
		}
		if (whole_space)
			::uml::memory::publish_code_change(*state, 0, 0);
		else if (publish_target)
			::uml::memory::publish_code_change(*state, page, page_size);
#if DARWIN_UML_X86_64_GUEST
		/* A frame that was not named may be any frame an x86 body holds
		 * (invalidate_code()). */
		if (unknown_frame && x86_holders) {
			FrameAccounting::note_unresolved_kernel_write();
			publish_all_generations();
		}
#endif
		publish_to_holders(target, others);
		return 0;
	}

	/* The frame at `offset` of `backing` was written through the kernel's
	 * alias of it: its stamp moves, version and epoch, if a translator ever
	 * asked for one (no record is made), ordered before a later read of the
	 * frame's holder word as step_stamp is, and, once x86 code may hold it
	 * (`x86_holders`), the frame accounting publishes at its x86 holders, a
	 * frame with no record costing nothing. Allocates nothing. */
	static void retire_written_frame(MappedBacking &backing,
		std::uint64_t offset, [[maybe_unused]] bool x86_holders) noexcept
	{
		if (auto *record = backing.find_code_page(offset);
		    record != nullptr) {
			MappedBacking::invalidate_code_page(record);
			std::atomic_thread_fence(std::memory_order_seq_cst);
		}
#if DARWIN_UML_X86_64_GUEST
		if (x86_holders)
			FrameAccounting::publish_kernel_write(backing, offset, 0,
				page_size);
#endif
	}

	/* Linux wrote the bytes of the whole guest pages [offset, offset +
	 * length) of the frame source at `fd` through its own alias, for no
	 * process and at no user address: a write(2), splice(2), truncate or hole
	 * punch into a page-cache or memfd frame, or a block read into a page a
	 * process pinned, anonymous or not (any frame of guest RAM is named by
	 * the fd and offset of its backing). Only x86 can hold stale code
	 * for it (an arm64 guest follows the architecture's cache maintenance),
	 * so the call publishes at the frames' x86 holders, whichever address
	 * spaces map them, and returns at once while no x86 code may hold a
	 * frame (kernel_code_write_publishes()). A negative `fd` names no frame:
	 * every generation reads every translation again. A frame no generation
	 * maps has no backing here, so no holder can exist. */
	int invalidate_code_frames(int fd, std::uint64_t offset,
				   std::uint64_t length)
	{
		if (length == 0)
			return 0;
		if (((offset | length) & (page_size - 1)) != 0 ||
		    offset + length < offset)
			return -1;
#if DARWIN_UML_X86_64_GUEST
		FrameAccounting::note_kernel_frame_write();
		if (!kernel_code_write_publishes())
			return 0;
		/* The kernel's bytes are in the frames already; the holders are
		 * read after them (invalidate_code()). */
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (fd < 0) {
			FrameAccounting::note_unresolved_kernel_write();
			publish_all_generations();
			return 0;
		}
		const auto backing = existing_backing(fd);
		if (!backing)
			return 0;
		for (auto frame = offset; frame < offset + length;
		     frame += page_size)
			retire_written_frame(*backing, frame, true);
#else
		(void)fd;
#endif
		return 0;
	}

#if DARWIN_UML_X86_64_GUEST
	/* The state an x86 translator's frame calls act on: the entered
	 * context's, or the generation's. */
	[[nodiscard]] GenerationReference frame_call_state(
		void *opaque, std::uint64_t generation, std::uint64_t page)
	{
		auto *context = static_cast<ExecutionContext *>(opaque);
		if (context != nullptr && context->entered &&
		    context->generation == generation &&
		    !context->state->retired.load(std::memory_order_acquire))
			return {{}, context->state.get()};
		return reference_for(generation_for(page, generation));
	}

	/* Registers the page as a holder of its frame (frame_accounting.hpp). */
	int hold_code_frame(void *context, std::uint64_t generation,
		std::uint64_t address, darwin_uml_static_frame_location *owed,
		std::size_t capacity, darwin_uml_static_frame_hold *hold)
	{
		if (hold == nullptr || (capacity != 0 && owed == nullptr))
			return DARWIN_UML_STATIC_FRAME_FAILED;
		const bool global_ok = hold->global_ok != 0;
		*hold = {};
		const auto page = address & ~(page_size - 1);
		auto reference = frame_call_state(context, generation, page);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto report = FrameAccounting::hold(
			*reference.state, page, owed, capacity, global_ok);
		hold->record_identity = report.record_identity;
		hold->debt_sequence = report.debt_sequence;
		hold->owed = report.owed;
		hold->record = report.record;
		hold->alias_changes = report.alias_changes;
		switch (report.result) {
		case FrameAccounting::HoldResult::settled:
			return DARWIN_UML_STATIC_FRAME_SETTLED;
		case FrameAccounting::HoldResult::owed:
			return DARWIN_UML_STATIC_FRAME_OWED;
		case FrameAccounting::HoldResult::refused:
			return DARWIN_UML_STATIC_FRAME_REFUSED;
		case FrameAccounting::HoldResult::crowded:
			return DARWIN_UML_STATIC_FRAME_CROWDED;
		case FrameAccounting::HoldResult::global:
			return DARWIN_UML_STATIC_FRAME_GLOBAL;
		case FrameAccounting::HoldResult::failed:
			break;
		}
		return DARWIN_UML_STATIC_FRAME_FAILED;
	}

	void clear_code_debt(void *context, std::uint64_t generation,
		std::uint64_t address, std::uint64_t record_identity,
		std::uint64_t debt_sequence)
	{
		const auto page = address & ~(page_size - 1);
		auto reference = frame_call_state(context, generation, page);
		std::shared_lock lock{reference.state->mappings_mutex};
		FrameAccounting::clear_debt(*reference.state, page, record_identity,
			debt_sequence);
	}

	int frame_state(void *context, std::uint64_t generation,
		std::uint64_t address, darwin_uml_static_frame_state *state)
	{
		if (state == nullptr)
			return -1;
		const auto page = address & ~(page_size - 1);
		auto reference = frame_call_state(context, generation, page);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto fill = FrameAccounting::fill_state(*reference.state, page);
		*state = {fill.backing, fill.offset, fill.record, fill.alias_changes,
			fill.other_holder ? 1 : 0};
		return 0;
	}

	int frame_own_holders(void *context, std::uint64_t generation,
		std::uint64_t address, std::uint64_t length, std::uint64_t *own,
		std::size_t capacity, darwin_uml_static_frame_publication *result)
	{
		if (result == nullptr || length == 0)
			return -1;
		*result = {};
		const auto page = address & ~(page_size - 1);
		auto reference = frame_call_state(context, generation, page);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto done = FrameAccounting::own_holders(*reference.state, page,
			address & (page_size - 1), length, own, capacity);
		result->own_count = done.own_count;
		result->overflow = done.overflow ? 1 : 0;
		return done.handled ? 1 : 0;
	}

	int publish_frame_store(void *context, std::uint64_t generation,
		std::uint64_t address, std::uint64_t length, std::uint64_t *own,
		std::size_t capacity, darwin_uml_static_frame_publication *result)
	{
		if (result == nullptr || length == 0)
			return -1;
		*result = {};
		const auto page = address & ~(page_size - 1);
		auto reference = frame_call_state(context, generation, page);
		std::shared_lock lock{reference.state->mappings_mutex};
		const auto done = FrameAccounting::publish_store(*reference.state,
			page, address & (page_size - 1), length, own, capacity);
		result->own_count = done.own_count;
		result->own_records = done.own_records;
		result->overflow = done.overflow ? 1 : 0;
		return done.handled ? 1 : 0;
	}

	/* Every generation of the address space reads every translation again:
	 * the fallback for a write that names no frame. The flush serial moves,
	 * because a stamp does not. It runs after the bytes changed, so it
	 * cannot fail: it walks the live table and allocates nothing, and the
	 * two calls it makes are atomic stores. */
	void publish_all_generations() noexcept
	{
		std::shared_lock lock{states_mutex_};
		for (const auto &[generation, state] : states_) {
			(void)generation;
			FrameAccounting::bump_flush_serial(*state);
			::uml::memory::publish_code_change(*state, 0, 0);
		}
	}
#endif

	[[nodiscard]] std::uint64_t active_generation() const noexcept
	{
		const auto &active = active_thread_generation();
		return active.address_space == this && active.state &&
			!active.state->retired.load(std::memory_order_acquire) ?
			active.generation : 0;
	}

	void forget_descriptor(int fd)
	{
		std::lock_guard lock{backing_mutex_};
		descriptor_backings_.erase(fd);
	}

	/* The backing this address space made for the descriptor, if it still has
	 * one. */
	[[nodiscard]] std::shared_ptr<MappedBacking> existing_backing(int fd)
	{
		std::shared_lock lock{backing_mutex_};
		if (const auto found = descriptor_backings_.find(fd);
		    found != descriptor_backings_.end())
			return found->second.lock();
		return {};
	}

	int begin_batch(std::uint64_t generation)
	{
		auto &batch = mapping_batch();
		if (batch.depth != 0) {
			if (batch.address_space != this ||
			    batch.generation != generation)
				return -1;
			++batch.depth;
			return 0;
		}
		/* A walker has already resolved the generation it is bound to. */
		const auto &walker = active_walker();
		auto state = walker.state && walker.address_space == this &&
				walker.generation == generation &&
				!walker.state->retired.load(std::memory_order_acquire) ?
			walker.state : state_for(generation);
		const auto invalidation_epoch = state->invalidation_epoch.load(
			std::memory_order_acquire);
		reset_mapping_batch();
		batch.address_space = this;
		batch.generation = generation;
		batch.invalidation_epoch = invalidation_epoch;
		batch.state = std::move(state);
		batch.depth = 1;
		return 0;
	}

	int end_batch()
	{
		auto &batch = mapping_batch();
		if (batch.depth == 0 || batch.address_space != this)
			return -1;
		if (--batch.depth != 0)
			return 0;
		struct BatchReset final {
			~BatchReset() { reset_mapping_batch(); }
		};
		[[maybe_unused]] BatchReset reset;

		int result = 0;
		auto state = batch.state;
		if (!state || state->retired.load(std::memory_order_acquire))
			return -EAGAIN;
		{
			std::unique_lock lock{state->mappings_mutex};
			if (state->retired.load(std::memory_order_acquire) ||
			    state->invalidation_epoch.load(std::memory_order_acquire) !=
			    batch.invalidation_epoch) {
				result = -EAGAIN;
			} else if (!batch_changes_published_mapping(
					   *state, batch.operations)) {
				/*
				 * Publishing a page which was previously absent cannot
				 * invalidate a running vCPU's positive-only TLB.  Its
				 * mapping miss either finishes before this lock is acquired
				 * or observes the new entry after the lock is released.
				 * Avoid stopping every sibling vCPU for ordinary first-touch
				 * demand paging.
				 */
				apply_batch_locked(*state, batch.operations);
			} else {
				lock.unlock();
				ExecutionQuiesce quiesce{state, batch.generation};
				lock.lock();
				if (state->retired.load(std::memory_order_acquire) ||
				    state->invalidation_epoch.load(
					    std::memory_order_acquire) !=
				    batch.invalidation_epoch) {
					result = -EAGAIN;
				} else {
					apply_batch_locked(*state, batch.operations);
				}
				lock.unlock();
			}
		}
		return result;
	}

	int lock_walker(std::uint64_t generation)
	{
		auto &active = active_walker();
		if (active.state)
			return -1;
		active.state = state_for(generation);
		active.address_space = this;
		active.generation = generation;
		return 0;
	}

	int unlock_walker(std::uint64_t generation)
	{
		auto &active = active_walker();
		if (!active.state || active.address_space != this ||
		    active.generation != generation)
			return -1;
		active.state.reset();
		active.address_space = nullptr;
		active.generation = 0;
		return 0;
	}

	ExecutionContext *create_execution_context(std::uint64_t generation)
	{
		auto state = state_for(generation);
		return new (std::nothrow) ExecutionContext{
			std::move(state), generation, 0, false};
	}

	/* Gives the thread's entered context back to the one `context` replaced.
	 * Contexts end innermost first, and no production path nests them; a
	 * caller that ends an outer context first leaves the chain pointing at a
	 * context that has ended, which is the caller's contract to keep. */
	static void unlink_entered(ExecutionContext *context) noexcept
	{
		if (entered_thread_context() == context)
			entered_thread_context() = context->previous_entered;
		context->previous_entered = nullptr;
	}

	static void destroy_execution_context(ExecutionContext *context)
	{
		if (context == nullptr)
			return;
		if (context->entered) {
			unlink_entered(context);
			execution_reader_end(*context->state);
		}
		delete context;
	}

	static int execution_context_begin(ExecutionContext *context,
					   std::uint64_t guest_pc)
	{
		if (context == nullptr || context->entered ||
		    context->state->retired.load(std::memory_order_acquire))
			return -1;
		/* Capture before entering the gate.  If a writer closes the gate in
		 * between, this reader either waits for it or observes its interrupt;
		 * it can never miss a shootdown that is waiting on this reader.
		 */
		context->execution_interrupts = context->state->execution_interrupts.load(
			std::memory_order_acquire);
		if (!execution_reader_begin(*context->state, context->generation,
					    guest_pc))
			return -1;
		context->entered = true;
		context->previous_entered = entered_thread_context();
		entered_thread_context() = context;
		return 0;
	}

	static int execution_context_end(ExecutionContext *context)
	{
		if (context == nullptr || !context->entered)
			return -1;
		unlink_entered(context);
		execution_reader_end(*context->state);
		context->entered = false;
		return 0;
	}

	[[nodiscard]] static const std::uint64_t *execution_context_code_sequence(
		const ExecutionContext *context) noexcept
	{
		static_assert(sizeof(std::atomic_uint64_t) == sizeof(std::uint64_t) &&
			std::atomic_uint64_t::is_always_lock_free,
			"translated code loads the sequence as a plain word");
		if (context == nullptr)
			return nullptr;
		return reinterpret_cast<const std::uint64_t *>(
			&context->state->code_changes);
	}

	static darwin_uml_static_sequences execution_context_sequences(
		const ExecutionContext *context)
	{
		if (context == nullptr)
			return {};
		return {
			context->state->mapping_changes.load(std::memory_order_acquire),
			context->state->code_changes.load(std::memory_order_acquire),
			context->state->execution_interrupts.load(
				std::memory_order_acquire) !=
				context->execution_interrupts,
		};
	}

#if UML_MEMORY_ALLOCATION_TESTING || UML_MEMORY_COUNT_AUDIT
	/* What a recount found: the records compared and the ones whose count of
	 * executable mappings differs from the executable words of the space's
	 * generations. */
	struct CountTally final {
		std::size_t records{};
		std::size_t mismatches{};
		/* What a mismatch looked like, for the first few: the record's count
		 * and the recount; the generations held; and the executable words
		 * that named no record. */
		std::size_t generations{};
		std::size_t unnamed{};
		std::size_t samples{};
		std::array<std::uint64_t, 4> sample_count{};
		std::array<std::uint64_t, 4> sample_expected{};
	};

	/* Recounts the executable guest-code words of every generation of this
	 * space per physical-page record and compares each record's count (a
	 * record that can no longer count is not compared). With `exact` every
	 * generation is held at once, so no change can fall between two of them,
	 * and the recount is a fact; a generation that cannot be held without
	 * waiting ends the audit with false. Without it the generations are held
	 * one at a time, for a caller that has stopped changing mappings. */
	[[nodiscard]] bool tally_executable_counts(CountTally &tally,
		bool exact) noexcept
	{
		try {
			std::vector<std::shared_ptr<GenerationState>> generations;
			/* An exact audit keeps the set of generations from changing:
			 * one made after the list was taken could count a mapping in
			 * that the recount never saw. It waits for no generation
			 * below, so holding this cannot make anyone wait on it for
			 * long. */
			std::shared_lock states_lock{states_mutex_};
			for (const auto &[generation, state] : states_) {
				(void)generation;
				generations.push_back(state);
			}
			if (!exact)
				states_lock.unlock();
			std::vector<std::shared_lock<WordSharedMutex>> held;
			if (exact) {
				held.reserve(generations.size());
				for (const auto &state : generations) {
					std::shared_lock lock{state->mappings_mutex,
						std::try_to_lock};
					if (!lock.owns_lock())
						return false;
					held.push_back(std::move(lock));
				}
			}
			std::unordered_map<const void *, std::uint64_t> recount;
			for (const auto &state : generations) {
				std::shared_lock<WordSharedMutex> lock;
				if (!exact)
					lock = std::shared_lock{state->mappings_mutex};
				(void)state->fill_index.scan(0,
					std::numeric_limits<std::uint64_t>::max(),
					[&](std::uint64_t page, std::uintptr_t word) {
						const auto fields = FillIndex::decode(word);
						if (!fields.guest_code || !fields.exec)
							return false;
						const auto mapping = decode_page(*state, page, word);
						auto *record = mapping ?
							mapping->backing->find_code_page(mapping->offset) :
							nullptr;
						++recount[record];
						return false;
					});
			}
			std::vector<std::shared_ptr<MappedBacking>> backings;
			{
				std::shared_lock lock{backing_mutex_};
				for (const auto &[key, weak] : backings_) {
					(void)key;
					if (auto backing = weak.lock())
						backings.push_back(std::move(backing));
				}
				for (const auto &[fd, weak] : descriptor_backings_) {
					(void)fd;
					if (auto backing = weak.lock())
						backings.push_back(std::move(backing));
				}
			}
			tally.generations += generations.size();
			if (const auto none = recount.find(nullptr); none != recount.end())
				tally.unnamed += none->second;
			tally.mismatches += recount.contains(nullptr) ? 1 : 0;
			std::unordered_map<const void *, bool> seen;
			for (const auto &backing : backings) {
				backing->for_each_code_page(
					[&](std::uint64_t, void *record) {
						if (seen.emplace(record, true).second == false)
							return;
						MappedBacking::RecordStamp stamp;
						(void)MappedBacking::load_code_page_stamp(
							record, stamp);
						if (stamp.never_share)
							return;
						++tally.records;
						const auto found = recount.find(record);
						const auto expected =
							found == recount.end() ? 0 : found->second;
						const auto counted =
							MappedBacking::executable_mappings(record);
						if (counted != expected) {
							if (tally.samples < tally.sample_count.size()) {
								tally.sample_count[tally.samples] = counted;
								tally.sample_expected[tally.samples] = expected;
								++tally.samples;
							}
							++tally.mismatches;
						}
					});
			}
			return true;
		} catch (...) {
			tally.mismatches = ~std::size_t{0};
			return true;
		}
	}
#endif

private:
	friend class AddressSpaceAccess;

	[[nodiscard]] static bool stays_in_generation(
		std::uint64_t address, std::uint64_t length) noexcept
	{
		if (length > std::numeric_limits<std::uint64_t>::max() - address)
			return false;
		return AddressLayout::classify(address, length) !=
			AddressDomain::invalid;
	}

	static std::uint64_t generation_for(std::uint64_t address,
					    std::uint64_t generation) noexcept
	{
		return AddressLayout::is_guest(address) ? generation : 0;
	}

	GenerationReference reference_for(std::uint64_t generation)
	{
		auto &active = active_thread_generation();
		if (active.address_space == this &&
		    active.generation == generation && active.state &&
		    !active.state->retired.load(std::memory_order_acquire))
			return {{}, active.state.get()};
		auto owner = state_for(generation);
		auto *state = owner.get();
		return {std::move(owner), state};
	}

	std::shared_ptr<MappedBacking> backing_for(int fd)
	{
		auto &batch = mapping_batch();
		if (batch.depth != 0 && batch.address_space == this) {
			if (auto found = batch.backings.find(fd);
			    found != batch.backings.end())
				return found->second;
		}
		{
			/* An expired entry stays until the slow path below replaces
			 * it; erasing it here would need the lock exclusively. */
			std::shared_lock lock{backing_mutex_};
			if (auto found = descriptor_backings_.find(fd);
			    found != descriptor_backings_.end()) {
				if (auto backing = found->second.lock())
					return backing;
			}
		}
		struct stat status {};
		if (fstat(fd, &status) != 0)
			return {};
		const BackingKey key{static_cast<std::uint64_t>(status.st_dev),
				     static_cast<std::uint64_t>(status.st_ino)};
		/* Darwin POSIX shared-memory descriptors report st_dev == 0 and
		 * st_ino == 0 for every distinct object.  Such an inode cannot identify
		 * a backing across descriptors: Venus, for example, exports its ring and
		 * reply buffers as separate shm objects with that same stat tuple.  Keep
		 * descriptor-local reuse, but never merge an inode-less object with a
		 * different descriptor. */
		const bool stable_identity = key.inode != 0;
		std::lock_guard lock{backing_mutex_};
		if (stable_identity) {
			if (auto found = backings_.find(key); found != backings_.end()) {
				if (auto backing = found->second.lock()) {
					descriptor_backings_.insert_or_assign(fd, backing);
					if (batch.depth != 0 && batch.address_space == this)
						batch.backings.insert_or_assign(fd, backing);
					return backing;
				}
			}
		}
#if DARWIN_UML_X86_64_GUEST
		auto backing = MappedBacking::create(fd, status, frame_domain_);
#else
		auto backing = MappedBacking::create(fd, status);
#endif
		if (backing) {
			if (stable_identity)
				backings_.insert_or_assign(key, backing);
			descriptor_backings_.insert_or_assign(fd, backing);
			if (batch.depth != 0 && batch.address_space == this)
				batch.backings.insert_or_assign(fd, backing);
		}
		return backing;
	}

	std::shared_ptr<GenerationState> state_for_locked(
		std::uint64_t generation)
	{
		if (const auto found = states_.find(generation);
		    found != states_.end()) {
			if (!found->second->retired.load(std::memory_order_acquire))
				return found->second;
			states_.erase(found);
		}
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::generation);
#if DARWIN_UML_X86_64_GUEST
		auto state = std::make_shared<GenerationState>(
			heap_, frame_domain_, generation);
#else
		auto state = std::make_shared<GenerationState>(heap_);
#endif
		state->address_space_lifetime = lifetime_;
		states_.emplace(generation, state);
		return state;
	}

	std::shared_ptr<GenerationState> state_for(std::uint64_t generation)
	{
		{
			std::shared_lock lock{states_mutex_};
			if (const auto found = states_.find(generation);
			    found != states_.end() &&
			    !found->second->retired.load(std::memory_order_acquire))
				return found->second;
		}
		std::lock_guard lock{states_mutex_};
		return state_for_locked(generation);
	}

	void apply_batch_locked(GenerationState &state,
		std::span<const MappingOperation> operations)
	{
		constexpr std::size_t maximum_plan_reserve = 64 * 1024;
		const auto add_reserve_pages = [&](std::size_t &total,
						 const std::uint64_t pages) {
			if (pages >= maximum_plan_reserve - total)
				total = maximum_plan_reserve;
			else
				total += static_cast<std::size_t>(pages);
		};
		SlotSettler settler{state};
		std::size_t touched_pages = 0;
		std::size_t possible_pages = std::min(
			state.mapped_pages, maximum_plan_reserve);
		for (const auto &operation : operations) {
			const auto operation_pages = operation.length / page_size;
			add_reserve_pages(touched_pages, operation_pages);
			if (operation.map)
				add_reserve_pages(possible_pages, operation_pages);
		}
		const auto plan_reserve = std::min(touched_pages, possible_pages);
		/* The plan lives in the thread's scratch: it is dropped whole when
		 * the batch ends, instead of leaving its largest vector in the
		 * default malloc zone. */
		ScratchScope scratch;
		const ScratchAllocator<char> scratch_allocator{scratch.arena()};
		std::vector<PlannedMappingPage, ScratchAllocator<PlannedMappingPage>>
			pages{scratch_allocator};
		std::unordered_map<std::uint64_t, std::size_t, GuestPageHash,
			std::equal_to<std::uint64_t>,
			ScratchAllocator<std::pair<const std::uint64_t, std::size_t>>>
			page_indices{scratch_allocator};
		std::vector<MappingChangePlan, ScratchAllocator<MappingChangePlan>>
			changes{scratch_allocator};
		/* The operation count is a poor capacity estimate: one coalesced TLB
		 * range can cover thousands of pages. Reserve a checked upper bound on
		 * planned pages, capped to avoid a hostile overlapping batch forcing an
		 * otherwise unnecessary address-space-sized allocation. */
		pages.reserve(plan_reserve);
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::batch_index);
		page_indices.reserve(plan_reserve);
		changes.reserve(operations.size());
		const auto page_for = [&](const std::uint64_t address)
			-> PlannedMappingPage & {
			if (const auto found = page_indices.find(address);
			    found != page_indices.end())
				return pages[found->second];
			const auto original = find_page(state, address);
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch_index);
			page_indices.emplace(address, pages.size());
			pages.push_back({address, original, original, nullptr,
				BackingSlots::overflow});
			return pages.back();
		};

		for (const auto &operation : operations) {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::batch_commit);
			bool changed = false;
			/* A translation can exist only for what the page held before
			 * the batch: the vCPUs see the batch whole, so a page it maps
			 * and unmaps again leaves none to retire. A page an earlier
			 * operation of the batch already changed is judged again
			 * against its original, which at worst names it twice. */
			CodeSpan code;
			/* The slot of the backing this operation maps, claimed when the
			 * first page needs it. */
			std::optional<unsigned> operation_slot;
			const auto clear_page = [&](PlannedMappingPage &page) {
				if (!page.final)
					return;
				changed = true;
				note_code_change(code, page.address, page.original,
					std::nullopt);
				page.final.reset();
			};
			const auto operation_pages = operation.length / page_size;
			const auto candidate_pages =
				state.mapped_pages >
					std::numeric_limits<std::size_t>::max() - pages.size() ?
				std::numeric_limits<std::size_t>::max() :
				state.mapped_pages + pages.size();
			if (!operation.map && operation_pages > candidate_pages) {
				const auto end = operation.address + operation.length;
				/* A broad unmap is commonly a sparse exit_mmap() range. It can
				 * only change pages which were already published or mapped by an
				 * earlier operation in this batch; do not materialize every hole. */
				(void)state.fill_index.scan(operation.address, end,
					[&](std::uint64_t address, std::uintptr_t) {
						clear_page(page_for(address));
						return false;
					});
				for (auto &page : pages)
					if (page.address >= operation.address &&
					    page.address < end)
						clear_page(page);
			} else {
				for (std::uint64_t position = 0;
				     position < operation.length;
				     position += page_size) {
					const auto page_address =
						operation.address + position;
					if (!operation.map) {
						/* An unmap changes only a page the
						 * generation or this batch holds. Most
						 * name pages nothing mapped, and a 4 KiB
						 * guest sends one per page, so plan none
						 * for them. */
						if (!page_indices.contains(page_address) &&
						    state.fill_index.lookup(page_address) == 0)
							continue;
						clear_page(page_for(page_address));
						continue;
					}
					auto &page = page_for(page_address);
					const std::optional<PageMapping> next{PageMapping{
						operation.mapping.backing.get(),
						operation.mapping.offset + position,
						operation.mapping.fd,
						operation.mapping.protection,
						operation.mapping.guest_code,
						operation.mapping.guest_writable,
						operation.mapping.guest_immutable}};
					if (same_mapping(page.final, next))
						continue;
					if (!operation_slot)
						operation_slot = state.backings.claim(
							operation.mapping.backing,
							operation.mapping.fd,
							operation.mapping.guest_immutable);
					changed = true;
					note_code_change(code, page.address,
						page.original, next);
					page.final = next;
					page.owner = &operation.mapping.backing;
					page.slot = *operation_slot;
				}
			}
			if (changed)
				changes.push_back({operation.address,
					operation.length, code});
		}
		if (changes.empty())
			return;

		reserve_planned_pages(state, pages);
		PendingMappingChanges pending{state, changes};
		commit_planned_pages(state, pages);
		pending.commit();
	}

	void map_locked(GenerationState &state, std::uint64_t address,
		std::uint64_t length, const Mapping &mapping)
	{
		SlotSettler settler{state};
		ScratchScope scratch;
		std::vector<PlannedMappingPage, ScratchAllocator<PlannedMappingPage>>
			pages{ScratchAllocator<PlannedMappingPage>{scratch.arena()}};
		pages.reserve(static_cast<std::size_t>(length / page_size));
		CodeSpan code;
		std::optional<unsigned> slot;
		for (std::uint64_t position = 0; position < length;
		     position += page_size) {
			const auto page_address = address + position;
			const auto previous = find_page(state, page_address);
			const std::optional<PageMapping> next{PageMapping{
				mapping.backing.get(), mapping.offset + position,
				mapping.fd, mapping.protection, mapping.guest_code,
				mapping.guest_writable, mapping.guest_immutable}};
			if (same_mapping(previous, next))
				continue;
			if (!slot)
				slot = state.backings.claim(mapping.backing, mapping.fd,
					mapping.guest_immutable);
			note_code_change(code, page_address, previous, next);
			pages.push_back({page_address, previous, next,
				&mapping.backing, *slot});
		}
		if (pages.empty())
			return;
		const std::array changes{
			MappingChangePlan{address, length, code}};
		reserve_planned_pages(state, pages);
		PendingMappingChanges pending{state, changes};
		commit_planned_pages(state, pages);
		pending.commit();
	}

	[[nodiscard]] static bool batch_changes_published_mapping(
		const GenerationState &state,
		std::span<const MappingOperation> operations)
	{
		for (const auto &operation : operations) {
			const auto differs = [&](std::uint64_t address,
						 std::uintptr_t word) {
				if (!operation.map)
					return true;
				const auto mapping = decode_page(state, address, word);
				if (!mapping)
					return true;
				const auto position = address - operation.address;
				return mapping->backing != operation.mapping.backing.get() ||
					mapping->fd != operation.mapping.fd ||
					mapping->offset != operation.mapping.offset + position ||
					mapping->protection != operation.mapping.protection ||
					mapping->guest_code != operation.mapping.guest_code ||
					mapping->guest_writable !=
						operation.mapping.guest_writable ||
					mapping->immutable !=
						operation.mapping.guest_immutable;
			};
			if (state.fill_index.scan(operation.address,
				    operation.address + operation.length, differs))
				return true;
		}
		return false;
	}

	[[nodiscard]] bool unmap_locked(GenerationState &state,
		std::uint64_t address,
		std::uint64_t length)
	{
		/* exit_mmap() unmaps the whole user range of a generation that has
		 * few pages mapped, often none: the index walks only the subtrees that
		 * hold a page, whatever the width of the range. */
		const auto end = address + length;
		bool changed = false;
		CodeSpan code;
		(void)state.fill_index.scan(address, end,
			[&](std::uint64_t page, std::uintptr_t word) {
				changed = true;
				const auto fields = FillIndex::decode(word);
				if (fields.guest_code && fields.exec) {
					code.add(page);
					if (const auto mapping = decode_page(state, page, word))
						mapping->backing->retire_code_page(mapping->offset);
				}
				return false;
			});
		if (!changed)
			return false;
		const std::array changes{
			MappingChangePlan{address, length, code}};
		PendingMappingChanges pending{state, changes};
		state.fill_index.erase_range(address, end,
			[&state](std::uint64_t page, std::uintptr_t word) noexcept {
				count_executable_departure(state, page, word);
#if DARWIN_UML_X86_64_GUEST
				FrameAccounting::release_word(state, page, word);
#endif
				state.backings.remove(FillIndex::decode(word).slot, page);
				--state.mapped_pages;
			});
		state.backings.settle();
		pending.commit();
		return true;
	}

	/* A code synchronization at `address` in `generation`, whose state is
	 * `state`: the frame's stamp moves, the generation publishes the page, and
	 * every other generation that translated the frame since its last
	 * publication hears of a code change. The frame is the one the writer's own
	 * view maps, whatever that view permits: IC IVAU names the physical
	 * address, so a writable alias that cannot execute reaches the frame the
	 * executable aliases hold. The caller's fence (a code synchronization
	 * issues one after the bytes land) orders the holder words after them. */
	static bool synchronize_code_in(GenerationState &state,
		std::uint64_t generation, std::uint64_t address,
		code_holders::HolderList &others,
		std::atomic<std::uint64_t> &in_flight, bool &holds)
	{
		const auto page = address & ~(page_size - 1);
		std::shared_lock lock{state.mappings_mutex};
		const auto mapping = find_page(state, page);
		if (!mapping)
			return !AddressLayout::is_guest(page);
		step_stamp(*mapping);
		if (AddressLayout::is_guest(page))
			holds = take_holders(*mapping, generation, others, in_flight,
				false);
		return true;
	}

	bool synchronize_code(GenerationState &state, std::uint64_t generation,
		std::uint64_t address)
	{
		code_holders::HolderList others;
		bool holds = false;
		const bool found = synchronize_code_in(state, generation, address,
			others, holder_takes_in_flight_, holds);
		/* The synchronizing view is not the only one the generation may
		 * hold: a code synchronization names the physical address, so the
		 * write may have gone through a view that cannot execute while the
		 * frame runs, or was verified, at another address. A generation that
		 * holds the frame (or whose list goes to every generation) hears of
		 * the change at every address. */
		if (holds || others.every())
			::uml::memory::publish_code_change(state, 0, 0);
		else
			::uml::memory::publish_code_change(
				state, address & ~(page_size - 1), page_size);
		publish_to_holders(generation, others);
		return found;
	}

	/* Moves the stamp of the frame `mapping` names, whatever the publisher's
	 * own view of it permits: the record exists where some mapping executes
	 * the frame, and a verdict filed for the frame's bytes must not outlive
	 * the write. The fence orders the step before the holder word is read
	 * (take_holders), and pairs with the one a reader makes after it registers
	 * in the word and before it reads the stamp (register_holder_slow;
	 * Binder::verify_page): either the word read here sees the registration
	 * and the reader is told, or the stamp it reads shows the step. Without
	 * it the load of the word (acquire here, which the compiler may emit as
	 * an RCpc load) can be satisfied before the step is visible, and a reader
	 * that is neither told nor shown the step runs the old bytes. The fence is
	 * a full barrier (dmb ish) whatever the host's code generation is for the
	 * loads and the read-modify-writes either side of it. The bytes are
	 * ordered before the word by the caller's own fence, which is all the
	 * frame's holder protocol needs for the engines, which read no stamp. A
	 * frame with no record has no stamp to step, and a record made later has
	 * a new identity, so no verdict of that frame exists to outlive the
	 * write. */
	static void step_stamp(const PageMapping &mapping) noexcept
	{
		auto *record = mapping.guest_code ?
			mapping.backing->code_page_handle(mapping.offset) :
			mapping.backing->find_code_page(mapping.offset);
		if (record == nullptr)
			return;
		MappedBacking::invalidate_code_page(record);
		std::atomic_thread_fence(std::memory_order_seq_cst);
	}

	/* Takes the holder word of the frame `mapping` names and adds the
	 * generations it held, but `generation`'s own, to `others`. A word that
	 * is empty or names only the writer's generation is left alone: the common
	 * frame, held by its writer alone, costs one load and no exchange. For a
	 * holder of the writer's mapping lock; nothing here waits. `kernel` says
	 * the take is a kernel write's, which the counters keep apart. Answers
	 * whether `generation` holds the frame itself: its backing's holders are
	 * unknown, or the word read every generation or named it.
	 *
	 * An empty word says only that nobody has registered since a publisher
	 * emptied it: that publisher may not have published yet. So whoever
	 * empties a word is counted in `in_flight` until its list is destroyed
	 * (after its publication), and a take that finds more than its own share
	 * counted publishes to every generation: the readers the unfinished take
	 * consumed are among them. The word is read before the counter, and its
	 * emptying is ordered after the count, so a word read empty by a later
	 * publisher comes with the count of the one that emptied it. */
	static bool take_holders(const PageMapping &mapping,
		std::uint64_t generation, code_holders::HolderList &others,
		std::atomic<std::uint64_t> &in_flight, bool kernel) noexcept
	{
		return take_holders(*mapping.backing, mapping.offset, generation,
			others, in_flight, kernel);
	}

	/* The same for the frame at `offset` of `backing`, which a page of the
	 * generation need not map. */
	static bool take_holders(const MappedBacking &backing,
		std::uint64_t offset, std::uint64_t generation,
		code_holders::HolderList &others,
		std::atomic<std::uint64_t> &in_flight, bool kernel) noexcept
	{
		if (backing.holders_unknown()) {
			others.add_every();
			return true;
		}
		bool own = false;
		if (auto *word = backing.find_holder_word(offset)) {
			const auto slot = code_holders::slot_of(generation);
			auto held = word->load(std::memory_order_acquire);
			own = code_holders::holds(held, slot);
			if (held != code_holders::none &&
			    !code_holders::holds_only(held, slot)) {
				others.begin_take(in_flight);
				code_holders::count(
					&code_holders::detail::CounterStore::exchanges);
				if (kernel)
					code_holders::count(&code_holders::detail::
						CounterStore::kernel_exchanges);
				held = word->exchange(code_holders::none,
					std::memory_order_acq_rel);
				own = code_holders::holds(held, slot);
				others.add(held, slot);
			}
		}
		if (!others.every() &&
		    in_flight.load(std::memory_order_acquire) > others.own_take()) {
			others.add_every();
			code_holders::count(
				&code_holders::detail::CounterStore::overlap_widenings);
		}
		return own;
	}

	/* A whole-space code change in each generation `holders` names, or in
	 * every live generation but `except`, the writer's own, which its caller
	 * published. The caller's list stays counted as in flight until this
	 * returns (take_holders). A generation that is gone is skipped, and none is made: only
	 * a find, so a dead holder does not come back. Never under a mapping lock;
	 * it takes the table of generations shared, and everything it does there
	 * is an atomic update. A whole-space change costs a reader no more than
	 * a page's: any change in a generation makes every translation of it
	 * revalidate its bytes at its next use. */
	void publish_to_holders(std::uint64_t except,
		const code_holders::HolderList &holders)
	{
		if (holders.empty())
			return;
		/* Test variant only: where a test holds a publisher that has taken
		 * its words and not yet published. */
		UML_MEMORY_ALLOCATION_CHECKPOINT(
			testing::AllocationSite::holder_publication);
		std::uint64_t published = 0;
		{
			std::shared_lock lock{states_mutex_};
			if (holders.every()) {
				for (const auto &[generation, state] : states_) {
					if (generation == except ||
					    state->retired.load(std::memory_order_acquire))
						continue;
					::uml::memory::publish_code_change(*state, 0, 0);
					++published;
				}
			} else {
				for (const auto generation : holders.generations()) {
					const auto found = states_.find(generation);
					if (found == states_.end() ||
					    found->second->retired.load(
						    std::memory_order_acquire))
						continue;
					::uml::memory::publish_code_change(
						*found->second, 0, 0);
					++published;
				}
			}
		}
		using code_holders::detail::CounterStore;
		code_holders::count(&CounterStore::publications);
		if (holders.every())
			code_holders::count(&CounterStore::all_publications);
		code_holders::count(&CounterStore::generations_published, published);
	}

	/* Shared with every generation: the last holder gives the pages back. */
	std::shared_ptr<DomainHeap> heap_{std::make_shared<DomainHeap>()};
#if DARWIN_UML_X86_64_GUEST
	/* The alias table of the frame accounting and the heap of its metadata
	 * (frame_accounting.hpp); held by every state and backing of the space. */
	std::shared_ptr<FrameAccountingDomain> frame_domain_{
		std::make_shared<FrameAccountingDomain>(heap_)};
#endif
	mutable WordSharedMutex states_mutex_;
	WordSharedMutex backing_mutex_;
	/* Publishers that have emptied a holder word and not finished publishing
	 * to the generations it named (code_holders::HolderList). */
	std::atomic<std::uint64_t> holder_takes_in_flight_{0};
	std::unordered_map<std::uint64_t, std::shared_ptr<GenerationState>> states_;
	std::unordered_map<BackingKey, std::weak_ptr<MappedBacking>, BackingKeyHash>
		backings_;
	std::unordered_map<int, std::weak_ptr<MappedBacking>> descriptor_backings_;
	std::shared_ptr<AddressSpaceLifetime> lifetime_{
		std::make_shared<AddressSpaceLifetime>()};
	std::atomic_bool retired_{};

#if UML_MEMORY_ALLOCATION_TESTING && DARWIN_UML_X86_64_GUEST
public:
	/* The test hooks of the frame accounting (frame_accounting.hpp). */
	[[nodiscard]] std::shared_ptr<GenerationState> state_for_testing(
		std::uint64_t generation)
	{
		std::shared_lock lock{states_mutex_};
		const auto found = states_.find(generation);
		return found == states_.end() ? nullptr : found->second;
	}

	[[nodiscard]] std::shared_ptr<MappedBacking> backing_for_testing(int fd)
	{
		struct stat status {};
		if (fstat(fd, &status) != 0)
			return {};
		std::shared_lock lock{backing_mutex_};
		if (auto found = descriptor_backings_.find(fd);
		    found != descriptor_backings_.end())
			return found->second.lock();
		return {};
	}

	[[nodiscard]] testing::FrameWriterAudit audit_frame_writers_for_testing()
	{
		using Site = testing::FrameWriterSite;
		testing::FrameWriterAudit audit;
		std::vector<std::shared_ptr<GenerationState>> states;
		std::vector<std::shared_ptr<MappedBacking>> backings;
		{
			std::shared_lock lock{states_mutex_};
			for (const auto &[generation, state] : states_) {
				(void)generation;
				states.push_back(state);
			}
		}
		{
			std::shared_lock lock{backing_mutex_};
			const auto add = [&](const std::weak_ptr<MappedBacking> &weak) {
				if (auto backing = weak.lock();
				    backing && std::find(backings.begin(), backings.end(),
					    backing) == backings.end())
					backings.push_back(std::move(backing));
			};
			for (const auto &[key, weak] : backings_) {
				(void)key;
				add(weak);
			}
			for (const auto &[fd, weak] : descriptor_backings_) {
				(void)fd;
				add(weak);
			}
		}
		std::vector<Site> recount;
		for (const auto &state : states) {
			std::shared_lock lock{state->mappings_mutex};
			(void)state->fill_index.scan(0, ~std::uint64_t{0},
				[&](std::uint64_t page, std::uintptr_t word) {
					const auto fields = FillIndex::decode(word);
					if (!fields.write || !FrameAccounting::tracks(page))
						return false;
					const auto entry = state->backings.entry(fields.slot, page);
					if (entry.backing == nullptr)
						return false;
					const auto offset = fields.host -
						reinterpret_cast<std::uintptr_t>(entry.backing->base());
					recount.push_back({entry.backing, offset / page_size,
						state->accounting.alias, page});
					return false;
				});
		}
		std::vector<Site> held;
		for (const auto &backing : backings) {
			std::vector<std::uint64_t> frames;
			backing->for_each_writer_word(
				[&](std::uint64_t offset, std::uint64_t) {
					frames.push_back(offset);
				});
			for (const auto offset : frames) {
				std::vector<WriterLocation> locations;
				if (!FrameAccounting::locations_for_testing(
					    *backing, offset, locations))
					++audit.unknown_frames;
				for (const auto &location : locations)
					held.push_back({backing.get(), offset / page_size,
						location.alias, location.page});
			}
		}
		std::sort(recount.begin(), recount.end());
		std::sort(held.begin(), held.end());
		audit.state_locations = recount.size();
		audit.frame_locations = held.size();
		audit.equal = recount == held && audit.unknown_frames == 0;
		return audit;
	}

	[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>>
	frame_writers_for_testing(int fd, std::uint64_t offset)
	{
		std::vector<std::pair<std::uint64_t, std::uint64_t>> writers;
		const auto backing = backing_for_testing(fd);
		if (!backing)
			return writers;
		std::vector<WriterLocation> locations;
		(void)FrameAccounting::locations_for_testing(
			*backing, offset, locations);
		std::shared_lock lock{states_mutex_};
		for (const auto &location : locations)
			for (const auto &[generation, state] : states_)
				if (state->accounting.alias == location.alias)
					writers.emplace_back(generation, location.page);
		return writers;
	}
#endif
};

/* Holds space for one operation. It either owns a reference or borrows the
 * one in its thread's ThreadAddressSpaceCache; a borrowing access keeps that
 * cache entry pinned until it is destroyed. */
class AddressSpaceAccess final {
public:
	AddressSpaceAccess() noexcept = default;

	AddressSpaceAccess(AddressSpace *space,
		std::shared_ptr<AddressSpace> owned,
		ThreadAddressSpaceCache &cache)
		: owned_{std::move(owned)}, space_{space},
		  lender_{owned_ != nullptr ? nullptr : &cache},
		  slot_{&active_access_slot()}
	{
		if (lender_ != nullptr)
			++lender_->borrowed_accesses;
		slot_->space.store(space_, std::memory_order_seq_cst);
		if (!space_->retired_.load(std::memory_order_seq_cst))
			return;
		slot_->space.store(nullptr, std::memory_order_seq_cst);
		slot_ = nullptr;
		release();
		errno = ESHUTDOWN;
	}

	~AddressSpaceAccess()
	{
		if (slot_ != nullptr)
			slot_->space.store(nullptr, std::memory_order_seq_cst);
		release();
	}

	AddressSpaceAccess(AddressSpaceAccess &&other) noexcept
		: owned_{std::move(other.owned_)},
		  space_{std::exchange(other.space_, nullptr)},
		  lender_{std::exchange(other.lender_, nullptr)},
		  slot_{std::exchange(other.slot_, nullptr)}
	{
	}
	AddressSpaceAccess &operator=(AddressSpaceAccess &&) noexcept = delete;

	AddressSpaceAccess(const AddressSpaceAccess &) = delete;
	AddressSpaceAccess &operator=(const AddressSpaceAccess &) = delete;

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return space_ != nullptr;
	}

	[[nodiscard]] AddressSpace &operator*() const noexcept
	{
		return *space_;
	}

	[[nodiscard]] AddressSpace *operator->() const noexcept
	{
		return space_;
	}

private:
	/* Runs after the slot is cleared, so a concurrent retire() can never be
	 * left waiting on a slot whose address space has already gone. */
	void release() noexcept
	{
		space_ = nullptr;
		owned_.reset();
		if (lender_ != nullptr)
			--std::exchange(lender_, nullptr)->borrowed_accesses;
	}

	std::shared_ptr<AddressSpace> owned_;
	AddressSpace *space_{};
	ThreadAddressSpaceCache *lender_{};
	ActiveAccessSlot *slot_{};
};

constexpr std::uint64_t legacy_domain = 1;

std::atomic<DomainProvider> &domain_provider()
{
	static std::atomic<DomainProvider> provider{};
	return provider;
}

std::atomic<DomainRetireObserver> &domain_retire_observer()
{
	static std::atomic<DomainRetireObserver> observer{};
	return observer;
}

#if UML_MEMORY_ALLOCATION_TESTING
std::atomic<testing::SoftwareAddressSpaceAcquiredHook> &
software_address_space_acquired_hook()
{
	static std::atomic<testing::SoftwareAddressSpaceAcquiredHook> hook{};
	return hook;
}
#endif

[[nodiscard]] bool resolve_current_domain_impl(std::uint64_t &domain) noexcept
{
	const auto provider = domain_provider().load(std::memory_order_acquire);
	if (provider == nullptr) {
		domain = legacy_domain;
		return true;
	}
	return provider(domain) && domain != 0;
}

/* Every software-TLB miss in both engines resolves the address space through
 * current(). Before the engine split that was a process-global instance reached
 * by a plain reference, so a miss cost one lock: the shared_lock on the
 * generation's mappings_mutex inside translate(). Domain support made it a
 * registry lookup, which adds a second acquisition of a process-global
 * shared_mutex plus a hash find to every miss on every vCPU thread - and
 * libc++'s shared_mutex takes a real internal std::mutex in lock_shared(), so
 * "shared" readers serialize on one cacheline rather than proceeding in
 * parallel. That cost scales with vCPU count, which is why it shows up in the
 * multi-core phase.
 *
 * The resolved owner is therefore cached per thread. activate() already keeps a
 * thread-local generation slot of exactly this shape. Validity is one relaxed
 * comparison against the global retirement sequence, which retire_domain()
 * advances before it touches the registry, so a cache hit proves no retirement
 * has begun since the entry was published. A hit lends the thread's own
 * reference (see ThreadAddressSpaceCache); a resolution made while a loan is
 * outstanding leaves the cache alone and returns an owning reference. */
struct ResolvedOwner final {
	AddressSpace *space{};
	/* Empty when space is lent from the thread's cache. */
	std::shared_ptr<AddressSpace> owned;
};

class AddressSpaceRegistry final {
public:
	[[nodiscard]] ResolvedOwner current(
		ThreadAddressSpaceCache &cache) noexcept
	{
		for (;;) {
			const auto retirement_sequence =
				domain_retirement_sequence_storage().load(
					std::memory_order_acquire);
			std::uint64_t domain;
			if (!resolve_current_domain_impl(domain)) {
				errno = ESRCH;
				return {};
			}
			auto &resolved = cache.resolved;
			/* A lent reference must outlive its access, so an outstanding
			 * loan freezes the entry and a miss resolves without it. */
			const bool cacheable = cache.borrowed_accesses == 0;
			if (resolved.owner != nullptr) {
				if (resolved.retirement_sequence == retirement_sequence &&
				    resolved.domain == domain)
					return ResolvedOwner{resolved.owner.get(), {}};
				/* Any retirement invalidates the entry, which also releases
				 * this thread's reference to a space that may be the retired
				 * one. */
				if (resolved.retirement_sequence != retirement_sequence &&
				    cacheable)
					resolved = {};
			}
			/* Only ever reached after the sequence has been re-read and found
			 * unchanged, so the published entry is one no retirement has
			 * touched. */
			const auto publish =
				[&](std::shared_ptr<AddressSpace> space) noexcept {
					if (!cacheable) {
						auto *raw = space.get();
						return ResolvedOwner{raw, std::move(space)};
					}
					resolved.domain = domain;
					resolved.retirement_sequence = retirement_sequence;
					resolved.owner = std::move(space);
					return ResolvedOwner{resolved.owner.get(), {}};
				};
			try {
				{
					std::shared_lock lock{mutex_};
					if (const auto found = spaces_.find(domain);
					    found != spaces_.end()) {
						if (domain_retirement_sequence_storage().load(
							    std::memory_order_acquire) !=
							    retirement_sequence)
							continue;
						return publish(found->second);
					}
				}
				std::lock_guard lock{mutex_};
				if (domain_retirement_sequence_storage().load(
					    std::memory_order_acquire) != retirement_sequence)
					continue;
				if (const auto found = spaces_.find(domain);
				    found != spaces_.end())
					return publish(found->second);
				auto space = std::make_shared<AddressSpace>();
				/* Retirement advances the sequence before acquiring mutex_. A resolver
				 * that captured a domain earlier therefore cannot install state after
				 * retirement. If retirement starts after this check, it waits for this
				 * publication and removes the entry. This needs no permanent tombstone
				 * or second provider lookup on the ordinary path. */
				if (domain_retirement_sequence_storage().load(
					    std::memory_order_acquire) != retirement_sequence)
					continue;
				spaces_.emplace(domain, space);
				return publish(std::move(space));
			} catch (const std::bad_alloc &) {
				errno = ENOMEM;
				return {};
			} catch (...) {
				errno = EIO;
				return {};
			}
		}
	}

	void retire(std::uint64_t domain) noexcept
	{
		std::shared_ptr<AddressSpace> retired;
		{
			std::lock_guard lock{mutex_};
			if (const auto found = spaces_.find(domain);
			    found != spaces_.end()) {
				retired = std::move(found->second);
				spaces_.erase(found);
			}
		}
		if (retired)
			retired->retire();
	}

#if UML_MEMORY_ALLOCATION_TESTING
	[[nodiscard]] std::size_t size_for_testing() const noexcept
	{
		std::shared_lock lock{mutex_};
		return spaces_.size();
	}
#endif

#if UML_MEMORY_ALLOCATION_TESTING || UML_MEMORY_COUNT_AUDIT
	/* The recount of every space; false when `exact` and a space could not be
	 * held whole. */
	[[nodiscard]] bool tally_executable_counts(
		AddressSpace::CountTally &tally, bool exact) const noexcept
	{
		std::shared_lock lock{mutex_};
		for (const auto &[domain, space] : spaces_) {
			(void)domain;
			if (!space->tally_executable_counts(tally, exact))
				return false;
		}
		return true;
	}
#endif

	void forget_descriptor(int fd)
	{
		std::shared_lock lock{mutex_};
		for (const auto &[domain, space] : spaces_) {
			(void)domain;
			space->forget_descriptor(fd);
		}
	}

private:
	mutable std::shared_mutex mutex_;
	std::unordered_map<std::uint64_t, std::shared_ptr<AddressSpace>> spaces_;
};

std::atomic<AddressSpaceRegistry *> published_address_space_registry{};
std::atomic_flag address_space_registry_initialization = ATOMIC_FLAG_INIT;

[[nodiscard]] AddressSpaceRegistry *existing_address_space_registry() noexcept
{
	return published_address_space_registry.load(std::memory_order_acquire);
}

AddressSpaceRegistry *address_space_registry() noexcept
{
	/* Context teardown can run during static destruction. Keep the registry at
	 * process lifetime so a late Context destructor never calls through
	 * an already destroyed mutex or container. Initialization is retryable: a
	 * transient allocation failure must not permanently cache a null registry. */
	return process_singleton(published_address_space_registry,
		address_space_registry_initialization,
		[]() noexcept -> AddressSpaceRegistry * {
			try {
				UML_MEMORY_ALLOCATION_CHECKPOINT(
					testing::AllocationSite::software_domain_registry);
				return new (std::nothrow) AddressSpaceRegistry;
			} catch (...) {
				return nullptr;
			}
		});
}

[[nodiscard]] AddressSpaceAccess address_space() noexcept
{
	try {
		auto &cache = thread_address_space_cache();
		prune_retired_thread_state(cache);
		errno = 0;
		auto *registry = address_space_registry();
		if (registry == nullptr) {
			errno = ENOMEM;
			return {};
		}
		auto owner = registry->current(cache);
		if (owner.space == nullptr)
			return {};
#if UML_MEMORY_ALLOCATION_TESTING
		if (const auto hook = software_address_space_acquired_hook().load(
			    std::memory_order_acquire); hook != nullptr) {
			/* The hook may resolve again, so a lent space stays pinned. */
			const bool lent = owner.owned == nullptr;
			cache.borrowed_accesses += lent;
			hook();
			cache.borrowed_accesses -= lent;
		}
#endif
		return AddressSpaceAccess{
			owner.space, std::move(owner.owned), cache};
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
	} catch (...) {
		errno = EIO;
	}
	return {};
}

} // namespace

void set_domain_provider(DomainProvider provider) noexcept
{
	domain_provider().store(provider, std::memory_order_release);
}

bool resolve_current_domain(std::uint64_t &domain) noexcept
{
	return resolve_current_domain_impl(domain);
}

std::uint64_t domain_retirement_sequence() noexcept
{
	return domain_retirement_sequence_storage().load(
		std::memory_order_acquire);
}

#if UML_MEMORY_ALLOCATION_TESTING
namespace testing {

std::size_t software_domain_count() noexcept
{
	auto *registry = existing_address_space_registry();
	return registry != nullptr ? registry->size_for_testing() : 0;
}

void set_software_address_space_acquired_hook(
	SoftwareAddressSpaceAcquiredHook hook) noexcept
{
	software_address_space_acquired_hook().store(
		hook, std::memory_order_release);
}

std::size_t audit_executable_counts() noexcept
{
	auto *registry = existing_address_space_registry();
	AddressSpace::CountTally tally;
	if (registry != nullptr)
		(void)registry->tally_executable_counts(tally, false);
	return tally.mismatches;
}

} // namespace testing
#endif

#if UML_MEMORY_COUNT_AUDIT
namespace {
/* What scripts/check-no-aot-diagnostics.py finds in a linked binary whose
 * symbols were stripped: only the compilation that has the audit holds it. */
[[gnu::used]] const char count_audit_build_marker[] =
	"UML_MEMORY_COUNT_AUDIT_BUILD";

std::atomic<std::uint64_t> &anomaly_total() noexcept
{
	static std::atomic<std::uint64_t> total{};
	return total;
}
} // namespace

CountAuditResult audit_executable_counts() noexcept
{
	CountAuditResult result;
	auto *registry = existing_address_space_registry();
	if (registry == nullptr) {
		result.exact = true;
		return result;
	}
	AddressSpace::CountTally tally;
	result.exact = registry->tally_executable_counts(tally, true);
	if (result.exact) {
		result.records = tally.records;
		result.mismatches = tally.mismatches;
		result.generations = tally.generations;
		result.unnamed = tally.unnamed;
		result.samples = tally.samples;
		result.sample_count = tally.sample_count;
		result.sample_expected = tally.sample_expected;
	}
	return result;
}

std::uint64_t count_anomalies() noexcept
{
#if UML_MEMORY_ALLOCATION_TESTING
	return testing::count_anomalies();
#else
	return anomaly_total().load(std::memory_order_relaxed);
#endif
}

void note_count_anomaly() noexcept
{
	anomaly_total().fetch_add(1, std::memory_order_relaxed);
}
#endif

void set_domain_retire_observer(DomainRetireObserver observer) noexcept
{
	domain_retire_observer().store(observer, std::memory_order_release);
}

void retire_domain(std::uint64_t domain) noexcept
{
	if (domain == 0)
		return;
	domain_retirement_sequence_storage().fetch_add(
		1, std::memory_order_acq_rel);
	/* Retirement is a teardown operation. It must never allocate or initialize
	 * process-wide state merely to discover that the domain was untouched. */
	if (auto *registry = existing_address_space_registry(); registry != nullptr)
		registry->retire(domain);
	if (const auto observer = domain_retire_observer().load(
		    std::memory_order_acquire); observer != nullptr)
		observer(domain);
}

} // namespace uml::memory

namespace {

template<typename Function>
[[nodiscard]] int with_address_space(Function &&function) noexcept
{
	return uml::memory::bridge_call(
		[] { return uml::memory::address_space(); },
		std::forward<Function>(function),
		[] { uml::memory::reset_mapping_batch(); });
}

/* The `guest_code` argument of a map call is a small bit set: bit 0 for guest
 * code and bit 1 for the kernel's fact that the page belongs to a file system
 * that cannot change under the guest. The fact means something only with
 * bit 0, and a caller that passes 0 or 1 never sets it. */
[[nodiscard]] constexpr bool mapping_is_immutable(int guest_code) noexcept
{
	return (guest_code & 3) == 3;
}

template<typename Function>
[[nodiscard]] std::uint64_t with_address_space_sequence(
	Function &&function) noexcept
{
	try {
		auto space = uml::memory::address_space();
		return space ? function(*space) : 0;
	} catch (...) {
		uml::memory::reset_mapping_batch();
		return 0;
	}
}

} // namespace

extern "C" int darwin_uml_close(int fd)
{
	try {
		if (auto *registry = uml::memory::existing_address_space_registry();
		    registry != nullptr)
			registry->forget_descriptor(fd);
	} catch (...) {
		/* Descriptor closure must still happen after bookkeeping failure. */
	}
	return ::close(fd);
}

extern "C" int darwin_uml_static_vm_map(void *address, int fd,
					 unsigned long long offset,
					 unsigned long length, int protection,
					 int guest_code, int guest_writable)
{
	return with_address_space([&](auto &space) {
		return space.map(space.active_generation(),
			reinterpret_cast<std::uintptr_t>(address), fd, offset, length,
			protection, guest_code != 0, guest_writable != 0,
			mapping_is_immutable(guest_code));
	});
}

extern "C" int darwin_uml_static_vm_map_generation(
	unsigned long generation, void *address, int fd,
	unsigned long long offset, unsigned long length, int protection,
	int guest_code, int guest_writable)
{
	return with_address_space([&](auto &space) {
		return space.map(generation,
			reinterpret_cast<std::uintptr_t>(address), fd, offset,
			length, protection, guest_code != 0, guest_writable != 0,
			mapping_is_immutable(guest_code));
	});
}

extern "C" int darwin_uml_static_vm_protect(void *address,
					     unsigned long length,
					     int protection)
{
	return with_address_space([&](auto &space) {
		return space.protect(
			reinterpret_cast<std::uintptr_t>(address), length, protection);
	});
}

extern "C" int darwin_uml_static_vm_unmap(void *address,
					   unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.unmap(space.active_generation(),
			reinterpret_cast<std::uintptr_t>(address), length);
	});
}

extern "C" int darwin_uml_static_vm_unmap_generation(
	unsigned long generation, void *address, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.unmap(generation,
			reinterpret_cast<std::uintptr_t>(address), length);
	});
}

extern "C" int darwin_uml_static_vm_flush_generation(
	unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.flush_generation(generation); });
}

extern "C" int darwin_uml_static_vm_activate(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.activate(generation); });
}

extern "C" int darwin_uml_static_vm_destroy(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.destroy(generation); });
}

extern "C" int darwin_uml_static_vm_generation_live(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.generation_live(generation); });
}

extern "C" unsigned long long darwin_uml_static_vm_generation_retirements(void)
{
	return uml::memory::generation_retirement_sequence().load(
		std::memory_order_acquire);
}

extern "C" int darwin_uml_static_vm_discard_if_stale(unsigned long address,
	int fd, unsigned long long offset)
{
	return with_address_space([&](auto &space) {
		return space.discard_if_stale(address, fd, offset);
	});
}

extern "C" int darwin_uml_static_vm_copy_from_buffer(
	unsigned long address, const void *buffer, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.copy_from_buffer(address, buffer, length, false);
	});
}

extern "C" int darwin_uml_static_vm_copy_kernel_write(
	unsigned long address, const void *buffer, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.copy_from_buffer(address, buffer, length, true);
	});
}

extern "C" void darwin_uml_static_vm_require_coherent_kernel_code_writes(void)
{
	auto &required = uml::memory::coherent_kernel_code_writes();
	if (required.load(std::memory_order_acquire))
		return;
	required.store(true, std::memory_order_seq_cst);
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

extern "C" int darwin_uml_static_vm_retry_fault(
	unsigned long address, int execute, int write)
{
	return with_address_space([&](auto &space) {
		void *translated = nullptr;
		const auto access = execute != 0 ? DARWIN_UML_STATIC_EXECUTE :
			write != 0 ? DARWIN_UML_STATIC_WRITE : DARWIN_UML_STATIC_READ;
		const int result = space.translate(
			space.active_generation(), address, access, &translated);
		if (result == DARWIN_UML_STATIC_TRANSLATED &&
		    translated != nullptr)
			return 1;
		return result == DARWIN_UML_STATIC_HOST_ERROR ? -1 : 0;
	});
}

extern "C" int darwin_uml_static_vm_batch_begin(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.begin_batch(generation); });
}

extern "C" int darwin_uml_static_vm_batch_end(void)
{
	return with_address_space([](auto &space) { return space.end_batch(); });
}

extern "C" int darwin_uml_static_vm_walker_lock(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.lock_walker(generation); });
}

extern "C" int darwin_uml_static_vm_walker_unlock(unsigned long generation)
{
	return with_address_space(
		[&](auto &space) { return space.unlock_walker(generation); });
}

extern "C" int darwin_uml_static_vm_translate(
	unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address)
{
	return with_address_space([&](auto &space) {
		return space.translate(generation, address, access, host_address);
	});
}

extern "C" int darwin_uml_static_vm_translate_execute(
	unsigned long generation, unsigned long address, void **host_address,
	void **holder)
{
	return with_address_space([&](auto &space) {
		return space.translate_execute(
			generation, address, host_address, holder);
	});
}

extern "C" int darwin_uml_static_vm_execution_context_translate(
	void *context, unsigned long generation, unsigned long address,
	enum darwin_uml_static_access access, void **host_address)
{
	const int result = uml::memory::AddressSpace::execution_context_translate(
		static_cast<uml::memory::ExecutionContext *>(context), generation,
		address, access, host_address);
	if (result != uml::memory::AddressSpace::execution_context_fallback)
		return result;
	return darwin_uml_static_vm_translate(generation, address, access,
		host_address);
}

extern "C" int darwin_uml_static_vm_code_page(
	unsigned long generation, unsigned long address, void **handle,
	struct darwin_uml_static_code_page_stamp *stamp)
{
	return with_address_space([&](auto &space) {
		return space.code_page(generation, address, handle, stamp);
	});
}

extern "C" int darwin_uml_static_vm_code_page_stamp(
	const void *handle, struct darwin_uml_static_code_page_stamp *stamp)
{
	if (stamp == nullptr)
		return -1;
	*stamp = {};
	return uml::memory::load_stamp(handle, *stamp) ? 0 : -1;
}

extern "C" void darwin_uml_static_vm_invalidate_code_page(void *handle)
{
	uml::memory::MappedBacking::invalidate_code_page(handle);
}

extern "C" uint64_t darwin_uml_static_vm_changes(unsigned long generation)
{
	return with_address_space_sequence(
		[&](auto &space) { return space.changes(generation); });
}

extern "C" int darwin_uml_static_vm_mapping_changes(
	unsigned long generation, uint64_t sequence,
	struct darwin_uml_static_mapping_change *changes, size_t capacity,
	uint64_t *current_sequence)
{
	return with_address_space([&](auto &space) {
		return space.mapping_changes(
			generation, sequence, changes, capacity, current_sequence);
	});
}

extern "C" int darwin_uml_static_vm_execution_context_code_change_ranges(
	void *context, uint64_t sequence,
	struct darwin_uml_static_code_change *changes, size_t capacity,
	uint64_t *current_sequence)
{
	return uml::memory::execution_context_code_changes(context, sequence,
		changes, capacity, current_sequence);
}

extern "C" void *darwin_uml_static_vm_execution_context_create(
	unsigned long generation)
{
	try {
		auto space = uml::memory::address_space();
		return space ? space->create_execution_context(generation) : nullptr;
	} catch (...) {
		return nullptr;
	}
}

extern "C" void darwin_uml_static_vm_execution_context_destroy(void *context)
{
	try {
		uml::memory::AddressSpace::destroy_execution_context(
			static_cast<uml::memory::ExecutionContext *>(context));
	} catch (...) {
		/* The C destruction boundary cannot report an error. */
	}
}

extern "C" int darwin_uml_static_vm_execution_context_begin(
	void *context, unsigned long guest_pc)
{
	try {
		errno = 0;
		const int result = uml::memory::AddressSpace::execution_context_begin(
			static_cast<uml::memory::ExecutionContext *>(context), guest_pc);
		if (result < 0 && errno == 0)
			errno = EFAULT;
		return result;
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
		return -1;
	} catch (...) {
		errno = EIO;
		return -1;
	}
}

extern "C" int darwin_uml_static_vm_execution_context_end(void *context)
{
	try {
		errno = 0;
		const int result = uml::memory::AddressSpace::execution_context_end(
			static_cast<uml::memory::ExecutionContext *>(context));
		if (result < 0 && errno == 0)
			errno = EFAULT;
		return result;
	} catch (const std::bad_alloc &) {
		errno = ENOMEM;
		return -1;
	} catch (...) {
		errno = EIO;
		return -1;
	}
}

extern "C" struct darwin_uml_static_sequences
darwin_uml_static_vm_execution_context_sequences(void *context)
{
	try {
		return uml::memory::AddressSpace::execution_context_sequences(
			static_cast<uml::memory::ExecutionContext *>(context));
	} catch (...) {
		return {};
	}
}

extern "C" const uint64_t *
darwin_uml_static_vm_execution_context_code_sequence(void *context)
{
	return uml::memory::AddressSpace::execution_context_code_sequence(
		static_cast<uml::memory::ExecutionContext *>(context));
}

extern "C" int darwin_uml_static_vm_invalidate_code_generation(
	unsigned long generation, unsigned long address, unsigned long length)
{
	return with_address_space([&](auto &space) {
		return space.invalidate_code(generation, address, length);
	});
}

extern "C" int darwin_uml_static_vm_invalidate_code_frame(
	unsigned long generation, unsigned long address, unsigned long length,
	int fd, unsigned long long offset)
{
	return with_address_space([&](auto &space) {
		return space.invalidate_code_frame(generation, address, length, fd,
			offset);
	});
}

extern "C" int darwin_uml_static_vm_invalidate_code_frames(
	int fd, unsigned long long offset, unsigned long long length)
{
	return with_address_space([&](auto &space) {
		return space.invalidate_code_frames(fd, offset, length);
	});
}

extern "C" void darwin_uml_static_vm_invalidate_code(unsigned long address)
{
	(void)with_address_space([&](auto &space) {
		(void)space.invalidate_code(address);
		return 0;
	});
}

extern "C" int darwin_uml_static_vm_synchronize_code(unsigned long address)
{
	/* No address space for the thread makes the bridge answer -1, which the
	 * helper takes for an unmapped address, as it does any non-zero answer. */
	return with_address_space([&](auto &space) {
		return space.invalidate_code(address) ? 0 : 1;
	});
}

#if DARWIN_UML_X86_64_GUEST
extern "C" int darwin_uml_static_vm_hold_code_frame(
	void *context, unsigned long generation, unsigned long address,
	struct darwin_uml_static_frame_location *owed, size_t capacity,
	struct darwin_uml_static_frame_hold *hold)
{
	const int result = with_address_space([&](auto &space) {
		return space.hold_code_frame(
			context, generation, address, owed, capacity, hold);
	});
	/* No address space to ask is a failure the caller treats as any other. */
	return result < 0 ? DARWIN_UML_STATIC_FRAME_FAILED : result;
}

extern "C" void darwin_uml_static_vm_clear_code_debt(
	void *context, unsigned long generation, unsigned long address,
	uint64_t record_identity, uint64_t debt_sequence)
{
	(void)with_address_space([&](auto &space) {
		space.clear_code_debt(context, generation, address, record_identity,
			debt_sequence);
		return 0;
	});
}

extern "C" int darwin_uml_static_vm_frame_state(
	void *context, unsigned long generation, unsigned long address,
	struct darwin_uml_static_frame_state *state)
{
	return with_address_space([&](auto &space) {
		return space.frame_state(context, generation, address, state);
	});
}

extern "C" int darwin_uml_static_vm_execution_context_frame_fill(
	void *context, const void *code_page, unsigned long address,
	struct darwin_uml_static_frame_state *state)
{
	const auto *entered = static_cast<uml::memory::ExecutionContext *>(context);
	if (entered == nullptr || !entered->state || state == nullptr)
		return -1;
	const auto page =
		address & ~std::uint64_t{DARWIN_UML_GUEST_PAGE_SIZE - 1U};
	/* A page that holds a translation has its record's handle; a page that
	 * cannot (a data mapping of the frame) is resolved by its word. A frame
	 * with no record yet is a fill too: the record appearing moves it. */
	const auto fill = code_page != nullptr ?
		uml::memory::FrameAccounting::fill_state(*entered->state, page,
			code_page) :
		uml::memory::FrameAccounting::fill_state(*entered->state, page);
	if (fill.record == nullptr && fill.backing == nullptr)
		return -1;
	*state = {fill.backing, fill.offset, fill.record, fill.alias_changes,
		fill.other_holder ? 1 : 0};
	return 0;
}

extern "C" int darwin_uml_static_vm_frame_state_unchanged(
	const struct darwin_uml_static_frame_state *state)
{
	if (state == nullptr)
		return 1;
	return uml::memory::FrameAccounting::fill_unchanged(
		{state->record, state->alias_changes, state->other_holder != 0,
			const_cast<uml::memory::MappedBacking *>(
				static_cast<const uml::memory::MappedBacking *>(
					state->backing)),
			state->offset}) ? 1 : 0;
}

extern "C" int darwin_uml_static_vm_publish_frame_store(
	void *context, unsigned long generation, unsigned long address,
	unsigned long length, uint64_t *own, size_t capacity,
	struct darwin_uml_static_frame_publication *publication)
{
	return with_address_space([&](auto &space) {
		return space.publish_frame_store(context, generation, address,
			length, own, capacity, publication);
	});
}

extern "C" int darwin_uml_static_vm_frame_own_holders(
	void *context, unsigned long generation, unsigned long address,
	unsigned long length, uint64_t *own, size_t capacity,
	struct darwin_uml_static_frame_publication *publication)
{
	return with_address_space([&](auto &space) {
		return space.frame_own_holders(context, generation, address, length,
			own, capacity, publication);
	});
}

extern "C" uint64_t darwin_uml_static_vm_execution_context_flush_serial(
	void *context)
{
	const auto *entered = static_cast<uml::memory::ExecutionContext *>(context);
	return entered == nullptr || !entered->state ? 0 :
		uml::memory::FrameAccounting::flush_serial(*entered->state);
}
#endif

#if UML_MEMORY_ALLOCATION_TESTING && DARWIN_UML_X86_64_GUEST
namespace uml::memory::testing {

FrameWriterAudit audit_frame_writers() noexcept
{
	try {
		if (const auto access = address_space(); access)
			return access->audit_frame_writers_for_testing();
	} catch (...) {
	}
	return {};
}

std::shared_ptr<GenerationState> generation_state(std::uint64_t generation)
{
	if (const auto access = address_space(); access)
		return access->state_for_testing(generation);
	return {};
}

std::shared_ptr<MappedBacking> mapped_backing(int fd)
{
	if (const auto access = address_space(); access)
		return access->backing_for_testing(fd);
	return {};
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> frame_writers(
	int fd, std::uint64_t offset)
{
	if (const auto access = address_space(); access)
		return access->frame_writers_for_testing(fd, offset);
	return {};
}

} // namespace uml::memory::testing
#endif
