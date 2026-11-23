// SPDX-License-Identifier: GPL-2.0-or-later
/* Move run heads (engine/run_heads.inc). A row of register moves, MOV Xd, Xm
 * and MOV Wd, Wm, is one group: a head that does the row as a parallel copy
 * of at most four moves and replaces the row's words. This test checks
 * where the builder puts a head, what it encodes and which form it selects;
 * that the head takes a register forwarded into the row and forwards the
 * row's last destination to the instruction behind it; that a block with a
 * head retires, faults and reaches its SVC at the instructions the ordinary
 * words would; and, over generated rows with dependent moves, swaps,
 * overwritten destinations, XZR sources and destinations, and both widths,
 * that every block ends with the registers a model of the instructions
 * gives, with forwarding on and off and with no head at all. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/scalar_forwarding.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <memory_resource>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

#define UML_DECLARE_RUN_MOVES(widths)                                      \
	extern "C" void uml_arm64_gadget_run_moves_##widths();              \
	extern "C" void uml_arm64_gadget_run_moves_##widths##_forward();
#define UML_DECLARE_RUN_MOVES_2(prefix)                                    \
	UML_DECLARE_RUN_MOVES(prefix##x) UML_DECLARE_RUN_MOVES(prefix##w)
#define UML_DECLARE_RUN_MOVES_3(prefix)                                    \
	UML_DECLARE_RUN_MOVES_2(prefix##x) UML_DECLARE_RUN_MOVES_2(prefix##w)
#define UML_DECLARE_RUN_MOVES_4(prefix)                                    \
	UML_DECLARE_RUN_MOVES_3(prefix##x) UML_DECLARE_RUN_MOVES_3(prefix##w)
UML_DECLARE_RUN_MOVES_2(x)
UML_DECLARE_RUN_MOVES_2(w)
UML_DECLARE_RUN_MOVES_3(x)
UML_DECLARE_RUN_MOVES_3(w)
UML_DECLARE_RUN_MOVES_4(x)
UML_DECLARE_RUN_MOVES_4(w)
#undef UML_DECLARE_RUN_MOVES_4
#undef UML_DECLARE_RUN_MOVES_3
#undef UML_DECLARE_RUN_MOVES_2
#undef UML_DECLARE_RUN_MOVES

namespace {

using uml::emulator::tests::address;
using uml::testing::require;

/* A head, by its name's widths, and its forwarding form. */
struct Head final {
	const char *widths;
	bool forward;
};

