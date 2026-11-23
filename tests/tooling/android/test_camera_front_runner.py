#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The front camera runner without a guest: its reading of the chart, of the probe's statement and of the camera
service's dump, its verdict for each camera, and the probe application's package.

run_android_camera_front.py boots a release with the synthetic phone whose front camera the host turns half
around, opens each camera through Camera API 1 with a probe application, and judges the orientation the
framework states and the preview frame the application receives. The frames are drawn here as the synthetic
camera draws its chart (marker row, ramp, slanted edge) and turned and mirrored by an independent rotation, so the
chart reading is held to the eight views a frame can have. The package test skips where the host has no keytool
and jarsigner.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

TESTS = Path(__file__).resolve().parents[2]
INTEGRATION = TESTS / "integration/android"
sys.path.insert(0, str(INTEGRATION))
SPEC = importlib.util.spec_from_file_location("run_android_camera_front", INTEGRATION / "run_android_camera_front.py")
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
chart = runner.chart
probe = runner.probe

WIDTH, HEIGHT = 640, 480
TAG = 0x5A01  # a chart of the reading tests, not a camera's


def upright_chart(width: int = WIDTH, height: int = HEIGHT, counter: int = 77, tag: int = TAG) -> list[list[int]]:
    """The chart as the synthetic camera draws it, in the parts the reading uses: the ramp along every row, the
    slanted edge in the bottom quarter and the marker on the middle row."""
    grid = [[16 + 219 * x // (width - 1) for x in range(width)] for _ in range(height)]
    for y in range(height * 3 // 4, height):
        for x in range(width):
            grid[y][x] = 220 if x * 2 + (y - height * 3 // 4) > width else 40
    first48 = (tag << 32) | counter
    bits = (first48 << 16) | ((tag ^ ((first48 >> 16) & 0xFFFF) ^ (first48 & 0xFFFF)) & 0xFFFF)
    top = ((height - 8) // 2) & ~1
    for index in range(chart.MARKER_CELLS):
        one = (bits >> (chart.MARKER_CELLS - 1 - index)) & 1
        for y in range(8):
            for x in range(index * width // chart.MARKER_CELLS, (index + 1) * width // chart.MARKER_CELLS):
                grid[top + y][x] = 235 if one else 16
    return grid


def rotated_clockwise(grid: list[list[int]], quarter_turns: int) -> list[list[int]]:
    for _ in range(quarter_turns % 4):
        grid = [list(row) for row in zip(*grid[::-1])]
    return grid


def mirrored(grid: list[list[int]]) -> list[list[int]]:
    return [row[::-1] for row in grid]


def nv21_of(grid: list[list[int]]) -> bytes:
    width, height = len(grid[0]), len(grid)
    return bytes(value for row in grid for value in row) + bytes([128]) * (width * height // 2)


class ChartReading(unittest.TestCase):
    def test_an_upright_frame_is_upright_at_no_turn(self) -> None:
        luma = bytes(v for row in upright_chart() for v in row)
        self.assertEqual(chart.upright_views(luma, WIDTH, HEIGHT), [(0, False)])

    def test_a_frame_turned_by_any_quarter_is_upright_after_the_turn_that_undoes_it(self) -> None:
        for quarter in range(4):
            grid = rotated_clockwise(upright_chart(), quarter)
            luma = bytes(v for row in grid for v in row)
            width, height = len(grid[0]), len(grid)
            self.assertEqual(chart.upright_views(luma, width, height), [((360 - 90 * quarter) % 360, False)],
                             f"the chart turned {90 * quarter} degrees")

    def test_a_mirrored_frame_is_told_from_one_that_is_not(self) -> None:
        luma = bytes(v for row in mirrored(upright_chart()) for v in row)
        self.assertEqual(chart.upright_views(luma, WIDTH, HEIGHT), [(0, True)])

    def test_the_marker_read_from_its_other_end_alone_is_not_the_chart(self) -> None:
        # The check word holds for a reversed marker; the tag and the edge tell it.
        luma = bytes(v for row in rotated_clockwise(upright_chart(), 2) for v in row)
        width, height, sample = chart.view(luma, WIDTH, HEIGHT, 0, False)
        valid, tag, _ = chart.decode_marker(width, height, sample)
        self.assertTrue(valid)
        self.assertNotEqual(tag >> 8, chart.TAG_HIGH_BYTE)

    def test_a_short_frame_or_a_wrong_turn_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            chart.luma_of_nv21(b"\0" * 100, WIDTH, HEIGHT)
        with self.assertRaises(ValueError):
            chart.view(b"\0" * 4, 2, 2, 45, False)
        self.assertEqual(len(chart.luma_of_nv21(nv21_of(upright_chart()), WIDTH, HEIGHT)), WIDTH * HEIGHT)


class Statements(unittest.TestCase):
    def test_the_probes_statement_is_read(self) -> None:
        info = runner.parse_info("orientation=270 facing=1 preview=1920x1080 format=17 camera=1\n")
        self.assertEqual((info.orientation, info.facing, info.width, info.height, info.preview_format, info.camera),
                         (270, 1, 1920, 1080, 17, 1))
        for bad in ("", "orientation=270", "orientation=x facing=1 preview=1x1 format=17 camera=1"):
            with self.assertRaises(runner.Failure):
                runner.parse_info(bad)

    def test_the_dump_gives_each_cameras_orientation(self) -> None:
        dump = "\n".join([
            "== Camera service events log (most recent at top): ==",
            "    Orientation: 11",
            "== Camera HAL device device@1.1/internal/0 (v1.4) static information: ==",
            "  Resource cost: 100",
            "    Facing: Back",
            "    Orientation: 90",
            "== Camera HAL device device@1.1/internal/0 (v1.4) dumpState: ==",
            "    Orientation: 12",
            "== Camera HAL device device@1.1/internal/1 (v1.4) static information: ==",
            "    Facing: Front",
            "    Orientation: 270",
            "      android.sensor.orientation (e000e): int32[1]",
        ])
        self.assertEqual(runner.dump_orientations(dump), {"0": 90, "1": 270})
        self.assertEqual(runner.dump_orientations("nothing to read"), {})

    def test_the_profile_and_its_cameras_are_the_camera_runners(self) -> None:
        rac = runner.rac
        self.assertIn(runner.PROFILE, rac.PROFILE_CAMERAS)
        self.assertEqual(rac.PROFILE_CAMERAS[runner.PROFILE], rac.PROFILE_CAMERAS["phone"])
        self.assertEqual(rac.make_plan(rac.DEFAULT_LAYOUTS[runner.PROFILE], runner.PROFILE, "live").profile, runner.PROFILE)
        options = runner.build_parser().parse_args(["--emulator", "x"])
        self.assertEqual((options.camera_profile, options.mode), (runner.PROFILE, "live"))


class Verdicts(unittest.TestCase):
    # Each camera's chart carries its own tag (the platform index): the phone profile's front camera is the platform's 3.
    FRONT = nv21_of(rotated_clockwise(upright_chart(tag=0x5A03), 2))
    BACK = nv21_of(upright_chart(tag=0x5A00))
    BACK_TURNED = nv21_of(rotated_clockwise(upright_chart(tag=0x5A00), 2))
    FRONT_UNTURNED = nv21_of(upright_chart(tag=0x5A03))
    BACK_OF_ANOTHER_LENS = nv21_of(upright_chart(tag=0x5A01))

    def info(self, orientation: int, facing: int, camera: int, preview_format: int = 17) -> object:
        return runner.Info(orientation, facing, WIDTH, HEIGHT, preview_format, camera)

    def test_the_cameras_the_host_presents_pass(self) -> None:
        self.assertEqual(runner.check_camera(runner.FRONT, self.info(270, 1, 1), self.FRONT), [])
        self.assertEqual(runner.check_camera(runner.BACK, self.info(90, 0, 0), self.BACK), [])

    def test_each_deviation_is_named(self) -> None:
        front = runner.FRONT
        cases = {
            "orientation": (self.info(90, 1, 1), self.FRONT, "an orientation of 90"),
            "facing": (self.info(270, 0, 1), self.FRONT, "facing 0"),
            "camera": (self.info(270, 1, 0), self.FRONT, "opened camera 0"),
            "frame not turned": (self.info(270, 1, 1), self.FRONT_UNTURNED, "upright after the turns [(0, False)]"),
            "short frame": (self.info(270, 1, 1), b"\0" * 10, "fewer than an NV21"),
            "preview format": (self.info(270, 1, 1, 42), self.FRONT, "the preview format is 42, not NV21 (17)"),
        }
        for name, (info, frame, text) in cases.items():
            problems = runner.check_camera(front, info, frame)
            self.assertTrue(any(text in problem for problem in problems), f"{name}: {problems}")
        problems = runner.check_camera(runner.BACK, self.info(90, 0, 0), self.FRONT)
        self.assertTrue(any("another camera's chart" in problem for problem in problems), problems)
        # The back camera's own chart, turned half around, is a frame the host must not have turned.
        problems = runner.check_camera(runner.BACK, self.info(90, 0, 0), self.BACK_TURNED)
        self.assertTrue(any("upright after the turns [(180, False)]" in problem for problem in problems), problems)

    def test_a_chart_of_another_camera_is_not_the_cameras_own(self) -> None:
        # The back camera the guest calls 0 is the platform's 0 and draws the tag 0x5a00; a frame of the chart of another
        # lens (0x5a01), turned the right way for it, was taken for it when only the tag's prefix was read.
        problems = runner.check_camera(runner.BACK, self.info(90, 0, 0), self.BACK_OF_ANOTHER_LENS)
        self.assertTrue(any("another camera's chart" in problem and "0x5a00" in problem for problem in problems), problems)
        problems = runner.check_camera(runner.BACK, self.info(90, 0, 0, 42), self.BACK)
        self.assertTrue(any("not NV21" in problem for problem in problems), problems)


class Package(unittest.TestCase):
    def test_the_probe_package_is_signed_and_has_a_valid_dex(self) -> None:
        if shutil.which("keytool") is None or shutil.which("jarsigner") is None:
            raise unittest.SkipTest("no keytool and jarsigner")
        if subprocess.run(["keytool", "-help"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL, check=False).returncode != 0:
            raise unittest.SkipTest("keytool does not run")
        with tempfile.TemporaryDirectory() as directory:
            apk = probe.build_probe_apk(Path(directory) / "probe.apk")
            with zipfile.ZipFile(apk) as package:
                names = package.namelist()
                dex = package.read("classes.dex")
            for name in ("AndroidManifest.xml", "classes.dex", "META-INF/MANIFEST.MF"):
                self.assertIn(name, names)
        self.assertEqual(dex[:8], b"dex\n035\0")
        self.assertEqual(struct.unpack_from("<I", dex, 8)[0], zlib.adler32(dex[12:]) & 0xFFFFFFFF)
        self.assertEqual(dex[12:32], hashlib.sha1(dex[32:]).digest())
        self.assertEqual(struct.unpack_from("<I", dex, 32)[0], len(dex))
        for text in (probe.CLASS_NAME.replace(".", "/"), "orientation", "facing", "getCameraInfo", "onPreviewFrame", "frame.nv21"):
            self.assertIn(text.encode(), dex, text)
        self.assertEqual(probe.COMPONENT, probe.PACKAGE + "/.Probe")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[1:]], exit=True)
