#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the guest's one Wi-Fi network: never disabled, a failed check only warns, kept through a suspend, rejoined after a boot.

The platform disables a saved network at its first failed internet check, and a
disabled network is never joined again, so a guest whose first check failed has
no network from then on (android/lineage-24/README.md, "Networking"). The image
overlays the thresholds that disable it (rro/KernariumWifiOverlay) and a boot
script saves the network again, until the station has joined it once, when a
boot finds it missing from the Wi-Fi service's list or finds Wi-Fi on and nothing
joined (overlay/tools/kernarium_wifi_network.sh).
The platform validates the network before it checks it (routes p0029 and p0033):
a failed check, and a sign-in or block page in place of the expected answer, only
show Android's own Wi-Fi mark. The runner shows all of it:

- fresh: the module's overlay is applied (`cmd overlay list`) and holds the
  largest threshold for both no-internet reasons; the script ran without
  SELinux denials; with the validation probes at a black hole, the network stays
  validated and joined while Settings says "Limited connection", the status bar's
  Wi-Fi icon says "No internet", the validation log says the check failed and no
  notification or sign-in activity follows, also after a guest suspend and wake
  (the same network, never validated again: the station is kept); a sign-in page (a redirect) and a block page
  (a 200 answer) that the host's server gives the probes through `adb reverse`
  show the same and nothing more, and the current network's own probe log names
  the section's URL and the answer it received; on one network that is never
  replaced or validated again (`lastValidated` unchanged), with the Internet
  page in front and no other action, the network is validated before its first
  check, the warning shows in Settings and in the status bar, the check comes
  back after the stock back-off, and when the server answers 204 the warning
  goes from both and a new passing check is in the log; a forced re-evaluation
  (`cmd connectivity reevaluate`, whose failure fails the section) brings it back
  and the next passing check takes it away; a passing check on a network that
  has just joined leaves no warning, by the host's server (the HTTPS probe off:
  the platform's `captive_portal_use_https` flag of DeviceConfig's connectivity
  namespace at 0, which the log shows by sending no HTTPS probe) and by the stock
  servers (both probes asked, the log naming each stock URL and its 204); with
  a strict private DNS name the platform can use, a check that fails again and
  again leaves the network validated with its private DNS not broken, and the
  private DNS probe is not repeated for each check; a forced
  re-evaluation of a network whose strict name does not resolve leaves it not
  validated with its private DNS broken, without a check at the server or in the
  log from the request on, and once the name is gone the network is validated
  and a second request is checked; each request is judged by the new line it
  adds to the network's own log; a network is observed validated while its first
  check is still unanswered (the server holds that answer, and the network's log
  has no probe or check that completed before it), and no check follows a
  passing one until an evaluation is forced; across all
  of it NetworkStack, SystemUI and Settings (each by the exact name it runs
  under) neither crash nor stop answering and nothing hits an illegal
  instruction, which one continuous log stream of each boot of a guest of the
  release under test carries (the main, system and crash buffers from their
  start, kept in a host file), each finding belonging to the section whose time
  window holds it or to none, and either fails the run; a stream that ends while
  its guest is up fails the section in progress, and is confirmed alive and
  carrying before a restart or suspend; one that does not end when the old boot
  is drained fails the run; with the
  probes at a black hole and a strict private DNS name that does not resolve,
  and VirtWifi forgotten and saved again, the network stays enabled and joined for
  a minute and a half (a platform that counts no failed check must say in its
  validation log that the injected failure reached it: a failed check or the
  private DNS name evaluated; on the image under test the old "validated
  without probes" statement is a failure), and stays joined through
  a guest suspend and wake (the host's route, `suspend` and `wake` on the
  management console; the launcher's log, with the guest kernel's console in
  it, is read from before the suspend to the end of the section, whenever its
  lines arrive and however the section ends: the host's own reset line fails it
  as a host GPU reset across the suspend, a known host defect that leaves the
  guest's graphics gone, and any other GPU error line as GPU errors whose cause
  is not established, neither a Wi-Fi failure; see gpu_suspend_log), is joined
  again after a restart, and a suspend keeps the station (see below); a network
  the user marked metered, or turned Auto-connect off for,
  keeps that after a restart (the script has nothing to save once the station
  has joined, and ends at once at marker 2); a forgotten network is still forgotten after a restart; and Wi-Fi
  that was turned off is still off;
- a suspend keeps the station, in a stay of five seconds, in one after Wi-Fi was
  turned off and on (the supplicant restarted), and in one of 40 s with the
  emulator process stopped as iOS holds the app (the runner stops and continues
  its own emulator): the network after the wake is the network before it, with
  its ID; the guest's log from before the suspend to ten seconds after the wake
  holds no disconnect of the supplicant or of the platform; a request made a
  second after the wake (a ping to the gateway and a DNS query over TCP to the
  name server, both answered inside the host) is answered; and the connections
  that the host holds into the station's address (through a forward of the
  launcher, to an echo server of the runner on that address) carry data both
  ways, a line that the host wrote in the frozen stay arrives, and one that the
  host closed in it is ended for the guest by the time the request starts. A
  suspend that lands inside the connect delay of the joining station (virt_wifi
  finishes a connect two seconds after it is asked for), with the emulator stopped
  for a minute (longer than cfg80211 keeps a BSS that nothing holds), ends with
  the station joined and a request working within a bounded time, or retried and
  joined, never joined by its own report with no working network, and the guest
  kernel's console shows no `WARNING:` and no call trace;
- upgrade (only with --previous-release, once for each release): the data disk
  of a release whose platform disables the network for lack of internet (the
  stock rules), on which the probes and a forgotten and saved network disable
  VirtWifi and which holds the network disabled, with the station away from it,
  after a restart of its own guest, boots the release under test: the first boot
  saves the network again, the station joins it, the next boot leaves it alone,
  a failed check warns, and Auto-connect turned off after the repair stays off
  across a restart: the next boot, which has no join to wait for, reads
  Auto-connect from the Wi-Fi service's dump and logs that it left the network
  alone (the line also proves its own domain reads the dump). The setting is the
  one the dump prints under VirtWifi's own configuration, which it must show,
  and the script's service must have ended on that boot. The data disk of
  a release that never disables the network, with the network joined, the
  user's metered setting and the probes at a black hole, boots the release under
  test: nothing saves the network again, the setting and the join are kept, a
  failed check warns, and Auto-connect turned off stays off across a restart with
  nothing logged (the marker is 2);
- quick stop: a guest stopped within a second or two of the boot script saving the
  network, or at the first moment the station reports the join, can leave the
  Wi-Fi service's config store without the network, because the service writes
  the store some time after both. The same data boots the release under test
  again, with the continuous crash capture attached. The stopped data's own
  list is read at the boot's first observation, before the script's guard can
  repair it: a list without VirtWifi is a stop that came before the store was
  written, and a list that holds it one that came after, which is tried again
  (up to six times, and twice for the stop at the join). Data that lost the
  network must hold an address within 60 s of boot_completed, last observation
  included; the script must say it left marker 1 (a boot that had to save the
  network never sets 2), and after a restart, which the store survives, the
  next boot must say it found the network stored and joined and set marker 2.
  The script's log lines are the runner's window on its marker: the shell cannot
  read the property. No stop that comes before the store's write fails the
  first case; for the second, which needs a join before a write that follows
  the save by a second or two, it is a note. `--quick-stop-only` runs these
  cases alone, which is how an image without the fix shows its defect: its
  script ends at marker 1 with no network and the case fails.
- wake only (`--wake-only`): the warning of a failed check on the network that a wake
  keeps, and the sections of a suspend, alone: how a kernel shows its defect or
  its fix.

Limits: a log buffer that wrapped before the stream attached cannot be detected;
a crash during the final cleanup after the summary section is not scanned; and
the 60 s hold on a network whose private DNS failed ends with an observation
that precedes its last sleep, so a check in its final second is not seen.

The checks of one section do not stop the others, so a release that lacks the
overlay shows every symptom; the run passes only if none failed. Every wait has a
deadline. The guest is reached through the shared adb server, always named with
`-s`, and stopped through its own process on any exit.

The runner is opt-in: it needs the macos-android17 executable, an installed
release, and an adb client.
usage: run_android_wifi_network.py --emulator uml-darwin-android17 [--release DIR]
       [--previous-release DIR]... [--quick-stop-only | --wake-only]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import http.server
import importlib.util
import io
import os
import pathlib
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from xml.etree import ElementTree

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
ANDROID_TOOLS = REPOSITORY / "android" / "lineage-24" / "tools"
BOOT_SCRIPT = REPOSITORY / "android" / "lineage-24" / "overlay" / "tools" / "kernarium_wifi_network.sh"
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import gpu_suspend_log  # noqa: E402
import slot_ports  # noqa: E402


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_android = load_module("boot_android", REPOSITORY / "tools" / "boot_android.py")

DEFAULT_ADB_PORT = 5584
COMMAND_SECONDS = 120
BOOT_SECONDS = 900
JOIN_SECONDS = 240
RESUME_SECONDS = 120
HOLD_SECONDS = 90
POLL_SECONDS = 5
SUSPEND_ATTEMPTS = 4
STORE_SECONDS = 30
MARGIN_SECONDS = 30
# A suspend keeps the station: what is judged around a wake. The launcher forwards a host port (the slot's fourth, a
# fixed one outside a slot) to the guest's Wi-Fi address, where the runner's echo server listens, so the host holds a
# real connection into wlan0 and the guest's request goes through the default network the way an app's does.
GUEST_WIFI_ADDRESS = "10.0.2.16"
ECHO_PORT = 7001
FORWARD_OFFSET = 3
DEFAULT_FORWARD_PORT = 5994
ECHO_DIRECTORY = "/data/local/tmp/wifi-wake"
ECHO_SECONDS = 10.0
ECHO_ATTEMPTS = 20
# How long a stay is (short: the guest's own alarms wake a longer one, before the host's wake and the request that
# measures from it), how long after the wake the guest's request goes out, and how long after the wake no
# disconnect may show (the supplicant's reports and the platform's come within a tenth of a second of a wake).
SUSPEND_STAY_SECONDS = 5
WAKE_REQUEST_SECONDS = 1.0
WAKE_WINDOW_SECONDS = 10.0
# With the emulator stopped the stay is as long as a thawed guest's own RTC alarm would not let it be (about 30 s: the
# alarm ends any suspend), and the host acts on the connections FROZEN_FIRST_SECONDS in.
FROZEN_STAY_SECONDS = 40
FROZEN_FIRST_SECONDS = 5
# After Wi-Fi was turned on and the station joined, how long it is left before a suspend (the platform settles).
TOGGLE_SETTLE_SECONDS = 20
# A DNS answer over TCP: two bytes of length and the 12-byte header at the least.
DNS_ANSWER_MIN_BYTES = 14
# A suspend inside the station's connect delay (virt_wifi finishes a connect two seconds after it is asked for): the
# supplicant's line that it has begun to connect, the one that it has connected, how long the guest is given to
# begin, how long the emulator is stopped (past the 30 s after which cfg80211 forgets a BSS it does not hold), how
# many times the suspend is tried to land inside the delay, and how long after its start a connect that finished
# is taken to have been held across the suspend (the delay itself is two seconds).
CONNECT_START_LINE = "Trying to associate with"
CONNECT_DONE_LINE = "CTRL-EVENT-CONNECTED"
CONNECT_WAIT_SECONDS = 60
CONNECT_STAY_SECONDS = 60
CONNECT_ATTEMPTS = 3
CONNECT_HELD_SECONDS = 10.0
# A saved network the disk holds enabled is joined well within this after boot_completed.
BOOT_JOIN_SECONDS = 30
# How long the platform takes to join a network the script saved again, by its own scan.
REJOIN_SECONDS = 180
BLACKHOLE = "192.0.2.1"
UNRESOLVED_NAME = "no-such-host.invalid"
SSID = "VirtWifi"
OVERLAY_PACKAGE = "com.android.wifi.resources.kernarium"
OVERLAY_FILE = "/vendor/overlay/KernariumWifiOverlay.apk"
SCRIPT_LOG_TAG = "kernarium_wifi_network"
SCRIPT_DOMAIN = "kernarium_wifi_network"
SCRIPT_LOG_LINE = "VirtWifi saved again"
NOT_STORED_LINE = "VirtWifi was not stored: saved again"
# What the script logs when it ends a boot with its marker at 1 (it saved the network, or found it unlisted) or at 2
# (it found the network stored and joined, or removed in this boot). The shell cannot read the marker; the script logs
# a marker 2 only after the property was set.
UNSTORED_LINE = "VirtWifi joined, and is not known to be stored: marker stays 1"
SETTLED_LINE = "VirtWifi was stored and joined: marker 2"
# What it logs when a boot without marker 2 finds the network listed, nothing joined and Auto-connect off: the user's
# choice. The line also shows that the script's own domain read Auto-connect from the Wi-Fi service's dump.
LEFT_ALONE_LINE = "VirtWifi has Auto-connect off: left alone"
# The quick-stop cases: how many stops each tries before it gives up (the stop at the join, which comes after the
# store's write unless that is late, tries fewer), how long the first boot has to reach its stop point, and how long
# after boot_completed the second boot has to hold an address.
QUICK_ATTEMPTS = 6
QUICK_JOIN_ATTEMPTS = 2
QUICK_FIRST_SECONDS = 120
LEASE_SECONDS = 60
PROBE_SETTINGS = {
    "captive_portal_https_url": f"https://{BLACKHOLE}/generate_204",
    "captive_portal_http_url": f"http://{BLACKHOLE}/generate_204",
    "captive_portal_fallback_url": f"http://{BLACKHOLE}/gen_204",
    "private_dns_mode": "hostname",
    "private_dns_specifier": UNRESOLVED_NAME,
}
# What fails the check alone, leaving the network itself alone: a strict private DNS name that does not resolve
# fails the network (the platform reports it invalid), so the warning sections leave the private DNS settings out.
CHECK_SETTINGS = {key: value for key, value in PROBE_SETTINGS.items() if not key.startswith("private_dns")}
PRIVATE_DNS_SETTINGS = ("private_dns_mode", "private_dns_specifier")
# The platform's switch for the HTTPS probe is a flag of the `connectivity` namespace of DeviceConfig (`cmd
# device_config`), not a global setting: the image's NetworkStack sends the HTTPS probe whatever the setting of that
# name says. With the flag at 0 a check asks the HTTP probe alone, which the host's server answers; with it unset a
# pass needs the HTTPS probe answered too, which only the stock servers do. The sections put and delete it with
# `put_settings` and `delete_settings`, as they do the probe settings; the monitor of a network reads it when the
# network joins.
USE_HTTPS_FLAG = "captive_portal_use_https"
HTTPS_FLAG_NAMESPACE = "connectivity"
ALL_CHECK_SETTINGS = (*PROBE_SETTINGS, USE_HTTPS_FLAG)
# What the platform probes with no URL setting, as the image's validation log names the servers.
STOCK_HTTP_URL = "http://connectivitycheck.gstatic.com/generate_204"
STOCK_HTTPS_URL = "https://www.google.com/generate_204"
# The state init keeps for the boot script's service: "running" while the script waits, "stopped" once it has
# exited, and nothing before it first starts. The shell reads it on a user build. The script's own marker
# (persist.sys.kernarium.virtwifi_saved) is not readable that way: its label is the script domain's alone, so
# `getprop` as the shell prints nothing for it whatever the disk holds (an avc denial), and no section may
# take that for the marker being unset.
SERVICE_PROPERTY = "init.svc.kernarium_wifi_network"
# The validation log lines of the NetworkMonitor topic of route p0029 (the verifier requires them in the image).
CHECK_FAILED_LINE = "Wi-Fi internet check failed, the network stays validated"
CHECK_PASSED_LINE = "Wi-Fi internet check passed"
VALIDATED_FIRST_LINE = "Wi-Fi is validated before its internet check"
PRIVATE_DNS_PROBE = "PROBE_PRIVDNS "
# A private DNS server that serves the strict name the platform probes (the host must reach it, as it must
# reach the stock servers for the section that takes the warning away with them).
PRIVATE_DNS_HOST = "dns.google"
# The checks of one network that a section counts for the back-off (1 s, 2 s, 4 s between the first four),
# and how close to the stock delay a gap between two checks must be.
RETRIES_NEEDED = 4
INITIAL_BACKOFF_SECONDS = 1.0
BACKOFF_FLOOR = 0.9
# How long a forced re-evaluation has to show: a new check at the server.
FORCE_SECONDS = 45
# How long a forced re-evaluation of a network whose strict private DNS has failed is watched: longer than a
# failing check takes (10 s to 25 s) and than the first retry of one.
UNVALIDATED_SECONDS = 60
# How long a network is left alone after a passing check before an evaluation is forced: no timer is left
# after a pass, so no check reaches the server in it.
QUIET_SECONDS = 20
# The platform's HTTP probe gives up after 10 s (NetworkMonitor's SOCKET_TIMEOUT_MS, the connect and read timeout
# that makeProbeConnection sets, for the single HTTP probe the sections that turn HTTPS off send). The server holds
# the answer to a network's first check for at most HELD_ANSWER_SECONDS, below it, while the runner looks at the
# network: the platform validates a Wi-Fi network first and checks it after, so it is validated while the check
# is unanswered.
PROBE_SOCKET_TIMEOUT_SECONDS = 10.0
HELD_ANSWER_SECONDS = 8.0
FORCED_LINE = "Forcing reevaluation"
# Android's own words: the Wi-Fi list's summary and the status bar icon's spoken description.
LIMITED_CONNECTION = "Limited connection"
NO_INTERNET = "No internet"
SYSTEMUI_PACKAGE = "com.android.systemui"
UI_DUMP = "/data/local/tmp/wifi-network-ui.xml"
# From the join to the mark: a blocked check takes about 10 to 25 s, and Settings redraws on the report.
WARNING_SECONDS = 90
# How long a warning is held against a disabled, disconnected, unvalidated or notified network.
WARNING_HOLD_SECONDS = 60
SHORT_HOLD_SECONDS = 30
PAGE_BYTES = 2048
MAGIC, VERSION, DATA = 0xCAFEBABE, 2, 512


class Failure(RuntimeError):
    pass


class CaptureFailure(Failure):
    """The log capture lost evidence that cannot be read again: the run fails with it, and a retry that waits for
    the guest does not swallow it."""


# ---- What the guest prints, read.

def script_seconds(script: str, name: str) -> int:
    """A wait of the boot script, from its `name=` line."""
    match = re.search(rf"^{name}=(\d+)\s*$", script, re.M)
    if match is None:
        raise Failure(f"the boot script has no {name}= line")
    return int(match[1])


def boot_settle_seconds(script: str) -> int:
    """The seconds the boot script gives the station to join before it saves the network again."""
    return script_seconds(script, "settle")


@dataclass(frozen=True)
class Waits:
    """How long a section waits after a boot for the script, which gives the station `settle` seconds before
    it saves the network again, and for the platform to join a network the script saved: `rejoin` more. The
    script no longer waits for that join, so `rejoin` is the platform's time, not a value of the script."""
    settle: int
    rejoin: int

    @property
    def quiet(self) -> int:
        """Long enough after a boot to have seen the script save the network again, if it would."""
        return self.settle + MARGIN_SECONDS

    @property
    def repair(self) -> int:
        """Long enough after a boot for the script to save the network again and the station to join it."""
        return self.settle + self.rejoin + MARGIN_SECONDS


def wifi_enabled(status: str) -> bool:
    """Whether `cmd wifi status` says Wi-Fi is on."""
    return "Wifi is enabled" in status.splitlines()


def wifi_joined(status: str) -> bool:
    """Whether `cmd wifi status` says the station has joined VirtWifi."""
    return f'Wifi is connected to "{SSID}"' in status.splitlines()


def saved_ssids(listing: str) -> list[str]:
    """The SSIDs `cmd wifi list-networks` lists (a network twice, once per security type, counts once)."""
    found: list[str] = []
    for line in listing.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and fields[0].isdigit() and fields[1] not in found:
            found.append(fields[1])
    return found


