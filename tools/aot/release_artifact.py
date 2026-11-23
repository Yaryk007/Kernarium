#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AOT artifact of an Android release: build, verify, describe, install.

An artifact is the blob tools/aot/rewrite.py generates from one release's
system image (and the APEX and partition images it holds), kept as a
directory beside the release that names it. It is the only form in which the
AOT component reaches a product: a build wraps the blob in
UmlArm64Aot.framework and generates nothing.

    blob.bin        the rewritten libraries
    manifest.json   what the blob holds (rewrite.py's manifest)
    licences.json   the notice each library states in the image
    artifact.json   what ties the three to the release and to the tools

artifact.json names the release (its ID, the SHA-256 of its manifest and of
the system partition the libraries were read from), the digest and layout
version of the blob, the digest of the other two files, the libraries the
rewrite was asked for, and the SHA-256 of every file the rewrite is a function
of (the rewriter, its tables and tools/aot/libraries.txt). A blob built from
other inputs than the tree being built fails the check, so an artifact never
carries a change it was not built with.

`build` runs on the Mac against an installed release and needs an fsck.erofs
that decodes Zstandard (README.md). `verify` reads the directory, and the
release when one is named; it runs no tool. `describe` prints the descriptor
(android/lineage-24/metadata/accepted-aot.env) that accepts the artifact, and
`verify --accepted` checks the artifact against it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ARTIFACT_SCHEMA = 1
ARTIFACT_FILES = ("artifact.json", "blob.bin", "licences.json", "manifest.json")
BLOB_MAGIC = b"UMLAOT16"
# The release fields of the blob header, which rewrite.py writes and the
# runtime compares with the release a launch boots: the ID, then the SHA-256
# of the release's manifest and of its system partition.
BLOB_RELEASE_ID_OFFSET = 0x30
BLOB_RELEASE_ID_BYTES = 96
BLOB_MANIFEST_SHA256_OFFSET = BLOB_RELEASE_ID_OFFSET + BLOB_RELEASE_ID_BYTES
BLOB_SYSTEM_SHA256_OFFSET = BLOB_MANIFEST_SHA256_OFFSET + 32
BLOB_HEADER_BYTES = BLOB_SYSTEM_SHA256_OFFSET + 32
# Every file the blob is a function of, relative to the repository root.
GENERATOR_INPUTS = (
    "tools/aot/a64decode.py",
    "tools/aot/asm.py",
    "tools/aot/elf64.py",
    "tools/aot/extract_image.py",
    "tools/aot/libraries.txt",
    "tools/aot/licences.py",
    "tools/aot/rewrite.py",
    "tests/unit/arm64/a64_instruction_decode_partitions.tsv",
    "darwin/cpu/arm64/layout/assembly_offsets.h",
)
ACCEPTED_DESCRIPTOR = "android/lineage-24/metadata/accepted-aot.env"
ACCEPTED_FIELDS = (
    ("LINEAGE24_ACCEPTED_AOT_SCHEMA", True),
    ("LINEAGE24_ACCEPTED_AOT_RELEASE_ID", False),
    ("LINEAGE24_ACCEPTED_AOT_ARTIFACT_SHA256", False),
    ("LINEAGE24_ACCEPTED_AOT_BLOB_SHA256", False),
    ("LINEAGE24_ACCEPTED_AOT_BLOB_VERSION", True),
    ("LINEAGE24_ACCEPTED_AOT_ARCHIVES", False),
)
# The one switch for an archive of Andarium's Release configuration: the
# component is in its iOS device runtimes, and allowed in its app, only when the
# descriptor says "yes". Every other product takes an accepted artifact.
ARCHIVES_KEY = "LINEAGE24_ACCEPTED_AOT_ARCHIVES"
ARCHIVE_STATES = ("no", "yes")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")


class ArtifactError(Exception):
    pass


def fail(message: str) -> None:
    raise ArtifactError(message)


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while chunk := handle.read(8 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def _no_duplicates(pairs: list) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            fail(f"repeated JSON key {key!r}")
        result[key] = value
    return result


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"),
                           object_pairs_hook=_no_duplicates)
    except (OSError, ValueError) as error:
        fail(f"{path}: {error}")
    if not isinstance(value, dict):
        fail(f"{path}: not a JSON object")
    return value


def library_list(repo: Path) -> tuple[list[str], list[str]]:
    """The libraries and the boot-code patterns of tools/aot/libraries.txt."""
    libraries: list[str] = []
    boot: list[str] = []
    for line in (repo / "tools/aot/libraries.txt").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        kind, _, path = line.partition(" ")
        if kind not in ("library", "boot") or not path.startswith("/") or \
                " " in path:
            fail(f"tools/aot/libraries.txt: unrecognised line: {line}")
        (libraries if kind == "library" else boot).append(path)
    return libraries, boot


def blob_version(repo: Path) -> int:
    """BLOB_VERSION of the rewriter in the tree."""
    match = re.search(r"^BLOB_VERSION = (\d+)$",
                      (repo / "tools/aot/rewrite.py").read_text(), re.M)
    if match is None:
        fail("tools/aot/rewrite.py states no BLOB_VERSION")
    return int(match.group(1))


def generator_digests(repo: Path) -> dict[str, str]:
    try:
        return {path: sha256_file(repo / path) for path in GENERATOR_INPUTS}
    except OSError as error:
        fail(f"cannot read a generator input: {error}")


def generator_changes(description: dict, repo: Path) -> list[str]:
    """The files the rewrite is a function of that are not what the artifact
    was built with, in tree order; a file the artifact does not record or the
    tree lacks counts."""
    recorded = description["generator"]
    changed = []
    for path in GENERATOR_INPUTS:
        try:
            current = sha256_file(repo / path)
        except OSError:
            current = None
        if recorded.get(path) != current:
            changed.append(path)
    return changed


def release_identity(release: Path) -> dict:
    """What a release states that an artifact binds to."""
    manifest_path = release / "release-manifest.json"
    layout_path = release / "guest/partition-layout.json"
    manifest = load_json(manifest_path)
    layout = load_json(layout_path)
    release_id = manifest.get("release_id")
    if not isinstance(release_id, str) or not RELEASE_ID_RE.match(release_id):
        fail(f"{manifest_path}: no valid release_id")
    partitions = [entry for entry in layout.get("system_disk", {}).get(
        "partitions", []) if entry.get("name") == "system"]
    if len(partitions) != 1 or not SHA256_RE.match(
            str(partitions[0].get("sha256"))):
        fail(f"{layout_path}: no system partition digest")
    return {
        "release_id": release_id,
        "directory_name": manifest.get("directory_name"),
        "release_manifest_sha256": sha256_file(manifest_path),
        "system_partition_sha256": partitions[0]["sha256"],
    }


def read_blob_header(blob: Path) -> tuple[bytes, int, dict]:
    """The magic, the layout version and the release the header names (ID,
    manifest digest, system partition digest) of a blob."""
    with open(blob, "rb") as handle:
        return parse_blob_header(handle.read(BLOB_HEADER_BYTES))


def parse_blob_header(header: bytes) -> tuple[bytes, int, dict]:
    """read_blob_header of a blob's first bytes, wherever they came from (the
    code section of a packaged framework)."""
    if len(header) < BLOB_HEADER_BYTES:
        fail("blob.bin is shorter than its header")
    magic, version = struct.unpack_from("<8sI", header, 0)
    raw = header[BLOB_RELEASE_ID_OFFSET:BLOB_MANIFEST_SHA256_OFFSET]
    return magic, version, {
        "release_id": raw.split(b"\0", 1)[0].decode("ascii", "replace"),
        "release_manifest_sha256": header[
            BLOB_MANIFEST_SHA256_OFFSET:BLOB_SYSTEM_SHA256_OFFSET].hex(),
        "system_partition_sha256": header[
            BLOB_SYSTEM_SHA256_OFFSET:BLOB_HEADER_BYTES].hex(),
    }


def _check_regular_files(artifact: Path) -> None:
    if artifact.is_symlink() or not artifact.is_dir():
        fail(f"{artifact}: not a regular directory")
    names = set()
    for entry in artifact.iterdir():
        mode = entry.lstat().st_mode
        if not stat.S_ISREG(mode):
            fail(f"{artifact}: {entry.name} is not a regular file")
        names.add(entry.name)
    if names != set(ARTIFACT_FILES):
        fail(f"{artifact}: files differ from the artifact's "
             f"(missing={sorted(set(ARTIFACT_FILES) - names)}, "
             f"extra={sorted(names - set(ARTIFACT_FILES))})")


ARTIFACT_KEYS = {"schema", "release", "blob", "files", "libraries",
                 "generator"}
RELEASE_KEYS = ("release_id", "release_manifest_sha256",
                "system_partition_sha256")
BLOB_KEYS = {"bytes", "sha256", "magic", "version"}


def verify(artifact: Path, *, repo: Path | None = None,
           release: Path | None = None, accepted: Path | None = None,
           generator: bool = True) -> dict:
    """Check an artifact directory and return its artifact.json.

    With `release`, the artifact must be the one built from that release's
    system partition. With `accepted`, it must be the one the descriptor
    names. With `generator`, every file the rewrite is a function of must be
    what the artifact was built with."""
    repo = repo or repository_root()
    _check_regular_files(artifact)
    description = load_json(artifact / "artifact.json")
    if set(description) != ARTIFACT_KEYS:
        fail("artifact.json keys differ from the artifact contract")
    if description["schema"] != ARTIFACT_SCHEMA:
        fail(f"artifact.json schema is not {ARTIFACT_SCHEMA}")
    bound = description["release"]
    if not isinstance(bound, dict) or set(bound) != set(RELEASE_KEYS) or \
            not RELEASE_ID_RE.match(str(bound["release_id"])) or \
            not SHA256_RE.match(str(bound["release_manifest_sha256"])) or \
            not SHA256_RE.match(str(bound["system_partition_sha256"])):
        fail("artifact.json release is malformed")
    blob = description["blob"]
    if not isinstance(blob, dict) or set(blob) != BLOB_KEYS or \
            not SHA256_RE.match(str(blob["sha256"])) or \
            blob["magic"] != BLOB_MAGIC.decode() or \
            not isinstance(blob["version"], int) or \
            not isinstance(blob["bytes"], int):
        fail("artifact.json blob is malformed")
    files = description["files"]
    if not isinstance(files, dict) or set(files) != {"licences.json",
                                                    "manifest.json"} or \
            not all(SHA256_RE.match(str(value)) for value in files.values()):
        fail("artifact.json files are malformed")
    libraries = description["libraries"]
    if not isinstance(libraries, dict) or set(libraries) != {"library",
                                                              "boot"} or \
            not all(isinstance(value, list) and all(
                isinstance(item, str) for item in value)
                    for value in libraries.values()):
        fail("artifact.json libraries are malformed")

    blob_path = artifact / "blob.bin"
    if blob_path.stat().st_size != blob["bytes"] or \
            sha256_file(blob_path) != blob["sha256"]:
        fail("blob.bin differs from the artifact's digest")
    for name, digest in files.items():
        if sha256_file(artifact / name) != digest:
            fail(f"{name} differs from the artifact's digest")
    magic, version, header_release = read_blob_header(blob_path)
    if magic != BLOB_MAGIC or version != blob["version"]:
        fail("blob.bin header differs from artifact.json")
    for key in RELEASE_KEYS:
        if header_release[key] != bound[key]:
            fail(f"blob.bin names another release than artifact.json ({key})")
    manifest = load_json(artifact / "manifest.json")
    if manifest.get("release_id") != bound["release_id"] or \
            manifest.get("blob_bytes") != blob["bytes"]:
        fail("manifest.json differs from the blob")
    licences = load_json(artifact / "licences.json")
    if licences.get("release_id") != bound["release_id"]:
        fail("licences.json names another release")

    if generator:
        recorded = description["generator"]
        if not isinstance(recorded, dict):
            fail("artifact.json generator is malformed")
        current = generator_digests(repo)
        if set(recorded) != set(current):
            fail("artifact.json names other generator inputs than the tree")
        changed = generator_changes(description, repo)
        if changed:
            fail("the artifact was built with other generator inputs than "
                 f"this tree's: {', '.join(changed)}")
        if blob["version"] != blob_version(repo):
            fail("the blob's layout version is not the tree's BLOB_VERSION")
        listed_libraries, listed_boot = library_list(repo)
        if libraries != {"library": listed_libraries, "boot": listed_boot}:
            fail("the artifact's library list is not tools/aot/libraries.txt")

    if release is not None:
        identity = release_identity(release)
        for key in RELEASE_KEYS:
            if bound[key] != identity[key]:
                fail(f"the artifact was not built from this release ({key})")
    if accepted is not None:
        expected = read_accepted(accepted)
        values = accepted_values(description,
                                 sha256_file(artifact / "artifact.json"))
        for key, _ in ACCEPTED_FIELDS:
            if key != ARCHIVES_KEY and expected[key] != values[key]:
                fail(f"the artifact differs from accepted descriptor {key}")
    return description


def accepted_values(description: dict, artifact_json_sha256: str,
                    archives: str = "no") -> dict[str, str]:
    return {
        "LINEAGE24_ACCEPTED_AOT_SCHEMA": str(description["schema"]),
        "LINEAGE24_ACCEPTED_AOT_RELEASE_ID":
            description["release"]["release_id"],
        "LINEAGE24_ACCEPTED_AOT_ARTIFACT_SHA256": artifact_json_sha256,
        "LINEAGE24_ACCEPTED_AOT_BLOB_SHA256": description["blob"]["sha256"],
        "LINEAGE24_ACCEPTED_AOT_BLOB_VERSION":
            str(description["blob"]["version"]),
        ARCHIVES_KEY: archives,
    }


def render_accepted(description: dict, artifact_json_sha256: str,
                    archives: str = "no") -> str:
    if archives not in ARCHIVE_STATES:
        fail(f"archives must be one of {ARCHIVE_STATES}")
    values = accepted_values(description, artifact_json_sha256, archives)
    lines = ["# SPDX-License-Identifier: GPL-2.0-or-later",
             "# Accepted AOT artifact of an Android release "
             "(tools/aot/release_artifact.py)."]
    for key, numeric in ACCEPTED_FIELDS:
        lines.append(f"{key}={values[key]}" if numeric
                     else f"{key}='{values[key]}'")
    return "\n".join(lines) + "\n"


def read_accepted(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"([A-Z0-9_]+)=(?:'([^']*)'|([0-9]+))", line)
        if match is None:
            fail(f"{path}:{number}: unrecognised line")
        key = match.group(1)
        if key in values:
            fail(f"{path}:{number}: repeated key {key}")
        values[key] = match.group(2) if match.group(2) is not None \
            else match.group(3)
    if set(values) != {key for key, _ in ACCEPTED_FIELDS}:
        fail(f"{path}: keys differ from the accepted-artifact contract")
    if values[ARCHIVES_KEY] not in ARCHIVE_STATES:
        fail(f"{path}: {ARCHIVES_KEY} is not one of {ARCHIVE_STATES}")
    return values


def trees_for(libraries: list[str]) -> list[tuple[str, str]]:
    """The images beyond the system partition the libraries come from, in
    first-use order: ("apex", name) for a path under /apex/<name>, and
    ("partition", name) for one under /vendor, /product, /system_ext or
    /odm (darwin/cpu/arm64/aot/CMakeLists.txt does the same for a build that
    generates)."""
    trees: list[tuple[str, str]] = []
    for library in libraries:
        apex = re.match(r"^/apex/([^/]+)/", library)
        partition = re.match(r"^/(vendor|product|system_ext|odm)/", library)
        tree = ("apex", apex.group(1)) if apex else \
            ("partition", partition.group(1)) if partition else None
        if tree and tree not in trees:
            trees.append(tree)
    return trees


def _run(arguments: list[str]) -> None:
    subprocess.run(arguments, check=True)


def write_description(staging: Path, identity: dict, libraries: list[str],
                      boot: list[str], repo: Path) -> dict:
    """Write artifact.json for the blob, manifest and notices in `staging`."""
    magic, version, _ = read_blob_header(staging / "blob.bin")
    description = {
        "schema": ARTIFACT_SCHEMA,
        "release": {key: identity[key] for key in RELEASE_KEYS},
        "blob": {"bytes": (staging / "blob.bin").stat().st_size,
                 "sha256": sha256_file(staging / "blob.bin"),
                 "magic": magic.decode("ascii", "replace"),
                 "version": version},
        "files": {name: sha256_file(staging / name)
                  for name in ("licences.json", "manifest.json")},
        "libraries": {"library": libraries, "boot": boot},
        "generator": generator_digests(repo),
    }
    (staging / "artifact.json").write_text(
        json.dumps(description, indent=2, sort_keys=True) + "\n")
    return description


def build(release: Path, fsck_erofs: Path, out: Path, *,
          repo: Path | None = None, jobs: int = 0) -> dict:
    """Build the artifact of an installed release into `out` (which must not
    exist)."""
    repo = repo or repository_root()
    if out.exists() or out.is_symlink():
        fail(f"{out} exists")
    identity = release_identity(release)
    libraries, boot = library_list(repo)
    tools = repo / "tools/aot"
    python = sys.executable
    out.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f".{out.name}.", dir=out.parent))
    try:
        extract = [python, "-B", str(tools / "extract_image.py"),
                   "--release", str(release), "--fsck-erofs", str(fsck_erofs)]
        image = work / "image"
        _run([*extract, "--out", str(image)])
        mounts: list[str] = []
        for kind, name in trees_for(libraries):
            directory = work / f"{kind}-{name}"
            if kind == "apex":
                _run([*extract, "--apex", name, "--system-root",
                      str(image / "root"), "--out", str(directory)])
                mounts += ["--mount", f"/apex/{name}={directory}/root"]
            else:
                _run([*extract, "--partition", name, "--out",
                      str(directory)])
                mounts += ["--mount", f"/{name}={directory}/root"]
        rewritten = work / "rewritten"
        command = [python, "-B", str(tools / "rewrite.py"),
                   "--root", str(image / "root"), *mounts,
                   "--release-id", identity["release_id"],
                   "--release-manifest-sha256",
                   identity["release_manifest_sha256"],
                   "--system-partition-sha256",
                   identity["system_partition_sha256"],
                   "--out", str(rewritten)]
        for path in (*libraries, *boot):
            command += ["--library", path]
        if jobs:
            command += ["--jobs", str(jobs)]
        _run(command)
        staging = work / "artifact"
        staging.mkdir()
        for name in ("blob.bin", "licences.json", "manifest.json"):
            shutil.copyfile(rewritten / name, staging / name)
        description = write_description(staging, identity, libraries, boot,
                                        repo)
        verify(staging, repo=repo, release=release)
        os.replace(staging, out)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return description


