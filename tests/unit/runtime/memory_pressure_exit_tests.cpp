// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The memory pressure response at process exit. A guest still owns its watch
 * when the process exits, so the pressure queue can deliver an event after
 * exit() has run the destructors of ordinary statics: the response's state must
 * still be alive then. The binary substitutes the memory pressure source for one
 * it raises (as memory_pressure_response_tests does), and raises the level from
 * a handler memory_pressure_exit_first.cpp registers before the response's
 * statics exist, so the handler runs after their destructors. The handler
 * records the delivery and the response's own queue handles it, so the binary
 * waits for both queues.
 *
 * The exit status says what happened: 0 when the report was delivered after
 * static destruction, 2 when the handler never ran, 3 when no report arrived,
 * and an abort when the response met a destroyed mutex.
 */
#include "memory_pressure_response.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dispatch/dispatch.h>
#include <dlfcn.h>

namespace {

std::atomic<dispatch_source_t> pressure_source{nullptr};
std::atomic<dispatch_queue_t> pressure_queue{nullptr};
std::atomic<dispatch_queue_t> policy_queue{nullptr};
std::atomic<int> reports_heard{0};

} // namespace

namespace uml::testing {

void afterStaticDestruction()
{
	std::fputs("raising a level after static destruction\n", stdout);
	std::fflush(stdout);
	dispatch_source_merge_data(pressure_source.load(), DISPATCH_MEMORYPRESSURE_NORMAL);
	/* Two calls on each serial queue: the second returns after the handler the
	 * merge started, whichever of them libdispatch ran it behind. The source's
	 * handler records the delivery and signals the policy queue, which makes
	 * the report. */
	for (const auto queue : {pressure_queue.load(), policy_queue.load()}) {
		dispatch_sync_f(queue, nullptr, [](void *) {});
		dispatch_sync_f(queue, nullptr, [](void *) {});
	}
	const int heard = reports_heard.load();
	std::printf("reports heard after static destruction: %d\n", heard);
	std::fflush(stdout);
	std::_Exit(heard == 1 ? 0 : 3);
}

} // namespace uml::testing

extern "C" dispatch_source_t dispatch_source_create(
	dispatch_source_type_t type, uintptr_t handle, uintptr_t mask,
	dispatch_queue_t queue)
{
	using Create = dispatch_source_t (*)(
		dispatch_source_type_t, uintptr_t, uintptr_t, dispatch_queue_t);
	static const auto real = reinterpret_cast<Create>(
		dlsym(RTLD_NEXT, "dispatch_source_create"));
	/* The response's own signal to its policy queue is the one data source
	 * made through this call. */
	if (type == DISPATCH_SOURCE_TYPE_DATA_OR)
		policy_queue.store(queue);
	if (type != DISPATCH_SOURCE_TYPE_MEMORYPRESSURE)
		return real(type, handle, mask, queue);
	const dispatch_source_t source =
		real(DISPATCH_SOURCE_TYPE_DATA_OR, 0, 0, queue);
	pressure_source.store(source);
	pressure_queue.store(queue);
	return source;
}

int main()
{
	uml::runtime::set_pressure_observer(
		[](void *, const uml::runtime::PressureReport &) noexcept {
			reports_heard.fetch_add(1);
		}, nullptr);
	auto watch = uml::runtime::start_memory_pressure_response(nullptr, nullptr);
	if (watch == nullptr) {
		std::fputs("no watch was made\n", stderr);
		return 1;
	}
	/* The guest still runs: nothing stops the watch before exit(). */
	(void)watch.release();
	std::exit(2);
}
