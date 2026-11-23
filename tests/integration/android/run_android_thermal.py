#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's thermal HAL and the 30 fps cap it brings.

The guest is the release on a fresh data disk (or on the instance named by --instance, for example an
older release's data disk), started with the macOS launcher and ``--vsock-forward=127.0.0.1:0-5559``, so
that the runner plays the host application: it sends the HAL's records over the forward. The contract is
android/lineage-24/README.md ("Thermal"), and the wire is overlay/thermal/thermal_protocol.hpp:

- before any host connects: the thermal manager is connected to a HAL of AIDL version 3, the one SKIN
  sensor reads 31.0 degrees with status NONE, its seven hot thresholds are NaN, 34, 37, 40, 43, 46 and 49,
  the headroom thresholds are NaN, 0.8, 0.9, 1.0, 1.1, 1.2 and 1.3, there is no battery sensor, and the
  display's skin observer holds the MODERATE point of 60 Hz and the SEVERE point of 30 Hz;
- CRITICAL sent from NONE (the application sends it when iOS jumps straight to critical, and NONE when
  the hold after critical ends with nothing hotter) sets the status to 4 and brings the same cap as
  SEVERE, and NONE sent straight after it lifts the cap;
- LIGHT sets the status to 1 and casts no vote on the frame rate;
- MODERATE sets the status to 2 and holds the render rate to 60 Hz (the vote the application relies on when
  Low Power Mode holds a 120 Hz display to 60), which changes no frame rate of this runner's 60 Hz display;
- SEVERE sets the status to 3, the display manager holds a render-rate vote of 0 to 30 Hz at the skin
  temperature priority, SurfaceFlinger renders at 30.00 Hz on the unchanged mode, and
  ``kernarium_vk_present`` and the host's presented frames run at about 30 a second;
- CRITICAL sets the status to 4 and keeps the same cap;
- records that name EMERGENCY or SHUTDOWN, a wrong magic or a reserved byte close the connection and leave
  the status where it was; the guest stays up;
- a host that drops keeps the severity it last sent;
- NONE lifts the cap: the vote is gone, SurfaceFlinger renders at the display's rate and both frame rates
  are back to about 58 a second.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the
guest is reached through the shared adb server, always named with ``-s``.

The runner is opt-in: it needs the macos-android17 executable, an installed release, and an adb client.
usage: run_android_thermal.py --emulator uml-darwin-android17 [--release DIR] [--instance DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import math
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from dataclasses import dataclass

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

SPEC = importlib.util.spec_from_file_location("boot_android", REPOSITORY / "tools" / "boot_android.py")
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)

DEFAULT_ADB_PORT = 5578
THERMAL_PORT = 5559
FORWARD = re.compile(r"vsock forward (\S+):(\d+) -> guest port (\d+)")
FORWARD_SECONDS = 60
COMMAND_SECONDS = 120
SETTLE_SECONDS = 90
STATE_SECONDS = 45
CLOSE_SECONDS = 15
# A measurement must last two statistics windows (FRAME_STATS_SECONDS) for one of them to lie wholly inside
# it: 300 frames are 10 s at the capped 30 a second, and twice as many are 10 s at the uncapped 58.
PRESENT_FRAMES = 300
UNCAPPED_PRESENT_FRAMES = 600
PRESENT_SECONDS = 300
FRAME_STATS_SECONDS = 5
MAGIC = b"ATH1"

HOT_THRESHOLDS = (math.nan, 34.0, 37.0, 40.0, 43.0, 46.0, 49.0)
HEADROOM_THRESHOLDS = (math.nan, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3)
# Severity, the skin temperature the HAL reports for it, and the headroom at that temperature.
LEVELS = {
    0: (31.0, 0.70),
    1: (35.5, 0.85),
    2: (38.5, 0.95),
    3: (41.5, 1.05),
    4: (44.5, 1.15),
}
CAPPED_FPS = (24.0, 34.0)
UNCAPPED_FPS_MINIMUM = 45.0
CAPPED_RENDER_RATE = (29.5, 30.5)


class Failure(RuntimeError):
    pass


def record(severity: int, magic: bytes = MAGIC, reserved: bytes = b"\0\0\0") -> bytes:
    """One record of the wire (thermal_protocol.hpp), with the fields that a bad one breaks."""
    return magic + bytes([severity]) + reserved


# What the HAL must refuse, as the golden frames name them.
REFUSED = {
    "emergency": record(5),
    "shutdown": record(6),
    "magic": record(0, magic=b"ATH2"),
    "reserved": record(1, reserved=b"\0\1\0"),
}


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

@dataclass
class ThermalDump:
    status: int | None
    cached: list[tuple[str, int, int, float]]
    hal: list[tuple[str, int, int, float]]
    thresholds: list[tuple[str, int, list[float]]]
    headroom_thresholds: list[float]
    hal_line: str | None
    hal_ready: bool


TEMPERATURE = re.compile(r"Temperature\{mValue=(\S+?), mType=(-?\d+), mName=(\S+?), mStatus=(-?\d+)\}")
THRESHOLD = re.compile(
    r"TemperatureThreshold\{mType=(-?\d+), mName=(\S+?), mHotThrottlingThresholds=\[([^\]]*)\]")


def numbers(text: str) -> list[float]:
    return [float(item) for item in text.replace(" ", "").split(",") if item]


def section(lines: list[str], start: str, *ends: str) -> list[str]:
    """The lines after the line that starts with `start`, up to the first line that starts with an end."""
    for index, line in enumerate(lines):
        if line.startswith(start):
            found = []
            for following in lines[index + 1:]:
                if any(following.startswith(end) for end in ends):
                    break
                found.append(following)
            return found
    return []


def temperatures(lines: list[str]) -> list[tuple[str, int, int, float]]:
    found = []
    for line in lines:
        match = TEMPERATURE.search(line)
        if match:
            found.append((match[3], int(match[2]), int(match[4]), float(match[1])))
    return found


def parse_thermal_dump(text: str) -> ThermalDump:
    """`dumpsys thermalservice`: the status, the cached and the HAL's temperatures, its thresholds."""
    lines = text.splitlines()
    status = next((int(m[1]) for line in lines if (m := re.fullmatch(r"Thermal Status: (-?\d+)", line.strip()))), None)
    thresholds = []
    for line in section(lines, "Temperature static thresholds from HAL:", "Temperature headroom thresholds:"):
        match = THRESHOLD.search(line)
        if match:
            thresholds.append((match[2], int(match[1]), numbers(match[3])))
    headroom = section(lines, "Temperature headroom thresholds:")
    hal_line = next((line.strip() for line in lines if line.strip().startswith("ThermalHAL ")), None)
    return ThermalDump(
        status=status,
        cached=temperatures(section(lines, "Cached temperatures:", "HAL Ready:")),
        hal=temperatures(section(lines, "Current temperatures from HAL:", "Current cooling devices from HAL:")),
        thresholds=thresholds,
        headroom_thresholds=numbers(headroom[0].strip().strip("[]")) if headroom else [],
        hal_line=hal_line,
        hal_ready="HAL Ready: true" in text,
    )


def parse_headroom(text: str) -> float | None:
    """`cmd thermalservice headroom N`: the value, or None when it prints an error or NaN."""
    match = re.search(r"Headroom in \d+ seconds: (\S+)", text)
    if not match:
        return None
    value = float(match[1])
    return None if math.isnan(value) else value


@dataclass
class DisplayDump:
    status: int | None
    map_has_severe_cap: bool
    map_has_moderate_hold: bool
    skin_vote_minimum: float | None
    skin_vote_maximum: float | None
    skin_vote: bool


def parse_display_dump(text: str) -> DisplayDump:
    """`dumpsys display`: the skin observer's status and map, and the vote it cast."""
    status = re.search(r"SkinThermalStatusObserver:\s+mStatus: (-?\d+)", text)
    map_match = re.search(r"mThermalThrottlingByDisplay: (\{.*\})", text)
    # SurfaceControl.RefreshRateRange prints as "(min max)"; status 3 is SEVERE and status 2 MODERATE. Both
    # endpoints are the contract: a point of 15 to 60 Hz holds the maximum and lets Android render at 15.
    severe = bool(map_match and re.search(r"\b3=\(0(?:\.0)? 30\.0\)", map_match[1]))
    moderate = bool(map_match and re.search(r"\b2=\(0(?:\.0)? 60\.0\)", map_match[1]))
    vote = re.search(r"PRIORITY_SKIN_TEMPERATURE -> (.*)", text)
    minimum = maximum = None
    if vote:
        found = re.search(r"mMinRefreshRate=(\S+?)[ ,}]", vote[1])
        minimum = float(found[1]) if found else None
        found = re.search(r"mMaxRefreshRate=(\S+?)[ ,}]", vote[1])
        maximum = float(found[1]) if found else None
    return DisplayDump(int(status[1]) if status else None, severe, moderate, minimum, maximum, vote is not None)


def parse_render_rates(text: str) -> list[float]:
    """`dumpsys SurfaceFlinger`: the render rate of each display that prints one."""
    return [float(rate) for rate in re.findall(r"renderRate=(\d+(?:\.\d+)?) Hz", text)]


FRAME_LINE = re.compile(r"Native display frames: guest (\d+(?:\.\d+)?)/s presented (\d+(?:\.\d+)?)/s")


def presented_rate(log_text: str) -> float | None:
    """The highest presented rate of a measurement's full statistics windows, or None when it has none.

    The launcher prints a line every FRAME_STATS_SECONDS for the window that just ended. The first line
    printed after a measurement starts is not the measurement's: its window began earlier and holds the
    previous step's last frames (a 43.0 line of an uncapped step ahead of a capped one), so it is ignored.
    Every later line ends inside the measurement, and the busy ones are its windows. A measurement of two
    windows or more has one; with none there is no figure and the caller fails."""
    rates = [float(match[2]) for match in FRAME_LINE.finditer(log_text)][1:]
    busy = [rate for rate in rates if rate >= 5.0]
    return max(busy) if busy else None


# ---- The guest.

class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 forward_port: int) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.forward_port = forward_port

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False)

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()[-500:]}")
        return result.stdout

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])

    def thermal(self) -> ThermalDump:
        return parse_thermal_dump(self.shell("dumpsys thermalservice"))

    def display(self) -> DisplayDump:
        return parse_display_dump(self.shell("dumpsys display"))

    def render_rates(self) -> list[float]:
        return parse_render_rates(self.shell("dumpsys SurfaceFlinger"))

    def headroom(self) -> float | None:
        return parse_headroom(self.shell("cmd thermalservice headroom 0"))

    def present_rate(self, frames: int = PRESENT_FRAMES) -> tuple[float, float | None]:
        """Frames a second of ``kernarium_vk_present`` by the clock, and of the host's presented frames."""
        before = self.log.stat().st_size
        started = time.monotonic()
        self.shell(f"kernarium_vk_present {frames}", timeout=PRESENT_SECONDS)
        seconds = time.monotonic() - started
        with self.log.open("rb") as stream:
            stream.seek(before)
            added = stream.read().decode(errors="replace")
        return frames / seconds, presented_rate(added)


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: Guest | None = None) -> None:
    """Calls `check` until it returns None (done); its last message is the failure."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        message = check()
        if message is None:
            return
        time.sleep(1.0)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


def vsock_forward_port(log: pathlib.Path, process: subprocess.Popen, guest_port: int) -> int:
    """The host port the launcher chose for the forward to a guest vsock port."""
    deadline = time.monotonic() + FORWARD_SECONDS
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise Failure(f"the guest exited with status {process.returncode}:\n"
                          + log.read_text(errors="replace")[-3000:])
        for _, port, found in FORWARD.findall(log.read_text(errors="replace")):
            if int(found) == guest_port:
                return int(port)
        time.sleep(0.2)
    raise Failure(f"the launcher reported no vsock forward to guest port {guest_port}")


class Host:
    """The host application's end of the stream: one connection at a time."""

    def __init__(self, port: int) -> None:
        self.port = port
        self.connection: socket.socket | None = None

    def connect(self) -> None:
        self.close()
        self.connection = socket.create_connection(("127.0.0.1", self.port), timeout=10)

    def send(self, data: bytes) -> None:
        if self.connection is None:
            self.connect()
        assert self.connection is not None
        self.connection.sendall(data)

    def closed_by_guest(self, seconds: float) -> bool:
        """Whether the guest closed the connection within the time (it sends nothing, so any read ends it)."""
        assert self.connection is not None
        self.connection.settimeout(seconds)
        try:
            return self.connection.recv(16) == b""
        except socket.timeout:
            return False
        except ConnectionError:
            return True

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None


