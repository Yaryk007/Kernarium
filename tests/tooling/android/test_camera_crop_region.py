#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A result of the camera HAL states the crop region its centred camera applies.

For a HAL that lists the zoom ratio the camera service splits an application's zoom into CONTROL_ZOOM_RATIO and a residual
SCALER_CROP_REGION, and joins them again in the result by unscaling the result's crop region by the result's ratio. The stock
emulated request state (EmulatedRequestState::InitializeResult) set the result's region to the whole active array, which lost
the residual: on a 4032 x 3024 front camera the region CTS testDigitalZoom got back above about 57x was a pixel or three off
(53x40 for 52x38, 47x35 for 44x34). The route's last topic states the region the camera applies, by the rule of the overlay's
camera_crop.hpp (its unit row executes the rule and the whole request-to-result round trip through the session layer's zoom
ratio arithmetic): the requested size, centred in the array, clamped to the array and to the smallest size the maximum digital
zoom allows. The session layer has divided the region by the ratio before the request state sees it, so the ratio is not an
input of the rule.

The stock source is not in this repository, so this row is a structural check: it reads what the series' patches do to it, the
lines they add and remove, and does not execute the HAL. Without the topic the call that stated the whole array is still there and
the rule is not used. Nothing here boots anything.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import re
import unittest

REPOSITORY = Path(__file__).resolve().parents[3]
SERIES = Path(os.environ.get("UML_CAMERA_HAL_SERIES") or REPOSITORY / "android/lineage-24/patches/projects/p0030/series")
HEADER = REPOSITORY / "android/lineage-24/overlay/camera/hal/camera_crop.hpp"
PATH = "devices/EmulatedCamera/hwl/EmulatedRequestState.cpp"


def patch_names() -> list[str]:
    return [line.split("\t", 1)[1] for line in (SERIES / "series.tsv").read_text(encoding="utf-8").splitlines()
            if line.startswith("patch\t")]


def changed_lines(prefix: str) -> str:
    """The lines (`+` or `-`) that the series' patches add to or remove from the request state's source, in series order."""
    found: list[str] = []
    for name in patch_names():
        inside = False
        for line in (SERIES / name).read_text(encoding="utf-8").splitlines():
            if line.startswith("diff --git "):
                inside = line.startswith(f"diff --git a/{PATH} ")
            elif inside and line.startswith(prefix) and not line.startswith(prefix * 3):
                found.append(line[1:])
    return "\n".join(found)


class CropRegionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.added = changed_lines("+")
        cls.removed = changed_lines("-")

    def test_the_result_no_longer_states_the_whole_array(self) -> None:
        # The stock call: the region of the result is the array (or the maximum resolution array), whatever was asked.
        stock = re.compile(r"Set\(ANDROID_SCALER_CROP_REGION,\s*chosen_crop_region,")
        self.assertRegex(self.removed, stock, "the series does not remove the call that states the whole array")
        self.assertNotRegex(self.added, stock, "and does not put it back")

    def test_the_result_states_the_region_the_camera_applies(self) -> None:
        # The region is read from the request's own settings, not from the device info.
        self.assertRegex(self.added, r"request_settings_->Get\(ANDROID_SCALER_CROP_REGION, &asked\)")
        self.assertRegex(self.added, r"andarium::camera::result_crop_region\(\s*has_asked \? asked\.data\.i32 : nullptr,\s*"
                                     r"has_asked \? asked\.count : 0,")
        # ... centred in the array of the sensor mode in use, which is the chosen one (the default or the maximum resolution
        # array), with the camera's maximum digital zoom and no zoom ratio.
        self.assertRegex(self.added, r"\{chosen_crop_region\[0\], chosen_crop_region\[1\], chosen_crop_region\[2\],\s*"
                                     r"chosen_crop_region\[3\]\},\s*info\.max_zoom_\)")
        self.assertNotRegex(self.added, r"result_crop_region\([^;]*zoom_ratio_", "the rule takes no zoom ratio")
        self.assertRegex(self.added, r"Set\(ANDROID_SCALER_CROP_REGION, crop_region,")

    def test_the_rule_is_the_overlays_header(self) -> None:
        self.assertIn('#include "camera_crop.hpp"', self.added)
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("result_crop_region", header)
        self.assertTrue(header.startswith("// SPDX-License-Identifier: Apache-2.0"))

    def test_the_topic_is_in_the_series_after_the_ones_it_builds_on(self) -> None:
        names = patch_names()
        crop = [index for index, name in enumerate(names) if "state-the-crop-region-a-centred-camera-applies" in name]
        self.assertEqual(len(crop), 1, names)
        self.assertGreater(crop[0], 0)


if __name__ == "__main__":
    unittest.main()
