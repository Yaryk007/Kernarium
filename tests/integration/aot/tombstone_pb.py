# SPDX-License-Identifier: GPL-2.0-or-later
"""Android's protobuf tombstone, read into the records the rows compare.

The crash reporter writes every tombstone twice: as text and as a protobuf
file beside it (tombstone_NN.pb). The text is for a person. The rows read the
protobuf: a strict decoder of the wire format (protowire.py) driven by the
table below, and a model that builds the same records the rows use for the
guest program's own output (records.py): the process, the signal, the crashing
thread's registers, each frame, the code around the pc and the lr, and the
other threads. Nothing reads text.

A register's value that points into a mapping is located against the mapping
list of the file itself: a place in a named mapping (its exact name, which
file for a file), or in an unnamed one (its size, protection, place among the
unnamed mappings of that shape in address order, how many there are, and the
offset in it). The list is put in address order first, so the order the file
holds it in is not a fact of the guest, and a list whose entries overlap, run
backwards or repeat a begin address is no list.

The numbers in TABLES follow the tombstone.proto of the source tree the image
is built from; the message in each table's comment is that proto's message.
Only the fields the model reads are listed. A field number that is not listed
is skipped by its wire type and counted (Digest.unread), so a schema that
grows is reported by the row that read the file.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from collections import Counter

import protowire
import records
from protowire import Field, WireError
from records import Record, Value

ARCHITECTURES = {0: "arm32", 1: "arm64", 2: "x86", 3: "x86_64", 4: "riscv64", 5: "none"}

TABLES: dict[str, dict[int, Field]] = {
    # Tombstone
    "Tombstone": {
        1: Field("arch", "enum", values=tuple(ARCHITECTURES)),
        2: Field("build_fingerprint", "string"),
        3: Field("revision", "string"),
        5: Field("pid", "u32"),
        6: Field("tid", "u32"),
        7: Field("uid", "u32"),
        8: Field("selinux_label", "string"),
        9: Field("command_line", "string", repeated=True),
        10: Field("signal_info", "message", message="Signal"),
        14: Field("abort_message", "string"),
        16: Field("threads", "map", message="Thread", key="u32"),
        17: Field("memory_mappings", "message", message="MemoryMapping", repeated=True),
        22: Field("page_size", "u32"),
        23: Field("has_been_16kb_mode", "bool"),
        24: Field("guest_arch", "enum", values=tuple(ARCHITECTURES)),
        27: Field("executable_name", "string"),
        28: Field("kernel_release", "string"),
        29: Field("ppid", "u32"),
        1000: Field("lineage_version", "string"),
    },
    # Signal
    "Signal": {
        1: Field("number", "i32"),
        2: Field("name", "string"),
        3: Field("code", "i32"),
        4: Field("code_name", "string"),
        5: Field("has_sender", "bool"),
        8: Field("has_fault_address", "bool"),
        9: Field("fault_address", "u64"),
    },
    # Thread
    "Thread": {
        1: Field("id", "i32"),
        2: Field("name", "string"),
        3: Field("registers", "message", message="Register", repeated=True),
        4: Field("current_backtrace", "message", message="BacktraceFrame", repeated=True),
        5: Field("memory_dump", "message", message="MemoryDump", repeated=True),
        6: Field("tagged_addr_ctrl", "i64"),
        8: Field("pac_enabled_keys", "i64"),
    },
    # Register
    "Register": {
        1: Field("name", "string"),
        2: Field("u64", "u64"),
    },
    # BacktraceFrame
    "BacktraceFrame": {
        1: Field("rel_pc", "u64"),
        4: Field("function_name", "string"),
        5: Field("function_offset", "u64"),
        6: Field("file_name", "string"),
        7: Field("file_map_offset", "u64"),
        8: Field("build_id", "string"),
    },
    # MemoryDump
    "MemoryDump": {
        1: Field("register_name", "string"),
        2: Field("mapping_name", "string"),
        3: Field("begin_address", "u64"),
        4: Field("memory", "bytes"),
    },
    # MemoryMapping
    "MemoryMapping": {
        1: Field("begin_address", "u64"),
        2: Field("end_address", "u64"),
        3: Field("offset", "u64"),
        4: Field("read", "bool"),
        5: Field("write", "bool"),
        6: Field("execute", "bool"),
        7: Field("mapping_name", "string"),
    },
}

ARM64 = 1
# The registers of an arm64 crash, in the order the file lists them; esr and
# vg are listed after them when the reporter has them.
REQUIRED_REGISTERS = tuple(f"x{n}" for n in range(30)) + ("lr", "sp", "pc", "pst")
OPTIONAL_REGISTERS = ("esr", "vg")

class TombstoneError(ValueError):
    """A tombstone that decodes but is not one the model can use."""


class Digest(list):
    """The records of a tombstone, and what the reader skipped."""

    unread: Counter

    def __init__(self, items=(), unread: Counter | None = None) -> None:
        super().__init__(items)
        self.unread = unread if unread is not None else Counter()


def read(data: bytes) -> Digest:
    """The records of the tombstone file `data`, or an error that says what
    in it is not what the model needs."""
    reading = protowire.decode(data, "Tombstone", TABLES)
    return Digest(build(reading.values), reading.skipped)


def mappings_of(tomb: dict) -> list[records.Mapping]:
    """The memory mappings in address order, each (low, high, file offset,
    name, protection)."""
    maps = []
    for number, item in enumerate(tomb["memory_mappings"]):
        low, high = item["begin_address"], item["end_address"]
        if high <= low:
            raise TombstoneError(f"mapping {number} ends at {high:#x}, not after its begin {low:#x}")
        protection = ("r" if item["read"] else "-") + ("w" if item["write"] else "-") + \
                     ("x" if item["execute"] else "-")
        maps.append((low, high, item["offset"], item["mapping_name"], protection))
    maps.sort(key=lambda each: (each[0], each[1]))
    for before, after in zip(maps, maps[1:]):
        if after[0] < before[1]:
            raise TombstoneError(f"the mappings {before[0]:#x}-{before[1]:#x} and {after[0]:#x}-{after[1]:#x} "
                                 f"overlap")
    if not maps:
        raise TombstoneError("the tombstone lists no memory mapping")
    return maps


def threads_of(tomb: dict) -> tuple[dict, list[dict]]:
    """The crashing thread (the entry of the thread map that is the
    tombstone's own tid) and the other threads in thread id order."""
    threads: dict[int, dict] = {}
    for key, thread in tomb["threads"]:
        if key in threads:
            raise TombstoneError(f"the thread map holds thread {key} twice")
        threads[key] = thread
    tid = tomb["tid"]
    if tid not in threads:
        raise TombstoneError(f"the thread map has no entry for the crashing thread {tid}")
    if threads[tid]["id"] != tid:
        raise TombstoneError(f"the entry for thread {tid} says it is thread {threads[tid]['id']}")
    return threads[tid], [threads[key] for key in sorted(threads) if key != tid]


def build(tomb: dict) -> list[Record]:
    """The records of a decoded Tombstone, in a fixed order."""
    if tomb["signal_info"] is None:
        raise TombstoneError("the tombstone has no signal_info")
    if not tomb["lineage_version"]:
        # The reporter writes the fields in the order of their numbers, and this
        # is the last: a file that ends before it was cut short, at a field
        # boundary or not.
        raise TombstoneError("the tombstone ends before its last field (lineage_version)")
    if tomb["arch"] != ARM64:
        raise TombstoneError(f"the tombstone's architecture is {ARCHITECTURES[tomb['arch']]}, not arm64")
    maps = mappings_of(tomb)
    crashing, others = threads_of(tomb)

    def where(value: int) -> Value:
        return records.resolve(value, maps, thread_stacks=True, keep_paths=True)

    signal = tomb["signal_info"]
    out: list[Record] = [Record("tomb.head", (
        ("arch", ARCHITECTURES[tomb["arch"]]), ("lineage", tomb["lineage_version"]),
        ("fingerprint", tomb["build_fingerprint"]), ("kernel", tomb["kernel_release"]),
        ("revision", tomb["revision"]), ("uid", tomb["uid"]), ("selinux", tomb["selinux_label"]),
        ("page_size", tomb["page_size"]), ("kernel_16k", "yes" if tomb["has_been_16kb_mode"] else "no")))]
    for index, argument in enumerate(tomb["command_line"]):
        out.append(Record("tomb.cmdline", (("index", index), ("argument", argument))))
    out.append(Record("tomb.process", (
        ("pid", tomb["pid"]), ("ppid", tomb["ppid"]), ("tid", tomb["tid"]), ("name", crashing["name"]),
        ("executable", tomb["executable_name"]))))
    out.append(Record("tomb.state", (("tagged_addr_ctrl", crashing["tagged_addr_ctrl"]),
                                     ("pac_enabled_keys", crashing["pac_enabled_keys"]))))
    fault = signal["fault_address"] if signal["has_fault_address"] else None
    fault_at: Value = "none" if fault is None else where(fault)
    if isinstance(fault_at, int):
        fault_at = "outside every mapping"
    out.append(Record("tomb.signal", (
        ("signo", signal["number"]), ("name", signal["name"]), ("code", signal["code"]),
        ("code_name", signal["code_name"]), ("sender", "yes" if signal["has_sender"] else "no"),
        ("fault", "none" if fault is None else "address"),
        ("page_offset", 0 if fault is None else fault & 0xfff), ("fault_at", fault_at))))
    out.append(Record("tomb.abort", (("message", tomb["abort_message"]),)))

    registers: list[tuple[str, Value]] = []
    for item in crashing["registers"]:
        name = item["name"]
        if name in dict(registers):
            raise TombstoneError(f"the register {name} of the crashing thread is listed twice")
        if name not in REQUIRED_REGISTERS and name not in OPTIONAL_REGISTERS:
            raise TombstoneError(f"the crashing thread has a register {name!r} the model does not know")
        registers.append((name, where(item["u64"])))
    missing = [name for name in REQUIRED_REGISTERS if name not in dict(registers)]
    if missing:
        raise TombstoneError("the crashing thread lacks the registers " + ", ".join(missing))
    out.append(Record("tomb.regs", tuple(registers)))

    out.append(Record("tomb.frames", (("count", len(crashing["current_backtrace"])),)))
    for index, frame in enumerate(crashing["current_backtrace"]):
        out.append(Record("tomb.frame", (
            ("index", index), ("rel_pc", frame["rel_pc"]), ("file", frame["file_name"]),
            ("function", frame["function_name"]), ("function_offset", frame["function_offset"]),
            ("file_offset", frame["file_map_offset"]), ("build_id", frame["build_id"]))))

    for dump in crashing["memory_dump"]:
        if dump["register_name"] not in ("pc", "lr"):
            continue
        start = where(dump["begin_address"])
        out.append(Record("tomb.code", (
            ("region", dump["register_name"]),
            ("start", "outside every mapping" if isinstance(start, int) else start),
            ("mapping", dump["mapping_name"]), ("bytes", dump["memory"].hex()))))

    for thread in others:
        out.append(Record("tomb.thread", (("name", thread["name"]),)))
        for index, frame in enumerate(thread["current_backtrace"]):
            # Where a thread was caught while it ran is not a fact of the
            # guest: its first frame keeps the library and nothing else.
            out.append(Record("tomb.thread_frame", (
                ("index", index), ("file", frame["file_name"]),
                ("function", "" if index == 0 else frame["function_name"]))))
    return out
