# SPDX-License-Identifier: GPL-2.0-or-later
"""The record model of the AOT debugger and crash-report rows, and the wire
encoding the guest program prints it in.

Everything the rows compare is a field of a record: a (record kind, field
name, value) triple whose value is a number, a place (a mapping and the
offset in it) or a text that is only ever compared whole. Nothing is compared,
masked or counted as a whitespace token or a line of text.

The guest program (aot_guest_debug.cpp, through guest_records.hpp) prints one
field per line:

    AOT <TAB> kind <TAB> field <TAB> type <TAB> value...

The types are n (an unsigned number, lowercase hexadecimal), i (a signed
number, decimal), t (text), m (a place in a named mapping: its name, the
offset in hex) and u (a place in an unnamed mapping: size, protection, place
among the mappings of that shape, how many there are, the offset in hex).
Text is escaped: every byte outside SAFE is written %xx, so that nothing in a
value can be taken for the separator, a field label or the end of the line.
decode() accepts a line only if encoding what it read gives the same line, a
record only if it is whole and its fields are those of its kind's schema, in
order, once each. A tombstone is outside text: debug_rows.digest_tombstone is
the one reader of it and builds the same records.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
from dataclasses import dataclass

PREFIX = "AOT"
SEPARATOR = "\t"
SAFE = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+/:[]-")
IDENTIFIER = re.compile(r"[a-z][a-z0-9_.]*")
PROTECTION = re.compile(r"[-r][-w][-x]")
HEX = re.compile(r"(?:0|[1-9a-f][0-9a-f]*)")
DECIMAL = re.compile(r"-?(?:0|[1-9][0-9]*)")
MASK64 = (1 << 64) - 1


class RecordError(ValueError):
    """A line or a record that is not exactly what the encoding says."""


@dataclass(frozen=True)
class Named:
    """A place in a named mapping: a file by its base name (a tombstone's by
    its whole path), or the mapping's own name, and the offset in it. A
    thread's stack in a tombstone is one of several mappings that carry one
    name once its thread id is left out: which one it is among them (lowest
    address first) and how many there are are part of the place (`ordinal` and
    `count`); everything else is the only mapping of its name, 0 of 1."""
    identity: str
    offset: int
    ordinal: int = 0
    count: int = 1


@dataclass(frozen=True)
class Unnamed:
    """A place in an unnamed mapping: its shape (size and protection), its
    place among the unnamed mappings of that shape, lowest address first, how
    many there are of them, and the offset in it."""
    size: int
    protection: str
    ordinal: int
    count: int
    offset: int

    def shape(self) -> str:
        return f"anon[{self.size:#x},{self.protection}]"

    def __str__(self) -> str:
        return f"{self.shape()}#{self.ordinal}/{self.count}+{self.offset:#x}"


Value = int | str | Named | Unnamed


def describe(value: Value) -> str:
    """A value as a person reads it (never as something compared)."""
    if isinstance(value, bool):
        raise RecordError(f"{value!r} is no value")
    if isinstance(value, int):
        return f"{value:#x}"
    if isinstance(value, str):
        return repr(value)
    if isinstance(value, Named):
        among = f"#{value.ordinal}/{value.count}" if value.count != 1 else ""
        return f"{value.identity}{among}+{value.offset:#x}"
    return str(value)


def check_value(value: object) -> Value:
    if isinstance(value, bool) or not isinstance(value, (int, str, Named, Unnamed)):
        raise RecordError(f"{value!r} is no value of a record")
    return value  # type: ignore[return-value]


@dataclass(frozen=True)
class Record:
    """A kind and its fields: names that are unique within the record, each
    with a value."""
    kind: str
    fields: tuple[tuple[str, Value], ...]

    def __post_init__(self) -> None:
        names = [name for name, _ in self.fields]
        if len(set(names)) != len(names):
            duplicate = next(name for name in names if names.count(name) > 1)
            raise RecordError(f"{self.kind}: the field {duplicate} is seen twice")
        for _, value in self.fields:
            check_value(value)

    def names(self) -> tuple[str, ...]:
        return tuple(name for name, _ in self.fields)

    def get(self, name: str) -> Value:
        for field, value in self.fields:
            if field == name:
                return value
        raise RecordError(f"{self.kind}: no field {name}")

    def __str__(self) -> str:
        return f"{self.kind}(" + ", ".join(f"{name}={describe(value)}" for name, value in self.fields) + ")"


def record(kind: str, **fields: Value) -> Record:
    return Record(kind, tuple(fields.items()))


# ---- the schema of the guest program's records -----------------------------

# Field types: n unsigned number, i signed number, t text, v a value (a number
# or a place).
REGISTERS = tuple(f"x{n:02d}" for n in range(31)) + ("sp", "pc", "nzcv")

SCHEMA: dict[str, tuple[tuple[str, str], ...]] = {
    "randomization": (("state", "t"),),
    "done": (("mode", "t"),),
    "note": (("text", "t"),),
    "warm": (("sum", "n"),),
    "attached": (("pc", "v"),),
    "breakpoint": (("what", "t"), ("hit", "v"), ("at_breakpoint", "t")),
    "regs": (("tag", "t"),) + tuple((name, "v") for name in REGISTERS[:-1]) + (("nzcv", "n"),),
    "returns_to": (("location", "v"),),
    "step": (("n", "i"), ("pc", "v")),
    "returned": (("pc", "v"), ("at_return", "t")),
    "changed": (("what", "t"), ("verdict", "t")),
    "transition": (("step", "i"), ("from", "t"), ("to", "t"), ("pc", "v")),
    "steps": (("steps", "i"), ("changes", "i"), ("libc", "i")),
    "tracee_done": (("sum", "n"),),
    "tracee_ended": (("status", "n"),),
    "detached": (("status", "n"),),
    "sibling_entry": (("round", "t"), ("hit", "t"), ("by_sibling", "t"), ("pc_at_breakpoint", "t"),
                      ("in_libz", "t"), ("code", "i"), ("pc", "v")),
    "sibling_loop": (("round", "t"), ("hit", "t"), ("by_sibling", "t"), ("pc_at_breakpoint", "t"),
                     ("in_libz", "t"), ("code", "i")),
    "sibling_runs_on": (("round", "t"), ("yes", "t")),
}


# ---- text ------------------------------------------------------------------

def escape(text: str) -> str:
    """Every byte of the text's UTF-8 outside SAFE as %xx; a byte that is no
    UTF-8 travels as the lone surrogate surrogateescape gives it."""
    out = []
    for byte in text.encode("utf-8", "surrogateescape"):
        char = chr(byte)
        out.append(char if byte < 0x80 and char in SAFE else f"%{byte:02x}")
    return "".join(out)


def unescape(text: str) -> str:
    out = bytearray()
    index = 0
    while index < len(text):
        char = text[index]
        if char == "%":
            digits = text[index + 1:index + 3]
            if len(digits) != 2 or not re.fullmatch(r"[0-9a-f]{2}", digits):
                raise RecordError(f"{text!r}: a % that is not followed by two lowercase hexadecimal digits")
            byte = int(digits, 16)
            if byte < 0x80 and chr(byte) in SAFE:
                raise RecordError(f"{text!r}: %{digits} escapes a byte that is not escaped")
            out.append(byte)
            index += 3
        elif char in SAFE:
            out.append(ord(char))
            index += 1
        else:
            raise RecordError(f"{text!r}: {char!r} is not written %xx")
    return out.decode("utf-8", "surrogateescape")


# ---- lines -----------------------------------------------------------------

def encode_value(kind_type: str, value: Value) -> list[str]:
    """The columns after the type of one field: `kind_type` is the schema's n,
    i, t or v."""
    check_value(value)
    if isinstance(value, Named):
        if kind_type != "v":
            raise RecordError(f"{value} is a place, not a {kind_type}")
        if not 0 <= value.offset <= MASK64 or (value.ordinal, value.count) != (0, 1):
            raise RecordError(f"{value} is no place the guest program prints")
        return ["m", escape(value.identity), f"{value.offset:x}"]
    if isinstance(value, Unnamed):
        if kind_type != "v":
            raise RecordError(f"{value} is a place, not a {kind_type}")
        if (not PROTECTION.fullmatch(value.protection) or not 0 <= value.ordinal < value.count
                or not 0 <= value.size <= MASK64 or not 0 <= value.offset <= MASK64):
            raise RecordError(f"{value} is no place")
        return ["u", f"{value.size:x}", value.protection, str(value.ordinal), str(value.count),
                f"{value.offset:x}"]
    if isinstance(value, str):
        if kind_type != "t":
            raise RecordError(f"{value!r} is a text, not a {kind_type}")
        return ["t", escape(value)]
    if kind_type == "i":
        if not -(1 << 63) <= value < (1 << 63):
            raise RecordError(f"{value} is no signed number")
        return ["i", str(value)]
    if kind_type not in ("n", "v") or not 0 <= value <= MASK64:
        raise RecordError(f"{value} is no unsigned number of the field")
    return ["n", f"{value:x}"]


def encode(kind: str, field_name: str, kind_type: str, value: Value) -> str:
    return SEPARATOR.join([PREFIX, kind, field_name] + encode_value(kind_type, value))


def encode_record(rec: Record, schema: dict[str, tuple[tuple[str, str], ...]] | None = None) -> list[str]:
    """The lines of a record: every field of its kind's schema, in order."""
    schema = SCHEMA if schema is None else schema
    if rec.kind not in schema:
        raise RecordError(f"no record kind {rec.kind}")
    if rec.names() != tuple(name for name, _ in schema[rec.kind]):
        raise RecordError(f"{rec.kind}: the fields are {rec.names()}, not those of the schema")
    return [encode(rec.kind, name, kind_type, value)
            for (name, kind_type), (_, value) in zip(schema[rec.kind], rec.fields)]


