#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a built Andarium application that is about to be archived.

An archive of the Release configuration (Andarium/README.md#configurations) is
the build that is handed out. The build phase "Check embedded artifacts" runs
this script beside scripts/check-embedded-artifacts.py when the action is an
archive, and no other build is checked. The application must:

  - link to the developer's profile on X (Follow developer on X) only with a
    handle of the developer's own: the build writes the handle to Info.plist
    under AndariumFollowXHandle where the ignored local configuration names
    one. No key, or an empty one, is no link. A handle that is not a handle of
    X (1 to 15 letters, digits and underscores), and the placeholder that
    Config/Developer.local.xcconfig.example names, in any letter case, are
    refused: a copy of the example that nobody edited would otherwise ship a
    link to it;
  - read the version file of its updates (the update notice) only at the
    maker's own address: the build writes the host, the path and the link
    hosts to Info.plist under UpdateSourceHost, UpdateSourcePath and
    UpdateLinkHosts where the ignored local configuration names a host. No key,
    or an empty host, is no notice. An address the notice cannot read (a host
    or path that is not well formed), the reserved example domains that
    GuestHost/Config/Update.local.xcconfig.example names, in any letter case
    (also as a link host), an original identifier that is not the bundle's own
    (UpdateAppIdentifier) and a bundle that cannot ask the system for AltStore
    and SideStore (LSApplicationQueriesSchemes) are refused: a copy of the
    example that nobody edited would otherwise ship a read of it;
  - keep nothing of the machine that built it in the host code of its
    executable and of each embedded framework: no path under a home
    directory, on a mounted volume or in a temporary build directory, whatever
    the account or volume is called (only a printf conversion, a regular
    expression or a placeholder such as <user> is no name; the guest's own
    sections, which carry a kernel built elsewhere, are not host code), and no
    linker debug map in an embedded framework (the executable's is stripped when
    Xcode archives);
  - carry no test bundle and none of the frameworks and libraries that run
    one (XCTest, XCUIAutomation, Swift Testing, and the other test libraries of
    the SDK's developer directory), embedded or not: a load command of an
    executable or framework that names one is refused as well; and
  - link no test-support library (AndariumCoreTestSupport and its like): the
    code only tests and the probe tools use. Linkage is read from the load
    commands and the symbols and strings of the code; the paths of a debug
    map (a Derived Data directory called after a library) are no evidence.

An archive and an explicit install of Release both run the build phase (Xcode
gives both the action install), and both produce what is handed out, so both
are checked.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import plistlib
import re
import struct
import sys
from pathlib import Path

# The Info.plist key the build writes the handle to
# (Andarium/Config/Andarium.xcconfig).
FOLLOW_X_KEY = "AndariumFollowXHandle"
# The shape of a handle of X, the same rules as FollowXProfile.isWellFormed.
FOLLOW_X_HANDLE = re.compile(r"[A-Za-z0-9_]{1,15}")
# What Config/Developer.local.xcconfig.example names, and the handle the tests
# and the screen captures state: no archive links to it.
FOLLOW_X_EXAMPLE = "example_handle"


# The Info.plist keys the build writes the update notice's address to
# (GuestHost/Config/Update.xcconfig, GuestHost/Core's UpdateConfiguration).
UPDATE_HOST_KEY = "UpdateSourceHost"
UPDATE_PATH_KEY = "UpdateSourcePath"
UPDATE_LINK_HOSTS_KEY = "UpdateLinkHosts"
UPDATE_IDENTIFIER_KEY = "UpdateAppIdentifier"
UPDATE_SCHEMES_KEY = "LSApplicationQueriesSchemes"
# The sideloading apps the notice asks the system about.
UPDATE_SCHEMES = ("altstore", "sidestore")
# What Update.local.xcconfig.example names: the domains RFC 2606 reserves.
UPDATE_EXAMPLE_DOMAINS = ("example.com", "example.org", "example.net")


# What a path names about the machine that built the code: the account whose
# home directory held the checkout, the volume that held the checkout or the
# dependency prefixes, and the build's temporary directories. A bare
# "/Volumes/" or a format such as "/Volumes/%s" is no path of a build and does
# not match.
# The name of an account or a volume is any run of the characters a path
# component can hold (spaces, brackets, parentheses and non-ASCII bytes
# included), up to the limit of a component, that does not start with a blank.
# A literal name is a path wherever it begins: a disk called "[CI] Build SSD" is
# a build location. What is no name is what a tool carries as a pattern or a
# template, and only what is recognizable as one:
#   - a printf conversion: %s, %@, %1$s, %-8.3ld;
#   - a regular expression: a character class that is negated or has a
#     quantifier ([^/\s"']+, [a-z]+, [0-9]{1,3}), a group that holds an escape
#     class or a wildcard ((\w+), (.*)), an escape class or a wildcard with a
#     quantifier (\w+, .*);
#   - a placeholder: <user>, {volume}, $USER, ${HOME}, one of a few words in
#     any letter case, and the whole name.
# The application's own redaction (SessionRecord) carries /Users/[^/\s"']+.
_QUANTIFIER = rb"(?:[+*?]|\{[0-9]+(?:,[0-9]*)?\})"
_PLACEHOLDER = rb"(?:user|username|account|name|volume|disk|home|id|uuid|path|dir|directory|prefix)"
NOT_A_NAME = (
    rb"%(?:[0-9]+\$)?[0-9.*lhzjtqL-]*[A-Za-z@]"
    rb"|\[\^[^\]\x00]{1,64}\]"
    rb"|\[[^\]\x00]{1,64}\]" + _QUANTIFIER +
    rb"|\((?:\?:)?[^)\x00]{0,64}(?:\\[wWsSdD]|\.[*+]|\[\^)[^)\x00]{0,64}\)"
    rb"|\\[wWsSdD]" + _QUANTIFIER +
    rb"|\.[*+?]"
    rb"|(?i:<" + _PLACEHOLDER + rb">|\{" + _PLACEHOLDER + rb"\}|\$\{?" + _PLACEHOLDER + rb"\}?)(?=/)"
)
NAME = rb"(?!" + NOT_A_NAME + rb")[^\x00-\x1f\x7f/ ][^\x00-\x1f\x7f/]{0,254}"
HOST_PATHS = (
    ("a path under a home directory",
     re.compile(rb"/Users/" + NAME + rb"/[\x21-\x7e\x80-\xff]*")),
    ("a path on a mounted volume",
     re.compile(rb"/Volumes/" + NAME + rb"/[\x21-\x7e\x80-\xff]*")),
    ("a path in a temporary build directory",
     re.compile(rb"/(?:private/)?var/folders/[\x21-\x7e]+")),
)
# Sections that hold the guest, not the host: the Linux kernel image a runtime
# carries, and the boot files of a Linux runtime.
GUEST_SECTIONS = {("__TEXT", "__uml_elf"), ("__TEXT", "__uml_boot_base"), ("__TEXT", "__uml_boot_init")}

# What runs tests: a test bundle, and the frameworks and libraries that load it
# (XCTest, XCUIAutomation and the rest, Swift Testing), and a library of test
# support. They are in an application that `xcodebuild test` hosted.
TEST_SUPPORT = re.compile(
    r"^(?:.*\.xctest|XC[A-Za-z]*\.framework|libXC[A-Za-z]*\.dylib"
    r"|_?Testing(?:_[A-Za-z0-9]+)?\.framework|lib_?Testing[A-Za-z0-9_]*\.dylib"
    # The other frameworks of the SDK's developer directory, which holds no
    # library an application ships.
    r"|(?:AppIntentsTesting|Evaluations|StoreKitTest)\.framework"
    r"|[A-Za-z0-9_]*TestSupport[A-Za-z0-9_]*\.framework|lib[A-Za-z0-9_]*TestSupport[A-Za-z0-9_]*\.dylib)$")
# Where a platform keeps its developer libraries: no application loads one from
# there, whatever its name.
DEVELOPER_LIBRARIES = re.compile(r"/Developer/(?:Library/(?:Private)?Frameworks|usr/lib)/")
# The test-support library that only tests and the probe tool link
# (Andarium/AndariumCore/Package.swift), by its name as it appears in a linked
# binary: the module name, in Swift's mangled names and in plain text.
TEST_SUPPORT_LIBRARIES = (b"AndariumCoreTestSupport",)

MACHO_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
# The load commands that name a library the code needs: load, weak, re-export,
# upward and lazy.
LOAD_DYLIB_COMMANDS = {0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20}
N_STAB = 0xE0
N_OSO = 0x66
# The stabs whose strings name a function or a variable: the linker shares the
# string with the symbol of the same name, so it is evidence of what is linked
# and no part of a debug map's paths.
SYMBOL_STABS = frozenset({0x20, 0x24, 0x26, 0x28, 0x4E})


def emit(text: str, error: bool = False) -> None:
    print(text, file=sys.stderr if error else sys.stdout, flush=True)


def follow_x_problems(info: dict) -> list[str]:
    """What is wrong with the profile link the build names; nothing for no link."""
    if FOLLOW_X_KEY not in info:
        return []
    handle = info[FOLLOW_X_KEY]
    if handle == "":
        return []
    if not isinstance(handle, str) or not FOLLOW_X_HANDLE.fullmatch(handle):
        shown = repr(handle)[:40]
        return [
            f"Info.plist: {FOLLOW_X_KEY} is {shown}, not a handle of X "
            "(1 to 15 letters, digits and underscores)"
        ]
    if handle.lower() == FOLLOW_X_EXAMPLE:
        return [
            f"Info.plist: {FOLLOW_X_KEY} is the example handle of "
            "Developer.local.xcconfig.example; name the developer's own, or none"
        ]
    return []


def is_host_name(name: str) -> bool:
    """A host name as UpdateHostRule reads one: lowercase letters, digits, hyphens and dots."""
    if not 1 <= len(name) <= 253:
        return False
    return all(
        1 <= len(label) <= 63 and not label.startswith("-") and not label.endswith("-")
        and re.fullmatch(r"[0-9a-z-]+", label) is not None
        for label in name.split("."))


def is_example_domain(name: str) -> bool:
    name = name.lower()
    return any(name == domain or name.endswith("." + domain) for domain in UPDATE_EXAMPLE_DOMAINS)


def update_problems(info: dict) -> list[str]:
    """What is wrong with the update notice's address; nothing for no notice."""
    host = info.get(UPDATE_HOST_KEY)
    if host is None or host == "":
        return []
    problems = []
    if not isinstance(host, str) or "." not in host or not is_host_name(host):
        problems.append(
            f"Info.plist: {UPDATE_HOST_KEY} is {repr(host)[:40]}, not a host name the update notice reads "
            "(lowercase letters, digits and hyphens in dot-separated labels, no port)")
    elif is_example_domain(host):
        problems.append(
            f"Info.plist: {UPDATE_HOST_KEY} is the example address of "
            "Update.local.xcconfig.example; name the maker's own host, or none")
    path = info.get(UPDATE_PATH_KEY)
    if (not isinstance(path, str) or not path.startswith("/") or len(path) > 512
            or any(ord(c) <= 0x20 or ord(c) >= 0x7F or c in "?#" for c in path)):
        problems.append(
            f"Info.plist: {UPDATE_PATH_KEY} is {repr(path)[:40]}, not a path the update notice reads "
            "(it starts with / and holds no space, query or fragment)")
    hosts = info.get(UPDATE_LINK_HOSTS_KEY, "")
    if not isinstance(hosts, str):
        problems.append(f"Info.plist: {UPDATE_LINK_HOSTS_KEY} is {repr(hosts)[:40]}, not a list of host names")
    else:
        for entry in hosts.split():
            name = entry.lower().removeprefix("*.")
            if not is_host_name(name):
                problems.append(f"Info.plist: {UPDATE_LINK_HOSTS_KEY} names {entry[:40]!r}, no host name or *. pattern")
            elif is_example_domain(name):
                problems.append(
                    f"Info.plist: {UPDATE_LINK_HOSTS_KEY} names the example domain {entry[:40]!r} of "
                    "Update.local.xcconfig.example")
    identifier = info.get(UPDATE_IDENTIFIER_KEY)
    if not isinstance(identifier, str) or identifier != info.get("CFBundleIdentifier"):
        problems.append(
            f"Info.plist: {UPDATE_IDENTIFIER_KEY} is {repr(identifier)[:60]}, not the bundle's own identifier "
            "(the build copies PRODUCT_BUNDLE_IDENTIFIER)")
    schemes = info.get(UPDATE_SCHEMES_KEY)
    if not isinstance(schemes, list) or not all(scheme in schemes for scheme in UPDATE_SCHEMES):
        problems.append(
            f"Info.plist: {UPDATE_SCHEMES_KEY} must list {' and '.join(UPDATE_SCHEMES)}, "
            "or the notice never learns which sideloading app is installed")
    return problems


def thin_offset(data: bytes) -> int | None:
    """The offset of the arm64 slice of a Mach-O file; None for any other file."""
    if len(data) < 8:
        return None
    if struct.unpack_from(">I", data, 0)[0] == FAT_MAGIC:
        count = struct.unpack_from(">I", data, 4)[0]
        for index in range(min(count, 16)):
            cpu, _sub, offset, _size, _align = struct.unpack_from(">IIIII", data, 8 + 20 * index)
            if cpu == CPU_TYPE_ARM64:
                return offset
        return None
    return 0 if struct.unpack_from("<I", data, 0)[0] == MACHO_64 else None


def guest_ranges(data: bytes, base: int) -> list[tuple[int, int]]:
    """File ranges of the sections that hold the guest."""
    found = []
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SEGMENT_64:
            sections = struct.unpack_from("<I", data, position + 64)[0]
            entry = position + 72
            for _ in range(sections):
                name = data[entry:entry + 16].rstrip(b"\0").decode()
                segment = data[entry + 16:entry + 32].rstrip(b"\0").decode()
                length, offset = struct.unpack_from("<QI", data, entry + 40)
                if (segment, name) in GUEST_SECTIONS:
                    found.append((base + offset, base + offset + length))
                entry += 80
        position += size
    return found


def host_code(path: Path) -> tuple[bytes, int] | None:
    """The bytes of one Mach-O file with the guest's sections blanked, and the
    offset of its arm64 slice; None for a file that is not an arm64 Mach-O."""
    data = path.read_bytes()
    base = thin_offset(data[:4096])
    if base is None:
        return None
    buffer = bytearray(data)
    for start, end in guest_ranges(data, base):
        buffer[start:end] = bytes(end - start)
    return bytes(buffer), base


def symbol_table(data: bytes, base: int) -> tuple[int, int, int, int] | None:
    """The symbol table of a Mach-O: the offset and count of its struct
    nlist_64 entries, and the offset and size of its string table."""
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SYMTAB:
            offset, symbols, strings, strings_size = struct.unpack_from("<IIII", data, position + 8)
            return base + offset, symbols, base + strings, strings_size
        position += size
    return None


def debug_stabs(data: bytes, base: int) -> tuple[int, int]:
    """The debug stabs in a Mach-O's symbol table, and how many of them are
    N_OSO, each of which names an object file."""
    table = symbol_table(data, base)
    if table is None:
        return 0, 0
    offset, symbols, _strings, _size = table
    # struct nlist_64 is 16 bytes and n_type is its fifth.
    kinds = data[offset + 4:offset + 16 * symbols:16]
    return sum(1 for kind in kinds if kind & N_STAB), kinds.count(N_OSO)


def without_debug_map(data: bytes, base: int, keep: frozenset[int] = frozenset()) -> bytes:
    """The Mach-O with the strings of its debug stabs blanked: the names of the
    objects, sources and functions the linker recorded. A framework that
    carries them fails for that, and Xcode strips them from the application's
    own executable when it archives, so neither is a path the build host left
    in the code. `keep` names stab types whose strings stay (SYMBOL_STABS: the
    symbol the code carries shares its string)."""
    table = symbol_table(data, base)
    if table is None:
        return data
    offset, symbols, strings, size = table
    buffer = bytearray(data)
    for entry in range(symbols):
        string, kind = struct.unpack_from("<IB", data, offset + 16 * entry)
        if kind & N_STAB and kind not in keep and 0 < string < size:
            end = data.find(b"\0", strings + string, strings + size)
            if end > 0:
                buffer[strings + string:end] = bytes(end - strings - string)
    return bytes(buffer)


def build_host_traces(path: Path, debug_map: bool) -> list[str]:
    """What one Mach-O file keeps of the machine that built it. `debug_map`
    says whether its debug stabs count: they do for a framework, whose binary
    ships as linked. The strings of the stabs are never searched for a path: a
    framework with a debug map fails on the map, and the application's own
    executable loses it when Xcode archives."""
    code = host_code(path)
    if code is None:
        return []
    data, base = code
    found = []
    stabs, objects = debug_stabs(data, base) if debug_map else (0, 0)
    if stabs:
        found.append(
            f"carries the linker's debug map ({stabs} stabs, {objects} N_OSO "
            "object paths); strip -S it after dsymutil")
    data = without_debug_map(data, base)
    for label, pattern in HOST_PATHS:
        paths = sorted({match.decode() for match in pattern.findall(data)})
        if paths:
            found.append(
                f"names {label} ({len(paths)}): "
                f"{paths[0][:80]}{' ...' if len(paths) > 1 else ''}")
    return found


def loaded_libraries(data: bytes, base: int) -> list[str]:
    """The install names of the libraries the load commands of a Mach-O name."""
    names = []
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if size < 8:
            break
        if command in LOAD_DYLIB_COMMANDS:
            offset = struct.unpack_from("<I", data, position + 8)[0]
            end = data.find(b"\0", position + offset, position + size)
            raw = data[position + offset:end if end > 0 else position + size]
            names.append(raw.decode("utf-8", "replace"))
        position += size
    return names


def is_test_library(install_name: str) -> bool:
    """Whether a library is one that only tests load: by a component of its
    install name, or by the developer directory it is loaded from."""
    return bool(DEVELOPER_LIBRARIES.search(install_name)) or any(
        TEST_SUPPORT.match(part) for part in install_name.split("/"))


def test_support_links(path: Path) -> list[str]:
    """How one Mach-O file links test support: a load command that names a test
    library, and a test-support library whose code is in the file. The strings
    of a debug map's paths are no evidence, only the load commands and what
    the code and its symbols carry."""
    code = host_code(path)
    if code is None:
        return []
    data, base = code
    found = [f"loads {name}, which only tests load" for name in loaded_libraries(data, base) if is_test_library(name)]
    data = without_debug_map(data, base, keep=SYMBOL_STABS)
    found.extend(f"links {name.decode()}, which only tests link" for name in TEST_SUPPORT_LIBRARIES if name in data)
    return found


def executables_of(bundle: Path) -> list[Path]:
    """The executable of one bundle, if its Info.plist names one."""
    try:
        with (bundle / "Info.plist").open("rb") as handle:
            name = plistlib.load(handle).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException, ValueError):
        return []
    return [bundle / name] if isinstance(name, str) and (bundle / name).is_file() else []


def executables(app: Path) -> list[Path]:
    """The executables of the application and of its embedded frameworks."""
    return [
        binary
        for bundle in [app, *sorted((app / "Frameworks").glob("*.framework"))]
        for binary in executables_of(bundle)
    ]


def test_support(app: Path) -> list[str]:
    """The test bundles, test frameworks and test-support libraries anywhere in
    the application."""
    found = []
    for directory, directories, files in os.walk(app):
        for name in list(directories):
            if TEST_SUPPORT.match(name):
                found.append(str((Path(directory) / name).relative_to(app)))
                directories.remove(name)
        found.extend(
            str((Path(directory) / name).relative_to(app))
            for name in files if TEST_SUPPORT.match(name))
    return sorted(found)


def main() -> int:
    # A path of the build host may be in any script.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path, help="the built application bundle")
    options = parser.parse_args()
    app = options.app

    try:
        with (app / "Info.plist").open("rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        emit(f"{app}: has no readable Info.plist", error=True)
        return 1

    problems = [f"{app.name}: {problem}" for problem in follow_x_problems(info) + update_problems(info)]
    for binary in executables(app):
        name = binary.relative_to(app)
        # The application's own executable is stripped when Xcode archives it.
        problems.extend(
            f"{name}: {trace}" for trace in build_host_traces(binary, debug_map=binary.parent != app))
        problems.extend(f"{name}: {link}" for link in test_support_links(binary))
    problems.extend(
        f"{path}: a test bundle or test framework; archive the application without its tests"
        for path in test_support(app))
    for problem in problems:
        emit(problem, error=True)
    if problems:
        return 1
    emit(f"{app.name}: ready to archive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
