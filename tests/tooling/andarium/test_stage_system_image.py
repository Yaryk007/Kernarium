#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/stage-system-image.py stages a checked, accepted release.

A fake release carries the files and manifest fields the script reads: the
system disk (any bytes), the partition layout that declares the disk's size
and sha256, the kernel command line and the manifest inventory.

With --unsigned-proof, for developer builds, it stages an unsigned proof image
instead: a fake proof is a real small system disk (the tracked disk policy's
partitions with synthetic contents, from the Android release fixture) and the
manifest a proof producer writes. The staged manifest stays a proof's and
gains the fields the application reads that a proof lacks.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import atexit
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests/tooling/android"))
import magisk_fixture  # noqa: E402

SCRIPT = REPOSITORY / "Andarium/scripts/stage-system-image.py"
PACKAGER = REPOSITORY / "Kernarium/scripts/package-ios-frameworks.sh"
DISK_FIXTURE = REPOSITORY / "tests/support/android/android_release_fixture.py"
STAGED_PROOF_GOLDEN = REPOSITORY / "tests/unit/andarium/fixtures/staged-unsigned-proof-manifest.json"

RELEASE_ID = "20261031-los24-r4"
FAMILY = "lineage-24-mainline-6.18-4k"
DIRECTORY = f"lineage24-mainline6.18-4k-{RELEASE_ID}"
SYSTEM_DISK = b"GPT-and-partitions" * 1000


def make_release(root: pathlib.Path, **overrides) -> pathlib.Path:
    release = root / DIRECTORY
    (release / "guest").mkdir(parents=True)
    (release / "boot").mkdir()
    disk = overrides.get("disk", SYSTEM_DISK)
    (release / "guest/system.img").write_bytes(disk)
    (release / "boot/kernel-command-line.txt").write_text("init=/init console=ttyS0\n")
    layout = json.dumps({
        "schema_version": overrides.get("layout_schema", 2),
        "sector_size": 512,
        "system_disk": {
            "size_bytes": overrides.get("layout_bytes", len(disk)),
            "sha256": overrides.get("layout_sha256", hashlib.sha256(disk).hexdigest()),
        },
        "data_disk": {"filesystem": "ext4"},
    })
    (release / "guest/partition-layout.json").write_text(layout)
    inventory = [
        {"path": "boot/kernel-command-line.txt", "role": "kernel-command-line",
         "size": (release / "boot/kernel-command-line.txt").stat().st_size},
        {"path": "guest/partition-layout.json", "role": "partition-layout", "size": len(layout)},
        {"path": "guest/system.img", "role": overrides.get("disk_role", "system-disk"),
         "size": overrides.get("disk_size", len(disk))},
        {"path": "kernel/vmlinux", "role": "kernel-elf", "size": 1},
    ]
    manifest = {
        "acceptance": overrides.get("acceptance", "accepted"),
        "android": {"major": 17},
        "kernel": {"page_size": overrides.get("page_size", 4096), "release": "6.18.54-gabc"},
        "product_family": overrides.get("family", FAMILY),
        "release_id": overrides.get("release_id", RELEASE_ID),
        "schema_version": 6,
        "inventory": inventory,
    }
    (release / "release-manifest.json").write_text(json.dumps(manifest))
    return release


def make_acceptance(path: pathlib.Path, **overrides) -> pathlib.Path:
    values = {
        "LINEAGE24_ACCEPTED_RELEASE_ID": RELEASE_ID,
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME": DIRECTORY,
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": FAMILY,
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": "17",
    }
    values.update(overrides)
    lines = [f"{key}='{value}'" if not value.isdigit() else f"{key}={value}" for key, value in values.items()]
    path.write_text("\n".join(lines) + "\n")
    return path


PROOF_ID = "unsigned-proof-ff1"
PROOF_DIRECTORY = f"lineage24-mainline6.18-4k-{PROOF_ID}"
KERNEL_RELEASE = "6.18.99-gfeedface0000"
_PROOF_TEMPLATE: list[pathlib.Path] = []


def proof_template() -> pathlib.Path:
    """A real small system disk with its layout and command line, made once."""
    if not _PROOF_TEMPLATE:
        scratch = pathlib.Path(tempfile.mkdtemp(prefix="proof-template."))
        atexit.register(shutil.rmtree, scratch, ignore_errors=True)
        subprocess.run(
            [sys.executable, "-B", str(DISK_FIXTURE), "--android-root", str(REPOSITORY / "android/lineage-24"),
             "--output", str(scratch)],
            check=True, capture_output=True, stdin=subprocess.DEVNULL)
        _PROOF_TEMPLATE.append(scratch / "release")
    return _PROOF_TEMPLATE[0]


