# SPDX-License-Identifier: GPL-2.0-or-later
"""Scripted protobuf tombstones for the AOT rows.

A writer of the wire format (varint, length-delimited) and a tombstone of a
crash in libc built with it, for the rows of test_aot_debug_rows.py and
test_aot_tombstone_pb.py. The writer takes the field numbers literally and
does not read the decoder's table (tombstone_pb.TABLES), so a row that decodes
what it wrote holds the table to these numbers. Like the crash reporter it
leaves out a scalar that holds its default (a zero, an empty string, a false).
"""

__import__("sys").dont_write_bytecode = True

# ---- the wire format ---------------------------------------------------------


def varint(value: int) -> bytes:
    if value < 0:
        value += 1 << 64
    out = bytearray()
    while True:
        low = value & 0x7f
        value >>= 7
        if value:
            out.append(low | 0x80)
        else:
            out.append(low)
            return bytes(out)


def tag(number: int, wire: int) -> bytes:
    return varint((number << 3) | wire)


def number_field(number: int, value: int) -> bytes:
    """A varint field; a zero is left out, as proto3 does."""
    return tag(number, 0) + varint(value) if value else b""


def always(number: int, value: int) -> bytes:
    """A varint field that is written even when it holds a zero."""
    return tag(number, 0) + varint(value)


def bytes_field(number: int, payload: bytes) -> bytes:
    return tag(number, 2) + varint(len(payload)) + payload


def text_field(number: int, text: str) -> bytes:
    """A string field of the text's UTF-8 (a lone surrogate is the byte it
    stands for, as the decoder reads it back)."""
    return bytes_field(number, text.encode("utf-8", "surrogateescape")) if text else b""


# ---- the messages --------------------------------------------------------------

LIBC = "/apex/com.android.runtime/lib64/bionic/libc.so"
LIBZ = "/system/lib64/libz.so"
TOYBOX = "/system/bin/toybox"
AOTDEBUG = "/data/local/tmp/libaotdebug.so"
REGISTER_NAMES = tuple(f"x{n}" for n in range(30)) + ("lr", "sp", "pc", "pst", "esr", "vg")


def mapping(low: int, high: int, name: str = "", protection: str = "rw-", offset: int = 0) -> bytes:
    """A MemoryMapping: begin 1, end 2, offset 3, read 4, write 5, execute 6,
    mapping_name 7."""
    return (number_field(1, low) + number_field(2, high) + number_field(3, offset)
            + number_field(4, protection[0] == "r") + number_field(5, protection[1] == "w")
            + number_field(6, protection[2] == "x") + text_field(7, name))


def register(name: str, value: int) -> bytes:
    """A Register: name 1, u64 2."""
    return text_field(1, name) + number_field(2, value)


def frame(rel_pc: int, file: str, function: str = "", offset: int = 0, file_offset: int = 0,
          build_id: str = "") -> bytes:
    """A BacktraceFrame: rel_pc 1, pc 2, sp 3, function_name 4,
    function_offset 5, file_name 6, file_map_offset 7, build_id 8."""
    return (number_field(1, rel_pc) + number_field(2, 0x7ff100000000 + rel_pc) + number_field(3, 0x7fffffffe000)
            + text_field(4, function) + number_field(5, offset) + text_field(6, file)
            + number_field(7, file_offset) + text_field(8, build_id))


def memory_dump(register_name: str, mapping_name: str, begin: int, memory: bytes) -> bytes:
    """A MemoryDump: register_name 1, mapping_name 2, begin_address 3,
    memory 4."""
    return (text_field(1, register_name) + text_field(2, mapping_name) + number_field(3, begin)
            + (bytes_field(4, memory) if memory else b""))


def thread(tid: int, name: str, registers: bytes = b"", frames: bytes = b"", dumps: bytes = b"") -> bytes:
    """A Thread: id 1, name 2, registers 3, current_backtrace 4, memory_dump 5,
    tagged_addr_ctrl 6, pac_enabled_keys 8."""
    return (number_field(1, tid) + text_field(2, name) + registers + frames + dumps
            + number_field(6, 1) + always(8, -1))


