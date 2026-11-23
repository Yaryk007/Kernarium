// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The activity record the stall watch sleeps by (runtime/carrier_activity):
 * which vCPU carriers are outside an idle wait, and the timer of the watch's
 * queue that wakes it 50 ms after a carrier leaves one. The test thread plays
 * the watch, and threads that name themselves carriers play the carriers.
 *
 * The timer is checked by the time its event arrives, with generous upper
 * bounds (a loaded host delays a wake-up) and lower bounds that hold on any
 * host: a timer that must not have gone off yet does not go off early.
 */
#include "carrier_activity.hpp"
#include "require.hpp"
#include "thread_nap.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

namespace {

using namespace uml::runtime;
using uml::testing::nap;
using uml::testing::require;

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Waited final {
	unsigned bits{};
	double ms{};
};

/* Waits as the watch does, for at most `limit_ms`. */
Waited wait_for(unsigned limit_ms, Clock::time_point since = Clock::now())
{
	const std::uint64_t timeout = static_cast<std::uint64_t>(limit_ms) * 1'000'000;
	Waited waited;
	waited.bits = watch_queue_wait(&timeout);
	waited.ms = milliseconds_since(since);
	return waited;
}

void drain()
{
	for (;;) {
		const std::uint64_t none = 0;
		if (watch_queue_wait(&none) == 0)
			return;
	}
}

/* A thread that runs what the test hands it, one piece at a time, so that the
 * steps of a carrier are in the order the test says. */
class Actor final {
public:
	Actor() : thread_{[this] { loop(); }} {}

	~Actor()
	{
		{
			std::lock_guard lock{mutex_};
			finished_ = true;
		}
		changed_.notify_all();
		thread_.join();
	}

	void run(std::function<void()> work)
	{
		std::unique_lock lock{mutex_};
		work_ = std::move(work);
		changed_.notify_all();
		changed_.wait(lock, [this] { return !work_; });
	}

	/* Scopes the actor holds across its steps. */
	std::optional<CarrierIdle> idle;
	std::optional<CarrierIdle> inner;

private:
	void loop()
	{
		std::unique_lock lock{mutex_};
		for (;;) {
			changed_.wait(lock, [this] { return work_ || finished_; });
			if (work_) {
				work_();
				work_ = nullptr;
				changed_.notify_all();
			} else {
				return;
			}
		}
	}

