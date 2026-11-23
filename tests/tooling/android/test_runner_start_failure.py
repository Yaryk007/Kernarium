#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Android runners' start_guest stops the emulator it launched when the start fails.

Each runner that boots its own guest creates the emulator in start_guest and
returns the Guest that its cleanup stops. A start that timed out waiting for
adb, for the launcher's forward or for boot_completed, or that was
interrupted, raised before it returned, so the caller had no guest to stop and
the emulator ran on. start_guest owns the emulator until it returns the guest.

Every row runs the runner's own start_guest (the thermal, keyboard and mouse,
gamepad, camera, Wi-Fi network and orientation runners; the compass and the
old-application runners start through the orientation runner's) against a
bounded stand-in for the emulator and an adb that does nothing, with the
waits made to time out, fail or be interrupted: the stand-in is gone each
time, adb is disconnected, and a guest that booted is returned running. The
video-suspend runner's, which the sync and vDSO runners share, is in
test_video_suspend_runner.py.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPOSITORY = Path(__file__).resolve().parents[3]
INTEGRATION = REPOSITORY / "tests/integration/android"

# What the stand-in launcher prints for each vsock forward it was asked for, as the macOS launcher does.
FORWARD_PORT = 34567
STAND_IN = (
    "import signal, sys, time\n"
    "signal.signal(signal.SIGINT, signal.SIG_DFL)\n"
    "for guest_port in sys.argv[1:]:\n"
    f"    print(f'vsock forward 127.0.0.1:{FORWARD_PORT} -> guest port {{guest_port}}', flush=True)\n"
    "time.sleep(20)\n")


