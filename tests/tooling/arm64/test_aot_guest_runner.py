#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AOT guest runner against scripted guests.

tests/integration/aot/run_aot_guest.py compares an Android guest with the
ahead-of-time component off and on. Here a fake launcher and a fake adb stand
in for the guest, and each row scripts what the guest printed and which exit
statuses it returned. The runner must pass a complete, successful, identical
pair of arms and fail every incomplete or failed one.
"""

__import__("sys").dont_write_bytecode = True

import collections
import contextlib
import hashlib
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap
import types
import unittest
import unittest.mock

RUNNER_DIRECTORY = (pathlib.Path(__file__).resolve().parents[2] /
                    "integration" / "aot")
sys.path.insert(0, str(RUNNER_DIRECTORY))
sys.path.insert(0, str(RUNNER_DIRECTORY.parent / "support"))
import run_aot_guest as runner  # noqa: E402
import share_rows  # noqa: E402

# A run of a debug mode is its records; these rows only count runs.
ONE_RECORD = [runner.debug_rows.Record("done", (("mode", "x"),))]

FAKE_LAUNCHER = """\
import os, pathlib, signal, sys, time
state = pathlib.Path(os.environ["FAKE_GUEST_STATE"])
arm = {"1": "on", "0": "off"}[os.environ["DARWIN_UML_AOT"]]
(state / "arm").write_text(arm)
scenario = json.loads((state / "scenario.json").read_text())
report = os.environ.get("DARWIN_UML_AOT_REPORT")
template = scenario.get("report", {}).get(arm)
clock = scenario.get("audit_clock")
begun = time.monotonic()
started_at = time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)
def write_report():
    # What the diagnostics runtime writes: a whole report each time, in which
    # the audits begin and finish as the clock says (one every tenth of a
    # second; with a stop_at, none finishes after that many seconds).
    # The stamp is when the counters were read; with frozen_stamp the report
    # is one sampled before the guest's work and published late.
    stamp = started_at if (clock or {}).get("frozen_stamp") else time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)
    text = template.replace("{report_at}", str(stamp))
    if clock is not None:
        elapsed = time.monotonic() - begun
        started = 1 + int(elapsed / 0.1)
        finished = started - 1
        if clock.get("stop_at") is not None:
            finished = min(finished, int(clock["stop_at"] / 0.1))
        text = (text.replace("{started}", str(started)).replace("{last_exact}", str(finished))
                .replace("{runs}", str(finished)))
    pathlib.Path(report + ".part").write_text(text)
    os.replace(report + ".part", report)
if report and template is not None:
    write_report()
# The emulator: a process of that name below the launcher.
import subprocess
link = state / "uml-darwin-android17"
if not link.exists():
    link.symlink_to("/bin/sleep")
child = subprocess.Popen([str(link), "120"])
def stop(*_):
    child.kill()
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
deadline = time.monotonic() + 120
while time.monotonic() < deadline:
    time.sleep(0.05)
    if clock is not None and report and template is not None:
        write_report()
child.kill()
"""

FAKE_VMMAP = """\
import json, os, pathlib, sys
state = pathlib.Path(os.environ["FAKE_GUEST_STATE"])
scenario = json.loads((state / "scenario.json").read_text())
arm = (state / "arm").read_text() if (state / "arm").exists() else "off"
sys.stdout.write(scenario["vmmap"][arm])
sys.exit(scenario.get("vmmap_status", 0))
"""

def vmmap_text(resident: str, framework: str = "UmlArm64Aot") -> str:
    return ("REGION TYPE                    START - END         [ VSIZE  RSDNT"
            "  DIRTY   SWAP] PRT/MAX SHRMOD PURGE    REGION DETAIL\n"
            "__TEXT                      114450000-123808000    [243.7M"
            f"  {resident}     0K     0K] r-x/rwx SM=COW          "
            f"/x/bin/Frameworks/{framework}.framework/{framework}\n"
            "__LINKEDIT                  123808000-123884000    [  496K"
            "    16K     0K     0K] r--/rwx SM=COW          "
            f"/x/bin/Frameworks/{framework}.framework/{framework}\n")


FAKE_ADB = """\
import json, os, pathlib, sys
state = pathlib.Path(os.environ["FAKE_GUEST_STATE"])
scenario = json.loads((state / "scenario.json").read_text())
arm = (state / "arm").read_text() if (state / "arm").exists() else "off"
arguments = sys.argv[1:]
if arguments[:1] == ["-s"]:
    arguments = arguments[2:]
