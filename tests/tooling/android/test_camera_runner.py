#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera runner's expectations and its reading of the guest.

run_android_camera.py boots a release with the launcher's synthetic camera and a camera layout on the
vendor SKU, and reads `pm list features`, `dumpsys media.camera`, a screenshot and the still the camera
application takes. The texts below have the layout those commands print, and the images are built here,
so the runner's plan for every layout, its parsers and its image readers run without a guest. The
expectations are cross-checked against the generator's layouts, the camera ID plan of the HAL and the
launcher's own option parser.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import dataclasses
import importlib.util
import inspect
import io
import itertools
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
import zlib

TESTS = Path(__file__).resolve().parents[2]
REPOSITORY = TESTS.parent
SCRIPT = TESTS / "integration/android/run_android_camera.py"
SPEC = importlib.util.spec_from_file_location("run_android_camera", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
layouts = runner.camera_layouts

FEATURES = """\
feature:android.hardware.audio.output
feature:android.hardware.camera
feature:android.hardware.camera.any
feature:android.hardware.camera.autofocus
feature:android.hardware.camera.capability.manual_sensor
feature:android.hardware.camera.front
feature:android.hardware.location
feature:android.software.midi=0
"""

MEDIA_CAMERA = """\
== Service global info: ==

Number of camera devices: 3
Number of normal camera devices: 3
   Device 0 maps to "0"
   Device 1 maps to "1"
   Device 2 maps to "2"
Active Camera Clients:
[]
Allowed user IDs: 0

== Camera service events log (most recent at top): ==
  01-01 00:01:02 : CONNECT device 0 client for package org.lineageos.aperture (PID 1301) for Camera API 2 with Camera API 2.0 and cameraId 0
  01-01 00:00:55 : DISCONNECT device 1 client for package org.example.other (PID 900) (PRIORITY: 0)
"""

# The log is most recent first: a disconnect above a connect ends it.
MEDIA_CAMERA_DISCONNECTED = MEDIA_CAMERA.replace(
    "  01-01 00:01:02 : CONNECT device 0",
    "  01-01 00:01:30 : DISCONNECT device 0 client for package org.lineageos.aperture (PID 1301) (PRIORITY: 0)\n"
    "  01-01 00:01:02 : CONNECT device 0")
MEDIA_CAMERA_RECONNECTED = MEDIA_CAMERA_DISCONNECTED.replace(
    "  01-01 00:01:30 : DISCONNECT",
    "  01-01 00:02:10 : CONNECT device 1 client for package org.lineageos.aperture (PID 1330) for Camera API 2 with "
    "Camera API 2.0 and cameraId 1\n  01-01 00:01:30 : DISCONNECT")

MEDIA_CAMERA_IDLE = MEDIA_CAMERA.split("== Camera service events log")[0]
# The service prints each client of the list on a line of its own (and runs "State" into "User Id").
ACTIVE_CLIENTS = (
    "[\n(Camera ID: 0, Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
    "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\n]")
MEDIA_CAMERA_ACTIVE = MEDIA_CAMERA_IDLE.replace("[]", ACTIVE_CLIENTS)
# Another package holds the camera, and the log says this one connected before.
MEDIA_CAMERA_OTHER_ACTIVE = MEDIA_CAMERA.replace("[]", ACTIVE_CLIENTS.replace(
    "org.lineageos.aperture", "org.example.other"))


def static_info(device: str, facing: str, capabilities: str, flash: str, af_modes: str, extra: str = "") -> str:
    """One camera's static metadata as `dumpsys media.camera` prints it: under its header, the entries in
    the order of their names (the HAL's metadata is built from a JSON file), so the tags fall where the
    names of the sections do not follow the tags (android.info after android.flash, before android.lens),
    and the values of an entry in brackets under it."""
    return f"""\
== Camera HAL device device@1.1/internal/{device} (v3.7) static information: ==
  Resource cost: 100
  Conflicting devices: none
  API1 info:
    Has a flash unit: {flash.lower()}
    Facing: {facing}
    Orientation: 90
  CameraCharacteristics:
    android.colorCorrection.availableAberrationModes (00004): byte[3]
        [OFF FAST HIGH_QUALITY ]
    android.control.aeAvailableModes (1000e): byte[2]
        [OFF ON ]
    android.control.afAvailableModes (10012): byte[{len(af_modes.split())}]
        [{af_modes} ]
    android.flash.info.available (60000): byte[1]
        [{flash} ]
    android.info.supportedHardwareLevel (150000): byte[1]
        [LIMITED ]
    android.lens.facing (80005): byte[1]
        [{facing} ]
    android.request.availableCapabilities (c0012): byte[{len(capabilities.split())}]
        [{capabilities} ]
    android.request.availableRequestKeys (c0013): int32[40]
        [786432 786433 786434 786435 786436 786437 786438 786439 786440 786441 786442 786443 786444 786445 786446 786447 ]
        [786448 786449 786450 786451 786452 786453 786454 786455 786456 786457 786458 786459 786460 786461 786462 786463 ]
        [786464 786465 786466 786467 786468 786469 786470 786471 ]
""" + extra


def raw_metadata(hot_pixel: str = "OFF ON", shading_maps: str = "OFF ON", applied: str | None = "FALSE") -> str:
    """The entries a camera with RAW states for its maps, as the dump prints them (the lens shading state may be absent)."""
    return (f"    android.statistics.info.availableHotPixelMapModes (120006): byte[{len(hot_pixel.split())}]\n"
            f"        [{hot_pixel} ]\n"
            f"    android.statistics.info.availableLensShadingMapModes (120007): byte[{len(shading_maps.split())}]\n"
            f"        [{shading_maps} ]\n"
            + ("" if applied is None else f"    android.sensor.info.lensShadingApplied (f0009): byte[1]\n        [{applied} ]\n"))


PHONE_STATIC_INFO = (
    "== Camera Provider HAL internal/0 (v2.7, remote) static info: 2 devices: ==\n"
    + static_info("0", "BACK", "BACKWARD_COMPATIBLE MANUAL_SENSOR RAW READ_SENSOR_SETTINGS",
                  "TRUE", "OFF AUTO MACRO CONTINUOUS_PICTURE CONTINUOUS_VIDEO")
    + static_info("1", "FRONT", "BACKWARD_COMPATIBLE MANUAL_SENSOR RAW", "FALSE", "OFF AUTO CONTINUOUS_PICTURE")
    + "== Camera device dump: ==\nDumping the request that is in flight:\n"
      "  android.control.mode (10001): byte[1]\n        [AUTO ]\n")


def png(width: int, height: int, color: int, pixels: list[bytes], filters: list[int]) -> bytes:
    """A PNG whose rows use the given filters (cycled), from rows of raw pixel bytes."""
    channels = {0: 1, 2: 3, 6: 4}[color]
    stride = width * channels
    previous = bytearray(stride)
    raw = bytearray()
    for y in range(height):
        kind = filters[y % len(filters)]
        row = pixels[y]
        out = bytearray()
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            predictor = {0: 0, 1: left, 2: up, 3: (left + up) >> 1, 4: runner.paeth(left, up, corner)}[kind]
            out.append((row[i] - predictor) & 255)
        raw.append(kind)
        raw += out
        previous = bytearray(row)

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return (runner.PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def gradient(width: int, height: int, channels: int) -> list[bytes]:
    return [bytes((x * 7 + y * 13 + c * 31) & 255 for x in range(width) for c in range(channels))
            for y in range(height)]


def jpeg(width: int, height: int, exif: bool = True, end: bool = True) -> bytes:
    data = b"\xff\xd8"
    if exif:
        body = b"Exif\x00\x00" + b"MM\x00*\x00\x00\x00\x08\x00\x00"
        data += b"\xff\xe1" + struct.pack(">H", len(body) + 2) + body
    sof = struct.pack(">BHHB", 8, height, width, 1) + b"\x01\x11\x00"
    data += b"\xff\xc0" + struct.pack(">H", len(sof) + 2) + sof
    data += b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00" + b"\x12\x34\xff\x00\x56"
    return data + (b"\xff\xd9" if end else b"")


class PlanTests(unittest.TestCase):
    def test_every_layout_of_a_profile_has_a_plan_that_follows_the_generator(self) -> None:
        for profile, (cameras, ids) in runner.PROFILE_CAMERAS.items():
            matching = [layout for layout in layouts.all_layouts() if layout.cameras == cameras]
            self.assertTrue(matching, profile)
            for layout in matching:
                with self.subTest(profile=profile, layout=layout.name):
                    plan = runner.make_plan(layout.name, profile, "live")
                    self.assertEqual(plan.features, frozenset(layout.features))
                    self.assertEqual(plan.media_variant, layout.media_variant)
                    self.assertEqual(plan.camera_ids, ids)
                    self.assertTrue(plan.features)

    def test_a_layout_must_describe_the_profiles_cameras(self) -> None:
        for layout, profile in (("front", "phone"), ("bf-small", "laptop"), ("bf-large", "laptop"),
                                ("front-manual", "tablet"), ("none", "phone")):
            with self.subTest(layout=layout, profile=profile), self.assertRaises(ValueError):
                runner.make_plan(layout, profile, "live")
        for arguments in (("bf-huge", "phone", "live"), ("bf-small", "watch", "live"), ("bf-small", "phone", "x")):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                runner.make_plan(*arguments)

    def test_a_guest_without_the_camera_device_has_no_camera(self) -> None:
        plan = runner.make_plan("none", "phone", "none")
        self.assertEqual((plan.camera_ids, plan.features, plan.media_variant), ((), frozenset(), ""))
        with self.assertRaises(ValueError):
            runner.make_plan("bf-small", "phone", "none")

    def test_the_default_layouts_exist_and_fit_their_profiles(self) -> None:
        self.assertEqual(set(runner.DEFAULT_LAYOUTS), set(runner.PROFILE_CAMERAS))
        for profile, name in runner.DEFAULT_LAYOUTS.items():
            with self.subTest(profile=profile):
                runner.make_plan(name, profile, "live")
                runner.make_plan(name, profile, "blocked")

    def test_the_default_layout_of_a_profile_is_the_synthetic_camera_it_presents(self) -> None:
        # darwin/platform/synthetic_camera: the phone's back lenses have flash and manual control and its
        # wide and front cameras stream together; the tablet's front camera cannot make the guaranteed
        # concurrent sizes; the laptop has one front camera. A fresh host offers no RAW: it comes with the
        # colour data a first still keeps (darwin/device/camera/README.md, Calibration).
        phone = layouts.by_name()[runner.DEFAULT_LAYOUTS["phone"]]
        self.assertEqual((phone.flash, phone.manual, phone.raw, phone.concurrent, phone.front2160),
                         (True, True, False, True, True))
        tablet = layouts.by_name()[runner.DEFAULT_LAYOUTS["tablet"]]
        self.assertEqual((tablet.flash, tablet.manual, tablet.raw, tablet.concurrent),
                         (True, True, False, False))
        self.assertEqual(layouts.by_name()[runner.DEFAULT_LAYOUTS["laptop"]].features,
                         ("android.hardware.camera.any", "android.hardware.camera.front"))
        for name in runner.DEFAULT_LAYOUTS.values():
            self.assertFalse(layouts.by_name()[name].raw, name)
            # the layout a calibrated host composes is the same with RAW, and it exists
            if layouts.by_name()[name].manual:
                self.assertTrue(name.replace("manual", "manual-raw", 1) in layouts.by_name(), name)

    def test_the_camera_ids_follow_the_hals_plan(self) -> None:
        header = (REPOSITORY / "android/lineage-24/overlay/camera/hal/camera_ids.hpp").read_text(encoding="utf-8")
        found = {name: int(value) for name, value in re.findall(
            r"constexpr std::uint32_t (\w+_camera_id) = (\d+);", header)}
        self.assertEqual(found, {"wide_camera_id": 2, "ultra_wide_camera_id": 3, "telephoto_camera_id": 4})
        # The host publishes no zoom camera: the wide lens is "0", so a phone has no "2".
        self.assertEqual(runner.PROFILE_CAMERAS["phone"][1],
                         ("0", "1", str(found["ultra_wide_camera_id"]), str(found["telephoto_camera_id"])))
        self.assertEqual(runner.PROFILE_CAMERAS["tablet"][1], ("0", "1"))
        self.assertEqual(runner.PROFILE_CAMERAS["laptop"][1], ("0",))

    def test_the_still_caps_follow_the_tiers(self) -> None:
        small = runner.make_plan("bf-small", "tablet", "live")
        large = runner.make_plan("bf-large", "phone", "live")
        self.assertLess(small.still_maximum_pixels, large.still_maximum_pixels)
        self.assertGreaterEqual(small.still_maximum_pixels, 4000 * 3000)
        self.assertGreaterEqual(large.still_maximum_pixels, 8000 * 6000)


class LauncherTests(unittest.TestCase):
    def test_the_launcher_arguments(self) -> None:
        # Every host is given the small tier for now, so a layout of the large tier asks the launcher for it.
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-large-flash", "phone", "live")),
                         ["--camera=synthetic", "--camera-profile=phone",
                          "androidboot.product.vendor.sku=bf-large-flash", "--camera-tier=large"])
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-small-flash", "phone", "live")),
                         ["--camera=synthetic", "--camera-profile=phone",
                          "androidboot.product.vendor.sku=bf-small-flash"])
        self.assertNotIn("--camera-tier=large",
                         runner.launcher_arguments(runner.make_plan("front", "laptop", "blocked")))

    def test_every_mode_has_a_camera_source_and_the_launcher_arguments_for_it(self) -> None:
        # The restart mode runs its guests as the live plan (main() maps it); every other mode makes its own plan.
        sources = {"live": "synthetic", "restart": "synthetic", "video-lost": "synthetic", "video-leave": "synthetic",
                   "blocked": "blocked", "none": "none"}
        self.assertEqual(set(sources), set(runner.MODES), "a mode without an expected source is a mode this row does not walk")
        for mode, source in sources.items():
            with self.subTest(mode=mode):
                plan_mode = "live" if mode == "restart" else mode
                plan = runner.make_plan("none" if mode == "none" else "bf-small-flash", "phone", plan_mode)
                arguments = runner.launcher_arguments(plan)
                self.assertEqual(arguments[0], f"--camera={source}")
                if mode != "none":
                    self.assertIn("--camera-profile=phone", arguments)
        # With auto the launcher composes the layout and no SKU is passed.
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-small-flash", "phone", "live"), auto=True),
                         ["--camera=synthetic", "--camera-profile=phone", "--camera-layout=auto"])
        self.assertEqual(runner.launcher_arguments(runner.make_plan("bf-small-flash", "phone", "live"), auto=True,
                                                   calibration=Path("/c"))[-1],
                         "--camera-calibration-dir=/c")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("front", "laptop", "blocked"))[0],
                         "--camera=blocked")
        self.assertEqual(runner.launcher_arguments(runner.make_plan("none", "tablet", "none"))[0],
                         "--camera=none")
        # The launcher refuses a camera profile without a camera.
        self.assertNotIn("--camera-profile=tablet",
                         runner.launcher_arguments(runner.make_plan("none", "tablet", "none")))

    def test_the_composed_layout_has_raw_only_once_a_still_taught_the_colour_data(self) -> None:
        # Every host composes the small tier for now: no 2160p front camera, and no concurrent pair.
        self.assertEqual(runner.auto_layout("phone", taught=False), "bf-small-flash-manual")
        self.assertEqual(runner.auto_layout("phone", taught=True), "bf-small-flash-manual-raw")
        self.assertEqual(runner.auto_layout("tablet", taught=False), "bf-small-flash-manual")
        self.assertEqual(runner.auto_layout("tablet", taught=True), "bf-small-flash-manual-raw")
        self.assertEqual(runner.auto_layout("laptop", taught=False), "front")
        for profile in runner.PROFILE_CAMERAS:
            for taught in (False, True):
                name = runner.auto_layout(profile, taught)
                self.assertNotIn("large", name)
                runner.make_plan(name, profile, "live")

    def test_the_launcher_accepts_what_the_runner_passes(self) -> None:
        source = (REPOSITORY / "darwin/app/macos/main.cpp").read_text(encoding="utf-8")
        for value in set(runner.CAMERA_SOURCES.values()):
            self.assertRegex(source, rf'value == "{value}"', value)
        for profile in runner.PROFILE_CAMERAS:
            self.assertRegex(source, rf'value == "{profile}"', profile)
        self.assertIn('valued("--camera=")', source)
        self.assertIn('valued("--camera-profile=")', source)
        self.assertIn('valued("--camera-layout=")', source)
        self.assertIn('valued("--camera-calibration-dir=")', source)
        self.assertIn('valued("--camera-tier=")', source)

    def test_the_sku_argument_becomes_the_property_the_trigger_reads(self) -> None:
        self.assertEqual(layouts.SKU_BOOT_ARGUMENT, "androidboot." + layouts.SKU_PROPERTY.removeprefix("ro.boot.") + "=")
        rc = (REPOSITORY / "android/lineage-24/overlay/camera/generated" / layouts.INIT_FILE).read_text(encoding="utf-8")
        self.assertIn(f"on property:{layouts.SKU_PROPERTY}=bf-large", rc)

    def test_the_parser_defaults(self) -> None:
        options = runner.build_parser().parse_args([])
        self.assertEqual((options.mode, options.camera_profile, options.layout), ("live", "phone", None))
        self.assertEqual(options.adb_port, runner.slot_ports.slot_port(runner.slot_ports.ADB_OFFSET, 5580))
        self.assertEqual(options.switch_rounds, 4)
        self.assertIsNone(options.audio_hold_library)

    def test_the_switch_rounds_are_a_positive_whole_number(self) -> None:
        parser = runner.build_parser()
        self.assertEqual(parser.parse_args(["--switch-rounds", "12"]).switch_rounds, 12)
        self.assertEqual(parser.parse_args(["--switch-rounds", "1"]).switch_rounds, 1)
        for bad in ("0", "-3", "x", "2.5", ""):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit, msg=bad):
                parser.parse_args(["--switch-rounds", bad])
        layout = "bf-large-flash-manual-concurrent-front2160"
        self.assertEqual(runner.make_plan(layout, "phone", "live").switch_rounds, 4)
        self.assertEqual(runner.make_plan(layout, "phone", "live", 12).switch_rounds, 12)
        with self.assertRaisesRegex(ValueError, "at least one"):
            runner.make_plan(layout, "phone", "live", 0)


class GuestReaderTests(unittest.TestCase):
    def test_camera_features(self) -> None:
        self.assertEqual(runner.parse_camera_features(FEATURES), {
            "android.hardware.camera", "android.hardware.camera.any", "android.hardware.camera.autofocus",
            "android.hardware.camera.capability.manual_sensor", "android.hardware.camera.front"})
        self.assertEqual(runner.parse_camera_features("feature:android.hardware.wifi\n"), set())

    def test_camera_ids(self) -> None:
        self.assertEqual(runner.parse_camera_ids(MEDIA_CAMERA), ["0", "1", "2"])
        self.assertEqual(runner.parse_camera_ids("Number of camera devices: 0\n"), [])

    def test_a_client_of_the_camera_service(self) -> None:
        package = runner.CAMERA_PACKAGE
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA, package))
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA_ACTIVE, package))
        self.assertFalse(runner.camera_connected(MEDIA_CAMERA_IDLE, package))
        self.assertFalse(runner.camera_connected(MEDIA_CAMERA, "org.example.absent"))

    def test_a_disconnect_after_the_connect_ends_the_connection(self) -> None:
        package = runner.CAMERA_PACKAGE
        self.assertFalse(runner.camera_connected(MEDIA_CAMERA_DISCONNECTED, package))
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA_RECONNECTED, package))
        # Another package's disconnect does not end this package's connection.
        self.assertTrue(runner.camera_connected(MEDIA_CAMERA, package))
        # An active client is a connection whatever the log says.
        active = MEDIA_CAMERA_DISCONNECTED.replace("[]", ACTIVE_CLIENTS)
        self.assertTrue(runner.camera_connected(active, package))
        # The clients are the lines of the list, and only another package's does not count.
        self.assertEqual(runner.active_client_packages(MEDIA_CAMERA_ACTIVE), {package})
        self.assertEqual(runner.active_client_packages(MEDIA_CAMERA_OTHER_ACTIVE), {"org.example.other"})
        self.assertEqual(runner.active_client_packages(MEDIA_CAMERA_IDLE), set())
        self.assertFalse(runner.camera_connected(
            MEDIA_CAMERA_OTHER_ACTIVE.replace("CONNECT device 0", "DISCONNECT device 0"), package))

    def test_the_static_info_of_each_camera(self) -> None:
        infos = runner.parse_static_info(PHONE_STATIC_INFO)
        self.assertEqual([info.facing for info in infos], ["BACK", "FRONT"])
        self.assertEqual(infos[0].capabilities, {"BACKWARD_COMPATIBLE", "MANUAL_SENSOR", "RAW", "READ_SENSOR_SETTINGS"})
        self.assertEqual((infos[0].flash, infos[1].flash), (True, False))
        self.assertEqual(infos[0].af_modes, {"OFF", "AUTO", "MACRO", "CONTINUOUS_PICTURE", "CONTINUOUS_VIDEO"})
        self.assertEqual(runner.parse_static_info("Number of camera devices: 0\n"), [])
        # Entries under a header of another kind belong to no camera.
        self.assertEqual(runner.parse_static_info(
            "== Camera device dump: ==\n  android.lens.facing (80005): byte[1]\n        [BACK ]\n"), [])

    def test_a_camera_starts_at_its_header_not_at_a_falling_tag(self) -> None:
        # In the fixture the tags fall inside a camera (android.info is dumped before android.lens).
        tags = [int(m[1], 16) for m in re.finditer(r"\((\w+)\): ", static_info("0", "BACK", "RAW", "TRUE", "OFF"))]
        self.assertTrue(any(later < earlier for earlier, later in zip(tags, tags[1:])))
        infos = runner.parse_static_info(static_info("0", "BACK", "RAW", "TRUE", "OFF AUTO"))
        self.assertEqual(len(infos), 1)
        self.assertEqual((infos[0].flash, infos[0].af_modes), (True, {"OFF", "AUTO"}))

    def test_the_features_the_characteristics_earn(self) -> None:
        infos = runner.parse_static_info(PHONE_STATIC_INFO)
        earned = runner.features_of(infos)
        self.assertEqual(earned, {
            "android.hardware.camera", "android.hardware.camera.any", "android.hardware.camera.autofocus",
            "android.hardware.camera.capability.manual_sensor", "android.hardware.camera.capability.raw",
            "android.hardware.camera.flash", "android.hardware.camera.front"})
        # The phone's default layout states them, bar RAW (a fresh host has no calibration) and the combination feature.
        stated = set(layouts.by_name()[runner.DEFAULT_LAYOUTS["phone"]].features)
        self.assertEqual(stated - earned, {runner.CONCURRENT_FEATURE})
        self.assertEqual(earned - stated, {"android.hardware.camera.capability.raw"})
        # A back camera without flash or RAW, and a layout that says it has them, differ.
        plain = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "OFF AUTO"))
        self.assertEqual(runner.features_of(plain), {"android.hardware.camera", "android.hardware.camera.any",
                                                     "android.hardware.camera.autofocus"})
        # The flash and the autofocus of a front camera earn nothing: only the back camera's count.
        front = runner.parse_static_info(static_info("1", "FRONT", "BACKWARD_COMPATIBLE MANUAL_SENSOR", "TRUE", "OFF AUTO"))
        self.assertEqual(runner.features_of(front), {"android.hardware.camera.any", "android.hardware.camera.front",
                                                     "android.hardware.camera.capability.manual_sensor"})
        self.assertEqual(runner.features_of([]), set())
        # Android 17 prints the autofocus modes as numbers: 1 is AUTO.
        numbered = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "0 1 2 3 4"))
        self.assertEqual(numbered[0].af_modes, {"OFF", "AUTO", "MACRO", "CONTINUOUS_VIDEO", "CONTINUOUS_PICTURE"})
        self.assertIn("android.hardware.camera.autofocus", runner.features_of(numbered))
        unfocused = runner.parse_static_info(static_info("0", "BACK", "BACKWARD_COMPATIBLE", "FALSE", "0"))
        self.assertNotIn("android.hardware.camera.autofocus", runner.features_of(unfocused))

    def test_the_cameraserver_restart_needs_a_debuggable_build(self) -> None:
        self.assertIsNone(runner.cameraserver_restart_skip("1"))
        for value in ("0", "", "unknown"):
            self.assertIn("user build", runner.cameraserver_restart_skip(value) or "")

    def test_the_blocked_preview_is_sampled_twice(self) -> None:
        chart, black = (120.0, 60.0), (2.0, 1.0)
        sleeps: list[float] = []
        for samples, expected in (([black, black], None),
                                  ([black, chart], "the preview shows a picture (mean 120.0, deviation 60.0, sample 2)"),
                                  ([chart, black], "the preview shows a picture (mean 120.0, deviation 60.0, sample 1)")):
            with self.subTest(samples=samples):
                taken = iter(samples)
                sleeps.clear()
                self.assertEqual(runner.blocked_preview(lambda: next(taken), sleeps.append), expected)
                self.assertEqual(sleeps, [runner.BLOCKED_SAMPLE_GAP_SECONDS] if expected is None
                                 or "sample 2" in expected else [])
        self.assertEqual(runner.BLOCKED_SAMPLES, 2)

    def test_listings(self) -> None:
        self.assertEqual(runner.parse_listing("IMG_1.jpg\nIMG_2.jpg\n"), {"IMG_1.jpg", "IMG_2.jpg"})
        self.assertEqual(runner.parse_listing("ls: /sdcard/DCIM/Camera: No such file or directory\n"), set())
        self.assertEqual(runner.parse_listing(""), set())


class ProviderLifeTests(unittest.TestCase):
    PS = """\
  PID NAME
    1 init
  612 cameraserver
 1480 android.hardware.camera.provider@2.7-service-google-lazy
 1500 org.lineageos.aperture
"""

    def test_the_provider_process_is_read_from_the_process_listing(self) -> None:
        self.assertEqual(runner.parse_provider_pids(self.PS), [1480])
        self.assertEqual(runner.parse_provider_pids("  PID NAME\n  612 cameraserver\n"), [])
        self.assertEqual(runner.parse_provider_pids(""), [])

    class Guest:
        def __init__(self, listings: list[list[int]]) -> None:
            self.listings = listings

        def alive(self) -> None:
            pass

        def provider_pids(self) -> list[int]:
            return self.listings.pop(0) if len(self.listings) > 1 else self.listings[0]

    def test_the_wait_ends_when_the_provider_is_gone(self) -> None:
        guest = self.Guest([[1480], [1480], []])
        with mock.patch.object(runner.time, "sleep"):
            self.assertGreaterEqual(runner.wait_provider_exit(guest, "after the listing"), 0)

    def test_a_provider_that_never_exits_fails_the_wait(self) -> None:
        guest = self.Guest([[1480]])
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "PROVIDER_EXIT_SECONDS", 0.05):
            with self.assertRaisesRegex(runner.Failure, "camera provider to exit after the listing: it still runs as \\[1480\\]"):
                runner.wait_provider_exit(guest, "after the listing")


