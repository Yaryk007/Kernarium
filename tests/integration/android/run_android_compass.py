#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's magnetometer and the compass feature it declares from the launch.

The guest is the release on a fresh data disk, started with the macOS launcher and
``--vsock-forward=127.0.0.1:0-5556``, so that the runner plays the host application over the motion
connection: it reads the guest's control frames ("ANC1", "ANC2" and the 20-byte "ANC3") and sends the
records the application sends from the iPhone, the magnetic field among them. The contract is
android/lineage-24/README.md ("Motion sensors"); the wire is overlay/sensors/motion_protocol.hpp and
tests/unit/andarium/fixtures/motion-orientation-golden-frames.txt. The device's own side (Core Motion's
field, the mount's axes) is the application's, so this runner sends the vectors the application would send
for each of the four mounts and checks what a client of the guest's SensorManager sees.

``--expect sensor`` (the default) is a release that lists the magnetometer when the launch says
``androidboot.kernarium.compass=1``, which the runner passes as the application does (``--with-orientation``
adds the orientation sensor's argument too). It checks:

- the launch state: the property, the sensor service's list (``Host Magnetometer``, type 2, continuous,
  non-wake-up, and with it the rotation vector and the geomagnetic rotation vector the service makes from the
  three sensors; the orientation sensor exactly when its argument was passed), ``pm list features`` and the
  package manager's own answer for ``android.hardware.sensor.compass`` (exit 0 and "true", or exit 1 and
  "false"), the sub-HAL's log line, and no SELinux denial about the property or the sensors HAL;
- the demand: the guest's frame is "ANC3" with the magnetometer idle, and nothing holds the sensor on the home
  screen, in Settings, in the camera application or across a screen off and on;
- the field: a foreground client (the motion probe, ``--probe-apk``, in its ``compass`` mode) registers a
  listener, the guest says so (bit 3 of "ANC3" and the period it asked for) and the range and resolution the
  client is given meet the compatibility definition, the rotation vector gets events and settles on one heading
  for a steady field, the application's field reaches the listener for each of the four mounts (to within half
  of the sensor service's rounding) with the azimuth the platform computes from it, a turn through north, east,
  south and west gives those azimuths, and each of the four statuses reaches ``onAccuracyChanged`` and
  ``SensorEvent.accuracy``;
- a record the guest refuses (a status above 3) ends the connection and not the guest: the sensor is in use
  again on the connection that follows, a different field reaches the same listener as a new sample, and
  neither system_server nor the sensors HAL restarted; the listener's end says the magnetometer idle.

A frame that is no frame, or the guest ending a connection nobody asked it to, fails the step that was running,
whatever came after it.

``--expect none`` is a release that does not list the sensor: an older release, or this release booted without
the argument (the default), or an older release given it (``--with-argument``: the application passes it to
every image, and the older one reads it as a property nothing uses). The guest sends "ANC1" frames (or "ANC2"
with ``--with-orientation``) and never "ANC3", lists no magnetic field sensor, declares no compass feature, and
ends the connection on a magnetic field record as it always has, with system_server and the sensors HAL still
the same processes.

``--launch-only`` stops each guest once the launch's state is checked: the first thing to run on a new image.
``--feed SECONDS`` keeps the guest up for that long as a host that sends a steady field, accelerometer and
gyroscope while the frames ask for them, and prints the adb serial: for a client of the guest that brings no
field of its own (the Compatibility Test Suite's sensor tests, run from another shell).

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest
is reached through the shared adb server, always named with ``-s``. The runner is opt-in: it needs the
macos-android17 executable, an installed release, an adb client and, for ``--expect sensor``, the probe
application (tests/integration/android/motion/build-apk.sh).
usage: run_android_compass.py --emulator uml-darwin-android17 [--release DIR] --probe-apk APK [--expect sensor|none]
       run_android_compass.py --emulator uml-darwin-android17 [--release DIR] --feed SECONDS
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import math
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
from collections.abc import Callable
from dataclasses import dataclass

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "integration" / "support"))
sys.path.insert(0, str(HERE))
# The orientation runner owns the guest: its launcher, its adb, its parsers of `dumpsys sensorservice`.
SPEC = importlib.util.spec_from_file_location("run_android_orientation", HERE / "run_android_orientation.py")
assert SPEC is not None and SPEC.loader is not None
orient = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = orient
SPEC.loader.exec_module(orient)
Failure = orient.Failure
Guest = orient.Guest
Report = orient.Report

COMPASS_ARGUMENT = "androidboot.kernarium.compass=1"
COMPASS_PROPERTY = "ro.boot.kernarium.compass"
MAGNETOMETER = "Host Magnetometer"
MAGNETIC_FIELD_TYPE = 2
COMPASS_FEATURE = "android.hardware.sensor.compass"
PROBE_PACKAGE = "org.example.andarium.motionprobe"
PROBE_COMPONENT = f"{PROBE_PACKAGE}/.MotionActivity"
PROBE_REQUESTED_US = 20_000
FRAME_SECONDS = orient.FRAME_SECONDS
SEND_SECONDS = orient.SEND_SECONDS
FIELD_SECONDS = 40
IDLE_SECONDS = 20
# The feeder looks this often for a sensor that is due, and sends each at half the period the guest asked for in its
# frame, never faster than every 10 ms: what the application does, so the guest sees the rate it asked for.
STREAM_TICK = 0.005
MINIMUM_STREAM_INTERVAL = 0.01
GRAVITY = orient.GRAVITY
# A field of a mid-latitude site in the northern hemisphere, in microtesla: 25 toward north (horizontal) and 40
# down, so a device lying face up reads (0, 25, -40) when its y axis points north.
HORIZONTAL = 25.0
VERTICAL = -40.0
# The sensor service rounds every event value to an eighth of the resolution it holds for the sensor (and may take
# that resolution from the range), so the probe reports the resolution a client sees and a value read back is the
# value sent to within half of that quantum, plus a float's slack: never a whole quantum.
SERVICE_FACTOR = 0.125
FLOAT_SLACK = 1e-4
# The figures of the compatibility definition the declarations are checked against: a range of at least 900 uT and a
# resolution of at most 0.6 uT; the range is a whole number of resolution steps, as the sensor service requires.
MINIMUM_RANGE = 900.0
MAXIMUM_RESOLUTION = 0.6
RANGE_STEP_SLACK = 1e-3
# The sensors HAL's process: the product installs it as this service binary.
SENSORS_HAL = "android.hardware.sensors-service.multihal"
# The recovery step after a refused record sends this heading's field: not the one before it.
RECOVERY_HEADING = 90.0
AZIMUTH_TOLERANCE = 2.0
# Types of the sensors the service makes from the accelerometer, the gyroscope and the magnetometer.
ROTATION_VECTOR_TYPE = 11
GEOMAGNETIC_ROTATION_VECTOR_TYPE = 20
# The rotation vector has settled when its azimuth stays within this many degrees over this many samples.
SETTLE_SAMPLES = 5
SETTLE_TOLERANCE = 3.0
ROTATION_SECONDS = 90