def network_id(listing: str) -> int | None:
    """The network ID of VirtWifi, or None when it is not saved."""
    for line in listing.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and fields[0].isdigit() and fields[1] == SSID:
            return int(fields[0])
    return None


def selection_status(dump: str) -> str | None:
    """The first network selection status `dumpsys wifi` prints, e.g. NETWORK_SELECTION_ENABLED."""
    match = re.search(r"^\s*NetworkSelectionStatus (NETWORK_SELECTION_\w+)\s*$", dump, re.M)
    return match[1] if match else None


def selection_disable_reason(dump: str) -> str | None:
    """The reason of a disabled network, or None."""
    match = re.search(r"^\s*mNetworkSelectionDisableReason (NETWORK_SELECTION_DISABLED_\w+)\s*$", dump, re.M)
    return match[1] if match else None


def no_internet_count(dump: str) -> int:
    """How many failed internet checks the platform counted against the network, summed over both reasons.

    The dump prints the network more than once, each time with its counters, so a reason counts once."""
    total = 0
    for reason in ("PERMANENT", "TEMPORARY"):
        counts = re.findall(rf"^NETWORK_SELECTION_DISABLED_NO_INTERNET_{reason} counter:(\d+)\s*$", dump, re.M)
        total += max((int(count) for count in counts), default=0)
    return total


UNPROBED_MARKER = "Wi-Fi is validated without probes"
VALIDATED_WIFI = re.compile(r"^\s*NetworkAgentInfo\{[^\n]*ni\{WIFI CONNECTED[^\n]*(?<![A-Z_])IS_VALIDATED(?![A-Z_])", re.M)


def failure_reached_platform(network_stack_dump: str, image_under_test: bool) -> str | None:
    """`dumpsys network_stack`: what the validation log says the platform did with the injected failures, or None
    when it says nothing of them. A platform that counts no failed check against the network is trusted to have
    been at risk only when its own log shows the failures reached it. The image under test validates Wi-Fi first
    (route p0029), so its failed check leaves the network validated: the failed check, or the private DNS name
    evaluated, shows it. The statement that Wi-Fi is validated without probes is the true statement of an earlier
    release's platform alone, so only a previous release's guest is told by it; on the image under test it is not
    accepted (see `check_not_disabled`, which fails on it)."""
    statements = [(CHECK_FAILED_LINE, "its internet check failed and the network stays validated"),
                  (PRIVATE_DNS_PROBE, "it evaluated the private DNS name")]
    if not image_under_test:
        statements.append((UNPROBED_MARKER, "it validates Wi-Fi without probes"))
    for line, what in statements:
        if line in network_stack_dump:
            return what
    return None


def wifi_validated(connectivity_dump: str) -> bool:
    """`dumpsys connectivity`: the connected Wi-Fi network is validated."""
    return VALIDATED_WIFI.search(connectivity_dump) is not None


def metered_override(dump: str) -> int | None:
    """The metered override of the saved network (0 none, 1 metered, 2 not metered)."""
    match = re.search(r"^\s*meteredOverride (\d+)\s*$", dump, re.M)
    return int(match[1]) if match else None


def autojoin_values(dump: str) -> list[bool]:
    """Auto-connect of the saved network, one value for each time `dumpsys wifi` prints it."""
    return [value == "true" for value in re.findall(r"\ballowAutojoin=(true|false)\b", dump)]


def configuration_autojoin(dump: str, network: int) -> list[bool]:
    """Auto-connect of the saved network with this ID, one value for each time `dumpsys wifi` prints it. Only the
    values under a configuration header (`ID: n SSID: ...`) of that ID count, as the boot script reads them:
    a header for another ID ends the configuration, and the SSID text of a header is never searched."""
    values: list[bool] = []
    current: int | None = None
    for line in dump.splitlines():
        # The configuration of a connected network has a star before its ID; the others have none. A header
        # starts the line or follows the `=` of a map entry: a log line with the same text is no header.
        header = re.search(r"(?:^|=)(?:\* )?ID: (\d+) SSID: ", line)
        if header:
            current = int(header[1])
        elif current == network:
            values += [value == "true" for value in re.findall(r"\ballowAutojoin=(true|false)\b", line)]
    return values


def avc_denials(log: str, domain: str) -> list[str]:
    """The SELinux denials in a log that name the domain, its executable or its property type, as the
    source or the target."""
    named = re.compile(rf"u:(?:object_)?r:{domain}(?:_exec|_prop)?:")
    return [line.strip() for line in log.splitlines()
            if "avc:" in line and "denied" in line and named.search(line)]


def repair_logged(log: str) -> bool:
    """Whether the boot script's log shows that it saved the network again, for either reason it logs: Wi-Fi on
    with nothing joined, or the network missing from the Wi-Fi service's list at marker 1."""
    return SCRIPT_LOG_LINE in log or NOT_STORED_LINE in log


def not_stored_logged(log: str) -> bool:
    """Whether the boot script's log shows that it found the network missing at marker 1 and saved it again."""
    return NOT_STORED_LINE in log


def left_alone_logged(log: str) -> bool:
    """Whether the boot script's log shows that it found Auto-connect off and left the network alone."""
    return LEFT_ALONE_LINE in log


def marker_one_logged(log: str) -> bool:
    """Whether the boot script's log shows that its boot ended with the marker at 1."""
    return UNSTORED_LINE in log


def marker_two_logged(log: str) -> bool:
    """Whether the boot script's log shows that it set the marker to 2 after finding the network stored and
    joined."""
    return SETTLED_LINE in log


PRIVATE_DNS_BROKEN_TOKEN = re.compile(r"\sPrivateDnsBroken(?=[\s\]])")


def wifi_networks(dump: str) -> dict[int, frozenset[str]]:
    """The capabilities of each Wi-Fi network `dumpsys connectivity` lists, by network ID. NetworkAgentInfo prints
    its `nc{...}` as NetworkCapabilities.toString does: the capabilities are the `&`-joined list after
    `Capabilities: `, and broken private DNS is a token of its own (` PrivateDnsBroken`, after the SSID), not a
    capability. It is added to the set as PRIVATE_DNS_BROKEN."""
    found: dict[int, frozenset[str]] = {}
    for line in dump.splitlines():
        match = re.match(r"\s*NetworkAgentInfo\{network\{(\d+)\}.*?\bni\{WIFI\b.*?"
                         r"\bnc\{(\[ Transports: WIFI Capabilities: ([A-Z_&]+).*)", line)
        if match:
            capabilities = set(match[3].split("&"))
            if PRIVATE_DNS_BROKEN_TOKEN.search(match[2].split("  factorySerialNumber=")[0]):
                capabilities.add("PRIVATE_DNS_BROKEN")
            found[int(match[1])] = frozenset(capabilities)
    return found


def last_validated(dump: str) -> dict[int, int]:
    """When the platform last gave each Wi-Fi network the validated capability (`lastValidated`, which it
    sets only when the capability changes, so it stays put while a check reports on a validated network)."""
    found: dict[int, int] = {}
    for line in dump.splitlines():
        match = re.match(r"\s*NetworkAgentInfo\{network\{(\d+)\}.*?\bni\{WIFI\b.*?\blastValidated (\d+)", line)
        if match:
            found[int(match[1])] = int(match[2])
    return found


def newest_wifi_network(dump: str) -> int | None:
    """The ID of the Wi-Fi network `dumpsys connectivity` lists with the highest ID (the one the station
    joined last), or None."""
    return max(wifi_networks(dump), default=None)


def validation_problems(capabilities: frozenset[str] | None) -> list[str]:
    """What is wrong with a Wi-Fi network that must stay validated: not listed, not validated, partial,
    behind a sign-in page, or with its private DNS reported broken."""
    if capabilities is None:
        return ["dumpsys connectivity lists no Wi-Fi network"]
    problems = []
    if "VALIDATED" not in capabilities:
        problems.append("the network is not validated")
    for name in ("PARTIAL_CONNECTIVITY", "CAPTIVE_PORTAL", "PRIVATE_DNS_BROKEN"):
        if name in capabilities:
            problems.append(f"the network has {name}")
    return problems


def validation_entries(dump: str) -> dict[int, list[tuple[str, str]]]:
    """The entries (time stamp, message) of each network's validation log in `dumpsys network_stack`, by
    network ID, oldest first."""
    logs: dict[int, list[tuple[str, str]]] = {}
    current: list[tuple[str, str]] | None = None
    in_section = False
    for line in dump.splitlines():
        if line.startswith("Validation logs"):
            in_section = True
            continue
        if not in_section:
            continue
        # The log's tag is the network and its name (`<id> - <name>`, the SSID or the network's extra info).
        header = re.fullmatch(r"(\d+) - .*", line)
        if header:
            current = logs.setdefault(int(header[1]), [])
            continue
        entry = re.fullmatch(r"\s+(\d{4}-\d\d-\d\dT[\d:.]+) - (.*)", line)
        if entry and current is not None:
            current.append((entry[1], entry[2]))
        elif line.strip():
            current = None
            if not line.startswith(" "):
                in_section = False
    return logs


def validation_logs(dump: str) -> dict[int, list[str]]:
    """The messages of each network's validation log in `dumpsys network_stack`, by network ID."""
    return {net_id: [message for _, message in entries] for net_id, entries in validation_entries(dump).items()}


class LogUnion:
    """The entries of a validation log collected over several polls. The log keeps 20 lines, so a section
    that counts over a long time reads it often and keeps each entry (its time stamp and message) once."""

    def __init__(self) -> None:
        self.entries: dict[tuple[str, str], None] = {}

    def add(self, entries: list[tuple[str, str]]) -> None:
        for entry in entries:
            self.entries.setdefault(entry, None)

    def count(self, text: str) -> int:
        return sum(1 for _, message in self.entries if text in message)


def check_log_problems(messages: list[str], expected: str, probes_failed: bool = False) -> list[str]:
    """What the validation log of the network lacks: the line of the check's result, and with `probes_failed`
    a failed HTTP and a failed HTTPS probe (a page answer ends the probing before the HTTPS probe fails)."""
    problems = []
    if not any(expected in message for message in messages):
        problems.append(f"the validation log has no line {expected!r}")
    if probes_failed:
        for probe in ("PROBE_HTTP", "PROBE_HTTPS"):
            if not any(message.startswith(f"{probe} ") and "Probe failed" in message for message in messages):
                problems.append(f"the validation log shows no failed {probe}")
    return problems


def first_validation_problems(messages: list[str]) -> list[str]:
    """Whether the network was validated before its first check: the log's reason line comes before any
    probe of the check (the private DNS probe of a strict name comes before the validation, as stock)."""
    reason = next((index for index, message in enumerate(messages) if VALIDATED_FIRST_LINE in message), None)
    probe = next((index for index, message in enumerate(messages)
                  if re.match(r"PROBE_(?!PRIVDNS)\w+ ", message)), None)
    if reason is None:
        return [f"the validation log has no line {VALIDATED_FIRST_LINE!r}: the network was not validated "
                "before its check"]
    if probe is not None and probe < reason:
        return ["a probe of the check completed before the network was validated"]
    return []


# The line the validation log has for an internet probe that completed (an answer or a failure; the DNS probe that
# comes before a request is not one).
PROBE_COMPLETION = re.compile(r"PROBE_(?:HTTP|HTTPS|FALLBACK) ")


def completed_before_problems(messages: list[str]) -> list[str]:
    """While the first request of the section is held, the network's log holds no internet probe or check that
    completed: an attempt that completed or failed before the held one means the held request is a retry, not
    the first check."""
    done = [message for message in messages
            if PROBE_COMPLETION.match(message) or CHECK_PASSED_LINE in message or CHECK_FAILED_LINE in message]
    return [f"a probe or check had completed before the held request, so it is not the first: {done[0][:160]!r}"] \
        if done else []


def first_completion_problems(messages: list[str], scenario: str, code: int) -> list[str]:
    """After the held request is answered, the first probe that completed in the network's log is that request's
    (an HTTP probe of the scenario's URL that received `code`), and no check result comes before it."""
    completions = [index for index, message in enumerate(messages) if PROBE_COMPLETION.match(message)]
    if not completions:
        return ["the validation log shows no probe that completed"]
    first = messages[completions[0]]
    problems = []
    if not (first.startswith("PROBE_HTTP ") and f"{scenario}/generate_204" in first and f" ret={code} " in first
            and "Probe failed" not in first):
        problems.append(f"the first probe to complete is not the held request's ({code} for {scenario}): "
                        f"{first[:160]}")
    results = [index for index, message in enumerate(messages)
               if CHECK_PASSED_LINE in message or CHECK_FAILED_LINE in message]
    if results and results[0] < completions[0]:
        problems.append("a check result is logged before the first probe completed")
    return problems


def passed_after_failed_problems(messages: list[str]) -> list[str]:
    """A new passing check after the failed ones: the last passing line comes after the last failed one."""
    passed = [index for index, message in enumerate(messages) if CHECK_PASSED_LINE in message]
    failed = [index for index, message in enumerate(messages) if CHECK_FAILED_LINE in message]
    if not passed:
        return [f"the validation log has no line {CHECK_PASSED_LINE!r}"]
    if failed and max(failed) > max(passed):
        return ["the last check in the validation log failed after the one that passed"]
    return []


def probe_answer_problems(messages: list[str], scenario: str, code: int) -> list[str]:
    """Whether the current network's HTTP probe of this scenario's URL received the answer `code`: a line
    of the validation log names the URL and the code (`ret=`), and is not a probe that failed (a request
    that the server saw and the probe timed out on is no answer)."""
    lines = [message for message in messages if message.startswith("PROBE_HTTP ") and scenario in message]
    if not lines:
        return [f"the validation log shows no HTTP probe of {scenario}"]
    answered = [line for line in lines if f" ret={code} " in line and "Probe failed" not in line]
    if not answered:
        failures = [line for line in lines if "Probe failed" in line]
        if failures:
            return [f"the HTTP probe of {scenario} failed without an answer: {failures[-1][:160]}"]
        return [f"the HTTP probe of {scenario} did not receive {code}: {lines[-1][:160]}"]
    if code == 200 and any("interpreted as failed" in line and scenario in line for line in messages):
        return [f"the 200 answer of {scenario} was taken for a failed answer, not a page"]
    return []


def https_off_problems(messages: list[str]) -> list[str]:
    """With the HTTPS flag at 0 a check asks the HTTP probe alone: a log line for an HTTPS probe means the platform
    did not take the flag, and the answer that passed is not the host's server's alone."""
    sent = [message for message in messages if message.startswith("PROBE_HTTPS ")]
    return [f"the platform sent an HTTPS probe with the HTTPS flag at 0: {sent[0][:160]}"] if sent else []


def stock_answer_problems(messages: list[str]) -> list[str]:
    """Whether a check's probes asked the stock servers and both received 204: a line of the validation log for the
    HTTP probe of the stock URL and one for the HTTPS probe of the stock URL, each with `ret=204` and neither a probe
    that failed (the URLs the image's platform probes with no setting)."""
    problems = []
    for probe, url in (("PROBE_HTTP", STOCK_HTTP_URL), ("PROBE_HTTPS", STOCK_HTTPS_URL)):
        lines = [message for message in messages if message.startswith(f"{probe} ")]
        if not any(line.startswith(f"{probe} {url} ") and " ret=204 " in line and "Probe failed" not in line
                   for line in lines):
            last = f": the last is {lines[-1][:160]!r}" if lines else ""
            problems.append(f"the validation log shows no {probe} of {url} that received 204{last}")
    return problems


def retry_problems(times: list[float]) -> list[str]:
    """The first checks of a network that fails them come back after the stock back-off: with the times at
    which their requests reached the server, at least RETRIES_NEEDED of them, and each gap at least as long
    as the delay that follows the failure before it (1 s, 2 s, 4 s)."""
    if len(times) < RETRIES_NEEDED:
        return [f"only {len(times)} of {RETRIES_NEEDED} checks reached the server"]
    problems = []
    for index in range(1, RETRIES_NEEDED):
        gap = times[index] - times[index - 1]
        floor = BACKOFF_FLOOR * INITIAL_BACKOFF_SECONDS * 2 ** (index - 1)
        if gap < floor:
            problems.append(f"check {index + 1} came {gap:.1f} s after the one before, under the back-off "
                            f"of {floor:.1f} s")
    return problems


def private_dns_failed_problems(capabilities: frozenset[str] | None) -> list[str]:
    """What differs from the network the platform leaves when its strict private DNS name fails: not validated,
    with its private DNS reported broken."""
    if capabilities is None:
        return ["dumpsys connectivity lists no Wi-Fi network"]
    problems = []
    if "VALIDATED" in capabilities:
        problems.append("the network is validated")
    if "PRIVATE_DNS_BROKEN" not in capabilities:
        problems.append("the network does not have PRIVATE_DNS_BROKEN")
    return problems


