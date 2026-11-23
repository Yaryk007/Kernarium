#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rewrite arm64 Linux libraries into one position-independent AOT blob.

The blob is the whole content of the AOT framework's __TEXT,__text: the
entry/exit routines, the rewritten code, the per-instruction ENTRY tables and
the per-page SHA-256 hashes and page digests that bind the code to the guest
bytes it stands for. See
docs/architecture.md (ahead-of-time code) for the contract this implements.

Every guest instruction is either inlined or becomes a STEP: AOT code leaves
at that instruction with committed state and the present engine executes it.
The rewriter therefore never needs to refuse for correctness.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import dataclasses
import hashlib
import json
import multiprocessing
import os
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import a64decode  # noqa: E402
import asm as A  # noqa: E402
import elf64  # noqa: E402
import licences  # noqa: E402

# ---- the register plan (docs/architecture.md, section "AOT code") -------

BUD, TLB, T0, T1, T2 = 24, 25, 26, 27, 28
SCRATCH = (T0, T1, T2)

FRAME = 0x110
SLOT_CPU, SLOT_BIAS, SLOT_BUD0, SLOT_SP = 0xA0, 0xA8, 0xB0, 0xB8
SLOT_XTAB = 0xF0
# Instructions started in this entry (a diagnostics build counts them).
SLOT_COUNT = 0xF8
# The state bytes of the library instance the code is running in: byte p is
# 1 once text page p matched its stored hash (the binder writes them
# between entries; the code only reads them).
SLOT_PSTATE = 0x100
# The frame's link slot, the last 8 bytes of the 0x110-byte frame: the guest
# keeps no link register in it at an indirect branch, so the lookup of the
# branch's target waits there.
SLOT_LR = 0x108
# CrossTable::Entry (aot_runtime.hpp): the offsets the cross-library thunk
# reads from the entry the host answers with.
CROSS_BIAS, CROSS_PAGES = 16, 24
# The refill thunk's own frame: x0-x17, x30, NZCV, then q0-q31.
REFILL_FRAME = 0x2a0
# The members of the host table that ENTER's last argument names (CrossTable
# in aot_runtime.hpp) the generated thunks call through.
XTAB_REFILL, XTAB_LOOKUP, XTAB_MOPS = 0, 8, 16
COND_LO, COND_MI = 3, 4
# The kinds the host's MOPS epilogue routine takes.
MOPS_KIND = {"cpyf": 0, "cpy": 1, "set": 2}
VSLOT = {18: 0xC0, 24: 0xC8, 25: 0xD0, 26: 0xD8, 27: 0xE0, 28: 0xE8}
VIRTUAL = frozenset(VSLOT) | {"sp"}

# The most instructions that run between two charges of the budget: every
# forward run of code from a charge point, and from any instruction an
# indirect branch can land on, is at most this long.
RUN_LIMIT = 32

EXIT_BUDGET, EXIT_BRANCH, EXIT_STEP = 0, 1, 2
ABSOLUTE = 0x100

PAGE = 4096
# The magic names the entry table's layout: 16-bit entries and an anchor per
# text page (Library.fill_entry16).
ENTRY_MAGIC = b"UMLAOT16"
# A function this long or longer makes the page that holds its end a row of
# branches in the entry table (Library.entry16_wide_pages).
WIDE16_FUNCTION = 32768
# The farthest an entry of a page may lie from its anchor: 65535 words.
ENTRY16_REACH = 4 * 65535
# The version of the layout the magic names (see the runtime's blob_version):
# 2 adds the count of libraries the rewriter left to the engine, 3 the
# digests of the release's manifest and system partition beside its ID, which
# bind the blob to the system a launch boots.
BLOB_VERSION = 3
HEADER_SIZE = 256
LIB_RECORD_SIZE = 256


def slot_of(guest) -> int:
    return SLOT_SP if guest == "sp" else VSLOT[guest]


def sext(value: int, bits: int) -> int:
    return (value & ((1 << bits) - 1)) - ((value & (1 << (bits - 1))) << 1)


# ---- the CPU state layout, read from the repository's own contract -------

def read_layout(header: pathlib.Path) -> dict[str, int]:
    values: dict[str, int] = {}
    pattern = re.compile(r"^#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$")
    for line in header.read_text().splitlines():
        match = pattern.match(line)
        if not match:
            continue
        expression = match.group(2)
        try:
            values[match.group(1)] = int(eval(  # noqa: S307
                expression, {"__builtins__": {}}, dict(values)))
        except (NameError, SyntaxError, TypeError):
            pass
    return values


LAYOUT_HEADER = (pathlib.Path(__file__).resolve().parents[2] / "darwin" /
                 "cpu" / "arm64" / "layout" / "assembly_offsets.h")


def crc32c_table() -> list[int]:
    table = []
    for index in range(256):
        value = index
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        table.append(value)
    return table


_CRC_TABLE = crc32c_table()


def crc32c_raw(data: bytes, crc: int) -> int:
    """The hardware CRC32C instruction folded over `data`: no pre/post xor."""
    table = _CRC_TABLE
    for byte in data:
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8)
    return crc


def page_digest(page: bytes) -> int:
    """The 64-bit identity of one 4 KiB page (see aot_digest.hpp)."""
    low = crc32c_raw(page, 0)
    high = crc32c_raw(page, 0x9e3779b9)
    digest = (high << 32) | low
    return digest or 1


PAGE_HASH_BYTES = 32


def page_hashes(source: bytes) -> bytes:
    """The SHA-256 of each 4 KiB page of `source`, in page order (see
    Binder::verify_page)."""
    if len(source) % PAGE:
        raise ValueError("the source is not a whole number of pages")
    return b"".join(hashlib.sha256(source[offset:offset + PAGE]).digest()
                    for offset in range(0, len(source), PAGE))


# ---- classification policy ----------------------------------------------

BASE_COPY_CLASSES = {
    "addsub_imm", "addsub_shift", "addsub_ext", "addsub_carry", "log_imm",
    "log_shift", "movewide", "bitfield", "extract", "condsel", "condcmp_reg",
    "condcmp_imm", "dp_3src",
}

RESERVED_CORNER_CLASSES = BASE_COPY_CLASSES | {"floatcmp"}


def reserved_corner(cls: str, word: int) -> bool:
    """Whether `word`, an encoding of the data-processing class `cls` or of
    the floating-point compare, is one the architecture leaves unallocated by
    a rule on its fields that the instruction tables do not express (a 32-bit
    operation naming a bit position of 32 or more, a bitmask no value has,
    a register in a field that must be zero). The engine raises an
    illegal instruction at such a word: copied, the host would raise it
    instead and end the emulator. A word like it is found as data inside the
    code of a method (a literal pool), where code can still be entered."""
    sf = word >> 31
    if cls == "bitfield":
        n = (word >> 22) & 1
        immr, imms = (word >> 16) & 63, (word >> 10) & 63
        return ((word >> 29) & 3) == 3 or n != sf or \
            (not sf and (immr | imms) & 32 != 0)
    if cls == "extract":
        return ((word >> 22) & 1) != sf or \
            (not sf and (word >> 10) & 32 != 0)
    if cls == "movewide":
        return ((word >> 29) & 3) == 1 or (not sf and (word >> 22) & 1 != 0)
    if cls == "addsub_shift":
        return ((word >> 22) & 3) == 3 or (not sf and (word >> 10) & 32 != 0)
    if cls == "log_shift":
        return not sf and (word >> 10) & 32 != 0
    if cls == "addsub_ext":
        return ((word >> 22) & 3) != 0 or ((word >> 10) & 7) > 4
    if cls == "floatcmp":
        # The forms that compare with zero (opcode2 bit 3) have no Rm.
        return (word >> 3) & 1 != 0 and (word >> 16) & 31 != 0
    if cls == "log_imm":
        n = (word >> 22) & 1
        imms = (word >> 10) & 63
        if not sf and n:
            return True
        length = ((n << 6) | (~imms & 63)).bit_length() - 1
        if length < 1:
            return True
        levels = (1 << length) - 1
        return imms & levels == levels
    return False


# Reserved forms of the classes the rewriter copies or translates field by
# field, which the instruction tables list as allocated: a size, Q, opcode,
# immediate or scale the architecture leaves unallocated for that form, and a
# multiply-high that names a register where the architecture wants the zero
# register. The engine raises an illegal instruction at each; copied, the host
# would raise it instead and end the emulator. Each pattern is bits 31 to 10
# of the word (1 and 0 fixed, x free; Rn and Rd take no part), as the engine
# answered for every word of the class that the rewriter would otherwise copy.
# A load of a pair into one register is the rule of `_memory`.
RESERVED_FORMS = {
    "asimdall": (
        "x0xxxxxx1xxxxxxxxxxxxx",
        "xxxxxxxx11xxxxxxxxxxxx",
    ),
    "asimddiff": (
        "xxxxxxxx00xxxxxx1xx1xx",
        "xxxxxxxx01xxxxxx111xxx",
        "xxxxxxxx10xxxxxx111xxx",
        "xxxxxxxx11xxxxxx0xxxxx",
        "xxxxxxxx11xxxxxxx0xxxx",
        "xxxxxxxx11xxxxxxxx0xxx",
    ),
    "asimdelem": (
        "xxxxxxxx00xxxxxxxxxxxx",
        "x0xxxxxx11xxxxxxxxxxxx",
        "xxxxxxxx11xxxxxxxx1xxx",
        "xxxxxxxx11xxxxxxxxx0xx",
        "xxxxxxxx11xxxxxx11xxxx",
        "xxxxxxxx111xxxxxxxxxxx",
    ),
    "asimdext": (
        "x0xxxxxxxxxxxxxxx1xxxx",
    ),
    "asimdins": (
        "x0xxxxxxxxxxx000xxxxxx",
        "x0xxxxxxxxxxxx00xx10xx",
        "xxxxxxxxxxxx0000xxxxxx",
        "xx0xxxxxxxxxx000xx10xx",
    ),
    "asimdmisc": (
        "xx0xxxxxx1xxxxxx0x01xx",
        "x0xxxxxxx1xxxxx11xxxxx",
        "xxxxxxxx1xxxxxxx0x01xx",
        "x0xxxxxx11xxxxxxxxxxxx",
        "xx1xxxxx1xxxxxxx000xxx",
        "xxxxxxxx11xxxxxx0x0xxx",
        "xxxxxxxx11xxxxxx0xx0xx",
        "xxxxxxxx11xxxxx1x1x0xx",
        "xxxxxxxx11xxxxx10xxxxx",
    ),
    "asimdperm": (
        "x0xxxxxx11xxxxxxxxxxxx",
    ),
    "asimdsame": (
        "xxxxxxxx00xxxxxx10110x",
        "x0xxxxxxx1xxxxxx11xxxx",
        "x0xxxxxx11xxxxxxxxx0xx",
        "x0xxxxxx11xxxxxxxxxx0x",
        "x0xxxxxx11xxxxxxxx1xxx",
        "x0xxxxxx11xxxxxxx1xxxx",
        "x0xxxxxx11xxxxxx1xxxxx",
        "x01xxxxxx1xxxxxx1xx11x",
        "xx1xxxxx1xxxxxxx1xx11x",
        "xxxxxxxx11xxxxxx000x0x",
        "xxxxxxxx11xxxxxx00x00x",
        "xxxxxxxx11xxxxxx011xxx",
        "xxxxxxxx11xxxxxx1001xx",
        "xx0xxxxx11xxxxxx1x10xx",
        "xxxxxxxx11xxxxxx101x0x",
        "xx1xxxxxx1xxxxxx10x11x",
        "xx1xxxxx11xxxxxx101xxx",
    ),
    "asimdshf": (
        "x0xxxxxxxx00xxxx11xxxx",
        "x0xxxxxxx1xxxxxxxxxxxx",
        "xxxxxxxxx000xxxx11xxxx",
        "xxxxxxxxx1xxxxxx10xxxx",
    ),
    "asisddiff": (
        "xxxxxxxx00xxxxxxxxxxxx",
        "xxxxxxxx11xxxxxxxxxxxx",
    ),
    "asisdelem": (
        "xxxxxxxx00xxxxxxxxxxxx",
        "xxxxxxxx11xxxxxxxx1xxx",
        "xxxxxxxx11xxxxxx11xxxx",
        "xxxxxxxx111xxxxxxxxxxx",
    ),
    "asisdmisc": (
        "xxxxxxxx11xxxxx10xxxxx",
    ),
    "asisdone": (
        "xxxxxxxxxxxx0000xxxxxx",
    ),
    "asisdsame": (
        "xxxxxxxx00xxxxxx10xxxx",
        "xxxxxxxx11xxxxxx101xxx",
    ),
    "asisdshf": (
        "xxxxxxxxx000xxxx11xxxx",
        "xxxxxxxxx1xxxxxx10xxxx",
    ),
    "dp_3src": (
        "xxxxxxxxx1xxxxxxx0xxxx",
        "xxxxxxxxx1xxxxxxxx0xxx",
        "xxxxxxxxx1xxxxxxxxx0xx",
        "xxxxxxxxx1xxxxxxxxxx0x",
        "xxxxxxxxx1xxxxxxxxxxx0",
    ),
    "float2fix": (
        "0xxxxxxxxxxxxxxx0xxxxx",
    ),
}


def _compile_forms(forms):
    compiled = {}
    for cls, patterns in forms.items():
        rows = []
        for pattern in patterns:
            assert len(pattern) == 22, pattern
            mask = value = 0
            for bit, char in zip(range(31, 9, -1), pattern):
                if char != "x":
                    mask |= 1 << bit
                    value |= int(char) << bit
            rows.append((mask, value))
        compiled[cls] = tuple(rows)
    return compiled


_RESERVED_FORMS = _compile_forms(RESERVED_FORMS)


def reserved_form(cls: str, word: int) -> bool:
    """Whether `word`, an encoding of class `cls`, is a reserved form of
    RESERVED_FORMS."""
    for mask, value in _RESERVED_FORMS.get(cls, ()):
        if word & mask == value:
            return True
    return False


DP1_ALLOWED = {"RBIT", "REV16", "REV", "REV32", "CLZ", "CLS"}
DP2_ALLOWED = {"UDIV", "SDIV", "LSLV", "LSRV", "ASRV", "RORV", "CRC32B",
               "CRC32H", "CRC32W", "CRC32X", "CRC32CB", "CRC32CH",
               "CRC32CW", "CRC32CX"}
SIMD_CLASSES = {
    "asimdsame", "asimdmisc", "asimdall", "asimddiff", "asimdelem",
    "asimdext", "asimdperm", "asimdins", "asimdshf", "asimdtbl", "asimdimm",
    "asisdsame", "asisdmisc", "asisdone", "asisdpair", "asisdshf",
    "asisddiff", "asisdelem", "cryptoaes", "cryptosha2", "cryptosha3",
}
SIMD_FEATURES = {"FEAT_AdvSIMD", "FEAT_AES", "FEAT_PMULL", "FEAT_SHA1",
                 "FEAT_SHA256"}