ACCELEROMETER_TYPE = 1
GYROSCOPE_TYPE = 4
# Frame lengths by the magic's last byte.
FRAME_LENGTHS = {ord("1"): 16, ord("2"): 16, ord("3"): 20}
# The azimuth (degrees from magnetic north to the device's y axis, clockwise, from -180 to 180) of each mount when
# the phone's own y axis points north, which is what the rotation of the application's axes means for a guest.
MOUNTS = {"portrait": 0.0, "portraitUpsideDown": 180.0, "landscapeLeft": -90.0, "landscapeRight": 90.0}
STATUS_NAMES = {0: "unreliable", 1: "low", 2: "medium", 3: "high"}


# ---- The wire (overlay/sensors/motion_protocol.hpp). The tooling test checks these against the golden frames.

def sensor_record(sensor_type: int, x: float, y: float, z: float, status: int = 0) -> bytes:
    """A host-to-guest record: "ANS1", the type as a uint16, the status byte, a zero byte, three float32."""
    return b"ANS1" + struct.pack("<HBB3f", sensor_type, status, 0, x, y, z)


def magnetic_record(x: float, y: float, z: float, status: int = 3) -> bytes:
    return sensor_record(MAGNETIC_FIELD_TYPE, x, y, z, status)


@dataclass(frozen=True)
class Frame:
    """A guest-to-host control frame, "ANC1", "ANC2" or "ANC3"."""
    version: int
    mask: int
    accelerometer_us: int
    gyroscope_us: int
    magnetometer_us: int | None = None

    @property
    def accelerometer_active(self) -> bool:
        return bool(self.mask & 1)

    @property
    def gyroscope_active(self) -> bool:
        return bool(self.mask & 2)

    @property
    def orientation_active(self) -> bool:
        return bool(self.mask & 4)

    @property
    def magnetometer_active(self) -> bool:
        return self.version == 3 and bool(self.mask & 8)

    @property
    def orientation_listed(self) -> bool:
        return self.version == 2 or (self.version == 3 and bool(self.mask & 16))


def frame_length(prefix: bytes) -> int | None:
    """How long the frame is that begins with these (at least four) bytes; None for a magic no frame has."""
    if prefix[:3] != b"ANC" or prefix[3] not in FRAME_LENGTHS:
        return None
    return FRAME_LENGTHS[prefix[3]]


def parse_frame(frame: bytes) -> Frame:
    """Reads a control frame; anything the host would end the connection for is a ValueError."""
    length = frame_length(frame[:4]) if len(frame) >= 4 else None
    if length is None or len(frame) != length:
        raise ValueError(f"not a control frame: {frame.hex(' ')}")
    version = frame[3] - ord("0")
    words = struct.unpack(f"<{length // 4 - 1}I", frame[4:])
    mask = words[0]
    if version == 1 and mask & ~0b11:
        raise ValueError(f"reserved bits in the mask {mask:#x} of a version 1 frame")
    if version == 2 and mask & ~0b111:
        raise ValueError(f"reserved bits in the mask {mask:#x} of a version 2 frame")
    if version == 3 and (mask & ~0b11111 or (mask & 0b100 and not mask & 0b10000)):
        raise ValueError(f"no such state: the mask {mask:#x} of a version 3 frame")
    return Frame(version, mask, words[1], words[2], words[3] if version == 3 else None)


def mounted(axes: str, x: float, y: float, z: float) -> tuple[float, float, float]:
    """What the application's MotionSample makes of a field in the phone's frame for a mount's axes
    (Andarium/AndariumCore/Sources/AndariumCore/MotionStream.swift)."""
    if axes == "portrait":
        return x, y, z
    if axes == "portraitUpsideDown":
        return -x, -y, z
    if axes == "landscapeLeft":
        return y, -x, z
    if axes == "landscapeRight":
        return -y, x, z
    raise ValueError(axes)


def azimuth(gravity: tuple[float, float, float], field: tuple[float, float, float]) -> float:
    """SensorManager.getRotationMatrix and getOrientation: the azimuth in degrees, -180 to 180."""
    ex, ey, ez = field
    ax, ay, az = gravity
    hx, hy, hz = ey * az - ez * ay, ez * ax - ex * az, ex * ay - ey * ax
    norm_h = math.sqrt(hx * hx + hy * hy + hz * hz)
    if norm_h < 0.1:
        raise ValueError("the field is too weak or parallel to gravity")
    hx, hy, hz = hx / norm_h, hy / norm_h, hz / norm_h
    norm_a = math.sqrt(ax * ax + ay * ay + az * az)
    ax, ay, az = ax / norm_a, ay / norm_a, az / norm_a
    my = az * hx - ax * hz
    return math.degrees(math.atan2(hy, my))


def angle_difference(a: float, b: float) -> float:
    """The difference of two angles in degrees, from 0 to 180."""
    return abs((a - b + 180.0) % 360.0 - 180.0)


def field_at_heading(heading: float) -> tuple[float, float, float]:
    """The field a device lying face up reads when its y axis points ``heading`` degrees clockwise from north."""
    radians = math.radians(heading)
    return -HORIZONTAL * math.sin(radians), HORIZONTAL * math.cos(radians), VERTICAL


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

HAL_LOG_LINE = re.compile(
    r"Andarium motion: ro\.boot\.kernarium\.orientation=(\d+), (\d+) sensors(?:, ro\.boot\.kernarium\.compass=(\d+))?")


