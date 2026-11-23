#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the artifacts an application embeds carry the commit being built.

An accepted artifact is the only way a change to its inputs reaches users, so a
release build that embeds one older than the inputs on its commit ships
without that change (AGENTS.md, "Release builds"). The check compares the
checkout it runs in, a Git work tree, with the accepted descriptors and with
what is staged and packaged; it reads no kernel checkout and runs no build.

Android release (--android-release):
  - the staged system (.build/andarium-system) and, with --app, the packaged
    one are the release android/lineage-24/metadata/accepted-release.env
    selects. A manifest that is not marked accepted fails, an unsigned proof
    image (Andarium/scripts/stage-system-image.py --unsigned-proof, for
    developer builds) by name, whatever release ID it carries. The staged and
    the packaged system disk are tied to the accepted release as installed
    below .build/android/releases (install-release.py), whose own layout names
    the digest: a manifest that says accepted over another disk fails. The
    manifests and layouts are read refusing a repeated JSON key;
  - the Magisk root files (a root/ directory and the manifest's root record)
    are the accepted release's: a staged or packaged system with a record the
    installed release lacks or differs from, without the record the installed
    release has, or with a root/ file that is not the recorded one or that no
    record declares, fails;
  - the Git object ID of every input path of the release
    (android/lineage-24/support/release/input-paths.tsv) equals the one the
    descriptor records. A changed input fails, naming the path and the commits
    since the release was built; a descriptor that records no IDs fails as
    unverifiable.

Kernel bundle (--kernel android|arm64|x86_64):
  - the tracked final tree of the line (UML_KERNEL_FINAL_TREE) equals the one
    the accepted descriptor records, which is every patch of the series;
  - with --app, the kernel in the packaged runtime framework is the kernel the
    descriptor names (the release in its Linux banner), its banner names the
    neutral build user and host kernarium@kernarium, and its embedded
    configuration (CONFIG_IKCONFIG) holds every symbol the tracked fragments
    request, as the build's own check does. The bundle manifest records no
    configuration identity, so a symbol removed from a fragment, which falls
    back to its default, is the one change this cannot see.

AOT artifact (--aot):
  - without an accepted descriptor (android/lineage-24/metadata/accepted-aot.env)
    an application that packages UmlArm64Aot.framework fails. The descriptor's
    archive field (LINEAGE24_ACCEPTED_AOT_ARCHIVES) is the one switch that lets
    an archive of Andarium's Release configuration carry the component: a framework in an
    archive while it is "no" fails, and so does an archive without one while
    it is "yes";
  - a packaged runtime framework whose load commands name UmlArm64Aot.framework
    fails an application that carries no such framework, whatever the switch
    says: dyld refuses to launch an application that lacks a framework its
    runtime links (the descriptor and the switch decide the build, this decides
    the product);
  - the packaged framework's code section is the blob the descriptor names
    (SHA-256), and the descriptor is for the accepted Android release;
  - that blob names the packaged system: the release ID, the manifest digest
    and the system partition digest in its header are the packaged system's own
    (release-manifest.json and guest/partition-layout.json), the values the
    runtime compares at launch, so an archive never carries a blob the
    application would not run;
  - the artifact installed below .build/android/aot is the one the descriptor
    names, and every file its blob is a function of (tools/aot, the decode
    partitions, the library list) equals what it was built with: a changed
    one is pending, as for the other artifacts.

The inputs, the descriptors and the files the checks read must be clean in the
work tree: an uncommitted change would be archived without a commit that
records it.

A stale or unverifiable artifact (the two findings below "pending") stops the
build unless the user has decided to ship it as it is: PENDING_INPUTS_DECISION,
a build setting or an environment value, names the release IDs (space or comma
separated) of the artifacts whose pending changes that decision covers. A
decision that names another release ID, or the ID of a release that is current,
covers nothing, so it cannot carry over to a later release. A dirty input, a
staged or packaged artifact that is not the accepted one and a kernel that is
not the descriptor's are never covered.

Andarium's Release configuration runs it as a build phase when the action is an
archive; any other build is not checked.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import hashlib
import os
import re
import stat
import struct
import sys
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parents[1]
for _tools in ("kernel/common/artifacts", "android/lineage-24/tools", "tools/aot"):
    sys.path.insert(0, str(HERE / _tools))
import contract_checks  # noqa: E402
import magisk_image  # noqa: E402
import release_artifact  # noqa: E402
import release_inputs  # noqa: E402

DECISION_ENV = "PENDING_INPUTS_DECISION"
ANDROID_DESCRIPTOR = "android/lineage-24/metadata/accepted-release.env"
INPUT_TREES_KEY = "LINEAGE24_ACCEPTED_INPUT_TREES"
ROOT_RECORD_KEY = "LINEAGE24_ACCEPTED_ROOT_RECORD_SHA256"
UNSIGNED_PROOF = "unsigned-proof"
STAGED_SYSTEM = ".build/andarium-system"
# Where install-release.py installs a release, below the work tree.
INSTALLED_RELEASES = ".build/android/releases"
# The AOT artifact beside it, and where the framework holds its blob.
INSTALLED_AOT = ".build/android/aot"
AOT_NAME = "UmlArm64Aot"
PACKAGED_AOT = f"Frameworks/{AOT_NAME}.framework/{AOT_NAME}"
AOT_SECTION = ("__TEXT", "__text")
PACKAGED_SYSTEM = "andarium-system"
MANIFEST = "release-manifest.json"
LAYOUT = "guest/partition-layout.json"
SYSTEM_DISK = "guest/system.img"
ROOT_DIRECTORY = "root"
ELF_SECTION = ("__TEXT", "__uml_elf")
# The build user and host build-kernel.sh gives every kernel; any other pair in
# a banner is the builder's own account and machine.
NEUTRAL_BUILDER = "kernarium@kernarium"
HISTORY_LIMIT = 400
SHOWN_COMMITS = 10

MAINLINE = "kernel/mainline-6.18"
SHARED_CONFIG = "kernel/common/config/darwin-uml.fragment"
KERNELS = {
    "android": {
        "descriptor": f"{MAINLINE}/artifacts/accepted-android-4k.env",
        "metadata": f"{MAINLINE}/patches/metadata.env",
        "series": ("kernel/common/patches", f"{MAINLINE}/patches/series"),
        "fragments": (SHARED_CONFIG, f"{MAINLINE}/config/android.fragment"),
        "required": f"{MAINLINE}/config/android-required-builtins.txt",
        "forbidden": f"{MAINLINE}/config/android-forbidden-options.txt",
        "framework": "KRuntime_Android_6_18_4K",
    },
    "arm64": {
        "descriptor": f"{MAINLINE}/artifacts/accepted-4k.env",
        "metadata": f"{MAINLINE}/patches/metadata.env",
        "series": ("kernel/common/patches", f"{MAINLINE}/patches/series"),
        "fragments": (SHARED_CONFIG, f"{MAINLINE}/config/mainline.fragment"),
        "framework": "KRuntime_6_18_4K",
    },
    "x86_64": {
        "descriptor": f"{MAINLINE}/slices/x86_64/artifacts/accepted-4k.env",
        "metadata": f"{MAINLINE}/slices/x86_64/patches/metadata.env",
        "series": (
            "kernel/common/patches",
            f"{MAINLINE}/patches/series",
            f"{MAINLINE}/slices/x86_64/patches/series",
        ),
        "fragments": (
            SHARED_CONFIG,
            f"{MAINLINE}/config/mainline.fragment",
            f"{MAINLINE}/slices/x86_64/config/userspace.fragment",
        ),
    },
}


class Report:
    """Findings of one run: errors that stand, and pending changes per artifact."""

    def __init__(self, decisions):
        self.decisions = decisions
        self.errors = []
        self.pending = {}
        self.notes = []

    def error(self, title, *details):
        self.errors.append((title, details))

    def pend(self, artifact_id, title, *details):
        self.pending.setdefault(artifact_id, []).append((title, details))

    def note(self, line):
        self.notes.append(line)

    def finish(self):
        warnings = []
        covered = set()
        for artifact_id, findings in sorted(self.pending.items()):
            if artifact_id in self.decisions:
                covered.add(artifact_id)
                for title, details in findings:
                    warnings.append((f"{title}; the archive will not carry it", details))
                continue
            for title, details in findings:
                self.errors.append((
                    title,
                    details + (
                        "build and accept a new artifact first; shipping this one "
                        "as it is waits for the user's decision "
                        f"({DECISION_ENV}={artifact_id})",
                    ),
                ))
        for line in self.notes:
            print(f"check-embedded-artifacts: {line}")
        for title, details in warnings:
            emit("warning", title, details)
        for unused in sorted(self.decisions - covered):
            emit("warning", f"{DECISION_ENV} names {unused}, which has no pending change; "
                 "it covers nothing", ())
        for title, details in self.errors:
            emit("error", title, details)
        return 1 if self.errors else 0


def emit(level, title, details):
    print(f"{level}: {title}", file=sys.stderr)
    for line in details:
        print(f"    {line}", file=sys.stderr)


def short(object_id):
    return object_id[:10] if object_id else "absent"


def git_text(repo, *arguments, stdin=None):
    return release_inputs.git(repo, *arguments, stdin=stdin).decode("utf-8", "replace")


def object_ids_at(repo, paths, revision="HEAD"):
    """Object ID of each path at a revision, None where the revision has none."""
    if not paths:
        return {}
    request = "".join(f"{revision}:{path}\n" for path in paths).encode("utf-8")
    answer = git_text(repo, "cat-file", "--batch-check", stdin=request).splitlines()
    if len(answer) != len(paths):
        raise release_inputs.InputError("git cat-file answered a different number of paths")
    result = {}
    for path, line in zip(paths, answer):
        fields = line.split(" ")
        found = len(fields) == 3 and release_inputs.OBJECT_ID_RE.fullmatch(fields[0])
        result[path] = fields[0] if found else None
    return result


def dirty_paths(repo, paths):
    """Paths below the given ones with a change the work tree has not committed."""
    listing = release_inputs.git(
        repo, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--", *paths
    ).decode("utf-8", "replace")
    fields = listing.split("\0")
    found = set()
    index = 0
    while index < len(fields):
        entry = fields[index]
        index += 1
        if len(entry) < 4:
            continue
        found.add(entry[3:])
        if entry[0] in "RC":
            index += 1  # the rename's origin
    return sorted(found)


def commits_touching(repo, path, newest_first_limit=HISTORY_LIMIT):
    """(full hash, short hash, subject) of the commits that changed a path, newest first."""
    out = git_text(repo, "log", f"-n{newest_first_limit}", "--format=%H\t%h\t%s", "HEAD", "--", path)
    return [tuple(line.split("\t", 2)) for line in out.splitlines() if line.count("\t") == 2]


def commits_since_object(repo, path, recorded):
    """Commits that changed a path after it held the recorded object, or None if
    no commit in the history held it."""
    commits = commits_touching(repo, path)
    ids = []
    if commits:
        request = "".join(f"{full}:{path}\n" for full, _, _ in commits).encode("utf-8")
        for line in git_text(repo, "cat-file", "--batch-check", stdin=request).splitlines():
            fields = line.split(" ")
            ids.append(fields[0] if len(fields) == 3 else None)
    for index, object_id in enumerate(ids):
        if object_id == recorded:
            return commits[:index]
    return None


def commit_lines(commits):
    lines = [f"{short_hash} {subject}" for _, short_hash, subject in commits[:SHOWN_COMMITS]]
    if len(commits) > SHOWN_COMMITS:
        lines.append(f"... and {len(commits) - SHOWN_COMMITS} more")
    return lines


def read_json(path):
    return contract_checks.load_json_object(path, ValueError)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def descriptor_of(repo, relative):
    return contract_checks.parse_assignments(repo / relative)


# ---------------------------------------------------------------- Android --


def is_link_or_file(path):
    """The path exists and is not a real directory: a link (whatever it points to)
    or a file. A missing path is not."""
    try:
        return not stat.S_ISDIR(os.lstat(path).st_mode)
    except OSError:
        return False


def real_directories(report, base, label, relatives):
    """Each of `relatives` below `base` ("" is `base` itself) is a real directory,
    checked before any is read: a link stays behind when a tree is moved, whatever it
    holds now. A directory that is missing is left to the check that reads it."""
    for relative in relatives:
        path = base / relative
        if is_link_or_file(path):
            report.error(
                f"{label} {f'has a {relative}/ that is' if relative else 'is'} not a real directory",
                f"{path} is a link or a file: stage the accepted release with "
                "Andarium/scripts/stage-system-image.py and package that",
            )
            return False
    return True


def real_system_directories(report, system, label):
    """The system directory and the two directories the checks read below it
    (guest/ and boot/) are real directories."""
    return real_directories(report, system, label, ("", "guest", "boot"))


def check_staged_system(report, release_id, descriptor, system, label):
    """The staged or packaged system is the accepted release; returns its manifest."""
    if not real_system_directories(report, system, label):
        return None
    manifest_path = system / MANIFEST
    if not manifest_path.is_file():
        report.error(
            f"{label} holds no {MANIFEST}",
            f"{system}: stage the accepted release with Andarium/scripts/stage-system-image.py",
        )
        return None
    try:
        manifest = read_json(manifest_path)
    except ValueError as error:
        report.error(f"{label} has an unreadable manifest", str(error))
        return None
    acceptance = manifest.get("acceptance")
    if acceptance != "accepted":
        # An accepted release's manifest says so; the proof image of a
        # developer build says it is a proof. Neither the release ID nor any
        # other field a proof shares with the release can stand in for it.
        what = (
            "an unsigned proof image, which only a developer build embeds"
            if acceptance == UNSIGNED_PROOF
            else f"not an accepted release (acceptance {acceptance!r})"
        )
        report.error(
            f"{label} is {what}",
            f"{system}: stage the accepted release with Andarium/scripts/stage-system-image.py, "
            "without --unsigned-proof",
        )
        return manifest
    if manifest.get("release_id") != release_id:
        report.error(
            f"{label} is release {manifest.get('release_id')}, not the accepted {release_id}",
            f"{system}: stage the accepted release with Andarium/scripts/stage-system-image.py",
        )
        return manifest
    recorded = descriptor.get(INPUT_TREES_KEY)
    outer = manifest.get("outer")
    trees = outer.get("input_trees") if isinstance(outer, dict) else None
    if recorded is not None and (
        not isinstance(trees, dict) or release_inputs.render_trees(trees) != recorded
    ):
        report.error(
            f"{label} records input trees that differ from the accepted descriptor's",
            f"{manifest_path} is not the release {ANDROID_DESCRIPTOR} accepted",
        )
    return manifest


def check_packaged_disk(report, staged, packaged, release_id, expected):
    """The packaged manifest, layout and command line are the staged ones, and
    the packaged system disk is the accepted release's (`expected`, its digest)."""
    for relative in (MANIFEST, LAYOUT, "boot/kernel-command-line.txt"):
        theirs = packaged / relative
        ours = staged / relative
        if not theirs.is_file() or not ours.is_file() or theirs.read_bytes() != ours.read_bytes():
            report.error(
                f"the packaged {relative} is not the staged one of release {release_id}",
                f"{theirs} differs from {ours}; package from the staged system in the same run",
            )
            return
    if expected is None:
        return
    try:
        recorded = read_json(packaged / LAYOUT)["system_disk"]["sha256"]
    except (ValueError, KeyError, TypeError) as error:
        report.error("the packaged partition layout names no system disk digest", str(error))
        return
    disk = packaged / SYSTEM_DISK
    if recorded != expected or not disk.is_file() or sha256_file(disk) != expected:
        report.error(
            f"the packaged system disk is not the one of release {release_id}",
            f"{disk} does not match the digest of the installed accepted release, which "
            f"{LAYOUT} must name",
        )


def installed_release(repo, report, descriptor, release_id):
    """The system disk digest and the Magisk root record (None for a release
    without root files) of the accepted release as installed below the work
    tree, which the accepted descriptor selects by its directory name, or None
    after reporting why there is none. The staged and the packaged disk and root
    files are tied to it, not to a digest their own layout or manifest names."""
    name = descriptor.get("LINEAGE24_ACCEPTED_DIRECTORY_NAME")
    if not name or "/" in name or name in (".", ".."):
        report.error("the accepted descriptor names no release directory", ANDROID_DESCRIPTOR)
        return None
    installed = repo / INSTALLED_RELEASES / name
    if not real_directories(report, installed, "the installed accepted release", ("guest",)):
        return None
    try:
        manifest = read_json(installed / MANIFEST)
        digest = read_json(installed / LAYOUT)["system_disk"]["sha256"]
    except (ValueError, KeyError, TypeError) as error:
        report.error(
            f"the accepted Android release {release_id} is not installed here, so the system "
            "disk cannot be tied to it",
            f"{installed}: install it with android/lineage-24/tools/install-release.py ({error})",
        )
        return None
    if manifest.get("release_id") != release_id or manifest.get("acceptance") != "accepted":
        report.error(
            f"the release installed as {name} is not the accepted {release_id}",
            f"{installed / MANIFEST} names another release or is not marked accepted",
        )
        return None
    # Without a digest to tie to, both disk checks would be skipped.
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        report.error(
            f"the accepted Android release {release_id} installed as {name} names no usable "
            "system disk digest",
            f"{installed / LAYOUT}: system_disk.sha256 must be 64 lowercase hexadecimal digits",
        )
        return None
    if not root_binding_holds(report, descriptor, manifest, installed / MANIFEST):
        return None
    return digest, manifest.get("root")


def root_binding_holds(report, descriptor, manifest, installed):
    """The accepted descriptor and the installed release agree on the root files. A
    schema 6 or 7 descriptor names none, so the release has no root record; a schema
    8 descriptor names the digest of its root record, or none. Without this a
    release with root files and one without would both pass under one descriptor."""
    schema = descriptor.get("LINEAGE24_ACCEPTED_SCHEMA", "")
    bound = descriptor.get(ROOT_RECORD_KEY)
    if not schema.isdigit():
        report.error("the accepted descriptor names no schema", ANDROID_DESCRIPTOR)
        return False
    if int(schema) < magisk_image.ROOT_SCHEMA:
        if bound is None and "root" not in manifest:
            return True
        report.error(
            f"the accepted descriptor is schema {schema}, which admits no root files, but "
            f"{'it binds a root record' if bound is not None else 'the installed release carries one'}",
            f"{ANDROID_DESCRIPTOR} and {installed}: a release with root files is accepted by a "
            f"schema {magisk_image.ROOT_SCHEMA} descriptor",
        )
        return False
    if bound is None:
        report.error(
            f"the accepted descriptor is schema {schema} and binds no root record",
            f"{ANDROID_DESCRIPTOR} must name {ROOT_RECORD_KEY}: the digest of the reviewed root "
            "record, or none",
        )
        return False
    if bound != magisk_image.root_record_digest(manifest.get("root")):
        report.error(
            "the installed root record is not the one the accepted descriptor binds",
            f"{installed} and {ANDROID_DESCRIPTOR} ({ROOT_RECORD_KEY}) disagree: install "
            "the accepted release again",
        )
        return False
    return True


def check_root_files(report, system, manifest, expected, label):
    """The Magisk root files of a staged or packaged system are the accepted
    release's: the manifest's root record is the installed release's (`expected`,
    None when it has none), every root file hashes to it, and root/ holds
    nothing else."""
    record = manifest.get("root")
    held = system / ROOT_DIRECTORY
    if expected is None:
        if record is not None:
            report.error(
                f"{label} carries Magisk root files the accepted release does not",
                f"{system / MANIFEST} has a root record; stage the accepted release with "
                "Andarium/scripts/stage-system-image.py",
            )
        elif held.exists() or held.is_symlink():
            report.error(
                f"{label} holds a root/ its manifest records no root files for",
                f"{held} is not part of the accepted release",
            )
        return
    if record != expected:
        report.error(
            f"{label} {'lacks' if record is None else 'records root files that differ from'} "
            "the Magisk root files of the accepted release",
            f"{system / MANIFEST}: stage the accepted release with Andarium/scripts/stage-system-image.py",
        )
        return
    try:
        files = {item["path"]: item["sha256"] for item in (record["boot_image"], record["apk"])}
    except (KeyError, TypeError) as error:
        report.error(f"{label} has an unreadable root record", str(error))
        return
    try:
        mode = os.lstat(held).st_mode
    except OSError:
        mode = None
    if mode is None or stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
        report.error(
            f"{label} has a root/ that is not a real directory",
            f"{held} is missing, a link or a file; the accepted release's root files are two files in a "
            "real root/ directory",
        )
        return
    names = {PurePosixPath(relative).name for relative in files}
    for relative, digest in sorted(files.items()):
        path = system / relative
        try:
            regular = stat.S_ISREG(os.lstat(path).st_mode)
        except OSError:
            regular = False
        if not regular or sha256_file(path) != digest:
            report.error(
                f"{label} has a root file that is not the accepted release's: {relative}",
                f"{path} is missing, not a regular file, or does not hash to the record's digest",
            )
    extra = sorted(set(os.listdir(held)) - names)
    if extra:
        report.error(
            f"{label} holds root files the accepted release does not",
            *(f"{held / name}" for name in extra),
        )


def check_staged_disk(report, staged, label, expected):
    """The staged system disk is the accepted release's: its layout names the
    digest and the disk hashes to it."""
    try:
        recorded = read_json(staged / LAYOUT)["system_disk"]["sha256"]
    except (ValueError, KeyError, TypeError) as error:
        report.error(f"{label} has a partition layout that names no system disk digest", str(error))
        return
    disk = staged / SYSTEM_DISK
    if recorded != expected or not disk.is_file() or sha256_file(disk) != expected:
        report.error(
            f"the system disk of {label} is not the accepted release's",
            f"{disk} does not match the digest of the installed accepted release",
        )


NEW_INPUT = "is an input now and was not when the release was built"


def input_findings(recorded, paths, current):
    """(path, what, recorded object ID or None) for each input path that
    differs from what a release recorded. `current` maps the list's paths to
    their object IDs here, None for a path the commit lacks. A path the list
    gained since the release is NEW_INPUT: pending until a release carries it."""
    findings = []
    for path in sorted(set(recorded) | set(paths)):
        if path not in recorded:
            findings.append((path, NEW_INPUT, None))
        elif path not in current:
            findings.append((path, "is no longer an input of the list", None))
        elif current[path] is None:
            findings.append((path, f"was removed (recorded {short(recorded[path])})", None))
        elif current[path] != recorded[path]:
            findings.append((
                path,
                f"changed (recorded {short(recorded[path])}, now {short(current[path])})",
                recorded[path],
            ))
    return findings


def is_accepted(manifest, release_id):
    """A manifest that says it is the accepted release: the other cases (a proof,
    another release) are reported as what they are, not checked for their files."""
    return (
        manifest is not None
        and manifest.get("acceptance") == "accepted"
        and manifest.get("release_id") == release_id
    )


def check_android(repo, report, staged_root, app):
    try:
        descriptor = descriptor_of(repo, ANDROID_DESCRIPTOR)
        release_id = descriptor["LINEAGE24_ACCEPTED_RELEASE_ID"]
        paths = release_inputs.input_paths(repo / release_inputs.LIST_RELATIVE)
    except (ValueError, KeyError, release_inputs.InputError) as error:
        report.error("the Android release descriptor or its input list is unreadable", str(error))
        return
    label = f"the Android release {release_id}"

    installed = installed_release(repo, report, descriptor, release_id)
    expected = installed[0] if installed is not None else None
    staged = staged_root
    staged_manifest = check_staged_system(
        report, release_id, descriptor, staged, f"the staged system ({label})")
    if staged_manifest is not None and expected is not None:
        check_staged_disk(report, staged, f"the staged system ({label})", expected)
    if installed is not None and is_accepted(staged_manifest, release_id):
        check_root_files(report, staged, staged_manifest, installed[1], f"the staged system ({label})")
    if app is not None:
        packaged = app / PACKAGED_SYSTEM
        packaged_manifest = check_staged_system(
            report, release_id, descriptor, packaged, f"the packaged system ({label})")
        if (
            staged_manifest is not None and packaged_manifest is not None
            and staged_manifest.get("release_id") == release_id
            and packaged_manifest.get("release_id") == release_id
        ):
            check_packaged_disk(report, staged, packaged, release_id, expected)
        if installed is not None and is_accepted(packaged_manifest, release_id):
            check_root_files(
                report, packaged, packaged_manifest, installed[1], f"the packaged system ({label})")

    recorded_text = descriptor.get(INPUT_TREES_KEY)
    if recorded_text is None:
        schema = descriptor.get("LINEAGE24_ACCEPTED_SCHEMA")
        report.pend(
            release_id,
            f"{label} records no input trees, so it cannot be checked against this commit",
            f"its descriptor is schema {schema}; a release published with schema 7 or later records them",
        )
        return
    try:
        recorded = release_inputs.parse_trees(recorded_text)
    except release_inputs.InputError as error:
        report.error(f"the accepted descriptor's input trees are malformed: {error}")
        return
    current = object_ids_at(repo, paths)
    findings = input_findings(recorded, paths, current)
    if not findings:
        report.note(f"{label}: its {len(recorded)} input paths are the ones on this commit")
        return
    details = []
    for path, what, recorded_id in findings:
        details.append(f"{path} {what}")
        if recorded_id is None:
            continue
        since = commits_since_object(repo, path, recorded_id)
        if since is None:
            details.append("    the recorded tree is not in the last "
                           f"{HISTORY_LIMIT} changes of this path's history (a history rewrite, "
                           "or built from a checkout never committed)")
        else:
            details.extend(f"    {line}" for line in commit_lines(since))
    report.pend(
        release_id,
        f"{label} is behind this commit: {len(findings)} input path"
        f"{'s' if len(findings) != 1 else ''} differ from what it was built from",
        *details,
    )


# -------------------------------------------------------------------- AOT --


def check_blob_release(report, app, blob):
    """The packaged blob is the one the packaged system selects at launch:
    the runtime runs the component only when the release the blob names (its
    ID, manifest digest and system partition digest) is the release it boots,
    which the staged system's own files state. Anything else would archive an
    application that silently runs without it."""
    if any(is_link_or_file(app / PACKAGED_SYSTEM / relative) for relative in ("", "guest")):
        # A link to the packaged system is reported by the Android check, which
        # runs only with --android-release; nothing is read through it here.
        report.error(
            "the packaged AOT blob cannot be compared with the packaged system",
            f"{app / PACKAGED_SYSTEM} or its guest/ is a link or a file, not a real directory",
        )
        return
    try:
        _, _, named = release_artifact.parse_blob_header(blob[:release_artifact.BLOB_HEADER_BYTES])
        booted = release_artifact.release_identity(app / PACKAGED_SYSTEM)
    except release_artifact.ArtifactError as error:
        report.error("the packaged AOT blob cannot be compared with the packaged system", str(error))
        return
    differing = [key for key in release_artifact.RELEASE_KEYS if named[key] != booted[key]]
    if differing:
        report.error(
            "the packaged AOT blob was not made from the packaged system, so the application would run without it",
            f"{', '.join(differing)} differ between the blob's header and {PACKAGED_SYSTEM}",
        )
    else:
        report.note("the packaged AOT blob is made from the packaged system")


def check_aot(repo, report, app, *, downloaded_android=False):
    descriptor_path = repo / release_artifact.ACCEPTED_DESCRIPTOR
    accepted = None
    if descriptor_path.exists():
        try:
            accepted = release_artifact.read_accepted(descriptor_path)
        except release_artifact.ArtifactError as error:
            report.error("the accepted AOT descriptor is unreadable", str(error))
            return
    framework = app / PACKAGED_AOT if app is not None else None
    packaged = framework is not None and framework.exists()
    if app is not None and not packaged:
        # A runtime that links the component needs it to launch, whatever the
        # descriptor says: the build asked for it by name or by default.
        linking, unreadable = runtimes_requiring(app, AOT_NAME)
        for runtime, reason in unreadable:
            report.error(
                f"the packaged framework {runtime} cannot be read as a Mach-O file, "
                f"so what it links is unknown", reason)
        for runtime in linking:
            report.error(
                f"the packaged runtime {runtime} links {AOT_NAME}.framework, but the application carries no AOT framework",
                f"{PACKAGED_AOT} is missing, and dyld refuses to launch an application without a framework its runtime links",
            )
    if accepted is None:
        if packaged:
            report.error(
                "the application carries the AOT component, but no AOT artifact is accepted",
                f"{release_artifact.ACCEPTED_DESCRIPTOR} names none",
            )
        else:
            report.note("no AOT artifact is accepted, and the application carries none")
        return
    release_id = accepted["LINEAGE24_ACCEPTED_AOT_RELEASE_ID"]
    label = f"the AOT artifact of {release_id}"
    archives = accepted[release_artifact.ARCHIVES_KEY] == "yes"
    try:
        release = descriptor_of(repo, ANDROID_DESCRIPTOR)
        release_directory = release["LINEAGE24_ACCEPTED_DIRECTORY_NAME"]
        accepted_release = release["LINEAGE24_ACCEPTED_RELEASE_ID"]
    except (ValueError, KeyError) as error:
        report.error("the Android release descriptor is unreadable", str(error))
        return
    if accepted_release != release_id:
        report.error(f"{label} is not for the accepted Android release {accepted_release}")
        return
    if packaged and not archives:
        report.error(
            "the application carries the AOT component, which no archive may yet",
            f"{release_artifact.ACCEPTED_DESCRIPTOR} sets {release_artifact.ARCHIVES_KEY} to no",
        )
    if app is not None and archives and not packaged:
        report.error(
            f"{label} is accepted for archives, but the application carries no AOT framework",
            f"{PACKAGED_AOT} is missing",
        )
    if packaged:
        blob = macho_section(framework, AOT_SECTION)
        if blob is None:
            report.error(f"the packaged AOT framework has no {AOT_SECTION[1]} section")
        else:
            digest = hashlib.sha256(blob).hexdigest()
            recorded = accepted["LINEAGE24_ACCEPTED_AOT_BLOB_SHA256"]
            if digest != recorded:
                report.error(
                    f"the packaged AOT framework is not the blob of {label}",
                    f"it holds {digest}; the descriptor names {recorded}",
                )
            else:
                report.note(f"the packaged AOT framework is the blob of {label}")
                if not downloaded_android:
                    check_blob_release(report, app, blob)

    installed = repo / INSTALLED_AOT / release_directory
    if not installed.is_dir():
        if packaged or archives:
            report.pend(
                f"aot-{release_id}",
                f"{label} is not installed, so its tools cannot be compared with this commit",
                f"expected at {INSTALLED_AOT}/{release_directory} (install-release.py --aot-artifact)",
            )
        return
    try:
        description = release_artifact.verify(
            installed, repo=repo, accepted=descriptor_path, generator=False)
    except release_artifact.ArtifactError as error:
        report.error(f"the installed AOT artifact is not the accepted one: {error}")
        return
    changed = release_artifact.generator_changes(description, repo)
    if not changed:
        report.note(f"{label}: its generator inputs are the ones on this commit")
        return
    details = []
    for path in changed:
        details.append(path)
        for line in commit_lines(commits_touching(repo, path)):
            details.append(f"    {line}")
    report.pend(
        f"aot-{release_id}",
        f"{label} is behind this commit: {len(changed)} generator input"
        f"{'s' if len(changed) != 1 else ''} differ from what it was built with",
        *details,
    )


# ----------------------------------------------------------------- kernel --


def requested_symbols(repo, fragments):
    """symbol -> (wanted value, fragment); the last fragment naming a symbol wins."""
    wanted = {}
    for relative in fragments:
        for raw in (repo / relative).read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            match = re.fullmatch(r"(CONFIG_[A-Za-z0-9_]+)=(.*)", line)
            if match:
                wanted[match.group(1)] = (match.group(2), relative)
                continue
            match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line)
            if match:
                wanted[match.group(1)] = ("n", relative)
    return wanted


