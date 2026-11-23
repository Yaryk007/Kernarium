#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify the guest kernel's vDSO in an Android guest.

The guest kernel gives every arm64 task a vDSO whose entries are plain system
calls (the arm64 task interface paragraph of kernel/common/README.md), and
bionic, which looks the entries up through AT_SYSINFO_EHDR, calls them.  This
runner boots the release on a fresh data disk with the macOS launcher
(tools/boot_android.py) and runs the probes the Linux guests run
(tests/integration/kernel/vdso_probe.cpp, vdso_libc_probe.cpp) as the shell
user:

- the vDSO probe uses no C library.  It checks the auxiliary vector, the ELF
  object behind AT_SYSINFO_EHDR, /proc/self/maps, both hash tables, the
  unwind tables, the agreement of each entry with its system call, a fork and
  an exec, and what a task can do to its own vDSO.  Android's configuration
  seals the system mappings, so it runs with --seal=sealed: the mapping must
  carry the sealed flag and refuse a move, an unmap, a cover and an mprotect;
- the library probe links against the guest's own libc.so, so it runs bionic's
  clock_gettime, gettimeofday and time.  It checks their values and, by tracing
  a child, that the system call each makes is made from the [vdso] mapping,
  which shows that bionic found the entries.

Both are built here, with the host's clang++ and ld.lld, for aarch64
(``--clang`` and ``--lld``, or on PATH).  The runner is opt-in: it needs the
macos-android17 executable, an installed release and an adb client.  Every wait
has a deadline, the guest is stopped through its own process on any exit, and
it is reached through the shared adb server, always named with `-s`.
usage: run_android_vdso.py --emulator uml-darwin-android17 [--release DIR]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
KERNEL_TESTS = REPOSITORY / "tests" / "integration" / "kernel"

SPEC = importlib.util.spec_from_file_location(
    "run_android_video_suspend", HERE / "run_android_video_suspend.py")
assert SPEC is not None and SPEC.loader is not None
base = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = base
SPEC.loader.exec_module(base)
boot_android = base.boot_android
Failure = base.Failure

GUEST_DIRECTORY = "/data/local/tmp"
RUN_SECONDS = 150
COMPILE_SECONDS = 240
FREESTANDING = ["--target=aarch64-linux-gnu", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
                "-fno-stack-protector", "-fno-exceptions", "-fno-rtti", "-fno-pic"]
BIONIC = ["--target=aarch64-linux-android", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
          "-fno-rtti", "-fno-stack-protector", "-fPIE", "-ffreestanding"]


def run_tool(command: list[str], what: str) -> None:
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=COMPILE_SECONDS, check=False)
    if result.returncode != 0:
        raise Failure(f"{what} failed ({result.returncode}): {result.stdout[-1500:]}")


def build_freestanding(clang: str, lld: str, scratch: pathlib.Path) -> pathlib.Path:
    """The vDSO probe: static, no C library."""
    obj = scratch / "vdso_probe.o"
    out = scratch / "vdso_probe"
    run_tool([clang, *FREESTANDING, "-c", str(KERNEL_TESTS / "vdso_probe.cpp"), "-o", str(obj)], "compiling vdso_probe")
    run_tool([lld, "-static", "-nostdlib", "-o", str(out), str(obj)], "linking vdso_probe")
    return out


def build_bionic(clang: str, lld: str, scratch: pathlib.Path) -> pathlib.Path:
    """The library probe: a PIE that bionic's linker loads, linked against an empty stand-in of libc.so that
    defines the names the probe imports (the host has no copy of the guest's libc) and started by its own
    entry, as the host has no C runtime start-up files for it."""
    obj = scratch / "vdso_libc_probe.o"
    out = scratch / "vdso_libc_probe"
    run_tool([clang, *BIONIC, "-DVDSO_PROBE_OWN_START", "-c", str(KERNEL_TESTS / "vdso_libc_probe.cpp"), "-o", str(obj)],
             "compiling vdso_libc_probe")
    # The names the probe imports are its extern "C" declarations.
    source = (KERNEL_TESTS / "vdso_libc_probe.cpp").read_text()
    declarations = source[source.index('extern "C" {'):].split("\n}", 1)[0]
    names = sorted(set(re.findall(r"(\w+)\(", declarations)))
    stub = scratch / "stub.c"
    stub.write_text("".join(f"void {name}(void) {{}}\n" for name in names))
    stub_object = scratch / "stub.o"
    run_tool([clang, "--target=aarch64-linux-android", "-fPIC", "-x", "c", "-c", str(stub), "-o", str(stub_object)],
             "compiling the libc stand-in")
    stand_in = scratch / "libc.so"
    run_tool([lld, "-shared", "-soname", "libc.so", "-o", str(stand_in), str(stub_object)],
             "linking the libc stand-in")
    run_tool([lld, "-pie", "--dynamic-linker=/system/bin/linker64", "-z", "now", "-o", str(out), str(obj),
              str(stand_in)], "linking vdso_libc_probe")
    return out


def run_on_guest(guest: "base.Guest", binary: pathlib.Path, name: str, arguments: str = "") -> int:
    target = f"{GUEST_DIRECTORY}/{name}"
    pushed = guest.adb_run("push", str(binary), target, timeout=60)
    if pushed.returncode != 0:
        raise Failure(f"push {name} failed: {pushed.stdout.decode(errors='replace')[-500:]}")
    guest.shell(f"chmod 755 {target}")
    result = guest.adb_run("shell", f"{target} {arguments}; echo rc=$?", timeout=RUN_SECONDS)
    text = result.stdout.decode(errors="replace")
    print(text.strip())
    match = re.search(r"rc=(\d+)\s*$", text.strip())
    if not match:
        raise Failure(f"{name} printed no status: {text[-500:]}")
    return int(match.group(1))


def main() -> int:
    parser = base.build_parser()
    parser.description = __doc__
    parser.add_argument("--clang", help="clang++ that builds the probes (default: on PATH)")
    parser.add_argument("--lld", help="ld.lld that links them (default: on PATH)")
    options = parser.parse_args()
    if options.cpus <= 0 or options.boot_timeout <= 0:
        print("counts and timeouts must be positive", file=sys.stderr)
        return 2
    clang = options.clang or shutil.which("clang++")
    lld = options.lld or shutil.which("ld.lld")
    if not clang or not lld:
        print("--clang and --lld (or clang++ and ld.lld on PATH) build the probes", file=sys.stderr)
        return 2
    adb = boot_android.resolve_executable(options.adb, sdk_fallback=True)
    # Short, so the management console's socket path stays within sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-avd-"))
    guest: "base.Guest | None" = None
    try:
        # Android's configuration seals the system mappings: the probe requires it.
        probes = [("vdso_probe", build_freestanding(clang, lld, private), "--seal=sealed"),
                  ("vdso_libc_probe", build_bionic(clang, lld, private), "")]
        guest = base.start_guest(options, str(adb), private)
        failures = 0
        for name, binary, arguments in probes:
            status = run_on_guest(guest, binary, name, arguments)
            if status != 0:
                print(f"FAIL {name} reported {status} failure(s)", file=sys.stderr)
                failures += 1
        guest.alive()
        guest.shell("echo guest-answers")
        if failures:
            return 1
        print("PASS")
        return 0
    except (Failure, boot_android.LauncherError, subprocess.TimeoutExpired, OSError) as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        log = private / "emulator.log"
        if log.is_file():
            print("launcher log tail:\n" + "\n".join(log.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            boot_android.disconnect_adb(guest.adb, guest.serial)
            boot_android.stop_guest(guest.process)
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
