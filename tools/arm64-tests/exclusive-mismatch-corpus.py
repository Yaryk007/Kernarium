#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the compare-retry exclusive cases of the A64 instruction corpus.

usage: exclusive-mismatch-corpus.py DUMP OUTPUT

A load-exclusive, a compare and a branch that leaves the loop on a mismatch,
a store-exclusive that retries on failure, and an alternate store-exclusive
at the branch's target are the shape of a compare-and-swap whose mismatch
path reaches a store of its own without loading again:

    ldxr x3, [x1]; cmp x3, x4; b.ne alt; stxr w5, x2, [x1]; cbnz w5, top;
    clrex; svc; alt: stxr w6, x7, [x1]; cbnz w6, top; clrex; svc

The branch does not clear the exclusive monitor, so the store at `alt`
succeeds on the first attempt, and a program whose store fails without a
cause the architecture allows retries and still completes. An engine that
clears the monitor on the mismatch and continues makes that store fail on
every pass, and the loop never ends (the runner's instruction budget then
ends it, which fails the case). The CLREX before each SVC puts the monitor
fields in the state every other exclusive case of the corpus ends in.

The cases are the X and W forms, with and without acquire/release, each
taking the mismatch and the match. The match runs the first store-exclusive
and leaves at the first SVC; the mismatch runs the alternate store-exclusive
and leaves at the second. Either way a pass retires seven instructions, and
every retried pass (a store-exclusive that failed without cause) five more.

The expectation is the architecture's: the compare sets NZCV as
AddWithCarry(Rn, NOT(Rm), 1), a store-exclusive writes zero to its status on
success, and only the registers the loop names change. The width of the
compare is the width of the load, so a W form compares the low 32 bits and
leaves the other four bytes of the location as they were.

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

SITE = "tools/arm64-tests/exclusive-mismatch-corpus.py"
PRODUCER = "arm64-exclusive-mismatch"
PAGE = 4096
# The writable page holding the location, and the location's offset in it.
DATA = 0x5100_0000
OFFSET = 0x100
READ_WRITE_FLAGS = 0x13
SVC = 0xD400_0001
CLREX = 0xD503_3F5F
CMP_X = 0xEB04_007F  # cmp x3, x4
CMP_W = 0x6B04_007F  # cmp w3, w4
# Word indices of the program: the load, the compare, the branch, the first
# store, its retry, CLREX and SVC, then the alternate store, its retry, CLREX
# and SVC.
FIRST_STORE = 3
ALTERNATE = 7
FIRST_EXIT = 6
SECOND_EXIT = 10
B_NE_ALT = 0x5400_0001 | ((ALTERNATE - 2) << 5)  # b.ne alt
CBNZ_W5_TOP = 0x3500_0005 | ((-(FIRST_STORE + 1) & 0x7FFFF) << 5)
CBNZ_W6_TOP = 0x3500_0006 | ((-(ALTERNATE + 1) & 0x7FFFF) << 5)

# The location's contents, the value the match stores, the value the
# mismatch's alternate store stores, and the first compared value; all
# distinct in every byte so a wrong width or address shows.
LOCATION = 0x1122_3344_5566_7788
MATCH_STORE = 0xAABB_CCDD_1122_3344
ALTERNATE_STORE = 0x8899_AABB_CCDD_EEFF
MASK64 = (1 << 64) - 1


@dataclasses.dataclass(frozen=True)
class Form:
    name: str
    wide: bool  # the 64-bit form
    load: int
    compare: int
    store: int  # stxr w5, x2, [x1]
    alternate: int  # stxr w6, x7, [x1]

    @property
    def words(self) -> tuple[int, ...]:
        return (self.load, self.compare, B_NE_ALT, self.store,
                CBNZ_W5_TOP, CLREX, SVC, self.alternate, CBNZ_W6_TOP,
                CLREX, SVC)


FORMS = (
    Form("ldxr/stxr x", True, 0xC85F_7C23, CMP_X, 0xC805_7C22,
         0xC806_7C27),
    Form("ldxr/stxr w", False, 0x885F_7C23, CMP_W, 0x8805_7C22,
         0x8806_7C27),
    Form("ldaxr/stlxr x", True, 0xC85F_FC23, CMP_X, 0xC805_FC22,
         0xC806_FC27),
    Form("ldaxr/stlxr w", False, 0x885F_FC23, CMP_W, 0x8805_FC22,
         0x8806_FC27),
)


