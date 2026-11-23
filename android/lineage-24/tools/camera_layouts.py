#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera layouts of the Android image, and the files each one selects.

One image serves every host. The host discovers its cameras and passes the layout
that describes them on the vendor SKU axis (androidboot.product.vendor.sku=<name>).
A layout is a closed name: the permission directory sku_<name>/ of the vendor
partition declares the camera features it earns, and an init trigger on the name
selects the media profiles file that matches its size caps, and a trigger in a
system_ext init file restarts the camera provider whenever cameraserver starts (a
layout without cameras restarts none). This module is the only source of the set,
its names, its fallback order and the files:

    camera_layouts.py            rewrite overlay/camera/generated
    camera_layouts.py --check    fail when a checked-in file differs

The verifier of the built target files and the tests import it: render() returns
the whole generated tree as {relative path: bytes}, and fallback_chain() is the
reference for the runtime that composes a layout.

The dimensions: the host's cameras (none, the front camera alone, or back and
front), and for back and front the memory tier (small: video to 1080p; large: video
to 4K), the back flash, manual sensor control, RAW (only with manual control), the
concurrent streaming of the front and the back camera, and for the large tier the
front camera's video maximum (1080p or 2160p). A front camera alone has manual
control or not. That is 3 + 2 x 2 x 3 x 2 + 12 = 39 layouts.

`concurrent` means one thing, here, in the HAL (camera_ids.hpp, concurrent_feature)
and in the application: the cameras "0" and "1" are in one set that the host
streams together, and both meet the camera API's guaranteed concurrent stream
sizes as the HAL derives them (camera_plan.hpp, meets_concurrent_streams). It is
Android's own condition for the feature android.hardware.camera.concurrent.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import functools
from dataclasses import dataclass
from pathlib import Path
import sys

ANDROID_ROOT = Path(__file__).resolve().parents[1]
GENERATED = ANDROID_ROOT / "overlay/camera/generated"
# Where the product installs the files; the path of the overlay in the device tree
# is device/kernarium/andarium (sync-device-tree.py copies overlay/ there).
DEVICE_DIRECTORY = "device/kernarium/andarium/camera/generated"
PERMISSIONS_DIRECTORY = "$(TARGET_COPY_OUT_VENDOR)/etc/permissions"
MEDIA_DIRECTORY = "$(TARGET_COPY_OUT_VENDOR)/etc"
INIT_DIRECTORY = "$(TARGET_COPY_OUT_VENDOR)/etc/init"
# The provider's start actions are in the system_ext partition: init reads a vendor init file in the vendor_init
# context, which may not read init.svc.cameraserver, and drops an action whose property trigger it cannot read.
PROVIDER_INIT_DIRECTORY = "$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init"
SKU_PROPERTY = "ro.boot.product.vendor.sku"
# The kernel argument that Android init imports as that property.
SKU_BOOT_ARGUMENT = "androidboot.product.vendor.sku="
MEDIA_PROPERTY = "ro.media.xml_variant.profiles"
# The service of the lazy camera provider (the stock rc of hardware/google/camera, which the verifier
# reads) and the state of cameraserver that a layout with cameras starts it on.
PROVIDER_SERVICE = "vendor.camera-provider-2-7-google"
PROVIDER_COMMAND = "restart"
SERVER_PROPERTY = "init.svc.cameraserver"
INIT_FILE = "init.kernarium_camera.rc"
PROVIDER_INIT_FILE = "kernarium_camera_provider.rc"
MAKEFILE = "camera_layouts.mk"
TABLE = "layouts.tsv"
GENERATOR = "android/lineage-24/tools/camera_layouts.py"

TIERS = ("small", "large")
# What a media profile variant covers: the largest video size of the back cameras
# and of the front camera.
BACK_VIDEO = {"small": 1080, "large": 2160}


