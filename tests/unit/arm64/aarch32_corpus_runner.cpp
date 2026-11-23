// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Checks the generated AArch32 instruction corpus. tests/unit/arm64/
 * aarch32_corpus/README.md owns the pack format and the oracles.
 *
 * This lane runs no engine, so a case is checked against what is known
 * without one: its program decodes, in order and in the IT state the earlier
 * instructions leave, to legal forms except at the instruction that raised an
 * event; the instruction under test is the form the case names; the event
 * is one the instruction's kind can raise; the retired count, the fault
 * position and address, the state changes and the memory runs are consistent
 * with that; a store-exclusive that succeeded has its range checked, which the
 * engine lanes' retry of a lost reservation relies on; and a case is stored
 * once. Every case of the 4 KiB profile runs.
 *
 *   uml_arm64_aarch32_corpus_tests PACK [--record NAME]
 */
#include "aarch32_corpus_pack.hpp"
#include "cpu/arm64/aarch32/decode.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using ::uml::testing::require;
using namespace ::uml::cpu::arm64::aarch32;
using namespace ::uml::testing::aarch32_corpus;

/* Checking ---------------------------------------------------------------------- */

void require_in(const std::string &context, bool condition, const char *what)
{
	if (!condition) {
		std::fprintf(stderr, "%s: %s\n", context.c_str(), what);
		std::abort();
	}
}

std::vector<ProgramStep> decode_program(const std::string &context, const Case &test, const Profile &profile,
	bool &truncated)
{
	std::vector<ProgramStep> result;
	const auto end = for_each_instruction(test, profile, [&](const ProgramStep &step) {
		result.push_back(step);
		return true;
	});
	/* A program that switches state can end in a half of a word. */
	require_in(context, end != ProgramEnd::split_word || !(test.flags & kFlagStraight),
		"an A32 program is whole words");
	/* The words of A32 code a program branches into end a Thumb program this way, and so does a
	 * Thumb-2 instruction cut off by the end of the code region. */
	truncated = end == ProgramEnd::cut_off;
	require_in(context, !truncated || !(test.flags & kFlagStraight), "a Thumb program ends inside an instruction");
	return result;
}

const std::map<std::string, Form> &forms_by_name()
{
	static const std::map<std::string, Form> forms = [] {
		std::map<std::string, Form> result;
		for (std::size_t index = 0; index < kFormCount; ++index)
			result.emplace(kForms[index].name, static_cast<Form>(index));
		return result;
	}();
	return forms;
}

bool is_linux_breakpoint(const Instruction &instruction)
{
	/* The three encodings Linux turns into SIGTRAP (arm64's compat layer, as
	 * arm's): A32 0xe7f001f0, T16 0xde01 and T32 0xf7f0a000. */
	return instruction.word == 0xE7F001F0 || instruction.word == 0xDE010000 ||
		instruction.word == 0xF7F0A000;
}

/* for_each_instruction() hands every lane the program as the decoder reads it:
 * A32 words, or Thumb instructions in the IT state the earlier ones leave. */
