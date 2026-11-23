#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the accepted Android release as the system image Andarium carries.

The application bundles the staged directory as it stands, laid out as the
installed release: the read-only system disk `guest/system.img`, its
`guest/partition-layout.json`, `boot/kernel-command-line.txt` and
`release-manifest.json`, and, when the release carries them, the Magisk root
files under `root/` (a patched copy of its `init_boot` and the official APK),
which are checked against the manifest's root record, the release's own
`init_boot` and the APK. The runtime boots from that directory and creates the
writable data disk beside the guest's own files. The staged files are copies of
an accepted release, which is checked against its own manifest and layout; this
script neither builds nor signs anything.

A developer build can stage an unsigned proof image instead (`--release DIR
--unsigned-proof`): the proof of a change before a release carries it, built
from unsigned target files. It is staged with a manifest that still says it is
an unsigned proof, and an archive of the Release configuration refuses it
(`scripts/check-embedded-artifacts.py`).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ACCEPTANCE = ROOT / "android/lineage-24/metadata/accepted-release.env"
DEFAULT_RELEASES = ROOT / ".build/android/releases"
DEFAULT_OUTPUT = ROOT / ".build/andarium-system"
# The tracked metadata of this checkout that an unsigned proof's missing fields
# come from: the image line's, and the accepted Android kernel's, whose runtime
# the application packages.
IMAGE_METADATA_RELATIVE = "android/lineage-24/metadata/lineage.env"
KERNEL_DESCRIPTOR_RELATIVE = "kernel/mainline-6.18/artifacts/accepted-android-4k.env"
IMAGE_METADATA = ROOT / IMAGE_METADATA_RELATIVE
KERNEL_DESCRIPTOR = ROOT / KERNEL_DESCRIPTOR_RELATIVE
DISK_TOOL = ROOT / "android/lineage-24/tools/ubd-disk.py"
ROOT_VERIFIER = ROOT / "android/lineage-24/tools/verify-magisk-boot.py"
for _tools in (ROOT / "kernel/common/artifacts", ROOT / "android/lineage-24/tools"):
    if str(_tools) not in sys.path:
        sys.path.insert(0, str(_tools))
import contract_checks  # noqa: E402
import magisk_image  # noqa: E402

MANIFEST = "release-manifest.json"
COMMAND_LINE = "boot/kernel-command-line.txt"
PARTITION_LAYOUT = "guest/partition-layout.json"
SYSTEM_DISK = "guest/system.img"
SYSTEM_DISK_ROLE = "system-disk"
FILES = (MANIFEST, COMMAND_LINE, PARTITION_LAYOUT, SYSTEM_DISK)
ROOT_DIRECTORY = "root"
ROOT_RECORD = "root"
LAYOUT_SCHEMA = 2
GUEST_PAGE_SIZE = 4096
UNSIGNED_PROOF = "unsigned-proof"
PROOF_ID_PREFIX = "unsigned-proof-"
# What the application accepts as a release ID (ReleaseIdentity.releaseIDIsValid).
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
# The most digits the instance record takes for a build number (InstanceState).
MAX_BUILD_NUMBER = 9_999_999_999


class StageError(Exception):
    pass


def fail(message: str) -> None:
    raise SystemExit(f"Andarium/scripts/stage-system-image.py: {message}")


def read_strict_json(path: Path) -> dict:
    """Reads a JSON object, refusing a key repeated in any object."""
    return contract_checks.load_json_object(path, StageError)


def check_release_directories(release: Path) -> None:
    """Each directory of the release that the script reads or copies from is a real
    one, a link to none. This runs before the first read, so nothing is read, sized
    or hashed through a linked directory and then refused. A directory that is
    missing is left to the checks of the files in it."""
    below = {PurePosixPath(relative).parent.as_posix() for relative in FILES} | {ROOT_DIRECTORY}
    for relative in sorted(below - {"."}):
        if os.path.lexists(release / relative):
            magisk_image.real_directory(release / relative, f"the release's {relative}/", StageError)


