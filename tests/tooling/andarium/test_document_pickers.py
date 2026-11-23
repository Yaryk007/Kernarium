#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium opens the files a person picks as copies (Andarium/scripts/check-document-pickers.py).

The application's sources must hold no SwiftUI fileImporter, no document picker
made without `asCopy: true` outside Shared/CopyDocumentPicker.swift, and both
call sites (Install APK and the file browser's Import) must present the
copy-mode picker. The first case reads this tree's sources; the others change
one thing in a fixture copy of the two call sites and the presenter, and expect
the check to refuse it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(".")
SCRIPT = Path("Andarium/scripts/check-document-pickers.py")
SOURCES = Path("Andarium/Andarium")
FILES = (
    "Shared/Settings.swift",
    "Shared/FileBrowserBar.swift",
    "Shared/CopyDocumentPicker.swift",
)


def check(sources: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(ROOT / SCRIPT), str(sources)], check=False,
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)


class Tree(unittest.TestCase):
    def setUp(self) -> None:
        self.sources = Path(self.enterContext(tempfile.TemporaryDirectory())) / "Andarium"
        for name in FILES:
            target = self.sources / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / SOURCES / name, target)

    def edit(self, name: str, old: str, new: str) -> None:
        path = self.sources / name
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text, f"{name} no longer holds the text this case changes")
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def add(self, name: str, text: str) -> None:
        path = self.sources / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def refused(self, *words: str) -> None:
        result = check(self.sources)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for word in words:
            self.assertIn(word, result.stderr)

    def accepted(self) -> None:
        result = check(self.sources)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class TheSources(unittest.TestCase):
    def test_this_trees_sources_pass(self) -> None:
        result = check(ROOT / SOURCES)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_missing_source_directory_fails(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            result = check(Path(scratch) / "absent")
        self.assertEqual(result.returncode, 1)


class FileImporter(Tree):
    def test_the_fixture_passes(self) -> None:
        self.accepted()

    def test_an_importer_in_another_source_fails(self) -> None:
        self.add("Shared/Third.swift",
                 "struct Third: View {\n    var body: some View {\n"
                 "        Color.clear.fileImporter(isPresented: .constant(false),\n"
                 "                                 allowedContentTypes: [.item]) { _ in }\n    }\n}\n")
        self.refused("Shared/Third.swift:3", "fileImporter")

    def test_an_importer_in_a_call_site_fails(self) -> None:
        self.edit("Shared/Settings.swift", ".documentCopyPicker(",
                  ".fileImporter(isPresented: .constant(false), allowedContentTypes: [.item]) { _ in }\n        .documentCopyPicker(")
        self.refused("Shared/Settings.swift", "fileImporter")

    def test_an_importer_in_a_comment_or_a_string_is_no_code(self) -> None:
        self.add("Shared/Third.swift",
                 "// .fileImporter(isPresented: x, allowedContentTypes: []) { _ in }\n"
                 "/* fileImporter(isPresented: x) /* nested */ fileImporter( */\n"
                 'let words = "fileImporter(is not called"\n')
        self.accepted()


class DocumentPicker(Tree):
    def test_a_picker_without_the_copy_flag_fails(self) -> None:
        self.add("Shared/Third.swift",
                 "let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item])\n")
        self.refused("Shared/Third.swift:1", "asCopy")

    def test_a_picker_with_the_copy_flag_off_fails(self) -> None:
        self.add("Shared/Third.swift",
                 "let picker = UIDocumentPickerViewController(\n"
                 "    forOpeningContentTypes: [.item], asCopy: false)\n")
        self.refused("Shared/Third.swift:1", "asCopy")

    def test_an_export_picker_without_the_copy_flag_fails(self) -> None:
        self.add("Shared/Third.swift",
                 "let picker = UIDocumentPickerViewController(forExporting: [url])\n")
        self.refused("Shared/Third.swift:1", "asCopy")

    def test_an_init_spelling_does_not_hide_a_picker(self) -> None:
        self.add("Shared/Third.swift",
                 "let picker = UIDocumentPickerViewController.init(forOpeningContentTypes: [.item])\n")
        self.refused("Shared/Third.swift:1")

    def test_a_copy_picker_outside_the_presenter_fails(self) -> None:
        self.add("Shared/Third.swift",
                 "let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item], asCopy: true)\n")
        self.refused("Shared/Third.swift:1", "CopyDocumentPicker.swift")

    def test_a_picker_in_a_comment_is_no_code(self) -> None:
        self.add("Shared/Third.swift",
                 "/// A picker (`UIDocumentPickerViewController(forOpeningContentTypes: [.item])`).\n")
        self.accepted()


class Presenter(Tree):
    def test_the_presenter_without_the_copy_flag_fails(self) -> None:
        self.edit("Shared/CopyDocumentPicker.swift", "asCopy: true)", "asCopy: false)")
        self.refused("Shared/CopyDocumentPicker.swift", "asCopy")

    def test_the_presenter_with_no_picker_fails(self) -> None:
        self.edit("Shared/CopyDocumentPicker.swift", "UIDocumentPickerViewController(", "Picker(")
        self.refused("Shared/CopyDocumentPicker.swift", "creates no document picker")

    def test_the_presenter_without_its_modifier_fails(self) -> None:
        self.edit("Shared/CopyDocumentPicker.swift", "func documentCopyPicker(", "func somethingElse(")
        self.refused("Shared/CopyDocumentPicker.swift", "documentCopyPicker")

    def test_a_missing_presenter_fails(self) -> None:
        (self.sources / "Shared/CopyDocumentPicker.swift").unlink()
        self.refused("presenter is missing")


class CallSites(Tree):
    def test_install_apk_without_the_copy_picker_fails(self) -> None:
        self.edit("Shared/Settings.swift", ".documentCopyPicker(", ".somethingElse(")
        self.refused("Shared/Settings.swift", "presents no documentCopyPicker")

    def test_the_file_browser_without_the_copy_picker_fails(self) -> None:
        self.edit("Shared/FileBrowserBar.swift", ".documentCopyPicker(", ".somethingElse(")
        self.refused("Shared/FileBrowserBar.swift", "presents no documentCopyPicker")

    def test_a_call_site_that_only_mentions_the_modifier_fails(self) -> None:
        self.edit("Shared/Settings.swift", ".documentCopyPicker(", "// .documentCopyPicker(\n        .somethingElse(")
        self.refused("Shared/Settings.swift", "presents no documentCopyPicker")

    def test_a_missing_call_site_fails(self) -> None:
        (self.sources / "Shared/FileBrowserBar.swift").unlink()
        self.refused("Shared/FileBrowserBar.swift: the call site is missing")


class BuildPhase(Tree):
    """The build phase "Check document pickers" runs the check on every build."""

    def phase(self) -> str:
        project = json.loads(re.sub(
            r",(\s*[}\]])", r"\1",
            (ROOT / "Andarium/Andarium.xcodeproj/project.xcproj").read_text(encoding="utf-8")))
        found = []

        def walk(value) -> None:
            if isinstance(value, dict):
                if value.get("kind") == "script" and value.get("name") == "Check document pickers":
                    found.append(value["script"])
                for item in value.values():
                    walk(item)
            elif isinstance(value, list):
                for item in value:
                    walk(item)

        walk(project)
        self.assertEqual(len(found), 1, "expected one build phase named Check document pickers")
        return found[0] if isinstance(found[0], str) else "\n".join(found[0])

    def run_phase(self, srcroot: Path, configuration: str) -> subprocess.CompletedProcess[str]:
        environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "SRCROOT": str(srcroot),
                       "CONFIGURATION": configuration}
        return subprocess.run(
            ["/bin/sh", "-c", self.phase()], env=environment, check=False,
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)

    def srcroot(self) -> Path:
        """What the phase sees as SRCROOT (Andarium/): the real script beside the fixture sources."""
        srcroot = self.sources.parent / "srcroot"
        (srcroot / "scripts").mkdir(parents=True)
        shutil.copy(ROOT / SCRIPT, srcroot / "scripts" / SCRIPT.name)
        (srcroot / "Andarium").symlink_to(self.sources, target_is_directory=True)
        return srcroot

    def test_the_phase_passes_a_tree_that_holds_to_it_in_every_configuration(self) -> None:
        srcroot = self.srcroot()
        for configuration in ("Debug", "Release"):
            with self.subTest(configuration):
                result = self.run_phase(srcroot, configuration)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_the_phase_stops_a_build_whose_sources_open_files_in_place_in_every_configuration(self) -> None:
        self.add("Shared/Third.swift", "let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item])\n")
        srcroot = self.srcroot()
        for configuration in ("Debug", "Release"):
            with self.subTest(configuration):
                result = self.run_phase(srcroot, configuration)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("asCopy", result.stderr)


def main() -> int:
    global ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    args, rest = parser.parse_known_args()
    ROOT = args.root
    return 0 if unittest.main(argv=[sys.argv[0], *rest], exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
