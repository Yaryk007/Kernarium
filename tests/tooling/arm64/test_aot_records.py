#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The records of the AOT guest debug program: their encoding, the one reader
of it, and the guest program's emitter against that reader.

tests/integration/aot/records.py is the model (a record is a kind and
fields; a value is a number, a place or a text) and the one reader of the wire
form. tests/integration/aot/guest_records.hpp is the emitter the guest program
uses. The guest runs only in a guest, so the header is built into a host
program (aot_records_harness.cpp) and given the same inputs as the Python
encoder: what the two write for a value must be the same line, and what the
reader decodes from the emitter's lines must be the value that was given,
however hostile the bytes of a name are. The rows after that judge the
emitter's whole output (a maps text too long for the program's buffer, an
error in the middle of a record) through the runner's own judgement of a run
(debug_rows.run_mode).
"""

__import__("sys").dont_write_bytecode = True

import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
AOT = HERE.parents[1] / "integration" / "aot"
sys.path.insert(0, str(AOT))
sys.path.insert(0, str(HERE))
import debug_rows as rows  # noqa: E402
import records as rec  # noqa: E402
from records import Named, Record, RecordError, Unnamed  # noqa: E402
import test_aot_debug_rows as fixture  # noqa: E402

PAGE = 0x1000
Mapping = tuple[int, int, int, str, str]  # low, high, file offset, name, protection
TAB = "\t"


def maps_text(mappings: list[Mapping]) -> bytes:
    """A layout as the kernel writes /proc/<pid>/maps: a name is separated
    from the inode by padding, an unnamed mapping's line ends at the inode."""
    lines = []
    for low, high, offset, name, protection in mappings:
        line = f"{low:012x}-{high:012x} {protection}p {offset:08x} 00:00 0"
        lines.append(line + (" " * 26 + name if name else ""))
    return ("\n".join(lines) + "\n").encode("utf-8", "surrogateescape")


def probe_schema(kind_type: str) -> dict:
    return {"probe": (("value", kind_type),)}


def line_of(kind_type: str, value) -> str:
    return rec.encode("probe", "value", kind_type, value)


