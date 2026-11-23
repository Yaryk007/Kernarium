#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The old-application runner without a guest: the probe's package, its code, the verdicts and the commands.

run_android_oldapp.py installs the probe application of oldapp_probe_apk.py in two variants and reads what each
one reports. Here the manifest and the DEX file of the package are read back with independent readers, the native
sources are built for armv7 when the host has the tools, and the runner's steps run against a fake guest that
records the commands it is given.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path
from subprocess import CompletedProcess
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SCRIPT = TESTS / "integration/android/run_android_oldapp.py"
SPEC = importlib.util.spec_from_file_location("run_android_oldapp", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their annotations through sys.modules.
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
probe = runner.probe
binary_xml = probe.binary_xml

ACC_STATIC, ACC_NATIVE, ACC_PUBLIC = 0x8, 0x100, 0x1


def uleb(data: bytes, offset: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7


class Dex:
    """A reader of the parts of a DEX file the probe uses, written from the format's description."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        header = struct.unpack_from("<8sI20sIIIIIIIIIIIIIIIIIIII", data, 0)
        (self.magic, self.checksum, self.signature, self.file_size, self.header_size, self.endian, _, _, self.map_off,
         self.string_count, self.string_off, self.type_count, self.type_off, self.proto_count, self.proto_off, _, _,
         self.method_count, self.method_off, self.class_count, self.class_off, self.data_size, self.data_off) = header
        self.strings = [self.string(i) for i in range(self.string_count)]
        self.types = [self.strings[struct.unpack_from("<I", data, self.type_off + 4 * i)[0]]
                      for i in range(self.type_count)]
        self.methods = []
        for index in range(self.method_count):
            owner, proto, name = struct.unpack_from("<HHI", data, self.method_off + 8 * index)
            shorty, return_type, parameters = struct.unpack_from("<III", data, self.proto_off + 12 * proto)
            self.methods.append((self.types[owner], self.strings[name], self.strings[shorty], self.types[return_type]))

    def string(self, index: int) -> str:
        (offset,) = struct.unpack_from("<I", self.data, self.string_off + 4 * index)
        _, offset = uleb(self.data, offset)
        return self.data[offset:self.data.index(b"\0", offset)].decode()

    def class_methods(self) -> dict[str, tuple[int, int]]:
        """name -> (access flags, code offset) over the one class's direct and virtual methods."""
        (_, _, _, _, _, _, class_data, _) = struct.unpack_from("<IIIIIIII", self.data, self.class_off)
        offset = class_data
        counts = []
        for _ in range(4):
            count, offset = uleb(self.data, offset)
            counts.append(count)
        found = {}
        for count in counts[2:]:
            index = 0
            for _ in range(count):
                step, offset = uleb(self.data, offset)
                access, offset = uleb(self.data, offset)
                code, offset = uleb(self.data, offset)
                index += step
                name = self.methods[index][1]
                found[name + "/instance" if name in found else name] = (access, code)  # Runnable.run beside run(String)
        return found

    def interfaces(self) -> list[str]:
        """The interfaces the one class implements."""
        (_, _, _, offset, _, _, _, _) = struct.unpack_from("<IIIIIIII", self.data, self.class_off)
        if offset == 0:
            return []
        (count,) = struct.unpack_from("<I", self.data, offset)
        return [self.types[i] for i in struct.unpack_from(f"<{count}H", self.data, offset + 4)]

    def instructions(self, code_offset: int) -> list[int]:
        registers, ins, outs, tries, debug, size = struct.unpack_from("<HHHHII", self.data, code_offset)
        return list(struct.unpack_from(f"<{size}H", self.data, code_offset + 16))


class ProbePackageTests(unittest.TestCase):
    def test_the_dex_file_is_well_formed(self) -> None:
        data = probe.oldapp_probe_dex.build_dex()
        dex = Dex(data)
        self.assertEqual(dex.magic, b"dex\n035\0")
        self.assertEqual(dex.file_size, len(data))
        self.assertEqual(dex.checksum, zlib.adler32(data[12:]) & 0xFFFFFFFF)
        self.assertEqual(dex.signature, hashlib.sha1(data[32:]).digest())
        self.assertEqual(dex.strings, sorted(dex.strings, key=lambda text: text.encode()))
        self.assertEqual(dex.class_count, 1)
        self.assertEqual(dex.types[struct.unpack_from("<I", data, dex.class_off)[0]],
                         "Lorg/example/andarium/oldappprobe/Probe;")

    def test_the_class_has_the_native_methods_and_the_methods_the_native_code_calls(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        methods = dex.class_methods()
        natives = {"run"} | {name for name, _, _ in probe.oldapp_probe_dex.NATIVES}
        java = {"payload", "echo", "spin", "asset", "peek", "scene", "poke", "same", "collect", "render", "run/instance"}
        self.assertEqual(set(methods), {"<init>", "onReceive"} | java | natives)
        for name in natives:
            self.assertEqual(methods[name][0], ACC_PUBLIC | ACC_STATIC | ACC_NATIVE, name)
            self.assertEqual(methods[name][1], 0, f"{name}: a native method has no code")
        for name in {"<init>", "onReceive"} | java:
            self.assertNotEqual(methods[name][1], 0, name)
        self.assertEqual(methods["run/instance"][0], ACC_PUBLIC, "Runnable.run is an instance method")
        self.assertEqual(methods["onReceive"][0], ACC_PUBLIC)
        self.assertEqual(methods["payload"][0], ACC_PUBLIC | ACC_STATIC)
        owner = "Lorg/example/andarium/oldappprobe/Probe;"
        listed = {(name, shorty, ret) for klass, name, shorty, ret in dex.methods if klass == owner}
        self.assertIn(("payload", "L", "[B"), listed)
        self.assertIn(("echo", "IL", "I"), listed)
        self.assertIn(("run", "LL", "Ljava/lang/String;"), listed)

    def test_echo_returns_the_length_and_payload_returns_the_bytes_of_the_string(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        methods = dex.class_methods()
        # array-length v0, v1; return v0
        self.assertEqual(dex.instructions(methods["echo"][1]), [0x1021, 0x000F])
        payload = dex.instructions(methods["payload"][1])
        self.assertEqual(payload[-1], 0x0011, "return-object v0")
        self.assertIn(probe.oldapp_probe_dex.PAYLOAD, dex.strings)

    def test_the_payload_matches_the_native_probe(self) -> None:
        source = probe.PROBE_SOURCE.read_text()
        self.assertIn(f'kPayload[] = "{probe.oldapp_probe_dex.PAYLOAD}"', source)
        self.assertIn("Java_org_example_andarium_oldappprobe_Probe_run", source)

    def test_every_native_method_of_the_class_is_bound_in_the_native_probe(self) -> None:
        source = probe.PROBE_SOURCE.read_text()
        for name, _, _ in probe.oldapp_probe_dex.NATIVES:
            bound_by_name = f"Java_org_example_andarium_oldappprobe_Probe_{name}" in source
            registered = f'{{"{name}",' in source
            self.assertTrue(bound_by_name or registered, name)

    def test_the_spin_loop_calls_the_empty_native_method_n_times(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        spin = dex.instructions(dex.class_methods()["spin"][1])
        self.assertEqual(spin[0], 0x0012, "const/4 v0, #0")
        self.assertEqual(spin[1] & 0xFF, 0x35, "if-ge")
        self.assertEqual(spin[-1], 0x000F, "return v0")

    def test_the_asset_reader_and_the_scene_loop(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        methods = dex.class_methods()
        asset = dex.instructions(methods["asset"][1])
        self.assertEqual(asset[0] & 0xFF, 0x23, "new-array")
        self.assertEqual(asset[2] & 0xFF, 0x8D, "int-to-byte: Arrays.fill takes a byte")
        self.assertEqual(asset[-1] & 0xFF, 0x11, "return-object")
        self.assertEqual(dex.instructions(methods["peek"][1])[0] & 0xFF, 0x48, "aget-byte")
        scene = dex.instructions(methods["scene"][1])
        self.assertEqual(scene[-1], 0x0F | 0 << 8, "return v0")
        self.assertIn(0xB5, [unit & 0xFF for unit in scene], "and-int/2addr: every frame's status is kept")
        owner = "Lorg/example/andarium/oldappprobe/Probe;"
        listed = {(name, shorty, ret) for klass, name, shorty, ret in dex.methods if klass == owner}
        self.assertIn(("asset", "LII", "[B"), listed)
        self.assertIn(("peek", "ILI", "I"), listed)
        self.assertIn(("scene", "II", "I"), listed)
        self.assertIn(("sceneFrame", "II", "I"), listed)

    def test_the_render_thread_is_a_java_thread_that_runs_the_probe_class(self) -> None:
        dex = Dex(probe.oldapp_probe_dex.build_dex())
        methods = dex.class_methods()
        owner = "Lorg/example/andarium/oldappprobe/Probe;"
        self.assertEqual(dex.interfaces(), ["Ljava/lang/Runnable;"])
        listed = {(klass, name, shorty) for klass, name, shorty, ret in dex.methods}
        for method in (("Ljava/lang/Thread;", "<init>", "VL"), ("Ljava/lang/Thread;", "start", "V"),
                       ("Ljava/lang/Thread;", "join", "V"), (owner, "run", "V"), (owner, "render", "V")):
            self.assertIn(method, listed)
        render = dex.instructions(methods["render"][1])
        self.assertEqual([unit & 0xFF for unit in render].count(0x22), 2, "new-instance: the Thread and the probe object")
        self.assertEqual([unit & 0xFF for unit in render].count(0x6E), 2, "invoke-virtual: start and join")
        body = dex.instructions(methods["run/instance"][1])
        self.assertEqual(body[-1] & 0xFF, 0x0E, "return-void")
        self.assertEqual([unit & 0xFF for unit in body].count(0x71), 3, "sceneFrames, scene, sceneDone")

    def test_the_named_variant_runs_in_a_process_that_is_not_its_package(self) -> None:
        named = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("named")))
        old = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("old")))
        application = [child for child in named.children if child.tag == "application"][0]
        process = application.attributes["process"]
        self.assertEqual(process, probe.PROCESS_NAMES["named"])
        self.assertFalse(process.startswith(probe.package_of("named")))
        self.assertNotIn("process", [child for child in old.children if child.tag == "application"][0].attributes)
        self.assertEqual(named.attributes["package"], probe.package_of("named"))

    def test_the_variants_differ_in_package_and_target_level_only(self) -> None:
        old = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("old")))
        modern = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("modern")))
        self.assertEqual(old.attributes["package"], "org.example.andarium.oldappprobe")
        self.assertEqual(modern.attributes["package"], "org.example.andarium.oldappprobe.modern")
        old_sdk, modern_sdk = old.children[0], modern.children[0]
        self.assertEqual(old_sdk.attributes["targetSdkVersion"], 10)
        self.assertLess(old_sdk.attributes["targetSdkVersion"], 14)
        self.assertEqual(modern_sdk.attributes["targetSdkVersion"], 29)
        self.assertGreaterEqual(modern_sdk.attributes["targetSdkVersion"], 14)
        for tree in (old, modern):
            application = tree.children[1]
            self.assertNotIn("debuggable", application.attributes, "ART runs a debuggable application with CheckJNI")
            receiver = application.children[0]
            self.assertEqual(receiver.attributes["name"], probe.RECEIVER)
            self.assertIs(receiver.attributes["exported"], True)
            self.assertEqual(receiver.children, [], "no intent filter: only an explicit component starts it")

    def test_the_package_holds_the_code_and_the_32_bit_libraries(self) -> None:
        if shutil.which("jarsigner") is None or shutil.which("keytool") is None:
            self.skipTest("no JDK to sign the package")
        with tempfile.TemporaryDirectory(prefix="oldapp-test-") as scratch_name:
            scratch = Path(scratch_name)
            lib = scratch / "libkoldprobe.so"
            lib.write_bytes(b"\x7fELFprobe")
            shim = scratch / "libkernarium_oldapp.so"
            shim.write_bytes(b"\x7fELFshim")
            plain = probe.build_apk(scratch / "plain.apk", "old", lib)
            bundled = probe.build_apk(scratch / "bundled.apk", "modern", lib, shim)
            with zipfile.ZipFile(plain) as package:
                names = set(package.namelist())
            self.assertIn("classes.dex", names)
            self.assertIn("lib/armeabi-v7a/libkoldprobe.so", names)
            self.assertNotIn("lib/armeabi-v7a/libkernarium_oldapp.so", names)
            self.assertTrue(any(name.startswith("META-INF/") and name.endswith(".RSA") for name in names))
            with zipfile.ZipFile(bundled) as package:
                self.assertEqual(package.read("lib/armeabi-v7a/libkernarium_oldapp.so"), b"\x7fELFshim")

    def test_the_native_sources_build_freestanding_for_armv7(self) -> None:
        clang, lld = shutil.which("clang++"), shutil.which("ld.lld")
        if clang is None or lld is None:
            self.skipTest("no clang++ or ld.lld on PATH")
        with tempfile.TemporaryDirectory(prefix="oldapp-test-") as scratch_name:
            scratch = Path(scratch_name)
            library = probe.compile_library((probe.PROBE_SOURCE,), scratch / "libkoldprobe.so", "libkoldprobe.so",
                                            clang=clang, lld=lld, needed=probe.PROBE_NEEDED)
            shim = probe.compile_library(probe.SHIM_SOURCES, scratch / "libkernarium_oldapp.so",
                                         "libkernarium_oldapp.so", clang=clang, lld=lld, needed=probe.SHIM_NEEDED)
            for path, names, needed in ((library, (b"Java_org_example_andarium_oldappprobe_Probe_run", b"JNI_OnLoad"),
                                         probe.PROBE_NEEDED),
                                        (shim, (b"JNI_OnLoad",), probe.SHIM_NEEDED)):
                data = path.read_bytes()
                self.assertEqual(data[:4], b"\x7fELF")
                self.assertEqual(data[4], 1, "ELF32")
                self.assertEqual(struct.unpack_from("<HH", data, 16), (3, 40), "a shared object for ARM")
                for name in names:
                    self.assertIn(name, data)
                # bionic resolves an undefined symbol only in the libraries the object names.
                for name in needed:
                    self.assertIn(name.encode() + b"\0", data)


