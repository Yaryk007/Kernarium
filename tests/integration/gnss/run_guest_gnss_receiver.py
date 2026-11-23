#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the guest's GNSS receiver reads the host's synthetic position.

The launcher gives the guest the Virtio GNSS device with a fixed synthetic
position (``--location=fixed:...``; nothing here reads a real location). The
guest's driver registers ``/dev/gnss0`` of type NMEA, and the runner checks:

* the node exists, its class says NMEA, and the host powers the receiver only
  while something has the node open: nothing is sent, and the launcher reports
  no power-on, until a reader opens it;
* a reader gets whole epochs of GGA, RMC, GSA, GST, VTG and PKRNF about once a
  second, every checksum right, the fix precise (GGA quality 1, RMC mode A,
  class P), the coordinates, altitude and accuracies of the position, and a
  UTC time that is the host's;
* the host sees the power go on at the first open and off at the last close,
  and a reader that opens again after the node was closed for a while starts
  with a fresh epoch, not bytes left from before;
* a suspend the guest enters by itself with the receiver closed holds until a
  wake request, as it does without the device;
* a suspend the guest enters by itself with the receiver open ends without any
  request, within a few seconds, because the host wakes a guest that takes no
  epoch, and the reader then gets fresh epochs again.

The contract is the device's README, darwin/device/gnss/README.md. A host
suspend (the host's quiesce) cannot be asked of the launcher; the device tests
cover the queues across it.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import calendar
import os
import pathlib
import re
import shutil
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402
# The runner of the suspend contract owns the management console and the
# suspend vocabulary.
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "kernel"))
from run_guest_suspend_holds import (  # noqa: E402
    HOLD_SECONDS,
    SELF_SUSPEND,
    SUSPEND_ENTRY,
    SUSPEND_EXIT,
    ManagementConsole,
    drain,
    hold_quietly,
    require_clean,
    wait_for_exit_line,
    wait_for_line,
)


VCPUS = 2
BOOT_SECONDS = 180
LOCATION = "fixed:35.6895,139.6917,40,8"
READER = "/opt/uml-test/gnss_reader.py"
NODE = "/dev/gnss0"
RECEIVER_LINE = re.compile(r"GNSS receiver: (on|off)$", re.MULTILINE)

# Bounds. Healthy results are far below them; a stall costs a timeout.
COMMAND_SECONDS = 60
POWER_SECONDS = 30
IDLE_SECONDS = 2.5
READ_SECONDS = 4
REOPEN_SECONDS = 2.5
CLOSED_SECONDS = 3.0
SUSPEND_READ_SECONDS = 40
# The host wakes a guest that took no epoch for a period; the period, the
# wake and the resume together stay under this.
WAKE_LIMIT_SECONDS = 10.0
# The guest's clock is the host's, so a fix's time is the time it was read.
CLOCK_TOLERANCE_SECONDS = 5.0
EPOCH_SENTENCES = ("GNGGA", "GNRMC", "GNGSA", "GNGST", "GNVTG", "PKRNF")

GUEST_READER = r'''
import os, select, sys, time

seconds, log = float(sys.argv[1]), sys.argv[2]
descriptor = os.open("/dev/gnss0", os.O_RDONLY | os.O_NONBLOCK)
buffer = b""
with open(log, "w") as out:
    print("opened %.3f" % time.time(), file=out, flush=True)
    end = time.time() + seconds
    while time.time() < end:
        if not select.select([descriptor], [], [], 0.5)[0]:
            continue
        buffer += os.read(descriptor, 4096)
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            print("%.3f %s" % (time.time(), line.decode(errors="replace").rstrip()),
                  file=out, flush=True)
    print("closing %.3f" % time.time(), file=out, flush=True)
os.close(descriptor)
'''


def checksum_ok(sentence: str) -> bool:
    if not sentence.startswith("$") or sentence[-3] != "*":
        return False
    body = sentence[1:-3]
    value = 0
    for character in body.encode():
        value ^= character
    return sentence[-2:] == "%02X" % value


