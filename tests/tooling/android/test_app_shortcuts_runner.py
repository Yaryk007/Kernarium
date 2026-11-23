#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher-application runner without a guest.

run_android_app_shortcuts.py drives the image's `kernarium_apps` command and checks what it answers. Here a
scripted guest plays an image that follows the contract (it reads the manifests of the probe packages the runner
installs, keeps a foreground activity and a marker file, and prints the command's answers), and each way the image
can differ makes the step that owns that rule fail: the probe packages (manifests and resource table read back), the
strict parsers (each deviation of a header, a row, a label, an icon record, the sampler's answer), the judgement of
what the tool's process holds and which domains the renderer's processes run in, and every step against the scripted
guest. It skips (77) where the host has no keytool and jarsigner (the packages are signed).
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import contextlib
import hashlib
import io
import pathlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
import zipfile
import zlib
from typing import NamedTuple

HERE = pathlib.Path(__file__).resolve().parent
INTEGRATION = HERE.parents[1] / "integration" / "android"
sys.path.insert(0, str(INTEGRATION))
import appsprobe_apk as probe  # noqa: E402
import orientation_probe_apk as binary_xml  # noqa: E402
import run_android_app_shortcuts as runner  # noqa: E402

Failure = runner.Failure
SKIP = 77
SETTINGS = "com.android.settings"
CALCULATOR = "com.android.calculator2"


def make_png(size: int, opaque_corners: bool = True, alpha: bool = True) -> bytes:
    """A PNG of two colours (a square with a disc), RGBA or RGB."""
    channels = 4 if alpha else 3
    rows = bytearray()
    centre = size / 2
    for y in range(size):
        rows.append(0)
        for x in range(size):
            inside = (x - centre) ** 2 + (y - centre) ** 2 < (size / 3) ** 2
            colour = (255, 255, 255) if inside else (30, 144, 255)
            corner = (x < 3 or x >= size - 3) and (y < 3 or y >= size - 3)
            rows += bytes(colour) + (bytes([0 if (corner and not opaque_corners) else 255]) if alpha else b"")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 6 if alpha else 2, 0, 0, 0)
    return runner.PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b"")


def chunked(signature: bytes, parts: list[tuple[bytes, bytes]]) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return signature + b"".join(chunk(kind, data) for kind, data in parts)


def gray_png(size: int) -> bytes:
    """A valid 8-bit grayscale PNG: not the RGB or RGBA the contract names."""
    rows = b"".join(b"\0" + bytes((i * 7 + y) & 255 for i in range(size)) for y in range(size))
    return chunked(runner.PNG_SIGNATURE, [(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 0, 0, 0, 0)),
                                          (b"IDAT", zlib.compress(rows)), (b"IEND", b"")])


def corrupt_png(size: int) -> bytes:
    """A PNG whose header is right and whose pixel data is not zlib."""
    return chunked(runner.PNG_SIGNATURE, [(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)),
                                          (b"IDAT", b"not zlib at all" * 8), (b"IEND", b"")])


def probe_png(size: int, flat: bool = False) -> bytes:
    """What an adaptive icon of the second probe comes out as: its background colour, with its disc in the middle."""
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            inside = not flat and (x + 0.5 - size / 2) ** 2 + (y + 0.5 - size / 2) ** 2 <= (size / 3) ** 2
            rows += bytes((*(probe.FOREGROUND_RGB if inside else probe.BACKGROUND_RGB), 255))
    return chunked(runner.PNG_SIGNATURE, [(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)),
                                          (b"IDAT", zlib.compress(bytes(rows))), (b"IEND", b"")])


class Denial(NamedTuple):
    at: str
    comm: str
    target: str
    name: str
    source: str = "u:r:shell:s0"
    perms: str = "read"
    kind: str = "file"
    key: str = "name"
    bug: str = ""


ISOLATED_SOURCE = "u:r:isolated_app:s0:c512,c768"
APP_DATA = "u:object_r:app_data_file:s0:c125,c256,c512,c768"


class Package:
    def __init__(self, name: str, version: int, installed: int, builtin: bool, activities: list[dict]) -> None:
        self.name, self.version, self.installed, self.builtin = name, version, installed, builtin
        for activity in activities:
            activity.setdefault("enabled", True)
            activity.setdefault("manifest_enabled", True)
            activity.setdefault("alias", False)
            activity.setdefault("target", None)
        self.activities = activities
        self.enabled = True
        self.test_only = False