def decode_line(line: str) -> tuple[str, str, str, Value]:
    """One field line: (kind, field, schema type it fits, value). A line is
    accepted only if encoding what was read gives the line back."""
    columns = line.split(SEPARATOR)
    if len(columns) < 5 or columns[0] != PREFIX:
        raise RecordError(f"not a record line: {line!r}")
    kind, field_name, type_char = columns[1], columns[2], columns[3]
    rest = columns[4:]
    if not IDENTIFIER.fullmatch(kind) or not IDENTIFIER.fullmatch(field_name):
        raise RecordError(f"{line!r}: the kind or the field is no identifier")
    value: Value
    fits = type_char
    if type_char == "n":
        if len(rest) != 1 or not HEX.fullmatch(rest[0]) or len(rest[0]) > 16:
            raise RecordError(f"{line!r}: a number is lowercase hexadecimal of at most 16 digits")
        value = int(rest[0], 16)
    elif type_char == "i":
        if len(rest) != 1 or not DECIMAL.fullmatch(rest[0]):
            raise RecordError(f"{line!r}: a signed number is decimal")
        value = int(rest[0])
        if not -(1 << 63) <= value < (1 << 63):
            raise RecordError(f"{line!r}: the signed number is out of range")
    elif type_char == "t":
        if len(rest) != 1:
            raise RecordError(f"{line!r}: a text is one column")
        value = unescape(rest[0])
    elif type_char == "m":
        if len(rest) != 2 or not HEX.fullmatch(rest[1]) or len(rest[1]) > 16:
            raise RecordError(f"{line!r}: a place in a named mapping is a name and an offset")
        value = Named(unescape(rest[0]), int(rest[1], 16))
        fits = "v"
    elif type_char == "u":
        if (len(rest) != 5 or not HEX.fullmatch(rest[0]) or len(rest[0]) > 16 or not PROTECTION.fullmatch(rest[1])
                or not re.fullmatch(r"(?:0|[1-9][0-9]*)", rest[2]) or not re.fullmatch(r"(?:0|[1-9][0-9]*)", rest[3])
                or not HEX.fullmatch(rest[4]) or len(rest[4]) > 16):
            raise RecordError(f"{line!r}: a place in an unnamed mapping is size, protection, place, count, offset")
        value = Unnamed(int(rest[0], 16), rest[1], int(rest[2]), int(rest[3]), int(rest[4], 16))
        if value.ordinal >= value.count:
            raise RecordError(f"{line!r}: the place is not among that many mappings")
        fits = "v"
    else:
        raise RecordError(f"{line!r}: no type {type_char!r}")
    if type_char == "n":
        fits = "n"
    if encode(kind, field_name, "v" if type_char in "mu" else type_char, value) != line:
        raise RecordError(f"{line!r}: not the encoding of what it says")
    return kind, field_name, fits, value


