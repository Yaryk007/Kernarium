#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The protobuf reader of the AOT rows and the tombstone model built on it.

tests/integration/aot/protowire.py reads the wire format strictly, and
tombstone_pb.py reads a tombstone with it into the records the rows compare.
These rows give the reader hand-built bytes of every kind of bad file, pin the
field numbers the model uses (the table follows the tombstone.proto of the
source tree the image is built from; the numbers here are written out by
hand), and hold the model to the false passes found in it: a mapping with the
fault address in it that was dropped, two mappings that differed by a trailing
space and were one name, and the stacks of two threads that were one name.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "integration" / "aot"))
sys.path.insert(0, str(HERE))
import aot_pb_fixture as pb  # noqa: E402
import debug_rows as rows  # noqa: E402
import protowire  # noqa: E402
import records as rec  # noqa: E402
import tombstone_pb  # noqa: E402
from protowire import Field, WireError  # noqa: E402
from records import Named, Unnamed  # noqa: E402

# A message of every kind of field the reader has.
TABLES = {
    "M": {1: Field("u", "u32"), 2: Field("s", "string"), 3: Field("n", "message", message="N"),
          4: Field("r", "string", repeated=True), 5: Field("b", "bool"), 6: Field("i", "i32"),
          7: Field("e", "enum", values=(0, 1, 2)), 8: Field("q", "u64"), 9: Field("m", "map", message="N", key="u32"),
          10: Field("y", "bytes"), 11: Field("j", "i64"), 12: Field("ns", "message", message="N", repeated=True)},
    "N": {1: Field("x", "u32")},
}


def decode(data: bytes):
    return protowire.decode(data, "M", TABLES)


def tombstone_of(**keywords) -> bytes:
    return pb.tombstone(**keywords)


def digest(data: bytes):
    return rows.digest_tombstone(data)


def field_of(records_of_run, kind, name):
    return next(each for each in records_of_run if each.kind == kind).get(name)