def resolved_config(text):
    built = {}
    for raw in text.splitlines():
        match = re.match(r"(CONFIG_[A-Za-z0-9_]+)=(.*)", raw)
        if match:
            built[match.group(1)] = match.group(2)
            continue
        match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", raw)
        if match:
            built[match.group(1)] = "n"
    return built


def config_mismatches(repo, kernel, config_text):
    """Lines naming each symbol a fragment requests that the configuration lacks.

    Mirrors uml_kernel_config_verify in kernel/common/tooling/config-fragments.sh,
    and adds the Android product's required and forbidden lists.
    """
    built = resolved_config(config_text)
    problems = []
    for symbol, (wanted, fragment) in sorted(requested_symbols(repo, kernel["fragments"]).items()):
        got = built.get(symbol, "n")
        if got != wanted:
            problems.append(
                f"{symbol}: {fragment} requests {wanted}, the packaged kernel has "
                f"{got if symbol in built else 'absent'}")
    for key, wanted in (("required", "y"), ("forbidden", "n")):
        if key not in kernel:
            continue
        for raw in (repo / kernel[key]).read_text(encoding="utf-8").splitlines():
            symbol = raw.strip()
            if symbol and not symbol.startswith("#") and built.get(symbol, "n") != wanted:
                problems.append(
                    f"{symbol}: {kernel[key]} requires {wanted}, the packaged kernel has "
                    f"{built.get(symbol, 'absent')}")
    return problems


