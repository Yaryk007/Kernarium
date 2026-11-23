#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Vulkan smoke runner's reading of what the image's check prints.

run_android_vulkan_smoke.py runs kernarium_vk_ahb_import in an Android guest and
reads its lines. The tool and the runner change together: the lines below are
rendered from the tool's own printf formats (taken from its source), so a format
the runner no longer reads fails here, and every way a run can be wrong - an
extension the driver does not offer, a handle type that does not share, a case
without a verdict, a last line that disagrees with the lines - is a failure of
the runner and not a pass.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import re
import sys
import unittest

TESTS = Path(__file__).resolve().parents[2]
ROOT = TESTS.parent
TOOL = ROOT / "android/lineage-24/overlay/tools/kernarium_vk_ahb_import.cpp"
SCRIPT = TESTS / "integration/android/run_android_vulkan_smoke.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("run_android_vulkan_smoke", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

EXTENSION = "memory-fd extension %s: %s\n"
FD_PROPERTIES = "memory-fd %s properties\n"
FD_SHARED = "memory-fd %s %llu bytes\n"
FD_ANOTHER_INSTANCE = "memory-fd %s %llu bytes, imported by another instance\n"
FD_FREED_EXPORTER = "memory-fd %s %llu bytes, imported after the exporter was freed\n"
FD_OUTLIVING_DEVICES = "memory-fd %s %llu bytes, kept past both devices\n"
HARDWARE_VERDICT = "hardware-buffer verdict: %s: %d cases imported, %d failed, %d not allocated\n"
VERDICT = "  %s\n"
HARDWARE = "%-14s %4ux%-3u stride %u (%u bytes)\n"
SKIPPED = "%-14s %4ux%-3u SKIP: the buffer cannot be allocated\n"
FINAL = "%s: %d cases imported, %d failed, %d not allocated; memory fd %d cases, %d failed\n"


def c_formats(source: str) -> list[str]:
    """The format string of every printf call of the source, its literals joined and PRIu64 as llu."""
    formats = []
    token = re.compile(r'\s*(?:"((?:[^"\\]|\\.)*)"|(PRIu64))')
    for call in re.finditer(r"\bprintf\(", source):
        position = call.end()
        pieces = []
        while True:
            match = token.match(source, position)
            if not match:
                break
            pieces.append(match.group(1) if match.group(1) is not None else "llu")
            position = match.end()
        if pieces:
            formats.append("".join(pieces).replace("\\n", "\n").replace('\\"', '"'))
    return formats


def render(fmt: str, *values: object) -> str:
    """A C format of the tool as Python's % does it."""
    return re.sub(r"%(-?\d*)(?:zu|llu|u)", r"%\1d", fmt) % values


class SmokeOutput:
    """A run of the tool, built from its own formats."""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def extensions(self, fd: bool = True, dma_buf: bool = True, verdict: str | None = None) -> "SmokeOutput":
        self.lines.append(render(EXTENSION, "VK_KHR_external_memory_fd", "offered" if fd else "not offered"))
        self.lines.append(render(EXTENSION, "VK_EXT_external_memory_dma_buf", "offered" if dma_buf else "not offered"))
        self.lines.append(render(VERDICT, verdict or ("PASS" if fd and dma_buf else "FAIL")))
        return self

    def fd(self, kind: str, what: str | int, verdict: str = "PASS", notes: tuple[str, ...] = (),
           variant: str = "") -> "SmokeOutput":
        """A memory-fd case; `variant` is "", "another-instance", "freed-exporter" or "outliving-devices"."""
        if what == "properties":
            self.lines.append(render(FD_PROPERTIES, kind))
        else:
            formats = {"": FD_SHARED, "another-instance": FD_ANOTHER_INSTANCE, "freed-exporter": FD_FREED_EXPORTER,
                       "outliving-devices": FD_OUTLIVING_DEVICES}
            self.lines.append(render(formats[variant], kind, what))
        self.lines.extend("    " + note + "\n" for note in notes)
        self.lines.append(render(VERDICT, verdict))
        return self

    def hardware(self, name: str, width: int, verdict: str = "PASS") -> "SmokeOutput":
        self.lines.append(render(HARDWARE, name, width, 48, width, width * 4))
        self.lines.append(render(VERDICT, verdict))
        return self

    def skipped(self, name: str, width: int) -> "SmokeOutput":
        self.lines.append(render(SKIPPED, name, width, 48))
        return self

    def hardware_verdict(self, verdict: str, imported: int, failed: int, not_allocated: int) -> "SmokeOutput":
        self.lines.append(render(HARDWARE_VERDICT, verdict, imported, failed, not_allocated))
        return self

    def last(self, verdict: str, imported: int, failed: int, not_allocated: int, fd_cases: int,
             fd_failed: int) -> "SmokeOutput":
        self.lines.append(render(FINAL, verdict, imported, failed, not_allocated, fd_cases, fd_failed))
        return self

    def exit(self, status: int) -> str:
        return "".join(self.lines) + f"{runner.EXIT_MARKER}{status}\n"


