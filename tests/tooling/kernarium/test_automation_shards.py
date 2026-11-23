#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The shards of a unittest module run every test once, and a shard that runs nothing is an error.

The iPad automation watch tests run as several CTest shards (automation_shards.py). The cases run small test
modules as commands: the shards of one module partition its tests, a shard that selects no test fails
whatever the Python version, and arguments that name no shard fail.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def module_text(tests: int, failing: bool = False) -> str:
    cases = "".join(
        f"    def test_{number}(self):\n        self.assertTrue({not (failing and number == 0)})\n"
        for number in range(tests)) or "    pass\n"
    return (f"import sys\nsys.dont_write_bytecode = True\nsys.path.insert(0, {str(HERE)!r})\n"
            "import unittest\nimport automation_shards\n\n\nclass Cases(unittest.TestCase):\n"
            f"{cases}\n\nif __name__ == \"__main__\":\n    automation_shards.main()\n")


class AutomationShards(unittest.TestCase):
    def run_module(self, text: str, *arguments: str) -> tuple[int, list[str], str]:
        """Status, the tests that ran and standard error of a module run as a command."""
        with tempfile.TemporaryDirectory(prefix="automation-shards-") as directory:
            script = Path(directory) / "cases.py"
            script.write_text(text, encoding="utf-8")
            done = subprocess.run(
                [sys.executable, "-B", str(script), *arguments, "-v"], check=False, stdin=subprocess.DEVNULL,
                capture_output=True, text=True, timeout=60)
        ran = re.findall(r"^(test_\d+) \(", done.stderr, re.M)
        return done.returncode, ran, done.stderr

    def test_the_shards_of_a_module_run_each_test_once(self) -> None:
        text = module_text(5)
        self.assertEqual(self.run_module(text)[:2], (0, [f"test_{number}" for number in range(5)]))
        for count, sizes in ((1, [5]), (2, [3, 2]), (3, [2, 2, 1]), (5, [1] * 5)):
            ran: list[str] = []
            for index in range(count):
                status, shard, _ = self.run_module(text, "--shard", f"{index}/{count}")
                self.assertEqual((status, len(shard)), (0, sizes[index]), f"shard {index}/{count}")
                ran += shard
            self.assertEqual(sorted(ran), [f"test_{number}" for number in range(5)], f"{count} shards")

    def test_a_shard_that_selects_no_test_is_an_error(self) -> None:
        for tests, argument in ((1, "1/2"), (5, "5/7"), (5, "6/7"), (0, "0/1"), (0, "0/3")):
            with self.subTest(tests=tests, shard=argument):
                status, ran, error = self.run_module(module_text(tests), "--shard", argument)
                self.assertNotEqual(status, 0, error)
                self.assertEqual(ran, [])
                self.assertIn(f"shard {argument} selects none of the {tests} tests", error)
        self.assertEqual(self.run_module(module_text(1), "--shard", "0/2")[:2], (0, ["test_0"]))

    def test_a_failing_test_fails_its_shard(self) -> None:
        text = module_text(4, failing=True)
        self.assertEqual(self.run_module(text, "--shard", "0/2")[0], 1)
        self.assertEqual(self.run_module(text, "--shard", "1/2")[0], 0)

    def test_arguments_that_name_no_shard_are_errors(self) -> None:
        for arguments in (("--shard",), ("--shard", "x"), ("--shard", "1"), ("--shard", "2/2"),
                          ("--shard", "0/0"), ("--shard", "-1/2"), ("--shard", "1/x")):
            with self.subTest(arguments=arguments):
                status, ran, error = self.run_module(module_text(3), *arguments)
                self.assertNotEqual(status, 0, error)
                self.assertEqual(ran, [])
                self.assertIn("usage: --shard INDEX/COUNT", error)


if __name__ == "__main__":
    unittest.main()
