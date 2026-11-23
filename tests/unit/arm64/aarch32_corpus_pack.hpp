// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The AArch32 instruction corpus pack: its memory map, the reader and the
 * decoded cases. The unit runner (aarch32_corpus_runner.cpp) checks the cases
 * against the decoder; an engine lane includes this header to drive each case
 * through its engine. tests/unit/arm64/aarch32_corpus/README.md owns the
 * format.
 *
 * A case starts from `recipes[case.recipe]` changed by `case.initial` (already
 * applied: `Case::initial` is the complete starting state), with the data
 * region filled by `data_fill(case.fill)` and then `case.data_patches`, the
 * read-only region holding `read_only_fill()`, and the program placed at
 * kCodeBase + `Case::origin`. `Case::final` is the complete state after the
 * program, and `Case::memory_patches` the data bytes it changed. code_image()
 * and data_before() lay a case out that way, and for_each_instruction() walks
 * its program through the decoder.
 */
#pragma once

#include "cpu/arm64/aarch32/decode.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uml::testing::aarch32_corpus {

/* The memory map every case runs in (tools/arm64-tests/a32_oracle.cpp). */
constexpr std::uint32_t kPackVersion = 2; /* 2: a case holds its program origin */
constexpr std::uint32_t kCodeBase = 0x4F000000;
constexpr std::uint32_t kCodeSize = 0x4000;
constexpr std::uint32_t kArmExit = kCodeBase + 0x800;   /* finishes in A32 state */
constexpr std::uint32_t kThumbExit = kCodeBase + 0x900; /* finishes in Thumb state */
/* A stub is `ldr pc, [pc, #-4]` (Thumb: `ldr.w pc, [pc]`, f8df f000) and then one of these
 * words; a program that loads a stub reads them. The stub at the end of a program is the same. */
constexpr std::uint32_t kArmEpilogue = kCodeBase + 0x3F00;
constexpr std::uint32_t kThumbEpilogue = kCodeBase + 0x3F08;
constexpr std::uint32_t kDataBase = 0x50000000;
constexpr std::uint32_t kDataSize = 0x8000;
constexpr std::uint32_t kReadOnlyBase = 0x50008000;
constexpr std::uint32_t kReadOnlySize = 0x4000;
constexpr unsigned kProfileBit = 2U; /* the 4 KiB profile */
constexpr unsigned kMaxHalfwords = 0x380;
constexpr unsigned kRegisters = 15;
constexpr unsigned kVectors = 32;

constexpr std::uint8_t kFlagThumb = 1;
constexpr std::uint8_t kFlagStraight = 2;
constexpr std::uint8_t kFlagExitThumb = 8;

constexpr unsigned kSigIll = 4;
constexpr unsigned kSigTrap = 5;
constexpr unsigned kSigBus = 7;
constexpr unsigned kSigFpe = 8;
constexpr unsigned kSigSegv = 11;
constexpr unsigned kEventSvc = 255;

/* Reading ---------------------------------------------------------------------- */

class Reader final {
public:
	explicit Reader(const std::vector<std::uint8_t> &data, std::size_t offset = 0)
		: data_(data), offset_(offset)
	{
	}

	template<typename T>
	T get()
	{
		require(offset_ + sizeof(T) <= data_.size(), "the pack is truncated");
		T value{};
		std::memcpy(&value, data_.data() + offset_, sizeof(T));
		offset_ += sizeof(T);
		return value;
	}

	std::string str16()
	{
		const auto size = get<std::uint16_t>();
		return take(size);
	}

	std::string str32()
	{
		const auto size = get<std::uint32_t>();
		return take(size);
	}

	std::string take(std::size_t size)
	{
		require(offset_ + size <= data_.size(), "the pack is truncated");
		std::string text(reinterpret_cast<const char *>(data_.data() + offset_), size);
		offset_ += size;
		return text;
	}

	void skip(std::size_t size)
	{
		require(offset_ + size <= data_.size(), "the pack is truncated");
		offset_ += size;
	}

