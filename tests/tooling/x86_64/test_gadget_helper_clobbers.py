#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that no x86-64 gadget reads a register a helper call may have clobbered.

A `helper_call` (darwin/cpu/x86_64/gadgets/engine/common.inc) reaches a C++
function through a veneer, and AAPCS64 lets that function overwrite x0 to x17.
x0 and x1 carry its results; any other of them a gadget reads after the call
must have been written after it. This reads the assembled engine, whose
generated banks and tail groups are included into it, and over-approximates
every path a run can take after a call: it finds the veneers (a `b` to a
symbol the object does not define), and from every call to one it follows
fall-through and both arms of every branch, into the cold section and the
shared exits, resolving each branch's relocation, with x2 to x17 marked
clobbered until an instruction writes them, and reports a read of a
clobbered one. A path ends at an indirect branch (the next gadget's
dispatch), a return, a trap, another call, or once every register is written
again. Two prunings keep it to paths a run can take: code no entry reaches
(from a symbol, or an `adr` in reached code, through branches and
fall-through) is not followed, so a call there is not one, and a `tbz` or
`tbnz` of a bit the path has already tested, on a register it has not
written since, takes only the arm that test chose. A self-check feeds the
scan listings with and without such a read first.

usage: test_gadget_helper_clobbers.py --objdump OBJDUMP --object TARGET OBJECT
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path

SYMBOL = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSTRUCTION = re.compile(r"^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$")
RELOCATION = re.compile(r"^\s+([0-9a-f]+):\s+ARM64_RELOC_BRANCH26\s+(\S+)$")
ADDEND = re.compile(r"^\s+([0-9a-f]+):\s+ARM64_RELOC_ADDEND\s+0x([0-9a-f]+)$")
REGISTER = re.compile(r"(?<![\w.])[xw](\d{1,2})(?!\w)")
MEMORY = re.compile(r"\[([^\]]*)\](!?)(.*)$")
CONDITIONAL = re.compile(r"^(?:b\.|cbn?z$|tbn?z$)")
ENDS = frozenset({"br", "ret", "brk"})
CLOBBERED = frozenset(f"x{number}" for number in range(2, 18))
# Mnemonics that write no register operand: stores, compares, branches.
NO_DESTINATION = re.compile(
    r"^(?:st(?!xr|lxr|xp|lxp)|cmp|cmn|tst|ccmp|ccmn|cbn?z|tbn?z|b\.|br$|blr$|"
    r"ret$|msr$|prfm|dmb|dsb|isb|fcmp|brk$|hint|nop$)")
STATUS_STORE = re.compile(r"^st(?:l?xr|l?xp)")
PAIR_LOAD = re.compile(r"^ld(?:n?p|a?xp)")
ATOMIC = re.compile(r"^(?:swp|ld(?:add|clr|eor|set|smax|smin|umax|umin))")
# Writes its first operand and reads it too.
READ_MODIFY_WRITE = re.compile(r"^(?:movk|bfi|bfxil|bfm|cas)")
SHOWN = 20


def registers(text: str) -> list[str]:
    return [f"x{number}" for number in REGISTER.findall(text)
            if int(number) <= 30]


def accesses(mnemonic: str, operands: str) -> tuple[list[str], list[str]]:
    """The general registers an instruction reads and writes."""
    operands = operands.split("//")[0]
    memory = MEMORY.search(operands)
    head = operands[:memory.start()] if memory else operands
    reads = registers(memory[1]) if memory else []
    writes = []
    if memory and reads and (memory[2] == "!" or memory[3].strip()):
        writes.append(reads[0])
    named = []
    for operand in head.split(","):
        found = registers(operand)
        named.append(found[0] if found else None)
    present = [register for register in named if register]
    first = named[0] if named else None
    if STATUS_STORE.match(mnemonic):
        writes += [first] if first else []
        reads += [register for register in named[1:] if register]
    elif NO_DESTINATION.match(mnemonic):
        reads += present
    elif PAIR_LOAD.match(mnemonic):
        writes += [register for register in named[:2] if register]
        reads += [register for register in named[2:] if register]
    elif ATOMIC.match(mnemonic):
        reads += [first] if first else []
        writes += [named[1]] if len(named) > 1 and named[1] else []
    elif READ_MODIFY_WRITE.match(mnemonic):
        reads += present
        writes += [first] if first else []
    else:
        writes += [first] if first else []
        reads += [register for register in named[1:] if register]
    return reads, writes


def target_of(operands: str) -> int | None:
    """The address a direct branch names in the disassembly."""
    found = re.findall(r"0x([0-9a-f]+)(?=\s*<|\s*$)", operands)
    return int(found[-1], 16) if found else None


def bit_test(operands: str) -> tuple[str, int] | None:
    """The register and bit a `tbz` or `tbnz` tests."""
    found = re.match(r"\s*[xw](\d{1,2}),\s*#(0x[0-9a-f]+|\d+)", operands)
    return (f"x{found[1]}", int(found[2], 0)) if found else None