void check_program_walk()
{
	const Profile profile;
	struct Seen final {
		unsigned half, cond;
		bool in_it, last_in_it;
	};
	const auto walk = [&](const Case &test, std::size_t stop_after, std::vector<Seen> &seen) {
		return for_each_instruction(test, profile, [&](const ProgramStep &step) {
			seen.push_back({step.half, step.instruction.cond, step.instruction.in_it,
				step.instruction.last_in_it});
			return seen.size() < stop_after;
		});
	};
	std::vector<Seen> seen;
	Case thumb;
	thumb.flags = kFlagThumb;
	thumb.halves = {0xbf04, 0x2001, 0x2102, 0x2203}; /* itt eq; movs; movs; movs */
	require(walk(thumb, 99, seen) == ProgramEnd::whole && seen.size() == 4, "a Thumb program is walked whole");
	require(seen[0].cond == 14 && !seen[0].in_it, "an IT instruction is not in its block");
	require(seen[1].half == 1 && seen[1].cond == 0 && seen[1].in_it && !seen[1].last_in_it,
		"the IT state reaches the next instruction");
	require(seen[2].half == 2 && seen[2].cond == 0 && seen[2].in_it && seen[2].last_in_it,
		"the IT state follows the block");
	require(seen[3].cond == 14 && !seen[3].in_it, "the IT block ends");
	seen.clear();
	require(walk(thumb, 2, seen) == ProgramEnd::whole && seen.size() == 2, "a visitor stops the walk");
	seen.clear();
	thumb.halves = {0x2001, 0xf000};
	require(walk(thumb, 99, seen) == ProgramEnd::cut_off && seen.size() == 1,
		"a Thumb-2 instruction cut off is reported");
	seen.clear();
	Case arm;
	arm.halves = {0x0000, 0xe1a0, 0x0000, 0xe1a0, 0x1234};
	require(walk(arm, 99, seen) == ProgramEnd::split_word && seen.size() == 2 && seen[1].half == 2,
		"half an A32 word is reported");
}

/* The retirement a lane takes for a lost reservation in a loop that retries its
 * store-exclusive: whole passes of a recognized loop more than the pack, and
 * nothing else. */
void check_retry_classification()
{
	constexpr std::string_view record = "exclusive-retry-qemu";
	const auto loop = [](const char *name, std::uint16_t retired, std::uint8_t event = 0) {
		Case test;
		test.name = name;
		test.retired = retired;
		test.event = event;
		return test;
	};
	const auto accepts = [&](const Case &test, std::size_t excess) {
		return retry_excess_accepted(record, test, excess);
	};
	/* A six-instruction pass: one pass, two and sixteen are accepted, and one
	 * instruction, the five or seven of the other loops, and nothing are not. */
	const Case six = loop("STREX_A1/side_exit_match.a", 7);
	require(retry_loop_length(record, six) == 6, "a pass of the A32 loop is six instructions");
	require(accepts(six, 6) && accepts(six, 12) && accepts(six, 96), "whole passes of a six-long loop");
	require(!accepts(six, 1) && !accepts(six, 5) && !accepts(six, 7) && !accepts(six, 13) && !accepts(six, 0),
		"an excess that is not whole passes of a six-long loop is accepted");
	/* The CBNZ loop is five long and the IT and ITE loops seven. */
	const Case cbnz = loop("STREX_T1/side_exit_cbnz.t", 6);
	require(retry_loop_length(record, cbnz) == 5 && accepts(cbnz, 5) && accepts(cbnz, 10),
		"whole passes of the CBNZ loop");
	require(!accepts(cbnz, 6) && !accepts(cbnz, 12) && !accepts(cbnz, 1) && !accepts(cbnz, 7),
		"an excess that is not whole passes of the CBNZ loop is accepted");
	for (const char *name : {"STREX_T1/it_eq.t", "STREX_T1/it_ite.t"}) {
		const Case it = loop(name, 8);
		require(retry_loop_length(record, it) == 7 && accepts(it, 7) && accepts(it, 14),
			"whole passes of an IT loop");
		require(!accepts(it, 6) && !accepts(it, 12) && !accepts(it, 1) && !accepts(it, 8),
			"an excess that is not whole passes of an IT loop is accepted");
	}
	/* Nothing outside the record's loops: another record, a case that raised
	 * an event, and one whose count is not a pass and the tail. */
	require(!retry_excess_accepted("exclusive-qemu", six, 6) && !retry_excess_accepted("exclusive-retry", six, 6),
		"an excess in another record is accepted");
	require(!accepts(loop("STREX_A1/side_exit_match.a", 7, kSigSegv), 6), "an excess after an event is accepted");
	require(!accepts(loop("STREX_A1/side_exit_match.a", 9), 6) && !accepts(loop("STREX_A1/side_exit_match.a", 3), 6),
		"a case that is not a pass and a tail is a retry loop");
}

