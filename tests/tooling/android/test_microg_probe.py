#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The microG signature spoofing probe (tests/integration/android/microg) and its runner.

The probe's negative case is only meaningful if the decoys would be spoofed but for their signer: the package
names must be the two microG packages and the `fake-signature` must be the certificate LineageOS compares
with (ComputerEngine.MICROG_FAKE_SIGNATURE, Google's Android certificate). These tests check the sources
against each other and the runner's reading of `am instrument -r` output; the run itself is the opt-in
`android-microg-spoofing` integration test.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import io
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
PROBE = ROOT / "tests/integration/android/microg"
RUNNER = ROOT / "tests/integration/android/run_android_microg_spoofing.py"
ANDROID = "{http://schemas.android.com/apk/res/android}"
# The SHA-256 of the certificate that LineageOS's ComputerEngine presents for microG's packages.
GOOGLE_SHA256 = "f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83"
PACKAGES = ("com.google.android.gms", "com.android.vending")

SPEC = importlib.util.spec_from_file_location("run_android_microg_spoofing", RUNNER)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)


def decoy(directory: str) -> tuple[str, str, str]:
    """(package, hasCode, fake-signature) of a decoy manifest."""
    manifest = ET.parse(PROBE / directory / "AndroidManifest.xml").getroot()
    application = manifest.find("application")
    assert application is not None
    values = {item.get(ANDROID + "name"): item.get(ANDROID + "value") for item in application.findall("meta-data")}
    return manifest.get("package", ""), application.get(ANDROID + "hasCode", ""), values.get("fake-signature", "")


class DecoySourceTests(unittest.TestCase):
    def test_the_decoys_take_the_two_microg_names(self) -> None:
        self.assertEqual(decoy("decoy-services")[0], PACKAGES[0])
        self.assertEqual(decoy("decoy-store")[0], PACKAGES[1])

    def test_the_decoys_carry_no_code(self) -> None:
        for directory in ("decoy-services", "decoy-store"):
            with self.subTest(decoy=directory):
                self.assertEqual(decoy(directory)[1], "false")
                self.assertEqual(sorted(path.name for path in (PROBE / directory).iterdir()), ["AndroidManifest.xml"])

    def test_the_fake_signature_is_googles_certificate_in_both(self) -> None:
        values = {decoy(directory)[2] for directory in ("decoy-services", "decoy-store")}
        self.assertEqual(len(values), 1)
        value = values.pop()
        self.assertRegex(value, r"^[0-9a-f]+$")
        der = bytes.fromhex(value)
        # A DER certificate: a SEQUENCE whose two-byte length covers the rest.
        self.assertEqual(der[:2], b"\x30\x82")
        self.assertEqual(int.from_bytes(der[2:4], "big"), len(der) - 4)
        self.assertEqual(hashlib.sha256(der).hexdigest(), GOOGLE_SHA256)

    def test_no_overlay_file_names_a_probe_module(self) -> None:
        # The probe never reaches an image: no product makefile or overlay names its modules.
        for path in (ROOT / "android/lineage-24/overlay").rglob("*"):
            if path.is_file() and path.suffix in {".mk", ".bp", ".xml", ".txt"}:
                self.assertNotIn("AndariumMicrog", path.read_text(errors="replace"), str(path))


class ProbeSourceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.java = (PROBE / "src/com/android/frameworks/andariummicrog/MicrogSpoofingProbe.java").read_text()
        self.manifest = ET.parse(PROBE / "AndroidManifest.xml").getroot()

    def test_the_probe_reads_the_two_names_and_knows_googles_digest(self) -> None:
        self.assertIn(f'GOOGLE = "{GOOGLE_SHA256}"', self.java)
        match = re.search(r"PACKAGES = \{([^}]*)\}", self.java)
        self.assertIsNotNone(match)
        self.assertEqual(re.findall(r'"([^"]+)"', match.group(1)), list(PACKAGES))

    def test_the_manifest_makes_both_names_visible_and_declares_the_runner(self) -> None:
        queried = [item.get(ANDROID + "name") for item in self.manifest.findall("queries/package")]
        self.assertEqual(queried, list(PACKAGES))
        instrumentation = self.manifest.find("instrumentation")
        self.assertIsNotNone(instrumentation)
        self.assertEqual(instrumentation.get(ANDROID + "targetPackage"), self.manifest.get("package"))
        self.assertEqual(self.manifest.get("package"), runner.PROBE_PACKAGE)
        self.assertEqual(instrumentation.get(ANDROID + "name"), runner.PROBE_RUNNER.split("/", 1)[1])

    def test_the_own_mode_requires_the_decoys_fake_signature(self) -> None:
        # Without it a decoy would not be spoofed whatever the build does and the check would prove nothing.
        self.assertIn('getString("fake-signature")', self.java)
        self.assertIn("GET_META_DATA", self.java)
        self.assertRegex(self.java, r"GOOGLE\.equals\(found\.fake\)")

    def test_every_mode_the_runner_uses_exists_in_the_probe(self) -> None:
        for mode in ("none", "own", "google", "notgoogle", "report"):
            self.assertIn(f'case "{mode}":', self.java)

    def test_every_apk_the_runner_installs_is_built_by_the_script(self) -> None:
        script = (PROBE / "build-apks.sh").read_text()
        for name in runner.APKS.values():
            self.assertIn(name, script)

    def test_the_apks_are_never_signed_with_a_release_key(self) -> None:
        script = (PROBE / "build-apks.sh").read_text()
        self.assertIn("testkey.pk8", script)
        self.assertNotIn("releasekey", script)


