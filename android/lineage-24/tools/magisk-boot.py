#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make a release's Magisk-patched init_boot from the pinned official APK.

    magisk-boot.py patch --magisk-apk APK --disk system.img \\
        --layout partition-layout.json --output-dir DIR

runs on the x86-64 Linux builder, after the system disk is assembled. It reads
the APK, a local file, once, refuses it unless it is the pinned release
(metadata/magisk.env), and uses those same bytes to extract from, to run and to
deliver: the path is never reopened. It reads the stock init_boot partition from
the disk and runs Magisk's own boot_patch.sh from the APK twice, in private
directories, with the fixed patch inputs: Magisk's x86-64 magiskboot patches, and
only the APK's arm64 magiskinit, magisk and init-ld go into the ramdisk. The two
results must be identical, and the result must verify as the stock image plus exactly what Magisk
adds (magisk_image.py). DIR, which must not exist or be empty, then holds
init_boot-magisk.img and the unmodified APK under its official name, which is how a
release carries them as its root/ files; the pair appears together or not at all.
It refuses to run as the machine's root and accepts the root of a user namespace.
With MAGISK_BOOT_LOG_DIR set to an absolute directory, every step's whole output and
every image the script made are kept there, whether the run passes or fails.

The script cannot name the data disk's userdata partition, which it asks the
guest's own magisk for; the tool therefore writes the missing PREINITDEVICE line
into .backup/.magisk itself when the script did not, with magiskboot, and
repacks.

    magisk-boot.py record --root-dir DIR --disk system.img \\
        --layout partition-layout.json

prints the manifest's `root` record for the files of such a DIR, for a producer
that writes its own manifest (publish-release.py builds the record itself). The
record is the producer's statement; verify-magisk-boot.py --release checks the
files against it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import magisk_image  # noqa: E402

RUNS = 2
STEP_TIMEOUT_SECONDS = 300
# How much of a failing step's own output the refusal repeats.
TAIL_LINES = 40
UID_MAP = "/proc/self/uid_map"
# A directory (absolute) for the whole output of every step and the images the script
# made, for a run that passes and for one that fails.
LOG_DIRECTORY_VARIABLE = "MAGISK_BOOT_LOG_DIR"
TOOL_PATH = "/usr/bin:/bin"
PATCHED_NAME = Path(magisk_image.PATCHED_PATH).name
NEW_IMAGE = "new-boot.img"
STOCK_IMAGE = "init_boot.img"
SHELL = "/bin/sh"

# The files the script expects beside it, and where each comes from in the APK.
STAGED = (
    ("boot_patch.sh", magisk_image.SCRIPT_MEMBER, 0o755),
    ("util_functions.sh", magisk_image.UTILITY_MEMBER, 0o755),
    ("magiskboot", magisk_image.MAGISKBOOT_MEMBER, 0o755),
    ("magiskinit", magisk_image.MAGISKINIT_MEMBER, 0o755),
    ("magisk", magisk_image.MAGISK_MEMBER, 0o755),
    ("init-ld", magisk_image.INIT_LD_MEMBER, 0o644),
)


def error(message: str) -> SystemExit:
    return SystemExit(f"Magisk boot patcher: {message}")


def root_of_the_system() -> bool:
    """Whether euid 0 is root of the machine: a user namespace maps it to a user."""
    if os.geteuid() != 0:
        return False
    try:
        lines = Path(UID_MAP).read_text().splitlines()
    except OSError:
        return True
    for line in lines:
        fields = line.split()
        try:
            inside, outside, count = (int(field) for field in fields)
        except ValueError:
            return True
        if len(fields) == 3 and inside <= 0 < inside + count:
            return outside == 0
    return True


class Keeper:
    """Where one run of the patcher keeps its steps' output and its images."""

    def __init__(self, directory: Path | None, run: int) -> None:
        self.directory = directory
        self.prefix = f"run{run}-"

    def _create(self, name: str):
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC
        return os.fdopen(os.open(self.directory / (self.prefix + name), flags, 0o644), "wb")

    def step(self, name: str, arguments: list[str], output: str, outcome: str) -> None:
        if self.directory is None:
            return
        text = f"$ {' '.join(arguments)}\n{output}"
        if text and not text.endswith("\n"):
            text += "\n"
        with self._create(f"{name}.log") as handle:
            handle.write(f"{text}{outcome}\n".encode(errors="replace"))

    def image(self, name: str, path: Path) -> None:
        if self.directory is None or path.is_symlink() or not path.is_file():
            return
        with path.open("rb") as source, self._create(name) as handle:
            shutil.copyfileobj(source, handle)


