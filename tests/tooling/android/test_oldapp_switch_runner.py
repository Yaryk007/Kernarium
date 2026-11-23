#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Old app compatibility runner without a guest.

run_android_oldapp_switch.py drives the image's `kernarium_oldapp` command and checks the property's policy
from an ordinary application. Here a scripted guest plays an image that behaves as the contract says, and
each way the image can differ makes one step fail: the probe packages (manifests, the stand-in library, the
probe's scripts), the parsers of the listing and of the probe's result, and every step against the scripted
guest, which keeps installed packages with their first-install times, processes, and the layer's mapping.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import contextlib
import io
import pathlib
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
import unittest.mock
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
INTEGRATION = HERE.parents[1] / "integration" / "android"
sys.path.insert(0, str(INTEGRATION))
import oldswitch_probe_apk as probe  # noqa: E402
import orientation_probe_apk as binary_xml  # noqa: E402
import permission_review as review  # noqa: E402
import property_probe_dex  # noqa: E402
import run_android_oldapp_switch as runner  # noqa: E402

Failure = runner.Failure
KEY_PART = re.compile(r"[A-Za-z0-9_@:-]+(\.[A-Za-z0-9_@:-]+)*")
HOOK_DOMAIN = "u:r:untrusted_app_25:s0:c512,c768"
#: The two denials of an application's `setprop` (see run_android_oldapp_switch.HOOK_DOMAIN): a user build refuses it at
#: the property service's socket; a domain that may reach the socket is refused by the property type's rule.
SOCKET_DENIAL = ('11-15 17:00:{second:02d}.000  {pid}  {pid} I auditd  : type=1400 audit(0.0:{serial}): avc:  denied  '
                 '{{ write }} for  comm="setprop" name="property_service" dev="tmpfs" ino=460 scontext={domain} '
                 'tcontext=u:object_r:property_socket:s0 tclass=sock_file permissive=0')
PROPERTY_DENIAL = ('11-15 17:00:{second:02d}.000   500   500 W auditd : avc:  denied  {{ set }} for property={name} '
                   'pid={pid} uid=10200 gid=10200 scontext={domain} '
                   'tcontext=u:object_r:kernarium_oldapp_prop:s0 tclass=property_service permissive=0')
NOISE = ('11-15 16:59:59.000   591   591 I auditd  : type=1400 audit(0.0:1): avc:  denied  {{ getopt }} for '
         'comm="binder:591_D" scontext=u:r:system_server:s0 tcontext=u:r:shell:s0 tclass=unix_stream_socket permissive=0')


class ScriptedGuest:
    """An image that follows the contract: packages with first-install times, processes, and the layer's
    mapping. Attributes switch one way of differing on."""

    def __init__(self) -> None:
        self.properties: dict[str, str] = {}
        self.clock = 1_700_000_000
        self.packages: dict[str, tuple[str, int]] = {}   # package -> (variant, first-install milliseconds)
        self.counter = 0
        self.processes: dict[str, int] = {}
        self.results: dict[str, str] = {}
        self.next_pid = 4000
        self.commands: list[str] = []
        self.real = {"real": ("org.example.real", "Real Game")}
        # Ways to differ.
        self.list_extra: list[str] = []
        self.label: str | None = None
        self.refuse_with = 2
        self.app_can_set = False
        self.app_view_value: str | None = None
        self.domain = "u:r:untrusted_app_29:s0:c512,c768"
        self.hook_domain = HOOK_DOMAIN
        self.shell_context = "u:r:shell:s0"
        self.path = runner.WRAPPER_PATH
        self.keep_on_off = True
        self.result_file: str | None = None
        self.version_line = runner.VERSION_LINE
        self.on_time_offset = 0
        # Rules of the cleanup that the image lacks: absent, ineligible (the rule of the installation still runs),
        # reinstalled, foreign.
        self.skip_cleanup: set[str] = set()
        self.lock_type = "shell_data_file"
        self.lock_mode = "-rw-------"
        self.tightens_lock = True
        self.list_tightens_lock = True
        self.off_needs_eligible = False
        self.accept_unrepresentable = False
        self.layer = "when-on"                    # when-on, never, always
        self.keep_process = False                 # set does not end the application's process
        self.running_can_set = False
        self.avc_form = "socket"                  # socket, property, none: what the application's refused set leaves
        self.logcat_reads = 0
        self.logcat_fails = lambda read: False    # whether the nth read of the log (1-based) fails
        self.denials: list[str] = [NOISE, SOCKET_DENIAL.format(second=1, pid=3999, serial=2, domain=HOOK_DOMAIN)]
        self.updated: dict[str, int] = {}         # package -> last-update epoch seconds
        self.in_place = True                      # `install -r` over an installed package of the same key updates it
        self.review_row = 7                       # the permission row that carries the review flag, after this many
        self.flag_outside_block = False           # a REVIEW_REQUIRED outside the runtime permissions block
        self.review_required = True               # an application that targets below API 23 gets the permission review
        self.review: set[str] = set()
        self.review_open: str | None = None
        self.review_button = True                 # the Continue button is on the review screen
        self.ui = ""
        for variant in probe.VARIANTS:
            self.install_variant(variant)

    # ---- the model
    def first_install(self) -> int:
        """Epoch milliseconds, unique per installation whatever the second."""
        self.counter += 1
        return self.clock * 1000 + self.counter

    def install_variant(self, variant: str, package: str | None = None) -> None:
        self.packages[package or probe.package_of(variant)] = (variant, self.first_install())
        self.updated[package or probe.package_of(variant)] = self.clock
        self.processes.pop(package or probe.package_of(variant), None)
        if self.review_required and probe.target_of(variant) < 23:
            self.review.add(package or probe.package_of(variant))
        else:
            self.review.discard(package or probe.package_of(variant))

    def eligible(self, package: str) -> bool:
        if package not in self.packages:
            return False
        variant = self.packages[package][0]
        if variant == "real":
            return True
        _, _, target, native, shared, _ = probe.SPECS[variant]
        return target < 14 and native and shared is None and KEY_PART.fullmatch(package) is not None

    def label_of(self, package: str) -> str:
        variant = self.packages[package][0]
        if variant == "real":
            return self.real["real"][1]
        if self.label is not None:
            return self.label
        return (probe.LISTED_LABEL if variant in ("old", "raised", "oldshared") else f"Switch Probe {variant}")

    def stored(self) -> dict[str, str]:
        return {name[len(probe.STORE_PROPERTY_PREFIX):]: value for name, value in self.properties.items()
                if name.startswith(probe.STORE_PROPERTY_PREFIX) and value}

    def names_installation(self, package: str, value: str) -> bool:
        match = re.fullmatch(r"([0-9]+) ([0-9]+)", value)
        return match is not None and int(match.group(2)) == self.packages[package][1]

    def clear_stale(self) -> bool:
        """What the listing does about stale choices; it takes the lock, and makes the file safe, only when it
        has one to clear. Returns whether it cleared any."""
        cleared = False
        for package, value in self.stored().items():
            if package not in self.packages:
                cause = "absent"
            elif not self.eligible(package) and "ineligible" not in self.skip_cleanup:
                cause = "ineligible"
            elif not self.names_installation(package, value):
                cause = "reinstalled" if re.fullmatch(r"[0-9]+ [0-9]+", value) else "foreign"
            else:
                continue
            if cause not in self.skip_cleanup:
                self.properties[probe.STORE_PROPERTY_PREFIX + package] = ""
                cleared = True
        if cleared and self.list_tightens_lock:
            self.lock_mode = "-rw-------"
        return cleared

    def adb_run(self, *arguments: str, timeout: float = 30):
        self.commands.append("adb " + " ".join(arguments))
        if arguments[:1] == ("shell",):
            result = self.run(arguments[1], timeout)
            return types.SimpleNamespace(returncode=result.status, stdout=result.out, stderr=result.err)
        refused = None
        if arguments[:1] == ("install",):
            variant = pathlib.Path(arguments[-1]).stem
            self.clock += 3
            if variant in self.real:
                self.packages[self.real[variant][0]] = ("real", self.first_install())
            else:
                shared = probe.SPECS[variant][4]
                holder = next((package for package, (other, _) in self.packages.items()
                               if shared is not None and probe.SPECS.get(other, (None,) * 5)[4] == shared
                               and package != probe.package_of(variant)), None)
                if holder is not None:
                    # Each probe is signed with a key of its own, so a second package of the user ID is refused.
                    refused = ("Failure [INSTALL_FAILED_SHARED_USER_INCOMPATIBLE: Reconciliation failed...: "
                               f"Package {probe.package_of(variant)} has no signatures that match those in shared "
                               f"user {shared}; ignoring!]")
                elif (package := probe.package_of(variant)) in self.packages:
                    held = self.packages[package][0]
                    if "-r" not in arguments:
                        refused = (f"Failure [INSTALL_FAILED_ALREADY_EXISTS: Attempt to re-install {package} without "
                                   "first uninstalling.]")
                    elif not probe.same_key(held, variant):
                        refused = (f"Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package {package} signatures do "
                                   "not match previously installed version; ignoring!]")
                    elif self.in_place:
                        # An update: the package manager keeps the installation and its first-install time.
                        self.packages[package] = (variant, self.packages[package][1])
                        self.updated[package] = self.clock
                        self.processes.pop(package, None)
                        if probe.target_of(variant) >= 23:
                            self.review.discard(package)
                    else:
                        self.install_variant(variant)
                else:
                    self.install_variant(variant)
        elif arguments[:1] == ("uninstall",):
            self.packages.pop(arguments[1], None)
            self.processes.pop(arguments[1], None)
            self.review.discard(arguments[1])
        if refused is not None:
            return types.SimpleNamespace(returncode=1, stdout=refused + "\n", stderr="")
        return types.SimpleNamespace(returncode=0, stdout="Success\n", stderr="")

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {result.err}")
        return result.out

    def run(self, command: str, timeout: float = 120) -> runner.Run:
        self.commands.append(command)
        words = shlex.split(command)
        if words[:1] == ["date"]:
            self.clock += 1
            return runner.Run(0, f"{self.clock}\n", "")
        if words[:1] == ["id"]:
            return runner.Run(0, self.shell_context + "\n", "")
        if words[:3] == ["ls", "-lZ", runner.LOCK_PATH]:
            return runner.Run(0, f"{self.lock_mode} 1 shell shell u:object_r:{self.lock_type}:s0 0 2026-11-15 17:00 {runner.LOCK_PATH}\n", "")
        if words[:1] == ["chmod"]:
            self.lock_mode = "-rw-rw-rw-" if words[1] == "666" else "-rw-------"
            return runner.Run(0, "", "")
        if words[:1] == ["setprop"]:
            self.properties[words[1]] = words[2]
            return runner.Run(0, "", "")
        if words[:1] == ["ls"]:
            return runner.Run(0, "\n".join(f"-rwxr-xr-x 1 root shell u:object_r:system_file:s0 1 {path}"
                                           for path in words[2:]) + "\n", "")
        if words[:2] == ["command", "-v"]:
            return runner.Run(0, self.path + "\n", "")
        if words[:1] == ["getprop"]:
            return runner.Run(0, self.properties.get(words[1], "") + "\n", "")
        if words[:1] == ["pidof"]:
            pid = self.processes.get(words[1])
            return runner.Run(0 if pid else 1, f"{pid}\n" if pid else "", "")
        if words[:1] == ["logcat"]:
            self.logcat_reads += 1
            if self.logcat_fails(self.logcat_reads):
                return runner.Run(1, "", "logcat: read failed\n")
            return runner.Run(0, "\n".join(self.denials) + "\n", "")
        if words[:2] == ["dumpsys", "package"]:
            return runner.Run(0, self.package_dump(words[2]), "")
        if words[:4] == ["am", "start", "-W", "-a"]:
            self.review_open = words[-1] if words[-1] in self.review else None
            return runner.Run(0, "Status: ok\n", "")
        if words[:2] == ["uiautomator", "dump"]:
            button = ('<node resource-id="com.android.permissioncontroller:id/continue_button" '
                      'bounds="[100,200][300,260]" />' if self.review_open and self.review_button else "")
            self.ui = f'<?xml version="1.0"?><hierarchy rotation="0"><node resource-id="x" bounds="[0,0][720,1280]">{button}</node></hierarchy>'
            return runner.Run(0, "UI hierarchy dumped\n", "")
        if words[:1] == ["cat"] and words[1:] == [review.UI_DUMP]:
            return runner.Run(0, self.ui, "")
        if words[:2] == ["input", "tap"]:
            if self.review_open and (int(words[2]), int(words[3])) == (200, 230):
                self.review.discard(self.review_open)
                self.review_open = None
            return runner.Run(0, "", "")
        if words[:3] == ["am", "broadcast", "-n"]:
            return self.broadcast(words[3])
        if words[:1] == ["run-as"]:
            if words[2] == "rm":
                self.results.pop(words[1], None)
                return runner.Run(0, "", "")
            if self.result_file is not None:
                return runner.Run(0, self.result_file, "")
            return runner.Run(0, self.results.get(words[1], ""), "")
        if words[:1] == [runner.COMMAND]:
            return self.command(words[1:])
        raise AssertionError(f"unscripted command: {command}")

    def package_dump(self, package: str) -> str:
        """`dumpsys package PACKAGE`: the times, and the user's runtime permissions block, where the review flag is
        on one row of several (after `review_row` others) while the review is pending."""
        if package not in self.packages:
            return "Packages:\n"
        stamp = lambda seconds: time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(seconds))  # noqa: E731
        rows = [f"        android.permission.LEGACY_{index}: granted=true, flags=[ ]"
                for index in range(self.review_row)]
        if package in self.review:
            rows.append("        android.permission.POST_NOTIFICATIONS: granted=true, flags=[ REVIEW_REQUIRED]")
        rows.append("        android.permission.ACCESS_LOCAL_NETWORK: granted=true, flags=[ ]")
        lines = ["Packages:", f"  Package [{package}] (abc123):", "    userId=10200",
                 f"    lastUpdateTime={stamp(self.updated.get(package, self.clock))}",
                 f"    User 0: ceDataInode=1 installed=true hidden=false",
                 f"      firstInstallTime={stamp(self.packages[package][1] // 1000)}", "      gids=[3003]",
                 "      runtime permissions:", *rows, "", "Queries:", "  system apps queryable: false"]
        if self.flag_outside_block:
            lines.append("  flags=[ REVIEW_REQUIRED] (not a permission of the package)")
        return "\n".join(lines) + "\n"

    def broadcast(self, component: str) -> runner.Run:
        package = component.split("/")[0]
        variant = self.packages[package][0]
        if package not in self.processes:
            self.next_pid += 1
            self.processes[package] = self.next_pid
        if package in self.review:
            # The permission review comes first: the broadcast is skipped and no component runs.
            self.processes.pop(package, None)
            return runner.Run(0, "Broadcast completed: result=0\n", "")
        name = probe.STORE_PROPERTY_PREFIX + (package if variant == "running" else probe.package_of("old"))
        value = self.properties.get(name, "")
        if variant == "running":
            mapped = {"when-on": bool(value), "never": False, "always": True}[self.layer]
            after = "7" if self.running_can_set else value
            self.next_pid += 1
            setpid = self.next_pid
            self.results[package] = (f"context={self.hook_domain}\npid={self.processes[package]}\n"
                                     f"layer={1 if mapped else 0}\nget={value}\n"
                                     f"set={'allowed' if self.running_can_set else 'denied'}\nsetpid={setpid}\n"
                                     f"after={after}\ndone\n")
            domain, can_set = self.hook_domain, self.running_can_set
        else:
            after = "7" if self.app_can_set else value
            self.results[package] = (f"context={self.domain}\nget={value if self.app_view_value is None else self.app_view_value}\n"
                                     f"set={'allowed' if self.app_can_set else 'denied'}\nafter={after}\ndone\n")
            domain, can_set = self.domain, self.app_can_set
            self.next_pid += 1
            setpid = self.next_pid
        if not can_set and self.avc_form != "none":
            template = SOCKET_DENIAL if self.avc_form == "socket" else PROPERTY_DENIAL
            self.denials.append(template.format(second=len(self.denials) % 60, pid=setpid,
                                                serial=len(self.denials) + 1, domain=domain, name=name))
        return runner.Run(0, "Broadcast completed: result=0\n", "")

    def command(self, arguments: list[str]) -> runner.Run:
        usage = runner.Run(64, "", "usage: kernarium_oldapp list\n       kernarium_oldapp set PACKAGE on|off\n")
        if arguments == ["list"]:
            self.clear_stale()
            lines = [self.version_line]
            for package in sorted(self.packages):
                if self.eligible(package):
                    value = self.properties.get(probe.STORE_PROPERTY_PREFIX + package, "")
                    valid = value and self.names_installation(package, value)
                    seconds = value.split(" ")[0] if valid else "0"
                    lines.append(f"{package}\t{self.label_of(package)}\t{'on' if valid else 'off'}\t{seconds}\t"
                                 f"{self.packages[package][1] // 1000}")
            lines += self.list_extra
            return runner.Run(0, "\n".join(lines) + "\n", "")
        if len(arguments) == 3 and arguments[0] == "set" and arguments[2] in ("on", "off"):
            package, mode = arguments[1], arguments[2]
            if self.tightens_lock and self.lock_mode != "-rw-------":
                self.lock_mode = "-rw-------"
            refused = runner.Run(self.refuse_with, "", f"kernarium_oldapp: {package} is not an application it applies to\n")
            if KEY_PART.fullmatch(package) is None and not self.accept_unrepresentable:
                return refused
            if mode == "on" and not self.eligible(package):
                return refused
            if mode == "off" and self.off_needs_eligible and not self.eligible(package):
                return refused
            name = probe.STORE_PROPERTY_PREFIX + package
            if mode == "on":
                self.properties[name] = f"{self.clock + self.on_time_offset} {self.packages[package][1]}"
            elif self.keep_on_off and self.properties.get(name):
                self.properties[name] = ""
            if package in self.packages and not self.keep_process:
                self.processes.pop(package, None)
            return runner.Run(0, f"{package}\t{mode}\n", "")
        return usage