struct Totals final {
	std::size_t records = 0, cases = 0, events = 0;
	std::map<std::string, std::size_t> per_oracle;
	/* The forms of the store-exclusives whose bytes were checked, and how many
	 * cases that took. */
	std::set<std::string> exclusive_forms;
	std::size_t exclusive_stores = 0;
};

/* A store-exclusive that succeeded: its range, which the engine lanes take
 * from exclusive_store() to tell a lost reservation from a wrong byte, is the
 * place the pack's own data shows the store at, and lost_exclusive_data()
 * accepts the data such a loss leaves and nothing else. */
void check_exclusive_store(const std::string &context, const Case &test, Totals &totals)
{
	const auto fail = [&](const char *what) {
		std::fprintf(stderr, "%s: %s\n", context.c_str(), what);
		std::abort();
	};
	const auto store = exclusive_store(test);
	if (!store)
		fail("a store-exclusive that succeeded has a range the pack does not name");
	const auto &operands = store->instruction.operands;
	const auto initial = data_before(test);
	const auto expected = data_after(test, initial);
	const auto in_range = [&](std::uint32_t byte) {
		return byte >= store->offset && byte - store->offset < store->size;
	};

	/* What the store writes: its register or registers, in memory order. */
	const bool pair = store->size == 8;
	if (test.initial.r[operands.t] == test.final.r[operands.t] &&
	    (!pair || test.initial.r[operands.t2] == test.final.r[operands.t2])) {
		std::uint8_t bytes[8] = {};
		std::memcpy(bytes, &test.final.r[operands.t], std::min<std::uint32_t>(store->size, 4));
		if (pair)
			std::memcpy(bytes + 4, &test.final.r[operands.t2], 4);
		if (std::memcmp(expected.data() + store->offset, bytes, store->size) != 0)
			fail("a store-exclusive's range does not hold what it stores");
		totals.exclusive_forms.insert(test.form);
		++totals.exclusive_stores;
	}

	/* The data a lost reservation leaves is accepted... */
	auto lost = expected;
	std::copy_n(initial.begin() + store->offset, store->size, lost.begin() + store->offset);
	const auto accepts = [&](const std::vector<std::uint8_t> &actual) {
		return lost_exclusive_data(test, initial, expected, actual);
	};
	if (!accepts(lost))
		fail("the data a lost reservation leaves is not accepted");
	/* ...and nothing else is: not a wrong byte beside it, before it or in what
	 * else the case wrote, one in its own range, a store that was made whole or
	 * in part, or a case whose address the pack no longer names. */
	const auto refuses = [&](std::vector<std::uint8_t> actual, std::uint32_t byte, const char *what) {
		actual[byte] ^= 0x80;
		if (accepts(actual))
			fail(what);
	};
	std::vector<std::uint32_t> elsewhere = {0, kDataSize - 1, store->offset - 1, store->offset + store->size};
	for (const auto &patch : test.memory_patches)
		elsewhere.push_back(patch.offset);
	for (const std::uint32_t byte : elsewhere)
		if (byte < kDataSize && !in_range(byte))
			refuses(lost, byte, "a wrong byte outside the store-exclusive's range is accepted");
	for (std::uint32_t byte = store->offset; byte < store->offset + store->size; ++byte) {
		refuses(lost, byte, "a wrong byte inside the store-exclusive's range is accepted");
		if (expected[byte] != initial[byte]) {
			auto partial = lost;
			partial[byte] = expected[byte];
			if (accepts(partial))
				fail("a store that was made in part is accepted");
			if (accepts(expected))
				fail("a store that was made is accepted");
		}
	}
	/* The range ends where the store does: a byte next to it that the case wrote, which
	 * these cases leave alone, is the pack's and not what the case started with. */
	for (const std::uint32_t neighbour : {store->offset - 1, store->offset + store->size}) {
		if (neighbour >= kDataSize)
			continue;
		auto wrote = expected;
		wrote[neighbour] = static_cast<std::uint8_t>(initial[neighbour] ^ 0x5A);
		auto after = wrote;
		std::copy_n(initial.begin() + store->offset, store->size, after.begin() + store->offset);
		if (!lost_exclusive_data(test, initial, wrote, after))
			fail("a byte next to a store-exclusive's range is taken for part of it");
		after[neighbour] = initial[neighbour];
		if (lost_exclusive_data(test, initial, wrote, after))
			fail("a byte that the case wrote next to a store-exclusive's range is accepted as the start's");
	}
	Case moved = test;
	moved.final.r[operands.n] ^= 0x40;
	if (lost_exclusive_data(moved, initial, expected, lost))
		fail("a store-exclusive whose base register changed is accepted");
	Case other = test;
	other.form = "LDREX_A1";
	if (lost_exclusive_data(other, initial, expected, lost))
		fail("a case that is not a store-exclusive is accepted");
}

