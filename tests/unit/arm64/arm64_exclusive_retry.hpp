// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*
 * A store-exclusive may fail without cause, as the architecture allows. The
 * gadget engine runs the host's own exclusives across two gadgets, and the
 * JIT fuses a pair into adjacent host instructions; the host's monitor ends
 * at any interrupt or preemption between the load and the store, so under
 * load one lands in that window now and then and the store writes nothing. A
 * guest loops on the status. A test that runs a pair once and requires the
 * store has to do the same: it runs the pair again while the outcome is
 * exactly a store that lost its reservation, at most exclusive_attempts times
 * in all, and judges the last run, so that a store that never writes, writes
 * other bytes or writes elsewhere still fails.
 */
#include <functional>
#include <type_traits>
#include <utility>

namespace uml::emulator::tests {

/* How many times a pair whose store lost its reservation may run. A loss is
 * an interrupt that landed in a window of tens of nanoseconds, so a few
 * attempts are enough; an engine whose store never succeeds fails every one. */
inline constexpr unsigned exclusive_attempts = 16;

/* The last of the runs and how many there were. */
template <class Result> struct ExclusiveRun final {
	Result result;
	unsigned attempts;
	/* The last run was still a lost reservation. */
	bool lost;
};

/* Runs `run` until `lost(result)` is false, at most exclusive_attempts times. */
template <class Run, class Lost>
[[nodiscard]] auto run_exclusive(Run &&run, Lost &&lost)
{
	using Result = std::invoke_result_t<Run &>;
	auto result = std::invoke(run);
	unsigned attempts = 1;
	while (std::invoke(lost, result) && attempts < exclusive_attempts) {
		result = std::invoke(run);
		++attempts;
	}
	const bool still_lost = std::invoke(lost, result);
	return ExclusiveRun<Result>{std::move(result), attempts, still_lost};
}

} // namespace uml::emulator::tests