def read_manifest(release: Path) -> bytes:
    """The release manifest's bytes, read once through a regular file that is no link.
    Everything the script checks, stages or derives is taken from these bytes."""
    return magisk_image.read_file(
        release / MANIFEST, magisk_image.MAX_MANIFEST, "the release manifest", StageError)


def parse_object(data: bytes, label: str, *, strict: bool = False) -> dict:
    """A JSON object from bytes already read; with `strict`, a key repeated in any
    object is refused."""

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"repeats the key {key}")
            result[key] = value
        return result

    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=pairs if strict else None)
    except (UnicodeError, ValueError) as error:
        raise StageError(f"cannot read {label}: {error}") from error
    if not isinstance(value, dict):
        raise StageError(f"{label} is not a JSON object")
    return value


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise StageError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise StageError(f"{path} is not a JSON object")
    return value


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def check_release(release: Path, manifest: dict, accepted: dict[str, str] | None) -> None:
    """Checks the release against its manifest and layout; raises StageError."""
    if manifest.get("acceptance") != "accepted":
        raise StageError("the release is not accepted")
    android = manifest.get("android")
    kernel = manifest.get("kernel")
    if not isinstance(android, dict) or not isinstance(kernel, dict):
        raise StageError("the manifest has no android or kernel section")
    if kernel.get("page_size") != GUEST_PAGE_SIZE:
        raise StageError(f"the release is not a {GUEST_PAGE_SIZE}-byte-page release")
    if not str(manifest.get("product_family", "")).startswith("lineage-"):
        raise StageError("the release is not a LineageOS product family")
    if accepted is not None:
        expected = {
            "release_id": accepted["LINEAGE24_ACCEPTED_RELEASE_ID"],
            "product_family": accepted["LINEAGE24_ACCEPTED_PRODUCT_FAMILY"],
        }
        for key, value in expected.items():
            if manifest.get(key) != value:
                raise StageError(f"{key} {manifest.get(key)!r} is not the accepted {value!r}")
        if str(android.get("major")) != accepted["LINEAGE24_ACCEPTED_ANDROID_MAJOR"]:
            raise StageError("the Android major version is not the accepted one")

    inventory = manifest.get("inventory")
    if not isinstance(inventory, list):
        raise StageError("the manifest has no inventory")
    by_path = {entry.get("path"): entry for entry in inventory if isinstance(entry, dict)}
    disks = [entry for entry in by_path.values() if entry.get("role") == SYSTEM_DISK_ROLE]
    if len(disks) != 1 or disks[0].get("path") != SYSTEM_DISK:
        raise StageError(f"expected one {SYSTEM_DISK_ROLE} at {SYSTEM_DISK} in the inventory")
    for relative in (COMMAND_LINE, PARTITION_LAYOUT, SYSTEM_DISK):
        path = release / relative
        if not path.is_file() or path.is_symlink():
            raise StageError(f"missing or unsafe release file: {path}")
        entry = by_path.get(relative)
        if entry is None:
            raise StageError(f"{relative} is not in the manifest inventory")
        if path.stat().st_size != entry.get("size"):
            raise StageError(f"{relative} does not match the size in the manifest")

    check_layout(release)


def check_layout(release: Path, read=read_json) -> dict:
    """Checks the system disk against its partition layout; returns the layout."""
    layout = read(release / PARTITION_LAYOUT)
    if layout.get("schema_version") != LAYOUT_SCHEMA:
        raise StageError(f"the partition layout is not schema {LAYOUT_SCHEMA}")
    system = layout.get("system_disk")
    if not isinstance(system, dict):
        raise StageError("the partition layout has no system_disk")
    if (release / SYSTEM_DISK).stat().st_size != system.get("size_bytes"):
        raise StageError("the system disk does not match the size in the partition layout")
    if sha256_of(release / SYSTEM_DISK) != system.get("sha256"):
        raise StageError("the system disk does not match the sha256 in the partition layout")
    return layout


