#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build-checker fixtures exercise the flags actually passed to the compiler."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("jit_build", ROOT / "Andarium/scripts/check-ios-jit-build.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class JitBuildTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.build = Path(self.scratch.name)
        self.cache = {
            "CMAKE_BUILD_TYPE": "Release", "UML_APPLE_PLATFORM": "ios",
            "UML_KERNEL_PRODUCT": "android-17", "CMAKE_OSX_ARCHITECTURES": "arm64",
        }
        self.entries = [
            {"file": "darwin/cpu/jit_memory/executable_memory.cpp", "arguments": ["clang++", "-O3", "-DUML_JIT_MEMORY_REGION_ROUTE=1"]},
            {"file": "darwin/cpu/arm64/jit/jit_compiler.cpp", "arguments": ["clang++", "-O3"]},
            {"file": "Kernarium/Runtime/KernariumRuntime.mm", "arguments": ["clang++", "-O3", "-DKERNARIUM_RUNTIME_ANDROID_GUEST=1"]},
        ]

    def check(self):
        (self.build / "CMakeCache.txt").write_text("\n".join(f"{k}:STRING={v}" for k, v in self.cache.items()))
        (self.build / "compile_commands.json").write_text(json.dumps(self.entries))
        return checker.check(self.build)

    def test_release_passes(self):
        self.assertEqual(self.check(), [])

    def test_non_product_configurations_fail(self):
        for key, value in [("CMAKE_BUILD_TYPE", "Debug"), ("UML_APPLE_PLATFORM", "ios-simulator"), ("UML_APP_STORE_RUNTIME", "ON"), ("UML_KERNARIUM_AUTOMATION_HARNESS", "ON"), ("UML_ENABLE_IOS_STIKDEBUG_JIT", "ON"), ("CMAKE_OSX_ARCHITECTURES", "x86_64")]:
            with self.subTest(key=key):
                before = self.cache.copy()
                self.cache[key] = value
                self.assertTrue(self.check())
                self.cache = before

    def test_late_unoptimized_flag_fails(self):
        self.entries[1]["arguments"].append("-O0")
        self.assertTrue(self.check())

    def test_reenabled_optimization_passes(self):
        self.entries[1]["arguments"].extend(["-O0", "-O2"])
        self.assertEqual(self.check(), [])

    def test_old_macro_does_not_enable_current_region(self):
        self.entries[0]["arguments"][-1] = "-DUML_ENABLE_IOS_STIKDEBUG_JIT=1"
        self.assertTrue(self.check())

    def test_missing_or_disabled_route_fails(self):
        for flag in ("-UUML_JIT_MEMORY_REGION_ROUTE", "-DUML_JIT_MEMORY_REGION_ROUTE=0"):
            with self.subTest(flag=flag):
                self.entries[0]["arguments"].append(flag)
                self.assertTrue(self.check())
                self.entries[0]["arguments"].pop()

    def test_test_only_and_store_objects_fail(self):
        for flag in ("-DUML_JIT_MEMORY_REGION_TEST=1", "-DUML_APP_STORE_RUNTIME"):
            with self.subTest(flag=flag):
                self.entries[0]["arguments"].append(flag)
                self.assertTrue(self.check())
                self.entries[0]["arguments"].pop()

    def test_missing_jit_compiler_fails(self):
        self.entries.pop(1)
        self.assertTrue(self.check())

    def test_shell_command_and_split_defines(self):
        self.entries[0].pop("arguments")
        self.entries[0]["command"] = 'clang++ -O3 -D UML_JIT_MEMORY_REGION_ROUTE=1 -I "path with spaces"'
        self.assertEqual(self.check(), [])


if __name__ == "__main__":
    unittest.main()