class PngTests(unittest.TestCase):
    def test_every_filter_decodes(self) -> None:
        for color, channels in ((0, 1), (2, 3), (6, 4)):
            width, height = 9, 8
            pixels = gradient(width, height, channels)
            for filters in ([0], [1], [2], [3], [4], [0, 1, 2, 3, 4]):
                with self.subTest(color=color, filters=filters):
                    image = runner.decode_png(png(width, height, color, pixels, filters))
                    self.assertEqual((image.width, image.height, image.channels), (width, height, channels))
                    self.assertEqual(image.rows, pixels)

    def test_a_bad_png_is_refused(self) -> None:
        good = png(4, 4, 6, gradient(4, 4, 4), [0])
        for name, data in (("signature", b"GIF89a" + good), ("truncated", good[:40]), ("empty", b""),
                           ("cut data", good[:-20])):
            with self.subTest(name=name), self.assertRaises((ValueError, zlib.error)):
                runner.decode_png(data)
        interlaced = bytearray(good)
        interlaced[8 + 8 + 12] = 1  # IHDR's interlace byte
        with self.assertRaises(ValueError):
            runner.decode_png(bytes(interlaced))

    def test_the_chart_is_told_from_black(self) -> None:
        width, height = 40, 40
        black = runner.decode_png(png(width, height, 6, [bytes(width * 4)] * height, [0]))
        mean, deviation = runner.luma_statistics(black)
        self.assertEqual((mean, deviation), (0.0, 0.0))
        patches = [bytes(value for x in range(width)
                         for value in ((255, 255, 255, 255) if (x // 5 + y // 5) % 2 else (20, 20, 20, 255)))
                   for y in range(height)]
        mean, deviation = runner.luma_statistics(runner.decode_png(png(width, height, 6, patches, [4])))
        self.assertGreater(mean, runner.BLACK_MAXIMUM_MEAN)
        self.assertGreater(deviation, runner.CHART_MINIMUM_DEVIATION)
        self.assertLess(runner.BLACK_MAXIMUM_DEVIATION, runner.CHART_MINIMUM_DEVIATION)


class JpegTests(unittest.TestCase):
    def test_a_complete_still(self) -> None:
        info = runner.parse_jpeg(jpeg(4032, 3024))
        self.assertEqual((info.width, info.height, info.exif, info.complete), (4032, 3024, True, True))

    def test_what_the_check_tells_apart(self) -> None:
        self.assertFalse(runner.parse_jpeg(jpeg(640, 480, exif=False)).exif)
        self.assertFalse(runner.parse_jpeg(jpeg(640, 480, end=False)).complete)
        for data in (b"", b"\x89PNG\r\n", b"\xff\xd8\x00"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                runner.parse_jpeg(data)


REAL_DUMP_CLIENT = (
    "Active Camera Clients:\n(Camera ID: 1, Cost: 100, PID: 2131, Score: 0, State: 2User Id: 0, "
    "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {2, 3, 4, })\n"
    "Active Client listener PIDs: 605, 876, 2131\n")
CONTENT_ROWS = (
    "Row: 0 _id=33, _display_name=VID_20261112_123456.mp4, _data=/storage/emulated/0/Movies/Aperture/"
    "VID_20261112_123456.mp4, duration=4021, _size=912345\n"
    "Row: 1 _id=34, _display_name=a, b.mp4, _data=/storage/emulated/0/Movies/x.mp4, duration=NULL, _size=0\n")
HAL_LOG = """\
11-12 12:38:31.955  2194  2215 I GCH_AidlCameraDeviceSession: ProcessBatchCaptureResult: First frame done
11-12 12:38:32.059  2194  2204 E V4l2Stream: Queue: every buffer of the stream is in flight
11-12 12:38:32.059  2194  2204 E V4l2FrameSource: BeginRequest: cannot queue a 1008x756 buffer of format 23
11-12 12:38:32.059  2194  2204 W EmulatedSensorHost: CaptureFromHost: frame 2 was not queued completely (-2)
11-12 12:38:32.100  2194  2204 W V4l2FrameSource: BeginRequest: 1 buffers of earlier requests were never collected
11-12 12:38:32.200  2194  2204 W V4l2Stream: Wait: the camera did not deliver the buffer of slot 0; the stream starts over
11-12 12:38:32.300  2194  2204 W V4l2FrameSource: QueueBuffer: the node refused the dma-buf; the 1x1 stream copies
11-12 12:38:32.400  2194  2204 E SomeOtherTag: unrelated error
11-12 12:38:32.500  2194  2204 E V4l2FrameSource: WaitFor: the camera did not deliver a buffer (-32)
11-12 12:38:32.600  2194  2204 E V4l2FrameSource: WaitFor: the camera did not deliver a buffer (-110)
"""


def box(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4s", len(payload) + 8, kind) + payload


# A log across a photo to video switch: the denial and the refusal that the camera service logs when binder will
# not hand the display composer's fence to the camera HAL (the lines have the layout logcat prints), the events
# of the switch, and lines that share a word with the denial or the refusal and are neither.
SWITCH_DENIAL = ("11-13 03:50:46.961   495   495 I auditd  : type=1400 audit(0.0:137): avc:  denied  { use } for "
                 'comm="C3Dev-0-ReqQueu" path="anon_inode:sync_file" dev="anon_inodefs" ino=5 '
                 "scontext=u:r:hal_camera_default:s0 tcontext=u:r:hal_graphics_composer_default:s0 tclass=fd "
                 "permissive=0")
SWITCH_REFUSAL = ("11-13 03:50:46.970   495  2263 E Camera3-Device: Camera 0: sendRequestsBatch: RequestThread: "
                  "Unable to submit capture request 1494 to HAL device: Broken pipe (-32)")
SWITCH_PAUSED = ("11-13 03:50:46.900  2180  2180 I wm_on_paused_called: "
                 "[87926199,org.lineageos.aperture.CameraActivity,performPause,6]")
SWITCH_RESUMED = ("11-13 03:50:47.100  2180  2180 I wm_on_resume_called: "
                  "[157384853,org.lineageos.aperture.VideoCamera,RESUME_ACTIVITY,2]")
# What a correct image logs around a switch and a force-stop (the two Camera3-OutputStream lines are cameraserver's
# own, from a stream whose application was stopped), and denials that are not the camera HAL's use of the
# composer's descriptors.
SWITCH_QUIET = """\
11-13 03:50:46.900   495  2263 D Camera3-Device: Camera 0: processCaptureRequest: request 1493 submitted
11-13 03:50:46.910   215   230 I auditd  : type=1400 audit(0.0:138): avc:  denied  { read } for comm="dmesg" \
scontext=u:r:shell:s0 tcontext=u:r:kernel:s0 tclass=system permissive=0
11-13 03:50:46.920   215   230 I auditd  : type=1400 audit(0.0:139): avc:  granted  { use } for \
scontext=u:r:hal_camera_default:s0 tcontext=u:r:surfaceflinger:s0 tclass=fd
11-13 03:50:46.930  1200  1212 W adbd    : Failed to write while dumping service activity: Broken pipe
11-13 03:50:46.940  1200  1212 E adbd    : write failed: Broken pipe (-32)
11-13 03:50:46.950   495  2263 I Camera3-Device: Camera 0: stopped: Broken pipe (-32)
11-13 05:21:55.914   491  1603 E Camera3-OutputStream: returnBufferCheckedLocked: Stream 15: Error queueing buffer to native window: Broken pipe (-32)
11-13 05:21:55.914   491  2366 E Camera3-OutputStream: getBufferLockedCommon: Stream 15: Can't dequeue next output buffer: Broken pipe (-32)
11-13 05:21:56.111   491   566 W Camera3-OutputStream: disconnectLocked: While disconnecting stream 15 from native window, the native window died from under us
11-13 03:50:46.970  5000  5001 E CameraUploader: network write failed: Broken pipe (-32)
11-13 03:50:46.971  5000  5001 E Other-Device: Unable to submit capture request 3 to HAL device: Broken pipe (-32)
"""
SWITCH_UNRELATED_DENIALS = (
    "11-13 03:50:46.970  5000  5001 I auditd: avc: denied { ptrace } for scontext=u:r:untrusted_app:s0 "
    "tcontext=u:r:hal_camera_default:s0 tclass=process permissive=0",
    "11-13 03:50:46.971   495   495 I auditd  : type=1400 audit(0.0:140): avc:  denied  { read } for "
    'comm="android.hardwar" name="foo" scontext=u:r:hal_camera_default:s0 tcontext=u:object_r:vendor_file:s0 '
    "tclass=file permissive=0",
    "11-13 03:50:46.972   495   495 I auditd  : type=1400 audit(0.0:141): avc:  denied  { use } for "
    "scontext=u:r:hal_camera_default:s0 tcontext=u:r:surfaceflinger:s0 tclass=fd permissive=0",
    "11-13 03:50:46.973   495   495 I auditd  : type=1400 audit(0.0:142): avc:  denied  { use } for "
    "scontext=u:r:cameraserver:s0 tcontext=u:r:hal_graphics_composer_default:s0 tclass=fd permissive=0",
)


MEDIA_SCALE = 90000


def edit_list(entries: list[tuple[int, int]]) -> bytes:
    """An `edts` box of one version 0 `elst`: (segment duration in the movie's time scale, media time) entries."""
    body = struct.pack(">BxxxI", 0, len(entries)) + b"".join(struct.pack(">IiI", segment, media_time, 0x10000)
                                                            for segment, media_time in entries)
    return box(b"edts", box(b"elst", body))


def audio_track(aac_frames: int, stretch: float = 1.0, rate: int = 48000, entry: bool = True,
                samples: bool = True, jump: tuple[int, float] | None = None,
                edit: tuple[float, float] | None = None) -> bytes:
    """An audio `trak` box as the recorder writes it: its `mdhd` time scale is the sample rate, its `stts` has one
    entry of `aac_frames` samples, each lasting `stretch` times the 1024 audio samples of an AAC frame, and, with
    `entry`, its `stsd` has an `mp4a` sample entry that states the sample rate. With `jump` = (index, seconds) the
    sample at `index` lasts that many seconds longer (a step in the sample times); with `edit` = (empty seconds,
    skipped seconds) an edit list starts the track after an empty edit and skips the start of its media."""
    delta = round(1024 * stretch)
    mdhd = struct.pack(">B3xIIII", 0, 0, 0, rate, aac_frames * delta) + b"\x00" * 4
    handler = box(b"hdlr", b"\x00\x00\x00\x00\x00\x00\x00\x00soun" + b"\x00" * 12)
    entries = [(aac_frames, delta)]
    if jump is not None:
        index, seconds = jump
        entries = [(index, delta), (1, delta + round(seconds * rate)), (aac_frames - index - 1, delta)]
    stts = box(b"stts", struct.pack(">BxxxI", 0, len(entries)) + b"".join(struct.pack(">II", c, d) for c, d in entries))
    # The fixed part of an audio sample entry: six reserved bytes and the data reference index, the version,
    # revision and vendor, the channel count, sample size and two reserved fields, and the 16.16 sample rate.
    mp4a = box(b"mp4a", b"\x00" * 6 + struct.pack(">H", 1) + b"\x00" * 8 + struct.pack(">HHHH", 1, 16, 0, 0)
               + struct.pack(">I", rate << 16))
    stsd = box(b"stsd", struct.pack(">BxxxI", 0, 1) + mp4a) if entry else b""
    stbl = box(b"stbl", stsd + stts) if samples else b""
    edts = b""
    if edit is not None:
        empty, skipped = edit
        edts = edit_list([(round(empty * 1000), -1), (1_000_000, round(skipped * rate))])
    return box(b"trak", edts + box(b"mdia", box(b"mdhd", mdhd) + handler + (box(b"minf", stbl) if samples else b"")))


# The bytes of encoded video a frame of the scripted movies holds (the runner wants MINIMUM_SAMPLE_BYTES for each).
PAYLOAD_PER_FRAME = 64


def movie(seconds: float, scale: int = 1000, video: bool = True, version: int = 0, fps: float = 30.0,
          samples: bool = True, audio: bytes = b"", video_jump: tuple[int, float] | None = None,
          payload: int | None = None) -> bytes:
    """A finished movie: `mvhd`, and one track whose `mdhd` has MEDIA_SCALE ticks a second and, for a video
    track with `samples`, an `stts` of one entry of seconds * fps frames (`video_jump` = (index, seconds): the frame
    at `index` lasts that many seconds longer, a freeze); with `audio` (an `audio_track`) a second track follows. The `mdat`
    holds `payload` bytes (a video track with samples: PAYLOAD_PER_FRAME for each frame; otherwise 40)."""
    if version == 1:
        mvhd = struct.pack(">B3xQQIQ", 1, 0, 0, scale, int(seconds * scale)) + b"\x00" * 80
    else:
        mvhd = struct.pack(">B3xIIII", 0, 0, 0, scale, int(seconds * scale)) + b"\x00" * 80
    frames, delta = int(seconds * fps), round(MEDIA_SCALE / fps)
    if version == 1:
        mdhd = struct.pack(">B3xQQIQ", 1, 0, 0, MEDIA_SCALE, frames * delta) + b"\x00" * 4
    else:
        mdhd = struct.pack(">B3xIIII", 0, 0, 0, MEDIA_SCALE, frames * delta) + b"\x00" * 4
    handler = box(b"hdlr", b"\x00\x00\x00\x00\x00\x00\x00\x00" + (b"vide" if video else b"soun") + b"\x00" * 12)
    entries = [(frames, delta)]
    if video_jump is not None:
        index, extra = video_jump
        entries = [(index, delta), (1, delta + round(extra * MEDIA_SCALE)), (frames - index - 1, delta)]
    stts = box(b"stts", struct.pack(">BxxxI", 0, len(entries)) + b"".join(struct.pack(">II", c, d) for c, d in entries))
    track = box(b"mdia", box(b"mdhd", mdhd) + handler + (box(b"minf", box(b"stbl", stts)) if video and samples else b""))
    if payload is None:
        payload = frames * PAYLOAD_PER_FRAME if video and samples else 40
    return (box(b"ftyp", b"isom\x00\x00\x02\x00isom") + box(b"mdat", b"\x00" * payload)
            + box(b"moov", box(b"mvhd", mvhd) + box(b"trak", track) + audio))


def sound(seconds: float = 3.5, stretch: float = 1.0, rate: int = 48000, **options) -> bytes:
    """An audio track that holds `seconds` of audio under a time line of `stretch` times as much."""
    return audio_track(round(seconds * rate / 1024), stretch, rate, **options)


class VideoAndFrontTests(unittest.TestCase):
    def test_the_cameras_a_package_holds(self) -> None:
        package = "org.lineageos.aperture"
        self.assertEqual(runner.active_client_camera_ids(REAL_DUMP_CLIENT, package), {"1"})
        self.assertEqual(runner.active_client_camera_ids(MEDIA_CAMERA_ACTIVE, package), {"0"})
        self.assertEqual(runner.active_client_camera_ids(MEDIA_CAMERA_IDLE, package), set())
        self.assertEqual(runner.active_client_camera_ids(MEDIA_CAMERA_OTHER_ACTIVE, package), set())

    def test_the_rows_of_a_content_query(self) -> None:
        rows = runner.parse_content_rows(CONTENT_ROWS + "No result found.\n")
        self.assertEqual([row["_id"] for row in rows], ["33", "34"])
        self.assertEqual(rows[0]["duration"], "4021")
        self.assertTrue(rows[0]["_data"].endswith("VID_20261112_123456.mp4"))
        self.assertEqual(rows[1]["_display_name"], "a, b.mp4")
        self.assertEqual(runner.parse_content_rows("No result found.\n"), [])

    def test_the_duration_and_tracks_of_a_movie(self) -> None:
        for version in (0, 1):
            info = runner.parse_mp4(movie(4.0, version=version))
            self.assertAlmostEqual(info.seconds, 4.0)
            self.assertTrue(info.video)
            self.assertFalse(info.audio)
        self.assertFalse(runner.parse_mp4(movie(2.0, video=False)).video)
        self.assertAlmostEqual(runner.parse_mp4(movie(1.5, scale=90000)).seconds, 1.5)

    def test_the_average_frame_rate_of_a_movie(self) -> None:
        for version in (0, 1):
            for fps in (15.0, 24.0, 30.0):
                with self.subTest(version=version, fps=fps):
                    info = runner.parse_mp4(movie(4.0, version=version, fps=fps))
                    self.assertEqual(info.frames, int(4.0 * fps))
                    self.assertAlmostEqual(info.video_seconds, 4.0, places=2)
                    self.assertAlmostEqual(info.fps, fps, places=1)
        self.assertEqual(runner.parse_mp4(movie(3.0, video=False)).fps, 0.0)
        no_samples = runner.parse_mp4(movie(3.0, samples=False))
        self.assertEqual((no_samples.frames, no_samples.fps), (0, 0.0))

    def test_the_audio_track_of_a_movie(self) -> None:
        info = runner.parse_mp4(movie(4.0, audio=sound(3.0)))
        self.assertTrue(info.audio)
        self.assertEqual((info.audio_samples, info.audio_rate), (141, 48000))
        self.assertAlmostEqual(info.audio_coded_seconds, 3.008, places=3)
        self.assertAlmostEqual(info.audio_seconds, 3.008, places=3)
        self.assertAlmostEqual(info.audio_stretch, 1.0, places=3)
        # The video track's own numbers are those of a movie without audio.
        self.assertEqual((info.frames, info.video_seconds), (120, 4.0))
        none = runner.parse_mp4(movie(4.0))
        self.assertEqual((none.audio, none.audio_samples, none.audio_rate, none.audio_stretch), (False, 0, 0, 0.0))

    def test_a_stamped_time_without_audio_behind_it_is_the_stretch(self) -> None:
        for stretch in (1.0, 1.29, 1.39, 4.74):
            with self.subTest(stretch=stretch):
                info = runner.parse_mp4(movie(5.0, audio=sound(3.0, stretch)))
                self.assertAlmostEqual(info.audio_coded_seconds, 3.008, places=3)
                self.assertAlmostEqual(info.audio_seconds, 3.008 * stretch, places=2)
                self.assertAlmostEqual(info.audio_stretch, stretch, places=3)

    def test_where_the_tracks_end_and_their_longest_steps(self) -> None:
        info = runner.parse_mp4(movie(4.0, audio=sound(4.0)))
        self.assertAlmostEqual(info.video_end, 4.0, places=3)
        self.assertAlmostEqual(info.audio_end, 4.0107, places=3)
        self.assertAlmostEqual(info.audio_skew, 0.0107, places=3)
        self.assertAlmostEqual(info.video_gap, 1 / 30, places=3)
        self.assertAlmostEqual(info.audio_gap, 1024 / 48000, places=4)
        # A step in the sample times is the longest duration of one sample, and adds to the end.
        jumped = runner.parse_mp4(movie(5.5, fps=15.0, audio=sound(4.0, jump=(100, 1.5)), video_jump=(30, 1.4)))
        self.assertAlmostEqual(jumped.audio_gap, 1.5213, places=3)
        self.assertAlmostEqual(jumped.video_gap, 1.4667, places=3)
        self.assertAlmostEqual(jumped.audio_end, 4.0107 + 1.5, places=3)
        self.assertAlmostEqual(jumped.video_end, 82 * 6000 / 90000 + 1.4, places=3)
        # Without a sample table, or a track, there is nothing to end.
        self.assertEqual(runner.parse_mp4(movie(4.0, samples=False)).video_end, 0.0)
        self.assertEqual((runner.parse_mp4(movie(4.0)).audio_end, runner.parse_mp4(movie(4.0)).audio_gap), (0.0, 0.0))

    def test_an_edit_list_moves_the_end_by_its_empty_edit_and_skip(self) -> None:
        # The movie's time scale is 1000: an empty edit of 0.5 s starts the track late, the next edit skips 0.02 s of
        # its media (an encoder's priming), so the track ends 0.48 s after its media's duration.
        info = runner.parse_mp4(movie(4.0, audio=sound(4.0, edit=(0.5, 0.02))))
        self.assertAlmostEqual(info.audio_end, 4.0107 + 0.5 - 0.02, places=3)
        self.assertAlmostEqual(info.audio_seconds, 4.0107, places=3)  # the stretch does not see edits
        plain = runner.parse_mp4(movie(4.0, audio=sound(4.0)))
        self.assertAlmostEqual(info.audio_end - plain.audio_end, 0.48, places=3)
        # Version 1 and an edit list with only a skip.
        only_skip = runner.parse_mp4(movie(4.0, audio=sound(4.0, edit=(0.0, 0.03))))
        self.assertAlmostEqual(only_skip.audio_end, 4.0107 - 0.03, places=3)

    def test_the_capture_of_the_audio_flinger_is_read_from_its_dump(self) -> None:
        self.assertEqual(runner.active_capture_frames(audio_flinger_dump("capturing", 12345)), 12345)
        for state in ("standby", "idle", "none"):
            with self.subTest(state=state):
                self.assertIsNone(runner.active_capture_frames(audio_flinger_dump(state, 12345)))
        # The history of a removed record thread (lines with a dash) and an output thread are not capture.
        self.assertIsNone(runner.active_capture_frames("- Input thread 0x1, name AudioIn_E, tid 523, type 3 (RECORD):\n"
                                                       "-   Standby: no\n-   Frames read: 5\n-   1 Tracks of which 1 are active\n"))
        self.assertIsNone(runner.active_capture_frames(""))
        # Two record threads: the one that reads the most is the capture.
        both = audio_flinger_dump("capturing", 100) + audio_flinger_dump("capturing", 900)
        self.assertEqual(runner.active_capture_frames(both), 900)

    def test_the_frames_the_input_made_up_are_read_from_its_log(self) -> None:
        log = ("11-14 21:40:01.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers no frames in time, "
               "making silence up to keep to real time\n"
               "11-14 21:40:02.500   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers again, after 67200 "
               "frames of silence made up\n"
               "11-14 21:40:09.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: dropped 480 frames since the last message\n"
               "11-14 21:40:12.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers again, after 1024 "
               "frames of silence made up\n")
        self.assertEqual(runner.made_up_frames(log), [67200, 1024])
        self.assertEqual(runner.made_up_frames(""), [])

    def test_the_sample_rate_is_the_sample_entrys_and_else_the_time_scale(self) -> None:
        for rate in (44100, 48000):
            info = runner.parse_mp4(movie(4.0, audio=sound(3.0, rate=rate)))
            self.assertEqual(info.audio_rate, rate)
            self.assertAlmostEqual(info.audio_coded_seconds, info.audio_samples * 1024 / rate)
        # Without a sample entry the track's time scale, which the recorder sets to the sample rate, stands in.
        self.assertEqual(runner.parse_mp4(movie(4.0, audio=sound(3.0, rate=44100, entry=False))).audio_rate, 44100)
        # A sample entry too short to hold the rate is no entry.
        short = bytearray(movie(4.0, audio=sound(3.0, rate=44100)))
        at = bytes(short).index(b"mp4a") - 4
        short[at:at + 4] = struct.pack(">I", 20)
        self.assertEqual(runner.parse_mp4(bytes(short)).audio_rate, 44100)

    def test_an_audio_track_without_a_sample_table_has_no_stretch(self) -> None:
        info = runner.parse_mp4(movie(4.0, audio=sound(3.0, samples=False)))
        self.assertTrue(info.audio)
        self.assertEqual((info.audio_samples, info.audio_seconds, info.audio_rate, info.audio_stretch), (0, 0.0, 0, 0.0))

    def test_a_sample_table_that_does_not_fit_is_refused(self) -> None:
        whole = bytearray(movie(2.0))
        at = bytes(whole).index(b"stts") + 8
        whole[at:at + 4] = struct.pack(">I", 1000)  # entry count: far more than the box holds
        with self.assertRaisesRegex(ValueError, "does not fit"):
            runner.parse_mp4(bytes(whole))

    def test_an_unfinished_movie_is_refused(self) -> None:
        whole = movie(3.0)
        for data in (b"", b"\x00\x00\x00\x18ftypisom", whole[:-30], box(b"ftyp", b"isom") + box(b"mdat", b"\x00" * 8)):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                runner.parse_mp4(data)

    def test_the_hal_log_failures(self) -> None:
        failures = runner.hal_failures(HAL_LOG)
        self.assertEqual(len(failures), 6)
        self.assertTrue(all("SomeOtherTag" not in line and "dma-buf" not in line for line in failures))
        # A wait the framework's flush interrupted is no failed buffer; one that timed out is.
        self.assertTrue(all("(-32)" not in line for line in failures))
        self.assertTrue(any("(-110)" in line for line in failures))
        self.assertEqual(runner.hal_failures(HAL_LOG.split("\n")[0] + "\n"), [])
        self.assertEqual(runner.hal_failures(""), [])


def audio_flinger_dump(state: str, frames_read: int) -> str:
    """The parts of `dumpsys media.audio_flinger` that the capture check reads: a record thread in the state
    `capturing` (out of standby, one active client), `standby`, `idle` (out of standby, no client) or `none` (no record
    thread), an output thread that the check must not take for it, and the history of a removed record thread."""
    standby = "yes" if state == "standby" else "no"
    clients = "0 Tracks of which 0 are active" if state in ("standby", "idle") else "1 Tracks of which 1 are active"
    record = ("Input thread 0xb4007e6502f60a38, name AudioIn_1E, tid 2500, type 3 (RECORD):\n"
              f"  Standby: {standby}\n  Sample rate: 48000 Hz\n  Frames read: {frames_read}\n  {clients}\n"
              "    Active     Id Client(pid/uid) Session Port Id  S  Flags   Format Chn mask  SRate Source\n"
              "  11-14 12:15:31.604 standby\n") if state != "none" else ""
    return ("Output thread 0xb4007e6502f60a30, name AudioOut_D, tid 2000, type 0 (MIXER):\n  Standby: no\n"
            "  1 Tracks of which 1 are active\n"
            + record +
            "- Input thread 0xb4007e6502f60a38, name AudioIn_E, tid 523, type 3 (RECORD):\n-   Standby: no\n"
            "-   Frames read: 99999999\n-   1 Tracks of which 1 are active\n"
            "Notes:\n  nothing else\n")


class AudioHoldTests(unittest.TestCase):
    """The runner's side of the audio hold library: a request, renamed to its name, and the library's log line."""

    def guest(self, directory: Path, library_lines: list[str] | None = None, alive: bool = True):
        process = mock.Mock(pid=4242)
        process.poll.return_value = None if alive else 1
        process.returncode = 1
        log = directory / "emulator.log"
        log.write_text("")
        return runner.Guest("adb", "127.0.0.1:1", process, log, directory, directory / "hold")

    def serve(self, directory: Path, answer: str) -> threading.Thread:
        """The library: takes the request file named `hold`, removes it and logs `answer` (formatted with the held ns)."""
        def run() -> None:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                request = directory / "hold" / "hold"
                if request.is_file():
                    asked = int(request.read_text())
                    request.unlink()
                    with (directory / "hold" / "holds").open("a") as log:
                        log.write(answer.format(asked=asked, held=asked * 1_000_000) + "\n")
                    return
                time.sleep(0.01)
        thread = threading.Thread(target=run)
        thread.start()
        return thread

    def test_a_hold_is_a_request_the_library_answers(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "hold").mkdir()
            guest = self.guest(directory)
            thread = self.serve(directory, "1 {held} 12345 1")
            self.assertAlmostEqual(guest.hold_audio(0.5), 0.5)
            thread.join()
            # Nothing is left to be read half written, and the next hold is the next line of the log.
            self.assertEqual(sorted(path.name for path in (directory / "hold").iterdir()), ["holds"])
            thread = self.serve(directory, "2 {held} 12346 1")
            self.assertAlmostEqual(guest.hold_audio(0.25), 0.25)
            thread.join()

    def test_a_hold_that_did_not_happen_is_a_failure(self) -> None:
        for answer, what in (("1 0 12345 0", r"held 0 thread\(s\)"), ("1 {held} 12345 2", r"held 2 thread\(s\)"),
                             ("1 100000000 12345 1", r"for 0\.10 s of the 0\.5 s asked")):
            with self.subTest(answer=answer), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                (directory / "hold").mkdir()
                thread = self.serve(directory, answer)
                with self.assertRaisesRegex(runner.Failure, what + r".*cannot judge the input"):
                    self.guest(directory).hold_audio(0.5)
                thread.join()

    def test_no_answer_and_no_library_and_a_dead_guest_fail_with_their_reasons(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "hold").mkdir()
            with mock.patch.object(runner, "AUDIO_HOLD_WAIT_SECONDS", 0.2), self.assertRaisesRegex(
                    runner.Failure, r"reported no hold within 0\.7 s \(requested 0\.5 s\)"):
                self.guest(directory).hold_audio(0.5)
            with self.assertRaisesRegex(runner.Failure, "the guest exited with status 1"):
                self.guest(directory, alive=False).hold_audio(0.5)
            bare = self.guest(directory)
            bare.hold_directory = None
            with self.assertRaisesRegex(runner.Failure, "no audio hold library: the run was not given --audio-hold-library"):
                bare.hold_audio(0.5)

    def test_the_library_is_inserted_into_the_launcher_the_runner_starts(self) -> None:
        options = runner.build_parser().parse_args(["--audio-hold-library", "build/uml_audio_hold"])
        self.assertEqual(options.audio_hold_library, Path("build/uml_audio_hold"))
        source = SCRIPT.read_text()
        start = source[source.index("def start_guest"):source.index("def positive_integer")]
        self.assertIn('environment["DYLD_INSERT_LIBRARIES"] = str(options.audio_hold_library.resolve())', start)
        self.assertIn('environment["UML_AUDIO_HOLD_DIRECTORY"] = str(hold_directory)', start)
        self.assertIn("env=environment", start)


# The consumer the camera service lists for the application's preview stream.
PREVIEW_CONSUMER = "f279c8e SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity]#1"


class FakeGuest:
    """A guest that answers the runner's commands from a script and records them."""

    def __init__(self, plan, front_holds: bool = True, movies=None) -> None:
        self.plan = plan
        self.commands: list[str] = []
        # The scripted guest has no side keys: the sensor privacy step says so and asks it nothing.
        self.hardware_input = False
        self.held = {"0"}
        self.front_holds = front_holds
        self.rows: dict[str, dict[str, str]] = {"1": {"_id": "1", "_data": "/sdcard/old.mp4"}}
        self.recording = False
        self.movie = movies if movies is not None else movie(3.5, audio=sound(3.5))
        # The seconds for which the runner stopped the emulator process, each time it did.
        self.frozen: list[float] = []
        # What the held still step reads: the launcher's log, the preview layer and the times SurfaceFlinger
        # presented its frames. A stop of the emulator leaves the preview away for as long as it lasted and
        # `away_more` seconds more, and the launcher prints a closed and an opened capture session if the stop
        # makes the camera HAL close the stream (`closes_on_freeze`).
        self.launcher = ""
        self.layer = "SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity](BLAST)#138"
        self.present = [1_000_000_000 + 33_000_000 * i for i in range(30)]
        self.closes_on_freeze = False
        self.away_more = 0.0
        # The stop lasts this much longer than it was asked to (a sleep that overshoots); the step compares with the
        # stop as it lasted.
        self.stop_extra = 0.0
        # What the camera HAL's result dispatcher reports for the nth stop: 1, the still's buffer is the oldest pending
        # (the stop was inside its request); 0, the preview's is and the still's waits behind it (before the request);
        # 2, only the preview's buffers are pending (after it); None: no report. A round beyond the list uses the last.
        self.head_streams: list[int | None] = [1]
        # How long after the marker (guest seconds) the report is printed.
        self.report_after = 1.0
        self.markers: list[str] = []
        self.has_still_stream = True
        # The audio hold library: the seconds each hold of the sound device's thread was asked for, whether the
        # input then makes silence up (its log line), what the audio flinger's record thread shows, and the
        # input's own log.
        self.hold_directory: Path | None = Path("audio-hold")
        self.audio_held: list[float] = []
        self.makes_up = True
        self.capture_state = "capturing"
        self.audio_frames_read = 100000
        self.audio_looks = 0
        self.stream_text = ""
        self.still_files = {"IMG_old.jpg"}
        self.log = ""
        # The camera service's lines that report a leaked stream (the video-lost mode's oracle).
        self.service_leaks: list[str] = []
        # The log of a mode switch step. The video mode's start by round number n logs the events of the switch
        # (events[n] = (photo paused, video resumed), both by default) and then switch_logs[n]; the nth read of
        # the log is followed by late_logs[n] (lines that land after it) and, in wrap_after_read, by a log that
        # holds nothing any more.
        self.full_log = "11-13 03:50:00.000   495   495 I Camera3-Device: before any switch\n"
        self.switch_logs: dict[int, str] = {}
        self.events: dict[int, tuple[bool, bool]] = {}
        self.late_logs: dict[int, str] = {}
        self.wrap_after_read: set[int] = set()
        self.no_markers = False
        self.log_reads = 0
        self.video_starts = 0
        # The camera service's frame counters: they rise on every read except in the phases named here.
        self.stalled: set[str] = set()
        # The phases ("photo", "video") in which the application opens no camera.
        self.no_camera_in: set[str] = set()
        self.in_video = False
        self.frames = 1000
        # Commands that fail, as a guest command that exits non-zero does.
        self.failing: set[str] = set()
        # The global settings (the window animation scales) and what they were at each start of the video mode.
        self.settings: dict[str, str] = {}
        self.settings_at_video_start: list[dict[str, str]] = []
        chart = [bytes(value for x in range(40) for value in ((255,) * 4 if (x // 5 + y // 5) % 2 else (20, 20, 20, 255)))
                 for y in range(40)]
        self.chart = runner.decode_png(png(40, 40, 6, chart, [0]))

    def alive(self) -> None:
        pass

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if command in self.failing:
            raise runner.Failure(f"'{command}' exited 1: injected")
        if command.startswith("am start") and "USE_FRONT_CAMERA true" in command:
            # The laptop's only camera is the front one, camera 0.
            front = "0" if self.plan.profile == "laptop" else "1"
            self.held = {front} if self.front_holds else {"0"}
        elif command.startswith("am start"):
            self.held = set() if ("video" if "VIDEO_CAMERA" in command else "photo") in self.no_camera_in else {"0"}
        if command.startswith(f"log -t {runner.HELD_STILL_MARKER_TAG} '"):
            self.markers.append(command.split("'")[1])
        if command.startswith(f"log -t {runner.SWITCH_MARKER_TAG} '") and not self.no_markers:
            marker = command.split("'")[1]
            number = int(re.search(r"switch (\d+) of", marker).group(1))
            self.full_log += f"11-13 03:50:46.800   100   100 I {runner.SWITCH_MARKER_TAG}: {marker}\n"
        if command == "logcat -d -b all":
            self.log_reads += 1
            text = self.full_log
            self.full_log += self.late_logs.get(self.log_reads, "")
            if self.log_reads in self.wrap_after_read:
                self.full_log = ""
            return text
        if command.startswith("settings get global "):
            return self.settings.get(command.rsplit(" ", 1)[1], "null") + "\n"
        if command.startswith("settings put global "):
            _, _, _, key, value = command.split(" ")
            self.settings[key] = value
        if command.startswith("settings delete global "):
            self.settings.pop(command.rsplit(" ", 1)[1], None)
        if command.startswith("am start") and "VIDEO_CAMERA" in command:
            self.settings_at_video_start.append(dict(self.settings))
            self.video_starts += 1
            self.in_video = True
            paused, resumed = self.events.get(self.video_starts, (True, True))
            self.full_log += (SWITCH_PAUSED + "\n" if paused else "") + (SWITCH_RESUMED + "\n" if resumed else "")
            self.full_log += self.switch_logs.get(self.video_starts, "")
        elif command.startswith("am start") or command.startswith("am force-stop"):
            self.in_video = False
        if command == f"input keyevent {runner.CAMERA_KEYCODE}":
            if "VIDEO_CAMERA" in " ".join(self.commands):
                if self.recording:
                    self.rows["2"] = {"_id": "2", "_data": "/sdcard/new.mp4", "_display_name": "new.mp4"}
                self.recording = not self.recording
            else:
                self.still_files.add("IMG_new%d.jpg" % len(self.still_files))
        return ""

    def media_camera(self) -> str:
        clients = "".join(
            f"(Camera ID: {camera}, Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
            "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\n" for camera in sorted(self.held))
        if ("video" if self.in_video else "photo") not in self.stalled:
            self.frames += 25
        still = ("    Stream[1]: Output\n      Consumer name: ImageReader-4032x3024f21m4-2209-0\n"
                 "      Dims: 4032 x 3024, format 0x21, dataspace 0x8c20000\n"
                 "      Frames produced: 9, last timestamp: 1 ns\n") if self.has_still_stream else ""
        return ("Active Camera Clients:\n" + clients + "Active Client listener PIDs: 1\n"
                f"    Stream[3]: Output\n      Consumer name: {PREVIEW_CONSUMER}\n"
                f"      Dims: 1008 x 756, format 0x23, dataspace 0x0\n"
                f"      Frames produced: {self.frames}, last timestamp: 1 ns\n" + still)

    def screenshot(self):
        return self.chart

    def freeze(self, seconds: float) -> float:
        self.frozen.append(seconds)
        seconds += self.stop_extra
        resume = self.present[-1] + round((seconds + self.away_more) * 1e9)
        self.present += [resume + 33_000_000 * i for i in range(10)]
        if self.closes_on_freeze:
            self.launcher += ("Camera: memory: capture session closed, footprint 3466 MiB\n"
                              "Camera: memory: capture session opened, footprint 3468 MiB\n")
        return seconds

    def dispatcher_log(self) -> str:
        """The result dispatcher's report after each stop's marker: the oldest pending buffer first by frame number."""
        text = ""
        for number, marker in enumerate(self.markers):
            head = self.head_streams[min(number, len(self.head_streams) - 1)]
            second = 100.0 + 10 * number
            text += f"   {second:.3f}  2423  2423 I {runner.HELD_STILL_MARKER_TAG}: {marker}\n"
            if head == 2:
                text += (f"   {second + self.report_after:.3f}  2292  2379 W GCH_ResultDispatcher: [Cam0_ResultDispatcher] "
                         f"PrintTimeoutMessages: normal_request_map: pending buffer of stream 0 for frames "
                         f"[{905 + number} - {908 + number}] (4 frames) ready 0\n")
            elif head is not None:
                other = 1 - head
                text += (f"   {second + self.report_after:.3f}  2292  2379 W GCH_ResultDispatcher: [Cam0_ResultDispatcher] "
                         f"PrintTimeoutMessages: normal_request_map: pending buffer of stream {other} for frames "
                         f"[{905 + number} - {908 + number}] (4 frames) ready 0\n"
                         f"   {second + self.report_after:.3f}  2292  2379 W GCH_ResultDispatcher: [Cam0_ResultDispatcher] "
                         f"PrintTimeoutMessages: normal_request_map: pending buffer of stream {head} for frame {904 + number} ready 0\n"
                         f"   {second + self.report_after + 0.5:.3f}  2292  2379 W GCH_ResultDispatcher: [Cam0_ResultDispatcher] "
                         f"PrintTimeoutMessages: normal_request_map: pending buffer of stream {other} for frames "
                         f"[{905 + number} - {910 + number}] (6 frames) ready 0\n")
        return text

    def launcher_log(self) -> str:
        return self.launcher

    def preview_layer(self) -> str | None:
        return self.layer

    def present_times(self, layer: str) -> list[int]:
        return list(self.present)

    def hold_audio(self, seconds: float) -> float:
        self.audio_held.append(seconds)
        if self.makes_up:
            self.stream_text += ("11-14 21:40:01.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers "
                                 f"again, after {round((seconds - 0.1) * 48000)} frames of silence made up\n")
        return seconds

    def audio_flinger(self) -> str:
        self.audio_looks += 1
        # The record thread reads more between two looks while it captures; "stuck" is a client that reads nothing.
        return audio_flinger_dump("capturing" if self.capture_state == "stuck" else self.capture_state,
                                  self.audio_frames_read + (4096 * self.audio_looks if self.capture_state == "capturing" else 0))

    def stream_log(self) -> str:
        return self.stream_text

    def videos(self):
        return dict(self.rows), "Row: 0 ..."

    def stills(self) -> set[str]:
        return set(self.still_files)

    def hal_log(self) -> str:
        return self.log

    def stream_leaks(self) -> list[str]:
        return list(self.service_leaks)

    def pull(self, remote: str) -> bytes:
        return jpeg(4000, 3000) if remote.endswith(".jpg") else self.movie


CAMERA_RESUMED = "topResumedActivity=ActivityRecord{1a2b3c u0 org.lineageos.aperture/.CameraActivity t12}"
CAMERA_FOCUS = "mCurrentFocus=Window{4d5e6f u0 org.lineageos.aperture/org.lineageos.aperture.CameraActivity}"


def foreground_text(resumed: str = CAMERA_RESUMED, focus: str = CAMERA_FOCUS, wakefulness: str = "Awake") -> str:
    return f"  {resumed}\n  {focus}\n  mWakefulness={wakefulness}\n"


class BlockedSamplerTests(unittest.TestCase):
    """A blocked preview sample counts only while the camera application is the resumed activity and the
    device is awake."""

    BLACK, PICTURE = (0.0, 0.0), (128.2, 21.5)
    FRONT = runner.parse_foreground(foreground_text())
    LAUNCHER = runner.parse_foreground(foreground_text(
        "topResumedActivity=ActivityRecord{7a8b9c u0 org.lineageos.trebuchet/.Launcher t1}",
        "mCurrentFocus=Window{9d8e7f u0 org.lineageos.trebuchet/org.lineageos.trebuchet.Launcher}"))
    ASLEEP = runner.parse_foreground(foreground_text(wakefulness="Asleep"))

    def sampler(self, states: list, values: list, calls: dict):
        """A sampler over scripted foreground states and sample values; `calls` records the screenshots, the
        bring-backs and the settle sleeps."""
        shown, taken = iter(states), iter(values)
        calls.update(screenshots=0, brought=0, slept=[])

        def take():
            calls["screenshots"] += 1
            return next(taken)

        def bring_back():
            calls["brought"] += 1

        return runner.BlockedSampler(lambda: next(shown), take, bring_back, calls["slept"].append)

    def test_the_foreground_is_read_from_the_guest_dump(self) -> None:
        front = runner.parse_foreground(foreground_text())
        self.assertEqual((front.resumed, front.wakefulness), ("org.lineageos.aperture/.CameraActivity", "Awake"))
        self.assertEqual(front.focus, "org.lineageos.aperture/org.lineageos.aperture.CameraActivity")
        self.assertTrue(front.camera_in_front())
        older = runner.parse_foreground(foreground_text(
            "mResumedActivity: ActivityRecord{1a2b3c u0 org.lineageos.aperture/.CameraActivity t12}"))
        self.assertTrue(older.camera_in_front())
        both = runner.parse_foreground("ResumedActivity: ActivityRecord{1 u0 other.app/.Main t2}\n"
                                       + foreground_text())
        self.assertEqual(both.resumed, "org.lineageos.aperture/.CameraActivity")
        self.assertFalse(self.LAUNCHER.camera_in_front())
        self.assertFalse(self.ASLEEP.camera_in_front())
        nothing = runner.parse_foreground("")
        self.assertEqual((nothing.resumed, nothing.focus, nothing.wakefulness), ("", "", ""))
        self.assertFalse(nothing.camera_in_front())
        # A package that only starts like the camera application's is not it.
        self.assertFalse(runner.parse_foreground(foreground_text(
            "topResumedActivity=ActivityRecord{1 u0 org.lineageos.aperture.other/.Main t2}")).camera_in_front())

    def test_in_front_and_black_passes(self) -> None:
        calls: dict = {}
        sampler = self.sampler([self.FRONT] * 4, [self.BLACK, self.BLACK], calls)
        self.assertIsNone(runner.blocked_preview(sampler, lambda seconds: None))
        self.assertEqual((calls["screenshots"], calls["brought"]), (2, 0))

    def test_in_front_and_a_picture_fails_the_picture_check(self) -> None:
        calls: dict = {}
        sampler = self.sampler([self.FRONT] * 4, [self.BLACK, self.PICTURE], calls)
        self.assertEqual(runner.blocked_preview(sampler, lambda seconds: None),
                         "the preview shows a picture (mean 128.2, deviation 21.5, sample 2)")
        self.assertEqual(calls["brought"], 0)

    def test_not_in_front_twice_is_its_own_failure(self) -> None:
        for gone in (self.LAUNCHER, self.ASLEEP):
            with self.subTest(gone=gone):
                calls: dict = {}
                sampler = self.sampler([gone, gone], [], calls)
                out = io.StringIO()
                with contextlib.redirect_stdout(out), self.assertRaises(runner.Failure) as raised:
                    runner.blocked_preview(sampler, lambda seconds: None)
                message = str(raised.exception)
                self.assertIn("a blocked session cannot be judged: the camera application is not in front", message)
                self.assertIn(f"resumed activity {gone.resumed}", message)
                self.assertIn(f"focused window {gone.focus}", message)
                self.assertIn(f"wakefulness {gone.wakefulness}", message)
                self.assertEqual((calls["screenshots"], calls["brought"]), (0, 1))
                self.assertEqual(calls["slept"], [runner.BRING_BACK_SETTLE_SECONDS])

    def test_nothing_known_about_the_foreground_is_a_failure_not_a_pass(self) -> None:
        calls: dict = {}
        empty = runner.parse_foreground("")
        sampler = self.sampler([empty, empty], [self.BLACK], calls)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(
                runner.Failure, r"resumed activity none, focused window none, wakefulness unknown"):
            runner.blocked_preview(sampler, lambda seconds: None)
        self.assertEqual(calls["screenshots"], 0)

    def test_brought_back_once_and_sampled_again(self) -> None:
        calls: dict = {}
        sampler = self.sampler([self.LAUNCHER, self.FRONT, self.FRONT, self.FRONT, self.FRONT], [self.BLACK, self.BLACK], calls)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertIsNone(runner.blocked_preview(sampler, lambda seconds: None))
        self.assertIn("the camera application is not in front (resumed activity org.lineageos.trebuchet/.Launcher", out.getvalue())
        self.assertEqual((calls["screenshots"], calls["brought"]), (2, 1))

    def test_an_application_that_leaves_during_the_screenshot_does_not_count(self) -> None:
        calls: dict = {}
        # In front before the first screenshot, gone after it (that picture is dropped), back and in front after bring-back.
        sampler = self.sampler([self.FRONT, self.LAUNCHER, self.FRONT, self.FRONT, self.FRONT, self.FRONT],
                               [self.PICTURE, self.BLACK, self.BLACK], calls)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertIsNone(runner.blocked_preview(sampler, lambda seconds: None))
        self.assertEqual((calls["screenshots"], calls["brought"]), (3, 1))

    def test_the_bring_back_is_once_for_the_whole_step(self) -> None:
        calls: dict = {}
        # Sample 1 needs the bring-back; the application is gone again at sample 2: the step fails, no second bring-back.
        sampler = self.sampler([self.LAUNCHER, self.FRONT, self.FRONT, self.LAUNCHER], [self.BLACK], calls)
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(runner.Failure, "cannot be judged"):
            runner.blocked_preview(sampler, lambda seconds: None)
        self.assertEqual((calls["screenshots"], calls["brought"]), (1, 1))

    def test_check_preview_asks_the_guest_and_starts_the_application_again(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        black = runner.decode_png(png(40, 40, 6, [bytes((0, 0, 0, 255)) * 40] * 40, [0]))
        guest.screenshot = lambda: black
        answers = iter([foreground_text(CAMERA_RESUMED.replace("aperture", "trebuchet"), wakefulness="Awake")]
                       + [foreground_text()] * 4)
        base_shell = guest.shell

        def shell(command: str, timeout: float = 0) -> str:
            if command == runner.FOREGROUND_COMMAND:
                guest.commands.append(command)
                return next(answers)
            return base_shell(command, timeout)

        guest.shell = shell
        out = io.StringIO()
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), contextlib.redirect_stdout(out):
            runner.check_preview(guest, plan)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertEqual(len(starts), 2)
        self.assertIn("bringing it back", out.getvalue())
        # The application's own events are read before the second start, and printed with the line that says it left.
        events = [index for index, command in enumerate(guest.commands) if "FATAL EXCEPTION" in command]
        second_start = [index for index, command in enumerate(guest.commands) if command.startswith("am start")][1]
        self.assertEqual(len(events), 1)
        self.assertLess(events[0], second_start)
        self.assertIn("events of the camera application and its windows:", out.getvalue())
        self.assertIn("blocked preview (2 samples): mean 0.0, deviation 0.0", out.getvalue())


class FailureLogTests(unittest.TestCase):
    def test_the_camera_log_lines_are_printed_at_a_failure(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        commands: list[str] = []
        guest.shell = lambda command, timeout=0: (commands.append(command), "E V4l2FrameSource: a line\n")[1]
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        self.assertIn("camera log lines of the guest:\nE V4l2FrameSource: a line", err.getvalue())
        self.assertIn("logcat -d -b all | grep -a -E", commands[0])
        self.assertIn("vrend", commands[0])
        self.assertIn("grep -a -v -E 'SurfaceFlinger|adbd", commands[0])
        # Every screenshot allocates a buffer, and each allocation logs the allocator's start-up lines; with them
        # the last 120 lines of a failed run held nothing of the camera.
        for flood in ("minigbm:virtgpu_init", "minigbm:virgl_get_caps", "minigbm:virgl_add_combination"):
            self.assertIn(flood, commands[0])

    def test_a_guest_that_is_gone_does_not_hide_the_failure(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)

        def gone(command: str, timeout: float = 0) -> str:
            raise OSError("adb: device offline")

        guest.shell = gone
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        self.assertIn("no camera log lines: adb: device offline", err.getvalue())

    def test_what_was_in_front_is_printed_after_the_log(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        commands: list[str] = []
        answers = ["E CameraService: a line\n",
                   "topResumedActivity=ActivityRecord{x u0 org.lineageos.aperture/.CameraActivity}\n",
                   "I ActivityTaskManager: Displayed org.lineageos.aperture\n",
                   "E Aperture: its own line, which does not name the package\n"]
        guest.shell = lambda command, timeout=0: (commands.append(command), answers[len(commands) - 1])[1]
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        text = err.getvalue()
        self.assertLess(text.index("camera log lines of the guest"), text.index("in front at the failure"))
        self.assertLess(text.index("in front at the failure"), text.index("events of the camera application"))
        self.assertIn("in front at the failure:\ntopResumedActivity=ActivityRecord{x u0 org.lineageos.aperture", text)
        self.assertIn("events of the camera application and its windows:\nI ActivityTaskManager", text)
        self.assertIn("dumpsys activity activities", commands[1])
        self.assertIn("mCurrentFocus", commands[1])
        for what in ("FATAL EXCEPTION", "ANR in", "am_crash", "am_anr", "am_proc_died", "org.lineageos.aperture"):
            self.assertIn(what, commands[2])
        # The application's own lines name no package: they come by process id.
        self.assertIn("the camera application's own log lines (by process id):\nE Aperture: its own line", text)
        self.assertIn("pidof org.lineageos.aperture", commands[3])
        self.assertIn("logcat -d -b all --pid=$pid", commands[3])
        self.assertIn("grep -a -v -E", commands[3])

    def test_an_application_without_a_process_has_no_own_lines(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        guest.shell = lambda command, timeout=0: ""
        out = io.StringIO()
        runner.print_application_events(guest, out)
        self.assertIn("the camera application's own log lines (by process id):\n(none: no process)", out.getvalue())

    def test_a_guest_lost_after_the_log_keeps_the_log(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "blocked")
        guest = FakeGuest(plan)
        calls: list[str] = []

        def second_fails(command: str, timeout: float = 0) -> str:
            calls.append(command)
            if len(calls) == 2:
                raise OSError("adb: device offline")
            return "E CameraService: a line\n"

        guest.shell = second_fails
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            runner.print_camera_log(guest)
        self.assertIn("E CameraService: a line", err.getvalue())
        self.assertIn("no foreground state: adb: device offline", err.getvalue())
        self.assertIn("events of the camera application and its windows:", err.getvalue())


class HeldStillTests(unittest.TestCase):
    PLAN = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")

    def test_the_session_lines_of_the_launcher_are_read_in_order(self) -> None:
        text = ("Camera: camera 0: torch off, still flash automatic\n"
                "Camera: memory: capture session opened, footprint 3350 MiB\n"
                "Camera: memory: still delivered, footprint 3442 MiB\n"
                "Camera: memory: capture session closed, footprint 3367 MiB\n"
                "Camera: idle\n"
                "Camera: memory: capture session opened, footprint 3373 MiB\n"
                "Kernarium: memory tick footprint 2682.2 MiB capture session closed\n")
        self.assertEqual(runner.session_events(text), ["opened", "closed", "opened"])
        self.assertEqual(runner.session_events("Camera: streaming\n"), [])

    def test_the_preview_layer_is_the_newest_of_the_application(self) -> None:
        text = ("Display 0\n"
                "SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity](BLAST)#99\n"
                "SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity](BLAST)#138\n"
                "SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity](BLAST)#7\n")
        self.assertEqual(runner.parse_preview_layer(text),
                         "SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity](BLAST)#138")
        self.assertIsNone(runner.parse_preview_layer("Display 0\n"))

    def test_the_preview_layer_keeps_the_hash_the_list_gives_it(self) -> None:
        name = "SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity]"
        text = ("RequestedLayerState{Background for 7c01cb3 " + name + "#137 parentId=135 relativeParentId=128}\n"
                "RequestedLayerState{7c01cb3 " + name + "(BLAST)#136 parentId=135}\n"
                "RequestedLayerState{7c01cb3 " + name + "#135 parentId=134 relativeParentId=128 z=-2}\n")
        self.assertEqual(runner.parse_preview_layer(text), "7c01cb3 " + name + "(BLAST)#136")

    def test_the_present_times_of_a_layer(self) -> None:
        text = ("16666666\n"
                "100  1000000000  1010000000\n"
                "100  1033000000  1040000000\n"
                "100  9223372036854775807  1050000000\n"
                "0  0  0\n"
                "100  1033000000  1040000000\n"
                "100  1066000000  1070000000\n")
        self.assertEqual(runner.parse_present_times(text), [1_000_000_000, 1_033_000_000, 1_066_000_000])
        self.assertEqual(runner.parse_present_times("16666666\n"), [])

    def test_the_longest_gap(self) -> None:
        self.assertAlmostEqual(runner.longest_gap_seconds([0, 33_000_000, 1_133_000_000, 1_166_000_000]), 1.1)
        self.assertEqual(runner.longest_gap_seconds([5]), 0.0)
        self.assertEqual(runner.longest_gap_seconds([]), 0.0)

    # As the camera service prints it: the stream's header, its consumer, a second line with its surface, then its size.
    DUMP = ("    Stream[0]: Output\n      Consumer name: f279c8e SurfaceView[org.lineageos.aperture/org.lineageos.aperture.CameraActivity]#1\n"
            "    Stream[0]: Surface Id: 0\n      State: 4\n      Dims: 1008 x 756, format 0x23, dataspace 0x0\n      Frames produced: 12\n"
            "    Stream[1]: Output\n      Consumer name: ImageReader-4032x3024f21m4-2209-0\n"
            "    Stream[1]: Surface Id: 0\n      State: 4\n      Dims: 4032 x 3024, format 0x21, dataspace 0x8c20000\n      Frames produced: 3\n"
            "    Stream[2]: Output\n      Consumer name: ImageReader-4032x3024f20m4-2209-1\n"
            "    Stream[2]: Surface Id: 0\n      Dims: 4032 x 3024, format 0x20, dataspace 0x0\n")

    def test_the_still_streams_of_the_camera_service(self) -> None:
        self.assertEqual(runner.parse_still_streams(self.DUMP), {1, 2})
        # A stream without a Dims line of its own does not borrow the next stream's.
        self.assertEqual(runner.parse_still_streams("    Stream[3]: Output\n      Frames produced: 5\n"
                                                   "    Stream[4]: Output\n      Dims: 1 x 1, format 0x21\n"), {4})
        self.assertEqual(runner.parse_still_streams("no streams\n"), set())

    @staticmethod
    def report(second: float, stream: int, frames: str) -> str:
        return (f"   {second:.3f}  2292  2379 W GCH_ResultDispatcher: [Cam0_ResultDispatcher] PrintTimeoutMessages: "
                f"normal_request_map: pending buffer of stream {stream} for {frames} ready 0\n")

    MARKER = "   120.000  2423  2423 I CamHeldStill: held-still-1-77\n"

    def test_the_oldest_pending_buffer_says_whether_a_still_held_the_pipeline(self) -> None:
        report, marker = self.report, self.MARKER
        # The preview's frames wait behind the still's (the frame before them): the still holds the pipeline.
        held = marker + report(121.0, 0, "frames [1166 - 1169] (4 frames)") + report(121.0, 1, "frame 1165") + \
            report(121.5, 0, "frames [1166 - 1171] (6 frames)")
        self.assertEqual(runner.pipeline_head(held, "held-still-1-77", {1}),
                         (True, "the oldest pending buffer is stream 1's frame 1165"))
        self.assertEqual(runner.pending_phase(held, "held-still-1-77", {1})[0], "inside")
        # The still's frame comes after preview frames that the stop caught: the stop came before the still's request.
        missed = marker + report(121.0, 0, "frames [647 - 648] (2 frames)") + report(121.0, 1, "frame 649") + \
            report(121.0, 0, "frames [650 - 651] (2 frames)")
        self.assertEqual(runner.pipeline_head(missed, "held-still-1-77", {1}),
                         (False, "the oldest pending buffer is stream 0's frame 647; "
                                 "the still's buffer (stream 1, frame 649) waits behind it"))
        self.assertEqual(runner.pending_phase(missed, "held-still-1-77", {1})[0], "before")
        # No still's buffer is pending at all: the still's request was over (or not yet sent).
        after = marker + report(121.0, 0, "frames [905 - 908] (4 frames)") + report(121.0, 0, "frame 910")
        self.assertEqual(runner.pending_phase(after, "held-still-1-77", {1}),
                         ("after", "the oldest pending buffer is stream 0's frame 905; no buffer of a still is pending"))
        self.assertEqual(runner.pipeline_head(after, "held-still-1-77", {1})[0], False)
        # A later report (half a second on) does not change the verdict; a report before the marker is another round's.
        later = report(100.0, 1, "frame 1") + marker + report(121.0, 0, "frame 5") + report(121.6, 1, "frame 2")
        self.assertEqual(runner.pipeline_head(later, "held-still-1-77", {1})[0], False)
        self.assertEqual(runner.pipeline_head(marker, "held-still-1-77", {1})[0], None)
        self.assertEqual(runner.pipeline_head("", "held-still-1-77", {1})[0], None)

    def test_the_buffers_of_the_oldest_frame_are_one_request(self) -> None:
        """A capture request that has the preview and the JPEG has one frame number: its buffers hold the pipeline
        together, whichever stream number comes first."""
        same = self.MARKER + self.report(121.0, 0, "frame 904") + self.report(121.0, 1, "frame 904") + \
            self.report(121.0, 0, "frames [905 - 908] (4 frames)")
        self.assertEqual(runner.pipeline_head(same, "held-still-1-77", {1}),
                         (True, "the oldest pending buffers are of frame 904 (streams 0 and 1)"))
        # And the same frame without a still's buffer among them is no still's request.
        none = self.MARKER + self.report(121.0, 0, "frame 904") + self.report(121.0, 2, "frame 904") + self.report(121.0, 1, "frame 910")
        self.assertEqual(runner.pending_phase(none, "held-still-1-77", {1})[0], "before")

    def test_a_report_is_the_rounds_only_within_its_stop(self) -> None:
        """The report that follows a stop comes as the guest resumes, a stop's length and a little after the marker.
        One that comes much later is another event's, not this round's evidence."""
        late = self.MARKER + self.report(140.0, 1, "frame 904")
        # No bound given: it was the first report after the marker.
        self.assertEqual(runner.pipeline_head(late, "held-still-1-77", {1})[0], True)
        self.assertEqual(runner.pipeline_head(late, "held-still-1-77", {1}, 2.2),
                         (None, "the result dispatcher reported no pending buffer inside the round's interval (2.2 s after the marker)"))
        inside = self.MARKER + self.report(122.0, 1, "frame 904")
        self.assertEqual(runner.pipeline_head(inside, "held-still-1-77", {1}, 2.2)[0], True)
        # A late report after an early one does not join it.
        both = self.MARKER + self.report(121.0, 0, "frame 900") + self.report(140.0, 1, "frame 904")
        self.assertEqual(runner.pending_phase(both, "held-still-1-77", {1}, 2.2)[0], "after")

    def run_step(self, guest) -> str:
        out = io.StringIO()
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), mock.patch.object(runner, "STILL_SECONDS", 0.05), \
                contextlib.redirect_stdout(out):
            runner.check_still_keeps_preview(guest, self.PLAN)
        return out.getvalue()

    def guest(self) -> "FakeGuest":
        guest = FakeGuest(self.PLAN)
        guest.launcher = "Camera: memory: capture session opened, footprint 3350 MiB\n"
        return guest

    def test_three_stills_that_hold_the_pipeline_leave_the_stream_open(self) -> None:
        guest = self.guest()
        printed = self.run_step(guest)
        self.assertEqual(guest.frozen, [runner.HELD_STILL_SECONDS] * runner.HELD_STILL_ROUNDS)
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), runner.HELD_STILL_ROUNDS)
        self.assertIn("3 stops of 1 s inside a still's request: the preview's stream stayed open (no capture session closed)",
                      printed)
        self.assertEqual(printed.count("its stop overlapped a still's request: counted"), 3)

    def test_a_session_the_host_closed_behind_the_stills_is_a_failure(self) -> None:
        guest = self.guest()
        guest.closes_on_freeze = True
        with self.assertRaisesRegex(runner.Failure, r"held still 1: the host closed its capture session \(1 so far\) behind a "
                                    "still that held the request pipeline: the camera HAL closed the preview's stream"):
            self.run_step(guest)

    def test_a_session_closed_before_the_step_is_not_counted(self) -> None:
        guest = self.guest()
        guest.launcher += "Camera: memory: capture session closed, footprint 3466 MiB\n"
        self.run_step(guest)

    def test_a_launcher_that_prints_no_session_lines_proves_nothing(self) -> None:
        guest = self.guest()
        guest.launcher = "Camera: streaming\n"
        with self.assertRaisesRegex(runner.Failure, "the launcher has printed no capture session line, so a check that none "
                                    "closed proves nothing"):
            self.run_step(guest)
        self.assertEqual(guest.frozen, [])

    def test_a_preview_that_stays_away_longer_than_the_stop_is_a_failure(self) -> None:
        guest = self.guest()
        guest.away_more = runner.HELD_STILL_RESUME_SECONDS + 0.4
        with self.assertRaisesRegex(runner.Failure, r"held still 1: the preview was away 1\.9\d s, more than the stop "
                                    r"\(1\.00 s\) and 0\.5 s"):
            self.run_step(guest)

    def test_a_preview_that_comes_back_within_the_bound_passes(self) -> None:
        guest = self.guest()
        guest.away_more = runner.HELD_STILL_RESUME_SECONDS - 0.2
        self.run_step(guest)

    def test_the_bound_follows_the_stop_as_it_lasted(self) -> None:
        # A sleep that overshoots makes a stop of 1.6 s: the preview away 1.6 s is the stop, not a defect.
        guest = self.guest()
        guest.stop_extra = 0.6
        printed = self.run_step(guest)
        self.assertIn("stop 1.60 s", printed)
        # And 0.6 s more than that stop is a defect whatever the stop was.
        guest = self.guest()
        guest.stop_extra = 0.6
        guest.away_more = runner.HELD_STILL_RESUME_SECONDS + 0.2
        with self.assertRaisesRegex(runner.Failure, r"more than the stop \(1\.60 s\)"):
            self.run_step(guest)

    def test_a_round_that_gives_no_still_is_a_failure(self) -> None:
        guest = self.guest()
        guest.stills = lambda: {"IMG_old.jpg"}
        with self.assertRaisesRegex(runner.Failure, "held still 1: a still in the camera folder: no new still"):
            self.run_step(guest)

    def test_no_preview_layer_and_no_frames_are_failures(self) -> None:
        guest = self.guest()
        guest.layer = None
        with self.assertRaisesRegex(runner.Failure, "no preview layer"):
            self.run_step(guest)
        guest = self.guest()
        guest.present = []
        with self.assertRaisesRegex(runner.Failure, "held still 1: the preview layer has presented no frame"):
            self.run_step(guest)

    def test_no_still_output_stream_is_a_failure(self) -> None:
        guest = self.guest()
        guest.has_still_stream = False
        with self.assertRaisesRegex(runner.Failure, "lists no still output stream"):
            self.run_step(guest)

    def test_only_a_stop_that_overlapped_a_still_counts(self) -> None:
        guest = self.guest()
        guest.head_streams = [0, 2, 1, None, 1, 1]
        printed = self.run_step(guest)
        self.assertEqual(len(guest.frozen), 6)
        self.assertEqual(printed.count("its stop came before the still's request began: not counted"), 1)
        self.assertEqual(printed.count("its stop came after the still's request ended: not counted"), 1)
        self.assertEqual(printed.count("no evidence that its stop overlapped a still's request: not counted"), 1)
        self.assertEqual(printed.count("its stop overlapped a still's request: counted"), 3)
        # The delay is found from what the rounds showed: up after a stop that came before the request, down after one
        # that came after it (and by half the step after a change of direction), and kept for a hit and for no report.
        self.assertEqual(re.findall(r"s, ([0-9.]+) s after the key", printed), ["0.20", "0.24", "0.22", "0.22", "0.22", "0.22"])

    def test_a_step_that_never_overlapped_a_still_is_a_failure_not_a_pass(self) -> None:
        for heads in ([0], [2], [None]):
            guest = self.guest()
            guest.head_streams = heads
            with self.assertRaisesRegex(runner.Failure, rf"only 0 of {runner.HELD_STILL_ATTEMPTS} stops overlapped a still's "
                                        r"request \(3 are needed\)"):
                self.run_step(guest)
            self.assertEqual(len(guest.frozen), runner.HELD_STILL_ATTEMPTS)

    def test_the_delay_search(self) -> None:
        start, step = runner.HELD_STILL_DELAY_START, runner.HELD_STILL_DELAY_STEP
        low, high = runner.HELD_STILL_DELAY_RANGE
        self.assertEqual(runner.next_held_still_delay(start, "before", step, 0), (start + step, step, 1))
        self.assertEqual(runner.next_held_still_delay(start, "after", step, 0), (start - step, step, -1))
        self.assertEqual(runner.next_held_still_delay(start, "inside", step, 1), (start, step, 1))
        self.assertEqual(runner.next_held_still_delay(start, None, step, -1), (start, step, -1))
        # A change of direction halves the step, down to the smallest.
        delay, new_step, direction = runner.next_held_still_delay(start, "after", step, 1)
        self.assertEqual((round(delay, 3), new_step, direction), (round(start - step / 2, 3), step / 2, -1))
        small = runner.HELD_STILL_DELAY_SMALLEST_STEP
        self.assertEqual(runner.next_held_still_delay(start, "before", small, -1)[1], small)
        # The delay stays in its range.
        self.assertEqual(runner.next_held_still_delay(low, "after", step, -1)[0], low)
        self.assertEqual(runner.next_held_still_delay(high, "before", step, 1)[0], high)

    def run_window(self, centre: float, width: float, jitter=(0.0,)) -> tuple["WindowGuest", str]:
        guest = WindowGuest(self.PLAN, centre, width, jitter)
        guest.launcher = "Camera: memory: capture session opened, footprint 3350 MiB\n"
        return guest, self.run_step_with_clock(guest)

    def test_the_step_finds_a_window_that_the_fixed_delays_missed(self) -> None:
        """The still's own request is a window of a tenth of a second at 0.15 to 0.3 s after the key, which moves with
        the size of the still and by 0.03 s from key to key: the step finds it from the rounds' evidence."""
        for centre in (0.12, 0.20, 0.26, 0.31, 0.42):
            for width in (0.09, 0.04):
                with self.subTest(centre=centre, width=width):
                    guest, printed = self.run_window(centre, width)
                    self.assertIn("3 stops of 1 s inside a still's request", printed)
                    self.assertLessEqual(len(guest.frozen), runner.HELD_STILL_ATTEMPTS)
        # A window of a hundredth of a second, found by the halved steps.
        guest, printed = self.run_window(0.26, 0.012)
        self.assertIn("3 stops of 1 s inside a still's request", printed)
        # And with the window moving by 0.03 s from key to key.
        guest, printed = self.run_window(0.22, 0.09, jitter=(0.0, 0.03, -0.03, 0.02, -0.02))
        self.assertIn("3 stops of 1 s inside a still's request", printed)
        self.assertLessEqual(len(guest.frozen), runner.HELD_STILL_ATTEMPTS)

    def test_a_window_the_delays_never_reach_is_a_failure(self) -> None:
        guest = WindowGuest(self.PLAN, 5.0, 0.09, (0.0,))   # a still request five seconds after the key: out of range
        guest.launcher = "Camera: memory: capture session opened, footprint 3350 MiB\n"
        with self.assertRaisesRegex(runner.Failure, r"only 0 of 16 stops overlapped"):
            self.run_step_with_clock(guest)

    def test_a_report_that_comes_long_after_the_stop_is_no_evidence(self) -> None:
        guest = self.guest()
        guest.report_after = 30.0
        with self.assertRaisesRegex(runner.Failure, rf"only 0 of {runner.HELD_STILL_ATTEMPTS} stops overlapped"):
            self.run_step(guest)

    def test_a_key_command_that_returns_late_leaves_the_frame_history_short(self) -> None:
        """An adb call that takes eight seconds while the preview runs: SurfaceFlinger's 128 frames no longer reach
        back to the sample taken before it. The round says its observation does not cover the interval, and does
        not report a freeze that it did not see."""
        guest = LateKeyGuest(self.PLAN)
        guest.launcher = "Camera: memory: capture session opened, footprint 3350 MiB\n"
        with self.assertRaisesRegex(runner.Failure, r"held still 1: inconclusive: the observation does not cover the round: "
                                    r"the frame history no longer reaches back to the sample taken before the key"):
            self.run_step_with_clock(guest)
        # A late file or a short late command that leaves the history whole is no such case.
        guest = LateKeyGuest(self.PLAN, late=1.0)
        guest.launcher = "Camera: memory: capture session opened, footprint 3350 MiB\n"
        self.assertIn("3 stops of 1 s inside a still's request", self.run_step_with_clock(guest))

    def test_a_late_still_does_not_lose_the_frames_of_the_stop(self) -> None:
        """SurfaceFlinger keeps the last 128 frames of a layer: with the still file there eight seconds after the key
        and a 30 fps preview, a read after the still is found has lost the frames around the stop. The step reads
        them every half second from the resume on."""
        guest = RingGuest(self.PLAN)
        guest.launcher = "Camera: memory: capture session opened, footprint 3350 MiB\n"
        guest.still_after = 8.0
        printed = self.run_step_with_clock(guest)
        self.assertIn("3 stops of 1 s inside a still's request", printed)
        self.assertRegex(printed, r"the preview was away 1\.0\d s")

    def run_step_with_clock(self, guest) -> str:
        out = io.StringIO()
        with mock.patch.object(runner.time, "sleep", guest.advance), mock.patch.object(runner, "STILL_SECONDS", 60), \
                contextlib.redirect_stdout(out):
            runner.check_still_keeps_preview(guest, self.PLAN)
        return out.getvalue()

    def test_the_step_runs_in_live_mode_after_the_first_still(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        live = source[source.index("check_still(guest, plan, before)\n    if plan.mode == \"live\":"):]
        self.assertLess(live.index("check_still_keeps_preview(guest, plan)"), live.index("check_video(guest, plan)"))


class RingGuest(FakeGuest):
    """A guest whose clock runs: a preview of 30 frames a second that SurfaceFlinger remembers the last 128 of, a stop
    that takes the frames away for as long as it lasts, and a still file that appears `still_after` seconds after the key."""

    def __init__(self, plan) -> None:
        super().__init__(plan)
        self.clock = 0.0
        self.keys: list[float] = []
        self.stops: list[tuple[float, float]] = []
        self.still_after = 0.3
        self.present = []

    def advance(self, seconds: float) -> None:
        self.clock += seconds

    def shell(self, command: str, timeout: float = 0) -> str:
        if command == f"input keyevent {runner.CAMERA_KEYCODE}":
            self.commands.append(command)
            self.keys.append(self.clock)
            return ""
        return super().shell(command, timeout)

    def freeze(self, seconds: float) -> float:
        self.frozen.append(seconds)
        self.stops.append((self.clock, self.clock + seconds))
        self.clock += seconds
        return seconds

    def present_times(self, layer: str) -> list[int]:
        frames = []
        for index in range(int(self.clock * 30) + 1):
            when = 5.0 + index / 30
            if when > self.clock + 5.0:
                break
            if any(start + 5.0 <= when < end + 5.0 for start, end in self.stops):
                continue
            frames.append(round(when * 1e9))
        return frames[-127:]

    def stills(self) -> set[str]:
        files = {"IMG_old.jpg"}
        files.update(f"IMG_new{number}.jpg" for number, key in enumerate(self.keys) if self.clock >= key + self.still_after)
        return files


class LateKeyGuest(RingGuest):
    """A guest whose camera key command returns `late` seconds late while the preview goes on."""

    def __init__(self, plan, late: float = 8.0) -> None:
        super().__init__(plan)
        self.late = late

    def shell(self, command: str, timeout: float = 0) -> str:
        result = super().shell(command, timeout)
        if command == f"input keyevent {runner.CAMERA_KEYCODE}":
            self.advance(self.late)
        return result


class WindowGuest(RingGuest):
    """A guest whose still's request lasts `width` seconds, `centre` seconds after the key command returns (moved by
    `jitter`, one entry for each key in turn): a stop inside it has the still's buffer oldest in the dispatcher's
    report, one before it the preview's, one after it no still's buffer at all."""

    def __init__(self, plan, centre: float, width: float, jitter=(0.0,)) -> None:
        super().__init__(plan)
        self.centre, self.width, self.jitter = centre, width, jitter
        self.head_streams = []

    def freeze(self, seconds: float) -> float:
        waited = self.clock - self.keys[-1]
        middle = self.centre + self.jitter[(len(self.keys) - 1) % len(self.jitter)]
        if waited < middle - self.width / 2:
            self.head_streams.append(0)
        elif waited <= middle + self.width / 2:
            self.head_streams.append(1)
        else:
            self.head_streams.append(2)
        return super().freeze(seconds)


class VideoAndFrontFlowTests(unittest.TestCase):
    def run_step(self, step, plan, guest) -> None:
        with mock.patch.object(runner.time, "sleep", lambda seconds: None):
            step(guest, plan)

    def test_the_video_step_records_with_the_shutter_key_in_video_mode(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        sleeps: list[float] = []
        with mock.patch.object(runner.time, "sleep", sleeps.append):
            runner.check_video(guest, plan)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertEqual(starts, [f"am start -a {runner.VIDEO_ACTION} --ez {runner.FRONT_EXTRA} false"])
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 2)
        self.assertIn(f"am force-stop {runner.CAMERA_PACKAGE}", guest.commands)
        self.assertEqual(runner.VIDEO_HOLD_SECONDS, 5)
        self.assertIn(runner.VIDEO_HOLD_SECONDS, sleeps)

    def test_the_video_step_prints_the_average_frame_rate(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, fps=29.97, audio=sound(4.0))))
        self.assertRegex(out.getvalue(), r"video: new\.mp4 4\.00 s, 119 frames over 3\.97 s = 30\.0 fps average")
        self.assertRegex(out.getvalue(), r"audio: 188 AAC frames = 4\.01 s of audio over a 4\.01 s time line at 48000 Hz, "
                                         r"stretch 1\.000")

    def test_a_movie_below_the_frame_rate_floor_fails_after_it_is_printed(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaisesRegex(runner.Failure, r"averages 15\.0 fps .*below 20"):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, fps=15.0)))
        self.assertIn("= 15.0 fps average", out.getvalue())

    def test_a_movie_without_a_sample_table_fails(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        with self.assertRaisesRegex(runner.Failure, "no sample table"):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.0, samples=False)))

    def test_a_movie_that_is_too_short_or_has_no_video_fails(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for data, what in ((movie(0.4), "lasts 0.40 s"), (movie(3.0, video=False), "no video track"),
                           (b"junk", "not a finished movie")):
            with self.subTest(what=what), self.assertRaisesRegex(runner.Failure, what):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=data))

    def test_the_audio_of_a_recording_must_hold_what_its_time_stamps_span(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for stretch in (0.96, 1.0, 1.007, 1.05):
            with self.subTest(stretch=stretch):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(3.0, audio=sound(3.0, stretch))))
        for stretch in (1.06, 1.29, 1.39, 4.74):
            out = io.StringIO()
            with self.subTest(stretch=stretch), contextlib.redirect_stdout(out), self.assertRaisesRegex(
                    runner.Failure, rf"holds 3\.01 s of audio under a [0-9.]+ s time line \(stretch {stretch}\d*, above 1\.05\)"
                                    r".*plays the clip too fast"):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(3.0, audio=sound(3.0, stretch))))
            self.assertIn(f"stretch {stretch}", out.getvalue())  # printed before the check
        # The other way: a time line shorter than the audio it holds is as wrong.
        for stretch in (0.94, 0.7):
            with self.subTest(stretch=stretch), self.assertRaisesRegex(
                    runner.Failure, rf"stretch {stretch}\d*, below 0\.95\): more audio than its time stamps span"):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(3.0, audio=sound(3.0, stretch))))
        self.assertEqual((runner.AUDIO_STRETCH_MINIMUM, runner.AUDIO_STRETCH_MAXIMUM), (0.95, 1.05))

    def test_a_jump_in_the_audio_table_fails_even_when_the_stretch_is_small(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        # 12 s of audio with one step of 0.55 s: a stretch of 1.046, and a jump of more than half a second.
        data = movie(12.55, audio=sound(12.0, jump=(100, 0.55)))
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaisesRegex(
                runner.Failure, r"jumps 0\.57 s between two samples \(above 0\.5 s\)"):
            self.run_step(runner.check_video, plan, FakeGuest(plan, movies=data))
        self.assertRegex(out.getvalue(), r"stretch 1\.04\d; longest step of its sample times 0\.57\d* s")
        self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(12.4, audio=sound(12.0, jump=(100, 0.4)))))

    def test_the_audio_must_end_with_the_video(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for seconds, what in ((3.0, r"ends \+1\.0\d s after the video"), (5.1, r"ends -1\.0\d s after the video")):
            with self.subTest(video=seconds), self.assertRaisesRegex(runner.Failure, what + r" \(more than 0\.25 s either way\)"):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(seconds, audio=sound(4.0))))
        self.run_step(runner.check_video, plan, FakeGuest(plan, movies=movie(4.2, audio=sound(4.0))))

    def test_a_recording_without_audio_or_its_sample_table_fails(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        for data, what in ((movie(4.0), "has no audio track"),
                           (movie(4.0, audio=sound(3.0, samples=False)), "no sample table for its audio track")):
            with self.subTest(what=what), self.assertRaisesRegex(runner.Failure, what):
                self.run_step(runner.check_video, plan, FakeGuest(plan, movies=data))

    def test_no_new_movie_fails_with_the_query(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        guest.rows = {"1": {"_id": "1"}}
        guest.shell = lambda command, timeout=0: ""  # the shutter key records nothing
        with mock.patch.object(runner, "VIDEO_SECONDS", 0.2), self.assertRaisesRegex(
                runner.Failure, "MediaStore lists no new movie"):
            self.run_step(runner.check_video, plan, guest)

    PHONE = "bf-large-flash-manual-concurrent-front2160"

    def stall_step(self, data, **changes):
        """The audio stall step on a guest that returns `data` as its recording; returns the guest and the output."""
        plan = runner.make_plan(self.PHONE, "phone", "live")
        guest = FakeGuest(plan, movies=data)
        for name, value in changes.items():
            setattr(guest, name, value)
        out = io.StringIO()
        try:
            with mock.patch.object(runner.time, "sleep", lambda seconds: None), contextlib.redirect_stdout(out):
                runner.check_audio_stall(guest, plan)
        except runner.Failure as failure:
            failure.guest, failure.out = guest, out.getvalue()
            raise
        return guest, out.getvalue()

    def test_the_stall_step_holds_the_sound_device_three_times_while_the_recording_runs(self) -> None:
        guest, out = self.stall_step(movie(12.0, audio=sound(12.0)))
        self.assertEqual(guest.audio_held, [runner.STALL_SECONDS] * runner.DISTURB_COUNT)
        self.assertEqual((runner.STALL_SECONDS, runner.DISTURB_COUNT), (1.5, 3))
        self.assertEqual(guest.frozen, [])  # the guest itself stands still never
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 2)
        self.assertRegex(out, r"audio capture runs: the record thread has read \d+, then \d+ frames")
        self.assertRegex(out, r"stall 1: the sound device's thread held 1\.50 s from \d+\.\d s after the start key")
        self.assertRegex(out, r"video with the sound device held 3 x 1\.5 s: new\.mp4 12\.00 s, 360 frames")
        self.assertRegex(out, r"the HAL made 201600 frames of silence up in 3 stall\(s\)")

    def test_the_stall_step_needs_the_audio_capture_before_it_holds_anything(self) -> None:
        # Checked before the first hold: a record thread in standby, without a client, absent or reading nothing
        # is a recording whose audio the stall could not have touched.
        for state in ("standby", "idle", "none", "stuck"):
            with self.subTest(state=state), mock.patch.object(runner, "AUDIO_CAPTURE_SECONDS", 0.05), \
                    mock.patch.object(runner.time, "monotonic", mock.Mock(side_effect=itertools.count(0, 0.02))):
                with self.assertRaisesRegex(runner.Failure, r"audio capture: .* \(after 0 s\)") as raised:
                    self.stall_step(movie(12.0, audio=sound(12.0)), capture_state=state)
                self.assertEqual(raised.exception.guest.audio_held, [])
        self.stall_step(movie(12.0, audio=sound(12.0)))

    def test_the_stall_step_fails_for_the_input_that_does_not_make_the_stall_up(self) -> None:
        # Three stalls of 1.5 s under an input that makes only its own frames up: 4.5 s of time line more than audio.
        with self.assertRaisesRegex(runner.Failure, r"stretch 1\.3\d*, above 1\.05\).*plays the clip too fast"):
            self.stall_step(movie(15.0, audio=sound(11.5, 1.3)), makes_up=False)
        # A jump with a small stretch is its own failure.
        with self.assertRaisesRegex(runner.Failure, r"jumps 0\.92 s between two samples"):
            self.stall_step(movie(24.9, audio=sound(24.0, jump=(300, 0.9))))

    def test_the_stall_step_fails_when_the_hal_does_not_say_it_made_the_stall_up(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"made 0 frames of silence up in 0 stall\(s\) during 3 stalls of 1\.5 s "
                                                    r"\(at least 108000 frames wanted\)") as raised:
            self.stall_step(movie(12.0, audio=sound(12.0)), makes_up=False)
        self.assertIn("the HAL made 0 frames of silence up in 0 stall(s)", raised.exception.out)
        # Only this recording's lines count: what the log held before it is not the stalls'.
        before = ("11-14 21:00:00.000   200   300 W AHAL_StreamAlsaMonoPipe: readInput: the device delivers again, "
                  "after 90000 frames of silence made up\n")
        with self.assertRaisesRegex(runner.Failure, "made 0 frames of silence up"):
            self.stall_step(movie(12.0, audio=sound(12.0)), makes_up=False, stream_text=before)

    def test_a_stall_that_stops_the_video_too_cannot_be_judged(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"the video has a gap of 1\.53 s \(above 0\.5 s\): the guest stood still"):
            self.stall_step(movie(12.0, audio=sound(12.0), video_jump=(100, 1.5)))

    def test_the_stall_step_does_not_run_without_the_library(self) -> None:
        guest, out = self.stall_step(movie(12.0, audio=sound(12.0)), hold_directory=None)
        self.assertEqual((guest.audio_held, guest.commands), ([], []))
        self.assertIn("audio stall: not run (no --audio-hold-library)", out)

    def freeze_step(self, data):
        plan = runner.make_plan(self.PHONE, "phone", "live")
        guest = FakeGuest(plan, movies=data)
        out = io.StringIO()
        try:
            with mock.patch.object(runner.time, "sleep", lambda seconds: None), contextlib.redirect_stdout(out):
                runner.check_video_freeze(guest, plan)
        except runner.Failure as failure:
            failure.guest, failure.out = guest, out.getvalue()
            raise
        return guest, out.getvalue()

    def test_the_freeze_step_stops_the_emulator_three_times_and_wants_equal_gaps(self) -> None:
        # The video has a gap of 1.5 s and so has the audio (a step of its sample times), and both end together.
        guest, out = self.freeze_step(movie(12.0, fps=15.0, audio=sound(12.0, jump=(100, 1.5)), video_jump=(40, 1.5)))
        self.assertEqual(guest.frozen, [runner.FREEZE_SECONDS] * runner.DISTURB_COUNT)
        self.assertEqual(guest.audio_held, [])
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 2)
        self.assertRegex(out, r"freeze 3: the emulator stopped 1\.5 s from \d+\.\d s after the start key")
        self.assertRegex(out, r"longest gap in the video 1\.5\d+ s")
        self.assertRegex(out, r"longest step of its sample times 1\.52\d* s, ends -0\.011 s after the video")

    def test_the_freeze_step_fails_for_frames_that_come_in_a_burst_after_the_thaw(self) -> None:
        # The burst leaves the gaps equal and runs the audio on past the video by as long as the freeze was.
        with self.assertRaisesRegex(runner.Failure, r"the audio ends \+1\.39 s after the video \(more than 0\.25 s either way\)"):
            self.freeze_step(movie(10.6, fps=15.0, audio=sound(12.0, jump=(100, 1.5)), video_jump=(40, 1.5)))

    def test_the_freeze_step_fails_for_a_freeze_the_video_does_not_show_or_gaps_that_differ(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"the freeze left a gap of only 0\.03 s in the video"):
            self.freeze_step(movie(12.0, audio=sound(12.0)))
        with self.assertRaisesRegex(runner.Failure, r"audio's longest step is 0\.02 s and the video's 1\.57 s .*same gap in both tracks"):
            self.freeze_step(movie(12.0, fps=15.0, audio=sound(12.0), video_jump=(40, 1.5)))
        with self.assertRaisesRegex(runner.Failure, r"audio's longest step is 1\.47 s and the video's 1\.07 s"):
            self.freeze_step(movie(12.0, fps=15.0, audio=sound(12.0, jump=(100, 1.45)), video_jump=(40, 1.0)))
        with self.assertRaisesRegex(runner.Failure, "has no audio track"):
            self.freeze_step(movie(12.0, video_jump=(40, 1.5)))

    def test_the_whole_run_names_the_reasons_of_the_old_input(self) -> None:
        # What the r26 input leaves in a recording with three stalls of 1.5 s: stamps with no audio behind them. And
        # the input of the first proof image in the freeze step: the gaps equal, the burst after them.
        with self.assertRaisesRegex(runner.Failure, r"stretch 1\.\d+, above 1\.05\)"):
            self.stall_step(movie(12.0, audio=sound(8.2, 1.46)))
        with self.assertRaisesRegex(runner.Failure, r"the audio ends \+[0-9.]+ s after the video"):
            self.freeze_step(movie(7.0, fps=15.0, audio=sound(8.4, jump=(100, 1.5)), video_jump=(40, 1.5)))

    def test_the_guest_stops_its_emulator_process_and_continues_it_whatever_happens(self) -> None:
        for fail in (False, True):
            events: list[tuple] = []
            guest = runner.Guest("adb", "127.0.0.1:1", mock.Mock(pid=4242), Path("emulator.log"), Path("private"))

            def sleep(seconds: float) -> None:
                events.append(("sleep", seconds))
                if fail:
                    raise KeyboardInterrupt

            with self.subTest(fail=fail), mock.patch.object(runner.os, "kill", lambda pid, signal: events.append(
                    ("kill", pid, signal))), mock.patch.object(runner.time, "sleep", sleep):
                if fail:
                    with self.assertRaises(KeyboardInterrupt):
                        guest.freeze(1.5)
                else:
                    guest.freeze(1.5)
            self.assertEqual(events, [("kill", 4242, runner.signal.SIGSTOP), ("sleep", 1.5),
                                      ("kill", 4242, runner.signal.SIGCONT)])

    def test_the_laptop_records_with_its_front_camera(self) -> None:
        plan = runner.make_plan("front", "laptop", "live")
        guest = FakeGuest(plan)
        self.run_step(runner.check_video, plan, guest)
        self.assertIn(f"am start -a {runner.VIDEO_ACTION} --ez {runner.FRONT_EXTRA} true", guest.commands)

    def test_the_front_step_opens_camera_1_and_takes_a_still(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        guest = FakeGuest(plan)
        self.run_step(runner.check_front, plan, guest)
        self.assertIn(f"am start -a android.media.action.STILL_IMAGE_CAMERA --ez {runner.FRONT_EXTRA} true",
                      guest.commands)
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 1)

    def test_the_front_step_fails_when_the_application_holds_the_back_camera(self) -> None:
        plan = runner.make_plan("bf-large-flash-manual-concurrent-front2160", "phone", "live")
        with mock.patch.object(runner, "PREVIEW_SECONDS", 0.2), self.assertRaisesRegex(
                runner.Failure, r"holding camera 1 \(front\).*it holds \['0'\]"):
            self.run_step(runner.check_front, plan, FakeGuest(plan, front_holds=False))

    def test_a_front_only_profile_has_no_second_facing(self) -> None:
        plan = runner.make_plan("front", "laptop", "live")
        guest = FakeGuest(plan)
        self.run_step(runner.check_front, plan, guest)
        self.assertEqual(guest.commands, [])

    def test_the_hal_log_check(self) -> None:
        guest = FakeGuest(None)
        runner.check_hal_log(guest)
        guest.log = HAL_LOG
        with self.assertRaisesRegex(runner.Failure, "6 failed buffer lines; the first: .*every buffer"):
            runner.check_hal_log(guest)

    def test_the_new_steps_run_in_live_mode_only(self) -> None:
        source = SCRIPT.read_text()
        run_checks = source[source.index("def run_checks"):source.index("def cameraserver_restart_skip")]
        # The switches follow the other live steps, which see the state they always saw.
        self.assertRegex(run_checks, r'if plan.mode == "live":\n\s+check_still_keeps_preview\(guest, plan\)\n'
                         r'\s+check_video\(guest, plan\)\n\s+check_audio_stall\(guest, plan\)\n\s+check_video_freeze\(guest, plan\)\n'
                         r'\s+check_front\(guest, plan\)\n\s+check_mode_switch\(guest, plan\)\n'
                         r'\s+check_hal_log\(guest\)')


