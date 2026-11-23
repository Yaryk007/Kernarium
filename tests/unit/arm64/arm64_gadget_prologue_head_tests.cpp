// SPDX-License-Identifier: GPL-2.0-or-later
/* Prologue run heads (engine/run_heads.inc). A run of SP-based 64-bit stores,
 * at most one SUB SP and one ADD X29, SP begins with a head, which does the
 * whole run when SP is aligned and every byte it writes lies in one page the
 * TLB holds for writing, and otherwise runs the ordinary words behind it.
 * This test checks where the builder puts a head and what it encodes, up to
 * the bounds of its operand fields; that the head does the run itself and
 * leaves nothing forwarded into the instruction after it; that a span across
 * two pages, a misaligned SP, a page the TLB does not hold, a page it holds
 * only for reading and a guard page below the stack each fall back and end as
 * the architecture says, with every byte of memory; that a tagged SP falls
 * back until the TLB holds its tagged page and then takes the head; that a
 * fault inside the fallback, in the ordinary
 * path and in the slow path of a pair across two pages, resumes at its
 * instruction; that a block with a prologue head and an epilogue head runs
 * both; over generated runs, that every block ends with the state and
 * memory an independent model of the instructions gives; and in a counting
 * build, that execution histogram slot zero counts each instruction once on
 * either path. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/execution_histogram.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_illegal();
extern "C" void uml_arm64_gadget_run_epilogue_2_0();
#define UML_DECLARE_PROLOGUE_HEADS(pairs)                                   \
	extern "C" void uml_arm64_gadget_run_prologue_##pairs##_0_0();      \
	extern "C" void uml_arm64_gadget_run_prologue_##pairs##_0_1();      \
	extern "C" void uml_arm64_gadget_run_prologue_##pairs##_1_0();      \
	extern "C" void uml_arm64_gadget_run_prologue_##pairs##_1_1();
UML_DECLARE_PROLOGUE_HEADS(1)
UML_DECLARE_PROLOGUE_HEADS(2)
UML_DECLARE_PROLOGUE_HEADS(3)
UML_DECLARE_PROLOGUE_HEADS(4)
UML_DECLARE_PROLOGUE_HEADS(5)
UML_DECLARE_PROLOGUE_HEADS(6)
#undef UML_DECLARE_PROLOGUE_HEADS

namespace {

using uml::emulator::tests::address;
using uml::testing::require;

#define UML_PROLOGUE_HEADS(pairs)                                           \
	{{{uml_arm64_gadget_run_prologue_##pairs##_0_0,                     \
	   uml_arm64_gadget_run_prologue_##pairs##_0_1},                    \
	  {uml_arm64_gadget_run_prologue_##pairs##_1_0,                     \
	   uml_arm64_gadget_run_prologue_##pairs##_1_1}}}
/* By pairs, then single stores, then SUB SP count. */
constexpr std::array<std::array<std::array<void (*)(), 2>, 2>, 6> heads{{
	UML_PROLOGUE_HEADS(1), UML_PROLOGUE_HEADS(2), UML_PROLOGUE_HEADS(3),
	UML_PROLOGUE_HEADS(4), UML_PROLOGUE_HEADS(5), UML_PROLOGUE_HEADS(6),
}};
#undef UML_PROLOGUE_HEADS

constexpr std::uint64_t code_pc = 0x4000;
constexpr std::uint64_t page_size = 4096;
/* Guest pages `low`, `high` and `beyond` are adjacent; each case says which
 * the TLB holds and how. Nothing can fill an entry the TLB lacks, so a store
 * there faults. */
constexpr std::uint64_t low = 0x7000'0000;
constexpr std::uint64_t high = low + page_size;
constexpr std::uint64_t beyond = high + page_size;
constexpr std::size_t pages = 3;

constexpr std::uint32_t svc = 0xd400'0001U;
constexpr std::uint32_t ret = 0xd65f'03c0U;

/* Logical tag bits, which a guest that ignores the top byte of its data
 * addresses may set in SP. */
constexpr std::uint64_t tag = 0x5aULL << 56;

[[nodiscard]] constexpr std::uint64_t untagged(std::uint64_t address) noexcept
{
	return address & 0x00ff'ffff'ffff'ffffULL;
}

enum class Mode : std::uint32_t {
	offset = 0xa900'0000U,
	pre = 0xa980'0000U,
	post = 0xa880'0000U,
	no_allocate = 0xa800'0000U,
};

[[nodiscard]] constexpr std::uint32_t stp(unsigned first, unsigned second,
	int offset, Mode mode = Mode::offset) noexcept
{
	return static_cast<std::uint32_t>(mode) |
		((static_cast<std::uint32_t>(offset / 8) & 0x7fU) << 15) |
		(second << 10) | (31U << 5) | first;
}
[[nodiscard]] constexpr std::uint32_t stp32(unsigned first, unsigned second,
	int offset) noexcept
{
	return 0x2900'0000U |
		((static_cast<std::uint32_t>(offset / 4) & 0x7fU) << 15) |
		(second << 10) | (31U << 5) | first;
}
[[nodiscard]] constexpr std::uint32_t str(unsigned reg, unsigned offset) noexcept
{
	return 0xf900'0000U | ((offset / 8) << 10) | (31U << 5) | reg;
}
/* STUR (0x000), STR post-index (0x400) and pre-index (0xc00). */
[[nodiscard]] constexpr std::uint32_t str9(unsigned reg, int offset,
	std::uint32_t kind) noexcept
{
	return 0xf800'0000U | kind |
		((static_cast<std::uint32_t>(offset) & 0x1ffU) << 12) |
		(31U << 5) | reg;
}
[[nodiscard]] constexpr std::uint32_t sub_sp(unsigned immediate) noexcept
{
	return 0xd100'03ffU | (immediate << 10);
}
/* SUB SP, SP, #immediate, LSL #12. */
[[nodiscard]] constexpr std::uint32_t sub_sp_pages(unsigned immediate) noexcept
{
	return 0xd140'03ffU | (immediate << 10);
}
/* Near misses of the elements: STTR, STR Xt, [SP, Xm], and LDADD Xs, Xt,
 * [SP], which only bit 21 tells from a STUR. */