# Scalar floating point. The engine runs these as the host's own instruction
# under the guest FPCR and FPSR for single and double precision; its software
# paths exist for half precision (FEAT_FP16), BFloat16, FRINT32/64
# (FEAT_FRINTTS), FJCVTZS, the new FPRCVT forms and the dot-product and FHM
# families, which the decoder labels with a feature of their own and which
# therefore step. FPCR.AH, FIZ and NEP are excluded at entry.
FLOAT_CLASSES = {"floatdp1", "floatdp2", "floatdp3", "floatcmp", "floatccmp",
                 "floatsel", "floatimm"}
FP_FEATURES = {"FEAT_FP", "FEAT_AdvSIMD"}
MEM_CLASSES = {
    "ldst_pos", "ldst_unscaled", "ldst_immpre", "ldst_immpost", "ldst_regoff",
    "ldstpair_off", "ldstpair_pre", "ldstpair_post", "ldstnapair_offs",
    "loadlit",
}
PAIR_CLASSES = {"ldstpair_off", "ldstpair_pre", "ldstpair_post",
                "ldstnapair_offs"}
MEM_FEATURES = {"baseline", "FEAT_FP"}
# Advanced SIMD load/store structures: multiple structures and single
# structures, without an offset and with a post-index.
SIMD_STRUCT_CLASSES = {"asisdlse", "asisdlsep", "asisdlso", "asisdlsop"}
# The sizes of an access that the refill thunks name.
REFILL_TOTALS = (1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64)
# A function branches to an exit stub that an earlier function emitted when
# the stub is no further back than this many words: a conditional branch
# reaches 1 MiB, and a quarter of that stays for the code that relaxation
# adds after the reference.
XB_SHARE_WORDS = 192 * 1024
# HINT #imm7. The engine runs YIELD (1), DGH (6), CSDB (20) and CLRBHB (22)
# as gadgets with an effect and every other hint, allocated or not, as a
# no-op (pointer authentication, branch targets, GCS and the rest are not
# advertised to the guest): gadget_scalar_selection.cpp,
# architectural_hint_gadget().
HINT_MASK = 0xfffff01f
HINT_BASE = 0xd503201f
HINT_EFFECTS = (1, 6, 20, 22)
SITE_SAMPLES = 8
# LDAR/LDARB/LDARH and STLR/STLRB/STLRH (size in bits 31:30), the LSE atomic
# memory operations (LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX,
# LDUMIN and SWP in any acquire/release form) and CAS. The other words of the
# same encoding groups (LDLAR, LDAPR, ST64B, CASP, ...) step.
ORDERED_MASK = 0x3ffffc00
LDAR_BASE, STLR_BASE = 0x08dffc00, 0x089ffc00
AMO_MASK, AMO_BASE = 0x3f200c00, 0x38200000
CAS_MASK, CAS_BASE = 0x3fa07c00, 0x08a07c00
TPIDR_EL0 = 0xd53bd040
TPIDR_MASK = 0xffffffe0


# The byte census of a unit: every emitted word has one kind. The narrowest
# emitter that knows a word's purpose tags it, and the untagged words of an
# instruction take the instruction's class. "mem.probe" is the inline TLB
# probe of plain loads and stores only: an atomic's probe is part of
# "atomic", a copy's or set's of "mops.site" and "mops.thunk".
CENSUS = ("other", "insn", "spill", "branch", "pcrel", "step", "hint",
          "atomic", "mops.site", "mem.ea", "mem.probe", "mem.access",
          "mem.wb", "charge", "gate", "stub.mm", "stub.st", "stub.xu",
          "stub.ma", "stub.xb", "exits", "mops.thunk", "table", "src",
          "stub.refill", "count")
CENSUS_CODE = {name: code for code, name in enumerate(CENSUS)}
CENSUS_OUTER = {"branch_imm": "branch", "condbranch": "branch",
                "compbranch": "branch", "testbranch": "branch",
                "branch_reg": "branch", "pcreladdr": "pcrel",
                "memcms": "mops.site", "ldstord": "atomic", "memop": "atomic",
                "comswap": "atomic", "hint": "hint"}


@dataclasses.dataclass
class Stats:
    functions: int = 0
    instructions: int = 0
    inlined: int = 0
    steps: int = 0
    steps_by_cause: collections.Counter = dataclasses.field(
        default_factory=collections.Counter)
    kinds: collections.Counter = dataclasses.field(
        default_factory=collections.Counter)
    # Words emitted, by instruction class, charges, stubs and the rest.
    words: collections.Counter = dataclasses.field(
        default_factory=collections.Counter)
    # Bytes of the unit by kind (CENSUS), set when the unit is built.
    census: dict = dataclasses.field(default_factory=dict)


class EntryTableRefused(Exception):
    """A library whose entry table cannot be formed: it is left out of the
    blob and the engine runs it."""


