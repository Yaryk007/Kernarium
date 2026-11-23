#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's side of the host's hardware keyboard and mouse.

The guest is the release on a fresh data disk (or on the instance named by --instance, for example the
previous release's data disk), started with the macOS launcher on a loopback VNC display and
``--hardware-input``, so that the runner plays the display's user: a VNC client whose first key plugs the
guest's hardware keyboard, whose pointer plugs the mouse and the pointer, and whose disconnect unplugs them
(darwin/device/input/README.md). The contract is android/lineage-24/README.md ("Keyboard and mouse"). The
probe is tests/integration/android/keymouse, an application outside the image that logs what Android
delivers to it; build its APK and name it with --probe-apk. The runner checks:

- before any client: the guest has the input devices an Andarium guest has before a host keyboard is plugged,
  the touchscreen and the side keys' "Kernarium Keyboard" (the macOS launcher's ``--hardware-input`` attaches
  both, and nothing feeds them), the keyboard with the non-alphabetic layout of its key layout file, and none
  of the three devices the host plugs;
- a first key plugs "Kernarium Hardware Keyboard": the launcher reports the plug and the guest's
  registration, Android lists it as an external alphabetic keyboard with the product's device configuration
  and the generic layout, the side keys' device stays non-alphabetic, the probe is told of it, the same key
  sent again reaches the probe, and modifiers, Enter, Escape, End, F5 and Tab arrive with their key and scan
  codes;
- Caps Lock reaches the keyboard's LED and the host hears it;
- the on-screen keyboard: ``show_ime_with_hard_keyboard`` is 0, the one-time change of
  ``kernarium_keyboard_default`` logged what it did (``--keyboard-default updated`` for a data disk that held
  the old default, as the previous release made it), and a later boot leaves a person's choice alone;
- a pointer event plugs "Kernarium Mouse" and "Kernarium Pointer": the cursor lands under the VNC pointer,
  and a click, a right click, a middle click and the wheel reach the probe;
- pointer capture: the probe's request turns the mouse's LED_MISC on and the host hears it, the absolute
  pointer is silent while it holds, the wheel reaches the probe as a captured event (and relative motion,
  where the shell may write the mouse's event node: a ``userdebug`` image), and the release turns the LED
  off and gives the cursor back;
- the client's disconnect unplugs the three devices and Android removes them.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the
guest is reached through the shared adb server, always named with ``-s``.

The runner is opt-in: it needs the macos-android17 executable, an installed release, the probe's APK and an
adb client.
usage: run_android_keymouse.py --emulator uml-darwin-android17 --probe-apk APK [--release DIR] [--instance DIR]
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
from collections.abc import Callable
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402

SPEC = importlib.util.spec_from_file_location("boot_android", REPOSITORY / "tools" / "boot_android.py")
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)

DEFAULT_ADB_PORT = 5582
DEFAULT_VNC_PORT = 5992
# The VNC display's port is the slot's second.
VNC_OFFSET = 1
HARDWARE_INPUT_OPTION = "--hardware-input"
COMMAND_SECONDS = 120
SETTLE_SECONDS = 60
EVENT_SECONDS = 30
PLUG_SECONDS = 45
QUIET_SECONDS = 3
CONNECT_SECONDS = 60
SETTINGS_WRITE_SECONDS = 3
REBOOT_SECONDS = 600

SIDE_KEYS = "Kernarium Keyboard"
KEYBOARD = "Kernarium Hardware Keyboard"
MOUSE = "Kernarium Mouse"
POINTER = "Kernarium Pointer"
HARDWARE_DEVICES = (KEYBOARD, MOUSE, POINTER)
VENDOR = 0x1AF4
PRODUCTS = {SIDE_KEYS: 0x0002, KEYBOARD: 0x0003, MOUSE: 0x0004, POINTER: 0x0005}

PROBE_PACKAGE = "org.example.andarium.keymouseprobe"
PROBE_ACTIVITY = f"{PROBE_PACKAGE}/.KeyMouseActivity"
KEYBOARD_SETTING = "show_ime_with_hard_keyboard"
KEYBOARD_DEFAULT_TAG = "kernarium_keyboard_default"
KEYBOARD_DEFAULT_LOGS = {
    "fresh": "already the product's default",
    "updated": "(was 1)",
}

# What the launcher prints on stderr about the devices (the hardware input bridge of the macOS launcher).
HOST_KEYBOARD_PLUGGED = "Hardware keyboard: plugged"
HOST_KEYBOARD_UNPLUGGED = "Hardware keyboard: unplugged"
HOST_KEYBOARD_REGISTERED = "Hardware keyboard: registered by the guest"
HOST_KEYBOARD_UNREGISTERED = "Hardware keyboard: unregistered by the guest"
HOST_MOUSE_PLUGGED = "Hardware mouse: plugged"
HOST_MOUSE_UNPLUGGED = "Hardware mouse: unplugged"
HOST_MOUSE_REGISTERED = "Hardware mouse: registered by the guest"
HOST_MOUSE_UNREGISTERED = "Hardware mouse: unregistered by the guest"
HOST_POINTER_REGISTERED = "Hardware pointer: registered by the guest"
HOST_POINTER_UNREGISTERED = "Hardware pointer: unregistered by the guest"
HOST_CAPTURE_ON = "Hardware mouse: pointer capture on"
HOST_CAPTURE_OFF = "Hardware mouse: pointer capture off"
HOST_CAPS_ON = "Hardware keyboard: Caps Lock on"
HOST_CAPS_OFF = "Hardware keyboard: Caps Lock off"

