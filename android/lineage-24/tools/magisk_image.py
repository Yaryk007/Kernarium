# SPDX-License-Identifier: GPL-2.0-or-later
"""The Magisk-patched init_boot of an Android release.

A release can carry, beside its system disk, a copy of its own init_boot that
Magisk's patcher made, and the official Magisk APK it was made from, as its
`root/` files. This module owns what the producer (magisk-boot.py), the release
verifier (release_common.py) and verify-magisk-boot.py share: the pin of the
Magisk release, the fixed patch inputs, the root record a manifest keeps, and the
check that a patched image is its stock image plus exactly what Magisk adds.

Magisk's patcher turns a stock ramdisk into one that starts Magisk's init in
front of the stock init: it replaces /init, keeps the stock one as
`.backup/init.xz`, adds `overlay.d/sbin/{magisk,init-ld}.xz` and a `.backup`
directory with the patch flags, the stock image's SHA-1 and the list of added
entries, and compresses the ramdisk as lz4 legacy. The check decodes both
ramdisks with the standard library and compares them entry by entry, so it runs
wherever Python does and needs none of Magisk's tools. Each caller passes its own
error type, as android_image.py's readers do.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import io
import json
import lzma
import os
import re
import stat
import struct
import sys
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

import android_image

_ARTIFACTS = str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
import contract_checks  # noqa: E402

Error = android_image.Error

ANDROID_ROOT = Path(__file__).resolve().parents[1]
PIN_PATH = ANDROID_ROOT / "metadata/magisk.env"

# The release manifest schema that carries a root record (and the accepted descriptor
# schema that binds one).
ROOT_SCHEMA = 8
ROOT_DIRECTORY = "root"
PATCHED_PATH = f"{ROOT_DIRECTORY}/init_boot-magisk.img"
PATCHED_PAYLOAD = ("magisk-init-boot", "boot-image-v4-kernelless")
APK_PAYLOAD = ("magisk-apk", "android-package")
MANIFEST = "release-manifest.json"
SYSTEM_DISK = "guest/system.img"
PARTITION_LAYOUT = "guest/partition-layout.json"

MAGISK_NAME = "Magisk"
VERSION_RE = re.compile(r"^v[0-9]+\.[0-9]+(\.[0-9]+)?$")
HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")

# The ABI of the guest, whose Magisk binaries go into the ramdisk, and of the
# builder, whose magiskboot runs the patcher. The APK carries both.
GUEST_ABI = "arm64-v8a"
BUILDER_ABI = "x86_64"
SCRIPT_MEMBER = "assets/boot_patch.sh"
UTILITY_MEMBER = "assets/util_functions.sh"
MAGISKBOOT_MEMBER = f"lib/{BUILDER_ABI}/libmagiskboot.so"
MAGISKINIT_MEMBER = f"lib/{GUEST_ABI}/libmagiskinit.so"
MAGISK_MEMBER = f"lib/{GUEST_ABI}/libmagisk.so"
INIT_LD_MEMBER = f"lib/{GUEST_ABI}/libinit-ld.so"
MAX_APK_MEMBER = 64 * 1024 * 1024

# What the patch is told, and so what the patched ramdisk's .backup/.magisk says:
# the verified boot and the forced encryption of the stock ramdisk stay as they
# are (the guest has neither), and the image is not a recovery or vendor boot.
PATCH_FLAGS = (
    ("KEEPVERITY", "true"),
    ("KEEPFORCEENCRYPT", "false"),
    ("RECOVERYMODE", "false"),
    ("VENDORBOOT", "false"),
)
# Magisk keeps its modules and settings on a partition it names at boot: the
# data disk's userdata, which first-stage init finds as the second block device
# of the guest. The position is read from the release's own layout.
PREINIT_DISK = "ubdb"
PREINIT_PARTITION = "userdata"

# What the patch adds to the stock ramdisk, with the mode of each entry.
ADDED_DIRECTORIES = {
    ".backup": stat.S_IFDIR | 0o000,
    "overlay.d": stat.S_IFDIR | 0o750,
    "overlay.d/sbin": stat.S_IFDIR | 0o750,
}
ADDED_FILES = {
    ".backup/.magisk": stat.S_IFREG | 0o000,
    ".backup/.rmlist": stat.S_IFREG | 0o000,
    "overlay.d/sbin/magisk.xz": stat.S_IFREG | 0o644,
    "overlay.d/sbin/init-ld.xz": stat.S_IFREG | 0o644,
}
BACKUP_INIT = ".backup/init.xz"
CONFIG_ENTRY = ".backup/.magisk"
REMOVAL_LIST_ENTRY = ".backup/.rmlist"
REMOVAL_LIST = (
    "overlay.d", "overlay.d/sbin", "overlay.d/sbin/init-ld.xz", "overlay.d/sbin/magisk.xz",
)

LZ4_LEGACY_MAGIC = b"\x02\x21\x4c\x18"
LZ4_LEGACY_BLOCK = 8 * 1024 * 1024
MAX_RAMDISK = 256 * 1024 * 1024
MAX_DECODED_MEMBER = 64 * 1024 * 1024
# The most a legacy block's compressed form can be (LZ4_compressBound of one
# chunk), the memory an xz decoder may use (a real member needs 16 MiB) and how
# large a boot image partition or an APK file may be before it is read.
LZ4_COMPRESS_BOUND = LZ4_LEGACY_BLOCK + LZ4_LEGACY_BLOCK // 255 + 16
MAX_XZ_MEMORY = 64 * 1024 * 1024
MAX_BOOT_IMAGE = 64 * 1024 * 1024
MAX_APK_FILE = 128 * 1024 * 1024
MAX_MANIFEST = 16 * 1024 * 1024
MAX_LAYOUT = android_image.MAX_LAYOUT


def apk_path(version: str) -> str:
    """The release path of the official APK of a Magisk version."""
    return f"{ROOT_DIRECTORY}/Magisk-{version}.apk"


def sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


# ------------------------------------------------------------------- the pin --


@dataclass(frozen=True)
class Pin:
    name: str
    version: str
    version_code: int
    apk_sha256: str
    apk_size: int


PIN_KEYS = {
    "LINEAGE24_MAGISK_SCHEMA",
    "LINEAGE24_MAGISK_NAME",
    "LINEAGE24_MAGISK_VERSION",
    "LINEAGE24_MAGISK_VERSION_CODE",
    "LINEAGE24_MAGISK_APK_SHA256",
    "LINEAGE24_MAGISK_APK_SIZE",
}


def read_pin(path: Path, error: Error) -> Pin:
    values = contract_checks.parse_assignments(path, error)
    if set(values) != PIN_KEYS:
        raise error("the Magisk pin does not have exactly its fields")
    if values["LINEAGE24_MAGISK_SCHEMA"] != "1":
        raise error("the Magisk pin is not schema 1")
    if values["LINEAGE24_MAGISK_NAME"] != MAGISK_NAME:
        raise error(f"the Magisk pin is not for {MAGISK_NAME}")
    version = values["LINEAGE24_MAGISK_VERSION"]
    if VERSION_RE.fullmatch(version) is None:
        raise error("the Magisk pin's version is malformed")
    digest = values["LINEAGE24_MAGISK_APK_SHA256"]
    if HEX64_RE.fullmatch(digest) is None:
        raise error("the Magisk pin's APK digest is not a SHA-256")
    code = int(values["LINEAGE24_MAGISK_VERSION_CODE"])
    size = int(values["LINEAGE24_MAGISK_APK_SIZE"])
    if code < 1 or size < 1:
        raise error("the Magisk pin's version code or APK size is zero")
    return Pin(MAGISK_NAME, version, code, digest, size)


# How a file is opened to be read: not through a link, and without waiting. open() of
# a pipe blocks until a writer comes, so what a file is has to be told by its
# descriptor (fstat), after an open that cannot block.
OPEN_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)


def read_pinned_apk(apk: Path, pin: Pin, error: Error) -> bytes:
    """The bytes of the APK, read once through one descriptor, after checking that
    they are the pinned Magisk release by size and SHA-256. A caller uses these
    bytes for everything it does with the APK and never reopens the path, so a file
    that is replaced meanwhile changes nothing."""
    unpinned = error(
        f"the Magisk APK is not the pinned {pin.name} {pin.version} "
        "(its size or SHA-256 differs from metadata/magisk.env)"
    )
    try:
        descriptor = os.open(apk, OPEN_FLAGS)
    except OSError as failure:
        raise error(f"the Magisk APK must be a regular non-symlink file: {apk} ({failure})") from failure
    with os.fdopen(descriptor, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise error(f"the Magisk APK must be a regular non-symlink file: {apk}")
        if info.st_size != pin.apk_size:
            raise unpinned
        data = handle.read(pin.apk_size + 1)
    if len(data) != pin.apk_size or sha256_bytes(data) != pin.apk_sha256:
        raise unpinned
    return data


def check_apk_is_pin(apk: Path, pin: Pin, error: Error) -> None:
    """The APK is the pinned Magisk release, by size and SHA-256."""
    read_pinned_apk(apk, pin, error)


# --------------------------------------------------------------- the record --


def _expect_keys(value: object, keys: set[str], label: str, error: Error) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        raise error(f"{label} must be an object with exactly {sorted(keys)}")
    return value


def _hex(value: object, pattern: re.Pattern[str], label: str, error: Error) -> str:
    if not isinstance(value, str) or pattern.fullmatch(value) is None:
        raise error(f"{label} is not a lowercase hexadecimal digest")
    return value


def _size(value: object, label: str, error: Error) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise error(f"{label} is not a positive integer")
    return value


def validate_root_record(value: object, error: Error) -> dict:
    """Check the `root` record a release manifest keeps; returns it."""
    record = _expect_keys(value, {"magisk", "apk", "boot_image"}, "the root record", error)
    magisk = _expect_keys(record["magisk"], {"name", "version_name", "version_code"},
                          "the root record's magisk", error)
    if magisk["name"] != MAGISK_NAME:
        raise error(f"the root record is not for {MAGISK_NAME}")
    version = magisk["version_name"]
    if not isinstance(version, str) or VERSION_RE.fullmatch(version) is None:
        raise error("the root record's Magisk version is malformed")
    _size(magisk["version_code"], "the root record's Magisk version code", error)
    apk = _expect_keys(record["apk"], {"path", "sha256", "size"}, "the root record's apk", error)
    if apk["path"] != apk_path(version):
        raise error(f"the root record's APK path is not {apk_path(version)}")
    _hex(apk["sha256"], HEX64_RE, "the root record's APK SHA-256", error)
    _size(apk["size"], "the root record's APK size", error)
    patched = _expect_keys(
        record["boot_image"],
        {"path", "sha256", "size", "stock_init_boot_sha256", "stock_init_boot_sha1"},
        "the root record's boot_image", error,
    )
    if patched["path"] != PATCHED_PATH:
        raise error(f"the root record's patched image path is not {PATCHED_PATH}")
    _hex(patched["sha256"], HEX64_RE, "the root record's patched image SHA-256", error)
    _size(patched["size"], "the root record's patched image size", error)
    _hex(patched["stock_init_boot_sha256"], HEX64_RE, "the root record's stock init_boot SHA-256", error)
    _hex(patched["stock_init_boot_sha1"], HEX40_RE, "the root record's stock init_boot SHA-1", error)
    return record


def root_record_digest(record: dict | None) -> str:
    """What an accepted-release descriptor keeps of a release's root record: the
    SHA-256 of its canonical JSON, or `none` for a release without root files."""
    if record is None:
        return "none"
    return sha256_bytes(
        json.dumps(record, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    )


def root_payload_specs(record: dict) -> dict[str, tuple[str, str]]:
    """The inventory role and format of each root file, by path."""
    return {
        record["boot_image"]["path"]: PATCHED_PAYLOAD,
        record["apk"]["path"]: APK_PAYLOAD,
    }


def root_files(record: dict) -> dict[str, dict]:
    """The recorded size and digest of each root file, by path."""
    return {
        record["boot_image"]["path"]: record["boot_image"],
        record["apk"]["path"]: record["apk"],
    }


def _mode(path: Path, label: str, error: Error) -> int | None:
    """The mode of `path` without following a link; None when it does not exist."""
    try:
        return os.lstat(path).st_mode
    except FileNotFoundError:
        return None
    except OSError as failure:
        raise error(f"cannot read {label}: {path} ({failure})") from failure


def real_directory(path: Path, label: str, error: Error) -> Path:
    """`path` is a directory itself, not a link to one."""
    mode = _mode(path, label, error)
    if mode is None or stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
        raise error(f"{label} must be a real directory, not a link or a file (and must exist): {path}")
    return path


def real_path_below(base: Path, relative: str, error: Error, *, directory: bool = False) -> Path:
    """`base` joined with `relative`, every component below `base` a real directory
    and the last a regular file (a real directory with `directory`): a link, a
    special file or a missing one on the way is refused."""
    current = base
    parts = PurePosixPath(relative).parts
    for number, part in enumerate(parts):
        current = current / part
        mode = _mode(current, relative, error)
        wants_directory = directory or number < len(parts) - 1
        if mode is None or stat.S_ISLNK(mode) or not (
            stat.S_ISDIR(mode) if wants_directory else stat.S_ISREG(mode)
        ):
            kind = "directory" if wants_directory else "file"
            raise error(f"{relative} must be a real {kind} (no link or special file, none missing): {current}")
    return current


def check_root_files(
    release_root: Path, record: dict, error: Error, *, image_limit: int | None = None
) -> dict[str, bytes]:
    """root/ is a real directory holding exactly the two declared files, each a
    regular file of the recorded size and SHA-256. Returns their bytes: a caller
    that goes on to use the files uses these and never reopens the paths.
    `image_limit` bounds the patched image (the stock partition's size)."""
    root_dir = release_root / ROOT_DIRECTORY
    mode = _mode(root_dir, "release root/", error)
    if mode is None:
        raise error("release root/ is partial: the directory is missing")
    if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
        raise error("release root/ must be a real directory, not a link or a file")
    files = root_files(record)
    declared = {PurePosixPath(relative).name for relative in files}
    try:
        held = set(os.listdir(root_dir))
    except OSError as failure:
        raise error(f"cannot read release root/: {failure}") from failure
    for relative in files:
        if PurePosixPath(relative).name not in held:
            raise error(f"release root/ is partial: {relative} is missing or unsafe")
    if held - declared:
        raise error(
            "release root/ holds files its manifest's root record does not declare: "
            f"{sorted(held - declared)}"
        )
    contents: dict[str, bytes] = {}
    for relative, entry in files.items():
        path = release_root / relative
        mode = _mode(path, relative, error)
        if mode is None or stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
            raise error(f"release root/ is partial: {relative} is missing or unsafe")
        is_image = relative == record["boot_image"]["path"]
        limit = (image_limit if image_limit is not None else MAX_BOOT_IMAGE) if is_image else MAX_APK_FILE
        if entry["size"] > limit:
            raise error(
                f"release root file {relative} is larger than the stock init_boot partition"
                if is_image and image_limit is not None
                else f"release root file {relative} is larger than {limit} bytes"
            )
        data = read_file(path, limit, f"release root file {relative}", error)
        if len(data) != entry["size"] or sha256_bytes(data) != entry["sha256"]:
            raise error(f"release root file differs from its recorded size or SHA-256: {relative}")
        contents[relative] = data
    return contents


# ------------------------------------------------------------ the disk side --


def read_file(path: Path, limit: int, label: str, error: Error) -> bytes:
    """A regular file of at most `limit` bytes, whose size is checked before it is
    read, opened without following a link."""
    try:
        descriptor = os.open(path, OPEN_FLAGS)
    except OSError as failure:
        raise error(f"{label} must be a regular non-symlink file: {path} ({failure})") from failure
    with os.fdopen(descriptor, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise error(f"{label} must be a regular non-symlink file: {path}")
        if info.st_size > limit:
            raise error(f"{label} is larger than {limit} bytes: {path}")
        data = handle.read(limit + 1)
    if len(data) > limit:
        raise error(f"{label} is larger than {limit} bytes: {path}")
    return data


def read_layout(path: Path, error: Error) -> bytes:
    """The bytes of a partition layout, read once with a bound. A caller reads the
    layout through this and passes the bytes on, so that the partition it reads and
    the pre-init device it names come from one layout."""
    return read_file(path, MAX_LAYOUT, "the partition layout", error)


def read_partition(disk: Path, layout: bytes, name: str, error: Error) -> bytes:
    """The bytes of a system disk partition, by the release's partition layout. The
    disk is opened once, without waiting for a writer, and its type and size are
    those of that descriptor, which the partition is read from."""
    try:
        descriptor = os.open(disk, OPEN_FLAGS)
    except OSError as failure:
        raise error(f"the system disk must be a regular non-symlink file: {disk} ({failure})") from failure
    with os.fdopen(descriptor, "rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise error(f"the system disk must be a regular non-symlink file: {disk}")
        start, size = android_image.partition_range_in(layout, info.st_size, name, error)
        if size > MAX_BOOT_IMAGE:
            raise error(f"partition {name} is larger than {MAX_BOOT_IMAGE} bytes")
        handle.seek(start)
        return android_image.read_exact(handle, size, f"partition {name}", error)


def preinit_device(layout: bytes, error: Error) -> str:
    """The guest's name of the data disk's userdata partition, from the layout."""
    try:
        document = json.loads(layout.decode("utf-8"))
        partitions = document["data_disk"]["partitions"]
        names = [entry["name"] for entry in partitions]
    except (UnicodeError, ValueError, KeyError, TypeError) as failure:
        raise error(f"cannot read the data disk partitions of the layout: {failure}") from failure
    if names.count(PREINIT_PARTITION) != 1:
        raise error(f"the data disk layout has no single {PREINIT_PARTITION} partition")
    return f"{PREINIT_DISK}{names.index(PREINIT_PARTITION) + 1}"


def backup_config(preinit: str, stock_sha1: str, *, with_preinit: bool = True) -> bytes:
    """The text of .backup/.magisk that the patch writes for these inputs."""
    lines = [f"{key}={value}" for key, value in PATCH_FLAGS]
    if with_preinit:
        lines.append(f"PREINITDEVICE={preinit}")
    lines.append(f"SHA1={stock_sha1}")
    return ("\n".join(lines) + "\n").encode("ascii")


# ------------------------------------------------------------- the formats --


# The kernel unpacks the ramdisk with LZ4_decompress_safe(block, 8 MiB) per block
# (lib/decompress_unlz4.c, lib/lz4/lz4_decompress.c). _lz4_block accepts what that
# function accepts, byte for byte, with one deliberate narrowing: it refuses a match
# offset of zero, which the C code turns into zero bytes. What the C code leans on
# is not "well-formed" in the format's own sense but its parsing restrictions, and
# those are what a block must meet here too:
#   - a sequence is the last one when its literals reach within 8 bytes of the end
#     of the input (or within 12 of the end of the output buffer), and the last
#     sequence must end exactly at the end of the input;
#   - a match must end at least 5 bytes before the end of the output buffer;
#   - the fast path for a sequence with up to 14 literals and a match of up to 18
#     bytes skips the last-sequence test when 16 bytes of input and 32 of output
#     remain, so such a block may end on an empty literal run after a match;
#   - a literal length of 15 and more is read only while 15 bytes of input remain.
LZ4_MIN_MATCH = 4
LZ4_LAST_LITERALS = 5
LZ4_MF_LIMIT = 12
LZ4_SHORTCUT_INPUT = 16
LZ4_SHORTCUT_OUTPUT = 32


def _lz4_block(block: bytes, limit: int, error: Error) -> bytes:
    """Decode one LZ4 block into a buffer of `limit` bytes, as the kernel does."""
    out = bytearray()
    index = 0
    end = len(block)
    if end == 0:
        raise error("empty lz4 block")
    try:
        while True:
            token = block[index]
            index += 1
            literals = token >> 4
            matched = token & 15
            if (
                literals != 15 and index < end - LZ4_SHORTCUT_INPUT
                and len(out) <= limit - LZ4_SHORTCUT_OUTPUT
            ):
                out += block[index:index + literals]
                index += literals
                offset = block[index] | (block[index + 1] << 8)
                index += 2
                if matched != 15 and 8 <= offset <= len(out):
                    out += _lz4_match(out, offset, matched + LZ4_MIN_MATCH)
                    continue
            else:
                if literals == 15:
                    if index >= end - 15:
                        raise error("truncated lz4 literal length")
                    while True:
                        extra = block[index]
                        index += 1
                        literals += extra
                        if not (index < end - 15 and extra == 255):
                            break
                if len(out) + literals > limit - LZ4_MF_LIMIT or index + literals > end - 8:
                    if index + literals != end or len(out) + literals > limit:
                        raise error(
                            "lz4 block does not end with its literals, or decodes to more "
                            "than its limit"
                        )
                    out += block[index:end]
                    return bytes(out)
                out += block[index:index + literals]
                index += literals
                offset = block[index] | (block[index + 1] << 8)
                index += 2
            if offset == 0 or offset > len(out):
                raise error("lz4 match offset is outside the decoded data")
            if matched == 15:
                while True:
                    extra = block[index]
                    index += 1
                    if index > end - LZ4_LAST_LITERALS:
                        raise error("truncated lz4 match length")
                    matched += extra
                    if extra != 255:
                        break
            length = matched + LZ4_MIN_MATCH
            if len(out) + length > limit - LZ4_LAST_LITERALS:
                raise error("lz4 match ends within the last 5 bytes of its buffer or past it")
            out += _lz4_match(out, offset, length)
    except IndexError:
        raise error("truncated lz4 block") from None


def _lz4_match(out: bytearray, offset: int, length: int) -> bytes:
    """The `length` bytes an LZ4 match at `offset` copies, overlapping included."""
    start = len(out) - offset
    if offset >= length:
        return bytes(out[start:start + length])
    pattern = bytes(out[start:])
    return (pattern * (length // offset + 1))[:length]


def lz4_legacy_decode(data: bytes, error: Error, limit: int = MAX_RAMDISK) -> bytes:
    """Decode a stream in the lz4 legacy frame: the magic, then blocks of a
    four-byte little-endian size and an LZ4 block of at most 8 MiB decoded."""
    if data[:4] != LZ4_LEGACY_MAGIC:
        raise error("ramdisk is not an lz4 legacy stream")
    index = 4
    out = bytearray()
    while index < len(data):
        if index + 4 > len(data):
            raise error("truncated lz4 legacy block size")
        (size,) = struct.unpack_from("<I", data, index)
        index += 4
        if size > LZ4_COMPRESS_BOUND:
            raise error("an lz4 legacy block is larger than an lz4 block of an 8 MiB chunk can be")
        if size == 0 or index + size > len(data):
            raise error("lz4 legacy block exceeds the stream")
        out += _lz4_block(data[index:index + size], LZ4_LEGACY_BLOCK, error)
        index += size
        if len(out) > limit:
            raise error("ramdisk decodes to more than its limit")
    return bytes(out)


def xz_decode(data: bytes, error: Error, limit: int = MAX_DECODED_MEMBER) -> bytes:
    """Decode a whole xz stream, refusing trailing data and an oversized result."""
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=MAX_XZ_MEMORY)
    try:
        out = decoder.decompress(data, max_length=limit + 1)
    except lzma.LZMAError as failure:
        raise error(f"xz member does not decode (or needs more than {MAX_XZ_MEMORY} bytes of memory): {failure}") from failure
    if len(out) > limit:
        raise error("xz member decodes to more than its limit")
    if not decoder.eof or decoder.unused_data:
        raise error("xz member is truncated or has trailing data")
    return out


NEWC_MAGIC = b"070701"


@dataclass(frozen=True)
class Entry:
    """An archive entry as a guest sees it. The inode, link count and time are kept
    for the checks of `parse_newc` and left out of the comparison: Magisk renumbers
    them when it writes the archive."""

    mode: int
    uid: int
    gid: int
    rdev: tuple[int, int]
    payload: bytes
    inode: int = field(default=0, compare=False)
    links: int = field(default=1, compare=False)


def parse_newc(data: bytes, error: Error, *, guest: bool = False) -> dict[str, Entry]:
    """Map each path of an uncompressed newc archive (magic 070701) to its entry.
    Only identical empty directories may repeat, and only zero bytes may follow the
    trailer.

    With `guest`, the archive is one a guest's kernel unpacks, and it must unpack to
    what its entries say. The kernel makes a regular file whose link count is two or
    more a hard link to the earlier entry of the same device and inode and writes the
    body into that shared inode; it drops an entry whose parent directory is not
    unpacked yet; and it checks the sum of a 070702 archive. So a non-directory entry
    has a link count of one and an inode of its own (Magisk writes both), and a
    directory entry comes before everything below it.
    """
    entries: dict[str, Entry] = {}
    inodes: dict[tuple[int, int, int], str] = {}
    index = 0
    while True:
        header = data[index:index + android_image.NEWC_HEADER_SIZE]
        if len(header) != android_image.NEWC_HEADER_SIZE:
            raise error("truncated newc header")
        if header[:6] != NEWC_MAGIC:
            raise error("archive is not an uncompressed newc archive (magic 070701)")
        try:
            fields = [int(header[6 + 8 * n:14 + 8 * n], 16) for n in range(13)]
        except ValueError as failure:
            raise error(f"malformed newc header: {failure}") from failure
        inode, mode, uid, gid, links, file_size = (fields[n] for n in (0, 1, 2, 3, 4, 6))
        rdev = (fields[9], fields[10])
        name_size = fields[11]
        if file_size > android_image.MAX_NEWC_ENTRY_SIZE or not 2 <= name_size <= 4096:
            raise error("newc entry has an invalid size")
        index += android_image.NEWC_HEADER_SIZE
        raw = data[index:index + name_size]
        if len(raw) != name_size or raw[-1:] != b"\0":
            raise error("newc path is truncated or not NUL terminated")
        index = android_image.align4(index + name_size)
        if raw[:-1] == b"TRAILER!!!":
            if file_size != 0:
                raise error("newc trailer has data")
            if any(data[index:]):
                raise error("newc archive has nonzero data after its trailer")
            return entries
        name = android_image.safe_name(raw[:-1], error)
        payload = data[index:index + file_size]
        if len(payload) != file_size:
            raise error(f"truncated payload for {name}")
        index = android_image.align4(index + file_size)
        previous = entries.get(name)
        if previous is not None and not (
            stat.S_ISDIR(previous.mode) and stat.S_ISDIR(mode)
            and previous.mode == mode and not previous.payload and not payload
        ):
            raise error(f"ambiguous duplicate archive entry: {name}")
        if guest:
            parent = name.rpartition("/")[0]
            if parent and not (parent in entries and stat.S_ISDIR(entries[parent].mode)):
                raise error(f"newc entry {name} comes before its parent directory")
            if not stat.S_ISDIR(mode):
                if links != 1:
                    raise error(f"newc entry {name} has {links} links: a guest unpacks it as a hard link")
                owner = inodes.setdefault((fields[7], fields[8], inode), name)
                if owner != name:
                    raise error(f"newc entries {owner} and {name} share an inode")
        entries[name] = Entry(mode, uid, gid, rdev, payload, inode, links)


def boot_ramdisk(image: bytes, label: str, error: Error) -> tuple[android_image.BootImage, bytes]:
    """The ramdisk of a kernelless boot-image v4, with nothing after it but zeros."""
    info = android_image.parse_boot_image(io.BytesIO(image), 0, len(image), error)
    end = info.ramdisk_offset + info.ramdisk_size
    if any(image[end:]):
        raise error(f"{label} has data after its ramdisk")
    return info, image[info.ramdisk_offset:end]


def decode_ramdisk(ramdisk: bytes, label: str, error: Error) -> bytes:
    """The raw newc archive of a ramdisk that is raw or lz4 legacy."""
    if ramdisk[:6] == NEWC_MAGIC:
        return ramdisk
    if ramdisk[:4] == LZ4_LEGACY_MAGIC:
        return lz4_legacy_decode(ramdisk, error)
    raise error(f"{label} ramdisk is neither a raw newc archive nor lz4 legacy")


# ------------------------------------------------------------- the checks --


def apk_member(archive: zipfile.ZipFile, name: str, error: Error) -> bytes:
    try:
        info = archive.getinfo(name)
    except KeyError:
        raise error(f"the Magisk APK has no {name}") from None
    if info.is_dir() or info.file_size <= 0 or info.file_size > MAX_APK_MEMBER:
        raise error(f"the Magisk APK member has an unsafe size: {name}")
    try:
        return archive.read(info)
    except (OSError, EOFError, NotImplementedError, RuntimeError, ValueError, zipfile.BadZipFile,
            zlib.error, lzma.LZMAError) as failure:
        raise error(f"cannot read {name} of the Magisk APK: {failure}") from failure


def open_apk(apk: Path | bytes, error: Error) -> zipfile.ZipFile:
    """The APK as a zip archive, from a path or from bytes already read."""
    try:
        return zipfile.ZipFile(io.BytesIO(apk) if isinstance(apk, bytes) else apk)
    except (OSError, zipfile.BadZipFile) as failure:
        raise error(f"the Magisk APK is not a readable archive: {failure}") from failure


def verify_patched(
    patched: bytes,
    stock: bytes,
    apk: zipfile.ZipFile,
    preinit: str,
    error: Error,
) -> None:
    """The patched init_boot is its stock image plus what Magisk adds, and nothing else.

    `patched` and `stock` are whole images. "Stock plus what the patch adds" is:
      - the 4 KiB header equal except the ramdisk size, and nothing but zeros after
        the ramdisk;
      - a ramdisk of the patched image that is lz4 legacy, and a ramdisk of the stock
        image, that each unpack in a guest's kernel to their entries as listed
        (070701, link count one, an inode of its own, parents before children:
        `parse_newc` with `guest`);
      - exactly the entries of the stock archive plus .backup, .backup/.magisk,
        .backup/.rmlist, .backup/init.xz, overlay.d, overlay.d/sbin and
        overlay.d/sbin/{magisk,init-ld}.xz, which the stock archive does not hold;
      - every stock entry but /init equal in mode, owner, device numbers and
        payload (inode, link count, time and order are not compared);
      - /init the APK's arm64 magiskinit, and .backup/init.xz the stock /init;
      - overlay.d/sbin/{magisk,init-ld}.xz the APK's arm64 binaries;
      - the added entries with their fixed modes and root ownership, the removal
        list naming the entries the patch added, and .backup/.magisk the fixed
        patch flags, the pre-init partition and the SHA-1 of the stock image.
    """
    _, stock_ramdisk = boot_ramdisk(stock, "stock init_boot", error)
    _, patched_ramdisk = boot_ramdisk(patched, "patched init_boot", error)
    stock_header = bytearray(stock[:android_image.BOOT_PAGE_SIZE])
    patched_header = bytearray(patched[:android_image.BOOT_PAGE_SIZE])
    stock_header[12:16] = patched_header[12:16] = b"\0\0\0\0"
    if stock_header != patched_header:
        raise error("the patched boot image header differs from the stock one beyond its ramdisk size")
    if patched_ramdisk[:4] != LZ4_LEGACY_MAGIC:
        raise error("the patched ramdisk is not lz4 legacy")

    # Both archives are read as a guest unpacks them: the comparison is of the
    # filesystems the two make, and an archive the kernel would unpack otherwise
    # than its entries say, on either side, has no such filesystem to compare.
    before = parse_newc(
        decode_ramdisk(stock_ramdisk, "stock", error),
        lambda message: error(f"the stock ramdisk: {message}"), guest=True)
    after = parse_newc(
        decode_ramdisk(patched_ramdisk, "patched", error),
        lambda message: error(f"the patched ramdisk: {message}"), guest=True)
    added = set(ADDED_DIRECTORIES) | set(ADDED_FILES) | {BACKUP_INIT}
    if set(before) & added:
        raise error("the stock ramdisk already holds entries the patch adds")
    stock_init = before.get("init")
    if stock_init is None or not stat.S_ISREG(stock_init.mode):
        raise error("the stock ramdisk has no regular /init")
    missing = sorted(set(before) - set(after))
    if missing:
        raise error(f"the patch removed stock entries: {missing[:4]}")
    unexpected = sorted(set(after) - set(before) - added)
    if unexpected:
        raise error(f"the patch added unexpected entries: {unexpected[:4]}")
    absent = sorted(added - set(after))
    if absent:
        raise error(f"the patch did not add entries it must: {absent[:4]}")
    for name, entry in before.items():
        if name != "init" and after[name] != entry:
            raise error(f"the patch changed the stock entry {name}")

    init = after["init"]
    if (
        init.payload != apk_member(apk, MAGISKINIT_MEMBER, error)
        or init.mode != stat.S_IFREG | 0o750
        or init.uid != 0 or init.gid != 0 or init.rdev != (0, 0)
    ):
        raise error("/init of the patched ramdisk is not the APK's arm64 magiskinit")
    for name, expected in {**ADDED_DIRECTORIES, **ADDED_FILES}.items():
        entry = after[name]
        if entry.mode != expected or entry.uid != 0 or entry.gid != 0 or entry.rdev != (0, 0):
            raise error(f"the patched ramdisk's {name} has the wrong mode or owner")
    backup = after[BACKUP_INIT]
    if backup.mode != stock_init.mode or backup.uid != 0 or backup.gid != 0:
        raise error(f"the patched ramdisk's {BACKUP_INIT} has the wrong mode or owner")
    if xz_decode(backup.payload, error) != stock_init.payload:
        raise error(f"{BACKUP_INIT} is not the stock /init")
    for name, member in (
        ("overlay.d/sbin/magisk.xz", MAGISK_MEMBER),
        ("overlay.d/sbin/init-ld.xz", INIT_LD_MEMBER),
    ):
        if xz_decode(after[name].payload, error) != apk_member(apk, member, error):
            raise error(f"{name} is not the APK's arm64 {member.rsplit('/', 1)[1]}")
    removal = after[REMOVAL_LIST_ENTRY].payload
    if removal != b"".join(entry.encode("ascii") + b"\0" for entry in REMOVAL_LIST):
        raise error("the patched ramdisk's removal list is not the list of the entries it added")
    expected_config = backup_config(preinit, hashlib.sha1(stock).hexdigest())
    if after[CONFIG_ENTRY].payload != expected_config:
        raise error(
            "the patched ramdisk's .backup/.magisk is not the fixed patch inputs, the data "
            "disk's pre-init partition and the SHA-1 of the stock image"
        )


def verify_release_root(
    release_root: Path, error: Error, pin: Pin | None = None, manifest: dict | None = None
) -> dict:
    """Verify the root files of a release directory against its own manifest.

    The manifest's root record is well formed, both files are the recorded
    ones, the recorded stock digests are those of the release's own init_boot
    partition, and the patched image verifies against that partition and the
    APK. With a pin, the record is the pinned Magisk release. Returns the record.
    `manifest` is the manifest as the caller already read it: its record is the one
    verified, and the file is not read again.
    """
    if manifest is None:
        manifest_path = real_path_below(release_root, MANIFEST, error)
        manifest = contract_checks.parse_json_object(
            read_file(manifest_path, MAX_MANIFEST, "the release manifest", error),
            str(manifest_path), error)
    if "root" not in manifest:
        raise error("the release manifest has no root record")
    record = validate_root_record(manifest["root"], error)
    disk = real_path_below(release_root, SYSTEM_DISK, error)
    layout = read_layout(real_path_below(release_root, PARTITION_LAYOUT, error), error)
    stock = read_partition(disk, layout, "init_boot", error)
    contents = check_root_files(release_root, record, error, image_limit=len(stock))
    if pin is not None:
        magisk, apk = record["magisk"], record["apk"]
        if (magisk["version_name"], magisk["version_code"]) != (pin.version, pin.version_code):
            raise error("the root record is not the pinned Magisk version")
        if (apk["sha256"], apk["size"]) != (pin.apk_sha256, pin.apk_size):
            raise error("the Magisk APK digest is not the pinned one")
    patched = record["boot_image"]
    if (
        sha256_bytes(stock) != patched["stock_init_boot_sha256"]
        or hashlib.sha1(stock).hexdigest() != patched["stock_init_boot_sha1"]
    ):
        raise error("the root record's stock digests are not those of the release's init_boot")
    with open_apk(contents[record["apk"]["path"]], error) as apk:
        verify_patched(
            contents[patched["path"]],
            stock,
            apk,
            preinit_device(layout, error),
            error,
        )
    return record
