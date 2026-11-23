#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the launcher-application command of Add to Home Screen on an image.

android/lineage-24/overlay/app_shortcuts is `kernarium_apps`, a command that Andarium runs as the adb shell user:
``digest`` prints a header block whose digest changes with the set of launcher applications, their versions and
Android's language, ``list`` adds one row per launcher activity (package, activity, label, built in, icon key),
``icons`` answers one PNG record per component and ``launch`` starts exactly one launcher activity and refuses
anything else. This runner installs the probe applications of appsprobe_apk.py and, as the adb shell user, checks
that:

- the command and its jar are in the image and `kernarium_apps` resolves on the shell's PATH;
- ``digest`` and ``list`` carry the same header block (version line, digest, count, skipped, locales, user), the
  user is the foreground user, the first locale is the system's, the rows are in order with no component twice, every
  name matches the grammar and every label is safe (no control, separator or bidirectional character, at most 80
  code points and 320 bytes, ends trimmed and single-spaced), the answer ends with the count, and every bound holds;
- a built-in application (Settings) is listed as built in, the probe's launcher activities are listed as installed
  with the labels the escaping and the cut give, and its not exported launcher activity and its activity with no
  launcher filter are not listed;
- ``icons`` gives a PNG of the declared size whose length and SHA-256 are the declared ones, with the signature and
  a header that agrees, that decodes (8-bit RGB or RGBA) to a picture that is not blank and whose ALPHA field says
  exactly whether any pixel is not opaque (an adaptive icon has opaque corners: it is composed with no mask), for the
  built-in applications; for the second probe, whose label is a string resource and whose icon is an adaptive icon of
  a colour and a bitmap layer, an adaptive 192 x 192 picture with the colour in its corners and the bitmap's disc in
  its middle; ``noicon`` for the first probe (it declares no icon); ``missing`` for a component that is not a launcher
  activity; records in the order asked; nine components are a usage error;
- the drawing is not done in the shell: while ``icons`` runs over eight components (the second probe's among them) the
  tool's own process, sampled through /proc, maps and holds open no application, overlay or resource-cache file (nor
  anything in the directory of a target application), and the platform runs the renderer in two processes of
  its own, an ordinary application's (untrusted_app, u0_a) and an isolated one's (isolated_app, u0_i), neither the
  shell's; the renderer refuses an ordinary application's call; and an icon that cannot be drawn (the ``bad`` probe,
  a foreground that is not a picture) is ``error`` for it alone among seven good ones;
- ``launch`` starts the probe's launcher activity (it becomes the resumed activity, and the answer is exactly its
  ``launched`` line with nothing on standard error) and starts it again when it is already open (the same), and
  refuses with status 2 and nothing on standard output: an activity that is not exported, one with no
  launcher filter, a built-in application's activity that is not a launcher one, a package that is not installed, a
  disabled application, and every hostile argument (shell metacharacters, options, a second component, the
  shorthand), none of which runs anything (a marker file that the hostile arguments would create is never made);
- a launcher alias that the manifest disables is not listed, has no icon and is refused; once ``pm enable`` has
  enabled it for the foreground user it is listed (with its label), answered ``noicon`` and started, and disabled
  again it is gone, with the earlier digest back;
- the digest is the one the contract defines: it is recomputed here from the rows, the locale line and the user;
- the digest follows the set: the same twice, different once another application is installed, the same again once
  it is removed, different after an update in place (and the icon keys of the updated application change, those of
  others do not) and different with an application disabled;
- usage errors are status 64 with nothing on standard output, the command writes no file in the shell user's data
  directory and no new denial of the shell domain reaches the log (the log is read before the first call of the
  command, so a denial that only the first call meets counts). The isolated renderer's own audit lines in that
  window are reported with the result, not judged: AOSP's one known record, a getattr on the application's data
  directory with its bug note, is one of them.

Not automated: Android's language changing the labels (the listing reads the system's configuration, which a user
build's shell cannot change; the first locale of the header is checked against the property), and the user switching.

Every wait has a deadline. The guest's launcher is stopped through its own process on any exit, and the guest is
reached through the shared adb server, always named with `-s`.
usage: run_android_app_shortcuts.py --emulator uml-darwin-android17 [--release DIR] [--serial HOST:PORT]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import pathlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unicodedata
import zlib
from typing import NamedTuple

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import appsprobe_apk as probe  # noqa: E402

# The orientation runner owns the guest: its launcher and its adb.
SPEC = importlib.util.spec_from_file_location("run_android_orientation", HERE / "run_android_orientation.py")
assert SPEC is not None and SPEC.loader is not None
orient = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = orient  # dataclasses resolve their annotations through sys.modules
SPEC.loader.exec_module(orient)
Failure = orient.Failure
Report = orient.Report

COMMAND = "kernarium_apps"
WRAPPER_PATH = "/system_ext/bin/kernarium_apps"
JAR_PATH = "/system_ext/framework/kernarium_apps.jar"
VERSION_LINE = b"kernarium_apps 1"
SHELL_CONTEXT = "u:r:shell:s0"
EXIT_REFUSED = 2
EXIT_USAGE = 64
INSTALL_SECONDS = 180
COMMAND_SECONDS = 90
LAUNCH_SECONDS = 30
#: A built-in application that has a launcher activity, and one of its activities that is not a launcher one.
SETTINGS_PACKAGE = "com.android.settings"
SETTINGS_NOT_LAUNCHER = "com.android.settings/com.android.settings.SubSettings"
#: The file the hostile launch arguments would create if any of them ran as a command.
MARKER = "/data/local/tmp/kernarium_apps_marker"
SHELL_DATA = "/data/local/tmp"
HEADER_KEYS = ("digest", "count", "skipped", "locales", "user")
#: The bounds of the contract.
MAX_ROWS = 1024
MAX_PER_PACKAGE = 8
MAX_LABEL_POINTS = 80
MAX_LABEL_BYTES = 320
MAX_PNG_BYTES = 98304
MAX_ICONS = 8
ICON_SIZES = (192, 128, 96)
MAX_LIST_BYTES = 900 * 1024
MAX_ICONS_BYTES = 800 * 1024
MAX_DIGEST_BYTES = 1024
DIGEST_SECONDS = 30
PACKAGE_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+")
ACTIVITY_NAME = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*(\.[A-Za-z_$][A-Za-z0-9_$]*)*")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
#: The renderer: its package, the two processes it runs in, the domains and users the platform gives them.
RENDERER_PACKAGE = "com.kernarium.icons"
RENDERER_ISOLATED_PROCESS = "com.kernarium.icons:com.kernarium.icons.IconRenderService"
ISOLATED_CONTEXT = "u:r:isolated_app:s0"
PROVIDER_DOMAIN = re.compile(r"u:r:untrusted_app(_\d+)?:s0(:\S+)?")
PROVIDER_USER = re.compile(r"u0_a\d+")
ISOLATED_USER = re.compile(r"u0_i\d+")
#: What the sampler's script prints (one `M` line for each path the tool process maps or holds, one `P` line for each
#: process of the renderer's package, the number of rounds and the tool's status), and where it starts.
SAMPLER_MARK = "# kernarium_apps process sampler"
SAMPLER_ROUNDS = 600
SAMPLER_SECONDS = 120
CALLER_SECONDS = 40


class Run(NamedTuple):
    status: int
    out: bytes
    err: str

    @property
    def text(self) -> str:
        return self.out.decode("utf-8", errors="replace")


