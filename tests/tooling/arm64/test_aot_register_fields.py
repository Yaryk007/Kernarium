#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Which register fields of an inlined instruction the AOT rewriter treats as
general registers, against the system disassembler.

The rewriter substitutes a guest general register that its generated code
keeps elsewhere (x18, x24-x28 and sp) and must never touch a vector
register's field. This test takes a sample of words of every encoding of the
decode table that the rewriter copies, gives the four register positions
distinct numbers, has llvm-objdump print each word, and requires that every
position the disassembler prints as a general register is one the rewriter
substitutes and that no position it prints as a vector register is. A host
without the disassembler skips the test.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "tools" /
                       "aot"))

import a64decode  # noqa: E402
import rewrite  # noqa: E402

SAMPLES_PER_ENCODING = 6
POSITIONS = (0, 5, 10, 16)
TOKEN = re.compile(r"\b([xwvqdshb])(\d+)\b|\b(sp|wsp|xzr|wzr)\b")
LINE = re.compile(r"^\s*[0-9a-f]+:\s+([0-9a-f]{8})\s+(.*)$")
GENERAL = {"x", "w"}
VECTOR = {"v", "q", "d", "s", "h", "b"}


def find_objdump() -> str | None:
    found = shutil.which("llvm-objdump")
    if found:
        return found
    try:
        result = subprocess.run(["xcrun", "--find", "llvm-objdump"], capture_output=True,
                                text=True, check=False, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    path = result.stdout.strip()
    return path if result.returncode == 0 and path else None


def sample_words(decoder: a64decode.Decoder) -> list[tuple[int, a64decode.Encoding]]:
    rng = random.Random(0x5eed)
    words: list[tuple[int, a64decode.Encoding]] = []
    for encoding in decoder.encodings():
        taken = 0
        for _ in range(200):
            word = encoding.value | (rng.getrandbits(32) & ~encoding.mask & 0xffffffff)
            if decoder.decode(word) is not encoding:
                continue
            values = [(word >> position) & 31 for position in POSITIONS]
            if len(set(values)) != len(values):
                continue
            if rewrite.Library._copy_fields(None, word, encoding) is None:
                break
            words.append((word, encoding))
            taken += 1
            if taken == SAMPLES_PER_ENCODING:
                break
    return words


def disassemble(objdump: str, words: list[int]) -> dict[int, str]:
    with tempfile.TemporaryDirectory(prefix="aot-regfields-") as directory:
        source = pathlib.Path(directory) / "words.s"
        source.write_text(".text\n" + "".join(f".inst {word:#010x}\n" for word in words))
        obj = pathlib.Path(directory) / "words.o"
        subprocess.run(["clang", "--target=arm64-apple-macos", "-c", str(source), "-o", str(obj)],
                       check=True, capture_output=True, timeout=300)
        text = subprocess.run([objdump, "-d", "--mattr=+all", str(obj)], check=True,
                              capture_output=True, text=True, timeout=300).stdout
    rows: dict[int, str] = {}
    for line in text.splitlines():
        match = LINE.match(line)
        if match:
            # objdump prints the word as the bytes are stored: little endian
            # words print as the integer already.
            rows[int(match.group(1), 16)] = match.group(2)
    return rows


def kinds(text: str) -> dict[int, str]:
    """Number to 'general' or 'vector' for each register named in `text`."""
    named: dict[int, str] = {}
    for match in TOKEN.finditer(text):
        if match.group(3):
            named[31] = "general"
        elif match.group(1) in GENERAL:
            named[int(match.group(2))] = "general"
        elif match.group(1) in VECTOR:
            named.setdefault(int(match.group(2)), "vector")
    return named


class RegisterFieldSweep(unittest.TestCase):
    def test_general_register_fields_agree_with_the_disassembler(self) -> None:
        objdump = find_objdump()
        if objdump is None:
            self.skipTest("no llvm-objdump")
        decoder = a64decode.Decoder()
        sample = sample_words(decoder)
        self.assertGreater(len(sample), 1000)
        rows = disassemble(objdump, [word for word, _ in sample])
        checked = 0
        failures: list[str] = []
        for word, encoding in sample:
            text = rows.get(word)
            if text is None or text.startswith((".inst", "<unknown>", ".long")):
                continue
            named = kinds(text)
            general = set()
            vector = set()
            for position in POSITIONS:
                number = (word >> position) & 31
                kind = named.get(number)
                if kind == "general":
                    general.add(position)
                elif kind == "vector":
                    vector.add(position)
            fields = rewrite.Library._copy_fields(None, word, encoding)
            actual = {lsb for lsb, _ in fields}
            checked += 1
            # A field the text does not name is an immediate, a fixed field
            # or the zero register an alias leaves out; only the registers the
            # disassembler prints decide.
            if not general <= actual or actual & vector:
                failures.append(f"{word:#010x} {encoding.name} ({encoding.decode_class}): "
                                f"rewriter {sorted(actual)}, general {sorted(general)}, "
                                f"vector {sorted(vector)}: {text}")
        self.assertGreater(checked, 1000)
        self.assertEqual(failures[:20], [], f"{len(failures)} of {checked} words disagree")


if __name__ == "__main__":
    outcome = unittest.main(exit=False).result
    # 77 is CTest's skip: a host without the disassembler checked nothing.
    sys.exit(1 if not outcome.wasSuccessful() else 77 if outcome.skipped else 0)
