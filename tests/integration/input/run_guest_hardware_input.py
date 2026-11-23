#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The guest's virtio-input driver follows the host plugging and unplugging
the hardware keyboard, mouse and pointer.

The guest is started with the macOS launcher's ``--hardware-input`` on a
loopback VNC display, and this runner plays the display's user: a VNC client
whose first key plugs the guest's hardware keyboard, whose first pointer event
plugs the mouse and the pointer, and whose disconnect unplugs them
(darwin/device/input/README.md). The guest end is the kernel's virtio_input
driver (kernel/common/input/README.md), read through sysfs and the event
nodes by hardware_input_probe.py. The runner checks:

- before any client, the touchscreen and the side keys' keyboard are input
  devices with the identity and capabilities they always had, and the three
  devices the host plugs are not: a device that reports no event type has no
  input device;
- a key plugs the keyboard and nothing else: the input device appears with
  its identity and bitmaps, the host hears the guest's registration, keys
  arrive in order, a LED the guest writes reaches the host, and a client that
  leaves with a key held ends it: the input device goes and the host hears the
  unregistration (a reader that has not read the key's release by then gets
  ENODEV from the node, so it sees the release or loses it, and never sees
  more);
- a pointer event plugs the mouse and the pointer and no keyboard: the
  buttons, the pointer's contact and a wheel step arrive, and the same
  unplug;
- all of that again after the unplug, for each device kind, and with all three
  plugged by one client;
- a client that connects and leaves again and again, with the keyboard and
  the mouse plugged and unplugged in quick succession, leaves the devices in
  the state of the last client, and the checks of that state run once their
  event nodes hold still for a second.

Every wait has a deadline. The guest is stopped on any exit.
usage: run_guest_hardware_input.py --emulator uml-darwin --rootfs ROOTFS [--init-shell]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import pathlib
import re
import socket
import struct
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
PROBE = "/opt/uml-test/hardware_input_probe.py"
PROBE_SOURCE = HERE / "hardware_input_probe.py"
BOOT_SECONDS = 180
COMMAND_SECONDS = 60
# From the client's first event to the device in sysfs, and the host's report.
PLUG_SECONDS = 45
UNPLUG_SECONDS = 45
EVENT_SECONDS = 20
LISTEN_SECONDS = 30
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|rcu: INFO"
)

TOUCHSCREEN = "Kernarium Touchscreen"
SIDE_KEYS = "Kernarium Keyboard"
KEYBOARD = "Kernarium Hardware Keyboard"
MOUSE = "Kernarium Mouse"
POINTER = "Kernarium Pointer"
HARDWARE = (KEYBOARD, MOUSE, POINTER)
VENDOR = 0x1AF4
PRODUCTS = {TOUCHSCREEN: 1, SIDE_KEYS: 2, KEYBOARD: 3, MOUSE: 4, POINTER: 5}
SERIALS = {
    KEYBOARD: "kernarium-hardware-keyboard-0",
    MOUSE: "kernarium-mouse-0",
    POINTER: "kernarium-pointer-0",
}
BUS_VIRTUAL = 6

# Linux input codes (include/uapi/linux/input-event-codes.h).
EV_KEY, EV_REL, EV_ABS, EV_LED, EV_REP = 0x01, 0x02, 0x03, 0x11, 0x14
KEY_ENTER, KEY_A, KEY_K, KEY_LEFTSHIFT = 28, 30, 37, 42
KEY_POWER, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_BACK, KEY_HOMEPAGE = 116, 114, 115, 158, 172
BTN_TOOL_MOUSE, BTN_TOUCH = 0x146, 0x14A
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA = 0x110, 0x111, 0x112, 0x113, 0x114
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES = 0, 1, 6, 8, 11, 12
ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x2F, 0x35, 0x36, 0x39
LED_NUML, LED_CAPSL, LED_SCROLLL, LED_MISC = 0, 1, 2, 8
INPUT_PROP_DIRECT = 1
WHEEL_DETENT = 120

