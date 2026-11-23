#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's device-orientation sensor, and the display rotation it drives.

The guest is the release on a fresh data disk, started with the macOS launcher and
``--vsock-forward=127.0.0.1:0-5556``, so that the runner plays the host application over the motion
connection: it reads the guest's control frames and sends the orientation records the application sends from
the iPhone. The contract is android/lineage-24/README.md ("Motion sensors"); the wire is
overlay/sensors/motion_protocol.hpp and tests/unit/andarium/fixtures/motion-orientation-golden-frames.txt.

``--expect sensor`` (the default) is a release that lists the sensor when the launch says
``androidboot.kernarium.orientation=1``, which the runner passes as the application does. It boots twice:

- early, a fresh data disk with a host that connects at once and sends a value before system_server has
  registered the sensor (the first frame must not yet say it is in use): the image's auto-rotate default;
  the setup wizard must follow the sensor and turn to that value; with the home screen provisioned, Settings
  follows each of the four values (no 180 degree turn on a phone) and holds each; with auto-rotate off the
  display stays and the rotation suggestion button appears, and tapping it turns the display; applications
  that ask for landscape, sensor landscape, portrait and full sensor are turned as DisplayRotation turns
  them;
- late, no host for a minute: the window manager has registered the sensor and the display stays upright; a
  host that connects late is told the sensor is in use (an "ANC2" frame with bit 2), no value turns nothing,
  and the first value turns the display; the lock screen (a swipe lock the step turns on) stays upright and
  the newest value turns the display at unlock; a value said again changes nothing; a value sent with a
  finger down leaves the display alone until the finger lifts, then turns it with no further event from the
  host; a value sent while the screen is off turns the display once it is on, the guest having said the
  sensor idle while off and in use again, with the sensor's own memory and with the application's rule of
  saying it again at activation; a host that drops and connects again is told the same, and a record the
  guest refuses ends the connection and leaves the guest up. Restarting the framework or the sensors HAL
  needs root, which a user build does not give.

``--expect none`` is a release that does not list the sensor (an older release given the argument, or this
release without it, ``--no-argument``). The guest sends "ANC1" frames only, lists no orientation sensor, the
window manager judges the accelerometer, and the runner turns the display with accelerometer records of
the same four postures (then the same repeat, touch, screen-off and lock-screen steps with a posture streamed
until the next), which proves the older path is untouched and gives the rotation each posture means.

``--with-compass`` (with ``--expect sensor``) also passes ``androidboot.kernarium.compass=1``, as an iPhone with a
magnetometer boots: the guest then lists the magnetometer too and sends the 20-byte "ANC3" frame (bit 4 says
the orientation sensor is listed) in place of "ANC2", and every step above is the same. The legacy phase
(``--expect none``) expects ANC1 frames and no magnetometer, so the option is refused with it: an older release
given the compass argument is run_android_compass.py's ``--expect none --with-argument``.

``--launch-only`` stops each guest once the launch's state is checked (the property, the sensor service's
list and the sensor's flags, the sub-HAL's log line, no SELinux denial about the property, the judge the window
manager picked and what it holds): the first thing to run on a new image.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest
is reached through the shared adb server, always named with ``-s``. The runner is opt-in: it needs the
macos-android17 executable, an installed release, an adb client and a JDK (the probe application is built and
signed with ``keytool`` and ``jarsigner``).
usage: run_android_orientation.py --emulator uml-darwin-android17 [--release DIR] [--expect sensor|none]
       [--with-compass]
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
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ElementTree
from collections.abc import Callable
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(HERE))
import slot_ports  # noqa: E402
import orientation_probe_apk  # noqa: E402

SPEC = importlib.util.spec_from_file_location("boot_android", REPOSITORY / "tools" / "boot_android.py")
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)

DEFAULT_ADB_PORT = 5586
MOTION_PORT = 5556
ORIENTATION_ARGUMENT = "androidboot.kernarium.orientation=1"
COMPASS_ARGUMENT = "androidboot.kernarium.compass=1"
FORWARD = re.compile(r"vsock forward (\S+):(\d+) -> guest port (\d+)")
FORWARD_SECONDS = 60
COMMAND_SECONDS = 120
SETTLE_SECONDS = 45
ROTATION_SECONDS = 40
HOLD_SECONDS = 6
CLOSE_SECONDS = 15
FRAME_SECONDS = 20
SEND_SECONDS = 10
# Frame lengths by the magic's last byte: "ANC1" and "ANC2" are 16 bytes, "ANC3" (with the magnetometer) 20.
FRAME_LENGTHS = {ord("1"): 16, ord("2"): 16, ord("3"): 20}
RECORD_SIZE = 20
ORIENTATION_TYPE = 27
ACCELEROMETER_TYPE = 1
GRAVITY = 9.80665

SENSOR_NAME = "Host Device Orientation"
# ActivityInfo.screenOrientationToString of each request the probe application makes.
REQUEST_CONSTANTS = {
    "landscape": "SCREEN_ORIENTATION_LANDSCAPE",
    "sensorLandscape": "SCREEN_ORIENTATION_SENSOR_LANDSCAPE",
    "portrait": "SCREEN_ORIENTATION_PORTRAIT",
    "fullSensor": "SCREEN_ORIENTATION_FULL_SENSOR",
}
UPRIGHT, COUNTER_CLOCKWISE, UPSIDE_DOWN, CLOCKWISE = 0, 1, 2, 3
ROTATION_NAMES = {0: "ROTATION_0", 1: "ROTATION_90", 2: "ROTATION_180", 3: "ROTATION_270"}
ROTATION_FROM_NAME = {"ROTATION_0": 0, "ROTATION_90": 1, "ROTATION_180": 2, "ROTATION_270": 3}


class Failure(RuntimeError):
    pass


# ---- The wire (overlay/sensors/motion_protocol.hpp). The tooling test checks these against the golden frames.

def orientation_record(value: int | float) -> bytes:
    """A host-to-guest record of the device-orientation sensor: "ANS1", type 27, the value, two zeros."""
    return b"ANS1" + struct.pack("<I3f", ORIENTATION_TYPE, float(value), 0.0, 0.0)


def accelerometer_record(x: float, y: float, z: float) -> bytes:
    return b"ANS1" + struct.pack("<I3f", ACCELEROMETER_TYPE, x, y, z)


@dataclass(frozen=True)
class Control:
    """A guest-to-host control frame: "ANC1", "ANC2" or the 20-byte "ANC3" (mask bit 2: the orientation sensor is
    in use; bit 3: the magnetometer is; bit 4: the orientation sensor is listed)."""
    version: int
    mask: int
    accelerometer_us: int
    gyroscope_us: int
    magnetometer_us: int | None = None

    @property
    def orientation_active(self) -> bool:
        return bool(self.mask & 4)

    @property
    def orientation_listed(self) -> bool:
        """Whether the guest lists the orientation sensor: version 2 always does, version 3 says so in bit 4."""
        return self.version == 2 or (self.version == 3 and bool(self.mask & 16))


def frame_length(prefix: bytes) -> int | None:
    """How long the frame is that begins with these (at least four) bytes; None for a magic no frame has."""
    if prefix[:3] != b"ANC" or prefix[3] not in FRAME_LENGTHS:
        return None
    return FRAME_LENGTHS[prefix[3]]


