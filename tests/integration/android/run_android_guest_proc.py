#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the shell command that Andarium's bug report sends to the guest, in an Android guest.

The command is the one the application sends, taken from its single source
(Andarium/AndariumCore/Sources/AndariumCore/GuestProcFiles.swift, between the
`guest-proc-command` markers), and is run the way the application's adb
channel runs it: as the shell user, through the shell service, with no tty.
The guest is the release on a fresh data disk, started with the macOS launcher
(tools/boot_android.py), or any running guest named by --serial.

Checked, with every file reported on its own line:

- the shell accepts the command: nothing it prints is a shell error (an alias
  or a function name the guest's shell defines, a syntax error);
- every file the command names has its `==> path <==` line and exactly one
  outcome after its content: read as the shell user, read as root through
  `su 0`, or not readable with the reason for both ways;
- /proc/interrupts and /proc/stat arrive with content (the shell user reads
  both on every build); the other files arrive with content or, where the
  guest's permissions or SELinux policy refuse the shell user and the build
  has no `su` (a user build), say why in words, for both ways of reading;
- a file's content is no longer than its limit;
- the command ends within the time the bug report allows it.

Every wait has a deadline, the guest's launcher is stopped through its own
process on any exit, and the guest is reached through the shared adb server,
always named with `-s`.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
SOURCE = (REPOSITORY / "Andarium" / "AndariumCore" / "Sources" / "AndariumCore" / "GuestProcFiles.swift")
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

DEFAULT_ADB_PORT = 5575
# GuestProcFiles.commandLimit: the longest the bug report waits for the command.
COMMAND_SECONDS = 5
# Files the shell user reads on every build. /proc/timer_list is root's alone and the pressure files
# belong to SELinux types the shell domain may not read on a user build.
REQUIRED = ("/proc/interrupts", "/proc/stat")
READ_AS_SHELL = "(read as the shell user)"
READ_AS_ROOT = "(read as root through su)"
NOT_READABLE = "(not readable: "
SHELL_ERRORS = ("syntax error", "not found", "unexpected", "bad substitution", "inaccessible or not found")
TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)


class Failure(RuntimeError):
    pass


def command_from_source(source: pathlib.Path = SOURCE) -> str:
    """The literal between the markers: the text the application sends."""
    lines = source.read_text(encoding="utf-8").split("\n")
    begin = next(index for index, line in enumerate(lines) if "// BEGIN guest-proc-command" in line)
    end = next(index for index, line in enumerate(lines) if "// END guest-proc-command" in line)
    if '#"""' not in lines[begin + 1] or lines[end - 1] != '"""#':
        raise Failure("GuestProcFiles.swift: the command literal is not between its markers")
    return "\n".join(lines[begin + 2:end - 1])


def sources_of(command: str) -> list[tuple[str, int]]:
    """The (path, limit) pairs on the command's first line."""
    first = command.split("\n", 1)[0]
    match = re.search(r"for entry in (.*?); do", first)
    if not match:
        raise Failure("the command's first line does not list its files")
    return [(path, int(limit)) for path, limit in (word.rsplit(":", 1) for word in match.group(1).split())]


def parse_sections(output: str, sources: list[tuple[str, int]]) -> dict[str, tuple[str, str]]:
    """path -> (content, outcome line) for every section of the output."""
    sections: dict[str, tuple[str, str]] = {}
    parts = re.split(r"^==> (.*) <==\n", output, flags=re.MULTILINE)
    # parts: [before, path, body, path, body, ...]
    for path, body in zip(parts[1::2], parts[2::2]):
        for marker in (READ_AS_SHELL, READ_AS_ROOT, NOT_READABLE):
            at = body.rfind(marker)
            if at >= 0:
                sections[path] = (body[:at], body[at:].rstrip("\n"))
                break
        else:
            raise Failure(f"{path}: the section ends in no outcome line")
    return sections


class Device:
    def __init__(self, adb: str, serial: str) -> None:
        self.adb = adb
        self.serial = serial

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], text=True, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=timeout, check=False)

    def connect(self) -> bool:
        try:
            self.run("connect", self.serial, timeout=15)
            state = self.run("-s", self.serial, "get-state", timeout=15)
        except subprocess.TimeoutExpired:
            return False
        return state.returncode == 0 and state.stdout.strip() == "device"

    def shell(self, command: str, timeout: int = 30) -> str:
        result = self.run("-s", self.serial, "shell", command, timeout=timeout)
        return result.stdout.strip()


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if guest is not None and guest.poll() is not None:
            raise Failure(f"the guest's launcher exited ({guest.returncode}) before the guest booted")
        try:
            if device.connect() and device.shell("getprop sys.boot_completed") == "1":
                return
        except subprocess.TimeoutExpired:
            pass
        time.sleep(2)
    raise Failure(f"the guest was not booted within {seconds} s")