verb = arguments[0] if arguments else ""
if verb == "get-state":
    print("device")
elif verb == "shell" and arguments[1].startswith("getprop"):
    print("1")
elif verb == "shell" and "LD_PRELOAD" in arguments[1]:
    counter = state / ("probe-" + arm)
    number = int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(number + 1))
    texts = scenario["probe"][arm]
    sys.stdout.write(texts[min(number, len(texts) - 1)])
    sys.exit(scenario.get("probe_status", 0))
elif verb == "shell":
    sys.stdout.write(scenario["workloads"][arm])
    sys.exit(scenario.get("workloads_status", 0))
elif verb == "push":
    sys.exit(scenario.get("push_status", 0))
sys.exit(0)
"""

WRAPPER = """\
import sys
sys.path.insert(0, {runner_directory!r})
sys.path.insert(0, {support_directory!r})
import run_aot_guest as runner
runner.LAUNCHER = {launcher!r}
runner.SETTLE_SECONDS = 0
runner.REPORT_SECONDS = 0
runner.AUDIT_WAIT_SECONDS = 2
runner.REPORT_WAIT_SECONDS = 3
runner.REPORT_POLL_SECONDS = 0.05
sys.exit(runner.main())
"""


def digest(name: str) -> str:
    return hashlib.md5(name.encode()).hexdigest()


def good_workloads() -> list[str]:
    return [f"{name} {number} 0.25 {digest(name)} 0"
            for name in runner.WORKLOAD_NAMES
            for number in runner.REPETITIONS]


def workload_text(rows: list[str], finished: bool = True) -> str:
    return "\n".join(rows + ([runner.WORKLOADS_FINISHED] if finished else [])) + "\n"


def good_probe() -> str:
    lines = []
    for case in runner.PROBE_CASES:
        lines.append(f"case {case}")
        lines.append("  fault signo=11 code=2 addr=R+0x1000 pc=libz.so+0x40 "
                     "sp=S+0x10 pstate=0x0")
        lines.append("  regs x00=0x1 x01=0x2 x02=R+0x20")
    lines += ["done faults=%d" % len(runner.PROBE_CASES), "rc=0"]
    return "\n".join(lines) + "\n"


def audit_line(runs: str = "{runs}", records: int = 40, mismatches: int = 0, anomalies: int = 0,
               last_mismatches: int = 0, skipped: int = 1, nanos: str = "nanos_total 9000000 nanos_max 3200000",
               sequence: str = "started {started} last_exact {last_exact}") -> str:
    """A report's count-audit line; `runs`, `started` and `last_exact` follow the
    scripted guest's clock unless a row pins them."""
    return (f"count_audit runs {runs} skipped {skipped} records {records} mismatches {mismatches} "
            f"last_mismatches {last_mismatches} anomalies {anomalies} {nanos} {sequence}\n")


REPORT_STAMP = "report at {report_at}\n"
GOOD_AUDIT = audit_line()
GOOD_STACK = ("stack_free 59200 stack_size 65536 stack_kind 0 "
              "stack_task_bytes 65536 stack_engine_free 62000 stack_growth 2800\n")


def good_scenario() -> dict:
    text = workload_text(good_workloads())
    return {"workloads": {"off": text, "on": text},
            "audit_clock": {"stop_at": None},
            "probe": {"off": [good_probe()], "on": [good_probe()]},
            "report": {"on": REPORT_STAMP + "binds 3 enters 9 steps 2 poisoned 0 refills 0\n" +
                       GOOD_AUDIT + GOOD_STACK,
                       "off": None},
            "vmmap": {"off": vmmap_text("16K"), "on": vmmap_text("19.3M")}}


