#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Magisk root files of an Android release.

A release can carry, beside its system disk, a copy of its own init_boot that
Magisk's patcher made and the official Magisk APK it was made from, both from the
one release pinned in android/lineage-24/metadata/magisk.env. These tests use the
synthetic inputs of magisk_fixture.py: no Magisk file is in the repository.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import errno
import hashlib
import importlib.util
import io
import json
import lzma
import os
import pathlib
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
import zipfile
import zlib
from unittest import mock
from pathlib import Path

import magisk_fixture
from magisk_fixture import (
    APK_NATIVE, TEST_VERSION, Fixture, lz4_legacy, lz4_matches, make_apk_bytes,
    newc_dump, patch_script, apk_members, pseudo_random, reference_patched, stock_entries,
)

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / "android/lineage-24/tools"
PIN_KEYS = {
    "LINEAGE24_MAGISK_SCHEMA",
    "LINEAGE24_MAGISK_NAME",
    "LINEAGE24_MAGISK_VERSION",
    "LINEAGE24_MAGISK_VERSION_CODE",
    "LINEAGE24_MAGISK_APK_SHA256",
    "LINEAGE24_MAGISK_APK_SIZE",
}


def run_tool(
    script: str, *arguments: object, timeout: int = 120, env: dict[str, object] | None = None
) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(TOOLS / script), *map(str, arguments)],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout, check=False,
        env=None if env is None else {**os.environ, **{key: str(value) for key, value in env.items()}},
    )


# -------------------------------------------------------------------- tests --


class PinTests(unittest.TestCase):
    """The tracked pin names the release and its digest, and nothing about where a copy came from."""

    def setUp(self) -> None:
        self.path = ROOT / "android/lineage-24/metadata/magisk.env"
        self.pin = contract_checks.parse_assignments(self.path)

    def test_the_pin_has_exactly_its_fields(self) -> None:
        self.assertEqual(set(self.pin), PIN_KEYS)
        self.assertEqual(self.pin["LINEAGE24_MAGISK_SCHEMA"], "1")
        self.assertEqual(self.pin["LINEAGE24_MAGISK_NAME"], "Magisk")

    def test_the_version_and_digest_are_well_formed(self) -> None:
        self.assertRegex(self.pin["LINEAGE24_MAGISK_VERSION"], r"^v[0-9]+\.[0-9]+(\.[0-9]+)?$")
        self.assertGreater(int(self.pin["LINEAGE24_MAGISK_VERSION_CODE"]), 0)
        self.assertRegex(self.pin["LINEAGE24_MAGISK_APK_SHA256"], r"^[0-9a-f]{64}$")
        self.assertGreater(int(self.pin["LINEAGE24_MAGISK_APK_SIZE"]), 0)

    def test_the_pin_records_no_source_location(self) -> None:
        text = self.path.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"https?:|git@|\.com|\.org|/Users/|/home/|/Volumes/", text))

    def test_the_producer_files_are_classified(self) -> None:
        rows = release_inputs.read_rows(ROOT / release_inputs.LIST_RELATIVE)
        kinds = {path: kind for kind, path, _ in rows}
        self.assertEqual(kinds["android/lineage-24/metadata/magisk.env"], "input")
        # The producer and what it shares with the verifier decide the release's
        # bytes; the verifier's own command line does not.
        self.assertEqual(kinds["android/lineage-24/tools/magisk-boot.py"], "input")
        self.assertEqual(kinds["android/lineage-24/tools/magisk_image.py"], "input")
        # The producer reads the partition it patches through the shared image reader, so
        # that reader decides which bytes Magisk patches.
        self.assertEqual(kinds["android/lineage-24/tools/android_image.py"], "input")
        self.assertEqual(kinds["android/lineage-24/tools/verify-magisk-boot.py"], "excluded")

    def test_the_tracked_pin_reads(self) -> None:
        pin = magisk_image.read_pin(self.path, SystemExit)
        self.assertEqual((pin.name, pin.version), ("Magisk", self.pin["LINEAGE24_MAGISK_VERSION"]))
        self.assertEqual(pin.apk_size, int(self.pin["LINEAGE24_MAGISK_APK_SIZE"]))

    def test_a_malformed_pin_is_refused(self) -> None:
        text = self.path.read_text(encoding="utf-8")
        for label, edit in (
            ("another name", lambda t: t.replace("'Magisk'", "'Other'")),
            ("a version without a v", lambda t: re.sub(r"='v([0-9.]+)'", r"='\1'", t, count=1)),
            ("a short digest", lambda t: re.sub(r"[0-9a-f]{64}", "abc", t)),
            ("a missing size", lambda t: re.sub(r"LINEAGE24_MAGISK_APK_SIZE=.*\n", "", t)),
            ("another schema", lambda t: t.replace("SCHEMA=1", "SCHEMA=2")),
            ("a zero size", lambda t: re.sub(r"(APK_SIZE)=[0-9]+", r"\1=0", t)),
        ):
            with self.subTest(label), tempfile.TemporaryDirectory() as directory:
                broken = Path(directory) / "magisk.env"
                broken.write_text(edit(text), encoding="utf-8")
                with self.assertRaises(SystemExit):
                    magisk_image.read_pin(broken, SystemExit)