def request_entries(before: list[tuple[str, str]], entries: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """The entries a validation log gained after `before`, the entries it held when a request was made, in time
    order. An older line with the same words is in `before`, so it is not the request's."""
    seen = set(before)
    return sorted((entry for entry in entries if entry not in seen), key=lambda entry: entry[0])


def forced_request_problems(before: list[tuple[str, str]], entries: list[tuple[str, str]],
                            checked: bool) -> list[str]:
    """What the validation log of the network shows of one forced re-evaluation request: a NEW line that says
    it was forced (an older one proves nothing), and then either no internet check anywhere from the request to
    the end of the hold (a network whose private DNS failed is not checked, so a check between two such lines
    fails too), or, with `checked`, an internet check after that line."""
    gained = request_entries(before, entries)
    forced = [index for index, (_, message) in enumerate(gained) if message.startswith(FORCED_LINE)]
    if not forced:
        return [f"the validation log has no new {FORCED_LINE!r} line after the request: the platform did not "
                "take the forced re-evaluation"]
    checks = [index for index, (_, message) in enumerate(gained) if "Wi-Fi internet check" in message]
    if checked:
        if not any(index > forced[0] for index in checks):
            return ["the validation log has no internet check after the forced re-evaluation"]
        return []
    return [f"an internet check ran after the forced re-evaluation request: {gained[checks[0]][1]!r}"] if checks else []


def connectivity_notifications(dump: str) -> list[str]:
    """The records `dumpsys notification` lists that ConnectivityService posted (no Internet, partial
    connectivity, lost Internet, Sign in to network all carry the tag ConnectivityNotification:<netId>)."""
    return [line.strip()[:200] for line in dump.splitlines()
            if re.search(r"\btag=ConnectivityNotification:\d+", line)]


def captive_portal_activities(dump: str) -> list[str]:
    """The lines of `dumpsys activity activities` that name the captive portal login activity."""
    return [line.strip()[:200] for line in dump.splitlines()
            if re.search(r"captiveportallogin|CaptivePortalLogin", line)]


@dataclass(frozen=True)
class UiNode:
    text: str
    description: str
    package: str


def ui_nodes(hierarchy: str) -> list[UiNode]:
    """The nodes of a `uiautomator dump --windows` (the status bar is a window of its own)."""
    start = hierarchy.find("<?xml")
    if start < 0:
        start = hierarchy.find("<")
    if start < 0:
        raise Failure(f"the UI dump is not XML: {hierarchy[:200]!r}")
    # What the tool prints after the document ("UI hierarchy dumped to: ...") is not part of it.
    end = -1
    for closing in ("</displays>", "</hierarchy>"):
        found = hierarchy.rfind(closing)
        if found >= 0 and found + len(closing) > end:
            end = found + len(closing)
    try:
        root = ElementTree.fromstring(hierarchy[start:end] if end >= 0 else hierarchy[start:])
    except ElementTree.ParseError as error:
        raise Failure(f"the UI dump is not XML: {error}") from error
    return [UiNode(node.get("text", ""), node.get("content-desc", ""), node.get("package", ""))
            for node in root.iter("node")]


def summary_limited(nodes: list[UiNode]) -> bool:
    """Whether the Wi-Fi list says "Limited connection" (its summary reads "Connected / Limited connection")."""
    return any(LIMITED_CONNECTION in node.text for node in nodes)


def summary_connected(nodes: list[UiNode]) -> bool:
    """Whether the Wi-Fi list shows the network as plain "Connected"."""
    return any(node.text == "Connected" for node in nodes)


def status_bar_no_internet(nodes: list[UiNode]) -> bool:
    """Whether the status bar's Wi-Fi icon of a connected network speaks "No internet" (its description is the
    signal level, a comma and "No internet"; the icon of a disconnected Wi-Fi says "No internet" as well)."""
    return any(node.package == SYSTEMUI_PACKAGE and NO_INTERNET in node.description
               and "disconnected" not in node.description.lower() for node in nodes)


def status_bar_present(nodes: list[UiNode]) -> bool:
    """Whether the dump holds the status bar's Wi-Fi icon at all (a mark is judged absent only where it
    could be seen)."""
    return any(node.package == SYSTEMUI_PACKAGE and re.search(r"wi-?fi", node.description, re.I)
               for node in nodes)


def mark_problems(nodes: list[UiNode], shown: bool) -> list[str]:
    """What the screen shows against what Android's Wi-Fi mark must say: with `shown`, the list's "Limited
    connection" and the status bar's "No internet"; without, plain "Connected" and no "No internet", the
    status bar's Wi-Fi icon being in the dump."""
    problems = []
    if shown:
        if not summary_limited(nodes):
            problems.append(f"the Wi-Fi list does not say {LIMITED_CONNECTION!r}")
        if not status_bar_no_internet(nodes):
            problems.append(f"the status bar's Wi-Fi icon does not say {NO_INTERNET!r}")
    else:
        if summary_limited(nodes):
            problems.append(f"the Wi-Fi list still says {LIMITED_CONNECTION!r}")
        if not summary_connected(nodes):
            problems.append("the Wi-Fi list does not say 'Connected'")
        if not status_bar_present(nodes):
            problems.append("the dump has no status bar Wi-Fi icon, so its mark cannot be judged absent")
        elif status_bar_no_internet(nodes):
            problems.append(f"the status bar's Wi-Fi icon still says {NO_INTERNET!r}")
    return problems


def overlay_enabled(listing: str) -> bool:
    """Whether `cmd overlay list` shows the Wi-Fi module's overlay enabled."""
    return f"[x] {OVERLAY_PACKAGE}" in listing.splitlines()


def overlay_integers(apk: bytes) -> dict[str, list[int]]:
    """The integer resources of an overlay APK, by the verifier's reader of compiled resource tables."""
    verifier = load_verifier()
    with zipfile.ZipFile(io.BytesIO(apk)) as archive:
        return verifier.resource_integers(archive.read("resources.arsc"))


def load_verifier():
    sys.path.insert(0, str(ANDROID_TOOLS))
    try:
        return load_module("verify_target_files", ANDROID_TOOLS / "verify-target-files.py")
    finally:
        sys.path.remove(str(ANDROID_TOOLS))


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: "Guest | None" = None, interval: float = 2.0) -> None:
    """Calls `check` until it returns None (done); its last message is the failure."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        message = check()
        if message is None:
            return
        time.sleep(interval)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


class ProbeServer:
    """The host's answer to the guest's probes: a sign-in page (a redirect), a block page (a 200 answer with a
    body) or the expected 204. The guest cannot reach the machine the runner runs on (the provider refuses
    its connections to every address of a host interface, darwin/README.md), so the guest reaches the server
    through `adb reverse` on its own loopback, as any host page server is reached."""

    MODES = ("redirect", "page", "expected")

    def __init__(self) -> None:
        self.mode = "expected"
        self.seen: list[tuple[float, str, int]] = []
        self.lock = threading.Lock()
        # The hold on the first HTTP probe of one scenario, as (scenario, state): armed until that request
        # arrives, open while its answer is held, released once it may go out. The state changes only under the
        # lock, together with the record of the request, and is read as one value.
        self.hold: tuple[str, str] | None = None
        self.released = threading.Event()
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:  # noqa: N802
                mode = server.mode
                if server.arrive(self.path, {"redirect": 302, "page": 200}.get(mode, 204)):
                    server.released.wait(HELD_ANSWER_SECONDS)
                    server.release()
                if mode == "redirect":
                    self.send_response(302)
                    self.send_header("Location", f"http://127.0.0.1:{server.port}/login")
                    self.send_header("Content-Length", "0")
                    body = b""
                elif mode == "page":
                    body = server.page()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html")
                    self.send_header("Content-Length", str(len(body)))
                else:
                    self.send_response(204)
                    body = b""
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            do_HEAD = do_GET  # noqa: N815

            def log_message(self, *arguments: object) -> None:
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    @staticmethod
    def page() -> bytes:
        """A page that is not the expected answer: 2 KiB of HTML."""
        head = b"<html><head><title>Terms of use</title></head><body><p>Sign in to continue.</p><!--"
        tail = b"--></body></html>"
        return head + b"x" * (PAGE_BYTES - len(head) - len(tail)) + tail

    @staticmethod
    def scenario() -> str:
        """A path prefix no other section's probes use, for the URLs of one section."""
        return "/s" + uuid.uuid4().hex[:8]

    def record(self, path: str, status: int = 0, when: float | None = None) -> None:
        """A request the server saw, with the status it answers with, and when."""
        with self.lock:
            self.seen.append((time.monotonic() if when is None else when, path, status))

    def arrive(self, path: str, status: int = 0, when: float | None = None) -> bool:
        """A request is at the server: it is recorded and, when it is the held one, opened, in one step that no
        reader splits. True when its answer is held, which the caller does not send until the hold is over."""
        with self.lock:
            self.seen.append((time.monotonic() if when is None else when, path, status))
            if (self.hold is None or self.hold[1] != "armed" or not path.startswith(self.hold[0] + "/")
                    or not path.endswith("/generate_204")):
                return False
            self.hold = (self.hold[0], "open")
            return True

    def requests(self, scenario: str | None = None) -> list[tuple[float, str, int]]:
        """The requests (time, path, status), in order, of one scenario or all."""
        with self.lock:
            return [seen for seen in self.seen if scenario is None or seen[1].startswith(scenario + "/")]

    def probes(self, scenario: str | None = None) -> list[str]:
        """The paths the guest's probes asked for, in order."""
        return [path for _, path, _ in self.requests(scenario)]

    def checks(self, scenario: str) -> list[float]:
        """When the HTTP probes of a scenario arrived: one for each check."""
        return [when for when, path, _ in self.requests(scenario) if path.endswith("/generate_204")]

    def hold_first(self, scenario: str) -> None:
        """The answer to the first HTTP probe of the scenario is held (for at most HELD_ANSWER_SECONDS) until
        `release`: the check it belongs to stays unanswered, and `hold_state` says so."""
        with self.lock:
            self.hold = (scenario, "armed")
            self.released.clear()

    def hold_state(self, scenario: str) -> str:
        """Where the hold on the scenario is, as one value: `idle` (none), `armed` (its request has not
        arrived), `open` (arrived, answer held) or `released` (the answer may go out)."""
        with self.lock:
            return self.hold[1] if self.hold is not None and self.hold[0] == scenario else "idle"

    def is_open(self, scenario: str) -> bool:
        """Whether the held request of the scenario is at the server and not answered."""
        return self.hold_state(scenario) == "open"

    def release(self) -> None:
        """Ends the hold: the held request is answered, and none arriving later is held."""
        with self.lock:
            if self.hold is not None:
                self.hold = (self.hold[0], "released")
            self.released.set()

    def clear(self) -> None:
        with self.lock:
            self.seen.clear()
            self.hold = None
            self.released.clear()

    def close(self) -> None:
        self.release()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=10)


class Console:
    """The guest's management console: one datagram request, one reply."""

    def __init__(self, directory: pathlib.Path, reply_directory: pathlib.Path) -> None:
        self.directory = directory
        self.reply = reply_directory / "mc"

    def request(self, command: str, seconds: float = 90) -> str:
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self.reply.unlink(missing_ok=True)
            endpoint.bind(str(self.reply))
            data = command.encode()
            endpoint.sendto(struct.pack("<III", MAGIC, VERSION, len(data)) + data.ljust(DATA, b"\0"),
                            str(self.directory / "mconsole"))
            endpoint.settimeout(seconds)
            try:
                answer = endpoint.recv(12 + DATA)
            except socket.timeout:
                raise Failure(f"mconsole {command}: no reply in {seconds:.0f} s") from None
            error, _, length = struct.unpack_from("<III", answer)
            text = answer[12:12 + length].decode(errors="replace").rstrip("\0")
            if error:
                raise Failure(f"mconsole {command} failed: {text!r}")
            return text
        finally:
            endpoint.close()
            self.reply.unlink(missing_ok=True)


# The echo server that the guest runs on its Wi-Fi address (see GUEST_WIFI_ADDRESS): the first line of a connection
# names it, and the guest logs its start and its end, with its clock and the status of the reading (1 for a reset).
ECHO_SERVER = f"""#!/system/bin/sh
exec nc -s {GUEST_WIFI_ADDRESS} -p {ECHO_PORT} -L sh {ECHO_DIRECTORY}/echo.sh
"""
ECHO_CONNECTION = f"""#!/system/bin/sh
log={ECHO_DIRECTORY}/conn.log
read name
echo "open $name $(date +%s.%N)" >> $log
echo "$name"
cat
rc=$?
echo "close $name $(date +%s.%N) rc=$rc" >> $log
"""


class Echo:
    """One TCP connection that the host opened into the guest's Wi-Fi address (the launcher's forward to the echo
    server), which the guest echoes line by line."""

    def __init__(self, port: int, name: str) -> None:
        self.name = name
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=ECHO_SECONDS)
        self.buffer = b""

    def send(self, text: str) -> None:
        self.sock.sendall(f"{text}\n".encode())

    def expect(self, text: str, seconds: float = ECHO_SECONDS) -> str | None:
        """None once the guest has sent the line `text` (lines before it are passed over), else what happened
        instead: the guest ended the connection, it broke, or nothing came."""
        deadline = time.monotonic() + seconds
        while True:
            while b"\n" in self.buffer:
                line, _, self.buffer = self.buffer.partition(b"\n")
                if line.decode(errors="replace").strip() == text:
                    return None
            left = deadline - time.monotonic()
            if left <= 0:
                return f"no {text!r} in {seconds:.0f} s (the guest sent {self.buffer!r})"
            self.sock.settimeout(left)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return f"no {text!r} in {seconds:.0f} s (the guest sent {self.buffer!r})"
            except OSError as problem:
                return f"the connection broke ({problem})"
            if not chunk:
                return "the guest ended the connection"
            self.buffer += chunk

    def exchange(self, text: str, seconds: float = ECHO_SECONDS) -> str | None:
        """None when the guest echoes a line sent now, else what came instead."""
        try:
            self.send(text)
        except OSError as problem:
            return f"the connection broke ({problem})"
        return self.expect(text, seconds)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class EchoServer:
    """For one section: the guest's echo server on its Wi-Fi address, and the host's connections into it."""

    def __init__(self, guest: "Guest") -> None:
        self.guest = guest
        self.connections: list[Echo] = []

    def __enter__(self) -> "EchoServer":
        self.guest.start_echo()
        return self

    def __exit__(self, *_: object) -> None:
        for connection in self.connections:
            connection.close()
        self.guest.stop_echo()

    def open(self, name: str) -> Echo:
        """A connection that the guest has accepted: it echoes its name. The server is given time to listen."""
        last = "not tried"
        for _ in range(ECHO_ATTEMPTS):
            try:
                connection = Echo(self.guest.forward_port, name)
            except OSError as problem:
                last = f"connect: {problem}"
                time.sleep(1)
                continue
            problem = connection.exchange(name)
            if problem is None:
                self.connections.append(connection)
                return connection
            last = problem
            connection.close()
            time.sleep(1)
        raise Failure(f"the guest's echo server does not answer at {GUEST_WIFI_ADDRESS}:{ECHO_PORT} through the "
                      f"launcher's forward (host port {self.guest.forward_port}): {last}")


@dataclass
class Wake:
    """One watched wake: the line the runner logged before the suspend, when the wake was answered (the
    monotonic clock), and what the guest's request WAKE_REQUEST_SECONDS after it printed."""
    mark: str
    at: float = 0.0
    request: str = ""