class ScriptedImage:
    """An image that follows the contract. Attributes switch one way of differing on."""

    def __init__(self) -> None:
        self.clock = 1_700_000_000_000
        self.packages: dict[str, Package] = {}
        self.add(Package(SETTINGS, 100, 1, True, [
            {"name": "com.android.settings.Settings", "exported": True, "launcher": True, "label": "Settings",
             "icon": "adaptive"},
            {"name": "com.android.settings.SubSettings", "exported": False, "launcher": False, "label": "x",
             "icon": None}]))
        self.add(Package(CALCULATOR, 200, 1, True, [
            {"name": "com.android.calculator2.Calculator", "exported": True, "launcher": True,
             "label": "Calculator", "icon": "legacy"}]))
        for index in range(1, 7):   # a batch of eight takes seven built-in applications and the second probe
            self.add(Package(f"com.android.extra{index}", 300 + index, 1, True, [
                {"name": f"com.android.extra{index}.Main", "exported": True, "launcher": True, "label": f"Extra {index}",
                 "icon": "adaptive" if index % 2 else "legacy"}]))
        self.resumed: str | None = None
        self.files: set[str] = set()
        self.denials: list[Denial] = []
        self.caller_started: str | None = None
        self.wall = 5 * 3600   # the device's clock, in seconds of the day
        self.evict_before: str | None = None   # a log time before which the buffer has dropped its lines
        self.locales = "en-US"
        self.commands: list[str] = []
        self.calls = 0
        self.launches = 0
        # Ways to differ.
        self.deviation: str | None = None

    def add(self, package: Package) -> None:
        self.packages[package.name] = package

    # -- adb
    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        if arguments[0] == "uninstall":
            self.packages.pop(arguments[1], None)
            return subprocess.CompletedProcess(arguments, 0, b"Success\n", b"")
        if arguments[0] == "install":
            apk = pathlib.Path(arguments[-1])
            with zipfile.ZipFile(apk) as archive:
                manifest = binary_xml.decode_manifest(archive.read("AndroidManifest.xml"))
            name = dict(manifest.attributes)["package"]
            version = dict(manifest.attributes)["versionCode"]
            test_only = any(dict(child.attributes).get("testOnly") for child in manifest.children if child.tag == "application")
            if test_only and "-t" not in arguments:
                return subprocess.CompletedProcess(arguments, 1, b"", b"Failure [INSTALL_FAILED_TEST_ONLY]\n")
            old = self.packages.get(name)
            self.clock += 5_000
            activities = []
            for application in (child for child in manifest.children if child.tag == "application"):
                application_icon = application.attributes.get("icon")
                for activity in application.children:
                    attributes = dict(activity.attributes)
                    label = attributes.get("label", "Apps Probe Application")
                    if isinstance(label, binary_xml.Reference):
                        label = probe.SECOND_LABEL   # the string resource's text
                    activities.append({
                        "name": attributes["name"], "exported": bool(attributes.get("exported")),
                        "launcher": any(child.tag == "intent-filter" for child in activity.children),
                        "enabled": attributes.get("enabled", True), "manifest_enabled": attributes.get("enabled", True),
                        "permission": attributes.get("permission"),
                        "alias": activity.tag == "activity-alias", "target": attributes.get("targetActivity"),
                        "label": label, "icon": "resource" if isinstance(application_icon, binary_xml.Reference) else None})
            self.add(Package(name, version, old.installed if old else self.clock, False, activities))
            self.packages[name].test_only = test_only
            if old:
                self.packages[name].version = version
                self.packages[name].installed = self.clock  # lastUpdateTime moves
            return subprocess.CompletedProcess(arguments, 0, b"Performing Streamed Install\nSuccess\n", b"")
        raise AssertionError(arguments)

    def run(self, command: str, timeout: float = 30) -> "runner.Run":
        self.commands.append(command)
        words = shlex.split(command)
        out, status, err = self.dispatch(command, words)
        return runner.Run(status, out if isinstance(out, bytes) else out.encode(), err)

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {result.text.strip()[-300:]}")
        return result.text

    def dispatch(self, command: str, words: list[str]):
        if words[0] == "ls" and words[1] == "-lZ":
            return f"-rwxr-xr-x 1 root shell u:object_r:system_file:s0 1 {runner.WRAPPER_PATH}\n" \
                   f"-rw-r--r-- 1 root root u:object_r:system_file:s0 1 {runner.JAR_PATH}\n", 0, ""
        if words[:3] == ["command", "-v", runner.COMMAND]:
            return runner.WRAPPER_PATH + "\n", 0, ""
        if words == ["id", "-Z"]:
            return "u:r:shell:s0\n", 0, ""
        if words == ["am", "get-current-user"]:
            return "0\n", 0, ""
        if words[0] == "getprop":
            return ("\n" if words[1] == "persist.sys.locale" else "en-US\n"), 0, ""
        if words[0] == "dumpsys":
            line = f"  mResumedActivity: ActivityRecord{{abc u0 {self.resumed} t1}}\n" if self.resumed else ""
            if self.deviation == "launch-not-resumed":
                line = ""
            return line, 0, ""
        if words[:2] == ["input", "keyevent"]:
            if words[2] == "KEYCODE_HOME" and self.deviation != "home-ignored":
                self.resumed = None
            return "", 0, ""
        if words[:3] == ["settings", "get", "system"]:
            return "null\n", 0, ""
        if words[:2] == ["pm", "disable-user"]:
            self.packages[words[-1]].enabled = False
            return "", 0, ""
        if words[:2] in (["pm", "enable"], ["pm", "disable"]):
            enable = words[1] == "enable"
            if "/" in words[-1]:
                package, _, name = words[-1].partition("/")
                if not self.packages[package].test_only:   # the shell changes the state of a test-only package's component alone
                    return "", 255, f"java.lang.SecurityException: Shell cannot change component state for {words[-1]} to 1\n"
                for activity in self.packages[package].activities:
                    if activity["name"] == name:
                        activity["enabled"] = enable
            else:
                self.packages[words[-1]].enabled = enable
            return "Component new state\n", 0, ""
        if words[:2] == ["pm", "uninstall"]:
            self.packages.pop(words[-1], None)
            return "Success\n", 0, ""
        if words[:2] == ["rm", "-f"]:
            self.files.discard(words[2])
            return "", 0, ""
        if words[0] == "ls" and len(words) == 2:
            return "", (0 if words[1] in self.files else 1), ""
        if words[:2] == ["ls", "-A"]:
            return "".join(f"{name.rsplit('/', 1)[1]}\n" for name in sorted(self.files)), 0, ""
        if words[:4] == ["logcat", "-d", "-b", "all"] and (len(words) == 4 or (len(words) == 6 and words[4] == "-T")):
            return self.log(words[5] if len(words) == 6 else None), 0, ""
        if words[0] == "date":
            stamp = self.stamp()[:-3] + "000"   # a whole second; the clock then moves on, as a real one does between calls
            self.wall += 2
            return stamp + "\n", 0, ""
        if command.startswith(runner.SAMPLER_MARK):
            return self.sampler(words)
        if words[:2] == ["pm", "path"]:
            return f"package:{self.apk_path(words[2])}\n", 0, ""
        if words == probe.CALL_RENDERER.split():
            if self.deviation == "renderer-silent":
                return "", 1, "Error while accessing provider:com.kernarium.icons\n"
            return "Result: Bundle[{status=1}]\n", 0, ""
        if words[:3] == ["am", "broadcast", "-n"]:
            if self.deviation == "caller-not-started":
                return "", 1, "Error: Broadcast not delivered\n"
            self.caller_started = words[3].partition("/")[0]
            return f"Broadcasting: Intent {{ cmp={words[3]} }}\nBroadcast completed: result=0\n", 0, ""
        if words[0] == "run-as":
            if words[2:4] == ["rm", "-f"]:
                self.caller_started = None
                return "", 0, ""
            if words[2] == "cat" and self.caller_started == words[1] and self.deviation != "caller-no-answer":
                return self.caller_answer(), 0, ""
            return "", 1, f"cat: {words[-1]}: No such file or directory\n"
        if words[0] == runner.COMMAND:
            return self.tool(command, words[1:])
        raise AssertionError(f"the scripted guest does not know {command!r}")

    def apk_path(self, package: str) -> str:
        if self.packages[package].builtin:
            leaf = package.rsplit(".", 1)[1].capitalize()
            return f"/system/priv-app/{leaf}/{leaf}.apk"
        return f"/data/app/~~abc/{package}-xyz/base.apk"

    def caller_answer(self) -> str:
        """What the caller's shell wrote: its call of the renderer's provider as an ordinary application."""
        if self.deviation == "caller-allowed":
            return "Result: Bundle[{status=1}]\nstatus=0\n"
        if self.deviation == "caller-allowed-and-names-the-permission":
            return "Result: Bundle[{status=1}]\nwarning: ACCESS_CONTENT_PROVIDERS_EXTERNALLY\nstatus=0\n"
        if self.deviation == "caller-refused-for-nothing":
            return "Error while accessing provider:com.kernarium.icons\njava.lang.IllegalStateException: Could not find provider\nstatus=0\n"
        return ("Error while accessing provider:com.kernarium.icons\njava.lang.SecurityException: Permission Denial: "
                "Do not have permission in call getContentProviderExternal() from pid=5447, uid=10135 requires "
                "android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY\nstatus=0\n")

    def sampler(self, words: list[str]):
        """The sampler's answer for `icons` over the tokens in its script: the paths the tool's process holds, the two
        processes of the renderer's package, the rounds and the tool's status. Deviations differ in one way."""
        end = next(index for index, word in enumerate(words) if word.startswith(">"))
        tokens = words[words.index("icons") + 1:end]
        self.calls += 1
        self.isolated_denial()
        lines = ["M /system/bin/app_process64", "M /system_ext/framework/kernarium_apps.jar", "M /apex/com.android.art/javalib/core-oj.jar",
                 "M anon_inode:[eventfd]", "M socket:[1234]", "M pipe:[5678]"]
        deviation = self.deviation
        target = tokens[-1].partition("/")[0]
        if deviation == "tool-maps-apk":
            lines.append(f"M {self.apk_path(target)}")
        if deviation == "tool-maps-builtin-apk":
            lines.append(f"M {self.apk_path(tokens[0].partition('/')[0])}")
        if deviation == "tool-maps-framework-res":
            lines.append("M /system/framework/framework-res.apk")
        if deviation == "tool-maps-overlay":
            lines.append("M /data/resource-cache/x@y.frro")
        if deviation == "tool-maps-target-dir":
            lines.append(f"M {self.apk_path(tokens[0].partition('/')[0]).rsplit('/', 1)[0]}/oat/arm64/x.odex")
        if deviation == "tool-holds-data":
            lines.append(f"M /data/user/0/{target}/files/x")
        if deviation == "sampler-blind":
            lines = []
        processes = [
            f"P u:r:untrusted_app:s0:c125,c256,c512,c768 u0_a125 4321 1234 1 1 0 0 S {runner.RENDERER_PACKAGE}",
            f"P u:r:isolated_app:s0:c512,c768 u0_i9013 4322 1234 1 1 0 0 S {runner.RENDERER_ISOLATED_PROCESS}"]
        if deviation == "no-isolated-process":
            processes = processes[:1]
        if deviation == "no-provider-process":
            processes = processes[1:]
        if deviation == "isolated-in-shell-domain":
            processes[1] = processes[1].replace("u:r:isolated_app:s0:c512,c768", "u:r:shell:s0")
        if deviation == "isolated-wrong-domain":
            processes[1] = processes[1].replace("u:r:isolated_app", "u:r:untrusted_app")
        if deviation == "isolated-user-wrong":
            processes[1] = processes[1].replace("u0_i9013", "u0_a9013")
        if deviation == "provider-privileged":
            processes[0] = processes[0].replace("u:r:untrusted_app", "u:r:priv_app")
        if deviation == "provider-platform-user":
            processes[0] = processes[0].replace("u0_a125", "system")
        if deviation == "provider-in-shell":
            processes[0] = processes[0].replace("u:r:untrusted_app:s0:c125,c256,c512,c768", "u:r:shell:s0").replace("u0_a125", "shell")
        lines += processes
        lines += ["N 0" if deviation == "sampler-no-rounds" else "N 7"]
        if deviation == "sampler-garbage":
            lines.append("what is this")
        status = "status=1" if deviation == "tool-fails-under-sampler" else "status=0"
        if deviation != "sampler-no-status":
            lines.append(status)
        if deviation == "sampler-fails":
            return "\n".join(lines) + "\n", 3, "sh: boom\n"
        return "\n".join(lines) + "\n", 0, ""

    def isolated_denial(self) -> None:
        """AOSP's one known record of an isolated process, as the log holds it for each start."""
        self.denial("onRenderService", APP_DATA, "/data/user/0/com.kernarium.icons", source=ISOLATED_SOURCE,
                    perms="getattr", kind="dir", key="path", bug="b/120394782")
        if self.deviation == "isolated-denies-more":
            self.denial("onRenderService", "u:object_r:apk_data_file:s0", "base.apk", source=ISOLATED_SOURCE)

    def stamp(self, seconds: int | None = None) -> str:
        seconds = self.wall if seconds is None else seconds
        return f"11-19 {seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}.653"

    def denial(self, comm: str, target: str, name: str = "", at: str | None = None, **more: str) -> None:
        self.denials.append(Denial(at or self.stamp(), comm, target, name, **more))

    def log(self, since: str | None = None) -> str:
        """`logcat -d -b all [-T time]`: a denial is two lines, the kernel's with the comm and the one under the process's
        own tag without it. A buffer that has dropped its old lines no longer shows them."""
        lines = [f"{self.stamp(4 * 3600)} 100   100 I vold    : an unrelated line"]
        for serial, denial in enumerate(self.denials):
            if self.evict_before is not None and denial.at < self.evict_before:
                continue
            stamp = f"{denial.at} {2400 + serial:5d} {2400 + serial:5d}"
            record = f"type=1400 audit(0.0:{160 + serial}): avc:  denied  {{ {denial.perms} }} for "
            named = f'{denial.key}="{denial.name}" ' if denial.name else ""
            bug = f" bug={denial.bug}" if denial.bug else ""
            tail = (f'dev="tmpfs" ino=317 scontext={denial.source} tcontext={denial.target} tclass={denial.kind} '
                    f"permissive=0{bug}")
            lines.append(f'{stamp} I auditd  : {record}comm="{denial.comm}" {named}{tail}')
            lines.append(f"{stamp} W {denial.comm}: {record}{named}{tail}")
        lines.append(f"{self.stamp()} 100   100 I vold    : another unrelated line")
        if since is not None:
            lines = [line for line in lines if line[:18] >= since]
        return "\n".join(lines) + "\n"

    # -- the command
    def rows(self) -> list[tuple[str, str, str, int, str]]:
        """The listing the command prints: launcher activities of the enabled packages, in order."""
        found = []
        for package in self.packages.values():
            if not package.enabled:
                continue
            for activity in package.activities:
                listed = activity["launcher"] and (activity["exported"] or self.deviation == "hidden-listed") \
                    and (not activity.get("permission") or self.deviation == "guarded-listed")
                if not listed or not self.component_enabled(activity):
                    continue
                label = activity["label"]
                if self.deviation == "label-resource-unread" and label == probe.SECOND_LABEL:
                    label = package.name
                if self.deviation == "label-unresolved" and package.builtin:
                    label = package.name
                label = {probe.HOSTILE_LABEL: probe.LISTED_HOSTILE_LABEL, probe.LONG_LABEL: probe.LISTED_LONG_LABEL}.get(label, label)
                if self.deviation == "label-raw" and activity["label"] == probe.HOSTILE_LABEL:
                    label = "Apps\tProbe"
                if self.deviation == "label-uncut" and activity["label"] == probe.LONG_LABEL:
                    label = probe.LONG_LABEL
                if self.deviation == "label-bidi" and activity["label"] == probe.HOSTILE_LABEL:
                    label = probe.LISTED_HOSTILE_LABEL + "‮"
                found.append((package.name, activity["name"], label, 1 if package.builtin else 0, self.key(package, activity)))
        found.sort(key=lambda row: (row[0], row[1]))
        if self.deviation == "unsorted":
            found.reverse()
        return found

    def component_enabled(self, activity: dict) -> bool:
        """The component's state: the manifest's flag until `pm enable` or `pm disable` has set it."""
        if self.deviation == "alias-manifest-flag" and activity["alias"]:
            return bool(activity["manifest_enabled"])   # the bug: the manifest's flag, not the user's state
        if self.deviation == "alias-disabled-listed" and activity["alias"]:
            return True
        return bool(activity["enabled"])

    def key(self, package: Package, activity: dict) -> str:
        text = f"{package.name}\0{activity['name']}\0{'' if self.deviation == 'static-keys' else package.version}\0" \
               f"{'' if self.deviation == 'static-keys' else package.installed}\0{int(package.builtin)}"
        return hashlib.sha256(text.encode()).hexdigest()[:16]

    def digest(self, rows) -> str:
        if self.deviation == "digest-constant":
            return "0" * 64
        if self.deviation == "digest-short":
            return "0" * 63
        salt = "" if self.deviation == "digest-unsalted" else "kernarium_apps-digest-1\n"
        text = salt + "".join(f"{row[0]}\t{row[1]}\t{row[3]}\t{row[4]}\n" for row in rows) \
            + f"locales\t{self.locales}\nuser\t0\n"
        return hashlib.sha256(text.encode()).hexdigest()

    def header(self, rows, digest=None) -> str:
        return (f"kernarium_apps 1\ndigest\t{digest or self.digest(rows)}\ncount\t{len(rows)}\nskipped\t0\n"
                f"locales\t{self.locales}\nuser\t0\n")

    def tool(self, command: str, words: list[str]):
        if self.deviation == "writes-file" and words[:1] == ["list"]:
            self.files.add(f"{runner.SHELL_DATA}/kernarium_apps.cache")
        self.calls += 1
        if self.deviation == "first-call-denial" and self.calls == 1:
            self.denial("app_process64", "u:object_r:resourcecache_data_file:s0")
        if self.deviation == "denial" and words[:1] == ["list"]:
            self.denial("app_process64", "u:object_r:apk_data_file:s0", f"base-{len(self.denials)}.apk")
        if self.deviation in ("repeat-denial", "evicted-baseline") and self.calls == 1:
            self.denial("kernarium_apps-", "u:object_r:qemu_sf_lcd_density_prop:s0", "u:object_r:qemu_sf_lcd_density_prop:s0")
            if self.deviation == "evicted-baseline":   # the buffer drops the older line as the new one comes in
                self.evict_before = self.stamp()
        known = {"digest": 1, "list": 1}
        if not words or (words[0] in known and len(words) != 1) or words[0] not in ("digest", "list", "icons", "launch") \
                or (words[0] == "icons" and not 2 <= len(words) <= 9) or (words[0] == "launch" and len(words) != 2):
            return "", (2 if self.deviation == "usage-status" else 64), "usage: kernarium_apps ...\n"
        rows = self.rows()
        if words[0] == "digest":
            return self.header(rows), 0, ""
        if words[0] == "list":
            digest = self.digest(rows)
            header = self.header(rows, "f" * 64 if self.deviation == "header-differs" else digest)
            body = "rows\n" + "".join("\t".join(map(str, row)) + "\n" for row in rows)
            return header + body + ("" if self.deviation == "list-no-end" else f"end\t{len(rows)}\n"), 0, ""
        if words[0] == "icons":
            self.isolated_denial()
            return self.icons(rows, words[1:]), 0, ""
        return self.launch(rows, words[1])

    def icons(self, rows, tokens: list[str]) -> bytes:
        if self.deviation == "icons-order":
            tokens = list(reversed(tokens))
        bad_package = probe.package_of("bad")
        batch_has_bad = any(token.partition("/")[0] == bad_package for token in tokens)
        out = b"kernarium_apps 1\n" + f"icons\t{len(tokens)}\n".encode()
        for token in tokens:
            package, _, activity = token.partition("/")
            found = None
            if package in self.packages:
                for candidate in self.packages[package].activities:
                    if (candidate["name"] == activity and candidate["launcher"] and candidate["exported"]
                            and self.component_enabled(candidate)
                            and (not candidate.get("permission") or self.deviation == "guarded-icon")):
                        found = candidate
            if found is not None and self.deviation == "builtin-icons-error" and self.packages[package].builtin \
                    and package != SETTINGS:
                out += f"icon\t{token}\terror\t0\t0\t-\t0\t0\t-\n".encode()
            elif found is None or not self.packages[package].enabled:
                out += f"icon\t{token}\tmissing\t0\t0\t-\t0\t0\t-\n".encode()
            elif found["icon"] is None:
                out += f"icon\t{token}\tnoicon\t0\t0\t-\t0\t0\t-\n".encode()
            elif package == bad_package and not (self.deviation == "bad-icon-ok" or (
                    self.deviation == "bad-icon-alone-ok" and len(tokens) == 1)):
                out += f"icon\t{token}\terror\t0\t0\t-\t0\t0\t-\n".encode()
            elif self.deviation == "bad-icon-spoils-batch" and batch_has_bad:
                out += f"icon\t{token}\terror\t0\t0\t-\t0\t0\t-\n".encode()
            else:
                alpha = self.deviation == "icon-corner"
                resource = found["icon"] == "resource"
                kind = "adaptive" if resource else found["icon"]
                if resource:
                    data = probe_png(192, flat=self.deviation == "icon-resource-flat")
                else:
                    data = make_png(192, opaque_corners=not alpha)
                if self.deviation == "icon-gray":
                    data = gray_png(192)
                if self.deviation == "icon-corrupt":
                    data = corrupt_png(192)
                if self.deviation == "icon-bad-crc":
                    data = data[:-1] + bytes([data[-1] ^ 1])   # the IEND chunk's checksum; the record's SHA-256 follows the data
                flag = "1" if self.deviation == "icon-alpha-claimed" else "0"
                size = 128 if self.deviation == "icon-size" else 192
                sha = hashlib.sha256(data).hexdigest()
                if self.deviation == "icon-sha":
                    sha = "0" * 64
                out += (f"icon\t{token}\tok\t{size}\t{size}\t{kind}\t{flag}\t{len(data)}\t{sha}\n"
                        .encode() + data + b"\n")
        if self.deviation != "icons-no-end":
            out += f"end\t{len(tokens)}\n".encode()
        return out

    def launch(self, rows, token: str):
        package, _, activity = token.partition("/")
        marker = f"{runner.MARKER}"
        grammar_ok = runner.PACKAGE_NAME.fullmatch(package) and runner.ACTIVITY_NAME.fullmatch(activity) and "/" not in activity
        found = None
        if grammar_ok and package in self.packages and self.packages[package].enabled:
            for candidate in self.packages[package].activities:
                if candidate["name"] == activity and candidate["launcher"] and self.component_enabled(candidate) and (
                        candidate["exported"] or self.deviation == "launch-hidden") and (
                        not candidate.get("permission") or self.deviation == "guarded-launched"):
                    found = candidate
        if self.deviation == "injection" and not grammar_ok and "touch" in token:
            self.files.add(marker)
        if self.deviation == "disabled-launch" and package in self.packages and not self.packages[package].enabled:
            found = self.packages[package].activities[0]
        if found is None:
            err = "kernarium_apps: refused: not a launcher activity of the foreground user\n"
            if self.deviation == "refusal-multiline":
                err += "second line\n"
            return "", 2, err
        self.resumed = f"{package}/{found['target']}" if found["alias"] else f"{package}/{activity}"
        if self.deviation == "resumed-superstring":
            self.resumed += "Other"   # a component whose name only starts with the wanted one
        self.launches += 1
        err = "kernarium_apps: noise\n" if self.deviation == "launch-again-noisy" and self.launches == 2 else ""
        return f"launched\t{token}\n", 0, err


