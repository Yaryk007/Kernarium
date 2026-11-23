#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The guest's Wi-Fi network script against a fake Wi-Fi framework.

The real script (android/lineage-24/overlay/tools/kernarium_wifi_network.sh) runs
under `sh` with stub `cmd`, `dumpsys`, `getprop`, `setprop`, `sleep`, `date` and
`log` over a fake framework that has what the Wi-Fi service has: a config store
that the platform reads once (a save before the read is rejected) and writes some
time after a save, a join, a forget or a change of Auto-connect (a pending write
keeps its deadline), a list in memory, a station that joins an enabled network
that has Auto-connect on, a user who forgets it or turns Auto-connect off, a dump
that shows each saved configuration's Auto-connect under a header with its ID
(and that the service can stall part-way), a listing that stalls for four
seconds and prints what an empty one does, and the persistent marker. Each case
boots that framework once or more and states what the script did: the saves it
asked for, the lines it logged, the marker, what the store holds afterwards and
how long it slept. A guest stops when the script is killed right after it wrote
the marker, or a set time after it ended. The stubs keep time by the seconds the
script sleeps and the commands take, so nothing waits. A stub reaches the fake framework over a pair of
FIFOs and runs on shell builtins, so a call starts no interpreter.

The test checks itself: mutants of the script (marker 1 final, marker 2 set by a
boot that saved, a stalled listing read as absent, a network removed during the
boot saved again, a guard of fewer looks, no early end at marker 2, a repair that
ignores Auto-connect, an unreadable dump read as off, another configuration's
Auto-connect counted, a header's SSID text naming the configuration) must each
fail a case.

`test_wifi_network_script.py --script FILE` prints what a script gets wrong.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import concurrent.futures
import dataclasses
import os
import subprocess
import sys
import tempfile
import threading
import traceback
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wifi_dump_fixtures

CLOCK_LIMIT = 600
# What `dumpsys wifi` does: fails, says nothing for twelve seconds, says the caller may not, or prints its first
# lines and then stalls (up to a configuration's header, or through its first Auto-connect line).
DUMP_OK, DUMP_FAILS, DUMP_SLOW, DUMP_DENIED, DUMP_CUT_BEFORE, DUMP_CUT_AFTER, DUMP_LATE = 0, 1, 2, 3, 4, 5, 6
LATE = "--late--"   # in a stub's output: what follows is printed only after the call's whole duration
OTHER_NONE, OTHER_ON, OTHER_OFF = 0, 1, 2                    # another saved network, and its Auto-connect
# The other network's name: an ordinary one, the same name (with another security type), a name that holds the text
# of a header, and a name that ends with a quote.
OTHER_NAMES = ("Elsewhere", "VirtWifi", 'X SSID: "VirtWifi"', 'VirtWifi"')
HEADER = "Network Id      SSID                         Security type"


# ---- The fake framework: what a `cmd wifi` call does.

class Streams:
    """What one call prints."""

    def __init__(self) -> None:
        self.out: list[str] = []
        self.err: list[str] = []

    def say(self, text: str, error: bool = False) -> None:
        (self.err if error else self.out).append(text + "\n")


def load(path: str) -> dict:
    """The framework's state: `name value` lines, a value being None, a boolean or an integer."""
    values = {"None": None, "True": True, "False": False}
    memory = {}
    with open(path) as stream:
        for line in stream:
            name, text = line.split()
            memory[name] = values[text] if text in values else int(text)
    return memory


def save(path: str, memory: dict) -> None:
    with open(path, "w") as stream:
        stream.write("".join(f"{name} {value}\n" for name, value in memory.items()))


def note(state: str, call: str) -> None:
    with open(os.path.join(state, "calls"), "a") as stream:
        stream.write(call + "\n")


def elapsed(state: str) -> int:
    """The seconds the script has slept and its commands have taken (the monotonic clock)."""
    total = 0
    for name in ("sleeps", "extras"):
        with open(os.path.join(state, name)) as stream:
            total += sum(int(seconds) for seconds in stream.read().split())
    return total


def duration(state: str, seconds: int) -> None:
    """How long the call that is running takes: the stub that wraps it passes that time, or `timeout` cuts it."""
    with open(os.path.join(state, "duration"), "w") as stream:
        stream.write(f"{seconds}\n")


def framework(state: str, arguments: list[str], streams: Streams) -> int:
    """One `cmd wifi ...` call against the framework's state; prints what the service prints."""
    path = os.path.join(state, "framework")
    memory = load(path)
    now = elapsed(state)
    advance(memory, now)
    duration(state, 0)
    status = 0
    command = arguments[:2]
    if command == ["wifi", "list-networks"]:
        note(state, "list")
        if memory["failing"] > 0:
            # The Wi-Fi service is not up: `cmd` says so and exits with its own status.
            memory["failing"] -= 1
            streams.say("cmd: Can't find service: wifi", error=True)
            status = 20
        elif memory["stalled"] > 0 or (memory["stall_after"] is not None and now >= memory["stall_after"]):
            # The Wi-Fi thread does not answer: the service gives an empty list after four seconds, status 0. The
            # wall clock steps back while the call waits, when the boot says so.
            memory["stalled"] = max(0, memory["stalled"] - 1)
            duration(state, 4)
            if memory["clock_step_back"]:
                with open(os.path.join(state, "steps"), "a") as stream:
                    stream.write(f"{memory['clock_step_back']}\n")
            advance(memory, now + 4)
            streams.say("No networks")
        else:
            rows = []
            if memory["listed"]:
                rows.append((target_id(memory), "VirtWifi", "open"))
            if memory["other"]:
                rows.append((other_id(memory), OTHER_NAMES[memory["other_name"]], "wpa2"))
            if rows:
                streams.say(HEADER)
                for number, name, security in sorted(rows):
                    streams.say(f"{number:<12} {name:<32} {security}")
            else:
                streams.say("No networks")
            if memory["slow"] > 0:
                # An answer that takes two seconds is still an answer.
                memory["slow"] -= 1
                duration(state, 2)
    elif arguments[:1] == ["dumpsys"] and arguments[-1:] == ["wifi"]:
        status = dump(state, memory, streams)
    elif command == ["wifi", "status"]:
        note(state, "status")
        streams.say("Wifi is enabled" if memory["wifi_on"] else "Wifi is disabled")
        if memory["wifi_on"] and memory["joined"]:
            streams.say('Wifi is connected to "VirtWifi"')
        else:
            streams.say("Wifi is not connected")
    elif command == ["wifi", "add-network"]:
        note(state, "add")
        if memory["add_fails"]:
            status = 1
        elif not memory["read_done"]:
            # The platform rejects a save before it has read its store (WifiConfigManager's pending read).
            pass
        elif memory["add_ignored"] > 0:
            memory["add_ignored"] -= 1
        else:
            if not memory["listed"] or memory["disabled_now"] or not memory["autojoin"]:
                memory["since"] = now
            memory["listed"] = True
            memory["disabled_now"] = False
            # Saving the network again replaces its configuration: Auto-connect is on again.
            memory["autojoin"] = True
            schedule(memory, now)
    else:
        status = 2
    save(path, memory)
    return status


def target_id(memory: dict) -> int:
    """VirtWifi's network ID: the boot's own, else 0, or 1 when the boot gives the other network the first ID."""
    return memory["virtwifi_id"] or (1 if memory["swap_ids"] else 0)


def other_id(memory: dict) -> int:
    return 0 if memory["swap_ids"] else 1


def dump(state: str, memory: dict, streams: Streams) -> int:
    """`dumpsys wifi`: the saved networks' configurations, each printed more than once, as the Wi-Fi service prints
    them, with Auto-connect on a line of its own after a header line that names the configuration's ID and SSID."""
    note(state, "dump")
    mode = memory["dump_state"]
    if mode == DUMP_FAILS:
        streams.say("Can't find service: wifi", error=True)
        return 20
    if mode == DUMP_DENIED:
        streams.say("Permission Denial: can't dump WifiService from pid 1 uid 0")
        return 0
    if mode == DUMP_SLOW:
        duration(state, 12)
        return 0
    if memory["dump_fixture"]:
        # A dump the Wi-Fi service printed, kept in wifi_dump_fixtures.
        streams.say(wifi_dump_fixtures.DUMPS[sorted(wifi_dump_fixtures.DUMPS)[memory["dump_fixture"] - 1]])
        return 0
    networks = []
    if memory["listed"]:
        networks.append((target_id(memory), "VirtWifi", memory["autojoin"]))
    if memory["other"]:
        networks.append((other_id(memory), OTHER_NAMES[memory["other_name"]], memory["other"] == OTHER_ON))
    lines = ['mWifiInfo SSID: "VirtWifi", BSSID: 02:00:00:00:00:00, Supplicant state: COMPLETED']
    for section, order in enumerate((networks, networks[::-1])):
        lines.append("Dump of WifiConfigManager")
        for position, (number, name, on) in enumerate(order):
            # A map prints its first entry after its name and the others after a comma. The header of the
            # connected configuration, and only that one, has a star before its ID.
            lead = ("mPerId={" if not position else ", ") + f"{number}=" if section else ""
            star = "* " if memory["joined"] and number == target_id(memory) and name == "VirtWifi" else ""
            lines.append(f'{lead}{star}ID: {number} SSID: "{name}" PROVIDER-NAME: null BSSID: null HIDDEN: false')
            lines.append(" Protocols: RSN")
            lines.append(f" lcuid=0 allowAutojoin={str(on).lower()} mAllowedAutoJoinInAdvancedProtection=true")
            lines.append("lastConnected: 2026-11-14T03:34:22.375")
    lines.append('mScanResultMatchInfoMapForCurrentUser={ScanResultMatchInfo: SSID: "VirtWifi", from scan result: false')
    if mode in (DUMP_CUT_BEFORE, DUMP_CUT_AFTER):
        # The service stalls part-way: what it had written is all `timeout` leaves.
        first = next(number for number, line in enumerate(lines) if "allowAutojoin=" in line)
        lines = lines[:first + (mode == DUMP_CUT_AFTER)]
        duration(state, 12)
    if mode == DUMP_LATE:
        # The whole dump, after twelve seconds: too late for `timeout`.
        lines = [LATE] + lines
        duration(state, 12)
    streams.say("\n".join(lines))
    return 0


def schedule(memory: dict, when: int) -> None:
    """A change that the store must hold: its write is due `store_delay` seconds on, unless one is pending, which
    keeps its deadline."""
    if memory["store_due"] is None:
        memory["store_due"] = when + memory["store_delay"]


def advance(memory: dict, end: int) -> None:
    """Applies what happens by time `end`, in order: the config store's read, the user's forget, the join, the
    store's write."""
    if end > CLOCK_LIMIT:
        raise SystemExit("the script runs past the clock limit: it does not end")
    while True:
        due = []
        if not memory["read_done"] and memory["read_at"] <= end:
            due.append((memory["read_at"], 0, "read"))
        if memory["read_done"] and memory["listed"] and memory["forget_at"] is not None and memory["forget_at"] <= end:
            due.append((max(memory["forget_at"], memory["read_at"]), 1, "forget"))
        if (memory["read_done"] and memory["listed"] and memory["autojoin_off_at"] is not None
                and memory["autojoin_off_at"] <= end):
            due.append((max(memory["autojoin_off_at"], memory["read_at"]), 2, "autojoin"))
        if (memory["listed"] and memory["wifi_on"] and not memory["disabled_now"] and not memory["joined"]
                and memory["autojoin"] and memory["join_after"] is not None
                and memory["since"] + memory["join_after"] <= end):
            due.append((memory["since"] + memory["join_after"], 3, "join"))
        if memory["store_due"] is not None and memory["store_due"] <= end:
            due.append((memory["store_due"], 4, "store"))
        if not due:
            return
        when, _, what = min(due)
        if what == "read":
            memory["read_done"] = True
            memory["listed"] = memory["stored"]
            memory["disabled_now"] = memory["disabled"] and memory["stored"]
            memory["autojoin"] = memory["autojoin_stored"] or not memory["stored"]
            memory["since"] = memory["read_at"] if memory["stored"] else None
            memory["joined"] = memory["joined"] and memory["stored"]
        elif what == "forget":
            memory["listed"], memory["joined"], memory["disabled_now"] = False, False, False
            memory["autojoin"] = True
            memory["forget_at"] = None
            schedule(memory, when)
        elif what == "autojoin":
            memory["autojoin"] = False
            memory["autojoin_off_at"] = None
            schedule(memory, when)
        elif what == "store":
            memory["stored"], memory["disabled"] = memory["listed"], memory["disabled_now"]
            memory["autojoin_stored"] = memory["autojoin"] or not memory["listed"]
            memory["store_due"] = None
        else:
            memory["joined"] = True
            schedule(memory, when)


def append(path: str, seconds: int) -> None:
    with open(path, "a") as stream:
        stream.write(f"{seconds}\n")


def stub_call(state: str, words: list[str]) -> tuple[int, str, str]:
    """What a stub of the Wi-Fi service prints and ends with. `cmd` and `dumpsys` run a framework call; the
    `timeout LIMIT` stub runs one under a limit. The time a call takes is passed the way the stubs always did:
    a call under a limit that outlasts it is cut (status 124, and what follows its LATE line is lost), and
    any other call lets its whole duration pass, minus its LATE line."""
    limit = None
    if words[0] == "timeout":
        limit, words = int(words[1]), words[2:]
    if words[0] == "cmd":
        arguments = words[1:]
    elif words[0] == "dumpsys":
        arguments = words
    else:
        raise AssertionError(f"the fake Wi-Fi service has no command {words[0]!r}")
    streams = Streams()
    try:
        status = framework(state, arguments, streams)
    except SystemExit as stop:
        # What a process that ends so does: the message to standard error, and status 1.
        status = stop.code if isinstance(stop.code, int) else 1
        if isinstance(stop.code, str):
            streams.say(stop.code, error=True)
    with open(os.path.join(state, "duration")) as stream:
        passed = int(stream.read())
    text = "".join(streams.out).rstrip("\n")
    lines = text.split("\n") if text else []
    if limit is not None and passed > limit:
        append(os.path.join(state, "extras"), limit)
        lines = lines[:lines.index(LATE)] if LATE in lines else lines
        status = 124
    else:
        if passed > 0:
            append(os.path.join(state, "extras"), passed)
        lines = [line for line in lines if line != LATE]
    return status, "".join(line + "\n" for line in lines), "".join(streams.err)


class Service:
    """The fake Wi-Fi service of one boot, which its stubs reach over two FIFOs in the state directory: a stub
    writes its words, one a line and then END, to `request`, waits on `answer`, and prints `reply`: the status, the
    number of lines of standard output, and the lines of standard output and of standard error. A stub starts no
    interpreter and no program but its shell. The service holds each FIFO open for reading and writing, so that
    no open waits for the other side (a pair of waiting opens can fail to meet) and no end of file ends a request."""

    END = "---end-of-request---"

    def __init__(self, state: Path) -> None:
        self.state = str(state)
        self.reply = state / "reply"
        self.request = state / "request"
        self.answer = state / "answer"
        self.stopping = False
        self.thread: threading.Thread | None = None
        self.descriptors: tuple[int, int] = (-1, -1)

    def __enter__(self) -> "Service":
        for fifo in (self.request, self.answer):
            fifo.unlink(missing_ok=True)
            os.mkfifo(fifo)
        self.descriptors = (os.open(self.request, os.O_RDWR), os.open(self.answer, os.O_RDWR))
        self.stopping = False
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_exit) -> None:
        assert self.thread is not None
        self.stopping = True
        os.write(self.descriptors[0], b"\n")    # wakes the service that waits for a request
        self.thread.join(30)
        alive = self.thread.is_alive()
        for descriptor in self.descriptors:
            os.close(descriptor)
        for fifo in (self.request, self.answer):
            fifo.unlink(missing_ok=True)
        assert not alive, "the fake Wi-Fi service did not stop"

    def serve(self) -> None:
        requests, answers = self.descriptors
        words: list[str] = []
        pending = b""
        while True:
            chunk = os.read(requests, 4096)
            if self.stopping:
                return
            *lines, pending = (pending + chunk).split(b"\n")
            for line in lines:
                if line.decode() != self.END:
                    words.append(line.decode())
                    continue
                try:
                    status, out, err = stub_call(self.state, words)
                except Exception:   # a defect of the fake: the stub fails and says why
                    status, out, err = 70, "", traceback.format_exc()
                words = []
                self.reply.write_text(f"{status}\n{out.count(chr(10))}\n{out}{err}")
                os.write(answers, b"\n")