def value_tolerance(resolution: float) -> float:
    """How far a value read back may be from the one sent: half of the service's quantum for this resolution."""
    return resolution * SERVICE_FACTOR / 2 + FLOAT_SLACK


def finite_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def check_figures(resolution: object, max_range: object) -> str:
    """The range and the resolution the sensor service reports to a client for the magnetometer."""
    for name, value in (("resolution", resolution), ("range", max_range)):
        if not finite_number(value) or value <= 0:  # type: ignore[operator]
            raise Failure(f"the probe reports the magnetometer's {name} as {value!r}")
    assert isinstance(resolution, (int, float)) and isinstance(max_range, (int, float))
    if resolution > MAXIMUM_RESOLUTION:
        raise Failure(f"the magnetometer's resolution is {resolution} uT, finer than {MAXIMUM_RESOLUTION} is required")
    if max_range < MINIMUM_RANGE:
        raise Failure(f"the magnetometer's range is {max_range} uT, under {MINIMUM_RANGE:g}")
    steps = max_range / resolution
    if abs(steps - round(steps)) > RANGE_STEP_SLACK:
        raise Failure(f"the magnetometer's range {max_range} uT is not a whole number of its resolution {resolution} uT")
    return f"resolution {resolution:g} uT, range {max_range:g} uT ({round(steps)} steps)"


def has_feature_answer(returncode: int, text: str) -> bool:
    """`cmd package has-feature`: exit 0 and "true", or exit 1 and "false"; any other pair (an error, an empty
    answer, a stray exit status) is not an answer and raises."""
    answer = text.strip()
    if (returncode, answer) == (0, "true"):
        return True
    if (returncode, answer) == (1, "false"):
        return False
    raise Failure(f"cmd package has-feature {COMPASS_FEATURE}: exit {returncode} and {answer!r}, "
                  "expected exit 0 and 'true' or exit 1 and 'false'")


def parse_hal_pids(text: str) -> list[int]:
    """The pids in `ps -A -o PID,ARGS` lines that run the sensors HAL."""
    pids = []
    for line in text.splitlines():
        fields = line.split(None, 1)
        if len(fields) == 2 and fields[0].isdigit() and SENSORS_HAL in fields[1]:
            pids.append(int(fields[0]))
    return pids


def parse_start_time(stat: str) -> int:
    """The start time, in clock ticks since boot, from /proc/<pid>/stat (field 22; the name may hold spaces)."""
    try:
        return int(stat.rsplit(")", 1)[1].split()[19])
    except (IndexError, ValueError) as problem:
        raise Failure(f"not a /proc stat line: {stat.strip()[:120]!r}") from problem


def sensors_hal(guest: Guest) -> tuple[int, int]:
    """The sensors HAL's identity, its pid and start time: unchanged across a step means it did not restart."""
    pids = parse_hal_pids(guest.shell(f"ps -A -o PID,ARGS 2>/dev/null | grep -F {SENSORS_HAL} | grep -v -F grep; true"))
    if len(pids) != 1:
        raise Failure(f"expected one {SENSORS_HAL} process, found {pids}")
    return pids[0], parse_start_time(guest.shell(f"cat /proc/{pids[0]}/stat"))


def parse_features(text: str) -> set[str]:
    """`pm list features`: the names after "feature:"."""
    return {line[len("feature:"):].split("=")[0].strip() for line in text.splitlines() if line.startswith("feature:")}


def parse_probe_report(text: str) -> dict:
    """The probe's compass.json; a ValueError for text that is not a report."""
    try:
        report = json.loads(text)
    except json.JSONDecodeError as problem:
        raise ValueError(f"no report: {text.strip()[:200]!r}") from problem
    if not isinstance(report, dict) or "magnetometer" not in report:
        raise ValueError(f"not a compass report: {text.strip()[:200]!r}")
    return report


def stream_interval(period_us: int) -> float:
    """Seconds between the records of a sensor the guest asked for at this period: half of it, at least 10 ms."""
    return max(MINIMUM_STREAM_INTERVAL, period_us / 2_000_000)


class Pacer:
    """When each sensor's next record is due, from the latest frame's periods. A sensor that stops being in use
    forgets its schedule; one whose period shortens is due within the new interval; a late look sends once, never a
    burst to catch up."""

    def __init__(self) -> None:
        self.due: dict[int, float] = {}

    def sensors_due(self, now: float, frame: Frame) -> list[int]:
        wanted = ((ACCELEROMETER_TYPE, frame.accelerometer_active, frame.accelerometer_us),
                  (GYROSCOPE_TYPE, frame.gyroscope_active, frame.gyroscope_us),
                  (MAGNETIC_FIELD_TYPE, frame.magnetometer_active, frame.magnetometer_us))
        sending = []
        for kind, active, period_us in wanted:
            if not active or period_us is None:
                self.due.pop(kind, None)
                continue
            interval = stream_interval(period_us)
            earlier = self.due.get(kind)
            scheduled = 0.0 if earlier is None else min(earlier, now + interval)
            if now < scheduled:
                self.due[kind] = scheduled  # a shorter period brings the deadline in, and it stays there
                continue
            sending.append(kind)
            # Keep the rate while the looks are on time; start again from now after a late one.
            self.due[kind] = scheduled + interval if scheduled > now - interval and earlier is not None else now + interval
        return sending


# ---- The host's end of the connection.

