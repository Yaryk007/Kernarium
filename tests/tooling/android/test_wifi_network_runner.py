#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Wi-Fi network runner's reading of the guest, without a guest.

run_android_wifi_network.py reads `cmd wifi status`, `cmd wifi list-networks`,
`dumpsys wifi` and `cmd overlay list`. The texts below have the layout those
commands print (WifiShellCommand, WifiConfiguration's dump and the overlay
manager), so the runner's parsers run here; the runner also takes the boot
script's waits from the tracked script, and reads the overlay's integers with the
verifier's reader of compiled resource tables. The upgrade section's checks run
against a guest that answers from a script, on a clock that a sleep moves on. So do the sections that show
a failed check as a warning (`dumpsys connectivity`, `dumpsys network_stack`, `dumpsys notification`,
`dumpsys activity` and a UI dump), and the host's probe server answers a real client. A simulated guest
follows the server the probes ask, so the sections that wait for a failing check to pass, and for a check to
come back, run over the fake clock, and each fault of the platform or of the screen fails the section.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import http.client
import importlib.util
import inspect
import io
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest import mock
import zipfile
from collections.abc import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wifi_dump_fixtures  # noqa: E402

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "tests/integration/android/run_android_wifi_network.py"
SPEC = importlib.util.spec_from_file_location("run_android_wifi_network", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)

sys.path.insert(0, str(REPOSITORY / "tests/integration/support"))
import gpu_suspend_log  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_builder_lane import synthetic_resource_table  # noqa: E402

JOINED = """\
Wifi is enabled
Wifi scanning is only available when wifi is enabled
==== Primary ClientModeManager instance ====
Wifi is connected to "VirtWifi"
WifiInfo: SSID: "VirtWifi", Supplicant state: COMPLETED
"""
NOT_JOINED = """\
Wifi is enabled
Wifi scanning is only available when wifi is enabled
==== Primary ClientModeManager instance ====
Wifi is not connected
"""
OFF = """\
Wifi is disabled
Wifi scanning is only available when wifi is enabled
"""
LISTING = """\
Network Id      SSID                         Security type
0            VirtWifi                         open
0            VirtWifi                         owe^
"""
DISABLED_DUMP = """\
 NetworkSelectionStatus NETWORK_SELECTION_PERMANENTLY_DISABLED
 mNetworkSelectionDisableReason NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT
 hasEverValidatedInternetAccess: false
NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT counter:1
 meteredOverride 1
"""
ENABLED_DUMP = """\
 NetworkSelectionStatus NETWORK_SELECTION_ENABLED
 hasEverValidatedInternetAccess: true
NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT counter:3
NETWORK_SELECTION_DISABLED_NO_INTERNET_TEMPORARY counter:2
 meteredOverride 0
 lcuid=0 allowAutojoin=true mAllowedAutoJoinInAdvancedProtection=true noInternetAccessExpected=false
"""
AUTOJOIN_OFF_DUMP = ENABLED_DUMP.replace("allowAutojoin=true", "allowAutojoin=false")
# The dump as the Wi-Fi service prints a saved network: a header with the configuration's ID, then its values.
NETWORK_HEADER = '* ID: 0 SSID: "VirtWifi" PROVIDER-NAME: null HIDDEN: false\n'
OTHER_HEADER = '* ID: 1 SSID: "VirtWifi" PROVIDER-NAME: null HIDDEN: false\n'
NETWORK_ON = NETWORK_HEADER + ENABLED_DUMP
NETWORK_OFF = NETWORK_HEADER + AUTOJOIN_OFF_DUMP
DENIAL = ("11-11 09:15:55.822   333   333 W auditd  : type=1400 audit(0.0:7): avc:  denied  "
          "{ read } for comm=\"cmd\" scontext=u:r:kernarium_wifi_network:s0 "
          "tcontext=u:object_r:system_file:s0 tclass=file permissive=0\n")
REPAIR_LINE = "11-11 09:20:00.000 I kernarium_wifi_network: VirtWifi saved again: Wi-Fi is on and nothing joined\n"
LEFT_ALONE = "11-11 09:20:00.000 I kernarium_wifi_network: VirtWifi has Auto-connect off: left alone\n"

def agent(net_id: int, capabilities: str, transport: str = "WIFI", validated_at: int | None = None,
          created: str = "2026-11-10T13:30:32.485Z", private_dns_broken: bool = False,
          first_validated_at: int | None = None) -> str:
    """The line `dumpsys connectivity` prints for a network: NetworkAgentInfo.toString with its fields in its
    order, trimmed to what the runner reads. `lastValidated` is printed only while the network is validated
    (`firstValidated` once it ever was); NetworkCapabilities.toString prints broken private DNS as the token
    ` PrivateDnsBroken` after the SSID, not in the capability list."""
    first = validated_at if first_validated_at is None else first_validated_at
    return (f"  NetworkAgentInfo{{network{{{net_id}}}  handle{{441492361229}}  ni{{{transport} CONNECTED extra: }} "
            f"created={created} Score(Policies : TRANSPORT_PRIMARY&EVER_EVALUATED ; KeepConnected : 0)  created 149238"
            + ("" if first is None else f" firstValidated {first}")
            + ("" if validated_at is None else f" lastValidated {validated_at}")
            + "  lp{InterfaceName: wlan0 LinkAddresses: [ 10.0.2.16/24 ] DnsAddresses: [ /10.0.2.3 ]}"
            + f"  nc{{[ Transports: {transport} Capabilities: {capabilities} LinkUpBandwidth>=12000Kbps "
            'LinkDnBandwidth>=12000Kbps Specifier: <WifiNetworkAgentSpecifier [WifiConfiguration=, SSID="VirtWifi"]> '
            'TransportInfo: <SSID: "VirtWifi", BSSID: 02:00:00:00:01:00, Supplicant state: COMPLETED> '
            'SSID: "VirtWifi"' + (" PrivateDnsBroken" if private_dns_broken else "")
            + " UnderlyingNetworks: Null]}  factorySerialNumber=-1}\n")


REQUEST_OUTPUT = "request t=1000.5 ping_rc=0 dns_tcp_bytes=45\n"
WAKE_MARK = "wake-0123456789abcdef"
SUPPLICANT_DISCONNECT = ("wlan0: CTRL-EVENT-DISCONNECTED bssid=62:eb:c4:11:6e:64 reason=3 locally_generated=1")
PLATFORM_DISCONNECT = "[102 WIFI] disconnected, was satisfying 25"

STOCK = "NOT_METERED&INTERNET&NOT_RESTRICTED&TRUSTED&NOT_VPN&NOT_ROAMING&FOREGROUND&NOT_CONGESTED&NOT_SUSPENDED"
VALIDATED = STOCK.replace("NOT_VPN&", "NOT_VPN&VALIDATED&")
CONNECTIVITY = "Current networks:\n" + agent(101, STOCK) + agent(102, VALIDATED, validated_at=5000) + "Other state\n"
NETWORK_STACK = """\
NetworkStack version:
Recently active IpClient logs:
IpClient.wlan0

Validation logs (most recent first):
102 - "VirtWifi"
  2026-11-10T13:30:32.803492 - Wi-Fi is validated before its internet check
  2026-11-10T13:30:44.965598 - PROBE_HTTPS https://192.0.2.1/generate_204 Probe failed with exception java.net.SocketTimeoutException: failed to connect after 10000ms
  2026-11-10T13:30:44.971100 - PROBE_HTTP http://192.0.2.1/generate_204 Probe failed with exception java.net.SocketTimeoutException: failed to connect after 10000ms
  2026-11-10T13:30:44.975000 - Wi-Fi internet check failed, the network stays validated
101 - "VirtWifi"
  2026-11-10T13:29:29.956111 - PROBE_DNS 192.0.2.1 16ms FAIL in type ADDRCONFIG java.net.UnknownHostException
"""
PASSED_NETWORK_STACK = NETWORK_STACK.replace(
    "Wi-Fi internet check failed, the network stays validated", "Wi-Fi internet check passed")


def stack_of(net_id: int) -> str:
    """The validation logs with the failed check of the network under test under another ID."""
    return NETWORK_STACK.replace('102 - "VirtWifi"', f'{net_id} - "VirtWifi"')
QUIET_NOTIFICATIONS = """\
Current Notification Manager state:
  Notification List:
    NotificationRecord(0x0a1b2c3d: pkg=com.android.systemui user=UserHandle{0} id=7 tag=null importance=1 key=0|com.android.systemui|7|null|10089)
"""
SIGN_IN_NOTIFICATIONS = QUIET_NOTIFICATIONS + (
    "    NotificationRecord(0x0f3a2b1c: pkg=android user=UserHandle{0} id=41 tag=ConnectivityNotification:102 "
    "importance=2 key=0|android|41|ConnectivityNotification:102|1000)\n")
QUIET_ACTIVITIES = "ACTIVITY MANAGER ACTIVITIES\n  * Task{abc #1 type=home} A=1000:com.android.launcher3\n"
PORTAL_ACTIVITIES = QUIET_ACTIVITIES + (
    "  * Task{def #9 type=standard} A=10200:com.android.captiveportallogin\n"
    "    ActivityRecord{1 u0 com.android.captiveportallogin/.CaptivePortalLoginActivity t9}\n")


def hierarchy(settings_text: str, bar_description: str | None) -> str:
    """A `uiautomator dump --windows`: Settings' Internet page and, when given, the status bar's Wi-Fi icon."""
    bar = ("" if bar_description is None else
           '<node index="2" text="" resource-id="com.android.systemui:id/wifi_signal" '
           'class="android.widget.ImageView" package="com.android.systemui" '
           f'content-desc="{bar_description}" bounds="[600,0][640,24]" />')
    return (
        "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>\n"
        '<displays><display id="0"><hierarchy rotation="0">'
        '<node index="0" text="" class="android.widget.FrameLayout" package="com.android.settings" '
        'content-desc="" bounds="[0,0][720,1280]">'
        '<node index="1" text="VirtWifi" class="android.widget.TextView" package="com.android.settings" '
        'content-desc="" bounds="[0,100][720,150]" />'
        f'<node index="2" text="{settings_text}" class="android.widget.TextView" package="com.android.settings" '
        'content-desc="" bounds="[0,150][720,200]" /></node>'
        f"{bar}</hierarchy></display></displays>\nUI hierarchy dumped to: /data/local/tmp/x.xml\n")


WARNING_SCREEN = hierarchy("Connected / Limited connection", "Wifi signal full.,No internet")
CLEAR_SCREEN = hierarchy("Connected", "Wifi signal full.")

WAITS = runner.Waits(45, 180)


class FakeClock:
    """The runner's clock: a sleep moves it on at once."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def time(self) -> float:
        return 1.0e9 + self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class ScriptedGuest(runner.Guest):
    """A guest that answers each command from its list, one answer per call; the last answer repeats."""

    COMMANDS = {"status": "cmd wifi status", "dump": "dumpsys wifi",
                "script_log": f"logcat -d -s {runner.SCRIPT_LOG_TAG}", "log": "logcat -d -b all",
                "connectivity": "dumpsys connectivity", "network_stack": "dumpsys network_stack",
                "notifications": "dumpsys notification --noredact", "activities": "dumpsys activity activities",
                "ui": (f"uiautomator dump --windows {runner.UI_DUMP} >/dev/null 2>&1; cat {runner.UI_DUMP} "
                       f"2>/dev/null; rm -f {runner.UI_DUMP}"),
                "script_service": f"getprop {runner.SERVICE_PROPERTY}", "listing": "cmd wifi list-networks"}

    def __init__(self, **answers: list[str]) -> None:
        self.answers = {self.COMMANDS[name]: list(values) for name, values in answers.items()}
        self.denials = []
        self.denial_reads = 0
        self.restarts = 0

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        answers = self.answers[command]
        return answers.pop(0) if len(answers) > 1 else answers[0]

    def alive(self) -> None:
        pass

    def restart(self) -> None:
        self.restarts += 1

    def show_internet_page(self) -> None:
        pass

    def leave_internet_page(self) -> None:
        pass


class StubbedGuest(ScriptedGuest):
    """A scripted guest whose actions on the network (settings, rejoin, suspend, adb reverse) are only recorded."""

    def __init__(self, **answers: list[str]) -> None:
        super().__init__(**answers)
        self.calls: list[str] = []
        # What a watched suspend leaves: the request's output, what the guest's log holds after the mark, and what
        # the platform does at the wake.
        self.request = REQUEST_OUTPUT
        self.after_mark = ""
        self.on_wake = lambda: None
        # The echo server's log: the connection that the host closed in a frozen stay ended before the request began.
        self.echo_lines = "close A 1000.2 rc=0\n"

    def reevaluate(self, net_id: int) -> None:
        self.calls.append(f"reevaluate {net_id}")

    def reverse(self, port: int) -> None:
        self.calls.append(f"reverse {port}")

    def unreverse(self, port: int) -> None:
        self.calls.append(f"unreverse {port}")

    def put_settings(self, settings: dict[str, str]) -> None:
        self.calls.append(f"put {sorted(settings)}")

    def delete_settings(self, keys: tuple[str, ...] | list[str]) -> None:
        self.calls.append(f"delete {sorted(keys)}")

    def forget_and_save(self) -> None:
        self.calls.append("forget and save")

    def wait_joined(self, seconds: float, what: str) -> None:
        self.calls.append(f"joined {what}")

    def suspend_and_wake(self, watch: bool = False, frozen: "Callable[[], None] | None" = None) -> None:
        self.calls.append("suspend and wake" if frozen is None else "suspend and wake while frozen")
        if frozen is not None:
            frozen()
        self.on_wake()
        if watch:
            self.wake = runner.Wake(WAKE_MARK, runner.time.monotonic(), self.request)

    def log_text(self) -> str:
        return log_line(1, 1, "I", runner.SYNC_TAG, WAKE_MARK) + self.after_mark

    def echo_log(self) -> str:
        return self.echo_lines

    def has_address(self) -> bool:
        return True

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        if command.startswith("cmd wifi set-wifi-enabled"):
            self.calls.append(command)
            return ""
        return super().shell(command, timeout)

    def start_echo(self) -> None:
        self.calls.append("start echo")

    def stop_echo(self) -> None:
        self.calls.append("stop echo")

    def diagnose(self) -> str:
        return ""


UNPROBED_STACK = """\
Validation logs (most recent first):
100 - "VirtWifi"
  2026-11-12T10:33:06.272454 - Wi-Fi is validated without probes, resolving private DNS

useNeighborResource: false
"""
SILENT_STACK = """\
Validation logs (most recent first):
100 - "VirtWifi"
  2026-11-12T10:33:06.272454 - Probing https://connectivitycheck.gstatic.com/generate_204
"""
CHECK_FAILED_STACK = """\
Validation logs (most recent first):
100 - "VirtWifi"
  2026-11-12T10:33:06.272454 - Wi-Fi is validated before its internet check
  2026-11-12T10:33:16.272454 - Wi-Fi internet check failed, the network stays validated
"""
PRIVATE_DNS_STACK = """\
Validation logs (most recent first):
100 - "VirtWifi"
  2026-11-12T10:33:06.272454 - PROBE_PRIVDNS 1a2b3c4d-dnsotls-ds.metric.gstatic.com 12ms: []
"""
SCORED_CONNECTIVITY = (
    "  NetworkAgentInfo{network{100}  handle{432902426637}  ni{WIFI CONNECTED extra: } created=2026-11-12T10:33:01.583Z "
    "Score(Policies : TRANSPORT_PRIMARY&EVER_EVALUATED&IS_UNMETERED&EVER_VALIDATED&IS_VALIDATED ; KeepConnected : 0) "
    "lp{{InterfaceName: wlan0}}\n")
SCORED_CONNECTIVITY_NOT_VALIDATED = SCORED_CONNECTIVITY.replace("&IS_VALIDATED", "")
NOTHING_COUNTED = " NetworkSelectionStatus NETWORK_SELECTION_ENABLED\n hasEverValidatedInternetAccess: true\n"


class ReadingTests(unittest.TestCase):
    def test_the_station_is_joined_only_when_it_says_so(self) -> None:
        self.assertTrue(runner.wifi_joined(JOINED))
        self.assertFalse(runner.wifi_joined(NOT_JOINED))
        self.assertFalse(runner.wifi_joined(OFF))
        self.assertFalse(runner.wifi_joined('Wifi is connected to "OtherNet"\n'))

    def test_wifi_is_on_only_when_it_says_so(self) -> None:
        self.assertTrue(runner.wifi_enabled(JOINED))
        self.assertTrue(runner.wifi_enabled(NOT_JOINED))
        self.assertFalse(runner.wifi_enabled(OFF))

    def test_a_network_listed_twice_counts_once(self) -> None:
        self.assertEqual(runner.saved_ssids(LISTING), ["VirtWifi"])
        self.assertEqual(runner.network_id(LISTING), 0)
        self.assertEqual(runner.saved_ssids("No networks\n"), [])
        self.assertIsNone(runner.network_id("No networks\n"))

    def test_the_selection_status_and_its_reason(self) -> None:
        self.assertEqual(runner.selection_status(DISABLED_DUMP), "NETWORK_SELECTION_PERMANENTLY_DISABLED")
        self.assertEqual(runner.selection_disable_reason(DISABLED_DUMP),
                         "NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT")
        self.assertEqual(runner.selection_status(ENABLED_DUMP), "NETWORK_SELECTION_ENABLED")
        self.assertIsNone(runner.selection_disable_reason(ENABLED_DUMP))
        self.assertIsNone(runner.selection_status("nothing\n"))

    def test_failed_checks_are_counted_over_both_reasons(self) -> None:
        self.assertEqual(runner.no_internet_count(DISABLED_DUMP), 1)
        self.assertEqual(runner.no_internet_count(ENABLED_DUMP), 5)
        self.assertEqual(runner.no_internet_count("nothing\n"), 0)

    def test_a_network_the_dump_prints_three_times_is_counted_once(self) -> None:
        self.assertEqual(runner.no_internet_count(ENABLED_DUMP * 3), 5)

    def test_auto_connect_is_read_from_every_copy_of_the_network(self) -> None:
        self.assertEqual(runner.autojoin_values(ENABLED_DUMP), [True])
        self.assertEqual(runner.autojoin_values(AUTOJOIN_OFF_DUMP * 2), [False, False])
        self.assertEqual(runner.autojoin_values(ENABLED_DUMP + AUTOJOIN_OFF_DUMP), [True, False])
        self.assertEqual(runner.autojoin_values("mAutoJoinEnabled=false\n"), [])

    def test_only_denials_of_the_scripts_domain_count(self) -> None:
        domain = runner.SCRIPT_DOMAIN
        self.assertEqual(len(runner.avc_denials(DENIAL, domain)), 1)
        self.assertEqual(len(runner.avc_denials(DENIAL.replace("scontext=u:r:kernarium_wifi_network:s0",
                                                               "scontext=u:r:init:s0")
                                                .replace("system_file", "kernarium_wifi_network_exec"), domain)), 1)
        self.assertEqual(len(runner.avc_denials(DENIAL.replace("kernarium_wifi_network", "kernarium_net_setup"),
                                                domain)), 0)
        self.assertEqual(runner.avc_denials(DENIAL.replace("denied", "granted"), domain), [])
        self.assertEqual(runner.avc_denials("kernarium_wifi_network: VirtWifi saved again\n", domain), [])

    def test_the_scripts_repair_line_is_found(self) -> None:
        self.assertTrue(runner.repair_logged(
            "11-11 09:20:00.000 I kernarium_wifi_network: VirtWifi saved again: Wi-Fi is on and nothing joined\n"))
        self.assertFalse(runner.repair_logged("--------- beginning of main\n"))

    def test_the_metered_setting(self) -> None:
        self.assertEqual(runner.metered_override(DISABLED_DUMP), 1)
        self.assertEqual(runner.metered_override(ENABLED_DUMP), 0)
        self.assertIsNone(runner.metered_override("nothing\n"))

    def test_the_overlay_is_enabled_only_when_marked(self) -> None:
        listing = ("com.android.wifi.resources\n[x] com.android.wifi.resources.kernarium\n\n"
                   "com.android.intentresolver\n--- com.android.intentresolver.auto_generated_rro_product__\n")
        self.assertTrue(runner.overlay_enabled(listing))
        self.assertFalse(runner.overlay_enabled(listing.replace("[x]", "[ ]")))
        self.assertFalse(runner.overlay_enabled("com.android.wifi.resources\n--- other\n"))


class ScriptAndOverlayTests(unittest.TestCase):
    def test_the_settle_wait_comes_from_the_tracked_script(self) -> None:
        script = runner.BOOT_SCRIPT.read_text(encoding="utf-8")
        settle = runner.boot_settle_seconds(script)
        self.assertGreater(settle, 0)
        self.assertLessEqual(settle, 120)
        waits = runner.Waits(settle, runner.REJOIN_SECONDS)
        self.assertGreater(waits.quiet, settle)
        self.assertGreater(waits.repair, settle + runner.REJOIN_SECONDS)
        with self.assertRaises(runner.Failure):
            runner.boot_settle_seconds("marker=1\n")
        # The script has no rejoin wait any more: nothing sets the marker after a save.
        self.assertFalse(hasattr(runner, "boot_rejoin_seconds"))
        self.assertNotRegex(script, r"(?m)^rejoin=")

    def test_the_runner_reads_the_integers_of_a_compiled_overlay(self) -> None:
        # A synthetic resource table with the tracked overlay's resource names, read back through the
        # verifier's reader; the tracked overlay's own values are checked by test_builder_lane.
        verifier = runner.load_verifier()
        table = synthetic_resource_table(
            [("config_fixture", [])],
            integers=[(name, verifier.WIFI_NEVER_DISABLED_THRESHOLD) for name in verifier.WIFI_NO_INTERNET_THRESHOLDS])
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as apk:
            apk.writestr("resources.arsc", table)
        self.assertEqual(runner.overlay_integers(archive.getvalue()),
                         {name: [2**31 - 1] for name in verifier.WIFI_NO_INTERNET_THRESHOLDS})

    def test_the_injected_failures_name_the_probes_and_a_private_dns_name(self) -> None:
        self.assertEqual(runner.PROBE_SETTINGS["private_dns_mode"], "hostname")
        self.assertTrue(runner.PROBE_SETTINGS["private_dns_specifier"].endswith(".invalid"))
        for key in ("captive_portal_https_url", "captive_portal_http_url", "captive_portal_fallback_url"):
            self.assertIn(runner.BLACKHOLE, runner.PROBE_SETTINGS[key])


class UnprobedValidationTests(unittest.TestCase):
    """A platform that counts no failed internet check against the network (this image validates Wi-Fi first, so a
    failed check leaves it validated; an earlier one validated without probes) must say in its validation log that
    the injected failures reached it. The statement that Wi-Fi is validated without probes is the previous
    release's alone: on the image under test it is a failure of its own."""

    class Guest(runner.Guest):
        # A guest of the image under test has its log captured; a previous release's has none (the class default).
        def __init__(self, dump: str, network_stack: str, connectivity: str, under_test: bool = False) -> None:
            self.texts = (dump, network_stack, connectivity)
            if under_test:
                self.capture = mock.Mock()

        def inject_failing_checks(self) -> None:
            pass

        def forget_and_save(self) -> None:
            pass

        def wait_joined(self, seconds: float, what: str) -> None:
            pass

        def joined(self) -> bool:
            return True

        def alive(self) -> None:
            pass

        def dump(self) -> str:
            return self.texts[0]

        def network_stack_dump(self) -> str:
            return self.texts[1]

        def connectivity_dump(self) -> str:
            return self.texts[2]

    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)

    def test_what_the_log_says_of_the_injected_failures_is_read(self) -> None:
        for image_under_test in (True, False):
            with self.subTest(image_under_test=image_under_test):
                self.assertEqual(runner.failure_reached_platform(CHECK_FAILED_STACK, image_under_test),
                                 "its internet check failed and the network stays validated")
                self.assertEqual(runner.failure_reached_platform(PRIVATE_DNS_STACK, image_under_test),
                                 "it evaluated the private DNS name")
                self.assertIsNone(runner.failure_reached_platform(SILENT_STACK, image_under_test))

    def test_the_unprobed_statement_is_the_previous_releases_alone(self) -> None:
        self.assertEqual(runner.failure_reached_platform(UNPROBED_STACK, False), "it validates Wi-Fi without probes")
        self.assertIsNone(runner.failure_reached_platform(UNPROBED_STACK, True))

    def test_a_guest_is_the_image_under_test_when_its_log_is_captured(self) -> None:
        self.assertTrue(self.Guest("", "", "", under_test=True).image_under_test)
        self.assertFalse(self.Guest("", "", "").image_under_test)

    def test_the_validated_network_is_read(self) -> None:
        self.assertTrue(runner.wifi_validated(SCORED_CONNECTIVITY))
        self.assertFalse(runner.wifi_validated(SCORED_CONNECTIVITY_NOT_VALIDATED))
        self.assertFalse(runner.wifi_validated("  NetworkAgentInfo{network{101} ni{MOBILE CONNECTED} Score(IS_VALIDATED)\n"))

    def test_counted_failures_pass_as_before(self) -> None:
        for under_test in (True, False):
            with self.subTest(under_test=under_test):
                guest = self.Guest(ENABLED_DUMP, "", "", under_test)
                self.assertIn("5 failed internet checks counted", runner.check_not_disabled(guest))

    def test_nothing_counted_passes_when_the_log_shows_the_failure_reached_the_platform(self) -> None:
        for under_test in (True, False):
            for stack, said in ((CHECK_FAILED_STACK, "its internet check failed"),
                                (PRIVATE_DNS_STACK, "it evaluated the private DNS name")):
                with self.subTest(said=said, under_test=under_test):
                    guest = self.Guest(NOTHING_COUNTED, stack, SCORED_CONNECTIVITY, under_test)
                    self.assertIn(said, runner.check_not_disabled(guest))

    def test_the_unprobed_statement_on_a_previous_release_passes(self) -> None:
        guest = self.Guest(NOTHING_COUNTED, UNPROBED_STACK, SCORED_CONNECTIVITY)
        self.assertIn("it validates Wi-Fi without probes", runner.check_not_disabled(guest))

    def test_the_unprobed_statement_on_the_image_under_test_fails(self) -> None:
        # The old patch is in the image: nothing counted and a counted failure both fail on it.
        for dump in (NOTHING_COUNTED, ENABLED_DUMP):
            with self.subTest(counted=runner.no_internet_count(dump)):
                guest = self.Guest(dump, UNPROBED_STACK, SCORED_CONNECTIVITY, under_test=True)
                with self.assertRaisesRegex(runner.Failure, "patch that skips the probes is still in the image"):
                    runner.check_not_disabled(guest)
        # Even when the log also shows the failed check.
        guest = self.Guest(NOTHING_COUNTED, UNPROBED_STACK + CHECK_FAILED_STACK, SCORED_CONNECTIVITY, under_test=True)
        with self.assertRaisesRegex(runner.Failure, "patch that skips the probes is still in the image"):
            runner.check_not_disabled(guest)

    def test_nothing_counted_and_a_log_that_says_nothing_of_the_failures_is_the_vacuity_failure(self) -> None:
        for under_test in (True, False):
            with self.subTest(under_test=under_test):
                guest = self.Guest(NOTHING_COUNTED, SILENT_STACK, SCORED_CONNECTIVITY, under_test)
                with self.assertRaisesRegex(runner.Failure, "does not say that it handled the injected failures"):
                    runner.check_not_disabled(guest)

    def test_a_network_that_is_not_validated_is_reported_not_failed(self) -> None:
        # The injected strict private DNS name does not resolve, so the platform reports the network as not validated.
        guest = self.Guest(NOTHING_COUNTED, PRIVATE_DNS_STACK, SCORED_CONNECTIVITY_NOT_VALIDATED)
        self.assertIn("not validated", runner.check_not_disabled(guest))
        guest = self.Guest(NOTHING_COUNTED, PRIVATE_DNS_STACK, SCORED_CONNECTIVITY)
        self.assertRegex(runner.check_not_disabled(guest), r"joined, validated$")

    def test_a_disabled_network_still_fails(self) -> None:
        guest = self.Guest(DISABLED_DUMP, CHECK_FAILED_STACK, SCORED_CONNECTIVITY)
        with self.assertRaisesRegex(runner.Failure, "became NETWORK_SELECTION_PERMANENTLY_DISABLED"):
            runner.check_not_disabled(guest)


class UpgradeCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def test_a_repair_line_that_rotates_out_before_the_join_is_kept(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=["", "", REPAIR_LINE, ""],
                              status=[NOT_JOINED, NOT_JOINED, NOT_JOINED, JOINED], log=[""])
        self.assertIsNone(runner.check_repaired(guest, WAITS))

    def test_a_join_without_the_repair_line_fails(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=[""], status=[NOT_JOINED, JOINED],
                              log=[""])
        with self.assertRaisesRegex(runner.Failure, "without the boot script saving it again"):
            runner.check_repaired(guest, WAITS)

    def test_an_enabled_disk_without_the_repair_line_fails_at_once(self) -> None:
        guest = ScriptedGuest(dump=[ENABLED_DUMP], script_log=[""], status=[JOINED], log=[""])
        with self.assertRaisesRegex(runner.Failure, "not disabled"):
            runner.check_repaired(guest, WAITS)

    def test_a_denial_read_at_boot_fails_the_later_check(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=[REPAIR_LINE],
                              status=[NOT_JOINED, JOINED], log=[DENIAL, ""])
        runner.check_repaired(guest, WAITS)
        with self.assertRaisesRegex(runner.Failure, "denied the boot script's domain 1 times"):
            runner.check_no_denials(guest)

    def test_a_denial_read_when_the_repair_line_shows_fails_the_later_check(self) -> None:
        guest = ScriptedGuest(dump=[DISABLED_DUMP, ENABLED_DUMP], script_log=["", REPAIR_LINE],
                              status=[NOT_JOINED, NOT_JOINED, JOINED], log=["", DENIAL, ""])
        runner.check_repaired(guest, WAITS)
        with self.assertRaisesRegex(runner.Failure, "denied the boot script's domain 1 times"):
            runner.check_no_denials(guest)

    def test_a_log_without_the_scripts_denials_passes(self) -> None:
        guest = ScriptedGuest(log=[DENIAL.replace("kernarium_wifi_network", "kernarium_net_setup")])
        self.assertIn(f"none for {runner.SCRIPT_DOMAIN}", runner.check_no_denials(guest))

    def test_a_station_that_joins_after_the_previous_release_restarts_fails(self) -> None:
        # The join comes at the last poll of the watch.
        guest = ScriptedGuest(status=[NOT_JOINED] * (runner.BOOT_JOIN_SECONDS // runner.POLL_SECONDS) + [JOINED],
                              dump=[DISABLED_DUMP])
        with self.assertRaisesRegex(runner.Failure, "disk holds the network enabled"):
            runner.check_disabled_on_disk(guest)
        self.assertEqual(guest.restarts, 1)

    def test_a_disk_that_holds_the_network_disabled_passes(self) -> None:
        guest = ScriptedGuest(status=[NOT_JOINED], dump=[DISABLED_DUMP])
        runner.check_disabled_on_disk(guest)
        self.assertEqual(guest.restarts, 1)

    def test_a_network_enabled_after_the_previous_release_restarts_fails(self) -> None:
        guest = ScriptedGuest(status=[NOT_JOINED], dump=[ENABLED_DUMP])
        with self.assertRaisesRegex(runner.Failure, "did not reach the disk"):
            runner.check_disabled_on_disk(guest)


class WarningReadingTests(unittest.TestCase):
    def test_a_wifi_network_is_read_with_its_capabilities(self) -> None:
        networks = runner.wifi_networks(CONNECTIVITY)
        self.assertEqual(sorted(networks), [101, 102])
        self.assertNotIn("VALIDATED", networks[101])
        self.assertIn("VALIDATED", networks[102])
        self.assertEqual(runner.newest_wifi_network(CONNECTIVITY), 102)
        self.assertIsNone(runner.newest_wifi_network("Current networks:\n"))

    def test_a_network_of_another_transport_is_not_a_wifi_network(self) -> None:
        self.assertEqual(runner.wifi_networks(agent(103, VALIDATED, transport="CELLULAR")), {})

    def test_a_network_that_must_stay_validated(self) -> None:
        self.assertEqual(runner.validation_problems(frozenset(VALIDATED.split("&"))), [])
        self.assertEqual(runner.validation_problems(None), ["dumpsys connectivity lists no Wi-Fi network"])
        self.assertEqual(runner.validation_problems(frozenset(STOCK.split("&"))), ["the network is not validated"])
        for name in ("PARTIAL_CONNECTIVITY", "CAPTIVE_PORTAL", "PRIVATE_DNS_BROKEN"):
            problems = runner.validation_problems(frozenset(VALIDATED.split("&") + [name]))
            self.assertEqual(problems, [f"the network has {name}"])

    def test_the_validation_log_is_kept_for_each_network(self) -> None:
        logs = runner.validation_logs(NETWORK_STACK)
        self.assertEqual(sorted(logs), [101, 102])
        self.assertEqual(len(logs[102]), 4)
        self.assertEqual(logs[102][0], "Wi-Fi is validated before its internet check")
        self.assertTrue(logs[101][0].startswith("PROBE_DNS"))
        # What the dump prints before and after the section is not read.
        self.assertEqual(runner.validation_logs("IpClient.wlan0\n  2026-11-10T13:30:32.803492 - x\n"), {})
        after = NETWORK_STACK + "Other section:\n  2026-11-10T13:31:00.000000 - not a validation line\n"
        self.assertEqual(runner.validation_logs(after), logs)

    def test_the_checks_result_is_the_line_the_log_holds(self) -> None:
        messages = runner.validation_logs(NETWORK_STACK)[102]
        self.assertEqual(runner.check_log_problems(messages, runner.CHECK_FAILED_LINE, probes_failed=True), [])
        self.assertEqual(runner.check_log_problems(messages, runner.CHECK_FAILED_LINE), [])
        self.assertEqual(runner.check_log_problems(messages, runner.CHECK_PASSED_LINE),
                         [f"the validation log has no line {runner.CHECK_PASSED_LINE!r}"])
        self.assertEqual(runner.check_log_problems(messages[:1], runner.CHECK_FAILED_LINE, probes_failed=True),
                         [f"the validation log has no line {runner.CHECK_FAILED_LINE!r}",
                          "the validation log shows no failed PROBE_HTTP",
                          "the validation log shows no failed PROBE_HTTPS"])

    def test_a_notification_of_connectivity_service_is_found_by_its_tag(self) -> None:
        self.assertEqual(runner.connectivity_notifications(QUIET_NOTIFICATIONS), [])
        found = runner.connectivity_notifications(SIGN_IN_NOTIFICATIONS)
        self.assertEqual(len(found), 1)
        self.assertIn("ConnectivityNotification:102", found[0])

    def test_the_captive_portal_login_activity_is_found_by_its_package(self) -> None:
        self.assertEqual(runner.captive_portal_activities(QUIET_ACTIVITIES), [])
        self.assertEqual(len(runner.captive_portal_activities(PORTAL_ACTIVITIES)), 2)

    def test_the_marks_of_the_screen(self) -> None:
        warning = runner.ui_nodes(WARNING_SCREEN)
        clear = runner.ui_nodes(CLEAR_SCREEN)
        self.assertTrue(runner.summary_limited(warning))
        self.assertTrue(runner.status_bar_no_internet(warning))
        self.assertFalse(runner.summary_limited(clear))
        self.assertTrue(runner.summary_connected(clear))
        self.assertFalse(runner.status_bar_no_internet(clear))
        self.assertEqual(runner.mark_problems(warning, shown=True), [])
        self.assertEqual(runner.mark_problems(clear, shown=False), [])
        self.assertEqual(len(runner.mark_problems(clear, shown=True)), 2)
        self.assertEqual(len(runner.mark_problems(warning, shown=False)), 3)

    def test_a_no_internet_description_outside_the_status_bar_is_not_the_mark(self) -> None:
        elsewhere = runner.ui_nodes(WARNING_SCREEN.replace('package="com.android.systemui"',
                                                           'package="com.android.settings"'))
        self.assertFalse(runner.status_bar_no_internet(elsewhere))
        self.assertEqual(runner.mark_problems(elsewhere, shown=True),
                         [f"the status bar's Wi-Fi icon does not say {runner.NO_INTERNET!r}"])

    def test_a_dump_that_is_not_xml_fails(self) -> None:
        for text in ("", "uiautomator: not found", "<displays><node"):
            with self.assertRaisesRegex(runner.Failure, "not XML"):
                runner.ui_nodes(text)


class ProbeServerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.server = runner.ProbeServer()
        self.addCleanup(self.server.close)

    def get(self, path: str = "/generate_204") -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=10)
        try:
            connection.request("GET", path)
            response = connection.getresponse()
            return response.status, {key.lower(): value for key, value in response.getheaders()}, response.read()
        finally:
            connection.close()

    def test_the_expected_answer_is_a_204_without_a_body(self) -> None:
        self.server.mode = "expected"
        status, headers, body = self.get()
        self.assertEqual((status, body), (204, b""))

    def test_a_sign_in_page_is_a_redirect_to_its_own_login(self) -> None:
        self.server.mode = "redirect"
        status, headers, body = self.get("/gen_204")
        self.assertEqual(status, 302)
        self.assertEqual(headers["location"], f"http://127.0.0.1:{self.server.port}/login")
        self.assertEqual(body, b"")

    def test_a_block_page_is_a_200_answer_of_two_kibibytes(self) -> None:
        self.server.mode = "page"
        status, headers, body = self.get()
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/html")
        self.assertEqual(int(headers["content-length"]), runner.PAGE_BYTES)
        self.assertEqual(len(body), runner.PAGE_BYTES)
        self.assertTrue(body.startswith(b"<html>") and body.endswith(b"</html>"))

    def test_the_paths_the_probes_asked_for_are_kept_until_cleared(self) -> None:
        self.get("/generate_204")
        self.get("/gen_204")
        self.assertEqual(self.server.probes(), ["/generate_204", "/gen_204"])
        self.server.clear()
        self.assertEqual(self.server.probes(), [])

    def test_the_server_listens_on_the_loopback_alone(self) -> None:
        self.assertEqual(self.server.httpd.server_address[0], "127.0.0.1")
        self.assertGreater(self.server.port, 1023)

    # The held answer: the first probe of a scenario is at the server and unanswered until it is released.
    def held_get(self, path: str) -> tuple[threading.Thread, list[int]]:
        statuses: list[int] = []
        thread = threading.Thread(target=lambda: statuses.append(self.get(path)[0]), daemon=True)
        thread.start()
        return thread, statuses

    def wait_until(self, condition) -> None:
        deadline = time.monotonic() + 10
        while not condition() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(condition())

    def test_the_first_probe_of_a_held_scenario_is_unanswered_until_it_is_released(self) -> None:
        self.server.mode = "expected"
        self.server.hold_first("/s1")
        thread, statuses = self.held_get("/s1/generate_204")
        self.wait_until(lambda: self.server.is_open("/s1"))
        self.assertEqual(len(self.server.checks("/s1")), 1)
        time.sleep(0.3)
        self.assertEqual(statuses, [])
        self.server.release()
        thread.join(10)
        self.assertEqual(statuses, [204])
        self.assertFalse(self.server.is_open("/s1"))

    def test_only_the_first_probe_of_the_held_scenario_is_held(self) -> None:
        self.server.hold_first("/s1")
        self.assertEqual(self.get("/s2/generate_204")[0], 204)
        self.assertEqual(self.get("/s1/gen_204")[0], 204)
        self.assertFalse(self.server.is_open("/s1"))
        thread, statuses = self.held_get("/s1/generate_204")
        self.wait_until(lambda: self.server.is_open("/s1"))
        self.assertFalse(self.server.is_open("/s2"))
        self.server.release()
        thread.join(10)
        self.assertEqual(self.get("/s1/generate_204")[0], 204)
        self.assertEqual(statuses, [204])

    def test_a_held_answer_goes_out_by_itself_when_the_hold_is_over(self) -> None:
        with mock.patch.object(runner, "HELD_ANSWER_SECONDS", 0.3):
            self.server.hold_first("/s1")
            started = time.monotonic()
            status = self.get("/s1/generate_204")[0]
        self.assertEqual(status, 204)
        self.assertGreaterEqual(time.monotonic() - started, 0.25)
        self.assertFalse(self.server.is_open("/s1"))

    def test_the_hold_is_read_as_one_state_that_only_moves_forward(self) -> None:
        self.assertEqual(self.server.hold_state("/s1"), "idle")
        self.server.hold_first("/s1")
        self.assertEqual(self.server.hold_state("/s1"), "armed")
        self.assertEqual(self.server.hold_state("/s2"), "idle")
        self.assertFalse(self.server.arrive("/s2/generate_204", 204))
        self.assertFalse(self.server.arrive("/s1/gen_204", 204))
        self.assertEqual(self.server.hold_state("/s1"), "armed")
        self.assertTrue(self.server.arrive("/s1/generate_204", 204))
        self.assertEqual(self.server.hold_state("/s1"), "open")
        self.assertFalse(self.server.arrive("/s1/generate_204", 204))
        self.server.release()
        self.assertEqual(self.server.hold_state("/s1"), "released")
        self.assertFalse(self.server.arrive("/s1/generate_204", 204))

    def test_a_hold_that_is_released_before_its_request_arrives_holds_nothing(self) -> None:
        self.server.hold_first("/s1")
        self.server.release()
        self.assertFalse(self.server.arrive("/s1/generate_204", 204))
        self.assertEqual(self.server.hold_state("/s1"), "released")

    def test_a_request_is_recorded_and_opened_under_one_acquisition_of_the_lock(self) -> None:
        acquisitions: list[int] = []
        real = self.server.lock

        class CountingLock:
            def __enter__(self) -> None:
                acquisitions.append(1)
                real.acquire()

            def __exit__(self, *arguments: object) -> None:
                real.release()

        self.server.lock = CountingLock()
        self.server.hold_first("/s1")
        acquisitions.clear()
        self.assertTrue(self.server.arrive("/s1/generate_204", 204))
        self.assertEqual(len(acquisitions), 1)
        self.server.lock = real
        self.assertEqual(len(self.server.checks("/s1")), 1)
        self.assertTrue(self.server.is_open("/s1"))

    def test_the_handler_shows_no_request_that_is_recorded_and_not_yet_open(self) -> None:
        # The loopback interleaving: the handler is paused at the moment the request arrives. Nothing
        # is recorded and nothing is open then; once it goes on, the request is recorded and open together.
        arriving, go = threading.Event(), threading.Event()
        original = self.server.arrive

        def paused(path: str, status: int = 0, when: float | None = None) -> bool:
            arriving.set()
            self.assertTrue(go.wait(10))
            return original(path, status, when)

        self.server.arrive = paused
        self.server.hold_first("/sprobe")
        thread, statuses = self.held_get("/sprobe/generate_204")
        try:
            self.assertTrue(arriving.wait(10))
            self.assertEqual(self.server.checks("/sprobe"), [])
            self.assertEqual(self.server.hold_state("/sprobe"), "armed")
        finally:
            go.set()
        self.wait_until(lambda: self.server.is_open("/sprobe"))
        self.assertEqual(len(self.server.checks("/sprobe")), 1)
        self.assertEqual(statuses, [])
        self.server.release()
        thread.join(10)
        self.assertEqual(statuses, [204])

    def test_the_hold_ends_below_the_probes_own_timeout(self) -> None:
        # NetworkMonitor.SOCKET_TIMEOUT_MS is 10 s: the platform's HTTP probe gives up then.
        self.assertEqual(runner.PROBE_SOCKET_TIMEOUT_SECONDS, 10.0)
        self.assertLessEqual(runner.HELD_ANSWER_SECONDS + 2, runner.PROBE_SOCKET_TIMEOUT_SECONDS)

    def test_clearing_the_server_forgets_a_hold(self) -> None:
        self.server.hold_first("/s1")
        self.server.clear()
        self.assertEqual(self.get("/s1/generate_204")[0], 204)


class WarningSectionTests(unittest.TestCase):
    """The sections that show a failed check as a warning, against a guest that answers from a script."""

    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def warned(self, **changes: list[str]) -> ScriptedGuest:
        answers = {"connectivity": [CONNECTIVITY], "network_stack": [NETWORK_STACK],
                   "notifications": [QUIET_NOTIFICATIONS], "activities": [QUIET_ACTIVITIES],
                   "ui": [WARNING_SCREEN], "status": [JOINED], "dump": [ENABLED_DUMP]}
        answers.update(changes)
        return ScriptedGuest(**answers)

    def test_a_failed_check_that_shows_only_the_warning_has_no_problem(self) -> None:
        self.assertEqual(runner.warning_problems(self.warned(), probes_failed=True), [])

    def test_a_network_that_is_not_validated_is_a_problem(self) -> None:
        guest = self.warned(connectivity=[agent(102, STOCK)])
        self.assertEqual(runner.warning_problems(guest, probes_failed=True), ["the network is not validated"])

    def test_a_sign_in_state_is_a_problem(self) -> None:
        portal = agent(102, VALIDATED.replace("VALIDATED", "CAPTIVE_PORTAL&VALIDATED"))
        problems = runner.warning_problems(self.warned(connectivity=[portal]), probes_failed=True)
        self.assertEqual(problems, ["the network has CAPTIVE_PORTAL"])
        problems = runner.warning_problems(self.warned(notifications=[SIGN_IN_NOTIFICATIONS]), probes_failed=True)
        self.assertEqual(len(problems), 1)
        self.assertIn("a notification of ConnectivityService", problems[0])
        problems = runner.warning_problems(self.warned(activities=[PORTAL_ACTIVITIES]), probes_failed=True)
        self.assertEqual(len(problems), 2)
        self.assertIn("the captive portal login activity", problems[0])

    def test_a_check_that_the_log_does_not_report_is_a_problem(self) -> None:
        guest = self.warned(network_stack=[PASSED_NETWORK_STACK])
        problems = runner.warning_problems(guest, probes_failed=True)
        self.assertEqual(problems, [f"the validation log has no line {runner.CHECK_FAILED_LINE!r}"])

    def test_a_page_answer_needs_no_failed_probe_in_the_log(self) -> None:
        page_log = NETWORK_STACK.replace("Probe failed with exception", "time=3ms ret=200")
        self.assertEqual(runner.warning_problems(self.warned(network_stack=[page_log]), probes_failed=False), [])
        self.assertEqual(len(runner.warning_problems(self.warned(network_stack=[page_log]), probes_failed=True)), 2)

    def test_a_screen_without_the_marks_is_a_problem(self) -> None:
        problems = runner.warning_problems(self.warned(ui=[CLEAR_SCREEN]), probes_failed=True)
        self.assertEqual(len(problems), 2)

    def test_a_cleared_warning_has_the_passing_line_and_no_mark(self) -> None:
        guest = self.warned(network_stack=[PASSED_NETWORK_STACK], ui=[CLEAR_SCREEN])
        self.assertEqual(runner.cleared_problems(guest), [])
        still = self.warned(network_stack=[PASSED_NETWORK_STACK])
        self.assertEqual(len(runner.cleared_problems(still)), 3)

    def test_the_warning_that_shows_late_is_waited_for(self) -> None:
        guest = self.warned(ui=[CLEAR_SCREEN, CLEAR_SCREEN, WARNING_SCREEN])
        runner.wait_problems("the warning", 60, guest, lambda: runner.warning_problems(guest, True))

    def test_a_warning_that_never_shows_fails_after_its_deadline(self) -> None:
        guest = self.warned(ui=[CLEAR_SCREEN])
        with self.assertRaisesRegex(runner.Failure, r"the warning: .*does not say 'Limited connection'.*\(after 60 s\)"):
            runner.wait_problems("the warning", 60, guest, lambda: runner.warning_problems(guest, True))

    def test_the_hold_passes_for_a_network_that_stays_as_it_is(self) -> None:
        runner.hold_validated(self.warned(), 20)

    def test_the_hold_fails_for_a_network_that_is_disabled_or_left_or_not_validated(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "became NETWORK_SELECTION_PERMANENTLY_DISABLED"):
            runner.hold_validated(self.warned(dump=[DISABLED_DUMP]), 20)
        with self.assertRaisesRegex(runner.Failure, "Auto-connect is off"):
            runner.hold_validated(self.warned(dump=[AUTOJOIN_OFF_DUMP]), 20)
        with self.assertRaisesRegex(runner.Failure, "station left"):
            runner.hold_validated(self.warned(status=[NOT_JOINED]), 20)
        with self.assertRaisesRegex(runner.Failure, "not validated"):
            runner.hold_validated(self.warned(connectivity=[agent(102, STOCK)]), 20)
        with self.assertRaisesRegex(runner.Failure, "ConnectivityService"):
            runner.hold_validated(self.warned(notifications=[SIGN_IN_NOTIFICATIONS]), 20)

    def stubbed(self, **changes: list[str]) -> StubbedGuest:
        answers = {"connectivity": [CONNECTIVITY], "network_stack": [NETWORK_STACK],
                   "notifications": [QUIET_NOTIFICATIONS], "activities": [QUIET_ACTIVITIES],
                   "ui": [WARNING_SCREEN], "status": [JOINED], "dump": [ENABLED_DUMP]}
        answers.update(changes)
        return StubbedGuest(**answers)

    def test_a_failed_check_section_sets_only_the_probes_and_joins_the_network_again(self) -> None:
        guest = self.stubbed()
        self.assertIn("validated, joined and enabled", runner.check_failed_check_warns(guest))
        self.assertEqual(guest.calls[0], f"delete {sorted((*runner.PRIVATE_DNS_SETTINGS, runner.USE_HTTPS_FLAG))}")
        self.assertEqual(guest.calls[1], f"put {sorted(runner.CHECK_SETTINGS)}")
        self.assertEqual(guest.calls[2:4], ["forget and save", "joined the station joins the network saved again"])
        self.assertFalse([call for call in guest.calls if call.startswith("reevaluate")])

    def test_a_failed_check_section_fails_when_the_mark_never_shows(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "shows only the warning: .*Limited connection"):
            runner.check_failed_check_warns(self.stubbed(ui=[CLEAR_SCREEN]))

    def test_the_warning_after_a_wake_waits_for_the_mark_on_the_same_network(self) -> None:
        guest = self.stubbed(ui=[CLEAR_SCREEN, WARNING_SCREEN])
        self.assertEqual(runner.check_warning_after_wake(guest),
                         "network 102 kept through the wake, validated at 5000 throughout, with the warning")
        self.assertEqual(guest.calls[0], "suspend and wake")

    def test_a_wake_that_replaces_the_network_fails_with_the_new_one_named(self) -> None:
        woken = "Current networks:\n" + agent(103, VALIDATED, validated_at=9000) + "Other state\n"
        guest = self.stubbed(network_stack=[stack_of(103)])
        guest.on_wake = lambda: guest.answers.update({ScriptedGuest.COMMANDS["connectivity"]: [woken]})
        with self.assertRaisesRegex(runner.Failure, "network 102 became network 103"):
            runner.check_warning_after_wake(guest)

    def test_a_warning_that_is_not_shown_after_the_wake_fails_the_section(self) -> None:
        with self.assertRaisesRegex(runner.Failure,
                                    "the failed check shows only the warning after the wake: .*Limited connection"):
            runner.check_warning_after_wake(self.stubbed(ui=[CLEAR_SCREEN]))

    def test_a_wake_that_leaves_no_network_listed_fails_the_section(self) -> None:
        guest = self.stubbed()
        guest.on_wake = lambda: guest.answers.update({ScriptedGuest.COMMANDS["connectivity"]: ["Current networks:\n"]})
        with self.assertRaisesRegex(runner.Failure, "network 102 is gone: ConnectivityService lists no Wi-Fi network"):
            runner.check_warning_after_wake(guest)

    def test_a_page_answer_section_needs_a_request_at_the_server(self) -> None:
        server = runner.ProbeServer()
        self.addCleanup(server.close)
        guest = self.stubbed()
        with self.assertRaisesRegex(runner.Failure,
                                    "a probe reaches the host's server: the server has seen no request of this scenario"):
            runner.check_page_answer_warns(guest, server, "redirect")
        self.assertEqual(server.mode, "redirect")
        self.assertEqual(guest.calls[0], f"reverse {server.port}")

    def test_a_page_answer_section_needs_the_current_networks_probe_to_receive_the_answer(self) -> None:
        server = runner.ProbeServer()
        self.addCleanup(server.close)
        scenario = "/sfixed01"
        probe = (f"PROBE_HTTP http://127.0.0.1:{server.port}{scenario}/generate_204 time=3ms ret=302 "
                 "request={} headers={Location=[http://127.0.0.1/login]}")
        timeout = probe.split(" time=")[0] + " Probe failed with exception java.net.SocketTimeoutException: timeout"
        stack = NETWORK_STACK.replace("  2026-11-10T13:30:44.971100 - PROBE_HTTP http://192.0.2.1/generate_204 "
                                      "Probe failed with exception java.net.SocketTimeoutException: failed to connect "
                                      "after 10000ms\n", "  2026-11-10T13:30:44.971100 - {}\n")

        class Asked(StubbedGuest):
            def forget_and_save(self) -> None:
                super().forget_and_save()
                server.record(f"{scenario}/generate_204", 302)

        def guest(line: str) -> StubbedGuest:
            return Asked(connectivity=[CONNECTIVITY], network_stack=[stack.format(line)], notifications=[QUIET_NOTIFICATIONS],
                         activities=[QUIET_ACTIVITIES], ui=[WARNING_SCREEN], status=[JOINED], dump=[ENABLED_DUMP])

        with mock.patch.object(runner.ProbeServer, "scenario", staticmethod(lambda: scenario)):
            note = runner.check_page_answer_warns(guest(probe), server, "redirect")
            self.assertEqual(note, "1 requests answered 302 and read by the network's own probe")
            # The server saw the request, and the network's probe timed out on it: no answer.
            with self.assertRaisesRegex(runner.Failure, "failed without an answer"):
                runner.check_page_answer_warns(guest(timeout), server, "redirect")

    def test_the_page_sections_delete_every_setting_and_the_reverse_even_when_one_fails(self) -> None:
        server = runner.ProbeServer()
        self.addCleanup(server.close)
        guest = self.stubbed(ui=[CLEAR_SCREEN])
        report = runner.Report()
        with contextlib.redirect_stderr(io.StringIO()):
            runner.warning_sections(guest, server, report)
        self.assertEqual(len(report.failures), 9)
        self.assertEqual(guest.calls[-2:], [f"delete {sorted(runner.ALL_CHECK_SETTINGS)}", f"unreverse {server.port}"])

    def test_the_page_settings_point_the_probes_at_the_server_and_the_black_hole(self) -> None:
        settings = runner.page_settings(41234)
        self.assertEqual(settings["captive_portal_http_url"], "http://127.0.0.1:41234/generate_204")
        self.assertEqual(settings["captive_portal_fallback_url"], "http://127.0.0.1:41234/gen_204")
        scoped = runner.page_settings(41234, "/s1a2b3c4d")
        self.assertEqual(scoped["captive_portal_http_url"], "http://127.0.0.1:41234/s1a2b3c4d/generate_204")
        self.assertEqual(scoped["captive_portal_fallback_url"], "http://127.0.0.1:41234/s1a2b3c4d/gen_204")
        self.assertIn(runner.BLACKHOLE, settings["captive_portal_https_url"])
        # The strict private DNS name fails the network itself, so no warning section sets it.
        for settings in (runner.CHECK_SETTINGS, runner.page_settings(1)):
            self.assertFalse([key for key in settings if key.startswith("private_dns")])
        self.assertEqual(set(runner.ALL_CHECK_SETTINGS), set(runner.PROBE_SETTINGS) | {runner.USE_HTTPS_FLAG})