# ---- The checks.

def check_thermal_state(guest: Guest, severity: int) -> str | None:
    """None when the thermal manager shows `severity` for the one SKIN sensor, else what differs."""
    dump = guest.thermal()
    value, _ = LEVELS.get(severity, (None, None))
    if dump.status != severity:
        return f"Thermal Status {dump.status}, not {severity}"
    skin = [entry for entry in dump.cached if entry[1] == 3]
    if len(skin) != 1 or skin[0][0] != "skin":
        return f"the cached SKIN temperatures are {skin}"
    if skin[0][2] != severity:
        return f"the skin status is {skin[0][2]}, not {severity}"
    if value is not None and abs(skin[0][3] - value) > 0.05:
        return f"the skin temperature is {skin[0][3]}, not {value}"
    return None


def check_static(guest: Guest) -> None:
    dump = guest.thermal()
    if not dump.hal_ready or not dump.hal_line or "AIDL 3" not in dump.hal_line or "connected: yes" not in dump.hal_line:
        raise Failure(f"the thermal manager is not connected to a version 3 HAL: {dump.hal_line!r}")
    if [entry[:2] for entry in dump.hal] != [("skin", 3)]:
        raise Failure(f"the HAL reports {dump.hal}, not the one SKIN sensor")
    if len(dump.thresholds) != 1 or dump.thresholds[0][:2] != ("skin", 3):
        raise Failure(f"the HAL's thresholds are {dump.thresholds}, not the one SKIN sensor's")
    if not same_numbers(dump.thresholds[0][2], HOT_THRESHOLDS):
        raise Failure(f"the hot thresholds are {dump.thresholds[0][2]}, not {list(HOT_THRESHOLDS)}")
    if not same_numbers(dump.headroom_thresholds, HEADROOM_THRESHOLDS):
        raise Failure(f"the headroom thresholds are {dump.headroom_thresholds}, not {list(HEADROOM_THRESHOLDS)}")