class Host:
    """The host application's end of the stream: one connection at a time, with a thread that reads the frames.

    ``stream`` makes a thread send the sensors' records, 50 a second, while the latest frame says a sensor is
    in use: the application's rule, which sends a magnetic field record only while the frame has bit 3 set."""

    def __init__(self, port: int) -> None:
        self.port = port
        self.connection: socket.socket | None = None
        self.condition = threading.Condition()
        self.sending = threading.Lock()
        self.frames: list[Frame] = []
        self.bad: list[bytes] = []
        self.closed = False
        #: Set before sending what makes the guest end the connection; cleared by the next ``connect``.
        self.closure_expected = False
        #: What went wrong on any connection of the run, never cleared: a frame that is no frame, or the guest
        #: ending a connection nobody asked it to. ``raise_faults`` fails the step that finds any.
        self.faults: list[str] = []
        self.reader: threading.Thread | None = None
        self.field: tuple[float, float, float] | None = None
        self.status = 3
        self.gravity = (0.0, 0.0, GRAVITY)
        self.stopped = threading.Event()
        self.streamer: threading.Thread | None = None

    def connect(self) -> None:
        self.close()
        connection = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        connection.settimeout(SEND_SECONDS)
        with self.condition:
            self.connection = connection
            self.frames = []
            self.bad = []
            self.closed = False
            self.closure_expected = False
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
                            self.faults.append(f"a frame with no known magic: {buffer[:8].hex(' ')}")
                            self.condition.notify_all()
                        buffer = b""
                        break
                    if len(buffer) < length:
                        break
                    raw, buffer = buffer[:length], buffer[length:]
                    try:
                        frame = parse_frame(raw)
                    except ValueError as problem:
                        with self.condition:
                            self.bad.append(raw)
                            self.faults.append(str(problem))
                            self.condition.notify_all()
                        continue
                    with self.condition:
                        if self.connection is connection:
                            self.frames.append(frame)
                            self.condition.notify_all()
        except OSError:
            pass
        with self.condition:
            if self.connection is connection:
                self.closed = True
                if not self.closure_expected:
                    self.faults.append("the guest ended the connection")
            self.condition.notify_all()

    def raise_faults(self) -> None:
        with self.condition:
            faults = list(self.faults)
        if faults:
            raise Failure("the connection to the guest went wrong: " + "; ".join(faults[:4]))

    def latest(self) -> Frame | None:
        with self.condition:
            return self.frames[-1] if self.frames else None

    def wait_frame(self, seconds: float, predicate: Callable[[Frame], bool] = lambda frame: True,
                   after: int = 0) -> Frame | None:
        """The first frame that satisfies the predicate, or None. It looks among the frames received since
        connecting, from index ``after`` on (``len(host.frames)`` when a step began)."""
        deadline = time.monotonic() + seconds
        with self.condition:
            while True:
                if self.faults:
                    raise Failure("the connection to the guest went wrong: " + "; ".join(self.faults[:4]))
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

    def stream(self, field: tuple[float, float, float] | None, status: int = 3,
               gravity: tuple[float, float, float] = (0.0, 0.0, GRAVITY)) -> None:
        """Streams the field (None for none) with its status, and the gravity the accelerometer reads."""
        self.field, self.status, self.gravity = field, status, gravity
        if self.streamer is None:
            self.streamer = threading.Thread(target=self._stream, daemon=True)
            self.streamer.start()

    def _stream(self) -> None:
        pacer = Pacer()
        while not self.stopped.is_set():
            frame = self.latest()
            if frame is not None and not self.closed:
                try:
                    for kind in pacer.sensors_due(time.monotonic(), frame):
                        if kind == ACCELEROMETER_TYPE:
                            self.send(sensor_record(ACCELEROMETER_TYPE, *self.gravity))
                        elif kind == GYROSCOPE_TYPE:
                            self.send(sensor_record(GYROSCOPE_TYPE, 0.0, 0.0, 0.0))
                        elif self.field is not None:
                            self.send(magnetic_record(*self.field, self.status))
                except (OSError, Failure):
                    pass
            time.sleep(STREAM_TICK)

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
        self.stopped.set()
        self.close()


# ---- Checks. Each returns a line for the report, or raises Failure.

def launch_state(guest: Guest, compass: bool, with_orientation: bool, argument: bool | None = None) -> str:
    """What the launch's argument decided (``argument``: whether the launch carried it, which is ``compass``
    unless an older release is given it): the property, the sensors listed (the sub-HAL's own log line says how
    many), the magnetometer's description, the compass feature, and no SELinux denial about the property."""
    notes = []
    argument = compass if argument is None else argument
    value = guest.shell(f"getprop {COMPASS_PROPERTY}").strip()
    if value != ("1" if argument else ""):
        raise Failure(f"getprop {COMPASS_PROPERTY} reads {value!r}, expected {'1' if argument else 'nothing'}")
    dump = guest.shell("dumpsys sensorservice")
    sensors = orient.parse_sensor_state(dump)
    for name, kind in (("Host Accelerometer", 1), ("Host Gyroscope", 4)):
        if sensors.listed.get(name) != kind:
            raise Failure(f"the sensor service lists {sensors.listed}, not {name} (type {kind})")
    if with_orientation:
        if sensors.listed.get(orient.SENSOR_NAME) != orient.ORIENTATION_TYPE:
            raise Failure(f"the sensor service lists {sensors.listed}, not {orient.SENSOR_NAME} "
                          f"(type {orient.ORIENTATION_TYPE}) for a launch that carried its argument")
    elif orient.SENSOR_NAME in sensors.listed or orient.ORIENTATION_TYPE in sensors.listed.values():
        raise Failure(f"the sensor service lists a device orientation sensor (by name or type {orient.ORIENTATION_TYPE}) "
                      f"for a launch without its argument: {sensors.listed}")
    if compass:
        if sensors.listed.get(MAGNETOMETER) != MAGNETIC_FIELD_TYPE:
            raise Failure(f"the sensor service lists {sensors.listed}, not {MAGNETOMETER} (type 2)")
        description = orient.sensor_description(dump, MAGNETOMETER) or ""
        if "continuous" not in description or "non-wakeUp" not in description:
            raise Failure(f"{MAGNETOMETER} is not continuous and non-wake-up: {description!r}")
        # The delays the sub-HAL lists: 1 s at the slowest, 20 ms at the fastest.
        rates = re.search(r"minRate=([\d.]+)Hz \| maxRate=([\d.]+)Hz", description)
        if rates is None or abs(float(rates[1]) - 1.0) > 0.01 or abs(float(rates[2]) - 50.0) > 0.01:
            raise Failure(f"{MAGNETOMETER} is not listed at 1 Hz to 50 Hz: {description!r}")
        notes.append("listed (type 2), continuous, non-wakeUp, 1 Hz to 50 Hz")
        # The service adds these only when all three sensors are listed (the geomagnetic one with two).
        missing = [name for name, kind in (("rotation vector", ROTATION_VECTOR_TYPE),
                                           ("geomagnetic rotation vector", GEOMAGNETIC_ROTATION_VECTOR_TYPE))
                   if kind not in sensors.listed.values()]
        if missing:
            raise Failure(f"the sensor service lists no {' and no '.join(missing)} with the magnetometer: "
                          f"{sensors.listed}")
        notes.append("rotation vector and geomagnetic rotation vector listed")
    else:
        if MAGNETOMETER in sensors.listed or MAGNETIC_FIELD_TYPE in sensors.listed.values():
            raise Failure(f"the sensor service lists a magnetic field sensor without the argument: {sensors.listed}")
        fused = {name: kind for name, kind in sensors.listed.items()
                 if kind in (ROTATION_VECTOR_TYPE, GEOMAGNETIC_ROTATION_VECTOR_TYPE)}
        if fused:
            raise Failure(f"the sensor service lists sensors that need a magnetometer without one: {fused}")
        notes.append("no magnetic field sensor, no rotation vector")
    features = parse_features(guest.shell("pm list features"))
    # The package manager's own answer: exit 0 and "true", or exit 1 and "false" for a feature the device lacks.
    asked = guest.adb_run("shell", f"cmd package has-feature {COMPASS_FEATURE}", timeout=60)
    answer = has_feature_answer(asked.returncode, asked.stdout)
    if (COMPASS_FEATURE in features) != compass or answer != compass:
        raise Failure(f"{COMPASS_FEATURE}: pm list features says {COMPASS_FEATURE in features}, has-feature says "
                      f"{answer!r}, expected {compass}")
    notes.append(f"{COMPASS_FEATURE} {'declared' if compass else 'not declared'}")
    log = guest.early_log + "\n" + guest.shell(
        "logcat -d -b all 2>/dev/null | grep -E 'Andarium motion: ro.boot|avc: +denied'; true")
    said = HAL_LOG_LINE.findall(log)
    if said:
        orientation, count, said_compass = said[-1]
        expected = 2 + int(with_orientation) + int(compass)
        if int(count) != expected or (said_compass and said_compass != ("1" if compass else "0")):
            raise Failure(f"the sub-HAL's log line says orientation={orientation}, {count} sensors, "
                          f"compass={said_compass or '(not said)'}; expected {expected} sensors, compass={int(compass)}")
        notes.append(f"log line {count} sensors, compass={said_compass or 'not said'}")
    else:
        notes.append("log line not found in the buffer")
    denials = orient.sensors_hal_denials(log)
    if denials:
        raise Failure(f"SELinux denials about the property or the sensors HAL: {denials[:3]}")
    if sensors.active.get(MAGNETOMETER):
        raise Failure(f"the magnetometer is held at boot: {sensors.active}")
    return f"{COMPASS_PROPERTY}={value or '(unset)'}; " + "; ".join(notes)


