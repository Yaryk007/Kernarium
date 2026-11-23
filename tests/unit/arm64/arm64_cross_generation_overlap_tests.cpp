// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A completed code publication must reach every process that translated the
 * frame, whichever publication was in progress when it began. A publisher
 * takes a frame's holder word, which names the processes that translated it,
 * and publishes to them afterwards; a second publisher that starts in between
 * finds the word empty, and must publish to every process itself.
 *
 * Reader B translated a page through an executable view and also maps it
 * read-write. The first publisher A takes B out of the word and is held before
 * it publishes. Then a second publisher changes the page again and returns,
 * and B, which has not fetched the page again, must run the second change at
 * once, while A is still held:
 *
 *   guest/guest    A and C each write and synchronize their own view.
 *   guest/kernel   A synchronizes; the kernel then writes the frame through
 *                  B's read-write view (process_vm_writev() or a ptrace write
 *                  into B, or an AArch32 cacheflush() of it) and calls
 *                  darwin_uml_vm_invalidate_code_frame (naming the frame) or,
 *                  for the cacheflush(), darwin_uml_vm_invalidate_code for B.
 *   kernel/kernel  The same, with A's own change a kernel write too.
 *   kernel/guest   A's change is a kernel write, C's a synchronization.
 *
 * Each also runs with a late holder L, a process that translates the page
 * after A's take and before the second change: B and L must both run it.
 * Every kernel write runs by address and by frame. Both engines.
 */
#include "arm64_cross_generation_test_support.hpp"

#include "allocation_fault.hpp"
#include "cpu/arm64/code_synchronization.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>

namespace {

using namespace uml::testing::cross;
using uml::testing::require;
namespace memory_testing = ::uml::memory::testing;

std::atomic<bool> paused;
std::atomic<bool> released;
thread_local bool holds_at_publication;

/* Holds the first publisher between its take of the holder word and its
 * publication, where it holds no lock. */
void hold_the_publisher(memory_testing::AllocationSite site) noexcept
{
	if (site != memory_testing::AllocationSite::holder_publication ||
	    !holds_at_publication)
		return;
	paused.store(true, std::memory_order_release);
	const auto limit =
		std::chrono::steady_clock::now() + std::chrono::seconds{30};
	while (!released.load(std::memory_order_acquire)) {
		if (std::chrono::steady_clock::now() > limit)
			uml::testing::fail("the held publisher was never released");
		std::this_thread::yield();
	}
}

int failures;

void expect(std::uint64_t got, std::uint64_t wanted, const char *engine,
	const char *what, const char *who)
{
	if (got == wanted)
		return;
	++failures;
	std::printf("  %s %s: %s ran %llu, wanted %llu (STALE)\n", engine, what,
		who, static_cast<unsigned long long>(got),
		static_cast<unsigned long long>(wanted));
}

/* The kernel's call for `generation` after it wrote the frame through the
 * view at alias_base: by address, or naming the frame. */
void kernel_publication(bool named, unsigned long generation,
	uml::testing::ScratchBacking &shared)
{
	if (named)
		require(darwin_uml_static_vm_invalidate_code_frame(generation,
			alias_base, 4, shared.descriptor(), 0) == 0);
	else
		require(darwin_uml_static_vm_invalidate_code_generation(generation,
			alias_base, 4) == 0);
}

void overlap(uml::cpu::EngineKind kind, bool first_kernel, bool second_kernel,
	bool late_holder, bool named, unsigned long &next)
{
	const auto b = next++;
	const auto a = next++;
	const auto c = next++;
	const auto l = next++;
	for (const auto generation : {b, a, c, l})
		require(darwin_uml_static_vm_activate(generation) == 0);
	char what[64];
	std::snprintf(what, sizeof what, "%s/%s%s%s",
		first_kernel ? "kernel" : "guest", second_kernel ? "kernel" : "guest",
		late_holder ? " late" : "", named ? " frame" : " address");
	{
		uml::testing::ScratchBacking shared{"uml-overlap-code.XXXXXX", page};
		const std::uint32_t old_words[] = {movz_x0(1), svc0};
		shared.put(0, std::as_bytes(std::span{old_words}));
		const Map b_code{b, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		const Map b_view{b, alias_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map a_view{a, alias_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map c_view{c, alias_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map l_code{l, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		Vcpu reader{kind};
		Vcpu late{kind};
		require(reader.call(b, shared_base) == 1);
		require(reader.call(b, shared_base) == 1);

		paused.store(false);
		released.store(false);
		memory_testing::set_allocation_observer(&hold_the_publisher);
		std::thread first{[&] {
			holds_at_publication = true;
			shared.put32(0, movz_x0(2));
			if (first_kernel) {
				kernel_publication(named, a, shared);
			} else {
				require(darwin_uml_static_vm_activate(a) == 0);
				uml_arm64_guest_code_synchronize(alias_base);
			}
		}};
		const auto limit =
			std::chrono::steady_clock::now() + std::chrono::seconds{30};
		while (!paused.load(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() > limit) {
				released.store(true);
				first.join();
				uml::testing::fail("the first publisher never took the word");
			}
			std::this_thread::yield();
		}
		/* A has taken B out of the word and has not published. */
		std::uint64_t late_first = 0;
		if (late_holder)
			late_first = late.call(l, shared_base);
		shared.put32(0, movz_x0(3));
		if (second_kernel) {
			kernel_publication(named, b, shared);
		} else {
			require(darwin_uml_static_vm_activate(c) == 0);
			uml_arm64_guest_code_synchronize(alias_base);
		}
		/* The second change has completed; A is still held. */
		const auto b_ran = reader.call(b, shared_base);
		const auto l_ran = late_holder ? late.call(l, shared_base) : 3;
		released.store(true, std::memory_order_release);
		first.join();
		memory_testing::set_allocation_observer(nullptr);

		if (late_holder)
			expect(late_first, 2, engine_name(kind), what, "L's first run");
		expect(b_ran, 3, engine_name(kind), what, "B");
		expect(l_ran, 3, engine_name(kind), what, "L");
		expect(reader.call(b, shared_base), 3, engine_name(kind), what,
			"B after both");
	}
	for (const auto generation : {l, c, a, b})
		require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	unsigned long generation = 0x7100;
	for_each_engine(argc > 1 ? argv[1] : "", [&](uml::cpu::EngineKind kind) {
		for (const bool late_holder : {false, true}) {
			for (const bool first_kernel : {false, true}) {
				for (const bool second_kernel : {false, true}) {
					for (const bool named : {false, true}) {
						/* No kernel write: nothing to name. */
						if (named && !first_kernel && !second_kernel)
							continue;
						overlap(kind, first_kernel, second_kernel,
							late_holder, named, generation);
					}
				}
			}
		}
	});
	std::printf("arm64-cross-generation-overlap: %d stale\n", failures);
	return failures == 0 ? 0 : 1;
}