class Row(NamedTuple):
    package: str
    activity: str
    label: str
    builtin: bool
    key: str

    @property
    def token(self) -> str:
        return f"{self.package}/{self.activity}"


class Header(NamedTuple):
    digest: str
    count: int
    skipped: int
    locales: str
    user: int


class Listed(NamedTuple):
    header: Header
    rows: list[Row]


class IconRecord(NamedTuple):
    token: str
    status: str
    width: int
    height: int
    kind: str
    alpha: str
    length: int
    sha: str
    data: bytes


class Serial:
    """A running guest named by its adb serial."""

    def __init__(self, adb: str, serial: str) -> None:
        self.adb, self.serial = adb, serial

    def adb_run(self, *arguments: str, timeout: float = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=False, timeout=timeout, check=False)

    def run(self, command: str, timeout: float = COMMAND_SECONDS) -> Run:
        """A command in the adb shell: its exit status (adb's shell protocol carries it), its standard output as bytes
        (an icon is binary) and its standard error."""
        result = self.adb_run("shell", command, timeout=timeout)
        return Run(result.returncode, result.stdout, result.stderr.decode("utf-8", errors="replace"))

    def shell(self, command: str, timeout: float = 30) -> str:
        result = self.run(command, timeout)
        if result.status != 0:
            raise Failure(f"'{command}' exited {result.status}: {(result.text + result.err).strip()[-500:]}")
        return result.text


# ---------------------------------------------------------------------------------------------------------------
# The parsers: strict, as the host's reader must be. Each raises Failure with what is wrong.

def fail(message: str) -> Failure:
    return Failure(message)


def lines_of(out: bytes, what: str) -> list[bytes]:
    if not out.endswith(b"\n"):
        raise fail(f"{what}: the answer does not end with a line break")
    if b"\r" in out:
        raise fail(f"{what}: the answer holds a carriage return")
    return out[:-1].split(b"\n")


def decode_line(line: bytes, what: str) -> str:
    try:
        return line.decode("utf-8")
    except UnicodeDecodeError as problem:
        raise fail(f"{what}: a line is not UTF-8: {problem}") from problem


def parse_header(lines: list[bytes], what: str) -> tuple[Header, int]:
    """The version line and the header block; the index of the first line after them."""
    if not lines or lines[0] != VERSION_LINE:
        raise fail(f"{what}: the first line is {lines[:1]!r}, not {VERSION_LINE!r}")
    found: dict[str, str] = {}
    index = 1
    while index < len(lines) and decode_line(lines[index], what).split("\t")[0] in HEADER_KEYS:
        parts = decode_line(lines[index], what).split("\t")
        if len(parts) != 2:
            raise fail(f"{what}: the header line {parts[0]} has {len(parts)} fields")
        found[parts[0]] = parts[1]
        index += 1
    missing = [key for key in HEADER_KEYS if key not in found]
    if missing:
        raise fail(f"{what}: the header lacks {missing}")
    if not re.fullmatch(r"[0-9a-f]{64}", found["digest"]):
        raise fail(f"{what}: the digest {found['digest']!r} is not 64 lowercase hex characters")
    for key in ("count", "skipped", "user"):
        if not re.fullmatch(r"[0-9]+", found[key]):
            raise fail(f"{what}: {key} is {found[key]!r}")
    if len(found["locales"].encode()) > 128 or len(found["locales"].split(",")) > 5 or not re.fullmatch(
            r"([A-Za-z0-9-]+(,[A-Za-z0-9-]+)*)?", found["locales"]):
        raise fail(f"{what}: locales is {found['locales']!r}")
    return Header(found["digest"], int(found["count"]), int(found["skipped"]), found["locales"],
                  int(found["user"])), index


def parse_digest(out: bytes) -> Header:
    if len(out) > MAX_DIGEST_BYTES:
        raise fail(f"digest: the answer is {len(out)} bytes, over {MAX_DIGEST_BYTES}")
    lines = lines_of(out, "digest")
    header, index = parse_header(lines, "digest")
    if index != len(lines):
        raise fail(f"digest: {len(lines) - index} lines follow the header block")
    return header


DIGEST_SALT = "kernarium_apps-digest-1\n"


def expected_digest(listed: "Listed", all_locales: list[str]) -> str:
    """The digest the contract defines (section 4): SHA-256 over the salt line, one line for each row without its label,
    the locale line (and, only when it is not the whole locale list, a line with the hash of the whole list), and the
    user."""
    text = DIGEST_SALT + "".join(f"{row.package}\t{row.activity}\t{1 if row.builtin else 0}\t{row.key}\n"
                                for row in listed.rows)
    text += f"locales\t{listed.header.locales}\n"
    joined = ",".join(all_locales)
    if joined != listed.header.locales:
        text += f"localesall\t{hashlib.sha256(joined.encode()).hexdigest()}\n"
    text += f"user\t{listed.header.user}\n"
    return hashlib.sha256(text.encode()).hexdigest()


def label_problem(label: str) -> str | None:
    """Why a label is not safe, or None."""
    if not label:
        return "is empty"
    points = len(label)
    if points > MAX_LABEL_POINTS or len(label.encode("utf-8")) > MAX_LABEL_BYTES:
        return f"is longer than {MAX_LABEL_POINTS} code points or {MAX_LABEL_BYTES} bytes"
    if label != label.strip(" ") or "  " in label:
        return "has a space at an end or a run of spaces"
    for character in label:
        category = unicodedata.category(character)
        if category in ("Cc", "Zl", "Zp", "Cs") or (category == "Zs" and character != " "):
            return f"holds U+{ord(character):04X} ({category})"
        if category == "Cf" and character not in "‌‍":
            return f"holds U+{ord(character):04X} ({category})"
        if character in "؜‎‏‪‫‬‭‮⁦⁧⁨⁩﻿":
            return f"holds the bidirectional or byte order character U+{ord(character):04X}"
    return None


def parse_list(out: bytes) -> Listed:
    if len(out) > MAX_LIST_BYTES:
        raise fail(f"list: the answer is {len(out)} bytes, over {MAX_LIST_BYTES}")
    lines = lines_of(out, "list")
    header, index = parse_header(lines, "list")
    if index >= len(lines) or lines[index] != b"rows":
        raise fail("list: the header block is not followed by the rows line")
    index += 1
    rows: list[Row] = []
    for _ in range(header.count):
        if index >= len(lines):
            raise fail(f"list: the count says {header.count} rows and the answer has {len(rows)}")
        parts = decode_line(lines[index], "list").split("\t")
        index += 1
        if len(parts) < 5:
            raise fail(f"list: a row has {len(parts)} fields: {parts!r}")
        package, activity, label, builtin, key = parts[:5]
        if len(package) > 255 or not PACKAGE_NAME.fullmatch(package):
            raise fail(f"list: {package!r} is not a package name of the grammar")
        if len(activity) > 255 or not ACTIVITY_NAME.fullmatch(activity):
            raise fail(f"list: {activity!r} is not an activity name of the grammar")
        problem = label_problem(label)
        if problem:
            raise fail(f"list: the label of {package}/{activity} {problem}: {label!r}")
        if builtin not in ("0", "1"):
            raise fail(f"list: the built-in flag of {package}/{activity} is {builtin!r}")
        if not re.fullmatch(r"[0-9a-f]{16}", key):
            raise fail(f"list: the icon key of {package}/{activity} is {key!r}")
        rows.append(Row(package, activity, label, builtin == "1", key))
    if index >= len(lines) or lines[index] != f"end\t{header.count}".encode():
        raise fail(f"list: the rows are not followed by 'end\\t{header.count}': {lines[index:index + 1]!r}")
    if index + 1 != len(lines):
        raise fail("list: lines follow the end line")
    if len(rows) > MAX_ROWS:
        raise fail(f"list: {len(rows)} rows, over {MAX_ROWS}")
    tokens = [(row.package, row.activity) for row in rows]
    if tokens != sorted(tokens):
        raise fail("list: the rows are not in order by package and activity")
    if len(set(tokens)) != len(tokens):
        raise fail("list: a component is listed twice")
    for package in {row.package for row in rows}:
        if sum(1 for row in rows if row.package == package) > MAX_PER_PACKAGE:
            raise fail(f"list: {package} has more than {MAX_PER_PACKAGE} rows")
    return Listed(header, rows)