VARIANTS = ("", "another-instance", "freed-exporter", "outliving-devices")
# The memory-fd cases of a run that shares both types: the extension case, and per type its properties case and each
# of the two sizes in each of the four variants.
FD_CASES = 1 + 2 * (1 + 2 * len(VARIANTS))


def hardware_first(output: SmokeOutput, verdict: str = "PASS", imported: int = 2, failed: int = 0,
                   not_allocated: int = 1) -> SmokeOutput:
    """The tool's hardware-buffer cases and their verdict line, which it prints before any fd case."""
    output.hardware("R8G8B8A8", 640).hardware("R8G8B8A8", 17).skipped("R8", 1)
    return output.hardware_verdict(verdict, imported, failed, not_allocated)


def fd_cases(output: SmokeOutput, kinds: tuple[str, ...] = runner.FD_TYPES,
             variants: tuple[str, ...] = VARIANTS) -> SmokeOutput:
    for kind in kinds:
        output.fd(kind, "properties", notes=("features 0x6, compatible handle types 0x1, exportable from 0x1",))
        for size in (65540, 4194304):
            for variant in variants:
                output.fd(kind, size, variant=variant)
    return output


def passing_run() -> SmokeOutput:
    output = hardware_first(SmokeOutput()).extensions()
    return fd_cases(output).last("PASS", 2, 0, 1, FD_CASES, 0)


class FormatTests(unittest.TestCase):
    def test_the_runner_reads_the_formats_the_tool_prints(self) -> None:
        formats = c_formats(TOOL.read_text())
        for fmt in (EXTENSION, FD_PROPERTIES, FD_SHARED, FD_ANOTHER_INSTANCE, FD_FREED_EXPORTER, FD_OUTLIVING_DEVICES,
                    HARDWARE_VERDICT, VERDICT, HARDWARE, SKIPPED, FINAL):
            self.assertIn(fmt, formats)

    def test_the_tool_is_linked_for_android_and_installed(self) -> None:
        blueprint = (ROOT / "android/lineage-24/overlay/tools/Android.bp").read_text()
        block = blueprint[blueprint.index('name: "kernarium_vk_ahb_import"'):]
        block = block[:block.index("\n}")]
        for needle in ('"libvulkan"', '"libnativewindow"', "-DVK_USE_PLATFORM_ANDROID_KHR", "-Werror"):
            self.assertIn(needle, block)
        product = (ROOT / "android/lineage-24/overlay/lineage_andarium.mk").read_text()
        self.assertEqual(len(re.findall(r"^PRODUCT_PACKAGES \+= kernarium_vk_ahb_import$", product, re.M)), 1)


