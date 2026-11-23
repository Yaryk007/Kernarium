#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove another CPU dispatches the host's interrupts when CPU0 is late.

Darwin has no per-thread SIGIO, so the guest's CPU0 collects the host's
descriptor events at its clock tick and when it leaves idle. A host that does
not run CPU0's carrier thread for a while therefore holds every event back for
as long. A library inserted into the launcher (cpu_hold.cpp) suspends that
carrier for 300 ms every second, as a loaded host does, while the host echoes a
small message through the launcher's loopback forward to a guest vsock server
every 20 ms. The server runs on CPU1, so that CPU0 is only the CPU that
collects the host's events. Three guests of two vCPUs differ in what the CPUs
do meanwhile:

* busy: a spinner on each CPU, so CPU1 takes over at its own clock tick;
* spinner on CPU0: CPU1 idles, so the IRQ notification thread wakes it; and
* idle: nothing runs, so CPU0 idles when the host holds it, and the
  notification thread wakes CPU1 although CPU0 is the idle waiter.

Each echo that begins while the carrier is held must come back within
BOUND_ALLOWANCE_MS of the slowest echo of the same phase with no hold. Without
the takeover it comes back when the host lets CPU0 run, up to
HOLD_MILLISECONDS after the request. An echo already under way when a hold
begins may have work queued on CPU0, which no other CPU can run; it is not
judged. A hold that lands inside CPU0's dispatch leaves the events to it, so a
few holds per phase may fail, as many as that phase's duty cycle of CPU0 inside
its dispatch makes likely (HOLDS_ALLOWED_TO_FAIL). A hold in which no echo began
is not judged either, and is reported; a phase with fewer than
HOLDS_JUDGED_MINIMUM judged holds fails the run. The timing verdict is not
taken on a lockdep kernel: its checks slow the guest, and that run is read for
what the console reports.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import shutil
import socket
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "kernel"))
from run_guest_suspend_holds import drain, require_clean  # noqa: E402


VCPUS = 2
BOOT_SECONDS = 180
ECHO_PORT = 5000
ECHO_BYTES = 8
# The 50 Hz of an interactive stream, the rate the stalls were measured at.
ECHO_PERIOD_SECONDS = 0.020
ECHO_TIMEOUT_SECONDS = 10.0

HOLD_CPU = 0
HOLD_MILLISECONDS = 300
HOLD_PERIOD_MILLISECONDS = 1000
# A hold ends at least this long, by the library's clock, or the carrier did
# not stand still for what was asked.
HELD_MINIMUM_NS = int(0.95 * HOLD_MILLISECONDS * 1_000_000)
CONTROL_SECONDS = 4.0
HOLD_SECONDS = 20.0
HOLDS_MINIMUM = 15
ECHOES_MINIMUM = 400
# A hold in which no echo began judges nothing. A phase needs this many holds
# that judged at least one echo, or the run fails: it said nothing about them.
HOLDS_JUDGED_MINIMUM = 15

# What the takeover may add to an echo: two clock ticks without a dispatch
# (20 ms) plus a tick and the wake of the server, and room for the host. An
# echo comes back after the hold when nothing takes over: about HOLD_MILLISECONDS.
BOUND_ALLOWANCE_MS = 60.0
# The control, with no hold, must itself stay well inside the hold, or the host
# is too busy for this test to say anything: with the allowance, the bound is
# then still well under what an echo takes when nothing takes over.
CONTROL_LIMIT_MS = 150.0
# Without the takeover most holds fail. A hold that stops CPU0 inside its own
# dispatch fails with it, since the dispatch holds the token and no other CPU
# may take it from a CPU that is not running: that is the limit of the design.
# How many holds that is depends on how long CPU0 spends inside its dispatch in
# the phase, so each phase has its own allowance: the most failing holds out of
# HOLD_SECONDS holds that the phase's measured rate leaves a chance of under
# one in a thousand of exceeding. The rate is the share of holds that fail
# (nearly all land inside CPU0's dispatch), over about 400 holds per phase on
# probe and plain kernels: busy 1.8 %, spinner on CPU0 1.5 %, idle 2.0 %.
HOLDS_ALLOWED_TO_FAIL = {
    "busy": 3,
    "spinner on CPU0": 3,
    "idle": 3,
}