class EncodingTest(unittest.TestCase):
    """The reader, on lines written by hand."""

    def decode_one(self, line: str, kind_type: str):
        records = rec.decode([line], probe_schema(kind_type))
        self.assertEqual(len(records), 1)
        return records[0].get("value")

    def refused(self, line: str, kind_type: str = "v") -> None:
        with self.assertRaises(RecordError, msg=line):
            self.decode_one(line, kind_type)

    def test_each_type_decodes_to_its_value(self):
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\tn\t0", "n"), 0)
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\tn\tffffffffffffffff", "n"), (1 << 64) - 1)
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\ti\t-5", "i"), -5)
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\tt\tlibz.so", "t"), "libz.so")
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\tt\ta%20b%3dc", "t"), "a b=c")
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\tm\tlibz.so\t12190", "v"), Named("libz.so", 0x12190))
        self.assertEqual(self.decode_one(f"AOT\tprobe\tvalue\tu\t100000\trw-\t1\t3\t1234", "v"),
                         Unnamed(0x100000, "rw-", 1, 3, 0x1234))

    def test_numbers_are_exact(self):
        for bad in ("", "0x", "0x0", "00", "07", "0X1", "1F", "g", " 1", "1 ", "-1", "+1", "10000000000000000",
                    "1" * 17):
            with self.subTest(number=bad):
                self.refused(f"AOT\tprobe\tvalue\tn\t{bad}", "n")

    def test_signed_numbers_are_exact(self):
        for bad in ("", "+5", "-0", "05", "-05", "1e3", "0x5", "5 ", "--5", "9223372036854775808",
                    "-9223372036854775809", "1.0"):
            with self.subTest(number=bad):
                self.refused(f"AOT\tprobe\tvalue\ti\t{bad}", "i")
        self.assertEqual(self.decode_one("AOT\tprobe\tvalue\ti\t-9223372036854775808", "i"), -(1 << 63))

    def test_text_is_exact(self):
        for bad in ("a b", "a=b", "a%2", "a%", "a%zz", "a%41", "a%2D", "a%0A%", "café", "a\r", "a\\"):
            with self.subTest(text=bad):
                self.refused(f"AOT\tprobe\tvalue\tt\t{bad}", "t")
        self.refused("AOT\tprobe\tvalue\tt\ta\tb", "t")

    def test_places_are_exact(self):
        for bad in ("AOT\tprobe\tvalue\tm\tlibz.so", "AOT\tprobe\tvalue\tm\tlibz.so\t", "AOT\tprobe\tvalue\tm\tlibz.so\t0x10",
                    "AOT\tprobe\tvalue\tm\tlibz.so\t010", "AOT\tprobe\tvalue\tm\tlibz.so\t10\t1",
                    "AOT\tprobe\tvalue\tm\tlib z\t10",
                    "AOT\tprobe\tvalue\tu\t100000\trw-\t1\t3",
                    "AOT\tprobe\tvalue\tu\t100000\trw-\t3\t3\t10",
                    "AOT\tprobe\tvalue\tu\t100000\trw-\t0\t0\t10",
                    "AOT\tprobe\tvalue\tu\t100000\tr_-\t0\t1\t10",
                    "AOT\tprobe\tvalue\tu\t100000\trwxp\t0\t1\t10",
                    "AOT\tprobe\tvalue\tu\t0100000\trw-\t0\t1\t10",
                    "AOT\tprobe\tvalue\tu\t100000\trw-\t01\t2\t10",
                    "AOT\tprobe\tvalue\tu\t100000\trw-\t0\t1\t10\t5"):
            with self.subTest(line=bad):
                self.refused(bad)

    def test_a_line_that_is_not_the_encoding_of_what_it_says_is_refused(self):
        for bad in ("AOT\tprobe\tvalue\tn\t1\r", "AOT\tprobe\tvalue\tn\t1 ", " AOT\tprobe\tvalue\tn\t1",
                    "AOT probe value n 1", "AOT  probe\tvalue\tn\t1", "aot\tprobe\tvalue\tn\t1",
                    "AOT\tProbe\tvalue\tn\t1", "AOT\tprobe\tValue\tn\t1", "AOT\tprobe\t\tn\t1",
                    "AOT\t\tvalue\tn\t1", "AOT\tprobe\tvalue\tx\t1", "AOT\tprobe\tvalue\tn", "AOT\tprobe\tvalue",
                    "AOT\tprobe", "AOT", "", "FAIL something", "x07=0 sp=S+0x0", "AOT\tprobe\t1value\tn\t1"):
            with self.subTest(line=bad):
                self.refused(bad)

    def test_a_value_of_the_wrong_type_for_its_field_is_refused(self):
        for kind_type, line in (("n", "AOT\tprobe\tvalue\tt\t5"), ("t", "AOT\tprobe\tvalue\tn\t5"),
                                ("n", "AOT\tprobe\tvalue\tm\tlibz.so\t5"), ("i", "AOT\tprobe\tvalue\tn\t5"),
                                ("n", "AOT\tprobe\tvalue\ti\t5"), ("t", "AOT\tprobe\tvalue\tm\tlibz.so\t5"),
                                ("n", "AOT\tprobe\tvalue\tu\t10\trw-\t0\t1\t5"), ("v", "AOT\tprobe\tvalue\tt\t5"),
                                ("v", "AOT\tprobe\tvalue\ti\t5")):
            with self.subTest(line=line, field=kind_type):
                self.refused(line, kind_type)

    def test_a_value_field_takes_a_number_or_a_place(self):
        self.assertEqual(self.decode_one("AOT\tprobe\tvalue\tn\t5", "v"), 5)
        self.assertEqual(self.decode_one("AOT\tprobe\tvalue\tm\tx\t5", "v"), Named("x", 5))

    def test_the_writer_refuses_what_the_reader_would(self):
        for kind_type, value in (("n", -1), ("n", 1 << 64), ("n", "x"), ("t", 5), ("t", Named("x", 1)),
                                 ("n", Named("x", 1)), ("i", 1 << 63), ("i", -(1 << 63) - 1), ("i", "5"),
                                 ("v", "5"), ("v", Unnamed(0x1000, "rw-", 2, 2, 0)),
                                 ("v", Unnamed(0x1000, "rwx!", 0, 1, 0)), ("v", Unnamed(0x1000, "rw-", 0, 1, -1)),
                                 ("v", Named("x", -1)), ("v", Named("x", 1 << 64)), ("v", 1.5)):
            with self.subTest(kind_type=kind_type, value=value):
                with self.assertRaises(RecordError):
                    line_of(kind_type, value)

    def test_hostile_text_round_trips(self):
        names = ["", "plain", "a b", "a=b", "a\tb", "a\nAOT\tregs\tx06\tn\t0", "x06=0 sp=S+0x0", "a\r\nb", "100%",
                 "%41", "%", "café", "☃\U0001f600", "\\n", "[anon:space x06 0 x4 0]", "x" * 600,
                 "AOT\tprobe\tvalue\tn\t1", "\x00", "\x7f", "\x1b[31m"]
        for name in names:
            with self.subTest(name=name[:30]):
                line = line_of("t", name)
                self.assertNotIn("\n", line)
                self.assertEqual(line.count(TAB), 4)
                self.assertEqual(self.decode_one(line, "t"), name)
                place = line_of("v", Named(name, 0x20))
                self.assertEqual(place.count(TAB), 5)
                self.assertEqual(self.decode_one(place, "v"), Named(name, 0x20))
        generator = random.Random(20261117)
        for _ in range(300):
            raw = bytes(generator.randrange(256) for _ in range(generator.randrange(0, 40)))
            name = raw.decode("utf-8", "surrogateescape")
            self.assertEqual(self.decode_one(line_of("t", name), "t"), name)

    def good_regs(self) -> list[str]:
        return rec.encode_record(fixture.regs_record())

    def test_a_whole_record_decodes(self):
        lines = self.good_regs()
        self.assertEqual(len(lines), len(rec.SCHEMA["regs"]))
        decoded = rec.decode(lines)
        self.assertEqual(decoded, [fixture.regs_record()])

    def test_a_record_must_be_whole_and_in_order(self):
        lines = self.good_regs()
        at = next(n for n, line in enumerate(lines) if line.split(TAB)[2] == "x06")
        cases = {
            "x06 twice": lines[:at + 1] + [lines[at]] + lines[at + 1:],
            "x06 missing": lines[:at] + lines[at + 1:],
            "x06 and x07 swapped": lines[:at] + [lines[at + 1], lines[at]] + lines[at + 2:],
            "an unknown field": lines[:at] + ["AOT\tregs\tx99\tn\t0"] + lines[at + 1:],
            "cut short": lines[:at],
            "cut short at the first field": lines[:1],
            "another record in the middle": lines[:at] + rec.encode_record(fixture.step(1)) + lines[at:],
            "the last field missing": lines[:-1],
            "a field repeated at the end": lines + [lines[-1]],
            "an unknown kind": ["AOT\tnothing\tf\tn\t0"],
            "a name of one record on a line of another": lines[:at] + [lines[at].replace("regs", "step")] + lines[at + 1:],
        }
        for label, broken in cases.items():
            with self.subTest(label):
                with self.assertRaises(RecordError):
                    rec.decode(broken)

    def test_a_record_that_is_not_whole_is_refused_by_the_writer(self):
        short = Record("regs", fixture.regs_record().fields[:20])
        swapped = Record("step", (("pc", Named("libz.so", 1)), ("n", 1)))
        extra = Record("done", (("mode", "x"), ("more", "y")))
        for broken in (short, swapped, extra, Record("nothing", ())):
            with self.subTest(kind=broken.kind):
                with self.assertRaises(RecordError):
                    rec.encode_record(broken)

    def test_records_cannot_repeat_a_field(self):
        with self.assertRaises(ValueError):
            Record("done", (("mode", "x"), ("mode", "y")))

    def test_the_schema_names_every_field_once_in_a_known_type(self):
        for kind, fields in rec.SCHEMA.items():
            with self.subTest(kind):
                names = [name for name, _ in fields]
                self.assertEqual(len(names), len(set(names)))
                self.assertTrue(all(rec.IDENTIFIER.fullmatch(name) for name in names + [kind]))
                self.assertTrue(all(kind_type in ("n", "i", "t", "v") for _, kind_type in fields))
        self.assertEqual(len(rec.SCHEMA["regs"]), 1 + 34)


