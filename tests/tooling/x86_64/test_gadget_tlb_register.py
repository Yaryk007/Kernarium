#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that x22 holds the x86-64 gadget run's software-TLB base and nothing else.

The run's entry (`uml_x86_64_gadget_enter`, darwin/cpu/x86_64/gadgets/engine/
common.inc) loads the table's address from the context into x22, which
common.inc names `_tlb`, and every translation indexes the table from it. A
gadget that wrote x22 would send every later translation of the run to a wrong
table, so no source of the engine names it but that one alias: engine.S, every
file under engine/, which holds the included assembly and the generators of
its generated parts, and the shared host frame, which may only save and
restore it. With --object, the assembled engine is read too, which also sees a
register a macro or a generator builds from a number: the frame's one save and
one restore, the entry's one load, which must follow the save in the entry so
that the caller gets its own x22 back, and otherwise only the translations'
`add x11, x22, x10, lsl #5` may name x22. A self-check feeds the object check
listings that write x22 or index it from another register, and fails if it
accepts any of them.

usage: test_gadget_tlb_register.py SOURCE_ROOT
           [--objdump OBJDUMP --object TARGET OBJECT]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path

ENGINE = Path("darwin/cpu/x86_64/gadgets")
OWNER = ENGINE / "engine" / "common.inc"
OFFSETS = Path("darwin/cpu/x86_64/layout/assembly_offsets.h")
FRAME = Path("darwin/cpu/gadget_host_frame.inc")
NAME = re.compile(r"(?<![\w\\])[xw]22(?!\w)")
ALIAS = re.compile(r"^_tlb\s+\.req\s+x22\s*$")
# The frame's save and restore, and its layout comment.
FRAME_LINE = re.compile(
    r"^\s*(?:(?:stp|ldp)\s+x21,\s*x22,\s*\[sp,\s*#0x20\]|\*.*)$")
# In the disassembly: a symbol and an instruction.
SYMBOL = re.compile(r"^[0-9a-f]+ <(?P<name>[^>]+)>:$")
INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s*(?P<text>.*)$")
ENTRY = re.compile(r"^_?uml_x86_64_gadget_enter$")
INDEX = "add x11, x22, x10, lsl #5"
SAVE = "stp x21, x22, [sp, #0x20]"
RESTORE = "ldp x21, x22, [sp, #0x20]"
SHOWN = 20


def context_offset(root: Path) -> int:
    """UML_X86_64_CONTEXT_TLB, the context field the entry loads."""
    text = (root / OFFSETS).read_text(encoding="utf-8")
    found = re.search(r"^#define UML_X86_64_CONTEXT_TLB\s+(\d+)\s*$", text,
                      re.MULTILINE)
    if not found:
        raise SystemExit(f"{OFFSETS}: no UML_X86_64_CONTEXT_TLB")
    return int(found[1])


def disassembly_problems(disassembly: str, label: str,
                         offset: int) -> tuple[list[str], int]:
    """Problems with the instructions of a disassembly that name x22, and
    how many translations index the table from it."""
    load = f"ldr x22, [x21, #{offset:#x}]"
    symbol = None
    indexes = saves = restores = loads = 0
    saved_in_entry = False
    stray: list[str] = []
    for line in disassembly.splitlines():
        found = SYMBOL.match(line)
        if found:
            symbol = found["name"]
            continue
        found = INSTRUCTION.match(line)
        if not found or not NAME.search(found["text"]):
            continue
        text = " ".join(found["text"].split())
        in_entry = symbol is not None and ENTRY.match(symbol)
        if text == SAVE:
            saves += 1
            saved_in_entry = saved_in_entry or bool(in_entry)
        elif text == RESTORE:
            restores += 1
        elif text == load and in_entry:
            loads += 1
            if not saved_in_entry:
                stray.append(f"{symbol}: {text} comes before the entry saves "
                             "the caller's x22")
        elif text == INDEX:
            indexes += 1
        else:
            stray.append(f"{symbol}: {text}")
    problems = [f"{label}: {entry}" for entry in stray[:SHOWN]]
    if len(stray) > SHOWN:
        problems.append(f"{label}: ... and {len(stray) - SHOWN} more")
    if indexes == 0:
        problems.append(f"{label}: no translation indexes the table from "
                        "x22; this check has nothing to guard")
    if loads != 1:
        problems.append(f"{label}: expected the entry's one load `{load}`, "
                        f"found {loads}")
    if saves != 1 or restores != 1:
        problems.append(f"{label}: expected one save and one restore of x22, "
                        f"found {saves} and {restores}")
    return problems, indexes


