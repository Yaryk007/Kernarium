#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The camera application's product settings, the patch that defines them and the verifier agree.

The product overlay of the camera application (Aperture) replaces resources the application's own
patch series defines: the switch for free zoom (default on, off in the overlay) and the two settings
that list each lens of the host. A name that one side renames leaves the overlay applying to nothing,
and the verifier's constants must name the same settings and values as the overlay. The series also
ends a recording when the camera closes with a recoverable error (the host ended the camera's video):
the camera video-lost integration row relies on it. The call that does it must be code in that branch,
and the view model function behind it (the one stop, which the shutter uses too, and the guards of the
stopped recording's controls) must be code as well: the checks read the patch's added lines without
their comments, so a call that is commented out does not count.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import re
import shutil
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

OVERLAY_CONFIG = Path("overlay/overlay/packages/apps/Aperture/app/src/main/res/values/config.xml")
SERIES_DIRECTORY = Path("patches/projects/p0031/series")
VERIFIER = Path("tools/verify-target-files.py")
ZOOM = "config_enableZoom"
AUX_CAMERAS = "config_enableAuxCameras"
IGNORED_AUX_CAMERAS = "config_ignoredAuxCameraIds"
MAIN_CAMERAS = "LensZoomRatioUtils.getMainCameras("
ACTIVITY_FILE = "app/src/main/java/org/lineageos/aperture/CameraActivity.kt"
VIEW_MODEL_FILE = "app/src/main/java/org/lineageos/aperture/viewmodels/CameraViewModel.kt"
RECOVERABLE_BRANCH = "CameraXCameraState.ERROR_OTHER_RECOVERABLE_ERROR ->"
RECOVERABLE_TOAST = "showToast(R.string.error_other_recoverable_error)"
END_RECORDING = "viewModel.endVideoRecording()"
# What the view model's patch must add as code, with the number of lines that must have it.
VIEW_MODEL_CODE = (
    ("fun endVideoRecording(): Boolean", 1),
    ("recording.stop()", 1),
    ("isVideoRecordingEnding(", 3),  # the definition, the pause and resume button, the microphone toggle
    ("endingVideoRecording.compareAndSet(recordingId, 0L)", 1),  # the Finalize clears the mark of its own recording
    ("videoRecording.compareAndSet(", 2),  # the Finalize retires its own handle, and a Finalize before the handle is stored does
    ("else -> Log.w(", 1),  # the button pressed with no recording to pause or resume is logged, it does not throw
)
FINALIZE_CLEAR = "endingVideoRecording.compareAndSet(recordingId, 0L)"
FINALIZE_RETIRE = "videoRecording.compareAndSet(started.get(), null)"
FINALIZE_IDLE = "cameraState.value = CameraState.IDLE"
FINALIZE_DROP = "videoRecording.value = null"
FINALIZE_FLAG = "finalized.set(true)"
CANCEL_START = "videoRecordingStart?.cancel()"
# The start's own steps after startRecording() returns.
START_STORE_STARTED = "started.set(recording)"
START_STORE_HANDLE = "videoRecording.value = recording"
START_CHECK = "if (finalized.get()) {"
START_RETIRE = "videoRecording.compareAndSet(recording, null)"


def fail(message: str) -> None:
    raise SystemExit(f"android camera application contract fixture: {message}")


def expect_failure(function, label: str) -> None:
    try:
        function()
    except SystemExit:
        return
    fail(f"the check accepted {label}")


def overlay_values(config: Path) -> tuple[dict[str, str], dict[str, list[str]]]:
    booleans: dict[str, str] = {}
    arrays: dict[str, list[str]] = {}
    for element in ET.parse(config).getroot():
        name = element.get("name")
        if element.tag == "bool":
            booleans[name] = (element.text or "").strip()
        elif element.tag == "string-array":
            arrays[name] = [(item.text or "").strip() for item in element.findall("item")]
    return booleans, arrays


def series_patches(series_directory: Path) -> str:
    """The text of every patch of the series, in the order series.tsv lists them."""
    names = [
        line.split("\t", 1)[1]
        for line in (series_directory / "series.tsv").read_text(encoding="utf-8").splitlines()
        if line.startswith("patch\t")
    ]
    if not names:
        fail(f"{series_directory} lists no patch")
    return "\n".join((series_directory / name).read_text(encoding="utf-8") for name in names)


def file_diffs(patches: str) -> dict[str, list[list[str]]]:
    """The hunks of each file the patches change, by path: a hunk is the list of its lines that are context, added or removed
    (the first character says which)."""
    files: dict[str, list[list[str]]] = {}
    hunk: list[str] | None = None
    for line in patches.splitlines():
        if line.startswith("diff --git "):
            hunk = None
            current = files.setdefault(line.split(" b/", 1)[1], [])
        elif line.startswith("@@"):
            hunk = []
            current.append(hunk)
        elif hunk is not None and line[:1] in (" ", "+", "-"):
            hunk.append(line)
    return files


def code_of(body: list[tuple[bool, str]]) -> list[tuple[bool, str]]:
    """The lines (added or not, text) with the comments taken out: `//` to the end of the line and `/* */`, which can span lines.
    A call inside a comment is not code."""
    result = []
    in_block = False
    for added, text in body:
        code, rest = "", text
        while rest:
            if in_block:
                end = rest.find("*/")
                if end < 0:
                    rest = ""
                else:
                    rest, in_block = rest[end + 2:], False
                continue
            line_comment, block = rest.find("//"), rest.find("/*")
            if line_comment >= 0 and (block < 0 or line_comment < block):
                code, rest = code + rest[:line_comment], ""
            elif block >= 0:
                code, rest, in_block = code + rest[:block], rest[block + 2:], True
            else:
                code, rest = code + rest, ""
        result.append((added, code.strip()))
    return result


def body_of(hunk: list[str]) -> list[tuple[bool, str]]:
    """What the file has after the hunk: context and added lines, whether each was added."""
    return [(line[0] == "+", line[1:]) for line in hunk if line[0] != "-"]


def recoverable_branch_code(patches: str) -> list[str]:
    """The code that the patches ADD to the recoverable-error branch of the activity: the added lines between the branch's label
    and its toast, in the hunk that holds the label, without comments."""
    for hunk in file_diffs(patches).get(ACTIVITY_FILE, []):
        body = body_of(hunk)
        for start, (_, text) in enumerate(body):
            if RECOVERABLE_BRANCH not in text:
                continue
            for end in range(start + 1, len(body)):
                if RECOVERABLE_TOAST in body[end][1]:
                    return [code for added, code in code_of(body[start + 1:end]) if added and code]
    fail(f"the camera application's patches have no hunk of {ACTIVITY_FILE} that holds the recoverable-error branch up to its toast")


def view_model_code(patches: str) -> list[str]:
    """The code that the patches ADD to the view model, without comments."""
    added = []
    for hunk in file_diffs(patches).get(VIEW_MODEL_FILE, []):
        added += [code for was_added, code in code_of(body_of(hunk)) if was_added and code]
    return added


def finalize_order(patches: str) -> bool:
    """How the view model's Finalize handler and the start that stored the handle hand a recording over, in the hunk that adds the
    mark's clear (code only, without comments).

    The Finalize handler flags that its event came, retires the handle of its OWN recording (compare-and-set against the handle its
    start stored, never a plain write that a later recording's handle could lose to), publishes idle, which lets the next
    recording start and store its handle, then clears the ending mark. Flag before retire: otherwise an event that comes between
    the start's two steps below retires nothing and flags too late for the start to see it.

    The start stores the handle it got in `started` and in the state flow, both BEFORE it looks at the flag, and retires the
    handle after that look. Then, whichever of the two threads goes first, one of them retires the handle.

    The pin is structural: it reads the order of the patch's code lines, it is no compiler and no concurrency test."""
    for hunk in file_diffs(patches).get(VIEW_MODEL_FILE, []):
        body = code_of(body_of(hunk))
        for index, (added, code) in enumerate(body):
            if added and FINALIZE_CLEAR in code:
                texts = [text for _, text in body]
                before, after = texts[:index], texts[index + 1:]
                try:
                    flag = len(before) - 1 - before[::-1].index(FINALIZE_FLAG)
                    retire = len(before) - 1 - before[::-1].index(FINALIZE_RETIRE)
                    idle = len(before) - 1 - before[::-1].index(FINALIZE_IDLE)
                    stored_started = after.index(START_STORE_STARTED)
                    stored_handle = after.index(START_STORE_HANDLE)
                    check = after.index(START_CHECK)
                    late = after.index(START_RETIRE)
                except ValueError:
                    return False
                return (flag < retire < idle and FINALIZE_IDLE not in texts[:retire] and FINALIZE_DROP not in texts
                        and stored_started < check and stored_handle < check < late)
    return False


def idle_published_only_by_the_cancel(patches: str) -> bool:
    """The only idle that the patch ADDS to the view model is the cancel of a start that waits for its sound (it follows the
    cancel of the start's job). The Finalize handler's own idle is upstream's line, which finalize_order puts after the retire: an
    extra idle added anywhere else, before the retire or not, lets the next recording start while the old handle stands."""
    for hunk in file_diffs(patches).get(VIEW_MODEL_FILE, []):
        previous = ""
        for added, code in code_of(body_of(hunk)):
            if added and code == FINALIZE_IDLE and previous != CANCEL_START:
                return False
            if code:
                previous = code
    return True


def load_verifier(path: Path):
    specification = importlib.util.spec_from_file_location("verify_target_files", path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def check(android_root: Path) -> None:
    booleans, arrays = overlay_values(android_root / OVERLAY_CONFIG)
    if booleans.get(ZOOM) != "false":
        fail(f"the overlay must set {ZOOM} to false: {booleans.get(ZOOM)!r}")
    if booleans.get(AUX_CAMERAS) != "true":
        fail(f"the overlay must set {AUX_CAMERAS} to true: {booleans.get(AUX_CAMERAS)!r}")
    if arrays.get(IGNORED_AUX_CAMERAS) != ["2"]:
        fail(f"the overlay must ignore camera 2 and no other: {arrays.get(IGNORED_AUX_CAMERAS)!r}")

    verifier = load_verifier(android_root / VERIFIER)
    for name, value in (("CAMERA_ZOOM", ZOOM), ("CAMERA_AUX_CAMERAS", AUX_CAMERAS),
                        ("CAMERA_IGNORED_AUX_CAMERAS", IGNORED_AUX_CAMERAS)):
        if getattr(verifier, name, None) != value:
            fail(f"the verifier's {name} is not {value}")
    if verifier.CAMERA_IGNORED_AUX_IDS != arrays[IGNORED_AUX_CAMERAS]:
        fail("the verifier and the overlay ignore different camera IDs")

    # The application's own patch defines the switch, on by default, and reads it through the
    # overlay repository, which is what lets a product's overlay turn it off.
    patches = series_patches(android_root / SERIES_DIRECTORY)
    added = [line[1:].strip() for line in patches.splitlines() if line.startswith("+")]
    if f'<bool name="{ZOOM}">true</bool>' not in added:
        fail(f"the camera application's patches do not define {ZOOM} as true by default")
    for needle in (f"getBoolean(R.bool.{ZOOM})", "isPinchToZoomEnabled = overlaysRepository.enableZoom"):
        if not any(needle in line for line in added):
            fail(f"the camera application's patches never use the switch: no line adds {needle}")
    # The lens buttons' labels come from one rule, snapped to the labels of a phone's lens buttons.
    labels = re.search(r"floatArrayOf\(([^)]*)\)", patches)
    if labels is None or [item.strip() for item in labels.group(1).split(",")] != [
            "0.5f", "1f", "2f", "2.5f", "3f", "4f", "5f"]:
        fail("the camera application's patches do not snap the lens labels to 0.5, 1, 2, 2.5, 3, 4 and 5")
    # The labels of a side are relative to its main camera, found for the internal and the external
    # cameras alike by the same grouping.
    if not any(MAIN_CAMERAS in line for line in added):
        fail(f"the camera application's patches never group the cameras of a side: no line adds {MAIN_CAMERAS}")
    # The recording ends when the camera closes with a recoverable error: the host ending the camera's
    # video reaches the application as that error, and nothing else of the HAL's report ends the clip.
    # The call is code in that branch (not a comment), and it is the view model's one function that stops
    # a recording and marks it as ending until its Finalize, which the shutter's stop uses as well.
    if not any(END_RECORDING in code for code in recoverable_branch_code(patches)):
        fail("the camera application's patches never end a recording on the camera's recoverable error: "
             f"no added line of that branch has {END_RECORDING} as code")
    model = view_model_code(patches)
    for needle, count in VIEW_MODEL_CODE:
        found = sum(needle in code for code in model)
        if found < count:
            fail(f"the camera application's patches add {needle!r} as code on {found} line(s) of the view model, {count} needed")
    if "endVideoRecording()" not in model:
        fail("the camera application's patches do not stop a recording from the shutter through endVideoRecording()")
    if not idle_published_only_by_the_cancel(patches):
        fail("the camera application's patches add a state reset to idle to the view model other than the cancel of a start that "
             "waits for its sound")
    if not finalize_order(patches):
        fail("the camera application's patches do not end the Finalize handler as the contract says: retire the handle of its own "
             "recording, publish idle, then clear the ending mark, with no plain write of a null handle")


def fixtures(android_root: Path) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        copy = Path(temporary) / "lineage-24"
        for relative in (OVERLAY_CONFIG, SERIES_DIRECTORY, VERIFIER):
            source = android_root / relative
            target = copy / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)
        config = copy / OVERLAY_CONFIG
        good = config.read_text(encoding="utf-8")
        check(copy)

        for label, old, new in (
            ("free zoom left on", f'<bool name="{ZOOM}">false</bool>', f'<bool name="{ZOOM}">true</bool>'),
            ("no zoom switch", f'    <bool name="{ZOOM}">false</bool>\n', ""),
            ("a zoom switch under another name", f'name="{ZOOM}"', 'name="config_zoomEnabled"'),
            ("no ignored camera", "        <item>2</item>\n", ""),
            ("aux cameras off", f'<bool name="{AUX_CAMERAS}">true</bool>', f'<bool name="{AUX_CAMERAS}">false</bool>'),
        ):
            if old not in good:
                fail(f"the fixture for {label} found nothing to change in the overlay")
            config.write_text(good.replace(old, new), encoding="utf-8")
            expect_failure(lambda: check(copy), label)
        config.write_text(good, encoding="utf-8")

        for patch in sorted((copy / SERIES_DIRECTORY).glob("*.patch")):
            text = patch.read_text(encoding="utf-8")
            if f'<bool name="{ZOOM}">true</bool>' not in text:
                continue
            patch.write_text(text.replace(f'<bool name="{ZOOM}">true</bool>',
                                          f'<bool name="{ZOOM}">false</bool>'), encoding="utf-8")
            expect_failure(lambda: check(copy), "a patch that turns free zoom off by default")
            patch.write_text(text, encoding="utf-8")
            if MAIN_CAMERAS in text:
                patch.write_text(text.replace(MAIN_CAMERAS, "mainCamerasOf("), encoding="utf-8")
                expect_failure(lambda: check(copy), "a patch that does not group the cameras of a side")
                patch.write_text(text, encoding="utf-8")
        for patch in sorted((copy / SERIES_DIRECTORY).glob("*.patch")):
            text = patch.read_text(encoding="utf-8")
            if END_RECORDING not in text:
                continue
            # The call is gone, or it is there as text and no code: a comment of either kind.
            for label, new in (
                ("a patch that does not end the recording on the camera's recoverable error",
                 "viewModel.videoRecording.value?.pause()"),
                ("a patch whose call is commented out with //", "// " + END_RECORDING),
                ("a patch whose call is commented out with /* */", "/* " + END_RECORDING + " */ false"),
            ):
                patch.write_text(text.replace(END_RECORDING, new), encoding="utf-8")
                expect_failure(lambda: check(copy), label)
            patch.write_text(text, encoding="utf-8")
            # The call moved out of the branch (after its toast) is not the branch's.
            lines = text.splitlines()
            toast = next(line for line in lines if RECOVERABLE_TOAST in line)
            call = next(line for line in lines if END_RECORDING in line)
            moved = text.replace(call + "\n", "").replace(toast, toast + "\n" + call)
            patch.write_text(moved, encoding="utf-8")
            expect_failure(lambda: check(copy), "a patch whose call is after the branch's toast")
            patch.write_text(text, encoding="utf-8")
        for patch in sorted((copy / SERIES_DIRECTORY).glob("*.patch")):
            text = patch.read_text(encoding="utf-8")
            if "fun endVideoRecording" not in text:
                continue
            for label, old, new in (
                ("a view model that never stops the recording", "+            recording.stop()", "+            // recording.stop()"),
                ("a view model whose shutter stops the recording by itself", "+                endVideoRecording()",
                 "+                videoRecording.value?.stop()"),
                ("a pause and resume button without its guard", 'if (isVideoRecordingEnding("The pause or resume")) {', "if (false) {"),
                ("a mute without its guard", 'if (!isVideoRecordingEnding("Muting the recording")) {', "if (true) {"),
                ("a Finalize that never clears the mark", "endingVideoRecording.compareAndSet(recordingId, 0L)", "recordingId"),
                ("a button that throws with no recording to press", "else -> Log.w(", "else -> error("),
                ("a Finalize before the handle is stored that leaves the handle", "videoRecording.compareAndSet(recording, null)",
                 "recording"),
                ("a Finalize that retires whatever handle is there", FINALIZE_RETIRE, "videoRecording.compareAndSet(videoRecording.value, null)"),
                ("a Finalize that writes a null handle", FINALIZE_RETIRE, FINALIZE_DROP),
            ):
                if old not in text:
                    fail(f"the fixture for {label} found nothing to change in the patch")
                patch.write_text(text.replace(old, new), encoding="utf-8")
                expect_failure(lambda: check(copy), label)
                patch.write_text(text, encoding="utf-8")
        for patch in sorted((copy / SERIES_DIRECTORY).glob("*.patch")):
            text = patch.read_text(encoding="utf-8")
            if FINALIZE_CLEAR not in text:
                continue
            lines = text.split("\n")

            def find(needle: str, prefix: str) -> int:
                return next(number for number, line in enumerate(lines) if line.startswith(prefix) and needle in line)

            flag, retire, idle, clear = (find(FINALIZE_FLAG, "+"), find(FINALIZE_RETIRE, "+"), find(FINALIZE_IDLE, " "),
                                         find(FINALIZE_CLEAR, "+"))
            stored_started, stored_handle = find(START_STORE_STARTED, "+"), find(START_STORE_HANDLE, "+")
            look, late = find(START_CHECK, "+"), find(START_RETIRE, "+")
            if not flag < retire < idle < clear < stored_started < stored_handle < look < late:
                fail("the fixtures for the order of the Finalize handler found its lines out of order in the patch")

            def move(source: int, before: int) -> list[str]:
                """The patch with the line `source` moved in front of the line `before`."""
                rest = lines[:source] + lines[source + 1:]
                position = before - (1 if source < before else 0)
                return rest[:position] + [lines[source]] + rest[position:]

            def swap(first: int, second: int) -> list[str]:
                swapped = list(lines)
                swapped[first], swapped[second] = lines[second], lines[first]
                return swapped

            for label, variant in (
                ("a patch that publishes idle before it retires the handle", swap(retire, idle)),
                ("a patch that clears the ending mark before the state goes idle", move(clear, idle)),
                ("a patch that clears the ending mark before it retires the handle", move(clear, retire)),
                ("a patch that writes a null handle after idle", lines[:idle + 1] + ["+" + " " * 24 + FINALIZE_DROP] + lines[idle + 1:]),
                ("a patch that retires the handle before it flags the event", swap(flag, retire)),
                ("a patch whose start stores its handle in the holder after it looks at the flag", move(stored_started, late + 1)),
                ("a patch whose start stores its handle in the state flow after it looks at the flag", move(stored_handle, late + 1)),
                ("a patch whose start retires the handle before it looks at the flag", swap(look, late)),
                ("a patch that publishes an extra idle before the retire, then the sequence", lines[:flag] + ["+" + " " * 24 + FINALIZE_IDLE] + lines[flag:]),
                ("a patch that publishes an extra idle between the flag and the retire",
                 lines[:retire] + ["+" + " " * 24 + FINALIZE_IDLE] + lines[retire:]),
                ("a Finalize handler without the flag", lines[:flag] + lines[flag + 1:]),
                ("a Finalize handler whose flag is a comment", lines[:flag] + [lines[flag].replace(FINALIZE_FLAG, "// " + FINALIZE_FLAG)] + lines[flag + 1:]),
            ):
                patch.write_text("\n".join(variant), encoding="utf-8")
                expect_failure(lambda: check(copy), label)
            patch.write_text(text, encoding="utf-8")
        check(copy)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    android_root = args.source_root.resolve() / "android/lineage-24"
    check(android_root)
    fixtures(android_root)
    print("android camera application contract passed")


if __name__ == "__main__":
    main()
