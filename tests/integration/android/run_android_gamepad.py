#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's gamepad relay.

The guest is the release on a fresh data disk (or on the instance named by --instance), started with the macOS
launcher and ``--vsock-forward=127.0.0.1:0-5558``, so that the runner plays the host application: it creates
controllers over the forward, sends their reports and answers the drivers' requests. The contract is
android/lineage-24/README.md ("Game controllers"), the wire is overlay/gamepad/gamepad_protocol.hpp
(gamepad_wire.py), and the controllers are gamepad_families.py: a generic gamepad, an Xbox controller and a
DualSense, so that hid-generic, hid-microsoft and hid-playstation each bind one. The probe app is
tests/integration/android/gamepad (--probe-apk names its build).

The checks, each reported on its own so that one boot shows every difference:

- the relay runs as its own user and domain, answers a host with HELLO, and creates a controller of each family;
- Android sees each controller as the family says: vendor and product, gamepad and joystick sources, its keys,
  motion ranges, vibrators, battery and lights, and the key layout file the input service chose;
- every button reaches the probe app as the key code the layout names, and sticks, triggers and the hat as the
  axis values the reports carry;
- vibration reaches the host as the driver's output report (strong and weak motor, and the stop after the
  duration), and the lightbar and the player lights likewise;
- the DualSense's feature requests at probe time are answered, and its start-up output (lightbar, player LEDs) comes;
- four controllers hot plug and unplug, and the listener sees each;
- a second host replaces the first, a host that drops takes its controllers with it, frames that break the protocol
  close the connection, and killing the relay leads to a restart, a reconnect and working controllers again;
- the cost: the relay's CPU and the whole guest's per report, its PSS, the latency from the host's send to the app's
  event, the relay's own counters, and no SELinux denial of the relay.

A feature that no tool of the guest reaches is not tested: SET_REPORT has no sender (hidraw is not exposed), and
DualShock 4 is covered by CTS only.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest is
reached through the shared adb server, always named with ``-s``.

The runner is opt-in: it needs the macos-android17 executable, an installed release, an adb client and the probe APK.
usage: run_android_gamepad.py --emulator uml-darwin-android17 --probe-apk AndariumGamepadProbe.apk [--release DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import importlib.util
import json
import os
import pathlib
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(HERE))
import gamepad_families as families_module  # noqa: E402
import gamepad_wire as wire  # noqa: E402
import slot_ports  # noqa: E402
from gamepad_families import Family, State  # noqa: E402

SPEC = importlib.util.spec_from_file_location("boot_android", REPOSITORY / "tools" / "boot_android.py")
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)

DEFAULT_ADB_PORT = 5579
FORWARD = re.compile(r"vsock forward (\S+):(\d+) -> guest port (\d+)")
FORWARD_SECONDS = 60
COMMAND_SECONDS = 120
SETTLE_SECONDS = 60
HELLO_SECONDS = 10
CONNECT_SECONDS = 90
PROBE_SECONDS = 30
DEVICE_SECONDS = 40
EVENT_SECONDS = 15
OUTPUT_SECONDS = 15
RESTART_SECONDS = 90
STEP_SECONDS = 0.12
PROBE_PACKAGE = "org.example.andarium.gamepadprobe"
PROBE_ACTIVITY = f"{PROBE_PACKAGE}/.GamepadActivity"
PROBE_FOCUS = f"{PROBE_PACKAGE}/{PROBE_PACKAGE}.GamepadActivity"
PROBE_ACTION = f"{PROBE_PACKAGE}.COMMAND"
RELAY_NAME = "kernarium_gamepad"
RELAY_LOG_TAG = "kernarium.gamepad"
SOURCE_GAMEPAD = 0x00000401
SOURCE_JOYSTICK = 0x01000010
AXIS_NAMES = ("AXIS_X", "AXIS_Y", "AXIS_Z", "AXIS_RZ", "AXIS_LTRIGGER", "AXIS_RTRIGGER", "AXIS_HAT_X", "AXIS_HAT_Y")
AXIS_TOLERANCE = 0.02
RUMBLE_TOLERANCE = {"xbox": 3, "dualsense": 3}
LIGHT_TOLERANCE = 4
# The generous ends of a run that is not a benchmark: a slow host must not fail it, a stuck path must.
PSS_CEILING_KB = 64 * 1024
LATENCY_MEDIAN_CEILING_MS = 250.0
LATENCY_MAXIMUM_CEILING_MS = 2000.0
LOSS_CEILING = 0.01
CLOCK_TICK_SECONDS = 0.01  # the kernel's USER_HZ is 100 whatever its own tick rate
CLOCK_READINGS = 7  # reads of the guest's wall clock for its offset from the host's
IDENTITY_SECONDS = 20  # how long a controller's identity is read again before it fails
EVENT_SECONDS = 15  # how long the events of one step are read again before the sequence fails


class Failure(RuntimeError):
    pass


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

@dataclass
class ProbeDevice:
    id: int
    name: str
    vendor: int
    product: int
    sources: int
    controller_number: int
    descriptor: str
    keys: set[int]
    ranges: dict[str, dict]
    vibrators: int
    battery_present: bool
    battery_capacity: float | None
    lights: list[dict]


@dataclass
class ProbeEvent:
    kind: str
    device: int
    code: int = 0
    action: str = ""
    repeat: int = 0
    axes: dict[str, float] = field(default_factory=dict)
    kernel_ms: int = 0
    received_ms: int = 0


@dataclass
class ProbeState:
    nonce: str
    failure: str
    wall_ms: int
    key_count: int
    motion_samples: int
    devices: list[ProbeDevice]
    events: list[ProbeEvent]
    listener: list[tuple[str, int, int]]

    def device_named(self, name: str) -> ProbeDevice | None:
        return next((device for device in self.devices if device.name == name), None)


def parse_probe_state(text: str) -> ProbeState:
    """The probe app's files/state.json."""
    try:
        root = json.loads(text)
        devices = [
            ProbeDevice(
                id=item["id"], name=item["name"], vendor=item["vendor"], product=item["product"],
                sources=item["sources"], controller_number=item["controllerNumber"], descriptor=item["descriptor"],
                keys=set(item["keys"]), ranges={entry["axis"]: entry for entry in item["ranges"]},
                vibrators=item["vibrators"], battery_present=item["battery"]["present"],
                battery_capacity=item["battery"]["capacity"], lights=item["lights"])
            for item in root["devices"]]
        events = []
        for item in root["events"]:
            if item["kind"] == "key":
                events.append(ProbeEvent("key", item["device"], code=item["code"], action=item["action"],
                                         repeat=item["repeat"], kernel_ms=item["kernelMs"],
                                         received_ms=item["receivedMs"]))
            else:
                events.append(ProbeEvent("motion", item["device"], axes={
                    name: item[name] for name in ("x", "y", "z", "rz", "ltrigger", "rtrigger", "hat_x", "hat_y")},
                    kernel_ms=item["kernelMs"], received_ms=item["receivedMs"]))
        return ProbeState(
            nonce=root["nonce"], failure=root["failure"], wall_ms=root["wallMs"], key_count=root["keyCount"],
            motion_samples=root["motionSampleCount"], devices=devices, events=events,
            listener=[(item["kind"], item["id"], item["ms"]) for item in root["listener"]])
    except (ValueError, KeyError, TypeError) as problem:
        raise Failure(f"the probe's state file does not parse: {problem}: {text[:200]!r}") from problem