[[nodiscard]] const std::map<std::uintptr_t, Head> &heads()
{
	static const std::map<std::uintptr_t, Head> table = [] {
		std::map<std::uintptr_t, Head> out;
#define UML_RUN_MOVES(widths)                                              \
	out.emplace(address(uml_arm64_gadget_run_moves_##widths),           \
		Head{#widths, false});                                      \
	out.emplace(address(uml_arm64_gadget_run_moves_##widths##_forward), \
		Head{#widths, true});
#define UML_RUN_MOVES_2(prefix)                                            \
	UML_RUN_MOVES(prefix##x) UML_RUN_MOVES(prefix##w)
#define UML_RUN_MOVES_3(prefix)                                            \
	UML_RUN_MOVES_2(prefix##x) UML_RUN_MOVES_2(prefix##w)
#define UML_RUN_MOVES_4(prefix)                                            \
	UML_RUN_MOVES_3(prefix##x) UML_RUN_MOVES_3(prefix##w)
		UML_RUN_MOVES_2(x)
		UML_RUN_MOVES_2(w)
		UML_RUN_MOVES_3(x)
		UML_RUN_MOVES_3(w)
		UML_RUN_MOVES_4(x)
		UML_RUN_MOVES_4(w)
#undef UML_RUN_MOVES_4
#undef UML_RUN_MOVES_3
#undef UML_RUN_MOVES_2
#undef UML_RUN_MOVES
		return out;
	}();
	return table;
}

constexpr std::uint64_t code_pc = 0x4000;
constexpr std::uint32_t svc = 0xd400'0001U;
/* CpuState's zero slot, which a move from XZR reads. */
constexpr std::uintptr_t zero_slot = offsetof(arm64::CpuState, gadget_zero);

[[nodiscard]] constexpr std::uint32_t mov(unsigned destination, unsigned source,
	bool wide = true) noexcept
{
	return (wide ? 0xaa00'03e0U : 0x2a00'03e0U) | (source << 16) | destination;
}
/* ORR Xd, XZR, Xm, LSL #1: not a move. */
[[nodiscard]] constexpr std::uint32_t orr_shifted(unsigned destination,
	unsigned source) noexcept
{
	return mov(destination, source) | (1U << 10);
}
/* ADD Xd, Xn, #imm, neither of them SP. */
[[nodiscard]] constexpr std::uint32_t add(unsigned destination, unsigned source,
	unsigned immediate) noexcept
{
	return 0x9100'0000U | (immediate << 10) | (source << 5) | destination;
}
/* LDR Xt, [Xn]. */
[[nodiscard]] constexpr std::uint32_t ldr(unsigned destination,
	unsigned base) noexcept
{
	return 0xf940'0000U | (base << 5) | destination;
}

/* One move in a head's operand word, in the order the head does it. */
struct Encoded final {
	std::uintptr_t source;
	unsigned destination;
};

[[nodiscard]] constexpr std::uintptr_t operand(
	std::initializer_list<Encoded> moves) noexcept
{
	std::uintptr_t word = 0;
	unsigned shift = 0;
	for (const auto &move : moves) {
		word |= (move.source | (std::uintptr_t{move.destination} << 10))
			<< shift;
		shift += 15;
	}
	return word;
}

[[nodiscard]] constexpr std::uintptr_t x(unsigned reg) noexcept
{
	return reg * sizeof(std::uint64_t);
}

/* Direct targets resolve to records of their own, as the executor's do. */
const gadgets::BlockLink *unlinked(std::uint64_t pc)
{
	static std::map<std::uint64_t, uml::emulator::tests::UnlinkedLink>
		records;
	return &records.try_emplace(pc, pc).first->second.link;
}

struct Site final {
	Site()
	{
		for (auto &entry : site.entries)
			entry = {~std::uint64_t{}, &empty.link};
	}
	gadgets::IndirectLink site{};
	uml::emulator::tests::UnlinkedLink empty{~std::uint64_t{}};
};

/* A block built as the executor builds one, with indirect sites, or
 * without them, as a single step is. */
[[nodiscard]] gadgets::BuildResult build(std::span<const std::uint32_t> code,
	bool sites = true, bool forwarding = true)
{
	static Site site;
	gadgets::BuildOptions options{};
	options.scalar_forwarding = forwarding;
	if (!sites)
		return gadgets::Builder{}.build(code_pc, code, unlinked,
			std::pmr::get_default_resource(), {}, options);
	return gadgets::Builder{}.build(code_pc, code, unlinked,
		std::pmr::get_default_resource(),
		[](std::uint64_t) { return &site.site; }, options);
}

/* The heads in a program, with the word each is at. */
struct Found final {
	std::size_t at;
	Head head;
};

[[nodiscard]] std::vector<Found> heads_in(const gadgets::Program &program)
{
	std::vector<Found> out;
	const auto words = program.words();
	for (std::size_t index = 0; index < words.size(); ++index)
		if (const auto found = heads().find(words[index]);
		    found != heads().end())
			out.push_back({index, found->second});
	return out;
}

/* What the instructions do, run in order. */
[[nodiscard]] std::array<std::uint64_t, 31> model(
	std::span<const std::uint32_t> code, std::array<std::uint64_t, 31> regs)
{
	const auto value = [&](unsigned reg) {
		return reg == 31U ? std::uint64_t{0} : regs[reg];
	};
	for (const auto word : code) {
		if (word == svc)
			break;
		if ((word & 0x7f20'0000U) == 0x2a00'0000U) {
			/* ORR (shifted register), LSL only, Rn = XZR here. */
			const bool wide = (word >> 31) != 0;
			auto result = value((word >> 16) & 31U) << ((word >> 10) & 63U);
			if (!wide)
				result &= 0xffff'ffffU;
			if ((word & 31U) != 31U)
				regs[word & 31U] = result;
		} else if ((word & 0xff80'0000U) == 0x9100'0000U) {
			regs[word & 31U] = value((word >> 5) & 31U) +
				((word >> 10) & 0xfffU);
		} else {
			std::fprintf(stderr, "model: unexpected word %#x\n", word);
			std::abort();
		}
	}
	return regs;
}

[[nodiscard]] arm64::CpuState initial(std::uint64_t &seed)
{
	arm64::CpuState cpu{};
	for (auto &reg : cpu.registers) {
		seed ^= seed << 13;
		seed ^= seed >> 7;
		seed ^= seed << 17;
		reg = seed | 0x8000'0000'0000'0000ULL;
	}
	cpu.pc = code_pc;
	cpu.sp = 0x7000'0000;
	return cpu;
}

/* Runs `program` from `cpu` to its SVC and checks every register against
 * the model of `code`, and the PC and retired count against the code. */
void check(const char *what, const gadgets::Program &program,
	std::span<const std::uint32_t> code, const arm64::CpuState &cpu)
{
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	auto state = cpu;
	std::size_t executed = 0;
	const auto event = gadgets::execute(program, state, *tlb, executed);
	std::array<std::uint64_t, 31> start{};
	std::copy_n(cpu.registers.begin(), 31, start.begin());
	const auto expected = model(code, start);
	const auto fail = [&](const char *why) {
		std::fprintf(stderr, "arm64-gadget-move-heads: %s: %s (event %d, "
			     "PC %#llx, retired %zu)\n", what, why,
			     static_cast<int>(event),
			     static_cast<unsigned long long>(state.pc), executed);
		for (const auto word : code)
			std::fprintf(stderr, "  %08x\n", word);
		std::abort();
	};
	if (event != ::uml::cpu::EngineEvent::system_call)
		fail("the block did not reach its SVC");
	if (state.pc != code_pc + 4 * code.size())
		fail("wrong PC after the SVC");
	if (executed != code.size())
		fail("wrong retired count");
	for (unsigned index = 0; index < 31; ++index)
		if (state.registers[index] != expected[index])
			fail("wrong register");
	if (state.sp != cpu.sp)
		fail("wrong SP");
}

/* Every group here is two words, behind a counting group of three in a
 * counting build: the word group `n` begins with. */
#if UML_GADGET_EXECUTION_HISTOGRAM
constexpr std::size_t count_words = 3;
#else
constexpr std::size_t count_words = 0;
#endif
[[nodiscard]] constexpr std::size_t at(std::size_t n) noexcept
{
	return n * (count_words + 2) + count_words;
}

/* Where the builder puts a head, what it encodes, and its form. */
void structure()
{
	/* Two 64-bit moves: one group of the head and its operand word, then
	 * the SVC. Without indirect sites, as a single step builds, the row
	 * keeps its ordinary words. */
	{
		const std::array code{mov(1, 2), mov(3, 4), svc};
		const auto built = build(code);
		require(built.complete && built.program.instruction_count() == 3);
		const auto words = built.program.words();
		require(words[at(0)] == address(uml_arm64_gadget_run_moves_xx));
		require(words[at(0) + 1] == operand({{x(2), 1}, {x(4), 3}}));
		require(heads_in(built.program).size() == 1);
		require(words.size() + count_words + 2 ==
			build(code, false).program.words().size());
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The counting group in front counts both moves. */
		require(words[at(0) - 1] == 2);
#endif
		require(heads_in(build(code, false).program).empty());
	}
	/* One move, or a row that copies one register, has no head. */
	{
		const std::array one{mov(1, 2), svc};
		require(heads_in(build(one).program).empty());
		const std::array same{mov(1, 2), mov(1, 3), svc};
		require(heads_in(build(same).program).empty());
		/* An unshifted ORR into XZR is not a move: it ends the row. */
		const std::array discard{mov(1, 2), mov(31, 3),
			mov(4, 5), svc};
		require(heads_in(build(discard).program).empty());
		/* Nor is a shifted ORR from XZR. */
		const std::array shifted{mov(1, 2), orr_shifted(3, 4), svc};
		require(heads_in(build(shifted).program).empty());
	}
	/* Widths, from the first move: a 32-bit move into a register a later
	 * 64-bit move reads makes that one 32-bit, and XZR reads the zero
	 * slot. */
	{
		const std::array code{mov(1, 2, false), mov(3, 1), mov(5, 31),
			svc};
		const auto built_words = build(code);
		const auto words = built_words.program.words();
		require(words[at(0)] == address(uml_arm64_gadget_run_moves_wwx));
		require(words[at(0) + 1] == operand({{x(2), 1}, {x(2), 3},
			{zero_slot, 5}}));
	}
	/* A chain reads the first source three times; a swap through X9 reads
	 * each register as it was before the row. */
	{
		const std::array chain{mov(1, 0), mov(2, 1), mov(3, 2), svc};
		require(build(chain).program.words()[at(0) + 1] ==
			operand({{x(0), 1}, {x(0), 2}, {x(0), 3}}));
		const std::array swap{mov(9, 1), mov(1, 2), mov(2, 9), svc};
		require(build(swap).program.words()[at(0) + 1] ==
			operand({{x(1), 9}, {x(2), 1}, {x(1), 2}}));
	}
	/* A destination written again keeps only its last write, at the place
	 * of that write. */
	{
		const std::array code{mov(1, 2), mov(3, 4), mov(1, 5), svc};
		const auto built_words = build(code);
		const auto words = built_words.program.words();
		require(words[at(0)] == address(uml_arm64_gadget_run_moves_xx));
		require(words[at(0) + 1] == operand({{x(4), 3}, {x(5), 1}}));
	}
	/* A fifth destination starts the next head; a single move left over
	 * keeps its ordinary words. */
	{
		const std::array six{mov(1, 2), mov(3, 4), mov(5, 6), mov(7, 8),
			mov(9, 10), mov(11, 12), svc};
		const auto found = heads_in(build(six).program);
		require(found.size() == 2 && found[0].at == at(0) &&
			found[1].at == at(1));
		require(std::string_view{found[0].head.widths} == "xxxx");
		require(std::string_view{found[1].head.widths} == "xx");
		const std::array five{mov(1, 2), mov(3, 4), mov(5, 6), mov(7, 8),
			mov(9, 10), svc};
		const auto built = build(five);
		require(heads_in(built.program).size() == 1);
		require(built.program.instruction_count() == 6);
	}
	/* The register the instruction in front writes, read by a move other
	 * than the last, comes from x0: that move goes first. */
	{
		const std::array code{add(4, 4, 1), mov(1, 2), mov(3, 4),
			mov(5, 6), svc};
		const auto built_words = build(code);
		const auto words = built_words.program.words();
		require(words[at(1)] ==
			address(uml_arm64_gadget_run_moves_xxx_forward));
		require(words[at(1) + 1] ==
			operand({{x(4), 3}, {x(2), 1}, {x(6), 5}}));
		/* Not with forwarding off. */
		const auto built_off = build(code, true, false);
		const auto off = built_off.program.words();
		require(off[at(1)] == address(uml_arm64_gadget_run_moves_xxx));
		require(off[at(1) + 1] ==
			operand({{x(2), 1}, {x(4), 3}, {x(6), 5}}));
		/* Nor for the last move, whose destination the head forwards. */
		const std::array last{add(6, 6, 1), mov(1, 2), mov(5, 6), svc};
		require(build(last).program.words()[at(1)] ==
			address(uml_arm64_gadget_run_moves_xx));
		/* A 32-bit move takes the low half of x0. */
		const std::array narrow{add(2, 2, 1), mov(1, 2, false), mov(3, 4),
			svc};
		require(build(narrow).program.words()[at(1)] ==
			address(uml_arm64_gadget_run_moves_wx_forward));
	}
	/* The instruction behind reads the last destination from x0: its
	 * forwarding form, as behind the row's last ordinary word; one that
	 * reads an earlier destination gets none. */
	{
		const std::array code{mov(1, 2), mov(3, 4), add(5, 3, 1), svc};
		const auto built_on = build(code);
		const auto on = built_on.program.words();
		const auto built_off = build(code, true, false);
		const auto off = built_off.program.words();
		const auto access = gadgets::scalar_access(off.subspan(at(1), 2));
		require(access.contract != nullptr && on[at(1)] != off[at(1)]);
		require(on[at(1)] == access.contract->entries[access.source_mask(
			static_cast<std::uint16_t>(x(3)))]);
		const std::array earlier{mov(1, 2), mov(3, 4), add(5, 1, 1), svc};
		require(build(earlier).program.words()[at(1)] ==
			build(earlier, true, false).program.words()[at(1)]);
	}
	/* Every head is in the table, once per form. */
	require(heads().size() == 2 * (4 + 8 + 16));
}

/* A fault behind a head names its own instruction, and the block retires
 * the instructions before it, the row's included. */
void fault_behind_a_head()
{
	const std::array code{mov(1, 2), mov(3, 4), mov(5, 6), ldr(7, 8), svc};
	const auto built = build(code);
	require(heads_in(built.program).size() == 1);
	std::uint64_t seed = 0x1234'5678'9abc'def1ULL;
	auto cpu = initial(seed);
	cpu.registers[8] = 0x7100'0000;
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	std::size_t executed = 0;
	const auto event = gadgets::execute(built.program, cpu, *tlb, executed);
	gadgets::resolve_fault_position(built.program, cpu.pc);
	require(event == ::uml::cpu::EngineEvent::memory_fault);
	require(cpu.pc == code_pc + 3 * 4);
	/* A block retires whole at entry; the executor takes back what the
	 * fault PC did not reach. */
	require(executed == code.size());
	std::uint64_t again = 0x1234'5678'9abc'def1ULL;
	const auto before = initial(again);
	require(cpu.registers[1] == before.registers[2] &&
		cpu.registers[3] == before.registers[4] &&
		cpu.registers[5] == before.registers[6]);
}

/* The named cases against the model. */
void cases()
{
	const std::vector<std::vector<std::uint32_t>> named{
		/* A dependent chain, and the chain fed by the instruction in
		 * front and read by the one behind. */
		{mov(1, 0), mov(2, 1), mov(3, 2), svc},
		{add(0, 0, 3), mov(1, 0), mov(2, 1), mov(3, 2), add(4, 3, 1), svc},
		/* A swap through a third register, and one in place. */
		{mov(9, 1), mov(1, 2), mov(2, 9), svc},
		{mov(1, 2), mov(2, 1), svc},
		/* A destination of XZR in the middle of a row. */
		{mov(1, 2), mov(31, 3), mov(4, 5), mov(6, 7), svc},
		/* A 32-bit move into a register a later 64-bit move reads, and
		 * a 64-bit move into one a later 32-bit move reads. */
		{mov(1, 2, false), mov(3, 1), svc},
		{mov(1, 2), mov(3, 1, false), mov(4, 3), svc},
		/* XZR sources at both widths. */
		{mov(1, 31), mov(2, 31, false), mov(3, 1), svc},
		/* A 32-bit move of itself. */
		{mov(1, 1, false), mov(2, 1), svc},
		/* Forwarded in to a 32-bit first move, out of a 32-bit last. */
		{add(2, 2, 5), mov(1, 2, false), mov(3, 4), mov(5, 6, false),
			add(7, 5, 1), svc},
		/* Four destinations written over and over. */
		{mov(1, 2), mov(2, 3), mov(3, 4), mov(4, 1), mov(1, 3), mov(2, 4),
			mov(3, 1), mov(4, 2), mov(1, 2), svc},
	};
	std::uint64_t seed = 0x9e37'79b9'7f4a'7c15ULL;
	for (const auto &code : named) {
		const auto cpu = initial(seed);
		check("named case", build(code).program, code, cpu);
		check("named case, forwarding off", build(code, true, false).program,
			code, cpu);
		check("named case, no head", build(code, false).program, code, cpu);
	}
}

/* Generated rows against the model. */
void generated_rows()
{
	std::uint64_t seed = 0x2545'f491'4f6c'dd1dULL;
	const auto next = [&](std::uint64_t bound) {
		seed ^= seed << 13;
		seed ^= seed >> 7;
		seed ^= seed << 17;
		return seed % bound;
	};
	std::size_t with_head = 0, forwarded_in = 0, total = 0;
	std::set<std::string_view> forms;
	for (unsigned trial = 0; trial < 20000; ++trial) {
		std::vector<std::uint32_t> code;
		/* A few registers, so moves depend on and overwrite each other. */
		const unsigned span = 3 + static_cast<unsigned>(next(6));
		const auto reg = [&] { return static_cast<unsigned>(next(span)); };
		if (next(2) != 0)
			code.push_back(add(reg(), reg(), 1));
		const auto length = 1 + next(10);
		for (std::size_t index = 0; index < length; ++index) {
			const unsigned roll = static_cast<unsigned>(next(16));
			if (roll == 0)
				code.push_back(mov(31, reg()));
			else
				code.push_back(mov(reg(), roll == 1 ? 31U : reg(),
					next(3) != 0));
		}
		if (next(2) != 0)
			code.push_back(add(reg(), reg(), 7));
		code.push_back(svc);
		const auto cpu = initial(seed);
		const auto on = build(code);
		check("generated", on.program, code, cpu);
		check("generated, forwarding off", build(code, true, false).program,
			code, cpu);
		check("generated, no head", build(code, false).program, code, cpu);
		const auto found = heads_in(on.program);
		with_head += !found.empty();
		for (const auto &head : found) {
			forwarded_in += head.head.forward;
			forms.insert(head.head.widths);
		}
		++total;
	}
	std::printf("arm64-gadget-move-heads: %zu of %zu generated rows have a "
		    "head, %zu heads take a forwarded register, %zu forms\n",
		    with_head, total, forwarded_in, forms.size());
	require(forms.size() == 4 + 8 + 16 && forwarded_in != 0);
}

} // namespace

int main()
{
	structure();
	fault_behind_a_head();
	cases();
	generated_rows();
	std::puts("arm64-gadget-move-heads: ok");
	return 0;
}