class ModeSwitchTests(unittest.TestCase):
    LAYOUT = "bf-large-flash-manual-concurrent-front2160"

    def plan(self, rounds: int = 4, profile: str = "phone"):
        return runner.make_plan(self.LAYOUT if profile == "phone" else "front", profile, "live", rounds)

    def run_step(self, guest, plan=None) -> str:
        """The step with its sleeps skipped and its deadlines short, so that a wait that fails ends at once."""
        self.printed = io.StringIO()
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner, "SWITCH_OBSERVE_SECONDS", 0.05), contextlib.redirect_stdout(self.printed):
            runner.check_mode_switch(guest, plan or guest.plan)
        return self.printed.getvalue()

    def guest(self, rounds: int = 4, profile: str = "phone"):
        return FakeGuest(self.plan(rounds, profile))

    # -- What counts as a refusal.

    def test_the_denial_and_the_refusal_are_exactly_those_lines(self) -> None:
        self.assertEqual(runner.switch_failures(SWITCH_DENIAL), [(runner.SWITCH_DENIAL_KIND, SWITCH_DENIAL)])
        self.assertEqual(runner.switch_failures(SWITCH_REFUSAL), [(runner.SWITCH_REFUSAL_KIND, SWITCH_REFUSAL)])
        both = runner.switch_failures(f"{SWITCH_QUIET}{SWITCH_DENIAL}\n{SWITCH_REFUSAL}\n")
        self.assertEqual([what for what, _ in both], [runner.SWITCH_DENIAL_KIND, runner.SWITCH_REFUSAL_KIND])
        self.assertEqual(runner.SWITCH_DENIAL_KIND, "an SELinux denial naming hal_camera_default")
        # The kernel's own copy of the record (the process name as the tag, no comm field) and a security
        # context with categories are the same denial.
        copy = SWITCH_DENIAL.replace("I auditd  :", "W C3Dev-0-ReqQueu:").replace('comm="C3Dev-0-ReqQueu" ', "")
        self.assertEqual(len(runner.switch_failures(copy)), 1)
        categories = SWITCH_DENIAL.replace("hal_camera_default:s0", "hal_camera_default:s0:c512,c768")
        self.assertEqual(len(runner.switch_failures(categories)), 1)

    def test_a_correct_image_and_unrelated_events_are_not_refusals(self) -> None:
        self.assertEqual(runner.switch_failures(SWITCH_QUIET), [])
        self.assertEqual(runner.switch_failures(""), [])
        for line in SWITCH_UNRELATED_DENIALS:
            self.assertEqual(runner.switch_failures(line), [], line)
        # One field of the denial at a time: any other source, target, class or permission, or a grant.
        for old, new in (("scontext=u:r:hal_camera_default:s0", "scontext=u:r:cameraserver:s0"),
                         ("scontext=u:r:hal_camera_default:s0", "scontext=u:r:hal_camera_default_x:s0"),
                         ("tcontext=u:r:hal_graphics_composer_default:s0", "tcontext=u:r:surfaceflinger:s0"),
                         ("tcontext=u:r:hal_graphics_composer_default:s0",
                          "tcontext=u:r:hal_graphics_composer_default_x:s0"),
                         ("tclass=fd", "tclass=file"), ("{ use }", "{ read }"), ("{ use }", "{ ioctl }"),
                         ("denied", "granted")):
            self.assertEqual(runner.switch_failures(SWITCH_DENIAL.replace(old, new)), [], new)
        # A permission set that holds `use` among others is still the denial.
        self.assertEqual(len(runner.switch_failures(SWITCH_DENIAL.replace("{ use }", "{ read use }"))), 1)

    def test_the_stream_errors_of_a_stopped_application_do_not_fail_a_correct_image(self) -> None:
        plan = self.plan(2)
        guest = FakeGuest(plan)
        for number in (1, 2):
            guest.switch_logs[number] = SWITCH_QUIET + "\n".join(SWITCH_UNRELATED_DENIALS) + "\n"
        output = self.run_step(guest)
        self.assertIn("mode switch 2 of 2", output)

    # -- The flow.

    def test_every_round_starts_the_photo_mode_then_the_video_mode_and_reads_the_log_after_its_marker(self) -> None:
        guest = self.guest()
        sleeps: list[float] = []
        output = io.StringIO()
        with mock.patch.object(runner.time, "sleep", sleeps.append), contextlib.redirect_stdout(output):
            runner.check_mode_switch(guest, guest.plan)
        self.assertEqual(sleeps.count(runner.SWITCH_HOLD_SECONDS), runner.SWITCH_ROUNDS)
        self.assertEqual(sleeps.count(runner.SWITCH_SETTLE_SECONDS), runner.SWITCH_ROUNDS)
        self.assertEqual(sleeps.count(runner.SWITCH_DRAIN_SECONDS), 1)
        stop = f"am force-stop {runner.CAMERA_PACKAGE}"
        photo = f"am start -a android.media.action.STILL_IMAGE_CAMERA --ez {runner.FRONT_EXTRA} false"
        video = f"am start -a {runner.VIDEO_ACTION} --ez {runner.FRONT_EXTRA} false"
        activity = [command for command in guest.commands if command.startswith("am ")]
        self.assertEqual(activity, [stop, photo, video] * runner.SWITCH_ROUNDS)
        self.assertEqual(len([c for c in guest.commands if c.startswith(f"log -t {runner.SWITCH_MARKER_TAG} ")]),
                         runner.SWITCH_ROUNDS)
        # One read per round (the switch is seen at once) and the read after the last round.
        self.assertEqual(guest.log_reads, runner.SWITCH_ROUNDS + 1)
        self.assertIn(f"mode switch {runner.SWITCH_ROUNDS} of {runner.SWITCH_ROUNDS}: photo activity paused, video "
                      "activity resumed, capture before and after; no refused capture request", output.getvalue())

    def test_the_rounds_are_the_plans(self) -> None:
        for rounds in (1, 2, 12):
            guest = self.guest(rounds)
            output = self.run_step(guest)
            markers = [c for c in guest.commands if c.startswith(f"log -t {runner.SWITCH_MARKER_TAG} ")]
            self.assertEqual(len(markers), rounds)
            self.assertIn(f"photo to video switch {rounds} of {rounds}'", markers[-1])
            self.assertIn(f"mode switch {rounds} of {rounds}:", output)

    def test_the_animations_are_off_for_the_switches_and_restored_after_them(self) -> None:
        for before in ({}, {"window_animation_scale": "0.5", "transition_animation_scale": "1.5",
                            "animator_duration_scale": "2.0"}):
            guest = self.guest()
            guest.settings = dict(before)
            self.run_step(guest)
            self.assertEqual(len(guest.settings_at_video_start), runner.SWITCH_ROUNDS)
            for seen in guest.settings_at_video_start:
                self.assertEqual(seen, {key: "0" for key in runner.ANIMATION_SCALES})
            self.assertEqual(guest.settings, before)

    def test_the_laptop_switches_with_its_front_camera(self) -> None:
        guest = self.guest(2, "laptop")
        self.run_step(guest)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertTrue(all(f"{runner.FRONT_EXTRA} true" in command for command in starts), starts)

    # -- A round passes only when the switch was seen (live capture before, the pause, the resume, capture after).

    def test_a_round_without_the_switch_fails_by_name(self) -> None:
        for events, missing in (((False, False), "no pause of org.lineageos.aperture.CameraActivity and no resume of "
                                                 "org.lineageos.aperture.VideoCamera"),
                                ((True, False), "no resume of org.lineageos.aperture.VideoCamera"),
                                ((False, True), "no pause of org.lineageos.aperture.CameraActivity")):
            guest = self.guest()
            guest.events[1] = events
            with self.assertRaisesRegex(runner.Failure, r"^no mode switch observed in the photo to video switch 1 of 4: "
                                        r"the log after its marker shows " + re.escape(missing)):
                self.run_step(guest)

    def test_the_events_of_an_earlier_round_do_not_make_a_later_one(self) -> None:
        guest = self.guest()
        guest.events[3] = (False, False)
        with self.assertRaisesRegex(runner.Failure, "no mode switch observed in the photo to video switch 3 of 4"):
            self.run_step(guest)
        self.assertEqual(guest.video_starts, 3)

    def test_capture_must_be_running_before_the_switch_and_after_it(self) -> None:
        guest = self.guest()
        guest.stalled = {"photo"}
        with self.assertRaisesRegex(runner.Failure, r"^live capture \(photo, before switch 1\): the frame counters stand at"):
            self.run_step(guest)
        self.assertEqual(guest.video_starts, 0)
        guest = self.guest()
        guest.stalled = {"video"}
        with self.assertRaisesRegex(runner.Failure, r"^live capture \(video, after the photo to video switch 1 of 4\)"):
            self.run_step(guest)

    def test_the_application_must_hold_the_camera_and_show_the_chart_before_and_after(self) -> None:
        with mock.patch.object(runner, "PREVIEW_SECONDS", 0.05):
            guest = self.guest()
            guest.no_camera_in = {"photo"}
            with self.assertRaisesRegex(runner.Failure, r"^the camera application holding camera 0 \(photo\)"):
                self.run_step(guest)
            self.assertEqual(guest.video_starts, 0)
            guest = self.guest()
            guest.no_camera_in = {"video"}
            with self.assertRaisesRegex(runner.Failure, r"^the camera application holding camera 0 \(video\)"):
                self.run_step(guest)
            self.assertEqual(guest.video_starts, 1)

    def test_the_frame_counters_of_the_dump_are_summed(self) -> None:
        dump = "Stream[3]: Output\n  Frames produced: 815, last timestamp: 20 ns\nStream[4]: Output\n  Frames produced: 0, x\n"
        self.assertEqual(runner.frames_produced(dump), 815)
        self.assertEqual(runner.frames_produced("nothing"), 0)

    # -- The refusal is read wherever it lands: between two reads and after the last.

    def test_the_exact_denial_and_refusal_fail_the_round_and_name_both(self) -> None:
        for lines in (f"{SWITCH_REFUSAL}\n{SWITCH_DENIAL}\n", f"{SWITCH_DENIAL}\n{SWITCH_REFUSAL}\n"):
            guest = self.guest()
            guest.switch_logs[3] = lines
            with self.assertRaisesRegex(
                    runner.Failure, r'^an SELinux denial naming hal_camera_default and the camera service\'s "Unable to '
                    r'submit capture request" since the photo to video switch 2 of 4 \(read after the photo to video '
                    r"switch 3 of 4\): .*avc:  denied  \{ use \}.* \| .*Unable to submit"):
                self.run_step(guest)

    def test_a_denial_alone_and_a_refusal_alone_fail_the_step(self) -> None:
        for line, kind in ((SWITCH_DENIAL, "an SELinux denial naming hal_camera_default"),
                           (SWITCH_REFUSAL, 'the camera service\'s "Unable to submit capture request"')):
            guest = self.guest(2, "laptop")
            guest.switch_logs[1] = line + "\n"
            with self.assertRaisesRegex(runner.Failure, "^" + re.escape(kind) + " since the photo to video switch 1 of 2"):
                self.run_step(guest)

    def test_a_refusal_that_lands_between_two_reads_is_read_by_the_next(self) -> None:
        guest = self.guest()
        guest.late_logs[1] = SWITCH_DENIAL + "\n" + SWITCH_REFUSAL + "\n"
        with self.assertRaisesRegex(runner.Failure, r"since the photo to video switch 1 of 4 \(read after the photo to "
                                    r"video switch 2 of 4\)"):
            self.run_step(guest)
        # The same log without the late lines passes.
        self.run_step(self.guest())

    def test_a_refusal_that_lands_after_the_last_round_is_read_at_the_end(self) -> None:
        guest = self.guest(2)
        guest.late_logs[2] = SWITCH_REFUSAL + "\n"
        with self.assertRaisesRegex(runner.Failure, r"in the final read after the last switch \(photo to video switch 2 "
                                    r"of 2\): .*Unable to submit"):
            self.run_step(guest)
        self.assertEqual(guest.log_reads, 3)
        self.run_step(self.guest(2))

    def test_a_log_that_lost_the_previous_marker_cannot_be_judged(self) -> None:
        guest = self.guest()
        guest.wrap_after_read = {1}
        with self.assertRaisesRegex(runner.Failure, "the log has no line for the photo to video switch 1 of 4: it was "
                                    "lost or never written, so the photo to video switches cannot be judged"):
            self.run_step(guest)
        guest = self.guest()
        guest.no_markers = True
        with self.assertRaisesRegex(runner.Failure, "the log has no line for the photo to video switch 1 of 4"):
            self.run_step(guest)

    def test_what_the_log_held_before_the_first_marker_is_not_judged(self) -> None:
        guest = self.guest()
        guest.full_log += SWITCH_DENIAL + "\n" + SWITCH_REFUSAL + "\n"
        self.run_step(guest)

    def test_a_long_run_reads_only_from_the_previous_marker(self) -> None:
        # Twelve rounds outlast a log buffer that keeps three rounds: lines older than the previous marker are
        # gone from the log, and the step still passes because it never needs them.
        guest = self.guest(12)
        original = guest.shell

        def trimming(command, timeout=0):
            text = original(command, timeout)
            if command == "logcat -d -b all":
                markers = [m.end() for m in re.finditer(r"CamSwitch: photo to video switch \d+ of 12\n", text)]
                return text[markers[-2] - 60:] if len(markers) > 2 else text
            return text
        guest.shell = trimming
        self.run_step(guest)

    # -- The animation scales come back on every exit.

    def test_the_animations_are_restored_when_a_switch_fails_too(self) -> None:
        guest = self.guest()
        guest.settings = {"window_animation_scale": "0.5"}
        guest.switch_logs[2] = SWITCH_REFUSAL + "\n"
        with self.assertRaises(runner.Failure):
            self.run_step(guest)
        self.assertEqual(len(guest.settings_at_video_start), 2)
        self.assertEqual(guest.settings, {"window_animation_scale": "0.5"})

    def test_a_failed_second_write_leaves_no_scale_at_zero(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings put global transition_animation_scale 0"}
        with self.assertRaisesRegex(runner.Failure, "injected"):
            self.run_step(guest)
        self.assertEqual(guest.settings, {key: "1" for key in runner.ANIMATION_SCALES})
        self.assertEqual(guest.video_starts, 0)

    def test_a_failed_read_leaves_the_scales_alone(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings get global transition_animation_scale"}
        with self.assertRaisesRegex(runner.Failure, "injected"):
            self.run_step(guest)
        self.assertEqual(guest.settings, {key: "1" for key in runner.ANIMATION_SCALES})

    def test_a_restore_that_fails_fails_the_step_and_the_others_are_still_restored(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings put global window_animation_scale 1"}
        with self.assertRaisesRegex(runner.Failure, "the animation scale window_animation_scale was not restored"):
            self.run_step(guest)
        self.assertEqual(guest.settings, {"window_animation_scale": "0", "transition_animation_scale": "1",
                                          "animator_duration_scale": "1"})

    def test_a_restore_that_fails_does_not_hide_the_failure_of_the_switch(self) -> None:
        guest = self.guest()
        guest.settings = {key: "1" for key in runner.ANIMATION_SCALES}
        guest.failing = {"settings put global window_animation_scale 1"}
        guest.switch_logs[1] = SWITCH_DENIAL + "\n"
        with self.assertRaisesRegex(runner.Failure, "an SELinux denial naming"):
            self.run_step(guest)
        self.assertIn("the animation scale window_animation_scale was not restored", self.printed.getvalue())
        self.assertEqual(guest.settings["transition_animation_scale"], "1")
        self.assertEqual(guest.settings["animator_duration_scale"], "1")


# `dumpsys sensor_privacy` of a guest started with Camera off and the side keys: the camera (sensor 2) is
# blocked by the hardware toggle (type 2); a software toggle is type 1.
SENSOR_PRIVACY_BLOCKED = """SENSOR PRIVACY MANAGER STATE (dumpsys sensor_privacy)
{
  storage_implementation=com.android.server.sensorprivacy.SensorPrivacyStateControllerImpl
  users={
    user_id=0
    sensors={
      sensor=2
      toggles={
        toggle_type=2
        state_type=1
        last_change=20299
      }
    }
  }
}
"""
SENSOR_PRIVACY_LIVE = """SENSOR PRIVACY MANAGER STATE (dumpsys sensor_privacy)
{
  storage_implementation=com.android.server.sensorprivacy.SensorPrivacyStateControllerImpl
}
"""

# The interface dump of the platform's dialog over the camera application, trimmed to the nodes that matter, and
# the dump of the camera application alone.
SENSOR_DIALOG_DUMP = (
    "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation=\"0\">"
    '<node index="0" text="" resource-id="android:id/content" class="android.widget.FrameLayout" '
    'package="com.android.systemui" bounds="[0,0][720,1280]">'
    '<node index="1" text="Unblock device camera?" '
    'resource-id="com.android.systemui:id/sensor_use_started_title_message" '
    'class="android.widget.TextView" package="com.android.systemui" bounds="[0,0][1,1]" />'
    '<node index="0" text="This unblocks access for all apps and services allowed to use your camera." '
    'resource-id="android:id/message" class="android.widget.TextView" package="com.android.systemui" />'
    '<node index="1" text="Unblock" resource-id="android:id/button1" class="android.widget.Button" '
    'package="com.android.systemui" /></node></hierarchy>')
CAMERA_APPLICATION_DUMP = (
    "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation=\"0\">"
    '<node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="org.lineageos.aperture">'
    '<node index="0" text="PHOTO" resource-id="org.lineageos.aperture:id/photoModeButton" '
    'class="android.widget.Button" package="org.lineageos.aperture" />'
    '<node index="1" text="Unblock" resource-id="" class="android.widget.Button" '
    'package="org.example.other" /></node></hierarchy>')
DIALOG_EVENT = ("11-15 03:41:29.579   602   622 I wm_create_activity: [0,132003343,10,"
                "com.android.systemui/.sensorprivacy.SensorUseStartedActivity,NULL,NULL,NULL,277086208]")
CAMERA_EVENT = ("11-15 03:41:20.100   602   617 I wm_create_activity: [0,5,10,"
                "org.lineageos.aperture/.CameraActivity,NULL,NULL,NULL,0]")
# The line the patched sensor privacy service logs when it withholds the reminder of a blocked camera.
REMINDER_LOG = ("11-15 03:53:57.190   602   617 I SensorPrivacyService: kernarium: no reminder for a camera that "
                "the hardware toggle blocks (org.lineageos.aperture)")
OTHER_REMINDER_LOG = REMINDER_LOG.replace("org.lineageos.aperture", "org.example.other")


class SensorDialogTests(unittest.TestCase):
    """The camera switch as Android holds it, and the platform's Unblock dialog that must not appear."""

    def test_the_toggles_of_the_sensor_privacy_service_are_read_from_its_dump(self) -> None:
        self.assertEqual(runner.parse_sensor_privacy(SENSOR_PRIVACY_BLOCKED), [(0, 2, 2, 1)])
        self.assertEqual(runner.parse_sensor_privacy(SENSOR_PRIVACY_LIVE), [])
        self.assertEqual(runner.parse_sensor_privacy(""), [])
        both = SENSOR_PRIVACY_BLOCKED.replace("toggle_type=2", "toggle_type=1\n        state_type=1\n"
                                              "        toggle_type=2", 1)
        self.assertEqual(runner.parse_sensor_privacy(both), [(0, 2, 1, 1), (0, 2, 2, 1)])

    def test_only_the_hardware_toggle_of_the_camera_is_the_lens_cover(self) -> None:
        self.assertTrue(runner.camera_hardware_blocked([(0, 2, 2, 1)]))
        self.assertFalse(runner.camera_hardware_blocked([]))
        self.assertFalse(runner.camera_hardware_blocked([(0, 2, 1, 1)]), "the software toggle")
        self.assertFalse(runner.camera_hardware_blocked([(0, 1, 2, 1)]), "the microphone")
        self.assertFalse(runner.camera_hardware_blocked([(0, 2, 2, 2)]), "a toggle that is not enabled")

    def test_the_dialog_is_found_by_its_title_and_button_in_systemui_alone(self) -> None:
        found = runner.sensor_dialog_nodes(SENSOR_DIALOG_DUMP)
        self.assertEqual(len(found), 2)
        self.assertIn("com.android.systemui:id/sensor_use_started_title_message 'Unblock device camera?'", found)
        self.assertIn("android:id/button1 'Unblock'", found)
        self.assertEqual(runner.sensor_dialog_nodes(CAMERA_APPLICATION_DUMP), [])
        with self.assertRaisesRegex(ValueError, "not XML"):
            runner.sensor_dialog_nodes("<hierarchy><node")

    def test_the_dialog_the_platform_created_is_counted_by_name(self) -> None:
        self.assertEqual(runner.sensor_dialog_starts(""), 0)
        self.assertEqual(runner.sensor_dialog_starts("\n".join((CAMERA_EVENT, DIALOG_EVENT))), 1)
        self.assertEqual(runner.sensor_dialog_starts("\n".join((DIALOG_EVENT, DIALOG_EVENT))), 2)
        self.assertEqual(runner.sensor_dialog_starts(
            "I wm_restart_activity: [0,1,10,com.android.systemui/.sensorprivacy.SensorUseStartedActivity]"), 0)

    def test_the_services_log_line_counts_for_the_camera_application_only(self) -> None:
        self.assertEqual(runner.withheld_reminders(""), 0)
        self.assertEqual(runner.withheld_reminders(REMINDER_LOG), 1)
        self.assertEqual(runner.withheld_reminders("\n".join((REMINDER_LOG, REMINDER_LOG))), 2)
        self.assertEqual(runner.withheld_reminders(OTHER_REMINDER_LOG), 0, "another application")
        self.assertEqual(runner.withheld_reminders(REMINDER_LOG.replace("no reminder", "a reminder")), 0)

    def test_the_line_the_runner_waits_for_is_the_line_the_framework_patch_adds_and_the_verifier_requires(self) -> None:
        verifier = (REPOSITORY / "android/lineage-24/tools/verify-target-files.py").read_text(encoding="utf-8")
        self.assertIn(f'b"{runner.SENSOR_REMINDER_LINE}"', verifier)

    def test_a_blocked_session_always_has_the_side_keys(self) -> None:
        parser = runner.build_parser()
        self.assertTrue(runner.wants_hardware_input(parser.parse_args(["--mode", "blocked"])))
        self.assertFalse(runner.wants_hardware_input(parser.parse_args(["--mode", "live"])))
        self.assertTrue(runner.wants_hardware_input(parser.parse_args(["--mode", "live", "--hardware-input"])))
        self.assertFalse(runner.wants_hardware_input(parser.parse_args(["--mode", "none"])))
        self.assertFalse(runner.wants_hardware_input(parser.parse_args(["--mode", "restart"])))
        options = parser.parse_args([])
        self.assertEqual(options.vnc_port, runner.slot_ports.slot_port(runner.VNC_OFFSET, runner.DEFAULT_VNC_PORT))

    def test_the_restart_mode_has_no_side_keys_and_so_no_capture(self) -> None:
        # The restart mode is a Camera-on session of two guests; the capture and its absence checks belong to a
        # session with the side keys, and a restart that asked for them would leave its second guest uncaptured.
        err = io.StringIO()
        with mock.patch.object(sys, "argv", ["run_android_camera.py", "--mode", "restart", "--hardware-input"]), \
                contextlib.redirect_stderr(err):
            self.assertEqual(runner.main(), 2)
        self.assertIn("restart mode runs without the side keys", err.getvalue())

    def test_an_importer_that_builds_its_own_options_starts_the_guest_without_the_side_keys(self) -> None:
        # start_guest reads the side keys from the options main() prepared; options of an importer without them
        # (the blocked session of a CTS tool) give the offscreen guest of before, not an AttributeError.
        source = inspect.getsource(runner.start_guest)
        self.assertIn('getattr(options, "hardware_input", False)', source)
        self.assertIn('getattr(options, "vnc_port"', source)
        self.assertNotIn("options.hardware_input", source)

    def test_the_launcher_has_the_option_the_runner_adds(self) -> None:
        source = (REPOSITORY / "darwin/app/macos/main.cpp").read_text(encoding="utf-8")
        self.assertIn(f'"{runner.HARDWARE_INPUT_OPTION}"', source)

    class Guest:
        """A guest whose captured session log is one text (main and events lines together)."""

        def __init__(self, privacy: str, dump: str, log: str = "", hardware_input: bool = True) -> None:
            self.privacy, self.dump, self.log = privacy, dump, log
            self.hardware_input = hardware_input
            self.asked: list[str] = []

        def alive(self) -> None:
            pass

        def sensor_privacy(self) -> str:
            self.asked.append("privacy")
            return self.privacy

        def interface_dump(self) -> str:
            self.asked.append("dump")
            return self.dump

        def session_log(self) -> str:
            self.asked.append("log")
            return self.log

    @staticmethod
    def lines(*parts: str) -> str:
        return "\n".join(parts)

    def run_step(self, mode: str, guest, step=None) -> str:
        plan = runner.make_plan("bf-small-flash", "phone", mode)
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            (step or runner.check_sensor_dialog)(guest, plan)
        return printed.getvalue()

    def test_a_blocked_session_holds_the_toggle_and_the_service_withholds_the_reminder(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, self.lines(CAMERA_EVENT, REMINDER_LOG))
        printed = self.run_step("blocked", guest)
        self.assertIn("hardware toggle set; no dialog on the screen", printed)
        self.assertIn("reminder withheld 1 time(s)", printed)
        self.assertEqual(guest.asked[:2], ["privacy", "dump"])

    def test_a_live_session_holds_no_toggle_and_the_service_withholds_nothing(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_LIVE, CAMERA_APPLICATION_DUMP, CAMERA_EVENT)
        printed = self.run_step("live", guest)
        self.assertIn("hardware toggle not set", printed)
        self.assertIn("reminder withheld 0 time(s)", printed)

    def test_the_dialog_of_the_image_before_the_patch_fails_a_blocked_session(self) -> None:
        # What the accepted release shows: the toggle is set, the dialog is on the screen and was started, and the
        # service never says it withheld anything.
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, SENSOR_DIALOG_DUMP, self.lines(CAMERA_EVENT, DIALOG_EVENT))
        with self.assertRaisesRegex(runner.Failure, "sensor privacy dialog is on the screen of the blocked"):
            self.run_step("blocked", guest)

    def test_a_dialog_that_was_started_and_left_still_fails(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, self.lines(DIALOG_EVENT, REMINDER_LOG))
        with self.assertRaisesRegex(runner.Failure, "SensorUseStartedActivity was started 1 time"):
            self.run_step("blocked", guest)

    def test_no_dialog_and_no_log_line_is_not_a_pass_for_a_blocked_session(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, CAMERA_EVENT)
        with self.assertRaisesRegex(runner.Failure, "did not log 'kernarium: no reminder for a camera"):
            self.run_step("blocked", guest)
        guest = self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, self.lines(CAMERA_EVENT, OTHER_REMINDER_LOG))
        with self.assertRaisesRegex(runner.Failure, "did not log"):
            self.run_step("blocked", guest)

    def test_the_toggle_must_follow_the_mode(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "toggle is not set in a blocked session"):
            self.run_step("blocked", self.Guest(SENSOR_PRIVACY_LIVE, CAMERA_APPLICATION_DUMP,
                                                self.lines(CAMERA_EVENT, REMINDER_LOG)))
        with self.assertRaisesRegex(runner.Failure, "toggle is set in a live session"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_BLOCKED, CAMERA_APPLICATION_DUMP, CAMERA_EVENT))

    def test_a_live_session_that_shows_the_dialog_or_withholds_a_reminder_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "dialog is on the screen of the live"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_LIVE, SENSOR_DIALOG_DUMP, CAMERA_EVENT))
        with self.assertRaisesRegex(runner.Failure, "withheld the camera's reminder 1 time"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_LIVE, CAMERA_APPLICATION_DUMP,
                                             self.lines(CAMERA_EVENT, REMINDER_LOG)))

    def test_the_end_checks_are_absence_checks_over_the_whole_capture(self) -> None:
        # The dialog that came after the first look is in the capture and fails the end check; the positive line is
        # not asked for again (it counts once seen), and the screen is not asked at all.
        blocked = self.Guest(SENSOR_PRIVACY_BLOCKED, "", self.lines(CAMERA_EVENT, REMINDER_LOG, DIALOG_EVENT))
        with self.assertRaisesRegex(runner.Failure, "SensorUseStartedActivity was started 1 time"):
            self.run_step("blocked", blocked, runner.check_sensor_history)
        self.assertEqual(blocked.asked, ["log"])
        # A blocked session whose capture holds no line passes the end check: only the wait and the first look need it.
        self.run_step("blocked", self.Guest(SENSOR_PRIVACY_BLOCKED, "", CAMERA_EVENT), runner.check_sensor_history)
        with self.assertRaisesRegex(runner.Failure, "withheld the camera's reminder 1 time"):
            self.run_step("live", self.Guest(SENSOR_PRIVACY_LIVE, "", self.lines(CAMERA_EVENT, REMINDER_LOG)),
                          runner.check_sensor_history)

    def test_a_guest_without_the_side_keys_says_so_and_asks_nothing(self) -> None:
        guest = self.Guest(SENSOR_PRIVACY_LIVE, "", hardware_input=False)
        self.assertIn("not run, the guest has no side keys", self.run_step("blocked", guest))
        self.run_step("blocked", guest, runner.check_sensor_history)
        self.assertEqual(guest.asked, [])

    def waiting(self, guest, ticks):
        """Runs the wait on a clock that advances `ticks` seconds at every reading."""
        clock = itertools.count(step=ticks)
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner.time, "monotonic", lambda: float(next(clock))):
            runner.wait_for_withheld_reminder(guest)

    def test_the_wait_ends_when_the_line_comes_and_has_a_deadline(self) -> None:
        guest = self.Guest("", "")
        guest.session_log = lambda: (guest.asked.append("log"), REMINDER_LOG if len(guest.asked) >= 4 else "")[1]
        self.waiting(guest, 1)
        self.assertEqual(len(guest.asked), 4)
        silent = self.Guest("", "", OTHER_REMINDER_LOG)
        with self.assertRaisesRegex(runner.Failure, f"did not withhold.*after {runner.SENSOR_REMINDER_SECONDS} s"):
            self.waiting(silent, 1)
        self.assertGreater(len(silent.asked), 10)

    def test_a_look_that_completes_after_the_deadline_does_not_count(self) -> None:
        # The line is there at the last look, but the look began before the deadline and ended after it.
        late = self.Guest("", "")
        calls = []
        clock = itertools.count()

        def session_log() -> str:
            calls.append(1)
            for _ in range(runner.SENSOR_REMINDER_SECONDS + 5):
                next(clock)
            return REMINDER_LOG

        late.session_log = session_log
        with mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                mock.patch.object(runner.time, "monotonic", lambda: float(next(clock))):
            with self.assertRaisesRegex(runner.Failure, "did not withhold"):
                runner.wait_for_withheld_reminder(late)
        self.assertEqual(len(calls), 1)

    def test_a_capture_that_has_ended_is_a_failure_and_not_a_missing_line(self) -> None:
        guest = self.Guest("", "")

        def session_log() -> str:
            raise runner.Failure("the log capture (pid 123) ended early with status 1")

        guest.session_log = session_log
        with self.assertRaisesRegex(runner.Failure, "the log capture .* ended early"):
            self.waiting(guest, 1)
        with self.assertRaisesRegex(runner.Failure, "the log capture .* ended early"):
            self.run_step("blocked", guest, runner.check_sensor_history)

    def test_the_options_name_the_directory_the_capture_is_kept_in(self) -> None:
        options = runner.build_parser().parse_args(["--output-dir", "/tmp/example"])
        self.assertEqual(options.output_dir, Path("/tmp/example"))
        self.assertIsNone(runner.build_parser().parse_args([]).output_dir)


