# SPDX-License-Identifier: GPL-2.0-only
"""The controllers the gamepad runner plays: their HID identities and descriptors, the input reports it sends, the
feature reports it answers and the output reports it reads back.

This file transcribes report layouts of the Linux drivers that bind the devices (drivers/hid/hid-playstation.c: the
DualSense input, output and feature report structures, the valid-flag and button masks, the calibration, firmware
and pairing feature reports; drivers/hid/hid-microsoft.c: the Xbox force feedback report), and the names of the
Android key layouts that apply to them. A first-party file derived from kernel code keeps the kernel's identifier,
GPL-2.0-only (LICENSES/README.md), so this file does, and the exception list of the licence-header check names it.
The HID report descriptors are authored here from what those drivers and Linux's hid-input expect of a device; none
is copied from a device or from a CTS fixture.

The runner imports this module and is GPL-2.0-or-later. `gamepad_wire.py` is the wire and knows no family.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import struct
from dataclasses import dataclass

import gamepad_wire as wire

# Android's key codes (android.view.KeyEvent) and axes (android.view.MotionEvent), by the names the probe reports.
KEYCODES = {
    "a": 96, "b": 97, "x": 99, "y": 100, "l1": 102, "r1": 103, "l2": 104, "r2": 105,
    "thumbl": 106, "thumbr": 107, "start": 108, "select": 109, "mode": 110,
}
# A button the system itself may consume (the gamepad Home), so a runner treats its delivery as advisory.
ADVISORY_BUTTONS = frozenset({"mode"})
STICK_AXES = ("x", "y", "z", "rz")
TRIGGER_AXES = ("ltrigger", "rtrigger")
# The hat's eight directions, clockwise from up, as (HAT_X, HAT_Y).
HAT_AXES = ((0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1))


@dataclass(frozen=True)
class Identity:
    bus: int
    vendor: int
    product: int
    version: int
    name: str


@dataclass(frozen=True)
class State:
    """What a controller's elements read. Sticks and triggers are 8-bit, with 128 the stick's centre."""
    buttons: frozenset[str] = frozenset()
    left: tuple[int, int] = (128, 128)
    right: tuple[int, int] = (128, 128)
    left_trigger: int = 0
    right_trigger: int = 0
    hat: int | None = None  # 0..7 clockwise from up, None for released


NEUTRAL = State()


# ---- HID report descriptors, assembled from items.

def _unsigned_size(value: int) -> tuple[int, int]:
    return (1, 1) if value < 1 << 8 else (2, 2) if value < 1 << 16 else (4, 3)


def _signed_size(value: int) -> tuple[int, int]:
    return (1, 1) if -128 <= value < 128 else (2, 2) if -32768 <= value < 32768 else (4, 3)


def _item(kind: int, tag: int, value: int | None = None, signed: bool = False) -> bytes:
    if value is None:
        return bytes([tag << 4 | kind << 2])
    size, code = (_signed_size if signed else _unsigned_size)(value)
    return bytes([tag << 4 | kind << 2 | code]) + value.to_bytes(size, "little", signed=signed)


def usage_page(page: int) -> bytes: return _item(1, 0x0, page)
def logical_minimum(value: int) -> bytes: return _item(1, 0x1, value, signed=True)
def logical_maximum(value: int) -> bytes: return _item(1, 0x2, value, signed=True)
def physical_minimum(value: int) -> bytes: return _item(1, 0x3, value, signed=True)
def physical_maximum(value: int) -> bytes: return _item(1, 0x4, value, signed=True)
def unit(value: int) -> bytes: return _item(1, 0x6, value)
def report_size(bits: int) -> bytes: return _item(1, 0x7, bits)
def report_id(number: int) -> bytes: return _item(1, 0x8, number)
def report_count(count: int) -> bytes: return _item(1, 0x9, count)
def usage(code: int) -> bytes: return _item(2, 0x0, code)
def usage_minimum(code: int) -> bytes: return _item(2, 0x1, code)
def usage_maximum(code: int) -> bytes: return _item(2, 0x2, code)
def collection(kind: int) -> bytes: return _item(0, 0xA, kind)
def end_collection() -> bytes: return _item(0, 0xC)


DATA_VARIABLE = 0x02
CONSTANT = 0x03
DATA_VARIABLE_NULL_STATE = 0x42


def input_item(flags: int) -> bytes: return _item(0, 0x8, flags)
def output_item(flags: int) -> bytes: return _item(0, 0x9, flags)
def feature_item(flags: int) -> bytes: return _item(0, 0xB, flags)


def _field(size: int, count: int, low: int, high: int, kind=input_item, flags: int = DATA_VARIABLE) -> bytes:
    return logical_minimum(low) + logical_maximum(high) + report_size(size) + report_count(count) + kind(flags)


def _padding(bits: int, kind=input_item) -> bytes:
    return report_size(bits) + report_count(1) + kind(CONSTANT)


def _hat(low: int, high: int) -> bytes:
    return (usage_page(0x01) + usage(0x39) + physical_minimum(0) + physical_maximum(315) + unit(0x14)
            + _field(4, 1, low, high, flags=DATA_VARIABLE_NULL_STATE) + _padding(4))


def _buttons(count: int) -> bytes:
    return (usage_page(0x09) + usage_minimum(1) + usage_maximum(count) + _field(1, count, 0, 1)
            + _padding(16 - count))


def _gamepad(*body: bytes) -> bytes:
    return usage_page(0x01) + usage(0x05) + collection(0x01) + b"".join(body) + end_collection()


def generic_descriptor() -> bytes:
    """Report 1 of 9 bytes: X, Y, Z, Rz, Brake, Accelerator (8 bits each), the hat, fifteen buttons."""
    return _gamepad(
        report_id(1),
        usage_page(0x01), usage(0x30), usage(0x31), usage(0x32), usage(0x35), _field(8, 4, 0, 255),
        usage_page(0x02), usage(0xC5), usage(0xC4), _field(8, 2, 0, 255),
        _hat(0, 7),
        _buttons(15))


def xbox_descriptor() -> bytes:
    """Report 1 of 15 bytes (sticks of 16 bits, triggers of 10 bits, the hat from 1, fifteen buttons) and the
    force feedback output report 3 of 8 bytes that hid-microsoft's xb1s_ff_report fills."""
    force_feedback = (
        usage_page(0x0F) + usage(0x21) + collection(0x02) + report_id(3)
        + usage(0x97) + _field(4, 1, 0, 1, output_item) + _padding(4, output_item)
        + usage(0x70) + _field(8, 4, 0, 100, output_item)
        + usage(0x50) + _field(8, 1, 0, 255, output_item)
        + usage(0xA7) + _field(8, 1, 0, 255, output_item)
        + usage(0x7C) + _field(8, 1, 0, 255, output_item)
        + end_collection())
    return _gamepad(
        report_id(1),
        usage_page(0x01), usage(0x30), usage(0x31), usage(0x32), usage(0x35), _field(16, 4, 0, 65535),
        usage_page(0x02), usage(0xC5), _field(10, 1, 0, 1023), _padding(6),
        usage(0xC4), _field(10, 1, 0, 1023), _padding(6),
        _hat(1, 8),
        _buttons(15),
        force_feedback)