[[nodiscard, maybe_unused]] constexpr std::uint32_t sttr(unsigned reg, int offset) noexcept
{
	return str9(reg, offset, 0x800U);
}
[[nodiscard, maybe_unused]] constexpr std::uint32_t str_register(unsigned reg,
	unsigned index) noexcept
{
	return 0xf820'6800U | (index << 16) | (31U << 5) | reg;
}
[[nodiscard, maybe_unused]] constexpr std::uint32_t ldadd(unsigned source,
	unsigned reg) noexcept
{
	return 0xf820'03e0U | (source << 16) | reg;
}
static_assert(ldadd(1, 19) == 0xf821'03f3U);
[[nodiscard]] constexpr std::uint32_t add_x29(unsigned immediate) noexcept
{
	return 0x9100'03fdU | (immediate << 10);
}
/* ADD Xd, Xn, #imm, neither of them SP. */
[[nodiscard]] constexpr std::uint32_t add(unsigned destination, unsigned source,
	unsigned immediate) noexcept
{
	return 0x9100'0000U | (immediate << 10) | (source << 5) | destination;
}
[[nodiscard]] constexpr std::uint32_t ldp(unsigned first, unsigned second,
	int offset) noexcept
{
	return 0xa8c0'0000U |
		((static_cast<std::uint32_t>(offset / 8) & 0x7fU) << 15) |
		(second << 10) | (31U << 5) | first;
}

/* The instruction behind every case's run reads X29, which the run's last
 * element may have just written: X9 = X29 + 8. */
constexpr std::uint32_t after = add(9, 29, 8);

/* How the TLB holds a page. */
enum class Held : std::uint8_t { none, read, write };
using Holding = std::array<Held, pages>;
constexpr Holding all_written{Held::write, Held::write, Held::write};

/* The pattern every case starts from: no two 8-byte words share it. */
[[nodiscard]] std::vector<std::byte> pattern()
{
	std::vector<std::byte> bytes(pages * page_size);
	for (std::size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::byte>(
			(index * 131U + (index >> 8) * 7U + 13U) & 0xffU);
	return bytes;
}

/* Host pages behind the guest pages low, high and beyond. */
struct Memory final {
	Memory() : bytes{static_cast<std::byte *>(
			   std::aligned_alloc(page_size, pages * page_size))}
	{
		require(bytes != nullptr);
		reset();
	}
	~Memory() { std::free(bytes); }
	Memory(const Memory &) = delete;
	Memory &operator=(const Memory &) = delete;

	void reset() const
	{
		const auto initial = pattern();
		std::memcpy(bytes, initial.data(), initial.size());
	}
	[[nodiscard]] std::byte *host(std::uint64_t guest) const noexcept
	{
		return bytes + (guest - low);
	}
	[[nodiscard]] bool same(const std::vector<std::byte> &expected) const noexcept
	{
		return std::memcmp(bytes, expected.data(), expected.size()) == 0;
	}

	std::byte *bytes;
};

[[nodiscard]] std::unique_ptr<::uml::memory::Tlb> tlb_for(const Memory &memory,
	const Holding &holding)
{
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	for (std::size_t index = 0; index < pages; ++index) {
		if (holding[index] == Held::none)
			continue;
		const auto page = low + index * page_size;
		auto &entry = tlb->entries[(page >> 12) &
			(::uml::memory::tlb_entry_count - 1)];
		const auto delta =
			reinterpret_cast<std::uintptr_t>(memory.host(page)) - page;
		entry.read_page = page >> 12;
		entry.read_data_minus_address = delta;
		if (holding[index] == Held::write) {
			entry.write_page = page >> 12;
			entry.write_data_minus_address = delta;
		}
	}
	return tlb;
}

/* What the instructions do, from the architecture: SP must be 16-byte
 * aligned for every access through it, a store faults without writing
 * anything when a byte of it is on a page the TLB does not hold for writing,
 * and a store writes back only when it completes. The model runs every word
 * before the block's last, the SVC. */
struct Expected final {
	std::array<std::uint64_t, 31> registers{};
	std::uint64_t sp{};
	std::vector<std::byte> memory;
	std::optional<std::size_t> fault;
	bool alignment{};
};

[[nodiscard]] std::int64_t signed_field(std::uint32_t value, unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(static_cast<std::uint64_t>(value) << shift) >>
		shift;
}

[[nodiscard]] Expected model(std::span<const std::uint32_t> code,
	const arm64::CpuState &start, const Holding &holding)
{
	Expected out{};
	std::copy_n(start.registers.begin(), 31, out.registers.begin());
	out.sp = start.sp;
	out.memory = pattern();
	/* The top byte of an address is a tag the guest ignores. */
	const auto writable = [&](std::uint64_t at, std::uint64_t bytes) {
		at = untagged(at);
		for (auto byte = at; byte < at + bytes; ++byte)
			if (byte < low || byte >= low + pages * page_size ||
			    holding[(byte - low) / page_size] != Held::write)
				return false;
		return true;
	};
	const auto value = [&](unsigned reg) {
		return reg == 31U ? std::uint64_t{0} : out.registers[reg];
	};
	const auto put = [&](std::uint64_t at, std::uint64_t data,
				 std::size_t bytes) {
		std::memcpy(out.memory.data() + (untagged(at) - low), &data, bytes);
	};
	for (std::size_t index = 0; index + 1 < code.size(); ++index) {
		const auto word = code[index];
		const auto immediate = ((word >> 10) & 0xfffU) <<
			((word & 0x0040'0000U) != 0 ? 12U : 0U);
		if ((word & 0xff80'03ffU) == 0xd100'03ffU) {
			out.sp -= immediate;
			continue;
		}
		if ((word & 0xff80'0000U) == 0x9100'0000U) {
			const auto source = (word >> 5) & 31U;
			const auto result = (source == 31U ? out.sp :
				out.registers[source]) + immediate;
			if ((word & 31U) == 31U)
				out.sp = result;
			else
				out.registers[word & 31U] = result;
			continue;
		}
		if ((out.sp & 15U) != 0) {
			out.fault = index;
			out.alignment = true;
			return out;
		}
		std::uint64_t at = 0;
		std::uint64_t writeback = out.sp;
		const unsigned first = word & 31U;
		unsigned second = 31U;
		std::uint64_t bytes = 8;
		bool pair = false;
		if ((word & 0x3a00'0000U) == 0x2800'0000U) {
			pair = true;
			const bool wide = (word >> 30) == 2U;
			const auto scale = wide ? 8 : 4;
			bytes = wide ? 16 : 8;
			second = (word >> 10) & 31U;
			const auto displacement =
				signed_field((word >> 15) & 0x7fU, 7) * scale;
			const auto mode = (word >> 23) & 3U;
			at = mode == 1U ? out.sp :
				out.sp + static_cast<std::uint64_t>(displacement);
			if (mode == 1U || mode == 3U)
				writeback = out.sp +
					static_cast<std::uint64_t>(displacement);
		} else if ((word & 0xffc0'0000U) == 0xf900'0000U) {
			at = out.sp + (((word >> 10) & 0xfffU) << 3);
		} else {
			const auto displacement =
				signed_field((word >> 12) & 0x1ffU, 9);
			const auto kind = (word >> 10) & 3U;
			at = kind == 1U ? out.sp :
				out.sp + static_cast<std::uint64_t>(displacement);
			if (kind != 0U)
				writeback = out.sp +
					static_cast<std::uint64_t>(displacement);
		}
		if (!writable(at, bytes)) {
			out.fault = index;
			return out;
		}
		if (pair && bytes == 8) {
			put(at, value(first), 4);
			put(at + 4, value(second), 4);
		} else {
			put(at, value(first), 8);
			if (pair)
				put(at + 8, value(second), 8);
		}
		out.sp = writeback;
	}
	return out;
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
[[nodiscard]] gadgets::BuildResult build(std::uint64_t pc,
	std::span<const std::uint32_t> code, bool sites = true)
{
	static Site site;
	if (!sites)
		return gadgets::Builder{}.build(pc, code, unlinked,
			std::pmr::get_default_resource());
	return gadgets::Builder{}.build(pc, code, unlinked,
		std::pmr::get_default_resource(),
		[](std::uint64_t) { return &site.site; });
}

[[nodiscard]] arm64::CpuState initial(std::uint64_t sp, std::uint64_t pc)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < 31; ++index)
		cpu.registers[index] = 0x5a5a'0000'0000'0000ULL | (index << 8) | index;
	cpu.sp = sp;
	cpu.pc = pc;
	return cpu;
}

struct Outcome final {
	::uml::cpu::EngineEvent event{};
	arm64::CpuState cpu{};
	std::size_t executed{};
	/* The guest words execution histogram slot zero counted, in a
	 * counting build. */
	std::uint64_t counted{};
};

/* Execution histogram slot zero of this thread, or 0 in a build without the
 * histogram. */
[[nodiscard]] std::uint64_t slot_zero() noexcept
{
#if UML_GADGET_EXECUTION_HISTOGRAM
	return static_cast<const std::atomic_uint64_t *>(
		gadgets::diagnostics::execution_counters())->load(
		std::memory_order_relaxed);
#else
	return 0;
#endif
}

[[nodiscard]] Outcome run(const gadgets::Program &program,
	arm64::CpuState cpu, ::uml::memory::Tlb &tlb)
{
	std::size_t executed = 0;
	tlb.fault_alignment = 0;
	const auto before = slot_zero();
	const auto event = gadgets::execute(program, cpu, tlb, executed);
	const auto counted = slot_zero() - before;
	gadgets::resolve_fault_position(program, cpu.pc);
	return {event, cpu, executed, counted};
}

/* The block's outcome against the model: a fault at its instruction, or the
 * whole block to its SVC; then every register, SP and every byte. */
void check(const char *what, const Outcome &outcome, const Expected &expected,
	   const Memory &memory, std::uint64_t pc, std::size_t instructions)
{
	const auto fail = [&](const char *why) {
		std::fprintf(stderr, "arm64-gadget-prologue-heads: %s: %s (event "
			     "%d, PC %#llx, retired %zu, SP %#llx; expected SP "
			     "%#llx)\n", what, why,
			     static_cast<int>(outcome.event),
			     static_cast<unsigned long long>(outcome.cpu.pc),
			     outcome.executed,
			     static_cast<unsigned long long>(outcome.cpu.sp),
			     static_cast<unsigned long long>(expected.sp));
		std::abort();
	};
	if (expected.fault) {
		if (outcome.event != ::uml::cpu::EngineEvent::memory_fault)
			fail("no memory fault");
		if (outcome.cpu.pc != pc + *expected.fault * 4)
			fail("wrong fault PC");
		/* A block retires whole at entry; the executor takes back what
		 * the fault PC did not reach. */
		if (outcome.executed != instructions)
			fail("wrong retired count at the fault");
	} else {
		if (outcome.event != ::uml::cpu::EngineEvent::system_call)
			fail("the block did not reach its SVC");
		if (outcome.cpu.pc != pc + instructions * 4)
			fail("wrong PC after the SVC");
		if (outcome.executed != instructions)
			fail("wrong retired count");
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* Each instruction once, on the head's path or the run's. */
		if (outcome.counted != instructions)
			fail("histogram slot zero counted another number");
#endif
	}
	for (unsigned index = 0; index < 31; ++index)
		if (outcome.cpu.registers[index] != expected.registers[index])
			fail("wrong register");
	if (outcome.cpu.sp != expected.sp)
		fail("wrong SP");
	if (!memory.same(expected.memory))
		fail("wrong memory");
}

/* The head in front of a run: its pairs, single stores and SUB SP count,
 * which name it, and where it is, or nullopt. */
struct Placed final {
	std::size_t pairs{};
	std::size_t single{};
	std::size_t subs{};
	std::size_t at{};

	[[nodiscard]] std::size_t elements() const noexcept
	{
		return pairs + single + subs + 1;
	}
	[[nodiscard]] std::size_t operand_words() const noexcept
	{
		return 1 + (pairs + single + 1) / 4;
	}
};

[[nodiscard]] std::optional<Placed> head_of(const gadgets::Program &program)
{
	const auto words = program.words();
	for (std::size_t index = 0; index < words.size(); ++index)
		for (std::size_t pairs = 1; pairs <= heads.size(); ++pairs)
			for (std::size_t single = 0; single < 2; ++single)
				for (std::size_t subs = 0; subs < 2; ++subs)
					if (words[index] == address(
						    heads[pairs - 1][single][subs]))
						return Placed{pairs, single, subs,
							index};
	return std::nullopt;
}

#if !UML_GADGET_EXECUTION_HISTOGRAM
/* `program` with every group of the run behind the head replaced by the
 * illegal exit, which names the group's position, so a run that reaches
 * any of them leaves illegally; the words after the run stay. A counting
 * build puts a counting group in front of each, so it has no such copy. */
[[nodiscard]] gadgets::Program without_fallback(const gadgets::Program &program)
{
	const auto placed = head_of(program);
	require(placed.has_value());
	const auto words = program.words();
	gadgets::ProgramWords copy{std::pmr::get_default_resource()};
	for (const auto word : words)
		copy.push_back(word);
	const auto first = placed->at + 1 + placed->operand_words();
	for (std::size_t group = 0; group < placed->elements(); ++group) {
		copy[first + 2 * group] = address(uml_arm64_gadget_illegal);
		copy[first + 2 * group + 1] = 0xdead'0000U + 4U * group;
	}
	return gadgets::Program{program.guest_address(),
		program.instruction_count(), std::move(copy), false};
}
#endif

/* A run at a bound of the operand fields, the SP it runs from, where the
 * head goes, if anywhere, and for a head at the run's start its first
 * operand word up to the stores. */
struct Bound final {
	std::vector<std::uint32_t> code;
	std::uint64_t sp;
	std::optional<std::size_t> at;
	std::uintptr_t first;
};

const std::vector<Bound> &bound_runs()
{
	/* Span 0 to 15, X29 at the first byte. */
	constexpr auto span = std::uintptr_t{15} << 10;
	static const std::vector<Bound> all{
		/* The first byte -512 units, SP -256 at exit. */
		{{sub_sp_pages(1), stp(29, 30, 0), add_x29(0), after, svc},
		 beyond + 0x400, 0, (std::uintptr_t{0x200}) | span |
			 (std::uintptr_t{0x300} << 19)},
		/* -514 units: the head takes the pair and the ADD X29 alone. */
		{{sub_sp_pages(1), stp(29, 30, -16, Mode::pre), add_x29(0), after,
		  svc}, beyond + 0x400, 2, 0},
		/* SP -512 units at exit. */
		{{stp(29, 30, 0), add_x29(0), sub_sp_pages(2), after, svc},
		 high + 0x400, 0, span | (std::uintptr_t{0x200} << 19)},
		/* -513 units: no suffix has a pair and the ADD X29. */
		{{stp(29, 30, -16, Mode::pre), add_x29(0), sub_sp_pages(2), after,
		  svc}, high + 0x400, std::nullopt, 0},
	};
	return all;
}

/* Where the builder puts a head and the operand words it writes. */
void structure()
{
#if !UML_GADGET_EXECUTION_HISTOGRAM
	/* STP X29, X30 pre-index, STR of one register and MOV X29, SP. The
	 * single goes first; the span is SP - 32 to SP - 9, the single's last
	 * byte; SP - 32 and X29 = SP - 32 at exit. */
	{
		const std::array code{stp(29, 30, -32, Mode::pre), str(19, 16),
			add_x29(0), after, svc};
		const auto built = build(code_pc, code);
		const auto words = built.program.words();
		require(words[0] == address(uml_arm64_gadget_run_prologue_1_1_0));
		const auto units = static_cast<std::uintptr_t>(-4) & 0x3ffU;
		const auto exit = static_cast<std::uintptr_t>(-2) & 0x3ffU;
		const auto x19 = std::uintptr_t{2} | (19U << 5) | (19U << 10);
		const auto x29 = std::uintptr_t{0} | (29U << 5) | (30U << 10);
		require(words[1] == (units | (std::uintptr_t{23} << 10) |
			(exit << 19) | (x19 << 34) | (x29 << 49)));
		/* The instruction behind the run is translated as it would be
		 * with nothing before it to forward from, unlike the same
		 * instruction behind the run where no head can go. */
		const std::array alone{after, svc};
		const auto plain = build(code_pc + 12, alone).program.words()[0];
		require(words[2 + 2 * 3] == plain);
		const auto bare = build(code_pc, code, false);
		require(!head_of(bare.program));
		require(bare.program.words()[2 * 3] != plain);
	}
	/* The frame of six pairs below a SUB SP and ADD X29, SP, #80. */
	{
		const std::array code{sub_sp(96), stp(29, 30, 80), stp(20, 19, 64),
			stp(22, 21, 48), stp(24, 23, 32), stp(26, 25, 16),
			stp(28, 27, 0), add_x29(80), after, svc};
		const auto built = build(code_pc, code);
		const auto words = built.program.words();
		require(words[0] == address(uml_arm64_gadget_run_prologue_6_0_1));
		const auto units = static_cast<std::uintptr_t>(-12) & 0x3ffU;
		const auto exit = static_cast<std::uintptr_t>(-6) & 0x3ffU;
		const auto x29 = std::uintptr_t{10} | (29U << 5) | (30U << 10);
		const auto x20 = std::uintptr_t{8} | (20U << 5) | (19U << 10);
		require((words[1] & ((std::uintptr_t{1} << 34) - 1)) ==
			(units | (std::uintptr_t{95} << 10) | (exit << 19) |
			 (std::uintptr_t{10} << 29)));
		require(words[1] >> 34 == (x29 | (x20 << 15)));
		const auto x28 = std::uintptr_t{0} | (28U << 5) | (27U << 10);
		require(words[2] >> 45 == x28);
	}
	/* X29 set before the other stores, as GCC orders a prologue, and a
	 * single store among them. */
	{
		const std::array code{stp(29, 30, -48, Mode::pre), add_x29(0),
			stp(19, 20, 16), str(21, 32), after, svc};
		const auto placed = head_of(build(code_pc, code).program);
		require(placed && placed->at == 0 && placed->pairs == 2 &&
			placed->single == 1 && placed->subs == 0);
	}
	/* A store 31 units into the span and X29 31 units into it are the
	 * furthest a head takes; one further ends the run a head may take
	 * there. */
	{
		const std::array code{stp(29, 30, -256, Mode::pre), str(19, 248),
			add_x29(248), after, svc};
		require(head_of(build(code_pc, code).program).has_value());
	}
	for (const auto &code : std::vector<std::vector<std::uint32_t>>{
		     {stp(29, 30, -272, Mode::pre), str(19, 256), add_x29(0),
		      after, svc},
		     {stp(29, 30, -272, Mode::pre), add_x29(256), after, svc}}) {
		require(!head_of(build(code_pc, code).program));
	}
	/* What gets no head: no ADD X29, no pair, a run of one element, X29
	 * stored after it is set, XZR stored as either register of a pair or as
	 * the single, two single stores, and a block built without indirect
	 * sites. */
	for (const auto &code : std::vector<std::vector<std::uint32_t>>{
		     {stp(29, 30, -16, Mode::pre), stp(19, 20, -16, Mode::pre),
		      after, svc},
		     {str9(19, -16, 0xc00U), add_x29(0), after, svc},
		     {add_x29(0), after, svc},
		     {add_x29(0), stp(29, 30, -16, Mode::pre), after, svc},
		     {stp(31, 30, -16, Mode::pre), add_x29(0), after, svc},
		     {stp(29, 31, -16, Mode::pre), add_x29(0), after, svc},
		     {stp(29, 30, -32, Mode::pre), str(31, 16), add_x29(0), after,
		      svc},
		     {stp(29, 30, -32, Mode::pre), str(19, 16), str(20, 24),
		      add_x29(0), after, svc}}) {
		require(!head_of(build(code_pc, code).program));
	}
	{
		const std::array code{stp(29, 30, -16, Mode::pre), add_x29(0),
			after, svc};
		require(head_of(build(code_pc, code).program).has_value());
		require(!head_of(build(code_pc, code, false).program));
	}
	/* A run the head cannot do whole gets a head for the longest suffix it
	 * can: behind a 32-bit pair, STTR and a store to SP plus a register,
	 * which are no elements, a seventh pair, an SP the ordinary gadget
	 * would fault on, and a second SUB SP. */
	for (const auto &[code, at] :
	     std::vector<std::pair<std::vector<std::uint32_t>, std::size_t>>{
		     {{stp32(1, 2, -8), stp(29, 30, -16, Mode::pre), add_x29(0),
		       after, svc}, 2},
		     {{sttr(19, 16), stp(29, 30, -16, Mode::pre), add_x29(0),
		       after, svc}, 2},
		     {{str_register(19, 1), stp(29, 30, -16, Mode::pre),
		       add_x29(0), after, svc}, 2},
		     {{stp(0, 1, -16, Mode::pre), stp(2, 3, -16, Mode::pre),
		       stp(4, 5, -16, Mode::pre), stp(6, 7, -16, Mode::pre),
		       stp(19, 20, -16, Mode::pre), stp(21, 22, -16, Mode::pre),
		       stp(29, 30, -16, Mode::pre), add_x29(0), after, svc}, 2},
		     {{sub_sp(8), stp(29, 30, -16, Mode::pre), add_x29(0), after,
		       svc}, 2},
		     {{sub_sp(16), sub_sp(16), stp(29, 30, 0), add_x29(0), after,
		       svc}, 2}}) {
		const auto placed = head_of(build(code_pc, code).program);
		require(placed && placed->at == at);
	}
	/* Nor is an LSE atomic on SP, which only bit 21 tells from a STUR:
	 * read as one (X19 to SP + 16), it would give the whole run a head. The
	 * head takes the pair and the ADD X29 alone, behind the atomic's own
	 * group, whatever words that group has. */
	{
		const std::array code{ldadd(1, 19), stp(29, 30, -16, Mode::pre),
			add_x29(0), after, svc};
		const auto placed = head_of(build(code_pc, code).program);
		require(placed && placed->at != 0 && placed->pairs == 1 &&
			placed->single == 0 && placed->subs == 0);
	}
	/* A run that ends the block's window ends at its fallthrough. */
	{
		const std::array code{stp(29, 30, -16, Mode::pre), add_x29(0)};
		const auto placed = head_of(build(code_pc, code).program);
		require(placed && placed->at == 0);
	}
	/* The bounds of the operand fields: the first byte 512 units of 8 below
	 * SP at entry, or SP 512 units of 16 below it at exit, is as far as a
	 * head takes; one unit further leaves the head a suffix of the run, or
	 * nothing. */
	for (const auto &item : bound_runs()) {
		const auto built = build(code_pc, item.code);
		const auto placed = head_of(built.program);
		if (!item.at) {
			require(!placed);
			continue;
		}
		require(placed && placed->at == *item.at);
		if (item.first != 0)
			require((built.program.words()[1] &
				 ((std::uintptr_t{1} << 34) - 1)) == item.first);
	}
#endif
}

struct Case final {
	const char *what;
	std::vector<std::uint32_t> code; // the run, `after`, then SVC
};

const std::vector<Case> &cases()
{
	static const std::vector<Case> all{
		{"pair and single", {stp(29, 30, -32, Mode::pre), str(19, 16),
			add_x29(0), after, svc}},
		{"pairs", {stp(29, 30, -32, Mode::pre), stp(20, 19, 16),
			add_x29(0), after, svc}},
		{"frame", {sub_sp(96), stp(29, 30, 80), stp(20, 19, 64),
			stp(22, 21, 48), stp(24, 23, 32), stp(26, 25, 16),
			stp(28, 27, 0), add_x29(80), after, svc}},
		{"frame with a single", {sub_sp(96), stp(29, 30, 80),
			str(28, 72), stp(20, 19, 56), stp(22, 21, 40),
			stp(24, 23, 24), stp(26, 25, 8), add_x29(80), after, svc}},
		{"frame pointer first", {stp(29, 30, -48, Mode::pre), add_x29(0),
			stp(19, 20, 16), str(21, 32), after, svc}},
		{"mixed", {stp(29, 30, -16, Mode::pre), sub_sp(32),
			str9(20, 8, 0x000U), add_x29(32),
			stp(21, 22, 16, Mode::no_allocate), after, svc}},
		{"pre-index single", {str9(19, -16, 0xc00U),
			stp(29, 30, -16, Mode::pre), add_x29(0), after, svc}},
		{"post-index", {stp(29, 30, -16, Mode::post), add_x29(0),
			str9(21, -32, 0x400U), after, svc}},
		{"seven stores", {stp(29, 30, -112, Mode::pre), stp(19, 20, 16),
			stp(21, 22, 32), stp(23, 24, 48), stp(25, 26, 64),
			stp(27, 28, 80), str(0, 96), add_x29(0), after, svc}},
	};
	return all;
}

/* Each case's block where the head does the run: whole, and with nothing
 * behind the head to fall back to, which ends the same only if the head did
 * all of it. */
void head_does_the_run(const Memory &memory)
{
	for (const auto &item : cases()) {
		const auto built = build(code_pc, item.code);
		require(head_of(built.program).has_value());
		const auto start = initial(low + 0x400, code_pc);
		const auto expected = model(item.code, start, all_written);
		require(!expected.fault);
		memory.reset();
		auto tlb = tlb_for(memory, all_written);
		check(item.what, run(built.program, start, *tlb), expected, memory,
			code_pc, item.code.size());
#if !UML_GADGET_EXECUTION_HISTOGRAM
		memory.reset();
		const auto bare = without_fallback(built.program);
		check(item.what, run(bare, start, *tlb), expected, memory, code_pc,
			item.code.size());
#endif
	}
}

/* The highest SP in `high` at which `code` writes bytes of both low and
 * high: with either page alone it faults, and not for alignment. */
[[nodiscard]] std::uint64_t crossing_sp(std::span<const std::uint32_t> code)
{
	for (auto sp = beyond - 16; sp >= high; sp -= 16) {
		const auto start = initial(sp, code_pc);
		const auto below = model(code, start,
			{Held::write, Held::none, Held::none});
		const auto above = model(code, start,
			{Held::none, Held::write, Held::none});
		if (below.fault && above.fault && !below.alignment &&
		    !above.alignment)
			return sp;
	}
	require(false);
	return 0;
}

/* Each way the head falls back ends as the architecture says, and with
 * nothing behind the head the fallback reaches the run's first word. */
void head_falls_back(const Memory &memory)
{
	struct Fallback final {
		const char *what;
		/* 0 for an SP at which the run writes both low and high. */
		std::uint64_t sp;
		Holding holding;
	};
	const Fallback fallbacks[] = {
		/* The run's bytes cross from low into high; both are held. */
		{"across a page", 0, all_written},
		{"a misaligned SP", high + 0x208, all_written},
		/* The TLB does not hold the run's page. */
		{"a page the TLB lacks", high + 0x400,
			{Held::write, Held::none, Held::write}},
		/* The TLB holds the run's page for reading only. */
		{"a read-only page", high + 0x400,
			{Held::write, Held::read, Held::write}},
		/* SP at the base of its page and the page below a guard page,
		 * which holds every byte of the frame. */
		{"a guard page below the stack", high,
			{Held::none, Held::write, Held::write}},
	};
	for (const auto &fallback : fallbacks) {
		for (const auto &item : cases()) {
			const auto built = build(code_pc, item.code);
			const auto start = initial(fallback.sp != 0 ? fallback.sp :
				crossing_sp(item.code), code_pc);
			const auto expected = model(item.code, start,
				fallback.holding);
			memory.reset();
			auto tlb = tlb_for(memory, fallback.holding);
			const auto outcome = run(built.program, start, *tlb);
			check(item.what, outcome, expected, memory, code_pc,
				item.code.size());
			if (expected.alignment)
				require(tlb->fault_alignment != 0);
#if !UML_GADGET_EXECUTION_HISTOGRAM
			const auto bare = without_fallback(built.program);
			memory.reset();
			auto fresh = tlb_for(memory, fallback.holding);
			const auto stopped = run(bare, start, *fresh);
			if (stopped.event !=
				    ::uml::cpu::EngineEvent::illegal_instruction ||
			    stopped.cpu.pc != 0xdead'0000U || !memory.same(pattern())) {
				std::fprintf(stderr, "arm64-gadget-prologue-heads: "
					     "%s, %s: the fallback did not reach "
					     "the run untouched (event %d, PC "
					     "%#llx)\n", item.what, fallback.what,
					     static_cast<int>(stopped.event),
					     static_cast<unsigned long long>(
						     stopped.cpu.pc));
				std::abort();
			}
#endif
		}
	}
}

/* A fault inside the fallback leaves the stores before it done and nothing
 * of its own, and the block that starts at the faulting instruction, once
 * its page is there, finishes the run as one uninterrupted run would have:
 * for an ordinary store below a page the TLB lacks, and for a pair across
 * two pages, which faults in the slow path. */
void fault_and_resume(const Memory &memory)
{
	struct Resume final {
		const char *what;
		std::vector<std::uint32_t> code;
		std::uint64_t sp;
		Holding holding;
		/* The page the fault needs, which the TLB then holds. */
		std::size_t missing;
	};
	std::vector<Resume> resumes;
	for (const auto &item : cases())
		for (const auto offset : {16U, 48U, 80U})
			resumes.push_back({item.what, item.code, high + offset,
				{Held::none, Held::write, Held::write}, 0});
	/* The first pair is in low; the second, from 8 bytes below high's
	 * base, crosses into high, which the TLB lacks. */
	resumes.push_back({"a pair across two pages",
		{stp(29, 30, -48, Mode::pre), stp(19, 20, 24), add_x29(0),
		 after, svc},
		high + 16, {Held::write, Held::none, Held::write}, 1});
	std::size_t faulted_inside = 0;
	for (const auto &item : resumes) {
		const auto start = initial(item.sp, code_pc);
		const auto expected = model(item.code, start, item.holding);
		if (!expected.fault)
			continue;
		const auto built = build(code_pc, item.code);
		require(head_of(built.program).has_value());
		memory.reset();
		auto tlb = tlb_for(memory, item.holding);
		const auto outcome = ::run(built.program, start, *tlb);
		check(item.what, outcome, expected, memory, code_pc,
			item.code.size());
		if (*expected.fault != 0)
			++faulted_inside;

		auto holding = item.holding;
		holding[item.missing] = Held::write;
		auto held = tlb_for(memory, holding);
		const auto resume = *expected.fault;
		const std::span rest{item.code.data() + resume,
			item.code.size() - resume};
		const auto resumed_block = build(outcome.cpu.pc, rest);
		auto whole = model(item.code, start, holding);
		require(!whole.fault);
		const auto resumed = ::run(resumed_block.program, outcome.cpu,
			*held);
		check(item.what, resumed, whole, memory, outcome.cpu.pc,
			rest.size());
	}
	require(faulted_inside > 4);
}

/* A block with a prologue run, a body and an epilogue run before its RET
 * gets both heads, and each does its run. */
void both_heads(const Memory &memory)
{
	const std::array code{stp(29, 30, -32, Mode::pre), stp(20, 19, 16),
		add_x29(0), add(19, 0, 3), add(20, 19, 5), add(0, 29, 0),
		0xa941'4ff4U /* ldp x20, x19, [sp, #16] */, ldp(29, 30, 32), ret};
	const auto built = build(code_pc, code);
	const auto words = built.program.words();
	require(head_of(built.program).has_value());
	require(std::ranges::find(words,
			address(uml_arm64_gadget_run_epilogue_2_0)) != words.end());
	const auto sp = low + 0x400;
	const auto start = initial(sp, code_pc);
	memory.reset();
	auto tlb = tlb_for(memory, all_written);
	const auto outcome = run(built.program, start, *tlb);
	/* The RET's site misses and leaves at the X30 the prologue stored and
	 * the epilogue loaded back. */
	require(outcome.event == ::uml::cpu::EngineEvent::continued);
	require(outcome.cpu.pc == start.registers[30]);
	require(outcome.cpu.sp == sp);
	require(outcome.cpu.registers[0] == sp - 32);
	for (const unsigned reg : {19U, 20U, 29U, 30U})
		require(outcome.cpu.registers[reg] == start.registers[reg]);
	require(outcome.executed == code.size());
#if UML_GADGET_EXECUTION_HISTOGRAM
	require(outcome.counted == code.size());
#endif
	std::uint64_t saved = 0;
	std::memcpy(&saved, memory.host(sp - 32), sizeof(saved));
	require(saved == start.registers[29]);
	std::memcpy(&saved, memory.host(sp - 24), sizeof(saved));
	require(saved == start.registers[30]);
}

/* The runs at the bounds of the operand fields end as the model says, and
 * those a head takes whole do so with nothing behind the head. */
void field_bounds(const Memory &memory)
{
	for (const auto &item : bound_runs()) {
		const auto built = build(code_pc, item.code);
		const auto start = initial(item.sp, code_pc);
		const auto expected = model(item.code, start, all_written);
		require(!expected.fault);
		memory.reset();
		auto tlb = tlb_for(memory, all_written);
		check("a run at a field's bound", run(built.program, start, *tlb),
			expected, memory, code_pc, item.code.size());
#if !UML_GADGET_EXECUTION_HISTOGRAM
		if (item.at == 0) {
			memory.reset();
			check("a run at a field's bound, without its fallback",
				run(without_fallback(built.program), start, *tlb),
				expected, memory, code_pc, item.code.size());
		}
#endif
	}
}

/* A tagged SP, in an address space whose data addresses ignore their top
 * byte: SP-based transfers key the TLB by the tagged address, and the head
 * probes it as they do. The TLB first holds each page under its untagged
 * key, which the head misses, so the first run falls back, and the
 * ordinary words' miss keys the write entry by the tagged page
 * (memory::Tlb::translate_tagged); the next run finds it and the head does
 * the run. Every run ends as the model says, with every byte of memory. */
void tagged_sp(const Memory &memory)
{
	for (const auto &item : cases()) {
		const auto built = build(code_pc, item.code);
		const auto start = initial(tag | (low + 0x400), code_pc);
		const auto expected = model(item.code, start, all_written);
		require(!expected.fault && (expected.sp & ~untagged(~0ULL)) == tag);
		auto tlb = tlb_for(memory, all_written);
		tlb->tagged_addresses = true;
#if !UML_GADGET_EXECUTION_HISTOGRAM
		const auto bare = without_fallback(built.program);
		memory.reset();
		const auto cold = run(bare, start, *tlb);
		require(cold.event == ::uml::cpu::EngineEvent::illegal_instruction &&
			cold.cpu.pc == 0xdead'0000U && memory.same(pattern()));
#endif
		memory.reset();
		check(item.what, run(built.program, start, *tlb), expected, memory,
			code_pc, item.code.size());
#if !UML_GADGET_EXECUTION_HISTOGRAM
		memory.reset();
		check(item.what, run(bare, start, *tlb), expected, memory, code_pc,
			item.code.size());
#endif
		memory.reset();
		check(item.what, run(built.program, start, *tlb), expected, memory,
			code_pc, item.code.size());
	}
}

/* Generated runs of every element form, broken by 32-bit pairs and other
 * instructions, at SPs across, below and above the held pages and misaligned
 * ones, with pages held for writing, for reading only or not at all, against
 * the model. */
void generated_runs(const Memory &memory)
{
	std::uint64_t state = 0x2545'f491'4f6c'dd1dULL;
	const auto next = [&state](std::uint64_t bound) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state % bound;
	};
	std::size_t with_head = 0;
	std::size_t faulted = 0;
	constexpr std::size_t count = 4000;
	for (std::size_t iteration = 0; iteration < count; ++iteration) {
		const auto reg = [&]() {
			const auto pick = next(32);
			/* Mostly callee-saved registers and the frame record,
			 * sometimes XZR. */
			return pick < 28 ? 19U + static_cast<unsigned>(pick % 12) :
				static_cast<unsigned>(next(32));
		};
		std::vector<std::uint32_t> code;
		const auto elements = 1 + next(10);
		for (std::size_t index = 0; index < elements; ++index) {
			const auto pick = next(20);
			const int sixteen = static_cast<int>(next(13)) - 8;
			const int step = next(10) == 0 ? 8 * (2 * sixteen + 1) :
				16 * sixteen;
			if (pick < 9) {
				const Mode modes[] = {Mode::offset, Mode::pre,
					Mode::post, Mode::no_allocate};
				code.push_back(stp(reg(), reg(), step,
					modes[next(4)]));
			} else if (pick < 11) {
				code.push_back(str(reg(), 8 * static_cast<unsigned>(
					next(24))));
			} else if (pick < 13) {
				const int offset = 4 * (static_cast<int>(next(64)) - 40);
				const std::uint32_t kinds[] = {0x000U, 0x400U, 0xc00U};
				code.push_back(str9(reg(), offset, kinds[next(3)]));
			} else if (pick < 15) {
				code.push_back(sub_sp(next(8) == 0 ? 8 :
					16 * static_cast<unsigned>(next(12))));
			} else if (pick < 18) {
				code.push_back(add_x29(8 * static_cast<unsigned>(
					next(40))));
			} else if (pick < 19) {
				/* No element: it ends the run a head may take. */
				code.push_back(stp32(reg() & 15U, reg() & 15U,
					4 * static_cast<int>(next(16))));
			} else {
				code.push_back(add(0, 0, 1));
			}
		}
		code.push_back(after);
		code.push_back(svc);
		std::uint64_t sp = low + 16 * next(pages * page_size / 16);
		if (next(16) == 0)
			sp += 8;
		Holding holding{};
		for (auto &page : holding) {
			const auto pick = next(8);
			page = pick < 6 ? Held::write : pick < 7 ? Held::read :
				Held::none;
		}
		const auto built = build(code_pc, code);
		if (head_of(built.program))
			++with_head;
		const auto start = initial(sp, code_pc);
		const auto expected = model(code, start, holding);
		if (expected.fault)
			++faulted;
		for (const char *what : {"generated run", "generated run, again"}) {
			memory.reset();
			auto tlb = tlb_for(memory, holding);
			check(what, run(built.program, start, *tlb), expected,
				memory, code_pc, code.size());
		}
	}
	std::printf("arm64-gadget-prologue-heads: %zu of %zu generated runs have "
		    "a head, %zu fault\n", with_head, count, faulted);
	require(with_head > count / 8);
	require(faulted > count / 8 && faulted < count - count / 8);
}

} // namespace

int main()
{
	const Memory memory;
	structure();
	head_does_the_run(memory);
	head_falls_back(memory);
	fault_and_resume(memory);
	both_heads(memory);
	field_bounds(memory);
	tagged_sp(memory);
	generated_runs(memory);
	std::puts("arm64-gadget-prologue-heads: ok");
	return 0;
}
