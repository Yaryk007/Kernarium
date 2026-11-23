// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"

#if DARWIN_UML_X86_64_GUEST

#include "frame_heap.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace uml::memory {

struct GenerationState;

/* The frame accounting's state of one memory domain: its heap, and the table
 * that says which generation state an alias id names.
 *
 * A location in a frame (a writer, or a holder) names a generation state by
 * an alias id of 26 bits, so that a location fits a word. Ids are per domain:
 * every frame that carries a location is on a backing of the domain's address
 * space, so the table that resolves it is the one that space made. The domain
 * is held by the address space, every generation state and every backing,
 * which are the only readers, so the table outlives them.
 *
 * The entries sit in stable chunks of a three-level radix that never move
 * before the domain is gone (a leaf of 1024 entries is 32 KiB), so a reader (a
 * publication under a stripe lock, a registration) finds an entry with
 * acquiring loads and takes no lock. The id
 * mutex serializes allocating and freeing an id, which happen at a state's
 * creation and in its `finish`; no stripe lock is ever held across it. The
 * chunks come from the domain's heap. */
class FrameAccountingDomain final {
public:
	/* Alias ids are 26 bits so that a location fits a frame's word. */
	static constexpr std::uint32_t maximum_alias = (1U << 26) - 1U;

	struct Entry final {
		/* The state the id names; null while the id is free or retired. */
		std::atomic<GenerationState *> state{};
		/* Its generation number, for a holder's publication. */
		std::atomic_uint64_t generation{};
		/* The holders that name this id, moved under the stripe lock of the
		 * frame whose list changes. */
		std::atomic_uint64_t holders{};
		/* Guarded by the id mutex: the id is in use, or retired for good. */
		bool taken{};
	};

	explicit FrameAccountingDomain(std::shared_ptr<DomainHeap> heap) noexcept
		: heap_{std::move(heap)}
	{
	}
	~FrameAccountingDomain();
	FrameAccountingDomain(const FrameAccountingDomain &) = delete;
	FrameAccountingDomain &operator=(const FrameAccountingDomain &) = delete;

	/* The heap the domain's frame metadata lives in, or null for the
	 * process's. */
	[[nodiscard]] DomainHeap *heap() const noexcept { return heap_.get(); }

	/* The lowest free id, now naming `state` of `generation`. Throws
	 * std::bad_alloc, with nothing taken, when no chunk can be had or the ids
	 * are exhausted. */
	[[nodiscard]] std::uint32_t allocate(
		GenerationState &state, std::uint64_t generation);
	/* Names no state any more. With `leaked`, the id stays taken for good:
	 * a location may still name it, and a publication skips an entry that
	 * names no state. */
	void release(std::uint32_t id, bool leaked) noexcept;

	/* Lock-free; null for an id no chunk covers yet. */
	[[nodiscard]] Entry *entry(std::uint32_t id) const noexcept;
	[[nodiscard]] GenerationState *state_of(std::uint32_t id) const noexcept
	{
		const auto *found = entry(id);
		return found == nullptr ? nullptr :
			found->state.load(std::memory_order_acquire);
	}

private:
	static constexpr unsigned leaf_bits = 10;
	static constexpr unsigned mid_bits = 8;
	static constexpr unsigned root_bits = 8;
	static_assert(leaf_bits + mid_bits + root_bits == 26);
	using Leaf = std::array<Entry, std::size_t{1} << leaf_bits>;
	static_assert(sizeof(Leaf) == std::size_t{32} * 1024,
		"the alias table's leaf chunk is 32 KiB");
	using Mid = std::array<std::atomic<Leaf *>, std::size_t{1} << mid_bits>;
	using Root = std::array<std::atomic<Mid *>, std::size_t{1} << root_bits>;

	/* Declared first, so that it outlives the chunks it served. */
	std::shared_ptr<DomainHeap> heap_;
	std::mutex mutex_;
	std::atomic<Root *> root_{};
	/* Guarded by mutex_. */
	std::uint32_t next_new_{1};
	std::uint32_t lowest_free_{1};
};

} // namespace uml::memory

#endif
