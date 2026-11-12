#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject a non-device, unoptimized or non-JIT CMake tree before packaging.

Check generated compile commands, not just a requested preset: stale caches
and per-source flags can silently change the binary a release ships.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex
import sys


def check(build: Path) -> list[str]:
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        name, value = line.split("=", 1)
        cache[name.split(":", 1)[0]] = value
    problems = []
    for name, wanted in {
        "CMAKE_BUILD_TYPE": "Release",
        "UML_APPLE_PLATFORM": "ios",
        "UML_KERNEL_PRODUCT": "android-17",
        "CMAKE_OSX_ARCHITECTURES": "arm64",
    }.items():
        if cache.get(name) != wanted:
            problems.append(f"{name} must be {wanted}, got {cache.get(name)!r}")
    for name in ("UML_APP_STORE_RUNTIME", "UML_KERNARIUM_AUTOMATION_HARNESS",
                 "UML_ENABLE_IOS_STIKDEBUG_JIT"):
        if cache.get(name, "OFF").upper() not in ("OFF", "FALSE", "NO", "0", ""):
            problems.append(f"{name} must be off in the development release")
    required = {
        "darwin/cpu/jit_memory/executable_memory.cpp": "UML_JIT_MEMORY_REGION_ROUTE",
        "darwin/cpu/arm64/jit/jit_compiler.cpp": None,
        "Kernarium/Runtime/KernariumRuntime.mm": "KERNARIUM_RUNTIME_ANDROID_GUEST",
    }
    seen = set()
    for entry in json.loads((build / "compile_commands.json").read_text()):
        file = entry["file"].replace("\\", "/")
        source = next((path for path in required if file.endswith("/" + path) or file == path), None)
        if source is None:
            continue
        seen.add(source)
        args = entry.get("arguments") or shlex.split(entry["command"])
        flags = [arg for arg in args if arg.startswith("-O")]
        if not flags or flags[-1] not in ("-O2", "-O3", "-Os", "-Oz"):
            problems.append(f"{source}: optimized compilation required, got {flags}")
        definitions = {}
        i = 0
        while i < len(args):
            arg = args[i]
            if arg in ("-D", "-U"):
                i += 1
                arg += args[i]
            if arg.startswith("-D"):
                name, _, value = arg[2:].partition("=")
                definitions[name] = value or "1"
            elif arg.startswith("-U"):
                definitions.pop(arg[2:], None)
            i += 1
        macro = required[source]
        if macro and definitions.get(macro) != "1":
            problems.append(f"{source}: {macro}=1 is missing")
        if "UML_APP_STORE_RUNTIME" in definitions:
            problems.append(f"{source}: App Store code must not enter a JIT build")
        if definitions.get("UML_JIT_MEMORY_REGION_TEST", "0") != "0":
            problems.append(f"{source}: test-only memory route must not enter a product")
    for source in sorted(required.keys() - seen):
        problems.append(f"missing compilation: {source}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="CMake build directory with exported compile commands")
    options = parser.parse_args()
    try:
        problems = check(options.build)
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        print(f"Cannot verify JIT build: {error}", file=sys.stderr)
        return 1
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("Verified optimized arm64 iOS development runtime and debugger JIT route")
    return 0


if __name__ == "__main__":
    sys.exit(main())