def parse_control(frame: bytes) -> Control:
    """Reads "ANC1", "ANC2" or "ANC3"; anything else the host would end the connection for is a ValueError."""
    length = frame_length(frame[:4]) if len(frame) >= 4 else None
    if length is None or len(frame) != length:
        raise ValueError(f"not a control frame: {frame.hex(' ')}")
    version = frame[3] - ord("0")
    words = struct.unpack(f"<{length // 4 - 1}I", frame[4:])
    mask = words[0]
    if version < 3 and mask & ~(3 if version == 1 else 7):
        raise ValueError(f"reserved bits in the mask {mask:#x} of a version {version} frame")
    if version == 3 and (mask & ~0b11111 or (mask & 0b100 and not mask & 0b10000)):
        raise ValueError(f"no such state: the mask {mask:#x} of a version 3 frame")
    return Control(version, mask, words[1], words[2], words[3] if version == 3 else None)


# What the gravity vector of each posture looks like to the accelerometer (Android's axes: x to the right of the
# screen, y up the screen, z out of it; at rest the sensor reads gravity's reaction, +y when upright). Turning
# the device 90 degrees counter-clockwise points its x axis up (AccelSensorJudge turns that into ROTATION_90).
POSTURES = {
    UPRIGHT: (0.0, GRAVITY, 0.0),
    COUNTER_CLOCKWISE: (GRAVITY, 0.0, 0.0),
    UPSIDE_DOWN: (0.0, -GRAVITY, 0.0),
    CLOCKWISE: (-GRAVITY, 0.0, 0.0),
}


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

@dataclass
class RotationState:
    rotation: int | None
    user_mode: str | None
    user_rotation: int | None
    allow_all: bool | None
    landscape: int
    seascape: int
    app_orientation: str | None
    listener_enabled: bool | None
    judge: str | None
    sensor: str | None
    proposed: int | None


def rotation_number(name: str) -> int:
    return ROTATION_FROM_NAME[name]


def parse_rotation_state(text: str) -> RotationState:
    """`dumpsys window displays`: DisplayRotation's state and WindowOrientationListener's."""
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.strip() == "DisplayRotation"), None)
    if start is None:
        raise ValueError("no DisplayRotation section in the window dump")
    block = "\n".join(lines[start:start + 70])

    def find(pattern: str) -> re.Match | None:
        return re.search(pattern, block, re.MULTILINE)

    rotation = find(r"^\s*mRotation=(\d)\b")
    mode = find(r"mUserRotationMode=(\w+) mUserRotation=(ROTATION_\d+)")
    allow = find(r"mAllowAllRotations=(true|false)")
    ways = find(r"mLandscapeRotation=(ROTATION_\d+) mSeascapeRotation=(ROTATION_\d+)")
    app = find(r"mCurrentAppOrientation=(\S+)")
    enabled = find(r"^\s*mEnabled=(true|false)")
    judge = find(r"^\s*(AccelSensorJudge|OrientationSensorJudge)\s*$")
    sensor = find(r"^\s*mSensor=(.*)$")
    proposed = find(r"^\s*mProposedRotation=(-?\d+)")
    return RotationState(
        rotation=int(rotation[1]) if rotation else None,
        user_mode=mode[1] if mode else None,
        user_rotation=rotation_number(mode[2]) if mode else None,
        allow_all=(allow[1] == "true") if allow else None,
        landscape=rotation_number(ways[1]) if ways else COUNTER_CLOCKWISE,
        seascape=rotation_number(ways[2]) if ways else CLOCKWISE,
        app_orientation=app[1] if app else None,
        listener_enabled=(enabled[1] == "true") if enabled else None,
        judge=judge[1] if judge else None,
        sensor=sensor[1].strip() if sensor else None,
        proposed=int(proposed[1]) if proposed else None,
    )


@dataclass
class SensorState:
    listed: dict[str, int]
    active: dict[str, int]
    orientation_line: str | None


SENSOR_LINE = re.compile(r"^0x[0-9a-f]+\) (.+?)\s+\| .*?type: \S+?\((\d+)\)", re.MULTILINE)
ACTIVE_LINE = re.compile(r"^(.+?) \(handle=0x[0-9a-f]+, connections=(\d+)\)", re.MULTILINE)


def parse_sensor_state(text: str) -> SensorState:
    """`dumpsys sensorservice`: the sensors listed (name to type) and those with a connection."""
    listed = {m[1].strip(): int(m[2]) for m in SENSOR_LINE.finditer(text)}
    active: dict[str, int] = {}
    marker = text.find("Active sensors:")
    if marker >= 0:
        end = text.find("\n\n", marker)
        for match in ACTIVE_LINE.finditer(text[marker:end if end > 0 else len(text)]):
            active[match[1].strip()] = int(match[2])
    line = next((line for line in text.splitlines() if SENSOR_NAME in line and "type:" in line), None)
    return SensorState(listed, active, line)


def sensor_description(text: str, name: str) -> str | None:
    """The two lines `dumpsys sensorservice` lists a sensor with, joined: name, type and flags, then the reporting
    mode ("continuous" or "on-change"), the rates and "non-wakeUp"."""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if re.match(r"^0x[0-9a-f]+\) " + re.escape(name) + r"\s+\|", line):
            following = lines[index + 1].strip() if index + 1 < len(lines) else ""
            return f"{line.strip()} {following}"
    return None


def judges_connected(text: str) -> list[str]:
    """The window manager's judges holding a sensor connection, in the order the dump names them."""
    found: list[str] = []
    for match in re.finditer(r"WindowOrientationListener\$(\w+)", text):
        if match[1] not in found:
            found.append(match[1])
    return found


AVC_DENIAL = re.compile(r"avc:\s+denied", re.IGNORECASE)
HAL_LOG_LINE = re.compile(r"Andarium motion: ro\.boot\.kernarium\.orientation=(\d+), (\d+) sensors")


def hal_log_lines(log: str) -> str:
    """The lines of a log that launch_state reads: the sub-HAL's own line and every SELinux denial."""
    return "\n".join(line for line in log.splitlines() if "Andarium motion: ro.boot" in line or AVC_DENIAL.search(line))


def sensors_hal_denials(log: str) -> list[str]:
    """The SELinux denials in a log that concern the orientation property or the sensors HAL's own domain."""
    return [line.strip() for line in log.splitlines() if AVC_DENIAL.search(line)
            and ("kernarium" in line or "hal_sensors" in line or "ro.boot." in line)]


@dataclass(frozen=True)
class Node:
    text: str
    description: str
    resource: str
    package: str
    bounds: tuple[int, int, int, int]

    @property
    def centre(self) -> tuple[int, int]:
        return (self.bounds[0] + self.bounds[2]) // 2, (self.bounds[1] + self.bounds[3]) // 2


BOUNDS = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")


def parse_hierarchy(text: str) -> tuple[int | None, list[Node]]:
    """`uiautomator dump` (or ``--windows``, whose root is ``displays``): the display rotation it states and every
    node with a position."""
    start = text.find("<?xml")
    root = ElementTree.fromstring(text[start:] if start >= 0 else text)
    hierarchy = root if root.tag == "hierarchy" else root.find(".//hierarchy")
    rotation = hierarchy.get("rotation") if hierarchy is not None else None
    nodes = []
    for element in root.iter("node"):
        bounds = BOUNDS.fullmatch(element.get("bounds", ""))
        if not bounds:
            continue
        nodes.append(Node(element.get("text", ""), element.get("content-desc", ""),
                          element.get("resource-id", ""), element.get("package", ""),
                          (int(bounds[1]), int(bounds[2]), int(bounds[3]), int(bounds[4]))))
    return (int(rotation) if rotation is not None else None), nodes


