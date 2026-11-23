#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The macOS Android launcher's refusal of arguments that set what it sets itself.

UML joins the launcher's arguments with spaces and Android removes quotes, so an
argument that carries a word of `androidboot.serialno=`, the density, the
timeout multiplier or the ADB authorization sets it whether it stands alone, is
quoted or shares an argument with other words. The launcher refuses each of
them before it does anything else, and the identity options are checked in the
same place. The values here are synthetic.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import subprocess
import sys
import unittest


EMULATOR = ""


def launcher(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [EMULATOR, "--help", *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=30, check=False)


class LauncherArgumentTests(unittest.TestCase):
    def refused(self, argument: str, reason: str) -> None:
        result = launcher(argument)
        self.assertNotEqual(result.returncode, 0, f"accepted: {argument!r}")
        self.assertIn(reason, result.stdout)
        self.assertNotIn("TESTONLY123", result.stdout.replace(argument, ""))

    def test_a_serial_number_is_refused_wherever_the_argument_carries_it(self) -> None:
        reason = "serial number follows"
        for argument in ("androidboot.serialno=TESTONLY123",
                         "quiet androidboot.serialno=TESTONLY123",
                         "quiet\tandroidboot.serialno=TESTONLY123",
                         '"androidboot.serialno=TESTONLY123"',
                         "'androidboot.serialno=TESTONLY123'",
                         'androidboot.serialno="TESTONLY123"',
                         'androidboot.ser"ialno=TESTONLY123'):
            with self.subTest(argument):
                self.refused(argument, reason)

    def test_the_other_arguments_the_launcher_sets_are_refused_the_same_way(self) -> None:
        for argument, reason in (
                ("quiet androidboot.hw_timeout_multiplier=3", "timeout multiplier"),
                ('"androidboot.hw_timeout_multiplier=3"', "timeout multiplier"),
                ("quiet androidboot.lcd_density=240", "density"),
                ("a androidboot.kernarium.adb_auth=1", "ADB authorization")):
            with self.subTest(argument):
                self.refused(argument, reason)

    def test_ordinary_arguments_and_the_identity_options_are_not_refused(self) -> None:
        for argument in ("quiet", "--android-serialno=TESTONLY123", "--net-mac=02:aa:bb:cc:dd:ee"):
            with self.subTest(argument):
                self.assertEqual(launcher(argument).returncode, 0, launcher(argument).stdout)

    def test_the_root_option_is_a_bare_flag_given_once(self) -> None:
        self.assertEqual(launcher("--android-root").returncode, 0, launcher("--android-root").stdout)
        result = launcher("--android-root", "--android-root")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("only once", result.stdout)

    def test_the_identity_options_are_checked(self) -> None:
        for arguments, reason in (
                (("--android-identity=relative/identity.json",), "absolute"),
                (("--android-identity=/tmp/a.json", "--android-serialno=TESTONLY123"), "excludes"),
                (("--android-identity=/tmp/a.json", "--net-mac=02:aa:bb:cc:dd:ee"), "excludes"),
                (("--android-serialno=abc",), "6 to 20"),
                (("--android-serialno=TESTONLY123", "--android-serialno=TESTONLY456"), "only once"),
                (("--net-mac=03:aa:bb:cc:dd:ee",), "unicast"),
                (("--net-mac=02:aa:bb:cc:dd:ee", "--net-mac=02:aa:bb:cc:dd:ef"), "once")):
            with self.subTest(arguments):
                result = launcher(*arguments)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(reason, result.stdout)


if __name__ == "__main__":
    EMULATOR = sys.argv.pop(1)
    unittest.main()