class BuildScriptTests(unittest.TestCase):
    def run_script(self, root: Path, output: Path) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(PROBE / "build-apks.sh"), str(root), str(output)], text=True,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=30, check=False)

    def test_an_output_inside_the_source_tree_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / "tree"
            root.mkdir()
            result = self.run_script(root, root / "out")
            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertIn("outside the source tree", result.stdout)

    def test_a_tree_without_the_sdk_tools_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch) / "tree"
            root.mkdir()
            result = self.run_script(root, Path(scratch) / "out")
            self.assertEqual(result.returncode, 2, result.stdout)
            self.assertIn("missing", result.stdout)
            self.assertEqual(list((Path(scratch) / "out").iterdir()), [])


STATUS = """\
INSTRUMENTATION_STATUS: package=com.google.android.gms
INSTRUMENTATION_STATUS: installed=true
INSTRUMENTATION_STATUS: signatures=aa(google)
INSTRUMENTATION_STATUS: signing_info=aa(google)
INSTRUMENTATION_STATUS: history=aa(google)
INSTRUMENTATION_STATUS_CODE: 0
INSTRUMENTATION_STATUS: package=com.android.vending
INSTRUMENTATION_STATUS: installed=false
INSTRUMENTATION_STATUS_CODE: 0
"""


def output(verdict: str = "PASS", expect: str = "none", code: str = "-1", extra: str = "") -> str:
    return (STATUS + f"INSTRUMENTATION_RESULT: expect={expect}\nINSTRUMENTATION_RESULT: verdict={verdict}\n"
            + extra + f"INSTRUMENTATION_CODE: {code}\n")


class InstrumentationTests(unittest.TestCase):
    def test_a_pass_is_read_whole(self) -> None:
        blocks, result, code = runner.parse_instrumentation("WARNING: noise\n" + output())
        self.assertEqual([block["package"] for block in blocks], list(PACKAGES))
        self.assertEqual(blocks[0]["signing_info"], "aa(google)")
        self.assertEqual(blocks[1], {"package": "com.android.vending", "installed": "false"})
        self.assertEqual(result, {"expect": "none", "verdict": "PASS"})
        self.assertEqual(code, "-1")
        self.assertEqual(runner.verdict_problems(blocks, result, code, "none"), [])

    def test_a_failure_names_its_reasons(self) -> None:
        text = output("FAIL", "own", extra="INSTRUMENTATION_RESULT: failures=com.google.android.gms reports x\n")
        blocks, result, code = runner.parse_instrumentation(text)
        problems = runner.verdict_problems(blocks, result, code, "own")
        self.assertEqual(len(problems), 1)
        self.assertIn("com.google.android.gms reports x", problems[0])

    def test_a_value_may_continue_on_the_next_line(self) -> None:
        text = output("FAIL", "own", extra="INSTRUMENTATION_RESULT: failures=first\nsecond\n")
        _, result, _ = runner.parse_instrumentation(text)
        self.assertEqual(result["failures"], "first\nsecond")

    def test_the_wrong_mode_the_wrong_code_and_a_missing_package_fail(self) -> None:
        blocks, result, code = runner.parse_instrumentation(output())
        self.assertTrue(runner.verdict_problems(blocks, result, code, "own"))
        self.assertTrue(runner.verdict_problems(blocks, result, "0", "none"))
        self.assertTrue(runner.verdict_problems(blocks[:1], result, code, "none"))

    def test_output_without_a_result_fails(self) -> None:
        blocks, result, code = runner.parse_instrumentation("INSTRUMENTATION_FAILED: x\n")
        self.assertTrue(runner.verdict_problems(blocks, result, code, "none"))


