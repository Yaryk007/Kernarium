#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The iPad automation host never lets Xcode record the device's screen.

A host runs for hours. With Xcode's defaults its test run recorded the
device's screen for the whole session into a result bundle under the internal
disk's DerivedData, and kept every attachment. The scheme that generates the
test run file turns both off, `start` refuses a run file that does not say so,
and the supervisor puts the result bundle under the state directory. Nothing
here talks to a device or starts xcodebuild.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ElementTree

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Kernarium/scripts/ipad-automation.py"
SCHEME = (REPOSITORY / "Kernarium/Kernarium.xcodeproj/xcshareddata"
          / "xcschemes/KernariumAutomation.xcscheme")
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = "00000000-0000000000000000"
NONCE = "0123456789abcdef0123456789abcdef"
GOOD = {
    "PreferredScreenCaptureFormat": "screenshots",
    "SystemAttachmentLifetime": "keepNever",
    "UserAttachmentLifetime": "keepNever",
}


def target(**settings: str | None) -> dict[str, str]:
    """A test target of a run file; a None setting is left out."""
    entry = {"TestBundlePath": "__TESTHOST__/PlugIns/Host.xctest",
             **GOOD, **settings}
    return {key: value for key, value in entry.items() if value is not None}


def destination_target(**settings: str | None) -> dict:
    """A target that runs the destination's installed bundle: no bundle path."""
    entry = {"UseDestinationArtifacts": True, "BlueprintName": "Host", **GOOD,
             **settings}
    return {key: value for key, value in entry.items() if value is not None}


def format_two(*targets: dict[str, str]) -> dict:
    return {"__xctestrun_metadata__": {"FormatVersion": 2},
            "TestConfigurations": [{"Name": "Test", "TestTargets": list(targets)}]}


def format_one(entry: dict[str, str]) -> dict:
    return {"__xctestrun_metadata__": {"FormatVersion": 1}, "Host": entry}


class FakeProcess:
    """What subprocess.Popen returns for a process that ended at once."""

    def __init__(self, calls: list, command: list[str], **keywords) -> None:
        self.pid = 4242
        calls.append((command, keywords))

    def poll(self) -> int:
        return 1

    def wait(self, timeout: float | None = None) -> int:
        return 0


class SchemeTests(unittest.TestCase):
    def test_the_scheme_that_generates_the_run_file_records_nothing(self) -> None:
        action = ElementTree.parse(SCHEME).getroot().find("TestAction")
        assert action is not None
        self.assertEqual(action.get("preferredScreenCaptureFormat"), "screenshots")
        self.assertEqual(action.get("systemAttachmentLifetime"), "keepNever")
        self.assertEqual(action.get("userAttachmentLifetime"), "keepNever")


class RecordingProblemTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)

    def problems(self, plan: dict) -> list[str]:
        path = Path(self.scratch.name) / "Host.xctestrun"
        path.write_bytes(plistlib.dumps(plan))
        return automation.recording_problems(path)

    def test_both_file_formats_with_the_settings_pass(self) -> None:
        self.assertEqual(self.problems(format_two(target())), [])
        self.assertEqual(self.problems(format_one(target())), [])

    def test_xcodes_defaults_are_refused_in_both_formats(self) -> None:
        defaults = target(PreferredScreenCaptureFormat="screenRecording",
                          SystemAttachmentLifetime="deleteOnSuccess",
                          UserAttachmentLifetime="deleteOnSuccess")
        for plan in (format_two(defaults), format_one(defaults)):
            problems = self.problems(plan)
            self.assertEqual(len(problems), 3, problems)
            self.assertIn("PreferredScreenCaptureFormat is 'screenRecording'",
                          " ".join(problems))

    def test_each_setting_is_required_on_its_own(self) -> None:
        for key, value in (
            ("PreferredScreenCaptureFormat", "screenRecording"),
            ("SystemAttachmentLifetime", "keepAlways"),
            ("UserAttachmentLifetime", "deleteOnSuccess"),
            ("PreferredScreenCaptureFormat", None),
            ("SystemAttachmentLifetime", None),
            ("UserAttachmentLifetime", None),
        ):
            with self.subTest(key=key, value=value):
                problems = self.problems(format_two(target(**{key: value})))
                self.assertEqual(len(problems), 1, problems)
                self.assertTrue(problems[0].startswith(key), problems)

    def test_every_target_must_pass(self) -> None:
        problems = self.problems(format_two(
            target(), target(SystemAttachmentLifetime="deleteOnSuccess")))
        self.assertEqual(len(problems), 1, problems)

    def test_a_target_that_uses_the_destinations_artifacts_is_refused(self) -> None:
        # One ordinary safe target does not excuse a recording one beside it.
        for plan in (
            format_two(target(), destination_target(
                PreferredScreenCaptureFormat="screenRecording")),
            format_two(target(), destination_target()),
            format_two(destination_target()),
            format_one(destination_target()),
        ):
            with self.subTest(plan=plan):
                problems = self.problems(plan)
                self.assertTrue(any("destination" in p for p in problems), problems)

    def test_a_target_whose_shape_is_not_enumerated_is_refused(self) -> None:
        for plan in (
            format_two(target(), {"SomethingElse": 1}),
            {"__xctestrun_metadata__": {"FormatVersion": 3},
             "TestConfigurations": [{"TestTargets": [target()]}]},
            {"__xctestrun_metadata__": {"FormatVersion": 2},
             "TestConfigurations": [{"Name": "Test"}]},
            {"__xctestrun_metadata__": {"FormatVersion": 2}},
            {"__xctestrun_metadata__": {"FormatVersion": 1}, "Host": "text"},
            ["not", "a", "dictionary"],
        ):
            with self.subTest(plan=plan):
                self.assertTrue(self.problems(plan))

    def test_a_target_that_uses_the_artifacts_beside_its_own_path_is_refused(self) -> None:
        self.assertTrue(self.problems(format_two(
            target(UseDestinationArtifacts=True))))

    def test_a_file_without_a_test_target_or_unreadable_is_refused(self) -> None:
        self.assertIn("names no test target", self.problems(
            {"__xctestrun_metadata__": {"FormatVersion": 1}})[0])
        self.assertTrue(self.problems({"a": [{"b": 1}]}))
        broken = Path(self.scratch.name) / "Broken.xctestrun"
        broken.write_text("not a property list")
        self.assertIn("cannot be read", automation.recording_problems(broken)[0])
        missing = Path(self.scratch.name) / "Missing.xctestrun"
        self.assertIn("cannot be read", automation.recording_problems(missing)[0])


class StartAndSuperviseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.state = Path(self.scratch.name) / "state"
        self.derived = Path(self.scratch.name) / "derived"
        (self.derived / "Build/Products").mkdir(parents=True)
        saved = automation.host_state
        automation.host_state = lambda options: None
        self.addCleanup(setattr, automation, "host_state", saved)

    def options(self, *command: str):
        return automation.parse(
            ["--device", DEVICE, "--state-dir", str(self.state),
             "--derived-data", str(self.derived), *command])

    def run_file(self, plan: dict) -> Path:
        path = self.derived / "Build/Products/KernariumAutomation_iphoneos27.0.xctestrun"
        path.write_bytes(plistlib.dumps(plan))
        return path

    def start(self, *extra: str) -> tuple[int, str, list]:
        calls: list = []
        errors = io.StringIO()
        real = automation.subprocess.Popen

        def popen(command, *positional, **keywords):
            # Only the supervisor is a stand-in; `ps` and the like are real.
            if "_supervise" in command:
                return FakeProcess(calls, command, **keywords)
            return real(command, *positional, **keywords)
        with mock.patch.object(
            automation.subprocess, "Popen", popen,
        ), mock.patch.object(
            automation.ios_device_reports, "preflight"
        ), contextlib.redirect_stderr(errors):
            status = automation.command_start(
                self.options("start", "--wait", "1", *extra))
        return status, errors.getvalue(), calls

    def test_start_refuses_a_run_file_that_records_the_screen(self) -> None:
        self.run_file(format_two(target(
            PreferredScreenCaptureFormat="screenRecording",
            SystemAttachmentLifetime="deleteOnSuccess",
            UserAttachmentLifetime="deleteOnSuccess")))
        status, errors, calls = self.start()
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [], "no supervisor and no xcodebuild may start")
        self.assertIn("screenRecording", errors)
        self.assertIn("run `build` again", errors)
        self.assertFalse((self.state / DEVICE / "host.json").exists())

    def test_start_refuses_a_run_file_that_keeps_attachments(self) -> None:
        self.run_file(format_one(target(UserAttachmentLifetime="deleteOnSuccess")))
        status, errors, calls = self.start()
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [])
        self.assertIn("UserAttachmentLifetime", errors)

    def test_start_judges_the_run_file_it_would_launch(self) -> None:
        older = self.derived / "Build/Products/KernariumAutomation_a.xctestrun"
        older.write_bytes(plistlib.dumps(format_two(target(
            PreferredScreenCaptureFormat="screenRecording"))))
        launched = self.derived / "Build/Products/KernariumAutomation_b.xctestrun"
        launched.write_bytes(plistlib.dumps(format_two(target())))
        _, _, calls = self.start()
        self.assertEqual(len(calls), 1)
        self.assertIn(str(launched), calls[0][0])

    def test_the_result_bundle_lives_next_to_the_host_log(self) -> None:
        self.run_file(format_two(target()))
        status, _, calls = self.start()
        self.assertEqual(status, automation.EXIT_NO_HOST)  # the fake ended at once
        self.assertEqual(len(calls), 1)
        command = calls[0][0]
        bundle = Path(command[command.index("--result-bundle") + 1])
        record = json.loads((self.state / DEVICE / "host.json").read_text())
        log = Path(record["log"])
        self.assertEqual(bundle.parent, self.state / DEVICE)
        self.assertEqual(bundle.parent, log.parent)
        self.assertEqual(bundle.suffix, ".xcresult")
        self.assertEqual(bundle.stem, log.stem)
        self.assertEqual(record["result_bundle"], str(bundle))
        self.assertFalse(bundle.exists(), "xcodebuild refuses an existing bundle")

    def test_start_makes_room_for_reports_before_it_starts_anything(self) -> None:
        self.run_file(format_two(target()))
        order: list[str] = []

        def popen(command, **keywords):
            order.append("supervisor")
            return FakeProcess([], command, **keywords)

        with mock.patch.object(automation.subprocess, "Popen", popen), \
                mock.patch.object(
                    automation.ios_device_reports, "preflight",
                    side_effect=lambda *a, **k: order.append("reports")
                ) as preflight, contextlib.redirect_stderr(io.StringIO()):
            automation.command_start(self.options("start", "--wait", "1"))
        self.assertEqual(order, ["reports", "supervisor"])
        options, applications = preflight.call_args.args
        self.assertEqual(options.device, DEVICE)
        self.assertIn("KernariumAutomation-Runner",
                      preflight.call_args.kwargs["extra_processes"])

    def test_start_refuses_when_the_reports_cannot_be_made_room_for(self) -> None:
        self.run_file(format_two(target()))
        calls: list = []
        errors = io.StringIO()
        real = automation.subprocess.Popen

        def popen(command, *positional, **keywords):
            # Only the supervisor is a stand-in; `ps` and the like are real.
            if "_supervise" in command:
                return FakeProcess(calls, command, **keywords)
            return real(command, *positional, **keywords)
        with mock.patch.object(
            automation.subprocess, "Popen", popen,
        ), mock.patch.object(
            automation.ios_device_reports, "preflight",
            side_effect=automation.ios_device_reports.DeviceError(
                "still short of 5 free slots"),
        ), contextlib.redirect_stderr(errors):
            status = automation.command_start(self.options("start", "--wait", "1"))
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [])
        self.assertIn("still short of 5 free slots", errors.getvalue())
        self.assertFalse((self.state / DEVICE / "host.json").exists())

    def test_the_supervisor_gives_xcodebuild_that_result_bundle(self) -> None:
        status, _, calls, _ = self.supervise(format_two(target()))
        self.assertEqual(status, 0)
        (command, _), = calls
        self.assertEqual(command[:2], ["xcodebuild", "test-without-building"])
        self.assertEqual(
            command[command.index("-resultBundlePath") + 1],
            str(self.state / "bundle.xcresult"))
        self.assertEqual(command.count("-resultBundlePath"), 1)

    def supervise(self, plan: dict | None, preflight=mock.DEFAULT):
        """`_supervise` as a caller that skipped `start` runs it; returns the
        status, the stderr, the commands started and the preflight mock."""
        path = self.derived / "Host.xctestrun"
        if plan is not None:
            path.write_bytes(plistlib.dumps(plan))
        calls: list = []
        errors = io.StringIO()
        options = self.options(
            "_supervise", "--xctestrun", str(path), "--result-bundle",
            str(self.state / "bundle.xcresult"), "--seconds", "5",
            "--nonce", NONCE)
        with mock.patch.object(
            automation.subprocess, "Popen",
            lambda command, **keywords: FakeProcess(calls, command, **keywords),
        ), mock.patch.object(
            automation.ios_device_reports, "preflight", preflight
        ) as check, contextlib.redirect_stderr(errors), \
                contextlib.redirect_stdout(io.StringIO()):
            status = automation.command_supervise(options)
        return status, errors.getvalue(), calls, check

    def test_a_direct_supervisor_refuses_a_run_file_that_records(self) -> None:
        for plan in (
            format_two(target(PreferredScreenCaptureFormat="screenRecording")),
            format_two(target(), destination_target()),
        ):
            with self.subTest(plan=plan):
                status, errors, calls, check = self.supervise(plan)
                self.assertEqual(status, automation.EXIT_FAILED)
                self.assertEqual(calls, [], "xcodebuild must not start")
                self.assertIn("does not turn Xcode's recording off", errors)
                check.assert_not_called()

    def test_a_direct_supervisor_makes_room_for_reports_first(self) -> None:
        order: list[str] = []
        status, _, calls, check = self.supervise(
            format_two(target()),
            preflight=mock.Mock(side_effect=lambda *a, **k: order.append("reports")))
        self.assertEqual(status, 0)
        self.assertEqual(len(calls), 1)
        self.assertEqual(order, ["reports"])
        (options, applications), keywords = check.call_args
        self.assertEqual(options.device, DEVICE)
        self.assertIn("KernariumAutomation-Runner", keywords["extra_processes"])

    def test_a_direct_supervisor_stops_when_the_reports_cannot_be_made_room_for(self) -> None:
        status, errors, calls, _ = self.supervise(
            format_two(target()),
            preflight=mock.Mock(side_effect=automation.ios_device_reports.DeviceError(
                "still short of 5 free slots")))
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [])
        self.assertIn("still short of 5 free slots", errors)

    def test_a_direct_supervisor_judges_the_file_it_is_given_now(self) -> None:
        # The file `start` inspected may have changed since.
        self.supervise(format_two(target()))
        status, _, calls, _ = self.supervise(
            format_two(target(PreferredScreenCaptureFormat="screenRecording")))
        self.assertEqual(status, automation.EXIT_FAILED)
        self.assertEqual(calls, [])

    def test_start_hands_its_report_options_to_the_supervisor(self) -> None:
        self.run_file(format_two(target()))
        _, _, calls = self.start(
            "--device-reports-dir", "/somewhere/else", "--leave-device-reports")
        command = calls[0][0]
        # Each collection gets a new directory of its own beneath the one named.
        destination = command[command.index("--device-reports-dir") + 1]
        self.assertTrue(destination.startswith("/somewhere/else/host-"), destination)
        self.assertIn("--leave-device-reports", command)
        self.run_file(format_two(target()))
        (self.state / DEVICE / "host.json").unlink()
        _, _, calls = self.start()
        self.assertNotIn("--device-reports-dir", calls[-1][0])
        self.assertNotIn("--leave-device-reports", calls[-1][0])

    def test_two_starts_in_one_second_never_share_a_result_bundle(self) -> None:
        self.run_file(format_two(target()))
        names = []
        with mock.patch.object(automation.time, "strftime",
                               lambda fmt: "20260101-000000"):
            for _ in range(2):
                _, _, calls = self.start()
                command = calls[0][0]
                nonce = command[command.index("--nonce") + 1]
                bundle = Path(command[command.index("--result-bundle") + 1])
                record = json.loads((self.state / DEVICE / "host.json").read_text())
                self.assertIn(nonce, bundle.name)
                self.assertIn(nonce, Path(record["log"]).name)
                names.append((bundle, record["log"]))
        self.assertNotEqual(names[0][0], names[1][0])
        self.assertNotEqual(names[0][1], names[1][1])

    def test_a_supervisor_needs_a_result_bundle(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit):
            self.options("_supervise", "--xctestrun", "Host.xctestrun",
                         "--seconds", "5", "--nonce", NONCE)


