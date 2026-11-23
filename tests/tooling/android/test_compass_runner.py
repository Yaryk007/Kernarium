#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The compass runner's wire, its reading of the guest, its oracle and its host, without a guest.

run_android_compass.py plays the host application against a booted release: it reads the guest's control frames
("ANC1", "ANC2" and the 20-byte "ANC3"), sends the records of the accelerometer, the gyroscope and the magnetic
field, and reads Android's sensor service, package manager and the probe's report. The frames are the golden
frames of tests/unit/andarium/fixtures; the texts below have the layout the guest's commands print.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import json
import math
import re
import socket
import struct
import subprocess
import sys
import threading
import unittest
from pathlib import Path
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_compass.py"
SPEC = importlib.util.spec_from_file_location("run_android_compass", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
FIXTURE = TESTS / "unit/andarium/fixtures/motion-orientation-golden-frames.txt"


def golden() -> dict[str, bytes]:
    frames = {}
    for line in FIXTURE.read_text().splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        name, _, hex_bytes = line.partition(":")
        frames[name.strip()] = bytes.fromhex(hex_bytes.strip())
    return frames


SENSOR_DUMP = """\
Sensor List:
0x00000001) Host Accelerometer        | Andarium        | ver: 1 | type: android.sensor.accelerometer(1) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000002) Host Gyroscope            | Andarium        | ver: 1 | type: android.sensor.gyroscope(4) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000004) Host Magnetometer         | Andarium        | ver: 1 | type: android.sensor.magnetic_field(2) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=50.00Hz | no batching | non-wakeUp |
0x5f676172) Game Rotation Vector Sensor | AOSP            | ver: 3 | type: android.sensor.game_rotation_vector(15) | perm: n/a | flags: 0x00000000
\tcontinuous | maxDelay=0us | maxRate=100.00Hz | no batching | non-wakeUp |
0x5f726f74) Rotation Vector Sensor    | AOSP            | ver: 3 | type: android.sensor.rotation_vector(11) | perm: n/a | flags: 0x00000000
\tcontinuous | maxDelay=0us | maxRate=100.00Hz | no batching | non-wakeUp |
0x5f676d72) GeoMag Rotation Vector Sensor | AOSP            | ver: 3 | type: android.sensor.geomagnetic_rotation_vector(20) | perm: n/a | flags: 0x00000000
\tcontinuous | maxDelay=0us | maxRate=100.00Hz | no batching | non-wakeUp |
Active sensors:
Socket Buffer size = 39 events
"""


def without(dump: str, *needles: str) -> str:
    """The dump without the sensors (a line of the list and the line after it) whose first line has a needle."""
    lines, kept, skip = dump.splitlines(), [], 0
    for line in lines:
        if skip:
            skip -= 1
        elif line.startswith("0x") and any(needle in line for needle in needles):
            skip = 1
        else:
            kept.append(line)
    return "\n".join(kept) + "\n"


ORIENTATION_DUMP = SENSOR_DUMP.replace(
    "Active sensors:\n",
    "0x00000003) Host Device Orientation   | Andarium        | ver: 1 | type: android.sensor.device_orientation(27) "
    "| perm: n/a | flags: 0x00000000\n\ton-change | maxDelay=0us | no batching | non-wakeUp |\nActive sensors:\n")
# An older image, or this one without the argument: no magnetometer, and so no sensor made from it.
OLDER_DUMP = without(SENSOR_DUMP, "Host Magnetometer", "(11)", "(20)")
HELD_DUMP = SENSOR_DUMP.replace(
    "Active sensors:\n", "Active sensors:\nHost Magnetometer (handle=0x00000004, connections=1)\n")
FEATURES = "feature:android.hardware.touchscreen\nfeature:android.hardware.sensor.compass\nfeature:reqGlEsVersion=0x30002\n"
LOG_LINE = ("11-13 09:00:01.000   439   439 I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=%d, "
            "%d sensors, ro.boot.kernarium.compass=%d")
PROBE_REPORT = ('{"mode":"compass","run_id":"1","valid":true,"accelerometer":{"count":100},'
                '"gyroscope":{"count":100},"magnetometer":{"count":50,"latest":[0.0,25.0,-40.0],"accuracy":3,'
                '"resolution":0.125,"max_range":4912.0,"accuracy_calls":[3],'
                '"azimuth_deg":0.4},"rotation_vector":{"count":40,"latest":[0,0,0,1],"azimuth_deg":null}}')


class WireTests(unittest.TestCase):
    def test_the_records_are_the_golden_frames(self) -> None:
        frames = golden()
        for status in range(4):
            self.assertEqual(runner.magnetic_record(0.0, 25.0, -40.0, status), frames[f"magnetic-status-{status}"])
        self.assertEqual(runner.sensor_record(1, 0.0, 0.0, runner.GRAVITY), frames["accelerometer-gravity"])
        self.assertEqual(runner.sensor_record(4, 1.0, -2.0, 0.5), frames["gyroscope-turn"])
        self.assertEqual(len(runner.magnetic_record(0, 0, 0)), 20)

    def test_the_records_the_guest_refuses_are_the_golden_frames(self) -> None:
        frames = golden()
        self.assertEqual(runner.magnetic_record(0.0, 25.0, -40.0, 4), frames["reject-magnetic-status-high"])
        self.assertEqual(runner.magnetic_record(0.0, 25.0, -40.0, 255), frames["reject-magnetic-status-byte"])
        self.assertEqual(b"ANS1" + struct.pack("<HBB3f", 2, 0, 1, 0.0, 25.0, -40.0), frames["reject-magnetic-reserved"])
        self.assertEqual(runner.magnetic_record(math.nan, 0.0, 0.0), frames["reject-magnetic-nan"])
        self.assertEqual(runner.magnetic_record(0.0, math.inf, 0.0), frames["reject-magnetic-infinity"])
        self.assertEqual(runner.magnetic_record(0.0, 0.0, -math.inf), frames["reject-magnetic-negative-infinity"])
        self.assertEqual(runner.sensor_record(1, 0.0, 0.0, runner.GRAVITY, 1), frames["reject-accelerometer-status"])
        self.assertEqual(runner.sensor_record(4, 1.0, -2.0, 0.5, 3), frames["reject-gyroscope-status"])
        self.assertEqual(runner.sensor_record(3, 0.0, 0.0, 0.0), frames["reject-type-orientation-angles"])
        self.assertEqual(runner.sensor_record(14, 0.0, 25.0, -40.0), frames["reject-type-uncalibrated-field"])

    def test_the_control_frames_are_read_as_the_fixture_says(self) -> None:
        frames = golden()
        expected = {
            "control-v1-idle": runner.Frame(1, 0, 10_000, 10_000),
            "control-v1-motion": runner.Frame(1, 3, 20_000, 1_000_000),
            "control-v2-orientation": runner.Frame(2, 4, 10_000, 10_000),
            "control-v3-idle": runner.Frame(3, 0, 10_000, 10_000, 20_000),
            "control-v3-magnetometer": runner.Frame(3, 8, 10_000, 10_000, 50_000),
            "control-v3-orientation-idle": runner.Frame(3, 16, 10_000, 10_000, 20_000),
            "control-v3-orientation": runner.Frame(3, 20, 10_000, 10_000, 20_000),
            "control-v3-all": runner.Frame(3, 31, 20_000, 1_000_000, 40_000),
        }
        for name, frame in expected.items():
            self.assertEqual(runner.parse_frame(frames[name]), frame, name)
        self.assertTrue(expected["control-v3-magnetometer"].magnetometer_active)
        self.assertFalse(expected["control-v3-idle"].magnetometer_active)
        self.assertFalse(runner.parse_frame(frames["control-v2-all"]).magnetometer_active)
        # The orientation sensor is listed by "ANC2" always and by "ANC3" by bit 4.
        self.assertTrue(runner.parse_frame(frames["control-v2-idle"]).orientation_listed)
        self.assertFalse(runner.parse_frame(frames["control-v1-idle"]).orientation_listed)
        self.assertFalse(runner.parse_frame(frames["control-v3-idle"]).orientation_listed)
        self.assertTrue(runner.parse_frame(frames["control-v3-orientation-idle"]).orientation_listed)
        self.assertTrue(runner.parse_frame(frames["control-v3-all"]).orientation_active)

    def test_every_reject_control_frame_of_the_fixture_is_refused(self) -> None:
        frames = golden()
        rejected = [name for name in frames if name.startswith("reject-control-")]
        self.assertEqual(len(rejected), 8)
        for name in rejected:
            with self.assertRaises(ValueError, msg=name):
                runner.parse_frame(frames[name])

    def test_the_magic_says_how_long_a_frame_is(self) -> None:
        frames = golden()
        self.assertEqual(runner.frame_length(frames["control-v1-idle"][:4]), 16)
        self.assertEqual(runner.frame_length(frames["control-v2-idle"][:4]), 16)
        self.assertEqual(runner.frame_length(frames["control-v3-idle"][:16]), 20)
        self.assertIsNone(runner.frame_length(frames["reject-control-magic"][:4]))
        self.assertIsNone(runner.frame_length(b"ANS1"))
        with self.assertRaises(ValueError):
            runner.parse_frame(frames["control-v3-idle"][:16])
        with self.assertRaises(ValueError):
            runner.parse_frame(frames["control-v3-idle"] + b"\0")


class OracleTests(unittest.TestCase):
    def test_a_mount_turns_the_field_like_the_applications_axes(self) -> None:
        # The application's own test (MotionCompassTests): a field of (20, -5, -40) in each mount.
        self.assertEqual(runner.mounted("portrait", 20, -5, -40), (20, -5, -40))
        self.assertEqual(runner.mounted("landscapeLeft", 20, -5, -40), (-5, -20, -40))
        self.assertEqual(runner.mounted("landscapeRight", 20, -5, -40), (5, 20, -40))
        self.assertEqual(runner.mounted("portraitUpsideDown", 20, -5, -40), (-20, 5, -40))
        with self.assertRaises(ValueError):
            runner.mounted("flat", 0, 0, 0)

    def test_the_azimuth_of_each_mount_is_what_the_platform_computes(self) -> None:
        gravity = (0.0, 0.0, runner.GRAVITY)
        for mount, heading in runner.MOUNTS.items():
            field = runner.mounted(mount, 0.0, runner.HORIZONTAL, runner.VERTICAL)
            self.assertLess(runner.angle_difference(runner.azimuth(gravity, field), heading), 1e-6, mount)
        self.assertEqual(runner.MOUNTS, {"portrait": 0.0, "portraitUpsideDown": 180.0,
                                         "landscapeLeft": -90.0, "landscapeRight": 90.0})

    def test_a_turn_through_the_compass_points_gives_those_azimuths(self) -> None:
        gravity = (0.0, 0.0, runner.GRAVITY)
        for heading in (0.0, 90.0, 180.0, 270.0, 45.0, 315.0):
            field = runner.field_at_heading(heading)
            self.assertLess(runner.angle_difference(runner.azimuth(gravity, field), heading), 1e-6, heading)
            # The field's strength does not depend on the heading.
            self.assertAlmostEqual(math.sqrt(sum(v * v for v in field)), math.hypot(runner.HORIZONTAL, runner.VERTICAL))
        self.assertAlmostEqual(runner.field_at_heading(0.0)[0], 0.0)
        # A field straight along gravity gives no azimuth.
        with self.assertRaises(ValueError):
            runner.azimuth(gravity, (0.0, 0.0, -40.0))

    def test_the_difference_of_angles_wraps(self) -> None:
        self.assertEqual(runner.angle_difference(179.0, -179.0), 2.0)
        self.assertEqual(runner.angle_difference(0.0, 270.0), 90.0)
        self.assertEqual(runner.angle_difference(-90.0, 270.0), 0.0)


class ParserTests(unittest.TestCase):
    def test_the_features_are_the_names_after_feature(self) -> None:
        features = runner.parse_features(FEATURES)
        self.assertIn("android.hardware.sensor.compass", features)
        self.assertIn("reqGlEsVersion", features)
        self.assertNotIn("android.hardware.sensor.barometer", features)
        self.assertEqual(runner.parse_features("nothing"), set())

    def test_the_sub_hals_log_line_with_and_without_the_compass_property(self) -> None:
        self.assertEqual(runner.HAL_LOG_LINE.findall(LOG_LINE % (1, 4, 1)), [("1", "4", "1")])
        self.assertEqual(runner.HAL_LOG_LINE.findall(LOG_LINE % (0, 2, 0)), [("0", "2", "0")])
        # An older image's line has no compass part.
        older = "I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=1, 3 sensors"
        self.assertEqual(runner.HAL_LOG_LINE.findall(older), [("1", "3", "")])
        # The orientation runner reads the same line by its own, older pattern.
        self.assertEqual(runner.orient.HAL_LOG_LINE.findall(LOG_LINE % (1, 4, 1)), [("1", "4")])

    def test_the_probes_report(self) -> None:
        report = runner.parse_probe_report(PROBE_REPORT)
        self.assertEqual(report["magnetometer"]["latest"], [0.0, 25.0, -40.0])
        self.assertEqual(report["magnetometer"]["accuracy"], 3)
        for text in ("", "cat: files/compass.json: No such file or directory", "[1]", '{"mode":"events"}'):
            with self.assertRaises(ValueError, msg=text):
                runner.parse_probe_report(text)

    def test_the_declared_range_and_resolution_pass_the_runners_check(self) -> None:
        header = (TESTS.parent / "android/lineage-24/overlay/sensors/motion_protocol.hpp").read_text()
        declared = re.search(r"magnetometer_attributes\{\s*([\d.]+)f,\s*([\d.]+)f", header)
        self.assertIsNotNone(declared)
        maximum, resolution = float(declared[1]), float(declared[2])
        self.assertIn("steps", runner.check_figures(resolution, maximum))
        # The tolerance of a value read back is half of the service's quantum and a float's slack: under one
        # quantum, for the declared resolution and for one the service derives from the range (16 bits).
        for held in (resolution, maximum / 32768):
            self.assertLess(runner.value_tolerance(held), held * runner.SERVICE_FACTOR)
            self.assertGreater(runner.value_tolerance(held), held * runner.SERVICE_FACTOR / 2)

    def test_each_way_the_reported_figures_can_be_wrong(self) -> None:
        self.assertIn("32768 steps", runner.check_figures(4912 / 32768, 4912.0))
        cases = [
            ("too coarse", (0.7, 4912.0), "finer than"),
            ("too small a range", (0.125, 800.0), "under 900"),
            ("not a whole number of steps", (0.15, 4912.0), "not a whole number"),
            ("no resolution", (None, 4912.0), "resolution as None"),
            ("no range", (0.125, None), "range as None"),
            ("zero", (0.0, 4912.0), "resolution as 0.0"),
            ("not a number", (float("nan"), 4912.0), "resolution as nan"),
            ("a boolean", (True, 4912.0), "resolution as True"),
            ("infinite", (0.125, float("inf")), "range as inf"),
        ]
        for label, figures, message in cases:
            with self.subTest(label), self.assertRaisesRegex(runner.Failure, message):
                runner.check_figures(*figures)

    def test_the_answer_of_has_feature_is_one_of_two_pairs(self) -> None:
        self.assertIs(runner.has_feature_answer(0, "true\n"), True)
        self.assertIs(runner.has_feature_answer(1, "false\n"), False)
        for returncode, text in ((1, "true"), (0, "false"), (0, ""), (1, ""), (255, "Error: could not access the Package Manager"),
                                 (0, "True"), (0, "true false")):
            with self.subTest((returncode, text)), self.assertRaisesRegex(runner.Failure, "exit"):
                runner.has_feature_answer(returncode, text)

    def test_the_sensors_hal_is_found_by_its_service_name(self) -> None:
        listing = ("  439 /vendor/bin/hw/android.hardware.sensors-service.multihal\n"
                   "  612 /vendor/bin/hw/android.hardware.sensors.rc-something-else\n"
                   " 1023 grep -F android.hardware.sensors-service.multihal\n")
        self.assertEqual(runner.parse_hal_pids(listing), [439, 1023])
        self.assertEqual(runner.parse_hal_pids("PID ARGS\n"), [])
        stat = "439 (android.hardwar) S 1 439 439 0 -1 4194560 100 0 0 0 5 3 0 0 20 0 8 0 1234 99999 1 18446744073709551615"
        self.assertEqual(runner.parse_start_time(stat), 1234)
        self.assertEqual(runner.parse_start_time("77 (a b) c) S 1 2 3 0 0 0 0 0 0 0 5 3 0 0 20 0 8 0 4321 9"), 4321)
        for text in ("", "439 no parentheses", "439 (x) S 1"):
            with self.subTest(text), self.assertRaisesRegex(runner.Failure, "not a /proc stat line"):
                runner.parse_start_time(text)

    def test_the_sensor_service_lists_the_magnetometer(self) -> None:
        state = runner.orient.parse_sensor_state(SENSOR_DUMP)
        self.assertEqual(state.listed["Host Magnetometer"], 2)
        self.assertEqual(state.active, {})
        held = runner.orient.parse_sensor_state(HELD_DUMP)
        self.assertEqual(held.active, {"Host Magnetometer": 1})
        description = runner.orient.sensor_description(SENSOR_DUMP, "Host Magnetometer")
        self.assertIn("continuous", description or "")
        self.assertIn("maxRate=50.00Hz", description or "")


class FakeGuest:
    """The guest's end of the motion connection on loopback: says its frames, keeps the records, and ends the
    connection on a record of a status above 3 or of a sensor it does not list."""

    def __init__(self, first: bytes, takes_field: bool = True) -> None:
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.first = first
        self.takes_field = takes_field
        self.records: list[bytes] = []
        self.connections: list[socket.socket] = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self) -> None:
        while True:
            try:
                connection, _ = self.server.accept()
            except OSError:
                return
            with self.lock:
                self.connections.append(connection)
            connection.sendall(self.first)
            buffer = b""
            while True:
                try:
                    data = connection.recv(64)
                except OSError:
                    break
                if not data:
                    break
                buffer += data
                while len(buffer) >= 20:
                    record, buffer = buffer[:20], buffer[20:]
                    with self.lock:
                        self.records.append(record)
                    if record[4] == 2 and (not self.takes_field or record[6] > 3):
                        connection.close()
                        buffer = b""
                        break
            connection.close()

    def say(self, frame: bytes, *, pieces: int = 1) -> None:
        """Another frame to the newest connection, in pieces if asked."""
        with self.lock:
            connection = self.connections[-1]
        step = -(-len(frame) // pieces)
        for start in range(0, len(frame), step):
            connection.sendall(frame[start:start + step])
            threading.Event().wait(0.05)

    def wait_records(self, predicate, seconds: float = 5.0) -> list[bytes]:
        for _ in range(int(seconds / 0.05)):
            with self.lock:
                found = [record for record in self.records if predicate(record)]
            if found:
                return found
            threading.Event().wait(0.05)
        return []

    def forget(self) -> None:
        with self.lock:
            self.records.clear()

    def close(self) -> None:
        self.server.close()
        with self.lock:
            for connection in self.connections:
                connection.close()


class HostTests(unittest.TestCase):
    def test_the_host_reads_the_three_frames_and_notices_the_guest_ending_the_connection(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Frame(3, 0, 10_000, 10_000, 20_000))
            host.send(runner.magnetic_record(0.0, 25.0, -40.0, 3))
            self.assertEqual(guest.wait_records(lambda record: record[4] == 2), [frames["magnetic-status-3"]])
            host.closure_expected = True
            host.send(runner.magnetic_record(0.0, 25.0, -40.0, 4))
            self.assertTrue(host.closed_by_guest(5))
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Frame(3, 0, 10_000, 10_000, 20_000))
            self.assertFalse(host.closed)
            # The closure was asked for: nothing went wrong.
            host.raise_faults()
            self.assertEqual(host.faults, [])
        finally:
            host.stop()
            guest.close()

    def test_a_frame_that_arrives_in_pieces_is_waited_for(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v1-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Frame(1, 0, 10_000, 10_000))
            # The first 16 bytes of an "ANC3" frame are the start of one, not a frame of 16 bytes.
            guest.say(frames["control-v3-all"], pieces=5)
            self.assertEqual(host.wait_frame(5, lambda frame: frame.version == 3),
                             runner.Frame(3, 31, 20_000, 1_000_000, 40_000))
            guest.say(frames["control-v1-motion"] + frames["control-v3-magnetometer"])
            self.assertIsNotNone(host.wait_frame(5, lambda frame: frame.magnetometer_us == 50_000))
            self.assertEqual(host.bad, [])
        finally:
            host.stop()
            guest.close()

    def test_a_magic_no_frame_has_is_kept_apart(self) -> None:
        guest = FakeGuest(golden()["reject-control-magic"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            for _ in range(100):
                if host.bad:
                    break
                threading.Event().wait(0.05)
            self.assertEqual(host.frames, [])
            self.assertEqual(len(host.bad), 1)
        finally:
            host.stop()
            guest.close()

    def test_a_field_goes_out_only_while_the_latest_frame_says_the_magnetometer_is_in_use(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            host.stream((0.0, 25.0, -40.0), 2)
            threading.Event().wait(0.5)
            # Nothing is in use: nothing is sent.
            self.assertEqual(guest.wait_records(lambda record: True, 0.5), [])
            guest.say(frames["control-v3-magnetometer"])
            fields = guest.wait_records(lambda record: record[4] == 2)
            self.assertTrue(fields)
            self.assertEqual(fields[0], frames["magnetic-status-2"])
            # The accelerometer and the gyroscope are not in use, so none of theirs is sent.
            self.assertEqual([record for record in guest.records if record[4] != 2], [])
            guest.say(frames["control-v3-all"])
            self.assertTrue(guest.wait_records(lambda record: record[4] == 1))
            self.assertTrue(guest.wait_records(lambda record: record[4] == 4))
            # The magnetometer idle again: the field stops.
            guest.say(frames["control-v3-idle"])
            threading.Event().wait(0.3)
            guest.forget()
            threading.Event().wait(0.5)
            self.assertEqual(guest.wait_records(lambda record: True, 0.3), [])
        finally:
            host.stop()
            guest.close()

    def test_a_guest_without_the_sensor_is_never_sent_a_field_by_the_streamer(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v2-all"], takes_field=False)
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            host.stream((0.0, 25.0, -40.0), 3)
            self.assertTrue(guest.wait_records(lambda record: record[4] == 1))
            threading.Event().wait(0.3)
            self.assertEqual([record for record in guest.records if record[4] == 2], [])
            self.assertFalse(host.closed)
        finally:
            host.stop()
            guest.close()


class PacingTests(unittest.TestCase):
    """The feeder sends each sensor at half the period the guest's frame asked for, as the application does."""

    def simulate(self, frame: "runner.Frame", seconds: float) -> dict[int, int]:
        pacer = runner.Pacer()
        sent = {runner.ACCELEROMETER_TYPE: 0, runner.GYROSCOPE_TYPE: 0, runner.MAGNETIC_FIELD_TYPE: 0}
        for tick in range(int(seconds / runner.STREAM_TICK)):
            for kind in pacer.sensors_due(tick * runner.STREAM_TICK, frame):
                sent[kind] += 1
        return sent

    def test_the_interval_is_half_the_period_and_never_under_ten_milliseconds(self) -> None:
        self.assertEqual(runner.stream_interval(10_000), 0.01)
        self.assertEqual(runner.stream_interval(20_000), 0.01)
        self.assertEqual(runner.stream_interval(40_000), 0.02)
        self.assertEqual(runner.stream_interval(100_000), 0.05)
        self.assertEqual(runner.stream_interval(1_000_000), 0.5)

    def test_each_sensor_is_sent_at_its_own_rate(self) -> None:
        frame = runner.Frame(3, 0b01111, 10_000, 40_000, 1_000_000)
        sent = self.simulate(frame, 10.0)
        # 100 Hz (the 10 ms floor), 50 Hz, and 2 Hz for the period of 1 s.
        self.assertEqual(sent, {runner.ACCELEROMETER_TYPE: 1000, runner.GYROSCOPE_TYPE: 500,
                                runner.MAGNETIC_FIELD_TYPE: 20})
        # The old fixed 50 Hz would have sent 500 of each, 25 times the magnetometer's.
        slow = self.simulate(runner.Frame(3, 0b01000, 10_000, 10_000, 200_000), 10.0)
        self.assertEqual(slow, {runner.ACCELEROMETER_TYPE: 0, runner.GYROSCOPE_TYPE: 0, runner.MAGNETIC_FIELD_TYPE: 100})

    def test_a_sensor_not_in_use_is_not_sent_and_starts_at_once_when_it_is(self) -> None:
        pacer = runner.Pacer()
        idle = runner.Frame(3, 0, 10_000, 10_000, 20_000)
        accelerometer = runner.Frame(1, 1, 10_000, 10_000)
        self.assertEqual(pacer.sensors_due(0.0, idle), [])
        self.assertEqual(pacer.sensors_due(0.0, accelerometer), [runner.ACCELEROMETER_TYPE])
        self.assertEqual(pacer.sensors_due(0.004, accelerometer), [])
        # An older frame has no magnetometer period, and no field is sent whatever its mask says.
        self.assertEqual(pacer.sensors_due(5.0, runner.Frame(1, 0b11, 10_000, 10_000)),
                         [runner.ACCELEROMETER_TYPE, runner.GYROSCOPE_TYPE])
        # Idle forgets the schedule: the sensor is due at the next look after it is in use again.
        self.assertEqual(pacer.sensors_due(5.001, idle), [])
        self.assertEqual(pacer.sensors_due(5.002, accelerometer), [runner.ACCELEROMETER_TYPE])

    def test_a_shorter_period_is_due_within_the_new_interval(self) -> None:
        pacer = runner.Pacer()
        slow = runner.Frame(3, 0b01000, 10_000, 10_000, 1_000_000)
        fast = runner.Frame(3, 0b01000, 10_000, 10_000, 20_000)
        self.assertEqual(pacer.sensors_due(0.0, slow), [runner.MAGNETIC_FIELD_TYPE])
        self.assertEqual(pacer.sensors_due(0.1, slow), [])
        # The next record of the 1 s period was due at 0.5 s; the faster period must not wait for it.
        self.assertEqual(pacer.sensors_due(0.1, fast), [])
        self.assertEqual(pacer.sensors_due(0.111, fast), [runner.MAGNETIC_FIELD_TYPE])

    def test_a_late_look_sends_once_and_does_not_burst(self) -> None:
        pacer = runner.Pacer()
        frame = runner.Frame(3, 0b01000, 10_000, 10_000, 20_000)
        self.assertEqual(pacer.sensors_due(0.0, frame), [runner.MAGNETIC_FIELD_TYPE])
        self.assertEqual(pacer.sensors_due(5.0, frame), [runner.MAGNETIC_FIELD_TYPE])
        self.assertEqual(pacer.sensors_due(5.0, frame), [])
        self.assertEqual(pacer.sensors_due(5.005, frame), [])
        self.assertEqual(pacer.sensors_due(5.011, frame), [runner.MAGNETIC_FIELD_TYPE])

    def test_the_host_streams_at_the_rate_the_frame_asked_for(self) -> None:
        # A magnetometer period of 200 ms is a record every 100 ms: about ten in a second, where the old
        # fixed 20 ms sent fifty.
        frame = b"ANC3" + struct.pack("<4I", 0b01000, 10_000, 10_000, 200_000)
        guest = FakeGuest(frame)
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            host.stream((0.0, 25.0, -40.0), 3)
            threading.Event().wait(1.0)
            fields = [record for record in list(guest.records) if record[4] == 2]
            self.assertGreaterEqual(len(fields), 2)
            self.assertLessEqual(len(fields), 15)
            self.assertEqual([record for record in guest.records if record[4] != 2], [])
        finally:
            host.stop()
            guest.close()


class FaultTests(unittest.TestCase):
    """What the host notices on a connection and the step that was running must fail on."""

    def wait_for_fault(self, host: "runner.Host") -> None:
        for _ in range(100):
            if host.faults:
                return
            threading.Event().wait(0.05)

    def test_a_bad_frame_followed_by_a_good_one_still_fails(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["reject-control-magic"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.wait_for_fault(host)
            guest.say(frames["control-v3-idle"])
            # The good frame arrives, and a wait for it fails all the same; so does the check at the step's end.
            with self.assertRaisesRegex(runner.Failure, "no known magic"):
                host.wait_frame(5, lambda frame: frame.version == 3)
            with self.assertRaisesRegex(runner.Failure, "no known magic"):
                host.raise_faults()
            self.assertEqual(len(host.frames), 1)
        finally:
            host.stop()
            guest.close()

    def test_a_frame_with_a_known_magic_and_a_bad_mask_fails_too(self) -> None:
        guest = FakeGuest(golden()["reject-control-v3-orientation-unlisted"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.wait_for_fault(host)
            self.assertEqual(len(host.bad), 1)
            with self.assertRaisesRegex(runner.Failure, "no such state"):
                host.raise_faults()
        finally:
            host.stop()
            guest.close()

    def test_a_connection_the_guest_ends_unasked_fails_and_one_it_was_asked_to_does_not(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            guest.connections[-1].close()
            self.assertTrue(host.closed_by_guest(5))
            self.wait_for_fault(host)
            with self.assertRaisesRegex(runner.Failure, "the guest ended the connection"):
                host.raise_faults()
            # The faults of a connection are not forgotten by the next one.
            host.connect()
            threading.Event().wait(0.3)
            self.assertEqual(host.faults, ["the guest ended the connection"])
            with self.assertRaisesRegex(runner.Failure, "the guest ended the connection"):
                host.wait_frame(1)
        finally:
            host.stop()
            guest.close()

    def test_the_host_closing_its_own_end_is_no_fault(self) -> None:
        guest = FakeGuest(golden()["control-v3-idle"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            host.close()
            threading.Event().wait(0.3)
            self.assertEqual(host.faults, [])
        finally:
            host.stop()
            guest.close()

    def test_a_step_fails_on_a_fault_it_did_not_wait_for_and_says_so_when_it_fails_on_its_own(self) -> None:
        host = runner.Host(0)
        self.assertEqual(runner.checked(host, lambda: "fine"), "fine")
        host.faults.append("the guest ended the connection")
        with self.assertRaisesRegex(runner.Failure, "the guest ended the connection"):
            runner.checked(host, lambda: "the wait found what it wanted")

        def times_out() -> str:
            raise runner.Failure("the listener has [0.0], not [1.0]")

        with self.assertRaisesRegex(runner.Failure, r"the listener has .* and the connection went wrong: the guest ended"):
            runner.checked(host, times_out)
        clean = runner.Host(0)
        with self.assertRaisesRegex(runner.Failure, "^the listener has") as caught:
            runner.checked(clean, times_out)
        self.assertNotIn("connection went wrong", str(caught.exception))


class ScriptedGuest(runner.Guest):
    """A guest that answers the commands the launch check runs with the text a booted release prints."""

    ANSWERS = {"true": (0, "true\n"), "false": (1, "false\n"), "error": (1, "Error: could not access the Package Manager\n"),
               "empty": (0, ""), "true-exit-1": (1, "true\n"), "false-exit-0": (0, "false\n")}

    def __init__(self, *, property_value: str, dump: str, features: str, has_feature: str, log: str) -> None:  # no adb
        self.property_value, self.dump, self.features, self.has_feature, self.log = (
            property_value, dump, features, has_feature, log)
        self.early_log = ""
        self.commands: list[str] = []

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if command.startswith("getprop"):
            return self.property_value + "\n"
        if command == "dumpsys sensorservice":
            return self.dump
        if command == "pm list features":
            return self.features
        if command.startswith("logcat"):
            return self.log
        raise AssertionError(command)

    def adb_run(self, *arguments: str, timeout: float = 0) -> subprocess.CompletedProcess:
        # `cmd package has-feature` exits 1 and prints "false" for a feature the device does not have.
        self.commands.append(arguments[-1])
        assert arguments[-1].startswith("cmd package has-feature"), arguments
        returncode, stdout = self.ANSWERS[self.has_feature]
        return subprocess.CompletedProcess(arguments, returncode, stdout=stdout)

    def alive(self) -> None:
        pass


class LaunchStateTests(unittest.TestCase):
    def sensor_guest(self, **changes: str) -> ScriptedGuest:
        settings = dict(property_value="1", dump=SENSOR_DUMP, features=FEATURES, has_feature="true",
                        log=LOG_LINE % (0, 3, 1))
        settings.update(changes)
        return ScriptedGuest(**settings)

    def older_guest(self, **changes: str) -> ScriptedGuest:
        settings = dict(property_value="", dump=OLDER_DUMP,
                        features=FEATURES.replace("feature:android.hardware.sensor.compass\n", ""),
                        has_feature="false", log=LOG_LINE % (0, 2, 0))
        settings.update(changes)
        return ScriptedGuest(**settings)

    def test_a_release_with_the_argument_lists_the_sensor_and_declares_the_feature(self) -> None:
        note = runner.launch_state(self.sensor_guest(), True, False)
        self.assertIn("listed (type 2), continuous, non-wakeUp, 1 Hz to 50 Hz", note)
        self.assertIn("android.hardware.sensor.compass declared", note)
        self.assertIn("3 sensors, compass=1", note)
        self.assertIn("rotation vector and geomagnetic rotation vector listed", note)

    def test_with_the_orientation_sensor_too_the_list_has_four(self) -> None:
        guest = self.sensor_guest(dump=ORIENTATION_DUMP, log=LOG_LINE % (1, 4, 1))
        self.assertIn("4 sensors", runner.launch_state(guest, True, True))
        # The log line counts four, but a launch without the orientation argument expects three and no entry for it.
        with self.assertRaisesRegex(runner.Failure, "device orientation sensor .* for a launch without its argument"):
            runner.launch_state(self.sensor_guest(dump=ORIENTATION_DUMP, log=LOG_LINE % (1, 4, 1)), True, False)
        # Whatever its name, a type-27 sensor is the orientation sensor: a launch without its argument lists none.
        renamed = ORIENTATION_DUMP.replace("Host Device Orientation  ", "Some Other Sensor Name   ")
        with self.assertRaisesRegex(runner.Failure, r"by name or type 27\) for a launch without its argument"):
            runner.launch_state(self.sensor_guest(dump=renamed, log=LOG_LINE % (0, 4, 1)), True, False)
        # A launch that carried the argument needs the type-27 entry, whatever the log line says.
        with self.assertRaisesRegex(runner.Failure, r"not Host Device Orientation \(type 27\)"):
            runner.launch_state(self.sensor_guest(log=LOG_LINE % (1, 4, 1)), True, True)
        with self.assertRaisesRegex(runner.Failure, r"not Host Device Orientation \(type 27\)"):
            runner.launch_state(self.sensor_guest(dump=ORIENTATION_DUMP.replace("(27)", "(3)"), log=LOG_LINE % (1, 4, 1)),
                                True, True)

    def test_a_release_without_the_argument_lists_nothing_and_declares_nothing(self) -> None:
        note = runner.launch_state(self.older_guest(), False, False)
        self.assertIn("no magnetic field sensor", note)
        self.assertIn("not declared", note)
        # An older image's own line has no compass part.
        older = "I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=0, 2 sensors"
        self.assertIn("compass=not said", runner.launch_state(self.older_guest(log=older), False, False))

    def test_an_older_release_given_the_argument_reads_it_and_lists_nothing(self) -> None:
        # The application passes the argument to every image; an older one has the property and no consumer of it.
        guest = self.older_guest(property_value="1", log="I MotionSubHal: Andarium motion: "
                                 "ro.boot.kernarium.orientation=0, 2 sensors")
        self.assertIn("no magnetic field sensor", runner.launch_state(guest, False, False, True))
        with self.assertRaisesRegex(runner.Failure, "reads '1', expected nothing"):
            runner.launch_state(guest, False, False)
        with self.assertRaisesRegex(runner.Failure, "reads '', expected '1'|reads '', expected 1"):
            runner.launch_state(self.older_guest(), False, False, True)

    def test_each_way_the_launch_state_can_be_wrong(self) -> None:
        cases = [
            ("the property is unset", self.sensor_guest(property_value=""), True, "reads ''"),
            ("the sensor is not listed", self.sensor_guest(dump=OLDER_DUMP), True, "not Host Magnetometer"),
            ("the sensor is not 1 Hz to 50 Hz",
             self.sensor_guest(dump=SENSOR_DUMP.replace("maxRate=50.00Hz", "maxRate=100.00Hz")), True,
             "not listed at 1 Hz to 50 Hz"),
            ("no rotation vector with the magnetometer", self.sensor_guest(dump=without(SENSOR_DUMP, "(11)")), True,
             "no rotation vector with the magnetometer"),
            ("no geomagnetic rotation vector with the magnetometer",
             self.sensor_guest(dump=without(SENSOR_DUMP, "(20)")), True, "no geomagnetic rotation vector"),
            ("a rotation vector without the magnetometer",
             self.older_guest(dump=without(SENSOR_DUMP, "Host Magnetometer")), False,
             "need a magnetometer without one"),
            ("the sensor is on-change", self.sensor_guest(dump=SENSOR_DUMP.replace(
                "continuous | minRate=1.00Hz | maxRate=50.00Hz", "on-change | maxDelay=0us")), True,
             "not continuous and non-wake-up"),
            ("the feature is missing", self.sensor_guest(features="feature:android.hardware.touchscreen\n"), True,
             "pm list features says False"),
            ("the package manager disagrees", self.sensor_guest(has_feature="false"), True, "has-feature says False"),
            ("the package manager errors", self.sensor_guest(has_feature="error"), True, "exit 1 and 'Error"),
            ("the package manager says nothing", self.sensor_guest(has_feature="empty"), True, "exit 0 and ''"),
            ("true with the exit status of false", self.sensor_guest(has_feature="true-exit-1"), True, "exit 1 and 'true'"),
            ("false with the exit status of true", self.older_guest(has_feature="false-exit-0"), False, "exit 0 and 'false'"),
            ("an error for an absent feature", self.older_guest(has_feature="error"), False, "exit 1 and 'Error"),
            ("an empty answer for an absent feature", self.older_guest(has_feature="empty"), False, "exit 0 and ''"),
            ("a denial", self.sensor_guest(log=LOG_LINE % (0, 3, 1) + "\navc:  denied  { read } for "
                                           "property=ro.boot.kernarium.compass scontext=u:r:system_server:s0"),
             True, "SELinux denials"),
            ("the log says another count", self.sensor_guest(log=LOG_LINE % (0, 2, 1)), True, "expected 3 sensors"),
            ("the log says the property is off", self.sensor_guest(log=LOG_LINE % (0, 3, 0)), True, "compass=0"),
            ("held at boot", self.sensor_guest(dump=HELD_DUMP), True, "held at boot"),
            ("listed without the argument", self.older_guest(dump=SENSOR_DUMP), False, "without the argument"),
            ("a feature without the argument", self.older_guest(features=FEATURES, has_feature="true"), False,
             "expected False"),
            ("the property is set without the argument", self.older_guest(property_value="1"), False, "reads '1'"),
        ]
        for label, guest, compass, message in cases:
            with self.subTest(label), self.assertRaisesRegex(runner.Failure, message):
                runner.launch_state(guest, compass, False)


class ReportGuest:
    """A guest whose probe reports the rotation vector's azimuth, one value for each look."""

    serial = "127.0.0.1:5586"

    def __init__(self, azimuths: list[float | None], count: int = 40, cycle: bool = False, *, advance: bool = True,
                 valid: bool = True, report_run: str | None = None, probe_run_id: str | None = None) -> None:
        self.azimuths, self.count, self.looks, self.cycle = list(azimuths), count, 0, cycle
        self.advance, self.valid, self.report_run = advance, valid, report_run
        if probe_run_id is not None:
            self.probe_run_id = probe_run_id

    def alive(self) -> None:
        pass

    def adb_run(self, *arguments: str, timeout: float = 0) -> subprocess.CompletedProcess:
        value = self.azimuths[self.looks % len(self.azimuths) if self.cycle else min(self.looks, len(self.azimuths) - 1)]
        # The rotation vector's count grows with every look, as events do; ``advance=False`` freezes it.
        count = self.count + (self.looks if self.advance else 0) if value is not None else 0
        self.looks += 1
        report = {"mode": "compass", "valid": self.valid, "magnetometer": {"count": 50},
                  "rotation_vector": {"count": count, "azimuth_deg": value}}
        if self.report_run is not None:
            report["run_id"] = self.report_run
        return subprocess.CompletedProcess(arguments, 0, stdout=json.dumps(report))


class StreamingHost:
    def __init__(self) -> None:
        self.streams: list[tuple] = []

    def raise_faults(self) -> None:
        pass

    def stream(self, field, status: int = 3, gravity=None) -> None:
        self.streams.append((field, status))


class RotationVectorTests(unittest.TestCase):
    def run_step(self, azimuths: list[float | None], seconds: float = 90, cycle: bool = False, **report) -> str:
        host = StreamingHost()
        with mock.patch.object(runner.orient.time, "sleep"), mock.patch.object(runner, "ROTATION_SECONDS", seconds):
            note = runner.rotation_vector(ReportGuest(azimuths, cycle=cycle, **report), host)
        self.assertEqual(host.streams, [(runner.field_at_heading(0.0), 3)])
        return note

    def test_a_vector_that_settles_passes(self) -> None:
        self.assertIn("azimuth settled at 1.5", self.run_step([None, 40.0, 20.0, 5.0, 2.0, 1.4, 1.6, 1.5, 1.5, 1.5, 1.5]))

    def test_a_settled_heading_far_from_the_field_is_noted_not_failed(self) -> None:
        self.assertIn("the fusion's heading is its own", self.run_step([120.0] * 6))

    def test_each_way_it_does_not_settle_or_never_comes(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "still moving"):
            self.run_step([0.0, 10.0], seconds=0.3, cycle=True)
        with self.assertRaisesRegex(runner.Failure, "no rotation vector event"):
            self.run_step([None], seconds=0.3)

    def test_the_azimuth_wraps_at_180_when_it_settles(self) -> None:
        self.assertIn("settled", self.run_step([179.0, -179.0, 180.0, -180.0, 179.5, -179.5]))

    def test_a_stream_that_stops_after_one_event_never_settles(self) -> None:
        # The same report looked at again is not another sample: a frozen count passes no window.
        with self.assertRaisesRegex(runner.Failure, r"1 samples of the azimuth so far, and the count is not advancing"):
            self.run_step([1.5], seconds=0.3, advance=False)
        # A stream that gave a few events and stopped before the window filled does not settle either.
        guest = ReportGuest([1.5], advance=True)
        original = guest.adb_run
        guest.adb_run = lambda *arguments, timeout=0: original(*arguments) if guest.looks < 3 else (
            subprocess.CompletedProcess(arguments, 0, stdout='{"mode":"compass","valid":true,"magnetometer":'
                                        '{"count":50},"rotation_vector":{"count":42,"azimuth_deg":1.5}}'))
        with mock.patch.object(runner.orient.time, "sleep"), mock.patch.object(runner, "ROTATION_SECONDS", 0.3):
            with self.assertRaisesRegex(runner.Failure, "samples of the azimuth so far, and the count is not advancing"):
                runner.rotation_vector(guest, StreamingHost())

    def test_a_report_of_another_run_or_an_invalid_one_or_a_heading_that_is_no_number_never_passes(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "the report is of run '9', not the probe's '42'"):
            self.run_step([1.5] * 8, seconds=0.3, report_run="9", probe_run_id="42")
        self.assertIn("settled", self.run_step([1.5] * 8, report_run="42", probe_run_id="42"))
        with self.assertRaisesRegex(runner.Failure, "the probe says it is not valid"):
            self.run_step([1.5] * 8, seconds=0.3, valid=False)
        for bad in (float("nan"), float("inf")):
            with self.subTest(bad), self.assertRaisesRegex(runner.Failure, "heading is"):
                self.run_step([bad] * 8, seconds=0.3)

    def test_the_note_comes_from_the_report_that_settled(self) -> None:
        note = self.run_step([40.0, 20.0, 5.0, 2.0, 1.4, 1.6, 1.5, 1.5, 1.5, 1.5])
        self.assertRegex(note, r"^\d+ events, azimuth settled at 1\.5")


class LiveGuest:
    """A guest with a probe that reports the magnetometer, a system_server and a sensors HAL, each with an identity
    that can change between the ``before`` and ``after`` stages of a step."""

    serial = "127.0.0.1:5586"

    def __init__(self, run_id: str = "42") -> None:
        self.probe_run_id = run_id
        self.report_run = run_id
        self.stage = "before"
        self.system_server_pid = {"before": "1000", "after": "1000"}
        self.system_server_start = {"before": 777, "after": 777}
        self.hal = {"before": (439, 1234), "after": (439, 1234)}
        self.magnetometer: dict = {
            "count": 50, "last_ns": 1_000_000, "latest": list(runner.field_at_heading(0.0)), "accuracy": 3,
            "resolution": 0.125, "max_range": 4912.0, "accuracy_calls": [3], "azimuth_deg": 0.0}

    def alive(self) -> None:
        pass

    def system_server(self) -> str:
        return self.shell("pidof system_server").strip()

    def shell(self, command: str, timeout: float = 0) -> str:
        if command == "pidof system_server":
            return self.system_server_pid[self.stage] + "\n"
        if command.startswith("ps -A"):
            return f"  {self.hal[self.stage][0]} /vendor/bin/hw/{runner.SENSORS_HAL}\n"
        if command.startswith("cat /proc/"):
            pid = command.split("/")[2]
            if pid == self.system_server_pid[self.stage]:
                return f"{pid} (system_server) S 1 1 1 0 -1 4194560 1 0 0 0 5 3 0 0 20 0 8 0 {self.system_server_start[self.stage]} 9 1\n"
            return (f"{self.hal[self.stage][0]} (android.hardwar) S 1 439 439 0 -1 4194560 100 0 0 0 5 3 0 0 20 0 8 0 "
                    f"{self.hal[self.stage][1]} 99999 1\n")
        raise AssertionError(command)

    def adb_run(self, *arguments: str, timeout: float = 0) -> subprocess.CompletedProcess:
        report = {"mode": "compass", "run_id": self.report_run, "valid": True, "magnetometer": dict(self.magnetometer),
                  "rotation_vector": {"count": 5}}
        return subprocess.CompletedProcess(arguments, 0, stdout=json.dumps(report))

    def deliver(self, field: tuple[float, float, float], *, count: int | None = None, last_ns: int | None = None) -> None:
        self.magnetometer["latest"] = list(field)
        self.magnetometer["count"] = self.magnetometer["count"] + 30 if count is None else count
        self.magnetometer["last_ns"] = self.magnetometer["last_ns"] + 1_000_000_000 if last_ns is None else last_ns


class WaitFieldTests(unittest.TestCase):
    EXPECTED = (0.0, 25.0, -40.0)

    def wait(self, guest: LiveGuest, **keywords) -> dict:
        with mock.patch.object(runner.orient.time, "sleep"), mock.patch.dict(
                runner.wait_field.__kwdefaults__, {"seconds": 0.2}):
            return runner.wait_field(guest, self.EXPECTED, "a field", **keywords)

    def failing(self, guest: LiveGuest, message: str, **keywords) -> None:
        with self.assertRaisesRegex(runner.Failure, message):
            self.wait(guest, **keywords)

    def test_the_field_that_was_sent_passes(self) -> None:
        guest = LiveGuest()
        guest.magnetometer["latest"] = list(self.EXPECTED)
        self.assertEqual(self.wait(guest, status=3, callback=True, heading=0.0)["count"], 50)

    def test_a_value_the_service_rounded_passes_within_half_its_quantum_and_not_beyond(self) -> None:
        for resolution in (0.125, 4912 / 32768):
            quantum = resolution * runner.SERVICE_FACTOR
            guest = LiveGuest()
            guest.magnetometer.update(resolution=resolution)
            guest.magnetometer["latest"] = [0.0, 25.0 + quantum * 0.49, -40.0 - quantum * 0.49]
            self.wait(guest)
            # More than half a quantum off is a value the service did not round from the one sent; one whole
            # quantum off must never pass.
            for off in (quantum * 0.6, quantum):
                guest.magnetometer["latest"] = [0.0, 25.0 + off, -40.0]
                with self.subTest(resolution=resolution, off=off):
                    self.failing(guest, "the listener has")

    def test_a_value_that_is_not_a_number_fails(self) -> None:
        for bad in (float("nan"), float("inf"), None, "25"):
            guest = LiveGuest()
            guest.magnetometer["latest"] = [0.0, bad, -40.0]
            with self.subTest(bad):
                self.failing(guest, "not three numbers")
        guest = LiveGuest()
        guest.magnetometer["latest"] = [0.0, 25.0]
        self.failing(guest, "not three numbers")

    def test_a_missing_or_unusable_resolution_fails(self) -> None:
        for resolution in (None, 0.0, float("nan"), -0.1):
            guest = LiveGuest()
            guest.magnetometer.update(latest=list(self.EXPECTED), resolution=resolution)
            with self.subTest(resolution):
                self.failing(guest, "resolution as")

    def test_the_azimuth_and_the_accuracy_and_the_callback_are_checked(self) -> None:
        guest = LiveGuest()
        guest.magnetometer["latest"] = list(self.EXPECTED)
        self.failing(guest, "azimuth 0.0, not 90.0", heading=90.0)
        guest.magnetometer["azimuth_deg"] = float("nan")
        self.failing(guest, "azimuth nan, not 0.0", heading=0.0)
        guest.magnetometer["azimuth_deg"] = None
        self.failing(guest, "azimuth None, not 0.0", heading=0.0)
        guest.magnetometer["accuracy"] = 2
        self.failing(guest, "accuracy 2, not 3", status=3)
        # SensorEvent.accuracy says 3 but onAccuracyChanged was last called with another value, or never.
        guest.magnetometer.update(accuracy=3, accuracy_calls=[3, 1])
        self.failing(guest, "onAccuracyChanged was last called with 1, not 3", status=3, callback=True)
        guest.magnetometer["accuracy_calls"] = []
        self.failing(guest, "onAccuracyChanged was last called with None, not 3", status=3, callback=True)

    def test_a_report_of_another_run_is_never_taken(self) -> None:
        guest = LiveGuest()
        guest.magnetometer["latest"] = list(self.EXPECTED)
        guest.report_run = "41"
        self.failing(guest, "run '41', not the probe's '42'")
        guest.report_run = "42"
        self.wait(guest)

    def test_a_new_sample_must_advance_both_the_count_and_the_timestamp(self) -> None:
        for label, count, last_ns in (("frozen", 50, 1_000_000), ("the count only", 80, 1_000_000),
                                      ("the timestamp only", 50, 2_000_000)):
            guest = LiveGuest()
            guest.magnetometer.update(latest=list(self.EXPECTED), count=count, last_ns=last_ns)
            with self.subTest(label):
                self.failing(guest, "no new sample", newer=(50, 1_000_000))
        guest = LiveGuest()
        guest.magnetometer.update(latest=list(self.EXPECTED), count=51, last_ns=1_000_001)
        self.wait(guest, newer=(50, 1_000_000))


class RefusingHost:
    """The host's end for the steps that make the guest end a connection: what the runner does to it, and the answers
    a guest of each kind gives."""

    def __init__(self, guest: LiveGuest, *, ends: bool = True, in_use: bool = True, recovers: bool = True,
                 frames: list | None = None) -> None:
        self.guest, self.ends, self.in_use, self.recovers = guest, ends, in_use, recovers
        self.frames = list(frames or [])
        #: What the next connection's first frame is, for a guest of an older wire.
        self.again = self.frames[-1] if self.frames else None
        self.closure_expected = False
        self.sent: list[bytes] = []
        self.streams: list[tuple] = []
        self.connects = 0

    def stream(self, field, status: int = 3, gravity=None) -> None:
        self.streams.append((field, status))
        if self.guest.stage == "after" and self.recovers and field is not None:
            self.guest.deliver(field)

    def send(self, data: bytes) -> None:
        self.sent.append(data)

    def closed_by_guest(self, seconds: float) -> bool:
        return self.ends

    def connect(self) -> None:
        self.connects += 1
        self.guest.stage = "after"
        self.closure_expected = False

    def wait_frame(self, seconds: float, predicate=None, after: int = 0):
        if self.again is not None:
            return self.again
        return runner.Frame(3, 8 if self.in_use else 0, 20_000, 20_000, 20_000)

    def raise_faults(self) -> None:
        pass


class RefusedRecordTests(unittest.TestCase):
    def step(self, guest: LiveGuest, host: RefusingHost) -> str:
        with mock.patch.object(runner.orient.time, "sleep"), mock.patch.dict(
                runner.wait_field.__kwdefaults__, {"seconds": 0.2}):
            return runner.refused_record(guest, host)

    def test_a_guest_that_recovers_with_a_new_field_and_the_same_processes_passes(self) -> None:
        guest = LiveGuest()
        host = RefusingHost(guest)
        note = self.step(guest, host)
        self.assertIn("sample 80 after 50", note)
        self.assertIn("sensors HAL (pid 439)", note)
        # The record sent is the one the guest refuses, with the closure announced first; the field after it is
        # another heading's than the one before.
        self.assertEqual(host.sent, [runner.magnetic_record(0.0, runner.HORIZONTAL, runner.VERTICAL, 4)])
        self.assertEqual(host.connects, 1)
        self.assertEqual(host.streams[-1], (runner.field_at_heading(runner.RECOVERY_HEADING), 3))
        self.assertNotEqual(runner.field_at_heading(runner.RECOVERY_HEADING), runner.field_at_heading(0.0))

    def test_a_report_that_never_moves_again_is_no_recovery(self) -> None:
        guest = LiveGuest()
        with self.assertRaisesRegex(runner.Failure, "no new sample|the listener has"):
            self.step(guest, RefusingHost(guest, recovers=False))
        # The old field with a new count and timestamp is not the field that was sent.
        guest = LiveGuest()
        host = RefusingHost(guest)
        host.stream = lambda field, status=3, gravity=None: guest.deliver(tuple(guest.magnetometer["latest"])) if guest.stage == "after" else None  # type: ignore[method-assign]
        with self.assertRaisesRegex(runner.Failure, "the listener has"):
            self.step(guest, host)

    def test_the_sensors_hal_or_system_server_restarting_fails_it(self) -> None:
        for label, change, message in (
                ("a new pid", {"hal": {"after": (500, 1234)}}, "sensors HAL restarted"),
                ("the same pid and a new start time", {"hal": {"after": (439, 9999)}}, "sensors HAL restarted"),
                ("system_server", {"system_server_pid": {"after": "2000"}}, "system_server restarted"),
                ("system_server's start time", {"system_server_start": {"after": 999}}, "system_server restarted")):
            guest = LiveGuest()
            for name, value in change.items():
                getattr(guest, name).update(value)
            with self.subTest(label), self.assertRaisesRegex(runner.Failure, message):
                self.step(guest, RefusingHost(guest))

    def test_the_other_ways_the_step_fails(self) -> None:
        guest = LiveGuest()
        with self.assertRaisesRegex(runner.Failure, "kept the connection"):
            self.step(guest, RefusingHost(guest, ends=False))
        guest = LiveGuest()
        with self.assertRaisesRegex(runner.Failure, "does not say the magnetometer is in use"):
            self.step(guest, RefusingHost(guest, in_use=False))
        guest = LiveGuest()
        guest.report_run = "7"
        with self.assertRaisesRegex(runner.Failure, "is of run '7', not '42'"):
            self.step(guest, RefusingHost(guest))
        guest = LiveGuest()
        guest.magnetometer["count"] = 0
        with self.assertRaisesRegex(runner.Failure, "no valid samples"):
            self.step(guest, RefusingHost(guest))


class OlderWireTests(unittest.TestCase):
    def step(self, guest: LiveGuest, host: RefusingHost, with_orientation: bool = False) -> str:
        with mock.patch.object(runner.time, "sleep"):
            return runner.older_wire(guest, host, with_orientation)

    def test_an_image_without_the_sensor_ends_the_connection_and_keeps_its_processes(self) -> None:
        guest = LiveGuest()
        note = self.step(guest, RefusingHost(guest, frames=[runner.Frame(1, 0, 10_000, 10_000)]))
        self.assertIn("sensors HAL, pid 439, did not restart", note)
        guest = LiveGuest()
        self.assertIn("ANC2 only", self.step(
            guest, RefusingHost(guest, frames=[runner.Frame(2, 0, 10_000, 10_000)]), with_orientation=True))

    def test_the_hal_or_system_server_restarting_or_a_frame_of_the_new_wire_fails_it(self) -> None:
        for label, change, message in (
                ("the HAL", {"hal": {"after": (500, 1234)}}, "sensors HAL restarted"),
                ("the HAL's start time", {"hal": {"after": (439, 5)}}, "sensors HAL restarted"),
                ("system_server", {"system_server_pid": {"after": "2000"}}, "system_server restarted"),
                ("system_server's start time", {"system_server_start": {"after": 999}}, "system_server restarted")):
            guest = LiveGuest()
            for name, value in change.items():
                getattr(guest, name).update(value)
            with self.subTest(label), self.assertRaisesRegex(runner.Failure, message):
                self.step(guest, RefusingHost(guest, frames=[runner.Frame(1, 0, 10_000, 10_000)]))
        guest = LiveGuest()
        with self.assertRaisesRegex(runner.Failure, "frames of an image without the sensor"):
            self.step(guest, RefusingHost(guest, frames=[runner.Frame(3, 0, 10_000, 10_000, 20_000)]))
        guest = LiveGuest()
        with self.assertRaisesRegex(runner.Failure, "kept the connection"):
            self.step(guest, RefusingHost(guest, ends=False, frames=[runner.Frame(1, 0, 10_000, 10_000)]))


class FeedTests(unittest.TestCase):
    def test_feed_streams_a_steady_field_for_the_time_asked(self) -> None:
        host = StreamingHost()
        host.connect = lambda: None
        host.wait_frame = lambda seconds: object()
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(
                runner.time, "monotonic", side_effect=[0.0, 0.0, 1.0, 2.5, 3.5, 5.0]):
            note = runner.feed(ReportGuest([0.0]), host, 3)
        self.assertIn("fed 3 s on 127.0.0.1:5586", note)
        self.assertEqual(host.streams, [(runner.field_at_heading(0.0), 3)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