	[[nodiscard]] bool done() const { return offset_ == data_.size(); }

private:
	const std::vector<std::uint8_t> &data_;
	std::size_t offset_;
};

struct State final {
	std::uint32_t r[kRegisters]{};
	std::uint32_t apsr{}, fpscr{}, tpidruro{}, tpidrurw{};
	std::uint64_t d[kVectors]{};
};

/* The oracle's wire layout: r0-r14, APSR, FPSCR, TPIDRURO, TPIDRURW, a word of
 * padding, D0-D31. */
inline State state_from_wire(Reader &reader)
{
	State state;
	for (auto &value : state.r)
		value = reader.get<std::uint32_t>();
	state.apsr = reader.get<std::uint32_t>();
	state.fpscr = reader.get<std::uint32_t>();
	state.tpidruro = reader.get<std::uint32_t>();
	state.tpidrurw = reader.get<std::uint32_t>();
	reader.skip(4);
	for (auto &value : state.d)
		value = reader.get<std::uint64_t>();
	return state;
}

inline State delta_into(Reader &reader, State state)
{
	const auto registers = reader.get<std::uint16_t>();
	const auto misc = reader.get<std::uint8_t>();
	const auto vectors = reader.get<std::uint32_t>();
	require(registers < 1U << kRegisters && misc < 16, "a state change names a register that does not exist");
	for (unsigned index = 0; index < kRegisters; ++index)
		if (registers >> index & 1U)
			state.r[index] = reader.get<std::uint32_t>();
	std::uint32_t *const named[] = {&state.apsr, &state.fpscr, &state.tpidruro, &state.tpidrurw};
	for (unsigned index = 0; index < 4; ++index)
		if (misc >> index & 1U)
			*named[index] = reader.get<std::uint32_t>();
	for (unsigned index = 0; index < kVectors; ++index)
		if (vectors >> index & 1U)
			state.d[index] = reader.get<std::uint64_t>();
	return state;
}

struct Patch final {
	std::uint32_t offset;
	std::uint32_t length;
	std::vector<std::uint8_t> bytes;
};

inline std::vector<Patch> read_patches(Reader &reader)
{
	std::vector<Patch> patches;
	std::uint32_t end = 0;
	const auto count = reader.get<std::uint16_t>();
	for (unsigned index = 0; index < count; ++index) {
		const auto offset = reader.get<std::uint32_t>();
		const auto length = reader.get<std::uint16_t>();
		require(length != 0 && offset >= end && offset + length <= kDataSize,
			"a data patch is outside the data region or out of order");
		const auto bytes = reader.take(length);
		patches.push_back({offset, length, std::vector<std::uint8_t>(bytes.begin(), bytes.end())});
		end = offset + length;
	}
	return patches;
}

struct Case final {
	std::string name, form;
	std::uint8_t flags{};
	std::uint16_t target{};
	std::uint16_t origin{}; /* byte offset of the program from kCodeBase */
	std::vector<std::uint16_t> halves;
	std::uint16_t recipe{};
	State initial, final;
	std::uint8_t fill{};
	std::vector<Patch> data_patches, memory_patches;
	std::uint8_t event{};
	std::uint16_t fault_instruction{}, retired{};
	std::uint32_t fault_address{};
};

struct Record final {
	std::string name, oracle;
	std::uint8_t profiles{};
	std::vector<Case> cases;
};

struct Pack final {
	std::vector<State> recipes;
	std::vector<Record> records;
};