@dataclass
class InputDumpDevice:
    id: int
    name: str
    bus: int | None = None
    vendor: int | None = None
    product: int | None = None
    version: int | None = None
    key_layout: str | None = None
    path: str | None = None


DEVICE_HEADER = re.compile(r"^\s+(-?\d+): (.+?)\s*$")
# The Event Hub prints a device's classes as hex on older releases and as names (KEYBOARD | GAMEPAD) on Android 17.
CLASSES_LINE = re.compile(r"^\s+Classes: (?:0x[0-9a-fA-F]+|[A-Z][A-Z0-9_]*(?: \| [A-Z][A-Z0-9_]*)*)\s*$")
PROPERTY = re.compile(r"^\s+(\w+): ?(.*?)\s*$")
IDENTIFIER = re.compile(
    r"bus=0x([0-9a-fA-F]+), vendor=0x([0-9a-fA-F]+), product=0x([0-9a-fA-F]+), version=0x([0-9a-fA-F]+)")


def parse_input_dump(text: str) -> list[InputDumpDevice]:
    """`dumpsys input`: the event hub's devices (each header is followed by its Classes line) with their
    identifier, key layout file and path."""
    devices: list[InputDumpDevice] = []
    current: InputDumpDevice | None = None
    lines = text.splitlines()
    for index, line in enumerate(lines):
        header = DEVICE_HEADER.match(line)
        if header and index + 1 < len(lines) and CLASSES_LINE.match(lines[index + 1]):
            current = InputDumpDevice(int(header[1]), header[2])
            devices.append(current)
            continue
        if current is None:
            continue
        prop = PROPERTY.match(line)
        if not prop:
            continue
        key, value = prop[1], prop[2]
        if key == "Identifier" and current.bus is None:
            found = IDENTIFIER.search(value)
            if found:
                current.bus, current.vendor, current.product, current.version = (int(g, 16) for g in found.groups())
        elif key == "KeyLayoutFile" and current.key_layout is None:
            current.key_layout = value or None
        elif key == "Path" and current.path is None:
            current.path = value
    return devices


FOCUS = re.compile(r"mCurrentFocus=Window\{\S+ \S+ ([^\s}]+)\}")


def parse_focus(text: str) -> str | None:
    """`dumpsys window`: the component of the focused window."""
    found = FOCUS.search(text)
    return found[1] if found else None


def parse_pid(text: str) -> int | None:
    """`pidof`: the first pid, or None when it prints none."""
    words = text.split()
    return int(words[0]) if words and words[0].isdecimal() else None


def parse_ps_label(text: str, name: str) -> tuple[str, str] | None:
    """`ps -A -Z`: the user and the SELinux label of the process called `name` (the columns come from the header)."""
    columns: list[str] = []
    for line in text.splitlines():
        words = line.split()
        if not words:
            continue
        if "LABEL" in words and "USER" in words and "NAME" in words:
            columns = words
            continue
        if columns and len(words) >= len(columns) and words[-1] == name:
            return words[columns.index("USER")], words[columns.index("LABEL")]
    return None


def parse_meminfo_pss(text: str) -> int | None:
    """`dumpsys meminfo PID`: the total PSS in kB, or None when it prints none."""
    found = re.search(r"TOTAL PSS:\s+(\d+)", text)
    return int(found[1]) if found else None


def parse_smaps_rollup(text: str) -> dict[str, int]:
    """/proc/PID/smaps_rollup: the kB fields by name."""
    return {match[1]: int(match[2]) for match in re.finditer(r"^(\w+):\s+(\d+) kB$", text, re.MULTILINE)}


@dataclass
class CpuSample:
    busy_ticks: int
    total_ticks: int
    relay_ticks: int


def parse_cpu_sample(text: str) -> CpuSample:
    """The first line of /proc/stat followed by /proc/PID/stat: the CPU ticks that were not idle, all of them, and
    the relay's own (user plus system)."""
    lines = text.strip().splitlines()
    if len(lines) < 2 or not lines[0].startswith("cpu "):
        raise Failure(f"no CPU sample in {text[:200]!r}")
    fields = [int(item) for item in lines[0].split()[1:]]
    idle = fields[3] + (fields[4] if len(fields) > 4 else 0)
    total = sum(fields[:8])
    # The command name is in parentheses and may hold spaces; the fields start after the last one.
    rest = lines[1][lines[1].rindex(")") + 2:].split()
    return CpuSample(busy_ticks=total - idle, total_ticks=total, relay_ticks=int(rest[11]) + int(rest[12]))


@dataclass
class RelayCounters:
    frames: int
    created: int
    refused: int
    input_written: int
    input_dropped: int
    input_failed: int
    output: int
    requests: int
    replies: int


DISCONNECT = re.compile(
    r"host disconnected: (\d+) frames, (\d+) devices created \((\d+) refused\), (\d+) input reports written "
    r"\((\d+) dropped, (\d+) failed\), (\d+) output reports, (\d+) requests, (\d+) replies")


def parse_relay_disconnects(text: str) -> list[RelayCounters]:
    """The relay's log line at each disconnect, in order."""
    return [RelayCounters(*(int(group) for group in match.groups())) for match in DISCONNECT.finditer(text)]


def selinux_denials(text: str, domain: str = RELAY_NAME) -> list[str]:
    """The audit lines that deny the relay's domain something, or deny something on it."""
    return [line.strip() for line in text.splitlines()
            if "avc:" in line and "denied" in line
            and (f"scontext=u:r:{domain}:" in line or f"tcontext=u:r:{domain}:" in line)]


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


@dataclass
class LatencySample:
    host_to_kernel_ms: float
    kernel_to_app_ms: float

    @property
    def total_ms(self) -> float:
        return self.host_to_kernel_ms + self.kernel_to_app_ms


def latency_samples(sent_ms: list[float], samples: list[ProbeEvent], clock_offset_ms: float = 0.0) -> list[LatencySample]:
    """The i-th report is the i-th motion sample: every report moves the left stick to the other end, so none
    merges with its neighbour. `clock_offset_ms` is the guest's wall clock minus the host's: the guest takes its
    time from a one-second clock at boot, so the two differ by up to a second and the host's send times are
    moved onto the guest's clock first."""
    return [LatencySample(sample.kernel_ms - (sent + clock_offset_ms), sample.received_ms - sample.kernel_ms)
            for sent, sample in zip(sent_ms, samples)]