class UpgradeSectionTests(unittest.TestCase):
    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def test_the_previous_release_option_is_given_once_for_each_release(self) -> None:
        parser = runner.build_parser()
        options = parser.parse_args(["--previous-release", "r22", "--previous-release", "r23"])
        self.assertEqual([str(path) for path in options.previous_release], ["r22", "r23"])
        self.assertIsNone(parser.parse_args([]).previous_release)

    def test_a_previous_release_with_the_overlay_never_disables_the_network(self) -> None:
        with mock.patch.object(runner, "check_overlay", return_value=None):
            self.assertTrue(runner.previous_never_disables(ScriptedGuest()))
        with mock.patch.object(runner, "check_overlay", side_effect=runner.Failure("the overlay is not enabled")):
            self.assertFalse(runner.previous_never_disables(ScriptedGuest()))

    def test_a_disk_the_release_leaves_alone_keeps_its_network_and_its_metered_setting(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              script_service=["stopped"], listing=[LISTING], script_log=[""])
        self.assertIsNone(runner.check_left_alone(guest, WAITS))

    def test_a_boot_script_that_never_ran_fails(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              script_service=[""], listing=[LISTING], script_log=[""])
        with self.assertRaisesRegex(runner.Failure, "service is 'not started', not 'stopped'"):
            runner.check_left_alone(guest, WAITS)

    def test_a_boot_script_that_is_still_waiting_fails_after_the_repair_wait(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              script_service=["running"], listing=[LISTING], script_log=[""])
        with self.assertRaisesRegex(runner.Failure, r"service is 'running', not 'stopped'.*after 255 s"):
            runner.check_left_alone(guest, WAITS)

    def test_a_boot_script_that_stopped_with_the_network_unsaved_or_unjoined_fails(self) -> None:
        for listing, status, message in ((["Network Id      SSID                         Security type\n"], JOINED, "not saved"),
                                         ([LISTING], NOT_JOINED, "has not joined")):
            guest = ScriptedGuest(status=[status], dump=[ENABLED_DUMP], script_service=["stopped"], listing=listing)
            self.assertIn(message, runner.boot_script_finished(guest) or "")

    def test_a_network_saved_again_loses_its_metered_setting(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP], script_service=["stopped"], listing=[LISTING],
                              script_log=[""])
        with self.assertRaisesRegex(runner.Failure, "reset its metered setting"):
            runner.check_left_alone(guest, WAITS)

    def test_a_repair_line_on_a_disk_that_needs_none_fails(self) -> None:
        guest = ScriptedGuest(status=[JOINED], dump=[ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")],
                              script_service=["stopped"], listing=[LISTING], script_log=[REPAIR_LINE])
        with self.assertRaisesRegex(runner.Failure, "saved the network again although the station had joined"):
            runner.check_left_alone(guest, WAITS)

    def autojoin_guest(self, dumps: list[str], script_log: list[str], **answers: list[str]) -> StubbedGuest:
        answers = {"listing": [LISTING], "script_service": ["stopped"], **answers}
        guest = StubbedGuest(dump=dumps, script_log=script_log, **answers)
        guest.answers[f"cmd wifi add-network {runner.SSID} open -d"] = [""]
        return guest

    def test_auto_connect_off_that_a_boot_at_marker_1_leaves_alone_passes_when_the_line_shows(self) -> None:
        guest = self.autojoin_guest([NETWORK_OFF], ["", "", LEFT_ALONE])
        self.assertIsNone(runner.check_autojoin_kept(guest, WAITS, left_alone=True))
        self.assertEqual(guest.restarts, 1)

    def test_a_left_alone_line_that_rotated_out_of_the_log_is_kept_once_seen(self) -> None:
        guest = self.autojoin_guest([NETWORK_OFF], ["", LEFT_ALONE, ""])
        self.assertIsNone(runner.check_autojoin_kept(guest, WAITS, left_alone=True))

    def test_a_boot_that_does_not_say_it_left_the_network_alone_fails_when_the_line_is_required(self) -> None:
        guest = self.autojoin_guest([NETWORK_OFF], [""])
        with self.assertRaisesRegex(runner.Failure, "did not say that it left the network alone"):
            runner.check_autojoin_kept(guest, WAITS, left_alone=True)

    def test_a_boot_at_marker_2_logs_nothing_and_a_line_there_fails(self) -> None:
        self.assertIsNone(runner.check_autojoin_kept(self.autojoin_guest([NETWORK_OFF], [""]), WAITS,
                                                      left_alone=False))
        guest = self.autojoin_guest([NETWORK_OFF], [LEFT_ALONE])
        with self.assertRaisesRegex(runner.Failure, "only a boot without marker 2 does"):
            runner.check_autojoin_kept(guest, WAITS, left_alone=False)

    def test_either_is_accepted_when_the_marker_is_not_known(self) -> None:
        for log in ("", LEFT_ALONE):
            guest = self.autojoin_guest([NETWORK_OFF], [log])
            self.assertIsNone(runner.check_autojoin_kept(guest, WAITS))

    def test_auto_connect_on_again_after_the_restart_fails_whatever_the_log_says(self) -> None:
        for log in ("", LEFT_ALONE, REPAIR_LINE):
            guest = self.autojoin_guest([NETWORK_OFF, NETWORK_ON], [log])
            with self.assertRaisesRegex(runner.Failure, "Auto-connect is on again after a restart"):
                runner.check_autojoin_kept(guest, WAITS, left_alone=True)

    def test_a_repair_line_fails_even_when_it_rotates_out_and_auto_connect_reads_off(self) -> None:
        guest = self.autojoin_guest([NETWORK_OFF], [REPAIR_LINE, LEFT_ALONE, ""])
        with self.assertRaisesRegex(runner.Failure, "saved the network again although the user turned Auto-connect"):
            runner.check_autojoin_kept(guest, WAITS, left_alone=True)

    def test_a_dump_that_shows_no_auto_connect_for_the_network_after_the_restart_fails_in_every_mode(self) -> None:
        # An empty dump, or one with the values of another configuration only, is no evidence of a setting kept.
        for after in ("", ENABLED_DUMP, OTHER_HEADER + ENABLED_DUMP, "Permission Denial: can't dump WifiService\n"):
            for left_alone in (True, False, None):
                with self.subTest(after=after[:30], left_alone=left_alone):
                    guest = self.autojoin_guest([NETWORK_OFF, after], [LEFT_ALONE])
                    with self.assertRaisesRegex(runner.Failure, "shows no Auto-connect for VirtWifi's configuration"):
                        runner.check_autojoin_kept(guest, WAITS, left_alone=left_alone)

    def test_another_configurations_value_is_not_the_networks(self) -> None:
        # VirtWifi (ID 0) is off; another configuration of the same name (ID 1) is on, before or after it.
        for after in (NETWORK_OFF + OTHER_HEADER + ENABLED_DUMP, OTHER_HEADER + ENABLED_DUMP + NETWORK_OFF,
                      NETWORK_OFF + ", 1=" + OTHER_HEADER + ENABLED_DUMP):
            guest = self.autojoin_guest([NETWORK_OFF, after], [LEFT_ALONE])
            self.assertIsNone(runner.check_autojoin_kept(guest, WAITS, left_alone=True))
        self.assertEqual(runner.configuration_autojoin(NETWORK_OFF + OTHER_HEADER + ENABLED_DUMP, 0), [False])
        self.assertEqual(runner.configuration_autojoin(NETWORK_OFF + OTHER_HEADER + ENABLED_DUMP, 1), [True])
        self.assertEqual(runner.configuration_autojoin(NETWORK_OFF * 2, 0), [False, False])
        self.assertEqual(runner.configuration_autojoin(AUTOJOIN_OFF_DUMP, 0), [])

    def test_a_boot_script_that_did_not_end_on_this_boot_fails_in_every_mode(self) -> None:
        for state in ("running", ""):
            for left_alone in (True, False, None):
                with self.subTest(state=state, left_alone=left_alone):
                    guest = self.autojoin_guest([NETWORK_OFF], [LEFT_ALONE], script_service=[state])
                    with self.assertRaisesRegex(runner.Failure, rf"service is {state or 'not started'!r}, not 'stopped'"):
                        runner.check_autojoin_kept(guest, WAITS, left_alone=left_alone)

    def test_a_script_that_ends_late_is_waited_for(self) -> None:
        guest = self.autojoin_guest([NETWORK_OFF], [LEFT_ALONE], script_service=["running", "running", "stopped"])
        self.assertIsNone(runner.check_autojoin_kept(guest, WAITS, left_alone=True))

    def test_a_network_that_is_not_saved_after_the_restart_fails(self) -> None:
        guest = self.autojoin_guest([NETWORK_OFF], [LEFT_ALONE],
                                    listing=[LISTING, "Network Id      SSID                         Security type\n"])
        with self.assertRaisesRegex(runner.Failure, "VirtWifi is not saved after the restart"):
            runner.check_autojoin_kept(guest, WAITS, left_alone=True)

    def test_the_values_of_each_dump_the_service_printed_are_read_under_the_networks_own_id(self) -> None:
        for name, (number, on) in sorted(wifi_dump_fixtures.STATES.items()):
            with self.subTest(name):
                values = runner.configuration_autojoin(wifi_dump_fixtures.DUMPS[name], number)
                self.assertTrue(values)
                self.assertEqual(values, [on] * len(values))
        # Other configurations have their own, and a line that holds a header's text names no configuration.
        two = wifi_dump_fixtures.DUMPS["unconnected-off-two-more"]
        self.assertEqual([runner.configuration_autojoin(two, number) for number in (0, 1, 2, 7)],
                         [[True] * 4, [True] * 4, [False] * 4, []])
        decoy = wifi_dump_fixtures.DUMPS["synthetic-decoy-in-block"]
        self.assertEqual([runner.configuration_autojoin(decoy, number) for number in (0, 7)], [[False] * 4, []])

    def test_the_step_reads_the_dumps_the_service_printed_after_the_restart(self) -> None:
        before = wifi_dump_fixtures.DUMPS["connected-off"]
        for name, (number, on) in sorted(wifi_dump_fixtures.STATES.items()):
            row = f"Network Id      SSID                         Security type\n{number}            VirtWifi    open\n"
            guest = self.autojoin_guest([before, wifi_dump_fixtures.DUMPS[name]], [LEFT_ALONE], listing=[LISTING, row])
            with self.subTest(name):
                if on:
                    with self.assertRaisesRegex(runner.Failure, "Auto-connect is on again after a restart"):
                        runner.check_autojoin_kept(guest, WAITS, left_alone=True)
                else:
                    self.assertIsNone(runner.check_autojoin_kept(guest, WAITS, left_alone=True))

    def test_the_left_alone_line_is_read_as_what_it_says(self) -> None:
        self.assertTrue(runner.left_alone_logged(LEFT_ALONE))
        self.assertFalse(runner.left_alone_logged(REPAIR_LINE))
        self.assertFalse(runner.repair_logged(LEFT_ALONE))
        self.assertIn(runner.LEFT_ALONE_LINE, runner.BOOT_SCRIPT.read_text(encoding="utf-8"))

    def test_the_marker_property_is_not_read_as_the_shell(self) -> None:
        # The shell is denied the marker's label on a user build, so a read of it can only print nothing.
        self.assertFalse(hasattr(runner, "MARKER_PROPERTY"))
        self.assertFalse(hasattr(runner.Guest, "marker"))
        self.assertEqual(runner.SERVICE_PROPERTY, "init.svc.kernarium_wifi_network")

    def kept_guest(self, service: list[str], dumps: list[str], listing: list[str] | None = None) -> ScriptedGuest:
        class Kept(StubbedGuest):
            def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
                if command == f"cmd wifi add-network {runner.SSID} open -m":
                    self.calls.append("add metered network")
                    return ""
                return super().shell(command, timeout)

        return Kept(script_service=service, dump=dumps, status=[JOINED], listing=listing or [LISTING])

    def test_a_disk_of_a_release_that_never_disables_the_network_is_prepared_once_its_script_finished(self) -> None:
        metered = ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")
        guest = self.kept_guest(["running", "stopped"], [metered])
        self.assertIsNone(runner.prepare_kept(guest, WAITS))
        self.assertEqual(guest.restarts, 1)
        self.assertIn("add metered network", guest.calls)

    def test_a_previous_release_whose_script_did_not_run_fails_before_the_restart(self) -> None:
        # No saved network and no service state: the shape of a disk the script never touched.
        guest = self.kept_guest([""], [ENABLED_DUMP], listing=["Network Id      SSID                         Security type\n"])
        with self.assertRaisesRegex(runner.Failure, "VirtWifi is saved: not saved"):
            runner.prepare_kept(guest, WAITS)
        self.assertEqual(guest.restarts, 0)
        # A saved and joined network with the script never started or still waiting is not enough either.
        for state, text in (([""], "not started"), (["running"], "'running'")):
            guest = self.kept_guest(state, [ENABLED_DUMP])
            with self.assertRaisesRegex(runner.Failure, f"marks the network joined: .*{text}"):
                runner.prepare_kept(guest, WAITS)
            self.assertEqual(guest.restarts, 0)

    def test_a_script_that_does_not_finish_after_the_restart_fails(self) -> None:
        metered = ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")
        guest = self.kept_guest(["stopped", ""], [metered])
        with self.assertRaisesRegex(runner.Failure, "finishes after the restart: .*not started"):
            runner.prepare_kept(guest, WAITS)
        self.assertEqual(guest.restarts, 1)

    def test_a_network_the_script_saved_again_on_the_restart_loses_its_metered_setting(self) -> None:
        metered = ENABLED_DUMP.replace("meteredOverride 0", "meteredOverride 1")
        guest = self.kept_guest(["stopped"], [metered, ENABLED_DUMP])
        with self.assertRaisesRegex(runner.Failure, "holds metered 0, not 1"):
            runner.prepare_kept(guest, WAITS)

    def settings_guest(self, held: dict[str, str]) -> ScriptedGuest:
        class Settings(StubbedGuest):
            def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
                if command.startswith("settings get global "):
                    return held.get(command.removeprefix("settings get global "), "null") + "\n"
                return super().shell(command, timeout)

        return Settings(connectivity=[CONNECTIVITY], network_stack=[NETWORK_STACK], notifications=[QUIET_NOTIFICATIONS],
                        activities=[QUIET_ACTIVITIES], ui=[WARNING_SCREEN], status=[JOINED], dump=[ENABLED_DUMP])

    def test_the_probe_settings_the_disk_holds_are_read_back(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "lost the probe settings"):
            runner.check_warning_on_disk(self.settings_guest({}))
        self.assertIsNone(runner.check_warning_on_disk(self.settings_guest(dict(runner.CHECK_SETTINGS))))


NOT_STORED_LINE = "11-11 09:20:00.000 I kernarium_wifi_network: VirtWifi was not stored: saved again\n"
UNSTORED = "11-11 09:20:10.000 I kernarium_wifi_network: VirtWifi joined, and is not known to be stored: marker stays 1\n"
SETTLED = "11-11 09:25:10.000 I kernarium_wifi_network: VirtWifi was stored and joined: marker 2\n"
EMPTY_LISTING = "Network Id      SSID                         Security type\n"
ADDRESS = "    inet 192.0.2.16/24 scope global wlan0\n"


class QuickGuest(ScriptedGuest):
    """A guest for the boot after a quick stop. A restart gives the new boot's answers: its script log."""
    COMMANDS = {**ScriptedGuest.COMMANDS, "address": "ip -4 addr show dev wlan0 2>/dev/null; true"}

    def __init__(self, after_restart: list[str] | None = None, slow_address: float = 0.0, **answers: list[str]) -> None:
        super().__init__(**answers)
        self.after_restart = after_restart
        self.slow_address = slow_address

    def restart(self) -> None:
        super().restart()
        if self.after_restart is not None:
            self.answers[self.COMMANDS["script_log"]] = list(self.after_restart)

    def has_address(self) -> bool:
        runner.time.sleep(self.slow_address)
        return super().has_address()

    def diagnose(self) -> str:
        return ""


class QuickStopTests(unittest.TestCase):
    """The cases that stop a guest before the Wi-Fi service stores the network, and the boot that follows."""

    def setUp(self) -> None:
        clock = mock.patch.object(runner, "time", FakeClock())
        clock.start()
        self.addCleanup(clock.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def guest(self, **answers) -> QuickGuest:
        defaults = {"script_log": [""], "status": [JOINED], "listing": [LISTING], "script_service": ["stopped"],
                    "address": [ADDRESS], "log": [""]}
        return QuickGuest(**{**defaults, **answers})

    def lost(self, **answers) -> QuickGuest:
        """Data the stop left without the network: the first listing is empty, the script saves it again, and the
        boot after a restart finds it stored and joined."""
        answers.setdefault("after_restart", [SETTLED])
        return self.guest(listing=[EMPTY_LISTING, LISTING], script_log=["", NOT_STORED_LINE + UNSTORED], **answers)

    def judge(self, guest: QuickGuest) -> tuple[str | None, "runner.Quick"]:
        quick = runner.Quick()
        return runner.judge_quick_boot(guest, WAITS, quick), quick

    def test_the_lines_of_the_script_are_read_as_what_they_say(self) -> None:
        self.assertTrue(runner.repair_logged(REPAIR_LINE))
        self.assertTrue(runner.repair_logged(NOT_STORED_LINE))
        self.assertTrue(runner.not_stored_logged(NOT_STORED_LINE))
        self.assertFalse(runner.not_stored_logged(REPAIR_LINE))
        self.assertFalse(runner.repair_logged("--------- beginning of main\n"))
        self.assertTrue(runner.marker_one_logged(UNSTORED))
        self.assertTrue(runner.marker_two_logged(SETTLED))
        self.assertFalse(runner.marker_two_logged(UNSTORED))
        self.assertFalse(runner.marker_one_logged(SETTLED))

    def test_the_option_runs_the_cases_alone(self) -> None:
        parser = runner.build_parser()
        self.assertTrue(parser.parse_args(["--quick-stop-only"]).quick_stop_only)
        self.assertFalse(parser.parse_args([]).quick_stop_only)

    def test_data_that_lost_the_network_and_never_gets_it_back_fails(self) -> None:
        # What a release without the fix does: the script ends at marker 1, the list stays empty, nothing joins.
        guest = self.guest(status=[NOT_JOINED], listing=[EMPTY_LISTING], address=[""])
        with self.assertRaisesRegex(runner.Failure, r"that the stop left without VirtWifi held no address within "
                                    r"60 s.*lists nothing.*did not log that it saved"):
            self.judge(guest)

    def test_a_stop_after_the_store_wrote_the_network_shows_nothing(self) -> None:
        note, quick = self.judge(self.guest())
        self.assertFalse(quick.landed)
        self.assertIn("after the store wrote the network", note or "")

    def test_a_stored_network_that_never_joins_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"that held VirtWifi held no address within 60 s"):
            self.judge(self.guest(status=[NOT_JOINED], address=[""]))

    def test_the_network_saved_again_with_a_lease_and_marker_two_at_the_next_boot_passes(self) -> None:
        note, quick = self.judge(self.lost())
        self.assertTrue(quick.landed)
        self.assertIn("left marker 1 and the next boot set 2", note or "")

    def test_the_lines_are_kept_once_seen_and_the_lease_is_waited_for(self) -> None:
        guest = self.lost(status=[NOT_JOINED, NOT_JOINED, JOINED])
        guest.answers[guest.COMMANDS["script_log"]] = ["", NOT_STORED_LINE, UNSTORED, ""]
        self.assertIsNotNone(self.judge(guest)[0])

    def test_a_lease_that_the_last_observation_saw_late_fails(self) -> None:
        # The deadline is checked before each observation, so one that returns after it is judged by its time.
        with self.assertRaisesRegex(runner.Failure, r"held an address only 6\d s after boot_completed"):
            self.judge(self.lost(slow_address=61.0))

    def test_a_boot_that_saved_the_network_and_set_marker_two_fails(self) -> None:
        guest = self.lost()
        guest.answers[guest.COMMANDS["script_log"]] = ["", NOT_STORED_LINE + SETTLED]
        with self.assertRaisesRegex(runner.Failure, "set marker 2: a stop before the store's write"):
            self.judge(guest)

    def test_a_boot_that_does_not_say_it_left_marker_one_fails(self) -> None:
        guest = self.lost()
        guest.answers[guest.COMMANDS["script_log"]] = ["", NOT_STORED_LINE]
        with self.assertRaisesRegex(runner.Failure, "did not log that it left marker 1"):
            self.judge(guest)

    def test_a_script_that_does_not_end_fails_after_the_wait(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"the boot script is still running.*after 75 s"):
            self.judge(self.lost(script_service=["running"]))

    def test_a_network_that_is_not_in_the_store_after_the_restart_fails(self) -> None:
        guest = self.lost()
        guest.answers[guest.COMMANDS["listing"]] = [EMPTY_LISTING, LISTING, EMPTY_LISTING]
        with self.assertRaisesRegex(runner.Failure, "not listed after a restart"):
            self.judge(guest)

    def test_a_next_boot_that_does_not_set_marker_two_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "did not log that it set marker 2"):
            self.judge(self.lost(after_restart=[UNSTORED]))

    def test_a_denial_of_the_scripts_domain_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "SELinux denied the boot script's domain"):
            self.judge(self.lost(log=[DENIAL]))

    def case(self, outcomes: list, trigger: str = "saved", required: bool = True, attempts: int = runner.QUICK_ATTEMPTS):
        """run_quick_case over mocked boots: each outcome is None (the stop came after the write), 'landed', or a
        Failure the judged boot raises."""
        report = runner.Report()
        stopped = mock.Mock()
        first = mock.Mock()
        remaining = list(outcomes)

        def judge(guest, waits, quick):
            outcome = remaining.pop(0)
            if isinstance(outcome, Exception):
                raise outcome
            quick.landed = outcome == "landed"
            return "note"

        class Booted(ScriptedGuest):
            def diagnose(self) -> str:
                return ""

        options = runner.build_parser().parse_args([])
        with mock.patch.object(runner, "stop_when", first), \
                mock.patch.object(runner, "start_guest", lambda *a, **k: Booted()), \
                mock.patch.object(runner, "stop", stopped), \
                mock.patch.object(runner, "judge_quick_boot", judge):
            runner.run_quick_case(options, "adb", Path(tempfile.gettempdir()), report, WAITS, trigger, "title", required,
                                  attempts)
        return report, first.call_count, stopped.call_count

    def test_a_stop_that_shows_nothing_is_tried_again(self) -> None:
        report, boots, stops = self.case([None, None, "landed"])
        self.assertEqual((report.failures, boots, stops), ([], 3, 3))

    def test_no_stop_that_comes_before_the_store_fails_the_required_case(self) -> None:
        report, boots, stops = self.case([None] * 6)
        self.assertEqual((boots, stops), (6, 6))
        self.assertRegex(report.failures[0], "no stop came before the Wi-Fi service stored the network in 6 attempts")

    def test_no_stop_that_comes_before_the_store_is_a_note_for_the_other_case(self) -> None:
        report, boots, _ = self.case([None] * 2, trigger="joined", required=False, attempts=runner.QUICK_JOIN_ATTEMPTS)
        self.assertEqual((report.failures, boots), ([], 2))

    def test_a_failure_of_a_judged_boot_ends_the_case_at_once_and_stops_the_guest(self) -> None:
        report, boots, stops = self.case([runner.Failure("the data boots without VirtWifi")])
        self.assertEqual((len(report.failures), boots, stops), (1, 1, 1))

    def run_main(self, arguments: list[str]) -> list[str]:
        calls: list[str] = []
        with mock.patch.object(sys, "argv", ["run_android_wifi_network.py", *arguments]), \
                mock.patch.object(runner.boot_android, "resolve_executable", return_value="adb"), \
                mock.patch.object(runner, "fresh", lambda *a: calls.append("fresh")), \
                mock.patch.object(runner, "quick_stop", lambda *a: calls.append("quick stop")), \
                mock.patch.object(runner, "wake_only", lambda *a: calls.append("wake only")), \
                mock.patch.object(runner, "upgrade", lambda *a: calls.append("upgrade")):
            self.assertEqual(runner.main(), 0)
        return calls

    def test_the_wake_option_runs_the_sections_of_a_suspend_alone(self) -> None:
        self.assertEqual(self.run_main(["--wake-only"]), ["wake only"])

    def test_the_wake_option_takes_neither_of_the_other_section_options(self) -> None:
        for other in (["--quick-stop-only"], ["--previous-release", "r22"]):
            with self.subTest(other=other), contextlib.redirect_stderr(io.StringIO()) as errors, \
                    mock.patch.object(sys, "argv", ["run_android_wifi_network.py", "--wake-only", *other]), \
                    mock.patch.object(runner.boot_android, "resolve_executable", return_value="adb"):
                self.assertEqual(runner.main(), 2)
                self.assertIn("--wake-only runs on its own", errors.getvalue())

    def test_the_forward_port_follows_the_guest_slot(self) -> None:
        with mock.patch.dict(runner.os.environ):
            runner.os.environ.pop("GUEST_SLOT_PORT_BASE", None)
            self.assertEqual(runner.build_parser().parse_args([]).forward_port, runner.DEFAULT_FORWARD_PORT)
            runner.os.environ["GUEST_SLOT_PORT_BASE"] = "21300"
            parsed = runner.build_parser().parse_args([])
            self.assertEqual((parsed.adb_port, parsed.forward_port), (21300, 21300 + runner.FORWARD_OFFSET))
            self.assertEqual(runner.build_parser().parse_args(["--forward-port", "6001"]).forward_port, 6001)

    def test_the_cases_run_between_the_fresh_disk_and_the_upgrade(self) -> None:
        self.assertEqual(self.run_main(["--previous-release", "r22"]), ["fresh", "quick stop", "upgrade"])

    def test_the_option_leaves_out_every_other_section(self) -> None:
        self.assertEqual(self.run_main(["--quick-stop-only", "--previous-release", "r22"]), ["quick stop"])

    def test_a_guest_whose_bookkeeping_raises_is_stopped(self) -> None:
        options = runner.build_parser().parse_args([])
        launched = types.SimpleNamespace(poll=lambda: None, pid=1)
        stops: list[object] = []
        boot = runner.boot_android
        with mock.patch.object(boot, "parse_arguments", return_value=types.SimpleNamespace(
                    emulator=None, release=None)), \
                mock.patch.object(boot, "resolve_executable", return_value="emulator"), \
                mock.patch.object(boot, "release_path", return_value=Path("release")), \
                mock.patch.object(boot.android_instance, "read_release", return_value=object()), \
                mock.patch.object(boot, "plan_start", return_value=object()), \
                mock.patch.object(boot.android_instance, "apply"), \
                mock.patch.object(boot, "emulator_command", return_value=["emulator"]), \
                mock.patch.object(boot, "data_capacity", return_value=1), \
                mock.patch.object(boot, "wait_for_adb"), \
                mock.patch.object(boot, "wait_for_boot_completed", return_value=True), \
                mock.patch.object(boot.android_instance, "booted", side_effect=OSError("the instance record")), \
                mock.patch.object(runner.subprocess, "Popen", return_value=launched) as popen, \
                mock.patch.object(runner, "stop", lambda guest: stops.append(guest)), \
                mock.patch.object(runner, "Console", lambda *a: object()):
            with self.assertRaises(OSError):
                runner.start_guest(options, "adb", Path(tempfile.gettempdir()), None, Path(tempfile.gettempdir()) / "i",
                                   provision=False)
        self.assertEqual(len(stops), 1)
        # The launcher forwards a host port to the station's address, where the runner's echo server listens.
        self.assertIn(f"--net-forward=tcp:127.0.0.1:{options.forward_port}-10.0.2.16:7001", popen.call_args[0][0])


