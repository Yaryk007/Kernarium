# SPDX-License-Identifier: GPL-2.0-or-later
"""A debugger that serves the JIT's request for executable memory on an iOS
device, so a harness can run a development build on the JIT without a person
and without StikDebug.

On an iOS device the runtime's executable memory is a region it maps read and
execute, which a debugger attached to the process makes runnable once
(darwin/cpu/jit_memory/executable_memory.cpp, docs/architecture.md). The
process asks with ``brk #0xf00d`` and registers ``x16`` (the command), ``x0``
(an address) and ``x1`` (a length):

  - ``x16 == 1`` prepares the region at ``x0`` of ``x1`` bytes: the debugger
    writes one byte into each 16 KiB page of it through its own memory access,
    which is what makes the pages runnable, and resumes past the ``brk``. The
    address and the length are read as 40 bits, so the region is below 1 TiB.
  - ``x16 == 0`` asks the debugger to detach, after which the pages stay
    runnable and the process runs with no debugger.

This module is that debugger for a harness, in two parts. ``JitService``
decides each request and touches nothing but what it is given, so a test can
drive it. ``serve`` is the command lldb runs once attached (``jit_serve``).
``main`` launches the application on a device stopped, attaches lldb to the
process that launch made, serves it until it detaches and then holds the
console; the lease on the device, the application's install and everything
else a run needs stay with the caller. It writes nothing into the
application's container. The process of the launch is the one the device lists
that no earlier listing held: devicectl does not say, because with ``--console``
it writes its report only when the application ends, and the newest listed
instance can be an older one that is still on its way out.

Andarium makes its request as soon as it opens, so the debugger is attached
and serving before the application's first line runs: the process is launched
stopped, and lldb continues it.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from collections.abc import Callable
from dataclasses import dataclass
import json
import os
import pathlib
import re
import shlex
import signal
import subprocess
import sys
import tempfile
import time

# ``brk #0xf00d``.
REQUEST_WORD = 0xD43E01A0
PREPARE = 1
DETACH = 0
# The host page of the devices that need this route.
PAGE_BYTES = 0x4000
# The debugger reads the address and the length as 40 bits.
ADDRESS_LIMIT = 1 << 40
# The byte the debugger writes into each page.
BLESSING_BYTE = 0x69
PACKET_RETRIES = 3
# How long to wait for the launched process to be listed.
PID_TIMEOUT = 90


@dataclass(frozen=True)
class Request:
    command: int
    address: int
    length: int


@dataclass(frozen=True)
class Reply:
    """What the debugger does about one request."""

    # The value left in ``x0`` when the process resumes: the region's address
    # when it was prepared, zero when it was refused.
    x0: int
    # Detach after resuming past the request.
    detach: bool = False


class JitService:
    """Decides each request. ``write_byte(address)`` writes the blessing byte
    at an address through the debugger and says whether it did."""

    def __init__(
        self,
        write_byte: Callable[[int], bool],
        log: Callable[[str], None] = lambda _message: None,
    ) -> None:
        self._write_byte = write_byte
        self._log = log
        self.prepared_pages = 0
        self.detached = False

    def handle(self, request: Request) -> Reply | None:
        """The reply to a request, or None for a command this does not know
        (the process resumes past it and nothing else happens)."""
        if request.command == DETACH:
            self.detached = True
            self._log("detach requested")
            return Reply(x0=0, detach=True)
        if request.command == PREPARE:
            return Reply(x0=self._prepare(request.address, request.length))
        self._log(f"unknown command {request.command}")
        return None

    def _prepare(self, address: int, length: int) -> int:
        if length == 0:
            return address
        problem = None
        if address == 0:
            problem = "no address: this debugger prepares only a region the process mapped"
        elif address % PAGE_BYTES != 0:
            problem = f"address {address:#x} is not page aligned"
        elif address + length > ADDRESS_LIMIT:
            problem = f"{address:#x}+{length:#x} reaches past 40 bits"
        if problem is not None:
            self._log(f"refused: {problem}")
            return 0
        pages = (length + PAGE_BYTES - 1) // PAGE_BYTES
        started = time.monotonic()
        for page in range(pages):
            if not self._write_byte(address + page * PAGE_BYTES):
                self._log(f"the write at page {page} of {pages} failed")
                return 0
            self.prepared_pages += 1
        seconds = time.monotonic() - started
        self._log(
            f"prepared {address:#x}+{length:#x}: {pages} pages in {seconds:.1f} s"
        )
        return address


def decode(x16: int, x0: int, x1: int) -> Request:
    """A request from the three registers, as the process leaves them."""
    return Request(x16 & 0xFFFFFFFF, x0, x1)


# --- lldb ---------------------------------------------------------------


def _packet(interpreter: object, text: str) -> tuple[str, bool]:
    import lldb

    result = lldb.SBCommandReturnObject()
    interpreter.HandleCommand(  # type: ignore[attr-defined]
        f'process plugin packet send "{text}" --', result
    )
    out = (result.GetOutput() or "") + (result.GetError() or "")
    found = re.search(r"response: (.*)", out)
    return (found.group(1).strip() if found else ""), result.Succeeded()


def serve(debugger, command, _context, _result, _internal) -> None:  # noqa: ANN001
    """``jit_serve [--log FILE] [--timeout SECONDS]``: continue the attached
    process and serve its requests until it detaches, exits or the time is up."""
    options = argparse.ArgumentParser(prog="jit_serve")
    options.add_argument("--log", type=pathlib.Path)
    options.add_argument("--timeout", type=float, default=900)
    parsed = options.parse_args(shlex.split(command))
    log_file = parsed.log.open("a", encoding="utf-8") if parsed.log else None

    def log(message: str) -> None:
        line = time.strftime("%H:%M:%S ") + message
        sys.stderr.write(line + "\n")
        if log_file is not None:
            log_file.write(line + "\n")
            log_file.flush()

    try:
        _serve(debugger, parsed.timeout, log)
    except Exception:  # noqa: BLE001 - lldb would swallow it silently
        import traceback

        log("the debugger script failed:\n" + traceback.format_exc())
        raise


def _serve(debugger, timeout: float, log: Callable[[str], None]) -> None:  # noqa: ANN001
    import lldb

    interpreter = debugger.GetCommandInterpreter()
    process = debugger.GetSelectedTarget().GetProcess()
    deadline = time.monotonic() + timeout
    while (process.GetState() not in (lldb.eStateStopped, lldb.eStateExited,
                                      lldb.eStateDetached)
           and time.monotonic() < deadline):
        time.sleep(0.2)
    if process.GetState() != lldb.eStateStopped:
        log("the process is not stopped; nothing to serve")
        return
    log(f"attached to pid {process.GetProcessID()}")
    # Faults in the JIT's code are the process's to handle.
    for name in ("SIGBUS", "SIGSEGV", "SIGILL"):
        interpreter.HandleCommand(
            f"process handle {name} -p true -s false -n false",
            lldb.SBCommandReturnObject(),
        )

    def write_byte(address: int) -> bool:
        for _ in range(PACKET_RETRIES):
            reply, ok = _packet(interpreter, f"M{address:x},1:{BLESSING_BYTE:02x}")
            if ok and reply in ("OK", ""):
                return True
        return False

    service = JitService(write_byte, log)
    debugger.SetAsync(False)
    while time.monotonic() < deadline:
        error = process.Continue()
        if error.Fail():
            log(f"continue failed: {error.GetCString()}")
            return
        state = process.GetState()
        if state in (lldb.eStateExited, lldb.eStateDetached):
            log(f"the process ended (state {state})")
            return
        for thread in process:
            frame = thread.GetFrameAtIndex(0)
            pc = frame.GetPC()
            failure = lldb.SBError()
            word = process.ReadUnsignedFromMemory(pc, 4, failure)
            if failure.Fail() or word != REQUEST_WORD:
                continue
            request = decode(
                frame.FindRegister("x16").GetValueAsUnsigned(),
                frame.FindRegister("x0").GetValueAsUnsigned(),
                frame.FindRegister("x1").GetValueAsUnsigned(),
            )
            log(f"request {request}")
            reply = service.handle(request)
            if reply is not None and not reply.detach:
                frame.FindRegister("x0").SetValueFromCString(f"{reply.x0:#x}")
            frame.SetPC(pc + 4)
            if reply is not None and reply.detach:
                outcome = process.Detach()
                log("detached" if outcome.Success() else f"detach failed: {outcome.GetCString()}")
                return
            break
        else:
            thread = process.GetSelectedThread()
            log(f"another stop: {thread.GetStopDescription(100)}")
    log("the time is up")


def __lldb_init_module(debugger, _internal) -> None:  # noqa: ANN001
    debugger.HandleCommand(
        f"command script add -f {pathlib.Path(__file__).stem}.serve jit_serve"
    )


# --- the host side -------------------------------------------------------


class DebuggerError(RuntimeError):
    pass


def device_command(*arguments: str) -> list[str]:
    return ["xcrun", "devicectl", *arguments]


def attach_command(device: str, pid: int, log: pathlib.Path,
                   timeout: float) -> list[str]:
    """lldb, attached to the process, serving it as this file's ``jit_serve``."""
    return [
        "xcrun", "lldb", "-b",
        "-o", f"device select {device}",
        "-o", f"device process attach -p {pid}",
        "-o", f"command script import {pathlib.Path(__file__).resolve()}",
        "-o", f"jit_serve --log {shlex.quote(str(log))} --timeout {timeout:g}",
    ]