def check_root(release: Path, manifest: dict) -> list[str]:
    """Checks the release's Magisk root files; returns the paths to stage, none for
    a release without them.

    The manifest is the one the script read, and its root record is validated here,
    in the process that goes on to use it: the paths it returns are the two canonical
    ones. The files are then checked by android/lineage-24/tools/verify-magisk-boot.py
    (the record, both files, and the patched image against the release's own
    init_boot and the APK), the same check the installer runs, which stage() runs
    again on the staged copy; the pinned Magisk release is the producer's rule, not
    asked here. A root/ directory without a record is refused.
    """
    record = manifest.get(ROOT_RECORD)
    if record is None:
        if os.path.lexists(release / ROOT_DIRECTORY):
            raise StageError("the release has a root/ its manifest does not declare a root record for")
        return []
    magisk_image.validate_root_record(record, lambda message: StageError(f"the manifest's root record: {message}"))
    verify_root_files(release)
    return [record["boot_image"]["path"], record["apk"]["path"]]


def verify_root_files(release: Path) -> None:
    verified = subprocess.run(
        [sys.executable, "-B", str(ROOT_VERIFIER), "--release", str(release)],
        capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )
    if verified.returncode != 0:
        detail = (verified.stderr.strip() or verified.stdout.strip()).splitlines()
        raise StageError(f"the release's root files do not verify: {detail[-1] if detail else 'no reason given'}")


def is_root_destination(relative: str) -> bool:
    """One of the two paths a root file is staged at."""
    prefix, suffix = f"{magisk_image.ROOT_DIRECTORY}/Magisk-", ".apk"
    return relative == magisk_image.PATCHED_PATH or (
        relative.startswith(prefix) and relative.endswith(suffix)
        and magisk_image.VERSION_RE.fullmatch(relative[len(prefix):-len(suffix)]) is not None
    )


def tracked_line(acceptance: Path, image_metadata: Path, kernel_descriptor: Path) -> dict:
    """What this checkout's tracked metadata says of the image line an unsigned
    proof belongs to and of the kernel whose runtime the application packages."""
    accepted = contract_checks.parse_assignments(acceptance, StageError)
    image = contract_checks.parse_assignments(image_metadata, StageError)
    kernel = contract_checks.parse_assignments(kernel_descriptor, StageError)
    try:
        line = {
            "product_family": accepted["LINEAGE24_ACCEPTED_PRODUCT_FAMILY"],
            "android_major": int(image["LINEAGE24_ANDROID_MAJOR"]),
            "kernel_release": kernel["UML_KERNEL_ARTIFACT_RELEASE"],
            "sources": [IMAGE_METADATA_RELATIVE, KERNEL_DESCRIPTOR_RELATIVE],
        }
        page_sizes = (int(image["LINEAGE24_KERNEL_PAGE_SIZE"]), int(kernel["UML_KERNEL_ARTIFACT_PAGE_SIZE"]))
    except (KeyError, ValueError) as error:
        raise StageError(f"the tracked image or kernel metadata lacks {error}") from error
    if page_sizes != (GUEST_PAGE_SIZE, GUEST_PAGE_SIZE):
        raise StageError(f"the tracked image line or Android kernel is not a {GUEST_PAGE_SIZE}-byte-page line")
    return line