# VNC keysyms (X11) the display turns into Linux key codes, and the buttons
# of an RFB pointer event (RFC 6143).
KEYSYM_SHIFT_L = 0xFFE1
MASK_LEFT, MASK_WHEEL_UP = 1, 8


def bits(*positions: int) -> int:
    return sum(1 << position for position in positions)


class Failure(RuntimeError):
    pass


# ---- The RFB (VNC) client: the display's user.

class VncClient:
    """One RFB client that never asks for a framebuffer update; a reader thread
    is not needed because the server sends nothing until asked."""

    def __init__(self, guest: Guest, port: int) -> None:
        deadline = time.monotonic() + LISTEN_SECONDS
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise Failure("the VNC display never listened")
                drain(guest, 0.2)
        try:
            if not self._receive(12).startswith(b"RFB "):
                raise Failure("the VNC display is not RFB")
            self.socket.sendall(b"RFB 003.008\n")
            offered = self._receive(self._receive(1)[0])
            if 1 not in offered:
                raise Failure(f"VNC security types {list(offered)}")
            self.socket.sendall(b"\x01")
            if struct.unpack(">I", self._receive(4))[0] != 0:
                raise Failure("the VNC display refused the client")
            self.socket.sendall(b"\x01")
            server_init = self._receive(24)
            self.width, self.height = struct.unpack(">HH", server_init[:4])
            self._receive(struct.unpack(">I", server_init[20:24])[0])
        except BaseException:
            self.socket.close()
            raise

    def _receive(self, size: int) -> bytes:
        data = b""
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            if not chunk:
                raise Failure("the VNC display closed the client")
            data += chunk
        return data

    def key(self, keysym: int, down: bool) -> None:
        self.socket.sendall(struct.pack(">BBHI", 4, 1 if down else 0, 0, keysym))

    def tap(self, keysym: int) -> None:
        self.key(keysym, True)
        time.sleep(0.05)
        self.key(keysym, False)

    def pointer(self, mask: int, x: int, y: int) -> None:
        self.socket.sendall(struct.pack(">BBHH", 5, mask & 0xFF, x, y))

    def centre(self, mask: int = 0) -> None:
        self.pointer(mask, self.width // 2, self.height // 2)

    def close(self) -> None:
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()


# ---- The guest.

def drain(guest: Guest, seconds: float) -> None:
    if guest.wait_for_exit(seconds):
        raise Failure(
            f"the guest exited with status {guest.return_code}:\n" + guest.output()[-4000:]
        )


def probe(guest: Guest, arguments: str, seconds: float = COMMAND_SECONDS) -> str:
    return guest.check_command(f"python3 {PROBE} {arguments}", seconds)


def devices(guest: Guest) -> dict[str, dict]:
    found = {}
    for line in probe(guest, "list").splitlines():
        line = line.strip()
        if line.startswith("{"):
            device = json.loads(line)
            found[device["name"]] = device
    return found


def wait_for_device(guest: Guest, name: str, state: str, seconds: float, what: str) -> None:
    """Waits in the guest until the device is (or is no more) an input device."""
    result = guest.run_command(f"python3 {PROBE} wait '{name}' {state} {seconds}", seconds + 30)
    if result.return_code != 0:
        raise Failure(f"{what}: {result.output.strip()}\n" + describe(guest))


def describe(guest: Guest) -> str:
    names = ", ".join(sorted(devices(guest))) or "none"
    host = [line for line in guest.host_output().splitlines() if "Hardware" in line]
    return f"input devices now: {names}\nthe host reported: {host[-12:]}"


class HostLog:
    """The launcher's report of the devices on its stderr, from a mark on."""

    def __init__(self, guest: Guest) -> None:
        self.guest = guest

    def mark(self) -> int:
        return len(self.guest.host_output())

    def wait(self, text: str, since: int, seconds: float, what: str) -> None:
        deadline = time.monotonic() + seconds
        while text not in self.guest.host_output()[since:]:
            if time.monotonic() > deadline:
                raise Failure(
                    f"{what}: the host never reported {text!r}:\n" + describe(self.guest)
                )
            drain(self.guest, 0.05)

    def absent(self, text: str, since: int, what: str) -> None:
        if text in self.guest.host_output()[since:]:
            raise Failure(f"{what}: the host reported {text!r}")


def watch(guest: Guest, name: str, log: str, seconds: int = 240) -> None:
    """Starts a reader of the device's event node and waits until it has opened it."""
    guest.check_command(
        f"rm -f {log}; (python3 {PROBE} watch '{name}' {log} {seconds} >{log}.err 2>&1 </dev/null &)",
        COMMAND_SECONDS,
    )
    deadline = time.monotonic() + PLUG_SECONDS
    while "open" not in guest.check_command(f"cat {log} 2>/dev/null; true", COMMAND_SECONDS):
        if time.monotonic() > deadline:
            raise Failure(
                f"{name}: the reader never opened the device:\n"
                + guest.check_command(f"cat {log}.err; true", COMMAND_SECONDS)
            )
        drain(guest, 0.2)


def events(guest: Guest, log: str) -> list[tuple[int, int, int]]:
    text = guest.check_command(f"cat {log}; true", COMMAND_SECONDS)
    # The LEDs the test sets come back to a reader of the node as events.
    return [
        (int(kind), int(code), int(value))
        for kind, code, value in re.findall(r"^ev (\d+) (\d+) (-?\d+)\s*$", text, re.M)
        if int(kind) != EV_LED
    ]


def wait_for_events(guest: Guest, log: str, wanted: list[tuple[int, int, int]], what: str,
                    skip: int = 0, seconds: float = EVENT_SECONDS) -> None:
    """Waits until the log's events after the first ``skip``, in order, start with ``wanted``."""
    deadline = time.monotonic() + seconds
    while True:
        got = events(guest, log)[skip:]
        if got[: len(wanted)] == wanted:
            return
        if time.monotonic() > deadline:
            raise Failure(f"{what}: expected {wanted}, the guest received {got}")
        drain(guest, 0.2)


def mark_events(guest: Guest, client: "VncClient", log: str) -> int:
    """How many events the log holds once an Enter that was sent now has arrived.

    The key that plugged the keyboard reaches the guest when the input device
    registers, and a reader may open the device before or after that; the
    events from here on are the test's own."""
    client.tap(0xFF0D)
    deadline = time.monotonic() + EVENT_SECONDS
    while True:
        got = events(guest, log)
        if (EV_KEY, KEY_ENTER, 0) in got:
            return len(got) - got[::-1].index((EV_KEY, KEY_ENTER, 0))
        if time.monotonic() > deadline:
            raise Failure(f"the guest received no Enter: {got}")
        drain(guest, 0.2)


def wait_for_text(guest: Guest, log: str, text: str, what: str, seconds: float = EVENT_SECONDS) -> None:
    deadline = time.monotonic() + seconds
    while text not in guest.check_command(f"cat {log}; true", COMMAND_SECONDS):
        if time.monotonic() > deadline:
            raise Failure(f"{what}: {log} never said {text!r}")
        drain(guest, 0.2)


# ---- What each device must be.

def check_identity(device: dict, name: str) -> None:
    ids = (device["bus"], device["vendor"], device["product"])
    if ids != (BUS_VIRTUAL, VENDOR, PRODUCTS[name]):
        raise Failure(f"{name}: bus, vendor, product {ids}, not {(BUS_VIRTUAL, VENDOR, PRODUCTS[name])}")
    if name in SERIALS and device["uniq"] != SERIALS[name]:
        raise Failure(f"{name}: serial {device['uniq']!r}, not {SERIALS[name]!r}")


def require_bits(name: str, field: str, device: dict, wanted: int) -> None:
    if device[field] & wanted != wanted:
        raise Failure(f"{name}: {field} bitmap {device[field]:#x} lacks {wanted & ~device[field]:#x}")


def check_unchanged_devices(guest: Guest, found: dict[str, dict], what: str) -> None:
    """The touchscreen and the side keys register as they always did."""
    for name in (TOUCHSCREEN, SIDE_KEYS):
        if name not in found:
            raise Failure(f"{what}: {name} is not an input device: {sorted(found)}")
        check_identity(found[name], name)
    require_bits(SIDE_KEYS, "key", found[SIDE_KEYS],
                 bits(KEY_POWER, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_BACK, KEY_HOMEPAGE))
    require_bits(TOUCHSCREEN, "abs", found[TOUCHSCREEN],
                 bits(ABS_MT_SLOT, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID))
    require_bits(TOUCHSCREEN, "properties", found[TOUCHSCREEN], bits(INPUT_PROP_DIRECT))


def check_keyboard_device(device: dict) -> None:
    check_identity(device, KEYBOARD)
    require_bits(KEYBOARD, "key", device, bits(KEY_A, KEY_K, KEY_ENTER, KEY_LEFTSHIFT))
    require_bits(KEYBOARD, "led", device, bits(LED_NUML, LED_CAPSL, LED_SCROLLL))
    # The host sends no key repeat: Android repeats keys itself.
    if device["ev"] & bits(EV_REP):
        raise Failure(f"{KEYBOARD}: advertises key repeat")
    if device["rel"] or device["abs"]:
        raise Failure(f"{KEYBOARD}: has pointer axes")


def check_mouse_devices(mouse: dict, pointer: dict) -> None:
    check_identity(mouse, MOUSE)
    require_bits(MOUSE, "rel", mouse,
                 bits(REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES))
    require_bits(MOUSE, "key", mouse, bits(BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA))
    require_bits(MOUSE, "led", mouse, bits(LED_MISC))
    check_identity(pointer, POINTER)
    require_bits(POINTER, "abs", pointer,
                 bits(ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y))
    require_bits(POINTER, "key", pointer,
                 bits(BTN_TOUCH, BTN_TOOL_MOUSE, BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA))
    # An absolute mouse, not a touchscreen: no input property.
    if pointer["properties"]:
        raise Failure(f"{POINTER}: has input properties {pointer['properties']:#x}")


def require_only(guest: Guest, present: tuple[str, ...], what: str) -> dict[str, dict]:
    """The hardware devices that are input devices are exactly ``present``."""
    found = devices(guest)
    plugged = tuple(name for name in HARDWARE if name in found)
    if plugged != present:
        raise Failure(f"{what}: the guest has {plugged} of the hardware devices, not {present}: "
                      f"{sorted(found)}")
    check_unchanged_devices(guest, found, what)
    return found


# ---- The checks.

def keyboard_cycle(guest: Guest, host: HostLog, port: int, label: str) -> None:
    """Plug the keyboard with a key, type, set a LED, unplug with a key held."""
    since = host.mark()
    client = VncClient(guest, port)
    try:
        client.tap(ord("a"))
        host.wait("Hardware keyboard: plugged", since, PLUG_SECONDS, label)
        host.wait("Hardware keyboard: registered by the guest", since, PLUG_SECONDS, label)
        wait_for_device(guest, KEYBOARD, "present", PLUG_SECONDS, f"{label}: the keyboard never appeared")
        found = require_only(guest, (KEYBOARD,), f"{label}: after a key")
        check_keyboard_device(found[KEYBOARD])
        host.absent("Hardware mouse: plugged", since, f"{label}: a key")

        log = f"/run/keyboard-{label}.log"
        watch(guest, KEYBOARD, log)
        skip = mark_events(guest, client, log)
        typed = [(EV_KEY, KEY_K, 1), (EV_KEY, KEY_K, 0), (EV_KEY, KEY_A, 1), (EV_KEY, KEY_A, 0)]
        client.tap(ord("k"))
        client.tap(ord("a"))
        wait_for_events(guest, log, typed, f"{label}: two keys", skip)
        shifted = [(EV_KEY, KEY_LEFTSHIFT, 1), (EV_KEY, KEY_A, 1), (EV_KEY, KEY_A, 0), (EV_KEY, KEY_LEFTSHIFT, 0)]
        client.key(KEYSYM_SHIFT_L, True)
        client.tap(ord("a"))
        client.key(KEYSYM_SHIFT_L, False)
        wait_for_events(guest, log, typed + shifted, f"{label}: shift and a key", skip)

        led = host.mark()
        probe(guest, f"led '{KEYBOARD}' {LED_CAPSL} 1")
        host.wait("Hardware keyboard: Caps Lock on", led, EVENT_SECONDS, f"{label}: a LED written by the guest")
        probe(guest, f"led '{KEYBOARD}' {LED_CAPSL} 0")
        host.wait("Hardware keyboard: Caps Lock off", led, EVENT_SECONDS, f"{label}: a LED cleared by the guest")

        # The client leaves with a key held: the host releases it before the
        # device goes, though a reader may still lose the release.
        unplug = host.mark()
        client.key(ord("k"), True)
        wait_for_events(guest, log, typed + shifted + [(EV_KEY, KEY_K, 1)], f"{label}: a held key", skip)
    finally:
        client.close()
    host.wait("Hardware keyboard: unplugged", unplug, UNPLUG_SECONDS, label)
    wait_for_device(guest, KEYBOARD, "absent", UNPLUG_SECONDS, f"{label}: the keyboard stayed an input device")
    host.wait("Hardware keyboard: unregistered by the guest", unplug, UNPLUG_SECONDS, label)
    wait_for_text(guest, log, "gone", f"{label}: the reader was not told the device is gone")
    # The input core releases a key still down when it unregisters the device,
    # and the host releases it first, but an event node answers a reader that
    # has not read them yet with ENODEV once the device has gone, so what the
    # reader saw ends with the press or with its release, never with more.
    after = events(guest, log)[skip:]
    print(f"{label}: the reader's events at the unplug: {after[len(typed + shifted):]}")
    if after[len(typed + shifted):] not in ([(EV_KEY, KEY_K, 1)], [(EV_KEY, KEY_K, 1), (EV_KEY, KEY_K, 0)]):
        raise Failure(f"{label}: the held key's events at the unplug: {after}")
    require_only(guest, (), f"{label}: after the unplug")
    print(f"{label}: the keyboard plugged, typed, set a LED and unplugged with a key held")


def pointer_cycle(guest: Guest, host: HostLog, port: int, label: str) -> None:
    """Plug the mouse and the pointer with a pointer event, click, scroll, unplug."""
    since = host.mark()
    client = VncClient(guest, port)
    try:
        client.centre()
        host.wait("Hardware mouse: plugged", since, PLUG_SECONDS, label)
        host.wait("Hardware mouse: registered by the guest", since, PLUG_SECONDS, label)
        host.wait("Hardware pointer: registered by the guest", since, PLUG_SECONDS, label)
        wait_for_device(guest, MOUSE, "present", PLUG_SECONDS, f"{label}: the mouse never appeared")
        wait_for_device(guest, POINTER, "present", PLUG_SECONDS, f"{label}: the pointer never appeared")
        found = require_only(guest, (MOUSE, POINTER), f"{label}: after a pointer event")
        check_mouse_devices(found[MOUSE], found[POINTER])
        host.absent("Hardware keyboard: plugged", since, f"{label}: a pointer event")

        pointer_log = f"/run/pointer-{label}.log"
        mouse_log = f"/run/mouse-{label}.log"
        watch(guest, POINTER, pointer_log)
        watch(guest, MOUSE, mouse_log)
        # A click away from where the pointer plugged in, so that a position
        # event is due (the input core drops one that repeats the last).
        client.pointer(MASK_LEFT, client.width // 4, client.height // 4)
        time.sleep(0.1)
        client.pointer(0, client.width // 4, client.height // 4)
        deadline = time.monotonic() + EVENT_SECONDS
        while True:
            got = events(guest, pointer_log)
            if (EV_KEY, BTN_LEFT, 1) in got and (EV_KEY, BTN_LEFT, 0) in got:
                break
            if time.monotonic() > deadline:
                raise Failure(f"{label}: the pointer saw no click: {got}")
            drain(guest, 0.2)
        got = events(guest, pointer_log)
        if got.index((EV_KEY, BTN_LEFT, 1)) > got.index((EV_KEY, BTN_LEFT, 0)):
            raise Failure(f"{label}: the pointer's button went up before it went down: {got}")
        if not any(kind == EV_ABS and code in (ABS_MT_POSITION_X, ABS_MT_POSITION_Y) for kind, code, _ in got):
            raise Failure(f"{label}: the pointer reported no position: {got}")

        client.centre(MASK_WHEEL_UP)
        time.sleep(0.1)
        client.centre()
        deadline = time.monotonic() + EVENT_SECONDS
        while True:
            got = events(guest, mouse_log)
            steps = [value for kind, code, value in got if kind == EV_REL and code == REL_WHEEL]
            fine = [value for kind, code, value in got if kind == EV_REL and code == REL_WHEEL_HI_RES]
            if steps and fine:
                break
            if time.monotonic() > deadline:
                raise Failure(f"{label}: the mouse saw no wheel step: {got}")
            drain(guest, 0.2)
        if abs(steps[0]) != 1 or abs(fine[0]) != WHEEL_DETENT or (steps[0] > 0) != (fine[0] > 0):
            raise Failure(f"{label}: a wheel step arrived as {steps[0]} and {fine[0]}")

        # The client leaves with a button held: the host releases it before
        # the device goes, though a reader may still lose the release.
        unplug = host.mark()
        client.centre(MASK_LEFT)
        deadline = time.monotonic() + EVENT_SECONDS
        while events(guest, pointer_log).count((EV_KEY, BTN_LEFT, 1)) < 2:
            if time.monotonic() > deadline:
                raise Failure(f"{label}: the pointer saw no second press")
            drain(guest, 0.2)
    finally:
        client.close()
    host.wait("Hardware mouse: unplugged", unplug, UNPLUG_SECONDS, label)
    wait_for_device(guest, MOUSE, "absent", UNPLUG_SECONDS, f"{label}: the mouse stayed an input device")
    wait_for_device(guest, POINTER, "absent", UNPLUG_SECONDS, f"{label}: the pointer stayed an input device")
    host.wait("Hardware mouse: unregistered by the guest", unplug, UNPLUG_SECONDS, label)
    host.wait("Hardware pointer: unregistered by the guest", unplug, UNPLUG_SECONDS, label)
    wait_for_text(guest, pointer_log, "gone", f"{label}: the pointer's reader was not told it is gone")
    # As for the keyboard: the reader may miss the release, never see two presses.
    after = events(guest, pointer_log)
    print(f"{label}: the pointer reader's events: {after}")
    presses = after.count((EV_KEY, BTN_LEFT, 1))
    releases = after.count((EV_KEY, BTN_LEFT, 0))
    if presses != 2 or releases not in (1, 2) or after.index((EV_KEY, BTN_LEFT, 0)) < after.index((EV_KEY, BTN_LEFT, 1)):
        raise Failure(f"{label}: the pointer's button events at the unplug: {after}")
    require_only(guest, (), f"{label}: after the unplug")
    print(f"{label}: the mouse and the pointer plugged, clicked, scrolled and unplugged with a button held")


def everything_cycle(guest: Guest, host: HostLog, port: int, label: str) -> None:
    """One client plugs all three, and its departure unplugs all three."""
    since = host.mark()
    client = VncClient(guest, port)
    try:
        client.tap(ord("a"))
        client.centre()
        for name in HARDWARE:
            wait_for_device(guest, name, "present", PLUG_SECONDS, f"{label}: {name} never appeared")
        found = require_only(guest, HARDWARE, f"{label}: after a key and a pointer event")
        check_keyboard_device(found[KEYBOARD])
        check_mouse_devices(found[MOUSE], found[POINTER])
    finally:
        client.close()
    for name in HARDWARE:
        wait_for_device(guest, name, "absent", UNPLUG_SECONDS, f"{label}: {name} stayed an input device")
    require_only(guest, (), f"{label}: after the unplug")
    host.wait("Hardware mouse: unregistered by the guest", since, UNPLUG_SECONDS, label)
    print(f"{label}: all three plugged by one client and unplugged together")


def flapping_cycle(guest: Guest, host: HostLog, port: int, label: str) -> None:
    """Clients that come and go in quick succession leave the state of the last."""
    for index in range(4):
        client = VncClient(guest, port)
        client.tap(ord("a"))
        client.centre()
        time.sleep(0.05 * (index + 1))
        client.close()
        time.sleep(0.05)
    client = VncClient(guest, port)
    try:
        client.tap(ord("a"))
        client.centre()
        for name in HARDWARE:
            wait_for_device(guest, name, "present", PLUG_SECONDS, f"{label}: {name} did not survive the flapping")
        # A device still present from an earlier flap is registered again when
        # the guest reads the change of the last plug, under another node. Once
        # every device keeps its node for a second, no change is left queued.
        deadline = time.monotonic() + PLUG_SECONDS
        while True:
            first = devices(guest)
            time.sleep(1)
            second = devices(guest)
            nodes = {name: (first.get(name, {}).get("event"), second.get(name, {}).get("event"))
                     for name in HARDWARE}
            if all(before is not None and before == after for before, after in nodes.values()):
                break
            if time.monotonic() > deadline:
                raise Failure(f"{label}: the devices kept no event node for a second within "
                              f"{PLUG_SECONDS} s; the last two listings: {nodes}")
        found = require_only(guest, HARDWARE, f"{label}: after the flapping")
        check_keyboard_device(found[KEYBOARD])
        check_mouse_devices(found[MOUSE], found[POINTER])
        log = f"/run/flap-{label}.log"
        watch(guest, KEYBOARD, log)
        skip = mark_events(guest, client, log)
        client.tap(ord("k"))
        wait_for_events(guest, log, [(EV_KEY, KEY_K, 1), (EV_KEY, KEY_K, 0)],
                        f"{label}: a key after the flapping", skip)
    finally:
        client.close()
    for name in HARDWARE:
        wait_for_device(guest, name, "absent", UNPLUG_SECONDS, f"{label}: {name} stayed an input device")
    print(f"{label}: devices plugged and unplugged in quick succession ended in the last client's state")


def run(arguments: argparse.Namespace) -> None:
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}", "--gpu", "--gpu-display=vnc", f"--gpu-vnc-port={port}", "--hardware-input"),
        overlay_shell=not arguments.init_shell,
        init_shell=arguments.init_shell,
        engine=arguments.engine,
        payloads={PROBE: GuestFile(PROBE_SOURCE.read_bytes(), 0o755)},
        separate_host_output=True,
    ) as guest:
        guest.boot_to_shell(BOOT_SECONDS)
        mark = len(guest.output())
        host = HostLog(guest)

        found = require_only(guest, (), "before any client")
        print(f"before any client: the touchscreen and the side keys are input devices ({sorted(found)}); "
              "the hardware devices the host has not plugged are not")

        keyboard_cycle(guest, host, port, "keyboard-1")
        pointer_cycle(guest, host, port, "pointer-1")
        keyboard_cycle(guest, host, port, "keyboard-2")
        pointer_cycle(guest, host, port, "pointer-2")
        everything_cycle(guest, host, port, "all")
        flapping_cycle(guest, host, port, "flapping")

        failure = FAILURE.search(guest.output()[mark:])
        if failure is not None:
            tail = guest.output()[mark:]
            raise Failure(f"the guest printed {failure[0]!r}:\n" + tail[max(0, failure.start() - 2000):][:6000])
        if not guest.shutdown(60):
            raise Failure("the guest did not power off")
    print("PASS: the guest follows the host's plugging and unplugging of the keyboard, mouse and pointer")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--init-shell",
        action="store_true",
        help="boot the bare shell of an image that is not the arm64 integration rootfs "
        "(the x86-64 slice's images, with the macos-x86_64 emulator and --engine gadgets)",
    )
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    try:
        run(arguments)
    except Failure as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
