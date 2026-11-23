// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace uml::cpu::gadget_cache {

/* A process-wide byte budget for what the gadget caches keep per address
 * space.
 *
 * Slots bound how many translations a cache holds, not how large they are: at
 * 262,144 slots, every slot holding a block of the largest arena class would be
 * 4 GiB per vCPU, and a shared space's link records grow with every target it
 * ever linked to. So the storage each address space keeps -- its translations
 * and whatever holds them, its link and indirect records, its target tables --
 * is charged to an Account, one per address space as the engine keeps it (an
 * arm64 space, shared by the executors running it or private to one; an x86
 * executor's own record of an address space), and the accounts add up to one
 * figure for the process. A translation several executors share is charged
 * once, to the space that holds it. The slots are not charged: the slot count
 * fixes their bytes.
 *
 * The budget caps the accounts that are open. A store that finds them above it
 * calls relieve(), which asks open accounts to shed until they hold at most
 * seven eighths of the budget (the target), or fifteen sixteenths of it (the
 * line) while a running account between the two is spared. An executor that
 * enters translated code in an address space, or switches to it, tells its
 * account (Account::ran()), which keeps one bit for each of the last 64 epochs
 * (about 2.7 ms each, 175 ms in all) in which that happened: its activity.
 * Relief asks first any account that holds more than the line by itself, which
 * leaves the others less than a sixteenth of the budget to run in and which no
 * other account's shedding could spare, and any idle one that holds more than
 * the target. Then it asks the accounts the guest is least likely to run again
 * at once, only while the open ones are above the target: those no executor
 * entered in the window, the one idle longest first, then the others by
 * activity band, a quarter of the window each, the least active band first and
 * the smallest account first within a band, so the address space the guest is
 * running most is asked last. Activity, not the epoch of the last entry, orders
 * the running accounts: a stamp trails a run by more than the gap between
 * running accounts. A running account between the target and the line, a
 * program that alone holds most of the cache, is spared while there are others
 * to ask, and relief then stops at the line instead of the target. With no
 * change made meanwhile it leaves the open accounts at least a sixteenth of the
 * budget of headroom, which the growth of any open account, the spared one
 * included, uses up before the next relief. A spared account is asked only when
 * the open accounts are above the budget with nothing else left. A program that
 * grows past the line is asked whole and stalls while its threads translate its
 * working set again; shedding only the cold part of a running address space,
 * which the cache cannot do, would spare that.
 *
 * An account asked to shed tells its engine (Notify), and every executor
 * holding that address space lets go of it at its next entry into translated
 * code, as it lets go of a renewed space; the storage goes with the last of
 * them, and the address space starts afresh in a new account when it runs
 * again. The arm64 engine ends a running holder's run at its next return from
 * translated code, through the fairness exit, once its executor is asked, and
 * lets go of a space that nobody will run again (shed, closed, private, or of
 * a destroyed guest address space) by emptying its slots without telling the
 * space of each body, since the space dies with its last holder. The x86-64
 * engine lets go at the start of its next run, body by body. Until the storage
 * goes, a shedding account's bytes, whatever it charges or refunds after it
 * was asked, are counted apart from the open ones: every charge is attributed
 * to one figure or the other exactly once. Every store past the budget
 * relieves it in turn, counting the open accounts again under the ledger's
 * lock, so the open accounts stay within the budget but for the stores in
 * flight, one per storing vCPU.
 *
 * The budget does not bound the shed bytes: they wait for the holders, which
 * the ledger cannot hurry. A shed space stays until its last holder has let go
 * of it and unmapped it, so while every vCPU translates, each relief leaves
 * such a space in flight and the whole is above the budget by the spaces in
 * flight. How long a space is in flight is the time its slowest holder takes
 * to reach its next entry and let go. An idle vCPU lets go when it next runs,
 * and holds meanwhile what its window held when it went idle, which was part
 * of what was open then. The guest memory budget counts the most the whole
 * has exceeded the budget by in an Android guest's bursts
 * (runtime/guest_memory_budget.hpp, x(v)); a guest that keeps every vCPU
 * translating its own large address space exceeds the budget by more.
 *
 * Telling the account costs an entry a read of the host's counter and a load,
 * and a store only when the epoch has moved on, once per epoch however many
 * executors run the address space; no lock, no read-modify-write, nothing per
 * block. The record is approximate by design: executors entering in the same
 * epoch may overwrite one another's bit.
 *
 * The emulator sets the budget before any executor exists
 * (derive_budget()); a process that never does keeps fallback_budget.
 */

