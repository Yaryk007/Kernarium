#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The interruption runner's reading of a simulator device's processes.

A freshly booted device runs processes whose arguments span several lines, and
`ps` prints the later lines as rows of their own text. The runner sums the
device's CPU use over the rows that are processes and ignores the rest.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

UDID = "0F4C0A5B-1111-2222-3333-444455556666"
LISTING = f"""\
  100     1   0.0 /usr/libexec/launchd_sim /sim/{UDID}/data/var/run/launchd_sim_server
  200   100  12.5 /usr/bin/app --flag
  300   100   7.5 /usr/bin/helper first line
of its multi-line argument
  and a second continuation line
  400   300   5.0 /usr/bin/leaf
  900     1  50.0 /usr/bin/unrelated
"""


def fail(message: str) -> None:
    sys.exit(f"test_run_ios_interruption.py: {message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path, required=True)
    runner = parser.parse_args().runner
    spec = importlib.util.spec_from_file_location("run_ios_interruption_under_test", runner)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def listing(stdout: str):
        return lambda *args, **kwargs: subprocess.CompletedProcess(args, 0, stdout, "")

    module.subprocess.run = listing(LISTING)
    try:
        total = module.device_cpu(UDID)
    except ValueError as error:
        fail(f"a continuation line of a process's arguments was read as a process: {error}")
    if total != 25.0:
        fail(f"the device's processes use 25.0 percent, not {total!r}")

    module.subprocess.run = listing(LISTING.replace("launchd_sim", "launchd"))
    if module.device_cpu(UDID) is not None:
        fail("a listing without the device's launchd_sim has a device")
    return 0


if __name__ == "__main__":
    sys.exit(main())