	std::mutex mutex_;
	std::condition_variable changed_;
	std::function<void()> work_;
	bool finished_{};
	std::thread thread_;
};

constexpr unsigned timer = watch_woken_by_timer;

timespec relative_ms(long milliseconds)
{
	return {milliseconds / 1000, (milliseconds % 1000) * 1'000'000L};
}

timespec realtime_after_ms(long milliseconds)
{
	timespec now{};
	clock_gettime(CLOCK_REALTIME, &now);
	const long nanoseconds = now.tv_nsec + (milliseconds % 1000) * 1'000'000L;
	return {now.tv_sec + milliseconds / 1000 + nanoseconds / 1'000'000'000L,
		nanoseconds % 1'000'000'000L};
}

/* Every case starts and ends with no carrier and nothing queued. */
struct Case final {
	explicit Case(const char *name) : name{name}
	{
		require(carriers_outside_idle() == 0 && carriers_reporting() == 0,
			std::string{"carriers left behind before "} + name);
		require(watch_queue_open(), "the watch's queue");
		drain();
	}

	~Case()
	{
		watch_queue_close();
	}

	const char *name;
};

void threads_that_are_not_carriers_change_nothing()
{
	const Case scope{"non-carriers"};
	Actor other;
	Actor numbered;
	/* The kernel's other threads (uml-irq-notify, uml-rtc-alarm) wait too, and
	 * a number past the table's is no carrier either. */
	numbered.run([] {
		carrier_named(carrier_capacity);
		carrier_named(1001);
	});
	for (int round = 0; round < 3; ++round) {
		other.run([&] { other.idle.emplace(); });
		numbered.run([&] { numbered.idle.emplace(); });
		require(carriers_outside_idle() == 0 && carriers_reporting() == 0);
		other.run([&] { other.idle.reset(); });
		numbered.run([&] { numbered.idle.reset(); });
		require(carriers_outside_idle() == 0 && carriers_reporting() == 0);
	}
	const timespec limit = relative_ms(30);
	other.run([&] {
		const CarrierIdle timed{&limit, CarrierIdle::Limit::relative};
	});
	/* Nothing was armed, either: the queue stays silent past the 50 ms. */
	const Waited waited = wait_for(200);
	require(waited.bits == 0, "a thread that is no carrier armed the watch");
}

void a_carrier_arms_the_watch_when_it_starts_and_when_it_leaves_idle()
{
	const Case scope{"arming"};
	Actor actor;
	const Clock::time_point began = Clock::now();
	actor.run([] { carrier_named(3); });
	require(carriers_outside_idle() == 8 && carriers_reporting() == 8);
	/* A carrier starts outside its idle wait: the watch is armed. */
	Waited waited = wait_for(3000, began);
	require(waited.bits == timer, "a new carrier did not arm the watch");
	require(waited.ms >= 40, "the watch was armed before 50 ms");

	/* In its idle wait it is out of the way, and the timer is removed. */
	actor.run([&] { actor.idle.emplace(); });
	require(carriers_outside_idle() == 0 && carriers_reporting() == 8);
	require(wait_for(150).bits == 0, "the watch was armed for a carrier idle");

	/* Leaving the wait arms it 50 ms on. */
	const Clock::time_point left = Clock::now();
	actor.run([&] { actor.idle.reset(); });
	require(carriers_outside_idle() == 8);
	waited = wait_for(3000, left);
	require(waited.bits == timer, "leaving the idle wait did not arm the watch");
	require(waited.ms >= 40, "the watch was armed before 50 ms");
}

void a_carrier_that_blinks_never_wakes_the_watch()
{
	const Case scope{"blinking"};
	/* A guest's hrtimer wakes a carrier for a moment, again and again: every
	 * cycle leaves its wait and enters it within a millisecond. */
	std::thread blinker{[] {
		carrier_named(1);
		for (int round = 0; round < 120; ++round) {
			{
				const CarrierIdle idle;
				nap(8000);
			}
			nap(1000);
		}
	}};
	const Waited waited = wait_for(1000);
	blinker.join();
	require(waited.bits == 0,
		"a carrier that is idle but for a moment woke the watch");
	require(carriers_outside_idle() == 0 && carriers_reporting() == 0);
	require(wait_for(100).bits == 0, "a carrier that ended left the watch armed");
}

void the_first_carrier_out_arms_and_the_last_one_in_removes()
{
	const Case scope{"two carriers"};
	Actor first;
	Actor second;
	first.run([&] { carrier_named(0); first.idle.emplace(); });
	second.run([&] { carrier_named(1); second.idle.emplace(); });
	require(carriers_outside_idle() == 0);
	drain();

	const Clock::time_point began = Clock::now();
	first.run([&] { first.idle.reset(); });
	Waited waited = wait_for(3000, began);
	require(waited.bits == timer && waited.ms >= 40);

	/* A second carrier out, the first still out: no one is first, so nothing is
	 * armed again. */
	second.run([&] { second.idle.reset(); });
	require(carriers_outside_idle() == 3);
	require(wait_for(150).bits == 0, "a carrier armed the watch for a second time");

	/* The first in again: not the last. Then the last in: removed, and a
	 * carrier that leaves again arms it afresh. */
	first.run([&] { first.idle.emplace(); });
	require(carriers_outside_idle() == 2);
	second.run([&] { second.idle.emplace(); });
	require(carriers_outside_idle() == 0);
	require(wait_for(150).bits == 0);
	const Clock::time_point again = Clock::now();
	second.run([&] { second.idle.reset(); });
	waited = wait_for(3000, again);
	require(waited.bits == timer && waited.ms >= 40, "no arming after a quiet spell");
}

/* The earliest deadline of a timed wait, from any carrier, plus 50 ms. */
void the_last_carrier_in_pushes_the_timer_past_the_earliest_deadline()
{
	const Case scope{"timed waits"};
	Actor first;
	Actor second;
	first.run([&] { carrier_named(0); });
	second.run([&] { carrier_named(1); });

	/* A timed wait of 300 ms entered first, an untimed one entered last: the
	 * timer is not removed, it goes off 50 ms past the first one's deadline. */
	Clock::time_point began = Clock::now();
	const timespec three_hundred = relative_ms(300);
	first.run([&] {
		first.idle.emplace(&three_hundred, CarrierIdle::Limit::relative);
	});
	second.run([&] { second.idle.emplace(); });
	require(carriers_outside_idle() == 0);
	Waited waited = wait_for(5000, began);
	require(waited.bits == timer, "the deadline of a timed wait was lost");
	require(waited.ms >= 340, "the timer went off before the deadline and 50 ms");

	/* The earliest of two: 200 ms and 3 s, entered in either order. */
	first.run([&] { first.idle.reset(); });
	second.run([&] { second.idle.reset(); });
	drain();
	const timespec three_seconds = relative_ms(3000);
	const timespec two_hundred = relative_ms(200);
	began = Clock::now();
	first.run([&] {
		first.idle.emplace(&three_seconds, CarrierIdle::Limit::relative);
	});
	second.run([&] {
		second.idle.emplace(&two_hundred, CarrierIdle::Limit::relative);
	});
	waited = wait_for(2500, began);
	require(waited.bits == timer, "the earliest deadline was lost");
	require(waited.ms >= 240 && waited.ms < 2500,
		"the timer follows the later deadline");

	/* The same on the realtime clock, as a condition wait takes it. */
	first.run([&] { first.idle.reset(); });
	second.run([&] { second.idle.reset(); });
	drain();
	began = Clock::now();
	const timespec at = realtime_after_ms(250);
	first.run([&] {
		first.idle.emplace(&at, CarrierIdle::Limit::realtime);
	});
	second.run([&] { second.idle.emplace(); });
	waited = wait_for(5000, began);
	require(waited.bits == timer && waited.ms >= 290,
		"a deadline on the realtime clock is not honored: " +
		std::to_string(waited.bits) + " at " + std::to_string(waited.ms));

	/* A deadline long past is due at once. */
	first.run([&] { first.idle.reset(); });
	second.run([&] { second.idle.reset(); });
	drain();
	began = Clock::now();
	const timespec past = realtime_after_ms(-5000);
	first.run([&] {
		first.idle.emplace(&past, CarrierIdle::Limit::realtime);
	});
	second.run([&] { second.idle.emplace(); });
	waited = wait_for(5000, began);
	require(waited.bits == timer && waited.ms < 3000, "an overdue wait is not found");

	/* A wait of more than a day is one without a limit: the timer is removed. */
	first.run([&] { first.idle.reset(); });
	second.run([&] { second.idle.reset(); });
	drain();
	const timespec forever = {1'000'000'000, 0};
	first.run([&] {
		first.idle.emplace(&forever, CarrierIdle::Limit::relative);
	});
	second.run([&] { second.idle.emplace(); });
	require(wait_for(200).bits == 0, "a wait of years armed the watch");
}

/* A carrier that is still blocked after its deadline and the 50 ms: the push
 * fires, the watch looks, finds it in its wait, and settles to sleep. The
 * deadline it has looked past is spent, or the settle would put the timer in
 * the past and the watch would wake and look again at once, for as long as
 * the wait lasts. */
void the_watch_spends_a_deadline_it_has_looked_past()
{
	const Case scope{"overdue"};
	Actor actor;
	actor.run([&] { carrier_named(0); });
	drain();
	const timespec hundred = relative_ms(100);
	Clock::time_point began = Clock::now();
	actor.run([&] { actor.idle.emplace(&hundred, CarrierIdle::Limit::relative); });

	/* A settle before the deadline and the slack leaves the push alone. */
	watch_settle();
	Waited waited = wait_for(3000, began);
	require(waited.bits == timer, "the push of a timed wait was lost");
	require(waited.ms >= 140, "the push went off before the deadline and 50 ms: " +
		std::to_string(waited.ms) + " ms");

	/* The carrier is still blocked; the watch looks and settles. */
	watch_settle();
	require(carriers_outside_idle() == 0);
	waited = wait_for(600);
	require(waited.bits == 0,
		"a deadline the watch had looked past woke it again after " +
		std::to_string(waited.ms) + " ms");

	/* A wait the carrier enters afterwards has a deadline of its own. */
	actor.run([&] { actor.idle.reset(); });
	drain();
	const timespec two_hundred = relative_ms(200);
	began = Clock::now();
	actor.run([&] { actor.idle.emplace(&two_hundred, CarrierIdle::Limit::relative); });
	waited = wait_for(3000, began);
	require(waited.bits == timer && waited.ms >= 240,
		"the deadline of a later wait was lost: " + std::to_string(waited.bits) +
		" after " + std::to_string(waited.ms) + " ms");
}

/* The wait that the scope wraps reports its failure in errno. */
void leaving_an_idle_wait_keeps_errno()
{
	const Case scope{"errno"};
	Actor actor;
	actor.run([&] { carrier_named(2); });
	const timespec limit = relative_ms(5000);
	int kept = 0;
	actor.run([&] {
		actor.idle.emplace(&limit, CarrierIdle::Limit::relative);
		errno = EINTR;
		actor.idle.reset();
		kept = errno;
	});
	require(kept == EINTR, "leaving an idle wait changed errno");
}

void a_host_wake_arms_the_watch_and_no_carrier_takes_it_back()
{
	const Case scope{"host wake"};
	/* With no carrier at all. */
	Clock::time_point began = Clock::now();
	note_host_wake();
	Waited waited = wait_for(3000, began);
	require(waited.bits == timer && waited.ms >= 40, "a host wake did not arm the watch");

	/* A carrier that is woken and idles again before the 50 ms is over takes
	 * its own timer with it, and not this one. */
	Actor actor;
	actor.run([&] { carrier_named(2); actor.idle.emplace(); });
	drain();
	began = Clock::now();
	note_host_wake();
	actor.run([&] { actor.idle.reset(); });
	actor.run([&] { actor.idle.emplace(); });
	waited = wait_for(3000, began);
	require(waited.bits == timer && waited.ms >= 40,
		"a carrier took back the arming of a host wake");
}

void a_carrier_that_ends_reports_no_more()
{
	const Case scope{"ended carrier"};
	std::thread ended{[] { carrier_named(5); }};
	ended.join();
	require(carriers_outside_idle() == 0 && carriers_reporting() == 0,
		"an ended carrier is still counted");
	drain();
	std::thread other{[] { carrier_named(6); }};
	other.join();
	require(wait_for(150).bits == 0, "an ended carrier left the watch armed");

	/* One that ends while the others are idle does not hold the watch awake
	 * either: it was the last out. */
	Actor idle;
	idle.run([&] { carrier_named(7); idle.idle.emplace(); });
	drain();
	std::thread passing{[] {
		carrier_named(8);
		const CarrierIdle brief;
	}};
	passing.join();
	require(carriers_outside_idle() == 0 && carriers_reporting() == 0x80);
	require(wait_for(150).bits == 0);
}

void idle_scopes_inside_each_other_count_once()
{
	const Case scope{"nesting"};
	Actor actor;
	actor.run([&] { carrier_named(0); });
	require(carriers_outside_idle() == 1);
	actor.run([&] { actor.idle.emplace(); actor.inner.emplace(); });
	require(carriers_outside_idle() == 0);
	actor.run([&] { actor.inner.reset(); });
	require(carriers_outside_idle() == 0, "the inner wait ended the outer one");
	actor.run([&] { actor.idle.reset(); });
	require(carriers_outside_idle() == 1);
}

void the_watch_hears_a_poke_and_a_stop()
{
	const Case scope{"pokes"};
	watch_poke();
	Waited waited = wait_for(3000);
	require(waited.bits == watch_woken_by_poke);
	require(wait_for(60).bits == 0, "a poke is heard twice");
	watch_stop();
	note_host_wake();
	waited = wait_for(3000);
	require((waited.bits & watch_woken_by_stop) != 0);
	/* What the caller of one watch left is cleared for the next. */
	watch_queue_close();
	watch_poke();
	watch_stop();
	require(watch_queue_open());
	require(wait_for(80).bits == 0, "an earlier watch left a wake behind");
}

/* ---- The carriers against each other. ----
 *
 * Starting from a quiet queue with every carrier idle (so the timer is
 * removed), four carriers do a short burst of leaving and entering idle waits,
 * with timed waits among them, and end each in a state the test chose. If one
 * is outside an idle wait the watch must be woken, whatever the order the
 * calls of the burst interleaved in; if all are inside it must not be.
 */
class Burst final {
public:
	static constexpr unsigned carriers = 4;

	struct Plan final {
		std::uint32_t seed{};
		/* Toggles before the end, none for a carrier that sits the burst out. */
		bool toggles{};
		bool ends_outside{};
		/* An idle wait that ends in the far future, not an untimed one. */
		bool ends_far{};
	};

	Burst()
	{
		for (unsigned index = 0; index < carriers; ++index)
			threads_.emplace_back([this, index] { carrier(index); });
		require(await_generation(0), "the carriers did not start");
	}

	~Burst()
	{
		finish_.store(true, std::memory_order_release);
		for (std::thread &thread : threads_)
			thread.join();
	}

	/* Hands every carrier its plan and lets them go; true when all reach the
	 * end of it. */
	bool run(const Plan (&plans)[carriers])
	{
		for (unsigned index = 0; index < carriers; ++index)
			plans_[index] = plans[index];
		++generation_;
		go_.store(generation_, std::memory_order_release);
		return await_generation(generation_);
	}

private:
	bool await_generation(std::uint64_t generation)
	{
		for (int polls = 0; polls < 30000; ++polls) {
			bool all = true;
			for (unsigned index = 0; index < carriers; ++index)
				all = all && reached_[index].load(std::memory_order_acquire) >=
					generation;
			if (all)
				return true;
			nap(100);
		}
		return false;
	}

	static void spin_us(unsigned microseconds)
	{
		const auto until = Clock::now() + std::chrono::microseconds(microseconds);
		while (Clock::now() < until) {
		}
	}

	static void enter(std::optional<CarrierIdle> &wait, int kind)
	{
		if (kind == 0) {
			wait.emplace();
		} else {
			const timespec limit = relative_ms(kind == 1 ? 2 : 10'000);
			wait.emplace(&limit, CarrierIdle::Limit::relative);
		}
	}

	void carrier(unsigned index)
	{
		carrier_named(index);
		std::optional<CarrierIdle> wait;
		/* Every carrier starts inside an idle wait. */
		enter(wait, 0);
		reached_[index].store(0, std::memory_order_release);
		std::uint64_t seen = 0;
		for (;;) {
			while (go_.load(std::memory_order_acquire) == seen) {
				if (finish_.load(std::memory_order_acquire))
					return;
				nap(20);
			}
			seen = go_.load(std::memory_order_acquire);
			const Plan plan = plans_[index];
			std::mt19937 random{plan.seed};
			if (plan.toggles) {
				const unsigned steps = 4 + random() % 5;
				for (unsigned step = 0; step < steps; ++step) {
					if (wait.has_value())
						wait.reset();
					else
						enter(wait, static_cast<int>(random() % 3));
					spin_us(random() % 40);
				}
			}
			/* A wait with a short limit that the carrier does not return from
			 * is a carrier overdue, which the watch is rightly woken for: the
			 * burst ends in a wait with no limit, or one far off. */
			wait.reset();
			if (!plan.ends_outside)
				enter(wait, plan.ends_far ? 2 : 0);
			reached_[index].store(seen, std::memory_order_release);
		}
	}

	std::vector<std::thread> threads_;
	std::uint64_t generation_{0};
	std::atomic<std::uint64_t> go_{0};
	std::atomic<bool> finish_{false};
	std::atomic<std::uint64_t> reached_[carriers]{};
	Plan plans_[carriers]{};
};

void carriers_racing_never_lose_an_arming_nor_leave_one_behind()
{
	const Case scope{"burst"};
	Burst burst;
	std::mt19937 random{0x5eed};
	unsigned armed = 0;
	unsigned quiet = 0;
	for (int round = 0; round < 60; ++round) {
		/* Every carrier back inside its idle wait: no timer. */
		Burst::Plan settled[Burst::carriers]{};
		for (Burst::Plan &plan : settled)
			plan.ends_far = random() % 2 == 0;
		require(burst.run(settled), "the carriers did not settle");
		require(carriers_outside_idle() == 0);
		drain();

		Burst::Plan plans[Burst::carriers]{};
		unsigned outside = 0;
		for (Burst::Plan &plan : plans) {
			plan.seed = random();
			plan.toggles = random() % 4 != 0;
			plan.ends_outside = random() % 2 == 0;
			plan.ends_far = random() % 2 == 0;
			outside += plan.ends_outside ? 1 : 0;
		}
		require(burst.run(plans), "a carrier did not finish its burst");
		if (outside != 0) {
			require(carriers_outside_idle() != 0);
			const Waited waited = wait_for(3000);
			require((waited.bits & timer) != 0,
				"a carrier outside its idle wait left the watch unarmed (round " +
				std::to_string(round) + ")");
			++armed;
		} else {
			require(carriers_outside_idle() == 0);
			const Waited waited = wait_for(150);
			require((waited.bits & timer) == 0,
				"every carrier idle and the watch armed (round " +
				std::to_string(round) + ")");
			++quiet;
		}
	}
	require(armed > 10 && quiet > 3, "the bursts did not cover both ends");
}

/* The handoff the timer needs to get right: the last carrier to enter its idle
 * wait removes the timer, a carrier that leaves its wait at that moment arms
 * it, and the two system calls reach the queue in either order. One carrier
 * enters and leaves idle waits as fast as it can, some of them with a far
 * limit, which are pushed and not removed; another leaves its wait at a random
 * cycle of that and stays out. However the calls fell, the watch must be woken
 * for it. */
void a_carrier_leaving_as_the_last_one_enters_is_not_lost()
{
	const Case scope{"handoff"};
	std::mt19937 random{0xa11ce};
	for (int trial = 0; trial < 150; ++trial) {
		std::atomic<int> ready{0};
		std::atomic<bool> go{false};
		std::atomic<bool> release{false};
		std::atomic<unsigned> cycles{0};
		std::atomic<int> done{0};
		const unsigned leave_at = 20 + random() % 3000;
		const timespec far = relative_ms(10'000);
		std::thread toggler{[&] {
			carrier_named(0);
			std::optional<CarrierIdle> wait;
			wait.emplace();
			ready.fetch_add(1);
			while (!go.load(std::memory_order_acquire)) {
			}
			for (unsigned cycle = 0; cycle < 6000; ++cycle) {
				wait.reset();
				if ((cycle & 3) == 0)
					wait.emplace(&far, CarrierIdle::Limit::relative);
				else
					wait.emplace();
				cycles.store(cycle + 1, std::memory_order_release);
			}
			wait.reset();
			wait.emplace();
			done.fetch_add(1);
			while (!release.load(std::memory_order_acquire))
				nap(200);
		}};
		std::thread leaver{[&] {
			carrier_named(1);
			std::optional<CarrierIdle> wait;
			wait.emplace();
			ready.fetch_add(1);
			while (!go.load(std::memory_order_acquire)) {
			}
			while (cycles.load(std::memory_order_acquire) < leave_at) {
			}
			wait.reset();
			done.fetch_add(1);
			while (!release.load(std::memory_order_acquire))
				nap(200);
		}};
		while (ready.load() != 2)
			nap(100);
		/* Both start inside an idle wait, whatever armed the watch on the way. */
		nap(2000);
		drain();
		go.store(true, std::memory_order_release);
		while (done.load() != 2)
			nap(100);
		const std::uint32_t mask = carriers_outside_idle();
		const Waited waited = wait_for(3000);
		release.store(true, std::memory_order_release);
		toggler.join();
		leaver.join();
		require(mask == 2, "the carriers ended in other states than the test set");
		require((waited.bits & timer) != 0,
			"a carrier that left as the last one entered was lost (trial " +
			std::to_string(trial) + ")");
		drain();
	}
}

} // namespace

int main()
{
	threads_that_are_not_carriers_change_nothing();
	a_carrier_arms_the_watch_when_it_starts_and_when_it_leaves_idle();
	a_carrier_that_blinks_never_wakes_the_watch();
	the_first_carrier_out_arms_and_the_last_one_in_removes();
	the_last_carrier_in_pushes_the_timer_past_the_earliest_deadline();
	the_watch_spends_a_deadline_it_has_looked_past();
	leaving_an_idle_wait_keeps_errno();
	a_host_wake_arms_the_watch_and_no_carrier_takes_it_back();
	a_carrier_that_ends_reports_no_more();
	idle_scopes_inside_each_other_count_once();
	the_watch_hears_a_poke_and_a_stop();
	carriers_racing_never_lose_an_arming_nor_leave_one_behind();
	a_carrier_leaving_as_the_last_one_enters_is_not_lost();
	std::puts("carrier activity tests passed");
	return 0;
}
