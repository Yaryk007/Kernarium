// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a code synchronization publishes after the first time, and to how
 * many. A holder registers in a frame's word before it reads the frame, and a
 * publication takes the word, so a process that runs the new code must
 * register again, from the TLB entry it keeps from one run to the next, or its
 * next change is lost. A frame that more generations hold than a word names is
 * held by "every generation".
 *
 *   repeat    Process A rewrites and synchronizes the page again and again,
 *             and process B, which runs it from one vCPU in between, runs each
 *             version.
 *   overflow  Three processes run the page and A rewrites it, twice: all three
 *             run each version, which a word of two generations cannot name.
 *   own-flush Process B runs the page and holds it through a writable view as
 *             well, and so does process M. B synchronizes that view without
 *             a store (a flush of code it did not change), which takes the
 *             word; then A rewrites and synchronizes the page. B, which never
 *             fetched the page again, runs the new version.
 *
 * Both engines, with the writer on the reader's vCPU or on a second one.
 */
#include "arm64_cross_generation_test_support.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

namespace {

using namespace uml::testing::cross;
using uml::testing::require;

int failures;

void expect(std::uint64_t got, std::uint64_t wanted, const char *engine,
	const char *what, bool second_vcpu, std::uint32_t round)
{
	if (got == wanted)
		return;
	++failures;
	std::printf("  %s %s %s round %u: ran %llu, wanted %llu (STALE)\n",
		engine, what, second_vcpu ? "two-vcpus" : "one-vcpu", round,
		static_cast<unsigned long long>(got),
		static_cast<unsigned long long>(wanted));
}

constexpr std::uint32_t rounds = 5;

void repeat(uml::cpu::EngineKind kind, bool second_vcpu, unsigned long &next)
{
	const auto b = next++;
	const auto a = next++;
	require(darwin_uml_static_vm_activate(b) == 0);
	require(darwin_uml_static_vm_activate(a) == 0);
	{
		uml::testing::ScratchBacking shared{"uml-repeat-code.XXXXXX", page};
		uml::testing::ScratchBacking writer{"uml-repeat-writer.XXXXXX", page};
		const std::uint32_t old_words[] = {movz_x0(1), svc0};
		shared.put(0, std::as_bytes(std::span{old_words}));
		writer.put(0, std::as_bytes(std::span{writer_program}));
		Vcpu reader{kind};
		const Map b_code{b, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		const Map a_view{a, shared_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map a_writer{a, writer_base, writer.descriptor(),
			PROT_READ | PROT_EXEC, true};
		require(reader.call(b, shared_base) == 1);
		require(reader.call(b, shared_base) == 1);
		for (std::uint32_t round = 2; round < 2 + rounds; ++round) {
			on_vcpu(kind, *reader.engine, second_vcpu,
				[&](uml::cpu::CpuEngine &engine) {
					write_and_synchronize(engine, a, shared_base,
						movz_x0(round));
				});
			expect(reader.call(b, shared_base), round, engine_name(kind),
				"repeat", second_vcpu, round);
		}
	}
	require(darwin_uml_static_vm_destroy(a) == 0);
	require(darwin_uml_static_vm_destroy(b) == 0);
}

void overflow(uml::cpu::EngineKind kind, bool second_vcpu, unsigned long &next)
{
	constexpr std::size_t readers = 3;
	std::array<unsigned long, readers> reader_generation{};
	for (auto &generation : reader_generation) {
		generation = next++;
		require(darwin_uml_static_vm_activate(generation) == 0);
	}
	const auto a = next++;
	require(darwin_uml_static_vm_activate(a) == 0);
	{
		uml::testing::ScratchBacking shared{"uml-overflow-code.XXXXXX", page};
		uml::testing::ScratchBacking writer{"uml-overflow-writer.XXXXXX",
			page};
		const std::uint32_t old_words[] = {movz_x0(1), svc0};
		shared.put(0, std::as_bytes(std::span{old_words}));
		writer.put(0, std::as_bytes(std::span{writer_program}));
		std::array<std::unique_ptr<Vcpu>, readers> vcpu;
		std::array<std::unique_ptr<Map>, readers> code;
		for (std::size_t index = 0; index < readers; ++index) {
			vcpu[index] = std::make_unique<Vcpu>(kind);
			code[index] = std::make_unique<Map>(reader_generation[index],
				shared_base, shared.descriptor(), PROT_READ | PROT_EXEC,
				true);
			require(vcpu[index]->call(reader_generation[index],
				shared_base) == 1);
		}
		const Map a_view{a, shared_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map a_writer{a, writer_base, writer.descriptor(),
			PROT_READ | PROT_EXEC, true};
		for (std::uint32_t round = 2; round < 4; ++round) {
			on_vcpu(kind, *vcpu[0]->engine, second_vcpu,
				[&](uml::cpu::CpuEngine &engine) {
					write_and_synchronize(engine, a, shared_base,
						movz_x0(round));
				});
			for (std::size_t index = 0; index < readers; ++index)
				expect(vcpu[index]->call(reader_generation[index],
					       shared_base),
					round, engine_name(kind), "overflow", second_vcpu,
					round);
		}
	}
	require(darwin_uml_static_vm_destroy(a) == 0);
	for (const auto generation : reader_generation)
		require(darwin_uml_static_vm_destroy(generation) == 0);
}

/* ic ivau, x3; dsb ish; isb; svc: a flush of x3's view, no store. */
inline const std::vector<std::uint32_t> flush_program{
	0xd50b'7523U, 0xd503'3b9fU, 0xd503'3fdfU, svc0,
};

constexpr std::uint64_t flush_base = 0x5500'0000U;

void own_flush(uml::cpu::EngineKind kind, bool second_vcpu, unsigned long &next)
{
	const auto m = next++;
	const auto b = next++;
	const auto a = next++;
	for (const auto generation : {m, b, a})
		require(darwin_uml_static_vm_activate(generation) == 0);
	{
		uml::testing::ScratchBacking shared{"uml-ownflush-code.XXXXXX", page};
		uml::testing::ScratchBacking writer{"uml-ownflush-writer.XXXXXX",
			page};
		uml::testing::ScratchBacking flush{"uml-ownflush-flush.XXXXXX", page};
		const std::uint32_t old_words[] = {movz_x0(1), svc0};
		shared.put(0, std::as_bytes(std::span{old_words}));
		writer.put(0, std::as_bytes(std::span{writer_program}));
		flush.put(0, std::as_bytes(std::span{flush_program}));
		Vcpu holder_m{kind};
		Vcpu holder_b{kind};
		const Map m_code{m, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		const Map b_code{b, shared_base, shared.descriptor(),
			PROT_READ | PROT_EXEC, true};
		const Map b_view{b, alias_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map b_flush{b, flush_base, flush.descriptor(),
			PROT_READ | PROT_EXEC, true};
		const Map a_view{a, shared_base, shared.descriptor(),
			PROT_READ | PROT_WRITE, false};
		const Map a_writer{a, writer_base, writer.descriptor(),
			PROT_READ | PROT_EXEC, true};
		require(holder_m.call(m, shared_base) == 1);
		require(holder_b.call(b, shared_base) == 1);
		require(holder_b.call(b, shared_base) == 1);
		/* B flushes its writable view of a page it did not change. */
		arm64::CpuState cpu{};
		cpu.pc = flush_base;
		cpu.registers[3] = alias_base;
		const auto flushed = run(*holder_b.engine, b, cpu);
		require(flushed.reason == uml::cpu::ExitReason::system_call);
		on_vcpu(kind, *holder_b.engine, second_vcpu,
			[&](uml::cpu::CpuEngine &engine) {
				write_and_synchronize(engine, a, shared_base, movz_x0(2));
			});
		expect(holder_b.call(b, shared_base), 2, engine_name(kind),
			"own-flush", second_vcpu, 2);
		expect(holder_m.call(m, shared_base), 2, engine_name(kind),
			"own-flush-other", second_vcpu, 2);
	}
	for (const auto generation : {a, b, m})
		require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	unsigned long generation = 0x6e00;
	for_each_engine(argc > 1 ? argv[1] : "", [&](uml::cpu::EngineKind kind) {
		for (const bool second_vcpu : {false, true}) {
			repeat(kind, second_vcpu, generation);
			overflow(kind, second_vcpu, generation);
			own_flush(kind, second_vcpu, generation);
		}
	});
	std::printf("arm64-cross-generation-publication: %d stale\n", failures);
	return failures == 0 ? 0 : 1;
}