@dataclass(frozen=True)
class Layout:
    name: str
    cameras: str  # "none", "front" or "back+front"
    tier: str  # "" for a layout without a back camera
    flash: bool = False
    manual: bool = False
    raw: bool = False
    concurrent: bool = False
    front2160: bool = False

    @property
    def has_back(self) -> bool:
        return self.cameras == "back+front"

    @property
    def has_front(self) -> bool:
        return self.cameras != "none"

    @property
    def features(self) -> tuple[str, ...]:
        """The camera features of the vendor permission directory, in a fixed order."""
        if not self.has_front:
            return ()
        features = ["android.hardware.camera.any"]
        if self.has_back:
            features.append("android.hardware.camera")
        features.append("android.hardware.camera.front")
        if self.has_back:
            features.append("android.hardware.camera.autofocus")
        if self.flash:
            features.append("android.hardware.camera.flash")
        if self.manual:
            features.append("android.hardware.camera.capability.manual_sensor")
        if self.raw:
            features.append("android.hardware.camera.capability.raw")
        if self.concurrent:
            features.append("android.hardware.camera.concurrent")
        return tuple(features)

    @property
    def media_variant(self) -> str:
        """The value of ro.media.xml_variant.profiles, "" for the product's default file."""
        if not self.has_front:
            return ""
        if not self.has_back:
            return "_camfront"
        if self.tier == "small":
            return "_camsmall"
        return "_camlarge2160" if self.front2160 else "_camlarge"


def _name(tier: str, flash: bool, manual: bool, raw: bool, concurrent: bool, front2160: bool) -> str:
    parts = ["bf", tier]
    if flash:
        parts.append("flash")
    if manual:
        parts.append("manual")
    if raw:
        parts.append("raw")
    if concurrent:
        parts.append("concurrent")
    if front2160:
        parts.append("front2160")
    return "-".join(parts)


@functools.cache
def _layouts() -> tuple[Layout, ...]:
    layouts = [
        Layout("none", "none", ""),
        Layout("front", "front", ""),
        Layout("front-manual", "front", "", manual=True),
    ]
    for tier in TIERS:
        for front2160 in ((False, True) if tier == "large" else (False,)):
            for flash in (False, True):
                for manual, raw in ((False, False), (True, False), (True, True)):
                    for concurrent in (False, True):
                        layouts.append(Layout(_name(tier, flash, manual, raw, concurrent, front2160),
                                              "back+front", tier, flash, manual, raw, concurrent, front2160))
    return tuple(layouts)


@functools.cache
def _layouts_by_name() -> dict[str, Layout]:
    return {layout.name: layout for layout in _layouts()}


def all_layouts() -> list[Layout]:
    """Every layout, in the order of the table: none, the front camera alone, back and front.
    Layouts are immutable and built once; each call returns a list of its own."""
    return list(_layouts())


def by_name() -> dict[str, Layout]:
    return dict(_layouts_by_name())


def _drops(layout: Layout) -> list[Layout]:
    """The layouts that dropping one dimension after another leads to, nearest first.

    The order is the runtime's: concurrent streaming, then RAW, then the front
    camera's 2160p, then the flash, then the large tier, then manual control, then
    the back cameras altogether ("front"), then no camera."""
    steps: list[Layout] = []
    current = layout
    if layout.name == "none":
        return steps
    if current.has_back:
        for field, value in (("concurrent", False), ("raw", False), ("front2160", False), ("flash", False)):
            if getattr(current, field):
                current = _rename(current, **{field: value})
                steps.append(current)
        if current.tier == "large":
            current = _rename(current, tier="small")
            steps.append(current)
        if current.manual:
            current = _rename(current, manual=False, raw=False)
            steps.append(current)
        steps.append(by_name()["front"])
    elif layout.name == "front-manual":
        steps.append(by_name()["front"])
    steps.append(by_name()["none"])
    return steps


def _rename(layout: Layout, **changes) -> Layout:
    fields = dict(tier=layout.tier, flash=layout.flash, manual=layout.manual, raw=layout.raw,
                  concurrent=layout.concurrent, front2160=layout.front2160)
    fields.update(changes)
    if fields["tier"] != "large":
        fields["front2160"] = False
    if not fields["manual"]:
        fields["raw"] = False
    return by_name()[_name(fields["tier"], fields["flash"], fields["manual"], fields["raw"],
                           fields["concurrent"], fields["front2160"])]


def fallback_chain(name: str) -> list[str]:
    """The layout and then, in order, each layout it falls back to when the image lacks it."""
    layout = by_name()[name]
    return [layout.name] + [step.name for step in _drops(layout)]


