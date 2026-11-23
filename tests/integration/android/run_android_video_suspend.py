#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the Virtio video devices of an Android release across a guest suspend.

The decoder and the encoder hold the buffers a stream has queued, and a suspend
disables the guest's rings while they do. The devices and the transport keep
the stream through the disable (docs/architecture.md, "Devices and graphics");
this runner shows it with the apps that use the devices:

- the gallery (Glimpse, Media3 over c2.v4l2.avc.decoder) plays a clip and
  ``screenrecord`` (the hardware encoder) records the screen while the guest
  suspends, by the route the host's quiesce takes (``suspend`` and ``wake`` on
  the management console, the suspend asked again while the guest answers
  busy, as the host's client does) and by the route Android takes itself with
  its screen off and adb disconnected, ended by a ``wake``;
- the launcher's log from before each suspend to the end of the check that
  follows it (output can arrive after the wake) must show no GPU error: the
  host's own reset line fails the step as a host GPU reset across the
  suspend, the known host defect that leaves the guest's graphics gone; the
  guest kernel's refused commands or the host's refusal of an unknown context
  without it fail as GPU errors, whose cause is not established;
- after the wake the picture is moving again, no thread of the guest stays in
  uninterruptible sleep, ``screenrecord`` ends with status 0 and a file, and
  Back closes the player, which returns only if the device handed every
  buffer back;
- the host's video activity log shows one decoder stream for the player and one
  encoder stream for the recorder, each open until its app ends, closed by a
  destroy with no buffer pending, and none closed by a reset.

The clip is made in the guest: ``screenrecord`` films the settings list being
scrolled, so the host needs no encoder tool. Every wait has a deadline. The
guest's launcher is stopped through its own process on any exit, and the guest
is reached through the shared adb server, always named with ``-s``.

The runner is opt-in: it needs the macos-android17 executable, an installed
release, and an adb client.
usage: run_android_video_suspend.py --emulator uml-darwin-android17 [--release DIR] [--instance DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import functools
import hashlib
import importlib.util
import os
import pathlib
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import gpu_suspend_log  # noqa: E402
import slot_ports  # noqa: E402

SPEC = importlib.util.spec_from_file_location("boot_android", REPOSITORY / "tools" / "boot_android.py")
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)

DEFAULT_ADB_PORT = 5583
COMMAND_SECONDS = 120
SETTLE_SECONDS = 45
CLIP_SECONDS = 120
RECORD_SECONDS = 30
HOLD_SECONDS = 4.0
RESUME_SECONDS = 90
# The host's own suspend (darwin/device/mconsole/guest_console.cpp,
# suspend_guest()): a guest that answers "Suspend failed: -16" because a wake
# source of its own, an Android alarm due within two seconds, ended the attempt
# is asked again at this interval, for this long after the first refusal, and
# the whole suspend has one deadline from the first send.
SUSPEND_BUSY = "Suspend failed: -16"
SUSPEND_RETRY_INTERVAL = 0.25
SUSPEND_RETRY_WINDOW = 2.0
SUSPEND_RETRY_GUARD = 1.0
SUSPEND_DEADLINE = 10.0
MOVING_SECONDS = 60
CLOSE_SECONDS = 30
SLEEP_SECONDS = 20
CLIP = "/sdcard/Movies/clip.mp4"
RECORDING = "/sdcard/Movies/rec.mp4"
RECORDING_STATUS = "/sdcard/rec.out"
SCROLL_MARK = "swipe 360"
SCROLL = (f"while :; do input {SCROLL_MARK} 1000 360 300 1500; input {SCROLL_MARK} 300 360 1000 1500; done")
SUSPEND_ENTRY = "PM: suspend entry"
CLOSE = re.compile(r"^uml-video \S+ (encoder )?stream=(\d+) close=(\w+)(.*)$", re.M)
THREAD_IN_D = re.compile(r"^\s*\d+\s+\d+\s+D\S*\s.*$", re.M)
MAGIC, VERSION, DATA = 0xCAFEBABE, 2, 512


class Failure(RuntimeError):
    pass


def wait_for(description: str, seconds: float, check: Callable[[], str | None],
             guest: "Guest | None" = None) -> None:
    """Calls `check` until it returns None (done); its last message is the failure."""
    deadline = time.monotonic() + seconds
    message: str | None = "not checked"
    while time.monotonic() < deadline:
        if guest is not None:
            guest.alive()
        message = check()
        if message is None:
            return
        time.sleep(1.0)
    raise Failure(f"{description}: {message} (after {seconds:.0f} s)")


