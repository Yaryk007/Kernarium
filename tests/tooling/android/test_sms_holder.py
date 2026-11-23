#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The text message holder on the host, without a guest.

android/lineage-24/overlay/sms/listener/main.cpp is the native daemon that keeps vsock port 5560 from boot, starts
the service through a control property when a connection waits, and passes it the listening socket over its own
unix socket. This test compiles that file as it is (main renamed, -Wall -Wextra -Werror, C++20, as the image
build does) against the stand-ins in tests/tooling/android/sms/holder for the bionic pieces macOS lacks, links it
with holder_test.cpp, and runs the scenarios there: the handoff socket as init makes it (bound, NOT listening),
the vsock port as a unix socket at a path of the test, the property set, the peer's user and domain. What it
holds that no review catches: the holder must listen on the handoff socket itself, and stays in its loop.
Whether the holder's idle window is the service's (a session that ends with a connection waiting before it lasted a
window is an early end) is read from both sources and judged first, on every host. The scenarios are skipped
(status 77) where the host has no C++ compiler.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
HOLDER = REPOSITORY / "android" / "lineage-24" / "overlay" / "sms" / "listener" / "main.cpp"
SERVICE = (REPOSITORY / "android" / "lineage-24" / "overlay" / "sms" / "src" / "android" / "kernarium" / "sms"
           / "HostService.java")
SUPPORT = HERE / "sms" / "holder"
SKIP = 77
SCENARIOS = ("S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10", "S11", "S12")


def idle_window_problem() -> str | None:
    """The holder tells an early end of a session from a full one by the service's idle window: the two are one
    number, or the holder resets its back-off after sessions that did not last, or keeps it after ones that did."""
    holder = re.search(r"constexpr int64_t kIdleWindowMillis = (\d+);", HOLDER.read_text())
    service = re.search(r"static final long IDLE_MILLIS = ([\d_]+);", SERVICE.read_text())
    if holder is None or service is None:
        return "the idle window is not where this test reads it: kIdleWindowMillis in the holder, IDLE_MILLIS in the service"
    if int(holder.group(1)) != int(service.group(1).replace("_", "")):
        return (f"the holder's idle window is {holder.group(1)} ms and the service's is "
                f"{service.group(1).replace('_', '')} ms: they must be one number")
    return None


def compiler() -> str | None:
    path = shutil.which("clang++")
    if path is None:
        return None
    probe = subprocess.run([path, "--version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def run(command: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, timeout=timeout, check=False)


def main() -> int:
    problem = idle_window_problem()
    if problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    clang = compiler()
    if clang is None:
        print("SKIP: no C++ compiler (clang++) on PATH")
        return SKIP
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-include", str(SUPPORT / "prelude.h"),
             f"-I{SUPPORT / 'stubs'}"]
    with tempfile.TemporaryDirectory(prefix="sms-holder-build-") as build:
        holder = pathlib.Path(build) / "holder.o"
        driver = pathlib.Path(build) / "driver.o"
        binary = pathlib.Path(build) / "holder_test"
        for command in ([clang, *flags, "-Dmain=holder_main", "-c", str(HOLDER), "-o", str(holder)],
                        [clang, *flags, "-c", str(SUPPORT / "holder_test.cpp"), "-o", str(driver)],
                        [clang, str(holder), str(driver), "-o", str(binary), "-pthread"]):
            built = run(command, 300)
            if built.returncode != 0:
                print(f"FAIL: {' '.join(command[:2])} ...\n{built.stdout}", file=sys.stderr)
                return 1
        # A short path: a unix socket's path is at most 104 bytes on macOS.
        with tempfile.TemporaryDirectory(prefix="sms-h-", dir="/tmp") as scratch:
            ran = run([str(binary), scratch], 120)
    for line in ran.stdout.splitlines():
        if line.startswith(("PASS", "FAIL")):
            print(line)
    if ran.returncode != 0:
        print(f"FAIL: the holder's scenarios exited {ran.returncode}:\n{ran.stdout[-3000:]}", file=sys.stderr)
        return 1
    missing = [name for name in SCENARIOS if f"PASS {name}:" not in ran.stdout]
    if missing or not ran.stdout.rstrip().endswith("PASS"):
        print(f"FAIL: scenarios not passed: {missing}\n{ran.stdout[-3000:]}", file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