inline Pack read_pack(const std::vector<std::uint8_t> &data)
{
	require(data.size() >= 24 && std::memcmp(data.data(), "UMLA32C\0", 8) == 0, "not an AArch32 corpus pack");
	Reader reader(data, 8);
	const auto version = reader.get<std::uint32_t>();
	reader.skip(4);
	const auto length = reader.get<std::uint64_t>();
	require(version == kPackVersion && data.size() == 24 + length, "the pack header or its version is wrong");
	reader.str32(); /* the memory map and record names, for tools */
	Pack pack;
	const auto recipes = reader.get<std::uint32_t>();
	for (std::uint32_t index = 0; index < recipes; ++index) {
		reader.str16();
		pack.recipes.push_back(state_from_wire(reader));
	}
	const auto records = reader.get<std::uint32_t>();
	for (std::uint32_t index = 0; index < records; ++index) {
		Record record;
		record.name = reader.str16();
		record.oracle = reader.str16();
		record.profiles = reader.get<std::uint8_t>();
		const auto cases = reader.get<std::uint32_t>();
		for (std::uint32_t number = 0; number < cases; ++number) {
			Case test;
			test.name = reader.str16();
			test.form = reader.str16();
			test.flags = reader.get<std::uint8_t>();
			test.target = reader.get<std::uint16_t>();
			test.origin = reader.get<std::uint16_t>();
			const auto halves = reader.get<std::uint16_t>();
			require(halves != 0 && halves <= kMaxHalfwords, "a program has a wrong length");
			for (unsigned half = 0; half < halves; ++half)
				test.halves.push_back(reader.get<std::uint16_t>());
			test.recipe = reader.get<std::uint16_t>();
			require(test.recipe < pack.recipes.size(), "a case names a state recipe that does not exist");
			test.initial = delta_into(reader, pack.recipes[test.recipe]);
			test.fill = reader.get<std::uint8_t>();
			test.data_patches = read_patches(reader);
			test.event = reader.get<std::uint8_t>();
			test.fault_instruction = reader.get<std::uint16_t>();
			test.retired = reader.get<std::uint16_t>();
			test.fault_address = reader.get<std::uint32_t>();
			test.final = delta_into(reader, test.initial);
			test.memory_patches = read_patches(reader);
			record.cases.push_back(std::move(test));
		}
		pack.records.push_back(std::move(record));
	}
	require(reader.done(), "the pack has trailing bytes");
	return pack;
}

