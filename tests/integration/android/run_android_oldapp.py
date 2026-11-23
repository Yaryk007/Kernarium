#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the old-application layer: an old 32-bit application the person switched it on for starts on ART, any other is left alone.

android/lineage-24/overlay/oldapp is a library the framework loads into an application process when the
application targets an API level below 14 (Android before 4.0), is 32-bit and is switched on for it (the persistent
property persist.sys.kernarium.oldapp.<package> is not empty). It gives such an application what Dalvik gave it: a Java `byte[]` that reads
through the object pointer at the Dalvik offsets (a block that stands for the array: a write through it reaches
the array, it is the same object for every JNI function), and a heap below 2 GiB. This runner installs the probe
application of oldapp_probe_apk.py in its two variants and reads what each one reports:

- ``old`` (targetSdkVersion 10, installed past the package manager's block on old targets, as Andarium
  installs such an application): the layer is mapped in its process, the raw read of the `byte[]` returns the
  bytes (also from a thread of its own), the array still works as a JNI reference (handed back to Java, read
  and written through the array functions, one object with a global reference to it, locked, kept while more
  results arrive, popped out of a local frame, filled by Java and read again, written through the object pointer
  without a release and read by Java), getline and malloc_usable_size accept a block the application
  allocated and the application reallocates and frees a block libc allocated, the JNI and invoke tables are
  read-only again, and every allocation lies below 2 GiB;
- ``modern`` (targetSdkVersion 29): the layer is not mapped, and the raw read does not find the bytes (the
  reference is ART's own). The array handed back works, which shows that the probe itself runs.

On an image that carries the layer the framework loads it and the logcat tag ``kernarium_oldapp`` shows the
old process installing it and the modern one not; the old application is first run without being switched on
(the layer is left out) and then switched on. ``--host-shim`` is for an image that does not carry it (the
accepted release before this change): the runner builds the layer with the host's clang and lld, bundles it in
the packages and has the old probe load it, after a control run of the old probe without it (the raw read
fails), which proves the library's mechanism but not the framework's rule. The libraries need `--clang` and
`--lld` (default: those on PATH).

An application that targets below API 23 gets the permission review before its first component runs, and a
broadcast is skipped until then; the runner presses Continue, as the user does on the first launch.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest
is reached through the shared adb server, always named with `-s`.
usage: run_android_oldapp.py --emulator uml-darwin-android17 [--release DIR] [--host-shim] [--serial HOST:PORT]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "integration" / "support"))
sys.path.insert(0, str(HERE))
import oldapp_probe_apk as probe  # noqa: E402
import permission_review  # noqa: E402

# The orientation runner owns the guest: its launcher and its adb.
SPEC = importlib.util.spec_from_file_location("run_android_orientation", HERE / "run_android_orientation.py")
assert SPEC is not None and SPEC.loader is not None
orient = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = orient  # dataclasses resolve their annotations through sys.modules
SPEC.loader.exec_module(orient)
Failure = orient.Failure
Report = orient.Report

LOG_TAG = "kernarium_oldapp"
INSTALLED = "installed:"
INSTALL_SECONDS = 180
BROADCAST_SECONDS = 60
#: What each variant must report: the lines that must read exactly so (the others are recorded).
#: What an application does with an array and an allocation the runtime's own way: the layer must not change the verdicts.
SEMANTICS = {"write_through": "pass", "identity": "pass", "retained": "pass", "monitor": "pass", "pop": "pass",
             "getline": "pass", "usable_size": "pass", "libc_block": "pass", "table_read_only": "pass",
             "java_fill": "pass", "commit_chunks": "pass", "clone": "pass", "alias_elements": "pass",
             "alias_critical": "pass", "abi_args": "pass", "abi_returns": "pass", "ends_exception": "pass"}
#: What the layer adds: the blocks of a native method end when it returns, and an ended block stays where it was until
#: its array is collected (every row reads the block through the Dalvik offsets, so it is "fail" where the layer is not
#: loaded, as java_fill_raw is).
ENDS = ("ends_lazy_thumb", "ends_lazy_arm", "ends_reg_thumb", "ends_reg_arm", "ends_recursion", "ends_onload",
        "ends_outside", "ended_keep", "ended_arrays", "ended_pin", "ended_kept_ref")