def run_step(
    arguments: list[str], work: Path, env: dict[str, str], label: str,
    keep: Keeper | None = None, name: str = "step",
) -> str:
    """Run one step in its own process group, which a timeout kills whole."""
    try:
        process = subprocess.Popen(
            arguments,
            cwd=work,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            start_new_session=True,
        )
    except OSError as failure:
        raise error(f"cannot run {label}: {failure}") from failure
    try:
        output, _ = process.communicate(timeout=STEP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        if keep is not None:
            keep.step(name, arguments, output, f"killed after {STEP_TIMEOUT_SECONDS} s")
        raise error(f"{label} did not finish in {STEP_TIMEOUT_SECONDS} s") from None
    if keep is not None:
        keep.step(name, arguments, output, f"exit status {process.returncode}")
    if process.returncode != 0:
        tail = "\n".join(output.splitlines()[-TAIL_LINES:])
        raise error(f"{label} failed ({process.returncode}):\n{tail}")
    return output


def patch_once(archive, stock: bytes, preinit: str, work: Path, keep: Keeper | None = None) -> bytes:
    """One run of Magisk's patcher in the private directory `work`."""
    try:
        return patch_in(archive, stock, preinit, work, keep)
    finally:
        if keep is not None:
            keep.image(NEW_IMAGE, work / NEW_IMAGE)
            keep.image("final-boot.img", work / "final-boot.img")


def patch_in(archive, stock: bytes, preinit: str, work: Path, keep: Keeper | None) -> bytes:
    for name, member, mode in STAGED:
        path = work / name
        path.write_bytes(magisk_image.apk_member(archive, member, error))
        path.chmod(mode)
    (work / STOCK_IMAGE).write_bytes(stock)
    env = {
        "PATH": TOOL_PATH,
        "HOME": str(work),
        "LC_ALL": "C",
        # util_functions.sh looks for a zygote to tell a booted system from a recovery:
        # on a builder it would take this for a recovery, write its messages to an output
        # descriptor that is not there, and unmount system directories when it aborts.
        "BOOTMODE": "true",
        **dict(magisk_image.PATCH_FLAGS),
        "PREINITDEVICE": preinit,
    }
    run_step(
        [SHELL, "boot_patch.sh", STOCK_IMAGE], work, env, "Magisk's boot_patch.sh",
        keep, "boot_patch.sh",
    )
    produced = work / NEW_IMAGE
    if produced.is_symlink() or not produced.is_file():
        raise error(f"boot_patch.sh produced no {NEW_IMAGE}")
    image = magisk_image.read_file(produced, len(stock), f"the {NEW_IMAGE} of boot_patch.sh", error)

    # The script reads the data disk's partition from the guest's own magisk,
    # which cannot run here: complete .backup/.magisk when it left the line out.
    _, ramdisk = magisk_image.boot_ramdisk(image, "patched init_boot", error)
    entries = magisk_image.parse_newc(
        magisk_image.decode_ramdisk(ramdisk, "patched", error), error
    )
    config = entries.get(magisk_image.CONFIG_ENTRY)
    if config is None:
        raise error(f"the patched ramdisk has no {magisk_image.CONFIG_ENTRY}")
    stock_sha1 = hashlib.sha1(stock).hexdigest()
    wanted = magisk_image.backup_config(preinit, stock_sha1)
    if config.payload == wanted:
        return image
    if config.payload != magisk_image.backup_config(preinit, stock_sha1, with_preinit=False):
        raise error(
            f"the patcher wrote an unexpected {magisk_image.CONFIG_ENTRY}: it has neither "
            "the fixed patch inputs with the pre-init partition nor without it"
        )
    (work / "config.new").write_bytes(wanted)
    run_step(
        [str(work / "magiskboot"), "cpio", "ramdisk.cpio",
         f"add 000 {magisk_image.CONFIG_ENTRY} config.new"],
        work, env, "magiskboot cpio", keep, "magiskboot-cpio",
    )
    run_step(
        [str(work / "magiskboot"), "repack", STOCK_IMAGE, "final-boot.img"],
        work, env, "magiskboot repack", keep, "magiskboot-repack",
    )
    final = work / "final-boot.img"
    if final.is_symlink() or not final.is_file():
        raise error("magiskboot repack produced no image")
    return magisk_image.read_file(final, len(stock), "the image magiskboot repacked", error)


def first_difference(left: bytes, right: bytes) -> int:
    for offset, (one, other) in enumerate(zip(left, right)):
        if one != other:
            return offset
    return min(len(left), len(right))


def log_directory() -> Path | None:
    value = os.environ.get(LOG_DIRECTORY_VARIABLE)
    if not value:
        return None
    path = Path(value)
    if not path.is_absolute():
        raise error(f"{LOG_DIRECTORY_VARIABLE} must be an absolute local path")
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise error(f"{LOG_DIRECTORY_VARIABLE} must be a real directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_new(path: Path, data: bytes) -> None:
    """Write a file that must not exist, and put it on disk."""
    with path.open("xb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def patch(arguments: argparse.Namespace) -> None:
    if root_of_the_system():
        # Magisk's script unmounts and moves system directories when it aborts
        # outside a booted system. In a user namespace those are the namespace's.
        raise error("the patcher must not run as root: run it as the build user")
    logs = log_directory()
    pin = magisk_image.read_pin(arguments.pin, error)
    apk = arguments.magisk_apk
    if not apk.is_absolute():
        raise error("--magisk-apk must be an absolute local path")
    snapshot = magisk_image.read_pinned_apk(apk, pin, error)
    for label, path in (("--disk", arguments.disk), ("--layout", arguments.layout)):
        if path.is_symlink() or not path.is_file():
            raise error(f"{label} must be a regular non-symlink file")
    output = arguments.output_dir
    if not output.is_absolute():
        raise error("--output-dir must be an absolute local path")
    if output.is_symlink() or (output.exists() and not output.is_dir()):
        raise error(f"--output-dir must be a real directory, not a link or a file: {output}")
    patched_path = output / PATCHED_NAME
    apk_copy = output / Path(magisk_image.apk_path(pin.version)).name
    for path in (patched_path, apk_copy):
        if path.exists() or path.is_symlink():
            raise error(f"{path.name} already exists: an existing file is never replaced")

    layout = magisk_image.read_layout(arguments.layout, error)
    stock = magisk_image.read_partition(arguments.disk, layout, "init_boot", error)
    magisk_image.boot_ramdisk(stock, "the release's init_boot", error)
    preinit = magisk_image.preinit_device(layout, error)
    results: list[bytes] = []
    with magisk_image.open_apk(snapshot, error) as archive:
        for run in range(1, RUNS + 1):
            work = Path(tempfile.mkdtemp(prefix=".magisk-boot."))
            try:
                results.append(patch_once(archive, stock, preinit, work, Keeper(logs, run)))
            finally:
                shutil.rmtree(work, ignore_errors=True)
        for number, result in enumerate(results[1:], 2):
            if result != results[0]:
                raise error(
                    f"{RUNS} runs of the patcher on the same inputs gave different images "
                    f"(run 1: {magisk_image.sha256_bytes(results[0])}, run {number}: "
                    f"{magisk_image.sha256_bytes(result)}, first difference at byte "
                    f"{first_difference(results[0], result)}): the result must be a function "
                    "of the stock image, the APK and the flags"
                )
        magisk_image.verify_patched(results[0], stock, archive, preinit, error)

    # Both files are written in a private directory beside the output and published
    # together by renaming it: a failure on the way leaves nothing behind, so the
    # same command can be run again, and an existing directory must be empty.
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".magisk-root.", dir=output.parent))
    try:
        write_new(staging / PATCHED_NAME, results[0])
        write_new(staging / apk_copy.name, snapshot)
        for name in (PATCHED_NAME, apk_copy.name):
            os.chmod(staging / name, 0o644)
        os.chmod(staging, 0o755)
        try:
            os.rename(staging, output)
        except OSError as failure:
            raise error(
                f"cannot publish the root files to {output}, which must not exist or must be "
                f"an empty directory: {failure}"
            ) from failure
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    print(
        f"made the Magisk-patched init_boot of {pin.name} {pin.version}: "
        f"{patched_path} ({len(results[0])} bytes, SHA-256 {magisk_image.sha256_bytes(results[0])})"
    )


def record(arguments: argparse.Namespace) -> None:
    import release_common  # the one place the record is built

    for label, path in (("--disk", arguments.disk), ("--layout", arguments.layout)):
        if path.is_symlink() or not path.is_file():
            raise error(f"{label} must be a regular non-symlink file")
    if not arguments.root_dir.is_absolute() or not arguments.root_dir.is_dir():
        raise error("--root-dir must be an absolute local directory")
    document = release_common.root_record(
        arguments.root_dir, arguments.disk, arguments.layout, arguments.pin)
    print(json.dumps(document, indent=2, sort_keys=True))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    patch_parser = commands.add_parser(
        "patch", help="patch the release's init_boot with the pinned Magisk release"
    )
    patch_parser.add_argument("--magisk-apk", type=Path, required=True)
    patch_parser.add_argument("--disk", type=Path, required=True, help="the release's system.img")
    patch_parser.add_argument("--layout", type=Path, required=True, help="its partition-layout.json")
    patch_parser.add_argument("--output-dir", type=Path, required=True)
    patch_parser.add_argument("--pin", type=Path, default=magisk_image.PIN_PATH, help=argparse.SUPPRESS)
    record_parser = commands.add_parser(
        "record", help="print the manifest's root record for a producer's root/ directory"
    )
    record_parser.add_argument("--root-dir", type=Path, required=True)
    record_parser.add_argument("--disk", type=Path, required=True, help="the release's system.img")
    record_parser.add_argument("--layout", type=Path, required=True, help="its partition-layout.json")
    record_parser.add_argument("--pin", type=Path, default=None, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    (patch if arguments.command == "patch" else record)(arguments)


if __name__ == "__main__":
    main()