# VNC keysyms and the key code and Android key name the guest must see for each
# (darwin/device/input/vnc/vnc_input.cpp for the codes; Generic.kl for the names).
KEYSYM_SHIFT_L = 0xFFE1
KEYSYM_CAPS_LOCK = 0xFFE5
KEYS = (
    (ord("k"), 37, "KEYCODE_K"),
    (ord("a"), 30, "KEYCODE_A"),
    (0xFF0D, 28, "KEYCODE_ENTER"),
    (0xFF1B, 1, "KEYCODE_ESCAPE"),
    (0xFF57, 107, "KEYCODE_MOVE_END"),
    (0xFFC2, 63, "KEYCODE_F5"),
    (0xFF09, 15, "KEYCODE_TAB"),
)
META_SHIFT_ON = 0x1

# VNC pointer button mask bits (RFC 6143), and the button state Android reports for each.
MASK_LEFT, MASK_MIDDLE, MASK_RIGHT, MASK_WHEEL_UP, MASK_WHEEL_DOWN = 1, 2, 4, 8, 16
MASK_WHEEL_LEFT, MASK_WHEEL_RIGHT = 32, 64
BUTTON_PRIMARY, BUTTON_SECONDARY, BUTTON_TERTIARY = 0x1, 0x2, 0x4
TOOL_TYPE_MOUSE = 3
POSITION_TOLERANCE = 4.0
RELATIVE_MOTION = (5, -3)
RELATIVE_TOLERANCE = 0.5

EV_SYN, EV_REL = 0, 2
REL_X, REL_Y = 0, 1


class Failure(RuntimeError):
    pass


# ---- Readers of what the guest prints. The tooling test feeds them text in the same layout.

@dataclass
class HubDevice:
    id: int
    name: str
    classes: str = ""
    path: str = ""
    vendor: int | None = None
    product: int | None = None
    key_layout: str = ""
    key_character_map: str = ""
    configuration: str = ""


@dataclass
class ReaderDevice:
    id: int
    name: str
    external: bool | None = None
    sources: str = ""
    keyboard_type: int | None = None
    pointer_capture_led: str | None = None


@dataclass
class InputDump:
    hub: dict[str, HubDevice] = field(default_factory=dict)
    reader: dict[str, ReaderDevice] = field(default_factory=dict)


HUB_DEVICE = re.compile(r"(-?\d+): (.+?)(?: \(aka device 0 - built-in keyboard\))?")
READER_DEVICE = re.compile(r"Device (-?\d+): (.+)")
IDENTIFIER = re.compile(r"Identifier: bus=0x[0-9a-fA-F]+, vendor=0x([0-9a-fA-F]+), product=0x([0-9a-fA-F]+)")


def parse_input_dump(text: str) -> InputDump:
    """`dumpsys input`: the Event Hub's devices and the Input Reader's, each by its name."""
    dump = InputDump()
    state = ""
    hub: HubDevice | None = None
    reader: ReaderDevice | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if line == "Event Hub State:":
            state = "hub"
            continue
        if line.startswith("Input Reader State"):
            state = "reader"
            hub = reader = None
            continue
        if state == "hub":
            if line.startswith("Unattached video devices:"):
                state = ""
                continue
            match = HUB_DEVICE.fullmatch(line) if raw.startswith("    ") and not raw.startswith("     ") else None
            if match:
                hub = HubDevice(int(match[1]), match[2])
                dump.hub[hub.name] = hub
            elif hub is not None:
                identifier = IDENTIFIER.match(line)
                if identifier:
                    hub.vendor, hub.product = int(identifier[1], 16), int(identifier[2], 16)
                elif line.startswith("Classes: "):
                    hub.classes = line[len("Classes: "):]
                elif line.startswith("Path: "):
                    hub.path = line[len("Path: "):]
                elif line.startswith("KeyLayoutFile: "):
                    hub.key_layout = line[len("KeyLayoutFile: "):]
                elif line.startswith("KeyCharacterMapFile: "):
                    hub.key_character_map = line[len("KeyCharacterMapFile: "):]
                elif line.startswith("ConfigurationFile: "):
                    hub.configuration = line[len("ConfigurationFile: "):]
        elif state == "reader":
            if line.startswith("NextTimeout:"):
                state = ""
                continue
            match = READER_DEVICE.fullmatch(line) if raw.startswith("  ") and not raw.startswith("   ") else None
            if match:
                reader = ReaderDevice(int(match[1]), match[2])
                dump.reader[reader.name] = reader
            elif reader is not None:
                if line.startswith("IsExternal: "):
                    reader.external = line.endswith("true")
                elif line.startswith("Sources: "):
                    reader.sources = line[len("Sources: "):]
                elif line.startswith("KeyboardType: "):
                    reader.keyboard_type = int(line[len("KeyboardType: "):])
                elif line.startswith("PointerCaptureLed: "):
                    reader.pointer_capture_led = line[len("PointerCaptureLed: "):]
    return dump


@dataclass
class Event:
    sequence: int
    kind: str
    fields: dict[str, str]

    def number(self, name: str) -> float:
        return float(self.fields[name])

    def integer(self, name: str) -> int:
        return int(self.fields[name], 0)


FIELD = re.compile(r'(\w+)=("[^"]*"|\S*)')