class Guest:
    capture: "LogCapture | None" = None
    wake: Wake | None = None
    forward_port: int = DEFAULT_FORWARD_PORT

    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 console: Console, private: pathlib.Path) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.console = console
        self.private = private
        # The boot script's denials found by every read of the log so far.
        self.denials: list[str] = []
        self.denial_reads = 0
        # Set for a guest of the release under test: one log stream for each boot, read for crash evidence.
        self.capture: LogCapture | None = None
        self.stream_id = uuid.uuid4().hex[:8]

    @property
    def image_under_test(self) -> bool:
        """A guest of the release under test is the one whose log is captured; a previous release's is not."""
        return self.capture is not None

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False, timeout=timeout)

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        text = result.stdout.decode(errors="replace")
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {text.strip()[-500:]}")
        return text

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])
        if self.capture is not None:
            self.capture.check_stream()

    def open_log_stream(self, boot: int, segment: int) -> LogStream:
        return LogStream(self.adb, self.serial, self.private / f"logcat-{self.stream_id}-{boot}-{segment}.txt")

    # What the guest says about the network.
    def status(self) -> str:
        return self.shell("cmd wifi status")

    def listing(self) -> str:
        return self.shell("cmd wifi list-networks")

    def dump(self) -> str:
        return self.shell("dumpsys wifi", timeout=180)

    def network_stack_dump(self) -> str:
        return self.shell("dumpsys network_stack", timeout=180)

    def connectivity_dump(self) -> str:
        return self.shell("dumpsys connectivity", timeout=180)

    def joined(self) -> bool:
        return wifi_joined(self.status())

    def has_address(self) -> bool:
        """Whether the station's interface holds an IPv4 address (the lease)."""
        return " inet " in self.shell("ip -4 addr show dev wlan0 2>/dev/null; true")

    def script_log(self) -> str:
        """What the boot script logged since this boot."""
        return self.shell(f"logcat -d -s {SCRIPT_LOG_TAG}")

    def read_denials(self) -> int:
        """Keeps the boot script's SELinux denials that the log holds now; returns how many denials it
        holds in all. The log is small and rotates, so it is read more than once and nothing is dropped."""
        log = self.shell("logcat -d -b all", timeout=180)
        for line in avc_denials(log, SCRIPT_DOMAIN):
            if line not in self.denials:
                self.denials.append(line)
        self.denial_reads += 1
        return log.count("avc:")

    def uptime(self) -> float:
        return float(self.shell("cat /proc/uptime").split()[0])

    def start_echo(self) -> None:
        """Starts the echo server on the station's address: the address must be the one the launcher forwards to."""
        held = self.shell("ip -4 -o addr show dev wlan0 2>/dev/null; true")
        if GUEST_WIFI_ADDRESS not in held:
            raise Failure(f"wlan0 does not hold {GUEST_WIFI_ADDRESS}, the address the launcher's forward reaches: "
                          f"{held.strip()[-200:] or 'no address'}")
        directory = self.private / "echo"
        directory.mkdir(exist_ok=True)
        self.shell(f"rm -rf {ECHO_DIRECTORY}; mkdir -p {ECHO_DIRECTORY}")
        for name, text in (("serve.sh", ECHO_SERVER), ("echo.sh", ECHO_CONNECTION)):
            (directory / name).write_text(text)
            pushed = self.adb_run("push", str(directory / name), f"{ECHO_DIRECTORY}/{name}", timeout=60)
            if pushed.returncode != 0:
                raise Failure(f"adb push {name}: {pushed.stdout.decode(errors='replace').strip()[-300:]}")
        self.shell(f"nohup sh {ECHO_DIRECTORY}/serve.sh > {ECHO_DIRECTORY}/serve.log 2>&1 < /dev/null & "
                   f"echo $! > {ECHO_DIRECTORY}/pid", timeout=20)

    def stop_echo(self) -> None:
        """Ends the echo server and every connection it still holds, by the process it started and by the script of
        its connections (the bracket keeps the pattern from matching the command that carries it)."""
        try:
            self.adb_run("shell", f"kill $(cat {ECHO_DIRECTORY}/pid) 2>/dev/null; "
                         f"pkill -f '[w]ifi-wake/echo.sh' 2>/dev/null; rm -rf {ECHO_DIRECTORY}", timeout=30)
        except (subprocess.TimeoutExpired, OSError):
            pass

    def echo_log(self) -> str:
        return self.shell(f"cat {ECHO_DIRECTORY}/conn.log 2>/dev/null; true")

    def mark_log(self) -> str:
        """A line in the guest's log, to read what follows it: the window of a stay."""
        token = f"wake-{uuid.uuid4().hex}"
        self.shell(f"log -t {SYNC_TAG} {token}")
        return token

    def log_text(self) -> str:
        return self.shell("logcat -d -v threadtime -b main -b system", timeout=120)

    def wake_request(self) -> str:
        """The guest's request (WAKE_REQUEST), as it printed it, or why it did not run."""
        try:
            return self.shell(WAKE_REQUEST, timeout=30)
        except (Failure, subprocess.TimeoutExpired) as problem:
            return f"the request did not run: {problem}"

    def freeze(self) -> None:
        """Stops the emulator process this runner started, as iOS stops the app once the guest is suspended."""
        self.__dict__["frozen"] = True
        try:
            os.kill(self.process.pid, signal.SIGSTOP)
        except ProcessLookupError:
            raise Failure("the emulator is gone: it cannot be stopped for the stay") from None

    def thaw(self) -> None:
        """Continues the emulator this runner stopped, if it did: a process that is gone is nothing to continue."""
        if not self.__dict__.pop("frozen", False):
            return
        try:
            os.kill(self.process.pid, signal.SIGCONT)
        except ProcessLookupError:
            pass

    def stay_frozen(self, during: Callable[[], None], seconds: float = FROZEN_STAY_SECONDS) -> None:
        """A stay with the emulator stopped, `seconds` long, in which `during` acts on the host's side at
        FROZEN_FIRST_SECONDS. Nothing of the guest runs, so nothing wakes it: its RTC alarm does not end the stay. The
        emulator is continued whatever happens."""
        self.freeze()
        try:
            time.sleep(FROZEN_FIRST_SECONDS)
            during()
            time.sleep(max(0.0, seconds - FROZEN_FIRST_SECONDS))
        finally:
            self.thaw()

    def diagnose(self) -> str:
        parts = []
        for title, command in (
                ("wifi status", "cmd wifi status | head -5"),
                ("saved networks", "cmd wifi list-networks"),
                ("selection", "dumpsys wifi | grep -E 'NetworkSelectionStatus|mNetworkSelectionDisableReason|"
                              "DISABLED_NO_INTERNET|meteredOverride' | head -12"),
                ("boot script log", f"logcat -d -s {SCRIPT_LOG_TAG} | tail -5")):
            try:
                parts.append(f"{title}:\n{self.shell(command, timeout=60).strip()}")
            except (Failure, subprocess.TimeoutExpired, OSError) as problem:
                parts.append(f"{title}: unavailable ({problem})")
        return "\n".join(parts)

    def connectivity(self) -> str:
        return self.shell("dumpsys connectivity", timeout=180)

    def network_stack(self) -> str:
        return self.shell("dumpsys network_stack", timeout=180)

    def notifications(self) -> str:
        return self.shell("dumpsys notification --noredact", timeout=180)

    def activities(self) -> str:
        return self.shell("dumpsys activity activities", timeout=180)

    def ui(self) -> str:
        """The windows on the screen: Settings, and the status bar as a window of its own."""
        return self.shell(f"uiautomator dump --windows {UI_DUMP} >/dev/null 2>&1; cat {UI_DUMP} 2>/dev/null; "
                          f"rm -f {UI_DUMP}", timeout=90)

    def script_service(self) -> str:
        """The state init keeps for the boot script's service: running, stopped, or empty before it started."""
        return self.shell(f"getprop {SERVICE_PROPERTY}").strip()

    def validation_entries(self, net_id: int) -> list[tuple[str, str]]:
        return validation_entries(self.network_stack()).get(net_id, [])

    def validation_log(self, net_id: int) -> list[str]:
        return [message for _, message in self.validation_entries(net_id)]

    def reevaluate(self, net_id: int) -> None:
        """Forces the platform to check the network again (`cmd connectivity reevaluate`, which reports the
        opposite of what the network is and so makes the platform evaluate it). A refusal, an unknown network
        or any error is a failure: a section that forces a check must see it run."""
        result = self.adb_run("shell", f"cmd connectivity reevaluate {net_id}", timeout=60)
        output = result.stdout.decode(errors="replace").strip()
        if result.returncode != 0 or re.search(r"Unknown network|Exception|Error|denied|not found", output, re.I):
            raise Failure(f"cmd connectivity reevaluate {net_id} (rc {result.returncode}): {output[-300:]}")

    def reverse(self, port: int) -> None:
        """The guest's loopback port to the host's server (the shared adb server, named with -s)."""
        result = self.adb_run("reverse", f"tcp:{port}", f"tcp:{port}", timeout=60)
        if result.returncode != 0:
            raise Failure(f"adb reverse tcp:{port}: {result.stdout.decode(errors='replace').strip()[-300:]}")

    def unreverse(self, port: int) -> None:
        self.adb_run("reverse", "--remove", f"tcp:{port}", timeout=60)

    def show_internet_page(self) -> None:
        """Settings' Internet page, in front, on a lit screen."""
        for command in ("svc power stayon true", "input keyevent KEYCODE_WAKEUP", "wm dismiss-keyguard"):
            self.shell(f"{command} >/dev/null 2>&1; true")
        started = self.shell("am start -W -a android.settings.WIFI_SETTINGS")
        if "Error" in started:
            raise Failure(f"starting the Internet page of Settings: {started.strip()[-300:]}")

    def leave_internet_page(self) -> None:
        self.shell("input keyevent KEYCODE_HOME >/dev/null 2>&1; true")

    # What the runner does to the guest.
    def inject_failing_checks(self) -> None:
        """Probes at a black hole, and a strict private DNS name that does not resolve: either fails the
        platform's internet check, so the stock platform disables a network it has not validated."""
        for key, value in PROBE_SETTINGS.items():
            self.shell(f"settings put global {key} {value}")

    def put_settings(self, settings: dict[str, str]) -> None:
        for key, value in settings.items():
            if key == USE_HTTPS_FLAG:
                self.put_flag(key, value)
            else:
                self.shell(f"settings put global {key} {value}")

    def delete_settings(self, keys: tuple[str, ...] | list[str]) -> None:
        for key in keys:
            if key == USE_HTTPS_FLAG:
                self.delete_flag(key)
            else:
                self.shell(f"settings delete global {key}")

    def read_flag(self, key: str) -> str:
        return self.shell(f"cmd device_config get {HTTPS_FLAG_NAMESPACE} {key}").strip()

    def put_flag(self, key: str, value: str) -> None:
        """Puts a flag of the namespace and reads it back: a flag that does not read as put never reached the
        platform, and a section that relies on it would be judging the wrong switch."""
        self.shell(f"cmd device_config put {HTTPS_FLAG_NAMESPACE} {key} {value}")
        read = self.read_flag(key)
        if read != value:
            raise Failure(f"cmd device_config put {HTTPS_FLAG_NAMESPACE} {key} {value}: it reads {read!r}")

    def delete_flag(self, key: str) -> None:
        """Deletes a flag of the namespace (one that is not set is no error) and reads it back as unset."""
        self.adb_run("shell", f"cmd device_config delete {HTTPS_FLAG_NAMESPACE} {key}")
        read = self.read_flag(key)
        if read not in ("", "null"):
            raise Failure(f"cmd device_config delete {HTTPS_FLAG_NAMESPACE} {key}: it still reads {read!r}")

    def read_setting(self, key: str) -> str:
        return self.shell(f"settings get global {key}").strip()

    def rejoin_with_settings(self, settings: dict[str, str], clear: tuple[str, ...]) -> None:
        """Sets the probe settings and deletes `clear`, then joins the network again: the platform reads them
        when the network's monitor starts, so they take effect only for a network that is joined after."""
        self.delete_settings(clear)
        self.put_settings(settings)
        self.forget_and_save()
        self.wait_joined(JOIN_SECONDS, "the station joins the network saved again")

    def forget_and_save(self) -> None:
        identifier = network_id(self.listing())
        if identifier is not None:
            self.shell(f"cmd wifi forget-network {identifier}")
        wait_for("the network is forgotten", 30, lambda: None if network_id(self.listing()) is None
                 else "still saved", self)
        self.shell(f"cmd wifi add-network {SSID} open")

    def wait_joined(self, seconds: float, what: str) -> None:
        wait_for(what, seconds, lambda: None if self.joined() else "the station has not joined", self)

    def log_size(self) -> int:
        """How much the launcher's own log (the host's messages and the guest kernel's console) holds now."""
        return gpu_suspend_log.log_size(self.log)

    def log_since(self, offset: int) -> str:
        return gpu_suspend_log.log_since(self.log, offset)

    def suspend_log(self) -> "gpu_suspend_log.Pending":
        """The mark of the guest's last suspend, pending until the section that suspended it settles it."""
        pending = self.__dict__.get("pending_suspend")
        if pending is None:
            pending = self.__dict__["pending_suspend"] = gpu_suspend_log.Pending(self.log_size, self.log_since)
        return pending

    def settle_suspend_log(self) -> str | None:
        """What the launcher's log shows of the GPU since the last suspend, and the mark ended: the end of a
        section calls it, whether the section passed or not, because output can arrive after the suspend step."""
        pending = self.__dict__.get("pending_suspend")
        return None if pending is None else pending.settle()

    def prepare_suspend(self) -> None:
        """What a suspend needs of the runner first: the last suspend's mark settled, and the log stream confirmed
        alive and carrying (it takes a second or so, so a step that must ask for the suspend at a given moment calls
        it before, and passes `prepared` to the suspend)."""
        earlier = self.suspend_log().settle()
        if earlier is not None:
            raise Failure(f"before another suspend: {earlier}")
        if self.capture is not None:
            self.capture.confirm(self)

    def suspend_and_wake(self, watch: bool = False, frozen: Callable[[], None] | None = None,
                         prepared: bool = False, stay: float = FROZEN_STAY_SECONDS) -> None:
        """The route the host's quiesce takes. With `watch`, a line is logged before the suspend and a request goes
        out WAKE_REQUEST_SECONDS after the wake (`self.wake`); with `frozen` the stay is the one of `stay_frozen`, `stay`
        seconds long.

        The log stream may end with the connection: that is expected until the guest answers again, and a stream that
        ended starts another segment. The launcher's log is marked before the suspend and the mark stays pending
        through the checks that follow the wake, as gpu_suspend_log says: it is scanned when this returns, again by
        the section that ends (Report.section), and before another suspend takes a new mark. A failure of the whole
        operation (the suspend, the wait for adb, the log stream's resume) is replaced by the GPU diagnosis when the
        log shows one, and kept otherwise."""
        if not prepared:
            self.prepare_suspend()
        pending = self.suspend_log()
        capture = self.capture
        if capture is not None:
            capture.going_down()
        pending.begin()
        try:
            self.suspend_and_wake_guest(watch, frozen, stay)
            if capture is not None:
                capture.resumed(self)
        except BaseException as problem:
            if capture is not None:
                capture.expect_end = False
            found = pending.settle() if isinstance(problem, Exception) else None
            if found is not None:
                kind = CaptureFailure if isinstance(problem, CaptureFailure) else Failure
                raise kind(f"{found} (the step had failed with: {problem})") from problem
            raise
        found = pending.peek()
        if found is not None:
            pending.settle()
            raise Failure(found)

    def suspend_and_wake_guest(self, watch: bool = False, frozen: Callable[[], None] | None = None,
                               stay: float = FROZEN_STAY_SECONDS) -> None:
        wake = Wake(self.mark_log()) if watch else None
        # A suspend can be refused while the guest is busy (EBUSY); it is asked for again.
        answer = ""
        for attempt in range(SUSPEND_ATTEMPTS):
            try:
                answer = self.console.request("suspend")
            except Failure as refused:
                if "Suspend failed" not in str(refused):
                    raise
                answer = str(refused)
            if "suspended" in answer and "failed" not in answer:
                break
            print(f"     suspend attempt {attempt + 1}: {answer!r}")
            time.sleep(10)
        else:
            raise Failure(f"suspend: refused {SUSPEND_ATTEMPTS} times: {answer!r}")
        if frozen is None:
            time.sleep(SUSPEND_STAY_SECONDS)
        else:
            self.stay_frozen(frozen, stay)
        answer = self.console.request("wake")
        if "running" not in answer:
            raise Failure(f"wake: the guest replied {answer!r}")
        if wake is not None:
            wake.at = time.monotonic()
            time.sleep(WAKE_REQUEST_SECONDS)
            wake.request = self.wake_request()
            self.wake = wake
        wait_for("adb answers after the wake", RESUME_SECONDS, lambda: self.adb_answers(), self)

    def adb_answers(self) -> str | None:
        subprocess.run([self.adb, "connect", self.serial], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30, check=False)
        try:
            return None if self.shell("echo up", timeout=30).strip() == "up" else "no answer"
        except (Failure, subprocess.TimeoutExpired):
            return "no answer"

    def restart(self) -> None:
        """A guest restart, shown by the uptime going back, then boot_completed again.

        The Wi-Fi service writes a change to its configuration store a little later, and a restart
        that follows at once loses it (a forgotten network comes back), so the restart waits. The log
        stream of the old boot ends with it and is read to its end; the new boot's stream starts as soon
        as adb answers on it, and reads the new buffers from their start."""
        time.sleep(STORE_SECONDS)
        capture = self.capture
        before = self.uptime()
        if capture is not None:
            capture.confirm(self)
            capture.going_down()
        self.adb_run("reboot", timeout=60)
        started = False

        def rebooted() -> str | None:
            nonlocal started
            if self.adb_answers() is not None:
                return "adb is down"
            try:
                if self.uptime() >= before:
                    return "the guest has not restarted"
                if capture is not None and not started:
                    capture.new_boot(self)
                    started = True
                if self.shell("getprop sys.boot_completed").strip() != "1":
                    return "the guest has not booted"
            except CaptureFailure:
                raise
            except (Failure, subprocess.TimeoutExpired, ValueError, IndexError):
                return "the guest does not answer"
            return None

        wait_for("the guest restarts", BOOT_SECONDS, rebooted, self, interval=3.0)


# ---- Starting and stopping a guest.