def nzcv_of_compare(left: int, right: int, bits: int) -> int:
    """NZCV of CMP: AddWithCarry(left, NOT(right), 1) at `bits` wide."""
    mask = (1 << bits) - 1
    left &= mask
    right &= mask
    result = (left - right) & mask
    negative = result >> (bits - 1)
    zero = int(result == 0)
    carry = int(left >= right)
    overflow = ((left ^ right) & (left ^ result)) >> (bits - 1) & 1
    return (negative << 31) | (zero << 30) | (carry << 29) | (overflow << 28)


def page_with(location: int) -> bytes:
    """The page: a pattern everywhere, the location's eight bytes at OFFSET."""
    data = bytearray((7 * index + 1) & 0xFF for index in range(PAGE))
    data[OFFSET:OFFSET + 8] = location.to_bytes(8, "little")
    return bytes(data)


def digest_of(location: int) -> str:
    return hashlib.sha256(page_with(location)).hexdigest()


def case_item(form: Form, mismatch: bool, program: int) -> dict:
    bits = 64 if form.wide else 32
    mask = (1 << bits) - 1
    loaded = LOCATION & mask
    initial: dict[str, str] = {
        "x1": corpus.hex64(DATA + OFFSET),
        "x2": corpus.hex64(MATCH_STORE),
        "x7": corpus.hex64(ALTERNATE_STORE),
    }
    # The mismatch compares against a different value; the high half of the
    # W form's register is garbage the compare ignores.
    if mismatch:
        compared = loaded ^ 0x1
        initial["x4"] = corpus.hex64(
            compared if form.wide else 0xFFFF_FFFF_0000_0000 | compared)
    else:
        initial["x4"] = corpus.hex64(
            loaded if form.wide else 0xDEAD_BEEF_0000_0000 | loaded)
    stored = ALTERNATE_STORE if mismatch else MATCH_STORE
    final = (LOCATION & ~mask & MASK64) | (stored & mask)
    expected = {
        "x3": corpus.hex64(loaded),
        "nzcv": corpus.hex64(nzcv_of_compare(
            loaded, int(initial["x4"], 16), bits)),
        "pc": corpus.hex64(corpus.CODE_BASE + 4 *
                           ((SECOND_EXIT if mismatch else FIRST_EXIT) + 1)),
    }
    # The status register of the store that ran gets zero.
    expected["x6" if mismatch else "x5"] = corpus.hex64(0)
    initial_page = page_with(LOCATION)
    final_page = page_with(final)
    runs = corpus.runs_between(initial_page, final_page)
    kind = "mismatch to the alternate store-exclusive" if mismatch else \
        "match to the first store-exclusive"
    return {
        "name": f"{form.name} {kind}", "program": program,
        "event": "system-call", "ordinals": [0, 0],
        "invalid_encoding": False, "minimization": "full", "native": [],
        "initial": initial, "expected": expected,
        # One pass of seven instructions, plus five for each retried pass.
        "retired": {"minimum": 7, "step": 5, "maximum": None},
        "memory": [{
            "address": corpus.hex64(DATA), "size": PAGE,
            "flags": READ_WRITE_FLAGS, "blob": digest_of(LOCATION),
            "expected": [[offset, data.hex()] for offset, data in runs],
            "tags": [], "expected_tags": []}],
    }


@functools.cache
def build() -> dict:
    """The record."""
    items = []
    for program, form in enumerate(FORMS):
        for mismatch in (True, False):
            items.append(case_item(form, mismatch, program))
    names = [item["name"] for item in items]
    if len(set(names)) != len(names):
        raise SystemExit("two cases share a name")
    return {
        "record": f"{PRODUCER}/4k", "producer": PRODUCER, "site": SITE,
        "profiles": ["4k"], "options": [],
        "programs": [{"entry": 0,
                      "words": [[4 * offset, f"{word:08x}"]
                                for offset, word in enumerate(form.words)]}
                     for form in FORMS],
        "cases": items,
    }


def counts() -> dict[str, int]:
    cases = build()["cases"]
    return {
        "cases": len(cases),
        "mismatch_cases": sum(1 for case in cases
                              if "mismatch" in case["name"]),
        "match_cases": sum(1 for case in cases if "match to" in case["name"]
                           and "mismatch" not in case["name"]),
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
    digest = digest_of(LOCATION)
    if digest not in known:
        fresh.append(json.dumps({
            "blob": digest, "zlib": zlib.compress(page_with(LOCATION), 9)
            .hex()}, sort_keys=True))
    summary = counts()
    header["meta"]["exclusive_mismatch"] = {
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
    print(f"appended {summary['cases']} compare-retry exclusive cases "
          f"({summary['mismatch_cases']} mismatch, "
          f"{summary['match_cases']} match)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