def first_frame(host: Host, compass: bool, with_orientation: bool) -> str:
    """The first frame of a connection says what the image is: "ANC3" for one with the magnetometer, otherwise
    "ANC1" or, with the orientation sensor, "ANC2"; the magnetometer is idle."""
    host.connect()
    frame = host.wait_frame(FRAME_SECONDS)
    if frame is None:
        raise Failure("the guest sent no frame")
    expected = 3 if compass else 2 if with_orientation else 1
    if frame.version != expected:
        raise Failure(f"the first frame is version {frame.version}, expected {expected}")
    if compass and frame.orientation_listed != with_orientation:
        raise Failure(f"the frame says the orientation sensor is {'listed' if frame.orientation_listed else 'absent'}")
    if frame.magnetometer_active:
        raise Failure(f"the magnetometer is in use with nothing registered: {frame}")
    if compass and frame.magnetometer_us != 20_000:
        raise Failure(f"the magnetometer's idle period is {frame.magnetometer_us} us, not 20000")
    return f"ANC{frame.version}, mask {frame.mask:#04x}, the magnetometer idle"


def nothing_holds_the_magnetometer(guest: Guest, host: Host, label: str) -> None:
    deadline = time.monotonic() + IDLE_SECONDS
    time.sleep(4)
    while True:
        guest.alive()
        held = guest.sensors().active.get(MAGNETOMETER, 0)
        frame = host.latest()
        if not held and not (frame and frame.magnetometer_active):
            return
        if time.monotonic() >= deadline:
            raise Failure(f"{label}: the magnetometer is held ({held} connections, frame {frame})")
        time.sleep(1.0)


def idle_demand(guest: Guest, host: Host) -> str:
    """No client of the magnetometer at the home screen, in Settings, in the camera application or when the
    screen goes off and on."""
    surfaces = []
    guest.shell("am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null 2>&1; true")
    nothing_holds_the_magnetometer(guest, host, "home screen")
    surfaces.append("home")
    guest.settings_app()
    nothing_holds_the_magnetometer(guest, host, "Settings")
    surfaces.append("Settings")
    # Aperture, the camera application, if the image has one; a launch that finds none is not a failure.
    camera = guest.shell("am start -W -a android.media.action.STILL_IMAGE_CAMERA 2>&1; true")
    if "Error" not in camera:
        nothing_holds_the_magnetometer(guest, host, "the camera application")
        surfaces.append("camera")
    guest.shell("am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null 2>&1; true")
    guest.shell("input keyevent KEYCODE_SLEEP; true")
    time.sleep(3)
    nothing_holds_the_magnetometer(guest, host, "the screen off")
    guest.shell("input keyevent KEYCODE_WAKEUP; true")
    nothing_holds_the_magnetometer(guest, host, "the lock screen")
    surfaces.append("lock screen")
    return "nothing holds the magnetometer: " + ", ".join(surfaces)


def install_probe(guest: Guest, apk: pathlib.Path) -> None:
    result = guest.adb_run("install", "-r", str(apk), timeout=180)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing the probe: {result.stdout.strip()[-400:]}")


def start_probe(guest: Guest, magnetometer_us: int = PROBE_REQUESTED_US) -> str:
    """Starts the probe's compass mode and remembers its run id on the guest: every later look at its report must
    be of this run."""
    run_id = str(int(time.time()))
    output = guest.shell(
        f"am start -W -n {PROBE_COMPONENT} -e mode compass -e run_id {run_id} --ei seconds 240 "
        f"--ei mag_us {magnetometer_us}")
    if "Error" in output:
        raise Failure(f"starting the probe: {output.strip()[-300:]}")
    guest.probe_run_id = run_id  # type: ignore[attr-defined]
    return run_id


