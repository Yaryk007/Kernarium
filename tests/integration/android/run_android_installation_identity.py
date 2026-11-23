#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the installation identity of an Android guest.

Every installation (one instance directory, one data disk) gives the guest its
own serial number and its own eth0 address instead of the release's fixed ones;
the host keeps them in `identity.json` beside the data disk
(darwin/device/block/android_identity.hpp). The guest is the release started
with the macOS launcher (tools/boot_android.py), and the runner checks that:

- the file exists, is private to its owner, and the guest's `ro.serialno`,
  `ro.boot.serialno` and eth0 address are the file's, not the release's
  `umlandroid17` or the user network's default address, and the kernel's command
  line carries one serial number, the file's (the release's own token stays in
  the image as the fallback of a launch that states no serial number, and the
  launcher drops it when it states one);
- eth0 is configured and the host reaches adbd through it, so the user network
  answers the guest under its own address;
- a guest reboot (the restart loop passes the address on) and a new launch of
  the same instance keep the values, and the file is untouched;
- a second instance gets a serial number and an address of its own.

The values are random per installation and are never printed: a failure names
what differed, not what the value was, and the launcher's log tail that a
failure prints has every serial number, hardware address and adb key masked (`mask`).

`--image-inputs` qualifies what only a newer image carries (the accepted release has none of it,
and the default run, which passes on it, checks none of it; it only prints, for eth0, buried_eth0
and wlan0, whether the guest's own listing holds an IPv6 link-local address):

- wlan0's address is the one derived from eth0's (overlay/tools/kernarium_wifi_mac.h, here
  `derive_wifi_mac`, which the golden vectors and a differential run of the header tie to the C++),
  not the old fixed one nor eth0's, the same after a reboot and a new launch, and different
  between installations; wlan0 holds an IPv4 lease (Wi-Fi connected);
- eth0 and buried_eth0 carry no IPv6 link-local address, in the guest's own listing and in the
  interface listing an application calls (`NetworkInterface`, which names only interfaces that
  hold an address: the macvlan holds none, so it is not listed);
- an ordinary application cannot read `ro.boot.kernarium.adb_key`. The emulator is started through
  a wrapper that adds a placeholder key and a control argument to the kernel command line (the macOS
  launcher passes neither). A probe application (property_probe_apk.py) is started with
  `am broadcast`, so it runs in a normal application process of the domain `untrusted_app_29`; the
  runner records that context, requires that the application reads the control property, which every
  application may, and not the key, and requires a positive control in the same boot: root reads
  the key (`su`), or, on an image with no `su`, the kernel command line carries it, which the
  result then names as inferred. The probe targets API 29, the highest a host can sign without
  Android's tools, so it answers for applications of that target.

What a launcher run proves is the property's type and the address's absence in this boot. It does
not prove that adbd reads the key and authorizes the host: the macOS launcher fixes ADB
authorization off and refuses `androidboot.kernarium.adb_auth`, so adbd never compares the
placeholder with a host key. `run_android_adb_debugging.py` cannot prove it either: it connects to
a launcher's adb port with keys of its own, and a guest the Kernarium runtime started authorizes
only the key its application made for that boot, which no host adb server holds. The proof is
Andarium's own adb client against a runtime guest with authorization on,
`AndariumTests/AdbOverVsockTests` (the iOS Simulator or a device; docs/testing.md): the
application's key is accepted whatever the debugging setting, and a foreign key is refused with
no prompt while debugging is off and with a prompt while it is on.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import re
import secrets
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import slot_ports  # noqa: E402
import property_probe_apk  # noqa: E402

DEFAULT_ADB_PORT = 5575
IDENTITY_FILE = "identity.json"
LEGACY_SERIAL = "umlandroid17"
DEFAULT_ETHERNET_MAC = "52:54:00:12:34:56"
LEGACY_WIFI_MAC = "02:15:b2:00:00:00"
DEFAULT_EMULATOR = REPOSITORY / ".build" / "presets" / "macos-android17" / "bin" / "uml-darwin-android17"
GOLDEN_VECTORS = REPOSITORY / "tests" / "unit" / "andarium" / "fixtures" / "wifi-mac-golden-vectors.txt"
KEY_PROPERTY = "ro.boot.kernarium.adb_key"
CONTROL_PROPERTY = "ro.boot.kernarium.identity_probe"
# The interfaces whose IPv6 link-local addresses the run reports; eth0 and the macvlan, which the
# image's network setup turns IPv6 off on, must have none (wlan0's is the framework's).
LINK_NAMES = ("eth0", "buried_eth0", "wlan0")
LOWER_DEVICES = ("eth0", "buried_eth0")
PROBE_SECONDS = 60
LEASE_SECONDS = 120
STOP_SECONDS = 120
SETTLE_SECONDS = 60
TERMINATION_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)
MAC_PATTERN = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")
ANY_MAC = re.compile(r"(?i)(?<![0-9a-z])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-z])")
SERIAL_ASSIGNMENT = re.compile(r"(?i)(serialno\W{0,4})[0-9a-z]{6,20}")
KEY_ASSIGNMENT = re.compile(r"(?i)(adb_key\]?\s*[=:]\s*\[?)[^\s\]\"']+")
MASKED = "<masked>"


