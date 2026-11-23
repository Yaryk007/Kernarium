#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The thermal runner's reading of the guest.

run_android_thermal.py plays the host application against a booted release and
reads `dumpsys thermalservice`, `dumpsys display`, `dumpsys SurfaceFlinger`,
`cmd thermalservice headroom` and the launcher's frame statistics. The texts
below have the layout those commands print (framework classes
ThermalManagerService, SkinThermalStatusObserver and the display manager's vote
storage), so the runner's parsers and its records run without a guest. The
records are the golden frames of tests/unit/andarium/fixtures.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import math
from pathlib import Path
import sys
import unittest
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_thermal.py"
SPEC = importlib.util.spec_from_file_location("run_android_thermal", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
FIXTURE = TESTS / "unit/andarium/fixtures/thermal-golden-frames.txt"

THERMAL_DUMP = """\
IsStatusOverride: false
ThermalEventListeners:
	callbacks: 4
ThermalStatusListeners:
	callbacks: 2
Thermal Status: 3
Cached temperatures:
	Temperature{mValue=41.5, mType=3, mName=skin, mStatus=3}
HAL Ready: true
HAL connection:
	ThermalHAL AIDL 3  connected: yes
Current temperatures from HAL:
	Temperature{mValue=41.5, mType=3, mName=skin, mStatus=3}
Current cooling devices from HAL:
Temperature static thresholds from HAL:
	TemperatureThreshold{mType=3, mName=skin, mHotThrottlingThresholds=[NaN, 34.0, 37.0, 40.0, 43.0, 46.0, 49.0], mColdThrottlingThresholds=[NaN, NaN, NaN, NaN, NaN, NaN, NaN]}
Temperature headroom thresholds:
[NaN, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3]
"""

DISPLAY_DUMP_CAPPED = """\
  SkinThermalStatusObserver:
    mStatus: 3
    mThermalThrottlingByDisplay: {0={2=(0.0 60.0), 3=(0.0 30.0)}}
    mThermalWorkDurationsByDisplay: {}
  mVotesByDisplay:
    0:
      PRIORITY_SKIN_TEMPERATURE -> RenderVote{ RefreshRateVote{  mMinRefreshRate=0.0, mMaxRefreshRate=30.0 } }
      PRIORITY_USER_SETTING_MIN_RENDER_FRAME_RATE -> RenderVote{ RefreshRateVote{  mMinRefreshRate=0.0, mMaxRefreshRate=Infinity } }
"""

DISPLAY_DUMP_IDLE = """\
  SkinThermalStatusObserver:
    mStatus: 0
    mThermalThrottlingByDisplay: {0={2=(0.0 60.0), 3=(0.0 30.0)}}
  mVotesByDisplay:
    0:
      PRIORITY_USER_SETTING_MIN_RENDER_FRAME_RATE -> RenderVote{ RefreshRateVote{  mMinRefreshRate=0.0, mMaxRefreshRate=Infinity } }
"""

DISPLAY_DUMP_NO_MAP = """\
  SkinThermalStatusObserver:
    mStatus: 3
    mThermalThrottlingByDisplay: {0={}}
"""


class ThermalDumpTests(unittest.TestCase):
    def test_the_state_and_the_one_sensor(self) -> None:
        dump = runner.parse_thermal_dump(THERMAL_DUMP)
        self.assertEqual(dump.status, 3)
        self.assertEqual(dump.cached, [("skin", 3, 3, 41.5)])
        self.assertEqual(dump.hal, [("skin", 3, 3, 41.5)])
        self.assertTrue(dump.hal_ready)
        self.assertIn("AIDL 3", dump.hal_line)
        self.assertEqual(dump.thresholds[0][:2], ("skin", 3))
        self.assertTrue(runner.same_numbers(dump.thresholds[0][2], runner.HOT_THRESHOLDS))
        self.assertTrue(runner.same_numbers(dump.headroom_thresholds, runner.HEADROOM_THRESHOLDS))

    def test_a_second_sensor_is_seen(self) -> None:
        text = THERMAL_DUMP.replace(
            "Current cooling devices from HAL:",
            "\tTemperature{mValue=30.2, mType=2, mName=battery, mStatus=0}\nCurrent cooling devices from HAL:")
        self.assertEqual([entry[:2] for entry in runner.parse_thermal_dump(text).hal],
                         [("skin", 3), ("battery", 2)])

    def test_thresholds_that_differ_are_not_the_expected_ones(self) -> None:
        wrong = THERMAL_DUMP.replace("34.0, 37.0", "33.0, 37.0")
        dump = runner.parse_thermal_dump(wrong)
        self.assertFalse(runner.same_numbers(dump.thresholds[0][2], runner.HOT_THRESHOLDS))

    def test_no_dump_reads_as_nothing(self) -> None:
        dump = runner.parse_thermal_dump("")
        self.assertIsNone(dump.status)
        self.assertEqual((dump.cached, dump.hal, dump.thresholds), ([], [], []))
        self.assertFalse(dump.hal_ready)

    def test_headroom(self) -> None:
        self.assertAlmostEqual(runner.parse_headroom("Headroom in 0 seconds: 0.85\n"), 0.85)
        self.assertIsNone(runner.parse_headroom("Headroom in 0 seconds: NaN\n"))
        self.assertIsNone(runner.parse_headroom("Error: thermal HAL is not ready\n"))


class DisplayDumpTests(unittest.TestCase):
    def test_the_cap(self) -> None:
        display = runner.parse_display_dump(DISPLAY_DUMP_CAPPED)
        self.assertEqual(display.status, 3)
        self.assertTrue(display.map_has_severe_cap)
        self.assertTrue(display.skin_vote)
        self.assertEqual(display.skin_vote_maximum, 30.0)

    def test_no_vote_at_none(self) -> None:
        display = runner.parse_display_dump(DISPLAY_DUMP_IDLE)
        self.assertEqual(display.status, 0)
        self.assertTrue(display.map_has_severe_cap)
        self.assertFalse(display.skin_vote)
        self.assertIsNone(display.skin_vote_maximum)

    def test_an_empty_map_is_no_cap(self) -> None:
        display = runner.parse_display_dump(DISPLAY_DUMP_NO_MAP)
        self.assertFalse(display.map_has_severe_cap)
        self.assertFalse(display.skin_vote)

    def test_a_map_without_the_severe_point_is_no_cap(self) -> None:
        text = DISPLAY_DUMP_IDLE.replace("3=(0.0 30.0)", "3=(0.0 60.0)")
        self.assertFalse(runner.parse_display_dump(text).map_has_severe_cap)

    def test_a_point_whose_minimum_is_not_zero_is_no_cap_and_no_hold(self) -> None:
        # The vote of 15 to 60 Hz still lets Android render at 15: both endpoints of a point are the contract.
        moderate = DISPLAY_DUMP_IDLE.replace("2=(0.0 60.0)", "2=(15.0 60.0)")
        self.assertFalse(runner.parse_display_dump(moderate).map_has_moderate_hold)
        self.assertTrue(runner.parse_display_dump(moderate).map_has_severe_cap)
        severe = DISPLAY_DUMP_IDLE.replace("3=(0.0 30.0)", "3=(15.0 30.0)")
        self.assertFalse(runner.parse_display_dump(severe).map_has_severe_cap)
        self.assertTrue(runner.parse_display_dump(severe).map_has_moderate_hold)

    def test_the_vote_minimum_is_read(self) -> None:
        self.assertEqual(runner.parse_display_dump(DISPLAY_DUMP_CAPPED).skin_vote_minimum, 0.0)
        wrong = DISPLAY_DUMP_CAPPED.replace("mMinRefreshRate=0.0, mMaxRefreshRate=30.0", "mMinRefreshRate=15.0, mMaxRefreshRate=30.0")
        self.assertEqual(runner.parse_display_dump(wrong).skin_vote_minimum, 15.0)
        self.assertIsNone(runner.parse_display_dump(DISPLAY_DUMP_IDLE).skin_vote_minimum)

    def test_the_moderate_hold(self) -> None:
        self.assertTrue(runner.parse_display_dump(DISPLAY_DUMP_CAPPED).map_has_moderate_hold)
        self.assertTrue(runner.parse_display_dump(DISPLAY_DUMP_IDLE).map_has_moderate_hold)
        self.assertFalse(runner.parse_display_dump(DISPLAY_DUMP_NO_MAP).map_has_moderate_hold)

    def test_a_map_without_the_moderate_point_is_no_hold(self) -> None:
        text = DISPLAY_DUMP_IDLE.replace("2=(0.0 60.0), ", "")
        display = runner.parse_display_dump(text)
        self.assertFalse(display.map_has_moderate_hold)
        self.assertTrue(display.map_has_severe_cap)
        wrong = DISPLAY_DUMP_IDLE.replace("2=(0.0 60.0)", "2=(0.0 90.0)")
        self.assertFalse(runner.parse_display_dump(wrong).map_has_moderate_hold)

    def test_render_rates(self) -> None:
        text = "  x\n    renderRate=30.00 Hz\n    activeMode={vsyncRate=59.99 Hz}\n    renderRate=59.99 Hz\n"
        self.assertEqual(runner.parse_render_rates(text), [30.0, 59.99])
        self.assertEqual(runner.parse_render_rates("no rate"), [])


def frame_lines(*rates: float) -> str:
    """The launcher's frame statistics lines of a measurement, in the order they were printed."""
    return "\n".join(
        f"Native display frames: guest {rate}/s presented {rate}/s dropped 0.0/s (0.0%) cpu-fallback 0.0/s"
        for rate in rates)


class FrameStatisticsTests(unittest.TestCase):
    def test_the_first_line_after_the_start_is_not_the_measurements(self) -> None:
        # A capped step after an uncapped one, as the quiet host's log had it: the uncapped step's full
        # windows (60.0) were printed before the capped measurement began and are not in its slice; the
        # window that ended after the start (216 frames in 5 s, 43.0) is the previous step's trailing
        # one, then comes the capped step's own full window (150 frames, 30.0). The 43.0 must not be
        # the figure.
        self.assertEqual(runner.presented_rate(frame_lines(43.0, 30.0)), 30.0)
        self.assertEqual(runner.presented_rate(frame_lines(43.0, 30.0, 30.2)), 30.2)

    def test_an_uncapped_step_after_a_capped_one_reads_its_own_full_window(self) -> None:
        # The capped step's trailing window (28.5) leads; the uncapped step's full windows follow.
        self.assertEqual(runner.presented_rate(frame_lines(28.5, 60.0)), 60.0)
        self.assertEqual(runner.presented_rate(frame_lines(28.5, 45.2, 60.0)), 60.0)
        self.assertEqual(runner.presented_rate(frame_lines(0.0, 18.8, 30.6)), 30.6)

    def test_the_windows_at_the_end_of_a_run_do_not_lower_its_rate(self) -> None:
        self.assertEqual(runner.presented_rate(frame_lines(43.0, 30.2, 12.0)), 30.2)

    def test_a_measurement_without_a_full_busy_window_has_no_figure(self) -> None:
        self.assertIsNone(runner.presented_rate(frame_lines(43.0)))
        self.assertIsNone(runner.presented_rate(frame_lines(43.0, 0.0)))
        self.assertIsNone(runner.presented_rate(frame_lines(43.0, 4.9, 0.3)))
        self.assertIsNone(runner.presented_rate(""))
        self.assertIsNone(runner.presented_rate("Native display frames: guest 0.0/s presented 0.0/s dropped 0.0/s"))


class CapCheckTests(unittest.TestCase):
    """check_cap against a guest that answers from a script: the cap's checks and the measurement's length."""

    @staticmethod
    def check(guest, capped: bool, label: str, hold: float | None = None) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            runner.check_cap(guest, capped, label, hold)

    class Guest:
        def __init__(
            self, capped: bool, clock: float, host: float | None, vote: float | None = None,
            vote_minimum: float = 0.0,
        ) -> None:
            self.capped, self.clock, self.host, self.vote = capped, clock, host, vote
            self.vote_minimum = vote_minimum
            self.frames: list[int] = []

        def alive(self) -> None:
            pass

        def display(self):
            maximum = 30.0 if self.capped else self.vote
            minimum = self.vote_minimum if maximum is not None else None
            return runner.DisplayDump(3 if self.capped else 0, True, True, minimum, maximum, maximum is not None)

        def render_rates(self) -> list[float]:
            return [30.0] if self.capped else [59.99]

        def present_rate(self, frames: int = runner.PRESENT_FRAMES) -> tuple[float, float | None]:
            self.frames.append(frames)
            return self.clock, self.host

    def test_a_capped_and_an_uncapped_step_measure_ten_seconds_each(self) -> None:
        capped = self.Guest(True, 29.7, 30.0)
        self.check(capped, True, "severe")
        self.assertEqual(capped.frames, [runner.PRESENT_FRAMES])
        uncapped = self.Guest(False, 59.4, 60.0)
        self.check(uncapped, False, "none")
        self.assertEqual(uncapped.frames, [runner.UNCAPPED_PRESENT_FRAMES])
        self.assertEqual(runner.UNCAPPED_PRESENT_FRAMES, 2 * runner.PRESENT_FRAMES)

    def test_a_hold_casts_its_vote_and_changes_no_frame_rate(self) -> None:
        held = self.Guest(False, 59.4, 60.0, vote=60.0)
        self.check(held, False, "moderate", hold=60.0)
        self.assertEqual(held.frames, [runner.UNCAPPED_PRESENT_FRAMES])
        # A vote that is wrong never settles: the wait runs to its deadline, which the test shortens.
        with mock.patch.object(runner, "STATE_SECONDS", 0.5):
            with self.assertRaisesRegex(runner.Failure, "not a 0 to 60 Hz render vote"):
                self.check(self.Guest(False, 59.4, 60.0), False, "moderate", hold=60.0)
            with self.assertRaisesRegex(runner.Failure, "a skin vote remains"):
                self.check(held, False, "light")

    def test_a_vote_whose_minimum_is_not_zero_is_not_the_hold_or_the_cap(self) -> None:
        # A 15 to 60 Hz vote has the maximum of the hold and still lets Android render at 15.
        with mock.patch.object(runner, "STATE_SECONDS", 0.5):
            with self.assertRaisesRegex(runner.Failure, "not a 0 to 60 Hz render vote"):
                self.check(self.Guest(False, 59.4, 60.0, vote=60.0, vote_minimum=15.0), False, "moderate", hold=60.0)
            with self.assertRaisesRegex(runner.Failure, "not a 0 to 30 Hz render vote"):
                self.check(self.Guest(True, 29.7, 30.0, vote_minimum=15.0), True, "severe")

    def test_a_step_without_a_figure_fails_and_says_why(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "severe: .*no full busy statistics window"):
            self.check(self.Guest(True, 29.7, None), True, "severe")

    def test_a_host_rate_outside_the_band_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "the host presented 43.0 frames a second"):
            self.check(self.Guest(True, 29.7, 43.0), True, "severe")
        with self.assertRaisesRegex(runner.Failure, "the host presented 42.5 frames a second"):
            self.check(self.Guest(False, 58.4, 42.5), False, "none")