class RunnerTest(unittest.TestCase):
    def run_runner(self, scenario: dict, *extra: str):
        with tempfile.TemporaryDirectory(prefix="aot-runner-test-") as name:
            root = pathlib.Path(name)
            state = root / "state"
            state.mkdir()
            (state / "scenario.json").write_text(json.dumps(scenario))
            launcher = root / "launcher.py"
            launcher.write_text("import json\n" + FAKE_LAUNCHER)
            adb = root / "adb"
            adb.write_text(f"#!{sys.executable}\n" + FAKE_ADB)
            adb.chmod(0o755)
            wrapper = root / "wrapper.py"
            wrapper.write_text(WRAPPER.format(
                runner_directory=str(RUNNER_DIRECTORY),
                support_directory=str(RUNNER_DIRECTORY.parent / "support"),
                launcher=str(launcher)))
            vmmap = root / "vmmap"
            vmmap.write_text(f"#!{sys.executable}\n" + FAKE_VMMAP)
            vmmap.chmod(0o755)
            probe = root / "libaotprobe.so"
            probe.write_bytes(b"probe")
            environment = {**os.environ, "FAKE_GUEST_STATE": str(state),
                           "PYTHONDONTWRITEBYTECODE": "1"}
            return subprocess.run(
                [sys.executable, "-B", str(wrapper), "--adb", str(adb),
                 "--probe", str(probe), "--vmmap", str(vmmap),
                 "--boot-timeout", "30", *extra],
                stdin=subprocess.DEVNULL, capture_output=True, text=True,
                timeout=90, env=environment, check=False)

    def passes(self, scenario: dict, *extra: str):
        result = self.run_runner(scenario, *extra)
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)
        return result

    def fails(self, scenario: dict, expected: str, *extra: str):
        result = self.run_runner(scenario, *extra)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("PASS", result.stdout)
        self.assertIn(expected, result.stderr)

    def both_arms(self, scenario: dict, rows: list[str], arm: str = "on",
                  finished: bool = True) -> dict:
        scenario["workloads"][arm] = workload_text(rows, finished)
        return scenario

    # The scripted guest the other rows start from.
    def test_complete_identical_arms_pass(self):
        result = self.passes(good_scenario())
        self.assertIn("registers compared, identical", result.stdout)

    def test_script_has_eight_workloads_and_three_repetitions(self):
        self.assertEqual(len(runner.WORKLOAD_NAMES), 8)
        self.assertEqual(runner.REPETITIONS, (1, 2, 3))

    def test_empty_output_digest_fails(self):
        rows = good_workloads()
        rows[4] = f"{runner.WORKLOAD_NAMES[1]} 2 0.25 {runner.EMPTY_DIGEST} 0"
        self.fails(self.both_arms(good_scenario(), rows), "printed nothing")
        self.fails(self.both_arms(good_scenario(), rows, "off"),
                   "printed nothing")

    def test_workload_exit_status_fails(self):
        rows = good_workloads()
        rows[9] = rows[9][:-1] + "1"
        self.fails(self.both_arms(good_scenario(), rows), "status 1")
        rows[9] = rows[9][:-1] + "missing"
        self.fails(self.both_arms(good_scenario(), rows, "off"),
                   "status missing")

    def test_missing_repetition_fails(self):
        rows = good_workloads()
        del rows[7]
        self.fails(self.both_arms(good_scenario(), rows),
                   "workload rows missing")

    def test_missing_workload_fails(self):
        rows = [row for row in good_workloads()
                if not row.startswith(runner.WORKLOAD_NAMES[-1] + " ")]
        self.fails(self.both_arms(good_scenario(), rows, "off"),
                   "workload rows missing")
        self.fails(self.both_arms(good_scenario(), []), "workload rows missing")

    def test_unfinished_workloads_fail(self):
        self.fails(self.both_arms(good_scenario(), good_workloads(),
                                  finished=False), "did not run to their end")

    def test_workload_command_status_fails(self):
        scenario = good_scenario()
        scenario["workloads_status"] = 1
        self.fails(scenario, "the workload command exited 1")

    def test_output_that_differs_between_arms_fails(self):
        rows = good_workloads()
        rows[0] = f"{runner.WORKLOAD_NAMES[0]} 1 0.25 {digest('other')} 0"
        rows[1] = rows[1].replace(digest(runner.WORKLOAD_NAMES[0]),
                                  digest("other"))
        rows[2] = rows[2].replace(digest(runner.WORKLOAD_NAMES[0]),
                                  digest("other"))
        scenario = self.both_arms(good_scenario(), rows)
        self.fails(scenario, "output differs with the component on")

    def test_failed_push_fails(self):
        scenario = good_scenario()
        scenario["push_status"] = 1
        self.fails(scenario, "pushing the probe exited 1")

    def test_failed_probe_command_fails(self):
        scenario = good_scenario()
        scenario["probe_status"] = 1
        self.fails(scenario, "the probe command exited 1")

    # A probe that fails the same way in both arms is no difference between
    # them, and still no result.
    def bad_probe(self, text: str) -> dict:
        scenario = good_scenario()
        scenario["probe"] = {"off": [text], "on": [text]}
        return scenario

    def test_probe_ending_in_failure_fails(self):
        self.fails(self.bad_probe(good_probe().replace("rc=0", "rc=1")),
                   "not rc=0")

    def test_probe_missing_a_scenario_fails(self):
        self.fails(self.bad_probe(good_probe().replace(
            "case patch-change\n", "case other\n")),
            "did not take: patch-change")

    def test_probe_that_skipped_patching_fails(self):
        self.fails(self.bad_probe(good_probe().replace(
            "case patch-before\n",
            "case patch-before\n  patch cases skipped\n")),
            "patch cases skipped")

    def test_probe_without_faults_fails(self):
        text = "\n".join(line for line in good_probe().splitlines()
                         if not line.startswith("  regs")) + "\n"
        self.fails(self.bad_probe(text), "no fault registers")
        self.fails(self.bad_probe(good_probe().replace(
            "done faults=%d" % len(runner.PROBE_CASES), "done faults=0")),
            "closing record")

    def test_probe_that_differs_between_arms_fails(self):
        scenario = good_scenario()
        scenario["probe"]["on"] = [good_probe().replace(
            "x01=0x2", "x01=0x3")]
        self.fails(scenario, "without the component")

    def test_probe_that_does_not_run_to_its_end_fails(self):
        self.fails(self.bad_probe(good_probe().replace(
            "done faults=%d\n" % len(runner.PROBE_CASES), "")),
            "closing record")

    def test_product_arm_without_residency_evidence_fails(self):
        scenario = good_scenario()
        scenario["vmmap"]["on"] = vmmap_text("16K")
        self.fails(scenario, "no residency evidence")
        scenario = good_scenario()
        scenario["vmmap"]["on"] = vmmap_text("512K")
        self.fails(scenario, "no residency evidence")

    def test_framework_resident_with_the_component_off_fails(self):
        scenario = good_scenario()
        scenario["vmmap"]["off"] = vmmap_text("19.3M")
        self.fails(scenario, "resident with the component off")

    def test_product_mode_without_evidence_fails(self):
        scenario = good_scenario()
        scenario["vmmap_status"] = 1
        self.fails(scenario, "vmmap exited 1")
        scenario = good_scenario()
        scenario["vmmap"] = {"off": "nothing\n", "on": "nothing\n"}
        self.fails(scenario, "no text of the AOT framework")
        scenario = good_scenario()
        scenario["vmmap"] = {"off": vmmap_text("16K", "Other"),
                             "on": vmmap_text("19.3M", "Other")}
        self.fails(scenario, "no text of the AOT framework")

    def test_diagnostics_mode_does_not_need_the_framework_regions(self):
        scenario = good_scenario()
        scenario["vmmap"] = {"off": "nothing\n", "on": "nothing\n"}
        self.passes(scenario, "--diagnostics")

    def test_diagnostics_report_is_judged(self):
        self.passes(good_scenario(), "--diagnostics")
        scenario = good_scenario()
        scenario["report"]["on"] = REPORT_STAMP + "binds 3 enters 0 steps 0 poisoned 0\n" + GOOD_AUDIT
        self.fails(scenario, "never entered rewritten code", "--diagnostics")
        scenario = good_scenario()
        scenario["report"]["off"] = "binds 3 enters 4 steps 0 poisoned 0\n"
        self.fails(scenario, "ran with the component off", "--diagnostics")

    def test_the_duration_of_the_exact_audit_is_reported(self):
        """The exact audit holds every address space still for the length of
        its scan, so the timing of a diagnostics run is read with it."""
        result = self.passes(good_scenario(), "--diagnostics")
        self.assertIn("longest 3.2 ms", result.stdout)
        self.assertIn("total 9.0 ms", result.stdout)
        scenario = good_scenario()
        scenario["report"]["on"] = scenario["report"]["on"].replace(
            " nanos_total 9000000 nanos_max 3200000", "")
        self.fails(scenario, "lacks nanos_total, nanos_max", "--diagnostics")

    BASE = REPORT_STAMP + "binds 3 enters 9 steps 2 poisoned 0 refills 0\n"

    def test_the_count_audit_is_judged(self):
        """The run recounts the executable-mapping counts and fails on a
        mismatch, on an anomaly, or when nothing was recounted."""
        for report, expected in (
                (self.BASE, "no count audit"),
                (self.BASE + audit_line(runs="1", skipped=0), "needs at least two"),
                (self.BASE + audit_line(skipped=0, mismatches=2), "2 records whose"),
                # What the last mismatching audit saw is in the failure.
                (self.BASE + audit_line(skipped=0, mismatches=2, last_mismatches=2) +
                 "count_audit_detail generations 7 unnamed 0 c0 3 e0 2 c1 1 e1 0 "
                 "c2 0 e2 0 c3 0 e3 0\n", "c0 3, c1 1, c2 0, c3 0, e0 2"),
                (self.BASE + audit_line(skipped=0, anomalies=3), "3 accounting anomalies"),
                (self.BASE + audit_line(skipped=0, records=0), "compared no record")):
            scenario = good_scenario()
            scenario["report"]["on"] = report
            self.fails(scenario, expected, "--diagnostics")

    def test_a_count_audit_without_its_results_is_not_a_clean_one(self):
        """A report that names runs and records but not what the audit found
        is not a run that found nothing."""
        results = ("skipped", "mismatches", "last_mismatches", "anomalies", "nanos_total",
                   "nanos_max", "started", "last_exact")
        scenario = good_scenario()
        scenario["report"]["on"] = self.BASE + "count_audit runs 5 records 40\n"
        self.fails(scenario, "lacks", "--diagnostics")
        for field in results:
            scenario = good_scenario()
            scenario["report"]["on"] = re.sub(rf" {field} (\{{\w+\}}|\d+)", "",
                                              scenario["report"]["on"], count=1)
            self.assertNotIn(f" {field} ", scenario["report"]["on"])
            result = self.run_runner(scenario, "--diagnostics")
            self.assertEqual(result.returncode, 1, f"{field}: " + result.stdout + result.stderr)
            self.assertIn(field, result.stderr)

    def test_an_audit_must_have_finished_after_the_work_was_done(self):
        """The lifetime count of audits cannot say that one looked at the state
        the work left: the report names the audit it last finished, by its
        sequence number, and the runner needs one that began after the work."""
        result = self.passes(good_scenario(), "--diagnostics")
        self.assertIn("count audit:", result.stdout)
        # No audit finishes after the first third of a second: the report keeps
        # showing audits that began before the guest's work was done.
        scenario = good_scenario()
        scenario["audit_clock"] = {"stop_at": 0.3}
        self.fails(scenario, "no exact count audit finished after the guest's work was done",
                   "--diagnostics")

    def test_a_report_sampled_before_the_work_is_not_one_written_after_it(self):
        """A report whose counters were read before the guest's work ended and
        that is published late (the writer paused before the rename) shows
        audits that finished and began before the work, whatever their
        numbers say. The report carries when its counters were read."""
        scenario = good_scenario()
        scenario["audit_clock"] = {"stop_at": None, "frozen_stamp": True}
        self.fails(scenario, "no report was written after the guest's work was done",
                   "--diagnostics")

    def test_a_report_without_the_time_it_was_sampled_is_refused(self):
        scenario = good_scenario()
        scenario["report"]["on"] = scenario["report"]["on"].replace(REPORT_STAMP, "")
        self.fails(scenario, "carries no time stamp", "--diagnostics")

    def test_the_audit_to_exceed_comes_from_a_report_sampled_after_the_work(self):
        """The late publication of the review's schedule, as the runner sees it
        report by report: audit 6 finished before the work ended but its
        report appears after; the runner may take its audit number only from
        a report stamped after the work (7, with audit 7 begun), and needs an
        exact audit above that one (8)."""
        work_end = 1_000_000
        sequence = [
            {"report.at": work_end - 500, "count_audit.started": 5, "count_audit.last_exact": 5},
            {"report.at": work_end - 100, "count_audit.started": 6, "count_audit.last_exact": 6},
            {"report.at": work_end + 400, "count_audit.started": 7, "count_audit.last_exact": 6},
            {"report.at": work_end + 900, "count_audit.started": 8, "count_audit.last_exact": 7},
            {"report.at": work_end + 1400, "count_audit.started": 9, "count_audit.last_exact": 8},
        ]
        base = {"count_audit.runs": 9, **{f"count_audit.{name}": 0 for name in runner.COUNT_AUDIT_FIELDS}}
        reports = iter([{**base, **step} for step in sequence])
        arm = types.SimpleNamespace(options=types.SimpleNamespace(diagnostics=True), aot=True)
        with unittest.mock.patch.object(runner.time, "sleep"), \
                unittest.mock.patch.object(runner.time, "clock_gettime_ns", return_value=work_end), \
                unittest.mock.patch.object(runner, "read_report", side_effect=lambda _path: next(reports)):
            settled = runner.Arm.settled_report(arm, pathlib.Path("unused"))
        # The report that began audit 7 after the work is where `begun` comes
        # from; the first exact audit above 7 is the one that counts.
        self.assertEqual(settled["count_audit.last_exact"], 8)
        self.assertEqual(settled["report.at"], work_end + 1400)

    def with_stack(self, stack: str) -> dict:
        scenario = good_scenario()
        scenario["report"]["on"] = scenario["report"]["on"].replace(GOOD_STACK, stack)
        return scenario

    def test_the_stack_the_runtime_left_is_judged(self):
        self.passes(good_scenario(), "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("59200", "4096")),
                   "bytes of stack left", "--diagnostics")

    def test_a_report_without_a_stack_sample_is_not_a_run_that_had_room(self):
        sentinel = (1 << 64) - 1
        self.fails(self.with_stack(""), "no stack sample", "--diagnostics")
        self.fails(self.with_stack("stack_free 59200 stack_size 65536\n"),
                   "no stack sample", "--diagnostics")
        # What the sampler holds when no thread sampled at all.
        self.fails(self.with_stack(
            f"stack_free {sentinel} stack_size 0 stack_kind 0 "
            f"stack_task_bytes 65536 stack_engine_free {sentinel} stack_growth 0\n"),
                   "holds the sentinel", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("stack_size 65536", "stack_size 0")),
                   "zero bytes", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("stack_free 59200", "stack_free 70000")),
                   "bytes free of a stack of", "--diagnostics")

    def test_the_stack_size_is_the_kernels(self):
        # The runtime's assumption must be the kernel's configuration, and a
        # sample on a task stack must name that size.
        self.fails(self.with_stack(GOOD_STACK.replace("stack_task_bytes 65536",
                                                     "stack_task_bytes 32768")),
                   "configuration gives 65536", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("stack_size 65536", "stack_size 131072")),
                   "names a stack of 131072", "--diagnostics")
        # A thread on a host stack measures against that stack.
        self.passes(self.with_stack(GOOD_STACK.replace("stack_size 65536", "stack_size 524288")
                                    .replace("stack_kind 0", "stack_kind 1")),
                    "--diagnostics")

    def test_the_growth_over_the_engine_is_bounded(self):
        self.fails(self.with_stack(GOOD_STACK.replace("stack_growth 2800", "stack_growth 20000")),
                   "deeper than the engine's own frame", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace(" stack_growth 2800", "")),
                   "no stack sample", "--diagnostics")
        # The budget itself is allowed.
        self.passes(self.with_stack(GOOD_STACK.replace("stack_growth 2800",
                                                      f"stack_growth {runner.STACK_GROWTH_BUDGET}")),
                    "--diagnostics")

    def test_the_kernel_stack_comes_from_the_kernel_configuration(self):
        self.assertEqual(runner.kernel_stack_bytes(), 65536)
        with tempfile.TemporaryDirectory(prefix="aot-config-test-") as name:
            config = pathlib.Path(name) / "fragment"
            config.write_text("CONFIG_PAGE_SIZE_4KB=y\nCONFIG_KERNEL_STACK_ORDER=3\n")
            self.assertEqual(runner.kernel_stack_bytes(config), 32768)
            config.write_text("CONFIG_PAGE_SIZE_4KB=y\n")
            with self.assertRaises(runner.Failure):
                runner.kernel_stack_bytes(config)