def parse_probe_log(text: str) -> list[Event]:
    """The probe's events.log: `<sequence> <kind> key=value ...`, a value with a space is quoted."""
    events = []
    for line in text.splitlines():
        head = line.split(" ", 2)
        if len(head) < 2 or not head[0].isdigit():
            continue
        fields = {name: value.strip('"') for name, value in FIELD.findall(head[2] if len(head) > 2 else "")}
        events.append(Event(int(head[0]), head[1], fields))
    return events


def host_lines(log_text: str) -> list[str]:
    """The launcher's lines about the hardware devices, in order."""
    return [line.strip() for line in log_text.splitlines() if line.startswith("Hardware ")]


def plug_lines(lines: list[str]) -> list[str]:
    """The lines that say the launcher plugged a device. Whole lines: the launcher's start line says the
    devices are offered "unplugged", which contains the word."""
    return [line for line in lines if line in (HOST_KEYBOARD_PLUGGED, HOST_MOUSE_PLUGGED)]


def devices_added(events: list[Event]) -> dict[str, list[int]]:
    """The ids of each device name that the probe saw added or listed."""
    found: dict[str, list[int]] = {}
    for event in events:
        if event.kind in ("device", "device-added"):
            found.setdefault(event.fields["name"], []).append(int(event.fields["id"]))
    return found


def devices_removed(events: list[Event]) -> set[int]:
    return {int(event.fields["id"]) for event in events if event.kind == "device-removed"}


def key_events(events: list[Event], name: str, action: str, scan: int | None = None) -> list[Event]:
    return [event for event in events if event.kind == "key" and event.fields.get("code") == name
            and event.fields.get("action") == action and (scan is None or event.integer("scan") == scan)]


def pointer_events(events: list[Event], kind: str, **wanted: str) -> list[Event]:
    return [event for event in events if event.kind == kind
            and all(event.fields.get(name) == value for name, value in wanted.items())]


def near(event: Event, x: float, y: float, tolerance: float = POSITION_TOLERANCE) -> bool:
    return abs(event.number("x") - x) <= tolerance and abs(event.number("y") - y) <= tolerance


def relative_motion(event: Event, dx: float, dy: float, tolerance: float = RELATIVE_TOLERANCE) -> bool:
    """A captured event's motion, from its relative axes or, for a mouse source, from its x and y."""
    for x, y in ((event.fields.get("rel_x"), event.fields.get("rel_y")), (event.fields["x"], event.fields["y"])):
        if x is not None and y is not None and abs(float(x) - dx) <= tolerance and abs(float(y) - dy) <= tolerance:
            return True
    return False


# ---- The RFB (VNC) client: the display's user.

def key_event(down: bool, keysym: int) -> bytes:
    """RFC 6143 KeyEvent."""
    return struct.pack(">BBHI", 4, 1 if down else 0, 0, keysym)


def pointer_event(mask: int, x: int, y: int) -> bytes:
    """RFC 6143 PointerEvent."""
    return struct.pack(">BBHH", 5, mask & 0xFF, x, y)


def update_request(incremental: bool, x: int, y: int, width: int, height: int) -> bytes:
    """RFC 6143 FramebufferUpdateRequest."""
    return struct.pack(">BBHHHH", 3, 1 if incremental else 0, x, y, width, height)


class RfbError(RuntimeError):
    pass


def handshake(read: Callable[[int], bytes], write: Callable[[bytes], None]) -> tuple[int, int, str]:
    """Version, security (None only) and initialisation, shared; returns the framebuffer's size and name."""
    banner = read(12)
    match = re.fullmatch(rb"RFB (\d{3})\.(\d{3})\n", banner)
    if not match:
        raise RfbError(f"not an RFB server: {banner!r}")
    server = (int(match[1]), int(match[2]))
    if server >= (3, 8):
        minor = 8
    elif server >= (3, 7):
        minor = 7
    else:
        minor = 3
    write(b"RFB 003.%03d\n" % minor)
    if minor == 3:
        (security,) = struct.unpack(">I", read(4))
        if security != 1:
            raise RfbError(f"the server offers security type {security}, not None")
    else:
        (count,) = struct.unpack(">B", read(1))
        if count == 0:
            (length,) = struct.unpack(">I", read(4))
            raise RfbError("the server refused: " + read(length).decode(errors="replace"))
        offered = read(count)
        if 1 not in offered:
            raise RfbError(f"the server offers security types {list(offered)}, not None")
        write(b"\x01")
        if minor == 8:
            (result,) = struct.unpack(">I", read(4))
            if result != 0:
                (length,) = struct.unpack(">I", read(4))
                raise RfbError("security failed: " + read(length).decode(errors="replace"))
    write(b"\x01")
    width, height = struct.unpack(">HH", read(4))
    read(16)
    (length,) = struct.unpack(">I", read(4))
    return width, height, read(length).decode(errors="replace")


