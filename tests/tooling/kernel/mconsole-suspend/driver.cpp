// SPDX-License-Identifier: GPL-2.0-or-later
// Plays the management console around the series' wake bookkeeping
// (test_darwin_mconsole_suspend.py builds it from the topic): requests that
// need a process queue newest first and run one at a time, a wake is answered
// in the interrupt, and a suspend claims suspend_request before it decides
// whether to enter the PM core. The handlers' own text is not compiled here;
// their order of calls is checked in the patch, and the guest scenarios run
// them for real.
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

extern "C" {
#include "kernel.h"
void mconsole_stamp_request(struct mc_request *);
void test_note_wake(void);
bool test_overtaken(const struct mc_request *);
struct mc_request **test_suspend_request(void);
bool *test_suspend_woken(void);
}

namespace {

int failures = 0;
void check(bool condition, const char *what, int line)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
		failures++;
	}
}
#define CHECK(condition) check((condition), #condition, __LINE__)

struct Console {
	std::deque<mc_request *> queue;  // list_add: the newest is first
	std::vector<std::string> log;    // what each request did, in order
	int entered = 0;                 // times the PM core was called
	int next_id = 1;
	bool woke_in_pm = false;         // the next entry is woken by a wake

	mc_request make()
	{
		mc_request request{};
		request.id = next_id++;
		return request;
	}
	// The interrupt: a suspend needs a process, so it is stamped and queued.
	void arrive(mc_request &request)
	{
		mconsole_stamp_request(&request);
		queue.push_front(&request);
	}
	// A wake is answered in the interrupt, whatever is queued or running.
	void wake()
	{
		test_note_wake();
	}
	// The work: the newest request runs first. Mirrors mconsole_suspend().
	void run(mc_request &request, void (*during_pm)(Console &) = nullptr)
	{
		for (auto it = queue.begin(); it != queue.end(); ++it)
			if (*it == &request) {
				queue.erase(it);
				break;
			}
		CHECK(*test_suspend_request() == nullptr);
		*test_suspend_request() = &request;
		std::string result = "failed -16";
		if (!test_overtaken(&request)) {
			entered++;
			if (during_pm)
				during_pm(*this);
			result = *test_suspend_woken() ? "woken" : "suspended";
		}
		*test_suspend_request() = nullptr;
		*test_suspend_woken() = false;
		log.push_back("#" + std::to_string(request.id) + " " + result);
	}
};

}  // namespace

int main()
{
	// An ordinary suspend enters the PM core, and a wake with nothing waiting
	// only counts.
	{
		Console c;
		mc_request a = c.make();
		c.arrive(a);
		c.run(a);
		CHECK(c.entered == 1 && c.log == std::vector<std::string>{"#1 suspended"});
		c.wake();
		CHECK(*test_suspend_woken() == false);
	}

	// A wake before any suspend does not cancel a suspend that comes later.
	{
		Console c;
		c.wake();
		c.wake();
		mc_request a = c.make();
		c.arrive(a);
		c.run(a);
		CHECK(c.entered == 1 && c.log == std::vector<std::string>{"#1 suspended"});
	}

	// Suspend A is running, suspend B waits behind it, then the wake: A is
	// woken in the PM core as before, and B, when its turn comes, fails
	// without entering it.
	{
		Console c;
		mc_request a = c.make();
		mc_request b = c.make();
		c.arrive(a);
		c.arrive(b);  // queued while A is the one running
		c.run(a, [](Console &console) { console.wake(); });
		CHECK(c.log.size() == 1 && c.log[0] == "#1 woken");
		c.run(b);
		CHECK(c.entered == 1);
		CHECK(c.log.size() == 2 && c.log[1] == "#2 failed -16");
		CHECK(*test_suspend_woken() == false && *test_suspend_request() == nullptr);
	}

	// The same with A stuck before the PM core (a sync that waits): the wake
	// arrives with A claimed and marks it, and B still fails.
	{
		Console c;
		mc_request a = c.make();
		mc_request b = c.make();
		c.arrive(a);
		*test_suspend_request() = &a;  // A claimed, stuck
		c.arrive(b);
		c.wake();
		CHECK(*test_suspend_woken() == true);
		*test_suspend_request() = nullptr;
		*test_suspend_woken() = false;  // A ended
		c.run(b);
		CHECK(c.entered == 0 && c.log == std::vector<std::string>{"#2 failed -16"});
	}

	// Two suspends queued behind a stuck request and a wake: whichever runs
	// first, neither enters the PM core.
	for (int newest_first = 0; newest_first < 2; newest_first++) {
		Console c;
		mc_request a = c.make();
		mc_request b = c.make();
		c.arrive(a);
		c.arrive(b);
		c.wake();
		if (newest_first) {
			CHECK(c.queue.front() == &b);
			c.run(*c.queue.front());
			c.run(*c.queue.front());
		} else {
			c.run(a);
			c.run(b);
		}
		CHECK(c.entered == 0 && c.log.size() == 2);
		CHECK(c.log[0].find("failed -16") != std::string::npos);
		CHECK(c.log[1].find("failed -16") != std::string::npos);
	}

	// A, wake, B: the suspend that arrived after the wake is the host's next
	// attempt and runs first (newest first); the wake that follows it then
	// ends A, which arrived before both.
	{
		Console c;
		mc_request a = c.make();
		mc_request b = c.make();
		c.arrive(a);
		c.wake();
		c.arrive(b);
		CHECK(c.queue.front() == &b);
		c.run(*c.queue.front());
		CHECK(c.entered == 1 && c.log[0] == "#2 suspended");
		c.wake();
		c.run(*c.queue.front());
		CHECK(c.entered == 1 && c.log[1] == "#1 failed -16");
	}

	// A wake that ended an earlier suspend does not end one asked for after.
	{
		Console c;
		mc_request a = c.make();
		c.arrive(a);
		c.run(a);
		c.wake();
		mc_request b = c.make();
		c.arrive(b);
		c.run(b);
		CHECK(c.entered == 2 && c.log[1] == "#2 suspended");
	}

	if (failures) {
		std::fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	std::puts("PASS mconsole suspend: a wake ends every suspend that arrived before it, "
		  "in either order, and none that comes after");
	return 0;
}
