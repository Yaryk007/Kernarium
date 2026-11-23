#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera layouts: their closed set, their fallback order and the generated files.

camera_layouts.py is the only source of the layout names, the permission
directories, the media profiles and the init file; overlay/camera/generated holds
its output. The tests below state the set (39 names, the dimensions that make
them), the fallback order that the runtime composes with, and that every checked-in
file is what the generator renders, that the makefile installs each of them, and
that every file parses.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import re
import sys
import unittest
from xml.etree import ElementTree

TESTS = Path(__file__).resolve().parents[2]
REPOSITORY = TESTS.parent
SCRIPT = REPOSITORY / "android/lineage-24/tools/camera_layouts.py"
SPEC = importlib.util.spec_from_file_location("camera_layouts", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)

GENERATED = REPOSITORY / "android/lineage-24/overlay/camera/generated"


class LayoutSet(unittest.TestCase):
    def test_the_set_has_39_unique_layouts(self):
        names = [layout.name for layout in generator.all_layouts()]
        self.assertEqual(len(names), 39)
        self.assertEqual(len(set(names)), 39)
        self.assertEqual(names[:3], ["none", "front", "front-manual"])
        back = [layout for layout in generator.all_layouts() if layout.has_back]
        self.assertEqual(len(back), 36)
        # 2 small x 2 flash x 3 manual states x 2 concurrent, and the large tier twice (front 1080p, 2160p).
        self.assertEqual(sum(1 for layout in back if layout.tier == "small"), 12)
        self.assertEqual(sum(1 for layout in back if layout.tier == "large" and not layout.front2160), 12)
        self.assertEqual(sum(1 for layout in back if layout.front2160), 12)

    def test_a_name_says_its_dimensions_and_nothing_else(self):
        for layout in generator.all_layouts():
            if not layout.has_back:
                continue
            parts = layout.name.split("-")
            self.assertEqual(parts[:2], ["bf", layout.tier])
            self.assertEqual("flash" in parts, layout.flash, layout.name)
            self.assertEqual("manual" in parts, layout.manual, layout.name)
            self.assertEqual("raw" in parts, layout.raw, layout.name)
            self.assertEqual("concurrent" in parts, layout.concurrent, layout.name)
            self.assertEqual("front2160" in parts, layout.front2160, layout.name)
            self.assertTrue(set(parts) <= {"bf", "small", "large", "flash", "manual", "raw", "concurrent",
                                           "front2160"}, layout.name)

    def test_no_layout_has_an_impossible_combination(self):
        for layout in generator.all_layouts():
            if layout.raw:
                self.assertTrue(layout.manual, f"{layout.name}: RAW without manual control")
            if layout.front2160:
                self.assertEqual(layout.tier, "large", f"{layout.name}: front 2160p in the small tier")
            if not layout.has_back:
                self.assertFalse(layout.flash or layout.raw or layout.concurrent or layout.front2160, layout.name)

    def test_names_are_safe_in_a_command_line_and_an_init_trigger(self):
        for layout in generator.all_layouts():
            self.assertRegex(layout.name, r"^[a-z0-9]+(-[a-z0-9]+)*$")
            # A boot property's value holds 91 characters.
            self.assertLessEqual(len(layout.name), 64)