def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path, release: pathlib.Path | None,
                instance: pathlib.Path, provision: bool, watched: bool = False) -> Guest:
    boot_options = boot_android.parse_arguments([
        *(["--release", str(release)] if release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(instance),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        "--display", "offscreen",
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release_directory = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release_directory)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = boot_android.emulator_command(
        boot_options, release_directory, instance / boot_android.android_instance.DATA_DISK, emulator,
        boot_android.data_capacity(boot_options, identity))
    command.append(f"--net-forward=tcp:127.0.0.1:{options.forward_port}-{GUEST_WIFI_ADDRESS}:{ECHO_PORT}")
    log = private / f"emulator-{instance.name}-{int(time.time())}.log"
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True,
                                   env={**os.environ, "TMPDIR": str(private), "PYTHONDONTWRITEBYTECODE": "1"})
    serial = f"127.0.0.1:{options.adb_port}"
    guest = Guest(adb, serial, process, log, Console(private / f"uml-kern{process.pid}", private), private)
    guest.forward_port = options.forward_port
    try:
        boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
        # As soon as adb answers, the boot's log is streamed, from the start of the buffers.
        if watched:
            guest.capture = LogCapture()
            guest.capture.start_boot(guest)
        if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
            raise Failure("Android did not reach sys.boot_completed")
        # A pending update counts as applied from here, as for the launcher.
        boot_android.android_instance.booted(instance, identity)
        if provision:
            boot_android.provision_guest(adb, serial)
    except BaseException:
        stop(guest)
        raise
    return guest


def stop(guest: Guest) -> None:
    # A guest that is still stopped (a section that failed in its stay) would not see the signals that end it.
    guest.thaw()
    try:
        if guest.capture is not None:
            guest.capture.stop()
    finally:
        # The emulator ends whatever the capture's own end does.
        boot_android.disconnect_adb(guest.adb, guest.serial)
        boot_android.stop_guest(guest.process)


# ---- The sections.

class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def section(self, name: str, guest: Guest | None, body: Callable[[], str | None]) -> None:
        """Runs one section. On a guest of the release under test the log is read when the section ends (also
        when it failed, for a crash may be why): a crash or an ANR in its window, or a stream that ended, fails it."""
        capture = guest.capture if guest is not None else None
        problem: Exception | None = None
        note = None
        try:
            if capture is not None:
                capture.begin(guest, name)
            note = body()
        except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as failed:
            problem = failed
        # A suspend's GPU output can arrive after its step returned, while the section went on: the mark is
        # scanned here whether the section passed or not, and the diagnosis is the section's failure.
        gpu = guest.settle_suspend_log() if guest is not None else None
        if gpu is not None:
            problem = Failure(gpu if problem is None else f"{gpu} (the section had failed with: {problem})")
        if capture is not None:
            try:
                crashed = capture.end(guest)
            except (Failure, subprocess.TimeoutExpired, OSError) as unread:
                crashed = [f"the crash evidence could not be read: {unread}"]
            if crashed:
                text = "; ".join(crashed[:5])
                problem = Failure(f"{problem}; and {text}" if problem is not None else text)
        if problem is None:
            print(f"ok   {name}" + (f" ({note})" if note else ""))
            return
        print(f"FAIL {name}: {problem}", file=sys.stderr)
        if guest is not None:
            try:
                print(guest.diagnose(), file=sys.stderr)
            except (Failure, subprocess.TimeoutExpired, OSError):
                pass
        self.failures.append(f"{name}: {problem}")


def check_overlay(guest: Guest) -> str | None:
    listing = guest.shell("cmd overlay list --user 0")
    if not overlay_enabled(listing):
        raise Failure(f"the Wi-Fi module's overlay {OVERLAY_PACKAGE} is not enabled: {listing[-300:].strip()}")
    # adb pull is refused for /vendor/overlay; the shell may read the file.
    result = subprocess.run([guest.adb, "-s", guest.serial, "exec-out", "cat", OVERLAY_FILE],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False, timeout=60)
    if result.returncode != 0 or not result.stdout:
        raise Failure(f"cannot read {OVERLAY_FILE}: {result.stderr.decode(errors='replace').strip()[-300:]}")
    verifier = load_verifier()
    try:
        integers = overlay_integers(result.stdout)
    except (KeyError, ValueError, struct.error, IndexError, zipfile.BadZipFile) as error:
        raise Failure(f"{OVERLAY_FILE} has no readable resource table: {error}") from error
    for name in verifier.WIFI_NO_INTERNET_THRESHOLDS:
        if integers.get(name) != [verifier.WIFI_NEVER_DISABLED_THRESHOLD]:
            raise Failure(f"the overlay sets {name} to {integers.get(name)}, not "
                          f"{verifier.WIFI_NEVER_DISABLED_THRESHOLD}")
    return None


def check_saved_and_joined(guest: Guest) -> str | None:
    wait_for("VirtWifi is saved", 60, lambda: None if network_id(guest.listing()) is not None else "not saved", guest)
    # The boot script has saved the network: the denials of its first commands are read before they rotate out.
    guest.read_denials()
    guest.wait_joined(JOIN_SECONDS, "the station joins VirtWifi")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the saved network is {status}")
    return None


def check_not_disabled(guest: Guest) -> str | None:
    guest.inject_failing_checks()
    guest.forget_and_save()
    guest.wait_joined(JOIN_SECONDS, "the station joins the network saved again")
    deadline = time.monotonic() + HOLD_SECONDS
    dump = ""
    while time.monotonic() < deadline:
        guest.alive()
        dump = guest.dump()
        status = selection_status(dump)
        if status != "NETWORK_SELECTION_ENABLED":
            raise Failure(f"the network became {status} ({selection_disable_reason(dump)}) after a failed "
                          f"internet check (counted {no_internet_count(dump)})")
        time.sleep(POLL_SECONDS)
    if not guest.joined():
        raise Failure("the station left the network")
    counted = no_internet_count(dump)
    stack = guest.network_stack_dump()
    if guest.image_under_test and UNPROBED_MARKER in stack:
        # The old patch (validate without probes) is in the image: it is a failure whatever was counted.
        raise Failure(f"the validation log says {UNPROBED_MARKER!r}: the platform of the image under test validates "
                      "Wi-Fi first and checks it after, so the patch that skips the probes is still in the image")
    if counted < 1:
        # Nothing counted: the injected failures did not reach the platform, or the platform does not count them
        # against a network (it validates Wi-Fi before its check, so a failed check leaves it validated). Its own
        # validation log says which.
        reached = failure_reached_platform(stack, guest.image_under_test)
        if reached is None:
            raise Failure("the platform counted no failed internet check and its validation log does not say that "
                          "it handled the injected failures, so they did not reach it and the network was never "
                          "at risk")
        # Whether the network is validated is only reported: the injected strict private DNS name does not resolve,
        # which the platform's validation reports as not validated.
        state = ("validated" if wifi_validated(guest.connectivity_dump())
                 else "not validated (the injected private DNS name does not resolve)")
        return (f"no failed internet check counted: {reached}; the network stays enabled and joined, {state}")
    return f"{counted} failed internet checks counted, none disabled the network"


def kept_problems(guest: Guest, before: int, wake: Wake) -> list[str]:
    """What the guest's log and the platform's list say of a watched wake once the window of WAKE_WINDOW_SECONDS after
    it has passed: no disconnect from the suspend to the end of the window, and the Wi-Fi network after is the one
    before, with its ID (a wait for the list lets a station that joined again show the new network)."""
    time.sleep(max(0.0, wake.at + WAKE_WINDOW_SECONDS - time.monotonic()))
    problems = disconnect_problems(guest.log_text(), wake.mark)
    seen: list[dict[int, frozenset[str]]] = []

    def listed() -> str | None:
        networks = wifi_networks(guest.connectivity())
        seen[:] = [networks]
        return None if networks else "dumpsys connectivity lists no Wi-Fi network"

    try:
        wait_for("a Wi-Fi network is listed after the wake", RESUME_SECONDS, listed, guest)
    except Failure:
        pass
    return problems + kept_network_problems(seen[0] if seen else {}, before)


def connection_problems(guest: Guest, connections: dict[str, Echo]) -> list[str]:
    """What the connections that the host holds into the guest's Wi-Fi address show after a wake: the one that was
    idle carries a line both ways (so does a connection that the host wrote to during a frozen stay, whose line
    arrives), and the one that the host closed during the stay is ended for the guest by the time its request
    starts, which is within WAKE_REQUEST_SECONDS of the wake."""
    problems = []
    for name, connection in connections.items():
        if name == "A":
            continue
        if name == "C":
            problem = connection.expect("C-during-stay", ECHO_SECONDS)
            if problem is not None:
                problems.append(f"the line the host sent during the stay did not arrive: {problem}")
        problem = connection.exchange(f"{name}-after-wake")
        if problem is not None:
            problems.append(f"the open connection {name} carries no data after the wake: {problem}")
    if "A" in connections:
        started = request_time(guest.wake.request) if guest.wake is not None else None
        ended = closed_at(guest.echo_log(), "A")
        if started is None or ended is None or ended > started:
            problems.append("the connection that the host closed during the stay was not ended for the guest within "
                            f"{WAKE_REQUEST_SECONDS:.0f} s of the wake (its end: {ended}, the request began at {started})")
    return problems


def keeps_station_through_stay(guest: Guest, frozen: bool = False) -> str:
    """One suspend and wake of the guest with its station joined, and what the station must keep through it: the
    request after the wake goes through, the station is joined, the saved network stays enabled, no disconnect is
    reported, the network after is the network before, and the connections that the host holds into the station's
    address carry data (with the emulator stopped for the stay, three: one that the host closes during it, one that
    stays idle and one that the host writes to). Every problem is reported."""
    before = newest_wifi_network(guest.connectivity())
    if before is None:
        raise Failure("dumpsys connectivity lists no Wi-Fi network before the suspend")
    problems: list[str] = []
    with EchoServer(guest) as server:
        connections = {name: server.open(name) for name in (("A", "B", "C") if frozen else ("S",))}

        def during_stay() -> None:
            connections["A"].close()
            connections["C"].send("C-during-stay")

        guest.suspend_and_wake(watch=True, frozen=during_stay if frozen else None)
        wake = guest.wake
        if wake is None:
            raise Failure("the suspend left no record of the wake")
        problems += request_problems(wake.request)
        if not guest.joined():
            problems.append(f"the station is not joined {WAKE_REQUEST_SECONDS:.0f} s after the wake")
        problems += connection_problems(guest, connections)
        problems += kept_problems(guest, before, wake)
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        problems.append(f"the saved network is {status} after the wake")
    if problems:
        raise Failure("; ".join(problems))
    return (f"network {before} kept, no disconnect, the request answered, "
            f"{len(connections)} connection{'s' if len(connections) > 1 else ''} carrying data")


def check_suspend(guest: Guest) -> str | None:
    """A suspend and wake keep the station (a short stay: the route the host's quiesce takes)."""
    return keeps_station_through_stay(guest)


def check_suspend_after_wifi_toggle(guest: Guest) -> str | None:
    """Wi-Fi turned off and on again restarts the supplicant and joins a new network: what keeps the station through
    a suspend must outlast that, and the new network is the one the suspend must keep."""
    before = newest_wifi_network(guest.connectivity())
    guest.shell("cmd wifi set-wifi-enabled disabled")
    wait_for("Wi-Fi is off", 60, lambda: None if not wifi_enabled(guest.status()) else "still on", guest)
    guest.shell("cmd wifi set-wifi-enabled enabled")
    guest.wait_joined(JOIN_SECONDS, "the station joins after Wi-Fi was turned on")

    def new_network() -> str | None:
        net_id = newest_wifi_network(guest.connectivity())
        if net_id is None or (before is not None and net_id <= before):
            return "no new network is listed"
        return None if guest.has_address() else "the station has no address yet"

    wait_for("the station joins a new network", JOIN_SECONDS, new_network, guest)
    time.sleep(TOGGLE_SETTLE_SECONDS)
    return keeps_station_through_stay(guest)


def check_frozen_stay(guest: Guest) -> str | None:
    """A stay with the emulator stopped for longer than the guest's own RTC alarm would let it last, as iOS holds the
    app: the station and its connections are as they were, a connection that the host closed in the stay is ended
    for the guest, and a line that the host wrote in it arrives."""
    return keeps_station_through_stay(guest, frozen=True)


def suspend_inside_connect(guest: Guest) -> str:
    """Wi-Fi off and on, and the suspend asked for the moment the supplicant begins to connect: one command of the
    guest turns Wi-Fi on and waits for that line of the log, so the suspend follows it by one adb return and one
    console request, well inside the two seconds that virt_wifi takes to finish a connect. The emulator is stopped for
    CONNECT_STAY_SECONDS, then continued and woken. Returns the mark that the log was marked with before."""
    guest.shell("cmd wifi set-wifi-enabled disabled")
    wait_for("Wi-Fi is off", 60, lambda: None if not wifi_enabled(guest.status()) else "still on", guest)
    mark = guest.mark_log()
    guest.prepare_suspend()
    started = guest.adb_run(
        "shell", f"cmd wifi set-wifi-enabled enabled; timeout {CONNECT_WAIT_SECONDS} logcat -T 1 -m 1 -e "
        f"'{CONNECT_START_LINE}' >/dev/null; echo status=$?", timeout=CONNECT_WAIT_SECONDS + 30)
    if "status=0" not in started.stdout.decode(errors="replace"):
        raise Failure(f"the supplicant did not begin to connect within {CONNECT_WAIT_SECONDS} s of Wi-Fi being turned "
                      f"on: {started.stdout.decode(errors='replace').strip()[-200:]}")
    guest.suspend_and_wake(frozen=lambda: None, prepared=True, stay=CONNECT_STAY_SECONDS)
    return mark


def check_suspend_in_connect_delay(guest: Guest) -> str | None:
    """A suspend that lands inside the station's connect delay, with the emulator stopped for longer than a BSS that
    nothing holds lives (so the connect finishes after the BSS has expired): the stack ends with the station joined and
    a request working within a bounded time, or retried and joined, never connected by its own report with no working
    network, and the guest kernel's console shows no warning and no call trace. The suspend is tried
    CONNECT_ATTEMPTS times to land inside the delay (its connect then took the stay, not the delay); a row that never
    landed it proved nothing and fails."""
    console_from = guest.log_size()
    problems: list[str] = []
    took: float | None = None
    for attempt in range(1, CONNECT_ATTEMPTS + 1):
        mark = suspend_inside_connect(guest)

        def working() -> str | None:
            if not guest.joined():
                return "the station is not joined"
            found = request_problems(guest.wake_request())
            return ("the framework says the station is joined, but " + "; ".join(found)) if found else None

        try:
            wait_for("the station is joined and a request works", RESUME_SECONDS, working, guest, interval=3.0)
        except Failure as problem:
            problems.append(str(problem))
        took = connect_took(guest.log_text(), mark)
        held = took is not None and took > CONNECT_HELD_SECONDS
        print(f"     attempt {attempt}: the connect took "
              f"{'no recorded time' if took is None else f'{took:.1f} s'}: {'inside' if held else 'outside'} the delay")
        if held or problems:
            break
    problems += console_problems(guest.log_since(console_from))
    held = took is not None and took > CONNECT_HELD_SECONDS
    if not held and not problems:
        raise Failure(f"the suspend did not land inside the connect delay in {CONNECT_ATTEMPTS} attempts (the last "
                      f"connect took {took}): the row has shown nothing")
    if problems:
        raise Failure("; ".join(problems))
    return (f"the station joined and a request works after a suspend {attempt} attempt(s) in, the connect held "
            f"{took:.0f} s across the stay, the console quiet")


def suspend_sections(guest: Guest, report: "Report") -> None:
    report.section("the station stays joined through a suspend and wake", guest, lambda: check_suspend(guest))
    report.section("the station stays joined through a suspend that follows Wi-Fi being turned off and on", guest,
                   lambda: check_suspend_after_wifi_toggle(guest))
    report.section("the station stays joined through a stay with the emulator stopped", guest,
                   lambda: check_frozen_stay(guest))
    report.section("a stay with the emulator stopped inside the station's connect delay ends with a working network "
                   "and a quiet console", guest, lambda: check_suspend_in_connect_delay(guest))


def check_restart(guest: Guest) -> str | None:
    guest.restart()
    guest.wait_joined(JOIN_SECONDS, "the station joins after the restart")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the saved network is {status} after the restart")
    return None


def check_no_repair_logged(guest: Guest, when: str) -> None:
    if repair_logged(guest.script_log()):
        raise Failure(f"the boot script saved the network again {when}")


def check_metered_kept(guest: Guest, waits: Waits) -> str | None:
    guest.shell(f"cmd wifi add-network {SSID} open -m")
    wait_for("the metered network is joined", JOIN_SECONDS, lambda: None if guest.joined() else "not joined", guest)
    if metered_override(guest.dump()) != 1:
        raise Failure("the network is not marked metered")
    guest.restart()
    guest.wait_joined(JOIN_SECONDS, "the station joins after the restart")
    time.sleep(waits.quiet)
    if metered_override(guest.dump()) != 1:
        raise Failure("the boot script saved a joined network again and reset its metered setting")
    check_no_repair_logged(guest, "although the station had joined")
    return None


def script_ended(guest: Guest) -> str | None:
    """None once init reports the boot script's service stopped. init sets the state (init.svc.<name>) when it
    starts the service on this boot and the reboot clears it, so 'stopped' is a run that began and ended on the
    current boot; init exposes no exit status, so a script that ends early at marker 2 ends like one that ran."""
    state = guest.script_service()
    return None if state == "stopped" else f"the boot script's service is {state or 'not started'!r}, not 'stopped'"


def check_autojoin_kept(guest: Guest, waits: Waits, left_alone: bool | None = None) -> str | None:
    """Auto-connect turned off is a choice of the user's that no boot undoes (saving the network replaces it).

    The setting is the one the Wi-Fi service's dump prints under VirtWifi's own configuration (the ID of its row
    in the saved list), which must be there after the restart: a dump without it shows nothing. The boot script's
    service must have started and ended on that boot.

    `left_alone` is what the boot after the restart must log. A boot at marker 1 has no join to wait for (Auto-connect
    off keeps the station away), so the script reads Auto-connect from the Wi-Fi service's dump and says it left the
    network alone: True requires that line, which is also the proof that the script's own domain can read the dump. A
    boot at marker 2 ends at once and logs nothing: False requires none. None accepts either, for a disk whose
    marker the shell cannot read. The script's lines are looked for at every poll and kept once seen, because the
    main log buffer is small."""
    guest.shell(f"cmd wifi add-network {SSID} open -d")
    def autojoin_off() -> str | None:
        network = network_id(guest.listing())
        if network is None:
            return "VirtWifi is not saved"
        values = configuration_autojoin(guest.dump(), network)
        return None if values and not any(values) else "still on"

    wait_for("Auto-connect is off", 60, autojoin_off, guest)
    guest.restart()
    said = repaired = False
    deadline = time.monotonic() + waits.quiet
    while True:
        log = guest.script_log()
        said, repaired = said or left_alone_logged(log), repaired or repair_logged(log)
        if time.monotonic() >= deadline:
            break
        time.sleep(POLL_SECONDS)
    network = network_id(guest.listing())
    if network is None:
        raise Failure("VirtWifi is not saved after the restart")
    values = configuration_autojoin(guest.dump(), network)
    if not values:
        raise Failure("the Wi-Fi service's dump shows no Auto-connect for VirtWifi's configuration after the "
                      "restart: nothing shows that the setting was kept")
    if any(values):
        raise Failure("Auto-connect is on again after a restart: the boot script saved the network again")
    if repaired:
        raise Failure("the boot script saved the network again although the user turned Auto-connect off")
    wait_for("the boot script ends on this boot", waits.repair, lambda: script_ended(guest), guest,
             interval=POLL_SECONDS)
    if left_alone is True and not said:
        raise Failure("the boot script did not say that it left the network alone: it did not read Auto-connect "
                      "from the Wi-Fi service's dump in its own domain, or it found the network joined")
    if left_alone is False and said:
        raise Failure("the boot script says that it left the network alone, which only a boot without marker 2 does")
    return None


def check_forgotten_stays_forgotten(guest: Guest, waits: Waits) -> str | None:
    identifier = network_id(guest.listing())
    if identifier is None:
        raise Failure("VirtWifi is not saved")
    guest.shell(f"cmd wifi forget-network {identifier}")
    wait_for("the network is forgotten", 30, lambda: None if network_id(guest.listing()) is None else "saved", guest)
    guest.restart()
    time.sleep(waits.quiet)
    if SSID in saved_ssids(guest.listing()):
        raise Failure("VirtWifi is saved again after a restart although the user forgot it")
    check_no_repair_logged(guest, "although the user forgot the network")
    if not wifi_enabled(guest.status()):
        raise Failure("Wi-Fi is not on")
    return None


def check_wifi_off_stays_off(guest: Guest, waits: Waits) -> str | None:
    guest.shell(f"cmd wifi add-network {SSID} open")
    guest.wait_joined(JOIN_SECONDS, "the station joins the network saved again")
    guest.shell("cmd wifi set-wifi-enabled disabled")
    wait_for("Wi-Fi is off", 60, lambda: None if not wifi_enabled(guest.status()) else "still on", guest)
    guest.restart()
    time.sleep(waits.quiet)
    if wifi_enabled(guest.status()):
        raise Failure("Wi-Fi is on after a restart although the user turned it off")
    check_no_repair_logged(guest, "although Wi-Fi was off")
    if network_id(guest.listing()) is None:
        raise Failure("the saved network is gone")
    return None


def check_no_denials(guest: Guest) -> str | None:
    """The boot script runs in a domain of its own (overlay/sepolicy/system_ext/private); a rule it lacks
    shows as a denial, and the script then fails without a word. The denials earlier reads of this guest's
    log found count too, since the log may have rotated them out since."""
    logged = guest.read_denials()
    if guest.denials:
        raise Failure(f"SELinux denied the boot script's domain {len(guest.denials)} times:\n"
                      + "\n".join(guest.denials[:5]))
    return f"{logged} denials logged in all, none for {SCRIPT_DOMAIN} in {guest.denial_reads} reads of the log"


CRASH_SECTION = "NetworkStack, SystemUI and Settings neither crash nor stop answering"
# The processes of the warning, each by the exact name it runs under: the platform's network stack (the
# manifest's own process name, not its package), the status bar and Settings. A name that only starts with one
# of these, another package or a private subprocess such as com.android.systemui:screenshot, is another
# process, and none of those subprocesses is read as one of these three.
WARNING_PROCESSES = ("com.android.networkstack.process", "com.android.systemui", "com.android.settings")
# `logcat -v threadtime`: date, time, pid, tid, priority, tag, message. The log's own columns bind the lines of
# one record, which other processes' lines may interleave.
LOG_LINE = re.compile(r"^(\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+(\d+)\s+(\d+)\s+([VDIWEFS])\s+(\S+)\s*:\s?(.*)$")
JAVA_PROCESS_LINE = re.compile(r"Process: (\S+), PID: (\d+)")
ANR_LINE = re.compile(r"ANR in (\S+)(?: \(\S*\))?")
NATIVE_PROCESS_LINE = re.compile(r"pid: \d+, (?:ppid: \d+, )?tid: \d+, name: .*  >>> (\S+) <<<")
NATIVE_SIGNAL_LINE = re.compile(r"signal (\d+) \((\S+)\)")
SIGILL_LINE = re.compile(r"signal 4 \(SIGILL\)")
# The line the runner has the guest log to know that the stream carries everything logged before it.
SYNC_TAG = "wifi-runner"
# How long the stream may take to carry a line the guest logged, and to end after its guest went away.
SYNC_SECONDS = 60
DRAIN_SECONDS = 60


@dataclass(frozen=True)
class LogLine:
    when: str
    pid: int
    tid: int
    tag: str
    message: str
    text: str

    @property
    def stream(self) -> tuple[int, int, str]:
        """What binds the lines of one record: the process and thread that logged them, under one tag."""
        return self.pid, self.tid, self.tag


@dataclass(frozen=True)
class Finding:
    """A crash, an ANR or an illegal instruction in a log: the time stamp of its record, and what it says."""
    when: str
    problem: str


def log_lines(log: str) -> list[LogLine]:
    lines = []
    for text in log.splitlines():
        match = LOG_LINE.match(text)
        if match:
            lines.append(LogLine(match[1], int(match[2]), int(match[3]), match[5], match[6], text.strip()))
    return lines


def next_of_record(lines: list[LogLine], index: int, same_time: bool) -> LogLine | None:
    """The next line of the record that starts at `lines[index]`: the next one of the same process, thread and
    tag, however many lines of other processes come between. One logged message that has several lines (a
    crash's header and its process line) carries one time stamp."""
    first = lines[index]
    for line in lines[index + 1:]:
        if line.stream == first.stream and (line.when == first.when or not same_time):
            return line
    return None


def tombstone_process(lines: list[LogLine], index: int) -> str | None:
    """The process a tombstone's signal line belongs to: the name on the header line of the same record (the same
    logging process and thread), or None when the log holds no header for it."""
    stream = lines[index].stream
    for earlier in reversed(lines[:index]):
        if earlier.stream != stream:
            continue
        message = earlier.message.strip()
        header = NATIVE_PROCESS_LINE.fullmatch(message)
        if header:
            return header[1]
        if NATIVE_SIGNAL_LINE.match(message) or message.startswith("*** ***"):
            return None
    return None


def crash_findings(log: str) -> list[Finding]:
    """What the log says of the processes of the warning crashing or not answering, and of any illegal
    instruction (an instruction the guest's CPU lacks is a defect of the host, in whatever process). A Java
    crash, a native crash (any fatal signal) and an ANR are each read as the one process they name, by its
    exact name; a Java or native crash that names no process is a finding too, since it may be any of them."""
    lines = log_lines(log)
    found = []
    for index, line in enumerate(lines):
        message = line.message.strip()
        if SIGILL_LINE.search(line.message):
            found.append(Finding(line.when, f"an illegal instruction: {line.text[:160]}"))
        if line.message.startswith(("FATAL EXCEPTION", "*** FATAL EXCEPTION")):
            follow = next_of_record(lines, index, same_time=True)
            named = JAVA_PROCESS_LINE.fullmatch(follow.message.strip()) if follow else None
            if named is None:
                found.append(Finding(line.when, f"a process crashed without naming itself: {line.text[:160]}"))
            elif named[1] in WARNING_PROCESSES:
                found.append(Finding(line.when, f"{named[1]} crashed: {line.text[:160]}"))
        anr = ANR_LINE.fullmatch(message)
        if anr and anr[1] in WARNING_PROCESSES:
            found.append(Finding(line.when, f"{anr[1]} did not respond: {line.text[:160]}"))
        signal = NATIVE_SIGNAL_LINE.match(message)
        if signal and line.tag == "DEBUG" and signal[1] != "4":
            # Signal 4 is reported above in any process.
            process = tombstone_process(lines, index)
            if process is None:
                found.append(Finding(line.when, f"a native crash that names no process: {line.text[:160]}"))
            elif process in WARNING_PROCESSES:
                found.append(Finding(line.when, f"{process} died of signal {signal[1]} ({signal[2]}): "
                                                f"{line.text[:160]}"))
    return found


def crash_problems(log: str) -> list[str]:
    return [finding.problem for finding in crash_findings(log)]


# What the guest's log says of the station going away. A station that is kept through a suspend reports nothing of the
# kind: the supplicant's disconnect and the platform's own (ConnectivityService says it of the network by its ID) are
# what a station that the kernel dropped at the suspend shows within a tenth of a second of the wake.
STATION_DROPS = (
    ("wpa_supplicant", re.compile(r"\bCTRL-EVENT-DISCONNECTED\b"), "the supplicant reported a disconnect"),
    ("ConnectivityService", re.compile(r"\[\d+ WIFI\] disconnected\b"), "ConnectivityService disconnected a network"),
)


def lines_after(log: str, token: str) -> list[LogLine] | None:
    """The lines of the log after the one that carries `token`, or None when no line carries it (the buffer wrapped)."""
    lines = log_lines(log)
    for index, line in enumerate(lines):
        if token in line.message:
            return lines[index + 1:]
    return None


def disconnect_problems(log: str, mark: str) -> list[str]:
    """Every report of the station going away in the log since the mark that the runner logged before a suspend. A
    log that no longer holds the mark cannot be read, which is no pass."""
    after = lines_after(log, mark)
    if after is None:
        return ["the guest's log no longer holds the line logged before the suspend, so the stay cannot be read"]
    return [f"{what}: {line.text[:160]}" for line in after for tag, pattern, what in STATION_DROPS
            if line.tag == tag and pattern.search(line.message)]


def kept_network_problems(networks: dict[int, frozenset[str]], before: int) -> list[str]:
    """The Wi-Fi network that the platform lists after a wake is the one that it listed before the suspend, with its
    ID: a station that was dropped and joined again is a new network, and none listed is a network gone."""
    if not networks:
        return [f"network {before} is gone: ConnectivityService lists no Wi-Fi network"]
    newest = max(networks)
    return [f"network {before} became network {newest}"] if newest != before else []


def log_seconds(when: str) -> float:
    """The seconds into its day of a threadtime stamp (`MM-DD HH:MM:SS.mmm`); the day is kept, for a log that crosses
    midnight is no case of the stays this reads."""
    date, clock = when.split()
    month, day = date.split("-")
    hours, minutes, seconds = clock.split(":")
    return ((int(month) * 31 + int(day)) * 24 * 3600 + int(hours) * 3600 + int(minutes) * 60 + float(seconds))


def connect_took(log: str, mark: str) -> float | None:
    """From the supplicant's first start of a connect after the mark to its first report of the connection, in the
    guest's clock, or None when the log holds no such pair. A connect that a suspend held across a stay with the
    emulator stopped took the stay (the guest's clock moves on when it thaws), and one that did not took the
    connect delay."""
    after = lines_after(log, mark)
    if after is None:
        return None
    started: float | None = None
    for line in after:
        if line.tag != "wpa_supplicant":
            continue
        if started is None and CONNECT_START_LINE in line.message:
            started = log_seconds(line.when)
        elif started is not None and CONNECT_DONE_LINE in line.message:
            return log_seconds(line.when) - started
    return None


# What the guest's kernel console says of a kernel that is wrong: a warning and a call trace.
CONSOLE_FAULTS = re.compile(r"\bWARNING:|\bCall Trace:")


def console_problems(console: str) -> list[str]:
    """The lines of the launcher's log (the guest kernel's console among them) that report a kernel warning or print
    a call trace."""
    return [f"the guest console says: {line.strip()[:200]}" for line in console.splitlines()
            if CONSOLE_FAULTS.search(line)]


# The request that the guest makes WAKE_REQUEST_SECONDS after a wake, through its default network as an app's would
# go: an ICMP echo to the launcher's gateway and a DNS query for `localhost` over TCP to its name server, both
# answered inside the host. It prints the guest's clock at its start (the clock that the echo server logs with), the
# ping's status and the bytes of the DNS answer (an error message goes to the null device: a refusal counts none).
DNS_QUERY = bytes([0x00, 0x1b, 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 9]) + b"localhost" + bytes([0, 0, 1, 0, 1])
DNS_QUERY_OCTAL = "".join("\\%03o" % byte for byte in DNS_QUERY)
WAKE_REQUEST = ("t=$(date +%s.%N); ping -c 1 -W 2 10.0.2.2 >/dev/null 2>&1; p=$?; "
                f"n=$(printf '{DNS_QUERY_OCTAL}' | nc -w 3 10.0.2.3 53 2>/dev/null | wc -c); "
                'echo "request t=$t ping_rc=$p dns_tcp_bytes=$n"')
REQUEST_LINE = re.compile(r"^request t=(\d+(?:\.\d+)?) ping_rc=(\d+) dns_tcp_bytes=(\d+)\s*$", re.M)


def request_problems(output: str) -> list[str]:
    """What the guest's request after a wake says of its default network: the gateway answered the ping, and the name
    server answered the query over TCP."""
    match = REQUEST_LINE.search(output)
    if match is None:
        return [f"the request after the wake printed no answer: {output.strip()[-200:] or 'nothing'}"]
    problems = []
    if match[2] != "0":
        problems.append(f"the gateway did not answer a ping {WAKE_REQUEST_SECONDS:.0f} s after the wake (status {match[2]})")
    if int(match[3]) < DNS_ANSWER_MIN_BYTES:
        problems.append(f"the name server did not answer a query over TCP {WAKE_REQUEST_SECONDS:.0f} s after the wake "
                        f"({match[3]} bytes)")
    return problems


def request_time(output: str) -> float | None:
    """The guest's clock when its request after the wake started, or None when it printed none."""
    match = REQUEST_LINE.search(output)
    return float(match[1]) if match else None


# The echo server's log: one `close <name> <guest clock> rc=<status>` line when a connection ends.
CLOSE_LINE = re.compile(r"^close (\S+) (\d+(?:\.\d+)?) rc=(\d+)\s*$", re.M)


def closed_at(log: str, name: str) -> float | None:
    """The guest's clock when the connection of the name ended, or None when it has not."""
    for match in CLOSE_LINE.finditer(log):
        if match[1] == name:
            return float(match[2])
    return None


class LogStream:
    """One segment of a boot's log: `adb logcat` of the main, system and crash buffers in threadtime format, which
    reads the buffers from their start and then follows them, written to a host file for as long as it runs."""

    def __init__(self, adb: str, serial: str, path: pathlib.Path) -> None:
        self.adb, self.serial, self.path = adb, serial, path
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        arguments = [self.adb, "-s", self.serial, "logcat", "-v", "threadtime"]
        for buffer in ("main", "system", "crash"):
            arguments += ["-b", buffer]
        with self.path.open("wb") as output:
            self.process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=output,
                                            stderr=subprocess.STDOUT, start_new_session=True)

    def text(self) -> str:
        return self.path.read_text(errors="replace")

    def ended(self) -> bool:
        return self.process is None or self.process.poll() is not None

    def drain(self, seconds: float) -> None:
        """Waits for the stream to end of itself (its guest went away). One that does not is stopped and is a
        failure: what the guest still held for it was not read, and the file does not say."""
        if self.process is None:
            return
        try:
            self.process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            self.stop()
            raise CaptureFailure(f"the log stream did not end of itself in {seconds:.0f} s: it was stopped, and "
                                 "what its guest still held for it was not read") from None

    def stop(self) -> None:
        """Stops the logcat this object started, by its own process."""
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=10)


