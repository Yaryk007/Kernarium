// SPDX-License-Identifier: GPL-2.0-or-later
/* The process's byte budget for the gadget caches' address spaces, on its own:
 * what it derives from a guest's memory and a host's limit, how the accounts
 * add up, how an account remembers the epochs an executor entered it in, and
 * which of them relieve() asks to shed. The engines' own use of it is tested
 * with each engine. */
#include "cpu/gadget_cache_budget.hpp"
#include "require.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace cache = uml::cpu::gadget_cache;
using uml::testing::require;

namespace {

constexpr std::size_t kib = std::size_t{1} << 10;
constexpr std::size_t mib = std::size_t{1} << 20;
constexpr std::size_t gib = std::size_t{1} << 30;

/* A quarter of guest memory, at least 128 MiB, and at most an eighth of a
 * host's limit when it sets one. */
void derived_budgets()
{
	require(cache::derive_budget(8 * gib, 0) == 2 * gib);
	require(cache::derive_budget(256 * mib, 0) == 128 * mib);
	require(cache::derive_budget(0, 0) == 128 * mib);
	/* The iPad mini 6 lets an application reach 2,867 MiB. */
	require(cache::derive_budget(2 * gib, 2867 * mib) == 2867 * mib / 8);
	require(cache::derive_budget(4 * gib, 2867 * mib) == 2867 * mib / 8);
	require(cache::derive_budget(8 * gib, 64 * gib) == 2 * gib);
	/* A host limit below the minimum still leaves a budget. */
	require(cache::derive_budget(gib, 64 * mib) == 16 * mib);
}

/* What the measured workloads charged at their peak in a census at 4 KiB
 * guest pages (G282), at the geometry each ran with, against the budget
 * derived for it. None comes within half of it. */
void measured_workloads_stay_well_within_their_budget()
{
	struct Workload final {
		std::size_t guest_memory;
		std::size_t host_limit;
		std::size_t peak;
	};
	constexpr std::array workloads{
		/* Four arm64 vCPUs, 8 GiB: Geekbench single- and
		 * multi-core, and WebGL Aquarium. */
		Workload{8 * gib, 0, 68 * mib},
		Workload{8 * gib, 0, 74 * mib},
		Workload{8 * gib, 0, 223 * mib},
		/* Four x86 vCPUs, 8 GiB, whose executors keep their
		 * translations to themselves: multi-core Geekbench. */
		Workload{8 * gib, 0, 296 * mib},
		/* The iPad's geometry, two vCPUs and 2 GiB under its
		 * limit: multi-core Geekbench and Aquarium. */
		Workload{2 * gib, 2867 * mib, 71 * mib},
		Workload{2 * gib, 2867 * mib, 169 * mib},
	};
	for (const auto &workload : workloads)
		require(workload.peak * 2 <= cache::derive_budget(
			workload.guest_memory, workload.host_limit));
}

struct Owner final {
	std::atomic<unsigned> notified{};
};

void notify(void *owner) noexcept
{
	static_cast<Owner *>(owner)->notified.fetch_add(1,
		std::memory_order_relaxed);
}

/* The epoch the accounts of these tests open in, and a window's width. */
constexpr std::uint64_t t0 = 100000;
constexpr std::uint64_t window = cache::window_epochs;

struct Holder final {
	explicit Holder(std::uint64_t opened = t0) : account{notify, &owner, opened}
	{}

	/* An executor entered the address space in every epoch of [first, last]. */
	void ran(std::uint64_t first, std::uint64_t last)
	{
		for (auto epoch = first; epoch <= last; ++epoch)
			account.ran(epoch);
	}

	[[nodiscard]] bool asked() const
	{
		return owner.notified.load() != 0;
	}