class Features(unittest.TestCase):
    def test_features_follow_the_dimensions(self):
        layouts = generator.by_name()
        self.assertEqual(layouts["none"].features, ())
        self.assertEqual(layouts["front"].features,
                         ("android.hardware.camera.any", "android.hardware.camera.front"))
        self.assertEqual(layouts["front-manual"].features,
                         ("android.hardware.camera.any", "android.hardware.camera.front",
                          "android.hardware.camera.capability.manual_sensor"))
        full = layouts["bf-large-flash-manual-raw-concurrent-front2160"].features
        self.assertEqual(full, (
            "android.hardware.camera.any", "android.hardware.camera", "android.hardware.camera.front",
            "android.hardware.camera.autofocus", "android.hardware.camera.flash",
            "android.hardware.camera.capability.manual_sensor", "android.hardware.camera.capability.raw",
            "android.hardware.camera.concurrent"))
        self.assertEqual(layouts["bf-small"].features, (
            "android.hardware.camera.any", "android.hardware.camera", "android.hardware.camera.front",
            "android.hardware.camera.autofocus"))

    def test_no_layout_declares_a_level_the_hal_does_not_have(self):
        for layout in generator.all_layouts():
            for feature in layout.features:
                self.assertNotIn("level.full", feature)
                self.assertNotIn("manual_post_processing", feature)

    def test_a_back_camera_feature_needs_a_back_camera(self):
        for layout in generator.all_layouts():
            if not layout.has_back:
                self.assertNotIn("android.hardware.camera", layout.features)
                self.assertNotIn("android.hardware.camera.autofocus", layout.features)


class Fallback(unittest.TestCase):
    def test_every_chain_ends_in_none_and_names_existing_layouts(self):
        names = set(generator.by_name())
        for layout in generator.all_layouts():
            chain = generator.fallback_chain(layout.name)
            self.assertEqual(chain[0], layout.name)
            self.assertEqual(chain[-1], "none")
            self.assertTrue(set(chain) <= names, layout.name)
            self.assertEqual(len(chain), len(set(chain)), f"{layout.name}: a layout twice in its chain")

    def test_the_largest_layout_falls_back_one_dimension_at_a_time_in_the_set_order(self):
        # concurrent, then RAW, then the front camera's 2160p, then the flash, then the tier,
        # then manual control, then "front", then "none".
        self.assertEqual(generator.fallback_chain("bf-large-flash-manual-raw-concurrent-front2160"), [
            "bf-large-flash-manual-raw-concurrent-front2160",
            "bf-large-flash-manual-raw-front2160",
            "bf-large-flash-manual-front2160",
            "bf-large-flash-manual",
            "bf-large-manual",
            "bf-small-manual",
            "bf-small",
            "front",
            "none"])

    def test_each_step_drops_exactly_one_dimension(self):
        layouts = generator.by_name()
        for layout in generator.all_layouts():
            chain = [layouts[name] for name in generator.fallback_chain(layout.name)]
            for earlier, later in zip(chain, chain[1:]):
                if later.name in ("front", "none") or earlier.name in ("front", "front-manual"):
                    continue
                before = (earlier.tier, earlier.flash, earlier.manual, earlier.raw, earlier.concurrent,
                          earlier.front2160)
                after = (later.tier, later.flash, later.manual, later.raw, later.concurrent, later.front2160)
                changed = [index for index in range(6) if before[index] != after[index]]
                # Dropping manual control drops RAW with it, and leaving the large tier drops front 2160p.
                self.assertLessEqual(len(changed), 2, (earlier.name, later.name))

    def test_a_chain_never_gains_a_feature(self):
        layouts = generator.by_name()
        for layout in generator.all_layouts():
            earlier = set(layout.features)
            for name in generator.fallback_chain(layout.name)[1:]:
                later = set(layouts[name].features)
                self.assertTrue(later <= earlier, f"{layout.name} -> {name} gains {sorted(later - earlier)}")
                earlier = later

    def test_resolve_takes_the_first_layout_the_image_has(self):
        wanted = generator.by_name()["bf-large-flash-manual-raw-concurrent-front2160"]
        everything = set(generator.by_name())
        self.assertEqual(generator.resolve(wanted, everything), wanted.name)
        without = everything - {wanted.name, "bf-large-flash-manual-raw-front2160"}
        self.assertEqual(generator.resolve(wanted, without), "bf-large-flash-manual-front2160")
        self.assertEqual(generator.resolve(wanted, {"front", "none"}), "front")
        self.assertEqual(generator.resolve(wanted, {"none"}), "none")
        self.assertEqual(generator.resolve(wanted, set()), "none")

    def test_front_only_layouts_never_fall_back_to_a_back_camera(self):
        self.assertEqual(generator.fallback_chain("front-manual"), ["front-manual", "front", "none"])
        self.assertEqual(generator.fallback_chain("front"), ["front", "none"])
        self.assertEqual(generator.fallback_chain("none"), ["none"])