class GuestSourceTest(unittest.TestCase):
    """The guest program prints records and nothing else; the schema names
    what its source emits. The program runs only in a guest, so what the
    source says is what is checked here."""

    SOURCE = (AOT / "aot_guest_debug.cpp").read_text()

    def test_the_kinds_it_emits_are_the_schemas_kinds(self):
        emitted = set(re.findall(r'\brecord\("([a-z_.]+)"\)', self.SOURCE))
        for pair in re.findall(r'\brecord\([a-z_]+ \? "([a-z_]+)" : "([a-z_]+)"\)', self.SOURCE):
            emitted |= set(pair)
        self.assertEqual(emitted, set(rec.SCHEMA))

    def test_every_literal_field_of_a_kind_is_named_in_the_source(self):
        for kind, fields in rec.SCHEMA.items():
            if kind in ("regs", "sibling_entry", "sibling_loop", "sibling_runs_on"):
                continue
            for name, _ in fields:
                with self.subTest(kind=kind, field=name):
                    self.assertIn(f'"{name}"', self.SOURCE)
        for name in rec.REGISTERS[-3:]:
            self.assertIn(f'"{name}"', self.SOURCE)


class NativeTest(unittest.TestCase):
    """guest_records.hpp built on the host, against the Python encoder."""

    @classmethod
    def setUpClass(cls) -> None:
        compiler = os.environ.get("UML_CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("no C++ compiler")
        cls.directory = tempfile.mkdtemp(prefix="aot-records-")
        cls.harness = pathlib.Path(cls.directory) / "harness"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(AOT),
                        str(HERE / "aot_records_harness.cpp"), "-o", str(cls.harness)],
                       check=True, capture_output=True, text=True)
        cls.compiler = compiler

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.directory, ignore_errors=True)

    def run_harness(self, *arguments: str) -> list[str]:
        done = subprocess.run([str(self.harness), *arguments], check=True, capture_output=True, timeout=60)
        output = done.stdout.decode("ascii")
        self.assertTrue(output == "" or output.endswith("\n"))
        return output.splitlines()

    def native_maps(self, mappings: list[Mapping], values: list[int]) -> list[str]:
        path = pathlib.Path(self.directory) / "maps"
        path.write_bytes(maps_text(mappings))
        return self.run_harness("resolve", str(path), *[f"{value:x}" for value in values])

    def python_maps(self, mappings: list[Mapping], values: list[int]) -> list[str]:
        return [line_of("v", rec.resolve(value, mappings)) for value in values]

    def agree(self, mappings: list[Mapping], values: list[int]) -> list[str]:
        native = self.native_maps(mappings, values)
        self.assertEqual(native, self.python_maps(mappings, values))
        return native

    def decoded(self, lines: list[str]):
        return [each.get("value") for each in rec.decode(lines, probe_schema("v"))]

    @staticmethod
    def inside(mappings: list[Mapping]) -> list[int]:
        """The first byte, a byte in, and the last byte of every mapping."""
        return [value for low, high, *_ in mappings for value in (low, low + 0x10, high - 1)]

    def test_both_name_a_mixed_layout_alike(self):
        base = 0x7ff1ae500000
        mappings: list[Mapping] = [
            (0x400000, 0x401000, 0, "/system/bin/toybox", "r-x"),
            (base, base + 0xb0000, 0x6000, "/apex/com.android.runtime/lib64/bionic/libc.so", "r-x"),
            (base + 0xb0000, base + 0xb3000, 0, "[anon:.bss]", "rw-"),
            (base + 0xc0000, base + 0x1c0000, 0, "", "rw-"),
            (base + 0x200000, base + 0x300000, 0, "", "rw-"),
            (base + 0x300000, base + 0x340000, 0, "", "rw-"),
            (base + 0x340000, base + 0x440000, 0, "", "r--"),
            (base + 0x440000, base + 0x540000, 0, "", "rw-"),
        ]
        values = self.inside(mappings) + [0x1000, base + 0x1c0000 + 0x10, base + 0x540000]
        lines = self.agree(mappings, values)
        places = dict(zip(values, self.decoded(lines)))
        self.assertEqual(places[0x400000], Named("toybox", 0))
        self.assertEqual(places[base + 0x10], Named("libc.so", 0x6010))
        self.assertEqual(places[base + 0xb0010], Named("[anon:.bss]", 0x10))
        self.assertEqual(places[0x1000], 0x1000)
        self.assertEqual(places[base + 0x540000], base + 0x540000)
        # Three unnamed 1 MiB read-write mappings, told apart by their place
        # among the three.
        self.assertEqual([places[base + low + 0x10] for low in (0xc0000, 0x200000, 0x440000)],
                         [Unnamed(0x100000, "rw-", ordinal, 3, 0x10) for ordinal in range(3)])

    def test_both_name_more_than_128_shapes_alike(self):
        """A layout of 129 distinct shapes, and then two mappings of the last:
        nothing in the program is a table of shapes, so the two mappings of
        the last shape are told apart."""
        base = 0x7ff100000000
        mappings: list[Mapping] = []
        at = base
        for pages in range(1, 130):
            mappings.append((at, at + pages * PAGE, 0, "", "rw-"))
            at += pages * PAGE + PAGE
        low, high = at, at + 129 * PAGE
        mappings.append((low, high, 0, "", "rw-"))
        lines = self.agree(mappings, self.inside(mappings))
        first_last = self.decoded(self.agree(mappings, [mappings[128][0] + 0x10]))[0]
        second_last = self.decoded(self.agree(mappings, [low + 0x10]))[0]
        self.assertEqual(first_last, Unnamed(129 * PAGE, "rw-", 0, 2, 0x10))
        self.assertEqual(second_last, Unnamed(129 * PAGE, "rw-", 1, 2, 0x10))
        self.assertEqual(len(set(lines)), len(lines))

    def test_both_name_a_hostile_mapping_name_alike_and_whole(self):
        """A mapping whose name has a space, `=`, `%`, a tab, a field label, a
        600-character stretch and a byte that is no UTF-8: the name travels
        whole in one line, and both encoders write the same line."""
        base = 0x7ff100000000
        names = ["[anon:space x06 0 x4 0]", "[anon:x06=0 sp=S+0x0 pc=libz.so+0x1 nzcv=0x0]", "[anon:100% sure]",
                 "[anon:tab\there]", "[anon:" + "x" * 600 + "]", "[anon:\udcff\udcfe]", "[anon:café]",
                 "[anon:AOT\tregs\tx06\tn\t0]", "/system/lib64/lib with space.so", "[anon:" + "y" * 5000 + "]"]
        mappings: list[Mapping] = [(base + n * 0x10000, base + n * 0x10000 + PAGE, 0, name, "rw-")
                                   for n, name in enumerate(names)]
        values = [low + 0x10 for low, *_ in mappings]
        lines = self.agree(mappings, values)
        self.assertEqual(len(lines), len(names))
        for line, name in zip(lines, names):
            self.assertEqual(line.count(TAB), 5, line)
            self.assertTrue(line.startswith(f"AOT\tprobe\tvalue\tm\t"))
        decoded = self.decoded(lines)
        self.assertEqual([place.identity for place in decoded],
                         [name.rsplit("/", 1)[1] if name.startswith("/") else name for name in names])
        self.assertEqual({place.offset for place in decoded}, {0x10})

    def test_both_write_every_value_alike(self):
        hostile = [b"", b"plain", b"a b", b"a=b", b"a\tb", b"a\nAOT\tregs\tx06\tn\t0", b"a\r\nb", b"100%", b"%41",
                   "café".encode(), b"\xff\xfe\x80", b"x" * 600, bytes(range(256)), b"\x00", b"AOT\tx\ty\tn\t1"]
        lines = self.run_harness("text", *[each.hex() for each in hostile])
        self.assertEqual(lines, [line_of("t", each.decode("utf-8", "surrogateescape")) for each in hostile])
        self.assertEqual([each.get("value") for each in rec.decode(lines, probe_schema("t"))],
                         [each.decode("utf-8", "surrogateescape") for each in hostile])
        for each in hostile:
            name = each.decode("utf-8", "surrogateescape")
            with self.subTest(name=name[:20]):
                lines = self.run_harness("named", each.hex(), "7f")
                self.assertEqual(lines, [line_of("v", Named(name, 0x7f))])
                self.assertEqual(self.decoded(lines), [Named(name, 0x7f)])
        for number in (0, 1, 0xf, 0x10, 0xdeadbeef, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff):
            lines = self.run_harness("number", f"{number:x}")
            self.assertEqual(lines, [line_of("n", number)])
            self.assertEqual(rec.decode(lines, probe_schema("n"))[0].get("value"), number)
        for number in (0, 1, -1, 9, 10, -10, 123456789, (1 << 63) - 1, -(1 << 63)):
            lines = self.run_harness("integer", str(number))
            self.assertEqual(lines, [line_of("i", number)])
            self.assertEqual(rec.decode(lines, probe_schema("i"))[0].get("value"), number)

    def test_a_register_record_with_a_name_that_looks_like_a_field_keeps_every_field(self):
        """x07 points into a mapping named like the other registers: the
        record has every field once, x06 is the place it points to, and the
        name is x07's own."""
        base = 0x7ff100000000
        hostile = "[anon:x06=0 x07=1 sp=S+0x0 nzcv=0x0]"
        mappings: list[Mapping] = [(base, base + 0x100000, 0, "", "rw-"), (base + 0x200000, base + 0x201000, 0, hostile, "rw-")]
        path = pathlib.Path(self.directory) / "regs-maps"
        path.write_bytes(maps_text(mappings))
        lines = self.run_harness("regs", str(path), f"{base + 0x1234:x}", f"{base + 0x200020:x}")
        self.assertEqual(len(lines), len(rec.SCHEMA["regs"]))
        decoded = rec.decode(lines)
        self.assertEqual(len(decoded), 1)
        regs = decoded[0]
        self.assertEqual(regs.names(), tuple(name for name, _ in rec.SCHEMA["regs"]))
        self.assertEqual(regs.get("x06"), Unnamed(0x100000, "rw-", 0, 1, 0x1234))
        self.assertEqual(regs.get("x07"), Named(hostile, 0x20))
        self.assertEqual(regs.get("pc"), Named("libz.so", 0x12190))
        self.assertEqual(sum(1 for line in lines if line.split(TAB)[2] in ("x06", "x07")), 2)

    def test_the_runner_accepts_the_emitters_output_and_judges_a_moved_pointer(self):
        base = 0x7ff100000000
        path = pathlib.Path(self.directory) / "judged-maps"

        def regs(layout: list[Mapping], x06: int, x07: int) -> list[Record]:
            path.write_bytes(maps_text(layout))
            return rec.decode(self.run_harness("regs", str(path), f"{x06:x}", f"{x07:x}"))

        def names(first: str, second: str) -> list[Mapping]:
            return [(base, base + 0x100000, 0, "", "rw-"), (base + 0x200000, base + 0x201000, 0, first, "rw-"),
                    (base + 0x300000, base + 0x301000, 0, second, "rw-")]

        # the names hold other numbers of tokens and look like fields
        first = regs(names("[anon:left one two]", "[anon:x06=0 right]"), base + 0x1234, base + 0x300020)
        second = regs(names("[anon:left]", "[anon:x06=0 right]"), base + 0x1234, base + 0x300020)
        control = regs(names("[anon:left]", "[anon:x06=0 right]"), base + 0x1234, base + 0x300020)
        moved = regs(names("[anon:left one two]", "[anon:x06=0 right]"), base + 0x1238, base + 0x300020)
        compared, masked = rows.compare("ptrace", [first, second, first], control)
        self.assertGreater(compared, 30)
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("ptrace", [first, second, first], moved)
        self.assertIn("x06", str(caught.exception))