SERVER = "/opt/uml-test/irq_takeover_echo.py"
SPINNER = "/opt/uml-test/irq_takeover_spin.py"
SPINNER_PIDS = "/tmp/irq-takeover-spin.pids"

GUEST_SERVER = r'''
import os, socket, sys

port, cpu = int(sys.argv[1]), int(sys.argv[2])
os.sched_setaffinity(0, {cpu})
try:
    os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(1))
except OSError:
    pass
listener = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
listener.bind((socket.VMADDR_CID_ANY, port))
listener.listen(4)
while True:
    connection, _ = listener.accept()
    with connection:
        while True:
            data = connection.recv(64)
            if not data:
                break
            connection.sendall(data)
'''

GUEST_SPINNER = r'''
import os, sys

os.sched_setaffinity(0, {int(sys.argv[1])})
os.nice(19)
while True:
    pass
'''

# phase name -> the CPUs that run a spinner
PHASES = (
    ("busy", (0, 1)),
    ("spinner on CPU0", (0,)),
    ("idle", ()),
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


class HoldLibrary:
    """The control files of the library inserted into the launcher."""

    def __init__(self, directory: pathlib.Path) -> None:
        self._directory = directory

    def start(self) -> None:
        text = f"{HOLD_CPU} {HOLD_MILLISECONDS} {HOLD_PERIOD_MILLISECONDS}\n"
        (self._directory / "hold").write_text(text)

    def stop(self) -> None:
        (self._directory / "hold").unlink(missing_ok=True)

    def holds(self) -> list[tuple[int, int]]:
        """Each hold, in order, as (nanoseconds it lasted, when it began on
        CLOCK_MONOTONIC)."""
        path = self._directory / "holds"
        if not path.exists():
            return []
        return [
            (int(fields[1]), int(fields[2]))
            for fields in (line.split() for line in path.read_text().splitlines())
        ]


def connect(port: int) -> socket.socket:
    stream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        stream.settimeout(ECHO_TIMEOUT_SECONDS)
        stream.connect(("127.0.0.1", port))
    except BaseException:
        stream.close()
        raise
    return stream


def echo(stream: socket.socket, number: int) -> tuple[float, int]:
    """One round trip through the launcher's forward: its seconds, and when it
    began on CLOCK_MONOTONIC, the clock of the hold library."""
    message = number.to_bytes(ECHO_BYTES, "little")
    began = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    start = time.monotonic()
    stream.sendall(message)
    received = bytearray()
    while len(received) < ECHO_BYTES:
        chunk = stream.recv(ECHO_BYTES - len(received))
        if not chunk:
            raise EOFError("the echo stream ended")
        received += chunk
    elapsed = time.monotonic() - start
    if bytes(received) != message:
        raise RuntimeError(f"the echo returned {bytes(received)!r}")
    return elapsed, began


def run_echoes(guest: Guest, stream: socket.socket, seconds: float,
               first: int) -> list[tuple[float, int]]:
    """Round trips at 50 Hz for ``seconds``, each as (milliseconds, when it
    began), while the console stays drained."""
    times: list[tuple[float, int]] = []
    end = time.monotonic() + seconds
    number = first
    while time.monotonic() < end:
        began = time.monotonic()
        seconds_taken, started = echo(stream, number)
        times.append((seconds_taken * 1000.0, started))
        number += 1
        drain(guest, max(0.0, ECHO_PERIOD_SECONDS - (time.monotonic() - began)))
    return times


def wait_for_server(guest: Guest, port: int) -> None:
    deadline = time.monotonic() + 60
    while True:
        try:
            with connect(port) as stream:
                echo(stream, 0)
                return
        except (OSError, EOFError):
            pass
        if time.monotonic() > deadline:
            raise RuntimeError("the guest echo server never answered")
        drain(guest, 0.5)


def set_spinners(guest: Guest, cpus: tuple[int, ...]) -> None:
    guest.check_command(
        f"if [ -s {SPINNER_PIDS} ]; then kill -9 $(cat {SPINNER_PIDS}); fi; "
        f"rm -f {SPINNER_PIDS}",
        30,
    )
    for cpu in cpus:
        guest.check_command(
            f"(python3 {SPINNER} {cpu} >/dev/null 2>&1 </dev/null & "
            f"echo $! >> {SPINNER_PIDS})",
            30,
        )


def judge_holds(holds: list[tuple[int, int]],
                echoes: list[tuple[float, int]],
                bound_ms: float) -> dict:
    """Judge the echoes that began while a hold lasted.

    Returns "failed", the holds with an echo over the bound, each as (hold
    number, slowest milliseconds, when it began in milliseconds from the hold's
    start); "unjudged", the numbers of the holds in which no echo began, which
    judge nothing; and "judged" and "skipped", how many echoes were judged and
    how many were under way when a hold began, which are not.
    """
    failed: list[tuple[int, float, float]] = []
    unjudged: list[int] = []
    judged = 0
    skipped = 0
    for number, (lasted, begin) in enumerate(holds):
        worst = (0.0, 0.0)
        began_in_hold = 0
        for ms, started in echoes:
            if started < begin:
                skipped += started + ms * 1e6 > begin
            elif started <= begin + lasted:
                began_in_hold += 1
                if ms > worst[0]:
                    worst = (ms, (started - begin) / 1e6)
        judged += began_in_hold
        if not began_in_hold:
            unjudged.append(number)
        elif worst[0] > bound_ms:
            failed.append((number, worst[0], worst[1]))
    return {"failed": failed, "unjudged": unjudged, "judged": judged,
            "skipped": skipped}


def run_phase(guest: Guest, library: HoldLibrary, port: int, name: str,
              spinners: tuple[int, ...]) -> dict:
    set_spinners(guest, spinners)
    with connect(port) as stream:
        drain(guest, 1.0)
        control = [ms for ms, _ in run_echoes(guest, stream, CONTROL_SECONDS, 1)]
        before = len(library.holds())
        library.start()
        try:
            # The echoes outlast the last hold's start by half a period, so
            # that hold is judged too.
            held_echoes = run_echoes(
                guest, stream, HOLD_SECONDS + HOLD_PERIOD_MILLISECONDS / 2000.0,
                1 << 20,
            )
        finally:
            library.stop()
        # The last hold may still be under way.
        drain(guest, HOLD_MILLISECONDS / 1000.0 + 0.5)
    holds = library.holds()[before:]
    held = [ms for ms, _ in held_echoes]
    control_max = max(control)
    bound_ms = control_max + BOUND_ALLOWANCE_MS
    verdict = judge_holds(holds, held_echoes, bound_ms)
    failed, unjudged = verdict["failed"], verdict["unjudged"]
    judged_holds = len(holds) - len(unjudged)
    outcome = {
        "phase": name,
        "control_echoes": len(control),
        "control_max_ms": round(control_max, 1),
        "bound_ms": round(bound_ms, 1),
        "echoes": len(held),
        "judged": verdict["judged"],
        "skipped_under_way": verdict["skipped"],
        "p50_ms": round(percentile(held, 0.50), 1),
        "p99_ms": round(percentile(held, 0.99), 1),
        "max_ms": round(max(held), 1),
        "holds": len(holds),
        "holds_judged": judged_holds,
        "holds_unjudged": len(unjudged),
        "holds_failed": len(failed),
        "held_min_ms": round(min(lasted for lasted, _ in holds) / 1e6, 1)
        if holds else 0.0,
    }
    print(
        f"{name}: {len(held)} echoes through {len(holds)} holds of CPU{HOLD_CPU} "
        f"(each at least {outcome['held_min_ms']} ms): median "
        f"{outcome['p50_ms']} ms, p99 {outcome['p99_ms']} ms, slowest "
        f"{outcome['max_ms']} ms; {verdict['judged']} began in a hold and "
        f"{verdict['skipped']} were under way when one began; {judged_holds} holds "
        f"judged an echo and {len(failed)} of them had one over "
        f"{outcome['bound_ms']} ms (the slowest of {len(control)} echoes without "
        f"a hold, {outcome['control_max_ms']} ms, plus {BOUND_ALLOWANCE_MS:g})"
    )
    for number, ms, offset in failed:
        print(f"  hold {number}: an echo took {ms:.1f} ms, begun {offset:+.1f} ms "
              f"from the start of the hold")
    if unjudged:
        print(f"  holds in which no echo began, not judged: "
              f"{', '.join(map(str, unjudged))}")
    if len(holds) < HOLDS_MINIMUM:
        raise RuntimeError(
            f"{name}: only {len(holds)} holds, {HOLDS_MINIMUM} were needed"
        )
    if min(lasted for lasted, _ in holds) < HELD_MINIMUM_NS:
        raise RuntimeError(
            f"{name}: a hold lasted {outcome['held_min_ms']} ms, under "
            f"{HELD_MINIMUM_NS / 1e6:.0f} ms"
        )
    if judged_holds < HOLDS_JUDGED_MINIMUM:
        raise RuntimeError(
            f"{name}: only {judged_holds} of {len(holds)} holds judged an echo "
            f"({HOLDS_JUDGED_MINIMUM} were needed): the run says nothing about "
            f"the others"
        )
    if len(held) < ECHOES_MINIMUM:
        raise RuntimeError(f"{name}: only {len(held)} echoes in the hold run")
    if control_max > CONTROL_LIMIT_MS:
        raise RuntimeError(
            f"{name}: the echo took {control_max:.1f} ms with no hold "
            f"(limit {CONTROL_LIMIT_MS:g} ms): the host is too busy to judge"
        )
    return outcome


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--hold-library", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    # Short, so no path made inside it outgrows a socket address.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-irqt-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    environment = dict(os.environ)
    environment["DYLD_INSERT_LIBRARIES"] = str(arguments.hold_library.resolve())
    environment["UML_CPU_HOLD_DIRECTORY"] = str(private)
    try:
        with Guest(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            "1G",
            "tmpfs",
            (f"ncpus={VCPUS}", f"--vsock-forward=127.0.0.1:{port}-{ECHO_PORT}"),
            overlay_shell=True,
            engine=arguments.engine,
            environment=environment,
            payloads={
                SERVER: GuestFile(GUEST_SERVER.encode(), 0o755),
                SPINNER: GuestFile(GUEST_SPINNER.encode(), 0o755),
            },
        ) as guest:
            guest.boot_to_shell(BOOT_SECONDS)
            request = len(guest.output())
            library = HoldLibrary(private)
            guest.check_command(
                f"(python3 {SERVER} {ECHO_PORT} {1} "
                ">/run/irq-takeover-echo.log 2>&1 </dev/null &)",
                30,
            )
            wait_for_server(guest, port)
            report = [
                run_phase(guest, library, port, name, spinners)
                for name, spinners in PHASES
            ]
            set_spinners(guest, ())
            # Every phase runs before the bound judges any of them.
            slow = [
                f"{outcome['phase']}: {outcome['holds_failed']} of "
                f"{outcome['holds_judged']} judged holds (allowed "
                f"{HOLDS_ALLOWED_TO_FAIL[outcome['phase']]})"
                for outcome in report
                if outcome["holds_failed"] > HOLDS_ALLOWED_TO_FAIL[outcome["phase"]]
            ]
            log = guest.check_command("cat /run/irq-takeover-echo.log", 30).strip()
            if log:
                raise RuntimeError(f"the guest echo server reported:\n{log}")
            require_clean(guest, "irq takeover", request)
            print(json.dumps(report))
            if slow:
                raise RuntimeError(
                    f"echoes took too long while CPU{HOLD_CPU} was held: "
                    + "; ".join(slow)
                )
            if not guest.shutdown(60):
                raise RuntimeError("the guest did not power off")
    finally:
        shutil.rmtree(private, ignore_errors=True)
    print(
        f"echoes came back within {BOUND_ALLOWANCE_MS:g} ms of the host's own "
        f"round trip while CPU{HOLD_CPU} was held"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