class Library:
    """One library's rewrite: classification, then emission into the blob."""

    def __init__(self, index: int, name: str, elf: elf64.Elf,
                 decoder: a64decode.Decoder, layout: dict[str, int],
                 count_retired: bool = False) -> None:
        self.count_retired = count_retired
        self.index = index
        self.name = name
        self.elf = elf
        self.decoder = decoder
        self.layout = layout
        self.stats = Stats()
        segment = elf.executable_segment()
        self.segment = segment
        self.table_base = segment.vaddr & ~(PAGE - 1)
        end = (segment.vaddr + segment.filesz + PAGE - 1) & ~(PAGE - 1)
        self.table_end = end
        self.table_words = (end - self.table_base) // 4
        span = end - self.table_base
        self.log_span = max(span - 1, 1).bit_length()
        self.functions = [f for f in elf.functions()
                          if segment.vaddr <= f.start and
                          f.end <= segment.vaddr + segment.filesz and
                          f.start % 4 == 0 and f.end % 4 == 0]
        self.covered: set[int] = set()
        for function in self.functions:
            self.covered.update(range(function.start, function.end, 4))
        self.words = {}
        for function in self.functions:
            data = elf.read_vaddr(function.start, function.end - function.start)
            for offset in range(0, len(data), 4):
                self.words[function.start + offset] = struct.unpack_from(
                    "<I", data, offset)[0]
        self.entry_labels: list[int] = []
        self.step_list: list[tuple[int, str]] = []
        # Inlined instructions by "class:mnemonic": where each kind occurs,
        # so the differential test can start cases at a sample of each.
        self.sites: dict[str, list[int]] = {}
        self._site_key = ""
        # Per function, cleared as each is emitted: the cold stubs it needs.
        self.fn_start = 0
        self.fn_mm: list[tuple[int, str, int, bool]] = []
        self.fn_xu: set[int] = set()
        self.fn_xb: set[int] = set()
        # The exit-by-branch stubs already emitted, by target: (label, the
        # word the stub starts at). A later function branches to one within
        # reach instead of emitting its own.
        self.xb_emitted: dict[int, tuple[str, int]] = {}
        # The refill routines the cold miss stubs enter: (kind, bytes,
        # chunk of the entry table, whether the probe left the address of
        # the first byte rather than the last).
        self.refill_used: set[tuple[str, int, int, bool]] = set()
        self.fn_pg: set[int] = set()
        self.fn_ma: list[int] = []
        self.fn_st: list[int] = []
        # The register combinations of the MOPS epilogues in the library:
        # (kind, destination, count, source), one thunk each.
        self.mops_keys: set[tuple[str, int, int, int]] = set()
        # The guest offset of the instruction being emitted.
        self.cur = 0
        self.charge = self._plan_charges()
        # One census code per emitted word (see CENSUS).
        self.census = bytearray()

    def _flow(self, word: int, offset: int) -> tuple[str, int | None]:
        """How control leaves an instruction: its kind and direct target."""
        encoding = self.decoder.decode(word)
        if encoding is None:
            return "seq", None
        cls = encoding.decode_class
        mnemonic = encoding.mnemonic
        if cls == "branch_imm":
            target = offset + sext(word & 0x3ffffff, 26) * 4
            return ("call" if mnemonic == "BL" else "jump"), target
        if cls == "condbranch" and mnemonic == "B":
            return "cond", offset + sext((word >> 5) & 0x7ffff, 19) * 4
        if cls == "compbranch":
            return "cond", offset + sext((word >> 5) & 0x7ffff, 19) * 4
        if cls == "testbranch":
            return "cond", offset + sext((word >> 5) & 0x3fff, 14) * 4
        if cls == "branch_reg" and mnemonic in ("BR", "BLR", "RET"):
            return "indirect", None
        return "seq", None

    def _plan_charges(self) -> dict[int, int]:
        """Where the budget is charged, and by how much.

        A charge point pays in advance for the longest run of instructions
        that can follow it before the next charge point. Charge points are
        the function entries, every target of a backward or self branch (so
        every cycle of direct branches contains one), the targets of calls,
        and points inserted so that no run from an instruction that is not a
        charge point exceeds RUN_LIMIT. A direct branch to a charge point,
        and a fall-through into it, pay it; so does a landing through the
        entry table. An indirect branch pays for itself and for the longest
        run after any instruction it can land on (RUN_LIMIT), which no
        charge point has paid: with that every cycle, however it closes,
        pays at least as much as it executes, and an entry runs at most its
        budget and one unpaid run.
        """
        points = {function.start for function in self.functions}
        flow: dict[int, tuple[str, int | None]] = {}
        for offset, word in self.words.items():
            kind, target = self._flow(word, offset)
            flow[offset] = (kind, target)
            if target is not None and target in self.covered and (
                    kind == "call" or target <= offset):
                points.add(target)
        run: dict[int, int] = {}
        for offset in sorted(self.words, reverse=True):
            kind, target = flow[offset]
            longest = 0
            successors = []
            if kind in ("seq", "cond"):
                successors.append(offset + 4)
            if kind in ("jump", "cond") and target is not None and \
                    target > offset:
                successors.append(target)
            for successor in successors:
                if successor in self.covered and successor not in points:
                    longest = max(longest, run[successor])
            run[offset] = 1 + longest
            if offset not in points and run[offset] > RUN_LIMIT:
                points.add(offset)
        return {offset: run[offset] for offset in points
                if offset in self.covered}

    # labels
    def tag(self, kind: str, start: int) -> None:
        """Give the words emitted since `start` that have no kind this one."""
        end = len(self.asm._w)
        census = self.census
        if len(census) < end:
            census.extend(bytes(end - len(census)))
        if end > start:
            census[start:end] = census[start:end].replace(
                b"\0", bytes((CENSUS_CODE[kind],)))

    def lab(self, kind: str, value: int) -> str:
        return f"L{self.index}{kind}{value:x}"

    def mops_label(self, key: tuple[str, int, int, int]) -> str:
        return f"L{self.index}MOPS_{key[0]}_{key[1]}_{key[2]}_{key[3]}"

    def goto_label(self, target: int) -> str:
        if target in self.covered:
            # A target on another text page goes through that page's gate:
            # the page must have matched its stored hash.
            if target // PAGE != self.cur // PAGE:
                self.fn_pg.add(target)
                return self.pg_label(target)
            return self.direct_label(target)
        return self.xb_ref(target)

    def direct_label(self, target: int) -> str:
        # The branch charges the budget where the target is a charge point.
        return self.lab("G" if target in self.charge else "E", target)

    def pg_label(self, target: int) -> str:
        return f"L{self.index}PG{self.fn_start:x}_{target:x}"

    def page_test(self, target: int) -> None:
        """Leave by the branch exit of `target` unless its text page is
        known to match its stored hash: T0 is clobbered, flags are not."""
        page = (target - self.table_base) // PAGE
        if page >= 1 << 24:
            raise A.AsmError(
                "a library's text page index must fit 24 bits (64 GiB); a "
                "unit is limited much earlier, by the +-128 MiB reach of b")
        asm = self.asm
        before = len(asm._w)
        asm.word(A.ldr_x(T0, 31, SLOT_PSTATE))
        if page >= 4096:
            # The byte offset of the page's state does not fit the load's 12
            # bits: the rest of it is added first.
            asm.word(A.add_imm(T0, T0, page >> 12, shift12=True))
            page &= 0xfff
        asm.word(A.ldrb_w(T0, T0, page))
        asm.cbz(T0, self.xb_ref(target), sf=0)
        self.tag("gate", before)
        self.stats.words["gate"] += len(asm._w) - before

    def xb_label(self, target: int) -> str:
        # A target below the library's first byte is a negative offset.
        return f"L{self.index}XB{self.fn_start:x}_{target & ((1 << 64) - 1):x}"

    def xb_ref(self, target: int) -> str:
        """The label to branch to for leaving by the branch exit of `target`:
        the stub an earlier function emitted when it is behind this word
        within the reach of a conditional branch, else this function's own
        (emitted after its code)."""
        emitted = self.xb_emitted.get(target)
        if emitted is not None and \
                len(self.asm._w) - emitted[1] < XB_SHARE_WORDS:
            return emitted[0]
        self.fn_xb.add(target)
        return self.xb_label(target)

    # emission -------------------------------------------------------------
    def build(self, digests: list[int], source: bytes = b"") -> dict:
        """Emit the whole library as one self-contained unit.

        `source` is the library's text pages as the guest must map them. The
        unit stores the SHA-256 of each page, not the page: the binder hashes
        each mapped page and compares the hash with the stored one before any
        rewritten code of the page runs, so the code runs for bytes other
        than `source` only on a collision of SHA-256. `digests` (one per
        page) only find the library in the blob's hash.
        """
        asm = self.asm = A.Assembler()
        self.xb_emitted = {}
        self.refill_used = set()
        asm.label("UNIT")
        emit_exits(asm, self)
        self.tag("exits", 0)
        for function in self.functions:
            self.stats.functions += 1
            self._emit_function(function)
        before = len(asm._w)
        emit_mops_thunks(asm, self)
        self.stats.words["mops_thunks"] += len(asm._w) - before
        self.tag("mops.thunk", before)
        before = len(asm._w)
        emit_refill_stubs(asm, self)
        self.tag("stub.refill", before)
        before = len(asm._w)
        self.wide16 = self.entry16_wide_pages()
        have = set(self.entry_labels)
        for page in self.wide16:
            # One `b` per word of the page, to the word's native code: the
            # page's 16-bit entries index it (see fill_entry16).
            asm.label(f"TR{page:x}")
            for word in range(page * 1024, (page + 1) * 1024):
                offset = self.table_base + 4 * word
                if offset in have:
                    asm.b(self.lab("G" if offset in self.charge else "E",
                                   offset))
                else:
                    asm.word(0)
        self.tag("table", before)
        asm.label("TAB")
        before = len(asm._w)
        asm.data(bytes(self.table_region_bytes()))
        self.tag("table", before)
        asm.label("SRC")
        before = len(asm._w)
        asm.data(page_hashes(source))
        self.tag("src", before)
        unit = bytearray(asm.resolve())
        self.stats.census = {
            name: 4 * self.census.count(code)
            for name, code in CENSUS_CODE.items()}
        # Conditional branches out of range grew by one word each.
        self.stats.census["relax"] = len(unit) - 4 * len(asm._w)
        # Every word has a kind: a framework build holds what the tooling
        # test holds for the fixture library.
        if self.stats.census["other"]:
            raise RuntimeError(
                f"{self.name}: {self.stats.census['other']} bytes of the "
                "unit have no census kind")
        table_at = asm.label_address("TAB")
        entries = {offset: asm.label_address(
            self.lab("G" if offset in self.charge else "E", offset))
            for offset in self.entry_labels}
        self.fill_entry16(unit, table_at, entries, {
            page: asm.label_address(f"TR{page:x}") for page in self.wide16})
        return {"unit": bytes(unit), "table_at": table_at,
                "table_region": self.table_region_bytes(),
                "hashes_at": asm.label_address("SRC")}

    # ---- the entry table ----------------------------------------------------
    #
    # The table holds 16 bits per source word (the unit offset of the word's
    # native code, 0 for a word without), followed (from the next 4 KiB, so
    # that the array is one add away) by one signed 32-bit anchor per text
    # page, relative to that array: a word's entry is the anchor of its page
    # plus four bytes for each unit of its 16 bits. The anchor is the page's
    # lowest entry less one word, so the page's entries must lie within
    # 256 KiB of code. A function's out-of-line stubs follow it, so a page
    # that holds the end of a function of WIDE16_FUNCTION bytes or more (and
    # the start of the next) can be wider than that: it gets a row of 1024
    # branches instead, one to each word's native code, and its entries
    # index the row. A table that cannot be formed (a page whose entries are
    # still further apart, or more than 16 MiB of entries) raises
    # EntryTableRefused, and the library is left to the engine.

    def entry16_wide_pages(self) -> tuple[int, ...]:
        """The pages whose entries are reached through a row of branches."""
        pages = set()
        for function in self.functions:
            if function.end - function.start >= WIDE16_FUNCTION and \
                    function.end % PAGE:
                pages.add((function.end - 4 - self.table_base) // PAGE)
        return tuple(sorted(pages))

    def table_len16(self) -> int:
        return (2 * self.table_words + PAGE - 1) & ~(PAGE - 1)

    def table_region_bytes(self) -> int:
        return self.table_len16() + 4 * ((self.table_words + 1023) // 1024)

    def fill_entry16(self, unit: bytearray, table_at: int,
                     entries: dict[int, int],
                     rows: dict[int, int] | None = None) -> None:
        rows = rows or {}
        pb_at = table_at + self.table_len16()
        pages: dict[int, list[tuple[int, int]]] = collections.defaultdict(
            list)
        for offset, address in entries.items():
            word = (offset - self.table_base) // 4
            pages[word // 1024].append((word, address))
        for page, items in pages.items():
            if page in rows:
                struct.pack_into("<i", unit, pb_at + 4 * page,
                                 rows[page] - 4 - pb_at)
                for word, _ in items:
                    struct.pack_into("<H", unit, table_at + 2 * word,
                                     word % 1024 + 1)
                continue
            anchor = min(address for _, address in items) - 4
            if max(address for _, address in items) - anchor > ENTRY16_REACH:
                raise EntryTableRefused(
                    f"{self.name}: the entries of text page {page} span "
                    "more than 256 KiB of code")
            struct.pack_into("<i", unit, pb_at + 4 * page, anchor - pb_at)
            for word, address in items:
                struct.pack_into("<H", unit, table_at + 2 * word,
                                 (address - anchor) // 4)

    def _emit_function(self, function: elf64.Function) -> None:
        asm = self.asm
        self.fn_start = function.start
        self.fn_mm = []
        self.fn_xu = set()
        self.fn_xb = set()
        self.fn_pg = set()
        self.fn_ma = []
        self.fn_st = []
        last_falls_through = True
        for offset in range(function.start, function.end, 4):
            word = self.words[offset]
            self.cur = offset
            # Falling into the next page of the function is an edge between
            # pages; branches into the page enter at the labels below.
            if offset % PAGE == 0 and offset != function.start and \
                    last_falls_through:
                self.page_test(offset)
            cost = self.charge.get(offset, 0)
            if cost:
                asm.label(self.lab("G", offset))
                self.fn_xu.add(offset)
                before = len(asm._w)
                asm.word(A.add_imm(BUD, BUD, cost, sub=True))
                asm.tbz(BUD, 63, self.lab("XU", offset), nz=True)
                self.tag("charge", before)
                self.stats.words["charge"] += 2
            asm.label(self.lab("E", offset))
            if self.count_retired:
                before = len(asm._w)
                asm.word(A.ldr_x(T2, 31, SLOT_COUNT))
                asm.word(A.add_imm(T2, T2, 1))
                asm.word(A.str_x(T2, 31, SLOT_COUNT))
                self.tag("count", before)
                self.stats.words["count"] += 3
            self.entry_labels.append(offset)
            self.stats.instructions += 1
            last_falls_through = self._emit_instruction(offset, word)
        if last_falls_through:
            self.cur = function.end - 4
            before = len(asm._w)
            asm.b(self.goto_label(function.end))
            self.tag("branch", before)
        before = len(asm._w)
        self._emit_stubs(function)
        self.stats.words["stubs"] += len(asm._w) - before

    # classification of one instruction; returns whether it falls through
    def _emit_instruction(self, offset: int, word: int) -> bool:
        steps = self.stats.steps
        self._site_key = "unallocated"
        before = len(self.asm._w)
        result = self._classify(offset, word)
        label = self._site_key.split(":")[0] if self.stats.steps == steps \
            else "step"
        self.tag("step" if label == "step" else
                 CENSUS_OUTER.get(label, "spill"), before)
        self.stats.words[label] += len(self.asm._w) - before
        if self.stats.steps == steps:
            self.sites.setdefault(self._site_key, []).append(offset)
        return result

    def _classify(self, offset: int, word: int) -> bool:
        if (word & HINT_MASK) == HINT_BASE:
            return self._hint(offset, word)
        encoding = self.decoder.decode(word)
        if encoding is None:
            return self._step(offset, "unallocated")
        cls, mnemonic, feature = (encoding.decode_class, encoding.mnemonic,
                                  encoding.feature)
        self._site_key = f"{cls}:{mnemonic}"
        if mnemonic == "UNALLOCATED":
            return self._step(offset, "unallocated")
        if (cls in RESERVED_CORNER_CLASSES and reserved_corner(cls, word)) \
                or reserved_form(cls, word):
            return self._step(offset, "reserved")
        if cls == "branch_imm":
            return self._branch_imm(offset, word, mnemonic == "BL")
        if cls == "condbranch" and mnemonic == "B":
            target = offset + sext((word >> 5) & 0x7ffff, 19) * 4
            return self._cond_branch(offset, "bcond", word & 15, target)
        if cls == "compbranch":
            target = offset + sext((word >> 5) & 0x7ffff, 19) * 4
            return self._compare_branch(offset, word, target)
        if cls == "testbranch":
            target = offset + sext((word >> 5) & 0x3fff, 14) * 4
            return self._test_branch(offset, word, target)
        if cls == "branch_reg" and mnemonic in ("BR", "BLR", "RET"):
            return self._indirect_branch(offset, word, mnemonic)
        if cls == "pcreladdr":
            return self._pc_relative(offset, word, mnemonic == "ADRP")
        if cls in MEM_CLASSES and feature in MEM_FEATURES:
            return self._memory(offset, word, encoding)
        if cls in SIMD_STRUCT_CLASSES and feature == "FEAT_AdvSIMD":
            return self._simd_struct(offset, word, cls)
        if cls == "systemmove" and (word & TPIDR_MASK) in (
                TPIDR_EL0, TPIDR_EL0 ^ 0x00200000):
            return self._tpidr(offset, word)
        if cls == "systemmove" and (word & 0xffffffe0) in (
                0xd53b4200, 0xd51b4200):
            return self._copy(offset, word, encoding, [(0, False)])
        if cls == "barriers" and mnemonic == "DMB":
            self.asm.word(word)
            self.stats.inlined += 1
            self.stats.kinds["dmb"] += 1
            return True
        if cls == "ldstord" and feature == "baseline":
            return self._ordered(offset, word)
        if cls in ("memop", "comswap") and feature == "FEAT_LSE":
            return self._atomic(offset, word)
        if cls == "memcms" and feature == "FEAT_MOPS":
            return self._mops(offset, word)
        fields = self._copy_fields(word, encoding)
        if fields is not None:
            return self._copy(offset, word, encoding, fields)
        return self._step(offset, f"{cls}:{mnemonic}:{feature}")

    def _hint(self, offset: int, word: int) -> bool:
        self._site_key = "hint"
        if ((word >> 5) & 0x7f) in HINT_EFFECTS:
            return self._step(offset, "hint-effect")
        self.stats.inlined += 1
        self.stats.kinds["hint"] += 1
        return True

    def _copy_fields(self, word: int, encoding: a64decode.Encoding):
        cls, mnemonic, feature = (encoding.decode_class, encoding.mnemonic,
                                  encoding.feature)
        if cls in BASE_COPY_CLASSES and feature == "baseline":
            s_bit = (word >> 29) & 1
            if cls == "addsub_imm":
                return [(0, not s_bit), (5, True)]
            if cls == "addsub_ext":
                return [(0, not s_bit), (5, True), (16, False)]
            if cls in ("addsub_shift", "addsub_carry", "log_shift", "condsel",
                       "extract"):
                return [(0, False), (5, False), (16, False)]
            if cls == "log_imm":
                return [(0, ((word >> 29) & 3) != 3), (5, False)]
            if cls == "movewide":
                return [(0, False)]
            if cls == "bitfield":
                return [(0, False), (5, False)]
            if cls == "condcmp_reg":
                return [(5, False), (16, False)]
            if cls == "condcmp_imm":
                return [(5, False)]
            if cls == "dp_3src":
                return [(0, False), (5, False), (10, False), (16, False)]
        if cls == "dp_1src" and feature == "baseline" and \
                mnemonic in DP1_ALLOWED:
            return [(0, False), (5, False)]
        if cls == "dp_2src" and feature in ("baseline", "FEAT_CRC32") and \
                mnemonic in DP2_ALLOWED:
            return [(0, False), (5, False), (16, False)]
        if cls in FLOAT_CLASSES and feature in FP_FEATURES:
            return []
        if cls == "float2int" and feature in FP_FEATURES:
            # SCVTF, UCVTF and FMOV from a general register name it in Rn;
            # every other form writes Rd.
            return [(5, False)] if (word >> 16) & 7 in (2, 3, 7) else \
                [(0, False)]
        if cls == "float2fix" and feature in FP_FEATURES:
            return [(5, False)] if (word >> 16) & 7 in (2, 3) else \
                [(0, False)]
        if cls in SIMD_CLASSES and feature in SIMD_FEATURES:
            if cls == "asimdins" and not (word >> 29) & 1:
                # INS (general), DUP (general), SMOV and UMOV (op = 0); INS
                # (element) (op = 1) names vector registers only.
                imm4 = (word >> 11) & 15
                if imm4 in (1, 3):
                    return [(5, False)]
                if imm4 in (5, 7):
                    return [(0, False)]
            return []
        return None

    # --- generic helpers ---------------------------------------------------
    def _resolve(self, value: int, sp_capable: bool):
        if value == 31:
            return "sp" if sp_capable else "zr"
        return value

    def _step(self, offset: int, cause: str) -> bool:
        """Leave at this instruction with committed state: the engine runs it."""
        self._exit_to(offset, "EXIT_STEP")
        self.step_list.append((offset, cause))
        self.stats.steps += 1
        self.stats.steps_by_cause[cause] += 1
        return False

    def _exit_to(self, offset: int, routine: str) -> None:
        """T0 = the guest offset, then the named exit routine. The offset is
        formed in 32 bits: an exit address beyond that would leave at a
        wrapped PC, so a build that has one is refused.

        A branch exit (a direct branch to code outside this library) may
        target below the library's first byte, where another library mapped
        lower lies (an OAT file of the boot image branching into one that
        precedes it), or beyond 4 GiB: its target is then formed as a full
        address (the load bias plus the offset, wrapping) and leaves through
        the absolute exit."""
        asm = self.asm
        if routine == "EXIT_BRANCH" and not 0 <= offset < 1 << 32:
            self._materialize(T0, offset & ((1 << 64) - 1))
            asm.b("EXIT_BRANCH_ABS")
            return
        if not 0 <= offset < 1 << 32:
            raise A.AsmError(
                f"an exit to {offset:#x} does not fit the 32 bits an exit "
                "address is formed in (a library's text must lie below "
                "4 GiB)")
        asm.word(A.movz(T0, offset & 0xffff))
        if offset >> 16:
            asm.word(A.movk(T0, (offset >> 16) & 0xffff, 1))
        asm.b(routine)

    def _load_guest(self, host: int, guest) -> None:
        self.asm.word(A.ldr_x(host, 31, slot_of(guest)))

    def _store_guest(self, host: int, guest) -> None:
        self.asm.word(A.str_x(host, 31, slot_of(guest)))

    def _materialize(self, host: int, vaddr: int) -> None:
        """host = BIAS + vaddr (a guest virtual address). A target below the
        unit's zero is the 64-bit wrap of a negative number, never its low
        24 bits."""
        vaddr &= (1 << 64) - 1
        asm = self.asm
        asm.word(A.ldr_x(host, 31, SLOT_BIAS))
        low = vaddr & 0xfff
        high = (vaddr >> 12) & 0xfff
        if vaddr >= (1 << 24):
            asm.word(A.movz(T1 if host != T1 else T2, vaddr & 0xffff))
            scratch = T1 if host != T1 else T2
            asm.word(A.movk(scratch, (vaddr >> 16) & 0xffff, 1))
            if vaddr >> 32:
                asm.word(A.movk(scratch, (vaddr >> 32) & 0xffff, 2))
            if vaddr >> 48:
                asm.word(A.movk(scratch, (vaddr >> 48) & 0xffff, 3))
            asm.word(A.add_reg(host, host, scratch))
            return
        if low:
            asm.word(A.add_imm(host, host, low))
        if high:
            asm.word(A.add_imm(host, host, high, shift12=True))

    # --- plain instructions ---------------------------------------------------
    def _copy(self, offset: int, word: int, encoding, fields) -> bool:
        needed: list = []
        for lsb, sp_capable in fields:
            guest = self._resolve((word >> lsb) & 31, sp_capable)
            if guest in VIRTUAL and guest not in needed:
                needed.append(guest)
        if not needed:
            self.asm.word(word)
            self.tag("insn", len(self.asm._w) - 1)
        elif len(needed) > len(SCRATCH):
            return self._step(offset, "operands")
        else:
            mapping = dict(zip(needed, SCRATCH))
            patched = word
            for guest in needed:
                self._load_guest(mapping[guest], guest)
            for lsb, sp_capable in fields:
                guest = self._resolve((word >> lsb) & 31, sp_capable)
                if guest in mapping:
                    patched = (patched & ~(31 << lsb)) | (mapping[guest] << lsb)
            self.asm.word(patched)
            self.tag("insn", len(self.asm._w) - 1)
            written = {self._resolve(word & 31, sp_capable)
                       for lsb, sp_capable in fields if lsb == 0}
            for guest in needed:
                if guest in written:
                    self._store_guest(mapping[guest], guest)
            if "sp" in written and (word & 31) == 31:
                self._sp_aligned_after(offset, word, encoding, mapping["sp"])
        self.stats.inlined += 1
        self.stats.kinds[encoding.mnemonic] += 1
        return True

    def _sp_aligned_after(self, offset: int, word: int, encoding,
                          register: int) -> None:
        """Keep the invariant that AOT code runs with a 16-byte aligned SP.

        The engine faults an SP-based access made with a misaligned SP. AOT is
        entered only with an aligned SP (GadgetExecutor::run_aot), and an
        instruction that leaves SP misaligned ends the entry after it
        commits, so the engine runs the rest. A write that adds a multiple of
        sixteen to an aligned SP needs no test. The test leaves the flags
        alone.
        """
        if encoding.decode_class == "addsub_imm" and (word >> 5) & 31 == 31:
            amount = ((word >> 10) & 0xfff) << (12 if (word >> 22) & 1 else 0)
            if amount % 16 == 0:
                return
        self._guard_sp(offset, register)

    def _guard_sp(self, offset: int, register: int) -> None:
        asm = self.asm
        check = T1 if register != T1 else T2
        asm.word(A.ubfx(check, register, 0, 4))
        self.fn_ma.append(offset)
        asm.cbz(check, self.lab("MA", offset), nz=True)

    def _tpidr(self, offset: int, word: int) -> bool:
        asm = self.asm
        rt = word & 31
        is_read = (word & 0x00200000) != 0
        guest = self._resolve(rt, False)
        asm.word(A.ldr_x(T0, 31, SLOT_CPU))
        tls = self.layout["UML_GADGET_CPU_TLS"]
        if is_read:
            if guest == "zr":
                pass
            elif guest in VIRTUAL:
                asm.word(A.ldr_x(T1, T0, tls))
                self._store_guest(T1, guest)
            else:
                asm.word(A.ldr_x(rt, T0, tls))
        else:
            if guest == "zr":
                asm.word(A.str_x(31, T0, tls))
            elif guest in VIRTUAL:
                self._load_guest(T1, guest)
                asm.word(A.str_x(T1, T0, tls))
            else:
                asm.word(A.str_x(rt, T0, tls))
        self.stats.inlined += 1
        self.stats.kinds["tpidr"] += 1
        return True

    # --- control flow -------------------------------------------------------
    def _branch_imm(self, offset: int, word: int, link: bool) -> bool:
        target = offset + sext(word & 0x3ffffff, 26) * 4
        if link:
            self._materialize(30, offset + 4)
        self.asm.b(self.goto_label(target))
        self.stats.inlined += 1
        self.stats.kinds["bl" if link else "b"] += 1
        return False

    def _cond_branch(self, offset: int, kind: str, cond: int, target: int
                     ) -> bool:
        self.asm.bcond(cond, self.goto_label(target))
        self.stats.inlined += 1
        self.stats.kinds["bcond"] += 1
        return True

    def _compare_branch(self, offset: int, word: int, target: int) -> bool:
        sf = word >> 31
        nz = bool((word >> 24) & 1)
        guest = self._resolve(word & 31, False)
        rt = word & 31
        if guest in VIRTUAL:
            self._load_guest(T0, guest)
            rt = T0
        self.asm.cbz(rt, self.goto_label(target), sf=sf, nz=nz)
        self.stats.inlined += 1
        self.stats.kinds["cbz"] += 1
        return True

    def _test_branch(self, offset: int, word: int, target: int) -> bool:
        nz = bool((word >> 24) & 1)
        bit = ((word >> 31) << 5) | ((word >> 19) & 31)
        guest = self._resolve(word & 31, False)
        rt = word & 31
        if guest in VIRTUAL:
            self._load_guest(T0, guest)
            rt = T0
        self.asm.tbz(rt, bit, self.goto_label(target), nz=nz)
        self.stats.inlined += 1
        self.stats.kinds["tbz"] += 1
        return True

    def _indirect_branch(self, offset: int, word: int, mnemonic: str) -> bool:
        asm = self.asm
        rn = (word >> 5) & 31
        self.fn_xu.add(offset)
        asm.word(A.add_imm(BUD, BUD, RUN_LIMIT + 1, sub=True))
        asm.tbz(BUD, 63, self.lab("XU", offset), nz=True)
        guest = self._resolve(rn, False)
        if guest in VIRTUAL:
            self._load_guest(T0, guest)
        else:
            asm.word(A.mov_reg(T0, rn))
        if mnemonic == "BLR":
            self._materialize(30, offset + 4)
        asm.b("IND")
        self.stats.inlined += 1
        self.stats.kinds[mnemonic.lower()] += 1
        return False

    def _pc_relative(self, offset: int, word: int, page: bool) -> bool:
        immlo = (word >> 29) & 3
        immhi = (word >> 5) & 0x7ffff
        imm = sext((immhi << 2) | immlo, 21)
        target = ((offset & ~0xfff) + (imm << 12)) if page else offset + imm
        rd = word & 31
        guest = self._resolve(rd, False)
        if guest == "zr":
            pass
        elif guest in VIRTUAL:
            self._materialize(T0, target & 0xffffffffffffffff)
            self._store_guest(T0, guest)
        else:
            self._materialize(rd, target & 0xffffffffffffffff)
        self.stats.inlined += 1
        self.stats.kinds["adrp" if page else "adr"] += 1
        return True

    # --- memory --------------------------------------------------------------
    def _memory(self, offset: int, word: int, encoding) -> bool:
        cls = encoding.decode_class
        asm = self.asm
        pair = cls in PAIR_CLASSES
        literal = None
        regoff = None
        wb_mode = "none"
        base = None
        imm = 0
        rt2 = 0
        if cls == "loadlit":
            opc, vector = word >> 30, (word >> 26) & 1
            rt = word & 31
            if not vector and opc == 3:
                return self._nop_prefetch(encoding)
            literal = offset + sext((word >> 5) & 0x7ffff, 19) * 4
            size, mem_opc = {(0, 0): (2, 1), (0, 1): (3, 1), (0, 2): (2, 2),
                             (1, 0): (2, 1), (1, 1): (3, 1),
                             (1, 2): (0, 3)}[(vector, opc)]
            n = 16 if (vector and opc == 2) else 1 << size
            load = True
        elif pair:
            opc, vector = word >> 30, (word >> 26) & 1
            load = bool((word >> 22) & 1)
            imm7 = sext((word >> 15) & 0x7f, 7)
            rt2, base, rt = (word >> 10) & 31, (word >> 5) & 31, word & 31
            if vector:
                if opc == 3:
                    return self._step(offset, "pair-reserved")
                n = {0: 4, 1: 8, 2: 16}[opc]
            else:
                if opc == 3:
                    return self._step(offset, "pair-reserved")
                n = {0: 4, 1: 4, 2: 8}[opc]
            imm = imm7 * n
            wb_mode = {"ldstpair_off": "none", "ldstnapair_offs": "none",
                       "ldstpair_pre": "pre", "ldstpair_post": "post"}[cls]
        else:
            size = word >> 30
            vector = (word >> 26) & 1
            mem_opc = (word >> 22) & 3
            base, rt = (word >> 5) & 31, word & 31
            if not vector and size == 3 and mem_opc == 2:
                return self._nop_prefetch(encoding)
            n = 16 if (vector and size == 0 and mem_opc >= 2) else 1 << size
            load = bool(mem_opc & 1) if vector else mem_opc != 0
            if cls == "ldst_pos":
                imm = ((word >> 10) & 0xfff) * n
            elif cls in ("ldst_unscaled", "ldst_immpre", "ldst_immpost"):
                imm = sext((word >> 12) & 0x1ff, 9)
                wb_mode = {"ldst_unscaled": "none", "ldst_immpre": "pre",
                           "ldst_immpost": "post"}[cls]
            else:  # ldst_regoff
                regoff = ((word >> 16) & 31, (word >> 13) & 7,
                          (word >> 12) & 1)
        total = 2 * n if pair else n
        base_g = None if literal is not None else self._resolve(base, True)
        if vector:
            operands = []
        else:
            operands = [self._resolve(rt, False)]
            if pair:
                operands.append(self._resolve(rt2, False))
        if pair and load and rt == rt2:
            # Unpredictable, and the engine rejects it, the zero register
            # twice and a vector register twice included.
            return self._step(offset, "pair-same-register")
        if wb_mode != "none" and any(o == base_g for o in operands):
            return self._step(offset, "writeback-overlap")
        base_virtual = base_g in VIRTUAL
        virtual_operands = [o for o in operands if o in VIRTUAL]
        if pair and base_virtual and virtual_operands and wb_mode != "none":
            return self._step(offset, "pair-virtual-writeback")
        if pair and len(virtual_operands) == 2 and virtual_operands[0] == \
                virtual_operands[1]:
            return self._step(offset, "pair-same-virtual")
        # A stack access with an immediate offset inside one aligned 16-byte
        # granule cannot cross a page (AOT code runs with a 16-byte aligned
        # SP: _sp_aligned_after): the page of its first byte is the page of
        # its last, so the probe needs no last-byte address.
        short = (base_g == "sp" and literal is None and regoff is None and
                 total > 1 and
                 (0 if wb_mode == "post" else imm) % 16 + total <= 16)
        # ---- the effective address, in a host register
        census_start = len(asm._w)
        if literal is not None:
            self._materialize(T0, literal)
            ea = T0
        elif regoff is not None:
            rm, option, s_bit = regoff
            if option not in (2, 3, 6, 7):
                return self._step(offset, "regoff-option")
            idx_g = self._resolve(rm, False)
            amount = (4 if n == 16 else size) if s_bit else 0
            if base_virtual:
                self._load_guest(T0, base_g)
                base_host = T0
            else:
                base_host = base
            if idx_g in VIRTUAL:
                self._load_guest(T1, idx_g)
                idx_host = T1
            else:
                idx_host = rm
            asm.word(A.add_ext(T0, base_host, idx_host, option, amount))
            ea = T0
        else:
            if base_virtual:
                self._load_guest(T0, base_g)
                host_base = T0
            else:
                host_base = base
            effective = 0 if wb_mode == "post" else imm
            if effective == 0:
                ea = host_base
            else:
                self._add_immediate(T0, host_base, effective)
                ea = T0
        # ---- the probe. The engine keys its entries by the address without
        # its top byte (TBI is on for every Android process, and the executor
        # enters AOT code only then) and strips it for every base but SP, so
        # the probe and the access use that address. An access that spans
        # bytes is probed by the page of its final byte: the direct index never
        # puts neighbouring pages in one entry (darwin/memory/tlb.hpp), so a
        # crossing is a miss. A miss restarts the instruction from its entry
        # after the host fills the entry, or steps it when the host cannot.
        self.tag("mem.ea", census_start)
        census_start = len(asm._w)
        if literal is None and base_g != "sp":
            asm.word(A.ubfx(T0, ea, 0, 56))
        entry = 0 if load else 16
        # A scalar of more than one byte is indexed by the page of its last
        # byte and its tag compared with the page of its first, which T0 keeps
        # for the access (register-offset form: the host delta of the entry
        # plus the guest address): neighbouring pages have different direct
        # slots, so an access that crosses a page finds the slot of the second
        # page holding another page and misses, and the probe needs no add on
        # T0 or the delta. A stack access in one granule needs no last byte at
        # all. A pair adds the delta itself, so it keeps the first form.
        flip = total > 1 and not pair and not short
        first = short or flip
        self.fn_mm.append((offset, "LOAD" if load else "STORE", total, first))
        if flip:
            asm.word(A.add_imm(T1, T0, total - 1))
            asm.word(A.ubfx(T2, T1, 12, 14))
        else:
            asm.word(A.ubfx(T2, T0, 12, 14))
        asm.word(A.add_reg(T2, TLB, T2, shift=0, amount=5))
        asm.word(A.ldp_x(T1, T2, T2, entry))
        if pair and not short:
            asm.word(A.add_reg(T2, T2, T0))
            asm.word(A.add_imm(T0, T0, total - 1))
        elif short and pair:
            asm.word(A.add_reg(T2, T2, T0))
        asm.word(A.eor_reg(T1, T1, T0, shift=1, amount=12))
        asm.cbz(T1, self.lab("MM", offset), sf=1, nz=True)
        self.tag("mem.probe", census_start)
        census_start = len(asm._w)
        # ---- the access, through the entry's host delta
        if pair:
            keep = word & ((3 << 30) | (1 << 26) | (1 << 22))
            hosts = []
            if vector:
                hosts = [(rt, None), (rt2, None)]
            else:
                scratch = [T0, T1]
                for guest, field in zip(operands, (rt, rt2)):
                    if guest in VIRTUAL:
                        hosts.append((scratch.pop(0), guest))
                    elif guest == "zr":
                        hosts.append((31, None))
                    else:
                        hosts.append((field, None))
                if not load:
                    for host, guest in hosts:
                        if guest is not None:
                            self._load_guest(host, guest)
            asm.word(keep | (0b101 << 27) | (0b010 << 23) |
                     (hosts[1][0] << 10) | (T2 << 5) | hosts[0][0])
            if load:
                for host, guest in hosts:
                    if guest is not None:
                        self._store_guest(host, guest)
        else:
            host_rt = rt
            guest_rt = None if vector else operands[0]
            if guest_rt in VIRTUAL:
                host_rt = T1
                if not load:
                    self._load_guest(T1, guest_rt)
            elif guest_rt == "zr":
                host_rt = 31
            asm.word((size << 30) | (0b111 << 27) | (vector << 26) |
                     (mem_opc << 22) | (1 << 21) | (T0 << 16) |
                     (0b011 << 13) | (0b10 << 10) | (T2 << 5) | host_rt)
            if guest_rt in VIRTUAL and load:
                self._store_guest(T1, guest_rt)
        self.tag("mem.access", census_start)
        census_start = len(asm._w)
        # ---- writeback of the base register, after the access succeeded
        if wb_mode != "none":
            if base_virtual:
                # T0 was changed by the probe; the base keeps its own tag.
                self._load_guest(T0, base_g)
                self._add_immediate(T0, T0, imm)
                self._store_guest(T0, base_g)
                if base_g == "sp" and imm % 16:
                    self._guard_sp(offset, T0)
            else:
                self._add_immediate(base, base, imm)
        self.tag("mem.wb", census_start)
        self.stats.inlined += 1
        self.stats.kinds["pair" if pair else "mem"] += 1
        return True

    def _simd_struct(self, offset: int, word: int, cls: str) -> bool:
        """LDn, STn, LDnR and the lane forms: the host executes the same
        instruction on the host address of the guest bytes, which the engine
        moves with one transfer of at most 64 bytes (it translates every page
        before it changes a register), so a probe of the last byte covers the
        whole access and a miss restarts the instruction untouched."""
        asm = self.asm
        load = bool((word >> 22) & 1)
        q = (word >> 30) & 1
        size = (word >> 10) & 3
        post = cls in ("asisdlsep", "asisdlsop")
        if cls in ("asisdlse", "asisdlsep"):
            opcode = (word >> 12) & 15
            registers = {0: 4, 2: 4, 4: 3, 6: 3, 7: 1, 8: 2, 10: 2
                         }.get(opcode)
            if registers is None or (opcode in (0, 4, 8) and size == 3 and
                                     not q):
                return self._step(offset, "simd-struct-reserved")
            total = registers * (16 if q else 8)
        else:
            opcode, replicate = (word >> 13) & 7, False
            scale = opcode >> 1
            selem = (((opcode & 1) << 1) | ((word >> 21) & 1)) + 1
            s_bit = (word >> 12) & 1
            if scale == 3:
                if not load or s_bit:
                    return self._step(offset, "simd-struct-reserved")
                scale = size
            elif scale == 1 and (size & 1):
                return self._step(offset, "simd-struct-reserved")
            elif scale == 2:
                if size & 2:
                    return self._step(offset, "simd-struct-reserved")
                if size & 1:
                    if s_bit:
                        return self._step(offset, "simd-struct-reserved")
                    scale = 3
            total = selem * (1 << scale)
        base = (word >> 5) & 31
        rm = (word >> 16) & 31
        if base == 31:
            return self._step(offset, "simd-struct-sp")
        index_g = self._resolve(rm, False) if post and rm != 31 else None
        base_virtual = base in VIRTUAL
        # The effective address is the base register.
        census_start = len(asm._w)
        if base_virtual:
            self._load_guest(T0, base)
            host_base = T0
        else:
            host_base = base
        self.tag("mem.ea", census_start)
        census_start = len(asm._w)
        asm.word(A.ubfx(T0, host_base, 0, 56))
        entry = 0 if load else 16
        self.fn_mm.append((offset, "LOAD" if load else "STORE", total, False))
        asm.word(A.ubfx(T2, T0, 12, 14))
        asm.word(A.add_reg(T2, TLB, T2, shift=0, amount=5))
        asm.word(A.ldp_x(T1, T2, T2, entry))
        asm.word(A.add_reg(T2, T2, T0))
        if total > 1:
            asm.word(A.add_imm(T0, T0, total - 1))
        asm.word(A.eor_reg(T1, T1, T0, shift=1, amount=12))
        asm.cbz(T1, self.lab("MM", offset), sf=1, nz=True)
        self.tag("mem.probe", census_start)
        census_start = len(asm._w)
        # The same instruction without its post-index, on the host address.
        asm.word((word & ~((1 << 23) | (31 << 16) | (31 << 5))) | (T2 << 5))
        self.tag("mem.access", census_start)
        census_start = len(asm._w)
        if post:
            if index_g is None:
                amount = None
            elif index_g in VIRTUAL:
                self._load_guest(T1, index_g)
                amount = T1
            else:
                amount = rm
            if base_virtual:
                self._load_guest(T0, base)
                if amount is None:
                    self._add_immediate(T0, T0, total)
                else:
                    asm.word(A.add_reg(T0, T0, amount))
                self._store_guest(T0, base)
            elif amount is None:
                self._add_immediate(base, base, total)
            else:
                asm.word(A.add_reg(base, base, amount))
        self.tag("mem.wb", census_start)
        self.stats.inlined += 1
        self.stats.kinds["simd-struct"] += 1
        return True

    # --- ordered accesses, atomics and FEAT_MOPS -------------------------------
    def _atomic_probe(self, offset: int, base_g, base_field: int, n: int,
                      write: bool) -> None:
        """Probe the page of a naturally aligned access of n bytes at the
        base register's address (a plain base, no offset). An unaligned
        address steps (the engine decides between the LSE2 completion and the
        alignment fault); a missing translation refills and restarts. On
        return T2 is the host address."""
        asm = self.asm
        if base_g in VIRTUAL:
            self._load_guest(T0, base_g)
            source = T0
        else:
            source = base_field
        asm.word(A.ubfx(T0, source, 0, 56))
        if n > 1:
            asm.word(A.ubfx(T1, T0, 0, n.bit_length() - 1))
            asm.cbz(T1, self.lab("ST", offset), sf=1, nz=True)
            self.fn_st.append(offset)
        asm.word(A.ubfx(T2, T0, 12, 14))
        asm.word(A.add_reg(T2, TLB, T2, shift=0, amount=5))
        asm.word(A.ldp_x(T1, T2, T2, 16 if write else 0))
        asm.word(A.add_reg(T2, T2, T0))
        if n > 1:
            asm.word(A.add_imm(T0, T0, n - 1))
        asm.word(A.eor_reg(T1, T1, T0, shift=1, amount=12))
        asm.cbz(T1, self.lab("MM", offset), sf=1, nz=True)
        self.fn_mm.append((offset, "STORE" if write else "LOAD", n, False))

    def _ordered(self, offset: int, word: int) -> bool:
        """LDAR and STLR of a byte, halfword, word or doubleword."""
        load = bool((word >> 22) & 1)
        if (word & ORDERED_MASK) != (LDAR_BASE if load else STLR_BASE):
            return self._step(offset, "ordered-other")
        n = 1 << (word >> 30)
        rn, rt = (word >> 5) & 31, word & 31
        base_g = self._resolve(rn, True)
        if base_g == "sp":
            return self._step(offset, "ordered-sp")
        rt_g = self._resolve(rt, False)
        self._atomic_probe(offset, base_g, rn, n, write=not load)
        host_rt = rt
        if rt_g in VIRTUAL:
            host_rt = T1
            if not load:
                self._load_guest(T1, rt_g)
        elif rt_g == "zr":
            host_rt = 31
        self.asm.word((word & ~0x3ff) | (T2 << 5) | host_rt)
        if load and rt_g in VIRTUAL:
            self._store_guest(T1, rt_g)
        self.stats.inlined += 1
        self.stats.kinds["ordered"] += 1
        return True

    def _atomic(self, offset: int, word: int) -> bool:
        """An LSE atomic memory operation or compare-and-swap, on the write
        translation of its address, run as the host's own instruction."""
        if (word & AMO_MASK) == AMO_BASE:
            if (word >> 15) & 1 and (word >> 12) & 7:
                return self._step(offset, "atomic-other")
            cas = False
        elif (word & CAS_MASK) == CAS_BASE:
            cas = True
        else:
            return self._step(offset, "atomic-other")
        n = 1 << (word >> 30)
        rs, rn, rt = (word >> 16) & 31, (word >> 5) & 31, word & 31
        base_g = self._resolve(rn, True)
        if base_g == "sp":
            return self._step(offset, "atomic-sp")
        rs_g, rt_g = self._resolve(rs, False), self._resolve(rt, False)
        self._atomic_probe(offset, base_g, rn, n, write=True)
        host_s, host_t = rs, rt
        if rs_g in VIRTUAL:
            host_s = T0
            self._load_guest(T0, rs_g)
        elif rs_g == "zr":
            host_s = 31
        if rt_g in VIRTUAL:
            if rt_g == rs_g:
                host_t = host_s
            else:
                host_t = T1
                if cas:
                    self._load_guest(T1, rt_g)
        elif rt_g == "zr":
            host_t = 31
        self.asm.word((word & ~((31 << 16) | (31 << 5) | 31)) |
                      (host_s << 16) | (T2 << 5) | host_t)
        if cas:
            # Rs receives the old value; Rt is only read.
            if rs_g in VIRTUAL:
                self._store_guest(T0, rs_g)
        elif rt_g in VIRTUAL:
            self._store_guest(host_t, rt_g)
        self.stats.inlined += 1
        self.stats.kinds["cas" if cas else "atomic"] += 1
        return True

    def _mops_epilogue(self, offset: int, kind: str, d: int, n: int, s: int
                       ) -> bool:
        """The stage that moves the data, which the virtual CPU does whole
        (option B). The site calls the thunk of its registers (emit_mops_thunks)
        with T0 = where to go on and T1 = its slot in the entry table; an
        operand held in a frame slot steps."""
        operands = [d, n] + ([] if kind == "set" and s == 31 else [s])
        if any(self._resolve(register, False) in VIRTUAL
               for register in operands):
            return self._step(offset, "mops-epilogue-virtual")
        self.mops_keys.add((kind, d, n, s))
        slot = offset - self.table_base
        asm = self.asm
        # adr T0 = the word after the branch below.
        asm.word(A.adr_word(T0, 16 if slot >> 16 else 12))
        asm.word(A.movz(T1, slot & 0xffff))
        if slot >> 16:
            asm.word(A.movk(T1, slot >> 16, 1))
        asm.b(self.mops_label((kind, d, n, s)))
        self.stats.inlined += 1
        self.stats.kinds["mops-epilogue"] += 1
        return True

    def _mops(self, offset: int, word: int) -> bool:
        """The prologue and main stages of CPY, CPYF and SET. The virtual CPU
        implements option B (darwin/cpu/arm64/detail/memory_copy_set.hpp): the
        prologue formats the registers and the flags, the main stage does
        nothing, and the epilogue, which moves the data, is the engine's."""
        family_set = (word & 0x3fe00c00) == 0x19c00400
        tag = (word & 0x3fe00c00) == 0x1dc00400
        forward = not family_set and not tag and \
            (word & 0x3f200c00) == 0x19000400
        general = not family_set and not tag and not forward and \
            (word & 0x3f200c00) == 0x1d000400
        if not (family_set or forward or general):
            return self._step(offset, "mops-other")
        d, n, s = word & 31, (word >> 5) & 31, (word >> 16) & 31
        stage = (word >> 14) & 3 if family_set else (word >> 22) & 3
        if (word >> 30) != 0 or stage == 3 or d == 31 or n == 31 or \
                d == n or s == d or s == n or (not family_set and s == 31):
            return self._step(offset, "mops-invalid")
        if stage == 2:
            return self._mops_epilogue(offset, "set" if family_set else
                                       "cpyf" if forward else "cpy", d, n, s)
        names = [("n", n), ("d", d), ("s", s)] if general else [("n", n)]
        virtual = [name for name, reg in names
                   if self._resolve(reg, False) in VIRTUAL]
        if len(virtual) > 1 and stage == 0:
            return self._step(offset, "mops-virtual")
        self.stats.inlined += 1
        self.stats.kinds["mops"] += 1
        if stage == 1:
            return True
        asm = self.asm
        host = {name: reg for name, reg in names}
        slot_of_virtual = None
        for name, reg in names:
            if name in virtual:
                self._load_guest(T1, self._resolve(reg, False))
                host[name] = T1
                slot_of_virtual = self._resolve(reg, False)
        count = host["n"]
        # remaining = min(count, the largest the stage allows).
        asm.word(A.orr_ones(T0, 31, 55 if general else 63))
        asm.word(A.cmp_reg(count, T0))
        asm.word(A.csel(count, count, T0, 9))          # LS
        if not general:
            asm.word(A.movz(T0, 0x2000, 1))
            asm.word(A.msr("nzcv", T0))
        else:
            source, dest = host["s"], host["d"]
            # Backward when 0 < to_low - from_low < remaining, with the low
            # 56 bits of each address and the subtraction in 64 bits: a
            # negative difference wraps above any clamped count.
            asm.word(A.and_ones(T0, source, 56))
            asm.word(A.and_ones(T2, dest, 56))
            asm.word(A.add_reg(T2, T2, T0, sub=True))
            asm.word(A.cmp_reg(T2, count))
            asm.word(A.ccmp_imm(T2, 0, 4, 3))           # LO
            asm.word(A.add_reg(T0, source, count))
            asm.word(A.csel(source, T0, source, 1))      # NE
            asm.word(A.add_reg(T0, dest, count))
            asm.word(A.csel(dest, T0, dest, 1))
            asm.word(A.movz(T0, 0x2000, 1))
            asm.word(A.movz(T2, 0xa000, 1))
            asm.word(A.csel(T0, T2, T0, 1))
            asm.word(A.msr("nzcv", T0))
        if slot_of_virtual is not None:
            self._store_guest(T1, slot_of_virtual)
        return True

    def _nop_prefetch(self, encoding) -> bool:
        self.stats.inlined += 1
        self.stats.kinds["prefetch"] += 1
        return True

    def _add_immediate(self, rd: int, rn: int, imm: int) -> None:
        asm = self.asm
        sub = imm < 0
        magnitude = -imm if sub else imm
        low, high = magnitude & 0xfff, (magnitude >> 12) & 0xfff
        if magnitude >> 24:
            raise A.AsmError("offset too large")
        source = rn
        if low or not high:
            asm.word(A.add_imm(rd, source, low, sub=sub))
            source = rd
        if high:
            asm.word(A.add_imm(rd, source, high, sub=sub, shift12=True))

    # --- cold stubs --------------------------------------------------------------
    def _emit_stubs(self, function: elf64.Function) -> None:
        """The function's out-of-line exits, right after its code."""
        asm = self.asm
        # Cross-page direct branches land here first (page_test adds the
        # branch-exit stubs they leave by).
        for target in sorted(self.fn_pg):
            asm.label(self.pg_label(target))
            before = len(asm._w)
            self.page_test(target)
            asm.b(self.direct_label(target))
            self.tag("gate", before)
            self.stats.words["gate"] += 1
        before = len(asm._w)
        for offset, kind, total, first in self.fn_mm:
            # The thunk gets the instruction's slot in the entry table in T1
            # and the access in the routine it enters.
            asm.label(self.lab("MM", offset))
            slot = offset - self.table_base
            asm.word(A.movz(T1, slot & 0xffff))
            chunk = slot >> 16
            self.refill_used.add((kind, total, chunk, first))
            asm.b(f"REFILL_{kind}_{total}" + ("F" if first else "") +
                  (f"_{chunk}" if chunk else ""))
        self.tag("stub.mm", before)
        before = len(asm._w)
        for offset in self.fn_st:
            asm.label(self.lab("ST", offset))
            self._exit_to(offset, "EXIT_STEP")
        self.tag("stub.st", before)
        before = len(asm._w)
        for offset in sorted(self.fn_xu):
            asm.label(self.lab("XU", offset))
            self._exit_to(offset, "EXIT_BUDGET")
        self.tag("stub.xu", before)
        before = len(asm._w)
        for offset in self.fn_ma:
            asm.label(self.lab("MA", offset))
            self._exit_to(offset + 4, "EXIT_BRANCH")
        self.tag("stub.ma", before)
        before = len(asm._w)
        for target in sorted(self.fn_xb):
            asm.label(self.xb_label(target))
            self.xb_emitted[target] = (self.xb_label(target), len(asm._w))
            self._exit_to(target, "EXIT_BRANCH")
        self.tag("stub.xb", before)


# ---- the entry and exit routines -----------------------------------------

def _entry16_jump(asm: A.Assembler, library: "Library", off: int, table: int,
                  page: int, none: str | None = None) -> None:
    """Branch to the native code of the source word at byte offset `off`
    (register `off`, clobbered) through the 16-bit entry table; `table` and
    `page` are scratch. With `none`, a word without an entry branches
    there (with the registers clobbered); without, it is a word the caller
    knows has one."""
    len16 = library.table_len16()
    if len16 >> 12 >= 4096:
        raise EntryTableRefused(f"{library.name}: the entry table of "
                                f"{len16} bytes is too large for one add")
    asm.adrp_add(table, "TAB")
    asm.word(A.lsr_imm(page, off, 12))
    asm.word(A.lsr_imm(off, off, 2))
    asm.word(A.ldrh_reg(off, table, off))
    if none is not None:
        asm.cbz(off, none, sf=0, nz=False)
    asm.word(A.add_imm(table, table, len16 >> 12, shift12=True))
    asm.word(A.ldrsw_reg(page, table, page))
    asm.word(A.add_reg(page, page, off, shift=0, amount=2))
    asm.word(A.add_reg(page, page, table))
    asm.word(A.br(page))


def emit_enter(asm: A.Assembler, layout: dict[str, int],
               count_retired: bool = False) -> None:
    L = layout
    cpu_regs = L["UML_GADGET_CPU_X0"]
    v0 = L["UML_GADGET_CPU_V0"]
    asm.label("ENTER")
    asm.word(A.add_imm(31, 31, FRAME, sub=True))
    asm.word(A.stp_x(29, 30, 31, 0))
    for index, pair in enumerate(((19, 20), (21, 22), (23, 24), (25, 26),
                                  (27, 28))):
        asm.word(A.stp_x(pair[0], pair[1], 31, 16 + 16 * index))
    for index in range(4):
        asm.word(A.ldp_d(8 + 2 * index, 9 + 2 * index, 31, 0x60 + 16 * index,
                         load=False))
    asm.word(A.str_x(0, 31, SLOT_CPU))
    asm.word(A.str_x(2, 31, SLOT_BIAS))
    asm.word(A.str_x(4, 31, SLOT_BUD0))
    asm.word(A.str_x(5, 31, SLOT_XTAB))
    asm.word(A.str_x(6, 31, SLOT_PSTATE))
    if count_retired:
        asm.word(A.str_x(31, 31, SLOT_COUNT))
    asm.word(A.mov_reg(TLB, 1))
    asm.word(A.mov_reg(BUD, 4))
    asm.word(A.mov_reg(T0, 3))
    asm.word(A.mov_reg(16, 0))
    # activate the guest floating-point environment when it is not live
    fp_active = L["UML_GADGET_CPU_FP_ACTIVE"]
    asm.word(A.ldrb_w(9, 16, fp_active))
    asm.cbz(9, "ENTER_FP", sf=0, nz=False)
    asm.b("ENTER_FP_DONE")
    asm.label("ENTER_FP")
    asm.word(A.mrs(10, "fpcr"))
    asm.word(A.mrs(11, "fpsr"))
    asm.word(A.str_x(10, 16, L["UML_GADGET_CPU_HOST_FPCR"]))
    asm.word(A.str_x(11, 16, L["UML_GADGET_CPU_HOST_FPSR"]))
    asm.word(A.movz(9, 1, sf=0))
    asm.word(A.strb_w(9, 16, fp_active))
    asm.word(A.ldr_w(9, 16, L["UML_GADGET_CPU_FPCR"]))
    asm.word(A.movz(10, L["UML_ARM64_FPCR_TRAP_ENABLES"], sf=0))
    asm.word(0x2a0a0129)  # orr w9, w9, w10
    asm.word(0x4a0a0129)  # eor w9, w9, w10
    asm.word(A.msr("fpcr", 9))
    asm.word(A.ldr_w(9, 16, L["UML_GADGET_CPU_FPSR"]))
    asm.word(A.msr("fpsr", 9))
    asm.label("ENTER_FP_DONE")
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, 16, v0 + 32 * index))
    asm.word(A.ldr_w(9, 16, L["UML_GADGET_CPU_NZCV"]))
    asm.word(A.msr("nzcv", 9))
    for guest, slot in VSLOT.items():
        asm.word(A.ldr_x(9, 16, cpu_regs + 8 * guest))
        asm.word(A.str_x(9, 31, slot))
    asm.word(A.ldr_x(9, 16, L["UML_GADGET_CPU_SP"]))
    asm.word(A.str_x(9, 31, SLOT_SP))
    skip = {16, 18, 24, 25, 26, 27, 28}
    index = 0
    while index <= 30:
        if index in skip:
            index += 1
            continue
        if index + 1 <= 30 and (index + 1) not in skip and index % 1 == 0:
            asm.word(A.ldp_x(index, index + 1, 16, cpu_regs + 8 * index))
            index += 2
        else:
            asm.word(A.ldr_x(index, 16, cpu_regs + 8 * index))
            index += 1
    asm.word(A.ldr_x(16, 16, cpu_regs + 8 * 16))
    asm.word(A.br(T0))


def emit_save_state(asm: A.Assembler) -> None:
    """Push the REFILL_FRAME that holds what a C++ call may change: x0-x17,
    x30, NZCV and q0-q31. Clobbers x9 after saving it."""
    asm.word(A.add_imm(31, 31, REFILL_FRAME, sub=True))
    for index in range(0, 18, 2):
        asm.word(A.stp_x(index, index + 1, 31, 8 * index))
    asm.word(A.str_x(30, 31, 0x90))
    asm.word(A.mrs(9, "nzcv"))
    asm.word(A.str_x(9, 31, 0x98))
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, 31, 0xa0 + 32 * index,
                         load=False))