def png_header(data: bytes) -> tuple[int, int, int, int]:
    """Width, height, bit depth and colour type of a PNG's first chunk."""
    if data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR":
        raise fail("not a PNG (signature or header chunk)")
    width, height, depth, colour = struct.unpack(">IIBB", data[16:26])
    return width, height, depth, colour


def decode_png(data: bytes) -> tuple[int, int, int, list[bytes]]:
    """Width, height, channels and unfiltered rows of an 8-bit, non-interlaced RGB or RGBA PNG; raises Failure for any
    other kind (the command's encoder writes these two)."""
    width, height, depth, colour = png_header(data)
    if depth != 8 or colour not in (2, 6) or data[28] != 0:
        raise fail(f"PNG of depth {depth}, colour type {colour}, interlace {data[28]}: not 8-bit RGB or RGBA")
    channels = 3 if colour == 2 else 4
    compressed = b""
    offset = 8
    ended = False
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        if offset + 12 + length > len(data):
            raise fail(f"the PNG's {kind!r} chunk runs past the end of the data")
        stored = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])[0]
        if zlib.crc32(kind + data[offset + 8:offset + 8 + length]) & 0xFFFFFFFF != stored:
            raise fail(f"the PNG's {kind!r} chunk has a wrong checksum")
        if kind == b"IDAT":
            compressed += data[offset + 8:offset + 8 + length]
        if kind == b"IEND":
            ended = True
            break
        offset += 12 + length
    if not ended:
        raise fail("the PNG has no IEND chunk")
    try:
        raw = zlib.decompress(compressed)
    except zlib.error as problem:
        raise fail(f"the PNG's pixel data does not decompress: {problem}") from problem
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise fail("the PNG's pixel data is not the size its header says")
    rows: list[bytes] = []
    previous = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 255
            elif kind == 4:
                estimate = left + up - upper_left
                near_left, near_up, near_corner = abs(estimate - left), abs(estimate - up), abs(estimate - upper_left)
                chosen = left if near_left <= near_up and near_left <= near_corner else (
                    up if near_up <= near_corner else upper_left)
                line[x] = (line[x] + chosen) & 255
            elif kind != 0:
                raise fail(f"the PNG has a row of filter {kind}")
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def pixel(rows: list[bytes], channels: int, x: int, y: int) -> tuple[int, ...]:
    start = x * channels
    return tuple(rows[y][start:start + channels])


def parse_icons(out: bytes, asked: list[str]) -> list[IconRecord]:
    if len(out) > MAX_ICONS_BYTES:
        raise fail(f"icons: the answer is {len(out)} bytes, over {MAX_ICONS_BYTES}")
    position = 0

    def line() -> bytes:
        nonlocal position
        end = out.find(b"\n", position)
        if end < 0:
            raise fail("icons: the answer ends inside a line")
        taken = out[position:end]
        position = end + 1
        return taken

    if line() != VERSION_LINE:
        raise fail(f"icons: the first line is not {VERSION_LINE!r}")
    head = decode_line(line(), "icons").split("\t")
    if head[0] != "icons" or len(head) != 2 or head[1] != str(len(asked)):
        raise fail(f"icons: the second line is {head!r}, not icons and {len(asked)}")
    records: list[IconRecord] = []
    for token in asked:
        fields = decode_line(line(), "icons").split("\t")
        if len(fields) < 9 or fields[0] != "icon" or fields[1] != token:
            raise fail(f"icons: the record for {token} begins {fields[:2]!r}")
        _, _, status, width, height, kind, alpha, length, sha = fields[:9]
        if status not in ("ok", "missing", "noicon", "error", "deferred"):
            raise fail(f"icons: the status of {token} is {status!r}")
        data = b""
        if status == "ok":
            size = int(length)
            data = out[position:position + size]
            if len(data) != size or out[position + size:position + size + 1] != b"\n":
                raise fail(f"icons: the data of {token} is not {size} bytes followed by a line break")
            position += size + 1
            if hashlib.sha256(data).hexdigest() != sha:
                raise fail(f"icons: the digest of {token} is not the SHA-256 of its data")
            if size > MAX_PNG_BYTES:
                raise fail(f"icons: the PNG of {token} is {size} bytes, over {MAX_PNG_BYTES}")
            if int(width) != int(height) or int(width) not in ICON_SIZES:
                raise fail(f"icons: the size of {token} is {width} by {height}")
            if png_header(data)[:2] != (int(width), int(height)):
                raise fail(f"icons: the PNG of {token} is not the size its record says")
            if kind not in ("adaptive", "legacy") or alpha not in ("0", "1"):
                raise fail(f"icons: the kind or alpha of {token} is {kind!r} {alpha!r}")
        elif (width, height, kind, alpha, length, sha) != ("0", "0", "-", "0", "0", "-"):
            raise fail(f"icons: the record of {token} ({status}) carries values: {fields[3:9]!r}")
        records.append(IconRecord(token, status, int(width), int(height), kind, alpha, int(length), sha, data))
    if decode_line(line(), "icons") != f"end\t{len(asked)}":
        raise fail("icons: the records are not followed by the end line")
    if position != len(out):
        raise fail("icons: bytes follow the end line")
    return records


# ---------------------------------------------------------------------------------------------------------------
# Running the command.

def quoted(*words: str) -> str:
    return " ".join(shlex.quote(word) for word in words)


def command_run(guest, *words: str, timeout: float = COMMAND_SECONDS) -> Run:
    return guest.run(f"{COMMAND} {quoted(*words)}", timeout=timeout)


def good(guest, *words: str) -> Run:
    result = command_run(guest, *words)
    if result.status != 0 or result.err.strip():
        raise fail(f"'{COMMAND} {' '.join(words)}' exited {result.status}: "
                   f"{(result.text + result.err).strip()[-500:]}")
    return result


def digest_of(guest) -> Header:
    return parse_digest(good(guest, "digest").out)


def list_of(guest) -> Listed:
    return parse_list(good(guest, "list").out)


def icons_of(guest, tokens: list[str]) -> list[IconRecord]:
    return parse_icons(good(guest, "icons", *tokens).out, tokens)


def install(guest, apk: pathlib.Path, package: str) -> None:
    result = guest.adb_run("install", "-r", "-t", str(apk), timeout=INSTALL_SECONDS)   # test-only: the shell may enable its alias
    text = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    if result.returncode != 0 or "Success" not in text:
        raise fail(f"installing {package}: {text.strip()[-400:]}")


def fresh_install(guest, apk: pathlib.Path, package: str) -> None:
    """Each run signs with a key of its own, so an earlier copy goes first."""
    guest.adb_run("uninstall", package, timeout=60)
    install(guest, apk, package)