class Console:
    """The guest's management console: one datagram request, one reply."""

    def __init__(self, directory: pathlib.Path, reply_directory: pathlib.Path) -> None:
        self.directory = directory
        self.reply = reply_directory / "mc"

    def exchange(self, command: str, seconds: float) -> tuple[int, str]:
        """One request on a fresh socket: the reply's error flag and text."""
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self.reply.unlink(missing_ok=True)
            endpoint.bind(str(self.reply))
            data = command.encode()
            endpoint.sendto(struct.pack("<III", MAGIC, VERSION, len(data)) + data.ljust(DATA, b"\0"),
                            str(self.directory / "mconsole"))
            endpoint.settimeout(seconds)
            try:
                answer = endpoint.recv(12 + DATA)
            except socket.timeout:
                raise Failure(f"mconsole {command}: no reply in {seconds:.0f} s") from None
            error, _, length = struct.unpack_from("<III", answer)
            return error, answer[12:12 + length].decode(errors="replace").rstrip("\0")
        finally:
            endpoint.close()
            self.reply.unlink(missing_ok=True)

    def request(self, command: str, seconds: float = 90) -> str:
        error, text = self.exchange(command, seconds)
        if error:
            raise Failure(f"mconsole {command} failed: {text!r}")
        return text

    def suspend(self) -> str:
        """The host's suspend as the product sends it: a busy refusal is asked again."""
        deadline = time.monotonic() + SUSPEND_DEADLINE
        retry_deadline: float | None = None
        asks = 0
        while True:
            asks += 1
            error, text = self.exchange("suspend", deadline - time.monotonic())
            if not error or not text.startswith(SUSPEND_BUSY):
                break
            if retry_deadline is None:
                retry_deadline = time.monotonic() + SUSPEND_RETRY_WINDOW
            time.sleep(SUSPEND_RETRY_INTERVAL)
            now = time.monotonic()
            if now >= retry_deadline or deadline - now <= SUSPEND_RETRY_GUARD:
                break
        if error:
            raise Failure(f"mconsole suspend failed after {asks} ask(s): {text!r}")
        print(f"host suspend answered by ask {asks}")
        return text


class Guest:
    def __init__(self, adb: str, serial: str, process: subprocess.Popen, log: pathlib.Path,
                 activity: pathlib.Path, console: Console) -> None:
        self.adb = adb
        self.serial = serial
        self.process = process
        self.log = log
        self.activity = activity
        self.console = console

    def adb_run(self, *arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, "-s", self.serial, *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False, timeout=timeout)

    def shell(self, command: str, timeout: float = COMMAND_SECONDS) -> str:
        result = self.adb_run("shell", command, timeout=timeout)
        text = result.stdout.decode(errors="replace")
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {text.strip()[-500:]}")
        return text

    def alive(self) -> None:
        if self.process.poll() is not None:
            raise Failure(f"the guest exited with status {self.process.returncode}:\n"
                          + self.log.read_text(errors="replace")[-3000:])

    def screen(self) -> str:
        result = self.adb_run("exec-out", "screencap", "-p", timeout=60)
        return hashlib.md5(result.stdout).hexdigest()

    def moving(self) -> bool:
        """Whether the screen changed in at least two of three pairs of captures two seconds apart."""
        changed = 0
        for _ in range(3):
            first = self.screen()
            time.sleep(2)
            changed += first != self.screen()
        return changed >= 2

    def threads_in_d(self) -> list[str]:
        return [line.strip() for line in THREAD_IN_D.findall(self.shell("ps -A -T -o PID,TID,STAT,WCHAN,NAME"))]

    def diagnose(self) -> str:
        """What a failed check leaves to classify it by: the host's video log, the focus and the codec lines."""
        parts = ["video activity log tail:\n"
                 + (self.activity.read_text(errors="replace")[-2000:] if self.activity.is_file() else "(none)")]
        for title, command in (
                ("threads in uninterruptible sleep", "ps -A -T -o PID,TID,STAT,WCHAN,NAME | grep ' D' | tail -20"),
                ("focus", "dumpsys window | grep -E 'mCurrentFocus|mFocusedApp' | tail -4"),
                ("codec log", "logcat -d -t 600 | grep -E 'MediaCodec|ExoPlayer|CCodec|v4l2|FATAL|ANR' | tail -40")):
            try:
                parts.append(f"{title}:\n{self.shell(command, timeout=30).strip()}")
            except (Failure, subprocess.TimeoutExpired, OSError) as problem:
                parts.append(f"{title}: unavailable ({problem})")
        return "\n".join(parts)

    def suspend_entries(self) -> int:
        return self.log.read_text(errors="replace").count(SUSPEND_ENTRY)

    def closes(self) -> list[tuple[bool, str, str]]:
        """Each stream the host closed: whether an encoder's, how, and the rest of the line."""
        if not self.activity.is_file():
            return []
        return [(bool(encoder), kind, rest)
                for encoder, _, kind, rest in CLOSE.findall(self.activity.read_text(errors="replace"))]