class ProbePackages(unittest.TestCase):
    def test_variants_differ_in_one_clause_each(self) -> None:
        old = probe.VARIANTS["old"]
        self.assertEqual((old[2], old[3], old[4]), (10, True, None))
        self.assertEqual(probe.VARIANTS["modern"][2], 29)
        self.assertTrue(probe.VARIANTS["modern"][3])
        self.assertEqual(probe.VARIANTS["javaonly"][2:5], (10, False, None))
        self.assertEqual(probe.VARIANTS["shared"][2:4], (10, True))
        self.assertIsNotNone(probe.VARIANTS["shared"][4])
        self.assertEqual(set(probe.VARIANTS), set(probe.ELIGIBLE) | set(probe.REFUSED))

    def test_successors_replace_old_under_its_name(self) -> None:
        for variant in probe.SUCCESSORS:
            self.assertEqual(probe.package_of(variant), probe.package_of("old"))
        self.assertEqual(probe.SUCCESSORS["raised"][2], 29)
        self.assertIsNone(probe.SUCCESSORS["raised"][4])
        self.assertEqual(probe.SUCCESSORS["oldshared"][2:5], (10, True, probe.PACKAGE_PREFIX + ".shareduid"))
        running = probe.RUNNING["running"]
        self.assertEqual((running[2], running[3], running[4], running[5]), (10, True, None, True))
        self.assertNotIn(probe.package_of("running"), {probe.package_of(v) for v in probe.VARIANTS})

    def test_manifests_read_back(self) -> None:
        for variant in probe.SPECS:
            tree = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree(variant)))
            package, minimum, target, _, shared, receiver = probe.SPECS[variant]
            self.assertEqual(tree.attributes["package"], package)
            self.assertEqual(tree.attributes.get("sharedUserId"), shared)
            sdk = next(node for node in tree.children if node.tag == "uses-sdk")
            self.assertEqual(sdk.attributes, {"minSdkVersion": minimum, "targetSdkVersion": target})
            application = next(node for node in tree.children if node.tag == "application")
            if variant in ("old", "raised", "oldshared"):
                self.assertEqual(application.attributes["label"], probe.LABEL)
            else:
                self.assertEqual(application.attributes["label"], f"Switch Probe {variant}")
            self.assertEqual(application.attributes.get("debuggable") is True, receiver)
            # Only the native-only fixtures declare no code.
            self.assertEqual(application.attributes.get("hasCode") is False, variant in probe.NATIVE_ONLY, variant)
            self.assertEqual([node.attributes["name"] for node in application.children],
                             [probe.RECEIVER] if receiver else [])

    def test_a_receiver_holds_internet(self) -> None:
        """The probe class lists the network interfaces first: without the permission the list is null and it dies."""
        for variant in probe.SPECS:
            tree = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree(variant)))
            permissions = [node.attributes["name"] for node in tree.children if node.tag == "uses-permission"]
            receiver = probe.SPECS[variant][5]
            self.assertEqual(permissions, ["android.permission.INTERNET"] if receiver else [], variant)

    def test_label_has_a_tab_the_listing_turns_into_a_space(self) -> None:
        self.assertIn("\t", probe.LABEL)
        self.assertEqual(probe.LABEL.replace("\t", " "), probe.LISTED_LABEL)

    def test_stand_in_library_is_an_arm_elf32_shared_object(self) -> None:
        image = probe.stand_in_library()
        self.assertEqual(image[:7], b"\x7fELF\x01\x01\x01")
        e_type, machine, _, _, phoff, _, flags, ehsize, phentsize, phnum = struct.unpack_from("<HHIIIIIHHH", image, 16)
        self.assertEqual((e_type, machine, ehsize, phentsize, phnum), (3, 40, 52, 32, 2))
        self.assertEqual(flags >> 24, 5, "EABI version 5")
        kinds = [struct.unpack_from("<I", image, phoff + 32 * index)[0] for index in range(phnum)]
        self.assertEqual(kinds, [1, 2])
        _, offset, _, _, filesz, memsz, _, alignment = struct.unpack_from("<IIIIIIII", image, phoff)
        self.assertEqual((offset, filesz, memsz), (0, len(image), len(image)))
        self.assertEqual(alignment % 0x4000, 0, "aligned for a 16 KiB page as well")
        dynamic_offset, dynamic_size = struct.unpack_from("<II", image, phoff + 32 + 4)[0], struct.unpack_from(
            "<I", image, phoff + 32 + 16)[0]
        self.assertEqual(image[dynamic_offset:dynamic_offset + dynamic_size], bytes(8), "DT_NULL")

    def test_probe_script_reads_then_tries_to_set_the_eligible_property(self) -> None:
        name = probe.property_of("old")
        script = probe.probe_script(probe.package_of("modern"), name)
        self.assertIn(f"getprop {name}", script)
        self.assertIn(f"setprop {name} 7", script)
        self.assertLess(script.index("get="), script.index("setprop"))
        self.assertLess(script.index("setprop"), script.index("after="))
        self.assertIn(f"/data/data/{probe.package_of('modern')}/files", script)
        dex = property_probe_dex.build_dex(probe.package_of("modern"), name, name, shell_script=script)
        self.assertIn(script.encode(), dex)
        self.assertNotIn(property_probe_dex.script(probe.package_of("modern"), name, name).encode(), dex)
        default = property_probe_dex.build_dex(probe.package_of("modern"), name, name)
        self.assertIn(property_probe_dex.script(probe.package_of("modern"), name, name).encode(), default)

    def test_running_script_reports_the_process_and_the_layer(self) -> None:
        name = probe.property_of("running")
        script = probe.running_script(probe.package_of("running"), name)
        # The shell's parent is the application's process: its id and its memory map.
        self.assertIn("pid=$PPID", script)
        self.assertIn(f"grep -c {probe.LAYER_LIBRARY} /proc/$PPID/maps", script)
        self.assertEqual(probe.LAYER_LIBRARY, "libkernarium_oldapp.so")
        self.assertIn(f"getprop {name}", script)
        self.assertIn(f"setprop {name} 7", script)
        self.assertLess(script.index("get="), script.index("setprop"))
        self.assertLess(script.index("setprop"), script.index("after="))
        self.assertTrue(script.rstrip().endswith("mv $d/result.tmp $d/result"))

    def test_running_script_reports_the_process_of_the_set(self) -> None:
        """The log's denial names the `setprop` process, which is not the application's: the script starts the set in
        the background to know its id, waits for it, and reports the id with the outcome."""
        script = probe.running_script(probe.package_of("running"), probe.property_of("running"))
        self.assertIn("& p=$!", script)
        self.assertIn("wait $p", script)
        self.assertIn('echo "setpid=$p"', script)
        self.assertLess(script.index("setprop"), script.index("wait $p"))
        self.assertLess(script.index("wait $p"), script.index("setpid="))
        self.assertLess(script.index("setpid="), script.index("after="))

    def test_old_and_its_update_share_one_key_and_no_other_fixture_does(self) -> None:
        self.assertTrue(probe.same_key("old", "raised") and probe.same_key("raised", "old"))
        self.assertTrue(probe.same_key("old", "old"))
        for other in set(probe.SPECS) - {"old", "raised"}:
            self.assertFalse(probe.same_key("old", other), other)
            self.assertFalse(probe.same_key(other, "raised"), other)
        try:
            with tempfile.TemporaryDirectory(prefix="uml-oldswitch-test-") as scratch:
                directory = pathlib.Path(scratch)
                keystore = directory / "family.p12"
                certificates = {}
                for variant in ("old", "raised", "modern"):
                    built = probe.build_apk(directory / f"{variant}.apk", variant, keystore=keystore)
                    printed = subprocess.run(["keytool", "-printcert", "-jarfile", str(built)], text=True,
                                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT, timeout=120, check=True).stdout
                    certificates[variant] = re.search(r"SHA256:\s*(\S+)", printed).group(1)
        except FileNotFoundError:
            self.skipTest("keytool and jarsigner are not on PATH")
        self.assertEqual(certificates["old"], certificates["raised"])
        self.assertNotEqual(certificates["old"], certificates["modern"],
                            "a fixture outside the family keeps its own key")

    def test_signed_packages_hold_the_files_the_manifest_needs(self) -> None:
        """Android 17 refuses a package that declares code and has no classes.dex ("code is missing"): each fixture
        either declares no code or carries a DEX file, and the ones that start a process carry the receiver."""
        try:
            with tempfile.TemporaryDirectory(prefix="uml-oldswitch-test-") as scratch:
                directory = pathlib.Path(scratch)
                packages = {}
                for variant in probe.SPECS:
                    with zipfile.ZipFile(probe.build_apk(directory / f"{variant}.apk", variant)) as built:
                        packages[variant] = (built.namelist(), binary_xml.decode_manifest(built.read("AndroidManifest.xml")))
        except FileNotFoundError:
            self.skipTest("keytool and jarsigner are not on PATH")
        for variant in ("old", "modern", "shared", "raised", "oldshared", "running"):
            self.assertIn(probe.LIBRARY, packages[variant][0], variant)
        self.assertNotIn(probe.LIBRARY, packages["javaonly"][0])
        for variant, (names, manifest) in packages.items():
            application = next(node for node in manifest.children if node.tag == "application")
            declares_code = application.attributes.get("hasCode", True) is not False
            self.assertEqual(declares_code, "classes.dex" in names,
                             f"{variant}: a package declares code exactly when it carries classes.dex")
        for variant in probe.NATIVE_ONLY:
            self.assertNotIn("classes.dex", packages[variant][0], variant)
        for variant in ("modern", "running", "javaonly"):
            self.assertIn("classes.dex", packages[variant][0], variant)
        self.assertEqual(set(probe.NATIVE_ONLY) | set(probe.JAVA_ONLY) | {"modern", "running"}, set(probe.SPECS))

    def test_a_java_only_fixture_has_no_component_that_names_its_code(self) -> None:
        tree = binary_xml.decode_manifest(binary_xml.encode_manifest(probe.manifest_tree("javaonly")))
        application = next(node for node in tree.children if node.tag == "application")
        self.assertEqual(application.children, [])
        self.assertNotIn("debuggable", application.attributes)