void check_case(const Record &record, const Case &test, const Profile &profile, Totals &totals)
{
	const auto context = record.name + ": " + test.name;
	const auto fail = [&](const char *what) {
		std::fprintf(stderr, "%s: %s\n", context.c_str(), what);
		std::abort();
	};
	if (record.name == "exclusive-retry-qemu" && retry_loop_length(record.name, test) == 0)
		fail("a case of the exclusive-retry record is not a recognized retry loop");
	bool truncated = false;
	const auto program = decode_program(context, test, profile, truncated);
	const auto named = forms_by_name().find(test.form);
	if (named == forms_by_name().end())
		fail("names a form the decoder does not have");
	if (truncated && test.target == program.size() && (test.flags & kFlagThumb)) {
		/* The first halfword of a 32-bit instruction is the last one mapped: the fetch
		 * of the second faults, at the first byte past the code region, before the
		 * instruction runs. */
		if (test.origin + 2U * test.halves.size() != kCodeSize || test.event != kSigSegv ||
			test.fault_address != kCodeBase + kCodeSize || test.retired != program.size())
			fail("a cut-off fetch faults at the end of the code region");
		++totals.events;
		++totals.cases;
		++totals.per_oracle[record.oracle];
		return;
	}
	if (test.target >= program.size())
		fail("the instruction under test is not in the program");
	const Instruction &target = program[test.target].instruction;
	if (target.form != named->second)
		fail("the instruction under test decodes to a different form");

	const bool thumb = test.flags & kFlagThumb;
	const bool straight = test.flags & kFlagStraight;
	const bool exit_thumb = test.flags & kFlagExitThumb;
	if (test.event == 0) {
		if (straight) {
			if (exit_thumb != thumb)
				fail("a straight program leaves in the instruction set it entered");
			if (test.retired != program.size())
				fail("a straight program retires all its instructions");
			for (const auto &decoded : program)
				if (!decoded.instruction.legal())
					fail("a program that finished holds an instruction the decoder rejects");
		}
	} else {
		++totals.events;
		if (test.event != kSigIll && test.event != kSigTrap && test.event != kSigBus &&
			test.event != kSigFpe && test.event != kSigSegv && test.event != kEventSvc && test.event != 14)
			fail("names an event that does not exist");
		const auto position = std::find_if(program.begin(), program.end(),
			[&](const ProgramStep &decoded) { return decoded.half == test.fault_instruction; });
		if (straight) {
			if (position == program.end())
				fail("the fault is not at an instruction");
			const auto ordinal = static_cast<unsigned>(position - program.begin());
			if (test.retired != ordinal)
				fail("a straight program retires the instructions before the fault");
			for (unsigned index = 0; index < ordinal; ++index)
				if (!program[index].instruction.legal())
					fail("an instruction ahead of the fault is rejected by the decoder");
			const Instruction &faulting = position->instruction;
			if (test.event == kSigIll) {
				/* UDF decodes and its execution is the undefined instruction. */
				if (faulting.legal() && !std::string_view(faulting.info().name).starts_with("UDF"))
					fail("SIGILL at an instruction the decoder accepts");
				if (test.fault_address != kCodeBase + test.origin + 2U * test.fault_instruction)
					fail("SIGILL reports the instruction's address");
			} else if (test.event == kSigTrap) {
				if (!is_linux_breakpoint(faulting) &&
					!std::string_view(faulting.info().name).starts_with("BKPT"))
					fail("SIGTRAP at an instruction that is neither BKPT nor one of Linux's breakpoints");
			} else if (test.event == kEventSvc) {
				if (!std::string_view(faulting.info().name).starts_with("SVC"))
					fail("a system call event at an instruction that is not SVC");
			} else if (test.event == kSigSegv || test.event == kSigBus) {
				constexpr std::uint16_t memory = trait::loads | trait::stores | trait::exclusive | trait::prefetch;
				if (!faulting.legal() || !(faulting.info().traits & memory))
					fail("a memory fault at an instruction that does not access memory");
			}
		}
	}

	std::uint32_t end = 0;
	for (const auto &patch : test.memory_patches) {
		if (patch.offset < end || patch.offset + patch.length > kDataSize)
			fail("a changed data run is outside the data region");
		end = patch.offset + patch.length;
	}
	if ((test.final.apsr & ~0xF80F0000U) != 0 || (test.initial.apsr & ~0xF80F0000U) != 0)
		fail("an APSR holds bits other than N, Z, C, V, Q and GE");
	/* A store-exclusive that succeeded: not one that failed on purpose, one that raised an
	 * event or one a condition skipped. */
	if (std::string_view(test.form).starts_with("STREX") || std::string_view(test.form).starts_with("STLEX")) {
		const auto &operands = target.operands;
		if (test.event == 0 && target.cond == 14 && operands.d >= 0 && operands.d < static_cast<int>(kRegisters) &&
		    test.final.r[operands.d] == 0)
			check_exclusive_store(context, test, totals);
	}
	++totals.cases;
	++totals.per_oracle[record.oracle];
}