class Failure(RuntimeError):
    pass


def mask(text: str, known: tuple[str, ...] = ()) -> str:
    """The text with every serial number, hardware address and adb key replaced: the values
    this run knows (the identity files' and the probe's), any address, and whatever follows
    `serialno` or `adb_key`."""
    for value in known:
        if value:
            text = text.replace(value, MASKED)
    return KEY_ASSIGNMENT.sub(r"\1" + MASKED, SERIAL_ASSIGNMENT.sub(r"\1" + MASKED, ANY_MAC.sub(MASKED, text)))


def known_values(instances: list[pathlib.Path], probes: "Probes | None" = None) -> tuple[str, ...]:
    """The serial numbers and addresses of the identity files that exist, best effort, and the
    values the run put on the kernel command line itself."""
    values: list[str] = [probes.key, probes.control] if probes else []
    for instance in instances:
        try:
            members = json.loads((instance / IDENTITY_FILE).read_text())
            values += [str(members.get("serialno", "")), str(members.get("ethernet_mac", ""))]
        except (OSError, ValueError, AttributeError):
            pass
    return tuple(values)


def ignore_termination() -> None:
    """Cleanup runs to its own deadlines; a second signal must not cut it short."""
    for name in TERMINATION_SIGNALS:
        signal.signal(name, signal.SIG_IGN)


def terminate_on_signal(signum: int, _frame: object) -> None:
    ignore_termination()
    raise SystemExit(128 + signum)


class Device:
    """The guest as the host's adb sees it, over the shared server."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb = adb
        self.serial = serial

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], text=True, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
            check=False)

    def connect(self) -> bool:
        try:
            self.run("connect", self.serial, timeout=15)
            state = self.run("-s", self.serial, "get-state", timeout=15)
        except subprocess.TimeoutExpired:
            return False
        return state.returncode == 0 and state.stdout.strip() == "device"

    def shell(self, command: str, timeout: int = 30) -> str:
        result = self.run("-s", self.serial, "shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}")
        return result.stdout.strip()

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}")


def wait_for(description: str, probe: Callable[[], bool], seconds: int,
             guest: subprocess.Popen | None = None) -> None:
    """Poll the probe until it is true; a probe that raises is not true yet."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if guest is not None and guest.poll() is not None:
            raise Failure(f"the guest's launcher exited ({guest.returncode}) while waiting for {description}")
        try:
            if probe():
                return
        except (Failure, subprocess.TimeoutExpired):
            pass
        time.sleep(1)
    raise Failure(f"{description}: not within {seconds} s")


def boot_id(device: Device) -> str:
    return device.shell("cat /proc/sys/kernel/random/boot_id")


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None,
                previous_boot: str | None = None) -> None:
    def booted() -> bool:
        if not device.connect() or device.getprop("sys.boot_completed") != "1":
            return False
        return previous_boot is None or boot_id(device) != previous_boot
    wait_for("the guest to be booted", booted, seconds, guest)


