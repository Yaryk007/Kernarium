#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The keyboard and mouse runner's reading of the guest, and its VNC client.

run_android_keymouse.py plays the display's user against a booted release: a
VNC client sends keys and pointer events, `dumpsys input` says which devices
Android has, and the probe application (tests/integration/android/keymouse)
logs what it receives. The texts below have the layout those print (EventHub
and InputReader dumps of lineage-24.0-u6, and the probe's events.log), and the
client's handshake runs against a scripted server, so the parsers, the verdicts
and the wire run without a guest. The key table is checked against the
launcher's own.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import re
import socket
import struct
import sys
import threading
import unittest
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
REPOSITORY = TESTS.parent
SCRIPT = TESTS / "integration/android/run_android_keymouse.py"
SPEC = importlib.util.spec_from_file_location("run_android_keymouse", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

PROBE = TESTS / "integration/android/keymouse"
BRIDGE = REPOSITORY / "darwin/emulator/src/hardware_input_bridge.hpp"
VNC_INPUT = REPOSITORY / "darwin/device/input/vnc/vnc_input.cpp"


def hub_device(number: int, name: str, product: int, configuration: str, layout: str = "",
               character_map: str = "") -> str:
    return (
        f"    {number}: {name}\n"
        "      Classes: KEYBOARD | ALPHAKEY\n"
        f"      Path: /dev/input/event{number}\n"
        "      Enabled: true\n"
        "      Descriptor: 0123456789abcdef\n"
        "      Location: \n"
        "      ControllerNumber: 0\n"
        "      UniqueId: \n"
        f"      Identifier: bus=0x0006, vendor=0x1af4, product=0x{product:04x}, version=0x0001, bluetoothAddress=<none>\n"
        f"      KeyLayoutFile: {layout}\n"
        f"      KeyCharacterMapFile: {character_map}\n"
        f"      ConfigurationFile: {configuration}\n"
        "      VideoDevice: <none>\n")


def reader_device(number: int, name: str, external: bool, sources: str, keyboard_type: int,
                  mapper: str = "") -> str:
    return (
        f"  Device {number}: {name}\n"
        f"    EventHub Devices: [ {number} ] \n"
        "    Generation: 4\n"
        f"    IsExternal: {str(external).lower()}\n"
        "    IsVirtualDevice: false\n"
        "    IsWaking: false\n"
        "    AssociatedDisplayPort: <none>\n"
        f"    Sources: {sources}\n"
        f"    KeyboardType: {keyboard_type}\n"
        "    ControllerNum: 0\n"
        f"{mapper}")


CURSOR_MAPPER = (
    "    Cursor Input Mapper:\n"
    "      Parameters:\n"
    "        HasHWheel: true\n"
    "      XScale: 1.000\n"
    "      HaveVWheel: true\n"
    "      ButtonState: 0x00000000\n"
    "      PointerCaptureLed: {led}\n"
    "      Down: false\n")

SIDE_KEYS_HUB = hub_device(2, "Kernarium Keyboard", 0x0002, "<none>",
                           "/vendor/usr/keylayout/Vendor_1af4_Product_0002.kl",
                           "/system/usr/keychars/Generic.kcm")
KEYBOARD_HUB = hub_device(5, "Kernarium Hardware Keyboard", 0x0003,
                          "/vendor/usr/idc/Vendor_1af4_Product_0003.idc",
                          "/system/usr/keylayout/Generic.kl", "/system/usr/keychars/Generic.kcm")
MOUSE_HUB = hub_device(6, "Kernarium Mouse", 0x0004, "/vendor/usr/idc/Vendor_1af4_Product_0004.idc")
POINTER_HUB = hub_device(7, "Kernarium Pointer", 0x0005, "/vendor/usr/idc/Vendor_1af4_Product_0005.idc")
SIDE_KEYS_READER = reader_device(2, "Kernarium Keyboard", False, "KEYBOARD", 1)


def dump(hub: list[str], reader: list[str]) -> str:
    return (
        "INPUT MANAGER (dumpsys input)\n\n"
        "Event Hub State:\n"
        "  BuiltInKeyboardId: -2\n"
        "  Devices:\n"
        + "".join(hub)
        + "  Unattached video devices:\n    <none>\n\n"
        f"Input Reader State (Nums of device: {len(reader)}):\n"
        + "".join(reader)
        + "  NextTimeout: -9223372036854775808\n"
        "  Configuration:\n"
        "    ExcludedDeviceNames: []\n")


def plugged_keyboard(led: str = "true") -> str:
    return dump(
        [SIDE_KEYS_HUB, KEYBOARD_HUB, MOUSE_HUB, POINTER_HUB],
        [SIDE_KEYS_READER,
         reader_device(5, "Kernarium Hardware Keyboard", True, "KEYBOARD", 2),
         reader_device(6, "Kernarium Mouse", True, "MOUSE", 0, CURSOR_MAPPER.format(led=led)),
         reader_device(7, "Kernarium Pointer", False, "MOUSE", 0)])


class InputDumpTests(unittest.TestCase):
    def test_the_devices_read_by_name(self) -> None:
        parsed = runner.parse_input_dump(plugged_keyboard())
        self.assertEqual(list(parsed.hub), [runner.SIDE_KEYS, runner.KEYBOARD, runner.MOUSE, runner.POINTER])
        self.assertEqual(list(parsed.reader), list(parsed.hub))
        keyboard = parsed.hub[runner.KEYBOARD]
        self.assertEqual((keyboard.id, keyboard.vendor, keyboard.product), (5, 0x1AF4, 0x0003))
        self.assertEqual(keyboard.path, "/dev/input/event5")
        self.assertEqual(keyboard.configuration, "/vendor/usr/idc/Vendor_1af4_Product_0003.idc")
        self.assertEqual(keyboard.key_layout, "/system/usr/keylayout/Generic.kl")
        reader = parsed.reader[runner.KEYBOARD]
        self.assertEqual((reader.external, reader.keyboard_type, reader.sources), (True, 2, "KEYBOARD"))
        self.assertIsNone(reader.pointer_capture_led)
        self.assertEqual(parsed.reader[runner.MOUSE].pointer_capture_led, "true")
        self.assertIs(parsed.reader[runner.POINTER].external, False)

    def test_the_side_keys_alone(self) -> None:
        parsed = runner.parse_input_dump(dump([SIDE_KEYS_HUB], [SIDE_KEYS_READER]))
        self.assertEqual(list(parsed.hub), [runner.SIDE_KEYS])
        self.assertIsNone(runner.unplugged_problem(parsed))
        self.assertIsNone(runner.side_keys_problem(parsed))

    def test_nothing_reads_as_nothing(self) -> None:
        parsed = runner.parse_input_dump("")
        self.assertEqual((parsed.hub, parsed.reader), ({}, {}))
        self.assertIn("is not an input device", runner.side_keys_problem(parsed))

    def test_a_built_in_keyboard_header_keeps_its_name(self) -> None:
        text = dump([SIDE_KEYS_HUB.replace("2: Kernarium Keyboard", "2: Kernarium Keyboard (aka device 0 - built-in keyboard)")],
                    [SIDE_KEYS_READER])
        self.assertEqual(list(runner.parse_input_dump(text).hub), [runner.SIDE_KEYS])


class VerdictTests(unittest.TestCase):
    def parse(self, **replacements: str) -> runner.InputDump:
        text = plugged_keyboard()
        for old, new in replacements.items():
            text = text.replace(old.replace("__", " "), new)
        return runner.parse_input_dump(text)

    def test_a_plugged_keyboard_is_the_product_s_keyboard(self) -> None:
        parsed = runner.parse_input_dump(plugged_keyboard())
        self.assertIsNone(runner.keyboard_problem(parsed))
        self.assertIsNone(runner.mouse_problem(parsed))
        self.assertEqual(runner.unplugged_problem(parsed),
                         "still input devices: ['Kernarium Hardware Keyboard', 'Kernarium Mouse', 'Kernarium Pointer']")

    def test_an_internal_or_non_alphabetic_keyboard_is_refused(self) -> None:
        text = plugged_keyboard()
        internal = text.replace(
            "IsExternal: true\n    IsVirtualDevice: false\n    IsWaking: false\n    AssociatedDisplayPort: <none>\n"
            "    Sources: KEYBOARD\n    KeyboardType: 2", "IsExternal: false\n    IsVirtualDevice: false\n"
            "    IsWaking: false\n    AssociatedDisplayPort: <none>\n    Sources: KEYBOARD\n    KeyboardType: 2")
        self.assertIn("IsExternal False", runner.keyboard_problem(runner.parse_input_dump(internal)))
        plain = text.replace("Sources: KEYBOARD\n    KeyboardType: 2", "Sources: KEYBOARD\n    KeyboardType: 1")
        self.assertIn("keyboard type 1, not 2", runner.keyboard_problem(runner.parse_input_dump(plain)))

    def test_a_keyboard_without_the_product_configuration_is_refused(self) -> None:
        text = plugged_keyboard().replace("/vendor/usr/idc/Vendor_1af4_Product_0003.idc", "<none>")
        self.assertIn("device configuration '<none>'", runner.keyboard_problem(runner.parse_input_dump(text)))

    def test_a_keyboard_on_a_vendor_layout_is_refused(self) -> None:
        text = plugged_keyboard().replace("/system/usr/keylayout/Generic.kl",
                                          "/vendor/usr/keylayout/Vendor_1af4_Product_0003.kl")
        self.assertIn("key layout", runner.keyboard_problem(runner.parse_input_dump(text)))

    def test_a_side_keys_device_on_the_generic_layout_is_refused(self) -> None:
        text = plugged_keyboard().replace("/vendor/usr/keylayout/Vendor_1af4_Product_0002.kl",
                                          "/system/usr/keylayout/Generic.kl")
        self.assertIn("key layout", runner.keyboard_problem(runner.parse_input_dump(text)))

    def test_a_pointer_that_is_external_is_refused(self) -> None:
        parsed = runner.parse_input_dump(plugged_keyboard())
        parsed.reader[runner.POINTER].external = True
        self.assertIn("IsExternal True", runner.mouse_problem(parsed))

    def test_a_mouse_without_mouse_sources_is_refused(self) -> None:
        parsed = runner.parse_input_dump(plugged_keyboard())
        parsed.reader[runner.MOUSE].sources = "KEYBOARD"
        self.assertIn("sources", runner.mouse_problem(parsed))

    def test_the_capture_led(self) -> None:
        self.assertIsNone(runner.capture_led_problem(runner.parse_input_dump(plugged_keyboard("true")), "true"))
        self.assertIsNone(runner.capture_led_problem(runner.parse_input_dump(plugged_keyboard("false")), "false"))
        self.assertIn("not 'false'", runner.capture_led_problem(runner.parse_input_dump(plugged_keyboard("true")), "false"))
        without = runner.parse_input_dump(plugged_keyboard().replace("      PointerCaptureLed: true\n", ""))
        self.assertIn("no PointerCaptureLed line", runner.capture_led_problem(without, "true"))
        none = runner.parse_input_dump(plugged_keyboard("<none>"))
        self.assertIn("not 'true'", runner.capture_led_problem(none, "true"))

    def test_relative_motion_needs_a_writable_event_node(self) -> None:
        class Shell:
            def __init__(self, answer: str) -> None:
                self.answer, self.commands = answer, []

            def shell(self, command: str) -> str:
                self.commands.append(command)
                return self.answer + "\n"

        writable, denied = Shell("writable"), Shell("denied")
        self.assertTrue(runner.can_inject_relative(writable, "/dev/input/event3"))
        self.assertFalse(runner.can_inject_relative(denied, "/dev/input/event3"))
        self.assertIn("test -w /dev/input/event3", writable.commands[0])


class ChoiceSurvivesABootTests(unittest.TestCase):
    """A person's later choice of the on-screen keyboard survives a reboot; a failure says what the script logged."""

    class Guest:
        def __init__(self, after_reboot: str, log: str) -> None:
            self.after_reboot, self.log, self.commands = after_reboot, log, []

        def shell(self, command: str) -> str:
            self.commands.append(command)
            if command.startswith("cmd settings get"):
                return self.after_reboot + "\n"
            if command.startswith("logcat"):
                return self.log
            return ""

    def test_a_kept_choice_passes(self) -> None:
        guest, rebooted, slept = self.Guest("1", ""), [], []
        with mock.patch.object(runner.time, "sleep", slept.append):
            runner.check_choice_survives_a_boot(guest, lambda: rebooted.append(True))
        self.assertEqual(rebooted, [True])
        self.assertEqual(slept, [runner.SETTINGS_WRITE_SECONDS])  # the write is given its time before the reboot
        self.assertIn(f"cmd settings put secure {runner.KEYBOARD_SETTING} 1", guest.commands)
        self.assertEqual(guest.commands[-1], f"cmd settings put secure {runner.KEYBOARD_SETTING} 0")

    def test_a_lost_choice_fails_with_the_script_log(self) -> None:
        log = "I kernarium_keyboard_default: the on-screen keyboard now hides while a hardware keyboard is attached (was 1)"
        with self.assertRaises(runner.Failure) as raised:
            runner.check_choice_survives_a_boot(self.Guest("0", log), lambda: None)
        message = str(raised.exception)
        self.assertIn("changed a person's choice of the on-screen keyboard to '0'", message)
        self.assertIn("(was 1)", message)

    def test_a_script_that_ran_again_fails_even_with_the_choice_kept(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "ran again after a reboot"):
            runner.check_choice_survives_a_boot(
                self.Guest("1", "I kernarium_keyboard_default: the on-screen keyboard setting is already the product's default (1)"),
                lambda: None)


PROBE_LOG = """\
1 begin run_id="keymouse"
2 device id=1 name="Kernarium Keyboard" vendor=0x1af4 product=0x0002 sources=0x00000101 keyboard_type=1 virtual=false
3 device-added id=9 name="Kernarium Hardware Keyboard" vendor=0x1af4 product=0x0003 sources=0x00000101 keyboard_type=2 virtual=false
4 key action=down code=KEYCODE_K scan=37 device=9 repeat=0 meta=0x0
5 key action=up code=KEYCODE_K scan=37 device=9 repeat=0 meta=0x0
6 key action=down code=KEYCODE_A scan=30 device=9 repeat=0 meta=0x41
7 hover action=move x=432.0 y=1200.0 rel_x=0.0 rel_y=0.0 vscroll=0.00 hscroll=0.00 buttons=0x0 action_button=0x0 tool=3 source=0x00002002 device=11
8 touch action=down x=432.0 y=1200.0 rel_x=0.0 rel_y=0.0 vscroll=0.00 hscroll=0.00 buttons=0x2 action_button=0x0 tool=3 source=0x00002002 device=11
9 scroll action=scroll x=432.0 y=1200.0 rel_x=0.0 rel_y=0.0 vscroll=-1.00 hscroll=0.00 buttons=0x0 action_button=0x0 tool=3 source=0x00002002 device=10
10 capture-request enable=true focus=true
11 capture state=on
12 captured action=move x=5.0 y=-3.0 rel_x=5.0 rel_y=-3.0 vscroll=0.00 hscroll=0.00 buttons=0x0 action_button=0x0 tool=1 source=0x00020004 device=10
13 device-removed id=9
not a record
"""


class ProbeLogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.events = runner.parse_probe_log(PROBE_LOG)

    def test_records_and_quoted_values(self) -> None:
        self.assertEqual([event.sequence for event in self.events], list(range(1, 14)))
        self.assertEqual(self.events[2].fields["name"], "Kernarium Hardware Keyboard")
        self.assertEqual(self.events[0].fields, {"run_id": "keymouse"})
        self.assertEqual(self.events[3].integer("scan"), 37)
        self.assertEqual(self.events[5].integer("meta"), 0x41)

    def test_devices(self) -> None:
        self.assertEqual(runner.devices_added(self.events),
                         {runner.SIDE_KEYS: [1], runner.KEYBOARD: [9]})
        self.assertEqual(runner.devices_removed(self.events), {9})

    def test_keys(self) -> None:
        self.assertEqual(len(runner.key_events(self.events, "KEYCODE_K", "down", 37)), 1)
        self.assertEqual(runner.key_events(self.events, "KEYCODE_K", "down", 38), [])
        self.assertEqual(len(runner.key_events(self.events, "KEYCODE_A", "down")), 1)

    def test_pointer_events(self) -> None:
        self.assertEqual(len(runner.pointer_events(self.events, "touch", action="down")), 1)
        self.assertEqual(runner.pointer_events(self.events, "touch", action="up"), [])
        hover = runner.pointer_events(self.events, "hover")[0]
        self.assertTrue(runner.near(hover, 430, 1203))
        self.assertFalse(runner.near(hover, 430, 1210))
        self.assertEqual(runner.pointer_events(self.events, "scroll")[0].number("vscroll"), -1.0)
        self.assertEqual(runner.pointer_events(self.events, "touch", action="down")[0].integer("buttons"),
                         runner.BUTTON_SECONDARY)

    def test_relative_motion_reads_either_pair(self) -> None:
        captured = runner.pointer_events(self.events, "captured", action="move")[0]
        self.assertTrue(runner.relative_motion(captured, 5, -3))
        self.assertFalse(runner.relative_motion(captured, 5, 3))
        only_position = runner.Event(1, "captured", {"x": "5.0", "y": "-3.0"})
        self.assertTrue(runner.relative_motion(only_position, 5, -3))

    def test_the_launchers_lines(self) -> None:
        log = ("noise\nHardware keyboard: plugged\nHardware mouse: pointer capture on\n  Hardware indented\n"
               "Hardware keyboard: registered by the guest\n")
        self.assertEqual(runner.host_lines(log), [
            "Hardware keyboard: plugged", "Hardware mouse: pointer capture on",
            "Hardware keyboard: registered by the guest"])

    def test_the_launchers_start_line_is_not_a_plug(self) -> None:
        # The line the launcher prints at start says the devices are offered "unplugged".
        launcher = (REPOSITORY / "darwin/app/macos/main.cpp").read_text()
        # The launcher's literal, which its source splits over two lines.
        printed = [re.findall(r'"((?:[^"\\]|\\.)*)"', call) for call in
                   re.findall(r'std::fputs\(((?:\s*"(?:[^"\\]|\\.)*")+)', launcher)]
        starts = ["".join(parts).removesuffix("\\n") for parts in printed
                  if "".join(parts).startswith("Hardware input:")]
        self.assertEqual(len(starts), 1)
        start = starts[0]
        self.assertEqual(start, "Hardware input: keyboard, mouse and pointer offered, unplugged")
        log = f"VNC display: 127.0.0.1:5992\n{start}\n"
        self.assertEqual(runner.host_lines(log), [start])
        # A test for the word in the line would take it for a plug.
        self.assertTrue(any("plugged" in line for line in runner.host_lines(log)))
        self.assertEqual(runner.plug_lines(runner.host_lines(log)), [])
        # What it prints for a plug is one, and only for the line as the runner waits for it.
        plugged = f"{log}Hardware keyboard: plugged\nHardware mouse: plugged\n"
        self.assertEqual(runner.plug_lines(runner.host_lines(plugged)),
                         [runner.HOST_KEYBOARD_PLUGGED, runner.HOST_MOUSE_PLUGGED])
        bridge = BRIDGE.read_text()
        for line in (runner.HOST_KEYBOARD_PLUGGED, runner.HOST_MOUSE_PLUGGED):
            self.assertIn(f'"{line}\\n"', bridge)


class WireTests(unittest.TestCase):
    def test_the_client_messages(self) -> None:
        self.assertEqual(runner.key_event(True, 0xFF0D), b"\x04\x01\x00\x00\x00\x00\xff\x0d")
        self.assertEqual(runner.key_event(False, ord("k")), b"\x04\x00\x00\x00\x00\x00\x00\x6b")
        self.assertEqual(runner.pointer_event(runner.MASK_RIGHT | runner.MASK_WHEEL_UP, 0x0102, 0x0304),
                         b"\x05\x0c\x01\x02\x03\x04")
        self.assertEqual(runner.update_request(True, 1, 2, 3, 4), b"\x03\x01\x00\x01\x00\x02\x00\x03\x00\x04")

    def serve(self, banner: bytes, script: list[tuple[str, bytes]]) -> tuple[tuple[int, int, str] | None, bytes, Exception | None]:
        """Runs the handshake against a scripted server; `script` lists what it expects and says next."""
        client, server = socket.socketpair()
        received = bytearray()
        problem: list[Exception] = []

        def serving() -> None:
            try:
                server.sendall(banner)
                for expected, answer in script:
                    received.extend(runner.read_exactly(server, len(expected.encode("latin1"))))
                    server.sendall(answer)
            except Exception as error:  # noqa: BLE001 - reported to the test below
                problem.append(error)

        thread = threading.Thread(target=serving)
        thread.start()
        result = None
        error = None
        try:
            result = runner.handshake(lambda count: runner.read_exactly(client, count), client.sendall)
        except runner.RfbError as failure:
            error = failure
        client.close()
        thread.join(5)
        server.close()
        self.assertEqual(problem, [])
        return result, bytes(received), error

    INIT = struct.pack(">HH", 1080, 2400) + b"\0" * 16 + struct.pack(">I", 5) + b"Guest"

    def test_version_3_8(self) -> None:
        result, received, error = self.serve(
            b"RFB 003.008\n",
            [("RFB 003.008\n", b"\x01\x01"), ("\x01", struct.pack(">I", 0)), ("\x01", self.INIT)])
        self.assertIsNone(error)
        self.assertEqual(result, (1080, 2400, "Guest"))
        self.assertEqual(received, b"RFB 003.008\n\x01\x01")

    def test_version_3_7_has_no_security_result(self) -> None:
        result, received, error = self.serve(
            b"RFB 003.007\n", [("RFB 003.007\n", b"\x01\x01"), ("\x01", b""), ("\x01", self.INIT)])
        self.assertIsNone(error)
        self.assertEqual(result, (1080, 2400, "Guest"))
        self.assertEqual(received, b"RFB 003.007\n\x01\x01")

    def test_version_3_3_names_one_security_type(self) -> None:
        result, received, error = self.serve(
            b"RFB 003.003\n", [("RFB 003.003\n", struct.pack(">I", 1)), ("\x01", self.INIT)])
        self.assertIsNone(error)
        self.assertEqual(result, (1080, 2400, "Guest"))
        self.assertEqual(received, b"RFB 003.003\n\x01")

    def test_a_newer_server_is_answered_with_3_8(self) -> None:
        result, received, error = self.serve(
            b"RFB 004.001\n",
            [("RFB 003.008\n", b"\x01\x01"), ("\x01", struct.pack(">I", 0)), ("\x01", self.INIT)])
        self.assertIsNone(error)
        self.assertIsNotNone(result)

    def test_a_server_that_wants_a_password_is_refused(self) -> None:
        _, _, error = self.serve(b"RFB 003.008\n", [("RFB 003.008\n", b"\x01\x02")])
        self.assertIn("not None", str(error))

    def test_a_failed_security_result_carries_the_reason(self) -> None:
        _, _, error = self.serve(
            b"RFB 003.008\n",
            [("RFB 003.008\n", b"\x01\x01"), ("\x01", struct.pack(">I", 1) + struct.pack(">I", 4) + b"nope")])
        self.assertIn("nope", str(error))

    def test_something_that_is_not_a_server(self) -> None:
        _, _, error = self.serve(b"HTTP/1.1 400\n", [])
        self.assertIn("not an RFB server", str(error))


class KeyTableTests(unittest.TestCase):
    """The keysyms the runner sends are the launcher's, with the codes Android must report."""

    @staticmethod
    def launcher_codes() -> dict[int, int]:
        codes = {}
        for symbol, code in re.findall(r"Key \{(0x[0-9a-fA-F]+|'(?:\\.|.)'), (\d+)\}", VNC_INPUT.read_text()):
            value = ord(symbol[1:-1].encode().decode("unicode_escape")) if symbol.startswith("'") else int(symbol, 16)
            codes.setdefault(value, int(code))
        return codes

    def test_every_key_the_runner_sends_is_in_the_launchers_table(self) -> None:
        codes = self.launcher_codes()
        for keysym, scan, name in runner.KEYS:
            with self.subTest(key=name):
                self.assertEqual(codes.get(keysym), scan)
        self.assertEqual(codes[runner.KEYSYM_SHIFT_L], 42)
        self.assertEqual(codes[runner.KEYSYM_CAPS_LOCK], 58)

    def test_the_names_are_android_s_for_those_scan_codes(self) -> None:
        # linux/input-event-codes.h and Generic.kl: KEY_K 37, KEY_A 30, KEY_ENTER 28, KEY_ESC 1, KEY_END 107,
        # KEY_F5 63, KEY_TAB 15.
        expected = {37: "KEYCODE_K", 30: "KEYCODE_A", 28: "KEYCODE_ENTER", 1: "KEYCODE_ESCAPE",
                    107: "KEYCODE_MOVE_END", 63: "KEYCODE_F5", 15: "KEYCODE_TAB"}
        self.assertEqual({scan: name for _, scan, name in runner.KEYS}, expected)

    def test_the_wheel_and_button_bits_are_rfc_6143_s(self) -> None:
        self.assertEqual(
            (runner.MASK_LEFT, runner.MASK_MIDDLE, runner.MASK_RIGHT, runner.MASK_WHEEL_UP, runner.MASK_WHEEL_DOWN),
            (1, 2, 4, 8, 16))


class ProbeSourceTests(unittest.TestCase):
    """The probe writes what the runner reads."""

    def test_the_event_kinds_and_fields_the_runner_reads_are_the_probes(self) -> None:
        source = (PROBE / "app/src/org/example/andarium/keymouseprobe/KeyMouseActivity.java").read_text()
        for kind in ("begin", "device", "device-added", "device-removed", "device-changed", "key", "hover", "touch",
                     "button", "scroll", "captured", "capture", "capture-request"):
            with self.subTest(kind=kind):
                self.assertIn(f'"{kind}"', source)
        for field in ("action=", "code=", "scan=", "meta=", "buttons=", "tool=", "rel_x=", "vscroll=", "hscroll=",
                      "name=", "keyboard_type=", "state=", "enable=", "id="):
            with self.subTest(field=field):
                self.assertIn(field, source)
        self.assertIn(runner.PROBE_PACKAGE, (PROBE / "app/AndroidManifest.xml").read_text())
        self.assertIn(runner.PROBE_ACTIVITY.split("/")[1][1:], (PROBE / "app/AndroidManifest.xml").read_text())
        self.assertIn("AndariumKeyMouseProbe", (PROBE / "Android.bp").read_text())


if __name__ == "__main__":
    unittest.main()