class FakeDevice:
    """A guest that installs and removes packages and answers the probe as a build without the spoofing bug would."""

    serial = "127.0.0.1:1"

    def __init__(self, installed: set[str] | None = None, spoofs_decoys: bool = False) -> None:
        self.installed = set(installed or ())
        self.spoofs_decoys = spoofs_decoys
        self.history: list[str] = []

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        assert arguments[:2] == ("-s", self.serial)
        verb, rest = arguments[2], arguments[3:]
        if verb == "install":
            name = Path(rest[-1]).name
            package = {"AndariumMicrogTests.apk": runner.PROBE_PACKAGE, "AndariumMicrogDecoyServices.apk": PACKAGES[0],
                       "AndariumMicrogDecoyStore.apk": PACKAGES[1]}[name]
            self.installed.add(package)
            self.history.append(f"install {package}")
            return subprocess.CompletedProcess(arguments, 0, "Performing Streamed Install\nSuccess\n")
        if verb == "uninstall":
            self.installed.discard(rest[0])
            self.history.append(f"uninstall {rest[0]}")
            return subprocess.CompletedProcess(arguments, 0, "Success\n")
        assert verb == "shell"
        match = re.search(r"-e expect (\w+) ", rest[0])
        assert match, rest[0]
        expect = match.group(1)
        self.history.append(f"probe {expect}")
        lines = []
        problems = []
        for package in PACKAGES:
            here = package in self.installed
            lines.append(f"INSTRUMENTATION_STATUS: package={package}\nINSTRUMENTATION_STATUS: installed={str(here).lower()}")
            if here:
                certificate = GOOGLE_SHA256 if self.spoofs_decoys else "11" * 32
                lines.append(f"INSTRUMENTATION_STATUS: signatures={certificate}\n"
                             f"INSTRUMENTATION_STATUS: signing_info={certificate}\nINSTRUMENTATION_STATUS: history={certificate}\n"
                             f"INSTRUMENTATION_STATUS: fake_signature={GOOGLE_SHA256}")
                if expect == "own" and self.spoofs_decoys:
                    problems.append(f"{package} reports Google's certificate")
            elif expect == "own":
                problems.append(f"{package} is not installed")
            if here and expect == "none":
                problems.append(f"{package} is installed")
            lines.append("INSTRUMENTATION_STATUS_CODE: 0")
        verdict = "FAIL" if problems else "PASS"
        text = "\n".join(lines) + f"\nINSTRUMENTATION_RESULT: expect={expect}\nINSTRUMENTATION_RESULT: verdict={verdict}\n"
        if problems:
            text += f"INSTRUMENTATION_RESULT: failures={'; '.join(problems)}\n"
        return subprocess.CompletedProcess(arguments, 0, text + "INSTRUMENTATION_CODE: -1\n")

    def shell(self, command: str, timeout: int = 30) -> str:
        assert command == "pm list packages"
        return "\n".join(f"package:{name}" for name in sorted(self.installed | {"android"}))


class RegressionFlowTests(unittest.TestCase):
    APKS = {key: Path(name) for key, name in runner.APKS.items()}

    def setUp(self) -> None:
        # The runner prints what the probe found.
        patcher = mock.patch("sys.stdout", new_callable=io.StringIO)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_clean_run_installs_checks_and_leaves_nothing(self) -> None:
        device = FakeDevice()
        runner.check_regression(device, self.APKS)
        self.assertEqual(device.history, [
            "install " + runner.PROBE_PACKAGE, "probe none", "install " + PACKAGES[0], "install " + PACKAGES[1],
            "probe own", "uninstall " + PACKAGES[0], "uninstall " + PACKAGES[1], "probe none",
            "uninstall " + runner.PROBE_PACKAGE])
        self.assertEqual(device.installed, set())

    def test_a_build_that_spoofs_the_decoys_fails_and_still_cleans_up(self) -> None:
        device = FakeDevice(spoofs_decoys=True)
        with self.assertRaises(runner.Failure) as raised:
            runner.check_regression(device, self.APKS)
        self.assertIn("Google's certificate", str(raised.exception))
        self.assertEqual(device.installed, set())

    def test_a_guest_that_already_has_microg_is_refused_untouched(self) -> None:
        device = FakeDevice({PACKAGES[0]})
        with self.assertRaises(runner.Failure):
            runner.check_regression(device, self.APKS)
        self.assertEqual(device.history, [])
        self.assertEqual(device.installed, {PACKAGES[0]})

    def test_report_mode_leaves_the_guests_packages_alone(self) -> None:
        device = FakeDevice({PACKAGES[0], PACKAGES[1]})
        runner.check_report(device, self.APKS, "report")
        self.assertEqual(device.installed, set(PACKAGES))

    def test_missing_apks_are_named(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            with self.assertRaises(runner.Failure) as raised:
                runner.apk_paths(Path(scratch))
            self.assertIn("AndariumMicrogTests.apk", str(raised.exception))
            with self.assertRaises(runner.Failure):
                runner.apk_paths(None)


class RegistrationTests(unittest.TestCase):
    def test_the_integration_test_is_opt_in_and_needs_both_inputs(self) -> None:
        text = (ROOT / "tests/integration/android/CMakeLists.txt").read_text()
        self.assertIn("NAME android-microg-spoofing", text)
        self.assertIn("UML_ANDROID_MICROG_RELEASE", text)
        self.assertIn("UML_ANDROID_MICROG_PROBE_DIR", text)
        self.assertRegex(text, r"if\(UML_ANDROID_MICROG_RELEASE AND UML_ANDROID_MICROG_PROBE_DIR")


if __name__ == "__main__":
    unittest.main()
