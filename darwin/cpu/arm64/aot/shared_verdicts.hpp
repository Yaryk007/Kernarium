// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Verdicts shared by the address spaces of one process.
 *
 * Every address space that maps a library verifies the pages it maps against
 * the library's stored SHA-256 once, and a process has about 300 address
 * spaces a boot that map the same few thousand physical pages. A verdict
 * "this physical page held the bytes of library L's page k" can be shared by
 * every address space only while it is certain that the page still holds
 * them, which the memory layer states as a PageStamp:
 *
 *   identity   names one physical-page record and is never reused;
 *   epoch      moves when the page's last executable mapping goes and at every
 *              publication of a change to its code;
 *   immutable  says that THIS mapping is of a page of a file system that
 *              cannot change (the guest kernel's statement, per mapping).
 *
 * A page that is not immutable is never filed and never answered. The table
 * is a hint that is allowed to forget: an entry dropped, overwritten or
 * missed is always safe, because the caller then hashes the page itself. */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace uml::cpu::arm64::aot {

struct PageStamp final {
	std::uint64_t identity{};
	std::uint64_t epoch{};
	bool immutable{};
	[[nodiscard]] friend bool operator==(
		const PageStamp &, const PageStamp &) noexcept = default;
};

/* One slot is {seq, identity, epoch, where}. A writer owns the slot between
 * its even-to-odd compare-and-swap and its final store, so two writers never
 * interleave their words, and an insert that finds the slot owned is
 * dropped. A reader takes the words between two reads of the sequence and
 * refuses anything the sequence moved under. Identity 0 is never issued, so
 * a cleared slot matches nothing. */
class VerdictTable final {
public:
	static constexpr std::size_t slot_bits = 15;
	static constexpr std::size_t slot_count = std::size_t{1} << slot_bits;

	[[nodiscard]] static constexpr std::uint64_t where(
		std::uint32_t library, std::uint32_t page) noexcept
	{
		return (static_cast<std::uint64_t>(library) << 32) | page;
	}

	[[nodiscard]] static constexpr std::size_t slot_of(
		std::uint64_t identity) noexcept
	{
		return static_cast<std::size_t>(
			(identity * 0x9e37'79b9'7f4a'7c15ULL) >> (64 - slot_bits));
	}

	/* Files the verdict; false when the slot was being written (dropped). */
	bool insert(const PageStamp &stamp, std::uint64_t at) noexcept
	{
		if (!stamp.immutable || stamp.identity == 0)
			return false;
		auto &slot = slots_[slot_of(stamp.identity)];
		auto seq = slot.seq.load(std::memory_order_relaxed);
		if ((seq & 1U) != 0 ||
		    !slot.seq.compare_exchange_strong(seq, seq + 1,
			    std::memory_order_acquire, std::memory_order_relaxed))
			return false;
		std::atomic_thread_fence(std::memory_order_release);
		write(slot, stamp.identity, stamp.epoch, at);
		slot.seq.store(seq + 2, std::memory_order_release);
		return true;
	}

	[[nodiscard]] bool contains(
		const PageStamp &stamp, std::uint64_t at) const noexcept
	{
		if (!stamp.immutable || stamp.identity == 0)
			return false;
		const auto &slot = slots_[slot_of(stamp.identity)];
		const auto first = slot.seq.load(std::memory_order_acquire);
		if ((first & 1U) != 0)
			return false;
		const auto identity = slot.identity.load(std::memory_order_relaxed);
		const auto epoch = slot.epoch.load(std::memory_order_relaxed);
		const auto filed = slot.where.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		return slot.seq.load(std::memory_order_relaxed) == first &&
			identity == stamp.identity && epoch == stamp.epoch &&
			filed == at;
	}

	/* Whether the slot of this page frame holds a verdict for the same page
	 * of the same library under another epoch: the one contains() would
	 * have answered had the epoch not moved. A diagnostics build asks it
	 * when it hashes a page the table did not answer, to count the
	 * verifications that the epoch forced (a frame the page cache gave back,
	 * a publication); nothing else reads it. */
	[[nodiscard]] bool filed_under_another_epoch(
		const PageStamp &stamp, std::uint64_t at) const noexcept
	{
		if (!stamp.immutable || stamp.identity == 0)
			return false;
		const auto &slot = slots_[slot_of(stamp.identity)];
		const auto first = slot.seq.load(std::memory_order_acquire);
		if ((first & 1U) != 0)
			return false;
		const auto identity = slot.identity.load(std::memory_order_relaxed);
		const auto epoch = slot.epoch.load(std::memory_order_relaxed);
		const auto filed = slot.where.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		return slot.seq.load(std::memory_order_relaxed) == first &&
			identity == stamp.identity && epoch != stamp.epoch &&
			filed == at;
	}

	/* Forgets every verdict. Writers own a slot as insert() does; a slot
	 * another thread is writing is waited for. */
	void clear() noexcept
	{
		for (auto &slot : slots_) {
			auto seq = slot.seq.load(std::memory_order_relaxed);
			while ((seq & 1U) != 0 ||
			       !slot.seq.compare_exchange_weak(seq, seq + 1,
				       std::memory_order_acquire,
				       std::memory_order_relaxed)) {
				std::this_thread::yield();
				seq = slot.seq.load(std::memory_order_relaxed);
			}
			std::atomic_thread_fence(std::memory_order_release);
			write(slot, 0, 0, 0);
			slot.seq.store(seq + 2, std::memory_order_release);
		}
	}

private:
	struct alignas(32) Slot final {
		std::atomic<std::uint64_t> seq{0};
		std::atomic<std::uint64_t> identity{0};
		std::atomic<std::uint64_t> epoch{0};
		std::atomic<std::uint64_t> where{0};
	};

	static void write(Slot &slot, std::uint64_t identity,
		std::uint64_t epoch, std::uint64_t at) noexcept
	{
		slot.identity.store(identity, std::memory_order_relaxed);
		slot.epoch.store(epoch, std::memory_order_relaxed);
		slot.where.store(at, std::memory_order_relaxed);
	}

	std::array<Slot, slot_count> slots_{};
};

enum class SharedOutcome : std::uint8_t {
	shared_hit,  /* the table answered; nothing was fetched or hashed */
	match,       /* fetched and hashed, equal to the stored hash */
	mismatch,    /* fetched and hashed, different */
	unreadable,  /* the page could not be fetched */
};

/* The verification of one page of an instance, with the shared table.
 *
 *  stamp(address, out)   the memory layer's stamp of the mapping at address;
 *                        false when it has none (the mapping cannot hold a
 *                        translation: no execute permission, not guest code,
 *                        unmapped), which is the execute contract: such a
 *                        page is never answered from the table;
 *  hash_page()           fetches the page and compares its SHA-256 with the
 *                        stored one: returns match, mismatch or unreadable;
 *  moved()               whether the verifying address space's mapping-change
 *                        sequence moved since the call began;
 *  epoch_forced()        optional, called when the page is hashed because
 *                        the table holds this frame's verdict for the page
 *                        under another epoch.
 *
 * The whole stamp is read before the fetch and, after a match, once more: a
 * verdict is filed only when it is unchanged and immutable, so a page that
 * was replaced, published or unmapped while it was being hashed is not
 * filed. The stamps alone could not tell the frame being replaced and put
 * back in the verifying space, and the bytes fetched through the address are
 * the stamped frame's only because the execution gate keeps that space's
 * mapping changes out while it verifies; the sequence makes the call check
 * what it relies on: a verification during which the space's mappings moved
 * says neither match nor mismatch and files nothing. */
/* The observer of a verification the epoch forced: a diagnostics build
 * passes one that counts, and every other build this one, which makes the
 * question below compile away. */
struct NoEpochObserver final {
	void operator()() const noexcept {}
};

template <class Stamp, class Hash, class Moved,
	class EpochForced = NoEpochObserver>
[[nodiscard]] SharedOutcome verify_through_table(VerdictTable &table,
	Stamp &&stamp, Hash &&hash_page, Moved &&moved, std::uint64_t address,
	std::uint64_t at, EpochForced &&epoch_forced = EpochForced{}) noexcept
{
	PageStamp before;
	const bool stamped = stamp(address, before);
	if (stamped && before.immutable && table.contains(before, at))
		return SharedOutcome::shared_hit;
	if constexpr (!std::is_same_v<std::remove_cvref_t<EpochForced>,
			      NoEpochObserver>) {
		if (stamped && table.filed_under_another_epoch(before, at))
			epoch_forced();
	}
	const SharedOutcome result = hash_page();
	if (moved())
		return SharedOutcome::unreadable;
	if (result == SharedOutcome::match && stamped && before.immutable) {
		PageStamp after;
		if (stamp(address, after) && after == before)
			(void)table.insert(before, at);
	}
	return result;
}

} // namespace uml::cpu::arm64::aot