def probe_report(guest: Guest) -> dict | None:
    result = guest.adb_run("shell", f"run-as {PROBE_PACKAGE} cat files/compass.json", timeout=60)
    if result.returncode != 0:
        return None
    try:
        return parse_probe_report(result.stdout)
    except ValueError:
        return None


def wait_field(guest: Guest, expected: tuple[float, float, float], label: str, *, heading: float | None = None,
               status: int | None = None, callback: bool = False, newer: tuple[int, int] | None = None,
               seconds: float = FIELD_SECONDS) -> dict:
    """The listener's last field is what the host sent (to within half of the service's quantum for the
    resolution the probe reports), and the azimuth and the accuracy are what they should be. ``callback``: the last
    onAccuracyChanged call was with ``status``. ``newer``: the report holds a sample after this (count, timestamp)
    pair. A report of another run of the probe is never taken."""
    run_id = getattr(guest, "probe_run_id", None)

    def check() -> str | None:
        report = probe_report(guest)
        if report is None:
            return "no report from the probe"
        if run_id is not None and report.get("run_id") != run_id:
            return f"the report is of run {report.get('run_id')!r}, not the probe's {run_id!r}"
        if not report.get("valid"):
            return "the probe says it is not valid (a sensor missing or an event out of order)"
        magnetometer = report["magnetometer"]
        resolution = magnetometer.get("resolution")
        if not finite_number(resolution) or resolution <= 0:
            return f"the probe reports the resolution as {resolution!r}"
        if newer is not None and not (magnetometer.get("count", 0) > newer[0] and magnetometer.get("last_ns", 0) > newer[1]):
            return (f"no new sample: count {magnetometer.get('count')}, timestamp {magnetometer.get('last_ns')}, "
                    f"after count {newer[0]}, timestamp {newer[1]}")
        latest = magnetometer.get("latest", [])
        if len(latest) != 3 or not all(finite_number(value) for value in latest):
            return f"the listener has {latest}, not three numbers"
        if any(abs(a - b) > value_tolerance(resolution) for a, b in zip(latest, expected)):
            return f"the listener has {latest}, not {list(expected)}"
        if status is not None and magnetometer.get("accuracy") != status:
            return f"accuracy {magnetometer.get('accuracy')}, not {status}"
        if callback:
            calls = magnetometer.get("accuracy_calls") or []
            if not calls or calls[-1] != status:
                return f"onAccuracyChanged was last called with {calls[-1] if calls else None}, not {status}"
        if heading is not None:
            found = magnetometer.get("azimuth_deg")
            if not finite_number(found) or angle_difference(found, heading) > AZIMUTH_TOLERANCE:
                return f"azimuth {found}, not {heading}"
        return None

    wait_for = orient.wait_for
    wait_for(label, seconds, check, guest)
    report = probe_report(guest) or {}
    return report.get("magnetometer", {})


def listener_registers(guest: Guest, host: Host, probe: pathlib.Path) -> str:
    """The probe registers a listener; the guest's frame says the magnetometer is in use at the period it asked for."""
    install_probe(guest, probe)
    after = len(host.frames)
    run_id = start_probe(guest)
    frame = host.wait_frame(FRAME_SECONDS, lambda found: found.magnetometer_active, after)
    if frame is None:
        raise Failure("the guest never said the magnetometer is in use after the probe registered a listener")
    if frame.magnetometer_us != PROBE_REQUESTED_US:
        raise Failure(f"the magnetometer's period is {frame.magnetometer_us} us, the probe asked for {PROBE_REQUESTED_US}")
    sensors = guest.sensors()
    if not sensors.active.get(MAGNETOMETER):
        raise Failure(f"the sensor service has no connection to {MAGNETOMETER}: {sensors.active}")
    figures: list[str] = []

    def reported() -> str | None:
        report = probe_report(guest)
        if report is None or report.get("run_id") != run_id:
            return "no report of this run from the probe"
        magnetometer = report["magnetometer"]
        if magnetometer.get("resolution") is None or magnetometer.get("max_range") is None:
            return "the report has no resolution and range"
        figures[:] = [check_figures(magnetometer["resolution"], magnetometer["max_range"])]
        return None

    orient.wait_for("the probe's report of the magnetometer", 30, reported, guest)
    return f"ANC3 mask {frame.mask:#04x}, magnetometer period {frame.magnetometer_us} us; {figures[0]}"


def rotation_vector(guest: Guest, host: Host) -> str:
    """The rotation vector the service makes from the three sensors gets events and settles on one heading for a
    steady field (a device lying face up with its y axis to north); the heading itself is the fusion's. Only a
    report of the probe's own run, valid, with a finite heading and a count that has advanced adds a sample to the
    window: a stream that stops, or reports looked at again, never settles."""
    host.stream(field_at_heading(0.0), 3)
    run_id = getattr(guest, "probe_run_id", None)
    recent: list[float] = []
    counted = [0]
    validated: list[dict] = []

    def check() -> str | None:
        report = probe_report(guest)
        if report is None:
            return "no report from the probe"
        if run_id is not None and report.get("run_id") != run_id:
            return f"the report is of run {report.get('run_id')!r}, not the probe's {run_id!r}"
        if not report.get("valid"):
            return "the probe says it is not valid (a sensor missing or an event out of order)"
        vector = report.get("rotation_vector") or {}
        count, found = vector.get("count"), vector.get("azimuth_deg")
        if not finite_number(count) or count <= 0:
            return "the probe has seen no rotation vector event"
        if not finite_number(found):
            return f"the rotation vector's heading is {found!r}"
        if count > counted[0]:
            counted[0] = count
            recent.append(found)
            del recent[:-SETTLE_SAMPLES]
        elif len(recent) < SETTLE_SAMPLES:
            return f"{len(recent)} samples of the azimuth so far, and the count is not advancing ({count})"
        if len(recent) < SETTLE_SAMPLES:
            return f"{len(recent)} samples of the azimuth so far"
        spread = max(angle_difference(value, recent[-1]) for value in recent)
        if spread > SETTLE_TOLERANCE:
            return f"the azimuth is still moving: {[round(value, 1) for value in recent]}"
        validated[:] = [report]
        return None

    orient.wait_for("the rotation vector", ROTATION_SECONDS, check, guest)
    vector = validated[0]["rotation_vector"]
    found = vector["azimuth_deg"]
    note = "" if angle_difference(found, 0.0) <= 10.0 else " (the field points to 0; the fusion's heading is its own)"
    return f"{vector['count']} events, azimuth settled at {found:.1f}{note}"