class Account;
struct LedgerState;
inline void relieve() noexcept;
inline void relieve_at(std::uint64_t now) noexcept;
[[nodiscard]] inline LedgerState ledger_state();
[[nodiscard]] inline bool largest_open_bytes(std::size_t &bytes) noexcept;

namespace detail {

/* Accounts are linked through themselves, so opening one never allocates. */
struct Ledger final {
	std::mutex mutex;
	Account *first{};
	std::atomic<std::size_t> budget;
	/* The bytes of the open accounts, and of those asked to shed. Each
	 * charge lands in exactly one of them (Account::charge()), so they are
	 * exact but for the charges in flight. */
	std::atomic<std::size_t> open{};
	std::atomic<std::size_t> shedding{};
	std::atomic<std::uint64_t> reliefs{};
	std::atomic<std::uint64_t> shed_accounts{};
	/* Shared address spaces closed for renewal, not asked to shed. */
	std::atomic<std::uint64_t> renewals{};
	std::atomic<std::size_t> peak_open{};
};

} // namespace detail

/* What a process that never chose a budget keeps: a unit test, or an embedding
 * that runs executors without the emulator. */
inline constexpr std::size_t fallback_budget = std::size_t{1} << 30;
/* derive_budget() never goes below this for a guest's memory: a full cache of
 * one vCPU at the default slot count, of the body sizes Geekbench measures,
 * with its link records, is about 100 MiB. */
inline constexpr std::size_t minimum_budget = std::size_t{128} << 20;
/* The share of a host's process memory limit a budget may take, as a divisor
 * of the limit. */
inline constexpr std::size_t limit_divisor = 8;
/* The share of guest memory a budget takes, as a divisor. */
inline constexpr std::size_t guest_memory_divisor = 4;

/* The share of the budget relief leaves the others to run in when one program
 * holds most of the cache, as a divisor: an account larger than the budget less
 * this share is asked to shed whole. At an eighth it is the target's, and no
 * account is spared. */
inline constexpr std::size_t target_divisor = 8;
inline constexpr std::size_t reserve_divisor = 16;

/* The clock accounts are told their runs by: the host's counter (CNTVCT_EL0,
 * 24 MHz on Apple silicon) with its low bits dropped, so an epoch is about
 * 2.7 ms. Reading it is a register read, not a call. */
inline constexpr unsigned epoch_shift = 16;

[[nodiscard]] inline std::uint64_t epoch() noexcept
{
#if defined(__aarch64__)
	std::uint64_t ticks;
	asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
	return ticks >> epoch_shift;
#else
	return static_cast<std::uint64_t>(
		std::chrono::steady_clock::now().time_since_epoch().count()) >>
		(epoch_shift + 5);
#endif
}

/* The epochs an account remembers entries for: the width of its history. */
inline constexpr unsigned window_epochs = 64;
/* An activity band is this many epochs of the window: four of them, and a few
 * epochs missed seldom move an account to another. */
inline constexpr unsigned band_epochs = 16;

/* Never destroyed, under the exit contract in docs/architecture.md: carriers may
 * still store, and lock the ledger to relieve it, while exit() runs. */
[[nodiscard]] inline detail::Ledger &ledger() noexcept
{
	[[clang::no_destroy]] static detail::Ledger instance{
		.budget = fallback_budget};
	return instance;
}

