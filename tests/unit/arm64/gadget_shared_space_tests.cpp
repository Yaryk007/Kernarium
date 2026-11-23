// SPDX-License-Identifier: GPL-2.0-or-later
/* The limbo both gadget engines' shared address spaces keep their unpublished
 * bodies in, on its own: a body is freed only once every executor that was
 * inside translated code when it was unpublished has left, and running out of
 * memory part way only delays that. The engines' use of it is tested with each
 * engine. */
#include "cpu/gadget_shared_space.hpp"

#include "allocation_failure.hpp"
#include "require.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace cache = uml::cpu::gadget_cache;
using uml::testing::require;

namespace {

struct Body final {
	unsigned frees{};
};

/* What an engine's free_bodies does, without allocating: the injector fails
 * the allocations of the limbo alone. */
void free_bodies(std::vector<Body *> &bodies) noexcept
{
	for (auto *body : bodies)
		++body->frees;
	bodies.clear();
}

/* Executors attached to one address space: the reclaimer, idle ones and one
 * inside translated code, attached last so that a snapshot that stops part
 * way misses it. */
struct Executors final {
	static constexpr std::size_t idle_count = 4;

	Executors()
	{
		cache::announce_entry(self);
		attached.attach(&self);
		for (auto &announcement : idle)
			attached.attach(&announcement);
		cache::announce_entry(reader);
		attached.attach(&reader);
	}

	cache::ExecutorAnnouncement self;
	cache::ExecutorAnnouncement idle[idle_count];
	cache::ExecutorAnnouncement reader;
	cache::AttachedExecutors attached;
};

/* A reclaim that cannot take its snapshot of the executors for want of memory
 * frees nothing early and throws nothing, whichever of its allocations fails:
 * the body still waits for the executor that was inside when it was
 * unpublished, and goes once that executor has left. */
void a_failed_snapshot_frees_nothing_early()
{
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 64, "the failure point never moved past the "
			"end of a reclaim");
		Executors executors;
		cache::Limbo<Body> limbo;
		Body body;
		limbo.push(&body);
		uml::testing::fail_after_allocations(allowed);
		bool threw = false;
		try {
			limbo.reclaim(&executors.self, executors.attached,
				free_bodies);
		} catch (const std::bad_alloc &) {
			threw = true;
		}
		/* True once the reclaim made fewer allocations than allowed. */
		const bool swept =
			uml::testing::cancel_pending_allocation_failure();
		limbo.reclaim(&executors.self, executors.attached, free_bodies);
		require(body.frees == 0, "a body was freed while an executor "
			"that was inside when it was unpublished still is");
		require(!threw, "a reclaim threw for want of memory");
		require(limbo.size() == 1, "a body left limbo unfreed");
		cache::announce_exit(executors.reader);
		limbo.reclaim(&executors.self, executors.attached, free_bodies);
		require(body.frees == 1 && limbo.size() == 0,
			"a body was not freed once every executor had left");
		if (swept)
			return;
	}
}

/* A body that has waited its turn is freed even when the snapshot for the
 * bodies unpublished after it cannot be taken, and those wait for the
 * executors inside when they were unpublished. */