GUEST_PROBE = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "guest_records.hpp"
static std::string incoming;
static size_t cursor;
static int fake_open(const char *, int) { cursor = 0; return 1; }
static int fake_close(int) { return 0; }
static long fake_read(int, void *out, size_t capacity) {
    const size_t amount = std::min(capacity, incoming.size() - cursor);
    std::memcpy(out, incoming.data() + cursor, amount);
    cursor += amount;
    return static_cast<long>(amount);
}
#define open fake_open
#define close fake_close
#define read fake_read
%(functions)s
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    incoming.assign(std::string(argv[1]) == "full" ? (1 << 17) - 1 : (1 << 17) - 2, 'x');
    load_maps(123);
    std::printf("maps_size=%%ld\n", maps_size);
    return 0;
}
"""


class GuestMapsTest(unittest.TestCase):
    """What the guest program does with a maps text that fills its buffer,
    through the runner's own judgement of a run (debug_rows.run_mode): the
    function is the program's own, taken from its source and built on the host
    with the file reads replaced."""

    @classmethod
    def setUpClass(cls) -> None:
        compiler = os.environ.get("UML_CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None:
            raise unittest.SkipTest("no C++ compiler")
        source = (AOT / "aot_guest_debug.cpp").read_text()
        functions = source[source.index("char maps_text[1 << 17];"):source.index("// Where a register's value points")]
        cls.directory = tempfile.mkdtemp(prefix="aot-guest-maps-")
        program = pathlib.Path(cls.directory) / "probe.cpp"
        program.write_text(GUEST_PROBE % {"functions": functions})
        cls.probe = pathlib.Path(cls.directory) / "probe"
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-I", str(AOT), str(program), "-o", str(cls.probe)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.directory, ignore_errors=True)

    def output(self, which: str) -> str:
        return subprocess.run([str(self.probe), which], check=True, capture_output=True, text=True,
                              timeout=30).stdout

    def judged(self, extra: str):
        lines = fixture.Wire.lines(fixture.ptrace_records(True))
        at = next(n for n, line in enumerate(lines) if line.split(TAB)[1] == "regs") + 7
        text = "pid=4242\n" + "\n".join(lines[:at]) + "\n" + extra + "\n".join(lines[at:]) + "\nrc=0\n"
        return rows.run_mode(lambda *_args, **_kw: text, "ptrace")

    def test_a_maps_text_that_fits_is_no_failure(self):
        output = self.output("fits")
        self.assertNotIn("FAIL", output)
        self.assertEqual(self.judged("")[-1], fixture.done("ptrace"))

    def test_a_maps_text_that_does_not_fit_is_a_line_of_its_own(self):
        output = self.output("full")
        failures = [line for line in output.splitlines() if "FAIL" in line]
        self.assertEqual(len(failures), 1, output)
        self.assertTrue(failures[0].startswith("FAIL the maps of process 123 do not fit"), output)
        self.assertTrue(output.startswith("\nFAIL"), repr(output))

    def test_a_maps_text_that_does_not_fit_fails_the_row_even_in_the_middle_of_a_record(self):
        failure = "\n".join(line for line in self.output("full").splitlines() if "FAIL" in line) + "\n"
        with self.assertRaises(rows.DebugFailure) as caught:
            self.judged("\n" + failure)
        self.assertIn("FAIL", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