def proof_manifest(digest: str | None = None) -> dict:
    """The manifest of an unsigned proof as its producer writes it: no major, no
    kernel, no inventory, no source."""
    producer = {
        "entry_point": "proof.sh", "limit": "Built from the unsigned target files.",
        "proof_for_release_id": "ff1", "target_files_sha256": "d" * 64,
    }
    if digest is not None:
        producer["system_disk_sha256"] = digest
    return {
        "acceptance": "unsigned-proof",
        "android": {
            "build_id": "CP2A.260101.001", "build_number": 29, "security_patch": "2026-10-01", "signed": False,
            "fingerprint": "Andarium/lineage_andarium/andarium:17/CP2A.260101.001/29:user/test-keys",
        },
        "directory_name": PROOF_DIRECTORY,
        "inventory": [],
        "kernel": {},
        "outer": {"dirty": False, "revision": "c" * 40},
        "producer": producer,
        "product_family": FAMILY,
        "published_at_utc": "2026-11-18T00:00:00Z",
        "release_id": PROOF_ID,
        "schema_version": 0,
        "source": {},
    }


def make_proof(root: pathlib.Path, edit=None, digest: bool = True) -> pathlib.Path:
    """A proof directory; `edit(directory, manifest)` changes it before the manifest is written."""
    proof = root / PROOF_DIRECTORY
    shutil.copytree(proof_template(), proof)
    layout = json.loads((proof / "guest/partition-layout.json").read_text())
    manifest = proof_manifest(layout["system_disk"]["sha256"] if digest else None)
    if edit is not None:
        edit(proof, manifest)
    (proof / "release-manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return proof


class Checkout:
    """A checkout of its own: the staging script, the two files it imports and
    the tracked metadata it reads, so a test chooses the line without options
    of the script. `revision` is its one commit when it is a Git repository."""

    def __init__(self, script: pathlib.Path, revision: str | None) -> None:
        self.script = script
        self.revision = revision


def make_checkout(
    root: pathlib.Path, major: int = 17, page_size: int = 4096, kernel_page_size: int = 4096,
    family: str = FAMILY, git: bool = False,
) -> Checkout:
    for relative in (
        "Andarium/scripts/stage-system-image.py", "kernel/common/artifacts/contract_checks.py",
        "android/lineage-24/tools/ubd-disk.py", "android/lineage-24/tools/verify-magisk-boot.py",
        "android/lineage-24/tools/magisk_image.py", "android/lineage-24/tools/android_image.py",
    ):
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPOSITORY / relative, root / relative)
    metadata = root / "android/lineage-24/metadata"
    metadata.mkdir(parents=True)
    (metadata / "lineage.env").write_text(
        f"LINEAGE24_ANDROID_MAJOR={major}\nLINEAGE24_KERNEL_PAGE_SIZE={page_size}\n")
    make_acceptance(metadata / "accepted-release.env", LINEAGE24_ACCEPTED_PRODUCT_FAMILY=family)
    kernel = root / "kernel/mainline-6.18/artifacts"
    kernel.mkdir(parents=True)
    (kernel / "accepted-android-4k.env").write_text(
        f"UML_KERNEL_ARTIFACT_RELEASE='{KERNEL_RELEASE}'\nUML_KERNEL_ARTIFACT_PAGE_SIZE={kernel_page_size}\n")
    revision = None
    if git:
        for command in (["init", "-q", "."], ["add", "-A"], ["commit", "-q", "-m", "fixture"]):
            subprocess.run(["git", "-C", str(root), *command], check=True, capture_output=True,
                           stdin=subprocess.DEVNULL)
        revision = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True, capture_output=True,
                                  text=True, stdin=subprocess.DEVNULL).stdout.strip()
    return Checkout(root / "Andarium/scripts/stage-system-image.py", revision)


def run_script(script: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(script), *arguments],
        capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )


def run(*arguments: str) -> subprocess.CompletedProcess:
    return run_script(SCRIPT, *arguments)


def tree(directory: pathlib.Path) -> list[str]:
    return sorted(str(path.relative_to(directory)) for path in directory.rglob("*") if path.is_file())


