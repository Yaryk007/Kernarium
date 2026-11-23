#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Configure scratch trees through the Apple toolchain at each deployment target.

Every Apple platform builds at the one floor the toolchain names for it: macOS
at 27.0, iOS and the iOS Simulator at 26.0. A tree whose CMake cache or command
line holds another target, another platform's floor included, must fail to
configure rather than build at it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# The deployment floor of each platform. iOS and the iOS Simulator share one.
FLOORS = {"macos": "27.0", "ios": "26.0", "ios-simulator": "26.0"}
PLATFORMS = tuple(FLOORS)
CACHE_ENTRY = re.compile(r"^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]+=(.*)$", re.MULTILINE)


def require(condition: bool, message: str) -> None:
    if not condition:
        sys.exit(f"test_apple_deployment_floor.py: {message}")


def write_source(root: Path, languages: str) -> Path:
    source = root / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.27)\n"
        f"project(uml_deployment_floor_fixture LANGUAGES {languages})\n",
        encoding="utf-8",
    )
    return source


def configure(
    cmake: Path, toolchain: Path, source: Path, build: Path, platform: str, *extra: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(cmake),
            "-S",
            str(source),
            "-B",
            str(build),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            f"-DUML_APPLE_PLATFORM={platform}",
            *extra,
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def cached_target(build: Path) -> str:
    match = CACHE_ENTRY.search((build / "CMakeCache.txt").read_text(encoding="utf-8"))
    require(match is not None, f"{build} caches no CMAKE_OSX_DEPLOYMENT_TARGET")
    return match.group(1)


def below(floor: str) -> str:
    return f"{int(floor.split('.')[0]) - 1}.0"


def above(floor: str) -> str:
    return f"{int(floor.split('.')[0]) + 1}.0"


def require_refusal(
    result: subprocess.CompletedProcess[str], label: str, stale: str, floor: str
) -> None:
    text = re.sub(r"\s+", " ", result.stdout)
    require(result.returncode != 0, f"{label} configured at {stale}:\n{result.stdout}")
    for needle in (
        f"CMAKE_OSX_DEPLOYMENT_TARGET={stale}",
        f"floor {floor}",
        "--fresh",
        "binary directory",
    ):
        require(needle in text, f"{label} refusal lacks {needle!r}:\n{result.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    options = parser.parse_args()
    toolchain = options.toolchain.resolve()

    with tempfile.TemporaryDirectory(prefix="uml-deployment-floor.") as scratch:
        root = Path(scratch)
        bare = write_source(root, "NONE")

        # The toolchain's own default is each platform's floor. The floors a
        # tree must refuse are the ones one major version below and above it.
        for platform in PLATFORMS:
            build = root / f"{platform}-probe"
            result = configure(options.cmake, toolchain, bare, build, platform)
            require(result.returncode == 0, f"{platform}: {result.stdout}")
            require(
                cached_target(build) == FLOORS[platform],
                f"{platform} defaults to {cached_target(build)}, not "
                f"{FLOORS[platform]}",
            )

        # The floor configures on every platform and a second configure of the
        # same tree stays at it. An empty value is no target and takes the floor.
        for platform in PLATFORMS:
            floor = FLOORS[platform]
            for label, extra in (
                ("default", ()),
                ("explicit", (f"-DCMAKE_OSX_DEPLOYMENT_TARGET={floor}",)),
                ("empty", ("-DCMAKE_OSX_DEPLOYMENT_TARGET=",)),
            ):
                build = root / f"{platform}-{label}"
                for attempt, arguments in (("first", extra), ("second", ())):
                    result = configure(
                        options.cmake, toolchain, bare, build, platform, *arguments
                    )
                    require(
                        result.returncode == 0,
                        f"{platform} {label} {attempt} configure failed:\n"
                        f"{result.stdout}",
                    )
                    require(
                        cached_target(build) == floor,
                        f"{platform} {label} caches {cached_target(build)}",
                    )

        # A tree configured before the floor moved caches the old target. The
        # toolchain refuses it, without rewriting the cache, and both a new
        # binary directory and --fresh recover.
        for platform in PLATFORMS:
            floor = FLOORS[platform]
            stale = below(floor)
            build = root / f"{platform}-stale"
            result = configure(options.cmake, toolchain, bare, build, platform)
            require(result.returncode == 0, f"{platform}: {result.stdout}")
            cache = build / "CMakeCache.txt"
            cache.write_text(
                CACHE_ENTRY.sub(
                    f"CMAKE_OSX_DEPLOYMENT_TARGET:STRING={stale}",
                    cache.read_text(encoding="utf-8"),
                ),
                encoding="utf-8",
            )
            refused = configure(options.cmake, toolchain, bare, build, platform)
            require_refusal(refused, f"{platform} stale cache", stale, floor)
            require(
                cached_target(build) == stale,
                f"{platform}: the refused configure rewrote the cache",
            )
            recovered = configure(
                options.cmake, toolchain, bare, build, platform, "--fresh"
            )
            require(
                recovered.returncode == 0,
                f"{platform}: --fresh did not recover:\n{recovered.stdout}",
            )
            require(cached_target(build) == floor, f"{platform}: --fresh kept {stale}")

        # Any other target passed on the command line is refused, below the
        # floor or above it. Another platform's floor is another target.
        for platform in PLATFORMS:
            floor = FLOORS[platform]
            for passed in sorted({below(floor), above(floor), *FLOORS.values()} - {floor}):
                result = configure(
                    options.cmake,
                    toolchain,
                    bare,
                    root / f"passed-{platform}-{passed}",
                    platform,
                    f"-DCMAKE_OSX_DEPLOYMENT_TARGET={passed}",
                )
                require_refusal(result, f"{platform} passed {passed}", passed, floor)

        # A project with a compiler runs the toolchain again inside CMake's
        # compiler probes, which must see the floor too.
        for platform in PLATFORMS:
            compiled_root = root / f"compiled-{platform}"
            compiled_root.mkdir()
            result = configure(
                options.cmake,
                toolchain,
                write_source(compiled_root, "C"),
                compiled_root / "build",
                platform,
            )
            require(
                result.returncode == 0,
                f"{platform}: compiled configure failed:\n{result.stdout}",
            )
    return 0


if __name__ == "__main__":
    sys.exit(main())
