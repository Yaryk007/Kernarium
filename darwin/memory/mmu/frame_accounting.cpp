// SPDX-License-Identifier: GPL-2.0-or-later
#include "frame_accounting.hpp"

#if DARWIN_UML_X86_64_GUEST

#include "address_space_types.hpp"
#include "allocation_fault.hpp"
#include "frame_heap.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace uml::memory {
namespace {

constexpr std::uint64_t frame_bytes = 4096;
constexpr std::size_t stripe_count = 4096;

/* One lock per frame stripe, process-wide, so that no lock outlives the
 * backing it covers. A frame maps to a stripe by a hash of its backing and
 * its number. */
[[nodiscard]] WordSharedMutex &stripe_for(
	const MappedBacking *backing, std::uint64_t offset) noexcept
{
	static std::array<WordSharedMutex, stripe_count> stripes;
	auto key = (reinterpret_cast<std::uintptr_t>(backing) >> 4U) *
			0x9e3779b97f4a7c15ULL ^
		(offset / frame_bytes) * 0xc2b2ae3d27d4eb4fULL;
	key ^= key >> 29U;
	return stripes[key & (stripe_count - 1U)];
}

/* A frame's stripe lock, held exclusively. Nothing allocates, makes a record
 * or takes another lock while it is held; the test variant counts every
 * allocation made under it. */
class StripeLock final {
public:
	explicit StripeLock(WordSharedMutex &mutex) noexcept : mutex_{mutex}
	{
		mutex_.lock();
		UML_MEMORY_STRIPE_LOCK_ENTERED();
	}
	~StripeLock()
	{
		UML_MEMORY_STRIPE_LOCK_LEFT();
		mutex_.unlock();
	}
	StripeLock(const StripeLock &) = delete;
	StripeLock &operator=(const StripeLock &) = delete;

private:
	WordSharedMutex &mutex_;
};

using WriterList = std::vector<WriterLocation, FrameAllocator<WriterLocation>>;
/* A frame's word: its kind in the top two bits, and for a single writer its
 * location, the alias id and the page number. "Several" keeps its locations
 * in the frame's code-page record. */
enum class Kind : std::uint64_t { none, single, several, unknown };
constexpr unsigned kind_shift = 62;
constexpr unsigned alias_shift = 36;
constexpr std::uint64_t page_number_mask = (std::uint64_t{1} << alias_shift) - 1U;

[[nodiscard]] constexpr Kind kind_of(std::uint64_t word) noexcept
{
	return static_cast<Kind>(word >> kind_shift);
}

[[nodiscard]] constexpr std::uint64_t kind_word(Kind kind) noexcept
{
	return static_cast<std::uint64_t>(kind) << kind_shift;
}

[[nodiscard]] constexpr std::uint64_t single_word(
	const WriterLocation &location) noexcept
{
	return kind_word(Kind::single) |
		(std::uint64_t{location.alias} << alias_shift) |
		((location.page / frame_bytes) & page_number_mask);
}

[[nodiscard]] constexpr WriterLocation location_of(std::uint64_t word) noexcept
{
	return {static_cast<std::uint32_t>(
			(word >> alias_shift) & FrameAccountingDomain::maximum_alias),
		(word & page_number_mask) * frame_bytes};
}

using NodeList = std::vector<MappedBacking::WriterNode,
	FrameAllocator<MappedBacking::WriterNode>>;

[[nodiscard]] bool contains(const WriterList &list,
	const WriterLocation &location) noexcept
{
	return std::find(list.begin(), list.end(), location) != list.end();
}

/* The summary word of a holder list: none, the one holder, or several. */
[[nodiscard]] std::uint64_t summary_of(const WriterList &holders) noexcept
{
	if (holders.empty())
		return 0;
	return holders.size() == 1 ? single_word(holders.front()) :
		kind_word(Kind::several);
}

/* The host ranges the kernel maps shared from a descriptor, as a small table
 * of sequence-locked entries: written under a mutex and read without a lock. */
struct DirectRange final {
	std::atomic_uint64_t sequence{0};
	std::atomic_uintptr_t begin{0};
	std::atomic_uintptr_t end{0};
	std::atomic_int fd{-1};
	std::atomic_uint64_t offset{0};