class SessionCaptureTests(unittest.TestCase):
    """The continuous capture of the guest's logs: a process the runner starts, records and stops."""

    def script(self, directory: Path, body: str) -> str:
        path = directory / "fake-adb"
        path.write_text("#!/bin/sh\n" + body + "\n", encoding="utf-8")
        path.chmod(0o755)
        return str(path)

    def test_the_log_is_kept_in_a_file_with_the_pid_recorded_and_the_process_stopped_by_the_runner(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            adb = self.script(directory, "echo \"$@\"; echo first; exec sleep 60")
            capture = runner.SessionCapture(adb, "serial-1", directory / "capture.log")
            capture.start()
            try:
                pid = int(capture.pid_path.read_text())
                self.assertEqual(pid, capture.process.pid)
                self.assertIn("-s serial-1 logcat -b main -b events -v threadtime", capture.text())
                self.assertIn("first", capture.text())
                capture.check()
            finally:
                capture.stop()
            self.assertIsNotNone(capture.process.poll())
            with self.assertRaisesRegex(runner.Failure, f"the log capture \\(pid {pid}\\) ended early"):
                capture.text()

    def test_a_capture_that_ends_early_is_a_failure_of_its_own(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            adb = self.script(directory, "echo one line; exit 3")
            capture = runner.SessionCapture(adb, "serial-1", directory / "capture.log")
            capture.start()
            capture.process.wait(10)
            with self.assertRaisesRegex(runner.Failure, r"ended early with status 3; its last output: 'one line"):
                capture.text()

    def test_a_capture_that_writes_nothing_is_a_failure_and_is_stopped(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            adb = self.script(directory, "exec sleep 60")
            capture = runner.SessionCapture(adb, "serial-1", directory / "capture.log")
            with mock.patch.object(runner, "CAPTURE_START_SECONDS", 1):
                with self.assertRaisesRegex(runner.Failure, "wrote nothing in 1 s"):
                    capture.start()
            self.assertIsNotNone(capture.process.poll())

    def test_the_stop_ends_only_the_process_it_started(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            adb = self.script(directory, "echo line; exec sleep 60")
            other = subprocess.Popen(["sleep", "60"])
            try:
                capture = runner.SessionCapture(adb, "serial-1", directory / "capture.log")
                capture.start()
                capture.stop()
                self.assertIsNone(other.poll())
            finally:
                other.kill()
                other.wait()

    def test_the_failure_print_names_the_capture_and_its_sensor_lines(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            capture = runner.SessionCapture("adb", "serial-1", directory / "capture.log")
            capture.path.write_text("\n".join((CAMERA_EVENT, REMINDER_LOG, DIALOG_EVENT)), encoding="utf-8")
            capture.pid_path.write_text("123\n", encoding="utf-8")
            guest = SensorDialogTests.Guest("", "")
            guest.capture = capture
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                runner.print_sensor_capture(guest)
            self.assertIn("capture pid 123", err.getvalue())
            self.assertIn("kernarium: no reminder", err.getvalue())
            self.assertIn("SensorUseStartedActivity", err.getvalue())
            self.assertNotIn(".CameraActivity", err.getvalue())


class RestartTests(unittest.TestCase):
    """The restart mode: a guest restarted over the colour data a back still taught lists the cameras it listed
    before. The accepted r27 image lost every such lens: its HAL rejected a RAW camera whose static metadata lacked
    the lens shading map mode ON, and a camera it rejects while it builds its list is never listed again."""

    TAUGHT = "bf-small-flash-manual-raw"
    PLAIN = "bf-small-flash-manual"
    IDS = ["0", "1", "3", "4"]

    def dump(self, ids: list[str], raw_in: set[str]) -> str:
        """`dumpsys media.camera` of a phone: the devices, then the characteristics of each (the front camera is 1)."""
        text = "".join(f'   Device {n} maps to "{device}"\n' for n, device in enumerate(ids))
        for device in ids:
            front = device == "1"
            capabilities = "BACKWARD_COMPATIBLE MANUAL_SENSOR READ_SENSOR_SETTINGS" + (" RAW" if device in raw_in else "")
            text += static_info(device, "FRONT" if front else "BACK", capabilities, "FALSE" if front else "TRUE",
                                "OFF AUTO CONTINUOUS_PICTURE" if front else "OFF AUTO MACRO CONTINUOUS_PICTURE",
                                raw_metadata() if device in raw_in else "")
        return text

    class Guest:
        def __init__(self, sku: str, plan, dump: str) -> None:
            self.sku, self.plan, self.dump = sku, plan, dump

        def alive(self) -> None:
            pass

        def prop(self, name: str) -> str:
            return {runner.camera_layouts.SKU_PROPERTY: self.sku,
                    runner.camera_layouts.MEDIA_PROPERTY: self.plan.media_variant,
                    "ro.vendor.camera.config": "v4l2"}[name]

        def features(self) -> set[str]:
            return set(self.plan.features)

        def media_camera(self) -> str:
            return self.dump

    def check(self, ids: list[str], raw_in: set[str], sku: str | None = None) -> None:
        plan = runner.make_plan(self.TAUGHT, "phone", "live")
        guest = self.Guest(sku or self.TAUGHT, plan, self.dump(ids, raw_in))
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "RESTART_LIST_SECONDS", 0.05), \
                contextlib.redirect_stdout(io.StringIO()):
            runner.check_restart_keeps_cameras(guest, plan, list(self.IDS))

    def test_the_same_cameras_with_a_raw_one_pass(self) -> None:
        self.check(self.IDS, {"0", "3"})

    def test_a_lens_that_went_missing_after_the_restart_is_named(self) -> None:
        # What r27 did: the calibrated lenses were rejected at boot and Android never listed them.
        with self.assertRaisesRegex(runner.Failure, r"EmulatedCameraDeviceInfo.*: cameras \['1'\], not the "
                                    r"\['0', '1', '3', '4'\] of the session before: missing \['0', '3', '4'\], extra \[\]"):
            self.check(["1"], set())

    def test_an_extra_camera_is_not_the_same_list(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"extra \['5'\]"):
            self.check(self.IDS + ["5"], {"0"})

    def test_a_restart_without_a_raw_camera_exercised_nothing(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "no camera states the RAW capability"):
            self.check(self.IDS, set())

    def test_a_host_that_composed_the_untaught_layout_says_so(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "after the restart: the guest booted with the layout 'bf-small-flash-manual', "
                                    "not 'bf-small-flash-manual-raw' .*teach no colour data"):
            self.check(self.IDS, {"0"}, sku=self.PLAIN)

    # ---- the metadata a RAW camera states, read from the dump

    def test_the_dump_gives_the_maps_and_the_lens_shading_state_of_a_raw_camera(self) -> None:
        text = static_info("3", "BACK", "BACKWARD_COMPATIBLE RAW", "TRUE", "OFF AUTO", raw_metadata())
        info, = runner.parse_static_info(text)
        self.assertEqual((info.device, info.hot_pixel_map_modes, info.lens_shading_map_modes, info.lens_shading_applied),
                         ("3", {"OFF", "ON"}, {"OFF", "ON"}, ("FALSE",)))
        self.assertEqual(runner.raw_metadata_problems([info]), [])
        # The dump prints enums as numbers on some builds.
        numbers, = runner.parse_static_info(static_info("3", "BACK", "RAW", "TRUE", "OFF", raw_metadata("0 1", "0 1", "1")))
        self.assertEqual((numbers.hot_pixel_map_modes, numbers.lens_shading_map_modes, numbers.lens_shading_applied),
                         ({"OFF", "ON"}, {"OFF", "ON"}, ("TRUE",)))
        self.assertEqual(runner.raw_metadata_problems([numbers]), [])

    def test_a_raw_camera_that_lacks_a_map_mode_or_the_shading_state_is_named_by_camera(self) -> None:
        cameras = (static_info("0", "BACK", "BACKWARD_COMPATIBLE RAW", "TRUE", "OFF", raw_metadata(hot_pixel="OFF"))
                   + static_info("3", "BACK", "BACKWARD_COMPATIBLE RAW", "TRUE", "OFF", raw_metadata(shading_maps="OFF"))
                   + static_info("4", "BACK", "BACKWARD_COMPATIBLE RAW", "TRUE", "OFF", raw_metadata(applied=None)))
        problems = runner.raw_metadata_problems(runner.parse_static_info(cameras))
        self.assertEqual(len(problems), 3, problems)
        self.assertRegex(problems[0], r"camera 0 states RAW but the hot pixel map modes \['OFF'\] lack ON")
        self.assertRegex(problems[1], r"camera 3 states RAW but the lens shading map modes \['OFF'\] lack ON")
        self.assertRegex(problems[2], r"camera 4 states RAW but not whether its lens shading is applied")

    def test_a_camera_without_raw_is_not_asked_for_the_maps(self) -> None:
        plain = static_info("1", "FRONT", "BACKWARD_COMPATIBLE MANUAL_SENSOR", "FALSE", "OFF", raw_metadata("OFF", "OFF", None))
        self.assertEqual(runner.raw_metadata_problems(runner.parse_static_info(plain)), [])

    def test_a_restart_names_a_raw_camera_that_states_no_hot_pixel_map_on(self) -> None:
        plan = runner.make_plan(self.TAUGHT, "phone", "live")
        text = self.dump(self.IDS, set()).replace(
            static_info("0", "BACK", "BACKWARD_COMPATIBLE MANUAL_SENSOR READ_SENSOR_SETTINGS", "TRUE", "OFF AUTO MACRO CONTINUOUS_PICTURE"),
            static_info("0", "BACK", "BACKWARD_COMPATIBLE MANUAL_SENSOR READ_SENSOR_SETTINGS RAW", "TRUE",
                        "OFF AUTO MACRO CONTINUOUS_PICTURE", raw_metadata(hot_pixel="OFF")))
        guest = self.Guest(self.TAUGHT, plan, text)
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "RESTART_LIST_SECONDS", 0.05), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(runner.Failure, r"lack metadata Android requires of one: camera 0 states RAW but the hot "
                                                        r"pixel map modes \['OFF'\] lack ON"):
                runner.check_restart_keeps_cameras(guest, plan, list(self.IDS))

    # ---- the primary back camera, whatever camera the instance's application used last

    class FacingGuest:
        """The camera application keeps the camera it last used (`saved`), unless a launch names one with the front extra;
        `obeys` is False for an application that ignores the extra."""
        def __init__(self, saved: str, obeys: bool = True) -> None:
            self.holds, self.obeys, self.commands = saved, obeys, []
            self.hardware_input = False  # the restart mode runs without the side keys: nothing to capture

        def alive(self) -> None:
            pass

        def shell(self, command: str, timeout: float = 0) -> str:
            self.commands.append(command)
            if command.startswith("am start") and self.obeys:
                if f"--ez {runner.FRONT_EXTRA} false" in command:
                    self.holds = "0"
                elif f"--ez {runner.FRONT_EXTRA} true" in command:
                    self.holds = "1"
            return ""

        def screenshot(self):
            return object()

        def media_camera(self) -> str:
            return ("Active Camera Clients:\n[\n(Camera ID: " + self.holds + ", Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
                    "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\n]\n")

        def stills(self) -> set[str]:
            return set()

    def first_session(self, guest, calibration: Path) -> list[str]:
        plan = runner.make_plan(self.PLAIN, "phone", "live")
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "PREVIEW_SECONDS", 0.05), \
                mock.patch.object(runner, "luma_statistics", return_value=(100.0, 50.0)), \
                mock.patch.object(runner, "check_still"), mock.patch.object(runner, "check_camera_list"), \
                mock.patch.object(runner, "check_layout"), mock.patch.object(runner, "select_the_lens_that_learns", return_value=None), \
                mock.patch.object(runner, "parse_camera_ids", return_value=list(self.IDS)), contextlib.redirect_stdout(io.StringIO()):
            return runner.run_restart_checks(guest, plan, calibration)

    def test_an_instance_that_last_used_the_front_camera_is_asked_for_the_back_one_before_the_first_still(self) -> None:
        guest = self.FacingGuest(saved="1")
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "SyntheticPhone1_1.back-wide.cal").write_bytes(b"UMLCAL02")
            self.assertEqual(self.first_session(guest, Path(directory)), self.IDS)
        starts = [command for command in guest.commands if command.startswith("am start")]
        self.assertEqual(len(starts), 1)
        self.assertIn(f"--ez {runner.FRONT_EXTRA} false", starts[0])
        self.assertEqual(guest.holds, runner.PRIMARY_BACK_CAMERA)

    def test_an_application_that_keeps_the_front_camera_fails_at_the_camera_not_at_a_missing_lens_selector(self) -> None:
        guest = self.FacingGuest(saved="1", obeys=False)
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(
                runner.Failure, r"the camera application holding the primary back camera: it holds \['1'\]"):
            self.first_session(guest, Path(directory))

    def test_the_first_session_returns_the_ids_once_the_colour_data_is_kept(self) -> None:
        plan = runner.make_plan(self.PLAIN, "phone", "live")
        guest = self.Guest(self.PLAIN, plan, self.dump(self.IDS, set()))
        with tempfile.TemporaryDirectory() as directory:
            calibration = Path(directory)
            (calibration / "SyntheticPhone1_1.back-wide.cal").write_bytes(b"UMLCAL02")
            (calibration / "SyntheticPhone1_1.back-tele.cal").write_bytes(b"UMLCAL02")
            guest.stills = lambda: set()
            with mock.patch.object(runner, "check_preview"), mock.patch.object(runner, "check_primary_back_camera"), \
                    mock.patch.object(runner, "check_still"), \
                    mock.patch.object(runner, "check_camera_list"), mock.patch.object(runner, "check_layout"), \
                    mock.patch.object(runner, "select_the_lens_that_learns", return_value="2.5"), \
                    contextlib.redirect_stdout(io.StringIO()) as printed:
                self.assertEqual(runner.run_restart_checks(guest, plan, calibration), self.IDS)
            self.assertIn("lens: the button '2.5'", printed.getvalue())
            self.assertIn("SyntheticPhone1_1.back-wide.cal", printed.getvalue())
            self.assertIn("SyntheticPhone1_1.back-tele.cal", printed.getvalue())

    def test_a_first_session_that_kept_nothing_fails_before_the_restart(self) -> None:
        plan = runner.make_plan(self.PLAIN, "phone", "live")
        guest = self.Guest(self.PLAIN, plan, self.dump(self.IDS, set()))
        guest.stills = lambda: set()
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(runner.time, "sleep"), \
                mock.patch.object(runner, "CALIBRATION_SECONDS", 0.05), mock.patch.object(runner, "check_preview"), \
                mock.patch.object(runner, "check_primary_back_camera"), \
                mock.patch.object(runner, "check_still"), mock.patch.object(runner, "check_camera_list"), \
                mock.patch.object(runner, "check_layout"), mock.patch.object(runner, "select_the_lens_that_learns"), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(runner.Failure, "the colour data of the back stills \\(2\\) in the calibration directory: 0 .cal files"):
                runner.run_restart_checks(guest, plan, Path(directory))

    def options(self, *arguments: str):
        return runner.build_parser().parse_args(["--mode", "restart", *arguments])

    # What `uiautomator dump` prints of the application with three lens buttons: the selector's layout holds a button
    # for each lens, the shutter and the flip button are outside it.
    LENS_DUMP = """<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>
<hierarchy rotation="0">
  <node index="0" text="" resource-id="" class="android.widget.FrameLayout" package="org.lineageos.aperture" bounds="[0,0][720,1280]">
    <node index="0" text="" resource-id="org.lineageos.aperture:id/lensSelectorLayout" class="android.widget.LinearLayout" bounds="[200,900][520,960]">
      <node index="1" text="1\u00d7" resource-id="" class="android.widget.Button" bounds="[280,905][400,955]" />
      <node index="0" text=".5" resource-id="" class="android.widget.Button" bounds="[205,905][275,955]" />
      <node index="2" text="2.5" resource-id="" class="android.widget.Button" bounds="[405,905][515,955]" />
    </node>
    <node index="1" text="" resource-id="org.lineageos.aperture:id/shutter" class="android.widget.Button" bounds="[300,1000][420,1120]" />
  </node>
</hierarchy>
"""

    def test_the_lens_buttons_are_read_left_to_right_from_inside_the_selector(self) -> None:
        self.assertEqual(runner.parse_lens_buttons(self.LENS_DUMP),
                         [(".5", 240, 930), ("1\u00d7", 340, 930), ("2.5", 460, 930)])
        self.assertEqual(runner.parse_lens_buttons(self.LENS_DUMP.replace("lensSelectorLayout", "other")), [])
        self.assertEqual(runner.parse_lens_buttons('<hierarchy rotation="0"/>'), [])

    class UiGuest:
        def __init__(self, dump: str, holds: str = "4") -> None:
            self.dump, self.holds, self.commands = dump, holds, []

        def alive(self) -> None:
            pass

        def shell(self, command: str, timeout: float = 0) -> str:
            self.commands.append(command)
            return self.dump if "uiautomator dump" in command else ""

        def media_camera(self) -> str:
            return ("Active Camera Clients:\n[\n(Camera ID: " + self.holds + ", Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
                    "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\n]\n")

    def select(self, dump: str, profile: str = "phone", holds: str = "4"):
        guest = self.UiGuest(dump, holds)
        plan = runner.make_plan("bf-small-flash-manual" if profile == "phone" else "bf-small-flash-manual", profile, "live")
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "PREVIEW_SECONDS", 0.05):
            return guest, runner.select_the_lens_that_learns(guest, plan)

    def test_a_phone_taps_the_last_lens_button_and_waits_for_the_telephoto_camera(self) -> None:
        guest, label = self.select(self.LENS_DUMP)
        self.assertEqual(label, "2.5")
        self.assertIn("input tap 460 930", guest.commands)

    def test_a_tap_that_does_not_bring_the_telephoto_camera_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"holding camera 4 after the tap on the lens button '2.5': it holds \['0'\]"):
            self.select(self.LENS_DUMP, holds="0")

    def test_a_phone_without_lens_buttons_has_no_lens_to_teach(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "lists no lens buttons on the phone profile"):
            self.select('<?xml version="1.0"?><hierarchy rotation="0"/>')

    def test_a_tablet_without_buttons_teaches_with_its_own_camera(self) -> None:
        guest, label = self.select('<?xml version="1.0"?><hierarchy rotation="0"/>', profile="tablet")
        self.assertIsNone(label)
        self.assertFalse([command for command in guest.commands if command.startswith("input tap")])

    def test_the_restart_boots_the_same_instance_over_the_same_directory_with_the_taught_layout(self) -> None:
        options = self.options("--calibration-dir", "/cal")
        options.instance = None
        plan = runner.make_plan("bf-small-flash-manual", "phone", "live")
        first = mock.Mock(serial="127.0.0.1:5580")
        started = []

        def start(passed, again, adb, private):
            started.append((passed, again, adb, private))
            return "second guest"

        with mock.patch.object(runner, "start_guest", side_effect=start), \
                mock.patch.object(runner.boot_android, "disconnect_adb") as disconnect, \
                mock.patch.object(runner.boot_android, "stop_guest") as stop, tempfile.TemporaryDirectory() as directory:
            guest, again = runner.restart_guest(options, plan, "adb", Path(directory), first)
        self.assertEqual(guest, "second guest")
        disconnect.assert_called_once_with("adb", "127.0.0.1:5580")
        stop.assert_called_once_with(first.process)
        (passed, plan_again, adb, private), = started
        self.assertEqual(again.layout.name, self.TAUGHT)
        self.assertIs(plan_again, again)
        self.assertEqual((passed.layout, passed.calibration_dir, passed.instance), (runner.AUTO_LAYOUT, Path("/cal"), Path(directory) / "instance"))
        self.assertEqual(private, Path(directory) / "restart")
        # The launcher composes the layout from the cameras and the directory, as the host's application does.
        self.assertEqual(runner.launcher_arguments(again, True, passed.calibration_dir),
                         ["--camera=synthetic", "--camera-profile=phone", "--camera-layout=auto",
                          "--camera-calibration-dir=/cal"])

    def test_an_instance_of_the_caller_is_the_one_restarted(self) -> None:
        options = self.options("--instance", "/instance")
        plan = runner.make_plan(self.PLAIN, "phone", "live")
        with mock.patch.object(runner, "start_guest") as start, mock.patch.object(runner.boot_android, "disconnect_adb"), \
                mock.patch.object(runner.boot_android, "stop_guest"), tempfile.TemporaryDirectory() as directory:
            runner.restart_guest(options, plan, "adb", Path(directory), mock.Mock(serial="s"))
        self.assertEqual(start.call_args.args[0].instance, Path("/instance"))

    def test_a_profile_without_a_lens_that_learns_colour_data_is_refused(self) -> None:
        with mock.patch.object(sys, "argv", ["run_android_camera.py", "--mode", "restart", "--camera-profile", "laptop"]), \
                contextlib.redirect_stderr(io.StringIO()) as error:
            self.assertEqual(runner.main(), 2)
        self.assertIn("no lens with colour data to restart over", error.getvalue())

    def test_the_mode_runs_the_first_session_then_the_restart_then_the_checks_and_the_raw_still(self) -> None:
        calls = []
        directories = []
        second = mock.Mock(serial="s2", process=mock.Mock(), log=Path("/none"))

        def first(guest, plan, calibration):
            calls.append(("first", plan.layout.name, plan.mode))
            directories.append(calibration)
            self.assertTrue(calibration.is_dir())
            return list(self.IDS)

        def restart(options, plan, adb, private, guest):
            calls.append(("restart", options.calibration_dir == directories[0]))
            return second, runner.make_plan(self.TAUGHT, "phone", "live")

        def keep(guest, plan, before):
            calls.append(("keeps", guest is second, plan.layout.name, before))

        def raw_still(guest, plan):
            calls.append(("raw still", guest is second, plan.layout.name))

        arguments = ["run_android_camera.py", "--mode", "restart", "--emulator", "/emulator", "--release", "/release"]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(runner.boot_android, "resolve_executable", return_value="adb"), \
                mock.patch.object(runner.boot_android, "disconnect_adb"), mock.patch.object(runner.boot_android, "stop_guest"), \
                mock.patch.object(runner, "start_guest", return_value=mock.Mock(serial="s", process=mock.Mock(), log=Path("/none"),
                                                                hardware_input=False, capture=None)), \
                mock.patch.object(runner, "run_restart_checks", side_effect=first), mock.patch.object(runner, "restart_guest", side_effect=restart), \
                mock.patch.object(runner, "check_restart_keeps_cameras", side_effect=keep), \
                mock.patch.object(runner, "check_raw_still", side_effect=raw_still), contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(runner.main(), 0)
        self.assertEqual(calls, [("first", self.PLAIN, "live"), ("restart", True), ("keeps", True, self.TAUGHT, self.IDS),
                                 ("raw still", True, self.TAUGHT)])
        self.assertIn("PASS", printed.getvalue())
        self.assertFalse(directories[0].exists(), "the run's own directory is removed at the end")

    def test_the_mode_is_a_choice_of_the_parser_and_the_failure_log_names_the_device_info(self) -> None:
        self.assertEqual(self.options().mode, "restart")
        self.assertIn("EmulatedCameraDeviceInfo", runner.FAILURE_LOG_PATTERN)
        self.assertIn("V4l2Characteristics", runner.FAILURE_LOG_PATTERN)


