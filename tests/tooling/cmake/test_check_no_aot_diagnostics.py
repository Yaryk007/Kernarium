#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the packaging check finds the AOT diagnostics build."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


TIMEOUT_SECONDS = 60
MACHO = bytes.fromhex("cffaedfe") + bytes(60)
AR = b"!<arch>\n" + bytes(60)
CLEAN = b"_uml_aot_enter _ZN3uml3cpu5arm643aot5enterE" + bytes(16)
SYMBOL = b"__ZN3uml3cpu5arm643aot11diagnostics5resetEv" + bytes(16)
STRING = b"DARWIN_UML_AOT_REPORT\0" + bytes(16)
# The memory layer's compilation with the executable-mapping count audit
# (darwin/memory/count_audit.hpp): its three entry points, the record reader
# its recount uses, and the string it keeps once the symbols are stripped. The
# production memory layer holds none of them, and the checker reads the audit
# as a diagnostics build whatever else the product linked. The names are what
# `nm` reports for the audited objects, which test_audit_symbols_refused.py
# reads from the build itself.
AUDIT_SYMBOLS = (
    b"__ZN3uml6memory23audit_executable_countsEv",
    b"__ZN3uml6memory15count_anomaliesEv",
    b"__ZN3uml6memory18note_count_anomalyEv",
    b"__ZN3uml6memory13MappedBacking19executable_mappingsEPKv",
)
AUDIT_STRING = b"UML_MEMORY_COUNT_AUDIT_BUILD\0" + bytes(16)
# The memory layer's own names the audit does not own.
MEMORY = b"__ZN3uml6memory18latch_sharing_offEv" + bytes(16)
# The gadget engine's own diagnostics namespace is not the AOT's.
GADGETS = b"__ZN3uml3cpu5arm647gadgets11diagnostics5resetEv" + bytes(16)


def run(checker: Path, paths: list[Path]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(checker), *map(str, paths)],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    options = parser.parse_args()

    failures = []
    with tempfile.TemporaryDirectory(prefix="uml-no-aot-diagnostics-") as name:
        root = Path(name)
        clean = root / "clean.o"
        clean.write_bytes(MACHO + CLEAN)
        symbol = root / "symbol.o"
        symbol.write_bytes(MACHO + SYMBOL)
        gadgets = root / "gadgets.o"
        gadgets.write_bytes(MACHO + GADGETS)
        memory = root / "memory.o"
        memory.write_bytes(MACHO + MEMORY)
        audit_archive = root / "libaudit.a"
        audit_archive.write_bytes(AR + CLEAN + AUDIT_SYMBOLS[0] + bytes(16))
        audit_stripped = root / "audit-stripped.o"
        audit_stripped.write_bytes(MACHO + AUDIT_STRING)
        stripped = root / "stripped.o"
        stripped.write_bytes(MACHO + STRING)
        archive = root / "libplanted.a"
        archive.write_bytes(AR + CLEAN + SYMBOL)
        # A framework binary nested in an application bundle, and a text
        # file that names the marker without being a binary.
        bundle = root / "Fixture.app" / "Frameworks" / "Fixture.framework"
        bundle.mkdir(parents=True)
        (bundle / "Fixture").write_bytes(MACHO + STRING)
        (root / "Fixture.app" / "notes.txt").write_bytes(STRING)
        empty = root / "Empty.app"
        empty.mkdir()
        (empty / "notes.txt").write_bytes(STRING)

        audit_objects = []
        for number, symbol_text in enumerate(AUDIT_SYMBOLS):
            audit_object = root / f"audit-{number}.o"
            audit_object.write_bytes(MACHO + symbol_text + bytes(16))
            audit_objects.append(audit_object)
        cases = (
            ("clean object", [clean], 0, "no AOT diagnostics build in 1"),
            ("gadget diagnostics", [gadgets], 0,
             "no AOT diagnostics build in 1"),
            ("diagnostics symbol", [symbol], 1, "3aot11diagnostics"),
            ("report name", [stripped], 1, "DARWIN_UML_AOT_REPORT"),
            ("archive member", [archive], 1, "3aot11diagnostics"),
            ("nested framework", [root / "Fixture.app"], 1,
             "DARWIN_UML_AOT_REPORT"),
            ("memory layer without the audit", [memory], 0,
             "no AOT diagnostics build in 1"),
            *(
                (f"audit entry point {number}", [audit_object], 1,
                 "count audit")
                for number, audit_object in enumerate(audit_objects)
            ),
            ("audit in an archive member", [audit_archive], 1, "count audit"),
            ("audit once stripped", [audit_stripped], 1, "count audit"),
            ("bundle without binaries", [empty], 1, "no Mach-O binary"),
            ("missing path", [root / "absent"], 1, "does not exist"),
        )
        for label, paths, expected, message in cases:
            checked = run(options.checker, paths)
            if checked.returncode != expected or message not in checked.stdout:
                failures.append(
                    f"{label}: status {checked.returncode}, expected "
                    f"{expected} with {message!r}:\n{checked.stdout}"
                )

    if failures:
        raise SystemExit("\n".join(failures))
    print("the AOT packaging check accepted 3 and rejected 12 products")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
