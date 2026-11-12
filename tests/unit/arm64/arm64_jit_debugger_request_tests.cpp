// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/jit_memory/executable_memory.hpp"
#include "cpu/jit_memory/executable_memory_testing.hpp"
#include "require.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

/* A request to the debugger when none serves it. On an iOS device the runtime
 * executes `brk #0xf00d` and waits for the debugger to resume it; when the
 * debugger is gone, or goes while the request waits, the system turns the
 * breakpoint into SIGTRAP, which ends the process. This process has no
 * debugger, so every real request here is one nothing serves: the guard must
 * step over it, say it went unanswered and leave the process running, read
 * P_TRACED (the probe) right before it, and hand every other SIGTRAP on. */
namespace {

namespace jit_memory = ::uml::cpu::jit_memory;
namespace hooks = ::uml::cpu::jit_memory::testing;
using uml::testing::require;

constexpr int answered = 0;
constexpr int no_debugger = 1;
constexpr int unanswered = 3;
constexpr std::size_t kib = 1024U;

/* P_TRACED as the test states it: attached, absent, or attached for a number
 * of reads and gone after them. */
std::atomic<int> reads_left{0};
std::atomic<int> reads{0};

bool attached() noexcept
{
	reads.fetch_add(1);
	return true;
}

bool absent() noexcept
{
	reads.fetch_add(1);
	return false;
}

/* The debugger is there until the request has been sent, then gone. */
bool leaves_while_the_request_waits() noexcept
{
	reads.fetch_add(1);
	return reads_left.fetch_sub(1) > 0;
}

std::atomic<int> foreign_traps{0};

void earlier_handler(int)
{
	foreign_traps.fetch_add(1);
}

void a_request_to_a_process_no_debugger_holds_is_not_sent()
{
	/* The variant's default: a request is answered at once. */
	require(hooks::request(0) == answered, "the default request was not answered");
	hooks::send_real_requests(true);
	hooks::set_traced_probe(absent);
	reads = 0;
	require(hooks::request(0) == no_debugger,
		"a request went out with no debugger attached");
	require(hooks::unanswered_requests_seen() == 0,
		"a breakpoint ran with no debugger attached");
	require(reads.load() >= 1, "P_TRACED was not read before the request");
}

void a_request_nothing_serves_is_stepped_over_and_the_process_lives()
{
	/* A disposition in place before the first request, so that a trap that
	 * is not a request can be seen to go to it. */
	struct sigaction earlier {};
	earlier.sa_handler = earlier_handler;
	sigemptyset(&earlier.sa_mask);
	require(sigaction(SIGTRAP, &earlier, nullptr) == 0, "no earlier handler");

	hooks::set_traced_probe(attached);
	require(hooks::request(1) == unanswered,
		"a breakpoint nothing resumed was not reported unanswered");
	require(hooks::unanswered_requests_seen() == 1,
		"the breakpoint was not stepped over exactly once");
	require(hooks::request(0) == unanswered,
		"a second request was not stepped over");
	require(hooks::unanswered_requests_seen() == 2,
		"the second breakpoint was not counted");

	/* Not a request: the earlier disposition gets it. */
	require(foreign_traps.load() == 0, "a request reached the earlier handler");
	std::raise(SIGTRAP);
	require(foreign_traps.load() == 1,
		"a SIGTRAP that is no request did not reach the earlier handler");
	require(hooks::unanswered_requests_seen() == 2,
		"a foreign SIGTRAP was counted as a request");
}

void a_debugger_that_left_while_the_request_waited_is_no_debugger()
{
	const auto before = hooks::unanswered_requests_seen();
	/* Two reads see it (the caller's, the sender's), the third does not. */
	reads_left = 2;
	hooks::set_traced_probe(leaves_while_the_request_waits);
	require(hooks::request(0) == no_debugger,
		"a debugger that left during the request was still reported there");
	require(hooks::unanswered_requests_seen() == before + 1,
		"the breakpoint was not stepped over");
}

void a_region_is_given_back_when_the_debugger_left_before_it_served()
{
	/* The region's own look, the caller's and the sender's see it; the read
	 * after the breakpoint does not. */
	reads_left = 3;
	hooks::set_traced_probe(leaves_while_the_request_waits);
	require(jit_memory::prepare_debugger_region(100 * kib) ==
			jit_memory::RegionStatus::no_debugger,
		"a vanished debugger did not leave the no-debugger status");
	require(!jit_memory::available() &&
			jit_memory::prepared_region_bytes() == 0,
		"a region the debugger never prepared was kept");

	/* The process asked nothing of a debugger that was there, so it can ask
	 * again; a debugger that answers (the test variant's default) prepares. */
	hooks::send_real_requests(false);
	hooks::set_traced_probe(nullptr);
	require(jit_memory::prepare_debugger_region(100 * kib) ==
			jit_memory::RegionStatus::ready,
		"a later preparation with a debugger did not succeed");
	require(jit_memory::available(), "the later region is not ready");
}

} // namespace

int main()
{
	try {
		a_request_to_a_process_no_debugger_holds_is_not_sent();
		a_request_nothing_serves_is_stepped_over_and_the_process_lives();
		a_debugger_that_left_while_the_request_waited_is_no_debugger();
		a_region_is_given_back_when_the_debugger_left_before_it_served();
		std::puts("JIT debugger request tests passed");
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		return std::fprintf(stderr, "JIT debugger request test failed: %s\n",
			error.what()), EXIT_FAILURE;
	}
}