# ---- The suspend, by each route.

def suspend_by_host(guest: Guest) -> None:
    """The route the host's quiesce takes."""
    answer = guest.console.suspend()
    if "suspended" not in answer:
        raise Failure(f"host suspend: the guest replied {answer!r}")
    time.sleep(HOLD_SECONDS)
    answer = guest.console.request("wake")
    if "running" not in answer:
        raise Failure(f"host suspend: wake replied {answer!r}")


def suspend_by_guest(guest: Guest) -> None:
    """The route Android takes with its screen off and nothing connected over adb,
    ended by the wake a power key sends."""
    entries = guest.suspend_entries()
    guest.shell("input keyevent KEYCODE_SLEEP")
    boot_android.disconnect_adb(guest.adb, guest.serial)
    wait_for("the guest enters its own suspend", RESUME_SECONDS,
             lambda: None if guest.suspend_entries() > entries else "no suspend entry", guest)
    time.sleep(HOLD_SECONDS)
    answer = guest.console.request("wake")
    if "running" not in answer:
        raise Failure(f"guest suspend: wake replied {answer!r}")
    subprocess.run([guest.adb, "connect", guest.serial], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=30, check=False)
    boot_android.wait_for_adb(guest.adb, guest.serial, guest.process, RESUME_SECONDS)
    guest.shell("input keyevent KEYCODE_WAKEUP")
    guest.shell("wm dismiss-keyguard")


ROUTES = {"host": suspend_by_host, "guest": suspend_by_guest}


def pending_of(guest: Guest) -> "gpu_suspend_log.Pending":
    """The mark of the guest's last suspend, pending until the check that suspended it settles it."""
    pending = guest.__dict__.get("pending_suspend")
    if pending is None:
        pending = guest.__dict__["pending_suspend"] = gpu_suspend_log.Pending(
            lambda: gpu_suspend_log.log_size(guest.log), lambda offset: gpu_suspend_log.log_since(guest.log, offset))
    return pending


def suspend(guest: Guest, route: str) -> None:
    """One suspend by `route`, and the proof that the guest answers after it. The launcher's log is marked before
    it and the mark stays pending through the checks that follow the wake (`settles` ends it): output can arrive
    after this returns, when the first graphics work reaches a missing context. A failure of the whole operation,
    the route and its adb waits included, is replaced by the GPU diagnosis when the log shows one (the guest that
    does not answer is its symptom) and kept otherwise; so is a mark an earlier suspend left pending."""
    pending = pending_of(guest)
    earlier = pending.begin()
    if earlier is not None:
        raise Failure(f"before the {route} suspend: {earlier}")
    try:
        ROUTES[route](guest)
        guest.shell("echo guest-answers")
    except Exception as problem:
        found = pending.settle()
        if found is not None:
            raise Failure(f"after the {route} suspend: {found} (the step had failed with: {problem})") from problem
        raise
    found = pending.peek()
    if found is not None:
        pending.settle()
        raise Failure(f"after the {route} suspend: {found}")


def settles(check: Callable[..., None]) -> Callable[..., None]:
    """A check that suspends the guest: when it ends, passed or failed, the pending mark is scanned and ended, and a
    GPU diagnosis is the failure."""
    @functools.wraps(check)
    def settled(guest: Guest, *arguments: object) -> None:
        try:
            check(guest, *arguments)
        except Exception as problem:
            found = pending_of(guest).settle()
            if found is not None:
                raise Failure(f"{found} (the check had failed with: {problem})") from problem
            raise
        found = pending_of(guest).settle()
        if found is not None:
            raise Failure(found)

    return settled


# ---- What the apps do.

def start_scrolling(guest: Guest) -> None:
    """Something always moving on the screen: the settings list, scrolled up and down."""
    guest.shell("input keyevent KEYCODE_WAKEUP")
    guest.shell("wm dismiss-keyguard")
    guest.shell("am start -a android.settings.SETTINGS")
    time.sleep(3)
    guest.shell(f"setsid sh -c '{SCROLL}' >/dev/null 2>&1 &")


def stop_scrolling(guest: Guest) -> None:
    guest.adb_run("shell", f"pkill -f '{SCROLL_MARK}'")