SCRIPT = Path(__file__).resolve().parents[3] / "android/lineage-24/overlay/tools/kernarium_wifi_network.sh"
MARKER = "persist.sys.kernarium.virtwifi_saved"


# ---- A guest the cases boot.

SLEEP_LIMIT = 113     # the script's own bound on its sleeping, which its header states


@dataclasses.dataclass(frozen=True)
class Boot:
    """What the framework does during one boot."""
    read_at: int | None = None       # when the config store is read; None: it was read before the script ran
    store_delay: int = 10            # seconds from a change to the store's write
    join_after: int | None = 5       # seconds an enabled network takes to join; None: never
    stalled_looks: int = 0           # the first listings the Wi-Fi thread does not answer: four seconds, "No networks"
    stall_after: int | None = None   # the listings from this second on stall as well
    slow_looks: int = 0              # the first listings that take two seconds and answer
    clock_step_back: int = 0         # seconds the wall clock steps back during each stalled listing
    failing_looks: int = 0           # the first listings that fail: the Wi-Fi service is not up
    add_fails: bool = False          # the Wi-Fi service is not up: saving fails
    add_ignored: int = 0             # the first saves that do nothing (the service is busy)
    wifi_on: bool = True
    forget_at: int | None = None     # when the user forgets the network
    autojoin_off_at: int | None = None   # when the user turns Auto-connect off for the network
    autojoin_start: bool | None = None   # the store holds Auto-connect on (True) or off (False) from the boot on
    other: int = OTHER_NONE          # another saved network, with Auto-connect on or off
    other_name: int = 0              # the index of its name in OTHER_NAMES
    dump_fixture: int = 0            # a dump kept in wifi_dump_fixtures (the position in its sorted names, from 1)
    virtwifi_id: int = 0             # VirtWifi's ID when it is not 0 (or 1, with swap_ids): the fixture's
    swap_ids: bool = False           # the other network has the first ID and VirtWifi the second
    dump_state: int = DUMP_OK        # `dumpsys wifi` fails, takes twelve seconds or says the caller may not
    kill_after_marker: bool = False  # the guest stops right after the script wrote the marker
    stop_after: int = 30             # seconds the guest runs on after the script ends before it stops


