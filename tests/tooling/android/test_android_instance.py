#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The rules for starting a release on a guest instance.

The decision vectors in android/lineage-24/support/instance/update-decisions.json
are the contract every implementation (this tool, the Andarium app) must
reproduce; this test runs them through tools/android_instance.py and exercises
the file operations that apply a decision.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import datetime
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("android_instance", ROOT / "tools/android_instance.py")
assert SPEC is not None and SPEC.loader is not None
android_instance = importlib.util.module_from_spec(SPEC)
sys.modules["android_instance"] = android_instance
SPEC.loader.exec_module(android_instance)
VECTORS = ROOT / "android/lineage-24/support/instance/update-decisions.json"
NOW = datetime.datetime(2026, 11, 1, 12, 0, 0, tzinfo=datetime.timezone.utc)


def release(
    release_id: str = "rel-b", build: int = 200, sha: str = "b" * 64, layout: int = 1
) -> "android_instance.Release":
    return android_instance.Release(
        release_id=release_id,
        build_fingerprint=f"fixture/{release_id}",
        build_number=build,
        security_patch="2026-10-05",
        system_disk_sha256=sha,
        data_layout=layout,
        minimum_gib=4,
        default_gib=8,
        maximum_gib=64,
    )


def state_for(
    vector: dict | None, *, snapshot: str | None = None
) -> "android_instance.State | None":
    if vector is None:
        return None
    pending = None
    if vector["pending_to_release_id"] is not None:
        pending = {
            "to_release_id": vector["pending_to_release_id"],
            "snapshot": snapshot or android_instance.snapshot_name(vector["pending_to_release_id"]),
            "from": {
                "release_id": "rel-0",
                "build_fingerprint": "fixture/rel-0",
                "build_incremental": "50",
                "system_disk_sha256": "0" * 64,
                "data_layout": 1,
                "last_booted": "2026-10-01T00:00:00Z",
            },
        }
    return android_instance.State(
        data_layout=vector["data_layout"],
        created_with="rel-0",
        release_id=vector["release_id"],
        build_fingerprint=f"fixture/{vector['release_id']}",
        build_incremental=str(vector["build_number"]),
        system_disk_sha256=vector["system_disk_sha256"],
        last_booted="2026-10-02T00:00:00Z",
        pending_update=pending,
    )


class DecisionVectors(unittest.TestCase):
    def test_every_vector_is_reproduced(self) -> None:
        document = json.loads(VECTORS.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], 1)
        seen = set()
        for case in document["cases"]:
            with self.subTest(case["name"]):
                flags = android_instance.Flags(**{name: True for name in case["flags"]})
                decision = android_instance.decide(
                    state_for(case["instance"]),
                    case["data_disk"],
                    case["snapshot"],
                    release(**{
                        "release_id": case["release"]["release_id"],
                        "build": case["release"]["build_number"],
                        "sha": case["release"]["system_disk_sha256"],
                        "layout": case["release"]["data_layout"],
                    }),
                    flags,
                )
                self.assertEqual(decision.action, case["expect"])
                self.assertIn(case["expect"], android_instance.ACTIONS)
                seen.add(case["expect"])
        # The vectors exercise every action there is.
        self.assertEqual(seen, set(android_instance.ACTIONS))

    def test_the_legacy_range_is_the_vectors(self) -> None:
        document = json.loads(VECTORS.read_text(encoding="utf-8"))
        self.assertEqual(
            document["legacy_build_numbers"],
            {
                "first": android_instance.LEGACY_BUILD_FIRST,
                "last": android_instance.LEGACY_BUILD_LAST,
            },
        )


class BuildOrder(unittest.TestCase):
    def test_the_old_counter_is_older_than_every_other_number(self) -> None:
        first = android_instance.LEGACY_BUILD_FIRST
        last = android_instance.LEGACY_BUILD_LAST
        order = android_instance.build_order
        ascending = [
            first, first + 1, 2026110805, 2026110817, last,
            1, 26, 27, 10_000, first - 1, last + 1, 9_999_999_999,
        ]
        keys = [order(number) for number in ascending]
        self.assertEqual(len(set(keys)), len(keys))
        for position, key in enumerate(keys):
            for other in keys[position + 1:]:
                self.assertLess(key, other)

    def test_the_range_has_two_edges(self) -> None:
        order = android_instance.build_order
        self.assertEqual((android_instance.LEGACY_BUILD_FIRST, android_instance.LEGACY_BUILD_LAST),
                         (2026110800, 2026110899))
        # Both edges are in the range; the numbers just outside it are not.
        self.assertEqual(order(2026110800)[0], order(2026110899)[0])
        self.assertNotEqual(order(2026110799)[0], order(2026110800)[0])
        self.assertNotEqual(order(2026110900)[0], order(2026110899)[0])
        self.assertLess(order(2026110800), order(2026110799))
        self.assertLess(order(2026110899), order(2026110900))
        self.assertGreater(order(2026110799), order(2026110899))