class WireTest(unittest.TestCase):
    def refused(self, data: bytes, *fragments: str) -> None:
        with self.assertRaises(WireError) as caught:
            decode(data)
        for fragment in fragments:
            self.assertIn(fragment, str(caught.exception))

    def test_every_kind_of_field_decodes(self):
        data = (pb.always(1, 7) + pb.text_field(2, "name") + pb.bytes_field(3, pb.always(1, 9))
                + pb.text_field(4, "a") + pb.text_field(4, "b") + pb.always(5, 1) + pb.always(6, -2)
                + pb.always(7, 2) + pb.always(8, (1 << 64) - 1) + pb.bytes_field(9, pb.always(1, 5) + pb.bytes_field(
                    2, pb.always(1, 6))) + pb.bytes_field(10, b"\x00\xff") + pb.always(11, -(1 << 62))
                + pb.bytes_field(12, pb.always(1, 1)) + pb.bytes_field(12, pb.always(1, 2)))
        values = decode(data).values
        self.assertEqual((values["u"], values["s"], values["r"], values["b"], values["i"], values["e"]),
                         (7, "name", ["a", "b"], 1, -2, 2))
        self.assertEqual((values["q"], values["y"], values["j"]), ((1 << 64) - 1, b"\x00\xff", -(1 << 62)))
        self.assertEqual(values["n"], {"x": 9})
        self.assertEqual(values["m"], [(5, {"x": 6})])
        self.assertEqual(values["ns"], [{"x": 1}, {"x": 2}])

    def test_a_field_that_is_not_there_is_its_default(self):
        values = decode(b"").values
        self.assertEqual((values["u"], values["s"], values["n"], values["r"], values["b"], values["y"]),
                         (0, "", None, [], 0, b""))

    def test_a_varint_that_is_cut_short_or_too_long_is_refused(self):
        self.refused(b"\x08", "cut short")
        self.refused(b"\x08\x80", "cut short")
        self.refused(b"\x08" + b"\xff" * 9 + b"\x02", "does not fit 64 bits")
        self.refused(b"\x08" + b"\xff" * 10 + b"\x01", "does not fit 64 bits")
        self.refused(b"\x80", "cut short")
        # the largest 64-bit varint is fine
        self.assertEqual(decode(b"\x40" + b"\xff" * 9 + b"\x01").values["q"], (1 << 64) - 1)

    def test_a_length_that_runs_past_its_message_is_refused(self):
        self.refused(pb.tag(2, 2) + pb.varint(5) + b"abc", "runs past")
        self.refused(pb.tag(2, 2), "cut short")
        nested = pb.bytes_field(3, pb.tag(1, 0) + pb.varint(1) + pb.tag(9, 2) + pb.varint(50) + b"x")
        self.refused(nested, "runs past")
        # a nested message whose own length is cut short inside a parent that continues
        inner = pb.tag(1, 0)
        self.refused(pb.bytes_field(3, inner) + pb.always(1, 5), "cut short")
        self.refused(pb.bytes_field(9, pb.tag(2, 2) + pb.varint(9) + b"ab"), "runs past")

    def test_a_file_that_ends_inside_a_field_is_refused_at_every_cut(self):
        data = (pb.always(1, 300) + pb.text_field(2, "hello") + pb.bytes_field(3, pb.always(1, 1 << 20)))
        boundaries = {0, len(pb.always(1, 300)), len(pb.always(1, 300) + pb.text_field(2, "hello")), len(data)}
        for cut in range(len(data) + 1):
            if cut in boundaries:
                decode(data[:cut])
                continue
            with self.subTest(cut=cut):
                with self.assertRaises(WireError):
                    decode(data[:cut])

    def test_wire_types_that_are_no_value_types_are_refused(self):
        for wire in (3, 4, 6, 7):
            with self.subTest(wire=wire):
                self.refused(pb.tag(15, wire) + b"\x00", f"wire type {wire}")

    def test_field_number_zero_and_beyond_the_range_are_refused(self):
        self.refused(b"\x00", "field number 0")
        self.refused(b"\x00\x00\x00", "field number 0")
        self.refused(pb.varint((1 << 29) << 3) + b"\x00", "field number")
        decode(pb.varint(((1 << 29) - 1) << 3) + b"\x00")

    def test_bytes_after_the_last_field_that_are_not_a_field_are_refused(self):
        good = pb.always(1, 5)
        for extra in (b"\x00", b"\x08", b"\x12", b"\x0d\x00\x00", b"\x09" + b"\x00" * 7):
            with self.subTest(extra=extra):
                with self.assertRaises(WireError):
                    decode(good + extra)

    def test_a_field_of_the_table_with_the_wrong_wire_type_is_refused(self):
        self.refused(pb.tag(1, 2) + pb.varint(1) + b"x", "wire type 2")
        self.refused(pb.tag(2, 0) + pb.varint(1), "wire type 0")
        self.refused(pb.tag(3, 0) + pb.varint(1), "wire type 0")
        self.refused(pb.tag(1, 1) + b"\x00" * 8, "wire type 1")
        self.refused(pb.tag(1, 5) + b"\x00" * 4, "wire type 5")
        self.refused(pb.tag(9, 0) + pb.varint(1), "wire type 0")

    def test_a_singular_field_that_arrives_twice_is_refused(self):
        self.refused(pb.always(1, 1) + pb.always(1, 1), "twice")
        self.refused(pb.text_field(2, "a") + pb.text_field(2, "a"), "twice")
        self.refused(pb.bytes_field(3, pb.always(1, 1)) + pb.bytes_field(3, pb.always(1, 1)), "twice")
        self.refused(pb.always(5, 1) + pb.always(5, 0), "twice")
        self.refused(pb.always(1, 1) + pb.text_field(2, "x") + pb.always(1, 1), "twice")
        decode(pb.text_field(4, "a") + pb.text_field(4, "a"))

    def test_a_value_that_does_not_fit_its_kind_is_refused(self):
        self.refused(pb.always(1, 1 << 32), "32 bits")
        self.refused(pb.always(5, 2), "not a bool")
        self.refused(pb.always(7, 3), "does not name")
        self.refused(pb.always(6, 1 << 31), "does not fit i32")
        self.refused(pb.always(6, 0xffffffff), "does not fit i32")
        self.assertEqual(decode(pb.always(6, -(1 << 31))).values["i"], -(1 << 31))
        self.assertEqual(decode(pb.always(1, 0xffffffff)).values["u"], 0xffffffff)

    def test_the_fields_the_table_does_not_name_are_skipped_by_wire_type_and_counted(self):
        data = (pb.always(30, 1) + pb.always(30, 2) + pb.tag(31, 1) + b"\x00" * 8 + pb.tag(32, 5) + b"\x00" * 4
                + pb.bytes_field(33, b"\x00\x00") + pb.bytes_field(3, pb.always(1, 4) + pb.always(2, 9))
                + pb.always(1, 3))
        reading = decode(data)
        self.assertEqual(reading.values["u"], 3)
        self.assertEqual(reading.values["n"], {"x": 4})
        self.assertEqual(dict(reading.skipped), {("M", 30): 2, ("M", 31): 1, ("M", 32): 1, ("M", 33): 1, ("N", 2): 1})
        self.assertEqual(protowire.describe(reading.skipped), "M.30 x2, M.31 x1, M.32 x1, M.33 x1, N.2 x1")
        # a skipped fixed-width field that is cut short is refused too
        self.refused(pb.tag(31, 1) + b"\x00" * 7, "eight bytes")
        self.refused(pb.tag(32, 5) + b"\x00" * 3, "four bytes")

    def test_a_map_entry_is_a_key_and_a_message_once_each(self):
        entry = pb.always(1, 5) + pb.bytes_field(2, pb.always(1, 6))
        decode(pb.bytes_field(9, entry))
        for label, broken in (("no value", pb.always(1, 5)), ("no key", pb.bytes_field(2, b"")),
                              ("two keys", pb.always(1, 5) + pb.always(1, 6) + pb.bytes_field(2, b"")),
                              ("two values", entry + pb.bytes_field(2, b"")),
                              ("a field of its own", entry + pb.always(3, 1)),
                              ("a key of the wrong wire type", pb.tag(1, 2) + pb.varint(0) + pb.bytes_field(2, b""))):
            with self.subTest(label):
                self.refused(pb.bytes_field(9, broken))

    def test_a_string_keeps_the_bytes_it_was_given(self):
        text = decode(pb.bytes_field(2, b"/tmp/\xff\xfe a ")).values["s"]
        self.assertEqual(text.encode("utf-8", "surrogateescape"), b"/tmp/\xff\xfe a ")
        self.assertEqual(rec.escape(text), "/tmp/%ff%fe%20a%20")
        self.assertEqual(rec.unescape(rec.escape(text)), text)

    def test_what_it_skipped_is_not_what_it_refused(self):
        # Unknown numbers inside a nested message and a map entry's value are read like any other.
        reading = decode(pb.bytes_field(9, pb.always(1, 1) + pb.bytes_field(2, pb.always(7, 1))))
        self.assertEqual(dict(reading.skipped), {("N", 7): 1})


