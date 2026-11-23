#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AOT artifact of an Android release: what verify accepts and refuses.

A scripted release and a scripted tree stand in for the real ones: the check
reads files and digests only, and it must refuse an artifact that is not the
one the release, the descriptor and the tools in the tree name. Building from
a real image is covered by the AOT differential and guest rows.
"""

__import__("sys").dont_write_bytecode = True

import contextlib
import io
import json
import os
import pathlib
import shutil
import struct
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve()
sys.path.insert(0, str(HERE.parents[3] / "tools" / "aot"))

import release_artifact as R  # noqa: E402

RELEASE_ID = "20261201-los24-r99"
SYSTEM_SHA = "ab" * 32


def write_json(path: pathlib.Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def make_tree(root: pathlib.Path) -> pathlib.Path:
    """A tree with the files the rewrite is a function of."""
    for index, relative in enumerate(R.GENERATOR_INPUTS):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"input {index}\n")
    (root / "tools/aot/rewrite.py").write_text("BLOB_VERSION = 3\n")
    (root / "tools/aot/libraries.txt").write_text(
        "# libraries\nlibrary /system/lib64/libz.so\n"
        "library /apex/com.android.art/lib64/libart.so\n"
        "boot /system/framework/arm64/boot*.oat\n")
    return root


def make_release(root: pathlib.Path, *, system_sha: str = SYSTEM_SHA,
                 release_id: str = RELEASE_ID) -> pathlib.Path:
    write_json(root / "release-manifest.json", {
        "release_id": release_id,
        "directory_name": f"lineage24-mainline6.18-4k-{release_id}"})
    write_json(root / "guest/partition-layout.json", {"system_disk": {
        "partitions": [{"name": "boot", "sha256": "00" * 32},
                       {"name": "system", "sha256": system_sha}]}})
    return root


def make_artifact(directory: pathlib.Path, tree: pathlib.Path,
                  release: pathlib.Path, *, release_id: str = RELEASE_ID
                  ) -> dict:
    directory.mkdir(parents=True)
    blob = bytearray(4096)
    struct.pack_into("<8sI", blob, 0, R.BLOB_MAGIC, 3)
    blob[R.BLOB_RELEASE_ID_OFFSET:R.BLOB_RELEASE_ID_OFFSET + len(
        release_id)] = release_id.encode()
    identity = R.release_identity(release)
    blob[R.BLOB_MANIFEST_SHA256_OFFSET:R.BLOB_SYSTEM_SHA256_OFFSET] = \
        bytes.fromhex(identity["release_manifest_sha256"])
    blob[R.BLOB_SYSTEM_SHA256_OFFSET:R.BLOB_HEADER_BYTES] = bytes.fromhex(
        identity["system_partition_sha256"])
    (directory / "blob.bin").write_bytes(bytes(blob))
    write_json(directory / "manifest.json", {
        "release_id": release_id, "blob_bytes": len(blob)})
    write_json(directory / "licences.json", {
        "release_id": release_id, "libraries": {}})
    libraries, boot = R.library_list(tree)
    return R.write_description(directory, R.release_identity(release),
                               libraries, boot, tree)


class ArtifactTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = pathlib.Path(tempfile.mkdtemp(prefix="aot-artifact."))
        self.addCleanup(shutil.rmtree, self.scratch, True)
        self.tree = make_tree(self.scratch / "tree")
        self.release = make_release(self.scratch / "release")
        self.artifact = self.scratch / "artifact"
        self.description = make_artifact(self.artifact, self.tree,
                                         self.release)

    def check(self, **keywords) -> dict:
        keywords.setdefault("repo", self.tree)
        return R.verify(self.artifact, **keywords)

    def refuses(self, text: str, **keywords) -> None:
        with self.assertRaises(R.ArtifactError) as raised:
            self.check(**keywords)
        self.assertIn(text, str(raised.exception))

    def rewrite_description(self, edit) -> None:
        path = self.artifact / "artifact.json"
        description = json.loads(path.read_text())
        edit(description)
        write_json(path, description)

    def test_verifies_against_its_release_and_tree(self) -> None:
        self.assertEqual(self.check(release=self.release)["schema"], 1)

    def test_other_release_is_refused(self) -> None:
        other = make_release(self.scratch / "other", system_sha="cd" * 32)
        self.refuses("system_partition_sha256", release=other)
        other = make_release(self.scratch / "renamed", release_id="r100")
        self.refuses("release_id", release=other)

    def test_changed_manifest_of_the_release_is_refused(self) -> None:
        write_json(self.release / "release-manifest.json", {
            "release_id": RELEASE_ID, "directory_name": "x", "more": 1})
        self.refuses("release_manifest_sha256", release=self.release)

    def test_changed_blob_is_refused(self) -> None:
        with open(self.artifact / "blob.bin", "r+b") as handle:
            handle.seek(4000)
            handle.write(b"\1")
        self.refuses("blob.bin differs")

    def test_blob_of_another_layout_mark_is_refused(self) -> None:
        # The format mark names the layout the units and the runtime share: a
        # blob with another one (a build of an earlier layout) is not read,
        # whether its header or the descriptor names it.
        path = self.artifact / "blob.bin"
        data = bytearray(path.read_bytes())
        data[:8] = b"UMLAOT15"
        path.write_bytes(bytes(data))
        digest = R.sha256_file(path)
        self.rewrite_description(lambda d: d["blob"].update(sha256=digest))
        self.refuses("blob.bin header differs from artifact.json")
        self.rewrite_description(lambda d: d["blob"].update(magic="UMLAOT15"))
        self.refuses("blob is malformed")

    def test_blob_of_another_release_is_refused(self) -> None:
        with open(self.artifact / "blob.bin", "r+b") as handle:
            handle.seek(R.BLOB_RELEASE_ID_OFFSET)
            handle.write(b"other")
        # The digest no longer matches either, so rebuild the description.
        libraries, boot = R.library_list(self.tree)
        R.write_description(self.artifact, R.release_identity(self.release),
                            libraries, boot, self.tree)
        self.refuses("another release")

    def test_blob_of_another_system_is_refused(self) -> None:
        # The header's digests are the release's: a blob built from another
        # system partition, or another manifest, does not verify even under
        # the same release ID.
        for offset, key in ((R.BLOB_MANIFEST_SHA256_OFFSET,
                             "release_manifest_sha256"),
                            (R.BLOB_SYSTEM_SHA256_OFFSET,
                             "system_partition_sha256")):
            with self.subTest(key=key):
                blob = (self.artifact / "blob.bin").read_bytes()
                changed = bytearray(blob)
                changed[offset] ^= 1
                (self.artifact / "blob.bin").write_bytes(bytes(changed))
                libraries, boot = R.library_list(self.tree)
                R.write_description(
                    self.artifact, R.release_identity(self.release),
                    libraries, boot, self.tree)
                self.refuses(key)
                (self.artifact / "blob.bin").write_bytes(blob)
                R.write_description(
                    self.artifact, R.release_identity(self.release),
                    libraries, boot, self.tree)
                self.check(release=self.release)

    def test_changed_notices_or_manifest_are_refused(self) -> None:
        write_json(self.artifact / "licences.json",
                   {"release_id": RELEASE_ID, "libraries": {"a": 1}})
        self.refuses("licences.json differs")

    def test_changed_generator_input_is_refused(self) -> None:
        with open(self.tree / "tools/aot/asm.py", "a") as handle:
            handle.write("changed\n")
        self.refuses("tools/aot/asm.py")
        # A check that reads the artifact alone does not look.
        self.check(generator=False)

    def test_changed_blob_version_is_refused(self) -> None:
        (self.tree / "tools/aot/rewrite.py").write_text("BLOB_VERSION = 4\n")
        self.refuses("tools/aot/rewrite.py")
        self.rewrite_description(lambda d: d["generator"].update(
            {"tools/aot/rewrite.py": R.sha256_file(
                self.tree / "tools/aot/rewrite.py")}))
        self.refuses("BLOB_VERSION")

    def test_changed_library_list_is_refused(self) -> None:
        with open(self.tree / "tools/aot/libraries.txt", "a") as handle:
            handle.write("library /system/lib64/libm.so\n")
        self.refuses("tools/aot/libraries.txt")

    def test_extra_missing_and_linked_files_are_refused(self) -> None:
        (self.artifact / "extra.txt").write_text("x")
        self.refuses("extra")
        (self.artifact / "extra.txt").unlink()
        (self.artifact / "licences.json").rename(self.scratch / "moved")
        self.refuses("missing")
        os.symlink(self.scratch / "moved", self.artifact / "licences.json")
        self.refuses("not a regular file")

    def test_malformed_description_is_refused(self) -> None:
        self.rewrite_description(lambda d: d.update(extra=1))
        self.refuses("keys differ")
        self.rewrite_description(lambda d: (d.pop("extra"),
                                            d.update(schema=2)))
        self.refuses("schema")
        self.rewrite_description(lambda d: (d.update(schema=1),
                                            d["blob"].update(sha256="zz")))
        self.refuses("blob is malformed")

    def test_repeated_json_key_is_refused(self) -> None:
        text = (self.artifact / "artifact.json").read_text()
        (self.artifact / "artifact.json").write_text(
            text.replace('"schema": 1\n', '"schema": 1,\n  "schema": 1\n'))
        self.refuses("repeated JSON key")

    def test_descriptor_round_trip_and_mismatch(self) -> None:
        digest = R.sha256_file(self.artifact / "artifact.json")
        text = R.render_accepted(self.description, digest)
        descriptor = self.scratch / "accepted-aot.env"
        descriptor.write_text(text)
        self.check(accepted=descriptor)
        self.assertEqual(R.read_accepted(descriptor)[
            "LINEAGE24_ACCEPTED_AOT_BLOB_VERSION"], "3")
        descriptor.write_text(text.replace(self.description["blob"]["sha256"],
                                           "11" * 32))
        self.refuses("ACCEPTED_AOT_BLOB_SHA256", accepted=descriptor)
        descriptor.write_text(text + "LINEAGE24_ACCEPTED_AOT_EXTRA=1\n")
        self.refuses("keys differ", accepted=descriptor)

    def test_archives_switch_is_a_descriptor_field_the_artifact_check_ignores(
            self) -> None:
        digest = R.sha256_file(self.artifact / "artifact.json")
        descriptor = self.scratch / "accepted-aot.env"
        for state in R.ARCHIVE_STATES:
            descriptor.write_text(
                R.render_accepted(self.description, digest, state))
            self.assertEqual(R.read_accepted(descriptor)[R.ARCHIVES_KEY],
                             state)
            self.check(accepted=descriptor)
        with self.assertRaises(R.ArtifactError):
            R.render_accepted(self.description, digest, "maybe")
        descriptor.write_text(R.render_accepted(self.description, digest)
                              .replace("ARCHIVES='no'", "ARCHIVES='soon'"))
        with self.assertRaises(R.ArtifactError):
            R.read_accepted(descriptor)

    def test_generator_changes_names_the_files_in_tree_order(self) -> None:
        self.assertEqual(R.generator_changes(self.description, self.tree), [])
        (self.tree / "tools/aot/rewrite.py").write_text("BLOB_VERSION = 2\n#")
        (self.tree / "tools/aot/asm.py").write_text("changed")
        (self.tree / "tools/aot/licences.py").unlink()
        self.assertEqual(
            R.generator_changes(self.description, self.tree),
            ["tools/aot/asm.py", "tools/aot/licences.py",
             "tools/aot/rewrite.py"])

    def test_install_checks_then_copies_once(self) -> None:
        destination = self.scratch / "aot"
        installed = R.install(self.artifact, self.release, destination,
                              repo=self.tree)
        self.assertEqual(installed.name, f"lineage24-mainline6.18-4k-"
                         f"{RELEASE_ID}")
        self.assertEqual(sorted(path.name for path in installed.iterdir()),
                         sorted(R.ARTIFACT_FILES))
        with self.assertRaises(R.ArtifactError):
            R.install(self.artifact, self.release, destination,
                      repo=self.tree)
        other = make_release(self.scratch / "other", system_sha="cd" * 32)
        with self.assertRaises(R.ArtifactError):
            R.install(self.artifact, other, self.scratch / "elsewhere",
                      repo=self.tree)
        self.assertFalse((self.scratch / "elsewhere").exists())

    def test_trees_for_names_the_images_beyond_system(self) -> None:
        self.assertEqual(R.trees_for([
            "/system/lib64/libz.so",
            "/apex/com.android.art/lib64/libart.so",
            "/vendor/lib64/a.so", "/apex/com.android.art/bin/dex2oat64",
            "/odm/lib64/b.so", "/vendor/lib64/c.so"]), [
                ("apex", "com.android.art"), ("partition", "vendor"),
                ("partition", "odm")])

    def test_command_line_reports_failure(self) -> None:
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            status = R.main(["verify", "--artifact", str(self.artifact),
                             "--no-generator", "--release",
                             str(make_release(self.scratch / "wrong",
                                              system_sha="cd" * 32))])
        self.assertEqual(status, 1)
        self.assertIn("system_partition_sha256", errors.getvalue())
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = R.main(["verify", "--artifact", str(self.artifact),
                             "--no-generator", "--release",
                             str(self.release)])
        self.assertEqual((status, output.getvalue().strip()),
                         (0, f"AOT artifact of {RELEASE_ID} verified"))

    def test_library_list_of_the_repository_parses(self) -> None:
        libraries, boot = R.library_list(R.repository_root())
        self.assertGreater(len(libraries), 40)
        self.assertEqual(len(libraries), len(set(libraries)))
        self.assertEqual(boot, ["/system/framework/arm64/boot*.oat"])
        self.assertEqual(R.blob_version(R.repository_root()), 3)


if __name__ == "__main__":
    unittest.main()
