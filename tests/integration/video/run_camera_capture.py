#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the Virtio camera's guest probe against the synthetic camera.

A live session boots twice. The host learns a camera's colour data from the
first Bayer still it takes and offers Bayer stills only from the next session
on, so the first boot takes that still into a calibration directory of this
run, and the second boot, which sees the cameras with their Bayer stills, runs
the whole probe. A blocked session boots once. The synthetic camera is never a
real one.
"""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import subprocess
import sys
import tempfile

PROBE = pathlib.Path(__file__).resolve().parent / "camera_capture_probe.cpp"
RUNNER = pathlib.Path(__file__).resolve().parents[1] / "support" / "run_guest_probe.py"
# One boot compiles the probe and runs it; both take about a minute.
PROBE_TIMEOUT_SECONDS = 420
BUILD_TIMEOUT_SECONDS = 300
CONSOLE_FAILURE = "WARNING:|Oops|BUG:|Call Trace:"


def boot(arguments: argparse.Namespace, mode: str, probe_mode: str, calibration: str | None) -> None:
    command = [
        sys.executable,
        "-B",
        str(RUNNER),
        "--emulator", str(arguments.emulator),
        "--rootfs", str(arguments.rootfs),
        "--probe", str(PROBE),
        "--cxxflags=-Wall -Wextra -Werror",
        f"--guest-argument=--camera={mode}",
        "--guest-argument=--camera-profile=tablet",
        f"--probe-argument={probe_mode}",
        "--console-failure", CONSOLE_FAILURE,
        "--engine", arguments.engine,
        "--ncpus", "2",
        "--probe-timeout", str(PROBE_TIMEOUT_SECONDS),
        "--build-timeout", str(arguments.build_timeout),
    ]
    if calibration is not None:
        command.append(f"--guest-argument=--camera-calibration-dir={calibration}")
    print(f"camera capture: {probe_mode} boot", flush=True)
    subprocess.run(command, stdin=subprocess.DEVNULL, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--mode", required=True, choices=("synthetic", "blocked"))
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--build-timeout", type=int, default=BUILD_TIMEOUT_SECONDS, metavar="SECONDS",
                        help="how long the guest has to build the probe")
    arguments = parser.parse_args()
    if arguments.mode == "blocked":
        boot(arguments, "blocked", "blocked", None)
        return 0
    with tempfile.TemporaryDirectory(prefix="uml-camera-calibration-") as calibration:
        boot(arguments, "synthetic", "calibrate", calibration)
        boot(arguments, "synthetic", "synthetic", calibration)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