/* The budget for a guest of `guest_memory` bytes on a host that limits the
 * process to `memory_limit` bytes, zero for none: a quarter of guest memory,
 * at least minimum_budget, and at most an eighth of the limit.
 *
 * At a quarter of eight GiB, the memory the benchmark guests run with, the
 * largest figure measured (multi-core Geekbench on four x86 vCPUs, whose
 * executors keep their translations to themselves) is a seventh of the budget;
 * on the iPad mini 6 the limit (2.8 GiB) gives 358 MiB where a 2 GiB guest
 * would give 512. */
[[nodiscard]] constexpr std::size_t derive_budget(std::size_t guest_memory,
	std::size_t memory_limit) noexcept
{
	auto budget = std::max(minimum_budget,
		guest_memory / guest_memory_divisor);
	if (memory_limit != 0)
		budget = std::min(budget, std::max<std::size_t>(
			memory_limit / limit_divisor, std::size_t{16} << 20));
	return budget;
}

/* Set the process's budget. A run sets it once, before its first executor;
 * tests set it as they need. */
inline void set_budget(std::size_t bytes) noexcept
{
	ledger().budget.store(std::max<std::size_t>(bytes, 1),
		std::memory_order_relaxed);
}

[[nodiscard]] inline std::size_t budget() noexcept
{
	return ledger().budget.load(std::memory_order_relaxed);
}

/* Whether the open accounts hold more than the budget: what every store asks,
 * two loads. */
[[nodiscard]] inline bool over_budget() noexcept
{
	auto &book = ledger();
	return book.open.load(std::memory_order_relaxed) >
		book.budget.load(std::memory_order_relaxed);
}

/* One address space's storage, charged to the process. The engine charges it
 * as that storage grows and shrinks and destroys it with the address space,
 * which refunds what is left. `notify` is called with `owner`, under the
 * ledger's lock, when the account must shed: it may only set what the
 * executors holding the address space read at their next entry, and take no
 * lock but ones the ledger's is always taken before. The owner keeps
 * everything `notify` touches alive until the account is destroyed, which
 * waits for a call in progress. */
class Account final {
public:
	using Notify = void (*)(void *owner) noexcept;

	/* A new account counts as running from `opened` on: it opens because an
	 * executor is about to run the address space. */
	Account(Notify notify, void *owner,
		std::uint64_t opened = epoch()) noexcept
		: notify_{notify}, owner_{owner}, last_run_{opened}
	{
		auto &book = ledger();
		const std::lock_guard guard{book.mutex};
		next_ = book.first;
		if (next_ != nullptr)
			next_->previous_ = this;
		book.first = this;
	}

	~Account()
	{
		auto &book = ledger();
		const std::lock_guard guard{book.mutex};
		if (previous_ != nullptr)
			previous_->next_ = next_;
		else
			book.first = next_;
		if (next_ != nullptr)
			next_->previous_ = previous_;
		const auto state = state_.load(std::memory_order_relaxed);
		figure(state).fetch_sub(state & bytes_mask,
			std::memory_order_relaxed);
	}

	Account(const Account &) = delete;
	Account &operator=(const Account &) = delete;

	/* The address space's storage grew by `delta` bytes, or shrank. The
	 * change counts in the open figure or the shedding one as the account
	 * stood when it was made, which one atomic step decides: a change made
	 * before the account was asked to shed moves with it when it is asked,
	 * and one made after counts as shedding. An account never holds less
	 * than nothing, so a refund never reaches the flag. */
	void charge(std::ptrdiff_t delta) noexcept
	{
		if (delta == 0)
			return;
		const auto bytes = static_cast<std::uint64_t>(delta);
		const auto before = state_.fetch_add(bytes,
			std::memory_order_relaxed);
		figure(before).fetch_add(static_cast<std::size_t>(bytes),
			std::memory_order_relaxed);
	}