class RecordTests(unittest.TestCase):
    def fixture(self) -> dict[str, bytes]:
        frames = {}
        for line in FIXTURE.read_text().splitlines():
            if line and not line.startswith("#"):
                name, hexbytes = line.split(":")
                frames[name] = bytes.fromhex(hexbytes.replace(" ", ""))
        return frames

    def test_the_records_are_the_golden_frames(self) -> None:
        frames = self.fixture()
        self.assertEqual(runner.record(0), frames["none"])
        self.assertEqual(runner.record(1), frames["light"])
        self.assertEqual(runner.record(2), frames["moderate"])
        self.assertEqual(runner.record(3), frames["severe"])
        self.assertEqual(runner.record(4), frames["critical"])

    def test_the_refused_records_are_the_golden_rejects(self) -> None:
        frames = self.fixture()
        self.assertEqual(runner.REFUSED["emergency"], frames["reject-emergency"])
        self.assertEqual(runner.REFUSED["shutdown"], frames["reject-shutdown"])
        self.assertEqual(runner.REFUSED["magic"], frames["reject-magic"])
        self.assertEqual(runner.REFUSED["reserved"], frames["reject-reserved"])

    def test_the_levels_agree_with_the_headroom_rule(self) -> None:
        # ThermalManagerService normalizes a temperature against the SEVERE threshold (40): 1.0 there, 30
        # degrees for one unit.
        severe = runner.HOT_THRESHOLDS[3]
        for severity, (temperature, headroom) in runner.LEVELS.items():
            self.assertAlmostEqual((temperature - (severe - 30.0)) / 30.0, headroom, places=5)
            self.assertTrue(math.isnan(runner.HOT_THRESHOLDS[0]))
            low = runner.HOT_THRESHOLDS[severity] if severity else -math.inf
            self.assertLessEqual(low, temperature)
            self.assertLess(temperature, runner.HOT_THRESHOLDS[severity + 1])


if __name__ == "__main__":
    unittest.main()