def install(artifact: Path, release: Path, destination_root: Path, *,
            repo: Path | None = None, accepted: Path | None = None) -> Path:
    """Verify an artifact against the release it was built from and copy it
    below `destination_root`, in a directory named like the release's."""
    repo = repo or repository_root()
    identity = release_identity(release)
    verify(artifact, repo=repo, release=release, accepted=accepted)
    name = identity["directory_name"]
    if not isinstance(name, str) or "/" in name or name in ("", ".", ".."):
        fail("the release names no directory")
    if destination_root.is_symlink():
        fail("destination root must not be a symlink")
    destination = destination_root / name
    if destination.exists() or destination.is_symlink():
        fail("the AOT artifact is already installed")
    destination_root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{name}.", dir=destination_root))
    try:
        for file in ARTIFACT_FILES:
            shutil.copyfile(artifact / file, staging / file,
                            follow_symlinks=False)
        verify(staging, repo=repo, release=release, accepted=accepted)
        os.replace(staging, destination)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return destination


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--release", type=Path, required=True)
    build_parser.add_argument("--fsck-erofs", type=Path, required=True)
    build_parser.add_argument("--out", type=Path, required=True)
    build_parser.add_argument("--jobs", type=int, default=0)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--artifact", type=Path, required=True)
    verify_parser.add_argument("--release", type=Path)
    verify_parser.add_argument("--accepted", type=Path)
    verify_parser.add_argument("--no-generator", action="store_true")
    describe_parser = commands.add_parser("describe")
    describe_parser.add_argument("--artifact", type=Path, required=True)
    describe_parser.add_argument("--archives", choices=ARCHIVE_STATES,
                                 default="no", help="whether the archive "
                                 "configurations may carry the component")
    install_parser = commands.add_parser("install")
    install_parser.add_argument("--artifact", type=Path, required=True)
    install_parser.add_argument("--release", type=Path, required=True)
    install_parser.add_argument("--destination-root", type=Path)
    arguments = parser.parse_args(argv)
    repo = repository_root()
    try:
        if arguments.command == "build":
            description = build(arguments.release.resolve(),
                                arguments.fsck_erofs.resolve(),
                                arguments.out.resolve(), jobs=arguments.jobs)
            print(f"built the AOT artifact of "
                  f"{description['release']['release_id']}: "
                  f"{description['blob']['bytes']} bytes, sha256 "
                  f"{description['blob']['sha256']}")
        elif arguments.command == "verify":
            description = verify(
                arguments.artifact.resolve(),
                release=arguments.release.resolve()
                if arguments.release else None,
                accepted=arguments.accepted.resolve()
                if arguments.accepted else None,
                generator=not arguments.no_generator)
            print(f"AOT artifact of {description['release']['release_id']} "
                  "verified")
        elif arguments.command == "describe":
            artifact = arguments.artifact.resolve()
            description = verify(artifact)
            sys.stdout.write(render_accepted(
                description, sha256_file(artifact / "artifact.json"),
                arguments.archives))
        else:
            root = arguments.destination_root or repo / ".build/android/aot"
            destination = install(arguments.artifact.resolve(),
                                  arguments.release.resolve(), root.resolve())
            print(f"installed AOT artifact: {destination}")
    except ArtifactError as error:
        print(f"release_artifact.py: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"release_artifact.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