def scan(listing: str, symbols: str) -> tuple[int, int, list[str]]:
    """The helper calls of a disassembly on code an entry reaches, the calls
    on code none does, and the reads of a register a call may have
    clobbered. `symbols` is the object's symbol table."""
    defined: dict[str, int] = {}
    undefined: set[str] = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[-2] == "*UND*":
            undefined.add(fields[-1])
        elif len(fields) >= 3 and re.fullmatch(r"[0-9a-f]+", fields[0]):
            defined[fields[-1]] = int(fields[0], 16)
    rows: dict[int, tuple[str, str, str | None]] = {}
    order: list[int] = []
    branch: dict[int, str] = {}
    addend: dict[int, int] = {}
    symbol = None
    for line in listing.splitlines():
        found = SYMBOL.match(line)
        if found:
            symbol = found[2]
            continue
        found = RELOCATION.match(line)
        if found:
            branch[int(found[1], 16)] = found[2]
            continue
        found = ADDEND.match(line)
        if found:
            addend[int(found[1], 16)] = int(found[2], 16)
            continue
        found = INSTRUCTION.match(line)
        if found:
            address = int(found[1], 16)
            rows[address] = (found[2], found[3], symbol)
            order.append(address)
    following = dict(zip(order, order[1:]))

    def destination(address: int) -> int | None:
        name = branch.get(address)
        if name is not None:
            if name in defined:
                return defined[name] + addend.get(address, 0)
            return None
        return target_of(rows[address][1])

    def successors(address: int) -> list[int | None]:
        mnemonic = rows[address][0]
        if mnemonic in ENDS:
            return []
        if mnemonic == "b":
            return [destination(address)]
        if CONDITIONAL.match(mnemonic):
            return [destination(address), following.get(address)]
        return [following.get(address)]

    # The code an entry reaches: every symbol, and every address an `adr` in
    # reached code takes, through branches, calls' returns and fall-through.
    live: set[int] = set()
    pending = [address for address in defined.values() if address in rows]
    while pending:
        address = pending.pop()
        if address is None or address in live or address not in rows:
            continue
        live.add(address)
        mnemonic, operands, _ = rows[address]
        if mnemonic in {"adr", "bl"}:
            pending.append(target_of(operands) if mnemonic == "adr"
                           else destination(address))
        pending += successors(address)

    veneers = {address for address, (mnemonic, _, _) in rows.items()
               if mnemonic == "b" and branch.get(address) in undefined}

    def helper_call(address: int) -> bool:
        if rows[address][0] != "bl":
            return False
        name = branch.get(address)
        if name is not None and name in undefined:
            return True
        return destination(address) in veneers

    calls = dead = 0
    problems: list[str] = []
    reported: set[int] = set()
    State = tuple[int, frozenset[str], frozenset[tuple[str, int, int]]]
    for call in order:
        if not helper_call(call):
            continue
        if call not in live:
            dead += 1
            continue
        calls += 1
        start = following.get(call)
        paths: list[State] = []
        if start is not None:
            paths.append((start, frozenset(CLOBBERED), frozenset()))
        seen: set[State] = set()
        while paths:
            state = paths.pop()
            address, clobbered, known = state
            if address not in rows or state in seen:
                continue
            seen.add(state)
            mnemonic, operands, symbol = rows[address]
            if mnemonic in {"bl", "blr"} or mnemonic in ENDS:
                continue
            reads, writes = accesses(mnemonic, operands)
            stale = sorted({register for register in reads
                            if register in clobbered})
            if stale and address not in reported:
                reported.add(address)
                problems.append(
                    f"{symbol} {address:#x}: `{mnemonic} {operands.strip()}` "
                    f"reads {', '.join(stale)} after the helper call at "
                    f"{call:#x}")
            clobbered = clobbered - set(writes)
            if not clobbered:
                continue
            known = frozenset(fact for fact in known if fact[0] not in writes)
            tested = bit_test(operands) if mnemonic in {"tbz", "tbnz"} else None
            if tested is None:
                paths += [(step, clobbered, known)
                          for step in successors(address) if step is not None]
                continue
            # A tbz branches on a clear bit and a tbnz on a set one.
            taken_value = 0 if mnemonic == "tbz" else 1
            value = next((fact[2] for fact in known
                          if fact[:2] == tested), None)
            arms = [(destination(address), taken_value),
                    (following.get(address), 1 - taken_value)]
            for step, outcome in arms:
                if step is None or (value is not None and value != outcome):
                    continue
                paths.append((step, clobbered,
                              known | {(tested[0], tested[1], outcome)}))
    return calls, dead, problems


