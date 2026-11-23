# SPDX-License-Identifier: GPL-2.0-or-later
"""A front camera states an orientation of 270, and an application that reads it through Camera API 1 gets the
picture the host has turned for it.

Android's face-scan applications read a camera through android.hardware.Camera, turn the preview frame by
``CameraInfo.orientation`` and assume 270 of a front camera. Every built-in front camera states 270, and the host
turns its pictures half around where the sensor is mounted at 90 (darwin/device/camera/README.md, Orientation).
The runner boots the release with the synthetic phone whose front camera is presented that way
(``--camera-profile=phone-turned-front``) and installs a probe application (camera_api1_probe_apk.py), which opens
each camera through API 1 and saves what the framework says (orientation, facing, preview size) and one preview
frame as the application receives it. It checks, for the front camera (id 1) and the back camera (id 0):

- the orientation and facing the framework states: the front camera 270 and front facing, the back camera 90 and
  back facing, and the same orientation in the camera service's own dump;
- that the frame is the synthetic chart turned half around for the front camera and as drawn for the back camera
  (camera_chart_orientation.py): an application that turns it by the stated orientation shows the front camera's
  chart as the camera drew it, which is what upright means for a chart without a world.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest is
reached through the shared adb server, always named with ``-s``. The runner is opt-in: it needs the macos-android17
executable, an installed release and an adb client.

usage: run_android_camera_front.py --emulator uml-darwin-android17 [--release DIR]
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

import camera_api1_probe_apk as probe
import camera_chart_orientation as chart
import run_android_camera as rac

PROFILE = "phone-turned-front"
FRONT, BACK = 1, 0
CAMERA_FACING_BACK, CAMERA_FACING_FRONT = 0, 1
# What the guest states of each camera: SENSOR_ORIENTATION and the facing API 1 reports.
EXPECTED = {FRONT: (270, CAMERA_FACING_FRONT), BACK: (90, CAMERA_FACING_BACK)}
# The tag of each camera's chart: 0x5a00 and the camera's index in the platform's list (chartTag in the synthetic camera). In
# the phone profile the back camera the guest calls 0 is the platform's 0 and the front camera it calls 1 is the platform's 3.
CHART_TAG = {FRONT: 0x5A03, BACK: 0x5A00}
# android.graphics.ImageFormat.NV21, the preview format the probe asks for and the check reads the frame as.
NV21 = 17
# The turn the host gives the chart's frame before the guest reads it, in degrees clockwise: the frame that shows
# the chart as the camera drew it needs this turn, and an application turns it by the stated orientation
# (the front camera's, 270, plus the half turn the host already made).
FRAME_TURN = {FRONT: 180, BACK: 0}
INFO_PATTERN = re.compile(r"^orientation=(\d+) facing=(\d+) preview=(\d+)x(\d+) format=(\d+) camera=(\d+)$")
FRAME_SECONDS = 60
SETTLE_SECONDS = 4
COMMAND_SECONDS = 120


class Failure(Exception):
    """A check of the run that did not hold."""


class Info:
    """What the probe wrote of a camera: ``orientation=270 facing=1 preview=640x480 format=17 camera=1``."""

    def __init__(self, orientation: int, facing: int, width: int, height: int, preview_format: int, camera: int) -> None:
        self.orientation, self.facing, self.width, self.height = orientation, facing, width, height
        self.preview_format, self.camera = preview_format, camera


def parse_info(text: str) -> Info:
    match = INFO_PATTERN.match(text.strip())
    if match is None:
        raise Failure(f"the probe's statement {text!r} is not of the form it writes")
    return Info(*(int(item) for item in match.groups()))


def dump_orientations(dump: str) -> dict[str, int]:
    """The orientation each camera's static information states in `dumpsys media.camera`, by camera id: the
    ``Orientation:`` line of the section that starts ``== Camera HAL device device@1.1/internal/<id> ...
    static information: ==``."""
    states: dict[str, int] = {}
    current = None
    for line in dump.splitlines():
        header = re.match(r"== Camera HAL device \S+/(\d+) .*static information", line)
        if header:
            current = header.group(1)
            continue
        if line.startswith("== "):
            current = None
        stated = re.match(r"\s+Orientation:\s*(\d+)\s*$", line)
        if stated and current is not None:
            states.setdefault(current, int(stated.group(1)))
    return states


def check_camera(camera: int, info: Info, frame: bytes) -> list[str]:
    """What is wrong with the probe's reading of a camera, one sentence for each; none when it holds."""
    problems = []
    orientation, facing = EXPECTED[camera]
    if info.camera != camera:
        problems.append(f"the probe opened camera {info.camera}, not {camera}")
    if info.orientation != orientation:
        problems.append(f"camera {camera} states an orientation of {info.orientation}, not {orientation}")
    if info.facing != facing:
        problems.append(f"camera {camera} states facing {info.facing}, not {facing}")
    if info.preview_format != NV21:
        # The frame is read as NV21 only if that is what the framework says it is.
        return problems + [f"camera {camera}: the preview format is {info.preview_format}, not NV21 ({NV21})"]
    try:
        luma = chart.luma_of_nv21(frame, info.width, info.height)
    except ValueError as problem:
        return problems + [f"camera {camera}: {problem}"]
    views = chart.upright_views(luma, info.width, info.height, CHART_TAG[camera])
    if views != [(FRAME_TURN[camera], False)]:
        others = chart.upright_views(luma, info.width, info.height)
        other = f" (it is another camera's chart: upright after {others})" if not views and others else ""
        problems.append(f"camera {camera}: the chart of tag {CHART_TAG[camera]:#06x} is upright after the turns {views} of the frame, "
                        f"not after {[(FRAME_TURN[camera], False)]}{other}")
    return problems