class ContinuityReadingTests(unittest.TestCase):
    """What the runner reads to hold one network, and the logs of its checks, to account."""

    def test_when_a_network_was_last_validated_is_read_for_wifi_alone(self) -> None:
        dump = (CONNECTIVITY + agent(103, VALIDATED, transport="CELLULAR", validated_at=77))
        self.assertEqual(runner.last_validated(dump), {102: 5000})
        self.assertEqual(runner.last_validated(agent(102, STOCK)), {})

    def test_a_pinned_network_must_stay_the_newest_and_stay_as_it_was_validated(self) -> None:
        pin = runner.Pin(102, 5000)
        self.assertEqual(runner.pin_problems(CONNECTIVITY, pin), [])
        replaced = CONNECTIVITY + agent(103, VALIDATED, validated_at=6000)
        self.assertEqual(runner.pin_problems(replaced, pin), ["network 102 was replaced by network 103"])
        again = "Current networks:\n" + agent(102, VALIDATED, validated_at=5600)
        self.assertEqual(runner.pin_problems(again, pin),
                         ["network 102 was validated again (lastValidated 5000 -> 5600)"])
        lost = "Current networks:\n" + agent(102, STOCK)
        self.assertEqual(runner.pin_problems(lost, pin), ["network 102 is no longer validated"])
        self.assertEqual(runner.pin_problems("Current networks:\n", pin), ["network 102 is gone"])

    def test_the_network_that_is_pinned_is_the_newest_validated_one(self) -> None:
        with mock.patch.object(runner, "time", FakeClock()):
            guest = ScriptedGuest(connectivity=["Current networks:\n" + agent(102, STOCK), CONNECTIVITY])
            self.assertEqual(runner.pin_network(guest, 30), runner.Pin(102, 5000))
            old = ScriptedGuest(connectivity=[CONNECTIVITY])
            with self.assertRaisesRegex(runner.Failure, "the newest network is still 102, not a new one"):
                runner.pin_network(old, 30, newer_than=102)

    def test_the_first_check_follows_the_validation(self) -> None:
        reason = runner.VALIDATED_FIRST_LINE
        probe = "PROBE_HTTP http://127.0.0.1/s1/generate_204 time=3ms ret=200"
        self.assertEqual(runner.first_validation_problems([reason, probe]), [])
        self.assertEqual(runner.first_validation_problems([reason]), [])
        # The strict private DNS name is probed before the network is validated, as stock does.
        self.assertEqual(runner.first_validation_problems(["PROBE_PRIVDNS ab.dns.google 12ms: []", reason, probe]), [])
        self.assertEqual(runner.first_validation_problems([probe, reason]),
                         ["a probe of the check completed before the network was validated"])
        self.assertEqual(len(runner.first_validation_problems([probe])), 1)

    def test_a_passing_check_is_new_only_after_the_last_failed_one(self) -> None:
        failed, passed = runner.CHECK_FAILED_LINE, runner.CHECK_PASSED_LINE
        self.assertEqual(runner.passed_after_failed_problems([failed, failed, passed]), [])
        self.assertEqual(runner.passed_after_failed_problems([passed]), [])
        self.assertEqual(runner.passed_after_failed_problems([failed, passed, failed]),
                         ["the last check in the validation log failed after the one that passed"])
        self.assertEqual(runner.passed_after_failed_problems([failed]),
                         [f"the validation log has no line {passed!r}"])

    def test_the_probe_of_the_scenario_must_have_received_the_answer(self) -> None:
        url = "http://127.0.0.1:4000/s1a2b3c4d/generate_204"
        answered = [f"PROBE_HTTP {url} time=3ms ret=302 request={{}} headers={{}}"]
        self.assertEqual(runner.probe_answer_problems(answered, "/s1a2b3c4d", 302), [])
        self.assertEqual(runner.probe_answer_problems([], "/s1a2b3c4d", 302),
                         ["the validation log shows no HTTP probe of /s1a2b3c4d"])
        # Another scenario's probe, or a stale one of an earlier network, does not count.
        self.assertEqual(len(runner.probe_answer_problems(answered, "/s00000000", 302)), 1)
        # The server saw the request and the probe timed out on it.
        timed_out = [f"PROBE_HTTP {url} Probe failed with exception java.net.SocketTimeoutException: timeout"]
        problems = runner.probe_answer_problems(timed_out, "/s1a2b3c4d", 302)
        self.assertEqual(len(problems), 1)
        self.assertIn("failed without an answer", problems[0])
        # An answer of another kind.
        self.assertIn("did not receive 200", runner.probe_answer_problems(answered, "/s1a2b3c4d", 200)[0])
        failed = [f"PROBE_HTTP {url} time=3ms ret=200 request={{}} headers={{}}",
                  f"PROBE_HTTP {url} 200 response with Content-length <= 4 interpreted as failed response."]
        self.assertEqual(runner.probe_answer_problems(failed, "/s1a2b3c4d", 200),
                         ["the 200 answer of /s1a2b3c4d was taken for a failed answer, not a page"])

    def test_the_checks_come_back_after_the_stock_back_off(self) -> None:
        self.assertEqual(runner.retry_problems([0.0, 1.3, 3.6, 8.0]), [])
        self.assertEqual(runner.retry_problems([0.0, 0.2, 3.6, 8.0]),
                         ["check 2 came 0.2 s after the one before, under the back-off of 0.9 s"])
        self.assertEqual(runner.retry_problems([0.0, 1.0, 2.0, 3.0]),
                         ["check 3 came 1.0 s after the one before, under the back-off of 1.8 s",
                          "check 4 came 1.0 s after the one before, under the back-off of 3.6 s"])
        self.assertEqual(runner.retry_problems([0.0, 1.3]), ["only 2 of 4 checks reached the server"])

    def test_the_private_dns_probe_is_not_repeated_for_each_check(self) -> None:
        self.assertEqual(runner.private_dns_probe_problems(1, 1), [])
        self.assertEqual(len(runner.private_dns_probe_problems(1, 5)), 1)
        self.assertEqual(len(runner.private_dns_probe_problems(0, 0)), 1)

    def test_a_log_collected_over_several_polls_counts_each_entry_once(self) -> None:
        union = runner.LogUnion()
        first = [("2026-11-13T03:00:01.000000", "PROBE_PRIVDNS ab.dns.google 12ms: []"),
                 ("2026-11-13T03:00:02.000000", runner.CHECK_FAILED_LINE)]
        union.add(first)
        union.add(first + [("2026-11-13T03:00:04.000000", runner.CHECK_FAILED_LINE)])
        self.assertEqual((union.count(runner.PRIVATE_DNS_PROBE), union.count(runner.CHECK_FAILED_LINE)), (1, 2))

    def test_the_entries_of_the_validation_log_carry_their_time_stamps(self) -> None:
        entries = runner.validation_entries(NETWORK_STACK)[102]
        self.assertEqual(entries[0], ("2026-11-10T13:30:32.803492", "Wi-Fi is validated before its internet check"))

    def test_a_dump_that_ends_in_a_hierarchy_root_is_read_whole(self) -> None:
        document = ("<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>\n<hierarchy rotation=\"0\">"
                    '<node index="0" text="Connected" package="com.android.settings" content-desc="" />'
                    "</hierarchy>\nUI hierarchy dumped to: /data/local/tmp/x.xml\n")
        self.assertEqual([node.text for node in runner.ui_nodes(document)], ["Connected"])
        displays = document.replace("<hierarchy", "<displays><hierarchy").replace("</hierarchy>", "</hierarchy></displays>")
        self.assertEqual([node.text for node in runner.ui_nodes(displays)], ["Connected"])

    def test_the_status_bar_icon_must_be_in_the_dump_to_judge_its_mark_absent(self) -> None:
        without_bar = runner.ui_nodes(hierarchy("Connected", None))
        self.assertFalse(runner.status_bar_present(without_bar))
        self.assertEqual(runner.mark_problems(without_bar, shown=False),
                         ["the dump has no status bar Wi-Fi icon, so its mark cannot be judged absent"])
        self.assertTrue(runner.status_bar_present(runner.ui_nodes(CLEAR_SCREEN)))
        # A node of another package that mentions Wi-Fi is not the status bar's icon.
        elsewhere = runner.ui_nodes(CLEAR_SCREEN.replace('package="com.android.systemui"',
                                                         'package="com.android.settings"'))
        self.assertFalse(runner.status_bar_present(elsewhere))


class HttpsFlagTests(unittest.TestCase):
    """The platform's switch for the HTTPS probe is a DeviceConfig flag of the connectivity namespace: the real
    `put_settings` and `delete_settings` send it there and read it back, over commands that are answered as the
    image answers them (a flag that was put reads back; one that was deleted, or never set, reads `null`; deleting
    one that is not set prints a failure and exits 0)."""

    class Recording(runner.Guest):
        def __init__(self, **faults: bool) -> None:
            self.commands: list[str] = []
            self.flag: str | None = None
            self.faults = faults

        def adb_run(self, *arguments: str, timeout: float = runner.COMMAND_SECONDS) -> subprocess.CompletedProcess:
            command = arguments[-1]
            self.commands.append(command)
            text = ""
            if command.startswith("cmd device_config put "):
                if not self.faults.get("put_ignored"):
                    self.flag = command.rsplit(" ", 1)[1]
            elif command.startswith("cmd device_config delete "):
                if self.flag is None:
                    text = "Failed to delete captive_portal_use_https from connectivity\n"
                elif not self.faults.get("delete_ignored"):
                    self.flag = None
            elif command.startswith("cmd device_config get "):
                text = (self.flag or "null") + "\n"
            return subprocess.CompletedProcess(list(arguments), 0, stdout=text.encode())

    def test_the_flag_is_put_to_device_config_and_read_back(self) -> None:
        guest = self.Recording()
        guest.put_settings({runner.USE_HTTPS_FLAG: "0"})
        self.assertEqual(guest.commands, ["cmd device_config put connectivity captive_portal_use_https 0",
                                          "cmd device_config get connectivity captive_portal_use_https"])

    def test_the_probe_settings_stay_global_settings(self) -> None:
        guest = self.Recording()
        guest.put_settings({"captive_portal_http_url": "http://127.0.0.1:1/generate_204", runner.USE_HTTPS_FLAG: "0"})
        guest.delete_settings(("captive_portal_http_url", runner.USE_HTTPS_FLAG))
        self.assertEqual(guest.commands, [
            "settings put global captive_portal_http_url http://127.0.0.1:1/generate_204",
            "cmd device_config put connectivity captive_portal_use_https 0",
            "cmd device_config get connectivity captive_portal_use_https",
            "settings delete global captive_portal_http_url",
            "cmd device_config delete connectivity captive_portal_use_https",
            "cmd device_config get connectivity captive_portal_use_https"])
        # The image's NetworkStack ignores a global setting of the flag's name: none is ever put.
        self.assertFalse([command for command in guest.commands if "global captive_portal_use_https" in command])

    def test_a_flag_that_does_not_read_as_put_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"cmd device_config put connectivity captive_portal_use_https 0: "
                                                    r"it reads 'null'"):
            self.Recording(put_ignored=True).put_settings({runner.USE_HTTPS_FLAG: "0"})

    def test_deleting_a_flag_that_is_not_set_is_no_error(self) -> None:
        guest = self.Recording()
        guest.delete_settings((runner.USE_HTTPS_FLAG,))
        self.assertEqual(guest.flag, None)

    def test_a_flag_that_is_still_set_after_the_delete_fails(self) -> None:
        guest = self.Recording(delete_ignored=True)
        guest.put_settings({runner.USE_HTTPS_FLAG: "0"})
        with self.assertRaisesRegex(runner.Failure, r"cmd device_config delete connectivity captive_portal_use_https: "
                                                    r"it still reads '0'"):
            guest.delete_settings((runner.USE_HTTPS_FLAG,))

    def test_the_flag_is_one_of_the_names_every_cleanup_deletes(self) -> None:
        self.assertIn(runner.USE_HTTPS_FLAG, runner.ALL_CHECK_SETTINGS)


class ProbeAnswerReadingTests(unittest.TestCase):
    """What the validation log says of the servers a check asked."""

    HTTP = f"PROBE_HTTP {runner.STOCK_HTTP_URL} time=20ms ret=204 request={{}} headers={{}}"
    HTTPS = f"PROBE_HTTPS {runner.STOCK_HTTPS_URL} time=234ms ret=204 request={{}} headers={{}}"

    def test_a_check_that_asked_no_https_probe_has_no_problem(self) -> None:
        self.assertEqual(runner.https_off_problems([self.HTTP, runner.CHECK_PASSED_LINE]), [])

    def test_an_https_probe_with_the_flag_at_zero_is_a_problem(self) -> None:
        problems = runner.https_off_problems([self.HTTP, self.HTTPS])
        self.assertEqual(len(problems), 1)
        self.assertIn("sent an HTTPS probe with the HTTPS flag at 0", problems[0])

    def test_the_stock_servers_answered_when_both_probes_got_204_at_their_urls(self) -> None:
        self.assertEqual(runner.stock_answer_problems([self.HTTP, self.HTTPS, runner.CHECK_PASSED_LINE]), [])

    def test_a_pass_needs_both_probes_at_the_stock_urls(self) -> None:
        # PROBE_HTTP is not the line of PROBE_HTTPS, and the stock HTTPS probe is not the stock HTTP one.
        self.assertEqual(len(runner.stock_answer_problems([self.HTTP])), 1)
        self.assertEqual(len(runner.stock_answer_problems([self.HTTPS])), 1)
        self.assertEqual(len(runner.stock_answer_problems([])), 2)

    def test_a_probe_that_failed_or_asked_another_server_is_not_the_stock_servers_answer(self) -> None:
        failed = self.HTTPS.replace("time=234ms ret=204", "Probe failed with exception java.io.IOException: x")
        self.assertEqual(len(runner.stock_answer_problems([self.HTTP, failed])), 1)
        elsewhere = self.HTTP.replace("connectivitycheck.gstatic.com", "127.0.0.1:9")
        self.assertEqual(len(runner.stock_answer_problems([elsewhere, self.HTTPS])), 1)
        redirected = self.HTTP.replace("ret=204", "ret=302")
        self.assertEqual(len(runner.stock_answer_problems([redirected, self.HTTPS])), 1)


class PlatformFormatTests(unittest.TestCase):
    """The lines the runner parses, in the shape the platform prints them (the print statements are named in
    each test), where a fixture that only agreed with the parser once hid a defect."""

    def test_broken_private_dns_is_the_token_after_the_ssid_not_a_capability(self) -> None:
        # NetworkCapabilities.toString: ` SSID: ...` then ` PrivateDnsBroken`; the capability list has no such name.
        broken = agent(7, VALIDATED, validated_at=1, private_dns_broken=True)
        plain = agent(7, VALIDATED, validated_at=1)
        self.assertIn(' SSID: "VirtWifi" PrivateDnsBroken UnderlyingNetworks: Null]}', broken)
        self.assertNotIn("PRIVATE_DNS_BROKEN", broken)
        self.assertIn("PRIVATE_DNS_BROKEN", runner.wifi_networks(broken)[7])
        self.assertNotIn("PRIVATE_DNS_BROKEN", runner.wifi_networks(plain)[7])
        self.assertIn("VALIDATED", runner.wifi_networks(broken)[7])

    def test_the_token_is_read_only_where_the_platform_prints_it(self) -> None:
        # An SSID that spells it, and text after `nc{...}`, are not the token.
        named = agent(7, VALIDATED, validated_at=1).replace('SSID: "VirtWifi"', 'SSID: "PrivateDnsBroken"')
        self.assertNotIn("PRIVATE_DNS_BROKEN", runner.wifi_networks(named)[7])
        trailing = agent(7, VALIDATED, validated_at=1).replace("factorySerialNumber=-1", "factorySerialNumber=-1 PrivateDnsBroken")
        self.assertNotIn("PRIVATE_DNS_BROKEN", runner.wifi_networks(trailing)[7])

    def test_a_network_that_is_not_validated_prints_no_last_validated(self) -> None:
        # NetworkAgentInfo.toString: lastValidated only while isValidated(), firstValidated once ever validated.
        line = agent(7, STOCK, validated_at=None, first_validated_at=500)
        self.assertIn("firstValidated 500", line)
        self.assertNotIn("lastValidated", line)
        self.assertEqual(runner.last_validated(line), {})
        self.assertEqual(runner.last_validated(agent(7, VALIDATED, validated_at=800)), {7: 800})

    def test_the_validation_log_header_is_the_network_and_its_name_quoted_or_not(self) -> None:
        # NetworkStackService prints the SharedLog tag, `<network> - <name>`, then its entries indented.
        for name in ('"VirtWifi"', "VirtWifi", ""):
            with self.subTest(name=name):
                dump = ("Validation logs (most recent first):\n" + f"102 - {name}\n"
                        "  2026-11-10T13:30:32.803492 - Wi-Fi is validated before its internet check\n\n"
                        "useNeighborResource: false\n")
                self.assertEqual(runner.validation_logs(dump), {102: ["Wi-Fi is validated before its internet check"]})

    def test_a_local_date_time_without_seconds_or_fraction_is_read(self) -> None:
        # LocalDateTime.toString leaves the seconds out when they and the nanoseconds are zero.
        dump = "Validation logs (most recent first):\n102 - VirtWifi\n  2026-11-10T13:30 - x\n"
        self.assertEqual(runner.validation_entries(dump), {102: [("2026-11-10T13:30", "x")]})

    def test_a_network_listed_with_its_security_types_on_one_line_is_found(self) -> None:
        # WifiShellCommand prints one line for each saved network, its types joined by `/` (`^` marks an upgrade).
        listing = "Network Id      SSID                         Security type\n0            VirtWifi                         open/owe^\n"
        self.assertEqual(runner.network_id(listing), 0)
        self.assertEqual(runner.saved_ssids(listing), ["VirtWifi"])

    def test_the_status_bar_icon_of_a_disconnected_wifi_is_not_the_mark(self) -> None:
        # SystemUI's inactive icon says "Wifi disconnected." and "No internet"; the mark is the connected icon's.
        off = runner.ui_nodes(hierarchy("Connected", "Wifi disconnected.,No internet"))
        self.assertFalse(runner.status_bar_no_internet(off))
        on = runner.ui_nodes(hierarchy("Connected", "Wifi signal full.,No internet"))
        self.assertTrue(runner.status_bar_no_internet(on))

    def test_a_forced_evaluation_that_the_platform_prints_as_unknown_is_a_failure(self) -> None:
        # ConnectivityService prints "Unknown network (net ID not found or no default network)" and exits 0.
        guest = StubbedGuest()
        guest.reevaluate = lambda net_id: runner.Guest.reevaluate(guest, net_id)
        guest.adb_run = lambda *a, timeout=0: mock.Mock(returncode=0, stdout=b"Unknown network (net ID not found or no default network)\n")
        with self.assertRaisesRegex(runner.Failure, "Unknown network"):
            guest.reevaluate(5)


class ForcedEvaluationReadingTests(unittest.TestCase):
    ENTRIES = [("2026-11-13T03:00:01.000000", "Wi-Fi internet check passed"),
               ("2026-11-13T03:00:09.000000", "Forcing reevaluation for UID 0"),
               ("2026-11-13T03:00:12.000000", "PROBE_PRIVDNS 1a2b3c4d-dnsotls-ds.metric.gstatic.com 12ms: []")]

    def test_a_network_whose_strict_private_dns_failed_is_not_validated_and_has_it_broken(self) -> None:
        self.assertEqual(runner.private_dns_failed_problems(frozenset({"INTERNET", "PRIVATE_DNS_BROKEN"})), [])
        self.assertIn("the network is validated",
                      runner.private_dns_failed_problems(frozenset({"VALIDATED", "PRIVATE_DNS_BROKEN"}))[0])
        self.assertIn("does not have PRIVATE_DNS_BROKEN",
                      runner.private_dns_failed_problems(frozenset({"INTERNET"}))[0])
        self.assertIn("lists no Wi-Fi network", runner.private_dns_failed_problems(None)[0])

    def test_no_check_after_the_forced_re_evaluation_is_the_proof(self) -> None:
        self.assertEqual(runner.forced_request_problems(self.ENTRIES[:1], self.ENTRIES, checked=False), [])

    def test_a_check_after_the_forced_re_evaluation_is_a_problem(self) -> None:
        for line in ("Wi-Fi internet check passed", "Wi-Fi internet check failed, the network stays validated"):
            with self.subTest(line=line):
                problems = runner.forced_request_problems(
                    self.ENTRIES[:1], [*self.ENTRIES, ("2026-11-13T03:00:14.000000", line)], checked=False)
                self.assertIn("an internet check ran after the forced re-evaluation request", problems[0])

    def test_a_check_that_the_log_held_before_the_request_is_not_the_requests(self) -> None:
        self.assertEqual(runner.forced_request_problems(self.ENTRIES[:1], self.ENTRIES, checked=False), [])

    def test_a_check_between_two_force_lines_is_a_problem(self) -> None:
        between = [self.ENTRIES[0], self.ENTRIES[1], ("2026-11-13T03:00:10.000000", "Wi-Fi internet check failed, x"),
                   ("2026-11-13T03:00:11.000000", "Forcing reevaluation for UID 0")]
        self.assertIn("an internet check ran",
                      runner.forced_request_problems(self.ENTRIES[:1], between, checked=False)[0])

    def test_a_check_before_the_first_force_line_of_the_request_is_a_problem(self) -> None:
        early = [self.ENTRIES[0], ("2026-11-13T03:00:08.000000", "Wi-Fi internet check passed"), self.ENTRIES[1]]
        self.assertIn("an internet check ran", runner.forced_request_problems(self.ENTRIES[:1], early, False)[0])

    def test_a_log_without_a_new_forced_line_proves_nothing(self) -> None:
        for entries in ([self.ENTRIES[0], self.ENTRIES[2]], self.ENTRIES[:1]):
            with self.subTest(entries=len(entries)):
                self.assertIn("no new 'Forcing reevaluation' line",
                              runner.forced_request_problems(self.ENTRIES[:1], entries, checked=False)[0])

    def test_an_older_forced_line_does_not_satisfy_a_later_request(self) -> None:
        # The log held the line before the request was made: only a line gained after it counts, however alike.
        self.assertIn("no new 'Forcing reevaluation' line",
                      runner.forced_request_problems(self.ENTRIES[:2], self.ENTRIES, checked=False)[0])
        self.assertIn("no new 'Forcing reevaluation' line",
                      runner.forced_request_problems(self.ENTRIES[:2], self.ENTRIES, checked=True)[0])
        again = [*self.ENTRIES, ("2026-11-13T03:00:30.000000", "Forcing reevaluation for UID 0")]
        self.assertEqual(runner.forced_request_problems(self.ENTRIES[:2], again, checked=False), [])

    def test_a_request_that_is_to_be_checked_needs_a_check_after_its_forced_line(self) -> None:
        before = self.ENTRIES[:2]
        forced = ("2026-11-13T03:01:00.000000", "Forcing reevaluation for UID 0")
        checked = ("2026-11-13T03:01:02.000000", "Wi-Fi internet check passed")
        autonomous = ("2026-11-13T03:00:59.000000", "Wi-Fi internet check passed")
        self.assertEqual(runner.forced_request_problems(before, [*before, forced, checked], checked=True), [])
        self.assertIn("no internet check after the forced re-evaluation",
                      runner.forced_request_problems(before, [*before, forced], checked=True)[0])
        self.assertIn("no internet check after the forced re-evaluation",
                      runner.forced_request_problems(before, [*before, autonomous, forced], checked=True)[0])

    def test_the_entries_are_read_in_time_order(self) -> None:
        late = [self.ENTRIES[2], ("2026-11-13T03:00:14.000000", "Wi-Fi internet check passed"), self.ENTRIES[1]]
        self.assertEqual(len(runner.forced_request_problems([], late, checked=False)), 1)


class FirstCompletionReadingTests(unittest.TestCase):
    URL = "http://127.0.0.1:41000/s1/generate_204"
    HELD = f"PROBE_HTTP {URL} time=3ms ret=200 request={{}} headers={{}}"
    FAILED = f"PROBE_HTTP {URL} Probe failed with exception java.net.ConnectException: Connection refused"

    def test_a_log_with_no_completed_probe_or_check_has_no_problem_while_the_request_is_held(self) -> None:
        for messages in ([], [runner.VALIDATED_FIRST_LINE],
                         [runner.VALIDATED_FIRST_LINE, "PROBE_DNS 127.0.0.1 3ms: [/127.0.0.1]",
                          "PROBE_PRIVDNS 1a2b3c4d-dnsotls-ds.metric.gstatic.com 12ms: [/8.8.8.8]"]):
            with self.subTest(messages=messages):
                self.assertEqual(runner.completed_before_problems(messages), [])

    def test_a_probe_or_check_that_completed_before_the_held_request_is_a_problem(self) -> None:
        for line in (self.FAILED, self.HELD, "PROBE_HTTPS https://192.0.2.1/generate_204 Probe failed with exception x",
                     "PROBE_FALLBACK http://127.0.0.1:41000/s1/gen_204 time=2ms ret=204", runner.CHECK_FAILED_LINE,
                     runner.CHECK_PASSED_LINE):
            with self.subTest(line=line):
                problems = runner.completed_before_problems([runner.VALIDATED_FIRST_LINE, line])
                self.assertIn("completed before the held request, so it is not the first", problems[0])

    def test_the_first_probe_to_complete_must_be_the_held_requests(self) -> None:
        good = [runner.VALIDATED_FIRST_LINE, self.HELD, runner.CHECK_FAILED_LINE]
        self.assertEqual(runner.first_completion_problems(good, "/s1", 200), [])
        for messages in ([runner.VALIDATED_FIRST_LINE, self.FAILED, self.HELD, runner.CHECK_FAILED_LINE],
                         [self.HELD.replace("ret=200", "ret=204"), runner.CHECK_FAILED_LINE],
                         [self.HELD.replace("/s1/", "/s2/"), runner.CHECK_FAILED_LINE]):
            with self.subTest(messages=messages):
                self.assertIn("the first probe to complete is not the held request's",
                              runner.first_completion_problems(messages, "/s1", 200)[0])

    def test_a_check_result_before_the_first_probe_is_a_problem(self) -> None:
        problems = runner.first_completion_problems([runner.CHECK_FAILED_LINE, self.HELD], "/s1", 200)
        self.assertIn("a check result is logged before the first probe completed", problems)

    def test_a_log_without_a_completed_probe_is_a_problem(self) -> None:
        self.assertEqual(runner.first_completion_problems([runner.VALIDATED_FIRST_LINE], "/s1", 200),
                         ["the validation log shows no probe that completed"])