def resolve(wanted: Layout, available: set[str]) -> str:
    """The layout a host gets from an image that has `available`: the first of the chain."""
    for name in fallback_chain(wanted.name):
        if name in available:
            return name
    return "none"


# ----------------------------------------------------------------------------------------------
# The files

PERMISSIONS_HEADER = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    "<!-- SPDX-License-Identifier: Apache-2.0 -->\n"
    f"<!-- Generated by {GENERATOR}; do not edit. -->\n")


def permissions_file(layout: Layout) -> bytes:
    lines = [PERMISSIONS_HEADER,
             f"<!-- Camera features of the layout {layout.name}: what the host's cameras earn. -->\n",
             "<permissions>\n"]
    for feature in layout.features:
        lines.append(f'    <feature name="{feature}" />\n')
    lines.append("</permissions>\n")
    return "".join(lines).encode()


# The profile of each quality the host's sizes offer (sizes the camera device scales to).
QUALITIES = (
    # quality, width, height, h264 bit rate, audio codec, audio bit rate, audio sample rate
    ("qcif", 176, 144, 192000, "amrnb", 12200, 8000),
    ("qvga", 320, 240, 500000, "amrnb", 12200, 8000),
    ("vga", 640, 480, 2000000, "aac", 96000, 48000),
    ("720p", 1280, 720, 12000000, "aac", 96000, 48000),
    ("1080p", 1920, 1080, 17000000, "aac", 96000, 48000),
    ("2160p", 3840, 2160, 42000000, "aac", 96000, 48000),
)
FRAME_RATE = 30
# The product's own caps for the encoders that stay as they are (h263 and m4v), and the
# h264 cap per variant: the largest video size of its cameras.
MEDIA_HEADER = """<?xml version="1.0" encoding="utf-8"?>
<!-- SPDX-License-Identifier: Apache-2.0 -->
<!--
     Generated by {generator}; do not edit.
     The camera layout's media profiles. Derived from the AOSP media profiles of
     the Cuttlefish product, Copyright 2017 The Android Open Source Project,
     Licensed under the Apache License, Version 2.0.
-->
<!DOCTYPE MediaSettings [
<!ELEMENT MediaSettings (CamcorderProfiles+,
                         EncoderOutputFileFormat+,
                         VideoEncoderCap+,
                         AudioEncoderCap+,
                         VideoDecoderCap,
                         AudioDecoderCap)>
<!ELEMENT CamcorderProfiles (EncoderProfile+, ImageEncoding+, ImageDecoding)>
<!ATTLIST CamcorderProfiles cameraId CDATA #REQUIRED>
<!ELEMENT EncoderProfile (Video, Audio)>
<!ATTLIST EncoderProfile quality CDATA #REQUIRED>
<!ATTLIST EncoderProfile fileFormat (mp4|3gp) #REQUIRED>
<!ATTLIST EncoderProfile duration (30|60) #REQUIRED>
<!ELEMENT Video EMPTY>
<!ATTLIST Video codec (h264|h263|m4v) #REQUIRED>
<!ATTLIST Video bitRate CDATA #REQUIRED>
<!ATTLIST Video width CDATA #REQUIRED>
<!ATTLIST Video height CDATA #REQUIRED>
<!ATTLIST Video frameRate CDATA #REQUIRED>
<!ELEMENT Audio EMPTY>
<!ATTLIST Audio codec (amrnb|amrwb|aac) #REQUIRED>
<!ATTLIST Audio bitRate CDATA #REQUIRED>
<!ATTLIST Audio sampleRate CDATA #REQUIRED>
<!ATTLIST Audio channels (1|2) #REQUIRED>
<!ELEMENT ImageEncoding EMPTY>
<!ATTLIST ImageEncoding quality (95|90|80|70|60|50|40) #REQUIRED>
<!ELEMENT ImageDecoding EMPTY>
<!ATTLIST ImageDecoding memCap CDATA #REQUIRED>
<!ELEMENT EncoderOutputFileFormat EMPTY>
<!ATTLIST EncoderOutputFileFormat name (mp4|3gp) #REQUIRED>
<!ELEMENT VideoEncoderCap EMPTY>
<!ATTLIST VideoEncoderCap name (h264|h263|m4v|wmv) #REQUIRED>
<!ATTLIST VideoEncoderCap enabled (true|false) #REQUIRED>
<!ATTLIST VideoEncoderCap minBitRate CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap maxBitRate CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap minFrameWidth CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap maxFrameWidth CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap minFrameHeight CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap maxFrameHeight CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap minFrameRate CDATA #REQUIRED>
<!ATTLIST VideoEncoderCap maxFrameRate CDATA #REQUIRED>
<!ELEMENT AudioEncoderCap EMPTY>
<!ATTLIST AudioEncoderCap name (amrnb|amrwb|aac|wma) #REQUIRED>
<!ATTLIST AudioEncoderCap enabled (true|false) #REQUIRED>
<!ATTLIST AudioEncoderCap minBitRate CDATA #REQUIRED>
<!ATTLIST AudioEncoderCap maxBitRate CDATA #REQUIRED>
<!ATTLIST AudioEncoderCap minSampleRate CDATA #REQUIRED>
<!ATTLIST AudioEncoderCap maxSampleRate CDATA #REQUIRED>
<!ATTLIST AudioEncoderCap minChannels (1|2) #REQUIRED>
<!ATTLIST AudioEncoderCap maxChannels (1|2) #REQUIRED>
<!ELEMENT VideoDecoderCap EMPTY>
<!ATTLIST VideoDecoderCap name (wmv) #REQUIRED>
<!ATTLIST VideoDecoderCap enabled (true|false) #REQUIRED>
<!ELEMENT AudioDecoderCap EMPTY>
<!ATTLIST AudioDecoderCap name (wma) #REQUIRED>
<!ATTLIST AudioDecoderCap enabled (true|false) #REQUIRED>
]>
<MediaSettings>
""".format(generator=GENERATOR)