def emit_restore_state(asm: A.Assembler) -> None:
    """Pop what emit_save_state pushed."""
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, 31, 0xa0 + 32 * index))
    asm.word(A.ldr_x(9, 31, 0x98))
    asm.word(A.msr("nzcv", 9))
    asm.word(A.ldr_x(30, 31, 0x90))
    for index in range(0, 18, 2):
        asm.word(A.ldp_x(index, index + 1, 31, 8 * index))
    asm.word(A.add_imm(31, 31, REFILL_FRAME))


def emit_exits(asm: A.Assembler, library: "Library") -> None:
    """The exit routines and the refill thunk, once in every unit so that no
    branch needs more than a unit's reach."""
    L = library.layout
    cpu_regs = L["UML_GADGET_CPU_X0"]
    v0 = L["UML_GADGET_CPU_V0"]
    for name, reason in (("EXIT_BUDGET", EXIT_BUDGET),
                         ("EXIT_BRANCH", EXIT_BRANCH),
                         ("EXIT_STEP", EXIT_STEP),
                         ("EXIT_BRANCH_ABS", EXIT_BRANCH | ABSOLUTE)):
        asm.label(name)
        asm.word(A.movz(T1, reason))
        asm.b("EXIT_COMMON")
    asm.label("EXIT_COMMON")
    asm.word(A.ldr_x(T2, 31, SLOT_CPU))
    index = 0
    while index <= 30:
        if index in (18, 24, 25, 26, 27, 28):
            index += 1
            continue
        if index + 1 <= 30 and (index + 1) not in (18, 24, 25, 26, 27, 28):
            asm.word(A.stp_x(index, index + 1, T2, cpu_regs + 8 * index))
            index += 2
        else:
            asm.word(A.str_x(index, T2, cpu_regs + 8 * index))
            index += 1
    for guest, slot in VSLOT.items():
        asm.word(A.ldr_x(TLB, 31, slot))
        asm.word(A.str_x(TLB, T2, cpu_regs + 8 * guest))
    asm.word(A.ldr_x(TLB, 31, SLOT_SP))
    asm.word(A.str_x(TLB, T2, L["UML_GADGET_CPU_SP"]))
    asm.word(A.mrs(TLB, "nzcv"))
    asm.word(A.str_w(TLB, T2, L["UML_GADGET_CPU_NZCV"]))
    asm.word(A.ldr_x(TLB, 31, SLOT_BIAS))
    asm.tbz(T1, 8, "EXIT_REL", nz=False)
    asm.b("EXIT_PC")
    asm.label("EXIT_REL")
    asm.word(A.add_reg(T0, T0, TLB))
    asm.label("EXIT_PC")
    asm.word(A.str_x(T0, T2, L["UML_GADGET_CPU_PC"]))
    for index in range(16):
        asm.word(A.ldp_q(2 * index, 2 * index + 1, T2, v0 + 32 * index,
                         load=False))
    asm.word(A.ubfx(0, T1, 0, 8))
    if library.count_retired:
        asm.word(A.ldr_x(1, 31, SLOT_COUNT))
    else:
        # The budget the entry used up. Charge points pay for the run that
        # follows them in advance, so this is not a count of instructions
        # (one guest RET reports 33); a counting build is the exact count.
        asm.word(A.ldr_x(TLB, 31, SLOT_BUD0))
        asm.word(A.add_reg(1, TLB, BUD, sub=True))
    for index in range(4):
        asm.word(A.ldp_d(8 + 2 * index, 9 + 2 * index, 31, 0x60 + 16 * index))
    asm.word(A.ldp_x(29, 30, 31, 0))
    for index, pair in enumerate(((19, 20), (21, 22), (23, 24), (25, 26),
                                  (27, 28))):
        asm.word(A.ldp_x(pair[0], pair[1], 31, 16 + 16 * index))
    asm.word(A.add_imm(31, 31, FRAME))
    asm.word(A.ret_())

    # The host-call thunks. Both save every register a C++ call may change,
    # call through a pointer in the table that ENTER's last argument names,
    # restore, and branch.
    #
    # State the C++ call may change and the thunks therefore save: x0-x17,
    # x30, NZCV and q0-q31 (the caller-saved set of the host ABI). They keep
    # nothing else: x19-x28 (BUD, TLB, T0-T2) and d8-d15 are callee-saved,
    # host x18 is never used, FPCR/FPSR belong to the guest while an entry is
    # live and the callees do no floating point, and nothing below sp is
    # touched.
    rf = REFILL_FRAME

    def save() -> None:
        emit_save_state(asm)

    def restore() -> None:
        emit_restore_state(asm)

    asm.label("REFILL")
    save()
    asm.word(A.ldr_x(9, 31, rf + SLOT_XTAB))
    asm.word(A.ldr_x(9, 9, 0))
    asm.word(A.mov_reg(0, TLB))
    asm.word(A.mov_reg(1, T0))
    asm.word(A.mov_reg(2, T2))
    asm.word(A.blr(9))
    asm.word(A.mov_reg(T2, 0))
    restore()
    asm.cbz(T2, "REFILL_FAIL", sf=1, nz=False)
    # T1 = the slot's byte offset; T0 and T2 are free here, the instruction
    # recomputes what it needs from its entry.
    _entry16_jump(asm, library, T1, T0, T2)
    asm.label("REFILL_FAIL")
    base_pages = library.table_base >> 12
    if library.table_base & 0xfff or library.table_base >= 1 << 32:
        raise A.AsmError("a library's code must start page-aligned below "
                         "4 GiB")
    if base_pages < 4096:
        asm.word(A.add_imm(T0, T1, base_pages, shift12=True))
    else:
        asm.word(A.movz(T0, library.table_base & 0xffff))
        asm.word(A.movk(T0, library.table_base >> 16, 1))
        asm.word(A.add_reg(T0, T0, T1))
    asm.b("EXIT_STEP")

    # An indirect branch: T0 = the guest target, the budget already charged.
    # The target is inside this library when it lies in the code span.
    asm.label("IND")
    asm.word(A.ldr_x(T1, 31, SLOT_BIAS))
    asm.word(A.add_reg(T1, T0, T1, sub=True))  # vaddr
    base_pages = library.table_base >> 12
    if 0 < base_pages < 4096:
        asm.word(A.add_imm(T1, T1, base_pages, sub=True, shift12=True))
    elif base_pages:
        asm.word(A.movz(T2, library.table_base & 0xffff))
        asm.word(A.movk(T2, library.table_base >> 16, 1))
        asm.word(A.add_reg(T1, T1, T2, sub=True))
    asm.word(A.ror_imm(T2, T1, 2))
    asm.word(A.lsr_imm(T2, T2, library.log_span - 2))
    asm.cbz(T2, "XLIB", nz=True)
    # The test above passes anything below the next power of two; the
    # table covers exactly the code span.
    span = library.table_end - library.table_base
    if (span >> 12) < 4096 and span % 4096 == 0:
        asm.word(A.add_imm(T2, T1, span >> 12, sub=True, shift12=True))
    else:
        asm.word(A.movz(T2, span & 0xffff))
        asm.word(A.movk(T2, (span >> 16) & 0xffff, 1))
        asm.word(A.add_reg(T2, T1, T2, sub=True))
    asm.tbz(T2, 63, "XLIB", nz=False)
    # The target's text page must have matched its stored hash.
    asm.word(A.ldr_x(T2, 31, SLOT_PSTATE))
    asm.word(A.add_reg(T2, T2, T1, shift=1, amount=12))
    asm.word(A.ldrb_w(T2, T2, 0))
    asm.cbz(T2, "EXIT_BRANCH_ABS", sf=0, nz=False)
    # T0, the guest target, is the register the lookup needs most: it waits
    # in the frame's link slot (free at an indirect branch).
    asm.word(A.str_x(T0, 31, SLOT_LR))
    _entry16_jump(asm, library, T1, T0, T2, none="IND_NOENTRY")
    asm.label("IND_NOENTRY")
    asm.word(A.ldr_x(T0, 31, SLOT_LR))
    asm.b("EXIT_BRANCH_ABS")

    # An indirect branch to an address outside this library: T0 = the target.
    # When the host knows a verified library instance that holds it, the
    # branch goes on in that instance's code (and its load bias); otherwise
    # the entry ends at the branch and the engine takes it.
    asm.label("XLIB")
    save()
    asm.word(A.ldr_x(0, 31, rf + SLOT_XTAB))
    asm.word(A.ldr_x(9, 0, 8))
    asm.word(A.mov_reg(1, T0))
    asm.word(A.blr(9))
    asm.word(A.mov_reg(T2, 0))
    asm.word(A.mov_reg(T1, 1))
    restore()
    asm.cbz(T2, "EXIT_BRANCH_ABS", sf=1, nz=False)
    # T1 = the CrossTable entry of the instance the target is in.
    asm.word(A.ldr_x(T0, T1, CROSS_BIAS))
    asm.word(A.str_x(T0, 31, SLOT_BIAS))
    asm.word(A.ldr_x(T0, T1, CROSS_PAGES))
    asm.word(A.str_x(T0, 31, SLOT_PSTATE))
    asm.word(A.br(T2))