def rows_of(listed: Listed, package: str) -> list[Row]:
    return [row for row in listed.rows if row.package == package]


# ---------------------------------------------------------------------------------------------------------------
# The steps.

def command_present(guest) -> str:
    listing = guest.shell(f"ls -lZ {WRAPPER_PATH} {JAR_PATH}")
    if WRAPPER_PATH not in listing or JAR_PATH not in listing:
        raise fail(f"the image lacks the command: {listing.strip()[-300:]}")
    resolved = guest.shell(f"command -v {COMMAND}").strip()
    if resolved != WRAPPER_PATH:
        raise fail(f"{COMMAND} resolves to {resolved!r}, not {WRAPPER_PATH}")
    context = guest.shell("id -Z").strip()
    if not context.startswith(SHELL_CONTEXT):
        raise fail(f"adb shell runs in {context!r}, not {SHELL_CONTEXT}")
    return f"{resolved}; adb shell is {context}"


def system_locales(guest) -> list[str]:
    """The system's whole locale list, as BCP-47 tags, when the image says it (else the empty list)."""
    value = guest.shell("settings get system system_locales").strip()
    if not value or value == "null":
        value = ""
    return [tag.replace("_", "-") for tag in value.split(",") if tag]


def check_header(guest) -> str:
    started = time.monotonic()
    first = parse_digest(good(guest, "digest").out)
    seconds = time.monotonic() - started
    if seconds > DIGEST_SECONDS:
        raise fail(f"digest took {seconds:.1f} s, over {DIGEST_SECONDS} s")
    second = digest_of(guest)
    if first != second:
        raise fail(f"digest twice gives {first} and {second}")
    listed = list_of(guest)
    if listed.header != first:
        raise fail(f"list's header block {listed.header} is not digest's {first}")
    user = int(guest.shell("am get-current-user").strip())
    if listed.header.user != user:
        raise fail(f"the header names user {listed.header.user}, the foreground user is {user}")
    system = guest.shell("getprop persist.sys.locale").strip() or guest.shell("getprop ro.product.locale").strip()
    wanted = re.split(r"[-_]", system)[0].lower() if system else ""
    tags = listed.header.locales.split(",") if listed.header.locales else []
    if wanted and (not tags or re.split(r"[-_]", tags[0])[0].lower() != wanted):
        raise fail(f"the first locale is {tags[:1]}, the system's is {system!r}")
    if listed.header.count != len(listed.rows) or listed.header.count == 0:
        raise fail(f"count is {listed.header.count} for {len(listed.rows)} rows")
    all_locales = system_locales(guest) or tags
    computed = expected_digest(listed, all_locales)
    if listed.header.digest != computed:
        raise fail(f"the digest {listed.header.digest} is not the one the contract defines for these rows, locales "
                   f"{listed.header.locales!r} (all {all_locales}) and user {listed.header.user}: {computed}")
    return (f"digest {first.digest[:12]}, {first.count} rows, skipped {first.skipped}, locales "
            f"{first.locales or '-'}, user {first.user}; digest in {seconds:.1f} s")


def check_rows(guest) -> str:
    listed = list_of(guest)
    built_in = [row for row in listed.rows if row.builtin]
    if not any(row.package == SETTINGS_PACKAGE for row in built_in):
        raise fail(f"{SETTINGS_PACKAGE} is not listed as a built-in application: "
                   f"{[row.package for row in built_in][:12]}")
    if any(row.package == SETTINGS_PACKAGE and row.label == SETTINGS_PACKAGE for row in built_in):
        raise fail(f"{SETTINGS_PACKAGE} is listed under its package name: the labels in the applications' "
                   "resources are not read")
    mine = rows_of(listed, probe.package_of("first"))
    expected = {
        probe.component("first", "listed"): probe.LISTED_HOSTILE_LABEL,
        probe.component("first", "long"): probe.LISTED_LONG_LABEL,
    }
    if {row.token: row.label for row in mine} != expected:
        raise fail(f"the probe's rows are {[(row.token, row.label) for row in mine]}, "
                   f"not {sorted(expected.items())}: the escaping, the cut, or the not exported / no-filter activities")
    if any(row.builtin for row in mine):
        raise fail("an installed application is listed as built in")
    other = rows_of(listed, probe.package_of("second"))
    if [(row.token, row.label, row.builtin) for row in other] != [
            (probe.component("second", "only"), probe.LISTED_SECOND_LABEL, False)]:
        raise fail(f"the second probe's rows are {other}")
    return (f"{len(built_in)} built in, Settings among them under its own label; the probe's two launcher "
            f"activities are listed with {probe.LISTED_HOSTILE_LABEL!r} and a {len(probe.LISTED_LONG_LABEL)}-character "
            "cut label; its hidden and no-filter activities are not")