MEDIA_FOOTER = """    <EncoderOutputFileFormat name="3gp" />
    <EncoderOutputFileFormat name="mp4" />

    <!-- A codec that is not enabled is invisible to applications. The h264 cap spans the
         largest video size of the layout's cameras; the other two stay at the product's. -->
    <VideoEncoderCap name="h264" enabled="true"
        minBitRate="64000" maxBitRate="{h264_bit_rate}"
        minFrameWidth="176" maxFrameWidth="{width}"
        minFrameHeight="144" maxFrameHeight="{height}"
        minFrameRate="15" maxFrameRate="60" />

    <VideoEncoderCap name="h263" enabled="true"
        minBitRate="64000" maxBitRate="12000000"
        minFrameWidth="176" maxFrameWidth="1280"
        minFrameHeight="144" maxFrameHeight="720"
        minFrameRate="15" maxFrameRate="30" />

    <VideoEncoderCap name="m4v" enabled="true"
        minBitRate="64000" maxBitRate="12000000"
        minFrameWidth="176" maxFrameWidth="1280"
        minFrameHeight="144" maxFrameHeight="720"
        minFrameRate="15" maxFrameRate="30" />

    <AudioEncoderCap name="aac" enabled="true"
        minBitRate="8000" maxBitRate="96000"
        minSampleRate="8000" maxSampleRate="48000"
        minChannels="1" maxChannels="1" />

    <AudioEncoderCap name="amrwb" enabled="true"
        minBitRate="6600" maxBitRate="23050"
        minSampleRate="16000" maxSampleRate="16000"
        minChannels="1" maxChannels="1" />

    <AudioEncoderCap name="amrnb" enabled="true"
        minBitRate="5525" maxBitRate="12200"
        minSampleRate="8000" maxSampleRate="8000"
        minChannels="1" maxChannels="1" />

    <VideoDecoderCap name="wmv" enabled="false"/>
    <AudioDecoderCap name="wma" enabled="false"/>
</MediaSettings>
"""