class StageSystemImageTests(unittest.TestCase):
    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        self.root = pathlib.Path(self._scratch.name)
        self.output = self.root / "out/andarium-system"

    def test_stages_the_four_files_of_an_explicit_release_at_their_release_paths(self):
        release = make_release(self.root / "releases")
        result = run("--release", str(release), "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            tree(self.output),
            ["boot/kernel-command-line.txt", "guest/partition-layout.json",
             "guest/system.img", "release-manifest.json"],
        )
        for relative in tree(self.output):
            self.assertEqual((self.output / relative).read_bytes(), (release / relative).read_bytes(), relative)
        self.assertIn(RELEASE_ID, result.stdout)

    def test_the_accepted_release_is_found_through_the_acceptance_file(self):
        make_release(self.root / "releases")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.output / "release-manifest.json").is_file())

    def test_a_release_that_is_not_the_accepted_one_is_refused(self):
        make_release(self.root / "releases", release_id="20260101-other")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release_id", result.stderr)
        self.assertFalse(self.output.exists())

    def test_a_missing_release_is_named(self):
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "none"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not installed", result.stderr)

    def test_each_defect_of_a_release_is_refused_and_stages_nothing(self):
        cases = {
            "acceptance": ({"acceptance": "candidate"}, "not accepted"),
            "page size": ({"page_size": 16384}, "4096"),
            "family": ({"family": "aosp-17"}, "LineageOS"),
            "inventory size": ({"disk_size": 7}, "size in the manifest"),
            "no system disk role": ({"disk_role": "guest-disk-archive"}, "system-disk"),
            "layout size": ({"layout_bytes": len(SYSTEM_DISK) + 1}, "size in the partition layout"),
            "layout sha256": ({"layout_sha256": "0" * 64}, "sha256 in the partition layout"),
            "layout schema": ({"layout_schema": 1}, "schema 2"),
        }
        for name, (overrides, message) in cases.items():
            with self.subTest(name):
                release = make_release(self.root / name, **overrides)
                output = self.root / "out" / name
                result = run("--release", str(release), "--output", str(output))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())

    def test_a_missing_file_is_refused(self):
        release = make_release(self.root / "releases")
        (release / "boot/kernel-command-line.txt").unlink()
        result = run("--release", str(release), "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing or unsafe", result.stderr)

    def test_staging_again_replaces_the_output_and_a_failure_keeps_it(self):
        first = make_release(self.root / "first")
        self.assertEqual(run("--release", str(first), "--output", str(self.output)).returncode, 0)
        before = (self.output / "release-manifest.json").read_text()

        broken = make_release(self.root / "broken", page_size=16384)
        self.assertNotEqual(run("--release", str(broken), "--output", str(self.output)).returncode, 0)
        self.assertEqual((self.output / "release-manifest.json").read_text(), before)

        second = make_release(self.root / "second", release_id="20261101-los24-r5", disk=b"newer disk")
        self.assertEqual(run("--release", str(second), "--output", str(self.output)).returncode, 0)
        self.assertIn("20261101-los24-r5", (self.output / "release-manifest.json").read_text())
        self.assertEqual((self.output / "guest/system.img").read_bytes(), b"newer disk")
        # No scratch directory is left beside the output.
        self.assertEqual([entry.name for entry in self.output.parent.iterdir()], ["andarium-system"])


def set_path(*keys, value):
    """An edit that sets one manifest field, or removes it when the value is None."""
    def edit(_directory, manifest):
        owner = manifest
        for key in keys[:-1]:
            owner = owner[key]
        if value is None:
            owner.pop(keys[-1], None)
        else:
            owner[keys[-1]] = value
    return edit


def rewrite_layout(directory: pathlib.Path, change) -> None:
    path = directory / "guest/partition-layout.json"
    layout = json.loads(path.read_text())
    change(layout)
    path.write_text(json.dumps(layout, indent=2) + "\n")