class Guest:
    """One launch of an instance: the launcher process and its adb connection."""

    def __init__(self, options: argparse.Namespace, instance: pathlib.Path, log: pathlib.Path) -> None:
        self.options = options
        self.instance = instance
        self.log = log
        self.process: subprocess.Popen | None = None
        self.connected = True
        self.device = Device(options.adb, f"127.0.0.1:{options.adb_port}")

    def start(self) -> None:
        self.connected = True
        options = self.options
        command = [sys.executable, str(LAUNCHER), "--instance", str(self.instance),
                   "--adb", options.adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                   "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
        if options.release:
            command += ["--release", str(options.release)]
        if options.emulator:
            command += ["--emulator", str(options.emulator)]
        with open(self.log, "ab") as log:
            self.process = subprocess.Popen(
                command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        wait_booted(self.device, options.boot_timeout, self.process)

    def stop(self) -> None:
        """Ends the launcher and drops the connection; neither a launcher that ignores SIGTERM
        nor an adb that times out keeps the other half from running."""
        process, self.process = self.process, None
        try:
            if process is not None and process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=STOP_SECONDS)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=30)
        finally:
            self.disconnect()

    def disconnect(self) -> None:
        """Best effort: a guest that is gone has nothing to disconnect from."""
        if not self.connected:
            return
        self.connected = False
        try:
            self.device.run("disconnect", self.device.serial, timeout=30)
        except (subprocess.TimeoutExpired, OSError):
            pass

    def reboot(self) -> None:
        previous = boot_id(self.device)
        self.device.run("-s", self.device.serial, "reboot", timeout=30)
        wait_booted(self.device, self.options.boot_timeout, self.process, previous)


class Identity:
    """What a guest reports about itself, and what the host's file says."""

    def __init__(self, serial: str, boot_serial: str, ethernet: str) -> None:
        self.serial = serial
        self.boot_serial = boot_serial
        self.ethernet = ethernet

    @staticmethod
    def of(device: Device) -> "Identity":
        return Identity(device.getprop("ro.serialno"), device.getprop("ro.boot.serialno"),
                        ethernet_address(device))

    def same_as(self, other: "Identity") -> bool:
        return (self.serial, self.boot_serial, self.ethernet) == (other.serial, other.boot_serial, other.ethernet)


def ethernet_address(device: Device) -> str:
    """eth0's hardware address as `ip addr` shows it: the shell may read neither sysfs nor
    a single link through netlink (`ip link show eth0` is refused), only the whole listing."""
    for line in device.shell("ip addr show eth0").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "link/ether":
            return fields[1].lower()
    raise Failure("`ip addr` shows no hardware address for eth0")


def read_identity_file(instance: pathlib.Path) -> tuple[bytes, dict]:
    path = instance / IDENTITY_FILE
    if not path.is_file():
        raise Failure(f"the instance has no {IDENTITY_FILE}")
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise Failure(f"{IDENTITY_FILE} is readable beyond its owner (mode {mode:o})")
    content = path.read_bytes()
    try:
        members = json.loads(content)
    except ValueError as problem:
        raise Failure(f"{IDENTITY_FILE} is not JSON: {problem}") from problem
    if set(members) != {"schema", "serialno", "ethernet_mac"} or members["schema"] != 1:
        raise Failure(f"{IDENTITY_FILE} has other members than schema 1's")
    return content, members


