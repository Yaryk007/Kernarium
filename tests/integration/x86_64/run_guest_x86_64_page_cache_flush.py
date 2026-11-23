#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what the kernel's flush after a write into a mapped page costs.

Cross-builds the freestanding page_cache_flush_probe.cpp for x86-64 and boots an
x86-64 image on an emulator built with the macos-x86_64 preset, one boot per
mode. The x86_64 kernel names a frame it wrote to the host only where a mapping
with execute permission was installed for it:

ring1, ring2, ring3: the kernel's own writers of a user-mapped ring (an
AF_PACKET receive ring of each TPACKET version) run in interrupt context, where
the host cannot be called. The guest runs with panic_on_warn, and its console
must show no warning, no BUG and no oops.

marks: the kernel marks the frames of executable mappings, and no others, which
the probe reads from /proc/kpageflags. Every mapping it checks (including the
packet ring and the AIO ring) must be reported: a setup that fails fails the row.

data, code: the emulator's count of frames named (kernel_frame_writes in the
UML_X86_ACCOUNTING_LOG line it appends when it ends) is 0 after files mapped
without execute permission were written and read, and positive after a file the
process runs was.
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


PROBE = pathlib.Path(__file__).with_name("page_cache_flush_probe.cpp")
PROBES = {"page-cache-flush-probe": ("x86-64", X86_64)}
GUEST_PROBE = f"{GUEST_DIRECTORY}/page-cache-flush-probe"
VERDICT = re.compile(
    r"^page-cache-flush: mode=(\w+) failed=(\d+) verdict=(\w+)$", re.MULTILINE)
KERNEL_FAILURE = re.compile(r"WARNING:|BUG:|Oops|Call Trace:|Kernel panic")
FRAME_WRITES = re.compile(r"kernel_frame_writes=(\d+)")
MARK = re.compile(r"^page-cache-flush: mark ([\w-]+)=(\d+)$", re.MULTILINE)
RING_MODES = ("ring1", "ring2", "ring3")
# What the marks mode reports for each mapping. A mapping that is missing from
# its output fails the run, so that a setup that could not be made (an
# io_setup() that failed, say) is no pass.
EXPECTED_MARKS = {
    "anonymous": 0,
    "file-readable-writable": 0,
    "file-executable": 1,
    "file-executable-through-data-mapping": 1,
    "file-after-mprotect-to-executable": 1,
    "packet-ring": 0,
    "aio-ring": 0,
}
CPUS = 4
PROBE_TIMEOUT_SECONDS = 120


def frame_writes(log: pathlib.Path) -> int | None:
    """The frames the kernel named to the host, as the emulator reported them
    when it ended."""
    if not log.is_file():
        return None
    found = FRAME_WRITES.findall(log.read_text())
    return int(found[-1]) if found else None


def run_mode(arguments, payloads, directory: pathlib.Path, mode: str) -> tuple[bool, int | None]:
    """One boot that runs the probe in `mode`: whether the mode held, and the
    frames the emulator reported."""
    log = directory / f"{mode}.log"
    os.environ["UML_X86_ACCOUNTING_LOG"] = str(log)
    held = True
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
        try:
            result = guest.run_command(f"{GUEST_PROBE} {mode}", PROBE_TIMEOUT_SECONDS)
        except Exception as error:  # a guest that panicked takes its console with it
            print(f"FAIL {mode}: the guest did not finish the probe: {type(error).__name__}",
                  flush=True)
            held = False
        else:
            for line in result.output.splitlines():
                if line.startswith("page-cache-flush:"):
                    print(line, flush=True)
            match = VERDICT.search(result.output)
            if result.return_code != 0 or match is None or match.group(3) != "ok":
                print(f"FAIL {mode}: exit {result.return_code}", flush=True)
                held = False
            if mode == "marks":
                reported = {name: int(value) for name, value in MARK.findall(result.output)}
                for name, expected in EXPECTED_MARKS.items():
                    if reported.get(name) != expected:
                        print(f"FAIL marks: {name} is {reported.get(name)}, expected {expected}",
                              flush=True)
                        held = False
        console = guest.output()
        failures = sorted({line.strip() for line in console.splitlines()
                           if KERNEL_FAILURE.search(line)})
        if failures:
            held = False
            print(f"FAIL {mode}: the kernel reported", flush=True)
            for line in failures[:8]:
                print(f"  {line}", flush=True)
        guest.shutdown(60)
    return held, frame_writes(log)


def main() -> int:
    arguments = parse_arguments(
        __doc__, compiler_help="clang driver with an x86-64 Linux target and ld.lld",
        timeout_help="boot timeout")
    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    failures = []
    counts = {}
    with tempfile.TemporaryDirectory(prefix="uml-page-cache-flush-") as scratch:
        directory = pathlib.Path(scratch)
        for mode in (*RING_MODES, "marks", "data", "code"):
            held, counts[mode] = run_mode(arguments, payloads, directory, mode)
            if not held:
                failures.append(f"{mode}: the mode failed")
    print(f"frames named to the host: data={counts['data']} code={counts['code']}", flush=True)
    if counts["data"] is None or counts["code"] is None:
        failures.append("the emulator reported no count of frames named")
    else:
        if counts["data"] != 0:
            failures.append(f"files nobody runs were named to the host: {counts['data']}")
        if counts["code"] <= 0:
            failures.append("a file the process runs was never named to the host")
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        raise RuntimeError(f"{len(failures)} check(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