def feed(guest: Guest, host: Host, seconds: float) -> str:
    """The host, for a client that brings no field: steady field, accelerometer and gyroscope while the frames
    ask, for ``seconds``."""
    host.connect()
    if host.wait_frame(FRAME_SECONDS) is None:
        raise Failure("the guest sent no frame")
    host.stream(field_at_heading(0.0), 3)
    print(f"feeding for {seconds:.0f} s; adb serial {guest.serial}", flush=True)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        guest.alive()
        host.raise_faults()
        time.sleep(2.0)
    return f"fed {seconds:.0f} s on {guest.serial}"


def four_mounts(guest: Guest, host: Host) -> str:
    """The field of each mount reaches the listener as sent, with the azimuth the platform computes from it."""
    lines = []
    for mount, heading in MOUNTS.items():
        field = mounted(mount, 0.0, HORIZONTAL, VERTICAL)
        gravity = (0.0, 0.0, GRAVITY)
        if angle_difference(azimuth(gravity, field), heading) > 1e-6:
            raise Failure(f"the oracle disagrees with itself for {mount}")
        host.stream(field, 3, gravity)
        found = wait_field(guest, field, f"mount {mount}", heading=heading, status=3)
        lines.append(f"{mount} {tuple(round(v, 1) for v in field)} azimuth {found.get('azimuth_deg'):.1f}")
    return "; ".join(lines)


def compass_points(guest: Guest, host: Host) -> str:
    """A device turned through north, east, south and west reads those azimuths."""
    lines = []
    for name, heading in (("north", 0.0), ("east", 90.0), ("south", 180.0), ("west", 270.0)):
        field = field_at_heading(heading)
        host.stream(field, 3)
        found = wait_field(guest, field, f"heading {name}", heading=heading, status=3)
        lines.append(f"{name} {found.get('azimuth_deg'):.1f}")
    return "; ".join(lines)


def statuses(guest: Guest, host: Host) -> str:
    """Each of the four statuses reaches the listener as its accuracy."""
    field = field_at_heading(0.0)
    for status in (0, 1, 2, 3):
        host.stream(field, status)
        wait_field(guest, field, f"status {status}", status=status, callback=True)
    return "unreliable, low, medium, high reach onAccuracyChanged and SensorEvent.accuracy"


def probe_position(guest: Guest) -> tuple[str | None, int, int]:
    """The probe's run id and its magnetometer's sample count and last timestamp: where the listener stands."""
    report = probe_report(guest)
    magnetometer = (report or {}).get("magnetometer") or {}
    if report is None or not report.get("valid") or not magnetometer.get("count"):
        raise Failure(f"the probe has no valid samples to stand on: {report}")
    run_id = getattr(guest, "probe_run_id", None)
    if run_id is not None and report.get("run_id") != run_id:
        raise Failure(f"the probe's report is of run {report.get('run_id')!r}, not {run_id!r}")
    return report.get("run_id"), magnetometer["count"], magnetometer["last_ns"]


def system_server_identity(guest: Guest) -> tuple[int, int]:
    """system_server's pid and start time, as the sensors HAL's identity is read."""
    pids = guest.system_server().split()
    pid = pids[0] if pids else ""
    if not pid.isdigit():
        raise Failure(f"system_server is not running: pidof says {pid!r}")
    return int(pid), parse_start_time(guest.shell(f"cat /proc/{pid}/stat"))


def unchanged(guest: Guest, server: tuple[int, int], hal: tuple[int, int], where: str) -> None:
    """Neither system_server nor the sensors HAL restarted: the same pid and start time of each."""
    if system_server_identity(guest) != server:
        raise Failure(f"system_server restarted {where}: it was (pid, start time) {server}, it is "
                      f"{system_server_identity(guest)}")
    if sensors_hal(guest) != hal:
        raise Failure(f"the sensors HAL restarted {where}: it was (pid, start time) {hal}, it is {sensors_hal(guest)}")


def refused_record(guest: Guest, host: Host) -> str:
    """A record the guest refuses ends the connection and not the guest: the next connection says the magnetometer
    is in use and a different field then reaches the same listener as a new sample, with neither system_server nor
    the sensors HAL restarted (the same pid and start time of each)."""
    server = system_server_identity(guest)
    hal = sensors_hal(guest)
    _, count, last_ns = probe_position(guest)
    host.stream(None)
    host.closure_expected = True
    host.send(magnetic_record(0.0, HORIZONTAL, VERTICAL, 4))
    if not host.closed_by_guest(orient.CLOSE_SECONDS):
        raise Failure("the guest kept the connection after a record of status 4")
    host.connect()
    frame = host.wait_frame(FRAME_SECONDS)
    if frame is None or not frame.magnetometer_active:
        raise Failure(f"the first frame of the next connection does not say the magnetometer is in use: {frame}")
    field = field_at_heading(RECOVERY_HEADING)
    host.stream(field, 3)
    found = wait_field(guest, field, "after the connection was ended", status=3, newer=(count, last_ns))
    unchanged(guest, server, hal, "after the record was refused")
    return (f"ended by a status 4; the next connection says the magnetometer is in use and a new field reaches the "
            f"listener (sample {found.get('count')} after {count}); system_server and the sensors HAL (pid {hal[0]}) "
            "did not restart")


def listener_leaves(guest: Guest, host: Host) -> str:
    after = len(host.frames)
    guest.shell(f"am force-stop {PROBE_PACKAGE}")
    frame = host.wait_frame(30, lambda found: not found.magnetometer_active, after)
    if frame is None:
        raise Failure("the guest never said the magnetometer idle after the listener's end")
    host.stream(None)
    nothing_holds_the_magnetometer(guest, host, "after the probe")
    return "the magnetometer is idle again"


