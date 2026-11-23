#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The root files in the release manifest, installer and publisher.

Schema 8 lets a release carry the Magisk root files of its own init_boot: a `root`
record in the manifest and two files under root/. A release without them stays
valid, as does every release of schema 6 and 7. The release tooling refuses a
release whose root/ is partial, whose patched image does not verify against its
own stock init_boot, or whose APK is not the pinned one, each by name. The files
are the synthetic ones of magisk_fixture.py: no Magisk file is in the repository.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock
from pathlib import Path

import magisk_fixture
from magisk_fixture import TEST_VERSION, Fixture, make_apk_bytes, patch_script

ROOT = Path(__file__).resolve().parents[3]


# What the semantic checks read of a release besides the disk: the fixed partitions
# of the fstab and well-formed XML; the rest of a payload is a byte.
PAYLOADS = {
    "support/fstab.uml": "".join(
        f"/dev/block/by-name/{name} /{name} ext4 ro 0 0\n"
        for name in ("system", "system_ext", "product", "vendor", "odm", "metadata", "userdata", "misc")
    ).encode(),
    "support/vintf-manifest.xml": b"<manifest/>",
    "support/compatibility-matrix.xml": b"<compatibility-matrix/>",
    "provenance/source-repo-manifest.xml": b"<manifest/>",
}


@contextlib.contextmanager
def tools_stubbed():
    """The semantic checks that need a kernel, a disk and their tools do nothing: what
    runs is the rest of verify_semantics, the root check among it."""
    with mock.patch.object(release_common, "run_checked", lambda *a, **k: None), \
            mock.patch.object(release_common, "verify_kernel_config", lambda *a, **k: None), \
            mock.patch.object(release_common, "verify_disk_boot_payloads", lambda *a, **k: None):
        yield


def load_installer():
    spec = importlib.util.spec_from_file_location("install_release_tool", TOOLS / "install-release.py")
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    return installer