class MediaProfiles(unittest.TestCase):
    def test_every_layout_with_cameras_has_a_variant_file_and_none_keeps_the_default(self):
        files = generator.render()
        for layout in generator.all_layouts():
            if layout.name == "none":
                self.assertEqual(layout.media_variant, "")
                continue
            self.assertIn(f"media_profiles{layout.media_variant}.xml", files, layout.name)

    def test_the_variant_follows_the_tier_and_the_front_camera(self):
        layouts = generator.by_name()
        self.assertEqual(layouts["front"].media_variant, "_camfront")
        self.assertEqual(layouts["front-manual"].media_variant, "_camfront")
        self.assertEqual(layouts["bf-small-flash-manual-raw-concurrent"].media_variant, "_camsmall")
        self.assertEqual(layouts["bf-large-flash"].media_variant, "_camlarge")
        self.assertEqual(layouts["bf-large-flash-front2160"].media_variant, "_camlarge2160")

    def _profiles(self, variant: str):
        root = ElementTree.fromstring(generator.render()[f"media_profiles{variant}.xml"])
        result = {}
        for profiles in root.findall("CamcorderProfiles"):
            result[int(profiles.get("cameraId"))] = {
                profile.get("quality"): (int(profile.find("Video").get("width")),
                                         int(profile.find("Video").get("height")))
                for profile in profiles.findall("EncoderProfile")}
        cap = next(element for element in root.findall("VideoEncoderCap") if element.get("name") == "h264")
        return result, (int(cap.get("maxFrameWidth")), int(cap.get("maxFrameHeight")), int(cap.get("maxBitRate")))

    def test_the_caps_cover_the_largest_profile_of_the_variant(self):
        for variant in generator.media_variants():
            profiles, (width, height, bit_rate) = self._profiles(variant)
            sizes = [size for per_camera in profiles.values() for size in per_camera.values()]
            self.assertEqual(max(size[0] for size in sizes), width, variant)
            self.assertEqual(max(size[1] for size in sizes), height, variant)
            root = ElementTree.fromstring(generator.render()[f"media_profiles{variant}.xml"])
            for video in root.iter("Video"):
                self.assertLessEqual(int(video.get("bitRate")), bit_rate, variant)

    def test_the_small_tier_stops_at_1080p_and_the_front_at_1080p_unless_asked(self):
        profiles, caps = self._profiles("_camsmall")
        self.assertEqual(caps[:2], (1920, 1080))
        for camera in profiles.values():
            self.assertNotIn("2160p", camera)
        profiles, _ = self._profiles("_camlarge")
        self.assertIn("2160p", profiles[0])
        self.assertNotIn("2160p", profiles[1])
        profiles, _ = self._profiles("_camlarge2160")
        self.assertIn("2160p", profiles[1])

    def test_the_recording_size_is_the_largest_video_mode_of_the_tier(self):
        # The HAL adds a JPEG at the camera's recording size (the largest profile CTS reads), cut from the video mode
        # of that size: that is only the host's largest mode when the media profiles stop where the tier's video does.
        text = (REPOSITORY / "darwin/device/camera/camera_tiers.hpp").read_text()
        limits = re.search(r"tier == CameraTier::small \? CameraTierLimits \{(\d+), (\d+),[^}]*\}\s*:\s*"
                           r"CameraTierLimits \{(\d+), (\d+),", text)
        self.assertIsNotNone(limits)
        small, large = (int(limits[1]), int(limits[2])), (int(limits[3]), int(limits[4]))
        self.assertEqual((small, large), ((1920, 1080), (3840, 2160)))

        def recording_size(variant: str, camera: int):
            profiles, _ = self._profiles(variant)
            return max(profiles[camera].values(), key=lambda size: size[0] * size[1])

        self.assertEqual(recording_size("_camsmall", 0), small)
        self.assertEqual(recording_size("_camsmall", 1), small)
        self.assertEqual(recording_size("_camlarge", 0), large)
        self.assertEqual(recording_size("_camlarge2160", 0), large)
        self.assertEqual(recording_size("_camlarge2160", 1), large)
        self.assertEqual(recording_size("_camfront", 0), small)
        # The large layout with a 1080p front camera keeps that camera at the small tier's size, which the HAL's
        # cameras of that kind offer.
        self.assertEqual(recording_size("_camlarge", 1), small)

    def test_a_front_camera_alone_is_camera_zero(self):
        profiles, caps = self._profiles("_camfront")
        self.assertEqual(sorted(profiles), [0])
        self.assertNotIn("2160p", profiles[0])
        self.assertEqual(caps[:2], (1920, 1080))

    def test_every_camera_has_the_lowest_and_a_720p_profile(self):
        for variant in generator.media_variants():
            profiles, _ = self._profiles(variant)
            for camera, qualities in profiles.items():
                self.assertIn("qcif", qualities, (variant, camera))
                self.assertIn("720p", qualities, (variant, camera))


