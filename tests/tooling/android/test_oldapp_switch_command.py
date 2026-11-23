#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Old app compatibility shell command's logic on a plain JVM.

android/lineage-24/overlay/oldapp_switch is a Java tool that the image runs through app_process as the
adb shell user. Its rule (which applications are eligible), listing format, writes, order of the stop and
exit statuses do not touch the platform: OldAppCommandTest.java drives them against a fake platform. The
sources of the tool compile here without the Android glue (AndroidPlatform and Main, which need the
framework); the Android calls are compiled by the build host's request and exercised by the runner on an
image. Skipped (status 77) where the host has no JDK.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
SOURCES = REPOSITORY / "android" / "lineage-24" / "overlay" / "oldapp_switch" / "src"
TEST = HERE / "oldapp_switch" / "com" / "kernarium" / "oldapp" / "OldAppCommandTest.java"
ANDROID_GLUE = ("AndroidPlatform.java", "Main.java")
SKIP = 77


def jdk_tool(name: str) -> str | None:
    """A working javac or java: the macOS /usr/bin stubs exist without a JDK, so ask it."""
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def main() -> int:
    javac, java = jdk_tool("javac"), jdk_tool("java")
    if javac is None or java is None:
        print("SKIP: no JDK (javac and java) on PATH")
        return SKIP
    sources = sorted(path for path in SOURCES.rglob("*.java") if path.name not in ANDROID_GLUE)
    expected = {"Candidate.java", "LockFile.java", "OldAppCommand.java", "OldAppEligibility.java", "PropertyListing.java"}
    if {path.name for path in sources} != expected:
        print(f"FAIL: the platform-free sources are {sorted(path.name for path in sources)}, not {sorted(expected)}",
              file=sys.stderr)
        return 1
    for name in ANDROID_GLUE:
        if not (SOURCES / "com" / "kernarium" / "oldapp" / name).is_file():
            print(f"FAIL: {name} is missing", file=sys.stderr)
            return 1
    with tempfile.TemporaryDirectory(prefix="uml-oldapp-switch-") as scratch:
        classes = pathlib.Path(scratch) / "classes"
        classes.mkdir()
        compiled = subprocess.run(
            [javac, "-Xlint:all", "-Werror", "-encoding", "UTF-8", "-d", str(classes),
             *(str(path) for path in sources), str(TEST)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240,
            check=False)
        if compiled.returncode != 0:
            print(f"FAIL: javac:\n{compiled.stdout}", file=sys.stderr)
            return 1
        ran = subprocess.run([java, "-Dfile.encoding=UTF-8", "-cp", str(classes), "com.kernarium.oldapp.OldAppCommandTest"],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             timeout=120, check=False)
        print(ran.stdout, end="")
        if ran.returncode != 0 or not ran.stdout.rstrip().endswith("PASS"):
            print("FAIL: the command's test", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