def check_guest_matches_file(device: Device, instance: pathlib.Path) -> Identity:
    """The guest reports the installation's identity, and not the release's own."""
    _, members = read_identity_file(instance)
    guest = Identity.of(device)
    if guest.serial != members["serialno"]:
        raise Failure("ro.serialno is not the identity file's serial number")
    if guest.boot_serial != members["serialno"]:
        raise Failure("ro.boot.serialno is not the identity file's serial number")
    if guest.serial == LEGACY_SERIAL:
        raise Failure("the guest kept the release's fixed serial number")
    # The release's own serial number is dropped from the command line, so the guest is given one.
    arguments = [token for token in device.shell("cat /proc/cmdline").split()
                 if token.startswith("androidboot.serialno=")]
    if arguments != [f"androidboot.serialno={members['serialno']}"]:
        raise Failure("the kernel command line does not carry exactly the installation's serial number")
    if not MAC_PATTERN.match(guest.ethernet) or guest.ethernet != members["ethernet_mac"]:
        raise Failure("eth0's address is not the identity file's")
    if guest.ethernet == DEFAULT_ETHERNET_MAC:
        raise Failure("eth0 kept the user network's default address")
    return guest


def check_network(device: Device) -> None:
    """eth0 is configured under its own address; adbd was reached through it."""
    if " inet " not in device.shell("ip -4 addr show dev eth0"):
        raise Failure("eth0 has no IPv4 address")


# ---- What only a newer image carries (--image-inputs).

MASK64 = (1 << 64) - 1
WIFI_KEY = 0x6B726E5F77696669  # "krn_wifi"
WIFI_BITS = 46
WIFI_HALF_BITS = 23
WIFI_HALF_MASK = (1 << WIFI_HALF_BITS) - 1
WIFI_VALUES = 1 << WIFI_BITS
WIFI_ROUNDS = 6
WIFI_MAX_STEPS = 258


def mix64(value: int) -> int:
    """The finalizer of MurmurHash3's 64-bit hash, as in kernarium_wifi_mac.h."""
    value ^= value >> 33
    value = value * 0xFF51AFD7ED558CCD & MASK64
    value ^= value >> 33
    value = value * 0xC4CEB9FE1A85EC53 & MASK64
    return value ^ value >> 33


def format_mac(packed: int) -> str:
    return ":".join(f"{(packed >> shift) & 0xFF:02x}" for shift in range(40, -8, -8))


def mac_bits(mac: str) -> int:
    """The address without its two flag bits (WifiMacBits)."""
    packed = int(mac.replace(":", ""), 16)
    return (packed >> 42) << 40 | packed & 0xFFFFFFFFFF


def mac_from_bits(bits: int) -> int:
    """The locally administered unicast address of the 46 bits, packed (WifiMacFromBits)."""
    return ((bits >> 40 << 2 | 0x02) << 40) | bits & 0xFFFFFFFFFF


def wifi_reserved(packed: int) -> bool:
    return packed == 0x020000000000 or packed >> 8 == 0x52550A0002


def wifi_round(half: int, key: int, round_number: int) -> int:
    return mix64(half ^ (key + round_number * 0x9E3779B97F4A7C15 & MASK64) & MASK64) & WIFI_HALF_MASK


def wifi_permute(bits: int, key: int = WIFI_KEY) -> int:
    left, right = bits >> WIFI_HALF_BITS, bits & WIFI_HALF_MASK
    for round_number in range(WIFI_ROUNDS):
        left, right = right, left ^ wifi_round(right, key, round_number)
    return left << WIFI_HALF_BITS | right


def wifi_unpermute(bits: int, key: int = WIFI_KEY) -> int:
    left, right = bits >> WIFI_HALF_BITS, bits & WIFI_HALF_MASK
    for round_number in reversed(range(WIFI_ROUNDS)):
        left, right = right ^ wifi_round(left, key, round_number), left
    return left << WIFI_HALF_BITS | right


def derive_wifi_mac(ethernet: str, key: int = WIFI_KEY) -> str:
    """wlan0's address as a function of eth0's: a permutation of the 46 free bits (a Feistel
    network rho), taken at the first step after eth0's along rho^-1(rho(x) + k) that is not
    reserved. The port of DeriveWifiMac in android/lineage-24/overlay/tools/kernarium_wifi_mac.h,
    which the golden vectors tie to the C++."""
    position = wifi_permute(mac_bits(ethernet), key)
    for step in range(1, WIFI_MAX_STEPS + 1):
        candidate = mac_from_bits(wifi_unpermute((position + step) % WIFI_VALUES, key))
        if not wifi_reserved(candidate):
            break
    return format_mac(candidate)


