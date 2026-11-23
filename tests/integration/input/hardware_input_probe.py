#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-hardware-input: the input devices the host plugs and unplugs.

Runs in the guest, driven by run_guest_hardware_input.py through the shell.

  list                      one JSON object per input device
  wait NAME present|absent SECONDS
                            exit 0 once a device of that name is (or is no
                            longer) in sysfs, 1 at the deadline
  watch NAME LOG SECONDS    wait for the device, open its event node, and
                            append to LOG "open EVENT", then one "ev TYPE CODE
                            VALUE" line per event but SYN_REPORT, and "gone"
                            when the node reports the device is gone
  led NAME CODE VALUE       write one EV_LED event to the device's node
"""

import errno
import glob
import json
import os
import select
import struct
import sys
import time

INPUT_CLASS = "/sys/class/input"
EVENT = struct.Struct("llHHi")
EV_SYN, EV_LED = 0x00, 0x11
CAPABILITIES = ("ev", "key", "rel", "abs", "led", "msc", "sw", "snd")
WORD_BITS = 64


def read(path):
    with open(path) as handle:
        return handle.read().strip()


def bitmap(path):
    """A sysfs capability bitmap, most significant word first, as an integer."""
    value = 0
    for word in read(path).split():
        value = (value << WORD_BITS) | int(word, 16)
    return value


def devices():
    found = []
    for path in sorted(glob.glob(f"{INPUT_CLASS}/input[0-9]*")):
        try:
            nodes = [os.path.basename(node) for node in glob.glob(f"{path}/event*")]
            found.append(
                {
                    "name": read(f"{path}/name"),
                    "uniq": read(f"{path}/uniq"),
                    "bus": int(read(f"{path}/id/bustype"), 16),
                    "vendor": int(read(f"{path}/id/vendor"), 16),
                    "product": int(read(f"{path}/id/product"), 16),
                    "version": int(read(f"{path}/id/version"), 16),
                    "properties": bitmap(f"{path}/properties"),
                    "event": nodes[0] if nodes else None,
                    **{
                        capability: bitmap(f"{path}/capabilities/{capability}")
                        for capability in CAPABILITIES
                    },
                }
            )
        except (OSError, ValueError, IndexError):
            # The device went while it was being read.
            continue
    return found


def find(name):
    for device in devices():
        if device["name"] == name and device["event"]:
            return device
    return None


def present(name):
    return any(device["name"] == name for device in devices())


def command_wait(name, state, seconds):
    deadline = time.monotonic() + float(seconds)
    while time.monotonic() < deadline:
        if present(name) == (state == "present"):
            print(f"{name}: {state}")
            return 0
        time.sleep(0.05)
    print(f"{name}: not {state} after {seconds} s")
    return 1


def command_watch(name, log_path, seconds):
    deadline = time.monotonic() + float(seconds)
    device = None
    while device is None and time.monotonic() < deadline:
        device = find(name)
        if device is None:
            time.sleep(0.05)
    if device is None:
        print(f"{name}: no device")
        return 1
    descriptor = os.open(f"/dev/input/{device['event']}", os.O_RDONLY | os.O_NONBLOCK)
    with open(log_path, "w", buffering=1) as log:
        log.write(f"open {device['event']}\n")
        while time.monotonic() < deadline:
            readable, _, _ = select.select([descriptor], [], [], 0.2)
            if not readable:
                continue
            try:
                data = os.read(descriptor, EVENT.size * 64)
            except BlockingIOError:
                continue
            except OSError as error:
                if error.errno == errno.ENODEV:
                    log.write("gone\n")
                    return 0
                raise
            for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, kind, code, value = EVENT.unpack_from(data, offset)
                if kind != EV_SYN:
                    log.write(f"ev {kind} {code} {value}\n")
        log.write("deadline\n")
    return 1


def command_led(name, code, value):
    device = find(name)
    if device is None:
        print(f"{name}: no device")
        return 1
    descriptor = os.open(f"/dev/input/{device['event']}", os.O_WRONLY)
    try:
        os.write(descriptor, EVENT.pack(0, 0, EV_LED, int(code), int(value)))
        os.write(descriptor, EVENT.pack(0, 0, EV_SYN, 0, 0))
    finally:
        os.close(descriptor)
    return 0


def main(arguments):
    if arguments[:1] == ["list"]:
        for device in devices():
            print(json.dumps(device))
        return 0
    if arguments[:1] == ["wait"] and len(arguments) == 4 and arguments[2] in ("present", "absent"):
        return command_wait(arguments[1], arguments[2], arguments[3])
    if arguments[:1] == ["watch"] and len(arguments) == 4:
        return command_watch(arguments[1], arguments[2], arguments[3])
    if arguments[:1] == ["led"] and len(arguments) == 4:
        return command_led(arguments[1], arguments[2], arguments[3])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