def log_line(pid: int, tid: int, priority: str, tag: str, message: str, when: str = "03:10:01.000") -> str:
    """One line of `logcat -v threadtime`."""
    return f"11-13 {when} {pid:5d} {tid:5d} {priority} {tag:<8s}: {message}\n"


def java_crash(process: str, pid: int = 1234, tid: int = 1234, when: str = "03:10:01.000") -> str:
    """A Java crash: one logged message of several lines, which carry one time stamp."""
    return "".join(log_line(pid, tid, "E", "AndroidRuntime", message, when) for message in (
        "FATAL EXCEPTION: main", f"Process: {process}, PID: {pid}", "java.lang.NullPointerException",
        "\tat com.example.Foo.bar(Foo.java:12)"))


def anr(process: str, component: str | None = None, when: str = "03:10:01.000") -> str:
    component = component or f"{process}/.Main"
    return "".join(log_line(500, 520, "E", "ActivityManager", message, when) for message in (
        f"ANR in {process} ({component})", "PID: 1234", "Reason: Input dispatching timed out"))


def native_crash(process: str, signal: str = "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0",
                 dump_pid: int = 900, when: str = "03:10:01.000") -> str:
    """The tombstone that crash_dump logs under the tag DEBUG, from its own process and thread."""
    return "".join(log_line(dump_pid, dump_pid, "F", "DEBUG", message, when) for message in (
        "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***",
        f"Cmdline: {process}",
        f"pid: 1234, tid: 1250, name: RenderThread  >>> {process} <<<",
        "uid: 10001", signal))


FORMS = {"a Java crash": java_crash, "an ANR": anr, "a native crash": native_crash}


class CrashReadingTests(unittest.TestCase):
    def test_a_quiet_log_has_no_problem(self) -> None:
        self.assertEqual(runner.crash_problems(log_line(1000, 1, "I", "x", "nothing")), [])

    def test_the_three_processes_are_named_exactly(self) -> None:
        self.assertEqual(runner.WARNING_PROCESSES,
                         ("com.android.networkstack.process", "com.android.systemui", "com.android.settings"))

    def test_each_form_of_crash_of_each_process_is_a_problem(self) -> None:
        for process in runner.WARNING_PROCESSES:
            for form, make in FORMS.items():
                with self.subTest(process=process, form=form):
                    problems = runner.crash_problems(make(process))
                    self.assertEqual(len(problems), 1, problems)
                    self.assertIn(process, problems[0])

    def test_another_process_whose_name_extends_one_of_them_is_not(self) -> None:
        for process in runner.WARNING_PROCESSES:
            for other in (f"{process}.other", f"{process}:screenshot", f"x.{process}", f"{process}2"):
                for form, make in FORMS.items():
                    with self.subTest(process=other, form=form):
                        self.assertEqual(runner.crash_problems(make(other)), [])

    def test_the_package_of_the_network_stack_is_not_its_process(self) -> None:
        for form, make in FORMS.items():
            with self.subTest(form=form):
                self.assertEqual(runner.crash_problems(make("com.android.networkstack")), [])

    def test_a_process_that_does_not_belong_to_the_warning_is_not_a_problem(self) -> None:
        for form, make in FORMS.items():
            with self.subTest(form=form):
                self.assertEqual(runner.crash_problems(make("com.example.game")), [])

    def test_a_native_crash_of_any_fatal_signal_is_a_problem(self) -> None:
        for number, name in ((11, "SIGSEGV"), (6, "SIGABRT"), (7, "SIGBUS"), (8, "SIGFPE")):
            with self.subTest(signal=name):
                problems = runner.crash_problems(native_crash(
                    "com.android.settings", f"signal {number} ({name}), code 1 (X), fault addr 0x0"))
                self.assertEqual(len(problems), 1)
                self.assertIn(f"signal {number} ({name})", problems[0])

    def test_a_tombstone_header_without_a_signal_is_not_a_crash(self) -> None:
        self.assertEqual(runner.crash_problems(native_crash("com.android.settings", "uid: 10001")), [])

    def test_a_native_fatal_signal_that_names_no_process_is_a_problem(self) -> None:
        for signal in ("signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0", "signal 6 (SIGABRT), code -1"):
            with self.subTest(signal=signal):
                problems = runner.crash_problems(log_line(900, 900, "F", "DEBUG", signal))
                self.assertEqual(len(problems), 1)
                self.assertIn("a native crash that names no process", problems[0])

    def test_a_signal_line_whose_header_belongs_to_another_record_names_no_process(self) -> None:
        first = native_crash("com.example.game", "signal 11 (SIGSEGV), code 1 (X), fault addr 0x0")
        orphan = log_line(900, 900, "F", "DEBUG", "signal 6 (SIGABRT), code -1")
        self.assertIn("names no process", runner.crash_problems(first + orphan)[0])

    def test_a_line_that_only_looks_like_a_signal_line_is_not_a_native_crash(self) -> None:
        self.assertEqual(runner.crash_problems(log_line(7, 7, "I", "other", "signal 9 (SIGKILL) sent")), [])

    def test_a_finding_carries_the_time_stamp_of_its_record(self) -> None:
        found = runner.crash_findings(java_crash("com.android.settings", when="03:11:12.345"))
        self.assertEqual([finding.when for finding in found], ["11-13 03:11:12.345"])

    def test_the_signal_of_another_crash_does_not_belong_to_the_header(self) -> None:
        log = (native_crash("com.android.settings", "uid: 10001", dump_pid=900)
               + native_crash("com.example.game", dump_pid=901))
        self.assertEqual(runner.crash_problems(log), [])

    def test_an_illegal_instruction_in_any_process_is_a_problem_once(self) -> None:
        log = log_line(7, 7, "F", "libc", "Fatal signal 4 (SIGILL), code 1 (ILL_ILLOPC) in tid 7 (game)")
        self.assertIn("an illegal instruction", runner.crash_problems(log)[0])
        named = native_crash("com.android.settings", "signal 4 (SIGILL), code 1 (ILL_ILLOPC), fault addr 0x1")
        problems = runner.crash_problems(named)
        self.assertEqual(len(problems), 1)
        self.assertIn("an illegal instruction", problems[0])

    def test_the_process_line_of_a_crash_may_come_after_any_number_of_other_lines(self) -> None:
        crash = java_crash("com.android.settings", pid=1234).splitlines(keepends=True)
        noise = [log_line(2000 + n, 2000 + n, "I", "other", f"line {n}") for n in range(40)]
        log = crash[0] + "".join(noise) + "".join(crash[1:])
        problems = runner.crash_problems(log)
        self.assertEqual(len(problems), 1)
        self.assertIn("com.android.settings crashed", problems[0])

    def test_two_crashes_that_interleave_are_read_by_their_process_and_thread(self) -> None:
        settings = java_crash("com.android.settings", pid=1234, tid=1234).splitlines(keepends=True)
        game = java_crash("com.example.game", pid=777, tid=780).splitlines(keepends=True)
        log = settings[0] + game[0] + game[1] + settings[1] + game[2] + settings[2:] [0]
        problems = runner.crash_problems(log)
        self.assertEqual(len(problems), 1)
        self.assertIn("com.android.settings crashed", problems[0])
        swapped = game[0] + settings[0] + settings[1] + game[1]
        self.assertEqual(len(runner.crash_problems(swapped)), 1)

    def test_a_crash_that_names_no_process_is_a_problem(self) -> None:
        log = log_line(900, 900, "E", "AndroidRuntime", "*** FATAL EXCEPTION IN SYSTEM PROCESS: android.fg")
        self.assertIn("without naming itself", runner.crash_problems(log)[0])

    def test_a_process_line_of_another_message_is_not_the_crashs_own(self) -> None:
        log = (log_line(1234, 1234, "E", "AndroidRuntime", "FATAL EXCEPTION: main", "03:10:01.000")
               + log_line(1234, 1234, "E", "AndroidRuntime", "Process: com.android.settings, PID: 1234",
                          "03:10:09.000"))
        self.assertIn("without naming itself", runner.crash_problems(log)[0])

    def test_an_anr_is_read_with_and_without_its_component(self) -> None:
        for line in ("ANR in com.android.settings (com.android.settings/.Settings)", "ANR in com.android.settings"):
            with self.subTest(line=line):
                self.assertEqual(len(runner.crash_problems(log_line(500, 500, "E", "ActivityManager", line))), 1)

    def test_a_message_that_only_mentions_an_anr_is_not_one(self) -> None:
        line = "Skipping ANR in com.android.settings for ProcessRecord{1 1234:com.android.settings/u0a1}"
        self.assertEqual(runner.crash_problems(log_line(500, 500, "I", "ActivityManager", line)), [])


class MemoryStream:
    """The log stream of one boot (or one segment of it) of a LoggingGuest: the lines of the buffers from their
    start, then each line the guest logs, until the stream is cut. With `lag` it carries one more line of what
    the guest has logged each time it is read."""

    def __init__(self, guest: "LoggingGuest", lag: bool) -> None:
        self.guest, self.lag = guest, lag
        self.lines: list[str] = []
        self.pending: list[str] = []
        self.cut = False
        # A stream whose guest went away but which does not end of itself: its drain times out.
        self.stuck = False

    def start(self) -> None:
        self.pending = list(self.guest.buffer)
        self.guest.events.append(f"start {self.guest.streams.index(self)}")

    def text(self) -> str:
        # What the host has not read when the connection is cut is read only by a drain.
        take = 0 if self.cut else 1 if self.lag else len(self.pending)
        self.lines += self.pending[:take]
        del self.pending[:take]
        return "".join(self.lines)

    def ended(self) -> bool:
        return self.cut

    def drain(self, seconds: float) -> None:
        self.guest.events.append(f"drain {self.guest.streams.index(self)}")
        self.cut = True
        if self.stuck:
            raise runner.CaptureFailure(f"the log stream did not end of itself in {seconds:.0f} s")
        self.lines += self.pending
        self.pending = []

    def stop(self) -> None:
        self.cut = True


class LoggingGuest(ScriptedGuest):
    """A guest whose log is a list of lines that its streams carry: a line logged for the runner (its sync line)
    joins it, and a fault of the guest (a crash, a rotation, a stream that dies) is made by a section's body."""

    def __init__(self, lag: bool = False) -> None:
        super().__init__()
        self.buffer: list[str] = []
        self.streams: list[MemoryStream] = []
        self.events: list[str] = []
        self.lag = lag
        self.tick = 0
        self.capture = runner.LogCapture()
        self.capture.start_boot(self)

    def open_log_stream(self, boot: int, segment: int) -> MemoryStream:
        stream = MemoryStream(self, self.lag)
        self.streams.append(stream)
        return stream

    def stamp(self) -> str:
        self.tick += 1
        return f"03:{self.tick // 60:02d}:{self.tick % 60:02d}.000"

    def emit(self, text: str) -> None:
        self.buffer.append(text)
        for stream in self.streams:
            if not stream.cut:
                stream.pending.append(text)

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        if command.startswith("log -t "):
            tag, message = command[len("log -t "):].split(" ", 1)
            self.emit(log_line(4000, 4000, "I", tag, message, self.stamp()))
            return ""
        raise AssertionError(command)

    def alive(self) -> None:
        self.capture.check_stream()

    def diagnose(self) -> str:
        return ""

    def crash(self, process: str, form: str = "a Java crash") -> None:
        self.emit(FORMS[form](process, when=self.stamp()))

    def rotate(self) -> None:
        """The buffers wrap: what a stream has carried is in its file, what it has not yet is gone."""
        self.buffer = []


class CrashEvidenceTests(unittest.TestCase):
    """What the log stream of a guest says, read when each section ends and once more at the end."""

    def setUp(self) -> None:
        output = contextlib.redirect_stdout(io.StringIO())
        errors = contextlib.redirect_stderr(io.StringIO())
        output.__enter__()
        errors.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.addCleanup(errors.__exit__, None, None, None)
        patch = mock.patch.object(runner, "time", FakeClock())
        patch.start()
        self.addCleanup(patch.stop)
        self.report = runner.Report()
        self.guest = LoggingGuest()

    def run_section(self, name: str, body) -> None:
        self.report.section(name, self.guest, body)

    def summary(self) -> None:
        self.run_section(runner.CRASH_SECTION, lambda: runner.check_no_crashes(self.guest))

    def test_a_crash_during_a_section_fails_that_section_alone(self) -> None:
        for process in runner.WARNING_PROCESSES:
            for form in FORMS:
                with self.subTest(process=process, form=form):
                    report, guest = runner.Report(), LoggingGuest()
                    report.section("first", guest, lambda: guest.crash(process, form))
                    report.section("second", guest, lambda: None)
                    report.section(runner.CRASH_SECTION, guest, lambda: runner.check_no_crashes(guest))
                    self.assertEqual(len(report.failures), 2, report.failures)
                    self.assertTrue(report.failures[0].startswith("first: "), report.failures)
                    self.assertIn(process, report.failures[0])
                    self.assertTrue(report.failures[1].startswith(f"{runner.CRASH_SECTION}: "), report.failures)

    def test_a_crash_that_a_section_fails_on_is_named_with_its_own_failure(self) -> None:
        def body() -> None:
            self.guest.crash("com.android.systemui")
            raise runner.Failure("the mark never showed")

        self.run_section("a", body)
        self.assertIn("the mark never showed", self.report.failures[0])
        self.assertIn("com.android.systemui crashed", self.report.failures[0])

    def test_a_crash_of_an_unrelated_process_passes_everything(self) -> None:
        self.run_section("a", lambda: self.guest.crash("com.android.settings.other"))
        self.summary()
        self.assertEqual(self.report.failures, [])

    # The four sequences that lost a record to a boundary.
    def test_a_tombstone_header_at_the_end_of_one_section_and_its_signal_in_the_next_is_found(self) -> None:
        for process in runner.WARNING_PROCESSES:
            with self.subTest(process=process):
                report, guest = runner.Report(), LoggingGuest()

                def record(part) -> list[str]:
                    return part(native_crash(process, when=guest.stamp()).splitlines(keepends=True))

                report.section("a", guest, lambda: [guest.emit(line) for line in record(lambda lines: lines[:-1])])
                report.section("b", guest, lambda: guest.emit(record(lambda lines: lines[-1:])[0]))
                report.section(runner.CRASH_SECTION, guest, lambda: runner.check_no_crashes(guest))
                died = [failure for failure in report.failures if "died of signal 11" in failure]
                self.assertEqual(len(died), 2, report.failures)
                self.assertTrue(died[0].startswith("b: "), report.failures)

    def test_a_complete_crash_during_a_restart_before_boot_completed_is_found(self) -> None:
        guest, report = RebootingGuest(), runner.Report()
        guest.during_reboot = lambda: guest.crash("com.android.systemui")
        report.section("restarts", guest, guest.restart)
        self.assertEqual(len(report.failures), 1, report.failures)
        self.assertIn("com.android.systemui crashed", report.failures[0])

    def test_a_complete_crash_between_two_sections_is_found_and_belongs_to_none(self) -> None:
        self.run_section("one", lambda: None)
        self.guest.crash("com.android.settings")
        self.run_section("two", lambda: None)
        self.summary()
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertTrue(self.report.failures[0].startswith(f"{runner.CRASH_SECTION}: "))
        self.assertIn("outside a section: com.android.settings crashed", self.report.failures[0])

    def test_a_crash_before_the_first_section_is_found_and_belongs_to_none(self) -> None:
        self.guest.crash("com.android.networkstack.process", "an ANR")
        self.run_section("one", lambda: None)
        self.summary()
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertIn("outside a section: com.android.networkstack.process did not respond", self.report.failures[0])

    def test_a_native_signal_that_names_no_process_fails_the_section(self) -> None:
        self.run_section("a", lambda: self.guest.emit(log_line(
            900, 900, "F", "DEBUG", "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0", self.guest.stamp())))
        self.assertIn("a native crash that names no process", self.report.failures[0])

    def test_a_stream_that_dies_fails_the_section_in_progress(self) -> None:
        def body() -> None:
            self.guest.streams[-1].cut = True
            self.guest.alive()

        self.run_section("a", body)
        self.assertIn("the log stream of this boot ended while the guest is up", self.report.failures[0])

    def test_a_stream_that_dies_without_a_wait_fails_the_section_at_its_end(self) -> None:
        self.run_section("a", lambda: setattr(self.guest.streams[-1], "cut", True))
        self.assertIn("could not be read", self.report.failures[0])
        self.assertIn("the log stream ended", self.report.failures[0])

    def test_a_stream_that_carries_nothing_fails_the_section_after_its_bound(self) -> None:
        self.guest.streams[-1].pending = []
        self.guest.shell = lambda command, timeout=0: ""
        self.run_section("a", lambda: None)
        self.assertIn("did not carry a line the guest logged", self.report.failures[0])

    def test_the_log_wrapping_does_not_lose_what_the_stream_carried(self) -> None:
        self.run_section("one", lambda: None)
        self.guest.crash("com.android.systemui")
        self.guest.rotate()
        self.run_section("two", lambda: None)
        self.summary()
        self.assertIn("outside a section: com.android.systemui crashed", self.report.failures[0] + self.report.failures[-1])

    def test_a_stream_that_lags_is_waited_for(self) -> None:
        report, guest = runner.Report(), LoggingGuest(lag=True)
        report.section("a", guest, lambda: guest.crash("com.android.settings"))
        self.assertEqual(len(report.failures), 1, report.failures)
        self.assertIn("com.android.settings crashed", report.failures[0])

    def test_a_record_is_reported_once(self) -> None:
        self.run_section("a", lambda: self.guest.crash("com.android.settings"))
        self.run_section("b", lambda: None)
        self.summary()
        self.assertEqual(len(self.guest.capture.problems), 1, self.guest.capture.problems)
        self.assertEqual(len(self.report.failures), 2)

    def test_the_summary_passes_after_quiet_sections_and_lists_everything_after_a_crash(self) -> None:
        self.run_section("a", lambda: None)
        self.run_section("b", lambda: None)
        self.assertIn("2 sections read", runner.check_no_crashes(self.guest))
        self.run_section("c", lambda: self.guest.crash("com.android.settings", "an ANR"))
        self.guest.crash("com.android.systemui")
        with self.assertRaises(runner.Failure) as raised:
            runner.check_no_crashes(self.guest)
        self.assertIn("c: com.android.settings did not respond", str(raised.exception))
        self.assertIn("outside a section: com.android.systemui crashed", str(raised.exception))

    def test_the_summary_fails_when_no_section_read_the_log(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "no section"):
            runner.check_no_crashes(self.guest)
        with self.assertRaisesRegex(runner.Failure, "no section"):
            runner.check_no_crashes(ScriptedGuest())

    def test_a_guest_that_is_not_watched_reads_nothing(self) -> None:
        guest = ScriptedGuest()
        report = runner.Report()
        report.section("a", guest, lambda: None)
        self.assertEqual(report.failures, [])

    def test_the_last_section_of_each_guest_of_the_release_under_test_is_the_summary(self) -> None:
        for function in (runner.fresh, runner.wake_only, runner.upgrade_from):
            source = inspect.getsource(function)
            last = source[source.rindex("report.section("):]
            self.assertTrue(last.startswith("report.section(CRASH_SECTION, guest, "), function.__name__)

    def test_a_guest_of_the_release_under_test_is_watched_and_the_previous_release_is_not(self) -> None:
        source = inspect.getsource(runner.upgrade_from)
        self.assertEqual(source.count("watched=True"), 1)
        self.assertIn("options.release, instance, provision=False, watched=True", source)
        self.assertIn("watched=True", inspect.getsource(runner.fresh))

    def test_the_marker_reads_are_gone(self) -> None:
        for name in ("CrashLog", "SECTION_MARKER_TAG"):
            self.assertFalse(hasattr(runner, name), name)
        self.assertNotIn("section-start", inspect.getsource(runner))


class RebootingGuest(LoggingGuest):
    """A guest whose restart is the real one: its reboot cuts the streams of the old boot and clears the log, and
    `during_reboot` logs what the new boot logs before adb answers."""

    restart = runner.Guest.restart

    def __init__(self) -> None:
        super().__init__()
        self.rebooted = False
        self.before_reboot = lambda: None
        self.during_reboot = lambda: None

    def uptime(self) -> float:
        return 5.0 if self.rebooted else 900.0

    def adb_run(self, *arguments: str, timeout: float = 0) -> object:
        if arguments[0] == "reboot":
            self.before_reboot()
            self.rebooted = True
            for stream in self.streams:
                stream.cut = True
            self.buffer = []
            self.during_reboot()
        return mock.Mock(returncode=0, stdout=b"")

    def adb_answers(self) -> str | None:
        return None

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        if command == "getprop sys.boot_completed":
            return "1\n"
        return super().shell(command, timeout)


class CrashEvidenceAcrossARestartTests(unittest.TestCase):
    def setUp(self) -> None:
        patch = mock.patch.object(runner, "time", FakeClock())
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        errors = contextlib.redirect_stderr(io.StringIO())
        output.__enter__()
        errors.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.addCleanup(errors.__exit__, None, None, None)
        self.report = runner.Report()
        self.guest = RebootingGuest()

    def test_a_crash_before_the_restart_is_read_from_the_old_boots_stream(self) -> None:
        def body() -> None:
            self.guest.crash("com.android.networkstack.process")
            self.guest.restart()

        self.report.section("restarts", self.guest, body)
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertIn("com.android.networkstack.process crashed", self.report.failures[0])

    def test_a_crash_after_the_restart_is_read_from_the_new_boots_stream(self) -> None:
        def body() -> None:
            self.guest.restart()
            self.guest.crash("com.android.settings", "a native crash")

        self.report.section("restarts", self.guest, body)
        self.assertIn("com.android.settings died of signal 11", self.report.failures[0])

    def test_the_old_stream_is_read_to_its_end_before_the_new_boots_starts(self) -> None:
        # The guest logs a crash as it goes down: the old stream has not carried it when the connection ends,
        # and only its drain reads it. Without the drain the crash is lost, and the new boot's stream is not
        # the place to look for it.
        self.guest.before_reboot = lambda: self.guest.crash("com.android.settings")
        self.report.section("restarts", self.guest, self.guest.restart)
        self.assertEqual(len(self.guest.capture.boots), 2)
        self.assertTrue(self.guest.streams[0].cut)
        self.assertFalse(self.guest.streams[1].cut)
        self.assertEqual(self.guest.events, ["start 0", "drain 0", "start 1"])
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertIn("com.android.settings crashed", self.report.failures[0])

    def test_a_stream_that_dies_during_the_store_wait_fails_the_restart_before_the_reboot(self) -> None:
        # The stream dies while the restart waits for the configuration store, and an old-boot crash never reaches
        # it. The reboot that follows would clear the buffers, and the disconnect would pass for the stream's end.
        guest = self.guest

        class Clock(FakeClock):
            def sleep(self, seconds: float) -> None:
                super().sleep(seconds)
                if seconds == runner.STORE_SECONDS:
                    guest.streams[-1].cut = True
                    guest.crash("com.android.systemui")

        with mock.patch.object(runner, "time", Clock()):
            self.report.section("restarts", guest, guest.restart)
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertIn("the log stream of this boot ended while the guest is up", self.report.failures[0])
        self.assertFalse(guest.rebooted)

    def test_a_stream_that_dies_before_a_suspend_fails_it_before_the_connection_drops(self) -> None:
        guest = SuspendingGuest(drops=True)

        def body() -> None:
            guest.streams[-1].cut = True
            guest.suspend_and_wake()

        self.report.section("suspends", guest, body)
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertIn("the log stream of this boot ended while the guest is up", self.report.failures[0])

    def test_the_stream_is_confirmed_carrying_before_the_reboot_is_asked_for(self) -> None:
        asked: list[bool] = []
        self.guest.before_reboot = lambda: asked.append(self.guest.streams[0].text().count("sync-") >= 2)
        self.report.section("restarts", self.guest, self.guest.restart)
        self.assertEqual(asked, [True])

    def test_an_old_stream_that_does_not_end_is_a_failure_the_restart_does_not_retry_away(self) -> None:
        clock = FakeClock()
        self.guest.before_reboot = lambda: setattr(self.guest.streams[0], "stuck", True)
        with mock.patch.object(runner, "time", clock):
            self.report.section("restarts", self.guest, self.guest.restart)
        self.assertEqual(len(self.report.failures), 1, self.report.failures)
        self.assertIn("did not end of itself", self.report.failures[0])
        # Not waited out as a guest that does not answer yet, and no new boot's stream starts over it.
        self.assertLess(clock.now, 2 * runner.STORE_SECONDS)
        self.assertEqual(len(self.guest.capture.boots), 1)
        self.assertIn("evidence lost", " ".join(self.guest.capture.problems))

    def test_a_quiet_restart_passes_and_both_boots_are_read(self) -> None:
        self.report.section("restarts", self.guest, self.guest.restart)
        self.assertEqual(self.report.failures, [])
        self.assertEqual(len(self.guest.capture.boots), 2)

    def test_a_restart_between_sections_keeps_the_stream_going(self) -> None:
        self.guest.restart()
        self.assertEqual(len(self.guest.capture.boots), 2)
        self.report.section("after", self.guest, lambda: self.guest.crash("com.android.systemui"))
        self.assertIn("com.android.systemui crashed", self.report.failures[0])

    def test_a_restart_that_never_brings_adb_back_leaves_the_section_failed_not_passed(self) -> None:
        self.guest.adb_answers = lambda: "no answer"
        self.report.section("restarts", self.guest, self.guest.restart)
        self.assertEqual(len(self.report.failures), 1)
        self.assertIn("the guest restarts", self.report.failures[0])


class SuspendingGuest(LoggingGuest):
    """A guest that suspends and wakes through the real method; its suspend cuts the streams when it `drops`."""

    suspend_and_wake = runner.Guest.suspend_and_wake
    suspend_and_wake_guest = runner.Guest.suspend_and_wake_guest

    def __init__(self, drops: bool, before: str = "", on_wake: str = "") -> None:
        super().__init__()
        self.drops = drops
        # The launcher's log (the host's messages and the guest kernel's console): what it held before the
        # suspend, and what the wake adds.
        self.launcher_log = before
        guest = self

        class Console:
            def request(self, command: str, seconds: float = 0) -> str:
                if command == "suspend" and guest.drops:
                    for stream in guest.streams:
                        stream.cut = True
                if command == "wake":
                    guest.launcher_log += on_wake
                return "suspended" if command == "suspend" else "running"

        self.console = Console()

    def log_size(self) -> int:
        return len(self.launcher_log)

    def log_since(self, offset: int) -> str:
        return self.launcher_log[offset:]

    def adb_answers(self) -> str | None:
        return None


class CrashEvidenceAcrossASuspendTests(unittest.TestCase):
    def setUp(self) -> None:
        patch = mock.patch.object(runner, "time", FakeClock())
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        errors = contextlib.redirect_stderr(io.StringIO())
        output.__enter__()
        errors.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.addCleanup(errors.__exit__, None, None, None)
        self.report = runner.Report()

    def test_a_stream_that_ends_with_the_connection_starts_another_segment_of_the_same_boot(self) -> None:
        guest = SuspendingGuest(drops=True)
        self.report.section("suspends", guest, guest.suspend_and_wake)
        self.assertEqual(self.report.failures, [])
        self.assertEqual([len(boot) for boot in guest.capture.boots], [2])

    def test_a_stream_that_survives_the_suspend_is_kept(self) -> None:
        guest = SuspendingGuest(drops=False)
        self.report.section("suspends", guest, guest.suspend_and_wake)
        self.assertEqual(self.report.failures, [])
        self.assertEqual([len(boot) for boot in guest.capture.boots], [1])

    def test_a_record_that_two_segments_carry_is_reported_once(self) -> None:
        guest = SuspendingGuest(drops=True)

        def body() -> None:
            guest.crash("com.android.settings")
            guest.suspend_and_wake()

        self.report.section("suspends", guest, body)
        self.assertEqual(len(guest.capture.problems), 1, guest.capture.problems)
        self.assertEqual(len(self.report.failures), 1)

    def test_a_stream_that_ends_after_the_wake_fails_the_next_wait(self) -> None:
        guest = SuspendingGuest(drops=False)
        guest.suspend_and_wake()
        guest.streams[-1].cut = True
        with self.assertRaisesRegex(runner.Failure, "the log stream of this boot ended"):
            guest.alive()


