# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic Magisk inputs for the root-file tests.

A synthetic stock init_boot in a synthetic system disk and layout, a synthetic APK
and pin, and stand-in tools: a magiskboot (this file with --fake-magiskboot, run
through a wrapper the APK carries) and a boot_patch.sh that calls it in Magisk's
order. The test's own encoders (newc, lz4 legacy, the patch's entries) are
independent of the code under test. No Magisk file is in the repository.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import json
import lzma
import os
import stat
import struct
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "android/lineage-24/tools"
THIS = Path(__file__).resolve()
TEST_VERSION = "v99.1"
TEST_VERSION_CODE = 99100
magisk_image = None


def configure(source_root: Path) -> None:
    """Point the fixtures at a source tree and load its tools."""
    global ROOT, TOOLS, magisk_image
    ROOT = source_root.resolve()
    TOOLS = ROOT / "android/lineage-24/tools"
    for path in (ROOT / "kernel/common/artifacts", TOOLS):
        if str(path) not in sys.path:
            sys.path.insert(0, str(path))
    import magisk_image as module  # noqa: PLC0415

    magisk_image = module


def pad4(length: int) -> bytes:
    return b"\0" * (-length % 4)


def newc_dump(entries: dict, *, order: list[str] | None = None, mtime: int = 0, tail: int = 0,
              inodes: dict[str, int] | None = None, links: dict[str, int] | None = None,
              magics: dict[str, bytes] | None = None) -> bytes:
    """A newc archive of {name: Entry}, in Magisk's way (sorted, inodes from 300001,
    link count 1) unless `order` names the order. `inodes`, `links` and `magics` set
    the inode number, the link count and the header magic of the entries they name."""
    out = bytearray()
    names = order if order is not None else sorted(entries)
    for number, name in enumerate(names, 300001):
        entry = entries[name]
        raw = name.encode() + b"\0"
        fields = ((inodes or {}).get(name, number), entry.mode, entry.uid, entry.gid,
                  (links or {}).get(name, 1), mtime, len(entry.payload),
                  0, 0, entry.rdev[0], entry.rdev[1], len(raw), 0)
        out += (magics or {}).get(name, b"070701") + b"".join(b"%08X" % value for value in fields)
        out += raw + pad4(110 + len(raw)) + entry.payload + pad4(len(entry.payload))
    raw = b"TRAILER!!!\0"
    out += b"070701" + b"".join(b"%08X" % value for value in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(raw), 0))
    out += raw + pad4(110 + len(raw))
    return bytes(out) + b"\0" * tail


def lz4_literals(data: bytes) -> bytes:
    """An LZ4 block of literals only."""
    count = len(data)
    out = bytearray([min(count, 15) << 4])
    if count >= 15:
        remaining = count - 15
        while remaining >= 255:
            out.append(255)
            remaining -= 255
        out.append(remaining)
    return bytes(out) + data


def lz4_matches(data: bytes) -> bytes:
    """An LZ4 block with matches, by a naive greedy search: for small inputs, to
    exercise the decoder's overlapping copies and extended lengths. It keeps the
    format's end rules, which the kernel's decoder enforces: the last match starts
    at least 12 bytes before the end of the data and the last 5 bytes are literals."""
    out = bytearray()

    def extension(count: int) -> None:
        count -= 15
        while count >= 255:
            out.append(255)
            count -= 255
        out.append(count)

    def emit(literals: bytes, offset: int, length: int) -> None:
        match = length - 4 if length else 0
        out.append(min(len(literals), 15) << 4 | min(match, 15))
        if len(literals) >= 15:
            extension(len(literals))
        out.extend(literals)
        if length:
            out.extend(struct.pack("<H", offset))
            if match >= 15:
                extension(match)

    position = 0
    literal_start = 0
    while position + 12 <= len(data):
        best = (0, 0)
        for start in range(max(0, position - 65535), position):
            length = 0
            while position + length < len(data) - 5 and data[start + length] == data[position + length]:
                length += 1
            if length > best[0]:
                best = (length, position - start)
        if best[0] >= 4:
            emit(data[literal_start:position], best[1], best[0])
            position += best[0]
            literal_start = position
        else:
            position += 1
    emit(data[literal_start:], 0, 0)
    return bytes(out)