def older_wire(guest: Guest, host: Host, with_orientation: bool) -> str:
    """An image without the sensor: no "ANC3", a magnetic field record ends the connection, the guest stays up."""
    server = system_server_identity(guest)
    hal = sensors_hal(guest)
    host.stream(None)
    host.connect()
    time.sleep(3)
    host.raise_faults()
    frames = list(host.frames)
    if not frames or any(frame.version == 3 for frame in frames):
        raise Failure(f"the frames of an image without the sensor are {frames}")
    expected = 2 if with_orientation else 1
    if any(frame.version != expected for frame in frames):
        raise Failure(f"the frames are not all version {expected}: {frames}")
    host.closure_expected = True
    host.send(magnetic_record(0.0, HORIZONTAL, VERTICAL, 3))
    if not host.closed_by_guest(orient.CLOSE_SECONDS):
        raise Failure("the guest kept the connection after a magnetic field record it never listed")
    host.connect()
    again = host.wait_frame(FRAME_SECONDS)
    if again is None or again.version != expected:
        raise Failure(f"the next connection's first frame is {again}")
    unchanged(guest, server, hal, "after the record was refused")
    return (f"ANC{expected} only; a magnetic field record ends the connection, the guest stays up (system_server and "
            f"the sensors HAL, pid {hal[0]}, did not restart)")


# ---- The run.

def passes_argument(options: argparse.Namespace) -> bool:
    """The launch carries the compass argument for a release that lists the sensor, and for an older one if asked."""
    return options.expect == "sensor" or options.with_argument


def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path) -> Guest:
    """The orientation runner's launch, with this runner's arguments: the orientation sensor's if asked for and the
    compass argument for ``--expect sensor``."""
    settings = argparse.Namespace(
        release=options.release, emulator=options.emulator, instance=None, adb_port=options.adb_port,
        cpus=options.cpus, memory=options.memory, boot_timeout=options.boot_timeout,
        no_argument=not options.with_orientation,
        extra_arguments=[COMPASS_ARGUMENT] if passes_argument(options) else [])
    return orient.start_guest(settings, adb, private, early=None, holder={}, provision=True)


def checked(host: Host, function: Callable[[], str]) -> str:
    """A step's note, unless the connection went wrong during it: a frame that is no frame, or the guest ending a
    connection nobody asked it to, fails the step even when what it waited for came later."""
    try:
        note = function()
    except Failure as problem:
        if host.faults:
            raise Failure(f"{problem}; and the connection went wrong: {'; '.join(host.faults[:4])}") from problem
        raise
    host.raise_faults()
    return note


def run(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    compass = options.expect == "sensor"
    print(f"expect {options.expect}: booting {'with' if passes_argument(options) else 'without'} {COMPASS_ARGUMENT}"
          f"{' and the orientation argument' if options.with_orientation else ''}", flush=True)
    guest = start_guest(options, adb, private / "guest")
    host = Host(guest.forward_port)

    def step(name: str, function: Callable[[], str]) -> bool:
        return report.run(name, lambda: checked(host, function))

    try:
        report.run("launch state", lambda: launch_state(
            guest, compass, options.with_orientation, passes_argument(options)))
        if options.launch_only:
            return
        if options.feed:
            step("feed", lambda: feed(guest, host, options.feed))
            return
        if not compass:
            step("an image without the sensor", lambda: older_wire(guest, host, options.with_orientation))
            return
        if step("first frame", lambda: first_frame(host, True, options.with_orientation)):
            step("idle demand", lambda: idle_demand(guest, host))
            if step("a listener registers", lambda: listener_registers(guest, host, options.probe_apk)):
                step("the rotation vector", lambda: rotation_vector(guest, host))
                step("the four mounts", lambda: four_mounts(guest, host))
                step("north, east, south and west", lambda: compass_points(guest, host))
                step("each accuracy", lambda: statuses(guest, host))
                step("a refused record", lambda: refused_record(guest, host))
                step("the listener leaves", lambda: listener_leaves(guest, host))
    finally:
        host.stop()
        orient.stop_guest(adb, guest)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--expect", choices=("sensor", "none"), default="sensor",
                        help="sensor: the release lists the magnetometer when asked; none: it does not")
    parser.add_argument("--with-argument", action="store_true",
                        help="with --expect none, pass androidboot.kernarium.compass=1 anyway: an older image "
                        "given what a newer application always passes")
    parser.add_argument("--with-orientation", action="store_true",
                        help="also pass androidboot.kernarium.orientation=1, as a phone does")
    parser.add_argument("--probe-apk", type=pathlib.Path,
                        help="AndariumMotionProbe.apk (tests/integration/android/motion/build-apk.sh); "
                        "required for --expect sensor unless --launch-only")
    parser.add_argument("--launch-only", action="store_true",
                        help="stop the guest once the launch's state is checked (the first thing to run on a new image)")
    parser.add_argument("--feed", type=int, default=0, metavar="SECONDS",
                        help="keep the guest up for this long as a host that sends a steady field, accelerometer and "
                        "gyroscope while the frames ask, and print the adb serial, for a client from another shell; "
                        "no probe needed")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int,
                        default=orient.slot_ports.slot_port(orient.slot_ports.ADB_OFFSET, orient.DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {orient.DEFAULT_ADB_PORT})")
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
    if options.feed < 0:
        print("--feed takes a positive number of seconds", file=sys.stderr)
        return 2
    if options.expect == "sensor" and not options.launch_only and not options.feed and (
            options.probe_apk is None or not options.probe_apk.is_file()):
        print("--expect sensor needs --probe-apk (tests/integration/android/motion/build-apk.sh)", file=sys.stderr)
        return 2
    adb = orient.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-compass-"))
    report = Report()
    try:
        run(options, adb, private, report)
        print("\n".join(f"{status:4} {name}: {detail}" for name, status, detail in report.rows))
        print("FAIL" if report.failed else "PASS")
        return 1 if report.failed else 0
    except (Failure, orient.boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        for log in sorted(private.glob("*/emulator.log")):
            print(f"{log.parent.name} launcher log tail:\n"
                  + "\n".join(log.read_text(errors="replace").splitlines()[-30:]), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