class Epoch:
    """One epoch as the reader logged it: when its first sentence arrived and
    the six sentences, split into fields."""

    def __init__(self, received: float) -> None:
        self.received = received
        self.sentences: dict[str, list[str]] = {}

    def add(self, sentence: str) -> None:
        if not checksum_ok(sentence):
            raise RuntimeError(f"bad checksum: {sentence!r}")
        fields = sentence[1:-3].split(",")
        if fields[0] in self.sentences:
            raise RuntimeError(f"{fields[0]} twice in one epoch")
        self.sentences[fields[0]] = fields

    def fix_time(self) -> float:
        """The UTC time of the epoch's sentences, as seconds since 1970."""
        time_text = self.sentences["GNGGA"][1]
        date_text = self.sentences["GNRMC"][9]
        moment = time.strptime(date_text + time_text[:6], "%d%m%y%H%M%S")
        return calendar.timegm(moment) + float(time_text[6:])


def read_log(guest: Guest, path: str) -> tuple[list[Epoch], float, float | None]:
    """The epochs a reader logged, when it opened and when it closed."""
    text = guest.check_command(f"cat {path}", COMMAND_SECONDS)
    epochs: list[Epoch] = []
    opened = closed = None
    for line in text.splitlines():
        words = line.split(None, 1)
        if not words:
            continue
        if words[0] == "opened":
            opened = float(words[1])
        elif words[0] == "closing":
            closed = float(words[1])
        elif re.fullmatch(r"\d+\.\d+", words[0]) and len(words) == 2:
            sentence = words[1]
            if sentence.startswith("$GNGGA"):
                epochs.append(Epoch(float(words[0])))
            elif not epochs:
                raise RuntimeError(f"a sentence before the first GGA: {sentence!r}")
            epochs[-1].add(sentence)
    if opened is None:
        raise RuntimeError(f"the reader's log has no open time:\n{text[-1000:]}")
    return epochs, opened, closed


def check_epoch(epoch: Epoch) -> None:
    if tuple(epoch.sentences) != EPOCH_SENTENCES:
        raise RuntimeError(
            f"an epoch holds {tuple(epoch.sentences)}, not {EPOCH_SENTENCES}")
    gga, rmc, gsa, gst, vtg, kernarium = (
        epoch.sentences[name] for name in EPOCH_SENTENCES)
    expected_gga = ["GNGGA", gga[1], "3541.3700000", "N", "13941.5020000", "E",
                    "1", "", "", "40.0", "M", "0.0", "M", "", ""]
    if gga != expected_gga:
        raise RuntimeError(f"GGA {gga} is not {expected_gga}")
    if rmc[2] != "A" or rmc[3:7] != ["3541.3700000", "N", "13941.5020000", "E"] \
            or rmc[12] != "A" or rmc[1] != gga[1]:
        raise RuntimeError(f"RMC {rmc} is not a precise fix of the position")
    if gsa[1:3] != ["A", "3"]:
        raise RuntimeError(f"GSA {gsa} is not a three dimensional fix")
    if gst[1] != gga[1] or gst[3] != "5.66" or gst[8] != "10.0":
        raise RuntimeError(f"GST {gst} does not carry the accuracies")
    if vtg[2] != "T" or vtg[9] != "A":
        raise RuntimeError(f"VTG {vtg} is not a precise fix")
    if kernarium != ["PKRNF", "P", "8.0", "10.0", "", ""]:
        raise RuntimeError(f"PKRNF {kernarium} is not the precise class")
    skew = abs(epoch.fix_time() - epoch.received)
    if skew > CLOCK_TOLERANCE_SECONDS:
        raise RuntimeError(
            f"an epoch's time is {skew:.1f} s from when it was read")


def receiver_events(guest: Guest) -> list[str]:
    """What the launcher reported of the receiver's power, in order."""
    return RECEIVER_LINE.findall(guest.host_output())