def suggestion_button(nodes: list[Node]) -> Node | None:
    """The rotation suggestion button: ``rotate_suggestion``, "Rotate screen". SystemUI's navigation bar hosts it,
    and so does the launcher's taskbar on a large screen (this image's 720x1280 at 213 dpi is one)."""
    for node in nodes:
        if node.package in ("com.android.systemui", "com.android.launcher3") and (
                node.resource.endswith(":id/rotate_suggestion")
                or re.search(r"rotate screen", node.description, re.IGNORECASE)):
            return node
    return None


# ---- What DisplayRotation.rotationForOrientation turns each request into (frameworks/base,
# services/core/java/com/android/server/wm/DisplayRotation.java). `value` is the sensor's newest value, or None
# before the host said one; `last` is the rotation the display has.

def expected_rotation(request: str, value: int | None, last: int, *, locked: bool = False, user_rotation: int = 0,
                      allow_all: bool = False, landscape: int = COUNTER_CLOCKWISE,
                      seascape: int = CLOCKWISE) -> int:
    sensor = last if value is None else value
    follows = request in ("unspecified", "sensor", "fullSensor", "sensorLandscape", "sensorPortrait") and (
        not locked or request != "unspecified")
    if follows:
        preferred = sensor if (sensor != UPSIDE_DOWN or allow_all or request == "fullSensor") else last
    elif locked and request not in ("landscape", "portrait"):
        preferred = user_rotation
    else:
        preferred = -1
    in_landscape = lambda rotation: rotation in (landscape, seascape)  # noqa: E731
    in_portrait = lambda rotation: rotation in (UPRIGHT, UPSIDE_DOWN)  # noqa: E731
    if request == "portrait":
        return preferred if in_portrait(preferred) else UPRIGHT
    if request == "landscape":
        return preferred if in_landscape(preferred) else landscape
    if request == "sensorLandscape":
        if in_landscape(preferred):
            return preferred
        return last if in_landscape(last) else landscape
    return preferred if preferred >= 0 else UPRIGHT


# ---- The host's end of the connection.

class Host:
    """The host application's end of the stream: one connection at a time, with a thread that reads the frames.

    ``current`` is the orientation the application would say now. With ``replay`` set the host plays the
    application's rule (Andarium/README.md, Motion sensors): after the first "ANC2" frame of a connection, and
    each time a frame says the sensor became active, it says ``current`` again. ``sustain`` is the accelerometer
    side of a release that lists no orientation sensor: a posture's records, streamed until another is named."""

    def __init__(self, port: int) -> None:
        self.port = port
        self.connection: socket.socket | None = None
        self.condition = threading.Condition()
        self.sending = threading.Lock()
        self.frames: list[Control] = []
        self.bad: list[bytes] = []
        self.closed = False
        self.reader: threading.Thread | None = None
        self.current: int | None = None
        self.replay = False
        self.replays = 0
        self.posture: int | None = None
        self.sustained = threading.Event()
        self.sustainer: threading.Thread | None = None
        self._seen_anc2 = False
        self._active = False

    def connect(self) -> None:
        self.close()
        # A bound on every send: a guest that stops reading fails the step instead of blocking the runner. The
        # reader treats a timeout as no frame yet.
        connection = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        connection.settimeout(SEND_SECONDS)
        with self.condition:
            self.connection = connection
            self.frames = []
            self.bad = []
            self.closed = False
            self._seen_anc2 = False
            self._active = False
        self.reader = threading.Thread(target=self._read, args=(connection,), daemon=True)
        self.reader.start()

    def _read(self, connection: socket.socket) -> None:
        buffer = b""
        try:
            while True:
                try:
                    data = connection.recv(256)
                except socket.timeout:
                    continue
                if not data:
                    break
                buffer += data
                # A frame is cut where its magic says; a prefix of a longer frame is waited for.
                while len(buffer) >= 4:
                    length = frame_length(buffer)
                    if length is None:
                        with self.condition:
                            self.bad.append(buffer)
                            self.condition.notify_all()
                        buffer = b""
                        break
                    if len(buffer) < length:
                        break
                    frame, buffer = buffer[:length], buffer[length:]
                    try:
                        control = parse_control(frame)
                    except ValueError:
                        with self.condition:
                            self.bad.append(frame)
                            self.condition.notify_all()
                        continue
                    with self.condition:
                        if self.connection is not connection:
                            continue
                        self.frames.append(control)
                        say = (control.orientation_listed and self.replay and self.current is not None
                               and (not self._seen_anc2 or (control.orientation_active and not self._active)))
                        if control.orientation_listed:
                            self._seen_anc2 = True
                            self._active = control.orientation_active
                        self.condition.notify_all()
                    if say:
                        self.replays += 1
                        self.send(orientation_record(self.current))
        except OSError:
            pass
        with self.condition:
            if self.connection is connection:
                self.closed = True
            self.condition.notify_all()

    def wait_frame(self, seconds: float, predicate: Callable[[Control], bool] = lambda frame: True,
                   after: int = 0) -> Control | None:
        """The first frame that satisfies the predicate, or None. It looks among the frames received since
        connecting, from index ``after`` on (``len(host.frames)`` when a step began)."""
        deadline = time.monotonic() + seconds
        with self.condition:
            while True:
                for frame in self.frames[after:]:
                    if predicate(frame):
                        return frame
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.closed:
                    return None
                self.condition.wait(min(remaining, 1.0))

    def send(self, data: bytes) -> None:
        connection = self.connection
        if connection is None:
            raise Failure("the host has no connection")
        with self.sending:
            connection.sendall(data)

    def push(self, value: int) -> None:
        """The application says an orientation: it is the current one, and a connection hears it."""
        self.current = value
        self.send(orientation_record(value))

    def hold_posture(self, posture: int, seconds: float) -> None:
        """Accelerometer records of a posture, at 50 a second."""
        x, y, z = POSTURES[posture]
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.send(accelerometer_record(x, y, z))
            time.sleep(0.02)

    def sustain(self, posture: int | None) -> None:
        """Streams the posture's accelerometer records, 50 a second, until told another (or None for none)."""
        self.posture = posture
        if self.sustainer is None:
            self.sustainer = threading.Thread(target=self._sustain, daemon=True)
            self.sustainer.start()

    def _sustain(self) -> None:
        while not self.sustained.is_set():
            posture = self.posture
            if posture is not None:
                try:
                    self.send(accelerometer_record(*POSTURES[posture]))
                except (OSError, Failure):
                    pass
            time.sleep(0.02)

    def closed_by_guest(self, seconds: float) -> bool:
        deadline = time.monotonic() + seconds
        with self.condition:
            while not self.closed:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self.condition.wait(min(remaining, 1.0))
        return True

    def close(self) -> None:
        with self.condition:
            connection, self.connection = self.connection, None
        if connection is not None:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()

    def stop(self) -> None:
        self.sustained.set()
        self.close()


class EarlyHost:
    """Connects the moment the guest accepts and says a value at its first "ANC2" frame, as early as the
    guest allows: before system_server has registered the sensor."""

    def __init__(self, host: Host, value: int, seconds: float) -> None:
        self.host = host
        self.value = value
        self.deadline = time.monotonic() + seconds
        self.sent_at: float | None = None
        self.first: Control | None = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        while not self.stop.is_set() and time.monotonic() < self.deadline:
            try:
                self.host.connect()
            except OSError:
                time.sleep(1.0)
                continue
            frame = self.host.wait_frame(3.0)
            if frame is None:
                self.host.close()
                time.sleep(1.0)
                continue
            self.first = frame
            if frame.orientation_listed:
                try:
                    self.host.push(self.value)
                except OSError:
                    continue
                self.sent_at = time.monotonic()
            return

    def join(self) -> None:
        self.stop.set()
        self.thread.join(timeout=10)


