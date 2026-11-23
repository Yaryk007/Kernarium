#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unpack one read-only partition, or one APEX, of an accepted Android release.

The release's system.img is a GPT disk whose partitions are Zstandard-
compressed EROFS images. The partition is copied out at the offset the
release's partition-layout.json gives, checked against the digest the layout
records, and unpacked with an EROFS tool that can decode Zstandard (the
Android build's fsck.erofs; the Homebrew build cannot). An APEX is a zip
archive in the system partition whose apex_payload.img is an EROFS image; it
is unpacked from an already unpacked system partition. Reads the release only;
writes only under --out.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import zipfile


def unpack(fsck: pathlib.Path, image: pathlib.Path, root: pathlib.Path
           ) -> None:
    root.mkdir()
    subprocess.run([str(fsck), f"--extract={root}", "--overwrite",
                    str(image)], check=True, stdout=subprocess.DEVNULL)


def current(stamp: pathlib.Path, identity: dict, root: pathlib.Path) -> bool:
    if stamp.exists() and json.loads(stamp.read_text()) == identity and \
            root.is_dir():
        os.utime(stamp)  # the build system sees the output as current
        return True
    return False


def extract_apex(arguments: argparse.Namespace) -> int:
    source = arguments.system_root / "system" / "apex" / (
        arguments.apex + ".apex")
    if not source.is_file():
        print(f"no APEX {source}", file=sys.stderr)
        return 1
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    identity = {"apex": arguments.apex, "apex_sha256": digest}
    root = arguments.out / "root"
    stamp = arguments.out / "identity.json"
    if current(stamp, identity, root):
        return 0
    if arguments.out.exists():
        shutil.rmtree(arguments.out)
    arguments.out.mkdir(parents=True)
    with zipfile.ZipFile(source) as archive:
        image = arguments.out / "apex_payload.img"
        with archive.open("apex_payload.img") as payload, \
                open(image, "wb") as sink:
            shutil.copyfileobj(payload, sink)
    unpack(arguments.fsck_erofs, image, root)
    image.unlink()
    stamp.write_text(json.dumps(identity, indent=2, sort_keys=True) + "\n")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=pathlib.Path, required=True,
                        help="an accepted release directory")
    parser.add_argument("--partition", default="system")
    parser.add_argument("--apex", help="unpack this APEX of the system "
                        "partition instead of a partition")
    parser.add_argument("--system-root", type=pathlib.Path,
                        help="the unpacked system partition (with --apex)")
    parser.add_argument("--fsck-erofs", type=pathlib.Path, required=True,
                        help="an fsck.erofs that decodes Zstandard")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args(argv)
    if arguments.apex:
        return extract_apex(arguments)
    guest = arguments.release / "guest"
    layout = json.loads((guest / "partition-layout.json").read_text())
    matches = [p for p in layout["system_disk"]["partitions"]
               if p["name"] == arguments.partition]
    if len(matches) != 1:
        print(f"no partition named {arguments.partition}", file=sys.stderr)
        return 1
    partition = matches[0]
    manifest = json.loads((arguments.release / "release-manifest.json"
                           ).read_text())
    identity = {
        "release_id": manifest["release_id"],
        "partition": arguments.partition,
        "partition_sha256": partition["sha256"],
    }
    root = arguments.out / "root"
    stamp = arguments.out / "identity.json"
    if current(stamp, identity, root):
        return 0
    if arguments.out.exists():
        shutil.rmtree(arguments.out)
    arguments.out.mkdir(parents=True)
    image = arguments.out / "partition.erofs"
    digest = hashlib.sha256()
    with open(guest / "system.img", "rb") as source, open(image, "wb") as sink:
        source.seek(partition["start_bytes"])
        remaining = partition["image_bytes"]
        while remaining:
            chunk = source.read(min(remaining, 8 << 20))
            if not chunk:
                print("short read of system.img", file=sys.stderr)
                return 1
            sink.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
    if digest.hexdigest() != partition["sha256"]:
        print(f"the copied {arguments.partition} partition does not match "
              "the digest the release records", file=sys.stderr)
        return 1
    unpack(arguments.fsck_erofs, image, root)
    image.unlink()
    stamp.write_text(json.dumps(identity, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