class LogCapture:
    """The crash and ANR evidence of one guest: the guest's log as one continuous stream for each boot, written
    to host files from the start of the boot's buffers, so that nothing between two reads, two sections or a
    restart is lost and every record is complete in the file. The scanner reads all of it each time and reports
    a record once, as a finding of the section whose time window holds it (the window is the time stamps of two
    lines that the guest logged at the section's start and end, in the log's own clock) or of no section.
    Before the guest is taken down (a restart, a suspend) the stream is confirmed alive and carrying, so that a
    stream that died earlier is not taken for the disconnect that follows; a restart reads the old boot's
    stream to its end before the new boot's starts, and one that does not end is a permanent failure; a stream
    that ends while its guest should be up fails the section in progress."""

    def __init__(self) -> None:
        # Each boot's segments: a suspend that drops the connection starts another segment, which reads the
        # buffers again from their start, so a record may be in two segments of one boot.
        self.boots: list[list[LogStream]] = []
        self.expect_end = False
        self.seen: set[tuple[int, str, str]] = set()
        # Every finding, with the section it belongs to; and the findings of the section in progress.
        self.problems: list[str] = []
        self.section_problems: list[str] = []
        self.name = ""
        self.begin_at: tuple[int, str] | None = None
        self.reads = 0

    # The streams.
    def start_boot(self, guest: "Guest") -> None:
        self.boots.append([])
        self.start_segment(guest)

    def start_segment(self, guest: "Guest") -> None:
        stream = guest.open_log_stream(len(self.boots) - 1, len(self.boots[-1]))
        stream.start()
        self.boots[-1].append(stream)
        self.expect_end = False

    @property
    def current(self) -> LogStream:
        return self.boots[-1][-1]

    def confirm(self, guest: "Guest") -> None:
        """The guest is about to restart or suspend: its stream must be alive now and carry the guest's log up
        to now. A stream that died before this fails the section in progress; a stream that is only found
        ended after the disconnect would be taken for the disconnect's own end, and the records it never
        received would go with the reboot."""
        self.check_stream()
        self.sync(guest)

    def going_down(self) -> None:
        """The guest is about to restart or suspend: its stream is expected to end."""
        self.expect_end = True

    def drain(self, streams: list[LogStream]) -> None:
        """Reads each of the streams to its end. One that does not end is evidence lost for good: the run
        records it, and the failure is the caller's to let through."""
        for stream in streams:
            try:
                stream.drain(DRAIN_SECONDS)
            except CaptureFailure as lost:
                self.problems.append(f"evidence lost: {lost}")
                raise

    def new_boot(self, guest: "Guest") -> None:
        """The guest has restarted and adb answers: the old boot's stream is read to its end, then the new
        boot's starts."""
        self.drain(self.boots[-1])
        self.start_boot(guest)

    def resumed(self, guest: "Guest") -> None:
        """The guest woke from a suspend. A stream that ended with the connection is read to its end, and a
        further segment of the same boot starts."""
        if self.current.ended():
            self.drain([self.current])
            self.start_segment(guest)
        self.expect_end = False

    def check_stream(self) -> None:
        if not self.expect_end and self.boots and self.current.ended():
            raise Failure("the log stream of this boot ended while the guest is up: crashes and ANRs after "
                          "this point cannot be seen")

    def stop(self) -> None:
        for segments in self.boots:
            for stream in segments:
                stream.stop()

    # The reading.
    def sync(self, guest: "Guest") -> tuple[tuple[int, str], str]:
        """Has the guest log a line and waits for the stream to carry it: everything the guest logged before it
        is in the file. Returns the line's position (the boot and its time stamp in the log's clock) and the
        stream's text up to it, which is complete by construction."""
        token = f"sync-{uuid.uuid4().hex}"
        guest.shell(f"log -t {SYNC_TAG} {token}")
        deadline = time.monotonic() + SYNC_SECONDS
        while True:
            text = self.current.text()
            at = text.find(token)
            if at >= 0:
                line_end = text.find("\n", at)
                line_start = text.rfind("\n", 0, at) + 1
                parsed = log_lines(text[line_start:line_end if line_end >= 0 else len(text)])
                if parsed:
                    return (len(self.boots) - 1, parsed[0].when), text[:line_end if line_end >= 0 else len(text)]
            if self.current.ended():
                raise Failure("the log stream ended before it carried a line the guest logged")
            if time.monotonic() >= deadline:
                raise Failure(f"the log stream did not carry a line the guest logged in {SYNC_SECONDS} s")
            time.sleep(0.5)

    def scan(self, snapshot: str) -> list[tuple[tuple[int, str], str]]:
        """Every finding of the guest's log not reported before, with its position: every earlier segment whole
        (its stream has ended) and the current one up to the snapshot."""
        found = []
        for boot, segments in enumerate(self.boots):
            for index, stream in enumerate(segments):
                is_current = boot == len(self.boots) - 1 and index == len(segments) - 1
                for finding in crash_findings(snapshot if is_current else stream.text()):
                    key = (boot, finding.when, finding.problem)
                    if key not in self.seen:
                        self.seen.add(key)
                        found.append(((boot, finding.when), finding.problem))
        return found

    def begin(self, guest: "Guest", name: str) -> None:
        self.check_stream()
        self.name = name
        self.section_problems = []
        self.begin_at = None
        self.begin_at, _ = self.sync(guest)

    def end(self, guest: "Guest") -> list[str]:
        """Reads the log up to now. A finding whose position is in this section's window belongs to the section;
        any other (found only now: between two sections, or before the first) belongs to none, and still counts
        in the summary."""
        if self.begin_at is None:
            return []
        begin, self.begin_at = self.begin_at, None
        end, snapshot = self.sync(guest)
        self.reads += 1
        for position, problem in self.scan(snapshot):
            if begin <= position <= end:
                self.section_problems.append(problem)
                self.problems.append(f"{self.name}: {problem}")
            else:
                self.problems.append(f"outside a section: {problem}")
        return self.section_problems


def check_no_crashes(guest: Guest) -> str | None:
    """Across the guest's boots, NetworkStack, SystemUI and Settings neither crashed nor stopped answering, and
    nothing hit an illegal instruction: the whole log read once more, and every finding listed, those of the
    sections and those that fall in none."""
    capture = guest.capture
    if capture is None or capture.reads == 0:
        raise Failure("no section of this guest read its log")
    _, snapshot = capture.sync(guest)
    for _, problem in capture.scan(snapshot):
        capture.problems.append(f"outside a section: {problem}")
    if capture.problems:
        raise Failure("; ".join(capture.problems[:20]))
    return f"{capture.reads} sections read from the log of {len(capture.boots)} boot(s): no crash, ANR or illegal instruction"


@dataclass
class Pin:
    """The network a section judges, and when the platform last gave it the validated capability: the same
    network, never given it again, is what "validated throughout" means."""
    net_id: int
    validated_at: int


def pin_problems(dump: str, pin: Pin) -> list[str]:
    """How the network differs from the pinned one: replaced by a newer network, gone, or validated again
    (`lastValidated` moved), as it would be after the capability dropped and came back."""
    networks = wifi_networks(dump)
    if pin.net_id not in networks:
        return [f"network {pin.net_id} is gone"]
    problems = []
    newest = max(networks)
    if newest != pin.net_id:
        problems.append(f"network {pin.net_id} was replaced by network {newest}")
    now = last_validated(dump).get(pin.net_id)
    if now is None:
        problems.append(f"network {pin.net_id} is no longer validated")
    elif now != pin.validated_at:
        problems.append(f"network {pin.net_id} was validated again (lastValidated {pin.validated_at} -> {now})")
    return problems


def pin_network(guest: "Guest", seconds: float = WARNING_SECONDS, newer_than: int | None = None) -> Pin:
    """Waits for the newest Wi-Fi network (newer than `newer_than`, when given) to be validated, and pins it."""
    found: list[Pin] = []

    def validated() -> str | None:
        dump = guest.connectivity()
        net_id = newest_wifi_network(dump)
        if net_id is None:
            return "dumpsys connectivity lists no Wi-Fi network"
        if newer_than is not None and net_id <= newer_than:
            return f"the newest network is still {net_id}, not a new one"
        if "VALIDATED" not in wifi_networks(dump)[net_id]:
            return f"network {net_id} is not validated yet"
        when = last_validated(dump).get(net_id)
        if when is None:
            return f"network {net_id} shows no lastValidated"
        found.append(Pin(net_id, when))
        return None

    wait_for("the joined network is validated", seconds, validated, guest, interval=1.0)
    return found[-1]


def wait_problems(description: str, seconds: float, guest: Guest, problems: Callable[[], list[str]],
                  interval: float = 5.0) -> None:
    """Waits until `problems` returns none; the last ones are the failure."""
    wait_for(description, seconds, lambda: "; ".join(problems()) or None, guest, interval=interval)


def sign_in_problems(guest: Guest) -> list[str]:
    """Whether anything asks to sign in: a notification of ConnectivityService (Sign in to network, and the
    no Internet, partial connectivity and lost Internet notices that tap through to a dialog whose answer
    tears the network down), or the captive portal login activity."""
    problems = [f"a notification of ConnectivityService: {line}"
                for line in connectivity_notifications(guest.notifications())]
    problems += [f"the captive portal login activity: {line}" for line in captive_portal_activities(guest.activities())]
    return problems


def warning_problems(guest: Guest, probes_failed: bool, pin: Pin | None = None) -> list[str]:
    """What differs from a validated network that shows only the warning of a failed check: the capabilities
    (validated, not partial, not behind a sign-in page), the validation log, the notifications and the
    activities (no sign-in), and the screen (Settings' "Limited connection", the status bar's "No internet").
    With `pin` it is the pinned network, still the one."""
    dump = guest.connectivity()
    net_id = pin.net_id if pin is not None else newest_wifi_network(dump)
    if net_id is None:
        return validation_problems(None)
    problems = pin_problems(dump, pin) if pin is not None else []
    problems += validation_problems(wifi_networks(dump).get(net_id))
    logs = validation_logs(guest.network_stack()).get(net_id, [])
    problems += check_log_problems(logs, CHECK_FAILED_LINE, probes_failed)
    problems += sign_in_problems(guest)
    problems += mark_problems(ui_nodes(guest.ui()), shown=True)
    return problems


def cleared_problems(guest: Guest, pin: Pin | None = None, after_failure: bool = False) -> list[str]:
    """What differs from a validated network whose check passed: the same, with the check's passing line and
    without the mark (and the status bar's icon in the dump). With `after_failure` the passing line is a
    new one, after the last failed check of the same network."""
    dump = guest.connectivity()
    net_id = pin.net_id if pin is not None else newest_wifi_network(dump)
    if net_id is None:
        return validation_problems(None)
    problems = pin_problems(dump, pin) if pin is not None else []
    problems += validation_problems(wifi_networks(dump).get(net_id))
    logs = validation_logs(guest.network_stack()).get(net_id, [])
    problems += passed_after_failed_problems(logs) if after_failure else check_log_problems(logs, CHECK_PASSED_LINE)
    problems += sign_in_problems(guest)
    problems += mark_problems(ui_nodes(guest.ui()), shown=False)
    return problems


def stays_validated(guest: Guest, pin: Pin | None = None) -> None:
    """Raises unless the network is enabled and joined with Auto-connect on, validated (not partial, not
    behind a sign-in page, its private DNS not broken), and nothing asks to sign in. With `pin` it is also
    the one network, never validated again."""
    dump = guest.dump()
    status = selection_status(dump)
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the network became {status} ({selection_disable_reason(dump)}) while the check failed")
    values = autojoin_values(dump)
    if not values or not all(values):
        raise Failure("Auto-connect is off")
    if not guest.joined():
        raise Failure("the station left the network")
    connectivity = guest.connectivity()
    net_id = pin.net_id if pin is not None else newest_wifi_network(connectivity)
    problems = pin_problems(connectivity, pin) if pin is not None else []
    problems += validation_problems(None if net_id is None else wifi_networks(connectivity).get(net_id))
    problems += sign_in_problems(guest)
    if problems:
        raise Failure("; ".join(problems))


def hold_validated(guest: Guest, seconds: float, pin: Pin | None = None) -> None:
    """For `seconds`, the network stays as `stays_validated` says."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        guest.alive()
        stays_validated(guest, pin)
        time.sleep(POLL_SECONDS)


def hold_quiet(guest: Guest, server: "ProbeServer", scenario: str, seconds: float, pin: Pin) -> None:
    """For `seconds` the network stays as `stays_validated` says and no check reaches the server: a check that
    comes after a pass is a timer that the pass left, and it would pass for the forced check that follows."""
    seen = len(server.checks(scenario))
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        guest.alive()
        stays_validated(guest, pin)
        extra = len(server.checks(scenario)) - seen
        if extra:
            raise Failure(f"{extra} check(s) reached the server in the {seconds:.0f} s after a check passed: "
                          "a timer was left, and the forced check would not be told from it")
        time.sleep(POLL_SECONDS)


def wait_watching(description: str, seconds: float, guest: Guest, pin: Pin, check: Callable[[], str | None],
                  interval: float = 3.0) -> None:
    """Waits for `check` while the network stays the one validated network: a change of it fails at once."""
    def step() -> str | None:
        stays_validated(guest, pin)
        return check()

    wait_for(description, seconds, step, guest, interval=interval)


def warning_shown(guest: Guest, probes_failed: bool, seconds: float, what: str, pin: Pin | None = None) -> None:
    """The warning shows within WARNING_SECONDS, and is held for `seconds`, with the Internet page in front."""
    guest.show_internet_page()
    try:
        wait_problems(what, WARNING_SECONDS, guest, lambda: warning_problems(guest, probes_failed, pin))
        hold_validated(guest, seconds, pin)
        problems = warning_problems(guest, probes_failed, pin)
        if problems:
            raise Failure("after the hold: " + "; ".join(problems))
    finally:
        guest.leave_internet_page()


def check_failed_check_warns(guest: Guest) -> str | None:
    """The validation probes at a black hole fail the check, and nothing else: the network stays validated
    (as on a host that reaches no validation server) while Android's Wi-Fi mark shows the failed check."""
    guest.rejoin_with_settings(CHECK_SETTINGS, clear=(*PRIVATE_DNS_SETTINGS, USE_HTTPS_FLAG))
    pin = pin_network(guest)
    warning_shown(guest, True, WARNING_HOLD_SECONDS, "the failed check shows only the warning", pin)
    return f"validated, joined and enabled for {WARNING_HOLD_SECONDS} s with the mark shown"