/* Every form of store-exclusive has had its range checked: the byte, halfword, word and
 * doubleword ones, STREX and STLEX, in A32 and T32. */
void check_exclusive_forms(const Totals &totals)
{
	for (const char *mnemonic : {"STREX", "STLEX"})
		for (const char *width : {"", "B", "H", "D"})
			for (const char *state : {"A1", "T1"}) {
				const std::string form = std::string(mnemonic) + width + "_" + state;
				if (!totals.exclusive_forms.contains(form)) {
					std::fprintf(stderr, "no case checks the range of %s\n", form.c_str());
					std::abort();
				}
			}
}

} // namespace

int main(int argc, char **argv)
{
	const char *path = nullptr;
	std::string only;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--record" && index + 1 < argc) {
			only = argv[++index];
		} else {
			require(path == nullptr && !argument.starts_with("--"), "usage: PACK [--record NAME]");
			path = argv[index];
		}
	}
	require(path != nullptr, "usage: PACK [--record NAME]");

	check_program_walk();
	check_retry_classification();
	const Pack pack = read_pack(read_file(path));
	const Profile profile;
	Totals totals;
	std::set<std::string> names;
	for (const Record &record : pack.records) {
		require(names.insert(record.name).second, "a record name is used twice");
		if ((record.profiles & kProfileBit) == 0 || (!only.empty() && record.name != only))
			continue;
		++totals.records;
		std::set<std::tuple<std::string, std::string, std::uint16_t, std::vector<std::uint16_t>>> seen;
		for (const Case &test : record.cases) {
			require(seen.emplace(test.name, test.form, test.recipe, test.halves).second,
				"a case is stored twice");
			check_case(record, test, profile, totals);
		}
	}
	require(totals.records != 0, "no record applies to the 4 KiB profile");
	if (only.empty())
		check_exclusive_forms(totals);
	std::printf("AArch32 corpus: %zu cases in %zu records (%zu raise an event)\n",
		totals.cases, totals.records, totals.events);
	std::printf("  store-exclusives with a checked range: %zu in %zu forms\n", totals.exclusive_stores,
		totals.exclusive_forms.size());
	for (const auto &[oracle, count] : totals.per_oracle)
		std::printf("  %s: %zu\n", oracle.c_str(), count);
	return 0;
}