def study_record(mode: str, before: int = 33, after: int = 8, faults: int = 3,
                 pressure: tuple[int, ...] = (1, 2, 0, 1), evicted: int = 1) -> list[str]:
    """What the share program prints for a mode, as a guest returned it."""
    count = share_rows.EXPECTED_CHILDREN[mode]
    lines = [f"sequential child {i} sum={0x1000 + i:#x}" for i in range(count)]
    if mode == "pressure":
        lines += [f"# round {n}: {faults} faults read libz.so's pages back; the pressure child exited 0"
                  for n, faults in enumerate(pressure)]
    if mode == "evict":
        lines += ["# control: 256 of 256 pages cached before, 0 after",
                  f"# libz.so: 0 faults read pages back before the eviction, {evicted} after"]
    if mode == "reclaim":
        lines += [f"# reclaim: libz.so {before} pages cached before the eviction, {after} after; "
                  f"{faults} faults read them back"] * share_rows.RECLAIM_ROUNDS
    return lines + [f"done mode={mode}"]


def study_arm(label: str, **eviction) -> types.SimpleNamespace:  # noqa: C901
    return types.SimpleNamespace(
        label=label, workloads={}, probe=[], debug={}, report={}, resident=(1 << 20) if label == "on" else 0,
        share={mode: study_record(mode, **eviction) for mode in share_rows.modes(True)},
        share_counts={}, share_rounds={})


