#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the image's Vulkan hardware-buffer and memory-fd check in an Android guest.

The check is `kernarium_vk_ahb_import`
(android/lineage-24/overlay/tools/kernarium_vk_ahb_import.cpp), run as the shell
user over adb. It imports CPU-written hardware buffers into the product's Vulkan
HAL and reads them back, and it checks the driver's guest memory file
descriptors beside them: both fd extensions are offered and enabled on the same
device, and for each fd handle type host-visible memory is exported, mapped by
the Vulkan mapping, by the fd and by another process, read back by the GPU, and
imported again from a second fd. Three more cases per type and size hand the fd
to another instance of the tool, started with exec so that it holds nothing of
the exporting process but the fd: it imports, maps, reads and writes it while
the exporter is alive; it does so after the exporter freed its memory and
closed its fd; and a fd and its mapping outlive two destroyed devices.

The runner reads the tool's lines and fails when:

- the tool exits non-zero or its last line is not PASS, or the lines do not
  have the layout the tool documents (the tool's output and this parser change
  together; tests/tooling/android/test_vulkan_smoke_runner.py pins both);
- an fd extension is not offered, or an fd handle type has no passing
  properties case or lacks a passing case of any of the four sharing kinds
  (one process, another instance with a live exporter, another instance after
  the exporter was freed, a fd outliving both devices);
- no hardware-buffer case imported (the fd cases are checked beside them,
  not instead of them), or the lines do not put the hardware-buffer verdict
  line before the fd cases (a crash in the fd cases must leave the hardware
  verdict printed).

The guest is the release on a fresh data disk, started with the macOS launcher
(tools/boot_android.py), or any running guest named by --serial. Every wait has
a deadline and the launcher is stopped through its own process on any exit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run_android_guest_proc as guest_proc  # noqa: E402
from run_android_guest_proc import Failure  # noqa: E402

