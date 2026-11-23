#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the launcher's restart-loop guard on an Android guest: boots that never finish end the
loop, and restarts after finished boots never do.

The macOS launcher boots a guest again each time the guest restarts itself. It ends the launch
(exit status 3) after five restarts in a row within 15 s of their boot, or eight in a row that
were early: within that window or, for a guest whose console says when its boot ends, before the
boot had finished (darwin/emulator/src/restart_loop_guard.hpp). The runner shows both halves on the
release under test:

- the late loop: `adb reboot` 16 s after each kernel start, which is after the launcher's 15 s
  window and before Android's `sys.boot_completed`, so only the unfinished boot makes the restart
  early. Before each reboot the runner asks Android itself whether the property is still unset,
  independently of the console lines under test. The launcher boots the guest seven times again,
  then ends the launch with status 3 and says the guest restarted eight times in a row before it
  finished booting;
- the control: `adb reboot` after each boot has finished (CTS and tradefed suites reboot this way),
  eight boots in a row with seven restarts. The launcher counts none of them, and the guest
  powers off cleanly afterwards.

A boot that finishes before the late loop restarts it proves nothing about the guard, so it fails
the runner with that reason. Every wait has a deadline. The guest is reached through the shared adb
server, always named with `-s`, and stopped through its own process on any exit.

The runner is opt-in: it needs the macos-android17 executable, an installed release, and an adb
client.
usage: run_android_restart_loop.py --emulator uml-darwin-android17 [--release DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_android = load_module("boot_android", REPOSITORY / "tools" / "boot_android.py")

DEFAULT_ADB_PORT = 5584
# The launcher's window for an early restart, and the early restarts in a row that end the launch
# when the boots did not finish.
WINDOW_SECONDS = 15
LIMIT = 8
GUARD_STATUS = 3
# The late loop restarts the guest this long after each kernel start by default: past the window,
# before the boot finishes (a warm boot of the release finishes after about 20 s).
RESTART_AFTER_SECONDS = 16
LOOP_SECONDS = 900
BOOT_SECONDS = 900
POWER_OFF_SECONDS = 180
CONTROL_BOOTS = 8
POLL_SECONDS = 1.0
KERNEL_LINE = re.compile(r"Linux version \S+")
COMPLETED_LINE = "processing action (sys.boot_completed=1)"
RESTARTING_LINE = re.compile(r"^guest restarting \(command ", re.M)


class Failure(RuntimeError):
    pass


@dataclass
class Launch:
    adb: str
    serial: str
    process: subprocess.Popen
    log: pathlib.Path

    def text(self) -> str:
        return self.log.read_text(errors="replace")

    def kernel_starts(self) -> int:
        return len(KERNEL_LINE.findall(self.text()))

    def segments(self) -> list[str]:
        """The console of each boot so far: what follows each kernel start up to the next."""
        return KERNEL_LINE.split(self.text())[1:]

    def console_completions(self) -> list[bool]:
        """For each boot so far, whether its console shows Android's boot completed (init processes
        the action once for each rc file that has one, so a boot's count is not one)."""
        return [COMPLETED_LINE in segment for segment in self.segments()]

    def boot_property(self) -> str | None:
        """What Android says of the running boot, from sys.boot_completed ("1" once it finished, ""
        before); None when adb could not be asked."""
        subprocess.run([self.adb, "connect", self.serial], stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)
        try:
            result = self.adb_run("shell", "getprop", "sys.boot_completed", timeout=30)
        except subprocess.TimeoutExpired:
            return None
        if result.returncode != 0:
            return None
        return result.stdout.decode(errors="replace").strip()

    def boot_completed(self) -> bool:
        """Whether Android says the running boot finished."""
        return self.boot_property() == "1"

    def adb_run(self, *arguments: str, timeout: float = 60) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False, timeout=timeout)

    def reboot(self) -> bool:
        """Asks Android to restart; whether adb took the command."""
        subprocess.run([self.adb, "connect", self.serial], stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)
        try:
            return self.adb_run("reboot", timeout=30).returncode == 0
        except subprocess.TimeoutExpired:
            return False