def signal(number: int = 11, name: str = "SIGSEGV", code: int = 2, code_name: str = "SEGV_ACCERR",
           fault: int | None = 0x7ff1b1f19000) -> bytes:
    """A Signal: number 1, name 2, code 3, code_name 4, has_sender 5,
    sender_uid 6, sender_pid 7, has_fault_address 8, fault_address 9."""
    return (number_field(1, number) + text_field(2, name) + number_field(3, code) + text_field(4, code_name)
            + (number_field(8, 1) + number_field(9, fault) if fault is not None else b""))


def thread_entry(tid: int, body: bytes) -> bytes:
    """An entry of the map<uint32, Thread> field: key 1, value 2."""
    return number_field(1, tid) + bytes_field(2, body)


def sibling_thread(tid: int = 2112, function: str = "crc32_z", offset: int = 360) -> tuple[int, bytes]:
    """A thread that was running libz when the process crashed: (tid, Thread)."""
    body = thread(tid, "toybox", registers=b"".join(bytes_field(3, register(name, 0)) for name in ("x0", "x1")),
                  frames=bytes_field(4, frame(0x123d8, LIBZ, function, offset, build_id="014ea0f1"))
                  + bytes_field(4, frame(0x5fec, AOTDEBUG)))
    return tid, body


def tombstone(address: int = 0x7ff1b1f19000, function: str = "__memmove_aarch64_mops+16", x13: int = 1,
              pid: int = 2038, extra_threads: list[tuple[int, bytes]] | None = None, load: int = 0x7ff1ae500000,
              pointer: int = 0x2854d, stack_name: str = "[anon:stack_and_tls:2040]",
              unnamed: int = 0x1234, unnamed_size: int = 0x100000, second_unnamed: bool = False,
              unnamed_target: int = 0, lower_unnamed: bool = False, lower_size: int = 0,
              x5_unnamed: bool = False, extra_mappings: list[bytes] | None = None,
              registers: dict[str, int] | None = None, drop_registers: tuple[str, ...] = (),
              signal_message: bytes | None = None, abort_message: str = "", extra: bytes = b"",
              last: bool = True, mapping_order: list[int] | None = None, guard_page: bool = False,
              extra_registers: list[tuple[str, int]] | None = None, no_signal: bool = False,
              crashing_id: int | None = None, thread_key: int | None = None, duplicate_thread: bool = False,
              arch: int = 1) -> bytes:
    """A tombstone of a crash in libc (loaded at `load`): x4 points `pointer`
    bytes into libc's file, x5 into its .bss, x6 into an unnamed mapping, and
    x7 into a thread stack. With `second_unnamed` a second unnamed mapping of
    the same size and protection lies above the stack, and with
    `lower_unnamed` a third lies below the first (of `lower_size` bytes if
    that is given, which makes its shape another); `unnamed_target` says
    which x6 points into: 0 the first, 1 the one above the stack, 2 the one
    below the first. With `x5_unnamed` x5 points into a fourth unnamed mapping
    of 4 KiB instead of the .bss. `registers` overrides registers by name,
    `extra_mappings` adds mappings, `mapping_order` lists the mappings in
    another order, `guard_page` adds an unnamed 4 KiB PROT_NONE mapping above
    the stack that the fault address lies in. `last=False` leaves out the
    field the reporter writes last. `extra_registers` lists more registers
    after the others, `no_signal` leaves the signal out, `crashing_id` and
    `thread_key` say the crashing thread's id and its key in the thread map
    (the pid by default), `duplicate_thread` writes its map entry twice, and
    `arch` is the architecture."""
    base = load + 0x70d50
    unnamed_base = load + {0: 0x200000, 1: 0x500000, 2: 0xc0000}[unnamed_target]
    below = lower_size or unnamed_size
    guard = load + 0x800000
    if guard_page:
        address = guard + 0x10
    mappings = [mapping(0x400000, 0x401000, TOYBOX, "r-x"),
                mapping(load, load + 0xb0000, LIBC, "r-x"),
                mapping(load + 0xb0000, load + 0xb3000, "[anon:.bss]")]
    if lower_unnamed:
        mappings.append(mapping(load + 0xc0000, load + 0xc0000 + below))
    if x5_unnamed:
        mappings.append(mapping(load + 0x1e0000, load + 0x1e1000))
    mappings.append(mapping(load + 0x200000, load + 0x200000 + unnamed_size))
    mappings.append(mapping(load + 0x300000, load + 0x400000, stack_name))
    if second_unnamed:
        mappings.append(mapping(load + 0x500000, load + 0x500000 + unnamed_size))
    if guard_page:
        mappings.append(mapping(guard, guard + 0x1000, protection="---"))
    mappings += extra_mappings or []
    if mapping_order is not None:
        mappings = [mappings[index] for index in mapping_order]
    x5_value = load + 0x1e0010 if x5_unnamed else load + 0xb0000 + 0x2070
    values = {name: 0 for name in REGISTER_NAMES}
    values.update({"x0": 0xb4007ff19a825220, "x1": address, "x2": 0x28, "x3": 0xb4007ff19a825238,
                   "x4": load + pointer, "x5": x5_value, "x6": unnamed_base + unnamed,
                   "x7": load + 0x300000 + 0xf8c00, "x12": 1, "x13": x13, "x14": 1, "x15": 0x1863,
                   "lr": load + 0x25d0, "sp": 0x7ffff7299dd0, "pc": base, "pst": 0x20000000})
    values.update(registers or {})
    crashing = thread(
        pid if crashing_id is None else crashing_id, "toybox",
        registers=b"".join(bytes_field(3, register(name, values[name])) for name in REGISTER_NAMES
                           if name not in drop_registers)
        + b"".join(bytes_field(3, register(name, value)) for name, value in extra_registers or []),
        frames=bytes_field(4, frame(0x70d50, LIBC, function.split("+")[0], int(function.split("+")[1]),
                                    build_id="f1804e2e"))
        + bytes_field(4, frame(0x195cc, LIBZ, "updatewindow(z_stream_s*, unsigned char const*, unsigned int)", 76,
                               build_id="014ea0f1"))
        + bytes_field(4, frame(0x197b4, LIBZ, "inflateSetDictionary", 148, build_id="014ea0f1"))
        + bytes_field(4, frame(0x500c, AOTDEBUG, file_offset=0x1000)),
        dumps=bytes_field(5, memory_dump("x3", "[stack]", 0x7fffd9634e10, bytes(range(16))))
        + bytes_field(5, memory_dump("pc", LIBC, base - 0x20,
                                     bytes.fromhex("e0031faa" * 4 + "e0031faa" * 4)))
        + bytes_field(5, memory_dump("lr", LIBC, load + 0x25d0 - 0x20, bytes(32))))
    key = pid if thread_key is None else thread_key
    threads = [(key, crashing)] * (2 if duplicate_thread else 1) + (extra_threads or [])
    out = (number_field(1, arch) + text_field(2, "Andarium/lineage_andarium/andarium:17/ID/1:user/release-keys")
           + text_field(3, "0") + number_field(5, pid) + number_field(6, pid) + number_field(7, 2000)
           + text_field(8, "u:r:shell:s0\x00") + text_field(9, TOYBOX) + text_field(9, "true")
           + (b"" if no_signal else
              bytes_field(10, signal_message if signal_message is not None else signal(fault=address)))
           + text_field(14, abort_message)
           + b"".join(bytes_field(16, thread_entry(tid, body)) for tid, body in threads)
           + b"".join(bytes_field(17, item) for item in mappings)
           + number_field(22, 4096) + always(24, 5) + text_field(27, TOYBOX)
           + text_field(28, "6.18.60-test") + number_field(29, 2036) + extra
           + (text_field(1000, "24.0-20261114-UNOFFICIAL-test") if last else b""))
    return out