def dualsense_descriptor() -> bytes:
    """What hid-playstation needs of a descriptor: input report 1 of 63 bytes, output report 2 of 62, and the
    feature reports 5 (40 bytes), 9 (19) and 0x20 (63). The driver reads the bytes itself; the fields only give
    them their sizes: six axes, a counter, the hat byte, three bytes of buttons, 52 bytes of sensors and status."""
    vendor = usage_page(0xFF00)
    return _gamepad(
        report_id(1),
        usage_page(0x01), usage(0x30), usage(0x31), usage(0x32), usage(0x35), usage(0x33), usage(0x34),
        _field(8, 6, 0, 255),
        vendor, usage(0x20), _field(8, 1, 0, 255),
        _hat(0, 7),
        vendor, usage(0x21), _field(1, 24, 0, 1),
        usage(0x22), _field(8, 52, 0, 255),
        report_id(2), usage(0x23), _field(8, 62, 0, 255, output_item),
        report_id(5), usage(0x24), _field(8, 40, 0, 255, feature_item),
        report_id(9), usage(0x25), _field(8, 19, 0, 255, feature_item),
        report_id(0x20), usage(0x26), _field(8, 63, 0, 255, feature_item))


def report_sizes(descriptor: bytes) -> dict[tuple[str, int], int]:
    """The data bytes of each report of a descriptor, keyed by (kind, report id): a walk of its main items that
    adds up report size times count. An error is a ValueError, so a descriptor that does not parse is found
    without a kernel."""
    sizes: dict[tuple[str, int], int] = {}
    size = count = identifier = 0
    depth = 0
    position = 0
    while position < len(descriptor):
        prefix = descriptor[position]
        length = (0, 1, 2, 4)[prefix & 3]
        if prefix == 0xFE or position + 1 + length > len(descriptor):
            raise ValueError("a descriptor item runs past the end")
        value = int.from_bytes(descriptor[position + 1:position + 1 + length], "little")
        kind, tag = (prefix >> 2) & 3, prefix >> 4
        position += 1 + length
        if kind == 1 and tag == 0x7:
            size = value
        elif kind == 1 and tag == 0x9:
            count = value
        elif kind == 1 and tag == 0x8:
            identifier = value
        elif kind == 0 and tag in (0x8, 0x9, 0xB):
            name = {0x8: "input", 0x9: "output", 0xB: "feature"}[tag]
            sizes[(name, identifier)] = sizes.get((name, identifier), 0) + size * count
        elif kind == 0 and tag == 0xA:
            depth += 1
        elif kind == 0 and tag == 0xC:
            depth -= 1
            if depth < 0:
                raise ValueError("an unbalanced end of collection")
    if depth != 0:
        raise ValueError("a collection is not closed")
    for key, bits in sizes.items():
        if bits % 8:
            raise ValueError(f"{key[0]} report {key[1]} is {bits} bits, not whole bytes")
    return {key: bits // 8 for key, bits in sizes.items()}


# ---- The families.

@dataclass
class Family:
    key: str
    identity: Identity
    descriptor: bytes
    # The Android key layout the device must get, and what Android reports for it.
    key_layout: str
    buttons: tuple[str, ...]
    vibrators: int
    lights: bool = False
    battery: bool = False
    notes: str = ""

    def uniq(self, slot: int) -> str:
        return ""

    def report(self, state: State, sequence: int = 0) -> bytes:
        raise NotImplementedError

    def feature_reply(self, slot: int, number: int) -> bytes | None:
        """The data of the feature report `number`, whole with its ID, or None for a report the device lacks."""
        return None

    def create_frame(self, slot: int) -> bytes:
        identity = self.identity
        return wire.create(slot, identity.bus, identity.vendor, identity.product, identity.version, 0,
                           identity.name, self.uniq(slot), self.descriptor)

    def input_frame(self, slot: int, state: State, sequence: int = 0) -> bytes:
        return wire.input_report(slot, self.report(state, sequence))

    def decode_output(self, data: bytes) -> object | None:
        return None


class GenericFamily(Family):
    # Button numbers of the descriptor's fifteen (1 based): their evdev codes run from BTN_SOUTH, and Generic.kl
    # reads 0x133 and 0x134 as X and Y.
    BUTTON_NUMBERS = {"a": 1, "b": 2, "x": 4, "y": 5, "l1": 7, "r1": 8, "l2": 9, "r2": 10, "select": 11,
                      "start": 12, "mode": 13, "thumbl": 14, "thumbr": 15}

    def __init__(self) -> None:
        super().__init__("generic", Identity(wire.BUS_USB, 0, 0, 0x0100, "Game Controller"), generic_descriptor(),
                         "Generic.kl", tuple(self.BUTTON_NUMBERS), vibrators=0)

    def report(self, state: State, sequence: int = 0) -> bytes:
        mask = sum(1 << (self.BUTTON_NUMBERS[name] - 1) for name in state.buttons)
        hat = 8 if state.hat is None else state.hat
        return struct.pack("<B4BBBBH", 1, *state.left, *state.right, state.left_trigger, state.right_trigger,
                           hat, mask)


class XboxFamily(Family):
    BUTTON_NUMBERS = {"a": 1, "b": 2, "x": 4, "y": 5, "l1": 7, "r1": 8, "select": 11, "start": 12, "mode": 13,
                      "thumbl": 14, "thumbr": 15}

    def __init__(self) -> None:
        super().__init__("xbox", Identity(wire.BUS_BLUETOOTH, 0x045E, 0x0B13, 0x0100, "Xbox Wireless Controller"),
                         xbox_descriptor(), "Generic.kl", tuple(self.BUTTON_NUMBERS), vibrators=2)

    def report(self, state: State, sequence: int = 0) -> bytes:
        mask = sum(1 << (self.BUTTON_NUMBERS[name] - 1) for name in state.buttons)
        hat = 0 if state.hat is None else state.hat + 1
        trigger = lambda value: (value * 1023 + 127) // 255  # noqa: E731
        return struct.pack("<BHHHHHHBH", 1, state.left[0] * 257, state.left[1] * 257, state.right[0] * 257,
                           state.right[1] * 257, trigger(state.left_trigger), trigger(state.right_trigger), hat, mask)

    def decode_output(self, data: bytes) -> XboxOutput | None:
        """The force feedback report: id 3, the enable bits, four magnitudes of which the third is the strong
        (left) motor and the fourth the weak (right), the duration, the delay and the loop count."""
        if len(data) != 9 or data[0] != 3:
            return None
        return XboxOutput(enable=data[1], strong=data[4], weak=data[5], duration=data[6], loops=data[8])


@dataclass(frozen=True)
class XboxOutput:
    enable: int
    strong: int
    weak: int
    duration: int
    loops: int

    @property
    def rumble(self) -> tuple[int, int]:
        return self.strong, self.weak


# hid-playstation's DualSense.
DS_FEATURE_VERSION_2_33 = (2 << 8) | 33
DS_VALID_FLAG0_COMPATIBLE_VIBRATION = 1 << 0
DS_VALID_FLAG0_HAPTICS_SELECT = 1 << 1
DS_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE = 1 << 2
DS_VALID_FLAG1_PLAYER_INDICATOR_CONTROL_ENABLE = 1 << 4
DS_VALID_FLAG2_LIGHTBAR_SETUP_CONTROL_ENABLE = 1 << 1
DS_VALID_FLAG2_COMPATIBLE_VIBRATION2 = 1 << 2
DS_BATTERY_UNITS = 8  # 80 to 89 per cent, which the driver reports as 85


@dataclass(frozen=True)
class DualSenseOutput:
    valid_flag0: int
    valid_flag1: int
    valid_flag2: int
    motor_left: int
    motor_right: int
    lightbar: tuple[int, int, int]
    player_leds: int
    lightbar_setup: int

    @property
    def rumble_valid(self) -> bool:
        """Whether the report carries rumble: the new firmware's flag in flag 2 with the haptics select."""
        return bool(self.valid_flag2 & DS_VALID_FLAG2_COMPATIBLE_VIBRATION2) and bool(
            self.valid_flag0 & DS_VALID_FLAG0_HAPTICS_SELECT)

    @property
    def rumble(self) -> tuple[int, int]:
        return self.motor_left, self.motor_right

    @property
    def lightbar_valid(self) -> bool:
        return bool(self.valid_flag1 & DS_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE)

    @property
    def player_leds_valid(self) -> bool:
        return bool(self.valid_flag1 & DS_VALID_FLAG1_PLAYER_INDICATOR_CONTROL_ENABLE)


class DualSenseFamily(Family):
    BUTTON_BITS = {"x": (0, 4), "a": (0, 5), "b": (0, 6), "y": (0, 7), "l1": (1, 0), "r1": (1, 1), "l2": (1, 2),
                   "r2": (1, 3), "select": (1, 4), "start": (1, 5), "thumbl": (1, 6), "thumbr": (1, 7),
                   "mode": (2, 0)}

    def __init__(self) -> None:
        super().__init__("dualsense", Identity(wire.BUS_USB, 0x054C, 0x0CE6, 0x0100, "DualSense Wireless Controller"),
                         dualsense_descriptor(), "Vendor_054c_Product_0ce6.kl", tuple(self.BUTTON_BITS),
                         vibrators=2, lights=True, battery=True,
                         notes="the driver raises the version to 0x8100 and sets uniq to the pairing address")

    @staticmethod
    def mac(slot: int) -> bytes:
        """A locally administered address that depends only on the slot (the driver prints it reversed)."""
        return bytes([slot, 0x4E, 0x52, 0x41, 0x4B, 0x02])

    def report(self, state: State, sequence: int = 0, battery_units: int = DS_BATTERY_UNITS) -> bytes:
        buttons = [0, 0, 0, 0]
        for name in state.buttons:
            byte, bit = self.BUTTON_BITS[name]
            buttons[byte] |= 1 << bit
        buttons[0] |= 8 if state.hat is None else state.hat
        payload = bytearray(63)
        payload[0:6] = bytes([state.left[0], state.left[1], state.right[0], state.right[1], state.left_trigger,
                              state.right_trigger])
        payload[6] = sequence & 0xFF
        payload[7:11] = bytes(buttons)
        # Both touch points inactive (contact bit 7), the sensors at rest, the status block after them.
        payload[32] = 0x80
        payload[36] = 0x80
        payload[52] = battery_units & 0x0F
        return bytes([1]) + bytes(payload)

    def feature_reply(self, slot: int, number: int) -> bytes | None:
        if number == 0x05:  # calibration: zero biases, symmetric ranges, so no axis divides by zero
            data = bytearray(41)
            data[0] = 0x05
            struct.pack_into("<3h", data, 1, 0, 0, 0)
            struct.pack_into("<6h", data, 7, 8192, -8192, 8192, -8192, 8192, -8192)
            struct.pack_into("<2h", data, 19, 540, 540)
            struct.pack_into("<6h", data, 23, 8192, -8192, 8192, -8192, 8192, -8192)
            return bytes(data)
        if number == 0x09:  # pairing: the address the driver puts in uniq
            return bytes([0x09]) + self.mac(slot) + bytes(13)
        if number == 0x20:  # firmware: hardware and firmware versions, the feature version at byte 44
            data = bytearray(64)
            data[0] = 0x20
            struct.pack_into("<II", data, 24, 0x00000100, 0x01000200)
            struct.pack_into("<H", data, 44, DS_FEATURE_VERSION_2_33)
            return bytes(data)
        return None

    def decode_output(self, data: bytes) -> DualSenseOutput | None:
        """The USB output report: id 2, then valid flags 0 and 1, the DualShock 4 compatible motors (right, then
        left), the audio and mute fields, 27 reserved bytes, audio control 2, valid flag 2, the lightbar setup, the
        brightness, the player LEDs and the lightbar's red, green and blue."""
        if len(data) != 63 or data[0] != 2:
            return None
        return DualSenseOutput(valid_flag0=data[1], valid_flag1=data[2], valid_flag2=data[39],
                               motor_right=data[3], motor_left=data[4], lightbar_setup=data[42],
                               player_leds=data[44], lightbar=(data[45], data[46], data[47]))


def families() -> dict[str, Family]:
    return {family.key: family for family in (GenericFamily(), XboxFamily(), DualSenseFamily())}


# ---- What Android must report for a family, from its key layout and the driver's axes.

def expected_stick(raw: int) -> float:
    """The normalised value Android reports for an 8-bit stick reading (the range maps to -1 to 1)."""
    return raw / 255 * 2 - 1


def expected_trigger(raw: int) -> float:
    return raw / 255


def expected_state_axes(state: State) -> dict[str, float]:
    hat_x, hat_y = (0, 0) if state.hat is None else HAT_AXES[state.hat]
    return {
        "x": expected_stick(state.left[0]), "y": expected_stick(state.left[1]),
        "z": expected_stick(state.right[0]), "rz": expected_stick(state.right[1]),
        "ltrigger": expected_trigger(state.left_trigger), "rtrigger": expected_trigger(state.right_trigger),
        "hat_x": float(hat_x), "hat_y": float(hat_y),
    }