class TransitionVectors(unittest.TestCase):
    def test_every_transition_is_reproduced(self) -> None:
        document = json.loads(VECTORS.read_text(encoding="utf-8"))
        self.assertTrue(document["transitions"])
        for case in document["transitions"]:
            with self.subTest(case["name"]), tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
                instance = Path(temporary) / "instance"
                state = state_for(case["instance"])
                if state is not None:
                    android_instance.write_state(instance, state)
                    (instance / "data.img").write_bytes(b"guest data")
                if case["snapshot"]:
                    (instance / "data.img.pre-rel-b").write_bytes(b"snapshot")
                target = release(**{
                    "release_id": case["release"]["release_id"],
                    "build": case["release"]["build_number"],
                    "sha": case["release"]["system_disk_sha256"],
                    "layout": case["release"]["data_layout"],
                })
                expect = case["expect"]
                if case["event"] == "boot_completed" and expect == "refused":
                    with self.assertRaises(android_instance.InstanceError):
                        android_instance.booted(instance, target)
                    continue
                if case["event"] == "boot_completed":
                    android_instance.booted(instance, target)
                after = android_instance.read_state(instance)
                self.assertEqual(
                    after.pending_update["to_release_id"] if after.pending_update else None,
                    expect["pending_to_release_id"],
                )
                self.assertEqual((instance / "data.img.pre-rel-b").exists(), expect["snapshot"])
                self.assertEqual((instance / "data.img").read_bytes(), b"guest data")