def self_check() -> list[str]:
    """The scan reports a read after a call on any path a run can take, and
    nothing on code or arms no run reaches."""
    def listing(body: list[str], entries: tuple[str, ...] = ()) -> tuple[str, str]:
        first = int(body[0].split(":")[0], 16)
        lines = [f"{first:016x} <_gadget>:"]
        for row in body:
            address = int(row.split(":")[0], 16)
            if f"{address:x}" in entries:
                lines.append(f"{address:016x} <_entry_{address:x}>:")
            mnemonic, _, operands = row.split(":", 1)[1].strip().partition(" ")
            lines.append(f"{address:8x}:      \t{mnemonic}\t{operands}")
        lines += ["0000000000000040 <_island>:",
                  "      40:      \tb\t0x40 <_island>",
                  "\t\t0000000000000040:  ARM64_RELOC_BRANCH26\t_helper",
                  "      44:      \tret"]
        table = [f"{first:016x} g     F __TEXT,__text _gadget",
                 "0000000000000040 g     F __TEXT,__text _island",
                 "0000000000000000         *UND* _helper"]
        table += [f"{int(entry, 16):016x} g     F __TEXT,__text _entry_{entry}"
                  for entry in entries]
        return "\n".join(lines), "\n".join(table)
    call = "4: bl 0x40 <_island>"
    store = "str x8, [x20, #0x10]"
    cases = [
        ("a read on the fall-through path", (1, 0, True),
         ["0: mov x8, #0x1", call, f"8: {store}", "c: ret"], ()),
        ("a register an instruction reads and writes", (1, 0, True),
         [call, "8: ldrb w9, [x21, #0x31]", "c: bfi x9, x17, #0, #8",
          "10: ret"], ()),
        ("a read behind a branch", (1, 0, True),
         [call, "8: b 0x10 <_gadget+0x10>", "c: mov x8, x0", f"10: {store}",
          "14: ret"], ()),
        ("a read past a conditional branch", (1, 0, True),
         [call, "8: cbz x0, 0x14 <_gadget+0x14>", "c: nop", f"10: {store}",
          "14: ret"], ()),
        ("a read at a conditional branch's target", (1, 0, True),
         [call, "8: cbz x0, 0x14 <_gadget+0x14>", "c: mov x8, x0",
          "10: ret", f"14: {store}", "18: ret"], ()),
        ("registers written after the call, and the results", (1, 0, False),
         [call, "8: mov x8, x0", "c: add x9, x1, #0x1", f"10: {store}",
          "14: ret"], ()),
        ("a read no path from the call reaches", (1, 0, False),
         [call, "8: b 0x14 <_gadget+0x14>", "c: nop", f"10: {store}",
          "14: ret"], ()),
        ("a call on code no entry reaches", (0, 1, False),
         ["0: b 0x10 <_gadget+0x10>", call, f"8: {store}", "c: ret",
          "10: ret"], ()),
        ("the same call once an entry reaches it", (1, 0, True),
         ["0: b 0x10 <_gadget+0x10>", call, f"8: {store}", "c: ret",
          "10: ret"], ("4",)),
        ("one bit tested twice on an unchanged register", (1, 0, False),
         [call, "8: ldrb w13, [x20, #0x39b]",
          "c: tbz w13, #0x2, 0x14 <_gadget+0x14>",
          "10: ldr w12, [x20, #0x390]", "14: nop",
          "18: tbz w13, #0x2, 0x20 <_gadget+0x20>",
          "1c: str x12, [x20, #0x10]", "20: ret"], ()),
        ("one bit tested twice with the register written between",
         (1, 0, True),
         [call, "8: ldrb w13, [x20, #0x39b]",
          "c: tbz w13, #0x2, 0x14 <_gadget+0x14>",
          "10: ldr w12, [x20, #0x390]", "14: ldrb w13, [x20, #0x39c]",
          "18: tbz w13, #0x2, 0x20 <_gadget+0x20>",
          "1c: str x12, [x20, #0x10]", "20: ret"], ()),
    ]
    problems = []
    for name, (calls, dead, stale), body, entries in cases:
        text, table = listing(body, entries)
        found_calls, found_dead, found = scan(text, table)
        if (found_calls, found_dead, bool(found)) != (calls, dead, stale):
            problems.append(
                f"self-check: {name}: {found_calls} calls, {found_dead} on "
                f"code no entry reaches, {len(found)} reads reported")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--object", nargs=2, metavar=("TARGET", "OBJECT"),
                        required=True)
    arguments = parser.parse_args()
    problems = self_check()
    target, name = arguments.object
    path = Path(name)
    if not path.is_file():
        problems.append(f"{target}: {path} does not exist")
    elif not problems:
        listing = subprocess.run(
            [arguments.objdump, "-d", "-r", "--no-show-raw-insn", str(path)],
            check=True, capture_output=True, text=True).stdout
        symbols = subprocess.run(
            [arguments.objdump, "-t", str(path)],
            check=True, capture_output=True, text=True).stdout
        calls, dead, found = scan(listing, symbols)
        problems += [f"{target}: {path.name}: {entry}"
                     for entry in found[:SHOWN]]
        if len(found) > SHOWN:
            problems.append(f"{target}: ... and {len(found) - SHOWN} more")
        if calls == 0:
            problems.append(f"{target}: {path.name}: no helper call found; "
                            "this check has nothing to guard")
        if not problems:
            print(f"{target}: {path.name}: {calls} helper calls followed on "
                  f"every path, {dead} on code no entry reaches; no register "
                  "read before it is written again")
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