def clock_offset_ms(readings: list[tuple[float, float, float]]) -> float:
    """The guest's clock minus the host's, in milliseconds, from (host before, guest reading, host after)
    triples: the reading with the shortest round trip decides, and is wrong by at most half of it."""
    before, guest, after = min(readings, key=lambda reading: reading[2] - reading[0])
    return guest - (before + after) / 2


def per_report_microseconds(idle: tuple[CpuSample, CpuSample], loaded: tuple[CpuSample, CpuSample],
                            idle_seconds: float, loaded_seconds: float, reports: int) -> tuple[float, float]:
    """The guest's CPU per report beyond what it uses idle, and the relay's own, in microseconds."""
    idle_rate = (idle[1].busy_ticks - idle[0].busy_ticks) / idle_seconds
    extra = (loaded[1].busy_ticks - loaded[0].busy_ticks) - idle_rate * loaded_seconds
    relay = loaded[1].relay_ticks - loaded[0].relay_ticks
    return extra * CLOCK_TICK_SECONDS * 1e6 / reports, relay * CLOCK_TICK_SECONDS * 1e6 / reports


# ---- What a family must look like and do.

def identity_problems(device: ProbeDevice, family: Family) -> list[str]:
    problems = []
    identity = family.identity
    if (device.vendor, device.product) != (identity.vendor, identity.product):
        problems.append(f"vendor and product {device.vendor:04x}:{device.product:04x}, "
                        f"not {identity.vendor:04x}:{identity.product:04x}")
    for name, mask in (("gamepad", SOURCE_GAMEPAD), ("joystick", SOURCE_JOYSTICK)):
        if device.sources & mask != mask:
            problems.append(f"sources {device.sources:#x} lack {name}")
    wanted = {families_module.KEYCODES[button] for button in family.buttons
              if button not in families_module.ADVISORY_BUTTONS}
    if wanted - device.keys:
        problems.append(f"no key codes {sorted(wanted - device.keys)}")
    for axis in AXIS_NAMES:
        if axis not in device.ranges:
            problems.append(f"no motion range {axis}")
    if device.vibrators != family.vibrators:
        problems.append(f"{device.vibrators} vibrators, not {family.vibrators}")
    if device.battery_present != family.battery:
        problems.append(f"battery present {device.battery_present}, not {family.battery}")
    if family.battery and device.battery_present:
        capacity = device.battery_capacity
        if capacity is None or abs(capacity - 0.85) > 0.06:
            problems.append(f"battery capacity {capacity}, not about 0.85")
    colour = [light for light in device.lights if light["rgb"]]
    plain = [light for light in device.lights if not light["rgb"]]
    if family.lights and not (colour and plain):
        problems.append(f"lights {device.lights}: want a colour light (the lightbar) and a player id light")
    if not family.lights and device.lights:
        problems.append(f"lights {device.lights}, want none")
    return problems


def key_sequence_problem(events: list[ProbeEvent], device: int, buttons: list[str]) -> str | None:
    """The key events of one device must be a press and a release per button, in order. A button that the system
    may consume (the gamepad Home) may be missing, and a repeat is not an event."""
    wanted: list[tuple[int, str]] = []
    optional: set[int] = set()
    for button in buttons:
        code = families_module.KEYCODES[button]
        wanted += [(code, "down"), (code, "up")]
        if button in families_module.ADVISORY_BUTTONS:
            optional.add(code)
    seen = [(event.code, event.action) for event in events
            if event.kind == "key" and event.device == device and event.repeat == 0]
    position = 0
    for item in seen:
        while position < len(wanted) and wanted[position] != item and wanted[position][0] in optional:
            position += 1
        if position >= len(wanted) or wanted[position] != item:
            return f"unexpected key event {item} after {position} of {len(wanted)} expected; saw {seen}"
        position += 1
    while position < len(wanted) and wanted[position][0] in optional:
        position += 1
    if position != len(wanted):
        return f"only {position} of {len(wanted)} key events arrived; saw {seen}"
    return None


def axes_subsequence_problem(events: list[ProbeEvent], device: int, expected: list[tuple[str, dict[str, float]]],
                             tolerance: float = AXIS_TOLERANCE) -> str | None:
    """Each expected axis vector must appear among the device's motion samples, in order (a state that moves
    nothing yields no sample, and the state before a step may repeat)."""
    samples = [event for event in events if event.kind == "motion" and event.device == device]
    position = 0
    for label, vector in expected:
        while position < len(samples) and not all(
                abs(samples[position].axes[axis] - value) <= tolerance for axis, value in vector.items()):
            position += 1
        if position == len(samples):
            nearest = samples[-1].axes if samples else None
            return f"no motion sample for '{label}' {vector} (the last sample was {nearest}, {len(samples)} in all)"
        position += 1
    return None


def button_steps(family: Family) -> list[tuple[str, State]]:
    steps: list[tuple[str, State]] = []
    for button in family.buttons:
        steps += [(f"{button} down", State(buttons=frozenset({button}))), (f"{button} up", families_module.NEUTRAL)]
    return steps


def axis_steps(family: Family) -> list[tuple[str, State]]:
    neutral = families_module.NEUTRAL
    moves = [
        ("left x low", State(left=(0, 128))), ("left x high", State(left=(255, 128))),
        ("left y low", State(left=(128, 0))), ("left y high", State(left=(128, 255))),
        ("left diagonal", State(left=(192, 64))),
        ("right x low", State(right=(0, 128))), ("right x high", State(right=(255, 128))),
        ("right y low", State(right=(128, 0))), ("right y high", State(right=(128, 255))),
        ("left trigger full", State(left_trigger=255)), ("left trigger half", State(left_trigger=128)),
        ("right trigger full", State(right_trigger=255)), ("right trigger half", State(right_trigger=128)),
    ]
    moves += [(f"hat {direction}", State(hat=direction)) for direction in range(8)]
    steps: list[tuple[str, State]] = []
    for label, state in moves:
        steps += [(label, state), ("neutral", neutral)]
    return steps


# ---- The host's end: one connection, a reader thread that answers the driver's requests.

