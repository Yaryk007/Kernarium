#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The video-suspend runner's suspend step, without a guest.

run_android_video_suspend.py suspends the guest by the host's route and by
Android's own, with a clip playing and the screen recorded. The launcher's
log is marked before each suspend and the mark stays pending through the
checks that follow the wake (tests/integration/support/gpu_suspend_log.py):
the host's own reset line fails the step as a host GPU reset, any other GPU
error as GPU errors whose cause is not established, whenever the line arrives
(the first graphics work after the wake prints a refusal) and whichever way
the suspend or the check ends.

The runner's start_guest, which the other runners that share it use as well,
owns the emulator it launched until it returns the guest: a failed or
interrupted start stops that emulator rather than leaving it to the runner's
cleanup, which only knows a guest once start_guest has returned one.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "tests/integration/android/run_android_video_suspend.py"
SPEC = importlib.util.spec_from_file_location("run_android_video_suspend", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
sys.path.insert(0, str(REPOSITORY / "tests/integration/support"))
import gpu_suspend_log  # noqa: E402

KERNEL_GPU_ERROR = "[  186.505954][   T11] [drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1204 (command 0x202)\n"
KERNEL_OUT_OF_MEMORY = "[  186.505954][   T11] [drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1201 (command 0x204)\n"
HOST_GPU_REFUSAL = "Virtio-GPU submit rejected unknown context: id=3 bytes=44\n"
HOST_RESET_LINE = gpu_suspend_log.RESET_LINE + " ring 1 held a deferred request\n"
RESUMED = "[  186.1][  T1] PM: suspend exit\n"


class Clock:
    """The runner's clock: a sleep moves it on at once."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class FakeGuest:
    """The parts of the runner's Guest that a suspend step and the checks around it touch."""

    def __init__(self, directory: str, silent: bool = False) -> None:
        self.log = Path(directory) / "emulator.log"
        self.log.write_text("boot\n")
        self.silent = silent
        self.commands: list[str] = []
        self.late = ""
        self.picture_moves = True

    def write(self, text: str) -> None:
        with self.log.open("a") as stream:
            stream.write(text)

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if self.silent and command == "echo guest-answers":
            raise runner.Failure(f"'{command}' exited 255: closed")
        return ""

    def adb_run(self, *arguments: str, timeout: float = 0) -> subprocess.CompletedProcess:
        self.commands.append(" ".join(arguments))
        return subprocess.CompletedProcess(list(arguments), 0, stdout=b"")

    def closes(self) -> list[tuple[bool, str, str]]:
        return []

    def moving(self) -> bool:
        # The first graphics work after the wake: its refusal reaches the log while the check waits for the picture.
        if self.late:
            self.write(self.late)
            self.late = ""
        return self.picture_moves

    def threads_in_d(self) -> list[str]:
        return []

    def alive(self) -> None:
        pass


class SuspendTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = directory.name
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        clock = mock.patch.object(runner, "time", Clock())
        clock.start()
        self.addCleanup(clock.stop)

    def routes(self, guest: FakeGuest, text: str = "", error: Exception | None = None):
        """Both routes replaced by one that writes `text` to the launcher's log, then fails with `error` if given."""
        def route(_: object) -> None:
            guest.write(text)
            if error is not None:
                raise error
        return mock.patch.dict(runner.ROUTES, {"host": route, "guest": route})

    # The two labels.
    def test_a_wake_with_no_gpu_line_in_the_log_passes_and_the_guest_answers(self) -> None:
        guest = FakeGuest(self.directory)
        with self.routes(guest, RESUMED + "Virtio-GPU vhost-user transport\n"):
            for route in ("host", "guest"):
                runner.suspend(guest, route)
                runner.pending_of(guest).settle()
        self.assertEqual(guest.commands, ["echo guest-answers"] * 2)

    def test_the_hosts_reset_line_fails_the_step_as_a_host_gpu_reset(self) -> None:
        for route in ("host", "guest"):
            with self.subTest(route=route):
                guest = FakeGuest(self.directory)
                with self.routes(guest, RESUMED + KERNEL_GPU_ERROR + HOST_RESET_LINE), \
                        self.assertRaises(runner.Failure) as raised:
                    runner.suspend(guest, route)
                self.assertEqual(str(raised.exception),
                                 f"after the {route} suspend: {gpu_suspend_log.RESET_LABEL}: {HOST_RESET_LINE.strip()}")

    def test_gpu_errors_without_the_reset_line_fail_the_step_as_gpu_errors_of_unknown_cause(self) -> None:
        for text in (KERNEL_OUT_OF_MEMORY, KERNEL_GPU_ERROR, "Virtio-GPU: context create failed\n" + HOST_GPU_REFUSAL):
            with self.subTest(text=text.strip()[-50:]):
                guest = FakeGuest(self.directory)
                with self.routes(guest, RESUMED + text), self.assertRaises(runner.Failure) as raised:
                    runner.suspend(guest, "host")
                message = str(raised.exception)
                self.assertTrue(message.startswith(f"after the host suspend: {gpu_suspend_log.ERRORS_LABEL}: "), message)
                self.assertIn("the cause is not established", message)
                self.assertNotIn(gpu_suspend_log.RESET_LABEL, message)

    def test_lines_from_before_the_suspend_are_not_in_its_interval(self) -> None:
        guest = FakeGuest(self.directory)
        guest.write(KERNEL_GPU_ERROR + HOST_RESET_LINE)
        with self.routes(guest, RESUMED):
            runner.suspend(guest, "host")

    # Output that arrives after the step has returned.
    def test_the_mark_stays_pending_after_the_step_returns(self) -> None:
        guest = FakeGuest(self.directory)
        with self.routes(guest, RESUMED):
            runner.suspend(guest, "host")
        self.assertIsNotNone(runner.pending_of(guest).mark)

    def test_a_refusal_that_arrives_while_the_picture_is_checked_fails_the_player_check(self) -> None:
        # The reviewer's counterexample: the wake and the echo succeed, then moving() makes the first graphics work.
        for route in ("host", "guest"):
            with self.subTest(route=route):
                guest = FakeGuest(self.directory)
                guest.late, guest.picture_moves = HOST_RESET_LINE, False
                with mock.patch.object(runner, "play"), self.routes(guest, RESUMED), \
                        self.assertRaises(runner.Failure) as raised:
                    runner.check_player(guest, 1, route)
                message = str(raised.exception)
                self.assertTrue(message.startswith(f"{gpu_suspend_log.RESET_LABEL}: {HOST_RESET_LINE.strip()}"), message)
                self.assertIn("the check had failed with: the picture moves after the wake: the picture is still", message)

    def test_a_late_refusal_fails_a_check_that_otherwise_passed(self) -> None:
        guest = FakeGuest(self.directory)
        guest.late = HOST_GPU_REFUSAL

        @runner.settles
        def check(guest: FakeGuest) -> None:
            runner.suspend(guest, "host")
            guest.moving()

        with self.routes(guest, RESUMED), self.assertRaisesRegex(runner.Failure, gpu_suspend_log.ERRORS_LABEL):
            check(guest)
        self.assertIsNone(runner.pending_of(guest).mark)

    def test_a_check_that_settles_clean_ends_the_mark(self) -> None:
        guest = FakeGuest(self.directory)

        @runner.settles
        def check(guest: FakeGuest) -> None:
            runner.suspend(guest, "host")

        with self.routes(guest, RESUMED):
            check(guest)
        self.assertIsNone(runner.pending_of(guest).mark)

    def test_a_diagnosis_is_reported_once(self) -> None:
        guest = FakeGuest(self.directory)
        guest.late = HOST_RESET_LINE

        @runner.settles
        def check(guest: FakeGuest) -> None:
            runner.suspend(guest, "host")
            guest.moving()

        with self.routes(guest, RESUMED):
            with self.assertRaises(runner.Failure):
                check(guest)
            check(guest)

    def test_a_line_that_arrived_after_the_step_is_not_lost_to_the_next_suspend(self) -> None:
        guest = FakeGuest(self.directory)
        with self.routes(guest, RESUMED):
            runner.suspend(guest, "host")
            guest.write(HOST_RESET_LINE)
            with self.assertRaisesRegex(runner.Failure, f"before the host suspend: {gpu_suspend_log.RESET_LABEL}"):
                runner.suspend(guest, "host")
        self.assertEqual(guest.commands, ["echo guest-answers"])

    def test_the_player_and_the_recorder_suspend_through_the_step(self) -> None:
        guest = FakeGuest(self.directory)
        with mock.patch.object(runner, "play"), self.routes(guest, HOST_RESET_LINE):
            with self.assertRaisesRegex(runner.Failure, f"after the host suspend: {gpu_suspend_log.RESET_LABEL}"):
                runner.check_player(guest, 1, "host")
            with self.assertRaisesRegex(runner.Failure, f"after the guest suspend: {gpu_suspend_log.RESET_LABEL}"):
                runner.check_recording(guest, "guest")

    def test_no_check_calls_a_route_without_the_step(self) -> None:
        self.assertEqual(SCRIPT.read_text().count("ROUTES[route](guest)"), 1)

    # A failure of the whole operation.
    def test_a_route_that_fails_after_the_reset_line_is_replaced_by_the_diagnosis(self) -> None:
        # The reviewer's counterexample: the guest route's adb wait fails after the wake, the reset line is in the log.
        for route, error in (("guest", runner.boot_android.LauncherError("ADB did not become ready")),
                             ("host", runner.Failure("host suspend: wake replied 'busy'"))):
            with self.subTest(route=route):
                guest = FakeGuest(self.directory)
                with self.routes(guest, HOST_RESET_LINE, error), self.assertRaises(runner.Failure) as raised:
                    runner.suspend(guest, route)
                message = str(raised.exception)
                self.assertTrue(message.startswith(f"after the {route} suspend: {gpu_suspend_log.RESET_LABEL}: "), message)
                self.assertIn(f"(the step had failed with: {error})", message)
                self.assertIs(raised.exception.__cause__, error)

    def test_a_route_that_fails_with_no_gpu_line_fails_as_it_did(self) -> None:
        for error in (runner.boot_android.LauncherError("ADB did not become ready"),
                      runner.Failure("host suspend: wake replied 'busy'")):
            with self.subTest(error=type(error).__name__):
                guest = FakeGuest(self.directory)
                with self.routes(guest, RESUMED, error), self.assertRaises(type(error)) as raised:
                    runner.suspend(guest, "host")
                self.assertIs(raised.exception, error)
                self.assertEqual(guest.commands, [])

    def test_the_named_defect_comes_before_the_silence_it_causes(self) -> None:
        guest = FakeGuest(self.directory, silent=True)
        with self.routes(guest, HOST_RESET_LINE), self.assertRaisesRegex(runner.Failure, gpu_suspend_log.RESET_LABEL):
            runner.suspend(guest, "host")

    def test_a_guest_that_does_not_answer_after_the_wake_fails_without_a_gpu_line(self) -> None:
        guest = FakeGuest(self.directory, silent=True)
        with self.routes(guest, RESUMED), self.assertRaisesRegex(runner.Failure, "echo guest-answers' exited 255"):
            runner.suspend(guest, "host")


class StartGuestTests(unittest.TestCase):
    """start_guest against a bounded stand-in for the emulator: no guest boots and no adb is run."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="uml-avs-test-"))
        self.addCleanup(shutil.rmtree, self.directory, ignore_errors=True)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.children: list[subprocess.Popen] = []
        self.disconnected: list[str] = []
        self.addCleanup(self.reap)
        self.options = types.SimpleNamespace(
            instance=None, release=None, emulator=None, adb_port=5599, cpus=1, memory="1G", boot_timeout=5)
        real_popen = subprocess.Popen

        def standin(_command: object, **keywords: object) -> subprocess.Popen:
            # Bounded, and it ends on SIGINT as the emulator does.
            child = real_popen([sys.executable, "-c",
                                "import signal, time; signal.signal(signal.SIGINT, signal.SIG_DFL); time.sleep(20)"],
                               **keywords)
            self.children.append(child)
            return child

        boot = runner.boot_android
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
            mock.patch.object(boot, "emulator_command", return_value=["unused"]),
            mock.patch.object(boot, "provision_guest"),
            mock.patch.object(boot, "disconnect_adb", side_effect=lambda _adb, serial: self.disconnected.append(serial)),
            mock.patch.object(runner, "SETTLE_SECONDS", 0),
            # Only the runner's own launch is replaced: what the helpers run (adb, getconf) stays real.
            mock.patch.object(runner, "subprocess", types.SimpleNamespace(**{**vars(subprocess), "Popen": standin})),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def reap(self) -> None:
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=10)

    def start(self) -> object:
        return runner.start_guest(self.options, "adb", self.directory)

    def assert_stopped(self) -> None:
        self.assertEqual(len(self.children), 1)
        self.assertIsNotNone(self.children[0].poll(), "the emulator was left running")
        self.assertEqual(self.disconnected, ["127.0.0.1:5599"])

    def test_an_adb_that_never_answers_stops_the_emulator(self) -> None:
        with mock.patch.object(runner.boot_android, "wait_for_adb",
                               side_effect=runner.boot_android.LauncherError("injected startup timeout")):
            with self.assertRaisesRegex(runner.boot_android.LauncherError, "injected startup timeout"):
                self.start()
        self.assert_stopped()

    def test_a_guest_that_does_not_finish_booting_stops_the_emulator(self) -> None:
        with mock.patch.object(runner.boot_android, "wait_for_adb"), \
                mock.patch.object(runner.boot_android, "wait_for_boot_completed", return_value=False):
            with self.assertRaisesRegex(runner.Failure, "sys.boot_completed"):
                self.start()
        self.assert_stopped()

    def test_an_interrupt_while_waiting_stops_the_emulator(self) -> None:
        with mock.patch.object(runner.boot_android, "wait_for_adb", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.start()
        self.assert_stopped()

    def test_a_failure_after_boot_stops_the_emulator(self) -> None:
        with mock.patch.object(runner.boot_android, "wait_for_adb"), \
                mock.patch.object(runner.boot_android, "wait_for_boot_completed", return_value=True), \
                mock.patch.object(runner.boot_android, "provision_guest", side_effect=OSError("injected")):
            with self.assertRaisesRegex(OSError, "injected"):
                self.start()
        self.assert_stopped()

    def test_a_guest_that_booted_is_returned_running_and_not_stopped(self) -> None:
        with mock.patch.object(runner.boot_android, "wait_for_adb"), \
                mock.patch.object(runner.boot_android, "wait_for_boot_completed", return_value=True):
            guest = self.start()
        self.assertEqual(len(self.children), 1)
        self.assertIsNone(self.children[0].poll())
        self.assertIs(guest.process, self.children[0])
        self.assertEqual(self.disconnected, [])


class SharedReaderTests(unittest.TestCase):
    def test_the_log_is_read_from_the_mark_and_a_missing_log_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "emulator.log"
            log.write_text("before\n")
            size = gpu_suspend_log.log_size(log)
            log.write_text("before\n" + KERNEL_GPU_ERROR)
            self.assertEqual(gpu_suspend_log.log_since(log, size), KERNEL_GPU_ERROR)
            self.assertEqual(gpu_suspend_log.log_size(Path(directory) / "gone"), 0)

    def test_a_long_line_is_cut(self) -> None:
        line = HOST_RESET_LINE.strip() + " " + "x" * 400
        self.assertEqual(len(gpu_suspend_log.diagnosis(line)),
                         len(gpu_suspend_log.RESET_LABEL) + 2 + 300)

    def test_an_error_of_another_driver_or_another_level_is_not_a_gpu_error(self) -> None:
        for text in ("[drm] virtio_gpu_dequeue_ctrl_func: ok\n", "[drm:other_func] *ERROR* response 0x1204\n", "\n"):
            self.assertIsNone(gpu_suspend_log.diagnosis(text), text)

    def test_pending_marks_are_scanned_before_they_are_replaced_and_reported_once(self) -> None:
        text: list[str] = [""]
        pending = gpu_suspend_log.Pending(lambda: len(text[0]), lambda offset: text[0][offset:])
        self.assertIsNone(pending.begin())
        text[0] += RESUMED
        self.assertIsNone(pending.peek())
        text[0] += HOST_RESET_LINE
        self.assertIn(gpu_suspend_log.RESET_LABEL, pending.begin())
        self.assertIsNone(pending.settle())
        self.assertIsNone(pending.peek())


if __name__ == "__main__":
    unittest.main()