class ReleaseFixture:
    """A release directory of schema 8 with the root files, and the means to change it."""

    def __init__(self, directory: Path, *, root: bool = True, schema: int | None = None) -> None:
        self.directory = directory
        self.android_root = ROOT / "android/lineage-24"
        self.schema = schema or release_common.SCHEMA_VERSION
        self.name = release_common.DIRECTORY_PREFIX + "fixture-1"
        self.release = directory / self.name
        (directory / "synthetic").mkdir()
        self.synthetic = Fixture(directory / "synthetic")
        self.apk = self.synthetic.apk(script=patch_script())
        self.pin = self.synthetic.pin(self.apk)
        self.release.mkdir()
        for relative in release_common.PAYLOAD_SPECS:
            path = self.release / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(PAYLOADS.get(relative, b"x"))
        shutil.copyfile(self.synthetic.disk, self.release / "guest/system.img")
        shutil.copyfile(self.synthetic.layout, self.release / "guest/partition-layout.json")
        self.root = root
        if root:
            root_dir = self.release / "root"
            root_dir.mkdir()
            (root_dir / "init_boot-magisk.img").write_bytes(self.synthetic.patched_reference())
            shutil.copyfile(self.apk, root_dir / f"Magisk-{TEST_VERSION}.apk")
        self.manifest = self.build_manifest()
        self.write()

    def build_manifest(self) -> dict:
        tracked = release_common.tracked_identity(self.android_root)
        aosp = release_common.parse_env(self.android_root / "metadata/lineage.env")
        manifest = {
            "schema_version": self.schema,
            "release_id": "fixture-1",
            "directory_name": self.name,
            "product_family": release_common.PRODUCT_FAMILY,
            "acceptance": "accepted",
            "published_at_utc": "2026-11-22T00:00:00Z",
            "android": {
                "major": 17, "sdk": 37, "build_id": "FIXTURE", "build_number": 7,
                "security_patch": "2026-10-05", "build_variant": tracked["android"]["build_variant"],
                "fingerprint": "fixture/fingerprint", "vendor_api_level": 202704,
                "product": aosp["LINEAGE24_PRODUCT"], "release_config": aosp["LINEAGE24_RELEASE_CONFIG"],
                "abi_list": ["arm64-v8a"],
            },
            "source": {
                **tracked["source"],
                "routes": release_common.read_routes(
                    self.android_root / "patches/projects.tsv", self.android_root, "source"),
            },
            "kernel": {
                **tracked["kernel"], "release": "6.18.21-fixture", "page_size": 4096,
                "elf_role": "darwin-uml",
            },
            "outer": {"revision": "a" * 40, "dirty": False, **tracked["outer"]},
            "producer": {
                "entry_point": "android/lineage-24/build/publish-release.sh",
                "functional_options": ["fixture"],
                "builder": {
                    "os": "Linux fixture", "architecture": "x86_64", "jdk": "fixture",
                    "clang": "fixture", "linker": "fixture", "darwin_sdk": "fixture",
                    "android_build_tools": "fixture",
                },
            },
            "inventory": [],
        }
        if self.schema < release_common.INPUT_TREES_SCHEMA:
            del manifest["outer"]["input_trees"]
        if self.root and self.schema >= release_common.ROOT_SCHEMA:
            manifest["root"] = release_common.root_record(
                self.release / "root", self.release / "guest/system.img",
                self.release / "guest/partition-layout.json", self.pin)
        return manifest

    def write(self, manifest: dict | None = None) -> None:
        """Write the manifest, with an inventory that matches the files, and its descriptor."""
        manifest = manifest if manifest is not None else self.manifest
        manifest["inventory"] = []
        try:
            specs = release_common.payload_specs(manifest)
        except (KeyError, TypeError):  # a malformed record: the verifier is what must refuse it
            specs = dict(release_common.PAYLOAD_SPECS)
        for relative, (role, file_format) in sorted(specs.items()):
            path = self.release / relative
            manifest["inventory"].append({
                "path": relative, "role": role, "format": file_format,
                "size": path.stat().st_size if path.is_file() else 1,
            })
        (self.release / "release-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        self.acceptance = self.directory / "accepted-release.env"
        self.acceptance.write_text(release_common.render_acceptance(manifest))

    def verify(self, *, semantic: bool = False) -> dict:
        with tools_stubbed() if semantic else contextlib.nullcontext():
            return release_common.verify_release(
                self.release, self.android_root, acceptance_path=self.acceptance, semantic=semantic)

    def refuses(self, test: unittest.TestCase, pattern: str) -> None:
        with test.assertRaisesRegex(SystemExit, pattern):
            self.verify()


class AcceptanceTests(unittest.TestCase):
    """The accepted descriptor binds the reviewed root record, and the accepting side
    checks it: the pin where a candidate is accepted, the descriptor where it is installed."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def make(self, **options) -> ReleaseFixture:
        return ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)), **options)

    def test_the_descriptor_binds_the_root_record_including_its_absence(self) -> None:
        rooted, plain = self.make(), self.make(root=False)
        key = release_common.ROOT_RECORD_KEY
        self.assertRegex(release_common.parse_env(rooted.acceptance)[key], "^[0-9a-f]{64}$")
        self.assertEqual(release_common.parse_env(plain.acceptance)[key], "none")
        self.assertNotEqual(rooted.acceptance.read_text(), plain.acceptance.read_text())
        # Nothing else of the two descriptors differs: one release identity, two roots.
        keep = lambda fixture: [  # noqa: E731
            line for line in fixture.acceptance.read_text().splitlines()
            if not line.startswith(key) and "INPUT_TREES" not in line]
        self.assertEqual(keep(rooted), keep(plain))

    def test_a_release_installs_only_against_the_descriptor_that_binds_its_root(self) -> None:
        rooted, plain = self.make(), self.make(root=False)
        for release, descriptor, label in (
            (rooted, plain, "a rooted release against the rootless descriptor"),
            (plain, rooted, "a rootless release against the rooted descriptor"),
        ):
            with self.subTest(label), self.assertRaisesRegex(
                    SystemExit, "differs from accepted descriptor field LINEAGE24_ACCEPTED_ROOT_RECORD_SHA256"):
                release_common.verify_release(
                    release.release, release.android_root,
                    acceptance_path=descriptor.acceptance, semantic=False)
        # Another root that is consistent with its own record is not the reviewed one.
        other = self.make()
        apk = other.release / f"root/Magisk-{TEST_VERSION}.apk"
        payload = apk.read_bytes() + b"\0"
        apk.write_bytes(payload)
        manifest = copy.deepcopy(other.manifest)
        manifest["root"]["apk"].update(sha256=hashlib.sha256(payload).hexdigest(), size=len(payload))
        other.write(manifest)
        other.verify()
        with self.assertRaisesRegex(SystemExit, "LINEAGE24_ACCEPTED_ROOT_RECORD_SHA256"):
            release_common.verify_release(
                other.release, other.android_root, acceptance_path=rooted.acceptance, semantic=False)

    def test_a_schema_7_release_keeps_its_descriptor(self) -> None:
        older = self.make(root=False, schema=7)
        text = older.acceptance.read_text()
        self.assertEqual(set(release_common.parse_env(older.acceptance)), release_common.INPUT_TREES_ACCEPTANCE_KEYS)
        self.assertNotIn("ROOT", text)
        self.assertTrue(text.splitlines()[-1].startswith("LINEAGE24_ACCEPTED_INPUT_TREES='"))

    def run_installer(self, *arguments: str) -> None:
        installer = load_installer()
        with tools_stubbed(), mock.patch.object(sys, "argv", ["install-release.py", *arguments]):
            installer.main()

    def test_printing_the_acceptance_of_a_candidate_checks_the_tracked_pin(self) -> None:
        # The candidate's APK is the synthetic one, not the pin the tracked tree names.
        rooted = self.make()
        with self.assertRaisesRegex(SystemExit, "not the pinned Magisk version"):
            self.run_installer("--source", str(rooted.release), "--print-acceptance")
        # A release without root files has no pin to check.
        plain = self.make(root=False)
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            self.run_installer("--source", str(plain.release), "--print-acceptance")
        self.assertEqual(printed.getvalue(), plain.acceptance.read_text())

    def test_the_record_that_is_checked_is_the_record_that_was_read(self) -> None:
        # The descriptor a candidate prints names the record that was read, and the pin
        # and the deep check apply to that record, not to whatever the manifest path
        # holds when the deep check comes: the manifest is replaced between the two.
        rooted = self.make()
        later = copy.deepcopy(rooted.manifest)
        later["root"]["magisk"]["version_name"] = "v98.0"
        later["root"]["apk"]["path"] = "root/Magisk-v98.0.apk"
        text = json.dumps(later, indent=2, sort_keys=True) + "\n"
        real = release_common.verify_inventory

        def swapping(*arguments, **keywords):
            real(*arguments, **keywords)
            (rooted.release / "release-manifest.json").write_text(text)

        with tools_stubbed(), mock.patch.object(release_common, "verify_inventory", swapping):
            manifest = release_common.verify_release(
                rooted.release, rooted.android_root, acceptance_path=rooted.acceptance,
                semantic=True, pin=rooted.pin)
        self.assertEqual(manifest["root"], rooted.manifest["root"])

    def test_installing_an_accepted_release_is_checked_against_its_descriptor_alone(self) -> None:
        # History is not held to a pin that has moved: the descriptor binds the record.
        rooted = self.make()
        destination = self.directory / "releases"
        self.run_installer(
            "--source", str(rooted.release), "--destination-root", str(destination),
            "--acceptance", str(rooted.acceptance))
        self.assertTrue((destination / rooted.name / "root/init_boot-magisk.img").is_file())
        other = self.make(root=False)
        with self.assertRaisesRegex(SystemExit, "LINEAGE24_ACCEPTED_ROOT_RECORD_SHA256"):
            self.run_installer(
                "--source", str(rooted.release), "--destination-root", str(self.directory / "again"),
                "--acceptance", str(other.acceptance))


class InstallerPathTests(unittest.TestCase):
    """The installer's own path, with a descriptor that matches the manifest: only the
    deep check of the patched image can refuse these releases, and it must."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def refuses(self, fixture: ReleaseFixture, pattern: str) -> None:
        destination = self.directory / f"releases-{len(list(self.directory.iterdir()))}"
        installer = load_installer()
        arguments = [
            "install-release.py", "--source", str(fixture.release),
            "--destination-root", str(destination), "--acceptance", str(fixture.acceptance)]
        with tools_stubbed(), mock.patch.object(sys, "argv", arguments), \
                self.assertRaisesRegex(SystemExit, pattern):
            installer.main()
        self.assertEqual([path for path in destination.glob("*") if not path.name.startswith(".")], [])

    @staticmethod
    def replace(fixture: ReleaseFixture, name: str, payload: bytes) -> None:
        """Change a root file and keep the record and the descriptor in step with it."""
        (fixture.release / "root" / name).write_bytes(payload)
        manifest = copy.deepcopy(fixture.manifest)
        entry = manifest["root"]["apk" if name.endswith(".apk") else "boot_image"]
        entry.update(sha256=hashlib.sha256(payload).hexdigest(), size=len(payload))
        fixture.write(manifest)

    def test_a_good_release_installs(self) -> None:
        fixture = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)))
        destination = self.directory / "good"
        arguments = [
            "install-release.py", "--source", str(fixture.release),
            "--destination-root", str(destination), "--acceptance", str(fixture.acceptance)]
        with tools_stubbed(), mock.patch.object(sys, "argv", arguments):
            load_installer().main()
        self.assertTrue((destination / fixture.name / "root/init_boot-magisk.img").is_file())

    def test_a_patched_image_that_does_not_verify_is_not_installed(self) -> None:
        fixture = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)))
        self.replace(fixture, "init_boot-magisk.img", fixture.synthetic.stock_image)
        self.refuses(fixture, "lz4 legacy")

    def test_an_apk_whose_binaries_are_not_in_the_image_is_not_installed(self) -> None:
        fixture = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)))
        other = fixture.directory / "other.apk"
        make_apk_bytes(other, magisk_fixture.apk_members(patch_script()) | {
            "lib/arm64-v8a/libmagiskinit.so": b"another build"})
        self.replace(fixture, f"Magisk-{TEST_VERSION}.apk", other.read_bytes())
        self.refuses(fixture, "not the APK's arm64 magiskinit")

    def test_a_manifest_that_repeats_a_key_is_not_installed(self) -> None:
        # The installer, the standalone verifier and staging read a manifest by one
        # rule. A key repeated with the same value collapses to the same object, so the
        # descriptor still matches it: only the parse can tell.
        fixture = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)))
        path = fixture.release / "release-manifest.json"
        text = path.read_text()
        start = text.index('"apk": {')
        end = text.index("}", start)
        repeated = re.sub(r'"size": (\d+)', r'"size": \1,\n      "size": \1', text[start:end])
        self.assertNotEqual(repeated, text[start:end])
        path.write_text(text[:start] + repeated + text[end:])
        self.assertEqual(json.loads(path.read_text()), fixture.manifest)
        self.refuses(fixture, "repeats the key")
        with self.assertRaisesRegex(SystemExit, "repeats the key"):
            magisk_image.verify_release_root(fixture.release, SystemExit)

    def test_stock_digests_that_are_not_the_disks_are_not_installed(self) -> None:
        fixture = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)))
        manifest = copy.deepcopy(fixture.manifest)
        manifest["root"]["boot_image"]["stock_init_boot_sha256"] = "0" * 64
        fixture.write(manifest)
        self.refuses(fixture, "stock digests are not those of the release's init_boot")


class ManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def fixture(self, **options) -> ReleaseFixture:
        return ReleaseFixture(self.directory, **options)

    def test_a_release_with_root_files_verifies(self) -> None:
        fixture = self.fixture()
        manifest = fixture.verify()
        self.assertEqual(manifest["schema_version"], 8)
        self.assertEqual(sorted(entry["path"] for entry in manifest["inventory"]), sorted(
            [*release_common.PAYLOAD_SPECS, "root/init_boot-magisk.img", f"root/Magisk-{TEST_VERSION}.apk"]))
        self.assertEqual(
            release_common.parse_env(fixture.acceptance)["LINEAGE24_ACCEPTED_SCHEMA"], "8")
        specs = release_common.payload_specs(manifest)
        self.assertEqual(specs["root/init_boot-magisk.img"][0], "magisk-init-boot")
        self.assertEqual(specs[f"root/Magisk-{TEST_VERSION}.apk"][0], "magisk-apk")

    def test_a_release_without_root_files_verifies_in_every_schema(self) -> None:
        for schema in (6, 7, 8):
            with self.subTest(schema=schema):
                directory = Path(tempfile.mkdtemp(dir=self.directory))
                fixture = ReleaseFixture(directory, root=False, schema=schema)
                manifest = fixture.verify()
                self.assertNotIn("root", manifest)
                self.assertEqual(release_common.payload_specs(manifest), release_common.PAYLOAD_SPECS)

    def test_only_schema_8_carries_a_root_record(self) -> None:
        fixture = self.fixture()
        for schema in (6, 7):
            with self.subTest(schema=schema):
                older = copy.deepcopy(fixture.manifest)
                older["schema_version"] = schema
                if schema == 6:
                    del older["outer"]["input_trees"]
                fixture.write(older)
                fixture.refuses(self, "cannot carry a root record")

    def test_a_partial_root_directory_is_refused_by_name(self) -> None:
        fixture = self.fixture()
        patched = fixture.release / "root/init_boot-magisk.img"
        apk = fixture.release / f"root/Magisk-{TEST_VERSION}.apk"
        saved = patched.read_bytes()
        patched.unlink()
        fixture.refuses(self, r"release root/ is partial: missing \['root/init_boot-magisk.img'\]")
        patched.write_bytes(saved)
        apk.unlink()
        fixture.refuses(self, r"release root/ is partial: missing \['root/Magisk-v99.1.apk'\]")

    def test_root_files_without_a_record_are_refused_by_name(self) -> None:
        fixture = self.fixture()
        without = copy.deepcopy(fixture.manifest)
        del without["root"]
        fixture.write(without)
        fixture.refuses(self, "root/ holds files its manifest's root record does not declare")
        fixture.write()
        (fixture.release / "root/extra").write_bytes(b"x")
        fixture.refuses(self, "root/ holds files its manifest's root record does not declare")

    def test_a_root_file_that_is_not_the_recorded_one_is_refused(self) -> None:
        fixture = self.fixture()
        apk = fixture.release / f"root/Magisk-{TEST_VERSION}.apk"
        original = apk.read_bytes()
        flipped = bytearray(original)
        flipped[100] ^= 1
        apk.write_bytes(bytes(flipped))
        fixture.write()
        fixture.refuses(self, "differs from its recorded size or SHA-256")
        apk.write_bytes(original + b"x")
        fixture.write()
        fixture.refuses(self, "differs from its recorded size or SHA-256")
        # The inventory's size of a file is checked by its own rule.
        apk.write_bytes(original)
        fixture.write()
        fixture.verify()
        entries = json.loads((fixture.release / "release-manifest.json").read_text())
        for entry in entries["inventory"]:
            if entry["path"] == f"root/Magisk-{TEST_VERSION}.apk":
                entry["size"] += 1
        (fixture.release / "release-manifest.json").write_text(json.dumps(entries))
        fixture.refuses(self, "release payload size differs for root/Magisk-v99.1.apk")

    def test_a_record_the_inventory_does_not_list_is_refused(self) -> None:
        fixture = self.fixture()
        entries = json.loads((fixture.release / "release-manifest.json").read_text())
        entries["inventory"] = [
            entry for entry in entries["inventory"] if not entry["path"].startswith("root/")]
        (fixture.release / "release-manifest.json").write_text(json.dumps(entries))
        fixture.refuses(self, "differs from its fixed contract")

    def test_a_malformed_root_record_is_refused(self) -> None:
        fixture = self.fixture()
        for label, edit in (
            ("an extra field", lambda r: r["apk"].update(extra=1)),
            ("a missing section", lambda r: r.pop("magisk")),
            ("another name", lambda r: r["magisk"].update(name="Other")),
            ("a version without a v", lambda r: r["magisk"].update(version="99.1")),
            ("a zero version code", lambda r: r["magisk"].update(version_code=0)),
            ("a boolean size", lambda r: r["apk"].update(size=True)),
            ("an APK path that is not its version's", lambda r: r["apk"].update(path="root/Magisk.apk")),
            ("another patched image path", lambda r: r["boot_image"].update(path="root/boot.img")),
            ("a short digest", lambda r: r["apk"].update(sha256="abc")),
            ("a capital digest", lambda r: r["apk"].update(sha256="A" * 64)),
            ("a short SHA-1", lambda r: r["boot_image"].update(stock_init_boot_sha1="a" * 39)),
            ("a path that climbs out", lambda r: r["apk"].update(path="../Magisk-v99.1.apk")),
        ):
            with self.subTest(label):
                broken = copy.deepcopy(fixture.manifest)
                edit(broken["root"])
                fixture.write(broken)
                with self.assertRaises(SystemExit):
                    fixture.verify()
        broken = copy.deepcopy(fixture.manifest)
        broken["root"] = "root/"
        fixture.write(broken)
        with self.assertRaises(SystemExit):
            fixture.verify()

    def test_a_schema_8_manifest_is_the_tracked_trees_own_and_older_ones_are_not(self) -> None:
        fixture = self.fixture()
        for schema in (6, 7):
            older = copy.deepcopy(fixture.manifest)
            older["schema_version"] = schema
            del older["root"]
            if schema == 6:
                del older["outer"]["input_trees"]
            with self.subTest(schema=schema), self.assertRaisesRegex(SystemExit, "tracked tree's own release"):
                release_common.verify_tracked_identity(older, fixture.android_root)

    def test_the_json_schema_names_the_record_the_verifier_checks(self) -> None:
        schema = json.loads(
            (ROOT / "android/lineage-24/support/release/release-manifest.schema.json").read_text())
        self.assertEqual(schema["properties"]["schema_version"]["enum"], list(release_common.SCHEMA_VERSIONS))
        record = schema["properties"]["root"]
        self.assertFalse(record["additionalProperties"])
        self.assertEqual(set(record["required"]), {"magisk", "apk", "boot_image"})
        self.assertEqual(set(record["properties"]["magisk"]["required"]), {"name", "version_name", "version_code"})
        self.assertEqual(set(record["properties"]["apk"]["required"]), {"path", "sha256", "size"})
        self.assertEqual(
            set(record["properties"]["boot_image"]["required"]),
            {"path", "sha256", "size", "stock_init_boot_sha256", "stock_init_boot_sha1"})
        self.assertEqual(record["properties"]["boot_image"]["properties"]["path"]["const"], magisk_image.PATCHED_PATH)
        # A root record needs schema 8, and the input trees need schema 7 or later.
        conditions = {json.dumps(rule["then"], sort_keys=True) for rule in schema["allOf"]}
        self.assertIn(json.dumps({"properties": {"schema_version": {"const": 8}}}, sort_keys=True), conditions)