class InstanceFiles(unittest.TestCase):
    def apply(self, instance: Path, target, **flags):
        state = android_instance.read_state(instance)
        decision = android_instance.decide(
            state,
            (instance / "data.img").is_file(),
            any(instance.glob("data.img.pre-*")),
            target,
            android_instance.Flags(**flags),
        )
        return decision, android_instance.apply(instance, target, decision, now=lambda: NOW)

    def test_the_state_round_trips_and_is_strict(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            _decision, state = self.apply(instance, release("rel-a", 100, "a" * 64))
            self.assertEqual(android_instance.read_state(instance), state)
            document = json.loads((instance / "instance.json").read_text())
            self.assertEqual(
                set(document),
                {"schema", "data_layout", "release_id", "build_fingerprint",
                 "build_incremental", "system_disk_sha256", "created_with",
                 "last_booted", "last_completed", "pending_update"},
            )
            self.assertEqual(document["last_booted"], "2026-11-01T12:00:00Z")
            self.assertEqual(document["build_incremental"], "100")
            for damage in (
                lambda value: value.pop("created_with"),
                lambda value: value.update(schema=2),
                lambda value: value.update(build_incremental="eng.x"),
                lambda value: value.update(data_layout=0),
                lambda value: value.update(last_completed={"release_id": "x"}),
                lambda value: value.update(extra=1),
            ):
                broken = json.loads(json.dumps(document))
                damage(broken)
                with self.assertRaises(android_instance.InstanceError):
                    android_instance.parse_state(broken)
            (instance / "instance.json").write_text("{", encoding="utf-8")
            with self.assertRaises(android_instance.InstanceError):
                android_instance.read_state(instance)

    def test_an_update_snapshots_with_a_clone_and_records_where_it_came_from(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            decision, state = self.apply(instance, release("rel-b", 200, "b" * 64))
            self.assertEqual(decision.action, "update")
            self.assertEqual(state.release_id, "rel-b")
            self.assertEqual(state.build_incremental, "200")
            self.assertEqual(state.created_with, "rel-a")
            self.assertEqual(state.pending_update["from"]["release_id"], "rel-a")
            self.assertEqual(state.pending_update["from"]["build_incremental"], "100")
            self.assertEqual(
                (instance / "data.img.pre-rel-b").read_bytes(), b"guest data"
            )
            # The snapshot is a clone: changing the disk leaves it alone.
            (instance / "data.img").write_bytes(b"changed")
            self.assertEqual(
                (instance / "data.img.pre-rel-b").read_bytes(), b"guest data"
            )
            done = android_instance.complete(
                instance, release("rel-b", 200, "b" * 64), now=lambda: NOW
            )
            self.assertIsNone(done.pending_update)
            self.assertEqual(done.last_completed["release_id"], "rel-b")
            self.assertEqual(done.last_completed["at"], "2026-11-01T12:00:00Z")
            self.assertFalse((instance / "data.img.pre-rel-b").exists())
            with self.assertRaises(android_instance.InstanceError):
                android_instance.complete(instance, release("rel-c", 300, "c" * 64))

    def test_an_interrupted_snapshot_is_replaced_not_trusted(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            (instance / "data.img.pre-rel-b").write_bytes(b"stale")
            (instance / "data.img.pre-rel-b.tmp").write_bytes(b"torn")
            # No pending update is recorded, so the stale file does not block.
            decision, _state = self.apply(instance, release("rel-b", 200, "b" * 64))
            self.assertEqual(decision.action, "update")
            self.assertEqual((instance / "data.img.pre-rel-b").read_bytes(), b"guest data")

    def test_restore_returns_to_the_previous_identity_and_updates_again(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            self.apply(instance, release("rel-b", 200, "b" * 64))
            (instance / "data.img").write_bytes(b"damaged")
            decision, state = self.apply(
                instance, release("rel-b", 200, "b" * 64), restore_snapshot=True
            )
            self.assertEqual(decision.action, "restore")
            self.assertEqual((instance / "data.img").read_bytes(), b"guest data")
            # The release then updates the restored data afresh.
            self.assertEqual(state.release_id, "rel-b")
            self.assertEqual(state.pending_update["from"]["release_id"], "rel-a")
            self.assertEqual(state.created_with, "rel-a")
            self.assertEqual(
                (instance / "data.img.pre-rel-b").read_bytes(), b"guest data"
            )

    def test_a_newer_release_restores_an_unfinished_update_before_starting(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            self.apply(instance, release("rel-b", 200, "b" * 64))
            (instance / "data.img").write_bytes(b"partially upgraded data")
            decision, state = self.apply(instance, release("rel-d", 10000, "d" * 64))
            self.assertEqual(decision.action, "update")
            self.assertEqual((instance / "data.img").read_bytes(), b"guest data")
            self.assertEqual((instance / "data.img.pre-rel-d").read_bytes(), b"guest data")
            self.assertFalse((instance / "data.img.pre-rel-b").exists())
            self.assertEqual(state.release_id, "rel-d")
            self.assertEqual(state.created_with, "rel-a")
            self.assertEqual(state.pending_update["from"]["release_id"], "rel-a")
            self.assertEqual(state.pending_update["to_release_id"], "rel-d")
            android_instance.complete(instance, release("rel-d", 10000, "d" * 64))
            self.assertFalse((instance / "data.img.pre-rel-d").exists())

    def test_an_update_that_booted_is_not_rolled_back_by_the_next_release(self) -> None:
        # The sequence that lost a week of user data: r13 settles, r15 updates
        # and boots, the host is killed before Android powers off, r15 starts
        # again, and r16 arrives.
        r13, r15, r16 = (
            release("r13", 13, "a" * 64), release("r15", 15, "b" * 64), release("r16", 16, "c" * 64),
        )
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.assertEqual(self.apply(instance, r13)[0].action, "create")
            (instance / "data.img").write_bytes(b"data written on r13")
            android_instance.complete(instance, r13)
            self.assertEqual(self.apply(instance, r15)[0].action, "update")
            self.assertTrue((instance / "data.img.pre-r15").exists())
            # r15 reaches boot_completed: the update is applied.
            android_instance.booted(instance, r15)
            self.assertIsNone(android_instance.read_state(instance).pending_update)
            self.assertFalse((instance / "data.img.pre-r15").exists())
            (instance / "data.img").write_bytes(b"r15 upgraded data and a week of user data")
            # The host dies without a clean power-off; the next start is an
            # ordinary one, not a retry.
            self.assertEqual(self.apply(instance, r15)[0].action, "boot")
            (instance / "data.img").write_bytes(b"r15 upgraded data and a month of user data")
            decision, state = self.apply(instance, r16)
            self.assertEqual(decision.action, "update")
            self.assertEqual((instance / "data.img").read_bytes(), b"r15 upgraded data and a month of user data")
            # r16 takes its own snapshot of the data as it is, and r15's is gone.
            self.assertEqual(
                (instance / "data.img.pre-r16").read_bytes(), b"r15 upgraded data and a month of user data"
            )
            self.assertFalse((instance / "data.img.pre-r15").exists())
            self.assertEqual(state.pending_update["from"]["release_id"], "r15")

    def test_an_update_that_never_booted_is_still_restored_by_the_next_release(self) -> None:
        r13, r15, r16 = (
            release("r13", 13, "a" * 64), release("r15", 15, "b" * 64), release("r16", 16, "c" * 64),
        )
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, r13)
            (instance / "data.img").write_bytes(b"data written on r13")
            android_instance.complete(instance, r13)
            self.apply(instance, r15)
            (instance / "data.img").write_bytes(b"r15 half upgraded data")
            decision, _ = self.apply(instance, r16)
            self.assertEqual(decision.action, "update")
            self.assertEqual((instance / "data.img").read_bytes(), b"data written on r13")

    def test_a_clean_shutdown_after_boot_records_the_completion(self) -> None:
        r13, r15 = release("r13", 13, "a" * 64), release("r15", 15, "b" * 64)
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, r13)
            self.apply(instance, r15)
            android_instance.booted(instance, r15)
            state = android_instance.complete(instance, r15, now=lambda: NOW)
            self.assertIsNone(state.pending_update)
            self.assertEqual(state.last_completed["release_id"], "r15")

    def test_a_missing_snapshot_preserves_the_unfinished_updates_data(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            self.apply(instance, release("rel-b", 200, "b" * 64))
            (instance / "data.img").write_bytes(b"partially upgraded data")
            (instance / "data.img.pre-rel-b").unlink()
            before = (instance / "instance.json").read_bytes()
            with self.assertRaises(android_instance.InstanceError):
                self.apply(instance, release("rel-d", 10000, "d" * 64))
            self.assertEqual((instance / "instance.json").read_bytes(), before)
            self.assertEqual((instance / "data.img").read_bytes(), b"partially upgraded data")

    def test_restoring_before_a_downgrade_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            self.apply(instance, release("rel-b", 200, "b" * 64))
            # A release older than the restored data's: the restore happens,
            # then the release is refused as the downgrade it is.
            with self.assertRaises(android_instance.InstanceError):
                self.apply(
                    instance, release("rel-0", 50, "0" * 64), restore_snapshot=True
                )

    def test_migration_needs_a_registered_step(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            with self.assertRaises(android_instance.InstanceError):
                self.apply(instance, release("rel-b", 200, "b" * 64, layout=2))
            steps = []
            with mock.patch.dict(
                android_instance.MIGRATIONS, {(1, 2): lambda disk: steps.append(disk.read_bytes())}
            ):
                decision, state = self.apply(
                    instance, release("rel-b", 200, "b" * 64, layout=2)
                )
            self.assertEqual(decision.action, "migrate")
            self.assertEqual(steps, [b"guest data"])
            self.assertEqual(state.data_layout, 2)
            self.assertEqual(state.pending_update["from"]["data_layout"], 1)

    def test_reset_removes_the_instance_and_creates_it_anew(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-b", 200, "b" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            (instance / "data.img.pre-rel-b").write_bytes(b"snapshot")
            decision, state = self.apply(instance, release("rel-a", 100, "a" * 64), reset=True)
            self.assertEqual(decision.action, "reset")
            self.assertEqual(state.release_id, "rel-a")
            self.assertEqual(state.created_with, "rel-a")
            self.assertFalse((instance / "data.img").exists())
            self.assertEqual(list(instance.glob("data.img.pre-*")), [])

    def test_the_installation_identity_follows_the_data_disk(self) -> None:
        # What the emulator keeps beside the data disk (synthetic here): an update,
        # a retry, a restore and a start leave it, and a reset or a new data disk
        # ends it, with what a cut-short write of it left.
        content = (
            '{"ethernet_mac": "02:aa:bb:cc:dd:ee", "schema": 1, '
            '"serialno": "0123456789abcdef"}\n'
        )
        with tempfile.TemporaryDirectory() as temporary:
            instance = Path(temporary) / "instance"
            first = release("rel-a", build=100, sha="a" * 64)
            second = release("rel-b", build=200)
            android_instance.apply(instance, first, android_instance.decide(None, False, False, first), now=lambda: NOW)
            (instance / "data.img").write_bytes(b"android data")
            identity = instance / android_instance.IDENTITY_FILE
            identity.write_text(content, encoding="utf-8")

            def unchanged(step: str) -> None:
                self.assertEqual(identity.read_text(encoding="utf-8"), content, step)

            def decide(rel, **flags):
                state = android_instance.read_state(instance)
                return android_instance.decide(
                    state, (instance / "data.img").exists(),
                    any(instance.glob(android_instance.SNAPSHOT_PREFIX + "*")), rel,
                    android_instance.Flags(**flags))

            android_instance.apply(instance, second, decide(second), now=lambda: NOW)
            unchanged("an update")
            android_instance.apply(instance, second, decide(second, retry=True), now=lambda: NOW)
            unchanged("a retry")
            # The restore puts the data back and then updates again, as the rules say.
            android_instance.apply(instance, second, decide(second, restore_snapshot=True), now=lambda: NOW)
            unchanged("a restore")
            android_instance.complete(instance, second, now=lambda: NOW)
            android_instance.apply(instance, second, decide(second), now=lambda: NOW)
            unchanged("a start")

            leftover = instance / (android_instance.IDENTITY_FILE + ".tmp-123")
            leftover.write_text("half", encoding="utf-8")
            android_instance.apply(instance, second, decide(second, reset=True), now=lambda: NOW)
            self.assertFalse(identity.exists() or leftover.exists(), "a reset")

            (instance / "data.img").write_bytes(b"android data")
            identity.write_text(content, encoding="utf-8")
            (instance / "data.img").unlink()
            decision = decide(second)
            self.assertEqual(decision.action, "create")
            android_instance.apply(instance, second, decision, now=lambda: NOW)
            self.assertFalse(identity.exists(), "a data disk that is gone")

    def test_a_reset_waits_for_a_start_that_holds_the_installation(self) -> None:
        # A start prepares the data disk and makes its identity under the
        # installation's lock; a reset that interleaves would leave the identity
        # the start made after it deleted the one before. The reset runs after.
        content = (
            '{"ethernet_mac": "02:aa:bb:cc:dd:ee", "schema": 1, '
            '"serialno": "0123456789abcdef"}\n'
        )
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            first = release("rel-a", build=100, sha="a" * 64)
            android_instance.apply(instance, first, android_instance.decide(None, False, False, first), now=lambda: NOW)
            (instance / "data.img").write_bytes(b"android data")
            identity = instance / android_instance.IDENTITY_FILE
            holding = threading.Event()

            def start() -> None:
                # The emulator's side: its own open of the lock file, as another process has.
                descriptor = os.open(instance / android_instance.LOCK_FILE, os.O_RDWR | os.O_CREAT, 0o600)
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX)
                    holding.set()
                    time.sleep(0.4)
                    identity.write_text(content, encoding="utf-8")
                finally:
                    os.close(descriptor)

            starter = threading.Thread(target=start)
            starter.start()
            self.assertTrue(holding.wait(10))
            state = android_instance.read_state(instance)
            reset = android_instance.decide(state, True, False, first, android_instance.Flags(reset=True))
            began = time.monotonic()
            android_instance.apply(instance, first, reset, now=lambda: NOW)
            starter.join()
            self.assertGreaterEqual(time.monotonic() - began, 0.3, "the reset did not wait for the start")
            self.assertFalse(identity.exists(), "the reset left the identity a start made meanwhile")
            self.assertTrue((instance / android_instance.LOCK_FILE).exists(), "the lock file was removed")

    def test_the_lock_is_waited_for_with_a_limit_and_is_reentrant(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            with android_instance.installation_lock(instance):
                # This process's own second use does not wait for itself.
                with android_instance.installation_lock(instance, limit=0.1):
                    pass
                # Another process's (here: another open file) does.
                other = os.open(instance / android_instance.LOCK_FILE, os.O_RDWR)
                try:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    os.close(other)
            descriptor = os.open(instance / android_instance.LOCK_FILE, os.O_RDWR)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                began = time.monotonic()
                with self.assertRaises(android_instance.InstanceError):
                    with android_instance.installation_lock(instance, limit=0.2):
                        self.fail("a held installation was locked twice")
                self.assertLess(time.monotonic() - began, 5)
            finally:
                os.close(descriptor)
            # Released, it is free again.
            with android_instance.installation_lock(instance, limit=0.2):
                pass

    def test_a_create_decided_before_another_start_made_the_disk_deletes_nothing(self) -> None:
        # Two launches plan "create" while the installation is absent. The first
        # applies its decision and its emulator makes the disk and the identity;
        # the second then takes the lock and must not delete what the first made.
        content = (
            '{"ethernet_mac": "02:aa:bb:cc:dd:ee", "schema": 1, '
            '"serialno": "0123456789abcdef"}\n'
        )
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            rel = release("rel-a", build=100, sha="a" * 64)
            one = android_instance.decide(None, False, False, rel)
            two = android_instance.decide(None, False, False, rel)
            self.assertEqual((one.action, two.action), ("create", "create"))
            android_instance.apply(instance, rel, one, now=lambda: NOW)
            (instance / "data.img").write_bytes(b"the first start's disk")
            identity = instance / android_instance.IDENTITY_FILE
            identity.write_text(content, encoding="utf-8")
            state_before = (instance / android_instance.STATE_FILE).read_bytes()
            with self.assertRaises(android_instance.InstanceError):
                android_instance.apply(instance, rel, two, now=lambda: NOW)
            self.assertEqual((instance / "data.img").read_bytes(), b"the first start's disk")
            self.assertEqual(identity.read_text(encoding="utf-8"), content)
            self.assertEqual((instance / android_instance.STATE_FILE).read_bytes(), state_before)
            # What is decided afresh is a plain boot, and a person's reset still deletes.
            state = android_instance.read_state(instance)
            self.assertEqual(android_instance.decide(state, True, False, rel).action, "boot")
            reset = android_instance.decide(state, True, False, rel, android_instance.Flags(reset=True))
            android_instance.apply(instance, rel, reset, now=lambda: NOW)
            self.assertFalse((instance / "data.img").exists() or identity.exists())

    def test_a_restore_decided_before_the_update_ended_changes_nothing(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            first = release("rel-a", build=100, sha="a" * 64)
            second = release("rel-b", build=200)
            android_instance.apply(instance, first, android_instance.decide(None, False, False, first), now=lambda: NOW)
            (instance / "data.img").write_bytes(b"data before the update")
            state = android_instance.read_state(instance)
            android_instance.apply(instance, second, android_instance.decide(state, True, False, second), now=lambda: NOW)
            (instance / "data.img").write_bytes(b"data after the update")
            state = android_instance.read_state(instance)
            restore = android_instance.decide(state, True, True, second, android_instance.Flags(restore_snapshot=True))
            self.assertEqual(restore.action, "restore")
            # The update boots to completion before the restore takes the lock.
            android_instance.complete(instance, second, now=lambda: NOW)
            with self.assertRaises(android_instance.InstanceError):
                android_instance.apply(instance, second, restore, now=lambda: NOW)
            self.assertEqual((instance / "data.img").read_bytes(), b"data after the update")

    def test_a_volume_without_clones_is_a_refusal(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            instance = Path(temporary) / "instance"
            self.apply(instance, release("rel-a", 100, "a" * 64))
            (instance / "data.img").write_bytes(b"guest data")
            failed = android_instance.subprocess.CompletedProcess([], 1, "", "not supported")
            with mock.patch.object(android_instance.subprocess, "run", return_value=failed):
                with self.assertRaises(android_instance.InstanceError):
                    self.apply(instance, release("rel-b", 200, "b" * 64))
            # Nothing was recorded: the instance still runs the old release.
            self.assertEqual(android_instance.read_state(instance).release_id, "rel-a")

    def test_release_identity_comes_from_manifest_and_layout(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-instance-") as temporary:
            root = Path(temporary)
            (root / "guest").mkdir()
            manifest = {
                "release_id": "rel-a",
                "android": {
                    "fingerprint": "fp", "build_number": 7, "security_patch": "2026-10-05",
                },
            }
            layout = {
                "system_disk": {"sha256": "a" * 64},
                "data_disk": {
                    "layout_version": 1,
                    "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64},
                },
            }
            (root / "release-manifest.json").write_text(json.dumps(manifest))
            (root / "guest/partition-layout.json").write_text(json.dumps(layout))
            identity = android_instance.read_release(root)
            self.assertEqual(identity.identity()["build_incremental"], "7")
            self.assertEqual((identity.minimum_gib, identity.default_gib, identity.maximum_gib), (4, 8, 64))
            for key, value in (("build_number", 0), ("build_number", "7"), ("release_id", "Bad Id")):
                broken = json.loads(json.dumps(manifest))
                if key == "release_id":
                    broken[key] = value
                else:
                    broken["android"][key] = value
                (root / "release-manifest.json").write_text(json.dumps(broken))
                with self.assertRaises(android_instance.InstanceError):
                    android_instance.read_release(root)
            (root / "release-manifest.json").write_text("{}")
            with self.assertRaises(android_instance.InstanceError):
                android_instance.read_release(root)


if __name__ == "__main__":
    unittest.main()
