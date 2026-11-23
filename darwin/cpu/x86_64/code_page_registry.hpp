// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/software_tlb.hpp"
#include "memory/guest_address_space.hpp"
#include "memory/mmu/word_shared_mutex.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace uml::cpu::x86_64 {

/* Which guest pages hold a cached translation in any executor of one address
 * space.
 *
 * Linux marks every data page of a READ_IMPLIES_EXEC process executable, so
 * "this page may hold code" is true for the stack and the heap, and a store
 * that publishes on that predicate publishes on almost every store. What a
 * publication protects is a cached translation, and only the executors know
 * where those are. Every executor running the address space reports the pages
 * its cache covers here; the miss handler tags a write translation for
 * publication only when the page is on the list.
 *
 * The list is shared between vCPU threads, so two things keep it exact.
 * Adding a page retires every participant's write translation for it and
 * bumps a change count that the miss handler re-reads after filling, so no
 * untagged translation can outlive the moment the page gained code. And a
 * participant that is inside the engine may already have translated a store
 * it has not performed, so the adder waits until every such participant has
 * left the engine once -- bounded by the run's instruction window -- before
 * the translation reads the page's bytes.
 *
 * Membership is exact: a count per page. A bit per hashed page in front of it
 * answers "no" with one load, which is the answer for almost every page a
 * guest writes, and only a set bit is confirmed against the counts. The bits
 * alone are not enough, because the hash keeps neighbours adjacent and a run of
 * code pages shadows a run of data pages the same length: when that run was a
 * thread's stack, every block that pushed published a code change, the
 * sequence moved faster than any other vCPU could validate a cached block
 * against it, and all of them translated every block again for as long as the
 * process lived. Where a stack lands is up to the guest's address-space
 * randomization, so one run in ten lost nine tenths of its speed.
 *
 * Every executor counts a page in when it caches a translation there and out
 * when it drops one, and a guest whose code outgrows the caches does both
 * hundreds of thousands of times a second across its vCPUs. The counts are
 * therefore guarded in stripes by a lock that never sleeps; behind one
 * std::mutex the vCPUs of a multi-core compile spent a tenth of their busy
 * time queued in the kernel for it.
 */
class CodePageRegistry final {
public:
	struct Participant final {
		NativeTlbEntry *write_translations{};
		/* Odd while the participant is inside the engine. */
		std::atomic<std::uint64_t> engine_epoch{0};
	};
	/* How long a registration waits for the participants inside the engine:
	 * `slices` slices of `slice`. A participant still inside the engine
	 * after that is stuck, for instance a thread the host stopped. */
	struct WaitPolicy final {
		std::chrono::milliseconds slice{200};
		unsigned slices{5};
	};
	/* A stuck participant, as the join that made it a member and the odd
	 * epoch it did not leave. */
	struct StuckMember final {
		std::uint64_t serial{};
		std::uint64_t epoch{};
	};
	/* What a registration settled. A registration is settled when every
	 * participant that could still land a store through a write translation
	 * retired for the page has left the engine since: only then may the
	 * caller cache a translation of the page. Otherwise the caller must not
	 * cache one, and `stuck` names the participants it waited out; each
	 * stays stuck until its epoch moves (`left_or_moved`). */
	struct Settlement final {
		bool settled{true};
		std::vector<StuckMember> stuck;
	};
	/* Retires the write translation a participant whose software TLB has
	 * another layout holds for `page`, if it holds one. Any thread may call
	 * it, while the participant runs. */
	using RetireWriteTranslation = void (*)(
		void *context, std::uint64_t page) noexcept;

	/* One registry per guest address-space generation, shared for as long as
	 * any executor holds it. */
	[[nodiscard]] static std::shared_ptr<CodePageRegistry> acquire(
		std::uint64_t generation);
	/* The same, made with `policy` when no executor holds one yet. */
	[[nodiscard]] static std::shared_ptr<CodePageRegistry> acquire(
		std::uint64_t generation, const WaitPolicy &policy);
	/* The registry of `generation` while some executor holds it, and null
	 * otherwise; it never makes one. The caller's reference keeps the
	 * registry alive, so a registry cannot go away under a caller that
	 * found it. */
	[[nodiscard]] static std::shared_ptr<CodePageRegistry> find(
		std::uint64_t generation);

	CodePageRegistry();
	explicit CodePageRegistry(const WaitPolicy &policy);
	~CodePageRegistry();
	CodePageRegistry(const CodePageRegistry &) = delete;
	CodePageRegistry &operator=(const CodePageRegistry &) = delete;