def same_numbers(found: list[float], expected: tuple[float, ...], tolerance: float = 1e-3) -> bool:
    return len(found) == len(expected) and all(
        (math.isnan(a) and math.isnan(b)) or abs(a - b) <= tolerance for a, b in zip(found, expected))


def check_headroom(guest: Guest, severity: int) -> None:
    expected = LEVELS[severity][1]

    def settled() -> str | None:
        value = guest.headroom()
        if value is None:
            return "no headroom yet"
        if abs(value - expected) > 0.08:
            return f"headroom {value}, not about {expected}"
        return None

    wait_for(f"headroom at severity {severity}", STATE_SECONDS, settled, guest)


def check_cap(guest: Guest, capped: bool, label: str, hold: float | None = None) -> None:
    """The vote, the render rate and the frame rates, together. `hold` is the maximum of a vote that is not
    the 30 Hz cap and changes no frame rate here (MODERATE's 60 Hz on a display that shows 60). A vote is
    0 to its maximum: both endpoints are checked."""
    def vote() -> str | None:
        display = guest.display()
        if capped:
            if not display.skin_vote or (display.skin_vote_minimum, display.skin_vote_maximum) != (0.0, 30.0):
                return (f"the skin vote is {display.skin_vote_minimum} to {display.skin_vote_maximum}, "
                        "not a 0 to 30 Hz render vote")
        elif hold is not None:
            if not display.skin_vote or (display.skin_vote_minimum, display.skin_vote_maximum) != (0.0, hold):
                return (f"the skin vote is {display.skin_vote_minimum} to {display.skin_vote_maximum}, "
                        f"not a 0 to {hold:g} Hz render vote")
        elif display.skin_vote:
            return f"a skin vote remains (maximum {display.skin_vote_maximum})"
        return None

    def render() -> str | None:
        rates = guest.render_rates()
        if not rates:
            return "SurfaceFlinger prints no render rate"
        inside = [CAPPED_RENDER_RATE[0] <= rate <= CAPPED_RENDER_RATE[1] for rate in rates]
        if capped and not all(inside):
            return f"render rates {rates}, not all 30"
        if not capped and any(inside):
            return f"render rates {rates} still include 30"
        return None

    wait_for(f"{label}: the display manager's vote", STATE_SECONDS, vote, guest)
    wait_for(f"{label}: SurfaceFlinger's render rate", STATE_SECONDS, render, guest)
    clock, host = guest.present_rate(PRESENT_FRAMES if capped else UNCAPPED_PRESENT_FRAMES)
    print(f"  {label}: kernarium_vk_present {clock:.1f} fps, host presented "
          f"{'none' if host is None else f'{host:.1f}'} a second")
    low, high = CAPPED_FPS if capped else (UNCAPPED_FPS_MINIMUM, math.inf)
    if not low <= clock <= high:
        raise Failure(f"{label}: kernarium_vk_present ran at {clock:.1f} fps, not in {low} to {high}")
    if host is None:
        raise Failure(f"{label}: the launcher printed no full busy statistics window inside the measurement "
                      "(UML_GPU_FRAME_STATS; the first line after the start is not the measurement's)")
    if not low <= host <= high:
        raise Failure(f"{label}: the host presented {host:.1f} frames a second, not in {low} to {high}")


