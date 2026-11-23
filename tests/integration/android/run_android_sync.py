#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the management console's sync on an Android release.

Android's init leaves magic SysRq off, so the host's ``sysrq s`` request is
refused there and a hard end of the process loses what the guest still holds
dirty. The ``sync`` command needs no SysRq: it runs sync(2) in the guest's
process context and replies ``synced in N ms`` only after the call returned
(the management-console paragraph of kernel/common/README.md). This runner
shows it on the release's own kernel and file system:

- the guest's SysRq mask is 0, as the launcher leaves it: ``sysrq s`` is
  refused with ``sysrq s disabled``;
- a file of a byte the data disk does not otherwise hold is written under
  /data/local/tmp (unencrypted), and at once, before the guest's own
  write-back runs, it is dirty in the guest's page cache but not in the
  host's image (the control: a request that has not been made reaches
  nothing; a file the flusher got to first is written again);
- ``sync`` replies with a success, and the image on the host then holds the
  file;
- the host's quiesce requests follow in its order (``sync``, ``trim once``,
  ``suspend``, ``wake``), the guest answers adb again and ``sync`` replies
  once more;
- a suspend asked with no sync before it states in its reply whether it synced
  (``suspended, synced``, the PM core's sync having returned, or ``suspended,
  not synced``, as the guest's sync_on_suspend says), and the image holds a
  fresh file right after a reply that says synced (the byte count shows that
  the file had reached the image by the reply, not that the suspend's own sync
  wrote it); ``--suspend-statement none`` expects a kernel without the
  statement, whose reply is ``suspended``;
- the emulator is killed outright, the way iOS ends a process that never runs
  again, and the image still holds it.

``--request`` names another command for the same run: a kernel without ``sync``
answers ``Unknown command`` and ``sysrq s`` is refused with SysRq off, so the
file is not in the image and the run fails saying so. Every wait has a
deadline, and the guest is stopped through its own process on any exit.

The runner is opt-in: it needs the macos-android17 executable, an installed
release, and an adb client. It boots like run_android_video_suspend.py, whose
start-up it uses.
usage: run_android_sync.py --emulator uml-darwin-android17 [--release DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]

SPEC = importlib.util.spec_from_file_location(
    "run_android_video_suspend", HERE / "run_android_video_suspend.py")
assert SPEC is not None and SPEC.loader is not None
base = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = base
SPEC.loader.exec_module(base)
boot_android = base.boot_android
Failure = base.Failure

PAYLOAD_BYTES = 8 << 20
# One byte per attempt: a retry must not find the earlier file's blocks, which
# the guest may reuse, already holding what it counts.
PAYLOAD_BYTES_BY_ATTEMPT = (0xA5, 0xB6, 0xC7)
# The same for the file the suspend alone must write back.
SUSPEND_PAYLOAD_BYTES_BY_ATTEMPT = (0xD8, 0xE9, 0xFA)
PAYLOAD = "/data/local/tmp/sync-payload"
# The suspend's own file: the first one must stay for the check after the kill.
SUSPEND_PAYLOAD = "/data/local/tmp/suspend-payload"
# The release is a user build, where the shell is no root: the write-back
# cannot be held off, so the control looks at the image at once.
ATTEMPTS = len(PAYLOAD_BYTES_BY_ATTEMPT)
# A few of the image's bytes before the file may be the payload's byte by
# chance, and the file's blocks overwrite them.
SLACK = 4096
SYNC_REPLY = re.compile(r"synced in \d+ ms")
CHUNK = 64 << 20


def write_command(byte: int, path: str = PAYLOAD) -> str:
    return f"head -c {PAYLOAD_BYTES} /dev/zero | tr '\\000' '\\{byte:03o}' > {path}"


def image_payload(image: pathlib.Path, byte: int) -> int:
    """How many bytes of the image on the host are `byte`."""
    count = 0
    marker = bytes([byte])
    with open(image, "rb") as handle:
        while chunk := handle.read(CHUNK):
            count += chunk.count(marker)
    return count


def background_edge(guest: "base.Guest") -> None:
    """The requests of the host's quiesce, in its order: sync, a trim, the
    suspend (asked again while the guest answers busy) and the wake. The guest
    answers adb again and writes back again afterwards."""
    for command in ("sync", "trim once"):
        guest.console.request(command, 60)
    guest.console.suspend()
    guest.console.request("wake", 60)
    base.wait_for("the guest after the wake", 90,
                  lambda: None if guest.shell("echo alive", timeout=30).strip() == "alive" else "no answer", guest)
    text = guest.console.request("sync", 60)
    if not SYNC_REPLY.fullmatch(text):
        raise Failure(f"sync after the wake replied {text!r}, not 'synced in N ms'")


def suspend_states_its_sync(guest: "base.Guest", image: pathlib.Path, statement: str) -> None:
    """A suspend with no sync before it: its reply says whether the suspend
    synced (as the guest's sync_on_suspend says, when it can be read), and a
    reply that says it did has the fresh file in the image right after it.

    What the byte count proves is only that the file had reached the image by
    the reply, which a reply that says synced requires. It does not show that
    the suspend's own sync wrote it (the guest's flusher would show the same),
    and a reply that says not synced is not checked against the image at all:
    the file may already be there. The Linux runner
    (run_guest_mconsole_console_clients.py) holds write-back off and checks
    both directions, and a suspend the guest started itself."""
    for byte in SUSPEND_PAYLOAD_BYTES_BY_ATTEMPT:
        before_write = image_payload(image, byte)
        guest.shell(f"rm -f {SUSPEND_PAYLOAD}; {write_command(byte, SUSPEND_PAYLOAD)}")
        control = image_payload(image, byte) - before_write
        if control < PAYLOAD_BYTES // 4:
            break
    else:
        raise Failure("the image held the suspend's payload before any request in each attempt")
    text = guest.console.suspend()
    held = image_payload(image, byte) - before_write
    guest.console.request("wake", 60)
    base.wait_for("the guest after the wake", 90,
                  lambda: None if guest.shell("echo alive", timeout=30).strip() == "alive" else "no answer", guest)
    if statement == "none":
        if text != "suspended":
            raise Failure(f"the suspend replied {text!r}; a kernel without the statement replies 'suspended'")
        print(f"suspend replied {text!r}: no statement, as expected of a kernel without it "
              f"(the image held {held} payload bytes right after it, of {PAYLOAD_BYTES})")
        return
    # The shell is no root and may not read it: then either statement stands.
    flag = guest.shell("cat /sys/power/sync_on_suspend 2>/dev/null; true").strip()
    expected = {"1": "suspended, synced", "0": "suspended, not synced"}.get(flag)
    if text not in ("suspended, synced", "suspended, not synced"):
        raise Failure(f"the suspend replied {text!r}, which states nothing of its sync")
    if expected is not None and text != expected:
        raise Failure(f"the suspend replied {text!r} with sync_on_suspend {flag}: expected {expected!r}")
    if text == "suspended, synced" and held < PAYLOAD_BYTES - SLACK:
        raise Failure(f"the suspend replied {text!r} and the image held {held} of the payload's "
                      f"{PAYLOAD_BYTES} bytes right after it (the control held {control}): "
                      "the statement was not true")
    print(f"suspend replied {text!r} (sync_on_suspend {flag or 'unreadable'}); the image held {held} payload "
          f"bytes right after it, of {PAYLOAD_BYTES}, the control {control}")


def run_checks(guest: "base.Guest", image: pathlib.Path, request: str, statement: str) -> None:
    error, text = guest.console.exchange("sysrq s", 60)
    if not error or text != "sysrq s disabled":
        raise Failure(f"'sysrq s' ended with {'an error' if error else 'a success'} {text!r}: the guest's "
                      "SysRq is on, and the run proves nothing about a guest that needs no SysRq")
    # The write-back cannot be held off, so a file that the guest's flusher
    # wrote before the control looked is written again, as another file.
    for byte in PAYLOAD_BYTES_BY_ATTEMPT:
        # What the guest wrote while booting is not the payload's byte.
        before_write = image_payload(image, byte)
        guest.shell(f"rm -f {PAYLOAD}; {write_command(byte)}")
        control = image_payload(image, byte)
        if control - before_write < PAYLOAD_BYTES // 4:
            break
    else:
        raise Failure(f"the image held the payload before any request in each of {ATTEMPTS} attempts: the "
                      "data disk's file system does not hold the file back, and the check below proves nothing")
    dirty = re.search(r"Dirty:\s+(\d+) kB", guest.shell("grep ^Dirty /proc/meminfo"))
    if dirty is None or int(dirty[1]) < PAYLOAD_BYTES // 1024 // 4:
        raise Failure(f"the payload is not dirty in the guest: {dirty and dirty[0]!r}")
    error, text = guest.console.exchange(request, 60)
    after = image_payload(image, byte)
    if error:
        raise Failure(f"{request!r} was refused with {text!r}; the image holds {after - before_write} of the "
                      f"payload's {PAYLOAD_BYTES} bytes (the control held {control - before_write})")
    if request == "sync" and not SYNC_REPLY.fullmatch(text):
        raise Failure(f"sync replied {text!r}, not 'synced in N ms'")
    if after - before_write < PAYLOAD_BYTES - SLACK:
        raise Failure(f"{request!r} replied {text!r} and the image holds {after - before_write} of the "
                      f"payload's {PAYLOAD_BYTES} bytes (the control held {control - before_write})")
    background_edge(guest)
    suspend_states_its_sync(guest, image, statement)
    guest.process.kill()
    guest.process.wait(timeout=30)
    killed = image_payload(image, byte)
    if killed - before_write < PAYLOAD_BYTES - SLACK:
        raise Failure(f"after a hard kill the image holds {killed - before_write} payload bytes of "
                      f"{PAYLOAD_BYTES}")
    print(f"{request!r} replied {text!r}; the image held {max(control - before_write, 0)} payload bytes before "
          f"it, {after - before_write} after it and {killed - before_write} after a hard kill "
          f"(of {PAYLOAD_BYTES})")


def main() -> int:
    parser = base.build_parser()
    parser.description = __doc__
    parser.add_argument("--request", default="sync",
                        help="the command that must reach the image (default sync)")
    parser.add_argument("--suspend-statement", choices=("stated", "none"), default="stated",
                        help="whether the kernel states in its suspend reply whether the suspend synced "
                        "(default), or is one without the statement, which replies 'suspended'")
    options = parser.parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-asy-"))
    instance = options.instance or private / "instance"
    guest: "base.Guest | None" = None
    try:
        guest = base.start_guest(options, str(adb), private)
        run_checks(guest, instance / boot_android.android_instance.DATA_DISK, options.request,
                   options.suspend_statement)
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        log = private / "emulator.log"
        if log.is_file():
            print("launcher log tail:\n" + "\n".join(log.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            boot_android.disconnect_adb(guest.adb, guest.serial)
            boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