class Fixture(unittest.TestCase):
    apks: dict[str, pathlib.Path] = {}
    caller: pathlib.Path
    scratch: pathlib.Path

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("keytool") is None or shutil.which("jarsigner") is None:
            raise unittest.SkipTest("no keytool and jarsigner")
        probe_ok = subprocess.run(["keytool", "-help"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, check=False).returncode
        if probe_ok not in (0, 1):
            raise unittest.SkipTest("keytool does not run")
        cls.scratch = pathlib.Path(tempfile.mkdtemp(prefix="uml-appshort-test-"))
        keystore = cls.scratch / "family.p12"
        for variant in (*probe.VARIANTS, *probe.SUCCESSORS):
            cls.apks[variant] = probe.build_apk(cls.scratch / f"{variant}.apk", variant, keystore=keystore)
        cls.caller = probe.build_caller_apk(cls.scratch / "caller.apk", keystore=cls.scratch / "caller.p12")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.scratch, ignore_errors=True)

    def build(self, destination: pathlib.Path, variant: str, keystore=None) -> pathlib.Path:
        shutil.copy(self.apks[variant], destination)
        return destination

    def build_caller(self, destination: pathlib.Path, keystore=None) -> pathlib.Path:
        shutil.copy(self.caller, destination)
        return destination