	void join(Participant &participant);
	/* A participant that leaves `write_translations` null retires its own
	 * entries through `retire`. */
	void join(Participant &participant, RetireWriteTranslation retire,
		void *context);
	/* Takes `participant` out of the registry. A waiter that is waiting for
	 * it stops waiting, and the participant may be destroyed once this
	 * returns. The caller is outside the engine: every store the participant
	 * translated has landed. A caller that is not is counted
	 * (`leaves_inside_engine`). */
	void leave(Participant &participant) noexcept;

	[[nodiscard]] bool holds(std::uint64_t address) const noexcept
	{
		const auto bit = bit_index(address);
		if ((bits_[bit >> 6U].load(std::memory_order_relaxed) &
			    (std::uint64_t{1} << (bit & 63U))) == 0)
			return false;
		return holds_exactly(address, bit);
	}
	[[nodiscard]] std::uint64_t changes() const noexcept
	{
		return changes_.load(std::memory_order_acquire);
	}

	/* Called by the executor that is about to translate `page`, before it
	 * reads the page's bytes. The call that gains the page, and any later
	 * one while the gain is not settled, retires the participants' write
	 * translations for it and waits for the participants inside the engine
	 * to leave it once; a call settles the gain only by a wait that
	 * completed. A caller that does not get a settled page translates it
	 * once and caches nothing. */
	[[nodiscard]] Settlement add(
		std::uint64_t page, const Participant *self) noexcept;
	void remove(std::uint64_t page) noexcept;
	/* The debt of a frame registration (frame_accounting.hpp): retires every
	 * participant's write translation for `page`, an address of this
	 * generation that another translator of its frame may write through, and
	 * waits for the participants inside the engine to leave once. The same
	 * wait and escalation as `add`, for a page this registry does not count. */
	[[nodiscard]] Settlement retire_and_wait(
		std::uint64_t page, const Participant *self) noexcept;

	/* The debt of a frame registration whose writers cannot be named one by
	 * one (design 2.5): retires every write translation of every participant
	 * and waits for them to leave once. */
	[[nodiscard]] Settlement retire_all_and_wait(
		const Participant *self) noexcept;
	/* A retirer is called with this page to retire everything it holds. */
	static constexpr std::uint64_t all_pages = ~std::uint64_t{0};
	/* Every registry some executor holds. Throws std::bad_alloc. */
	[[nodiscard]] static std::vector<std::shared_ptr<CodePageRegistry>>
	registries();

	/* Whether the participant a registration waited out as `serial` at
	 * `epoch` has since left the engine or the registry. */
	[[nodiscard]] bool left_or_moved(
		std::uint64_t serial, std::uint64_t epoch) const noexcept;
	/* The stuck participants a registration returns at once for, for
	 * diagnostics. */
	[[nodiscard]] std::size_t expiry_records() const noexcept;
	/* Departures that found the participant inside the engine; must stay 0. */
	[[nodiscard]] std::uint64_t leaves_inside_engine() const noexcept
	{
		return leaves_inside_engine_.load(std::memory_order_relaxed);
	}

private:
	static constexpr unsigned bit_count_log2 = 18;
	static constexpr std::size_t bit_count = std::size_t{1} << bit_count_log2;

	[[nodiscard]] static std::size_t bit_index(std::uint64_t address) noexcept
	{
		const auto page = address >> 12U;
		return static_cast<std::size_t>(
			(page ^ (page >> bit_count_log2)) & (bit_count - 1U));
	}
	[[nodiscard]] bool holds_exactly(
		std::uint64_t address, std::size_t bit) const noexcept;
	[[nodiscard]] Settlement wait_for_participants(
		const Participant *self) noexcept;
	void retire_write_translations(std::uint64_t page) noexcept;
	void forget_departed_expiries() noexcept;

	/* A participant is named by where it lives and by the join that made it
	 * a member. A waiter may only touch the participant while it finds that
	 * pair among the members, under the lock; a participant constructed at
	 * a departed one's address joins with a serial no earlier member had,
	 * so it is never mistaken for it. */
	struct Member final {
		Participant *participant{};
		std::uint64_t serial{};
	};