def study_options(**changes) -> types.SimpleNamespace:
    values = dict(probe=None, debug_program=None, share_program=pathlib.Path("libaotshare.so"),
                  study_eviction=True, diagnostics=False, manifest=None, expect_entered=[])
    values.update(changes)
    return types.SimpleNamespace(**values)


class DebugBaselineTest(unittest.TestCase):
    """The debug rows run each mode several times without the component, and a
    field is masked when two of those runs differ. A register that counts how
    far a sibling thread got when the process crashed varies in crash-sibling
    from run to run, in a spread that depends on the host's load (measured on
    one guest: 19 values in 40 runs, and in another session one value in
    half of its runs), so three baseline runs sometimes all show one value
    and a run with the component then differs from them by chance. That mode
    has more baseline runs."""

    CRASH_SIBLING_RUNS = 8

    def test_crash_sibling_runs_more_baselines_than_the_other_modes(self):
        calls: collections.Counter = collections.Counter()

        def run_mode(shell, mode):
            calls[mode] += 1
            return ONE_RECORD
        arm = types.SimpleNamespace(
            options=types.SimpleNamespace(debug_program=pathlib.Path("libaotdebug.so")),
            aot=False, label="off", debug={})
        device = unittest.mock.Mock()
        device.run.return_value = types.SimpleNamespace(returncode=0, stdout="")
        with unittest.mock.patch.object(runner.debug_rows, "run_mode", side_effect=run_mode):
            runner.Arm.run_debug_rows(arm, device, "serial")
        for mode in runner.debug_rows.MODES:
            self.assertEqual(calls[mode],
                             self.CRASH_SIBLING_RUNS if mode == "crash-sibling" else 3, mode)

    def test_the_judging_wants_the_same_numbers(self):
        def records(sibling_runs: int):
            return {mode: [ONE_RECORD] * (sibling_runs if mode == "crash-sibling" else 3)
                    for mode in runner.debug_rows.MODES}
        options = study_options(debug_program=pathlib.Path("libaotdebug.so"), share_program=None,
                                study_eviction=False)
        on = types.SimpleNamespace(label="on", workloads={}, probe=[], report={}, resident=1 << 20,
                                   share={}, share_counts={}, share_rounds={},
                                   debug={mode: [ONE_RECORD] for mode in runner.debug_rows.MODES})
        short = types.SimpleNamespace(label="off", workloads={}, probe=[], report={}, resident=0,
                                      share={}, share_counts={}, share_rounds={}, debug=records(3))
        with self.assertRaises(runner.Failure) as caught:
            runner.check(short, on, options)
        self.assertIn(f"crash-sibling ran 3 times without the component", str(caught.exception))
        self.assertIn(f"not {self.CRASH_SIBLING_RUNS} and 1", str(caught.exception))


