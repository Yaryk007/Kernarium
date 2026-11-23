#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify a Magisk-patched init_boot against its stock image and the Magisk APK.

Either name the files (--image, --apk, --layout for the data disk's pre-init
partition, and the stock image as --stock-image or as the init_boot partition of
--disk), or name a release directory
(--release): its manifest's root record, both root files and its own init_boot
partition. --pin also requires the pinned Magisk release (metadata/magisk.env).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

import magisk_image


def error(message: str) -> SystemExit:
    return SystemExit(f"Magisk boot image verifier: {message}")


def regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise error(f"{label} must be a regular non-symlink file: {path}")
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=Path, help="a release directory with root/ files")
    parser.add_argument("--image", type=Path, help="the patched init_boot image")
    parser.add_argument("--apk", type=Path, help="the official Magisk APK")
    parser.add_argument("--stock-image", type=Path, help="the stock init_boot image")
    parser.add_argument("--disk", type=Path, help="the system disk holding the stock init_boot")
    parser.add_argument("--layout", type=Path, help="the release's partition layout")
    parser.add_argument(
        "--pin", nargs="?", type=Path, const=magisk_image.PIN_PATH,
        help="require the pinned Magisk release (default: metadata/magisk.env)",
    )
    args = parser.parse_args()

    pin = magisk_image.read_pin(args.pin, error) if args.pin is not None else None
    if args.release is not None:
        if any(value is not None for value in (
                args.image, args.apk, args.stock_image, args.disk, args.layout)):
            raise error("--release cannot be combined with file arguments")
        release = args.release.resolve()
        if not release.is_dir():
            raise error(f"release directory is missing: {release}")
        record = magisk_image.verify_release_root(release, error, pin)
        print(
            f"verified Magisk root files of release {release.name}: "
            f"{record['magisk']['name']} {record['magisk']['version_name']}"
        )
        return

    if args.image is None or args.apk is None or args.layout is None:
        raise error("--image, --apk and --layout are required without --release")
    if (args.stock_image is None) == (args.disk is None):
        raise error("name the stock image as --stock-image or as the init_boot partition of --disk")
    # Every file is read once, with a bound, and what is checked is those bytes: no
    # path is opened a second time, so a file that changes meanwhile changes nothing.
    layout = magisk_image.read_layout(args.layout, error)
    if pin is not None:
        apk = magisk_image.read_pinned_apk(args.apk, pin, error)
    else:
        apk = magisk_image.read_file(args.apk, magisk_image.MAX_APK_FILE, "the Magisk APK", error)
    if args.stock_image is not None:
        stock = magisk_image.read_file(
            args.stock_image, magisk_image.MAX_BOOT_IMAGE, "the stock image", error)
    else:
        stock = magisk_image.read_partition(
            regular_file(args.disk, "system disk"), layout, "init_boot", error)
    patched = magisk_image.read_file(args.image, len(stock), "the patched image", error)
    with magisk_image.open_apk(apk, error) as archive:
        magisk_image.verify_patched(
            patched, stock, archive, magisk_image.preinit_device(layout, error), error)
    print(f"verified Magisk-patched init_boot: {args.image}")


if __name__ == "__main__":
    main()