def wait_for_events(guest: Guest, events: list[str], what: str) -> None:
    deadline = time.monotonic() + POWER_SECONDS
    while receiver_events(guest) != events:
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"{what}: the host saw {receiver_events(guest)}, not {events}")
        drain(guest, 0.05)


def run_presence(guest: Guest) -> None:
    kind = guest.check_command("cat /sys/class/gnss/gnss0/type", COMMAND_SECONDS)
    if kind.strip() != "NMEA":
        raise RuntimeError(f"/dev/gnss0 is of type {kind.strip()!r}, not NMEA")
    guest.check_command(
        f"[ -c {NODE} ] || mknod {NODE} c $(sed 's/:/ /' /sys/class/gnss/gnss0/dev)",
        COMMAND_SECONDS)
    # The node's parent is the Virtio device, and the driver bound to it is
    # the receiver's.
    driver = guest.check_command(
        "basename $(readlink /sys/class/gnss/gnss0/device/driver)",
        COMMAND_SECONDS).strip()
    if "kernarium" not in driver:
        raise RuntimeError(f"/dev/gnss0 is served by {driver!r}")
    # Nothing powers the receiver until a reader opens the node.
    drain(guest, IDLE_SECONDS)
    if receiver_events(guest):
        raise RuntimeError(
            f"the host saw {receiver_events(guest)} with the node closed")
    print(f"presence: /dev/gnss0 is NMEA, served by {driver}, and the "
          "receiver stays off while the node is closed")


def run_epochs(guest: Guest) -> None:
    guest.check_command(f"python3 {READER} {READ_SECONDS} /run/read1.log",
                        COMMAND_SECONDS + READ_SECONDS)
    wait_for_events(guest, ["on", "off"], "epochs")
    epochs, opened, closed = read_log(guest, "/run/read1.log")
    if len(epochs) < READ_SECONDS - 1:
        raise RuntimeError(f"{len(epochs)} epochs in {READ_SECONDS} s")
    for epoch in epochs:
        check_epoch(epoch)
    # One at the open and about one a second after, never a burst or a gap.
    gaps = [b.received - a.received for a, b in zip(epochs, epochs[1:])]
    if epochs[0].received - opened > 3.0 or min(gaps) < 0.4 or max(gaps) > 3.0:
        raise RuntimeError(f"epochs at the wrong pace: gaps {gaps}, first "
                           f"{epochs[0].received - opened:.2f} s after the open")
    print(f"epochs: {len(epochs)} whole precise epochs in {READ_SECONDS} s, "
          f"gaps {min(gaps):.2f} to {max(gaps):.2f} s, power on then off")


def run_reopen(guest: Guest) -> None:
    drain(guest, CLOSED_SECONDS)
    guest.check_command(f"python3 {READER} {REOPEN_SECONDS} /run/read2.log",
                        COMMAND_SECONDS + REOPEN_SECONDS)
    wait_for_events(guest, ["on", "off", "on", "off"], "reopen")
    epochs, opened, _ = read_log(guest, "/run/read2.log")
    if not epochs:
        raise RuntimeError("the reopened node gave no epoch")
    for epoch in epochs:
        check_epoch(epoch)
    # Nothing from before the close: the first epoch was made for this open.
    if epochs[0].fix_time() < opened - 1.0:
        raise RuntimeError(
            f"the first epoch after a reopen is {opened - epochs[0].fix_time():.1f} "
            "s older than the open")
    print(f"reopen: after {CLOSED_SECONDS:g} s closed the first epoch is fresh "
          "and the host saw the power go on and off again")