# The numbers the model reads, written out by hand: message -> number -> (name, kind). A change of the table that is
# not a change of these is a drift from the image's source.
PINNED = {
    "Tombstone": {1: ("arch", "enum"), 2: ("build_fingerprint", "string"), 3: ("revision", "string"),
                  5: ("pid", "u32"), 6: ("tid", "u32"), 7: ("uid", "u32"), 8: ("selinux_label", "string"),
                  9: ("command_line", "string"), 10: ("signal_info", "message"), 14: ("abort_message", "string"),
                  16: ("threads", "map"), 17: ("memory_mappings", "message"), 22: ("page_size", "u32"),
                  23: ("has_been_16kb_mode", "bool"), 24: ("guest_arch", "enum"), 27: ("executable_name", "string"),
                  28: ("kernel_release", "string"), 29: ("ppid", "u32"), 1000: ("lineage_version", "string")},
    "Signal": {1: ("number", "i32"), 2: ("name", "string"), 3: ("code", "i32"), 4: ("code_name", "string"),
               5: ("has_sender", "bool"), 8: ("has_fault_address", "bool"), 9: ("fault_address", "u64")},
    "Thread": {1: ("id", "i32"), 2: ("name", "string"), 3: ("registers", "message"),
               4: ("current_backtrace", "message"), 5: ("memory_dump", "message"), 6: ("tagged_addr_ctrl", "i64"),
               8: ("pac_enabled_keys", "i64")},
    "Register": {1: ("name", "string"), 2: ("u64", "u64")},
    "BacktraceFrame": {1: ("rel_pc", "u64"), 4: ("function_name", "string"), 5: ("function_offset", "u64"),
                       6: ("file_name", "string"), 7: ("file_map_offset", "u64"), 8: ("build_id", "string")},
    "MemoryDump": {1: ("register_name", "string"), 2: ("mapping_name", "string"), 3: ("begin_address", "u64"),
                   4: ("memory", "bytes")},
    "MemoryMapping": {1: ("begin_address", "u64"), 2: ("end_address", "u64"), 3: ("offset", "u64"),
                      4: ("read", "bool"), 5: ("write", "bool"), 6: ("execute", "bool"), 7: ("mapping_name", "string")},
}