	/* Neighbouring pages hash to neighbouring bits, so they land in
	 * different stripes. A stripe guards the page counts, the bit counts and
	 * the membership bits of its bits; it is taken before
	 * participants_mutex_. A page that lost its last holder keeps its entry
	 * at zero, so a cache that keeps replacing one translation does not keep
	 * allocating one. */
	static constexpr std::size_t stripe_count = 64;
	/* The holders of a page, and whether the gain that made it held is
	 * settled. Each gain takes the next number of its stripe; `settled` is
	 * the highest gain a completed wait covered, so a wait that finishes
	 * after the page was lost and gained again settles nothing. */
	struct Page final {
		std::uint32_t count{};
		std::uint64_t gain{};
		std::uint64_t settled{};
	};
	struct alignas(64) Stripe final {
		mutable ::uml::memory::WordSharedMutex mutex;
		std::unordered_map<std::uint64_t, Page> pages;
		std::uint64_t gains{};
	};
	[[nodiscard]] Stripe &stripe_for(std::size_t bit) noexcept
	{
		return stripes_[bit & (stripe_count - 1U)];
	}
	[[nodiscard]] const Stripe &stripe_for(std::size_t bit) const noexcept
	{
		return stripes_[bit & (stripe_count - 1U)];
	}

	std::array<Stripe, stripe_count> stripes_;
	mutable ::uml::memory::WordSharedMutex participants_mutex_;
	std::vector<Member> participants_;
	/* The serial of the next join; 64 bits, so never reused. Guarded by
	 * participants_mutex_ like the list. */
	std::uint64_t next_serial_{1};
	std::atomic_uint64_t leaves_inside_engine_{0};
	/* The participants a registration waited out, at the epoch each did not
	 * leave. A later registration does not wait for such a participant
	 * again while its epoch is unchanged. Guarded by participants_mutex_;
	 * dropped when the participant leaves the engine or the registry. */
	std::vector<StuckMember> expiries_;
	const WaitPolicy policy_;
	std::unique_ptr<std::atomic<std::uint64_t>[]> bits_;
	std::unique_ptr<std::uint8_t[]> counts_;
	std::atomic<std::uint64_t> changes_{0};
	/* A page count could not be allocated, so the bits answer alone again. */
	std::atomic<bool> inexact_{false};
	/* The generation whose shared table entry this registry owns; empty for
	 * a registry constructed directly. */
	std::optional<std::uint64_t> generation_;
	/* The participants that retire their own entries, guarded by
	 * participants_mutex_ like the list above. */
	struct Retirer final {
		Participant *participant{};
		/* The serial of the join that made the participant a member. */
		std::uint64_t serial{};
		RetireWriteTranslation retire{};
		void *context{};
	};
	std::vector<Retirer> retirers_;
};

/* What registering the frame of a page returned. */
enum class FrameRegistration : std::uint8_t {
	/* The holder stands and nothing is owed. */
	settled,
	/* The holder stands, but a participant that could still land a store
	 * through a write translation retired for it is stuck inside the engine
	 * (or the memory cannot say the debt was cleared). */
	unsettled,
	/* Nothing was registered. */
	failed,
	/* The page is not translatable at this location, or the generation is
	 * gone. */
	refused,
};

/* The outcome of `register_frame`, with the waits that did not settle, so a
 * translator that keeps code compiled before a stuck participant moves can
 * name that participant. */
struct FrameRegistrationOutcome final {
	FrameRegistration status{FrameRegistration::failed};
	struct Wait final {
		std::shared_ptr<CodePageRegistry> registry;
		CodePageRegistry::Settlement settlement;
	};
	std::vector<Wait> stuck;
	/* An unsettled wait could not be recorded in `stuck`. */
	bool stuck_lost{};
	/* For a settled registration the address space did not answer globally:
	 * the frame's record and its holder change count as the registration left
	 * them (RegistrationMemo). */
	const void *record{};
	std::uint64_t alias_changes{};
};

/* Registers the frame the page at `page` maps as held by the generation the
 * execution context belongs to, and retires and waits out the write
 * translations the registration owes (frame_accounting.hpp), all before the
 * caller reads a byte of the page. `self` is the participant that registers,
 * which the waits do not wait for. */
[[nodiscard]] FrameRegistrationOutcome register_frame(
	::uml::memory::GuestAddressSpace &memory, void *execution_context,
	const CodePageRegistry::Participant *self, std::uint64_t page) noexcept;

/* What the process's registrations came to, summed over every engine and
 * address space, for a run's report. Always compiled in; they move on cold
 * paths only. */
struct RegistrationTotals final {
	/* Calls of `register_frame`, and how they ended when they did not
	 * settle. */
	std::uint64_t registrations;
	std::uint64_t failures;
	std::uint64_t refusals;
	std::uint64_t unsettled;
	/* Stamp hits the gadget engine refused because the generation's flush
	 * serial had moved. */
	std::uint64_t flush_serial_misses;
};
[[nodiscard]] RegistrationTotals registration_totals() noexcept;
void note_flush_serial_miss() noexcept;

} // namespace uml::cpu::x86_64
