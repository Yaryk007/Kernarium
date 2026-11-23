#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The installation identity runner's reading of the guest and of the host's file.

The runner (tests/integration/android/run_android_installation_identity.py)
compares what the guest reports with `identity.json` beside the data disk. The
values here are synthetic: a test never holds a real installation's.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_installation_identity.py"
SPEC = importlib.util.spec_from_file_location("run_android_installation_identity", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

SERIAL = "0123456789abcdef"
ETHERNET = "02:aa:bb:cc:dd:ee"
OTHER_ETHERNET = "02:aa:bb:cc:dd:ef"


def listing(address: str) -> str:
    """What `ip addr show eth0` prints, with the other lines a real one has."""
    return (
        "9: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UNKNOWN group default qlen 1000\n"
        f"    link/ether {address.upper()} brd ff:ff:ff:ff:ff:ff\n"
        "    inet 10.0.2.15/24 brd 10.0.2.255 scope global eth0\n"
        "       valid_lft forever preferred_lft forever\n")


class FakeDevice:
    def __init__(self, serial: str = SERIAL, boot_serial: str = SERIAL, address: str = ETHERNET) -> None:
        self.properties = {"ro.serialno": serial, "ro.boot.serialno": boot_serial}
        self.address = address
        self.command_line = f"quiet androidboot.serialno={boot_serial} console=ttyS0"

    def getprop(self, name: str) -> str:
        return self.properties[name]

    def shell(self, command: str, timeout: int = 30) -> str:
        if command == "ip addr show eth0":
            return listing(self.address).strip()
        if command == "cat /proc/cmdline":
            return self.command_line
        raise runner.Failure(f"'{command}' exited 1")


class InstanceDirectory:
    def __init__(self, test: unittest.TestCase) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        test.addCleanup(self.scratch.cleanup)
        self.path = Path(self.scratch.name)

    def write(self, members: dict | None = None, mode: int = 0o600) -> Path:
        members = members if members is not None else {
            "schema": 1, "serialno": SERIAL, "ethernet_mac": ETHERNET}
        file = self.path / runner.IDENTITY_FILE
        file.write_text(json.dumps(members))
        os.chmod(file, mode)
        return file


class EthernetAddressTests(unittest.TestCase):
    def test_the_address_is_the_one_on_eth0s_ether_line_in_lower_case(self) -> None:
        self.assertEqual(runner.ethernet_address(FakeDevice(address="02:AA:BB:CC:DD:EE")), ETHERNET)

    def test_a_listing_without_an_address_fails(self) -> None:
        device = FakeDevice()
        device.shell = lambda command, timeout=30: "9: eth0: <UP>\n    inet 10.0.2.15/24"  # type: ignore[method-assign]
        with self.assertRaises(runner.Failure):
            runner.ethernet_address(device)


class IdentityFileTests(unittest.TestCase):
    def test_a_private_file_of_schema_one_is_read(self) -> None:
        directory = InstanceDirectory(self)
        directory.write()
        content, members = runner.read_identity_file(directory.path)
        self.assertEqual((members["serialno"], members["ethernet_mac"]), (SERIAL, ETHERNET))
        self.assertEqual(json.loads(content), members)

    def test_what_the_runner_refuses(self) -> None:
        refused = {
            "no file": None,
            "readable by the group": (None, 0o640),
            "readable by everyone": (None, 0o644),
            "another schema": ({"schema": 2, "serialno": SERIAL, "ethernet_mac": ETHERNET}, 0o600),
            "another member": ({"schema": 1, "serialno": SERIAL, "ethernet_mac": ETHERNET, "x": 1}, 0o600),
            "a missing member": ({"schema": 1, "serialno": SERIAL}, 0o600),
        }
        for name, case in refused.items():
            with self.subTest(name):
                directory = InstanceDirectory(self)
                if case is not None:
                    members, mode = case
                    directory.write(members, mode)
                with self.assertRaises(runner.Failure):
                    runner.read_identity_file(directory.path)

    def test_a_file_that_is_not_json_is_refused(self) -> None:
        directory = InstanceDirectory(self)
        file = directory.write()
        file.write_text("{")
        with self.assertRaises(runner.Failure):
            runner.read_identity_file(directory.path)


class GuestMatchesFileTests(unittest.TestCase):
    def check(self, device: FakeDevice, members: dict | None = None) -> runner.Identity:
        directory = InstanceDirectory(self)
        directory.write(members)
        return runner.check_guest_matches_file(device, directory.path)

    def test_a_guest_that_reports_the_files_values_passes(self) -> None:
        identity = self.check(FakeDevice())
        self.assertEqual((identity.serial, identity.boot_serial, identity.ethernet), (SERIAL, SERIAL, ETHERNET))

    def test_every_difference_fails(self) -> None:
        cases = {
            "ro.serialno": FakeDevice(serial="fedcba9876543210"),
            "ro.boot.serialno": FakeDevice(boot_serial="fedcba9876543210"),
            "eth0's address": FakeDevice(address=OTHER_ETHERNET),
        }
        for name, command_line in {
            "no serial number on the command line": "quiet console=ttyS0",
            "the release's serial number beside the installation's":
                f"androidboot.serialno={runner.LEGACY_SERIAL} androidboot.serialno={SERIAL}",
            "another serial number on the command line": "androidboot.serialno=fedcba9876543210",
            "the serial number twice": f"androidboot.serialno={SERIAL} androidboot.serialno={SERIAL}",
        }.items():
            device = FakeDevice()
            device.command_line = command_line
            cases[name] = device
        for name, device in cases.items():
            with self.subTest(name), self.assertRaises(runner.Failure):
                self.check(device)

    def test_the_releases_own_values_fail_even_when_the_file_holds_them(self) -> None:
        legacy_serial = {"schema": 1, "serialno": runner.LEGACY_SERIAL, "ethernet_mac": ETHERNET}
        with self.assertRaises(runner.Failure):
            self.check(FakeDevice(serial=runner.LEGACY_SERIAL, boot_serial=runner.LEGACY_SERIAL), legacy_serial)
        default_address = {"schema": 1, "serialno": SERIAL, "ethernet_mac": runner.DEFAULT_ETHERNET_MAC}
        with self.assertRaises(runner.Failure):
            self.check(FakeDevice(address=runner.DEFAULT_ETHERNET_MAC), default_address)

    def test_identities_compare_on_all_three_values(self) -> None:
        base = runner.Identity(SERIAL, SERIAL, ETHERNET)
        self.assertTrue(base.same_as(runner.Identity(SERIAL, SERIAL, ETHERNET)))
        self.assertFalse(base.same_as(runner.Identity("fedcba9876543210", SERIAL, ETHERNET)))
        self.assertFalse(base.same_as(runner.Identity(SERIAL, "fedcba9876543210", ETHERNET)))
        self.assertFalse(base.same_as(runner.Identity(SERIAL, SERIAL, OTHER_ETHERNET)))


class MaskTests(unittest.TestCase):
    def test_serial_numbers_and_addresses_are_masked_wherever_they_stand(self) -> None:
        text = ("Kernel command line: quiet androidboot.serialno=feedface12345678 console=ttyS0\n"
                "[ro.serialno]: [feedface12345678]\n"
                "    link/ether 02:AA:BB:CC:DD:EE brd ff:ff:ff:ff:ff:ff\n"
                "mac 02-aa-bb-cc-dd-ef\n")
        masked = runner.mask(text)
        for value in ("feedface12345678", "02:AA:BB:CC:DD:EE", "02-aa-bb-cc-dd-ef", "ff:ff:ff:ff:ff:ff"):
            self.assertNotIn(value, masked)
        self.assertIn("quiet", masked)
        self.assertIn("console=ttyS0", masked)

    def test_a_value_the_runner_knows_is_masked_where_no_pattern_finds_it(self) -> None:
        masked = runner.mask("Build.SERIAL is abcd1234efgh5678 and so", ("abcd1234efgh5678", ""))
        self.assertNotIn("abcd1234efgh5678", masked)
        self.assertIn("Build.SERIAL is", masked)

    def test_the_values_of_the_identity_files_are_known(self) -> None:
        directory = InstanceDirectory(self)
        directory.write()
        known = runner.known_values([directory.path, directory.path / "absent"])
        self.assertEqual(set(known), {SERIAL, ETHERNET})


class FakeProcess:
    def __init__(self) -> None:
        self.alive = True
        self.signals = 0

    def poll(self):
        return None if self.alive else 0

    def send_signal(self, _signal: int) -> None:
        self.signals += 1
        self.alive = False

    def wait(self, timeout: float = 0) -> int:
        return 0

    def kill(self) -> None:
        self.alive = False


class RunnerLifecycleTests(unittest.TestCase):
    """main() with the guests replaced: what it prints on a failure and what it stops."""

    def run_main(self, guest_class) -> tuple[int | None, str, bool]:
        escaped = False
        output = io.StringIO()
        result = None
        with mock.patch.object(runner, "Guest", guest_class), \
                mock.patch.object(sys, "argv", [str(SCRIPT)]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                result = runner.main()
            except subprocess.TimeoutExpired:
                escaped = True
        return result, output.getvalue(), escaped

    def test_a_failure_prints_a_log_tail_with_no_serial_number_or_address(self) -> None:
        serial, address = "feedface12345678", "02:44:55:66:77:88"

        class Failing(runner.Guest):
            def __init__(self, options, instance, log) -> None:
                super().__init__(options, instance, log)

            def start(self) -> None:
                self.instance.mkdir(parents=True, exist_ok=True)
                (self.instance / runner.IDENTITY_FILE).write_text(json.dumps(
                    {"schema": 1, "serialno": "abcd1234efgh5678", "ethernet_mac": "02:11:22:33:44:99"}))
                self.log.write_text(
                    f"Kernel command line: androidboot.serialno={serial}\nlink/ether {address}\n"
                    "Build.SERIAL abcd1234efgh5678 on 02:11:22:33:44:99\n")
                raise runner.Failure(f"ro.serialno {serial} is not the file's")

            def stop(self) -> None:
                pass

        result, text, _ = self.run_main(Failing)
        self.assertEqual(result, 1)
        self.assertIn("FAIL:", text)
        for value in (serial, address, "abcd1234efgh5678", "02:11:22:33:44:99"):
            self.assertNotIn(value, text)
        self.assertIn("launcher log tail", text)
        self.assertIn("androidboot.serialno=", text)

    def test_every_guest_is_stopped_when_an_earlier_disconnect_times_out(self) -> None:
        made: list = []

        class Device:
            def __init__(self, number: int) -> None:
                self.number = number
                self.serial = "synthetic-endpoint"
                self.disconnects = 0

            def run(self, *arguments: str, **keywords) -> subprocess.CompletedProcess:
                self.disconnects += 1
                if self.number == 0 and self.disconnects == 3:
                    raise subprocess.TimeoutExpired(["adb", "disconnect"], 30)
                return subprocess.CompletedProcess([], 0, "")

        class Guest(runner.Guest):
            def __init__(self, options, instance, log) -> None:
                super().__init__(options, instance, log)
                self.number = len(made)
                self.device = Device(self.number)
                made.append(self)

            def start(self) -> None:
                self.connected = True
                self.process = self.child = FakeProcess()

            def reboot(self) -> None:
                pass

        with mock.patch.object(runner, "check_guest_matches_file", lambda device, _instance: runner.Identity(
                    "first-serial" if device.number == 0 else "second-serial", "unused",
                    "02:11:22:33:44:55" if device.number == 0 else "06:11:22:33:44:55")), \
                mock.patch.object(runner, "check_network", lambda _device: None), \
                mock.patch.object(runner, "report_link_local", lambda _device: {}), \
                mock.patch.object(runner, "read_identity_file", lambda _instance: (b"unchanged", {})):
            result, text, escaped = self.run_main(Guest)
        self.assertFalse(escaped, "a disconnect timeout escaped the cleanup")
        self.assertEqual(result, 0, text)
        self.assertEqual(len(made), 2)
        self.assertGreater(made[1].child.signals, 0, "the second guest was never stopped")
        self.assertFalse(made[1].child.alive)

    def test_a_guest_stops_even_when_adb_does_not_answer_the_disconnect(self) -> None:
        class SilentDevice:
            serial = "synthetic-endpoint"

            def run(self, *arguments: str, **keywords) -> subprocess.CompletedProcess:
                raise subprocess.TimeoutExpired(["adb", "disconnect"], 30)

        options = runner.build_parser().parse_args([])
        guest = runner.Guest(options, Path("unused"), Path("unused.log"))
        guest.device = SilentDevice()  # type: ignore[assignment]
        process = FakeProcess()
        guest.process = process  # type: ignore[assignment]
        guest.stop()
        self.assertFalse(process.alive, "the launcher was not stopped")
        self.assertIsNone(guest.process)
        guest.stop()  # a guest that is stopped has nothing to stop or disconnect

    def test_a_launcher_that_will_not_stop_does_not_keep_the_others_running(self) -> None:
        stopped: list[str] = []

        class Stubborn:
            def stop(self) -> None:
                raise subprocess.TimeoutExpired(["launcher"], 30)

        class Fine:
            def stop(self) -> None:
                stopped.append("fine")

        with contextlib.redirect_stderr(io.StringIO()):
            runner.stop_all([Stubborn(), Fine()])  # type: ignore[list-item]
        self.assertEqual(stopped, ["fine"])


HEADER_DIRECTORY = Path(__file__).resolve().parents[3] / "android/lineage-24/overlay/tools"
DRIVER = r"""
#include <stdio.h>
#include "kernarium_wifi_mac.h"
int main() {
  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    unsigned v[6];
    if (sscanf(line, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 2;
    kernarium_net::Mac ethernet = {};
    for (int i = 0; i < 6; ++i) ethernet[i] = static_cast<unsigned char>(v[i]);
    const kernarium_net::Mac wifi = kernarium_net::DeriveWifiMac(ethernet);
    const kernarium_net::Mac back = kernarium_net::EthernetFromWifiMac(wifi);
    printf("%02x:%02x:%02x:%02x:%02x:%02x %02x:%02x:%02x:%02x:%02x:%02x\n",
           wifi[0], wifi[1], wifi[2], wifi[3], wifi[4], wifi[5],
           back[0], back[1], back[2], back[3], back[4], back[5]);
  }
  return 0;
}
"""


def reserved_addresses() -> list[int]:
    return [0x020000000000] + [0x52550A000200 | last for last in range(256)]


def is_host_address(mac: str) -> bool:
    """An address the host can give eth0: locally administered unicast and not reserved."""
    packed = int(mac.replace(":", ""), 16)
    return packed >> 40 & 0x03 == 0x02 and packed not in reserved_addresses()


def random_macs(count: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    return [runner.format_mac(runner.mac_from_bits(rng.getrandbits(46))) for _ in range(count)]


class WifiDerivationTests(unittest.TestCase):
    def test_the_port_gives_the_golden_vectors_the_cpp_gives(self) -> None:
        vectors = runner.golden_vectors()
        self.assertGreaterEqual(len(vectors), 1000)
        for ethernet, wifi in vectors:
            self.assertEqual(runner.derive_wifi_mac(ethernet), wifi, ethernet)
            if is_host_address(ethernet):
                self.assertEqual(runner.ethernet_from_wifi_mac(wifi), ethernet, wifi)

    def test_every_result_is_a_local_unicast_address_that_is_not_refused(self) -> None:
        for ethernet in ["52:54:00:12:34:56", "00:00:00:00:00:00", ETHERNET, OTHER_ETHERNET,
                         "02:15:b2:00:00:00", "52:55:0a:00:02:02"] + random_macs(2000, 1):
            wifi = runner.derive_wifi_mac(ethernet)
            self.assertEqual(int(wifi[:2], 16) & 0x03, 0x02, ethernet)
            self.assertNotEqual(wifi, ethernet)
            self.assertNotIn(int(wifi.replace(":", ""), 16), reserved_addresses(), ethernet)

    def test_the_review_pair_gives_two_addresses(self) -> None:
        # A hash gave these two eth0 addresses one Wi-Fi address.
        self.assertNotEqual(runner.derive_wifi_mac("0a:11:22:33:44:56"), runner.derive_wifi_mac("62:a7:34:c8:24:f6"))

    def test_the_inverse_round_trips_and_the_permutation_is_one_to_one(self) -> None:
        for ethernet in random_macs(3000, 2):
            self.assertEqual(runner.ethernet_from_wifi_mac(runner.derive_wifi_mac(ethernet)), ethernet)
        rng = random.Random(3)
        for _ in range(3000):
            bits = rng.getrandbits(runner.WIFI_BITS)
            self.assertEqual(runner.wifi_unpermute(runner.wifi_permute(bits)), bits)
        self.assertEqual(len({runner.derive_wifi_mac(ethernet) for ethernet in random_macs(20000, 4)}), 20000)

    def test_a_reserved_address_is_walked_past_with_at_most_the_stated_bound(self) -> None:
        reserved = reserved_addresses()
        self.assertEqual(len(reserved), 257)
        self.assertEqual(runner.WIFI_MAX_STEPS, len(reserved) + 1)
        for packed in reserved:
            # The eth0 address whose first step lands on the reserved one.
            position = runner.wifi_permute(runner.mac_bits(runner.format_mac(packed)))
            ethernet = runner.format_mac(runner.mac_from_bits(
                runner.wifi_unpermute((position - 1) % runner.WIFI_VALUES)))
            wifi = int(runner.derive_wifi_mac(ethernet).replace(":", ""), 16)
            self.assertNotEqual(wifi, packed)
            self.assertNotIn(wifi, reserved)
            expected = next(candidate for step in range(2, runner.WIFI_MAX_STEPS + 1)
                            for candidate in [runner.mac_from_bits(
                                runner.wifi_unpermute((position - 1 + step) % runner.WIFI_VALUES))]
                            if candidate not in reserved)
            self.assertEqual(wifi, expected)

    def test_what_the_host_never_generates_has_the_documented_result(self) -> None:
        # The flag bits are ignored: the broadcast address gives the result of its local twin.
        self.assertEqual(runner.derive_wifi_mac("ff:ff:ff:ff:ff:ff"), runner.derive_wifi_mac("fe:ff:ff:ff:ff:ff"))
        self.assertEqual(runner.derive_wifi_mac("01:aa:bb:cc:dd:ee"), runner.derive_wifi_mac("02:aa:bb:cc:dd:ee"))
        # A reserved input gives the result of the unreserved address before it on the cycle.
        for packed in reserved_addresses()[:20]:
            before = runner.wifi_unpermute((runner.wifi_permute(runner.mac_bits(runner.format_mac(packed))) - 1)
                                           % runner.WIFI_VALUES)
            if runner.mac_from_bits(before) in reserved_addresses():
                continue
            self.assertEqual(runner.derive_wifi_mac(runner.format_mac(packed)),
                             runner.derive_wifi_mac(runner.format_mac(runner.mac_from_bits(before))))

    @unittest.skipUnless(shutil.which("c++"), "no C++ compiler")
    def test_a_differential_run_of_the_header_agrees_with_the_port(self) -> None:
        macs = random_macs(5000, 5) + [
            "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "01:23:45:67:89:ab", "02:00:00:00:00:00",
            "52:55:0a:00:02:02", "52:55:0a:00:02:ff", "02:15:b2:00:00:00", "0a:11:22:33:44:56"]
        rng = random.Random(6)
        macs += [runner.format_mac(rng.getrandbits(48)) for _ in range(2000)]  # flag bits included
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "driver.cpp"
            binary = Path(directory) / "driver"
            source.write_text(DRIVER)
            subprocess.run(["c++", "-std=c++20", "-O1", f"-I{HEADER_DIRECTORY}", str(source), "-o", str(binary)],
                           check=True, capture_output=True)
            output = subprocess.run([str(binary)], input="\n".join(macs) + "\n", text=True, check=True,
                                    capture_output=True).stdout.splitlines()
        self.assertEqual(len(output), len(macs))
        for ethernet, line in zip(macs, output):
            wifi, back = line.split()
            self.assertEqual(wifi, runner.derive_wifi_mac(ethernet), ethernet)
            self.assertEqual(back, runner.ethernet_from_wifi_mac(wifi), ethernet)
            if is_host_address(ethernet):
                self.assertEqual(back, ethernet)


WLAN0 = (
    "10: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc noqueue state UP group default qlen 1000\n"
    "    link/ether {address} brd ff:ff:ff:ff:ff:ff\n"
    "{inet}")
INET = "    inet 10.0.2.16/24 brd 10.0.2.255 scope global wlan0\n"
LINK_LOCAL_LINE = "    inet6 fe80::1234:5678:9abc:def0/64 scope link\n"
APP_CONTEXT = "u:r:untrusted_app_29:s0:c123,c256,c512,c768"


def full_listing(link_local: dict[str, bool]) -> str:
    """What `ip addr show` prints for the interfaces the runner looks at, with synthetic MACs."""
    lines = ["1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536\n    inet 127.0.0.1/8 scope host lo\n"]
    for number, name in enumerate(("eth0", "buried_eth0", "wlan0"), start=2):
        lines.append(f"{number}: {name}: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500\n"
                     "    link/ether 02:00:00:00:00:01 brd ff:ff:ff:ff:ff:ff\n"
                     + (LINK_LOCAL_LINE if link_local.get(name) else ""))
    return "".join(lines).strip()


class ImageDevice(FakeDevice):
    """A guest that carries the newer image: wlan0, the lower devices, the probe application and the
    kernel's command line."""

    def __init__(self, probes, wifi: str | None = None, inet: bool = True, key_visible: bool = False,
                 control_visible: bool = True, cmdline_has_key: bool = True, installed: bool = True,
                 install_works: bool = True, context: str = APP_CONTEXT, su: str = "root",
                 root_link_local: dict[str, bool] | None = None, app_links: dict[str, bool] | None = None,
                 broadcast_works: bool = True, writes_result: bool = True, reboots: bool = False) -> None:
        super().__init__()
        self.probes = probes
        self.wifi = wifi if wifi is not None else runner.derive_wifi_mac(ETHERNET)
        self.inet = inet
        self.key_visible = key_visible
        self.control_visible = control_visible
        self.cmdline_has_key = cmdline_has_key
        self.installed = installed
        self.install_works = install_works
        self.context = context
        self.su = su  # "root": root reads the key; "none": the image has no su; "dropped": root reads nothing
        self.root_link_local = root_link_local or {"wlan0": True}
        self.app_links = app_links if app_links is not None else {
            "dummy0": True, "eth0": False, "wlan0": True, "lo": False, "buried_eth0": False}
        self.broadcast_works = broadcast_works
        self.writes_result = writes_result
        self.reboots = reboots
        self.boots = 0
        self.serial = "127.0.0.1:1"
        self.commands: list[str] = []

    def result_text(self) -> str:
        links = " ".join(f"{name}={'yes' if found else 'no'}" for name, found in self.app_links.items()) + " "
        return "\n".join([
            f"context={self.context}", f"links={links}",
            f"control={'readable' if self.control_visible else 'empty'}",
            f"key={'readable' if self.key_visible else 'empty'}", "done"])

    def shell(self, command: str, timeout: int = 30) -> str:
        self.commands.append(command)
        package = runner.property_probe_apk.PACKAGE
        result_file = runner.property_probe_apk.RESULT_FILE
        if command == "ip addr show wlan0":
            return WLAN0.format(address=self.wifi.upper(), inet=INET if self.inet else "").strip()
        if command == "ip addr show":
            return full_listing(self.root_link_local)
        if command == "ip -4 addr show dev wlan0":
            return INET if self.inet else "10: wlan0: <UP>"
        if command == "cat /proc/cmdline":
            key = f" androidboot.kernarium.adb_key={self.probes.key}" if self.cmdline_has_key else ""
            return "console=ttyS0" + key
        if command == "cat /proc/sys/kernel/random/boot_id":
            self.boots += 1
            return f"boot-{self.boots if self.reboots else 0}"
        if command == f"pm path {package} || true":
            return f"package:/data/app/{package}/base.apk" if self.installed else ""
        if command == f"run-as {package} rm -f {result_file}":
            return ""
        if command == f"am broadcast -n {runner.property_probe_apk.COMPONENT}":
            return "Broadcast completed: result=0" if self.broadcast_works else "Error: no such component"
        if command == f"run-as {package} cat {result_file}":
            if not self.writes_result:
                raise runner.Failure(f"'{command}' exited 1")
            return self.result_text()
        if command == f"su 0 getprop {runner.KEY_PROPERTY}":
            if self.su == "none":
                raise runner.Failure(f"'{command}' exited 127")
            return self.probes.key if self.su == "root" else ""
        return super().shell(command, timeout)

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        self.commands.append(" ".join(arguments))
        return subprocess.CompletedProcess(arguments, 0 if self.install_works else 1,
                                           "Success\n" if self.install_works else "Failure\n")


OPCODE_UNITS = {0x0E: 1, 0x12: 1, 0x0A: 1, 0x0C: 1, 0x1A: 2, 0x22: 2, 0x1F: 2, 0x20: 2, 0x23: 2, 0x38: 2, 0x29: 2,
                0x4D: 2, 0x6E: 3, 0x70: 3, 0x71: 3, 0x72: 3}
TYPE_INDEXED = {0x22, 0x1F}  # new-instance, check-cast (21c); 0x20 and 0x23 are 22c
METHOD_INDEXED = {0x6E, 0x70, 0x71, 0x72}


def uleb128(data: bytes, offset: int) -> tuple[int, int]:
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, offset


class DexReader:
    """An independent reader of the DEX the probe's code is assembled into: it checks the layout
    rules (sorted tables, alignment, the map, the digests) and walks the instructions."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        (self.magic, self.checksum, self.signature, self.file_size, self.header_size, self.endian,
         self.link_size, self.link_off, self.map_off) = struct.unpack_from("<8sI20sIIIIII", data, 0)
        names = ("string_ids", "type_ids", "proto_ids", "field_ids", "method_ids", "class_defs", "data")
        self.sections = {}
        for number, name in enumerate(names):
            self.sections[name] = struct.unpack_from("<II", data, 0x38 + 8 * number)
        count, offset = self.sections["string_ids"]
        self.strings = []
        for index in range(count):
            (position,) = struct.unpack_from("<I", data, offset + 4 * index)
            size, position = uleb128(data, position)
            end = data.index(b"\0", position)
            self.strings.append(data[position:end])
            assert size == len(data[position:end].decode("utf-8")), "utf16_size"
        count, offset = self.sections["type_ids"]
        self.types = [struct.unpack_from("<I", data, offset + 4 * i)[0] for i in range(count)]
        count, offset = self.sections["proto_ids"]
        self.protos = [struct.unpack_from("<III", data, offset + 12 * i) for i in range(count)]
        count, offset = self.sections["method_ids"]
        self.methods = [struct.unpack_from("<HHI", data, offset + 8 * i) for i in range(count)]
        count, offset = self.sections["class_defs"]
        self.class_defs = [struct.unpack_from("<IIIIIIII", data, offset + 32 * i) for i in range(count)]

    def descriptor(self, type_index: int) -> str:
        return self.strings[self.types[type_index]].decode()

    def type_list(self, offset: int) -> list[int]:
        if not offset:
            return []
        (size,) = struct.unpack_from("<I", self.data, offset)
        return list(struct.unpack_from(f"<{size}H", self.data, offset + 4))

    def method_name(self, index: int) -> str:
        class_index, proto_index, name_index = self.methods[index]
        _, return_type, parameters = self.protos[proto_index]
        arguments = "".join(self.descriptor(item) for item in self.type_list(parameters))
        return (f"{self.descriptor(class_index)}.{self.strings[name_index].decode()}"
                f"({arguments}){self.descriptor(return_type)}")

    def code(self, offset: int) -> tuple[tuple[int, int, int], list[int]]:
        registers, ins, outs, tries, debug, size = struct.unpack_from("<HHHHII", self.data, offset)
        assert tries == 0 and debug == 0
        return (registers, ins, outs), list(struct.unpack_from(f"<{size}H", self.data, offset + 16))

    def class_methods(self) -> dict[str, tuple[int, int]]:
        """Method name to (access flags, code offset), for the one class."""
        offset = self.class_defs[0][6]
        sizes = []
        for _ in range(4):
            value, offset = uleb128(self.data, offset)
            sizes.append(value)
        found = {}
        for count in sizes[2:]:
            index = 0
            for _ in range(count):
                step, offset = uleb128(self.data, offset)
                access, offset = uleb128(self.data, offset)
                code, offset = uleb128(self.data, offset)
                index += step
                found[self.method_name(index)] = (access, code)
        return found


class PropertyProbeApplicationTests(unittest.TestCase):
    PACKAGE = runner.property_probe_apk.PACKAGE

    def dex(self) -> DexReader:
        return DexReader(runner.property_probe_apk.property_probe_dex.build_dex(
            self.PACKAGE, runner.CONTROL_PROPERTY, runner.KEY_PROPERTY))

    def test_the_layout_follows_the_formats_rules(self) -> None:
        reader = self.dex()
        data = reader.data
        self.assertEqual(reader.magic, b"dex\n035\0")
        self.assertEqual((reader.file_size, reader.header_size, reader.endian), (len(data), 0x70, 0x12345678))
        self.assertEqual(reader.checksum, zlib.adler32(data[12:]) & 0xFFFFFFFF)
        self.assertEqual(reader.signature, hashlib.sha1(data[32:]).digest())
        self.assertEqual(reader.strings, sorted(set(reader.strings)))
        self.assertEqual(reader.types, sorted(set(reader.types)))
        self.assertEqual(reader.sections["field_ids"][0], 0)
        protos = [(ret, reader.type_list(params)) for _, ret, params in reader.protos]
        self.assertEqual(protos, sorted(protos))
        self.assertEqual(len({(ret, tuple(params)) for ret, params in protos}), len(protos))
        for _, proto, name in reader.methods:
            self.assertLess(proto, len(reader.protos))
            self.assertLess(name, len(reader.strings))
        self.assertEqual(reader.methods, sorted(reader.methods, key=lambda m: (m[0], m[2], m[1])))
        self.assertEqual(len(set(reader.methods)), len(reader.methods))
        for shorty, return_type, parameters in reader.protos:
            expected = "".join("L" if reader.descriptor(i)[0] in "L[" else reader.descriptor(i)
                               for i in [return_type, *reader.type_list(parameters)])
            self.assertEqual(reader.strings[shorty].decode(), expected)
        self.assertEqual(reader.sections["data"][1] % 4, 0)
        self.assertEqual(reader.sections["data"][0] % 4, 0)
        self.assertEqual(reader.sections["data"][1] + reader.sections["data"][0], len(data))

    def test_the_map_lists_every_section_in_offset_order(self) -> None:
        reader = self.dex()
        (count,) = struct.unpack_from("<I", reader.data, reader.map_off)
        entries = [struct.unpack_from("<HHII", reader.data, reader.map_off + 4 + 12 * i) for i in range(count)]
        self.assertEqual(reader.map_off % 4, 0)
        self.assertEqual([offset for _, _, _, offset in entries], sorted(offset for _, _, _, offset in entries))
        kinds = {kind: (size, offset) for kind, _, size, offset in entries}
        self.assertEqual(kinds[0x0000], (1, 0))
        self.assertEqual(kinds[0x0001], (len(reader.strings), reader.sections["string_ids"][1]))
        self.assertEqual(kinds[0x0002], (len(reader.types), reader.sections["type_ids"][1]))
        self.assertEqual(kinds[0x0003], (len(reader.protos), reader.sections["proto_ids"][1]))
        self.assertEqual(kinds[0x0005], (len(reader.methods), reader.sections["method_ids"][1]))
        self.assertEqual(kinds[0x0006], (1, reader.sections["class_defs"][1]))
        self.assertEqual(kinds[0x1000], (1, reader.map_off))
        self.assertEqual(kinds[0x2000][0], 1)
        self.assertEqual(kinds[0x2001][0], 2)
        self.assertEqual(kinds[0x2002][0], len(reader.strings))
        self.assertEqual(kinds[0x2001][1] % 4, 0)
        self.assertEqual(kinds[0x1001][1] % 4, 0)

    def test_the_class_is_a_broadcast_receiver_with_a_constructor_and_onreceive(self) -> None:
        reader = self.dex()
        _, flags, superclass, interfaces, source, annotations, class_data, static = reader.class_defs[0]
        self.assertEqual(reader.descriptor(reader.class_defs[0][0]), "Lorg/example/andarium/propertyprobe/Probe;")
        self.assertEqual(reader.descriptor(superclass), "Landroid/content/BroadcastReceiver;")
        self.assertEqual((flags, interfaces, source, annotations, static), (1, 0, 0xFFFFFFFF, 0, 0))
        methods = reader.class_methods()
        prefix = "Lorg/example/andarium/propertyprobe/Probe;."
        self.assertEqual(sorted(methods), [prefix + "<init>()V",
                                           prefix + "onReceive(Landroid/content/Context;Landroid/content/Intent;)V"])
        self.assertEqual(methods[prefix + "<init>()V"][0], 0x10001)
        self.assertEqual(methods[prefix + "onReceive(Landroid/content/Context;Landroid/content/Intent;)V"][0], 1)

    def test_every_instruction_is_whole_and_every_index_and_branch_lands(self) -> None:
        reader = self.dex()
        for name, (_, offset) in reader.class_methods().items():
            (registers, ins, outs), units = reader.code(offset)
            self.assertLessEqual(ins, registers, name)
            position, starts, branches = 0, set(), []
            while position < len(units):
                starts.add(position)
                opcode = units[position] & 0xFF
                self.assertIn(opcode, OPCODE_UNITS, f"{name}: opcode {opcode:#x}")
                width = OPCODE_UNITS[opcode]
                if opcode == 0x1A:
                    self.assertLess(units[position + 1], len(reader.strings))
                elif opcode in TYPE_INDEXED or opcode in (0x20, 0x23):
                    self.assertLess(units[position + 1], len(reader.types))
                elif opcode in METHOD_INDEXED:
                    self.assertLess(units[position + 1], len(reader.methods))
                    count = units[position] >> 12
                    self.assertLessEqual(count, outs if name.endswith("onReceive(Landroid/content/Context;Landroid/content/Intent;)V") else 1)
                    for number in range(count):
                        # the registers of the call (4 bits each, C D E F then G)
                        packed = units[position + 2] | (units[position] >> 8 & 0xF) << 16
                        self.assertLess((packed >> 4 * number) & 0xF, registers)
                elif opcode in (0x38, 0x29):
                    offset_signed = units[position + 1] - 0x10000 if units[position + 1] & 0x8000 else units[position + 1]
                    branches.append(position + offset_signed)
                position += width
            self.assertEqual(position, len(units), f"{name}: the instructions end inside an instruction")
            for target in branches:
                self.assertIn(target, starts, f"{name}: a branch lands inside an instruction")
            self.assertEqual(units[-1] & 0xFF, 0x0E, f"{name} does not end with return-void")

    def test_the_script_runs_as_the_application_and_never_writes_the_key(self) -> None:
        text = runner.property_probe_apk.property_probe_dex.script(self.PACKAGE, runner.CONTROL_PROPERTY, runner.KEY_PROPERTY)
        self.assertIn(f"/data/data/{self.PACKAGE}/files", text)
        self.assertIn(f"getprop {runner.CONTROL_PROPERTY}", text)
        self.assertIn(f"getprop {runner.KEY_PROPERTY}", text)
        self.assertIn("id -Z", text)
        self.assertIn("key=readable", text)
        # The key's value is tested for being non-empty, never echoed.
        self.assertNotRegex(text, rf"echo[^;]*\$\(getprop {runner.KEY_PROPERTY}\)")
        self.assertIn(text.replace("\\", ""), self.dex().data.decode("latin-1"))

    def test_the_manifest_declares_a_debuggable_application_with_the_internet_permission_and_one_receiver(self) -> None:
        import orientation_probe_apk as binary_xml
        tree = binary_xml.decode_manifest(binary_xml.encode_manifest(runner.property_probe_apk.manifest_tree()))
        self.assertEqual(tree.attributes["package"], self.PACKAGE)
        sdk, permission, application = tree.children
        self.assertEqual(sdk.attributes["targetSdkVersion"], binary_xml.TARGET_SDK)
        self.assertEqual(permission.attributes["name"], "android.permission.INTERNET")
        self.assertEqual(application.tag, "application")
        self.assertIs(application.attributes["debuggable"], True)
        self.assertNotIn("hasCode", application.attributes)
        (receiver,) = application.children
        self.assertEqual((receiver.tag, receiver.attributes["name"], receiver.attributes["exported"]),
                         ("receiver", runner.property_probe_apk.RECEIVER, True))
        self.assertEqual(receiver.children, [])
        self.assertEqual(runner.property_probe_apk.COMPONENT, f"{self.PACKAGE}/{runner.property_probe_apk.RECEIVER}")

    @unittest.skipUnless(shutil.which("keytool") and shutil.which("jarsigner"), "needs a JDK")
    def test_the_package_holds_the_manifest_and_the_code_and_is_signed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            apk = runner.property_probe_apk.build_apk(Path(directory) / "probe.apk", runner.CONTROL_PROPERTY,
                                                      runner.KEY_PROPERTY, Path(directory))
            with zipfile.ZipFile(apk) as package:
                names = package.namelist()
                self.assertIn("AndroidManifest.xml", names)
                self.assertIn("classes.dex", names)
                self.assertEqual(package.read("classes.dex"), self.dex().data)
            result = subprocess.run(["jarsigner", "-verify", str(apk)], capture_output=True, text=True, check=False)
            self.assertIn("jar verified", result.stdout)


class ProbeResultTests(unittest.TestCase):
    def test_the_result_file_is_read_into_fields(self) -> None:
        result = runner.parse_probe_result(ImageDevice(None).result_text())
        self.assertTrue(result.complete)
        self.assertEqual(result.domain, "u:r:untrusted_app_29:s0")
        self.assertTrue(result.control)
        self.assertFalse(result.key)
        self.assertEqual(result.links, {"dummy0": True, "eth0": False, "wlan0": True, "lo": False, "buried_eth0": False})

    def test_a_result_without_its_last_line_is_not_complete(self) -> None:
        text = ImageDevice(None).result_text().replace("\ndone", "")
        self.assertFalse(runner.parse_probe_result(text).complete)


class ImageInputTests(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)
        patcher = mock.patch.object(runner.property_probe_apk, "build_apk", side_effect=lambda path, *names: path)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.probes = runner.Probes(self.scratch, Path("/path with space/emulator"))

    def check(self, device: ImageDevice) -> tuple[str, str]:
        """check_image's result and what it printed."""
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            wifi = runner.check_image(device, ETHERNET, self.probes)
        return wifi, output.getvalue()

    def test_the_wrapper_adds_the_probe_arguments_after_the_launchers_own(self) -> None:
        text = self.probes.emulator.read_text()
        self.assertIn("exec '/path with space/emulator' \"$@\" ", text)
        self.assertIn(f"androidboot.kernarium.adb_key={self.probes.key} ", text)
        self.assertIn(f"androidboot.kernarium.identity_probe={self.probes.control}\n", text)
        self.assertTrue(os.access(self.probes.emulator, os.X_OK))
        other = runner.Probes(self.scratch, Path("/emulator"))
        self.assertNotEqual((other.key, other.control), (self.probes.key, self.probes.control))

    def test_a_guest_of_the_new_image_passes_and_gives_its_wifi_address(self) -> None:
        wifi, printed = self.check(ImageDevice(self.probes))
        self.assertEqual(wifi, runner.derive_wifi_mac(ETHERNET))
        self.assertIn("ran as u:r:untrusted_app_29:s0", printed)
        self.assertIn("root reads the key", printed)
        self.assertNotIn("c123", printed)

    def test_an_image_without_su_passes_and_says_the_control_is_inferred(self) -> None:
        _, printed = self.check(ImageDevice(self.probes, su="none"))
        self.assertIn("inferred", printed)

    def test_the_probe_application_is_installed_once(self) -> None:
        device = ImageDevice(self.probes, installed=False)
        self.check(device)
        self.assertEqual(sum(1 for command in device.commands if command.startswith("-s ")), 1)
        device = ImageDevice(self.probes, installed=True)
        self.check(device)
        self.assertEqual(sum(1 for command in device.commands if command.startswith("-s ")), 0)

    def test_the_application_is_started_as_a_broadcast_receiver_not_run_as(self) -> None:
        device = ImageDevice(self.probes)
        self.check(device)
        package = runner.property_probe_apk.PACKAGE
        self.assertIn(f"am broadcast -n {runner.property_probe_apk.COMPONENT}", device.commands)
        self.assertFalse([command for command in device.commands if command.startswith(f"run-as {package} getprop")])

    def test_every_difference_fails(self) -> None:
        cases = {
            "another wlan0 address": ImageDevice(self.probes, wifi="02:aa:bb:cc:dd:01"),
            "the old fixed wlan0 address": ImageDevice(self.probes, wifi=runner.LEGACY_WIFI_MAC),
            "eth0's own address on wlan0": ImageDevice(self.probes, wifi=ETHERNET),
            "no lease": ImageDevice(self.probes, inet=False),
            "an application reads the key": ImageDevice(self.probes, key_visible=True),
            "an application cannot read the control": ImageDevice(self.probes, control_visible=False),
            "the kernel never had the key": ImageDevice(self.probes, cmdline_has_key=False),
            "root does not read the key": ImageDevice(self.probes, su="dropped"),
            "the probe will not install": ImageDevice(self.probes, installed=False, install_works=False),
            "the broadcast is not delivered": ImageDevice(self.probes, broadcast_works=False),
            "the probe writes no result": ImageDevice(self.probes, writes_result=False),
            "the probe is run-as, not an application": ImageDevice(self.probes, context="u:r:runas_app:s0:c1"),
            "the probe is shell": ImageDevice(self.probes, context="u:r:shell:s0"),
            "the boot changes while it runs": ImageDevice(self.probes, reboots=True),
            "eth0 has a link-local address": ImageDevice(self.probes, root_link_local={"eth0": True}),
            "buried_eth0 has a link-local address": ImageDevice(self.probes, root_link_local={"buried_eth0": True}),
            "an application sees eth0's": ImageDevice(self.probes, app_links={"eth0": True, "buried_eth0": False}),
            "an application sees buried_eth0's": ImageDevice(self.probes, app_links={"eth0": False, "buried_eth0": True}),
            "an application's listing lacks eth0, which holds an address": ImageDevice(
                self.probes, app_links={"wlan0": True, "buried_eth0": False}),
        }
        for label, device in cases.items():
            with self.subTest(label), mock.patch.object(runner, "LEASE_SECONDS", 1), \
                    mock.patch.object(runner, "PROBE_SECONDS", 1), \
                    mock.patch.object(runner.time, "sleep", lambda seconds: None):
                with self.assertRaises(runner.Failure):
                    self.check(device)

    def test_the_macvlan_without_an_address_is_not_in_the_applications_listing(self) -> None:
        # The listing names interfaces that hold an address: buried_eth0 holds none without IPv6.
        self.check(ImageDevice(self.probes, app_links={"dummy0": True, "eth0": False, "wlan0": True, "lo": False}))

    def test_wlan0_may_hold_a_link_local_address(self) -> None:
        self.check(ImageDevice(self.probes, root_link_local={"wlan0": True}, app_links={
            "eth0": False, "buried_eth0": False, "wlan0": True}))

    def test_a_failure_names_no_address_and_no_key(self) -> None:
        for device in (ImageDevice(self.probes, wifi="02:aa:bb:cc:dd:01"), ImageDevice(self.probes, key_visible=True)):
            with self.assertRaises(runner.Failure) as raised:
                self.check(device)
            message = str(raised.exception)
            for value in (ETHERNET, "02:aa:bb:cc:dd:01", runner.derive_wifi_mac(ETHERNET), self.probes.key,
                          self.probes.control):
                self.assertNotIn(value, message)

    def test_the_default_run_reports_each_interface_with_no_address(self) -> None:
        device = ImageDevice(self.probes, root_link_local={"eth0": True, "buried_eth0": True})
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            found = runner.report_link_local(device)
        self.assertEqual(found, {"eth0": True, "buried_eth0": True, "wlan0": False})
        self.assertEqual(output.getvalue().splitlines(), [
            "eth0: IPv6 link-local address in the guest's listing: yes",
            "buried_eth0: IPv6 link-local address in the guest's listing: yes",
            "wlan0: IPv6 link-local address in the guest's listing: no"])
        self.assertNotIn("fe80", output.getvalue())


class ImageFailureOutputTests(unittest.TestCase):
    """main() with --image-inputs: what a failure prints never holds the probe's key."""

    def test_the_probe_key_is_masked_in_the_message_and_the_log_tail(self) -> None:
        key = "probe" + "aaaabbbbccccdddd"

        class Failing(runner.Guest):
            def start(self) -> None:
                self.log.write_text(
                    f"emulator args: androidboot.kernarium.adb_key={key} androidboot.kernarium.identity_probe=aaaabbbbccccdddd\n"
                    f"[ro.boot.kernarium.adb_key]: [{key}]\nadb_key: {key}\nthe key {key} again\n")
                raise runner.Failure(f"an application can read ro.boot.kernarium.adb_key={key}")

            def stop(self) -> None:
                pass

        output = io.StringIO()
        with mock.patch.object(runner, "Guest", Failing), \
                mock.patch.object(runner.property_probe_apk, "build_apk", side_effect=lambda path, *names: path), \
                mock.patch.object(runner.secrets, "token_hex", return_value="aaaabbbbccccdddd"), \
                mock.patch.object(sys, "argv", [str(SCRIPT), "--image-inputs"]), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = runner.main()
        self.assertEqual(result, 1)
        text = output.getvalue()
        self.assertIn("FAIL:", text)
        self.assertNotIn(key, text)
        self.assertNotIn("aaaabbbbccccdddd", text)
        self.assertIn("launcher log tail", text)
        self.assertIn("adb_key=", text)


class MaskKeyTests(unittest.TestCase):
    def test_key_assignments_are_masked_in_every_form_and_the_property_name_is_not(self) -> None:
        text = ("androidboot.kernarium.adb_key=QUFBQUJC\n[ro.boot.kernarium.adb_key]: [QUFBQUJC]\n"
                "adb_key: QUFBQUJC\nan application can read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop\n")
        masked = runner.mask(text)
        self.assertNotIn("QUFBQUJC", masked)
        self.assertIn("an application can read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop", masked)

    def test_the_run_knows_the_probe_values(self) -> None:
        class Holder:
            key, control = "probekey0123", "ctl4567"
        self.assertEqual(set(runner.known_values([], Holder())), {"probekey0123", "ctl4567"})


if __name__ == "__main__":
    unittest.main()