def make_clip(guest: Guest) -> int:
    """Films the scrolled settings list with the hardware encoder and returns the clip's media id."""
    guest.shell(f"rm -f {CLIP}")
    start_scrolling(guest)
    try:
        guest.shell(f"screenrecord --time-limit {CLIP_SECONDS} {CLIP}", timeout=CLIP_SECONDS + 60)
    finally:
        stop_scrolling(guest)
    guest.shell("content call --uri content://media/ --method scan_volume --arg external_primary")
    found: list[int] = []

    def indexed() -> str | None:
        text = guest.shell("content query --uri content://media/external/video/media "
                           "--projection _id:_display_name --where \"_display_name='clip.mp4'\"")
        match = re.search(r"_id=(\d+)", text)
        if match:
            found.append(int(match[1]))
            return None
        return "the clip is not in the media store"

    wait_for("the clip is indexed", 60, indexed, guest)
    return found[0]


def play(guest: Guest, media_id: int) -> None:
    guest.shell("input keyevent KEYCODE_WAKEUP")
    guest.shell("wm dismiss-keyguard")
    guest.shell("am start -a android.intent.action.VIEW "
                f"-d content://media/external/video/media/{media_id} -t video/mp4")
    wait_for("the player shows a moving picture", MOVING_SECONDS,
             lambda: None if guest.moving() else "the picture is still", guest)


def check_no_thread_in_d(guest: Guest, route: str) -> None:
    """A thread is in uninterruptible sleep for an instant at a flush or a binder
    transaction; one still there after SLEEP_SECONDS is the wedge a device must not cause."""
    def clear() -> str | None:
        stuck = guest.threads_in_d()
        return f"still in uninterruptible sleep: {stuck}" if stuck else None

    wait_for(f"no thread in uninterruptible sleep after the {route} suspend", SLEEP_SECONDS, clear, guest)


@settles
def check_player(guest: Guest, media_id: int, route: str) -> None:
    print(f"player across a {route} suspend")
    opened = len(guest.closes())
    play(guest, media_id)
    suspend(guest, route)
    wait_for("the picture moves after the wake", MOVING_SECONDS,
             lambda: None if guest.moving() else "the picture is still", guest)
    check_no_thread_in_d(guest, route)
    # One decoder stream, open since the player started: a close before Back
    # means the player released its codec and the suspend held no stream.
    early = [close for close in guest.closes()[opened:] if not close[0]]
    if early:
        raise Failure(f"the player's decoder stream closed before Back, so no stream spanned the {route} suspend: {early}")
    guest.shell("input keyevent KEYCODE_BACK")

    def decoder_closed() -> str | None:
        closes = [close for close in guest.closes()[opened:] if not close[0]]
        if not closes:
            return "the player's decoder stream was not closed"
        kind, rest = closes[-1][1:]
        if kind != "destroy" or "pending_at_close=0" not in rest:
            return f"the decoder stream closed as {kind}:{rest}"
        return None

    wait_for(f"the player returns its buffers after the {route} suspend", CLOSE_SECONDS, decoder_closed, guest)


@settles
def check_recording(guest: Guest, route: str) -> None:
    print(f"screenrecord across a {route} suspend")
    opened = len(guest.closes())
    guest.shell(f"rm -f {RECORDING} {RECORDING_STATUS}")
    start_scrolling(guest)
    try:
        guest.shell(f"setsid sh -c 'screenrecord --time-limit {RECORD_SECONDS} {RECORDING} >{RECORDING_STATUS} 2>&1; "
                    f"echo screenrecord-rc=$? >>{RECORDING_STATUS}' >/dev/null 2>&1 &")
        time.sleep(10)
        suspend(guest, route)

        def finished() -> str | None:
            status = guest.shell(f"cat {RECORDING_STATUS}")
            return None if "screenrecord-rc=" in status else "screenrecord has not ended"

        wait_for("screenrecord ends", RECORD_SECONDS + RESUME_SECONDS, finished, guest)
    finally:
        stop_scrolling(guest)
    status = guest.shell(f"cat {RECORDING_STATUS}")
    if "screenrecord-rc=0" not in status or re.search(r"Unable|failed|rror", status.replace("screenrecord-rc=0", "")):
        raise Failure(f"screenrecord after the {route} suspend: {status.strip()[-500:]}")
    size = int(guest.shell(f"stat -c %s {RECORDING}").strip())
    if size < 1 << 20:
        raise Failure(f"the recording is {size} bytes")
    check_no_thread_in_d(guest, route)

    def encoder_closes() -> list[tuple[bool, str, str]]:
        return [close for close in guest.closes()[opened:] if close[0]]

    wait_for(f"the recorder's encoder stream is closed after the {route} suspend", CLOSE_SECONDS,
             lambda: None if encoder_closes() else "no encoder stream was closed", guest)
    # Exactly one stream, so the recording ran on the stream the suspend found.
    closes = encoder_closes()
    if len(closes) != 1 or closes[0][1] != "destroy" or "errors=0" not in closes[0][2]:
        raise Failure(f"the encoder stream did not span the {route} suspend and close cleanly: {closes}")


