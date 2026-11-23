#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The iPad apps that are also offered on a Mac carry the Mac's own minimum.

Andarium and Kernarium are enabled for "Mac (Designed for iPad)". For such an
app the App Store takes the Mac's minimum from the processed Info.plist's
LSMinimumSystemVersion when it has one, and otherwise maps MinimumOSVersion to
the Mac's version of the same number, which is 26.0 now that iOS builds at its
own floor. The toolchain's macOS floor is the Mac's minimum, so each plist,
processed as Xcode processes it, must name that floor.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import plistlib
import re
import subprocess
import sys
from pathlib import Path

# The applications Apple offers on a Mac, with their Info.plist and whether
# the build runs it through the C preprocessor first (Andarium's does, for
# its optional features' keys).
APPLICATIONS = {
    "Andarium": ("Andarium/Config/Info.plist", True),
    "Kernarium": ("Kernarium/Info.plist", False),
}
MACOS_FLOOR = re.compile(
    r'STREQUAL "macos"\)\s*\n\s*set\(_uml_apple_platform[^\n]*\n'
    r"\s*set\(_uml_apple_deployment_target ([0-9]+(?:\.[0-9]+)*)\)"
)


def fail(message: str) -> None:
    sys.exit(f"test_ipad_apps_mac_minimum.py: {message}")


def macos_floor(root: Path) -> str:
    match = MACOS_FLOOR.search(
        (root / "cmake/toolchains/Apple.cmake").read_text(encoding="utf-8")
    )
    if match is None:
        fail("cmake/toolchains/Apple.cmake names no macOS deployment floor")
    return match.group(1)


def processed(root: Path, relative: str, preprocess: bool) -> dict:
    """The plist as the build produces it: preprocessed with every optional
    feature's key switched on where the build preprocesses."""
    path = root / relative
    if not preprocess:
        return plistlib.loads(path.read_bytes())
    definitions = sorted(
        set(re.findall(r"^#ifdef (\w+)$", path.read_text(encoding="utf-8"), re.M))
    )
    result = subprocess.run(
        [
            "xcrun", "clang", "-E", "-P", "-x", "c", "-traditional", "-undef",
            *(f"-D{name}" for name in definitions), str(path),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        fail(f"{relative} does not preprocess:\n{result.stderr}")
    return plistlib.loads(result.stdout.encode("utf-8"))


def mac_minimum_problem(info: dict, floor: str) -> str | None:
    """Why `info` leaves a Mac's minimum below `floor`, or None."""
    named = info.get("LSMinimumSystemVersion")
    if named is None:
        return "has no LSMinimumSystemVersion, so the Mac's minimum follows MinimumOSVersion"
    if named != floor:
        return f"names LSMinimumSystemVersion {named}, not the macOS floor {floor}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", type=Path, required=True)
    root = parser.parse_args().repository.resolve()
    floor = macos_floor(root)

    # The check itself: a plist with no minimum, or a lower one, is a problem.
    require_problems = {
        "none": ({}, True),
        "lower": ({"LSMinimumSystemVersion": "26.0"}, True),
        "floor": ({"LSMinimumSystemVersion": floor}, False),
    }
    for label, (info, problem) in require_problems.items():
        if (mac_minimum_problem(info, floor) is not None) != problem:
            fail(f"the check judges a plist with {label} wrongly")

    for name, (relative, preprocess) in APPLICATIONS.items():
        problem = mac_minimum_problem(processed(root, relative, preprocess), floor)
        if problem is not None:
            fail(f"{name}'s {relative} {problem}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