#: The render-thread scene of an application (a run of its own, with the options "scene"): the same in every variant,
#: since a thread that gets a few hundred results at a time works on ART too. The first three rows are a thread that
#: Java starts; scene_attached is a short run on a native thread that attaches itself.
SCENE_ROWS = ("scene_complete", "scene_data", "scene_write", "scene_attached")
EXPECTED = {
    "scene": {**dict.fromkeys(SCENE_ROWS, "pass"), "done": "1"},
    # A library of another package is closed and the application's own opened at its address: the layer's binding is decided
    # from the loader's list as it is (a run of its own, with the options "swap|<foreign>|<own>").
    "swap": {"bind_swap": "pass", "bind_swap_good": "600", "bind_swap_same_address": "yes", "done": "1"},
    "old": {"shim_mapped": "yes", "raw_read": "pass", "passback": "pass", "elements": "pass", "global": "pass",
            "thread_raw": "pass", **SEMANTICS, "java_fill_raw": "pass", "raw_write_java": "pass", "low_heap": "pass",
            **dict.fromkeys(ENDS, "pass"), "done": "1"},
    "modern": {"shim_mapped": "no", "raw_read": "fail", "passback": "pass", "elements": "pass", "global": "pass",
               **SEMANTICS, "java_fill_raw": "fail", "raw_write_java": "skipped", **dict.fromkeys(ENDS, "fail"),
               "done": "1"},
    # The old application before the layer is loaded (a host-shim run's control, or an image's application that is not
    # switched on): what the layer changes.
    "control": {"shim_mapped": "no", "raw_read": "fail", "passback": "pass", "elements": "pass", "global": "pass",
                **SEMANTICS, "java_fill_raw": "fail", "raw_write_java": "skipped", **dict.fromkeys(ENDS, "fail"),
                "done": "1"},
}
DATA = re.compile(r'data="(.*)"', re.DOTALL)


def parse_result(text: str) -> dict[str, str]:
    """The probe's ``key=value`` entries, separated by semicolons or lines."""
    result: dict[str, str] = {}
    for entry in re.split(r"[;\n]", text):
        key, separator, value = entry.strip().partition("=")
        if separator and key:
            result[key] = value
    return result


def broadcast_data(output: str) -> str:
    """The result data `am broadcast` prints after the receiver ran, or an empty string."""
    match = DATA.search(output)
    return match.group(1) if match else ""


def evaluate(variant: str, result: dict[str, str]) -> str:
    """The note of a passing variant; the failure names every line that differs from what the variant must report."""
    wrong = [f"{key}={result.get(key, '<missing>')} (expected {value})"
             for key, value in EXPECTED[variant].items() if result.get(key) != value]
    if wrong:
        raise Failure(f"{variant}: " + ", ".join(wrong) + f"; the file read: {sorted(result.items())}"
                      + (f"; the log: {result['log']}" if "log" in result else ""))
    return ", ".join(f"{key}={value}" for key, value in result.items())


def installs_in_log(text: str) -> int:
    """How many times the log says the layer was installed."""
    return sum(1 for line in text.splitlines() if LOG_TAG in line and INSTALLED in line)


#: The layer's diagnosis line for the first byte[] result that stayed ART's own reference (the real games' sessions read it).
DECLINED = "stayed a real reference"


def lines_in_log(text: str, marker: str) -> int:
    """How many lines of the layer's tag carry `marker`."""
    return sum(1 for line in text.splitlines() if LOG_TAG in line and marker in line)