def settle_severity(guest: Guest, severity: int) -> None:
    wait_for(f"the thermal manager at severity {severity}", STATE_SECONDS,
             lambda: check_thermal_state(guest, severity), guest)


def run_checks(guest: Guest) -> None:
    host = Host(guest.forward_port)
    try:
        print("before any host: the HAL's own state")
        check_static(guest)
        settle_severity(guest, 0)
        display = guest.display()
        if not display.map_has_severe_cap:
            raise Failure("the display's skin observer has no SEVERE point of 30 Hz "
                          "(is the default display configuration read?)")
        if not display.map_has_moderate_hold:
            raise Failure("the display's skin observer has no MODERATE point of 60 Hz "
                          "(is the default display configuration read?)")
        if display.skin_vote:
            raise Failure("a skin vote is cast at NONE")

        print("host connects and says NONE")
        host.send(record(0))
        time.sleep(2)
        settle_severity(guest, 0)
        check_headroom(guest, 0)

        # What the application sends: the host goes from NONE straight to CRITICAL and straight back.
        print("CRITICAL from NONE")
        host.send(record(4))
        settle_severity(guest, 4)
        check_headroom(guest, 4)
        check_cap(guest, True, "critical from none")

        print("NONE after CRITICAL")
        host.send(record(0))
        settle_severity(guest, 0)
        check_headroom(guest, 0)
        check_cap(guest, False, "none after critical")

        print("LIGHT")
        host.send(record(1))
        settle_severity(guest, 1)
        check_headroom(guest, 1)
        check_cap(guest, False, "light")

        print("MODERATE")
        host.send(record(2))
        settle_severity(guest, 2)
        check_headroom(guest, 2)
        check_cap(guest, False, "moderate", hold=60.0)

        print("SEVERE")
        host.send(record(3))
        settle_severity(guest, 3)
        check_headroom(guest, 3)
        check_cap(guest, True, "severe")

        print("CRITICAL")
        host.send(record(4))
        settle_severity(guest, 4)
        check_headroom(guest, 4)
        check_cap(guest, True, "critical")

        print("records the HAL refuses leave the state alone")
        for name, frame in REFUSED.items():
            host.connect()
            host.send(frame)
            if not host.closed_by_guest(CLOSE_SECONDS):
                raise Failure(f"the HAL kept the connection after the {name} record")
            time.sleep(2)
            guest.alive()
            problem = check_thermal_state(guest, 4)
            if problem:
                raise Failure(f"after the {name} record: {problem}")
            if guest.thermal().status in (5, 6):
                raise Failure(f"the {name} record reached EMERGENCY or SHUTDOWN")
        guest.shell("echo guest-answers")

        print("a host that drops keeps what it last said")
        host.connect()
        host.send(record(3))
        settle_severity(guest, 3)
        host.close()
        time.sleep(15)
        guest.alive()
        problem = check_thermal_state(guest, 3)
        if problem:
            raise Failure(f"after the host dropped: {problem}")
        check_cap(guest, True, "after the host dropped")

        print("NONE lifts the cap")
        host.connect()
        host.send(record(0))
        settle_severity(guest, 0)
        check_cap(guest, False, "none")
    finally:
        host.close()


