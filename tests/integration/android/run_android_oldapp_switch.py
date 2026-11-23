#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the shell command and the store of Old app compatibility on an image.

android/lineage-24/overlay/oldapp_switch is `kernarium_oldapp`, a command that Andarium runs as the adb shell
user: ``list`` names the applications the compatibility layer can apply to (not a system application, a 32-bit
primary ABI, a target level below 14, no shared user ID, a package name that can end a property name) with their
labels, ``set PACKAGE on|off`` writes or clears the persistent property ``persist.sys.kernarium.oldapp.<package>``
and stops the application. The property's SELinux type (overlay/sepolicy/system_ext/private/kernarium_oldapp_prop.te)
lets the shell domain set it and every application domain read it, and no application domain set it. The value
names the installation (turn-on seconds, a space, the installation's first-install milliseconds) and a choice does
not outlive it: ``list`` clears the property of a package that is absent, is not eligible any more or is another
installation than the value names, and turning off is accepted for any name that can be a property name. This runner installs the probe applications of oldswitch_probe_apk.py (one eligible,
three that each fail one clause of the rule) and, as the adb shell user, checks that:

- the command and its jar are in the image and `kernarium_oldapp` resolves on the shell's PATH;
- ``list`` starts with the version line, names the eligible application once, with its label (the TAB of the
  manifest's string turned into a space), ``off``, a turn-on time of 0 and a last-update time between the
  install's start and now, and names none of the others;
- ``set on`` refuses an application that fails a clause, a system application and a package that is not installed
  with status 2 and one line on standard error, and ``set off`` for the same packages exits 0 with ``<package>
  <TAB>off``; a name that cannot be a property name (an empty segment, a trailing dot) is refused for both; the
  property stays empty throughout; unknown arguments are status 64;
- ``set old on`` exits 0 with ``<package><TAB>on``, the property holds the turn-on time (the guest's clock, which
  is the host's), and ``list`` shows ``on`` with that time;
- an ordinary application (the modern probe, domain ``untrusted_app_29``, started with `am broadcast`) reads the
  property with that value and its attempt to set it is refused, with the value unchanged;
- ``set old off`` exits 0 with ``<package><TAB>off``, the property reads empty and ``list`` shows ``off``;
- a choice does not outlive its installation: after the application is uninstalled and installed again (at once,
  with no regard to the clock), or updated in place by a package that is not eligible any more (the ``raised``
  probe, signed with the key of ``old``, installed over it without uninstalling: the first-install time is
  unchanged, so only the eligibility rule can clear the choice), or replaced by a package of the same name with
  a shared user ID (the ``shared`` probe is uninstalled first, because two packages of one shared user ID need
  one signing key and it has a key of its own), or only uninstalled, or the property holds the earlier
  one-number form or another installation's value, the next ``list`` clears the property and shows the
  application ``off`` or not at all;
- the command's lock is a file of the shell user in its own data directory (type ``shell_data_file``) that only
  its owner can read and write (mode 600), and one that was loosened (mode 666) is tightened by the next command
  that takes the lock: a ``set``, and a ``list`` that has a stale choice to clear (a ``list`` with nothing to
  clear does not use the lock).

``--hook`` (an image that also carries the framework hook and the compatibility library) adds the steps that
need an old application's own process: the process of the ``running`` probe, started by `am broadcast` in the
hook's domain (``untrusted_app_25``), reads the property and cannot set it (its refused `setprop` leaves an avc
line in the log, which the step requires and prints: the line of the `setprop` process the probe reports, in the
probe's domain, naming the probe's property where the audit form does, newer than a baseline read that must
itself succeed; on a user build the refusal is at the property service's socket, before the property's own rule;
the runner presses Continue on the permission review that an application targeting below API 23 gets before
its first component runs, after each install of that probe); switching on or off ends that process; the process
that starts afterwards has the layer's library mapped exactly while the switch is on; and a stale choice is gone
from the next process. Not
automated here: that the choice survives a restart or an Android update of the guest (the image proof's own
recipe restarts the guest), that a snapshot restore brings it back, that an in-place update of an eligible
application keeps it (the probe packages are signed with a key of their own per build, so none can update
another), that a choice made after the guest's clock was set back is kept (the runner does not change the
guest's clock; the command's JVM test does), and that a listing and a set racing each other lose no choice
(the JVM test serialises them against the production core).

``--real-apk`` (with ``--real-package`` and ``--real-label``) also installs an application you provide, whose
label is a resource of its own package, and requires the listing to show that label: the probe's label is a
literal, so only this application exercises the resource path. The command stops a running application with
`cmd activity force-stop`; without ``--hook`` the runner checks that it exits 0, not that a process died.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest is
reached through the shared adb server, always named with `-s`.
usage: run_android_oldapp_switch.py --emulator uml-darwin-android17 [--release DIR] [--serial HOST:PORT] [--hook]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import NamedTuple

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import oldswitch_probe_apk as probe  # noqa: E402
import permission_review  # noqa: E402

# The orientation runner owns the guest: its launcher and its adb.
SPEC = importlib.util.spec_from_file_location("run_android_orientation", HERE / "run_android_orientation.py")
assert SPEC is not None and SPEC.loader is not None
orient = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = orient  # dataclasses resolve their annotations through sys.modules
SPEC.loader.exec_module(orient)
Failure = orient.Failure
Report = orient.Report

COMMAND = "kernarium_oldapp"
WRAPPER_PATH = "/system_ext/bin/kernarium_oldapp"
JAR_PATH = "/system_ext/framework/kernarium_oldapp.jar"
VERSION_LINE = "kernarium_oldapp 1"
SHELL_CONTEXT = "u:r:shell:s0"
EXIT_REFUSED = 2
EXIT_USAGE = 64
INSTALL_SECONDS = 180
COMMAND_SECONDS = 120
PROBE_SECONDS = 60
#: Seconds between an install and the update over it, so that the two are told apart to the second.
UPDATE_GAP_SECONDS = 2
#: The system application the command must refuse.
SYSTEM_PACKAGE = "com.android.settings"
UNKNOWN_PACKAGE = "org.example.andarium.oldswitchprobe.notinstalled"
#: Package names the package manager installs and a property name cannot hold (an empty segment, a trailing dot).
UNREPRESENTABLE_PACKAGES = ("org.example..oldapp", "org.example.oldapp.")
#: The lock file the command takes (the shell user's own data directory) and the type its directory gives it.
LOCK_PATH = "/data/local/tmp/kernarium_oldapp.lock"
LOCK_TYPE = "shell_data_file"
#: Its mode: readable and writable by its owner alone, so that no application can open it and hold the lock.
LOCK_MODE = "-rw-------"
#: What the property holds: the turn-on time in seconds, a space, the installation's first-install time in milliseconds.
CHOICE_VALUE = re.compile(r"([0-9]+) ([0-9]+)")
#: How long the log is given to show the denial of the application's set.
AVC_SECONDS = 10
#: The domain of the hook's probe, and the two lines its refused `setprop` can leave in the log. A user build refuses it
#: at the property service's socket (an application domain may not write it), before the property's own rule is asked:
#:   avc: denied { write } for comm="setprop" name="property_service" scontext=u:r:untrusted_app_25:... \
#:     tcontext=u:object_r:property_socket:s0 tclass=sock_file
#: A domain that may reach the socket is refused by the rule of the property's type instead:
#:   avc: denied { set } for property=persist.sys.kernarium.oldapp.<package> ... scontext=u:r:untrusted_app_25:... \
#:     tcontext=u:object_r:kernarium_oldapp_prop:s0 tclass=property_service
HOOK_DOMAIN = "u:r:untrusted_app_25:"
#: Slack between the host's reading of the guest's clock and a time the guest wrote.
CLOCK_SLACK_SECONDS = 5


class Row(NamedTuple):
    package: str
    label: str
    state: str
    turned_on: int
    last_update: int


class Run(NamedTuple):
    status: int
    out: str
    err: str


def parse_listing(text: str) -> list[Row]:
    """The rows of `kernarium_oldapp list`: the version line first, then five TAB-separated fields on each line."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    else:
        raise Failure(f"the listing does not end with a line break: {text[-80:]!r}")
    if not lines or lines[0] != VERSION_LINE:
        raise Failure(f"the listing starts with {lines[:1]!r}, not {VERSION_LINE!r}")
    rows = []
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) < 5:
            raise Failure(f"a listing line has {len(fields)} fields, not five: {line!r}")
        package, label, state, turned_on, last_update = fields[:5]
        if state not in ("on", "off") or not turned_on.isdigit() or not last_update.isdigit():
            raise Failure(f"a listing line is malformed: {line!r}")
        rows.append(Row(package, label, state, int(turned_on), int(last_update)))
    return rows


def parse_result(text: str) -> dict[str, str]:
    """The probe application's ``key=value`` lines; a line without ``=`` is a key with an empty value (``done``)."""
    result: dict[str, str] = {}
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key.strip():
            result[key.strip()] = value.strip()
    return result


def evaluate_process(result: dict[str, str], value: str, mapped: bool) -> int:
    """The process id of an old application's process that read `value` (empty for off), could not set the
    property and had the layer's library mapped or not; raises when the view differs."""
    context = result.get("context", "")
    if not context.startswith("u:r:untrusted_app_25:"):
        raise Failure(f"the process is not in the old application's domain untrusted_app_25: context {context!r}")
    wrong = [f"{key}={result.get(key, '<missing>')!r} (expected {expected!r})"
             for key, expected in (("get", value), ("set", "denied"), ("after", value), ("done", ""))
             if result.get(key) != expected]
    if wrong:
        raise Failure("the old application's view of its property differs: " + ", ".join(wrong))
    if not result.get("pid", "").isdigit():
        raise Failure(f"the process reported no process id: {result.get('pid')!r}")
    if not result.get("setpid", "").isdigit():
        raise Failure(f"the process reported no process id for its setprop: setpid={result.get('setpid')!r}")
    layer = result.get("layer", "")
    if not layer.isdigit():
        raise Failure(f"the process could not read its own memory map: layer={layer!r}")
    if (int(layer) > 0) != mapped:
        raise Failure(f"the layer's library is {'mapped' if int(layer) else 'not mapped'} in process "
                      f"{result['pid']}; it must be {'mapped' if mapped else 'absent'} while the switch is "
                      f"{'on' if mapped else 'off'}")
    return int(result["pid"])


def evaluate_probe(result: dict[str, str], expected: str) -> str:
    """The note of a passing probe: an ordinary application read the value the command stored and could not
    change it."""
    context = result.get("context", "")
    if not context.startswith("u:r:untrusted_app_"):
        raise Failure(f"the probe did not run as an ordinary application: context {context!r}")
    wrong = [f"{key}={result.get(key, '<missing>')!r} (expected {value!r})"
             for key, value in (("get", expected), ("set", "denied"), ("after", expected), ("done", ""))
             if result.get(key) != value]
    if wrong:
        raise Failure("the application's view of the property differs: " + ", ".join(wrong))
    return f"{context.split(':s0')[0]}:s0 read {expected} and could not set it"


class Serial:
    """A running guest named by its adb serial."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb, self.serial = adb, serial

    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, timeout=timeout, check=False)

    def run(self, command: str, timeout: float = COMMAND_SECONDS) -> Run:
        """A command in the adb shell: its exit status (adb's shell protocol carries it) and its two streams."""
        result = self.adb_run("shell", command, timeout=timeout)
        return Run(result.returncode, result.stdout, result.stderr)

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {(result.out + result.err).strip()[-500:]}")
        return result.out


def guest_time(guest) -> int:
    return int(guest.shell("date +%s").strip())


def install(guest, apk: pathlib.Path, package: str) -> None:
    """Installs past the block on old targets (what Andarium's installer does for an old application). Each run
    signs with a key of its own, so an earlier copy goes first."""
    guest.adb_run("uninstall", package, timeout=60)
    update(guest, apk, package)


def update(guest, apk: pathlib.Path, package: str) -> None:
    """Installs over the installed package, which must be signed with the same key: an update in place, which keeps
    the installation (and its first-install time)."""
    result = guest.adb_run("install", "-r", "--bypass-low-target-sdk-block", str(apk), timeout=INSTALL_SECONDS)
    if result.returncode != 0 or "Success" not in result.stdout:
        raise Failure(f"installing {package}: {(result.stdout + result.stderr).strip()[-400:]}")


class Times(NamedTuple):
    first_install: str
    last_update: str


def installation_times(guest, package: str) -> Times:
    """What the package manager prints of the installation (`dumpsys package`: the first-install and last-update times
    to the second). An update in place moves the second and keeps the first; a reinstall moves both."""
    dump = guest.shell(f"dumpsys package {package}", timeout=60)
    found = [re.search(rf"^\s*{name}=(.+?)\s*$", dump, re.MULTILINE) for name in ("firstInstallTime", "lastUpdateTime")]
    if not all(found):
        raise Failure(f"`dumpsys package {package}` prints no firstInstallTime and lastUpdateTime")
    return Times(found[0].group(1), found[1].group(1))


def command_present(guest) -> str:
    """The wrapper and the jar are in the image, the wrapper resolves on the shell's PATH, and the shell is
    the shell domain (the policy under test is the shell domain's)."""
    listing = guest.shell(f"ls -lZ {WRAPPER_PATH} {JAR_PATH}")
    if WRAPPER_PATH not in listing or JAR_PATH not in listing:
        raise Failure(f"the image lacks the command: {listing.strip()[-300:]}")
    resolved = guest.shell(f"command -v {COMMAND}").strip()
    if resolved != WRAPPER_PATH:
        raise Failure(f"{COMMAND} resolves to {resolved!r}, not {WRAPPER_PATH}")
    context = guest.shell("id -Z").strip()
    if not context.startswith(SHELL_CONTEXT):
        raise Failure(f"adb shell runs in {context!r}, not {SHELL_CONTEXT}")
    return f"{resolved}; adb shell is {context}"


def listing_of(guest) -> list[Row]:
    result = guest.run(f"{COMMAND} list")
    if result.status != 0 or result.err.strip():
        raise Failure(f"'{COMMAND} list' exited {result.status}: {(result.out + result.err).strip()[-500:]}")
    return parse_listing(result.out)


def row_of(rows: list[Row], variant: str) -> Row | None:
    found = [row for row in rows if row.package == probe.package_of(variant)]
    if len(found) > 1:
        raise Failure(f"{probe.package_of(variant)} is listed {len(found)} times")
    return found[0] if found else None


def check_listing(guest, installed_at: int) -> str:
    rows = listing_of(guest)
    now = guest_time(guest)
    old = row_of(rows, "old")
    if old is None:
        raise Failure(f"the eligible probe is not listed: {[row.package for row in rows]}")
    if old.label != probe.LISTED_LABEL:
        raise Failure(f"the label reads {old.label!r}, not {probe.LISTED_LABEL!r}")
    if (old.state, old.turned_on) != ("off", 0):
        raise Failure(f"the eligible probe starts as {old.state} {old.turned_on}, not off 0")
    if not installed_at - CLOCK_SLACK_SECONDS <= old.last_update <= now + CLOCK_SLACK_SECONDS:
        raise Failure(f"last-update {old.last_update} lies outside the install window [{installed_at}, {now}]")
    unlisted = [probe.package_of(variant) for variant in probe.REFUSED if row_of(rows, variant) is not None]
    if unlisted or any(row.package == SYSTEM_PACKAGE for row in rows):
        raise Failure(f"applications that are not eligible are listed: {unlisted or SYSTEM_PACKAGE}")
    return f"{len(rows)} application(s) listed; {old.package} {old.label!r} off, updated {old.last_update}"


def property_value(guest, name: str) -> str:
    return guest.shell(f"getprop {name}").strip()


def check_refusals(guest) -> str:
    notes = []
    targets = [(probe.package_of(variant), probe.property_of(variant)) for variant in probe.REFUSED]
    targets += [(SYSTEM_PACKAGE, probe.STORE_PROPERTY_PREFIX + SYSTEM_PACKAGE),
                (UNKNOWN_PACKAGE, probe.STORE_PROPERTY_PREFIX + UNKNOWN_PACKAGE)]
    for package, name in targets:
        result = guest.run(f"{COMMAND} set {package} on")
        lines = result.err.strip().splitlines()
        if result.status != EXIT_REFUSED or result.out.strip() or len(lines) != 1:
            raise Failure(f"set {package} on: status {result.status}, out {result.out.strip()!r}, "
                          f"err {result.err.strip()!r}; expected status {EXIT_REFUSED} and one line on stderr")
        # Off is accepted for any name that can be a property name, so that a choice nothing else could
        # turn off can still be turned off.
        result = guest.run(f"{COMMAND} set {package} off")
        if result.status != 0 or result.out != f"{package}\toff\n" or result.err.strip():
            raise Failure(f"set {package} off: status {result.status}, out {result.out!r}, "
                          f"err {result.err.strip()!r}; expected status 0 and '{package}<TAB>off'")
        if property_value(guest, name):
            raise Failure(f"{name} holds a value after a refusal or an off")
        notes.append(package.rsplit(".", 1)[-1])
    for package in UNREPRESENTABLE_PACKAGES:
        for mode in ("on", "off"):
            result = guest.run(f"{COMMAND} set {package} {mode}")
            if result.status != EXIT_REFUSED or result.out.strip() or len(result.err.strip().splitlines()) != 1:
                raise Failure(f"set {package} {mode}: status {result.status}, out {result.out.strip()!r}, "
                              f"err {result.err.strip()!r}; a name that cannot be a property name is refused "
                              f"(status {EXIT_REFUSED}, one line on stderr)")
    for arguments in ("", "list extra", "set", f"set {probe.package_of('old')}", f"set {probe.package_of('old')} maybe",
                      f"set {probe.package_of('old')} on extra", "frobnicate"):
        result = guest.run(f"{COMMAND} {arguments}".strip())
        if result.status != EXIT_USAGE or not result.err.startswith("usage: ") or result.out.strip():
            raise Failure(f"'{COMMAND} {arguments}': status {result.status}, out {result.out.strip()!r}, "
                          f"err {result.err.strip()!r}; expected status {EXIT_USAGE} and the usage on stderr")
    return ("on refused (status 2, one line), off accepted: " + ", ".join(notes)
            + "; unrepresentable names refused; unknown arguments: status 64")


def switch_on(guest) -> tuple[str, str]:
    """Turns the eligible probe on and returns the note and the property's value (turn-on seconds, a space and
    the installation's first-install time in milliseconds)."""
    package, name = probe.package_of("old"), probe.property_of("old")
    before = guest_time(guest)
    result = guest.run(f"{COMMAND} set {package} on")
    after = guest_time(guest)
    if result.status != 0 or result.out != f"{package}\ton\n" or result.err.strip():
        raise Failure(f"set {package} on: status {result.status}, out {result.out!r}, err {result.err.strip()!r}")
    value = property_value(guest, name)
    seconds = choice_seconds(value, name)
    if not before - CLOCK_SLACK_SECONDS <= seconds <= after + CLOCK_SLACK_SECONDS:
        raise Failure(f"{name} holds {value!r}: the turn-on time is not between {before} and {after}")
    row = row_of(listing_of(guest), "old")
    if row is None or (row.state, row.turned_on) != ("on", seconds):
        raise Failure(f"the listing shows {row} after set on; the property holds {value}")
    return f"{name} = {value}; listed on", value


def choice_seconds(value: str, name: str) -> int:
    """The turn-on seconds of a stored value, which must be the two numbers the command writes."""
    match = CHOICE_VALUE.fullmatch(value)
    if match is None or int(match.group(2)) <= 0:
        raise Failure(f"{name} holds {value!r}, not '<turn-on seconds> <first-install milliseconds>'")
    return int(match.group(1))


def lock_listing(guest) -> str:
    """`ls -lZ` of the lock file, which must be a file of the shell user in its own data directory."""
    listing = guest.shell(f"ls -lZ {LOCK_PATH}").strip()
    if LOCK_PATH not in listing or LOCK_TYPE not in listing or " shell " not in f" {listing} ":
        raise Failure(f"the lock file is not a {LOCK_TYPE} file of the shell user: {listing[-200:]}")
    return listing


def check_lock_file(guest) -> str:
    """The command's lock is a file the shell domain made in its own data directory, closed to every other user;
    one that was loosened is tightened by the next command (which takes the lock, so a command still works)."""
    listing = lock_listing(guest)
    if listing.split()[0] != LOCK_MODE:
        raise Failure(f"the lock file is not {LOCK_MODE} (an application could open it): {listing[-200:]}")
    guest.shell(f"chmod 666 {LOCK_PATH}")
    result = guest.run(f"{COMMAND} set {UNKNOWN_PACKAGE} off")
    if result.status != 0 or result.out != f"{UNKNOWN_PACKAGE}\toff\n" or result.err.strip():
        raise Failure(f"set {UNKNOWN_PACKAGE} off over a loosened lock file: status {result.status}, "
                      f"out {result.out!r}, err {result.err.strip()!r}")
    tightened = lock_listing(guest)
    if tightened.split()[0] != LOCK_MODE:
        raise Failure(f"the command did not tighten the lock file: {tightened[-200:]}")
    # A listing that has a stale choice to clear (the earlier one-number form, for a package that is not installed)
    # takes the lock for it and so makes the file safe too. One with nothing to clear does not use the lock.
    stale = probe.STORE_PROPERTY_PREFIX + UNKNOWN_PACKAGE
    guest.shell(f"chmod 666 {LOCK_PATH}")
    guest.shell(f"setprop {stale} '1700000000'")
    try:
        listing_of(guest)
        if property_value(guest, stale):
            raise Failure(f"{stale} still holds a value after a listing: the cleanup did not run")
        cleaned = lock_listing(guest)
        if cleaned.split()[0] != LOCK_MODE:
            raise Failure(f"a listing that cleared a choice left the lock file loose: {cleaned[-200:]}")
    finally:
        guest.shell(f"setprop {stale} ''")
    return f"{LOCK_TYPE}, {listing.split()[0]}; a loosened file is tightened again by a set and by a listing that cleans"


def probe_result(guest, variant: str) -> dict[str, str]:
    """Starts a process of the variant's receiver with `am broadcast` and returns the lines its script wrote."""
    package = probe.package_of(variant)
    guest.adb_run("shell", f"run-as {package} rm -f {probe.RESULT_FILE}", timeout=30)
    broadcast = guest.shell(f"am broadcast -n {probe.component_of(variant)}", timeout=60)
    if "Broadcast completed" not in broadcast:
        raise Failure(f"the probe broadcast was not delivered: {broadcast.strip()[-300:]}")
    deadline = time.monotonic() + PROBE_SECONDS
    result: dict[str, str] = {}
    while time.monotonic() < deadline:
        read = guest.run(f"run-as {package} cat {probe.RESULT_FILE}", timeout=30)
        result = parse_result(read.out) if read.status == 0 else {}
        if "done" in result:
            return result
        time.sleep(1)
    raise Failure(f"the probe application wrote no result in {PROBE_SECONDS} s: {result}")


def application_view(guest, value: str) -> str:
    """The modern probe, an ordinary application, reads the eligible application's property and tries to set it."""
    result = probe_result(guest, "modern")
    note = evaluate_probe(result, value)
    held = property_value(guest, probe.property_of("old"))
    if held != value:
        raise Failure(f"the property reads {held!r} after the application's attempt, not {value!r}")
    return note


def switch_off(guest) -> str:
    package, name = probe.package_of("old"), probe.property_of("old")
    result = guest.run(f"{COMMAND} set {package} off")
    if result.status != 0 or result.out != f"{package}\toff\n" or result.err.strip():
        raise Failure(f"set {package} off: status {result.status}, out {result.out!r}, err {result.err.strip()!r}")
    held = property_value(guest, name)
    if held:
        raise Failure(f"{name} holds {held!r} after set off")
    row = row_of(listing_of(guest), "old")
    if row is None or (row.state, row.turned_on) != ("off", 0):
        raise Failure(f"the listing shows {row} after set off")
    return f"{name} is empty; listed off"


def cleared_off(guest, package: str, what: str) -> None:
    result = guest.run(f"{COMMAND} set {package} off")
    if result.status != 0 or result.out != f"{package}\toff\n" or result.err.strip():
        raise Failure(f"set {package} off with {what}: status {result.status}, out {result.out!r}, "
                      f"err {result.err.strip()!r}")


def check_lifecycle(guest, apks: dict[str, pathlib.Path]) -> str:
    """A choice does not outlive the installation it was made for: the next listing clears it. The property
    survives the uninstall (it is on /data under its own name), so each case first looks for it still there."""
    package, name = probe.package_of("old"), probe.property_of("old")
    notes = []

    def switched_on() -> str:
        result = guest.run(f"{COMMAND} set {package} on")
        if result.status != 0 or result.out != f"{package}\ton\n":
            raise Failure(f"set {package} on: status {result.status}, out {result.out!r}, err {result.err.strip()!r}")
        value = property_value(guest, name)
        choice_seconds(value, name)
        return value

    def still_stored(value: str, what: str) -> None:
        held = property_value(guest, name)
        if held != value:
            raise Failure(f"{name} holds {held!r} after {what}, not {value}: the cleanup is not exercised")

    # Uninstalled and installed again at once: another installation, whatever the clock and the second say.
    value = switched_on()
    install(guest, apks["old"], package)
    still_stored(value, "the reinstall")
    row = row_of(listing_of(guest), "old")
    if row is None or (row.state, row.turned_on) != ("off", 0):
        raise Failure(f"after a reinstall the listing shows {row}, not the application off")
    if property_value(guest, name):
        raise Failure(f"{name} still holds a value after the listing of a reinstalled application")
    notes.append("reinstalled: cleared")

    # Updated in place by a package that raises the target level: signed with the same key, installed over the
    # installed package without uninstalling it. The package manager keeps the installation, so the choice still
    # names it, and only the rule that a package that is not eligible any more keeps no choice can clear it
    # (after a reinstall the rule of the installation would clear it whatever the eligibility rule says).
    install(guest, apks["old"], package)
    value = switched_on()
    before = installation_times(guest, package)
    time.sleep(UPDATE_GAP_SECONDS)
    update(guest, apks["raised"], package)
    after = installation_times(guest, package)
    if after.first_install != before.first_install or after.last_update == after.first_install:
        raise Failure(f"the raised package did not update the installation in place: first install "
                      f"{before.first_install} then {after.first_install}, last update {after.last_update}")
    still_stored(value, "the update in place")
    if row_of(listing_of(guest), "old") is not None:
        raise Failure("the package is listed after the update that raised its target level")
    if property_value(guest, name):
        raise Failure(f"{name} still holds a value after the listing, a raised target level (the installation is "
                      f"the same, first installed {after.first_install})")
    cleared_off(guest, package, "a raised target level")
    notes.append("a raised target level, updated in place: cleared")

    # Replaced by the same package name with a shared user ID. The shared probe goes first: two packages of one
    # shared user ID need one signing key, the shared probe has a key of its own, and its rows are done.
    guest.adb_run("uninstall", probe.package_of("shared"), timeout=60)
    install(guest, apks["old"], package)
    value = switched_on()
    install(guest, apks["oldshared"], package)
    still_stored(value, "the replacement with a shared user ID")
    if row_of(listing_of(guest), "old") is not None:
        raise Failure("the package is listed after the replacement with a shared user ID")
    if property_value(guest, name):
        raise Failure(f"{name} still holds a value after the listing, a shared user ID")
    cleared_off(guest, package, "a shared user ID")
    notes.append("a shared user ID: cleared")

    # Only uninstalled.
    install(guest, apks["old"], package)
    value = switched_on()
    guest.adb_run("uninstall", package, timeout=60)
    still_stored(value, "the uninstall")
    if row_of(listing_of(guest), "old") is not None:
        raise Failure("an uninstalled package is listed")
    if property_value(guest, name):
        raise Failure(f"{name} still holds a value after the listing of an uninstalled package")
    notes.append("uninstalled: cleared")

    # A value that names no installation, or another one, is cleared. The shell may set the property, which
    # is how the earlier one-number form (a proof image wrote it) and a foreign installation are made here.
    install(guest, apks["old"], package)
    for text, what in (("1700000000", "the earlier one-number form"), ("1700000000 5", "another installation"),
                       ("on", "a value that is no time")):
        guest.shell(f"setprop {name} '{text}'")
        if property_value(guest, name) != text:
            raise Failure(f"the shell could not set {name} to {text!r}")
        row = row_of(listing_of(guest), "old")
        if row is None or (row.state, row.turned_on) != ("off", 0):
            raise Failure(f"with {what} the listing shows {row}, not the application off")
        if property_value(guest, name):
            raise Failure(f"{name} still holds a value after the listing, {what}")
        notes.append(f"{what}: cleared")
    return "; ".join(notes)


def process_ids(guest, package: str) -> list[int]:
    """The process ids of the application (`pidof` exits 1 with no output when it has none)."""
    return [int(word) for word in guest.run(f"pidof {package}").out.split() if word.isdigit()]


def denials(guest) -> list[str]:
    """The auditd lines of denials to an old application's domain in the whole log. A log that cannot be read is
    a failure, never an empty log."""
    read = guest.run("logcat -d -b all", timeout=60)
    if read.status != 0:
        raise Failure(f"the log could not be read: logcat -d -b all exited {read.status}: "
                      f"{(read.out + read.err).strip()[-200:]}")
    return [line.strip() for line in read.out.splitlines()
            if "avc" in line and "denied" in line and f"scontext={HOOK_DOMAIN}" in line]


def denied_process(line: str) -> int | None:
    """The process a denial is about: the `pid=` the audit text carries, or else the process id that logcat prints
    for the line (logd takes the denied process's from the audit record)."""
    match = re.search(r"\bpid=(\d+)", line) or re.match(r"\s*\d\d-\d\d\s+[\d:.]+\s+(\d+)\s+\d+\s+\w\s", line)
    return int(match.group(1)) if match else None


def denial_kind(line: str, context: str, process: int, name: str) -> str | None:
    """Which refusal of the probe's `setprop` the line records, or None when it is another denial: one of the
    process `process` (the `setprop` the probe started, in the domain `context`), and for the property's own rule,
    of the property `name`."""
    source = re.search(r"\bscontext=(\S+)", line)
    if denied_process(line) != process or source is None or source.group(1) != context:
        return None
    if ('comm="setprop"' in line and "{ write }" in line and "tcontext=u:object_r:property_socket:" in line
            and "tclass=sock_file" in line):
        return "at the property service's socket"
    named = re.search(r"\bproperty=(\S+)", line)
    if ("{ set }" in line and named is not None and named.group(1) == name
            and "tcontext=u:object_r:kernarium_oldapp_prop:" in line and "tclass=property_service" in line):
        return "by the rule of the property's type"
    return None


def denial_summary(line: str) -> str:
    """The process, the permission, the process name, the target type and class of a denial line, for a message."""
    parts = [re.search(pattern, line) for pattern in (r"(pid=\d+)", r"denied\s+(\{[^}]*\})", r'(comm="[^"]*")',
                                                     r"(property=\S+)", r"tcontext=u:object_r:([^:\s]+)",
                                                     r"tclass=(\S+)")]
    return " ".join(part.group(1) for part in parts if part)


def denial_note(guest, before: set[str], result: dict[str, str] | None, name: str) -> str:
    """The refusal of the probe's set that the log gained since `before`: the denial of the `setprop` process the
    probe reported, in the probe's domain, naming the probe's property where the audit form does. A denial of
    another process or property is not this probe's evidence, and a log that cannot be read is no evidence."""
    if result is None:
        raise Failure("the probe reported no result of its set, so there is no process to find a denial of")
    context, process = result["context"], int(result["setpid"])
    deadline = time.monotonic() + AVC_SECONDS
    unreadable = ""
    while True:
        new: list[str] = []
        try:
            new = [line for line in denials(guest) if line not in before]
            unreadable = ""
        except Failure as problem:
            unreadable = str(problem)
        for line in new:
            kind = denial_kind(line, context, process, name)
            if kind:
                return f"setprop process {process} refused {kind}: {line[line.index('avc'):][:300]}"
        if time.monotonic() >= deadline:
            seen = sorted({denial_summary(line) for line in new})
            raise Failure(f"no avc line for the probe's refused set (setprop process {process} in {context}, "
                          f"property {name}) within {AVC_SECONDS} s: "
                          + (unreadable if unreadable else f"{len(new)} new denial(s) to {HOOK_DOMAIN}*"
                             + (f", none of that process ({'; '.join(seen[:4])})" if seen else ""))
                          + "; a refused setprop at the property service's socket, or a refused set of the "
                          "property, of that process is what the step requires")
        time.sleep(1)


def check_hook(guest, apks: dict[str, pathlib.Path], report: Report) -> None:
    """The steps that need the framework hook: an old application's own process and the layer in it."""
    package, name = probe.package_of("running"), probe.property_of("running")
    state: dict = {}

    def installed() -> str:
        install(guest, apks["running"], package)
        review = permission_review.complete_review(guest, package, Failure)
        row = next((row for row in listing_of(guest) if row.package == package), None)
        if row is None or (row.state, row.turned_on) != ("off", 0):
            raise Failure(f"the running probe is listed as {row}, not off")
        return f"{package} installed ({review}) and listed off"

    def off_process() -> str:
        pid = evaluate_process(probe_result(guest, "running"), "", mapped=False)
        if pid not in process_ids(guest, package):
            raise Failure(f"process {pid} is not among the application's processes {process_ids(guest, package)}")
        state["pid"] = pid
        return f"process {pid} in untrusted_app_25, no layer mapped, property empty"

    def switch(mode: str) -> str:
        result = guest.run(f"{COMMAND} set {package} {mode}")
        if result.status != 0 or result.out != f"{package}\t{mode}\n":
            raise Failure(f"set {package} {mode}: status {result.status}, out {result.out!r}, err {result.err.strip()!r}")
        remaining = process_ids(guest, package)
        if remaining:
            raise Failure(f"the application still has processes {remaining} after set {mode}")
        return f"set {mode}: the application has no process"

    def on_process() -> str:
        value = property_value(guest, name)
        choice_seconds(value, name)
        result = probe_result(guest, "running")
        pid = evaluate_process(result, value, mapped=True)
        if pid == state.get("pid"):
            raise Failure(f"the process after set on is the earlier process {pid}")
        state["pid"] = pid
        state["result"] = result
        return f"new process {pid}: layer mapped, read {value}, set refused"

    def read_baseline() -> str:
        state["earlier"] = set(denials(guest))
        return f"{len(state['earlier'])} earlier denial(s) of {HOOK_DOMAIN}* in the log"

    def off_again() -> str:
        pid = evaluate_process(probe_result(guest, "running"), "", mapped=False)
        return f"new process {pid}: no layer, property empty"

    def stale() -> str:
        result = guest.run(f"{COMMAND} set {package} on")
        if result.status != 0:
            raise Failure(f"set {package} on: status {result.status}, err {result.err.strip()!r}")
        value = property_value(guest, name)
        install(guest, apks["running"], package)
        permission_review.complete_review(guest, package, Failure)
        if property_value(guest, name) != value:
            raise Failure(f"{name} did not survive the reinstall: the stale case is not exercised")
        listing_of(guest)
        pid = evaluate_process(probe_result(guest, "running"), "", mapped=False)
        return f"after the reinstall and a listing, process {pid} has no layer and the property is empty"

    try:
        if not report.run("hook: the running probe is installed", installed):
            return
        report.run("hook: off, the old application's process has no layer", off_process)
        report.run("hook: set on ends the application's process", lambda: switch("on"))
        read = report.run("hook: the log is read before the probe's set", read_baseline)
        report.run("hook: on, the new process maps the layer and reads the property", on_process)
        if read:
            report.run("hook: the denied set leaves an avc line",
                       lambda: denial_note(guest, state["earlier"], state.get("result"), name))
        else:
            report.add("hook: the denied set leaves an avc line", "FAIL",
                       "not established: the log could not be read before the probe's set, so an earlier denial "
                       "cannot be told from a new one")
        report.run("hook: set off ends the process", lambda: switch("off"))
        report.run("hook: off again, the next process has no layer", off_again)
        report.run("hook: a stale choice is gone before the next process", stale)
    finally:
        guest.adb_run("uninstall", package, timeout=60)


def check_real(guest, apk: pathlib.Path, package: str, label: str) -> str:
    """An application whose label is a resource of its own package is listed under that label."""
    install(guest, apk, package)
    try:
        row = next((row for row in listing_of(guest) if row.package == package), None)
        if row is None:
            raise Failure(f"{package} is not listed: it is not eligible, or the command does not see it")
        if row.label != label:
            raise Failure(f"{package} is listed as {row.label!r}, not {label!r}")
        return f"{package} is listed as {row.label!r}"
    finally:
        guest.adb_run("uninstall", package, timeout=60)


def open_guest(options: argparse.Namespace, adb: str, private: pathlib.Path):
    """The guest the steps drive, and what stops it. The steps read a command's status and its two streams apart, which
    only a Serial gives, so the guest this runner launches is driven through one too (the launcher's own Guest merges the
    streams and has no run): both ways of getting a guest give the steps the same thing."""
    if options.serial:
        return Serial(adb, options.serial), lambda: None
    settings = argparse.Namespace(
        release=options.release, emulator=options.emulator, instance=None, adb_port=options.adb_port,
        cpus=options.cpus, memory=options.memory, boot_timeout=options.boot_timeout, no_argument=True,
        extra_arguments=[])
    launched = orient.start_guest(settings, adb, private / "guest", early=None, holder={}, provision=True)
    return Serial(adb, launched.serial), lambda: orient.stop_guest(adb, launched)


def run(options: argparse.Namespace, adb: str, private: pathlib.Path, report: Report) -> None:
    private.mkdir(parents=True, exist_ok=True)
    built = [*probe.VARIANTS, *probe.SUCCESSORS, *(probe.RUNNING if options.hook else ())]
    # "old" and its update "raised" are signed with one key, so that the second installs over the first in place.
    keystore = private / "old-family.p12"
    apks = {variant: probe.build_apk(private / f"{variant}.apk", variant, keystore=keystore) for variant in built}
    guest, stop = open_guest(options, adb, private)
    try:
        report.run("the command is in the image", lambda: command_present(guest))
        installed_at = guest_time(guest)
        for variant in probe.VARIANTS:
            report.run(f"{variant}: install", lambda variant=variant: (
                install(guest, apks[variant], probe.package_of(variant)) or f"{probe.package_of(variant)} installed"))
        report.run("list names the eligible application and no other", lambda: check_listing(guest, installed_at))
        report.run("set refuses what is not eligible; usage", lambda: check_refusals(guest))
        state: dict[str, str] = {}

        def on() -> str:
            note, state["value"] = switch_on(guest)
            return note

        try:
            if report.run("set on", on):
                report.run("the lock is a private shell_data_file of the shell user", lambda: check_lock_file(guest))
                report.run("an application reads it and cannot set it", lambda: application_view(guest, state["value"]))
        finally:
            report.run("set off", lambda: switch_off(guest))
        report.run("a choice does not outlive its installation", lambda: check_lifecycle(guest, apks))
        if options.hook:
            check_hook(guest, apks, report)
        if options.real_apk:
            report.run("a resource label is listed", lambda: check_real(
                guest, options.real_apk, options.real_package, options.real_label))
    finally:
        for variant in probe.VARIANTS:
            guest.adb_run("uninstall", probe.package_of(variant), timeout=60)
        stop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release built with the command")
    parser.add_argument("--serial", help="a running guest (host:port); no guest is launched")
    parser.add_argument("--hook", action="store_true",
                        help="the image also carries the framework hook and the compatibility library: add the "
                        "steps that start an old application's process and look for the layer in it")
    parser.add_argument("--real-apk", type=pathlib.Path,
                        help="an old application whose label is a resource, to check the label path")
    parser.add_argument("--real-package", help="its package name (with --real-apk)")
    parser.add_argument("--real-label", help="the label the listing must show for it (with --real-apk)")
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
    parser = build_parser()
    options = parser.parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    if bool(options.real_apk) != bool(options.real_package) or bool(options.real_apk) != bool(options.real_label):
        parser.error("--real-apk, --real-package and --real-label go together")
    adb = orient.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-oldswitch-"))
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
    sys.exit(main())