def find_pids(listing: object, process_name: str) -> set[int]:
    """The pids of the processes whose executable is ``<process_name>.app`` in
    a ``devicectl device info processes`` listing, however it is nested."""
    found: set[int] = set()

    def walk(node: object) -> None:
        if isinstance(node, dict):
            executable = node.get("executable")
            pid = node.get("processIdentifier")
            if (isinstance(executable, str) and isinstance(pid, int)
                    and f"/{process_name}.app/" in executable):
                found.add(pid)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    walk(listing)
    return found


def application_pids(device: str, process_name: str) -> set[int]:
    """The pids the device lists for the application now."""
    with tempfile.TemporaryDirectory(prefix="ios-jit-processes-") as name:
        report = pathlib.Path(name) / "processes.json"
        subprocess.run(
            device_command(
                "device", "info", "processes", "--device", device,
                "--json-output", str(report), "--quiet", "--timeout", "30",
            ),
            check=False, stdin=subprocess.DEVNULL, timeout=60,
        )
        if not report.exists():
            raise DebuggerError("the device did not list its processes")
        return find_pids(json.loads(report.read_text(encoding="utf-8")),
                         process_name)


def launched_pid(device: str, process_name: str, before: set[int],
                 timeout: float) -> int:
    """The pid of the process a launch made: the one the device lists for the
    application that ``before``, the pids it listed ahead of the launch, did
    not hold. The newest listed pid is not it when an older instance is still
    on its way out, and attaching to that one leaves the new process stopped
    for ever."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            new = application_pids(device, process_name) - before
        except (DebuggerError, subprocess.SubprocessError, ValueError):
            new = set()
        if new:
            return max(new)
        if time.monotonic() >= deadline:
            raise DebuggerError(
                f"no new {process_name} process on the device within "
                f"{timeout:.0f} seconds")
        time.sleep(1.0)


def launch_command(device: str, bundle_id: str) -> list[str]:
    """devicectl, launching the application stopped for a debugger and holding
    its console."""
    return device_command(
        "device", "process", "launch", "--device", device,
        "--terminate-existing", "--start-stopped", "--console", bundle_id)


def start_serving(device: str, pid: int, log: pathlib.Path, *,
                  serve_timeout: float = 900,
                  output: pathlib.Path | None = None) -> subprocess.Popen[bytes]:
    """Starts lldb serving the stopped process ``pid``; the caller waits for
    the returned process, which ends once the application has detached (or the
    time is up), and reads ``log``."""
    sink = output.open("wb") if output is not None else subprocess.DEVNULL
    return subprocess.Popen(
        attach_command(device, pid, log, serve_timeout),
        stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT)


def main(argv: list[str] | None = None) -> int:
    """Launch an application stopped, serve its JIT request, hold its console."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="devicectl identifier or UDID")
    parser.add_argument("--bundle-id", required=True)
    parser.add_argument("--process-name", required=True,
                        help="the application's name, as in <name>.app")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="a launch setting for the application")
    parser.add_argument("--log", type=pathlib.Path, required=True,
                        help="where the console and the debugger's log go (a directory)")
    parser.add_argument("--until", default=None, metavar="REGEX",
                        help="stop holding the console at the first line that matches")
    parser.add_argument("--pid-timeout", type=float, default=PID_TIMEOUT,
                        help="how long to wait for the launched process to be listed")
    parser.add_argument("--hold-seconds", type=float, default=600)
    parser.add_argument("--serve-timeout", type=float, default=900)
    options = parser.parse_args(argv)
    options.log.mkdir(parents=True, exist_ok=True)
    console_log = options.log / "console.log"
    serve_log = options.log / "jit-debugger.log"
    # What the device lists before the launch is not the launch's process.
    before = application_pids(options.device, options.process_name)
    environment = dict(os.environ)
    for setting in options.env:
        name, _, value = setting.partition("=")
        environment[f"DEVICECTL_CHILD_{name}"] = value
    with console_log.open("wb") as console:
        launch = subprocess.Popen(
            launch_command(options.device, options.bundle_id),
            stdin=subprocess.DEVNULL, stdout=console, stderr=subprocess.STDOUT,
            env=environment)
        try:
            pid = launched_pid(
                options.device, options.process_name, before,
                options.pid_timeout)
            debugger = start_serving(
                options.device, pid, serve_log,
                serve_timeout=options.serve_timeout,
                output=options.log / "lldb.log")
            debugger.wait(timeout=options.serve_timeout + 60)
            until = re.compile(options.until) if options.until else None
            deadline = time.monotonic() + options.hold_seconds
            while launch.poll() is None and time.monotonic() < deadline:
                if until is not None and until.search(
                        console_log.read_text(encoding="utf-8", errors="replace")):
                    break
                time.sleep(1.0)
        finally:
            if launch.poll() is None:
                launch.send_signal(signal.SIGINT)
                try:
                    launch.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    launch.terminate()
    print(f"console: {console_log}; debugger: {serve_log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