class ParseTests(unittest.TestCase):
    def test_a_passing_run_is_a_proof(self) -> None:
        report = runner.parse(passing_run().exit(0))
        self.assertEqual(runner.evaluate(report), [])
        self.assertEqual(report.extensions, {"VK_KHR_external_memory_fd": True,
                                             "VK_EXT_external_memory_dma_buf": True})
        self.assertEqual(report.final, ("PASS", 2, 0, 1, FD_CASES, 0))
        self.assertEqual(report.hardware_verdict, ("PASS", 2, 0, 1))
        self.assertEqual(report.skipped, 1)
        self.assertEqual(len(report.of_kind("fd")), FD_CASES - 1)
        self.assertEqual(len(report.of_kind("hardware")), 2)
        summary = runner.summary(report)
        self.assertIn("memory-fd opaque-fd 65540 bytes: PASS", summary)
        self.assertIn("memory-fd dma-buf 4194304 bytes, kept past both devices: PASS", summary)
        self.assertIn("hardware-buffer verdict line: PASS: 2 imported", summary)

    def test_an_image_without_the_fd_extensions_fails_and_says_which(self) -> None:
        output = SmokeOutput().hardware("R8G8B8A8", 640).hardware_verdict("PASS", 1, 0, 0)
        output.extensions(fd=False, dma_buf=False)
        output.last("FAIL", 1, 0, 0, 1, 1)
        problems = runner.evaluate(runner.parse(output.exit(1)))
        self.assertTrue(any("VK_KHR_external_memory_fd is not offered" in problem for problem in problems), problems)
        self.assertTrue(any("VK_EXT_external_memory_dma_buf is not offered" in problem for problem in problems),
                        problems)
        self.assertTrue(any("opaque-fd has no passing properties case" in problem for problem in problems), problems)

    def test_a_failed_sharing_case_carries_its_notes(self) -> None:
        output = SmokeOutput().hardware("R8G8B8A8", 640).hardware_verdict("PASS", 1, 0, 0).extensions()
        output.fd("opaque-fd", "properties").fd("opaque-fd", 65540, "FAIL", (
            "the mapping of the fd after the Vulkan mapping wrote: 3 of 65540 bytes differ, first at 0: read 1, wrote 2",))
        output.fd("opaque-fd", 4194304)
        output.fd("dma-buf", "properties").fd("dma-buf", 65540).fd("dma-buf", 4194304)
        output.last("FAIL", 1, 0, 0, 7, 1)
        problems = runner.evaluate(runner.parse(output.exit(1)))
        self.assertTrue(any("fd opaque-fd 65540 bytes FAIL: the mapping of the fd" in problem for problem in problems),
                        problems)

    def test_a_failed_case_of_another_instance_carries_its_notes(self) -> None:
        output = hardware_first(SmokeOutput()).extensions()
        output.fd("opaque-fd", "properties")
        output.fd("opaque-fd", 65540, "FAIL", ("the other instance was killed by signal 11",),
                  variant="freed-exporter")
        problems = runner.evaluate(runner.parse(output.last("FAIL", 2, 0, 1, 3, 1).exit(1)))
        self.assertTrue(any("fd opaque-fd 65540 bytes, imported after the exporter was freed FAIL: the other "
                            "instance was killed by signal 11" in problem for problem in problems), problems)

    def test_a_type_with_only_failing_sharing_cases_has_no_proof(self) -> None:
        output = hardware_first(SmokeOutput()).extensions()
        fd_cases(output, kinds=("opaque-fd",))
        for kind in runner.FD_TYPES[1:]:
            output.fd(kind, "properties")
        output.last("PASS", 2, 0, 1, FD_CASES - 8, 0)
        problems = runner.evaluate(runner.parse(output.exit(0)))
        self.assertEqual(problems, ["dma-buf has no passing sharing case",
                                    "dma-buf has no passing another-instance case",
                                    "dma-buf has no passing freed-exporter case",
                                    "dma-buf has no passing outliving-devices case"])

    def test_each_kind_of_sharing_is_required_for_each_type(self) -> None:
        for dropped in VARIANTS[1:]:
            with self.subTest(dropped=dropped):
                output = hardware_first(SmokeOutput()).extensions()
                fd_cases(output, kinds=("opaque-fd",))
                fd_cases(output, kinds=("dma-buf",), variants=tuple(v for v in VARIANTS if v != dropped))
                output.last("PASS", 2, 0, 1, FD_CASES - 2, 0)
                problems = runner.evaluate(runner.parse(output.exit(0)))
                self.assertEqual(problems, [f"dma-buf has no passing {dropped} case"])

    def test_a_hardware_buffer_that_fails_fails_the_run(self) -> None:
        output = SmokeOutput().hardware("R8G8B8A8", 640, "FAIL").hardware_verdict("FAIL", 1, 1, 0).extensions()
        fd_cases(output).last("FAIL", 1, 1, 0, FD_CASES, 0)
        problems = runner.evaluate(runner.parse(output.exit(1)))
        self.assertTrue(any("hardware R8G8B8A8 640x48 FAIL" in problem for problem in problems), problems)

    def test_no_hardware_buffer_case_is_not_a_proof(self) -> None:
        output = SmokeOutput().skipped("R8", 1).hardware_verdict("PASS", 0, 0, 1).extensions()
        fd_cases(output).last("PASS", 0, 0, 1, FD_CASES, 0)
        self.assertIn("no hardware-buffer case imported", runner.evaluate(runner.parse(output.exit(0))))

    def test_the_exit_status_and_the_last_line_must_agree(self) -> None:
        problems = runner.evaluate(runner.parse(passing_run().exit(1)))
        self.assertTrue(any("exited 1 but its last line says PASS" in problem for problem in problems), problems)
        text = "".join(passing_run().lines)
        self.assertIn("the exit status of the tool was not read", runner.evaluate(runner.parse(text)))

    def test_the_counts_of_the_last_line_must_match_the_lines(self) -> None:
        output = passing_run()
        output.lines[-1] = render(FINAL, "PASS", 2, 0, 1, FD_CASES + 1, 0)
        problems = runner.evaluate(runner.parse(output.exit(0)))
        self.assertTrue(any(f"counts {FD_CASES + 1} memory-fd cases" in problem for problem in problems), problems)
        output = passing_run()
        output.lines[-1] = render(FINAL, "PASS", 3, 0, 1, FD_CASES, 0)
        problems = runner.evaluate(runner.parse(output.exit(0)))
        self.assertIn("the hardware-buffer cases do not match the last line", problems)
        self.assertIn("the hardware-buffer verdict line does not match the last line", problems)

    def test_a_tool_that_did_not_finish_has_no_proof(self) -> None:
        text = "".join(passing_run().lines[:-1])
        self.assertEqual(runner.evaluate(runner.parse(text + f"{runner.EXIT_MARKER}137\n")),
                         ["the tool printed no last line (it did not finish)"])

    def test_a_crash_in_the_fd_cases_leaves_the_hardware_verdict_in_the_report(self) -> None:
        # The tool prints the hardware-buffer verdict before the fd cases, so a crash in them (the process dies with no
        # last line) leaves the verdict to read.
        lines = hardware_first(SmokeOutput(), failed=0).lines
        lines.append(render(EXTENSION, "VK_KHR_external_memory_fd", "offered"))
        lines.append(render(EXTENSION, "VK_EXT_external_memory_dma_buf", "offered"))
        lines.append(render(VERDICT, "PASS"))
        lines.append(render(FD_PROPERTIES, "opaque-fd"))
        lines.append("    features 0x6, compatible handle types 0x1, exportable from 0x1\n")
        # The properties case has no verdict: the tool was killed there.
        with self.assertRaises(runner.Failure):
            runner.parse("".join(lines) + f"{runner.EXIT_MARKER}139\n")
        lines.append(render(VERDICT, "PASS"))
        report = runner.parse("".join(lines) + f"{runner.EXIT_MARKER}139\n")
        self.assertEqual(report.hardware_verdict, ("PASS", 2, 0, 1))
        self.assertEqual(runner.evaluate(report), ["the tool printed no last line (it did not finish)"])
        self.assertIn("hardware-buffer verdict line: PASS: 2 imported", runner.summary(report))

    def test_the_hardware_verdict_comes_before_the_fd_cases(self) -> None:
        output = SmokeOutput().extensions()
        hardware_first(output)
        with self.assertRaises(runner.Failure):
            runner.parse("".join(output.lines))
        output = SmokeOutput().hardware("R8G8B8A8", 640).extensions()
        with self.assertRaises(runner.Failure):
            # A hardware-buffer line after the fd cases began.
            runner.parse("".join(output.lines) + "".join(SmokeOutput().hardware("R8", 1).lines))
        output = hardware_first(SmokeOutput())
        output.hardware("R8", 1)
        with self.assertRaises(runner.Failure):
            runner.parse("".join(output.lines))
        output = hardware_first(SmokeOutput())
        output.hardware_verdict("PASS", 2, 0, 1)
        with self.assertRaises(runner.Failure):
            runner.parse("".join(output.lines))

    def test_a_run_without_the_hardware_verdict_line_is_not_a_proof(self) -> None:
        lines = passing_run().lines
        index = next(i for i, line in enumerate(lines) if line.startswith("hardware-buffer verdict"))
        del lines[index]
        problems = runner.evaluate(runner.parse("".join(lines) + f"{runner.EXIT_MARKER}0\n"))
        self.assertEqual(problems, ["the tool printed no hardware-buffer verdict line before the fd cases"])

    def test_a_case_without_a_verdict_is_refused(self) -> None:
        lines = passing_run().lines
        index = next(i for i, line in enumerate(lines) if line.startswith("memory-fd opaque-fd 65540"))
        del lines[index + 1]
        with self.assertRaises(runner.Failure):
            runner.parse("".join(lines) + f"{runner.EXIT_MARKER}0\n")

    def test_a_line_the_tool_does_not_print_is_refused(self) -> None:
        lines = passing_run().lines
        lines.insert(3, "something unexpected\n")
        with self.assertRaises(runner.Failure):
            runner.parse("".join(lines) + f"{runner.EXIT_MARKER}0\n")
        with self.assertRaises(runner.Failure):
            runner.parse("  PASS\n")
        with self.assertRaises(runner.Failure):
            runner.parse("".join(passing_run().lines) + "trailing\n")


if __name__ == "__main__":
    unittest.main()