	[[nodiscard]] std::size_t bytes() const noexcept
	{
		return static_cast<std::size_t>(
			state_.load(std::memory_order_relaxed) & bytes_mask);
	}

	/* An executor entered translated code in the address space, or switched
	 * to it, in epoch `at` (epoch()). A load and a compare, and two stores
	 * only when the epoch has moved on: no lock and no read-modify-write,
	 * and the executors running one address space mostly read the line. */
	void ran(std::uint64_t at) noexcept
	{
		const auto last = last_run_.load(std::memory_order_relaxed);
		if (at <= last)
			return;
		const auto gap = at - last;
		const auto before = gap < window_epochs ?
			recent_.load(std::memory_order_relaxed) << gap : 0;
		recent_.store(before | 1, std::memory_order_relaxed);
		last_run_.store(at, std::memory_order_relaxed);
	}

	/* The epoch of the account's last entry, or of its opening. */
	[[nodiscard]] std::uint64_t last_run() const noexcept
	{
		return last_run_.load(std::memory_order_relaxed);
	}

	/* The epochs of the window ending in epoch `now` in which an executor
	 * entered the address space: window_epochs for one that runs all the
	 * time (and for a new one, until its history is its own), none for
	 * one that no executor entered in the window. */
	[[nodiscard]] unsigned activity(std::uint64_t now) const noexcept
	{
		const auto last = last_run_.load(std::memory_order_relaxed);
		const auto gap = now > last ? now - last : 0;
		if (gap >= window_epochs)
			return 0;
		return static_cast<unsigned>(std::popcount(
			recent_.load(std::memory_order_relaxed) << gap));
	}

	/* Whether the address space was asked to shed: its executors let go
	 * of it at their next entry, and none takes it up again. */
	[[nodiscard]] bool shedding() const noexcept
	{
		return (state_.load(std::memory_order_relaxed) &
			shedding_flag) != 0;
	}

private:
	friend void relieve_at(std::uint64_t now) noexcept;
	friend LedgerState ledger_state();
	friend bool largest_open_bytes(std::size_t &bytes) noexcept;

	static constexpr std::uint64_t shedding_flag = std::uint64_t{1} << 63;
	static constexpr std::uint64_t bytes_mask = shedding_flag - 1;

	/* The ledger's figure a change to an account in `state` counts in. */
	[[nodiscard]] static std::atomic<std::size_t> &figure(
		std::uint64_t state) noexcept
	{
		auto &book = ledger();
		return (state & shedding_flag) != 0 ? book.shedding : book.open;
	}

	/* Ask the account to shed, under the ledger's lock, and move what it
	 * held when asked from the open figure to the shedding one; return
	 * that. A charge made after counts as shedding by itself. */
	std::size_t start_shedding() noexcept
	{
		const auto before = state_.fetch_or(shedding_flag,
			std::memory_order_relaxed);
		const auto bytes = static_cast<std::size_t>(before & bytes_mask);
		auto &book = ledger();
		book.open.fetch_sub(bytes, std::memory_order_relaxed);
		book.shedding.fetch_add(bytes, std::memory_order_relaxed);
		return bytes;
	}

	Notify notify_;
	void *owner_;
	/* What the account holds, with shedding_flag once it was asked to. */
	std::atomic<std::uint64_t> state_{};
	/* The epoch of the last entry, and one bit for each epoch of the window
	 * ending there, the newest in bit 0, set where an executor entered.
	 * A new account starts with every bit set. */
	std::atomic<std::uint64_t> last_run_;
	std::atomic<std::uint64_t> recent_{~std::uint64_t{}};
	Account *previous_{};
	Account *next_{};
};