class ProbePackages(Fixture):
    def test_manifests_read_back(self) -> None:
        for variant in (*probe.VARIANTS, *probe.SUCCESSORS):
            with zipfile.ZipFile(self.apks[variant]) as archive:
                manifest = binary_xml.decode_manifest(archive.read("AndroidManifest.xml"))
                self.assertIn("META-INF/MANIFEST.MF", archive.namelist())
            attributes = dict(manifest.attributes)
            self.assertEqual(probe.package_of(variant), attributes["package"])
            self.assertEqual(2 if variant.endswith("-v2") else 1, attributes["versionCode"])
            application = next(child for child in manifest.children if child.tag == "application")
            self.assertIs(True, dict(application.attributes).get("testOnly"), "the shell enables an alias of a test-only package")
            activities = [child for child in application.children if child.tag == "activity"]
            aliases = [child for child in application.children if child.tag == "activity-alias"]
            self.assertEqual(len(probe.activity_names(variant)), len(activities))
            self.assertEqual(1 if variant.startswith("first") else 0, len(aliases))
            for activity, expected in zip(activities, probe.activity_names(variant)):
                values = dict(activity.attributes)
                self.assertEqual(probe.CLASSES[expected], values["name"])
                self.assertEqual(expected != "hidden", values["exported"])
                self.assertEqual(probe.GUARD_PERMISSION if expected == "guarded" else None, values.get("permission"))
                launcher = [child for child in activity.children if child.tag == "intent-filter"]
                self.assertEqual(0 if expected == "plain" else 1, len(launcher))
                if expected == "listed":
                    self.assertEqual(probe.HOSTILE_LABEL, values["label"])
                if expected == "long":
                    self.assertEqual(probe.LONG_LABEL, values["label"])
                if expected == "only":
                    self.assertEqual(binary_xml.Reference(probe.RES_STRING_LABEL), values["label"])
            for alias in aliases:
                values = dict(alias.attributes)
                self.assertEqual(probe.ALIAS_NAME, values["name"])
                self.assertEqual(probe.ALIAS_TARGET, values["targetActivity"])
                self.assertIs(False, values["enabled"], "the alias is disabled in the manifest")
                self.assertIs(True, values["exported"])
                self.assertEqual(1, len([child for child in alias.children if child.tag == "intent-filter"]))
            icon = dict(application.attributes).get("icon")
            self.assertEqual(binary_xml.Reference(probe.RES_MIPMAP_ICON) if variant in probe.RESOURCE_VARIANTS else None, icon)

    def test_the_resource_backed_package_reads_back(self) -> None:
        with zipfile.ZipFile(self.apks["second"]) as archive:
            names = archive.namelist()
            table = probe.decode_resource_table(archive.read("resources.arsc"))
            icon = binary_xml.decode_manifest(archive.read(probe.ICON_XML))
            png = archive.read(probe.FOREGROUND_PNG)
        for name in ("resources.arsc", probe.ICON_XML, probe.FOREGROUND_PNG):
            self.assertIn(name, names)
        self.assertEqual({
            probe.RES_MIPMAP_ICON: ("mipmap", "ic_launcher", probe.DENSITY_ANY, 26, probe.VALUE_STRING, probe.ICON_XML),
            probe.RES_DRAWABLE_FOREGROUND: ("drawable", "layer_foreground", probe.DENSITY_XXXHIGH, 0, probe.VALUE_STRING,
                                            probe.FOREGROUND_PNG),
            probe.RES_COLOR_BACKGROUND: ("color", "layer_background", 0, 0, probe.VALUE_COLOR,
                                         0xFF000000 | probe.BACKGROUND_RGB[0] << 16 | probe.BACKGROUND_RGB[1] << 8
                                         | probe.BACKGROUND_RGB[2]),
            probe.RES_STRING_LABEL: ("string", "probe_label", 0, 0, probe.VALUE_STRING, probe.SECOND_LABEL),
        }, table)
        self.assertEqual("adaptive-icon", icon.tag)
        self.assertEqual([("background", binary_xml.Reference(probe.RES_COLOR_BACKGROUND)),
                          ("foreground", binary_xml.Reference(probe.RES_DRAWABLE_FOREGROUND))],
                         [(child.tag, child.attributes["drawable"]) for child in icon.children])
        width, height, channels, rows = runner.decode_png(png)
        self.assertEqual((probe.FOREGROUND_SIZE, probe.FOREGROUND_SIZE, 4), (width, height, channels))
        middle = probe.FOREGROUND_SIZE // 2
        self.assertEqual((*probe.FOREGROUND_RGB, 255), runner.pixel(rows, channels, middle, middle))
        self.assertEqual((0, 0, 0, 0), runner.pixel(rows, channels, 0, 0))

    def test_the_bad_package_is_the_second_with_a_foreground_that_is_no_picture(self) -> None:
        with zipfile.ZipFile(self.apks["bad"]) as bad, zipfile.ZipFile(self.apks["second"]) as second:
            self.assertEqual(sorted(second.namelist()), sorted(bad.namelist()))
            png = bad.read(probe.FOREGROUND_PNG)
            self.assertEqual(second.read(probe.ICON_XML), bad.read(probe.ICON_XML))
            self.assertNotEqual(second.read(probe.FOREGROUND_PNG), png)
            table = probe.decode_resource_table(bad.read("resources.arsc"))
        self.assertTrue(png.startswith(runner.PNG_SIGNATURE), "the signature of a picture, nothing after it")
        with self.assertRaises(Failure):
            runner.decode_png(png)
        self.assertEqual(4, len(table))
        self.assertEqual(f"{probe.PACKAGE_PREFIX}.bad", probe.package_of("bad"))
        self.assertEqual(f"{probe.PACKAGE_PREFIX}.bad/android.app.Activity", probe.component("bad", "only"))

    def test_the_caller_is_a_debuggable_receiver_that_calls_the_renderer(self) -> None:
        with zipfile.ZipFile(self.caller) as archive:
            manifest = binary_xml.decode_manifest(archive.read("AndroidManifest.xml"))
            dex = archive.read("classes.dex")
        self.assertEqual(probe.CALLER_PACKAGE, dict(manifest.attributes)["package"])
        application = next(child for child in manifest.children if child.tag == "application")
        self.assertIs(True, dict(application.attributes).get("debuggable"), "run-as reads its answer")
        self.assertEqual(["receiver"], [child.tag for child in application.children], "no activity: never listed")
        receiver = dict(application.children[0].attributes)
        self.assertEqual((probe.CALLER_RECEIVER, True), (receiver["name"], receiver["exported"]))
        self.assertEqual(0, len([child for child in application.children if child.children]), "no intent filter")
        self.assertIn("android.permission.INTERNET",
                      [dict(child.attributes)["name"] for child in manifest.children if child.tag == "uses-permission"])
        self.assertIn(probe.CALLER_SCRIPT.encode(), dex, "the receiver's shell runs the call")
        self.assertIn(f"content call --uri content://{probe.RENDERER_AUTHORITY} --method render", probe.CALLER_SCRIPT)
        self.assertEqual("com.kernarium.icons", probe.RENDERER_AUTHORITY)
        self.assertIn(f"/data/data/{probe.CALLER_PACKAGE}/files", probe.CALLER_SCRIPT)
        self.assertTrue(probe.CALLER_SCRIPT.endswith("> $d/result.tmp && mv $d/result.tmp $d/result"))
        self.assertTrue(probe.CALLER_RESULT.endswith("result"))

    def test_the_resource_table_reader_rejects_a_damaged_table(self) -> None:
        table = probe.build_resource_table(probe.package_of("second"))
        for damaged in (table[:-4], table + b"\0\0\0\0", b"\0" * 12 + table[12:],
                        table[:4] + struct.pack("<I", len(table) + 4) + table[8:]):
            with self.assertRaises((ValueError, struct.error)):
                probe.decode_resource_table(damaged)

    def test_the_variants_of_a_package_are_one_package(self) -> None:
        self.assertEqual(probe.package_of("first"), probe.package_of("first-v2"))
        self.assertNotEqual(probe.package_of("first"), probe.package_of("second"))
        self.assertEqual(len(set(probe.CLASSES[name] for name in probe.activity_names("first"))), 5)
        self.assertEqual(f"{probe.package_of('first')}/{probe.ALIAS_NAME}", probe.alias_component())
        self.assertTrue(runner.ACTIVITY_NAME.fullmatch(probe.ALIAS_NAME))

    def test_expected_labels_follow_the_rule(self) -> None:
        self.assertIsNone(runner.label_problem(probe.LISTED_HOSTILE_LABEL))
        self.assertIsNone(runner.label_problem(probe.LISTED_LONG_LABEL))
        self.assertIsNone(runner.label_problem(probe.LISTED_ALIAS_LABEL))
        self.assertEqual(80, len(probe.LISTED_LONG_LABEL))
        self.assertIsNotNone(runner.label_problem(probe.HOSTILE_LABEL))
        self.assertIsNotNone(runner.label_problem(probe.LONG_LABEL))