# ---- The guest.

class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 forward_port: int) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.forward_port = forward_port
        #: When sys.boot_completed was seen (time.monotonic), for the early host's ordering check.
        self.boot_completed_at: float | None = None
        #: What launch_state reads from the log, taken at boot_completed: the sub-HAL logs once, early, and
        #: the buffer has rolled over it by the time a step looks.
        self.early_log = ""

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

    def rotation_state(self) -> RotationState:
        return parse_rotation_state(self.shell("dumpsys window displays"))

    def rotation(self) -> int:
        value = self.rotation_state().rotation
        if value is None:
            raise Failure("the window dump states no rotation")
        return value

    def sensors(self) -> SensorState:
        return parse_sensor_state(self.shell("dumpsys sensorservice"))

    def setting(self, name: str) -> str:
        return self.shell(f"settings get system {name}").strip()

    def auto_rotate(self, on: bool) -> None:
        self.shell(f"settings put system accelerometer_rotation {1 if on else 0}")

    def system_server(self) -> str:
        return self.shell("pidof system_server").strip()

    def hierarchy(self) -> tuple[int | None, list[Node]]:
        # --windows: the navigation bar and the taskbar are windows of their own, outside the active one.
        self.shell("uiautomator dump --windows /data/local/tmp/orientation.xml >/dev/null 2>&1", timeout=60)
        return parse_hierarchy(self.shell("cat /data/local/tmp/orientation.xml"))

    def start(self, component: str) -> None:
        output = self.shell(f"am start -W -n {component}")
        if "Error" in output:
            raise Failure(f"starting {component}: {output.strip()[-300:]}")

    def settings_app(self) -> None:
        output = self.shell("am start -W -a android.settings.SETTINGS")
        if "Error" in output:
            raise Failure(f"starting Settings: {output.strip()[-300:]}")


def wait_for(description: str, seconds: float, check: Callable[[], str | None], guest: Guest | None = None) -> None:
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


# ---- Checks. Each returns a line for the report, or raises Failure.

def wait_rotation(guest: Guest, expected: int, label: str, seconds: float = ROTATION_SECONDS) -> None:
    def settled() -> str | None:
        found = guest.rotation()
        return None if found == expected else f"rotation {found}, not {expected}"

    wait_for(f"{label}: rotation", seconds, settled, guest)


