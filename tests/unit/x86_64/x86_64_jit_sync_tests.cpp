// SPDX-License-Identifier: GPL-2.0-or-later
/* What a vCPU hands the x86-64 JIT before each run, driven with a stub engine:
 * the call that settles the participants a registration waited out comes
 * first, before either of the two returns that skip the hand-over, because
 * it concerns code that no address-space change describes. */
#include "cpu/x86_64/jit/fex_engine.hpp"
#include "require.hpp"
#include "runtime/x86_jit_sync.hpp"
#include "uml_darwin_static_mmu.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using uml::cpu::x86_64::ExecutionMode;
using uml::testing::require;

/* The calls the engine receives, in order. */
struct StubEngine final {
	using Changes = uml::cpu::x86_64::jit::AddressSpaceChanges;
	using Range = uml::cpu::x86_64::jit::GuestRange;
	std::string calls;
	bool consumed{};
	std::uint64_t mapping_seen{};
	std::uint64_t code_seen{};

	void settle_stuck_participants(unsigned long, ExecutionMode) noexcept
	{
		calls += 's';
	}
	[[nodiscard]] bool consumed_sequences(unsigned long, ExecutionMode,
		std::uint64_t &mapping, std::uint64_t &code) const noexcept
	{
		mapping = mapping_seen;
		code = code_seen;
		return consumed;
	}
	void apply_changes(unsigned long, ExecutionMode,
		const uml::cpu::x86_64::jit::AddressSpaceChanges &, bool) noexcept
	{
		calls += 'a';
	}
};

struct StubScope final {
	[[nodiscard]] void *context() const noexcept { return nullptr; }
};

darwin_uml_static_sequences sequences(std::uint64_t mapping, std::uint64_t code)
{
	darwin_uml_static_sequences result{};
	result.mapping_changes = mapping;
	result.code_changes = code;
	return result;
}

void test_the_stuck_participants_are_settled_before_every_return()
{
	const StubScope scope;
	/* The engine holds nothing for the generation yet: it is handed the
	 * sequences, and the participants come first. */
	StubEngine fresh;
	uml::runtime::sync_x86_jit_engine(fresh, scope, 1,
		ExecutionMode::long64, sequences(5, 7), true);
	require(fresh.calls == "sa",
		"a first hand-over did not settle the participants first");

	/* Nothing changed since the engine last ran: no hand-over, and the
	 * participants are still settled. */
	StubEngine current;
	current.consumed = true;
	current.mapping_seen = 5;
	current.code_seen = 7;
	uml::runtime::sync_x86_jit_engine(current, scope, 1,
		ExecutionMode::long64, sequences(5, 7), true);
	require(current.calls == "s",
		"an up-to-date engine did not settle its participants, or was handed "
		"changes");
}

} // namespace

int main()
{
	test_the_stuck_participants_are_settled_before_every_return();
	std::puts("x86-64 JIT sync tests passed");
	return EXIT_SUCCESS;
}