def check_proof(release: Path, manifest: dict, line: dict) -> dict:
    """Checks an unsigned proof image; returns the manifest to stage.

    A proof is checked for what it can be checked for. What a signed release is
    checked for (check_release, and install-release.py when it is installed)
    and a proof cannot satisfy is named beside each check; nothing is skipped
    without saying so.
    """
    # Signed release: acceptance == "accepted". A proof is never accepted: the
    # caller has seen it say unsigned-proof, and the staged manifest keeps saying so.
    # Signed release: the release ID is the accepted one (with the default
    # path). A proof has an ID of its own, "unsigned-proof-" and the ID of the
    # release it is the proof of, so it can never stand for an accepted release.
    release_id = manifest.get("release_id")
    if not isinstance(release_id, str) or not release_id.startswith(PROOF_ID_PREFIX) \
            or release_id == PROOF_ID_PREFIX:
        raise StageError(f"the proof's release ID does not start with {PROOF_ID_PREFIX} and name a release")
    if not RELEASE_ID_RE.fullmatch(release_id):
        raise StageError("the proof's release ID is not one the application accepts")
    producer = manifest.get("producer")
    if not isinstance(producer, dict):
        raise StageError("the proof's manifest has no producer section")
    proof_for = producer.get("proof_for_release_id")
    if proof_for is not None and release_id != PROOF_ID_PREFIX + str(proof_for):
        raise StageError("the proof's release ID is not the proof of the release its producer names")
    # Signed release: the product family starts with lineage- and, with the
    # default path, is the accepted one. A proof is of the same line.
    if manifest.get("product_family") != line["product_family"]:
        raise StageError(
            f"the proof's product family {manifest.get('product_family')!r} is not this "
            f"checkout's {line['product_family']!r}")

    # Signed release: an android section (the accepted Android major among its
    # fields). The proof's has no major; the application needs one, so it is
    # taken from the tracked image line (derived) and a stated one must agree.
    android = manifest.get("android")
    if not isinstance(android, dict):
        raise StageError("the proof's manifest has no android section")
    # What the application's readers refuse, and nothing more: the release
    # identity (ReleaseIdentity) needs a fingerprint, a build number from 1 and
    # a security patch string; the instance record (InstanceState) refuses a
    # build number of more than ten digits; the system info (SystemInfo) reads
    # the build ID and the patch date if they are there and shows none otherwise.
    fingerprint = android.get("fingerprint")
    if not isinstance(fingerprint, str) or not fingerprint:
        raise StageError("the proof's android section has no fingerprint")
    number = android.get("build_number")
    if not isinstance(number, int) or isinstance(number, bool) or not 1 <= number <= MAX_BUILD_NUMBER:
        raise StageError(f"the proof's android build_number is not an integer of 1 to {MAX_BUILD_NUMBER}")
    if not isinstance(android.get("security_patch"), str):
        raise StageError("the proof's android security_patch is not a string")
    # A proof is built from the unsigned target files: it says so.
    if android.get("signed") is not False:
        raise StageError("the proof does not say its android build is unsigned")
    if "major" in android and android["major"] != line["android_major"]:
        raise StageError(f"the proof's Android major {android['major']!r} is not the tracked {line['android_major']}")

    # Signed release: kernel.page_size == 4096 (the 4 KiB line) and the kernel
    # build that qualified the target files. A proof records no kernel: the page
    # size is the tracked line's and the release the accepted Android kernel's,
    # which the runtime in the application is. A stated page size or release
    # must be the tracked one.
    kernel = manifest.get("kernel")
    if not isinstance(kernel, dict):
        raise StageError("the proof's manifest has no kernel section")
    if "page_size" in kernel and kernel["page_size"] != GUEST_PAGE_SIZE:
        raise StageError(f"the proof is not a {GUEST_PAGE_SIZE}-byte-page image")
    if "release" in kernel and kernel["release"] != line["kernel_release"]:
        raise StageError(
            f"the proof's kernel release {kernel['release']!r} is not the accepted Android "
            f"kernel's {line['kernel_release']!r}, whose runtime the application packages")

    # Signed release: the inventory lists the system disk, the layout and the
    # command line with their sizes. A proof has an empty inventory: only the
    # system disk is checked, against the partition layout (size and sha256, as
    # below); the layout and the command line are checked for being there, not
    # links, and the command line for a word in it. Their sizes and the command
    # line's content are not checked.
    for relative in (COMMAND_LINE, PARTITION_LAYOUT, SYSTEM_DISK):
        path = release / relative
        if not path.is_file() or path.is_symlink():
            raise StageError(f"missing or unsafe release file: {path}")
    # The runtime's reader refuses a command line with no word in it.
    if not (release / COMMAND_LINE).read_text(encoding="utf-8", errors="replace").split():
        raise StageError("the proof's kernel command line is empty")
    layout = check_layout(release, read_strict_json)
    # The proof's own record of the disk's digest, where it carries one (the
    # producer section's), is a second statement beside the layout's.
    recorded = producer.get("system_disk_sha256")
    if recorded is not None and recorded != layout["system_disk"]["sha256"]:
        raise StageError("the system disk does not match the sha256 the proof's producer recorded")
    # The disk's GPT and every partition's sha256, as the installer checks a
    # signed release. The tracked disk policy is not enforced (it is for a
    # release the tracked tree builds; a proof may come from another revision).
    verified = subprocess.run(
        [sys.executable, "-B", str(DISK_TOOL), "verify", "--layout", str(release / PARTITION_LAYOUT),
         "--disk", str(release / SYSTEM_DISK), "--hash"],
        capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )
    if verified.returncode != 0:
        detail = (verified.stderr.strip() or verified.stdout.strip()).splitlines()
        raise StageError(f"the system disk does not match its partition layout: {detail[-1] if detail else 'no reason given'}")

    # Not checked, because a proof cannot satisfy them or no consumer needs
    # them (proof_notes lists each): the signature (a proof is built from AOSP
    # test keys), the target files (outside the directory; the producer's digest
    # of them stands unverified), the input trees (outer.input_trees: a proof
    # records none), the source provenance (source: empty), the inventory, the
    # command line's content, the tracked disk policy, and the producer's own
    # digest of the disk when it records none. The revision it was built from
    # is reported, not required.
    staged = json.loads(json.dumps(manifest))
    derived = []
    for section, key, value in (
        ("android", "major", line["android_major"]),
        ("kernel", "page_size", GUEST_PAGE_SIZE),
        ("kernel", "release", line["kernel_release"]),
    ):
        if key not in staged[section]:
            staged[section][key] = value
            derived.append(f"{section}.{key}")
    staged["proof_staging"] = {"derived": derived, "from": line["sources"]}
    return staged