sys.path.insert(0, str(guest_proc.REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

# Outside a guest slot: a forward of its own, beside the other runners' (test_slot_port_defaults.py).
DEFAULT_ADB_PORT = 5587
TOOL = "kernarium_vk_ahb_import"
EXIT_MARKER = "kernarium-exit="
# The tool allocates a few dozen buffers and shares four memory objects; the bound is generous for an
# interpreted guest and a busy host.
TOOL_SECONDS = 1200
FD_EXTENSIONS = ("VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf")
FD_TYPES = ("opaque-fd", "dma-buf")
# The sharing cases of one type: what the end of the case's line says, and the name the problems use.
FD_SHARING = (
    ("bytes", "sharing"),
    ("bytes, imported by another instance", "another-instance"),
    ("bytes, imported after the exporter was freed", "freed-exporter"),
    ("bytes, kept past both devices", "outliving-devices"),
)

EXTENSION_LINE = re.compile(r"^memory-fd extension (VK_[A-Za-z0-9_]+): (offered|not offered)$")
FD_CASE_LINE = re.compile(
    r"^memory-fd (opaque-fd|dma-buf) (properties|\d+ bytes(?:, imported by another instance|"
    r", imported after the exporter was freed|, kept past both devices)?)$")
HARDWARE_VERDICT_LINE = re.compile(
    r"^hardware-buffer verdict: (PASS|FAIL): (\d+) cases imported, (\d+) failed, (\d+) not allocated$")
HARDWARE_LINE = re.compile(r"^(\S+) +(\d+)x(\d+) +stride (\d+) \((\d+) bytes\)$")
SKIP_LINE = re.compile(r"^(\S+) +(\d+)x(\d+) +SKIP: ")
VERDICT_LINE = re.compile(r"^  (PASS|FAIL)$")
NOTE_LINE = re.compile(r"^    \S")
FINAL_LINE = re.compile(
    r"^(PASS|FAIL): (\d+) cases imported, (\d+) failed, (\d+) not allocated; "
    r"memory fd (\d+) cases, (\d+) failed$")


@dataclasses.dataclass
class Case:
    kind: str  # "extensions", "fd" or "hardware"
    label: str
    verdict: str = ""
    notes: list[str] = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class Report:
    extensions: dict[str, bool] = dataclasses.field(default_factory=dict)
    cases: list[Case] = dataclasses.field(default_factory=list)
    skipped: int = 0
    hardware_verdict: tuple[str, int, int, int] | None = None
    final: tuple[str, int, int, int, int, int] | None = None
    exit_status: int | None = None

    def of_kind(self, kind: str) -> list[Case]:
        return [case for case in self.cases if case.kind == kind]


def parse(output: str) -> Report:
    """The tool's lines as a report. A line that is none of the documented ones is a failure."""
    report = Report()
    pending: Case | None = None
    fd_started = False
    for number, line in enumerate(output.splitlines(), 1):
        if not line.strip():
            continue
        if line.startswith(EXIT_MARKER):
            report.exit_status = int(line[len(EXIT_MARKER):])
            continue
        if report.final is not None:
            raise Failure(f"line {number} follows the last line: {line!r}")
        verdict = VERDICT_LINE.match(line)
        if verdict:
            if pending is None:
                raise Failure(f"line {number}: a verdict without a case: {line!r}")
            pending.verdict = verdict.group(1)
            pending = None
            continue
        if NOTE_LINE.match(line):
            if pending is not None:
                pending.notes.append(line.strip())
            elif report.cases:
                report.cases[-1].notes.append(line.strip())
            else:
                raise Failure(f"line {number}: a note before any case: {line!r}")
            continue
        if pending is not None and not EXTENSION_LINE.match(line):
            raise Failure(f"line {number}: {pending.label!r} has no verdict before {line!r}")
        extension = EXTENSION_LINE.match(line)
        if extension:
            fd_started = True
            if pending is None:
                pending = Case("extensions", "extensions")
                report.cases.append(pending)
            report.extensions[extension.group(1)] = extension.group(2) == "offered"
            continue
        fd_case = FD_CASE_LINE.match(line)
        if fd_case:
            fd_started = True
            pending = Case("fd", f"{fd_case.group(1)} {fd_case.group(2)}")
            report.cases.append(pending)
            continue
        hardware_verdict = HARDWARE_VERDICT_LINE.match(line)
        if hardware_verdict:
            if fd_started:
                raise Failure(f"line {number}: the hardware-buffer verdict follows the fd cases: {line!r}")
            if report.hardware_verdict is not None:
                raise Failure(f"line {number}: a second hardware-buffer verdict: {line!r}")
            report.hardware_verdict = (hardware_verdict.group(1), *(int(hardware_verdict.group(i)) for i in (2, 3, 4)))
            continue
        if SKIP_LINE.match(line):
            if fd_started or report.hardware_verdict is not None:
                raise Failure(f"line {number}: a hardware-buffer line after the hardware-buffer verdict: {line!r}")
            report.skipped += 1
            continue
        hardware = HARDWARE_LINE.match(line)
        if hardware:
            if fd_started or report.hardware_verdict is not None:
                raise Failure(f"line {number}: a hardware-buffer line after the hardware-buffer verdict: {line!r}")
            pending = Case("hardware", f"{hardware.group(1)} {hardware.group(2)}x{hardware.group(3)}")
            report.cases.append(pending)
            continue
        final = FINAL_LINE.match(line)
        if final:
            if pending is not None:
                raise Failure(f"line {number}: {pending.label!r} has no verdict before the last line")
            report.final = (final.group(1), *(int(final.group(index)) for index in range(2, 7)))
            continue
        raise Failure(f"line {number} is none of the tool's lines: {line!r}")
    if pending is not None:
        raise Failure(f"{pending.label!r} has no verdict at the end of the output")
    return report


def evaluate(report: Report) -> list[str]:
    """What is wrong with a report; nothing when it is a proof."""
    problems: list[str] = []
    if report.final is None:
        return ["the tool printed no last line (it did not finish)"]
    verdict, imported, failed, _not_allocated, fd_cases, fd_failed = report.final
    if report.exit_status is None:
        problems.append("the exit status of the tool was not read")
    elif (report.exit_status == 0) != (verdict == "PASS"):
        problems.append(f"the tool exited {report.exit_status} but its last line says {verdict}")
    if verdict != "PASS":
        problems.append(f"the last line says {verdict}")
    for case in report.cases:
        if case.verdict != "PASS":
            detail = f": {'; '.join(case.notes)}" if case.notes else ""
            problems.append(f"{case.kind} {case.label} {case.verdict}{detail}")
    for name in FD_EXTENSIONS:
        if not report.extensions.get(name):
            problems.append(f"{name} is not offered")
    fd = report.of_kind("fd")
    if len(fd) + len(report.of_kind("extensions")) != fd_cases:
        problems.append(f"the last line counts {fd_cases} memory-fd cases, the lines hold "
                        f"{len(fd) + len(report.of_kind('extensions'))}")
    if sum(1 for case in fd if case.verdict != "PASS") + sum(
            1 for case in report.of_kind("extensions") if case.verdict != "PASS") != fd_failed:
        problems.append("the failed memory-fd cases do not match the last line")
    for fd_type in FD_TYPES:
        cases = [case.label.split(" ", 1)[1] for case in fd if case.label.startswith(fd_type + " ") and
                 case.verdict == "PASS"]
        if "properties" not in cases:
            problems.append(f"{fd_type} has no passing properties case")
        for ending, name in FD_SHARING:
            # A plain sharing case ends in "bytes" and no variant does; the others end in their own text.
            if not any(label.endswith(ending) and (name != "sharing" or "," not in label) for label in cases):
                problems.append(f"{fd_type} has no passing {name} case")
    hardware = report.of_kind("hardware")
    if len(hardware) != imported or sum(1 for case in hardware if case.verdict != "PASS") != failed:
        problems.append("the hardware-buffer cases do not match the last line")
    if report.hardware_verdict is None:
        problems.append("the tool printed no hardware-buffer verdict line before the fd cases")
    elif report.hardware_verdict != (("PASS" if not failed else "FAIL"), imported, failed, _not_allocated):
        problems.append("the hardware-buffer verdict line does not match the last line")
    if imported < 1:
        problems.append("no hardware-buffer case imported")
    return problems


def summary(report: Report) -> str:
    lines = [f"{name}: {'offered' if offered else 'not offered'}" for name, offered in report.extensions.items()]
    for case in report.cases:
        if case.kind == "fd":
            lines.append(f"memory-fd {case.label}: {case.verdict}")
    hardware = report.of_kind("hardware")
    if report.hardware_verdict is not None:
        lines.insert(0, "hardware-buffer verdict line: {}: {} imported, {} failed, {} not allocated".format(
            *report.hardware_verdict))
    lines.append(f"hardware buffers: {sum(1 for case in hardware if case.verdict == 'PASS')} of "
                 f"{len(hardware)} cases passed, {report.skipped} not allocated")
    return "\n".join(lines)


def run_tool(device: guest_proc.Device) -> Report:
    result = device.run("-s", device.serial, "shell", f"{TOOL}; echo {EXIT_MARKER}$?", timeout=TOOL_SECONDS)
    print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr.strip():
        print("stderr:\n" + result.stderr.strip(), file=sys.stderr)
    return parse(result.stdout)


def check(device: guest_proc.Device) -> None:
    print(f"guest: {device.shell('getprop ro.build.fingerprint')}")
    report = run_tool(device)
    print("-- report --\n" + summary(report))
    problems = evaluate(report)
    if problems:
        raise Failure("; ".join(problems))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--serial", help="a running guest (host:port); no guest is launched")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int,
                        default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port)")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    return parser


def main() -> int:
    options = build_parser().parse_args()
    adb = shutil.which(options.adb) or options.adb
    guest: subprocess.Popen | None = None
    for name in guest_proc.TERMINATION_SIGNALS:
        signal.signal(name, guest_proc.terminate_on_signal)
    with tempfile.TemporaryDirectory(prefix="android-vulkan-smoke-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        serial = options.serial or f"127.0.0.1:{options.adb_port}"
        log_path = scratch / "launcher.log"
        try:
            if not options.serial:
                launch = [sys.executable, str(guest_proc.LAUNCHER), "--instance", str(scratch / "instance"),
                          "--adb", adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                          "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
                if options.release:
                    launch += ["--release", str(options.release)]
                if options.emulator:
                    launch += ["--emulator", str(options.emulator)]
                with open(log_path, "wb") as log:
                    guest = subprocess.Popen(launch, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                             env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            device = guest_proc.Device(adb, serial)
            guest_proc.wait_booted(device, options.boot_timeout, guest)
            check(device)
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem!r}", file=sys.stderr)
            if log_path.is_file():
                print("launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                      file=sys.stderr)
            return 1
        finally:
            for name in guest_proc.TERMINATION_SIGNALS:
                signal.signal(name, signal.SIG_IGN)
            if guest is not None:
                guest_proc.stop_launcher(guest)
                subprocess.run([adb, "disconnect", serial], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