class VncClient:
    """One shared RFB client. It never requests an update, so the server sends it nothing but a pending
    notice; a thread discards whatever arrives so the connection never blocks the server."""

    def __init__(self, port: int) -> None:
        self.port = port
        self.socket: socket.socket | None = None
        self.width = self.height = 0
        self.name = ""
        self.lock = threading.Lock()

    def connect(self) -> None:
        deadline = time.monotonic() + CONNECT_SECONDS
        problem: Exception | None = None
        while time.monotonic() < deadline:
            try:
                connection = socket.create_connection(("127.0.0.1", self.port), timeout=10)
            except OSError as error:
                problem = error
                time.sleep(1)
                continue
            try:
                self.width, self.height, self.name = handshake(
                    lambda count: read_exactly(connection, count), connection.sendall)
            except (RfbError, OSError) as error:
                connection.close()
                problem = error
                time.sleep(1)
                continue
            connection.settimeout(None)
            self.socket = connection
            threading.Thread(target=self.drain, args=(connection,), daemon=True).start()
            return
        raise Failure(f"no VNC handshake on 127.0.0.1:{self.port} within {CONNECT_SECONDS} s: {problem}")

    @staticmethod
    def drain(connection: socket.socket) -> None:
        try:
            while connection.recv(65536):
                pass
        except OSError:
            pass

    def send(self, data: bytes) -> None:
        if self.socket is None:
            raise Failure("the VNC client is not connected")
        with self.lock:
            self.socket.sendall(data)

    def key(self, keysym: int, down: bool) -> None:
        self.send(key_event(down, keysym))

    def tap(self, keysym: int) -> None:
        self.key(keysym, True)
        time.sleep(0.05)
        self.key(keysym, False)

    def pointer(self, mask: int, x: int, y: int) -> None:
        self.send(pointer_event(mask, x, y))

    def click(self, mask: int, x: int, y: int) -> None:
        self.pointer(mask, x, y)
        time.sleep(0.1)
        self.pointer(0, x, y)

    def close(self) -> None:
        if self.socket is not None:
            try:
                self.socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.socket.close()
            self.socket = None


def read_exactly(connection: socket.socket, count: int) -> bytes:
    data = b""
    while len(data) < count:
        chunk = connection.recv(count - len(data))
        if not chunk:
            raise RfbError("the server closed the connection")
        data += chunk
    return data


# ---- The guest.

class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 vnc_port: int) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.vnc_port = vnc_port

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

    def host_lines(self) -> list[str]:
        return host_lines(self.log.read_text(errors="replace"))

    def input_dump(self) -> InputDump:
        return parse_input_dump(self.shell("dumpsys input"))

    def probe_events(self) -> list[Event]:
        result = self.adb_run("exec-out", "run-as", PROBE_PACKAGE, "cat", "files/events.log")
        if result.returncode != 0:
            raise Failure(f"cannot read the probe's log: {result.stdout.strip()[-300:]}")
        return parse_probe_log(result.stdout)

    def start_probe(self, *extras: str) -> None:
        self.shell(f"am start -n {PROBE_ACTIVITY} {' '.join(extras)}")


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: Guest | None = None) -> None:
    """Calls `check` until it returns None (done); its last message is the failure."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        try:
            message = check()
        except (Failure, subprocess.TimeoutExpired) as problem:
            message = str(problem)
        if message is None:
            return
        time.sleep(1.0)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


def wait_for_host(guest: Guest, line: str, since: int, seconds: float = PLUG_SECONDS) -> None:
    """The launcher printed `line` after its first `since` hardware lines."""
    wait_for(f"the launcher to say {line!r}", seconds,
             lambda: None if line in guest.host_lines()[since:] else "not said yet", guest)


def wait_for_events(guest: Guest, description: str, since: int,
                    check: Callable[[list[Event]], str | None], seconds: float = EVENT_SECONDS) -> list[Event]:
    """The probe's events after the first `since`, once `check` accepts them."""
    found: list[Event] = []

    def settled() -> str | None:
        found[:] = guest.probe_events()[since:]
        return check(found)

    try:
        wait_for(description, seconds, settled, guest)
    except Failure as problem:
        seen = ", ".join(f"{event.kind}:{event.fields.get('name') or event.fields.get('key') or ''}" for event in found[-12:])
        try:
            focus = " | ".join(line.strip() for line in guest.shell("dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'").splitlines())
        except (Failure, subprocess.TimeoutExpired) as unreadable:
            focus = f"unreadable ({unreadable})"
        raise Failure(f"{problem}; the probe's last events: [{seen}]; window focus: {focus}") from None
    return list(found)


# ---- What the dump must show.

def device_problem(dump: InputDump, name: str, external: bool, keyboard_type: int | None,
                   layout_is_vendor: bool) -> str | None:
    """None when Android lists the device as the product configures it, else what differs."""
    hub = dump.hub.get(name)
    reader = dump.reader.get(name)
    if hub is None or reader is None:
        return f"{name!r} is not an input device"
    product = PRODUCTS[name]
    if (hub.vendor, hub.product) != (VENDOR, product):
        return f"{name!r} is {hub.vendor}:{hub.product}, not {VENDOR:#06x}:{product:#06x}"
    if reader.external is not external:
        return f"{name!r} has IsExternal {reader.external}, not {external}"
    if keyboard_type is not None and reader.keyboard_type != keyboard_type:
        return f"{name!r} has keyboard type {reader.keyboard_type}, not {keyboard_type}"
    configuration = pathlib.PurePosixPath(hub.configuration)
    if name != SIDE_KEYS:
        if not hub.configuration.startswith("/vendor/usr/idc/") or configuration.name != f"Vendor_1af4_Product_{product:04x}.idc":
            return f"{name!r} has the device configuration {hub.configuration!r}"
    layout = pathlib.PurePosixPath(hub.key_layout).name
    if layout_is_vendor != (layout == f"Vendor_1af4_Product_{product:04x}.kl"):
        return f"{name!r} has the key layout {hub.key_layout!r}"
    return None