	Owner owner;
	cache::Account account;
};

/* Whether the open and shedding figures every store reads are what the
 * accounts hold, counted one by one. */
[[nodiscard]] bool figures_agree()
{
	const auto state = cache::ledger_state();
	const auto &book = cache::ledger();
	return book.open.load() == state.total - state.shedding &&
		book.shedding.load() == state.shedding;
}

/* An account remembers one bit for each epoch of a window in which an
 * executor entered its address space: a new one every epoch of it, one that
 * runs all the time all of them, one that stopped what it has left of the
 * window, and one that has not been entered for a window none. Entering twice
 * in an epoch counts once, and an entry in an epoch already passed counts for
 * nothing. */
void an_account_remembers_the_epochs_it_ran_in()
{
	Holder fresh{t0};
	require(fresh.account.activity(t0) == window);
	require(fresh.account.activity(t0 + 10) == window - 10);
	require(fresh.account.activity(t0 + window - 1) == 1);
	require(fresh.account.activity(t0 + window) == 0);
	require(fresh.account.last_run() == t0);

	Holder busy{t0};
	busy.ran(t0 + 1, t0 + 3 * window);
	require(busy.account.last_run() == t0 + 3 * window);
	require(busy.account.activity(t0 + 3 * window) == window);
	require(busy.account.activity(t0 + 3 * window + 40) == window - 40);

	/* Every fourth epoch, with a repeat and a stale entry: 16 bits. */
	Holder sparse{t0};
	for (std::uint64_t epoch = t0 + 4; epoch <= t0 + 2 * window; epoch += 4) {
		sparse.account.ran(epoch);
		sparse.account.ran(epoch);
		sparse.account.ran(epoch - 1);
	}
	require(sparse.account.last_run() == t0 + 2 * window);
	require(sparse.account.activity(t0 + 2 * window) == window / 4);

	/* An entry after a gap of a window or more forgets the history. */
	Holder gap{t0};
	gap.account.ran(t0 + 5 * window);
	require(gap.account.activity(t0 + 5 * window) == 1);
	require(gap.account.activity(t0 + 5 * window + 1) == 1);
	/* A clock reading older than the last entry sees it as now. */
	require(gap.account.activity(t0) == 1);
}

/* Accounts add up, and the open ones are asked to shed until they are down to
 * seven eighths of the budget. A shedding account's bytes count apart however
 * it changes, and an account that goes refunds what it held. The figures every
 * store reads stay exact throughout. Accounts that all run all the time go
 * smallest first. */
void relief_accounting()
{
	cache::set_budget(64 * kib);
	const auto base = cache::ledger_state();
	require(base.total == 0 && base.accounts == 0);
	auto small = std::make_unique<Holder>();
	auto large = std::make_unique<Holder>();
	auto middle = std::make_unique<Holder>();
	small->account.charge(10 * kib);
	large->account.charge(50 * kib);
	middle->account.charge(30 * kib);
	auto state = cache::ledger_state();
	require(state.total == 90 * kib && state.accounts == 3 &&
		state.shedding == 0);
	require(cache::over_budget());

	/* 90 against 64 KiB: down to 56, which the small one and the middle one
	 * do, in that order, leaving the large one. */
	cache::relieve_at(t0);
	state = cache::ledger_state();
	require(state.reliefs == base.reliefs + 1 &&
		state.shed_accounts == base.shed_accounts + 2);
	require(small->account.shedding() && middle->account.shedding() &&
		!large->account.shedding());
	require(small->owner.notified == 1 && middle->owner.notified == 1 &&
		large->owner.notified == 0);
	require(state.shedding == 40 * kib && !cache::over_budget());
	require(state.peak_open >= 90 * kib);

	/* A shedding address space may go on growing until its executors let
	 * go of it; it counts apart, and the open ones are what the budget
	 * bounds. */
	small->account.charge(40 * kib);
	require(!cache::over_budget());
	require(cache::ledger_state().shedding == 80 * kib);
	require(figures_agree());
	cache::relieve_at(t0);
	require(cache::ledger_state().reliefs == base.reliefs + 1);
	require(small->owner.notified == 1 && !cache::over_budget());

	/* The open one passes it alone: it is asked, whatever it is. */
	large->account.charge(20 * kib);
	require(cache::over_budget());
	cache::relieve_at(t0);
	require(large->account.shedding() && large->owner.notified == 1);
	state = cache::ledger_state();
	require(state.total == 150 * kib && state.shedding == 150 * kib);
	require(!cache::over_budget() && figures_agree());

	/* Address spaces that go refund what they held. */
	small.reset();
	middle.reset();
	state = cache::ledger_state();
	require(state.total == 70 * kib && state.shedding == 70 * kib &&
		state.accounts == 1);
	large.reset();
	require(cache::ledger_state().total == 0 && figures_agree());
	cache::set_budget(cache::fallback_budget);
}

/* A shed address space may give back part of what it held before its last
 * executor lets go of it, as each frees what only it held. That leaves the
 * open accounts as they were, so a store finds the budget passed as soon as
 * they pass it, not once they have also grown by what the shed one gave back.
 */
void a_shed_account_that_shrinks_leaves_the_open_ones_exact()
{
	cache::set_budget(64 * kib);
	const auto base = cache::ledger_state();
	auto old_space = std::make_unique<Holder>();
	auto new_space = std::make_unique<Holder>();
	old_space->account.charge(60 * kib);
	new_space->account.charge(10 * kib);
	/* The old one has not run for a window, the new one has. */
	new_space->ran(t0 + 1, t0 + 2 * window);
	require(cache::over_budget());
	cache::relieve_at(t0 + 2 * window);
	require(old_space->account.shedding() &&
		!new_space->account.shedding());
	old_space->account.charge(-50 * static_cast<std::ptrdiff_t>(kib));
	new_space->account.charge(50 * kib);
	require(!cache::over_budget() && figures_agree());
	new_space->account.charge(5 * kib);
	require(cache::over_budget());
	cache::relieve_at(t0 + 2 * window);
	require(new_space->account.shedding() && figures_agree());
	require(cache::ledger_state().reliefs == base.reliefs + 2);
	old_space.reset();
	new_space.reset();
	require(cache::ledger_state().total == 0 && figures_agree());
	cache::set_budget(cache::fallback_budget);
}

/* The accounts that have not been entered for a window are idle and go first,
 * the one idle longest first; the one that is running is not asked while they
 * hold enough to bring the open ones down to the target, however large it is
 * (short of holding more than the line by itself) and however many of them
 * there are. */
void idle_accounts_go_before_the_one_running()
{
	const auto now = t0 + 4 * window;
	cache::set_budget(100 * kib);
	{
		Holder game;
		game.account.charge(70 * kib);
		game.ran(t0 + 1, now);
		Holder oldest;
		Holder older;
		Holder old;
		oldest.account.charge(10 * kib);
		older.account.charge(15 * kib);
		old.account.charge(20 * kib);
		oldest.ran(t0 + 1, t0 + 2);
		older.ran(t0 + 1, t0 + 10);
		old.ran(t0 + 1, t0 + 20);
		require(oldest.account.activity(now) == 0 &&
			old.account.activity(now) == 0 &&
			game.account.activity(now) == window);
		/* 115 KiB against 100: 87.5 is the target, 27.5 to shed. The
		 * oldest and the older one, 25, are not enough; the third is, and
		 * the game stays. */
		cache::relieve_at(now);
		require(oldest.asked() && older.asked() && old.asked());
		require(!game.asked());
		require(figures_agree());
	}
	/* Only as many as the target takes, the one idle longest first. */
	{
		Holder runner;
		runner.account.charge(70 * kib);
		runner.ran(t0 + 1, now);
		Holder first;
		Holder second;
		first.account.charge(20 * kib);
		second.account.charge(15 * kib);
		first.ran(t0 + 1, t0 + 5);
		second.ran(t0 + 1, t0 + 9);
		cache::relieve_at(now);
		require(first.asked() && !second.asked() && !runner.asked());
	}
	cache::set_budget(cache::fallback_budget);
}

/* An account is entered in `count` of the 64 epochs ending at `now`, spread
 * over them, the last of the epochs being now. */
void run_in(Holder &holder, std::uint64_t now, unsigned count)
{
	for (unsigned index = 0; index < count; ++index)
		holder.account.ran(count == 1 ? now :
			now - (window - 1) + index * (window - 1) / (count - 1));
	require(holder.account.activity(now) == count);
}

/* An account that holds more than the line (fifteen sixteenths of the budget)
 * by itself leaves the others less than the reserve however many shed, so it is
 * asked first and the others are left alone when that brings the open ones
 * down: the neighbours of a program that must be shed anyway keep their
 * translations, whether they are idle or running, in any activity band. Here
 * the budget is 100 KiB, the target 87.5 and the line 93.75. */
void an_account_over_the_line_is_asked_alone()
{
	const auto now = t0 + 4 * window;
	cache::set_budget(100 * kib);
	/* The game holds 95 of the 100, and is running; the others are idle. */
	{
		Holder game;
		game.account.charge(95 * kib);
		game.ran(t0 + 1, now);
		Holder idle;
		Holder idler;
		Holder idlest;
		idle.account.charge(10 * kib);
		idler.account.charge(5 * kib);
		idlest.account.charge(3 * kib);
		idle.ran(t0 + 1, t0 + 3);
		idler.ran(t0 + 1, t0 + 2);
		idlest.ran(t0 + 1, t0 + 1);
		const auto base = cache::ledger_state();
		cache::relieve_at(now);
		require(game.asked() && !idle.asked() && !idler.asked() &&
			!idlest.asked());
		const auto state = cache::ledger_state();
		require(state.reliefs == base.reliefs + 1 &&
			state.shed_accounts == base.shed_accounts + 1);
		require(state.shedding == 95 * kib && figures_agree() &&
			!cache::over_budget());
	}
	/* The same beside accounts that run, one in each activity band. */
	{
		Holder game;
		game.account.charge(96 * kib);
		game.ran(t0 + 1, now);
		Holder band1;
		Holder band2;
		Holder band3;
		Holder band4;
		band1.account.charge(1 * kib);
		band2.account.charge(2 * kib);
		band3.account.charge(1 * kib);
		band4.account.charge(1 * kib);
		run_in(band1, now, 8);
		run_in(band2, now, 24);
		run_in(band3, now, 40);
		run_in(band4, now, 60);
		const auto base = cache::ledger_state();
		cache::relieve_at(now);
		require(game.asked() && !band1.asked() && !band2.asked() &&
			!band3.asked() && !band4.asked());
		require(cache::ledger_state().shed_accounts ==
			base.shed_accounts + 1);
		require(figures_agree() && !cache::over_budget());
	}
	/* It need not be the one the order puts last: a less active program that
	 * holds the budget is shed beside a small one that is more active, and
	 * not beside the idle and the less active ones before it. */
	{
		Holder big;
		big.account.charge(95 * kib);
		run_in(big, now, 40);
		Holder small;
		small.account.charge(2 * kib);
		small.ran(t0 + 1, now);
		Holder idle;
		idle.account.charge(4 * kib);
		Holder less;
		less.account.charge(1 * kib);
		run_in(less, now, 10);
		cache::relieve_at(now);
		require(big.asked() && !small.asked() && !idle.asked() &&
			!less.asked());
		require(figures_agree() && !cache::over_budget());
	}
	/* Two that cannot be spared are both asked, and the neighbours are not. */
	{
		Holder first;
		Holder second;
		first.account.charge(95 * kib);
		second.account.charge(96 * kib);
		first.ran(t0 + 1, now);
		second.ran(t0 + 1, now);
		Holder idle;
		idle.account.charge(10 * kib);
		cache::relieve_at(now);
		require(first.asked() && second.asked() && !idle.asked());
		require(figures_agree() && !cache::over_budget());
	}
	/* What it leaves above the target is brought down in the order: the game
	 * goes, and then the idle account before the running one. */
	{
		Holder game;
		game.account.charge(95 * kib);
		game.ran(t0 + 1, now);
		Holder idle;
		idle.account.charge(40 * kib);
		idle.ran(t0 + 1, t0 + 3);
		Holder running;
		running.account.charge(50 * kib);
		running.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(game.asked() && idle.asked() && !running.asked());
		require(figures_agree() && !cache::over_budget());
	}
	/* Not before the budget is passed. */
	{
		const auto base = cache::ledger_state();
		Holder holds;
		holds.account.charge(100 * kib);
		holds.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(!holds.asked() &&
			cache::ledger_state().reliefs == base.reliefs);
	}
	cache::set_budget(cache::fallback_budget);
}

/* A program that holds most of the cache, between the target and the line, is
 * spared while the others can be asked, which is the Asphalt launch: its
 * address space held 213 MiB of a 240 MiB budget (target 210, line 225) beside
 * small ones, and was asked whole where its neighbours would have done. The
 * others go as far as the line, not the target, and the next relief comes only
 * after the stores have taken what that left; the program is asked when it
 * passes the line, or when the open ones are above the budget and nothing else
 * is left. */
void a_program_that_holds_most_of_the_cache_is_spared_up_to_the_line()
{
	const auto now = t0 + 4 * window;
	cache::set_budget(240 * mib);
	require(cache::budget() - cache::budget() / cache::reserve_divisor ==
		225 * mib && cache::budget() - cache::budget() / cache::target_divisor ==
		210 * mib);
	const auto charge = [](Holder &holder, std::size_t bytes) {
		holder.account.charge(static_cast<std::ptrdiff_t>(bytes));
	};
	/* The game 213 MiB, running, and 34 of neighbours: the idle ones go, the
	 * one idle longest first, then the less active, until the open ones are
	 * at the line, 225. The running neighbour stays, though the open ones
	 * are above the target. */
	{
		Holder game;
		Holder idle_long;
		Holder idle;
		Holder less;
		Holder running;
		charge(game, 213 * mib);
		charge(idle_long, 12 * mib);
		charge(idle, 9 * mib);
		charge(less, 8 * mib);
		charge(running, 5 * mib);
		game.ran(t0 + 1, now);
		idle_long.ran(t0 + 1, t0 + 2);
		idle.ran(t0 + 1, t0 + 9);
		run_in(less, now, 10);
		running.ran(t0 + 1, now);
		require(cache::over_budget());
		const auto base = cache::ledger_state();
		cache::relieve_at(now);
		require(!game.asked() && !running.asked());
		require(idle_long.asked() && idle.asked() && less.asked());
		const auto state = cache::ledger_state();
		require(state.reliefs == base.reliefs + 1 &&
			state.shed_accounts == base.shed_accounts + 3);
		require(state.shedding == 29 * mib && figures_agree());
		require(cache::ledger().open.load() == 218 * mib);
		/* The stores of the others now have 22 MiB before the budget, and no
		 * relief runs before they pass it, however many stores that takes. */
		charge(running, 22 * mib);
		require(!cache::over_budget());
		cache::relieve_at(now);
		require(cache::ledger_state().reliefs == state.reliefs &&
			!running.asked());
		charge(running, 1);
		require(cache::over_budget());
		cache::relieve_at(now);
		require(running.asked() && !game.asked());
		require(cache::ledger_state().reliefs == state.reliefs + 1);
		require(cache::ledger().open.load() == 213 * mib && figures_agree());
	}
	/* One byte over the line, it is asked first and alone: the neighbours that
	 * could not have spared it keep what they have. The line itself is spared. */
	{
		Holder game;
		Holder idle;
		Holder running;
		charge(game, 225 * mib + 1);
		charge(idle, 10 * mib);
		charge(running, 5 * mib);
		game.ran(t0 + 1, now);
		idle.ran(t0 + 1, t0 + 2);
		running.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(game.asked() && !idle.asked() && !running.asked());
		require(figures_agree());
	}
	{
		Holder game;
		Holder idle;
		charge(game, 225 * mib);
		charge(idle, 16 * mib);
		game.ran(t0 + 1, now);
		idle.ran(t0 + 1, t0 + 2);
		cache::relieve_at(now);
		require(!game.asked() && idle.asked() && figures_agree());
	}
	/* The line is as far as the neighbours bring the open ones: the running one
	 * stays when the idle one takes them to it. */
	{
		Holder game;
		Holder idle;
		Holder running;
		charge(game, 220 * mib);
		charge(idle, 16 * mib);
		charge(running, 5 * mib);
		game.ran(t0 + 1, now);
		idle.ran(t0 + 1, t0 + 2);
		running.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(!game.asked() && idle.asked() && !running.asked());
		require(cache::ledger().open.load() == 225 * mib && figures_agree());
	}
	/* Alone over the budget it is asked: nothing could make room. Alone at the
	 * budget the open ones do not pass it, and nothing is asked. */
	{
		Holder game;
		charge(game, 240 * mib);
		game.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(!game.asked());
		charge(game, 1);
		cache::relieve_at(now);
		require(game.asked() && figures_agree());
	}
	/* Two between the target and the line, with nothing else to ask: the open
	 * ones are above the budget, so the one the order puts first goes (of one
	 * band, the smaller), and the other, now within the line, stays. */
	{
		Holder first;
		Holder second;
		charge(first, 213 * mib);
		charge(second, 212 * mib);
		first.ran(t0 + 1, now);
		second.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(second.asked() && !first.asked() && figures_agree());
	}
	/* One that nobody runs is not spared: an idle program of 213 MiB is asked
	 * first and alone, as it is above the target, and the running neighbours
	 * keep what they have. */
	{
		Holder idle_big;
		Holder running;
		Holder other;
		charge(idle_big, 213 * mib);
		charge(running, 20 * mib);
		charge(other, 9 * mib);
		idle_big.ran(t0 + 1, t0 + 2);
		running.ran(t0 + 1, now);
		other.ran(t0 + 1, now);
		require(idle_big.account.activity(now) == 0);
		cache::relieve_at(now);
		require(idle_big.asked() && !running.asked() && !other.asked());
		require(figures_agree());
	}
	/* Spared accounts are asked only after every other: an idle one and a
	 * running one go first, and a running program of 224 MiB, within the line,
	 * stays however little room that leaves. */
	{
		Holder game;
		Holder idle;
		Holder running;
		charge(game, 224 * mib);
		charge(idle, 9 * mib);
		charge(running, 8 * mib);
		game.ran(t0 + 1, now);
		idle.ran(t0 + 1, t0 + 2);
		running.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(!game.asked() && idle.asked() && running.asked());
		require(cache::ledger().open.load() == 224 * mib && figures_agree());
	}
	cache::set_budget(cache::fallback_budget);
}

/* Between accounts that run, the less active goes first, a band of a quarter
 * of the window at a time, and within a band the smaller. The few epochs by
 * which two running accounts differ do not decide. */
void running_accounts_go_by_activity_band_then_size()
{
	const auto now = t0 + 4 * window;
	cache::set_budget(100 * kib);
	{
		/* Entered in 22 of the last 64 epochs: band 2. */
		Holder bursty;
		bursty.account.charge(50 * kib);
		for (std::uint64_t epoch = now - 63; epoch <= now; epoch += 3)
			bursty.account.ran(epoch);
		require(bursty.account.activity(now) == 22);
		Holder steady_small;
		Holder steady_large;
		steady_small.account.charge(30 * kib);
		steady_large.account.charge(40 * kib);
		steady_small.ran(t0 + 1, now);
		steady_large.ran(t0 + 1, now);
		/* 120 against 100, target 87.5: bursty covers the 32.5, though it
		 * holds more than the steady ones and ran in the last epoch. */
		cache::relieve_at(now);
		require(bursty.asked() && !steady_small.asked() &&
			!steady_large.asked());
	}
	/* In one band: 60 of 64 epochs against all 64 is no difference, and the
	 * smaller goes, whichever is the more active. */
	{
		Holder missed;
		missed.account.charge(70 * kib);
		missed.ran(t0 + 1, now - 20);
		missed.ran(now - 15, now);
		require(missed.account.activity(now) == window - 4);
		Holder all;
		all.account.charge(40 * kib);
		all.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(all.asked() && !missed.asked());
	}
	/* Twelve idle accounts of 1 MiB that free too little, and running ones of
	 * 17, 77 and 88 MiB whose last entries differ by a few epochs. */
	cache::set_budget(192 * mib);
	{
		Holder system_server;
		Holder service;
		Holder big;
		system_server.account.charge(static_cast<std::ptrdiff_t>(88 * mib));
		service.account.charge(static_cast<std::ptrdiff_t>(17 * mib));
		big.account.charge(static_cast<std::ptrdiff_t>(77 * mib));
		system_server.ran(t0 + 1, now - 8);
		system_server.ran(now - 6, now - 5);
		service.ran(t0 + 1, now);
		big.ran(t0 + 1, now);
		Holder others[12];
		for (auto &other : others) {
			other.account.charge(static_cast<std::ptrdiff_t>(mib));
			other.ran(t0 + 1, now - 70);
		}
		/* 194 MiB against 192, target 168: 26 to shed. The idle ones give
		 * 12 and the running ones are of one band, so the smallest of
		 * them, 17 MiB, covers the 14 that are left. */
		cache::relieve_at(now);
		for (const auto &other : others)
			require(other.asked());
		require(service.asked() && !system_server.asked() && !big.asked());
	}
	cache::set_budget(cache::fallback_budget);
}

/* Accounts equal in every respect are asked newest-opened first, and an empty
 * account is never asked, however far over the rest are. */
void ties_and_empty_accounts()
{
	const auto now = t0 + 4 * window;
	cache::set_budget(64 * kib);
	/* Idle, last ran in one epoch, one size. */
	{
		Holder first;
		Holder second;
		first.account.charge(40 * kib);
		second.account.charge(40 * kib);
		first.ran(t0 + 1, t0 + 2);
		second.ran(t0 + 1, t0 + 2);
		cache::relieve_at(now);
		require(second.asked() && !first.asked());
	}
	/* Idle, last ran in one epoch: the larger first. */
	{
		Holder small;
		Holder large;
		small.account.charge(30 * kib);
		large.account.charge(50 * kib);
		small.ran(t0 + 1, t0 + 2);
		large.ran(t0 + 1, t0 + 2);
		cache::relieve_at(now);
		require(large.asked() && !small.asked());
	}
	/* Running, equally active, one size: the one that ran earlier. */
	{
		Holder earlier;
		Holder later;
		earlier.account.charge(40 * kib);
		later.account.charge(40 * kib);
		earlier.ran(t0 + 1, now - 1);
		later.ran(t0 + 1, now);
		cache::relieve_at(now);
		require(earlier.asked() && !later.asked());
	}
	/* Within the budget nothing is asked, and an empty account never is. */
	{
		const auto base = cache::ledger_state();
		Holder empty;
		Holder full;
		full.account.charge(64 * kib);
		require(!cache::over_budget());
		cache::relieve_at(now);
		require(cache::ledger_state().reliefs == base.reliefs);
		require(!full.account.shedding() && full.owner.notified == 0);
		full.account.charge(1);
		cache::relieve_at(now);
		require(full.account.shedding() && !empty.account.shedding());
		require(empty.owner.notified == 0);
	}
	cache::set_budget(cache::fallback_budget);
}

/* A store past the budget relieves it even while another thread holds the
 * ledger's lock -- an address space opening or going, a census, another
 * relief -- by waiting its turn. One that gave up would go on storing past the
 * budget for as long as the holder kept the lock, which a preempted holder
 * keeps for a scheduling quantum. */
void relief_waits_for_the_lock()
{
	cache::set_budget(64 * kib);
	Holder space;
	std::atomic<bool> held{};
	std::thread holder{[&held] {
		const std::lock_guard guard{cache::ledger().mutex};
		held.store(true);
		std::this_thread::sleep_for(std::chrono::milliseconds{200});
	}};
	while (!held.load())
		std::this_thread::yield();
	space.account.charge(65 * kib);
	require(cache::over_budget());
	cache::relieve();
	require(space.account.shedding() && space.owner.notified == 1);
	holder.join();
	require(!cache::over_budget() && figures_agree());
	cache::set_budget(cache::fallback_budget);
}

/* The largest open account is what the session record's checkpoint writes
 * beside the relief counters: shedding accounts do not count, and a ledger
 * whose lock is taken is not waited for. */
void the_largest_open_account_is_read_without_waiting()
{
	cache::set_budget(64 * kib);
	std::size_t largest = 123;
	require(cache::largest_open_bytes(largest) && largest == 0,
		"a ledger without accounts has no largest");
	Holder small;
	Holder large;
	Holder middle;
	small.account.charge(10 * kib);
	large.account.charge(50 * kib);
	middle.account.charge(30 * kib);
	require(cache::largest_open_bytes(largest) && largest == 50 * kib);
	const auto reliefs = cache::ledger_state().reliefs;
	/* A window on, no account was entered since they opened: the idle ones
	 * go larger first. */
	cache::relieve_at(t0 + window);
	require(large.account.shedding() &&
		cache::ledger_state().reliefs == reliefs + 1);
	require(cache::largest_open_bytes(largest) && largest == 30 * kib,
		"an account asked to shed was counted as open");

	std::atomic<bool> held{};
	std::atomic<bool> release{};
	std::thread holder{[&] {
		const std::lock_guard guard{cache::ledger().mutex};
		held.store(true);
		while (!release.load())
			std::this_thread::yield();
	}};
	while (!held.load())
		std::this_thread::yield();
	largest = 777;
	const bool read = cache::largest_open_bytes(largest);
	release.store(true);
	holder.join();
	require(!read && largest == 777,
		"a reading waited for the ledger or changed its answer");
	cache::set_budget(cache::fallback_budget);
}

/* Within the budget nothing is asked to shed, and an empty account never
 * is, however far over the rest are. */
void within_the_budget_nothing_sheds()
{
	cache::set_budget(64 * kib);
	const auto base = cache::ledger_state();
	Holder empty;
	Holder full;
	full.account.charge(64 * kib);
	require(!cache::over_budget());
	cache::relieve();
	require(cache::ledger_state().reliefs == base.reliefs);
	require(!full.account.shedding() && full.owner.notified == 0);
	full.account.charge(1);
	cache::relieve();
	require(full.account.shedding() && !empty.account.shedding());
	require(empty.owner.notified == 0);
	cache::set_budget(cache::fallback_budget);
}

/* Executors charge their address spaces and relieve the budget from their
 * own threads at once, and an address space comes and goes while another
 * relieves: what the ledger holds stays the sum of its accounts. */
void concurrent_charges_add_up()
{
	cache::set_budget(256 * kib);
	constexpr unsigned threads = 4;
	std::array<std::thread, threads> workers;
	std::atomic<std::size_t> notified{};
	for (unsigned index = 0; index < threads; ++index)
		workers[index] = std::thread{[index, &notified] {
			std::mt19937_64 random{index + 1U};
			for (unsigned round = 0; round < 200; ++round) {
				Holder holder;
				std::size_t held = 0;
				for (unsigned step = 0; step < 50; ++step) {
					const auto bytes = 512 +
						random() % (8 * kib);
					holder.account.charge(
						static_cast<std::ptrdiff_t>(bytes));
					held += bytes;
					if (cache::over_budget())
						cache::relieve();
					if (holder.account.shedding())
						break;
				}
				holder.account.charge(
					-static_cast<std::ptrdiff_t>(held / 2));
				notified.fetch_add(holder.owner.notified,
					std::memory_order_relaxed);
			}
		}};
	for (auto &worker : workers)
		worker.join();
	const auto state = cache::ledger_state();
	require(state.total == 0 && state.shedding == 0 &&
		state.accounts == 0);
	/* Every charge landed in one figure or the other exactly once. */
	require(cache::ledger().open.load() == 0 &&
		cache::ledger().shedding.load() == 0);
	require(notified.load() != 0);
	cache::set_budget(cache::fallback_budget);
}

} // namespace

int main()
{
	require(cache::budget() == cache::fallback_budget);
	derived_budgets();
	measured_workloads_stay_well_within_their_budget();
	an_account_remembers_the_epochs_it_ran_in();
	relief_accounting();
	a_shed_account_that_shrinks_leaves_the_open_ones_exact();
	idle_accounts_go_before_the_one_running();
	an_account_over_the_line_is_asked_alone();
	a_program_that_holds_most_of_the_cache_is_spared_up_to_the_line();
	running_accounts_go_by_activity_band_then_size();
	ties_and_empty_accounts();
	relief_waits_for_the_lock();
	the_largest_open_account_is_read_without_waiting();
	within_the_budget_nothing_sheds();
	concurrent_charges_add_up();
	return 0;
}