namespace detail {

/* An open account as relief weighs it. */
struct Candidate final {
	Account *account{};
	/* 0 for an idle account, up to window_epochs / band_epochs. */
	unsigned band{};
	std::uint64_t last_run{};
	std::size_t bytes{};
};

/* Whether `a` is asked to shed before `b`: the lower activity band first. In
 * band 0 the account idle longest, and of those that last ran in the same
 * epoch the larger, which frees more for the one translation it will cost;
 * in the others the smaller, which stalls its program for less, and of equal
 * size the one that ran earlier. Equal in every respect, neither is first. */
[[nodiscard]] constexpr bool shed_before(const Candidate &a,
	const Candidate &b) noexcept
{
	if (a.band != b.band)
		return a.band < b.band;
	if (a.band == 0) {
		if (a.last_run != b.last_run)
			return a.last_run < b.last_run;
		return a.bytes > b.bytes;
	}
	if (a.bytes != b.bytes)
		return a.bytes < b.bytes;
	return a.last_run < b.last_run;
}

} // namespace detail

/* Ask open accounts to shed until the open ones hold at most seven eighths of
 * the budget (the target), or fifteen sixteenths of it (the line) while a
 * running account between the two is spared, when they hold more than all of
 * it, as epoch `now` sees them.
 *
 * What cannot be avoided comes first: an account holding more than the line
 * (fifteen sixteenths of the budget, reserve_divisor) by itself leaves the
 * others less than the reserve however many of them shed, so it is asked, the
 * largest first, before any account that might have been enough instead. Then
 * the others are asked in the order of detail::shed_before(), which puts the
 * accounts the guest is running last, only while the open ones are above the
 * target. An account the guest runs between the target and the line is spared
 * while any other is left to ask: the others go until the open ones hold at
 * most the line, which is as far as they can bring the open ones, and a spared
 * account is asked only when they are still above the budget with nothing else
 * left. One that is idle is asked as soon as it holds more than the target. A
 * relief that meets no concurrent charge so leaves the open accounts at most at
 * the line, at least a reserve under the budget, and the next comes only when
 * the growth of the open accounts, every one of them (the spared one too), has
 * used the reserve up; one store may use it all, and a charge made meanwhile
 * moves the figures. An account holding nothing is never asked, and a budget
 * smaller than one address space still holds. Of accounts equal in every
 * respect, the one opened last is asked first.
 *
 * Every caller takes its turn, however the lock is held: one that went on
 * storing while another held it, for an account opening or going, a census or
 * a relief, could take the open accounts past the budget by as much as it
 * stored meanwhile. A caller that waited counts again and finds the budget
 * kept, or relieves it once more. No one holding the ledger's lock waits for
 * anything but the locks taken after it, so the wait is short, and it comes
 * only past the budget. */
inline void relieve_at(std::uint64_t now) noexcept
{
	auto &book = ledger();
	const std::lock_guard guard{book.mutex};
	/* The open accounts, counted afresh: what they hold, not the figure
	 * a charge in flight may not have reached yet. */
	std::size_t open = 0;
	for (auto *account = book.first; account != nullptr;
	     account = account->next_)
		if (!account->shedding())
			open += account->bytes();
	const auto budget = book.budget.load(std::memory_order_relaxed);
	if (open > book.peak_open.load(std::memory_order_relaxed))
		book.peak_open.store(open, std::memory_order_relaxed);
	if (open <= budget)
		return;
	const auto target = budget - budget / target_divisor;
	const auto line = budget - budget / reserve_divisor;
	bool relieved = false;
	for (;;) {
		/* Each pass scans the ledger again: reliefs are rare, and nothing
		 * is allocated under the lock. The largest account over the line
		 * by itself, or over the target and idle, if any; the first of the
		 * others to ask, and of the running ones between the target and
		 * the line, which are spared. */
		detail::Candidate unavoidable;
		detail::Candidate chosen;
		detail::Candidate spared;
		for (auto *account = book.first; account != nullptr;
		     account = account->next_) {
			const auto bytes = account->bytes();
			if (account->shedding() || bytes == 0)
				continue;
			const detail::Candidate candidate{account,
				(account->activity(now) + band_epochs - 1) /
					band_epochs,
				account->last_run(), bytes};
			if (bytes > line || (bytes > target && candidate.band == 0)) {
				if (bytes > unavoidable.bytes)
					unavoidable = candidate;
				continue;
			}
			auto &pool = bytes > target ? spared : chosen;
			if (pool.account == nullptr ||
			    detail::shed_before(candidate, pool))
				pool = candidate;
		}
		if (open <= (spared.account != nullptr ? line : target))
			break;
		if (unavoidable.account != nullptr)
			chosen = unavoidable;
		else if (chosen.account == nullptr) {
			if (spared.account == nullptr || open <= budget)
				break;
			chosen = spared;
		}
		const auto shed = chosen.account->start_shedding();
		open = open > shed ? open - shed : 0;
		book.shed_accounts.fetch_add(1, std::memory_order_relaxed);
		relieved = true;
		chosen.account->notify_(chosen.account->owner_);
	}
	if (relieved)
		book.reliefs.fetch_add(1, std::memory_order_relaxed);
}

