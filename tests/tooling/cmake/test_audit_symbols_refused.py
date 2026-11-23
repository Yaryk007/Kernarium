#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the packaging check refuses the audited memory layer's own symbols.

The memory layer's count audit (darwin/memory/count_audit.hpp) is compiled
twice from the same sources. This reads the symbol tables of both
compilations built in this tree, takes every memory-layer name the audited
compilation defines and the production one does not, and shows that
scripts/check-no-aot-diagnostics.py refuses an object holding any one of them.
The names come from `nm`, not from a list in this file, so a name the checker
spells wrongly cannot be spelled wrongly here as well.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


TIMEOUT_SECONDS = 60
MACHO = bytes.fromhex("cffaedfe") + bytes(60)
# The memory layer's own namespace, free and member functions; the standard
# library's instantiations that the audit happens to need are not its names.
MEMORY_NAME = re.compile(r"^_+ZNK?3uml6memory")
# Fewer than the three entry points of count_audit.hpp means this read the
# wrong objects.
LEAST_SYMBOLS = 3


def defined_symbols(nm: str, directory: Path) -> set[str]:
    objects = sorted(directory.rglob("*.o"))
    if not objects:
        raise SystemExit(f"{directory}: holds no object; build it first")
    names: set[str] = set()
    for path in objects:
        listing = subprocess.run(
            [nm, "-gU", str(path)],
            check=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            text=True,
            timeout=TIMEOUT_SECONDS,
        ).stdout
        names.update(line.split()[-1] for line in listing.splitlines() if line.strip())
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--audited", type=Path, required=True)
    parser.add_argument("--production", type=Path, required=True)
    options = parser.parse_args()

    nm = shutil.which("nm")
    if nm is None:
        raise SystemExit("nm is not on PATH")
    audit_only = sorted(
        name
        for name in defined_symbols(nm, options.audited)
        - defined_symbols(nm, options.production)
        if MEMORY_NAME.match(name)
    )
    if len(audit_only) < LEAST_SYMBOLS:
        raise SystemExit(
            f"only {len(audit_only)} memory-layer names are the audit's alone: {audit_only}"
        )

    failures = []
    with tempfile.TemporaryDirectory(prefix="uml-audit-symbols-") as name:
        root = Path(name)
        for number, symbol in enumerate(audit_only):
            fixture = root / f"audit-{number}.o"
            fixture.write_bytes(MACHO + symbol.encode() + bytes(16))
            checked = subprocess.run(
                [sys.executable, "-B", str(options.checker), str(fixture)],
                check=False,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=TIMEOUT_SECONDS,
            )
            if checked.returncode != 1 or "count audit" not in checked.stdout:
                failures.append(
                    f"{symbol}: status {checked.returncode}, expected 1 with "
                    f"the count audit named:\n{checked.stdout}"
                )
    if failures:
        raise SystemExit("\n".join(failures))
    print(f"the packaging check refused all {len(audit_only)} names the audit alone defines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
