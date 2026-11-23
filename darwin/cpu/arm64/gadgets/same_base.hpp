// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Same-base translation reuse (engine/same_base.inc): the builder's view of
 * the transfers that take part and of what every other instruction writes. */

#include <array>
#include <cstdint>
#include <optional>

namespace uml::cpu::arm64::gadgets::same_base {

/* One row of engine/same_base.inc's table. */
struct Row final {
	std::uintptr_t established;
	/* By the forwarding mask the builder selected; 0 where that variant
	 * has none. */
	std::array<std::uintptr_t, 4> leader;
	std::array<std::uintptr_t, 4> follower;
	std::uint64_t info;

	[[nodiscard]] unsigned family() const noexcept
	{
		return static_cast<unsigned>(info & 0xffU);
	}
	/* d in operand bits 55:48 (signed 8 bits), else in 15:0 (16 bits). */
	[[nodiscard]] bool narrow_displacement(unsigned mask) const noexcept
	{
		return ((info >> (mask == 2U ? 9U : 8U)) & 1U) != 0;
	}
};
static_assert(sizeof(Row) == 80);

/* The table's bounds; the rows are in no particular order. */
extern "C" const Row uml_arm64_gadget_same_base_rows[];
extern "C" const Row uml_arm64_gadget_same_base_rows_end[];

enum Family : unsigned {
	inplace = 0,
	narrow = 1,
	unscaled = 2,
	vector_single = 3,
	integer_pair = 4,
	vector_pair = 5,
};

[[nodiscard]] constexpr std::uint32_t bit(unsigned r) noexcept
{
	return std::uint32_t{1} << (r & 31U);
}

/* A transfer the table can hold: its base (31 = SP), direction, byte
 * offset from the base, and the registers it writes (bit 31 = SP). */
struct Access final {
	unsigned base;
	bool load;
	std::int64_t offset;
	std::uint32_t written;
	unsigned family;
};

[[nodiscard]] constexpr std::int64_t sign_extend(std::uint32_t value,
	unsigned bits) noexcept
{
	const auto shift = 64U - bits;
	return static_cast<std::int64_t>(std::uint64_t{value} << shift) >> shift;
}

/* LDR/STR (unsigned offset), LDUR/STUR and LDTR/STTR, and LDP/STP/LDNP/STNP
 * with a signed offset, general and SIMD&FP; nothing else. */
[[nodiscard]] constexpr std::optional<Access> scope_access(
	std::uint32_t w) noexcept
{
	const unsigned rt = w & 31U;
	const unsigned rn = (w >> 5) & 31U;
	const bool vector = ((w >> 26) & 1U) != 0;
	const unsigned size = w >> 30;
	if ((w & 0x3b00'0000U) == 0x3900'0000U ||
	    (w & 0x3b20'0000U) == 0x3800'0000U) {
		const bool unsigned_offset = (w & 0x0100'0000U) != 0;
		const unsigned opc = (w >> 22) & 3U;
		const unsigned mode = (w >> 10) & 3U;
		if (!unsigned_offset && (mode == 1U || mode == 3U ||
					 (vector && mode == 2U)))
			return std::nullopt; /* post-, pre-index */
		unsigned scale = size;
		bool load = false;
		unsigned family = unscaled;
		if (vector) {
			if ((opc & 2U) != 0) {
				if (size != 0)
					return std::nullopt;
				scale = 4;
			}
			load = (opc & 1U) != 0;
			family = vector_single;
		} else {
			if ((size == 3U && opc >= 2U) || (size == 2U && opc == 3U))
				return std::nullopt; /* PRFM, unallocated */
			load = opc != 0U;
			if (unsigned_offset)
				family = size >= 2U && opc <= 1U ? inplace : narrow;
		}
		Access access{rn, load, 0, 0, family};
		access.offset = unsigned_offset ?
			static_cast<std::int64_t>(((w >> 10) & 0xfffU) << scale) :
			sign_extend((w >> 12) & 0x1ffU, 9);
		if (load && !vector && rt != 31U)
			access.written = bit(rt);
		return access;
	}
	if ((w & 0x3a00'0000U) == 0x2800'0000U) {
		const unsigned mode = (w >> 23) & 3U;
		const bool load = ((w >> 22) & 1U) != 0;
		if (mode != 0U && mode != 2U)
			return std::nullopt; /* pre-, post-index */
		unsigned element = 0;
		if (vector) {
			if (size == 3U)
				return std::nullopt;
			element = 4U << size;
		} else {
			if (size == 3U || (size == 1U && !load))
				return std::nullopt; /* unallocated, STGP */
			element = size == 2U ? 8U : 4U;
		}
		Access access{rn, load, 0, 0, vector ? vector_pair : integer_pair};
		access.offset = sign_extend((w >> 15) & 0x7fU, 7) *
			static_cast<std::int64_t>(element);
		if (load && !vector) {
			const unsigned rt2 = (w >> 10) & 31U;
			if (rt != 31U)
				access.written |= bit(rt);
			if (rt2 != 31U)
				access.written |= bit(rt2);
		}
		return access;
	}
	return std::nullopt;
}

/* The general registers (bit 31 = SP) an instruction may write, as the
 * builder's kill set: exact for the classes translated code is made of, every
 * register named for a rarer load or store, and every bit for anything else.
 * A flag-setting or XZR destination is not SP. */
[[nodiscard]] constexpr std::uint32_t writes(std::uint32_t w) noexcept
{
	constexpr std::uint32_t all = ~std::uint32_t{0};
	const unsigned rd = w & 31U;
	const auto xzr_or = [](unsigned r) { return r == 31U ? 0U : bit(r); };
	const unsigned op0 = (w >> 25) & 15U;
	if ((op0 & 0b1110U) == 0b1000U) {
		/* Data processing, immediate. */
		const unsigned kind = (w >> 23) & 7U;
		const bool flags = ((w >> 29) & 1U) != 0;
		if (kind == 2U) /* add/sub: SP unless flag-setting */
			return flags ? xzr_or(rd) : bit(rd);
		if (kind == 3U) /* with tags, min/max */
			return bit(rd);
		if (kind == 4U) /* logical: SP unless ANDS */
			return ((w >> 29) & 3U) == 3U ? xzr_or(rd) : bit(rd);
		return xzr_or(rd); /* PC-relative, move wide, bitfield, extract */
	}
	if ((op0 & 0b1110U) == 0b1010U) {
		/* Branches, exceptions, system. */
		if ((w & 0xff00'0000U) == 0x5400'0000U)
			return 0; /* B.cond, BC.cond */
		if ((w & 0x7c00'0000U) == 0x1400'0000U)
			return (w >> 31) != 0 ? bit(30) : 0U; /* BL, B */
		if ((w & 0x7e00'0000U) == 0x3400'0000U ||
		    (w & 0x7e00'0000U) == 0x3600'0000U)
			return 0; /* CBZ, CBNZ, TBZ, TBNZ */
		if ((w & 0xff00'0000U) == 0xd400'0000U)
			return 0; /* exception generation */
		if ((w & 0xfe00'0000U) == 0xd600'0000U)
			return bit(30); /* branch to register */
		if ((w & 0xffc0'0000U) == 0xd500'0000U) {
			if (((w >> 21) & 1U) != 0)
				return xzr_or(rd); /* MRS, SYSL */
			if ((w & 0xffff'f01fU) == 0xd503'201fU)
				return bit(16) | bit(17) | bit(30); /* hints */
			return 0; /* MSR, SYS, barriers, PSTATE */
		}
		return all;
	}
	if ((op0 & 0b0111U) == 0b0101U) {
		/* Data processing, register. */
		if ((w & 0x1fe0'0000U) == 0x1a40'0000U)
			return 0; /* CCMP, CCMN */
		if ((w & 0x1f20'0000U) == 0x0b20'0000U) /* extended register */
			return ((w >> 29) & 1U) != 0 ? xzr_or(rd) : bit(rd);
		return xzr_or(rd);
	}
	if ((op0 & 0b0111U) == 0b0111U) {
		/* SIMD&FP: only the forms that write a general register. */
		if ((w & 0x5f20'fc00U) == 0x1e20'0000U) {
			const unsigned opcode = (w >> 16) & 7U;
			return opcode == 2U || opcode == 3U || opcode == 7U ?
				0U : xzr_or(rd);
		}
		if ((w & 0x5f20'0000U) == 0x1e00'0000U)
			return ((w >> 16) & 7U) <= 1U ? xzr_or(rd) : 0U;
		if ((w & 0x9fe0'8400U) == 0x0e00'0400U) {
			const unsigned imm4 = (w >> 11) & 15U;
			return imm4 == 5U || imm4 == 7U ? xzr_or(rd) : 0U;
		}
		return 0;
	}
	if ((w & 0x0a00'0000U) != 0x0800'0000U)
		return all; /* SME, SVE, unallocated */
	/* Loads and stores. */
	const unsigned rn = (w >> 5) & 31U;
	const unsigned rt2 = (w >> 10) & 31U;
	const unsigned rs = (w >> 16) & 31U;
	const bool vector = ((w >> 26) & 1U) != 0;
	if (const auto access = scope_access(w))
		return access->written;
	if ((w & 0x3b00'0000U) == 0x1800'0000U) /* literal; PRFM writes none */
		return !vector && (w >> 30) != 3U ? xzr_or(rd) : 0U;
	if ((w & 0x3a00'0000U) == 0x3800'0000U) {
		/* Single register: pre/post-index, register offset, atomics.
		 * Bits 23:22 are opc everywhere but in the atomic cell (bit 21
		 * set, bits 11:10 clear), where they are A:R, so PRFM is known
		 * by its three cells, never by opc alone. */
		const unsigned opc = (w >> 22) & 3U;
		const bool unsigned_offset = (w & 0x0100'0000U) != 0;
		const bool bit21 = ((w >> 21) & 1U) != 0;
		const unsigned cell = (w >> 10) & 3U;
		if (!vector && (w >> 30) == 3U && opc == 2U &&
		    (unsigned_offset || (!bit21 && cell == 0U) ||
		     (bit21 && cell == 2U)))
			return 0; /* PRFM (immediate, register), PRFUM */
		if (unsigned_offset)
			return all; /* unallocated unsigned-offset forms */
		const bool load = vector ? (opc & 1U) != 0 : opc != 0U;
		if ((w & 0x0120'0000U) == 0) /* pre-, post-index */
			return (load && !vector ? xzr_or(rd) : 0U) | bit(rn);
		if ((w & 0x0120'0c00U) == 0x0020'0800U) /* register offset */
			return load && !vector ? xzr_or(rd) : 0U;
		if ((w & 0x0120'0c00U) == 0x0020'0000U && !vector) {
			/* LSE: LDADD to LDUMIN, SWP and LDAPR write Rt only. */
			const unsigned o3 = (w >> 15) & 1U;
			const unsigned opc = (w >> 12) & 7U;
			if (o3 == 0U || opc == 0U || opc == 4U)
				return xzr_or(rd);
			return all; /* LD64B and the other FEAT_LS64 forms */
		}
	}
	if ((w & 0x3a00'0000U) == 0x2800'0000U) /* pairs with writeback */
		return (((w >> 22) & 1U) != 0 && !vector ?
			xzr_or(rd) | xzr_or(rt2) : 0U) | bit(rn);
	if ((w & 0xbf00'0000U) == 0x0c00'0000U ||
	    (w & 0xbf00'0000U) == 0x0d00'0000U) /* SIMD structures */
		return ((w >> 23) & 1U) != 0 ? bit(rn) : 0U;
	/* Every other load or store: each register it names, CASP's second
	 * register of a pair, and the base. */
	return bit(rd) | bit(rd + 1U) | bit(rt2) | bit(rs) | bit(rs + 1U) |
		bit(rn);
}

} // namespace uml::cpu::arm64::gadgets::same_base
