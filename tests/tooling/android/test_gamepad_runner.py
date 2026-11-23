#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The gamepad runner without a guest.

run_android_gamepad.py plays the host application against a booted release: it creates controllers over the
relay's vsock port, sends their reports, answers the drivers' requests and reads the probe app's state, `dumpsys
input`, `/proc` and the log. This test runs everything of it that does not need the guest: the host's end of the
wire against the golden frames of tests/unit/andarium/fixtures and against the C++ header's constants, the
controller families against the drivers' own report structures, each parser on a text in the layout the guest
prints, the checks on records they must accept and records they must reject, and the reader thread of the
connection against a fake relay on loopback.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import dataclasses
import importlib.util
import io
import json
from pathlib import Path
import re
import socket
import struct
import sys
import threading
import types
import unittest
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
REPOSITORY = TESTS.parent
INTEGRATION = TESTS / "integration/android"
sys.path.insert(0, str(INTEGRATION))
import gamepad_families as families  # noqa: E402
import gamepad_wire as wire  # noqa: E402

SCRIPT = INTEGRATION / "run_android_gamepad.py"
SPEC = importlib.util.spec_from_file_location("run_android_gamepad", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

HEADER = REPOSITORY / "android/lineage-24/overlay/gamepad/gamepad_protocol.hpp"
FIXTURE = TESTS / "unit/andarium/fixtures/gamepad-golden-frames.txt"


def golden_frames() -> dict[str, bytes]:
    frames = {}
    for line in FIXTURE.read_text().splitlines():
        if line and not line.startswith("#"):
            name, _, text = line.partition(":")
            frames[name.strip()] = bytes.fromhex(text.replace(" ", ""))
    return frames


class WireTests(unittest.TestCase):
    def test_the_constants_are_the_cpp_headers(self) -> None:
        text = HEADER.read_text()
        values = {name: int(value) for name, value in re.findall(r"constexpr (?:std::size_t|uint32_t|uint16_t) (\w+) = (\d+);", text)}
        pairs = {
            "port": wire.PORT, "header_size": wire.HEADER_SIZE, "maximum_payload": wire.MAXIMUM_PAYLOAD,
            "slot_count": wire.SLOT_COUNT, "maximum_descriptor": wire.MAXIMUM_DESCRIPTOR,
            "maximum_report": wire.MAXIMUM_REPORT, "name_size": wire.NAME_SIZE, "uniq_size": wire.UNIQ_SIZE,
            "protocol_version": wire.PROTOCOL_VERSION,
        }
        for name, expected in pairs.items():
            with self.subTest(constant=name):
                self.assertEqual(values[name], expected)
        types = {name: int(value) for name, value in re.findall(r"^\s+(\w+) = (\d+),$", text, re.MULTILINE)}
        for name, expected in {"create": wire.CREATE, "destroy": wire.DESTROY, "input": wire.INPUT,
                               "get_report_reply": wire.GET_REPORT_REPLY, "set_report_reply": wire.SET_REPORT_REPLY,
                               "hello": wire.HELLO, "created": wire.CREATED, "event": wire.EVENT,
                               "output": wire.OUTPUT, "get_report": wire.GET_REPORT,
                               "set_report": wire.SET_REPORT}.items():
            with self.subTest(frame=name):
                self.assertEqual(types[name], expected)

    def test_the_fixed_parts_are_the_headers(self) -> None:
        values = {name: int(value) for name, value in re.findall(
            r"constexpr std::size_t (\w+_(?:fixed_size|payload_size)) = (\d+);", HEADER.read_text())}
        descriptor = bytes([0x05, 0x01])
        self.assertEqual(len(wire.create(0, 3, 1, 2, 3, 0, "n", "u", descriptor)),
                         wire.HEADER_SIZE + values["create_fixed_size"] + len(descriptor))
        self.assertEqual(len(wire.destroy(0)), wire.HEADER_SIZE + values["destroy_payload_size"])
        self.assertEqual(len(wire.input_report(0, b"\1")), wire.HEADER_SIZE + values["input_fixed_size"] + 1)
        self.assertEqual(len(wire.get_report_reply(0, 1, b"\1")),
                         wire.HEADER_SIZE + values["get_report_reply_fixed_size"] + 1)
        self.assertEqual(len(wire.set_report_reply(0, 1)), wire.HEADER_SIZE + values["set_report_reply_payload_size"])

    def test_the_encoders_reproduce_the_golden_frames(self) -> None:
        golden = golden_frames()
        self.assertEqual(wire.destroy(1), golden["destroy-slot-1"])
        self.assertEqual(wire.input_report(0, bytes([0x01, 0x80, 0x80])), golden["input-slot-0"])

    def test_the_reader_reads_the_golden_hello_in_pieces(self) -> None:
        frame = golden_frames()["hello"]
        for size in range(1, len(frame) + 1):
            with self.subTest(piece=size):
                reader = wire.GuestReader()
                found = []
                for start in range(0, len(frame), size):
                    found += reader.feed(frame[start:start + size])
                self.assertEqual(found, [wire.Hello(wire.PROTOCOL_VERSION, wire.SLOT_COUNT)])

    def test_the_reader_refuses_the_golden_rejects(self) -> None:
        for name in ("reject-magic", "reject-type", "reject-length"):
            with self.subTest(frame=name):
                with self.assertRaises(wire.WireError):
                    wire.GuestReader().feed(golden_frames()[name])

    def test_create_lays_out_its_fields(self) -> None:
        descriptor = bytes(range(1, 21))
        frame = wire.create(3, wire.BUS_USB, 0x054C, 0x0CE6, 0x0100, 7, "name", "uniq", descriptor, flags=2)
        payload = frame[wire.HEADER_SIZE:]
        self.assertEqual(frame[:4], b"AGP1")
        self.assertEqual(struct.unpack("<HH", frame[4:8]), (wire.CREATE, len(payload)))
        self.assertEqual(struct.unpack("<BBHIIII", payload[:20]), (3, 2, wire.BUS_USB, 0x054C, 0x0CE6, 0x0100, 7))
        self.assertEqual(payload[20:148], b"name" + bytes(124))
        self.assertEqual(payload[148:212], b"uniq" + bytes(60))
        self.assertEqual(struct.unpack("<H", payload[212:214])[0], len(descriptor))
        self.assertEqual(payload[214:], descriptor)

    def test_a_name_must_leave_room_for_its_terminator(self) -> None:
        wire.fixed_text("x" * (wire.NAME_SIZE - 1), wire.NAME_SIZE)
        with self.assertRaises(wire.WireError):
            wire.fixed_text("x" * wire.NAME_SIZE, wire.NAME_SIZE)

    def test_ranges(self) -> None:
        with self.assertRaises(wire.WireError):
            wire.create(0, 3, 0, 0, 0, 0, "n", "", b"")
        with self.assertRaises(wire.WireError):
            wire.input_report(0, bytes(wire.MAXIMUM_REPORT + 1))
        with self.assertRaises(wire.WireError):
            wire.input_report(0, b"")

    def test_replies(self) -> None:
        reply = wire.get_report_reply(2, 0x01020304, b"\xaa\xbb", error=5)
        self.assertEqual(reply[wire.HEADER_SIZE:], struct.pack("<BxHIH", 2, 5, 0x01020304, 2) + b"\xaa\xbb")
        reply = wire.set_report_reply(1, 9)
        self.assertEqual(reply[wire.HEADER_SIZE:], struct.pack("<BxHI", 1, 0, 9))

    def test_the_guests_frames_decode(self) -> None:
        reader = wire.GuestReader()
        stream = b"".join([
            wire.frame(wire.CREATED, struct.pack("<BxxxI", 2, (-19) & 0xFFFFFFFF)),
            wire.frame(wire.EVENT, struct.pack("<BBxx", 1, 2)),
            wire.frame(wire.OUTPUT, struct.pack("<BBH", 3, wire.OUTPUT_REPORT, 3) + b"\2\3\4"),
            wire.frame(wire.GET_REPORT, struct.pack("<BBBxI", 0, wire.FEATURE_REPORT, 5, 77)),
            wire.frame(wire.SET_REPORT, struct.pack("<BBBxIH", 4, wire.FEATURE_REPORT, 9, 78, 2) + b"\x07\x08"),
        ])
        self.assertEqual(reader.feed(stream), [
            wire.Created(2, -19), wire.Event(1, "open"), wire.Output(3, wire.OUTPUT_REPORT, b"\2\3\4"),
            wire.GetReport(0, wire.FEATURE_REPORT, 5, 77),
            wire.SetReport(4, wire.FEATURE_REPORT, 9, 78, b"\x07\x08")])

    def test_a_frame_the_relay_never_sends_is_refused(self) -> None:
        for frame_type, payload in ((wire.CREATE, bytes(214)), (wire.HELLO, b"\1"), (wire.EVENT, struct.pack("<BBxx", 0, 9)),
                                    (wire.OUTPUT, struct.pack("<BBH", 0, 1, 5) + b"\0")):
            with self.subTest(frame_type=frame_type):
                with self.assertRaises(wire.WireError):
                    wire.GuestReader().feed(wire.frame(frame_type, payload))


# The driver's own structures, written from hid-playstation.c: the common part of the DualSense input report after
# its report ID (63 bytes), and the USB output report (63 bytes).
DS_INPUT = struct.Struct("<6BB4B4B3h3hIB8B12B3B8B")
DS_OUTPUT = struct.Struct("<B10B27BBB2BBBB3B15B")


class FamilyTests(unittest.TestCase):
    def test_descriptors_parse_to_the_sizes_the_drivers_need(self) -> None:
        self.assertEqual(families.report_sizes(families.dualsense_descriptor()),
                         {("input", 1): 63, ("output", 2): 62, ("feature", 5): 40, ("feature", 9): 19,
                          ("feature", 0x20): 63})
        self.assertEqual(families.report_sizes(families.xbox_descriptor()), {("input", 1): 15, ("output", 3): 8})
        self.assertEqual(families.report_sizes(families.generic_descriptor()), {("input", 1): 9})
        self.assertEqual(DS_INPUT.size, 63)
        self.assertEqual(DS_OUTPUT.size, 63)

    def test_a_broken_descriptor_does_not_parse(self) -> None:
        with self.assertRaises(ValueError):
            families.report_sizes(families.generic_descriptor()[:-1])
        with self.assertRaises(ValueError):
            families.report_sizes(families.generic_descriptor() + families.end_collection())
        with self.assertRaises(ValueError):  # a report of a whole number of bits but not of bytes
            families.report_sizes(families.report_id(1) + families.report_size(3) + families.report_count(1)
                                  + families.input_item(2))

    def test_the_reports_have_the_sizes_the_descriptors_declare(self) -> None:
        for key, family in families.families().items():
            sizes = families.report_sizes(family.descriptor)
            with self.subTest(family=key):
                self.assertEqual(len(family.report(families.NEUTRAL)) - 1, sizes[("input", 1)])

    def test_the_names_the_runner_matches_by_are_unique(self) -> None:
        names = [family.identity.name for family in families.families().values()]
        self.assertEqual(len(set(names)), len(names))

    def test_every_button_has_an_android_key_code(self) -> None:
        for key, family in families.families().items():
            for button in family.buttons:
                with self.subTest(family=key, button=button):
                    self.assertIn(button, families.KEYCODES)

    def test_the_dualsense_report_is_read_as_the_drivers_structure(self) -> None:
        family = families.DualSenseFamily()
        state = families.State(buttons=frozenset({"a", "l1", "mode", "thumbr"}), left=(1, 2), right=(3, 4),
                               left_trigger=5, right_trigger=6, hat=3)
        report = family.report(state, sequence=9)
        self.assertEqual(report[0], 1)
        fields = DS_INPUT.unpack(report[1:])
        self.assertEqual(fields[0:6], (1, 2, 3, 4, 5, 6))  # x, y, rx, ry, z, rz
        self.assertEqual(fields[6], 9)
        buttons = fields[7:11]
        self.assertEqual(buttons[0] & 0x0F, 3)  # the hat: down-right
        self.assertEqual(buttons[0] & 0xF0, 1 << 5)  # cross
        self.assertEqual(buttons[1], (1 << 0) | (1 << 7))  # L1 and R3
        self.assertEqual(buttons[2] & 0x07, 1)  # the PS button
        # The structure's fields after the buttons: reserved (4), gyro (3), accel (3), timestamp, reserved, two touch
        # points of 4 bytes, reserved (12), status (3), reserved (8).
        self.assertEqual((fields[23], fields[27]), (0x80, 0x80))  # touch point contacts: inactive
        self.assertEqual(fields[43] & 0x0F, families.DS_BATTERY_UNITS)  # status[0]: battery capacity
        self.assertEqual(fields[43] >> 4, 0)  # not charging

    def test_the_dualsense_neutral_has_the_hat_centred_and_the_sticks_in_the_middle(self) -> None:
        report = families.DualSenseFamily().report(families.NEUTRAL)
        self.assertEqual(report[1:7], bytes([128, 128, 128, 128, 0, 0]))
        self.assertEqual(report[8] & 0x0F, 8)

    def test_the_dualsense_feature_replies_are_what_the_driver_reads(self) -> None:
        family = families.DualSenseFamily()
        calibration = family.feature_reply(0, 0x05)
        self.assertEqual((len(calibration), calibration[0]), (41, 5))
        (pitch_bias, yaw_bias, roll_bias) = struct.unpack_from("<3h", calibration, 1)
        pitch_plus, pitch_minus, yaw_plus, yaw_minus, roll_plus, roll_minus = struct.unpack_from("<6h", calibration, 7)
        speed_plus, speed_minus = struct.unpack_from("<2h", calibration, 19)
        # The driver divides by these.
        self.assertGreater(abs(pitch_plus - pitch_bias) + abs(pitch_minus - pitch_bias), 0)
        self.assertGreater(abs(yaw_plus - yaw_bias) + abs(yaw_minus - yaw_bias), 0)
        self.assertGreater(abs(roll_plus - roll_bias) + abs(roll_minus - roll_bias), 0)
        self.assertGreater(speed_plus + speed_minus, 0)
        for offset in (23, 27, 31):
            plus, minus = struct.unpack_from("<2h", calibration, offset)
            self.assertNotEqual(plus - minus, 0)
        firmware = family.feature_reply(0, 0x20)
        self.assertEqual((len(firmware), firmware[0]), (64, 0x20))
        update_version = struct.unpack_from("<H", firmware, 44)[0]
        self.assertGreaterEqual(update_version, (2 << 8) | 21)  # the firmware that has the new vibration
        pairing = family.feature_reply(3, 0x09)
        self.assertEqual((len(pairing), pairing[0]), (20, 9))
        self.assertIsNone(family.feature_reply(0, 0x99))

    def test_the_pairing_address_depends_only_on_the_slot(self) -> None:
        family = families.DualSenseFamily()
        addresses = {family.feature_reply(slot, 0x09)[1:7] for slot in range(wire.SLOT_COUNT)}
        self.assertEqual(len(addresses), wire.SLOT_COUNT)
        self.assertEqual(family.feature_reply(2, 0x09), family.feature_reply(2, 0x09))
        for address in addresses:
            self.assertEqual(address[5] & 0x03, 0x02)  # locally administered, unicast (the driver prints it reversed)

    def test_the_dualsense_output_is_read_as_the_drivers_structure(self) -> None:
        family = families.DualSenseFamily()
        values = [2,
                  families.DS_VALID_FLAG0_HAPTICS_SELECT, families.DS_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE
                  | families.DS_VALID_FLAG1_PLAYER_INDICATOR_CONTROL_ENABLE,
                  40, 200,  # motor right, then left
                  0, 0, 0, 0, 0, 0]
        values += [0] * 27
        values += [0, families.DS_VALID_FLAG2_COMPATIBLE_VIBRATION2, 0, 0, 0, 0, 0b10101, 0x10, 0x20, 0x30]
        values += [0] * 15
        report = DS_OUTPUT.pack(*values)
        self.assertEqual(len(report), 63)
        output = family.decode_output(report)
        self.assertEqual(output.rumble, (200, 40))
        self.assertTrue(output.rumble_valid)
        self.assertEqual(output.lightbar, (0x10, 0x20, 0x30))
        self.assertTrue(output.lightbar_valid)
        self.assertTrue(output.player_leds_valid)
        self.assertEqual(output.player_leds, 0b10101)
        self.assertIsNone(family.decode_output(report[:-1]))
        self.assertIsNone(family.decode_output(b"\x03" + report[1:]))

    def test_an_output_without_the_rumble_flags_is_not_rumble(self) -> None:
        values = [2, 0, 0, 9, 9] + [0] * 6 + [0] * 27 + [0] * 10 + [0] * 15
        output = families.DualSenseFamily().decode_output(DS_OUTPUT.pack(*values))
        self.assertFalse(output.rumble_valid)

    def test_the_xbox_output_is_the_force_feedback_report(self) -> None:
        # hid-microsoft's xb1s_ff_report: id 3, enable (weak 1, strong 2), four magnitudes (the third is the strong
        # motor, the fourth the weak), duration, start delay, loop count.
        report = bytes([3, 0b11, 0, 0, 80, 20, 255, 0, 255])
        output = families.XboxFamily().decode_output(report)
        self.assertEqual(output.rumble, (80, 20))
        self.assertEqual((output.enable, output.duration, output.loops), (3, 255, 255))
        self.assertIsNone(families.XboxFamily().decode_output(report[:-1]))
        self.assertIsNone(families.XboxFamily().decode_output(bytes([4]) + report[1:]))

    def test_the_generic_report(self) -> None:
        family = families.GenericFamily()
        report = family.report(families.State(buttons=frozenset({"a", "x", "mode", "thumbr"}), left=(9, 8),
                                              right=(7, 6), left_trigger=5, right_trigger=4, hat=7))
        self.assertEqual(report[:9], bytes([1, 9, 8, 7, 6, 5, 4, 7]) + report[8:9])
        mask = struct.unpack("<H", report[8:10])[0]
        self.assertEqual(mask, (1 << 0) | (1 << 3) | (1 << 12) | (1 << 14))
        self.assertEqual(family.report(families.NEUTRAL)[7], 8)  # the hat's null state

    def test_the_xbox_report(self) -> None:
        family = families.XboxFamily()
        report = family.report(families.State(left=(255, 0), right=(128, 128), left_trigger=255, right_trigger=0,
                                              hat=0, buttons=frozenset({"b", "y", "select"})))
        self.assertEqual(len(report), 16)
        fields = struct.unpack("<BHHHHHHBH", report)
        self.assertEqual(fields[1:5], (65535, 0, 32896, 32896))
        self.assertEqual(fields[5:7], (1023, 0))
        self.assertEqual(fields[7], 1)  # the hat counts from 1, up
        self.assertEqual(fields[8], (1 << 1) | (1 << 4) | (1 << 10))
        self.assertEqual(struct.unpack("<BHHHHHHBH", family.report(families.NEUTRAL))[7], 0)

    def test_the_expected_axes(self) -> None:
        axes = families.expected_state_axes(families.State(left=(0, 255), right=(128, 128), left_trigger=255,
                                                           right_trigger=0, hat=3))
        self.assertAlmostEqual(axes["x"], -1.0)
        self.assertAlmostEqual(axes["y"], 1.0)
        self.assertAlmostEqual(axes["z"], 128 / 255 * 2 - 1)
        self.assertAlmostEqual(axes["ltrigger"], 1.0)
        self.assertEqual((axes["hat_x"], axes["hat_y"]), (1.0, 1.0))
        neutral = families.expected_state_axes(families.NEUTRAL)
        self.assertEqual((neutral["hat_x"], neutral["hat_y"]), (0.0, 0.0))

    def test_every_step_changes_the_state_so_that_it_yields_a_sample(self) -> None:
        for key, family in families.families().items():
            for name, steps in (("buttons", runner.button_steps(family)), ("axes", runner.axis_steps(family))):
                with self.subTest(family=key, steps=name):
                    states = [families.NEUTRAL] + [state for _, state in steps]
                    for before, after in zip(states, states[1:]):
                        self.assertNotEqual(before, after)

    def test_a_create_carries_the_family(self) -> None:
        for key, family in families.families().items():
            with self.subTest(family=key):
                frame = family.create_frame(2)
                payload = frame[wire.HEADER_SIZE:]
                slot, _, bus, vendor, product, version, _ = struct.unpack("<BBHIIII", payload[:20])
                identity = family.identity
                self.assertEqual((slot, bus, vendor, product, version),
                                 (2, identity.bus, identity.vendor, identity.product, identity.version))
                self.assertTrue(payload[20:148].startswith(identity.name.encode()))
                self.assertEqual(payload[214:], family.descriptor)


PROBE_JSON = {
    "nonce": "abc", "failure": "", "uptimeMs": 5, "wallMs": 1700000000000, "keyCount": 2, "motionSampleCount": 3,
    "recording": True,
    "devices": [{
        "id": 7, "name": "DualSense Wireless Controller", "vendor": 0x054C, "product": 0x0CE6,
        "sources": runner.SOURCE_GAMEPAD | runner.SOURCE_JOYSTICK, "controllerNumber": 1, "descriptor": "d",
        "external": True, "keys": [96, 97, 99, 100, 102, 103, 104, 105, 106, 107, 108, 109, 110],
        "ranges": [{"axis": axis, "source": 0, "min": 0.0, "max": 1.0, "flat": 0.0, "fuzz": 0.0}
                   for axis in runner.AXIS_NAMES],
        "vibrators": 2, "battery": {"present": True, "capacity": 0.85, "status": 3},
        "lights": [{"id": 1, "name": "lightbar", "ordinal": 0, "type": 10001, "rgb": True, "brightness": True},
                   {"id": 2, "name": "player", "ordinal": 0, "type": 10002, "rgb": False, "brightness": False}]}],
    "events": [
        {"kind": "key", "device": 7, "code": 96, "action": "down", "repeat": 0, "kernelMs": 100, "receivedMs": 104},
        {"kind": "motion", "device": 7, "x": -1.0, "y": 0.0, "z": 0.0, "rz": 0.0, "ltrigger": 0.0, "rtrigger": 0.0,
         "hat_x": 0.0, "hat_y": 0.0, "kernelMs": 110, "receivedMs": 113}],
    "listener": [{"kind": "added", "id": 7, "ms": 99}],
}


def probe_state(**changes) -> "runner.ProbeState":
    return runner.parse_probe_state(json.dumps({**PROBE_JSON, **changes}))


class ParserTests(unittest.TestCase):
    def test_the_probe_state(self) -> None:
        state = probe_state()
        self.assertEqual((state.nonce, state.key_count, state.motion_samples), ("abc", 2, 3))
        device = state.device_named("DualSense Wireless Controller")
        self.assertEqual((device.id, device.vibrators, device.battery_capacity), (7, 2, 0.85))
        self.assertEqual(device.ranges["AXIS_X"]["max"], 1.0)
        self.assertEqual([event.kind for event in state.events], ["key", "motion"])
        self.assertEqual(state.events[0].code, 96)
        self.assertEqual(state.events[1].axes["x"], -1.0)
        self.assertEqual(state.listener, [("added", 7, 99)])
        self.assertIsNone(state.device_named("Nothing"))

    def test_an_unknown_battery_level_is_none(self) -> None:
        devices = json.loads(json.dumps(PROBE_JSON["devices"]))
        devices[0]["battery"]["capacity"] = None
        self.assertIsNone(probe_state(devices=devices).devices[0].battery_capacity)

    def test_a_state_that_does_not_parse_is_a_failure(self) -> None:
        for text in ("", "{", '{"nonce": "x"}'):
            with self.subTest(text=text):
                with self.assertRaises(runner.Failure):
                    runner.parse_probe_state(text)

    INPUT_DUMP = """\
Input Dispatcher State:
Event Hub State:
  BuiltInKeyboardId: -2
  Devices:
    -1: Virtual
      Classes: 0x40000023
      Path: <virtual>
      Identifier: bus=0x0000, vendor=0x0000, product=0x0000, version=0x0000
      KeyLayoutFile: /system/usr/keylayout/Generic.kl
    5: DualSense Wireless Controller
      Classes: 0x00000181
      Path: /dev/input/event3
      Enabled: true
      Descriptor: 0123abcd
      Location: kernarium-gamepad-0
      ControllerNumber: 1
      UniqueId:
      Identifier: bus=0x0003, vendor=0x054c, product=0x0ce6, version=0x8100
      KeyLayoutFile: /system/usr/keylayout/Vendor_054c_Product_0ce6.kl
      KeyCharacterMapFile:
      ConfigurationFile: /system/usr/idc/Vendor_054c_Product_0ce6.idc
    6: DualSense Wireless Controller Motion Sensors
      Classes: 0x00000400
      Path: /dev/input/event4
      Identifier: bus=0x0003, vendor=0x054c, product=0x0ce6, version=0x8100
    7: Xbox Wireless Controller
      Classes: 0x00000191
      Path: /dev/input/event5
      Identifier: bus=0x0005, vendor=0x045e, product=0x0b13, version=0x0100
      KeyLayoutFile: /system/usr/keylayout/Generic.kl
Input Reader State:
  Device 5: DualSense Wireless Controller
    Sources: 0x01000511
"""

    def test_the_input_dump(self) -> None:
        devices = runner.parse_input_dump(self.INPUT_DUMP)
        self.assertEqual([device.id for device in devices], [-1, 5, 6, 7])
        dualsense = devices[1]
        self.assertEqual((dualsense.name, dualsense.vendor, dualsense.product, dualsense.version, dualsense.bus),
                         ("DualSense Wireless Controller", 0x054C, 0x0CE6, 0x8100, 3))
        self.assertTrue(dualsense.key_layout.endswith("/Vendor_054c_Product_0ce6.kl"))
        self.assertEqual(dualsense.path, "/dev/input/event3")
        self.assertIsNone(devices[2].key_layout)
        self.assertEqual(devices[3].product, 0x0B13)
        self.assertEqual(runner.parse_input_dump("nothing here"), [])

    ANDROID_17_DUMP = """\
Event Hub State:
  BuiltInKeyboardId: -2
  Devices:
    -1: Virtual
      Classes: KEYBOARD | ALPHAKEY | DPAD | EXTERNAL
      Path: <virtual>
      Identifier: bus=0x0000, vendor=0x0000, product=0x0000, version=0x0000, bluetoothAddress=<not set>
      KeyLayoutFile: /system/usr/keylayout/Generic.kl
    3: DualSense Wireless Controller
      Classes: KEYBOARD | GAMEPAD | JOYSTICK | VIBRATOR | EXTERNAL
      Path: /dev/input/event2
      Identifier: bus=0x0003, vendor=0x054c, product=0x0ce6, version=0x8100, bluetoothAddress=<not set>
      KeyLayoutFile: /system/usr/keylayout/Vendor_054c_Product_0ce6.kl
    4: DualSense Wireless Controller Motion Sensors
      Classes: SENSOR | BATTERY | LIGHT | EXTERNAL
      Path: /dev/input/event4
    7: Xbox Wireless Controller
      Classes: KEYBOARD | GAMEPAD | JOYSTICK | VIBRATOR | EXTERNAL
      Path: /dev/input/event6
      Identifier: bus=0x0005, vendor=0x045e, product=0x0b13, version=0x0100, bluetoothAddress=<not set>
      KeyLayoutFile: /system/usr/keylayout/Generic.kl
"""

    def test_the_input_dump_of_android_17_names_the_classes(self) -> None:
        devices = runner.parse_input_dump(self.ANDROID_17_DUMP)
        self.assertEqual([(device.id, device.name) for device in devices],
                         [(-1, "Virtual"), (3, "DualSense Wireless Controller"),
                          (4, "DualSense Wireless Controller Motion Sensors"), (7, "Xbox Wireless Controller")])
        self.assertTrue(devices[1].key_layout.endswith("/Vendor_054c_Product_0ce6.kl"))
        self.assertEqual((devices[3].vendor, devices[3].product), (0x045E, 0x0B13))
        self.assertIsNone(devices[2].key_layout)

    PS = """\
LABEL                          USER           PID  PPID        VSZ    RSS WCHAN            ADDR S NAME
u:r:kernarium_gamepad:s0       nobody         502     1   10809792   4212 0                   0 S kernarium_gamepad
u:r:untrusted_app_34:s0:c122,c256,c512,c768 u0_a122 2182 419 17367492 165132 0                0 S org.example.andarium.gamepadprobe
"""

    def test_the_process_listing_gives_the_user_and_the_label(self) -> None:
        self.assertEqual(runner.parse_ps_label(self.PS, "kernarium_gamepad"), ("nobody", "u:r:kernarium_gamepad:s0"))
        self.assertIsNone(runner.parse_ps_label(self.PS, "absent"))
        self.assertIsNone(runner.parse_ps_label("", "kernarium_gamepad"))
        # a name that only ends with the wanted one is another process
        self.assertIsNone(runner.parse_ps_label(self.PS, "gamepad"))

    def test_the_total_pss(self) -> None:
        text = "Applications Memory Usage (in Kilobytes):\n\n           TOTAL PSS:     1234            TOTAL RSS:     4321       TOTAL SWAP PSS:        0\n"
        self.assertEqual(runner.parse_meminfo_pss(text), 1234)
        self.assertIsNone(runner.parse_meminfo_pss("No process found for: 502\n"))

    def test_the_focus(self) -> None:
        text = "  mCurrentFocus=Window{1a2b3c u0 org.example.andarium.gamepadprobe/org.example.andarium.gamepadprobe.GamepadActivity}\n"
        self.assertEqual(runner.parse_focus(text), runner.PROBE_FOCUS)
        self.assertIsNone(runner.parse_focus("mCurrentFocus=null\n"))

    def test_pid(self) -> None:
        self.assertEqual(runner.parse_pid("1234\n"), 1234)
        self.assertEqual(runner.parse_pid("1234 5678\n"), 1234)
        self.assertIsNone(runner.parse_pid(""))
        self.assertIsNone(runner.parse_pid("pidof: not found\n"))

    def test_smaps_rollup(self) -> None:
        text = "00400000-7fffffff ---p 00000000 00:00 0   [rollup]\nRss:                1900 kB\nPss:                1100 kB\nPss_Anon:            600 kB\n"
        self.assertEqual(runner.parse_smaps_rollup(text), {"Rss": 1900, "Pss": 1100, "Pss_Anon": 600})

    CPU = ("cpu  100 5 50 800 10 2 3 0 0 0\n"
           "1234 (kernarium_gamepad) S 1 1234 1234 0 -1 4194560 100 0 0 0 7 3 0 0 20 0 1 0 5000 1000 200 "
           "18446744073709551615\n")

    def test_the_cpu_sample(self) -> None:
        sample = runner.parse_cpu_sample(self.CPU)
        self.assertEqual(sample.total_ticks, 100 + 5 + 50 + 800 + 10 + 2 + 3)
        self.assertEqual(sample.busy_ticks, 100 + 5 + 50 + 2 + 3)
        self.assertEqual(sample.relay_ticks, 10)

    def test_a_command_name_with_spaces_and_parentheses_is_read(self) -> None:
        sample = runner.parse_cpu_sample(self.CPU.replace("(kernarium_gamepad)", "(a b) c)"))
        self.assertEqual(sample.relay_ticks, 10)

    def test_no_cpu_sample(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.parse_cpu_sample("garbage")

    def test_the_cost_per_report(self) -> None:
        def sample(busy: int, relay: int) -> "runner.CpuSample":
            return runner.CpuSample(busy, busy + 1000, relay)

        # Idle: 10 ticks in 10 s. Loaded: 70 ticks in 10 s, of which the relay 12; 3000 reports.
        guest, relay = runner.per_report_microseconds(
            (sample(0, 0), sample(10, 0)), (sample(100, 5), sample(170, 17)), 10.0, 10.0, 3000)
        self.assertAlmostEqual(guest, (70 - 10) * 0.01 * 1e6 / 3000)
        self.assertAlmostEqual(relay, 12 * 0.01 * 1e6 / 3000)

    def test_the_relays_disconnect_line(self) -> None:
        text = ("11-11 09:00:00.000   120   120 I kernarium.gamepad: host disconnected: 3012 frames, 1 devices created "
                "(0 refused), 3010 input reports written (0 dropped, 0 failed), 4 output reports, 3 requests, "
                "3 replies\n"
                "11-11 09:00:05.000   120   120 I kernarium.gamepad: host disconnected: 5 frames, 2 devices created "
                "(1 refused), 3 input reports written (1 dropped, 2 failed), 0 output reports, 0 requests, "
                "0 replies\n")
        counters = runner.parse_relay_disconnects(text)
        self.assertEqual(len(counters), 2)
        self.assertEqual(counters[0], runner.RelayCounters(3012, 1, 0, 3010, 0, 0, 4, 3, 3))
        self.assertEqual((counters[1].refused, counters[1].input_dropped, counters[1].input_failed), (1, 1, 2))
        self.assertEqual(runner.parse_relay_disconnects("nothing"), [])

    def test_the_relays_log_line_is_the_one_the_source_prints(self) -> None:
        source = (REPOSITORY / "android/lineage-24/overlay/gamepad/Relay.cpp").read_text()
        for phrase in ("host disconnected: %u frames, %u devices created (%u refused), %u input reports written (%u dropped, ",
                       "%u failed), %u output reports, %u requests, %u replies"):
            self.assertIn(phrase, source.replace('"\n              "', ""))
        self.assertIn(f'#define LOG_TAG "{runner.RELAY_LOG_TAG}"', source)

    def test_denials_of_the_relays_domain(self) -> None:
        mine = ("11-11 09:00:00.000 I auditd: avc:  denied  { write } for comm=\"kernarium_gamepad\" name=\"uhid\" "
                "scontext=u:r:kernarium_gamepad:s0 tcontext=u:object_r:uhid_device:s0 tclass=chr_file permissive=0")
        onto = mine.replace("scontext=u:r:kernarium_gamepad:s0 tcontext=u:object_r:uhid_device:s0",
                            "scontext=u:r:shell:s0 tcontext=u:r:kernarium_gamepad:s0")
        other = ("avc:  denied  { read } for comm=\"x\" scontext=u:r:shell:s0 tcontext=u:object_r:proc:s0 "
                 "tclass=file permissive=0")
        self.assertEqual(runner.selinux_denials("\n".join([other, mine, "allowed", onto])), [mine, onto])
        self.assertEqual(runner.selinux_denials(other), [])

    def test_percentiles_and_latency(self) -> None:
        values = [float(number) for number in range(1, 101)]
        self.assertEqual(runner.percentile(values, 0.5), 51.0)
        self.assertEqual(runner.percentile(values, 0.95), 96.0)
        self.assertEqual(runner.percentile([4.0], 0.99), 4.0)
        events = [runner.ProbeEvent("motion", 1, kernel_ms=1005, received_ms=1009),
                  runner.ProbeEvent("motion", 1, kernel_ms=1030, received_ms=1031)]
        samples = runner.latency_samples([1000.0, 1020.0], events)
        self.assertEqual([(s.host_to_kernel_ms, s.kernel_to_app_ms, s.total_ms) for s in samples],
                         [(5.0, 4, 9.0), (10.0, 1, 11.0)])

    def test_a_guest_clock_ahead_of_the_host_is_removed(self) -> None:
        # The guest's clock runs 322 ms ahead: its events are stamped that much later than the host's sends.
        events = [runner.ProbeEvent("motion", 1, kernel_ms=1005 + 322, received_ms=1009 + 322),
                  runner.ProbeEvent("motion", 1, kernel_ms=1030 + 322, received_ms=1031 + 322)]
        samples = runner.latency_samples([1000.0, 1020.0], events, 322.0)
        self.assertEqual([(s.host_to_kernel_ms, s.kernel_to_app_ms) for s in samples], [(5.0, 4), (10.0, 1)])
        behind = runner.latency_samples([1000.0], [runner.ProbeEvent("motion", 1, kernel_ms=1005 - 300, received_ms=1009 - 300)], -300.0)
        self.assertEqual(behind[0].host_to_kernel_ms, 5.0)

    def test_the_clock_offset_comes_from_the_shortest_round_trip(self) -> None:
        # (host before, guest reading, host after): the second pair has the 10 ms trip, so its midpoint 5005 decides.
        readings = [(1000.0, 1522.0, 1400.0), (5000.0, 5327.0, 5010.0), (9000.0, 9600.0, 9100.0)]
        self.assertEqual(runner.clock_offset_ms(readings), 5327.0 - 5005.0)
        self.assertEqual(runner.clock_offset_ms([(10.0, 10.0, 10.0)]), 0.0)

    def test_the_guest_reads_its_clock_in_nanoseconds(self) -> None:
        class Shell:
            def __init__(self, answers: list[str]) -> None:
                self.answers, self.commands = answers, []

            def shell(self, command: str, timeout: float = 0) -> str:
                self.commands.append(command)
                return self.answers.pop(0)

        guest = Shell(["1700000000123456789\n"] * runner.CLOCK_READINGS)
        with mock.patch.object(runner.time, "time", side_effect=[1700000000.100 + 0.001 * i for i in range(2 * runner.CLOCK_READINGS)]):
            offset = runner.Guest.clock_offset_ms(guest)  # type: ignore[arg-type]
        self.assertEqual(set(guest.commands), {"date +%s%N"})
        self.assertAlmostEqual(offset, 1700000000123.456789 - (1700000000100.0 + 1700000000101.0) / 2, delta=1.0)
        with self.assertRaisesRegex(runner.Failure, "printed"):
            runner.Guest.clock_offset_ms(Shell(["date: bad"]))  # type: ignore[arg-type]


class CostConnectionTests(unittest.TestCase):
    def test_the_cost_step_measures_on_a_connection_of_its_own(self) -> None:
        """The protocol check leaves a connection that has counted one deliberate drop; the cost step's counters
        must not include it."""
        calls: list[str] = []

        class Stop(Exception):
            pass

        def create_all(plan) -> None:
            calls.append("create_all")
            raise Stop

        session = types.SimpleNamespace(
            guest=object(), families={"dualsense": families.DualSenseFamily()},
            connect=lambda: calls.append("connect"), create_all=create_all)
        with self.assertRaises(Stop):
            runner.measure_cost(session, types.SimpleNamespace(report_interval_ms=4.0, reports=10))
        self.assertEqual(calls, ["connect", "create_all"])


class LateArrivalTests(unittest.TestCase):
    """The identity and the events of a controller are read again until they are right; a controller that never is
    still fails, with the last problem."""

    class Link:
        def __init__(self) -> None:
            self.driven = []

        def drive(self, slot, family, steps) -> None:
            self.driven.append(steps)

    class Guest:
        def __init__(self, states: list) -> None:
            self.states, self.reads, self.broadcasts = states, 0, []

        def alive(self) -> None:
            pass

        def probe_state(self):
            self.reads += 1
            return self.states[min(self.reads, len(self.states)) - 1]

        def broadcast(self, **fields) -> None:
            self.broadcasts.append(fields)

    def session(self, states: list):
        family = families.DualSenseFamily()
        return types.SimpleNamespace(families={"dualsense": family}, guest=self.Guest(states), link=self.Link()), family

    def key_state(self, family, names, device_id=7):
        events = [runner.ProbeEvent("key", device_id, code=families.KEYCODES[name], action=action, repeat=0)
                  for name in names for action in ("down", "up")]
        base = probe_state()
        return dataclasses.replace(base, events=events, devices=[dataclasses.replace(base.devices[0], id=device_id)])

    def test_the_identity_is_read_again_until_it_is_right(self) -> None:
        session, family = self.session([probe_state(), probe_state()])
        wrong = device(battery_capacity=1.0)
        stale = dataclasses.replace(probe_state(), devices=[wrong])
        with mock.patch.object(runner.time, "sleep", lambda seconds: None):
            runner.check_identities(session, stale)
        self.assertEqual(session.guest.reads, 1)

    def test_an_identity_that_stays_wrong_fails_with_the_last_problem(self) -> None:
        session, family = self.session([dataclasses.replace(probe_state(), devices=[device(battery_capacity=1.0)])])
        stale = dataclasses.replace(probe_state(), devices=[device(battery_capacity=1.0)])
        clock = iter(range(0, 1000))
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner.time, "monotonic", lambda: float(next(clock))), \
                self.assertRaisesRegex(runner.Failure, r"the controllers' identities: dualsense: battery capacity 1.0, not about 0.85"):
            runner.check_identities(session, stale)

    def test_a_right_identity_is_not_read_again(self) -> None:
        session, family = self.session([probe_state()])
        runner.check_identities(session, probe_state())
        self.assertEqual(session.guest.reads, 0)

    def test_the_buttons_of_a_slow_guest_are_read_again(self) -> None:
        family = families.DualSenseFamily()
        names = list(family.buttons)
        partial, whole = self.key_state(family, names[:3]), self.key_state(family, names)
        session, _ = self.session([])
        # The device lookup reads once, the first read after the drive is partial, the next ones whole.
        reads = [whole, partial, whole]
        session.guest.probe_state = lambda: reads.pop(0) if len(reads) > 1 else reads[0]
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner, "axis_steps", lambda family: []):
            runner.check_events(session, "dualsense", 0)
        self.assertEqual(reads, [whole])

    def test_buttons_that_never_arrive_fail_with_the_sequence_problem(self) -> None:
        family = families.DualSenseFamily()
        partial = self.key_state(family, list(family.buttons)[:2])
        session, _ = self.session([partial])
        clock = iter(range(0, 1000))
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner.time, "monotonic", lambda: float(next(clock))), \
                mock.patch.object(runner, "axis_steps", lambda family: []), \
                self.assertRaisesRegex(runner.Failure, r"buttons: buttons: only \d+ of \d+ key events arrived"):
            runner.check_events(session, "dualsense", 0)


def device(**changes) -> "runner.ProbeDevice":
    return probe_state().devices[0] if not changes else _replaced(probe_state().devices[0], **changes)


def _replaced(item, **changes):
    import dataclasses
    return dataclasses.replace(item, **changes)


class CheckTests(unittest.TestCase):
    def test_a_matching_device_has_no_problems(self) -> None:
        self.assertEqual(runner.identity_problems(device(), families.DualSenseFamily()), [])

    def test_each_difference_is_a_problem(self) -> None:
        family = families.DualSenseFamily()
        cases = {
            "product": device(product=0x1234),
            "gamepad": device(sources=runner.SOURCE_JOYSTICK),
            "joystick": device(sources=runner.SOURCE_GAMEPAD),
            "keys": device(keys={96}),
            "range": device(ranges={}),
            "vibrators": device(vibrators=0),
            "battery": device(battery_present=False),
            "capacity": device(battery_capacity=0.3),
            "lights": device(lights=[]),
        }
        for name, changed in cases.items():
            with self.subTest(difference=name):
                self.assertTrue(runner.identity_problems(changed, family), name)

    def test_a_family_without_lights_or_battery_has_none(self) -> None:
        generic = families.GenericFamily()
        base = device(name="Game Controller", vendor=0, product=0, vibrators=0, battery_present=False,
                      battery_capacity=None, lights=[], keys={families.KEYCODES[b] for b in generic.buttons})
        self.assertEqual(runner.identity_problems(base, generic), [])
        self.assertTrue(runner.identity_problems(_replaced(base, vibrators=2), generic))
        self.assertTrue(runner.identity_problems(_replaced(base, battery_present=True, battery_capacity=1.0), generic))
        self.assertTrue(runner.identity_problems(_replaced(base, lights=PROBE_JSON["devices"][0]["lights"]), generic))

    def test_the_xbox_has_no_digital_triggers(self) -> None:
        self.assertNotIn("l2", families.XboxFamily().buttons)
        self.assertIn("l2", families.GenericFamily().buttons)

    @staticmethod
    def keys(*pairs: tuple[str, str], device_id: int = 7, repeat: int = 0) -> list["runner.ProbeEvent"]:
        return [runner.ProbeEvent("key", device_id, code=families.KEYCODES[name], action=action, repeat=repeat)
                for name, action in pairs]

    def test_the_key_sequence(self) -> None:
        events = self.keys(("a", "down"), ("a", "up"), ("b", "down"), ("b", "up"))
        self.assertIsNone(runner.key_sequence_problem(events, 7, ["a", "b"]))
        self.assertIsNotNone(runner.key_sequence_problem(events[:3], 7, ["a", "b"]))
        self.assertIsNotNone(runner.key_sequence_problem(events, 7, ["b", "a"]))
        self.assertIsNotNone(runner.key_sequence_problem(events, 7, ["a"]))
        self.assertIsNotNone(runner.key_sequence_problem(self.keys(("a", "down")), 7, ["a"]))

    def test_other_devices_and_repeats_do_not_count(self) -> None:
        events = (self.keys(("x", "down"), device_id=8) + self.keys(("a", "down"), ("a", "up"))
                  + self.keys(("a", "down"), repeat=2))
        self.assertIsNone(runner.key_sequence_problem(events, 7, ["a"]))

    def test_the_gamepad_home_may_be_consumed(self) -> None:
        events = self.keys(("a", "down"), ("a", "up"), ("b", "down"), ("b", "up"))
        self.assertIsNone(runner.key_sequence_problem(events, 7, ["a", "mode", "b"]))
        with_mode = self.keys(("a", "down"), ("a", "up"), ("mode", "down"), ("mode", "up"), ("b", "down"), ("b", "up"))
        self.assertIsNone(runner.key_sequence_problem(with_mode, 7, ["a", "mode", "b"]))
        # Only the Home itself is optional: any other key in its place is a problem.
        self.assertIsNotNone(runner.key_sequence_problem(
            self.keys(("a", "down"), ("a", "up"), ("x", "down"), ("x", "up"), ("b", "down"), ("b", "up")), 7,
            ["a", "mode", "b"]))

    @staticmethod
    def motion(**axes: float) -> "runner.ProbeEvent":
        return runner.ProbeEvent("motion", 7, axes={name: axes.get(name, 0.0) for name in (
            "x", "y", "z", "rz", "ltrigger", "rtrigger", "hat_x", "hat_y")})

    def test_the_axes_in_order(self) -> None:
        expected = [("left", {"x": -1.0, "y": 0.0}), ("neutral", {"x": 0.0, "y": 0.0}), ("hat", {"hat_x": 1.0})]
        events = [self.motion(x=-1.0), self.motion(x=0.004), self.motion(hat_x=1.0)]
        self.assertIsNone(runner.axes_subsequence_problem(events, 7, expected))
        self.assertIsNone(runner.axes_subsequence_problem([self.motion(y=0.3)] + events, 7, expected))
        self.assertIn("hat", runner.axes_subsequence_problem(events[:2], 7, expected))
        self.assertIn("left", runner.axes_subsequence_problem([self.motion(x=-0.9)], 7, expected))
        self.assertIsNotNone(runner.axes_subsequence_problem([events[2], events[1], events[0]], 7, expected))

    def test_the_expected_vectors_of_every_step_are_in_the_probes_axes(self) -> None:
        names = {"x", "y", "z", "rz", "ltrigger", "rtrigger", "hat_x", "hat_y"}
        for _, state in runner.axis_steps(families.DualSenseFamily()):
            self.assertEqual(set(families.expected_state_axes(state)), names)

    def test_the_probe_sources_name_what_the_runner_reads(self) -> None:
        java = (INTEGRATION / "gamepad/app/src/org/example/andarium/gamepadprobe/GamepadActivity.java").read_text()
        for axis in ("x", "y", "z", "rz", "ltrigger", "rtrigger", "hat_x", "hat_y"):
            self.assertIn(f'"{axis}"', java)
        for key in ("nonce", "failure", "wallMs", "keyCount", "motionSampleCount", "devices", "events", "listener",
                    "controllerNumber", "vibrators", "battery", "capacity", "present", "lights", "rgb", "kernelMs",
                    "receivedMs", "repeat", "ranges", "keys"):
            self.assertIn(f'"{key}"', java)
        self.assertIn(f'"{runner.PROBE_ACTION}"', java.replace(
            'static final String ACTION = "org.example.andarium.gamepadprobe.COMMAND"', f'"{runner.PROBE_ACTION}"'))
        for command in ("reset", "record", "dump", "vibrate", "cancel", "light", "release"):
            self.assertIn(f'case "{command}"', java)
        manifest = (INTEGRATION / "gamepad/app/AndroidManifest.xml").read_text()
        self.assertIn(f'package="{runner.PROBE_PACKAGE}"', manifest)
        self.assertIn('android:debuggable="true"', manifest)  # run-as reads the state file


class FakeRelay:
    """The relay's end of the stream, on loopback: HELLO on accept, CREATED for each CREATE, a GET_REPORT after it
    for a DualSense, and the host's replies kept."""

    def __init__(self) -> None:
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.received: list[tuple[int, bytes]] = []
        self.replied = threading.Event()
        self.connection: socket.socket | None = None
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self) -> None:
        self.connection, _ = self.server.accept()
        self.connection.sendall(wire.frame(wire.HELLO, struct.pack("<HBx", 1, 8)))
        buffer = b""
        while True:
            try:
                data = self.connection.recv(65536)
            except OSError:
                return
            if not data:
                return
            buffer += data
            while len(buffer) >= wire.HEADER_SIZE:
                _, frame_type, length = struct.unpack("<4sHH", buffer[:wire.HEADER_SIZE])
                if len(buffer) < wire.HEADER_SIZE + length:
                    break
                payload, buffer = buffer[wire.HEADER_SIZE:wire.HEADER_SIZE + length], buffer[wire.HEADER_SIZE + length:]
                self.received.append((frame_type, payload))
                if frame_type == wire.CREATE:
                    slot = payload[0]
                    self.connection.sendall(wire.frame(wire.CREATED, struct.pack("<BxxxI", slot, 0)))
                    if struct.unpack("<II", payload[4:12]) == (0x054C, 0x0CE6):
                        self.connection.sendall(wire.frame(
                            wire.GET_REPORT, struct.pack("<BBBxI", slot, wire.FEATURE_REPORT, 0x20, 41)))
                        self.connection.sendall(wire.frame(
                            wire.GET_REPORT, struct.pack("<BBBxI", slot, wire.FEATURE_REPORT, 0x99, 42)))
                if frame_type == wire.GET_REPORT_REPLY:
                    ids = [struct.unpack("<BxHIH", item[:10])[2] for kind, item in self.received
                           if kind == wire.GET_REPORT_REPLY]
                    if len(ids) == 2:
                        self.replied.set()

    def close(self) -> None:
        for sock in (self.connection, self.server):
            if sock is not None:
                sock.close()


class HostLinkTests(unittest.TestCase):
    def test_a_create_is_answered_and_a_request_is_replied_to(self) -> None:
        relay = FakeRelay()
        link = runner.HostLink(relay.port)
        try:
            hello = link.connect()
            self.assertEqual(hello, wire.Hello(1, 8))
            self.assertEqual(link.create(0, families.DualSenseFamily()), 0)
            self.assertTrue(relay.replied.wait(10), "the host never answered the requests")
            replies = {}
            for kind, payload in relay.received:
                if kind == wire.GET_REPORT_REPLY:
                    slot, error, request_id, length = struct.unpack("<BxHIH", payload[:10])
                    replies[request_id] = (error, payload[10:10 + length])
            self.assertEqual(replies[41][0], 0)
            self.assertEqual(replies[41][1], families.DualSenseFamily().feature_reply(0, 0x20))
            self.assertEqual(replies[42], (5, b""))  # a report the device lacks: EIO
            link.drive(0, families.DualSenseFamily(), [("a", families.State(buttons=frozenset({"a"})))], step=0.0)
            deadline = threading.Event()
            for _ in range(200):
                if any(kind == wire.INPUT for kind, _ in relay.received):
                    break
                deadline.wait(0.05)
            inputs = [payload for kind, payload in relay.received if kind == wire.INPUT]
            self.assertEqual(len(inputs), 1)
            self.assertEqual(inputs[0][4:], families.DualSenseFamily().report(families.State(buttons=frozenset({"a"}))))
        finally:
            link.close()
            relay.close()

    def test_a_guest_that_closes_is_seen(self) -> None:
        relay = FakeRelay()
        link = runner.HostLink(relay.port)
        try:
            link.connect()
            relay.connection.shutdown(socket.SHUT_RDWR)
            self.assertTrue(link.closed_by_guest(5))
        finally:
            link.close()
            relay.close()

    def test_a_local_close_is_not_the_guests(self) -> None:
        relay = FakeRelay()
        link = runner.HostLink(relay.port)
        try:
            link.connect()
        finally:
            link.close()
            relay.close()
        self.assertFalse(link.closed_by_guest(0))

    def test_no_hello_is_a_failure(self) -> None:
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        accepted = []
        threading.Thread(target=lambda: accepted.append(server.accept()), daemon=True).start()
        link = runner.HostLink(server.getsockname()[1])
        original = runner.HELLO_SECONDS
        runner.HELLO_SECONDS = 0.3
        try:
            with self.assertRaises(runner.Failure):
                link.connect()
        finally:
            runner.HELLO_SECONDS = original
            link.close()
            for connection, _ in accepted:
                connection.close()
            server.close()


class ReportTests(unittest.TestCase):
    def test_each_check_is_reported_and_a_failure_does_not_stop_the_next(self) -> None:
        report = runner.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            with report.check("first"):
                raise runner.Failure("broken")
            with report.check("second"):
                pass
        self.assertEqual(report.results, [("first", "broken"), ("second", None)])
        self.assertEqual(report.failures(), [("first", "broken")])

    def test_a_check_that_a_user_build_cannot_run_is_said_and_is_not_a_failure(self) -> None:
        report = runner.Report()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            report.skip("killing the relay", "no su")
        self.assertEqual(report.not_run, [("killing the relay", "no su")])
        self.assertEqual(report.failures(), [])
        self.assertIn("not run on a user build", output.getvalue())

    def test_root_is_refused_on_a_user_build_and_the_shell_reads_are_not(self) -> None:
        class Shell(runner.Guest):
            def __init__(self, debuggable: str) -> None:
                super().__init__("adb", "serial", None, Path("log"), 0)  # type: ignore[arg-type]
                self.answer, self.commands = debuggable, []

            def shell(self, command: str, timeout: float = 0) -> str:
                self.commands.append(command)
                return self.answer if command == "getprop ro.debuggable" else "out"

        user = Shell("0\n")
        with self.assertRaises(runner.Failure):
            user.root("kill -9 1")
        self.assertEqual(user.shell("ps -A -Z"), "out")
        self.assertNotIn("su 0 sh -c 'kill -9 1'", user.commands)
        debuggable = Shell("1\n")
        debuggable.root("kill -9 1")
        self.assertIn("su 0 sh -c 'kill -9 1'", debuggable.commands)

    def test_a_programming_error_is_not_a_failed_check(self) -> None:
        with self.assertRaises(KeyError):
            with runner.Report().check("x"):
                raise KeyError("x")

    def test_the_default_port_is_the_next_free_one(self) -> None:
        self.assertEqual(runner.DEFAULT_ADB_PORT, 5579)


if __name__ == "__main__":
    unittest.main()