class Parsers(unittest.TestCase):
    DIGEST = "ab" * 32

    def header(self, **changes: str) -> bytes:
        fields = {"digest": self.DIGEST, "count": "1", "skipped": "0", "locales": "en-US", "user": "0"}
        fields.update(changes)
        return ("kernarium_apps 1\n" + "".join(f"{key}\t{value}\n" for key, value in fields.items())).encode()

    ROW = "com.a.app\tcom.a.app.Main\tA label\t0\t0123456789abcdef\n"

    def listing(self, header: bytes | None = None, rows: str | None = None, end: str = "end\t1\n") -> bytes:
        return (header or self.header()) + b"rows\n" + (self.ROW if rows is None else rows).encode() + end.encode()

    def test_accepts_the_contract(self) -> None:
        self.assertEqual(self.DIGEST, runner.parse_digest(self.header()).digest)
        listed = runner.parse_list(self.listing())
        self.assertEqual(["com.a.app/com.a.app.Main"], [row.token for row in listed.rows])
        extra = self.ROW.replace("\n", "\tfuture\n")
        self.assertEqual(1, len(runner.parse_list(self.listing(rows=extra)).rows))
        unknown = self.header().replace(b"user\t0\n", b"user\t0\nfuture\tx\n")
        # A header line this reader does not know is not skipped by this strict reader: the host must, but the runner
        # asserts the version-1 block exactly.
        with self.assertRaises(Failure):
            runner.parse_digest(unknown)

    def test_rejects_each_deviation(self) -> None:
        bad_headers = [
            self.header(digest="AB" * 32), self.header(digest="ab" * 31), self.header(count="x"),
            self.header(locales="en US"), self.header(locales="a,b,c,d,e,f"), self.header(user="-1"),
            b"kernarium_apps 2\n" + self.header()[17:], self.header().replace(b"\n", b"\r\n"),
            self.header().rstrip(b"\n"),
        ]
        for data in bad_headers:
            with self.assertRaises(Failure, msg=data):
                runner.parse_digest(data)
        for rows, end in [
            (self.ROW, "end\t2\n"), (self.ROW, ""), (self.ROW, "end\t1\nextra\n"), ("", "end\t1\n"),
            (self.ROW.replace("\t0\t0123", "\t2\t0123"), "end\t1\n"), (self.ROW.replace("0123456789abcdef", "XYZ"), "end\t1\n"),
            ("com.a\n", "end\t1\n"),
            (self.ROW.replace("A label", "A‮label"), "end\t1\n"), (self.ROW.replace("A label", "A  label"), "end\t1\n"),
            (self.ROW.replace("A label", " A label"), "end\t1\n"), (self.ROW.replace("A label", ""), "end\t1\n"),
            (self.ROW.replace("A label", "x" * 81), "end\t1\n"), (self.ROW.replace("A label", "A​label"), "end\t1\n"),
            (self.ROW.replace("com.a.app\t", "bad name\t"), "end\t1\n"), (self.ROW.replace("com.a.app.Main", "-x"), "end\t1\n"),
        ]:
            with self.assertRaises(Failure, msg=(rows, end)):
                runner.parse_list(self.listing(rows=rows, end=end))
        two = "com.b.app\tcom.b.app.Main\tB\t0\t0123456789abcdef\n"
        with self.assertRaises(Failure):
            runner.parse_list(self.listing(header=self.header(count="2"), rows=two + self.ROW, end="end\t2\n"))
        with self.assertRaises(Failure):
            runner.parse_list(self.listing(header=self.header(count="2"), rows=self.ROW + self.ROW, end="end\t2\n"))

    def record(self, data: bytes, **changes: str) -> bytes:
        fields = {"status": "ok", "width": "192", "height": "192", "kind": "adaptive", "alpha": "0",
                  "length": str(len(data)), "sha": hashlib.sha256(data).hexdigest()}
        fields.update(changes)
        return (f"icon\ta.b/C\t{fields['status']}\t{fields['width']}\t{fields['height']}\t{fields['kind']}\t"
                f"{fields['alpha']}\t{fields['length']}\t{fields['sha']}\n").encode() + data + b"\n"

    def test_icons(self) -> None:
        data = make_png(192)
        head = b"kernarium_apps 1\nicons\t1\n"
        good = head + self.record(data) + b"end\t1\n"
        records = runner.parse_icons(good, ["a.b/C"])
        self.assertEqual("ok", records[0].status)
        self.assertEqual("decoded", runner.check_picture(records[0]))
        for broken in [
            good[:-6], good + b"x", good.replace(b"end\t1", b"end\t2"), head + self.record(data, sha="0" * 64) + b"end\t1\n",
            head + self.record(data, width="128", height="128") + b"end\t1\n",
            head + self.record(data, length=str(len(data) + 1)) + b"end\t1\n",
            head + self.record(data, kind="round") + b"end\t1\n", head + self.record(data, status="weird") + b"end\t1\n",
            head + self.record(b"GIF89a" + data[6:]) + b"end\t1\n",
            head + self.record(make_png(192), width="200", height="200") + b"end\t1\n",
            head + self.record(b"", status="missing") + b"end\t1\n",
            good.replace(b"icon\ta.b/C", b"icon\ta.b/D"),
        ]:
            with self.assertRaises(Failure, msg=broken[:80]):
                runner.parse_icons(broken, ["a.b/C"])
        missing = head + b"icon\ta.b/C\tmissing\t0\t0\t-\t0\t0\t-\n" + b"end\t1\n"
        self.assertEqual("missing", runner.parse_icons(missing, ["a.b/C"])[0].status)
        transparent = runner.IconRecord("a.b/C", "ok", 192, 192, "adaptive", "0", 1, "x", make_png(192, opaque_corners=False))
        with self.assertRaises(Failure):
            runner.check_picture(transparent)

    def test_the_picture_is_checked_strictly(self) -> None:
        def record(data: bytes, alpha: str = "0", kind: str = "adaptive", size: int = 192) -> "runner.IconRecord":
            return runner.IconRecord("a.b/C", "ok", size, size, kind, alpha, len(data), "x", data)

        self.assertEqual("decoded", runner.check_picture(record(make_png(192))))
        self.assertEqual("decoded", runner.check_picture(record(make_png(192, opaque_corners=False), alpha="1")))
        self.assertEqual("decoded", runner.check_picture(record(make_png(192, alpha=False))), "RGB is allowed")
        for data, why in [
                (gray_png(192), "a grayscale PNG is not RGB or RGBA"), (corrupt_png(192), "pixel data that does not decompress"),
                (make_png(192)[:-12], "a PNG with no IEND"), (make_png(128), "a picture of another size than the record's")]:
            with self.assertRaises(Failure, msg=why):
                runner.check_picture(record(data))
        with self.assertRaises(Failure, msg="alpha claimed, every pixel opaque"):
            runner.check_picture(record(make_png(192), alpha="1"))
        with self.assertRaises(Failure, msg="opaque claimed, a pixel is not"):
            runner.check_picture(record(make_png(192, opaque_corners=False), alpha="0", kind="legacy"))
        with self.assertRaises(Failure, msg="RGB cannot have alpha"):
            runner.check_picture(record(make_png(192, alpha=False), alpha="1"))
        with self.assertRaises(Failure, msg="one colour"):
            runner.check_picture(record(probe_png(192, flat=True)))
        self.assertEqual("decoded", runner.check_picture(
            record(probe_png(192)), corners=probe.BACKGROUND_RGB, centre=probe.FOREGROUND_RGB))
        with self.assertRaises(Failure, msg="the wrong corner colour"):
            runner.check_picture(record(probe_png(192)), corners=(1, 2, 3), centre=probe.FOREGROUND_RGB)
        with self.assertRaises(Failure, msg="the wrong middle colour"):
            runner.check_picture(record(probe_png(192)), corners=probe.BACKGROUND_RGB, centre=(1, 2, 3))

    def test_the_digest_is_recomputed(self) -> None:
        header = runner.Header("0" * 64, 2, 0, "en-US", 0)
        rows = [runner.Row("a.b", "a.b.A", "A", True, "0123456789abcdef"),
                runner.Row("c.d", "c.d.C", "Another label", False, "fedcba9876543210")]
        text = ("kernarium_apps-digest-1\na.b\ta.b.A\t1\t0123456789abcdef\nc.d\tc.d.C\t0\tfedcba9876543210\n"
                "locales\ten-US\nuser\t0\n")
        self.assertEqual(hashlib.sha256(text.encode()).hexdigest(), runner.expected_digest(runner.Listed(header, rows), ["en-US"]))
        longer = "locales\ten-US\nlocalesall\t" + hashlib.sha256(b"en-US,fr-FR").hexdigest() + "\nuser\t0\n"
        self.assertEqual(hashlib.sha256(text.replace("locales\ten-US\nuser\t0\n", longer).encode()).hexdigest(),
                         runner.expected_digest(runner.Listed(header, rows), ["en-US", "fr-FR"]))
        self.assertNotEqual(runner.expected_digest(runner.Listed(header, rows), ["en-US"]),
                            runner.expected_digest(runner.Listed(header, rows[:1]), ["en-US"]))

    def test_png_decoder(self) -> None:
        width, height, channels, rows = runner.decode_png(make_png(24))
        self.assertEqual((24, 24, 4), (width, height, channels))
        self.assertEqual((30, 144, 255, 255), runner.pixel(rows, channels, 0, 0))
        self.assertEqual((255, 255, 255, 255), runner.pixel(rows, channels, 12, 12))
        self.assertEqual(3, runner.decode_png(make_png(24, alpha=False))[2])

    def test_the_png_chunk_checksums_are_checked(self) -> None:
        good = make_png(24)
        for position in (20, len(good) - 1, len(good) - 30):   # in the header, the end chunk's checksum, the pixel data
            damaged = bytearray(good)
            damaged[position] ^= 1
            with self.assertRaises(Failure):
                runner.decode_png(bytes(damaged))
        with self.assertRaises(Failure):
            runner.decode_png(good[:-6])

    def test_a_denial_is_the_same_denial_however_the_log_shows_it(self) -> None:
        auditd = ('11-19 04:46:11.653  2416  2416 I auditd  : type=1400 audit(0.0:160): avc:  denied  { read } for  '
                  'comm="kernarium_apps-" name="u:object_r:qemu_sf_lcd_density_prop:s0" dev="tmpfs" ino=317 '
                  'scontext=u:r:shell:s0 tcontext=u:object_r:qemu_sf_lcd_density_prop:s0 tclass=file permissive=0')
        tagged = ('11-19 04:48:02.001  2999  2999 W main    : type=1400 audit(0.0:171): avc:  denied  { read } for '
                  'name="u:object_r:qemu_sf_lcd_density_prop:s0" dev="tmpfs" ino=317 scontext=u:r:shell:s0 '
                  'tcontext=u:object_r:qemu_sf_lcd_density_prop:s0 tclass=file permissive=0')
        key = runner.audit_key(auditd)
        self.assertEqual(("avc", "read", "u:r:shell:s0", "u:object_r:qemu_sf_lcd_density_prop:s0", "file",
                          '"u:object_r:qemu_sf_lcd_density_prop:s0"'), key)
        self.assertEqual(key, runner.audit_key(tagged))
        for changed in (auditd.replace("{ read }", "{ write }"), auditd.replace("tclass=file", "tclass=dir"),
                        auditd.replace("qemu_sf_lcd_density_prop:s0\" dev", "other:s0\" dev"),
                        auditd.replace("tcontext=u:object_r:qemu_sf_lcd_density_prop", "tcontext=u:object_r:other"),
                        auditd.replace("scontext=u:r:shell:s0", "scontext=u:r:shell:s0:c1")):
            self.assertNotEqual(key, runner.audit_key(changed), changed)
        for other in (auditd.replace("scontext=u:r:shell:s0", "scontext=u:r:system_server:s0"),
                      auditd.replace("denied", "granted"), "an unrelated line", ""):
            self.assertIsNone(runner.audit_key(other), other)
        # A shell denial in a shape this runner cannot take apart still counts.
        self.assertIsNotNone(runner.audit_key("avc: denied for scontext=u:r:shell:s0 odd"))

    GOOD_SAMPLE = ("M /system/bin/app_process64\nM /apex/com.android.art/javalib/core-oj.jar\nM anon_inode:[eventfd]\n"
                   "P u:r:untrusted_app:s0:c125,c256,c512,c768 u0_a125 4321 1234 1 1 0 0 S com.kernarium.icons\n"
                   "P u:r:isolated_app:s0:c512,c768 u0_i9013 4322 1234 1 1 0 0 S "
                   "com.kernarium.icons:com.kernarium.icons.IconRenderService\nN 7\nstatus=0\n")

    def test_the_sampler_answer_is_read_strictly(self) -> None:
        sampled = runner.parse_sampler(self.GOOD_SAMPLE)
        self.assertEqual((3, 7, 0, 2), (len(sampled.paths), sampled.rounds, sampled.status, len(sampled.processes)))
        self.assertEqual(("u:r:isolated_app:s0:c512,c768", "u0_i9013", runner.RENDERER_ISOLATED_PROCESS), sampled.processes[1])
        self.assertEqual(3, runner.parse_sampler(self.GOOD_SAMPLE.replace("status=0", "status=3")).status)
        for broken, why in [
                (self.GOOD_SAMPLE.replace("N 7\n", ""), "no round count"),
                (self.GOOD_SAMPLE.replace("status=0\n", ""), "no status"),
                (self.GOOD_SAMPLE + "M /late\n", "a line after the status"),
                (self.GOOD_SAMPLE.replace("N 7\n", "N 7\nN 8\n"), "two round counts"),
                (self.GOOD_SAMPLE.replace("N 7", "N x"), "a round count that is not a number"),
                (self.GOOD_SAMPLE.replace("status=0", "status=x"), "a status that is not a number"),
                (self.GOOD_SAMPLE.replace("N 7\n", "N 7\nwhat is this\n"), "a line that is nothing"),
                (self.GOOD_SAMPLE.replace("P u:r:untrusted_app:s0:c125,c256,c512,c768 u0_a125 4321", "P u0_a125 4321"),
                 "a process line with no context"),
                ("", "nothing at all")]:
            with self.assertRaises(Failure, msg=why):
                runner.parse_sampler(broken)

    def test_what_the_tool_process_holds_is_judged(self) -> None:
        files = ["/system/priv-app/Settings/Settings.apk", "/data/app/~~abc/org.x.second-xyz/base.apk",
                 "/data/app/~~def/org.y.split-xyz/split_config.xxhdpi.apk"]
        packages = ["com.android.settings", "org.x.second", "org.y.split"]

        def judged(*paths: str) -> list[str]:
            return runner.drawing_in_the_tool(runner.Sampled(set(paths), [], 1, 0), packages, files)

        for fine in ("/system/bin/app_process64", "/system_ext/framework/kernarium_apps.jar", "/system/framework/framework.jar",
                     "/apex/com.android.art/javalib/core-oj.jar", "anon_inode:[eventfd]", "socket:[1]", "/dev/null",
                     "/system/priv-app/SettingsX/other.jar", "/data/app/~~abc/org.x.second-xyzzy/other"):
            self.assertEqual([], judged(fine), fine)
        for held in files + ["/system/framework/framework-res.apk", "/data/resource-cache/a@b.frro", "/vendor/overlay/A.idmap", "/vendor/overlay/B.frro",
                             "/data/resource-cache/anything", "/system/priv-app/Settings/oat/arm64/Settings.odex",
                             "/data/app/~~abc/org.x.second-xyz/oat/arm64/base.odex", "/data/app/~~abc/org.x.second-xyz/lib/arm64/x.so",
                             "/data/user/0/org.x.second/files/x", "/data/data/org.y.split/cache/y", "/data/app/~~def/org.y.split-xyz/x.dm"]:
            self.assertEqual([held], judged(held), held)
        self.assertEqual("/data/app/~~abc/org.x.second-xyz", runner.own_directory(files[1]))
        self.assertEqual("/data/app/~~def/org.y.split-xyz", runner.own_directory(files[2]))
        self.assertEqual("/system/priv-app/Settings", runner.own_directory(files[0]))
        for shared in ("/system/framework/framework-res.apk", "/system/app/Bar.apk", "/foo.apk", "/product/app/Other/Foo.apk"):
            self.assertIsNone(runner.own_directory(shared), shared)

    def test_the_renderers_two_processes_are_judged(self) -> None:
        provider = ("u:r:untrusted_app:s0:c125,c256,c512,c768", "u0_a125", runner.RENDERER_PACKAGE)
        isolated = ("u:r:isolated_app:s0:c512,c768", "u0_i9013", runner.RENDERER_ISOLATED_PROCESS)

        def judged(*processes: tuple[str, str, str]) -> str:
            return runner.renderer_domains(runner.Sampled(set(), list(processes), 1, 0))

        self.assertIn("u0_a125 in u:r:untrusted_app", judged(provider, isolated))
        self.assertIn("u0_a125 in u:r:untrusted_app_29", judged((provider[0].replace("untrusted_app", "untrusted_app_29"),
                                                                 *provider[1:]), isolated))
        self.assertIn("u0_a125", judged((provider[0].replace(":c125,c256,c512,c768", ""), *provider[1:]), isolated))
        for broken, why in [
                ((provider,), "no isolated process"), ((isolated,), "no provider process"), ((), "no process at all"),
                ((provider, (isolated[0].replace("isolated_app", "untrusted_app"), *isolated[1:])), "the service in an application domain"),
                ((provider, (isolated[0].replace("isolated_app", "shell"), *isolated[1:])), "the service in the shell domain"),
                ((provider, (isolated[0], "u0_a9013", isolated[2])), "the service under an application user"),
                ((provider, (isolated[0], "shell", isolated[2])), "the service under the shell user"),
                (((provider[0].replace("untrusted_app", "priv_app"), *provider[1:]), isolated), "a privileged application"),
                (((provider[0].replace("untrusted_app", "platform_app"), *provider[1:]), isolated), "a platform application"),
                (((provider[0].replace("untrusted_app", "system_app"), "system", provider[2]), isolated), "the system's"),
                (((provider[0].replace("untrusted_app", "shell"), "shell", provider[2]), isolated), "the shell's"),
                ((provider, isolated, (isolated[0].replace("isolated_app", "shell"), "shell", isolated[2])), "one more process of the service in the shell"),
                (((provider[0], "u0_a125x", provider[2]), isolated), "a user that only starts like an application's"),
                (((provider[0].replace("untrusted_app", "untrusted_appx"), *provider[1:]), isolated), "a domain that only starts like untrusted_app")]:
            with self.assertRaises(Failure, msg=why):
                judged(*broken)

    def test_the_sampler_script_runs_the_tool_bounded_and_writes_nothing(self) -> None:
        tokens = ["com.android.settings/com.android.settings.Settings", "org.x.second/android.app.Activity"]
        script = runner.sampler_script(tokens)
        self.assertTrue(script.startswith(runner.SAMPLER_MARK + "\n"))
        self.assertIn(f"{runner.COMMAND} icons {' '.join(tokens)} >/dev/null 2>&1 &", script)
        self.assertIn(f"[ $n -lt {runner.SAMPLER_ROUNDS} ] && kill -0 $p", script)
        self.assertIn("/proc/$p/maps", script)
        self.assertIn("/proc/$p/fd", script)
        self.assertIn(f"grep -F ' {runner.RENDERER_PACKAGE}'", script)
        self.assertIn('echo "N $n"', script)
        self.assertIn('wait $p; echo "status=$?"', script)
        self.assertEqual({"/dev/null"}, set(re.findall(r">+\s*([^\s&;|]+)", script)), "no file is written")
        quoted = runner.sampler_script(["a.b/C$D"])
        self.assertIn("'a.b/C$D'", quoted)
        syntax = subprocess.run(["sh", "-n"], input=script, text=True, capture_output=True, check=False)
        self.assertEqual(0, syntax.returncode, syntax.stderr)
        self.assertGreater(runner.SAMPLER_SECONDS, 30)

    def test_an_audit_line_is_keyed_by_domain_and_names_its_path(self) -> None:
        isolated = ('11-19 07:57:01.406  5609  5609 I auditd  : type=1400 audit(0.0:407): avc:  denied  { getattr } for  '
                    'comm="onRenderService" path="/data/user/0/com.kernarium.icons" dev="ubdb3" ino=341464 '
                    'scontext=u:r:isolated_app:s0:c512,c768 tcontext=u:object_r:app_data_file:s0:c125,c256,c512,c768 '
                    'tclass=dir permissive=0 bug=b/120394782')
        tagged = isolated.replace("I auditd  :", "W onRenderService:").replace(' comm="onRenderService"', "")
        self.assertIsNone(runner.audit_key(isolated), "it is no denial of the shell")
        key = runner.audit_key(isolated, runner.ISOLATED_CONTEXT)
        self.assertEqual(("avc", "getattr", "u:r:isolated_app:s0:c512,c768",
                          "u:object_r:app_data_file:s0:c125,c256,c512,c768", "dir", '"/data/user/0/com.kernarium.icons"'), key)
        self.assertEqual(key, runner.audit_key(tagged, runner.ISOLATED_CONTEXT))
        self.assertNotEqual(key, runner.audit_key(isolated.replace("path=\"/data/user/0/com.kernarium.icons\"", "path=\"/other\""),
                                                  runner.ISOLATED_CONTEXT))
        self.assertIsNone(runner.audit_key(isolated.replace("isolated_app", "untrusted_app"), runner.ISOLATED_CONTEXT))
        report = runner.audit_report([isolated, tagged, isolated], runner.ISOLATED_CONTEXT)
        self.assertEqual('u:r:isolated_app:s0 audit lines (reported, not judged): { getattr } dir '
                         '"/data/user/0/com.kernarium.icons" -> u:object_r:app_data_file:s0:c125,c256,c512,c768 '
                         '(b/120394782) x3', report)
        self.assertEqual("u:r:isolated_app:s0 audit lines: none", runner.audit_report([], runner.ISOLATED_CONTEXT))
        self.assertEqual("u:r:isolated_app:s0 audit lines: none", runner.audit_report(["unrelated"], runner.ISOLATED_CONTEXT))
        other = isolated.replace("{ getattr }", "{ read }").replace("tclass=dir", "tclass=file").replace(" bug=b/120394782", "")
        self.assertEqual(2, runner.audit_report([isolated, other], runner.ISOLATED_CONTEXT).count(" x1"))

    def test_the_resumed_component_is_read_whole(self) -> None:
        dump = ("  mResumedActivity: ActivityRecord{1a2b u0 org.x.a/.Main t14}\n"
                "  topResumedActivity=ActivityRecord{3c4d u0 org.x.b/android.app.Activity t15 f}\n"
                "  ResumedActivity: ActivityRecord{5e6f u10 org.x.c/org.x.c.Deep t9}\n  noise ResumedActivity\n")
        self.assertEqual(["org.x.a/org.x.a.Main", "org.x.b/android.app.Activity", "org.x.c/org.x.c.Deep"],
                         runner.resumed_components(dump))