class PermissionReview(unittest.TestCase):
    """permission_review.py, which the runner shares with the others that start an old application's process."""

    def test_continue_button_is_found_by_its_resource_id(self) -> None:
        ui = ('<hierarchy><node resource-id="com.android.permissioncontroller:id/continue_button" '
              'bounds="[10,20][110,60]"/></hierarchy>')
        self.assertEqual(review.continue_button(ui), (60, 40))
        self.assertIsNone(review.continue_button('<hierarchy><node resource-id="x:id/cancel_button" bounds="[1,2][3,4]"/></hierarchy>'))
        self.assertIsNone(review.continue_button("not xml"))
        self.assertIsNone(review.continue_button(
            '<hierarchy><node resource-id="x:id/continue_button" bounds="nothing"/></hierarchy>'))

    def test_no_review_pending_presses_nothing(self) -> None:
        guest = ScriptedGuest()
        self.assertEqual(review.complete_review(guest, probe.package_of("modern"), Failure), "no review pending")
        self.assertFalse(any(command.startswith("input") or command.startswith("am start") for command in guest.commands))

    def test_a_pending_review_is_completed_or_reported_with_the_callers_failure(self) -> None:
        with unittest.mock.patch("time.sleep"), unittest.mock.patch.object(review, "REVIEW_SECONDS", 1):
            guest = ScriptedGuest()
            package = probe.package_of("old")
            self.assertIn(package, guest.review)
            self.assertEqual(review.complete_review(guest, package, Failure), "permission review completed")
            self.assertNotIn(package, guest.review)
            guest.install_variant("old")
            guest.review_button = False
            with self.assertRaises(Failure):
                review.complete_review(guest, package, Failure)
            with self.assertRaises(RuntimeError):
                review.complete_review(guest, package)


    def test_the_whole_runtime_permissions_block_is_read(self) -> None:
        """The flag may be on any row of the package's block, however many rows precede it; it counts nowhere
        else, and a block ends at the first line that is not deeper than its header."""
        def dump(*, rows: int, flagged: int | None, header: str = "      runtime permissions:",
                 after: tuple[str, ...] = ()) -> str:
            lines = ["  Package [org.example.old] (abc):", "    User 0: installed=true", header]
            for index in range(rows):
                flag = " REVIEW_REQUIRED" if index == flagged else ""
                lines.append(f"        android.permission.P{index}: granted=true, flags=[{flag} ]")
            return "\n".join([*lines, *after, "    other: x"]) + "\n"

        class Dumping:
            def __init__(self, text: str) -> None:
                self.text, self.commands = text, []

            def shell(self, command: str, timeout: float = 30) -> str:
                self.commands.append(command)
                return self.text

        for rows, flagged in ((1, 0), (5, 4), (12, 11), (60, 59), (60, 0)):
            guest = Dumping(dump(rows=rows, flagged=flagged))
            self.assertTrue(review.review_pending(guest, "org.example.old"), (rows, flagged))
            self.assertEqual(guest.commands, ["dumpsys package org.example.old"])
        self.assertFalse(review.review_pending(Dumping(dump(rows=60, flagged=None)), "org.example.old"))
        # Two users: the flag in the second user's block counts.
        self.assertTrue(review.review_pending(Dumping(
            dump(rows=3, flagged=None) + dump(rows=9, flagged=8, header="      runtime permissions:")),
            "org.example.old"))
        # Outside a runtime permissions block the flag is another line's.
        self.assertFalse(review.review_pending(Dumping(
            dump(rows=3, flagged=None, after=("      flags=[ REVIEW_REQUIRED]",))), "org.example.old"))
        self.assertFalse(review.review_pending(Dumping(
            "    install permissions:\n      android.permission.INTERNET: granted=true REVIEW_REQUIRED\n"),
            "org.example.old"))
        # A blank line inside the block does not end it; an equally indented line does.
        self.assertTrue(review.review_pending(Dumping(
            "    runtime permissions:\n      a: granted=true, flags=[ ]\n\n      b: flags=[ REVIEW_REQUIRED]\n"),
            "org.example.old"))
        self.assertFalse(review.review_pending(Dumping(
            "    runtime permissions:\n      a: flags=[ ]\n    next:\n      b: flags=[ REVIEW_REQUIRED]\n"),
            "org.example.old"))
        self.assertEqual(review.runtime_permission_blocks("nothing\n"), [])

    def test_a_flag_after_many_rows_still_gets_the_review_completed(self) -> None:
        with unittest.mock.patch("time.sleep"), unittest.mock.patch.object(review, "REVIEW_SECONDS", 1):
            guest = ScriptedGuest()
            guest.review_row = 40
            package = probe.package_of("old")
            self.assertEqual(review.complete_review(guest, package, Failure), "permission review completed")
            self.assertNotIn(package, guest.review)
            guest.install_variant("old")
            guest.flag_outside_block = True
            guest.review.discard(package)
            self.assertEqual(review.complete_review(guest, package, Failure), "no review pending")