def revision_note(manifest: dict) -> str:
    """Whether the revision a proof was built from is in this checkout's history."""
    outer = manifest.get("outer")
    revision = outer.get("revision") if isinstance(outer, dict) else None
    if not isinstance(revision, str) or not REVISION_RE.fullmatch(revision):
        return "built from a revision it does not record"
    try:
        answer = subprocess.run(
            ["git", "-C", str(ROOT), "merge-base", "--is-ancestor", revision, "HEAD"],
            capture_output=True, check=False, stdin=subprocess.DEVNULL,
        ).returncode
    except OSError:
        answer = 128
    where = {
        0: "in this checkout's history",
        1: "not in this checkout's history",
    }.get(answer, "unknown to this checkout")
    dirty = " (a dirty work tree)" if outer.get("dirty") is True else ""
    return f"built from revision {revision[:12]}{dirty}, which is {where}"


def proof_notes(manifest: dict, staged: dict) -> list[str]:
    """What the person staging a proof is told beside the result."""
    derived = staged["proof_staging"]["derived"]
    unchecked = [
        "the signature (test keys)", "the target files", "the input trees", "the source provenance",
        "the inventory (the sizes of the command line and the layout file)", "the command line's content",
        "the tracked disk policy",
    ]
    if "system_disk_sha256" not in manifest["producer"]:
        unchecked.append("the producer's own disk digest (the proof records none)")
    notes = [
        "for a developer build only: a Release archive refuses it",
        revision_note(manifest),
        # InstanceRules.decide: a proof starts only a fresh instance (or one with no
        # data), and an instance it created starts only it; any other is a mismatch.
        "the application starts it only on a fresh or isolated guest instance "
        "(ANDARIUM_GUEST_INSTANCE=<name>): a guest with data of another system cannot start "
        "until Erase virtual machine, and a guest made from this proof cannot start another system",
    ]
    if derived:
        notes.append("taken from this checkout's tracked metadata, not from the proof: " + ", ".join(derived))
    notes.append("not checked: " + ", ".join(unchecked))
    return notes


def copy(source: Path, destination: Path) -> None:
    # A clone costs nothing on APFS; elsewhere the bytes are copied.
    cloned = subprocess.run(
        ["cp", "-c", str(source), str(destination)], capture_output=True, check=False
    )
    if cloned.returncode != 0:
        shutil.copyfile(source, destination)


