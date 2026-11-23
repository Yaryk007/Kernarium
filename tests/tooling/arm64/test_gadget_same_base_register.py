#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that only the same-base family names x25 in the arm64 gadget engine.

x25 carries a translation from one transfer of a block execution to the next
(darwin/cpu/arm64/gadgets/engine/same_base.inc): a follower adds its
displacement to it. A gadget that wrote x25 between the two would send the
follower to a wrong host address, so no other source of the engine names it:
engine.S, every file under engine/, which holds the included assembly and the
generators of its generated parts, and the shared host frame, which may only
save and restore it. With --object, the assembled engine is read too, which
also sees a register a macro or a generator builds from a number: only the
same-base entries (and the code after each up to the next symbol) and the
frame's one save and one restore may name x25.

usage: test_gadget_same_base_register.py SOURCE_ROOT
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

ENGINE = Path("darwin/cpu/arm64/gadgets")
OWNER = ENGINE / "engine" / "same_base.inc"
FRAME = Path("darwin/cpu/gadget_host_frame.inc")
NAME = re.compile(r"(?<![\w\\])[xw]25(?!\w)")
# The frame's save and restore, and its layout comment.
FRAME_LINE = re.compile(
    r"^\s*(?:(?:stp|ldp)\s+x25,\s*x26,\s*\[sp,\s*#0x40\]|\*.*)$")
# In the disassembly: a symbol, an instruction, and the frame's two.
SYMBOL = re.compile(r"^[0-9a-f]+ <(?P<name>[^>]+)>:$")
INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s*(?P<text>.*)$")
OWNED = re.compile(r"^_?uml_arm64_gadget_sb_")
SAVE = "stp x25, x26, [sp, #0x40]"
RESTORE = "ldp x25, x26, [sp, #0x40]"
SHOWN = 20


def object_names(objdump: str, target: str, path: Path) -> list[str]:
    """Problems with the instructions of the engine object that name x25."""
    if not path.is_file():
        return [f"{target}: {path} does not exist"]
    disassembly = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", str(path)],
        check=True, capture_output=True, text=True).stdout
    symbol = None
    owned = saves = restores = 0
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
        if text == SAVE:
            saves += 1
        elif text == RESTORE:
            restores += 1
        elif symbol is not None and OWNED.match(symbol):
            owned += 1
        else:
            stray.append(f"{symbol}: {text}")
    problems = [f"{target}: {path.name}: {entry}" for entry in stray[:SHOWN]]
    if len(stray) > SHOWN:
        problems.append(f"{target}: ... and {len(stray) - SHOWN} more")
    if owned == 0:
        problems.append(f"{target}: {path.name}: no same-base entry names "
                        "x25; this check has nothing to guard")
    if saves != 1 or restores != 1:
        problems.append(f"{target}: {path.name}: expected one save and one "
                        f"restore of x25, found {saves} and {restores}")
    if not problems:
        print(f"{target}: {path.name}: x25 in {owned} instructions of the "
              "same-base entries and the frame's save and restore")
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
    names = 0
    checked = sources(root)
    for relative in checked:
        text = (root / relative).read_text(encoding="utf-8")
        hits = [(number, line) for number, line in
                enumerate(text.splitlines(), 1) if NAME.search(line)]
        if relative == OWNER:
            names = len(hits)
            continue
        for number, line in hits:
            print(f"{relative}:{number}: names x25 outside {OWNER.name}: "
                  f"{line.strip()}", file=sys.stderr)
            failed = True
    if names == 0:
        print(f"{OWNER}: names no x25; this check has nothing to guard",
              file=sys.stderr)
        failed = True
    saved = 0
    for number, line in enumerate(
            (root / FRAME).read_text(encoding="utf-8").splitlines(), 1):
        if not NAME.search(line):
            continue
        if not FRAME_LINE.match(line):
            print(f"{FRAME}:{number}: does more with x25 than save or "
                  f"restore it: {line.strip()}", file=sys.stderr)
            failed = True
        elif re.match(r"^\s*(?:stp|ldp)\b", line):
            saved += 1
    if saved != 2:
        print(f"{FRAME}: expected one save and one restore of x25, found "
              f"{saved}", file=sys.stderr)
        failed = True
    if arguments.object is not None:
        target, name = arguments.object
        for problem in object_names(arguments.objdump, target, Path(name)):
            print(problem, file=sys.stderr)
            failed = True
    if failed:
        return 1
    print(f"{len(checked)} engine sources: x25 only in {OWNER.name} "
          f"({names} lines) and the host frame's save and restore")
    return 0


if __name__ == "__main__":
    sys.exit(main())
