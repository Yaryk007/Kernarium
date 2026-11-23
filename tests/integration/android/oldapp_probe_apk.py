# SPDX-License-Identifier: GPL-2.0-or-later
"""The old-application probe: one application in three variants, built without Android's SDK.

android/lineage-24/overlay/oldapp makes a very old 32-bit game start on ART. A test of it needs an application
that does what such a game does and that is no one's game: the receiver here (oldapp_probe_dex.py) loads
`libkoldprobe.so` (oldapp/oldapp_probe.cpp), which reads a Java `byte[]` through the object pointer the way
Dalvik allowed, uses it through the array functions, and allocates from a heap that must lie below 2 GiB.

The variants have the same code and differ in the package, the target level and the process:

- ``old``: targetSdkVersion 10. The framework loads the layer for it once it is switched on, and the probe passes;
- ``modern``: targetSdkVersion 29, the highest a host can sign without Android's tools (the JAR signature of
  orientation_probe_apk.py is scheme v1). The rule the layer follows is "below 14", so any level from 14 up
  is the same witness; this one never gets the layer;
- ``named``: the old variant in a process named other than its package (android:process), as a game that
  declares its own process name is: the layer must still know which libraries are the application's own.

The old variant installs only past the package manager's block on old targets (`adb install
--bypass-low-target-sdk-block`, the path Andarium's installer takes for them). The packages are not
debuggable, as a game's is not (ART runs a debuggable application with CheckJNI), except the old package built a
second time for the scene row's CheckJNI run: the probe reports through the broadcast's result data, which
`am broadcast` prints.

The native libraries are built here with the host's clang and lld, freestanding for armeabi-v7a (the sources
include no Android header): the probe, and for a run on an image that lacks the layer, the layer itself.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import shutil
import subprocess
import tempfile

import oldapp_probe_dex
import orientation_probe_apk as binary_xml

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
OVERLAY = REPOSITORY / "android" / "lineage-24" / "overlay" / "oldapp"
PROBE_SOURCE = pathlib.Path(__file__).resolve().parent / "oldapp" / "oldapp_probe.cpp"
SHIM_SOURCES = (OVERLAY / "oldapp_android.cpp", OVERLAY / "jni_shim.cpp", OVERLAY / "oldapp_thunks.S")
#: The libraries each one's undefined symbols resolve in (compile_library).
PROBE_NEEDED = ("libc.so", "libdl.so")
SHIM_NEEDED = ("libc.so", "libdl.so", "liblog.so")
PROBE_LIBRARY = "koldprobe"
PROBE_LIBRARY_COPY = "koldprobe2"  # the same bytes under another name: a second file for the row bind_swap
SHIM_LIBRARY = "kernarium_oldapp"
#: The path on an image, where the framework loads the layer from.
SHIM_IMAGE_PATH = "/system/lib/libkernarium_oldapp.so"
#: The label an application may map and execute that library with.
SHIM_IMAGE_LABEL = "u:object_r:system_lib_file:s0"
#: The persistent property the framework reads for a package the person switched the layer on for (its value is the
#: turn-on time; empty means off): this prefix and the package name.
STORE_PROPERTY_PREFIX = "persist.sys.kernarium.oldapp."
RECEIVER = oldapp_probe_dex.CLASS_NAME
ABI_DIRECTORY = "lib/armeabi-v7a"

#: variant -> (package, minSdkVersion, targetSdkVersion)
VARIANTS = {
    "old": ("org.example.andarium.oldappprobe", 4, 10),
    "modern": ("org.example.andarium.oldappprobe.modern", 21, 29),
    "named": ("org.example.andarium.oldappprobe.named", 4, 10),
}
#: variant -> the name of its process, where it is not the package's (android:process): the application's own
#: libraries are told apart by the package, which the process name then does not show.
PROCESS_NAMES = {"named": "org.example.andarium.namedprocess"}


def package_of(variant: str) -> str:
    return VARIANTS[variant][0]


def component_of(variant: str) -> str:
    return f"{package_of(variant)}/{RECEIVER}"


def manifest_tree(variant: str, debuggable: bool = False) -> binary_xml.Element:
    """An application with one exported receiver that has no intent filter. A debuggable one runs with CheckJNI."""
    package, minimum, target = VARIANTS[variant]
    return binary_xml.Element("manifest", (("package", package), ("versionCode", 1)), (
        binary_xml.Element("uses-sdk", (("minSdkVersion", minimum), ("targetSdkVersion", target))),
        binary_xml.Element("application", (("label", "Old App Probe"), *((("debuggable", True),) if debuggable else ()),
                                           *((("process", PROCESS_NAMES[variant]),) if variant in PROCESS_NAMES else ())), (
            binary_xml.Element("receiver", (("name", RECEIVER), ("exported", True))),
        )),
    ))


def compile_library(sources: tuple[pathlib.Path, ...], destination: pathlib.Path, soname: str, *,
                    clang: str, lld: str, needed: tuple[str, ...] = ("libc.so",),
                    include: pathlib.Path = OVERLAY) -> pathlib.Path:
    """Builds a freestanding armv7 (Thumb) shared library from C++ sources that include no platform header (and the
    assembly files next to them).
    The library names `needed` become its DT_NEEDED entries: bionic resolves a library's undefined symbols
    only in the libraries it names (and the program's), and the host has no copies of them to link against,
    so each is linked as an empty stand-in of that soname."""
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="uml-oldapp-lib-"))
    try:
        empty = scratch / "empty.cpp"
        empty.write_text("")
        empty_object = scratch / "empty.o"
        subprocess.run([clang, "--target=armv7-linux-androideabi", "-march=armv7-a", "-c", str(empty), "-o",
                        str(empty_object)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=120, check=True)
        stand_ins = []
        for name in needed:
            stand_in = scratch / name
            subprocess.run([lld, "-shared", "-soname", name, "-o", str(stand_in), str(empty_object)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           timeout=120, check=True)
            stand_ins.append(str(stand_in))
        objects = []
        for source in sources:
            obj = scratch / (source.stem + ".o")
            if source.suffix == ".S":
                command = [clang, "--target=armv7-linux-androideabi", "-march=armv7-a", "-Werror", f"-I{include}", "-c",
                           str(source), "-o", str(obj)]
            else:
                command = [clang, "--target=armv7-linux-androideabi", "-march=armv7-a", "-mthumb", "-mfloat-abi=softfp",
                           "-std=c++20", "-ffreestanding", "-fPIC", "-fvisibility=hidden", "-fno-exceptions", "-fno-rtti",
                           "-fno-threadsafe-statics", "-O2", "-Wall", "-Wextra", "-Werror", "-nostdinc++", f"-I{include}",
                           "-c", str(source), "-o", str(obj)]
            subprocess.run(
                command,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                timeout=300, check=True)
            objects.append(str(obj))
        subprocess.run(
            [lld, "-shared", "-soname", soname, "--hash-style=gnu", "-z", "noexecstack", "-z", "relro", "-z", "now",
             "-o", str(destination), *objects, *stand_ins],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300,
            check=True)
        return destination
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def build_apk(destination: pathlib.Path, variant: str, probe_library: pathlib.Path,
              shim_library: pathlib.Path | None = None, workdir: pathlib.Path | None = None,
              debuggable: bool = False) -> pathlib.Path:
    """The signed package of a variant at `destination`. `shim_library` bundles the layer into the package, for a
    run on an image that does not carry it (the receiver loads it when its intent says ``--es shim
    kernarium_oldapp``)."""
    files = {
        "classes.dex": oldapp_probe_dex.build_dex(),
        f"{ABI_DIRECTORY}/lib{PROBE_LIBRARY}.so": probe_library.read_bytes(),
        # A second file of the same library: the row bind_swap opens it where another package's copy was closed.
        f"{ABI_DIRECTORY}/lib{PROBE_LIBRARY_COPY}.so": probe_library.read_bytes(),
    }
    if shim_library is not None:
        files[f"{ABI_DIRECTORY}/lib{SHIM_LIBRARY}.so"] = shim_library.read_bytes()
    return binary_xml.build_apk(destination, workdir, manifest_tree(variant, debuggable), files)