def self_check(offset: int) -> list[str]:
    """The object check rejects what it exists to reject."""
    clean = [
        "0000000000000000 <_uml_x86_64_gadget_enter>:",
        f"       0: {SAVE}",
        f"       4: ldr x22, [x21, #{offset:#x}]",
        "0000000000000010 <uml_x86_64_gadget_leave>:",
        f"      10: {RESTORE}",
        "0000000000000020 <_uml_x86_64_gadget_load>:",
        f"      20: {INDEX}",
    ]
    problems = []
    if disassembly_problems("\n".join(clean), "self-check", offset)[0]:
        problems.append("self-check: the object check rejects a clean listing")
    for bad in ("add x22, x22, x10, lsl #5", "add x11, x22, x9, lsl #5",
                "mov x22, x0", f"ldr x22, [x21, #{offset:#x}]"):
        listing = "\n".join(clean + [f"      24: {bad}"])
        if not disassembly_problems(listing, "self-check", offset)[0]:
            problems.append(f"self-check: the object check accepts `{bad}`")
    early = clean[:1] + [clean[2], clean[1]] + clean[3:]
    if not disassembly_problems("\n".join(early), "self-check", offset)[0]:
        problems.append("self-check: the object check accepts the entry's "
                        "load before the frame's save")
    return problems


def object_names(objdump: str, target: str, path: Path,
                 offset: int) -> list[str]:
    """Problems with the instructions of the engine object that name x22."""
    if not path.is_file():
        return [f"{target}: {path} does not exist"]
    disassembly = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", str(path)],
        check=True, capture_output=True, text=True).stdout
    problems, indexes = disassembly_problems(
        disassembly, f"{target}: {path.name}", offset)
    if not problems:
        print(f"{target}: {path.name}: x22 in the entry's load, {indexes} "
              "translations and the frame's save and restore")
    return problems


def sources(root: Path) -> list[Path]:
    """engine.S and everything under engine/, as repository paths."""
    engine = root / ENGINE
    found = [ENGINE / "engine.S"]
    found += sorted(path.relative_to(root)
                    for path in (engine / "engine").rglob("*")
                    if path.is_file() and path.suffix in {".inc", ".S", ".py"})
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("root", type=Path)
    parser.add_argument("--objdump")
    parser.add_argument("--object", nargs=2, metavar=("TARGET", "OBJECT"))
    arguments = parser.parse_args()
    if (arguments.objdump is None) != (arguments.object is None):
        parser.error("--objdump and --object go together")
    root = arguments.root.resolve()
    failed = False
    aliases = 0
    checked = sources(root)
    for relative in checked:
        text = (root / relative).read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), 1):
            if not NAME.search(line):
                continue
            if relative == OWNER and ALIAS.match(line):
                aliases += 1
                continue
            print(f"{relative}:{number}: names x22, which holds the run's "
                  f"software-TLB base as `_tlb`: {line.strip()}",
                  file=sys.stderr)
            failed = True
    if aliases != 1:
        print(f"{OWNER}: expected one `_tlb .req x22`, found {aliases}",
              file=sys.stderr)
        failed = True
    saved = 0
    for number, line in enumerate(
            (root / FRAME).read_text(encoding="utf-8").splitlines(), 1):
        if not NAME.search(line):
            continue
        if not FRAME_LINE.match(line):
            print(f"{FRAME}:{number}: does more with x22 than save or "
                  f"restore it: {line.strip()}", file=sys.stderr)
            failed = True
        elif re.match(r"^\s*(?:stp|ldp)\b", line):
            saved += 1
    if saved != 2:
        print(f"{FRAME}: expected one save and one restore of x22, found "
              f"{saved}", file=sys.stderr)
        failed = True
    for problem in self_check(context_offset(root)):
        print(problem, file=sys.stderr)
        failed = True
    if arguments.object is not None:
        target, name = arguments.object
        for problem in object_names(arguments.objdump, target, Path(name),
                                    context_offset(root)):
            print(problem, file=sys.stderr)
            failed = True
    if failed:
        return 1
    print(f"{len(checked)} engine sources: x22 only as {OWNER.name}'s `_tlb` "
          "and in the host frame's save and restore")
    return 0


if __name__ == "__main__":
    sys.exit(main())
