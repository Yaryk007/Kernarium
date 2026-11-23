// SPDX-License-Identifier: GPL-2.0-or-later
/* The verdicts the address spaces of a process share (shared_verdicts.hpp)
 * against a stub of the memory layer's page stamp: the table's own protocol,
 * and the decision of when a page may be answered from it and when filed. */

#include "cpu/arm64/aot/shared_verdicts.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

namespace aot = uml::cpu::arm64::aot;
using aot::PageStamp;
using aot::SharedOutcome;
using aot::VerdictTable;

#define require(condition)                                                  \
	do {                                                                \
		if (!(condition)) {                                         \
			std::fprintf(stderr, "%s:%d: requirement failed: %s\n", \
				__FILE__, __LINE__, #condition);            \
			std::abort();                                       \
		}                                                           \
	} while (false)

namespace {

constexpr std::uint64_t where_a = VerdictTable::where(3, 17);
constexpr std::uint64_t where_b = VerdictTable::where(3, 18);

void basic_rows()
{
	VerdictTable table;
	const PageStamp stamp{101, 7, true};
	require(!table.contains(stamp, where_a));
	require(table.insert(stamp, where_a));
	require(table.contains(stamp, where_a));
	/* A different library page, epoch, identity or fact is a different
	 * verdict. */
	require(!table.contains(stamp, where_b));
	require(!table.contains({101, 8, true}, where_a));
	require(!table.contains({102, 7, true}, where_a));
	require(!table.contains({101, 7, false}, where_a));
	/* Not immutable: never filed, never answered. */
	require(!table.insert({205, 1, false}, where_a));
	require(!table.contains({205, 1, false}, where_a));
	/* Identity 0 is never issued; a cleared slot matches nothing. */
	require(!table.insert({0, 1, true}, where_a));
	require(!table.contains({0, 0, true}, 0));
	/* A later filing in the same slot replaces the earlier one. */
	std::uint64_t other = 102;
	while (VerdictTable::slot_of(other) != VerdictTable::slot_of(101))
		++other;
	require(table.insert({other, 1, true}, where_b));
	require(table.contains({other, 1, true}, where_b));
	require(!table.contains(stamp, where_a));
	table.clear();
	require(!table.contains({other, 1, true}, where_b));
	require(table.insert(stamp, where_a) && table.contains(stamp, where_a));
	std::fprintf(stderr, "basic rows passed\n");
}

/* Identities that all map to one slot, so every writer and reader meets. */
std::vector<std::uint64_t> colliding_identities(std::size_t count)
{
	std::vector<std::uint64_t> out;
	const auto slot = VerdictTable::slot_of(1);
	for (std::uint64_t id = 1; out.size() < count; ++id)
		if (VerdictTable::slot_of(id) == slot)
			out.push_back(id);
	return out;
}

/* The three words of verdict i; a stored triple that mixes two verdicts is
 * the failure. */
PageStamp stamp_of(const std::vector<std::uint64_t> &ids, std::size_t i)
{
	return {ids[i], 1000 + 3 * i, true};
}

std::uint64_t at_of(std::size_t i) { return VerdictTable::where(5, 40 + i); }

/* Two writers and a reader forced onto one slot. Every combination of the
 * words of two verdicts is asked: only the whole verdicts may ever hit. */
void writers_and_a_reader_on_one_slot()
{
	constexpr std::size_t kinds = 6;
	const auto ids = colliding_identities(kinds);
	VerdictTable table;
	std::atomic<bool> stop{false};
	std::atomic<std::uint64_t> inserts{0}, dropped{0}, hits{0};
	const auto writer = [&](std::size_t from) {
		std::size_t i = from;
		while (!stop.load(std::memory_order_relaxed)) {
			if (table.insert(stamp_of(ids, i % kinds), at_of(i % kinds)))
				inserts.fetch_add(1, std::memory_order_relaxed);
			else
				dropped.fetch_add(1, std::memory_order_relaxed);
			++i;
		}
	};
	std::atomic<bool> torn{false};
	const auto reader = [&] {
		while (!stop.load(std::memory_order_relaxed))
			for (std::size_t a = 0; a < kinds; ++a)
				for (std::size_t b = 0; b < kinds; ++b)
					for (std::size_t c = 0; c < kinds; ++c) {
						const PageStamp asked{ids[a], 1000 + 3 * b, true};
						if (!table.contains(asked, at_of(c)))
							continue;
						hits.fetch_add(1, std::memory_order_relaxed);
						if (a != b || b != c)
							torn.store(true);
					}
	};
	std::thread w1{writer, 0}, w2{writer, 3}, r1{reader}, r2{reader};
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	stop.store(true);
	w1.join();
	w2.join();
	r1.join();
	r2.join();
	std::fprintf(stderr, "one slot: %llu inserts, %llu dropped, %llu hits\n",
		static_cast<unsigned long long>(inserts.load()),
		static_cast<unsigned long long>(dropped.load()),
		static_cast<unsigned long long>(hits.load()));
	require(!torn.load());
	require(inserts.load() > 1000);
	require(hits.load() > 0);
}

/* The protocol without writer ownership, stepped by hand: both writers read
 * an even sequence, both make it odd, and their words interleave. The reader
 * then finds an even, unchanged sequence around words of two verdicts. This
 * is what the compare-and-swap rules out; the row shows that the property the
 * stress row asserts is the one that fails without it. */
void the_protocol_without_ownership_tears()
{
	struct Slot {
		std::uint64_t seq{0}, identity{0}, epoch{0}, where{0};
	} slot;
	const auto a = PageStamp{11, 1100, true};
	const auto b = PageStamp{22, 2200, true};
	const std::uint64_t at_a = at_of(1), at_b = at_of(2);
	/* Both writers read the same even sequence. */
	const auto seq_a = slot.seq, seq_b = slot.seq;
	slot.seq = seq_a + 1;            /* writer A: odd */
	slot.seq = seq_b + 1;            /* writer B: odd (the same value) */
	slot.identity = a.identity;      /* A */
	slot.epoch = a.epoch;            /* A */
	slot.identity = b.identity;      /* B */
	slot.epoch = b.epoch;            /* B */
	slot.where = at_a;               /* A */
	slot.seq = seq_a + 2;            /* A: even */
	/* B never finishes; a reader sees an even, stable sequence. */
	const bool stable = (slot.seq & 1U) == 0;
	require(stable);
	require(slot.identity == b.identity && slot.epoch == b.epoch &&
		slot.where == at_a);
	/* Identity and epoch of B with the place of A: not a verdict anyone
	 * filed. */
	require(!(slot.where == at_b));
}

struct StubMemory final {
	bool stamped{true};
	PageStamp stamp{900, 1, true};
	std::function<void()> on_fetch;
	std::uint64_t fetches{0};
	SharedOutcome content{SharedOutcome::match};
	/* The verifying space's mapping-change sequence moved since the
	 * verification began. */
	bool moved{false};
	/* Verifications the epoch forced, as a diagnostics build counts them. */
	std::uint64_t epoch_forced{0};

	SharedOutcome verify(VerdictTable &table, std::uint64_t at)
	{
		return aot::verify_through_table(table,
			[&](std::uint64_t, PageStamp &out) {
				out = stamp;
				return stamped;
			},
			[&] {
				++fetches;
				if (on_fetch)
					on_fetch();
				return content;
			},
			[&] { return moved; },
			0x7000'0000, at, [&] { ++epoch_forced; });
	}
};

void deciding_rows()
{
	VerdictTable table;
	StubMemory memory;
	/* The first sight hashes and files; the next is answered. */
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.fetches == 1);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	require(memory.fetches == 1);
	/* The same physical page as another library page is another verdict. */
	require(memory.verify(table, where_b) == SharedOutcome::match);
	require(memory.fetches == 2);
	/* A published change or a last departure moved the epoch: hashed again,
	 * then filed under the new epoch. */
	memory.stamp.epoch = 2;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.fetches == 3);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	/* A frame reused under the same address has another identity. */
	memory.stamp = {901, 1, true};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.fetches == 4);
	/* Execute removal against a warmed hit: the mapping can no longer give
	 * a stamp, so the table is not consulted and the ordinary path (here: the
	 * fetch refused as it is for a page without execute permission) runs. */
	memory.stamped = false;
	memory.content = SharedOutcome::unreadable;
	require(memory.verify(table, where_a) == SharedOutcome::unreadable);
	require(memory.fetches == 5);
	memory.stamped = true;
	memory.content = SharedOutcome::match;
	/* A mapping without the immutable fact never shares, though another
	 * mapping of the same page (same identity and epoch) has filed. */
	memory.stamp = {902, 1, true};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	memory.stamp.immutable = false;
	const auto before = memory.fetches;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.fetches == before + 1);
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.fetches == before + 2);
	/* A page that does not match is never filed. */
	memory.stamp = {903, 1, true};
	memory.content = SharedOutcome::mismatch;
	require(memory.verify(table, where_a) == SharedOutcome::mismatch);
	require(memory.verify(table, where_a) == SharedOutcome::mismatch);
	require(memory.fetches == before + 4);
	memory.content = SharedOutcome::match;
	/* The stamp moving during the fetch (replaced, published, unmapped and
	 * mapped again) files nothing. */
	for (int field = 0; field < 3; ++field) {
		memory.stamp = {910 + static_cast<std::uint64_t>(field), 1, true};
		memory.on_fetch = [&] {
			if (field == 0)
				++memory.stamp.identity;
			else if (field == 1)
				++memory.stamp.epoch;
			else
				memory.stamp.immutable = false;
		};
		const auto fetched = memory.fetches;
		require(memory.verify(table, where_b) == SharedOutcome::match);
		memory.on_fetch = nullptr;
		/* Back to the stamp read before the fetch: nothing was filed under
		 * it, so it is hashed again. */
		memory.stamp = {910 + static_cast<std::uint64_t>(field), 1, true};
		require(memory.verify(table, where_b) == SharedOutcome::match);
		require(memory.fetches == fetched + 2);
	}
	/* The verifying space's mapping changes while the page is fetched (which
	 * the execution gate forbids; Rule S does not rely on it alone): what was
	 * fetched may not be the frame the stamps name, so the verification
	 * answers neither a match nor a mismatch and files nothing, however
	 * equal the stamps are before and after. */
	memory.stamp = {930, 1, true};
	memory.on_fetch = [&] { memory.moved = true; };
	const auto fetched = memory.fetches;
	require(memory.verify(table, where_a) == SharedOutcome::unreadable);
	memory.content = SharedOutcome::mismatch;
	require(memory.verify(table, where_a) == SharedOutcome::unreadable);
	memory.content = SharedOutcome::unreadable;
	require(memory.verify(table, where_a) == SharedOutcome::unreadable);
	memory.content = SharedOutcome::match;
	memory.on_fetch = nullptr;
	memory.moved = false;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.fetches == fetched + 4);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	/* A sequence that moved before the verification began is not a move. */
	memory.stamp = {931, 1, true};
	memory.moved = false;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	/* A page hashed because the epoch of its frame moved is told apart from
	 * every other hash: the table holds the same frame's verdict for the
	 * same page under another epoch. The first sight, a page of another
	 * frame, another page of the frame, a mapping without the fact and a
	 * page that was never stamped are not it. */
	memory.stamp = {940, 1, true};
	memory.moved = false;
	const auto forced_before = memory.epoch_forced;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	require(memory.epoch_forced == forced_before);
	memory.stamp.epoch = 2;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.epoch_forced == forced_before + 1);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	require(memory.epoch_forced == forced_before + 1);
	memory.stamp = {941, 1, true};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	memory.stamp = {942, 1, true};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	memory.stamp.epoch = 2;
	require(memory.verify(table, where_b) == SharedOutcome::match);
	memory.stamp = {943, 1, true};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	memory.stamp = {943, 2, false};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	memory.stamped = false;
	require(memory.verify(table, where_a) == SharedOutcome::match);
	memory.stamped = true;
	require(memory.epoch_forced == forced_before + 1);
	require(!table.filed_under_another_epoch({0, 2, true}, where_a));
	/* A caller that passes no observer compiles the question away. */
	require(aot::verify_through_table(table,
		[](std::uint64_t, PageStamp &out) {
			out = {944, 1, true};
			return true;
		},
		[] { return SharedOutcome::match; }, [] { return false; },
		0x7000'0000, where_a) == SharedOutcome::match);
	/* Clearing at hand-back: nothing hits afterwards. */
	memory.stamp = {920, 1, true};
	require(memory.verify(table, where_a) == SharedOutcome::match);
	require(memory.verify(table, where_a) == SharedOutcome::shared_hit);
	table.clear();
	require(memory.verify(table, where_a) == SharedOutcome::match);
	std::fprintf(stderr, "deciding rows passed\n");
}

} // namespace

int main()
{
	basic_rows();
	the_protocol_without_ownership_tears();
	writers_and_a_reader_on_one_slot();
	deciding_rows();
	return 0;
}