class HostLink:
    def __init__(self, port: int) -> None:
        self.port = port
        self.sock: socket.socket | None = None
        self.thread: threading.Thread | None = None
        self.lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.frames: list[tuple[float, wire.GuestFrame]] = []
        self.slots: dict[int, Family] = {}
        self.closed = threading.Event()
        self.closed_locally = False
        self.error: str | None = None

    def connect(self) -> wire.Hello:
        """Connects and waits for HELLO. The launcher's forward accepts the connection before the guest does, so a
        guest that is not listening shows as a connection that closes without a word."""
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        self.sock.settimeout(None)
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()
        hello = self.wait_for("HELLO", HELLO_SECONDS, lambda frames: next(
            (frame for _, frame in frames if isinstance(frame, wire.Hello)), None))
        return hello

    def read(self) -> None:
        assert self.sock is not None
        reader = wire.GuestReader()
        while True:
            try:
                data = self.sock.recv(65536)
            except OSError:
                break
            if not data:
                break
            try:
                frames = reader.feed(data)
            except wire.WireError as problem:
                self.error = str(problem)
                break
            now = time.time()
            for frame in frames:
                with self.lock:
                    self.frames.append((now, frame))
                self.answer(frame)
        self.closed.set()

    def answer(self, frame: wire.GuestFrame) -> None:
        """The host's answers to the driver's requests, made at once so that probing never waits out uhid's timeout."""
        try:
            if isinstance(frame, wire.GetReport):
                family = self.slots.get(frame.slot)
                data = (family.feature_reply(frame.slot, frame.number)
                        if family is not None and frame.report_type == wire.FEATURE_REPORT else None)
                self.send(wire.get_report_reply(frame.slot, frame.request_id, data or b"", 0 if data else 5))
            elif isinstance(frame, wire.SetReport):
                self.send(wire.set_report_reply(frame.slot, frame.request_id))
        except OSError:
            pass

    def send(self, data: bytes) -> None:
        assert self.sock is not None
        with self.send_lock:
            self.sock.sendall(data)

    def mark(self) -> int:
        with self.lock:
            return len(self.frames)

    def frames_since(self, mark: int) -> list[tuple[float, wire.GuestFrame]]:
        with self.lock:
            return list(self.frames[mark:])

    def wait_for(self, description: str, seconds: float, find: Callable[[list], object | None],
                 since: int = 0) -> object:
        """Polls the frames since the mark until `find` returns something (not None, not False)."""
        deadline = time.monotonic() + seconds
        while True:
            found = find(self.frames_since(since))
            if found is not None and found is not False:
                return found
            if self.closed.is_set():
                raise Failure(f"{description}: the connection closed first"
                              + (f" ({self.error})" if self.error else ""))
            if time.monotonic() > deadline:
                raise Failure(f"{description}: nothing in {seconds:.0f} s; "
                              f"{len(self.frames_since(since))} frames: {self.frames_since(since)[:8]}")
            time.sleep(0.02)

    def closed_by_guest(self, seconds: float) -> bool:
        return self.closed.wait(seconds) and not self.closed_locally

    def close(self) -> None:
        self.closed_locally = True
        if self.sock is not None:
            with contextlib.suppress(OSError):
                self.sock.shutdown(socket.SHUT_RDWR)
        if self.thread is not None:
            self.thread.join(5)
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    # The controllers.

    def create(self, slot: int, family: Family, seconds: float = OUTPUT_SECONDS) -> int:
        """Creates a controller and returns the CREATED result (0 when the device was made)."""
        self.slots[slot] = family
        mark = self.mark()
        self.send(family.create_frame(slot))
        created = self.wait_for(f"CREATED for slot {slot}", seconds, lambda frames: next(
            (frame for _, frame in frames if isinstance(frame, wire.Created) and frame.slot == slot), None), mark)
        assert isinstance(created, wire.Created)
        return created.result

    def destroy(self, slot: int) -> None:
        self.slots.pop(slot, None)
        self.send(wire.destroy(slot))

    def drive(self, slot: int, family: Family, steps: list[tuple[str, State]], step: float = STEP_SECONDS) -> None:
        for sequence, (_, state) in enumerate(steps):
            self.send(family.input_frame(slot, state, sequence))
            time.sleep(step)

    def outputs(self, slot: int, since: int = 0) -> list[tuple[float, object]]:
        """The decoded output reports of a slot since the mark, with their arrival times."""
        family = self.slots.get(slot)
        found = []
        for at, frame in self.frames_since(since):
            if isinstance(frame, wire.Output) and frame.slot == slot and family is not None:
                decoded = family.decode_output(frame.data)
                if decoded is not None:
                    found.append((at, decoded))
        return found


# ---- The guest.

class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 forward_port: int) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.forward_port = forward_port
        self._debuggable: bool | None = None

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False)

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()[-500:]}")
        return result.stdout

    def debuggable(self) -> bool:
        """Only a debuggable build has `su`. A user build (what the product ships) is reached through the shell alone."""
        if self._debuggable is None:
            self._debuggable = self.shell("getprop ro.debuggable").strip() == "1"
        return self._debuggable

    def root(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        if not self.debuggable():
            raise Failure(f"'{command}' needs root and this is a user build")
        return self.shell("su 0 sh -c " + shlex.quote(command), timeout)

    def pss_kb(self, pid: int) -> int | None:
        """The process's PSS: smaps_rollup as root on a debuggable build, `dumpsys meminfo` on a user build (None when it prints none)."""
        if self.debuggable():
            return parse_smaps_rollup(self.root(f"cat /proc/{pid}/smaps_rollup")).get("Pss", 0)
        return parse_meminfo_pss(self.shell(f"dumpsys meminfo {pid}"))

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])

    def relay_pid(self) -> int | None:
        result = self.adb_run("shell", f"pidof {RELAY_NAME}")
        return parse_pid(result.stdout) if result.returncode == 0 else None

    def broadcast(self, **extras: object) -> None:
        arguments = ["am", "broadcast", "-a", PROBE_ACTION, "-p", PROBE_PACKAGE]
        for name, value in extras.items():
            flag, text = (("--ez", "true" if value else "false") if isinstance(value, bool)
                          else ("--ei", str(value)) if isinstance(value, int) else ("--es", str(value)))
            arguments += [flag, name, text]
        self.shell(" ".join(shlex.quote(item) for item in arguments))

    def probe_state(self) -> ProbeState:
        """Asks the probe app to write its state and reads it, after its nonce comes back."""
        nonce = uuid.uuid4().hex
        self.broadcast(cmd="dump", nonce=nonce)
        deadline = time.monotonic() + PROBE_SECONDS
        text = ""
        while time.monotonic() < deadline:
            result = self.adb_run("exec-out", f"run-as {PROBE_PACKAGE} cat files/state.json")
            text = result.stdout
            if result.returncode == 0 and f'"nonce":"{nonce}"' in text:
                return parse_probe_state(text)
            time.sleep(0.3)
        raise Failure(f"the probe wrote no state with its nonce in {PROBE_SECONDS} s: {text[:200]!r}")

    def wait_devices(self, description: str, count: int, seconds: float = DEVICE_SECONDS) -> ProbeState:
        deadline = time.monotonic() + seconds
        state = self.probe_state()
        while len(state.devices) != count:
            self.alive()
            if time.monotonic() > deadline:
                raise Failure(f"{description}: {len(state.devices)} game controllers, not {count}: "
                              f"{[device.name for device in state.devices]}")
            time.sleep(0.5)
            state = self.probe_state()
        return state

    def input_dump(self) -> list[InputDumpDevice]:
        return parse_input_dump(self.shell("dumpsys input", timeout=COMMAND_SECONDS))

    def clock_offset_ms(self) -> float:
        """The guest's wall clock minus the host's (see `clock_offset_ms`), read CLOCK_READINGS times."""
        readings = []
        for _ in range(CLOCK_READINGS):
            before = time.time() * 1000
            text = self.shell("date +%s%N").strip()
            after = time.time() * 1000
            if not text.isdigit():
                raise Failure(f"the guest's `date +%s%N` printed {text!r}")
            readings.append((before, int(text) / 1e6, after))
        return clock_offset_ms(readings)

    def cpu_sample(self, pid: int) -> tuple[CpuSample, float]:
        """The CPU ticks now, and the monotonic time at the middle of the call that read them."""
        begun = time.monotonic()
        sample = parse_cpu_sample(self.shell(f"head -n 1 /proc/stat; cat /proc/{pid}/stat"))
        return sample, (begun + time.monotonic()) / 2


