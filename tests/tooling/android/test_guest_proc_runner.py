#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The guest /proc runner's reading of the command Andarium sends and of its output.

The command is written once, in AndariumCore's GuestProcFiles.swift, between two
markers. The runner takes it from there, so what it runs in an Android guest is
what the application sends. Here the command runs on the host's shell, with
`/proc` swapped for a directory of files and a stub `su` first in PATH (the
real `su` is never run), to check that it prints a section for every file and
one outcome for each, and that the runner reads those sections.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_guest_proc.py"
SPEC = importlib.util.spec_from_file_location("run_android_guest_proc", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

STUB_SU = """#!/bin/sh
for last; do :; done
[ -e "$last" ] || { echo "su: $last: not found" >&2; exit 1; }
echo "read-by-the-stub-su"
"""


class GuestProcCommandTests(unittest.TestCase):
    def test_the_command_is_the_literal_between_the_markers(self) -> None:
        command = runner.command_from_source()
        self.assertTrue(command.startswith("for entry in /proc/timer_list:"))
        self.assertTrue(command.rstrip().endswith("done"))
        self.assertNotIn('"""', command)
        paths = [path for path, _limit in runner.sources_of(command)]
        self.assertEqual(paths[:3], ["/proc/timer_list", "/proc/interrupts", "/proc/stat"])
        self.assertIn("/proc/pressure/memory", paths)

    def test_the_command_defines_no_function_and_no_word_a_shell_aliases(self) -> None:
        command = runner.command_from_source()
        # Android's mksh aliases r (fc -e -), which a function of that name lost.
        self.assertNotRegex(command, r"(?m)^\s*\w+\s*\(\)")
        self.assertNotRegex(command, r"(?m)^\s*(r|history|autoload|functions|integer|local|hash)\b")

    def test_the_sections_are_read_with_their_outcomes(self) -> None:
        sources = [("/proc/a", 10), ("/proc/b", 10), ("/proc/c", 10)]
        output = ("==> /proc/a <==\nbody a\n(read as the shell user)\n"
                  "==> /proc/b <==\nroot body(read as root through su)\n"
                  "==> /proc/c <==\n(not readable: denied; through su: not found)\n")
        sections = runner.parse_sections(output, sources)
        self.assertEqual(sections["/proc/a"], ("body a\n", "(read as the shell user)"))
        self.assertEqual(sections["/proc/b"][1], "(read as root through su)")
        self.assertTrue(sections["/proc/c"][1].startswith("(not readable: denied"))
        with self.assertRaises(runner.Failure):
            runner.parse_sections("==> /proc/a <==\nbody with no outcome\n", sources)

    def test_the_command_on_a_shell_prints_a_section_and_an_outcome_for_every_file(self) -> None:
        command = runner.command_from_source()
        with tempfile.TemporaryDirectory(prefix="guest-proc-runner-") as name:
            root = Path(name)
            bin_directory = root / "bin"
            bin_directory.mkdir()
            stub = bin_directory / "su"
            stub.write_text(STUB_SU)
            stub.chmod(0o755)
            (root / "timer_list").write_text("t" * 200_000)
            (root / "stat").write_text("cpu 1 2 3\n")
            (root / "stat").chmod(0)
            try:
                swapped = command.replace("/proc/timer_list", str(root / "timer_list")).replace(
                    "/proc/stat", str(root / "stat"))
                result = subprocess.run(
                    ["/bin/sh", "-c", swapped], env={"PATH": f"{bin_directory}:/usr/bin:/bin"},
                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30, check=False)
            finally:
                (root / "stat").chmod(0o644)
        self.assertEqual(result.returncode, 0, result.stderr)
        sources = [(path.replace("/proc/timer_list", str(root / "timer_list")).replace(
            "/proc/stat", str(root / "stat")), limit) for path, limit in runner.sources_of(command)]
        sections = runner.parse_sections(result.stdout, sources)
        self.assertEqual(sorted(sections), sorted(path for path, _limit in sources))
        timer = sections[str(root / "timer_list")]
        self.assertEqual(len(timer[0]), 131_072)
        self.assertEqual(timer[1], "(read as the shell user)")
        self.assertEqual(sections[str(root / "stat")][1], "(read as root through su)")
        self.assertTrue(sections["/proc/interrupts"][1].startswith("(not readable: "))
        self.assertIn("through su: su: /proc/interrupts: not found", sections["/proc/interrupts"][1])


if __name__ == "__main__":
    unittest.main()
