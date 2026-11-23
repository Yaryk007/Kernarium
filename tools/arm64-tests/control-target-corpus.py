#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the indirect-branch target cases of the A64 instruction corpus.

usage: control-target-corpus.py DUMP OUTPUT

An indirect branch (BR, BLR, RET, and the fused epilogue LDP X29, X30, [SP],
#16 ; RET) takes its target from a register in any state: nothing stops a
guest from branching to the all-ones address, to an address one word below it,
or to one no memory maps. The branch retires; the next fetch faults. With a
target whose low two bits are not clear the fault is a PC alignment fault, and
otherwise an instruction fetch fault at the target (user space has no
translation for these addresses, and top-byte ignore is off). Either way the
register the branch names keeps its value, a link register is written before
the fault (BLR), and the post-indexed pair of the epilogue is retired.

The expectation is the architecture's: the PC is the target, the fault is at
the target with execute access, and only the registers the instruction writes
change. The cases run through a cold site, so they hold the exit every engine
must give; the cached sites of the gadget engine are the unit tests' work
(arm64-pc-alignment-contract and arm64-gadget-indirect-links).

Run it on the dump of the accepted pack and --repack the result. It replaces
its own record, so a second run changes nothing.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import functools
import hashlib
import importlib.util
import json
import pathlib
import re
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, filename: str):
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, HERE / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", "build-unit-corpus.py")

SITE = "tools/arm64-tests/control-target-corpus.py"
PRODUCER = "arm64-control-target"
MASK64 = (1 << 64) - 1
PAGE = 4096
# The read-only page the epilogue's pair is loaded from.
FRAME = 0x5200_0000
READ_ONLY_FLAGS = 0x01
# The frame pointer value the epilogue restores, distinct from every fill.
FRAME_POINTER = 0x0123_4567_89AB_CDEF

BR_X15 = 0xD61F_01E0
BLR_X15 = 0xD63F_01E0
RET_X15 = 0xD65F_01E0
RET = 0xD65F_03C0
LDP_X29_X30_POST_16 = 0xA8C1_7BFD

# Targets whose low two bits are not clear: a PC alignment fault.
MISALIGNED = (MASK64, MASK64 - 2, 0x1002)
# Aligned targets nothing maps: a fetch fault at the target.
UNMAPPED = (MASK64 - 3, 0xDEAD_BEEF_0000_1000, 0x0000_0000_DEAD_0000, 0)


@dataclasses.dataclass(frozen=True)
class Form:
    mnemonic: str
    words: tuple[int, ...]
    register: int | None  # the register that holds the target, or None
    links: bool = False
    pops: bool = False

    @property
    def retired(self) -> int:
        return len(self.words)


FORMS = (
    Form("br x15", (BR_X15,), 15),
    Form("blr x15", (BLR_X15,), 15, links=True),
    Form("ret x15", (RET_X15,), 15),
    Form("ret", (RET,), 30),
    Form("ldp x29, x30, [sp], #16 ; ret", (LDP_X29_X30_POST_16, RET), None,
         pops=True),
)


def frame_page(target: int) -> bytes:
    """The page the epilogue loads: the saved frame pointer and the target."""
    data = bytearray(PAGE)
    data[0:8] = FRAME_POINTER.to_bytes(8, "little")
    data[8:16] = (target & MASK64).to_bytes(8, "little")
    return bytes(data)


def digest_of(target: int) -> str:
    return hashlib.sha256(frame_page(target)).hexdigest()


def case_item(form: Form, target: int, program: int) -> dict:
    registers = list(corpus.FILL_REGISTERS)
    initial: dict[str, str] = {}
    expected: dict[str, str] = {}
    memory = []
    if form.register is not None:
        registers[form.register] = target
    else:
        initial["sp"] = corpus.hex64(FRAME)
        memory.append({
            "address": corpus.hex64(FRAME), "size": PAGE,
            "flags": READ_ONLY_FLAGS, "blob": digest_of(target),
            "expected": [], "tags": [], "expected_tags": []})
        expected["x29"] = corpus.hex64(FRAME_POINTER)
        expected["x30"] = corpus.hex64(target)
        expected["sp"] = corpus.hex64(FRAME + 16)
    for number in range(31):
        if registers[number] != corpus.FILL_REGISTERS[number]:
            initial[f"x{number}"] = corpus.hex64(registers[number])
    if form.links:
        expected["x30"] = corpus.hex64(corpus.CODE_BASE + 4)
    expected["pc"] = corpus.hex64(target)
    item = {
        "name": f"{form.mnemonic} to {target:#x}", "program": program,
        "event": "pc-alignment" if target & 3 else "memory-fault",
        "ordinals": [0, 0], "invalid_encoding": False,
        "minimization": "full", "native": [],
        "initial": initial, "expected": expected,
        "retired": form.retired,
        "fault": {"address": corpus.hex64(target), "access": "execute"},
    }
    if memory:
        item["memory"] = memory
    return item


@functools.cache
def build() -> dict:
    """The record."""
    programs: list[tuple[int, ...]] = []
    items = []
    for form in FORMS:
        if form.words not in programs:
            programs.append(form.words)
        for target in MISALIGNED + UNMAPPED:
            items.append(case_item(form, target, programs.index(form.words)))
    names = [item["name"] for item in items]
    if len(set(names)) != len(names):
        raise SystemExit("two cases share a name")
    return {
        "record": f"{PRODUCER}/4k", "producer": PRODUCER, "site": SITE,
        "profiles": ["4k"], "options": [],
        "programs": [{"entry": 0,
                      "words": [[4 * offset, f"{word:08x}"]
                                for offset, word in enumerate(words)]}
                     for words in programs],
        "cases": items,
    }


def counts() -> dict[str, int]:
    cases = build()["cases"]
    return {
        "cases": len(cases),
        "alignment_cases": sum(1 for case in cases
                               if case["event"] == "pc-alignment"),
        "fetch_fault_cases": sum(1 for case in cases
                                 if case["event"] == "memory-fault"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    kept = []
    for line in lines[1:]:
        if not line.strip():
            continue
        if line.startswith('{"cases"') and \
                json.loads(line)["producer"] == PRODUCER:
            continue
        kept.append(line)
    known = {match.group(1) for line in kept
             if (match := re.match(r'\{"blob": "([0-9a-f]{64})"', line))}
    fresh = []
    for target in MISALIGNED + UNMAPPED:
        digest = digest_of(target)
        if digest not in known:
            known.add(digest)
            fresh.append(json.dumps({
                "blob": digest, "zlib": zlib.compress(frame_page(target), 9)
                .hex()}, sort_keys=True))
    summary = counts()
    header["meta"]["control_target"] = {
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes())
        .hexdigest(), **summary,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in kept:
            sink.write(line + "\n")
        for line in fresh:
            sink.write(line + "\n")
        sink.write(json.dumps(build(), sort_keys=True) + "\n")
    print(f"appended {summary['cases']} indirect-branch target cases "
          f"({summary['alignment_cases']} PC alignment, "
          f"{summary['fetch_fault_cases']} fetch faults)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