KERNEL_GPU_ERROR = "[  186.505954][   T11] [drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1204 (command 0x202)\n"
KERNEL_OUT_OF_MEMORY = "[  186.505954][   T11] [drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1201 (command 0x204)\n"
HOST_GPU_REFUSAL = "Virtio-GPU submit rejected unknown context: id=3 bytes=44\n"
CONTEXT_REFUSED = "Virtio-GPU: the renderer refused to create context id=3\n"
HOST_RESET_LINE = gpu_suspend_log.RESET_LINE + " ring 1 held a deferred request\n"


class SuspendGpuLogTests(unittest.TestCase):
    """What the launcher's log says of the GPU across a suspend: the host's own reset line is the reset, any other
    GPU error is only an error whose cause is not established; the mark stays pending through the checks that
    follow the wake, and a failure of the whole operation yields to the diagnosis."""

    def setUp(self) -> None:
        patch = mock.patch.object(runner, "time", FakeClock())
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    # The two labels.
    def test_the_hosts_reset_line_is_the_reset_whatever_else_the_log_holds(self) -> None:
        text = "[ 1.0][ T1] init: x\n" + KERNEL_GPU_ERROR + HOST_GPU_REFUSAL + HOST_RESET_LINE
        self.assertEqual(gpu_suspend_log.diagnosis(text), f"{gpu_suspend_log.RESET_LABEL}: {HOST_RESET_LINE.strip()}")

    def test_only_an_out_of_memory_error_is_a_gpu_error_and_not_a_reset(self) -> None:
        found = gpu_suspend_log.diagnosis("PM: suspend exit\n" + KERNEL_OUT_OF_MEMORY)
        self.assertTrue(found.startswith(f"{gpu_suspend_log.ERRORS_LABEL}: {KERNEL_OUT_OF_MEMORY.strip()}"), found)
        self.assertIn("the cause is not established", found)
        self.assertNotIn(gpu_suspend_log.RESET_LABEL, found)

    def test_a_refusal_of_an_unknown_context_after_a_failed_creation_is_not_a_reset(self) -> None:
        found = gpu_suspend_log.diagnosis(CONTEXT_REFUSED + HOST_GPU_REFUSAL)
        self.assertTrue(found.startswith(f"{gpu_suspend_log.ERRORS_LABEL}: {HOST_GPU_REFUSAL.strip()}"), found)
        self.assertNotIn(gpu_suspend_log.RESET_LABEL, found)

    def test_the_first_error_line_is_quoted(self) -> None:
        found = gpu_suspend_log.diagnosis(KERNEL_GPU_ERROR + HOST_GPU_REFUSAL)
        self.assertTrue(found.startswith(f"{gpu_suspend_log.ERRORS_LABEL}: {KERNEL_GPU_ERROR.strip()} "), found)

    def test_a_log_without_a_gpu_line_has_no_diagnosis(self) -> None:
        self.assertIsNone(gpu_suspend_log.diagnosis("[ 1.0][ T1] PM: suspend exit\nVirtio-GPU vhost-user transport\n"))
        self.assertIsNone(gpu_suspend_log.diagnosis(""))

    def test_the_reset_phrase_inside_another_line_is_not_the_hosts_line(self) -> None:
        echoed = "[ 5.0][ T1] note: " + HOST_RESET_LINE
        self.assertIsNone(gpu_suspend_log.diagnosis(echoed))

    # A wake that shows it, through the real method.
    def test_a_wake_that_shows_the_hosts_reset_fails_with_the_reset_label_and_line(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake="PM: suspend exit\n" + KERNEL_GPU_ERROR + HOST_RESET_LINE)
        with self.assertRaisesRegex(runner.Failure, re.escape(f"{gpu_suspend_log.RESET_LABEL}: {HOST_RESET_LINE.strip()}")):
            guest.suspend_and_wake()

    def test_a_wake_that_shows_only_gpu_errors_fails_as_gpu_errors(self) -> None:
        for on_wake, line in ((KERNEL_GPU_ERROR, KERNEL_GPU_ERROR), (HOST_GPU_REFUSAL, HOST_GPU_REFUSAL)):
            with self.subTest(line=line.strip()[:40]):
                guest = SuspendingGuest(drops=True, on_wake=on_wake)
                with self.assertRaises(runner.Failure) as raised:
                    guest.suspend_and_wake()
                self.assertTrue(str(raised.exception).startswith(f"{gpu_suspend_log.ERRORS_LABEL}: {line.strip()}"))
                self.assertNotIn(gpu_suspend_log.RESET_LABEL, str(raised.exception))

    def test_lines_from_before_the_suspend_are_not_in_its_interval(self) -> None:
        guest = SuspendingGuest(drops=False, before=KERNEL_GPU_ERROR + HOST_RESET_LINE, on_wake="PM: suspend exit\n")
        guest.suspend_and_wake()

    def test_the_launchers_log_is_read_from_the_mark(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "emulator.log"
            log.write_text("before\n")
            stand_in = types.SimpleNamespace(log=log)
            size = runner.Guest.log_size(stand_in)
            log.write_text("before\n" + KERNEL_GPU_ERROR)
            self.assertEqual(runner.Guest.log_since(stand_in, size), KERNEL_GPU_ERROR)
            self.assertEqual(runner.Guest.log_size(types.SimpleNamespace(log=Path(directory) / "gone")), 0)

    # Output that arrives after the step has returned.
    class LateOutput(SuspendingGuest):
        """The first graphics work after the wake prints its refusal while the section waits for the station."""

        def __init__(self, line: str = HOST_RESET_LINE, **arguments: object) -> None:
            super().__init__(drops=False, **arguments)
            self.line = line

        def wait_joined(self, seconds: float, what: str) -> None:
            self.launcher_log += self.line

        def dump(self) -> str:
            return ENABLED_DUMP

    @staticmethod
    def waits_for_the_station(guest: "SuspendingGuest") -> None:
        """A section that suspends the guest and waits for the station: where late output arrives."""
        guest.suspend_and_wake()
        guest.wait_joined(runner.RESUME_SECONDS, "the station is joined after the wake")

    def test_a_reset_that_arrives_while_the_section_waits_for_the_station_fails_the_section(self) -> None:
        guest = self.LateOutput(on_wake="PM: suspend exit\n")
        report = runner.Report()
        report.section("a section that waits for the station", guest, lambda: self.waits_for_the_station(guest))
        self.assertEqual(len(report.failures), 1, report.failures)
        self.assertIn(f"{gpu_suspend_log.RESET_LABEL}: {HOST_RESET_LINE.strip()}", report.failures[0])

    def test_late_gpu_errors_fail_the_section_as_gpu_errors(self) -> None:
        guest = self.LateOutput(line=HOST_GPU_REFUSAL, on_wake="PM: suspend exit\n")
        report = runner.Report()
        report.section("a section that waits for the station", guest, lambda: self.waits_for_the_station(guest))
        self.assertIn(f"{gpu_suspend_log.ERRORS_LABEL}: {HOST_GPU_REFUSAL.strip()}", report.failures[0])
        self.assertIn("the cause is not established", report.failures[0])

    def test_a_section_that_failed_for_another_reason_still_gets_the_diagnosis_first(self) -> None:
        guest = self.LateOutput(on_wake="PM: suspend exit\n")

        def body() -> None:
            guest.suspend_and_wake()
            guest.wait_joined(1, "x")
            raise runner.Failure("the picture is still")

        report = runner.Report()
        report.section("a section", guest, body)
        self.assertTrue(report.failures[0].startswith(f"a section: {gpu_suspend_log.RESET_LABEL}: "), report.failures)
        self.assertIn("(the section had failed with: the picture is still)", report.failures[0])

    def test_a_clean_section_ends_the_pending_mark_and_a_diagnosis_is_reported_once(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake="PM: suspend exit\n")
        report = runner.Report()
        report.section("clean", guest, guest.suspend_and_wake)
        self.assertEqual(report.failures, [])
        self.assertIsNone(guest.suspend_log().mark)
        late = self.LateOutput(on_wake="PM: suspend exit\n")
        report.section("late", late, lambda: self.waits_for_the_station(late))
        report.section("the next section", late, lambda: None)
        self.assertEqual(len(report.failures), 1, report.failures)

    def test_the_mark_stays_pending_after_the_step_returns(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake="PM: suspend exit\n")
        guest.suspend_and_wake()
        self.assertIsNotNone(guest.suspend_log().mark)

    def test_a_line_that_arrived_after_the_step_is_not_lost_to_the_next_suspend(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake="PM: suspend exit\n")
        guest.suspend_and_wake()
        guest.launcher_log += HOST_RESET_LINE
        with self.assertRaisesRegex(runner.Failure, re.escape(f"before another suspend: {gpu_suspend_log.RESET_LABEL}")):
            guest.suspend_and_wake()

    # A failure of the whole operation.
    class AdbNeverAnswers(SuspendingGuest):
        def adb_answers(self) -> str | None:
            return "no answer"

    def test_an_adb_timeout_after_the_wake_yields_to_the_diagnosis_it_hides(self) -> None:
        guest = self.AdbNeverAnswers(drops=False, on_wake=HOST_RESET_LINE)
        with self.assertRaises(runner.Failure) as raised:
            guest.suspend_and_wake()
        text = str(raised.exception)
        self.assertTrue(text.startswith(f"{gpu_suspend_log.RESET_LABEL}: {HOST_RESET_LINE.strip()}"), text)
        self.assertIn("(the step had failed with: adb answers after the wake: no answer", text)
        self.assertIsInstance(raised.exception.__cause__, runner.Failure)
        self.assertFalse(guest.capture.expect_end)

    def test_an_adb_timeout_with_no_gpu_line_is_kept_as_it_was(self) -> None:
        guest = self.AdbNeverAnswers(drops=False, on_wake="PM: suspend exit\n")
        with self.assertRaisesRegex(runner.Failure, r"^adb answers after the wake: no answer \(after 120 s\)$") as raised:
            guest.suspend_and_wake()
        self.assertIsNone(raised.exception.__cause__)
        self.assertFalse(guest.capture.expect_end)

    def test_a_failed_resume_of_the_log_stream_yields_to_the_diagnosis_and_keeps_its_kind(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake=KERNEL_GPU_ERROR)
        guest.capture = mock.Mock()
        guest.capture.resumed.side_effect = runner.CaptureFailure("the old log stream did not end")
        with self.assertRaises(runner.CaptureFailure) as raised:
            guest.suspend_and_wake()
        self.assertIn(gpu_suspend_log.ERRORS_LABEL, str(raised.exception))
        self.assertIn("the old log stream did not end", str(raised.exception))
        self.assertFalse(guest.capture.expect_end)

    def test_a_failed_resume_of_the_log_stream_with_no_gpu_line_is_the_original(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake="PM: suspend exit\n")
        guest.capture = mock.Mock()
        failure = runner.CaptureFailure("the old log stream did not end")
        guest.capture.resumed.side_effect = failure
        with self.assertRaises(runner.CaptureFailure) as raised:
            guest.suspend_and_wake()
        self.assertIs(raised.exception, failure)

    def test_an_interrupt_is_not_turned_into_a_diagnosis(self) -> None:
        guest = SuspendingGuest(drops=False, on_wake=HOST_RESET_LINE)
        guest.capture = mock.Mock()
        guest.capture.resumed.side_effect = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            guest.suspend_and_wake()
        self.assertFalse(guest.capture.expect_end)


class LogStreamDrainTests(unittest.TestCase):
    """The real LogStream's drain, with the logcat process replaced."""

    def stream(self, *waits: object) -> runner.LogStream:
        stream = runner.LogStream("unused", "unused", Path("unused"))
        stream.process = mock.Mock()
        stream.process.poll.return_value = None
        stream.process.wait.side_effect = list(waits)
        return stream

    def test_a_stream_that_ends_of_itself_in_time_is_drained(self) -> None:
        stream = self.stream(0)
        stream.drain(60)
        stream.process.terminate.assert_not_called()

    def test_a_stream_that_does_not_end_is_stopped_and_is_a_failure(self) -> None:
        stream = self.stream(runner.subprocess.TimeoutExpired("capture", 60), 0)
        with self.assertRaisesRegex(runner.CaptureFailure, "did not end of itself in 60 s"):
            stream.drain(60)
        stream.process.terminate.assert_called_once()

    def test_the_failure_is_a_failure_of_the_section_and_not_a_retry(self) -> None:
        self.assertTrue(issubclass(runner.CaptureFailure, runner.Failure))


class SimulatedGuest(StubbedGuest):
    """A guest whose network follows the server that its probes ask, on the fake clock: a check at the time the
    platform would make it, a log line for each, and a screen that shows what the platform last reported. The
    platform is the image's: a check passes only when every probe it sends is answered 204, and it sends the HTTPS
    probe unless the HTTPS flag read 0 when the network joined, so the host's server (no HTTPS answer) passes a
    check only with the flag at 0 and the stock servers (both answered) with no URL setting. A fault is a way the
    platform or the screen can be wrong: each of them must fail the section that sees it."""

    def __init__(self, server: "runner.ProbeServer", clock: FakeClock, **faults: object) -> None:
        super().__init__()
        self.server, self.clock, self.faults = server, clock, faults
        self.settings: dict[str, str] = {}
        self.net_id = 100
        self.log: list[tuple[str, str]] = []
        self.other_log: list[tuple[str, str]] = []
        self.counter = 0
        self.forces = 0
        self.join()

    # What the platform does.
    def stamp(self) -> str:
        self.counter += 1
        return f"2026-11-13T03:{self.counter // 60:02d}:{self.counter % 60:02d}.000000"

    def add(self, message: str) -> None:
        self.log.append((self.stamp(), message))
        del self.log[:-20]

    def join(self) -> None:
        self.net_id += 1
        self.validated_at = 5000 + int(self.clock.now * 1000)
        url = self.settings.get("captive_portal_http_url", "")
        match = re.fullmatch(r"http://127\.0\.0\.1:\d+(/s[0-9a-f]+)?/generate_204", url)
        self.scenario = match[1] if match else None
        # No URL setting: the stock servers. The HTTPS flag is read once, when the network joins.
        self.stock = "captive_portal_http_url" not in self.settings
        self.https_off = self.settings.get(runner.USE_HTTPS_FLAG) == "0" and not self.faults.get("https_flag_ignored")
        self.strict = self.settings.get("private_dns_mode") == "hostname"
        self.failures = self.checks_done = 0
        self.settings_mark = self.bar_mark = bool(self.faults.get("mark_from_start"))
        self.log = []
        self.created = self.stamp() + "Z"
        self.broken = False
        self.validated = not self.faults.get("capability_late")
        self.in_flight: tuple[float, int] | None = None
        probe = "PROBE_HTTP http://127.0.0.1:1/x/generate_204 time=3ms ret=200 request={} headers={}"
        if self.faults.get("probe_first"):
            self.add(probe)
        if self.strict and not self.faults.get("no_private_dns_probe"):
            self.add("PROBE_PRIVDNS 1a2b3c4d-dnsotls-ds.metric.gstatic.com 12ms: [/8.8.8.8]")
        if not self.faults.get("explanation_late"):
            self.add(runner.VALIDATED_FIRST_LINE)
        self.next_check: float | None = None if self.faults.get("no_checks") else self.clock.now + 0.5

    def advance(self) -> None:
        if self.in_flight is not None:
            # The held answer goes out when the runner releases it, or when the server's hold is over.
            arrived, status = self.in_flight
            if not self.server.is_open(self.scenario) or self.clock.now - arrived >= runner.HELD_ANSWER_SECONDS:
                self.server.release()
                self.in_flight = None
                if self.faults.get("earlier_completes_late"):
                    self.answered_after_an_earlier_attempt(self.clock.now, status)
                else:
                    self.answered(self.clock.now, status)
        while self.next_check is not None and self.next_check <= self.clock.now:
            when, self.next_check = self.next_check, None
            self.check(when)

    def check(self, when: float) -> None:
        self.checks_done += 1
        if self.stock:
            # The probes ask the stock servers: with the HTTPS flag unset both are asked and both are answered; with
            # the flag at 0 the HTTP probe alone is asked. A host that reaches no stock server fails both probes.
            http, https = (runner.STOCK_HTTP_URL, runner.STOCK_HTTPS_URL)
            if self.faults.get("pass_elsewhere"):
                http, https = "http://198.51.100.7/generate_204", "https://198.51.100.7/generate_204"
            reachable = not self.faults.get("stock_servers_unreachable")
            failure = "Probe failed with exception java.net.UnknownHostException: no route"
            self.add(f"PROBE_HTTP {http} time=64ms ret=204 request={{}} headers={{}}" if reachable
                     else f"PROBE_HTTP {http} {failure}")
            if not self.https_off:
                self.add(f"PROBE_HTTPS {https} time=107ms ret=204 request={{}} headers={{}}" if reachable
                         else f"PROBE_HTTPS {https} {failure}")
            self.conclude(when, reachable)
            return
        if self.scenario is None:
            self.add("PROBE_HTTPS https://192.0.2.1/generate_204 Probe failed with exception java.net.SocketTimeoutException")
            self.add("PROBE_HTTP http://192.0.2.1/generate_204 Probe failed with exception java.net.SocketTimeoutException")
            self.conclude(when, False)
            return
        status = {"page": 200, "redirect": 302, "expected": 204}[self.server.mode]
        path = f"{self.scenario}/generate_204"
        if self.server.arrive(path, status, when=when):
            # The server holds the answer: the check is unanswered until it goes out.
            self.in_flight = (when, status)
            return
        self.answered(when, status)

    def answered(self, when: float, status: int) -> None:
        url = f"http://127.0.0.1:41000{self.scenario}/generate_204"
        if self.faults.get("probe_times_out"):
            self.add(f"PROBE_HTTP {url} Probe failed with exception java.net.SocketTimeoutException: timeout")
            passed = False
        else:
            self.add(f"PROBE_HTTP {url} time=3ms ret={status} request={{}} headers={{}}")
            passed = status == 204
            if passed and not self.https_off:
                # The HTTPS probe goes to the black hole and fails: partial connectivity, a failed check.
                self.add("PROBE_HTTPS https://192.0.2.1/generate_204 Probe failed with exception "
                         "java.net.SocketTimeoutException")
                passed = False
            elif passed and self.faults.get("https_probe_sent_anyway"):
                self.add("PROBE_HTTPS https://192.0.2.1/generate_204 time=3ms ret=204 request={} headers={}")
        if self.faults.get("explanation_late") and self.checks_done == 1:
            self.add(runner.VALIDATED_FIRST_LINE)
        self.conclude(when, passed)

    def answered_after_an_earlier_attempt(self, when: float, status: int) -> None:
        """The held request's answer goes out, and an attempt that was in flight before it logs its failure just
        before the answer's own line (nothing had completed when the runner looked)."""
        self.add(f"PROBE_HTTP http://127.0.0.1:41000{self.scenario}/generate_204 Probe failed with exception "
                 "java.net.ConnectException: Connection refused")
        self.answered(when, status)

    def conclude(self, when: float, passed: bool) -> None:
        if self.faults.get("capability_late") and not self.validated and not self.broken:
            # The platform of before: the network is validated only when its first check has an answer.
            self.validated, self.validated_at = True, 5000 + int(self.clock.now * 1000)
        if self.faults.get("private_dns_for_each_check") and self.strict:
            self.add("PROBE_PRIVDNS 5e6f7a8b-dnsotls-ds.metric.gstatic.com 12ms: [/8.8.8.8]")
        if passed:
            self.add(runner.CHECK_PASSED_LINE)
            self.failures = 0
            if self.faults.get("timer_after_pass"):
                self.next_check = when + 5
            if not self.faults.get("clear_only_on_rejoin"):
                self.settings_mark = self.bar_mark = False
        else:
            self.add(runner.CHECK_FAILED_LINE)
            self.failures += 1
            self.settings_mark = True
            self.bar_mark = self.bar_mark or not self.faults.get("bar_ignores_reports")
            delays = self.faults.get("delays")
            delay = delays[min(self.failures - 1, len(delays) - 1)] if delays else 2 ** (self.failures - 1)
            if not self.faults.get("no_retries"):
                self.next_check = when + 0.3 + delay
        if self.checks_done == self.faults.get("replace_at"):
            self.net_id += 1
        if self.checks_done == self.faults.get("revalidate_at"):
            self.validated_at += 1000

    # What the runner does to it.
    def put_settings(self, settings: dict[str, str]) -> None:
        super().put_settings(settings)
        self.settings.update(settings)
        self.private_dns_changed()

    def delete_settings(self, keys: tuple[str, ...] | list[str]) -> None:
        super().delete_settings(keys)
        for key in keys:
            self.settings.pop(key, None)
        self.private_dns_changed()

    def private_dns_changed(self) -> None:
        """A strict name that does not resolve fails the network the platform has, as the stock platform does:
        not validated, private DNS broken, nothing to check; the name gone, it is validated again."""
        broken = (self.settings.get("private_dns_mode") == "hostname"
                  and self.settings.get("private_dns_specifier") == runner.UNRESOLVED_NAME)
        if broken and not self.broken:
            self.broken, self.validated, self.next_check = True, False, None
            if self.faults.get("stale_force_line"):
                self.add(f"{runner.FORCED_LINE} for UID 0")
        elif not broken and self.broken and not self.faults.get("stays_broken"):
            if self.faults.get("reconnect_on_recovery"):
                self.join()
                return
            self.broken, self.validated = False, True
            self.validated_at += 1000
            if self.faults.get("autonomous_check_on_recovery"):
                self.next_check = self.clock.now + 2

    def capabilities(self) -> str:
        return VALIDATED if self.validated else STOCK

    def forget_and_save(self) -> None:
        super().forget_and_save()
        self.join()

    def suspend_and_wake(self, watch: bool = False, frozen: Callable[[], None] | None = None) -> None:
        super().suspend_and_wake(watch, frozen)
        if self.faults.get("wake_replaces_network"):
            self.join()

    def reevaluate(self, net_id: int) -> None:
        super().reevaluate(net_id)
        if self.faults.get("reevaluate_fails"):
            raise runner.Failure(f"cmd connectivity reevaluate {net_id} (rc 255): Permission denied")
        self.forces += 1
        if self.faults.get("reevaluate_ignored") or self.forces in self.faults.get("ignore_forces", ()):
            return
        line = f"{runner.FORCED_LINE} for UID 0"
        if self.faults.get("force_for_another_network"):
            self.other_log.append((self.stamp(), line))
        else:
            self.add(line)
        if self.faults.get("check_between_forces") and self.broken:
            self.add(runner.CHECK_FAILED_LINE)
            self.add(line)
        if self.faults.get("revalidate_on_force") and not self.broken:
            self.validated_at += 1000
        if self.broken:
            # The stock platform evaluates private DNS again, and it fails again. A check that completes
            # (the platform before the fix) reports the network validated, with its private DNS still broken.
            if self.faults.get("forced_check_validates_broken"):
                self.validated = True
                self.check(self.clock.now)
            if self.faults.get("forced_request_unlogged") and self.scenario:
                self.server.record(f"{self.scenario}/generate_204", 204, when=self.clock.now)
            return
        if self.faults.get("validated_force_checks_nothing"):
            return
        self.next_check = self.clock.now

    # What it answers: the platform acts by itself, so every look at the guest lets the clock's checks happen.
    def alive(self) -> None:
        self.advance()

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        if command == "dumpsys connectivity" and self.in_flight is not None:
            self.clock.now += self.faults.get("slow_look", 0)
        self.advance()
        answers = {
            "dumpsys connectivity": lambda: ("Current networks:\n" + agent(self.net_id - 1, STOCK)
                                             + agent(self.net_id, self.capabilities(),
                                                     validated_at=self.validated_at if self.validated else None,
                                                     first_validated_at=self.validated_at, created=self.created,
                                                     private_dns_broken=self.broken)),
            "dumpsys network_stack": lambda: ("Validation logs (most recent first):\n"
                                              + f'{self.net_id} - "VirtWifi"\n'
                                              + "".join(f"  {stamp} - {message}\n" for stamp, message in self.log)
                                              + (f'{self.net_id - 1} - "VirtWifi"\n' if self.other_log else "")
                                              + "".join(f"  {stamp} - {message}\n"
                                                        for stamp, message in self.other_log)),
            "dumpsys notification --noredact": lambda: (SIGN_IN_NOTIFICATIONS if self.faults.get("notification")
                                                        else QUIET_NOTIFICATIONS),
            "dumpsys activity activities": lambda: QUIET_ACTIVITIES,
            "cmd wifi status": lambda: JOINED,
            "dumpsys wifi": lambda: ENABLED_DUMP,
            ScriptedGuest.COMMANDS["ui"]: lambda: hierarchy(
                "Connected / Limited connection" if self.settings_mark else "Connected",
                "Wifi signal full.,No internet" if self.bar_mark else "Wifi signal full."),
        }
        return answers[command]()


class RetryIsFirstArrival(SimulatedGuest):
    """A platform whose first attempt at the check fails with a connection error before it reaches the server."""

    def check(self, when: float) -> None:
        if self.scenario is not None and self.checks_done == 0:
            self.checks_done += 1
            self.add(f"PROBE_HTTP http://127.0.0.1:41000{self.scenario}/generate_204 Probe failed with exception "
                     "java.net.ConnectException: Connection refused")
            self.conclude(when, False)
            return
        super().check(when)


class SimulatedSectionTests(unittest.TestCase):
    """The sections that follow one network over time, against a guest that behaves as the platform should, and
    against one that is wrong in each way the sections are there to catch."""

    def setUp(self) -> None:
        self.clock = FakeClock()
        patch = mock.patch.object(runner, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.server = runner.ProbeServer()
        self.addCleanup(self.server.close)

    def guest(self, **faults: object) -> SimulatedGuest:
        return SimulatedGuest(self.server, self.clock, **faults)

    def fails(self, section, guest: SimulatedGuest, pattern: str) -> None:
        with self.assertRaisesRegex(runner.Failure, pattern):
            section(guest)

    def recovery(self, guest: SimulatedGuest) -> str | None:
        return runner.check_recovery_on_one_network(guest, self.server)

    # One network from failing to passing and back.
    def test_the_warning_follows_one_network_from_a_failing_check_to_a_passing_one_and_back(self) -> None:
        guest = self.guest()
        note = self.recovery(guest)
        self.assertRegex(note, r"^network 102 validated while its first check was unanswered and at \d+ throughout, "
                               r"\d+ checks at the host's server \(HTTPS probe off\)$")
        # No rejoin or other action: the network joined once, and the forced check was asked for by its ID.
        self.assertEqual(guest.net_id, 102)
        self.assertEqual(guest.calls.count("forget and save"), 1)
        self.assertEqual([call for call in guest.calls if call.startswith("reevaluate")], ["reevaluate 102"])
        self.assertEqual(self.server.mode, "expected")

    def test_a_status_bar_that_does_not_redraw_for_a_report_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(bar_ignores_reports=True), "status bar's Wi-Fi icon does not say 'No internet'")

    def test_a_warning_that_goes_only_when_the_network_rejoins_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(clear_only_on_rejoin=True), "still says 'Limited connection'")

    def test_a_replaced_network_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(replace_at=2), "network 102 was replaced by network 103")

    def test_a_network_that_is_validated_again_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(revalidate_at=2), r"network 102 was validated again \(lastValidated")

    def test_checks_that_never_come_back_fail_the_section(self) -> None:
        self.fails(self.recovery, self.guest(no_retries=True), r"the check comes back after the back-off: 1 of 4 checks so far")

    def test_checks_that_come_back_before_the_back_off_fail_the_section(self) -> None:
        self.fails(self.recovery, self.guest(delays=[0.1]), "under the back-off of 1.8 s")

    def test_a_probe_that_completed_before_the_held_request_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(probe_first=True), "a probe or check had completed before the held request")

    def test_an_explanation_logged_after_the_first_probe_fails_the_section(self) -> None:
        # Nothing completed while the request was held; the explanation line comes after the probe's own line.
        self.fails(self.recovery, self.guest(explanation_late=True),
                   "a probe of the check completed before the network was validated")

    def test_a_refused_forced_re_evaluation_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(reevaluate_fails=True), "cmd connectivity reevaluate 102 .rc 255.: Permission denied")

    def test_a_forced_re_evaluation_that_checks_nothing_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(reevaluate_ignored=True),
                   "a forced re-evaluation checks the network again: the server saw no new check")

    def test_a_sign_in_notification_fails_the_section(self) -> None:
        self.fails(self.recovery, self.guest(notification=True), "a notification of ConnectivityService")

    # A page in place of the answer, received by the current network's own probe.
    def test_a_sign_in_page_and_a_block_page_are_read_by_the_probe_of_the_current_network(self) -> None:
        for mode, code in (("redirect", 302), ("page", 200)):
            guest = self.guest()
            note = runner.check_page_answer_warns(guest, self.server, mode)
            self.assertRegex(note, rf"requests answered {code} and read by the network's own probe$")

    def test_a_request_that_the_probe_timed_out_on_is_not_a_page_answer(self) -> None:
        guest = self.guest(probe_times_out=True)
        with self.assertRaisesRegex(runner.Failure, "the current network's probe received the page answer: "
                                    "the HTTP probe of /s[0-9a-f]+ failed without an answer"):
            runner.check_page_answer_warns(guest, self.server, "page")

    # Fresh monitors.
    def test_a_passing_check_on_a_network_that_has_just_joined_leaves_no_warning(self) -> None:
        self.assertIsNone(runner.check_passing_check_clears(self.guest(), self.server))

    def test_a_warning_that_stays_after_a_passing_check_fails_the_section(self) -> None:
        guest = self.guest(clear_only_on_rejoin=True, mark_from_start=True)
        with self.assertRaisesRegex(runner.Failure, "the warning is not there when the host's server passes the check: .*"):
            runner.check_passing_check_clears(guest, self.server)

    def test_a_platform_that_ignores_the_https_flag_does_not_pass_the_host_servers_check(self) -> None:
        # The HTTP probe gets 204 and the HTTPS probe goes to the black hole: partial connectivity, a failed check.
        guest = self.guest(https_flag_ignored=True)
        with self.assertRaisesRegex(runner.Failure, "the warning is not there when the host's server passes the check"):
            runner.check_passing_check_clears(guest, self.server)

    def test_the_host_server_alone_passes_a_check_only_with_the_https_flag_at_zero(self) -> None:
        for flag, passed in (({runner.USE_HTTPS_FLAG: "0"}, True), ({}, False)):
            with self.subTest(flag=flag):
                guest = self.guest()
                scenario = self.server.scenario()
                self.server.mode = "expected"
                self.server.clear()
                guest.rejoin_with_settings({**runner.page_settings(self.server.port, scenario), **flag}, clear=())
                self.clock.now += 5
                guest.advance()
                lines = [message for _, message in guest.log]
                self.assertEqual(runner.CHECK_PASSED_LINE in lines, passed, lines)
                self.assertEqual(runner.CHECK_FAILED_LINE in lines, not passed, lines)

    def test_an_https_probe_that_the_platform_sends_with_the_flag_at_zero_fails_the_sections_that_pass(self) -> None:
        pattern = "the platform sent an HTTPS probe with the HTTPS flag at 0"
        with self.assertRaisesRegex(runner.Failure, pattern):
            runner.check_passing_check_clears(self.guest(https_probe_sent_anyway=True), self.server)
        with self.assertRaisesRegex(runner.Failure, pattern):
            self.recovery(self.guest(https_probe_sent_anyway=True))
        with self.assertRaisesRegex(runner.Failure, pattern):
            runner.check_forced_evaluation_after_private_dns_failed(self.guest(https_probe_sent_anyway=True),
                                                                    self.server)

    def test_a_check_that_the_stock_servers_pass_leaves_no_warning(self) -> None:
        guest = self.guest()
        self.assertIsNone(runner.check_stock_servers_clear(guest))
        self.assertEqual(guest.settings, {})

    def test_a_pass_that_did_not_ask_the_stock_servers_is_not_theirs(self) -> None:
        with self.assertRaisesRegex(runner.Failure, f"no PROBE_HTTP of {re.escape(runner.STOCK_HTTP_URL)} that "
                                                    "received 204"):
            runner.check_stock_servers_clear(self.guest(pass_elsewhere=True))

    def test_stock_servers_that_the_host_cannot_reach_leave_the_warning(self) -> None:
        guest = self.guest(stock_servers_unreachable=True)
        with self.assertRaisesRegex(runner.Failure, r"the stock servers leave no warning \(the host must reach them\)"):
            runner.check_stock_servers_clear(guest)
        probes = [message for _, message in guest.log if message.startswith("PROBE_HTTP")]
        self.assertGreaterEqual(len(probes), 2, probes)
        self.assertTrue(all("Probe failed" in probe and "ret=204" not in probe for probe in probes), probes)
        self.assertIn(runner.CHECK_FAILED_LINE, [message for _, message in guest.log])

    def test_a_flag_that_a_deletion_left_at_zero_cannot_pass_the_stock_servers_check(self) -> None:
        # The stock probes then ask the HTTP probe alone: no HTTPS probe of the stock URL answered 204.
        class RetainsFlag(SimulatedGuest):
            def delete_settings(self, keys: tuple[str, ...] | list[str]) -> None:
                kept = self.settings.get(runner.USE_HTTPS_FLAG)
                super().delete_settings(keys)
                if kept is not None:
                    self.settings[runner.USE_HTTPS_FLAG] = kept

        guest = RetainsFlag(self.server, self.clock)
        guest.settings[runner.USE_HTTPS_FLAG] = "0"
        with self.assertRaisesRegex(runner.Failure, f"no PROBE_HTTPS of {re.escape(runner.STOCK_HTTPS_URL)} that "
                                                    "received 204"):
            runner.check_stock_servers_clear(guest)
        self.assertTrue(guest.https_off)
        self.assertFalse(any(message.startswith("PROBE_HTTPS ") for _, message in guest.log))

    def test_the_stock_branch_asks_the_https_probe_unless_the_flag_is_at_zero(self) -> None:
        for flag, https in (({}, True), ({runner.USE_HTTPS_FLAG: "0"}, False)):
            with self.subTest(flag=flag):
                guest = self.guest()
                guest.rejoin_with_settings(flag, clear=runner.ALL_CHECK_SETTINGS)
                self.clock.now += 5
                guest.advance()
                lines = [message for _, message in guest.log]
                self.assertEqual(any(line.startswith("PROBE_HTTPS ") for line in lines), https, lines)
                self.assertIn(runner.CHECK_PASSED_LINE, lines)

    def test_the_failed_check_of_a_black_hole_shows_only_the_warning(self) -> None:
        self.assertIn("validated, joined and enabled", runner.check_failed_check_warns(self.guest()))

    def test_a_wake_keeps_the_network_and_the_warning_stays_on_it(self) -> None:
        guest = self.guest()
        runner.check_failed_check_warns(guest)
        self.assertRegex(runner.check_warning_after_wake(guest),
                         r"^network 102 kept through the wake, validated at \d+ throughout, with the warning$")
        self.assertEqual(guest.net_id, 102)

    def test_a_wake_that_joins_a_new_network_fails_the_section(self) -> None:
        guest = self.guest(wake_replaces_network=True)
        runner.check_failed_check_warns(guest)
        self.fails(runner.check_warning_after_wake, guest, "network 102 became network 103")

    def test_a_disconnect_in_the_window_fails_the_section_although_the_network_is_kept(self) -> None:
        guest = self.guest()
        guest.after_mark = log_line(864, 864, "I", "wpa_supplicant", SUPPLICANT_DISCONNECT)
        runner.check_failed_check_warns(guest)
        self.fails(runner.check_warning_after_wake, guest, "the supplicant reported a disconnect")

    # Strict private DNS.
    def test_a_check_that_fails_again_and_again_does_not_probe_a_strict_private_dns_name_again(self) -> None:
        note = runner.check_strict_private_dns_holds(self.guest(), self.server)
        self.assertRegex(note, r"^\d+ failed checks, 1 private DNS probes, validated throughout$")

    def test_a_private_dns_probe_for_each_check_fails_the_section(self) -> None:
        guest = self.guest(private_dns_for_each_check=True)
        with self.assertRaisesRegex(runner.Failure, r"private DNS was probed \d+ more times while the checks failed"):
            runner.check_strict_private_dns_holds(guest, self.server)

    def test_a_strict_name_that_was_never_evaluated_proves_nothing(self) -> None:
        guest = self.guest(no_private_dns_probe=True)
        with self.assertRaisesRegex(runner.Failure, "the strict name was never evaluated"):
            runner.check_strict_private_dns_holds(guest, self.server)


