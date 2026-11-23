// SPDX-License-Identifier: GPL-2.0-or-later
/* Same-base translation reuse (engine/same_base.inc). A transfer through a
 * base register that nothing has written since the previous transfer through
 * it in the same direction is a follower: it reuses that transfer's
 * translation, and that transfer becomes its leader. This test builds blocks
 * of every family the builder pairs and counts their leaders and followers.
 * It runs each block, and the same guest words built with the pass off, from
 * one state and one memory, and requires the same event, PC, retired count,
 * fault address, registers, SIMD&FP registers and memory; the pass may change
 * only the words it makes leaders and followers and a follower's
 * displacement. The blocks cover a fault between a leader and its follower
 * and the block that resumes after it; a follower's own fault, by a miss, a
 * page crossing and a page held only for reading, and the resumed block; a
 * follower at the last offset of its page in both directions for every
 * access size, one byte past it, and displacements just outside the
 * operand's range; the kinds of instruction the builder knows write the base,
 * from a W write to a fused ADRP and ADD, an acquire atomic, a row of moves
 * and a run head's SUB SP; every path of the in-place leaders that keep their
 * own (an SP base, or a base or data forwarded), with misses that fault and
 * misses the helper fills; and a follower's result read by the next
 * instruction. arm64-gadget-same-base-writes checks the builder's list of
 * what each instruction writes against the engine. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/execution_histogram.hpp"
#include "cpu/arm64/gadgets/gadget_symbols.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/same_base.hpp"
#include "cpu/arm64/gadgets/scalar_forwarding.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace same_base = gadgets::same_base;

namespace {

using uml::testing::require;

constexpr std::uint64_t code_pc = 0x4000;
constexpr std::uint64_t page_size = 4096;
/* Guest pages low, high and beyond are adjacent; each case says which the
 * TLB holds and how. Nothing can fill an entry the TLB lacks, so an access
 * there faults. */
constexpr std::uint64_t low = 0x7000'0000;
constexpr std::uint64_t high = low + page_size;
constexpr std::uint64_t beyond = high + page_size;
constexpr std::size_t pages = 3;

constexpr std::uint32_t svc = 0xd400'0001U;
constexpr unsigned sp = 31;
constexpr unsigned zr = 31;

enum class Held : std::uint8_t { none, read, write };
using Holding = std::array<Held, pages>;
constexpr Holding all_written{Held::write, Held::write, Held::write};
constexpr Holding high_only{Held::none, Held::write, Held::none};
constexpr Holding beyond_absent{Held::write, Held::write, Held::none};
constexpr Holding beyond_read{Held::write, Held::write, Held::read};
constexpr Holding high_read{Held::write, Held::read, Held::write};