def _profiles(camera_id: int, largest_height: int) -> str:
    lines = [f'    <CamcorderProfiles cameraId="{camera_id}">\n']
    for quality, width, height, video_rate, audio, audio_rate, sample_rate in QUALITIES:
        if height > largest_height:
            continue
        lines.append(
            f'        <EncoderProfile quality="{quality}" fileFormat="mp4" duration="60">\n'
            f'            <Video codec="h264" bitRate="{video_rate}" width="{width}" height="{height}"'
            f' frameRate="{FRAME_RATE}" />\n'
            f'            <Audio codec="{audio}" bitRate="{audio_rate}" sampleRate="{sample_rate}" channels="1" />\n'
            "        </EncoderProfile>\n")
    lines.append('        <ImageEncoding quality="95" />\n'
                 '        <ImageEncoding quality="80" />\n'
                 '        <ImageEncoding quality="70" />\n'
                 '        <ImageDecoding memCap="20000000" />\n'
                 "    </CamcorderProfiles>\n\n")
    return "".join(lines)


def media_variants() -> dict[str, tuple[int, int, tuple[int, ...]]]:
    """variant -> (back video height, front video height, the camera IDs it describes)."""
    return {
        "_camfront": (0, 1080, (0,)),
        "_camsmall": (1080, 1080, (0, 1, 2, 3, 4)),
        "_camlarge": (2160, 1080, (0, 1, 2, 3, 4)),
        "_camlarge2160": (2160, 2160, (0, 1, 2, 3, 4)),
    }


def media_profiles_file(variant: str) -> bytes:
    back, front, ids = media_variants()[variant]
    body = [MEDIA_HEADER]
    for camera_id in ids:
        # Camera 1 is the front camera of a layout with a back camera; camera 0 is the front
        # camera of a layout without one.
        is_front = camera_id == 1 or (camera_id == 0 and back == 0)
        body.append(_profiles(camera_id, front if is_front else back))
    largest = max(back, front)
    width = {1080: 1920, 2160: 3840}[largest]
    body.append(MEDIA_FOOTER.format(h264_bit_rate=60000000 if largest == 2160 else 40000000,
                                    width=width, height=largest))
    return "".join(body).encode()


def provider_trigger(name: str) -> str:
    """The trigger of the action that restarts the camera provider for a layout: cameraserver running and the
    layout's name on the vendor SKU. init runs the action each time cameraserver reaches running, which is
    its first start and every restart."""
    return f"property:{SERVER_PROPERTY}=running && property:{SKU_PROPERTY}={name}"


def init_file() -> bytes:
    lines = ["# SPDX-License-Identifier: Apache-2.0\n",
             f"# Generated by {GENERATOR}; do not edit.\n",
             "#\n",
             "# The camera layout the host composed selects the media profiles variant that matches its\n",
             "# size caps. A layout with no variant (none) keeps the product's default profiles.\n"]
    for layout in all_layouts():
        if layout.media_variant:
            lines.append(f"\non property:{SKU_PROPERTY}={layout.name}\n"
                         f"    setprop {MEDIA_PROPERTY} {layout.media_variant}\n")
    return "".join(lines).encode()


def provider_init_file() -> bytes:
    lines = ["# SPDX-License-Identifier: Apache-2.0\n",
             f"# Generated by {GENERATOR}; do not edit.\n",
             "#\n",
             "# A layout with cameras restarts the camera provider whenever cameraserver starts. The\n",
             "# provider is lazy: it exits when cameraserver holds no camera, and init starts it\n",
             "# again through its interface when an application opens one. cameraserver lists only a\n",
             "# provider that has registered, at its start or later, and nothing else starts a lazy\n",
             "# AIDL provider the first time, or after it has exited when cameraserver starts again\n",
             "# (a crash, a restart of the framework). restart, not start: a provider that is still\n",
             "# running but has unregistered, which a lazy provider does a moment before it exits, is\n",
             "# not started again by start, and restart starts one that is not running at once. A\n",
             "# layout without cameras starts none.\n"]
    for layout in all_layouts():
        if layout.has_front:
            lines.append(f"\non {provider_trigger(layout.name)}\n"
                         f"    {PROVIDER_COMMAND} {PROVIDER_SERVICE}\n")
    return "".join(lines).encode()


