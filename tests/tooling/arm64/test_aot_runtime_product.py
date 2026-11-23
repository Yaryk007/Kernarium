#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The product compilation of the AOT runtime carries no test switch.

A build that sets UML_AOT_DIAGNOSTICS links a second compilation of the same
source with the counters, the report and the switches the differential test
drives (set_enabled, set_refill_stress, set_run_hook, reset_stats). The
compilation every other build links must hold none of them in any
configuration. The object file is checked for the diagnostics namespace and
for the strings only the diagnostics compilation has.
"""

__import__("sys").dont_write_bytecode = True

import pathlib
import subprocess
import sys

FORBIDDEN_SYMBOLS = ("11diagnostics",)
FORBIDDEN_STRINGS = (b"DARWIN_UML_AOT_REPORT",)
REQUIRED_SYMBOLS = ("3aot7enabledEv", "3aot5enterE")


def inspect(path: pathlib.Path):
    symbols = subprocess.run(["/usr/bin/nm", "-g", str(path)], check=True,
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True).stdout
    return symbols, path.read_bytes()


def check(symbols: str, content: bytes):
    problems = []
    for needle in FORBIDDEN_SYMBOLS:
        if needle in symbols:
            problems.append("forbidden symbol text %r" % needle)
    for needle in FORBIDDEN_STRINGS:
        if needle in content:
            problems.append("forbidden string %r" % needle.decode())
    # An object that lost the runtime itself would pass every check above.
    for needle in REQUIRED_SYMBOLS:
        if needle not in symbols:
            problems.append("missing runtime symbol text %r" % needle)
    return problems


def main(argv) -> int:
    if len(argv) != 2:
        print("usage: test_aot_runtime_product.py <aot_runtime.o>",
              file=sys.stderr)
        return 2
    problems = check(*inspect(pathlib.Path(argv[1])))
    for problem in problems:
        print("FAIL:", problem, file=sys.stderr)
    if not problems:
        print("the product AOT runtime holds no diagnostics or test switch")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