def read_camera(guest: rac.Guest, adb: str, camera: int, output: pathlib.Path) -> tuple[Info, bytes]:
    package = probe.PACKAGE
    guest.adb_run("shell", f"am force-stop {package}")
    guest.adb_run("shell", f"run-as {package} rm -f files/frame.nv21 files/frame.tmp files/info.txt")
    started = guest.adb_run("shell", f"am start -W -n {probe.COMPONENT} --ei camera {camera}", timeout=COMMAND_SECONDS)
    print(f"camera {camera} start: {started.stdout.strip()!r}")
    deadline = time.monotonic() + FRAME_SECONDS
    while time.monotonic() < deadline:
        if "frame.nv21" in guest.adb_run("shell", f"run-as {package} ls files").stdout:
            break
        time.sleep(1)
    else:
        raise Failure(f"camera {camera}: no preview frame reached the probe within {FRAME_SECONDS} s")
    time.sleep(SETTLE_SECONDS)  # later frames: the exposure has settled
    info = parse_info(guest.adb_run("shell", f"run-as {package} cat files/info.txt").stdout)
    frame = subprocess.run([adb, "-s", guest.serial, "exec-out", "run-as", package, "cat", "files/frame.nv21"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=COMMAND_SECONDS, check=False).stdout
    (output / f"frame-{camera}.nv21").write_bytes(frame)
    guest.adb_run("shell", f"am force-stop {package}")
    return info, frame


def build_parser() -> argparse.ArgumentParser:
    parser = rac.build_parser()
    parser.description = __doc__
    parser.set_defaults(camera_profile=PROFILE, mode="live")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.camera_profile != PROFILE or options.mode != "live":
        print(f"the runner is for the {PROFILE} profile in the live mode", file=sys.stderr)
        return 2
    options.hardware_input = False
    plan = rac.make_plan(rac.DEFAULT_LAYOUTS[PROFILE], PROFILE, "live", options.switch_rounds, options.leave_trials)
    adb = rac.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-front-"))
    guest = None
    try:
        apk = probe.build_probe_apk(private / "probe.apk")
        guest = rac.start_guest(options, plan, adb, private)
        output = options.output_dir or private
        output.mkdir(parents=True, exist_ok=True)
        installed = guest.adb_run("install", "-r", "-g", str(apk), timeout=COMMAND_SECONDS).stdout.strip()
        if "Success" not in installed:
            raise Failure(f"the probe was not installed: {installed!r}")
        guest.adb_run("shell", f"pm grant {probe.PACKAGE} android.permission.CAMERA")
        dump = guest.shell("dumpsys media.camera", timeout=COMMAND_SECONDS)
        (output / "dumpsys-media-camera.txt").write_text(dump)
        stated = dump_orientations(dump)
        print(f"the camera service states {stated}")
        problems = []
        for camera in (FRONT, BACK):
            info, frame = read_camera(guest, adb, camera, output)
            print(f"camera {camera}: orientation {info.orientation} facing {info.facing} preview "
                  f"{info.width}x{info.height} format {info.preview_format}, {len(frame)} bytes")
            problems += check_camera(camera, info, frame)
            if stated.get(str(camera)) not in (None, info.orientation):
                problems.append(f"camera {camera}: the camera service states {stated[str(camera)]}, API 1 says "
                                f"{info.orientation}")
        if stated.get(str(FRONT)) is None:
            problems.append("the camera service's dump states no orientation for camera 1")
        if problems:
            raise Failure("; ".join(problems))
        print("PASS")
        return 0
    except (Failure, rac.boot_android.LauncherError, subprocess.TimeoutExpired, OSError, ValueError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        if guest is not None and not isinstance(problem, rac.boot_android.LauncherError):
            rac.print_camera_log(guest)
        return 1
    finally:
        if guest is not None:
            rac.boot_android.disconnect_adb(adb, guest.serial)
            rac.boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