def check_warning_after_wake(guest: Guest) -> str | None:
    """A wake keeps the station: the network before the suspend is the network after it (no disconnect, the same ID),
    and the failed check's warning stays on it, which is never validated again (`lastValidated` unchanged)."""
    pin = pin_network(guest)
    guest.suspend_and_wake(watch=True)
    if guest.wake is None:
        raise Failure("the suspend left no record of the wake")
    problems = kept_problems(guest, pin.net_id, guest.wake)
    try:
        warning_shown(guest, True, SHORT_HOLD_SECONDS, "the failed check shows only the warning after the wake", pin)
    except Failure as problem:
        problems.append(str(problem))
    if problems:
        raise Failure("; ".join(problems))
    return f"network {pin.net_id} kept through the wake, validated at {pin.validated_at} throughout, with the warning"


def page_settings(port: int, scenario: str = "") -> dict[str, str]:
    """The probes' HTTP and fallback URLs at the host's server (through `adb reverse` on the guest's loopback),
    under the scenario's path, and the HTTPS URL at the black hole, so a page answer is all the check gets."""
    return {"captive_portal_http_url": f"http://127.0.0.1:{port}{scenario}/generate_204",
            "captive_portal_fallback_url": f"http://127.0.0.1:{port}{scenario}/gen_204",
            "captive_portal_https_url": f"https://{BLACKHOLE}/generate_204"}


PAGE_CODES = {"redirect": 302, "page": 200}


def server_answer_problems(server: "ProbeServer", scenario: str, code: int) -> list[str]:
    """What the server gave the HTTP probes of the scenario: some request, each answered with `code`."""
    statuses = [status for _, path, status in server.requests(scenario) if path.endswith("/generate_204")]
    if not statuses:
        return [f"the server saw no HTTP probe of {scenario}"]
    wrong = sorted({status for status in statuses if status != code})
    return [f"the server answered the probes of {scenario} with {wrong}, not {code}"] if wrong else []


def check_page_answer_warns(guest: Guest, server: "ProbeServer", mode: str) -> str | None:
    """A sign-in page or a block page answers the probe in place of the expected answer: the current network's
    own probe received it (its log names the scenario's URL and the code, and the server gave that code), the
    platform stays validated, no captive portal state, notification or login activity follows, and only the
    warning shows."""
    code = PAGE_CODES[mode]
    scenario = server.scenario()
    server.mode = mode
    server.clear()
    guest.reverse(server.port)
    guest.rejoin_with_settings(page_settings(server.port, scenario), clear=(*PRIVATE_DNS_SETTINGS, USE_HTTPS_FLAG))
    pin = pin_network(guest)
    wait_for("a probe reaches the host's server", WARNING_SECONDS,
             lambda: None if server.probes(scenario) else "the server has seen no request of this scenario", guest)
    wait_problems(f"the current network's probe received the {mode} answer", WARNING_SECONDS, guest,
                  lambda: probe_answer_problems(guest.validation_log(pin.net_id), scenario, code)
                  + server_answer_problems(server, scenario, code), interval=2.0)
    warning_shown(guest, False, WARNING_HOLD_SECONDS, f"the {mode} answer shows only the warning", pin)
    return f"{len(server.probes(scenario))} requests answered {code} and read by the network's own probe"


def validated_while_unanswered(guest: Guest, server: "ProbeServer", scenario: str,
                               seconds: float = WARNING_SECONDS) -> Pin:
    """Waits for the first check of the network (the scenario's first HTTP probe, which the server holds) to be
    at the server, looks at `dumpsys connectivity` while that request is open and unanswered (open before the
    look and still open after it, so the whole look fell inside), and requires the newest network to be
    validated then, not partial, not behind a sign-in page, its private DNS not broken, and its validation
    log to hold no probe or check that completed before the held request. It then answers the
    request and pins that network. A network that is validated only when its check is answered fails here, as
    does a look that did not fit in the hold (the hold is HELD_ANSWER_SECONDS, below the probe's own timeout)."""
    found: list[Pin] = []

    def look() -> str | None:
        # The hold is read as one state, never as the record of a request and a flag read apart.
        state = server.hold_state(scenario)
        if state in ("idle", "armed"):
            return "the server has seen no check"
        if state == "released":
            raise Failure("the first check's answer was out before a look at the network was made: "
                          f"no look fell inside the {HELD_ANSWER_SECONDS:.0f} s hold")
        dump = guest.connectivity()
        net_id = newest_wifi_network(dump)
        messages = [] if net_id is None else guest.validation_log(net_id)
        if server.hold_state(scenario) != "open":
            raise Failure("the held answer was out before the look at the network was complete: "
                          f"no look fell inside the {HELD_ANSWER_SECONDS:.0f} s hold")
        problems = validation_problems(None if net_id is None else wifi_networks(dump)[net_id])
        problems += completed_before_problems(messages)
        when = None if net_id is None else last_validated(dump).get(net_id)
        if when is None and not problems:
            problems = [f"network {net_id} shows no lastValidated"]
        if problems:
            raise Failure("while its first internet check was unanswered: " + "; ".join(problems))
        found.append(Pin(net_id, when))
        return None

    try:
        wait_for("the first check's request is held at the host's server", seconds, look, guest, interval=1.0)
    finally:
        server.release()
    return found[-1]


def check_recovery_on_one_network(guest: Guest, server: "ProbeServer") -> str | None:
    """One network, never replaced and never validated again, from a failing check to a passing one: the
    probes ask a server that answers with a page. With the Internet page in front and no other action, the
    network is validated while its first check is still unanswered (the server holds that answer, and no probe
    or check of the network's log completed before it), the warning shows in Settings and in the status bar,
    the check comes back after the stock back-off, and when the server answers 204 the next check passes and
    the warning goes from both, with a new passing line in the log. A forced re-evaluation then checks the
    network again, which brings the warning back, and the next passing check takes it away."""
    scenario = server.scenario()
    server.mode = "page"
    server.clear()
    server.hold_first(scenario)
    guest.reverse(server.port)
    guest.show_internet_page()
    try:
        guest.rejoin_with_settings({**page_settings(server.port, scenario), USE_HTTPS_FLAG: "0"},
                                   clear=PRIVATE_DNS_SETTINGS)
        pin = validated_while_unanswered(guest, server, scenario)
        # The answer is out: the check's result is logged, and the network was validated before any probe of it.
        wait_problems("the first check's result is logged", WARNING_SECONDS, guest,
                      lambda: check_log_problems(guest.validation_log(pin.net_id), CHECK_FAILED_LINE), interval=1.0)
        messages = guest.validation_log(pin.net_id)
        first = first_validation_problems(messages) + first_completion_problems(messages, scenario, 200)
        if first:
            raise Failure("; ".join(first))
        wait_watching("the failed check shows in Settings and in the status bar, with no other action",
                      WARNING_SECONDS, guest, pin,
                      lambda: "; ".join(warning_problems(guest, False, pin)
                                        + probe_answer_problems(guest.validation_log(pin.net_id), scenario, 200)) or None)

        def retried() -> str | None:
            times = server.checks(scenario)
            if len(times) < RETRIES_NEEDED:
                return f"{len(times)} of {RETRIES_NEEDED} checks so far"
            problems = retry_problems(times)
            if problems:
                raise Failure("; ".join(problems))
            return None

        wait_watching("the check comes back after the back-off", WARNING_SECONDS, guest, pin, retried)
        wrong = server_answer_problems(server, scenario, 200)
        if wrong:
            raise Failure("; ".join(wrong))
        # The server now gives the expected answer, and the HTTP probe alone is asked: nothing else changes,
        # and no rejoin.
        server.mode = "expected"
        wait_watching("the warning goes from Settings and the status bar when the host's server passes a check",
                      WARNING_SECONDS, guest, pin,
                      lambda: "; ".join(cleared_problems(guest, pin, after_failure=True)
                                        + https_off_problems(guest.validation_log(pin.net_id))) or None)
        # A forced re-evaluation checks the network again. No timer is left after a pass, which the quiet hold
        # shows, so a new check at the server is the forced one.
        hold_quiet(guest, server, scenario, QUIET_SECONDS, pin)
        server.mode = "page"
        seen = len(server.checks(scenario))
        guest.reevaluate(pin.net_id)
        wait_watching("a forced re-evaluation checks the network again", FORCE_SECONDS, guest, pin,
                      lambda: None if len(server.checks(scenario)) > seen else "the server saw no new check")
        wait_watching("the warning returns after the forced check", WARNING_SECONDS, guest, pin,
                      lambda: "; ".join(warning_problems(guest, False, pin)) or None)
        server.mode = "expected"
        wait_watching("the warning goes again when the next check passes at the host's server", WARNING_SECONDS,
                      guest, pin,
                      lambda: "; ".join(cleared_problems(guest, pin, after_failure=True)
                                        + https_off_problems(guest.validation_log(pin.net_id))) or None)
        hold_validated(guest, SHORT_HOLD_SECONDS, pin)
    finally:
        guest.leave_internet_page()
    return (f"network {pin.net_id} validated while its first check was unanswered and at {pin.validated_at} "
            f"throughout, {len(server.checks(scenario))} checks at the host's server (HTTPS probe off)")


def same_network_problems(networks: dict[int, frozenset[str]], net_id: int) -> list[str]:
    """The network is still listed and is still the newest one: not gone, not replaced by a new join."""
    if net_id not in networks:
        return [f"network {net_id} is gone"]
    newest = max(networks)
    return [f"network {net_id} was replaced by network {newest}"] if newest != net_id else []


def unresolved_problems(guest: Guest, net_id: int) -> list[str]:
    """The pinned network is still the newest one and is as the platform leaves a network whose strict private
    DNS name fails: not validated, its private DNS reported broken."""
    networks = wifi_networks(guest.connectivity())
    problems = same_network_problems(networks, net_id)
    return problems + (private_dns_failed_problems(networks[net_id]) if net_id in networks else [])


def pin_recovered(guest: Guest, net_id: int, seconds: float = WARNING_SECONDS) -> Pin:
    """Waits for the network to be validated again with its private DNS not broken, and pins it (it was
    validated again, so `lastValidated` is new). Another network, or none, fails at once: the recovery of
    a network that was replaced is no proof about it."""
    found: list[Pin] = []

    def recovered() -> str | None:
        dump = guest.connectivity()
        networks = wifi_networks(dump)
        replaced = same_network_problems(networks, net_id)
        if replaced:
            raise Failure("; ".join(replaced))
        problems = validation_problems(networks[net_id])
        when = last_validated(dump).get(net_id)
        if not problems and when is None:
            problems = [f"network {net_id} shows no lastValidated"]
        if problems:
            return "; ".join(problems)
        found.append(Pin(net_id, when))
        return None

    wait_for("the network is validated again and its private DNS is not broken", seconds, recovered, guest,
             interval=2.0)
    return found[-1]


def first_pass_problems(messages: list[str]) -> str | None:
    """Whether a network's first check has passed, with the HTTP probe alone."""
    if not any(CHECK_PASSED_LINE in message for message in messages):
        return "no passing check yet"
    return "; ".join(https_off_problems(messages)) or None


def check_forced_evaluation_after_private_dns_failed(guest: Guest, server: "ProbeServer") -> str | None:
    """A forced re-evaluation of a network whose strict private DNS name fails is evaluated as the stock
    platform does: the network stays not validated, with its private DNS broken, and no check runs (a check
    would report it validated while its private DNS is broken). Each request is judged by itself: the log
    gained a new line that says it was forced, naming this network, and no internet check from the request to
    the end of the hold, and the server sees no check. When the setting is deleted the network is validated
    again with its private DNS no longer broken, and a new request is taken (a new line) and checked. The host's
    server answers every check (the expected 204, and the HTTP probe alone is asked), which it sees."""
    scenario = server.scenario()
    server.mode = "expected"
    server.clear()
    guest.reverse(server.port)
    guest.rejoin_with_settings({**page_settings(server.port, scenario), USE_HTTPS_FLAG: "0"},
                               clear=PRIVATE_DNS_SETTINGS)
    pin = pin_network(guest)
    net_id = pin.net_id
    wait_watching("the first check passes at the host's server", WARNING_SECONDS, guest, pin,
                  lambda: first_pass_problems(guest.validation_log(net_id)))
    union = LogUnion()
    # The setting changes with no rejoin: the platform evaluates the name for the network it has.
    guest.put_settings({"private_dns_mode": "hostname", "private_dns_specifier": UNRESOLVED_NAME})
    wait_problems("the platform reports the name that does not resolve: not validated, private DNS broken",
                  WARNING_SECONDS, guest, lambda: unresolved_problems(guest, net_id), interval=2.0)
    checks = len(server.checks(scenario))
    union.add(guest.validation_entries(net_id))
    before = list(union.entries)
    guest.reevaluate(net_id)
    deadline = time.monotonic() + UNVALIDATED_SECONDS
    while time.monotonic() < deadline:
        guest.alive()
        problems = unresolved_problems(guest, net_id)
        if len(server.checks(scenario)) > checks:
            problems.append("a check reached the host's server after the forced re-evaluation")
        union.add(guest.validation_entries(net_id))
        if problems:
            raise Failure("after the forced re-evaluation: " + "; ".join(problems))
        time.sleep(POLL_SECONDS)
    problems = forced_request_problems(before, list(union.entries), checked=False)
    if problems:
        raise Failure("; ".join(problems))
    # The name is not set any more: the network is validated again, and a new request checks it.
    guest.delete_settings(PRIVATE_DNS_SETTINGS)
    recovered = pin_recovered(guest, net_id)
    union.add(guest.validation_entries(net_id))
    before = list(union.entries)
    checks = len(server.checks(scenario))
    guest.reevaluate(net_id)

    def checked() -> str | None:
        union.add(guest.validation_entries(net_id))
        if len(server.checks(scenario)) <= checks:
            return "the server saw no new check"
        return "; ".join(forced_request_problems(before, list(union.entries), checked=True)) or None

    wait_watching("a forced re-evaluation checks the network that is validated again", FORCE_SECONDS, guest,
                  recovered, checked)
    return (f"network {net_id}: not validated and no check for {UNVALIDATED_SECONDS} s after the forced "
            "re-evaluation, validated and checked again once the name was gone")


def private_dns_probe_problems(first: int, last: int) -> list[str]:
    """The strict private DNS name is probed when the network is validated and not again for each check: at
    least one probe by the first failed check, and no more by the last."""
    if first < 1:
        return ["the log shows no private DNS probe: the strict name was never evaluated"]
    if last > first:
        return [f"private DNS was probed {last - first} more times while the checks failed ({first} -> {last}): "
                "each check evaluates it again"]
    return []


def check_strict_private_dns_holds(guest: Guest, server: "ProbeServer") -> str | None:
    """With a working strict private DNS name and a check that fails again and again, the network stays
    validated with its private DNS not broken, and the private DNS probe is not repeated for each check
    (the validation log, collected over the polls, holds the same probes before and after the checks)."""
    scenario = server.scenario()
    server.mode = "page"
    server.clear()
    guest.reverse(server.port)
    settings = {**page_settings(server.port, scenario), USE_HTTPS_FLAG: "0", "private_dns_mode": "hostname",
                "private_dns_specifier": PRIVATE_DNS_HOST}
    guest.rejoin_with_settings(settings, clear=())
    try:
        pin = pin_network(guest)
    except Failure as problem:
        raise Failure(f"{problem}; the host must reach the private DNS server {PRIVATE_DNS_HOST}") from problem
    union = LogUnion()
    first: list[int] = []

    def failed_enough() -> str | None:
        union.add(guest.validation_entries(pin.net_id))
        failed = union.count(CHECK_FAILED_LINE)
        if failed and not first:
            first.append(union.count(PRIVATE_DNS_PROBE))
        return None if failed >= RETRIES_NEEDED + 1 else f"{failed} failed checks so far"

    wait_watching("the checks fail one after the other while the network stays validated", WARNING_SECONDS,
                  guest, pin, failed_enough, interval=2.0)
    last = union.count(PRIVATE_DNS_PROBE)
    problems = private_dns_probe_problems(first[0], last)
    if problems:
        raise Failure("; ".join(problems))
    hold_validated(guest, SHORT_HOLD_SECONDS, pin)
    return f"{union.count(CHECK_FAILED_LINE)} failed checks, {last} private DNS probes, validated throughout"


def check_passing_check_clears(guest: Guest, server: "ProbeServer") -> str | None:
    """The expected answer (the host's server with the HTTPS probe off, the HTTP probe alone asked) passes the
    check of a network that has just joined: the current network's probe received the 204, no HTTPS probe was
    sent, the warning is not there and the network is validated."""
    scenario = server.scenario()
    server.mode = "expected"
    server.clear()
    guest.reverse(server.port)
    guest.rejoin_with_settings({**page_settings(server.port, scenario), USE_HTTPS_FLAG: "0"},
                               clear=PRIVATE_DNS_SETTINGS)
    pin = pin_network(guest)
    guest.show_internet_page()
    try:
        wait_problems("the warning is not there when the host's server passes the check", WARNING_SECONDS, guest,
                      lambda: cleared_problems(guest, pin)
                      + probe_answer_problems(guest.validation_log(pin.net_id), scenario, 204)
                      + https_off_problems(guest.validation_log(pin.net_id)))
        hold_validated(guest, SHORT_HOLD_SECONDS, pin)
    finally:
        guest.leave_internet_page()
    return None


def check_stock_servers_clear(guest: Guest) -> str | None:
    """With every setting and flag deleted the probes go to the stock servers through the host's network: the
    HTTP probe and the HTTPS probe both receive 204 from the stock URLs, the check passes and the warning is gone.
    A host that reaches no stock server cannot show this."""
    guest.rejoin_with_settings({}, clear=ALL_CHECK_SETTINGS)
    pin = pin_network(guest)
    guest.show_internet_page()
    try:
        wait_problems("the stock servers leave no warning (the host must reach them)", WARNING_SECONDS, guest,
                      lambda: cleared_problems(guest, pin) + stock_answer_problems(guest.validation_log(pin.net_id)))
    finally:
        guest.leave_internet_page()
    return None


def warning_sections(guest: Guest, server: "ProbeServer", report: "Report") -> None:
    """A failed check, and a page in place of the answer, show only the warning; the warning follows one
    network from a failing check to a passing one and back; a passing check shows none; strict private DNS
    is not probed again for each check. The settings are deleted after, so the sections that follow start
    from the stock ones."""
    try:
        report.section("a failed internet check shows only the warning, and the network stays validated", guest,
                       lambda: check_failed_check_warns(guest))
        report.section("the warning stays on the same network after a suspend and wake", guest,
                       lambda: check_warning_after_wake(guest))
        report.section("a sign-in page in place of the expected answer shows only the warning", guest,
                       lambda: check_page_answer_warns(guest, server, "redirect"))
        report.section("a block page in place of the expected answer shows only the warning", guest,
                       lambda: check_page_answer_warns(guest, server, "page"))
        report.section("the warning follows one network from a failing check to a passing one and back, at the "
                       "host's server with the HTTPS probe off", guest,
                       lambda: check_recovery_on_one_network(guest, server))
        report.section("a forced re-evaluation does not validate a network whose strict private DNS failed "
                       "(the host's server answers, the HTTPS probe off)", guest,
                       lambda: check_forced_evaluation_after_private_dns_failed(guest, server))
        report.section("a check that the host's server passes, with the HTTPS probe off, leaves no warning", guest,
                       lambda: check_passing_check_clears(guest, server))
        report.section("a check that the stock servers pass, with both probes asked, leaves no warning", guest,
                       lambda: check_stock_servers_clear(guest))
        report.section("a failing check does not probe a strict private DNS name again", guest,
                       lambda: check_strict_private_dns_holds(guest, server))
    finally:
        try:
            guest.delete_settings(ALL_CHECK_SETTINGS)
            guest.unreverse(server.port)
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"     the probe settings were not deleted: {problem}", file=sys.stderr)