def side_keys_problem(dump: InputDump) -> str | None:
    """The side keys' own device: a non-alphabetic keyboard on its own key layout."""
    return device_problem(dump, SIDE_KEYS, external=False, keyboard_type=1, layout_is_vendor=True)


def keyboard_problem(dump: InputDump) -> str | None:
    """The host's keyboard: an external alphabetic keyboard on the generic layout."""
    problem = device_problem(dump, KEYBOARD, external=True, keyboard_type=2, layout_is_vendor=False)
    if problem:
        return problem
    return side_keys_problem(dump)


def mouse_problem(dump: InputDump) -> str | None:
    """The mouse is external and relative, the pointer an internal absolute mouse."""
    problem = device_problem(dump, MOUSE, external=True, keyboard_type=None, layout_is_vendor=False)
    if problem:
        return problem
    if "MOUSE" not in dump.reader[MOUSE].sources.upper():
        return f"{MOUSE!r} has the sources {dump.reader[MOUSE].sources!r}"
    problem = device_problem(dump, POINTER, external=False, keyboard_type=None, layout_is_vendor=False)
    if problem:
        return problem
    if "MOUSE" not in dump.reader[POINTER].sources.upper():
        return f"{POINTER!r} has the sources {dump.reader[POINTER].sources!r}"
    return None


def unplugged_problem(dump: InputDump) -> str | None:
    present = [name for name in HARDWARE_DEVICES if name in dump.hub or name in dump.reader]
    return f"still input devices: {present}" if present else None


def capture_led_problem(dump: InputDump, state: str) -> str | None:
    """The mouse's mapper dumps `PointerCaptureLed: true`, `false`, or `<none>` for a device without the LED."""
    reader = dump.reader.get(MOUSE)
    if reader is None:
        return f"{MOUSE!r} is not an input device"
    if reader.pointer_capture_led is None:
        return "the cursor mapper prints no PointerCaptureLed line (is the LED topic in the image?)"
    if reader.pointer_capture_led != state:
        return f"PointerCaptureLed is {reader.pointer_capture_led!r}, not {state!r}"
    return None


# ---- The checks.

def check_before_any_client(guest: Guest) -> None:
    dump = guest.input_dump()
    problem = unplugged_problem(dump) or side_keys_problem(dump)
    if problem:
        raise Failure(f"before any client: {problem}")
    if plug_lines(guest.host_lines()):
        raise Failure(f"the launcher plugged a device before any client: {guest.host_lines()}")
    print("before any client: the side keys' keyboard is non-alphabetic; none of the host's devices")