def load(name: str) -> types.ModuleType:
    spec = importlib.util.spec_from_file_location(name, INTEGRATION / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    # Dataclasses resolve their annotations through sys.modules.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Family:
    """One runner's start_guest against the stand-in: the rows every runner has. A subclass names the runner."""

    # The guest ports whose vsock forwards the launcher reports, and which the runner reads before it waits for adb.
    ports: tuple[int, ...] = ()

    def launching(self) -> types.ModuleType:
        """The module that makes the launch, whose launcher helpers and subprocess the rows replace."""
        return self.module

    def options(self, adb_port: int) -> types.SimpleNamespace:
        return types.SimpleNamespace(
            instance=None, release=None, emulator=None, adb_port=adb_port, cpus=1, memory="1G", boot_timeout=5)

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        raise NotImplementedError

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="uml-rsf-test-"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.children: list[subprocess.Popen] = []
        self.disconnected: list[str] = []
        self.addCleanup(self.reap)
        # An adb that answers nothing, for the runner's own commands (the launch's `adb_run` calls).
        self.adb = self.directory / "adb"
        self.adb.write_text("#!/bin/sh\nexit 0\n")
        self.adb.chmod(self.adb.stat().st_mode | stat.S_IXUSR)
        self.adb_port = free_port()
        self.serial = f"127.0.0.1:{self.adb_port}"
        self.reported_ports = list(self.ports)
        owner = self.launching()
        real_popen = subprocess.Popen

        def standin(_command: object, **keywords: object) -> subprocess.Popen:
            # Bounded, and it ends on SIGINT as the emulator does.
            child = real_popen([sys.executable, "-c", STAND_IN, *map(str, self.reported_ports)], **keywords)
            self.children.append(child)
            return child

        boot = owner.boot_android
        patches = [
            mock.patch.object(boot, "parse_arguments", return_value=types.SimpleNamespace(
                emulator="unused", release="unused")),
            mock.patch.object(boot, "resolve_executable", return_value=Path("unused")),
            mock.patch.object(boot, "release_path", return_value=Path("unused")),
            mock.patch.object(boot.android_instance, "read_release", return_value={}),
            mock.patch.object(boot, "plan_start", return_value={}),
            mock.patch.object(boot.android_instance, "apply"),
            mock.patch.object(boot.android_instance, "booted"),
            mock.patch.object(boot, "data_capacity", return_value=0),
            mock.patch.object(boot, "emulator_command", side_effect=lambda *_a, **_k: ["unused"]),
            mock.patch.object(boot, "provision_guest"),
            mock.patch.object(boot, "disconnect_adb",
                              side_effect=lambda _adb, serial: self.disconnected.append(serial)),
            mock.patch.object(owner, "SETTLE_SECONDS", 0, create=True),
            # Longer than a loaded host takes to start the stand-in, and shorter than its own life.
            mock.patch.object(owner, "FORWARD_SECONDS", 15, create=True),
            # Only the runner's own launch is replaced: what the helpers run (adb) stays real.
            mock.patch.object(owner, "subprocess", types.SimpleNamespace(**{**vars(subprocess), "Popen": standin})),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def reap(self) -> None:
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=10)

    def run_start(self) -> object:
        private = self.directory / "guest"
        private.mkdir()
        return self.start(self.options(self.adb_port), str(self.adb), private)

    def assert_stopped(self) -> None:
        self.assertEqual(len(self.children), 1)
        self.assertIsNotNone(self.children[0].poll(), "the emulator was left running")
        self.assertEqual(self.disconnected, [self.serial])

    def test_an_adb_that_never_answers_stops_the_emulator(self) -> None:
        boot = self.launching().boot_android
        with mock.patch.object(boot, "wait_for_adb", side_effect=boot.LauncherError("injected startup timeout")):
            with self.assertRaisesRegex(boot.LauncherError, "injected startup timeout"):
                self.run_start()
        self.assert_stopped()

    def test_a_guest_that_does_not_finish_booting_stops_the_emulator(self) -> None:
        boot = self.launching().boot_android
        with mock.patch.object(boot, "wait_for_adb"), \
                mock.patch.object(boot, "wait_for_boot_completed", return_value=False):
            with self.assertRaisesRegex(self.launching().Failure, "sys.boot_completed"):
                self.run_start()
        self.assert_stopped()

    def test_an_interrupt_while_waiting_stops_the_emulator(self) -> None:
        boot = self.launching().boot_android
        with mock.patch.object(boot, "wait_for_adb", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.run_start()
        self.assert_stopped()

    def test_a_failure_after_boot_stops_the_emulator(self) -> None:
        boot = self.launching().boot_android
        with mock.patch.object(boot, "wait_for_adb"), \
                mock.patch.object(boot, "wait_for_boot_completed", return_value=True), \
                mock.patch.object(boot, "provision_guest", side_effect=OSError("injected")):
            with self.assertRaisesRegex(OSError, "injected"):
                self.run_start()
        self.assert_stopped()

    def test_a_guest_that_booted_is_returned_running_and_not_stopped(self) -> None:
        boot = self.launching().boot_android
        with mock.patch.object(boot, "wait_for_adb"), \
                mock.patch.object(boot, "wait_for_boot_completed", return_value=True):
            guest = self.run_start()
        self.addCleanup(self.finish, guest)
        self.assertEqual(len(self.children), 1)
        self.assertIsNone(self.children[0].poll())
        self.assertIs(guest.process, self.children[0])
        self.assertEqual(self.disconnected, [])

    def finish(self, guest: object) -> None:
        """What a run's own cleanup does with a returned guest."""

    def test_a_launcher_that_reports_no_forward_stops_the_emulator(self) -> None:
        if not self.ports:
            self.skipTest("this runner reads no vsock forward")
        self.reported_ports = []
        with mock.patch.object(self.launching(), "FORWARD_SECONDS", 1):
            with self.assertRaisesRegex(self.launching().Failure, "no vsock forward"):
                self.run_start()
        self.assert_stopped()


class ThermalStart(Family, unittest.TestCase):
    def setUp(self) -> None:
        self.module = load("run_android_thermal")
        self.ports = (self.module.THERMAL_PORT,)
        super().setUp()

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(options, adb, private)


class KeyMouseStart(Family, unittest.TestCase):
    def setUp(self) -> None:
        self.module = load("run_android_keymouse")
        super().setUp()

    def options(self, adb_port: int) -> types.SimpleNamespace:
        options = super().options(adb_port)
        options.vnc_port = free_port()
        return options

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(options, adb, private)


class GamepadStart(Family, unittest.TestCase):
    def setUp(self) -> None:
        self.module = load("run_android_gamepad")
        self.ports = (self.module.wire.PORT,)
        super().setUp()

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(options, adb, private)


class CameraStart(Family, unittest.TestCase):
    def setUp(self) -> None:
        self.module = load("run_android_camera")
        super().setUp()
        patch = mock.patch.object(self.module, "launcher_arguments", return_value=[])
        patch.start()
        self.addCleanup(patch.stop)

    def options(self, adb_port: int) -> types.SimpleNamespace:
        options = super().options(adb_port)
        options.layout = None
        options.calibration_dir = None
        options.audio_hold_library = None
        return options

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(options, types.SimpleNamespace(mode="none"), adb, private)


class WifiNetworkStart(Family, unittest.TestCase):
    def setUp(self) -> None:
        self.module = load("run_android_wifi_network")
        super().setUp()

    def options(self, adb_port: int) -> types.SimpleNamespace:
        options = super().options(adb_port)
        options.forward_port = free_port()
        return options

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(
            options, adb, private, None, private / "instance", provision=True, watched=False)

    def test_a_capture_that_fails_to_end_does_not_keep_the_emulator_running(self) -> None:
        # A guest of the release under test is watched: the stop that follows a failed start ends the log
        # capture first, and the emulator ends whatever that does.
        boot = self.module.boot_android
        capture = mock.Mock()
        capture.stop.side_effect = subprocess.TimeoutExpired("adb logcat", 10)
        private = self.directory / "guest"
        private.mkdir()
        with mock.patch.object(self.module, "LogCapture", return_value=capture), \
                mock.patch.object(boot, "wait_for_adb"), \
                mock.patch.object(boot, "wait_for_boot_completed", return_value=False):
            with self.assertRaises(subprocess.TimeoutExpired):
                self.module.start_guest(self.options(self.adb_port), str(self.adb), private, None,
                                        private / "instance", provision=True, watched=True)
        capture.start_boot.assert_called_once()
        capture.stop.assert_called_once()
        self.assert_stopped()


class OrientationStart(Family, unittest.TestCase):
    def setUp(self) -> None:
        self.module = load("run_android_orientation")
        self.ports = (self.module.MOTION_PORT,)
        self.holder: dict[str, object] = {}
        super().setUp()

    def options(self, adb_port: int) -> types.SimpleNamespace:
        options = super().options(adb_port)
        options.no_argument = False
        return options

    def early(self) -> int | None:
        return None

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(
            options, adb, private, early=self.early(), holder=self.holder, provision=True)

    def finish(self, guest: object) -> None:
        early = self.holder.get("early")
        if early is not None:
            early.stop.set()
            early.thread.join(timeout=10)
        if "host" in self.holder:
            self.holder["host"].stop()


class OrientationEarlyHostStart(OrientationStart):
    """The phase that says a value before the sensor is registered: the host that talks to the guest from its
    first frame is made before the boot is waited for, and ends with a start that failed."""

    def early(self) -> int | None:
        return 3

    def assert_stopped(self) -> None:
        super().assert_stopped()
        if not self.reported_ports:
            # No forward, so no host yet: the start failed before it made one.
            self.assertEqual(self.holder, {})
            return
        early = self.holder["early"]
        early.thread.join(timeout=10)
        self.assertFalse(early.thread.is_alive(), "the early host outlived the guest it was to talk to")
        self.assertTrue(early.stop.is_set())
        self.assertIsNone(self.holder["host"].connection)


class CompassStart(Family, unittest.TestCase):
    """The compass runner (and the two old-application runners, which build their options the same way) start
    through the orientation runner's start_guest, from their own copy of it."""

    def setUp(self) -> None:
        self.module = load("run_android_compass")
        self.ports = (self.module.orient.MOTION_PORT,)
        super().setUp()

    def launching(self) -> types.ModuleType:
        return self.module.orient

    def options(self, adb_port: int) -> types.SimpleNamespace:
        options = super().options(adb_port)
        options.expect = "sensor"
        options.with_argument = False
        options.with_orientation = True
        return options

    def start(self, options: types.SimpleNamespace, adb: str, private: Path) -> object:
        return self.module.start_guest(options, adb, private)


# The mixin's rows run only under the classes above.
del Family

if __name__ == "__main__":
    unittest.main()
