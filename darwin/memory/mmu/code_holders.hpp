// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

/*
 * Which address spaces hold a translation of a frame of guest code.
 *
 * An arm64 guest that rewrites code and synchronizes it (IC IVAU, which is
 * broadcast and works on the physical address) changes what every process
 * that runs the frame executes, but a code change is published in one
 * generation, the writer's. So each frame has a holder word in its backing
 * (MappedBacking::holder_word()), and the two sides meet in it:
 *
 *  - a translator registers its generation in the frame's word before it
 *    reads the frame's bytes, at the instruction-fetch path;
 *  - a code synchronization or a kernel write into the frame takes the word
 *    and publishes a whole-space code change in every generation it named.
 *
 * Taking the word consumes it: a publication marks the holder's translations
 * stale, and the holder cannot run them again without fetching, and so
 * registering again. That keeps a word from holding every process that ever
 * ran the frame.
 * Until the publication that took a word has finished, though, an empty word
 * does not mean its readers are told: HolderList counts such a publisher, and
 * a publisher that meets the count publishes to every generation.
 *
 * The two sides are a store and then a load of the other's variable (the
 * translator stores its registration and loads the bytes, the writer stores
 * the bytes and loads the word), so each side puts a sequentially consistent
 * fence between them; acquire and release do not order such a pair.
 */
namespace uml::memory::code_holders {

using Word = std::atomic<std::uint64_t>;
static_assert(Word::is_always_lock_free);

/* Nobody has translated the frame since its last publication. */
inline constexpr std::uint64_t none = 0;
/* Any generation may have. */
inline constexpr std::uint64_t all = ~std::uint64_t{0};
/* A slot holds 0 (empty) or a generation in [1, max_generation]. A kernel
 * generation is a 32-bit counter. */
inline constexpr std::uint32_t slot_all = ~std::uint32_t{0};
inline constexpr std::uint64_t max_generation = slot_all - 1U;
/* One leaf of a backing's table covers this many frames: 4 KiB of words. */
inline constexpr std::size_t leaf_frames = 512;
using Leaf = std::array<Word, leaf_frames>;

/* The slot a generation registers in: itself, or slot_all for one a slot
 * cannot hold, which registers as `all`. */
[[nodiscard]] constexpr std::uint32_t slot_of(std::uint64_t generation) noexcept
{
	return generation >= 1 && generation <= max_generation ?
		static_cast<std::uint32_t>(generation) : slot_all;
}

[[nodiscard]] constexpr std::uint32_t low(std::uint64_t word) noexcept
{
	return static_cast<std::uint32_t>(word);
}

[[nodiscard]] constexpr std::uint32_t high(std::uint64_t word) noexcept
{
	return static_cast<std::uint32_t>(word >> 32);
}

[[nodiscard]] constexpr std::uint64_t join(
	std::uint32_t first, std::uint32_t second) noexcept
{
	return static_cast<std::uint64_t>(second) << 32 | first;
}

/* Whether the word already names the slot, or every generation. */
[[nodiscard]] constexpr bool holds(
	std::uint64_t word, std::uint32_t slot) noexcept
{
	return word == all || low(word) == slot || high(word) == slot;
}

/* Whether the word names nobody but `slot`. A generation a slot cannot hold
 * is never judged alone. */
[[nodiscard]] constexpr bool holds_only(
	std::uint64_t word, std::uint32_t slot) noexcept
{
	return word != all && slot != slot_all &&
		(low(word) == 0 || low(word) == slot) &&
		(high(word) == 0 || high(word) == slot);
}

/* The generations a publication goes to, or every one. A word names two, so
 * a few words' worth are kept before the set widens to every generation.
 *
 * Emptying a word makes its readers the list's to tell: until the list's
 * publication has finished, an empty word does not mean its readers know.
 * So a list that has emptied a word is counted in a takes-in-flight counter
 * (begin_take) until it is destroyed, and a publisher that finds the counter
 * above its own share publishes to every generation (AddressSpace::
 * take_holders). Keep the list alive until its publication returns. */
class HolderList final {
public:
	static constexpr std::size_t capacity = 8;

	HolderList() = default;
	HolderList(const HolderList &) = delete;
	HolderList &operator=(const HolderList &) = delete;
	~HolderList()
	{
		if (in_flight_ != nullptr)
			in_flight_->fetch_sub(1, std::memory_order_release);
	}