class SimulatedForcedEvaluationTests(unittest.TestCase):
    """The sections of the forced re-evaluation of a network whose strict private DNS failed, of the quiet hold
    after a pass and of the time from the join to the validated line, against a platform that behaves as it
    should and against one that is wrong in the way each section is there to catch."""

    def setUp(self) -> None:
        self.clock = FakeClock()
        patch = mock.patch.object(runner, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.server = runner.ProbeServer()
        self.addCleanup(self.server.close)

    def guest(self, **faults: object) -> SimulatedGuest:
        return SimulatedGuest(self.server, self.clock, **faults)

    def forced(self, guest: SimulatedGuest) -> str | None:
        return runner.check_forced_evaluation_after_private_dns_failed(guest, self.server)

    def recovery(self, guest: SimulatedGuest) -> str | None:
        return runner.check_recovery_on_one_network(guest, self.server)

    def test_a_forced_re_evaluation_leaves_a_network_with_failed_private_dns_unvalidated_and_unchecked(self) -> None:
        note = self.forced(self.guest())
        self.assertIn("not validated and no check for 60 s", note)

    def test_the_platform_before_the_fix_reports_such_a_network_validated_and_the_section_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "after the forced re-evaluation: .*(the network is validated|"
                                                   "a check reached the host's server)"):
            self.forced(self.guest(forced_check_validates_broken=True))

    def test_the_server_sees_no_check_and_the_log_no_check_line_in_the_hold(self) -> None:
        guest = self.guest()
        self.forced(guest)
        text = " ".join(message for _, message in guest.log)
        self.assertEqual(guest.checks_done, 2, text)

    def test_a_platform_that_does_not_take_the_forced_re_evaluation_proves_nothing(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "no new 'Forcing reevaluation' line after the request"):
            self.forced(self.guest(reevaluate_ignored=True))

    def test_an_older_forced_line_does_not_satisfy_the_request(self) -> None:
        # The log holds a line from before the request; the request is ignored. The line is not the request's.
        with self.assertRaisesRegex(runner.Failure, "no new 'Forcing reevaluation' line after the request"):
            self.forced(self.guest(stale_force_line=True, ignore_forces=(1,)))

    def test_a_check_logged_between_two_force_lines_fails_the_request(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "an internet check ran after the forced re-evaluation request"):
            self.forced(self.guest(check_between_forces=True))

    def test_a_force_line_in_the_log_of_another_network_is_not_this_networks(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "no new 'Forcing reevaluation' line after the request"):
            self.forced(self.guest(force_for_another_network=True))

    def test_a_recovery_request_the_platform_ignores_is_not_satisfied_by_an_autonomous_check(self) -> None:
        # The first request is taken; the second is ignored, and the platform checks the network by itself.
        guest = self.guest(ignore_forces=(2,), autonomous_check_on_recovery=True)
        with self.assertRaisesRegex(runner.Failure, "no new 'Forcing reevaluation' line after the request"):
            self.forced(guest)
        self.assertEqual(guest.checks_done, 2)

    def test_a_recovery_request_that_is_taken_is_judged_by_its_own_line_and_check(self) -> None:
        guest = self.guest(autonomous_check_on_recovery=True)
        self.forced(guest)
        lines = [message for _, message in guest.log]
        self.assertEqual(sum(line.startswith(runner.FORCED_LINE) for line in lines), 2, lines)

    def test_a_network_replaced_while_it_recovers_fails_the_section_at_once(self) -> None:
        guest = self.guest(reconnect_on_recovery=True)
        with self.assertRaisesRegex(runner.Failure, "network 102 was replaced by network 103"):
            self.forced(guest)
        self.assertEqual(guest.calls.count("reevaluate 102"), 1)
        self.assertLess(self.clock.now, 200)

    def test_a_network_validated_again_by_the_recovery_request_fails_the_section(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"network 102 was validated again \(lastValidated"):
            self.forced(self.guest(revalidate_on_force=True))

    def test_a_request_at_the_server_after_the_forced_re_evaluation_fails_even_when_nothing_is_logged(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "a check reached the host's server"):
            self.forced(self.guest(forced_request_unlogged=True))

    def test_a_forced_re_evaluation_that_checks_nothing_once_validated_again_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "the server saw no new check"):
            self.forced(self.guest(validated_force_checks_nothing=True))

    def test_a_network_that_stays_unvalidated_after_the_name_is_gone_fails(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "the network is validated again|the network is not validated"):
            self.forced(self.guest(stays_broken=True))

    def test_a_network_that_is_never_reported_broken_fails(self) -> None:
        guest = self.guest()
        guest.private_dns_changed = lambda: None
        with self.assertRaisesRegex(runner.Failure, "the network does not have PRIVATE_DNS_BROKEN"):
            self.forced(guest)

    def test_the_section_does_not_return_until_the_network_is_validated_and_checked_again(self) -> None:
        guest = self.guest()
        self.forced(guest)
        self.assertTrue(guest.validated)
        self.assertFalse(guest.broken)
        self.assertEqual(guest.calls.count(f"reevaluate {guest.net_id}"), 2)

    # The network is validated while its first check is unanswered.
    def test_a_network_that_is_validated_while_its_first_check_is_unanswered_passes(self) -> None:
        guest = self.guest()
        note = self.recovery(guest)
        self.assertIn("validated while its first check was unanswered", note)
        # The server held the first answer, and it went out only after the runner looked at the network.
        self.assertFalse(self.server.is_open(guest.scenario))
        self.assertEqual(guest.calls.count("forget and save"), 1)

    def test_the_runner_looks_at_the_network_while_the_request_is_open_and_then_answers_it(self) -> None:
        guest = self.guest()
        looks: list[bool] = []
        connectivity = guest.connectivity

        def look() -> str:
            looks.append(self.server.is_open(guest.scenario))
            return connectivity()

        guest.connectivity = look
        self.recovery(guest)
        self.assertIn(True, looks)
        self.assertEqual(guest.net_id, 102)

    def test_an_early_explanation_with_a_late_capability_fails_the_section(self) -> None:
        # The log's line comes at the join, and the capability only when the check is answered: the platform
        # before the change, which no time bound between the join and the line can tell from the right one.
        guest = self.guest(capability_late=True)
        with self.assertRaisesRegex(runner.Failure, "while its first internet check was unanswered: "
                                                    "the network is not validated"):
            self.recovery(guest)
        self.assertFalse(self.server.is_open(guest.scenario))

    def test_a_retry_that_the_server_holds_in_place_of_a_first_attempt_that_failed_fails_the_section(self) -> None:
        # The first attempt fails with a connection error before it reaches the server, and only that
        # completion makes the network validated. The retry is the first request the server sees: holding it
        # does not keep the platform's real first check unanswered.
        for faults in ({"capability_late": True}, {}):
            with self.subTest(faults=faults):
                self.server.clear()
                guest = RetryIsFirstArrival(self.server, self.clock, **faults)
                with self.assertRaisesRegex(runner.Failure, "a probe or check had completed before the held "
                                                            "request, so it is not the first"):
                    self.recovery(guest)
                self.assertGreater(guest.checks_done, len(self.server.checks(guest.scenario)))

    def test_an_earlier_attempt_that_completes_after_the_look_is_not_the_held_requests_answer(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "the first probe to complete is not the held request's"):
            self.recovery(self.guest(earlier_completes_late=True))

    def test_a_look_that_does_not_fit_in_the_hold_fails_the_section(self) -> None:
        guest = self.guest(slow_look=runner.HELD_ANSWER_SECONDS + 1)
        with self.assertRaisesRegex(runner.Failure, "the held answer was out before the look at the network"):
            self.recovery(guest)

    def test_a_request_that_arrives_between_two_looks_at_the_hold_is_not_an_answer_that_is_out(self) -> None:
        # The interleaving: the hold is read as not arrived, and the request arrives right after. Read
        # as one state, that is "no check yet" and the next look finds it open, so the section goes on.
        guest = self.guest(no_checks=True)
        scenario = "/race"
        self.server.hold_first(scenario)
        read = self.server.hold_state
        raced: list[bool] = []

        def state_then_arrival(name: str) -> str:
            value = read(name)
            if not raced:
                raced.append(True)
                self.server.arrive(scenario + "/generate_204", 200)
            return value

        self.server.hold_state = state_then_arrival
        pin = runner.validated_while_unanswered(guest, self.server, scenario)
        self.assertEqual(pin.net_id, guest.net_id)
        self.assertEqual(raced, [True])

    def test_a_first_check_that_never_reaches_the_server_fails_the_section(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "the first check's request is held at the host's server: "
                                                    "the server has seen no check"):
            self.recovery(self.guest(no_checks=True))

    def test_a_network_that_is_partial_while_its_check_is_unanswered_fails_the_section(self) -> None:
        guest = self.guest()
        guest.capabilities = lambda: VALIDATED + "&PARTIAL_CONNECTIVITY"
        with self.assertRaisesRegex(runner.Failure, "while its first internet check was unanswered: .*PARTIAL"):
            self.recovery(guest)

    # The quiet hold after a pass.
    def test_a_pass_that_leaves_no_timer_lets_the_forced_check_be_told_apart(self) -> None:
        self.assertIn("checks", self.recovery(self.guest()))

    def test_a_timer_that_a_pass_leaves_fails_the_recovery_section(self) -> None:
        with self.assertRaisesRegex(runner.Failure, r"check\(s\) reached the server in the 20 s after a check passed"):
            self.recovery(self.guest(timer_after_pass=True))

    def test_the_hold_is_the_quiet_seconds_long_and_ends_at_once_on_a_check(self) -> None:
        guest = self.guest()
        scenario = self.server.scenario()
        self.server.record(f"{scenario}/generate_204", 204, when=0)
        pin = runner.pin_network(guest)
        started = self.clock.now
        runner.hold_quiet(guest, self.server, scenario, runner.QUIET_SECONDS, pin)
        self.assertGreaterEqual(self.clock.now - started, runner.QUIET_SECONDS)


class RunnerNamesTests(unittest.TestCase):
    def test_the_log_lines_are_the_ones_the_verifier_requires_in_the_image(self) -> None:
        verifier = runner.load_verifier()
        required = [marker.decode() for marker in verifier.NETWORK_STACK_MARKERS]
        self.assertIn(runner.CHECK_FAILED_LINE, required)
        # The first line is the validation before the check; the runner reads the check's results.
        self.assertEqual(len(required), 2)

    def test_the_words_are_androids_own(self) -> None:
        # WifiTrackerLib's wifitrackerlib_wifi_limited_connection and SystemUI's "No internet" description.
        self.assertEqual((runner.LIMITED_CONNECTION, runner.NO_INTERNET), ("Limited connection", "No internet"))


# ---- A suspend keeps the station.

def wpa_line(message: str, when: str = "03:10:01.000") -> str:
    return log_line(864, 864, "I", "wpa_supplicant", message, when)


class StationReadingTests(unittest.TestCase):
    """What the runner reads of a stay: the log after the mark, the platform's list, the guest's request and the echo
    server's log."""

    MARK = log_line(2270, 2270, "I", runner.SYNC_TAG, WAKE_MARK, "03:10:00.000")

    def test_a_log_without_a_disconnect_after_the_mark_has_no_problem(self) -> None:
        log = self.MARK + wpa_line("wlan0: CTRL-EVENT-CONNECTED - Connection to 62:eb completed")
        self.assertEqual(runner.disconnect_problems(log, WAKE_MARK), [])

    def test_the_supplicants_disconnect_after_the_mark_is_a_problem(self) -> None:
        log = self.MARK + wpa_line(SUPPLICANT_DISCONNECT, "03:10:16.013")
        found = runner.disconnect_problems(log, WAKE_MARK)
        self.assertEqual(len(found), 1, found)
        self.assertIn("the supplicant reported a disconnect", found[0])
        self.assertIn("reason=3 locally_generated=1", found[0])

    def test_the_platforms_disconnect_of_the_network_after_the_mark_is_a_problem(self) -> None:
        log = self.MARK + log_line(618, 808, "D", "ConnectivityService", PLATFORM_DISCONNECT, "03:10:16.050")
        found = runner.disconnect_problems(log, WAKE_MARK)
        self.assertEqual(len(found), 1, found)
        self.assertIn("ConnectivityService disconnected a network", found[0])

    def test_a_disconnect_before_the_mark_is_not_the_stays(self) -> None:
        log = wpa_line(SUPPLICANT_DISCONNECT, "03:09:00.000") + self.MARK
        self.assertEqual(runner.disconnect_problems(log, WAKE_MARK), [])

    def test_the_same_words_from_another_process_are_no_report(self) -> None:
        log = self.MARK + log_line(
            593, 593, "W", "MagnificationConnectionManager",
            "requestConnection duplicated request: connect=false, mConnectionState=DISCONNECTED")
        log += log_line(593, 775, "D", "ConnectivityService", "[100 CELLULAR] disconnected, was satisfying 25")
        log += log_line(864, 864, "I", "other", "wlan0: CTRL-EVENT-DISCONNECTED")
        self.assertEqual(runner.disconnect_problems(log, WAKE_MARK), [])

    def test_a_log_that_no_longer_holds_the_mark_cannot_be_read(self) -> None:
        found = runner.disconnect_problems(wpa_line("wlan0: CTRL-EVENT-CONNECTED"), WAKE_MARK)
        self.assertEqual(len(found), 1)
        self.assertIn("no longer holds the line logged before the suspend", found[0])

    def test_the_network_after_a_wake_must_be_the_one_before(self) -> None:
        networks = runner.wifi_networks(CONNECTIVITY)
        self.assertEqual(runner.kept_network_problems(networks, 102), [])
        self.assertEqual(runner.kept_network_problems(networks, 101), ["network 101 became network 102"])
        self.assertEqual(runner.kept_network_problems({}, 102),
                         ["network 102 is gone: ConnectivityService lists no Wi-Fi network"])

    def test_the_request_after_the_wake_needs_the_gateway_and_the_name_server(self) -> None:
        self.assertEqual(runner.request_problems(REQUEST_OUTPUT), [])
        failed = runner.request_problems("request t=1000.5 ping_rc=2 dns_tcp_bytes=0\n")
        self.assertEqual(len(failed), 2, failed)
        self.assertIn("the gateway did not answer a ping 1 s after the wake (status 2)", failed[0])
        self.assertIn("the name server did not answer a query over TCP 1 s after the wake (0 bytes)", failed[1])
        self.assertEqual(len(runner.request_problems("request t=1000.5 ping_rc=0 dns_tcp_bytes=13\n")), 1)
        self.assertIn("printed no answer", runner.request_problems("sh: nc: not found\n")[0])
        self.assertIn("printed no answer", runner.request_problems("")[0])

    def test_the_request_goes_through_the_default_network_to_the_launchers_servers(self) -> None:
        self.assertIn("ping -c 1 -W 2 10.0.2.2", runner.WAKE_REQUEST)
        self.assertIn("nc -w 3 10.0.2.3 53 2>/dev/null | wc -c", runner.WAKE_REQUEST)
        # The query is a DNS question for localhost over TCP: its length, the header, the name.
        query = bytes(int(octal, 8) for octal in runner.DNS_QUERY_OCTAL.split("\\")[1:])
        self.assertEqual(int.from_bytes(query[:2], "big"), len(query) - 2)
        self.assertEqual(query[14:24], b"\tlocalhost")

    def test_the_time_of_the_request_is_the_guests_clock_at_its_start(self) -> None:
        self.assertEqual(runner.request_time(REQUEST_OUTPUT), 1000.5)
        self.assertIsNone(runner.request_time("sh: nc: not found\n"))

    def test_the_end_of_a_connection_is_the_guests_clock_in_the_echo_servers_log(self) -> None:
        log = "open A 999.1\nopen B 999.2\nclose A 1000.25 rc=1\n"
        self.assertEqual(runner.closed_at(log, "A"), 1000.25)
        self.assertIsNone(runner.closed_at(log, "B"))
        self.assertIsNone(runner.closed_at("", "A"))

    def test_a_connect_that_took_the_stay_is_told_from_one_that_took_the_delay(self) -> None:
        start = wpa_line("wlan0: Trying to associate with 62:eb:c4:11:6e:64 (SSID='VirtWifi' freq=5240 MHz)", "03:10:05.000")
        held = self.MARK + start + wpa_line("wlan0: CTRL-EVENT-CONNECTED - Connection to 62:eb completed", "03:11:09.200")
        quick = self.MARK + start + wpa_line("wlan0: CTRL-EVENT-CONNECTED - Connection to 62:eb completed", "03:10:07.070")
        self.assertAlmostEqual(runner.connect_took(held, WAKE_MARK), 64.2, places=3)
        self.assertAlmostEqual(runner.connect_took(quick, WAKE_MARK), 2.07, places=3)
        self.assertIsNone(runner.connect_took(self.MARK + start, WAKE_MARK))
        self.assertIsNone(runner.connect_took(start, WAKE_MARK))
        # A connection that was reported before the connect began is not the connect.
        early = self.MARK + wpa_line("wlan0: CTRL-EVENT-CONNECTED - Connection to 62:eb completed", "03:10:01.000") + start
        self.assertIsNone(runner.connect_took(early, WAKE_MARK))

    def test_a_kernel_warning_and_a_call_trace_in_the_console_are_problems(self) -> None:
        console = ("[  134.17][   T9] PM: suspend entry (deep)\n"
                   "[  139.25][   T9] WARNING: CPU: 0 PID: 9 at net/wireless/sme.c:845 __cfg80211_connect_result+0x1c\n"
                   "[  139.26][   T9] Call Trace:\n"
                   "[  139.27][   T9] PM: suspend exit\n")
        found = runner.console_problems(console)
        self.assertEqual(len(found), 2, found)
        self.assertIn("WARNING: CPU: 0", found[0])
        self.assertIn("Call Trace:", found[1])
        self.assertEqual(runner.console_problems("[  1.0][ T1] PM: suspend entry (deep)\n[  2.0][ T1] PM: suspend exit\n"), [])