class MachOError(ValueError):
    """A Mach-O file that is cut short or lists a load command that is."""


def thin_offset(data):
    if len(data) < 8:
        return None
    if struct.unpack_from(">I", data, 0)[0] == 0xCAFEBABE:
        count = min(struct.unpack_from(">I", data, 4)[0], 16)
        if len(data) < 8 + 20 * count:
            raise MachOError("the fat header is cut short")
        for index in range(count):
            cpu, _, offset, _, _ = struct.unpack_from(">IIIII", data, 8 + 20 * index)
            if cpu == 0x0100000C:
                return offset
        return None
    return 0 if struct.unpack_from("<I", data, 0)[0] == 0xFEEDFACF else None


def macho_section(path, wanted):
    """The bytes of one section of an arm64 Mach-O file, or None (a file that
    is not one, or is cut short, has no such section)."""
    data = path.read_bytes()
    try:
        base = thin_offset(data[:4096])
        if base is None:
            return None
        count = struct.unpack_from("<I", data, base + 16)[0]
        position = base + 32
        for _ in range(count):
            command, size = struct.unpack_from("<II", data, position)
            if command == 0x19:  # LC_SEGMENT_64
                sections = struct.unpack_from("<I", data, position + 64)[0]
                entry = position + 72
                for _ in range(sections):
                    name = data[entry:entry + 16].rstrip(b"\0").decode("ascii", "replace")
                    segment = data[entry + 16:entry + 32].rstrip(b"\0").decode("ascii", "replace")
                    length, offset = struct.unpack_from("<QI", data, entry + 40)
                    if (segment, name) == wanted:
                        return data[base + offset:base + offset + length]
                    entry += 80
            position += size
    except (MachOError, struct.error):
        return None
    return None