def ethernet_from_wifi_mac(wifi: str, key: int = WIFI_KEY) -> str:
    """The inverse of derive_wifi_mac (EthernetFromWifiMac)."""
    position = wifi_permute(mac_bits(wifi), key)
    for step in range(1, WIFI_MAX_STEPS + 1):
        candidate = mac_from_bits(wifi_unpermute((position - step) % WIFI_VALUES, key))
        if not wifi_reserved(candidate):
            break
    return format_mac(candidate)


def golden_vectors(path: pathlib.Path = GOLDEN_VECTORS) -> list[tuple[str, str]]:
    vectors = []
    for line in path.read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            ethernet, wifi = line.split()
            vectors.append((ethernet, wifi))
    return vectors


def link_address(device: Device, name: str) -> str:
    """A device's hardware address as `ip addr` shows it (see ethernet_address)."""
    for line in device.shell(f"ip addr show {name}").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "link/ether":
            return fields[1].lower()
    raise Failure(f"`ip addr` shows no hardware address for {name}")


class Probes:
    """What `--image-inputs` passes to the guest and asks it about: a key and a control value
    for the kernel command line, and the application that reads them as an application."""

    def __init__(self, scratch: pathlib.Path, emulator: pathlib.Path) -> None:
        # The key is a placeholder: the macOS launcher fixes ADB authorization off, so adbd
        # never compares it with a host's.
        self.key = "probe" + secrets.token_hex(8)
        self.control = secrets.token_hex(8)
        self.emulator = scratch / "emulator-with-probes"
        self.emulator.write_text(
            "#!/bin/sh\n"
            f"exec {shlex.quote(str(emulator))} \"$@\" "
            f"androidboot.kernarium.adb_key={self.key} "
            f"androidboot.kernarium.identity_probe={self.control}\n")
        self.emulator.chmod(0o700)
        self.apk = property_probe_apk.build_apk(scratch / "property-probe.apk", CONTROL_PROPERTY, KEY_PROPERTY)


def check_wifi(device: Device, ethernet: str) -> str:
    """wlan0 has the address derived from eth0's, is not the image's old fixed one, and holds
    its lease (the framework has joined the virtual network). Returns the address."""
    wifi = link_address(device, "wlan0")
    if wifi != derive_wifi_mac(ethernet):
        raise Failure("wlan0's address is not the one derived from eth0's")
    if wifi in (LEGACY_WIFI_MAC, ethernet):
        raise Failure("wlan0 has the image's old fixed address or eth0's own")
    wait_for("wlan0 to hold an IPv4 address", lambda: " inet " in device.shell("ip -4 addr show dev wlan0"),
             LEASE_SECONDS)
    return wifi


def link_local_interfaces(device: Device) -> dict[str, bool]:
    """Whether the guest's own listing (`ip addr`) holds an IPv6 link-local address for each of
    LINK_NAMES: only yes or no, never an address."""
    found = {name: False for name in LINK_NAMES}
    current = None
    for line in device.shell("ip addr show").splitlines():
        header = re.match(r"^\d+: ([^:@]+)[:@]", line)
        if header:
            current = header.group(1)
            continue
        fields = line.split()
        if current in found and fields[:1] == ["inet6"] and len(fields) > 1 and fields[1].lower().startswith("fe80:"):
            found[current] = True
    return found


def report_link_local(device: Device) -> dict[str, bool]:
    """Prints a line for each of the three interfaces (no address) and returns what it found."""
    found = link_local_interfaces(device)
    for name in LINK_NAMES:
        print(f"{name}: IPv6 link-local address in the guest's listing: {'yes' if found[name] else 'no'}")
    return found


def check_lower_devices(device: Device) -> None:
    """eth0 and buried_eth0 carry no IPv6 link-local address (wlan0's is the framework's)."""
    found = link_local_interfaces(device)
    for name in LOWER_DEVICES:
        if found[name]:
            raise Failure(f"{name} has an IPv6 link-local address in the guest's listing")