def copies() -> list[tuple[str, str]]:
    """The generated files the product installs: (path below overlay/camera/generated, install path
    with the vendor partition spelled as the makefile does)."""
    result = []
    for layout in all_layouts():
        if layout.features:
            relative = f"sku_{layout.name}/kernarium_camera.xml"
            result.append((relative, f"{PERMISSIONS_DIRECTORY}/{relative}"))
    for variant in media_variants():
        relative = f"media_profiles{variant}.xml"
        result.append((relative, f"{MEDIA_DIRECTORY}/{relative}"))
    result.append((INIT_FILE, f"{INIT_DIRECTORY}/{INIT_FILE}"))
    result.append((PROVIDER_INIT_FILE, f"{PROVIDER_INIT_DIRECTORY}/{PROVIDER_INIT_FILE}"))
    return result


def installed() -> dict[str, bytes]:
    """What a built image holds of the layouts, by its path in the target-files archive."""
    files = render()
    return {destination.replace("$(TARGET_COPY_OUT_VENDOR)", "VENDOR")
            .replace("$(TARGET_COPY_OUT_SYSTEM_EXT)", "SYSTEM_EXT"): files[relative]
            for relative, destination in copies()}


def makefile() -> bytes:
    lines = ["# SPDX-License-Identifier: Apache-2.0\n",
             f"# Generated by {GENERATOR}; do not edit.\n",
             "#\n",
             "# The files of the camera layouts: a permission directory for each layout that has cameras,\n",
             "# the media profiles files, the init file that selects one by the layout, and the init file\n",
             "# that restarts the camera provider.\n",
             "PRODUCT_COPY_FILES += \\\n"]
    lines.append(" \\\n".join(f"    {DEVICE_DIRECTORY}/{relative}:{destination}"
                              for relative, destination in copies()) + "\n")
    return "".join(lines).encode()


def table() -> bytes:
    lines = ["# SPDX-License-Identifier: Apache-2.0\n",
             f"# Generated by {GENERATOR}; do not edit.\n",
             "#\n",
             "# The camera layouts: the names the host passes as androidboot.product.vendor.sku, the\n",
             "# dimensions each one has, the media profiles variant its init trigger selects, and the\n",
             "# layouts it falls back to, nearest first, when an image lacks it.\n",
             "# name\tcameras\ttier\tflash\tmanual\traw\tconcurrent\tfront2160\tmedia_variant\tfallback\n"]
    for layout in all_layouts():
        fallback = ",".join(fallback_chain(layout.name)[1:]) or "-"
        lines.append("\t".join([layout.name, layout.cameras, layout.tier or "-",
                                *("1" if value else "0" for value in (layout.flash, layout.manual, layout.raw,
                                                                       layout.concurrent, layout.front2160)),
                                layout.media_variant or "-", fallback]) + "\n")
    return "".join(lines).encode()


def render() -> dict[str, bytes]:
    """The whole generated tree: path relative to overlay/camera/generated -> bytes."""
    files = {TABLE: table(), MAKEFILE: makefile(), INIT_FILE: init_file(), PROVIDER_INIT_FILE: provider_init_file()}
    for layout in all_layouts():
        if layout.features:
            files[f"sku_{layout.name}/kernarium_camera.xml"] = permissions_file(layout)
    for variant in media_variants():
        files[f"media_profiles{variant}.xml"] = media_profiles_file(variant)
    return files


def existing(directory: Path) -> set[str]:
    return {str(path.relative_to(directory)) for path in directory.rglob("*") if path.is_file()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="fail when a checked-in file differs")
    parser.add_argument("--output", type=Path, default=GENERATED)
    arguments = parser.parse_args()
    files = render()
    if arguments.check:
        problems = []
        present = existing(arguments.output) if arguments.output.is_dir() else set()
        for name in sorted(files):
            path = arguments.output / name
            if not path.is_file():
                problems.append(f"missing: {name}")
            elif path.read_bytes() != files[name]:
                problems.append(f"differs from the generator: {name}")
        for name in sorted(present - set(files)):
            problems.append(f"not generated: {name}")
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1
        print(f"verified {len(files)} generated camera layout files ({len(all_layouts())} layouts)")
        return 0
    for name in sorted(existing(arguments.output) - set(files)) if arguments.output.is_dir() else []:
        (arguments.output / name).unlink()
    for name, data in files.items():
        path = arguments.output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    for directory in sorted((path for path in arguments.output.rglob("*") if path.is_dir()), reverse=True):
        if not any(directory.iterdir()):
            directory.rmdir()
    print(f"wrote {len(files)} files ({len(all_layouts())} layouts) to {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
