#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium's developer-owned local settings are ignored, whatever their name.

A checkout that built an earlier Andarium holds ignored `*.local.xcconfig` files
beside the tracked configuration, with values that must never reach the history.
The rule has to keep them ignored: the names an earlier build read, the one the
build reads now and a name nothing reads yet, while the tracked `.example` files
stay visible to Git. The check runs on a scratch repository that holds this
tree's .gitignore and asks `git check-ignore --no-index` about names alone; it
reads no file of the configuration directory and lists only the examples.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(".")

# Names an earlier build read, the name the build reads now, and one that
# nothing reads.
IGNORED = (
    "Andarium/Config/Feedback.local.xcconfig",
    "Andarium/Config/TipJar.local.xcconfig",
    "Andarium/Config/Review.local.xcconfig",
    "Andarium/Config/Developer.local.xcconfig",
    "Andarium/Config/Arbitrary.local.xcconfig",
    "GuestHost/Config/Signing.local.xcconfig",
    "Andarium/Config/Local/x-logo.svg",
)


class LocalConfigIgnore(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.repo = Path(scratch.name)
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        shutil.copy(ROOT / ".gitignore", self.repo / ".gitignore")

    def ignored(self, name: str) -> bool:
        result = subprocess.run(
            ["git", "-C", str(self.repo), "check-ignore", "--no-index", "-q", "--", name],
            check=False,
        )
        self.assertIn(result.returncode, (0, 1), name)
        return result.returncode == 0

    def test_the_local_files_are_ignored(self) -> None:
        for name in IGNORED:
            with self.subTest(name=name):
                self.assertTrue(self.ignored(name), f"{name} is not ignored")

    def test_the_tracked_examples_are_not_ignored(self) -> None:
        examples = sorted(
            [str(p.relative_to(ROOT)) for p in (ROOT / "Andarium/Config").glob("*.local.xcconfig.example")]
            + [str(p.relative_to(ROOT)) for p in (ROOT / "GuestHost/Config").glob("*.local.xcconfig.example")]
        )
        self.assertTrue(examples, "no tracked example found")
        for name in examples:
            with self.subTest(name=name):
                self.assertFalse(self.ignored(name), f"{name} is ignored")

    def test_the_tracked_configuration_is_not_ignored(self) -> None:
        for name in ("Andarium/Config/Andarium.xcconfig", "Andarium/Config/Info.plist"):
            with self.subTest(name=name):
                self.assertFalse(self.ignored(name), f"{name} is ignored")


def main() -> int:
    global ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    args, rest = parser.parse_known_args()
    ROOT = args.root
    return 0 if unittest.main(argv=[sys.argv[0], *rest], exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
