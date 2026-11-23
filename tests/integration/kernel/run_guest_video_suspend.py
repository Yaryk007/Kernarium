#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Virtio video decoder and encoder survive a guest system suspend.

A host that leaves the foreground suspends its guest; Android suspends itself
once its display is off. Either way the guest's virtio-uml disables the rings
of every device, and a ring disable used to reset a device that still held a
request. The video devices hold requests across a suspend by design (a codec
stream's queued capture or coded buffers wait for the next picture), and they
parked their event buffers in the device too, so every suspend reset them:
streams were closed under their clients, and a decode session never saw its
source change again. The event queues now leave their buffers in the ring, and
the transport keeps what the device holds across the ring disable.

Each arm boots a two-vCPU guest with the decoder and the encoder and suspends
it by one route, the one the host's quiesce takes (`suspend` and `wake` on the
management console) or the one the guest takes by itself (`echo mem` into
/sys/power/state, ended by a `wake`). Around the suspend it runs:

- a decode and an encode session before it, to show the probe works;
- one decode and one encode client that opened its node before the suspend
  and starts after the wake, as a VA-API driver's pool of nodes does: the
  decoder still announces the picture size and delivers a picture, the
  encoder still returns coded data;
- a new decode and encode session after the wake: the decoder's source change
  arrives and a picture follows, and the encoder returns coded data;
- one decode and one encode stream in progress across a second suspend, each
  holding its queued buffers with the device: the next picture or coded
  buffer arrives after the wake, the queues stop and the node closes;
- a third suspend after those streams ended, which the guest must enter.

Every session stops its queues and closes its node, which returns only when
the device handed back every buffer. The host's activity log must show no
stream closed by a reset while the clients waited, and the guest must print no
kernel warning, oops or failure to freeze. Every phase is reported, and the
arm fails if any phase did. The contract is the suspend section of the Virtio
video README.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
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
# The directory of this script is first on sys.path.
from run_guest_suspend_holds import (  # noqa: E402
    ManagementConsole, SELF_SUSPEND, SUSPEND_ENTRY, SUSPEND_EXIT, drain,
    hold_quietly, wait_for_exit_line, wait_for_line)


VCPUS = 2
BOOT_SECONDS = 180
SESSION_SECONDS = 120
READY_SECONDS = 60
RESULT_SECONDS = 90
HOLD_SECONDS = 3.0
PROBE = "/opt/uml-test/video_suspend_probe.py"
PACKET = "/opt/uml-test/h264-main.bin"
FLAG = "/tmp/video-suspend-go"
STREAM_FLAG = "/tmp/video-suspend-go-stream"
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|refusing to freeze|"
    r"Freezing user space processes failed|blocked for more than"
)
RESULT = re.compile(r"^VSUSP (\S+) (ok|FAIL: .*?)\r?$", re.M)
LINE = re.compile(r"^VSUSP .*?$", re.M)
RESET_CLOSE = re.compile(r"^uml-video .*close=reset.*$", re.M)
ARMS = ("host", "self")


def log_path(label: str) -> str:
    return f"/tmp/video-suspend-{label}.log"


def result_of(text: str, label: str) -> str | None:
    """`ok` or the failure line the probe printed for ``label``, if any."""
    for name, verdict in RESULT.findall(text):
        if name == label:
            return verdict
    return None


def session_command(label: str, kind: str, flag: str | None = None) -> str:
    """The probe's command line: ``kind`` is decode, encode, decode-stream or
    encode-stream, and ``flag`` the file a held session waits for."""
    words = ["python3", "-B", PROBE, label, kind]
    if kind.startswith("decode"):
        words += [PACKET, "H264"]
    if flag:
        words.append(flag)
    return " ".join(words)


def run_session(guest: Guest, label: str, kind: str, problems: list[str]) -> None:
    """One session in the foreground."""
    result = guest.run_command(
        f"{session_command(label, kind)} >{log_path(label)} 2>&1; "
        f"cat {log_path(label)}",
        SESSION_SECONDS,
    )
    report(label, result.output, problems)


def report(label: str, output: str, problems: list[str]) -> None:
    for line in LINE.findall(output):
        print(line.rstrip("\r"))
    verdict = result_of(output, label)
    if verdict is None:
        problems.append(f"{label}: the probe reported nothing")
    elif verdict != "ok":
        problems.append(f"{label}: {verdict.removeprefix('FAIL: ')}")


def wait_for_log(guest: Guest, label: str, marker: str | None, seconds: float) -> str | None:
    """The probe's log once it holds ``marker`` or a verdict."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        text = guest.run_command(f"cat {log_path(label)}", 30).output
        if (marker and marker in text) or result_of(text, label) is not None:
            return text
        drain(guest, 0.5)
    return None


def suspend_by_host(guest: Guest, console: ManagementConsole) -> None:
    """The route the host's quiesce takes."""
    exits = guest.output().count(SUSPEND_EXIT)
    answer = console.request("suspend")
    if "suspended" not in answer:
        raise RuntimeError(f"host suspend: the guest replied {answer!r}")
    hold_quietly(guest, exits, HOLD_SECONDS, "host suspend")
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"host suspend: wake replied {answer!r}")
    wait_for_exit_line(guest, exits, "host suspend: the guest did not resume")


