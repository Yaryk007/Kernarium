#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write a tiny arm64 shared object whose code is the AOT test fixture.

Real libraries cannot be relied on to contain particular control flow, so
the differential test (tests/unit/arm64/arm64_aot_differential_tests.cpp)
also rewrites this file. It holds a loop that runs through the interior of a
second unwind range: a function whose backward branch lands inside another
function. The file is generated into the build tree of a diagnostics build
and is never part of a product.

A second file imitates an OAT file of the boot image (build_oat): no unwind
table, its methods named by the function symbols of an XZ-compressed
.gnu_debugdata, methods that touch, and direct branches that reach below the
library's first byte, as a boot-framework method does into boot.oat.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import lzma
import pathlib
import struct
import sys

PAGE = 4096
STT_OBJECT, STT_FUNC = 1, 2
TEXT = 0x1000
TEXT_PAGES = 4
EH_FRAME = TEXT + TEXT_PAGES * PAGE


def b_cond(cond: int, here: int, target: int) -> int:
    return 0x54000000 | (((target - here) // 4) & 0x7ffff) << 5 | cond


def b(here: int, target: int) -> int:
    return 0x14000000 | (((target - here) // 4) & 0x3ffffff)


def bl(here: int, target: int) -> int:
    return 0x94000000 | (((target - here) // 4) & 0x3ffffff)


def cbz(rt: int, here: int, target: int) -> int:
    return 0xb4000000 | (((target - here) // 4) & 0x7ffff) << 5 | rt


def tbz(rt: int, bit: int, here: int, target: int) -> int:
    return 0x36000000 | bit << 19 | (((target - here) // 4) & 0x3fff) << 5 | rt


def adr(rd: int, here: int, target: int) -> int:
    delta = target - here
    return 0x10000000 | (delta & 3) << 29 | ((delta >> 2) & 0x7ffff) << 5 | rd


NOP = 0xd503201f
RET = 0xd65f03c0
BRK = 0xd4200000

# Function A (0x1000..0x1010) enters the loop; function B (0x1020..0x1030)
# closes it with a backward branch to 0x1004, inside A, after A jumped into
# B's interior. The loop passes through no function entry. The two ranges
# are not adjacent, so they stay two functions.
LOOP_A = TEXT
LOOP_B = TEXT + 0x20
CODE = {
    LOOP_A + 0x00: 0xd2800000,                      # movz x0, #0
    LOOP_A + 0x04: 0x91000400,                      # add x0, x0, #1
    LOOP_A + 0x08: b(LOOP_A + 0x08, LOOP_B + 4),    # b LOOP_B+4
    LOOP_A + 0x0c: RET,
    LOOP_B + 0x00: 0x91000421,                      # add x1, x1, #1
    LOOP_B + 0x04: 0xeb03001f,                      # cmp x0, x3
    LOOP_B + 0x08: b_cond(3, LOOP_B + 0x08, LOOP_A + 0x04),  # b.lo A+4
    LOOP_B + 0x0c: RET,
}
FUNCTIONS = ((LOOP_A, 0x10), (LOOP_B, 0x10))

# Page edges. The functions below are on one text page and leave it for the
# next by every kind of edge the rewritten code can take; the target there
# adds 100 to x0 and returns through x15. The differential test maps the
# next page with other bytes than the framework was built from, so an edge
# that runs the stored code of a page it was not compared with gives another
# x0 than the engine does.
SRC_PAGE = TEXT + 2 * PAGE
DST_PAGE = TEXT + 3 * PAGE
TARGET = DST_PAGE + 0x100
MOV_X15_X30 = 0xaa1e03ef
CMP_X1_X1 = 0xeb01003f
MOV_X2_0 = 0xd2800002
BR_X16, BLR_X16, RET_X15 = 0xd61f0200, 0xd63f0200, 0xd65f01e0
EDGES = {
    "fall": SRC_PAGE + 0xff8,
    "b": SRC_PAGE,
    "bl": SRC_PAGE + 0x20,
    "bcond": SRC_PAGE + 0x40,
    "cbz": SRC_PAGE + 0x60,
    "tbz": SRC_PAGE + 0x80,
    "br": SRC_PAGE + 0xa0,
    "blr": SRC_PAGE + 0xc0,
    "ret": SRC_PAGE + 0xe0,
    "reg": SRC_PAGE + 0x100,
}


def edge_code() -> dict[int, int]:
    code: dict[int, int] = {}

    def put(at: int, *words: int) -> None:
        for index, word in enumerate(words):
            code[at + 4 * index] = word

    e = EDGES
    # Falls into the next page: 1 + 2 on this one, 4 on the next.
    put(e["fall"], 0x91000400, 0x91000800, 0x91001000, RET)
    put(TARGET, 0x91019000, RET_X15)
    put(e["b"], MOV_X15_X30, b(e["b"] + 4, TARGET))
    put(e["bl"], MOV_X15_X30, bl(e["bl"] + 4, TARGET), RET)
    put(e["bcond"], MOV_X15_X30, CMP_X1_X1,
        b_cond(0, e["bcond"] + 8, TARGET), RET)
    put(e["cbz"], MOV_X15_X30, MOV_X2_0, cbz(2, e["cbz"] + 8, TARGET), RET)
    put(e["tbz"], MOV_X15_X30, MOV_X2_0, tbz(2, 0, e["tbz"] + 8, TARGET), RET)
    put(e["br"], MOV_X15_X30, adr(16, e["br"] + 4, TARGET), BR_X16)
    put(e["blr"], MOV_X15_X30, adr(16, e["blr"] + 4, TARGET), BLR_X16, RET)
    put(e["ret"], MOV_X15_X30, adr(30, e["ret"] + 4, TARGET), RET)
    # x16 comes from the case: the other instance of the library.
    put(e["reg"], MOV_X15_X30, BR_X16)
    return code


def edge_functions() -> tuple[tuple[int, int], ...]:
    sizes = {"fall": 0x10, "bl": 0x10, "bcond": 0x10, "cbz": 0x10, "tbz": 0x10,
             "blr": 0x10, "ret": 0x10, "b": 8, "br": 0x0c, "reg": 8}
    return tuple((EDGES[name], size) for name, size in sizes.items()
                 ) + ((TARGET, 8),)


# A PLT stub: it has no unwind entry, only a .plt section, as in every
# library the linker produced. The GOT slot it reads is 0x100 into the page.
PLT = TEXT + 0x40
PLT_CODE = (
    0x90000010,                                     # adrp x16, #0
    0xf9408211,                                     # ldr x17, [x16, #0x100]
    0x91040210,                                     # add x16, x16, #0x100
    0xd61f0220,                                     # br x17
)

# A routine only a function symbol names, as the assembly of a crypto library
# is: no unwind entry, a data word after its RET inside the symbol's size (a
# round constant of SHA-256), and the symbols around it that name no range (no
# size, a data object, an address that is no instruction's, an undefined one)
# or one the unwind tables already give.
SYMBOL_ONLY = DST_PAGE + 0xe00
SYMBOL_CODE = (
    0x91001c00,                                     # add x0, x0, #7
    0x18000061,                                     # ldr w1, [the data word]
    0x8b010000,                                     # add x0, x0, x1
    RET,
    0x428a2f98,                                     # the data word
)
SYMBOL_DATA = SYMBOL_ONLY + 4 * (len(SYMBOL_CODE) - 1)
SYMBOLS_NO_RANGE = (DST_PAGE + 0xb00, DST_PAGE + 0xb40, DST_PAGE + 0xb82)


# The same stub in an .iplt section, as the linker makes them for the ifunc
# targets of a static executable. It sits after every other function so the
# functions before it keep their places.
IPLT = DST_PAGE + 0x200


# ---- instruction forms ----------------------------------------------------
# One small function per form: the instructions of the form, then RET. The
# differential test starts cases at every function, so each form runs from
# random states against the engine, whatever the library it was written for
# contains. The words are built from the architecture's field layouts and
# checked against an assembler by the tooling test.

FORMS_AT = TEXT + 0x100
FORM_SIZE = 0x14           # a function of 0x10 bytes and a gap that keeps
                           # neighbouring ranges from merging
SIZES = ("b", "h", "w", "x")
ORDERINGS = ("", "a", "l", "al")      # none, acquire, release, both


def ordered(load: bool, size: int, rn: int, rt: int) -> int:
    return (size << 30) | (0x08dffc00 if load else 0x089ffc00) | \
        (rn << 5) | rt


def amo(size: int, acquire: int, release: int, rs: int, op: int, swap: bool,
        rn: int, rt: int) -> int:
    return (size << 30) | 0x38200000 | (acquire << 23) | (release << 22) | \
        (rs << 16) | (int(swap) << 15) | (op << 12) | (rn << 5) | rt


def cas(size: int, acquire: int, release: int, rs: int, rn: int, rt: int
        ) -> int:
    return (size << 30) | 0x08a07c00 | (acquire << 22) | (release << 15) | \
        (rs << 16) | (rn << 5) | rt


def mops(kind: str, stage: int, hint: int, rd: int, rn: int, rs: int) -> int:
    """kind: 'cpyf', 'cpy' or 'set'; hint is the option bits 15:12 (or, for
    SET, bits 13:12)."""
    if kind == "set":
        return 0x19c00400 | (rs << 16) | (stage << 14) | ((hint & 3) << 12) \
            | (rn << 5) | rd
    base = 0x19000400 if kind == "cpyf" else 0x1d000400
    return base | (stage << 22) | (rs << 16) | (hint << 12) | (rn << 5) | rd


def forms() -> list[tuple[str, list[int]]]:
    """(name, words) of every form; the words end before the RET."""
    out: list[tuple[str, list[int]]] = []
    # LDAR and STLR, by width, with real, virtual and zero registers and a
    # stack base (which steps).
    for size in range(4):
        for load in (True, False):
            tag = "ldar" if load else "stlr"
            out.append((f"{tag}{size}", [ordered(load, size, 1, 2)]))
            out.append((f"{tag}{size}_vt", [ordered(load, size, 1, 24)]))
            out.append((f"{tag}{size}_vb", [ordered(load, size, 25, 2)]))
            out.append((f"{tag}{size}_zt", [ordered(load, size, 1, 31)]))
            out.append((f"{tag}{size}_sp", [ordered(load, size, 31, 2)]))
            out.append((f"{tag}{size}_vv", [ordered(load, size, 18, 28)]))
    # The atomic memory operations: every operation, ordering and width.
    for op in range(8):
        for size in range(4):
            for order, (a, r) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
                out.append((f"amo{op}_{size}_{order}",
                            [amo(size, a, r, 3, op, False, 1, 2)]))
    for size in range(4):
        for order, (a, r) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
            out.append((f"swp_{size}_{order}",
                        [amo(size, a, r, 3, 0, True, 1, 2)]))
            out.append((f"cas_{size}_{order}",
                        [cas(size, a, r, 3, 1, 2)]))
    # Register variants of one operation of each kind: virtual operands,
    # equal operands, the zero register, a stack or virtual base.
    for rs, rn, rt, name in ((24, 1, 2, "vs"), (3, 1, 28, "vt"),
                             (24, 1, 28, "vsvt"), (25, 1, 25, "same"),
                             (31, 1, 2, "zs"), (3, 1, 31, "zt"),
                             (3, 18, 2, "vb"), (3, 31, 2, "sp"),
                             (2, 1, 2, "rsrt"), (2, 2, 3, "rnrs"),
                             (3, 3, 3, "all")):
        out.append((f"amo_add_{name}", [amo(3, 1, 1, rs, 0, False, rn, rt)]))
        out.append((f"swp_{name}", [amo(2, 0, 1, rs, 0, True, rn, rt)]))
        out.append((f"cas_{name}", [cas(3, 1, 1, rs, rn, rt)]))
        out.append((f"casb_{name}", [cas(0, 1, 0, rs, rn, rt)]))
    # Other words of the same groups: LDAPR (steps), the unallocated
    # neighbours of the atomic and ordered groups.
    out.append(("ldapr", [0xf8bfc022]))
    out.append(("ldlar", [0xc8df7c22]))
    out.append(("stllr", [0xc89f7c22]))
    out.append(("ldapr_b", [0x38bfc022]))
    # FEAT_MOPS: the three stages in a row, from the same registers, with
    # every option set, then variants with virtual registers (one is
    # inlined, two step) and the zero fill register.
    for kind in ("cpyf", "cpy", "set"):
        hints = (0, 1, 2, 3) if kind == "set" else (0, 4, 8, 12, 1, 2, 5, 10)
        for hint in hints:
            out.append((f"{kind}_{hint}", [mops(kind, 0, hint, 0, 2, 1),
                                           mops(kind, 1, hint, 0, 2, 1),
                                           mops(kind, 2, hint, 0, 2, 1)]))
    for name, d, n, s in (("vn", 0, 24, 1), ("vd", 25, 2, 1),
                          ("vs", 0, 2, 26), ("vdvn", 24, 25, 1),
                          ("vnvs", 0, 18, 28), ("same", 0, 0, 1),
                          ("zs", 0, 2, 31), ("xzr_d", 31, 2, 1),
                          ("high", 17, 16, 15)):
        for kind in ("cpyf", "cpy", "set"):
            out.append((f"{kind}_{name}",
                        [mops(kind, 0, 0, d, n, s), mops(kind, 1, 0, d, n, s),
                         mops(kind, 2, 0, d, n, s)]))
    # A prologue repeated without its epilogue, and a main stage alone.
    for kind in ("cpyf", "cpy", "set"):
        out.append((f"{kind}_pp", [mops(kind, 0, 0, 0, 2, 1),
                                   mops(kind, 0, 0, 0, 2, 1)]))
        out.append((f"{kind}_m", [mops(kind, 1, 0, 0, 2, 1)]))
        out.append((f"{kind}_e", [mops(kind, 2, 0, 0, 2, 1)]))
    # An ISB, which the engine runs, then a MOV: the code-change scenario
    # rewrites the MOV while the ISB's function is being entered.
    out.append(("isb_mov", [0xd5033fdf, 0xd2800020]))
    return out


def form_code() -> dict[int, int]:
    code = {}
    for index, (_, words) in enumerate(forms()):
        base = FORMS_AT + index * FORM_SIZE
        if len(words) + 1 > (FORM_SIZE - 4) // 4:
            raise ValueError("a form is too long")
        for position, word in enumerate(words):
            code[base + 4 * position] = word
        code[base + 4 * len(words)] = RET
    return code


def form_functions() -> tuple[tuple[int, int], ...]:
    return tuple((FORMS_AT + index * FORM_SIZE, FORM_SIZE - 4)
                 for index in range(len(forms())))


# ---- shared-memory loops --------------------------------------------------
# Three loops that add one to the 64-bit cell at [x2], x0 times, each by a
# different instruction family: LDADD (inlined as a native atomic), a CAS
# retry loop (inlined) and LDXR/STXR (left to the engine, which opens and
# closes the exclusive monitor between entries of rewritten code). The
# differential test runs one in rewritten code and another in the engine
# on the same cell, so a lost update shows as a final count that is short.
HAMMER_AT = TEXT + 0x1a00
HAMMER_ADD = HAMMER_AT
HAMMER_CAS = HAMMER_AT + 0x40
HAMMER_LLSC = HAMMER_AT + 0x80
SUBS_X0_X0_1 = 0xf1000400
MOV_X7_X5 = 0xaa0503e7
CMP_X7_X5 = 0xeb0500ff


def cbnz_w(rt: int, here: int, target: int) -> int:
    return 0x35000000 | (((target - here) // 4) & 0x7ffff) << 5 | rt


def hammer_code() -> dict[int, int]:
    code: dict[int, int] = {}

    def put(at: int, *words: int) -> None:
        for index, word in enumerate(words):
            code[at + 4 * index] = word

    a = HAMMER_ADD
    put(a, 0xd2800023,                              # movz x3, #1
        amo(3, 0, 0, 3, 0, False, 2, 4),            # ldadd x3, x4, [x2]
        SUBS_X0_X0_1, b_cond(1, a + 12, a + 4), RET)
    c = HAMMER_CAS
    put(c, 0xf9400045,                              # ldr x5, [x2]
        0x910004a6,                                 # add x6, x5, #1
        MOV_X7_X5,
        cas(3, 0, 0, 7, 2, 6),                      # cas x7, x6, [x2]
        CMP_X7_X5, b_cond(1, c + 20, c),            # retry on a lost race
        SUBS_X0_X0_1, b_cond(1, c + 28, c), RET)
    d = HAMMER_LLSC
    put(d, 0xc85f7c45,                              # ldxr x5, [x2]
        0x910004a5,                                 # add x5, x5, #1
        0xc8067c45,                                 # stxr w6, x5, [x2]
        cbnz_w(6, d + 12, d),
        SUBS_X0_X0_1, b_cond(1, d + 20, d), RET)
    return code


def hammer_functions() -> tuple[tuple[int, int], ...]:
    return ((HAMMER_ADD, 0x14), (HAMMER_CAS, 0x24), (HAMMER_LLSC, 0x1c))


# ---- the memory copy and set stages ---------------------------------------
# Three-stage sequences with fixed registers, which the differential test
# starts from chosen states (x0 destination, x1 source or fill value, x2
# count; the second set of rows names x17, x15 and x16). The words are the
# prologue, main and epilogue stages and a RET.
MOPS_AT = TEXT + 0x1b00
MOPS_STEP = 0x20
MOPS_ROWS = (("cpyf", 0, 2, 1), ("cpy", 0, 2, 1), ("set", 0, 2, 1),
             ("cpyf", 17, 16, 15), ("cpy", 17, 16, 15), ("set", 17, 16, 15),
             ("set", 0, 2, 31))


def mops_code() -> dict[int, int]:
    code: dict[int, int] = {}
    for index, (kind, d, n, s) in enumerate(MOPS_ROWS):
        base = MOPS_AT + index * MOPS_STEP
        for stage in range(3):
            code[base + 4 * stage] = mops(kind, stage, 0, d, n, s)
        code[base + 12] = RET
    return code


def mops_functions() -> tuple[tuple[int, int], ...]:
    return tuple((MOPS_AT + index * MOPS_STEP, 0x10)
                 for index in range(len(MOPS_ROWS)))


# ---- Advanced SIMD structure loads and stores -----------------------------
# One instruction per row (and a RET), the differential test starts cases at
# every row: multiple and single structures, replicating loads, post-index
# by immediate and by register, virtual base and index registers, an SP base
# (which steps) and the reserved words.
SIMD_AT = TEXT + 0x1c00
SIMD_STEP = 8
# After the one-instruction rows: a byte load from x2, the structure access
# from x1 and a RET. The differential starts them with x2 in a page whose TLB
# entry the load fills, and x1 in the neighbouring page.
SIMD_WARM_AT = SIMD_AT + 0x300
SIMD_WARM_STEP = 0x10
LDRB_W9_X2 = 0x39400049


def simd_multi(load: bool, q: int, opcode: int, size: int, rn: int, rt: int,
               post: int | None = None) -> int:
    """post: None for no offset, 31 for the immediate, else the register."""
    word = 0x0c000000 | (q << 30) | (int(load) << 22) | (opcode << 12) | \
        (size << 10) | (rn << 5) | rt
    if post is not None:
        word |= (1 << 23) | (post << 16)
    return word


def simd_single(load: bool, q: int, r: int, opcode: int, s: int, size: int,
                rn: int, rt: int, post: int | None = None) -> int:
    word = 0x0d000000 | (q << 30) | (int(load) << 22) | (r << 21) | \
        (opcode << 13) | (s << 12) | (size << 10) | (rn << 5) | rt
    if post is not None:
        word |= (1 << 23) | (post << 16)
    return word


def simd_rows() -> list[tuple[str, int]]:
    rows: list[tuple[str, int]] = []
    # Multiple structures: LD1 of one to four registers, LD2 to LD4, by
    # arrangement.
    for name, load, q, opcode, size, post in (
            ("ld1_16b", 1, 1, 7, 0, None), ("ld1_8b_i", 1, 0, 7, 0, 31),
            ("ld1_2x16b_i", 1, 1, 10, 1, 31), ("ld1_3x4h_i", 1, 0, 6, 1, 31),
            ("ld1_3x8h_r", 1, 1, 6, 1, 2), ("ld1_4x16b_i", 1, 1, 2, 0, 31),
            ("ld1_4x8b", 1, 0, 2, 0, None), ("ld1_1d_i", 1, 0, 7, 3, 31),
            ("ld2_4s_i", 1, 1, 8, 2, 31), ("ld2_8b", 1, 0, 8, 0, None),
            ("ld3_8h_i", 1, 1, 4, 1, 31), ("ld3_2s_r", 1, 0, 4, 2, 2),
            ("ld4_16b_r", 1, 1, 0, 0, 2), ("ld4_2d_i", 1, 1, 0, 3, 31),
            ("ld4_1d_reserved", 1, 0, 0, 3, 31),
            ("st1_16b", 0, 1, 7, 0, None), ("st1_2x4s_i", 0, 1, 10, 2, 31),
            ("st1_4x16b_i", 0, 1, 2, 0, 31), ("st1_3x8b_r", 0, 0, 6, 0, 2),
            ("st2_4s_i", 0, 1, 8, 2, 31), ("st3_8h", 0, 1, 4, 1, None),
            ("st4_4h_i", 0, 0, 0, 1, 31), ("st4_2d_r", 0, 1, 0, 3, 2)):
        rows.append((name, simd_multi(bool(load), q, opcode, size, 1, 0,
                                      post)))
    # Virtual base and index registers, an index equal to the base, the
    # stack pointer.
    rows += [
        ("ld1_vb", simd_multi(True, 1, 7, 0, 25, 0)),
        ("ld1_vb_i", simd_multi(True, 1, 10, 0, 25, 0, 31)),
        ("ld1_vi_r", simd_multi(True, 1, 7, 0, 1, 0, 24)),
        ("ld1_vbvi_r", simd_multi(True, 1, 7, 0, 25, 0, 24)),
        ("ld1_vb_same_r", simd_multi(True, 1, 7, 0, 25, 0, 25)),
        ("ld1_same_r", simd_multi(True, 1, 7, 0, 1, 0, 1)),
        ("st1_vb_i", simd_multi(False, 1, 7, 0, 18, 0, 31)),
        ("ld1_sp", simd_multi(True, 1, 7, 0, 31, 0)),
        ("ld1_sp_i", simd_multi(True, 1, 7, 0, 31, 0, 31)),
    ]
    # Single structures: one lane of each element size, then LD2 to LD4 and
    # ST1 to ST4 lanes.
    for name, load, q, r, opcode, s, size, post in (
            ("ld1_b0", 1, 0, 0, 0, 0, 0, None),
            ("ld1_b15_i", 1, 1, 0, 0, 1, 3, 31),
            ("ld1_h3_i", 1, 0, 0, 2, 1, 2, 31),
            ("ld1_s1_i", 1, 0, 0, 4, 1, 0, 31),
            ("ld1_d1", 1, 1, 0, 4, 0, 1, None),
            ("ld2_s", 1, 0, 1, 4, 0, 0, None),
            ("ld2_h_r", 1, 1, 1, 2, 1, 0, 2),
            ("ld3_h_i", 1, 0, 0, 3, 0, 2, 31),
            ("ld3_d_i", 1, 1, 0, 5, 0, 1, 31),
            ("ld4_b_r", 1, 0, 1, 1, 0, 0, 2),
            ("ld4_d_i", 1, 0, 1, 5, 0, 1, 31),
            ("st1_b_i", 0, 1, 0, 0, 1, 1, 31),
            ("st1_s", 0, 0, 0, 4, 1, 0, None),
            ("st2_h_i", 0, 0, 1, 2, 0, 0, 31),
            ("st3_d", 0, 1, 0, 5, 0, 1, None),
            ("st4_s_r", 0, 1, 1, 5, 0, 0, 2),
            ("ld1_reserved_h", 1, 0, 0, 2, 0, 1, None),
            ("ld1_reserved_d", 1, 0, 0, 4, 1, 1, None)):
        rows.append((name, simd_single(bool(load), q, r, opcode, s, size, 1,
                                       0, post)))
    # Replicating loads, by arrangement and count.
    for name, q, r, opcode, size, post in (
            ("ld1r_8b", 0, 0, 6, 0, None), ("ld1r_16b_i", 1, 0, 6, 0, 31),
            ("ld1r_4h_i", 0, 0, 6, 1, 31), ("ld1r_4s_i", 1, 0, 6, 2, 31),
            ("ld1r_1d", 0, 0, 6, 3, None), ("ld1r_2d_r", 1, 0, 6, 3, 2),
            ("ld2r_2s_i", 0, 1, 6, 2, 31), ("ld3r_4s_i", 1, 0, 7, 2, 31),
            ("ld4r_2d_i", 1, 1, 7, 3, 31), ("ld4r_8b", 0, 1, 7, 0, None)):
        rows.append((name, simd_single(True, q, r, opcode, 0, size, 1, 0,
                                       post)))
    rows += [
        ("ld1r_vb_vi", simd_single(True, 1, 0, 6, 0, 2, 25, 0, 24)),
        ("ld1_s_vb_i", simd_single(True, 0, 0, 4, 1, 0, 25, 0, 31)),
        ("ld1r_sp", simd_single(True, 1, 0, 6, 0, 2, 31, 0)),
        ("st1r_reserved", simd_single(False, 1, 0, 6, 0, 2, 1, 0)),
    ]
    # A register list that wraps from v31 to v0.
    rows += [
        ("ld4_wrap_16b", simd_multi(True, 1, 0, 0, 1, 30)),
        ("st4_wrap_16b", simd_multi(False, 1, 0, 0, 1, 30)),
        ("ld3_wrap_i", simd_multi(True, 1, 4, 1, 1, 31, 31)),
        ("ld4_wrap_b", simd_single(True, 0, 1, 1, 0, 0, 1, 30)),
        ("ld4r_wrap", simd_single(True, 1, 1, 7, 0, 3, 1, 30)),
    ]
    if SIMD_AT + SIMD_STEP * len(rows) > SIMD_WARM_AT:
        raise ValueError("the SIMD rows run into the warm rows")
    return rows


def simd_warm_rows() -> list[tuple[str, int]]:
    return [("ld4_16b", simd_multi(True, 1, 0, 0, 1, 0)),
            ("st4_16b", simd_multi(False, 1, 0, 0, 1, 0)),
            ("ld1_16b", simd_multi(True, 1, 7, 0, 1, 0)),
            ("st1_4x16b", simd_multi(False, 1, 2, 0, 1, 0))]


def simd_code() -> dict[int, int]:
    code: dict[int, int] = {}
    for index, (_, word) in enumerate(simd_rows()):
        base = SIMD_AT + index * SIMD_STEP
        code[base] = word
        code[base + 4] = RET
    for index, (_, word) in enumerate(simd_warm_rows()):
        base = SIMD_WARM_AT + index * SIMD_WARM_STEP
        code[base] = LDRB_W9_X2
        code[base + 4] = word
        code[base + 8] = RET
    return code


def simd_functions() -> tuple[tuple[int, int], ...]:
    return tuple((SIMD_AT + index * SIMD_STEP, 8)
                 for index in range(len(simd_rows()))) + tuple(
        (SIMD_WARM_AT + index * SIMD_WARM_STEP, 12)
        for index in range(len(simd_warm_rows())))


# ---- load and store pairs -------------------------------------------------
# LDP and STP in every addressing mode, with virtual (x18, x24 to x28), plain
# and zero registers as the base and as the data, the stack pointer as the
# base, and the vector forms. They sit in the page the edges' target is in,
# after it, one small function each, so the differential test starts cases
# at them like at the other forms.

PAIRS_AT = DST_PAGE + 0x300
PAIR_SIZE = 0x14


def pair(load: bool, mode: str, opc: int, vector: int, imm: int, rt: int,
         rt2: int, rn: int) -> int:
    """LDP/STP of Rt, Rt2 from [Rn] with a signed byte offset `imm`: mode is
    "post", "off" (signed offset) or "pre"; opc 0, 1, 2 select 32-bit, ldpsw
    (not used) and 64-bit, or 4, 8 and 16 bytes of a vector register."""
    scale = ({0: 4, 1: 8, 2: 16} if vector else {0: 4, 2: 8})[opc]
    index = {"post": 0b001, "off": 0b010, "pre": 0b011}[mode]
    return (opc << 30) | (0b101 << 27) | (vector << 26) | (index << 23) | \
        (int(load) << 22) | (((imm // scale) & 0x7f) << 15) | (rt2 << 10) | \
        (rn << 5) | rt


def pair_forms() -> list[tuple[str, list[int]]]:
    out: list[tuple[str, list[int]]] = []
    # The base: virtual (x25, x18), the stack pointer, plain. The data
    # registers: virtual and virtual (the hot loop of toybox grep is
    # `ldp x24, x28, [x25, #0x38]!`), virtual and plain, plain and virtual,
    # two virtual ones that are not the same.
    data = (("vv", 24, 28), ("vp", 24, 2), ("pv", 3, 28), ("v2", 18, 26))
    for base_name, base, imm in (("vb", 25, 0x38), ("sp", 31, 0x30),
                                 ("pb", 1, 0x38)):
        for mode in ("pre", "post", "off"):
            if mode == "off" and base != 25:
                continue
            for load in (True, False):
                for name, rt, rt2 in data:
                    if base == 1 and name not in ("vv",):
                        continue
                    out.append((f"{'ldp' if load else 'stp'}_{mode}_"
                                f"{base_name}_{name}",
                                [pair(load, mode, 2, 0, imm, rt, rt2, base)]))
    # A stack pointer that the writeback leaves misaligned (an exit when it
    # does), a negative offset, the 32-bit forms.
    out.append(("ldp_pre_sp_vv_odd", [pair(True, "pre", 2, 0, 0x38, 24, 28,
                                           31)]))
    out.append(("stp_pre_sp_vv_odd", [pair(False, "pre", 2, 0, 0x38, 24, 28,
                                           31)]))
    out.append(("ldp_post_sp_vv_neg", [pair(True, "post", 2, 0, -0x30, 24, 28,
                                            31)]))
    out.append(("stp_pre_sp_vv_neg", [pair(False, "pre", 2, 0, -0x30, 24, 28,
                                           31)]))
    for load in (True, False):
        tag = "ldp" if load else "stp"
        out.append((f"{tag}_pre_vb_vv_w", [pair(load, "pre", 0, 0, 0x38, 24,
                                                28, 25)]))
        out.append((f"{tag}_post_vb_vp_w", [pair(load, "post", 0, 0, 0x38, 24,
                                                 2, 25)]))
    # Both data registers one virtual register (a store), zero registers.
    for mode in ("pre", "post", "off"):
        out.append((f"stp_{mode}_vb_same", [pair(False, mode, 2, 0, 0x38, 24,
                                                 24, 25)]))
        out.append((f"stp_{mode}_vb_zz", [pair(False, mode, 2, 0, 0x38, 31,
                                               31, 25)]))
        out.append((f"stp_{mode}_sp_same", [pair(False, mode, 2, 0, 0x30, 28,
                                                 28, 31)]))
    # The same load register twice and a data register equal to the base:
    # the engine's own cases.
    out.append(("ldp_off_vb_same", [pair(True, "off", 2, 0, 0x38, 24, 24,
                                         25)]))
    out.append(("ldp_pre_vb_base", [pair(True, "pre", 2, 0, 0x38, 25, 28,
                                         25)]))
    out.append(("stp_post_vb_base", [pair(False, "post", 2, 0, 0x38, 24, 25,
                                          25)]))
    # The stack pointer as the base with plain data registers: the offsets
    # inside one aligned 16-byte granule (the stack-granule probe's short
    # form) and those that leave it, 64-bit and 32-bit, a post-index pair, and
    # a store of two zero registers.
    for load in (True, False):
        tag = "ldp" if load else "stp"
        for mode, imm, opc in (("off", 0x10, 2), ("off", 0x18, 2),
                               ("pre", 0x10, 2), ("pre", 0x18, 2),
                               ("post", 0x20, 2), ("off", 0x18, 0),
                               ("off", 0x1c, 0), ("pre", 0x1c, 0)):
            out.append((f"{tag}_{mode}_sp_pp_{'x' if opc else 'w'}{imm:x}",
                        [pair(load, mode, opc, 0, imm, 1, 2, 31)]))
    out.append(("stp_off_sp_zz", [pair(False, "off", 2, 0, 0x10, 31, 31, 31)]))
    # The vector pairs with a virtual base or the stack pointer.
    for opc in (0, 1, 2):
        for load in (True, False):
            for mode, base in (("pre", 25), ("post", 31), ("off", 25)):
                out.append((f"{'ldp' if load else 'stp'}_{mode}_"
                            f"{'vb' if base == 25 else 'sp'}_q{opc}",
                            [pair(load, mode, opc, 1, 0x30, 0, 1, base)]))
    return out


def pair_code() -> dict[int, int]:
    code = {}
    for index, (_, words) in enumerate(pair_forms()):
        base = PAIRS_AT + index * PAIR_SIZE
        if len(words) + 1 > (PAIR_SIZE - 4) // 4:
            raise ValueError("a pair form is too long")
        for position, word in enumerate(words):
            code[base + 4 * position] = word
        code[base + 4 * len(words)] = RET
    return code


def pair_functions() -> tuple[tuple[int, int], ...]:
    return tuple((PAIRS_AT + index * PAIR_SIZE, PAIR_SIZE - 4)
                 for index in range(len(pair_forms())))


# ---- reserved forms -------------------------------------------------------
# One reserved encoding of each class the rewriter copies or translates field
# by field and whose reserved forms its instruction tables do not tell from
# allocated ones, and a RET: the engine raises an illegal instruction at each
# word, so the rewriter leaves it to the engine. The differential test starts
# cases at every row and asks the engine about every word.
RESERVED_AT = DST_PAGE + 0xc00
RESERVED_STEP = 8


def reserved_rows() -> list[tuple[str, int]]:
    return [
        ("add_vector_2d_q0", 0x0ee08400),
        ("asimdall", 0x0eb03800),
        ("asimddiff", 0x0e209000),
        ("asimdelem", 0x0f002000),
        ("asimdext", 0x2e004000),
        ("asimdins", 0x0e000400),
        ("asimdmisc", 0x0e601800),
        ("asimdperm", 0x0ec01800),
        ("asimdsame", 0x0e20b400),
        ("asimdshf", 0x0f08e400),
        ("asisddiff", 0x5e209000),
        ("asisdelem", 0x5f003000),
        ("asisdmisc", 0x5ee14800),
        ("asisdone", 0x5e000400),
        ("asisdsame", 0x5e20b400),
        ("asisdshf", 0x5f08e400),
        ("dp_3src", 0x9b400000),
        ("float2fix", 0x1e020000),
        ("floatcmp_zero_rm", 0x1e212018),
        # A load of a pair into one register: LDP S0, S0, [x0]; its
        # non-temporal, pre-index and post-index forms; and the general ones,
        # one naming the zero register twice.
        ("ldp_vector_same", 0x2d400000),
        ("ldnp_vector_same", 0x2c400000),
        ("ldp_post_vector_same", 0x2cc00000),
        ("ldp_pre_vector_same", 0x2dc00000),
        ("ldp_zero_zero", 0x29407fff),
        ("ldp_x1_x1", 0xa9400441),
    ]


def reserved_code() -> dict[int, int]:
    code: dict[int, int] = {}
    for index, (_, word) in enumerate(reserved_rows()):
        base = RESERVED_AT + index * RESERVED_STEP
        code[base] = word
        code[base + 4] = RET
    return code


def reserved_functions() -> tuple[tuple[int, int], ...]:
    return tuple((RESERVED_AT + index * RESERVED_STEP, RESERVED_STEP)
                 for index in range(len(reserved_rows())))


def eh_frame() -> bytes:
    # CIE: version 1, "zR", code alignment 4, data alignment -8, return
    # address x30, pointers are pc-relative signed 32-bit.
    cie_body = (struct.pack("<IBBB", 0, 1, ord("z"), ord("R")) + b"\0" +
                bytes([4, 0x78, 30, 1, 0x1b, 0x0c, 31, 0]))
    cie_body += bytes(-(len(cie_body) + 4) % 8)
    out = bytearray(struct.pack("<I", len(cie_body)) + cie_body)
    for start, size in (FUNCTIONS + form_functions() + edge_functions() +
                        hammer_functions() + mops_functions() +
                        simd_functions() + pair_functions() +
                        reserved_functions()):
        position = len(out)
        field = EH_FRAME + position + 8
        body = struct.pack("<Iii", position + 4, start - field, size)
        body += b"\0"
        body += bytes(-(len(body) + 4) % 8)
        out += struct.pack("<I", len(body)) + body
    out += struct.pack("<I", 0)
    return bytes(out)


def build() -> bytes:
    text = bytearray(PAGE * TEXT_PAGES)
    for offset in range(0, len(text), 4):
        struct.pack_into("<I", text, offset, BRK)
    for address, word in CODE.items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in form_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in edge_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    if FORMS_AT + len(forms()) * FORM_SIZE > HAMMER_AT:
        raise ValueError("the forms run into the shared-memory loops")
    for address, word in hammer_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in mops_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for address, word in simd_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    if PAIRS_AT + len(pair_forms()) * PAIR_SIZE > DST_PAGE + PAGE:
        raise ValueError("the pair forms run out of their page")
    for address, word in pair_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    if PAIRS_AT + len(pair_forms()) * PAIR_SIZE > SYMBOL_ONLY:
        raise ValueError("the pair forms run into the symbol-only routine")
    if RESERVED_AT + len(reserved_rows()) * RESERVED_STEP > SYMBOL_ONLY:
        raise ValueError("the reserved forms run into the symbol-only routine")
    if PAIRS_AT + len(pair_forms()) * PAIR_SIZE > RESERVED_AT:
        raise ValueError("the pair forms run into the reserved forms")
    for address, word in reserved_code().items():
        struct.pack_into("<I", text, address - TEXT, word)
    for index, word in enumerate(PLT_CODE):
        struct.pack_into("<I", text, PLT - TEXT + 4 * index, word)
        struct.pack_into("<I", text, IPLT - TEXT + 4 * index, word)
    for index, word in enumerate(SYMBOL_CODE):
        struct.pack_into("<I", text, SYMBOL_ONLY - TEXT + 4 * index, word)
    frame = eh_frame()
    names = (b"\0.text\0.plt\0.iplt\0.eh_frame\0.shstrtab\0.symtab\0"
             b".strtab\0")
    size_load = EH_FRAME + len(frame)
    image = bytearray(size_load)
    image[TEXT:TEXT + len(text)] = text
    image[EH_FRAME:EH_FRAME + len(frame)] = frame
    shstr_offset = len(image)
    image += names
    image += bytes(-len(image) % 8)
    symbols = [struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)]
    symbol_names = b"\0"
    for name, kind, shndx, value, size in (
            ("symbol_only", STT_FUNC, 1, SYMBOL_ONLY, 4 * len(SYMBOL_CODE)),
            ("inside_loop_a", STT_FUNC, 1, LOOP_A, 0x10),
            ("no_size", STT_FUNC, 1, SYMBOLS_NO_RANGE[0], 0),
            ("object", STT_OBJECT, 1, SYMBOLS_NO_RANGE[1], 0x20),
            ("misaligned", STT_FUNC, 1, SYMBOLS_NO_RANGE[2], 0x10),
            ("undefined", STT_FUNC, 0, 0, 0x10)):
        symbols.append(struct.pack("<IBBHQQ", len(symbol_names), kind, 0,
                                   shndx, value, size))
        symbol_names += name.encode() + b"\0"
    symtab_offset = len(image)
    image += b"".join(symbols)
    strtab_offset = len(image)
    image += symbol_names
    image += bytes(-len(image) % 8)
    section_offset = len(image)

    def section(name, kind, flags, addr, offset, size, align):
        return struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset,
                           size, 0, 0, align, 0)

    image += section(0, 0, 0, 0, 0, 0, 0)
    image += section(1, 1, 6, TEXT, TEXT, len(text), 4)
    image += section(7, 1, 6, PLT, PLT, 4 * len(PLT_CODE), 16)
    image += section(12, 1, 6, IPLT, IPLT, 4 * len(PLT_CODE), 16)
    image += section(18, 1, 2, EH_FRAME, EH_FRAME, len(frame), 8)
    image += section(28, 3, 0, 0, shstr_offset, len(names), 1)
    image += section(38, 2, 0, 0, symtab_offset, 24 * len(symbols), 8)
    image += section(46, 3, 0, 0, strtab_offset, len(symbol_names), 1)
    header = (b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) +
              struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, section_offset,
                          0, 64, 56, 1, 64, 8, 5))
    program = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, size_load, size_load,
                          PAGE)
    image[0:len(header)] = header
    image[64:64 + len(program)] = program
    return bytes(image)


# ---- an OAT file of the boot image ----------------------------------------
# The text starts at OAT_TEXT after a read-only first segment, as in a boot
# image's file. Its methods are packed with no gap: M0 and M1 touch. The
# branch methods below reach OAT_CALLEE of another instance of this file
# mapped OAT_LOWER_BY bytes lower (the differential test maps one there), so
# the target is below the first byte of the instance they run in.
OAT_TEXT = 0x1000
OAT_TEXT_PAGES = 4
OAT_LOWER_BY = 0x10000
OAT_CALLEE = OAT_TEXT + 0x1200
OAT_BELOW = OAT_CALLEE - OAT_LOWER_BY          # negative: -0xde00
OAT_PLAIN = OAT_TEXT
OAT_BRANCHES = {"b": OAT_TEXT + 0x100, "bl": OAT_TEXT + 0x120,
                "bcond": OAT_TEXT + 0x140, "cbz": OAT_TEXT + 0x160,
                "tbz": OAT_TEXT + 0x180}


def oat_code() -> dict[int, int]:
    code: dict[int, int] = {}

    def put(at: int, *words: int) -> None:
        for index, word in enumerate(words):
            code[at + 4 * index] = word

    put(OAT_PLAIN, 0xd2800000, 0x91000400, 0x91000800, RET)    # x0 = 3
    put(OAT_PLAIN + 0x10, 0x91001000, 0x91002000, 0x91004000, RET)
    t = OAT_BELOW
    e = OAT_BRANCHES
    put(e["b"], MOV_X15_X30, b(e["b"] + 4, t))
    put(e["bl"], MOV_X15_X30, bl(e["bl"] + 4, t), RET_X15)
    put(e["bcond"], MOV_X15_X30, CMP_X1_X1, b_cond(0, e["bcond"] + 8, t),
        RET)
    put(e["cbz"], MOV_X15_X30, MOV_X2_0, cbz(2, e["cbz"] + 8, t), RET)
    put(e["tbz"], MOV_X15_X30, MOV_X2_0, tbz(2, 0, e["tbz"] + 8, t), RET)
    put(OAT_CALLEE, 0x91019000, RET)                           # x0 += 100
    return code


def oat_functions() -> tuple[tuple[int, int], ...]:
    """(start, size) of each method, in address order."""
    sizes = {OAT_PLAIN: 0x10, OAT_PLAIN + 0x10: 0x10, OAT_CALLEE: 8}
    for name, at in OAT_BRANCHES.items():
        sizes[at] = {"b": 8, "bl": 12}.get(name, 16)
    return tuple(sorted(sizes.items()))


def debug_data() -> bytes:
    """The XZ-compressed ELF a stripped file carries as .gnu_debugdata: a
    symbol table whose function symbols give each method's start and size.
    A zero-sized function symbol and an object symbol are in it too, and
    name no method."""
    names = b"\0"
    symbols = [struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)]
    entries = [(f"method{index}", 2, at, size)
               for index, (at, size) in enumerate(oat_functions())]
    entries.append(("empty", 2, OAT_TEXT + 0x300, 0))
    entries.append(("table", 1, OAT_TEXT + 0x400, 0x20))
    for name, kind, value, size in entries:
        symbols.append(struct.pack("<IBBHQQ", len(names), kind, 0, 1, value,
                                   size))
        names += name.encode() + b"\0"
    section_names = b"\0.symtab\0.strtab\0.shstrtab\0"
    image = bytearray(64)
    symtab = len(image)
    image += b"".join(symbols)
    strtab = len(image)
    image += names
    shstrtab = len(image)
    image += section_names
    image += bytes(-len(image) % 8)
    section_offset = len(image)

    def section(name, kind, offset, size, entsize):
        return struct.pack("<IIQQQQIIQQ", name, kind, 0, 0, offset, size, 0,
                           0, 8, entsize)

    image += section(0, 0, 0, 0, 0)
    image += section(1, 2, symtab, 24 * len(symbols), 24)
    image += section(9, 3, strtab, len(names), 0)
    image += section(17, 3, shstrtab, len(section_names), 0)
    image[0:64] = (b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) +
                   struct.pack("<HHIQQQIHHHHHH", 1, 183, 1, 0, 0,
                               section_offset, 0, 64, 0, 0, 64, 4, 3))
    return lzma.compress(bytes(image))


def build_oat(code: bool = True) -> bytes:
    """The OAT-like file; with `code` False only its header segment, as the
    OAT file of a jar with no compiled method is."""
    size_text = OAT_TEXT_PAGES * PAGE
    image = bytearray(PAGE + (size_text if code else 0))
    if code:
        for offset in range(0, size_text, 4):
            struct.pack_into("<I", image, OAT_TEXT + offset, BRK)
        for address, word in oat_code().items():
            struct.pack_into("<I", image, address, word)
    names = b"\0.text\0.gnu_debugdata\0.shstrtab\0"
    debug = debug_data()
    debug_offset = len(image)
    image += debug
    shstr_offset = len(image)
    image += names
    image += bytes(-len(image) % 8)
    section_offset = len(image)

    def section(name, kind, flags, addr, offset, size):
        return struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset,
                           size, 0, 0, 1, 0)

    image += section(0, 0, 0, 0, 0, 0)
    sections = 3
    if code:
        image += section(1, 1, 6, OAT_TEXT, OAT_TEXT, size_text)
        sections += 1
    image += section(7, 1, 0, 0, debug_offset, len(debug))
    image += section(22, 3, 0, 0, shstr_offset, len(names))
    segments = [struct.pack("<IIQQQQQQ", 1, 4, 0, 0, 0, PAGE, PAGE, PAGE)]
    if code:
        segments.append(struct.pack("<IIQQQQQQ", 1, 5, OAT_TEXT, OAT_TEXT,
                                    OAT_TEXT, size_text, size_text, PAGE))
    header = (b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) +
              struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, section_offset,
                          0, 64, 56, len(segments), 64, sections,
                          sections - 1))
    image[0:len(header)] = header
    image[64:64 + 56 * len(segments)] = b"".join(segments)
    return bytes(image)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--oat-out", type=pathlib.Path,
                        help="also write the OAT-like file")
    arguments = parser.parse_args(argv)
    arguments.out.parent.mkdir(parents=True, exist_ok=True)
    arguments.out.write_bytes(build())
    if arguments.oat_out is not None:
        arguments.oat_out.parent.mkdir(parents=True, exist_ok=True)
        arguments.oat_out.write_bytes(build_oat())
    return 0


if __name__ == "__main__":
    sys.exit(main())
