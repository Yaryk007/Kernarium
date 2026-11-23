#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The archive switch of the AOT component gates the accepted artifact.

cmake/UmlAotArchives.cmake reads LINEAGE24_ACCEPTED_AOT_ARCHIVES from the
accepted AOT descriptor and selects the accepted artifact for a product
(darwin/cpu/CMakeLists.txt calls uml_aot_accepted_artifact). The rows run both
functions with `cmake -P` on a synthetic source tree that holds the descriptor,
the accepted release and an installed artifact: an iOS device runtime takes the
artifact only when the descriptor says yes, macOS and the Simulator take it
under either value, and the same rows must fail a module whose yes/no predicate
is turned round. Then they check that darwin/cpu/CMakeLists.txt makes that
call, and that an image blob is never an archive input.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ARGUMENTS: argparse.Namespace


def cmake(script: str, module: str | None = None, **definitions: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "probe.cmake"
        path.write_text(
            f'include("{module or ARGUMENTS.module}")\n{script}\n', encoding="utf-8")
        options = [f"-D{name}={value}" for name, value in definitions.items()]
        return subprocess.run(
            [ARGUMENTS.cmake, *options, "-P", str(path)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60, check=False)


class ArchiveSwitchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def descriptor(self, text: str | None) -> Path:
        path = self.directory / "accepted-aot.env"
        path.unlink(missing_ok=True)
        if text is not None:
            path.write_text(text, encoding="utf-8")
        return path

    def state(self, descriptor: Path) -> subprocess.CompletedProcess:
        return cmake('uml_aot_archives_state(state "${DESCRIPTOR}")\n'
                     'message("state=${state}")', DESCRIPTOR=str(descriptor))

    def test_the_state_is_what_the_descriptor_says(self) -> None:
        for text, expected in (
            ("LINEAGE24_ACCEPTED_AOT_ARCHIVES='yes'\n", "yes"),
            ("A='1'\nLINEAGE24_ACCEPTED_AOT_ARCHIVES='no'\nB='2'\n", "no"),
            (None, "none"),
        ):
            with self.subTest(expected=expected):
                result = self.state(self.descriptor(text))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"state={expected}", result.stderr)

    def test_a_descriptor_that_states_no_switch_is_refused(self) -> None:
        for text in ("A='1'\n", "LINEAGE24_ACCEPTED_AOT_ARCHIVES='maybe'\n",
                     "LINEAGE24_ACCEPTED_AOT_ARCHIVES=yes\n"):
            with self.subTest(text=text):
                result = self.state(self.descriptor(text))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("LINEAGE24_ACCEPTED_AOT_ARCHIVES of yes or no", result.stderr)


RELEASE = "rel-1"
PLATFORMS = ("ios", "macos", "ios-simulator")


def source_tree(root: Path, archives: str, installed: bool = True,
                accepted_id: str = RELEASE) -> Path:
    """A source tree with the accepted descriptor, the accepted release and,
    when `installed`, the artifact below .build/android/aot."""
    metadata = root / "android" / "lineage-24" / "metadata"
    metadata.mkdir(parents=True)
    (metadata / "accepted-aot.env").write_text(
        f"LINEAGE24_ACCEPTED_AOT_RELEASE_ID='{accepted_id}'\n"
        f"LINEAGE24_ACCEPTED_AOT_ARCHIVES='{archives}'\n", encoding="utf-8")
    (metadata / "accepted-release.env").write_text(
        f"LINEAGE24_ACCEPTED_RELEASE_ID='{RELEASE}'\n"
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME='release-dir'\n", encoding="utf-8")
    if installed:
        artifact = root / ".build" / "android" / "aot" / "release-dir"
        artifact.mkdir(parents=True)
        (artifact / "artifact.json").write_text("{}\n", encoding="utf-8")
    return root


def selection(module: str, root: Path, platform: str, mode: str = "AUTO"):
    """What uml_aot_accepted_artifact selects for `platform`: the result of the
    run, and the variables it set (empty when it left them alone)."""
    result = cmake(
        'uml_aot_accepted_artifact("${ROOT}" "${PLATFORM}" "${MODE}" '
        'taken_source taken_directory taken_descriptor)\n'
        'message("selected=[${taken_source}] directory=[${taken_directory}] '
        'descriptor=[${taken_descriptor}]")',
        module=module, ROOT=str(root), PLATFORM=platform, MODE=mode)
    found = re.search(r"selected=\[(.*)\] directory=\[(.*)\] descriptor=\[(.*)\]",
                      result.stderr)
    return result, (found.groups() if found else None)


def selection_failures(module: str) -> list[str]:
    """Where the module takes the artifact against the switch: an iOS device
    runtime under yes alone, macOS and the Simulator under both values."""
    failures = []
    for archives in ("yes", "no"):
        with tempfile.TemporaryDirectory() as directory:
            root = source_tree(Path(directory), archives)
            for platform in PLATFORMS:
                taken = archives == "yes" or platform != "ios"
                result, groups = selection(module, root, platform)
                label = f"{platform} with {archives}"
                if result.returncode != 0 or groups is None:
                    failures.append(f"{label}: {result.stderr.strip()}")
                    continue
                source, artifact, descriptor = groups
                expected = (
                    ("artifact", str(root / ".build/android/aot/release-dir"),
                     str(root / "android/lineage-24/metadata/accepted-aot.env"))
                    if taken else ("", "", ""))
                if (source, artifact, descriptor) != expected:
                    failures.append(f"{label}: selected {groups}, expected {expected}")
    return failures


class SelectionTests(unittest.TestCase):
    """The accepted artifact of a product, on an installed fixture."""

    def setUp(self) -> None:
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.module = ARGUMENTS.module

    def test_the_artifact_follows_the_switch_and_the_product(self) -> None:
        self.assertEqual(selection_failures(self.module), [])

    def test_an_ios_device_under_no_says_why(self) -> None:
        result, groups = selection(self.module, source_tree(self.root, "no"), "ios")
        self.assertEqual(groups, ("", "", ""))
        self.assertIn("not accepted for archives yet", result.stdout + result.stderr)

    def test_a_missing_install_is_said_where_the_artifact_would_be_taken(self) -> None:
        root = source_tree(self.root, "yes", installed=False)
        for platform in PLATFORMS:
            with self.subTest(platform=platform):
                result, groups = selection(self.module, root, platform)
                self.assertEqual(groups, ("", "", ""))
                self.assertIn("is not installed at", result.stdout + result.stderr)

    def test_no_descriptor_selects_nothing(self) -> None:
        for platform in PLATFORMS:
            with self.subTest(platform=platform):
                result, groups = selection(self.module, self.root, platform)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(groups, ("", "", ""))

    def test_a_descriptor_for_another_release_is_refused(self) -> None:
        root = source_tree(self.root, "yes", accepted_id="rel-2")
        result, _ = selection(self.module, root, "macos")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("accepts the artifact of 'rel-2'", result.stderr)

    def test_a_module_that_turns_the_switch_round_fails_the_rows(self) -> None:
        text = Path(self.module).read_text(encoding="utf-8")
        turned = text.replace('if(archive_product AND NOT archives STREQUAL "yes")',
                              'if(archive_product AND archives STREQUAL "yes")')
        self.assertNotEqual(turned, text, "the module no longer holds the predicate this row turns round")
        mutant = self.root / "UmlAotArchivesTurned.cmake"
        mutant.write_text(turned, encoding="utf-8")
        self.assertNotEqual(selection_failures(str(mutant)), [])

    def test_a_module_that_ignores_the_switch_fails_the_rows(self) -> None:
        text = Path(self.module).read_text(encoding="utf-8")
        ignored = text.replace('if(archive_product AND NOT archives STREQUAL "yes")',
                               'if(FALSE)')
        self.assertNotEqual(ignored, text)
        mutant = self.root / "UmlAotArchivesIgnored.cmake"
        mutant.write_text(ignored, encoding="utf-8")
        self.assertNotEqual(selection_failures(str(mutant)), [])


class WiringTests(unittest.TestCase):
    """darwin/cpu/CMakeLists.txt takes the component under the switch."""

    def setUp(self) -> None:
        self.text = Path(ARGUMENTS.cpu_lists).read_text(encoding="utf-8")

    def test_the_accepted_artifact_is_selected_by_the_module(self) -> None:
        call = re.search(r"uml_aot_accepted_artifact\(([^)]*)\)", self.text)
        self.assertIsNotNone(call, "darwin/cpu/CMakeLists.txt does not select the accepted artifact")
        arguments = call.group(1).split()
        self.assertEqual(
            arguments,
            ['"${PROJECT_SOURCE_DIR}"', '"${UML_APPLE_PLATFORM}"', '"${UML_AOT_MODE}"',
             "UML_AOT_SOURCE", "UML_AOT_ARTIFACT_DIR", "UML_AOT_ARTIFACT_ACCEPTED"])

    def test_the_switch_is_decided_in_the_module_alone(self) -> None:
        self.assertNotIn("uml_aot_archives_state", self.text)
        self.assertNotIn("LINEAGE24_ACCEPTED_AOT_ARCHIVES", self.text.replace("(LINEAGE24_ACCEPTED_AOT_ARCHIVES)", ""))

    def test_an_image_blob_is_not_an_archive_input(self) -> None:
        # UML_AOT_IMAGE qualifies a release on macOS and cannot be an iOS product.
        image = re.search(r"if\(UML_AOT_IMAGE\)(.*?)else\(\)", self.text, re.S)
        self.assertIsNotNone(image)
        self.assertIn("NOT UML_TARGET_MACOS", image.group(1))


def main() -> int:
    global ARGUMENTS
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--cpu-lists", required=True)
    ARGUMENTS, rest = parser.parse_known_args()
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