	/* Counts this list in `counter`, once, before its first word is emptied.
	 * The increment is ordered before the exchange that empties the word, so
	 * whoever reads that word empty reads the count too. */
	void begin_take(std::atomic<std::uint64_t> &counter) noexcept
	{
		if (in_flight_ != nullptr)
			return;
		counter.fetch_add(1, std::memory_order_seq_cst);
		in_flight_ = &counter;
	}
	/* How many takes in flight the list's own, if any, accounts for. */
	[[nodiscard]] std::uint64_t own_take() const noexcept
	{
		return in_flight_ != nullptr ? 1 : 0;
	}

	/* Adds what a consumed word named, but `except`'s slot. */
	void add(std::uint64_t word, std::uint32_t except) noexcept
	{
		if (word == all) {
			every_ = true;
			return;
		}
		add_slot(low(word), except);
		add_slot(high(word), except);
	}

	/* Every live generation, whatever the word said. */
	void add_every() noexcept { every_ = true; }

	[[nodiscard]] bool every() const noexcept { return every_; }
	[[nodiscard]] bool empty() const noexcept
	{
		return !every_ && count_ == 0;
	}
	[[nodiscard]] std::span<const std::uint32_t> generations() const noexcept
	{
		return {slots_.data(), count_};
	}

private:
	void add_slot(std::uint32_t slot, std::uint32_t except) noexcept
	{
		if (slot == 0 || slot == except || every_)
			return;
		for (std::size_t index = 0; index < count_; ++index)
			if (slots_[index] == slot)
				return;
		if (count_ == slots_.size()) {
			every_ = true;
			return;
		}
		slots_[count_++] = slot;
	}

	std::array<std::uint32_t, capacity> slots_{};
	std::size_t count_{};
	bool every_{};
	std::atomic<std::uint64_t> *in_flight_{};
};

/* Starts the periodic counters report the process asked for with
 * DARWIN_UML_CODE_HOLDER_COUNTERS, once; a no-op without it. A table's first leaf
 * calls it. */
void report_counters_once() noexcept;

/* What the mechanism did since the process began. Each counter is bumped on a
 * cold path (a registration that changed a word, a publication that found
 * other holders), never per fetch or per instruction-cache line. */
struct Counters final {
	/* A generation added to a frame's word, and the adds that filled the
	 * word and made it `all`. */
	std::uint64_t registrations{};
	std::uint64_t overflows{};
	/* Words a publication took with an exchange; one that finds nobody but
	 * the writer, or nobody, takes none. */
	std::uint64_t exchanges{};
	/* Publications that found other holders, those that went to every
	 * generation, and the generations they published in. */
	std::uint64_t publications{};
	std::uint64_t all_publications{};
	std::uint64_t generations_published{};
	/* Leaves of the tables made, and backings that could not make one and
	 * so publish to every generation. */
	std::uint64_t leaves{};
	std::uint64_t unknown_backings{};
	/* Publications that found another publisher's take unfinished and so went
	 * to every generation. */
	std::uint64_t overlap_widenings{};
	/* Pages a kernel write named that the target's software table had no
	 * entry for, each of which widened its publication to every generation. */
	std::uint64_t absent_kernel_pages{};
	/* The exchanges (above) the kernel path made, which tell a kernel write's
	 * take from a guest synchronization's. */
	std::uint64_t kernel_exchanges{};
};

namespace detail {

struct CounterStore final {
	std::atomic_uint64_t registrations{};
	std::atomic_uint64_t overflows{};
	std::atomic_uint64_t exchanges{};
	std::atomic_uint64_t publications{};
	std::atomic_uint64_t all_publications{};
	std::atomic_uint64_t generations_published{};
	std::atomic_uint64_t leaves{};
	std::atomic_uint64_t unknown_backings{};
	std::atomic_uint64_t overlap_widenings{};
	std::atomic_uint64_t absent_kernel_pages{};
	std::atomic_uint64_t kernel_exchanges{};
};

[[nodiscard]] inline CounterStore &counter_store() noexcept
{
	static CounterStore store;
	return store;
}

} // namespace detail

inline void count(std::atomic_uint64_t detail::CounterStore::*counter,
	std::uint64_t amount = 1) noexcept
{
	(detail::counter_store().*counter)
		.fetch_add(amount, std::memory_order_relaxed);
}

[[nodiscard]] inline Counters counters() noexcept
{
	auto &store = detail::counter_store();
	const auto read = [](const std::atomic_uint64_t &value) {
		return value.load(std::memory_order_relaxed);
	};
	return {read(store.registrations), read(store.overflows),
		read(store.exchanges), read(store.publications),
		read(store.all_publications), read(store.generations_published),
		read(store.leaves), read(store.unknown_backings),
		read(store.overlap_widenings), read(store.absent_kernel_pages),
		read(store.kernel_exchanges)};
}

} // namespace uml::memory::code_holders