def check_picture(record: IconRecord, corners: tuple[int, int, int] | None = None,
                  centre: tuple[int, int, int] | None = None) -> str:
    """The picture of an ok record decodes (8-bit RGB or RGBA: anything else is a failure), is the size the record says,
    is not blank, and has the alpha the record says: ALPHA is 1 exactly when some pixel is not opaque. `corners` and
    `centre` name the colour that the four corners and the middle must have."""
    width, height, channels, rows = decode_png(record.data)
    if (width, height) != (record.width, record.height):
        raise fail(f"{record.token}: the PNG is {width} by {height}, the record says {record.width} by {record.height}")
    colours = {pixel(rows, channels, x, y)
               for y in range(0, height, max(1, height // 24)) for x in range(0, width, max(1, width // 24))}
    if len(colours) < 2:
        raise fail(f"the picture of {record.token} is one colour: {colours}")
    translucent = channels == 4 and any(min(row[3::4]) != 255 for row in rows)
    if record.alpha == "1" and not translucent:
        raise fail(f"{record.token} says it has alpha and every pixel is opaque")
    if record.alpha == "0" and translucent:
        raise fail(f"{record.token} says it is opaque and a pixel is not")
    for x, y in ((0, 0), (width - 1, 0), (0, height - 1), (width - 1, height - 1)):
        value = pixel(rows, channels, x, y)
        if record.kind == "adaptive" and record.alpha == "0" and channels == 4 and value[3] != 255:
            raise fail(f"{record.token}: an adaptive icon's corner {value} is not opaque")
        if corners is not None and any(abs(value[i] - corners[i]) > 2 for i in range(3)):
            raise fail(f"{record.token}: the corner ({x}, {y}) is {value}, not {corners}")
    middle = pixel(rows, channels, width // 2, height // 2)
    if centre is not None and any(abs(middle[i] - centre[i]) > 2 for i in range(3)):
        raise fail(f"{record.token}: the middle is {middle}, not {centre}")
    return "decoded"


def check_icons(guest) -> str:
    listed = list_of(guest)
    built_in = [row.token for row in listed.rows if row.builtin]
    totals: dict[str, int] = {}
    kinds = {"adaptive": 0, "legacy": 0}
    failed: list[str] = []
    for start in range(0, len(built_in), MAX_ICONS):
        for record in icons_of(guest, built_in[start:start + MAX_ICONS]):
            totals[record.status] = totals.get(record.status, 0) + 1
            if record.status == "ok":
                kinds[record.kind] += 1
                check_picture(record)
            elif record.status == "error":
                failed.append(record.token)
    if failed:
        raise fail(f"the icons of {len(failed)} built-in applications came back error: {failed[:8]}")
    if totals.get("ok", 0) == 0:
        raise fail(f"no built-in application's icon was rendered: {totals}")
    if kinds["adaptive"] == 0:
        raise fail(f"no built-in icon came out adaptive ({kinds}): the composition of the layers is unproved")
    settings = [token for token in built_in if token.startswith(SETTINGS_PACKAGE + "/")]
    record = icons_of(guest, settings[:1])[0]
    if record.status != "ok" or record.width != 192:
        raise fail(f"the icon of {record.token} is {record.status} at {record.width}, not ok at 192")
    resource_token = probe.component("second", "only")
    resource = icons_of(guest, [resource_token])[0]
    if resource.status != "ok" or resource.kind != "adaptive" or resource.width != 192 or resource.alpha != "0":
        raise fail(f"the resource-backed icon of {resource_token} is {resource.status} {resource.kind} "
                   f"{resource.width} alpha {resource.alpha}, not an opaque adaptive ok at 192")
    check_picture(resource, corners=probe.BACKGROUND_RGB, centre=probe.FOREGROUND_RGB)
    probe_listed = probe.component("first", "listed")
    asked = [probe_listed, SETTINGS_NOT_LAUNCHER, probe.component("first", "hidden"), settings[0],
             probe.component("first", "plain"), probe.component("first", "guarded"),
             "org.example.andarium.appsprobe.absent/android.app.Activity"]
    expected = ["noicon", "missing", "missing", "ok", "missing", "missing", "missing"]
    statuses = [record.status for record in icons_of(guest, asked)]
    if statuses != expected:
        raise fail(f"statuses for {asked} are {statuses}, not {expected}")
    too_many = command_run(guest, "icons", *[f"org.example.a{index}/android.app.Activity" for index in range(9)])
    if too_many.status != EXIT_USAGE or too_many.out:
        raise fail(f"nine components exit {too_many.status} with {len(too_many.out)} bytes on stdout, not 64 and none")
    return (f"{totals} over {len(built_in)} built-in activities, {kinds['adaptive']} adaptive and "
            f"{kinds['legacy']} legacy; Settings at 192 x 192; the string- and bitmap-backed adaptive icon of the second "
            "probe has the colour in its corners and the disc in its middle; noicon, missing and the order asked")


class Sampled(NamedTuple):
    paths: set[str]
    processes: list[tuple[str, str, str]]   # (label, user, name) of each process of the renderer's package
    rounds: int
    status: int


def sampler_script(tokens: list[str]) -> str:
    """The shell script that runs `icons` over the tokens in the background and, until it ends (or SAMPLER_ROUNDS rounds
    have passed), lists every path the tool's process maps or holds open and every process of the renderer's package
    with its context and user, each once. The shell may read /proc/<pid>/maps and /proc/<pid>/fd of its own tool."""
    return "\n".join([
        SAMPLER_MARK,
        f"{COMMAND} icons {quoted(*tokens)} >/dev/null 2>&1 &",
        "p=$!; n=0",
        f"{{ while [ $n -lt {SAMPLER_ROUNDS} ] && kill -0 $p 2>/dev/null; do",
        "    n=$((n+1))",
        "    awk '{print \"M \" $6}' /proc/$p/maps 2>/dev/null",
        "    ls -l /proc/$p/fd 2>/dev/null | awk '{print \"M \" $NF}'",
        f"    ps -AZ 2>/dev/null | grep -F ' {RENDERER_PACKAGE}' | awk '{{print \"P \" $0}}'",
        "  done; echo \"N $n\"; } | sort -u",
        "wait $p; echo \"status=$?\"",
    ])


def parse_sampler(out: str) -> Sampled:
    """The sampler's answer, strictly: M, P and N lines and the status line last."""
    paths: set[str] = set()
    processes: list[tuple[str, str, str]] = []
    rounds: int | None = None
    status: int | None = None
    for line in out.splitlines():
        if status is not None:
            raise fail(f"the sampler: a line follows the status: {line!r}")
        if line.startswith("M "):
            if line[2:].strip():
                paths.add(line[2:].strip())
        elif line.startswith("P "):
            fields = line[2:].split()
            if len(fields) < 3 or fields[0].count(":") < 3:
                raise fail(f"the sampler: a process line that is not a context, a user and a name: {line!r}")
            processes.append((fields[0], fields[1], fields[-1]))
        elif line.startswith("N ") and line[2:].isdigit() and rounds is None:
            rounds = int(line[2:])
        elif line.startswith("status=") and line[7:].isdigit():
            status = int(line[7:])
        elif line.strip():
            raise fail(f"the sampler: a line that is none of M, P, N and status: {line!r}")
    if rounds is None or status is None:
        raise fail("the sampler's answer has no round count or no status")
    return Sampled(paths, processes, rounds, status)


def application_paths(guest, packages: list[str]) -> list[str]:
    """Where the platform keeps each package's code (`pm path`: the base package and the splits)."""
    found: list[str] = []
    for package in packages:
        listed = guest.shell(f"pm path {package}").splitlines()
        paths = [line[len("package:"):].strip() for line in listed if line.startswith("package:")]
        if not paths:
            raise fail(f"pm path {package} names no package file: {listed!r}")
        found.extend(paths)
    return found


def own_directory(path: str) -> str | None:
    """The directory an application's package file lives in when that directory is the application's own (named for the
    package file, or holding a base or a split package under the name the installer gives it), else None: a directory
    shared with other files is never a reason to refuse a path."""
    directory, _, name = path.rpartition("/")
    stem = name.rsplit(".", 1)[0]
    leaf = directory.rpartition("/")[2]
    named = leaf == stem or leaf.startswith(stem + "-") or stem == "base" or stem.startswith("split_")
    return directory if directory.count("/") >= 2 and named else None


def drawing_in_the_tool(sampled: Sampled, packages: list[str], package_files: list[str]) -> list[str]:
    """The paths that show the tool's own process reading an application or an overlay: a package file, a runtime
    overlay or its index, the resource cache, anything in the directory of a target application or in its data."""
    own = [directory for directory in map(own_directory, package_files) if directory is not None]
    found = []
    for path in sorted(sampled.paths):
        if path.endswith((".apk", ".frro", ".idmap")) or "/resource-cache/" in path or path in package_files \
                or any(path.startswith(directory + "/") for directory in own) \
                or any(path.startswith(f"/data/data/{package}/") or path.startswith(f"/data/user/0/{package}/")
                       for package in packages):
            found.append(path)
    return found


def renderer_domains(sampled: Sampled) -> str:
    """The renderer's two processes, in the domains and under the users the platform gives an ordinary application and its
    isolated service: not the shell's, and not a privileged application's."""
    provider = [(label, user) for label, user, name in sampled.processes if name == RENDERER_PACKAGE]
    isolated = [(label, user) for label, user, name in sampled.processes if name == RENDERER_ISOLATED_PROCESS]
    if not provider:
        raise fail(f"no process {RENDERER_PACKAGE} while icons ran: {sorted(set(sampled.processes))[:4]}")
    if not isolated:
        raise fail(f"no process {RENDERER_ISOLATED_PROCESS} while icons ran: {sorted(set(sampled.processes))[:4]}")
    for label, user in provider:
        if not PROVIDER_DOMAIN.fullmatch(label) or not PROVIDER_USER.fullmatch(user):
            raise fail(f"the renderer's provider process runs as {user} in {label}, not an ordinary application "
                       "(u0_a in untrusted_app)")
    for label, user in isolated:
        if not label.startswith(ISOLATED_CONTEXT) or not ISOLATED_USER.fullmatch(user):
            raise fail(f"the renderer's isolated process runs as {user} in {label}, not u0_i in {ISOLATED_CONTEXT}")
    return f"{provider[0][1]} in {provider[0][0].split(':s0')[0]}, {isolated[0][1]} in {isolated[0][0].split(':s0')[0]}"


def check_renderer_split(guest) -> str:
    listed = list_of(guest)
    built_in = [row.token for row in listed.rows if row.builtin]
    second = probe.component("second", "only")
    tokens = [*built_in[:MAX_ICONS - 1], second]
    if len(tokens) != MAX_ICONS:
        raise fail(f"only {len(built_in)} built-in launcher activities are listed: no batch of {MAX_ICONS} to draw")
    packages = sorted({token.partition("/")[0] for token in tokens})
    package_files = application_paths(guest, packages)
    result = guest.run(sampler_script(tokens), timeout=SAMPLER_SECONDS)
    if result.status != 0:
        raise fail(f"the sampler exited {result.status}: {(result.text + result.err).strip()[-300:]}")
    sampled = parse_sampler(result.text)
    if sampled.status != 0:
        raise fail(f"icons exited {sampled.status} under the sampler")
    if sampled.rounds < 1 or not any(path.rpartition("/")[2].startswith("app_process") for path in sampled.paths):
        raise fail(f"the sampler did not see the tool's process ({sampled.rounds} rounds, {len(sampled.paths)} paths): "
                   "it proves nothing")
    reads = drawing_in_the_tool(sampled, packages, package_files)
    if reads:
        raise fail(f"the tool's own process holds {len(reads)} application, overlay or resource files while it draws "
                   f"(the drawing is in the shell): {reads[:6]}")
    domains = renderer_domains(sampled)
    return (f"{len(sampled.paths)} distinct paths in {sampled.rounds} rounds of a tool over {len(packages)} applications "
            f"(the second probe's among them): none is a package file, an overlay, the resource cache or a target's own "
            f"directory; the renderer runs as {domains}")


def check_bad_icon(guest) -> str:
    listed = list_of(guest)
    built_in = [row.token for row in listed.rows if row.builtin][:12]
    good = [record.token for start in range(0, len(built_in), MAX_ICONS)
            for record in icons_of(guest, built_in[start:start + MAX_ICONS]) if record.status == "ok"][:6]
    if len(good) < 6:
        raise fail(f"only {len(good)} of the first built-in activities draw: no batch of seven good icons")
    bad = probe.component("bad", "only")
    batch = [*good[:3], bad, *good[3:], probe.component("second", "only")]
    statuses = {record.token: record.status for record in icons_of(guest, batch)}
    if statuses[bad] != "error":
        raise fail(f"the icon of {bad} (a foreground that is not a picture) is {statuses[bad]}, not error")
    broken = [token for token, status in statuses.items() if token != bad and status != "ok"]
    if broken:
        raise fail(f"the bad icon costs the batch more than itself: {broken} are not ok among {statuses}")
    again = icons_of(guest, [bad])[0].status
    if again != "error":
        raise fail(f"the bad icon asked again alone is {again}, not error")
    return ("among seven good icons (one resource-backed) the bad one is error and the seven are ok; "
            "asked again alone it is error again")


def check_untrusted_caller(guest, caller: pathlib.Path) -> str:
    control = guest.run(probe.CALL_RENDERER)
    if control.status != 0 or "Result:" not in control.text:
        raise fail(f"the shell's own call of the renderer's provider did not answer: {control.status} "
                   f"{(control.text + control.err).strip()[-300:]}")
    fresh_install(guest, caller, probe.CALLER_PACKAGE)
    try:
        guest.run(f"run-as {probe.CALLER_PACKAGE} rm -f {probe.CALLER_RESULT}")
        sent = guest.run(f"am broadcast -n {probe.CALLER_PACKAGE}/{probe.CALLER_RECEIVER}")
        if sent.status != 0 or "Broadcast completed" not in sent.text:
            raise fail(f"the caller's receiver was not started: {sent.status} {(sent.text + sent.err).strip()[-300:]}")
        deadline = time.monotonic() + CALLER_SECONDS
        answer = ""
        while time.monotonic() < deadline:
            read = guest.run(f"run-as {probe.CALLER_PACKAGE} cat {probe.CALLER_RESULT}")
            if read.status == 0 and "status=" in read.text:
                answer = read.text
                break
            time.sleep(0.5)
        if not answer:
            raise fail(f"the caller left no answer in {CALLER_SECONDS} s")
    finally:
        guest.adb_run("uninstall", probe.CALLER_PACKAGE, timeout=60)
    if "ACCESS_CONTENT_PROVIDERS_EXTERNALLY" not in answer or "Result:" in answer:
        raise fail(f"an ordinary application's call of the renderer was not refused for the shell-only permission: "
                   f"{answer.strip()[-400:]}")
    return ("the shell's own call is answered; an ordinary application's call is refused for "
            "ACCESS_CONTENT_PROVIDERS_EXTERNALLY")


RESUMED_ACTIVITY = re.compile(r"ResumedActivity[=:] *ActivityRecord\{\S+ u\d+ (\S+)[^}]*\}")


def resumed_components(dump: str) -> list[str]:
    """The whole components (package/class, the short form `package/.Class` expanded) the dump names as resumed."""
    found = []
    for line in dump.splitlines():
        match = RESUMED_ACTIVITY.search(line) if "ResumedActivity" in line else None
        if match:
            package, _, name = match.group(1).partition("/")
            found.append(f"{package}/{package + name if name.startswith('.') else name}")
    return found


def resumed(guest, token: str, seconds: float | None = None, alternatives: tuple[str, ...] = ()) -> bool:
    """Whether the component becomes the resumed activity within the time: the whole component, not a part of its
    name (an alias may show as the activity it stands for: name that class in `alternatives`)."""
    seconds = LAUNCH_SECONDS if seconds is None else seconds
    package = token.split("/")[0]
    wanted = {token, *(f"{package}/{name}" for name in alternatives)}
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if wanted & set(resumed_components(guest.run("dumpsys activity activities", timeout=60).text)):
            return True
        time.sleep(1)
    return False


def go_home(guest, package: str, seconds: float | None = None) -> None:
    """Presses HOME and waits, with a bound, until none of the package's activities is the resumed one."""
    seconds = LAUNCH_SECONDS if seconds is None else seconds
    guest.shell("input keyevent KEYCODE_HOME")
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        dump = guest.run("dumpsys activity activities", timeout=60).text
        if not any(component.partition("/")[0] == package for component in resumed_components(dump)):
            return
        time.sleep(1)
    raise fail(f"{package} is still the resumed application {seconds:g} s after the home key")


def check_launch(guest) -> str:
    target = probe.component("first", "listed")
    result = command_run(guest, "launch", target)
    if result.status != 0 or result.out != f"launched\t{target}\n".encode() or result.err.strip():
        raise fail(f"launch {target}: exit {result.status}, stdout {result.out!r}, stderr {result.err.strip()!r}")
    if not resumed(guest, target):
        raise fail(f"{target} did not become the resumed activity within {LAUNCH_SECONDS} s")
    go_home(guest, probe.package_of("first"))
    again = command_run(guest, "launch", target)
    if again.status != 0 or again.out != f"launched\t{target}\n".encode() or again.err.strip():
        raise fail(f"launch {target} again: exit {again.status}, stdout {again.out!r}, stderr {again.err.strip()!r}")
    if not resumed(guest, target):
        raise fail(f"{target} is not the resumed activity after launching it again (the app already open)")
    return f"{target} started, resumed, and brought to the front again"


def check_alias(guest) -> str:
    """A launcher alias that the manifest disables and `pm enable` has enabled for the user is a launcher activity: the
    command lists it, answers its icon and starts it, and takes it back once it is disabled again."""
    alias = probe.alias_component()
    user = int(guest.shell("am get-current-user").strip())
    first = list_of(guest)
    if any(row.token == alias for row in first.rows):
        raise fail(f"{alias} is listed while the manifest disables it")
    absent = icons_of(guest, [alias])[0]
    if absent.status != "missing":
        raise fail(f"the icon of the disabled alias is {absent.status}, not missing")
    refused = command_run(guest, "launch", alias)
    if refused.status != EXIT_REFUSED or refused.out:
        raise fail(f"launch of the disabled alias: exit {refused.status}, stdout {refused.out[:60]!r}")
    guest.shell(f"pm enable --user {user} {alias}")
    try:
        enabled = list_of(guest)
        rows = [row for row in enabled.rows if row.token == alias]
        if [(row.label, row.builtin) for row in rows] != [(probe.LISTED_ALIAS_LABEL, False)]:
            raise fail(f"the alias enabled for the user is listed as {rows}, not once with {probe.LISTED_ALIAS_LABEL!r}")
        if enabled.header.digest == first.header.digest:
            raise fail("enabling the alias leaves the digest as it was")
        icon = icons_of(guest, [alias])[0]
        if icon.status != "noicon":
            raise fail(f"the icon of the enabled alias is {icon.status}, not noicon (the application declares none)")
        go_home(guest, probe.package_of("first"))
        started = command_run(guest, "launch", alias)
        if started.status != 0 or started.out != f"launched\t{alias}\n".encode() or started.err.strip():
            raise fail(f"launch {alias}: exit {started.status}, stdout {started.out!r}, stderr {started.err.strip()!r}")
        if not resumed(guest, alias, alternatives=(probe.ALIAS_TARGET,)):
            raise fail(f"{alias} did not become the resumed activity within {LAUNCH_SECONDS} s")
        guest.shell("input keyevent KEYCODE_HOME")
    finally:
        guest.shell(f"pm disable --user {user} {alias}")
    after = list_of(guest)
    if any(row.token == alias for row in after.rows) or after.header.digest != first.header.digest:
        raise fail("the alias is still listed after it is disabled again, or the digest is not the earlier one")
    return "disabled in the manifest: not listed, missing, refused; enabled for the user: listed, noicon, started; " \
           "disabled again: gone, earlier digest back"


def check_refusals(guest) -> str:
    first = probe.package_of("first")
    refused = [
        probe.component("first", "hidden"),      # not exported
        probe.component("first", "plain"),       # exported, no launcher filter
        probe.component("first", "guarded"),     # exported with a launcher filter, behind a permission the shell lacks
        SETTINGS_NOT_LAUNCHER,                   # a built-in application's activity that is not a launcher one
        f"{first}/android.app.Missing",          # an activity the package does not have
        "org.example.andarium.appsprobe.absent/android.app.Activity",   # a package that is not installed
        f"{first}/.Activity",                    # the shorthand
        f"{first}",                              # no activity
        "/",
    ]
    hostile = [
        f"{probe.component('first', 'listed')}; touch {MARKER}",
        f"{probe.component('first', 'listed')} && touch {MARKER}",
        f"$(touch {MARKER})",
        f"`touch {MARKER}`",
        f"{first}/android.app.Activity$(touch {MARKER})",
        f"{first}/android.app.Activity\ntouch {MARKER}",
        f"-n {probe.component('first', 'listed')}",
        f"--user 0 {probe.component('first', 'listed')}",
        f"{probe.component('first', 'listed')} {probe.component('second', 'only')}",
        f"{first}/android.app.Activity' ; touch {MARKER} ; '",
        f"{first}/android.app.Activity --es x y",
        f"{first}/android.app.Activity|sh",
    ]
    guest.run(f"rm -f {MARKER}")
    for token in refused + hostile:
        result = command_run(guest, "launch", token)
        if result.status != EXIT_REFUSED or result.out or not result.err.strip():
            raise fail(f"launch {token!r}: exit {result.status} (not {EXIT_REFUSED}), stdout {result.out[:80]!r}, "
                       f"stderr {result.err.strip()[:120]!r}")
        if len(result.err.encode()) > 512 or "\n" in result.err.strip():
            raise fail(f"launch {token!r}: standard error is not one line of at most 512 bytes")
    if guest.run(f"ls {MARKER}").status == 0:
        raise fail(f"{MARKER} exists: an argument ran as a command")
    # A disabled application is neither listed nor started.
    package = probe.package_of("second")
    target = probe.component("second", "only")
    guest.shell(f"pm disable-user --user 0 {package}")
    try:
        disabled = command_run(guest, "launch", target)
        listed = list_of(guest)
        if disabled.status != EXIT_REFUSED or disabled.out:
            raise fail(f"launch of the disabled {target}: exit {disabled.status}")
        if rows_of(listed, package):
            raise fail(f"the disabled application is listed: {rows_of(listed, package)}")
    finally:
        guest.shell(f"pm enable {package}")
    return f"{len(refused)} components and {len(hostile)} hostile arguments refused with status 2 and no output; " \
           "the marker was never made; a disabled application is refused and unlisted"


def check_usage(guest) -> str:
    for words in ((), ("list", "extra"), ("digest", "x"), ("--locale", "en"), ("list", "--locale", "en"),
                  ("icons",), ("launch",), ("launch", "a.b/C", "d.e/F"), ("LIST",), ("help",)):
        result = command_run(guest, *words)
        if result.status != EXIT_USAGE or result.out:
            raise fail(f"'{COMMAND} {' '.join(words)}' exits {result.status} with {len(result.out)} bytes of "
                       f"stdout, not {EXIT_USAGE} and none")
    return "unknown verbs, flags and argument counts are status 64 with nothing on stdout"


AVC_DENIED = re.compile(r'avc:\s+denied\s+\{([^}]*)\}(.*?)\bscontext=(\S+)\s+tcontext=(\S+)\s+tclass=(\S+)')
AVC_NAME = re.compile(r'\b(?:name|path)=("[^"]*"|\S+)')
AVC_BUG = re.compile(r'\bbug=(\S+)')


def audit_key(line: str, domain: str = SHELL_CONTEXT) -> tuple[str, ...] | None:
    """The identity of a denial: the record type, the permissions, the source and target contexts, the class and the
    name or path. The time, the pid, the audit serial, the comm and the log tag are left out, so the two lines the log
    holds for one denial are one key. None when the line is no denial to the domain."""
    if "avc" not in line or "denied" not in line:
        return None
    found = AVC_DENIED.search(line)
    if found is None:
        # An audit line this runner cannot take apart still counts, by its text from the record on.
        return ("unparsed", line[line.index("avc"):].strip()) if f"scontext={domain}" in line else None
    permissions, between, source, target, kind = found.groups()
    if not source.startswith(domain):
        return None
    name = AVC_NAME.search(between)
    return ("avc", " ".join(sorted(permissions.split())), source, target, kind, name.group(1) if name else "")


LOG_TIME = re.compile(r"^\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}$")


def window_start(guest) -> str:
    """Where the window the command's calls are judged in begins: a whole second of the device's clock after everything
    the earlier steps did (logcat takes a time to the millisecond), so no line of theirs shares it."""
    time.sleep(1.1)
    stamp = guest.shell("date +'%m-%d %H:%M:%S.000'").strip()
    if not LOG_TIME.match(stamp):
        raise fail(f"the device's clock reads {stamp!r}, not MM-DD HH:MM:SS.mmm")
    return stamp


def denials_since(guest, since: str, domain: str = SHELL_CONTEXT) -> list[str]:
    """The audit lines of denials to a domain (the shell's unless another is named) that the log holds from `since` on.
    Only the window is read, so a line the buffer dropped before it cannot stand in for one in it: any denial in the
    window is a denial. A log that cannot be read is a failure."""
    read = guest.run(f"logcat -d -b all -T {shlex.quote(since)}", timeout=60)
    if read.status != 0:
        raise fail(f"the log could not be read: logcat -d -b all -T exited {read.status}: {read.err.strip()[-200:]}")
    return [line.strip() for line in read.text.splitlines() if audit_key(line, domain) is not None]


def audit_report(lines: list[str], domain: str) -> str:
    """What the audit lines of a domain say, one clause for each denial (the log holds two lines for one): its
    permissions, the class, the name or path it names, the target type and the bug note the policy tags it with."""
    seen: dict[tuple[str, ...], list] = {}
    for line in lines:
        key = audit_key(line, domain)
        if key is None:
            continue
        entry = seen.setdefault(key, [line, 0])
        entry[1] += 1
    if not seen:
        return f"{domain} audit lines: none"
    clauses = []
    for key, (line, count) in seen.items():
        bug = AVC_BUG.search(line)
        if key[0] == "avc":
            head = f"{{ {key[1]} }} {key[4]} {key[5]} -> {key[3]}".replace("  ", " ")
        else:
            head = key[1]
        clauses.append(f"{head}{' (' + bug.group(1) + ')' if bug else ''} x{count}")
    return f"{domain} audit lines (reported, not judged): " + "; ".join(clauses)


def tmp_listing(guest) -> str:
    return guest.shell(f"ls -A {SHELL_DATA}")


def check_set_changes(guest, apks: dict[str, pathlib.Path]) -> str:
    # Another application: the digest changes, and the rows of the others do not.
    second = probe.package_of("second")
    guest.shell(f"pm uninstall {second}")
    without = digest_of(guest)
    fresh_install(guest, apks["second"], second)
    with_second = digest_of(guest)
    if with_second.digest == without.digest:
        raise fail("installing an application leaves the digest as it was")
    guest.shell(f"pm uninstall {second}")
    removed = digest_of(guest)
    if removed.digest != without.digest:
        raise fail("the digest after removing the application is not the one from before it was installed")
    fresh_install(guest, apks["second"], second)
    # An update in place: the digest changes, the updated application's icon keys change, the others' do not.
    first = probe.package_of("first")
    before = list_of(guest)
    install(guest, apks["first-v2"], first)
    after = list_of(guest)
    if after.header.digest == before.header.digest:
        raise fail("updating an application in place leaves the digest as it was")
    keys_before = {row.token: row.key for row in rows_of(before, first)}
    keys_after = {row.token: row.key for row in rows_of(after, first)}
    if set(keys_after) != set(keys_before) or any(keys_after[token] == keys_before[token] for token in keys_after):
        raise fail(f"the icon keys after the update are {keys_after}, before {keys_before}: they must all change")
    others_before = {row.token: row.key for row in before.rows if row.package != first}
    others_after = {row.token: row.key for row in after.rows if row.package != first}
    if others_before != others_after:
        raise fail("the icon keys of applications that did not change moved, or the set of the others did")
    # A disabled application is gone from the listing and moves the digest.
    guest.shell(f"pm disable-user --user 0 {probe.package_of('first')}")
    try:
        disabled = list_of(guest)
        if rows_of(disabled, probe.package_of("first")) or disabled.header.digest == after.header.digest:
            raise fail("disabling an application leaves it listed or the digest unchanged")
    finally:
        guest.shell(f"pm enable {probe.package_of('first')}")
    return ("install, removal, update in place and disable each move the digest as they should; removal gives the "
            "earlier digest back; only the updated application's icon keys change")


def check_clean(guest, before_files: str, since: str) -> str:
    after_files = tmp_listing(guest)
    if after_files != before_files:
        raise fail(f"{SHELL_DATA} changed: before {before_files.split()}, after {after_files.split()}")
    found = denials_since(guest, since)
    if found:
        # One line for each denial (the log holds two), as the log gives it, with its comm and pid where it has them.
        first: dict[tuple[str, ...], str] = {}
        for line in found:
            first.setdefault(audit_key(line), line)
        raise fail(f"the shell domain was denied something since {since}:\n" + "\n".join(list(first.values())[:8]))
    return (f"{SHELL_DATA} unchanged; no denial of {SHELL_CONTEXT} since {since}; "
            + audit_report(denials_since(guest, since, ISOLATED_CONTEXT), ISOLATED_CONTEXT))


def open_guest(options: argparse.Namespace, adb: str, private: pathlib.Path):
    """The guest the steps drive, and what stops it. The steps read a command's status and its streams apart, which only
    a Serial gives: a guest this runner launches is driven through one too."""
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
    keystore = private / "first-family.p12"   # `first` and its update are signed with one key
    apks = {variant: probe.build_apk(private / f"{variant}.apk", variant, keystore=keystore)
            for variant in (*probe.VARIANTS, *probe.SUCCESSORS)}
    caller = probe.build_caller_apk(private / "caller.apk", keystore=private / "caller-family.p12")
    guest, stop = open_guest(options, adb, private)
    try:
        report.run("the command is in the image", lambda: command_present(guest))
        for variant in probe.VARIANTS:
            report.run(f"{variant}: install", lambda variant=variant: (
                fresh_install(guest, apks[variant], probe.package_of(variant)) or f"{probe.package_of(variant)} installed"))
        before_files = tmp_listing(guest)
        since = window_start(guest)   # before the command's first call: the log is judged from here on
        report.run("digest and list agree on the header block", lambda: check_header(guest))
        report.run("list names the built-in and the probe's applications, escaped and bounded", lambda: check_rows(guest))
        report.run("icons are PNG records that check out", lambda: check_icons(guest))
        report.run("the drawing is done in the renderer's two processes, not in the shell", lambda: check_renderer_split(guest))
        report.run("a bad icon costs only itself", lambda: check_bad_icon(guest))
        report.run("an ordinary application cannot use the renderer", lambda: check_untrusted_caller(guest, caller))
        report.run("launch starts a launcher activity", lambda: check_launch(guest))
        report.run("a launcher alias enabled for the user is listed, drawn and started", lambda: check_alias(guest))
        report.run("launch refuses all else, and runs nothing it is given", lambda: check_refusals(guest))
        report.run("usage errors are status 64", lambda: check_usage(guest))
        report.run("the command writes nothing and is denied nothing", lambda: check_clean(guest, before_files, since))
        report.run("the digest follows the set of applications", lambda: check_set_changes(guest, apks))
    finally:
        for variant in probe.VARIANTS:
            guest.adb_run("uninstall", probe.package_of(variant), timeout=60)
        guest.adb_run("uninstall", probe.CALLER_PACKAGE, timeout=60)
        guest.run(f"rm -f {MARKER}")
        stop()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release built with the command")
    parser.add_argument("--serial", help="a running, provisioned guest (host:port; the home key must reach the launcher); "
                        "no guest is launched")
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
    adb = orient.boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so every socket path below it stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-appshort-"))
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