def check(device: Device, command: str) -> None:
    sources = sources_of(command)
    print(f"guest: {device.shell('getprop ro.build.fingerprint')}, uid {device.shell('id -u')}, "
          f"shell {device.shell('echo $0')}")
    # What a reader needs to know about the guest's shell: the alias that made the first version fail.
    print(f"aliases named r in the guest's shell: {device.run('-s', device.serial, 'shell', 'alias r').stdout.strip()!r}")
    has_pressure = device.shell("[ -d /proc/pressure ] && echo yes || echo no") == "yes"
    print(f"/proc/pressure exists: {has_pressure}")

    started = time.monotonic()
    result = device.run("-s", device.serial, "shell", command, timeout=60)
    elapsed = time.monotonic() - started
    print(f"command: exit {result.returncode} in {elapsed:.2f} s, {len(result.stdout)} bytes on stdout, "
          f"{len(result.stderr)} on stderr")
    if result.returncode != 0:
        raise Failure(f"the command exited {result.returncode}: {result.stderr.strip()[:300]}")
    if elapsed > COMMAND_SECONDS:
        raise Failure(f"the command took {elapsed:.1f} s, more than the {COMMAND_SECONDS} s the bug report allows")
    for line in result.stderr.splitlines():
        if any(word in line for word in SHELL_ERRORS):
            raise Failure(f"the shell printed an error: {line}")

    sections = parse_sections(result.stdout, sources)
    problems: list[str] = []
    for path, limit in sources:
        if path not in sections:
            problems.append(f"{path}: no section")
            continue
        content, outcome = sections[path]
        if len(content.encode("utf-8", "replace")) > limit:
            problems.append(f"{path}: {len(content)} bytes, over its limit of {limit}")
        how = {READ_AS_SHELL: "read as the shell user", READ_AS_ROOT: "read as root through su"}.get(
            outcome, outcome)
        print(f"{path}: {how}, {len(content)} bytes of content")
        if outcome.startswith(NOT_READABLE):
            if path in REQUIRED:
                problems.append(f"{path}: {outcome}")
            reasons = outcome[len(NOT_READABLE):].rstrip(")").split("; through su: ")
            if len(reasons) != 2 or not all(reason.strip() for reason in reasons):
                problems.append(f"{path}: the entry does not say why for both ways of reading: {outcome!r}")
            if path.startswith("/proc/pressure/") and not has_pressure and "No such file" not in outcome:
                problems.append(f"{path}: the guest has no /proc/pressure, but the entry says {outcome!r}")
        elif not content.strip():
            problems.append(f"{path}: read, but empty")
    if problems:
        raise Failure("; ".join(problems))


def stop_launcher(guest: subprocess.Popen) -> None:
    if guest.poll() is None:
        guest.send_signal(signal.SIGTERM)
        try:
            guest.wait(timeout=120)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait(timeout=30)


def terminate_on_signal(signum: int, _frame: object) -> None:
    for name in TERMINATION_SIGNALS:
        signal.signal(name, signal.SIG_IGN)
    raise SystemExit(128 + signum)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--serial", help="a running guest (host:port); no guest is launched")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port)")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--print-command", action="store_true", help="print the command and exit")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    command = command_from_source()
    if options.print_command:
        print(command)
        return 0

    adb = shutil.which(options.adb) or options.adb
    guest: subprocess.Popen | None = None
    for name in TERMINATION_SIGNALS:
        signal.signal(name, terminate_on_signal)
    with tempfile.TemporaryDirectory(prefix="android-guest-proc-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        serial = options.serial or f"127.0.0.1:{options.adb_port}"
        log_path = scratch / "launcher.log"
        try:
            if not options.serial:
                launch = [sys.executable, str(LAUNCHER), "--instance", str(scratch / "instance"),
                          "--adb", adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                          "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
                if options.release:
                    launch += ["--release", str(options.release)]
                if options.emulator:
                    launch += ["--emulator", str(options.emulator)]
                with open(log_path, "wb") as log:
                    guest = subprocess.Popen(launch, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                             env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            device = Device(adb, serial)
            wait_booted(device, options.boot_timeout, guest)
            check(device, command)
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError, StopIteration) as problem:
            print(f"FAIL: {problem!r}", file=sys.stderr)
            if log_path.is_file():
                print("launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                      file=sys.stderr)
            return 1
        finally:
            for name in TERMINATION_SIGNALS:
                signal.signal(name, signal.SIG_IGN)
            if guest is not None:
                stop_launcher(guest)
                subprocess.run([adb, "disconnect", serial], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