# The load commands that make dyld refuse to launch an image whose dependency
# is missing: LC_LOAD_DYLIB, LC_LOAD_UPWARD_DYLIB, LC_REEXPORT_DYLIB and
# LC_LAZY_LOAD_DYLIB. LC_LOAD_WEAK_DYLIB (0x80000018) lets the image launch.
STRONG_DYLIB_COMMANDS = frozenset({0x0C, 0x20, 0x8000001F, 0x80000023})


def macho_dylibs(path):
    """The install names an arm64 Mach-O file requires at launch, in load
    command order; an empty list for any file that is not one. Raises
    MachOError for one that is cut short: a fat header, the load commands or
    a dylib command that ends before its own header does."""
    try:
        data = path.read_bytes()
    except OSError:
        return []
    base = thin_offset(data[:4096])
    if base is None:
        return []
    if len(data) < base + 32:
        raise MachOError("the Mach-O header is cut short")
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    names = []
    for index in range(count):
        if position + 8 > len(data):
            raise MachOError(f"load command {index} starts past the end of the file")
        command, size = struct.unpack_from("<II", data, position)
        if size < 8 or position + size > len(data):
            raise MachOError(f"load command {index} is shorter than its header or runs past the end of the file")
        if command in STRONG_DYLIB_COMMANDS:
            if size < 24:
                raise MachOError(f"dylib load command {index} is shorter than its header")
            offset = struct.unpack_from("<I", data, position + 8)[0]
            if not 24 <= offset < size:
                raise MachOError(f"dylib load command {index} names its dependency outside itself")
            raw = data[position + offset:position + size]
            names.append(raw.split(b"\0", 1)[0].decode("utf-8", "replace"))
        position += size
    return names


