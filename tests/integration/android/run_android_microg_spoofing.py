#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify that an Android release presents Google's certificate for microG's own packages only.

The guest is the release on a fresh data disk, started with the macOS launcher (tools/boot_android.py), or any
running guest named by --serial. The contract is android/lineage-24/README.md (the restricted signature spoofing
of route p0007): `com.google.android.gms` and `com.android.vending` present Google's certificate, in both
`signatures` and `signingInfo`, only when they are signed by microG's own key and carry that certificate as their
`fake-signature` meta-data. The probe is tests/integration/android/microg (build its APKs with build-apks.sh and
name the directory with --probe-dir).

Mode `regression` (the default, on a guest without microG) checks the part that holds in every image:

- with nothing installed under those names, PackageManager reports neither package;
- two decoys with those names, signed with the AOSP test key and carrying Google's certificate as their
  `fake-signature`, report their own signer and not Google's, through both lists;
- with the decoys removed, the names are free again.

Mode `report` runs the probe once against what the guest has installed, for example microG's own packages, and
checks `--expect`: `google` (Google's certificate alone, in both lists, for both packages), `notgoogle` (not in
any list) or `report` (print only).

Every wait has a deadline, the guest's launcher is stopped through its own process on any exit, and the guest is
reached through the shared adb server, always named with `-s`.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import slot_ports  # noqa: E402