def stage(release: Path, output: Path, manifest: bytes | None = None, extra: list[str] = ()) -> None:
    """Stages the release's files and the `extra` ones (its root files, only at their
    two canonical paths); `manifest` stands for the manifest's bytes. The staged
    copy is checked before it replaces the output."""
    for relative in extra:
        if not is_root_destination(relative):
            raise StageError(f"{relative} is not a root file destination")
    output.parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        for relative in (*FILES, *extra):
            (scratch / relative).parent.mkdir(parents=True, exist_ok=True)
            if relative == MANIFEST and manifest is not None:
                (scratch / relative).write_bytes(manifest)
            else:
                magisk_image.real_path_below(release, relative, StageError)
                copy(release / relative, scratch / relative)
        if extra:
            try:
                verify_root_files(scratch)
            except StageError as error:
                raise StageError(f"the staged root files do not verify: {error}") from error
        previous = None
        if output.exists():
            previous = Path(tempfile.mkdtemp(prefix=f".{output.name}.old.", dir=output.parent))
            os.rmdir(previous)
            os.rename(output, previous)
        try:
            os.rename(scratch, output)
        except OSError:
            if previous is not None:
                os.rename(previous, output)
            raise
        if previous is not None:
            shutil.rmtree(previous, ignore_errors=True)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=Path, help="a release directory (default: the accepted release)")
    parser.add_argument(
        "--unsigned-proof", action="store_true",
        help="stage the unsigned proof image --release names, for a developer build only: "
             "a Release archive refuses it",
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--acceptance", type=Path, default=DEFAULT_ACCEPTANCE, help=argparse.SUPPRESS)
    parser.add_argument("--releases", type=Path, default=DEFAULT_RELEASES, help=argparse.SUPPRESS)
    arguments = parser.parse_args(argv)

    notes: list[str] = []
    root_files: list[str] = []
    try:
        accepted = None
        release = arguments.release
        if release is None:
            if arguments.unsigned_proof:
                raise StageError("--unsigned-proof stages the directory --release names, never a release found by itself")
            accepted = contract_checks.parse_assignments(arguments.acceptance, StageError)
            release = arguments.releases / accepted["LINEAGE24_ACCEPTED_DIRECTORY_NAME"]
        release = release.resolve()
        if not release.is_dir():
            raise StageError(f"the Android release is not installed: {release}")
        check_release_directories(release)
        manifest_bytes = read_manifest(release)
        manifest = parse_object(manifest_bytes, MANIFEST)
        is_proof = manifest.get("acceptance") == UNSIGNED_PROOF
        staged_manifest = manifest_bytes
        if arguments.unsigned_proof:
            if not is_proof:
                raise StageError(
                    "--unsigned-proof stages an unsigned proof, and this release's acceptance is "
                    f"{manifest.get('acceptance')!r}: stage an accepted release without it")
            line = tracked_line(arguments.acceptance, IMAGE_METADATA, KERNEL_DESCRIPTOR)
            manifest = parse_object(manifest_bytes, MANIFEST, strict=True)
            staged = check_proof(release, manifest, line)
            staged_manifest = (json.dumps(staged, indent=2, sort_keys=True) + "\n").encode("utf-8")
            notes = proof_notes(manifest, staged)
        elif is_proof:
            raise StageError(
                "the release is an unsigned proof image, which only a developer build stages: "
                "pass --unsigned-proof (a Release archive refuses it)")
        else:
            check_release(release, manifest, accepted)
        root_files = check_root(release, manifest)
        if notes and root_files:
            notes.append("root files: checked against the proof's own init_boot and the APK, as a release's are")
        stage(release, arguments.output, staged_manifest, root_files)
    except StageError as error:
        fail(str(error))
    with_root = " with Magisk root files" if root_files else ""
    if notes:
        print(f"staged UNSIGNED PROOF {manifest['product_family']} {manifest['release_id']}{with_root}: {arguments.output}")
        for note in notes:
            print(f"  {note}")
    else:
        print(f"staged {manifest['product_family']} {manifest['release_id']}{with_root}: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