class Serial:
    """A running guest named by its adb serial: the part of the launcher's Guest that the steps use."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb, self.serial = adb, serial

    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout, check=False)

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()[-500:]}")
        return result.stdout


def install(guest, apk: pathlib.Path, variant: str) -> str:
    """Installs a variant past the block on old targets (what Andarium's installer does for an old application).
    Each run signs with a key of its own, so an earlier copy goes first."""
    guest.adb_run("uninstall", probe.package_of(variant), timeout=60)
    result = guest.adb_run("install", "-r", "--bypass-low-target-sdk-block", str(apk), timeout=INSTALL_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing {probe.package_of(variant)}: {result.stdout.strip()[-400:]}")
    return f"{probe.package_of(variant)} installed"


def layer_in_image(guest) -> str:
    """The library the framework loads is in the image, with the label an application may execute from."""
    listing = guest.shell(f"ls -lZ {probe.SHIM_IMAGE_PATH}").strip()
    if probe.SHIM_IMAGE_LABEL not in listing:
        raise Failure(f"{probe.SHIM_IMAGE_PATH} does not carry {probe.SHIM_IMAGE_LABEL}: {listing[-200:]}")
    return listing


def switch(guest, package: str, on: bool) -> str:
    """Switches the layer on for the application (or off) as the person does, with the image's own command,
    `kernarium_oldapp set PACKAGE on|off` as the adb shell user (a user build has no root shell), and checks the
    guest holds what was set. Where the image has no such command (a release from before it) the property of the
    store is written as root, which the command does for the person."""
    name = probe.STORE_PROPERTY_PREFIX + package
    if guest.shell("command -v kernarium_oldapp", timeout=30).strip().startswith("/"):
        verb = "on" if on else "off"
        result = guest.shell(f"kernarium_oldapp set {package} {verb}; echo rc=$?", timeout=60).strip()
        held = guest.shell(f"getprop {name}").strip()
        if not result.endswith("rc=0") or bool(held) != on:
            raise Failure(f"kernarium_oldapp set {package} {verb}: {result[-200:]!r}; {name} holds '{held}'")
        time.sleep(1)
        return f"{package} {'switched on' if on else 'switched off'} with kernarium_oldapp"
    value = str(int(time.time())) if on else ""
    guest.shell(f"su 0 setprop {name} '{value}'")
    held = guest.shell(f"getprop {name}").strip()
    if held != value:
        raise Failure(f"{name} holds '{held}', not '{value}'")
    return f"{package} {'switched on' if on else 'switched off'}"


def installed_count(guest) -> int:
    return installs_in_log(guest.adb_run("logcat", "-d", "-s", f"{LOG_TAG}:I", timeout=90).stdout)


def declined_count(guest) -> int:
    return lines_in_log(guest.adb_run("logcat", "-d", "-s", f"{LOG_TAG}:I", timeout=90).stdout, DECLINED)


DIAGNOSIS_WORDS = ("FATAL EXCEPTION", "UnsatisfiedLinkError", "Fatal signal", "backtrace:", "signal 11",
                   LOG_TAG, "died")


def diagnosis(guest, package: str) -> str:
    """What the guest's log says about a probe that never reached its last line: the lines about a crash or
    the layer, and the package's own."""
    text = guest.adb_run("logcat", "-d", "-b", "all", "-v", "threadtime", timeout=90).stdout
    lines = [line for line in text.splitlines()
             if " adbd " not in line and (package in line or any(word in line for word in DIAGNOSIS_WORDS))]
    return " | ".join(line.split(": ", 1)[-1][:200] for line in lines[-8:])


def package_library(guest, variant: str, library: str) -> str:
    """The path of a library of the variant's package as the package manager installed it."""
    output = guest.shell(f"pm path {probe.package_of(variant)}", timeout=30)
    paths = [line[len("package:"):].strip() for line in output.splitlines() if line.startswith("package:")]
    if not paths:
        raise Failure(f"{probe.package_of(variant)} is not installed: {output.strip()[-200:]}")
    directory = paths[0].rsplit("/", 1)[0] + "/lib"
    abis = guest.shell(f"ls {shlex.quote(directory)}", timeout=30).split()
    if not abis:
        raise Failure(f"{directory} holds no native libraries: {abis}")
    return f"{directory}/{abis[0]}/lib{library}.so"


def outside_library(guest) -> str:
    """The probe library as the modern probe package installed it: a library of another package than the old probe's,
    which the old process can load (the row ends_outside registers a native method from it)."""
    return package_library(guest, "modern", probe.PROBE_LIBRARY)


def run_variant(guest, variant: str, *, shim: bool, options: str | None = None,
                fresh: bool = True) -> tuple[dict[str, str], int]:
    """Starts the probe with `am broadcast` (the zygote forks an ordinary application process) and reads the probe's
    report from the result data the broadcast prints, and how many installs the log gained meanwhile. A probe that
    did not finish leaves "log" among the entries, with what the guest's log says."""
    package = probe.package_of(variant)
    # A process of an earlier run would take the broadcast with whatever it loaded then (the "peak" run wants it).
    if fresh:
        guest.adb_run("shell", f"am force-stop {package}", timeout=30)
    before = installed_count(guest)
    extras = f" --es shim {probe.SHIM_LIBRARY}" if shim else ""
    if options is not None:
        extras += f" --es opts {shlex.quote(options)}"
    output = guest.shell(f"am broadcast -n {probe.component_of(variant)}{extras}", timeout=BROADCAST_SECONDS)
    if "Broadcast completed" not in output:
        raise Failure(f"the broadcast to {package} was not delivered: {output.strip()[-300:]}")
    result = parse_result(broadcast_data(output))
    if "done" not in result:
        result = {**result, "log": diagnosis(guest, package)}
    return result, installed_count(guest) - before


#: What the scene's log must not say, wherever it comes from: a crash, or a misuse CheckJNI caught.
CRASH_WORDS = ("Fatal signal", "JNI DETECTED ERROR", "Abort message", "SIGILL")
ARENA_LINE = "the low arena has no room"
FIRST_ENDED_LINE = "ended at the return of a native method"
PEAK_LINE = re.compile(r"most live byte\[\] blocks one thread has held; blocks live in the process: (\d+) (\d+)")
#: A line of the layer's tag in `logcat -v brief` (the adb service lines that name the tag are not).
LAYER_LINE = re.compile(r"^[VDIWEF]/" + LOG_TAG + r"\(", re.MULTILINE)
#: The longest a critical region the layer opened stayed open, as it logs it (recorded, not judged).
REGION_LINE = re.compile(r"the longest a critical region of a synchronization stayed open, microseconds: (\d+)")
#: The scene's smallest peak: its init method holds 170 results at once, and a burst more than 100.
SCENE_FLOOR = 170
THREAD_LIMIT = 512
#: The layer logs a growing peak at most every 30 s; the next result after that logs the one that was kept.
PEAK_WAIT_SECONDS = 31


def layer_lines(text: str) -> str:
    """The lines of the layer's tag, as one text."""
    return "\n".join(line for line in text.splitlines() if LAYER_LINE.match(line))


def thread_peak(text: str) -> int:
    """The most blocks one thread held live, as the layer logged it (0 where it did not)."""
    return max((int(match.group(1)) for match in PEAK_LINE.finditer(text)), default=0)


def longest_region(text: str) -> int:
    """The longest time in microseconds a critical region of the layer's own stayed open, as it logged it (0 where it did not)."""
    return max((int(match.group(1)) for match in REGION_LINE.finditer(text)), default=0)


def scene_log_problems(text: str, *, layer: bool) -> list[str]:
    """What a scene's log says that it must not. With the layer: no result that stayed ART's own reference, no
    allocation that left the arena, blocks that ended at returns (the thunks were used), and a peak below the thread's
    limit; without it, no line of the layer at all."""
    problems = [f"the log has '{word}'" for word in CRASH_WORDS if word in text]
    own = layer_lines(text)
    if not layer:
        if own:
            problems.append("the log has lines of the layer in a process that must not load it")
        return problems
    for marker, what in ((DECLINED, "a result stayed ART's own reference"), (ARENA_LINE, "an allocation left the arena")):
        if lines_in_log(own, marker):
            problems.append(what)
    if not lines_in_log(own, FIRST_ENDED_LINE):
        problems.append("no block ended at a native method's return")
    held = thread_peak(own)
    if not SCENE_FLOOR <= held < THREAD_LIMIT:
        problems.append(f"the most live blocks of a thread is {held}, not {SCENE_FLOOR} to {THREAD_LIMIT - 1}")
    return problems


CHECKJNI_LINE = "Late-enabling -Xcheck:jni"


def scene(guest, variant: str, *, shim: bool, layer: bool, checked: bool = False) -> str:
    """The render-thread scene in a fresh process of the variant (one native init method and one per frame, entered
    from Java, with thousands of byte[] results that are never deleted), and then what its log says."""
    guest.adb_run("logcat", "-c", timeout=30)
    result, installs = run_variant(guest, variant, shim=shim, options="scene")
    note = evaluate("scene", result)
    if layer:
        if installs < 1:
            raise Failure(f"the log shows no install of the layer in the scene's process; {note}")
        # The same process once more, after the layer's interval between two lines: the peaks it kept are logged.
        time.sleep(PEAK_WAIT_SECONDS)
        again, _ = run_variant(guest, variant, shim=shim, options="peak", fresh=False)
        if again.get("peak_result") != "pass" or again.get("done") != "1":
            raise Failure(f"the scene's process did not answer the second run: {sorted(again.items())}")
    elif installs != 0:
        raise Failure(f"the layer installed itself {installs} times in a process that must not load it; {note}")
    text = guest.adb_run("logcat", "-d", "-b", "all", "-v", "brief", timeout=90).stdout
    problems = scene_log_problems(text, layer=layer)
    if checked and CHECKJNI_LINE not in text:
        problems.append("the process did not run with CheckJNI")
    if problems:
        raise Failure(f"{variant}: " + "; ".join(problems) + f"; {note}")
    return note + (f"; most live blocks of a thread {thread_peak(layer_lines(text))}"
                   f"; longest region the layer opened {longest_region(layer_lines(text))} us" if layer else "")


def swap(guest, *, shim: bool) -> str:
    """The row bind_swap in a process of its own: the modern package's copy of the probe library is opened in a reserved
    range, enters the layer and is closed, and the old package's second file of the library is opened at the same address
    and bound to a native method; every one of 600 calls must return a block."""
    guest.adb_run("logcat", "-c", timeout=30)
    paths = f"swap|{outside_library(guest)}|{package_library(guest, 'old', probe.PROBE_LIBRARY_COPY)}"
    result, installs = run_variant(guest, "old", shim=shim, options=paths)
    note = evaluate("swap", result)
    if installs < 1:
        raise Failure(f"the log shows no install of the layer in the process of bind_swap; {note}")
    text = guest.adb_run("logcat", "-d", "-b", "all", "-v", "brief", timeout=90).stdout
    problems = [f"the log has '{word}'" for word in CRASH_WORDS if word in text]
    if problems:
        raise Failure("old: " + "; ".join(problems) + f"; {note}")
    return note


def run(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    clang = options.clang or shutil.which("clang++")
    lld = options.lld or shutil.which("ld.lld")
    if not clang or not lld:
        raise Failure("--clang and --lld (or clang++ and ld.lld on PATH) build the armeabi-v7a libraries")
    private.mkdir(parents=True, exist_ok=True)
    probe_library = probe.compile_library((probe.PROBE_SOURCE,), private / f"lib{probe.PROBE_LIBRARY}.so",
                                          f"lib{probe.PROBE_LIBRARY}.so", clang=clang, lld=lld,
                                          needed=probe.PROBE_NEEDED)
    shim_library = None
    if options.host_shim:
        shim_library = probe.compile_library(probe.SHIM_SOURCES, private / f"lib{probe.SHIM_LIBRARY}.so",
                                             f"lib{probe.SHIM_LIBRARY}.so", clang=clang, lld=lld,
                                             needed=probe.SHIM_NEEDED)
    apks = {variant: probe.build_apk(private / f"{variant}.apk", variant, probe_library, shim_library)
            for variant in probe.VARIANTS}
    # The old application again, debuggable: ART runs it with CheckJNI, which aborts on a misuse of the JNI functions.
    debug_apk = probe.build_apk(private / "old-debug.apk", "old", probe_library, shim_library, debuggable=True)
    if options.serial:
        guest = Serial(adb, options.serial)
        stop = lambda: None  # noqa: E731
    else:
        settings = argparse.Namespace(
            release=options.release, emulator=options.emulator, instance=None, adb_port=options.adb_port,
            cpus=options.cpus, memory=options.memory, boot_timeout=options.boot_timeout, no_argument=True,
            extra_arguments=[])
        guest = orient.start_guest(settings, adb, private / "guest", early=None, holder={}, provision=True)
        stop = lambda: orient.stop_guest(adb, guest)  # noqa: E731
    try:
        if not options.host_shim:
            report.run("the layer is in the image", lambda: layer_in_image(guest))
        for variant in probe.VARIANTS:
            report.run(f"{variant}: install", lambda variant=variant: install(guest, apks[variant], variant))
        for variant in probe.VARIANTS:
            report.run(f"{variant}: permission review",
                       lambda variant=variant: permission_review.complete_review(guest, probe.package_of(variant), Failure))

        def old() -> str:
            declined_before = declined_count(guest)
            result, installs = run_variant(guest, "old", shim=options.host_shim, options=outside_library(guest))
            note = evaluate("old", result)
            if not options.host_shim and installs < 1:
                raise Failure(f"the log shows no install of the layer in the old process; {note}")
            # The probe holds 520 results at once: the 513th stays ART's own reference, and the log says so once.
            declined = declined_count(guest) - declined_before
            if declined != 1:
                raise Failure(f"the log shows {declined} lines for the first declined shadow of the process, not 1; {note}")
            return f"{note}; installs logged: {installs}; declined-shadow lines: {declined}"

        def modern() -> str:
            result, installs = run_variant(guest, "modern", shim=False, options=outside_library(guest))
            note = evaluate("modern", result)
            if installs != 0:
                raise Failure(f"the layer installed itself in the modern process {installs} times; {note}")
            return note

        def control() -> str:
            result, installs = run_variant(guest, "old", shim=False, options=outside_library(guest))
            note = evaluate("control", result)
            if installs != 0:
                raise Failure(f"the layer installed itself in an old process that is not switched on {installs} times; {note}")
            return note

        package = probe.package_of("old")

        def old_scene(checked: bool = False) -> None:
            what = "old, debuggable (CheckJNI)" if checked else "old"
            report.run(f"{what}: an application's render thread (one init, one native method per frame)",
                       lambda: scene(guest, "old", shim=options.host_shim, layer=True, checked=checked))

        def named_scene() -> None:
            report.run("old, process named otherwise: the render thread's scene still gets thunks",
                       lambda: scene(guest, "named", shim=options.host_shim, layer=True))

        if options.host_shim:
            report.run("old without the layer: the reads fail (control)", control)
            report.run("old without the layer: the render thread's scene", lambda: scene(guest, "old", shim=False, layer=False))
            report.run("old: the application's reads, references and allocations", old)
            old_scene()
            named_scene()
            report.run("old: a native method bound where another package's library was closed gets its thunk",
                       lambda: swap(guest, shim=True))
        else:
            report.run("old, not switched on: the layer is left out", control)
            report.run("old, not switched on: the render thread's scene", lambda: scene(guest, "old", shim=False, layer=False))
            report.run("old: switched on", lambda: switch(guest, package, True))
            try:
                report.run("old: the application's reads, references and allocations", old)
                old_scene()
                report.run("old: a native method bound where another package's library was closed gets its thunk",
                           lambda: swap(guest, shim=False))
            finally:
                switch(guest, package, False)
            named_package = probe.package_of("named")
            report.run("old, process named otherwise: switched on", lambda: switch(guest, named_package, True))
            try:
                named_scene()
            finally:
                switch(guest, named_package, False)
        report.run("modern: the layer is left out", modern)
        report.run("modern: the render thread's scene", lambda: scene(guest, "modern", shim=False, layer=False))
        report.run("old, debuggable: install", lambda: install(guest, debug_apk, "old"))
        report.run("old, debuggable: permission review", lambda: permission_review.complete_review(guest, package, Failure))
        if not options.host_shim:
            report.run("old, debuggable: switched on", lambda: switch(guest, package, True))
        try:
            old_scene(checked=True)
        finally:
            if not options.host_shim:
                switch(guest, package, False)
    finally:
        stop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--serial", help="a running guest (host:port); no guest is launched")
    parser.add_argument("--host-shim", action="store_true",
                        help="build the layer on the host, bundle it in the packages and load it from the old "
                        "probe: for an image that does not carry it")
    parser.add_argument("--clang", help="clang++ that builds armv7 (default: on PATH)")
    parser.add_argument("--lld", help="ld.lld (default: on PATH)")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int,
                        default=orient.slot_ports.slot_port(orient.slot_ports.ADB_OFFSET, orient.DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {orient.DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900, help="seconds to wait for adbd and boot_completed")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = orient.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-oldapp-"))
    report = Report()
    try:
        run(options, adb, private, report)
        print("\n".join(f"{status:4} {name}: {detail}" for name, status, detail in report.rows))
        print("FAIL" if report.failed else "PASS")
        return 1 if report.failed else 0
    except (Failure, orient.boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        for log in sorted(private.glob("*/emulator.log")):
            print(f"{log.parent.name} launcher log tail:\n"
                  + "\n".join(log.read_text(errors="replace").splitlines()[-30:]), file=sys.stderr)
        return 1
    finally:
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