def runtimes_requiring(app, framework_name):
    """(the embedded frameworks, by name, whose load commands require
    `<framework_name>.framework/<framework_name>`, and those that cannot be
    read as (name, reason))."""
    wanted = f"/{framework_name}.framework/{framework_name}"
    frameworks = app / "Frameworks"
    found, unreadable = [], []
    if not frameworks.is_dir():
        return found, unreadable
    for directory in sorted(frameworks.glob("*.framework")):
        if directory.stem == framework_name:
            continue
        binary = directory / directory.stem
        if not binary.is_file():
            continue
        try:
            if any(name.endswith(wanted) for name in macho_dylibs(binary)):
                found.append(directory.stem)
        except MachOError as error:
            unreadable.append((directory.stem, str(error)))
    return found, unreadable


def kernel_banner_releases(image):
    return sorted({
        match.group(1).decode("ascii")
        for match in re.finditer(rb"Linux version ([0-9][0-9A-Za-z._+~-]*) \(", image)
    })


def kernel_banner_builders(image):
    """The user@host pairs of the kernel's Linux banner and /proc/version format."""
    return sorted({
        match.group(1).decode("ascii", "replace")
        for match in re.finditer(
            rb"(?:Linux version [0-9][0-9A-Za-z._+~-]*|%s version %s) \(([^()]*)\)", image)
    })


