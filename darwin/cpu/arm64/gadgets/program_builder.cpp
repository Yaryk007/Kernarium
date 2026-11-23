// SPDX-License-Identifier: GPL-2.0-or-later
#include "program_builder_decode.hpp"
#include "afp_twin.hpp"
#include "gadget_selection.hpp"
#include "execution_histogram.hpp"
#include "group_starts.hpp"
#include "scalar_forwarding.hpp"
#include "same_base.hpp"

#include "cpu/arm64/floating_controls.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

/* Builder::build() below calls one of these five per guest instruction it
 * translates, and each of them calls on into the gadget selection tables.
 * Before the engine split that whole decode -> select -> emit chain lived in
 * one translation unit and the compiler inlined it freely; separating the
 * families into their own objects put hard call boundaries across the hottest
 * loop in translation and cost measurable score. Interprocedural optimization
 * recovered part of it but left single-core Clang, the most translation-heavy
 * workload, untouched, because ThinLTO imports across modules on a cost model
 * that declines functions this large. Composing them textually here is not a
 * shortcut around that decision: it is the only way to make the inliner see the
 * whole chain rather than leave it to a heuristic that has been measured to
 * decline. Each fragment still owns exactly one decode family. */
/* .ipp rather than .inc: every .inc in this tree is an assembly fragment of the
 * gadget bank, and editors do not treat .inc as C++. */
#include "program_builder_branch_system_impl.ipp"
#include "program_builder_data_processing_impl.ipp"
#include "program_builder_exception_impl.ipp"
#include "program_builder_floating_simd_impl.ipp"
#include "program_builder_load_store_impl.ipp"