	void store(std::uintptr_t first, std::uintptr_t last, int descriptor,
		std::uint64_t file_offset) noexcept
	{
		const auto before = sequence.load(std::memory_order_relaxed);
		sequence.store(before + 1U, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		begin.store(first, std::memory_order_relaxed);
		end.store(last, std::memory_order_relaxed);
		fd.store(descriptor, std::memory_order_relaxed);
		offset.store(file_offset, std::memory_order_relaxed);
		sequence.store(before + 2U, std::memory_order_release);
	}
};

constexpr std::size_t direct_range_count = 32;

struct DirectTable final {
	std::mutex mutex;
	std::array<DirectRange, direct_range_count> ranges;
};

[[nodiscard]] DirectTable &direct_table()
{
	static auto *table = new DirectTable;
	return *table;
}


std::atomic_uint64_t accounting_errors{};
std::atomic_uint64_t nonempty_destructions{};
std::atomic_uint64_t writer_additions{};
std::atomic_uint64_t writer_removals{};
std::atomic_uint64_t reservation_rollbacks{};
std::atomic_uint64_t reservation_growths{};
std::atomic_uint64_t holder_additions{};
std::atomic_uint64_t holder_removals{};
std::atomic_uint64_t serial_bumps{};
std::atomic_uint64_t holder_leaks{};
std::atomic_uint64_t unquiesced_removals{};
std::atomic_uint64_t unmarked_removals{};
std::atomic_uint64_t walked_refusals{};
std::atomic_uint64_t registration_failures{};
std::atomic_uint64_t publications{};
std::atomic_uint64_t unresolved_writes{};
std::atomic_uint64_t direct_range_drops{};
std::atomic_uint64_t global_holds{};
std::atomic_uint64_t kernel_frame_writes{};

/* The source of every flush serial. The 64-bit value moves once per walk of
 * a generation, and once for each state made; at a million moves a second it
 * would take over half a million years to wrap, so no two states and no two
 * moments of one state ever share a value, and a body recorded under a
 * serial can never stamp-hit under another. */
std::atomic_uint64_t flush_serial_source{1};

constexpr std::uint64_t execution_gate_closed = std::uint64_t{1} << 63;

/* Whether the generation's execution is quiesced: every store through its
 * locations has happened before. */
[[nodiscard]] bool quiesced(const GenerationState &state) noexcept
{
	return state.execution_gate.load(std::memory_order_acquire) ==
		execution_gate_closed;
}

/* Makes the frame's writers unknown, under its stripe lock. A registration
 * of such a frame can name no writer and owes every write translation, which
 * the memo of an executor that registered the frame earlier cannot say, so
 * the frame's change count moves in the section that makes the change. The
 * record is looked for again: one made after the caller looked is the one a
 * registration holds. */
void make_unknown(MappedBacking &backing, std::uint64_t offset,
	std::uint64_t *word) noexcept
{
	*word = kind_word(Kind::unknown);
	if (auto *record = backing.find_frame_record(offset); record != nullptr)
		record->alias_changes.fetch_add(1, std::memory_order_release);
}

/* Adds `location` to the frame's writers, under the frame's stripe lock.
 * `record` is the frame's record, found before the lock was taken; null if it
 * has none. When the list needs a record or more room than `spare` brings,
 * returns false with the room wanted in `wanted`; the caller makes the record
 * and `spare` outside the lock and tries again. */
[[nodiscard]] bool try_add(MappedBacking &backing, std::uint64_t offset,
	MappedBacking::FrameRecord *record, const WriterLocation &location,
	NodeList &spare, std::size_t &wanted,
	bool &added) noexcept
{
	auto *word = backing.writer_word(offset);
	if (word == nullptr) {
		/* The caller made the leaf before taking the lock. */
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		added = false;
		return true;
	}
	added = false;
	switch (kind_of(*word)) {
	case Kind::none:
		*word = single_word(location);
		if (record != nullptr)
			record->single_debt = 0;
		added = true;
		writer_additions.fetch_add(1, std::memory_order_relaxed);
		return true;
	case Kind::unknown:
		return true;
	case Kind::single: {
		const auto existing = location_of(*word);
		if (existing == location)
			return true;
		if (record == nullptr) {
			wanted = 2;
			return false;
		}
		auto &list = record->writers;
		if (list.capacity() < 2) {
			if (spare.capacity() < 2) {
				wanted = 2;
				return false;
			}
			spare.assign(list.begin(), list.end());
			list.swap(spare);
		}
		list.clear();
		list.push_back({existing, record->single_debt});
		list.push_back({location, 0});
		record->single_debt = 0;
		*word = kind_word(Kind::several);
		added = true;
		writer_additions.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	case Kind::several: {
		if (record == nullptr) {
			/* Several writers without their list: an error. */
			accounting_errors.fetch_add(1, std::memory_order_relaxed);
			make_unknown(backing, offset, word);
			return true;
		}
		auto &list = record->writers;
		if (std::any_of(list.begin(), list.end(),
			    [&](const MappedBacking::WriterNode &node) {
				    return node.location == location;
			    }))
			return true;
		if (list.size() == list.capacity()) {
			const auto room = std::max<std::size_t>(4, list.size() * 2U);
			if (spare.capacity() < room) {
				wanted = room;
				return false;
			}
			spare.assign(list.begin(), list.end());
			list.swap(spare);
		}
		list.push_back({location, 0});
		added = true;
		writer_additions.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	}
	return true;
}

/* Removes `location` from the holders of the frame at `offset`, if it is
 * one. The record is found before the lock and the room a list gives up is
 * freed after it. */
void remove_holder(const GenerationState &state, MappedBacking &backing,
	std::uint64_t offset, const WriterLocation &location) noexcept
{
	auto *record = backing.find_frame_record(offset);
	if (record == nullptr)
		return;
	WriterList released{FrameAllocator<WriterLocation>{backing.heap()}};
	const StripeLock lock{stripe_for(&backing, offset)};
	auto &holders = record->holders;
	const auto found = std::find(holders.begin(), holders.end(), location);
	if (found == holders.end())
		return;
	if (!quiesced(state) && !state.retired.load(std::memory_order_acquire))
		unquiesced_removals.fetch_add(1, std::memory_order_relaxed);
	*found = holders.back();
	holders.pop_back();
	record->summary.store(summary_of(holders), std::memory_order_release);
	if (auto *entry = backing.domain().entry(location.alias); entry != nullptr)
		entry->holders.fetch_sub(1, std::memory_order_relaxed);
	holder_removals.fetch_add(1, std::memory_order_relaxed);
	if (holders.empty())
		holders.swap(released);
}

/* Where the word of `page` lies. */
struct WordFrame final {
	MappedBacking *backing{};
	std::uint64_t offset{};
	bool writable{};
	bool translatable{};
};

[[nodiscard]] WordFrame frame_of_word(const GenerationState &state,
	std::uint64_t page, std::uintptr_t word) noexcept
{
	if (word == 0)
		return {};
	const auto fields = FillIndex::decode(word);
	const auto entry = state.backings.entry(fields.slot, page);
	if (entry.backing == nullptr)
		return {};
	return {entry.backing,
		fields.host - reinterpret_cast<std::uintptr_t>(entry.backing->base()),
		fields.write,
		FrameAccounting::translatable(fields.guest_code, fields.exec)};
}

} // namespace

bool FrameAccounting::add_writer(MappedBacking &backing,
	std::uint64_t offset, const WriterLocation &location)
{
	NodeList spare{FrameAllocator<MappedBacking::WriterNode>{backing.heap()}};
	std::size_t wanted = 0;
	for (;;) {
		UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::writers);
		/* The leaf and the record are made here, before the lock. */
		if ((wanted != 0 || backing.writer_word(offset) == nullptr) &&
		    !backing.prepare_writers(offset, wanted != 0))
			throw std::bad_alloc{};
		if (wanted != 0 && spare.capacity() < wanted)
			spare.reserve(wanted);
		auto *record = backing.find_frame_record(offset);
		std::size_t more = 0;
		bool added = false;
		{
			const StripeLock lock{stripe_for(&backing, offset)};
			if (try_add(backing, offset, record, location, spare, more, added))
				return added;
		}
		wanted = more;
	}
}

void FrameAccounting::remove_writer(MappedBacking &backing,
	std::uint64_t offset, const WriterLocation &location) noexcept
{
	/* Found without the lock, and the room a list gives up is freed after
	 * it. */
	auto *record = backing.find_frame_record(offset);
	NodeList released{FrameAllocator<MappedBacking::WriterNode>{backing.heap()}};
	const StripeLock lock{stripe_for(&backing, offset)};
	auto *word = backing.writer_word(offset);
	if (word == nullptr) {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto fail = [&] {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		make_unknown(backing, offset, word);
	};
	switch (kind_of(*word)) {
	case Kind::none:
		fail();
		return;
	case Kind::unknown:
		return;
	case Kind::single:
		if (location_of(*word) == location) {
			*word = 0;
			if (record != nullptr)
				record->single_debt = 0;
			writer_removals.fetch_add(1, std::memory_order_relaxed);
		} else {
			fail();
		}
		return;
	case Kind::several: {
		if (record == nullptr) {
			fail();
			return;
		}
		auto &list = record->writers;
		const auto found = std::find_if(list.begin(), list.end(),
			[&](const MappedBacking::WriterNode &node) {
				return node.location == location;
			});
		if (found == list.end()) {
			fail();
			return;
		}
		list.erase(found);
		writer_removals.fetch_add(1, std::memory_order_relaxed);
		if (list.size() == 1) {
			*word = single_word(list.front().location);
			record->single_debt = list.front().debt;
			list.clear();
			list.swap(released);
		} else if (list.empty()) {
			*word = 0;
			list.swap(released);
		}
		return;
	}
	}
}

void FrameAccounting::reserve(GenerationState &state, std::uint64_t page,
	const FrameRef &final)
{
	if (!final.writable || final.backing == nullptr || !tracks(page))
		return;
	state.accounting.reservation.add(
		state, *final.backing, final.offset, page);
}

void FrameAccounting::apply(GenerationState &state, std::uint64_t page,
	const FrameRef &before, const FrameRef &after) noexcept
{
	if (before.backing == nullptr || !tracks(page))
		return;
	const bool same_frame = after.backing == before.backing &&
		after.offset / frame_bytes == before.offset / frame_bytes;
	const WriterLocation here{state.accounting.alias, page};
	if (before.writable && !(after.writable && same_frame))
		remove_writer(*before.backing, before.offset, here);
	/* Where the code-change predicate names the page, so that the stamp move
	 * and the holder removal always go together. */
	if (before.translatable && !(after.translatable && same_frame))
		remove_holder(state, *before.backing, before.offset, here);
}

void FrameAccounting::release_word(GenerationState &state,
	std::uint64_t page, std::uintptr_t word) noexcept
{
	if (!tracks(page) || word == 0)
		return;
	const auto fields = FillIndex::decode(word);
	if (!fields.write && !(fields.guest_code && fields.exec))
		return;
	const auto frame = frame_of_word(state, page, word);
	if (frame.backing == nullptr) {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const WriterLocation here{state.accounting.alias, page};
	if (frame.writable)
		remove_writer(*frame.backing, frame.offset, here);
	if (frame.translatable)
		remove_holder(state, *frame.backing, frame.offset, here);
}

void FrameAccounting::release_writer(GenerationState &state,
	std::uint64_t page, std::uintptr_t word) noexcept
{
	if (!tracks(page) || word == 0)
		return;
	const auto frame = frame_of_word(state, page, word);
	if (!frame.writable)
		return;
	if (frame.backing == nullptr) {
		accounting_errors.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	remove_writer(*frame.backing, frame.offset,
		{state.accounting.alias, page});
}

void FrameAccounting::release_holder(GenerationState &state,
	std::uint64_t page, std::uintptr_t word) noexcept
{
	if (!tracks(page) || word == 0)
		return;
	const auto frame = frame_of_word(state, page, word);
	if (frame.backing == nullptr || !frame.translatable)
		return;
	remove_holder(state, *frame.backing, frame.offset,
		{state.accounting.alias, page});
}

void FrameAccounting::commit(GenerationState &state) noexcept
{
	state.accounting.reservation.commit();
}

void FrameAccounting::settle(GenerationState &state) noexcept
{
	state.accounting.reservation.settle(state.accounting.alias);
}

void FrameAccounting::walk(GenerationState &state) noexcept
{
	/* A walked state gave its id back, which another state may hold now. */
	if (!state.accounting.walked)
		(void)state.fill_index.scan(0, ~std::uint64_t{0},
			[&](std::uint64_t page, std::uintptr_t word) {
				release_word(state, page, word);
				return false;
			});
	/* After the holders are gone: no translation recorded before this point
	 * may be taken for current, because none of them stands any more. */
	bump_flush_serial(state);
}

void FrameAccounting::bump_flush_serial(GenerationState &state) noexcept
{
	/* The value is one no state has held. */
	state.accounting.flush_serial.store(
		flush_serial_source.fetch_add(1, std::memory_order_relaxed),
		std::memory_order_release);
	serial_bumps.fetch_add(1, std::memory_order_relaxed);
}

void FrameAccounting::finish(GenerationState &state) noexcept
{
	auto &accounting = state.accounting;
	if (accounting.walked)
		return;
	accounting.walked = true;
	/* By invariant H-W the walk removed every holder of this state. One that
	 * is left escaped it: the id then stays retired, never reused, and its
	 * entry names no state, so a publication skips those holders. */
	const auto *entry = accounting.domain->entry(accounting.alias);
	const bool leaked = entry != nullptr &&
		entry->holders.load(std::memory_order_relaxed) != 0;
	if (leaked)
		holder_leaks.fetch_add(1, std::memory_order_relaxed);
	accounting.domain->release(accounting.alias, leaked);
}

void FrameAccounting::retire(GenerationState &state) noexcept
{
	walk(state);
	finish(state);
}

FrameAccounting::Diagnostics FrameAccounting::diagnostics() noexcept
{
	return {accounting_errors.load(std::memory_order_relaxed),
		nonempty_destructions.load(std::memory_order_relaxed),
		writer_additions.load(std::memory_order_relaxed),
		writer_removals.load(std::memory_order_relaxed),
		reservation_rollbacks.load(std::memory_order_relaxed),
		reservation_growths.load(std::memory_order_relaxed),
		holder_additions.load(std::memory_order_relaxed),
		holder_removals.load(std::memory_order_relaxed),
		serial_bumps.load(std::memory_order_relaxed),
		holder_leaks.load(std::memory_order_relaxed),
		unquiesced_removals.load(std::memory_order_relaxed),
		unmarked_removals.load(std::memory_order_relaxed),
		walked_refusals.load(std::memory_order_relaxed),
		registration_failures.load(std::memory_order_relaxed),
		publications.load(std::memory_order_relaxed),
		unresolved_writes.load(std::memory_order_relaxed),
		direct_range_drops.load(std::memory_order_relaxed),
		global_holds.load(std::memory_order_relaxed),
		kernel_frame_writes.load(std::memory_order_relaxed)};
}

void FrameAccounting::note_nonempty_state_destruction() noexcept
{
	nonempty_destructions.fetch_add(1, std::memory_order_relaxed);
}

StateAccounting::StateAccounting(
	std::shared_ptr<FrameAccountingDomain> frame_domain,
	GenerationState &state, std::uint64_t generation)
	: domain{std::move(frame_domain)},
	  alias{domain->allocate(state, generation)}
{
	flush_serial.store(
		flush_serial_source.fetch_add(1, std::memory_order_relaxed),
		std::memory_order_relaxed);
}

StateAccounting::~StateAccounting()
{
	/* A state is destroyed after its walk, which gave its id back: it leaves
	 * its address space only through destroy and retire, each of which walks
	 * it, and the state's holders are out of every frame's list before the
	 * id is, so no publisher under a stripe lock can still reach it. One
	 * whose walk did not run and that reserved locations may still have them
	 * naming the id (an address space dropped without being retired, at the
	 * end of the process): it is counted and the id retired for good, and
	 * every test row fails on a count. One that never reserved gives its id
	 * back. */
	if (walked)
		return;
	const bool reserved = used.load(std::memory_order_relaxed);
	if (reserved)
		FrameAccounting::note_nonempty_state_destruction();
	domain->release(alias, reserved);
}

std::uint64_t FrameAccounting::flush_serial(const GenerationState &state) noexcept
{
	return state.accounting.flush_serial.load(std::memory_order_acquire);
}

FrameAccounting::HoldReport FrameAccounting::hold(GenerationState &state,
	std::uint64_t page, DebtLocation *owed, std::size_t capacity,
	bool global_ok) noexcept
{
	HoldReport report{HoldResult::failed, 0, 0, 0, nullptr, 0};
	const auto fail = [&]() -> HoldReport {
		registration_failures.fetch_add(1, std::memory_order_relaxed);
		report.result = HoldResult::failed;
		return report;
	};
	auto &accounting = state.accounting;
	if (accounting.walked) {
		walked_refusals.fetch_add(1, std::memory_order_relaxed);
		report.result = HoldResult::refused;
		return report;
	}
	accounting.used.store(true, std::memory_order_relaxed);
	const auto word = state.fill_index.lookup(page);
	const auto frame = frame_of_word(state, page, word);
	if (frame.backing == nullptr || !frame.translatable) {
		report.result = HoldResult::refused;
		return report;
	}
	if (!tracks(page))
		return fail();
	auto &backing = *frame.backing;
	const auto offset = frame.offset;
	MappedBacking::FrameRecord *record = nullptr;
	try {
		/* Found before the stripe lock: the record exists from the frame's
		 * first executable mapping, and this allocates nothing. It is null
		 * only for a backing with no slot for the frame in its record
		 * index, which cannot name the frame's holders; the registration
		 * fails, as it does for a frame it cannot track. */
		record = backing.frame_record(offset);
	} catch (...) {
	}
	if (record == nullptr)
		return fail();

	const WriterLocation here{accounting.alias, page};
	WriterList spare{FrameAllocator<WriterLocation>{backing.heap()}};
	std::size_t wanted = 0;
	for (;;) {
		try {
			UML_MEMORY_ALLOCATION_CHECKPOINT(
				testing::AllocationSite::holders);
			if (wanted > spare.capacity())
				spare.reserve(wanted);
		} catch (...) {
			return fail();
		}
		std::size_t more = 0;
		{
			const StripeLock lock{stripe_for(&backing, offset)};
			auto *writer_word = backing.writer_word(offset);
			const auto kind = writer_word == nullptr ? Kind::none :
				kind_of(*writer_word);
			/* Writers that are not known cannot be named: the debt is every
			 * write translation. */
			bool global = kind == Kind::unknown;
			auto &holders = record->holders;
			const bool repeat = contains(holders, here);
			/* The writers, with the debt each carries. */
			const auto each_writer = [&](auto &&visit) {
				if (kind == Kind::single)
					visit(location_of(*writer_word), record->single_debt);
				else if (kind == Kind::several)
					for (auto &node : record->writers)
						visit(node.location, node.debt);
			};
			/* Who this registration newly owes. */
			const auto newly_owed = [&](const WriterLocation &writer) {
				if (repeat)
					return false;
				if (holders.empty())
					return !(writer == here);
				return holders.size() == 1 && holders.front() == writer;
			};
			std::size_t count = 0;
			bool any_new = false;
			each_writer([&](const WriterLocation &writer, std::uint64_t debt) {
				const bool is_new = newly_owed(writer);
				any_new = any_new || is_new;
				if (is_new || debt > record->debt_cleared)
					++count;
			});
			if (count > capacity) {
				if (!global_ok) {
					report.result = HoldResult::crowded;
					report.owed = count;
					return report;
				}
				global = true;
			}
			if (!repeat && holders.size() == holders.capacity() &&
			    spare.capacity() <= holders.size()) {
				more = std::max<std::size_t>(4, holders.size() * 2U);
			} else {
				if (any_new)
					++record->debt_sequence;
				const auto sequence = record->debt_sequence;
				std::size_t written = 0;
				const auto note = [&](const WriterLocation &writer,
					std::uint64_t &debt) {
					if (newly_owed(writer))
						debt = sequence;
					if (debt <= record->debt_cleared)
						return;
					const auto *entry = accounting.domain->entry(writer.alias);
					if (entry != nullptr && owed != nullptr && written < capacity)
						owed[written++] = {entry->generation.load(
							std::memory_order_relaxed), writer.page};
				};
				if (kind == Kind::single) {
					auto writer = location_of(*writer_word);
					note(writer, record->single_debt);
				} else if (kind == Kind::several) {
					for (auto &node : record->writers)
						note(node.location, node.debt);
				}
				if (!repeat) {
					if (holders.size() == holders.capacity()) {
						spare.assign(holders.begin(), holders.end());
						holders.swap(spare);
					}
					holders.push_back(here);
					record->summary.store(summary_of(holders),
						std::memory_order_release);
					record->alias_changes.fetch_add(1,
						std::memory_order_release);
					if (auto *entry = accounting.domain->entry(here.alias);
					    entry != nullptr)
						entry->holders.fetch_add(1, std::memory_order_relaxed);
					holder_additions.fetch_add(1, std::memory_order_relaxed);
				}
				report.record_identity = MappedBacking::record_identity(*record);
				report.debt_sequence = record->debt_sequence;
				report.record = record;
				report.alias_changes =
					record->alias_changes.load(std::memory_order_acquire);
				if (global)
					global_holds.fetch_add(1, std::memory_order_relaxed);
				report.owed = global ? count : written;
				report.result = global ? HoldResult::global :
					written == 0 ? HoldResult::settled : HoldResult::owed;
				return report;
			}
		}
		wanted = more;
	}
}

void FrameAccounting::clear_debt(GenerationState &state, std::uint64_t page,
	std::uint64_t record_identity, std::uint64_t debt_sequence) noexcept
{
	const auto frame = frame_of_word(state, page, state.fill_index.lookup(page));
	if (frame.backing == nullptr)
		return;
	auto *record = frame.backing->find_frame_record(frame.offset);
	if (record == nullptr ||
	    MappedBacking::record_identity(*record) != record_identity)
		return;
	const StripeLock lock{stripe_for(frame.backing, frame.offset)};
	if (debt_sequence > record->debt_cleared)
		record->debt_cleared = debt_sequence;
}

namespace {

/* What the fill reads of a record: the change count first, then the holder
 * summary, so a holder added in between moves the count past the one read. */
FrameAccounting::FillState fill_state_of(const GenerationState &state,
	std::uint64_t page, const MappedBacking::FrameRecord &record,
	FrameAccounting::FillState result) noexcept
{
	result.record = &record;
	result.alias_changes = record.alias_changes.load(std::memory_order_acquire);
	const auto summary = record.summary.load(std::memory_order_acquire);
	switch (kind_of(summary)) {
	case Kind::none:
		break;
	case Kind::single:
		result.other_holder = !(location_of(summary) ==
			WriterLocation{state.accounting.alias, page});
		break;
	default:
		result.other_holder = true;
		break;
	}
	return result;
}

} // namespace

FrameAccounting::FillState FrameAccounting::fill_state(
	const GenerationState &state, std::uint64_t page) noexcept
{
	FillState result{nullptr, 0, false, nullptr, 0};
	const auto frame = frame_of_word(state, page, state.fill_index.lookup(page));
	if (frame.backing == nullptr)
		return result;
	result.backing = frame.backing;
	result.offset = frame.offset;
	auto *record = frame.backing->find_frame_record(frame.offset);
	if (record == nullptr)
		return result;
	return fill_state_of(state, page, *record, result);
}

FrameAccounting::FillState FrameAccounting::fill_state(
	const GenerationState &state, std::uint64_t page,
	const void *code_page) noexcept
{
	FillState result{nullptr, 0, false, nullptr, 0};
	if (code_page == nullptr)
		return result;
	return fill_state_of(state, page, *MappedBacking::record_of(code_page),
		result);
}

bool FrameAccounting::fill_unchanged(const FillState &was) noexcept
{
	if (was.record != nullptr)
		/* Records are never destroyed or replaced. */
		return static_cast<const MappedBacking::FrameRecord *>(was.record)
			->alias_changes.load(std::memory_order_acquire) ==
			was.alias_changes;
	if (was.backing == nullptr)
		return true;
	/* A record that has appeared counts as moved. */
	return was.backing->find_frame_record(was.offset) == nullptr;
}

namespace {

/* Publishes at every holder of the frame; see FrameAccounting::publish_store.
 * `writer` is the writing location, or null for a kernel write. */
FrameAccounting::Publication publish_at_holders(MappedBacking &backing,
	std::uint64_t frame_offset, std::uint64_t in_page, std::uint64_t length,
	const GenerationState *writer_state, const WriterLocation *writer,
	std::uint64_t *own, std::size_t capacity, bool publish = true) noexcept
{
	FrameAccounting::Publication result{};
	auto *record = backing.find_frame_record(frame_offset);
	if (record == nullptr)
		return result;
	const StripeLock lock{stripe_for(&backing, frame_offset)};
	const auto &holders = record->holders;
	if (holders.empty())
		return result;
	if (writer != nullptr &&
	    std::all_of(holders.begin(), holders.end(),
		    [&](const WriterLocation &holder) { return holder == *writer; }))
		return result;
	if (publish) {
		MappedBacking::move_stamp(*record);
		publications.fetch_add(1, std::memory_order_relaxed);
	}
	result.handled = true;
	for (const auto &holder : holders) {
		auto *state = backing.domain().state_of(holder.alias);
		if (state == nullptr)
			continue;
		const auto address = holder.page + in_page;
		if (publish)
			publish_code_change(*state, address, length);
		if (state != writer_state)
			continue;
		if (publish)
			++result.own_records;
		if (result.own_count < capacity && own != nullptr)
			own[result.own_count++] = address;
		else
			result.overflow = true;
	}
	return result;
}

} // namespace

FrameAccounting::Publication FrameAccounting::publish_store(
	GenerationState &writer, std::uint64_t page, std::uint64_t in_page,
	std::uint64_t length, std::uint64_t *own, std::size_t capacity) noexcept
{
	const auto frame = frame_of_word(writer, page, writer.fill_index.lookup(page));
	if (frame.backing == nullptr)
		return {};
	const WriterLocation here{writer.accounting.alias, page};
	return publish_at_holders(*frame.backing, frame.offset, in_page, length,
		&writer, &here, own, capacity);
}

FrameAccounting::Publication FrameAccounting::own_holders(
	GenerationState &writer, std::uint64_t page, std::uint64_t in_page,
	std::uint64_t length, std::uint64_t *own, std::size_t capacity) noexcept
{
	const auto frame = frame_of_word(writer, page, writer.fill_index.lookup(page));
	if (frame.backing == nullptr)
		return {};
	const WriterLocation here{writer.accounting.alias, page};
	return publish_at_holders(*frame.backing, frame.offset, in_page, length,
		&writer, &here, own, capacity, false);
}

void FrameAccounting::publish_kernel_write(MappedBacking &backing,
	std::uint64_t frame_offset, std::uint64_t in_page,
	std::uint64_t length) noexcept
{
	(void)publish_at_holders(backing, frame_offset, in_page, length, nullptr,
		nullptr, nullptr, 0);
}

namespace {

/* One contiguous host range of the direct table, as the mutex's holder sees
 * it. */
struct DirectPiece final {
	std::uintptr_t begin;
	std::uintptr_t end;
	int fd;
	std::uint64_t offset;

	[[nodiscard]] bool operator==(const DirectPiece &other) const noexcept
	{
		return begin == other.begin && end == other.end && fd == other.fd &&
			offset == other.offset;
	}
};

/* A range holds at most one piece of what a mapping or an unmap covers
 * and leaves at most two outside it, and the ranges never overlap. */
constexpr std::size_t direct_piece_limit = direct_range_count + 2;

/* Makes [first, last) map to `replacement` (nothing when null) and leaves the
 * rest of every range it overlapped as it was: a range that extends past
 * either end keeps that part, split in two when both ends remain. Adjacent
 * pieces of one descriptor that continue each other are one range. When the
 * table is still full, the smallest ranges go and are counted: a kernel write
 * through a range the table lost names no frame, which the fallback covers. */
void reshape_direct(std::uintptr_t first, std::uintptr_t last,
	const DirectPiece *replacement) noexcept
{
	auto &table = direct_table();
	const std::lock_guard lock{table.mutex};
	std::array<DirectPiece, direct_piece_limit> pieces{};
	std::size_t count = 0;
	for (const auto &range : table.ranges) {
		const auto begin = range.begin.load(std::memory_order_relaxed);
		const auto end = range.end.load(std::memory_order_relaxed);
		if (begin == end)
			continue;
		const auto descriptor = range.fd.load(std::memory_order_relaxed);
		const auto offset = range.offset.load(std::memory_order_relaxed);
		if (end <= first || begin >= last) {
			pieces[count++] = {begin, end, descriptor, offset};
			continue;
		}
		if (begin < first)
			pieces[count++] = {begin, first, descriptor, offset};
		if (end > last)
			pieces[count++] = {last, end, descriptor, offset + (last - begin)};
	}
	if (replacement != nullptr)
		pieces[count++] = *replacement;
	std::sort(pieces.begin(), pieces.begin() + static_cast<std::ptrdiff_t>(count),
		[](const DirectPiece &a, const DirectPiece &b) {
			return a.begin < b.begin;
		});
	std::size_t merged = 0;
	for (std::size_t index = 0; index < count; ++index) {
		if (merged != 0) {
			auto &before = pieces[merged - 1];
			if (before.end == pieces[index].begin &&
			    before.fd == pieces[index].fd &&
			    before.offset + (before.end - before.begin) ==
				    pieces[index].offset) {
				before.end = pieces[index].end;
				continue;
			}
		}
		pieces[merged++] = pieces[index];
	}
	count = merged;
	while (count > direct_range_count) {
		std::size_t smallest = 0;
		for (std::size_t index = 1; index < count; ++index)
			if (pieces[index].end - pieces[index].begin <
			    pieces[smallest].end - pieces[smallest].begin)
				smallest = index;
		pieces[smallest] = pieces[--count];
		direct_range_drops.fetch_add(1, std::memory_order_relaxed);
	}

	/* A slot that already holds a piece is left alone. Each other piece takes
	 * the slot of a range it overlaps, so that readers see the old range or
	 * the new one for every address both cover, and then a free slot. */
	std::array<bool, direct_range_count> kept{};
	std::array<bool, direct_piece_limit> placed{};
	for (std::size_t slot = 0; slot < direct_range_count; ++slot) {
		const auto &range = table.ranges[slot];
		const DirectPiece now{range.begin.load(std::memory_order_relaxed),
			range.end.load(std::memory_order_relaxed),
			range.fd.load(std::memory_order_relaxed),
			range.offset.load(std::memory_order_relaxed)};
		if (now.begin == now.end)
			continue;
		for (std::size_t index = 0; index < count; ++index)
			if (!placed[index] && pieces[index] == now) {
				placed[index] = true;
				kept[slot] = true;
				break;
			}
	}
	for (std::size_t index = 0; index < count; ++index) {
		if (placed[index])
			continue;
		std::size_t chosen = direct_range_count;
		for (std::size_t slot = 0; slot < direct_range_count; ++slot) {
			if (kept[slot])
				continue;
			const auto &range = table.ranges[slot];
			const auto begin = range.begin.load(std::memory_order_relaxed);
			const auto end = range.end.load(std::memory_order_relaxed);
			if (begin != end && begin < pieces[index].end &&
			    end > pieces[index].begin) {
				chosen = slot;
				break;
			}
			if (chosen == direct_range_count || begin == end)
				chosen = slot;
		}
		kept[chosen] = true;
		table.ranges[chosen].store(pieces[index].begin, pieces[index].end,
			pieces[index].fd, pieces[index].offset);
	}
	for (std::size_t slot = 0; slot < direct_range_count; ++slot)
		if (!kept[slot] &&
		    table.ranges[slot].begin.load(std::memory_order_relaxed) !=
			    table.ranges[slot].end.load(std::memory_order_relaxed))
			table.ranges[slot].store(0, 0, -1, 0);
}

} // namespace

void FrameAccounting::note_direct_map(const void *address,
	std::uint64_t length, int fd, std::uint64_t offset) noexcept
{
	const auto first = reinterpret_cast<std::uintptr_t>(address);
	if (length == 0 || first + length < first)
		return;
	const DirectPiece piece{first, first + length, fd, offset};
	reshape_direct(first, first + length, &piece);
}

void FrameAccounting::note_direct_unmap(const void *address,
	std::uint64_t length) noexcept
{
	const auto first = reinterpret_cast<std::uintptr_t>(address);
	if (length == 0 || first + length < first)
		return;
	reshape_direct(first, first + length, nullptr);
}

bool FrameAccounting::resolve_direct(const void *address, int &fd,
	std::uint64_t &offset) noexcept
{
	const auto where = reinterpret_cast<std::uintptr_t>(address);
	auto &table = direct_table();
	for (auto &range : table.ranges) {
		const auto before = range.sequence.load(std::memory_order_acquire);
		if ((before & 1U) != 0 || before == 0)
			continue;
		const auto begin = range.begin.load(std::memory_order_relaxed);
		const auto end = range.end.load(std::memory_order_relaxed);
		const auto descriptor = range.fd.load(std::memory_order_relaxed);
		const auto file_offset = range.offset.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (range.sequence.load(std::memory_order_relaxed) != before)
			continue;
		if (where < begin || where >= end)
			continue;
		fd = descriptor;
		offset = file_offset + (where - begin);
		return true;
	}
	return false;
}

void FrameAccounting::note_unresolved_kernel_write() noexcept
{
	unresolved_writes.fetch_add(1, std::memory_order_relaxed);
}

void FrameAccounting::note_kernel_frame_write() noexcept
{
	kernel_frame_writes.fetch_add(1, std::memory_order_relaxed);
}

#if UML_MEMORY_ALLOCATION_TESTING
bool FrameAccounting::locations_for_testing(MappedBacking &backing,
	std::uint64_t offset, std::vector<WriterLocation> &locations)
{
	/* The room is made before the lock, as everywhere. */
	std::size_t room = 2;
	for (;;) {
		locations.reserve(locations.size() + room);
		const StripeLock lock{stripe_for(&backing, offset)};
		const auto *word = backing.writer_word(offset);
		if (word == nullptr)
			return true;
		switch (kind_of(*word)) {
		case Kind::none:
			return true;
		case Kind::unknown:
			return false;
		case Kind::single:
			locations.push_back(location_of(*word));
			return true;
		case Kind::several: {
			const auto *record = backing.find_frame_record(offset);
			if (record == nullptr)
				return true;
			if (locations.capacity() - locations.size() <
			    record->writers.size()) {
				room = record->writers.size();
				continue;
			}
			for (const auto &node : record->writers)
				locations.push_back(node.location);
			return true;
		}
		}
		return true;
	}
}

void FrameAccounting::holders_for_testing(MappedBacking &backing,
	std::uint64_t offset, std::vector<WriterLocation> &holders)
{
	/* The room is made before the lock, as everywhere. */
	std::size_t room = 2;
	for (;;) {
		holders.reserve(holders.size() + room);
		const auto *record = backing.find_frame_record(offset);
		if (record == nullptr)
			return;
		const StripeLock lock{stripe_for(&backing, offset)};
		if (holders.capacity() - holders.size() < record->holders.size()) {
			room = record->holders.size();
			continue;
		}
		holders.insert(holders.end(), record->holders.begin(),
			record->holders.end());
		return;
	}
}

std::uint64_t FrameAccounting::alias_holders_for_testing(
	const MappedBacking &backing, std::uint32_t alias) noexcept
{
	const auto *entry = backing.domain().entry(alias);
	return entry == nullptr ? 0 :
		entry->holders.load(std::memory_order_relaxed);
}
#endif

FrameAccountingDomain::~FrameAccountingDomain()
{
	auto *root = root_.load(std::memory_order_acquire);
	if (root == nullptr)
		return;
	for (auto &mid_slot : *root) {
		auto *mid = mid_slot.load(std::memory_order_relaxed);
		if (mid == nullptr)
			continue;
		for (auto &leaf_slot : *mid)
			if (auto *leaf = leaf_slot.load(std::memory_order_relaxed);
			    leaf != nullptr) {
				leaf->~Leaf();
				frame_deallocate(heap(), leaf, sizeof(Leaf), alignof(Leaf));
			}
		mid->~Mid();
		frame_deallocate(heap(), mid, sizeof(Mid), alignof(Mid));
	}
	root->~Root();
	frame_deallocate(heap(), root, sizeof(Root), alignof(Root));
}

FrameAccountingDomain::Entry *FrameAccountingDomain::entry(
	std::uint32_t id) const noexcept
{
	if (id == 0 || id > maximum_alias)
		return nullptr;
	const auto *root = root_.load(std::memory_order_acquire);
	if (root == nullptr)
		return nullptr;
	const auto *mid = (*root)[id >> (leaf_bits + mid_bits)].load(
		std::memory_order_acquire);
	if (mid == nullptr)
		return nullptr;
	const auto *leaf = (*mid)[(id >> leaf_bits) & ((1U << mid_bits) - 1U)].load(
		std::memory_order_acquire);
	if (leaf == nullptr)
		return nullptr;
	return const_cast<Entry *>(&(*leaf)[id & ((1U << leaf_bits) - 1U)]);
}

std::uint32_t FrameAccountingDomain::allocate(
	GenerationState &state, std::uint64_t generation)
{
	const std::lock_guard lock{mutex_};
	UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::writers);
	auto id = lowest_free_;
	while (id < next_new_ && entry(id)->taken)
		++id;
	if (id == next_new_) {
		if (id > maximum_alias)
			throw std::bad_alloc{};
		/* The chunks that cover the id, each made before it is published. */
		auto *root = root_.load(std::memory_order_relaxed);
		if (root == nullptr) {
			auto *made = static_cast<Root *>(
				frame_allocate(heap(), sizeof(Root), alignof(Root)));
			root = new (made) Root{};
			root_.store(root, std::memory_order_release);
		}
		auto &mid_slot = (*root)[id >> (leaf_bits + mid_bits)];
		auto *mid = mid_slot.load(std::memory_order_relaxed);
		if (mid == nullptr) {
			auto *made = static_cast<Mid *>(
				frame_allocate(heap(), sizeof(Mid), alignof(Mid)));
			mid = new (made) Mid{};
			mid_slot.store(mid, std::memory_order_release);
		}
		auto &leaf_slot =
			(*mid)[(id >> leaf_bits) & ((1U << mid_bits) - 1U)];
		if (leaf_slot.load(std::memory_order_relaxed) == nullptr) {
			auto *made = static_cast<Leaf *>(
				frame_allocate(heap(), sizeof(Leaf), alignof(Leaf)));
			leaf_slot.store(new (made) Leaf{}, std::memory_order_release);
		}
		++next_new_;
	}
	auto *slot = entry(id);
	slot->taken = true;
	slot->holders.store(0, std::memory_order_relaxed);
	slot->generation.store(generation, std::memory_order_relaxed);
	slot->state.store(&state, std::memory_order_release);
	lowest_free_ = id + 1U;
	return id;
}

void FrameAccountingDomain::release(std::uint32_t id, bool leaked) noexcept
{
	auto *slot = entry(id);
	if (slot == nullptr)
		return;
	const std::lock_guard lock{mutex_};
	slot->state.store(nullptr, std::memory_order_release);
	if (leaked)
		return;
	slot->taken = false;
	lowest_free_ = std::min(lowest_free_, id);
}

WriterSettler::~WriterSettler()
{
	FrameAccounting::settle(state_);
}

void WriterReservation::add(GenerationState &state, MappedBacking &backing,
	std::uint64_t offset, std::uint64_t page)
{
	auto &accounting = state.accounting;
	/* A walked state belongs to a generation that is gone; its callers hold
	 * no reference they could reach it by, so this is an error, and it
	 * fails the primitive like any other the primitive cannot complete. */
	if (accounting.walked)
		throw std::bad_alloc{};
	accounting.used.store(true, std::memory_order_relaxed);
	UML_MEMORY_ALLOCATION_CHECKPOINT(testing::AllocationSite::writers);
	/* The room for the entry is had before the location is added, so that
	 * recording it cannot fail. The entries live in the thread's scratch
	 * arena, which the plan's scope keeps until commit or settle, and the
	 * room doubles, so a plan of any size reserves it in a few steps. */
	if (!scope_)
		scope_.emplace();
	if (size_ == capacity_) {
		const auto room = std::max<std::size_t>(16, capacity_ * 2U);
		auto *items = static_cast<Added *>(
			scope_->arena().allocate(room * sizeof(Added), alignof(Added)));
		if (size_ != 0)
			std::memcpy(items, items_, size_ * sizeof(Added));
		items_ = items;
		capacity_ = room;
		reservation_growths.fetch_add(1, std::memory_order_relaxed);
	}
	if (FrameAccounting::add_writer(backing, offset, {accounting.alias, page}))
		items_[size_++] = {&backing, offset, page};
}

void WriterReservation::settle(std::uint32_t alias) noexcept
{
	if (size_ != 0)
		reservation_rollbacks.fetch_add(1, std::memory_order_relaxed);
	for (auto added = size_; added != 0; --added)
		FrameAccounting::remove_writer(*items_[added - 1U].backing,
			items_[added - 1U].offset, {alias, items_[added - 1U].page});
	release();
}

void WriterReservation::release() noexcept
{
	items_ = nullptr;
	size_ = 0;
	capacity_ = 0;
	/* The outermost scope's end hands the arena's extra chunks back. */
	scope_.reset();
}

} // namespace uml::memory

#endif