def install_probe(guest: Guest, apk: pathlib.Path) -> None:
    result = guest.adb_run("install", "-r", str(apk), timeout=COMMAND_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing the probe failed: {result.stdout.strip()[-400:]}")
    guest.shell("wm dismiss-keyguard; input keyevent KEYCODE_WAKEUP")
    guest.start_probe("--es run_id keymouse")

    def started() -> str | None:
        events = guest.probe_events()
        return None if any(event.kind == "begin" for event in events) else "no begin line"

    wait_for("the probe to start", EVENT_SECONDS, started, guest)


def check_keyboard(guest: Guest, vnc: VncClient) -> None:
    """Plugs the keyboard with a first key and checks what Android makes of it.

    The key that plugs the keyboard is not required at the probe: the first event after a hot-plug has been
    seen not to arrive (a host-side question, queued with the host lanes). The same key sent once the probe
    has been told of the device must arrive."""
    host_before = len(guest.host_lines())
    probe_before = len(guest.probe_events())
    vnc.tap(KEYS[0][0])
    wait_for_host(guest, HOST_KEYBOARD_PLUGGED, host_before)
    wait_for_host(guest, HOST_KEYBOARD_REGISTERED, host_before)
    wait_for("Android's device configuration of the keyboard", PLUG_SECONDS,
             lambda: keyboard_problem(guest.input_dump()), guest)
    events = wait_for_events(
        guest, "the probe to be told of the keyboard", probe_before,
        lambda found: None if any(event.kind == "device-added" and event.fields["name"] == KEYBOARD
                                  for event in found) else "no device-added line for the keyboard")
    added = [event for event in events if event.kind == "device-added" and event.fields["name"] == KEYBOARD]
    if not added or added[0].integer("keyboard_type") != 2:
        raise Failure(f"the probe was not told of an alphabetic {KEYBOARD!r}: {added}")
    time.sleep(1)
    probe_before = len(guest.probe_events())
    vnc.tap(KEYS[0][0])
    wait_for_events(
        guest, "the first key sent again at the probe", probe_before,
        lambda found: None if key_events(found, KEYS[0][2], "down", KEYS[0][1])
        and key_events(found, KEYS[0][2], "up", KEYS[0][1]) else "the key did not arrive")
    print("keyboard: plugged by the first key, external and alphabetic, the key sent again arrived")

    probe_before = len(guest.probe_events())
    for keysym, scan, name in KEYS[1:]:
        vnc.tap(keysym)
    vnc.key(KEYSYM_SHIFT_L, True)
    vnc.tap(KEYS[1][0])
    vnc.key(KEYSYM_SHIFT_L, False)

    def typed(found: list[Event]) -> str | None:
        for keysym, scan, name in KEYS[1:]:
            if not key_events(found, name, "down", scan) or not key_events(found, name, "up", scan):
                return f"{name} (scan {scan}) did not arrive"
        shifted = [event for event in key_events(found, KEYS[1][2], "down", KEYS[1][1])
                   if event.integer("meta") & META_SHIFT_ON]
        if not shifted:
            return "Shift then A did not arrive with the shift meta state"
        return None

    wait_for_events(guest, "the typed keys at the probe", probe_before, typed)
    print("keys: modifiers, Enter, Escape, End, F5 and Tab arrive with their key and scan codes")


def check_caps_lock(guest: Guest, vnc: VncClient) -> None:
    host_before = len(guest.host_lines())
    vnc.tap(KEYSYM_CAPS_LOCK)
    wait_for_host(guest, HOST_CAPS_ON, host_before)
    host_before = len(guest.host_lines())
    vnc.tap(KEYSYM_CAPS_LOCK)
    wait_for_host(guest, HOST_CAPS_OFF, host_before)
    print("caps lock: Android lit the keyboard's LED and put it out, and the host heard both")


def check_keyboard_default(guest: Guest, expectation: str) -> None:
    value = guest.shell(f"cmd settings get secure {KEYBOARD_SETTING}").strip()
    if value != "0":
        raise Failure(f"{KEYBOARD_SETTING} is {value!r} with a hardware keyboard, not 0")
    logged = guest.shell(f"logcat -d -s {KEYBOARD_DEFAULT_TAG}:I")
    if KEYBOARD_DEFAULT_LOGS[expectation] not in logged:
        raise Failure(f"the on-screen keyboard script did not log {KEYBOARD_DEFAULT_LOGS[expectation]!r} "
                      f"(expected for a {expectation} data disk): {logged.strip()[-400:]!r}")
    print(f"on-screen keyboard: {KEYBOARD_SETTING} is 0 and the one-time change logged "
          f"{KEYBOARD_DEFAULT_LOGS[expectation]!r}")


def check_pointer(guest: Guest, vnc: VncClient) -> None:
    """Plugs the mouse and the pointer; checks the cursor, the buttons and the wheel."""
    host_before = len(guest.host_lines())
    probe_before = len(guest.probe_events())
    first = (vnc.width * 2 // 5, vnc.height // 2)
    second = (vnc.width * 3 // 5, vnc.height * 3 // 4)
    vnc.pointer(0, *first)
    wait_for_host(guest, HOST_MOUSE_PLUGGED, host_before)
    wait_for_host(guest, HOST_MOUSE_REGISTERED, host_before)
    wait_for_host(guest, HOST_POINTER_REGISTERED, host_before)
    wait_for("Android's configuration of the mouse and the pointer", PLUG_SECONDS,
             lambda: mouse_problem(guest.input_dump()), guest)
    # The event that plugs the devices can be lost (the first event after a hot-plug has been seen not to
    # arrive), and an absolute pointer sent to the position it already has makes no event: each position below
    # differs from the one before.
    vnc.pointer(0, *second)
    wait_for_events(
        guest, "a hover at the second position", probe_before,
        lambda found: None if any(near(event, *second) for event in pointer_events(found, "hover"))
        else f"no hover near {second}: {[(e.fields['x'], e.fields['y']) for e in pointer_events(found, 'hover')]}")
    vnc.pointer(0, *first)
    wait_for_events(
        guest, "a hover at the first position", probe_before,
        lambda found: None if any(near(event, *first) for event in pointer_events(found, "hover"))
        else "the cursor did not follow the pointer")
    print(f"pointer: plugged by a pointer event, the cursor lands within {POSITION_TOLERANCE:.0f} px of "
          f"the VNC pointer at {second} and {first}")

    for label, mask, state in (("primary", MASK_LEFT, BUTTON_PRIMARY), ("secondary", MASK_RIGHT, BUTTON_SECONDARY),
                               ("tertiary", MASK_MIDDLE, BUTTON_TERTIARY)):
        probe_before = len(guest.probe_events())
        vnc.click(mask, *second)
        wait_for_events(
            guest, f"a {label} click at the probe", probe_before,
            lambda found, state=state: None if any(
                event.integer("buttons") == state and event.integer("tool") == TOOL_TYPE_MOUSE
                for event in pointer_events(found, "touch", action="down"))
            else f"no mouse touch down with buttons {state:#x}")
    print("buttons: left, right and middle arrive as the primary, secondary and tertiary buttons")

    for label, mask, sign, axis in (("up", MASK_WHEEL_UP, 1, "vscroll"), ("down", MASK_WHEEL_DOWN, -1, "vscroll"),
                                    ("left", MASK_WHEEL_LEFT, 0, "hscroll"), ("right", MASK_WHEEL_RIGHT, 0, "hscroll")):
        probe_before = len(guest.probe_events())
        vnc.click(mask, *second)

        def scrolled(found: list[Event], sign=sign, axis=axis) -> str | None:
            values = [event.number(axis) for event in pointer_events(found, "scroll")]
            if sign == 0:
                return None if any(value != 0 for value in values) else f"no {axis}"
            return None if any(value * sign > 0 for value in values) else f"{axis} values {values}"

        wait_for_events(guest, f"a wheel step {label} at the probe", probe_before, scrolled)
    print("wheel: both axes arrive, the vertical one with the VNC mapping's direction")


def event_node(guest: Guest) -> str:
    dump = guest.input_dump()
    hub = dump.hub.get(MOUSE)
    if hub is None or not hub.path.startswith("/dev/input/event"):
        raise Failure(f"{MOUSE!r} has no event node: {hub}")
    return hub.path


def can_inject_relative(guest: Guest, node: str) -> bool:
    """Whether the shell may write the mouse's event node. A `user` image lets it read the input nodes and
    denies the write (SELinux, shell domain); only a `userdebug` image allows `sendevent`. The VNC pointer
    has no relative motion, so nothing else produces it."""
    return guest.shell(f"test -w {node} && echo writable || echo denied").strip() == "writable"


def inject_relative(guest: Guest, node: str, dx: int, dy: int) -> None:
    """One relative frame written to the mouse's event node, as `sendevent` does."""
    guest.shell(f"sendevent {node} {EV_REL} {REL_X} {dx}; sendevent {node} {EV_REL} {REL_Y} {dy}; "
                f"sendevent {node} {EV_SYN} 0 0")


def check_capture(guest: Guest, vnc: VncClient) -> None:
    node = event_node(guest)
    host_before = len(guest.host_lines())
    probe_before = len(guest.probe_events())
    guest.start_probe("--ez capture true")
    wait_for_events(guest, "pointer capture granted to the probe", probe_before,
                    lambda found: None if pointer_events(found, "capture", state="on") else "no capture on line")
    wait_for_host(guest, HOST_CAPTURE_ON, host_before)
    wait_for("the cursor mapper's LED on", PLUG_SECONDS, lambda: capture_led_problem(guest.input_dump(), "true"), guest)
    print("capture on: the probe holds it, LED_MISC is lit and the host heard it")

    # The absolute pointer is disabled while Android holds the capture. A hover that was in progress ends
    # with the capture, so the quiet starts after a moment.
    time.sleep(1.0)
    probe_before = len(guest.probe_events())
    vnc.pointer(0, vnc.width // 3, vnc.height // 3)
    vnc.pointer(0, vnc.width // 2, vnc.height // 2)
    time.sleep(QUIET_SECONDS)
    moved = [event for event in guest.probe_events()[probe_before:] if event.kind in ("hover", "touch")]
    if moved:
        raise Failure(f"the absolute pointer reached the probe while it held the capture: {moved[:3]}")

    relative = can_inject_relative(guest, node)
    if relative:
        inject_relative(guest, node, *RELATIVE_MOTION)
        wait_for_events(
            guest, "relative motion captured at the probe", probe_before,
            lambda found: None if any(
                relative_motion(event, *RELATIVE_MOTION) for event in pointer_events(found, "captured", action="move"))
            else "no captured move of the injected motion (unscaled, as capture requires)")
    else:
        print(f"skip: relative motion under the capture; the shell may not write {node} on this image (a user "
              "build) and a VNC client has no relative motion")
    probe_before = len(guest.probe_events())
    vnc.click(MASK_WHEEL_UP, vnc.width // 2, vnc.height // 2)
    wait_for_events(
        guest, "the wheel captured at the probe", probe_before,
        lambda found: None if any(event.number("vscroll") > 0 for event in pointer_events(found, "captured", action="scroll"))
        else "no captured scroll")
    print("capture: the pointer is silent, " + ("relative motion and the wheel arrive" if relative
                                                  else "the wheel arrives") + " as captured events")

    host_before = len(guest.host_lines())
    probe_before = len(guest.probe_events())
    guest.start_probe("--ez capture false")
    wait_for_events(guest, "pointer capture released", probe_before,
                    lambda found: None if pointer_events(found, "capture", state="off") else "no capture off line")
    wait_for_host(guest, HOST_CAPTURE_OFF, host_before)
    wait_for("the cursor mapper's LED off", PLUG_SECONDS, lambda: capture_led_problem(guest.input_dump(), "false"), guest)
    probe_before = len(guest.probe_events())
    target = (vnc.width // 4, vnc.height // 4)
    vnc.pointer(0, *target)
    wait_for_events(
        guest, "the cursor back under the absolute pointer", probe_before,
        lambda found: None if any(near(event, *target) for event in pointer_events(found, "hover"))
        else "the absolute pointer did not return")
    print("capture off: LED_MISC is out, the host heard it, and the absolute pointer is back")


def check_unplug(guest: Guest, vnc: VncClient) -> None:
    """The client's disconnect unplugs the three devices; Android removes them."""
    events = guest.probe_events()
    ids = {name: ids[-1] for name, ids in devices_added(events).items() if name in HARDWARE_DEVICES}
    if set(ids) != set(HARDWARE_DEVICES):
        raise Failure(f"the probe saw only {sorted(ids)} of the host's devices")
    host_before = len(guest.host_lines())
    vnc.close()
    for line in (HOST_KEYBOARD_UNPLUGGED, HOST_MOUSE_UNPLUGGED, HOST_KEYBOARD_UNREGISTERED,
                 HOST_MOUSE_UNREGISTERED, HOST_POINTER_UNREGISTERED):
        wait_for_host(guest, line, host_before)
    wait_for("Android to remove the three devices", PLUG_SECONDS,
             lambda: unplugged_problem(guest.input_dump()), guest)
    wait_for_events(
        guest, "the probe to be told of the removals", 0,
        lambda found: None if set(ids.values()) <= devices_removed(found) else "not all removed")
    side = guest.input_dump()
    problem = side_keys_problem(side)
    if problem:
        raise Failure(f"after the unplug: {problem}")
    print("unplug: the disconnect removed the keyboard, the mouse and the pointer; the side keys stay")


def check_choice_survives_a_boot(guest: Guest, reboot_guest: Callable[[], None]) -> None:
    """A person's later choice of the on-screen keyboard survives a reboot: the script has its marker."""
    guest.shell(f"cmd settings put secure {KEYBOARD_SETTING} 1")
    # SettingsProvider writes a changed setting to its file after a short delay; a reboot that came sooner would
    # lose the choice for a reason that has nothing to do with the script's marker, which is what this checks.
    time.sleep(SETTINGS_WRITE_SECONDS)
    reboot_guest()
    value = guest.shell(f"cmd settings get secure {KEYBOARD_SETTING}").strip()
    logged = guest.shell(f"logcat -d -s {KEYBOARD_DEFAULT_TAG}:I").strip()
    guest.shell(f"cmd settings put secure {KEYBOARD_SETTING} 0")
    if value != "1":
        raise Failure(f"a reboot changed a person's choice of the on-screen keyboard to {value!r}; "
                      f"the one-time script's log after the reboot: {logged[-300:]!r}")
    if "now hides" in logged or "already the product" in logged:
        raise Failure(f"the one-time script ran again after a reboot: {logged[-300:]!r}")
    print("on-screen keyboard: a later choice of 1 survives a reboot; the one-time change does not run again")


def run_checks(guest: Guest, options: argparse.Namespace) -> None:
    check_before_any_client(guest)
    install_probe(guest, options.probe_apk)
    initial = devices_added(guest.probe_events())
    if any(name in initial for name in HARDWARE_DEVICES):
        raise Failure(f"the probe saw the host's devices before any client: {sorted(initial)}")
    vnc = VncClient(guest.vnc_port)
    try:
        vnc.connect()
        print(f"VNC client connected to a {vnc.width}x{vnc.height} display; nothing is plugged yet")
        problem = unplugged_problem(guest.input_dump())
        if problem:
            raise Failure(f"a client that sent nothing plugged a device: {problem}")
        check_keyboard(guest, vnc)
        check_caps_lock(guest, vnc)
        check_keyboard_default(guest, options.keyboard_default)
        check_pointer(guest, vnc)
        check_capture(guest, vnc)
        check_unplug(guest, vnc)
    finally:
        vnc.close()
    check_choice_survives_a_boot(guest, lambda: reboot(guest, options))


def reboot(guest: Guest, options: argparse.Namespace) -> None:
    previous = guest.shell("cat /proc/sys/kernel/random/boot_id").strip()
    guest.adb_run("reboot", timeout=30)

    def booted() -> str | None:
        if guest.adb_run("get-state", timeout=10).stdout.strip() != "device":
            return "adb has not reconnected"
        if guest.shell("getprop sys.boot_completed").strip() != "1":
            return "not boot_completed"
        if guest.shell("cat /proc/sys/kernel/random/boot_id").strip() == previous:
            return "still the previous boot"
        return None

    boot_android.disconnect_adb(guest.adb, guest.serial)
    deadline = time.monotonic() + REBOOT_SECONDS
    while time.monotonic() < deadline:
        guest.alive()
        subprocess.run([guest.adb, "connect", guest.serial], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30, check=False)
        try:
            if booted() is None:
                break
        except (Failure, subprocess.TimeoutExpired):
            pass
        time.sleep(2)
    else:
        raise Failure(f"the guest did not boot again within {REBOOT_SECONDS} s")
    time.sleep(SETTLE_SECONDS)


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
        "--display", "vnc",
        "--vnc-port", str(options.vnc_port),
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
        HARDWARE_INPUT_OPTION,
    ]
    log = private / "emulator.log"
    environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True, env=environment)
    serial = f"127.0.0.1:{options.adb_port}"
    # The emulator is this function's until it returns the guest: the caller's cleanup knows a guest only
    # from that return, so a failure or an interrupt (a timeout, Ctrl-C) before it stops the emulator here.
    try:
        guest = Guest(adb, serial, process, log, options.vnc_port)
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
                        help="an existing instance to boot (for example the previous release's data disk, "
                        "with --keyboard-default updated); the default is a fresh one that is removed at the end")
    parser.add_argument("--probe-apk", type=pathlib.Path,
                        help="the AndariumKeyMouseProbe APK (tests/integration/android/keymouse); required")
    parser.add_argument("--keyboard-default", choices=sorted(KEYBOARD_DEFAULT_LOGS),
                        help="what the one-time on-screen keyboard change finds: fresh (a new data disk, "
                        "already the default) or updated (a disk that held the previous release's default 1); "
                        "default: fresh, or updated with --instance")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--vnc-port", type=int, default=slot_ports.slot_port(VNC_OFFSET, DEFAULT_VNC_PORT),
                        help="loopback port of the guest's VNC display (default: the guest slot's second port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_VNC_PORT})")
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
    if options.probe_apk is None or not options.probe_apk.is_file():
        print(f"the probe APK {options.probe_apk} is not a file (--probe-apk)", file=sys.stderr)
        return 2
    if options.keyboard_default is None:
        options.keyboard_default = "updated" if options.instance else "fresh"
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-keymouse-"))
    guest: Guest | None = None
    try:
        guest = start_guest(options, adb, private)
        run_checks(guest, options)
        print("PASS")
        return 0
    except (Failure, RfbError, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
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