class StudyEvictionTest(unittest.TestCase):
    """The study's reclaim row on a guest whose kernel does not evict (a
    stock one) fails whichever way the runner was invoked."""

    STOCK = dict(before=33, after=33, faults=0)

    def judged(self, off: types.SimpleNamespace, on: types.SimpleNamespace, **options):
        runner.check(off, on, study_options(**options))

    def test_a_stock_kernel_fails_in_product_mode(self):
        # No diagnostics: nothing was counted, and the records alone must say
        # that the guest evicted the library's pages.
        with self.assertRaises(runner.Failure) as caught:
            self.judged(study_arm("off", **self.STOCK), study_arm("on", **self.STOCK))
        self.assertIn("evicted 0 of 33 pages", str(caught.exception))

    def test_a_stock_kernel_fails_in_diagnostics_without_counts(self):
        with self.assertRaises(runner.Failure) as caught:
            self.judged(study_arm("off", **self.STOCK), study_arm("on", **self.STOCK),
                        diagnostics=True)
        self.assertIn("evicted 0 of 33 pages", str(caught.exception))

    def test_either_arm_that_evicted_nothing_fails(self):
        good = dict(before=33, after=8, faults=3)
        for off, on in ((self.STOCK, good), (good, self.STOCK)):
            with self.assertRaises(runner.Failure):
                self.judged(study_arm("off", **off), study_arm("on", **on))

    def test_a_guest_that_evicted_passes_the_records_without_counts(self):
        self.judged(study_arm("off"), study_arm("on"))

    def test_the_stock_rows_print_their_readbacks_and_do_not_judge_them(self):
        """Whether the stock guest dropped a page of libz.so in the pressure
        and evict rows is chance: the count is printed, and a run in which
        neither row took a fault is not a failed one (the reclaim row holds
        the mechanism). The rows must still have printed it."""
        kept = dict(pressure=(0, 0, 0, 0), evicted=0)
        with unittest.mock.patch("sys.stdout", new=io.StringIO()) as printed:
            self.judged(study_arm("off"), study_arm("on", **kept))
        self.assertIn("stock rows (informational): 0 faults", printed.getvalue())
        with unittest.mock.patch("sys.stdout", new=io.StringIO()) as printed:
            self.judged(study_arm("off"), study_arm("on", pressure=(0, 1, 0, 0), evicted=2))
        self.assertIn("stock rows (informational): 3 faults", printed.getvalue())

    def test_the_study_option_names_what_it_needs_before_a_guest_starts(self):
        parser = runner.build_parser()
        for arguments, needs in ((["--study-eviction"], "--share-program"),
                                 (["--study-eviction", "--share-program", "p.so"], "--diagnostics"),
                                 (["--study-eviction", "--share-program", "p.so", "--diagnostics"],
                                  "--manifest")):
            options = parser.parse_args(arguments)
            with self.assertRaises(SystemExit) as caught, \
                    unittest.mock.patch("sys.stderr", new=io.StringIO()) as error:
                runner.check_options(parser, options)
            self.assertEqual(caught.exception.code, 2)
            self.assertIn(needs, error.getvalue())
        options = parser.parse_args(["--study-eviction", "--share-program", "p.so", "--diagnostics",
                                     "--manifest", "m.json"])
        runner.check_options(parser, options)

    def test_main_refuses_before_it_starts_a_guest(self):
        # The arms are replaced, so that a main which got as far as starting
        # one fails here and does not boot anything.
        started = unittest.mock.Mock(side_effect=AssertionError("a guest was started"))
        for arguments in (["--study-eviction"],
                          ["--study-eviction", "--share-program", "p.so"],
                          ["--study-eviction", "--share-program", "p.so", "--diagnostics"]):
            with unittest.mock.patch.object(runner, "Arm", started), \
                    unittest.mock.patch.object(runner.signal, "signal"), \
                    unittest.mock.patch("sys.argv", ["run_aot_guest.py", *arguments]), \
                    unittest.mock.patch("sys.stderr", new=io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    runner.main()
            self.assertEqual(caught.exception.code, 2)


class ExpectEnteredTest(unittest.TestCase):
    """--expect-entered names a library by its file name or the tail of its
    path. Two copies of one file in different places are two libraries: a
    bare name that fits both is refused, never judged by whichever came last,
    and a copy the guest never entered is not covered by its twin."""

    NAMES = ["/system/lib64/libc.so", "/system/lib64/libcrypto.so",
             "/apex/com.android.conscrypt/lib64/libcrypto.so",
             "/apex/com.android.art/lib64/liblzma.so"]

    def judge(self, expect, entered):
        with tempfile.TemporaryDirectory() as scratch:
            manifest = pathlib.Path(scratch) / "manifest.json"
            manifest.write_text(json.dumps(
                {"libraries": [{"path": name} for name in self.NAMES]}))
            report = {f"library.{index}.enters": 1 for index in entered}
            options = types.SimpleNamespace(
                manifest=manifest, expect_entered=expect)
            with contextlib.redirect_stdout(io.StringIO()):
                runner.report_libraries(report, options)

    def test_a_file_name_that_is_one_library_is_judged(self):
        self.judge(["libc.so", "liblzma.so"], [0, 3])
        with self.assertRaisesRegex(runner.Failure, "never entered"):
            self.judge(["liblzma.so"], [0])

    def test_a_tail_tells_two_copies_apart(self):
        self.judge(["com.android.conscrypt/lib64/libcrypto.so"], [2])
        with self.assertRaisesRegex(runner.Failure, "never entered .*conscrypt"):
            self.judge(["com.android.conscrypt/lib64/libcrypto.so"], [1])
        with self.assertRaisesRegex(runner.Failure, "never entered /system"):
            self.judge(["system/lib64/libcrypto.so"], [2])

    def test_a_name_that_fits_two_libraries_is_refused(self):
        with self.assertRaisesRegex(runner.Failure, "2 libraries.*tail"):
            self.judge(["libcrypto.so"], [1, 2])

    def test_a_library_the_framework_lacks_is_refused(self):
        with self.assertRaisesRegex(runner.Failure, "does not cover"):
            self.judge(["libnope.so"], [0])
        with self.assertRaisesRegex(runner.Failure, "does not cover"):
            self.judge(["lib64/libcrypto.so.1"], [1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