def launch(options: argparse.Namespace, adb: str, private: pathlib.Path, name: str) -> Launch:
    boot_options = boot_android.parse_arguments([
        *(["--release", str(options.release)] if options.release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(private / name),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        "--display", "offscreen",
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    instance = private / name
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release_directory = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release_directory)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = boot_android.emulator_command(
        boot_options, release_directory, instance / boot_android.android_instance.DATA_DISK, emulator,
        boot_android.data_capacity(boot_options, identity))
    log = private / f"launcher-{name}.log"
    stream = log.open("wb")
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                               start_new_session=True,
                               env={**os.environ, "TMPDIR": str(private), "PYTHONDONTWRITEBYTECODE": "1"})
    stream.close()
    return Launch(adb, f"127.0.0.1:{options.adb_port}", process, log)


def stop(guest: Launch) -> None:
    boot_android.disconnect_adb(guest.adb, guest.serial)
    boot_android.stop_guest(guest.process)


def wait_until(what: str, seconds: float, ready, guest: Launch) -> None:
    """Polls `ready()` until it holds; a launcher that ended meanwhile is a failure."""
    deadline = time.monotonic() + seconds
    while True:
        if ready():
            return
        if guest.process.poll() is not None:
            raise Failure(f"the launcher ended with status {guest.process.returncode} while waiting for {what}")
        if time.monotonic() >= deadline:
            raise Failure(f"{what} did not happen within {seconds:.0f} s")
        time.sleep(POLL_SECONDS)


def late_loop(options: argparse.Namespace, adb: str, private: pathlib.Path) -> None:
    print("late loop: restart 16 s after each kernel start, before the boot finishes")
    guest = launch(options, adb, private, "late")
    try:
        seen = 0
        kernel_at: float | None = None
        restarted = 0
        unfinished_when_restarted = 0
        deadline = time.monotonic() + LOOP_SECONDS
        while guest.process.poll() is None:
            if time.monotonic() >= deadline:
                raise Failure(f"the launcher was still booting the guest after {LOOP_SECONDS} s: "
                              f"{restarted} restarts asked for")
            starts = guest.kernel_starts()
            if starts > seen:
                seen = starts
                kernel_at = time.monotonic()
            if kernel_at is not None and restarted < seen and time.monotonic() - kernel_at >= options.restart_after:
                # Android's own answer, not the console line under test: the boot must be unfinished
                # when it is restarted, or the run says nothing of the guard.
                state = guest.boot_property()
                if state == "1":
                    raise Failure(f"boot {seen} finished (sys.boot_completed=1) before the runner restarted "
                                  f"it at {options.restart_after} s, so the run says nothing of the guard; "
                                  "lower --restart-after")
                if state is not None and guest.reboot():
                    restarted += 1
                    unfinished_when_restarted += 1
                    print(f"     boot {seen}: sys.boot_completed unset, adb reboot at "
                          f"{time.monotonic() - kernel_at:.0f} s")
            time.sleep(POLL_SECONDS)
        status = guest.process.returncode
        text = guest.text()
        if status != GUARD_STATUS:
            raise Failure(f"the launcher ended with status {status}, not {GUARD_STATUS}:\n{text[-1500:]}")
        again = len(RESTARTING_LINE.findall(text))
        if again != LIMIT - 1:
            raise Failure(f"the guest was booted again {again} times, not {LIMIT - 1}")
        if f"restarted {LIMIT} times in a row, each before it finished booting" not in text:
            raise Failure(f"the launcher did not say why it stopped:\n{text[-1500:]}")
        if unfinished_when_restarted < LIMIT - 1:
            raise Failure(f"only {unfinished_when_restarted} of the restarts were asked for by the runner "
                          "with the boot's property unset: the rest came from the guest")
        print(f"ok   the guest was booted {again + 1} times, each unfinished by Android's own word, "
              f"then the launcher ended with status {status}")
    finally:
        stop(guest)


def control(options: argparse.Namespace, adb: str, private: pathlib.Path) -> None:
    print(f"control: {CONTROL_BOOTS - 1} restarts after finished boots")
    guest = launch(options, adb, private, "control")
    try:
        boot_android.wait_for_adb(guest.adb, guest.serial, guest.process, options.boot_timeout)
        kernel_at = time.monotonic()
        for boot in range(1, CONTROL_BOOTS + 1):
            # Android says so itself, not the console the launcher reads, so a signal the launcher
            # missed cannot hide behind the runner's own use of it.
            wait_until(f"boot {boot} to finish", BOOT_SECONDS,
                       lambda boot=boot: guest.kernel_starts() >= boot and guest.boot_completed(), guest)
            if boot == CONTROL_BOOTS:
                break
            # A suite restarts a booted guest at its leisure; a restart within the launcher's window
            # would count as early whatever the console said.
            late = kernel_at + WINDOW_SECONDS + 2
            if time.monotonic() < late:
                time.sleep(late - time.monotonic())
            if not guest.reboot():
                raise Failure(f"adb did not take the reboot after boot {boot}")
            wait_until(f"boot {boot + 1} to start", BOOT_SECONDS, lambda boot=boot: guest.kernel_starts() > boot, guest)
            kernel_at = time.monotonic()
        text = guest.text()
        again = len(RESTARTING_LINE.findall(text))
        if again != CONTROL_BOOTS - 1:
            raise Failure(f"the guest was booted again {again} times, not {CONTROL_BOOTS - 1}")
        if "not booting it again" in text or guest.process.poll() is not None:
            raise Failure("the launcher stopped booting a guest whose boots all finished")
        # The console line follows the property by a moment.
        wait_until("every boot's console to show it finished", 30, lambda: all(guest.console_completions()), guest)
        print(f"ok   {again} restarts after finished boots, none counted")
        guest.adb_run("shell", "reboot -p", timeout=60)
        deadline = time.monotonic() + POWER_OFF_SECONDS
        while guest.process.poll() is None:
            if time.monotonic() >= deadline:
                raise Failure(f"the guest did not power off within {POWER_OFF_SECONDS} s")
            time.sleep(POLL_SECONDS)
        if guest.process.returncode != 0:
            raise Failure(f"the power-off ended the launcher with status {guest.process.returncode}")
        print("ok   the guest powered off with status 0")
    finally:
        stop(guest)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release under test (default: accepted)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=BOOT_SECONDS,
                        help="seconds to wait for adbd")
    parser.add_argument("--log-dir", type=pathlib.Path,
                        help="directory to copy the launchers' logs to before the run's scratch is removed")
    parser.add_argument("--restart-after", type=int, default=RESTART_AFTER_SECONDS,
                        help="seconds after each kernel start at which the late loop restarts the guest")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0 or options.restart_after <= WINDOW_SECONDS:
        print("counts and timeouts must be positive, and the restart must come after the launcher's window",
              file=sys.stderr)
        return 2
    adb = str(boot_android.resolve_executable(options.adb, sdk_fallback=True))
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-arl-"))
    failures: list[str] = []
    try:
        for name, section in (("late loop", late_loop), ("control", control)):
            try:
                section(options, adb, private)
            except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
                print(f"FAIL {name}: {problem}", file=sys.stderr)
                failures.append(f"{name}: {problem}")
    finally:
        if options.log_dir is not None:
            options.log_dir.mkdir(parents=True, exist_ok=True)
            for log in private.glob("launcher-*.log"):
                shutil.copy(log, options.log_dir / log.name)
        shutil.rmtree(private, ignore_errors=True)
    if failures:
        print("FAIL: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