class LoopbackGuest:
    """A TCP echo server on the loopback that stands for the echo server of the guest: it echoes each line, and
    `ends_after` lines it has echoed (or on the line `ends_on`, which it does not echo) it ends the connection (a
    guest whose connection is dead).

    The end is a shutdown of its sending side; it then reads and drops what the host still writes until the host
    closes. A close would not do: a close with a line of the host unread, or a line of the host that reaches a closed
    socket, is answered with a reset, and whether the host reads the end or the reset first is the kernel's timing,
    which a slow scheduler changes. `resets` ends the connection with that reset on purpose. `end_with_line_pending`
    holds the end until a line of the host has reached the guest (`line_pending` records whether it did, or the wait
    ran out first), and `ended` is set once the guest has made its end: they put the end and the host's next line in
    either order, for the rows that force it."""

    def __init__(self, ends_after: int | None = None, ends_on: str | None = None, resets: bool = False,
                 end_with_line_pending: bool = False) -> None:
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.ends_after, self.ends_on = ends_after, ends_on
        self.resets, self.end_with_line_pending = resets, end_with_line_pending
        self.seen: list[str] = []
        self.closed = threading.Event()
        self.ended = threading.Event()
        self.line_pending: bool | None = None
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self) -> None:
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(connection,), daemon=True).start()

    def serve(self, connection: socket.socket) -> None:
        echoed = 0
        buffer = b""
        with connection:
            while True:
                try:
                    chunk = connection.recv(4096)
                except OSError:
                    return
                if not chunk:
                    self.closed.set()
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    text = line.decode()
                    self.seen.append(text)
                    if text == self.ends_on:
                        self.end(connection)
                        return
                    connection.sendall(line + b"\n")
                    echoed += 1
                    if self.ends_after is not None and echoed >= self.ends_after:
                        self.end(connection)
                        return

    def end(self, connection: socket.socket) -> None:
        if self.end_with_line_pending:
            self.line_pending = bool(select.select([connection], [], [], 5)[0])
        if self.resets:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            connection.close()
            self.ended.set()
            return
        connection.shutdown(socket.SHUT_WR)
        self.ended.set()
        while True:
            try:
                if not connection.recv(4096):
                    return
            except OSError:
                return

    def close(self) -> None:
        self.listener.close()


class LateReader(runner.Echo):
    """A host whose thread the scheduler holds between its write and its read: `pause` seconds pass after a line
    it writes."""

    pause = 0.0

    def send(self, text: str) -> None:
        super().send(text)
        time.sleep(self.pause)


class EchoConnectionTests(unittest.TestCase):
    """The host's connections into the guest's Wi-Fi address, against an echo server on the loopback."""

    def setUp(self) -> None:
        self.server = LoopbackGuest()
        self.addCleanup(self.server.close)
        self.connections: list[runner.Echo] = []

    def connect(self, name: str = "S", kind: type[runner.Echo] = runner.Echo) -> runner.Echo:
        connection = kind(self.server.port, name)
        self.connections.append(connection)
        self.addCleanup(connection.close)
        return connection

    def test_a_line_that_the_guest_echoes_passes(self) -> None:
        self.assertIsNone(self.connect().exchange("hello"))

    def test_expect_passes_over_the_lines_before_the_one_it_waits_for(self) -> None:
        connection = self.connect()
        connection.send("first")
        connection.send("second")
        self.assertIsNone(connection.expect("second", 5))

    def test_a_line_that_never_comes_is_what_the_failure_says(self) -> None:
        connection = self.connect()
        found = connection.expect("never", 1)
        self.assertIsNotNone(found)
        self.assertIn("no 'never' in 1 s", found)

    def test_a_connection_that_the_guest_ended_is_what_the_failure_says(self) -> None:
        self.server.ends_after = 1
        connection = self.connect()
        self.assertIsNone(connection.exchange("one"))
        self.assertEqual(connection.exchange("two", 5), "the guest ended the connection")

    def test_the_end_is_read_when_the_hosts_next_line_reached_the_guest_before_the_end(self) -> None:
        # A guest thread that the scheduler held after its last echo: the line is unread when the end comes.
        self.server.ends_after = 1
        self.server.end_with_line_pending = True
        connection = self.connect()
        self.assertIsNone(connection.exchange("one"))
        self.assertEqual(connection.exchange("two", 5), "the guest ended the connection")
        self.assertTrue(self.server.line_pending, "the guest ended before the host's line reached it: "
                                                  "the ordering that the row forces did not happen")

    def test_the_end_is_read_when_the_host_reads_long_after_it_wrote_to_the_ended_connection(self) -> None:
        # A host thread that the scheduler held between its write and its read: whatever the guest's socket
        # answered the line with has arrived by the time the host reads.
        self.server.ends_after = 1
        connection = self.connect(kind=LateReader)
        self.assertIsNone(connection.exchange("one"))
        self.assertTrue(self.server.ended.wait(5))
        connection.pause = 0.2
        self.assertEqual(connection.exchange("two", 5), "the guest ended the connection")

    def test_a_connection_that_the_guest_reset_is_what_the_failure_says(self) -> None:
        self.server.ends_on = "two"
        self.server.resets = True
        connection = self.connect()
        self.assertIsNone(connection.exchange("one"))
        found = connection.exchange("two", 5)
        self.assertTrue(found.startswith("the connection broke ("), found)

    def test_the_host_closing_its_end_reaches_the_server(self) -> None:
        connection = self.connect()
        self.assertIsNone(connection.exchange("hello"))
        connection.close()
        self.assertTrue(self.server.closed.wait(5))


class EchoServerTests(unittest.TestCase):
    def setUp(self) -> None:
        patch = mock.patch.object(runner, "time", FakeClock())
        patch.start()
        self.addCleanup(patch.stop)
        self.server = LoopbackGuest()
        self.addCleanup(self.server.close)
        self.calls: list[str] = []
        self.guest = types.SimpleNamespace(
            forward_port=self.server.port, start_echo=lambda: self.calls.append("start"),
            stop_echo=lambda: self.calls.append("stop"))

    def test_the_guests_server_is_started_before_a_connection_and_stopped_after_them_all(self) -> None:
        with runner.EchoServer(self.guest) as echo:
            first = echo.open("A")
            second = echo.open("B")
            self.assertEqual(self.calls, ["start"])
            self.assertIsNone(first.exchange("a"))
            self.assertIsNone(second.exchange("b"))
        self.assertEqual(self.calls, ["start", "stop"])
        self.assertIsNone(self.server.seen and None)
        self.assertEqual(self.server.seen[:2], ["A", "B"])

    def test_a_section_that_fails_still_stops_the_server_and_closes_the_connections(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "boom"):
            with runner.EchoServer(self.guest) as echo:
                connection = echo.open("A")
                raise runner.Failure("boom")
        self.assertEqual(self.calls, ["start", "stop"])
        with self.assertRaises(OSError):
            connection.sock.sendall(b"x\n")
            connection.sock.sendall(b"x\n")

    def test_a_server_that_does_not_answer_fails_with_the_forward_it_tried(self) -> None:
        free = socket.socket()
        free.bind(("127.0.0.1", 0))
        port = free.getsockname()[1]
        free.close()
        self.guest.forward_port = port
        with runner.EchoServer(self.guest) as echo:
            with self.assertRaisesRegex(runner.Failure, rf"does not answer at 10\.0\.2\.16:7001 .*host port {port}"):
                echo.open("A")

    def test_a_connection_that_is_accepted_but_not_echoed_is_tried_again(self) -> None:
        self.server.ends_on = "A"
        with runner.EchoServer(self.guest) as echo:
            with self.assertRaisesRegex(runner.Failure, "does not answer"):
                echo.open("A")
        self.assertGreaterEqual(self.server.seen.count("A"), runner.ECHO_ATTEMPTS)


class WatchedGuest(SuspendingGuest):
    """A guest that suspends through the real methods, with an order of events and the clock of each."""

    def __init__(self, **arguments: object) -> None:
        super().__init__(drops=False, **arguments)
        self.order: list[tuple[str, float]] = []
        self.process = types.SimpleNamespace(pid=4242, poll=lambda: None)
        self.adb, self.serial = "adb", "127.0.0.1:1"
        console = self.console

        def request(command: str, seconds: float = 0) -> str:
            self.note(command)
            return console.request(command, seconds)

        self.console = types.SimpleNamespace(request=request)

    def note(self, event: str) -> None:
        self.order.append((event, runner.time.monotonic()))

    def names(self) -> list[str]:
        return [event for event, _ in self.order]

    def shell(self, command: str, timeout: float = runner.COMMAND_SECONDS) -> str:
        if command == runner.WAKE_REQUEST:
            self.note("request")
            return REQUEST_OUTPUT
        if command.startswith(f"log -t {runner.SYNC_TAG} wake-"):
            self.note("mark")
        return super().shell(command, timeout)

    def prepare_suspend(self) -> None:
        self.note("prepare")
        super().prepare_suspend()


class WatchedSuspendTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        patch = mock.patch.object(runner, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.signals: list[tuple[int, int, float]] = []
        kill = mock.patch.object(runner.os, "kill", lambda pid, number: self.signals.append((pid, number, self.clock.now)))
        kill.start()
        self.addCleanup(kill.stop)

    def when(self, guest: WatchedGuest, event: str) -> float:
        return next(at for name, at in guest.order if name == event)

    def test_a_watched_suspend_marks_the_log_first_and_asks_a_second_after_the_wake(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.suspend_and_wake(watch=True)
        self.assertEqual(guest.names(), ["prepare", "mark", "suspend", "wake", "request"])
        self.assertGreaterEqual(self.when(guest, "request") - self.when(guest, "wake"), runner.WAKE_REQUEST_SECONDS)
        self.assertEqual(self.when(guest, "wake") - self.when(guest, "suspend"), runner.SUSPEND_STAY_SECONDS)
        self.assertEqual(guest.wake.request, REQUEST_OUTPUT)
        self.assertEqual(guest.wake.at, self.when(guest, "wake"))
        self.assertTrue(guest.wake.mark.startswith("wake-"))
        self.assertEqual(self.signals, [])

    def test_a_suspend_that_is_not_watched_asks_nothing_and_records_no_wake(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.suspend_and_wake()
        self.assertEqual(guest.names(), ["prepare", "suspend", "wake"])
        self.assertIsNone(guest.wake)

    def test_a_request_that_cannot_run_is_what_the_wake_records(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.shell = lambda command, timeout=0: (_ for _ in ()).throw(runner.Failure("adb: device offline"))
        self.assertIn("the request did not run: adb: device offline", guest.wake_request())

    def test_a_stay_with_the_emulator_stopped_runs_the_hosts_acts_in_it_and_continues_it(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.suspend_and_wake(frozen=lambda: guest.note("during"))
        stopped, continued = self.signals
        self.assertEqual((stopped[:2], continued[:2]), ((4242, runner.signal.SIGSTOP), (4242, runner.signal.SIGCONT)))
        self.assertEqual(continued[2] - stopped[2], runner.FROZEN_STAY_SECONDS)
        self.assertEqual(self.when(guest, "during") - stopped[2], runner.FROZEN_FIRST_SECONDS)
        self.assertEqual(guest.names(), ["prepare", "suspend", "during", "wake"])
        self.assertLessEqual(self.when(guest, "wake"), continued[2])

    def test_the_stay_with_the_emulator_stopped_is_as_long_as_it_is_asked_to_be(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.suspend_and_wake(frozen=lambda: None, stay=60)
        stopped, continued = self.signals
        self.assertEqual(continued[2] - stopped[2], 60)

    def test_the_stay_longer_than_the_guests_own_alarm_is_the_default_of_a_stopped_emulator(self) -> None:
        self.assertGreater(runner.FROZEN_STAY_SECONDS, 30)
        self.assertLess(runner.SUSPEND_STAY_SECONDS, 15)

    def test_the_emulator_is_continued_when_the_hosts_act_fails(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")

        def during() -> None:
            raise OSError("the connection broke")

        with self.assertRaisesRegex(OSError, "the connection broke"):
            guest.suspend_and_wake(frozen=during)
        self.assertEqual([signal for _, signal, _ in self.signals], [runner.signal.SIGSTOP, runner.signal.SIGCONT])
        self.assertNotIn("wake", guest.names())

    def test_the_emulator_is_continued_when_the_suspend_ends_in_the_stay(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.stay_frozen = lambda during, seconds: (guest.freeze(), (_ for _ in ()).throw(KeyboardInterrupt()))
        guest.thaw()
        self.assertEqual(self.signals, [])
        with self.assertRaises(KeyboardInterrupt):
            guest.suspend_and_wake(frozen=lambda: None)
        # The suspend's own failure path does not continue it: stop() does.
        self.assertEqual([signal for _, signal, _ in self.signals], [runner.signal.SIGSTOP])
        with mock.patch.object(runner.boot_android, "disconnect_adb"), \
                mock.patch.object(runner.boot_android, "stop_guest"):
            runner.stop(guest)
        self.assertEqual([signal for _, signal, _ in self.signals], [runner.signal.SIGSTOP, runner.signal.SIGCONT])

    def test_a_guest_that_was_never_stopped_is_not_signalled(self) -> None:
        guest = WatchedGuest()
        guest.thaw()
        with mock.patch.object(runner.boot_android, "disconnect_adb"), \
                mock.patch.object(runner.boot_android, "stop_guest"):
            runner.stop(guest)
        self.assertEqual(self.signals, [])

    def test_a_process_that_is_gone_is_nothing_to_continue_and_nothing_to_stop(self) -> None:
        guest = WatchedGuest()
        with mock.patch.object(runner.os, "kill", side_effect=ProcessLookupError):
            with self.assertRaisesRegex(runner.Failure, "the emulator is gone"):
                guest.freeze()
            guest.thaw()

    def test_a_suspend_that_is_prepared_does_not_confirm_the_log_stream_again(self) -> None:
        guest = WatchedGuest(on_wake="PM: suspend exit\n")
        guest.prepare_suspend()
        guest.suspend_and_wake(prepared=True)
        self.assertEqual(guest.streams[0].text().count("sync-"), 1)
        again = WatchedGuest(on_wake="PM: suspend exit\n")
        again.suspend_and_wake()
        self.assertEqual(again.streams[0].text().count("sync-"), 1)
        self.assertEqual(again.names().count("prepare"), 1)
        self.assertEqual(guest.names().count("prepare"), 1)


class StayRowTests(unittest.TestCase):
    """The rows that judge a stay, against a guest that answers from a script and an echo server on the loopback."""

    def setUp(self) -> None:
        self.clock = FakeClock()
        patch = mock.patch.object(runner, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)
        self.echo = LoopbackGuest()
        self.addCleanup(self.echo.close)

    def guest(self, **answers: list[str]) -> StubbedGuest:
        base = {"connectivity": [CONNECTIVITY], "status": [JOINED], "dump": [ENABLED_DUMP]}
        base.update(answers)
        guest = StubbedGuest(**base)
        guest.forward_port = self.echo.port
        return guest

    def fails(self, guest: StubbedGuest, pattern: str, **arguments: object) -> str:
        with self.assertRaisesRegex(runner.Failure, pattern) as raised:
            runner.keeps_station_through_stay(guest, **arguments)
        return str(raised.exception)

    def test_a_station_that_a_suspend_keeps_passes(self) -> None:
        guest = self.guest()
        self.assertEqual(runner.keeps_station_through_stay(guest),
                         "network 102 kept, no disconnect, the request answered, 1 connection carrying data")
        self.assertEqual(guest.calls, ["start echo", "suspend and wake", "stop echo"])
        self.assertEqual(self.echo.seen, ["S", "S-after-wake"])

    def test_the_stay_with_the_emulator_stopped_closes_one_connection_and_writes_to_another(self) -> None:
        guest = self.guest()
        self.assertEqual(runner.keeps_station_through_stay(guest, frozen=True),
                         "network 102 kept, no disconnect, the request answered, 3 connections carrying data")
        self.assertEqual(guest.calls, ["start echo", "suspend and wake while frozen", "stop echo"])
        self.assertEqual(self.echo.seen.count("C-during-stay"), 1)
        self.assertTrue(self.echo.closed.wait(5))
        self.assertIn("B-after-wake", self.echo.seen)
        self.assertIn("C-after-wake", self.echo.seen)
        self.assertNotIn("A-after-wake", self.echo.seen)

    def test_a_request_that_fails_after_the_wake_fails_the_row(self) -> None:
        guest = self.guest()
        guest.request = "request t=1000.5 ping_rc=2 dns_tcp_bytes=0\n"
        self.fails(guest, "the gateway did not answer a ping 1 s after the wake.*the name server did not answer")

    def test_a_station_that_is_not_joined_a_second_after_the_wake_fails_the_row(self) -> None:
        self.fails(self.guest(status=[NOT_JOINED]), "the station is not joined 1 s after the wake")

    def test_a_connection_that_is_dead_after_the_wake_fails_the_row(self) -> None:
        self.echo.ends_after = 1
        self.fails(self.guest(), "the open connection S carries no data after the wake: "
                                 "the guest ended the connection")

    def test_a_network_that_the_wake_replaced_fails_the_row_naming_both(self) -> None:
        guest = self.guest()
        woken = "Current networks:\n" + agent(103, VALIDATED, validated_at=9000) + "Other state\n"
        guest.on_wake = lambda: guest.answers.update({ScriptedGuest.COMMANDS["connectivity"]: [woken]})
        self.fails(guest, "network 102 became network 103")

    def test_a_disconnect_in_the_window_fails_the_row_with_the_line(self) -> None:
        guest = self.guest()
        guest.after_mark = (wpa_line(SUPPLICANT_DISCONNECT, "03:10:16.013")
                            + log_line(618, 808, "D", "ConnectivityService", PLATFORM_DISCONNECT, "03:10:16.050"))
        message = self.fails(guest, "the supplicant reported a disconnect")
        self.assertIn("ConnectivityService disconnected a network", message)

    def test_the_window_is_read_ten_seconds_after_the_wake(self) -> None:
        guest = self.guest()
        runner.keeps_station_through_stay(guest)
        self.assertGreaterEqual(self.clock.now, guest.wake.at + runner.WAKE_WINDOW_SECONDS)

    def test_a_saved_network_that_is_disabled_after_the_wake_fails_the_row(self) -> None:
        self.fails(self.guest(dump=[DISABLED_DUMP]), "the saved network is NETWORK_SELECTION_PERMANENTLY_DISABLED after the wake")

    def test_every_problem_is_reported_and_the_server_is_stopped(self) -> None:
        guest = self.guest(status=[NOT_JOINED])
        guest.request = "request t=1000.5 ping_rc=2 dns_tcp_bytes=0\n"
        message = self.fails(guest, "the station is not joined")
        self.assertIn("the gateway did not answer", message)
        self.assertEqual(guest.calls[-1], "stop echo")

    def test_a_connection_that_the_host_closed_must_have_ended_for_the_guest_by_the_request(self) -> None:
        for lines in ("", "open A 999.0\n", "close A 1002.0 rc=0\n"):
            with self.subTest(lines=lines):
                guest = self.guest()
                guest.echo_lines = lines
                self.fails(guest, "the connection that the host closed during the stay was not ended for the guest "
                                  r"within 1 s of the wake", frozen=True)

    def test_a_line_that_the_host_wrote_in_the_stay_must_arrive(self) -> None:
        self.echo.ends_on = "C-during-stay"
        self.fails(self.guest(), "the line the host sent during the stay did not arrive: "
                                 "the guest ended the connection", frozen=True)

    def test_an_idle_connection_that_carries_nothing_after_the_stay_fails_the_row(self) -> None:
        self.echo.ends_on = "B-after-wake"
        self.fails(self.guest(), "the open connection B carries no data after the wake", frozen=True)

    def test_no_network_listed_before_the_suspend_is_no_row(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "lists no Wi-Fi network before the suspend"):
            runner.keeps_station_through_stay(self.guest(connectivity=["Current networks:\n"]))

    def toggled(self, **answers: list[str]) -> StubbedGuest:
        newer = "Current networks:\n" + agent(103, VALIDATED, validated_at=9000) + "Other state\n"
        return self.guest(**{"connectivity": [CONNECTIVITY, newer], "status": [OFF, JOINED], **answers})

    def test_wifi_turned_off_and_on_joins_a_new_network_that_the_suspend_keeps(self) -> None:
        guest = self.toggled()
        self.assertEqual(runner.check_suspend_after_wifi_toggle(guest),
                         "network 103 kept, no disconnect, the request answered, 1 connection carrying data")
        self.assertEqual(guest.calls[:2], ["cmd wifi set-wifi-enabled disabled", "cmd wifi set-wifi-enabled enabled"])
        self.assertIn("joined the station joins after Wi-Fi was turned on", guest.calls)
        self.assertGreaterEqual(self.clock.now, runner.TOGGLE_SETTLE_SECONDS)

    def test_wifi_that_comes_back_as_the_same_network_proves_nothing_and_fails(self) -> None:
        guest = self.guest(status=[OFF, JOINED])
        with self.assertRaisesRegex(runner.Failure, "the station joins a new network: no new network is listed"):
            runner.check_suspend_after_wifi_toggle(guest)


class ConnectDelayGuest(StubbedGuest):
    """A guest for the suspend inside the connect delay: its log holds the supplicant's lines of each attempt, and what
    each attempt left of the station."""

    START = "wlan0: Trying to associate with 62:eb:c4:11:6e:64 (SSID='VirtWifi' freq=5240 MHz)"
    DONE = "wlan0: CTRL-EVENT-CONNECTED - Connection to 62:eb:c4:11:6e:64 completed"

    def __init__(self, tooks: list[float | None], console: str = "", **answers: list[str]) -> None:
        super().__init__(status=[OFF, JOINED], **answers)
        self.tooks = list(tooks)
        self.console = console
        self.attempt = -1
        self.shells: list[str] = []
        self.suspends: list[dict] = []
        self.joined_after = True
        self.request_after = REQUEST_OUTPUT

    def adb_run(self, *arguments: str, timeout: float = 0) -> object:
        self.shells.append(arguments[-1])
        status = 0 if self.tooks else 124
        return types.SimpleNamespace(returncode=0, stdout=f"status={status}\\n".encode())

    def mark_log(self) -> str:
        self.attempt += 1
        return WAKE_MARK

    def prepare_suspend(self) -> None:
        self.calls.append("prepare")

    def suspend_and_wake(self, watch: bool = False, frozen: Callable[[], None] | None = None,
                         prepared: bool = False, stay: float = 0) -> None:
        self.calls.append("suspend")
        self.suspends.append({"watch": watch, "frozen": frozen is not None, "prepared": prepared, "stay": stay})
        if frozen is not None:
            frozen()

    def log_text(self) -> str:
        took = self.tooks[min(self.attempt, len(self.tooks) - 1)] if self.tooks else None
        log = log_line(2270, 2270, "I", runner.SYNC_TAG, WAKE_MARK, "03:10:00.000")
        log += wpa_line(self.START, "03:10:05.000")
        if took is not None:
            seconds = 5 + took
            log += wpa_line(self.DONE, f"03:{10 + int(seconds) // 60}:{int(seconds) % 60:02d}.{int(seconds * 1000) % 1000:03d}")
        return log

    def log_size(self) -> int:
        return 0

    def log_since(self, offset: int) -> str:
        return self.console

    def joined(self) -> bool:
        return self.joined_after

    def wake_request(self) -> str:
        return self.request_after

    def status(self) -> str:
        return OFF if not self.calls.count("cmd wifi set-wifi-enabled enabled") else JOINED


class ConnectDelayRowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.clock = FakeClock()
        patch = mock.patch.object(runner, "time", self.clock)
        patch.start()
        self.addCleanup(patch.stop)
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def row(self, guest: ConnectDelayGuest) -> str | None:
        return runner.check_suspend_in_connect_delay(guest)

    def test_a_suspend_that_lands_in_the_delay_and_ends_with_a_working_network_passes(self) -> None:
        guest = ConnectDelayGuest([64.2])
        note = self.row(guest)
        self.assertEqual(note, "the station joined and a request works after a suspend 1 attempt(s) in, the connect "
                               "held 64 s across the stay, the console quiet")
        # Wi-Fi off first, then on in the command that waits for the supplicant's connect, then the suspend, which
        # waits for nothing else: the log stream was confirmed before, so the suspend follows the connect start at once.
        self.assertEqual(guest.calls, ["cmd wifi set-wifi-enabled disabled", "prepare", "suspend"])
        self.assertEqual(guest.suspends, [{"watch": False, "frozen": True, "prepared": True,
                                           "stay": runner.CONNECT_STAY_SECONDS}])
        self.assertIn("cmd wifi set-wifi-enabled enabled; timeout 60 logcat -T 1 -m 1 -e 'Trying to associate with'",
                      guest.shells[0])

    def test_the_stay_is_longer_than_a_bss_that_nothing_holds_lives(self) -> None:
        # cfg80211 forgets an unheld BSS after 30 s; the stay is twice that.
        self.assertGreaterEqual(runner.CONNECT_STAY_SECONDS, 2 * 30)

    def test_a_suspend_that_missed_the_delay_is_tried_again(self) -> None:
        guest = ConnectDelayGuest([2.07, 64.2])
        self.assertIn("2 attempt(s) in", self.row(guest))
        self.assertEqual(len(guest.suspends), 2)

    def test_a_suspend_that_never_lands_in_the_delay_proved_nothing_and_fails(self) -> None:
        guest = ConnectDelayGuest([2.07])
        with self.assertRaisesRegex(runner.Failure, "did not land inside the connect delay in 3 attempts"):
            self.row(guest)
        self.assertEqual(len(guest.suspends), runner.CONNECT_ATTEMPTS)

    def test_a_station_that_stays_not_joined_fails_the_row(self) -> None:
        guest = ConnectDelayGuest([64.2])
        guest.joined_after = False
        with self.assertRaisesRegex(runner.Failure, "the station is joined and a request works: the station is not "
                                                    r"joined \(after 120 s\)"):
            self.row(guest)
        self.assertEqual(len(guest.suspends), 1)

    def test_a_station_that_says_joined_with_no_working_network_fails_the_row(self) -> None:
        guest = ConnectDelayGuest([64.2])
        guest.request_after = "request t=1000.5 ping_rc=2 dns_tcp_bytes=0\n"
        with self.assertRaisesRegex(runner.Failure, "the framework says the station is joined, but the gateway did "
                                                    "not answer a ping"):
            self.row(guest)

    def test_a_kernel_warning_and_a_call_trace_fail_the_row_although_the_network_works(self) -> None:
        guest = ConnectDelayGuest([64.2], console=(
            "[  139.25][   T9] WARNING: CPU: 0 PID: 9 at net/wireless/sme.c:845 __cfg80211_connect_result+0x1c\n"
            "[  139.26][   T9] Call Trace:\n"))
        with self.assertRaisesRegex(runner.Failure, r"the guest console says: \[  139.25\]\[   T9\] WARNING: CPU: 0.*"
                                                    "the guest console says: .*Call Trace:"):
            self.row(guest)

    def test_a_supplicant_that_never_begins_to_connect_fails_the_row(self) -> None:
        guest = ConnectDelayGuest([])
        with self.assertRaisesRegex(runner.Failure, "the supplicant did not begin to connect within 60 s"):
            self.row(guest)
        self.assertEqual(guest.suspends, [])

    def test_the_row_is_the_last_of_the_suspend_sections(self) -> None:
        names: list[str] = []

        class Report:
            def section(self, name: str, guest: object, body: object) -> None:
                names.append(name)

        runner.suspend_sections(object(), Report())
        self.assertEqual(len(names), 4, names)
        self.assertTrue(names[-1].startswith("a stay with the emulator stopped inside the station's connect delay"))


if __name__ == "__main__":
    unittest.main()