class Parsers(unittest.TestCase):
    def test_listing(self) -> None:
        rows = runner.parse_listing("kernarium_oldapp 1\na.b\tA B\toff\t0\t5\nc.d\tC\ton\t17\t9\textra\n")
        self.assertEqual(rows, [runner.Row("a.b", "A B", "off", 0, 5), runner.Row("c.d", "C", "on", 17, 9)])
        self.assertEqual(runner.parse_listing("kernarium_oldapp 1\n"), [])
        for bad in ("", "kernarium_oldapp 2\n", "kernarium_oldapp 1", "kernarium_oldapp 1\na\tb\tc\n",
                    "kernarium_oldapp 1\na\tb\tmaybe\t0\t1\n", "kernarium_oldapp 1\na\tb\ton\tx\t1\n",
                    "kernarium_oldapp 1\na\tb\ton\t1\t-1\n", "hello\n"):
            with self.assertRaises(Failure, msg=repr(bad)):
                runner.parse_listing(bad)

    def test_probe_result(self) -> None:
        self.assertEqual(runner.parse_result("a=1\nb = two\nnoise\n=x\ndone\n"),
                         {"a": "1", "b": "two", "noise": "", "done": ""})
        good = {"context": "u:r:untrusted_app_29:s0:c1,c2", "get": "5", "set": "denied", "after": "5", "done": ""}
        self.assertIn("untrusted_app_29", runner.evaluate_probe(good, "5"))
        for key, value in (("context", "u:r:shell:s0"), ("get", "6"), ("set", "allowed"), ("after", "7")):
            with self.assertRaises(Failure, msg=key):
                runner.evaluate_probe({**good, key: value}, "5")
        with self.assertRaises(Failure):
            runner.evaluate_probe({k: v for k, v in good.items() if k != "done"}, "5")

    def test_process_result(self) -> None:
        good = {"context": HOOK_DOMAIN, "pid": "4001", "layer": "1", "get": "5", "set": "denied", "setpid": "4002",
                "after": "5", "done": ""}
        self.assertEqual(runner.evaluate_process(good, "5", mapped=True), 4001)
        self.assertEqual(runner.evaluate_process({**good, "layer": "0", "get": "", "after": ""}, "", mapped=False), 4001)
        for key, value in (("context", "u:r:untrusted_app_29:s0"), ("get", "6"), ("set", "allowed"), ("after", "7"),
                           ("pid", ""), ("layer", ""), ("layer", "0"), ("setpid", ""), ("setpid", "x1")):
            with self.assertRaises(Failure, msg=f"{key}={value}"):
                runner.evaluate_process({**good, key: value}, "5", mapped=True)
        with self.assertRaises(Failure):
            runner.evaluate_process(good, "5", mapped=False)
        with self.assertRaises(Failure):
            runner.evaluate_process({k: v for k, v in good.items() if k != "setpid"}, "5", mapped=True)


