#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The application's own touch feedback goes through one modifier.

A launch that silences haptics (`ANDARIUM_HAPTICS=0`, Andarium/README.md
#automation) must make the phone play nothing of its own, and automated taps
and drags reach the side keys and the rulers. Those play their feedback
through `interfaceFeedback`, which asks the model, so a site that calls
SwiftUI's `sensoryFeedback`, or UIKit's feedback generators, directly would
sidestep the launch. This reads the application's sources for each such call:
it guards that no site goes around the modifier, and does not show that the
modifier is silent (the hosted LaunchEnvironmentTests and
CapabilitiesFlowTests do).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path
import re
import sys
import unittest

SOURCES = ("Andarium/Andarium", "GuestHost/Sources", "Kernarium")
# The one file that calls SwiftUI's modifier.
MODIFIER = "Andarium/Andarium/Shared/InterfaceFeedback.swift"
# Feedback a view plays on its own, which the launch's setting must reach.
DIRECT = re.compile(
    r"\.sensoryFeedback\(|UI(?:Impact|Selection|Notification)FeedbackGenerator"
    r"|UIFeedbackGenerator|AudioServicesPlay(?:System|Alert)Sound"
    r"|kSystemSoundID_Vibrate")
COMMENT = re.compile(r"//.*$", re.MULTILINE)
INTERFACE_CALL = re.compile(r"\.interfaceFeedback\(")
# The sites that exist: each plays through the modifier.
SITES = {
    "Andarium/Andarium/Shared/SideKeys.swift": 3,
    "Andarium/Andarium/Shared/Ruler.swift": 1,
}

ROOT = Path(__file__).resolve().parents[3]


def swift_sources(root: Path):
    for directory in SOURCES:
        yield from sorted((root / directory).rglob("*.swift"))


def code(path: Path) -> str:
    return COMMENT.sub("", path.read_text(encoding="utf-8"))


class InterfaceFeedbackSources(unittest.TestCase):
    root = ROOT

    def test_no_view_plays_touch_feedback_around_the_modifier(self) -> None:
        found = [
            f"{path.relative_to(self.root)}: {match.group(0)}"
            for path in swift_sources(self.root)
            if path.relative_to(self.root).as_posix() != MODIFIER
            for match in DIRECT.finditer(code(path))
        ]
        self.assertEqual(found, [])

    def test_the_modifier_asks_the_model_whether_the_launch_silences_it(
        self
    ) -> None:
        source = code(self.root / MODIFIER)
        self.assertIn(".sensoryFeedback(", source)
        self.assertIn("interfaceFeedbackPlays", source)

    def test_the_known_sites_play_through_the_modifier(self) -> None:
        for relative, count in SITES.items():
            with self.subTest(file=relative):
                self.assertEqual(
                    len(INTERFACE_CALL.findall(code(self.root / relative))),
                    count)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    InterfaceFeedbackSources.root = arguments.root.resolve()
    unittest.main(argv=[sys.argv[0], *rest])
