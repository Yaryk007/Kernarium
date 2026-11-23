#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail if a packaged product carries the AOT diagnostics build.

UML_AOT_DIAGNOSTICS composes a second compilation of the AOT runtime with
counters, a report writer and the test switches, and a second compilation of
the memory layer with the executable-mapping count audit the runtime reports.
The unit rows `arm64-aot-runtime-product` and `memory-product-carries-no-
count-audit` check the products' own objects; this check reads what a package
or a built product linked. Every Mach-O binary and static archive below each
path must hold neither the diagnostics namespace, nor the audit's entry
points, nor the report environment name and the audit's own string, which
outlive a stripped symbol table.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path


# Mach-O magics of both byte orders, thin and universal, and the ar magic.
MAGICS = (
    bytes.fromhex("feedfacf"),
    bytes.fromhex("cffaedfe"),
    bytes.fromhex("feedface"),
    bytes.fromhex("cefaedfe"),
    bytes.fromhex("cafebabe"),
    bytes.fromhex("bebafeca"),
    b"!<arch>\n",
)
# The AOT diagnostics namespace in a mangled name (the gadget engine has a
# namespace of the same last name, so the match includes `aot`), and the
# variable only the diagnostics compilation reads. The memory layer's count
# audit (darwin/memory/count_audit.hpp) is named by its three entry points and
# the record reader the recount uses, all in namespace uml::memory, and by the
# string the audited compilation keeps. The lengths are the Itanium name
# lengths, so tests/tooling/cmake/test_audit_symbols_refused.py checks them
# against the names `nm` reports for the audited objects.
AUDIT_MARKERS = (
    b"6memory23audit_executable_counts",
    b"6memory15count_anomalies",
    b"6memory18note_count_anomaly",
    b"13MappedBacking19executable_mappings",
    b"UML_MEMORY_COUNT_AUDIT_BUILD",
)
MARKERS = (b"3aot11diagnostics", b"DARWIN_UML_AOT_REPORT") + AUDIT_MARKERS


def binaries(root: Path) -> list[Path]:
    candidates = [root] if root.is_file() else sorted(root.rglob("*"))
    found = []
    for path in candidates:
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as handle:
            head = handle.read(8)
        if any(head.startswith(magic) for magic in MAGICS):
            found.append(path)
    return found


def problems(path: Path) -> list[str]:
    data = path.read_bytes()
    return [
        f"{path}: holds the AOT diagnostics build's {marker.decode()}"
        + (" (the memory layer's count audit)" if marker in AUDIT_MARKERS else "")
        for marker in MARKERS
        if marker in data
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    arguments = parser.parse_args()

    checked = 0
    failures = []
    for root in arguments.paths:
        if not root.exists():
            failures.append(f"{root}: does not exist")
            continue
        found = binaries(root)
        if not found:
            failures.append(f"{root}: holds no Mach-O binary or archive")
        for path in found:
            checked += 1
            failures.extend(problems(path))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"no AOT diagnostics build in {checked} binaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
