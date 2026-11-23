#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's indirect-branch target records.

tools/arm64-tests/control-target-corpus.py writes the cases that branch
through BR, BLR, RET and the fused epilogue to the all-ones address, one word
below it, a misaligned address and addresses nothing maps. This test
re-derives the contract from the current script and hand-written values:

- provenance.json names the script and its case counts;
- the one arm64-control-target record is the script's;
- the encodings are the words written out here;
- a target with a low bit set is a PC alignment fault and every other is an
  instruction fetch fault, both at the target with execute access;
- the branch retires, a BLR writes its link register, the epilogue restores
  its pair and advances SP, and nothing else changes; and
- the targets the unit tests of the gadget engine's cached sites rely on are
  all present.

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
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "control-target-corpus.py"
CODE_BASE = 0x4F00_0000
ALL_ONES = (1 << 64) - 1


def load_generator():
    spec = importlib.util.spec_from_file_location("control_target_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check_encodings(gen) -> None:
    # BR X15, BLR X15, RET X15, RET, LDP X29, X30, [SP], #16.
    for built, word in ((gen.BR_X15, 0xD61F_01E0),
                        (gen.BLR_X15, 0xD63F_01E0),
                        (gen.RET_X15, 0xD65F_01E0),
                        (gen.RET, 0xD65F_03C0),
                        (gen.LDP_X29_X30_POST_16, 0xA8C1_7BFD)):
        if built != word:
            raise SystemExit(f"{built:08x} is not the word {word:08x}")


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
    names = set()
    targets = set()
    mnemonics = set()
    for case in record["cases"]:
        name = case["name"]
        names.add(name)
        mnemonic, _, text = name.rpartition(" to ")
        target = int(text, 16)
        targets.add(target)
        mnemonics.add(mnemonic)
        words = programs[case["program"]]
        initial = case["initial"]
        expected = case["expected"]
        # The instruction, by the word written out here.
        if words == [0xD61F_01E0]:
            holder, links, pops = "x15", False, False
        elif words == [0xD63F_01E0]:
            holder, links, pops = "x15", True, False
        elif words == [0xD65F_01E0]:
            holder, links, pops = "x15", False, False
        elif words == [0xD65F_03C0]:
            holder, links, pops = "x30", False, False
        elif words == [0xA8C1_7BFD, 0xD65F_03C0]:
            holder, links, pops = None, False, True
        else:
            raise SystemExit(f"{name}: an unexpected program")
        if case["invalid_encoding"] or case["native"] or \
                "tbi_enabled" in initial:
            raise SystemExit(f"{name}: not a plain top-byte-ignore-off case")
        if holder is not None and int(initial[holder], 16) != target:
            raise SystemExit(f"{name}: the target is not in {holder}")
        # The architecture: a set low bit is a PC alignment fault, any other
        # target nothing maps is a fetch fault; the fault is at the target.
        if case["event"] != ("pc-alignment" if target & 3 else
                             "memory-fault"):
            raise SystemExit(f"{name}: the wrong event")
        fault = case["fault"]
        if int(fault["address"], 16) != target or \
                fault["access"] != "execute":
            raise SystemExit(f"{name}: not an execute fault at the target")
        if case["retired"] != len(words):
            raise SystemExit(f"{name}: the instructions did not retire")
        if int(expected["pc"], 16) != target:
            raise SystemExit(f"{name}: the PC is not the target")
        written = set(expected) - {"pc"}
        if links:
            if written != {"x30"} or \
                    int(expected["x30"], 16) != CODE_BASE + 4:
                raise SystemExit(f"{name}: BLR did not write its link")
        elif pops:
            if written != {"x29", "x30", "sp"} or \
                    int(expected["x30"], 16) != target or \
                    int(expected["sp"], 16) != int(initial["sp"], 16) + 16:
                raise SystemExit(f"{name}: the epilogue did not restore "
                                 "its pair")
        elif written:
            raise SystemExit(f"{name}: a branch changed {sorted(written)}")
    if len(names) != len(record["cases"]):
        raise SystemExit("two cases share a name")
    # The values the unit tests of the cached sites use.
    for required in (ALL_ONES, ALL_ONES - 2, ALL_ONES - 3, 0x1002, 0,
                     0xDEAD_BEEF_0000_1000):
        if required not in targets:
            raise SystemExit(f"no case for target {required:#x}")
    if len(mnemonics) != 5:
        raise SystemExit(f"{len(mnemonics)} forms, expected five")
    if len(record["cases"]) != len(targets) * len(mnemonics):
        raise SystemExit("a form lacks a target")
    return len(record["cases"])


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    corpus, _tokens = builder.decode_pack(pathlib.Path(sys.argv[1]).read_bytes())
    check_encodings(gen)
    ours = list(builder.dump_records(corpus, [gen.PRODUCER]))
    if len(ours) != 1:
        raise SystemExit(f"{len(ours)} arm64-control-target records, "
                         "expected one")
    cases = check_record(gen, ours[0], corpus.meta.get("control_target", {}))
    print(f"indirect-branch target records: {cases} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