class VideoLostTests(unittest.TestCase):
    """The video-lost mode: the synthetic camera's control pipe tells the guest's camera that the application left the
    foreground. The recording ends by itself with its clip (which holds video samples) saved, the application stays in
    front, and the camera opens again for a recording when the application is started again after the person returns.
    The viewfinder cases then show a recovery by itself: the video mode's (the HAL ends the camera for it, as it binds an
    encoder surface) and the photo mode's (the HAL reopens its video streams and tells the application nothing), each
    within one deadline from the return and without an activity start. The HAL's own log lines name what it did."""

    LAYOUT = "bf-large-flash-manual-concurrent-front2160"

    @staticmethod
    def leak_line(second: int) -> str:
        """What the camera service logs (threadtime format) when a device closes while a stream is still referenced."""
        return f"11-16 03:12:{second % 60:02d}.123   588   600 E Camera3-Device: disconnectImpl: Stream 0 leaked! strong reference (1)!"

    END = "W V4l2FrameSource: camera 0: the host ended the camera: its video buffers fail under an interrupted record\n"
    REOPEN = "W V4l2FrameSource: camera 0: the host ended the camera's video: its video streams are closed and opened again\n"

    class Guest(FakeGuest):
        def __init__(self, plan, **kwargs) -> None:
            super().__init__(plan, **kwargs)
            self.control_lines: list[str] = []
            self.ends_on_loss = True
            self.in_front = True
            self.log = VideoLostTests.END
            # The video viewfinder, no recording: whether the host ends the camera for it, whether the application opens
            # its camera again by itself after the return, and what else may happen to it.
            self.viewfinder_lost = True
            self.reopens = True
            self.restarts = False
            self.pid = 4711
            self.crash = False
            self.events_log = ""
            # The photo viewfinder: the HAL reopens its video streams (once for the loss, or as many times as the host
            # ends the camera's video again) and the application notices nothing; or the HAL tells it the device is gone
            # (a defect); or it does not come back.
            self.photo = False
            self.photo_reopen_lines = 1
            self.photo_lost = False
            self.photo_returns = True
            # The movie the second recording saved, when it is not the first one.
            self.again_movie: bytes | None = None
            # The clip the loss saved is still being written for this many of the runner's first looks at it (a file the
            # muxer has not closed has no `moov` box yet); the file's pulls are counted.
            self.unfinished_pulls = 0
            self.lost_pulls = 0
            # The camera service logs a leaked stream at the nth time (counted from 1) the runner sends a control line:
            # {("interrupt background", 2)} is the loss of the video viewfinder, {("resume", 3)} the return of the photo one.
            self.leaks_at: set[tuple[str, int]] = set()
            # The application's log as `logcat -v epoch` prints it, on a clock that every `date` advances by ten seconds, and its
            # crash buffer. After the loss the application (route p0031) logs the stop at once and its Finalize 1.1 s later, and
            # logs each press of the pause and resume button that it ignores: `ignored_inside` of the pressed ones land before
            # the Finalize, `ignored_after` after it. An application without the topic crashes on the first press.
            self.now = 1_000_000.0
            self.app_log = ""
            self.crash_log = ""
            self.has_guard = True
            self.ignored_inside = 3
            self.ignored_after = 0
            self.pause_button = True
            # The loops of the start window, after the viewfinder cases: a loss just after the start key, per attempt, either
            # cancels the start ("cancel"), finds the recording already created and stops it ("stopped"), or does neither and the
            # recording goes on over the loss ("survives": the second key then stops it, and a clip as long as the wait appears).
            self.window_modes = ["cancel"]
            self.window_keys = 0
            self.pending = False
            self.window_clips = 0
            self.survivor_seconds = 7.5
            self.second_seconds = 3.4
            self.files: dict[str, bytes] = {}
            # A loss that stops a recording that had started ("stopped") leaves a short clip when this is set. The clip of the
            # key that stops the recording of the attempt is still being written for its first `second_unfinished_pulls` pulls.
            self.short_clip_seconds: float | None = None
            self.second_unfinished_pulls = 0
            self.unfinished: dict[str, int] = {}
            self.pulls: dict[str, int] = {}
            # Presses that found no recording, after the Finalize had reset the state.
            self.ignored_idle = 0
            # How the movies of the clips are built: the clip of the recording that the second key starts, the short clip a loss
            # leaves (movie() options).
            self.second_options: dict = {}
            self.short_options: dict = {}
            # The same for the clip of one attempt (by its number from 1), over second_options.
            self.attempt_options: dict[int, dict] = {}

        def note(self, offset: float, tag: str, message: str, level: str = "D") -> None:
            self.app_log += f"{self.now + offset:.3f}  2179  2179 {level} {tag}: {message}\n"

        def pull(self, remote: str) -> bytes:
            if remote in self.files:
                self.pulls[remote] = self.pulls.get(remote, 0) + 1
                whole = self.files[remote]
                if self.pulls[remote] <= self.unfinished.get(remote, 0):
                    return whole[:whole.index(b"moov") - 4]
                return whole
            if remote == "/sdcard/again.mp4" and self.again_movie is not None:
                return self.again_movie
            if remote == "/sdcard/lost.mp4":
                self.lost_pulls += 1
                if self.lost_pulls <= self.unfinished_pulls:
                    whole = super().pull(remote)
                    return whole[:whole.index(b"moov") - 4]
            return super().pull(remote)

        def camera_control(self, line: str) -> None:
            self.control_lines.append(line)
            self.commands.append(f"<control {line}>")
            if (line, self.control_lines.count(line)) in self.leaks_at:
                self.service_leaks.append(VideoLostTests.leak_line(len(self.control_lines)))
            if line == "interrupt background" and self.pending:
                attempt = self.window_keys // 3
                mode = self.window_modes[min(attempt, len(self.window_modes) - 1)]
                self.pending = False
                if mode == "cancel":
                    self.note(0.02, "CameraViewModel", "The start of the video recording is cancelled")
                elif mode == "stopped":
                    self.note(0.02, "CameraViewModel", "The video recording is stopped")
                    if self.short_clip_seconds is not None:
                        self.add_clip("short", self.short_clip_seconds)
                elif mode == "survives":
                    self.recording = True
                self.held = set()
                self.log += VideoLostTests.END
            elif line == "interrupt background" and self.recording:
                if self.ends_on_loss:
                    self.recording = False
                    self.rows["2"] = {"_id": "2", "_data": "/sdcard/lost.mp4", "_display_name": "lost.mp4"}
                    self.note(0.015, "CameraActivity", "Error: code: 3, type: RECOVERABLE", "E")
                    if self.has_guard:
                        self.note(0.016, "CameraViewModel", "The video recording is stopped")
                    self.note(1.15, "CameraViewModel", "Video capture succeeded: content://media/external/video/media/2")
            elif line == "interrupt background" and self.photo:
                if self.photo_lost:
                    self.held = set()
                    self.log += VideoLostTests.END
                else:
                    self.log += VideoLostTests.REOPEN * self.photo_reopen_lines
                if not self.photo_returns:
                    self.held = set()
            elif line == "interrupt background":
                if self.viewfinder_lost:
                    self.held = set()
                    self.log += VideoLostTests.END
            elif line == "resume" and not self.held and self.reopens and (self.photo_returns or not self.photo):
                self.held = {"0"}
                if self.restarts:
                    self.pid += 1
                if self.crash:
                    self.events_log += "11-15 16:50:00.000 I am_crash: [4711,0,org.lineageos.aperture]\n"

        def add_clip(self, name: str, seconds: float, unfinished_pulls: int = 0, attempt: int = 0) -> str:
            self.window_clips += 1
            path = f"/sdcard/window{self.window_clips}-{name}.mp4"
            options = {"second": {**self.second_options, **self.attempt_options.get(attempt, {})}, "short": self.short_options}.get(name, {})
            self.files[path] = movie(seconds, audio=sound(seconds), **options)
            self.unfinished[path] = unfinished_pulls
            identity = str(100 + self.window_clips)
            self.rows[identity] = {"_id": identity, "_data": path, "_display_name": f"window{self.window_clips}-{name}.mp4"}
            return path

        def window_key(self, command: str) -> str:
            self.commands.append(command)
            index = self.window_keys % 3
            self.window_keys += 1
            if index == 0:
                self.pending = True
            elif self.recording:
                self.recording = False
                # The application logs every stop, the key's own too.
                self.note(0.05, "CameraViewModel", "The video recording is stopped")
                if index == 1:
                    self.add_clip("survivor", self.survivor_seconds)
                else:
                    self.add_clip("second", self.second_seconds, self.second_unfinished_pulls, (self.window_keys - 1) // 3 + 1)
                self.note(0.1, "CameraViewModel", "Video capture succeeded: content://media/external/video/media/9")
            else:
                self.recording = True
            return ""

        def shell(self, command: str, timeout: float = 0) -> str:
            if command == "date +%s.%N":
                self.commands.append(command)
                self.now += 10.0
                return f"{self.now:.9f}\n"
            if "uiautomator dump" in command:
                self.commands.append(command)
                node = ('<node index="0" text="" resource-id="org.lineageos.aperture:id/videoRecordingStateButton" '
                        'class="android.widget.ImageButton" checked="false" bounds="[616,1039][659,1082]" />')
                return f'<hierarchy rotation="0">{node if self.pause_button else ""}</hierarchy>'
            if command.startswith("input tap ") and command.endswith(" wait"):
                self.commands.append(command)
                if self.has_guard:
                    for number in range(self.ignored_inside):
                        self.note(0.3 + 0.1 * number, "CameraViewModel", "The pause or resume is ignored, the video recording ends")
                    for number in range(self.ignored_after):
                        self.note(1.5 + 0.1 * number, "CameraViewModel", "The pause or resume is ignored, the video recording ends")
                    for number in range(self.ignored_idle):
                        self.note(1.6 + 0.1 * number, "CameraViewModel", "The pause or resume is ignored, no recording runs (IDLE)",
                                  "W")
                else:
                    when = self.now + 0.4
                    self.crash_log += (f"{when:.3f}  2179  2179 E AndroidRuntime: FATAL EXCEPTION: main\n"
                                       f"{when:.3f}  2179  2179 E AndroidRuntime: Process: org.lineageos.aperture, PID: 2179\n"
                                       f"{when:.3f}  2179  2179 E AndroidRuntime: java.lang.IllegalStateException: "
                                       "The recording has been stopped.\n")
                return ""
            if command.startswith("logcat -d -v epoch -b all CameraViewModel:D"):
                self.commands.append(command)
                return self.app_log
            if command.startswith("logcat -d -v epoch -b crash"):
                self.commands.append(command)
                return "--------- beginning of crash\n" + self.crash_log
            if command == f"input keyevent {runner.CAMERA_KEYCODE}" and self.control_lines.count("resume") >= 3:
                return self.window_key(command)
            if command.startswith("am start"):
                self.photo = "STILL_IMAGE_CAMERA" in command
            if command.startswith("pidof "):
                return f"{self.pid}\n"
            if command.startswith("logcat -d -b events"):
                return self.events_log
            if command == runner.FOREGROUND_COMMAND:
                return foreground_text() if self.in_front else foreground_text("topResumedActivity=ActivityRecord{1 u0 com.android.launcher3/.Launcher t1}")
            if command == f"input keyevent {runner.CAMERA_KEYCODE}" and self.recording and "2" in self.rows:
                # The key that stops the second recording: a new row, not the one the loss made.
                self.recording = False
                self.rows["3"] = {"_id": "3", "_data": "/sdcard/again.mp4", "_display_name": "again.mp4"}
                self.commands.append(command)
                return ""
            return super().shell(command, timeout)

    def run_check(self, guest, sleeps: list[float] | None = None) -> str:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), \
                mock.patch.object(runner.time, "sleep", lambda seconds: sleeps is None or sleeps.append(seconds)):
            runner.check_video_lost(guest, guest.plan)
        return out.getvalue()

    def plan(self):
        return runner.make_plan(self.LAYOUT, "phone", "video-lost")

    def test_the_mode_is_a_plan_of_the_live_cameras(self) -> None:
        plan = self.plan()
        self.assertEqual(plan.camera_ids, runner.PROFILE_CAMERAS["phone"][1])
        self.assertIn("video-lost", runner.MODES)
        self.assertEqual(runner.CONTROL_VARIABLE, "UML_SYNTHETIC_CAMERA_CONTROL")

    def test_the_loss_ends_the_recording_and_the_camera_opens_again(self) -> None:
        guest = self.Guest(self.plan())
        text = self.run_check(guest)
        self.assertIn("the recording ended by itself: lost.mp4", text)
        self.assertIn("the application stays in front", text)
        self.assertIn("the camera opened again for a new start of the application: again.mp4", text)
        self.assertIn("the HAL reported the end of the camera", text)
        self.assertIn("video viewfinder: the HAL ended the camera and did not reopen its video streams", text)
        self.assertIn("photo viewfinder: the HAL did not end the camera and reopened its video streams", text)
        self.assertIn("video viewfinder: live again after the return, same process 4711, no error window", text)
        self.assertIn("photo viewfinder: live again after the return, same process 4711, no error window", text)
        # The recording, the two viewfinders and the five losses of the start window.
        self.assertEqual(guest.control_lines, ["interrupt background", "resume"] * (3 + runner.START_WINDOW_ATTEMPTS))
        # The first recording is stopped by the loss, not by a second key press; the second is started and stopped by key;
        # each attempt of the start window presses it three times.
        self.assertEqual(guest.commands.count(f"input keyevent {runner.CAMERA_KEYCODE}"), 3 + 3 * runner.START_WINDOW_ATTEMPTS)
        self.assertEqual(guest.commands.count(f"am force-stop {runner.CAMERA_PACKAGE}"), 1,
                         "only the first start stops the application: the second shows that it opens its camera again")

    def test_only_the_recording_case_starts_the_application_after_the_return(self) -> None:
        # The recording case says it starts the application again after its return; the viewfinder cases show a recovery by
        # itself, so no activity start follows their return. The photo case starts its own mode before its loss.
        guest = self.Guest(self.plan())
        self.run_check(guest)
        tokens = ["start" if command.startswith("am start") else command[len("<control "):-1].split()[0]
                  for command in guest.commands if command.startswith("am start") or command.startswith("<control")]
        self.assertEqual(tokens, ["start", "interrupt", "resume", "start", "interrupt", "resume", "start", "interrupt", "resume",
                                  "start"] + ["interrupt", "resume"] * runner.START_WINDOW_ATTEMPTS)

    def test_a_recording_that_does_not_end_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.ends_on_loss = False
        with self.assertRaisesRegex(runner.Failure, "a movie in MediaStore"), \
                mock.patch.object(runner, "LOST_WAIT_SECONDS", 0):
            self.run_check(guest)

    def test_a_clip_with_a_video_track_and_no_samples_fails(self) -> None:
        # A header with the track marker and an empty sample table is not a recording (it was accepted before).
        guest = self.Guest(self.plan(), movies=movie(3.5, samples=False))
        with self.assertRaisesRegex(runner.Failure, "no encoded video samples"):
            self.run_check(guest)

    def test_a_clip_is_pulled_when_it_is_finished_not_a_fixed_time_after_the_loss(self) -> None:
        # The clip finishes a second or so after the loss: the runner looks every half second until it parses, and waits
        # no fixed time after the row appeared (the recordings that follow still settle for the application to close the file).
        guest = self.Guest(self.plan())
        guest.unfinished_pulls = 2
        sleeps: list[float] = []
        text = self.run_check(guest, sleeps)
        self.assertIn("the recording ended by itself: lost.mp4", text)
        self.assertEqual(guest.lost_pulls, 3)
        after_hold = sleeps[sleeps.index(runner.LOST_HOLD_SECONDS) + 1:]
        self.assertEqual(after_hold[:2], [0.5, 0.5])
        self.assertNotEqual(after_hold[2], 3, "no fixed wait follows the two looks")

    def test_a_clip_that_is_never_finished_fails_at_the_bound(self) -> None:
        guest = self.Guest(self.plan())
        guest.unfinished_pulls = 10 ** 6
        with self.assertRaisesRegex(runner.Failure, r"/sdcard/lost\.mp4 is not a finished movie: no ftyp and moov.*\(after 0 s\)"), \
                mock.patch.object(runner, "LOST_WAIT_SECONDS", 0.05):
            self.run_check(guest)
        self.assertGreaterEqual(guest.lost_pulls, 1)

    def clip_fails(self, clip: bytes, pattern: str) -> None:
        guest = self.Guest(self.plan(), movies=clip)
        with self.assertRaisesRegex(runner.Failure, pattern):
            self.run_check(guest)

    def test_the_clip_of_the_loss_is_measured_and_printed(self) -> None:
        text = self.run_check(self.Guest(self.plan()))
        self.assertIn("clip: 105 frames over 3.50 s = 30.0 fps average, longest step of its video sample times 0.033 s", text)
        self.assertIn("audio: 164 AAC frames", text)

    def test_a_clip_that_goes_on_after_the_loss_fails(self) -> None:
        # The recording went on over the camera's reopening: more than the video before the loss and the time to end it.
        limit = runner.LOST_HOLD_SECONDS + runner.LOST_END_SECONDS
        self.clip_fails(movie(limit + 0.5, audio=sound(limit + 0.5)), r"lasts 6\.50 s.*between 2 s and 6 s")
        guest = self.Guest(self.plan(), movies=movie(limit, audio=sound(limit)))
        self.assertIn("the recording ended by itself", self.run_check(guest))

    def test_a_clip_shorter_than_the_hold_fails(self) -> None:
        self.clip_fails(movie(1.5, audio=sound(1.5)), r"lasts 1\.50 s.*between 2 s and 6 s")

    def test_a_clip_with_too_few_frames_for_its_duration_fails(self) -> None:
        # A stall: 12 frames a second over the clip. A camera that was away for a part of the clip leaves this.
        self.clip_fails(movie(3.5, fps=12.0, audio=sound(3.5)), r"averages 12\.0 fps \(42 frames over 3\.50 s\), below 20")

    def test_a_clip_with_a_gap_in_its_video_fails(self) -> None:
        # One sample lasts 0.6 s longer than the others: the camera was away for that long and the recording went on.
        self.clip_fails(movie(3.5, audio=sound(3.5), video_jump=(60, 0.6)), r"steps 0\.63 s between two samples")

    def test_a_clip_with_no_audio_track_fails(self) -> None:
        self.clip_fails(movie(3.5), "has no audio track")

    def test_a_clip_whose_audio_outlasts_its_video_fails(self) -> None:
        # The microphone went on over the reopening and the video did not: the tracks end 1.5 s apart (to the AAC frame).
        self.clip_fails(movie(3.5, audio=sound(5.0)), r"the audio ends \+1\.4\d s after the video")

    def test_a_clip_whose_audio_time_line_is_stretched_fails(self) -> None:
        self.clip_fails(movie(3.5, audio=sound(3.5, 1.3)), "stretch 1.300")

    def test_a_recording_after_the_return_without_samples_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.again_movie = movie(3.5, samples=False)
        with self.assertRaisesRegex(runner.Failure, "with encoded video samples"):
            self.run_check(guest)

    # The pause and resume button, pressed at once after the loss: the stopped recording is closed (pause, resume and mute throw
    # on it) and the screen shows it until its Finalize.
    def test_the_button_is_read_before_the_loss_and_pressed_at_once_after_it(self) -> None:
        guest = self.Guest(self.plan())
        text = self.run_check(guest)
        taps = f"input tap 637 1060 & input tap 637 1060 & input tap 637 1060 & wait"
        self.assertEqual(guest.commands.count(taps), 1)
        index = guest.commands.index(taps)
        self.assertEqual(guest.commands[index - 1], "<control interrupt background>", "nothing between the loss and the presses")
        self.assertTrue(any("uiautomator dump" in command for command in guest.commands[:index]))
        self.assertIn("the pause and resume button was pressed 3 times at once after the loss: 3 presses landed while the "
                      "recording ended and were ignored, 0 more were ignored after its Finalize line but before the state was "
                      "reset, 0 found no recording (the application's log)", text)

    def test_the_presses_are_counted_inside_the_window_and_after_it(self) -> None:
        guest = self.Guest(self.plan())
        guest.ignored_inside, guest.ignored_after = 2, 1
        self.assertIn("2 presses landed while the recording ended and were ignored, 1 more were ignored after its Finalize line",
                      self.run_check(guest))

    def test_presses_ignored_after_the_finalize_line_count_as_ignored(self) -> None:
        # The ending mark outlives the Finalize line (the application broadcasts the clip and resets its state after it): a
        # press that came then and was ignored is the guard at work, not a row that showed nothing.
        guest = self.Guest(self.plan())
        guest.ignored_inside, guest.ignored_after = 0, 3
        text = self.run_check(guest)
        self.assertIn("0 presses landed while the recording ended and were ignored, 3 more were ignored after its Finalize line "
                      "but before the state was reset, 0 found no recording", text)
        self.assertIn("the recording ended by itself: lost.mp4", text)

    def test_an_application_that_ignores_nothing_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.ignored_inside = 0
        with self.assertRaisesRegex(runner.Failure, "no press of the pause and resume button was ignored for the stopped recording"):
            self.run_check(guest)

    def test_presses_that_only_found_no_recording_show_nothing_and_fail(self) -> None:
        # Every press came after the state was reset: the log line of a press that found no recording is no ignored press.
        guest = self.Guest(self.plan())
        guest.ignored_inside, guest.ignored_idle = 0, 3
        with self.assertRaisesRegex(runner.Failure, "no press of the pause and resume button was ignored for the stopped recording"):
            self.run_check(guest)

    def test_an_application_that_crashes_on_the_press_fails_with_the_exception(self) -> None:
        # An application with the stop and no managed transition: pause() on the closed recording throws.
        guest = self.Guest(self.plan())
        guest.has_guard = False
        with self.assertRaisesRegex(runner.Failure, r"the camera application crashed after the loss .*IllegalStateException: "
                                                    r"The recording has been stopped\."):
            self.run_check(guest)
        self.assertEqual(guest.lost_pulls, 0, "the crash is the failure, not a clip that never finishes")

    def test_a_screen_without_the_button_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.pause_button = False
        with self.assertRaisesRegex(runner.Failure, "shows no pause and resume button while it records"):
            self.run_check(guest)

    def test_the_clip_bound_follows_a_longer_hold_before_the_loss(self) -> None:
        # The button is read from the screen during the hold, which can take a few seconds on a loaded guest: the clip may then
        # last the real hold and LOST_END_SECONDS, not the nominal hold. 6.5 s fails after a 4 s hold and passes after 5 s.
        clock = self.Clock()
        guest = self.Guest(self.plan(), movies=movie(7.0, audio=sound(7.0)))
        original = guest.shell

        def shell(command: str, timeout: float = 0) -> str:
            if "uiautomator dump" in command:
                clock.sleep(5.0)
            return original(command, timeout)

        guest.shell = shell
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.object(runner.time, "monotonic", clock.monotonic), \
                mock.patch.object(runner.time, "sleep", clock.sleep):
            runner.check_video_lost(guest, guest.plan)
        self.assertIn("the recording ended by itself: lost.mp4 7.00 s", out.getvalue())

    def test_a_crash_is_the_applications_own_and_since_the_start(self) -> None:
        crash = ("--------- beginning of crash\n"
                 "99.000  1  1 E AndroidRuntime: FATAL EXCEPTION: main\n"
                 "99.000  1  1 E AndroidRuntime: Process: org.lineageos.aperture, PID: 1\n"
                 "99.001  1  1 E AndroidRuntime: java.lang.IllegalStateException: before\n"
                 "200.000  7  7 E AndroidRuntime: FATAL EXCEPTION: main\n"
                 "200.000  7  7 E AndroidRuntime: Process: com.other.app, PID: 7\n"
                 "200.001  7  7 E AndroidRuntime: java.lang.RuntimeException: not ours\n")

        class Stub:
            def shell(self, command: str, timeout: float = 0) -> str:
                return crash

        runner.require_no_crash(Stub(), 100.0, "x")  # the first is before, the second is another process
        with self.assertRaisesRegex(runner.Failure, r"crashed after the loss \(x\): .*before"):
            runner.require_no_crash(Stub(), 50.0, "x")

    def test_the_application_log_is_read_from_a_point_in_time(self) -> None:
        log = ("--------- beginning of main\n"
               "10.000  1  1 D CameraViewModel: before\n"
               "20.500  1  2 D CameraViewModel: The video recording is stopped\n"
               "20.600  1  2 E CameraActivity: Error: code: 3, type: RECOVERABLE\n")
        self.assertEqual(runner.parse_application_events(log, 20.0),
                         [(20.5, "CameraViewModel", "The video recording is stopped"),
                          (20.6, "CameraActivity", "Error: code: 3, type: RECOVERABLE")])
        self.assertEqual(runner.parse_application_events("", 0.0), [])

    # A loss in the half second between the start key and the creation of the recording (the start sound).
    def test_a_loss_just_after_the_start_key_cancels_the_start_in_every_attempt(self) -> None:
        guest = self.Guest(self.plan())
        text = self.run_check(guest)
        for number in range(1, runner.START_WINDOW_ATTEMPTS + 1):
            self.assertIn(f"start window {number} of 5: the start was cancelled; clips: window{number}-second.mp4 3.40 s", text)
        self.assertIn("the loss cancelled the start in 5 of 5 attempts (5 conclusive); the application stayed in front", text)
        # Each attempt: the start key, the loss at once, the return, the key that starts, the key that stops.
        loop = guest.commands[guest.commands.index("<control resume>", guest.commands.index("<control resume>",
                                                   guest.commands.index("<control resume>") + 1) + 1) + 1:]
        key = f"input keyevent {runner.CAMERA_KEYCODE}"
        first = loop.index(key)
        self.assertEqual(loop[first + 1], "<control interrupt background>", "no command between the key and the loss")

    def test_a_start_that_was_not_cancelled_goes_on_over_the_loss_and_fails(self) -> None:
        # The defect: the recording the person started before the loss runs over the reopening, and the key that should
        # start a recording stops it: one clip as long as the wait.
        guest = self.Guest(self.plan())
        guest.window_modes = ["survives"]
        with self.assertRaisesRegex(runner.Failure, r"window1-survivor\.mp4 lasts 7\.50 s \(above 4\.5 s: the 3\.0 s between the keys.*went on over the camera's reopening"):
            self.run_check(guest)

    def slow_stop_key(self, guest, delay: float) -> str:
        """The check on a virtual clock whose third key of every attempt (the one that stops the recording) takes `delay`
        seconds to be delivered, as it can on a guest under load."""
        clock = self.Clock()
        original = guest.shell
        key = f"input keyevent {runner.CAMERA_KEYCODE}"

        def shell(command: str, timeout: float = 0) -> str:
            if command == key and guest.control_lines.count("resume") >= 3 and guest.window_keys % 3 == 2:
                clock.sleep(delay)
            return original(command, timeout)

        guest.shell = shell
        out = io.StringIO()
        with contextlib.redirect_stdout(out), mock.patch.object(runner.time, "monotonic", clock.monotonic), \
                mock.patch.object(runner.time, "sleep", clock.sleep):
            runner.check_video_lost(guest, guest.plan)
        return out.getvalue()

    def test_the_bound_of_the_clip_follows_the_time_between_the_keys(self) -> None:
        # The stop key reaches the recording 2.5 s late: the recording ran 5.4 s between the keys (3 s hold, 2.5 s of delivery),
        # more than the nominal hold and its margin, and it is still the clip of a correct application.
        guest = self.Guest(self.plan())
        guest.second_seconds = 5.4
        text = self.slow_stop_key(guest, 2.5)
        self.assertIn("start window 1 of 5: the start was cancelled; clips: window1-second.mp4 5.40 s", text)
        # The same clip after keys that were delivered at once is longer than a recording of the hold can be.
        slow = self.Guest(self.plan())
        slow.second_seconds = 5.4
        with self.assertRaisesRegex(runner.Failure, r"window1-second\.mp4 lasts 5\.40 s \(above 4\.5 s: the 3\.0 s between the keys"):
            self.run_check(slow)

    def test_a_survivor_is_still_longer_than_the_slow_keys_allow(self) -> None:
        # With the stop key 2.5 s late the bound is 7.0 s: a recording that went on over the loss lasts as long as the wait.
        guest = self.Guest(self.plan())
        guest.window_modes = ["survives"]
        guest.survivor_seconds = 8.5
        with self.assertRaisesRegex(runner.Failure, r"window1-survivor\.mp4 lasts 8\.50 s \(above 7\.0 s: the 5\.5 s between the keys"):
            self.slow_stop_key(guest, 2.5)

    def test_the_recording_after_the_cancelled_start_must_hold_encoded_video(self) -> None:
        # A container of the right length with no video samples, or with the samples declared and no bytes behind them, is not
        # the recording that the key started.
        for options, pattern in (({"samples": False}, r"window1-second\.mp4 has a video track but no encoded video samples"),
                                 ({"payload": 40}, r"window1-second\.mp4 declares 102 video samples but its media data holds 40 bytes"),
                                 ({"video": False}, r"window1-second\.mp4 \(start window attempt 1\) has no video track")):
            guest = self.Guest(self.plan())
            guest.second_options = options
            with self.subTest(options=options), self.assertRaisesRegex(runner.Failure, pattern):
                self.run_check(guest)

    def test_the_short_clip_a_loss_leaves_is_not_the_recording_that_follows_the_cancelled_start(self) -> None:
        # The loss stopped a recording that had started: its short clip may hold nothing, the clip of the next recording is judged.
        guest = self.Guest(self.plan())
        guest.window_modes = ["stopped", "cancel"]
        guest.short_clip_seconds = 0.4
        guest.short_options = {"samples": False}
        text = self.run_check(guest)
        self.assertIn("start window 1 of 5: the recording had started and the loss stopped it; clips: window1-short.mp4 0.40 s", text)

    def test_losses_that_all_came_after_the_recording_was_created_prove_nothing_and_fail(self) -> None:
        guest = self.Guest(self.plan())
        guest.window_modes = ["stopped"]
        with self.assertRaisesRegex(runner.Failure, "no loss cancelled a start in 5 attempts"):
            self.run_check(guest)

    def test_one_cancelled_start_among_misses_is_enough(self) -> None:
        guest = self.Guest(self.plan())
        guest.window_modes = ["stopped", "stopped", "cancel", "stopped", "stopped"]
        text = self.run_check(guest)
        self.assertIn("start window 1 of 5: the recording had started and the loss stopped it", text)
        self.assertIn("start window 3 of 5: the start was cancelled", text)
        self.assertIn("the loss cancelled the start in 1 of 5 attempts", text)

    def test_a_loss_that_ended_nothing_in_the_log_is_said_so(self) -> None:
        # The key's own stop is one stop line: the loss itself stopped nothing and cancelled nothing.
        guest = self.Guest(self.plan())
        guest.window_modes = ["nothing", "cancel"]
        text = self.run_check(guest)
        self.assertIn("start window 1 of 5: inconclusive, the loss cancelled and stopped nothing in the log; clips: window1-second.mp4", text)
        self.assertIn("start window 2 of 5: the start was cancelled", text)
        self.assertNotIn("the recording had started and the loss stopped it", text)

    def test_a_loss_after_the_recording_was_created_leaves_a_short_clip_and_the_next_clip_is_awaited(self) -> None:
        # The loss stopped a recording with frames: its short clip is finished at once, and the clip of the key that stops the
        # next recording is still being written at the first looks. The attempt judges the clip of the second recording.
        guest = self.Guest(self.plan())
        guest.window_modes = ["stopped", "cancel"]
        guest.short_clip_seconds = 0.4
        guest.second_unfinished_pulls = 3
        text = self.run_check(guest)
        self.assertIn("start window 1 of 5: the recording had started and the loss stopped it; clips: window1-short.mp4 0.40 s, "
                      "window2-second.mp4 3.40 s", text)

    def test_the_clip_of_an_inconclusive_attempt_is_not_credited(self) -> None:
        # The log says neither a cancel nor a stop for the loss of attempt 1: the clip there is not judged as the recording after
        # the return, even when it holds no video, and the attempt is printed as inconclusive. The conclusive ones decide.
        guest = self.Guest(self.plan())
        guest.window_modes = ["nothing", "cancel"]
        guest.attempt_options = {1: {"samples": False}}
        text = self.run_check(guest)
        self.assertIn("start window 1 of 5: inconclusive, the loss cancelled and stopped nothing in the log", text)
        self.assertIn("the loss cancelled the start in 4 of 5 attempts (4 conclusive)", text)
        # The same clip after a conclusive attempt is judged.
        guest = self.Guest(self.plan())
        guest.window_modes = ["cancel"]
        guest.attempt_options = {1: {"samples": False}}
        with self.assertRaisesRegex(runner.Failure, r"window1-second\.mp4 has a video track but no encoded video samples"):
            self.run_check(guest)

    def test_an_inconclusive_attempt_does_not_hide_a_clip_that_went_on(self) -> None:
        guest = self.Guest(self.plan())
        guest.window_modes = ["survives", "cancel"]
        with self.assertRaisesRegex(runner.Failure, r"window1-survivor\.mp4 lasts 7\.50 s"):
            self.run_check(guest)

    def test_the_row_needs_its_number_of_conclusive_attempts(self) -> None:
        # Two attempts of five say what the loss did: the clips of the others are not credited and the row proves nothing.
        guest = self.Guest(self.plan())
        guest.window_modes = ["nothing", "nothing", "nothing", "cancel", "cancel"]
        with self.assertRaisesRegex(runner.Failure, r"says what the loss did in 2 of 5 attempts, 3 needed: the other clips are not credited"):
            self.run_check(guest)
        guest = self.Guest(self.plan())
        guest.window_modes = ["nothing", "nothing", "cancel", "cancel", "cancel"]
        text = self.run_check(guest)
        self.assertIn("the loss cancelled the start in 3 of 5 attempts (3 conclusive)", text)

    def test_losses_after_the_recording_was_created_count_as_conclusive_but_prove_no_cancel(self) -> None:
        guest = self.Guest(self.plan())
        guest.window_modes = ["stopped", "stopped", "stopped", "stopped", "stopped"]
        with self.assertRaisesRegex(runner.Failure, "no loss cancelled a start in 5 attempts"):
            self.run_check(guest)

    def test_presses_that_found_no_recording_are_counted(self) -> None:
        guest = self.Guest(self.plan())
        guest.ignored_idle = 2
        self.assertIn("3 presses landed while the recording ended and were ignored, 0 more were ignored after its Finalize line "
                      "but before the state was reset, 2 found no recording (the application's log)", self.run_check(guest))

    def test_a_recording_after_the_loss_that_is_too_short_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.second_seconds = 1.0
        with self.assertRaisesRegex(runner.Failure, r"lasts under 2 s .*the key did not start a recording"):
            self.run_check(guest)

    def test_an_application_that_left_the_front_in_the_start_window_fails(self) -> None:
        guest = self.Guest(self.plan())
        original = guest.shell

        def shell(command: str, timeout: float = 0) -> str:
            if command == runner.FOREGROUND_COMMAND and guest.window_keys >= 3:
                return foreground_text("topResumedActivity=ActivityRecord{1 u0 com.android.launcher3/.Launcher t1}")
            return original(command, timeout)

        guest.shell = shell
        with self.assertRaisesRegex(runner.Failure, "not in front after start window attempt 1"):
            self.run_check(guest)

    def test_the_movies_of_an_attempt_are_the_finished_ones_that_appeared(self) -> None:
        files = {"/sdcard/done.mp4": movie(3.0, audio=sound(3.0)), "/sdcard/part.mp4": movie(3.0)[:100]}
        rows = {"1": {"_id": "1", "_data": "/sdcard/old.mp4"},
                "2": {"_id": "2", "_data": "/sdcard/part.mp4"}, "3": {"_id": "3", "_data": "/sdcard/done.mp4"}}

        class Stub:
            def alive(self) -> None:
                pass

            def videos(self):
                return rows, ""

            def pull(self, remote: str) -> bytes:
                return files[remote]

        with mock.patch.object(runner.time, "sleep", lambda seconds: None):
            found = runner.finished_new_movies(Stub(), {"1": rows["1"]}, 5)
            self.assertEqual([row["_id"] for row, _ in found], ["3"], "a file still being written is left out")
            files.pop("/sdcard/done.mp4")
            files["/sdcard/done.mp4"] = movie(3.0)[:100]
            clock = self.Clock()
            with mock.patch.object(runner.time, "monotonic", clock.monotonic), mock.patch.object(runner.time, "sleep", clock.sleep):
                with self.assertRaisesRegex(runner.Failure, "no movie that appeared since the start key is a finished one after 5 s"):
                    runner.finished_new_movies(Stub(), {"1": rows["1"]}, 5)

    def test_the_movies_of_an_attempt_wait_for_the_clip_that_counts(self) -> None:
        # A short clip is finished and the clip of the second recording is still being written for the first looks: with a
        # minimum the call keeps looking until a clip of that length parses or every new row does (not at the first look).
        files = {"/sdcard/short.mp4": movie(0.4, audio=sound(0.4)), "/sdcard/long.mp4": movie(3.0, audio=sound(3.0))}
        rows = {"1": {"_id": "1", "_data": "/sdcard/old.mp4"},
                "2": {"_id": "2", "_data": "/sdcard/short.mp4"}, "3": {"_id": "3", "_data": "/sdcard/long.mp4"}}
        looks = {"long": 0}

        class Stub:
            def alive(self) -> None:
                pass

            def videos(self):
                return rows, ""

            def pull(self, remote: str) -> bytes:
                if remote == "/sdcard/long.mp4":
                    looks["long"] += 1
                    if looks["long"] <= 3:
                        return files[remote][:files[remote].index(b"moov") - 4]
                return files[remote]

        with mock.patch.object(runner.time, "sleep", lambda seconds: None):
            found = runner.finished_new_movies(Stub(), {"1": rows["1"]}, 5, 2.0)
            self.assertEqual([row["_id"] for row, _ in found], ["2", "3"])
            self.assertEqual(looks["long"], 4, "three looks at the unfinished clip, then the clip that counts")
            looks["long"] = -10 ** 6
            clock = self.Clock()
            with mock.patch.object(runner.time, "monotonic", clock.monotonic), mock.patch.object(runner.time, "sleep", clock.sleep):
                found = runner.finished_new_movies(Stub(), {"1": rows["1"]}, 5, 2.0)
            self.assertEqual([row["_id"] for row, _ in found], ["2"], "at the bound what parsed is returned for the caller to judge")

    def test_an_application_that_left_the_front_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.in_front = False
        with self.assertRaisesRegex(runner.Failure, "not in front after the recording ended"):
            self.run_check(guest)

    def test_a_hal_without_the_topic_fails_after_the_camera_opened_again(self) -> None:
        guest = self.Guest(self.plan())
        guest.log = ""
        with self.assertRaisesRegex(runner.Failure, "image lacks the HAL topic"):
            self.run_check(guest)

    def test_a_viewfinder_the_host_did_not_end_passes_and_says_so(self) -> None:
        guest = self.Guest(self.plan())
        guest.viewfinder_lost = False
        text = self.run_check(guest)
        self.assertIn("video viewfinder: the HAL did not end the camera", text)
        self.assertIn("live again after the return", text)

    def test_a_viewfinder_that_does_not_come_back_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.reopens = False
        with self.assertRaisesRegex(runner.Failure, "holding camera 0 \\(video viewfinder after the return\\)"), \
                mock.patch.object(runner, "VIEWFINDER_RECOVERY_SECONDS", 0):
            self.run_check(guest)

    def test_a_viewfinder_the_application_restarted_for_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.restarts = True
        with self.assertRaisesRegex(runner.Failure, "was restarted"):
            self.run_check(guest)

    def test_a_crash_around_the_loss_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.crash = True
        with self.assertRaisesRegex(runner.Failure, "crashed or stopped responding"):
            self.run_check(guest)

    def test_an_error_window_in_front_is_not_a_viewfinder(self) -> None:
        # The application holds its camera again but an error window has the focus.
        guest = self.Guest(self.plan())
        original = guest.shell

        def shell(command: str, timeout: float = 0) -> str:
            if command == runner.FOREGROUND_COMMAND and guest.control_lines.count("resume") == 2:
                return foreground_text(focus="mCurrentFocus=Window{9 u0 Application Error: org.lineageos.aperture}")
            return original(command, timeout)

        guest.shell = shell
        with self.assertRaisesRegex(runner.Failure, "does not show its viewfinder after the return"):
            self.run_check(guest)

    # The photo mode's viewfinder has no encoder stream: the HAL must not end the camera for it.
    def test_a_photo_viewfinder_the_hal_told_the_device_is_gone_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.photo_lost = True
        with self.assertRaisesRegex(runner.Failure, "told the photo viewfinder that the device is gone"):
            self.run_check(guest)

    def test_a_photo_viewfinder_the_hal_never_reopened_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.photo_reopen_lines = 0
        with self.assertRaisesRegex(runner.Failure, "logged no reopening of its video streams"):
            self.run_check(guest)

    # One close and one reopening per loss: the host holds the session the HAL opens while the application is away.
    def test_a_photo_viewfinder_reopened_once_for_the_loss_passes_and_says_so(self) -> None:
        guest = self.Guest(self.plan())
        text = self.run_check(guest)
        self.assertIn("photo viewfinder: live again after the return, same process 4711, no error window, one reopening", text)
        self.assertNotIn("video viewfinder: live again after the return, same process 4711, no error window, one reopening", text)

    def test_a_photo_viewfinder_reopened_again_and_again_for_one_loss_fails(self) -> None:
        # The loop of the defect: each session the HAL opens while the application is away is ended again.
        for lines in (2, 37):
            with self.subTest(lines=lines):
                guest = self.Guest(self.plan())
                guest.photo_reopen_lines = lines
                with self.assertRaisesRegex(runner.Failure,
                                            rf"reopened its video streams {lines} times for one loss \(one expected\)"):
                    self.run_check(guest)

    def test_the_reopenings_before_the_loss_are_not_the_losss(self) -> None:
        # The count is the loss's own: lines the HAL logged earlier (the log is cumulative) do not count.
        guest = self.Guest(self.plan())
        guest.log += self.REOPEN * 5
        text = self.run_check(guest)
        self.assertIn("photo viewfinder: live again after the return", text)

    def test_a_photo_viewfinder_that_does_not_come_back_fails(self) -> None:
        guest = self.Guest(self.plan())
        guest.photo_returns = False
        with self.assertRaisesRegex(runner.Failure, "holding camera 0 \\(photo viewfinder after the return\\)"), \
                mock.patch.object(runner, "VIEWFINDER_RECOVERY_SECONDS", 0), \
                contextlib.redirect_stdout(io.StringIO()), mock.patch.object(runner.time, "sleep", lambda seconds: None):
            runner.check_viewfinder_returns(guest, guest.plan, "photo")

    # The oracle that does not depend on how fast the application reacts: the camera service's own log of a leaked stream.
    def test_no_leaked_stream_passes_and_says_so(self) -> None:
        guest = self.Guest(self.plan())
        text = self.run_check(guest)
        self.assertIn("  the camera service reports no leaked stream\n", text)
        self.assertIn("  video viewfinder: the camera service reports no leaked stream\n", text)
        self.assertIn("  photo viewfinder: the camera service reports no leaked stream\n", text)

    def test_a_stream_leaked_at_the_loss_fails_in_every_case(self) -> None:
        for control, case in ((1, "recording"), (2, "video viewfinder"), (3, "photo viewfinder")):
            with self.subTest(case=case):
                guest = self.Guest(self.plan())
                guest.leaks_at = {("interrupt background", control)}
                with self.assertRaisesRegex(runner.Failure, rf"reports a leaked stream after the loss in the {case} \(1 line"):
                    self.run_check(guest)

    def test_a_stream_leaked_by_the_return_fails_in_every_case(self) -> None:
        # The close can come after the person returns: the case's last look sees it.
        for control, case in ((1, "recording"), (2, "video viewfinder"), (3, "photo viewfinder")):
            with self.subTest(case=case):
                guest = self.Guest(self.plan())
                guest.leaks_at = {("resume", control)}
                with self.assertRaisesRegex(runner.Failure, rf"reports a leaked stream after the loss in the {case}"):
                    self.run_check(guest)

    def test_the_failure_names_the_line_and_the_defect(self) -> None:
        guest = self.Guest(self.plan())
        guest.leaks_at = {("interrupt background", 1)}
        with self.assertRaises(runner.Failure) as raised:
            self.run_check(guest)
        self.assertIn("Stream 0 leaked! strong reference (1)!", str(raised.exception))
        self.assertIn("without the framework being told that it failed", str(raised.exception))

    def test_a_leak_from_before_the_row_is_not_the_losss(self) -> None:
        # The log is cumulative: lines that were there when a case began do not count.
        guest = self.Guest(self.plan())
        guest.service_leaks = [self.leak_line(1), self.leak_line(2)]
        text = self.run_check(guest)
        self.assertIn("photo viewfinder: the camera service reports no leaked stream", text)

    def test_the_leak_check_does_not_wait_for_the_application_to_react(self) -> None:
        # The application stays in front and everything else is as in a good run: only the camera service's line decides.
        guest = self.Guest(self.plan())
        guest.leaks_at = {("interrupt background", 2)}
        self.assertTrue(guest.in_front)
        with self.assertRaisesRegex(runner.Failure, "reports a leaked stream after the loss in the video viewfinder"):
            self.run_check(guest)

    def test_the_parser_takes_the_services_leaked_stream_lines_only(self) -> None:
        log = ("11-16 03:12:04.100   588   600 E Camera3-Device: disconnectImpl: Stream 1 leaked! strong reference (2)!\n"
               "11-16 03:12:04.200   588   600 E Camera3-Device: processCaptureResult: Stream 3 error\n"
               "11-16 03:12:04.300   588   600 E Camera3-Device: disconnectImpl: Stream 12 leaked! strong reference (1)!\n"
               "--------- beginning of main\n")
        self.assertEqual([line.split(": ", 1)[1] for line in runner.parse_stream_leaks(log)],
                         ["disconnectImpl: Stream 1 leaked! strong reference (2)!",
                          "disconnectImpl: Stream 12 leaked! strong reference (1)!"])
        self.assertEqual(runner.parse_stream_leaks(""), [])
        self.assertEqual(runner.parse_stream_leaks("11-16 03:12:04.300 588 600 E Camera3-Device: Stream 1 is idle\n"), [])

    def test_the_guest_reads_the_services_error_log_with_timestamps(self) -> None:
        asked: list[str] = []

        class Stub:
            def shell(self, command: str, timeout: float = 0) -> str:
                asked.append(command)
                return VideoLostTests.leak_line(5) + "\n"

        self.assertEqual(len(runner.Guest.stream_leaks(Stub())), 1)
        self.assertEqual(len(asked), 1)
        self.assertIn("logcat -d -v threadtime", asked[0])
        self.assertIn("Camera3-Device:E", asked[0])
        self.assertIn("'*:S'", asked[0])

    # One deadline from the return covers the camera, the picture and the frame counters.
    class Clock:
        def __init__(self) -> None:
            self.now = 0.0

        def monotonic(self) -> float:
            return self.now

        def sleep(self, seconds: float) -> None:
            self.now += seconds

    def timed_viewfinder(self, camera_back: float, picture_ready: float, counters_ready: float = 0.0, mode: str = "video"):
        """The viewfinder case on a virtual clock: the application holds its camera `camera_back` seconds after the return,
        the preview shows the chart `picture_ready` seconds after it, and the frame counters rise from `counters_ready`."""
        clock = self.Clock()
        outer = self

        class Slow(self.Guest):
            returned = None

            def camera_control(self, line: str) -> None:
                super().camera_control(line)
                if line == "resume":
                    self.returned = clock.now

            def media_camera(self) -> str:
                since = None if self.returned is None else clock.now - self.returned
                self.held = {"0"} if since is None or since >= camera_back else set()
                self.stalled = {"photo", "video"} if since is not None and since < counters_ready else set()
                return super().media_camera()

        guest = Slow(self.plan())

        def luma(image):
            since = None if guest.returned is None else clock.now - guest.returned
            return (100.0, 50.0 if since is None or since >= picture_ready else 0.0)

        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out), mock.patch.object(runner.time, "monotonic", clock.monotonic), \
                    mock.patch.object(runner.time, "sleep", clock.sleep), mock.patch.object(runner, "luma_statistics", luma):
                runner.check_viewfinder_returns(guest, guest.plan, mode)
            failure = None
        except runner.Failure as problem:
            failure = problem
        return guest, clock, out.getvalue(), failure

    def test_a_viewfinder_that_is_back_within_the_deadline_passes(self) -> None:
        for mode in ("video", "photo"):
            with self.subTest(mode=mode):
                guest, clock, text, failure = self.timed_viewfinder(5.0, 20.0, 10.0, mode)
                self.assertIsNone(failure, failure)
                self.assertIn("after the return (limit 45 s)", text)
                self.assertLessEqual(clock.now - guest.returned, runner.VIEWFINDER_RECOVERY_SECONDS)

    def test_the_viewfinder_has_one_deadline_not_one_per_wait(self) -> None:
        # The camera is back after 40 s and the chart after 80 s: each wait alone was within its own 45 s before, and the
        # recovery took 80 s; with one deadline from the return the case fails at 45 s.
        guest, clock, _, failure = self.timed_viewfinder(40.0, 80.0)
        self.assertIsNotNone(failure)
        self.assertRegex(str(failure), "preview showing the chart")
        self.assertLessEqual(clock.now - guest.returned, runner.VIEWFINDER_RECOVERY_SECONDS + 1.0)
        # The counters too: the chart is there at 30 s, the counters rise only at 70 s.
        guest, clock, _, failure = self.timed_viewfinder(5.0, 30.0, 70.0)
        self.assertIsNotNone(failure)
        self.assertRegex(str(failure), "live capture")
        self.assertLessEqual(clock.now - guest.returned, runner.VIEWFINDER_RECOVERY_SECONDS + 1.0)

    def on_clock(self, guest, mode: str = "video"):
        """Runs the viewfinder case for `guest` on a virtual clock; returns the clock and the failure (None when it passed)."""
        clock = self.Clock()
        guest.clock = clock
        out = io.StringIO()
        try:
            with contextlib.redirect_stdout(out), mock.patch.object(runner.time, "monotonic", clock.monotonic), \
                    mock.patch.object(runner.time, "sleep", clock.sleep):
                runner.check_viewfinder_returns(guest, guest.plan, mode)
            failure = None
        except runner.Failure as problem:
            failure = problem
        return clock, failure

    def test_a_query_that_returns_after_the_deadline_does_not_pass(self) -> None:
        # The last counter query takes 46 s and answers well: it is 47 s after the return, past the 45 s limit. It used to pass
        # (the limit was looked at before a query, never after it).
        class Slow(self.Guest):
            returned = None
            reads = 0

            def camera_control(self, line: str) -> None:
                super().camera_control(line)
                if line == "resume":
                    self.returned = self.clock.now

            def media_camera(self) -> str:
                if self.returned is not None:
                    self.reads += 1
                    if self.reads == 3:
                        self.clock.sleep(46)
                return super().media_camera()

        guest = Slow(self.plan())
        clock, failure = self.on_clock(guest)
        self.assertIsNotNone(failure)
        self.assertRegex(str(failure), r"live capture: .*the check finished .* s after the limit of")
        # A query that answers in time passes, with the same guest.
        guest = self.Guest(self.plan())
        _, failure = self.on_clock(guest)
        self.assertIsNone(failure)

    def test_a_command_runs_no_longer_than_the_time_left(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            process = mock.Mock(pid=4242)
            process.poll.return_value = None
            log = Path(directory) / "emulator.log"
            log.write_text("")
            guest = runner.Guest("adb", "127.0.0.1:1", process, log, Path(directory))
            clock = self.Clock()
            clock.now = 100.0
            timeouts: list[float] = []

            def run(*arguments, **options):
                timeouts.append(options["timeout"])
                return subprocess.CompletedProcess(arguments, 0, stdout="ok")

            with mock.patch.object(runner.time, "monotonic", clock.monotonic), mock.patch.object(runner.subprocess, "run", run):
                guest.adb_run("shell", "true")
                with runner.bounded(guest, 112.5):
                    guest.adb_run("shell", "true")
                    clock.now = 110.0
                    guest.adb_run("shell", "true", timeout=1.0)
                    clock.now = 112.5
                    with self.assertRaisesRegex(runner.Failure, "the deadline passed before"):
                        guest.adb_run("shell", "true")
                self.assertIsNone(guest.limit)
                guest.adb_run("shell", "true")
            # No limit: the usual bound. A limit: what is left, and 1.0 where the command asked for less.
            self.assertEqual(timeouts, [runner.COMMAND_SECONDS, 12.5, 1.0, runner.COMMAND_SECONDS])

            def expire(*arguments, **options):
                raise subprocess.TimeoutExpired(arguments, options["timeout"])

            with mock.patch.object(runner.subprocess, "run", expire):
                self.assertRaises(subprocess.TimeoutExpired, guest.adb_run, "shell", "true")
                with mock.patch.object(runner.time, "monotonic", clock.monotonic):
                    clock.now = 0.0
                    with runner.bounded(guest, 5.0), self.assertRaisesRegex(runner.Failure, "did not finish by the step's deadline"):
                        guest.adb_run("shell", "true")

    FROZEN = ("Active Camera Clients:\n(Camera ID: 0, Cost: 100, PID: 1301, Score: 0, State: 2User Id: 0, "
              "Client Package Name: org.lineageos.aperture, Conflicting Client Devices: {})\nActive Client listener PIDs: 1\n")

    @staticmethod
    def streams(preview: int | None, other: int, consumer: str = PREVIEW_CONSUMER) -> str:
        text = "    Stream[4]: Output\n      Consumer name: ImageReader-1x1-1-0\n      Dims: 640 x 480, format 0x23\n" \
               f"      Frames produced: {other}, last timestamp: 1 ns\n"
        if preview is not None:
            text = ("    Stream[3]: Output\n      Consumer name: " + consumer + "\n      Dims: 1008 x 756, format 0x23\n"
                    f"      Frames produced: {preview}, last timestamp: 1 ns\n") + text
        return text

    def test_a_frozen_preview_is_not_rescued_by_another_stream(self) -> None:
        # The application holds its camera and shows the picture it showed before the loss; its preview stream produced
        # nothing, and a different output runs. Counters summed over every stream rose, and the case passed.
        class Frozen(self.Guest):
            other = 0

            def media_camera(self) -> str:
                self.other += 1
                return VideoLostTests.FROZEN + VideoLostTests.streams(100, self.other)

        guest = Frozen(self.plan())
        clock, failure = self.on_clock(guest)
        self.assertIsNotNone(failure)
        self.assertRegex(str(failure), r"live capture: a new frame of the video viewfinder after the return preview stream")
        self.assertRegex(str(failure), "counters stand at")
        # The same guest with the preview stream's own counter rising passes.
        class Live(self.Guest):
            count = 0

            def media_camera(self) -> str:
                self.count += 1
                return VideoLostTests.FROZEN + VideoLostTests.streams(100 + self.count, 5)

        _, failure = self.on_clock(Live(self.plan()))
        self.assertIsNone(failure)

    def test_the_preview_stream_may_start_again_with_a_new_count(self) -> None:
        # The camera closes and opens again: the stream is gone for a read, then listed from 0. The read pair that straddles
        # it shows no rise, the next pair does.
        counts = [None, None, 0, 3, 6]

        class Reopening(self.Guest):
            reads = 0

            def media_camera(self) -> str:
                count = counts[min(self.reads, len(counts) - 1)]
                self.reads += 1
                return VideoLostTests.FROZEN + VideoLostTests.streams(count, 7)

        _, failure = self.on_clock(Reopening(self.plan()))
        self.assertIsNone(failure)

    def test_a_dump_without_the_application_s_preview_stream_is_not_a_live_viewfinder(self) -> None:
        class Elsewhere(self.Guest):
            count = 0

            def media_camera(self) -> str:
                self.count += 1
                return VideoLostTests.FROZEN + VideoLostTests.streams(self.count, self.count, "ImageReader-1x1-1-9")

        _, failure = self.on_clock(Elsewhere(self.plan()))
        self.assertIsNotNone(failure)
        self.assertRegex(str(failure), "lists no stream of the application's preview surface")

    def test_the_parser_reads_the_preview_stream_by_its_consumer(self) -> None:
        dump = self.FROZEN + self.streams(12, 99)
        self.assertEqual(runner.preview_stream_frames(dump), {3: 12})
        self.assertEqual(runner.preview_stream_frames(self.FROZEN + self.streams(None, 99)), {})
        self.assertEqual(runner.preview_stream_frames(self.FROZEN + self.streams(12, 99, "SurfaceView[other.app/x]#2")), {})
        # Without a counter the stream is not listed; frames_produced still sums every stream.
        self.assertEqual(runner.preview_stream_frames("    Stream[3]: Output\n      Consumer name: " + PREVIEW_CONSUMER + "\n"), {})
        self.assertEqual(runner.frames_produced(dump), 111)

    def test_a_clip_without_media_data_does_not_pass(self) -> None:
        # A sample table over an empty file (no mdat box, or an empty one, or one too small for the declared samples).
        raw = movie(3.5, payload=0)
        bare = b"".join(raw[start - 8:stop] for kind, start, stop in runner.mp4_boxes(raw, 0, len(raw)) if kind != b"mdat")
        self.assertNotIn(b"mdat", bare)
        self.assertGreater(runner.parse_mp4(bare).frames, 0)
        self.assertEqual(runner.parse_mp4(bare).media_bytes, 0)
        for name, clip in (("no mdat box", bare), ("an empty mdat", raw), ("a tiny mdat", movie(3.5, payload=100))):
            with self.subTest(clip=name, which="the clip the loss saved"):
                guest = self.Guest(self.plan(), movies=clip)
                with self.assertRaisesRegex(runner.Failure, r"declares 105 video samples but its media data holds \d+ bytes"):
                    self.run_check(guest)
            with self.subTest(clip=name, which="the recording after the return"):
                guest = self.Guest(self.plan())
                guest.again_movie = clip
                with self.assertRaisesRegex(runner.Failure, r"declares 105 video samples but its media data holds \d+ bytes"):
                    self.run_check(guest)
        # The smallest payload that passes: MINIMUM_SAMPLE_BYTES for each declared sample.
        needed = 105 * runner.MINIMUM_SAMPLE_BYTES
        self.assertIn("the recording ended by itself",
                      self.run_check(self.Guest(self.plan(), movies=movie(3.5, audio=sound(3.5), payload=needed))))
        with self.assertRaisesRegex(runner.Failure, "media data holds"):
            self.run_check(self.Guest(self.plan(), movies=movie(3.5, payload=needed - 1)))

    def test_the_pipe_takes_one_line_and_is_waited_for(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            pipe = Path(directory) / "control"
            process = mock.Mock(pid=4242)
            process.poll.return_value = None
            log = Path(directory) / "emulator.log"
            log.write_text("")
            guest = runner.Guest("adb", "127.0.0.1:1", process, log, Path(directory), None, pipe)
            self.assertRaises(runner.Failure, runner.Guest("adb", "127.0.0.1:1", process, log, Path(directory)).camera_control, "resume")
            os.mkfifo(pipe)
            reader = os.open(pipe, os.O_RDONLY | os.O_NONBLOCK)
            try:
                guest.camera_control("interrupt background")
                self.assertEqual(os.read(reader, 100), b"interrupt background\n")
            finally:
                os.close(reader)


class VideoLeaveTests(unittest.TestCase):
    """The video-leave mode's readers and its verdict on one leaving: what the launcher says of the edge, what the guest's
    log says of the loss, and the reasons a leaving fails (the live row needs a guest and is run by CTest)."""

    LEFT = "edge control: 1000.000 the application leaves the foreground\n"
    CLOSED = "edge control: 1001.000 the GPU gate closed\n"
    DRAINED = "edge control: 1001.250 GPU requests drained (1) after 1250 ms: video streams 2, stopped yes, answered yes\n"

    def edge(self, **changes) -> runner.Edge:
        edge = runner.Edge(left=1000.0, closed=1001.0, drained=1001.25, result=1, waited_ms=1250, streams=2, stopped=True,
                           answered=True)
        return dataclasses.replace(edge, **changes)

    def trial(self, edge: runner.Edge | None = None, events: dict[str, float] | None = None, skew: float = 0.4,
              video_end: float = 5.0, audio_end: float = 5.1, clock_error: float = 0.01,
              record_bound_hit: bool = False) -> runner.LeaveTrial:
        # The guest's clock is `skew` ahead: the loss at 1000 is 1000.4 on it, the gate's closing at 1001 is 1001.4.
        found = {"hal_end": 1000.42, "rec_stopping": 1000.5, "venc_stopped": 1001.5, "finalize": 1001.7} if events is None else events
        return runner.LeaveTrial(1, 0.0, edge or self.edge(), 1000.0, skew, clock_error, found, 5.2, video_end, audio_end,
                                 record_bound_hit)

    def test_the_leaving_is_read_from_the_launchers_log_after_a_point(self) -> None:
        log = "before\n" + self.LEFT + "noise\n" + self.CLOSED + self.DRAINED
        self.assertEqual(runner.parse_edge(log), self.edge())
        self.assertIsNone(runner.parse_edge(log[:log.index(self.CLOSED)]), "no closing yet: no leaving to judge")
        self.assertIsNone(runner.parse_edge(log[:log.index(self.DRAINED)]), "a closed gate whose wait has not ended is not done")
        self.assertIsNone(runner.parse_edge(log, log.index(self.CLOSED)), "a hold alone is not a leaving")
        self.assertIsNone(runner.parse_edge(log, len(log)), "nothing after the point")
        second = log + "edge control: 2000.000 the application leaves the foreground\n" + \
            self.CLOSED.replace("1001.000", "2000.500") + self.DRAINED.replace("1001.250", "2001.000").replace("stopped yes", "stopped no")
        found = runner.parse_edge(second, len(log))
        self.assertEqual((found.left, found.closed, found.drained, found.stopped), (2000.0, 2000.5, 2001.0, False),
                         "the first leaving after the point")

    def test_the_loss_is_read_from_the_guests_log(self) -> None:
        log = ("--------- beginning of main\n"
               "999.000 1 1 W V4l2FrameSource: the host ended the camera: its video buffers fail (before the point)\n"
               "1000.420 1 1 W V4l2FrameSource: NoteFailedBuffer: the host ended the camera: its video buffers fail under "
               "an interrupted record (flags 3)\n"
               "1000.425 1 2 W EmulatedSensorHost: CaptureFromHost: frame 9 failed: the host ended the camera\n"
               "1000.430 1 2 W EmulatedSensorHost: CaptureFromHost: frame 10 failed: none of its 2 outputs was produced\n"
               "1000.500 2 2 D Recorder: Transitioning Recorder internal state: RECORDING --> STOPPING\n"
               "1001.500 2 3 D VideoEncoder: Transitioning encoder internal state: STOPPING --> CONFIGURED\n"
               "1001.700 2 2 D Recorder: Sending VideoRecordEvent VideoRecordEvent$Finalize\n"
               "1001.800 2 2 D Recorder: Sending VideoRecordEvent VideoRecordEvent$Finalize\n")
        self.assertEqual(runner.leave_events(log, 1000.0),
                         {"hal_end": 1000.42, "hal_frame_failed": 1000.43, "rec_stopping": 1000.5, "venc_stopped": 1001.5,
                          "finalize": 1001.7})

    def test_a_leaving_that_ended_the_recording_before_the_hold_passes(self) -> None:
        self.assertEqual(runner.leave_failures(self.trial()), [])
        # The report may come a little before the gate's closing in the guest's clock and still within the margin after it.
        self.assertEqual(runner.leave_failures(self.trial(events={"hal_end": 1001.5, "rec_stopping": 1001.5})), [])
        # A busy host's round trips make the offset less certain: the margin follows them, never below the least.
        late = {"hal_end": 1001.8, "rec_stopping": 1001.8}
        self.assertNotEqual(runner.leave_failures(self.trial(events=late)), [])
        self.assertEqual(runner.leave_failures(self.trial(events=late, clock_error=0.6)), [])

    def test_a_stop_after_the_gate_closed_is_not_before_the_hold_when_the_wait_for_a_request_ended_later(self) -> None:
        # The gate closed at 1001.0 and the launcher's call returned at 1002.0, a second later, after waiting for a request that
        # was running. The HAL's report (1001.5) and the recorder's stop (1001.6) came after the gate closed: they did not
        # beat the hold, though both are before the time the call returned.
        trial = self.trial(edge=self.edge(closed=1001.0, drained=1002.0, result=0, waited_ms=2000), skew=0.0,
                           events={"hal_end": 1001.5, "rec_stopping": 1001.6})
        reasons = "; ".join(runner.leave_failures(trial))
        self.assertIn("the camera HAL's report of the loss came", reasons)
        self.assertIn("the recorder's stop came", reasons)
        self.assertIn("after the gate closed", reasons)

    def test_every_reason_a_leaving_fails_is_said(self) -> None:
        def reasons(**changes) -> str:
            return "; ".join(runner.leave_failures(self.trial(**changes)))
        self.assertEqual(reasons(edge=self.edge(stopped=False, waited_ms=2000)), "",
                         "a wait that ran out is told, and a recording that ended before the hold is a pass")
        self.assertIn("NOT stopped", runner.describe_leave(self.trial(edge=self.edge(stopped=False, waited_ms=2000))))
        self.assertIn("did not answer", reasons(edge=self.edge(answered=False, streams=0, stopped=False)))
        self.assertIn("no video stream to end", reasons(edge=self.edge(streams=0)))
        late = {"hal_end": 1004.0, "rec_stopping": 1004.1}
        self.assertIn("the camera HAL's report of the loss came", reasons(events=late))
        self.assertIn("the recorder's stop came", reasons(events=late))
        self.assertIn("recorder's stop is not in the guest's log", reasons(events={"hal_end": 1000.42}))
        self.assertIn("HAL's report of the loss is not in the guest's log", reasons(events={"rec_stopping": 1000.5}))
        self.assertIn("ordinary failure", reasons(events={"hal_end": 1000.42, "rec_stopping": 1000.5, "hal_frame_failed": 1000.41}))
        self.assertEqual(reasons(events={"hal_end": 1000.42, "rec_stopping": 1000.5, "hal_frame_failed": 1000.43}), "")
        self.assertIn("did not read the interrupted record", reasons(record_bound_hit=True))
        self.assertIn("recording went on in silence", reasons(video_end=2.9, audio_end=9.13))
        self.assertEqual(reasons(video_end=2.9, audio_end=3.5), "", "a tail within a second is a clip ending, not a stall")

    def test_the_mode_has_its_own_pipes_and_a_trial_count(self) -> None:
        self.assertIn("video-leave", runner.MODES)
        plan = runner.make_plan("bf-small-flash", "phone", "video-leave", leave_trials=3)
        self.assertEqual(plan.leave_trials, 3)
        self.assertEqual(runner.make_plan("bf-small-flash", "phone", "video-leave").leave_trials, runner.LEAVE_TRIALS)
        with self.assertRaises(ValueError):
            runner.make_plan("bf-small-flash", "phone", "video-leave", leave_trials=0)
        self.assertEqual(runner.EDGE_VARIABLE, "UML_BACKGROUND_EDGE_CONTROL")

    def test_the_edge_pipe_takes_one_line_and_is_waited_for(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            pipe = Path(directory) / "edge"
            process = mock.Mock(pid=4242)
            process.poll.return_value = None
            log = Path(directory) / "emulator.log"
            log.write_text("")
            guest = runner.Guest("adb", "127.0.0.1:1", process, log, Path(directory), edge=pipe)
            self.assertRaises(runner.Failure, runner.Guest("adb", "127.0.0.1:1", process, log, Path(directory)).edge_control, "leave")
            os.mkfifo(pipe)
            reader = os.open(pipe, os.O_RDONLY | os.O_NONBLOCK)
            try:
                guest.edge_control("leave")
                self.assertEqual(os.read(reader, 100), b"leave\n")
            finally:
                os.close(reader)

    def test_a_trial_is_described_in_milliseconds_from_the_loss(self) -> None:
        line = runner.describe_leave(self.trial())
        for part in ("waited 1250 ms for 2 stream(s) (stopped)", "HAL report +20", "Recorder STOPPING +100", "Finalize +1300",
                     "GPU gate closed +1000", "drained +1250", "audio ends 5.10 s (+0.10 s)"):
            self.assertIn(part, line)


def dng_bytes(width: int = 2016, height: int = 1512, bits: int = 16, version: bool = True, matrix: bool = True,
              in_sub_ifd: bool = False, photometric: int = 32803) -> bytes:
    """A little-endian TIFF laid out the way a DNG is: IFD0 with the DNG version and the colour matrix, and the colour filter
    array image in IFD0 or in a SubIFD."""
    def entry(tag: int, kind: int, count: int, value: int | bytes) -> bytes:
        field = struct.pack("<I", value) if isinstance(value, int) else value.ljust(4, b"\0")
        return struct.pack("<HHI", tag, kind, count) + field

    def ifd(entries: list[bytes]) -> bytes:
        ordered = sorted(entries, key=lambda item: struct.unpack_from("<H", item)[0])
        return struct.pack("<H", len(ordered)) + b"".join(ordered) + struct.pack("<I", 0)

    def image(extra: list[bytes]) -> list[bytes]:
        return [entry(0x0100, 4, 1, width), entry(0x0101, 4, 1, height), entry(0x0102, 3, 1, struct.pack("<H", bits)),
                entry(0x0106, 3, 1, struct.pack("<H", photometric))] + extra

    top_entries = ([entry(0xC612, 1, 4, bytes([1, 4, 0, 0]))] if version else [])
    top_count = len(top_entries) + (1 if matrix else 0) + (1 if in_sub_ifd else 4 + 0)
    top_count = len(top_entries) + (1 if matrix else 0) + (1 if in_sub_ifd else 4)
    top_size = 2 + 12 * top_count + 4
    sub_offset = 8 + top_size
    sub_size = 2 + 12 * 4 + 4
    data_offset = sub_offset + (sub_size if in_sub_ifd else 0)
    if matrix:
        top_entries.append(entry(0xC621, 10, 9, data_offset))
    if in_sub_ifd:
        top_entries.append(entry(0x014A, 4, 1, sub_offset))
    else:
        top_entries += image([])
    body = b"II*\0" + struct.pack("<I", 8) + ifd(top_entries)
    if in_sub_ifd:
        body += ifd(image([]))
    return body + (b"\x01\0\0\0\x01\0\0\0" * 9 if matrix else b"")


class DngTests(unittest.TestCase):
    def test_a_dng_gives_its_directory_tags_and_the_size_of_its_colour_filter_array_image(self) -> None:
        for in_sub_ifd in (False, True):
            with self.subTest(in_sub_ifd=in_sub_ifd):
                dng = runner.parse_dng(dng_bytes(in_sub_ifd=in_sub_ifd))
                self.assertEqual((dng.width, dng.height, dng.bits), (2016, 1512, 16))
                self.assertIn(runner.DNG_VERSION, dng.tags)
                self.assertIn(runner.DNG_COLOR_MATRIX1, dng.tags)
        self.assertNotIn(runner.DNG_VERSION, runner.parse_dng(dng_bytes(version=False)).tags)

    def test_a_file_that_is_not_a_tiff_with_a_colour_filter_array_image_is_refused(self) -> None:
        with self.assertRaisesRegex(ValueError, "not a TIFF"):
            runner.parse_dng(b"\xff\xd8\xff\xe0 a JPEG")
        with self.assertRaisesRegex(ValueError, "no colour filter array image"):
            runner.parse_dng(dng_bytes(photometric=2))
        with self.assertRaisesRegex(ValueError, "truncated"):
            runner.parse_dng(dng_bytes()[:20])


def ui_xml(*nodes: tuple[str, str, str, bool, tuple[int, int, int, int]]) -> str:
    """`uiautomator dump` of a screen: nodes as (resource id, text, class, checked, bounds)."""
    body = "".join(f'<node text="{text}" resource-id="{"org.lineageos.aperture:id/" + ident if ident else ""}" class="{kind}" '
                   f'checked="{str(checked).lower()}" bounds="[{box[0]},{box[1]}][{box[2]},{box[3]}]" />'
                   for ident, text, kind, checked, box in nodes)
    return f'<?xml version="1.0" encoding="UTF-8"?><hierarchy rotation="0">{body}</hierarchy>'


PRO_BUTTON_NODE = (runner.PRO_BUTTON, "", "android.widget.ImageButton", False, (600, 100, 700, 200))
SETTINGS_BUTTON_NODE = (runner.SETTINGS_BUTTON, "", "android.widget.ImageButton", False, (500, 200, 600, 300))
RAW_ROW_NODE = ("", runner.RAW_SETTING, "android.widget.TextView", False, (50, 500, 500, 600))


def raw_switch_node(checked: bool):
    return ("", "", "android.widget.Switch", checked, (600, 520, 700, 580))


class ApplicationUi:
    """The camera application's screens that the RAW setting is reached through: the viewfinder (the pro mode button), the
    row of buttons it opens (the settings button) and the settings list (the row of the setting, with its switch). A tap on
    the centre of an element moves between them; `sticky` is an application whose row does not toggle."""
    def __init__(self, on: bool = False, sticky: bool = False, rowless: bool = False) -> None:
        self.screen, self.on, self.sticky, self.rowless = "photo", on, sticky, rowless
        self.commands: list[str] = []
        self.taps: list[tuple[int, int]] = []

    def alive(self) -> None:
        pass

    def screens(self) -> str:
        if self.screen == "photo":
            return ui_xml(PRO_BUTTON_NODE)
        if self.screen == "pro":
            return ui_xml(PRO_BUTTON_NODE, SETTINGS_BUTTON_NODE)
        return ui_xml(*(() if self.rowless else (RAW_ROW_NODE, raw_switch_node(self.on))))

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if "uiautomator dump" in command:
            return self.screens()
        if command.startswith("am start"):
            self.screen = "photo"
        if (tap := re.fullmatch(r"input tap (\d+) (\d+)", command)):
            x, y = int(tap[1]), int(tap[2])
            self.taps.append((x, y))
            if self.screen == "photo" and (x, y) == (650, 150):
                self.screen = "pro"
            elif self.screen == "pro" and (x, y) == (550, 250):
                self.screen = "settings"
            elif self.screen == "settings" and (x, y) == (275, 550) and not self.sticky:
                self.on = not self.on
        return ""


class RawCaptureTests(unittest.TestCase):
    def enable(self, guest) -> None:
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "wait_for_chart"):
            runner.enable_raw_capture(guest)

    def test_the_setting_is_reached_through_the_pro_button_and_the_settings_and_switched_on(self) -> None:
        guest = ApplicationUi()
        self.enable(guest)
        self.assertEqual(guest.taps, [(650, 150), (550, 250), (275, 550)])
        self.assertTrue(guest.on)
        self.assertEqual(guest.commands[0], f"am force-stop {runner.CAMERA_PACKAGE}")
        self.assertEqual(guest.commands[-1], f"am force-stop {runner.CAMERA_PACKAGE}")
        self.assertTrue(any(command.startswith("input swipe") for command in guest.commands), "the list is scrolled to the row")

    def test_a_setting_that_is_on_already_is_left_on(self) -> None:
        guest = ApplicationUi(on=True)
        self.enable(guest)
        self.assertEqual(guest.taps, [(650, 150), (550, 250)])
        self.assertTrue(guest.on)

    def test_a_settings_list_without_the_row_or_a_row_that_does_not_toggle_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "settings show no row 'Enable RAW image capture'"):
            self.enable(ApplicationUi(rowless=True))
        with self.assertRaisesRegex(runner.Failure, "'Enable RAW image capture' is not on after the tap on its row"):
            self.enable(ApplicationUi(sticky=True))

    def test_the_dump_is_asked_again_when_the_tool_prints_nothing(self) -> None:
        answers = ["", "ERROR: could not get idle state.", ui_xml(PRO_BUTTON_NODE)]

        class Guest:
            def __init__(self) -> None:
                self.commands = []

            def alive(self) -> None:
                pass

            def shell(self, command: str, timeout: float = 0) -> str:
                self.commands.append(command)
                return answers.pop(0)

        guest = Guest()
        with mock.patch.object(runner.time, "sleep"):
            dump = runner.ui_dump(guest)
        self.assertTrue(dump.startswith("<hierarchy"))
        self.assertEqual(len(guest.commands), 3)
        self.assertTrue(all(command.startswith(f"rm -f {runner.UI_DUMP};") for command in guest.commands),
                        "a failed dump must not leave the last one to be read")
        answers[:] = [""] * 5
        with mock.patch.object(runner.time, "sleep"), self.assertRaisesRegex(runner.Failure, "printed no hierarchy in 3 attempts"):
            runner.ui_dump(guest)

    def test_the_nodes_carry_their_ids_texts_and_centres(self) -> None:
        nodes = runner.parse_ui_nodes(ui_xml(PRO_BUTTON_NODE, RAW_ROW_NODE, raw_switch_node(True)))
        self.assertEqual([(node.id, node.text, node.x, node.y, node.checked) for node in nodes],
                         [(runner.PRO_BUTTON, "", 650, 150, False), ("", runner.RAW_SETTING, 275, 550, False), ("", "", 650, 550, True)])


class RawStillGuest:
    """A guest that has a camera folder: the files listed before the camera key and after it."""
    def __init__(self, plan, files_after: set[str], dng: bytes, raw_in: set[str] = frozenset({"0", "3", "4"})) -> None:
        self.before, self.after, self.dng, self.pulled = {"IMG_old.jpg"}, files_after, dng, []
        self.listed = set(self.before)
        self.static = "".join(static_info(device, "BACK", "BACKWARD_COMPATIBLE MANUAL_SENSOR RAW" if device in raw_in
                                          else "BACKWARD_COMPATIBLE MANUAL_SENSOR", "TRUE", "OFF AUTO",
                                          raw_metadata() if device in raw_in else "") for device in ("0", "3", "4"))

    def alive(self) -> None:
        pass

    def shell(self, command: str, timeout: float = 0) -> str:
        if command.startswith("input keyevent"):
            self.listed = set(self.after)
        if command.startswith("ls "):
            return "\n".join(sorted(self.listed))
        return ""

    def media_camera(self) -> str:
        return self.static

    def pull(self, remote: str) -> bytes:
        self.pulled.append(remote)
        return self.dng


class RawStillTests(unittest.TestCase):
    NEW = {"IMG_old.jpg", "IMG_new.jpg", "IMG_new.dng"}

    def still(self, guest, seconds: float = 0.05) -> str:
        plan = runner.make_plan("bf-small-flash-manual-raw", "phone", "live")
        out = io.StringIO()
        with mock.patch.object(runner.time, "sleep"), mock.patch.object(runner, "enable_raw_capture"), \
                mock.patch.object(runner, "start_camera_application"), mock.patch.object(runner, "wait_for_chart"), \
                mock.patch.object(runner, "RAW_STILL_SECONDS", seconds), contextlib.redirect_stdout(out):
            runner.check_raw_still(guest, plan)
        return out.getvalue()

    def test_a_dng_beside_the_jpeg_of_the_primary_back_camera_passes(self) -> None:
        plan = runner.make_plan("bf-small-flash-manual-raw", "phone", "live")
        guest = RawStillGuest(plan, self.NEW, dng_bytes())
        printed = self.still(guest)
        self.assertEqual(guest.pulled, [f"{runner.STILL_DIRECTORY}/IMG_new.dng"])
        self.assertIn("RAW still: IMG_new.dng 2016x1512 at 16 bits", printed)

    def test_a_still_without_a_dng_fails_with_the_files_it_made(self) -> None:
        plan = runner.make_plan("bf-small-flash-manual-raw", "phone", "live")
        with self.assertRaisesRegex(runner.Failure, r"a DNG and a JPEG in the camera folder: new files \['IMG_new\.jpg'\]: DNG \[\], JPEG \['IMG_new\.jpg'\]"):
            self.still(RawStillGuest(plan, {"IMG_old.jpg", "IMG_new.jpg"}, dng_bytes()))

    def test_a_dng_that_is_not_a_dng_or_lacks_its_version_or_colour_matrix_fails(self) -> None:
        plan = runner.make_plan("bf-small-flash-manual-raw", "phone", "live")
        cases = ((b"\xff\xd8 a JPEG", "is not a DNG: not a TIFF file"), (dng_bytes(version=False), "has no DNGVersion"),
                 (dng_bytes(matrix=False), "has no ColorMatrix1"), (dng_bytes(width=64, height=48), "colour filter array image of 64x48"),
                 (dng_bytes(bits=8), "at 8 bits"))
        for data, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(runner.Failure, message):
                self.still(RawStillGuest(plan, self.NEW, data))

    def test_a_primary_back_camera_without_raw_has_no_raw_request_to_take(self) -> None:
        plan = runner.make_plan("bf-small-flash-manual-raw", "phone", "live")
        with self.assertRaisesRegex(runner.Failure, r"the primary back camera 0 states no RAW \(the cameras with RAW are \['3', '4'\]\)"):
            self.still(RawStillGuest(plan, self.NEW, dng_bytes(), raw_in={"3", "4"}))


if __name__ == "__main__":
    unittest.main()