namespace uml::cpu::arm64::gadgets {

using namespace selection;
using namespace builder_detail;

DecodeResult builder_detail::implementation_gap(DecodeContext &context)
{
	/* Keep a translated prefix executable. The terminal gadget reports the
	 * first unsupported allocation without charging it as a retired
	 * instruction; instruction_count deliberately remains the number of
	 * successfully translated words.
	 */
	emit(context.words, uml_arm64_gadget_illegal);
	emit(context.words, context.address);
	return DecodeResult::implementation_gap;
}

namespace {

/* Fill in the reserved suffix operand of each inlined conditional branch, now
 * that the block's translated length is final. A taken branch subtracts this
 * from the count its block was precharged, so it is the instructions after the
 * branch itself. */
void resolve_inline_suffixes(ProgramWords &words,
			     const InlineSuffixes &suffixes,
			     std::size_t compiled) noexcept
{
	for (std::uint32_t index = 0; index < suffixes.count; ++index) {
		const auto translated = suffixes.compiled[index];
		const auto suffix = translated >= compiled ?
			0 : compiled - translated;
		if (suffixes.shift[index] == 0)
			words[suffixes.word[index]] = suffix;
		else
			words[suffixes.word[index]] |=
				suffix << suffixes.shift[index];
	}
}

/* The epilogue run a head in front of it can do whole: SP-based 64-bit loads
 * and at most one ADD SP, SP, #imm, then the RET through X30 that ends the
 * block, or a hint the virtual CPU runs as a NOP and then that RET
 * (engine/run_heads.inc has what the head does and its operand words). */
namespace epilogue {

inline constexpr std::size_t maximum_loads = 8;
inline constexpr std::size_t maximum_adds = 1;
/* The elements a run may have, ADD SP included: every one of them and the
 * RET must still be in the builder's ring of group starts. */
inline constexpr std::size_t maximum_elements = 16;
inline constexpr std::size_t ring = GroupStarts::ring;
/* Every element and the RET is one decode group of two words, behind its
 * execution_count group in a counting build. */
#if UML_GADGET_EXECUTION_HISTOGRAM
inline constexpr std::size_t count_words = 3;
#else
inline constexpr std::size_t count_words = 0;
#endif
inline constexpr std::size_t group_words = count_words + 2;

/* By loads, then ADD SP count. */
constexpr std::array<std::array<void (*)(), maximum_adds + 1>, maximum_loads>
	heads{{
		{uml_arm64_gadget_run_epilogue_1_0,
		 uml_arm64_gadget_run_epilogue_1_1},
		{uml_arm64_gadget_run_epilogue_2_0,
		 uml_arm64_gadget_run_epilogue_2_1},
		{uml_arm64_gadget_run_epilogue_3_0,
		 uml_arm64_gadget_run_epilogue_3_1},
		{uml_arm64_gadget_run_epilogue_4_0,
		 uml_arm64_gadget_run_epilogue_4_1},
		{uml_arm64_gadget_run_epilogue_5_0,
		 uml_arm64_gadget_run_epilogue_5_1},
		{uml_arm64_gadget_run_epilogue_6_0,
		 uml_arm64_gadget_run_epilogue_6_1},
		{uml_arm64_gadget_run_epilogue_7_0,
		 uml_arm64_gadget_run_epilogue_7_1},
		{uml_arm64_gadget_run_epilogue_8_0,
		 uml_arm64_gadget_run_epilogue_8_1},
	}};

/* One load, at its address from SP at entry. A single load names its
 * register as both halves and owns the first 8 of the 16 bytes the head
 * reads. */
struct Load final {
	std::int64_t address{};
	unsigned first{};
	unsigned second{};
	std::int64_t bytes{};
};

struct Plan final {
	std::size_t loads{};
	std::size_t adds{};
	std::array<Load, maximum_loads> load{};
};

/* A head and its load count, which fixes its operand words. */
struct Head final {
	void (*gadget)(){};
	std::size_t loads{};
};

[[nodiscard]] constexpr bool is_element(std::uint32_t word) noexcept
{
	return (word & 0xfe40'03e0U) == 0xa840'03e0U ||  // LDP Xt, Xt2, [SP..]
		(word & 0xffc0'03e0U) == 0xf940'03e0U ||  // LDR Xt, [SP, #imm]
		((word & 0xffe0'03e0U) == 0xf840'03e0U && // LDUR, LDR Xt with
		 ((word >> 10) & 3U) != 2U) ||            // writeback
		(word & 0xff80'03ffU) == 0x9100'03ffU;    // ADD SP, SP, #imm
}

/* Adds one element, at SP offset `sp`, to `plan` and advances `sp` past
 * its writeback. False for a form the head does not perform: an access
 * through an SP the ordinary gadget would fault on as misaligned, a load
 * into XZR or of one register twice, a ninth load or a second ADD SP. */
[[nodiscard]] bool add(std::uint32_t word, std::int64_t &sp, Plan &plan) noexcept
{
	if ((word & 0xff80'03ffU) == 0x9100'03ffU) {
		if (plan.adds == maximum_adds)
			return false;
		++plan.adds;
		sp += static_cast<std::int64_t>(((word >> 10) & 0xfffU) <<
			((word & 0x0040'0000U) != 0 ? 12U : 0U));
		return true;
	}
	if ((sp & 15) != 0 || plan.loads == maximum_loads)
		return false;
	Load load{};
	if ((word & 0xfe40'03e0U) == 0xa840'03e0U) {
		load.first = word & 31U;
		load.second = (word >> 10) & 31U;
		load.bytes = 16;
		const auto mode = (word >> 23) & 3U;
		const auto displacement =
			sign_extend((word >> 15) & 0x7fU, 7) * 8;
		load.address = mode == 1U ? sp : sp + displacement;
		if (mode == 1U || mode == 3U)
			sp += displacement;
	} else if ((word & 0xffc0'03e0U) == 0xf940'03e0U) {
		load.first = load.second = word & 31U;
		load.bytes = 8;
		load.address = sp + static_cast<std::int64_t>(
			((word >> 10) & 0xfffU) << 3);
	} else {
		load.first = load.second = word & 31U;
		load.bytes = 8;
		const auto mode = (word >> 10) & 3U;
		const auto displacement = sign_extend((word >> 12) & 0x1ffU, 9);
		load.address = mode == 1U ? sp : sp + displacement;
		if (mode != 0U)
			sp += displacement;
	}
	if (load.first == 31U || load.second == 31U ||
	    (load.bytes == 16 && load.first == load.second))
		return false;
	plan.load[plan.loads++] = load;
	return true;
}

/* The head's operand words for the elements [start, ret) of `window`, the
 * RET at `ret`, into `operands`. The result is the head, without a gadget
 * when it cannot do the elements. */
[[nodiscard]] Head encode(std::span<const std::uint32_t> window,
	std::size_t start, std::size_t ret,
	std::array<std::uintptr_t, 4> &operands) noexcept
{
	Plan plan{};
	std::int64_t sp = 0;
	for (std::size_t index = start; index < ret; ++index)
		if (!add(window[index], sp, plan))
			return {};
	/* X30 comes from the second register of exactly one pair, which goes
	 * last so that it is the value the head returns through. */
	std::uint32_t written = 0;
	std::size_t link = maximum_loads;
	for (std::size_t index = 0; index < plan.loads; ++index) {
		const auto &load = plan.load[index];
		const auto registers = (1U << load.first) | (1U << load.second);
		if ((written & registers) != 0)
			return {};
		written |= registers;
		if (load.first == 30U && load.bytes == 8)
			return {};
		if (load.second == 30U && load.bytes == 16)
			link = index;
		else if (load.first == 30U)
			return {};
	}
	if (link == maximum_loads)
		return {};
	std::rotate(plan.load.begin() + static_cast<std::ptrdiff_t>(link),
		plan.load.begin() + static_cast<std::ptrdiff_t>(link) + 1,
		plan.load.begin() + static_cast<std::ptrdiff_t>(plan.loads));
	/* The bytes each load reads are its own; the head's 16-byte reads may
	 * reach past a single load's 8, so they stay in the span but may
	 * overlap another load's. */
	std::int64_t low = plan.load[0].address;
	std::int64_t end = low + 16;
	for (std::size_t index = 0; index < plan.loads; ++index) {
		const auto &load = plan.load[index];
		if ((load.address & 7) != 0)
			return {};
		for (std::size_t other = 0; other < index; ++other) {
			const auto &earlier = plan.load[other];
			if (load.address < earlier.address + earlier.bytes &&
			    earlier.address < load.address + load.bytes)
				return {};
		}
		low = std::min(low, load.address);
		end = std::max(end, load.address + 16);
	}
	const auto last = end - 1 - low;
	const auto low_units = low / 8;
	if (low_units < -4096 || low_units > 4095 ||
	    (sp & 15) != 0 || sp / 16 < -2048 || sp / 16 > 2047)
		return {};
	/* A load's place in the span has 5 bits, which keeps the span's last
	 * byte inside its 9. */
	static_assert(8 * 31 + 16 - 1 < 1 << 9);
	for (std::size_t index = 0; index < plan.loads; ++index)
		if (plan.load[index].address - low > 8 * 31)
			return {};
	const auto packed = [&](std::size_t index) {
		const auto &load = plan.load[index];
		return static_cast<std::uintptr_t>((load.address - low) / 8) |
			(static_cast<std::uintptr_t>(load.first) << 5) |
			(static_cast<std::uintptr_t>(load.second) << 10);
	};
	operands = {};
	operands[0] = (static_cast<std::uintptr_t>(low_units) & 0x1fffU) |
		(static_cast<std::uintptr_t>(last) << 13) |
		((static_cast<std::uintptr_t>(sp / 16) & 0xfffU) << 22) |
		(packed(0) << 34);
	for (std::size_t index = 1; index < plan.loads; ++index)
		operands[(index + 2) / 4] |= packed(index) <<
			(index < 2 ? 49 : 15 * ((index - 2) % 4));
	return {heads[plan.loads - 1][plan.adds], plan.loads};
}

[[nodiscard]] constexpr std::size_t operand_words(std::size_t loads) noexcept
{
	return 1 + (loads + 1) / 4;
}

using Starts = GroupStarts;
#if UML_GADGET_EXECUTION_HISTOGRAM
using Predecessors = std::array<const void *, ring>;
#endif

/* Put a head in front of the longest epilogue run the RET at `ret` ends, now
 * that the RET's group is the last one emitted. `starts` holds where each
 * recent instruction's group begins. The run's words stay where they are,
 * behind the head, and are its fallback. A group that is only the nop gadget
 * right before the RET, which a hint without effect on the virtual CPU
 * translates to (AUTIASP in code built for pointer authentication), may end
 * the run instead; it goes with the head, since it does nothing on either
 * path and the head finds the RET's cell at a fixed distance. */
void place(ProgramWords &words, InlineSuffixes &suffixes,
	std::span<const std::uint32_t> window, std::size_t ret,
	const Starts &starts
#if UML_GADGET_EXECUTION_HISTOGRAM
	, const Predecessors &predecessors
#endif
	)
{
	const auto ret_start = starts[ret];
	if (words.size() != ret_start + group_words ||
	    words[ret_start + count_words] != reinterpret_cast<std::uintptr_t>(
		    uml_arm64_gadget_return_x30_cached))
		return;
	auto end = ret;
	if (ret != 0 && starts[ret - 1] + count_words + 1 == ret_start &&
	    words[ret_start - 1] ==
		    reinterpret_cast<std::uintptr_t>(uml_arm64_gadget_nop))
		end = ret - 1;
	std::size_t length = 0;
	while (length < std::min(end, maximum_elements) &&
	       is_element(window[end - 1 - length]))
		++length;
	for (; length != 0; --length) {
		const auto start = end - length;
		if (!starts.grouped(start, end, group_words, starts[end]))
			continue;
		std::array<std::uintptr_t, 4> operands{};
		const auto [gadget, loads] = encode(window, start, end, operands);
		if (gadget == nullptr)
			continue;
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The head's group counts the instructions it stands for, a
		 * dropped hint and the RET among them, and their own groups
		 * count none: slot zero counts each once on either path. */
		for (auto index = start; index < end; ++index)
			words[starts[index] + 2] = 0;
		words[ret_start + 2] = 0;
#endif
		if (end != ret) {
			const auto dropped = starts[end];
			words.erase(dropped, count_words + 1);
#if UML_GADGET_EXECUTION_HISTOGRAM
			/* The RET's group now follows the run's last word. */
			words[dropped + 1] = diagnostics::execution_histogram_slot(
				predecessors[end % ring],
				reinterpret_cast<const void *>(
					uml_arm64_gadget_return_x30_cached));
#endif
			for (std::uint32_t index = 0; index < suffixes.count; ++index)
				if (suffixes.word[index] > dropped)
					suffixes.word[index] -= static_cast<std::uint32_t>(
						count_words + 1);
		}
		const auto head = reinterpret_cast<std::uintptr_t>(gadget);
		const std::size_t at = starts[start];
		const auto inserted = count_words + 1 + operand_words(loads);
		words.insert(at, inserted);
		auto cursor = at;
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The run's first word names the head as its predecessor, so a
		 * counting build tells the head's fallbacks from its runs. */
		words[cursor++] = reinterpret_cast<std::uintptr_t>(
			diagnostics::uml_arm64_gadget_execution_count);
		words[cursor++] = diagnostics::execution_histogram_slot(
			predecessors[start % ring],
			reinterpret_cast<const void *>(head));
		words[cursor++] = ret + 1 - start;
#endif
		words[cursor++] = head;
		for (std::size_t index = 0; index < operand_words(loads); ++index)
			words[cursor++] = operands[index];
#if UML_GADGET_EXECUTION_HISTOGRAM
		words[cursor + 1] = diagnostics::execution_histogram_slot(
			reinterpret_cast<const void *>(head),
			reinterpret_cast<const void *>(words[cursor + count_words]));
#endif
		for (std::uint32_t index = 0; index < suffixes.count; ++index)
			if (suffixes.word[index] >= at)
				suffixes.word[index] +=
					static_cast<std::uint32_t>(inserted);
		return;
	}
}

} // namespace epilogue

/* The prologue run a head in front of it can do whole: SP-based 64-bit
 * stores, at least one of them a pair and at most one a single, at most one
 * SUB SP, SP, #imm and exactly one ADD X29, SP, #imm (engine/run_heads.inc
 * has what the head does and its operand words). */
namespace prologue {

inline constexpr std::size_t maximum_pairs = 6;
inline constexpr std::size_t maximum_stores = maximum_pairs + 1;
/* The longest run the builder looks back over; every element must still be
 * in the ring of group starts. */
inline constexpr std::size_t maximum_elements = 16;
static_assert(maximum_elements < epilogue::ring);

/* By pairs, then single stores, then SUB SP count. */
using Heads = std::array<std::array<void (*)(), 2>, 2>;
constexpr std::array<Heads, maximum_pairs> heads{{
	{{{uml_arm64_gadget_run_prologue_1_0_0,
	   uml_arm64_gadget_run_prologue_1_0_1},
	  {uml_arm64_gadget_run_prologue_1_1_0,
	   uml_arm64_gadget_run_prologue_1_1_1}}},
	{{{uml_arm64_gadget_run_prologue_2_0_0,
	   uml_arm64_gadget_run_prologue_2_0_1},
	  {uml_arm64_gadget_run_prologue_2_1_0,
	   uml_arm64_gadget_run_prologue_2_1_1}}},
	{{{uml_arm64_gadget_run_prologue_3_0_0,
	   uml_arm64_gadget_run_prologue_3_0_1},
	  {uml_arm64_gadget_run_prologue_3_1_0,
	   uml_arm64_gadget_run_prologue_3_1_1}}},
	{{{uml_arm64_gadget_run_prologue_4_0_0,
	   uml_arm64_gadget_run_prologue_4_0_1},
	  {uml_arm64_gadget_run_prologue_4_1_0,
	   uml_arm64_gadget_run_prologue_4_1_1}}},
	{{{uml_arm64_gadget_run_prologue_5_0_0,
	   uml_arm64_gadget_run_prologue_5_0_1},
	  {uml_arm64_gadget_run_prologue_5_1_0,
	   uml_arm64_gadget_run_prologue_5_1_1}}},
	{{{uml_arm64_gadget_run_prologue_6_0_0,
	   uml_arm64_gadget_run_prologue_6_0_1},
	  {uml_arm64_gadget_run_prologue_6_1_0,
	   uml_arm64_gadget_run_prologue_6_1_1}}},
}};

[[nodiscard]] constexpr bool is_element(std::uint32_t word) noexcept
{
	return (word & 0xfe40'03e0U) == 0xa800'03e0U ||  // STP Xt, Xt2, [SP..]
		(word & 0xffc0'03e0U) == 0xf900'03e0U ||  // STR Xt, [SP, #imm]
		((word & 0xffe0'03e0U) == 0xf800'03e0U && // STUR, STR Xt with
		 ((word >> 10) & 3U) != 2U) ||            // writeback
		(word & 0xff80'03ffU) == 0xd100'03ffU ||  // SUB SP, SP, #imm
		(word & 0xff80'03ffU) == 0x9100'03fdU;    // ADD X29, SP, #imm
}

/* One store, at its address from SP at entry. A single store names its
 * register as both halves. */
struct Store final {
	std::int64_t address{};
	unsigned first{};
	unsigned second{};
	std::int64_t bytes{};
};

struct Plan final {
	std::size_t pairs{};
	std::size_t singles{};
	std::size_t subs{};
	std::size_t frames{};
	/* X29 after the run, from SP at entry. */
	std::int64_t frame{};
	std::size_t stores{};
	std::array<Store, maximum_stores> store{};
};

/* The head a plan selects and its store count, which fixes its operand
 * words. */
struct Head final {
	void (*gadget)(){};
	std::size_t stores{};
};

/* Adds one element, at SP offset `sp`, to `plan` and advances `sp` past
 * its writeback. False for a form the head does not perform: a store through
 * an SP the ordinary gadget would fault on as misaligned, a store of XZR, a
 * store of X29 after the run set it, a seventh pair, a second single store,
 * a second SUB SP or a second ADD X29. */
[[nodiscard]] bool add(std::uint32_t word, std::int64_t &sp, Plan &plan) noexcept
{
	const auto immediate = static_cast<std::int64_t>(
		((word >> 10) & 0xfffU) << ((word & 0x0040'0000U) != 0 ? 12U : 0U));
	if ((word & 0xff80'03ffU) == 0xd100'03ffU) {
		if (plan.subs != 0)
			return false;
		++plan.subs;
		sp -= immediate;
		return true;
	}
	if ((word & 0xff80'03ffU) == 0x9100'03fdU) {
		if (plan.frames != 0)
			return false;
		++plan.frames;
		plan.frame = sp + immediate;
		return true;
	}
	if ((sp & 15) != 0 || plan.stores == maximum_stores)
		return false;
	Store store{};
	if ((word & 0xfe40'03e0U) == 0xa800'03e0U) {
		store.first = word & 31U;
		store.second = (word >> 10) & 31U;
		store.bytes = 16;
		const auto mode = (word >> 23) & 3U;
		const auto displacement =
			sign_extend((word >> 15) & 0x7fU, 7) * 8;
		store.address = mode == 1U ? sp : sp + displacement;
		if (mode == 1U || mode == 3U)
			sp += displacement;
	} else if ((word & 0xffc0'03e0U) == 0xf900'03e0U) {
		store.first = store.second = word & 31U;
		store.bytes = 8;
		store.address = sp + static_cast<std::int64_t>(
			((word >> 10) & 0xfffU) << 3);
	} else {
		store.first = store.second = word & 31U;
		store.bytes = 8;
		const auto mode = (word >> 10) & 3U;
		const auto displacement = sign_extend((word >> 12) & 0x1ffU, 9);
		store.address = mode == 1U ? sp : sp + displacement;
		if (mode != 0U)
			sp += displacement;
	}
	if (store.first == 31U || store.second == 31U)
		return false;
	if (plan.frames != 0 && (store.first == 29U || store.second == 29U))
		return false;
	if (store.bytes == 16) {
		if (plan.pairs == maximum_pairs)
			return false;
		++plan.pairs;
	} else {
		if (plan.singles != 0)
			return false;
		++plan.singles;
	}
	plan.store[plan.stores++] = store;
	return true;
}

/* The head's operand words for the elements [start, end) of `window` into
 * `operands`. The result is the head, without a gadget when it cannot do the
 * elements. */
[[nodiscard]] Head encode(std::span<const std::uint32_t> window,
	std::size_t start, std::size_t end,
	std::array<std::uintptr_t, 3> &operands) noexcept
{
	Plan plan{};
	std::int64_t sp = 0;
	for (std::size_t index = start; index < end; ++index)
		if (!add(window[index], sp, plan))
			return {};
	if (plan.frames != 1 || plan.pairs == 0)
		return {};
	/* The single store goes first, where the head stores 8 bytes. */
	for (std::size_t index = 0; index < plan.stores; ++index)
		if (plan.store[index].bytes == 8)
			std::rotate(plan.store.begin(),
				plan.store.begin() +
					static_cast<std::ptrdiff_t>(index),
				plan.store.begin() +
					static_cast<std::ptrdiff_t>(index) + 1);
	/* The bytes each store writes are its own and no other store's. */
	std::int64_t low = plan.store[0].address;
	std::int64_t limit = low + plan.store[0].bytes;
	for (std::size_t index = 0; index < plan.stores; ++index) {
		const auto &store = plan.store[index];
		if ((store.address & 7) != 0)
			return {};
		for (std::size_t other = 0; other < index; ++other) {
			const auto &earlier = plan.store[other];
			if (store.address < earlier.address + earlier.bytes &&
			    earlier.address < store.address + store.bytes)
				return {};
		}
		low = std::min(low, store.address);
		limit = std::max(limit, store.address + store.bytes);
	}
	const auto last = limit - 1 - low;
	const auto low_units = low / 8;
	const auto frame = plan.frame - low;
	if (low_units < -512 || low_units > 511 ||
	    (sp & 15) != 0 || sp / 16 < -512 || sp / 16 > 511 ||
	    frame < 0 || frame > 8 * 31 || (frame & 7) != 0)
		return {};
	/* A store's place in the span has 5 bits, which keeps the span's last
	 * byte inside its 9. */
	static_assert(8 * 31 + 16 - 1 < 1 << 9);
	for (std::size_t index = 0; index < plan.stores; ++index)
		if (plan.store[index].address - low > 8 * 31)
			return {};
	const auto packed = [&](std::size_t index) {
		const auto &store = plan.store[index];
		return static_cast<std::uintptr_t>((store.address - low) / 8) |
			(static_cast<std::uintptr_t>(store.first) << 5) |
			(static_cast<std::uintptr_t>(store.second) << 10);
	};
	operands = {};
	operands[0] = (static_cast<std::uintptr_t>(low_units) & 0x3ffU) |
		(static_cast<std::uintptr_t>(last) << 10) |
		((static_cast<std::uintptr_t>(sp / 16) & 0x3ffU) << 19) |
		(static_cast<std::uintptr_t>(frame / 8) << 29) |
		(packed(0) << 34);
	for (std::size_t index = 1; index < plan.stores; ++index)
		operands[(index + 2) / 4] |= packed(index) <<
			(index < 2 ? 49 : 15 * ((index - 2) % 4));
	return {heads[plan.pairs - 1][plan.singles][plan.subs], plan.stores};
}

[[nodiscard]] constexpr std::size_t operand_words(std::size_t stores) noexcept
{
	return 1 + (stores + 1) / 4;
}

/* Put a head in front of the longest prologue run that ends right before
 * instruction `end`, whose group is not emitted yet. `starts` holds where each
 * recent instruction's group begins, and the entries of the run's elements
 * move with them. The run's words stay where they are, behind the head, and
 * are its fallback. True when a head was placed: the caller must then not
 * forward a register from the run's last element into instruction `end`,
 * since the head leaves none in the forwarding register. */
bool place(ProgramWords &words, InlineSuffixes &suffixes,
	std::span<const std::uint32_t> window, std::size_t end,
	epilogue::Starts &starts
#if UML_GADGET_EXECUTION_HISTOGRAM
	, const epilogue::Predecessors &predecessors
#endif
	)
{
	std::size_t length = 0;
	while (length < std::min(end, maximum_elements) &&
	       is_element(window[end - 1 - length]))
		++length;
	for (; length > 1; --length) {
		const auto start = end - length;
		if (!starts.grouped(start, end, epilogue::group_words,
			    words.size()))
			continue;
		std::array<std::uintptr_t, 3> operands{};
		const auto [gadget, stores] = encode(window, start, end, operands);
		if (gadget == nullptr)
			continue;
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* As the epilogue head's: the head's group counts the run. */
		for (auto index = start; index < end; ++index)
			words[starts[index] + 2] = 0;
#endif
		const auto head = reinterpret_cast<std::uintptr_t>(gadget);
		const std::size_t at = starts[start];
		const auto inserted = epilogue::count_words + 1 +
			operand_words(stores);
		words.insert(at, inserted);
		auto cursor = at;
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* As the epilogue head's: the run's first word names the head
		 * as its predecessor. */
		words[cursor++] = reinterpret_cast<std::uintptr_t>(
			diagnostics::uml_arm64_gadget_execution_count);
		words[cursor++] = diagnostics::execution_histogram_slot(
			predecessors[start % epilogue::ring],
			reinterpret_cast<const void *>(head));
		words[cursor++] = end - start;
#endif
		words[cursor++] = head;
		for (std::size_t index = 0; index < operand_words(stores); ++index)
			words[cursor++] = operands[index];
#if UML_GADGET_EXECUTION_HISTOGRAM
		words[cursor + 1] = diagnostics::execution_histogram_slot(
			reinterpret_cast<const void *>(head),
			reinterpret_cast<const void *>(
				words[cursor + epilogue::count_words]));
#endif
		starts.shift(start, end, inserted);
		for (std::uint32_t index = 0; index < suffixes.count; ++index)
			if (suffixes.word[index] >= at)
				suffixes.word[index] +=
					static_cast<std::uint32_t>(inserted);
		return true;
	}
	return false;
}

} // namespace prologue

/* A row of register moves that one head replaces (engine/run_heads.inc has
 * what the head does and its operand word). */
namespace moves {

/* Destinations a head writes. */
inline constexpr std::size_t maximum_moves = 4;
/* The fewest instructions a head replaces. */
inline constexpr std::size_t minimum_instructions = 2;
/* The most a head replaces, which keeps the builder's look ahead short. */
inline constexpr std::size_t maximum_instructions = 16;

/* MOV Xd, Xm or MOV Wd, Wm: ORR (shifted register) of XZR and Xm, unshifted,
 * to a register other than XZR. */
[[nodiscard]] constexpr bool is_move(std::uint32_t word) noexcept
{
	return (word & 0x7fe0'ffe0U) == 0x2a00'03e0U && (word & 31U) != 31U;
}

/* By the width of each move, bit n set for a 64-bit move n, and the ordinary
 * form or the one that takes the first source from x0. */
using Head = std::array<void (*)(), 2>;
#define UML_RUN_MOVES(widths) \
	Head{uml_arm64_gadget_run_moves_##widths, \
		uml_arm64_gadget_run_moves_##widths##_forward}
constexpr std::array<Head, 4> two{{
	UML_RUN_MOVES(ww), UML_RUN_MOVES(xw),
	UML_RUN_MOVES(wx), UML_RUN_MOVES(xx),
}};
constexpr std::array<Head, 8> three{{
	UML_RUN_MOVES(www), UML_RUN_MOVES(xww),
	UML_RUN_MOVES(wxw), UML_RUN_MOVES(xxw),
	UML_RUN_MOVES(wwx), UML_RUN_MOVES(xwx),
	UML_RUN_MOVES(wxx), UML_RUN_MOVES(xxx),
}};
constexpr std::array<Head, 16> four{{
	UML_RUN_MOVES(wwww), UML_RUN_MOVES(xwww),
	UML_RUN_MOVES(wxww), UML_RUN_MOVES(xxww),
	UML_RUN_MOVES(wwxw), UML_RUN_MOVES(xwxw),
	UML_RUN_MOVES(wxxw), UML_RUN_MOVES(xxxw),
	UML_RUN_MOVES(wwwx), UML_RUN_MOVES(xwwx),
	UML_RUN_MOVES(wxwx), UML_RUN_MOVES(xxwx),
	UML_RUN_MOVES(wwxx), UML_RUN_MOVES(xwxx),
	UML_RUN_MOVES(wxxx), UML_RUN_MOVES(xxxx),
}};
#undef UML_RUN_MOVES

/* One move of the parallel copy: the CpuState byte offset it reads as it was
 * before the row, and the index of the register it writes. */
struct Move final {
	std::uintptr_t source{};
	unsigned destination{};
	bool wide{};
};

/* The operand word holds a source in 10 bits, which the offset of every
 * register a move reads must fit, XZR's zero slot among them. */
static_assert([] {
	for (unsigned index = 0; index < 32; ++index)
		if (arithmetic_register_offset(index) >= std::uintptr_t{1} << 10)
			return false;
	return true;
}());

struct Plan final {
	std::size_t instructions{};
	std::size_t moves{};
	std::array<Move, maximum_moves> move{};
};

/* The row of moves at `start`, as a parallel copy with the same result: each
 * destination once, in the order the row last writes it, with the source it
 * had before the row and the narrowest width it passed through. The row ends
 * before the first instruction that is not a move or that would make a fifth
 * destination. */
[[nodiscard]] Plan resolve(std::span<const std::uint32_t> window,
	std::size_t start) noexcept
{
	Plan plan{};
	for (std::size_t index = start; index < window.size() &&
	     plan.instructions < maximum_instructions; ++index) {
		const auto word = window[index];
		if (!is_move(word))
			break;
		const unsigned source = (word >> 16) & 31U;
		Move move{arithmetic_register_offset(source), word & 31U,
			(word >> 31) != 0};
		std::size_t written = plan.moves;
		for (std::size_t other = 0; other < plan.moves; ++other) {
			const auto &earlier = plan.move[other];
			if (source != 31U && earlier.destination == source) {
				move.source = earlier.source;
				move.wide = move.wide && earlier.wide;
			}
			if (earlier.destination == move.destination)
				written = other;
		}
		if (written == plan.moves && plan.moves == maximum_moves)
			break;
		if (written != plan.moves) {
			std::copy(plan.move.begin() +
					static_cast<std::ptrdiff_t>(written) + 1,
				plan.move.begin() +
					static_cast<std::ptrdiff_t>(plan.moves),
				plan.move.begin() +
					static_cast<std::ptrdiff_t>(written));
			--plan.moves;
		}
		plan.move[plan.moves++] = move;
		++plan.instructions;
	}
	return plan;
}

/* A head, its operand word, the instructions it replaces and the register
 * it forwards. */
struct Placed final {
	void (*gadget)(){};
	std::uintptr_t operand{};
	std::size_t instructions{};
	std::uint16_t destination{scalar_no_register};
};

/* The head for the row at `start`, or none when the row is shorter than the
 * minimum or copies a single register. `previous` is the register the group
 * in front forwards; the head takes it from x0 for the first of its moves
 * that reads it, unless that move is the last, and forwards the last move's
 * destination. */
[[nodiscard]] Placed encode(std::span<const std::uint32_t> window,
	std::size_t start, std::uint16_t previous, bool forwarding) noexcept
{
	auto plan = resolve(window, start);
	if (plan.instructions < minimum_instructions || plan.moves < 2)
		return {};
	const auto moves = plan.moves;
	std::size_t forwarded = moves;
	if (forwarding && previous != scalar_no_register)
		for (std::size_t index = 0; index + 1 < moves; ++index)
			if (plan.move[index].source == previous) {
				forwarded = index;
				break;
			}
	if (forwarded != moves) {
		const auto first = plan.move.begin() +
			static_cast<std::ptrdiff_t>(forwarded);
		std::rotate(plan.move.begin(), first, first + 1);
	}
	std::uintptr_t operand = 0;
	unsigned widths = 0;
	for (std::size_t index = 0; index < moves; ++index) {
		const auto &move = plan.move[index];
		operand |= (move.source |
			(static_cast<std::uintptr_t>(move.destination) << 10)) <<
			(15 * index);
		widths |= static_cast<unsigned>(move.wide) << index;
	}
	const auto form = static_cast<std::size_t>(forwarded != moves);
	const auto gadget = moves == 2 ? two[widths][form] :
		moves == 3 ? three[widths][form] : four[widths][form];
	return {gadget, operand, plan.instructions, static_cast<std::uint16_t>(
		plan.move[moves - 1].destination * sizeof(std::uint64_t))};
}

} // namespace moves

/* Same-base translation reuse (engine/same_base.inc). */
namespace same_base_pass {

using same_base::Row;

/* The row whose established entry is `established`, or null. Never
 * destroyed, as the scalar contract index is: carriers translate with it until
 * their process ends. */
[[nodiscard]] const Row *find(std::uintptr_t established)
{
	[[clang::no_destroy]] static const auto sorted = [] {
		std::vector<const Row *> rows;
		for (const Row *row = same_base::uml_arm64_gadget_same_base_rows;
		     row != same_base::uml_arm64_gadget_same_base_rows_end;
		     ++row)
			rows.push_back(row);
		std::sort(rows.begin(), rows.end(),
			[](const Row *a, const Row *b) {
				return a->established < b->established;
			});
		return rows;
	}();
	const auto found = std::lower_bound(sorted.begin(), sorted.end(),
		established, [](const Row *row, std::uintptr_t key) {
			return row->established < key;
		});
	return found != sorted.end() && (*found)->established == established ?
		*found : nullptr;
}

/* The latest transfer through each base in each direction, which a later
 * one may follow while no other chain has written x25 since it: `writer` is
 * the ordinal of the latest follower. */
struct Candidate final {
	const Row *row;
	std::int64_t offset;
	std::uint32_t word;
	std::uint32_t ordinal;
	unsigned mask;
	/* The entry before it, which the execution histogram pairs it with. */
	const void *previous;
};

struct State final {
	std::array<std::array<Candidate, 2>, 32> candidates;
	std::array<std::uint32_t, 2> valid{};
	std::int64_t writer = -1;
	std::uint32_t ordinal = 0;

	void kill(std::uint32_t registers) noexcept
	{
		valid[0] &= ~registers;
		valid[1] &= ~registers;
	}

	void kill(std::span<const std::uint32_t> window, std::size_t first,
		std::size_t last) noexcept
	{
		for (std::size_t index = first;
		     index <= last && index < window.size(); ++index)
			kill(same_base::writes(window[index]));
	}

	/* The transfer whose entry is words[word], selected with forwarding
	 * mask `mask`, after the entry `previous`: a follower of the candidate
	 * it follows, which then leads, and the next candidate through its
	 * base. Returns the candidate it rewrote to a leader, if any. */
	std::optional<Candidate> transfer(ProgramWords &words, std::size_t word,
		const Row &row, unsigned mask, const same_base::Access &access,
		const void *previous) noexcept
	{
		const unsigned direction = access.load ? 1U : 0U;
		auto &candidate = candidates[access.base][direction];
		const auto current = ordinal++;
		std::optional<Candidate> led;
		if (((valid[direction] >> access.base) & 1U) != 0 &&
		    static_cast<std::int64_t>(candidate.ordinal) >= writer &&
		    row.follower[mask] != 0 &&
		    candidate.row->leader[candidate.mask] != 0) {
			const auto d = access.offset - candidate.offset;
			const bool narrow = row.narrow_displacement(mask);
			if (narrow ? d >= -128 && d <= 127 : d > -4096 && d < 4096) {
				if (static_cast<std::int64_t>(candidate.ordinal) >
				    writer) {
					words[candidate.word] =
						candidate.row->leader[candidate.mask];
					led = candidate;
				}
				words[word] = row.follower[mask];
				const auto field = narrow ?
					std::uintptr_t{0xff} << 48 : std::uintptr_t{0xffff};
				const auto value = narrow ?
					(static_cast<std::uintptr_t>(d) & 0xffU) << 48 :
					static_cast<std::uintptr_t>(d) & 0xffffU;
				words[word + 1] = (words[word + 1] & ~field) | value;
				writer = current;
			}
		}
		candidate = {&row, access.offset, static_cast<std::uint32_t>(word),
			current, mask, previous};
		valid[direction] |= same_base::bit(access.base);
		kill(access.written);
		return led;
	}
};

#if UML_GADGET_EXECUTION_HISTOGRAM
/* A candidate rewritten to its leader is paired anew: its own counter with
 * the entry before it, and the counter after its operand with it. Returns
 * the predecessor of the entry at `current`, which may be that next one. */
const void *recount(ProgramWords &words, const Candidate &led,
	std::size_t current, const void *previous)
{
	const auto *leader = reinterpret_cast<const void *>(words[led.word]);
	words[led.word - 2] =
		diagnostics::execution_histogram_slot(led.previous, leader);
	const std::size_t next = led.word + 2U;
	if (next + 3U < words.size() &&
	    words[next] == reinterpret_cast<std::uintptr_t>(
		    &diagnostics::uml_arm64_gadget_execution_count))
		words[next + 1] = diagnostics::execution_histogram_slot(leader,
			reinterpret_cast<const void *>(words[next + 3]));
	return next + 3U == current ? leader : previous;
}
#endif

} // namespace same_base_pass

} // namespace

BuildResult Builder::build(
	std::uint64_t guest_address,
	std::span<const std::uint32_t> instructions,
	LinkResolver resolve_link, std::pmr::memory_resource *storage,
	IndirectLinkResolver resolve_indirect_link, BuildOptions options) const
{
	const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
		options.host_capabilities};
	ProgramWords words{storage};
	InlineSuffixes suffixes{};
	std::size_t compiled = 0;
	bool program_accesses_fp_state = false;
	/* A transfer without a PC word names its instruction by an 8-bit
	 * position in the block, which also keeps every packed inline suffix
	 * within sixteen bits. */
	const auto window = instructions.first(std::min(instructions.size(),
		fault_position_instructions));
	/* A program for SVCR.SM = 1 selects only entries that are safe in
	 * streaming mode; forwarding contracts do not reach into them, and its
	 * executor loads the FP state before it enters, so it needs no FP link
	 * entry. */
	const bool streaming = (options.svcr & svcr_sm) != 0U;
	if (streaming) [[unlikely]] {
		options.scalar_forwarding = false;
		options.vector_forwarding = false;
	}
	const auto finish = [&](std::size_t retired) {
		resolve_inline_suffixes(words, suffixes, retired);
		return Program{guest_address, retired, std::move(words),
			program_accesses_fp_state && !streaming};
	};
#if UML_GADGET_EXECUTION_HISTOGRAM
	const void *previous_gadget = nullptr;
#endif
	std::uint16_t previous_scalar = scalar_no_register;
	std::uint16_t previous_vector = scalar_no_register;
	/* No streaming program takes part: its entries are streaming twins. */
	const bool same_base = !streaming && options.same_base;
	same_base_pass::State same_base_state;
	/* FIZ, AH and NEP are guest state only where the virtual CPU implements
	 * FEAT_AFP; there every entry that would ignore them runs as its twin,
	 * which carries no forwarding contract. */
	const bool afp = implements_afp(options.host_capabilities);
	epilogue::Starts group_starts{};
#if UML_GADGET_EXECUTION_HISTOGRAM
	epilogue::Predecessors group_predecessors{};
#endif
	/* Prologue and move heads go where epilogue heads do: not in a
	 * streaming program and not in one built without indirect sites, as a
	 * single step is. */
	const bool run_heads = !streaming &&
		static_cast<bool>(resolve_indirect_link);
	const auto place_prologue = [&] {
		return run_heads && compiled != 0 &&
			prologue::is_element(window[compiled - 1]) &&
			prologue::place(words, suffixes, window, compiled,
				group_starts
#if UML_GADGET_EXECUTION_HISTOGRAM
				, group_predecessors
#endif
				);
	};
	/* Indexed rather than ranged, because a decoder that fuses a pair needs to
	 * see what follows the instruction it was handed. `compiled` is the loop
	 * position as well as the retired count, and a fusing decoder advances it
	 * past both instructions itself. */
	while (compiled < window.size()) {
		const auto instruction = window[compiled];
		/* A run ends at the first instruction that is not an element of
		 * one, before that instruction's group begins. Behind a head no
		 * register is forwarded into it. */
		if (!prologue::is_element(instruction) && place_prologue()) {
			previous_scalar = scalar_no_register;
			previous_vector = scalar_no_register;
		}
		program_accesses_fp_state = program_accesses_fp_state ||
			uml::cpu::arm64::detail::
				instruction_accesses_fp_state(instruction);
		const auto address = guest_address + compiled * sizeof(instruction);
		group_starts.begin(compiled, words.size());
#if UML_GADGET_EXECUTION_HISTOGRAM
		group_predecessors[compiled % epilogue::ring] = previous_gadget;
		const auto counter_word = words.size();
		emit(words, diagnostics::uml_arm64_gadget_execution_count);
		emit(words, std::uintptr_t{sizeof(std::uint64_t)});
		emit(words, std::uintptr_t{1});
#endif
		/* A row of moves is one group, its head's, which forwards the
		 * register the row writes last. */
		if (run_heads && moves::is_move(instruction)) {
			const auto head = moves::encode(window, compiled,
				previous_scalar, options.scalar_forwarding);
			if (head.gadget != nullptr) {
				emit(words, head.gadget);
				emit(words, head.operand);
				for (std::size_t index = 1; index < head.instructions;
				     ++index) {
					group_starts.join(compiled + index);
#if UML_GADGET_EXECUTION_HISTOGRAM
					group_predecessors[(compiled + index) %
						epilogue::ring] = previous_gadget;
#endif
				}
#if UML_GADGET_EXECUTION_HISTOGRAM
				words[counter_word + 1] =
					diagnostics::execution_histogram_slot(
						previous_gadget,
						reinterpret_cast<const void *>(
							head.gadget));
				words[counter_word + 2] = head.instructions;
				previous_gadget =
					reinterpret_cast<const void *>(head.gadget);
#endif
				previous_scalar = head.destination;
				previous_vector = scalar_no_register;
				if (same_base)
					same_base_state.kill(window, compiled,
						compiled + head.instructions - 1);
				compiled += head.instructions;
				continue;
			}
		}
		if ((instruction & 0xffff'0000U) == 0) {
			emit(words, uml_arm64_gadget_illegal);
			emit(words, address);
			++compiled;
			return {finish(compiled), true, address, instruction};
		}

		const auto compiled_before = compiled;
		DecodeContext context{
			guest_address,
			address,
			instruction,
			window.subspan(compiled + 1),
			compiled,
			program_accesses_fp_state,
			words,
			resolve_link,
			resolve_indirect_link,
			options,
			suffixes,
		};
		const auto emitted_start = words.size();
		DecodeResult decoded = DecodeResult::decoded;
		switch (production_decode_starts[instruction >> 24]) {
		case ProductionDecodeStart::branch_system:
			decoded = decode_branch_system(context);
			break;
		case ProductionDecodeStart::data_processing_immediate:
		case ProductionDecodeStart::data_processing_register:
			decoded = decode_data_processing(context);
			break;
		case ProductionDecodeStart::data_processing_pointer_auth_overlap:
			decoded = pointer_auth_lr_unavailable(instruction) ?
				decode_branch_system(context) :
				decode_data_processing(context);
			break;
		case ProductionDecodeStart::floating_simd:
			decoded = decode_floating_simd(context);
			break;
		case ProductionDecodeStart::load_store:
			decoded = decode_load_store(context);
			break;
		case ProductionDecodeStart::exception:
			decoded = decode_exception(context);
			break;
		}
		if (afp && words.size() > emitted_start)
			words[emitted_start] = afp_twin(words[emitted_start]);
		const auto current_vector = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			vector_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto vector_mask = current_vector.source_mask(previous_vector);
		previous_vector = current_vector.destination;
		// The selected assembly entry owns its access description. Do not
		// infer it again from opcode bits: aliases and fused groups differ.
		const auto current_scalar = decoded == DecodeResult::decoded &&
			compiled == compiled_before ?
			scalar_access(std::span{words.data(), words.size()}.subspan(emitted_start)) : ScalarAccess{};
		const auto source_mask = current_scalar.source_mask(previous_scalar);
		if (source_mask != 0 && options.scalar_forwarding)
			words[emitted_start] = current_scalar.contract->entries[source_mask];
		previous_scalar = current_scalar.destination;
		if (vector_mask != 0 && options.vector_forwarding)
			words[emitted_start] = current_vector.contract->entries[vector_mask];
		if (streaming && words.size() > emitted_start &&
		    !select_streaming_entries(std::span{words.data(),
			    words.size()}.subspan(emitted_start))) [[unlikely]] {
			/* The base entries are unsafe in streaming mode: an
			 * FP&SIMD class legal there takes its generated form, and
			 * any other word ends the block at the illegal exit, as
			 * an unimplemented word does. */
			words.truncate(emitted_start);
			while (suffixes.count != 0 &&
			       suffixes.word[suffixes.count - 1] >= emitted_start)
				--suffixes.count;
			compiled = compiled_before;
			decoded = decode_streaming_fallback(context);
		}
		/* A run element is never a leader or a follower: a head may do it
		 * without dispatching its word. */
		if (same_base) {
			bool handled = false;
			const auto mask = options.scalar_forwarding ? source_mask : 0U;
			if (current_scalar.contract != nullptr &&
			    words.size() == emitted_start + 2U && mask < 4U &&
			    words[emitted_start] ==
				    current_scalar.contract->entries[mask] &&
			    !prologue::is_element(instruction) &&
			    !epilogue::is_element(instruction)) {
				const auto *row = same_base_pass::find(
					current_scalar.contract->entries[0]);
				const auto access = same_base::scope_access(instruction);
				if (row != nullptr && access &&
				    access->family == row->family()) {
#if UML_GADGET_EXECUTION_HISTOGRAM
					if (const auto led = same_base_state.transfer(
						    words, emitted_start, *row, mask,
						    *access, previous_gadget))
						previous_gadget = same_base_pass::recount(
							words, *led, emitted_start,
							previous_gadget);
#else
					(void)same_base_state.transfer(words,
						emitted_start, *row, mask, *access,
						nullptr);
#endif
					handled = true;
				}
			}
			if (!handled)
				same_base_state.kill(window, compiled_before, compiled);
		}
#if UML_GADGET_EXECUTION_HISTOGRAM
		/* The entry is final only after forwarding selection above. */
		if (words.size() > emitted_start) {
			words[counter_word + 1] = diagnostics::execution_histogram_slot(
				previous_gadget,
				reinterpret_cast<const void *>(words[emitted_start]));
			words[counter_word + 2] = std::max<std::size_t>(1,
				compiled - compiled_before +
				(decoded == DecodeResult::decoded));
		}
		/* The first word a decode emits is its gadget pointer, which the
		 * execution histogram pairs with its predecessor. */
		if (words.size() > emitted_start)
			previous_gadget = reinterpret_cast<const void *>(
				words[emitted_start]);
#endif
		if (decoded == DecodeResult::terminal && !streaming &&
		    instruction == 0xd65f'03c0U && compiled == compiled_before + 1)
			epilogue::place(words, suffixes, window, compiled_before,
				group_starts
#if UML_GADGET_EXECUTION_HISTOGRAM
				, group_predecessors
#endif
				);
		if (decoded != DecodeResult::decoded)
			return {finish(compiled), decoded == DecodeResult::terminal,
				address, instruction};
		/* What a fusing decoder took in behind this instruction is part
		 * of its group. */
		for (auto index = compiled_before + 1; index <= compiled; ++index)
			group_starts.join(index);
		++compiled;
	}
	(void)place_prologue();
	const auto fallthrough = guest_address + compiled * sizeof(std::uint32_t);
	if (resolve_link) {
		emit(words, uml_arm64_gadget_branch_linked);
		emit(words, reinterpret_cast<std::uintptr_t>(
			resolve_link(fallthrough)));
	} else {
		emit(words, uml_arm64_gadget_interrupt);
		emit(words, fallthrough);
	}
	return {finish(compiled), true, fallthrough, 0};
}

} // namespace uml::cpu::arm64::gadgets