class UnsignedProofTests(unittest.TestCase):
    """--unsigned-proof stages a proof for developer builds, and only a proof."""

    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        self.root = pathlib.Path(self._scratch.name)
        self.output = self.root / "out/andarium-system"
        self.checkout = make_checkout(self.root / "checkout")

    def stage(self, proof: pathlib.Path, *extra: str, output: pathlib.Path | None = None,
              checkout: Checkout | None = None):
        return run_script((checkout or self.checkout).script, "--release", str(proof), "--unsigned-proof",
                          "--output", str(output or self.output), *extra)

    def test_a_proof_is_staged_with_a_manifest_that_still_says_it_is_one(self):
        proof = make_proof(self.root / "proofs")
        result = self.stage(proof)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            tree(self.output),
            ["boot/kernel-command-line.txt", "guest/partition-layout.json",
             "guest/system.img", "release-manifest.json"])
        for relative in ("boot/kernel-command-line.txt", "guest/partition-layout.json", "guest/system.img"):
            self.assertEqual((self.output / relative).read_bytes(), (proof / relative).read_bytes(), relative)
        staged = json.loads((self.output / "release-manifest.json").read_text())
        original = json.loads((proof / "release-manifest.json").read_text())
        self.assertEqual(staged["acceptance"], "unsigned-proof")
        self.assertEqual(staged["release_id"], PROOF_ID)
        self.assertIs(staged["android"]["signed"], False)
        # Everything the proof said is still there; the three fields it lacked are added.
        for key, value in original.items():
            if key not in ("android", "kernel"):
                self.assertEqual(staged[key], value, key)
        self.assertEqual(staged["android"], {**original["android"], "major": 17})
        self.assertEqual(staged["kernel"], {"page_size": 4096, "release": KERNEL_RELEASE})
        self.assertEqual(staged["proof_staging"]["derived"], ["android.major", "kernel.page_size", "kernel.release"])

    def test_the_staged_manifest_is_the_golden_one_the_application_reads(self):
        # AndariumCore's SystemInfo and ReleaseIdentity read the same file.
        proof = make_proof(self.root / "proofs", digest=False)
        self.assertEqual(self.stage(proof).returncode, 0)
        self.assertEqual((self.output / "release-manifest.json").read_text(), STAGED_PROOF_GOLDEN.read_text())

    def test_the_output_says_what_a_proof_is_and_where_it_runs(self):
        proof = make_proof(self.root / "proofs")
        result = self.stage(proof)
        self.assertEqual(result.returncode, 0, result.stderr)
        for needle in (
            f"staged UNSIGNED PROOF {FAMILY} {PROOF_ID}",
            "developer build only", "a Release archive refuses it",
            "only on a fresh or isolated guest instance (ANDARIUM_GUEST_INSTANCE=<name>)",
            "a guest with data of another system cannot start until Erase virtual machine",
            "a guest made from this proof cannot start another system",
            "built from revision cccccccccccc, which is unknown to this checkout",
            "android.major, kernel.page_size, kernel.release",
            "not checked: the signature",
        ):
            self.assertIn(needle, result.stdout)

    def test_the_revision_is_said_to_be_in_this_checkout_when_it_is(self):
        checkout = make_checkout(self.root / "repository", git=True)
        proof = make_proof(self.root / "proofs", set_path("outer", "revision", value=checkout.revision))
        result = self.stage(proof, checkout=checkout)
        self.assertIn(f"built from revision {checkout.revision[:12]}, which is in this checkout's history", result.stdout)

    def test_the_output_lists_everything_that_is_not_checked(self):
        with_digest = self.stage(make_proof(self.root / "with"))
        without = self.stage(make_proof(self.root / "without", digest=False), output=self.root / "out/other")
        for result in (with_digest, without):
            line = next(each for each in result.stdout.splitlines() if "not checked:" in each)
            for needle in (
                "the signature", "the target files", "the input trees", "the source provenance",
                "the inventory", "the command line's content", "the tracked disk policy",
            ):
                self.assertIn(needle, line)
        self.assertNotIn("the producer's own disk digest", with_digest.stdout)
        self.assertIn("the producer's own disk digest (the proof records none)", without.stdout)

    def test_without_the_flag_a_proof_is_refused_and_the_flag_is_named(self):
        proof = make_proof(self.root / "proofs")
        result = run("--release", str(proof), "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsigned proof image", result.stderr)
        self.assertIn("--unsigned-proof", result.stderr)
        self.assertFalse(self.output.exists())

    def test_the_flag_alone_finds_no_release(self):
        make_proof(self.root / "releases")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--unsigned-proof", "--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--release names", result.stderr)
        self.assertFalse(self.output.exists())

    def test_the_flag_stages_nothing_but_a_proof(self):
        for acceptance in ("accepted", "candidate"):
            with self.subTest(acceptance):
                release = make_release(self.root / acceptance, acceptance=acceptance)
                result = run_script(self.checkout.script, "--release", str(release), "--unsigned-proof",
                                    "--output", str(self.output))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--unsigned-proof stages an unsigned proof", result.stderr)
                self.assertIn(repr(acceptance), result.stderr)
                self.assertFalse(self.output.exists())

    def test_each_defect_of_a_proof_is_refused_and_stages_nothing(self):
        def partition_digest(directory, manifest):
            # A disk whose partition differs from its layout, with every whole-disk
            # digest (the layout's and the producer's) repaired to the new bytes.
            disk = directory / "guest/system.img"
            data = bytearray(disk.read_bytes())
            layout = json.loads((directory / "guest/partition-layout.json").read_text())
            system = next(item for item in layout["system_disk"]["partitions"] if item["name"] == "system")
            data[system["start_bytes"] + 7] ^= 0xFF
            disk.write_bytes(bytes(data))
            digest = hashlib.sha256(bytes(data)).hexdigest()
            rewrite_layout(directory, lambda each: each["system_disk"].update(sha256=digest))
            manifest["producer"]["system_disk_sha256"] = digest

        def edited_layout(change):
            def edit(directory, _manifest):
                rewrite_layout(directory, lambda layout: change(directory, layout))
            return edit

        def remove(relative):
            def edit(directory, _manifest):
                (directory / relative).unlink()
            return edit

        def write_text(relative, text):
            def edit(directory, _manifest):
                (directory / relative).write_text(text)
            return edit

        def truncate(directory, _manifest):
            disk = directory / "guest/system.img"
            disk.write_bytes(disk.read_bytes()[:-4096])

        def link_disk(directory, _manifest):
            disk = directory / "guest/system.img"
            (directory / "disk.bin").write_bytes(disk.read_bytes())
            disk.unlink()
            disk.symlink_to(directory / "disk.bin")

        cases = {
            "layout digest": (
                edited_layout(lambda _d, layout: layout["system_disk"].update(sha256="0" * 64)),
                "sha256 in the partition layout"),
            "partition digest": (partition_digest, "does not match its partition layout"),
            "producer digest": (set_path("producer", "system_disk_sha256", value="1" * 64), "producer recorded"),
            "truncated disk": (truncate, "size in the partition layout"),
            "missing disk": (remove("guest/system.img"), "missing or unsafe"),
            "linked disk": (link_disk, "missing or unsafe"),
            "missing layout": (remove("guest/partition-layout.json"), "missing or unsafe"),
            "missing command line": (remove("boot/kernel-command-line.txt"), "missing or unsafe"),
            "layout schema": (edited_layout(lambda _d, layout: layout.update(schema_version=1)), "schema 2"),
            "stated page size": (set_path("kernel", "page_size", value=16384), "4096-byte-page"),
            "family": (set_path("product_family", value="aosp-17"), "product family"),
            "release ID": (set_path("release_id", value="20261101-los24-r5"), "does not start with"),
            "release ID the app refuses": (set_path("release_id", value="unsigned-proof-Q!"), "application accepts"),
            "release ID of another proof": (set_path("producer", "proof_for_release_id", value="ff2"), "producer names"),
            "no producer": (set_path("producer", value=None), "no producer section"),
            "signed": (set_path("android", "signed", value=True), "unsigned"),
            "no signed statement": (set_path("android", "signed", value=None), "unsigned"),
            "other major": (set_path("android", "major", value=16), "Android major"),
            "no fingerprint": (set_path("android", "fingerprint", value=None), "no fingerprint"),
            "build number": (set_path("android", "build_number", value=0), "build_number"),
            "boolean build number": (set_path("android", "build_number", value=True), "build_number"),
            "eleven-digit build number": (set_path("android", "build_number", value=10**10), "build_number"),
            "patch level that is no string": (set_path("android", "security_patch", value=20261001), "security_patch"),
            "bare proof prefix": (set_path("release_id", value="unsigned-proof-", ), "name a release"),
            "stated kernel release": (set_path("kernel", "release", value="6.18.1-gstated"), "accepted Android kernel's"),
            "empty command line": (write_text("boot/kernel-command-line.txt", "  \n"), "command line is empty"),
            "no android section": (set_path("android", value=None), "no android section"),
            "no kernel section": (set_path("kernel", value=None), "no kernel section"),
        }
        for name, (edit, message) in cases.items():
            with self.subTest(name):
                proof = make_proof(self.root / name, edit)
                output = self.root / "out" / name
                result = self.stage(proof, output=output)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())

    def test_the_tracked_line_must_be_a_four_kib_line(self):
        proof = make_proof(self.root / "proofs")
        for name, change in (("image line", {"page_size": 16384}), ("kernel", {"kernel_page_size": 16384})):
            with self.subTest(name):
                checkout = make_checkout(self.root / name, **change)
                result = self.stage(proof, checkout=checkout)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("4096-byte-page line", result.stderr)
                self.assertFalse(self.output.exists())

    def test_a_repeated_json_key_in_the_manifest_or_the_layout_is_refused(self):
        def repeat(text: str, key: str, value: str) -> str:
            return text.rstrip().rstrip("}").rstrip() + f',\n"{key}": {value}}}\n'

        for name, relative, key, value in (
            ("manifest acceptance", "release-manifest.json", "acceptance", '"unsigned-proof"'),
            ("manifest release ID", "release-manifest.json", "release_id", '"unsigned-proof-other"'),
            ("layout schema", "guest/partition-layout.json", "schema_version", "2"),
        ):
            with self.subTest(name):
                proof = make_proof(self.root / name)
                path = proof / relative
                path.write_text(repeat(path.read_text(), key, value))
                result = self.stage(proof, output=self.root / "out" / name)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"repeats the key {key}", result.stderr)
                self.assertFalse((self.root / "out" / name).exists())

    def test_a_proof_that_states_a_field_the_tracked_line_agrees_with_keeps_it(self):
        edits = [set_path("android", "major", value=17), set_path("kernel", "release", value=KERNEL_RELEASE),
                 set_path("kernel", "page_size", value=4096)]

        def edit(directory, manifest):
            for each in edits:
                each(directory, manifest)

        proof = make_proof(self.root / "proofs", edit)
        self.assertEqual(self.stage(proof).returncode, 0)
        staged = json.loads((self.output / "release-manifest.json").read_text())
        self.assertEqual(staged["kernel"], {"page_size": 4096, "release": KERNEL_RELEASE})
        self.assertEqual(staged["proof_staging"]["derived"], [])

    def test_a_failed_staging_keeps_what_was_staged(self):
        good = make_proof(self.root / "good")
        self.assertEqual(self.stage(good).returncode, 0)
        before = (self.output / "release-manifest.json").read_bytes()
        bad = make_proof(self.root / "bad", set_path("android", "signed", value=True))
        self.assertNotEqual(self.stage(bad).returncode, 0)
        self.assertEqual((self.output / "release-manifest.json").read_bytes(), before)
        self.assertEqual([entry.name for entry in self.output.parent.iterdir()], ["andarium-system"])

    def test_the_tracked_metadata_of_this_checkout_supplies_the_missing_fields(self):
        sys.path.insert(0, str(REPOSITORY / "kernel/common/artifacts"))
        import contract_checks

        accepted = contract_checks.parse_assignments(REPOSITORY / "android/lineage-24/metadata/accepted-release.env")
        image = contract_checks.parse_assignments(REPOSITORY / "android/lineage-24/metadata/lineage.env")
        kernel = contract_checks.parse_assignments(
            REPOSITORY / "kernel/mainline-6.18/artifacts/accepted-android-4k.env")
        proof = make_proof(self.root / "proofs", set_path("product_family", value=accepted["LINEAGE24_ACCEPTED_PRODUCT_FAMILY"]))
        result = run("--release", str(proof), "--unsigned-proof", "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        staged = json.loads((self.output / "release-manifest.json").read_text())
        self.assertEqual(staged["android"]["major"], int(image["LINEAGE24_ANDROID_MAJOR"]))
        self.assertEqual(staged["kernel"]["release"], kernel["UML_KERNEL_ARTIFACT_RELEASE"])
        self.assertEqual(staged["kernel"]["page_size"], int(kernel["UML_KERNEL_ARTIFACT_PAGE_SIZE"]))
        self.assertEqual(staged["proof_staging"]["from"], [
            "android/lineage-24/metadata/lineage.env", "kernel/mainline-6.18/artifacts/accepted-android-4k.env"])

    def test_a_signed_release_is_staged_as_before(self):
        release = make_release(self.root / "releases")
        result = run("--release", str(release), "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"staged {FAMILY} {RELEASE_ID}: {self.output}\n")
        self.assertEqual((self.output / "release-manifest.json").read_bytes(), (release / "release-manifest.json").read_bytes())
        self.assertNotIn("proof_staging", (self.output / "release-manifest.json").read_text())


ROOT_DIRECTORY = "root"


def rooted_release(root: pathlib.Path, *, proof: bool = False, edit=None) -> pathlib.Path:
    """A release (or an unsigned proof) with Magisk root files, on a real small disk.

    The disk is what the tracked disk policy assembles with the synthetic stock
    init_boot of the Magisk fixtures in its init_boot partition; root/ holds the
    synthetic patch of it and the synthetic APK, and the manifest's root record is
    written here by hand. `edit(directory, manifest)` changes the release before
    the manifest is written."""
    magisk_fixture.configure(REPOSITORY)
    work = root / "rooted-work"
    (work / "synthetic").mkdir(parents=True)
    synthetic = magisk_fixture.Fixture(work / "synthetic")
    shutil.copytree(proof_template().parent / "images", work / "images")
    (work / "images/init_boot.img").write_bytes(synthetic.stock_image)
    release = root / (PROOF_DIRECTORY if proof else DIRECTORY)
    subprocess.run(
        [sys.executable, "-B", str(REPOSITORY / "android/lineage-24/tools/ubd-disk.py"), "assemble",
         "--policy", str(REPOSITORY / "android/lineage-24/support/image/disk-policy.json"),
         "--images-dir", str(work / "images"), "--output-dir", str(release / "guest")],
        check=True, capture_output=True, stdin=subprocess.DEVNULL)
    (release / "boot").mkdir()
    (release / "boot/kernel-command-line.txt").write_text("init=/init console=ttyS0\n")
    (release / ROOT_DIRECTORY).mkdir()
    patched = synthetic.patched_reference()
    apk = synthetic.apk(script=magisk_fixture.patch_script()).read_bytes()
    (release / "root/init_boot-magisk.img").write_bytes(patched)
    (release / f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk").write_bytes(apk)
    record = {
        "magisk": {"name": "Magisk", "version_name": magisk_fixture.TEST_VERSION,
                   "version_code": magisk_fixture.TEST_VERSION_CODE},
        "apk": {"path": f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk",
                "sha256": hashlib.sha256(apk).hexdigest(), "size": len(apk)},
        "boot_image": {"path": "root/init_boot-magisk.img", "sha256": hashlib.sha256(patched).hexdigest(),
                      "size": len(patched), "stock_init_boot_sha256": hashlib.sha256(synthetic.stock_image).hexdigest(),
                      "stock_init_boot_sha1": hashlib.sha1(synthetic.stock_image).hexdigest()},
    }
    layout = json.loads((release / "guest/partition-layout.json").read_text())
    if proof:
        manifest = proof_manifest(layout["system_disk"]["sha256"])
    else:
        sizes = {name: (release / name).stat().st_size for name in (
            "boot/kernel-command-line.txt", "guest/partition-layout.json", "guest/system.img")}
        manifest = {
            "acceptance": "accepted", "android": {"major": 17},
            "kernel": {"page_size": 4096, "release": "6.18.54-gabc"}, "product_family": FAMILY,
            "release_id": RELEASE_ID, "schema_version": 8,
            "inventory": [
                {"path": "boot/kernel-command-line.txt", "role": "kernel-command-line",
                 "size": sizes["boot/kernel-command-line.txt"]},
                {"path": "guest/partition-layout.json", "role": "partition-layout",
                 "size": sizes["guest/partition-layout.json"]},
                {"path": "guest/system.img", "role": "system-disk", "size": sizes["guest/system.img"]},
            ],
        }
    manifest["root"] = record
    if edit is not None:
        edit(release, manifest)
    (release / "release-manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return release


class RootFilesTests(unittest.TestCase):
    """A release's Magisk root files are staged beside its disk, checked first."""

    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        self.root = pathlib.Path(self._scratch.name)
        self.output = self.root / "out/andarium-system"
        self.checkout = make_checkout(self.root / "checkout")
        self.files = [
            "boot/kernel-command-line.txt", "guest/partition-layout.json", "guest/system.img",
            "release-manifest.json", "root/init_boot-magisk.img",
            f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk",
        ]

    def stage(self, release: pathlib.Path, *extra: str, proof: bool = False) -> subprocess.CompletedProcess:
        return run_script(
            self.checkout.script, "--release", str(release), "--output", str(self.output),
            *(["--unsigned-proof"] if proof else []), *extra)

    def test_the_root_files_are_staged_beside_the_disk_as_copies(self):
        release = rooted_release(self.root / "releases")
        result = self.stage(release)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.output), sorted(self.files))
        for relative in self.files:
            self.assertEqual((self.output / relative).read_bytes(), (release / relative).read_bytes(), relative)
        self.assertEqual(result.stdout, f"staged {FAMILY} {RELEASE_ID} with Magisk root files: {self.output}\n")

    def test_a_proof_with_root_files_stages_them_and_keeps_saying_it_is_a_proof(self):
        proof = rooted_release(self.root / "proofs", proof=True)
        result = self.stage(proof, proof=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tree(self.output), sorted(self.files))
        staged = json.loads((self.output / "release-manifest.json").read_text())
        original = json.loads((proof / "release-manifest.json").read_text())
        self.assertEqual(staged["acceptance"], "unsigned-proof")
        self.assertEqual(staged["root"], original["root"])
        self.assertIn("with Magisk root files", result.stdout)
        self.assertIn("root files: checked against the proof's own init_boot and the APK", result.stdout)

    def test_a_release_without_root_files_stages_none(self):
        release = make_release(self.root / "releases")
        self.assertEqual(self.stage(release).returncode, 0)
        self.assertFalse((self.output / "root").exists())

    def test_each_defect_of_the_root_files_is_refused_and_stages_nothing(self):
        def replace(relative, change):
            def edit(directory, manifest):
                path = directory / relative
                path.write_bytes(change(path.read_bytes()))
            return edit

        def flip(data):
            return bytes([data[0] ^ 1]) + data[1:]

        def stale_stock(directory, manifest):
            manifest["root"]["boot_image"]["stock_init_boot_sha256"] = "0" * 64

        def stock_as_patched(directory, manifest):
            stock = magisk_fixture.Fixture(pathlib.Path(tempfile.mkdtemp(dir=self.root))).stock_image
            (directory / "root/init_boot-magisk.img").write_bytes(stock)
            manifest["root"]["boot_image"]["sha256"] = hashlib.sha256(stock).hexdigest()
            manifest["root"]["boot_image"]["size"] = len(stock)

        def other_magiskinit(directory, manifest):
            other = self.root / "other.apk"
            magisk_fixture.make_apk_bytes(other, magisk_fixture.apk_members(magisk_fixture.patch_script()) | {
                "lib/arm64-v8a/libmagiskinit.so": b"another build"})
            payload = other.read_bytes()
            (directory / f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk").write_bytes(payload)
            manifest["root"]["apk"]["sha256"] = hashlib.sha256(payload).hexdigest()
            manifest["root"]["apk"]["size"] = len(payload)

        def no_record(directory, manifest):
            del manifest["root"]

        def link_root(directory, manifest):
            (directory / "root").rename(directory / "root.real")
            (directory / "root").symlink_to("root.real")

        def unlink(relative):
            return lambda directory, manifest: (directory / relative).unlink()

        cases = {
            "a missing patched image": (unlink("root/init_boot-magisk.img"), "root/ is partial"),
            "a missing APK": (unlink(f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk"), "root/ is partial"),
            "a patched image that is not the recorded one": (
                replace("root/init_boot-magisk.img", flip), "recorded size or SHA-256"),
            "an APK that is not the recorded one": (
                replace(f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk", flip), "recorded size or SHA-256"),
            "stock digests that are not the disk's": (stale_stock, "stock digests are not those"),
            "a patched image that is the stock image": (stock_as_patched, "lz4 legacy"),
            "an APK with another magiskinit": (other_magiskinit, "not the APK's arm64 magiskinit"),
            "a root/ with no record": (no_record, "does not declare a root record"),
            "a file beside the two root files": (
                lambda directory, manifest: (directory / "root/extra.bin").write_bytes(b"x"),
                "does not declare"),
            "an empty directory beside the two root files": (
                lambda directory, manifest: (directory / "root/empty").mkdir(), "does not declare"),
            "a root/ that is a link": (link_root, "real directory"),
        }
        for proof in (False, True):
            for name, (edit, message) in cases.items():
                with self.subTest(name, proof=proof):
                    directory = self.root / f"case-{int(proof)}-{abs(hash(name))}"
                    directory.mkdir()
                    release = rooted_release(directory, proof=proof, edit=edit)
                    result = self.stage(release, proof=proof)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(message, result.stderr)
                    self.assertFalse(self.output.exists())


    def test_a_directory_that_is_a_link_is_refused_before_anything_is_read_through_it(self):
        # Each directory the script reads below must be a real one. The disk behind a
        # linked guest/ is not the one the manifest names, so that is the refusal if
        # the script reads through the link first.
        def link(name, disk=None):
            def edit(directory, manifest):
                (directory / name).rename(directory / f"{name}.real")
                (directory / name).symlink_to(f"{name}.real")
                if disk is not None:
                    (directory / f"{name}.real/system.img").write_bytes(disk)
            return edit

        cases = {
            "guest": link("guest", b"another disk"),
            "boot": link("boot"),
            "root": link("root"),
        }
        for proof in (False, True):
            for name, edit in cases.items():
                with self.subTest(name, proof=proof):
                    directory = self.root / f"link-{int(proof)}-{name}"
                    directory.mkdir()
                    release = rooted_release(directory, proof=proof, edit=edit)
                    result = self.stage(release, proof=proof)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertRegex(result.stderr, f"the release's {name}/ must be a real directory")
                    self.assertFalse(self.output.exists())


class StagedRootFilesTests(unittest.TestCase):
    """What is staged is what was validated and what verifies: the record that names the
    destinations is the one that was read, the destinations are the two canonical paths,
    and the staged bytes are checked before they replace the output."""

    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        self.root = pathlib.Path(self._scratch.name)
        self.output = self.root / "out/andarium-system"
        self.checkout = make_checkout(self.root / "checkout")
        spec = importlib.util.spec_from_file_location("stage_under_test", self.checkout.script)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_the_destinations_come_from_the_record_that_is_validated(self):
        # The manifest read at the start names the files; the verifier reads the file
        # again. A record that names anything but the two canonical paths is refused
        # whatever the file now says.
        release = rooted_release(self.root / "releases")
        manifest = json.loads((release / "release-manifest.json").read_text())
        self.assertEqual(
            self.module.check_root(release, manifest),
            ["root/init_boot-magisk.img", f"root/Magisk-{magisk_fixture.TEST_VERSION}.apk"])
        manifest["root"]["boot_image"]["path"] = "../../escape.bin"
        with self.assertRaisesRegex(self.module.StageError, "root record"):
            self.module.check_root(release, manifest)

    def test_only_the_two_canonical_paths_are_staged(self):
        release = rooted_release(self.root / "releases")
        (release.parent / "escape.bin").write_bytes(b"outside")
        with self.assertRaisesRegex(self.module.StageError, "not a root file"):
            self.module.stage(release, self.output, None, ["root/../../escape.bin"])
        self.assertFalse((self.output.parent / "escape.bin").exists())
        self.assertFalse(self.output.exists())

    def test_the_staged_bytes_are_checked_before_they_replace_the_output(self):
        release = rooted_release(self.root / "releases")
        manifest = json.loads((release / "release-manifest.json").read_text())
        extra = self.module.check_root(release, manifest)
        self.module.stage(release, self.output, None, extra)
        before = {path: (self.output / path).read_bytes() for path in self.module.FILES}
        real_copy = self.module.copy

        def corrupting(source, destination):
            real_copy(source, destination)
            if destination.name == "init_boot-magisk.img":
                destination.write_bytes(destination.read_bytes()[:-1] + b"\x01")

        with mock.patch.object(self.module, "copy", corrupting), \
                self.assertRaisesRegex(self.module.StageError, "staged root files do not verify"):
            self.module.stage(release, self.output, None, extra)
        self.assertEqual({path: (self.output / path).read_bytes() for path in self.module.FILES}, before)
        self.assertEqual(sorted(path.name for path in self.output.parent.iterdir()), ["andarium-system"])


class PackageOptionTests(unittest.TestCase):
    """The packaging script checks its options before it touches any tool."""

    def package(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["/bin/sh", str(PACKAGER), *arguments],
            capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
        )

    def test_an_unknown_runtime_set_is_refused(self):
        result = self.package("--runtimes", "linux")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--runtimes takes all or android", result.stderr)

    def test_runtimes_needs_a_value(self):
        result = self.package("--runtimes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("usage", result.stderr)

    def test_the_linux_automation_runtime_is_not_an_android_only_package(self):
        result = self.package("--runtimes", "android", "--automation")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--automation", result.stderr)

    def test_an_unknown_option_shows_the_usage_with_the_new_option(self):
        result = self.package("--bogus")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--runtimes", result.stderr)


if __name__ == "__main__":
    unittest.main()