# ---- Starting and stopping the guest.

def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path) -> Guest:
    instance = options.instance or private / "instance"
    boot_options = boot_android.parse_arguments([
        *(["--release", str(options.release)] if options.release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(instance),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        "--display", "offscreen",
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = [
        *boot_android.emulator_command(
            boot_options, release, instance / boot_android.android_instance.DATA_DISK, emulator,
            boot_android.data_capacity(boot_options, identity)),
        f"--vsock-forward=127.0.0.1:0-{THERMAL_PORT}",
    ]
    log = private / "emulator.log"
    environment = {**os.environ, "UML_GPU_FRAME_STATS": str(FRAME_STATS_SECONDS),
                   "PYTHONDONTWRITEBYTECODE": "1"}
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True, env=environment)
    serial = f"127.0.0.1:{options.adb_port}"
    # The emulator is this function's until it returns the guest: the caller's cleanup knows a guest only
    # from that return, so a failure or an interrupt (a timeout, Ctrl-C) before it stops the emulator here.
    try:
        guest = Guest(adb, serial, process, log, vsock_forward_port(log, process, THERMAL_PORT))
        boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
        if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
            raise Failure("Android did not reach sys.boot_completed")
        # A pending update counts as applied from here, as for the launcher; a fresh guest skips its wizard.
        boot_android.android_instance.booted(instance, identity)
        if not options.instance:
            boot_android.provision_guest(adb, serial)
        print(f"booted; settling for {SETTLE_SECONDS} s")
        time.sleep(SETTLE_SECONDS)
    except BaseException:
        boot_android.disconnect_adb(adb, serial)
        boot_android.stop_guest(process)
        raise
    return guest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--instance", type=pathlib.Path,
                        help="an existing instance to boot (for example an older release's data disk); "
                        "the default is a fresh one that is removed at the end")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-thermal-"))
    guest: Guest | None = None
    try:
        guest = start_guest(options, adb, private)
        run_checks(guest)
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        log = private / "emulator.log"
        if log.is_file():
            print("launcher log tail:\n" + "\n".join(log.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            boot_android.disconnect_adb(adb, guest.serial)
            boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
