#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Known answers for the AOT rewriter's encoders, decoder and assembler.

The words below were produced by the system assembler for the text beside
each; the tool must produce the same bits. The rewriter's whole output is
checked against the engine by the arm64-aot-differential differential test.
"""

__import__("sys").dont_write_bytecode = True

import collections
import hashlib
import os
import pathlib
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "tools" /
                       "aot"))

import a64decode  # noqa: E402
import asm as A  # noqa: E402
import elf64  # noqa: E402
import fixture  # noqa: E402
import rewrite  # noqa: E402

# The release the blobs are built for: the header carries both digests.
RELEASE = {"manifest_sha256": "ab" * 32, "system_sha256": "cd" * 32}

ENCODERS = [
    ("add x0, x1, #1", 0x91000420, lambda: A.add_imm(0, 1, 1)),
    ("sub sp, sp, #240", 0xd103c3ff,
     lambda: A.add_imm(31, 31, 240, sub=True)),
    ("movz x9, #0x1234", 0xd2824689, lambda: A.movz(9, 0x1234)),
    ("movk x9, #0x5678, lsl #16", 0xf2aacf09,
     lambda: A.movk(9, 0x5678, 1)),
    ("ubfx x26, x3, #12, #14", 0xd34c647a, lambda: A.ubfx(26, 3, 12, 14)),
    ("lsr x1, x2, #7", 0xd347fc41, lambda: A.lsr_imm(1, 2, 7)),
    ("add x28, x25, x28, lsl #5", 0x8b1c173c,
     lambda: A.add_reg(28, 25, 28, shift=0, amount=5)),
    ("eor x27, x27, x3, lsr #12", 0xca43337b,
     lambda: A.eor_reg(27, 27, 3, shift=1, amount=12)),
    ("mov x24, x4", 0xaa0403f8, lambda: A.mov_reg(24, 4)),
    ("ldr x9, [sp, #168]", 0xf94057e9, lambda: A.ldr_x(9, 31, 168)),
    ("str x2, [sp, #176]", 0xf9005be2, lambda: A.str_x(2, 31, 176)),
    ("ldr w9, [x16, #8]", 0xb9400a09, lambda: A.ldr_w(9, 16, 8)),
    ("ldrb w9, [x16, #904]", 0x394e2209, lambda: A.ldrb_w(9, 16, 904)),
    ("ldp x27, x28, [x28]", 0xa940739b, lambda: A.ldp_x(27, 28, 28, 0)),
    ("stp x29, x30, [sp]", 0xa9007bfd, lambda: A.stp_x(29, 30, 31, 0)),
    ("ldp q0, q1, [x16, #32]", 0xad410600,
     lambda: A.ldp_q(0, 1, 16, 32)),
    ("stp q2, q3, [x28, #64]", 0xad020f82,
     lambda: A.stp_q(2, 3, 28, 64)),
    ("mrs x9, nzcv", 0xd53b4209, lambda: A.mrs(9, "nzcv")),
    ("msr nzcv, x9", 0xd51b4209, lambda: A.msr("nzcv", 9)),
    ("mrs x10, fpcr", 0xd53b440a, lambda: A.mrs(10, "fpcr")),
    ("ret", 0xd65f03c0, lambda: A.ret_()),
    ("br x26", 0xd61f0340, lambda: A.br(26)),
    ("nop", 0xd503201f, lambda: A.nop()),
]

DECODED = [
    (0xd65f03c0, "RET", "branch_reg"),
    (0x91000420, "ADD", "addsub_imm"),
    (0xa940739b, "LDP", "ldstpair_off"),
    (0xd53b4209, "MRS", "systemmove"),
    (0xd4000001, "SVC", "exception"),
    (0xc85f7c20, "LDXR", "ldstexclr"),
    (0x1e202800, "FADD", "floatdp2"),
    (0x4e20842a, "ADD", "asimdsame"),
    (0x00000000, "UDF", "perm_undef"),
]


class EncoderTests(unittest.TestCase):
    def test_known_words(self) -> None:
        for text, word, build in ENCODERS:
            with self.subTest(text):
                self.assertEqual(build(), word, f"{build():#010x} for {text}")


class DecoderTests(unittest.TestCase):
    def test_identification(self) -> None:
        decoder = a64decode.Decoder()
        for word, mnemonic, decode_class in DECODED:
            with self.subTest(f"{word:#010x}"):
                encoding = decoder.decode(word)
                self.assertIsNotNone(encoding)
                self.assertEqual(encoding.mnemonic, mnemonic)
                self.assertEqual(encoding.decode_class, decode_class)

    def test_unallocated_words_are_unknown_to_the_table(self) -> None:
        decoder = a64decode.Decoder()
        # Reserved encoding space: not an instruction of any feature.
        self.assertIsNone(decoder.decode(0x00ff00ff))


class AssemblerTests(unittest.TestCase):
    def test_far_test_and_branch_is_relaxed(self) -> None:
        assembler = A.Assembler()
        assembler.tbz(1, 3, "far", nz=True)
        assembler.data(bytes(40000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        # tbnz is out of range (32 KiB): tbz over a following b.
        self.assertEqual(first, 0x36180041)
        self.assertEqual(second >> 26, 0b000101)
        target = assembler.label_address("far")
        delta = (second & 0x03ffffff) * 4
        self.assertEqual(4 + delta, target)

    def test_near_branch_is_not_relaxed(self) -> None:
        assembler = A.Assembler()
        assembler.cbz(5, "next", nz=True)
        assembler.label("next")
        assembler.word(A.nop())
        blob = assembler.resolve()
        self.assertEqual(len(blob), 8)
        (word,) = struct.unpack_from("<I", blob, 0)
        self.assertEqual(word, 0xb5000025)


class FakeElf:
    """The parts of an ELF library the rewriter reads: one code segment cut
    into the functions an unwind table would name."""

    def __init__(self, words: list[int], functions: list[tuple[int, int]],
                 base: int = 0x1000) -> None:
        self.base = base
        self.code = struct.pack(f"<{len(words)}I", *words)
        self.data = self.code
        self._functions = [elf64.Function(base + a, base + b)
                           for a, b in functions]
        self.sha256 = "00" * 32
        self.build_id = b"\0" * 16

    def executable_segment(self) -> elf64.Segment:
        return elf64.Segment(self.base, 0, len(self.code), len(self.code), 5)

    def functions(self) -> list[elf64.Function]:
        return self._functions

    def read_vaddr(self, vaddr: int, size: int) -> bytes:
        return self.code[vaddr - self.base:vaddr - self.base + size]


def library_of(words: list[int], functions: list[tuple[int, int]],
               base: int = 0x1000, **tuning) -> "rewrite.Library":
    layout = rewrite.read_layout(rewrite.LAYOUT_HEADER)
    return rewrite.Library(0, "/test/lib.so",
                           FakeElf(words, functions, base),
                           a64decode.Decoder(), layout, **tuning)


def unit_words(library: "rewrite.Library") -> list[int]:
    digests = [1] * ((library.table_end - library.table_base) // 4096)
    blob = library.build(digests)["unit"]
    return list(struct.unpack(f"<{len(blob) // 4}I", blob))


NOP = 0xd503201f
RET = 0xd65f03c0


class AssemblerFarBranchTests(unittest.TestCase):
    def test_far_never_branch_is_a_branch_too(self) -> None:
        # B.NV (condition 15) is taken like B.AL: AArch64 treats both as
        # always, so neither may be relaxed to a no-op.
        assembler = A.Assembler()
        assembler.bcond(15, "far")
        assembler.data(bytes(4 * 300000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        self.assertEqual(first >> 26, 0b000101)
        self.assertEqual(4 * (first & 0x03ffffff), assembler.label_address("far"))

    def test_far_always_branch_is_a_branch(self) -> None:
        # b.al has no inverse condition: the relaxed form is a branch.
        assembler = A.Assembler()
        assembler.bcond(14, "far")
        assembler.data(bytes(4 * 300000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        self.assertEqual(first >> 26, 0b000101)
        self.assertEqual(second, A.nop())
        self.assertEqual(4 * (first & 0x03ffffff), assembler.label_address("far"))

    def test_far_conditional_branch_is_inverted_over_a_branch(self) -> None:
        assembler = A.Assembler()
        assembler.bcond(0, "far")  # b.eq
        assembler.data(bytes(4 * 300000))
        assembler.label("far")
        assembler.word(A.nop())
        blob = assembler.resolve()
        first, second = struct.unpack_from("<2I", blob, 0)
        self.assertEqual(first & 15, 1)  # b.ne over the branch
        self.assertEqual(second >> 26, 0b000101)

    def test_an_unconditional_branch_out_of_reach_is_refused(self) -> None:
        # +-128 MiB: a unit larger than that cannot be assembled, and says so.
        with self.assertRaises(A.AsmError) as raised:
            A.b_word(1 << 25)
        self.assertIn("128 MiB", str(raised.exception))
        A.b_word((1 << 25) - 1)
        A.b_word(-(1 << 25))


class ExitAddressWidthTests(unittest.TestCase):
    """An exit forms its guest address in 32 bits: a library whose text
    reaches 4 GiB is refused at build time instead of leaving at a wrapped
    PC."""

    SVC = 0xd4000001

    def test_an_exit_address_beyond_4_gib_is_refused(self) -> None:
        # The text starts at 0xfffff000: its second page, at 0x100000000,
        # holds an instruction the engine has to step.
        words = [NOP] * 1024 + [self.SVC, RET]
        library = library_of(words, [(4096, 4104)], base=0xfffff000)
        with self.assertRaises(A.AsmError) as raised:
            unit_words(library)
        self.assertIn("4 GiB", str(raised.exception))

    def test_the_last_page_below_4_gib_builds(self) -> None:
        words = [NOP] * 1020 + [self.SVC, RET]
        library = library_of(words, [(4080, 4088)], base=0xfffff000)
        self.assertGreater(len(unit_words(library)), 0)


class PcRelativeWidthTests(unittest.TestCase):
    """ADR and ADRP form a 64-bit address, which wraps for a target below
    zero: the constant the rewritten code adds to the bias must keep every
    bit (a 48-bit constant gave the wrong address)."""

    @staticmethod
    def constants(words: list[int]) -> list[int]:
        """The values the movz and movk runs of `words` build, in order: a run
        ends where another movz writes its register again."""
        values: dict[int, int] = {}
        built: list[int] = []
        for word in words:
            rd = word & 31
            move = (word >> 23) & 0x1ff
            if move == 0b110100101:  # movz x, #imm16, lsl #(16 * hw)
                if rd in values:
                    built.append(values[rd])
                values[rd] = ((word >> 5) & 0xffff) << (16 * ((word >> 21) & 3))
            elif move == 0b111100101 and rd in values:  # movk
                shift = 16 * ((word >> 21) & 3)
                values[rd] = (values[rd] & ~(0xffff << shift)) | \
                    (((word >> 5) & 0xffff) << shift)
        return built + list(values.values())

    def test_an_adrp_below_zero_keeps_all_64_bits(self) -> None:
        # adrp x0, #-2 pages from page 1: the target is -0x1000.
        adrp = 0xd0ffffe0
        library = library_of([adrp, RET], [(0, 8)])
        built = self.constants(unit_words(library))
        self.assertIn(0xfffffffffffff000, built)

    def test_an_adrp_inside_the_library_is_unchanged(self) -> None:
        # adrp x0, #1 page from page 1: the target is 0x2000.
        adrp = 0x90000000 | (1 << 29)
        library = library_of([adrp, RET], [(0, 8)])
        built = self.constants(unit_words(library))
        self.assertNotIn(0xfffffffffffff000, built)


    # An LDR (literal) at 0x1000 whose literal is at 0x1000 + 4 * imm19.
    LITERAL_FORMS = {"ldr w": 0x18000000, "ldr x": 0x58000000,
                     "ldrsw": 0x98000000, "ldr s": 0x1c000000,
                     "ldr d": 0x5c000000, "ldr q": 0x9c000000}

    @staticmethod
    def literal_load(form: int, words_away: int) -> int:
        return form | ((words_away & 0x7ffff) << 5)

    def test_a_literal_load_below_zero_keeps_all_64_bits(self) -> None:
        # The literal lies at 0x1000 - 0x3000 = -0x2000 (the unit's base is
        # zero, another mapping lies below it): the address is the bias less
        # 0x2000, not the bias plus the low 24 bits of -0x2000.
        for name, form in self.LITERAL_FORMS.items():
            with self.subTest(name):
                library = library_of(
                    [self.literal_load(form, -0xc00), RET], [(0, 8)])
                built = self.constants(unit_words(library))
                self.assertIn(0xffffffffffffe000, built)

    def test_a_literal_load_inside_the_library_is_unchanged(self) -> None:
        for name, form in self.LITERAL_FORMS.items():
            with self.subTest(name):
                library = library_of(
                    [self.literal_load(form, 0xc00), RET], [(0, 8)])
                built = self.constants(unit_words(library))
                self.assertNotIn(0xffffffffffffe000, built)
                self.assertNotIn(0xffffe000, built)


class ExitBelowTheLibraryTests(unittest.TestCase):
    """A direct branch to code outside the library leaves by the branch exit.
    Its target may lie below the library's first byte (another library mapped
    lower: a method of boot-framework.oat calling into boot.oat) or beyond
    4 GiB: it is then formed as a full address and leaves by the absolute
    exit, never as a wrapped 32-bit offset."""

    @staticmethod
    def library(target: int, base: int = 0x1000) -> "rewrite.Library":
        word = 0x94000000 | (((target - (base + 4)) // 4) & 0x3ffffff)
        return library_of([NOP, word, RET], [(0, 12)], base=base)

    def test_a_target_below_the_library_keeps_all_64_bits(self) -> None:
        built = PcRelativeWidthTests.constants(
            unit_words(self.library(-0x2000)))
        self.assertIn(0xffffffffffffe000, built)
        self.assertNotIn(0xffffe000, built)

    @staticmethod
    def jumps_to(library: "rewrite.Library", label: str) -> int:
        """How many unconditional branches of the built unit go to `label`."""
        words = unit_words(library)
        target = library.asm.label_address(label)
        # The indirect branch routine's own way out when a word has no entry
        # is not a function's.
        routine = library.asm.label_address("IND_NOENTRY") + 4
        found = 0
        for index, word in enumerate(words):
            if word >> 26 == 0b000101 and 4 * index != routine:
                delta = ((word & 0x03ffffff) ^ 0x2000000) - 0x2000000
                found += 4 * (index + delta) == target
        return found

    def test_a_target_below_the_library_leaves_by_the_absolute_exit(self
                                                                    ) -> None:
        library = self.library(-0x2000)
        self.assertEqual(self.jumps_to(library, "EXIT_BRANCH_ABS"), 1)

    def test_a_target_beyond_4_gib_leaves_by_the_absolute_exit(self) -> None:
        library = self.library(0x100000100, base=0xfffff000)
        built = PcRelativeWidthTests.constants(unit_words(library))
        self.assertIn(0x100000100, built)

    def test_a_target_inside_the_library_still_leaves_by_an_offset(self
                                                                   ) -> None:
        # Out of the covered code but a small positive offset: the exit
        # routine adds the bias to it, as for every library until now.
        library = self.library(0x3000)
        built = PcRelativeWidthTests.constants(unit_words(library))
        self.assertIn(0x3000, built)
        self.assertEqual(self.jumps_to(library, "EXIT_BRANCH_ABS"), 0)
        self.assertEqual(self.jumps_to(library, "EXIT_BRANCH"), 1)


class ReservedWordTests(unittest.TestCase):
    """A word the instruction tables hold but the architecture leaves
    unallocated is left to the engine, which raises the illegal instruction
    itself: copied, the host would raise it and end the emulator. Data inside
    a method's code (ART's literal pools) is full of such words, and code is
    entered at any word."""

    # Valid words, from the system assembler.
    VALID = [
        ("ubfx w0, w1, #4, #8", 0x53042c20),
        ("sbfx x0, x1, #4, #8", 0x93442c20),
        ("and x0, x1, #0xff", 0x92401c20),
        ("and w0, w1, #0xff", 0x12001c20),
        ("movz w0, #1, lsl #16", 0x52a00020),
        ("movz x0, #1, lsl #48", 0xd2e00020),
        ("add w0, w1, w2, lsl #3", 0x0b020c20),
        ("add x0, x1, x2, asr #60", 0x8b82f020),
        ("extr w0, w1, w2, #3", 0x13820c20),
        ("extr x0, x1, x2, #40", 0x93c2a020),
        ("add x0, x1, w2, uxtw #4", 0x8b225020),
        ("orr w0, w1, w2, lsl #31", 0x2a027c20),
        # One bit from a reserved form of the classes below.
        ("add vector, 4H", 0x0e608400),
        ("saddlv", 0x0e303800),
        ("saddw", 0x0e201000),
        ("smlal by element", 0x0f402000),
        ("ext", 0x2e000000),
        ("dup element", 0x0e010400),
        ("rev16", 0x0e201800),
        ("uzp1", 0x0e401800),
        ("cmgt", 0x0e203400),
        ("sshll", 0x0f08a400),
        ("sqdmlal scalar", 0x5e609000),
        ("sqdmlal scalar by element", 0x5f403000),
        ("sqxtn scalar", 0x5e614800),
        ("dup scalar", 0x5e010400),
        ("sqdmulh scalar", 0x5e60b400),
        ("scvtf scalar by immediate", 0x5f18e400),
        ("madd", 0x9b000000),
        ("smulh", 0x9b407c00),
        ("scvtf s0, w0, #32", 0x1e028000),
        ("fcmpe s0, #0.0", 0x1e202018),
        ("fcmpe s0, s1", 0x1e212010),
        ("ldp s0, s1, [x0]", 0x2d400400),
        ("ldnp s0, s1, [x0]", 0x2c400400),
        ("ldp s0, s1, [x0], #0", 0x2cc00400),
        ("ldp s0, s1, [x0, #0]!", 0x2dc00400),
        ("ldp w0, wzr, [sp]", 0x29407c00),
        ("ldp x0, x1, [x2]", 0xa9400440),
    ]
    # The same forms with a field the architecture does not allow.
    RESERVED = [
        ("32-bit bitfield, immr of 32 or more", 0x53242c20),
        ("64-bit bitfield with N clear", 0x93042c20),
        ("32-bit bitfield with the unallocated opc", 0x73042c20 | 0x20000000),
        ("32-bit movz, hw 2", 0x52c00020),
        ("32-bit add, shift of 32", 0x0b028020),
        ("add with the shift type ror", 0x0bc20c20),
        ("32-bit orr, shift of 32", 0x2a028020),
        ("32-bit extr, lsb of 32 or more", 0x13828c20),
        ("add extended, shift of 5", 0x8b225420),
        ("32-bit logical immediate with N set", 0x12401c20),
        ("logical immediate of all ones", 0x12007c20),
        ("logical immediate with no element", 0x1200fc20),
        # The Advanced SIMD, multiply-high and fixed-point classes: the
        # engine rejects a reserved size, Q, opcode or scale like these, and
        # the tables do not tell them from the allocated forms.
        ("add vector, 2D with Q clear", 0x0ee08400),
        ("across lanes, reserved size", 0x0eb03800),
        ("three different, reserved size", 0x0e209000),
        ("by element, reserved size", 0x0f002000),
        ("ext with Q clear and imm4 bit 3", 0x2e004000),
        ("copy, reserved imm5", 0x0e000400),
        ("two-register misc, reserved size", 0x0e601800),
        ("permute, 2D with Q clear", 0x0ec01800),
        ("three same, reserved opcode", 0x0e20b400),
        ("shift by immediate, reserved immh", 0x0f08e400),
        ("scalar three different, reserved size", 0x5e209000),
        ("scalar by element, reserved size", 0x5f003000),
        ("scalar two-register misc, reserved size", 0x5ee14800),
        ("scalar copy, reserved imm5", 0x5e000400),
        ("scalar three same, reserved size", 0x5e20b400),
        ("scalar shift by immediate, reserved immh", 0x5f08e400),
        ("smulh with Ra not the zero register", 0x9b400000),
        ("32-bit fixed-point convert, scale of 0", 0x1e020000),
        ("fcmpe against zero with Rm named", 0x1e212018),
        ("ldp s0, s0, [x0]", 0x2d400000),
        ("ldnp s0, s0, [x0]", 0x2c400000),
        ("ldp s0, s0, [x0], #0", 0x2cc00000),
        ("ldp s0, s0, [x0, #0]!", 0x2dc00000),
        ("ldp wzr, wzr, [sp]", 0x29407fff),
        ("ldp x1, x1, [x2]", 0xa9400441),
    ]
    # Words the tables list as unallocated by name, in a class the rewriter
    # otherwise copies (found as the data of a literal pool).
    UNALLOCATED = [0x3bcccccd, 0x3a83126f]

    def classify(self, word: int) -> tuple[str, list[tuple[int, str]]]:
        decoder = a64decode.Decoder(a64decode.DEFAULT_TABLE)
        encoding = decoder.decode(word)
        library = library_of([word, RET], [(0, 8)])
        unit_words(library)
        return encoding.decode_class, library.step_list

    def test_valid_words_are_copied(self) -> None:
        for text, word in self.VALID:
            with self.subTest(text):
                cls, steps = self.classify(word)
                self.assertFalse(rewrite.reserved_corner(cls, word))
                self.assertEqual(steps, [])

    def test_reserved_words_are_left_to_the_engine(self) -> None:
        for text, word in self.RESERVED:
            with self.subTest(text):
                _, steps = self.classify(word)
                self.assertEqual(len(steps), 1, text)
                self.assertIn(steps[0][1], ("unallocated", "reserved",
                                            "pair-same-register"))

    def test_words_the_tables_name_unallocated_are_left_to_the_engine(self
                                                                      ) -> None:
        for word in self.UNALLOCATED:
            with self.subTest(hex(word)):
                _, steps = self.classify(word)
                self.assertEqual([cause for _, cause in steps],
                                 ["unallocated"])


class OatFileTests(unittest.TestCase):
    """A boot image's OAT file: no unwind table, its methods named by the
    symbols of .gnu_debugdata, and a text that may be absent."""

    def setUp(self) -> None:
        self.elf = elf64.Elf(fixture.build_oat())

    def test_methods_come_from_the_debug_symbols(self) -> None:
        self.assertEqual(
            [(f.start, f.end) for f in self.elf.functions()],
            [(at, at + size) for at, size in fixture.oat_functions()])

    def test_methods_that_touch_stay_apart(self) -> None:
        functions = [(f.start, f.end) for f in self.elf.functions()]
        self.assertIn((0x1000, 0x1010), functions)
        self.assertIn((0x1010, 0x1020), functions)

    def test_unwind_ranges_that_touch_are_still_one_function(self) -> None:
        # Every library with an unwind table is split as before.
        elf = elf64.Elf(fixture.build())
        found = [(0x1000, 0x1010), (0x1010, 0x1020), (0x1200, 0x1210)]
        with mock.patch.object(elf64.Elf, "_eh_frame_functions",
                               return_value=found):
            ranges = [(f.start, f.end) for f in elf.functions()]
        self.assertIn((0x1000, 0x1020), ranges)
        self.assertNotIn((0x1000, 0x1010), ranges)
        self.assertIn((0x1200, 0x1210), ranges)

    def test_a_symbol_with_no_size_or_no_function_type_names_no_method(self
                                                                       ) -> None:
        starts = [f.start for f in self.elf.functions()]
        self.assertNotIn(fixture.OAT_TEXT + 0x300, starts)
        self.assertNotIn(fixture.OAT_TEXT + 0x400, starts)

    def test_a_file_with_no_unwind_table_and_no_symbols_is_refused(self
                                                                   ) -> None:
        elf = elf64.Elf(fixture.build_oat())
        del elf.sections[".gnu_debugdata"]
        with self.assertRaises(elf64.ElfError):
            elf.functions()

    def test_the_header_only_file_has_no_code(self) -> None:
        self.assertTrue(self.elf.has_executable_segment())
        self.assertFalse(elf64.Elf(fixture.build_oat(code=False)
                                   ).has_executable_segment())

    def test_every_direct_branch_below_the_library_leaves_by_the_absolute_exit(
            self) -> None:
        library = rewrite.Library(
            0, "/aot-test/boot-fixture.oat", self.elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        digests = [1] * ((library.table_end - library.table_base) // 4096)
        blob = library.build(digests)["unit"]
        words = list(struct.unpack(f"<{len(blob) // 4}I", blob))
        built = PcRelativeWidthTests.constants(words)
        self.assertIn(fixture.OAT_BELOW & 0xffffffffffffffff, built)
        self.assertNotIn(fixture.OAT_BELOW & 0xffffffff, built)
        # The five branch kinds share the one stub the first emitted.
        self.assertEqual(ExitBelowTheLibraryTests.jumps_to(
            library, "EXIT_BRANCH_ABS"), 1)


class LibraryPatternTests(unittest.TestCase):
    """A library path may end in a pattern: every file it matches, in name
    order, except the ones that hold no code."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        folder = self.root / "system" / "framework" / "arm64"
        folder.mkdir(parents=True)
        (folder / "boot-b.oat").write_bytes(fixture.build_oat())
        (folder / "boot-a.oat").write_bytes(fixture.build_oat())
        (folder / "boot-c.oat").write_bytes(fixture.build_oat(code=False))
        (folder / "boot.vdex").write_bytes(b"not an oat")
        (folder / "other.oat").write_bytes(fixture.build_oat())
        # A named library must be a regular file of the image.
        library = self.root / "system" / "lib64"
        library.mkdir(parents=True)
        (library / "libz.so").write_bytes(fixture.build())

    def test_a_pattern_is_every_match_in_name_order_without_the_empty_ones(
            self) -> None:
        libraries, skipped = rewrite.expand_libraries(
            self.root, {}, ["/system/lib64/libz.so",
                            "/system/framework/arm64/boot*.oat"])
        self.assertEqual(libraries, [
            "/system/lib64/libz.so", "/system/framework/arm64/boot-a.oat",
            "/system/framework/arm64/boot-b.oat"])
        self.assertEqual(skipped, ["/system/framework/arm64/boot-c.oat"])

    def test_a_pattern_that_matches_nothing_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            rewrite.expand_libraries(self.root, {},
                                     ["/system/framework/arm64/none*.oat"])

    def test_a_pattern_is_for_the_last_component_only(self) -> None:
        with self.assertRaises(SystemExit):
            rewrite.expand_libraries(self.root, {},
                                     ["/system/framework/*/boot.oat"])

    def test_a_path_named_twice_is_refused(self) -> None:
        # By an explicit entry and a pattern, by two patterns, and by the
        # same entry twice: each page digest would then be shared by two
        # units and leave the digest hash, and the library would never bind.
        for entries in (
                ["/system/framework/arm64/boot-a.oat",
                 "/system/framework/arm64/boot*.oat"],
                ["/system/framework/arm64/boot*.oat",
                 "/system/framework/arm64/boot-*.oat"],
                ["/system/lib64/libz.so", "/system/lib64/libz.so"]):
            with self.subTest(entries):
                with self.assertRaises(SystemExit) as raised:
                    rewrite.expand_libraries(self.root, {}, entries)
                self.assertIn("listed twice", str(raised.exception))

    def test_a_link_is_the_file_it_names_inside_the_image(self) -> None:
        folder = self.root / "system" / "framework" / "arm64"
        (folder / "boot-link.oat").symlink_to("boot-a.oat")
        # The link is boot-a.oat again, which the pattern names itself.
        with self.assertRaises(SystemExit) as raised:
            rewrite.expand_libraries(
                self.root, {}, ["/system/framework/arm64/boot-*.oat"])
        self.assertIn("/system/framework/arm64/boot-a.oat is listed twice",
                      str(raised.exception))
        libraries, _ = rewrite.expand_libraries(
            self.root, {}, ["/system/framework/arm64/boot-l*.oat"])
        self.assertEqual(libraries, ["/system/framework/arm64/boot-a.oat"])

    def test_a_link_chain_is_followed_by_the_guest_s_rules(self) -> None:
        folder = self.root / "system" / "framework" / "arm64"
        (folder / "boot-abs.oat").symlink_to(
            "/system/framework/arm64/other.oat")
        libraries, skipped = rewrite.expand_libraries(
            self.root, {}, ["/system/framework/arm64/boot-abs*"])
        self.assertEqual(libraries, ["/system/framework/arm64/other.oat"])
        self.assertEqual(skipped, [])

    def test_a_link_that_leaves_the_image_is_skipped_not_dropped(self) -> None:
        folder = self.root / "system" / "framework" / "arm64"
        # The host has this file, the image does not: the link is followed
        # from the image's root, where nothing is.
        host_only = self.root.parent / "host-only.oat"
        host_only.write_bytes(fixture.build_oat())
        self.addCleanup(host_only.unlink)
        (folder / "boot-away.oat").symlink_to(host_only)
        (folder / "boot-loop.oat").symlink_to("boot-loop.oat")
        (folder / "boot-dangling.oat").symlink_to("/system/nothing.oat")
        libraries, skipped = rewrite.expand_libraries(
            self.root, {}, ["/system/framework/arm64/boot-*.oat"])
        self.assertEqual(libraries, ["/system/framework/arm64/boot-a.oat",
                                     "/system/framework/arm64/boot-b.oat"])
        self.assertEqual(skipped, [
            "/system/framework/arm64/boot-away.oat",
            "/system/framework/arm64/boot-c.oat",
            "/system/framework/arm64/boot-dangling.oat",
            "/system/framework/arm64/boot-loop.oat"])

    def test_a_named_file_is_not_looked_at_for_code(self) -> None:
        # Naming a file asks for it: an empty one is the build's own error
        # later, as before.
        libraries, skipped = rewrite.expand_libraries(
            self.root, {}, ["/system/framework/arm64/boot-c.oat"])
        self.assertEqual(libraries, ["/system/framework/arm64/boot-c.oat"])
        self.assertEqual(skipped, [])

    def test_a_named_file_that_is_not_in_the_image_is_refused(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            rewrite.expand_libraries(self.root, {},
                                     ["/system/lib64/libnothing.so"])
        self.assertIn("not a regular file of the image", str(raised.exception))


class ImageLinkTests(unittest.TestCase):
    """Every component of a library path is resolved by the guest image's
    rules before the host is asked anything: a link, whether it is the last
    component, a directory on the way or a directory inside another link's
    target, never reaches a file outside the unpacked image."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        base = pathlib.Path(self.directory.name)
        self.root = base / "image"
        self.outside = base / "outside"
        self.outside.mkdir()
        (self.outside / "libout.so").write_bytes(fixture.build())
        (self.outside / "boot-out.oat").write_bytes(fixture.build_oat())
        for folder in ("system/lib64", "system/etc", "system/framework"):
            (self.root / folder).mkdir(parents=True)
        (self.root / "system/lib64/libin.so").write_bytes(fixture.build())
        (self.root / "system/etc/boot-in.oat").write_bytes(
            fixture.build_oat())

    def link(self, name: str, target: "str | pathlib.Path") -> None:
        (self.root / name).symlink_to(target)

    def toward_outside(self, name: str) -> str:
        """A relative link from `name` that reaches the outside directory on
        the host: it climbs out of the image, which the guest's `..` cannot."""
        start = (self.root / name).parent
        return os.path.relpath(self.outside, start)

    def test_a_named_link_to_a_file_outside_is_refused(self) -> None:
        self.link("system/lib64/libaway.so", self.outside / "libout.so")
        self.link("system/lib64/libclimb.so",
                  self.toward_outside("system/lib64/libclimb.so") +
                  "/libout.so")
        for name in ("libaway.so", "libclimb.so"):
            with self.subTest(name):
                with self.assertRaises(SystemExit) as raised:
                    rewrite.expand_libraries(
                        self.root, {}, [f"/system/lib64/{name}"])
                self.assertIn("not a regular file of the image",
                              str(raised.exception))

    def test_a_matched_link_to_a_file_outside_is_skipped(self) -> None:
        # The final link alone, absolute and climbing out of the image: the
        # guest's `..` stops at the root, so it names a path that is not
        # there. The pre-existing rule; the row keeps it.
        self.link("system/framework/boot-away.oat",
                  self.outside / "boot-out.oat")
        self.link("system/framework/boot-climb.oat",
                  self.toward_outside("system/framework/boot-climb.oat") +
                  "/boot-out.oat")
        (self.root / "system/framework/boot-real.oat").write_bytes(
            fixture.build_oat())
        libraries, skipped = rewrite.expand_libraries(
            self.root, {}, ["/system/framework/boot-*.oat"])
        self.assertEqual(libraries, ["/system/framework/boot-real.oat"])
        self.assertEqual(skipped, ["/system/framework/boot-away.oat",
                                   "/system/framework/boot-climb.oat"])

    def test_a_final_link_through_a_directory_link_outside_is_skipped(
            self) -> None:
        # The final link is inside the image and its target runs through a
        # directory link whose own target is outside.
        self.link("system/framework/outdir", self.outside)
        self.link("system/framework/boot-through.oat", "outdir/boot-out.oat")
        (self.root / "system/framework/boot-real.oat").write_bytes(
            fixture.build_oat())
        libraries, skipped = rewrite.expand_libraries(
            self.root, {}, ["/system/framework/boot-*.oat"])
        self.assertEqual(libraries, ["/system/framework/boot-real.oat"])
        self.assertEqual(skipped, ["/system/framework/boot-through.oat"])

    def test_a_directory_link_to_the_outside_finds_nothing(self) -> None:
        # Both the enumeration of a pattern and the file a name reaches
        # through the link: absolute, and relative past the image's root.
        self.link("lib", self.outside)
        self.link("up", self.toward_outside("up"))
        for directory in ("lib", "up"):
            with self.subTest(directory, entry="pattern"):
                with self.assertRaises(SystemExit) as raised:
                    rewrite.expand_libraries(
                        self.root, {}, [f"/{directory}/boot*.oat"])
                self.assertIn("matches no file", str(raised.exception))
            with self.subTest(directory, entry="name"):
                with self.assertRaises(SystemExit) as raised:
                    rewrite.expand_libraries(
                        self.root, {}, [f"/{directory}/libout.so"])
                self.assertIn("not a regular file of the image",
                              str(raised.exception))

    def test_a_directory_link_inside_the_image_still_finds_its_unit(
            self) -> None:
        # As /etc is /system/etc: an absolute link, and a relative one.
        self.link("etc", "/system/etc")
        self.link("system/framework/etc2", "../etc")
        for entry in ("/etc/boot*.oat", "/system/framework/etc2/b*-in*",
                      "/etc/boot-in.oat"):
            with self.subTest(entry):
                libraries, skipped = rewrite.expand_libraries(
                    self.root, {}, [entry])
                self.assertEqual(libraries, ["/system/etc/boot-in.oat"])
                self.assertEqual(skipped, [])
        self.assertEqual(rewrite.follow_in_image(
            self.root, {}, "/etc/boot-in.oat"), "/system/etc/boot-in.oat")
        self.assertEqual(rewrite.follow_in_image(
            self.root, {}, "/system/framework/etc2/../lib64/libin.so"),
            "/system/lib64/libin.so")
        # `..` stops at the image's root.
        self.assertEqual(rewrite.follow_in_image(
            self.root, {}, "/../../system/lib64/libin.so"),
            "/system/lib64/libin.so")

    def test_a_mount_is_the_builds_not_the_images(self) -> None:
        mount = self.outside / "mounted"
        mount.mkdir()
        (mount / "libm.so").write_bytes(fixture.build())
        mounts = {"/apex/m": mount}
        self.link("system/lib64/libm.so", "/apex/m/libm.so")
        libraries, _ = rewrite.expand_libraries(
            self.root, mounts, ["/system/lib64/libm.so"])
        self.assertEqual(libraries, ["/apex/m/libm.so"])

    def test_a_chain_of_links_that_never_ends_is_refused(self) -> None:
        self.link("system/lib64/libloop.so", "libloop.so")
        with self.assertRaises(SystemExit):
            rewrite.expand_libraries(self.root, {},
                                     ["/system/lib64/libloop.so"])

    def test_the_link_bound_is_the_guests(self) -> None:
        # A chain of exactly LINK_LIMIT links resolves; one more is refused.
        self.assertEqual(rewrite.LINK_LIMIT, 40)
        for count, found in ((rewrite.LINK_LIMIT, True),
                             (rewrite.LINK_LIMIT + 1, False)):
            with self.subTest(links=count):
                folder = self.root / f"chain{count}"
                folder.mkdir()
                for index in range(count):
                    target = "libin.so" if index == count - 1 else \
                        f"l{index + 1}"
                    (folder / f"l{index}").symlink_to(
                        f"/system/lib64/{target}" if index == count - 1
                        else target)
                guest = f"/chain{count}/l0"
                self.assertEqual(
                    rewrite.follow_in_image(self.root, {}, guest),
                    "/system/lib64/libin.so" if found else None)


class SimdRegisterFieldTests(unittest.TestCase):
    """A vector field must never be treated as a general register (the
    register substitution applies only to x18, x24-x28 and sp)."""

    REGISTERS = (0, 5, 18, 24, 25, 26, 27, 28, 31)
    decoder = a64decode.Decoder()

    def fields(self, word: int):
        encoding = self.decoder.decode(word)
        self.assertIsNotNone(encoding, hex(word))
        return encoding, rewrite.Library._copy_fields(None, word, encoding)

    def test_ins_element_names_no_general_register(self) -> None:
        for imm5 in (1, 2, 4, 8, 16, 3, 6, 12, 24):
            for imm4 in range(16):
                for rn in self.REGISTERS:
                    for rd in self.REGISTERS:
                        word = (0x6e000400 | imm5 << 16 | imm4 << 11 |
                                rn << 5 | rd)
                        encoding = self.decoder.decode(word)
                        if encoding is None or \
                                encoding.decode_class != "asimdins":
                            continue
                        fields = rewrite.Library._copy_fields(
                            None, word, encoding)
                        self.assertEqual(fields, [], hex(word))

    def test_reviewed_ins_words_keep_their_vector_registers(self) -> None:
        # INS V0.B[0], V27.B[1] and INS V18.B[0], V0.B[5]: imm4 selects the
        # source lane, so no field names a general register.
        for word in (0x6e010f60, 0x6e012c12):
            self.assertEqual(self.fields(word)[1], [], hex(word))

    def test_general_register_forms_still_substitute(self) -> None:
        # INS (general), DUP (general): Rn is a general register.
        for rn in (18, 24, 31):
            ins = 0x4e001c00 | 8 << 16 | rn << 5 | 3
            self.assertEqual(self.fields(ins)[1], [(5, False)])
            dup = 0x4e000c00 | 8 << 16 | rn << 5 | 3
            self.assertEqual(self.fields(dup)[1], [(5, False)])
        # UMOV: Rd is a general register.
        for rd in (18, 28):
            umov = 0x4e003c00 | 8 << 16 | 4 << 5 | rd
            self.assertEqual(self.fields(umov)[1], [(0, False)])


class FixtureTests(unittest.TestCase):
    """The generated test library has the control flow the harness needs."""

    def setUp(self) -> None:
        self.elf = elf64.Elf(fixture.build())
        self.library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", self.elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))

    def test_ranges_and_plt(self) -> None:
        functions = [(f.start, f.end) for f in self.elf.functions()]
        self.assertEqual(
            functions[:3],
            [(0x1000, 0x1010), (0x1020, 0x1030), (0x1040, 0x1050)])
        # The .iplt stubs of a static executable are covered like the .plt.
        self.assertIn(
            (fixture.IPLT, fixture.IPLT + 4 * len(fixture.PLT_CODE)),
            functions)

    def test_a_routine_only_a_symbol_names_is_a_function(self) -> None:
        # Assembly with no unwind entry, a data word inside its range.
        functions = [(f.start, f.end) for f in self.elf.functions()]
        self.assertIn((fixture.SYMBOL_ONLY,
                       fixture.SYMBOL_ONLY + 4 * len(fixture.SYMBOL_CODE)),
                      functions)

    def test_symbols_that_name_no_range_or_one_already_given_add_nothing(
            self) -> None:
        functions = [(f.start, f.end) for f in self.elf.functions()]
        starts = [start for start, _ in functions]
        for at in fixture.SYMBOLS_NO_RANGE:
            self.assertNotIn(at, starts)
        self.assertIn((fixture.LOOP_A, fixture.LOOP_A + 0x10), functions)

    def test_a_symbol_extends_the_function_it_overlaps_not_one_it_touches(
            self) -> None:
        merged = [elf64.Function(0x1000, 0x1010),
                  elf64.Function(0x1020, 0x1030)]
        symbols = [(0x1008, 0x1018), (0x1030, 0x1040),
                   (0x100000, 0x100010)]
        with mock.patch.object(elf64.Elf, "_symbol_functions",
                               return_value=symbols):
            found = [(f.start, f.end) for f in self.elf._with_symbols(merged)]
        self.assertEqual(found, [(0x1000, 0x1018), (0x1020, 0x1030),
                                 (0x1030, 0x1040)])

    def test_the_symbols_of_the_debug_data_are_a_source(self) -> None:
        elf = elf64.Elf(fixture.build_oat())
        ranges = elf._symbol_functions()
        for at, size in fixture.oat_functions():
            self.assertIn((at, at + size), ranges)

    def test_loop_branch_lands_inside_another_range(self) -> None:
        # b.lo at 0x1028 (range B) targets 0x1004 (inside range A), and no
        # function entry lies on the cycle.
        kind, target = self.library._flow(fixture.CODE[0x1028], 0x1028)
        self.assertEqual((kind, target), ("cond", 0x1004))
        self.assertIn(0x1004, self.library.charge)
        self.assertNotIn(0x1008, self.library.charge)


class MemoryProbeShapeTests(unittest.TestCase):
    """The words of an access's probe: an SP access with an immediate offset
    inside one aligned 16-byte granule has no last-byte address, any other
    scalar of more than one byte indexes by its last byte and compares the tag
    with its first (the access then adds the host delta itself), a byte and a
    pair keep their forms; a probe that leaves the first byte's address enters
    the refill routine that gives the host the last."""

    @staticmethod
    def shape(word: int) -> tuple[int, int, set]:
        library = library_of([word, RET], [(0, 8)])
        unit_words(library)
        assert library.stats.steps == 0, hex(word)
        census = library.stats.census
        return (census["mem.probe"] // 4, census["mem.access"] // 4,
                library.refill_used)

    def test_a_stack_scalar_inside_the_granule(self) -> None:
        for word, kind, total in (
                (0xf94007e0, "LOAD", 8),     # ldr x0,[sp,#8]
                (0x3dc007e0, "LOAD", 16),    # ldr q0,[sp,#16]
                (0xf90007e0, "STORE", 8)):   # str x0,[sp,#8]
            self.assertEqual(self.shape(word),
                             (5, 1, {(kind, total, 0, True)}), hex(word))

    def test_other_scalars_index_by_the_last_byte(self) -> None:
        for word, probe, kind, total in (
                (0xf840c3e0, 6, "LOAD", 8),     # ldur x0,[sp,#12]: no granule
                (0x3cc083e0, 6, "LOAD", 16),    # ldur q0,[sp,#8]
                (0xf800c3e0, 6, "STORE", 8),    # stur x0,[sp,#12]
                (0xf9400420, 7, "LOAD", 8)):    # ldr x0,[x1,#8]: strips the tag
            self.assertEqual(self.shape(word),
                             (probe, 1, {(kind, total, 0, True)}), hex(word))

    def test_a_byte_and_a_pair_keep_their_forms(self) -> None:
        for word, probe, kind, total, first in (
                (0x394007e0, 5, "LOAD", 1, False),    # ldrb w0,[sp,#1]
                (0xa94107e0, 6, "LOAD", 16, True),    # ldp x0,x1,[sp,#16]
                (0xa94087e0, 7, "LOAD", 16, False),   # ldp x0,x1,[sp,#8]
                (0xa90087e0, 7, "STORE", 16, False),  # stp x0,x1,[sp,#8]
                (0xa9410440, 8, "LOAD", 16, False)):  # ldp x0,x1,[x2,#16]
            self.assertEqual(self.shape(word),
                             (probe, 1, {(kind, total, 0, first)}), hex(word))


class HintAndFloatingPointTests(unittest.TestCase):
    """What the rewriter inlines must be what the engine runs natively."""

    @staticmethod
    def steps(word: int) -> int:
        library = library_of([word, RET], [(0, 8)])
        unit_words(library)
        return library.stats.steps

    def test_hints_the_engine_runs_as_no_ops_are_inlined(self) -> None:
        # paciasp, autiasp, bti c, nop, wfe: no effect for this guest.
        for word in (0xd503233f, 0xd50323bf, 0xd503245f, 0xd503201f,
                     0xd503205f):
            self.assertEqual(self.steps(word), 0, hex(word))

    def test_hints_with_an_effect_step(self) -> None:
        # yield, dgh: gadgets with an effect in the engine.
        for word in (0xd503203f, 0xd50320df):
            self.assertEqual(self.steps(word), 1, hex(word))

    def test_single_and_double_precision_are_inlined(self) -> None:
        for word in (0x1e622820,   # fadd d0, d1, d2
                     0x1e612000,   # fcmp d0, d1
                     0x4e22d420):  # fadd v0.4s, v1.4s, v2.4s
            self.assertEqual(self.steps(word), 0, hex(word))

    def test_general_register_operands_are_substituted(self) -> None:
        decoder = a64decode.Decoder()
        for word, expected in ((0x9e620240, [(5, False)]),   # scvtf d0, x18
                               (0x9e780038, [(0, False)]),   # fcvtzs x24, d1
                               (0x9e660079, [(0, False)]),   # fmov x25, d3
                               (0x9e670343, [(5, False)])):  # fmov d3, x26
            encoding = decoder.decode(word)
            self.assertEqual(
                rewrite.Library._copy_fields(None, word, encoding),
                expected, hex(word))

    def test_what_the_engine_may_run_in_software_steps(self) -> None:
        for word in (0x1ee22820,   # fadd h0, h1, h2 (FEAT_FP16)
                     0x1e284020,   # frint32z s0, s1 (FEAT_FRINTTS)
                     0x1ef80020,   # fcvtzs w0, h1 (FEAT_FP16)
                     0x1e634020,   # bfcvt h0, s1 (FEAT_BF16)
                     0x1e7e0020):  # fjcvtzs w0, d1 (FEAT_JSCVT)
            self.assertEqual(self.steps(word), 1, hex(word))


class BudgetTests(unittest.TestCase):
    def test_every_backward_branch_target_is_charged(self) -> None:
        # Function B (0x1010..0x1020) branches back into the interior of
        # function A (0x1000..0x1010): a cycle through two unwind ranges.
        # b #-0xc from 0x1018 is the branch to 0x100c.
        b_back = 0x14000000 | ((-3) & 0x3ffffff)
        words = [NOP, NOP, NOP, NOP,          # A: 0x1000-0x100c
                 NOP, NOP, b_back, RET]       # B: 0x1010-0x101c
        library = library_of(words, [(0, 0x10), (0x10, 0x20)])
        self.assertIn(0x100c, library.charge)
        self.assertIn(0x1000, library.charge)
        self.assertIn(0x1010, library.charge)

    def test_a_self_loop_is_charged(self) -> None:
        words = [NOP, 0x14000000, RET]  # b . at 0x1004
        library = library_of(words, [(0, 0xc)])
        self.assertIn(0x1004, library.charge)


class RunBoundTests(unittest.TestCase):
    """No run of code is longer than the budget pays for (RUN_LIMIT)."""

    def test_long_straight_code_is_split(self) -> None:
        words = [NOP] * 200 + [RET]
        library = library_of(words, [(0, 4 * len(words))])
        points = sorted(library.charge)
        self.assertGreater(len(points), 200 // rewrite.RUN_LIMIT)
        for offset, cost in library.charge.items():
            self.assertLessEqual(cost, rewrite.RUN_LIMIT + 1)
        # The gaps between charge points are at most RUN_LIMIT words.
        edges = points + [0x1000 + 4 * len(words)]
        for before, after in zip(edges, edges[1:]):
            self.assertLessEqual((after - before) // 4, rewrite.RUN_LIMIT + 1)

    def test_a_return_loop_pays_for_what_it_executes(self) -> None:
        # Forty instructions and a RET that returns to the start: the
        # reviewed case that retired 42,000 instructions between polls
        # while charging four a lap. Every lap pays for the run it landed
        # on, plus the RET.
        words = [NOP] * 40 + [RET]
        library = library_of(words, [(0, 4 * len(words))])
        unit = unit_words(library)
        self.assertIn(A.add_imm(rewrite.BUD, rewrite.BUD,
                                rewrite.RUN_LIMIT + 1, sub=True), unit)
        # The lap itself crosses charge points that cover its 41 words.
        paid = sum(library.charge[offset] for offset in library.charge)
        self.assertGreaterEqual(paid, len(words))


class EntryTableTests(unittest.TestCase):
    """The entry table is 16 bits per source word and an anchor per text page:
    the words have entries, they are reachable from their page's anchor, a
    page of a long function's end is a row of branches, and a table that
    cannot be formed leaves the library to the engine."""

    @classmethod
    def setUpClass(cls) -> None:
        import tempfile
        cls.directory = tempfile.TemporaryDirectory()
        cls.path = pathlib.Path(cls.directory.name) / "libaotfixture.so"
        cls.path.write_bytes(fixture.build())
        cls.layout = rewrite.read_layout(rewrite.LAYOUT_HEADER)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def unit(self) -> dict:
        return rewrite.build_unit(
            (0, "/aot-test/libaotfixture.so", str(self.path), False))

    @staticmethod
    def entries(unit: dict) -> dict[int, int]:
        at, words = unit["table_at"], unit["table_words"]
        len16 = (2 * words + 4095) & ~4095
        data = unit["unit"]
        out = {}
        for word in range(words):
            (entry,) = struct.unpack_from("<H", data, at + 2 * word)
            if entry:
                (anchor,) = struct.unpack_from(
                    "<i", data, at + len16 + 4 * (word // 1024))
                out[word] = at + len16 + anchor + 4 * entry
        return out

    def test_the_words_have_entries_inside_the_unit(self) -> None:
        unit = self.unit()
        found = self.entries(unit)
        self.assertGreater(len(found), 100)
        for address in found.values():
            self.assertEqual(address % 4, 0)
            self.assertLess(address, unit["table_at"])
        # The table is the 16-bit words, padded to 4 KiB, and the anchors.
        words = unit["table_words"]
        self.assertEqual(unit["table_region"],
                         ((2 * words + 4095) & ~4095) +
                         4 * ((words + 1023) // 1024))
        self.assertLess(unit["table_region"], 4 * words)

    def test_the_census_the_magic_and_the_header_follow(self) -> None:
        unit = self.unit()
        census = unit["stats"]["census"]
        self.assertEqual(sum(census.values()), len(unit["unit"]))
        self.assertEqual(census["other"], 0)
        self.assertEqual(census["table"], unit["table_region"])
        blob, manifest = rewrite.build_blob(
            [unit], self.layout, "entry-table-row", False, **RELEASE)
        self.assertEqual(blob[:8], b"UMLAOT16")
        self.assertEqual(struct.unpack_from("<I", blob, 8)[0],
                         rewrite.BLOB_VERSION)
        self.assertEqual(struct.unpack_from("<I", blob, 36)[0], 0)
        self.assertEqual(manifest["refused"], [])
        # The release the blob belongs to: ID, manifest digest, system digest.
        self.assertEqual(blob[0x30:0x30 + 96].rstrip(b"\0"),
                         b"entry-table-row")
        self.assertEqual(blob[0x90:0xB0], bytes.fromhex(RELEASE[
            "manifest_sha256"]))
        self.assertEqual(blob[0xB0:0xD0], bytes.fromhex(RELEASE[
            "system_sha256"]))
        self.assertEqual(manifest["release_manifest_sha256"],
                         RELEASE["manifest_sha256"])
        with self.assertRaises(ValueError):
            rewrite.build_blob([unit], self.layout, "short-digest", False,
                               manifest_sha256="ab" * 31,
                               system_sha256=RELEASE["system_sha256"])

    def test_a_page_that_holds_a_long_function_s_end_is_a_row_of_branches(
            self) -> None:
        # A function of WIDE16_FUNCTION bytes and a short one behind it in
        # the same page: the first one's out-of-line stubs lie between their
        # entries, so the page's entries go through a row of branches.
        n = rewrite.WIDE16_FUNCTION // 4 + 5
        words = [NOP] * (n - 1) + [RET, NOP, RET]
        functions = [(0, 4 * n), (4 * n, 4 * n + 8)]
        digests = [1] * ((4 * len(words) + 4095) // 4096)
        wide = library_of(words, functions)
        built = wide.build(digests)
        page = (4 * n - 4) // 4096
        self.assertEqual(wide.wide16, (page,))
        unit = {"unit": built["unit"], "table_at": built["table_at"],
                "table_words": wide.table_words}
        found = self.entries(unit)
        self.assertEqual(sorted(found), sorted(
            (offset - wide.table_base) // 4
            for offset in wide.entry_labels))
        row_start = built["table_at"] - 4096
        final = {}
        for word, address in found.items():
            if address >= row_start:
                (value,) = struct.unpack_from("<I", built["unit"], address)
                self.assertEqual(value >> 26, 0b000101)
                delta = value & 0x3ffffff
                delta -= (1 << 26) if delta >> 25 else 0
                address += 4 * delta
            self.assertLess(address, row_start)
            self.assertEqual(address % 4, 0)
            final[word] = address
        # The words are in address order, so their native code is too.
        ordered = [final[word] for word in sorted(final)]
        self.assertEqual(ordered, sorted(ordered))
        self.assertEqual(wide.stats.census["other"], 0)
        self.assertEqual(sum(wide.stats.census.values()), len(built["unit"]))

    def test_a_page_whose_entries_are_too_far_apart_is_refused(self) -> None:
        library = library_of([NOP, RET], [(0, 8)])
        unit = bytearray(1 << 20)
        reach = rewrite.ENTRY16_REACH
        self.assertEqual(reach, 4 * 65535)
        with self.assertRaises(rewrite.EntryTableRefused):
            library.fill_entry16(unit, 0, {0x1000: 4, 0x1004: reach + 4})
        library.fill_entry16(unit, 0, {0x1000: 4, 0x1004: reach})

    def test_a_table_too_large_for_one_add_is_refused(self) -> None:
        # 16 MiB of 16-bit entries: 8M source words, 32 MiB of text.
        words = 1 << 23
        library = library_of([NOP, RET], [(0, 8)])
        library.table_words = words
        with self.assertRaises(rewrite.EntryTableRefused):
            rewrite._entry16_jump(A.Assembler(), library, 1, 2, 3)

    def test_a_refused_library_leaves_the_blob_with_a_count(self) -> None:
        with mock.patch.object(rewrite, "ENTRY16_REACH", 8):
            refused = self.unit()
        self.assertIn("span more than 256 KiB", refused["refused"])
        self.assertEqual(refused["path"], "/aot-test/libaotfixture.so")
        good = self.unit()
        good["index"] = 2
        refused["index"] = 1
        first = dict(good, index=0, path="/first", sha256="11" * 32)
        kept, left = rewrite.split_refused([good, refused, first])
        self.assertEqual([(unit["index"], unit["path"]) for unit in kept],
                         [(0, "/first"), (1, good["path"])])
        self.assertEqual(left, [refused])
        blob, manifest = rewrite.build_blob(
            kept, self.layout, "refused-row", False,
            [{"path": unit["path"], "reason": unit["refused"]}
             for unit in left], **RELEASE)
        self.assertEqual(struct.unpack_from("<I", blob, 16)[0], 2)
        self.assertEqual(struct.unpack_from("<I", blob, 36)[0], 1)
        self.assertEqual([entry["path"] for entry in manifest["refused"]],
                         [refused["path"]])
        self.assertEqual(len(manifest["libraries"]), 2)

    def test_the_new_instructions_encode_as_the_assembler_does(self) -> None:
        self.assertEqual(A.ldrh_reg(1, 2, 3), 0x78637841)
        self.assertEqual(A.ldrsw_reg(4, 5, 6), 0xb8a678a4)


class CountingLayoutTests(unittest.TestCase):
    """The counting layout (UML_AOT_COUNT_RETIRED) builds and counts.

    The differential test's independent bound on an entry reads the exact
    count only from this layout, which no preset builds. These rows keep the
    layout assembling on a library, keep it different from the product's in
    exactly the counter's words, and tie the test's bound to the rewriter's
    run limit, so the bound cannot go stale unnoticed.
    """

    @classmethod
    def setUpClass(cls) -> None:
        import tempfile
        cls.directory = tempfile.TemporaryDirectory()
        cls.path = pathlib.Path(cls.directory.name) / "libaotfixture.so"
        cls.path.write_bytes(fixture.build())
        cls.layout = rewrite.read_layout(rewrite.LAYOUT_HEADER)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def unit(self, count: bool) -> dict:
        return rewrite.build_unit(
            (0, "/aot-test/libaotfixture.so", str(self.path), count))

    @staticmethod
    def words(code: bytes) -> list[int]:
        return list(struct.unpack(f"<{len(code) // 4}I", code))

    def test_every_instruction_pays_three_words_to_count(self) -> None:
        product = self.unit(False)
        counting = self.unit(True)
        instructions = counting["stats"]["instructions"]
        self.assertGreater(instructions, 40)
        self.assertEqual(product["stats"]["instructions"], instructions)
        self.assertEqual(product["stats"]["words"].get("count", 0), 0)
        self.assertEqual(counting["stats"]["words"]["count"], 3 * instructions)
        # Every counter word has a census kind, so the unit builds at all.
        self.assertEqual(product["stats"]["census"]["count"], 0)
        self.assertEqual(counting["stats"]["census"]["count"],
                         12 * instructions)
        self.assertGreater(len(counting["unit"]), len(product["unit"]))
        load = A.ldr_x(rewrite.T2, 31, rewrite.SLOT_COUNT)
        self.assertNotIn(load, self.words(product["unit"]))
        self.assertGreaterEqual(self.words(counting["unit"]).count(load),
                                instructions)

    def test_the_entry_zeroes_and_reports_the_count(self) -> None:
        for count in (False, True):
            blob, manifest = rewrite.build_blob(
                [self.unit(count)], self.layout, "counting-row", count,
                **RELEASE)
            words = self.words(blob)
            zero = A.str_x(31, 31, rewrite.SLOT_COUNT)
            report = A.ldr_x(1, 31, rewrite.SLOT_COUNT)
            budget = A.ldr_x(rewrite.TLB, 31, rewrite.SLOT_BUD0)
            self.assertEqual(zero in words, count)
            self.assertEqual(report in words, count)
            self.assertEqual(budget in words, not count)

    def test_the_tests_bound_is_the_rewriters_run_limit(self) -> None:
        source = (pathlib.Path(__file__).resolve().parents[3] / "tests" /
                  "unit" / "arm64" /
                  "arm64_aot_differential_tests.cpp").read_text()
        match = re.search(
            r"constexpr std::uint64_t entry_bound = 4096 \+ (\d+) \+ (\d+);",
            source)
        self.assertIsNotNone(match)
        self.assertEqual(int(match.group(1)), rewrite.RUN_LIMIT)
        # The charge point's own word and the entry's last instruction.
        self.assertGreaterEqual(int(match.group(2)), 2)


class SameFileTwiceTests(unittest.TestCase):
    """A unit binds by the content of its pages, not by its path. A file
    shipped in several places (an APEX copy) is one unit: listed under two
    paths, every digest of its pages would be shared by two units and leave
    the digest hash, and neither unit would ever bind."""

    @classmethod
    def setUpClass(cls) -> None:
        import tempfile
        cls.directory = tempfile.TemporaryDirectory()
        base = pathlib.Path(cls.directory.name)
        (base / "a.so").write_bytes(fixture.build())
        (base / "b.so").write_bytes(fixture.build())
        cls.paths = {"a": base / "a.so", "b": base / "b.so"}
        cls.layout = rewrite.read_layout(rewrite.LAYOUT_HEADER)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def unit(self, index: int, name: str, which: str) -> dict:
        return rewrite.build_unit(
            (index, name, str(self.paths[which]), False))

    def test_the_same_file_under_two_paths_is_refused(self) -> None:
        units = [self.unit(0, "/apex/a/lib64/libx.so", "a"),
                 self.unit(1, "/apex/b/lib64/libx.so", "b")]
        with self.assertRaises(ValueError) as raised:
            rewrite.build_blob(units, self.layout, "same-twice", **RELEASE)
        self.assertIn("list it once", str(raised.exception))

    def test_a_unit_names_every_page_it_can_bind(self) -> None:
        blob, manifest = rewrite.build_blob(
            [self.unit(0, "/apex/a/lib64/libx.so", "a")], self.layout,
            "once", **RELEASE)
        library = manifest["libraries"][0]
        self.assertGreater(library["pages"], 0)
        self.assertEqual(library["bindable_pages"], library["pages"])


class StackAlignmentTests(unittest.TestCase):
    """AOT code runs with a 16-byte aligned SP, as the engine checks every
    SP-based access: an instruction that can leave SP misaligned is followed
    by a test that ends the entry."""

    GUARD = A.ubfx(27, 26, 0, 4)  # ubfx x27, x26, #0, #4 (T1 = T0 & 15)

    def test_mov_to_sp_is_checked(self) -> None:
        mov_sp = 0x9100001f | 1 << 5  # mov sp, x1 (add sp, x1, #0)
        unit = unit_words(library_of([mov_sp, RET], [(0, 8)]))
        self.assertIn(self.GUARD, unit)

    def test_aligned_adjustment_of_sp_is_not_checked(self) -> None:
        sub_sp = 0xd10043ff  # sub sp, sp, #16
        unit = unit_words(library_of([sub_sp, RET], [(0, 8)]))
        self.assertNotIn(self.GUARD, unit)

    def test_odd_adjustment_of_sp_is_checked(self) -> None:
        sub_sp = 0xd10023ff  # sub sp, sp, #8
        unit = unit_words(library_of([sub_sp, RET], [(0, 8)]))
        self.assertIn(self.GUARD, unit)

    def test_sp_stays_unchecked_for_a_plain_load(self) -> None:
        ldr = 0xf94003e0  # ldr x0, [sp]
        unit = unit_words(library_of([ldr, RET], [(0, 8)]))
        self.assertNotIn(self.GUARD, unit)


class SimdStructureTests(unittest.TestCase):
    """Advanced SIMD structure loads and stores are rewritten unless the
    base is SP or the word is reserved."""

    def test_only_sp_and_reserved_rows_step(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        for index, (name, _) in enumerate(fixture.simd_rows()):
            cause = causes.get(fixture.SIMD_AT + index * fixture.SIMD_STEP)
            if "_sp" in name:
                self.assertEqual(cause, "simd-struct-sp", name)
            elif "reserved" in name:
                self.assertIsNotNone(cause, name)
            else:
                self.assertIsNone(cause, name)

    def test_the_warm_rows_do_not_step(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        for index, (name, _) in enumerate(fixture.simd_warm_rows()):
            base = fixture.SIMD_WARM_AT + index * fixture.SIMD_WARM_STEP
            self.assertIsNone(causes.get(base), name)
            self.assertIsNone(causes.get(base + 4), name)

    def test_the_rows_fit_between_the_mops_rows_and_the_edges(self) -> None:
        end = fixture.SIMD_WARM_AT + fixture.SIMD_WARM_STEP * len(
            fixture.simd_warm_rows())
        self.assertLessEqual(end, min(fixture.EDGES.values()))
        self.assertGreaterEqual(fixture.SIMD_AT,
                                fixture.MOPS_AT + fixture.MOPS_STEP *
                                len(fixture.MOPS_ROWS))


class LargeLibraryTests(unittest.TestCase):
    """A library's text is not capped at 16 MiB: a page state beyond the 12
    bits of a load's offset costs the gates that name it one more word."""

    NOP_WORD = NOP

    def gate_words(self, page: int) -> list[int]:
        return [A.ldr_x(rewrite.T0, 31, rewrite.SLOT_PSTATE)] + (
            [A.add_imm(rewrite.T0, rewrite.T0, page >> 12, shift12=True)]
            if page >= 4096 else []) + [
            A.ldrb_w(rewrite.T0, rewrite.T0, page & 0xfff)]

    def contains(self, unit: list[int], words: list[int]) -> bool:
        return any(unit[i:i + len(words)] == words
                   for i in range(len(unit) - len(words) + 1))

    def test_a_gate_to_a_page_beyond_the_load_offset_adds_a_word(self) -> None:
        far = 17 * 1024 * 1024
        words = [NOP] * (far // 4 + 4)
        # A function at the start branches to one in the 4097th page.
        target = 4097 * 4096
        words[0] = 0x14000000 | (target >> 2)
        words[target // 4] = NOP
        words[target // 4 + 1] = RET
        library = library_of(words, [(0, 4), (target, target + 8)])
        unit = unit_words(library)
        self.assertTrue(self.contains(unit, self.gate_words(4097)))
        self.assertFalse(self.contains(unit, [
            A.ldr_x(rewrite.T0, 31, rewrite.SLOT_PSTATE),
            A.ldrb_w(rewrite.T0, rewrite.T0, 4097 & 0xfff)]))

    def test_a_gate_below_the_offset_keeps_its_three_words(self) -> None:
        target = 4095 * 4096
        words = [NOP] * (target // 4 + 4)
        words[0] = 0x14000000 | (target >> 2)
        words[target // 4 + 1] = RET
        library = library_of(words, [(0, 4), (target, target + 8)])
        unit = unit_words(library)
        self.assertTrue(self.contains(unit, self.gate_words(4095)))

    def test_text_above_16_mib_in_the_address_space_builds(self) -> None:
        library = rewrite.Library(
            0, "/test/lib.so", FakeElf([RET, NOP], [(0, 4)], base=0x2000000),
            a64decode.Decoder(), rewrite.read_layout(rewrite.LAYOUT_HEADER))
        unit = unit_words(library)
        # REFILL_FAIL and IND form the base of the code in a register.
        self.assertTrue(self.contains(unit, [
            A.movz(rewrite.T0, 0), A.movk(rewrite.T0, 0x200, 1)]))
        self.assertTrue(self.contains(unit, [
            A.movz(rewrite.T2, 0), A.movk(rewrite.T2, 0x200, 1),
            A.add_reg(rewrite.T1, rewrite.T1, rewrite.T2, sub=True)]))


class CensusTests(unittest.TestCase):
    """The byte census names the kind of every word of a unit."""

    LDR = 0xf9400020      # ldr x0, [x1]
    STR = 0xf9000022      # str x2, [x1]
    ADD = 0x8b020020      # add x0, x1, x2

    def census(self) -> tuple[dict, int]:
        library = library_of([self.LDR, self.STR, self.ADD, RET],
                             [(0, 16)])
        unit = unit_words(library)
        return library.stats.census, 4 * len(unit)

    def test_the_kinds_add_up_to_the_unit(self) -> None:
        census, size = self.census()
        self.assertEqual(sum(census.values()), size)
        self.assertEqual(census["other"], 0)

    def test_a_load_and_a_store_are_a_probe_an_access_and_a_stub(self) -> None:
        census, _ = self.census()
        # A probe of a wide scalar is seven words: the untag, the last byte,
        # the index, the entry address, the entry load, the compare with the
        # first byte's page and the branch to its cold stub of two words.
        self.assertEqual(census["mem.probe"], 2 * 7 * 4)
        self.assertEqual(census["mem.access"], 2 * 4)
        self.assertEqual(census["stub.mm"], 2 * 2 * 4)
        self.assertEqual(census["insn"], 4)
        # One text page of entries (its 16-bit words padded to 4 KiB) and
        # the page's anchor.
        self.assertEqual(census["table"], 4096 + 4)

    def test_only_the_refill_routines_a_site_enters_exist(self) -> None:
        census, _ = self.census()
        # A load and a store of eight bytes: the last byte's address (the
        # probe left the first), a movz and a branch each.
        self.assertEqual(census["stub.refill"], 2 * 3 * 4)

    def test_a_function_that_falls_off_its_end_tags_the_branch(self) -> None:
        library = library_of([self.ADD, self.ADD], [(0, 8)])
        unit = unit_words(library)
        self.assertEqual(sum(library.stats.census.values()), 4 * len(unit))
        self.assertEqual(library.stats.census["other"], 0)

    def test_nothing_is_untagged_in_a_whole_library(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        unit = library.build(
            [1] * ((library.table_end - library.table_base) // 4096))["unit"]
        census = library.stats.census
        self.assertEqual(sum(census.values()), len(unit))
        self.assertEqual(census["other"], 0)

    def test_a_build_with_an_untagged_word_is_refused(self) -> None:
        # Every framework build holds the census invariant, not only this
        # test: an emitter that stops tagging its words fails the build.
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        original = rewrite.Library.tag

        def skip_exit_stubs(self, kind, start):
            if kind != "stub.xu":
                original(self, kind, start)

        with mock.patch.object(rewrite.Library, "tag", skip_exit_stubs):
            with self.assertRaises(RuntimeError) as raised:
                library.build([1] * ((library.table_end -
                                      library.table_base) // 4096))
        self.assertIn("no census kind", str(raised.exception))

    def test_functions_that_leave_for_one_target_share_its_stub(self) -> None:
        # Three functions, each ending in a branch to the same uncovered
        # address (and a fourth to another one): one stub per target.
        def b(from_word: int, to_offset: int) -> int:
            return 0x14000000 | (((to_offset - 4 * from_word) >> 2) & 0x3ffffff)
        words = [b(0, 0x100), b(1, 0x100), b(2, 0x100), b(3, 0x104)]
        library = library_of(words, [(0, 4), (4, 4), (8, 4), (12, 4)])
        unit_words(library)
        # movz + b, per distinct target.
        self.assertEqual(library.stats.census["stub.xb"], 2 * 2 * 4)

    def test_a_second_build_starts_with_no_shared_stubs(self) -> None:
        def b(from_word: int, to_offset: int) -> int:
            return 0x14000000 | (((to_offset - 4 * from_word) >> 2) & 0x3ffffff)
        words = [b(0, 0x100), b(1, 0x100)]
        library = library_of(words, [(0, 4), (4, 4)])
        first = unit_words(library)
        second = unit_words(library)
        self.assertEqual(first, second)

    def test_a_text_page_is_stored_as_its_sha256(self) -> None:
        library = library_of([self.ADD, RET], [(0, 8)])
        pages = (library.table_end - library.table_base) // 4096
        self.assertEqual(pages, 1)
        source = bytes(range(256)) * 16
        unit = library.build([1] * pages, source)
        self.assertEqual(library.stats.census["src"], 32)
        stored = unit["unit"][unit["hashes_at"]:unit["hashes_at"] + 32]
        self.assertEqual(stored, hashlib.sha256(source).digest())
        self.assertEqual(len(unit["unit"]) - unit["hashes_at"], 32)

    def test_every_page_has_its_own_hash_in_page_order(self) -> None:
        source = b"".join(bytes([n]) * 4096 for n in range(5))
        hashes = rewrite.page_hashes(source)
        self.assertEqual(len(hashes), 5 * rewrite.PAGE_HASH_BYTES)
        for n in range(5):
            self.assertEqual(
                hashes[32 * n:32 * n + 32],
                hashlib.sha256(bytes([n]) * 4096).digest())

    def test_one_bit_in_a_page_changes_its_hash(self) -> None:
        page = bytearray(range(256)) * 16
        before = rewrite.page_hashes(bytes(page))
        for offset in (0, 1, 2047, 2048, 4094, 4095):
            changed = bytearray(page)
            changed[offset] ^= 0x10
            self.assertNotEqual(rewrite.page_hashes(bytes(changed)), before)

    def test_a_partial_page_of_source_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            rewrite.page_hashes(bytes(100))


class WideningTests(unittest.TestCase):
    """Ordered accesses, atomics and the MOPS stages that rewritten code
    runs itself, and what is left to the engine."""

    # Words an assembler produced for the encoders added with them.
    ENCODINGS = {
        "and x2, x1, #0x00ffffffffffffff": (A.and_ones(2, 1, 56), 0x9240dc22),
        "orr x26, xzr, #0x7fffffffffffffff":
            (A.orr_ones(26, 31, 63), 0xb240fbfa),
        "orr x26, xzr, #0x007fffffffffffff":
            (A.orr_ones(26, 31, 55), 0xb240dbfa),
        "cmp x1, x2": (A.cmp_reg(1, 2), 0xeb02003f),
        "csel x1, x1, x26, ls": (A.csel(1, 1, 26, 9), 0x9a9a9021),
        "ccmp x28, #0, #4, lo": (A.ccmp_imm(28, 0, 4, 3), 0xfa403b84),
        "ldar w3, [x28]": (fixture.ordered(True, 2, 28, 3), 0x88dfff83),
        "stlrh w4, [x28]": (fixture.ordered(False, 1, 28, 4), 0x489fff84),
        "ldaddal x26, x27, [x28]":
            (fixture.amo(3, 1, 1, 26, 0, False, 28, 27), 0xf8fa039b),
        "swplh w26, w27, [x28]":
            (fixture.amo(1, 0, 1, 26, 0, True, 28, 27), 0x787a839b),
        "casa x26, x27, [x28]": (fixture.cas(3, 1, 0, 26, 28, 27), 0xc8fa7f9b),
        "casah w26, w27, [x28]":
            (fixture.cas(1, 1, 0, 26, 28, 27), 0x48fa7f9b),
        "ld1 {v0.16b}, [x1]": (fixture.simd_multi(True, 1, 7, 0, 1, 0),
                               0x4c407020),
        "ld1 {v0.16b}, [x1], #16": (fixture.simd_multi(True, 1, 7, 0, 1, 0,
                                                       31), 0x4cdf7020),
        "ld1 {v0.16b, v1.16b}, [x1], x2":
            (fixture.simd_multi(True, 1, 10, 0, 1, 0, 2), 0x4cc2a020),
        "ld4 {v0.4s-v3.4s}, [x1], #64":
            (fixture.simd_multi(True, 1, 0, 2, 1, 0, 31), 0x4cdf0820),
        "st3 {v0.8h-v2.8h}, [x1]": (fixture.simd_multi(False, 1, 4, 1, 1, 0),
                                    0x4c004420),
        "ld1 {v0.s}[1], [x1], #4":
            (fixture.simd_single(True, 0, 0, 4, 1, 0, 1, 0, 31),
             0x0ddf9020),
        "ld1r {v0.16b}, [x1], #1":
            (fixture.simd_single(True, 1, 0, 6, 0, 0, 1, 0, 31), 0x4ddfc020),
        "ld1r {v0.2d}, [x1], x2":
            (fixture.simd_single(True, 1, 0, 6, 0, 3, 1, 0, 2), 0x4dc2cc20),
        "ld2 {v0.h, v1.h}[3], [x1]":
            (fixture.simd_single(True, 0, 1, 2, 1, 2, 1, 0), 0x0d605820),
        "cpyfp [x0]!, [x1]!, x2!": (fixture.mops("cpyf", 0, 0, 0, 2, 1),
                                    0x19010440),
        "cpym [x0]!, [x1]!, x2!": (fixture.mops("cpy", 1, 0, 0, 2, 1),
                                   0x1d410440),
        "cpye [x0]!, [x1]!, x2!": (fixture.mops("cpy", 2, 0, 0, 2, 1),
                                   0x1d810440),
        "cpypwn [x0]!, [x1]!, x2!": (fixture.mops("cpy", 0, 4, 0, 2, 1),
                                     0x1d014440),
        "setp [x0]!, x1!, x2": (fixture.mops("set", 0, 0, 0, 1, 2),
                                0x19c20420),
        "setm [x0]!, x1!, x2": (fixture.mops("set", 1, 0, 0, 1, 2),
                                0x19c24420),
        "setptn [x0]!, x1!, x2": (fixture.mops("set", 0, 3, 0, 1, 2),
                                  0x19c23420),
        # The page-edge functions of the fixture, from TARGET = 0x4100.
        "b target": (fixture.b(0x3004, 0x4100), 0x1400043f),
        "bl target": (fixture.bl(0x3024, 0x4100), 0x94000437),
        "b.eq target": (fixture.b_cond(0, 0x3048, 0x4100), 0x540085c0),
        "cbz x2, target": (fixture.cbz(2, 0x3068, 0x4100), 0xb40084c2),
        "tbz x2, #0, target": (fixture.tbz(2, 0, 0x3088, 0x4100),
                               0x360083c2),
        "adr x16, target": (fixture.adr(16, 0x30a4, 0x4100), 0x100082f0),
        "adr x30, target": (fixture.adr(30, 0x30e4, 0x4100), 0x100080fe),
    }

    def test_encoders_match_an_assembler(self) -> None:
        for text, (made, assembled) in self.ENCODINGS.items():
            self.assertEqual(made, assembled, f"{text}: {made:#x}")

    def test_shared_memory_loops_match_an_assembler(self) -> None:
        # Words from the system assembler for the loops the differential test
        # runs on one shared cell (fixture.hammer_code).
        code = fixture.hammer_code()
        add, cas, llsc = (fixture.HAMMER_ADD, fixture.HAMMER_CAS,
                          fixture.HAMMER_LLSC)
        expected = {
            add: 0xd2800023, add + 4: 0xf8230044, add + 8: 0xf1000400,
            add + 12: 0x54ffffc1, add + 16: 0xd65f03c0,
            cas: 0xf9400045, cas + 4: 0x910004a6, cas + 8: 0xaa0503e7,
            cas + 12: 0xc8a77c46, cas + 16: 0xeb0500ff,
            cas + 20: 0x54ffff61, cas + 24: 0xf1000400,
            cas + 28: 0x54ffff21, cas + 32: 0xd65f03c0,
            llsc: 0xc85f7c45, llsc + 4: 0x910004a5, llsc + 8: 0xc8067c45,
            llsc + 12: 0x35ffffa6, llsc + 16: 0xf1000400,
            llsc + 20: 0x54ffff61, llsc + 24: 0xd65f03c0}
        self.assertEqual(code, expected)

    def test_memory_copy_and_set_rows_match_an_assembler(self) -> None:
        # The rows the differential test starts memory copies and sets from
        # (fixture.mops_code), as the system assembler wrote the stages of
        # CPYF, CPY and SET with registers 0, 2, 1, then 17, 16, 15, and a
        # SET whose fill is the zero register.
        expected = {}
        for row, words in enumerate((
                (0x19010440, 0x19410440, 0x19810440),
                (0x1d010440, 0x1d410440, 0x1d810440),
                (0x19c10440, 0x19c14440, 0x19c18440),
                (0x190f0611, 0x194f0611, 0x198f0611),
                (0x1d0f0611, 0x1d4f0611, 0x1d8f0611),
                (0x19cf0611, 0x19cf4611, 0x19cf8611),
                (0x19df0440, 0x19df4440, 0x19df8440))):
            base = fixture.MOPS_AT + row * fixture.MOPS_STEP
            for stage, word in enumerate(words):
                expected[base + 4 * stage] = word
            expected[base + 12] = 0xd65f03c0
        self.assertEqual(fixture.mops_code(), expected)
        # Past the shared-memory loops, before the page edges.
        self.assertGreater(fixture.MOPS_AT, fixture.HAMMER_AT + 0x80)
        self.assertLess(fixture.MOPS_AT + len(fixture.MOPS_ROWS) *
                        fixture.MOPS_STEP, fixture.SRC_PAGE)

    def test_the_differential_test_finds_the_forms_it_names(self) -> None:
        # The test addresses cas_<size>_<ordering> by arithmetic.
        names = [name for name, _ in fixture.forms()]
        for size in range(4):
            for order in range(4):
                self.assertEqual(
                    names[177 + 2 * (size * 4 + order)],
                    f"cas_{size}_{order}")
        self.assertLessEqual(
            fixture.FORMS_AT + len(names) * fixture.FORM_SIZE,
            fixture.HAMMER_AT)

    def test_only_the_exclusive_loop_leaves_rewritten_code(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        stepped = sorted(offset for offset, _ in library.step_list
                         if fixture.HAMMER_AT <= offset < fixture.HAMMER_AT
                         + 0x100)
        self.assertEqual(stepped, [fixture.HAMMER_LLSC,
                                   fixture.HAMMER_LLSC + 8])

    def test_what_steps_is_what_the_engine_must_run(self) -> None:
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        stepped: dict[str, set[str]] = {}
        for index, (name, words) in enumerate(fixture.forms()):
            base = fixture.FORMS_AT + index * fixture.FORM_SIZE
            for position in range(len(words)):
                cause = causes.get(base + 4 * position)
                if cause is not None:
                    stepped.setdefault(name, set()).add(cause)
        expected = {}
        for name, _ in fixture.forms():
            if name.endswith("_sp"):
                expected[name] = {
                    "ordered-sp" if name[:4] in ("ldar", "stlr") else
                    "atomic-sp"}
        for name, cause in (("ldapr", "memop:LDAPR:FEAT_LRCPC"),
                            ("ldapr_b", "memop:LDAPRB:FEAT_LRCPC"),
                            ("ldlar", "ldstord:LDLAR:FEAT_LOR"),
                            ("stllr", "ldstord:STLLR:FEAT_LOR")):
            expected[name] = {cause}
        for kind in ("cpyf", "cpy", "set"):
            for name in ("same", "xzr_d"):
                expected[f"{kind}_{name}"] = {"mops-invalid"}
        expected["cpyf_zs"] = {"mops-invalid"}
        expected["cpy_zs"] = {"mops-invalid"}
        # The epilogue is carried out by a thunk unless an operand lives in a
        # frame slot; the prologue of a copy with two of them steps.
        for kind in ("cpyf", "cpy", "set"):
            for name in ("vn", "vd", "vs", "vdvn", "vnvs"):
                expected[f"{kind}_{name}"] = {"mops-epilogue-virtual"}
        expected["isb_mov"] = {"barriers:ISB:baseline"}
        expected["cpy_vdvn"] = {"mops-virtual", "mops-epilogue-virtual"}
        expected["cpy_vnvs"] = {"mops-virtual", "mops-epilogue-virtual"}
        # "xzr_d" is parsed as ("xzr", "d").
        for kind in ("cpyf", "cpy", "set"):
            expected[f"{kind}_xzr_d"] = {"mops-invalid"}
        self.assertEqual(stepped, expected)


    def test_stack_pairs_with_plain_data_registers_are_inlined(self) -> None:
        # The fixture's stack-base pairs with plain data registers (inside one
        # aligned granule and outside it) run in the rewritten code, so the
        # differential exercises the stack-granule probe on pairs.
        elf = elf64.Elf(fixture.build())
        library = rewrite.Library(
            0, "/aot-test/libaotfixture.so", elf,
            a64decode.Decoder(a64decode.DEFAULT_TABLE),
            rewrite.read_layout(rewrite.LAYOUT_HEADER))
        library.build([1] * ((library.table_end - library.table_base) // 4096))
        causes = dict(library.step_list)
        seen = 0
        for index, (name, words) in enumerate(fixture.pair_forms()):
            if "_sp_pp_" not in name and not name.endswith("_sp_zz"):
                continue
            seen += 1
            base = fixture.PAIRS_AT + index * fixture.PAIR_SIZE
            for position in range(len(words)):
                self.assertNotIn(base + 4 * position, causes, name)
        self.assertEqual(seen, 17)

if __name__ == "__main__":
    unittest.main()
