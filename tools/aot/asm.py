# SPDX-License-Identifier: GPL-2.0-or-later
"""A64 encoders and a small label-resolving assembler with branch relaxation.

Only the instructions the rewriter emits are encoded here. Everything the
guest wrote is copied or re-encoded from its own word, never re-assembled.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import bisect
import struct
from array import array

XZR = 31
SP = 31


class AsmError(Exception):
    pass


def _u(value: int, bits: int) -> int:
    if not 0 <= value < (1 << bits):
        raise AsmError(f"{value} does not fit in {bits} bits")
    return value


def _s(value: int, bits: int) -> int:
    low = -(1 << (bits - 1))
    if not low <= value < (1 << (bits - 1)):
        raise AsmError(f"{value} does not fit in {bits} signed bits")
    return value & ((1 << bits) - 1)


# ---- data processing ----------------------------------------------------

def add_imm(rd: int, rn: int, imm: int, *, sf: int = 1, sub: bool = False,
            shift12: bool = False) -> int:
    return ((sf << 31) | (int(sub) << 30) | (0b100010 << 23) |
            (int(shift12) << 22) | (_u(imm, 12) << 10) | (rn << 5) | rd)


def movz(rd: int, imm16: int, hw: int = 0, *, sf: int = 1) -> int:
    return ((sf << 31) | (0b10 << 29) | (0b100101 << 23) | (hw << 21) |
            (_u(imm16, 16) << 5) | rd)


def movk(rd: int, imm16: int, hw: int = 0, *, sf: int = 1) -> int:
    return ((sf << 31) | (0b11 << 29) | (0b100101 << 23) | (hw << 21) |
            (_u(imm16, 16) << 5) | rd)


def ubfm(rd: int, rn: int, immr: int, imms: int, *, sf: int = 1) -> int:
    return ((sf << 31) | (0b10 << 29) | (0b100110 << 23) | (sf << 22) |
            (_u(immr, 6) << 16) | (_u(imms, 6) << 10) | (rn << 5) | rd)


def ubfx(rd: int, rn: int, lsb: int, width: int) -> int:
    return ubfm(rd, rn, lsb, lsb + width - 1)


def lsr_imm(rd: int, rn: int, amount: int) -> int:
    return ubfm(rd, rn, amount, 63)


def ror_imm(rd: int, rn: int, amount: int) -> int:
    return ((1 << 31) | (0b100111 << 23) | (1 << 22) | (rn << 16) |
            (_u(amount, 6) << 10) | (rn << 5) | rd)


def add_reg(rd: int, rn: int, rm: int, *, shift: int = 0, amount: int = 0,
            sub: bool = False) -> int:
    return ((1 << 31) | (int(sub) << 30) | (0b01011 << 24) |
            (shift << 22) | (rm << 16) | (_u(amount, 6) << 10) | (rn << 5) | rd)


def add_ext(rd: int, rn: int, rm: int, option: int, amount: int) -> int:
    """ADD (extended register), 64-bit: option 0b010 UXTW, 0b011 UXTX/LSL,
    0b110 SXTW, 0b111 SXTX."""
    return ((1 << 31) | (0b01011001 << 21) | (rm << 16) | (option << 13) |
            (_u(amount, 3) << 10) | (rn << 5) | rd)


def eor_reg(rd: int, rn: int, rm: int, *, shift: int = 0, amount: int = 0
            ) -> int:
    return ((1 << 31) | (0b10 << 29) | (0b01010 << 24) | (shift << 22) |
            (rm << 16) | (_u(amount, 6) << 10) | (rn << 5) | rd)


def and_ones(rd: int, rn: int, ones: int) -> int:
    """AND (immediate), 64-bit, with the mask of `ones` low bits set."""
    return 0x92400000 | ((_u(ones, 6) - 1) << 10) | (rn << 5) | rd


def orr_ones(rd: int, rn: int, ones: int) -> int:
    """ORR (immediate), 64-bit, with the mask of `ones` low bits set (a
    register-to-register move of the mask when rn is XZR)."""
    return 0xb2400000 | ((_u(ones, 6) - 1) << 10) | (rn << 5) | rd


def cmp_reg(rn: int, rm: int) -> int:
    """SUBS XZR, Rn, Rm (64-bit)."""
    return 0xeb00001f | (rm << 16) | (rn << 5)


def csel(rd: int, rn: int, rm: int, cond: int) -> int:
    return 0x9a800000 | (rm << 16) | (_u(cond, 4) << 12) | (rn << 5) | rd


def ccmp_imm(rn: int, imm5: int, nzcv: int, cond: int) -> int:
    """CCMP (immediate), 64-bit."""
    return (0xfa400800 | (_u(imm5, 5) << 16) | (_u(cond, 4) << 12) |
            (rn << 5) | _u(nzcv, 4))


def orr_reg(rd: int, rn: int, rm: int, *, shift: int = 0, amount: int = 0
            ) -> int:
    """ORR (shifted register), 64-bit."""
    return ((1 << 31) | (0b01 << 29) | (0b01010 << 24) | (shift << 22) |
            (rm << 16) | (_u(amount, 6) << 10) | (rn << 5) | rd)


def movz_hw(rd: int, imm16: int, hw: int) -> int:
    return movz(rd, imm16, hw)


def mov_reg(rd: int, rm: int) -> int:
    return (1 << 31) | (0b01 << 29) | (0b01010 << 24) | (rm << 16) | \
        (XZR << 5) | rd


def nop() -> int:
    return 0xd503201f


# ---- memory --------------------------------------------------------------

def ldst_imm(rt: int, rn: int, imm: int, size: int, *, load: bool,
             vector: bool = False, opc: int | None = None) -> int:
    """Unsigned-offset LDR/STR; imm is in bytes and a multiple of the size."""
    if opc is None:
        opc = 1 if load else 0
    scale = size if not (vector and opc & 2) else 4
    if imm % (1 << scale):
        raise AsmError("unaligned scaled offset")
    return ((size << 30) | (0b111 << 27) | (int(vector) << 26) | (1 << 24) |
            (opc << 22) | (_u(imm >> scale, 12) << 10) | (rn << 5) | rt)


def ldst_unscaled(rt: int, rn: int, imm: int, size: int, *, load: bool,
                  vector: bool = False) -> int:
    """LDUR/STUR (a signed 9-bit byte offset); size 0-3 is B, H, W, X, and
    with `vector` and size 0 it is the 128-bit Q register."""
    opc = (3 if load else 2) if vector else (1 if load else 0)
    return ((size << 30) | (0b111 << 27) | (int(vector) << 26) |
            (opc << 22) | (_s(imm, 9) << 12) | (rn << 5) | rt)


def ldr_x(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 3, load=True)


def str_x(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 3, load=False)


def ldr_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 2, load=True)


def str_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 2, load=False)


def ldrb_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=True)


def strb_w(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=False)


def ldr_q(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=True, vector=True, opc=3)


def str_q(rt: int, rn: int, imm: int) -> int:
    return ldst_imm(rt, rn, imm, 0, load=False, vector=True, opc=2)


def ldr_reg_x(rt: int, rn: int, rm: int) -> int:
    """LDR Xt, [Xn, Xm] (register offset, LSL #0)."""
    return 0xf8606800 | (rm << 16) | (rn << 5) | rt


def ldr_reg_w(rt: int, rn: int, rm: int) -> int:
    """LDR Wt, [Xn, Xm]."""
    return 0xb8606800 | (rm << 16) | (rn << 5) | rt


def ldrh_reg(rt: int, rn: int, rm: int) -> int:
    """LDRH Wt, [Xn, Xm, LSL #1]."""
    return 0x78607800 | (rm << 16) | (rn << 5) | rt


def ldrsw_reg(rt: int, rn: int, rm: int) -> int:
    """LDRSW Xt, [Xn, Xm, LSL #2]."""
    return 0xb8a07800 | (rm << 16) | (rn << 5) | rt


def ldp_x(rt: int, rt2: int, rn: int, imm: int, *, load: bool = True) -> int:
    return ((0b10 << 30) | (0b101 << 27) | (0b010 << 23) |
            (int(load) << 22) | (_s(imm >> 3, 7) << 15) | (rt2 << 10) |
            (rn << 5) | rt)


def stp_x(rt: int, rt2: int, rn: int, imm: int) -> int:
    return ldp_x(rt, rt2, rn, imm, load=False)


def ldp_q(rt: int, rt2: int, rn: int, imm: int, *, load: bool = True) -> int:
    return ((0b10 << 30) | (0b101 << 27) | (1 << 26) | (0b010 << 23) |
            (int(load) << 22) | (_s(imm >> 4, 7) << 15) | (rt2 << 10) |
            (rn << 5) | rt)


def stp_q(rt: int, rt2: int, rn: int, imm: int) -> int:
    return ldp_q(rt, rt2, rn, imm, load=False)


def ldp_d(rt: int, rt2: int, rn: int, imm: int, *, load: bool = True) -> int:
    return ((0b01 << 30) | (0b101 << 27) | (1 << 26) | (0b010 << 23) |
            (int(load) << 22) | (_s(imm >> 3, 7) << 15) | (rt2 << 10) |
            (rn << 5) | rt)


# ---- system --------------------------------------------------------------

SYSREG = {"nzcv": 0xd53b4200, "fpcr": 0xd53b4400, "fpsr": 0xd53b4420}


def mrs(rt: int, name: str) -> int:
    return SYSREG[name] | rt


def msr(name: str, rt: int) -> int:
    return (SYSREG[name] ^ 0x00200000) | rt


def ret_() -> int:
    return 0xd65f03c0


def blr(rn: int) -> int:
    return 0xd63f0000 | (rn << 5)


def br(rn: int) -> int:
    return 0xd61f0000 | (rn << 5)


def ldrb_zero_ext(rt: int, rn: int, imm: int) -> int:
    return ldrb_w(rt, rn, imm)


# ---- control flow words, targets patched by the assembler ----------------

def _branch_reach(delta_words: int) -> int:
    """The 26-bit word displacement of b and bl, or the error that names the
    limit a unit's size actually has: the exit, refill and cross-library
    routines sit at the unit's ends, so every site reaches them by one b."""
    if not -(1 << 25) <= delta_words < (1 << 25):
        raise AsmError(
            f"a branch of {4 * delta_words} bytes is beyond the +-128 MiB "
            "reach of b and bl, which limits a unit to about that size; a "
            "larger unit needs a copy of its routines per 128 MiB or a "
            "veneer (adrp, add, br)")
    return delta_words & ((1 << 26) - 1)


def b_word(delta_words: int) -> int:
    return (0b000101 << 26) | _branch_reach(delta_words)


def bcond_word(cond: int, delta_words: int) -> int:
    return 0x54000000 | (_s(delta_words, 19) << 5) | cond


def cbz_word(rt: int, delta_words: int, *, sf: int = 1, nz: bool = False
             ) -> int:
    return ((sf << 31) | (0b011010 << 25) | (int(nz) << 24) |
            (_s(delta_words, 19) << 5) | rt)


def tbz_word(rt: int, bit: int, delta_words: int, *, nz: bool = False) -> int:
    return (((bit >> 5) << 31) | (0b011011 << 25) | (int(nz) << 24) |
            ((bit & 31) << 19) | (_s(delta_words, 14) << 5) | rt)


def adr_word(rd: int, delta: int) -> int:
    delta &= (1 << 21) - 1
    return ((delta & 3) << 29) | (0b10000 << 24) | ((delta >> 2) << 5) | rd


def adrp_word(rd: int, delta_pages: int) -> int:
    delta_pages &= (1 << 21) - 1
    return (1 << 31) | ((delta_pages & 3) << 29) | (0b10000 << 24) | \
        ((delta_pages >> 2) << 5) | rd


# ---- the assembler -------------------------------------------------------

_B, _BC, _CBZ, _TBZ, _ADRP = range(5)


class Assembler:
    """Collects words and label references, then lays them out.

    Plain words live in one array. A word that names a label is recorded in
    a side table at its position and is encoded when the layout is known; a
    conditional branch out of range is relaxed to an inverted branch over an
    unconditional one, which adds one word at that position. An address is
    its word index plus the number of added words before it. The assembler is
    sized for blobs of tens of millions of words.
    """

    def __init__(self) -> None:
        self._w = array("I")
        self._sp: dict[int, tuple] = {}
        self._labels: dict[str, int] = {}
        self._pending: list[str] = []
        self._ex: list[int] = []
        self._resolved = False

    # emission
    def label(self, name: str) -> None:
        if name in self._labels or name in self._pending:
            raise AsmError(f"duplicate label {name}")
        self._pending.append(name)

    def _here(self) -> int:
        position = len(self._w)
        if self._pending:
            for name in self._pending:
                self._labels[name] = position
            self._pending.clear()
        return position

    def word(self, value: int) -> None:
        if self._pending:
            self._here()
        self._w.append(value & 0xffffffff)

    def words(self, values) -> None:
        for value in values:
            self.word(value)

    def data(self, blob: bytes) -> None:
        if len(blob) % 4:
            raise AsmError("data must be a multiple of four bytes")
        if self._pending:
            self._here()
        self._w.frombytes(blob)

    def b(self, target: str) -> None:
        self._sp[self._here()] = (_B, target)
        self._w.append(0)

    def bcond(self, cond: int, target: str) -> None:
        self._sp[self._here()] = (_BC, cond, target)
        self._w.append(0)

    def cbz(self, rt: int, target: str, *, sf: int = 1, nz: bool = False
            ) -> None:
        self._sp[self._here()] = (_CBZ, rt, target, sf, nz)
        self._w.append(0)

    def tbz(self, rt: int, bit: int, target: str, *, nz: bool = False
            ) -> None:
        self._sp[self._here()] = (_TBZ, rt, bit, target, nz)
        self._w.append(0)

    def adrp_add(self, rd: int, target: str) -> None:
        self._sp[self._here()] = (_ADRP, rd, target)
        self._w.append(0)
        self._w.append(0)

    # layout
    def _address(self, index: int) -> int:
        return 4 * (index + bisect.bisect_left(self._ex, index))

    def _label_index(self, name: str) -> int:
        try:
            return self._labels[name]
        except KeyError:
            raise AsmError(f"undefined label {name}") from None

    def resolve(self) -> bytes:
        """Lay out with relaxation, then return the encoded bytes."""
        self._here()
        sp = self._sp
        labels = self._labels
        expanded: set[int] = set()
        while True:
            self._ex = sorted(expanded)
            ex = self._ex
            added = False
            for position, item in sp.items():
                kind = item[0]
                if kind == _B or kind == _ADRP or position in expanded:
                    continue
                target = item[3] if kind == _TBZ else item[2]
                index = self._label_index(target)
                delta = (index + bisect.bisect_left(ex, index) -
                         position - bisect.bisect_left(ex, position)) * 4
                reach = 1 << (15 if kind == _TBZ else 20)
                if not -reach <= delta < reach:
                    expanded.add(position)
                    added = True
            if not added:
                break
        self._ex = ex = sorted(expanded)
        out = array("I")
        words = self._w
        last = 0
        for position in sorted(sp):
            if position > last:
                out.extend(words[last:position])
            item = sp[position]
            kind = item[0]
            here = 4 * len(out)
            if kind == _B:
                delta = self._address(self._label_index(item[1])) - here
                out.append(b_word(delta // 4))
                last = position + 1
            elif kind == _ADRP:
                rd, target = item[1], item[2]
                address = self._address(self._label_index(target))
                out.append(adrp_word(rd, (address >> 12) - (here >> 12)))
                out.append(add_imm(rd, rd, address & 0xfff))
                last = position + 2
            else:
                long = position in expanded
                if kind == _BC:
                    cond, target = item[1], item[2]
                    delta = self._address(self._label_index(target)) - here
                    if not long:
                        out.append(bcond_word(cond, delta // 4))
                    elif cond >= 14:
                        # AL and NV (which AArch64 also takes as always)
                        # have no inverse: both are a plain branch; the
                        # second word keeps the size.
                        out.append(b_word(delta // 4))
                        out.append(nop())
                    else:
                        out.append(bcond_word(cond ^ 1, 2))
                        out.append(b_word((delta - 4) // 4))
                elif kind == _CBZ:
                    rt, target, sf, nz = item[1:]
                    delta = self._address(self._label_index(target)) - here
                    if not long:
                        out.append(cbz_word(rt, delta // 4, sf=sf, nz=nz))
                    else:
                        out.append(cbz_word(rt, 2, sf=sf, nz=not nz))
                        out.append(b_word((delta - 4) // 4))
                else:
                    rt, bit, target, nz = item[1:]
                    delta = self._address(self._label_index(target)) - here
                    if not long:
                        out.append(tbz_word(rt, bit, delta // 4, nz=nz))
                    else:
                        out.append(tbz_word(rt, bit, 2, nz=not nz))
                        out.append(b_word((delta - 4) // 4))
                last = position + 1
        out.extend(words[last:])
        self._resolved = True
        return out.tobytes()

    def label_address(self, name: str) -> int:
        return self._address(self._label_index(name))

    def end_address(self) -> int:
        return self._address(len(self._w))
