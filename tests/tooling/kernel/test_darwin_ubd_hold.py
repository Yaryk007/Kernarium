#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the UBD driver's hold of a request while the host's storage is full.

The two functions that decide and run a held request are cut out of the series'
own topic and compiled, in C, against the small kernel stand-in in ubd-hold/;
driver.cpp scripts the host's answers to them. No guest runs, so the test
neither fills a volume nor depends on the kernel source.
"""
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "kernel/common/tooling"))
import kernel_series  # noqa: E402

LINES = (
    ROOT / "kernel/mainline-6.18/patches/metadata.env",
    ROOT / "kernel/mainline-6.18/slices/x86_64/patches/metadata.env",
)
FIRST = "static bool darwin_ubd_hold("
LAST = "static void darwin_ubd_run("

TAIL = """
void test_run(struct ubd *dev, struct io_thread_req *io_req, char *buffer,
	      unsigned long length, unsigned long byte_offset)
{
	darwin_ubd_run(dev, io_req, buffer, length, byte_offset);
}

void test_set_stopped(bool stopped)
{
	darwin_ubd_stopped = stopped;
}
"""


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True, timeout=60)


def functions(line):
    """The added lines from darwin_ubd_hold() to the end of darwin_ubd_run()."""
    for series in kernel_series.check(kernel_series.load_line(line)):
        for patch in series.patches:
            added = [text[1:] for text in patch.read_text(encoding="utf-8").splitlines()
                     if text.startswith("+") and not text.startswith("+++")]
            start = next((i for i, text in enumerate(added) if text.startswith(FIRST)), None)
            if start is None:
                continue
            last = next(i for i, text in enumerate(added) if text.startswith(LAST))
            end = next(i for i in range(last, len(added)) if added[i] == "}")
            return "\n".join(added[start:end + 1]) + "\n"
    raise SystemExit(f"no topic of {line} adds {FIRST.strip()}")


def main():
    fixtures = Path(__file__).with_name("ubd-hold")
    for line in LINES:
        text = functions(line)
        if not re.search(r"\bBLK_STS_NOSPC\b", text):
            raise SystemExit("the held-request functions no longer name BLK_STS_NOSPC")
        with tempfile.TemporaryDirectory(prefix="uml-ubd-hold-") as temporary:
            work = Path(temporary)
            source = work / "ubd_hold.c"
            source.write_text(
                '#include "kernel.h"\n'
                "static bool darwin_ubd_stopped;\n"
                "static wait_queue_head_t darwin_ubd_hold_wq;\n\n"
                + text + TAIL,
                encoding="utf-8")
            run("cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}",
                "-c", str(source), "-o", str(work / "ubd_hold.o"))
            run("c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}",
                str(fixtures / "driver.cpp"), str(work / "ubd_hold.o"),
                "-o", str(work / "check"))
            run(str(work / "check"))


if __name__ == "__main__":
    main()