class Steps(Fixture):
    def run_steps(self, deviation: str | None) -> dict[str, str]:
        image = ScriptedImage()
        image.deviation = deviation
        if deviation in ("repeat-denial", "evicted-baseline", "old-denial"):   # an earlier run's denial is in the log already
            image.denial("kernarium_apps-", "u:object_r:qemu_sf_lcd_density_prop:s0", "u:object_r:qemu_sf_lcd_density_prop:s0",
                         at=image.stamp(4 * 3600 + 46 * 60 + 11))
        report = runner.Report()
        options = types_namespace()
        with contextlib.redirect_stdout(io.StringIO()), \
                unittest.mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                unittest.mock.patch.object(runner, "LAUNCH_SECONDS", 0.5), \
                unittest.mock.patch.object(runner, "CALLER_SECONDS", 0.5), \
                unittest.mock.patch.object(runner, "open_guest", lambda options, adb, private: (image, lambda: None)), \
                unittest.mock.patch.object(runner.probe, "build_apk", self.build), \
                unittest.mock.patch.object(runner.probe, "build_caller_apk", self.build_caller):
            runner.run(options, "adb", pathlib.Path(tempfile.mkdtemp(prefix="uml-appshort-run-")), report)
        self.image = image
        return {name: status for name, status, _ in report.rows}

    def failing(self, deviation: str) -> set[str]:
        return {name for name, status in self.run_steps(deviation).items() if status == "FAIL"}

    def test_a_conforming_image_passes_every_step(self) -> None:
        statuses = self.run_steps(None)
        self.assertEqual({"PASS"}, set(statuses.values()), statuses)
        self.assertEqual(16, len(statuses))
        self.assertEqual(set(), self.image.files)
        self.assertNotIn(probe.CALLER_PACKAGE, self.image.packages, "the caller is removed again")
        self.assertNotIn(probe.package_of("bad"), self.image.packages)

    def test_the_isolated_renderers_audit_lines_are_reported_and_not_judged(self) -> None:
        for deviation, expected in ((None, "{ getattr } dir"), ("isolated-denies-more", "apk_data_file")):
            with self.subTest(deviation=deviation):
                image = ScriptedImage()
                image.deviation = deviation
                report = runner.Report()
                with contextlib.redirect_stdout(io.StringIO()), \
                        unittest.mock.patch.object(runner.time, "sleep", lambda seconds: None), \
                        unittest.mock.patch.object(runner, "LAUNCH_SECONDS", 0.5), \
                        unittest.mock.patch.object(runner, "CALLER_SECONDS", 0.5), \
                        unittest.mock.patch.object(runner, "open_guest", lambda options, adb, private: (image, lambda: None)), \
                        unittest.mock.patch.object(runner.probe, "build_apk", self.build), \
                        unittest.mock.patch.object(runner.probe, "build_caller_apk", self.build_caller):
                    runner.run(types_namespace(), "adb", pathlib.Path(tempfile.mkdtemp(prefix="uml-appshort-run-")), report)
                details = {name: (status, detail) for name, status, detail in report.rows}
                status, detail = details["the command writes nothing and is denied nothing"]
                self.assertEqual("PASS", status, detail)
                self.assertIn("audit lines (reported, not judged)", detail)
                self.assertIn(expected, detail)
                self.assertIn("(b/120394782)", detail)
                self.assertEqual({"PASS"}, {status for status, _ in details.values()}, details)

    def test_a_denial_from_before_the_window_is_not_the_commands(self) -> None:
        statuses = self.run_steps("old-denial")
        self.assertEqual({"PASS"}, set(statuses.values()), statuses)

    def test_each_deviation_fails_its_step(self) -> None:
        header = "digest and list agree on the header block"
        rows = "list names the built-in and the probe's applications, escaped and bounded"
        icons = "icons are PNG records that check out"
        launch = "launch starts a launcher activity"
        refusals = "launch refuses all else, and runs nothing it is given"
        usage = "usage errors are status 64"
        split = "the drawing is done in the renderer's two processes, not in the shell"
        bad = "a bad icon costs only itself"
        caller = "an ordinary application cannot use the renderer"
        clean = "the command writes nothing and is denied nothing"
        alias = "a launcher alias enabled for the user is listed, drawn and started"
        sets = "the digest follows the set of applications"
        expected = {
            "digest-short": header, "header-differs": header, "unsorted": header, "label-raw": header,
            "label-uncut": header, "label-bidi": header, "list-no-end": header, "digest-unsalted": header,
            "hidden-listed": rows, "label-resource-unread": rows, "label-unresolved": rows, "guarded-listed": rows,
            "icon-sha": icons, "icon-size": icons, "icon-corner": icons, "icons-no-end": icons, "icons-order": icons,
            "guarded-icon": icons, "icon-gray": icons, "icon-corrupt": icons, "icon-bad-crc": icons, "icon-alpha-claimed": icons, "icon-resource-flat": icons,
            "launch-not-resumed": launch, "launch-again-noisy": launch, "home-ignored": launch,
            "resumed-superstring": launch,
            "alias-manifest-flag": alias, "alias-disabled-listed": rows,
            "launch-hidden": refusals, "guarded-launched": refusals, "injection": refusals, "refusal-multiline": refusals, "disabled-launch": refusals,
            "usage-status": usage,
            "writes-file": clean, "denial": clean, "first-call-denial": clean, "repeat-denial": clean,
            "evicted-baseline": clean,
            "digest-constant": sets, "static-keys": sets, "builtin-icons-error": icons,
            "tool-maps-apk": split, "tool-maps-builtin-apk": split, "tool-maps-framework-res": split, "tool-maps-overlay": split,
            "tool-maps-target-dir": split, "tool-holds-data": split, "sampler-blind": split, "no-isolated-process": split,
            "no-provider-process": split, "isolated-in-shell-domain": split, "isolated-wrong-domain": split,
            "isolated-user-wrong": split, "provider-privileged": split, "provider-platform-user": split,
            "provider-in-shell": split, "sampler-no-rounds": split, "sampler-garbage": split, "sampler-no-status": split,
            "tool-fails-under-sampler": split, "sampler-fails": split,
            "bad-icon-ok": bad, "bad-icon-spoils-batch": bad, "bad-icon-alone-ok": bad,
            "renderer-silent": caller, "caller-allowed": caller, "caller-allowed-and-names-the-permission": caller, "caller-refused-for-nothing": caller,
            "caller-no-answer": caller, "caller-not-started": caller,
        }
        for deviation, step in expected.items():
            with self.subTest(deviation=deviation):
                failed = self.failing(deviation)
                self.assertIn(step, failed, f"{deviation}: failed steps {sorted(failed)}")


def types_namespace():
    import argparse
    return argparse.Namespace(serial="x", release=None, emulator=None, adb_port=1, cpus=1, memory="1G", boot_timeout=1)


if __name__ == "__main__":
    if shutil.which("keytool") is None or shutil.which("jarsigner") is None:
        print("SKIP: no keytool and jarsigner on PATH")
        sys.exit(SKIP)
    unittest.main(argv=[sys.argv[0], "-v", *sys.argv[1:]], exit=True)