class TableTest(unittest.TestCase):
    def test_the_table_has_the_numbers_the_model_reads(self):
        self.assertEqual(set(tombstone_pb.TABLES), set(PINNED))
        for message, fields in PINNED.items():
            with self.subTest(message):
                table = tombstone_pb.TABLES[message]
                self.assertEqual({number: (entry.name, entry.kind) for number, entry in table.items()}, fields)

    def test_the_architectures_are_the_protos(self):
        self.assertEqual(tombstone_pb.ARCHITECTURES,
                         {0: "arm32", 1: "arm64", 2: "x86", 3: "x86_64", 4: "riscv64", 5: "none"})
        self.assertEqual(tombstone_pb.ARM64, 1)

    def test_hand_written_bytes_come_out_under_the_names_of_the_numbers(self):
        """The bytes of every field the model reads, with the tag bytes written
        out (a field number 5 varint is 0x28), decode under the field's name."""
        mapping = b"\x08\x80\x80\x04\x10\x90\x80\x04\x18\x07\x20\x01\x30\x01\x3a\x04name"
        register = b"\x0a\x02x0\x10\x2a"
        frame = b"\x08\x05\x22\x03fun\x28\x06\x32\x04file\x38\x07\x42\x02id"
        dump = b"\x0a\x02pc\x12\x03map\x18\x09\x22\x02\x01\x02"
        thread = (b"\x08\x04\x12\x03thr\x1a" + bytes([len(register)]) + register + b"\x22" + bytes([len(frame)]) + frame
                  + b"\x2a" + bytes([len(dump)]) + dump + b"\x30\x01\x40\x01")
        signal = b"\x08\x0b\x12\x01s\x18\x02\x22\x01c\x28\x01\x40\x01\x48\x09"
        entry = b"\x08\x04\x12" + bytes([len(thread)]) + thread
        data = (b"\x08\x01\x12\x02fp\x1a\x01r\x28\x05\x30\x06\x38\x07\x42\x02se\x4a\x01c\x52" + bytes([len(signal)]) + signal
                + b"\x72\x03abt\x82\x01" + bytes([len(entry)]) + entry + b"\x8a\x01" + bytes([len(mapping)]) + mapping
                + b"\xb0\x01\x80\x20\xb8\x01\x01\xc0\x01\x00\xda\x01\x02ex\xe2\x01\x02kr\xe8\x01\x03"
                + b"\xc2\x3e\x02lv")
        values = decode_tombstone(data)
        self.assertEqual((values["arch"], values["build_fingerprint"], values["revision"], values["pid"],
                          values["tid"], values["uid"], values["selinux_label"], values["command_line"]),
                         (1, "fp", "r", 5, 6, 7, "se", ["c"]))
        self.assertEqual((values["abort_message"], values["page_size"], values["has_been_16kb_mode"],
                          values["guest_arch"], values["executable_name"], values["kernel_release"], values["ppid"],
                          values["lineage_version"]), ("abt", 4096, 1, 0, "ex", "kr", 3, "lv"))
        self.assertEqual(values["signal_info"], {"number": 11, "name": "s", "code": 2, "code_name": "c",
                                                 "has_sender": 1, "has_fault_address": 1, "fault_address": 9})
        mapping = values["memory_mappings"][0]
        self.assertEqual((mapping["begin_address"], mapping["end_address"], mapping["offset"], mapping["read"],
                          mapping["write"], mapping["execute"], mapping["mapping_name"]),
                         (0x10000, 0x10010, 7, 1, 0, 1, "name"))
        [(key, thread)] = values["threads"]
        self.assertEqual((key, thread["id"], thread["name"], thread["tagged_addr_ctrl"], thread["pac_enabled_keys"]),
                         (4, 4, "thr", 1, 1))
        self.assertEqual(thread["registers"], [{"name": "x0", "u64": 42}])
        self.assertEqual(thread["current_backtrace"], [{"rel_pc": 5, "function_name": "fun", "function_offset": 6,
                                                        "file_name": "file", "file_map_offset": 7, "build_id": "id"}])
        self.assertEqual(thread["memory_dump"], [{"register_name": "pc", "mapping_name": "map", "begin_address": 9,
                                                  "memory": b"\x01\x02"}])