inline std::vector<std::uint8_t> read_file(const char *path)
{
	std::ifstream input(path, std::ios::binary);
	require(input.is_open(), "the corpus pack is missing");
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/* The data region before a case runs: zero or the hash32 byte recipe. */
inline std::vector<std::uint8_t> data_fill(std::uint8_t fill)
{
	std::vector<std::uint8_t> data(kDataSize, 0);
	if (fill != 0)
		for (std::uint32_t index = 0; index < kDataSize; ++index)
			data[index] = static_cast<std::uint8_t>(((1U + index) * 0x9E3779B1U) >> 24);
	return data;
}

/* The read-only region, identical for every case. */
inline std::vector<std::uint8_t> read_only_fill()
{
	std::vector<std::uint8_t> data(kReadOnlySize);
	for (std::uint32_t index = 0; index < kReadOnlySize; ++index)
		data[index] = static_cast<std::uint8_t>(((0x5AU + index) * 0x9E3779B1U) >> 24);
	return data;
}

/* The code region as the oracle prepares it: the program at its origin, an
 * exit stub after it (a halfword NOP first when a Thumb program leaves the
 * stub's word unaligned, which `padded` reports), the stubs at the two fixed
 * exits, and the lane's own exit code, `epilogue_word`, behind the two
 * epilogues. A program at the end of the region has no room for a stub and
 * falls off it. */
inline std::vector<std::uint8_t> code_image(const Case &test, std::uint32_t epilogue_word, bool &padded)
{
	std::vector<std::uint8_t> code(kCodeSize, 0);
	const auto put16 = [&](std::uint32_t offset, std::uint16_t value) {
		std::memcpy(code.data() + offset, &value, 2);
	};
	const auto put32 = [&](std::uint32_t offset, std::uint32_t value) {
		std::memcpy(code.data() + offset, &value, 4);
	};
	const auto arm_stub = [&](std::uint32_t at) {
		put32(at, 0xe51ff004U); /* ldr pc, [pc, #-4] */
		put32(at + 4, kArmEpilogue);
	};
	const auto thumb_stub = [&](std::uint32_t at) {
		put16(at, 0xf8df); /* ldr.w pc, [pc, #0] */
		put16(at + 2, 0xf000);
		put32(at + 4, kThumbEpilogue);
	};
	for (std::size_t index = 0; index < test.halves.size(); ++index)
		put16(test.origin + static_cast<std::uint32_t>(2 * index), test.halves[index]);
	std::uint32_t end = test.origin + static_cast<std::uint32_t>(2 * test.halves.size());
	padded = false;
	if (test.flags & kFlagThumb) {
		if (end % 4 != 0 && end + 2 + 8 <= kCodeSize) {
			padded = true;
			put16(end, 0xbf00); /* nop, so the stub is word aligned */
			end += 2;
		}
		if (end + 8 <= kCodeSize)
			thumb_stub(end);
	} else if (end + 8 <= kCodeSize) {
		arm_stub(end);
	}
	arm_stub(kArmExit - kCodeBase);
	thumb_stub(kThumbExit - kCodeBase);
	put32(kArmEpilogue - kCodeBase, epilogue_word);
	put32(kThumbEpilogue - kCodeBase, epilogue_word);
	return code;
}

/* The data region a case starts from, and what it holds after the program. */
inline std::vector<std::uint8_t> data_before(const Case &test)
{
	auto data = data_fill(test.fill);
	for (const auto &patch : test.data_patches)
		std::copy(patch.bytes.begin(), patch.bytes.end(), data.begin() + patch.offset);
	return data;
}

inline std::vector<std::uint8_t> data_after(const Case &test, std::vector<std::uint8_t> data)
{
	for (const auto &patch : test.memory_patches)
		std::copy(patch.bytes.begin(), patch.bytes.end(), data.begin() + patch.offset);
	return data;
}

/* One instruction of a case's program, decoded. */
struct ProgramStep final {
	::uml::cpu::arm64::aarch32::Instruction instruction;
	/* The word as the decoder takes it: the A32 bits, or hw1 << 16 | hw2 (hw1 << 16 for a T16). */
	std::uint32_t word;
	/* The IT state before it. */
	::uml::cpu::arm64::aarch32::ItState before;
	/* The index of its first halfword. */
	unsigned half;
	bool thumb;

	/* The same word, against another profile. */
	[[nodiscard]] ::uml::cpu::arm64::aarch32::Instruction decode_at(
		const ::uml::cpu::arm64::aarch32::Profile &profile) const
	{
		return thumb ? ::uml::cpu::arm64::aarch32::decode_thumb(word, before, profile)
			     : ::uml::cpu::arm64::aarch32::decode_a32(word, profile);
	}
};

/* How a program's halfwords ended. */
enum class ProgramEnd { whole, split_word, cut_off };

/* Decodes the program in order against `profile`, carrying the IT state the
 * earlier instructions leave, and hands each instruction to `visit`, which
 * returns false to stop. Reports a program that ends in half an A32 word
 * (`split_word`) or in a Thumb-2 instruction cut off by the end of the code
 * region (`cut_off`); `whole` otherwise, and when `visit` stopped it. */
template<typename Visit>
ProgramEnd for_each_instruction(const Case &test, const ::uml::cpu::arm64::aarch32::Profile &profile, Visit &&visit)
{
	namespace dec = ::uml::cpu::arm64::aarch32;
	const bool thumb = test.flags & kFlagThumb;
	dec::ItState it;
	for (unsigned half = 0; half < test.halves.size();) {
		ProgramStep step{{}, 0, it, half, thumb};
		unsigned length = 4;
		if (!thumb) {
			if (half + 1 >= test.halves.size())
				return ProgramEnd::split_word;
			step.word = std::uint32_t{test.halves[half + 1]} << 16 | test.halves[half];
			step.instruction = dec::decode_a32(step.word, profile);
		} else {
			const std::uint16_t first = test.halves[half];
			length = dec::thumb_length(first);
			if (half + length / 2 > test.halves.size())
				return ProgramEnd::cut_off;
			step.word = length == 4 ? std::uint32_t{first} << 16 | test.halves[half + 1]
						: std::uint32_t{first} << 16;
			step.instruction = dec::decode_thumb(step.word, it, profile);
			it = step.instruction.next_it;
		}
		if (!visit(step))
			return ProgramEnd::whole;
		half += length / 2;
	}
	return ProgramEnd::whole;
}

/* Store-exclusives -------------------------------------------------------------- */

/* A store-exclusive's place in the data region: the bytes it writes when it
 * succeeds, and the instruction. */
struct ExclusiveStore final {
	std::uint32_t offset;
	std::uint32_t size;
	::uml::cpu::arm64::aarch32::Instruction instruction;
};

/* The store-exclusive that `test` targets, or nothing when its place cannot
 * be named from the pack alone. A store-exclusive is a form of STREX or STLEX
 * (the byte, halfword and doubleword ones add B, H or D), and writes 1, 2, 4
 * or 8 bytes at its base register plus an offset that only STREX_T1 has
 * (imm8 << 2, decoded into imm32). The decoded instruction must be the form
 * the case names, the base register must hold the same value before the
 * program and after it so that the final state names the address, and the
 * bytes must lie in the data region. */
[[nodiscard]] inline std::optional<ExclusiveStore> exclusive_store(const Case &test)
{
	namespace dec = ::uml::cpu::arm64::aarch32;
	struct Width final {
		std::string_view mnemonic;
		std::uint32_t size;
	};
	static constexpr Width kWidths[] = {
		{"STREX", 4}, {"STREXB", 1}, {"STREXH", 2}, {"STREXD", 8},
		{"STLEX", 4}, {"STLEXB", 1}, {"STLEXH", 2}, {"STLEXD", 8},
	};
	std::optional<dec::Instruction> target;
	unsigned index = 0;
	for_each_instruction(test, dec::Profile{}, [&](const ProgramStep &step) {
		if (index++ != test.target)
			return true;
		target = step.instruction;
		return false;
	});
	if (!target || target->status != dec::Status::ok || test.form != target->info().name)
		return std::nullopt;
	std::uint32_t size = 0;
	for (const auto &width : kWidths)
		if (width.mnemonic == target->info().mnemonic)
			size = width.size;
	const auto base = target->operands.n;
	if (size == 0 || base < 0 || static_cast<unsigned>(base) >= kRegisters ||
	    test.initial.r[base] != test.final.r[base])
		return std::nullopt;
	const std::uint32_t address = test.final.r[base] + static_cast<std::uint32_t>(target->operands.imm32);
	if (address < kDataBase || address - kDataBase > kDataSize - size)
		return std::nullopt;
	return ExclusiveStore{address - kDataBase, size, *target};
}

/* Whether the data region `actual` is what a lost reservation leaves of
 * `expected`, the region the pack has after the case: its store-exclusive
 * wrote nothing, so the bytes it would have written hold what the case
 * started with (`initial`), and every other byte, a spill the case makes
 * before the store included, is as `expected` has it. A store-exclusive that
 * reports failure and has written, and any other difference, are not a loss,
 * and nor is a case that is no store-exclusive. */
[[nodiscard]] inline bool lost_exclusive_data(const Case &test, const std::vector<std::uint8_t> &initial,
					      const std::vector<std::uint8_t> &expected,
					      const std::vector<std::uint8_t> &actual)
{
	const auto store = exclusive_store(test);
	if (!store || initial.size() != kDataSize || expected.size() != kDataSize || actual.size() != kDataSize)
		return false;
	for (std::uint32_t byte = 0; byte < kDataSize; ++byte) {
		const bool written = byte >= store->offset && byte - store->offset < store->size;
		if (actual[byte] != (written ? initial[byte] : expected[byte]))
			return false;
	}
	return true;
}

/* Whether an attempt at `test` is a store-exclusive that lost its reservation
 * and nothing else: the register the pack holds the status in (the decoded
 * destination) reads 1 where the pack has 0, every other register is as the
 * pack has it, and the data is what lost_exclusive_data() accepts. A case
 * whose store-exclusive was to fail, or a register other than the status that
 * differs, is not one. `registers` are the engine's r0-r14 whole, so that a
 * slot that is not zero-extended differs. The caller has already found
 * everything else (the event, the exit state, the retired count, the flags,
 * D0-D31, FPSCR and both thread registers) as the pack has it. */
[[nodiscard]] inline bool lost_exclusive_attempt(const Case &test, const std::uint64_t *registers,
						 const std::vector<std::uint8_t> &initial,
						 const std::vector<std::uint8_t> &expected,
						 const std::vector<std::uint8_t> &actual)
{
	const auto store = exclusive_store(test);
	if (!store)
		return false;
	const auto status = store->instruction.operands.d;
	if (status < 0 || static_cast<unsigned>(status) >= kRegisters || test.final.r[status] != 0)
		return false;
	for (unsigned index = 0; index < kRegisters; ++index)
		if (registers[index] != (index == static_cast<unsigned>(status) ? 1U : test.final.r[index]))
			return false;
	return lost_exclusive_data(test, initial, expected, actual);
}

/* Faults --inject makes an engine lane commit on purpose, in the first attempt
 * at each case of one shape, to prove that its handling of a lost reservation
 * refuses what is not one. A plan whose `retried` is set must be taken for a
 * lost reservation, retried and pass; any other must fail on that attempt. */
enum class Fault { lost, lost_and_register, lost_and_tpidruro, unwritten_and_register, failed_and_register, excess };

struct FaultPlan final {
	Fault fault;
	const char *name;
	const char *record;
	std::string_view shape; /* the end of the names of the cases it applies to */
	bool retried;
	std::uint32_t excess = 0; /* Fault::excess: instructions the engine retires beyond the pack's */
	bool persistent = false;  /* the fault is made on every attempt, not only the first */
};

inline constexpr FaultPlan kFaultPlans[] = {
	/* A store-exclusive reports failure without writing, nothing else wrong: a loss. */
	{Fault::lost, "lost", "exclusive-qemu", "/pair.a", true},
	{Fault::lost, "lost", "exclusive-spill-qemu", "/spill_pair.a", true},
	/* A loss with another register wrong: r0 = 1 where the pack has 0. */
	{Fault::lost_and_register, "lost+register", "exclusive-spill-qemu", "/spill_pair.a", false},
	/* A loss with TPIDRURO wrong. */
	{Fault::lost_and_tpidruro, "lost+tpidruro", "exclusive-spill-qemu", "/spill_pair.a", false},
	/* The store reports success and did not write, and another register is wrong: no status
	 * reads 1, so the 1 in the other register is not one. */
	{Fault::unwritten_and_register, "unwritten+register", "exclusive-spill-qemu", "/spill_pair.a", false},
	/* A store-exclusive the pack has fail, with another register wrong. */
	{Fault::failed_and_register, "failed+register", "exclusive-qemu", "/lone.a", false},
	/* More instructions retired than the pack has, in the loops of the exclusive-retry record
	 * (retry_loop_length() names their lengths). Whole passes of the loop are a retried loss:
	 * one, two, and the five of the CBNZ loop and the seven of the IT one. */
	{Fault::excess, "excess+loop", "exclusive-retry-qemu", "/side_exit_match.a", true, 6},
	{Fault::excess, "excess+2loops", "exclusive-retry-qemu", "/side_exit_match.a", true, 12},
	{Fault::excess, "excess+cbnz-loop", "exclusive-retry-qemu", "/side_exit_cbnz.t", true, 5},
	{Fault::excess, "excess+it-loop", "exclusive-retry-qemu", "/it_ite.t", true, 7},
	/* Anything else fails on the first attempt: one instruction, the length of another loop,
	 * and an excess in a case that is no retry loop. */
	{Fault::excess, "excess+1", "exclusive-retry-qemu", "/side_exit_match.a", false, 1},
	{Fault::excess, "excess+cbnz-wrong", "exclusive-retry-qemu", "/side_exit_cbnz.t", false, 6},
	{Fault::excess, "excess+it-wrong", "exclusive-retry-qemu", "/it_eq.t", false, 6},
	{Fault::excess, "excess-no-loop", "exclusive-qemu", "/pair.a", false, 6},
	/* A whole pass on every attempt is retried to the attempt bound and then fails. */
	{Fault::excess, "excess+every-attempt", "exclusive-retry-qemu", "/side_exit_match.a", false, 6, true},
};

/* What a fault changes beyond the registers. */
struct FaultEffect final {
	bool put_back;                /* the store's bytes go back to what the case started with */
	std::uint32_t offset, size;   /* the store's bytes */
	std::uint32_t tpidruro_xor;
};

/* Makes `registers`, the engine's r0-r14 after a correct run of `test`, those
 * of an attempt with `fault`; nothing when the fault does not fit the case. */
[[nodiscard]] inline std::optional<FaultEffect> inject_fault(Fault fault, const Case &test, std::uint64_t *registers)
{
	if (fault == Fault::excess) /* the engine's count changes, nothing else */
		return FaultEffect{false, 0, 0, 0};
	const auto store = exclusive_store(test);
	if (!store || store->instruction.operands.d < 0 ||
	    static_cast<unsigned>(store->instruction.operands.d) >= kRegisters)
		return std::nullopt;
	const auto status = static_cast<unsigned>(store->instruction.operands.d);
	/* A register the pack has at 0, to make 1. */
	unsigned other = kRegisters;
	for (unsigned index = 0; index < kRegisters && other == kRegisters; ++index)
		if (index != status && test.final.r[index] == 0)
			other = index;
	const bool succeeded = test.final.r[status] == 0;
	FaultEffect effect{false, store->offset, store->size, 0};
	if (fault == Fault::lost_and_register || fault == Fault::unwritten_and_register ||
	    fault == Fault::failed_and_register) {
		if (other == kRegisters)
			return std::nullopt;
		registers[other] = 1;
	}
	if (fault == Fault::failed_and_register)
		return succeeded ? std::nullopt : std::optional{effect};
	if (!succeeded)
		return std::nullopt;
	effect.put_back = true;
	if (fault != Fault::unwritten_and_register)
		registers[status] = 1;
	if (fault == Fault::lost_and_tpidruro)
		effect.tpidruro_xor = 0x100;
	return effect;
}

/* The loops of the exclusive-retry record, whose exit reaches a store-exclusive that has no
 * load-exclusive of its own. A store-exclusive that loses its reservation sends such a loop
 * round again, so an engine that did lose it retires whole passes of the loop more than the
 * pack, which is one pass and the tail branch. A pass is six instructions, five in the CBNZ
 * loop and seven in the IT and ITE loops; 0 for any other case, and for one whose count
 * is not a pass and the tail. */
[[nodiscard]] inline std::uint32_t retry_loop_length(std::string_view record, const Case &test)
{
	if (record != "exclusive-retry-qemu" || test.event != 0)
		return 0;
	const std::string_view name = test.name;
	const std::uint32_t length = name.ends_with("/side_exit_cbnz.t") ? 5
		: name.ends_with("/it_eq.t") || name.ends_with("/it_ite.t") ? 7 : 6;
	return test.retired == length + 1 ? length : 0;
}

/* Whether `excess` instructions retired beyond the pack's are whole passes of a recognized
 * retry loop, which a lane takes for a lost reservation and runs again. */
[[nodiscard]] inline bool retry_excess_accepted(std::string_view record, const Case &test, std::size_t excess)
{
	const auto length = retry_loop_length(record, test);
	return length != 0 && excess != 0 && excess % length == 0;
}

/* Whether a plan applies to this case of this record. */
[[nodiscard]] inline bool plan_selects(const FaultPlan &plan, const Record &record, const Case &test)
{
	return record.name == plan.record && std::string_view(test.name).ends_with(plan.shape);
}

} // namespace uml::testing::aarch32_corpus
