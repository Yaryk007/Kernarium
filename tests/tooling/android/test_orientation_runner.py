#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The orientation runner's wire, its reading of the guest, and its probe application.

run_android_orientation.py plays the host application against a booted release: it reads the guest's control
frames, sends orientation records, and reads Android's rotation from `dumpsys window displays`, the sensor service
from `dumpsys sensorservice` and the navigation bar from `uiautomator dump`. The texts below have the layout those
commands print (WindowOrientationListener, SensorService, SystemUI), so the runner's parsers, its host and the
rotation each request earns run without a guest. The frames are the golden frames of tests/unit/andarium/fixtures.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import io
import math
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_orientation.py"
SPEC = importlib.util.spec_from_file_location("run_android_orientation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
probe = runner.orientation_probe_apk
FIXTURE = TESTS / "unit/andarium/fixtures/motion-orientation-golden-frames.txt"


def golden() -> dict[str, bytes]:
    frames = {}
    for line in FIXTURE.read_text().splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        name, _, hex_bytes = line.partition(":")
        frames[name.strip()] = bytes.fromhex(hex_bytes.strip())
    return frames


# A fresh guest in the setup wizard, then the same display after the window manager chose the orientation sensor.
ROTATION_DUMP = """\
  DisplayRotation
    mCurrentAppOrientation=SCREEN_ORIENTATION_UNSPECIFIED
    mLastOrientation=-1
    mRotation=0 mDeferredRotationPauseCount=0
    mLandscapeRotation=ROTATION_90 mSeascapeRotation=ROTATION_270
    mPortraitRotation=ROTATION_0 mUpsideDownRotation=ROTATION_180
    mSupportAutoRotation=true
    WindowOrientationListener
      mEnabled=true
      mCurrentRotation=ROTATION_0
      mSensorType=null
      mSensor={Sensor name="Host Accelerometer", vendor="Andarium", version=1, type=1, maxRange=156.906}
      mRate=2
      AccelSensorJudge
        mProposedRotation=-1
        mPredictedRotation=-1
        mLastFilteredX=0.0

    mCarDockRotation=-1 mDeskDockRotation=-1
    mUserRotationMode=USER_ROTATION_FREE mUserRotation=ROTATION_0 mCameraRotationMode=0 mAllowAllRotations=false
"""

ORIENTATION_DUMP = (
    ROTATION_DUMP
    .replace("mRotation=0 ", "mRotation=3 ")
    .replace("SCREEN_ORIENTATION_UNSPECIFIED", "SCREEN_ORIENTATION_SENSOR_LANDSCAPE")
    .replace('name="Host Accelerometer", vendor="Andarium", version=1, type=1',
             'name="Host Device Orientation", vendor="Andarium", version=1, type=27')
    .replace("AccelSensorJudge", "OrientationSensorJudge")
    .replace("mUserRotationMode=USER_ROTATION_FREE mUserRotation=ROTATION_0",
             "mUserRotationMode=USER_ROTATION_LOCKED mUserRotation=ROTATION_90")
)

SENSOR_DUMP = """\
Sensor List:
0x00000001) Host Accelerometer        | Andarium        | ver: 1 | type: android.sensor.accelerometer(1) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000002) Host Gyroscope            | Andarium        | ver: 1 | type: android.sensor.gyroscope(4) | perm: n/a | flags: 0x00000000
\tcontinuous | minRate=1.00Hz | maxRate=100.00Hz | no batching | non-wakeUp |
0x00000003) Host Device Orientation   | Andarium        | ver: 1 | type: android.sensor.device_orientation(27) | perm: n/a | flags: 0x00000002
\ton-change | maxDelay=0us | no batching | non-wakeUp |
0x5f676172) Game Rotation Vector Sensor | AOSP            | ver: 3 | type: android.sensor.game_rotation_vector(15) | perm: n/a | flags: 0x00000000
\tcontinuous | maxDelay=0us | maxRate=100.00Hz | no batching | non-wakeUp |
Fusion States:
9-axis fusion disabled (0 clients), gyro-rate= 200.00Hz, q=< 0, 0, 0, 0 > (0), b=< 0, 0, 0 >
Recent Sensor events:
Active sensors:
Host Device Orientation (handle=0x00000003, connections=1)
Socket Buffer size = 39 events

--- Client Listeners (2 registered) ---
1 open event connections
Connection Number: 0
\tOperating Mode: NORMAL
\t com.android.server.wm.WindowOrientationListener$OrientationSensorJudge | WakeLockRefCount 0 | uid 1000 | cache size 0
"""

HIERARCHY = """\
<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation="1">
<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="com.android.settings" content-desc="" bounds="[0,0][1280,720]">
<node index="0" text="" resource-id="com.android.systemui:id/rotate_suggestion" class="android.widget.ImageView" package="com.android.systemui" content-desc="Rotate screen" bounds="[1100,600][1180,680]"/>
<node index="1" text="Back" resource-id="" class="android.widget.ImageView" package="com.android.systemui" content-desc="Back" bounds="[10,600][90,680]"/>
</node></hierarchy>
"""


# `uiautomator dump --windows`: a root of displays, each window with a hierarchy; the button is a window of the
# launcher's taskbar on a large screen.
HIERARCHY_WINDOWS = """\
<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><displays><display id="0">
<window index="0" title="" bounds="[0,1184][96,1280]" active="false" id="20" type="TYPE_SYSTEM"><hierarchy rotation="0">
<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="com.android.launcher3" content-desc="" bounds="[0,1184][96,1280]">
<node index="0" text="" resource-id="com.android.launcher3:id/rotate_suggestion" class="android.widget.ImageView" package="com.android.launcher3" content-desc="Rotate screen" bounds="[0,1184][96,1280]"/>
</node></hierarchy></window>
<window index="1" title="Settings" bounds="[0,0][720,1280]" active="true" id="7" type="TYPE_APPLICATION"><hierarchy rotation="0">
<node index="0" text="Settings" resource-id="" class="android.widget.TextView" package="com.android.settings" content-desc="" bounds="[0,0][720,80]"/>
</hierarchy></window></display></displays>
"""


class WireTests(unittest.TestCase):
    def test_the_records_are_the_golden_frames(self) -> None:
        frames = golden()
        for value in range(4):
            self.assertEqual(runner.orientation_record(value), frames[f"orientation-{value}"])
        self.assertEqual(len(runner.orientation_record(0)), runner.RECORD_SIZE)

    def test_the_records_the_guest_refuses_are_the_golden_frames(self) -> None:
        frames = golden()
        self.assertEqual(runner.orientation_record(4), frames["reject-orientation-high"])
        self.assertEqual(runner.orientation_record(-1), frames["reject-orientation-negative"])
        self.assertEqual(runner.orientation_record(1.5), frames["reject-orientation-fraction"])
        self.assertEqual(b"ANS1" + struct.pack("<I3f", 27, 1.0, 1.0, 0.0), frames["reject-orientation-second"])
        self.assertEqual(b"ANS1" + struct.pack("<I3f", 27, 1.0, 0.0, 1.0), frames["reject-orientation-third"])
        self.assertEqual(b"ANS1" + struct.pack("<I3f", 27, math.nan, 0.0, 0.0), frames["reject-orientation-nan"])

    def test_the_control_frames_are_read_as_the_fixture_says(self) -> None:
        frames = golden()
        expected = {
            "control-v1-idle": runner.Control(1, 0, 10_000, 10_000),
            "control-v1-motion": runner.Control(1, 3, 20_000, 1_000_000),
            "control-v2-idle": runner.Control(2, 0, 10_000, 10_000),
            "control-v2-orientation": runner.Control(2, 4, 10_000, 10_000),
            "control-v2-all": runner.Control(2, 7, 20_000, 1_000_000),
        }
        for name, control in expected.items():
            self.assertEqual(runner.parse_control(frames[name]), control, name)
        self.assertTrue(expected["control-v2-orientation"].orientation_active)
        self.assertFalse(expected["control-v2-idle"].orientation_active)
        for name in ("reject-control-v1-orientation", "reject-control-reserved", "reject-control-magic"):
            with self.assertRaises(ValueError, msg=name):
                runner.parse_control(frames[name])
        with self.assertRaises(ValueError):
            runner.parse_control(frames["control-v2-idle"][:15])

    def test_the_20_byte_frame_of_a_guest_with_the_magnetometer_is_read_as_the_fixture_says(self) -> None:
        frames = golden()
        expected = {
            "control-v3-idle": (runner.Control(3, 0, 10_000, 10_000, 20_000), False, False),
            "control-v3-magnetometer": (runner.Control(3, 8, 10_000, 10_000, 50_000), False, False),
            "control-v3-orientation-idle": (runner.Control(3, 16, 10_000, 10_000, 20_000), True, False),
            "control-v3-orientation": (runner.Control(3, 20, 10_000, 10_000, 20_000), True, True),
            "control-v3-all": (runner.Control(3, 31, 20_000, 1_000_000, 40_000), True, True),
        }
        for name, (control, listed, active) in expected.items():
            parsed = runner.parse_control(frames[name])
            self.assertEqual(parsed, control, name)
            self.assertEqual((parsed.orientation_listed, parsed.orientation_active), (listed, active), name)
        # Version 2 always lists the sensor; version 1 never does.
        self.assertTrue(runner.parse_control(frames["control-v2-idle"]).orientation_listed)
        self.assertFalse(runner.parse_control(frames["control-v1-idle"]).orientation_listed)
        rejected = [name for name in frames if name.startswith("reject-control-")]
        self.assertGreaterEqual(len(rejected), 8)
        for name in rejected:
            with self.assertRaises(ValueError, msg=name):
                runner.parse_control(frames[name])
        with self.assertRaises(ValueError):
            runner.parse_control(frames["control-v3-idle"][:16])
        self.assertEqual([runner.frame_length(frames[name][:4]) for name in
                          ("control-v1-idle", "control-v2-idle", "control-v3-idle", "reject-control-magic")],
                         [16, 16, 20, None])

    def test_each_posture_is_the_orientation_the_accelerometer_judge_reads_from_it(self) -> None:
        # AccelSensorJudge: orientationAngle = -atan2(-x, y) in degrees, and the nearest quarter turn is
        # the rotation (frameworks/base WindowOrientationListener).
        for value, (x, y, _) in runner.POSTURES.items():
            angle = round(-math.degrees(math.atan2(-x, y))) % 360
            self.assertEqual(((angle + 45) // 90) % 4, value, f"posture {value}")


class ParserTests(unittest.TestCase):
    def test_the_rotation_state_of_a_guest_that_judges_the_accelerometer(self) -> None:
        state = runner.parse_rotation_state(ROTATION_DUMP)
        self.assertEqual((state.rotation, state.user_mode, state.user_rotation, state.allow_all),
                         (0, "USER_ROTATION_FREE", 0, False))
        self.assertEqual((state.landscape, state.seascape), (1, 3))
        self.assertEqual(state.app_orientation, "SCREEN_ORIENTATION_UNSPECIFIED")
        self.assertEqual((state.listener_enabled, state.judge, state.proposed), (True, "AccelSensorJudge", -1))
        self.assertIn("Host Accelerometer", state.sensor or "")

    def test_the_rotation_state_of_a_guest_that_follows_the_orientation_sensor(self) -> None:
        state = runner.parse_rotation_state(ORIENTATION_DUMP)
        self.assertEqual((state.rotation, state.user_mode, state.user_rotation), (3, "USER_ROTATION_LOCKED", 1))
        self.assertEqual((state.judge, state.app_orientation), ("OrientationSensorJudge", "SCREEN_ORIENTATION_SENSOR_LANDSCAPE"))
        self.assertIn("Host Device Orientation", state.sensor or "")

    def test_a_dump_without_the_section_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            runner.parse_rotation_state("Display{#0 state=ON size=720x1280 ROTATION_0}\n")

    def test_the_sensor_service_lists_and_the_active_sensors(self) -> None:
        state = runner.parse_sensor_state(SENSOR_DUMP)
        self.assertEqual(state.listed["Host Device Orientation"], 27)
        self.assertEqual(state.listed["Host Accelerometer"], 1)
        self.assertEqual(state.active, {"Host Device Orientation": 1})
        self.assertIn("type: android.sensor.device_orientation(27)", state.orientation_line or "")
        without = runner.parse_sensor_state(SENSOR_DUMP.replace("Host Device Orientation", "Other"))
        self.assertNotIn("Host Device Orientation", without.listed)
        self.assertIsNone(without.orientation_line)

    def test_the_sensors_description_and_who_holds_it(self) -> None:
        description = runner.sensor_description(SENSOR_DUMP, "Host Device Orientation")
        self.assertIn("type: android.sensor.device_orientation(27)", description or "")
        self.assertIn("on-change", description or "")
        self.assertIn("non-wakeUp", description or "")
        # A name that is only the start of another does not match it.
        self.assertIsNone(runner.sensor_description(SENSOR_DUMP, "Host Device"))
        self.assertEqual(runner.judges_connected(SENSOR_DUMP), ["OrientationSensorJudge"])
        self.assertEqual(runner.judges_connected("nothing here"), [])
        accelerometer = runner.sensor_description(SENSOR_DUMP, "Host Accelerometer")
        self.assertIn("continuous", accelerometer or "")

    def test_the_sub_hals_log_line_and_the_denials_that_matter(self) -> None:
        line = "11-12 22:00:01.000   439   439 I MotionSubHal: Andarium motion: ro.boot.kernarium.orientation=1, 3 sensors"
        self.assertEqual(runner.HAL_LOG_LINE.findall(line), [("1", "3")])
        self.assertEqual(runner.HAL_LOG_LINE.findall(line.replace("=1, 3", "=0, 2")), [("0", "2")])
        unrelated = "type=1400 audit: avc:  denied  { read } for comm=\"foo\" scontext=u:r:untrusted_app:s0 tcontext=u:object_r:x:s0"
        sensors = ("avc:  denied  { read } for property=ro.boot.kernarium.orientation pid=439 "
                   "scontext=u:r:hal_sensors_default:s0 tcontext=u:object_r:bootloader_prop:s0")
        both = f"noise\n{line}\nmore noise\n{unrelated}\nI/other: nothing\n"
        self.assertEqual(runner.hal_log_lines(both), f"{line}\n{unrelated}")
        self.assertEqual(runner.hal_log_lines("nothing here"), "")
        self.assertEqual(runner.sensors_hal_denials(unrelated), [])
        self.assertEqual(runner.sensors_hal_denials(f"{unrelated}\n{sensors}\n"), [sensors])
        self.assertEqual(runner.sensors_hal_denials("avc: granted { read } for hal_sensors_default"), [])

    def test_the_windows_dump_has_the_taskbars_button_whose_package_is_the_launchers(self) -> None:
        rotation, nodes = runner.parse_hierarchy(HIERARCHY_WINDOWS)
        self.assertEqual(rotation, 0)
        button = runner.suggestion_button(nodes)
        self.assertIsNotNone(button)
        self.assertEqual((button.package, button.centre), ("com.android.launcher3", (48, 1232)))
        self.assertIsNone(runner.suggestion_button([node for node in nodes if node.package == "com.android.settings"]))

    def test_the_rotation_suggestion_button_is_systemuis_rotate_screen(self) -> None:
        rotation, nodes = runner.parse_hierarchy(HIERARCHY)
        self.assertEqual(rotation, 1)
        button = runner.suggestion_button(nodes)
        self.assertIsNotNone(button)
        assert button is not None
        self.assertEqual(button.centre, (1140, 640))
        # A "Rotate" in another package is not the button.
        _, others = runner.parse_hierarchy(HIERARCHY.replace('package="com.android.systemui" content-desc="Rotate screen"',
                                                             'package="com.example" content-desc="Rotate screen"'))
        self.assertIsNone(runner.suggestion_button(others))


class RotationRuleTests(unittest.TestCase):
    """DisplayRotation.rotationForOrientation, for the requests the runner makes (a phone: no 180 degree turn)."""

    def rotate(self, request: str, value: int | None, last: int, **options: object) -> int:
        return runner.expected_rotation(request, value, last, **options)  # type: ignore[arg-type]

    def test_an_application_without_a_request_follows_the_sensor_except_upside_down(self) -> None:
        for value, expected in ((0, 0), (1, 1), (3, 3)):
            self.assertEqual(self.rotate("unspecified", value, 0), expected)
        self.assertEqual(self.rotate("unspecified", 2, 1), 1)
        self.assertEqual(self.rotate("unspecified", 2, 1, allow_all=True), 2)
        # Before the host says anything the display keeps its rotation.
        self.assertEqual(self.rotate("unspecified", None, 3), 3)

    def test_a_locked_user_rotation_beats_the_sensor_for_an_application_without_a_request(self) -> None:
        self.assertEqual(self.rotate("unspecified", 3, 0, locked=True, user_rotation=1), 1)
        # A sensor request is the application's own, whatever the lock says.
        self.assertEqual(self.rotate("fullSensor", 3, 0, locked=True, user_rotation=1), 3)

    def test_a_fixed_request_is_not_turned_by_the_sensor(self) -> None:
        for value in range(4):
            self.assertEqual(self.rotate("landscape", value, 0), 1, value)
            self.assertEqual(self.rotate("portrait", value, 1), 0, value)

    def test_sensor_landscape_follows_the_sensor_between_the_two_landscapes(self) -> None:
        self.assertEqual(self.rotate("sensorLandscape", 3, 1), 3)
        self.assertEqual(self.rotate("sensorLandscape", 1, 3), 1)
        # Upright or upside down is no landscape: the display keeps the landscape it has.
        self.assertEqual(self.rotate("sensorLandscape", 0, 3), 3)
        self.assertEqual(self.rotate("sensorLandscape", 2, 1), 1)
        self.assertEqual(self.rotate("sensorLandscape", 0, 0), 1)

    def test_full_sensor_takes_all_four(self) -> None:
        for value in range(4):
            self.assertEqual(self.rotate("fullSensor", value, 0), value)


class FakeGuest:
    """The guest's end of the motion connection on loopback: says a frame on connect, keeps the records, and
    ends the connection on a record it refuses."""

    def __init__(self, first: bytes, delay: float = 0.0) -> None:
        self.delay = delay
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.first = first
        self.records: list[bytes] = []
        self.connections = 0
        self.connections_open: list[socket.socket] = []
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
                self.connections += 1
                self.connections_open.append(connection)
            if self.delay:
                threading.Event().wait(self.delay)
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
                while len(buffer) >= runner.RECORD_SIZE:
                    record, buffer = buffer[:runner.RECORD_SIZE], buffer[runner.RECORD_SIZE:]
                    with self.lock:
                        self.records.append(record)
                    if record == golden()["reject-orientation-high"]:
                        connection.close()
                        break
            connection.close()

    def say(self, frame: bytes) -> None:
        """Another frame to the newest connection."""
        with self.lock:
            connection = self.connections_open[-1]
        connection.sendall(frame)

    def close(self) -> None:
        self.server.close()


class HostTests(unittest.TestCase):
    def test_the_host_reads_frames_sends_records_and_notices_the_guest_ending_the_connection(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v2-orientation"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            control = host.wait_frame(5)
            self.assertEqual(control, runner.Control(2, 4, 10_000, 10_000))
            host.send(runner.orientation_record(3))
            deadline = threading.Event()
            for _ in range(100):
                with guest.lock:
                    if guest.records:
                        break
                deadline.wait(0.05)
            self.assertEqual(guest.records, [frames["orientation-3"]])
            host.send(runner.orientation_record(4))
            self.assertTrue(host.closed_by_guest(5))
            # A new connection starts again: its frames are its own.
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Control(2, 4, 10_000, 10_000))
            self.assertFalse(host.closed)
            self.assertEqual(guest.connections, 2)
        finally:
            host.close()
            guest.close()

    def records_within(self, guest: FakeGuest, count: int) -> list[bytes]:
        for _ in range(100):
            with guest.lock:
                if len(guest.records) >= count:
                    break
            threading.Event().wait(0.05)
        with guest.lock:
            return list(guest.records)

    def test_a_host_that_replays_says_its_value_at_the_first_anc2_and_at_each_activation(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v2-orientation"])
        host = runner.Host(guest.port)
        try:
            # Nothing to say yet, and a host that does not replay says nothing.
            host.connect()
            host.wait_frame(5)
            threading.Event().wait(0.3)
            self.assertEqual(guest.records, [])
            host.replay = True
            host.push(2)
            self.assertEqual(self.records_within(guest, 1), [frames["orientation-2"]])
            # A new connection: the first ANC2 of it is answered with the current value.
            host.connect()
            host.wait_frame(5)
            self.assertEqual(self.records_within(guest, 2), [frames["orientation-2"]] * 2)
            self.assertEqual(host.replays, 1)
            # The sensor idle, then active again: said again at the edge only.
            guest.say(frames["control-v2-idle"])
            guest.say(frames["control-v2-idle"])
            threading.Event().wait(0.3)
            self.assertEqual(len(guest.records), 2)
            host.current = 3
            guest.say(frames["control-v2-orientation"])
            self.assertEqual(self.records_within(guest, 3)[-1], frames["orientation-3"])
            guest.say(frames["control-v2-orientation"])
            threading.Event().wait(0.3)
            self.assertEqual(len(guest.records), 3)
        finally:
            host.stop()
            guest.close()

    def test_a_host_replays_at_the_first_anc3_that_lists_the_orientation_sensor(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-orientation-idle"])
        host = runner.Host(guest.port)
        try:
            host.replay = True
            host.current = 2
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Control(3, 16, 10_000, 10_000, 20_000))
            self.assertEqual(self.records_within(guest, 1), [frames["orientation-2"]])
            # Active, in pieces and after a 16-byte frame: said at the edge, and the frames are cut where their
            # magics say.
            guest.say(frames["control-v2-idle"])
            guest.say(frames["control-v3-orientation"][:7])
            threading.Event().wait(0.2)
            guest.say(frames["control-v3-orientation"][7:])
            self.assertEqual(self.records_within(guest, 2), [frames["orientation-2"]] * 2)
            self.assertEqual(host.replays, 2)
            self.assertEqual([frame.version for frame in host.frames], [3, 2, 3])
            self.assertEqual(host.bad, [])
        finally:
            host.stop()
            guest.close()

    def test_a_guest_with_only_the_magnetometer_is_never_answered_with_an_orientation_record(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v3-magnetometer"])
        host = runner.Host(guest.port)
        try:
            host.replay = True
            host.current = 1
            host.connect()
            self.assertEqual(host.wait_frame(5), runner.Control(3, 8, 10_000, 10_000, 50_000))
            threading.Event().wait(0.3)
            self.assertEqual(guest.records, [])
        finally:
            host.stop()
            guest.close()

    def test_the_compass_argument_is_the_runners_option_or_a_builders_extra_argument(self) -> None:
        import argparse
        self.assertTrue(runner.with_compass(argparse.Namespace(with_compass=True)))
        self.assertTrue(runner.with_compass(argparse.Namespace(extra_arguments=[runner.COMPASS_ARGUMENT])))
        self.assertFalse(runner.with_compass(argparse.Namespace()))

    def test_the_legacy_phase_refuses_the_compass_argument_before_booting_anything(self) -> None:
        parse = runner.build_parser().parse_args
        for arguments in (["--expect", "none", "--with-compass"],
                          ["--expect", "none", "--no-argument", "--with-compass"]):
            with self.subTest(arguments):
                self.assertIn("--with-compass needs --expect sensor", runner.unsupported_combination(parse(arguments)))
        for arguments in (["--with-compass"], ["--expect", "sensor", "--with-compass"], ["--expect", "none"],
                          ["--expect", "none", "--no-argument"]):
            with self.subTest(arguments):
                self.assertIsNone(runner.unsupported_combination(parse(arguments)))
        # main() refuses before it resolves adb or starts a guest: no executable is needed.
        with mock.patch.object(sys, "argv", ["run", "--expect", "none", "--with-compass"]), mock.patch.object(
                runner.boot_android, "resolve_executable", side_effect=AssertionError("started")), mock.patch(
                "sys.stderr", new_callable=io.StringIO) as stderr:
            self.assertEqual(runner.main(), 2)
        self.assertIn("needs --expect sensor", stderr.getvalue())
        self.assertEqual(runner.COMPASS_ARGUMENT, "androidboot.kernarium.compass=1")

    def test_an_old_guest_is_never_answered_with_an_orientation_record(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v1-motion"])
        host = runner.Host(guest.port)
        try:
            host.replay = True
            host.current = 1
            host.connect()
            host.wait_frame(5)
            threading.Event().wait(0.3)
            self.assertEqual(guest.records, [])
        finally:
            host.stop()
            guest.close()

    def test_a_sustained_posture_is_streamed_until_another_is_named(self) -> None:
        guest = FakeGuest(golden()["control-v1-motion"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            host.wait_frame(5)
            host.sustain(runner.COUNTER_CLOCKWISE)
            self.assertGreaterEqual(len(self.records_within(guest, 5)), 5)
            host.sustain(runner.CLOCKWISE)
            threading.Event().wait(0.3)
            with guest.lock:
                newest = guest.records[-1]
            self.assertEqual(newest, runner.accelerometer_record(*runner.POSTURES[runner.CLOCKWISE]))
            host.sustain(None)
            threading.Event().wait(0.2)
            with guest.lock:
                count = len(guest.records)
            threading.Event().wait(0.3)
            with guest.lock:
                self.assertEqual(len(guest.records), count)
        finally:
            host.stop()
            guest.close()

    def test_a_guest_that_is_slow_to_speak_is_waited_for_not_taken_for_gone(self) -> None:
        guest = FakeGuest(golden()["control-v2-idle"], delay=0.6)
        host = runner.Host(guest.port)
        try:
            with mock.patch.object(runner, "SEND_SECONDS", 0.2):
                host.connect()
                self.assertEqual(host.connection.gettimeout(), 0.2)
                self.assertEqual(host.wait_frame(5), runner.Control(2, 0, 10_000, 10_000))
                self.assertFalse(host.closed)
        finally:
            host.stop()
            guest.close()

    def test_a_wait_can_start_after_the_frames_a_step_began_with(self) -> None:
        frames = golden()
        guest = FakeGuest(frames["control-v2-orientation"])
        host = runner.Host(guest.port)
        try:
            host.connect()
            self.assertIsNotNone(host.wait_frame(5))
            began = len(host.frames)
            self.assertIsNone(host.wait_frame(0.3, after=began))
            guest.say(frames["control-v2-idle"])
            idle = host.wait_frame(5, lambda frame: not frame.orientation_active, after=began)
            self.assertEqual(idle, runner.Control(2, 0, 10_000, 10_000))
            # The earlier, active frame is not what an idle wait from `began` finds.
            self.assertIsNone(host.wait_frame(0.3, lambda frame: frame.orientation_active, after=began))
        finally:
            host.stop()
            guest.close()

    def test_a_host_with_no_guest_listening_gets_no_frame(self) -> None:
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        listener.listen(1)
        # The launcher's forward accepts and closes when the guest does not listen.
        threading.Thread(target=lambda: listener.accept()[0].close(), daemon=True).start()
        host = runner.Host(port)
        try:
            host.connect()
            self.assertIsNone(host.wait_frame(2))
        finally:
            host.close()
            listener.close()


class VirtualTime:
    """The runner's clock, so that a step that waits for a deadline runs in no time."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class ScriptedHost:
    """What a step needs of the host: the frames the guest has said, and a way to say a value."""

    def __init__(self, device: "ScriptedDevice") -> None:
        self.device = device
        self.frames: list[runner.Control] = []
        self.replay = False

    def push(self, value: int) -> None:
        self.device.value(value)

    def wait_frame(self, seconds: float, predicate=lambda frame: True, after: int = 0):
        return next((frame for frame in self.frames[after:] if predicate(frame)), None)


class ScriptedDevice(runner.Guest):
    """An Android guest as far as the steps ask it: a rotation that follows the newest value unless a finger is
    down, the screen is off or a lock screen is up, and the commands that change those."""

    def __init__(self) -> None:  # no adb, no process
        self.host = ScriptedHost(self)
        self.shown = 0
        self.latest = 0
        self.touching = False
        self.awake = True
        self.lock_enabled = False
        self.keyguard = False
        self.request = "SCREEN_ORIENTATION_UNSPECIFIED"
        # The faults a step must notice.
        self.turns_under_touch = False
        self.says_idle = True
        self.says_active = True
        self.lock_shows = True
        self.lock_request = "SCREEN_ORIENTATION_NOSENSOR"
        self.boot_completed_at = 0.0

    def value(self, value: int) -> None:
        self.latest = value
        if self.touching and not self.turns_under_touch:
            return
        self.settle()

    def settle(self) -> None:
        if self.awake and not self.keyguard:
            self.shown = self.latest

    def frame(self, active: bool) -> None:
        self.host.frames.append(runner.Control(2, 4 if active else 0, 10_000, 10_000))

    def alive(self) -> None:
        pass

    def settings_app(self) -> None:
        pass

    def rotation_state(self):
        request = self.lock_request if self.keyguard else self.request
        return runner.RotationState(
            rotation=0 if self.keyguard else self.shown, user_mode="USER_ROTATION_FREE", user_rotation=0,
            allow_all=False, landscape=1, seascape=3, app_orientation=request, listener_enabled=True,
            judge="OrientationSensorJudge", sensor=runner.SENSOR_NAME, proposed=None)

    def rotation(self) -> int:
        return self.rotation_state().rotation

    def shell(self, command: str, timeout: float = 0) -> str:
        if command == "wm size":
            return "Physical size: 720x1280\n"
        if "motionevent DOWN" in command:
            self.touching = True
        elif "motionevent UP" in command:
            self.touching = False
            self.settle()
        elif "KEYCODE_SLEEP" in command:
            self.awake = False
            if self.says_idle:
                self.frame(False)
        elif "KEYCODE_WAKEUP" in command:
            self.awake = True
            self.keyguard = self.lock_enabled and self.lock_shows
            if self.says_active:
                self.frame(True)
            self.settle()
        elif "get-disabled" in command:
            return "false\n" if self.lock_enabled else "true\n"
        elif "set-disabled" in command:
            # Only the setting; a lock screen already up stays up until it is dismissed.
            self.lock_enabled = command.endswith("false")
        elif "wm dismiss-keyguard" in command:
            self.keyguard = False
            self.settle()
        elif "mWakefulness" in command:
            return f"  mWakefulness={'Awake' if self.awake else 'Asleep'}\n"
        elif "window policy" in command:
            return f"      showing={'true' if self.keyguard else 'false'}\n"
        return ""


class StepTests(unittest.TestCase):
    """The steps that must be able to fail: each passes on a guest that behaves and fails on one that does not."""

    def setUp(self) -> None:
        self.time = VirtualTime()
        patcher = mock.patch.object(runner, "time", self.time)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_touch_held_waits_for_the_finger_and_a_turn_under_it_fails_the_step(self) -> None:
        device = ScriptedDevice()
        text = runner.touch_held(device, device.host.push)
        self.assertIn("turned it to 1 when the finger lifted", text)
        device = ScriptedDevice()
        device.turns_under_touch = True
        with self.assertRaisesRegex(runner.Failure, "turned .* s into the touch, before the finger lifted"):
            runner.touch_held(device, device.host.push)
        self.assertFalse(device.touching, "the finger is lifted whatever happened")

    def test_the_screen_off_step_needs_the_sensor_idle_while_off_and_in_use_again_on_waking(self) -> None:
        device = ScriptedDevice()
        text = runner.screen_off_on(device, device.host, device.host.push, True)
        self.assertIn("idle while off and in use again on waking", text)
        self.assertFalse(device.host.replay)
        silent = ScriptedDevice()
        silent.says_idle = False
        with self.assertRaisesRegex(runner.Failure, "never said the orientation sensor idle"):
            runner.screen_off_on(silent, silent.host, silent.host.push, True)
        deaf = ScriptedDevice()
        deaf.says_active = False
        with self.assertRaisesRegex(runner.Failure, "never said the orientation sensor in use again"):
            runner.screen_off_on(deaf, deaf.host, deaf.host.push, True)

    def test_without_an_orientation_sensor_the_accelerometer_bit_is_what_goes_idle(self) -> None:
        device = ScriptedDevice()
        # ANC1 frames: bit 0 is the accelerometer.
        device.frame = lambda active: device.host.frames.append(runner.Control(1, 1 if active else 0, 1, 1))
        text = runner.screen_off_on(device, device.host, device.host.push, False)
        self.assertIn("the accelerometer idle while off", text)

    def test_the_lock_screen_must_show_ask_not_to_follow_and_hand_the_value_on_at_unlock(self) -> None:
        device = ScriptedDevice()
        text = runner.lock_screen(device, device.host.push)
        self.assertIn("after unlocking the display turned to 3", text)
        self.assertFalse(device.lock_enabled, "the provisioned guest has no lock screen again")
        absent = ScriptedDevice()
        absent.lock_shows = False
        with self.assertRaisesRegex(runner.Failure, "the keyguard is not showing"):
            runner.lock_screen(absent, absent.host.push)
        self.assertFalse(absent.lock_enabled)

    def test_a_lock_screen_step_that_fails_leaves_no_lock_screen_over_the_display(self) -> None:
        device = ScriptedDevice()
        device.lock_request = "SCREEN_ORIENTATION_PORTRAIT"
        with self.assertRaisesRegex(runner.Failure, "not SCREEN_ORIENTATION_NOSENSOR"):
            runner.lock_screen(device, device.host.push)
        self.assertFalse(device.keyguard, "the lock screen is dismissed")
        self.assertTrue(device.awake)
        self.assertFalse(device.lock_enabled, "the setting is as the step found it")
        # The next step's first rotation wait is not stuck behind the lock screen.
        device.value(runner.COUNTER_CLOCKWISE)
        self.assertEqual(device.rotation(), runner.COUNTER_CLOCKWISE)

    def test_the_lock_setting_is_put_back_as_the_step_found_it(self) -> None:
        device = ScriptedDevice()
        device.lock_enabled = True  # an instance that has a lock screen of its own
        runner.lock_screen(device, device.host.push)
        self.assertTrue(device.lock_enabled)
        self.assertFalse(device.keyguard)
        other = ScriptedDevice()
        other.shell = lambda command, timeout=0: "gone\n" if "get-disabled" in command else ""
        with self.assertRaisesRegex(runner.Failure, "get-disabled' says 'gone'"):
            runner.lock_screen(other, other.host.push)

    def test_the_keyguard_is_read_from_the_window_policy_dump(self) -> None:
        self.assertTrue(runner.keyguard_showing("  KeyguardServiceDelegate\n      showing=true\n"))
        self.assertFalse(runner.keyguard_showing("      showing=false\n"))
        self.assertIsNone(runner.keyguard_showing("mKeyguardOccluded=false\n"))

    def test_the_setup_wizard_must_follow_the_sensor(self) -> None:
        device = ScriptedDevice()
        device.shown = 3
        device.latest = 3
        text = runner.wizard_rotation(device, device.host)
        self.assertIn("went to 3 with the early value and back to 0", text)
        # The component comes from the activity dump, not from the "Broken pipe" grep -m1 leaves after it.
        device.shown = device.latest = 3
        device.shell = lambda command, timeout=0: (
            "  topResumedActivity=ActivityRecord{1 u0 org.lineageos.setupwizard/.WelcomeActivity t9}\n"
            "Failed to write while dumping service activity: Broken pipe\n" if "topResumedActivity" in command else "")
        self.assertIn("org.lineageos.setupwizard/.WelcomeActivity (", runner.wizard_rotation(device, device.host))
        fixed = ScriptedDevice()
        fixed.request = "SCREEN_ORIENTATION_PORTRAIT"
        with self.assertRaisesRegex(runner.Failure, "not an orientation that follows the sensor"):
            runner.wizard_rotation(fixed, fixed.host)

    def test_the_early_value_must_come_before_the_sensor_is_in_use(self) -> None:
        class Early:
            sent_at = 5.0

            def __init__(self, active: bool) -> None:
                self.first = runner.Control(2, 4 if active else 0, 10_000, 10_000)

            def join(self) -> None:
                pass

        device = ScriptedDevice()
        device.boot_completed_at = 30.0
        self.assertIn("ahead of the window manager's registration", runner.describe_early(device, Early(False)))
        with self.assertRaisesRegex(runner.Failure, "already says the sensor is in use"):
            runner.describe_early(device, Early(True))


class ProbeApplicationTests(unittest.TestCase):
    def test_the_manifest_asks_for_each_orientation_from_a_framework_activity(self) -> None:
        tree = probe.decode_manifest(probe.encode_manifest())
        self.assertEqual(tree.tag, "manifest")
        self.assertEqual(tree.attributes["package"], probe.PACKAGE)
        sdk = next(child for child in tree.children if child.tag == "uses-sdk")
        self.assertEqual((sdk.attributes["minSdkVersion"], sdk.attributes["targetSdkVersion"]), (29, 29))
        application = next(child for child in tree.children if child.tag == "application")
        self.assertIs(application.attributes["hasCode"], False)
        found = {child.attributes["name"]: child.attributes["screenOrientation"]
                 for child in application.children if child.tag == "activity"}
        self.assertEqual(found, {probe.ACTIVITY_CLASSES[name]: value for name, value in probe.ORIENTATIONS.items()})
        self.assertEqual(sorted(probe.ORIENTATIONS.values()), [0, 1, 6, 10])
        for child in application.children:
            self.assertIs(child.attributes["exported"], True)
            action = child.children[0].children[0]
            self.assertEqual(action.attributes["name"], "android.intent.action.MAIN")
        self.assertEqual(probe.component("sensorLandscape"),
                         f"{probe.PACKAGE}/android.app.ExpandableListActivity")

    def test_the_attributes_are_in_the_order_of_their_resource_ids(self) -> None:
        # Android reads a binary manifest's attributes by resource id, in order.
        data = probe.encode_manifest()
        ids = list(probe.ATTRIBUTES.values())
        self.assertEqual(ids, sorted(ids))
        self.assertIn(struct.pack(f"<{len(ids)}I", *ids), data)

    @unittest.skipUnless(shutil.which("keytool") and shutil.which("jarsigner"), "needs a JDK")
    def test_the_package_is_signed_with_a_throwaway_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "probe.apk"
            probe.build_apk(apk, Path(directory))
            self.assertTrue(apk.is_file())
            result = subprocess.run(["jarsigner", "-verify", str(apk)], capture_output=True, text=True, check=False)
            self.assertIn("jar verified", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