void a_failed_snapshot_keeps_the_next_bodies_waiting()
{
	Executors executors;
	cache::Limbo<Body> limbo;
	Body first;
	limbo.push(&first);
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(first.frees == 0);
	cache::announce_exit(executors.reader);
	/* More executors attach than the last snapshot had room for, so that
	 * taking the next one allocates, and the last of them enters. */
	cache::ExecutorAnnouncement late[8];
	for (auto &announcement : late)
		executors.attached.attach(&announcement);
	cache::announce_entry(late[7]);
	Body second;
	limbo.push(&second);
	uml::testing::fail_next_allocation();
	bool threw = false;
	try {
		limbo.reclaim(&executors.self, executors.attached, free_bodies);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	require(!uml::testing::cancel_pending_allocation_failure(),
		"the snapshot made no allocation");
	require(first.frees == 1, "a body whose executors had left was kept");
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(second.frees == 0, "a body was freed while an executor that "
		"was inside when it was unpublished still is");
	require(!threw, "a reclaim threw for want of memory");
	cache::announce_exit(late[7]);
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(second.frees == 1 && limbo.size() == 0,
		"a body was not freed once every executor had left");
}

/* A body the limbo has no memory to queue stays where it is, never freed by
 * the limbo, and the push throws nothing. */
void a_body_that_cannot_be_queued_is_never_freed()
{
	Executors executors;
	cache::announce_exit(executors.reader);
	cache::Limbo<Body> limbo;
	Body body;
	uml::testing::fail_next_allocation();
	bool threw = false;
	try {
		limbo.push(&body);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	require(!uml::testing::cancel_pending_allocation_failure(),
		"the push made no allocation");
	require(!threw, "a push threw for want of memory");
	limbo.reclaim(&executors.self, executors.attached, free_bodies);
	require(body.frees == 0 && limbo.size() == 0);
}

/* What the registry asks of an engine's address space, without an engine. */
struct FakeKey final {
	unsigned long generation{};
	std::uint8_t mode{};
	unsigned options{};
	const void *owner{};
};

class FakeSpace final {
public:
	FakeSpace(FakeKey key, std::uint64_t) : key_{key} {}

	[[nodiscard]] const FakeKey &key() const noexcept { return key_; }
	void attach(const cache::ExecutorAnnouncement *) noexcept { ++executors_; }
	[[nodiscard]] bool detach(const cache::ExecutorAnnouncement *) noexcept
	{
		return --executors_ == 0;
	}
	[[nodiscard]] bool close() noexcept { return !closed_.exchange(true); }
	[[nodiscard]] bool closed() const noexcept { return closed_.load(); }
	[[nodiscard]] bool shedding() const noexcept { return shedding_; }
	void shed() noexcept { shedding_ = true; }
	void request_renewal() noexcept { ++renewals_asked; }
	static void note_closed() noexcept {}

	unsigned renewals_asked{};

private:
	FakeKey key_;
	unsigned executors_{};
	std::atomic<bool> closed_{};
	bool shedding_{};
};

/* A shared address space closed for renewal is counted once in the cache
 * ledger, the figure the session record shows beside the reliefs; one the
 * budget asked to shed is a shed account instead. */
void a_renewal_is_counted_when_the_space_is_closed_for_it()
{
	using Registry = cache::SpaceRegistry<FakeSpace, FakeKey>;
	auto &registry = Registry::instance();
	cache::ExecutorAnnouncement executor;
	const auto before = cache::ledger().renewals.load();
	auto *renewed = registry.attach({1, 0, 0, nullptr}, 0, &executor);
	registry.close(renewed);
	require(cache::ledger().renewals.load() == before + 1 &&
		renewed->renewals_asked == 1, "a renewal was not counted");
	registry.close(renewed);
	require(cache::ledger().renewals.load() == before + 1,
		"a space closed twice was counted twice");
	registry.detach(renewed, &executor);

	auto *shed = registry.attach({2, 0, 0, nullptr}, 0, &executor);
	shed->shed();
	registry.close(shed);
	require(cache::ledger().renewals.load() == before + 1 &&
		shed->renewals_asked == 1, "a shed space was counted as renewed");
	registry.detach(shed, &executor);
}

/* A space the budget sheds while an executor attaches to it, at a point a
 * test chooses: relieve_at() sets the account's flag and then asks the
 * executors attached, which is what shed_now() does, under no lock of the
 * registry. */
class RacingSpace final {
public:
	/* Where the next attach() is raced: the budget acts just before this
	 * executor is among those attached, or just after. */
	enum class When { never, before_the_push, after_the_push };
	static inline When race = When::never;

	RacingSpace(FakeKey key, std::uint64_t) : key_{key} {}

	[[nodiscard]] const FakeKey &key() const noexcept { return key_; }
	void attach(const cache::ExecutorAnnouncement *announcement)
	{
		if (race == When::before_the_push)
			shed_now();
		attached_.attach(announcement);
		if (race == When::after_the_push)
			shed_now();
	}
	[[nodiscard]] bool detach(const cache::ExecutorAnnouncement *announcement)
	{
		return attached_.detach(announcement);
	}
	[[nodiscard]] bool close() noexcept { return !closed_.exchange(true); }
	[[nodiscard]] bool closed() const noexcept { return closed_.load(); }
	[[nodiscard]] bool shedding() const noexcept { return shedding_.load(); }
	void request_renewal() { attached_.request_renewal(); }
	static void note_closed() noexcept {}

private:
	void shed_now()
	{
		race = When::never;
		shedding_.store(true);
		attached_.request_renewal();
	}

	FakeKey key_;
	cache::AttachedExecutors attached_;
	std::atomic<bool> closed_{};
	std::atomic<bool> shedding_{};
};

/* An executor that attaches to a shared space as the budget sheds it is never
 * left in it with nothing to ask it to let go: what attach() returns is a
 * space that is not being shed, or the executor's announcement is set. The
 * budget's ask reaches the executors attached when it is made, so one that
 * attaches between the registry's check of the space and its own entry in the
 * list is not among them. Raced before its entry, the old registry handed out
 * the shed space and nobody asked the executor to let go of it; raced after,
 * the ask reaches it, and the registry has no more to do than not to lose
 * it. */
void an_executor_attaching_as_the_budget_sheds_is_asked_to_let_go(
	RacingSpace::When when, unsigned long generation)
{
	using Registry = cache::SpaceRegistry<RacingSpace, FakeKey>;
	auto &registry = Registry::instance();
	cache::ExecutorAnnouncement first;
	cache::ExecutorAnnouncement second;
	auto *held = registry.attach({generation, 0, 0, nullptr}, 0, &first);
	RacingSpace::race = when;
	auto *got = registry.attach({generation, 0, 0, nullptr}, 0, &second);
	require(RacingSpace::race == RacingSpace::When::never,
		"the attach was not raced");
	require(held->shedding() && first.renew.load(),
		"the first executor was not asked to let go");
	require((!got->shedding() && !got->closed()) || second.renew.load(),
		"an executor was left in a shed space with no ask to let go of it");
	/* The shed space is closed for the others to let go of, and the
	 * executor goes on in a space of its own. */
	require(got != held && !got->shedding() && held->closed(),
		"the shed space was handed out or left open");
	registry.detach(held, &first);
	registry.detach(got, &second);
}

/* The three ways a window sees an address space it holds. */
void a_window_tells_idle_from_held_here_from_running_elsewhere()
{
	using cache::PlaceUse;
	cache::ExecutorAnnouncement asking;
	cache::ExecutorAnnouncement other;
	cache::AttachedExecutors attached;
	attached.attach(&asking);
	cache::Account account{[](void *) noexcept {}, nullptr};
	const auto now = cache::epoch();

	/* A new account counts as entered, so with only this cache holding it
	 * the space is held here, and with another it is running elsewhere. */
	require(cache::place_use(attached, account, false, now) ==
		PlaceUse::here, "a space held by one cache was not held here");
	attached.attach(&other);
	require(cache::place_use(attached, account, false, now) ==
		PlaceUse::elsewhere, "a space two caches hold was not shared");

	/* A closed space is let go of at the next entry whoever runs it. */
	require(cache::place_use(attached, account, true, now) ==
		PlaceUse::idle, "a closed space was kept by its use");

	/* Nobody entered it for a whole window: idle however many hold it,
	 * until some executor enters it. */
	const auto later = now + cache::window_epochs;
	require(cache::place_use(attached, account, false, later) ==
		PlaceUse::idle, "a space nobody entered was not idle");
	account.ran(later);
	require(cache::place_use(attached, account, false, later) ==
		PlaceUse::elsewhere, "a space entered again was not running");
	require(attached.detach(&other) == false);
	require(cache::place_use(attached, account, false, later) ==
		PlaceUse::here, "a space left to one cache was not held here");
	attached.attach(&other);

	/* The budget asks it to shed: its holders let go of it at their next
	 * entry, so no window keeps it, though it is entered all the time. */
	cache::set_budget(1024);
	account.charge(4096);
	cache::relieve_at(later);
	cache::set_budget(cache::fallback_budget);
	require(account.shedding(), "the budget did not ask the space to shed");
	require(cache::place_use(attached, account, false, later) ==
		PlaceUse::idle, "a space asked to shed was kept by its use");
}

} // namespace

int main()
{
	a_failed_snapshot_frees_nothing_early();
	a_failed_snapshot_keeps_the_next_bodies_waiting();
	a_body_that_cannot_be_queued_is_never_freed();
	a_renewal_is_counted_when_the_space_is_closed_for_it();
	a_window_tells_idle_from_held_here_from_running_elsewhere();
	an_executor_attaching_as_the_budget_sheds_is_asked_to_let_go(
		RacingSpace::When::before_the_push, 31);
	an_executor_attaching_as_the_budget_sheds_is_asked_to_let_go(
		RacingSpace::When::after_the_push, 32);
	return 0;
}