def emit_refill_stubs(asm: A.Assembler, library: "Library") -> None:
    """The routines a cold miss stub enters, after the functions that name
    them: REFILL_<kind>_<bytes> with T0 = the address the probe formed (an
    access of more than one byte leaves the address of its last byte; the
    entry REFILL_<kind>_<bytes>F is for a probe that left the first) and
    T1 = the instruction's slot in this library's entry table. The thunk asks
    the host to fill the TLB entry; when that succeeds the instruction
    restarts from its entry (no state of the instruction was changed before
    the probe), and when it cannot, or the access crosses a page, the
    instruction steps. Only the routines a stub of this library enters
    exist."""
    for kind, access in (("LOAD", 0), ("STORE", 1)):
        for total in REFILL_TOTALS:
            chunks = {first: sorted(chunk for used_kind, used_total, chunk,
                                    used_first in library.refill_used
                                    if (used_kind, used_total, used_first) ==
                                    (kind, total, first))
                      for first in (False, True)}
            if not chunks[False] and not chunks[True]:
                continue
            if chunks[True]:
                # A probe that found no last-byte address (an access in one
                # aligned granule) leaves the address of the first byte; the
                # host is given the last, as every other probe leaves it.
                asm.label(f"REFILL_{kind}_{total}F")
                asm.word(A.add_imm(T0, T0, total - 1))
            asm.label(f"REFILL_{kind}_{total}")
            asm.word(A.movz(T2, access | (total << 8)))
            asm.b("REFILL")
            # A site names its slot in 16 bits; the thunk of a later 64 KiB
            # of the table supplies the rest.
            for first in (False, True):
                suffix = "F" if first else ""
                for chunk in chunks[first]:
                    if chunk == 0:
                        continue
                    asm.label(f"REFILL_{kind}_{total}{suffix}_{chunk}")
                    asm.word(A.movk(T1, chunk, 1))
                    asm.b(f"REFILL_{kind}_{total}{suffix}")


