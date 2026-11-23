#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""`ipad-automation.py watch` as a process: signals, exit statuses, launchd.

The same `watch` runs in a terminal, detached by the script, or as a job
manager's job, and must behave the same in each: it ends on SIGTERM and leaves
a serving host running, `stop --force` ends it through a real signal before
the host, and the job description the script prints is all a job manager needs
(no login shell, and no `KeepAlive`: a watch that ended is not brought back by
launchd, whatever the reason, and a watch that failed for approval must stay
ended). Each test runs the real script as a subprocess against
`automation_fakes.FakeDevice`, files that stand in for the device and Xcode.
The watch runs with its real bounds here; the logic tests scale them.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import automation_shards  # noqa: E402
from automation_fakes import FakeDevice  # noqa: E402

SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
LABEL = "org.example.automation-watch"


class ProcessTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.device = FakeDevice()
        self.addCleanup(self.device.cleanup)
        self.children: list[subprocess.Popen] = []
        self.addCleanup(self.reap)

    def reap(self) -> None:
        """Ends the watches this test started, then everything the fake
        device ran (device.cleanup, which runs after this)."""
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=30)
        pid = self.watcher()
        if pid:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def base(self) -> list[str]:
        return [sys.executable, str(SCRIPT), "--device", self.device.udid,
                "--state-dir", str(self.device.state_dir),
                "--derived-data", str(self.device.derived)]

    def reports(self) -> list[str]:
        return ["--device-reports-dir", str(self.device.reports),
                "--leave-device-reports"]

    def run_script(self, *arguments: str, timeout: float = 120):
        return subprocess.run(
            [*self.base(), *arguments], env=self.device.environment,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=timeout)

    def spawn(self, *arguments: str, environment=None) -> subprocess.Popen:
        output = tempfile.TemporaryFile()
        self.addCleanup(output.close)
        child = subprocess.Popen(
            [*self.base(), *arguments], env=environment or self.device.environment,
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        child.output = output  # type: ignore[attr-defined]
        self.children.append(child)
        return child

    @staticmethod
    def text(child: subprocess.Popen) -> str:
        child.output.seek(0)  # type: ignore[attr-defined]
        return child.output.read().decode(errors="replace")  # type: ignore[attr-defined]

    def record(self) -> dict:
        try:
            return json.loads((self.device.state_dir / self.device.udid
                               / "watch.json").read_text())
        except (OSError, ValueError):
            return {}

    def watcher(self) -> int | None:
        """The pid the lock file names while a process holds the lock."""
        import fcntl
        path = self.device.state_dir / self.device.udid / "watch.lock"
        try:
            fd = os.open(path, os.O_RDONLY)
        except OSError:
            return None
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            except BlockingIOError:
                try:
                    return int(json.loads(os.pread(fd, 512, 0).decode())["pid"])
                except (ValueError, KeyError, TypeError):
                    return 0
            return None
        finally:
            os.close(fd)

    def wait_state(self, state: str, seconds: float = 90) -> dict:
        return self.device.wait(
            lambda: self.record() if self.record().get("state") == state else None,
            seconds, f"the watch record says {state}")

    def status(self) -> tuple[int, dict]:
        done = self.run_script("status")
        return done.returncode, json.loads(done.stdout)


class WatchProcessTests(ProcessTestCase):
    def test_detach_runs_the_watch_and_stop_force_ends_it_before_the_host(self) -> None:
        # Rules: `watch --detach` runs the same watch in a session of its own;
        # one watch per device; `start` and a plain `stop` are refused while it
        # runs; `stop --force` ends the watch (a real SIGTERM) before the host,
        # so that the watch does not start the host again.
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        done = self.run_script("watch", "--detach", "--hours", "3", "--wait", "30",
                               *self.reports())
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("watching under pid", done.stderr)
        self.wait_state("watching")
        code, status = self.status()
        self.assertEqual(code, 0)
        self.assertTrue(status["watch"]["running"])
        pid = status["watch"]["pid"]
        self.assertEqual(status["watch"]["state"], "watching")
        self.assertEqual(status["watch"]["restarts"], 0)
        # A session of its own: not this process's group.
        self.assertNotEqual(os.getpgid(pid), os.getpgrp())
        again = self.run_script("watch", "--hours", "3", *self.reports())
        self.assertEqual(again.returncode, 6, again.stderr)
        detached = self.run_script("watch", "--detach", *self.reports())
        self.assertEqual(detached.returncode, 6, detached.stderr)
        self.assertEqual(self.run_script("start", *self.reports()).returncode, 1)
        self.assertEqual(self.run_script("stop").returncode, 1)
        self.assertEqual(len(self.device.invocations()), 1)
        self.assertTrue(self.device.serving())
        stopped = self.run_script("stop", "--force", timeout=180)
        self.assertEqual(stopped.returncode, 0, stopped.stderr)
        self.assertIn("ending the watch", stopped.stderr)
        self.assertIsNone(self.watcher())
        self.device.wait(lambda: not self.device.alive(pid), 30, "the watch is gone")
        self.assertEqual(self.record()["state"], "stopped")
        self.assertFalse(self.device.serving())
        host = self.device.invocations()[0]
        self.device.wait(lambda: not self.device.alive(host["pid"]), 30,
                         "the host ended")
        # The watch ended first and the host after it: nothing starts it again.
        self.assertEqual(len(self.device.invocations()), 1)

    def test_sigterm_ends_the_watch_and_leaves_a_serving_host(self) -> None:
        # Rule: ending the watch alone (SIGTERM, a job manager's stop) never
        # ends the host it was watching.
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        nonce = self.device.invocations()[0]["nonce"]
        child = self.spawn("watch", "--hours", "3", *self.reports())
        record = self.wait_state("watching")
        self.assertEqual(record["host"], nonce)
        child.send_signal(signal.SIGTERM)
        self.assertEqual(child.wait(timeout=30), 0, self.text(child))
        self.assertEqual(self.record()["state"], "stopped")
        self.assertIn("a serving host is left as it is", self.text(child))
        self.assertTrue(self.device.serving(nonce))
        supervisor = self.device.invocations()[0]["ppid"]
        self.assertTrue(self.device.alive(supervisor))
        self.assertEqual(len(self.device.invocations()), 1)

    def test_approval_ends_the_watch_with_its_own_status_and_it_stays_ended(self) -> None:
        # Rule: when the device asks for approval the watch fails once, says so
        # in `status`, exits 4, and a watch started again (by a job manager, a
        # loop) starts nothing until a `start` served.
        self.device.scenarios([{"kind": "serve"}, {"kind": "approval"}])
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        child = self.spawn("watch", "--hours", "3", *self.reports())
        record = self.wait_state("watching")
        self.device.control(record["host"], "die")
        self.assertEqual(child.wait(timeout=120), 4, self.text(child))
        self.assertIn("needs approval at the device", self.text(child))
        self.assertEqual(len(self.device.invocations()), 2)
        # (The dead host's last beat still counts as serving for 20 s.)
        _, status = self.status()
        self.assertEqual(status["watch"]["state"], "needs-approval")
        self.assertEqual(status["watch"]["meaning"], "needs approval at the device")
        self.assertFalse(status["watch"]["running"])
        self.assertIn("since", status["watch"])
        self.assertIn("observed", status["watch"]["approval"])
        # `status` says why: the cause and the device's own words.
        self.assertIn("the approval prompt was not answered in time",
                      status["watch"]["detail"])
        self.assertIn("Timed out while enabling automation mode",
                      status["watch"]["detail"])
        again = self.run_script("watch", "--hours", "3", *self.reports())
        self.assertEqual(again.returncode, 4, again.stderr)
        self.assertIn("starting nothing", again.stderr)
        self.assertEqual(len(self.device.invocations()), 2)
        # The refusal is a file of its own: a watch killed at once, before it
        # has read anything, leaves it (R2).
        latch = self.device.state_dir / self.device.udid / "approval.json"
        self.assertTrue(latch.exists())
        killed = self.spawn("watch", "--hours", "3", *self.reports())
        self.device.wait(lambda: self.watcher(), 30, "the watch holds its lock")
        killed.kill()
        killed.wait(timeout=30)
        self.assertTrue(latch.exists())
        third = self.run_script("watch", "--hours", "3", *self.reports())
        self.assertEqual(third.returncode, 4, third.stderr)
        self.assertEqual(len(self.device.invocations()), 2)

    def test_sigterm_to_the_supervisor_ends_an_xcodebuild_that_ignores_it(self) -> None:
        # Rule (R5): the supervisor stays responsible until its children are
        # gone. Only SIGTERM is sent, to the group, as ending a host does; the
        # xcodebuild ignores it, and the supervisor ends it with SIGKILL before
        # it exits itself. The supervisor's wait is shortened with its own
        # hidden option, which `start` sets from CHILD_TERM_WAIT.
        self.device.scenarios([{"kind": "serve", "stubborn": True, "child": True}])
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        host = self.device.invocations()[0]
        self.assertTrue(self.device.alive(host["pid"]))
        self.assertTrue(self.device.alive(host["child"]))
        os.killpg(host["ppid"], signal.SIGTERM)
        self.device.wait(lambda: not any(self.device.alive(pid) for pid in (
            host["pid"], host["ppid"], host["child"])), 60,
            "the whole host ended, helper included")
        log = (self.device.state_dir / self.device.udid).glob("host-*.log")
        self.assertIn("xcodebuild exited with status", next(iter(log)).read_text())

    def test_a_failed_start_has_a_status_of_its_own(self) -> None:
        # A host that served and died, and a replacement whose xcodebuild ends
        # before any runner comes up: one try, its own status, and no latch.
        self.device.scenarios([{"kind": "serve"}, {"kind": "exit", "status": 70}])
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        child = self.spawn("watch", "--hours", "3", *self.reports())
        record = self.wait_state("watching")
        self.device.control(record["host"], "die")
        self.assertEqual(child.wait(timeout=150), 5, self.text(child))
        self.assertEqual(len(self.device.invocations()), 2)
        self.assertEqual(self.record()["state"], "start-failed")
        self.assertIn("could not start a host", self.text(child))
        self.assertFalse((self.device.state_dir / self.device.udid
                          / "approval.json").exists())


class KeepAwakeProcessTests(ProcessTestCase):
    def test_the_watch_holds_an_idle_sleep_assertion_until_it_ends(self) -> None:
        # Rule: a watch process holds `caffeinate -i -w <its pid>` (idle sleep
        # only, never the display) and releases it when it ends: on SIGTERM
        # itself, and on SIGKILL by `-w`, which the real tool honours (the
        # fake holds until the process it is told to wait for is gone).
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        for kind in (signal.SIGTERM, signal.SIGKILL):
            with self.subTest(kind.name):
                before = len(self.device.caffeinated())
                child = self.spawn("watch", "--hours", "3", *self.reports())
                self.wait_state("watching")
                calls = self.device.caffeinated()
                self.assertEqual(len(calls), before + 1, calls)
                self.assertEqual(calls[-1]["argv"], ["-i", "-w", str(child.pid)])
                self.assertEqual(self.watcher(), child.pid)
                self.assertTrue(self.device.alive(calls[-1]["pid"]))
                child.send_signal(kind)
                child.wait(timeout=30)
                self.device.wait(lambda: not self.device.alive(calls[-1]["pid"]), 30,
                                 f"the assertion ends with the watch ({kind.name})")

    def test_a_refused_watch_holds_none(self) -> None:
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        self.spawn("watch", "--hours", "3", *self.reports())
        self.wait_state("watching")
        again = self.run_script("watch", "--hours", "3", *self.reports())
        self.assertEqual(again.returncode, 6, again.stderr)
        self.assertEqual(len(self.device.caffeinated()), 1)
        # Printing the job holds nothing either.
        self.run_script("watch", "--print-launchd-job", LABEL)
        self.assertEqual(len(self.device.caffeinated()), 1)


class LostConnectionProcessTests(ProcessTestCase):
    def test_a_lost_connection_is_a_plain_death_the_watch_replaces(self) -> None:
        # Rule: xcodebuild reports the connection lost; the supervisor ends it
        # before its relaunch of the runner (6 s later in the fake), and the
        # watch process replaces the host and says why in its record.
        self.device.scenarios([{"kind": "serve"}, {"kind": "serve"}])
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        child = self.spawn("watch", "--hours", "3", *self.reports())
        first = self.wait_state("watching")["host"]
        self.device.control(first, "connection-lost")
        record = self.device.wait(
            lambda: self.record() if self.record().get("restarts") == 1
            and self.record().get("state") == "watching" else None, 90,
            "the watch replaced the host")
        self.assertNotEqual(record["host"], first)
        self.assertIn("the device connection was lost at ",
                      record["last_end"]["reason"])
        self.assertIn("started no runner again", self.text(child))
        self.assertFalse((self.device.state_dir / self.device.udid
                          / "approval.json").exists())
        code, status = self.status()
        self.assertEqual(code, 0)
        self.assertTrue(status["serving"])
        self.assertIsNone(child.poll())


class LaunchdJobTests(ProcessTestCase):
    def job(self, *extra: str) -> dict:
        done = self.run_script(
            "watch", "--hours", "5", "--wait", "100", "--max-restarts", "3",
            *self.reports(), *extra, "--print-launchd-job", LABEL)
        self.assertEqual(done.returncode, 0, done.stderr)
        return plistlib.loads(done.stdout.encode())

    def test_the_job_description_stands_on_its_own(self) -> None:
        # Rule: a job manager's job has no login shell, so the interpreter, the
        # script, the state directory, the derived data and a PATH with the
        # device tools are all in the description; printing it creates nothing.
        job = self.job()
        self.assertFalse(self.device.state_dir.exists())
        self.assertEqual(job["Label"], LABEL)
        arguments = job["ProgramArguments"]
        self.assertEqual(arguments[:2], [sys.executable, str(SCRIPT)])
        self.assertEqual(arguments[arguments.index("--device") + 1], self.device.udid)
        self.assertEqual(arguments[arguments.index("--state-dir") + 1],
                         str(self.device.state_dir))
        self.assertEqual(arguments[arguments.index("--derived-data") + 1],
                         str(self.device.derived))
        watch = arguments[arguments.index("watch"):]
        self.assertEqual(watch[:7], ["watch", "--hours", "5", "--wait", "100",
                                     "--max-restarts", "3"])
        self.assertIn("--leave-device-reports", watch)
        self.assertEqual(watch[watch.index("--device-reports-dir") + 1],
                         str(self.device.reports))
        for word in ("--detach", "--print-launchd-job", LABEL):
            self.assertNotIn(word, arguments)
        path = job["EnvironmentVariables"]["PATH"].split(":")
        self.assertIn(str(self.device.bin), path)
        self.assertIn("/usr/bin", path)
        self.assertTrue(all(entry.startswith("/") for entry in path))
        self.assertEqual(job["StandardOutPath"], job["StandardErrorPath"])
        self.assertTrue(job["StandardOutPath"].startswith(str(self.device.state_dir)))
        self.assertNotIn("LaunchAgents", json.dumps(job))

    def test_every_path_in_the_job_is_absolute(self) -> None:
        # Rule (R10): a relative path would resolve against launchd's working
        # directory, not the one the job was printed in: the report directory,
        # the state and derived-data directories, the PATH entries of the
        # device tools and DEVELOPER_DIR are all made absolute.
        with tempfile.TemporaryDirectory() as scratch:
            work = Path(scratch).resolve() / "work"
            (work / "rel-bin").mkdir(parents=True)
            for name in ("xcrun", "xcodebuild"):
                tool = work / "rel-bin" / name
                tool.write_text("#!/bin/sh\nexit 0\n")
                tool.chmod(0o755)
            environment = dict(os.environ, PATH="rel-bin:/usr/bin:/bin",
                               DEVELOPER_DIR="rel-developer")
            done = subprocess.run(
                [sys.executable, str(SCRIPT), "--device", self.device.udid,
                 "--state-dir", "rel-state", "--derived-data", "rel-derived",
                 "watch", "--device-reports-dir", "../rel-reports",
                 "--print-launchd-job", LABEL],
                cwd=work, env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            job = plistlib.loads(done.stdout.encode())
            arguments = job["ProgramArguments"]
            for option in ("--state-dir", "--derived-data", "--device-reports-dir"):
                value = arguments[arguments.index(option) + 1]
                self.assertTrue(value.startswith("/"), (option, value))
            self.assertEqual(
                os.path.normpath(arguments[arguments.index("--device-reports-dir") + 1]),
                str(work.parent / "rel-reports"))
            path = job["EnvironmentVariables"]["PATH"].split(":")
            self.assertIn(str(work / "rel-bin"), path)
            self.assertTrue(all(entry.startswith("/") for entry in path), path)
            self.assertTrue(job["EnvironmentVariables"]["DEVELOPER_DIR"].startswith("/"))
            self.assertTrue(job["StandardOutPath"].startswith("/"))
            self.assertFalse((work / "rel-state").exists())

    def test_the_job_logs_to_the_state_directory_unless_told_otherwise(self) -> None:
        # Rule: the default log is STATE_DIR/UDID/watch-launchd.log, for both
        # streams.
        job = self.job()
        expected = str(self.device.state_dir.resolve() / self.device.udid
                       / "watch-launchd.log")
        self.assertEqual(job["StandardOutPath"], expected)
        self.assertEqual(job["StandardErrorPath"], expected)

    def test_the_log_option_moves_both_streams_and_nothing_else(self) -> None:
        # Rule: `--launchd-log` names the file both streams go to, wherever the
        # state directory is; the watch the job runs is the same one, and
        # printing creates nothing.
        log_file = self.device.root / "job logs" / "watch.log"
        with_option = self.job("--launchd-log", str(log_file))
        self.assertEqual(with_option["StandardOutPath"], str(log_file))
        self.assertEqual(with_option["StandardErrorPath"], str(log_file))
        self.assertFalse(log_file.parent.exists())
        self.assertFalse(self.device.state_dir.exists())
        default = self.job()
        for key in ("ProgramArguments", "EnvironmentVariables", "RunAtLoad",
                    "AbandonProcessGroup", "LimitLoadToSessionType", "Label"):
            self.assertEqual(with_option[key], default[key], key)
        self.assertNotIn("--launchd-log", with_option["ProgramArguments"])
        self.assertNotIn(str(log_file), with_option["ProgramArguments"])

    def test_the_directory_hint_follows_the_log_option(self) -> None:
        # Rule: the printed `mkdir -p` names the directory of the log that is
        # used, quoted for the shell.
        log_file = self.device.root / "job logs" / "watch.log"
        done = self.run_script("watch", "--launchd-log", str(log_file),
                              "--print-launchd-job", LABEL)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(shlex.split(done.stderr.split("`")[1]),
                         ["mkdir", "-p", str(log_file.parent)])
        self.assertNotIn(str(self.device.state_dir), done.stderr)

    def test_a_relative_log_path_is_made_absolute(self) -> None:
        # Rule (R10): launchd's working directory is not the one the job was
        # printed in, so a relative log path is made absolute against it, and
        # printing creates nothing.
        with tempfile.TemporaryDirectory() as scratch:
            work = Path(scratch).resolve() / "work"
            work.mkdir()
            done = subprocess.run(
                [sys.executable, str(SCRIPT), "--device", self.device.udid,
                 "--state-dir", str(self.device.state_dir), "watch",
                 "--launchd-log", "../rel-logs/watch.log",
                 "--print-launchd-job", LABEL],
                cwd=work, env=self.device.environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            job = plistlib.loads(done.stdout.encode())
            expected = str(work.parent / "rel-logs" / "watch.log")
            self.assertEqual(job["StandardOutPath"], expected)
            self.assertEqual(job["StandardErrorPath"], expected)
            self.assertEqual(shlex.split(done.stderr.split("`")[1]),
                             ["mkdir", "-p", str(work.parent / "rel-logs")])
            self.assertFalse((work.parent / "rel-logs").exists())

    def test_the_log_option_without_a_job_to_print_is_refused(self) -> None:
        # Rule: the option belongs to the printed job; a watch that would
        # silently ignore it is refused.
        done = self.run_script("watch", "--launchd-log", str(self.device.root / "x.log"),
                              timeout=30)
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("--launchd-log goes with --print-launchd-job", done.stderr)

    def test_the_usage_says_where_the_job_file_and_its_log_belong(self) -> None:
        # Rule: launchd must be able to open the job file and the log itself.
        helped = " ".join(subprocess.run(
            [sys.executable, str(SCRIPT), "--device", "X", "watch", "--help"],
            stdout=subprocess.PIPE, text=True, timeout=60).stdout.split())
        self.assertIn("launchd must be able to open the job file and the log itself, "
                      "so both belong on a volume with ownership enabled", helped)
        header = " ".join(subprocess.run(
            [sys.executable, str(SCRIPT), "--help"],
            stdout=subprocess.PIPE, text=True, timeout=60).stdout.split())
        self.assertIn("launchd must be able to open the job file and the log itself, "
                      "so both belong on a volume with ownership enabled", header)
        self.assertIn("--launchd-log", header)

    def test_printing_the_job_creates_nothing_and_names_the_directory_to_make(self) -> None:
        # Rule (E): launchd will not make the directory the job logs to, and
        # printing makes none: the usage text names the one `mkdir`.
        done = self.run_script("watch", "--print-launchd-job", LABEL)
        self.assertEqual(done.returncode, 0, done.stderr)
        job = plistlib.loads(done.stdout.encode())
        directory = Path(job["StandardOutPath"]).parent
        self.assertEqual(directory, self.device.state_dir / self.device.udid)
        self.assertFalse(self.device.state_dir.exists())
        self.assertIn(f"mkdir -p {directory}", done.stderr)
        # A state directory with a space in its name: the printed command makes
        # that directory, not two.
        spaced = subprocess.run(
            [sys.executable, str(SCRIPT), "--device", self.device.udid,
             "--state-dir", str(self.device.root / "state with spaces"),
             "watch", "--print-launchd-job", LABEL],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=60)
        self.assertEqual(spaced.returncode, 0, spaced.stderr)
        command = spaced.stderr.split("`")[1]
        self.assertEqual(shlex.split(command),
                         ["mkdir", "-p", str(self.device.root / "state with spaces"
                                             / self.device.udid)])
        helped = subprocess.run(
            [sys.executable, str(SCRIPT), "--device", "X", "watch", "--help"],
            stdout=subprocess.PIPE, text=True, timeout=60).stdout
        self.assertIn("mkdir -p", helped.replace("\n", " ").replace("  ", " "))

    def test_the_job_asks_for_interactive_scheduling_and_nothing_else_of_the_system(self) -> None:
        # Rule: launchd.plist(5): ProcessType Interactive lifts the light
        # resource limits a job gets by default (throttled CPU and I/O), and
        # LegacyTimers keeps the job's timers out of the coalescing. A watch of
        # the default kind was once not run for over an hour while the Mac
        # idled with its display off. Nothing else is asked: no Nice, no
        # priority, no power assertion (the watch holds its own).
        job = self.job()
        self.assertEqual(job["ProcessType"], "Interactive")
        self.assertIs(job["LegacyTimers"], True)
        for key in ("Nice", "LowPriorityIO", "LowPriorityBackgroundIO",
                    "HardResourceLimits", "SoftResourceLimits", "KeepAlive",
                    "StartInterval", "StartCalendarInterval", "ThrottleInterval"):
            self.assertNotIn(key, job)

    def test_the_usage_names_what_the_job_and_the_watch_hold(self) -> None:
        header = " ".join(subprocess.run(
            [sys.executable, str(SCRIPT), "--help"],
            stdout=subprocess.PIPE, text=True, timeout=60).stdout.split())
        self.assertIn("interactive scheduling (ProcessType Interactive, "
                      "LegacyTimers)", header)
        self.assertIn("idle-sleep assertion (`caffeinate -i -w PID`: never the "
                      "display)", header)
        self.assertIn("whenever a turn of its loop starts more than 15 s after "
                      "the one before it", header)

    def test_the_job_is_not_brought_back_after_the_watch_ends(self) -> None:
        # Rule (E): no KeepAlive. launchd starts the watch once and never
        # again, so that no end the watch reports (approval, giving up, not
        # being able to tell) can be undone by a restart that puts another
        # prompt on the device. A watch that crashes stays gone, and `status`
        # shows it; the usage text says so.
        helped = subprocess.run(
            [sys.executable, str(SCRIPT), "--device", "X", "watch", "--help"],
            stdout=subprocess.PIPE, text=True, timeout=60).stdout
        self.assertIn("never restarts the job", " ".join(helped.split()))
        job = self.job()
        self.assertTrue(job["RunAtLoad"])
        self.assertNotIn("KeepAlive", job)
        self.assertNotIn("ThrottleInterval", job)
        # What the watch started stays up when the watch ends.
        self.assertTrue(job["AbandonProcessGroup"])

    def test_the_job_file_is_a_valid_property_list(self) -> None:
        plutil = shutil.which("plutil")
        if plutil is None:
            self.skipTest("plutil is a macOS tool")
        done = self.run_script("watch", "--print-launchd-job", LABEL)
        with tempfile.NamedTemporaryFile(suffix=".plist") as handle:
            handle.write(done.stdout.encode())
            handle.flush()
            checked = subprocess.run([plutil, "-lint", handle.name],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout)

    def test_the_job_runs_without_the_shell_environment(self) -> None:
        # Rule: the description is enough. Run exactly its arguments with
        # exactly its environment (and the fake device's own variable): the
        # watch adopts the serving host and ends on SIGTERM.
        started = self.run_script("start", "--hours", "3", "--wait", "60",
                                  *self.reports())
        self.assertEqual(started.returncode, 0, started.stderr)
        job = self.job()
        environment = dict(job["EnvironmentVariables"], FAKE_ROOT=str(self.device.root),
                           HOME=os.environ.get("HOME", "/"))
        # The job names the fake tools' directory because that is where the
        # tools were found when it was printed; nothing else comes from here.
        self.assertNotIn("PYTHONPATH", environment)
        output = tempfile.TemporaryFile()
        self.addCleanup(output.close)
        child = subprocess.Popen(job["ProgramArguments"], env=environment,
                                 stdin=subprocess.DEVNULL, stdout=output,
                                 stderr=subprocess.STDOUT)
        self.children.append(child)
        record = self.wait_state("watching")
        self.assertEqual(record["host"], self.device.invocations()[0]["nonce"])
        child.send_signal(signal.SIGTERM)
        self.assertEqual(child.wait(timeout=30), 0)
        self.assertEqual(len(self.device.invocations()), 1)


if __name__ == "__main__":
    automation_shards.main()