def embedded_config(image):
    start = image.find(b"IKCFG_ST\x1f\x8b")
    end = image.find(b"IKCFG_ED", start + 8) if start >= 0 else -1
    if start < 0 or end < 0:
        return None
    try:
        return gzip.decompress(image[start + 8:end]).decode("utf-8", "replace")
    except (OSError, EOFError):
        return None


def last_commit(repo, path):
    out = git_text(repo, "log", "-n1", "--format=%H", "HEAD", "--", path).strip()
    return out or None


def check_kernel(repo, report, name, app):
    kernel = KERNELS[name]
    try:
        descriptor = descriptor_of(repo, kernel["descriptor"])
        metadata = contract_checks.parse_assignments(repo / kernel["metadata"])
        release_id = descriptor["UML_KERNEL_ARTIFACT_RELEASE_ID"]
        accepted_tree = descriptor["UML_KERNEL_ARTIFACT_FINAL_TREE"]
        accepted_release = descriptor["UML_KERNEL_ARTIFACT_RELEASE"]
        tracked_tree = metadata["UML_KERNEL_FINAL_TREE"]
    except (ValueError, KeyError) as error:
        report.error(f"the {name} kernel descriptor or its tracked metadata is unreadable", str(error))
        return
    label = f"the {name} kernel bundle {release_id}"

    if tracked_tree != accepted_tree:
        details = [
            f"{kernel['metadata']}: final tree {short(tracked_tree)} here, "
            f"{short(accepted_tree)} accepted",
        ]
        since = None
        for full, short_hash, _ in commits_touching(repo, kernel["metadata"]):
            blob = git_text(repo, "show", f"{full}:{kernel['metadata']}")
            if re.search(rf"^UML_KERNEL_FINAL_TREE='{accepted_tree}'$", blob, re.M):
                since = full
                break
        if since is None:
            details.append("the accepted tree is not in the last "
                           f"{HISTORY_LIMIT} changes of the metadata's history")
        else:
            paths = list(kernel["series"]) + [kernel["metadata"]]
            changed = git_text(repo, "diff", "--name-only", f"{since}..HEAD", "--", *paths).split()
            details.append("changed series paths: " + ", ".join(changed[:12])
                           + (f" and {len(changed) - 12} more" if len(changed) > 12 else ""))
            log = git_text(repo, "log", "--format=%H\t%h\t%s", f"{since}..HEAD", "--", *paths)
            commits = [tuple(line.split("\t", 2)) for line in log.splitlines()]
            details.extend(commit_lines(commits))
        report.pend(release_id, f"{label} is behind its patch series", *details)
    else:
        report.note(f"{label}: its final tree is the tracked one")

    if app is None or "framework" not in kernel:
        return
    framework = app / "Frameworks" / f"{kernel['framework']}.framework" / kernel["framework"]
    if not framework.is_file():
        report.error(f"the application embeds no {kernel['framework']} for {label}", str(framework))
        return
    image = macho_section(framework, ELF_SECTION)
    if image is None:
        report.error(f"{kernel['framework']} carries no {ELF_SECTION[1]} kernel image", str(framework))
        return
    releases = kernel_banner_releases(image)
    if releases != [accepted_release]:
        report.error(
            f"the kernel in {kernel['framework']} is not the one {label} accepts",
            f"packaged: {', '.join(releases) or 'no Linux banner'}; accepted: {accepted_release}; "
            "package the frameworks from the accepted bundle in the same run",
        )
        return
    # The pairs found are personal values, so the message never repeats them.
    if kernel_banner_builders(image) != [NEUTRAL_BUILDER]:
        report.error(
            f"the kernel in {kernel['framework']} does not name {NEUTRAL_BUILDER} as its build user and host",
            "build the bundle with build-kernel.sh and package the frameworks from it",
        )
        return
    config = embedded_config(image)
    if config is None:
        report.error(
            f"the kernel in {kernel['framework']} carries no configuration to check",
            "CONFIG_IKCONFIG embeds it",
        )
        return
    problems = config_mismatches(repo, kernel, config)
    if problems:
        paths = list(kernel["fragments"]) + [kernel[key] for key in ("required", "forbidden") if key in kernel]
        accepted_at = last_commit(repo, kernel["descriptor"])
        details = problems[:12]
        if len(problems) > 12:
            details.append(f"... and {len(problems) - 12} more")
        if accepted_at is not None:
            log = git_text(repo, "log", "--format=%H\t%h\t%s", f"{accepted_at}..HEAD", "--", *paths)
            commits = [tuple(line.split("\t", 2)) for line in log.splitlines()]
            if commits:
                details.append("configuration inputs changed since the descriptor was accepted:")
                details.extend(f"    {line}" for line in commit_lines(commits))
        report.pend(
            release_id,
            f"the packaged {name} kernel's configuration is behind its fragments",
            *details,
        )
    else:
        report.note(f"{label}: the packaged kernel holds every configuration symbol requested")