def decode(lines: list[str], schema: dict[str, tuple[tuple[str, str], ...]] | None = None) -> list[Record]:
    """The records of the lines, which are all records: a line that does not
    decode exactly, a record of an unknown kind, a field that is unknown,
    seen twice, missing or out of order, a value of another type than the
    field's, and a record that ends before its last field, are failures."""
    schema = SCHEMA if schema is None else schema
    records: list[Record] = []
    kind: str | None = None
    fields: list[tuple[str, Value]] = []
    for number, line in enumerate(lines, 1):
        try:
            line_kind, line_field, fits, value = decode_line(line)
        except RecordError as problem:
            raise RecordError(f"line {number}: {problem}") from None
        if kind is not None and line_kind != kind:
            raise RecordError(f"line {number}: the {kind} record stops after {len(fields)} of its "
                              f"{len(schema[kind])} fields, and a {line_kind} record begins")
        if line_kind not in schema:
            raise RecordError(f"line {number}: no record kind {line_kind!r}")
        names = schema[line_kind]
        if line_field != names[len(fields)][0]:
            known = [name for name, _ in names]
            what = ("seen twice" if line_field in [name for name, _ in fields] else
                    "unknown" if line_field not in known else "out of order or missing a field before it")
            raise RecordError(f"line {number}: the {line_kind} field {line_field} is {what} "
                              f"(the record expects {names[len(fields)][0]})")
        wanted = names[len(fields)][1]
        if fits != wanted and not (wanted == "v" and fits in ("n", "v")):
            raise RecordError(f"line {number}: {line_kind}.{line_field} is a {fits}, not a {wanted}")
        kind = line_kind
        fields.append((line_field, value))
        if len(fields) == len(names):
            records.append(Record(kind, tuple(fields)))
            kind, fields = None, []
    if kind is not None:
        raise RecordError(f"the {kind} record is cut short after {len(fields)} of its {len(schema[kind])} fields")
    return records