class Files(unittest.TestCase):
    def test_every_generated_file_is_checked_in_exactly_as_rendered(self):
        files = generator.render()
        present = {str(path.relative_to(GENERATED)) for path in GENERATED.rglob("*") if path.is_file()}
        self.assertEqual(present, set(files))
        for name, data in files.items():
            self.assertEqual((GENERATED / name).read_bytes(), data, name)

    def test_every_file_parses_and_carries_the_licence_line(self):
        for name, data in generator.render().items():
            text = data.decode()
            self.assertIn("SPDX-License-Identifier: Apache-2.0", text.splitlines()[0] + text.splitlines()[1], name)
            if name.endswith(".xml"):
                ElementTree.fromstring(data)
        for layout in generator.all_layouts():
            if layout.features:
                root = ElementTree.fromstring(generator.permissions_file(layout))
                self.assertEqual([element.get("name") for element in root.findall("feature")],
                                 list(layout.features))

    def test_the_makefile_installs_each_file_once_to_the_place_the_framework_reads(self):
        text = generator.makefile().decode()
        sources = re.findall(r"^\s+(\S+?):(\S+?)(?: \\)?$", text, re.MULTILINE)
        destinations = [destination for _, destination in sources]
        self.assertEqual(len(destinations), len(set(destinations)))
        files = generator.render()
        installed = {source.removeprefix(generator.DEVICE_DIRECTORY + "/") for source, _ in sources}
        self.assertEqual(installed, set(files) - {generator.TABLE, generator.MAKEFILE})
        for layout in generator.all_layouts():
            if layout.features:
                self.assertIn(f"$(TARGET_COPY_OUT_VENDOR)/etc/permissions/sku_{layout.name}/kernarium_camera.xml",
                              destinations)
        for variant in generator.media_variants():
            self.assertIn(f"$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles{variant}.xml", destinations)
        self.assertIn(f"$(TARGET_COPY_OUT_VENDOR)/etc/init/{generator.INIT_FILE}", destinations)
        self.assertIn(f"$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/{generator.PROVIDER_INIT_FILE}", destinations)
        # No camera file lands outside the vendor partition but the provider's init file, which init reads
        # without the vendor_init context, and no vintf manifest is named after a layout.
        for destination in destinations:
            if destination != f"$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/{generator.PROVIDER_INIT_FILE}":
                self.assertTrue(destination.startswith("$(TARGET_COPY_OUT_VENDOR)/etc/"), destination)
            self.assertNotIn("vintf", destination)

    def test_the_installed_paths_name_the_partitions_of_the_archive(self):
        installed = generator.installed()
        self.assertIn(f"VENDOR/etc/init/{generator.INIT_FILE}", installed)
        self.assertIn(f"SYSTEM_EXT/etc/init/{generator.PROVIDER_INIT_FILE}", installed)
        self.assertFalse([path for path in installed if "$(" in path])

    def test_the_init_file_selects_the_variant_of_each_layout_that_has_one(self):
        text = generator.init_file().decode()
        triggers = re.findall(r"^on property:(\S+)=(\S+)\n    setprop (\S+) (\S+)$", text, re.MULTILINE)
        expected = {layout.name: layout.media_variant for layout in generator.all_layouts() if layout.media_variant}
        self.assertEqual({name: variant for _, name, _, variant in triggers}, expected)
        for prop, _, target, _ in triggers:
            self.assertEqual(prop, generator.SKU_PROPERTY)
            self.assertEqual(target, generator.MEDIA_PROPERTY)
        self.assertEqual(len(triggers), len(expected))

    def test_the_provider_init_file_starts_the_provider_with_cameraserver_in_the_layouts_that_have_cameras_and_no_other(self):
        # cameraserver lists only a provider that has registered, and nothing but init starts the lazy AIDL
        # provider: the first time, and when cameraserver starts again after the provider has exited. So the
        # action is on cameraserver's state (and the layout's SKU), never on the SKU alone.
        text = generator.provider_init_file().decode()
        actions = {}
        for action in re.split(r"^(?=on )", text, flags=re.MULTILINE):
            match = re.match(r"on (.+)\n((?:    .+\n)*)", action)
            if match is not None:
                actions[match[1]] = [line.strip() for line in match[2].splitlines()]
        wanted = {generator.provider_trigger(layout.name): [f"restart {generator.PROVIDER_SERVICE}"]
                  for layout in generator.all_layouts() if layout.has_front}
        self.assertEqual(actions, wanted)
        self.assertNotIn(generator.provider_trigger("none"), actions)
        self.assertEqual(text.count(f"restart {generator.PROVIDER_SERVICE}"), len(wanted))
        self.assertEqual(generator.PROVIDER_COMMAND, "restart")
        # the vendor file's profile actions select the media profiles and start nothing: init reads that file in the
        # vendor_init context, which may not read init.svc.cameraserver
        profile = generator.init_file().decode()
        self.assertNotIn("start ", profile)
        self.assertNotIn(generator.SERVER_PROPERTY, profile)

    def test_the_vendor_file_triggers_on_the_sku_alone_and_the_provider_file_on_cameraserver_s_status(self):
        # init reads a vendor script in the vendor_init context, which may read ro.boot.* but not
        # init.svc.cameraserver (the verifier checks both against the image's policy).
        triggers = re.findall(r"^on property:(\S+?)=", generator.init_file().decode(), re.MULTILINE)
        self.assertTrue(triggers)
        self.assertEqual({trigger for trigger in triggers}, {generator.SKU_PROPERTY})
        self.assertTrue(generator.SKU_PROPERTY.startswith("ro.boot."))
        provider = re.findall(r"^on property:(\S+?)=running", generator.provider_init_file().decode(), re.MULTILINE)
        self.assertEqual(set(provider), {generator.SERVER_PROPERTY})

    def test_the_provider_trigger_is_cameraserver_running_and_the_layout(self):
        self.assertEqual(generator.provider_trigger("phone"),
                         "property:init.svc.cameraserver=running && property:ro.boot.product.vendor.sku=phone")

    def test_the_provider_service_is_the_one_of_the_stock_lazy_rc(self):
        self.assertEqual(generator.PROVIDER_SERVICE, "vendor.camera-provider-2-7-google")

    def test_the_table_lists_every_layout_with_its_fallback(self):
        rows = [line.split("\t") for line in generator.table().decode().splitlines() if not line.startswith("#")]
        self.assertEqual([row[0] for row in rows], [layout.name for layout in generator.all_layouts()])
        for row in rows:
            self.assertEqual(len(row), 10)
            self.assertEqual([] if row[9] == "-" else row[9].split(","), generator.fallback_chain(row[0])[1:])


if __name__ == "__main__":
    unittest.main()