class StartingCommandTests(unittest.TestCase):
    """Every host operation that starts an application first makes room for
    the reports of the application it names."""

    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.state = Path(scratch.name) / "state"
        self.order: list[str] = []

    def run_command(self, *command: str, preflight=None):
        options = automation.parse(
            ["--device", DEVICE, "--state-dir", str(self.state), *command])

        def send(options, command, timeout):
            self.order.append("send")
            return {"ok": True, "tree": ""}

        def default(options, applications, **keywords):
            self.order.append("reports:" + ",".join(keywords["extra_processes"]))

        with mock.patch.object(automation, "send", send), mock.patch.object(
            automation.ios_device_reports, "preflight",
            preflight or default,
        ), contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            return automation.run_op(options)

    def test_launch_and_activate_check_the_selected_application_first(self) -> None:
        for command in ("launch", "activate"):
            for bundle, process in (
                (None, "Kernarium"), ("com.imxieyi.andarium", "Andarium"),
                ("com.imxieyi.Kernarium", "Kernarium"),
                ("com.example.Orchid", "Orchid"),
            ):
                with self.subTest(command=command, bundle=bundle):
                    self.order.clear()
                    arguments = (["--bundle", bundle] if bundle else [])
                    # A stopped Andarium is activated only by a run that tests
                    # haptics (test_ipad_automation_launch.py).
                    tests_haptics = (
                        ["--haptics"] if command == "activate"
                        and bundle == "com.imxieyi.andarium" else [])
                    self.assertEqual(
                        self.run_command(*arguments, command, *tests_haptics), 0)
                    self.assertEqual(self.order, [f"reports:{process}", "send"])

    def test_the_other_operations_need_no_report_check(self) -> None:
        for command in (["terminate"], ["state"], ["ping"], ["tree"],
                        ["tap", "--id", "x"], ["type", "text"]):
            with self.subTest(command=command):
                self.order.clear()
                self.run_command(*command)
                self.assertEqual(self.order, ["send"])

    def test_a_launch_is_refused_when_the_reports_cannot_be_made_room_for(self) -> None:
        def refuse(options, applications, **keywords):
            raise automation.ios_device_reports.DeviceError("still short")
        self.assertEqual(
            self.run_command("launch", preflight=refuse), automation.EXIT_FAILED)
        self.assertEqual(self.order, [], "nothing was sent to the host")

    def test_the_two_report_options_are_accepted_by_both(self) -> None:
        for command in ("launch", "activate"):
            options = automation.parse(
                ["--device", DEVICE, command, "--leave-device-reports",
                 "--device-reports-dir", "/somewhere"])
            self.assertTrue(options.leave_device_reports)
            self.assertEqual(options.device_reports_dir, Path("/somewhere"))


if __name__ == "__main__":
    unittest.main()