def decode_tombstone(data: bytes) -> dict:
    return protowire.decode(data, "Tombstone", tombstone_pb.TABLES).values


class ModelTest(unittest.TestCase):
    def failure(self, data: bytes, *fragments: str) -> None:
        with self.assertRaises(rows.DebugFailure) as caught:
            digest(data)
        message = str(caught.exception)
        self.assertTrue(message.startswith("the tombstone: "), message)
        for fragment in fragments:
            self.assertIn(fragment, message)

    def test_a_good_tombstone_is_the_records_of_the_run(self):
        recs = digest(tombstone_of())
        self.assertEqual([each.kind for each in recs],
                         ["tomb.head", "tomb.cmdline", "tomb.cmdline", "tomb.process", "tomb.state", "tomb.signal",
                          "tomb.abort", "tomb.regs", "tomb.frames"] + ["tomb.frame"] * 4 + ["tomb.code"] * 2)
        regs = next(each for each in recs if each.kind == "tomb.regs")
        self.assertEqual(regs.get("pc"), Named(pb.LIBC, 0x70d50))
        self.assertEqual(len(regs.fields), 36)

    def test_a_missing_register_is_a_failure_whichever_it_is(self):
        for name in tombstone_pb.REQUIRED_REGISTERS:
            with self.subTest(name):
                self.failure(tombstone_of(drop_registers=(name,)), "lacks the registers", name)

    def test_the_optional_registers_may_be_missing_and_none_may_repeat(self):
        digest(tombstone_of(drop_registers=("esr", "vg")))
        for name in ("x7", "esr"):
            with self.subTest(name):
                self.failure(tombstone_of(extra_registers=[(name, 1)]), "listed twice", name)

    def test_a_register_the_model_does_not_know_is_a_failure(self):
        self.failure(tombstone_of(extra_registers=[("tpidr", 1)]), "tpidr")

    def test_a_register_that_holds_zero_is_the_zero(self):
        recs = digest(tombstone_of(registers={"x9": 0}))
        self.assertEqual(field_of(recs, "tomb.regs", "x9"), 0)

    def test_the_messages_the_model_needs_are_failures_when_they_are_missing(self):
        self.failure(tombstone_of(no_signal=True), "no signal_info")
        self.failure(tombstone_of(thread_key=1), "no entry for the crashing thread")
        self.failure(tombstone_of(crashing_id=7), "says it is thread 7")
        self.failure(tombstone_of(duplicate_thread=True), "twice")
        self.failure(tombstone_of(arch=3), "x86_64")
        self.failure(tombstone_of(last=False), "ends before its last field")
        self.failure(tombstone_of(extra_mappings=[], mapping_order=[]), "no memory mapping")

    def test_a_file_cut_at_a_field_boundary_does_not_pass(self):
        """The reporter writes the fields in the order of their numbers: any
        prefix of a whole file that holds whole fields lacks the last."""
        data = tombstone_of()
        found = 0
        for cut in range(len(data)):
            try:
                digest(data[:cut])
            except rows.DebugFailure:
                found += 1
            else:
                self.fail(f"a file cut at {cut} of {len(data)} bytes was read")
        self.assertEqual(found, len(data))

    def test_the_order_of_the_mappings_in_the_file_is_not_a_fact_of_the_guest(self):
        base = digest(tombstone_of(guard_page=True, second_unnamed=True, lower_unnamed=True))
        count = 8
        for order in (list(reversed(range(count))), [3, 1, 4, 0, 6, 2, 7, 5], [7, 0, 6, 1, 5, 2, 4, 3]):
            with self.subTest(order=order):
                shuffled = digest(tombstone_of(guard_page=True, second_unnamed=True, lower_unnamed=True,
                                               mapping_order=order))
                self.assertEqual(list(shuffled), list(base))

    def test_the_mappings_that_are_no_list_are_failures(self):
        self.failure(tombstone_of(extra_mappings=[pb.mapping(0x7ff1ae500000 + 0x10, 0x7ff1ae500000 + 0x20, "x")]),
                     "overlap")
        self.failure(tombstone_of(extra_mappings=[pb.mapping(0x7ff1ae500000, 0x7ff1ae500000 + 0xb0000, pb.LIBC, "r-x")]),
                     "overlap")
        self.failure(tombstone_of(extra_mappings=[pb.mapping(0x5000, 0x4000, "backwards")]), "ends at")
        self.failure(tombstone_of(extra_mappings=[pb.mapping(0x5000, 0x5000, "empty")]), "ends at")

    def test_the_fault_address_names_the_mapping_it_is_in(self):
        guard = digest(tombstone_of(guard_page=True))
        place = field_of(guard, "tomb.signal", "fault_at")
        self.assertEqual(place, Unnamed(0x1000, "---", 0, 1, 0x10))
        self.assertEqual(field_of(guard, "tomb.signal", "page_offset"), 0x10)
        self.assertEqual(field_of(digest(tombstone_of()), "tomb.signal", "fault_at"), "outside every mapping")
        none = digest(tombstone_of(signal_message=pb.signal(6, "SIGABRT", -6, "SI_TKILL", fault=None)))
        self.assertEqual(field_of(none, "tomb.signal", "fault_at"), "none")
        self.assertEqual(field_of(none, "tomb.signal", "fault"), "none")

    def test_a_mapping_name_is_the_bytes_the_kernel_gave(self):
        for name in ("/tmp/file", "/tmp/file ", "/tmp/file\t", "/tmp/ file", "[anon:a b]", "[anon:\udcff\udcfe]",
                     "/tmp/café", "[anon:" + "x" * 600 + "]", "[anon:x6  0 x4  0]"):
            with self.subTest(name=name[:20]):
                load = 0x7ff1ae500000
                recs = digest(tombstone_of(load=load, extra_mappings=[pb.mapping(load + 0x700000, load + 0x701000, name)],
                                           registers={"x4": load + 0x700020}))
                self.assertEqual(field_of(recs, "tomb.regs", "x4"), Named(name, 0x20))

    def test_a_file_is_named_by_its_whole_path(self):
        load = 0x7ff1ae500000
        maps = [pb.mapping(load + 0x700000, load + 0x701000, "/a/libz.so", "r-x"),
                pb.mapping(load + 0x710000, load + 0x711000, "/b/libz.so", "r-x")]
        one = digest(tombstone_of(load=load, extra_mappings=maps, registers={"x4": load + 0x700020}))
        two = digest(tombstone_of(load=load, extra_mappings=maps, registers={"x4": load + 0x710020}))
        self.assertEqual(field_of(one, "tomb.regs", "x4"), Named("/a/libz.so", 0x20))
        self.assertEqual(field_of(two, "tomb.regs", "x4"), Named("/b/libz.so", 0x20))

    def test_only_the_stack_name_with_a_thread_id_loses_the_thread_id(self):
        load = 0x7ff1ae500000
        names = {"[anon:stack_and_tls:2040]": "[anon:stack_and_tls:*]", "[anon:stack_and_tls:2040x]": None,
                 "[anon:stack_and_tls:]": None, "[anon:other:12]": None, "[anon:stack_and_tls:1][anon:stack_and_tls:2]": None}
        for name, shown in names.items():
            with self.subTest(name=name):
                recs = digest(tombstone_of(load=load, stack_name=name))
                self.assertEqual(field_of(recs, "tomb.regs", "x7").identity, shown or name)


