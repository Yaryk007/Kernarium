#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the management console's wake bookkeeping without booting a guest.

A host suspend that queued behind another console request must not put the
guest to sleep after the host's wake. The count of wakes, the stamp a queued
request takes in the interrupt, and the two tests on them are cut out of the
series' own topic and compiled, in C, against the small kernel stand-in in
mconsole-suspend/; driver.cpp plays the console around them. The handlers
that call them are not compiled, so this test also checks in the patch that
the interrupt stamps before it queues, that the suspend claims
suspend_request before it asks whether a wake overtook it and before it
enters the PM core, and that the wake records itself through the helper.
"""
__import__("sys").dont_write_bytecode = True

from pathlib import Path
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
FILE = "arch/um/drivers/mconsole_kern.c"
FIRST = "static struct mc_request *suspend_request;"
HELPER = "static bool mconsole_overtaken("

TAIL = """
void test_note_wake(void)
{
	mconsole_note_wake();
}

bool test_overtaken(const struct mc_request *request)
{
	return mconsole_overtaken(request);
}

struct mc_request **test_suspend_request(void)
{
	return &suspend_request;
}

bool *test_suspend_woken(void)
{
	return &suspend_woken;
}
"""


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True, timeout=60)


def hunks(patch):
    """The new side (context and added lines) of each hunk of FILE in a patch."""
    result = []
    current = None
    in_file = False
    for text in patch.read_text(encoding="utf-8").splitlines():
        if text.startswith("diff --git "):
            in_file = text.endswith(" b/" + FILE)
            current = None
        elif not in_file:
            continue
        elif text.startswith("@@"):
            current = []
            result.append(current)
        elif current is not None and text[:1] in (" ", "+"):
            current.append(text[1:])
    return result


def topic(line):
    """The hunks of the series' topic that adds the wake count, or exit."""
    for series in kernel_series.check(kernel_series.load_line(line)):
        for patch in series.patches:
            found = hunks(patch)
            if any(HELPER in "\n".join(hunk) for hunk in found):
                return found
    raise SystemExit(f"no topic of {line} adds {HELPER.strip()}")


def bookkeeping(found):
    """From suspend_request to the end of mconsole_overtaken(), in one hunk."""
    for hunk in found:
        if FIRST not in hunk or not any(text.startswith(HELPER) for text in hunk):
            continue
        start = hunk.index(FIRST)
        last = next(i for i, text in enumerate(hunk) if text.startswith(HELPER))
        end = next(i for i in range(last, len(hunk)) if hunk[i] == "}")
        return "\n".join(hunk[start:end + 1]) + "\n"
    raise SystemExit("the wake bookkeeping is not one hunk after suspend_request")


def before(lines, first, second):
    """Whether the first line is present and comes before the second."""
    firsts = [i for i, text in enumerate(lines) if first in text]
    seconds = [i for i, text in enumerate(lines) if second in text]
    return bool(firsts) and bool(seconds) and firsts[0] < seconds[0]


def call_order(found):
    lines = [text for hunk in found for text in hunk]
    if not before(lines, "mconsole_stamp_request(&new->request);",
                  "list_add(&new->list, &mc_requests);"):
        raise SystemExit("the interrupt must stamp a request before it queues it")
    if not before(lines, "cmpxchg(&suspend_request, NULL, req)",
                  "mconsole_overtaken(req)"):
        raise SystemExit("a suspend must claim suspend_request before it reads the wakes")
    if not before(lines, "mconsole_overtaken(req)", "error = pm_suspend(PM_SUSPEND_MEM);"):
        raise SystemExit("a suspend must ask whether a wake overtook it before the PM core")
    if not before(lines, "mconsole_note_wake();", "pm_system_wakeup();"):
        raise SystemExit("a wake must record itself before it wakes the PM core")


def main():
    fixtures = Path(__file__).with_name("mconsole-suspend")
    for line in LINES:
        found = topic(line)
        call_order(found)
        text = bookkeeping(found)
        with tempfile.TemporaryDirectory(prefix="uml-mconsole-suspend-") as temporary:
            work = Path(temporary)
            source = work / "mconsole_suspend.c"
            source.write_text('#include "kernel.h"\n' + text + TAIL, encoding="utf-8")
            run("cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}",
                "-c", str(source), "-o", str(work / "mconsole_suspend.o"))
            run("c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", f"-I{fixtures}",
                str(fixtures / "driver.cpp"), str(work / "mconsole_suspend.o"),
                "-o", str(work / "check"))
            run(str(work / "check"))


if __name__ == "__main__":
    main()