def run_suspend_closed(guest: Guest, console: ManagementConsole) -> None:
    exits = guest.output().count(SUSPEND_EXIT)
    entries = guest.output().count(SUSPEND_ENTRY)
    guest._write(SELF_SUSPEND)
    wait_for_line(guest, SUSPEND_ENTRY, entries, "suspend closed")
    hold_quietly(guest, exits, HOLD_SECONDS, "suspend with the receiver closed")
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"suspend closed: wake replied {answer!r}")
    wait_for_exit_line(guest, exits, "suspend closed: the guest stayed suspended")
    if receiver_events(guest) != ["on", "off", "on", "off"]:
        raise RuntimeError("the receiver was powered by a suspend with it closed")
    print(f"suspend closed: a suspend held {HOLD_SECONDS:g} s with the receiver "
          "closed and ended only at the wake request")


def run_suspend_open(guest: Guest) -> None:
    guest._write(f"python3 {READER} {SUSPEND_READ_SECONDS} /run/read3.log &\n")
    wait_for_events(guest, ["on", "off", "on", "off", "on"], "suspend open")
    drain(guest, 2.0)
    exits = guest.output().count(SUSPEND_EXIT)
    entries = guest.output().count(SUSPEND_ENTRY)
    guest._write(SELF_SUSPEND)
    wait_for_line(guest, SUSPEND_ENTRY, entries, "suspend open")
    started = time.monotonic()
    # No console wake: the receiver's epoch that nobody takes ends it.
    wait_for_exit_line(
        guest, exits,
        "suspend open: the guest stayed suspended with its receiver on")
    resumed = time.monotonic() - started
    if resumed > WAKE_LIMIT_SECONDS:
        raise RuntimeError(
            f"the receiver woke the guest only after {resumed:.1f} s")
    resumed_at = time.time()
    # The reader runs on, then closes by itself.
    deadline = time.monotonic() + SUSPEND_READ_SECONDS + POWER_SECONDS
    while receiver_events(guest) != ["on", "off", "on", "off", "on", "off"]:
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"suspend open: the host saw {receiver_events(guest)} at the end")
        drain(guest, 0.25)
    epochs, _, closed = read_log(guest, "/run/read3.log")
    if closed is None:
        raise RuntimeError("the suspended reader never closed the node")
    for epoch in epochs:
        check_epoch(epoch)
    after = [epoch for epoch in epochs if epoch.received >= resumed_at - 1.0]
    if len(after) < 10:
        raise RuntimeError(f"only {len(after)} epochs after the resume")
    # The first epoch after the gap was made after the guest woke.
    gaps = [(b.received - a.received, b) for a, b in zip(epochs, epochs[1:])]
    longest, resumed_epoch = max(gaps, key=lambda gap: gap[0])
    if resumed_epoch.received - resumed_epoch.fix_time() > 2.0:
        raise RuntimeError(
            f"the first epoch after the suspend is "
            f"{resumed_epoch.received - resumed_epoch.fix_time():.1f} s old")
    print(f"suspend open: the receiver woke a suspended guest in {resumed:.1f} s, "
          f"the longest gap between epochs was {longest:.1f} s, {len(after)} "
          "fresh epochs followed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-gnss-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        with Guest(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            "1G",
            "tmpfs",
            (f"ncpus={VCPUS}", f"--location={LOCATION}"),
            overlay_shell=True,
            separate_host_output=True,
            payloads={READER: GuestFile(GUEST_READER.encode(), 0o755)},
        ) as guest:
            guest.boot_to_shell(BOOT_SECONDS)
            console = ManagementConsole(guest, private)
            request = len(guest.output())
            run_presence(guest)
            run_epochs(guest)
            run_reopen(guest)
            run_suspend_closed(guest, console)
            run_suspend_open(guest)
            require_clean(guest, "gnss", request)
            if not guest.shutdown(60):
                raise RuntimeError("the guest did not power off")
            host = guest.host_output()
            if "vhost-user connection failed" in host:
                raise RuntimeError(f"the GNSS device failed:\n{host[-2000:]}")
    finally:
        shutil.rmtree(private, ignore_errors=True)
    print("the guest's GNSS receiver read the host's synthetic position")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