@dataclasses.dataclass(frozen=True)
class Expect:
    status: int = 0
    adds: int = 0
    notes: int = 0
    marker: str = ""
    stored: bool = False
    autojoin_off: bool = False       # the store holds Auto-connect off for the network afterwards
    asks: int | None = None          # the calls the script made of the Wi-Fi service, when it must be none
    slept: int | None = None         # the seconds the script slept, when the case pins them


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    marker: str
    stored: bool
    boots: tuple[tuple[Boot, Expect], ...]
    disabled: bool = False
    autojoin_off: bool = False       # the store holds Auto-connect off for the network


class Guest:
    """The persistent half of the framework and the property store, kept between boots."""

    def __init__(self, directory: Path, marker: str, stored: bool, disabled: bool, autojoin_off: bool) -> None:
        self.directory = directory
        self.state = directory / "state"
        (self.state / "properties").mkdir(parents=True)
        if marker:
            (self.state / "properties" / MARKER).write_text(marker + "\n")
        self.stored = stored
        self.disabled = disabled
        self.autojoin_stored = not autojoin_off
        self.service = Service(self.state)
        self.stubs = directory / "stubs"
        self.stubs.mkdir()
        # A call of the Wi-Fi service is a request to the guest's Service and the print of its reply. The
        # stubs read their files with builtins: no interpreter, `cat` or `sed` starts for a call. A step that
        # fails (the request not written, no acknowledgement, a reply that cannot be read whole) ends the stub
        # with status 69 and no reply: it never prints what an earlier call left in `reply`.
        service_call = (
            'unreachable() {\n    echo "the fake Wi-Fi service failed to $1" >&2\n    exit 69\n}\n'
            'printf "%s\\n" "@NAME@" "$@" "@END@" > "@STATE@/request" || unreachable "take the request"\n'
            'read -r _ < "@STATE@/answer" || unreachable "acknowledge the request"\n'
            'finished=\n'
            '{\n    read -r rc || unreachable "give a status"\n    read -r n || unreachable "give a line count"\n'
            '    case "$rc" in "" | *[!0-9]*) unreachable "give a numeric status" ;; esac\n'
            '    case "$n" in "" | *[!0-9]*) unreachable "give a numeric line count" ;; esac\n'
            '    while [ "$n" -gt 0 ]; do\n        IFS= read -r line || unreachable "give all its lines"\n'
            '        printf "%s\\n" "$line"\n        n=$((n - 1))\n    done\n'
            '    while IFS= read -r line; do\n        printf "%s\\n" "$line" >&2\n    done\n'
            '    finished=yes\n} < "@STATE@/reply"\n'
            '[ -n "$finished" ] || unreachable "give a reply"\n'
            'exit "$rc"\n'
        ).replace("@STATE@", str(self.state)).replace("@END@", Service.END)
        state = self.state
        bodies = {
            # `timeout LIMIT command...` is the service's too: it cuts a call that outlasts the limit.
            "cmd": service_call.replace("@NAME@", "cmd"),
            "dumpsys": service_call.replace("@NAME@", "dumpsys"),
            "timeout": service_call.replace("@NAME@", "timeout"),
            "getprop": (
                f'echo "getprop $*" >> "{state}/calls"\n'
                f'if [ -f "{state}/properties/$1" ]; then\n'
                f'    while IFS= read -r line; do printf "%s\\n" "$line"; done < "{state}/properties/$1"\n'
                'else\n    echo\nfi\n'
            ),
            "setprop": (
                f'echo "setprop $*" >> "{state}/calls"\necho "$2" > "{state}/properties/$1"\n'
                f'if [ -f "{state}/kill" ]; then rm -f "{state}/kill"; kill -9 $PPID; fi\n'
            ),
            "sleep": f'echo "$1" >> "{state}/sleeps"\n',
            # The wall clock: the monotonic time less the steps back it took (the script no longer reads it).
            "date": (
                f't=0\nfor f in sleeps extras; do\n'
                f'    while read -r s || [ -n "$s" ]; do t=$((t + s)); done < "{state}/$f"\ndone\n'
                f'while read -r s || [ -n "$s" ]; do t=$((t - s)); done < "{state}/steps"\necho $t\n'
            ),
            "log": f'echo "log $*" >> "{state}/calls"\n',
        }
        for name, body in bodies.items():
            (self.stubs / name).write_text("#!/bin/sh\n" + body)
            (self.stubs / name).chmod(0o755)

    def run(self, script: Path, boot: Boot) -> tuple[int, list[str], str, bool, int]:
        """Boots the framework and runs the script once, then lets the guest run on for `stop_after` seconds;
        returns the script's status, the calls it made, the marker, whether the config store holds the network
        when the guest stops, and the seconds the script slept."""
        if boot.autojoin_start is not None:
            self.autojoin_stored = boot.autojoin_start
        listed = self.stored and boot.read_at is None
        memory = {
            "autojoin_stored": self.autojoin_stored, "autojoin": self.autojoin_stored or not listed,
            "autojoin_off_at": boot.autojoin_off_at, "other": boot.other, "dump_state": boot.dump_state,
            "other_name": boot.other_name, "swap_ids": boot.swap_ids, "dump_fixture": boot.dump_fixture,
            "virtwifi_id": boot.virtwifi_id,
            "read_at": boot.read_at if boot.read_at is not None else 0,
            "read_done": boot.read_at is None, "store_delay": boot.store_delay, "store_due": None,
            "join_after": boot.join_after, "stalled": boot.stalled_looks, "failing": boot.failing_looks,
            "stall_after": boot.stall_after, "slow": boot.slow_looks, "clock_step_back": boot.clock_step_back,
            "add_fails": boot.add_fails, "add_ignored": boot.add_ignored, "wifi_on": boot.wifi_on,
            "stored": self.stored, "disabled": self.disabled, "forget_at": boot.forget_at,
            "listed": self.stored and boot.read_at is None,
            "disabled_now": self.disabled and self.stored and boot.read_at is None,
            "since": 0 if self.stored and boot.read_at is None else None, "joined": False,
        }
        save(str(self.state / "framework"), memory)
        for name in ("sleeps", "extras", "calls", "steps"):
            (self.state / name).write_text("")
        (self.state / "duration").write_text("0\n")
        if boot.kill_after_marker:
            (self.state / "kill").touch()
        with self.service:
            done = subprocess.run(["/bin/sh", str(script)], env={"PATH": f"{self.stubs}:/usr/bin:/bin"},
                                  capture_output=True, text=True, timeout=120)
        (self.state / "kill").unlink(missing_ok=True)
        ended = elapsed(str(self.state))
        after = load(str(self.state / "framework"))
        advance(after, ended + boot.stop_after)
        self.stored, self.disabled = after["stored"], after["disabled"]
        self.autojoin_stored = after["autojoin_stored"]
        marker_file = self.state / "properties" / MARKER
        marker = marker_file.read_text().strip() if marker_file.exists() else ""
        slept = sum(int(seconds) for seconds in (self.state / "sleeps").read_text().split())
        return done.returncode, (self.state / "calls").read_text().splitlines(), marker, self.stored, slept