def lz4_legacy(data: bytes) -> bytes:
    out = bytearray(b"\x02\x21\x4c\x18")
    for start in range(0, len(data), 8 * 1024 * 1024):
        block = lz4_literals(data[start:start + 8 * 1024 * 1024])
        out += struct.pack("<I", len(block)) + block
    return bytes(out)


def boot_image(ramdisk: bytes, size: int = 0, os_version: int = 0x1234) -> bytes:
    header = bytearray(4096)
    header[0:8] = b"ANDROID!"
    struct.pack_into("<IIII", header, 8, 0, len(ramdisk), os_version, 1584)
    struct.pack_into("<I", header, 40, 4)
    image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
    return image + b"\0" * max(0, size - len(image))


def make_apk_bytes(path: Path, members: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, payload in members.items():
            archive.writestr(name, payload)


def pseudo_random(label: str, size: int) -> bytes:
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(f"{label}{counter}".encode()).digest()
        counter += 1
    return bytes(out[:size])


# ------------------------------------------------------ the stand-in magiskboot --


def fake_magiskboot(argv: list[str]) -> int:
    """The few magiskboot commands boot_patch.sh and magisk-boot.py use, on the
    synthetic images. The first argument is the tools directory; options before the
    command: --nondeterministic (the cpio carries a random time) and --raw-ramdisk
    (repack does not compress)."""
    sys.path.insert(0, argv.pop(0))
    import magisk_image  # noqa: PLC0415

    options: list[str] = []
    while argv and argv[0].startswith("--"):
        options.append(argv.pop(0))
    command, *rest = argv

    def error(text: str) -> SystemExit:
        return SystemExit(text)

    def load_cpio(path: str) -> dict:
        return magisk_image.parse_newc(Path(path).read_bytes(), error)

    if command == "unpack":
        _, ramdisk = magisk_image.boot_ramdisk(Path(rest[0]).read_bytes(), "image", error)
        Path("ramdisk.cpio").write_bytes(ramdisk)
        print("RAMDISK_FMT      [raw]")
        return 0
    if command == "sha1":
        print(hashlib.sha1(Path(rest[0]).read_bytes()).hexdigest())
        return 0
    if command == "compress=xz":
        Path(rest[1]).write_bytes(lzma.compress(Path(rest[0]).read_bytes(), format=lzma.FORMAT_XZ))
        return 0
    if command == "cpio":
        path = rest[0]
        entries = load_cpio(path)
        for text in rest[1:]:
            words = text.split(" ")
            if words[0] == "add":
                entries[words[2]] = magisk_image.Entry(
                    stat.S_IFREG | int(words[1], 8), 0, 0, (0, 0), Path(words[3]).read_bytes())
            elif words[0] == "mkdir":
                entries[words[2]] = magisk_image.Entry(stat.S_IFDIR | int(words[1], 8), 0, 0, (0, 0), b"")
            elif words[0] == "patch":
                pass
            elif words[0] == "backup":
                original = load_cpio(words[1])
                removals = sorted(name for name in entries if name not in original)
                for name, entry in original.items():
                    if name in entries and entries[name] != entry and stat.S_ISREG(entry.mode):
                        entries[f".backup/{name}.xz"] = magisk_image.Entry(
                            entry.mode, 0, 0, (0, 0), lzma.compress(entry.payload, format=lzma.FORMAT_XZ))
                entries[".backup/.rmlist"] = magisk_image.Entry(
                    stat.S_IFREG, 0, 0, (0, 0), b"".join(name.encode() + b"\0" for name in removals))
            else:
                print(f"! unknown cpio command {text}", file=sys.stderr)
                return 1
        mtime = int.from_bytes(os.urandom(3), "big") if "--nondeterministic" in options else 0
        Path(path).write_bytes(newc_dump(entries, mtime=mtime))
        return 0
    if command == "repack":
        original = Path(rest[0]).read_bytes()
        out = rest[1] if len(rest) > 1 else "new-boot.img"
        cpio = Path("ramdisk.cpio").read_bytes()
        ramdisk = cpio if "--raw-ramdisk" in options else lz4_legacy(cpio)
        header = bytearray(original[:4096])
        struct.pack_into("<I", header, 12, len(ramdisk))
        image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
        Path(out).write_bytes(image + b"\0" * max(0, len(original) - len(image)))
        return 0
    print(f"! unknown command {command}", file=sys.stderr)
    return 1


# The script follows Magisk's boot_patch.sh in its commands and their order. The
# real one asks the guest's magisk for the pre-init partition, which cannot run on
# the builder: this one leaves PREINITDEVICE out unless told to write it.
PATCH_SCRIPT = r"""#!/system/bin/sh
cd "$(dirname "$0")"
BOOTIMAGE="$1"
./magiskboot unpack "$BOOTIMAGE" || exit 1
cp -af ramdisk.cpio ramdisk.cpio.orig
SHA1=$(./magiskboot sha1 "$BOOTIMAGE")
echo "KEEPVERITY=$KEEPVERITY" > config
echo "KEEPFORCEENCRYPT=$KEEPFORCEENCRYPT" >> config
echo "RECOVERYMODE=$RECOVERYMODE" >> config
echo "VENDORBOOT=${VENDORBOOT:-false}" >> config
@PREINIT@
echo "SHA1=$SHA1" >> config
./magiskboot compress=xz magisk magisk.xz
./magiskboot compress=xz init-ld init-ld.xz
./magiskboot cpio ramdisk.cpio \
@ADD_INIT@
"mkdir 0750 overlay.d" \
"mkdir 0750 overlay.d/sbin" \
"add 0644 overlay.d/sbin/magisk.xz magisk.xz" \
@ADD_INIT_LD@
"patch" \
"backup ramdisk.cpio.orig" \
"mkdir 000 .backup" \
"add 000 .backup/.magisk config"
./magiskboot repack "$BOOTIMAGE"
@EXIT@
"""


def patch_script(*, preinit: bool = False, add_init: bool = True, add_init_ld: bool = True,
                 exit_status: int = 0, hang: bool = False) -> bytes:
    text = PATCH_SCRIPT
    text = text.replace("@PREINIT@", 'echo "PREINITDEVICE=$PREINITDEVICE" >> config' if preinit else ":")
    text = text.replace("@ADD_INIT@", '"add 0750 init magiskinit" \\' if add_init else '"patch" \\')
    text = text.replace(
        "@ADD_INIT_LD@",
        '"add 0644 overlay.d/sbin/init-ld.xz init-ld.xz" \\' if add_init_ld else '"patch" \\')
    text = text.replace("@EXIT@", ("sleep 5\n" if hang else "") + f"exit {exit_status}")
    return text.encode()


APK_NATIVE = {
    "lib/arm64-v8a/libmagiskinit.so": pseudo_random("init", 3000),
    "lib/arm64-v8a/libmagisk.so": pseudo_random("magisk", 9000),
    "lib/arm64-v8a/libinit-ld.so": pseudo_random("init-ld", 500),
}


def apk_members(script: bytes, *, options: tuple[str, ...] = (), omit: tuple[str, ...] = ()) -> dict[str, bytes]:
    wrapper = (
        f'#!/bin/sh\nexec "{sys.executable}" -B "{THIS}" --fake-magiskboot "{TOOLS}" '
        f'{" ".join(options)} "$@"\n'
    ).encode()
    members = {
        "assets/boot_patch.sh": script,
        "assets/util_functions.sh": b"# stand-in\n",
        "lib/x86_64/libmagiskboot.so": wrapper,
        "lib/x86_64/libmagisk.so": b"builder-only magisk",
        "AndroidManifest.xml": b"stand-in manifest",
        **APK_NATIVE,
    }
    for name in omit:
        del members[name]
    return members


def stock_entries() -> dict:
    Entry = magisk_image.Entry
    return {
        "init": Entry(stat.S_IFREG | 0o750, 0, 0, (0, 0), b"stock-init-binary" * 40),
        "dev": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "dev/null": Entry(stat.S_IFCHR | 0o666, 0, 0, (1, 3), b""),
        "system": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "system/bin": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "system/bin/toolbox": Entry(stat.S_IFREG | 0o755, 0, 2000, (0, 0), b"toolbox"),
        "sbin": Entry(stat.S_IFLNK | 0o777, 0, 0, (0, 0), b"/system/bin"),
        "first_stage_ramdisk": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "first_stage_ramdisk/system": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "first_stage_ramdisk/system/etc": Entry(stat.S_IFDIR | 0o755, 0, 0, (0, 0), b""),
        "first_stage_ramdisk/system/etc/fstab.uml": Entry(
            stat.S_IFREG | 0o644, 0, 0, (0, 0), b"fstab\n"),
    }


def reference_patched(stock: dict, preinit: str, stock_sha1: str) -> dict:
    """What Magisk's patch makes of the stock entries, by the test's own account."""
    Entry = magisk_image.Entry
    entries = dict(stock)
    entries["init"] = Entry(
        stat.S_IFREG | 0o750, 0, 0, (0, 0), APK_NATIVE["lib/arm64-v8a/libmagiskinit.so"])
    entries[".backup"] = Entry(stat.S_IFDIR, 0, 0, (0, 0), b"")
    entries["overlay.d"] = Entry(stat.S_IFDIR | 0o750, 0, 0, (0, 0), b"")
    entries["overlay.d/sbin"] = Entry(stat.S_IFDIR | 0o750, 0, 0, (0, 0), b"")
    for name, member in (("magisk.xz", "libmagisk.so"), ("init-ld.xz", "libinit-ld.so")):
        entries[f"overlay.d/sbin/{name}"] = Entry(
            stat.S_IFREG | 0o644, 0, 0, (0, 0),
            lzma.compress(APK_NATIVE[f"lib/arm64-v8a/{member}"], format=lzma.FORMAT_XZ))
    entries[".backup/init.xz"] = Entry(
        stock["init"].mode, 0, 0, (0, 0), lzma.compress(stock["init"].payload, format=lzma.FORMAT_XZ))
    entries[".backup/.rmlist"] = Entry(stat.S_IFREG, 0, 0, (0, 0), b"".join(
        name.encode() + b"\0" for name in (
            "overlay.d", "overlay.d/sbin", "overlay.d/sbin/init-ld.xz", "overlay.d/sbin/magisk.xz")))
    entries[".backup/.magisk"] = Entry(
        stat.S_IFREG, 0, 0, (0, 0), magisk_image.backup_config(preinit, stock_sha1))
    return entries


class Fixture:
    """A synthetic stock init_boot in a disk and layout, and a synthetic APK and pin."""

    def __init__(self, directory: Path, *, userdata: bool = True) -> None:
        self.directory = directory
        self.stock = stock_entries()
        raw = newc_dump(self.stock, order=list(self.stock), tail=196)
        self.stock_image = boot_image(raw, size=65536)
        self.preinit = "ubdb3"
        self.disk = directory / "system.img"
        self.layout = directory / "partition-layout.json"
        start = 8192
        recovery = 4096
        disk = bytes(start) + self.stock_image + bytes(recovery)
        self.disk.write_bytes(disk)
        names = ["misc", "metadata"] + (["userdata"] if userdata else [])
        self.layout.write_text(json.dumps({
            "schema_version": 2,
            "sector_size": 512,
            "system_disk": {
                "size_bytes": len(disk),
                "partitions": [
                    {"name": "init_boot", "start_bytes": start, "size_bytes": len(self.stock_image)},
                    {"name": "recovery", "start_bytes": start + len(self.stock_image), "size_bytes": recovery},
                ],
            },
            "data_disk": {"partitions": [{"name": name} for name in names]},
        }))
        self.stock_sha1 = hashlib.sha1(self.stock_image).hexdigest()

    def apk(self, name: str = "Magisk.apk", **options) -> Path:
        path = self.directory / name
        make_apk_bytes(path, apk_members(**options))
        return path

    def pin(self, apk: Path, name: str = "magisk.env") -> Path:
        path = self.directory / name
        path.write_text(
            "# SPDX-License-Identifier: GPL-2.0-or-later\n"
            "LINEAGE24_MAGISK_SCHEMA=1\n"
            "LINEAGE24_MAGISK_NAME='Magisk'\n"
            f"LINEAGE24_MAGISK_VERSION='{TEST_VERSION}'\n"
            f"LINEAGE24_MAGISK_VERSION_CODE={TEST_VERSION_CODE}\n"
            f"LINEAGE24_MAGISK_APK_SHA256='{hashlib.sha256(apk.read_bytes()).hexdigest()}'\n"
            f"LINEAGE24_MAGISK_APK_SIZE={apk.stat().st_size}\n")
        return path

    def patched_reference(self) -> bytes:
        entries = reference_patched(self.stock, self.preinit, self.stock_sha1)
        header = bytearray(self.stock_image[:4096])
        ramdisk = lz4_legacy(newc_dump(entries))
        struct.pack_into("<I", header, 12, len(ramdisk))
        image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
        return image + b"\0" * max(0, len(self.stock_image) - len(image))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-magiskboot":
        raise SystemExit(fake_magiskboot(sys.argv[2:]))
    raise SystemExit("magisk_fixture.py is a helper of the root-file tests")