# ------------------------------------------------------------------- main --


def parse_decisions(text):
    return {item for item in re.split(r"[\s,]+", text or "") if item}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repository", type=Path, default=HERE,
                        help="the Git work tree being built (default: this one)")
    parser.add_argument("--app", type=Path,
                        help="the built application, whose packaged system and runtimes are checked")
    parser.add_argument("--staged", type=Path,
                        help=f"the staged system (default: <repository>/{STAGED_SYSTEM})")
    parser.add_argument("--android-release", action="store_true",
                        help="check the accepted Android release")
    parser.add_argument("--downloaded-android", action="store_true",
                        help="require the application to ship without an Android system")
    parser.add_argument("--kernel", action="append", default=[], choices=sorted(KERNELS),
                        help="check an accepted kernel bundle (repeatable)")
    parser.add_argument("--aot", action="store_true",
                        help="check the AOT artifact and, with --app, the framework that packages it")
    parser.add_argument("--decision", default=None,
                        help=f"the release IDs the user decided to ship as they are "
                             f"(default: ${DECISION_ENV})")
    options = parser.parse_args(argv)
    if not options.android_release and not options.kernel and not options.aot:
        parser.error("name an artifact: --android-release, --kernel or --aot")

    repo = options.repository.resolve()
    decisions = parse_decisions(
        options.decision if options.decision is not None else os.environ.get(DECISION_ENV))
    report = Report(decisions)
    try:
        release_inputs.git(repo, "rev-parse", "--git-dir")
    except release_inputs.InputError as error:
        report.error("this is not a Git work tree, so there is no commit to compare", str(error))
        return report.finish()

    covered = []
    if options.android_release:
        covered += [ANDROID_DESCRIPTOR, release_inputs.LIST_RELATIVE]
        try:
            covered += release_inputs.input_paths(repo / release_inputs.LIST_RELATIVE)
        except release_inputs.InputError:
            pass  # check_android reports the unreadable list
    if options.aot:
        covered += [release_artifact.ACCEPTED_DESCRIPTOR, ANDROID_DESCRIPTOR,
                    *release_artifact.GENERATOR_INPUTS]
    for name in options.kernel:
        kernel = KERNELS[name]
        covered += [kernel["descriptor"], kernel["metadata"], *kernel["series"], *kernel["fragments"]]
        covered += [kernel[key] for key in ("required", "forbidden") if key in kernel]
    try:
        dirty = dirty_paths(repo, sorted(set(covered)))
    except release_inputs.InputError as error:
        report.error("cannot read the work tree's status", str(error))
        dirty = []
    if dirty:
        report.error(
            f"{len(dirty)} path{'s' if len(dirty) != 1 else ''} the artifacts are checked against "
            "have uncommitted changes, which the archive would carry without a commit that records them",
            *dirty[:20],
        )

    if options.android_release:
        staged = options.staged if options.staged is not None else repo / STAGED_SYSTEM
        check_android(repo, report, staged, options.app)
    if options.downloaded_android and options.app is not None:
        bundled = options.app / PACKAGED_SYSTEM
        if bundled.exists() or bundled.is_symlink():
            report.error("the application bundles an Android system despite using first-launch downloads")
    for name in options.kernel:
        check_kernel(repo, report, name, options.app)
    if options.aot:
        check_aot(repo, report, options.app, downloaded_android=options.downloaded_android)
    return report.finish()


if __name__ == "__main__":
    sys.exit(main())
