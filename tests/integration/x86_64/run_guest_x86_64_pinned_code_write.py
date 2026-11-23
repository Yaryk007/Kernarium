#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a kernel read into a page the process runs reaches the code.

Cross-builds the freestanding pinned_code_write_probe.cpp for x86-64 and boots
an x86-64 image on an emulator built with the macos-x86_64 preset, with a second
disk (/dev/ubdb) that holds a function returning 83. The probe runs a function
that returns 41 from an anonymous page and from a shared memfd page, reads the
disk into the page with a plain O_DIRECT pread() and with an io_uring READ_FIXED
into a buffer registered (so pinned) before the page first ran, and requires
the new function to run afterwards: the kernel wrote the page through its own
alias with no process on the path, and names the frame to the host when the
write ends. A fifth case has a second process run the page without a pause while
a plain read fills it, which is the window between the flush the read's pin makes
and the write. Two more read a buffered file, and a loop device over it, into a
registered page with READ_FIXED: the kernel copies the file's bytes into the page
with the generic copy-out of a buffer vector, which ends each page it wrote the
same way.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

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


PROBE = pathlib.Path(__file__).with_name("pinned_code_write_probe.cpp")
PROBES = {"pinned-code-write-probe": ("x86-64", X86_64)}
GUEST_PROBE = f"{GUEST_DIRECTORY}/pinned-code-write-probe"
MODES = ("direct-anon", "direct-shared", "fixed-anon", "fixed-shared", "concurrent",
         "fixed-file", "loop-fixed")
VERDICT = re.compile(
    r"^pinned-code-write: mode=([\w-]+) failed=(\d+) verdict=(\w+)$", re.MULTILINE)
KERNEL_FAILURE = re.compile(r"WARNING:|BUG:|Oops|Call Trace:|Kernel panic")
CPUS = 2
PROBE_TIMEOUT_SECONDS = 60
PAGE = 4096
# The function the disk holds: mov eax, 83; ret.
DISK_PAGE = bytes([0xB8, 83, 0, 0, 0, 0xC3]) + bytes(PAGE - 6)


def main() -> int:
    arguments = parse_arguments(
        __doc__, compiler_help="clang driver with an x86-64 Linux target and ld.lld",
        timeout_help="boot timeout")
    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    failures = []
    with tempfile.TemporaryDirectory(prefix="uml-pinned-code-write-") as scratch:
        directory = pathlib.Path(scratch)
        disk = directory / "disk.bin"
        disk.write_bytes(DISK_PAGE)
        with Guest(
            arguments.emulator,
            arguments.rootfs,
            "512M",
            "tmpfs",
            (f"ncpus={CPUS}", f"ubd1={disk}"),
            init_shell=True,
            engine=arguments.engine,
            payloads=payloads,
        ) as guest:
            guest.boot_to_shell(arguments.timeout)
            for mode in MODES:
                try:
                    result = guest.run_command(f"{GUEST_PROBE} {mode}", PROBE_TIMEOUT_SECONDS)
                except Exception as error:  # a guest that panicked takes its console with it
                    print(f"FAIL {mode}: the guest did not finish the probe: {type(error).__name__}",
                          flush=True)
                    failures.append(f"{mode}: the probe did not finish")
                    break
                for line in result.output.splitlines():
                    if line.startswith("pinned-code-write:"):
                        print(line, flush=True)
                match = VERDICT.search(result.output)
                if result.return_code != 0 or match is None or match.group(3) != "ok":
                    print(f"FAIL {mode}: exit {result.return_code}", flush=True)
                    failures.append(f"{mode}: the mode failed")
            console = guest.output()
            reported = sorted({line.strip() for line in console.splitlines()
                               if KERNEL_FAILURE.search(line)})
            if reported:
                failures.append("the kernel reported a failure")
                print("FAIL the kernel reported", flush=True)
                for line in reported[:8]:
                    print(f"  {line}", flush=True)
            guest.shutdown(60)
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        raise RuntimeError(f"{len(failures)} check(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