/* relieve_at() by the clock. */
inline void relieve() noexcept
{
	relieve_at(epoch());
}

/* What the ledger holds, for diagnostics and tests. */
struct LedgerState final {
	std::size_t budget;
	/* Every account's bytes, and those of the accounts asked to shed. */
	std::size_t total;
	std::size_t shedding;
	/* The accounts, and the most the open ones held when relieve()
	 * counted them, which it does only past the budget. */
	std::size_t accounts;
	std::size_t peak_open;
	/* relieve() calls that asked an account to shed, and the accounts. */
	std::uint64_t reliefs;
	std::uint64_t shed_accounts;
};

/* Counted under the ledger's lock, so the figures agree with one another;
 * for a census or a test, not for a store. */
[[nodiscard]] inline LedgerState ledger_state()
{
	auto &book = ledger();
	const std::lock_guard guard{book.mutex};
	LedgerState state{
		.budget = book.budget.load(std::memory_order_relaxed),
		.total = 0,
		.shedding = 0,
		.accounts = 0,
		.peak_open = book.peak_open.load(std::memory_order_relaxed),
		.reliefs = book.reliefs.load(std::memory_order_relaxed),
		.shed_accounts = book.shed_accounts.load(
			std::memory_order_relaxed),
	};
	for (auto *account = book.first; account != nullptr;
	     account = account->next_) {
		const auto bytes = account->bytes();
		state.total += bytes;
		if (account->shedding())
			state.shedding += bytes;
		++state.accounts;
	}
	return state;
}

/* The bytes of the largest open account, for the session record's checkpoint.
 * It never waits: false, with `bytes` unchanged, when the ledger's lock is
 * taken. */
[[nodiscard]] inline bool largest_open_bytes(std::size_t &bytes) noexcept
{
	auto &book = ledger();
	const std::unique_lock guard{book.mutex, std::try_to_lock};
	if (!guard.owns_lock())
		return false;
	std::size_t largest = 0;
	for (auto *account = book.first; account != nullptr;
	     account = account->next_)
		if (!account->shedding())
			largest = std::max(largest, account->bytes());
	bytes = largest;
	return true;
}

/* Charge the change in a holder's bytes across a scope to `account`, null for
 * none, whether the scope finishes or throws. */
template <typename Measure>
class ChargeScope final {
public:
	ChargeScope(Account *account, Measure measure) noexcept
		: account_{account}, measure_{measure},
		  before_{account != nullptr ? measure() : 0}
	{}
	~ChargeScope()
	{
		if (account_ != nullptr)
			account_->charge(static_cast<std::ptrdiff_t>(measure_()) -
				static_cast<std::ptrdiff_t>(before_));
	}
	ChargeScope(const ChargeScope &) = delete;
	ChargeScope &operator=(const ChargeScope &) = delete;

private:
	Account *account_;
	Measure measure_;
	std::size_t before_;
};

} // namespace uml::cpu::gadget_cache
