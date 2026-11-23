#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what a fork leaves of an x86-64 process's address space.

Cross-builds the freestanding smp/guest_fork_flush.cpp for x86-64 and boots an
x86-64 image on an emulator built with the macos-x86_64 preset. Two parts:

The rows: the probe forks over private anonymous pages, the stack, .data and a
writable private file mapping, a shared page, a page mprotect() made read-only,
and private counters a sibling thread keeps writing during the fork, and each
side must see only its own bytes.

The counters: the kernel invalidates on the host only the writable private
mappings a fork write-protects. A parent that calls 32 code pages of its own
text after each of 1000 forks must register its frames with the translators
(holder_registrations in the UML_X86_ACCOUNTING_LOG line the emulator appends
when it ends) at most once per four forks more than a parent that only forks,
and a parent that runs one writable and executable private page after each fork
must register at least once per two forks more: the page fork write-protected
still loses its translations. A boot's own registrations vary by about a
hundred, which the thousand forks outweigh.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
import pathlib
import re
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest  # noqa: E402
from probe_runner import (  # noqa: E402
    GUEST_DIRECTORY, X86_64, build_probes, parse_arguments)


PROBE = REPOSITORY / "tests" / "integration" / "smp" / "guest_fork_flush.cpp"
PROBES = {"fork-flush-probe": ("x86-64", X86_64)}
GUEST_PROBE = f"{GUEST_DIRECTORY}/fork-flush-probe"
VERDICT = re.compile(
    r"^fork-flush: abi=x86-64 checks=(\d+) failed=(\d+) verdict=(\w+)$",
    re.MULTILINE)
REGISTRATIONS = re.compile(r"holder_registrations=(\d+)")
FORKS = 1000
CPUS = 4
PROBE_TIMEOUT_SECONDS = 180


def registrations(log: pathlib.Path) -> int | None:
    """The holder registrations the emulator reported when it ended."""
    if not log.is_file():
        return None
    found = REGISTRATIONS.findall(log.read_text())
    return int(found[-1]) if found else None


def counted_run(arguments, payloads, directory: pathlib.Path, mode: str) -> int | None:
    """One boot that runs the probe in `mode` and returns its registrations."""
    log = directory / f"{mode}.log"
    os.environ["UML_X86_ACCOUNTING_LOG"] = str(log)
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={CPUS}",),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(f"{GUEST_PROBE} {mode} {FORKS}", PROBE_TIMEOUT_SECONDS)
        print(result.output.strip(), flush=True)
        if result.return_code != 0:
            raise RuntimeError(f"{mode}: the probe exited {result.return_code}")
        guest.shutdown(60)
    return registrations(log)


def main() -> int:
    arguments = parse_arguments(
        __doc__, compiler_help="clang driver with an x86-64 Linux target and ld.lld",
        timeout_help="boot timeout")
    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    failures = []
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={CPUS}",),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(GUEST_PROBE, PROBE_TIMEOUT_SECONDS)
        for line in result.output.splitlines():
            if line.startswith("fork-flush: FAIL"):
                print(line, flush=True)
        match = VERDICT.search(result.output)
        if match is None:
            raise RuntimeError(f"exit {result.return_code}, no verdict in {result.output!r}")
        print(match.group(0), flush=True)
        if result.return_code != 0 or match.group(3) != "ok" or match.group(2) != "0":
            failures.append("a side of a fork saw bytes that were not its own")

    with tempfile.TemporaryDirectory(prefix="uml-fork-flush-") as scratch:
        directory = pathlib.Path(scratch)
        totals = {mode: counted_run(arguments, payloads, directory, mode)
                  for mode in ("bare", "text", "rwx")}
    print(f"holder registrations: {totals}", flush=True)
    if None in totals.values():
        failures.append("the emulator reported no holder registrations")
    else:
        text = totals["text"] - totals["bare"]
        rwx = totals["rwx"] - totals["bare"]
        print(f"  {FORKS} forks over 32 text pages, against forks alone: {text:+d} registrations",
              flush=True)
        print(f"  {FORKS} forks over one writable and executable page: {rwx:+d}", flush=True)
        if text >= FORKS // 4:
            failures.append(f"a fork made the parent register its text again: {text:+d}")
        if rwx < FORKS // 2:
            failures.append(f"a fork left the parent's writable and executable page "
                            f"registered: {rwx:+d}")
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        raise RuntimeError(f"{len(failures)} check(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
