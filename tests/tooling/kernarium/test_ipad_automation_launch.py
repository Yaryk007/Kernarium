#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The UI automation host's `launch` keeps an iPhone silent for Andarium.

An automated run on the phone plays no haptics unless it tests them. A launch
of Andarium states `ANDARIUM_HAPTICS=0`; `--haptics` (the runners' name for a
run that tests haptics) states nothing, so the application's own Haptics
switch decides. Kernarium plays no haptics, and its launch is as it was. The
host's `activate` starts a stopped application with no environment, so a
stopped Andarium is not activated without `--haptics`.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = ["--device", "00000000-0000000000000000"]
ANDARIUM = ["--bundle", "com.imxieyi.andarium"]


def launch(*arguments: str, application: list[str] = ()) -> dict:
    return automation.build_command(
        automation.parse([*DEVICE, *application, "launch", *arguments]))


def refused_by_parser(*arguments: str) -> bool:
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            automation.parse([*DEVICE, *arguments])
    except SystemExit:
        return True
    return False


class LaunchHapticsTests(unittest.TestCase):
    def test_a_launch_of_andarium_silences_the_phone_by_default(self) -> None:
        self.assertEqual(
            launch(application=ANDARIUM)["environment"],
            {"ANDARIUM_HAPTICS": "0"})
        self.assertEqual(
            launch("--env", "ANDARIUM_START=normal", "--env", "A=b",
                   application=ANDARIUM)["environment"],
            {"ANDARIUM_START": "normal", "A": "b", "ANDARIUM_HAPTICS": "0"})

    def test_a_launch_that_tests_haptics_states_no_haptics_setting(
        self
    ) -> None:
        self.assertEqual(
            launch("--haptics", application=ANDARIUM)["environment"], {})
        self.assertEqual(
            launch("--haptics", "--env", "ANDARIUM_START=normal",
                   application=ANDARIUM)["environment"],
            {"ANDARIUM_START": "normal"})

    def test_a_launch_of_kernarium_is_as_it_was(self) -> None:
        # Kernarium has no vibrator and no haptics of its own to silence.
        self.assertEqual(launch()["environment"], {})
        self.assertEqual(
            launch("--env", "KERNARIUM_ROOTFS=none.erofs")["environment"],
            {"KERNARIUM_ROOTFS": "none.erofs"})
        self.assertEqual(launch("--haptics")["environment"], {})

    def test_the_haptics_setting_is_the_option_s_and_not_an_environment_pair(
        self
    ) -> None:
        for value in ("0", "1"):
            with self.subTest(value=value):
                self.assertTrue(refused_by_parser(
                    *ANDARIUM, "launch", "--env", f"ANDARIUM_HAPTICS={value}"))

    def test_only_a_launch_states_it(self) -> None:
        # The host's activate carries no environment (and a running host is
        # never rebuilt for one), so an activation cannot state it either:
        # ActivationTests has what it does instead.
        for name in ("terminate", "state", "tree", "ping"):
            with self.subTest(name=name):
                command = automation.build_command(
                    automation.parse([*DEVICE, *ANDARIUM, name]))
                self.assertNotIn("environment", command)


class ActivationTests(unittest.TestCase):
    """Activating an application that is not running starts it, with no
    environment, so a stopped Andarium is launched silent and not activated."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.state_dir = Path(scratch.name) / "state"

    def activate(self, *arguments: str, application=ANDARIUM,
                 state: int | None = 4, state_ok: bool = True):
        """Runs `activate` against a host that reports `state` for the
        application; returns the status, what was sent, which applications
        the report check named, and what was logged."""
        sent: list[str] = []
        checked: list[str] = []

        def send(options, command, timeout):
            sent.append(command["op"])
            if command["op"] == "state":
                return {"ok": state_ok, **({"state": state} if state_ok else {})}
            return {"ok": True}

        def preflight(options, applications, **keywords):
            checked.extend(keywords["extra_processes"])

        options = automation.parse(
            [*DEVICE, "--state-dir", str(self.state_dir), *application,
             "activate", *arguments])
        errors = io.StringIO()
        with mock.patch.object(automation, "send", send), \
                mock.patch.object(automation.ios_device_reports, "preflight", preflight), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(errors):
            status = automation.run_op(options)
        return status, sent, checked, errors.getvalue()

    def test_a_stopped_andarium_is_not_activated_and_the_refusal_names_launch(
        self
    ) -> None:
        # XCUIApplication.State: unknown 0, notRunning 1.
        for state in (0, 1):
            with self.subTest(state=state):
                status, sent, checked, errors = self.activate(state=state)
                self.assertEqual(status, automation.EXIT_FAILED)
                self.assertEqual(sent, ["state"])
                # The report check comes first, as before any device action.
                self.assertEqual(checked, ["Andarium"])
                self.assertIn("launch", errors)
                self.assertIn("--haptics", errors)

    def test_a_running_andarium_is_brought_to_the_front(self) -> None:
        # runningBackgroundSuspended 2, runningBackground 3, runningForeground 4.
        for state in (2, 3, 4):
            with self.subTest(state=state):
                status, sent, checked, _ = self.activate(state=state)
                self.assertEqual(status, 0)
                self.assertEqual(sent, ["state", "activate"])
                self.assertEqual(checked, ["Andarium"])

    def test_a_run_that_tests_haptics_may_start_it_by_activation(self) -> None:
        status, sent, checked, _ = self.activate("--haptics", state=1)
        self.assertEqual(status, 0)
        self.assertEqual(sent, ["activate"])
        self.assertEqual(checked, ["Andarium"])

    def test_another_application_is_activated_as_it_was(self) -> None:
        for application in ([], ["--bundle", "com.imxieyi.Kernarium"],
                            ["--bundle", "com.example.Orchid"]):
            with self.subTest(application=application):
                status, sent, _, _ = self.activate(
                    application=application, state=1)
                self.assertEqual(status, 0)
                self.assertEqual(sent, ["activate"])

    def test_a_reply_with_no_state_is_taken_as_not_running(self) -> None:
        status, sent, _, _ = self.activate(state=None)
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(sent, ["state"])

    def test_a_state_the_host_cannot_give_stops_the_activation(self) -> None:
        status, sent, checked, _ = self.activate(state_ok=False)
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(sent, ["state"])
        self.assertEqual(checked, ["Andarium"])


if __name__ == "__main__":
    unittest.main()
