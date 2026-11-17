#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds a Google apps edition of an Andarium release's system disk: the
# release's product and system_ext EROFS partitions, unpacked with their
# owners and SELinux labels, take a MindTheGapps package's files and are
# packed again; ubd-disk.py assembles the disk from them and the release's
# other partitions. The result is one GPT disk that Andarium's Home ›
# Android system › Import takes as it is (SystemImageImport).
#
# Needs root (labels are security.* xattrs), erofs-utils with zstd, unzip and
# python3.
#
# build-gapps-system.sh RELEASE_TAR GAPPS_ZIP OUTPUT_DIR
set -euo pipefail

release_tar=$(realpath "$1")
gapps_zip=$(realpath "$2")
output=$(realpath -m "$3")
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
policy="$repo/android/lineage-24/support/image/disk-policy.json"
ubd_disk="$repo/android/lineage-24/tools/ubd-disk.py"
work=$(mktemp -d)
trap 'sudo rm -rf "$work"' EXIT

mkdir -p "$work/release" "$work/images" "$work/gapps" "$output"
tar -xf "$release_tar" -C "$work/release"
disk=$(find "$work/release" -path '*guest/system.img' | head -n 1)
layout=$(find "$work/release" -path '*guest/partition-layout.json' | head -n 1)
[[ -f "$disk" && -f "$layout" ]] || { echo "the release has no guest/system.img and layout" >&2; exit 1; }

# Each partition's image, exactly the bytes its layout records.
python3 -I - "$disk" "$layout" "$work/images" <<'PY'
import json, sys
disk, layout, out = sys.argv[1:]
with open(layout) as f:
    parts = json.load(f)["system_disk"]["partitions"]
with open(disk, "rb") as source:
    for p in parts:
        source.seek(p["start_bytes"])
        left = p["image_bytes"]
        with open(f"{out}/{p['name']}.img", "wb") as target:
            while left:
                chunk = source.read(min(left, 8 << 20))
                if not chunk:
                    sys.exit(f"{p['name']} ends early")
                target.write(chunk)
                left -= len(chunk)
        print(f"extracted {p['name']}: {p['image_bytes']} bytes")
PY

unzip -q "$gapps_zip" 'system/product/*' 'system/system_ext/*' -d "$work/gapps"

for partition in product system_ext; do
    root="$work/root-$partition"
    sudo fsck.erofs --extract="$root" --preserve --xattrs "$work/images/$partition.img"
    added="$work/added-$partition.txt"
    (CDPATH= cd -- "$work/gapps/system/$partition" && find . -mindepth 1 | sort) > "$added"
    sudo cp -a --no-preserve=ownership "$work/gapps/system/$partition/." "$root/"
    # Android's own files are root's with these modes; product and system_ext
    # files are system_file to SELinux.
    while IFS= read -r path; do
        target="$root/${path#./}"
        sudo chown -h 0:0 "$target"
        if [[ -d "$target" ]]; then sudo chmod 0755 "$target"; else sudo chmod 0644 "$target"; fi
        sudo setfattr -h -n security.selinux -v 'u:object_r:system_file:s0' "$target"
    done < "$added"
    echo "added $(wc -l < "$added") paths to $partition"
    rm -f "$work/images/$partition.img"
    sudo mkfs.erofs -zzstd,level=15 -C16384 -T0 "$work/images/$partition.img" "$root"
    sudo chown "$(id -u):$(id -g)" "$work/images/$partition.img"
    sudo fsck.erofs "$work/images/$partition.img"
done

python3 -I "$ubd_disk" assemble --policy "$policy" --images-dir "$work/images" --output-dir "$work/disk"
python3 -I "$ubd_disk" verify --layout "$work/disk/partition-layout.json" --disk "$work/disk/system.img"
xz -T1 -3 -c "$work/disk/system.img" > "$output/andarium-gapps-system.img.xz"
(CDPATH= cd -- "$output" && sha256sum andarium-gapps-system.img.xz > andarium-gapps-system.img.xz.sha256)
ls -l "$output"
