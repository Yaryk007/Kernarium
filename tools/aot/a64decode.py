# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact A64 instruction identification from the repository's decode table.

tests/unit/arm64/a64_instruction_decode_partitions.tsv lists every encoding of
the architecture (mask, value, field constraints, decode class, feature) as
the architecture XML gives them. This module looks a word up in it; the
rewriter's policy decides what each (class, mnemonic, feature) becomes.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import dataclasses
import pathlib
import re

DEFAULT_TABLE = (pathlib.Path(__file__).resolve().parents[2] / "tests" /
                 "unit" / "arm64" / "a64_instruction_decode_partitions.tsv")

_CONSTRAINT = re.compile(r"^(\w+)@(\d+):(\d+)!=([01x]+)$")


@dataclasses.dataclass(frozen=True)
class Encoding:
    name: str
    mnemonic: str
    decode_class: str
    feature: str
    mask: int
    value: int
    constraints: tuple[tuple[int, int, int, int], ...]

    def matches(self, word: int) -> bool:
        if word & self.mask != self.value:
            return False
        for lsb, width, care, pattern in self.constraints:
            field = (word >> lsb) & ((1 << width) - 1)
            if field & care == pattern:
                return False
        return True


class Decoder:
    def __init__(self, table: pathlib.Path = DEFAULT_TABLE) -> None:
        self._by_mask: dict[int, dict[int, list[Encoding]]] = {}
        with open(table, encoding="utf-8") as handle:
            header = handle.readline().rstrip("\n").split("\t")
            column = {name: index for index, name in enumerate(header)}
            for line in handle:
                fields = line.rstrip("\n").split("\t")
                if fields[column["decode_layer"]] != "instruction-table":
                    continue
                constraints = []
                text = fields[column["architecture_constraints"]]
                if text != "none":
                    for part in text.split(";"):
                        match = _CONSTRAINT.match(part)
                        if match is None:
                            raise ValueError(f"unknown constraint {part!r}")
                        lsb = int(match.group(2))
                        width = int(match.group(3))
                        pattern = match.group(4)
                        if len(pattern) != width:
                            raise ValueError(f"bad constraint {part!r}")
                        care = 0
                        value = 0
                        for char in pattern:
                            care <<= 1
                            value <<= 1
                            if char != "x":
                                care |= 1
                                value |= int(char)
                        constraints.append((lsb, width, care, value))
                name = fields[column["partition_name"]]
                encoding = Encoding(
                    name=name,
                    mnemonic=name.split("_", 1)[0],
                    decode_class=fields[column["decode_class"]],
                    feature=fields[column["feature_condition"]],
                    mask=int(fields[column["architecture_mask"]], 16),
                    value=int(fields[column["architecture_value"]], 16),
                    constraints=tuple(constraints))
                self._by_mask.setdefault(encoding.mask, {}).setdefault(
                    encoding.value, []).append(encoding)
        self._masks = sorted(self._by_mask, key=lambda m: -bin(m).count("1"))
        self._cache: dict[int, Encoding | None] = {}

    def encodings(self) -> list[Encoding]:
        """Every encoding of the table, in a fixed order."""
        return [encoding for mask in sorted(self._by_mask)
                for value in sorted(self._by_mask[mask])
                for encoding in self._by_mask[mask][value]]

    def decode(self, word: int) -> Encoding | None:
        """The one encoding that holds `word`, or None when none does."""
        if word in self._cache:
            return self._cache[word]
        result = None
        for mask in self._masks:
            for encoding in self._by_mask[mask].get(word & mask, ()):
                if encoding.matches(word):
                    result = encoding
                    break
            if result is not None:
                break
        self._cache[word] = result
        return result