def suspend_by_guest(guest: Guest, console: ManagementConsole) -> None:
    """A suspend the guest enters by itself, as Android does with its display
    off, ended by the wake a power key sends."""
    exits = guest.output().count(SUSPEND_EXIT)
    entries = guest.output().count(SUSPEND_ENTRY)
    guest._write(SELF_SUSPEND)
    wait_for_line(guest, SUSPEND_ENTRY, entries, "guest suspend")
    hold_quietly(guest, exits, HOLD_SECONDS, "guest suspend")
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"guest suspend: wake replied {answer!r}")
    wait_for_exit_line(guest, exits, "guest suspend: the guest did not resume")


def suspend_guest(suspend, guest: Guest, console: ManagementConsole, what: str,
                  problems: list[str]) -> bool:
    """One suspend and wake by ``suspend``'s route. A guest that does not
    enter it is reported, and the arm ends: it is wedged."""
    try:
        suspend(guest, console)
    except RuntimeError as error:
        problems.append(f"{what}: {error}")
        return False
    return True


def start_held(guest: Guest, clients: dict[str, str], flag: str, arm: str, suffix: str) -> None:
    """Starts one client per kind, each of which prints `ready` and waits for
    ``flag``. ``suffix`` is the probe kind's ending: "" for a node opened
    ahead of use, "-stream" for a stream in progress."""
    guest.check_command(f"rm -f {flag}", 30)
    for kind, label in clients.items():
        guest.check_command(
            f"({session_command(label, kind + suffix, flag)} "
            f">{log_path(label)} 2>&1 </dev/null &)",
            30,
        )
    for kind, label in clients.items():
        if wait_for_log(guest, label, "ready", READY_SECONDS) is None:
            raise RuntimeError(f"{arm}: the {kind} client never became ready")


def release_held(guest: Guest, clients: dict[str, str], flag: str, problems: list[str]) -> None:
    guest.check_command(f"touch {flag}", 30)
    for label in clients.values():
        text = wait_for_log(guest, label, None, RESULT_SECONDS)
        if text is None:
            problems.append(f"{label}: no result in {RESULT_SECONDS} s")
            continue
        report(label, text, problems)


def wait_for_devices(guest: Guest) -> None:
    guest.check_command(
        "for i in $(seq 1 100); do "
        "[ $(ls /sys/class/video4linux | wc -l) -ge 2 ] && break; "
        "sleep 0.1; done; ls /sys/class/video4linux",
        30,
    )


def run_arm(arguments, private: pathlib.Path, arm: str) -> list[str]:
    activity = private / f"video-{arm}.log"
    environment = dict(os.environ)
    environment["UML_VIDEO_ACTIVITY_LOG"] = str(activity)
    payloads = {
        PROBE: GuestFile(
            (REPOSITORY / "tests/integration/kernel/video_suspend_probe.py")
            .read_bytes(),
            0o755,
        ),
        PACKET: GuestFile(
            (REPOSITORY / "tests/support/video/h264-main.bin").read_bytes()
        ),
    }
    problems: list[str] = []
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        ("--video", "--video-encoder", f"ncpus={VCPUS}"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads=payloads,
        environment=environment,
    ) as guest:
        guest.boot_to_shell(BOOT_SECONDS)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        wait_for_devices(guest)
        run_session(guest, f"{arm}-decode-before", "decode", problems)
        run_session(guest, f"{arm}-encode-before", "encode", problems)
        if problems:
            # The probe itself does not work; the suspend adds nothing.
            return problems

        suspend = suspend_by_host if arm == "host" else suspend_by_guest

        # Two clients open their nodes now and start after the wake.
        opened = {"decode": f"{arm}-decode-held", "encode": f"{arm}-encode-held"}
        start_held(guest, opened, FLAG, arm, "")
        if not suspend_guest(suspend, guest, console, f"{arm}: suspend 1", problems):
            return problems
        release_held(guest, opened, FLAG, problems)

        run_session(guest, f"{arm}-decode-after", "decode", problems)
        run_session(guest, f"{arm}-encode-after", "encode", problems)

        # Two streams in progress, their buffers queued with the device.
        streams = {"decode": f"{arm}-decode-stream", "encode": f"{arm}-encode-stream"}
        start_held(guest, streams, STREAM_FLAG, arm, "-stream")
        if not suspend_guest(suspend, guest, console, f"{arm}: suspend 2", problems):
            return problems
        release_held(guest, streams, STREAM_FLAG, problems)

        # A stream that never closed would keep the freezer from entering
        # the next suspend.
        if not suspend_guest(suspend, guest, console, f"{arm}: suspend 3", problems):
            return problems
        run_session(guest, f"{arm}-decode-last", "decode", problems)
        run_session(guest, f"{arm}-encode-last", "encode", problems)

        # The streams the clients held stayed open on the host through the
        # suspend: a stream closed by a reset was the device losing them.
        resets = RESET_CLOSE.findall(activity.read_text()) if activity.exists() else []
        if resets:
            problems.append(
                f"{arm}: the host reset the video devices at the suspend:\n"
                + "\n".join(resets)
            )
        failure = FAILURE.search(guest.output()[request:])
        if failure is not None:
            tail = guest.output()[request:]
            problems.append(
                f"{arm}: the guest printed {failure[0]!r}:\n"
                + tail[max(0, failure.start() - 1500):][:5000]
            )
        if not problems:
            guest.check_command("echo alive", 30)
            if not guest.shutdown(60):
                problems.append(f"{arm}: the guest did not power off")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--arm", action="append", choices=ARMS)
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-vsusp-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    problems: list[str] = []
    try:
        for arm in arguments.arm or ARMS:
            problems += run_arm(arguments, private, arm)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    if problems:
        raise RuntimeError("\n".join(problems))
    print("the video devices carried on across a host and a guest suspend")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
