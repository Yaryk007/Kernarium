#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Free the diagnostic-report slots of a device before a run uses it.

iOS keeps a capped number of diagnostic reports per report name: the
``JetsamEvent`` reports of every process kill, and the crash reports of each
process (``<Process>-<date>.ips``) are separate sets. A full set drops the
next report instead of replacing an old one, so an application kill during a
run leaves no evidence. Every device runner therefore calls ``preflight``
before it installs, launches or activates anything: it counts the reports of
the kinds a run needs, and when fewer than ``FREE_SLOTS`` of ``REPORT_CAP``
slots are free for a kind, it moves the whole report store off the device and
counts again, failing the run if the slots are still short.

``devicectl`` lists and copies the ``systemCrashLogs`` domain but has no
command that removes a file, and ``idevicecrashreport`` (libimobiledevice)
removes without checking that its own copy was written, so a move has three
steps:

1. ``devicectl`` copies every report listed at the start into a fresh private
   directory (``copy/``), and each copy is checked against the listing: a
   regular file, not a link, of the listed size. A size match is not a byte
   comparison. A failed or unverifiable copy fails the run before any removal.
2. Only then does ``idevicecrashreport`` run, without a filter, into a second
   fresh private directory (``mover/``) and empty the store. A report that
   arrives after the first listing is not in ``copy/``: the mover removes it
   and it lands in ``mover/`` without a checked copy.
3. The store is listed again, and the run fails if a kind is still short or a
   file cannot be counted.

The mover needs the hardware UDID, which ``devicectl`` resolves from whatever
the caller named the device, and it is only needed when a kind is short.

Reports are the device owner's data, so they live outside every checkout, in
a per-user folder by default. Each run creates its own report directory: one
that exists is refused, as is a path with a symlink anywhere in it. This
module prints counts and a directory, never a report name or content, and
every failure it raises is one of a fixed set of messages: no tool output is
repeated.

The cap is an observation, not a documented value: a device's store held
exactly that many reports of several kinds and none above it, and its newest
``JetsamEvent`` was older than a process kill whose report was missing.

  ios_device_reports.py --device DEVICE [--dry-run] [--leave-device-reports]

checks a device on its own. Its exit status is 0 when every kind has its free
slots after any move, 3 when a kind is short (a dry run and the opt-out never
move anything, so they report the state as found), and 1 on a failure.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import errno
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass, field
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import time
import unicodedata

import ios_device
from ios_device import DeviceError

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
# iOS keeps at most this many reports of one name (observed, not documented).
REPORT_CAP = 25
# A run starts only with this many free slots in each kind it needs.
FREE_SLOTS = 5
JETSAM = "JetsamEvent"
CRASH_LOG_DOMAIN = "systemCrashLogs"
MOVE_TOOL = "idevicecrashreport"
DIRECTORY_VARIABLE = "UML_DEVICE_REPORTS_DIR"
LIST_TIMEOUT = 120
RESOLVE_TIMEOUT = 60
COPY_TIMEOUT = 120
# The whole verified copy of a full store.
COPY_BUDGET = 1800
MOVE_TIMEOUT = 600
# ``<name>-<date>-<time>[.<n>].<ips|crash>[.<suffix>]``: the name is what the
# cap counts.
REPORT_NAME = re.compile(
    r"^(?P<name>.+?)-\d{4}-\d{2}-\d{2}-\d{6}(?:\.\d+)?\.(?:ips|crash)(?:\..*)?$"
)
# The hardware UDID: eight and sixteen hexadecimal digits, or forty.
UDID = re.compile(r"^(?:[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}|[0-9A-Fa-f]{40})$")
# The processes of the applications this repository runs on a device, for a
# caller that names none.
APPLICATION_PROCESSES = ("Kernarium", "Andarium")


@dataclass(frozen=True)
class ListedFile:
    """A regular file of the device's report store."""

    path: str
    size: int


def not_understood(reason: str) -> DeviceError:
    return DeviceError(f"the device's report listing is not understood: {reason}")