/* Every case starts from this memory: no two 8-byte words share it. */
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
	}
	~Memory() { std::free(bytes); }
	Memory(const Memory &) = delete;
	Memory &operator=(const Memory &) = delete;

	void load(const std::vector<std::byte> &image) const
	{
		std::memcpy(bytes, image.data(), image.size());
	}
	[[nodiscard]] std::vector<std::byte> image() const
	{
		return {bytes, bytes + pages * page_size};
	}
	[[nodiscard]] std::byte *host(std::uint64_t guest) const noexcept
	{
		return bytes + (guest - low);
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

/* The roles of engine/same_base.inc: for each leader and follower word, the
 * rows and forwarding masks it serves, and the established entry each of
 * them stands for. A leader or follower of the in-place family's forms that
 * are not baked serves the row of every transferred register; the register
 * in its operand's low bits picks the row. */
struct Role final {
	std::uintptr_t established;
	unsigned mask;
	bool leader;
	unsigned transferred;  // the baked register, or 32
	/* A follower's d is in operand bits 55:48, else in 15:0. */
	bool narrow;
};

struct Roles final {
	std::multimap<std::uintptr_t, Role> by_word;
};

[[nodiscard]] const Roles &roles()
{
	static const Roles table = [] {
		std::map<std::uintptr_t, unsigned> baked;
		for (unsigned index = 0; index < 8U * 31U; ++index)
			baked.emplace(reinterpret_cast<std::uintptr_t>(
				gadgets::selection::
				uml_arm64_gadget_scalar_unsigned_inplace_table[index]),
				index % 31U);
		Roles out;
		std::size_t rows = 0;
		for (const auto *row = same_base::uml_arm64_gadget_same_base_rows;
		     row != same_base::uml_arm64_gadget_same_base_rows_end;
		     ++row, ++rows) {
			const std::array<std::uintptr_t, 2> entry{
				row->established, 0};
			const auto access = gadgets::scalar_access(entry);
			require(access.contract != nullptr);
			require(access.contract->entries[0] == row->established);
			const auto found = baked.find(row->established);
			const unsigned transferred =
				found == baked.end() ? 32U : found->second;
			for (unsigned mask = 0; mask < 4U; ++mask) {
				const auto established =
					access.contract->entries[mask];
				if (row->leader[mask] != 0) {
					require(established != 0);
					out.by_word.emplace(row->leader[mask],
						Role{established, mask, true,
						     transferred, false});
				}
				if (row->follower[mask] != 0) {
					require(established != 0);
					out.by_word.emplace(row->follower[mask],
						Role{established, mask, false,
						     transferred,
						     row->narrow_displacement(
							     mask)});
				}
			}
		}
		require(rows != 0);
		return out;
	}();
	return table;
}

/* The role of words[index], or null. */
[[nodiscard]] const Role *role_at(std::span<const std::uintptr_t> words,
	std::size_t index)
{
	const auto [first, last] = roles().by_word.equal_range(words[index]);
	if (first == last)
		return nullptr;
	if (std::next(first) == last)
		return &first->second;
	require(index + 1 < words.size());
	const auto transferred = static_cast<unsigned>(
		(words[index + 1] & 0xffffU) / sizeof(std::uint64_t));
	for (auto it = first; it != last; ++it)
		if (it->second.transferred == transferred)
			return &it->second;
	require(false);
	return nullptr;
}

struct Census final {
	int leaders{};
	int followers{};
	/* The forwarding masks the leaders and the followers carry. */
	unsigned leader_masks{};
	unsigned follower_masks{};
};

/* Gadget words are told apart from operands by position: each role word is
 * followed by its operand, which is never a gadget address. */
[[nodiscard]] Census census(const gadgets::Program &program)
{
	Census out;
	const auto words = program.words();
	bool led = false;
	for (std::size_t index = 0; index < words.size(); ++index) {
		const auto *role = role_at(words, index);
		if (role == nullptr)
			continue;
		if (role->leader) {
			++out.leaders;
			out.leader_masks |= 1U << role->mask;
			led = true;
		} else {
			/* A follower reuses what an earlier transfer of its
			 * block translated. */
			require(led);
			++out.followers;
			out.follower_masks |= 1U << role->mask;
		}
	}
	return out;
}

[[noreturn]] void fail(const std::string &name, const char *why)
{
	std::fprintf(stderr, "arm64-gadget-same-base: %s: %s\n", name.c_str(),
		     why);
	std::abort();
}

#if UML_GADGET_EXECUTION_HISTOGRAM
/* An execution counter's words, [counter, slot, guest words], stand before
 * each entry. */
[[nodiscard]] bool counter_at(std::span<const std::uintptr_t> words,
	std::size_t index) noexcept
{
	return index + 3 < words.size() &&
		words[index] == reinterpret_cast<std::uintptr_t>(
			&gadgets::diagnostics::uml_arm64_gadget_execution_count);
}

/* Each counter's slot pairs its entry with the entry counted before it, a
 * leader the pass rewrote included. */
void counted_in_pairs(const std::string &name,
	const gadgets::Program &program)
{
	const auto words = program.words();
	const void *previous = nullptr;
	for (std::size_t index = 0; index < words.size(); ++index) {
		if (!counter_at(words, index))
			continue;
		const auto *entry = reinterpret_cast<const void *>(words[index + 3]);
		if (words[index + 1] != gadgets::diagnostics::
				execution_histogram_slot(previous, entry))
			fail(name, "a counter pairs its entry with another");
		previous = entry;
		index += 3;
	}
}
#endif

/* The pass changes a block's words only where it makes a leader or a
 * follower, each standing for the entry the block built with the pass off
 * has there, and in a follower's displacement field, which no established
 * entry reads; in a histogram build, also the counters that pair those
 * entries. */
void rewrites_only_its_own(const std::string &name,
	const gadgets::Program &program, const gadgets::Program &plain)
{
	const auto words = program.words();
	const auto reference = plain.words();
	if (words.size() != reference.size() ||
	    program.instruction_count() != plain.instruction_count() ||
	    program.accesses_fp_state() != plain.accesses_fp_state())
		fail(name, "the pass changed the block's shape");
	for (std::size_t index = 0; index < words.size(); ++index) {
		const auto *role = role_at(words, index);
		if (role == nullptr) {
#if UML_GADGET_EXECUTION_HISTOGRAM
			if (index > 0 && counter_at(words, index - 1))
				continue;
#endif
			if (words[index] != reference[index])
				fail(name, "the pass changed a word it does not "
					   "own");
			continue;
		}
		if (reference[index] != role->established)
			fail(name, "a leader or follower stands for another "
				   "entry");
		++index;
		const std::uintptr_t field = role->leader ? 0U :
			role->narrow ? 0x00ff'0000'0000'0000U : 0xffffU;
		if (((words[index] ^ reference[index]) & ~field) != 0)
			fail(name, "the pass changed an operand beyond its "
				   "displacement");
	}
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

/* A block built as the executor builds one, with indirect sites, so run
 * heads are placed; with the pass off, the block the established engine
 * runs. */
[[nodiscard]] gadgets::BuildResult build(std::uint64_t pc,
	std::span<const std::uint32_t> code, bool same_base)
{
	static Site site;
	return gadgets::Builder{}.build(pc, code, unlinked,
		std::pmr::get_default_resource(),
		[](std::uint64_t) { return &site.site; },
		gadgets::BuildOptions{.same_base = same_base});
}

/* Encodings. */
[[nodiscard]] constexpr std::uint32_t field(std::int64_t value, unsigned bits,
	unsigned shift) noexcept
{
	return (static_cast<std::uint32_t>(value) & ((1U << bits) - 1U)) <<
		shift;
}

enum class Form : std::uint8_t { unsigned_offset, unscaled, pair };

/* One access shape: its opcode with every register and the immediate clear,
 * the immediate's unit, and the bytes it transfers. `wide` when its
 * follower's displacement is a 16-bit field, the baked in-place family's. */
struct Kind final {
	const char *name;
	std::uint32_t opcode;
	Form form;
	unsigned scale;
	unsigned bytes;
	bool wide;
	bool zero;  // the transferred registers are XZR
};

[[nodiscard]] constexpr std::uint32_t encode(const Kind &kind, unsigned rt,
	unsigned rn, std::int64_t offset) noexcept
{
	if (kind.zero)
		rt = zr;
	switch (kind.form) {
	case Form::unsigned_offset:
		return kind.opcode | field(offset / kind.scale, 12, 10) |
			(rn << 5) | rt;
	case Form::unscaled:
		return kind.opcode | field(offset, 9, 12) | (rn << 5) | rt;
	case Form::pair:
		break;
	}
	/* Clear of the bases the cases use, x2, x6 and SP. */
	const unsigned rt2 = kind.zero ? zr : rt + 3U;
	return kind.opcode | field(offset / kind.scale, 7, 15) | (rt2 << 10) |
		(rn << 5) | rt;
}

constexpr Kind ldr_x{"ldr x", 0xf940'0000U, Form::unsigned_offset, 8, 8, true, false};
constexpr Kind ldr_w{"ldr w", 0xb940'0000U, Form::unsigned_offset, 4, 4, true, false};
constexpr Kind str_x{"str x", 0xf900'0000U, Form::unsigned_offset, 8, 8, true, false};
constexpr Kind str_w{"str w", 0xb900'0000U, Form::unsigned_offset, 4, 4, true, false};
constexpr Kind str_wzr{"str wzr", 0xb900'0000U, Form::unsigned_offset, 4, 4, false, true};
constexpr Kind str_xzr{"str xzr", 0xf900'0000U, Form::unsigned_offset, 8, 8, false, true};
constexpr Kind ldr_xzr{"ldr xzr", 0xf940'0000U, Form::unsigned_offset, 8, 8, false, true};
constexpr Kind ldr_wzr{"ldr wzr", 0xb940'0000U, Form::unsigned_offset, 4, 4, false, true};
constexpr Kind ldrb{"ldrb", 0x3940'0000U, Form::unsigned_offset, 1, 1, false, false};
constexpr Kind ldrh{"ldrh", 0x7940'0000U, Form::unsigned_offset, 2, 2, false, false};
constexpr Kind ldrsb_x{"ldrsb x", 0x3980'0000U, Form::unsigned_offset, 1, 1, false, false};
constexpr Kind ldrsb_w{"ldrsb w", 0x39c0'0000U, Form::unsigned_offset, 1, 1, false, false};
constexpr Kind ldrsh_w{"ldrsh w", 0x79c0'0000U, Form::unsigned_offset, 2, 2, false, false};
constexpr Kind ldrsw{"ldrsw", 0xb980'0000U, Form::unsigned_offset, 4, 4, false, false};
constexpr Kind strb{"strb", 0x3900'0000U, Form::unsigned_offset, 1, 1, false, false};
constexpr Kind strh{"strh", 0x7900'0000U, Form::unsigned_offset, 2, 2, false, false};
constexpr Kind strb_zr{"strb wzr", 0x3900'0000U, Form::unsigned_offset, 1, 1, false, true};
constexpr Kind ldur_x{"ldur x", 0xf840'0000U, Form::unscaled, 1, 8, false, false};
constexpr Kind ldur_w{"ldur w", 0xb840'0000U, Form::unscaled, 1, 4, false, false};
constexpr Kind ldurh{"ldurh", 0x7840'0000U, Form::unscaled, 1, 2, false, false};
constexpr Kind ldursb_x{"ldursb x", 0x3880'0000U, Form::unscaled, 1, 1, false, false};
constexpr Kind stur_x{"stur x", 0xf800'0000U, Form::unscaled, 1, 8, false, false};
constexpr Kind stur_w{"stur w", 0xb800'0000U, Form::unscaled, 1, 4, false, false};
constexpr Kind sturh{"sturh", 0x7800'0000U, Form::unscaled, 1, 2, false, false};
constexpr Kind sturb_zr{"sturb wzr", 0x3800'0000U, Form::unscaled, 1, 1, false, true};
constexpr Kind ldr_b{"ldr b", 0x3d40'0000U, Form::unsigned_offset, 1, 1, false, false};
constexpr Kind ldr_h{"ldr h", 0x7d40'0000U, Form::unsigned_offset, 2, 2, false, false};
constexpr Kind ldr_s{"ldr s", 0xbd40'0000U, Form::unsigned_offset, 4, 4, false, false};
constexpr Kind ldr_d{"ldr d", 0xfd40'0000U, Form::unsigned_offset, 8, 8, false, false};
constexpr Kind ldr_q{"ldr q", 0x3dc0'0000U, Form::unsigned_offset, 16, 16, false, false};
constexpr Kind str_b{"str b", 0x3d00'0000U, Form::unsigned_offset, 1, 1, false, false};
constexpr Kind str_h{"str h", 0x7d00'0000U, Form::unsigned_offset, 2, 2, false, false};
constexpr Kind str_s{"str s", 0xbd00'0000U, Form::unsigned_offset, 4, 4, false, false};
constexpr Kind str_d{"str d", 0xfd00'0000U, Form::unsigned_offset, 8, 8, false, false};
constexpr Kind str_q{"str q", 0x3d80'0000U, Form::unsigned_offset, 16, 16, false, false};
constexpr Kind ldur_q{"ldur q", 0x3cc0'0000U, Form::unscaled, 1, 16, false, false};
constexpr Kind ldur_s{"ldur s", 0xbc40'0000U, Form::unscaled, 1, 4, false, false};
constexpr Kind stur_q{"stur q", 0x3c80'0000U, Form::unscaled, 1, 16, false, false};
constexpr Kind stur_d{"stur d", 0xfc00'0000U, Form::unscaled, 1, 8, false, false};
constexpr Kind ldp_x{"ldp x", 0xa940'0000U, Form::pair, 8, 16, false, false};
constexpr Kind ldp_w{"ldp w", 0x2940'0000U, Form::pair, 4, 8, false, false};
constexpr Kind ldpsw{"ldpsw", 0x6940'0000U, Form::pair, 4, 8, false, false};
constexpr Kind ldnp_x{"ldnp x", 0xa840'0000U, Form::pair, 8, 16, false, false};
constexpr Kind stp_x{"stp x", 0xa900'0000U, Form::pair, 8, 16, false, false};
constexpr Kind stp_w{"stp w", 0x2900'0000U, Form::pair, 4, 8, false, false};
constexpr Kind stp_xzr{"stp xzr", 0xa900'0000U, Form::pair, 8, 16, false, true};
constexpr Kind ldp_s{"ldp s", 0x2d40'0000U, Form::pair, 4, 8, false, false};
constexpr Kind ldp_d{"ldp d", 0x6d40'0000U, Form::pair, 8, 16, false, false};
constexpr Kind ldp_q{"ldp q", 0xad40'0000U, Form::pair, 16, 32, false, false};
constexpr Kind stp_s{"stp s", 0x2d00'0000U, Form::pair, 4, 8, false, false};
constexpr Kind stp_d{"stp d", 0x6d00'0000U, Form::pair, 8, 16, false, false};
constexpr Kind stp_q{"stp q", 0xad00'0000U, Form::pair, 16, 32, false, false};

[[nodiscard]] constexpr std::uint32_t op(const Kind &kind, unsigned rt,
	unsigned rn, std::int64_t offset) noexcept
{
	return encode(kind, rt, rn, offset);
}

/* ADD Xd, Xn, #imm (31 is SP for both), and ADD Wd, Wn, #imm. */
[[nodiscard]] constexpr std::uint32_t add(unsigned rd, unsigned rn,
	unsigned immediate) noexcept
{
	return 0x9100'0000U | (immediate << 10) | (rn << 5) | rd;
}
[[nodiscard]] constexpr std::uint32_t add_w(unsigned rd, unsigned rn,
	unsigned immediate) noexcept
{
	return 0x1100'0000U | (immediate << 10) | (rn << 5) | rd;
}
[[nodiscard]] constexpr std::uint32_t sub_sp(unsigned immediate) noexcept
{
	return 0xd100'03ffU | (immediate << 10);
}
/* MOV Xd, Xm (ORR Xd, XZR, Xm). */
[[nodiscard]] constexpr std::uint32_t mov(unsigned rd, unsigned rm) noexcept
{
	return 0xaa00'03e0U | (rm << 16) | rd;
}
/* LDR Xt, [Xn, #imm]! and LDR Xt, [Xn], #imm. */
[[nodiscard]] constexpr std::uint32_t ldr_pre(unsigned rt, unsigned rn,
	int immediate) noexcept
{
	return 0xf840'0c00U | field(immediate, 9, 12) | (rn << 5) | rt;
}
[[nodiscard]] constexpr std::uint32_t ldr_post(unsigned rt, unsigned rn,
	int immediate) noexcept
{
	return 0xf840'0400U | field(immediate, 9, 12) | (rn << 5) | rt;
}
/* ADRP Xd of `target`'s page, at `pc`. */
[[nodiscard]] constexpr std::uint32_t adrp(unsigned rd, std::uint64_t pc,
	std::uint64_t target) noexcept
{
	const auto pages_away = static_cast<std::int64_t>(target >> 12) -
		static_cast<std::int64_t>(pc >> 12);
	return 0x9000'0000U | field(pages_away & 3, 2, 29) |
		field(pages_away >> 2, 19, 5) | rd;
}

/* The registers every case starts with, before its own. */
constexpr std::uint64_t base_x2 = high + 0x200;
constexpr std::uint64_t base_x6 = high + 0x800;
constexpr std::uint64_t base_x7 = high + 0x900;
constexpr std::uint64_t base_x9 = high + 0x300;
constexpr std::uint64_t stack = high + 0x400;

/* A base carrying this tag misses the TLB's inline probe, which compares the
 * whole page number, and the miss helper resolves it through the entry of
 * its untagged page (memory::Tlb::translate_tagged): a miss the helper
 * fills, as it fills one the software MMU resolves, with no MMU behind the
 * test. The CPU state leaves TBI off, so no entry strips the tag first. */
constexpr std::uint64_t tag = 0x5aULL << 56;

[[nodiscard]] arm64::CpuState initial(
	std::span<const std::pair<unsigned, std::uint64_t>> set)
{
	arm64::CpuState cpu{};
	for (unsigned index = 0; index < 31; ++index)
		cpu.registers[index] =
			0x5a5a'0000'0000'0000ULL | (index << 8) | index;
	cpu.registers[2] = base_x2;
	cpu.registers[6] = base_x6;
	cpu.registers[7] = base_x7;
	cpu.registers[9] = base_x9;
	cpu.sp = stack;
	for (unsigned n = 0; n < cpu.vectors.size(); ++n)
		for (unsigned byte = 0; byte < 16; ++byte)
			cpu.vectors[n].bytes[byte] = static_cast<std::byte>(
				(n * 16U + byte) ^ 0xa5U);
	for (const auto &[reg, value] : set) {
		if (reg == sp)
			cpu.sp = value;
		else
			cpu.registers[reg] = value;
	}
	cpu.pc = code_pc;
	return cpu;
}

struct Outcome final {
	::uml::cpu::EngineEvent event{};
	arm64::CpuState cpu{};
	std::size_t executed{};
	std::uint64_t fault_address{};
	std::vector<std::byte> memory;
};

[[nodiscard]] Outcome run(Memory &memory, const gadgets::Program &program,
	const arm64::CpuState &start, const std::vector<std::byte> &image,
	const Holding &holding, bool tagged)
{
	memory.load(image);
	auto tlb = tlb_for(memory, holding);
	tlb->tagged_addresses = tagged;
	auto cpu = start;
	std::size_t executed = 0;
	const auto event = gadgets::execute(program, cpu, *tlb, executed);
	gadgets::resolve_fault_position(program, cpu.pc);
	return {event, cpu, executed, tlb->fault_address, memory.image()};
}

enum class Ends : std::uint8_t { either, complete, fault };

struct Case final {
	std::string name;
	std::vector<std::uint32_t> code;
	std::vector<std::pair<unsigned, std::uint64_t>> set{};
	Holding holding{all_written};
	int leaders{-1};
	int followers{-1};
	Ends ends{Ends::complete};
	std::size_t fault_at{};
	/* A mask some leader, or some follower, must carry. */
	unsigned leader_mask{};
	unsigned follower_mask{};
	/* After a fault: the holding and registers the resumed block runs
	 * with. */
	Holding resume{all_written};
	std::vector<std::pair<unsigned, std::uint64_t>> resume_set{};
	/* Doublewords the memory holds at these guest addresses. */
	std::vector<std::pair<std::uint64_t, std::uint64_t>> poke{};
	/* The TLB resolves tagged addresses (see `tag`). */
	bool tagged{};
};

void same(const std::string &name, const Outcome &got, const Outcome &want)
{
	if (got.event != want.event)
		fail(name, "a different event");
	if (got.cpu.pc != want.cpu.pc)
		fail(name, "a different PC");
	if (got.executed != want.executed)
		fail(name, "a different retired count");
	if (want.event == ::uml::cpu::EngineEvent::memory_fault &&
	    got.fault_address != want.fault_address)
		fail(name, "a different fault address");
	for (unsigned index = 0; index < 31; ++index)
		if (got.cpu.registers[index] != want.cpu.registers[index]) {
			std::fprintf(stderr, "x%u: %#llx, established %#llx\n",
				index,
				static_cast<unsigned long long>(
					got.cpu.registers[index]),
				static_cast<unsigned long long>(
					want.cpu.registers[index]));
			fail(name, "a different register");
		}
	if (got.cpu.sp != want.cpu.sp)
		fail(name, "a different SP");
	if (got.cpu.nzcv != want.cpu.nzcv)
		fail(name, "different flags");
	for (unsigned n = 0; n < got.cpu.vectors.size(); ++n)
		if (got.cpu.vectors[n].bytes != want.cpu.vectors[n].bytes)
			fail(name, "a different SIMD&FP register");
	if (got.memory != want.memory)
		fail(name, "different memory");
}

/* Builds `code` at `pc` with the pass on and off, runs both, and returns
 * the outcome. */
[[nodiscard]] Outcome run_both(Memory &memory, const std::string &name,
	std::uint64_t pc, std::span<const std::uint32_t> code,
	const arm64::CpuState &start, const std::vector<std::byte> &image,
	const Holding &holding, bool tagged, Census *counted)
{
	std::vector<std::uint32_t> block{code.begin(), code.end()};
	block.push_back(svc);
	const auto built = build(pc, block, true);
	const auto reference = build(pc, block, false);
	if (!built.complete || !reference.complete)
		fail(name, "the block did not build");
	const auto counts = census(built.program);
	if (counted != nullptr)
		*counted = counts;
	{
		const auto none = census(reference.program);
		require(none.leaders == 0 && none.followers == 0);
	}
	rewrites_only_its_own(name, built.program, reference.program);
#if UML_GADGET_EXECUTION_HISTOGRAM
	counted_in_pairs(name, built.program);
	counted_in_pairs(name + ", the pass off", reference.program);
#endif
	auto cpu = start;
	cpu.pc = pc;
	const auto got = run(memory, built.program, cpu, image, holding,
		tagged);
	const auto want = run(memory, reference.program, cpu, image, holding,
		tagged);
	same(name, got, want);
	return got;
}

void check(Memory &memory, const Case &item)
{
	Census counts;
	const auto start = initial(item.set);
	auto image = pattern();
	for (const auto &[address, value] : item.poke) {
		require(address >= low && address + 8 <= low + image.size());
		std::memcpy(image.data() + (address - low), &value,
			sizeof value);
	}
	const auto outcome = run_both(memory, item.name, code_pc, item.code,
		start, image, item.holding, item.tagged, &counts);
	if (item.leaders >= 0 && counts.leaders != item.leaders) {
		std::fprintf(stderr, "%d leaders, %d followers\n",
			counts.leaders, counts.followers);
		fail(item.name, "a different number of leaders");
	}
	if (item.followers >= 0 && counts.followers != item.followers) {
		std::fprintf(stderr, "%d leaders, %d followers\n",
			counts.leaders, counts.followers);
		fail(item.name, "a different number of followers");
	}
	if (item.leader_mask != 0 &&
	    (counts.leader_masks & (1U << item.leader_mask)) == 0)
		fail(item.name, "no leader carries the forwarded mask");
	if (item.follower_mask != 0 &&
	    (counts.follower_masks & (1U << item.follower_mask)) == 0)
		fail(item.name, "no follower carries the forwarded mask");
	switch (item.ends) {
	case Ends::either:
		return;
	case Ends::complete:
		if (outcome.event != ::uml::cpu::EngineEvent::system_call)
			fail(item.name, "the block did not reach its SVC");
		return;
	case Ends::fault:
		break;
	}
	if (outcome.event != ::uml::cpu::EngineEvent::memory_fault)
		fail(item.name, "no memory fault");
	const auto pc = code_pc + item.fault_at * sizeof(std::uint32_t);
	if (outcome.cpu.pc != pc)
		fail(item.name, "the fault is not at its instruction");

	/* Resume at the faulting instruction, from the state and memory the
	 * fault left, once its cause is gone: a new block, in which the first
	 * transfer through each base translates afresh (census() requires a
	 * leader before any follower). */
	auto resumed = outcome.cpu;
	for (const auto &[reg, value] : item.resume_set) {
		if (reg == sp)
			resumed.sp = value;
		else
			resumed.registers[reg] = value;
	}
	const auto rest = std::span{item.code}.subspan(item.fault_at);
	const auto after = run_both(memory, item.name + ", resumed", pc, rest,
		resumed, outcome.memory, item.resume, item.tagged, nullptr);
	if (after.event != ::uml::cpu::EngineEvent::system_call)
		fail(item.name, "the resumed block did not reach its SVC");
}

[[nodiscard]] std::vector<Case> family_cases()
{
	using C = std::vector<std::uint32_t>;
	return {
		{"in-place loads", C{op(ldr_x, 1, 2, 0), op(ldr_x, 3, 2, 8),
			op(ldr_w, 4, 2, 20)}, {}, all_written, 1, 2},
		{"in-place stores", C{op(str_x, 1, 2, 0), op(str_x, 3, 2, 16),
			op(str_w, 4, 2, 28)}, {}, all_written, 1, 2},
		{"word loads through SP", C{op(ldr_w, 1, sp, 32),
			op(ldr_w, 3, sp, 36)}, {}, all_written, 1, 1},
		{"word stores through SP", C{op(str_w, 1, sp, 32),
			op(str_w, 3, sp, 40)}, {}, all_written, 1, 1},
		/* A doubleword transfer through SP is a run element. */
		{"doublewords through SP", C{op(ldr_x, 1, sp, 32),
			op(ldr_x, 3, sp, 40), add(9, 1, 0)}, {}, all_written,
			0, 0},
		{"a leader with its base forwarded", C{add(2, 6, 16),
			op(ldr_x, 1, 2, 0), op(ldr_x, 3, 2, 8)}, {}, all_written,
			1, 1, Ends::complete, 0, 1},
		{"a store follower with its data forwarded", C{
			op(str_x, 3, 2, 0), add(1, 5, 1), op(str_x, 1, 2, 8)},
			{}, all_written, 1, 1, Ends::complete, 0, 0, 2},
		{"a store leader with base and data forwarded", C{
			add(2, 6, 8), op(str_x, 2, 2, 0), op(str_x, 3, 2, 8)},
			{}, all_written, 1, 1, Ends::complete, 0, 3},
		{"a word store through SP with its data forwarded", C{
			op(str_w, 3, sp, 32), add(1, 5, 1),
			op(str_w, 1, sp, 36)}, {}, all_written, 1, 1,
			Ends::complete, 0, 0, 2},
		{"word loads through SP after SP is set", C{add(sp, 6, 0),
			op(ldr_w, 1, sp, 4), op(ldr_w, 3, sp, 8)}, {},
			all_written, 1, 1},
		{"XZR stores", C{op(str_wzr, 0, 2, 0), op(str_wzr, 0, 2, 4),
			op(str_xzr, 0, 2, 8)}, {}, all_written, 1, 2},
		{"XZR word stores through SP", C{op(str_wzr, 0, sp, 4),
			op(str_wzr, 0, sp, 8)}, {}, all_written, 1, 1},
		{"XZR loads", C{op(ldr_wzr, 0, 2, 0), op(ldr_xzr, 0, 2, 8)}, {},
			all_written, 1, 1},
		{"narrow loads", C{op(ldrb, 1, 2, 0), op(ldrh, 3, 2, 2),
			op(ldrsb_x, 4, 2, 5), op(ldrsh_w, 5, 2, 6),
			op(ldrsw, 8, 2, 8)}, {}, all_written, 1, 4},
		{"narrow stores", C{op(strb, 1, 2, 0), op(strh, 3, 2, 2),
			op(strb_zr, 0, 2, 5), op(strh, 4, 2, 6)}, {},
			all_written, 1, 3},
		{"narrow loads through SP", C{op(ldrb, 1, sp, 3),
			op(ldrh, 3, sp, 6), op(ldrsb_w, 4, sp, 9)}, {},
			all_written, 1, 2},
		{"a narrow leader with its base forwarded", C{add(2, 6, 16),
			op(ldrb, 1, 2, 0), op(ldrb, 3, 2, 1)}, {}, all_written,
			1, 1, Ends::complete, 0, 1},
		{"unscaled loads", C{op(ldur_x, 1, 2, -8),
			op(ldur_w, 3, 2, -12), op(ldurh, 4, 2, -14),
			op(ldursb_x, 5, 2, -15), op(ldur_x, 8, 2, 3)}, {},
			all_written, 1, 4},
		{"unscaled stores", C{op(stur_x, 1, 2, -24),
			op(stur_w, 3, 2, -28), op(sturh, 4, 2, -30),
			op(sturb_zr, 0, 2, -31)}, {}, all_written, 1, 3},
		{"SIMD&FP loads", C{op(ldr_q, 1, 2, 0), op(ldr_d, 3, 2, 16),
			op(ldr_s, 4, 2, 28), op(ldr_h, 5, 2, 30),
			op(ldr_b, 6, 2, 31), op(ldur_q, 7, 2, -16),
			op(ldur_s, 8, 2, 33)}, {}, all_written, 1, 6},
		{"SIMD&FP stores", C{op(str_q, 1, 2, 32), op(str_d, 3, 2, 48),
			op(str_s, 4, 2, 56), op(str_h, 5, 2, 60),
			op(str_b, 6, 2, 62), op(stur_q, 7, 2, -32),
			op(stur_d, 8, 2, 65)}, {}, all_written, 1, 6},
		{"SIMD&FP through SP", C{op(ldr_q, 1, sp, 32),
			op(ldr_q, 3, sp, 48), op(ldur_q, 4, sp, -16)}, {},
			all_written, 1, 2},
		{"a SIMD&FP leader with its base forwarded", C{add(2, 6, 16),
			op(ldr_q, 1, 2, 0), op(ldr_q, 3, 2, 16)}, {},
			all_written, 1, 1, Ends::complete, 0, 1},
		{"pairs", C{op(ldp_x, 1, 2, 0), op(ldp_x, 3, 2, 16),
			op(ldp_w, 8, 2, 32), op(ldpsw, 10, 2, 40),
			op(ldnp_x, 12, 2, 48)}, {}, all_written, 1, 4},
		{"pair stores", C{op(stp_x, 1, 2, 64), op(stp_w, 3, 2, 80),
			op(stp_xzr, 0, 2, 96), op(stp_w, 10, 2, 112)}, {},
			all_written, 1, 3},
		{"SIMD&FP pairs", C{op(ldp_q, 1, 2, 0), op(ldp_d, 3, 2, 32),
			op(ldp_s, 6, 2, 48), op(stp_q, 8, 2, 64)}, {},
			all_written, 1, 2},
		{"SIMD&FP pairs through SP", C{op(ldp_q, 1, sp, 0),
			op(ldp_q, 3, sp, 32), op(stp_q, 5, sp, 64),
			op(stp_d, 7, sp, 96)}, {}, all_written, 2, 2},
		{"across families", C{op(ldr_x, 1, 2, 0), op(ldr_q, 1, 2, 16),
			op(ldp_x, 3, 2, 32), op(ldrb, 5, 2, 48),
			op(ldur_w, 8, 2, -4), op(ldp_s, 10, 2, 52)}, {},
			all_written, 1, 5},
		/* One chain at a time: the second base's transfers are not
		 * followed once the first base's chain has taken x25 after
		 * them. */
		{"interleaved bases", C{op(ldr_x, 1, 2, 0), op(ldr_x, 3, 6, 0),
			op(ldr_x, 4, 2, 8), op(ldr_x, 5, 6, 8)}, {}, all_written,
			1, 1},
		{"directions apart", C{op(ldr_x, 1, 2, 0), op(str_x, 3, 2, 8),
			op(ldr_x, 4, 2, 16), op(str_x, 5, 2, 24)}, {},
			all_written, 1, 1},
		{"a row of moves that leaves the base", C{op(ldr_x, 1, 2, 0),
			mov(10, 9), mov(11, 12), op(ldr_x, 3, 2, 8)}, {},
			all_written, 1, 1},
		{"a prologue run that leaves SP", C{op(ldr_w, 1, sp, 32),
			op(str_x, 19, sp, 8), op(stp_x, 20, sp, 16),
			op(ldr_w, 3, sp, 36)}, {}, all_written, 1, 1},
		{"writeback through another base", C{op(ldr_x, 1, 2, 0),
			ldr_post(5, 6, 8), op(ldr_x, 3, 2, 8)}, {}, all_written,
			1, 1},
		{"a follower that loads its own base", C{op(ldr_x, 1, 2, 0),
			op(ldr_x, 2, 2, 8), op(ldr_x, 3, 2, 16)},
			{{2, base_x2}}, all_written, 1, 1, Ends::either},
	};
}

/* The base written between a transfer and the next through it: never a
 * follower. Each writer is followed by the transfer directly, and with an
 * instruction between them, which keeps the base from being forwarded into
 * the transfer, which alone would rule a follower out. The writers that
 * load the base find a mapped address at x6. */
[[nodiscard]] std::vector<Case> kill_cases()
{
	using C = std::vector<std::uint32_t>;
	struct Writer final {
		const char *name;
		C words;
	};
	const std::vector<Writer> writers{
		{"a W write", C{add_w(2, 2, 8)}},
		{"writeback through the base", C{ldr_pre(5, 2, 16)}},
		{"post-index through the base", C{ldr_post(5, 2, 8)}},
		{"a load into the base", C{op(ldr_x, 2, 6, 0)}},
		{"a word load into the base", C{op(ldr_w, 2, 6, 0)}},
		{"an unscaled load into the base", C{op(ldur_x, 2, 6, -8)}},
		{"a pair into the base", C{0xa940'0000U | (2U << 10) |
			(6U << 5) | 7U}},
		{"LDADD into the base", C{0xf821'00c2U}},
		/* Bits 23:22 of an atomic are A:R, which a PRFM's opc
		 * shares: the acquire forms of the X atomics and LDAPR X sit
		 * where size 3 and opc 2 would be. The W forms are the
		 * control. */
		{"LDADDA into the base", C{0xf8a5'00c2U}},
		{"SWPA into the base", C{0xf8a5'80c2U}},
		{"LDAPR into the base", C{0xf8bf'c0c2U}},
		{"LDADDA W into the base", C{0xb8a5'00c2U}},
		{"LDXR into the base", C{0xc85f'7cc2U}},
		{"FMOV into the base", C{0x9e66'0002U}},
		{"UMOV into the base", C{0x0e04'3c02U}},
		{"MRS into the base", C{0xd53b'd042U}},
		{"CSET into the base", C{0x1a9f'17e2U}},
		{"MOVZ into the base", C{0xd280'0002U | (0x7000U << 5)}},
		{"a fused ADRP and ADD", C{adrp(2, code_pc + 4, high),
			add(2, 2, 0x200)}},
		{"a row of moves into the base", C{mov(2, 9), mov(10, 11)}},
	};
	std::vector<Case> out;
	for (const auto &writer : writers)
		for (const bool spaced : {false, true}) {
			C code{op(ldr_x, 1, 2, 0)};
			code.insert(code.end(), writer.words.begin(),
				writer.words.end());
			if (spaced)
				code.push_back(add(10, 10, 1));
			code.push_back(op(ldr_x, 3, 2, 8));
			out.push_back({.name = std::string{"the base written by "} +
					writer.name +
					(spaced ? ", one instruction before" : ""),
				.code = code, .leaders = 0, .followers = 0,
				.ends = Ends::either,
				.poke = {{base_x6, high + 0x600}}});
		}
	out.push_back({"SP written by a run head's SUB SP",
		C{op(ldr_w, 1, sp, 32), sub_sp(32), op(str_x, 19, sp, 8),
		  op(ldr_w, 3, sp, 36)}, {}, all_written, 0, 0});
	out.push_back({"SP written by ADD SP",
		C{op(ldr_w, 1, sp, 32), add(sp, sp, 16), add(10, 10, 1),
		  op(ldr_w, 3, sp, 36)},
		{}, all_written, 0, 0});
	out.push_back({"the base loaded by its own transfer",
		C{op(ldr_x, 2, 2, 0), op(ldr_x, 3, 2, 8)}, {}, all_written, 0, 0,
		Ends::either});
	return out;
}

/* A fault between a leader and its follower, and a follower's own. */
[[nodiscard]] std::vector<Case> fault_cases()
{
	using C = std::vector<std::uint32_t>;
	const std::uint64_t end = beyond;
	return {
		{"a miss between a leader and its follower",
			C{op(ldr_x, 1, 2, 0), op(ldr_x, 5, 6, 0),
			  op(ldr_x, 3, 2, 8)}, {{6, beyond + 0x10}},
			beyond_absent, 1, 1, Ends::fault, 1},
		{"a page crossing between a leader and its follower",
			C{op(ldr_x, 1, 2, 0), op(ldr_x, 5, 6, 0),
			  op(ldr_x, 3, 2, 8)}, {{6, end - 4}}, beyond_absent, 1,
			1, Ends::fault, 1},
		{"a misaligned SP between a leader and its follower",
			C{op(ldr_x, 1, 2, 0), op(ldr_w, 5, sp, 0),
			  op(ldr_x, 3, 2, 8)}, {{sp, stack + 8}}, all_written,
			1, 1, Ends::fault, 1, 0, 0, all_written, {{sp, stack}}},
		{"stores around a miss", C{op(str_x, 1, 2, 0),
			op(str_x, 5, 6, 0), op(str_x, 3, 2, 8)},
			{{6, beyond + 0x10}}, beyond_absent, 1, 1, Ends::fault,
			1},
		{"a pair across the page end between", C{op(ldr_x, 1, 2, 0),
			op(ldp_x, 5, 6, 0), op(ldr_x, 3, 2, 8)}, {{6, end - 8}},
			beyond_absent, 1, 1, Ends::fault, 1},
		{"a SIMD&FP pair across the page end between",
			C{op(ldr_q, 1, 2, 0), op(ldp_q, 3, 6, 0),
			  op(ldr_q, 5, 2, 16)}, {{6, end - 16}}, beyond_absent,
			1, 1, Ends::fault, 1},
		{"a follower's miss", C{op(ldr_x, 1, 2, 0), op(ldr_x, 3, 2, 8)},
			{{2, end - 8}}, beyond_absent, 1, 1, Ends::fault, 1},
		{"a follower across the page end", C{op(ldr_x, 1, 2, 0),
			op(ldr_x, 3, 2, 8)}, {{2, end - 12}}, beyond_absent, 1,
			1, Ends::fault, 1},
		{"a store follower across the page end", C{op(str_x, 1, 2, 0),
			op(str_x, 3, 2, 8)}, {{2, end - 12}}, beyond_absent, 1,
			1, Ends::fault, 1},
		{"a SIMD&FP pair follower across the page end",
			C{op(ldp_q, 1, 2, 0), op(ldp_q, 3, 2, 32)},
			{{2, end - 48}}, beyond_absent, 1, 1, Ends::fault, 1},
		{"a store follower onto a page held for reading",
			C{op(str_x, 1, 2, 0), op(str_x, 3, 2, 8)},
			{{2, end - 12}}, beyond_read, 1, 1, Ends::fault, 1},
		{"a follower below its page", C{op(ldr_x, 1, 2, 16),
			op(ldr_x, 3, 2, 0)}, {{2, high - 4}}, high_only, 1, 1,
			Ends::fault, 1},
		{"loads on a page held for reading", C{op(ldr_x, 1, 2, 0),
			op(ldr_x, 3, 2, 8)}, {}, high_read, 1, 1},
		{"a store leader on a page held for reading",
			C{op(str_x, 1, 2, 0), op(str_x, 3, 2, 8)}, {}, high_read,
			1, 1, Ends::fault, 0},
		/* The all-ones address is what the engine's caches hold in an
		 * empty entry; a base there must miss like any unmapped one. */
		{"a leader at the all-ones address", C{op(ldr_x, 1, 2, 0),
			op(ldr_x, 3, 2, 8)}, {{2, ~std::uint64_t{}}}, all_written,
			1, 1, Ends::fault, 0, 0, 0, all_written, {{2, low}}},
		{"a store leader at the all-ones address", C{op(str_x, 1, 2, 0),
			op(str_x, 3, 2, 8)}, {{2, ~std::uint64_t{}}}, all_written,
			1, 1, Ends::fault, 0, 0, 0, all_written, {{2, low}}},
		{"a leader ending at the top of the address space",
			C{op(ldr_x, 1, 2, 0), op(ldr_x, 3, 2, 8)},
			{{2, ~std::uint64_t{} - 7}}, all_written, 1, 1,
			Ends::fault, 0, 0, 0, all_written, {{2, low}}},
	};
}

/* For each access shape, a follower whose access ends at its page's last
 * byte and one that starts at its first, one byte past each, and a
 * displacement just outside the follower's field. The pages around `high`
 * are not held, so a follower that left its page without translating would
 * not fault where the established block does. */
[[nodiscard]] std::vector<Case> boundary_cases()
{
	using C = std::vector<std::uint32_t>;
	constexpr Kind kinds[] = {
		ldr_x, ldr_w, str_x, str_w, str_wzr, str_xzr, ldr_xzr, ldrb,
		ldrh, ldrsb_x, ldrsh_w, ldrsw, strb, strh, ldur_x, ldur_w,
		ldurh, stur_x, sturh, ldr_b, ldr_h, ldr_s, ldr_d, ldr_q, str_b,
		str_q, ldur_q, stur_q, ldp_x, ldp_w, ldpsw, stp_x, stp_w,
		ldp_s, ldp_d, ldp_q, stp_s, stp_q,
	};
	std::vector<Case> out;
	for (const auto &kind : kinds) {
		const std::string name{kind.name};
		const std::int64_t bytes = kind.bytes;
		const std::int64_t unit = kind.scale;
		/* The farthest displacement either way at which the access
		 * still fits the leader's page. */
		const std::int64_t forward = kind.wide ? page_size - bytes :
			127 / unit * unit;
		const std::int64_t backward = kind.wide ? page_size - bytes :
			128;
		const auto pair_of = [&](std::int64_t leader,
					 std::int64_t follower) {
			return C{op(kind, 1, 2, leader), op(kind, 3, 2, follower)};
		};
		const auto top = beyond - forward - bytes;
		out.push_back({name + ", to the page's last byte",
			pair_of(0, forward), {{2, top}}, high_only, 1, 1});
		out.push_back({name + ", one byte past the page's end",
			pair_of(0, forward), {{2, top + 1}}, high_only, 1, 1,
			Ends::fault, 1});
		out.push_back({name + ", from the page's first byte",
			pair_of(backward, 0), {{2, high}}, high_only, 1, 1});
		out.push_back({name + ", one byte before the page",
			pair_of(backward, 0), {{2, high - 1}}, high_only, 1, 1,
			Ends::fault, 1});
		if (kind.wide) {
			out.push_back({name + ", a page away",
				pair_of(0, page_size), {{2, low}}, all_written,
				0, 0});
			continue;
		}
		out.push_back({name + ", a displacement of 128",
			pair_of(0, 128), {{2, high}}, all_written, 0, 0});
		if (unit == 1 || kind.form == Form::unscaled)
			out.push_back({name + ", a displacement of -129",
				pair_of(129, 0), {{2, high}}, all_written, 0, 0});
	}
	return out;
}


/* LDAPR and the acquire LSE atomics load the base from [x6], which holds an
 * address on the same page, between a transfer and the next through the
 * base. A follower would read, or write, through the base before it. */
[[nodiscard]] std::vector<Case> atomic_cases()
{
	using C = std::vector<std::uint32_t>;
	struct Writer final {
		const char *name;
		std::uint32_t word;
	};
	constexpr Writer writers[] = {
		{"LDAPR", 0xf8bf'c0c2U},	// ldapr x2, [x6]
		{"LDADDA", 0xf8a5'00c2U},	// ldadda x5, x2, [x6]
		{"SWPA", 0xf8a5'80c2U},		// swpa x5, x2, [x6]
		{"LDAPR W", 0xb8bf'c0c2U},	// ldapr w2, [x6]
	};
	std::vector<Case> out;
	for (const auto &writer : writers)
		for (const auto &kind : {ldr_x, str_x})
			out.push_back({.name = std::string{kind.name} +
					" around " + writer.name +
					" into the base",
				.code = C{op(kind, 1, 2, 0), writer.word,
					op(kind, 3, 2, 8)},
				.leaders = 0, .followers = 0,
				.poke = {{base_x6, high + 0x600}}});
	return out;
}

using Setting = std::vector<std::pair<unsigned, std::uint64_t>>;

[[nodiscard]] std::uint64_t initially(unsigned reg)
{
	return reg == sp ? initial({}).sp : initial({}).registers[reg];
}

/* A block whose transfer at `at` takes the path under test: its access
 * address is register `from`, before the block, plus `reach`. */
struct Shape final {
	std::string name;
	std::vector<std::uint32_t> code;
	std::size_t at;
	unsigned from;
	std::int64_t reach;
	unsigned bytes;
	bool store;
	bool through_sp;
	int followers;
	unsigned leader_mask;
	unsigned follower_mask;
};

/* `from` set so the transfer at `at` accesses `address`. */
[[nodiscard]] Setting placing(const Shape &shape, std::uint64_t address)
{
	return {{shape.from, address - static_cast<std::uint64_t>(shape.reach)}};
}

/* The paths of a transfer that translates: a miss that faults and one the
 * helper fills, a crossing with the next page held and absent (an SP base
 * is aligned, so its word never crosses), a misaligned SP, and a store onto
 * a page held for reading. */
void translating(std::vector<Case> &out, const Shape &shape)
{
	const auto with = [&](std::string what, Setting set, Holding holding,
				  Ends ends, Setting resume_set = {},
				  bool tagged = false) {
		out.push_back({.name = shape.name + ": " + what,
			.code = shape.code, .set = std::move(set),
			.holding = holding, .leaders = 1,
			.followers = shape.followers, .ends = ends,
			.fault_at = shape.at, .leader_mask = shape.leader_mask,
			.follower_mask = shape.follower_mask,
			.resume_set = std::move(resume_set), .tagged = tagged});
	};
	const auto here = initially(shape.from) +
		static_cast<std::uint64_t>(shape.reach);
	/* Its offset from 16 bytes kept, so an SP base stays aligned. */
	with("a miss", placing(shape, beyond + 0x100 +
				    static_cast<std::uint64_t>(shape.reach & 15)),
		beyond_absent, Ends::fault);
	with("a miss the helper fills",
		{{shape.from, tag | initially(shape.from)}}, all_written,
		Ends::complete, {}, true);
	if (!shape.through_sp) {
		const auto crossing = beyond - shape.bytes / 2;
		with("across the page end", placing(shape, crossing),
			all_written, Ends::complete);
		with("across the page end onto a page not held",
			placing(shape, crossing), beyond_absent, Ends::fault);
	} else {
		with("a misaligned SP", placing(shape, here + 8), all_written,
			Ends::fault, {{sp, stack}});
	}
	if (shape.store)
		with("onto a page held for reading", {}, high_read,
			Ends::fault);
}

/* Every leader's miss, crossing and alignment paths. The in-place forms
 * that are not baked keep their own (engine/same_base.inc); every other
 * leader re-dispatches its word as its established entry, a forwarded
 * base still in x0. */
[[nodiscard]] std::vector<Case> leader_cases()
{
	using C = std::vector<std::uint32_t>;
	std::vector<Case> out;
	for (const auto &kind : {ldr_x, ldr_w, str_x, str_w}) {
		const std::string name{kind.name};
		const bool store = kind.opcode == str_x.opcode ||
			kind.opcode == str_w.opcode;
		const std::int64_t next = kind.bytes;
		/* Baked: the leader's word re-dispatched. */
		translating(out, {name + ", baked",
			C{op(kind, 1, 2, 0), op(kind, 3, 2, next)}, 0, 2, 0,
			kind.bytes, store, false, 1, 0, 0});
		/* f1: the base forwarded. */
		translating(out, {name + ", its base forwarded",
			C{add(2, 6, 16), op(kind, 1, 2, 0),
			  op(kind, 3, 2, next)}, 1, 6, 16, kind.bytes, store,
			false, 1, 1, 0});
		if (store) {
			/* f2: the data forwarded. */
			translating(out, {name + ", its data forwarded",
				C{add(1, 5, 1), op(kind, 1, 2, 0),
				  op(kind, 3, 2, next)}, 1, 2, 0, kind.bytes,
				true, false, 1, 2, 0});
			/* f3: both, the base stored through itself. */
			translating(out, {name + ", base and data forwarded",
				C{add(2, 6, 8), op(kind, 2, 2, 0),
				  op(kind, 3, 2, next)}, 1, 6, 8, kind.bytes,
				true, false, 1, 3, 0});
		}
	}
	/* The SP word forms: sp, sp_f1 and sp_f2. Nothing reaches sp_f3: a
	 * store's data register is never SP. */
	for (const auto &kind : {ldr_w, str_w}) {
		const std::string name{kind.name};
		const bool store = kind.opcode == str_w.opcode;
		translating(out, {name + " through SP",
			C{op(kind, 1, sp, 32), op(kind, 3, sp, 36)}, 0, sp, 32,
			4, store, true, 1, 0, 0});
		translating(out, {name + " through SP, SP forwarded",
			C{add(sp, 6, 0), op(kind, 1, sp, 4), op(kind, 3, sp, 8)},
			1, 6, 4, 4, store, true, 1, 1, 0});
		if (store)
			translating(out, {name + " through SP, data forwarded",
				C{add(1, 5, 1), op(kind, 1, sp, 32),
				  op(kind, 3, sp, 36)}, 1, sp, 32, 4, true, true,
				1, 2, 0});
	}
	/* The other families' leaders, which re-dispatch their word. */
	translating(out, {"ldrh", C{op(ldrh, 1, 2, 0), op(ldrh, 3, 2, 2)}, 0,
		2, 0, 2, false, false, 1, 0, 0});
	translating(out, {"strh", C{op(strh, 1, 2, 0), op(strh, 3, 2, 2)}, 0,
		2, 0, 2, true, false, 1, 0, 0});
	translating(out, {"ldrh through SP",
		C{op(ldrh, 1, sp, 32), op(ldrh, 3, sp, 34)}, 0, sp, 32, 2,
		false, true, 1, 0, 0});
	translating(out, {"ldur x", C{op(ldur_x, 1, 2, -8),
		op(ldur_x, 3, 2, 3)}, 0, 2, -8, 8, false, false, 1, 0, 0});
	translating(out, {"stur w", C{op(stur_w, 1, 2, -4),
		op(stur_w, 3, 2, 4)}, 0, 2, -4, 4, true, false, 1, 0, 0});
	translating(out, {"ldr q", C{op(ldr_q, 1, 2, 0), op(ldr_q, 3, 2, 16)},
		0, 2, 0, 16, false, false, 1, 0, 0});
	translating(out, {"str d", C{op(str_d, 1, 2, 0), op(str_d, 3, 2, 8)},
		0, 2, 0, 8, true, false, 1, 0, 0});
	translating(out, {"ldr q through SP",
		C{op(ldr_q, 1, sp, 0), op(ldr_q, 3, sp, 16)}, 0, sp, 0, 16,
		false, true, 1, 0, 0});
	translating(out, {"ldp x", C{op(ldp_x, 1, 2, 0), op(ldp_x, 4, 2, 16)},
		0, 2, 0, 16, false, false, 1, 0, 0});
	translating(out, {"stp w", C{op(stp_w, 1, 2, 0), op(stp_w, 3, 2, 8)},
		0, 2, 0, 8, true, false, 1, 0, 0});
	translating(out, {"ldp q", C{op(ldp_q, 1, 2, 0), op(ldp_q, 3, 2, 32)},
		0, 2, 0, 32, false, false, 1, 0, 0});
	translating(out, {"ldp q through SP",
		C{op(ldp_q, 1, sp, 0), op(ldp_q, 3, sp, 32)}, 0, sp, 0, 32,
		false, true, 1, 0, 0});
	/* Their forwarded-base leaders: the narrow and the SIMD&FP
	 * unsigned-offset ones; the unscaled and pair families have none. */
	translating(out, {"ldrh, its base forwarded",
		C{add(2, 6, 16), op(ldrh, 1, 2, 0), op(ldrh, 3, 2, 2)}, 1, 6,
		16, 2, false, false, 1, 1, 0});
	translating(out, {"ldr q, its base forwarded",
		C{add(2, 6, 16), op(ldr_q, 1, 2, 0), op(ldr_q, 3, 2, 16)}, 1,
		6, 16, 16, false, false, 1, 1, 0});
	translating(out, {"ldr q through SP, SP forwarded",
		C{add(sp, 6, 0), op(ldr_q, 1, sp, 0), op(ldr_q, 3, sp, 16)}, 1,
		6, 0, 16, false, true, 1, 1, 0});
	return out;
}

/* A follower whose access leaves its leader's page runs the leader's body,
 * which translates: for the followers of the forms that are not baked, the
 * body of a leader with its own miss and crossing paths. Their displacement
 * is a signed byte, so each leader sits 64 bytes below the page end. */
[[nodiscard]] std::vector<Case> fallback_cases()
{
	using C = std::vector<std::uint32_t>;
	std::vector<Case> out;
	const auto with = [&](const Shape &shape, std::string what,
				  Setting set, Holding holding, Ends ends,
				  bool tagged = false) {
		out.push_back({.name = shape.name + ": " + what,
			.code = shape.code, .set = std::move(set),
			.holding = holding, .leaders = 1, .followers = 1,
			.ends = ends, .fault_at = shape.at,
			.follower_mask = shape.follower_mask,
			.tagged = tagged});
	};
	const auto leaving = [&](const Shape &shape) {
		const auto first = placing(shape, beyond);
		with(shape, "leaves the page", first, all_written,
			Ends::complete);
		with(shape, "leaves the page and misses", first, beyond_absent,
			Ends::fault);
		with(shape, "leaves the page and misses, filled",
			{{shape.from, tag | first[0].second}}, all_written,
			Ends::complete, true);
		if (shape.store)
			with(shape, "leaves the page onto one held for reading",
				first, beyond_read, Ends::fault);
		if (shape.through_sp)
			return;
		const auto crossing = placing(shape, beyond - shape.bytes / 2);
		with(shape, "crosses the page end", crossing, all_written,
			Ends::complete);
		with(shape, "crosses the page end onto a page not held",
			crossing, beyond_absent, Ends::fault);
	};
	for (const auto &kind : {str_x, str_w})
		leaving({std::string{kind.name} + ", a follower with its data "
				 "forwarded",
			C{op(kind, 3, 2, 0), add(1, 5, 1), op(kind, 1, 2, 64)},
			2, 2, 64, kind.bytes, true, false, 1, 0, 2});
	for (const auto &kind : {ldr_w, str_w}) {
		const bool store = kind.opcode == str_w.opcode;
		leaving({std::string{kind.name} + ", a follower through SP",
			C{op(kind, 1, sp, 0), op(kind, 3, sp, 64)}, 1, sp, 64, 4,
			store, true, 1, 0, 0});
	}
	leaving({"str w, a follower through SP with its data forwarded",
		C{op(str_w, 3, sp, 0), add(1, 5, 1), op(str_w, 1, sp, 64)}, 2,
		sp, 64, 4, true, true, 1, 0, 2});
	return out;
}

/* A leader's or a follower's result read by the next instruction, which may
 * take it forwarded. */
[[nodiscard]] std::vector<Case> consumer_cases()
{
	using C = std::vector<std::uint32_t>;
	constexpr std::uint32_t add_v4 = 0x4e23'8464U; // add v4.16b, v3.16b, v3.16b
	return {
		{"a leader's result read next", C{op(ldr_x, 1, 2, 0),
			add(4, 1, 1), op(ldr_x, 3, 2, 8)}, {}, all_written, 1, 1},
		{"a follower's result read next", C{op(ldr_x, 1, 2, 0),
			op(ldr_x, 3, 2, 8), add(4, 3, 1)}, {}, all_written, 1, 1},
		{"a word follower's result read next", C{op(ldr_w, 1, 2, 0),
			op(ldr_w, 3, 2, 4), add_w(4, 3, 1)}, {}, all_written, 1,
			1},
		{"a follower through SP, its result read next",
			C{op(ldr_w, 1, sp, 32), op(ldr_w, 3, sp, 36),
			  add(4, 3, 1)}, {}, all_written, 1, 1},
		{"a follower through SP that leaves the page, its result "
		 "read next", C{op(ldr_w, 1, sp, 0), op(ldr_w, 3, sp, 64),
			add(4, 3, 1)}, {{sp, beyond - 64}}, all_written, 1, 1},
		{.name = "a follower through SP that leaves the page and "
			 "misses, filled, its result read next",
			.code = C{op(ldr_w, 1, sp, 0), op(ldr_w, 3, sp, 64),
				add(4, 3, 1)},
			.set = {{sp, tag | (beyond - 64)}}, .leaders = 1,
			.followers = 1, .tagged = true},
		{"a narrow follower's result read next", C{op(ldrb, 1, 2, 0),
			op(ldrsb_x, 3, 2, 1), add(4, 3, 1)}, {}, all_written, 1,
			1},
		{"an unscaled follower's result read next",
			C{op(ldur_x, 1, 2, -8), op(ldur_x, 3, 2, 3),
			  add(4, 3, 1)}, {}, all_written, 1, 1},
		{"a pair follower's result read next", C{op(ldp_x, 1, 2, 0),
			op(ldp_x, 3, 2, 16), add(9, 3, 1)}, {}, all_written, 1,
			1},
		{"a SIMD&FP follower's result read next", C{op(ldr_q, 1, 2, 0),
			op(ldr_q, 3, 2, 16), add_v4}, {}, all_written, 1, 1},
	};
}

} // namespace

int main()
{
	Memory memory;
	std::size_t count = 0;
	for (const auto &cases : {family_cases(), kill_cases(), atomic_cases(),
				  fault_cases(), boundary_cases(), leader_cases(),
				  fallback_cases(), consumer_cases()})
		for (const auto &item : cases) {
			check(memory, item);
			++count;
		}
	std::printf("arm64-gadget-same-base: %zu blocks: ok\n", count);
	return 0;
}