# ---- The cases.

ONE = Boot()
SETTLED = Expect(notes=1, marker="2", stored=True)     # a boot that finds the network stored and joined
CASES = (
    # Marker 2 never asks the service anything, in any state.
    *(Case(f"marker 2, {what}", "2", stored, (
        (boot, Expect(marker="2", stored=stored, asks=0, autojoin_off=what == "Auto-connect off")),),
        disabled=disabled, autojoin_off=what == "Auto-connect off")
      for what, stored, disabled, boot in (
          ("an empty list (forgotten)", False, False, ONE),
          ("a joined network", True, False, ONE),
          ("a network that does not join", True, False, Boot(join_after=None)),
          ("a disabled network", True, True, ONE),
          ("Wi-Fi off", True, False, Boot(wifi_on=False)),
          ("Wi-Fi off and an empty list", False, False, Boot(wifi_on=False)),
          ("a listing that fails", True, False, Boot(failing_looks=5)),
          ("a listing that stalls", True, False, Boot(stalled_looks=5)),
          ("the store read late", False, False, Boot(read_at=3)),
          ("Auto-connect off", True, False, Boot(autojoin_start=False)),
      )),
    # A first boot saves the network and leaves 1: a join does not prove the store holds it. The next boot finds
    # it stored and joined and sets 2.
    Case("a first boot, then a boot", "", False, (
        (ONE, Expect(adds=1, notes=1, marker="1", stored=True)),
        (ONE, SETTLED),
    )),
    # The guest stops right after the marker is written and before the store is: marker 1 and no network.
    Case("A: a stop before the store wrote the network, then boots", "", False, (
        (Boot(kill_after_marker=True, stop_after=0), Expect(status=-9, adds=1, marker="1", stored=False)),
        (ONE, Expect(adds=1, notes=2, marker="1", stored=True)),
        (ONE, SETTLED),
    )),
    # R1: the network joins before the store's write and the guest stops: the join did not make 2, so the next
    # boot saves again; and the stop right after 2, in the boot that follows, loses nothing, since 2 needs the
    # network to have been listed from the store.
    Case("R1: a join before the store wrote the network, a stop, then a stop right after marker 2", "", False, (
        (Boot(stop_after=0), Expect(adds=1, notes=1, marker="1", stored=False)),
        (ONE, Expect(adds=1, notes=2, marker="1", stored=True)),
        (Boot(kill_after_marker=True, stop_after=0), Expect(status=-9, marker="2", stored=True)),
        (ONE, Expect(marker="2", stored=True, asks=0)),
    )),
    Case("data of an earlier release at marker 1 without the network", "1", False, (
        (ONE, Expect(adds=1, notes=2, marker="1", stored=True)),
        (ONE, SETTLED),
    )),
    # R2: a listing that is not an answer is not an empty list. At marker 1 it never saves.
    Case("R2: an unanswered listing at marker 1 with the network stored", "1", True, (
        (Boot(stalled_looks=3), Expect(notes=1, marker="1", stored=True)),
        (ONE, SETTLED),
    )),
    Case("R2: a failing listing at marker 1 with the network stored", "1", True, (
        (Boot(failing_looks=3), Expect(notes=1, marker="1", stored=True)),
    )),
    Case("R2: an unanswered listing, Wi-Fi off, marker 1, the network stored", "1", True, (
        (Boot(wifi_on=False, stalled_looks=9), Expect(marker="1", stored=True)),
    )),
    # The listing that times out takes the wall clock with it: the script reads no clock, so a step back during
    # the call cannot make it an answer. A stored network that an earlier release left disabled must keep marker 1.
    *(Case(f"R2: the final listing times out while the wall clock steps back {step} s", "1", True, (
        (Boot(join_after=None, stall_after=40, clock_step_back=step), Expect(marker="1", stored=True)),
    ), disabled=True) for step in (2, 9)),
    Case("R2: a listing that answers in two seconds is an answer", "1", False, (
        (Boot(slow_looks=9), Expect(adds=1, notes=2, marker="1", stored=True)),
    )),
    Case("R2: an unanswered listing at marker 1 and an empty store", "1", False, (
        (Boot(stalled_looks=1), Expect(adds=1, notes=1, marker="1", stored=True)),
    )),
    # With no marker there is nothing to lose: an unknown look still leads to the save.
    Case("R2: an unanswered listing and no marker", "", True, (
        (Boot(stalled_looks=1), Expect(adds=1, notes=1, marker="1", stored=True)),
    )),
    # The stored networks are loaded late (before boot_completed in practice): the third look finds it.
    Case("the guard: the store read after the first looks", "1", True, (
        (Boot(read_at=7), Expect(notes=1, marker="1", stored=True)),
    )),
    # R3: removed during this boot, after it was listed at the first look: it stays removed.
    Case("R3: a network forgotten during the boot after it was listed", "1", True, (
        (Boot(join_after=None, forget_at=20), Expect(notes=1, marker="2", stored=False)),
        (ONE, Expect(marker="2", stored=False, asks=0)),
    )),
    Case("R3: a join the script did not see, and a forget, between two polls", "1", True, (
        (Boot(join_after=7, forget_at=8), Expect(notes=1, marker="2", stored=False)),
    )),
    # The network this boot saved and the user forgot before the settle is saved again (accepted).
    Case("a network forgotten in the session that saved it", "", False, (
        (Boot(join_after=None, forget_at=20), Expect(adds=2, notes=1, marker="1", stored=True)),
    )),
    Case("E: marker 1 and a stored network that joins", "1", True, ((ONE, SETTLED),)),
    Case("E: marker 1 and a stored network that joins late", "1", True, ((Boot(join_after=30), SETTLED),)),
    Case("marker 1, Wi-Fi off and an empty list", "1", False, (
        (Boot(wifi_on=False), Expect(adds=1, notes=1, marker="1", stored=True)),
    )),
    Case("marker 1, Wi-Fi off and a stored network", "1", True, ((Boot(wifi_on=False), Expect(marker="1", stored=True)),)),
    Case("no marker and a network saved by hand", "", True, ((ONE, SETTLED),)),
    # Wi-Fi on, a listed network that is not joined: the disabled network of an earlier release, or one the user
    # disconnected. It is saved again once and joins; the next boot finds it stored and joined.
    Case("a disabled network", "1", True, (
        (ONE, Expect(adds=1, notes=1, marker="1", stored=True)),
        (ONE, SETTLED),
    ), disabled=True),
    Case("a disabled network that does not join once saved again", "1", True, (
        (Boot(join_after=None), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), disabled=True),
    Case("Wi-Fi on and the user disconnected the network", "1", True, (
        (Boot(join_after=None), Expect(adds=1, notes=1, marker="1", stored=True)),
    )),
    # Auto-connect off is the user's choice, not a disabled network: it keeps it at every boot until a boot joins.
    # The data of an earlier release that left the network disabled is repaired once, with no marker; the user then
    # turns Auto-connect off, and no later boot turns it on again. Turned on, the next boot joins and sets 2.
    Case("an earlier release's disabled network, repaired, then Auto-connect off for every boot", "", True, (
        (ONE, Expect(adds=1, notes=1, marker="1", stored=True)),
        (Boot(autojoin_start=False), Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
        (ONE, Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
        (Boot(autojoin_start=True), SETTLED),
        (ONE, Expect(marker="2", stored=True, asks=0)),
    ), disabled=True),
    Case("Auto-connect turned off during the settle of the repair boot", "", True, (
        (Boot(autojoin_off_at=10), Expect(notes=1, marker="", stored=True, autojoin_off=True)),
    ), disabled=True),
    Case("Auto-connect turned off during the settle of the first boot", "", False, (
        (Boot(join_after=30, autojoin_off_at=8), Expect(adds=1, notes=1, marker="1", stored=True, autojoin_off=True)),
    )),
    Case("marker 1, a disabled network with Auto-connect off too, then Auto-connect on", "1", True, (
        (Boot(), Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
        (Boot(autojoin_start=True), Expect(adds=1, notes=1, marker="1", stored=True)),
        (ONE, SETTLED),
    ), disabled=True, autojoin_off=True),
    Case("marker 1, Auto-connect off, a stored network that does not join", "1", True, (
        (Boot(join_after=None), Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
    ), autojoin_off=True),
    # Only VirtWifi's Auto-connect counts, wherever the dump prints another network.
    Case("another network with Auto-connect off, VirtWifi disabled with it on", "1", True, (
        (Boot(other=OTHER_OFF), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), disabled=True),
    Case("another network with Auto-connect on, VirtWifi with it off", "1", True, (
        (Boot(other=OTHER_ON), Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
    ), autojoin_off=True),
    # The configuration is the open VirtWifi row's ID, not a header's SSID text: another configuration of the same
    # name (another security type), a name that holds the text of a header and a name that ends with a quote have
    # other IDs, whichever of them comes first.
    *(Case(f"another configuration ({what}) with Auto-connect on, VirtWifi with it off, {order} IDs", "1", True, (
        (Boot(other=OTHER_ON, other_name=name, swap_ids=swapped),
         Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
    ), autojoin_off=True) for what, name in (("the same name", 1), ("a name with header text", 2),
                                             ("a name with a quote", 3))
      for order, swapped in (("in order", False), ("swapped", True))),
    *(Case(f"another configuration ({what}) with Auto-connect off, VirtWifi disabled with it on", "1", True, (
        (Boot(other=OTHER_OFF, other_name=name, swap_ids=swapped), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), disabled=True) for what, name in (("the same name", 1), ("a name with header text", 2))
      for swapped in (False, True)),
    # A dump the service stalls part-way leaves what it wrote. Cut through the configuration's own off value it
    # still reads as off (the value is the network's); cut before it, or through an on value, it repairs.
    Case("marker 1, Auto-connect off and a dump cut after VirtWifi's own off value", "1", True, (
        (Boot(dump_state=DUMP_CUT_AFTER), Expect(notes=1, marker="1", stored=True, autojoin_off=True)),
    ), autojoin_off=True),
    Case("marker 1, Auto-connect off and a dump cut before VirtWifi's value", "1", True, (
        (Boot(dump_state=DUMP_CUT_BEFORE), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), autojoin_off=True),
    Case("marker 1, a disabled network with Auto-connect on and a dump cut after its value", "1", True, (
        (Boot(dump_state=DUMP_CUT_AFTER), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), disabled=True),
    # The dumps the Wi-Fi service printed (wifi_dump_fixtures): the network connected or not, with other networks
    # or not, in each form of a header; and one with a log line in a block, which no real dump has. VirtWifi's ID is
    # the fixture's, and the script finds its Auto-connect under that ID alone. Off is left alone; on, with the
    # network disabled, is repaired.
    *(Case(f"the dump {name}: Auto-connect {'on' if on else 'off'}", "1", True, (
        (Boot(join_after=None, dump_fixture=sorted(wifi_dump_fixtures.DUMPS).index(name) + 1, virtwifi_id=number),
         Expect(notes=1, marker="1", stored=True, autojoin_off=True) if not on else
         Expect(adds=1, notes=1, marker="1", stored=True)),
    ), disabled=on, autojoin_off=not on) for name, (number, on) in sorted(wifi_dump_fixtures.STATES.items())),
    # A dump that does not show it is no sign of the user's choice: the repair stays what it was.
    *(Case(f"marker 1, Auto-connect off and {what}", "1", True, (
        (Boot(dump_state=state), Expect(adds=1, notes=1, marker="1", stored=True)),
    ), autojoin_off=True) for what, state in (
        ("a dump that fails", DUMP_FAILS), ("a dump that says nothing for twelve seconds", DUMP_SLOW),
        ("a dump that comes after twelve seconds", DUMP_LATE), ("a dump that is denied", DUMP_DENIED))),
    Case("a Wi-Fi service that is not up, no marker", "", False, (
        (Boot(add_fails=True), Expect(status=1, adds=30, marker="", stored=False)),
    )),
    Case("a Wi-Fi service that is not up, marker 1", "1", False, (
        (Boot(add_fails=True), Expect(status=1, adds=30, marker="1", stored=False)),
    )),
    # The longest run: the guard, 29 saves that do nothing, the settle, the second save.
    Case("the longest run", "1", False, (
        (Boot(add_ignored=29, join_after=None), Expect(adds=31, notes=2, marker="1", stored=True, slept=SLEEP_LIMIT)),
    )),
)


def judge(script: Path, case: Case) -> list[str]:
    """What the script gets wrong in one case, as messages."""
    problems = []
    with tempfile.TemporaryDirectory(prefix="wifi-network-script-") as directory:
        guest = Guest(Path(directory), case.marker, case.stored, case.disabled, case.autojoin_off)
        for number, (boot, expect) in enumerate(case.boots, 1):
            status, calls, marker, stored, slept = guest.run(script, boot)
            adds = calls.count("add")
            notes = len([call for call in calls if call.startswith("log ")])
            asks = len([call for call in calls if call in ("add", "list", "status", "dump")])
            off = stored and not guest.autojoin_stored
            if (status, adds, notes, marker, stored, off) != (expect.status, expect.adds, expect.notes, expect.marker,
                                                               expect.stored, expect.autojoin_off):
                problems.append(f"{case.name}, boot {number}: exit {status}, saves {adds}, log lines {notes}, "
                                f"marker {marker!r}, store holds the network {stored}, with Auto-connect off {off}; "
                                f"expected exit {expect.status}, saves {expect.adds}, log lines {expect.notes}, "
                                f"marker {expect.marker!r}, store {expect.stored}, Auto-connect off "
                                f"{expect.autojoin_off}")
            if expect.asks is not None and asks != expect.asks:
                problems.append(f"{case.name}, boot {number}: the script made {asks} calls of the Wi-Fi service; "
                                f"expected {expect.asks}")
            if slept > SLEEP_LIMIT or (expect.slept is not None and slept != expect.slept):
                problems.append(f"{case.name}, boot {number}: the script slept {slept} s; its bound is "
                                f"{SLEEP_LIMIT} s" + (f" and the case pins {expect.slept} s" if expect.slept else ""))
            for call in calls:
                if call.startswith("setprop ") and call.split()[1:] not in (
                        [MARKER, "1"], [MARKER, "2"]):
                    problems.append(f"{case.name}, boot {number}: {call!r} is not the marker's 1 or 2")
    return problems


def problems_of(text: str, stop_at_first: bool = False) -> list[str]:
    """What the script text gets wrong over all the cases (the cases run side by side), or, with
    `stop_at_first`, over the cases up to the first that fails."""
    with tempfile.TemporaryDirectory(prefix="wifi-network-script-text-") as directory:
        script = Path(directory) / "kernarium_wifi_network.sh"
        script.write_text(text)
        found: list[str] = []
        if stop_at_first:
            for case in CASES:
                found += judge(script, case)
                if found:
                    break
            return found
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            for problems in pool.map(lambda case: judge(script, case), CASES):
                found += problems
        return found


def mutated(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"the mutation target {old!r} is not in the script exactly once")
    return text.replace(old, new)


class WifiNetworkScript(unittest.TestCase):
    maxDiff = None

    def test_the_script_does_what_the_cases_say(self) -> None:
        self.assertEqual(problems_of(SCRIPT.read_text()), [])

    def test_a_stub_that_cannot_use_the_service_fails_and_prints_nothing_old(self) -> None:
        """A stub ends non-zero when its request is not written, no acknowledgement comes, or the reply
        cannot be read whole, and it never prints what an earlier call left in `reply`. The transport is
        made of plain files and directories, so that each step fails by itself and no step waits."""
        stale = "0\n1\nstale success\n"
        file, directory, absent = "file", "directory", "absent"
        # step -> (request, answer, reply), each a file, a directory, absent, or the text of a file
        steps = {
            "the request cannot be written": (directory, "\n", stale),
            "the acknowledgement cannot be read": (file, directory, stale),
            "the acknowledgement is at end of file": (file, "", stale),
            "the reply is a directory": (file, "\n", directory),
            "the reply is absent": (file, "\n", absent),
            "the reply is empty": (file, "\n", ""),
            "the reply has no line count": (file, "\n", "0\n"),
            "the status is not a number": (file, "\n", "x\n0\n"),
            "the line count is not a number": (file, "\n", "0\nmany\nstale success\n"),
            "the reply ends before its lines": (file, "\n", "0\n3\nstale success\n"),
        }
        for step, (request, answer, reply) in steps.items():
            with self.subTest(step), tempfile.TemporaryDirectory(prefix="wifi-stub-transport-") as scratch:
                guest = Guest(Path(scratch), "", False, False, False)
                for name, kind in (("request", request), ("answer", answer), ("reply", reply)):
                    path = guest.state / name
                    if kind == directory:
                        path.mkdir()
                    elif kind != absent:
                        path.write_text("" if kind == file else kind)
                done = subprocess.run(
                    [str(guest.stubs / "cmd"), "wifi", "list-networks"], check=False, stdin=subprocess.DEVNULL,
                    capture_output=True, text=True, timeout=30)
                self.assertNotEqual(done.returncode, 0, done)
                if reply != "0\n3\nstale success\n":    # a reply cut short may print the lines it has
                    self.assertEqual(done.stdout, "", done)
                self.assertNotIn("stale success", done.stderr, done)

    def test_the_test_bites(self) -> None:
        text = SCRIPT.read_text()
        early = '[ "$current" = 2 ] && exit 0\n'
        settled = 'if [ "$listed" = 1 ]; then\n        setprop "$marker" 2 && note "VirtWifi was stored'
        removed = 'if [ "$listed" = 1 ]; then\n            setprop "$marker" 2 && note "VirtWifi was removed'
        mutants = (
            ("marker 1 is final", mutated(text, early, 'case "$current" in 1|2) exit 0 ;; esac\n')),
            ("marker 2 asks the Wi-Fi service", mutated(text, early, "")),
            ("a join in a boot that saved sets 2", mutated(text, settled, settled.replace('[ "$listed" = 1 ]', ":"))),
            ("an unanswered listing reads as absent", mutated(
                text, 'elif [ "$code" = 0 ]; then\n        state=absent', 'elif true; then\n        state=absent')),
            ("a network removed during the boot is saved again", mutated(
                text, removed, removed.replace('[ "$listed" = 1 ]', "false"))),
            ("the listing's time is read from the wall clock", mutated(mutated(
                text, 'listing=$(timeout "$limit" cmd wifi list-networks 2>/dev/null)\n    code=$?\n',
                'began=$(date +%s)\n    listing=$(cmd wifi list-networks 2>/dev/null)\n    code=$?\n'
                'ended=$(date +%s)\n'), 'elif [ "$code" = 0 ]; then\n        state=absent',
                'elif [ "$code" = 0 ] && [ $((ended - began)) -lt 4 ]; then\n        state=absent')),
            ("a limit above the Wi-Fi thread's four seconds", mutated(text, "limit=3", "limit=5")),
            ("a limit that kills an answer of two seconds", mutated(text, "limit=3", "limit=1")),
            ("a guard of two looks", mutated(text, "looks=3", "looks=2")),
            ("a guard of one look", mutated(text, "looks=3", "looks=1")),
            ("the saved network is not saved again after the settle", mutated(
                text, 'cmd wifi add-network VirtWifi open >/dev/null 2>&1\nsaved_it\nnote "VirtWifi saved again',
                'exit 0\nsaved_it\nnote "VirtWifi saved again')),
            ("a join is not waited for", mutated(text, 'if wait_joined "$settle"; then', "if :; then")),
            ("Auto-connect is not looked at", mutated(text, 'if [ "$autojoin" = off ]; then', "if false; then")),
            ("an unreadable dump reads as off", mutated(
                text, '[ "$off" = 1 ] && [ "$on" = 0 ] && autojoin=off', '[ "$on" = 0 ] && autojoin=off')),
            ("another configuration's Auto-connect counts", mutated(
                text, 'if [ "${rest%%[!0-9]*}" = "$netid" ]; then mine=1; else mine=0; fi',
                "mine=1")),
            ("a header's SSID text names the configuration", mutated(
                text, 'if [ "${rest%%[!0-9]*}" = "$netid" ]; then mine=1; else mine=0; fi',
                """case "$line" in *'SSID: "VirtWifi"'*) mine=1 ;; *) mine=0 ;; esac""")),
            ("a header must have a star", mutated(text, "(^|=)(\\* )?ID: [0-9]+", "(^|=)\\* ID: [0-9]+")),
            ("a line that holds the text of a header is a header", mutated(
                text, "(^|=)(\\* )?ID: [0-9]+", "ID: [0-9]+")),
            ("the dump has no time limit", mutated(
                text, 'timeout "$dumplimit" dumpsys -t', "dumpsys -t")),
        )
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda mutant: problems_of(mutant[1], stop_at_first=True), mutants))
        for (what, _), found in zip(mutants, results):
            with self.subTest(what):
                self.assertNotEqual(found, [], what)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--script":
        found = problems_of(Path(sys.argv[2]).read_text())
        print("\n".join(found) if found else "no case fails")
        raise SystemExit(1 if found else 0)
    unittest.main()
