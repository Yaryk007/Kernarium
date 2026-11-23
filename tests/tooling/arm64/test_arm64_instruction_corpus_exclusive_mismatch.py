#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's compare-retry exclusive records.

tools/arm64-tests/exclusive-mismatch-corpus.py writes the cases of a
compare-and-swap loop whose mismatch target holds a store-exclusive of its own.
This test re-derives the contract from the current script and hand-written
values:

- provenance.json names the script and its case counts;
- the one arm64-exclusive-mismatch record is the script's;
- the programs are the words written out here, with the branch to the
  alternate store, the retries to the load and a CLREX before each SVC;
- the mismatch runs the alternate store-exclusive and leaves at the second SVC,
  the match runs the first and leaves at the first, and each writes zero to its
  own status register only;
- the compare is as wide as the load: a W form compares the low 32 bits and
  stores into them alone, leaving the location's other four bytes; and
- a pass retires seven instructions and a retried pass five more.

build-unit-corpus.py --check already refuses a pack whose recorded script
hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "exclusive-mismatch-corpus.py"
CODE_BASE = 0x4F00_0000
DATA = 0x5100_0000
OFFSET = 0x100

B_NE_ALTERNATE = 0x5400_00A1  # b.ne pc + 20
CBNZ_W5_TOP = 0x35FF_FF85  # cbnz w5, pc - 16
CBNZ_W6_TOP = 0x35FF_FF06  # cbnz w6, pc - 32
CLREX = 0xD503_3F5F
SVC = 0xD400_0001

# Form: (load, compare, stxr w5, x2, [x1], stxr w6, x7, [x1], wide).
FORMS = {
    "ldxr/stxr x": (0xC85F_7C23, 0xEB04_007F, 0xC805_7C22, 0xC806_7C27, True),
    "ldxr/stxr w": (0x885F_7C23, 0x6B04_007F, 0x8805_7C22, 0x8806_7C27, False),
    "ldaxr/stlxr x": (0xC85F_FC23, 0xEB04_007F, 0xC805_FC22, 0xC806_FC27,
                      True),
    "ldaxr/stlxr w": (0x885F_FC23, 0x6B04_007F, 0x8805_FC22, 0x8806_FC27,
                      False),
}
PATHS = {
    "mismatch to the alternate store-exclusive": True,
    "match to the first store-exclusive": False,
}


def load_generator():
    spec = importlib.util.spec_from_file_location("exclusive_mismatch_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def page(location: int) -> bytes:
    data = bytearray((7 * index + 1) & 0xFF for index in range(4096))
    data[OFFSET:OFFSET + 8] = location.to_bytes(8, "little")
    return bytes(data)


def without_invalid_bits(item: dict) -> dict:
    return dict(item, cases=[{key: value for key, value in case.items()
                              if key != "invalid_encoding"}
                             for case in item["cases"]])


def check_record(gen, record: dict, meta: dict) -> int:
    if without_invalid_bits(record) != without_invalid_bits(gen.build()):
        raise SystemExit(f"{record['record']}: not the script's record")
    if meta.get("script") != gen.SITE or \
            any(meta.get(key) != value for key, value in gen.counts().items()) \
            or meta.get("sha256") != hashlib.sha256(
                GENERATOR.read_bytes()).hexdigest():
        raise SystemExit("provenance.json describes another script")
    if record["options"] != [] or record["profiles"] != ["4k"]:
        raise SystemExit(f"{record['record']}: not an optionless 4 KiB record")
    programs = [[int(word, 16) for _offset, word in program["words"]]
                for program in record["programs"]]
    if [program["entry"] for program in record["programs"]] != [0] * 4:
        raise SystemExit("a program does not start at its first word")
    seen = set()
    for case in record["cases"]:
        name = case["name"]
        form = next(known for known in FORMS if name.startswith(known + " "))
        mismatch = PATHS[name[len(form) + 1:]]
        seen.add((form, mismatch))
        load, compare, first, alternate, wide = FORMS[form]
        words = programs[case["program"]]
        if words != [load, compare, B_NE_ALTERNATE, first, CBNZ_W5_TOP, CLREX,
                     SVC, alternate, CBNZ_W6_TOP, CLREX, SVC]:
            raise SystemExit(f"{name}: an unexpected program")
        if case["event"] != "system-call" or "fault" in case or \
                case["invalid_encoding"] or case["native"]:
            raise SystemExit(f"{name}: not a plain system-call case")
        if case["retired"] != {"minimum": 7, "step": 5, "maximum": None}:
            raise SystemExit(f"{name}: not a pass of seven plus retries of "
                             "five")
        initial = case["initial"]
        expected = case["expected"]
        # The location, the registers, and the compare's flags: the mismatch
        # differs in the lowest bit, so the first value is below the second.
        location = 0x1122_3344_5566_7788
        loaded = location if wide else location & 0xFFFF_FFFF
        high = 0 if wide else (0xFFFF_FFFF_0000_0000 if mismatch
                               else 0xDEAD_BEEF_0000_0000)
        compared = (loaded ^ 1 if mismatch else loaded) | high
        if initial != {"x1": f"{DATA + OFFSET:#018x}",
                       "x2": "0xaabbccdd11223344",
                       "x7": "0x8899aabbccddeeff", "x4": f"{compared:#018x}"}:
            raise SystemExit(f"{name}: unexpected initial registers")
        # NZCV of loaded - (loaded ^ 1): the larger or smaller neighbour of
        # an even value is a borrow (N set, C clear); equal is Z and C.
        nzcv = 0x8000_0000 if mismatch else 0x6000_0000
        status = "x6" if mismatch else "x5"
        end = CODE_BASE + (44 if mismatch else 28)
        if expected != {"x3": f"{loaded:#018x}", "nzcv": f"{nzcv:#018x}",
                        "pc": f"{end:#018x}", status: f"{0:#018x}"}:
            raise SystemExit(f"{name}: unexpected final state")
        # The store: the alternate value on the mismatch, the first value on
        # the match; a W form changes its four low bytes only.
        stored = (0x8899_AABB_CCDD_EEFF if mismatch
                  else 0xAABB_CCDD_1122_3344)
        count = 8 if wide else 4
        run = [OFFSET, (stored & ((1 << 8 * count) - 1)).to_bytes(
            count, "little").hex()]
        region = case["memory"]
        if len(region) != 1 or region[0]["address"] != f"{DATA:#018x}" or \
                region[0]["size"] != 4096 or region[0]["flags"] != 0x13 or \
                region[0]["blob"] != hashlib.sha256(page(location)).hexdigest() \
                or region[0]["expected"] != [run]:
            raise SystemExit(f"{name}: unexpected memory")
    if len(seen) != 8 or len(record["cases"]) != 8:
        raise SystemExit("a form lacks a path")
    return len(record["cases"])


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    corpus, _tokens = builder.decode_pack(pathlib.Path(sys.argv[1]).read_bytes())
    ours = list(builder.dump_records(corpus, [gen.PRODUCER]))
    if len(ours) != 1:
        raise SystemExit(f"{len(ours)} arm64-exclusive-mismatch records, "
                         "expected one")
    cases = check_record(gen, ours[0],
                         corpus.meta.get("exclusive_mismatch", {}))
    print(f"compare-retry exclusive records: {cases} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