def parse_listing(text: str) -> list[ListedFile]:
    """The regular files of a ``devicectl device info files`` JSON listing.

    Only a listing in the shape devicectl documents is accepted: a flat list
    of entries that each name a relative path, say with booleans whether it is
    a directory and whether it is a link and, for a file, give its size.
    Anything else is an error, so that an unknown shape can never read as an
    empty store."""
    try:
        document = json.loads(text)
    except ValueError:
        raise not_understood("not JSON") from None
    result = document.get("result") if isinstance(document, dict) else None
    entries = result.get("files") if isinstance(result, dict) else None
    if not isinstance(entries, list):
        raise not_understood("no list of files")
    listed: list[ListedFile] = []
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise not_understood("an entry is not an object")
        # Only the two documented objects may be nested in an entry, and the
        # listing is flat: no entry holds a list or object of further entries.
        if "children" in entry or any(
            isinstance(value, (list, dict)) for key, value in entry.items()
            if key not in ("metadata", "resources")
        ):
            raise not_understood("a nested listing")
        path, resources = entry.get("relativePath"), entry.get("resources")
        if not isinstance(path, str) or not path or not isinstance(resources, dict):
            raise not_understood("an entry has no path or resources")
        if any(isinstance(value, (list, dict)) for value in resources.values()):
            raise not_understood("a nested listing")
        directory = resources.get("isDirectory")
        if not isinstance(directory, bool):
            raise not_understood("isDirectory is not a boolean")
        # A link is neither copied nor counted reliably, and the mover would
        # remove it: it is not a store this module can empty safely. An entry
        # that does not say is not known to be no link.
        link = resources.get("isSymbolicLink")
        if not isinstance(link, bool):
            raise not_understood("isSymbolicLink is not a boolean")
        if link:
            raise not_understood("a symbolic link")
        if directory:
            continue
        pure = pathlib.PurePosixPath(path)
        if (str(pure) != path or pure.is_absolute() or "\0" in path
                or any(part in (".", "..") for part in pure.parts)):
            raise not_understood("a path is not a plain relative path")
        metadata = entry.get("metadata")
        size = metadata.get("size") if isinstance(metadata, dict) else None
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            raise not_understood("a file has no size")
        if path in seen:
            raise not_understood("a path is listed twice")
        seen.add(path)
        listed.append(ListedFile(path, size))
    return listed


def report_name(file_name: str) -> str | None:
    """The kind a report counts under, or None for a file that is no report."""
    found = REPORT_NAME.match(file_name)
    return found.group("name") if found else None


def basenames(paths: Iterable[str]) -> list[str]:
    return [pathlib.PurePosixPath(path).name for path in paths]


def count_reports(
    paths: Iterable[str], processes: Sequence[str]
) -> dict[str, int]:
    """Reports held per kind: ``JetsamEvent`` and each process's crashes."""
    counts = {JETSAM: 0, **{process: 0 for process in processes}}
    for name in map(report_name, basenames(paths)):
        if name in counts:
            counts[name] += 1
    return counts


def unparsed_reports(paths: Iterable[str], processes: Sequence[str]) -> int:
    """Files named for a kind a run needs that this module cannot count."""
    prefixes = tuple(
        f"{name}{separator}"
        for name in (JETSAM, *processes) for separator in "-._"
    )
    return sum(
        1 for name in basenames(paths)
        if name.startswith(prefixes) and report_name(name) is None
    )


def short_kinds(counts: dict[str, int]) -> list[str]:
    return [
        name for name, held in counts.items() if REPORT_CAP - held < FREE_SLOTS
    ]


def default_destination() -> pathlib.Path:
    """A per-user folder outside any checkout, new for each run."""
    return (
        pathlib.Path(os.path.expanduser("~")) / "Library" / "Logs" / "Kernarium"
        / "device-reports" / time.strftime("%Y%m%d-%H%M%S")
    )


def inside_checkout(path: pathlib.Path) -> bool:
    """Whether ``path`` is in a checkout: this one, or any directory that
    holds a ``.git`` entry (a linked worktree has one too). The user's home
    and its parents are not looked at, so a home kept under version control
    does not refuse every default."""
    checkout = REPOSITORY.resolve()
    if path == checkout or checkout in path.parents:
        return True
    home = pathlib.Path(os.path.realpath(os.path.expanduser("~")))
    for ancestor in (path, *path.parents):
        if ancestor == home or ancestor in home.parents:
            break
        if os.path.lexists(ancestor / ".git"):
            return True
    return False


def validate_destination(destination: pathlib.Path) -> pathlib.Path:
    """The absolute report directory, or an error.

    Reports never enter a checkout, ignored by git or not, and no component
    of the path may be a symlink, the user's home and its parents included:
    the absolute path must be its own real path. This is the early check that
    names the problem; ``prepare_destination`` is what holds against a swap."""
    path = pathlib.Path(os.path.abspath(destination))
    real = pathlib.Path(os.path.realpath(path))
    if inside_checkout(path) or inside_checkout(real):
        raise DeviceError(
            "the report directory is inside a checkout; name one outside it "
            f"(--device-reports-dir or {DIRECTORY_VARIABLE})"
        )
    if real != path:
        raise DeviceError(
            "the report directory's path contains a symlink; name an "
            f"explicit directory without symlinks (--device-reports-dir or "
            f"{DIRECTORY_VARIABLE})"
        )
    return path


DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC


def open_path(
    start: int, names: Sequence[str], *, create: bool
) -> int:
    """A handle on the directory ``names`` below the handle ``start``, found
    one component at a time without following a symlink. Missing directories
    are made private when ``create`` is set. The caller closes the handle."""
    current = os.dup(start)
    try:
        for name in names:
            if create:
                try:
                    os.mkdir(name, 0o700, dir_fd=current)
                except FileExistsError:
                    pass
            following = os.open(name, DIRECTORY_FLAGS, dir_fd=current)
            os.close(current)
            current = following
    except BaseException:
        os.close(current)
        raise
    return current


def changed_path() -> DeviceError:
    return DeviceError(
        "the report directory's path holds a symlink or a file where a "
        "directory was; nothing was removed"
    )


class ReportDirectory:
    """The report directory of one run and the two directories in it, held
    open: ``copy/`` for the verified copy and ``mover/`` for the mover's.

    The run creates all three itself, so none of them is reused or left by
    an earlier run. The tools (``devicectl``, ``idevicecrashreport``) still
    take their directories as path strings, so a parent swapped for a link
    after the handles were opened can misplace what they write. Everything
    that decides on a removal goes through the handles instead: a copy is
    checked in ``copy/`` by its handle, so a misplaced copy is missing there
    and nothing is removed, and ``confirm`` compares the path strings with the
    handles once more right before the mover runs."""

    def __init__(self, path: pathlib.Path) -> None:
        self.path = path
        self.copy = path / "copy"
        self.mover = path / "mover"
        self.run_fd = self.copy_fd = self.mover_fd = -1

    def close(self) -> None:
        for name in ("mover_fd", "copy_fd", "run_fd"):
            descriptor = getattr(self, name)
            if descriptor >= 0:
                os.close(descriptor)
                setattr(self, name, -1)

    def __enter__(self) -> "ReportDirectory":
        return self

    def __exit__(self, *exception: object) -> None:
        self.close()

    def confirm(self) -> None:
        """Raise unless each path string still names the directory this run
        created, by its real path and outside every checkout."""
        for path, descriptor in ((self.path, self.run_fd),
                                 (self.copy, self.copy_fd),
                                 (self.mover, self.mover_fd)):
            try:
                named, held = os.stat(path), os.fstat(descriptor)
                same = (named.st_dev, named.st_ino) == (
                    held.st_dev, held.st_ino)
                real = pathlib.Path(os.path.realpath(path))
            except OSError:
                same, real = False, pathlib.Path()
            if not same or real != path or inside_checkout(real):
                raise DeviceError(
                    "the report directory is not where this run created it "
                    "any more; nothing was removed"
                )


def prepare_destination(path: pathlib.Path) -> ReportDirectory:
    """Create the run's report directory, private, with ``copy/`` and
    ``mover/`` in it, and hold them open.

    The directories above it are walked from the root one component at a
    time, without following a symlink (a missing one is made), and the run's
    own directory is made exclusively: one that exists, empty or not, is
    refused."""
    directory = ReportDirectory(path)
    try:
        if not path.is_absolute() or len(path.parts) < 2:
            raise OSError(errno.EINVAL, "not an absolute path")
        root = os.open(path.anchor, DIRECTORY_FLAGS)
        try:
            parent = open_path(root, path.parts[1:-1], create=True)
        finally:
            os.close(root)
        try:
            os.mkdir(path.name, 0o700, dir_fd=parent)
            directory.run_fd = os.open(path.name, DIRECTORY_FLAGS, dir_fd=parent)
        finally:
            os.close(parent)
        os.fchmod(directory.run_fd, 0o700)
        for name in ("copy", "mover"):
            os.mkdir(name, 0o700, dir_fd=directory.run_fd)
            descriptor = os.open(name, DIRECTORY_FLAGS, dir_fd=directory.run_fd)
            setattr(directory, f"{name}_fd", descriptor)
            os.fchmod(descriptor, 0o700)
    except FileExistsError:
        directory.close()
        raise DeviceError(
            "the report directory already exists; every run needs a new one"
        ) from None
    except OSError as error:
        directory.close()
        if error.errno in (errno.ELOOP, errno.ENOTDIR):
            raise changed_path() from None
        raise DeviceError("the report directory cannot be created") from None
    return directory