class ProbeResult:
    """What the probe application reported, the values the runner may print."""

    def __init__(self, fields: dict[str, str]) -> None:
        self.context = fields.get("context", "")
        self.control = fields.get("control") == "readable"
        self.key = fields.get("key") == "readable"
        self.links = {}
        for item in fields.get("links", "").split():
            name, _, state = item.partition("=")
            self.links[name] = state == "yes"
        self.complete = "done" in fields

    @property
    def domain(self) -> str:
        """The SELinux context without its categories (user, role, type and sensitivity level)."""
        return ":".join(self.context.split(":")[:4])


def parse_probe_result(text: str) -> ProbeResult:
    fields = {}
    for line in text.splitlines():
        name, separator, value = line.partition("=")
        fields[name.strip() if separator else line.strip()] = value.strip()
    return ProbeResult(fields)


def run_probe_application(device: Device, apk: pathlib.Path) -> ProbeResult:
    """Installs the probe once, starts it with `am broadcast` (the zygote forks a normal
    application process for the receiver) and reads the result file it wrote."""
    package = property_probe_apk.PACKAGE
    if "package:" not in device.shell(f"pm path {package} || true"):
        result = device.run("-s", device.serial, "install", str(apk), timeout=120)
        if result.returncode != 0 or "Success" not in result.stdout:
            raise Failure("the property probe application did not install")
    device.shell(f"run-as {package} rm -f {property_probe_apk.RESULT_FILE}")
    broadcast = device.shell(f"am broadcast -n {property_probe_apk.COMPONENT}")
    if "Broadcast completed" not in broadcast:
        raise Failure("the probe broadcast was not delivered")
    holder: list[ProbeResult] = []

    def written() -> bool:
        holder.append(parse_probe_result(device.shell(f"run-as {package} cat {property_probe_apk.RESULT_FILE}")))
        return holder[-1].complete

    wait_for("the probe application to write its result", written, PROBE_SECONDS)
    return holder[-1]


def root_reads_key(device: Device, probes: "Probes") -> bool | None:
    """Whether root reads the key in this boot: True or False, None where the image has no `su`."""
    try:
        return device.shell(f"su 0 getprop {KEY_PROPERTY}") == probes.key
    except Failure:
        return None


def check_key_property(device: Device, probes: "Probes") -> str:
    """An ordinary application reads the control property and not the key, which the kernel was
    given in this same boot. Returns the line the run prints."""
    boot = boot_id(device)
    if f"androidboot.kernarium.adb_key={probes.key}" not in device.shell("cat /proc/cmdline"):
        raise Failure("the kernel command line does not carry the probe key: the test cannot tell")
    result = run_probe_application(device, probes.apk)
    if not result.domain.startswith("u:r:untrusted_app_"):
        raise Failure(f"the probe application did not run as an ordinary application (its domain is {result.domain})")
    if not result.control:
        raise Failure(f"the application cannot read {CONTROL_PROPERTY}: the test cannot tell")
    rooted = root_reads_key(device, probes)
    if rooted is False:
        raise Failure(f"root does not read {KEY_PROPERTY} in this boot: the image dropped the property")
    if result.key:
        raise Failure(f"an application can read {KEY_PROPERTY}")
    if boot_id(device) != boot:
        raise Failure("the guest rebooted while the probe ran: the control and the key are not of one boot")
    # The listing names the interfaces that hold an address: eth0 holds 10.0.2.15, so it is there; the
    # macvlan holds none once it has no IPv6, so it is not listed and shows no address either.
    if "eth0" not in result.links:
        raise Failure("the application's interface listing has no eth0, which holds an IPv4 address: the test cannot tell")
    for name in LOWER_DEVICES:
        if result.links.get(name, False):
            raise Failure(f"the application's interface listing shows an IPv6 link-local address for {name}")
    control = "root reads the key" if rooted else "the key is on the kernel command line (inferred: no su)"
    return (f"the probe ran as {result.domain}: it reads the control property and not the key in a boot where "
            f"{control}; its interface listing shows no link-local address for eth0 or buried_eth0")