def load_adb_runner():
    """The adb debugging runner's guest helpers (Device, wait_booted, stop_launcher), read from its file."""
    path = HERE / "run_android_adb_debugging.py"
    spec = importlib.util.spec_from_file_location("run_android_adb_debugging", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


base = load_adb_runner()
Failure = base.Failure

DEFAULT_ADB_PORT = 5577
PACKAGES = ("com.google.android.gms", "com.android.vending")
PROBE_PACKAGE = "com.android.frameworks.andariummicrog.tests"
PROBE_RUNNER = f"{PROBE_PACKAGE}/com.android.frameworks.andariummicrog.MicrogSpoofingProbe"
APKS = {
    "probe": "AndariumMicrogTests.apk",
    "services": "AndariumMicrogDecoyServices.apk",
    "store": "AndariumMicrogDecoyStore.apk",
}
RUN_SECONDS = 180
INSTALL_SECONDS = 180
EXPECTATIONS = ("report", "google", "notgoogle")


def parse_instrumentation(text: str) -> tuple[list[dict[str, str]], dict[str, str], str | None]:
    """The status blocks, the result and the code of `am instrument -r` output.

    Every `INSTRUMENTATION_STATUS: key=value` line belongs to the block that the next
    `INSTRUMENTATION_STATUS_CODE` closes; `INSTRUMENTATION_RESULT: key=value` lines make the result; the
    value of a line that has no `key=` continues the one before (the framework wraps nothing, but a value
    may hold a newline).
    """
    blocks: list[dict[str, str]] = []
    current: dict[str, str] = {}
    result: dict[str, str] = {}
    code: str | None = None
    target: dict[str, str] | None = None
    last_key: str | None = None
    for line in text.splitlines():
        if line.startswith("INSTRUMENTATION_STATUS_CODE:"):
            blocks.append(current)
            current, target, last_key = {}, None, None
        elif line.startswith("INSTRUMENTATION_STATUS:") or line.startswith("INSTRUMENTATION_RESULT:"):
            body = line.split(":", 1)[1].strip()
            if "=" not in body:
                if target is not None and last_key is not None:
                    target[last_key] += "\n" + body
                continue
            key, value = body.split("=", 1)
            target = current if line.startswith("INSTRUMENTATION_STATUS:") else result
            target[key] = value
            last_key = key
        elif line.startswith("INSTRUMENTATION_CODE:"):
            code = line.split(":", 1)[1].strip()
            target, last_key = None, None
        elif target is not None and last_key is not None and line and not line.startswith("INSTRUMENTATION"):
            target[last_key] += "\n" + line
    if current:
        blocks.append(current)
    return blocks, result, code


def verdict_problems(blocks: list[dict[str, str]], result: dict[str, str], code: str | None,
                     expect: str) -> list[str]:
    """What is wrong with one probe run: an empty list is a pass of the mode `expect`."""
    problems: list[str] = []
    if code != "-1":
        problems.append(f"the instrumentation ended with code {code}")
    if result.get("expect") != expect:
        problems.append(f"the probe ran mode {result.get('expect')!r}, not {expect!r}")
    if result.get("verdict") != "PASS":
        problems.append(f"verdict {result.get('verdict')!r}: {result.get('failures', 'no reasons given')}")
    names = sorted(block.get("package", "") for block in blocks)
    if names != sorted(PACKAGES):
        problems.append(f"the probe reported {names}, not {sorted(PACKAGES)}")
    return problems


def apk_paths(directory: pathlib.Path | None) -> dict[str, pathlib.Path]:
    if directory is None:
        raise Failure("--probe-dir is required: the directory build-apks.sh filled")
    paths = {key: directory / name for key, name in APKS.items()}
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise Failure("the probe APKs are missing (tests/integration/android/microg/build-apks.sh builds them): "
                      + ", ".join(missing))
    return paths


def run_probe(device, expect: str) -> tuple[list[dict[str, str]], dict[str, str]]:
    result = device.run("-s", device.serial, "shell", f"am instrument -w -r -e expect {expect} {PROBE_RUNNER}",
                        timeout=RUN_SECONDS)
    if result.returncode != 0:
        raise Failure(f"am instrument exited {result.returncode}: {result.stdout.strip()[-500:]}")
    blocks, outcome, code = parse_instrumentation(result.stdout)
    for block in blocks:
        installed = block.get("installed")
        detail = (f" signatures={block.get('signatures')} signingInfo={block.get('signing_info')} "
                  f"history={block.get('history')} fakeSignature={block.get('fake_signature')}") if installed == "true" else ""
        print(f"  {block.get('package')}: installed={installed}{detail}")
    problems = verdict_problems(blocks, outcome, code, expect)
    if problems:
        raise Failure(f"probe mode {expect}: " + "; ".join(problems))
    return blocks, outcome


def install(device, path: pathlib.Path) -> None:
    result = device.run("-s", device.serial, "install", "-r", str(path), timeout=INSTALL_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing {path.name}: {result.stdout.strip()[-500:]}")


def uninstall(device, package: str) -> None:
    result = device.run("-s", device.serial, "uninstall", package, timeout=INSTALL_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"uninstalling {package}: {result.stdout.strip()[-300:]}")


def installed_packages(device) -> set[str]:
    listing = device.shell("pm list packages", timeout=60)
    return {line.removeprefix("package:").strip() for line in listing.splitlines() if line.startswith("package:")}


def check_regression(device, apks: dict[str, pathlib.Path]) -> None:
    present = installed_packages(device) & set(PACKAGES)
    if present:
        raise Failure(f"the guest already has {sorted(present)}: the regression mode needs a guest without microG")
    install(device, apks["probe"])
    try:
        print("nothing installed under the two names")
        run_probe(device, "none")
        try:
            install(device, apks["services"])
            install(device, apks["store"])
            print("test-key decoys with Google's certificate as their fake-signature")
            run_probe(device, "own")
        finally:
            for package in PACKAGES:
                if package in installed_packages(device):
                    uninstall(device, package)
        print("decoys removed")
        run_probe(device, "none")
    finally:
        if PROBE_PACKAGE in installed_packages(device):
            uninstall(device, PROBE_PACKAGE)


def check_report(device, apks: dict[str, pathlib.Path], expect: str) -> None:
    install(device, apks["probe"])
    try:
        print(f"what the guest has installed under the two names (expect {expect})")
        run_probe(device, expect)
    finally:
        if PROBE_PACKAGE in installed_packages(device):
            uninstall(device, PROBE_PACKAGE)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--serial", help="a running guest (host:port) that accepts this adb's key; no guest is launched")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--probe-dir", type=pathlib.Path,
                        help="the directory with the three APKs that build-apks.sh writes (required)")
    parser.add_argument("--mode", choices=("regression", "report"), default="regression")
    parser.add_argument("--expect", choices=EXPECTATIONS, default="report",
                        help="mode report: what the installed packages must show")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    return parser


def main() -> int:
    options = build_parser().parse_args()
    adb = shutil.which(options.adb) or options.adb
    guest: subprocess.Popen | None = None
    for name in base.TERMINATION_SIGNALS:
        signal.signal(name, base.terminate_on_signal)
    with tempfile.TemporaryDirectory(prefix="android-microg-spoofing-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        environment = dict(os.environ)
        serial = options.serial or f"127.0.0.1:{options.adb_port}"
        log_path = scratch / "launcher.log"
        try:
            apks = apk_paths(options.probe_dir)
            if not options.serial:
                command = [sys.executable, str(base.LAUNCHER), "--instance", str(scratch / "instance"),
                           "--adb", adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                           "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
                if options.release:
                    command += ["--release", str(options.release)]
                if options.emulator:
                    command += ["--emulator", str(options.emulator)]
                launcher_environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
                with open(log_path, "wb") as log:
                    guest = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                             stderr=subprocess.STDOUT, env=launcher_environment)
            device = base.Device(adb, serial, environment)
            base.wait_booted(device, options.boot_timeout, guest)
            if options.mode == "regression":
                check_regression(device, apks)
            else:
                check_report(device, apks, options.expect)
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            if log_path.is_file():
                print("launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                      file=sys.stderr)
            return 1
        finally:
            base.ignore_termination()
            if guest is not None:
                base.stop_launcher(guest)
                # The shared server stays up; only the guest this run launched is dropped. A guest named by
                # --serial is someone else's and stays connected.
                subprocess.run([adb, "disconnect", serial], env=environment, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
