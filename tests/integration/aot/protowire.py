# SPDX-License-Identifier: GPL-2.0-or-later
"""A strict reader of protobuf's wire format, for files the AOT rows read.

Only the wire format is implemented: a message is a run of (tag, value)
pairs, a tag is a varint holding a field number and a wire type, and a value
is a varint (type 0), eight bytes (1), a length and that many bytes (2) or
four bytes (5). A message is described to the reader by a table of the field
numbers the caller uses (tombstone_pb.py has the one for tombstones); the
reader never learns a schema from the file.

What it refuses, each with the offset and the message it happened in:

- a file or a length-delimited value that ends before it should, a varint
  that does not end or does not fit 64 bits, and a length that runs past the
  message that holds it;
- a wire type that is no value type (the group types and the unassigned
  ones), and a field number of 0 or beyond 2^29 - 1;
- a field of the table that arrives with another wire type than its kind
  needs, a value that does not fit its kind (a 32-bit field with a larger
  number, a bool that is not 0 or 1, an enum number the table does not name),
  and a singular field that arrives twice (the wire format would merge the two,
  and a merge is no reading the rows can compare);
- bytes after the last field that do not make a whole field (they are read
  as one more field and end the message inside it).

A field number that the table does not name is skipped by its wire type and
counted: the caller reports the count, so that a schema that grows is seen.
Strings are the bytes of the file decoded as UTF-8 with surrogateescape: a
byte that is no UTF-8 stays the byte it was (a mapping name is whatever the
kernel gave), and records.escape writes it back as the same %xx.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from collections import Counter
from dataclasses import dataclass, field

VARINT, FIXED64, LENGTH, FIXED32 = 0, 1, 2, 5
MAX_FIELD_NUMBER = (1 << 29) - 1
MASK64 = (1 << 64) - 1


class WireError(ValueError):
    """A byte string that is not the message the table describes."""


@dataclass(frozen=True)
class Field:
    """One entry of a message's table.

    kind is one of: u32, u64, i32, i64, bool, enum (varint values), string,
    bytes (length-delimited), message (a nested message named by `message`),
    map (a repeated entry message of a key kind and a message value).
    """
    name: str
    kind: str
    message: str = ""
    repeated: bool = False
    key: str = ""
    values: tuple[int, ...] = ()


@dataclass
class Reading:
    """What a decode found: the values by field name, and the numbers of the
    fields that were skipped as (message, number) counts."""
    values: dict = field(default_factory=dict)
    skipped: Counter = field(default_factory=Counter)


WIRE_OF_KIND = {"u32": VARINT, "u64": VARINT, "i32": VARINT, "i64": VARINT, "bool": VARINT, "enum": VARINT,
                "string": LENGTH, "bytes": LENGTH, "message": LENGTH, "map": LENGTH}


def read_varint(data: bytes, at: int, end: int, where: str) -> tuple[int, int]:
    """The varint at `at`: its value and the offset after it."""
    value = 0
    for index in range(10):
        if at >= end:
            raise WireError(f"{where}: a varint that is cut short at offset {at}")
        byte = data[at]
        at += 1
        # The tenth byte holds the 64th bit and no more, and ends the varint.
        if index == 9 and byte > 1:
            raise WireError(f"{where}: a varint that does not fit 64 bits, ending at offset {at}")
        value |= (byte & 0x7f) << (7 * index)
        if not byte & 0x80:
            return value, at
    raise AssertionError("a tenth byte of at most 1 ends the varint")


def scalar(kind: str, value: int, where: str, field_name: str, allowed: tuple[int, ...]) -> int:
    """A varint as the value of a field of `kind`."""
    if kind == "u64":
        return value
    if kind == "u32":
        if value > 0xffffffff:
            raise WireError(f"{where}: {field_name} is {value}, which does not fit 32 bits")
        return value
    if kind in ("i32", "i64"):
        signed = value - (1 << 64) if value >= (1 << 63) else value
        bits = 31 if kind == "i32" else 63
        if not -(1 << bits) <= signed < (1 << bits):
            raise WireError(f"{where}: {field_name} is {signed}, which does not fit {kind}")
        return signed
    if kind == "bool":
        if value > 1:
            raise WireError(f"{where}: {field_name} is {value}, not a bool")
        return value
    if kind == "enum":
        if allowed and value not in allowed:
            raise WireError(f"{where}: {field_name} is {value}, which the table does not name")
        return value
    raise WireError(f"{where}: {field_name} has no scalar kind {kind}")


def default_of(entry: Field):
    if entry.repeated or entry.kind == "map":
        return []
    if entry.kind == "string":
        return ""
    if entry.kind == "bytes":
        return b""
    if entry.kind == "message":
        return None
    if entry.kind == "bool":
        return 0
    return 0


def decode(data: bytes, root: str, tables: dict[str, dict[int, Field]]) -> Reading:
    """The message `root` in all of `data`."""
    reading = Reading()
    reading.values = decode_message(bytes(data), 0, len(data), root, tables, reading.skipped, root)
    return reading


def decode_message(data: bytes, start: int, end: int, name: str, tables: dict[str, dict[int, Field]],
                   skipped: Counter, where: str) -> dict:
    table = tables[name]
    values: dict = {}
    for entry in table.values():
        values[entry.name] = default_of(entry)
    seen: set[str] = set()
    at = start
    while at < end:
        tag, after = read_varint(data, at, end, where)
        number, wire = tag >> 3, tag & 7
        if number == 0 or number > MAX_FIELD_NUMBER:
            raise WireError(f"{where}: field number {number} at offset {at}")
        if wire not in (VARINT, FIXED64, LENGTH, FIXED32):
            raise WireError(f"{where}: wire type {wire} (field {number}) at offset {at} is no value type")
        at = after
        if wire == VARINT:
            raw, at = read_varint(data, at, end, f"{where} field {number}")
            payload = raw
        elif wire == FIXED64:
            if at + 8 > end:
                raise WireError(f"{where} field {number}: eight bytes expected at offset {at}, {end - at} left")
            payload = int.from_bytes(data[at:at + 8], "little")
            at += 8
        elif wire == FIXED32:
            if at + 4 > end:
                raise WireError(f"{where} field {number}: four bytes expected at offset {at}, {end - at} left")
            payload = int.from_bytes(data[at:at + 4], "little")
            at += 4
        else:
            length, at = read_varint(data, at, end, f"{where} field {number}")
            if length > end - at:
                raise WireError(f"{where} field {number}: a length of {length} at offset {at} runs past "
                                f"the {end - at} bytes of its message")
            payload = (at, at + length)
            at += length
        entry = table.get(number)
        if entry is None:
            skipped[(name, number)] += 1
            continue
        if WIRE_OF_KIND[entry.kind] != wire:
            raise WireError(f"{where}: field {number} ({entry.name}) arrives with wire type {wire}, "
                            f"which a {entry.kind} is not")
        label = f"{where}.{entry.name}"
        if entry.kind in ("string", "bytes"):
            low, high = payload
            raw_bytes = data[low:high]
            value = raw_bytes if entry.kind == "bytes" else raw_bytes.decode("utf-8", "surrogateescape")
        elif entry.kind == "message":
            low, high = payload
            value = decode_message(data, low, high, entry.message, tables, skipped, label)
        elif entry.kind == "map":
            low, high = payload
            value = decode_entry(data, low, high, entry, tables, skipped, label)
        else:
            value = scalar(entry.kind, payload, where, entry.name, entry.values)
        if entry.repeated or entry.kind == "map":
            values[entry.name].append(value)
        else:
            if entry.name in seen:
                raise WireError(f"{where}: the singular field {number} ({entry.name}) arrives twice")
            seen.add(entry.name)
            values[entry.name] = value
    return values


def decode_entry(data: bytes, start: int, end: int, entry: Field, tables: dict[str, dict[int, Field]],
                 skipped: Counter, where: str) -> tuple[int, dict]:
    """One entry of a map field: its key (field 1) and its message (field 2),
    each exactly once."""
    key = None
    message = None
    at = start
    while at < end:
        tag, at = read_varint(data, at, end, where)
        number, wire = tag >> 3, tag & 7
        if number == 1 and wire == VARINT:
            raw, at = read_varint(data, at, end, where)
            if key is not None:
                raise WireError(f"{where}: a map entry with two keys")
            key = scalar(entry.key, raw, where, "key", ())
        elif number == 2 and wire == LENGTH:
            length, at = read_varint(data, at, end, where)
            if length > end - at:
                raise WireError(f"{where}: a map entry whose message of {length} bytes runs past the "
                                f"{end - at} left")
            if message is not None:
                raise WireError(f"{where}: a map entry with two values")
            message = decode_message(data, at, at + length, entry.message, tables, skipped, f"{where}[value]")
            at += length
        else:
            raise WireError(f"{where}: a map entry with field {number} of wire type {wire}")
    if key is None or message is None:
        raise WireError(f"{where}: a map entry without its {'key' if key is None else 'message'}")
    return key, message


def describe(skipped: Counter) -> str:
    """The skipped field numbers as `Message.number xcount`, in order."""
    return ", ".join(f"{message}.{number} x{count}" for (message, number), count in sorted(skipped.items())) \
        or "none"