def run_checks(guest: Guest) -> None:
    media_id = make_clip(guest)
    print(f"clip ready (media id {media_id})")
    for route in ROUTES:
        check_player(guest, media_id, route)
        check_recording(guest, route)
    resets = [close for close in guest.closes() if close[1] == "reset"]
    if resets:
        raise Failure(f"the host closed streams by a reset at a suspend: {resets}")
    guest.alive()
    guest.shell("echo guest-answers")


# ---- Starting and stopping the guest.

def start_guest(options: argparse.Namespace, adb: str, private: pathlib.Path) -> Guest:
    instance = options.instance or private / "instance"
    boot_options = boot_android.parse_arguments([
        *(["--release", str(options.release)] if options.release else []),
        *(["--emulator", str(options.emulator)] if options.emulator else []),
        "--instance", str(instance),
        "--adb-port", str(options.adb_port),
        "--cpus", str(options.cpus),
        "--memory", options.memory,
        "--display", "offscreen",
        "--audio-output", "null",
        "--engine", "gadgets",
    ])
    emulator = boot_android.resolve_executable(boot_options.emulator)
    release = boot_android.release_path(boot_options.release)
    identity = boot_android.android_instance.read_release(release)
    decision = boot_android.plan_start(boot_options, identity, instance)
    boot_android.android_instance.apply(instance, identity, decision)
    command = boot_android.emulator_command(
        boot_options, release, instance / boot_android.android_instance.DATA_DISK, emulator,
        boot_android.data_capacity(boot_options, identity))
    log = private / "emulator.log"
    activity = private / "video.log"
    environment = {**os.environ, "UML_VIDEO_ACTIVITY_LOG": str(activity), "TMPDIR": str(private),
                   "PYTHONDONTWRITEBYTECODE": "1"}
    with log.open("wb") as stream:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stream, stderr=stream,
                                   start_new_session=True, env=environment)
    serial = f"127.0.0.1:{options.adb_port}"
    # The emulator is this function's until it returns the guest: the caller's cleanup knows a guest only
    # from that return, so a failure or an interrupt (a timeout, Ctrl-C) before it stops the emulator here.
    try:
        console = Console(private / f"uml-kern{process.pid}", private)
        guest = Guest(adb, serial, process, log, activity, console)
        boot_android.wait_for_adb(adb, serial, process, options.boot_timeout)
        if not boot_android.wait_for_boot_completed(adb, serial, process, options.boot_timeout):
            raise Failure("Android did not reach sys.boot_completed")
        # A pending update counts as applied from here, as for the launcher; a fresh guest skips its wizard.
        boot_android.android_instance.booted(instance, identity)
        if not options.instance:
            boot_android.provision_guest(adb, serial)
        print(f"booted; settling for {SETTLE_SECONDS} s")
        time.sleep(SETTLE_SECONDS)
    except BaseException:
        boot_android.disconnect_adb(adb, serial)
        boot_android.stop_guest(process)
        raise
    return guest


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=pathlib.Path,
                        help="the macos-android17 uml-darwin-android17 (the launcher's default)")
    parser.add_argument("--release", type=pathlib.Path, help="installed Android release (default: accepted)")
    parser.add_argument("--instance", type=pathlib.Path,
                        help="an existing instance to boot; the default is a fresh one that is removed "
                        "at the end")
    parser.add_argument("--adb", default="adb", help="adb client to use")
    parser.add_argument("--adb-port", type=int, default=slot_ports.slot_port(slot_ports.ADB_OFFSET, DEFAULT_ADB_PORT),
                        help="loopback port the launcher forwards adbd to (default: the guest slot's first port "
                        f"when GUEST_SLOT_PORT_BASE is set, else {DEFAULT_ADB_PORT})")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=900,
                        help="seconds to wait for adbd and for boot_completed")
    return parser


def main() -> int:
    options = build_parser().parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-avs-"))
    guest: Guest | None = None
    try:
        guest = start_guest(options, str(adb), private)
        run_checks(guest)
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        if guest is not None:
            print(guest.diagnose(), file=sys.stderr)
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