def emit_mops_thunks(asm: A.Assembler, library: "Library") -> None:
    """One thunk per register combination of a MOPS epilogue in the library.

    The virtual CPU implements option B (darwin/cpu/arm64/detail/
    memory_copy_set.hpp): the prologue formatted the registers and the flags,
    and the epilogue moves everything that remains, a page-contained region at
    a time, committing the registers between regions. The thunk does the
    common case itself, with no call and no change to the flags, and leaves
    the rest to the host routine that runs that very function:

    * the count is zero: nothing happens;
    * the flags are not the prologue's (C clear), the copy runs backward, or
      the count is above 64: the host routine;
    * the source (a copy) and the destination each lie in one page, both
      pages have their TLB entries (the key is the untagged page, as the
      engine looks it up): a copy of n <= 64 bytes is one region, which the
      engine moves with one memmove. Every byte is loaded before the first
      is stored, so the result is the memmove's for any overlap, and a set
      is one memset;
    * anything else, a page crossing or a missing entry included, goes to
      the host routine, which translates (filling the TLB) and moves the
      regions exactly as the engine does. When it completes the thunk goes
      on; when it stops at a fault or at registers the prologue cannot have
      formatted, the registers it committed are the engine's at that region
      and the instruction steps, so the engine redoes the rest and takes the
      fault with its own state.

    The site has set T0 to the word after it and T1 to its slot in the entry
    table. The thunk writes no flag; it saves the registers it uses.
    """
    rf = REFILL_FRAME
    for key in sorted(library.mops_keys):
        kind, d, n, s = key
        copy = kind != "set"
        has_source = copy or s != 31
        name = library.mops_label(key)
        a, b, c, e = [r for r in range(9, 18) if r not in (d, n, s)][:4]
        done, ret, slow, slow2, fail = (name + "_done", name + "_ret",
                                        name + "_slow", name + "_slow2",
                                        name + "_fail")

        def locate(address: int, pointer: int, entry: int) -> None:
            """pointer = the host address of the n bytes at the guest address
            in `address`, or leave by slow2."""
            asm.word(A.ubfx(pointer, address, 0, 56))
            asm.word(A.and_ones(T2, pointer, 12))
            asm.word(A.add_reg(T2, T2, n))
            asm.word(A.add_imm(T2, T2, 1, sub=True))
            asm.word(A.lsr_imm(T2, T2, 12))
            asm.cbz(T2, slow2, nz=True)
            asm.word(A.ubfx(c, pointer, 12, 14))
            asm.word(A.add_reg(c, TLB, c, shift=0, amount=5))
            asm.word(A.ldp_x(e, c, c, entry))
            asm.word(A.eor_reg(e, e, pointer, shift=1, amount=12))
            asm.cbz(e, slow2, nz=True)
            asm.word(A.add_reg(pointer, c, pointer))

        asm.label(name)
        asm.cbz(n, ret, sf=1)
        asm.bcond(COND_LO, slow)
        if kind == "cpy":
            asm.bcond(COND_MI, slow)
        asm.word(A.add_imm(T2, n, 1, sub=True))
        asm.word(A.lsr_imm(T2, T2, 6))
        asm.cbz(T2, slow, nz=True)
        asm.word(A.add_imm(31, 31, 32, sub=True))
        asm.word(A.stp_x(a, b, 31, 0))
        asm.word(A.stp_x(c, e, 31, 16))
        if copy:
            locate(s, a, 0)
        locate(d, b, 16)
        if not copy:
            # The fill byte in every byte of e.
            if s == 31:
                asm.word(A.movz(e, 0))
            else:
                asm.word(A.and_ones(e, s, 8))
                for amount in (8, 16, 32):
                    asm.word(A.orr_reg(e, e, e, shift=0, amount=amount))
        asm.word(A.add_imm(c, n, 1, sub=True))
        for bit, size in ((5, 33), (4, 17), (3, 9), (2, 5), (1, 3)):
            asm.tbz(c, bit, f"{name}_n{size}", nz=True)
        # n = 1 or 2, 3 or 4, 5 to 8 and 9 to 16: the first and the last
        # element of the size, every load before the first store.
        for label, log in ((f"{name}_n1", 0), (f"{name}_n3", 1),
                           (f"{name}_n5", 2), (f"{name}_n9", 3)):
            if label != f"{name}_n1":
                asm.label(label)
            size = 1 << log
            asm.word(A.add_reg(T2, b, n))
            if copy:
                asm.word(A.ldst_imm(e, a, 0, log, load=True))
                asm.word(A.add_reg(c, a, n))
                asm.word(A.ldst_unscaled(c, c, -size, log, load=True))
                asm.word(A.ldst_imm(e, b, 0, log, load=False))
                asm.word(A.ldst_unscaled(c, T2, -size, log, load=False))
            else:
                asm.word(A.ldst_imm(e, b, 0, log, load=False))
                asm.word(A.ldst_unscaled(e, T2, -size, log, load=False))
            asm.b(done)
        # 17 to 32 and 33 to 64 bytes.
        asm.label(f"{name}_n17")
        asm.word(A.add_reg(T2, b, n))
        if copy:
            asm.word(A.add_imm(31, 31, 32, sub=True))
            asm.word(A.stp_q(30, 31, 31, 0))
            asm.word(A.ldr_q(30, a, 0))
            asm.word(A.add_reg(c, a, n))
            asm.word(A.ldst_unscaled(31, c, -16, 0, load=True, vector=True))
            asm.word(A.str_q(30, b, 0))
            asm.word(A.ldst_unscaled(31, T2, -16, 0, load=False, vector=True))
            asm.word(A.ldp_q(30, 31, 31, 0))
            asm.word(A.add_imm(31, 31, 32))
        else:
            asm.word(A.stp_x(e, e, b, 0))
            asm.word(A.stp_x(e, e, T2, -16))
        asm.b(done)
        asm.label(f"{name}_n33")
        asm.word(A.add_reg(T2, b, n))
        if copy:
            asm.word(A.add_imm(31, 31, 64, sub=True))
            asm.word(A.stp_q(28, 29, 31, 0))
            asm.word(A.stp_q(30, 31, 31, 32))
            asm.word(A.ldp_q(28, 29, a, 0))
            asm.word(A.add_reg(c, a, n))
            asm.word(A.ldp_q(30, 31, c, -32))
            asm.word(A.stp_q(28, 29, b, 0))
            asm.word(A.stp_q(30, 31, T2, -32))
            asm.word(A.ldp_q(28, 29, 31, 0))
            asm.word(A.ldp_q(30, 31, 31, 32))
            asm.word(A.add_imm(31, 31, 64))
        else:
            asm.word(A.stp_x(e, e, b, 0))
            asm.word(A.stp_x(e, e, b, 16))
            asm.word(A.stp_x(e, e, T2, -32))
            asm.word(A.stp_x(e, e, T2, -16))
        # Commit the registers the way the engine does at the end of the
        # last region, and go on.
        asm.label(done)
        asm.word(A.add_reg(d, d, n))
        if copy:
            asm.word(A.add_reg(s, s, n))
        asm.word(A.movz(n, 0))
        asm.word(A.ldp_x(a, b, 31, 0))
        asm.word(A.ldp_x(c, e, 31, 16))
        asm.word(A.add_imm(31, 31, 32))
        asm.label(ret)
        asm.word(A.br(T0))
        # The fast path gave up after saving its registers.
        asm.label(slow2)
        asm.word(A.ldp_x(a, b, 31, 0))
        asm.word(A.ldp_x(c, e, 31, 16))
        asm.word(A.add_imm(31, 31, 32))
        # The host routine: it takes the registers (and the flags) in a block
        # on the stack and writes the registers back.
        asm.label(slow)
        asm.word(A.add_imm(31, 31, 32, sub=True))
        asm.word(A.stp_x(d, n, 31, 0))
        asm.word(A.str_x(s, 31, 16))
        emit_save_state(asm)
        asm.word(A.ldr_x(9, 31, 0x98))
        asm.word(A.str_x(9, 31, rf + 24))
        asm.word(A.ldr_x(0, 31, rf + 32 + SLOT_CPU))
        asm.word(A.mov_reg(1, TLB))
        asm.word(A.add_imm(2, 31, rf))
        asm.word(A.movz(3, MOPS_KIND[kind]))
        asm.word(A.ldr_x(9, 31, rf + 32 + SLOT_XTAB))
        asm.word(A.ldr_x(9, 9, XTAB_MOPS))
        asm.word(A.blr(9))
        asm.word(A.mov_reg(T2, 0))
        emit_restore_state(asm)
        asm.word(A.ldp_x(d, n, 31, 0))
        if has_source:
            asm.word(A.ldr_x(s, 31, 16))
        asm.word(A.add_imm(31, 31, 32))
        # 1 = done; 0 = a fault and 2 = registers the prologue cannot have
        # formatted: the instruction steps (T1 = its slot).
        asm.word(A.add_imm(T2, T2, 1, sub=True))
        asm.cbz(T2, fail, nz=True)
        asm.word(A.br(T0))
        asm.label(fail)
        asm.b("REFILL_FAIL")