class StackTest(unittest.TestCase):
    """A thread's stack is named by its thread id, which a run chooses: the
    place is told from the other stacks by its position among them."""

    LOADS = (0x700000000000, 0x710000000000, 0x720000000000)
    NAMES = "[anon:stack_and_tls:{}]"

    def run_of(self, load, first=7040, second=7041, at=(0x300000, 0x20), extra=(), swap=False):
        """Two thread stacks, the lower named for thread `first` and the
        higher for `second` (the other way round with `swap`), and x6 at the
        offset `at[1]` into the mapping that starts `at[0]` above the load."""
        low, high = (second, first) if swap else (first, second)
        maps = [pb.mapping(load + 0x500000, load + 0x600000, self.NAMES.format(high))] + list(extra)
        return digest(tombstone_of(load=load, stack_name=self.NAMES.format(low), extra_mappings=maps,
                                   extra_threads=[pb.sibling_thread(first), pb.sibling_thread(second)],
                                   registers={"x6": load + at[0] + at[1]}))

    def test_a_stack_is_its_place_among_the_stacks_and_not_its_thread_id(self):
        for first, second, swap in ((7040, 7041, False), (31, 9000, False), (7040, 7041, True)):
            with self.subTest(first=first, second=second, swap=swap):
                lower = field_of(self.run_of(self.LOADS[0], first, second, swap=swap), "tomb.regs", "x6")
                upper = field_of(self.run_of(self.LOADS[1], first, second, at=(0x500000, 0x20), swap=swap),
                                 "tomb.regs", "x6")
                self.assertEqual(lower, Named(rec.STACK_IDENTITY, 0x20, 0, 2))
                self.assertEqual(upper, Named(rec.STACK_IDENTITY, 0x20, 1, 2))

    def test_the_only_stack_is_the_place_it_always_was(self):
        recs = digest(tombstone_of(load=self.LOADS[0]))
        self.assertEqual(field_of(recs, "tomb.regs", "x7"), Named(rec.STACK_IDENTITY, 0xf8c00))
        self.assertEqual(field_of(recs, "tomb.regs", "x7"), Named(rec.STACK_IDENTITY, 0xf8c00, 0, 1))

    def test_the_mappings_of_one_thread_id_are_one_stack_and_the_offset_is_from_the_lowest(self):
        load = self.LOADS[0]
        guard = pb.mapping(load + 0x500000, load + 0x501000, self.NAMES.format(7041), "---")
        stack = pb.mapping(load + 0x501000, load + 0x600000, self.NAMES.format(7041))
        recs = digest(tombstone_of(load=load, stack_name=self.NAMES.format(7040), extra_mappings=[guard, stack],
                                   registers={"x6": load + 0x500010, "x7": load + 0x501010}))
        self.assertEqual(field_of(recs, "tomb.regs", "x6"), Named(rec.STACK_IDENTITY, 0x10, 1, 2))
        self.assertEqual(field_of(recs, "tomb.regs", "x7"), Named(rec.STACK_IDENTITY, 0x1010, 1, 2))

    def test_the_main_threads_stack_and_other_names_are_not_thread_stacks(self):
        load = self.LOADS[0]
        for name in ("[anon:stack_and_tls:main]", "[anon:thread signal stack]", "[stack]"):
            with self.subTest(name=name):
                recs = digest(tombstone_of(load=load, stack_name=name))
                self.assertEqual(field_of(recs, "tomb.regs", "x7"), Named(name, 0xf8c00))

    def test_a_place_among_stacks_is_no_line_of_the_guest_program(self):
        with self.assertRaises(rec.RecordError):
            rec.encode_value("v", Named(rec.STACK_IDENTITY, 0x20, 1, 2))
        self.assertEqual(rec.encode_value("v", Named("libc.so", 0x20)), ["m", "libc.so", "20"])

    def test_the_two_stacks_of_the_reviewer_are_two_places(self):
        """x6 is at +0x20 in the first of two thread stacks in the baselines
        and at +0x20 in the second in the run with the component, the
        mappings unchanged. Every stack was one name, so the move passed."""
        off = [self.run_of(load, at=(0x300000, 0x20)) for load in self.LOADS]
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, self.run_of(self.LOADS[0], at=(0x500000, 0x20)))
        self.assertIn("x6", str(caught.exception))
        self.assertIn("#0/2", str(caught.exception))
        self.assertIn("#1/2", str(caught.exception))
        compared, masked = rows.compare("crash-memcpy", off, self.run_of(0x730000000000, at=(0x300000, 0x20)))
        self.assertGreater(compared, 50)
        self.assertEqual(masked, 0)

    def test_the_thread_ids_a_run_chooses_do_not_matter(self):
        off = [self.run_of(load, 7040 + n, 7041 + n) for n, load in enumerate(self.LOADS)]
        compared, _ = rows.compare("crash-memcpy", off, self.run_of(0x730000000000, 100, 5000))
        self.assertGreater(compared, 50)


