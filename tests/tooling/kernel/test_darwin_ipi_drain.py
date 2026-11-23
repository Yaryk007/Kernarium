#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the Darwin port's idle wait and IPI code each kernel ships.

The idle wait (arch/um/os-Darwin/time.c) and the IPI code (smp.c) are built
from the series' own patches, in order, so a later topic that changes them is
tested too. The driver runs them on one thread as CPU0 and counts the host
calls an idle cycle makes: a wake by the clock must not look at the IPI wake
pipe unless a vector is pending, no cycle drains it twice, and an IPI sent
before, during or after the wait must be handled without a lost wake and with
at most one spurious cycle. A wait whose dispatch before it ran the handlers
of an edge must not sleep, for a handler may have made a task runnable here.
"""
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "kernel/common/tooling"))
import kernel_series  # noqa: E402

# The sources, and the private headers they include. Several topics change them.
FILES = (
    "arch/um/os-Darwin/time.c",
    "arch/um/os-Darwin/smp.c",
    "arch/um/os-Darwin/internal.h",
    "arch/um/os-Darwin/irq.h",
)
# Kernel headers the two files include and the fixture's os.h stands in for.
KERNEL_HEADERS = (
    "as-layout.h", "irq_user.h", "kern_util.h", "longjmp.h", "skas.h", "smp.h",
    "bridge.h", "sysdep/archsetjmp.h",
)
LINES = (
    ROOT / "kernel/mainline-6.18/patches/metadata.env",
    ROOT / "kernel/mainline-6.18/slices/x86_64/patches/metadata.env",
)


def run(*args, cwd=None, timeout=60):
    subprocess.run(args, cwd=cwd, check=True, timeout=timeout)


def materialize(work, line):
    """Write the files as the line's series leave them, as the IRQ test does."""
    include = [f"--include={name}" for name in FILES]
    for series in kernel_series.check(kernel_series.load_line(line)):
        for patch in series.patches:
            if any(name.encode() in patch.read_bytes() for name in FILES):
                run("git", "apply", *include, str(patch), cwd=work)


def main():
    if platform.system() != "Darwin":
        print("Darwin idle wait regression is only applicable on macOS")
        return
    fixtures = Path(__file__).with_name("ipi-drain")
    for line in LINES:
        with tempfile.TemporaryDirectory(prefix="uml-ipi-drain-") as temporary:
            work = Path(temporary)
            materialize(work, line)
            include = work / "include"
            for name in KERNEL_HEADERS:
                (include / name).parent.mkdir(parents=True, exist_ok=True)
                (include / name).write_text("#include <os.h>\n")
            sources = work / "arch/um/os-Darwin"
            objects = []
            for name in ("time", "smp"):
                objects.append(str(work / f"{name}.o"))
                run("cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}", f"-I{include}",
                    f"-I{sources}", "-c", str(sources / f"{name}.c"), "-o", objects[-1])
            run("c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{sources}", str(fixtures / "driver.cpp"), *objects,
                "-o", str(work / "check"))
            run(str(work / "check"))


if __name__ == "__main__":
    main()