class Steps(unittest.TestCase):
    def setUp(self) -> None:
        # No real waits: the scripted guest answers at once, and a step that expects an answer that never comes
        # gives up on its first look.
        for module, name, value in ((runner, "AVC_SECONDS", 0), (runner, "PROBE_SECONDS", 1), (review, "REVIEW_SECONDS", 1)):
            patcher = unittest.mock.patch.object(module, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        sleeper = unittest.mock.patch("time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def guest(self) -> ScriptedGuest:
        return ScriptedGuest()

    def apks(self) -> dict[str, pathlib.Path]:
        return {variant: pathlib.Path(f"{variant}.apk") for variant in probe.SPECS}

    def test_command_present(self) -> None:
        guest = self.guest()
        self.assertIn(runner.WRAPPER_PATH, runner.command_present(guest))
        guest.path = "/system/bin/kernarium_oldapp"
        with self.assertRaises(Failure):
            runner.command_present(guest)
        guest = self.guest()
        guest.shell_context = "u:r:su:s0"
        with self.assertRaises(Failure):
            runner.command_present(guest)

    def test_listing(self) -> None:
        guest = self.guest()
        self.assertIn("1 application(s) listed", runner.check_listing(guest, guest.clock - 60))
        for change in ("label", "extra-refused", "extra-system", "version", "stale", "future"):
            guest = self.guest()
            installed_at = guest.clock - 60
            if change == "label":
                guest.label = "Old Switch\tProbe"
            elif change == "extra-refused":
                guest.list_extra = [f"{probe.package_of('modern')}\tM\toff\t0\t{guest.clock}"]
            elif change == "extra-system":
                guest.list_extra = [f"{runner.SYSTEM_PACKAGE}\tSettings\toff\t0\t1"]
            elif change == "version":
                guest.version_line = "kernarium_oldapp 2"
            elif change == "stale":
                installed_at = guest.clock + 1000
            elif change == "future":
                guest.packages[probe.package_of("old")] = ("old", (guest.clock + 1000) * 1000)
            with self.assertRaises(Failure, msg=change):
                runner.check_listing(guest, installed_at)
        guest = self.guest()
        del guest.packages[probe.package_of("old")]
        with self.assertRaises(Failure):
            runner.check_listing(guest, guest.clock - 60)

    def test_refusals(self) -> None:
        guest = self.guest()
        self.assertIn("status 64", runner.check_refusals(guest))
        self.assertIn("off accepted", runner.check_refusals(guest))
        guest = self.guest()
        guest.refuse_with = 1
        with self.assertRaises(Failure):
            runner.check_refusals(guest)
        # An image that lets a refused package through.
        guest = self.guest()
        guest.install_variant("old", probe.package_of("modern"))
        with self.assertRaises(Failure):
            runner.check_refusals(guest)

    def test_off_is_accepted_for_any_name(self) -> None:
        guest = self.guest()
        guest.off_needs_eligible = True
        with self.assertRaises(Failure) as problem:
            runner.check_refusals(guest)
        self.assertIn("off", str(problem.exception))

    def test_a_name_that_cannot_be_a_property_name_is_refused(self) -> None:
        guest = self.guest()
        guest.accept_unrepresentable = True
        with self.assertRaises(Failure) as problem:
            runner.check_refusals(guest)
        self.assertIn("cannot be a property name", str(problem.exception))

    def test_switching_and_the_application_view(self) -> None:
        guest = self.guest()
        note, value = runner.switch_on(guest)
        self.assertIn(str(value), note)
        self.assertEqual(guest.properties[probe.property_of("old")], value)
        self.assertRegex(value, r"^[0-9]+ [0-9]+$")
        self.assertIn("could not set it", runner.application_view(guest, value))
        self.assertIn("is empty", runner.switch_off(guest))
        self.assertEqual(guest.properties[probe.property_of("old")], "")

    def test_switch_on_failures(self) -> None:
        guest = self.guest()
        guest.on_time_offset = -1000
        with self.assertRaises(Failure):
            runner.switch_on(guest)

    def test_the_application_can_set(self) -> None:
        guest = self.guest()
        _, value = runner.switch_on(guest)
        guest.app_can_set = True
        with self.assertRaises(Failure):
            runner.application_view(guest, value)

    def test_the_application_cannot_read(self) -> None:
        guest = self.guest()
        _, value = runner.switch_on(guest)
        guest.app_view_value = ""
        with self.assertRaises(Failure):
            runner.application_view(guest, value)

    def test_the_application_is_not_an_ordinary_one(self) -> None:
        guest = self.guest()
        _, value = runner.switch_on(guest)
        guest.domain = "u:r:shell:s0"
        with self.assertRaises(Failure):
            runner.application_view(guest, value)

    def test_switch_off_failures(self) -> None:
        guest = self.guest()
        runner.switch_on(guest)
        guest.keep_on_off = False
        with self.assertRaises(Failure):
            runner.switch_off(guest)

    def test_the_stored_value_is_two_numbers(self) -> None:
        self.assertEqual(runner.choice_seconds("1700000000 1700000000123", "p"), 1700000000)
        for bad in ("", "1700000000", "1700000000 ", "a b", "1 2 3", "1700000000 0", " 1 2"):
            with self.assertRaises(Failure, msg=repr(bad)):
                runner.choice_seconds(bad, "p")
        guest = self.guest()
        original = guest.command

        def old_form(arguments):
            result = original(arguments)
            if arguments[:1] == ["set"] and arguments[2] == "on":
                guest.properties[probe.property_of("old")] = str(guest.clock)
            return result

        guest.command = old_form  # type: ignore[method-assign]
        with self.assertRaises(Failure):
            runner.switch_on(guest)

    def test_the_lock_file(self) -> None:
        guest = self.guest()
        self.assertIn("shell_data_file", runner.check_lock_file(guest))
        guest.lock_type = "system_data_file"
        with self.assertRaises(Failure):
            runner.check_lock_file(guest)

    def test_the_lock_file_is_private(self) -> None:
        guest = self.guest()
        self.assertIn("-rw-------", runner.check_lock_file(guest))
        self.assertEqual(guest.lock_mode, "-rw-------")
        guest.lock_mode = "-rw-rw-rw-"
        with self.assertRaises(Failure):
            runner.check_lock_file(guest)
        guest = self.guest()
        guest.tightens_lock = False
        with self.assertRaises(Failure):
            runner.check_lock_file(guest)

    def test_lifecycle(self) -> None:
        guest = self.guest()
        note = runner.check_lifecycle(guest, self.apks())
        for what in ("reinstalled: cleared", "a raised target level, updated in place: cleared",
                     "a shared user ID: cleared",
                     "uninstalled: cleared", "the earlier one-number form: cleared", "another installation: cleared",
                     "a value that is no time: cleared"):
            self.assertIn(what, note)
        self.assertEqual(guest.properties[probe.property_of("old")], "")

    def test_a_listing_that_clears_a_choice_tightens_the_lock_file(self) -> None:
        """`list` takes the lock only to clear a stale choice, and then makes the file safe like `set`; the step
        makes such a choice, so a listing that did not tighten shows."""
        guest = self.guest()
        guest.list_tightens_lock = False
        with self.assertRaises(Failure) as problem:
            runner.check_lock_file(guest)
        self.assertIn("a listing that cleared a choice left the lock file loose", str(problem.exception))
        guest = self.guest()
        guest.skip_cleanup = {"absent"}
        with self.assertRaises(Failure) as problem:
            runner.check_lock_file(guest)
        self.assertIn("the cleanup did not run", str(problem.exception))
        guest = self.guest()
        runner.check_lock_file(guest)
        self.assertEqual(guest.properties[probe.STORE_PROPERTY_PREFIX + runner.UNKNOWN_PACKAGE], "", "left empty")

    def test_lifecycle_frees_the_shared_user_id_first(self) -> None:
        """Each probe has a key of its own, so the package that replaces `old` with a shared user ID is refused
        while the `shared` probe, which holds the same ID, is installed: the step removes it first."""
        guest = self.guest()
        self.assertIn(probe.package_of("shared"), guest.packages)
        runner.check_lifecycle(guest, self.apks())
        self.assertNotIn(probe.package_of("shared"), guest.packages)
        refusing = self.guest()
        original = refusing.adb_run

        def keeping(*arguments: str, timeout: float = 30):
            if arguments[:2] == ("uninstall", probe.package_of("shared")):
                return original("uninstall", "org.example.other", timeout=timeout)
            return original(*arguments, timeout=timeout)

        refusing.adb_run = keeping  # type: ignore[method-assign]
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(refusing, self.apks())
        self.assertIn("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE", str(problem.exception))

    def test_lifecycle_fails_when_the_listing_does_not_clear(self) -> None:
        for cause in ("reinstalled", "ineligible", "absent", "foreign"):
            guest = self.guest()
            guest.skip_cleanup = {cause}
            with self.assertRaises(Failure, msg=cause) as problem:
                runner.check_lifecycle(guest, self.apks())
            self.assertRegex(str(problem.exception), "holds|listing shows|is listed", cause)

    def test_the_raised_target_is_an_update_in_place_that_only_the_eligibility_rule_clears(self) -> None:
        """The update keeps the installation, so the rule of the installation does not clear the choice: an image
        without the eligibility rule keeps it, and the row must fail there. A reinstall would have been cleared by
        the other rule whatever the eligibility rule did."""
        guest = self.guest()
        guest.skip_cleanup = {"ineligible"}
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(guest, self.apks())
        self.assertIn("still holds a value after the listing, a raised target level", str(problem.exception))
        self.assertIn("the installation is the same", str(problem.exception))
        # The same image, with the update done as a reinstall: the installation rule clears it, and the row
        # notices that the update was not in place.
        guest = self.guest()
        guest.skip_cleanup = {"ineligible"}
        guest.in_place = False
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(guest, self.apks())
        self.assertIn("did not update the installation in place", str(problem.exception))

    def test_the_raised_target_is_installed_over_the_installed_package(self) -> None:
        guest = self.guest()
        note = runner.check_lifecycle(guest, self.apks())
        self.assertIn("updated in place", note)
        commands = guest.commands
        update = commands.index("adb install -r --bypass-low-target-sdk-block raised.apk")
        self.assertTrue(commands[update - 1].startswith("dumpsys package "),
                        "no uninstall between the times and the update")
        self.assertTrue(commands[update + 1].startswith("dumpsys package "), "the times are read again")

    def test_the_raised_target_needs_the_installation_to_be_unchanged(self) -> None:
        guest = self.guest()
        guest.in_place = False
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(guest, self.apks())
        self.assertIn("did not update the installation in place", str(problem.exception))
        # An image whose times do not tell the installations apart (or print no times) is not evidence.
        guest = self.guest()
        original = guest.package_dump
        guest.package_dump = (  # type: ignore[method-assign]
            lambda package: original(package).replace("firstInstallTime", "firstInstall"))
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(guest, self.apks())
        self.assertIn("prints no firstInstallTime and lastUpdateTime", str(problem.exception))

    def test_the_update_needs_one_key(self) -> None:
        """A package of another key is refused by the package manager; the step reports that as it is."""
        guest = self.guest()
        with unittest.mock.patch.object(probe, "same_key", return_value=False):
            with self.assertRaises(Failure) as problem:
                runner.check_lifecycle(guest, self.apks())
        self.assertIn("INSTALL_FAILED_UPDATE_INCOMPATIBLE", str(problem.exception))

    def test_lifecycle_needs_off_for_an_ineligible_package(self) -> None:
        guest = self.guest()
        guest.off_needs_eligible = True
        with self.assertRaises(Failure):
            runner.check_lifecycle(guest, self.apks())

    def test_lifecycle_notices_a_property_that_did_not_survive(self) -> None:
        """If an image dropped the choice at uninstall, the cleanup would not be exercised: the step says so."""
        guest = self.guest()
        original = guest.adb_run

        def dropping(*arguments: str, timeout: float = 30):
            if arguments[:1] == ("install",):
                guest.properties[probe.property_of("old")] = ""
            return original(*arguments, timeout=timeout)

        guest.adb_run = dropping  # type: ignore[method-assign]
        with self.assertRaises(Failure) as problem:
            runner.check_lifecycle(guest, self.apks())
        self.assertIn("not exercised", str(problem.exception))

    def hook(self, guest: ScriptedGuest) -> runner.Report:
        report = runner.Report()
        runner.check_hook(guest, self.apks(), report)
        return report

    def test_hook_steps_pass_on_an_image_that_follows_the_contract(self) -> None:
        guest = self.guest()
        report = self.hook(guest)
        self.assertEqual([status for _, status, _ in report.rows], ["PASS"] * 9, report.rows)
        self.assertNotIn(probe.package_of("running"), guest.packages, "the probe is uninstalled again")

    def test_hook_steps_notice_each_difference(self) -> None:
        for change, failing in (
            ("layer-never", "on, the new process maps the layer and reads the property"),
            ("layer-always", "off, the old application's process has no layer"),
            ("keeps-process", "set on ends the application's process"),
            ("running-can-set", "on, the new process maps the layer and reads the property"),
            ("wrong-domain", "off, the old application's process has no layer"),
            ("no-cleanup", "a stale choice is gone before the next process"),
        ):
            guest = self.guest()
            if change == "layer-never":
                guest.layer = "never"
            elif change == "layer-always":
                guest.layer = "always"
            elif change == "keeps-process":
                guest.keep_process = True
            elif change == "running-can-set":
                guest.running_can_set = True
            elif change == "wrong-domain":
                guest.hook_domain = "u:r:untrusted_app_29:s0"
            elif change == "no-cleanup":
                guest.skip_cleanup = {"reinstalled"}
            report = self.hook(guest)
            names = [name for name, status, _ in report.rows if status == "FAIL"]
            self.assertIn("hook: " + failing, names, f"{change}: {report.rows}")

    def avc_row(self, report: runner.Report) -> tuple[str, str]:
        return next((status, detail) for name, status, detail in report.rows
                    if name == "hook: the denied set leaves an avc line")

    def test_hook_avc_line_is_required_and_says_what_refused_the_set(self) -> None:
        """A user build refuses the application's setprop at the property service's socket; a domain that may reach
        it is refused by the property type's rule. Either line passes, and the note says which it was."""
        for form, kind in (("socket", "at the property service's socket"), ("property", "by the rule of the property's type")):
            guest = self.guest()
            guest.avc_form = form
            report = self.hook(guest)
            status, detail = self.avc_row(report)
            self.assertEqual(status, "PASS", form)
            self.assertIn(f"refused {kind}: avc:", detail)
            self.assertFalse(report.failed, report.rows)

    def test_hook_avc_line_missing_fails_and_says_what_the_log_held(self) -> None:
        guest = self.guest()
        guest.avc_form = "none"
        # Earlier denials of the same domain, another domain's, and a new one that is not a refused set.
        guest.denials.append(NOISE)
        guest.denials.append(SOCKET_DENIAL.format(second=9, pid=3000, serial=99, domain="u:r:untrusted_app_29:s0"))
        report = self.hook(guest)
        status, detail = self.avc_row(report)
        self.assertEqual(status, "FAIL")
        self.assertIn("no avc line for the probe's refused set", detail)
        self.assertIn("0 new denial(s) to u:r:untrusted_app_25:*", detail)
        self.assertEqual([name for name, state, _ in report.rows if state == "FAIL"],
                         ["hook: the denied set leaves an avc line"])
        # A new denial of the domain that is not the refused set is counted and described.
        guest = self.guest()
        guest.avc_form = "none"
        original = guest.broadcast

        def broadcasting(component: str) -> runner.Run:
            result = original(component)
            guest.denials.append(f'11-15 17:01:00.000  {4100 + len(guest.denials)}  4100 I auditd  : type=1400 '
                                 f'audit(0.0:{77 + len(guest.denials)}): avc:  denied  '
                                 '{ read } for comm="sh" scontext=u:r:untrusted_app_25:s0:c512,c768 '
                                 'tcontext=u:object_r:shell_data_file:s0 tclass=file permissive=0')
            return result

        guest.broadcast = broadcasting  # type: ignore[method-assign]
        status, detail = self.avc_row(self.hook(guest))
        self.assertEqual(status, "FAIL")
        self.assertRegex(detail, r"[1-9][0-9]* new denial\(s\) to u:r:untrusted_app_25:\*, none of that process "
                                 r"\(\{ read \} comm=\"sh\" shell_data_file file\)")

    def decoy_run(self, guest: ScriptedGuest, make) -> runner.Report:
        """Runs the hook steps on an image whose probe leaves no denial of its own, and whose log gains the line
        `make(setpid, pid, name)` when the probe's process has run: a denial that is not the probe's evidence."""
        guest.avc_form = "none"
        original = guest.broadcast

        def broadcasting(component: str) -> runner.Run:
            result = original(component)
            package = component.split("/")[0]
            values = dict(line.split("=", 1) for line in guest.results[package].splitlines() if "=" in line)
            guest.denials.append(make(int(values["setpid"]), int(values["pid"]), probe.property_of("running")))
            return result

        guest.broadcast = broadcasting  # type: ignore[method-assign]
        return self.hook(guest)

    def test_hook_avc_line_must_be_of_the_probes_process(self) -> None:
        """A denial of the same domain is not the probe's unless it is of the `setprop` process the probe reported,
        with that domain's categories, and, where the line names a property, of the probe's property."""
        other = probe.STORE_PROPERTY_PREFIX + "org.example.other"

        def socket(setpid: int, pid: int, name: str, *, process: int | None = None, domain: str = HOOK_DOMAIN,
                   comm: str = "setprop", permission: str = "write") -> str:
            line = SOCKET_DENIAL.format(second=5, pid=setpid if process is None else process, serial=70, domain=domain)
            return line.replace('comm="setprop"', f'comm="{comm}"').replace("{ write }", "{ %s }" % permission)

        def prop(setpid: int, pid: int, name: str, *, process: int | None = None, named: str | None = None,
                 domain: str = HOOK_DOMAIN) -> str:
            return PROPERTY_DENIAL.format(second=5, pid=setpid if process is None else process,
                                          name=named or name, domain=domain)

        passing = (
            ("socket form, the setprop process", lambda s, a, n: socket(s, a, n)),
            ("property form, the setprop process and property", lambda s, a, n: prop(s, a, n)),
        )
        failing = (
            ("socket form, another process of the domain", lambda s, a, n: socket(s, a, n, process=s + 100)),
            ("socket form, the application's own process", lambda s, a, n: socket(s, a, n, process=a)),
            ("property form, another process of the domain", lambda s, a, n: prop(s, a, n, process=s + 100)),
            ("property form, the application's own process", lambda s, a, n: prop(s, a, n, process=a)),
            ("property form, another property", lambda s, a, n: prop(s, a, n, named=other)),
            ("property form, a prefix of the property", lambda s, a, n: prop(s, a, n, named=n[:-1])),
            ("socket form, other categories of the domain",
             lambda s, a, n: socket(s, a, n, domain="u:r:untrusted_app_25:s0:c1,c2")),
            ("socket form, another process name", lambda s, a, n: socket(s, a, n, comm="sh")),
            ("socket form, another permission", lambda s, a, n: socket(s, a, n, permission="read")),
        )
        for label, make in passing:
            report = self.decoy_run(self.guest(), make)
            self.assertEqual(self.avc_row(report)[0], "PASS", f"{label}: {report.rows}")
        for label, make in failing:
            report = self.decoy_run(self.guest(), make)
            status, detail = self.avc_row(report)
            self.assertEqual(status, "FAIL", f"{label}: {report.rows}")
            self.assertIn("no avc line for the probe's refused set", detail, label)
            self.assertIn("1 new denial(s)", detail, label)

    def test_hook_avc_row_names_the_process_it_tied_the_line_to(self) -> None:
        guest = self.guest()
        original = guest.broadcast
        setpids: list[str] = []

        def broadcasting(component: str) -> runner.Run:
            result = original(component)
            setpids.append(re.search(r"^setpid=(\d+)$", guest.results[component.split("/")[0]], re.MULTILINE).group(1))
            return result

        guest.broadcast = broadcasting  # type: ignore[method-assign]
        report = self.hook(guest)
        self.assertEqual(len(set(setpids)), len(setpids), "each run of the set is a process of its own")
        # The broadcasts are the off process, the on process (whose set is checked), the off process, the stale one.
        self.assertRegex(self.avc_row(report)[1], rf"^setprop process {setpids[1]} refused ")

    def test_denial_parsing_takes_the_process_from_the_text_or_the_logs_prefix(self) -> None:
        socket = SOCKET_DENIAL.format(second=1, pid=4321, serial=1, domain=HOOK_DOMAIN)
        self.assertEqual(runner.denied_process(socket), 4321)
        named = PROPERTY_DENIAL.format(second=1, pid=777, name="x", domain=HOOK_DOMAIN)
        self.assertEqual(runner.denied_process(named), 777, "the text names the process; the prefix (500) is the log's")
        self.assertIsNone(runner.denied_process("avc: denied { write } scontext=u:r:x:s0"))

    def test_a_log_that_cannot_be_read_is_a_failure_and_not_an_empty_log(self) -> None:
        guest = self.guest()
        self.assertEqual(len(runner.denials(guest)), 1)
        guest.logcat_fails = lambda read: True
        with self.assertRaises(Failure) as problem:
            runner.denials(guest)
        self.assertIn("the log could not be read: logcat -d -b all exited 1", str(problem.exception))

    def test_hook_avc_row_needs_a_baseline_that_was_read(self) -> None:
        """With the baseline unread an earlier denial cannot be told from a new one: the baseline row fails, and the
        avc row says it is not established even though the log holds a matching line."""
        guest = self.guest()
        guest.logcat_fails = lambda read: read == 1
        report = self.hook(guest)
        rows = {name: (status, detail) for name, status, detail in report.rows}
        status, detail = rows["hook: the log is read before the probe's set"]
        self.assertEqual(status, "FAIL")
        self.assertIn("the log could not be read", detail)
        status, detail = rows["hook: the denied set leaves an avc line"]
        self.assertEqual(status, "FAIL")
        self.assertIn("not established", detail)
        self.assertEqual(sorted(name for name, state, _ in report.rows if state == "FAIL"),
                         ["hook: the denied set leaves an avc line", "hook: the log is read before the probe's set"])
        # The same with the probe leaving no denial of its own and an earlier line of the right shape in the log.
        guest = self.guest()
        guest.avc_form = "none"
        guest.logcat_fails = lambda read: read == 1
        report = self.hook(guest)
        self.assertEqual(self.avc_row(report)[0], "FAIL")
        self.assertTrue(report.failed)

    def test_hook_avc_row_fails_when_the_log_cannot_be_read_afterwards(self) -> None:
        guest = self.guest()
        guest.logcat_fails = lambda read: read >= 2
        report = self.hook(guest)
        rows = {name: status for name, status, _ in report.rows}
        self.assertEqual(rows["hook: the log is read before the probe's set"], "PASS")
        status, detail = self.avc_row(report)
        self.assertEqual(status, "FAIL")
        self.assertIn("the log could not be read: logcat -d -b all exited 1", detail)

    def test_hook_avc_row_tries_again_after_a_read_that_failed(self) -> None:
        guest = self.guest()
        guest.logcat_fails = lambda read: read == 2
        with unittest.mock.patch.object(runner, "AVC_SECONDS", 5):
            report = self.hook(guest)
        self.assertEqual(self.avc_row(report)[0], "PASS", report.rows)
        self.assertFalse(report.failed, report.rows)

    def test_hook_avc_row_without_a_result_is_not_a_pass(self) -> None:
        with self.assertRaises(Failure) as problem:
            runner.denial_note(self.guest(), set(), None, probe.property_of("running"))
        self.assertIn("no process to find a denial of", str(problem.exception))

    def test_hook_presses_continue_on_the_permission_review_after_each_install(self) -> None:
        """The probe targets API 10: its broadcast is skipped until the review is done, after the install and after the
        reinstall of the stale step."""
        guest = self.guest()
        report = self.hook(guest)
        self.assertFalse(report.failed, report.rows)
        self.assertEqual(sum(1 for command in guest.commands if command == "input tap 200 230"), 2)
        self.assertIn("(permission review completed)", dict((n, d) for n, _, d in report.rows)[
            "hook: the running probe is installed"])

    def test_hook_fails_when_the_review_cannot_be_completed(self) -> None:
        guest = self.guest()
        guest.review_button = False
        report = self.hook(guest)
        self.assertEqual([name for name, state, _ in report.rows if state == "FAIL"],
                         ["hook: the running probe is installed"])
        self.assertIn("permission review of " + probe.package_of("running"), report.rows[0][2])

    def test_without_the_review_the_probe_wrote_no_result(self) -> None:
        """The scripted guest skips a broadcast while the review is pending, as Android does."""
        guest = self.guest()
        with unittest.mock.patch.object(review, "complete_review", return_value="skipped"):
            report = self.hook(guest)
        self.assertIn("hook: off, the old application's process has no layer",
                      [name for name, state, _ in report.rows if state == "FAIL"])

    def test_real_application(self) -> None:
        guest = self.guest()
        self.assertIn("'Real Game'", runner.check_real(guest, pathlib.Path("real.apk"), "org.example.real", "Real Game"))
        with self.assertRaises(Failure):
            runner.check_real(guest, pathlib.Path("real.apk"), "org.example.real", "Other")
        with self.assertRaises(Failure):
            runner.check_real(guest, pathlib.Path("real.apk"), "org.example.absent", "Real Game")
        # The application is uninstalled again, also after a failure.
        self.assertEqual(sum(1 for command in guest.commands if command.startswith("adb uninstall org.example.real")), 4)


class LaunchedGuest:
    """What the launcher's start_guest hands back: a handle with the adb serial, whose adb_run merges the two streams
    and which has no run. The steps must not drive it."""

    serial = "127.0.0.1:21101"

    def adb_run(self, *arguments: str, timeout: float = 30):
        raise AssertionError("a step drove the launcher's Guest: " + " ".join(arguments))


class LaunchPaths(unittest.TestCase):
    """main() gets its guest two ways, from a running one named by --serial and by launching one. Both give the steps
    the same surface (status and the two streams apart), so a step that works on one works on the other. Here the whole of
    main() runs against the scripted guest through each way."""

    def setUp(self) -> None:
        for module, name, value in ((runner, "AVC_SECONDS", 0), (runner, "PROBE_SECONDS", 1), (review, "REVIEW_SECONDS", 1)):
            patcher = unittest.mock.patch.object(module, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        sleeper = unittest.mock.patch("time.sleep")
        sleeper.start()
        self.addCleanup(sleeper.stop)

    def main_with(self, arguments: list[str], guest: ScriptedGuest) -> tuple[int, str, list]:
        """Runs main() with `arguments`; returns its status, its standard output and the launcher calls it made."""
        launcher: list = []

        def adb(command, **keywords):
            self.assertEqual(command[:3], ["adb", "-s", LaunchedGuest.serial])
            return guest.adb_run(*command[3:], timeout=keywords.get("timeout", 30))

        def start_guest(settings, adb_client, directory, **keywords):
            launcher.append(("start", settings.release, settings.emulator, keywords.get("provision")))
            return LaunchedGuest()

        patches = (
            unittest.mock.patch.object(sys, "argv", ["run_android_oldapp_switch.py", *arguments]),
            unittest.mock.patch.object(runner.orient.boot_android, "resolve_executable", return_value="adb"),
            unittest.mock.patch.object(runner.probe, "build_apk", side_effect=lambda path, *rest, **keywords: pathlib.Path(path)),
            unittest.mock.patch.object(runner.subprocess, "run", side_effect=adb),
            unittest.mock.patch.object(runner.orient, "start_guest", side_effect=start_guest),
            unittest.mock.patch.object(runner.orient, "stop_guest",
                                       side_effect=lambda adb_client, launched: launcher.append(("stop", type(launched).__name__))),
        )
        out = io.StringIO()
        with contextlib.ExitStack() as stack, contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            for patcher in patches:
                stack.enter_context(patcher)
            status = runner.main()
        return status, out.getvalue(), launcher

    @staticmethod
    def rows(text: str) -> list[str]:
        return [line.split(":")[0] for line in text.splitlines() if re.match(r"(PASS|FAIL|SKIP)\b", line)]

    def test_the_two_ways_to_get_a_guest_run_the_same_rows(self) -> None:
        serial_status, serial_out, serial_calls = self.main_with(["--serial", LaunchedGuest.serial, "--hook"], ScriptedGuest())
        self.assertEqual(serial_status, 0, serial_out)
        self.assertEqual(serial_calls, [], "a running guest is neither launched nor stopped")
        own_status, own_out, own_calls = self.main_with(["--release", "/release", "--emulator", "/emulator", "--hook"], ScriptedGuest())
        self.assertEqual(own_status, 0, own_out)
        self.assertEqual(own_calls, [("start", pathlib.Path("/release"), pathlib.Path("/emulator"), True),
                                     ("stop", "LaunchedGuest")])
        self.assertEqual(self.rows(own_out), self.rows(serial_out))
        self.assertGreater(len(self.rows(own_out)), 15)

    def test_the_guest_this_runner_launched_is_stopped_after_a_failing_step(self) -> None:
        guest = ScriptedGuest()
        guest.shell_context = "u:r:su:s0"
        status, out, calls = self.main_with(["--release", "/release", "--emulator", "/emulator"], guest)
        self.assertEqual(status, 1, out)
        self.assertIn("FAIL", out)
        self.assertEqual([call[0] for call in calls], ["start", "stop"])


if __name__ == "__main__":
    unittest.main()