def fresh(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    print("fresh data disk")
    guest = start_guest(options, adb, private, options.release, private / "fresh", provision=True, watched=True)
    server: ProbeServer | None = None
    try:
        server = ProbeServer()
        report.section("overlay is applied", guest, lambda: check_overlay(guest))
        report.section("VirtWifi is saved and joined at the first boot", guest, lambda: check_saved_and_joined(guest))
        report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
        warning_sections(guest, server, report)
        report.section("a failed internet check does not disable the network", guest,
                       lambda: check_not_disabled(guest))
        suspend_sections(guest, report)
        report.section("the station joins again after a restart", guest, lambda: check_restart(guest))
        report.section("a joined network keeps its metered setting across a restart", guest,
                       lambda: check_metered_kept(guest, waits))
        report.section("a network with Auto-connect off keeps it across a restart", guest,
                       lambda: check_autojoin_kept(guest, waits))
        report.section("a forgotten network stays forgotten", guest,
                       lambda: check_forgotten_stays_forgotten(guest, waits))
        report.section("Wi-Fi that is off stays off", guest, lambda: check_wifi_off_stays_off(guest, waits))
        report.section(CRASH_SECTION, guest, lambda: check_no_crashes(guest))
    finally:
        if server is not None:
            server.close()
        stop(guest)


def wake_only(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    """The sections that judge a suspend and wake, on a fresh data disk: the warning of a failed check on the network
    that a wake keeps, and the three stays."""
    print("fresh data disk (a suspend and wake only)")
    guest = start_guest(options, adb, private, options.release, private / "fresh", provision=True, watched=True)
    try:
        report.section("VirtWifi is saved and joined at the first boot", guest, lambda: check_saved_and_joined(guest))
        try:
            report.section("a failed internet check shows only the warning, and the network stays validated", guest,
                           lambda: check_failed_check_warns(guest))
            report.section("the warning stays on the same network after a suspend and wake", guest,
                           lambda: check_warning_after_wake(guest))
        finally:
            try:
                guest.delete_settings(ALL_CHECK_SETTINGS)
            except (Failure, subprocess.TimeoutExpired, OSError) as problem:
                print(f"     the probe settings were not deleted: {problem}", file=sys.stderr)
        suspend_sections(guest, report)
        report.section(CRASH_SECTION, guest, lambda: check_no_crashes(guest))
    finally:
        stop(guest)


def check_repaired(guest: Guest, waits: Waits) -> str | None:
    """The main log buffer is small and nearly full at boot, so the script's repair line is looked for at
    every poll and kept once seen, and its denials are read early as well as by the later check."""
    state = selection_status(guest.dump())
    logged = repair_logged(guest.script_log())
    print(f"     the network is {state} when the release under test has booted")
    # The script has polled the station by now.
    guest.read_denials()
    if state != "NETWORK_SELECTION_PERMANENTLY_DISABLED" and not logged:
        raise Failure(f"the data disk holds the network {state}, not disabled: the previous release's disabled "
                      "state did not reach the disk, so no repair is shown")

    def joined() -> str | None:
        nonlocal logged
        station = guest.joined()
        if not logged and repair_logged(guest.script_log()):
            logged = True
            # The script has saved the network again and logged it.
            guest.read_denials()
        return None if station else "the station has not joined"

    wait_for("the first boot of the release joins the network", waits.repair, joined, guest, interval=POLL_SECONDS)
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the network joined but is {status}")
    if not logged and not repair_logged(guest.script_log()):
        raise Failure("the network joined without the boot script saving it again")
    return None


def check_disabled_on_disk(guest: Guest) -> None:
    """The previous release's disk holds the network disabled: after a restart of its guest the station
    stays away from the network and the dump says it is disabled. With the checks still failing, a
    network the disk held enabled is joined at boot and disabled again in memory, which a dump alone does
    not tell apart, so the station is watched first."""
    guest.restart()
    deadline = time.monotonic() + BOOT_JOIN_SECONDS
    while True:
        guest.alive()
        if guest.joined():
            raise Failure("the station joined the network after a restart of the previous release, so its "
                          "disk holds the network enabled")
        if time.monotonic() >= deadline:
            break
        time.sleep(POLL_SECONDS)
    state = selection_status(guest.dump())
    if state != "NETWORK_SELECTION_PERMANENTLY_DISABLED":
        raise Failure(f"the network is {state} after a restart of the previous release, so its "
                      "disabled state did not reach the disk")
    if guest.joined():
        raise Failure("the station joined the network after a restart of the previous release")


def previous_never_disables(guest: Guest) -> bool:
    """Whether the previous release's platform keeps the network enabled for lack of internet: its overlay
    holds the largest thresholds. A platform without the overlay has the stock ones and disables it."""
    try:
        check_overlay(guest)
    except Failure:
        return False
    return True


def boot_script_finished(guest: Guest) -> str | None:
    """None once the boot script has run to its end on this boot, VirtWifi is saved and the station has joined it;
    else what is still missing. It does not read the script's marker, which the shell cannot: a boot that saved
    the network ends at marker 1, and one that found it stored and joined at 2, and both end like this. A script
    that does not save the network, or leaves it unjoined (Wi-Fi off, or no join within the settle seconds),
    fails here, and so does a disk whose script did not run (no saved network, no service state). What the
    script logged of its marker, and of saving the network again, the sections that care read apart from this."""
    state = guest.script_service()
    if state != "stopped":
        return f"the boot script's service is {state or 'not started'!r}, not 'stopped'"
    if network_id(guest.listing()) is None:
        return "the boot script has stopped and VirtWifi is not saved"
    if not guest.joined():
        return "the boot script has stopped and the station has not joined VirtWifi"
    return None


def prepare_kept(guest: Guest, waits: Waits) -> str | None:
    """On the disk of a release that never disables the network: the station has joined it and the boot
    script has finished (it marks the network joined, boot_script_finished), the user marked it metered
    (saving it again would reset that), and the probes fail the check; a restart of this guest shows what
    the disk holds: the script does nothing more, so it finishes again and the metered setting is still
    there."""
    wait_for("VirtWifi is saved", 60, lambda: None if network_id(guest.listing()) is not None else "not saved", guest)
    guest.wait_joined(JOIN_SECONDS, "the previous release joins VirtWifi")
    wait_for("the boot script marks the network joined", waits.repair, lambda: boot_script_finished(guest), guest,
             interval=POLL_SECONDS)
    guest.shell(f"cmd wifi add-network {SSID} open -m")
    guest.wait_joined(JOIN_SECONDS, "the metered network is joined")
    if metered_override(guest.dump()) != 1:
        raise Failure("the network is not marked metered")
    guest.delete_settings(PRIVATE_DNS_SETTINGS)
    guest.put_settings(CHECK_SETTINGS)
    guest.restart()
    guest.wait_joined(JOIN_SECONDS, "the station joins after the restart")
    wait_for("the boot script finishes after the restart", waits.repair, lambda: boot_script_finished(guest), guest,
             interval=POLL_SECONDS)
    if metered_override(guest.dump()) != 1:
        raise Failure(f"the disk holds metered {metered_override(guest.dump())}, not 1: the boot script saved the "
                      "network again")
    if selection_status(guest.dump()) != "NETWORK_SELECTION_ENABLED":
        raise Failure("the previous release's network is not enabled")
    return None


def check_left_alone(guest: Guest, waits: Waits) -> str | None:
    """The first boot of the release under test finds the station joined once (marker 2), so the script saves
    nothing again: it finishes at once, and the metered setting and the join are the disk's."""
    guest.wait_joined(JOIN_SECONDS, "the station joins after the first boot of the release")
    time.sleep(MARGIN_SECONDS)
    wait_for("the boot script finishes on the first boot of the release", waits.repair,
             lambda: boot_script_finished(guest), guest, interval=POLL_SECONDS)
    if metered_override(guest.dump()) != 1:
        raise Failure("the boot script saved a joined network again and reset its metered setting")
    status = selection_status(guest.dump())
    if status != "NETWORK_SELECTION_ENABLED":
        raise Failure(f"the network is {status}")
    check_no_repair_logged(guest, "although the station had joined")
    return None


def check_warning_on_disk(guest: Guest) -> str | None:
    """The probe settings the disk holds fail the check of the network the station joined at this boot."""
    lost = [key for key, value in CHECK_SETTINGS.items() if guest.read_setting(key) != value]
    if lost:
        raise Failure(f"the data disk lost the probe settings {lost}")
    pin = pin_network(guest)
    warning_shown(guest, True, WARNING_HOLD_SECONDS, "the failed check shows only the warning on the upgraded disk",
                  pin)
    return None


# What the first boot of a quick-stop case waits for before it stops the guest: a guest-side condition, polled by
# the guest itself, so that the stop follows it by the least the host can manage.
QUICK_TRIGGERS = {
    "saved": f"cmd wifi list-networks | grep -q {SSID}",
    "joined": f"cmd wifi status | grep -q 'connected to \"{SSID}\"'",
}


def stop_when(options: argparse.Namespace, adb: str, private: pathlib.Path, instance: pathlib.Path,
              trigger: str) -> None:
    """Boots a fresh data disk and stops the guest the moment the trigger holds: the Wi-Fi service lists VirtWifi
    (the boot script saved it a moment before) or the station reports the join."""
    guest = start_guest(options, adb, private, options.release, instance, provision=False)
    try:
        try:
            guest.shell(f"end=$(( $(date +%s) + {QUICK_FIRST_SECONDS} )); while [ $(date +%s) -lt $end ]; do "
                        f"{QUICK_TRIGGERS[trigger]} && exit 0; sleep 0.2; done; exit 1",
                        timeout=QUICK_FIRST_SECONDS + 30)
        except (Failure, subprocess.TimeoutExpired) as problem:
            raise Failure(f"the first boot did not reach the stop point ({trigger}) within {QUICK_FIRST_SECONDS} s "
                          f"of boot_completed: {problem}") from problem
    finally:
        stop(guest)


@dataclass
class Quick:
    """What the boot after a stop showed: whether the stopped data lacked the network at the first observation."""
    landed: bool | None = None


def judge_quick_boot(guest: Guest, waits: Waits, quick: Quick) -> str | None:
    """The boot after a quick stop. The stopped data's own list is read first, before the script's guard (ten
    seconds) can save anything: a list without VirtWifi is a stop that came before the store was written, which
    sets quick.landed; the script's log is not evidence of the disk. Data that lacked the network must hold an
    address within LEASE_SECONDS of boot_completed (judged by the time the observation that saw it returned);
    the script must log that it left marker 1, since a boot that saved the network never sets 2; and the next
    boot, after a restart the store survives, must log that it found the network stored and joined and set 2.
    A list that held the network is a stop that came after the write: only the lease is judged, and the
    note says so. The script's lines are kept from every poll, as the main log buffer is small."""
    started = time.monotonic()
    quick.landed = network_id(guest.listing()) is None
    seen = {"one": False, "two": False, "not_stored": False}

    def read_log() -> None:
        text = guest.script_log()
        seen["one"] = seen["one"] or marker_one_logged(text)
        seen["two"] = seen["two"] or marker_two_logged(text)
        seen["not_stored"] = seen["not_stored"] or not_stored_logged(text)

    leased: float | None = None
    deadline = started + LEASE_SECONDS
    while True:
        guest.alive()
        read_log()
        if guest.joined() and guest.has_address():
            leased = time.monotonic() - started
            break
        if time.monotonic() >= deadline:
            break
        time.sleep(2)
    saved = network_id(guest.listing()) is not None
    if leased is None or leased > LEASE_SECONDS or not saved:
        raise Failure(
            f"the data {'that the stop left without' if quick.landed else 'that held'} {SSID} "
            + (f"held an address only {leased:.0f} s after boot_completed" if leased is not None
               else f"held no address within {LEASE_SECONDS} s of boot_completed")
            + f", and the Wi-Fi service lists {'it' if saved else 'nothing'}; the boot script "
            + f"{'logged' if seen['not_stored'] else 'did not log'} that it saved the network again")
    if not quick.landed:
        return f"the stop came after the store wrote the network; the lease took {leased:.0f} s"
    wait_for("the boot script ends", waits.quiet, lambda: None if guest.script_service() == "stopped"
             else "the boot script is still running", guest, interval=POLL_SECONDS)
    read_log()
    if seen["two"]:
        raise Failure("the boot that had to save the network set marker 2: a stop before the store's write "
                      "would then leave marker 2 and no network, which nothing repairs")
    if not seen["one"]:
        raise Failure("the boot script did not log that it left marker 1 after saving the network")
    check_no_denials(guest)
    guest.restart()
    if network_id(guest.listing()) is None:
        raise Failure(f"{SSID} is not listed after a restart: the store did not hold the network the script saved")
    wait_for("the boot script ends after the restart", waits.quiet, lambda: None if guest.script_service() == "stopped"
             else "the boot script is still running", guest, interval=POLL_SECONDS)
    if not marker_two_logged(guest.script_log()):
        raise Failure("the boot that found the network stored and joined did not log that it set marker 2")
    return (f"the data lost the network; the script saved it again, the lease took {leased:.0f} s, the boot left "
            "marker 1 and the next boot set 2")


def run_quick_case(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits,
                   trigger: str, title: str, required: bool, attempts: int = QUICK_ATTEMPTS) -> None:
    """Stops fresh guests at the trigger, boots the same data, and judges that boot. A stop that came after the
    store wrote the network is tried again; when none came before it, the case fails if `required` and is a note
    if not."""
    for attempt in range(1, attempts + 1):
        instance = private / f"quick-{trigger}-{attempt}"
        stop_when(options, adb, private, instance, trigger)
        guest = start_guest(options, adb, private, options.release, instance, provision=False, watched=True)
        quick = Quick()
        failed = len(report.failures)
        try:
            report.section(f"{title} (attempt {attempt})", guest, lambda: judge_quick_boot(guest, waits, quick))
        finally:
            stop(guest)
            shutil.rmtree(instance, ignore_errors=True)
        if quick.landed or len(report.failures) > failed:
            return
    message = f"{title}: no stop came before the Wi-Fi service stored the network in {attempts} attempts"
    if required:
        print(f"FAIL {message}", file=sys.stderr)
        report.failures.append(message)
    else:
        print(f"     not exercised: {message}")


def quick_stop(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    print("quick stop: a guest stopped before the Wi-Fi service stored the network")
    run_quick_case(options, adb, private, report, waits, "saved",
                   "a guest stopped when the Wi-Fi service lists VirtWifi gets it back", required=True)
    run_quick_case(options, adb, private, report, waits, "joined",
                   "a guest stopped at the first reported join keeps no marker 2 beside a missing network",
                   required=False, attempts=QUICK_JOIN_ATTEMPTS)


def upgrade(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits) -> None:
    current = boot_android.release_path(options.release)
    for index, release in enumerate(options.previous_release):
        previous = boot_android.release_path(release)
        if previous == current:
            print("upgrade: the previous release is the release under test; nothing to upgrade from")
            continue
        upgrade_from(options, adb, private, report, waits, previous, current, private / f"upgrade-{index}")


def upgrade_from(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report, waits: Waits,
                 previous: pathlib.Path, current: pathlib.Path, instance: pathlib.Path) -> None:
    print(f"upgrade: a data disk of {previous.name} booted by {current.name}")
    guest = start_guest(options, adb, private, previous, instance, provision=True)
    prepared = False
    try:
        kept = previous_never_disables(guest)

        def disable() -> str | None:
            nonlocal prepared
            wait_for("VirtWifi is saved", 60, lambda: None if network_id(guest.listing()) is not None
                     else "not saved", guest)
            guest.inject_failing_checks()
            guest.forget_and_save()
            guest.wait_joined(JOIN_SECONDS, "the station joins the network saved again")
            wait_for("the previous release disables the network", HOLD_SECONDS + JOIN_SECONDS,
                     lambda: None if selection_status(guest.dump()) == "NETWORK_SELECTION_PERMANENTLY_DISABLED"
                     else "still enabled", guest, interval=POLL_SECONDS)
            # The Wi-Fi service stores the change a few seconds later, and the disk the release under test
            # boots holds only what was stored: a restart of this guest shows what that is.
            check_disabled_on_disk(guest)
            prepared = True
            return None

        def keep() -> str | None:
            nonlocal prepared
            note = prepare_kept(guest, waits)
            prepared = True
            return note

        if kept:
            report.section(f"{previous.name} leaves VirtWifi joined and metered on its disk, with the probes failing",
                           guest, keep)
        else:
            report.section(f"{previous.name} leaves VirtWifi disabled on its disk", guest, disable)
    finally:
        stop(guest)
    if not prepared:
        return
    guest = start_guest(options, adb, private, options.release, instance, provision=False, watched=True)
    try:
        if kept:
            report.section("the first boot of the release leaves the joined network alone", guest,
                           lambda: check_left_alone(guest, waits))
            report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
            report.section("a failed internet check shows only the warning on the upgraded disk", guest,
                           lambda: check_warning_on_disk(guest))
            report.section("Auto-connect turned off on the upgraded disk stays off across a restart", guest,
                           lambda: check_autojoin_kept(guest, waits, left_alone=False))
        else:
            report.section("the first boot of the release repairs the network", guest,
                           lambda: check_repaired(guest, waits))
            report.section("the boot script runs without SELinux denials", guest, lambda: check_no_denials(guest))
            report.section("the repaired network joins again after a suspend and wake", guest,
                           lambda: check_suspend(guest))
            report.section("a failed internet check shows only the warning after the repair", guest,
                           lambda: check_failed_check_warns(guest))
            report.section("Auto-connect turned off after the repair stays off across a restart", guest,
                           lambda: check_autojoin_kept(guest, waits, left_alone=True))
        report.section(CRASH_SECTION, guest, lambda: check_no_crashes(guest))
    finally:
        stop(guest)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release under test (default: accepted)")
    parser.add_argument("--previous-release", type=pathlib.Path, action="append",
                        help="installed release whose data disk the upgrade section boots first (the section "
                        "runs only when this is given; give the option once for each release)")
    parser.add_argument("--quick-stop-only", action="store_true",
                        help="run the quick-stop case alone (a short run that shows an image's defect or fix)")
    parser.add_argument("--wake-only", action="store_true",
                        help="run the sections that judge a suspend and wake alone (a short run that shows a "
                        "kernel's defect or fix: the failed check's warning, and the three stays)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--forward-port", type=int, default=slot_ports.slot_port(FORWARD_OFFSET, DEFAULT_FORWARD_PORT),
                        help="loopback port the launcher forwards to the guest's Wi-Fi address (default: the guest "
                        f"slot's fourth port when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_FORWARD_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=BOOT_SECONDS,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    if options.wake_only and (options.quick_stop_only or options.previous_release):
        print("--wake-only runs on its own: it takes neither --quick-stop-only nor --previous-release", file=sys.stderr)
        return 2
    adb = str(boot_android.resolve_executable(options.adb, sdk_fallback=True))
    script = BOOT_SCRIPT.read_text(encoding="utf-8")
    waits = Waits(boot_settle_seconds(script), REJOIN_SECONDS)
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-awn-"))
    report = Report()
    try:
        if options.wake_only:
            wake_only(options, adb, private, report)
        else:
            if not options.quick_stop_only:
                fresh(options, adb, private, report, waits)
            quick_stop(options, adb, private, report, waits)
            if options.previous_release and not options.quick_stop_only:
                upgrade(options, adb, private, report, waits)
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        report.failures.append(str(problem))
    finally:
        shutil.rmtree(private, ignore_errors=True)
    if report.failures:
        print("FAIL: " + "; ".join(report.failures), file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