# ---- the blob ----------------------------------------------------------------

def resolve_library(root: pathlib.Path, mounts: dict[str, pathlib.Path],
                    path: str) -> pathlib.Path:
    """A guest path to its file: the longest mount prefix, else the root."""
    best = ""
    for prefix in mounts:
        if (path == prefix or path.startswith(prefix + "/")) and \
                len(prefix) > len(best):
            best = prefix
    if best:
        return mounts[best] / path[len(best):].lstrip("/")
    return root / path.lstrip("/")


# The bound the guest's own path walk has (Linux: 40 links, parent
# directories included).
LINK_LIMIT = 40


def resolve_in_image(root: pathlib.Path, mounts: dict[str, pathlib.Path],
                     path: str) -> str | None:
    """The guest path `path` names once every link on the way is followed as
    the guest's own file system would: an absolute link from the image's
    root, a relative one from its directory, `..` stopping at the root. Every
    component is looked at, the last one and each directory before it, and the
    result has no link in it. A link is read from the unpacked image only, so
    one that points outside the image names a path inside it that is not
    there. None when the chain of links never ends (LINK_LIMIT)."""
    pending = [part for part in path.split("/") if part][::-1]
    done: list[str] = []
    followed = 0
    while pending:
        part = pending.pop()
        if part == ".":
            continue
        if part == "..":
            if done:
                done.pop()
            continue
        here = "/" + "/".join(done + [part])
        # A mount's own directory is the build's, not the image's.
        host = None if here in mounts else resolve_library(root, mounts, here)
        if host is not None and host.is_symlink():
            followed += 1
            if followed > LINK_LIMIT:
                return None
            link = os.readlink(host)
            if link.startswith("/"):
                done = []
            pending.extend([name for name in link.split("/") if name][::-1])
            continue
        done.append(part)
    return "/" + "/".join(done)


def follow_in_image(root: pathlib.Path, mounts: dict[str, pathlib.Path],
                    path: str) -> str | None:
    """The guest path of the regular file `path` names inside the image (see
    resolve_in_image), or None for a name that is no regular file there."""
    resolved = resolve_in_image(root, mounts, path)
    if resolved is not None and \
            resolve_library(root, mounts, resolved).is_file():
        return resolved
    return None