def check_image(device: Device, ethernet: str, probes: Probes) -> str:
    wifi = check_wifi(device, ethernet)
    check_lower_devices(device)
    print(check_key_property(device, probes))
    return wifi


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--image-inputs", action="store_true",
                        help="also check what an image built with this repository's overlay carries: "
                        "the derived Wi-Fi address and the restricted adb key property")
    return parser


def stop_all(guests: list[Guest]) -> None:
    """Every guest is stopped, whatever an earlier stop raises: the emulator runs in a process
    group of its own, so the runner's exit does not end it."""
    for guest in guests:
        try:
            guest.stop()
        except (subprocess.TimeoutExpired, OSError) as problem:
            print(f"cleanup: a guest did not stop cleanly: {type(problem).__name__}", file=sys.stderr)


def main() -> int:
    options = build_parser().parse_args()
    options.adb = shutil.which(options.adb) or options.adb
    for name in TERMINATION_SIGNALS:
        signal.signal(name, terminate_on_signal)
    guests: list[Guest] = []
    probes: Probes | None = None
    with tempfile.TemporaryDirectory(prefix="android-installation-identity-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        log_path = scratch / "launcher.log"
        try:
            if options.image_inputs:
                probes = Probes(scratch, options.emulator or DEFAULT_EMULATOR)
                options.emulator = probes.emulator

            def image_inputs(guest: Guest, identity: Identity) -> str | None:
                return check_image(guest.device, identity.ethernet, probes) if probes else None

            first = Guest(options, scratch / "first", log_path)
            guests.append(first)
            first.start()
            identity = check_guest_matches_file(first.device, first.instance)
            check_network(first.device)
            print("first installation: its own serial number and eth0 address, network up")
            if probes is None:
                report_link_local(first.device)
            wifi = image_inputs(first, identity)
            file_before, _ = read_identity_file(first.instance)
            if probes:
                print("first installation: wlan0 derived from eth0, joined, the lower devices without IPv6, "
                      "the adb key unreadable by an application")

            first.reboot()
            if not check_guest_matches_file(first.device, first.instance).same_as(identity):
                raise Failure("a guest reboot changed the identity")
            check_network(first.device)
            if image_inputs(first, identity) != wifi:
                raise Failure("a guest reboot changed wlan0's address")
            print("reboot: identity kept")

            first.stop()
            first.start()
            if not check_guest_matches_file(first.device, first.instance).same_as(identity):
                raise Failure("a new launch of the instance changed the identity")
            if read_identity_file(first.instance)[0] != file_before:
                raise Failure(f"a new launch rewrote {IDENTITY_FILE}")
            if image_inputs(first, identity) != wifi:
                raise Failure("a new launch changed wlan0's address")
            print("new launch of the same instance: identity kept, file untouched")
            first.stop()

            second = Guest(options, scratch / "second", log_path)
            guests.append(second)
            second.start()
            other = check_guest_matches_file(second.device, second.instance)
            check_network(second.device)
            other_wifi = image_inputs(second, other)
            if other.serial == identity.serial:
                raise Failure("the second installation has the first one's serial number")
            if other.ethernet == identity.ethernet:
                raise Failure("the second installation has the first one's eth0 address")
            if other_wifi is not None and other_wifi == wifi:
                raise Failure("the second installation has the first one's wlan0 address")
            print("second installation: a serial number and an eth0 address of its own"
                  + (", and a wlan0 address" if probes else ""))
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            known = known_values([scratch / "first", scratch / "second"], probes)
            print(f"FAIL: {mask(str(problem), known)}", file=sys.stderr)
            if log_path.is_file():
                tail = "\n".join(log_path.read_text(errors="replace").splitlines()[-30:])
                print("launcher log tail (serial numbers and addresses masked):\n" + mask(tail, known),
                      file=sys.stderr)
            return 1
        finally:
            ignore_termination()
            stop_all(guests)


if __name__ == "__main__":
    raise SystemExit(main())