def hold_rotation(guest: Guest, expected: int, label: str, seconds: float = HOLD_SECONDS) -> None:
    """The display stays where it is: an event that turns it late shows here."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest.alive()
        found = guest.rotation()
        if found != expected:
            raise Failure(f"{label}: the display turned to {found} (expected it to stay at {expected})")
        time.sleep(1.0)


def check_sensor_listed(guest: Guest, expect_sensor: bool) -> str:
    sensors = guest.sensors()
    state = guest.rotation_state()
    if expect_sensor:
        if sensors.listed.get(SENSOR_NAME) != 27:
            raise Failure(f"the sensor service lists {sensors.listed}, not {SENSOR_NAME} (type 27)")
        if state.judge != "OrientationSensorJudge":
            raise Failure(f"the window manager judges with {state.judge}, not OrientationSensorJudge")
        if not state.sensor or SENSOR_NAME not in state.sensor:
            raise Failure(f"the window manager's sensor is {state.sensor}")
        return f"listed (type 27), judge {state.judge}"
    if SENSOR_NAME in sensors.listed:
        raise Failure(f"the sensor service lists {SENSOR_NAME} without the argument")
    if state.judge != "AccelSensorJudge":
        raise Failure(f"the window manager judges with {state.judge}, not AccelSensorJudge")
    return f"not listed, judge {state.judge}"


ORIENTATION_PROPERTY = "ro.boot.kernarium.orientation"
ACCELEROMETER_NAME = "Host Accelerometer"


def launch_state(guest: Guest, expect_sensor: bool, argument: bool, compass: bool = False) -> str:
    """What the launch's argument decided (``argument``: whether the launch carried it; an older release given
    it reads the property and lists no sensor): the property, the sensors the sub-HAL listed (its own log line says
    how many), the sensor's flags, which judge the window manager picked and which sensor it holds, and that
    SELinux denied the sensors HAL nothing about the property. ``compass``: the launch also carried the compass
    argument, which lists the magnetometer as a fourth sensor."""
    value = guest.shell(f"getprop {ORIENTATION_PROPERTY}").strip()
    if value != ("1" if argument else ""):
        raise Failure(f"getprop {ORIENTATION_PROPERTY} reads {value!r}, expected {'1' if argument else 'nothing'}")
    dump = guest.shell("dumpsys sensorservice")
    sensors = parse_sensor_state(dump)
    for name, kind in ((ACCELEROMETER_NAME, 1), ("Host Gyroscope", 4)):
        if sensors.listed.get(name) != kind:
            raise Failure(f"the sensor service lists {sensors.listed}, not {name} (type {kind})")
    notes = []
    log = guest.early_log + "\n" + guest.shell("logcat -d -b all 2>/dev/null | grep -E 'Andarium motion: ro.boot|avc: +denied'; true")
    said = HAL_LOG_LINE.findall(log)
    wanted = ("1", str(3 + int(compass))) if expect_sensor else ("0", "2")
    if not expect_sensor and argument:
        wanted = None  # an older sub-HAL says nothing; a newer one given the argument would list the sensor
    if said and wanted is not None and said[-1] != wanted:
        raise Failure("the sub-HAL's log line says =%s, %s sensors; expected =%s, %s" % (*said[-1], *wanted))
    notes.append("log line =%s, %s sensors" % said[-1] if said else "log line not found in the buffer")
    denials = sensors_hal_denials(log)
    if denials:
        raise Failure(f"SELinux denials about the property or the sensors HAL: {denials[:3]}")
    state = guest.rotation_state()
    if expect_sensor:
        description = sensor_description(dump, SENSOR_NAME)
        if description is None or "type: android.sensor.device_orientation(27)" not in description:
            raise Failure(f"the sensor service describes {SENSOR_NAME} as {description!r}")
        if "on-change" not in description or "non-wakeUp" not in description:
            raise Failure(f"{SENSOR_NAME} is not on-change and non-wake-up: {description!r}")
        settle_sensor_use(guest, True)
        dump = guest.shell("dumpsys sensorservice")
        sensors = parse_sensor_state(dump)
        judges = judges_connected(dump)
        if sensors.active.get(ACCELEROMETER_NAME):
            raise Failure(f"the accelerometer is held ({sensors.active}), by {judges}")
        if judges != ["OrientationSensorJudge"] or state.judge != "OrientationSensorJudge":
            raise Failure(f"the window manager's connections are {judges}, judge {state.judge}")
        notes += ["on-change, non-wakeUp", "held by OrientationSensorJudge", "the accelerometer idle"]
    else:
        if SENSOR_NAME in sensors.listed:
            raise Failure(f"the sensor service lists {SENSOR_NAME} without the argument")
        settle_accelerometer_use(guest)
        dump = guest.shell("dumpsys sensorservice")
        judges = judges_connected(dump)
        if judges != ["AccelSensorJudge"] or state.judge != "AccelSensorJudge":
            raise Failure(f"the window manager's connections are {judges}, judge {state.judge}")
        notes += ["two sensors and the virtual ones", "held by AccelSensorJudge"]
    return f"{ORIENTATION_PROPERTY}={value or '(unset)'}; " + "; ".join(notes)


def settle_accelerometer_use(guest: Guest) -> None:
    def check() -> str | None:
        return None if guest.sensors().active.get(ACCELEROMETER_NAME, 0) >= 1 else "the accelerometer is not held"

    wait_for("the accelerometer's use", ROTATION_SECONDS, check, guest)


def settle_sensor_use(guest: Guest, active: bool) -> None:
    """The window manager has the sensor registered (or not) once its use has settled."""
    def check() -> str | None:
        connections = guest.sensors().active.get(SENSOR_NAME, 0)
        if active and connections < 1:
            return "no connection holds the sensor"
        if not active and connections:
            return f"{connections} connections hold the sensor"
        return None

    wait_for("the sensor's use", ROTATION_SECONDS, check, guest)


def turn_sequence(guest: Guest, push: Callable[[int], None], values: list[int], request: str, label: str,
                  start: int) -> int:
    """Sends each value and checks the display ends where DisplayRotation puts it. Returns the last rotation."""
    last = start
    for value in values:
        state = guest.rotation_state()
        expected = expected_rotation(
            request, value, last, locked=state.user_mode == "USER_ROTATION_LOCKED",
            user_rotation=state.user_rotation or 0, allow_all=bool(state.allow_all),
            landscape=state.landscape, seascape=state.seascape)
        push(value)
        wait_rotation(guest, expected, f"{label}: value {value}")
        hold_rotation(guest, expected, f"{label}: value {value}", 4)
        last = expected
    return last


def request_name(guest: Guest) -> str:
    return guest.rotation_state().app_orientation or "?"


@dataclass
class Report:
    rows: list[tuple[str, str, str]] = field(default_factory=list)

    def add(self, name: str, status: str, detail: str) -> None:
        self.rows.append((name, status, detail))
        print(f"  [{status}] {name}: {detail}", flush=True)

    @property
    def failed(self) -> bool:
        return any(status == "FAIL" for _, status, _ in self.rows)

    def run(self, name: str, step: Callable[[], str]) -> bool:
        try:
            self.add(name, "PASS", step())
            return True
        except (Failure, subprocess.TimeoutExpired, OSError, ValueError) as problem:
            self.add(name, "FAIL", str(problem))
            return False


# ---- Phases.

def with_compass(options: argparse.Namespace) -> bool:
    """Whether the launch carries the compass argument: this runner's option, or an argument another runner built
    on this one passes."""
    return bool(getattr(options, "with_compass", False)) or COMPASS_ARGUMENT in getattr(options, "extra_arguments", ())


def early_phase(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    print("phase early: a fresh disk, a host that sends a value before the window manager has the sensor", flush=True)
    host_holder: dict[str, Host] = {}
    guest = start_guest(options, adb, private / "early", early=3, holder=host_holder, provision=False)
    host = host_holder["host"]
    early: EarlyHost = host_holder["early"]  # type: ignore[assignment]
    try:
        report.run("launch state", lambda: launch_state(guest, True, True, with_compass(options)))
        report.run("early host", lambda: describe_early(guest, early))
        report.run("fresh disk default", lambda: fresh_default(guest))
        report.run("sensor listed", lambda: check_sensor_listed(guest, True))
        report.run("setup wizard turns to the value sent before the sensor was registered",
                   lambda: wizard_rotation(guest, host))
        boot_android.provision_guest(adb, guest.serial)
        time.sleep(10)
        guest.settings_app()
        report.run("Settings follows each value", lambda: settings_values(
            guest, host.push))
        report.run("auto-rotate off: suggestion button", lambda: auto_rotate_off(guest, host))
        report.run("applications that ask for an orientation", lambda: probe_requests(
            guest, host.push, private))
    finally:
        host.stop()
        stop_guest(adb, guest)


def describe_early(guest: Guest, early: EarlyHost) -> str:
    early.join()
    if early.first is None:
        raise Failure("the early host never received a frame from the guest")
    if not early.first.orientation_listed:
        raise Failure(f"the first frame was ANC{early.first.version} without the orientation sensor listed")
    if early.first.orientation_active:
        raise Failure(f"the first frame already says the sensor is in use (mask {early.first.mask:#x}): the window "
                      "manager had registered it, so the value was not sent ahead of the registration")
    if early.sent_at is None:
        raise Failure("the early host sent no value")
    if guest.boot_completed_at is None or early.sent_at >= guest.boot_completed_at:
        raise Failure("the early value was not sent before sys.boot_completed")
    ahead = guest.boot_completed_at - early.sent_at
    return (f"first frame ANC{early.first.version} mask {early.first.mask:#x}; value 3 sent {ahead:.0f} s before "
            "sys.boot_completed, ahead of the window manager's registration")


def fresh_default(guest: Guest) -> str:
    value = guest.setting("accelerometer_rotation")
    mode = guest.shell("wm user-rotation").strip()
    if value != "1":
        raise Failure(f"accelerometer_rotation is {value!r} on a fresh disk, not 1 (auto-rotate on)")
    if "free" not in mode:
        raise Failure(f"'wm user-rotation' says {mode!r} on a fresh disk")
    return f"accelerometer_rotation={value}, wm user-rotation={mode}: Android turns without anyone touching auto-rotate"


def wizard_rotation(guest: Guest, host: Host) -> str:
    state = guest.rotation_state()
    top = guest.shell("dumpsys activity activities | grep -m1 topResumedActivity").strip()
    request = state.app_orientation or "?"
    follows = request.endswith("UNSPECIFIED") or request.endswith("SENSOR") or request.endswith("USER")
    if not follows:
        raise Failure(f"the setup wizard asks for {request} (rotation {state.rotation}), not an orientation that "
                      "follows the sensor")
    wait_rotation(guest, CLOCKWISE, "setup wizard")
    host.push(UPRIGHT)
    wait_rotation(guest, UPRIGHT, "setup wizard, back to upright")
    component = re.search(r"u\d+ (\S+/\S+)", top)
    return f"{component[1] if component else '?'} ({request}) went to 3 with the early value and back to 0"


def settings_values(guest: Guest, push: Callable[[int], None]) -> str:
    state = guest.rotation_state()
    request = (state.app_orientation or "").replace("SCREEN_ORIENTATION_", "").lower()
    if request not in ("unspecified", "user", "sensor"):
        raise Failure(f"Settings asks for {state.app_orientation}, not an orientation that follows the sensor")
    push(UPRIGHT)
    wait_rotation(guest, UPRIGHT, "Settings, upright")
    sequence = [COUNTER_CLOCKWISE, CLOCKWISE, UPRIGHT, UPSIDE_DOWN, COUNTER_CLOCKWISE, UPRIGHT]
    turn_sequence(guest, push, sequence, "unspecified", "Settings", UPRIGHT)
    return (f"values {sequence} (allow all rotations {state.allow_all}, so a 180 degree turn is "
            f"{'taken' if state.allow_all else 'refused'}); each held")


def auto_rotate_off(guest: Guest, host: Host) -> str:
    host.push(UPRIGHT)
    wait_rotation(guest, UPRIGHT, "before locking")
    guest.auto_rotate(False)
    wait_for("the lock", 20, lambda: None if guest.rotation_state().user_mode == "USER_ROTATION_LOCKED"
             else "the user rotation mode is not locked", guest)
    found: list[Node] = []
    attempts = 0

    def propose() -> str | None:
        # The button stays for a few seconds and a dump takes about four; a proposal that equals the display's
        # turns nothing, so a miss leaves the value and comes again.
        nonlocal attempts
        if attempts:
            host.push(UPRIGHT)
            time.sleep(2)
        attempts += 1
        host.push(COUNTER_CLOCKWISE)
        _, nodes = guest.hierarchy()
        shown = guest.rotation()
        if shown != UPRIGHT:
            raise Failure(f"auto-rotate off: the display turned to {shown} before the button was used")
        button = suggestion_button(nodes)
        if button is None:
            return "no rotation suggestion button in the windows' hierarchy"
        found.append(button)
        return None

    try:
        wait_for("the rotation suggestion button", 90, propose, guest)
    except Failure as problem:
        if "no rotation suggestion button" not in str(problem):
            raise
        _, nodes = guest.hierarchy()
        sample = [(node.package, node.description or node.text or node.resource) for node in nodes
                  if node.description][:12]
        raise Failure(f"no rotation suggestion button after {attempts} proposals; nodes with a description: {sample}")
    x, y = found[-1].centre
    # The button hides a few seconds after the proposal and a dump takes about four, so the tap follows a fresh
    # proposal (the value 0 takes the button away, the value 1 brings it back) once its animation has begun.
    host.push(UPRIGHT)
    time.sleep(2)
    host.push(COUNTER_CLOCKWISE)
    time.sleep(1.5)
    guest.shell(f"input tap {x} {y}")
    wait_rotation(guest, COUNTER_CLOCKWISE, "after the suggestion button")
    state = guest.rotation_state()
    if state.user_mode != "USER_ROTATION_LOCKED" or state.user_rotation != COUNTER_CLOCKWISE:
        raise Failure(f"after the button the user rotation is {state.user_mode} {state.user_rotation}")
    host.push(UPRIGHT)
    hold_rotation(guest, COUNTER_CLOCKWISE, "auto-rotate off, value 0", 4)
    guest.auto_rotate(True)
    wait_rotation(guest, UPRIGHT, "auto-rotate on again follows the newest value")
    return (f"value 1 left the display at 0 and offered the button ({found[-1].package}) at {x},{y}, which turned it to 1 "
            f"with the user rotation locked at 1 (proposals: {attempts}); auto-rotate on again: 0")


def probe_requests(guest: Guest, push: Callable[[int], None], private: pathlib.Path) -> str:
    apk = private / "orientation-probe.apk"
    orientation_probe_apk.build_apk(apk, private)
    result = guest.adb_run("install", "-r", str(apk), timeout=180)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing the probe: {result.stdout.strip()[-400:]}")
    lines = []
    try:
        for request, sequence in (("landscape", [CLOCKWISE, UPRIGHT]),
                                  ("sensorLandscape", [CLOCKWISE, COUNTER_CLOCKWISE, UPRIGHT]),
                                  ("portrait", [COUNTER_CLOCKWISE, UPRIGHT]),
                                  ("fullSensor", [UPSIDE_DOWN, CLOCKWISE, UPRIGHT])):
            push(UPRIGHT)
            guest.settings_app()
            wait_rotation(guest, UPRIGHT, f"{request}: before")
            guest.start(orientation_probe_apk.component(request))
            wait_for(f"{request}: the request", 30,
                     lambda: None if guest.rotation_state().app_orientation == REQUEST_CONSTANTS[request]
                     else f"asks for {guest.rotation_state().app_orientation}", guest)
            state = guest.rotation_state()
            start = expected_rotation(request, UPRIGHT, UPRIGHT, landscape=state.landscape, seascape=state.seascape)
            wait_rotation(guest, start, f"{request}: opening")
            end = turn_sequence(guest, push, sequence, request, request, start)
            lines.append(f"{request}: open at {start}, values {sequence} end at {end}")
    finally:
        guest.shell("pm uninstall " + orientation_probe_apk.PACKAGE + " >/dev/null 2>&1; true")
        guest.shell("am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null 2>&1; true")
    return "; ".join(lines)


def late_phase(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    print("phase late: no host for a minute, then a host that connects late", flush=True)
    holder: dict[str, Host] = {}
    guest = start_guest(options, adb, private / "late", early=None, holder=holder, provision=True)
    host = holder["host"]
    try:
        server = guest.system_server()
        guest.settings_app()
        report.run("launch state", lambda: launch_state(guest, True, True, with_compass(options)))
        report.run("sensor listed", lambda: check_sensor_listed(guest, True))
        report.run("no host: the window manager registered the sensor, the display stays", lambda: no_host(guest))
        if options.launch_only:
            return
        report.run("host connects late", lambda: late_host(guest, host))
        report.run("lock screen", lambda: lock_screen(guest, host.push))
        report.run("a value said again", lambda: duplicates(guest, host.push))
        report.run("a touch held during a turn", lambda: touch_held(guest, host.push))
        report.run("a value sent while the screen is off", lambda: screen_off_on(guest, host, host.push, True))
        report.run("reconnect and refused records", lambda: reconnect(guest, host))
        report.run("system_server stayed up", lambda: stayed_up(guest, server))
    finally:
        host.stop()
        stop_guest(adb, guest)


def no_host(guest: Guest) -> str:
    settle_sensor_use(guest, True)
    hold_rotation(guest, UPRIGHT, "no host", 8)
    return "the sensor has a connection; auto-rotate on, Settings in front, no value: the display stays at 0"


def late_host(guest: Guest, host: Host) -> str:
    host.connect()
    first = host.wait_frame(FRAME_SECONDS)
    if first is None:
        raise Failure("the guest sent no frame to the late host")
    if not first.orientation_listed or not first.orientation_active:
        raise Failure(f"the late host's first frame is ANC{first.version} with mask {first.mask:#x}; the sensor "
                      "is in use and the frame must say so (ANC2 or ANC3, bit 2)")
    hold_rotation(guest, UPRIGHT, "connected, no value", 5)
    host.push(COUNTER_CLOCKWISE)
    wait_rotation(guest, COUNTER_CLOCKWISE, "the first value")
    return f"first frame ANC{first.version} mask {first.mask:#x}; no value, no turn; value 1 turned the display to 1"


def keyguard_showing(text: str) -> bool | None:
    """`dumpsys window policy`: whether the keyguard is showing, or None if the dump does not say."""
    match = re.search(r"^\s*showing=(true|false)", text, re.MULTILINE)
    return None if match is None else match[1] == "true"


def keyguard_state(guest: Guest) -> bool:
    found = keyguard_showing(guest.shell("dumpsys window policy | grep -E '^ +showing='; true"))
    if found is None:
        raise Failure("the window policy dump states no keyguard")
    return found


def lock_screen(guest: Guest, push: Callable[[int], None]) -> str:
    """The lock screen asks not to follow the sensor, so the display shows it upright whatever the value, and a
    value sent there turns the display once the screen is unlocked. A provisioned guest has no lock screen, so
    the step turns it on (a swipe lock, no credential) and puts the setting back as it found it, with the
    screen on and no lock screen over the display, whatever happened in between."""
    before = guest.shell("cmd lock_settings get-disabled").strip()
    if before not in ("true", "false"):
        raise Failure(f"'cmd lock_settings get-disabled' says {before!r}")
    guest.shell("cmd lock_settings set-disabled false")
    try:
        guest.settings_app()
        push(COUNTER_CLOCKWISE)
        wait_rotation(guest, COUNTER_CLOCKWISE, "before the lock screen")
        guest.shell("input keyevent KEYCODE_SLEEP")
        wait_for("the screen going off", 20, lambda: None if wakefulness(guest) == "Asleep" else "awake", guest)
        time.sleep(1)
        guest.shell("input keyevent KEYCODE_WAKEUP")
        wait_for("the screen coming on", 20, lambda: None if wakefulness(guest) == "Awake" else "asleep", guest)
        wait_for("the lock screen", 20, lambda: None if keyguard_state(guest) else "the keyguard is not showing", guest)
        request = guest.rotation_state().app_orientation
        if request != "SCREEN_ORIENTATION_NOSENSOR":
            raise Failure(f"the lock screen asks for {request}, not SCREEN_ORIENTATION_NOSENSOR")
        push(CLOCKWISE)
        hold_rotation(guest, UPRIGHT, "the lock screen with the value 3", 5)
        guest.shell("wm dismiss-keyguard")
        guest.shell("input keyevent KEYCODE_MENU")
        wait_for("the lock screen to go", 20, lambda: "the keyguard is showing" if keyguard_state(guest) else None, guest)
        wait_rotation(guest, CLOCKWISE, "after unlocking")
        push(UPRIGHT)
        wait_rotation(guest, UPRIGHT, "after unlocking, a new value")
    finally:
        # A step that failed with the lock screen up would leave every later step waiting on a display that
        # cannot turn: wake the screen and dismiss the keyguard before the setting goes back.
        try:
            if wakefulness(guest) != "Awake":
                guest.shell("input keyevent KEYCODE_WAKEUP")
            if keyguard_state(guest):
                guest.shell("wm dismiss-keyguard")
                guest.shell("input keyevent KEYCODE_MENU")
        except Failure:
            pass
        guest.shell(f"cmd lock_settings set-disabled {before}")
    return ("the lock screen showed (request NOSENSOR) upright with the value 3 sent there; after unlocking the display "
            "turned to 3, and the next value (0) turned it back")


def reconnect(guest: Guest, host: Host) -> str:
    host.connect()
    first = host.wait_frame(FRAME_SECONDS)
    if first is None or not first.orientation_listed or not first.orientation_active:
        raise Failure(f"the reconnected host's first frame is {first}")
    host.push(COUNTER_CLOCKWISE)
    wait_rotation(guest, COUNTER_CLOCKWISE, "after reconnecting")
    refused = {
        "high": orientation_record(4), "negative": orientation_record(-1), "fraction": orientation_record(1.5),
        "second": b"ANS1" + struct.pack("<I3f", ORIENTATION_TYPE, 1.0, 1.0, 0.0),
        "nan": b"ANS1" + struct.pack("<I3f", ORIENTATION_TYPE, float("nan"), 0.0, 0.0),
    }
    for name, frame in refused.items():
        host.connect()
        if host.wait_frame(FRAME_SECONDS) is None:
            raise Failure(f"before the {name} record the guest sent no frame")
        host.send(frame)
        if not host.closed_by_guest(CLOSE_SECONDS):
            raise Failure(f"the guest kept the connection after the {name} record")
        guest.alive()
        hold_rotation(guest, COUNTER_CLOCKWISE, f"after the {name} record", 2)
    host.connect()
    if host.wait_frame(FRAME_SECONDS) is None:
        raise Failure("the guest does not accept a connection after the refused records")
    return f"a new connection is told the sensor is in use and takes a value; {', '.join(refused)} records end the connection and change nothing"


def stayed_up(guest: Guest, before: str) -> str:
    after = guest.system_server()
    if after != before:
        raise Failure(f"system_server restarted: pid {before}, then {after}")
    return f"system_server pid {after} throughout"


def touch_point(guest: Guest) -> int:
    """A coordinate inside the screen whichever way it is turned."""
    match = re.search(r"Physical size: (\d+)x(\d+)", guest.shell("wm size"))
    if match is None:
        raise Failure("'wm size' states no physical size")
    return min(int(match[1]), int(match[2])) // 2


def duplicates(guest: Guest, push: Callable[[int], None]) -> str:
    guest.settings_app()
    push(UPRIGHT)
    wait_rotation(guest, UPRIGHT, "duplicates, upright")
    push(COUNTER_CLOCKWISE)
    wait_rotation(guest, COUNTER_CLOCKWISE, "duplicates, the first 1")
    for _ in range(3):
        push(COUNTER_CLOCKWISE)
        time.sleep(0.4)
    hold_rotation(guest, COUNTER_CLOCKWISE, "the value 1 said again", 4)
    push(UPRIGHT)
    push(UPRIGHT)
    wait_rotation(guest, UPRIGHT, "duplicates, 0 twice")
    hold_rotation(guest, UPRIGHT, "the value 0 said twice", 4)
    return "1 said four times and 0 twice in a row: the display followed the first and no repeat moved it"


def touch_held(guest: Guest, push: Callable[[int], None]) -> str:
    """A turn made while a finger is down: Android waits for the touch to end, and no further host event comes."""
    guest.settings_app()
    push(UPRIGHT)
    wait_rotation(guest, UPRIGHT, "touch, upright")
    point = touch_point(guest)
    guest.shell(f"input touchscreen motionevent DOWN {point} {point}")
    turned: float | None = None
    start = time.monotonic()
    try:
        push(COUNTER_CLOCKWISE)
        start = time.monotonic()
        while time.monotonic() - start < 5:
            if guest.rotation() != UPRIGHT:
                turned = time.monotonic() - start
                break
            time.sleep(0.5)
    finally:
        # Away from where it went down, so that letting go is no tap on Settings.
        guest.shell(f"input touchscreen motionevent MOVE {point + 200} {point}")
        guest.shell(f"input touchscreen motionevent UP {point + 200} {point}")
    if turned is not None:
        # The step exists for the turn after the finger lifts; a turn under the finger would never exercise it.
        raise Failure(f"the display turned {turned:.1f} s into the touch, before the finger lifted")
    held_for = time.monotonic() - start
    wait_rotation(guest, COUNTER_CLOCKWISE, "after the touch ended")
    return (f"the value 1 sent with a finger down left the display at 0 for {held_for:.0f} s and turned it to 1 when "
            "the finger lifted, with no further event from the host")


def wakefulness(guest: Guest) -> str:
    found = re.search(r"mWakefulness=(\w+)", guest.shell("dumpsys power | grep -m1 mWakefulness="))
    return found[1] if found else "?"


def screen_off_on(guest: Guest, host: Host, push: Callable[[int], None], sensor: bool) -> str:
    """A value sent while the screen is off turns the display once it is on. The guest says the sensor idle
    while the screen is off and in use again after (for a release without the orientation sensor, the
    accelerometer). First with what the sensor keeps for itself, then with the application's own rule (the host
    says its value again at activation)."""
    in_use = (lambda frame: frame.orientation_active) if sensor else (lambda frame: bool(frame.mask & 1))
    what = "the orientation sensor" if sensor else "the accelerometer"
    notes = []
    for replay, first, second in ((False, UPRIGHT, CLOCKWISE), (True, CLOCKWISE, COUNTER_CLOCKWISE)):
        host.replay = replay
        try:
            guest.settings_app()
            push(first)
            wait_rotation(guest, first, "before the screen goes off")
            seen = len(host.frames)
            guest.shell("input keyevent KEYCODE_SLEEP")
            wait_for("the screen going off", 20, lambda: None if wakefulness(guest) == "Asleep" else "awake", guest)
            if host.wait_frame(FRAME_SECONDS, lambda frame: not in_use(frame), after=seen) is None:
                raise Failure(f"the guest never said {what} idle while the screen was off")
            time.sleep(1)
            push(second)
            time.sleep(2)
            mark = len(host.frames)
            guest.shell("input keyevent KEYCODE_WAKEUP")
            wait_for("the screen coming on", 20, lambda: None if wakefulness(guest) == "Awake" else "asleep", guest)
            if host.wait_frame(FRAME_SECONDS, in_use, after=mark) is None:
                raise Failure(f"the guest never said {what} in use again after the screen came on")
            time.sleep(2)
            guest.shell("wm dismiss-keyguard")
            guest.shell("input keyevent KEYCODE_MENU")
            wait_rotation(guest, second, "after the screen is on")
            notes.append(f"{'with' if replay else 'without'} the host saying it again: {first} -> {second}, "
                         f"{what} idle while off and in use again on waking")
        finally:
            host.replay = False
    return "; ".join(notes)


def legacy_phase(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    print("phase legacy: a release that does not list the sensor", flush=True)
    holder: dict[str, Host] = {}
    guest = start_guest(options, adb, private / "legacy", early=None, holder=holder, provision=True)
    host = holder["host"]
    try:
        guest.settings_app()
        report.run("launch state", lambda: launch_state(guest, False, not options.no_argument))
        report.run("sensor not listed", lambda: check_sensor_listed(guest, False))
        report.run("control frames are ANC1", lambda: legacy_frames(host))
        report.run("accelerometer postures turn the display", lambda: legacy_postures(guest, host))
        if options.launch_only:
            return
        report.run("Settings follows each posture", lambda: settings_values(
            guest, lambda value: host.hold_posture(value, 3.0)))
        # The same applications, turned by the accelerometer judge instead: the rules the sensor's
        # values are checked against are DisplayRotation's, whichever sensor proposes the rotation.
        report.run("applications that ask for an orientation", lambda: probe_requests(
            guest, lambda value: host.hold_posture(value, 3.0), private))
        # From here the posture is streamed until the next one, as the accelerometer's own samples would be.
        report.run("a posture said again", lambda: duplicates(guest, host.sustain))
        report.run("a touch held during a turn", lambda: touch_held(guest, host.sustain))
        report.run("the screen off and on", lambda: screen_off_on(guest, host, host.sustain, False))
        report.run("the lock screen", lambda: lock_screen(guest, host.sustain))
    finally:
        host.stop()
        stop_guest(adb, guest)


def legacy_frames(host: Host) -> str:
    host.connect()
    first = host.wait_frame(FRAME_SECONDS)
    if first is None:
        raise Failure("the guest sent no frame")
    time.sleep(3)
    versions = {frame.version for frame in host.frames}
    if versions != {1}:
        raise Failure(f"frames of versions {sorted(versions)} arrived, not only ANC1")
    if host.bad:
        raise Failure(f"frames the host cannot read: {[frame.hex(' ') for frame in host.bad]}")
    return f"{len(host.frames)} frames, all ANC1; first mask {first.mask:#x}"


def legacy_postures(guest: Guest, host: Host) -> str:
    seen = []
    for posture in (COUNTER_CLOCKWISE, UPRIGHT, CLOCKWISE, UPRIGHT):
        end = time.monotonic() + ROTATION_SECONDS
        while time.monotonic() < end:
            host.hold_posture(posture, 2.0)
            if guest.rotation() == posture:
                break
        else:
            raise Failure(f"posture {posture} left the display at {guest.rotation()}")
        seen.append(posture)
    return (f"gravity along +x turned the display to 1, along -x to 3, upright to 0 ({seen}): "
            "the rotation each orientation value means")


# ---- Starting and stopping the guest.

def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path, *, early: int | None,
                holder: dict, provision: bool) -> Guest:
    private.mkdir(parents=True, exist_ok=True)
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
        f"--vsock-forward=127.0.0.1:0-{MOTION_PORT}",
        *([] if options.no_argument else [ORIENTATION_ARGUMENT]),
        *([COMPASS_ARGUMENT] if getattr(options, "with_compass", False) else []),
        # More kernel arguments of a runner that builds on this one (run_android_compass.py).
        *getattr(options, "extra_arguments", ()),
    ]
    log = private / "emulator.log"
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    serial = f"127.0.0.1:{options.adb_port}"
    # The emulator is this function's until it returns the guest: the caller's cleanup knows a guest only
    # from that return, so a failure or an interrupt (a timeout, Ctrl-C) before it stops the emulator here.
    try:
        guest = Guest(adb, serial, process, log, vsock_forward_port(log, process, MOTION_PORT))
        host = Host(guest.forward_port)
        holder["host"] = host
        if early is not None:
            holder["early"] = EarlyHost(host, early, options.boot_timeout)
            holder["early"].start()
        boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
        if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
            raise Failure("Android did not reach sys.boot_completed")
        guest.boot_completed_at = time.monotonic()
        guest.early_log = hal_log_lines(guest.adb_run("logcat", "-d", "-b", "all", timeout=90).stdout)
        boot_android.android_instance.booted(instance, identity)
        if provision and not options.instance:
            boot_android.provision_guest(adb, serial)
        print(f"booted; settling for {SETTLE_SECONDS} s", flush=True)
        time.sleep(SETTLE_SECONDS)
    except BaseException:
        # The host that was to talk to this guest ends with it.
        early_host = holder.get("early")
        if early_host is not None:
            early_host.stop.set()
        if "host" in holder:
            holder["host"].stop()
        boot_android.disconnect_adb(adb, serial)
        boot_android.stop_guest(process)
        raise
    return guest


def stop_guest(adb: str, guest: Guest) -> None:
    boot_android.disconnect_adb(adb, guest.serial)
    boot_android.stop_guest(guest.process)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--instance", type=pathlib.Path,
                        help="an existing instance to boot; the default is a fresh one for each phase")
    parser.add_argument("--expect", choices=("sensor", "none"), default="sensor",
                        help="sensor: the release lists the orientation sensor when asked; none: it does not")
    parser.add_argument("--no-argument", action="store_true",
                        help="do not pass androidboot.kernarium.orientation=1 (an older application)")
    parser.add_argument("--with-compass", action="store_true",
                        help="with --expect sensor, also pass androidboot.kernarium.compass=1, as a phone with a "
                        "magnetometer boots: the guest sends ANC3 frames and lists the magnetometer too (refused "
                        "with --expect none)")
    parser.add_argument("--phases", default="early,late",
                        help="with --expect sensor, the phases to run (early, late); default both")
    parser.add_argument("--launch-only", action="store_true",
                        help="stop each guest once the launch's state is checked: the property, the sensors listed, "
                        "the judge and who holds which sensor (the first thing to run on a new image)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


def unsupported_combination(options: argparse.Namespace) -> str | None:
    """Why this run could only fail a valid guest, or None: the legacy phase checks for ANC1 frames and no
    magnetometer, which a guest booted with the compass argument does not give."""
    if options.expect == "none" and options.with_compass:
        return ("--with-compass needs --expect sensor: the legacy phase (--expect none) expects ANC1 frames and no "
                "magnetometer; an older release given the compass argument is run_android_compass.py --expect none "
                "--with-argument")
    return None


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    problem = unsupported_combination(options)
    if problem:
        print(problem, file=sys.stderr)
        return 2
    phases = [name for name in options.phases.split(",") if name]
    if options.expect == "sensor" and (not phases or set(phases) - {"early", "late"}):
        print("--phases names early, late or both", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-orient-"))
    report = Report()
    try:
        if options.expect == "none":
            legacy_phase(options, adb, private, report)
        else:
            if "early" in phases and not options.launch_only:
                early_phase(options, adb, private, report)
            if "late" in phases or options.launch_only:
                late_phase(options, adb, private, report)
        print("\n".join(f"{status:4} {name}: {detail}" for name, status, detail in report.rows))
        print("FAIL" if report.failed else "PASS")
        return 1 if report.failed else 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        for log in sorted(private.glob("*/emulator.log")):
            print(f"{log.parent.name} launcher log tail:\n"
                  + "\n".join(log.read_text(errors="replace").splitlines()[-30:]), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
