#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A management-console command survives other vCPUs printing.

``sysrq`` and ``stack`` send what the kernel prints while they run to the
client that asked, so the console's write() walks a list of those clients
while any vCPU may print, and end with a final reply that names the command
and says whether it ran: ``sysrq s ok``, or an error, ``sysrq s disabled``
with SysRq off and ``sysrq a failed`` for a key no operation has. The host
asks for ``sysrq s`` before it suspends a guest and flushes its disks. Three
tasks pinned to vCPUs 1 to 3 of a 4-vCPU guest keep writing to /dev/kmsg, so a
vCPU other than the one running the command owns the console whenever the
command ends, and the runner asks for ``sysrq s`` from a fresh client socket,
as the host does, EXCHANGES times or for EXCHANGE_SECONDS. Every exchange must
end with the status ``sysrq s ok``, the guest must run on, and the console
must show no fault, oops or panic. A kernel that walks the list unlocked
faulted at address 0x10 in console_write() within the first thousand
exchanges. A second guest boots with SysRq off, as Android's init leaves it,
and every exchange must end with the error ``sysrq s disabled`` and run
nothing.

``sync`` is the command the host asks for to have the guest write its storage
back, and it needs no SysRq: it runs sync(2) in the guest's process context
and replies ``synced in N ms`` only after the call returned. Two guests boot
with SysRq off and an ext4 disk whose write-back is held off (no periodic
write-back, a day's expiry, a journal commit every hour), write a file of a
byte the rest of the image does not hold, and are killed outright, the way iOS
ends a process that stops running. The one that was not asked to sync must lose
the file (the control, which proves that the setup keeps it dirty and the
image shows a loss); the one that asked ``sync`` first must have it in the
image right after the reply, before anything else is asked (a suspend syncs
only while the guest's sync_on_suspend is set, so only the reply may have put
it there), and its reply must be a success. The same guest then follows the
host's quiesce order (``sync``, ``trim once``, ``suspend``, ``wake``), runs on and
syncs again. ``--request`` names another command
for the second guest: a kernel without ``sync`` answers ``Unknown command``,
and ``sysrq s`` is refused with SysRq off, so the guest loses the file the
same way and the run says so.

A suspend states in its reply whether the suspend's own sync returned:
``suspended, synced`` once the PM core's sync of that suspend has returned
and ``suspended, not synced`` when sync_on_suspend is off, a kernel before the
statement says only ``suspended``, and the host takes the statement as the only
confirmation a suspend gives. One more guest dirties a file the same way and
asks ``suspend`` without a ``sync`` before it, with sync_on_suspend on, wakes
the guest, and does it again with a second file and sync_on_suspend off, in
that one guest. With it on, the reply must say synced and the image must hold
the whole file right after it. With it off, the reply must say not synced and
the image must still lack the second file: the statement and the image agree in
both directions. ``--suspend-statement none`` expects a kernel without the
statement (``suspended``, nothing more) and checks only that.

The statement is the request's own: a suspend that the guest started itself
(``echo mem > /sys/power/state``) and that is past its sync when the host's
``suspend`` arrives takes the request at its entry, and the reply is the bare
``suspended``, because the sync that ran was not the request's. One more guest
dirties 24 MiB, starts such a suspend, and sends the console's ``suspend`` at
growing delays after the guest printed its suspend entry (one attempt for each
of GUEST_STARTED_DELAYS, or ``--guest-started-delays``) until one overlaps the
guest's own suspend. The guest's console says which: the lines of a suspend
carry the guest's clock and the printing task, and the request overlaps when
the console worker's first suspend entry is printed before the guest's own
task disables the secondary CPUs, just before it enters and answers. A request
that comes later finds the guest asleep or the transition over and is answered
by a suspend of its own, which says nothing of the guest's. The reply of the
first overlap must be ``suspended`` (``--guest-started-reply bare``, the
default), and one wake must end the sleep; ``--guest-started-reply synced`` is
the control on a kernel without the request binding, which must show
``suspended, synced``. A run in which no attempt overlaps fails as "not
reproduced": it proves nothing either way. ``--only-guest-started`` runs this
scenario alone.

The contract is the management-console paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import socket
import struct
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import CommandTimeout, Guest  # noqa: E402


CPUS = 4
EXCHANGES = 10000
DISABLED_EXCHANGES = 50
EXCHANGE_SECONDS = 30.0
REPLY_SECONDS = 20.0
DISK_BYTES = 64 << 20
PAYLOAD_BYTES = 4 << 20
# The payload's byte; the rest of a freshly formatted image holds hardly any.
PAYLOAD_BYTE = 0xA5
# Held off until the host asks: no periodic write-back, an expiry of a day,
# a journal commit every hour, and ext4's delayed allocation keeps the data
# dirty in the page cache.
DIRTY_SETUP = (
    "mkfs.ext4 -q -F /dev/ubdb && mkdir -p /mnt/disk && "
    "mount -t ext4 -o commit=3600 /dev/ubdb /mnt/disk && "
    "echo 0 > /proc/sys/vm/dirty_writeback_centisecs && "
    "echo 8640000 > /proc/sys/vm/dirty_expire_centisecs && sync"
)
WRITE_PAYLOAD = (
    f"head -c {PAYLOAD_BYTES} /dev/zero | tr '\\000' '\\{PAYLOAD_BYTE:03o}' "
    "> /mnt/disk/payload && cat /proc/meminfo | grep ^Dirty"
)
# The suspend that follows a suspend with sync_on_suspend off in the same
# guest has a payload of its own, with its own byte, so that what the first
# synced is not counted for the second.
SECOND_PAYLOAD_BYTE = 0x5A
WRITE_SECOND_PAYLOAD = (
    f"head -c {PAYLOAD_BYTES} /dev/zero | tr '\\000' "
    f"'\\{SECOND_PAYLOAD_BYTE:03o}' "
    "> /mnt/disk/payload-second && cat /proc/meminfo | grep ^Dirty"
)
# The guest-started suspend has this much dirty data to sync, so that its
# transition takes long enough for a request to arrive while it runs.
REDIRTY = (
    "dd if=/dev/zero of=/mnt/disk/spin bs=1M count=24 conv=notrunc "
    "2>/dev/null && grep ^Dirty /proc/meminfo"
)
SUSPEND_ENTRY = "PM: suspend entry"
SELF_SUSPEND = (
    "(echo mem > /sys/power/state; echo $? > /tmp/self-suspend-rc) &\n"
)
# Seconds after the guest printed its own suspend entry at which the host's
# suspend is sent, one attempt each.
GUEST_STARTED_DELAYS = (0.0, 0.005, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8)
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
WRITERS = (
    "writers=; for c in 1 2 3; do taskset -c $c sh -c 'while :; do "
    "echo \"console clients $$ 0123456789abcdef0123456789abcdef\" "
    "> /dev/kmsg; done' </dev/null >/dev/null 2>&1 & writers=\"$writers $!\"; "
    "done; echo $writers > /tmp/console-writers"
)
STOP_WRITERS = (
    "kill $(cat /tmp/console-writers); wait $(cat /tmp/console-writers); "
    "echo stopped"
)
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Kernel panic|Darwin UML panic|Kernel mode fault|"
    r"Kernel mode signal"
)


