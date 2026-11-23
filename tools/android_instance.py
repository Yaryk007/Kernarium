# SPDX-License-Identifier: GPL-2.0-or-later
"""A guest instance of the Android release: its data disk and what it records.

An instance is a directory holding the guest's writable disk, `data.img`, and
`instance.json`, which says which release the disk was last started with, and
`identity.json`, the installation's serial number and eth0 address, which the
launcher makes (the emulator, from `--android-identity=`) and this module only
keeps or deletes with the disk, under the installation's lock (`identity.lock`,
see `installation_lock`). A
launcher decides before every start whether the release it carries may run on
that disk (`decide`) and applies the decision (`apply`); the moment it sees the
guest reach `sys.boot_completed` it calls `booted`, which counts a pending
update as applied, and after the guest has shut down cleanly it calls
`complete`. The rules are those of
android/lineage-24/README.md#guest-instance-storage, and
android/lineage-24/support/instance/update-decisions.json holds vectors that
every implementation of them (this one, the Andarium app) must reproduce.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import datetime
import fcntl
import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Mapping

SCHEMA = 1
DATA_DISK = "data.img"
STATE_FILE = "instance.json"
SNAPSHOT_PREFIX = "data.img.pre-"
# The installation's identity, made beside the data disk by the emulator
# (darwin/device/block/README.md owns the file). It lives and dies with the
# data disk: an update, a snapshot restore and a guest's own factory reset
# leave it, a reset or a new data disk ends it. A write of it that a crash cut
# short leaves a name that starts with the same text.
IDENTITY_FILE = "identity.json"
RELEASE_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
# The build number orders releases, with one exception: the numbers of the
# counter the first public releases were built with (it began as a date and
# every proof build spent one) are older than every number outside this range.
# Inside and outside it, numbers compare as integers.
LEGACY_BUILD_FIRST = 2026110800
LEGACY_BUILD_LAST = 2026110899


class InstanceError(RuntimeError):
    """A refusal or a failure the user must act on."""


def build_order(build_number: int) -> tuple[int, int]:
    """The key that orders build numbers: `a` is older than `b` when
    `build_order(a) < build_order(b)`, and the keys are equal only for equal
    numbers."""
    legacy = LEGACY_BUILD_FIRST <= build_number <= LEGACY_BUILD_LAST
    return (0 if legacy else 1, build_number)


# --- the release ---------------------------------------------------------------


@dataclass(frozen=True)
class Release:
    """What an instance records about the release it starts with."""

    release_id: str
    build_fingerprint: str
    build_number: int
    security_patch: str
    system_disk_sha256: str
    data_layout: int
    minimum_gib: int
    default_gib: int
    maximum_gib: int

    def identity(self) -> dict[str, str]:
        return {
            "release_id": self.release_id,
            "build_fingerprint": self.build_fingerprint,
            "build_incremental": str(self.build_number),
            "system_disk_sha256": self.system_disk_sha256,
        }


def _load_json(path: Path, label: str) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise InstanceError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise InstanceError(f"{label} {path} is not an object")
    return value


def read_release(release: Path) -> Release:
    """The identity of an installed release, from its manifest and layout."""
    manifest = _load_json(release / "release-manifest.json", "release manifest")
    layout = _load_json(release / "guest/partition-layout.json", "partition layout")
    try:
        android = manifest["android"]
        system = layout["system_disk"]
        data = layout["data_disk"]
        capacity = data["capacity_gib"]
        parsed = Release(
            release_id=manifest["release_id"],
            build_fingerprint=android["fingerprint"],
            build_number=android["build_number"],
            security_patch=android["security_patch"],
            system_disk_sha256=system["sha256"],
            data_layout=data["layout_version"],
            minimum_gib=capacity["minimum"],
            default_gib=capacity["default"],
            maximum_gib=capacity["maximum"],
        )
    except (KeyError, TypeError) as error:
        raise InstanceError(f"Android release {release} is incomplete: {error}") from error
    integers = (
        parsed.build_number, parsed.data_layout, parsed.minimum_gib,
        parsed.default_gib, parsed.maximum_gib,
    )
    if (
        not isinstance(parsed.release_id, str)
        or RELEASE_ID_PATTERN.fullmatch(parsed.release_id) is None
        or not isinstance(parsed.build_fingerprint, str)
        or not parsed.build_fingerprint
        or not isinstance(parsed.security_patch, str)
        or not isinstance(parsed.system_disk_sha256, str)
        or SHA256_PATTERN.fullmatch(parsed.system_disk_sha256) is None
        or any(isinstance(value, bool) or not isinstance(value, int) or value < 1
               for value in integers)
    ):
        raise InstanceError(
            f"Android release {release} has an invalid identity (it may predate "
            "manifest schema 6)"
        )
    return parsed


# --- the state -----------------------------------------------------------------


def _timestamp(now: Callable[[], datetime.datetime] | None = None) -> str:
    moment = (now or (lambda: datetime.datetime.now(datetime.timezone.utc)))()
    return moment.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


@dataclass(frozen=True)
class State:
    """instance.json, schema 1."""

    data_layout: int
    created_with: str
    # The release the data disk was last started with.
    release_id: str
    build_fingerprint: str
    build_incremental: str
    system_disk_sha256: str
    last_booted: str
    # The last start that reached boot_completed and shut down cleanly.
    last_completed: dict[str, str] | None = None
    # An update whose first boot has not reached boot_completed: the identity
    # the data disk was last started with before it, and the snapshot taken then.
    pending_update: dict[str, Any] | None = None

    @property
    def build_number(self) -> int:
        return int(self.build_incremental)

    def to_json(self) -> dict[str, Any]:
        return {
            "schema": SCHEMA,
            "data_layout": self.data_layout,
            "release_id": self.release_id,
            "build_fingerprint": self.build_fingerprint,
            "build_incremental": self.build_incremental,
            "system_disk_sha256": self.system_disk_sha256,
            "created_with": self.created_with,
            "last_booted": self.last_booted,
            "last_completed": self.last_completed,
            "pending_update": self.pending_update,
        }


_STATE_KEYS = {
    "schema", "data_layout", "release_id", "build_fingerprint",
    "build_incremental", "system_disk_sha256", "created_with", "last_booted",
    "last_completed", "pending_update",
}


def parse_state(document: Mapping[str, Any]) -> State:
    if set(document) != _STATE_KEYS:
        raise InstanceError("instance.json keys differ from schema 1")
    if document["schema"] != SCHEMA:
        raise InstanceError(f"unsupported instance.json schema: {document['schema']!r}")
    texts = ("release_id", "build_fingerprint", "build_incremental",
             "system_disk_sha256", "created_with", "last_booted")
    if any(not isinstance(document[key], str) or not document[key] for key in texts):
        raise InstanceError("instance.json has an empty or non-text field")
    if (
        isinstance(document["data_layout"], bool)
        or not isinstance(document["data_layout"], int)
        or document["data_layout"] < 1
        or re.fullmatch(r"[1-9][0-9]{0,9}", document["build_incremental"]) is None
    ):
        raise InstanceError("instance.json has an invalid data layout or build number")
    completed = document["last_completed"]
    pending = document["pending_update"]
    if completed is not None and (
        not isinstance(completed, dict)
        or set(completed) != {"release_id", "build_incremental", "at"}
    ):
        raise InstanceError("instance.json last_completed is malformed")
    if pending is not None and (
        not isinstance(pending, dict)
        or set(pending) != {"to_release_id", "snapshot", "from"}
        or not isinstance(pending["from"], dict)
        or set(pending["from"]) != {
            "release_id", "build_fingerprint", "build_incremental",
            "system_disk_sha256", "data_layout", "last_booted",
        }
    ):
        raise InstanceError("instance.json pending_update is malformed")
    return State(**{key: document[key] for key in _STATE_KEYS - {"schema"}})


def read_state(instance: Path) -> State | None:
    path = instance / STATE_FILE
    if not path.exists():
        return None
    return parse_state(_load_json(path, "instance state"))


def write_state(instance: Path, state: State) -> None:
    """Replace instance.json atomically."""
    instance.mkdir(parents=True, exist_ok=True)
    temporary = instance / (STATE_FILE + ".tmp")
    payload = json.dumps(state.to_json(), indent=2, sort_keys=True) + "\n"
    with temporary.open("w", encoding="utf-8") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, instance / STATE_FILE)


# --- the decision --------------------------------------------------------------


@dataclass(frozen=True)
class Flags:
    retry: bool = False
    restore_snapshot: bool = False
    reset: bool = False


@dataclass(frozen=True)
class Decision:
    action: str
    reason: str


ACTIONS = (
    "create", "boot", "update", "migrate", "retry", "restore", "reset",
    "refuse_downgrade", "refuse_conflict", "refuse_pending",
)


def decide(
    state: State | None,
    data_disk_exists: bool,
    snapshot_exists: bool,
    release: Release,
    flags: Flags = Flags(),
) -> Decision:
    """What to do before starting `release` on an instance.

    `state` is instance.json, or None when there is none. Pure: it looks at
    nothing else, so every implementation reproduces it from the vectors."""
    selected = [name for name in ("retry", "restore_snapshot", "reset") if getattr(flags, name)]
    if len(selected) > 1:
        return Decision("refuse_conflict", "retry, restore-snapshot and reset exclude each other")
    if flags.reset:
        return Decision("reset", "the instance is deleted and created anew")
    if state is None:
        if data_disk_exists:
            return Decision(
                "refuse_conflict",
                "a data disk has no instance.json to say which release wrote it",
            )
        return Decision("create", "a new instance")
    if state.pending_update is not None:
        pending = state.pending_update
        if flags.restore_snapshot:
            if not snapshot_exists:
                return Decision("refuse_conflict", "the update's snapshot is missing")
            return Decision("restore", "the data the update started from is put back")
        if flags.retry:
            if release.release_id != pending["to_release_id"] or not data_disk_exists:
                return Decision(
                    "refuse_conflict",
                    "a retry boots the release whose update did not complete",
                )
            return Decision("retry", "the update boots again, its snapshot kept")
        newer = build_order(release.build_number) > build_order(state.build_number)
        if release.release_id != state.release_id and newer:
            if not snapshot_exists:
                return Decision("refuse_conflict", "the update's snapshot is missing")
            if release.data_layout < pending["from"]["data_layout"]:
                return Decision("refuse_downgrade", "the release's data layout is older than the snapshot's")
            return Decision(
                "update" if release.data_layout == pending["from"]["data_layout"] else "migrate",
                "the unfinished update's data is restored before starting the newer release",
            )
        return Decision(
            "refuse_pending",
            f"the start of {pending['to_release_id']} never reached boot_completed: "
            "retry it, restore its snapshot, or reset",
        )
    if flags.retry or flags.restore_snapshot:
        return Decision("refuse_conflict", "no update is pending")
    if not data_disk_exists:
        return Decision("create", "the instance has no data disk")
    if release.release_id == state.release_id:
        if release.system_disk_sha256 != state.system_disk_sha256:
            return Decision("refuse_conflict", "one release id names two system disks")
        return Decision("boot", "the release the data disk was last started with")
    if build_order(release.build_number) < build_order(state.build_number):
        return Decision(
            "refuse_downgrade",
            f"build {release.build_number} is older than build {state.build_number}, "
            "which the data disk was started with: update the release, or reset",
        )
    if release.build_number == state.build_number:
        return Decision("refuse_conflict", "two releases share one build number")
    if release.data_layout == state.data_layout:
        return Decision("update", "a newer release on the same data layout")
    if release.data_layout > state.data_layout:
        return Decision("migrate", "a newer release on a newer data layout")
    return Decision("refuse_downgrade", "the release's data layout is older than the disk's")


# --- applying it -----------------------------------------------------------------


def snapshot_name(release_id: str) -> str:
    return SNAPSHOT_PREFIX + release_id


def clone_file(source: Path, destination: Path) -> None:
    """An APFS clone: instant, and it costs only the blocks that diverge."""
    result = subprocess.run(
        ["cp", "-c", str(source), str(destination)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        destination.unlink(missing_ok=True)
        raise InstanceError(
            f"cannot clone {source.name} (the instance needs an APFS volume): "
            f"{result.stderr.strip()}"
        )


def _replace_with_clone(source: Path, destination: Path) -> None:
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.unlink(missing_ok=True)
    clone_file(source, temporary)
    os.replace(temporary, destination)


# The lock that serializes what changes an installation's identity: a start's
# preparation of the data disk and of its identity (the emulator takes it) and
# this module's reset, restore and create (darwin/device/block/android_identity.hpp
# owns the file). It is an exclusive flock on `identity.lock`, which is never
# removed.
LOCK_FILE = "identity.lock"
LOCK_SECONDS = 60.0
# The locks this process holds, by instance: flock locks the open file, so a
# second open of the same file by the same process would wait for the first.
_HELD: dict[str, int] = {}


@contextlib.contextmanager
def installation_lock(instance: Path, limit: float = LOCK_SECONDS):
    """Hold the installation's lock, waiting up to `limit` seconds for another
    process that holds it; re-entrant within this process."""
    key = os.path.abspath(instance)
    if key in _HELD:
        yield
        return
    instance.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(instance / LOCK_FILE, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        deadline = time.monotonic() + limit
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise InstanceError(f"the installation stayed locked for {limit:g} s: {instance / LOCK_FILE}")
                time.sleep(0.02)
        _HELD[key] = descriptor
        try:
            yield
        finally:
            del _HELD[key]
            fcntl.flock(descriptor, fcntl.LOCK_UN)
    finally:
        os.close(descriptor)


def remove_identity(instance: Path) -> None:
    """Delete the installation's identity and what a cut-short write of it left,
    so the next start of the data disk makes a new one."""
    for entry in instance.glob(IDENTITY_FILE + "*"):
        entry.unlink(missing_ok=True)


def _new_state(release: Release, when: str) -> State:
    identity = release.identity()
    return State(
        data_layout=release.data_layout,
        created_with=release.release_id,
        last_booted=when,
        **identity,
    )


# Migrations of the data disk between layouts, each rewriting the disk's
# partition table inside the reserved gap, never moving userdata. There is one
# layout, so there is none; a release with a newer layout and no migration
# does not start.
MIGRATIONS: dict[tuple[int, int], Callable[[Path], None]] = {}


def apply(
    instance: Path,
    release: Release,
    decision: Decision,
    *,
    now: Callable[[], datetime.datetime] | None = None,
) -> State:
    """Carry out `decision` before the guest starts and return the new state.

    Every step leaves the instance in a state `decide` handles: the snapshot
    exists before instance.json records it, and instance.json is replaced
    atomically. What deletes or replaces the data disk (reset, restore and
    create) runs under the installation's lock, so that it neither interleaves
    with a start's preparation of the disk and its identity nor leaves an
    identity that a start made meanwhile. The decision was made before the
    lock was taken, so a create or restore is checked again under it: another
    start may have made what this one would delete."""
    if decision.action in ("reset", "restore", "create"):
        with installation_lock(instance):
            _revalidate(instance, decision)
            return _apply(instance, release, decision, now=now)
    return _apply(instance, release, decision, now=now)


def _revalidate(instance: Path, decision: Decision) -> None:
    """Refuse a create or restore whose ground has changed since it was
    decided, before it deletes anything. A reset is the person's explicit
    request and deletes whatever is there."""
    if decision.action == "create" and (instance / DATA_DISK).is_file():
        raise InstanceError(
            "a data disk appeared while this start waited for the installation "
            "(another start created it): start again"
        )
    if decision.action == "restore":
        state = read_state(instance)
        pending = None if state is None else state.pending_update
        if pending is None or not (instance / pending["snapshot"]).is_file():
            raise InstanceError(
                "the unfinished update ended while this start waited for the installation: start again"
            )


def _apply(
    instance: Path,
    release: Release,
    decision: Decision,
    *,
    now: Callable[[], datetime.datetime] | None = None,
) -> State:
    when = _timestamp(now)
    data = instance / DATA_DISK
    action = decision.action
    if action.startswith("refuse"):
        raise InstanceError(decision.reason)
    if action == "reset":
        for entry in [data, instance / STATE_FILE, *instance.glob(SNAPSHOT_PREFIX + "*")]:
            entry.unlink(missing_ok=True)
        remove_identity(instance)
        action = "create"
    if action == "restore":
        state = read_state(instance)
        assert state is not None and state.pending_update is not None
        snapshot = instance / state.pending_update["snapshot"]
        _replace_with_clone(snapshot, data)
        origin = state.pending_update["from"]
        restored = State(
            data_layout=origin["data_layout"],
            created_with=state.created_with,
            release_id=origin["release_id"],
            build_fingerprint=origin["build_fingerprint"],
            build_incremental=origin["build_incremental"],
            system_disk_sha256=origin["system_disk_sha256"],
            last_booted=origin["last_booted"],
            last_completed=state.last_completed,
            pending_update=None,
        )
        write_state(instance, restored)
        snapshot.unlink()
        # The data is back; the release now decides afresh.
        again = decide(restored, True, False, release)
        return apply(instance, release, again, now=now)
    if action == "create":
        instance.mkdir(parents=True, exist_ok=True)
        data.unlink(missing_ok=True)
        # A disk made at the next start is a new installation, whatever
        # identity an earlier one left beside it.
        remove_identity(instance)
        state = _new_state(release, when)
        write_state(instance, state)
        return state
    state = read_state(instance)
    assert state is not None
    if action == "boot":
        updated = State(**{**state.__dict__, "last_booted": when})
        write_state(instance, updated)
        return updated
    if action == "retry":
        updated = State(**{**state.__dict__, "last_booted": when})
        write_state(instance, updated)
        return updated
    assert action in ("update", "migrate")
    if state.pending_update is not None:
        return apply(
            instance, release, Decision("restore", "restore the data before the unfinished update"), now=now
        )
    name = snapshot_name(release.release_id)
    _replace_with_clone(data, instance / name)
    if action == "migrate":
        migration = MIGRATIONS.get((state.data_layout, release.data_layout))
        if migration is None:
            raise InstanceError(
                f"no migration from data layout {state.data_layout} to "
                f"{release.data_layout}; reset the instance"
            )
    pending = {
        "to_release_id": release.release_id,
        "snapshot": name,
        "from": {
            "release_id": state.release_id,
            "build_fingerprint": state.build_fingerprint,
            "build_incremental": state.build_incremental,
            "system_disk_sha256": state.system_disk_sha256,
            "data_layout": state.data_layout,
            "last_booted": state.last_booted,
        },
    }
    updated = State(
        data_layout=state.data_layout,
        created_with=state.created_with,
        last_booted=when,
        last_completed=state.last_completed,
        pending_update=pending,
        **release.identity(),
    )
    write_state(instance, updated)
    if action == "migrate":
        migration(data)
        updated = State(**{**updated.__dict__, "data_layout": release.data_layout})
        write_state(instance, updated)
    return updated


def booted(instance: Path, release: Release) -> State:
    """Record that a start of `release` reached boot_completed.

    Call it at the moment the host first sees the guest boot. A pending update
    counts as applied from then on: it ends, its snapshot is deleted, and a
    newer release updates from the data as it is, with a snapshot of its own.
    Restoring the snapshot is for an update that never booted. An instance with
    no pending update is left as it is."""
    state = read_state(instance)
    if state is None or state.release_id != release.release_id:
        raise InstanceError("the instance was not started with this release")
    if state.pending_update is None:
        return state
    snapshot = state.pending_update["snapshot"]
    updated = State(**{**state.__dict__, "pending_update": None})
    write_state(instance, updated)
    (instance / snapshot).unlink(missing_ok=True)
    return updated


def complete(
    instance: Path,
    release: Release,
    *,
    now: Callable[[], datetime.datetime] | None = None,
) -> State:
    """Record that `release` reached boot_completed and shut down cleanly
    (`last_completed`). A pending update that `booted` did not end is over: its
    snapshot is deleted."""
    state = read_state(instance)
    if state is None or state.release_id != release.release_id:
        raise InstanceError("the instance was not started with this release")
    snapshot = state.pending_update["snapshot"] if state.pending_update else None
    updated = State(**{
        **state.__dict__,
        "last_completed": {
            "release_id": release.release_id,
            "build_incremental": str(release.build_number),
            "at": _timestamp(now),
        },
        "pending_update": None,
    })
    write_state(instance, updated)
    if snapshot is not None:
        (instance / snapshot).unlink(missing_ok=True)
    return updated