class RootVerificationTests(unittest.TestCase):
    """The deep check: the patched image against its own stock init_boot, the APK and the pin."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.fixture = ReleaseFixture(self.directory)
        self.release = self.fixture.release

    def verify_root(self, *, pin: Path | None = None) -> None:
        manifest = json.loads((self.release / "release-manifest.json").read_text())
        release_common.verify_root(self.release, manifest, pin=pin)

    def replace_root_file(self, name: str, payload: bytes) -> None:
        """Change a root file and keep the manifest's record in step, so only the deep check can object."""
        (self.release / "root" / name).write_bytes(payload)
        manifest = copy.deepcopy(self.fixture.manifest)
        record = manifest["root"]
        entry = record["apk"] if name.endswith(".apk") else record["boot_image"]
        entry["sha256"] = hashlib.sha256(payload).hexdigest()
        entry["size"] = len(payload)
        self.fixture.write(manifest)

    def test_the_synthetic_release_verifies_without_and_with_its_pin(self) -> None:
        self.verify_root()
        self.verify_root(pin=self.fixture.pin)
        self.assertEqual(self.fixture.verify()["root"], self.fixture.manifest["root"])

    def test_an_apk_that_is_not_the_pin_is_refused_by_name(self) -> None:
        other_version = self.directory / "other-version.env"
        other_version.write_text(self.fixture.pin.read_text().replace(TEST_VERSION, "v98.0"))
        with self.assertRaisesRegex(SystemExit, "not the pinned Magisk version"):
            self.verify_root(pin=other_version)
        other_digest = self.directory / "other-digest.env"
        other_digest.write_text(self.fixture.pin.read_text().replace(
            hashlib.sha256(self.fixture.apk.read_bytes()).hexdigest(), "0" * 64))
        with self.assertRaisesRegex(SystemExit, "Magisk APK digest is not the pinned one"):
            self.verify_root(pin=other_digest)
        # The tracked tree's own pin is not the synthetic release's.
        with self.assertRaisesRegex(SystemExit, "not the pinned Magisk version"):
            self.verify_root(pin=ROOT / "android/lineage-24/metadata/magisk.env")

    def test_a_patched_image_that_is_not_its_stock_images_is_refused(self) -> None:
        self.replace_root_file("init_boot-magisk.img", self.fixture.synthetic.stock_image)
        with self.assertRaisesRegex(SystemExit, "lz4 legacy"):
            self.verify_root()

    def test_a_patch_of_another_stock_image_is_refused(self) -> None:
        # A release whose disk carries another init_boot than the one the image was patched from.
        disk = self.release / "guest/system.img"
        size = len(self.fixture.synthetic.stock_image)
        data = bytearray(disk.read_bytes())
        data[8192 + 5000] ^= 1
        disk.write_bytes(bytes(data))
        with self.assertRaisesRegex(SystemExit, "stock digests are not those of the release's init_boot"):
            self.verify_root()
        # The record's stock digests agreeing with the disk is not enough: the image
        # must be the patch of that stock image.
        data = bytearray(self.fixture.synthetic.disk.read_bytes())
        stock = data[8192:8192 + size]
        stock[bytes(stock).rfind(b"toolbox") + 6] ^= 1
        data[8192:8192 + size] = stock
        disk.write_bytes(bytes(data))
        manifest = copy.deepcopy(self.fixture.manifest)
        manifest["root"]["boot_image"]["stock_init_boot_sha256"] = hashlib.sha256(stock).hexdigest()
        manifest["root"]["boot_image"]["stock_init_boot_sha1"] = hashlib.sha1(stock).hexdigest()
        self.fixture.write(manifest)
        with self.assertRaisesRegex(SystemExit, "changed the stock entry system/bin/toolbox"):
            self.verify_root()

    def test_root_files_are_real_files_and_the_only_ones(self) -> None:
        # The standalone verifier (staging runs it) follows no link on any component
        # it opens and holds root/ to its two declared children, as the installer does.
        def link_root(release):
            (release / "root").rename(release / "root.real")
            (release / "root").symlink_to("root.real")

        def link_file(release):
            apk = release / f"root/Magisk-{TEST_VERSION}.apk"
            (release / "elsewhere.apk").write_bytes(apk.read_bytes())
            apk.unlink()
            apk.symlink_to("../elsewhere.apk")

        def link_guest(release):
            (release / "guest").rename(release / "guest.real")
            (release / "guest").symlink_to("guest.real")

        def link_disk(release):
            (release / "guest/system.img").rename(release / "guest/disk.real")
            (release / "guest/system.img").symlink_to("disk.real")

        def link_layout(release):
            (release / "guest/partition-layout.json").rename(release / "guest/layout.real")
            (release / "guest/partition-layout.json").symlink_to("layout.real")

        def link_manifest(release):
            (release / "release-manifest.json").rename(release / "manifest.real")
            (release / "release-manifest.json").symlink_to("manifest.real")

        cases = {
            "a file beside the two": (lambda r: (r / "root/extra.bin").write_bytes(b"x"), "does not declare"),
            "an empty directory beside the two": (lambda r: (r / "root/empty").mkdir(), "does not declare"),
            "a pipe beside the two": (lambda r: os.mkfifo(r / "root/pipe"), "does not declare"),
            "a link beside the two": (
                lambda r: (r / "root/link").symlink_to("init_boot-magisk.img"), "does not declare"),
            "root/ as a link to a directory with the files": (link_root, "real directory"),
            "an APK that is a link": (link_file, "partial|unsafe"),
            "guest/ as a link": (link_guest, "must be a real"),
            "a disk that is a link": (link_disk, "must be a real"),
            "a layout that is a link": (link_layout, "must be a real"),
            "a manifest that is a link": (link_manifest, "must be a real"),
        }
        for label, (change, pattern) in cases.items():
            with self.subTest(label):
                directory = Path(tempfile.mkdtemp(dir=self.directory))
                fixture = ReleaseFixture(directory)
                change(fixture.release)
                manifest = json.loads(
                    (fixture.release / "release-manifest.json").read_text()) if not (
                        fixture.release / "release-manifest.json").is_symlink() else fixture.manifest
                with self.assertRaisesRegex(SystemExit, pattern):
                    if label == "a manifest that is a link":
                        # The manifest read by path is the standalone verifier's: the
                        # release tooling is handed the manifest it has already read.
                        magisk_image.verify_release_root(fixture.release, SystemExit)
                    else:
                        release_common.verify_root(fixture.release, manifest, pin=None)
                refused = subprocess.run(
                    [sys.executable, "-B", str(TOOLS / "verify-magisk-boot.py"), "--release", str(fixture.release)],
                    capture_output=True, text=True, check=False)
                self.assertNotEqual(refused.returncode, 0, label)

    def test_a_patched_image_larger_than_the_stock_partition_is_refused_before_it_is_read(self) -> None:
        # The image is bounded by the partition it replaces, and the bound is checked
        # on the file's size, not on what was read.
        payload = self.fixture.synthetic.patched_reference() + bytes(1 << 20)
        self.replace_root_file("init_boot-magisk.img", payload)
        with self.assertRaisesRegex(SystemExit, "larger than the stock init_boot"):
            self.verify_root()

    def test_the_standalone_reads_are_bounded_before_they_are_read(self) -> None:
        # Each file that the verifier takes from a path is read with a bound that is
        # checked on its size: 16 MiB for a manifest, 1 MiB for a layout, 128 MiB for an
        # APK. A sparse file past the bound costs nothing to make, and what it holds is
        # never read: the refusal is the bound's, by name.
        def grow(path: Path, size: int) -> None:
            with path.open("r+b") as handle:
                handle.truncate(size)

        def verify_release() -> None:
            magisk_image.verify_release_root(self.release, SystemExit)

        grow(self.release / "release-manifest.json", (16 << 20) + 1)
        with self.assertRaisesRegex(SystemExit, "release manifest is larger than"):
            verify_release()
        self.fixture.write()
        grow(self.release / "guest/partition-layout.json", (1 << 20) + 1)
        with self.assertRaisesRegex(SystemExit, "partition layout is larger than"):
            verify_release()
        shutil.copyfile(self.fixture.synthetic.layout, self.release / "guest/partition-layout.json")
        verify_release()

        big = self.directory / "big.apk"
        big.write_bytes(b"")
        grow(big, (128 << 20) + 1)
        stock = self.directory / "stock.img"
        stock.write_bytes(self.fixture.synthetic.stock_image)
        refused = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "verify-magisk-boot.py"), "--image", str(stock),
             "--apk", str(big), "--layout", str(self.fixture.synthetic.layout),
             "--stock-image", str(stock)],
            capture_output=True, text=True, check=False, timeout=60)
        self.assertNotEqual(refused.returncode, 0)
        self.assertRegex(refused.stderr, "Magisk APK is larger than")

    def test_the_standalone_verifier_checks_one_snapshot_of_the_apk(self) -> None:
        # The APK is read once. What is checked against the pin and what the image is
        # verified against are those same bytes, whatever happens to the path after.
        spec = importlib.util.spec_from_file_location("verify_magisk_boot_tool", TOOLS / "verify-magisk-boot.py")
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        synthetic = self.fixture.synthetic
        apk = self.directory / "swapped.apk"
        shutil.copyfile(self.fixture.apk, apk)
        other = self.directory / "other.apk"
        make_apk_bytes(other, magisk_fixture.apk_members(patch_script()) | {
            "lib/arm64-v8a/libmagiskinit.so": b"another build"})
        stock = self.directory / "stock.img"
        stock.write_bytes(synthetic.stock_image)
        patched = self.directory / "patched.img"
        patched.write_bytes(synthetic.patched_reference())
        real_read_file = magisk_image.read_file

        def swapping(path, *rest):
            data = real_read_file(path, *rest)
            if Path(path) == stock:
                shutil.copyfile(other, apk)
            return data

        arguments = ["verify-magisk-boot.py", "--image", str(patched), "--apk", str(apk),
                     "--layout", str(synthetic.layout), "--stock-image", str(stock),
                     "--pin", str(self.fixture.pin)]
        with mock.patch.object(magisk_image, "read_file", swapping), \
                mock.patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()) as output:
            tool.main()
        self.assertIn("verified Magisk-patched init_boot", output.getvalue())
        self.assertEqual(apk.read_bytes(), other.read_bytes())

    def test_an_apk_whose_binaries_are_not_the_ones_in_the_image_is_refused(self) -> None:
        other = self.directory / "other.apk"
        make_apk_bytes(other, magisk_fixture.apk_members(patch_script()) | {
            "lib/arm64-v8a/libmagiskinit.so": b"another build"})
        self.replace_root_file(f"Magisk-{TEST_VERSION}.apk", other.read_bytes())
        with self.assertRaisesRegex(SystemExit, "not the APK's arm64 magiskinit"):
            self.verify_root()

    def test_the_verifier_command_line_checks_a_release_directory(self) -> None:
        verified = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "verify-magisk-boot.py"), "--release", str(self.release),
             "--pin", str(self.fixture.pin)],
            capture_output=True, text=True, check=False)
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertIn("verified Magisk root files", verified.stdout)
        (self.release / f"root/Magisk-{TEST_VERSION}.apk").unlink()
        refused = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "verify-magisk-boot.py"), "--release", str(self.release)],
            capture_output=True, text=True, check=False)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("root/ is partial", refused.stderr)
        without = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)), root=False)
        none = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "verify-magisk-boot.py"), "--release", str(without.release)],
            capture_output=True, text=True, check=False)
        self.assertNotEqual(none.returncode, 0)
        self.assertIn("no root record", none.stderr)


class ProducerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.fixture = ReleaseFixture(self.directory)
        self.release = self.fixture.release

    def test_the_record_a_producer_writes_is_the_one_the_verifier_checks(self) -> None:
        record = release_common.root_record(
            self.release / "root", self.release / "guest/system.img",
            self.release / "guest/partition-layout.json", self.fixture.pin)
        self.assertEqual(record, self.fixture.manifest["root"])
        stock = self.fixture.synthetic.stock_image
        self.assertEqual(record["boot_image"]["stock_init_boot_sha1"], hashlib.sha1(stock).hexdigest())
        self.assertEqual(record["boot_image"]["stock_init_boot_sha256"], hashlib.sha256(stock).hexdigest())
        self.assertEqual(record["magisk"]["version_name"], TEST_VERSION)

    def test_a_producer_refuses_a_root_directory_that_is_not_the_two_real_files(self) -> None:
        arguments = (self.release / "guest/system.img", self.release / "guest/partition-layout.json", self.fixture.pin)
        (self.release / "root/extra.bin").write_bytes(b"x")
        with self.assertRaisesRegex(SystemExit, "does not declare"):
            release_common.root_record(self.release / "root", *arguments)
        (self.release / "root/extra.bin").unlink()
        patched = self.release / "root/init_boot-magisk.img"
        data = patched.read_bytes()
        patched.unlink()
        (self.directory / "elsewhere.img").write_bytes(data)
        patched.symlink_to(self.directory / "elsewhere.img")
        with self.assertRaisesRegex(SystemExit, "missing, a link or not a regular file"):
            release_common.root_record(self.release / "root", *arguments)

    def test_a_producer_refuses_an_apk_that_is_not_the_pin(self) -> None:
        apk = self.release / f"root/Magisk-{TEST_VERSION}.apk"
        apk.write_bytes(apk.read_bytes() + b"x")
        with self.assertRaisesRegex(SystemExit, "is not the pinned Magisk"):
            release_common.root_record(
                self.release / "root", self.release / "guest/system.img",
                self.release / "guest/partition-layout.json", self.fixture.pin)
        apk.unlink()
        with self.assertRaises(SystemExit):
            release_common.root_record(
                self.release / "root", self.release / "guest/system.img",
                self.release / "guest/partition-layout.json", self.fixture.pin)

    def test_the_installer_copies_the_root_files_it_verified(self) -> None:
        installer = load_installer()
        manifest = json.loads((self.release / "release-manifest.json").read_text())
        staging = self.directory / "staging"
        staging.mkdir()
        installer.copy_declared(self.release, staging, manifest)
        copied = sorted(
            str(path.relative_to(staging)) for path in staging.rglob("*") if path.is_file())
        self.assertEqual(copied, sorted(["release-manifest.json", *release_common.payload_specs(manifest)]))
        for relative in release_common.payload_specs(manifest):
            self.assertEqual((staging / relative).read_bytes(), (self.release / relative).read_bytes())
        # Without a record the installer copies exactly the release it always did.
        plain = ReleaseFixture(Path(tempfile.mkdtemp(dir=self.directory)), root=False)
        plain_staging = self.directory / "plain"
        plain_staging.mkdir()
        installer.copy_declared(
            plain.release, plain_staging, json.loads((plain.release / "release-manifest.json").read_text()))
        self.assertFalse((plain_staging / "root").exists())

    def test_the_publisher_needs_root_files_or_an_explicit_choice_not_to(self) -> None:
        base = [
            "--aosp-source", "/a", "--kernel-artifact", "/k", "--target-files", "/t", "--disk-dir", "/d",
            "--darwin-sdk", "/s", "--clang", "/c", "--linker", "/l", "--vintf-checker", "/v",
            "--release-root", "/r", "--release-id", "x", "--signing-keys", "/s", "--first-boot-artifacts", "/f",
            "--mkbootfs", "/m", "--mkbootimg", "/i",
        ]

        def publish(*extra: str) -> subprocess.CompletedProcess:
            return subprocess.run(
                [sys.executable, "-B", str(TOOLS / "publish-release.py"), *base, *extra],
                stdin=subprocess.DEVNULL, capture_output=True, text=True, check=False)

        neither = publish()
        self.assertNotEqual(neither.returncode, 0)
        self.assertRegex(neither.stderr, r"one of the arguments --magisk-apk --without-root is required")
        both = publish("--magisk-apk", "/apk", "--without-root")
        self.assertNotEqual(both.returncode, 0)
        self.assertRegex(both.stderr, "not allowed with argument")


def main() -> None:
    global ROOT, TOOLS, release_common, magisk_image
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    ROOT = arguments.source_root.resolve()
    TOOLS = ROOT / "android/lineage-24/tools"
    magisk_fixture.configure(ROOT)
    import release_common as common  # noqa: PLC0415

    release_common, magisk_image = common, magisk_fixture.magisk_image
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
