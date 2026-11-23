#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The licence metadata of the two accessibility applications.

The image builds TalkBack and eSpeak NG from source, and their series carry
the Soong build files with the licence modules the image's NOTICE reports.
Each project names the kinds of what it builds, and every copyleft kind names
the full text of its licence: a text the series adds, or one the pinned
upstream tree holds.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[3]
PATCHES = REPOSITORY / "dependencies/patches"
BUILD_PATCH = "Build-with-Soong.patch"
# The licence texts the pinned upstream trees hold, as the build files name them.
UPSTREAM_TEXTS = {
    "talkback": {"LICENSE"},
    "espeak-ng": {"COPYING", "COPYING.APACHE", "sonic/LICENSE"},
}
TEXTS = {
    "GPL-2.0-or-later": ("licenses/GPL-2.0.txt", "GNU GENERAL PUBLIC LICENSE", "Version 2, June 1991"),
    "LGPL-2.1-or-later": (
        "licenses/LGPL-2.1.txt", "GNU LESSER GENERAL PUBLIC LICENSE", "Version 2.1, February 1999"),
}
COPYLEFT = ("GPL-2.0-or-later", "LGPL-2.1-or-later", "GPL-3.0-or-later")


def added_files(project: str) -> dict[str, str]:
    """path -> text of each file the project's build patch adds."""
    files: dict[str, list[str]] = {}
    current = None
    for line in (PATCHES / project / BUILD_PATCH).read_text(encoding="utf-8").splitlines():
        if line.startswith("diff --git "):
            current = None
        elif line.startswith("+++ b/"):
            current = files.setdefault(line[len("+++ b/"):], [])
        elif current is not None and line.startswith("+") and not line.startswith("+++"):
            current.append(line[1:])
    return {path: "\n".join(lines) + "\n" for path, lines in files.items()}


def blocks(files: dict[str, str], kind: str) -> dict[str, tuple[str, str]]:
    """name -> (directory, body) of each top-level `kind { ... }` block."""
    found = {}
    for path, text in files.items():
        if not path.endswith("Android.bp"):
            continue
        for match in re.finditer(rf"^{kind} {{\n(.*?)^}}", text, re.S | re.M):
            name = re.search(r'name: "([^"]+)"', match.group(1))
            found[name.group(1)] = (str(Path(path).parent), match.group(1))
    return found


def listed(body: str, field: str) -> list[str]:
    match = re.search(rf"{field}: \[(.*?)\]", body, re.S)
    return re.findall(r'"([^"]+)"', match.group(1)) if match else []


class AccessibilityLicenseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.files = {name: added_files(name) for name in ("talkback", "espeak-ng")}
        self.licenses = {name: blocks(files, "license") for name, files in self.files.items()}

    def kinds(self, project: str) -> set[str]:
        return {
            kind.removeprefix("SPDX-license-identifier-")
            for _, body in self.licenses[project].values()
            for kind in listed(body, "license_kinds")
        }

    def test_each_application_is_built_and_its_package_names_its_licence(self) -> None:
        for project, module, default in (
            ("talkback", "TalkBack", "external_talkback_license"),
            ("espeak-ng", "eSpeakNG", "external_espeak_ng_license"),
        ):
            self.assertIn(module, blocks(self.files[project], "android_app"))
            self.assertIn(
                f'default_applicable_licenses: ["{default}"]', self.files[project]["Android.bp"]
            )
            self.assertIn(default, self.licenses[project])

    def test_the_kinds_are_what_each_project_builds(self) -> None:
        talkback = self.kinds("talkback")
        espeak = self.kinds("espeak-ng")
        self.assertIn("Apache-2.0", talkback)
        self.assertIn("LGPL-2.1-or-later", talkback)  # BRLTTY and liblouis
        self.assertIn("GPL-3.0-or-later", espeak)
        self.assertIn("Apache-2.0", espeak)  # the Android parts and libsonic
        self.assertNotIn("GPL-3.0-or-later", talkback)
        for modules in self.licenses.values():
            for _, body in modules.values():
                kinds = listed(body, "license_kinds")
                self.assertTrue(kinds)
                self.assertTrue(all(kind.startswith("SPDX-license-identifier-") for kind in kinds))

    def test_every_copyleft_kind_names_a_full_text(self) -> None:
        for project, modules in self.licenses.items():
            for name, (directory, body) in modules.items():
                kinds = [kind.removeprefix("SPDX-license-identifier-") for kind in listed(body, "license_kinds")]
                texts = listed(body, "license_text")
                if any(kind in COPYLEFT for kind in kinds):
                    self.assertTrue(texts, f"{project} {name}: a copyleft licence without its text")
                for kind in kinds:
                    if kind in TEXTS:
                        self.assertIn(TEXTS[kind][0], texts, f"{project} {name}: {kind} has no licence text")
                for path in texts:
                    shipped = self.files[project].get(str(Path(directory, path)))
                    if shipped is None:
                        self.assertIn(
                            path, UPSTREAM_TEXTS[project],
                            f"{project} {name}: {path} is neither added by the series nor held upstream",
                        )
                        continue
                    for expected, *needles in TEXTS.values():
                        if path == expected:
                            self.assertGreater(len(shipped), 5000, path)
                            for needle in needles:
                                self.assertIn(needle, shipped[:400], path)

    def test_no_prebuilt_import_of_either_application_is_left(self) -> None:
        self.assertFalse((REPOSITORY / "android/lineage-24/overlay/accessibility").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