# ---- where a value points --------------------------------------------------

Mapping = tuple[int, int, int, str, str]  # low, high, file offset, name, protection


# A thread's stack is named by its thread id, which differs between runs.
STACK_NAME = re.compile(r"\[anon:stack_and_tls:[0-9]+\]")
# What a stack is called without its thread id.
STACK_IDENTITY = "[anon:stack_and_tls:*]"


def resolve(value: int, maps: list[Mapping], thread_stacks: bool = False, keep_paths: bool = False) -> Value:
    """Where a value points as the run does not choose it: a file by its base
    name (its whole path with `keep_paths`) and the offset in the file, a
    named mapping by its name and the offset in it, an unnamed mapping by its
    shape, its place among that shape (lowest address first; `maps` is in
    address order), how many there are, and the offset in it. The number
    itself where no mapping holds it. The place is the same place in two runs
    only if they agree on how many there are (compare checks that). With
    `thread_stacks` a thread's stack (STACK_NAME) is called STACK_IDENTITY, not
    by its thread id, and told from the other stacks by its place among them
    (lowest address first; the mappings that carry one thread id are one
    stack, and the offset is from the lowest of them) and how many there
    are."""
    counts: dict[tuple[int, str], int] = {}
    for low, high, _, name, protection in maps:
        if not name:
            counts[(high - low, protection[:3])] = counts.get((high - low, protection[:3]), 0) + 1
    stacks: dict[str, tuple[int, int]] = {}
    if thread_stacks:
        for low, _, _, name, _ in maps:
            if STACK_NAME.fullmatch(name) and name not in stacks:
                stacks[name] = (len(stacks), low)
    seen: dict[tuple[int, str], int] = {}
    for low, high, offset, name, protection in maps:
        shape = (high - low, protection[:3])
        ordinal = 0
        if not name:
            ordinal = seen.get(shape, 0)
            seen[shape] = ordinal + 1
        if low <= value < high:
            if name.startswith("/"):
                return Named(name if keep_paths else name.rsplit("/", 1)[1], offset + value - low)
            if name in stacks:
                place, lowest = stacks[name]
                return Named(STACK_IDENTITY, value - lowest, place, len(stacks))
            if name:
                return Named(name, value - low)
            return Unnamed(high - low, protection[:3], ordinal, counts[shape], value - low)
    return value