class VerdictTests(unittest.TestCase):
    SEMANTICS = {"write_through": "pass", "identity": "pass", "retained": "pass", "monitor": "pass", "pop": "pass",
                 "getline": "pass", "usable_size": "pass", "libc_block": "pass", "table_read_only": "pass",
                 "java_fill": "pass", "commit_chunks": "pass", "clone": "pass", "alias_elements": "pass",
                 "alias_critical": "pass", "abi_args": "pass", "abi_returns": "pass", "ends_exception": "pass"}
    ENDS = ("ends_lazy_thumb", "ends_lazy_arm", "ends_reg_thumb", "ends_reg_arm", "ends_recursion", "ends_onload",
            "ends_outside", "ended_keep", "ended_arrays", "ended_pin", "ended_kept_ref")
    OLD = {"shim_mapped": "yes", "raw_read": "pass", "passback": "pass", "elements": "pass", "global": "pass",
           "thread_raw": "pass", **SEMANTICS, "java_fill_raw": "pass", "raw_write_java": "pass",
           "low_heap": "pass", **dict.fromkeys(ENDS, "pass"), "empty_us": "123456", "done": "1"}
    MODERN = {"shim_mapped": "no", "raw_read": "fail", "passback": "pass", "elements": "pass", "global": "pass",
              "thread_raw": "fail", **SEMANTICS, "java_fill_raw": "fail", "raw_write_java": "skipped",
              "low_heap": "pass", **dict.fromkeys(ENDS, "fail"), "empty_us": "123456", "done": "1"}

    def test_a_probe_that_reports_what_the_variant_must_passes(self) -> None:
        self.assertIn("raw_read=pass", runner.evaluate("old", self.OLD))
        self.assertIn("shim_mapped=no", runner.evaluate("modern", self.MODERN))

    def test_the_heap_of_a_modern_application_is_recorded_not_judged(self) -> None:
        runner.evaluate("modern", {**self.MODERN, "low_heap": "fail"})

    def test_every_difference_is_named(self) -> None:
        for key, value in (("shim_mapped", "no"), ("raw_read", "fail"), ("passback", "fail"), ("elements", "fail"),
                           ("global", "fail"), ("thread_raw", "fail"), ("low_heap", "fail"),
                           ("write_through", "fail"), ("identity", "fail"), ("retained", "fail"), ("monitor", "fail"),
                           ("pop", "fail"), ("getline", "fail"), ("usable_size", "fail"), ("libc_block", "fail"),
                           ("table_read_only", "fail"), ("java_fill", "fail"), ("java_fill_raw", "fail"), ("raw_write_java", "fail"),
                           ("commit_chunks", "fail"), ("clone", "fail"), ("alias_elements", "fail"), ("alias_critical", "fail"),
                           ("abi_args", "fail"), ("abi_returns", "fail"), ("ends_exception", "fail"),
                           ("ends_lazy_thumb", "fail"), ("ends_lazy_arm", "fail"), ("ends_reg_thumb", "fail"),
                           ("ends_reg_arm", "fail"), ("ends_recursion", "fail"), ("ends_onload", "fail"),
                           ("ends_outside", "fail"), ("ended_keep", "fail"), ("ended_arrays", "fail"),
                           ("ended_pin", "fail"), ("ended_kept_ref", "fail"), ("done", "")):
            with self.assertRaises(runner.Failure) as raised:
                runner.evaluate("old", {**self.OLD, key: value})
            self.assertIn(f"{key}={value}", str(raised.exception))

    def test_a_missing_last_line_fails_whatever_else_passed(self) -> None:
        partial = {key: value for key, value in self.OLD.items() if key != "done"}
        with self.assertRaises(runner.Failure) as raised:
            runner.evaluate("old", partial)
        self.assertIn("done=<missing>", str(raised.exception))

    def test_the_layer_in_a_modern_process_fails(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.evaluate("modern", {**self.MODERN, "shim_mapped": "yes"})
        with self.assertRaises(runner.Failure):
            runner.evaluate("modern", {**self.MODERN, "raw_read": "pass"})

    def test_what_the_runtime_does_with_an_array_holds_with_and_without_the_layer(self) -> None:
        for variant, result in (("modern", self.MODERN), ("control", {**self.MODERN, "thread_raw": "fail"})):
            for key in self.SEMANTICS:
                with self.assertRaises(runner.Failure, msg=f"{variant} {key}"):
                    runner.evaluate(variant, {**result, key: "fail"})

    def test_a_probe_that_did_not_finish_carries_the_guests_log(self) -> None:
        with self.assertRaises(runner.Failure) as raised:
            runner.evaluate("old", {"log": "FATAL EXCEPTION: cannot locate symbol open"})
        self.assertIn("the log: FATAL EXCEPTION: cannot locate symbol open", str(raised.exception))

    def test_the_result_file_and_the_log_are_read(self) -> None:
        self.assertEqual(runner.parse_result("a=1\n\nb=\njunk\nc=x=y\n"), {"a": "1", "b": "", "c": "x=y"})
        self.assertEqual(runner.parse_result("a=1;b=pass;done=1;"), {"a": "1", "b": "pass", "done": "1"})
        self.assertEqual(runner.broadcast_data('Broadcast completed: result=0, data="x=1;y=2;"\n'), "x=1;y=2;")
        self.assertEqual(runner.broadcast_data("Broadcast completed: result=0\n"), "")
        log = ("11-15 kernarium_oldapp: region below 2 GiB at 0x30000000\n"
               "11-15 kernarium_oldapp: installed: jni=1 arena=1\n"
               "11-15 other: installed: nothing\n")
        self.assertEqual(runner.installs_in_log(log), 1)
        declined = ("11-15 kernarium_oldapp: the first byte[] result stayed a real reference (thread limit); "
                    "blocks and bytes held: 512 2048\n"
                    "11-15 other: the first byte[] result stayed a real reference\n")
        self.assertEqual(runner.lines_in_log(declined + log, runner.DECLINED), 1)
        self.assertEqual(runner.lines_in_log(log, runner.DECLINED), 0)


class FakeGuest:
    """Records the commands; answers a result file that the test sets."""

    def __init__(self, result: str, installs: list[str] | None = None) -> None:
        self.commands: list[str] = []
        self.result = result
        self.logs = installs or ["", ""]
        self.log_reads = 0

    def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
        self.commands.append(" ".join(arguments))
        if arguments[0] == "logcat":
            text = self.logs[min(self.log_reads, len(self.logs) - 1)]
            self.log_reads += 1
            return CompletedProcess(arguments, 0, text)
        if arguments[0] == "install":
            return CompletedProcess(arguments, 0, "Performing Streamed Install\nSuccess\n")
        if "cat " in arguments[-1]:
            return CompletedProcess(arguments, 0, self.result)
        return CompletedProcess(arguments, 0, "")

    def shell(self, command: str, timeout: float = 30) -> str:
        self.commands.append("shell " + command)
        data = f', data="{self.result}"' if self.result else ""
        return f"Broadcast completed: result=0{data}\n"


LAYER_LOG = ("I/kernarium_oldapp( 1): installed: jni=1 arena=1\n"
             "I/kernarium_oldapp( 1): most live byte[] blocks one thread has held; blocks live in the process: 241 241\n"
             "I/kernarium_oldapp( 1): the first byte[] blocks ended at the return of a native method; references ended, "
             "native methods with a thunk: 400 4\n"
             "I/kernarium_oldapp( 1): the most references one native method call has ended at its return; native "
             "methods with a thunk: 171 6\n")
SCENE_RESULT = "scene_complete=pass;scene_data=pass;scene_write=pass;scene_attached=pass;peak_result=pass;done=1;"


class SceneGuest(FakeGuest):
    """Answers the log of a scene: the layer's lines (or none) after the first read of the install count."""

    def __init__(self, log: str) -> None:
        super().__init__(SCENE_RESULT)
        self.log = log

    def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
        if arguments[0] == "logcat":
            self.commands.append(" ".join(arguments))
            if "-c" in arguments:
                return CompletedProcess(arguments, 0, "")
            self.log_reads += 1
            if "-s" in arguments:  # the install count: nothing before the run, what the log says after
                return CompletedProcess(arguments, 0, "" if self.log_reads == 1 else self.log)
            return CompletedProcess(arguments, 0, self.log)
        return super().adb_run(*arguments, timeout=timeout)


class SceneTests(unittest.TestCase):
    def test_the_scene_must_pass_in_every_variant(self) -> None:
        self.assertEqual(set(runner.EXPECTED["scene"]),
                         {"scene_complete", "scene_data", "scene_write", "scene_attached", "done"})
        with self.assertRaises(runner.Failure) as raised:
            runner.evaluate("scene", {"scene_complete": "pass", "scene_data": "fail", "scene_write": "pass", "done": "1"})
        self.assertIn("scene_data=fail", str(raised.exception))

    def test_the_peaks_are_read_from_the_layers_lines(self) -> None:
        self.assertEqual(runner.thread_peak(LAYER_LOG), 241)
        self.assertEqual(runner.thread_peak(""), 0)
        region = "I/kernarium_oldapp( 1): the longest a critical region of a synchronization stayed open, microseconds: 1234 0\n"
        self.assertEqual(runner.longest_region(LAYER_LOG + region), 1234)
        self.assertEqual(runner.longest_region(LAYER_LOG), 0)
        service = "I/adbd( 5): adbd service requested 'shell,v2:logcat -d -s kernarium_oldapp:I'\n"
        self.assertEqual(runner.layer_lines(service + LAYER_LOG).count("\n"), LAYER_LOG.count("\n") - 1,
                         "the adb service lines that name the tag are not the layer's")

    def test_a_log_with_the_layers_lines_and_none_of_the_forbidden_ones_is_clean(self) -> None:
        self.assertEqual(runner.scene_log_problems(LAYER_LOG, layer=True), [])

    def test_every_forbidden_line_is_named(self) -> None:
        declined = LAYER_LOG + "I/kernarium_oldapp( 1): the first byte[] result stayed a real reference (thread limit); x 512 9\n"
        self.assertIn("a result stayed ART's own reference", runner.scene_log_problems(declined, layer=True))
        arena = LAYER_LOG + "I/kernarium_oldapp( 1): the low arena has no room: an allocation went to libc; bytes 1 2\n"
        self.assertIn("an allocation left the arena", runner.scene_log_problems(arena, layer=True))
        for word in runner.CRASH_WORDS:
            self.assertTrue(runner.scene_log_problems(LAYER_LOG + f"F/libc: {word} here\n", layer=True), word)
        at_limit = LAYER_LOG.replace("241 241", "512 512")
        self.assertTrue(any("most live blocks" in p for p in runner.scene_log_problems(at_limit, layer=True)))
        small = LAYER_LOG.replace("241 241", "12 12")
        self.assertTrue(any("most live blocks" in p for p in runner.scene_log_problems(small, layer=True)))
        no_ends = "\n".join(line for line in LAYER_LOG.splitlines() if "first byte[] blocks ended" not in line)
        self.assertTrue(any("no block ended" in p for p in runner.scene_log_problems(no_ends, layer=True)))

    def test_without_the_layer_its_lines_are_a_problem(self) -> None:
        self.assertEqual(runner.scene_log_problems("I/art( 1): hello\nI/adbd( 5): service 'logcat -s kernarium_oldapp:I'\n",
                                                   layer=False), [])
        self.assertTrue(runner.scene_log_problems(LAYER_LOG, layer=False))

    def test_a_scene_with_the_layer_runs_twice_in_one_process_and_reads_the_log(self) -> None:
        guest = SceneGuest(LAYER_LOG)
        with mock.patch.object(runner, "PEAK_WAIT_SECONDS", 0):
            note = runner.scene(guest, "old", shim=True, layer=True)
        self.assertIn("most live blocks of a thread 241", note)
        broadcasts = [c for c in guest.commands if "am broadcast" in c]
        self.assertEqual(len(broadcasts), 2)
        self.assertIn("--es opts scene", broadcasts[0])
        self.assertIn("--es opts peak", broadcasts[1])
        stops = [c for c in guest.commands if "am force-stop" in c]
        self.assertEqual(len(stops), 1, "the second run goes to the process of the first")

    def test_a_debuggable_scene_must_have_run_with_checkjni(self) -> None:
        with mock.patch.object(runner, "PEAK_WAIT_SECONDS", 0):
            with self.assertRaises(runner.Failure) as raised:
                runner.scene(SceneGuest(LAYER_LOG), "old", shim=True, layer=True, checked=True)
            self.assertIn("CheckJNI", str(raised.exception))
            runner.scene(SceneGuest(LAYER_LOG + "I/art( 1): Late-enabling -Xcheck:jni\n"), "old", shim=True, layer=True,
                         checked=True)

    def test_a_scene_without_the_layer_is_one_run_and_installs_nothing(self) -> None:
        guest = SceneGuest("I/art( 1): hello\n")
        runner.scene(guest, "modern", shim=False, layer=False)
        self.assertEqual(len([c for c in guest.commands if "am broadcast" in c]), 1)
        with self.assertRaises(runner.Failure):
            runner.scene(SceneGuest(LAYER_LOG), "modern", shim=False, layer=False)


class StepTests(unittest.TestCase):
    def test_an_application_is_installed_past_the_block_on_old_targets(self) -> None:
        guest = FakeGuest("")
        runner.install(guest, Path("/tmp/old.apk"), "old")
        self.assertEqual(guest.commands, ["uninstall org.example.andarium.oldappprobe",
                                          "install -r --bypass-low-target-sdk-block /tmp/old.apk"])

    def test_a_failed_install_names_the_package(self) -> None:
        class Refusing(FakeGuest):
            def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
                return CompletedProcess(arguments, 1, "Failure [INSTALL_FAILED_DEPRECATED_SDK_VERSION]")

        with self.assertRaises(runner.Failure) as raised:
            runner.install(Refusing(""), Path("/tmp/old.apk"), "old")
        self.assertIn("org.example.andarium.oldappprobe", str(raised.exception))

    def test_the_broadcast_names_the_component_and_the_extras(self) -> None:
        guest = FakeGuest("shim_mapped=yes;done=1;", ["", "k kernarium_oldapp: installed: jni=1 arena=1\n"])
        result, installs = runner.run_variant(guest, "old", shim=True)
        self.assertEqual(result, {"shim_mapped": "yes", "done": "1"})
        self.assertEqual(installs, 1)
        broadcasts = [c for c in guest.commands if "am broadcast" in c]
        self.assertEqual(len(broadcasts), 1)
        self.assertIn(f"-n {probe.component_of('old')}", broadcasts[0])
        self.assertIn("--es shim kernarium_oldapp", broadcasts[0])
        self.assertNotIn("--es opts", broadcasts[0])
        stop = next(i for i, c in enumerate(guest.commands) if "am force-stop" in c)
        self.assertLess(stop, next(i for i, c in enumerate(guest.commands) if "am broadcast" in c),
                        "a process of an earlier run is gone before the new one starts")

    def test_the_review_is_completed_with_the_shared_helper(self) -> None:
        """The runner presses Continue with permission_review.py, which reads the package's whole runtime
        permissions block: the flag on a late row of many is found, and the failure is the runner's."""
        rows = "\n".join(f"        android.permission.P{index}: granted=true, flags=[ ]" for index in range(9))
        flagged = ("  Package [p] (abc):\n    User 0: installed=true\n      runtime permissions:\n" + rows +
                   "\n        android.permission.LATE: granted=true, flags=[ REVIEW_REQUIRED]\n    other: x\n")
        clear = flagged.replace("REVIEW_REQUIRED", "USER_SET")
        button = ("<hierarchy><node resource-id=\"com.android.permissioncontroller:id/continue_button\" "
                  "bounds=\"[0,100][200,200]\" /></hierarchy>")

        class Reviewing(FakeGuest):
            pending = True
            button_shown = True

            def adb_run(self, *arguments: str, timeout: float = 30) -> CompletedProcess:
                self.commands.append(" ".join(arguments))
                if arguments[-1] == f"cat {runner.permission_review.UI_DUMP}":
                    return CompletedProcess(arguments, 0, button if self.button_shown else "<hierarchy/>")
                return CompletedProcess(arguments, 0, "")

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                if command.startswith("dumpsys package"):
                    return flagged if self.pending else clear
                if command.startswith("input tap"):
                    self.pending = False
                return ""

        guest = Reviewing("")
        with mock.patch("time.sleep"):
            self.assertEqual(runner.permission_review.complete_review(guest, "p", runner.Failure),
                             "permission review completed")
            self.assertIn("shell input tap 100 150", guest.commands)
            self.assertTrue(any("android.intent.action.REVIEW_PERMISSIONS" in c for c in guest.commands))
            self.assertEqual(runner.permission_review.complete_review(guest, "p", runner.Failure), "no review pending")
            guest.pending, guest.button_shown = True, False
            with mock.patch.object(runner.permission_review, "REVIEW_SECONDS", 0):
                with self.assertRaises(runner.Failure):
                    runner.permission_review.complete_review(guest, "p", runner.Failure)

    def test_the_library_in_the_image_carries_the_label_an_application_may_execute(self) -> None:
        class Listing(FakeGuest):
            text = ""

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                return self.text

        guest = Listing("")
        guest.text = f"-rw-r--r-- 1 root root {probe.SHIM_IMAGE_LABEL} 95816 2026-11-15 {probe.SHIM_IMAGE_PATH}\n"
        self.assertIn(probe.SHIM_IMAGE_LABEL, runner.layer_in_image(guest))
        guest.text = f"-rw-r--r-- 1 root root u:object_r:system_file:s0 95816 2026-11-15 {probe.SHIM_IMAGE_PATH}\n"
        with self.assertRaises(runner.Failure) as raised:
            runner.layer_in_image(guest)
        self.assertIn(probe.SHIM_IMAGE_LABEL, str(raised.exception))

    def test_an_application_is_switched_on_and_off_and_the_guest_must_hold_it(self) -> None:
        class Properties(FakeGuest):
            def __init__(self) -> None:
                super().__init__("")
                self.value = ""

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                if command.startswith("su 0 setprop"):
                    self.value = command.split("'")[1]
                    return ""
                return self.value + "\n"

        guest = Properties()
        package = probe.package_of("old")
        name = f"persist.sys.kernarium.oldapp.{package}"
        self.assertIn("switched on", runner.switch(guest, package, True))
        self.assertTrue(guest.value.isdigit(), "the value is the turn-on time in epoch seconds")
        self.assertTrue(any(c.startswith(f"shell su 0 setprop {name} '") for c in guest.commands))
        self.assertIn("switched off", runner.switch(guest, package, False))
        self.assertEqual(guest.value, "")

        class Refusing(Properties):
            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                return ""

        with self.assertRaises(runner.Failure):
            runner.switch(Refusing(), package, True)

    def test_an_image_with_the_switch_command_is_switched_with_it_and_without_root(self) -> None:
        class Image(FakeGuest):
            def __init__(self, command: bool) -> None:
                super().__init__("")
                self.value = ""
                self.command = command

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append("shell " + command)
                if command.startswith("command -v"):
                    return "/system/bin/kernarium_oldapp\n" if self.command else ""
                if command.startswith("kernarium_oldapp set"):
                    self.value = "1700000000" if " on;" in command else ""
                    return "rc=0\n"
                if command.startswith("su 0"):
                    self.value = command.split("'")[1]
                    return ""
                return self.value + "\n"

        package = probe.package_of("old")
        with mock.patch.object(runner.time, "sleep"):
            with_command = Image(True)
            self.assertIn("with kernarium_oldapp", runner.switch(with_command, package, True))
            self.assertIn(f"shell kernarium_oldapp set {package} on; echo rc=$?", with_command.commands)
            self.assertEqual(with_command.value, "1700000000")
            self.assertIn("switched off", runner.switch(with_command, package, False))
            self.assertFalse(any(c.startswith("shell su ") for c in with_command.commands), "a user build has no su")
            without = Image(False)
            runner.switch(without, package, True)
            self.assertTrue(any(c.startswith("shell su 0 setprop") for c in without.commands))

            class Failing(Image):
                def shell(self, command: str, timeout: float = 30) -> str:
                    if command.startswith("kernarium_oldapp set"):
                        self.commands.append("shell " + command)
                        return "not eligible\nrc=1\n"
                    return super().shell(command, timeout)

            with self.assertRaises(runner.Failure):
                runner.switch(Failing(True), package, True)

    def test_the_old_probe_is_told_where_another_packages_copy_of_the_library_is(self) -> None:
        class Listing(FakeGuest):
            def shell(self, command: str, timeout: float = 30) -> str:
                if command.startswith("pm path"):
                    return "package:/data/app/~~Qw3e==/org.example.andarium.oldappprobe.modern-Zx9==/base.apk\n"
                if command.startswith("ls "):
                    return "arm\n"
                return super().shell(command, timeout)

        guest = Listing("done=1;")
        outside = runner.outside_library(guest)
        self.assertEqual(outside, "/data/app/~~Qw3e==/org.example.andarium.oldappprobe.modern-Zx9==/lib/arm/libkoldprobe.so")
        runner.run_variant(guest, "old", shim=False, options=outside)
        broadcast = next(c for c in guest.commands if "am broadcast" in c)
        self.assertIn("--es opts '" + outside + "'", broadcast)

    def test_a_missing_modern_package_fails_the_lookup(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.outside_library(FakeGuest(""))

    def test_a_modern_run_asks_for_nothing_extra(self) -> None:
        guest = FakeGuest("done=1;")
        runner.run_variant(guest, "modern", shim=False)
        broadcast = next(c for c in guest.commands if "am broadcast" in c)
        self.assertNotIn("--es", broadcast)
        self.assertIn(probe.package_of("modern"), broadcast)

    def test_the_parser_defaults_to_the_orientation_runners_port(self) -> None:
        options = runner.build_parser().parse_args([])
        self.assertEqual(options.adb_port, runner.orient.slot_ports.slot_port(
            runner.orient.slot_ports.ADB_OFFSET, runner.orient.DEFAULT_ADB_PORT))
        self.assertFalse(options.host_shim)


if __name__ == "__main__":
    unittest.main()