def wait_until(description: str, seconds: float, check: Callable[[], str | None],
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


# ---- The checks, each reported on its own.

class Report:
    def __init__(self) -> None:
        self.results: list[tuple[str, str | None]] = []
        self.not_run: list[tuple[str, str]] = []

    def skip(self, name: str, reason: str) -> None:
        """A check (or part of one) that this build cannot run, said in the output and the summary, never dropped."""
        self.not_run.append((name, reason))
        print(f"  skip {name}: not run on a user build ({reason})", flush=True)

    @contextlib.contextmanager
    def check(self, name: str) -> Iterator[None]:
        try:
            yield
        except Failure as problem:
            self.results.append((name, str(problem)))
            print(f"  FAIL {name}: {problem}", flush=True)
        else:
            self.results.append((name, None))
            print(f"  ok   {name}", flush=True)

    def failures(self) -> list[tuple[str, str]]:
        return [(name, problem) for name, problem in self.results if problem is not None]


def raise_problems(problems: list[str]) -> None:
    if problems:
        raise Failure("; ".join(problems))


class Session:
    """The runner's connection and its guest, replaced as the checks reconnect."""

    def __init__(self, guest: Guest, report: Report) -> None:
        self.guest = guest
        self.report = report
        self.link = HostLink(guest.forward_port)
        self.families = families_module.families()

    def connect(self, seconds: float = CONNECT_SECONDS) -> wire.Hello:
        """A new connection, retried until the guest's relay answers (it may be starting)."""
        self.link.close()
        deadline = time.monotonic() + seconds
        while True:
            self.guest.alive()
            self.link = HostLink(self.guest.forward_port)
            try:
                return self.link.connect()
            except (Failure, OSError) as problem:
                self.link.close()
                if time.monotonic() > deadline:
                    raise Failure(f"no HELLO from the relay in {seconds:.0f} s: {problem}") from problem
                time.sleep(2)

    def create_all(self, plan: list[tuple[int, str]]) -> None:
        for slot, key in plan:
            result = self.link.create(slot, self.families[key])
            if result != 0:
                raise Failure(f"CREATE of the {key} in slot {slot} failed with {result}")
            self.link.send(self.families[key].input_frame(slot, families_module.NEUTRAL))

    def destroy_all(self, slots: list[int]) -> None:
        for slot in slots:
            self.link.destroy(slot)


def check_static(session: Session) -> int:
    guest = session.guest
    pid = guest.relay_pid()
    if pid is None:
        raise Failure(f"no {RELAY_NAME} process: init did not start the relay")
    found = parse_ps_label(guest.shell("ps -A -Z"), RELAY_NAME)
    if found is None:
        raise Failure(f"`ps -A -Z` lists no {RELAY_NAME} process")
    user, label = found
    problems = []
    if user != "nobody":
        problems.append(f"runs as {user}, not nobody")
    if not label.startswith(f"u:r:{RELAY_NAME}:"):
        problems.append(f"domain {label}, not {RELAY_NAME}")
    # The shell may list /dev but not stat the node.
    if "uhid" not in guest.shell("ls /dev").split():
        problems.append("no /dev/uhid")
    raise_problems(problems)
    return pid


def check_hello(session: Session) -> None:
    hello = session.connect()
    if (hello.version, hello.slots) != (wire.PROTOCOL_VERSION, wire.SLOT_COUNT):
        raise Failure(f"HELLO says version {hello.version} and {hello.slots} slots")


def check_creation(session: Session) -> ProbeState:
    """Creates the three families; the DualSense's driver asks for feature reports while it probes."""
    link = session.link
    mark = link.mark()
    created_at = time.time()
    session.create_all([(0, "dualsense"), (1, "xbox"), (2, "generic")])
    session.guest.broadcast(cmd="reset")
    state = session.guest.wait_devices("after the creates", 3)

    def started() -> bool | None:
        outputs = link.outputs(0, mark)
        return (any(getattr(out, "lightbar_valid", False) for _, out in outputs)
                and any(getattr(out, "player_leds_valid", False) for _, out in outputs)) or None

    # The driver sets the lightbar and the player LEDs at the end of its probe, after Android has the device.
    with contextlib.suppress(Failure):
        link.wait_for("the DualSense's start-up output", OUTPUT_SECONDS, lambda frames: started(), mark)
    outputs = link.outputs(0, mark)
    # The bound is a deadline, not a measurement: say how long the start-up output took and what the host was doing.
    lightbar_at = next((at for at, out in outputs if getattr(out, "lightbar_valid", False)), None)
    leds_at = next((at for at, out in outputs if getattr(out, "player_leds_valid", False)), None)
    print("    the DualSense's start-up output after the create: "
          + ", ".join(f"{what} {at - created_at:.1f} s" if at is not None else f"{what} never"
                      for what, at in (("lightbar", lightbar_at), ("player LEDs", leds_at)))
          + f" (bound {OUTPUT_SECONDS} s after the devices are listed; host load {os.getloadavg()[0]:.1f})")
    requests = {frame.number for _, frame in link.frames_since(mark)
                if isinstance(frame, wire.GetReport) and frame.slot == 0}
    problems = []
    if {0x05, 0x09, 0x20} - requests:
        problems.append(f"the DualSense asked for feature reports {sorted(hex(number) for number in requests)}, "
                        "not calibration (0x05), pairing (0x09) and firmware (0x20)")
    bars = [out for _, out in outputs if getattr(out, "lightbar_valid", False)]
    if not any(out.lightbar == (0, 0, 128) for out in bars):
        problems.append(f"no blue lightbar at start-up: {[out.lightbar for out in bars]}")
    if not any(getattr(out, "player_leds_valid", False) and out.player_leds for _, out in outputs):
        problems.append("no player LEDs at start-up")
    raise_problems(problems)
    return state


def identity_problem_text(session: Session, state: ProbeState) -> str | None:
    problems = []
    for family in session.families.values():
        device = state.device_named(family.identity.name)
        if device is None:
            problems.append(f"{family.key}: no device named {family.identity.name!r}; saw "
                            f"{[item.name for item in state.devices]}")
            continue
        problems += [f"{family.key}: {problem}" for problem in identity_problems(device, family)]
    return "; ".join(problems) if problems else None


def check_identities(session: Session, state: ProbeState) -> None:
    """A controller's battery level and the like arrive with the driver's first reports, which a starved guest
    processes late: what Android reports is read again until it is right, for IDENTITY_SECONDS."""
    first = identity_problem_text(session, state)
    if first is None:
        return
    wait_until("the controllers' identities", IDENTITY_SECONDS,
               lambda: identity_problem_text(session, session.guest.probe_state()), session.guest)


def check_key_layouts(session: Session) -> None:
    dump = session.guest.input_dump()
    problems = []
    for family in session.families.values():
        found = [device for device in dump if device.name == family.identity.name]
        if not found:
            problems.append(f"{family.key}: the input service lists no {family.identity.name!r}")
            continue
        layout = found[0].key_layout
        if layout is None or not layout.endswith("/" + family.key_layout):
            problems.append(f"{family.key}: key layout {layout}, not {family.key_layout}")
    sensors = [device.name for device in dump if device.name.startswith("DualSense Wireless Controller ")]
    print(f"    the DualSense's other devices: {sensors}")
    raise_problems(problems)


def check_events(session: Session, key: str, slot: int) -> None:
    family = session.families[key]
    guest, link = session.guest, session.link
    device = guest.probe_state().device_named(family.identity.name)
    if device is None:
        raise Failure(f"the probe sees no {family.identity.name!r}")
    problems = []
    for label, steps, check in (
            ("buttons", button_steps(family),
             lambda events: key_sequence_problem(events, device.id, list(family.buttons))),
            ("axes", axis_steps(family),
             lambda events: axes_subsequence_problem(
                 events, device.id, [(name, families_module.expected_state_axes(state))
                                     for name, state in axis_steps(family)]))):
        guest.broadcast(cmd="reset")
        link.drive(slot, family, [("neutral", families_module.NEUTRAL)])
        link.drive(slot, family, steps)
        time.sleep(1.0)
        problem = check(guest.probe_state().events)
        if problem:
            # The events of a starved guest arrive late, not out of order: the sequence is read again until it is
            # whole, for EVENT_SECONDS, and the last problem is what fails.
            try:
                wait_until(label, EVENT_SECONDS, lambda check=check: check(guest.probe_state().events), guest)
            except Failure as failure:
                problems.append(f"{label}: {failure}")
    raise_problems(problems)


def await_rumble(link: HostLink, slot: int, since: int, accept: Callable[[object], bool],
                 description: str) -> tuple[float, object]:
    def find(frames: list) -> tuple[float, object] | None:
        for at, output in link.outputs(slot, since):
            if accept(output):
                return at, output
        return None

    return link.wait_for(description, OUTPUT_SECONDS, find, since)  # type: ignore[return-value]


def check_rumble(session: Session, key: str, slot: int) -> None:
    family = session.families[key]
    guest, link = session.guest, session.link
    device = guest.probe_state().device_named(family.identity.name)
    if device is None:
        raise Failure(f"the probe lists no device named {family.identity.name!r}")
    tolerance = RUMBLE_TOLERANCE[key]
    scale = 100 if key == "xbox" else 255
    strong, weak, milliseconds = 255, 100, 600

    def rumble(output: object) -> tuple[int, int]:
        return output.rumble  # type: ignore[attr-defined]

    def valid(output: object) -> bool:
        return getattr(output, "rumble_valid", True)

    # Each motor on its own: the strong one is the left, the weak one the right.
    mark = link.mark()
    guest.broadcast(cmd="vibrate", device=device.id, strong=strong, weak=weak, ms=milliseconds, mode="motors")
    started, output = await_rumble(link, slot, mark, lambda out: valid(out) and rumble(out) != (0, 0),
                                   f"{key}: the output report that starts the rumble")
    left, right = rumble(output)
    expected = (round(strong * scale / 255), round(weak * scale / 255))
    if abs(left - expected[0]) > tolerance or abs(right - expected[1]) > tolerance:
        raise Failure(f"strong and weak motors {left}, {right}; expected about {expected}")
    after_start = link.mark()
    stopped, _ = await_rumble(link, slot, after_start, lambda out: valid(out) and rumble(out) == (0, 0),
                              f"{key}: the output report that stops the rumble")
    seconds = stopped - started
    print(f"    {key}: motors {left}, {right}; stopped after {seconds:.2f} s of {milliseconds / 1000:.1f}")
    if not milliseconds / 2000 <= seconds <= milliseconds / 1000 + 2.0:
        raise Failure(f"the rumble stopped after {seconds:.2f} s, not about {milliseconds / 1000:.1f}")
    # The device's own vibrator drives both motors alike.
    mark = link.mark()
    guest.broadcast(cmd="vibrate", device=device.id, strong=200, weak=0, ms=300, mode="device")
    _, output = await_rumble(link, slot, mark, lambda out: valid(out) and rumble(out) != (0, 0),
                             f"{key}: the output report of the device's own vibrator")
    left, right = rumble(output)
    if abs(left - right) > tolerance:
        raise Failure(f"the device's vibrator drives the motors unlike, {left} and {right}")
    after_start = link.mark()
    await_rumble(link, slot, after_start, lambda out: valid(out) and rumble(out) == (0, 0),
                 f"{key}: the stop of the device's own vibrator")


def check_lights(session: Session, slot: int) -> None:
    family = session.families["dualsense"]
    guest, link = session.guest, session.link
    device = guest.probe_state().device_named(family.identity.name)
    if device is None:
        raise Failure(f"the probe lists no device named {family.identity.name!r}")
    mark = link.mark()
    guest.broadcast(cmd="light", device=device.id, which="rgb", color=0xFF2040C0 - (1 << 32))
    _, output = await_rumble(link, slot, mark, lambda out: getattr(out, "lightbar_valid", False)
                             and out.lightbar != (0, 0, 128), "the lightbar output report")
    wanted = (0x20, 0x40, 0xC0)
    if any(abs(got - want) > LIGHT_TOLERANCE for got, want in zip(output.lightbar, wanted)):  # type: ignore[attr-defined]
        raise Failure(f"the lightbar is {output.lightbar}, not about {wanted}")  # type: ignore[attr-defined]
    mark = link.mark()
    guest.broadcast(cmd="light", device=device.id, which="player", id=2)
    _, output = await_rumble(link, slot, mark, lambda out: getattr(out, "player_leds_valid", False),
                             "the player LEDs output report")
    print(f"    player LEDs for player id 2: {output.player_leds:05b}")  # type: ignore[attr-defined]
    guest.broadcast(cmd="release")


def check_hot_plug(session: Session) -> None:
    guest, link = session.guest, session.link
    session.destroy_all([0, 1, 2])
    guest.wait_devices("after destroying the three controllers", 0)
    guest.broadcast(cmd="reset")
    plan = [(0, "dualsense"), (1, "xbox"), (2, "generic"), (3, "generic")]
    for slot, key in plan:
        link.slots[slot] = session.families[key]
    mark = link.mark()
    for slot, key in plan:  # back to back, as four controllers connecting together do
        link.send(session.families[key].create_frame(slot))
    link.wait_for("CREATED for the four", OUTPUT_SECONDS * 2, lambda frames: len(
        [frame for _, frame in frames if isinstance(frame, wire.Created) and frame.result == 0]) == 4 or None, mark)
    state = guest.wait_devices("with four controllers", 4)
    added = {identifier for kind, identifier, _ in state.listener if kind == "added"}
    ids = {device.id for device in state.devices}
    if not ids <= added:
        raise Failure(f"the listener saw adds for {sorted(added)}, the devices are {sorted(ids)}")
    session.destroy_all([0, 1, 2, 3])
    after = guest.wait_devices("after the four are destroyed", 0)
    removed = {identifier for kind, identifier, _ in after.listener if kind == "removed"}
    if not ids <= removed:
        raise Failure(f"the listener saw removals of {sorted(removed)}, the devices were {sorted(ids)}")


def check_replacement(session: Session) -> None:
    guest = session.guest
    session.create_all([(0, "xbox")])
    guest.wait_devices("before the second host", 1)
    first = session.link
    second = HostLink(guest.forward_port)
    try:
        hello = second.connect()
        if not first.closed_by_guest(HELLO_SECONDS):
            raise Failure("the first host stayed connected when a second one came")
        guest.wait_devices("after the second host replaced the first", 0)
        if hello.slots != wire.SLOT_COUNT:
            raise Failure(f"the second host's HELLO says {hello.slots} slots")
    finally:
        first.close()
        session.link = second
        guest.alive()


def check_host_drop(session: Session) -> None:
    guest = session.guest
    session.create_all([(0, "dualsense")])
    guest.wait_devices("before the host drops", 1)
    session.link.close()
    guest.wait_devices("after the host dropped", 0)
    session.connect()
    session.create_all([(0, "dualsense")])
    guest.wait_devices("after the host came back", 1)
    session.destroy_all([0])
    guest.wait_devices("after the destroy", 0)


def check_protocol_breaks(session: Session) -> None:
    guest = session.guest
    pid = guest.relay_pid()
    bad = {
        "a wrong magic": b"AGP2" + wire.frame(wire.DESTROY, b"\0\0\0\0")[4:],
        "a frame of a type the host never sends": wire.frame(wire.HELLO, b"\1\0\1\0"),
        "a slot of 8": wire.destroy(8),
        "a length over the limit": wire.MAGIC + b"\x03\x00\x31\x11",
    }
    problems = []
    for name, data in bad.items():
        session.connect()
        session.link.send(data)
        if not session.link.closed_by_guest(HELLO_SECONDS):
            problems.append(f"{name}: the connection stayed open")
    session.connect()
    # An input report for a slot that has no controller is dropped, and the connection lives on.
    session.link.send(wire.input_report(5, b"\1\2\3"))
    time.sleep(1)
    if session.link.closed.is_set():
        problems.append("an input report for an empty slot closed the connection")
    if guest.relay_pid() != pid:
        problems.append("the relay restarted")
    raise_problems(problems)


def check_recovery(session: Session) -> None:
    """Kills the relay (as root: a debuggable build only) and waits for init to restart it."""
    guest = session.guest
    session.create_all([(0, "dualsense")])
    guest.wait_devices("before the relay is killed", 1)
    old = guest.relay_pid()
    if old is None:
        raise Failure("no relay to kill")
    guest.root(f"kill -9 {old}")
    if not session.link.closed_by_guest(CONNECT_SECONDS):
        raise Failure("the connection stayed open after the relay died")
    guest.wait_devices("after the relay died", 0)

    def restarted() -> str | None:
        pid = guest.relay_pid()
        return None if pid is not None and pid != old else f"the relay's pid is {pid}"

    wait_until("init restarting the relay", RESTART_SECONDS, restarted, guest)
    session.connect()
    session.create_all([(0, "dualsense")])
    state = guest.wait_devices("after the relay restarted", 1)
    guest.broadcast(cmd="reset")
    family = session.families["dualsense"]
    session.link.drive(0, family, [("a down", State(buttons=frozenset({"a"}))), ("a up", families_module.NEUTRAL)])
    time.sleep(1)
    problem = key_sequence_problem(guest.probe_state().events, state.devices[0].id, ["a"])
    if problem:
        raise Failure(f"after the restart: {problem}")
    session.destroy_all([0])
    guest.wait_devices("after the final destroy", 0)


def measure_cost(session: Session, options: argparse.Namespace) -> None:
    """The guest's CPU per report, the relay's PSS, the relay's counters and the latency."""
    guest = session.guest
    family = session.families["dualsense"]
    # A connection of its own: the relay's counters are per connection, and the connection of the protocol check
    # carries that check's deliberate input report for an empty slot, which the relay counts as dropped.
    session.connect()
    session.create_all([(0, "dualsense")])
    state = guest.wait_devices("before measuring", 1)
    device_id = state.devices[0].id
    pid = guest.relay_pid()
    assert pid is not None
    interval = options.report_interval_ms / 1000
    reports = options.reports
    guest.broadcast(cmd="record", on=False)
    guest.broadcast(cmd="reset")
    time.sleep(5)
    idle_before, idle_start = guest.cpu_sample(pid)
    time.sleep(reports * interval)
    idle_after, idle_end = guest.cpu_sample(pid)
    pss_before = guest.pss_kb(pid)
    loaded_before, loaded_start = guest.cpu_sample(pid)
    for sequence in range(reports):
        session.link.send(family.input_frame(0, State(left=(255 if sequence % 2 else 0, 128)), sequence))
        time.sleep(interval)
    loaded_after, loaded_end = guest.cpu_sample(pid)
    time.sleep(2)
    pss_after = guest.pss_kb(pid)
    seen = guest.probe_state()
    guest_us, relay_us = per_report_microseconds(
        (idle_before, idle_after), (loaded_before, loaded_after), idle_end - idle_start, loaded_end - loaded_start,
        reports)
    print(f"    {reports} reports at {options.report_interval_ms} ms: the guest spent {guest_us:.0f} us beyond idle "
          f"for each, the relay {relay_us:.0f} us; its PSS {pss_before} kB before and {pss_after} kB after")
    if pss_after is None:
        session.report.skip("the relay's PSS", "smaps_rollup needs root and `dumpsys meminfo` printed no total")
    lost = 1 - seen.motion_samples / reports
    print(f"    the app saw {seen.motion_samples} of {reports} motion samples")
    problems = []
    if pss_after is not None and pss_after > PSS_CEILING_KB:
        problems.append(f"the relay's PSS is {pss_after} kB")
    if lost > LOSS_CEILING:
        problems.append(f"{lost:.1%} of the reports never reached the app")

    # Latency: a burst, recorded.
    guest.broadcast(cmd="record", on=True)
    session.link.send(family.input_frame(0, families_module.NEUTRAL))
    time.sleep(1)
    guest.broadcast(cmd="reset")
    offset = guest.clock_offset_ms()
    sent = []
    for sequence in range(200):
        sent.append(time.time() * 1000)
        session.link.send(family.input_frame(0, State(left=(255 if sequence % 2 else 0, 128)), sequence))
        time.sleep(0.02)
    time.sleep(2)
    events = [event for event in guest.probe_state().events if event.kind == "motion" and event.device == device_id]
    if len(events) != len(sent):
        problems.append(f"{len(events)} motion samples for {len(sent)} reports in the latency burst")
    else:
        latencies = latency_samples(sent, events, offset)
        totals = [sample.total_ms for sample in latencies]
        early = [sample.host_to_kernel_ms for sample in latencies]
        print(f"    latency from the host's send to the app: median {percentile(totals, 0.5):.0f} ms, "
              f"95th {percentile(totals, 0.95):.0f}, max {max(totals):.0f}; "
              f"to the kernel's timestamp: median {percentile(early, 0.5):.0f} ms, "
              f"from there to the app: median {percentile([s.kernel_to_app_ms for s in latencies], 0.5):.0f} ms "
              f"(the guest's clock is {offset:+.0f} ms from the host's, measured and removed)")
        if percentile(totals, 0.5) > LATENCY_MEDIAN_CEILING_MS or max(totals) > LATENCY_MAXIMUM_CEILING_MS:
            problems.append(f"latency median {percentile(totals, 0.5):.0f} ms, max {max(totals):.0f} ms")
        if min(early) < -50:
            problems.append(f"an event is stamped {-min(early):.0f} ms before it was sent: the clocks differ")
    # The relay's own books close with the connection.
    session.link.close()
    time.sleep(2)
    counters = parse_relay_disconnects(guest.shell(f"logcat -d -s {RELAY_LOG_TAG}:I"))
    if not counters:
        problems.append("the relay logged no disconnect line")
    else:
        last = counters[-1]
        print(f"    the relay's counters: {last}")
        if last.input_written < reports + 200 or last.input_failed or last.input_dropped:
            problems.append(f"the relay's counters: {last}")
    raise_problems(problems)


def check_denials(session: Session) -> None:
    guest = session.guest
    if guest.debuggable():
        text = guest.root("dmesg; logcat -d -b all")
    else:
        # The kernel log is not readable on a user build; auditd copies each denial into logcat.
        session.report.skip("the kernel log for SELinux denials", "dmesg needs root; the denials are read from logcat")
        text = guest.shell("logcat -d -b all")
    denials = selinux_denials(text)
    if denials:
        raise Failure(f"{len(denials)} SELinux denials of the relay's domain: {denials[:3]}")


def run_checks(guest: Guest, options: argparse.Namespace) -> Report:
    report = Report()
    session = Session(guest, report)

    def step(name: str):
        guest.alive()
        return report.check(name)

    try:
        print("the relay")
        with step("the relay runs as nobody in its own domain"):
            check_static(session)
        connected = False
        with step("HELLO names version 1 and 8 slots"):
            check_hello(session)
            connected = True
        if not connected:
            return report
        print("creation and identity")
        state: ProbeState | None = None
        with step("a controller of each family is created and Android sees them"):
            state = check_creation(session)
        if state is None:
            return report
        with step("identities: ids, sources, keys, ranges, vibrators, battery, lights"):
            check_identities(session, state)
        with step("the input service chose each family's key layout"):
            check_key_layouts(session)
        print("input")
        for key, slot in (("dualsense", 0), ("xbox", 1), ("generic", 2)):
            with step(f"{key}: buttons, sticks, triggers and the hat"):
                check_events(session, key, slot)
        print("output")
        for key, slot in (("xbox", 1), ("dualsense", 0)):
            with step(f"{key}: rumble, each motor and the stop"):
                check_rumble(session, key, slot)
        with step("dualsense: lightbar and player lights"):
            check_lights(session, 0)
        print("connections")
        with step("four controllers hot plug and unplug"):
            check_hot_plug(session)
        with step("a second host replaces the first"):
            check_replacement(session)
        with step("a host that drops takes its controllers and may return"):
            check_host_drop(session)
        with step("frames that break the protocol close the connection"):
            check_protocol_breaks(session)
        if guest.debuggable():
            with step("killing the relay: restart, reconnect, controllers work again"):
                check_recovery(session)
        else:
            report.skip("killing the relay: restart, reconnect, controllers work again",
                        "the shell cannot signal the relay, which runs as nobody, and there is no su")
        print("cost and health")
        with step("cost per report, PSS, loss, latency and the relay's counters"):
            measure_cost(session, options)
        with step("no SELinux denial of the relay"):
            check_denials(session)
    finally:
        session.link.close()
    return report


# ---- Starting and stopping the guest.

def install_and_start_probe(guest: Guest, apk: pathlib.Path) -> None:
    result = guest.adb_run("install", "-r", str(apk), timeout=COMMAND_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing the probe failed: {result.stdout.strip()[-300:]}")
    guest.shell("svc power stayon true")
    guest.shell("input keyevent KEYCODE_WAKEUP")
    guest.shell("wm dismiss-keyguard")
    guest.shell(f"am start -n {PROBE_ACTIVITY}")

    def focused() -> str | None:
        found = parse_focus(guest.shell("dumpsys window"))
        return None if found == PROBE_FOCUS else f"the focus is {found}"

    wait_until("the probe taking the focus", PROBE_SECONDS, focused, guest)


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
        f"--vsock-forward=127.0.0.1:0-{wire.PORT}",
    ]
    log = private / "emulator.log"
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    serial = f"127.0.0.1:{options.adb_port}"
    # The emulator is this function's until it returns the guest: the caller's cleanup knows a guest only
    # from that return, so a failure or an interrupt (a timeout, Ctrl-C) before it stops the emulator here.
    try:
        guest = Guest(adb, serial, process, log, vsock_forward_port(log, process, wire.PORT))
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
                        help="an existing instance to boot; the default is a fresh one that is removed at the end")
    parser.add_argument("--probe-apk", type=pathlib.Path,
                        help="AndariumGamepadProbe.apk (tests/integration/android/gamepad/build-apk.sh); required")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    parser.add_argument("--reports", type=int, default=3000, help="reports of the cost measurement")
    parser.add_argument("--report-interval-ms", type=float, default=4.0,
                        help="spacing of those reports (the host coalesces to 8 ms)")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0 or options.reports < 100 or options.report_interval_ms <= 0:
        print("counts, intervals and timeouts must be positive, and there are at least 100 reports", file=sys.stderr)
        return 2
    if options.probe_apk is None or not options.probe_apk.is_file():
        print(f"--probe-apk names no file: {options.probe_apk}", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-gamepad-"))
    guest: Guest | None = None
    try:
        guest = start_guest(options, adb, private)
        install_and_start_probe(guest, options.probe_apk)
        report = run_checks(guest, options)
        failures = report.failures()
        print(f"{len(report.results) - len(failures)} of {len(report.results)} checks passed"
              + (f"; not run on a user build: {', '.join(name for name, _ in report.not_run)}" if report.not_run else ""))
        if failures:
            print("FAIL: " + "; ".join(name for name, _ in failures), file=sys.stderr)
            return 1
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