def expand_libraries(root: pathlib.Path, mounts: dict[str, pathlib.Path],
                     entries: list[str]) -> tuple[list[str], list[str]]:
    """The libraries an entry list names, and the files a pattern matched
    that have no executable code or that no regular file of the image
    backs. An entry whose last component has a `*` names every file of that
    directory the pattern matches, in name order (the boot image's OAT
    files: some hold only a header, because their jar has no compiled
    method); one without names that file, which must be a regular file of
    the image. Every component of a path, the directories before the last
    one as well, is resolved by the guest's rules (resolve_in_image) before
    the host is asked anything, so a link never reaches a file outside the
    image: an explicit entry that is not a regular file there is refused and
    a match that is not is listed in `skipped`. A link is the file it names
    inside the image. A path named twice is refused: each of its page digests
    would then be shared by two units and leave the digest hash, so the
    library would never bind."""
    libraries: list[str] = []
    skipped: list[str] = []
    for entry in entries:
        directory, _, pattern = entry.rpartition("/")
        if "*" in directory:
            raise SystemExit(f"rewrite.py: {entry}: only the last component "
                             "of a library path may hold a pattern")
        if "*" not in pattern:
            guest = follow_in_image(root, mounts, entry)
            if guest is None:
                raise SystemExit(f"rewrite.py: {entry} is not a regular file "
                                 "of the image")
            libraries.append(guest)
            continue
        where = resolve_in_image(root, mounts, directory)
        folder = None if where is None else \
            resolve_library(root, mounts, where)
        names = [] if folder is None or not folder.is_dir() else sorted(
            path.name for path in folder.glob(pattern)
            if path.is_symlink() or path.is_file())
        if not names:
            raise SystemExit(f"rewrite.py: {entry} matches no file")
        for name in names:
            named = f"{directory}/{name}"
            guest = follow_in_image(root, mounts, f"{where}/{name}")
            if guest is None:
                skipped.append(named)
                continue
            elf = elf64.Elf(resolve_library(root, mounts, guest).read_bytes())
            (libraries if elf.has_executable_segment() else skipped).append(
                guest)
    seen: set[str] = set()
    for path in libraries:
        if path in seen:
            raise SystemExit(f"rewrite.py: {path} is listed twice")
        seen.add(path)
    return libraries, skipped


def build_unit(job: tuple) -> dict:
    """One library to its self-contained unit (runs in a worker process)."""
    index, name, file_path, count_retired = job[:4]
    decoder = a64decode.Decoder(a64decode.DEFAULT_TABLE)
    layout = read_layout(LAYOUT_HEADER)
    data = pathlib.Path(file_path).read_bytes()
    elf = elf64.Elf(data)
    library = Library(index, name, elf, decoder, layout, count_retired)
    segment = library.segment
    digests = []
    pages = []
    for page_vaddr in range(library.table_base, library.table_end, PAGE):
        file_offset = segment.offset + (page_vaddr - segment.vaddr)
        page = bytes(library.elf.data[file_offset:file_offset + PAGE])
        page += bytes(PAGE - len(page))
        digests.append(page_digest(page))
        pages.append(page)
    try:
        unit = library.build(digests, b"".join(pages))
    except EntryTableRefused as refusal:
        return {"index": index, "path": name, "refused": str(refusal)}
    unit.update({
        "index": index,
        "path": name,
        "sha256": library.elf.sha256,
        "build_id": library.elf.build_id,
        "digests": digests,
        "table_base": library.table_base,
        "table_end": library.table_end,
        "table_words": library.table_words,
        "text_end": segment.vaddr + segment.filesz,
        "vaddr": segment.vaddr,
        "filesz": segment.filesz,
        "functions": [(f.start, f.end) for f in library.functions],
        "step_list": library.step_list,
        "sites": library.sites,
        "host_path": file_path,
        "stats": {"functions": library.stats.functions,
                  "instructions": library.stats.instructions,
                  "inlined": library.stats.inlined,
                  "steps": library.stats.steps,
                  "steps_by_cause": dict(library.stats.steps_by_cause),
                  "inlined_by_kind": dict(library.stats.kinds),
                  "words": dict(library.stats.words),
                  "census": library.stats.census},
    })
    return unit


def build_blob(units: list[dict], layout: dict[str, int],
               release_id: str, count_retired: bool = False,
               refused: list[dict] | None = None, *, manifest_sha256: str,
               system_sha256: str) -> tuple[bytes, dict]:
    """Place the units after the header and ENTER; add the digest hash.
    The header names the release the libraries were read from: its ID, the
    SHA-256 of its manifest and that of its system partition (hex). `refused`
    names the libraries left to the engine; the header carries their count
    for the runtime's diagnostics."""
    for digest in (manifest_sha256, system_sha256):
        if len(bytes.fromhex(digest)) != 32:
            raise ValueError("a release digest is not a SHA-256")
    refused = refused or []
    units = sorted(units, key=lambda unit: unit["index"])
    enter_asm = A.Assembler()
    emit_enter(enter_asm, layout, count_retired)
    enter_code = enter_asm.resolve()
    header_size = HEADER_SIZE + LIB_RECORD_SIZE * len(units)

    def align(value: int, boundary: int = 64) -> int:
        return (value + boundary - 1) // boundary * boundary

    enter_at = align(header_size)
    # A unit's address-forming instructions are page-relative: it starts on
    # a page boundary of the framework, which is itself page aligned.
    cursor = align(enter_at + len(enter_code), PAGE)
    bases = []
    for unit in units:
        bases.append(cursor)
        cursor = align(cursor + len(unit["unit"]), PAGE)
    total_pages = sum(len(unit["digests"]) for unit in units)
    # The hash is read once per page's first comparison: a load of 60% costs
    # about two probes per hit.
    slots = 16
    while 3 * slots < 5 * total_pages:
        slots *= 2
    hash_at = cursor
    end = align(hash_at + 16 * slots, PAGE)
    blob = bytearray(end)
    blob[enter_at:enter_at + len(enter_code)] = enter_code
    for unit, base in zip(units, bases):
        blob[base:base + len(unit["unit"])] = unit["unit"]
    # the global page-digest hash: digests that identify one page only
    same: dict[str, str] = {}
    for unit in units:
        # The unit binds by the content of its pages, not by its path: a
        # second copy of the same file (a library an APEX ships in several
        # places) would take every digest out of the hash, and neither would
        # ever bind. One path names the file; the guest finds every copy by
        # its pages.
        if unit["sha256"] in same:
            raise ValueError(f"{unit['path']} is the same file as "
                             f"{same[unit['sha256']]}: list it once")
        same[unit["sha256"]] = unit["path"]
    occupancy: dict[int, int] = collections.Counter()
    for unit in units:
        occupancy.update(unit["digests"])
    for unit in units:
        for page_index, digest in enumerate(unit["digests"]):
            if occupancy[digest] != 1:
                continue
            slot = digest & (slots - 1)
            while struct.unpack_from("<Q", blob, hash_at + 16 * slot)[0]:
                slot = (slot + 1) & (slots - 1)
            struct.pack_into("<QII", blob, hash_at + 16 * slot, digest,
                             unit["index"], page_index)
    record_at = HEADER_SIZE
    struct.pack_into("<8sIIIIIIII", blob, 0, ENTRY_MAGIC, BLOB_VERSION,
                     HEADER_SIZE, len(units), record_at, enter_at, hash_at,
                     slots, len(refused))
    struct.pack_into("96s", blob, 0x30, release_id.encode()[:96])
    struct.pack_into("32s32s", blob, 0x90, bytes.fromhex(manifest_sha256),
                     bytes.fromhex(system_sha256))
    for unit, base in zip(units, bases):
        name = unit["path"].encode()
        if len(name) > 95:
            raise ValueError("library path too long")
        record = record_at + LIB_RECORD_SIZE * unit["index"]
        struct.pack_into("96s", blob, record, name)
        struct.pack_into("32s", blob, record + 96,
                         bytes.fromhex(unit["sha256"]))
        struct.pack_into("24s", blob, record + 128, unit["build_id"][:24])
        struct.pack_into(
            "<QQIIIIQIIII", blob, record + 152, unit["table_base"],
            unit["table_end"], base + unit["table_at"], unit["table_words"],
            0, len(unit["digests"]), unit["text_end"],
            unit["stats"]["instructions"], unit["stats"]["steps"], base,
            base + unit["hashes_at"])
    manifest = {
        "release_id": release_id,
        "release_manifest_sha256": manifest_sha256,
        "system_partition_sha256": system_sha256,
        # The libraries left to the engine because their entry table cannot
        # be formed (the header carries the count).
        "refused": refused,
        "blob_bytes": len(blob),
        # What is not in a unit: the header and library records, ENTER, the
        # page-digest hash, and the padding that aligns the units.
        "blob_overhead": {
            "header": header_size,
            "enter": len(enter_code),
            "hash": 16 * slots,
            "alignment": len(blob) - header_size - len(enter_code) -
            16 * slots - sum(len(unit["unit"]) for unit in units),
        },
        "libraries": [{
            "path": unit["path"],
            "elf_sha256": unit["sha256"],
            "build_id": unit["build_id"].hex(),
            "executable_segment": {"vaddr": unit["vaddr"],
                                   "filesz": unit["filesz"]},
            "unit_bytes": len(unit["unit"]),
            "table_bytes": unit["table_region"],
            "functions": unit["stats"]["functions"],
            "instructions": unit["stats"]["instructions"],
            "inlined": unit["stats"]["inlined"],
            "steps": unit["stats"]["steps"],
            "steps_by_cause": unit["stats"]["steps_by_cause"],
            "inlined_by_kind": unit["stats"]["inlined_by_kind"],
            "words": unit["stats"]["words"],
            "census": unit["stats"]["census"],
            "source_bytes": unit["table_end"] - unit["table_base"],
            # The pages the digest hash can name: one that two units share
            # (zero padding, a common stub) is left out of it.
            "pages": len(unit["digests"]),
            "bindable_pages": sum(1 for digest in unit["digests"]
                                  if occupancy[digest] == 1),
        } for unit in units],
    }
    return bytes(blob), manifest


def split_refused(units: list[dict]) -> tuple[list[dict], list[dict]]:
    """The built units in index order, numbered again without the libraries
    the engine keeps (build_unit's `refused` answers), and those."""
    refused = sorted((unit for unit in units if "refused" in unit),
                     key=lambda unit: unit["index"])
    kept = sorted((unit for unit in units if "refused" not in unit),
                  key=lambda unit: unit["index"])
    for position, unit in enumerate(kept):
        unit["index"] = position
    return kept, refused


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, required=True,
                        help="the unpacked partition that holds / (system)")
    parser.add_argument("--mount", action="append", default=[],
                        metavar="GUESTPATH=DIR",
                        help="another unpacked tree (a partition or an "
                        "APEX payload) that holds the files under GUESTPATH; "
                        "repeatable")
    parser.add_argument("--library", action="append", required=True,
                        help="a library's guest path, e.g. "
                        "/system/lib64/libz.so, or a pattern in its last "
                        "component, e.g. /system/framework/arm64/boot*.oat "
                        "(a match with no executable code is skipped); "
                        "repeatable")
    parser.add_argument("--generated", action="append", default=[],
                        metavar="GUESTPATH=FILE",
                        help="a library generated for the tests (tools/aot/"
                        "fixture.py), not part of the release; it has no "
                        "licence notice; repeatable")
    parser.add_argument("--release-id", required=True)
    parser.add_argument("--release-manifest-sha256", required=True,
                        help="the SHA-256 of the release's manifest (hex)")
    parser.add_argument("--system-partition-sha256", required=True,
                        help="the SHA-256 of the release's system partition "
                        "(hex)")
    parser.add_argument("--out", type=pathlib.Path, required=True,
                        help="output directory (blob.bin, manifest.json)")
    parser.add_argument("--count-retired", action="store_true",
                        help="count the instructions each entry starts and "
                        "report that count (a diagnostics build, to measure "
                        "the budget's bound independently)")
    parser.add_argument("--jobs", type=int, default=0,
                        help="worker processes (default: about half the CPUs)")
    arguments = parser.parse_args(argv)
    mounts: dict[str, pathlib.Path] = {}
    for item in arguments.mount:
        guest, _, directory = item.partition("=")
        mounts[guest.rstrip("/")] = pathlib.Path(directory)
    count = arguments.count_retired
    arguments.library, skipped = expand_libraries(
        arguments.root, mounts, arguments.library)
    jobs = [(index, path, str(resolve_library(arguments.root, mounts, path)),
             count)
            for index, path in enumerate(arguments.library)]
    for item in arguments.generated:
        guest, _, host = item.partition("=")
        jobs.append((len(jobs), guest, host, count))
    workers = arguments.jobs or max(1, (os.cpu_count() or 2) // 2)
    order = sorted(jobs, key=lambda job: -pathlib.Path(job[2]).stat().st_size)
    if workers == 1 or len(jobs) == 1:
        units = [build_unit(job) for job in order]
    else:
        with multiprocessing.Pool(min(workers, len(jobs))) as pool:
            units = list(pool.imap_unordered(build_unit, order))
    layout = read_layout(LAYOUT_HEADER)
    # A library whose entry table cannot be formed is not in the blob: the
    # engine runs it, as it runs any library the blob does not hold.
    units, refused = split_refused(units)
    for unit in refused:
        print(f"rewrite.py: left to the engine: {unit['refused']}",
              file=sys.stderr)
    kept = {unit["path"] for unit in units}
    arguments.library = [path for path in arguments.library if path in kept]
    blob, manifest = build_blob(
        units, layout, arguments.release_id, count,
        [{"path": unit["path"], "reason": unit["refused"]}
         for unit in refused],
        manifest_sha256=arguments.release_manifest_sha256,
        system_sha256=arguments.system_partition_sha256)
    manifest["skipped"] = skipped
    arguments.out.mkdir(parents=True, exist_ok=True)
    # The notice of each library as the image states it: generated output
    # beside the framework, never tracked.
    try:
        notices = licences.record(arguments.root, mounts, arguments.library)
    except licences.NoticeError as error:
        print(f"rewrite.py: {error}", file=sys.stderr)
        return 1
    (arguments.out / "licences.json").write_text(
        json.dumps({"release_id": arguments.release_id, "libraries": notices},
                   indent=2, sort_keys=True) + "\n")
    (arguments.out / "blob.bin").write_bytes(blob)
    (arguments.out / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    with open(arguments.out / "libraries.txt", "w") as handle:
        for unit in sorted(units, key=lambda unit: unit["index"]):
            handle.write(f"{unit['index']}\t{unit['path']}\t"
                         f"{unit['host_path']}\n")
    # A sample of each inlined kind of instruction per library: where the
    # differential test starts cases to exercise every class.
    with open(arguments.out / "sites.txt", "w") as handle:
        for unit in sorted(units, key=lambda unit: unit["index"]):
            for kind in sorted(unit["sites"]):
                offsets = unit["sites"][kind]
                count = min(len(offsets), SITE_SAMPLES)
                for pick in range(count):
                    at = offsets[pick * len(offsets) // count]
                    handle.write(f"{unit['index']} {at:x} {kind}\n")
    with open(arguments.out / "steps.txt", "w") as handle:
        for unit in sorted(units, key=lambda unit: unit["index"]):
            for offset, cause in sorted(unit["step_list"]):
                handle.write(f"{unit['index']} {offset:x} {cause}\n")
    # The functions the blob covers, for the tests that walk them.
    with open(arguments.out / "functions.txt", "w") as handle:
        for unit in sorted(units, key=lambda unit: unit["index"]):
            for start, stop in unit["functions"]:
                handle.write(f"{unit['index']} {start:#x} {stop:#x}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