def run_tool(
    command: Sequence[str], timeout: int, what: str
) -> subprocess.CompletedProcess[bytes]:
    """Run a tool with its output captured and never repeated."""
    try:
        return subprocess.run(
            list(command),
            capture_output=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        raise DeviceError(f"{what} timed out after {timeout} seconds") from None
    except OSError:
        raise DeviceError(f"{what} could not be started") from None


def folded(path: str) -> str:
    """What a case- and normalization-insensitive filesystem makes of a path."""
    return unicodedata.normalize("NFC", path).casefold()


class DeviceStore:
    """The report store of one device."""

    def __init__(self, device: str) -> None:
        self.device = device

    def listing(self) -> list[ListedFile]:
        """The device's report files, from ``devicectl`` in memory."""
        completed = run_tool(
            ios_device.device_command(
                "device", "info", "files", "--device", self.device,
                "--domain-type", CRASH_LOG_DOMAIN,
                "--json-output", "-", "--quiet", "--timeout", str(LIST_TIMEOUT),
            ),
            LIST_TIMEOUT + 30,
            "listing the device's diagnostic reports",
        )
        if completed.returncode != 0:
            raise DeviceError(
                "cannot list the device's diagnostic reports (devicectl "
                f"exited {completed.returncode})"
            )
        try:
            text = completed.stdout.decode("utf-8")
        except UnicodeDecodeError:
            raise not_understood("not text") from None
        return parse_listing(text)

    def resolve_udid(self) -> str:
        """The device's hardware UDID, which the mover is given exactly."""
        completed = run_tool(
            ios_device.device_command(
                "device", "info", "details", "--device", self.device,
                "--json-output", "-", "--quiet", "--timeout",
                str(RESOLVE_TIMEOUT),
            ),
            RESOLVE_TIMEOUT + 30,
            "resolving the device",
        )
        found = None
        if completed.returncode == 0:
            try:
                result = json.loads(completed.stdout.decode("utf-8"))["result"]
                for keys in (("properties", "hardware", "udid"),
                             ("hardwareProperties", "udid")):
                    node = result
                    for key in keys:
                        node = node.get(key) if isinstance(node, dict) else None
                    if isinstance(node, str):
                        found = node
                        break
            except (ValueError, KeyError, TypeError, AttributeError):
                found = None
        if found is None or UDID.match(found) is None:
            raise DeviceError(
                "cannot resolve the device's UDID, so no report was removed"
            )
        if UDID.match(self.device) and self.device.lower() != found.lower():
            raise DeviceError(
                "devicectl resolved the device to another UDID than the one "
                "named, so no report was removed"
            )
        return found

    def copy_verified(
        self, files: Sequence[ListedFile], directory: ReportDirectory
    ) -> None:
        """Copy every listed file into ``directory.copy`` and check each copy
        against the listing: it exists at its relative path, as a regular
        file, no link, of the listed size.

        ``devicectl`` takes the destination as a path string, so a swapped
        parent can misplace a copy. Every directory is made and every copy
        checked through the open handle of ``copy/`` instead, so a misplaced
        copy is missing there and fails the run before any removal."""
        if len({folded(file.path) for file in files}) != len(files):
            raise DeviceError(
                "two reports would share a path on this Mac; nothing was "
                "removed"
            )
        deadline = time.monotonic() + COPY_BUDGET
        for file in files:
            names = pathlib.PurePosixPath(file.path).parts
            try:
                os.close(open_path(directory.copy_fd, names[:-1], create=True))
            except OSError:
                raise DeviceError(
                    "the report directory cannot be written; nothing was "
                    "removed") from None
            if time.monotonic() >= deadline:
                raise DeviceError(
                    f"copying the reports took longer than {COPY_BUDGET} "
                    "seconds; nothing was removed")
            completed = run_tool(
                ios_device.device_command(
                    "device", "copy", "from", "--device", self.device,
                    "--domain-type", CRASH_LOG_DOMAIN,
                    "--source", file.path,
                    "--destination", str(directory.copy / file.path),
                    "--quiet", "--timeout", str(COPY_TIMEOUT),
                ),
                COPY_TIMEOUT + 30,
                "copying a report",
            )
            if completed.returncode != 0:
                raise DeviceError(
                    "copying a report failed (devicectl exited "
                    f"{completed.returncode}); nothing was removed"
                )
        wrong = 0
        for file in files:
            names = pathlib.PurePosixPath(file.path).parts
            try:
                parent = open_path(directory.copy_fd, names[:-1], create=False)
                try:
                    found = os.stat(
                        names[-1], dir_fd=parent, follow_symlinks=False)
                finally:
                    os.close(parent)
                good = stat.S_ISREG(found.st_mode) and found.st_size == file.size
            except OSError:
                good = False
            wrong += not good
        if wrong:
            raise DeviceError(
                f"{wrong} of {len(files)} copied reports are missing or "
                "differ from the listing; nothing was removed"
            )

    def require_mover(self) -> None:
        """The mover must be there before anything is copied."""
        if shutil.which(MOVE_TOOL) is None:
            raise DeviceError(
                f"{MOVE_TOOL} is not on PATH and the device's report store "
                "is short of free slots; install libimobiledevice (it "
                f"provides {MOVE_TOOL}) or empty the store by hand"
            )

    def remove_all(self, udid: str, directory: ReportDirectory) -> None:
        """Empty the store with ``idevicecrashreport``, whose own copy goes
        into ``directory.mover``. It is run only after ``copy_verified`` and
        ``directory.confirm()``.

        Not exercised against a device: the store was empty whenever this
        module was tried there, so what follows is covered by fakes only."""
        tool = shutil.which(MOVE_TOOL)
        if tool is None:
            raise DeviceError(f"{MOVE_TOOL} is not on PATH")
        completed = run_tool(
            [tool, "-u", udid, str(directory.mover)], MOVE_TIMEOUT, MOVE_TOOL)
        if completed.returncode != 0:
            raise DeviceError(
                f"{MOVE_TOOL} exited {completed.returncode} after the reports "
                "were copied; the store may be partly emptied"
            )


@dataclass
class ReportSlots:
    """What a preflight found and did."""

    before: dict[str, int]
    after: dict[str, int]
    short: list[str] = field(default_factory=list)
    destination: pathlib.Path | None = None
    held: int = 0
    moved: int = 0
    dry_run: bool = False
    left: bool = False
    failed: bool = False
    counted: bool = True

    @property
    def short_after(self) -> list[str]:
        return short_kinds(self.after) if self.after else []


def label(name: str) -> str:
    return name if name == JETSAM else f"{name} crash"


def summary_line(slots: ReportSlots) -> str:
    counts = ", ".join(
        f"{label(name)} {held} -> {slots.after[name]}"
        for name, held in slots.before.items()
    )
    if not slots.short:
        outcome = "nothing to move"
    elif slots.dry_run:
        outcome = f"would move the whole store ({slots.held} reports)"
    elif slots.left:
        outcome = "left in place, short: " + ", ".join(
            label(name) for name in slots.short)
    elif slots.failed:
        outcome = "could not move them"
    else:
        outcome = (f"moved the whole store ({slots.moved} reports) to "
                   f"{slots.destination}")
    return (
        f"device reports (cap {REPORT_CAP} per kind, {FREE_SLOTS} free "
        f"needed): {counts}; {outcome}"
    )


def ensure_report_slots(
    store: DeviceStore,
    processes: Sequence[str],
    destination: pathlib.Path | None,
    *,
    leave: bool = False,
    dry_run: bool = False,
    emit: Callable[[str], None] = lambda line: print(line, flush=True),
) -> ReportSlots:
    """At least ``FREE_SLOTS`` free slots of ``JetsamEvent`` and of each
    process's crash reports, or an error.

    ``destination`` is where a move puts the reports; only a dry run or
    ``leave`` may omit it. ``leave`` only counts and reports: a caller that
    needs the old reports on the device accepts a short store and a listing
    that cannot be read. ``dry_run`` also moves nothing but reports what it
    would move."""
    if destination is None and not (leave or dry_run):
        raise DeviceError("no directory was named for moved reports")
    if destination is not None and not leave:
        destination = validate_destination(destination)
    try:
        files = store.listing()
    except DeviceError as error:
        if not leave:
            raise
        emit(f"device reports: not counted ({error})")
        return ReportSlots({}, {}, left=True, counted=False)
    paths = [file.path for file in files]
    before = count_reports(paths, processes)
    unparsed = unparsed_reports(paths, processes)
    if unparsed and not leave:
        raise DeviceError(
            f"{unparsed} files on the device are named for a process under "
            "test but cannot be counted as reports; empty the store by hand "
            f"with {MOVE_TOOL} -u <udid> <directory>"
        )
    slots = ReportSlots(dict(before), dict(before), short_kinds(before),
                        held=len(files), dry_run=dry_run, left=leave)
    if not slots.short or leave or dry_run:
        emit(summary_line(slots))
        return slots
    assert destination is not None
    slots.destination = destination
    try:
        # Everything that can fail runs before the first removal.
        udid = store.resolve_udid()
        store.require_mover()
        with prepare_destination(destination) as directory:
            store.copy_verified(files, directory)
            # The tools still take path strings, so a swap can misplace
            # their output; the copy was checked through the handle, which
            # then fails, and this compares the strings with the handles once
            # more right before the mover (see ReportDirectory).
            directory.confirm()
            store.remove_all(udid, directory)
    except DeviceError:
        slots.failed = True
        emit(summary_line(slots))
        raise
    remaining = store.listing()
    left = [file.path for file in remaining]
    if unparsed_reports(left, processes):
        raise DeviceError(
            "files on the device are named for a process under test but "
            "cannot be counted as reports after the move, so its free slots "
            f"are unknown; empty the store by hand with {MOVE_TOOL} -u "
            "<udid> <directory>"
        )
    slots.after = count_reports(left, processes)
    slots.moved = max(len(files) - len(remaining), 0)
    emit(summary_line(slots))
    if slots.short_after:
        raise DeviceError(
            f"the device's report store is still short of {FREE_SLOTS} free "
            "slots for "
            + ", ".join(label(name) for name in slots.short_after)
            + " after moving the reports"
        )
    return slots


def add_arguments(parser: argparse.ArgumentParser) -> None:
    """The two options every device runner takes."""
    group = parser.add_argument_group("diagnostic reports")
    group.add_argument(
        "--device-reports-dir", type=pathlib.Path, metavar="DIR",
        default=pathlib.Path(os.environ[DIRECTORY_VARIABLE])
        if os.environ.get(DIRECTORY_VARIABLE) else None,
        help="a directory that does not exist yet, outside every checkout "
             "and with no symlink in its path, for the reports moved off the "
             f"device (default: ${DIRECTORY_VARIABLE}, else a per-user "
             "folder)",
    )
    group.add_argument(
        "--leave-device-reports", action="store_true",
        help="count the device's reports but never move any, and run even "
             f"when a kind has fewer than {FREE_SLOTS} free slots",
    )


def report_processes(
    applications: Iterable[ios_device.Application],
    extra: Iterable[str] = (),
) -> list[str]:
    names = [application.process_name for application in applications]
    names.extend(extra)
    return list(dict.fromkeys(names))


def preflight(
    options: argparse.Namespace,
    applications: Iterable[ios_device.Application],
    *,
    extra_processes: Iterable[str] = (),
    emit: Callable[[str], None] = lambda line: print(line, flush=True),
) -> ReportSlots:
    """The check every device runner makes before it installs, launches or
    activates anything.

    ``options`` carries ``device`` and the two ``add_arguments`` options."""
    return ensure_report_slots(
        DeviceStore(options.device),
        report_processes(applications, extra_processes),
        options.device_reports_dir or default_destination(),
        leave=options.leave_device_reports,
        emit=emit,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Count a device's diagnostic reports and free the slots "
                    "a run needs. Exit status: 0 every kind has its free "
                    "slots, 3 a kind is short (a dry run and the opt-out "
                    "move nothing), 1 a failure.")
    parser.add_argument("--device", required=True,
                        help="a devicectl identifier (any name or UDID)")
    parser.add_argument(
        "--process", action="append", metavar="NAME",
        help="a process whose crash reports a run needs slots for "
             f"(repeatable; default: {', '.join(APPLICATION_PROCESSES)})")
    parser.add_argument("--dry-run", action="store_true",
                        help="count and report, moving nothing")
    add_arguments(parser)
    options = parser.parse_args(argv)
    try:
        slots = ensure_report_slots(
            DeviceStore(options.device),
            options.process or list(APPLICATION_PROCESSES),
            options.device_reports_dir or default_destination(),
            leave=options.leave_device_reports,
            dry_run=options.dry_run,
        )
    except DeviceError as error:
        print(f"ios_device_reports.py: {error}", file=sys.stderr)
        return 1
    if not slots.counted:
        return 1
    return 3 if slots.short_after else 0


if __name__ == "__main__":
    sys.exit(main())