class GuestGone(RuntimeError):
    pass


def exchange(guest: Guest, directory: pathlib.Path, serial: int,
             command: str) -> tuple[bool, str]:
    """One request from a client socket of its own, read to its final
    reply. Returns whether the final reply is an error, and its text."""
    reply = directory / f"mc-clients-{serial}"
    reply.unlink(missing_ok=True)
    endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        endpoint.bind(str(reply))
        data = command.encode()
        try:
            endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(directory / "mconsole"),
            )
        except (FileNotFoundError, ConnectionRefusedError) as error:
            guest.wait_for_exit(10.0)
            raise GuestGone(f"exchange {serial}: the console is gone") from error
        endpoint.setblocking(False)
        deadline = time.monotonic() + REPLY_SECONDS
        while True:
            try:
                answer = endpoint.recv(12 + MCONSOLE_DATA)
            except BlockingIOError:
                if guest.wait_for_exit(0.002):
                    raise GuestGone(f"exchange {serial}: the guest exited")
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"exchange {serial}: no final reply in {REPLY_SECONDS:g} s"
                    )
                continue
            error, more, length = struct.unpack_from("<III", answer)
            if not more:
                # The reply's length counts the NUL that ends its text.
                text = answer[12:12 + min(length, MCONSOLE_DATA)]
                return bool(error), text.rstrip(b"\0").decode(errors="replace")
    finally:
        endpoint.close()
        reply.unlink(missing_ok=True)