class ReviewTest(unittest.TestCase):
    """The two false passes of the text reader, as protobuf files."""

    LOADS = (0x700000000000, 0x710000000000, 0x720000000000)

    def test_the_mapping_the_fault_lies_in_is_a_mapping(self):
        """A 4 KiB PROT_NONE page that x6 points into, at +0x20 in the three
        baselines (at three load addresses) and at +0x30 in the run with the
        component. The text reader dropped the page's line (the fault address
        was in it), so x6 was a plain number and the wrong result passed."""
        def run(load, offset):
            guard = load + 0x800000
            return digest(tombstone_of(load=load, guard_page=True, registers={"x6": guard + offset}))

        off = [run(load, 0x20) for load in self.LOADS]
        regs = field_of(off[0], "tomb.regs", "x6")
        self.assertEqual(regs, Unnamed(0x1000, "---", 0, 1, 0x20))
        with self.assertRaises(rows.DebugFailure) as caught:
            rows.compare("crash-memcpy", off, run(self.LOADS[0], 0x30))
        self.assertIn("x6", str(caught.exception))
        compared, masked = rows.compare("crash-memcpy", off, run(0x730000000000, 0x20))
        self.assertGreater(compared, 50)
        self.assertEqual(masked, 0)

    def test_two_mappings_that_differ_by_a_trailing_space_are_two_names(self):
        """x6 points at +0x20 into /tmp/file in the baselines and into
        /tmp/file<space> in the run with the component. The text reader
        trimmed the space and called them one name."""
        for other in ("/tmp/file ", "/tmp/file\t", "/tmp/file "):
            with self.subTest(other=other):
                def named(load, second):
                    maps = [pb.mapping(load + 0x700000, load + 0x701000, "/tmp/file"),
                            pb.mapping(load + 0x710000, load + 0x711000, other)]
                    return digest(tombstone_of(load=load, extra_mappings=maps,
                                               registers={"x6": load + (0x710000 if second else 0x700000) + 0x20}))

                off = [named(load, False) for load in self.LOADS]
                with self.assertRaises(rows.DebugFailure) as caught:
                    rows.compare("crash-memcpy", off, named(self.LOADS[0], True))
                self.assertIn("x6", str(caught.exception))
                compared, _ = rows.compare("crash-memcpy", off, named(0x730000000000, False))
                self.assertGreater(compared, 50)


if __name__ == "__main__":
    unittest.main()