class FormatTests(unittest.TestCase):
    """The decoders read what Magisk writes: lz4 legacy, xz, and newc with renumbered inodes."""

    def test_lz4_blocks_with_matches_decode(self) -> None:
        samples = (
            b"abcdabcdabcdabcd",
            b"a" * 300,
            b"xyz" * 1000 + b"tail",
            pseudo_random("lz4", 700) + pseudo_random("lz4", 700) + b"end",
            bytes(range(256)) * 8,
        )
        for data in samples:
            with self.subTest(length=len(data)):
                self.assertEqual(
                    magisk_image._lz4_block(lz4_matches(data), 1 << 20, SystemExit), data)

    def test_lz4_blocks_end_as_the_kernels_decoder_requires(self) -> None:
        # What the guest unpacks is what LZ4_decompress_safe accepts into an 8 MiB
        # buffer. That decoder treats a sequence whose literals reach within 8 bytes
        # of the end of the input as the last one, so a match followed by an empty
        # final token is refused unless the fast path is taken (16 bytes of input and
        # 32 of output remain): the sample below is the old positive fixture, which
        # it refused.
        literals = b"0123456789abcdefgh"
        limit = 8 * 1024 * 1024
        decode = lambda block, size=limit: magisk_image._lz4_block(block, size, SystemExit)  # noqa: E731
        with self.assertRaisesRegex(SystemExit, "does not end with its literals"):
            decode(bytes([0x48]) + b"abcd" + bytes([4, 0]) + b"\x00")
        with self.assertRaisesRegex(SystemExit, "does not end with its literals"):
            decode(bytes([0xD0]) + literals[:13] + bytes([8, 0]) + b"\x00")
        self.assertEqual(
            decode(bytes([0xE0]) + literals[:14] + bytes([8, 0]) + b"\x00"), literals[:14] + b"6789")
        self.assertEqual(decode(bytes([0x50]) + b"hello"), b"hello")
        data = b"abcd" * 10
        block = lz4_matches(data)
        self.assertEqual(decode(block, len(data)), data)
        with self.assertRaisesRegex(SystemExit, "more than its limit|within the last 5 bytes"):
            decode(block, len(data) - 1)

    def test_lz4_legacy_streams_decode_and_bad_ones_are_refused(self) -> None:
        data = b"hello ramdisk " * 5000
        stream = b"\x02\x21\x4c\x18"
        for start in range(0, len(data), 40000):
            block = lz4_matches(data[start:start + 40000])
            stream += struct.pack("<I", len(block)) + block
        self.assertEqual(magisk_image.lz4_legacy_decode(stream, SystemExit), data)
        self.assertEqual(magisk_image.lz4_legacy_decode(lz4_legacy(data), SystemExit), data)
        magic = b"\x02\x21\x4c\x18"
        for label, bad in (
            ("a missing magic", stream[1:]),
            ("a block that runs past the stream", stream[:-3]),
            ("a size field cut short", stream + b"\x01\x02"),
            ("a zero size", stream + b"\0\0\0\0"),
            ("an offset before the data", magic + struct.pack("<I", 4) + bytes([0x04, 0x61, 0x09, 0x00])),
            ("a zero offset", magic + struct.pack("<I", 4) + bytes([0x10, 0x61, 0x00, 0x00])),
            ("literals past the block", magic + struct.pack("<I", 2) + bytes([0xF0, 0x01])),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                magisk_image.lz4_legacy_decode(bad, SystemExit)

    def test_decoders_are_bounded_before_they_allocate(self) -> None:
        magic = b"\x02\x21\x4c\x18"
        # A block larger than the largest LZ4 block of an 8 MiB chunk is refused by its size field.
        bound = 8 * 1024 * 1024 + 8 * 1024 * 1024 // 255 + 16
        oversized = magic + struct.pack("<I", bound + 1) + bytes(bound + 1)
        with self.assertRaisesRegex(SystemExit, "larger than an lz4 block"):
            magisk_image.lz4_legacy_decode(oversized, SystemExit)
        # A literal run past the limit is refused without being copied: the peak
        # allocation while decoding stays far below the run.
        run = 9 * 1024 * 1024
        extension = b"\xff" * ((run - 15) // 255) + bytes([(run - 15) % 255])
        block = bytes([0xF0]) + extension + bytes(run)
        stream = magic + struct.pack("<I", len(block)) + block
        tracemalloc.start()
        try:
            with self.assertRaises(SystemExit):
                magisk_image.lz4_legacy_decode(stream, SystemExit)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 1024 * 1024)
        # An xz stream that declares a dictionary of 4 GiB is refused by the decoder's
        # memory limit; the members of a real patch need 16 MiB.
        packed = lzma.compress(b"hello" * 10, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64,
                               filters=[{"id": lzma.FILTER_LZMA2, "dict_size": 1 << 20}])
        header_size = (packed[12] + 1) * 4
        header = bytearray(packed[12:12 + header_size])
        header[header.index(b"\x21\x01", 2) + 2] = 40
        header[-4:] = struct.pack("<I", zlib.crc32(bytes(header[:-4])))
        huge = packed[:12] + bytes(header) + packed[12 + header_size:]
        with self.assertRaisesRegex(SystemExit, "(?i)memory"):
            magisk_image.xz_decode(huge, SystemExit)
        self.assertEqual(magisk_image.xz_decode(packed, SystemExit), b"hello" * 10)

    def test_a_corrupt_member_of_the_package_is_a_named_refusal(self) -> None:
        for label, cut in (("corrupt deflate data", False), ("a truncated deflate stream", True)):
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr(magisk_image.MAGISKINIT_MEMBER, bytes(range(256)) * 4000)
            data = bytearray(buffer.getvalue())
            info = zipfile.ZipFile(io.BytesIO(bytes(data))).getinfo(magisk_image.MAGISKINIT_MEMBER)
            start = info.header_offset + 30 + len(info.filename.encode())
            if cut:
                for index in range(start + 40, start + info.compress_size):
                    data[index] = 0
            else:
                for index in range(start + 2, start + 60):
                    data[index] ^= 0x5A
            with self.subTest(label), zipfile.ZipFile(io.BytesIO(bytes(data))) as archive, \
                    self.assertRaisesRegex(SystemExit, "cannot read lib/arm64-v8a/libmagiskinit.so"):
                magisk_image.apk_member(archive, magisk_image.MAGISKINIT_MEMBER, SystemExit)

    def test_a_partition_larger_than_a_boot_image_can_be_is_refused_before_it_is_read(self) -> None:
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        size = 80 * 1024 * 1024
        disk = directory / "system.img"
        with disk.open("wb") as handle:
            handle.truncate(size)
        layout = directory / "partition-layout.json"
        layout.write_text(json.dumps({
            "schema_version": 2,
            "system_disk": {"size_bytes": size, "partitions": [
                {"name": "init_boot", "start_bytes": 4096, "size_bytes": size - 8192}]},
        }))
        with self.assertRaisesRegex(SystemExit, "larger than"):
            magisk_image.read_partition(disk, layout.read_bytes(), "init_boot", SystemExit)

    def test_the_system_disk_is_typed_and_sized_on_the_descriptor_it_is_read_from(self) -> None:
        # A regular disk replaced by a pipe between a size check by path and the open
        # would wait for a writer. The disk is opened once, without waiting, and its
        # type and size are those of that descriptor. The alarm fails a regression
        # instead of hanging it.
        directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        fixture = Fixture(directory)
        layout = fixture.layout.read_bytes()

        class Stuck(Exception):
            pass

        def alarm(signum, frame):
            raise Stuck()

        previous = signal.signal(signal.SIGALRM, alarm)
        self.addCleanup(signal.signal, signal.SIGALRM, previous)
        real_stat = os.stat
        swapped = []

        def stat_then_swap(path, *arguments, **keywords):
            result = real_stat(path, *arguments, **keywords)
            if str(path) == str(fixture.disk) and not swapped:
                swapped.append(path)
                os.unlink(fixture.disk)
                os.mkfifo(fixture.disk)
            return result

        @contextlib.contextmanager
        def replaced_after_a_size_check():
            with mock.patch.object(os, "stat", stat_then_swap):
                yield

        @contextlib.contextmanager
        def a_pipe_from_the_start():
            os.unlink(fixture.disk)
            os.mkfifo(fixture.disk)
            yield

        original = fixture.disk.read_bytes()
        for label, scenario in (
            ("replaced after a size check by path", replaced_after_a_size_check),
            ("a pipe from the start", a_pipe_from_the_start),
        ):
            with self.subTest(label):
                fixture.disk.unlink(missing_ok=True)
                fixture.disk.write_bytes(original)
                swapped.clear()
                signal.alarm(10)
                try:
                    with scenario():
                        try:
                            stock = magisk_image.read_partition(fixture.disk, layout, "init_boot", SystemExit)
                        except SystemExit as refused:
                            self.assertRegex(str(refused), "regular non-symlink file")
                        else:
                            self.assertEqual(stock, fixture.stock_image)
                except Stuck:
                    self.fail("the system disk was waited for")
                finally:
                    signal.alarm(0)

    def test_xz_members_decode_and_bad_ones_are_refused(self) -> None:
        packed = lzma.compress(b"member", format=lzma.FORMAT_XZ)
        self.assertEqual(magisk_image.xz_decode(packed, SystemExit), b"member")
        for label, bad in (
            ("trailing data", packed + b"x"),
            ("a truncated stream", packed[:-4]),
            ("not xz", b"plain"),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                magisk_image.xz_decode(bad, SystemExit)
        with self.assertRaises(SystemExit):
            magisk_image.xz_decode(
                lzma.compress(b"\0" * 5000, format=lzma.FORMAT_XZ), SystemExit, limit=100)

    def test_newc_keeps_the_fields_a_guest_sees_and_not_the_numbers(self) -> None:
        entries = stock_entries()
        parsed = magisk_image.parse_newc(newc_dump(entries, order=list(entries), tail=512), SystemExit)
        self.assertEqual(parsed, entries)
        self.assertEqual(
            magisk_image.parse_newc(newc_dump(entries, mtime=7), SystemExit),
            magisk_image.parse_newc(newc_dump(entries, mtime=9, order=sorted(entries)), SystemExit))
        for label, bad in (
            ("data after the trailer", newc_dump(entries) + b"x"),
            ("a truncated archive", newc_dump(entries)[:-40]),
            ("not newc", b"x" * 300),
            ("an unsafe name", newc_dump({"../x": entries["init"]})),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                magisk_image.parse_newc(bad, SystemExit)
        single = newc_dump({"a": entries["init"]})
        with self.assertRaises(SystemExit):
            magisk_image.parse_newc(single[:-124] + single, SystemExit)


class VerifierTests(unittest.TestCase):
    """verify_patched accepts the patch of the stock image and refuses each departure from it."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.fixture = Fixture(self.directory)
        self.apk_path = self.fixture.apk(script=patch_script())

    def patched(self, edit=None, *, preinit: str | None = None, compress=lz4_legacy,
                os_version: int | None = None, **archive) -> bytes:
        entries = reference_patched(
            self.fixture.stock, preinit or self.fixture.preinit, self.fixture.stock_sha1)
        if edit is not None:
            edit(entries)
        header = bytearray(self.fixture.stock_image[:4096])
        if os_version is not None:
            struct.pack_into("<I", header, 16, os_version)
        ramdisk = compress(newc_dump(entries, **archive))
        struct.pack_into("<I", header, 12, len(ramdisk))
        image = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
        return image + b"\0" * max(0, len(self.fixture.stock_image) - len(image))

    def verify(self, patched: bytes, preinit: str | None = None) -> None:
        with zipfile.ZipFile(self.apk_path) as archive:
            magisk_image.verify_patched(
                patched, self.fixture.stock_image, archive, preinit or self.fixture.preinit, SystemExit)

    def test_the_patch_of_the_stock_image_verifies(self) -> None:
        self.verify(self.patched())
        self.assertEqual(self.patched(), self.fixture.patched_reference())

    def test_departures_from_the_patch_are_refused(self) -> None:
        Entry = magisk_image.Entry
        xz = lambda payload: lzma.compress(payload, format=lzma.FORMAT_XZ)  # noqa: E731
        config = magisk_image.backup_config("ubdb3", self.fixture.stock_sha1)

        def replace(name, **changes):
            def edit(entries):
                entry = entries[name]
                entries[name] = Entry(**{
                    "mode": entry.mode, "uid": entry.uid, "gid": entry.gid, "rdev": entry.rdev,
                    "payload": entry.payload, **changes})
            return edit

        def drop(name):
            return lambda entries: entries.pop(name)

        def add(name):
            return lambda entries: entries.__setitem__(
                name, Entry(stat.S_IFREG | 0o644, 0, 0, (0, 0), b"x"))

        for label, edit in (
            ("an /init that is not the APK's magiskinit", replace("init", payload=b"stock-init-binary")),
            ("an /init without the execute bits", replace("init", mode=stat.S_IFREG | 0o644)),
            ("an /init owned by a user", replace("init", uid=1000)),
            ("a changed stock entry", replace("system/bin/toolbox", payload=b"other")),
            ("a stock entry with another owner", replace("system/bin/toolbox", gid=0)),
            ("a stock entry with another mode", replace("system/bin/toolbox", mode=stat.S_IFREG | 0o700)),
            ("a changed link target", replace("sbin", payload=b"/system/xbin")),
            ("a removed stock entry", drop("system/bin/toolbox")),
            ("an unexpected added entry", add("system/bin/su")),
            ("a missing magisk.xz", drop("overlay.d/sbin/magisk.xz")),
            ("a missing init-ld.xz", drop("overlay.d/sbin/init-ld.xz")),
            ("a missing backup of /init", drop(".backup/init.xz")),
            ("a missing configuration", drop(".backup/.magisk")),
            ("a missing removal list", drop(".backup/.rmlist")),
            ("a missing .backup directory", drop(".backup")),
            ("a magisk.xz that is not the APK's", replace("overlay.d/sbin/magisk.xz", payload=xz(b"other"))),
            ("an init-ld.xz that is not the APK's", replace("overlay.d/sbin/init-ld.xz", payload=xz(b"other"))),
            ("a magisk.xz with trailing data", replace(
                "overlay.d/sbin/magisk.xz", payload=xz(APK_NATIVE["lib/arm64-v8a/libmagisk.so"]) + b"x")),
            ("a backup that is not the stock /init", replace(".backup/init.xz", payload=xz(b"other"))),
            ("an overlay directory with another mode", replace("overlay.d", mode=stat.S_IFDIR | 0o755)),
            ("a configuration with another flag", replace(
                ".backup/.magisk", payload=config.replace(b"KEEPVERITY=true", b"KEEPVERITY=false"))),
            ("a configuration with another SHA-1", replace(
                ".backup/.magisk", payload=magisk_image.backup_config("ubdb3", "0" * 40))),
            ("a configuration without the pre-init partition", replace(
                ".backup/.magisk", payload=magisk_image.backup_config(
                    "ubdb3", self.fixture.stock_sha1, with_preinit=False))),
            ("a removal list that misses an entry", replace(".backup/.rmlist", payload=b"overlay.d\0overlay.d/sbin\0")),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                self.verify(self.patched(edit))

    def test_an_archive_the_kernel_would_unpack_differently_is_refused(self) -> None:
        # "Stock plus what Magisk adds" is about the files the guest ends up with, and
        # the kernel's unpacker does more than copy entries: a regular file with a link
        # count of two or more becomes a hard link to the earlier entry of the same
        # inode, written into the shared inode; a file whose parent is not yet unpacked
        # is dropped; a CRC archive is checked and stops at its first bad sum. Real
        # Magisk output has link count 1, unique inodes, parents first and 070701.
        names = sorted(reference_patched(self.fixture.stock, "ubdb3", self.fixture.stock_sha1))
        init, later, earlier = "init", "system/bin/toolbox", "first_stage_ramdisk/system/etc/fstab.uml"
        self.assertLess(names.index(earlier), names.index(init))
        self.assertLess(names.index(init), names.index(later))
        shared = 4242
        moved = [name for name in names if name != "overlay.d/sbin/magisk.xz"]
        moved.insert(moved.index("overlay.d"), "overlay.d/sbin/magisk.xz")
        stock_last = [name for name in names if name != "system/bin"]
        stock_last.insert(stock_last.index("system/bin/toolbox") + 1, "system/bin")
        for label, archive in (
            ("a later file hard linked to /init (the kernel writes it into /init)",
             dict(inodes={init: shared, later: shared}, links={init: 2, later: 2})),
            ("an earlier file hard linked to /init (the kernel writes /init into it)",
             dict(inodes={earlier: shared, init: shared}, links={earlier: 2, init: 2})),
            ("a link count of two on a lone file", dict(links={later: 2})),
            ("one inode on two files", dict(inodes={init: shared, later: shared})),
            ("an overlay file before its directory", dict(order=moved)),
            ("a stock file before its directory", dict(order=stock_last)),
            ("a CRC archive entry", dict(magics={init: b"070702"})),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                self.verify(self.patched(**archive))
        # A directory's own link count and inode are nobody's business: cpio writes 2.
        self.verify(self.patched(links={"dev": 2, "system": 3, "overlay.d": 2}))
        with self.assertRaisesRegex(SystemExit, "070701"):
            magisk_image.parse_newc(
                newc_dump(stock_entries(), order=list(stock_entries()), magics={"init": b"070702"}),
                SystemExit)

    def test_a_stock_archive_the_kernel_would_unpack_differently_is_refused(self) -> None:
        # The stock side of the comparison is read as a guest reads it too. A stock
        # archive that lists a file before its directory, or carries hard links, is
        # not the filesystem its entries say: a patched archive of the same entries,
        # parent first and link count one, would compare equal to it and be another
        # filesystem in the guest.
        stock = self.fixture.stock
        names = list(stock)
        first, second = "system/bin/toolbox", "first_stage_ramdisk/system/etc/fstab.uml"
        child_first = [name for name in names if name != "system/bin"]
        child_first.insert(child_first.index(first) + 1, "system/bin")
        shared = 4242
        for label, archive in (
            ("a stock file before its directory", dict(order=child_first)),
            ("stock files hard linked to each other",
             dict(inodes={first: shared, second: shared}, links={first: 2, second: 2})),
            ("a link count of two on a lone stock file", dict(links={first: 2})),
            ("one inode on two stock files", dict(inodes={first: shared, second: shared})),
            ("a CRC stock archive entry", dict(magics={first: b"070702"})),
        ):
            with self.subTest(label):
                raw = newc_dump(stock, **({"order": names} | archive), tail=196)
                image = magisk_fixture.boot_image(raw, size=len(self.fixture.stock_image))
                entries = reference_patched(stock, self.fixture.preinit, hashlib.sha1(image).hexdigest())
                header = bytearray(image[:4096])
                ramdisk = lz4_legacy(newc_dump(entries))
                struct.pack_into("<I", header, 12, len(ramdisk))
                patched = bytes(header) + ramdisk + b"\0" * (-len(ramdisk) % 4096)
                patched += b"\0" * max(0, len(image) - len(patched))
                with zipfile.ZipFile(self.apk_path) as archive_apk, self.assertRaisesRegex(SystemExit, "stock"):
                    magisk_image.verify_patched(patched, image, archive_apk, self.fixture.preinit, SystemExit)

    def test_header_and_ramdisk_departures_are_refused(self) -> None:
        good = self.patched()
        with_data = bytearray(good)
        with_data[-1] = 1
        garbled = bytearray(good)
        garbled[4096 + 100:4096 + 104] = b"ZZZZ"
        for label, image in (
            ("a header that differs beyond the ramdisk size", self.patched(os_version=0x4321)),
            ("a raw ramdisk", self.patched(compress=lambda raw: raw)),
            ("data after the ramdisk", bytes(with_data)),
            ("a ramdisk that is neither raw nor lz4", self.patched(compress=lambda raw: b"\x1f\x8b" + raw)),
            ("a garbled ramdisk", bytes(garbled)),
        ):
            with self.subTest(label), self.assertRaises(SystemExit):
                self.verify(image)
        with self.assertRaises(SystemExit):
            self.verify(good, preinit="ubdb2")
        with self.assertRaises(SystemExit):
            self.verify(self.patched(preinit="ubdb2"))

    def test_another_stock_image_does_not_verify(self) -> None:
        other = bytearray(self.fixture.stock_image)
        other[5000] ^= 1
        with zipfile.ZipFile(self.apk_path) as archive, self.assertRaises(SystemExit):
            magisk_image.verify_patched(self.patched(), bytes(other), archive, "ubdb3", SystemExit)

    def test_an_apk_with_other_binaries_does_not_verify(self) -> None:
        other = self.directory / "other.apk"
        make_apk_bytes(other, apk_members(patch_script()) | {"lib/arm64-v8a/libmagiskinit.so": b"another build"})
        missing = self.directory / "missing.apk"
        make_apk_bytes(missing, apk_members(patch_script(), omit=("lib/arm64-v8a/libinit-ld.so",)))
        for path in (other, missing):
            with zipfile.ZipFile(path) as archive, self.assertRaises(SystemExit):
                magisk_image.verify_patched(
                    self.patched(), self.fixture.stock_image, archive, "ubdb3", SystemExit)

    def test_the_preinit_partition_follows_the_layout(self) -> None:
        self.assertEqual(magisk_image.preinit_device(magisk_image.read_layout(self.fixture.layout, SystemExit), SystemExit), "ubdb3")
        other = Fixture(Path(self.enterContext(tempfile.TemporaryDirectory())), userdata=False)
        with self.assertRaises(SystemExit):
            magisk_image.preinit_device(magisk_image.read_layout(other.layout, SystemExit), SystemExit)


class PatcherTests(unittest.TestCase):
    """magisk-boot.py runs the APK's own script and tools and refuses what does not verify."""

    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.fixture = Fixture(self.directory)
        self.output = self.directory / "out/root"

    def patch(self, apk: Path, *, pin: Path | None = None) -> subprocess.CompletedProcess:
        return run_tool(
            "magisk-boot.py", "patch", "--magisk-apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--output-dir", self.output,
            "--pin", pin or self.fixture.pin(apk))

    def assert_refused(self, result: subprocess.CompletedProcess, pattern: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertRegex(result.stderr, pattern)
        self.assertFalse((self.output / "init_boot-magisk.img").exists())

    def test_the_patch_is_the_stock_image_plus_what_magisk_adds(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        result = self.patch(apk)
        self.assertEqual(result.returncode, 0, result.stderr)
        image = self.output / "init_boot-magisk.img"
        patched = image.read_bytes()
        self.assertEqual(patched, self.fixture.patched_reference())
        self.assertEqual(len(patched), len(self.fixture.stock_image))
        self.assertEqual((self.output / f"Magisk-{TEST_VERSION}.apk").read_bytes(), apk.read_bytes())
        self.assertEqual(sorted(path.name for path in self.output.iterdir()),
                         [f"Magisk-{TEST_VERSION}.apk", "init_boot-magisk.img"])
        # The verifier's own command line agrees, from the disk and from a stock file.
        verified = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", self.fixture.pin(apk))
        self.assertEqual(verified.returncode, 0, verified.stderr)
        stock = self.directory / "stock.img"
        stock.write_bytes(self.fixture.stock_image)
        by_file = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", apk, "--stock-image", stock,
            "--layout", self.fixture.layout)
        self.assertEqual(by_file.returncode, 0, by_file.stderr)
        # An image that is not the stock one's patch does not verify, nor does an APK that is not the pin.
        image.write_bytes(self.fixture.stock_image)
        refused = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout)
        self.assertNotEqual(refused.returncode, 0)
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        not_pinned = run_tool(
            "verify-magisk-boot.py", "--image", image, "--apk", other, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", self.fixture.pin(apk))
        self.assertRegex(not_pinned.stderr, "is not the pinned Magisk")

    def test_record_prints_the_manifest_root_record_of_a_producers_files(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        self.assertEqual(self.patch(apk, pin=pin).returncode, 0)
        result = run_tool(
            "magisk-boot.py", "record", "--root-dir", self.output, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", pin)
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        patched = (self.output / "init_boot-magisk.img").read_bytes()
        stock = self.fixture.stock_image
        self.assertEqual(record["magisk"]["version_name"], TEST_VERSION)
        self.assertEqual(record["apk"], {
            "path": f"root/Magisk-{TEST_VERSION}.apk",
            "sha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
            "size": apk.stat().st_size,
        })
        self.assertEqual(record["boot_image"], {
            "path": "root/init_boot-magisk.img",
            "sha256": hashlib.sha256(patched).hexdigest(),
            "size": len(patched),
            "stock_init_boot_sha256": hashlib.sha256(stock).hexdigest(),
            "stock_init_boot_sha1": hashlib.sha1(stock).hexdigest(),
        })
        # No patched image, or an APK that is not the pin, gives no record.
        (self.output / "init_boot-magisk.img").unlink()
        missing = run_tool(
            "magisk-boot.py", "record", "--root-dir", self.output, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", pin)
        self.assertNotEqual(missing.returncode, 0)
        self.assertRegex(missing.stderr, "Magisk-patched init_boot is missing")
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        wrong = self.directory / "wrong"
        wrong.mkdir()
        (wrong / f"Magisk-{TEST_VERSION}.apk").write_bytes(other.read_bytes())
        (wrong / "init_boot-magisk.img").write_bytes(patched)
        not_pinned = run_tool(
            "magisk-boot.py", "record", "--root-dir", wrong, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--pin", pin)
        self.assertRegex(not_pinned.stderr, "is not the pinned Magisk")

    def test_a_script_that_writes_the_pre_init_line_gives_the_same_image(self) -> None:
        result = self.patch(self.fixture.apk(script=patch_script(preinit=True)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.output / "init_boot-magisk.img").read_bytes(), self.fixture.patched_reference())

    def test_an_apk_that_is_not_the_pin_is_refused(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        self.assert_refused(self.patch(other, pin=pin), "is not the pinned Magisk")
        pin.write_text(pin.read_text().replace(hashlib.sha256(apk.read_bytes()).hexdigest(), "0" * 64))
        self.assert_refused(self.patch(apk, pin=pin), "is not the pinned Magisk")

    def test_each_misbehaving_patcher_is_refused(self) -> None:
        for label, kwargs, pattern in (
            ("a script that fails", {"script": patch_script(exit_status=3)}, r"boot_patch\.sh failed \(3\)"),
            ("a script that leaves /init alone", {"script": patch_script(add_init=False)},
             "did not add|magiskinit"),
            ("a script that adds no init-ld", {"script": patch_script(add_init_ld=False)}, "did not add"),
            ("a tool that does not compress the ramdisk",
             {"script": patch_script(), "options": ("--raw-ramdisk",)}, "not lz4 legacy"),
            ("a tool whose output differs between runs",
             {"script": patch_script(), "options": ("--nondeterministic",)}, "different images"),
            ("an APK without the arm64 magiskinit",
             {"script": patch_script(), "omit": ("lib/arm64-v8a/libmagiskinit.so",)},
             "has no lib/arm64-v8a/libmagiskinit.so"),
            ("an APK without the builder's magiskboot",
             {"script": patch_script(), "omit": ("lib/x86_64/libmagiskboot.so",)},
             "has no lib/x86_64/libmagiskboot.so"),
        ):
            with self.subTest(label):
                self.assert_refused(self.patch(self.fixture.apk(**kwargs)), pattern)

    def load_tool(self):
        spec = importlib.util.spec_from_file_location("magisk_boot_tool", TOOLS / "magisk-boot.py")
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        return tool

    def patch_in_process(self, tool, apk: Path, pin: Path) -> None:
        tool.patch(argparse.Namespace(
            pin=pin, magisk_apk=apk, disk=self.fixture.disk, layout=self.fixture.layout,
            output_dir=self.output))

    def test_the_apk_is_one_set_of_bytes_whatever_happens_to_the_path(self) -> None:
        # The path is checked against the pin, and then used for the extraction, the
        # execution and the delivered copy. A file that is replaced in between must
        # change none of them: the bytes checked are the bytes used.
        original = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(original)
        original_bytes = original.read_bytes()
        other = self.fixture.apk("other.apk", script=patch_script(exit_status=1))
        for label, name in (
            ("while the stock image is read, before the archive is opened", "read_partition"),
            ("after the patch, before the copy is delivered", "verify_patched"),
        ):
            with self.subTest(label):
                shutil.rmtree(self.output, ignore_errors=True)
                original.write_bytes(original_bytes)
                tool = self.load_tool()
                real = getattr(magisk_image, name)

                def swapping(*arguments, _real=real, **keywords):
                    result = _real(*arguments, **keywords)
                    original.write_bytes(other.read_bytes())
                    return result

                with mock.patch.object(magisk_image, name, swapping):
                    self.patch_in_process(tool, original, pin)
                self.assertEqual(
                    (self.output / f"Magisk-{TEST_VERSION}.apk").read_bytes(), original_bytes)
                self.assertEqual(
                    (self.output / "init_boot-magisk.img").read_bytes(),
                    self.fixture.patched_reference())

    def test_an_apk_that_is_a_link_is_refused(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        link = self.directory / "link.apk"
        link.symlink_to(apk)
        self.assert_refused(self.patch(link, pin=self.fixture.pin(apk)), "regular non-symlink file")

    def test_a_pipe_is_refused_and_never_waited_for(self) -> None:
        # open() on a pipe blocks until a writer comes, so a file must be refused by what
        # its descriptor is, and the open must not wait. The bound is what fails a
        # regression instead of hanging it.
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        pipe = self.directory / "pipe"
        os.mkfifo(pipe)
        record = self.directory / "record"
        record.mkdir()
        os.mkfifo(record / f"Magisk-{TEST_VERSION}.apk")
        (record / "init_boot-magisk.img").write_bytes(b"x")
        verify = ("verify-magisk-boot.py", "--image", apk, "--apk", apk, "--layout", self.fixture.layout,
                  "--stock-image", pipe)
        for name, arguments in {
            "the APK of the patcher": (
                "magisk-boot.py", "patch", "--magisk-apk", pipe, "--disk", self.fixture.disk,
                "--layout", self.fixture.layout, "--output-dir", self.output, "--pin", pin),
            "the APK of a record": (
                "magisk-boot.py", "record", "--root-dir", record, "--disk", self.fixture.disk,
                "--layout", self.fixture.layout, "--pin", pin),
            "the stock image of the verifier": verify,
        }.items():
            with self.subTest(name):
                try:
                    result = run_tool(*arguments, timeout=20)
                except subprocess.TimeoutExpired:
                    self.fail(f"{name} given as a pipe was waited for")
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertRegex(result.stderr, "regular non-symlink file")

    def test_a_script_that_hangs_is_stopped(self) -> None:
        spec = importlib.util.spec_from_file_location("magisk_boot_tool", TOOLS / "magisk-boot.py")
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        tool.STEP_TIMEOUT_SECONDS = 1
        apk = self.fixture.apk(script=patch_script(hang=True))
        work = Path(tempfile.mkdtemp(dir=self.directory))
        with zipfile.ZipFile(apk) as archive, self.assertRaisesRegex(SystemExit, "did not finish"):
            tool.patch_once(archive, self.fixture.stock_image, "ubdb3", work)

    def test_an_existing_file_is_never_replaced(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        self.output.mkdir(parents=True)
        (self.output / "init_boot-magisk.img").write_bytes(b"earlier")
        result = self.patch(apk)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, "already exists")
        self.assertEqual((self.output / "init_boot-magisk.img").read_bytes(), b"earlier")

    def test_a_failed_invocation_leaves_nothing_that_a_retry_refuses(self) -> None:
        # The pair is published together: a failure while the APK is written (the disk
        # full) leaves neither file, so the same command can simply be run again.
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        real_open = pathlib.Path.open

        def failing(path, mode="r", *arguments, **keywords):
            handle = real_open(path, mode, *arguments, **keywords)
            if mode != "xb" or not path.name.endswith(".apk"):
                return handle

            class Broken:
                def __enter__(self):
                    return self

                def __exit__(self, *exception):
                    handle.close()

                def write(self, data):
                    handle.write(data[:10])
                    raise OSError(errno.ENOSPC, "No space left on device")

            return Broken()

        tool = self.load_tool()
        with mock.patch.object(pathlib.Path, "open", failing), self.assertRaises(OSError):
            self.patch_in_process(tool, apk, pin)
        self.assertEqual(sorted(path.name for path in self.output.parent.iterdir()), [])
        retry = self.patch(apk, pin=pin)
        self.assertEqual(retry.returncode, 0, retry.stderr)
        self.assertEqual(
            (self.output / "init_boot-magisk.img").read_bytes(), self.fixture.patched_reference())
        self.assertEqual((self.output / f"Magisk-{TEST_VERSION}.apk").read_bytes(), apk.read_bytes())
        self.assertEqual(sorted(path.name for path in self.output.parent.iterdir()), ["root"])

    def test_an_output_directory_with_other_files_is_refused_and_an_empty_one_is_filled(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        self.output.mkdir(parents=True)
        (self.output / "unrelated").write_bytes(b"mine")
        refused = self.patch(apk, pin=pin)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual([path.name for path in self.output.iterdir()], ["unrelated"])
        self.assertEqual(sorted(path.name for path in self.output.parent.iterdir()), ["root"])
        (self.output / "unrelated").unlink()
        self.assertEqual(self.patch(apk, pin=pin).returncode, 0)
        self.assertEqual(
            sorted(path.name for path in self.output.iterdir()),
            [f"Magisk-{TEST_VERSION}.apk", "init_boot-magisk.img"])

    def test_the_patcher_does_not_run_as_root(self) -> None:
        # Magisk's script unmounts and moves system directories when it aborts outside
        # a booted system, which is harmless for a build user and not for uid 0.
        apk = self.fixture.apk(script=patch_script())
        real = self.directory / "uid_map"
        real.write_text("         0          0 4294967295\n")
        tool = self.load_tool()
        tool.UID_MAP = str(real)
        with mock.patch.object(os, "geteuid", lambda: 0), \
                self.assertRaisesRegex(SystemExit, "must not run as root"):
            self.patch_in_process(tool, apk, self.fixture.pin(apk))
        self.assertFalse(self.output.exists())

    def test_a_root_of_a_user_namespace_is_not_root(self) -> None:
        # The build host runs the producer's image steps in a user namespace that maps
        # the build user to uid 0. That uid is root of the namespace only: nothing the
        # script unmounts or moves there is the host's. Real root, and a system that
        # does not say, stay refused.
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        tool = self.load_tool()
        namespace = self.directory / "uid_map-namespace"
        namespace.write_text("         0       1000          1\n")
        real = self.directory / "uid_map-real"
        real.write_text("         0          0 4294967295\n")
        for label, uid_map, refused in (
            ("a user namespace", namespace, False),
            ("real root", real, True),
            ("no uid_map", self.directory / "uid_map-missing", True),
        ):
            with self.subTest(label), mock.patch.object(os, "geteuid", lambda: 0):
                tool.UID_MAP = str(uid_map)
                shutil.rmtree(self.output, ignore_errors=True)
                if refused:
                    with self.assertRaisesRegex(SystemExit, "must not run as root"):
                        self.patch_in_process(tool, apk, pin)
                else:
                    self.patch_in_process(tool, apk, pin)
                    self.assertEqual(
                        (self.output / "init_boot-magisk.img").read_bytes(), self.fixture.patched_reference())

    NOISE = b'n=0; while [ $n -lt 60 ]; do n=$((n+1)); echo "progress $n"; done\n./magiskboot unpack'

    def run_with_log_directory(self, apk: Path, pin: Path, kept: Path | str):
        shutil.rmtree(self.output, ignore_errors=True)
        return run_tool(
            "magisk-boot.py", "patch", "--magisk-apk", apk, "--disk", self.fixture.disk,
            "--layout", self.fixture.layout, "--output-dir", self.output, "--pin", pin,
            env={"MAGISK_BOOT_LOG_DIR": kept})

    def test_the_steps_keep_their_whole_output_where_the_caller_asks(self) -> None:
        # A refusal repeats the last 40 lines of a failing step and a step that passes
        # says nothing, which is too little for a first run on a new machine. A caller
        # that names a directory in the environment (so that a script that runs the
        # patcher need not pass it on) gets every step's whole output and every image
        # the script made, for a run that passes and for one that fails.
        noisy = patch_script().replace(b"./magiskboot unpack", self.NOISE)
        apk = self.fixture.apk(script=noisy)
        kept = self.directory / "kept"
        result = self.run_with_log_directory(apk, self.fixture.pin(apk), kept)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = sorted(path.name for path in kept.iterdir())
        for run in ("run1", "run2"):
            self.assertIn(f"{run}-boot_patch.sh.log", names)
            self.assertIn(f"{run}-new-boot.img", names)
        log = (kept / "run1-boot_patch.sh.log").read_text()
        self.assertIn("progress 1\n", log)
        self.assertIn("progress 60\n", log)
        self.assertRegex(log, r"exit status 0\n$")
        self.assertEqual(
            (kept / "run1-new-boot.img").read_bytes(), (kept / "run2-new-boot.img").read_bytes())
        # A failing step keeps its whole output and the refusal still says only the tail.
        failing = patch_script(exit_status=3).replace(b"./magiskboot unpack", self.NOISE)
        bad = self.fixture.apk("bad.apk", script=failing)
        kept_bad = self.directory / "kept-bad"
        refused = self.run_with_log_directory(bad, self.fixture.pin(bad, "bad.env"), kept_bad)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("progress 60", refused.stderr)
        self.assertNotIn("progress 1\n", refused.stderr)
        self.assertIn("progress 1\n", (kept_bad / "run1-boot_patch.sh.log").read_text())
        self.assertRegex((kept_bad / "run1-boot_patch.sh.log").read_text(), r"exit status 3\n$")
        self.assertTrue((kept_bad / "run1-new-boot.img").is_file())
        # Without the variable nothing is written, and a relative path is refused first.
        plain = self.run_with_log_directory(apk, self.fixture.pin(apk), "")
        self.assertEqual(plain.returncode, 0, plain.stderr)
        relative = self.run_with_log_directory(apk, self.fixture.pin(apk), "kept-relative")
        self.assertNotEqual(relative.returncode, 0)
        self.assertRegex(relative.stderr, "MAGISK_BOOT_LOG_DIR must be an absolute")
        self.assertFalse(self.output.exists())

    def test_the_script_is_told_it_runs_from_a_booted_system(self) -> None:
        # util_functions.sh decides from `ps | grep zygote` whether it runs from a booted
        # system, and a builder has no zygote: the script then writes its messages to a
        # recovery's output descriptor, which is not there, and its abort unmounts system
        # directories. Said as BOOTMODE=true, the messages go to the output the log keeps,
        # and an abort does not run that cleanup (it still removes the script's own TMPDIR,
        # /dev/tmp, which the patcher does not guard). A caller's own BOOTMODE is not
        # passed on.
        shown = patch_script().replace(b"./magiskboot unpack", b'echo "BOOTMODE=$BOOTMODE"\n./magiskboot unpack')
        apk = self.fixture.apk(script=shown)
        kept = self.directory / "kept-bootmode"
        shutil.rmtree(self.output, ignore_errors=True)
        result = subprocess.run(
            [sys.executable, "-B", str(TOOLS / "magisk-boot.py"), "patch", "--magisk-apk", str(apk),
             "--disk", str(self.fixture.disk), "--layout", str(self.fixture.layout),
             "--output-dir", str(self.output), "--pin", str(self.fixture.pin(apk))],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120, check=False,
            env={**os.environ, "MAGISK_BOOT_LOG_DIR": str(kept), "BOOTMODE": "false"})
        self.assertEqual(result.returncode, 0, result.stderr)
        for run in ("run1", "run2"):
            self.assertIn("BOOTMODE=true\n", (kept / f"{run}-boot_patch.sh.log").read_text())

    def test_a_log_file_is_never_opened_through_a_link(self) -> None:
        # The log directory is the caller's, and a link in it must not carry a write to
        # another file: the logs are opened without following one, so a run that finds a
        # link where a log goes fails (logging on only) and changes nothing the link names.
        # A log directory that is itself a link is refused first.
        apk = self.fixture.apk(script=patch_script())
        pin = self.fixture.pin(apk)
        victim = self.directory / "victim.txt"
        victim.write_text("untouched\n")
        kept = self.directory / "kept-linked"
        kept.mkdir()
        (kept / "run1-boot_patch.sh.log").symlink_to(victim)
        refused = self.run_with_log_directory(apk, pin, kept)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(victim.read_text(), "untouched\n")
        self.assertFalse(self.output.exists())
        directory_link = self.directory / "kept-link-to-a-directory"
        directory_link.symlink_to(self.directory)
        refused = self.run_with_log_directory(apk, pin, directory_link)
        self.assertNotEqual(refused.returncode, 0)
        self.assertRegex(refused.stderr, "MAGISK_BOOT_LOG_DIR must be a real directory")
        self.assertEqual(victim.read_text(), "untouched\n")
        self.assertFalse(any(path.suffix == ".log" for path in self.directory.iterdir()))

    def test_runs_that_differ_say_where_and_keep_both_images(self) -> None:
        # The two runs of the patcher must agree. When they do not, the refusal names
        # the first byte that differs and both images are kept for a comparison.
        varying = patch_script().replace(
            b"cp -af ramdisk.cpio ramdisk.cpio.orig", b"cp -af ramdisk.cpio ramdisk.cpio.orig\necho $$ > pid"
        ).replace(
            b'"mkdir 0750 overlay.d/sbin" \\', b'"mkdir 0750 overlay.d/sbin" \\\n"add 0644 overlay.d/sbin/pid pid" \\')
        apk = self.fixture.apk(script=varying)
        kept = self.directory / "kept-differ"
        result = self.run_with_log_directory(apk, self.fixture.pin(apk), kept)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, r"gave different images \(run 1: [0-9a-f]{64}, run 2: [0-9a-f]{64}, "
                                        r"first difference at byte \d+\)")
        self.assertTrue((kept / "run1-new-boot.img").is_file())
        self.assertTrue((kept / "run2-new-boot.img").is_file())

    def test_the_tail_of_a_failing_step_shows_the_scripts_own_words(self) -> None:
        script = patch_script(exit_status=3).replace(
            b"./magiskboot unpack", b"for n in $(seq 1 30); do echo \"progress $n\"; done\n./magiskboot unpack")
        result = self.patch(self.fixture.apk(script=script))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("progress 1\n", result.stderr)
        self.assertIn("progress 30", result.stderr)

    def test_an_output_directory_that_is_a_link_is_refused(self) -> None:
        apk = self.fixture.apk(script=patch_script())
        target = self.directory / "target"
        target.mkdir()
        self.output.parent.mkdir(parents=True)
        self.output.symlink_to(target)
        result = self.patch(apk)
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, "real directory")
        self.assertEqual(list(target.iterdir()), [])

    def test_a_layout_without_userdata_is_refused(self) -> None:
        fixture = Fixture(Path(self.enterContext(tempfile.TemporaryDirectory())), userdata=False)
        apk = fixture.apk(script=patch_script())
        result = run_tool(
            "magisk-boot.py", "patch", "--magisk-apk", apk, "--disk", fixture.disk,
            "--layout", fixture.layout, "--output-dir", self.output, "--pin", fixture.pin(apk))
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stderr, "no single userdata partition")


class AssembleOptionTests(unittest.TestCase):
    """assemble-image.sh takes --magisk-apk, checks it before any work, and says what it makes."""

    def assemble(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", str(ROOT / "android/lineage-24/build/assemble-image.sh"), *arguments],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60, check=False)

    def test_the_usage_names_the_option_and_the_root_files(self) -> None:
        result = self.assemble("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--magisk-apk", result.stdout)
        self.assertIn("init_boot-magisk.img", result.stdout)

    def test_the_option_needs_an_absolute_path(self) -> None:
        required = ["--target-files", "/t.zip", "--kernel-artifact", "/k", "--mkbootfs", "/m",
                    "--mkbootimg", "/i", "--output-dir", "/o"]
        for label, arguments, pattern in (
            ("a relative path", ["--magisk-apk", "Magisk.apk"], "Magisk APK must be an absolute local path"),
            ("no path", ["--magisk-apk"], "--magisk-apk requires a path"),
        ):
            with self.subTest(label):
                result = self.assemble(*required, *arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(pattern, result.stderr)


def main() -> None:
    global ROOT, TOOLS, contract_checks, magisk_image, release_inputs
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    arguments, rest = parser.parse_known_args()
    ROOT = arguments.source_root.resolve()
    TOOLS = ROOT / "android/lineage-24/tools"
    magisk_fixture.configure(ROOT)
    import contract_checks as checks  # noqa: PLC0415
    import release_inputs as inputs  # noqa: PLC0415

    contract_checks, magisk_image, release_inputs = checks, magisk_fixture.magisk_image, inputs
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