def require_status(guest: Guest, directory: pathlib.Path, serial: int,
                   command: str, status: str, refused: bool) -> None:
    """The final reply to `command` is `status`, an error if `refused`."""
    error, text = exchange(guest, directory, serial, command)
    if error != refused or text != status:
        raise RuntimeError(
            f"exchange {serial}: {command!r} ended with "
            f"{'an error' if error else 'a success'} {text!r}, "
            f"not {'an error' if refused else 'a success'} {status!r}"
        )


def check_clean(guest: Guest) -> None:
    output = guest.output()
    failure = FAILURE.search(output)
    if failure is not None:
        raise RuntimeError(
            f"the guest printed {failure[0]!r}:\n"
            + output[max(0, failure.start() - 2000):][:6000]
        )


def gone_report(guest: Guest, gone: GuestGone, done: int) -> RuntimeError:
    """The console around the fault that took the guest away."""
    output = guest.output() + "\n" + guest.host_output()
    failure = FAILURE.search(output)
    start = max(0, failure.start() - 2000) if failure else -6000
    return RuntimeError(
        f"{gone} after {done} exchanges:\n" + output[start:][:6000]
    )


def run(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str,
        private: pathlib.Path) -> None:
    with Guest(emulator, rootfs, "1G", "tmpfs", (f"ncpus={CPUS}",),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command(WRITERS, 30)
        started = time.monotonic()
        done = 0
        try:
            while done < EXCHANGES and time.monotonic() - started < EXCHANGE_SECONDS:
                require_status(guest, directory, done + 1, "sysrq s",
                               "sysrq s ok", False)
                done += 1
            elapsed = time.monotonic() - started
            # A command that prints the kernel's stack ends the same way, and
            # a key without an operation is an error that names it.
            require_status(guest, directory, done + 1, "stack 1",
                           "stack ok", False)
            require_status(guest, directory, done + 2, "sysrq a",
                           "sysrq a failed", True)
        except GuestGone as gone:
            raise gone_report(guest, gone, done) from None
        guest.run_command(STOP_WRITERS, 60)
        check_clean(guest)
        print(f"{done} sysrq s exchanges in {elapsed:.1f} s while three vCPUs "
              "printed, each ending with sysrq s ok")
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")


def run_sysrq_off(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str,
                  private: pathlib.Path) -> None:
    """A guest with SysRq off, as Android's init leaves it, refuses the
    operation and says so, although it prints nothing a client could see."""
    with Guest(emulator, rootfs, "1G", "tmpfs",
               (f"ncpus={CPUS}", "sysctl.kernel.sysrq=0"),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command("cat /proc/sys/kernel/sysrq", 30, expected="0")
        done = 0
        try:
            while done < DISABLED_EXCHANGES:
                require_status(guest, directory, done + 1, "sysrq s",
                               "sysrq s disabled", True)
                done += 1
            # stack is no SysRq operation, and ends as it does with SysRq on.
            require_status(guest, directory, done + 1, "stack 1",
                           "stack ok", False)
        except GuestGone as gone:
            raise gone_report(guest, gone, done) from None
        # The refusal ran nothing: the console shows no sync.
        if "Emergency Sync" in guest.output():
            raise RuntimeError("a refused sysrq s synced the file systems")
        check_clean(guest)
        print(f"{done} sysrq s exchanges with SysRq off, each ending with "
              "sysrq s disabled")
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")


def background_edge(guest: Guest, directory: pathlib.Path, serial: int) -> None:
    """The requests of the host's quiesce, in its order: sync, a trim, the
    suspend (asked again while the guest answers busy) and the wake. The guest
    must answer each, run on, and write back again afterwards."""
    for command in ("sync", "trim once"):
        error, text = exchange(guest, directory, serial, command)
        serial += 1
        if error:
            raise RuntimeError(f"{command!r} ended with an error {text!r}")
    for _ in range(40):
        error, text = exchange(guest, directory, serial, "suspend")
        serial += 1
        if not error or not text.startswith("Suspend failed: -16"):
            break
        time.sleep(0.25)
    if error or "suspended" not in text:
        raise RuntimeError(f"'suspend' ended with {text!r}")
    error, text = exchange(guest, directory, serial, "wake")
    if error:
        raise RuntimeError(f"'wake' ended with an error {text!r}")
    guest.check_command("echo alive", 60, expected="alive")
    require_status_prefix(guest, directory, serial + 1, "sync", "synced in ")
    check_clean(guest)


def require_status_prefix(guest: Guest, directory: pathlib.Path, serial: int,
                          command: str, prefix: str) -> None:
    error, text = exchange(guest, directory, serial, command)
    if error or not text.startswith(prefix):
        raise RuntimeError(f"{command!r} ended with "
                           f"{'an error' if error else 'a success'} {text!r}")


def payload_bytes(image: pathlib.Path, byte: int = PAYLOAD_BYTE) -> int:
    """How many of the image's bytes are `byte`, the payload's."""
    return image.read_bytes().count(bytes([byte]))


def dirty_then_kill(emulator: pathlib.Path, rootfs: pathlib.Path,
                    engine: str, private: pathlib.Path, label: str,
                    request: str | None
                    ) -> tuple[int, tuple[bool, str] | None, int]:
    """Boots a guest with SysRq off and an ext4 disk, dirties a file that
    only the guest's page cache holds, asks `request` if there is one, and
    kills the emulator outright. Returns how many of the image's bytes are the
    payload's byte after the kill, the reply to the request, and how many were
    right after that reply, before the host's quiesce order (a suspend syncs
    only while the guest's sync_on_suspend is set, so only the reply may be
    what put the payload in the image)."""
    image = private / f"disk-{label}.img"
    with open(image, "wb") as handle:
        handle.truncate(DISK_BYTES)
    answer = None
    at_reply = 0
    with Guest(emulator, rootfs, "1G", "tmpfs",
               ("ncpus=2", f"ubd1={image}", "sysctl.kernel.sysrq=0"),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command("cat /proc/sys/kernel/sysrq", 30, expected="0")
        # Recorded, not required: a suspend syncs only while this is set, so
        # only the console's sync confirms the storage.
        print(f"{label}: /sys/power/sync_on_suspend:",
              guest.check_command("cat /sys/power/sync_on_suspend", 30).strip())
        guest.check_command(DIRTY_SETUP, 60)
        output = guest.check_command(WRITE_PAYLOAD, 60)
        dirty = re.search(r"Dirty:\s+(\d+) kB", output)
        if dirty is None or int(dirty[1]) < PAYLOAD_BYTES // 1024 // 2:
            raise RuntimeError(
                f"{label}: the payload is not dirty in the guest: {output!r}"
            )
        # Nothing of it reached the image yet, or the control proves nothing.
        if request is not None:
            try:
                answer = exchange(guest, directory, 1, request)
                at_reply = payload_bytes(image)
                if not answer[0]:
                    background_edge(guest, directory, 2)
            except GuestGone as gone:
                raise gone_report(guest, gone, 0) from None
        guest.terminate()
        if not guest.wait_for_exit(30):
            raise RuntimeError(f"{label}: the emulator survived SIGKILL")
    return payload_bytes(image), answer, at_reply


def suspend_exchange(guest: Guest, directory: pathlib.Path,
                     serial: int) -> tuple[bool, str]:
    """The host's suspend request, asked again while the guest answers busy
    (a wake source fired while it synced), as the host does."""
    for attempt in range(40):
        error, text = exchange(guest, directory, serial + attempt, "suspend")
        if not error or not text.startswith("Suspend failed: -16"):
            break
        time.sleep(0.25)
    return error, text


def check_payload_dirty(guest: Guest, label: str, command: str) -> None:
    """Writes the payload and requires most of it dirty in the guest."""
    output = guest.check_command(command, 60)
    dirty = re.search(r"Dirty:\s+(\d+) kB", output)
    if dirty is None or int(dirty[1]) < PAYLOAD_BYTES // 1024 // 2:
        raise RuntimeError(
            f"{label}: the payload is not dirty in the guest: {output!r}"
        )


def suspend_on_then_off(emulator: pathlib.Path, rootfs: pathlib.Path,
                        engine: str, private: pathlib.Path
                        ) -> list[tuple[str, str, int]]:
    """Boots one guest with an ext4 disk whose write-back is held off. With
    sync_on_suspend on, then off, in that guest, it dirties a file only the
    page cache holds, asks for a suspend with no sync before it, and wakes the
    guest. Returns, for each, the suspend's reply and how many of the image's
    bytes are that file's byte right after the reply."""
    image = private / "disk-suspend.img"
    with open(image, "wb") as handle:
        handle.truncate(DISK_BYTES)
    phases = (("on", True, WRITE_PAYLOAD, PAYLOAD_BYTE),
              ("off", False, WRITE_SECOND_PAYLOAD, SECOND_PAYLOAD_BYTE))
    results = []
    with Guest(emulator, rootfs, "1G", "tmpfs",
               ("ncpus=2", f"ubd1={image}", "sysctl.kernel.sysrq=0"),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command(DIRTY_SETUP, 60)
        serial = 1
        for label, enabled, write, byte in phases:
            label = f"sync_on_suspend {label}"
            guest.check_command(
                f"echo {int(enabled)} > /sys/power/sync_on_suspend", 30)
            guest.check_command("cat /sys/power/sync_on_suspend", 30,
                                expected=str(int(enabled)))
            check_payload_dirty(guest, label, write)
            # Nothing of it reached the image yet, or the reply proves
            # nothing.
            if payload_bytes(image, byte) >= PAYLOAD_BYTES // 4:
                raise RuntimeError(
                    f"{label}: the image holds the payload before any request"
                )
            try:
                error, text = suspend_exchange(guest, directory, serial)
                at_reply = payload_bytes(image, byte)
                if error:
                    raise RuntimeError(f"{label}: 'suspend' ended with an "
                                       f"error {text!r}")
                error, woken = exchange(guest, directory, serial + 100, "wake")
                if error:
                    raise RuntimeError(f"{label}: 'wake' ended with an error "
                                       f"{woken!r}")
            except GuestGone as gone:
                raise gone_report(guest, gone, 0) from None
            serial += 200
            guest.check_command("echo alive", 60, expected="alive")
            check_clean(guest)
            results.append((label, text, at_reply))
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")
    return results


def run_suspend_states_its_sync(emulator: pathlib.Path, rootfs: pathlib.Path,
                                engine: str, private: pathlib.Path,
                                statement: str) -> None:
    """The suspend's reply says whether its sync returned, and the image
    holds the file when it did, in one guest with the setting on and then off.
    What the off run's image shows is only that it did not hold the file by
    the reply: a write-back that is not the suspend's own would show the same
    way, which the reply cannot account for."""
    (on_label, on_text, on_bytes), (off_label, off_text, off_bytes) = \
        suspend_on_then_off(emulator, rootfs, engine, private)
    if statement == "none":
        for label, text in ((on_label, on_text), (off_label, off_text)):
            if text != "suspended":
                raise RuntimeError(
                    f"{label}: the suspend replied {text!r}; a kernel "
                    "without the statement replies 'suspended'"
                )
        print(f"suspend replied {on_text!r} with sync_on_suspend on ({on_bytes} "
              f"payload bytes of {PAYLOAD_BYTES} in the image) and "
              f"{off_text!r} with it off ({off_bytes}): no statement, as "
              "expected of a kernel without it")
        return
    if on_text != "suspended, synced":
        raise RuntimeError(
            f"{on_label}: the suspend replied {on_text!r}, not "
            "'suspended, synced'"
        )
    if on_bytes < PAYLOAD_BYTES:
        raise RuntimeError(
            f"the suspend replied {on_text!r}, and the image held only "
            f"{on_bytes} of the payload's {PAYLOAD_BYTES} bytes right after "
            "the reply: the statement was not true"
        )
    if off_text != "suspended, not synced":
        raise RuntimeError(
            f"{off_label}: the suspend replied {off_text!r}, not "
            "'suspended, not synced' (the setting was on in this guest "
            "before)"
        )
    if off_bytes >= PAYLOAD_BYTES // 4:
        raise RuntimeError(
            f"{off_label}: the image already held {off_bytes} of the "
            f"payload's {PAYLOAD_BYTES} bytes right after a reply that said "
            "'not synced': something wrote the file back before the reply "
            "that the reply does not account for"
        )
    print(f"suspend replied {on_text!r}: the image held {on_bytes} payload "
          f"bytes right after the reply, of {PAYLOAD_BYTES}; then, in the "
          f"same guest with sync_on_suspend off, it replied {off_text!r} and "
          f"the second file's image held {off_bytes}")


def wake_until_alive(guest: Guest, directory: pathlib.Path,
                     serial: int) -> int:
    """Wakes the guest and waits until its shell answers twice, 1.5 s apart,
    and returns how many wakes that took. One ends a suspend. A guest that
    was put to sleep again by a retry of the request, after the suspend that
    answered the request ended, is woken once more; the second answer is there
    because such a suspend syncs and freezes for about a second before it
    stops the shell."""
    for wakes in range(1, 6):
        error, text = exchange(guest, directory, serial + wakes, "wake")
        if error:
            raise RuntimeError(f"'wake' ended with an error {text!r}")
        try:
            for _ in range(2):
                time.sleep(1.5)
                # The kernel's own lines may be mixed into the output.
                answer = guest.run_command("echo alive", 10)
                if answer.return_code != 0 or "alive" not in answer.output:
                    raise RuntimeError(
                        f"the guest answered 'echo alive' with {answer!r}")
            return wakes
        except CommandTimeout:
            continue
    raise RuntimeError("the guest does not answer after five wakes")


PM_LINE = re.compile(r"\[\s*(\d+\.\d+)\]\[\s*T(\d+)\] (.*)")


def request_met_the_transition(log: str) -> bool:
    """Whether the host's suspend was pending when the guest's own suspend
    reached the platform: the first suspend entry of a task other than the
    guest's own (the console's worker, which publishes the request just
    before it) is printed before the guest's own task prints that it disables
    the secondary CPUs, which it does just before it enters and answers. The
    lines carry the guest's clock and the printing task's id. A request that
    arrives after that finds the guest asleep, or the transition over, and is
    answered, if at all, by a suspend of its own."""
    own = None
    entered = worker = disabled = None
    for line in log.splitlines():
        found = PM_LINE.search(line)
        if found is None:
            continue
        when, task, text = float(found[1]), found[2], found[3]
        if text.startswith("PM: suspend entry"):
            if own is None:
                own = task
            elif task != own and worker is None:
                worker = when
        elif task == own and disabled is None and \
                text.startswith("Disabling non-boot CPUs"):
            disabled = when
    return worker is not None and disabled is not None and worker < disabled


def awake_command(guest: Guest, directory: pathlib.Path, serial: int,
                  command: str) -> tuple[str, int]:
    """Runs `command`, and while the guest does not answer wakes it and asks
    again (a kernel that lets the request's retry put the guest to sleep
    again does it at a time of its own). Returns the output and how many
    wakes that took."""
    for wakes in range(5):
        try:
            return guest.check_command(command, 15), wakes
        except CommandTimeout:
            error, text = exchange(guest, directory, serial + wakes, "wake")
            if error:
                raise RuntimeError(f"'wake' ended with an error {text!r}")
    raise RuntimeError(f"the guest does not answer {command!r}")


def run_guest_started_suspend(emulator: pathlib.Path, rootfs: pathlib.Path,
                              engine: str, private: pathlib.Path,
                              expected: str,
                              delays: tuple[float, ...] = GUEST_STARTED_DELAYS
                              ) -> None:
    """A suspend that the guest started itself, past its sync, takes the
    host's request at its entry, and the statement in the reply is not the
    request's: it must be the bare 'suspended' (`expected`, the control on a
    kernel without the request binding is 'suspended, synced'). The host's
    `suspend` is sent at growing delays after the guest printed its own suspend
    entry; the first that overlaps the guest's suspend (the guest's write
    succeeded, and the request was answered, not refused) decides. No overlap
    in any attempt is 'not reproduced', which fails the run."""
    image = private / "disk-guest-started.img"
    with open(image, "wb") as handle:
        handle.truncate(DISK_BYTES)
    outcomes: list[str] = []
    with Guest(emulator, rootfs, "1G", "tmpfs",
               ("ncpus=2", f"ubd1={image}", "sysctl.kernel.sysrq=0"),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command(DIRTY_SETUP, 60)
        guest.check_command("cat /sys/power/sync_on_suspend", 30, expected="1")
        for attempt, delay in enumerate(delays):
            awake_command(guest, directory, 4000 + attempt * 10, REDIRTY)
            awake_command(guest, directory, 5000 + attempt * 10,
                          "rm -f /tmp/self-suspend-rc")
            entries = guest.output().count(SUSPEND_ENTRY)
            begun = len(guest.output())
            guest._write(SELF_SUSPEND)
            deadline = time.monotonic() + 30
            while guest.output().count(SUSPEND_ENTRY) <= entries:
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        "the guest never printed its suspend entry:\n"
                        + guest.output()[-4000:]
                    )
                guest.wait_for_exit(0.002)
            time.sleep(delay)
            try:
                started = time.monotonic()
                error, text = exchange(guest, directory, 1000 + attempt,
                                       "suspend")
                answered = time.monotonic() - started
                wakes = wake_until_alive(guest, directory, 2000 + attempt * 10)
            except GuestGone as gone:
                raise gone_report(guest, gone, attempt) from None
            check_clean(guest)
            status, more = awake_command(
                guest, directory, 3000 + attempt * 10,
                "sleep 1; cat /tmp/self-suspend-rc")
            wakes += more
            status = status.strip().splitlines()[-1] if status.strip() else ""
            if not request_met_the_transition(guest.output()[begun:]):
                outcomes.append(f"{delay:g} s: the request did not reach the "
                                "guest's own suspend before it entered (the "
                                f"reply {text!r}, the guest's write returned "
                                f"{status})")
                continue
            if error or status != "0":
                raise RuntimeError(
                    f"the host's suspend met the guest's own suspend "
                    f"({delay:g} s after its entry) and was answered "
                    f"{'with an error ' if error else ''}{text!r}, the "
                    f"guest's write returned {status!r}: expected the "
                    "guest's suspend to answer it and end with the wake"
                )
            if text != expected:
                raise RuntimeError(
                    f"the host's suspend overlapped the guest's own suspend "
                    f"({delay:g} s after its entry; answered after "
                    f"{answered:.3f} s) and was answered {text!r}, not "
                    f"{expected!r}: "
                    + ("the statement is the guest's own transition's sync, "
                       "not one the request ran"
                       if text.startswith("suspended,") else
                       "a kernel that binds the statement to the request "
                       "does not state it for another suspend's sync"
                       if text == "suspended" else
                       "an unexpected reply")
                )
            if wakes != 1 and expected == "suspended":
                raise RuntimeError(
                    f"one wake did not end the guest's sleep ({wakes} were "
                    "needed): the request's retry put the guest to sleep "
                    "again after the suspend that answered it ended, with "
                    "nobody waiting to wake it"
                )
            for outcome in outcomes:
                print("earlier attempt, " + outcome)
            print(f"the host's suspend overlapped the guest's own suspend "
                  f"{delay:g} s after its entry (attempt {attempt + 1} of "
                  f"{len(delays)}) and was answered {text!r} "
                  f"after {answered:.3f} s, as expected; the guest needed "
                  f"{wakes} wake(s) to run again")
            # Not shut down: a control kernel may put the guest to sleep
            # once more, and nothing here needs the guest's clean end.
            guest.terminate()
            if not guest.wait_for_exit(30):
                raise RuntimeError("the emulator survived SIGKILL")
            return
    raise RuntimeError(
        "not reproduced: no attempt overlapped the guest's own suspend, so "
        "the run proves nothing about the request's statement:\n  "
        + "\n  ".join(outcomes)
    )


def run_sync_reaches_the_image(emulator: pathlib.Path, rootfs: pathlib.Path,
                               engine: str, private: pathlib.Path,
                               request: str) -> None:
    """The control loses the dirty file at a hard kill; the guest that was
    asked to write back first has it in the image."""
    lost, _, _ = dirty_then_kill(emulator, rootfs, engine, private, "control",
                                 None)
    if lost >= PAYLOAD_BYTES // 4:
        raise RuntimeError(
            f"the control reached the image without a request ({lost} "
            f"payload bytes of {PAYLOAD_BYTES}): the setup does not hold the "
            "file back, and the check below proves nothing"
        )
    kept, answer, at_reply = dirty_then_kill(emulator, rootfs, engine,
                                             private, "request", request)
    assert answer is not None
    refused, text = answer
    if refused:
        raise RuntimeError(
            f"{request!r} was refused with {text!r}, and after a hard kill "
            f"the image holds {kept} of the payload's {PAYLOAD_BYTES} bytes "
            f"(the control held {lost}): the guest's writeback was not "
            "confirmed and its data was lost"
        )
    if request == "sync" and not re.fullmatch(r"synced in \d+ ms", text):
        raise RuntimeError(f"sync replied {text!r}, not 'synced in N ms'")
    if request == "sync" and at_reply < PAYLOAD_BYTES:
        raise RuntimeError(
            f"sync replied {text!r}, and the image held only {at_reply} of "
            f"the payload's {PAYLOAD_BYTES} bytes right after the reply "
            f"(the control held {lost}): the reply confirms a writeback that "
            "had not reached the host, and the quiesce order that follows "
            "would have hidden it"
        )
    if kept < PAYLOAD_BYTES:
        raise RuntimeError(
            f"{request!r} replied {text!r}, and after a hard kill the image "
            f"holds {kept} of the payload's {PAYLOAD_BYTES} bytes (the "
            f"control held {lost})"
        )
    print(f"{request!r} replied {text!r}; the image held {at_reply} payload "
          f"bytes right after the reply and {kept} after a hard kill, of "
          f"{PAYLOAD_BYTES}, the control {lost}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--only-sync", action="store_true",
                        help="run only the kill-after-request scenario")
    parser.add_argument("--only-suspend-statement", action="store_true",
                        help="run only the suspend-statement scenarios")
    parser.add_argument("--only-guest-started", action="store_true",
                        help="run only the guest-started suspend scenario")
    parser.add_argument("--guest-started-delays", default=None,
                        help="the seconds after the guest's own suspend entry "
                        "at which the host's suspend is sent, comma "
                        "separated, one attempt each (default "
                        + ",".join(f"{d:g}" for d in GUEST_STARTED_DELAYS) + ")")
    parser.add_argument("--guest-started-reply", choices=("bare", "synced"),
                        default="bare",
                        help="what the host's suspend, overlapping a suspend "
                        "the guest started, is answered: the bare "
                        "'suspended' (default), or, as the control on a "
                        "kernel without the request binding, "
                        "'suspended, synced'")
    parser.add_argument("--suspend-statement", choices=("stated", "none"),
                        default="stated",
                        help="whether the kernel states in its suspend reply "
                        "whether the suspend synced (default), or is one "
                        "without the statement, which replies 'suspended'")
    parser.add_argument("--request", default="sync",
                        help="the command that must reach the image (default "
                        "sync); only sync passes on every kernel that has it")
    arguments = parser.parse_args()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-mcc-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        emulator = arguments.emulator.resolve()
        rootfs = arguments.rootfs.resolve()
        delays = GUEST_STARTED_DELAYS if arguments.guest_started_delays is None \
            else tuple(float(d) for d in arguments.guest_started_delays.split(","))
        only = (arguments.only_sync or arguments.only_suspend_statement
                or arguments.only_guest_started)
        for scenario in () if only else (run, run_sysrq_off):
            scenario(emulator, rootfs, arguments.engine, private)
        if not (arguments.only_suspend_statement
                or arguments.only_guest_started):
            run_sync_reaches_the_image(emulator, rootfs, arguments.engine,
                                       private, arguments.request)
        if not (arguments.only_sync or arguments.only_guest_started):
            run_suspend_states_its_sync(emulator, rootfs, arguments.engine,
                                        private, arguments.suspend_statement)
        if not arguments.only_sync and arguments.suspend_statement == "stated":
            run_guest_started_suspend(
                emulator, rootfs, arguments.engine, private,
                "suspended" if arguments.guest_started_reply == "bare"
                else "suspended, synced", delays)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
