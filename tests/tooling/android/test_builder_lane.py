#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import base64
import contextlib
import gzip
import importlib.util
import io
import json
import os
import random
import re
import shlex
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

from erofs_fixture import synthetic_erofs_image


def fail(message: str) -> None:
    raise SystemExit(f"android builder lane fixture: {message}")


def run_tool(
    tool: Path, *arguments: object, check: bool = False, **options: object
) -> subprocess.CompletedProcess[str]:
    """Run a Python tool with stdin closed.

    A checked run raises on failure and keeps the test's own output streams.
    Any other run captures stdout and stderr together as text, unless
    `options` name the streams.
    """
    if not check:
        options = {
            "stdout": subprocess.PIPE,
            "stderr": subprocess.STDOUT,
            "text": True,
            **options,
        }
    return subprocess.run(
        [sys.executable, "-B", str(tool), *map(str, arguments)],
        check=check,
        stdin=subprocess.DEVNULL,
        **options,
    )


def with_imports(payload: bytes, elf64: bool, imports: tuple[str, ...]) -> bytes:
    """Append a dynamic symbol table that imports the given functions, and the
    section headers that name it, to a synthetic ELF."""
    data = bytearray(payload)
    strings = bytearray(b"\0")
    symbols = bytearray(24 if elf64 else 16)
    for name in imports:
        offset = len(strings)
        strings.extend(name.encode() + b"\0")
        symbols.extend(
            struct.pack("<IBBHQQ", offset, 0x12, 0, 0, 0, 0)
            if elf64 else struct.pack("<IIIBBH", offset, 0, 0, 0x12, 0, 0)
        )
    data.extend(b"\0" * (-len(data) % 8))
    string_offset = len(data)
    data.extend(strings)
    data.extend(b"\0" * (-len(data) % 8))
    symbol_offset = len(data)
    data.extend(symbols)
    data.extend(b"\0" * (-len(data) % 8))
    section_offset = len(data)
    entry_size = 64 if elf64 else 40
    data.extend(b"\0" * entry_size)  # SHT_NULL
    for kind, offset, size, link, entry in (
        (11, symbol_offset, len(symbols), 2, 24 if elf64 else 16),  # .dynsym
        (3, string_offset, len(strings), 0, 0),                     # .dynstr
    ):
        data.extend(
            struct.pack("<IIQQQQIIQQ", 0, kind, 2, 0, offset, size, link, 1, 8, entry)
            if elf64 else
            struct.pack("<IIIIIIIIII", 0, kind, 2, 0, offset, size, link, 1, 4, entry)
        )
    if elf64:
        struct.pack_into("<Q", data, 40, section_offset)
        struct.pack_into("<HH", data, 58, entry_size, 3)
    else:
        struct.pack_into("<I", data, 32, section_offset)
        struct.pack_into("<HH", data, 46, entry_size, 3)
    return bytes(data)


# What a library built with -D_FILE_OFFSET_BITS=64 imports on a 32-bit target,
# and what the same source imports on a 64-bit one, where Bionic has one off_t.
LARGE_FILE_IMPORTS_32 = ("open", "mmap64", "lseek64", "munmap")
LARGE_FILE_IMPORTS_64 = ("open", "mmap", "lseek", "munmap")


def synthetic_aarch64_elf(
    alignment: int = 16384,
    needed: tuple[str, ...] = (),
    edit: tuple[str, int, int] | None = None,
    imports: tuple[str, ...] | None = None,
) -> bytes:
    """An AArch64 ET_DYN; `edit` is a (struct format, offset, value) overwrite
    and `imports` adds a dynamic symbol table."""
    program_headers = 2 if needed else 1
    dynamic_entries = 3 + len(needed) if needed else 0
    dynamic_offset = 64 + 56 * program_headers
    string_table = bytearray(b"\0")
    string_offsets: dict[str, int] = {}
    for item in needed:
        string_offsets[item] = len(string_table)
        string_table.extend(item.encode())
        string_table.append(0)
    payload_size = dynamic_offset + 16 * dynamic_entries
    string_offset = payload_size
    if needed:
        payload_size += len(string_table)
    payload = bytearray(payload_size)
    payload[:4] = b"\x7fELF"
    payload[4] = 2
    payload[5] = 1
    payload[6] = 1
    struct.pack_into("<HHI", payload, 16, 3, 183, 1)
    struct.pack_into("<Q", payload, 32, 64)
    struct.pack_into("<HHH", payload, 52, 64, 56, program_headers)
    struct.pack_into(
        "<IIQQQQQQ",
        payload,
        64,
        1,
        5,
        0,
        0,
        0,
        len(payload),
        len(payload),
        alignment,
    )
    if needed:
        dynamic_size = 16 * dynamic_entries
        struct.pack_into(
            "<IIQQQQQQ",
            payload,
            120,
            2,
            4,
            dynamic_offset,
            dynamic_offset,
            dynamic_offset,
            dynamic_size,
            dynamic_size,
            8,
        )
        entries = [(5, string_offset), (10, len(string_table))]
        for name in needed:
            entries.append((1, string_offsets[name]))
        entries.append((0, 0))
        for index, (tag, value) in enumerate(entries):
            struct.pack_into("<qQ", payload, dynamic_offset + index * 16, tag, value)
        payload[string_offset:] = string_table
    if edit is not None:
        struct.pack_into(edit[0], payload, edit[1], edit[2])
    return with_imports(bytes(payload), True, imports) if imports is not None else bytes(payload)


def synthetic_arm_elf(
    alignment: int = 4096,
    needed: tuple[str, ...] = (),
    edit: tuple[str, int, int] | None = None,
    flags: int = 0x05000200,
    imports: tuple[str, ...] | None = None,
) -> bytes:
    """An Arm ET_DYN of the softfp EABI; `edit` is a (struct format, offset,
    value) overwrite and `imports` adds a dynamic symbol table."""
    program_headers = 2 if needed else 1
    dynamic_entries = 3 + len(needed) if needed else 0
    dynamic_offset = 52 + 32 * program_headers
    string_table = bytearray(b"\0")
    string_offsets: dict[str, int] = {}
    for item in needed:
        string_offsets[item] = len(string_table)
        string_table.extend(item.encode())
        string_table.append(0)
    payload_size = dynamic_offset + 8 * dynamic_entries
    string_offset = payload_size
    if needed:
        payload_size += len(string_table)
    payload = bytearray(payload_size)
    payload[:4] = b"\x7fELF"
    payload[4] = 1
    payload[5] = 1
    payload[6] = 1
    struct.pack_into("<HHI", payload, 16, 3, 40, 1)
    struct.pack_into("<I", payload, 28, 52)
    struct.pack_into("<I", payload, 36, flags)
    struct.pack_into("<HHH", payload, 40, 52, 32, program_headers)
    struct.pack_into(
        "<IIIIIIII", payload, 52, 1, 0, 0, 0, len(payload), len(payload), 5, alignment
    )
    if needed:
        dynamic_size = 8 * dynamic_entries
        struct.pack_into(
            "<IIIIIIII", payload, 84, 2, dynamic_offset, dynamic_offset,
            dynamic_offset, dynamic_size, dynamic_size, 4, 4,
        )
        entries = [(5, string_offset), (10, len(string_table))]
        for name in needed:
            entries.append((1, string_offsets[name]))
        entries.append((0, 0))
        for index, (tag, value) in enumerate(entries):
            struct.pack_into("<iI", payload, dynamic_offset + index * 8, tag, value)
        payload[string_offset:] = string_table
    if edit is not None:
        struct.pack_into(edit[0], payload, edit[1], edit[2])
    return with_imports(bytes(payload), False, imports) if imports is not None else bytes(payload)


# The permission the braille keyboard used to request for a receiver guard nobody declared: it is
# on neither application's list of permissions.
OLD_GUARD_PERMISSION = "com.google.android.marvin.talkback.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"
# The guard that androidx core adds to TalkBack's manifest, named after the installed package.
TALKBACK_GUARD_PERMISSION = "com.android.talkback.DYNAMIC_RECEIVER_NOT_EXPORTED_PERMISSION"


def synthetic_compiled_manifest(
    target_sdk: int | None, *, string_value: bool = False, debuggable: bool | str | None = None,
    permissions: tuple[str, ...] = (), declared: tuple[str, ...] = (), utf8: bool = False,
    sdk23: tuple[str, ...] = (),
) -> bytes:
    """A compiled manifest with <uses-sdk>, which carries targetSdkVersion when given, and, when
    debuggable is not None, an <application> that carries android:debuggable: a literal boolean,
    or a resource reference when debuggable is "reference". It requests each of `permissions` with
    <uses-permission> (and each of `sdk23` with <uses-permission-sdk-23>) and declares each of
    `declared` with <permission>; a name that starts with "@" is a resource reference, "@" and the
    hexadecimal id; the string pool is UTF-8
    when `utf8` is set, UTF-16 as the platform's own compiled manifests are otherwise.

    It has the chunks the verifier reads and no more: a string pool, the resource map that gives
    the attribute names their framework IDs, and the elements."""
    strings = ["targetSdkVersion", "debuggable", "uses-sdk", "application", "28", "name",
               "uses-permission", "permission", *permissions, *declared, *sdk23,
               "uses-permission-sdk-23"]
    pool_strings = b""
    offsets = []
    for text in strings:
        offsets.append(len(pool_strings))
        if utf8:
            pool_strings += bytes([len(text), len(text)]) + text.encode() + b"\0"
        else:
            pool_strings += struct.pack("<H", len(text)) + text.encode("utf-16-le") + b"\0\0"
    pool_strings += b"\0" * (-len(pool_strings) % 4)
    strings_start = 28 + 4 * len(strings)
    pool = struct.pack(
        "<HHIIIIII", 0x0001, 28, strings_start + len(pool_strings), len(strings),
        0, 0x100 if utf8 else 0, strings_start, 0,
    ) + b"".join(struct.pack("<I", offset) for offset in offsets) + pool_strings
    # The attribute names (strings 0 and 1, and "name" at 5) carry their framework IDs.
    ids = [0x01010270, 0x0101000F, 0, 0, 0, 0x01010003]
    resource_map = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(ids)) + b"".join(
        struct.pack("<I", value) for value in ids)

    def element(name: int, attributes: bytes) -> bytes:
        body = struct.pack(
            "<IIHHHHHH", 0xFFFFFFFF, name, 20, 20, len(attributes) // 20, 0, 0, 0
        ) + attributes
        return struct.pack("<HHIII", 0x0102, 16, 16 + len(body), 1, 0xFFFFFFFF) + body

    sdk_attributes = b""
    if target_sdk is not None:
        sdk_attributes = struct.pack(
            "<IIIHBBI", 0xFFFFFFFF, 0, 4 if string_value else 0xFFFFFFFF, 8, 0,
            0x03 if string_value else 0x10, 4 if string_value else target_sdk,
        )
    elements = element(2, sdk_attributes)
    for index, element_name in enumerate([6] * len(permissions) + [7] * len(declared)):
        permission = 8 + index
        text = strings[permission]
        if text.startswith("@"):
            # A name given as a resource reference ("@0x7f0a0001"): not a string.
            elements += element(element_name, struct.pack(
                "<IIIHBBI", 0xFFFFFFFF, 5, 0xFFFFFFFF, 8, 0, 0x01, int(text[1:], 16)))
        else:
            elements += element(element_name, struct.pack(
                "<IIIHBBI", 0xFFFFFFFF, 5, permission, 8, 0, 0x03, permission))
    sdk23_element = 8 + len(permissions) + len(declared) + len(sdk23)
    for index in range(len(sdk23)):
        permission = 8 + len(permissions) + len(declared) + index
        elements += element(sdk23_element, struct.pack(
            "<IIIHBBI", 0xFFFFFFFF, 5, permission, 8, 0, 0x03, permission))
    if debuggable is not None:
        value = (0x01, 0x7F010000) if debuggable == "reference" else (
            0x12, 0xFFFFFFFF if debuggable else 0)
        elements += element(3, struct.pack(
            "<IIIHBBI", 0xFFFFFFFF, 1, 0xFFFFFFFF, 8, 0, *value))
    payload = pool + resource_map + elements
    return struct.pack("<HHI", 0x0003, 8, 8 + len(payload)) + payload


def synthetic_apk(debuggable: bool | str | None = None, permissions: tuple[str, ...] = (),
                  declared: tuple[str, ...] = (), sdk23: tuple[str, ...] = ()) -> bytes:
    """An application archive whose compiled manifest is the one synthetic_compiled_manifest builds."""
    package = io.BytesIO()
    with zipfile.ZipFile(package, "w") as apk:
        apk.writestr("AndroidManifest.xml", synthetic_compiled_manifest(
            None, debuggable=debuggable, permissions=permissions, declared=declared, sdk23=sdk23))
    return package.getvalue()


# The compiled manifest of an overlay that aapt2 built with android:resourcesMap="@xml/map", whose map
# names config_sensorUseStartedActivity and config_sensorUseStartedActivity_hwToggle as targets (the
# overlay's own string table has only "different_name").
RESOURCES_MAP_OVERLAY_MANIFEST = base64.b64decode(
    "".join((
        "AwAIADQFAAABABwA8AIAABQAAAAAAAAAAAAAAGwAAAAAAAAAAAAAABIAAAAmAAAARAAAAGIAAACGAAAAmgAAAMAAAAD2AAAAEgEA",
        "ABoBAAAsAQAARgEAAHYBAADOAQAA4gEAAPQBAAAGAgAAOgIAAG4CAAAHAGgAYQBzAEMAbwBkAGUAAAAIAHAAcgBpAG8AcgBpAHQA",
        "eQAAAA0AdABhAHIAZwBlAHQAUABhAGMAawBhAGcAZQAAAA0AbQBpAG4AUwBkAGsAVgBlAHIAcwBpAG8AbgAAABAAdABhAHIAZwBl",
        "AHQAUwBkAGsAVgBlAHIAcwBpAG8AbgAAAAgAaQBzAFMAdABhAHQAaQBjAAAAEQBjAG8AbQBwAGkAbABlAFMAZABrAFYAZQByAHMA",
        "aQBvAG4AAAAZAGMAbwBtAHAAaQBsAGUAUwBkAGsAVgBlAHIAcwBpAG8AbgBDAG8AZABlAG4AYQBtAGUAAAAMAHIAZQBzAG8AdQBy",
        "AGMAZQBzAE0AYQBwAAAAAgAxADYAAAAHAGEAbgBkAHIAbwBpAGQAAAALAGEAcABwAGwAaQBjAGEAdABpAG8AbgAAABYAZQB4AGEA",
        "bQBwAGwAZQAuAHIAZQB2AGkAZQB3AC4AbwB2AGUAcgBsAGEAeQAAACoAaAB0AHQAcAA6AC8ALwBzAGMAaABlAG0AYQBzAC4AYQBu",
        "AGQAcgBvAGkAZAAuAGMAbwBtAC8AYQBwAGsALwByAGUAcwAvAGEAbgBkAHIAbwBpAGQAAAAIAG0AYQBuAGkAZgBlAHMAdAAAAAcA",
        "bwB2AGUAcgBsAGEAeQAAAAcAcABhAGMAawBhAGcAZQAAABgAcABsAGEAdABmAG8AcgBtAEIAdQBpAGwAZABWAGUAcgBzAGkAbwBu",
        "AEMAbwBkAGUAAAAYAHAAbABhAHQAZgBvAHIAbQBCAHUAaQBsAGQAVgBlAHIAcwBpAG8AbgBOAGEAbQBlAAAACAB1AHMAZQBzAC0A",
        "cwBkAGsAAAAAAIABCAAsAAAADAABARwAAQEhAAEBDAIBAXACAQFaBQEBcgUBAXMFAQEJBgEBAAEQABgAAAABAAAA/////woAAAAN",
        "AAAAAgEQAIgAAAABAAAA//////////8OAAAAFAAUAAUAAAAAAAAADQAAAAYAAAD/////CAAAECQAAAANAAAABwAAAAkAAAAIAAAD",
        "CQAAAP////8QAAAADAAAAAgAAAMMAAAA/////xEAAAD/////CAAAECQAAAD/////EgAAAP////8IAAAQEAAAAAIBEABMAAAAAgAA",
        "AP//////////EwAAABQAFAACAAAAAAAAAA0AAAADAAAA/////wgAABAeAAAADQAAAAQAAAD/////CAAAEB4AAAADARAAGAAAAAIA",
        "AAD//////////xMAAAACARAAdAAAAAMAAAD//////////w8AAAAUABQABAAAAAAAAAANAAAAAQAAAP////8IAAAQ5wMAAA0AAAAC",
        "AAAACgAAAAgAAAMKAAAADQAAAAUAAAD/////CAAAEv////8NAAAACAAAAP////8IAAABAAACfwMBEAAYAAAAAwAAAP//////////",
        "DwAAAAIBEAA4AAAABAAAAP//////////CwAAABQAFAABAAAAAAAAAA0AAAAAAAAA/////wgAABIAAAAAAwEQABgAAAAEAAAA////",
        "//////8LAAAAAwEQABgAAAABAAAA//////////8OAAAAAQEQABgAAAABAAAA/////woAAAANAAAA",
    ))
)


def synthetic_overlay_manifest(target_package: str | None, static: bool | None, *, utf8: bool = False) -> bytes:
    """A compiled manifest of a runtime resource overlay: an <overlay> element that carries
    android:targetPackage (a string) when given and android:isStatic (a literal boolean) when given,
    as the build's auto-generated overlays do."""
    strings = ["targetPackage", "isStatic", "overlay", target_package or ""]
    pool_strings = b""
    offsets = []
    for text in strings:
        offsets.append(len(pool_strings))
        if utf8:
            pool_strings += bytes([len(text), len(text)]) + text.encode() + b"\0"
        else:
            pool_strings += struct.pack("<H", len(text)) + text.encode("utf-16-le") + b"\0\0"
    pool_strings += b"\0" * (-len(pool_strings) % 4)
    strings_start = 28 + 4 * len(strings)
    pool = struct.pack(
        "<HHIIIIII", 0x0001, 28, strings_start + len(pool_strings), len(strings),
        0, 0x100 if utf8 else 0, strings_start, 0,
    ) + b"".join(struct.pack("<I", offset) for offset in offsets) + pool_strings
    ids = [0x01010021, 0x0101055A, 0, 0]
    resource_map = struct.pack("<HHI", 0x0180, 8, 8 + 4 * len(ids)) + b"".join(
        struct.pack("<I", value) for value in ids)
    attributes = b""
    if target_package is not None:
        attributes += struct.pack("<IIIHBBI", 0xFFFFFFFF, 0, 3, 8, 0, 0x03, 3)
    if static is not None:
        attributes += struct.pack("<IIIHBBI", 0xFFFFFFFF, 1, 0xFFFFFFFF, 8, 0, 0x12, 0xFFFFFFFF if static else 0)
    body = struct.pack("<IIHHHHHH", 0xFFFFFFFF, 2, 20, 20, len(attributes) // 20, 0, 0, 0) + attributes
    element = struct.pack("<HHIII", 0x0102, 16, 16 + len(body), 1, 0xFFFFFFFF) + body
    payload = pool + resource_map + element
    return struct.pack("<HHI", 0x0003, 8, 8 + len(payload)) + payload


def run_in_process(module, *arguments: object) -> subprocess.CompletedProcess[str]:
    """Run a loaded tool's main() as `run_tool` would run its script.

    The result carries the exit status the interpreter would report and the
    tool's stdout and stderr together: a tool that exits with a message
    reports status 1 and prints the message, and an uncaught exception reports
    status 1 and prints its traceback. A fixture-heavy test runs a verifier
    hundreds of times; a process each would spend most of its time starting
    Python and importing the verifier.
    """
    output = io.StringIO()
    status = 0
    saved = sys.argv
    sys.argv = [str(module.__file__), *map(str, arguments)]
    try:
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                module.main()
            except SystemExit as stop:
                if stop.code is None:
                    status = 0
                elif isinstance(stop.code, int):
                    status = stop.code
                else:
                    print(stop.code, file=sys.stderr)
                    status = 1
            except Exception:
                traceback.print_exc()
                status = 1
    finally:
        sys.argv = saved
    return subprocess.CompletedProcess(
        [sys.executable, str(module.__file__), *map(str, arguments)],
        status,
        output.getvalue(),
    )


def load_tool(path: Path):
    """Import a tool whose hyphenated file name is not a module name."""
    spec = importlib.util.spec_from_file_location(
        path.stem.replace("-", "_"), path
    )
    if spec is None or spec.loader is None:
        fail(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(path.parent))
    return module


_SMS_TOOL = None


def sms_policy_fixture(verifier: Path, variant: str | None, build_variant: str = "user") -> dict[str, str]:
    """The text message service's part of the policy a target-files fixture carries: the platform's vsock
    and property rules as the reviewed lists give them, the service's own domain, data type and rules (as
    overlay/sepolicy/system_ext/private/kernarium_sms.te makes them), the seapp_contexts lines and the property
    label. A variant breaks one thing, in the way the verifier's check must catch. The lists come from the
    verifier, so that the fixture is the baseline the check accepts and the variants differ from it by one
    defect."""
    global _SMS_TOOL
    if _SMS_TOOL is None:
        _SMS_TOOL = load_tool(verifier)
    tool = _SMS_TOOL
    plat = ""
    for name in sorted(tool.VSOCK_SOCKET_TYPES):
        permissions = "create bind connect read write getattr getopt shutdown"
        if name not in tool.VSOCK_TYPES_WITHOUT_LISTEN:
            permissions += " listen accept"
        # adbd has its compatibility type of the platform version, which the verifier counts as adbd.
        plat += f"(allow {name}{'_202604' if name == 'adbd' else ''} self (vsock_socket ({permissions})))\n"
    for setter in sorted(tool.ADB_LISTEN_ADDRS_SETTERS):
        plat += f"(allow {setter} system_prop (property_service (set)))\n"
    # The platform's own rule for dumpstate, the vsock instance of its getattr rule for every socket class:
    # over the attribute domain it reaches the service's sockets, and every Android 17 platform policy has it.
    plat += "(allow dumpstate domain (vsock_socket (getattr)))\n"
    # The platform's own rule for init, which owns the property service: it may set every property type, ours among
    # them (the system_ext policy lists the type in property_type, as the compiler writes it).
    plat += "(allow init property_type (property_service (set)))\n"
    # What the platform gives the application domains through groups of them: each group is the application
    # domains but the isolated, sandboxed and ephemeral ones, as the compiler writes the platform's rules.
    # The service is an application domain, so it inherits them, and they are accepted; a variant takes one
    # ordinary application domain out of a group, or puts the same rule in the system_ext policy.
    gap = {
        "inherited-find-without-untrusted-app": ("base_typeattr_275", "untrusted_app"),
        "inherited-find-without-platform-app": ("base_typeattr_275", "platform_app"),
        "inherited-find-without-priv-app": ("base_typeattr_275", "priv_app"),
        "inherited-find-without-system-app": ("base_typeattr_275", "system_app"),
        "inherited-file-without-priv-app": ("base_typeattr_280", "priv_app"),
        "inherited-vsock-without-platform-app": ("base_typeattr_285", "platform_app"),
    }.get(variant or "", (None, None))
    inherited = {
        "base_typeattr_275": "(allow base_typeattr_275 keystore_service (service_manager (find)))\n",
        "base_typeattr_280": "(allow base_typeattr_280 system_app_data_file (file (read write getattr map)))\n",
        "base_typeattr_285": "(allow base_typeattr_285 virtualizationservice (vsock_socket (read write getattr)))\n",
    }
    # A group that lists the four ordinary application domains by name, with the service: the control for a
    # variant that lists a type of a version suffix in place of one of them.
    stand_in = "priv_app_299999" if variant == "inherited-find-with-a-lookalike" else "priv_app"
    plat += (f"(typeattributeset base_typeattr_290 (kernarium_sms untrusted_app platform_app {stand_in} system_app))\n"
             "(allow base_typeattr_290 window_service (service_manager (find)))\n")
    for attribute, rule in inherited.items():
        left_out = " ".join(["isolated_app", "ephemeral_app", "sdk_sandbox_34", "sdk_sandbox_audit", "sdk_sandbox_next"]
                            + ([gap[1]] if gap[0] == attribute else []))
        plat += f"(typeattributeset {attribute} (and (appdomain ) (not ({left_out} ))))\n{rule}"
    # What the platform grants every domain in the binder family (the rules over the attribute domain with a few
    # exclusions, as the compiler writes them): a binder call to system_server and the lookup of the binder statistics
    # service. The holder and the waker hold them in effect; the group lists the ordinary application domains, as
    # for the service above. The platform's control property rules: system_app and system_server may set
    # ctl_default_prop, the label of every control key that has no entry of its own.
    plat += ("(typeattributeset base_typeattr_291 (kernarium_sms kernarium_sms_listener kernarium_sms_wake "
             "untrusted_app platform_app priv_app system_app))\n"
             f"(allow base_typeattr_291 system_server (binder ({'call transfer' if variant == 'holder-binder-grant-widened' else 'call'})))\n"
             "(allow base_typeattr_291 binder_stats_consumer_service (service_manager (find)))\n"
             "(allow system_app ctl_default_prop (property_service (set)))\n"
             "(allow system_server ctl_default_prop (property_service (set)))\n"
             # The platform lets every domain open the binder and the hardware binder device nodes (the vendor's
             # is the vendor's alone).
             "(allow base_typeattr_291 binder_device (chr_file (append getattr ioctl lock map open read watch "
             "watch_reads write)))\n"
             "(allow base_typeattr_291 hwbinder_device (chr_file (append getattr ioctl lock map open read watch "
             "watch_reads write)))\n")
    # A debuggable build lets every domain call the root shell (the variant puts the rule in a user build, which has
    # no such shell).
    if (build_variant != "user" or variant == "su-binder-in-a-user-build") and variant != "su-binder-removed":
        su_rights = {"su-binder-call-only": "call", "su-binder-transfer-only": "transfer"}.get(variant or "", "call transfer")
        plat += f"(allow domain su (binder ({su_rights})))\n"
    ext = (
        "(type kernarium_sms)\n(type kernarium_sms_data_file)\n"
        "(type kernarium_sms_listener)\n(type kernarium_sms_listener_exec)\n(type kernarium_sms_socket)\n"
        "(type kernarium_sms_wake)\n(type kernarium_sms_wake_exec)\n(type ctl_kernarium_sms_wake_prop)\n"
        "(type ctl_kernarium_sms_locked_prop)\n"
        "(typeattributeset property_type (ctl_kernarium_sms_wake_prop ctl_kernarium_sms_locked_prop))\n"
        "(typeattributeset domain (kernarium_sms kernarium_sms_listener kernarium_sms_wake))\n"
        "(typeattributeset data_file_type (kernarium_sms_data_file))\n"
        "(typeattributeset core_data_file_type (kernarium_sms_data_file))\n"
        "(typeattributeset app_data_file_type (kernarium_sms_data_file))\n"
        "(allow kernarium_sms kernarium_sms_data_file (dir (create getattr open read search write add_name remove_name "
        "rename reparent rmdir setattr lock)))\n"
        "(allow kernarium_sms kernarium_sms_data_file (file (create getattr open read write append rename setattr "
        "unlink lock map)))\n"
        "(allow kernarium_sms activity_service (service_manager (find)))\n"
        "(allow kernarium_sms package_service (service_manager (find)))\n"
        "(allow kernarium_sms role_service (service_manager (find)))\n"
        # The holder makes the socket of port 5560 and listens; the service accepts from the descriptor it is passed
        # and connects to the holder's unix socket to be passed it.
        "(allow kernarium_sms_listener self (vsock_socket (create bind listen)))\n"
        "(allow kernarium_sms kernarium_sms_listener (vsock_socket (accept read write shutdown getattr getopt)))\n"
        "(allow kernarium_sms kernarium_sms_listener (fd (use)))\n"
        "(allow kernarium_sms kernarium_sms_socket (sock_file (write)))\n"
        "(allow kernarium_sms kernarium_sms_listener (unix_stream_socket (connectto)))\n"
        # Init makes and binds the handoff socket and does not listen on a stream socket: the holder does.
        "(allow kernarium_sms_listener self (unix_stream_socket (accept getattr getopt listen read write)))\n"
        "(allow init kernarium_sms_listener (unix_stream_socket (create bind)))\n"
        "(allow kernarium_sms_listener ctl_kernarium_sms_wake_prop (property_service (set)))\n"
        "(allow kernarium_sms_listener property_socket (sock_file (write)))\n"
        "(allow kernarium_sms_listener init (unix_stream_socket (connectto)))\n"
        # init enters the holder's and the waker's domains through their own executables, and no one else does.
        "(allow init kernarium_sms_listener_exec (file (read getattr map execute open)))\n"
        "(allow init kernarium_sms_listener (process (transition)))\n"
        "(typetransition init kernarium_sms_listener_exec process kernarium_sms_listener)\n"
        "(allow init kernarium_sms_wake_exec (file (read getattr map execute open)))\n"
        "(allow init kernarium_sms_wake (process (transition)))\n"
        "(typetransition init kernarium_sms_wake_exec process kernarium_sms_wake)\n"
        # The waker is the framework's client for one call and holds nothing else.
        "(allow kernarium_sms_wake servicemanager (binder (call transfer)))\n"
        "(allow kernarium_sms_wake activity_service (service_manager (find)))\n"
        "(allow kernarium_sms_wake system_server (binder (transfer)))\n"
        "(allow system_server kernarium_sms_wake (binder (call)))\n"
        "(allow system_server kernarium_sms_wake (fd (use)))\n"
        # The platform's fd use over every domain, which reaches the holder as it reaches every domain.
        "(allow crash_dump domain (fd (use)))\n"
        "(allow installd app_data_file_type (dir (create getattr open read search write)))\n"
        "(allow installd app_data_file_type (file (create getattr open read write unlink)))\n"
    )
    # init checks the legacy key ctl.kernarium_sms_wake first, and the holder is denied it: the policy silences the denial.
    if variant == "holder-dontaudit-for-another":
        ext += "(dontaudit system_app ctl_kernarium_sms_locked_prop (property_service (set)))\n"
    elif variant == "holder-dontaudit-over-an-attribute":
        ext += "(dontaudit kernarium_sms_listener property_type (property_service (set)))\n"
    elif variant != "no-holder-dontaudit":
        ext += "(dontaudit kernarium_sms_listener ctl_kernarium_sms_locked_prop (property_service (set)))\n"
    if variant == "holder-dontaudit-of-another-type":
        ext += "(dontaudit kernarium_sms_listener ctl_kernarium_sms_wake_prop (property_service (set)))\n"
    elif variant == "holder-dontaudit-for-every-domain":
        ext += "(dontaudit domain system_prop (property_service (set)))\n"
    elif variant == "holder-dontaudit-over-an-attribute-too":
        ext += "(dontaudit kernarium_sms_listener property_type (property_service (set)))\n"
    if variant != "no-vsock-attribute":
        ext += "(typeattributeset unconstrained_vsock_violators (kernarium_sms))\n"
    if variant != "holder-without-the-vsock-attribute":
        ext += "(typeattributeset unconstrained_vsock_violators (kernarium_sms_listener))\n"
    # The platform labels the user's directories with the user's categories, which the service's level lacks: only a
    # domain that is trusted against the multi-level constraints can search them. The daemons search none.
    # The platform's compiler writes a membership as a chain of set expressions that name one another, which the
    # service's trust is here (the expression that the attribute names comes first, as the compiler's order allows).
    def trusted_through_expressions(domain: str, number: int) -> str:
        return (f"(typeattributeset mlstrustedsubject (and (base_typeattr_{number} ) (not (surfaceflinger ))))\n"
                f"(typeattributeset base_typeattr_{number} (and (base_typeattr_{number + 1} ) (not (surfaceflinger ))))\n"
                f"(typeattributeset base_typeattr_{number + 1} ({domain}))\n")

    if variant != "no-mls-trust":
        ext += trusted_through_expressions("kernarium_sms", 501)
    if variant == "holder-mls-trusted":
        ext += "(typeattributeset mlstrustedsubject (kernarium_sms_listener))\n"
    if variant == "waker-mls-trusted":
        ext += "(typeattributeset mlstrustedsubject (kernarium_sms_wake))\n"
    if variant == "holder-mls-trusted-through-expressions":
        ext += trusted_through_expressions("kernarium_sms_listener", 503)
    if variant == "waker-mls-trusted-through-expressions":
        ext += trusted_through_expressions("kernarium_sms_wake", 505)
    if variant != "no-core-domain":
        ext += "(typeattributeset coredomain (kernarium_sms))\n"
    if variant != "holder-not-a-core-domain":
        ext += "(typeattributeset coredomain (kernarium_sms_listener))\n"
    if variant != "waker-not-a-core-domain":
        ext += "(typeattributeset coredomain (kernarium_sms_wake))\n"
    if variant != "no-app-domain":
        ext += "(typeattributeset appdomain (kernarium_sms))\n"
    seapp_plat = (
        "user=system seinfo=platform domain=system_app type=system_app_data_file\n"
        "user=_app seinfo=platform domain=platform_app type=app_data_file levelFrom=user\n"
        "isSystemServer=true domain=system_server_app\n"
    )
    seapp_ext = (
        "" if variant == "no-seapp-line" else
        "user=system seinfo=platform name=android.kernarium.sms domain=kernarium_sms type=kernarium_sms_data_file\n"
    )
    property_context = "" if variant == "no-property-label" else (
        "service.adb.listen_addrs  u:object_r:system_prop:s0 exact string\n")
    # The control properties: the platform's generic entries (the default of every ctl. key, and the qualified keys of
    # start, stop, restart and sigstop) and the product's own, which label the key that starts the waker, the legacy
    # ctl.<service> key init checks first, and the oneshot keys the platform leaves to the default.
    property_context += (
        "ctl.  u:object_r:ctl_default_prop:s0\n"
        "ctl.sigstop_on$  u:object_r:ctl_sigstop_prop:s0\n"
        "ctl.sigstop_off$  u:object_r:ctl_sigstop_prop:s0\n"
        "ctl.start$  u:object_r:ctl_start_prop:s0\n"
        # A word that reads exact among the values of the type is not the match operation (the third field): the entry
        # is a prefix, and labels every longer name, which the product's keys include.
        "ctl.stop$  u:object_r:ctl_stop_prop:s0 prefix enum on exact off\n"
        "ctl.restart$  u:object_r:ctl_restart_prop:s0\n"
    )
    if variant != "wake-property-generic":
        property_context += ("ctl.start$kernarium_sms_wake  u:object_r:ctl_kernarium_sms_wake_prop:s0"
                             + {"wake-property-not-exact": "", "wake-property-exact-in-a-later-field": " prefix enum on exact off"
                                }.get(variant or "", " exact") + "\n")
    locked, generic = "u:object_r:ctl_kernarium_sms_locked_prop:s0", "u:object_r:ctl_default_prop:s0"
    for service in ("kernarium_sms_listener", "kernarium_sms_wake"):
        if variant != "control-legacy-unlabelled":
            as_generic = variant == "control-legacy-generic" or (
                variant == "control-legacy-waker-generic" and service == "kernarium_sms_wake")
            property_context += (f"ctl.{service}  {generic if as_generic else locked}"
                                 + {"control-legacy-not-exact": "", "control-legacy-exact-in-a-later-field": " prefix enum on exact off"
                                    }.get(variant or "", " exact") + "\n")
        if variant != "control-oneshot-unlabelled":
            for state in ("on", "off"):
                property_context += (f"ctl.oneshot_{state}${service}  "
                                     f"{generic if variant == 'control-oneshot-generic' else locked}"
                                     f"{' prefix enum on exact off' if variant == 'control-oneshot-exact-in-a-later-field' else ' exact'}\n")
    labels = {
        "holder-unlabelled": "kernarium_sms_listener_exec", "waker-unlabelled": "kernarium_sms_wake_exec",
        "socket-unlabelled": "kernarium_sms_socket",
    }
    file_context = "".join(
        f"{path}  u:object_r:{label}:s0\n" for path, label in (
            ("/(system_ext|system/system_ext)/bin/kernarium_sms_listener", "kernarium_sms_listener_exec"),
            ("/(system_ext|system/system_ext)/bin/kernarium_sms_wake\\.sh", "kernarium_sms_wake_exec"),
            ("/dev/socket/kernarium_sms", "kernarium_sms_socket"))
        if labels.get(variant or "") != label)
    # What each variant adds: one defect.
    if variant == "listener-through-attribute":
        ext += ("(typeattributeset base_typeattr_77 (untrusted_app))\n"
                "(allow base_typeattr_77 self (vsock_socket (create bind listen accept)))\n")
    elif variant == "creator-by-name":
        plat += "(allow hal_audio_default self (vsock_socket (create bind connect)))\n"
    elif variant == "listener-without-the-holder":
        ext = ext.replace("(allow kernarium_sms_listener self (vsock_socket (create bind listen)))\n", "")
    elif variant == "service-creates-vsock":
        ext += "(allow kernarium_sms self (vsock_socket (create bind listen)))\n"
    elif variant == "service-accepts-on-its-own":
        ext += "(allow kernarium_sms self (vsock_socket (accept read write)))\n"
    elif variant == "service-holds-more-on-the-holder":
        ext += "(allow kernarium_sms kernarium_sms_listener (vsock_socket (connect)))\n"
    elif variant == "holder-sockets-through-domain":
        ext += "(allow domain kernarium_sms_listener (vsock_socket (read write)))\n"
    elif variant == "holder-sockets-by-another-domain":
        ext += "(allow system_server kernarium_sms_listener (vsock_socket (read)))\n"
    elif variant == "holder-connects-out":
        ext += "(allow kernarium_sms_listener virtualizationservice (vsock_socket (getattr)))\n"
    elif variant and variant.startswith("holder-holds-"):
        ext += f"(allow kernarium_sms_listener self (vsock_socket ({variant.removeprefix('holder-holds-')})))\n"
    elif variant == "holder-app-domain":
        ext += "(typeattributeset appdomain (kernarium_sms_listener))\n"
    elif variant == "waker-with-vsock":
        ext += "(allow kernarium_sms_wake self (vsock_socket (create)))\n"
    elif variant == "waker-on-the-holder":
        ext += "(allow kernarium_sms_wake kernarium_sms_listener (vsock_socket (getattr)))\n"
    elif variant == "waker-finds-another-service":
        ext += "(allow kernarium_sms_wake window_service (service_manager (find)))\n"
    elif variant == "waker-finds-nothing":
        ext = ext.replace("(allow kernarium_sms_wake activity_service (service_manager (find)))\n", "")
    elif variant in ("holder-binder-through-a-group", "holder-finds-a-service-through-a-group",
                     "holder-capability-through-a-group", "holder-vendor-binder-device"):
        # The holder in a group of the policy's own, which an allow rule names in place of the holder.
        ext += "(typeattributeset base_typeattr_93 (kernarium_sms_listener))\n" + {
            "holder-binder-through-a-group": "(allow base_typeattr_93 servicemanager (binder (call)))\n",
            "holder-finds-a-service-through-a-group": "(allow base_typeattr_93 activity_service (service_manager (find)))\n",
            "holder-capability-through-a-group": "(allow base_typeattr_93 self (capability (net_admin)))\n",
            "holder-vendor-binder-device": "(allow base_typeattr_93 vndbinder_device (chr_file (read write open)))\n",
        }[variant]
    elif variant == "holder-hwservice-through-domain":
        plat += "(allow domain hal_foo_hwservice (hwservice_manager (find)))\n"
    elif variant in ("waker-finds-another-service-through-a-group", "waker-hwservice-through-a-group"):
        ext += "(typeattributeset base_typeattr_94 (kernarium_sms_wake))\n" + {
            "waker-finds-another-service-through-a-group": "(allow base_typeattr_94 window_service (service_manager (find)))\n",
            "waker-hwservice-through-a-group": "(allow base_typeattr_94 hal_foo_hwservice (hwservice_manager (find)))\n",
        }[variant]
    elif variant in ("control-stop-by-another", "control-start-by-another", "control-sigstop-by-another"):
        plat += "(allow system_app ctl_" + variant.split("-")[1] + "_prop (property_service (set)))\n"
    elif variant == "control-restart-through-attribute":
        plat += ("(typeattributeset base_typeattr_92 (shell))\n"
                 "(allow base_typeattr_92 ctl_restart_prop (property_service (set)))\n")
    elif variant == "control-locked-set-by-another":
        ext += "(allow system_app ctl_kernarium_sms_locked_prop (property_service (set)))\n"
    elif variant == "control-locked-set-by-the-holder":
        ext += "(allow kernarium_sms_listener ctl_kernarium_sms_locked_prop (property_service (set)))\n"
    elif variant == "control-stop-by-the-holder":
        ext += "(allow kernarium_sms_listener ctl_stop_prop (property_service (set)))\n"
    elif variant == "holder-calls-binder":
        ext += "(allow kernarium_sms_listener servicemanager (binder (call)))\n"
    elif variant == "holder-finds-a-service":
        ext += "(allow kernarium_sms_listener activity_service (service_manager (find)))\n"
    elif variant == "holder-binder-device-widened":
        ext += ("(typeattributeset base_typeattr_93 (kernarium_sms_listener))\n"
                "(allow base_typeattr_93 binder_device (chr_file (setattr)))\n")
    elif variant == "holder-hwbinder-device-widened":
        ext += ("(typeattributeset base_typeattr_93 (kernarium_sms_listener))\n"
                "(allow base_typeattr_93 hwbinder_device (chr_file (execute)))\n")
    elif variant == "waker-binder-device-widened":
        ext += "(allow kernarium_sms_wake binder_device (chr_file (unlink)))\n"
    elif variant == "waker-hwbinder-device-widened":
        ext += "(allow kernarium_sms_wake hwbinder_device (chr_file (relabelto)))\n"
    elif variant == "waker-binder-to-another":
        ext += "(allow kernarium_sms_wake surfaceflinger (binder (call)))\n"
    elif variant == "waker-binder-widened":
        ext += "(allow kernarium_sms_wake servicemanager (binder (set_context_mgr)))\n"
    elif variant == "waker-binder-through-a-group":
        ext += ("(typeattributeset base_typeattr_94 (kernarium_sms_wake))\n"
                "(allow base_typeattr_94 system_app (binder (call)))\n")
    elif variant == "su-binder-widened":
        ext += ("(typeattributeset base_typeattr_93 (kernarium_sms_listener))\n"
                "(allow base_typeattr_93 su (binder (impersonate)))\n")
    elif variant == "holder-owns-files":
        ext += "(allow kernarium_sms_listener system_data_file (dir (search)))\n"
    elif variant == "holder-entered-by-another":
        ext += "(typetransition shell kernarium_sms_listener_exec process kernarium_sms_listener)\n"
    elif variant == "holder-entered-through-another-file":
        ext += "(typetransition init system_file process kernarium_sms_listener)\n"
    elif variant == "holder-never-entered":
        ext = ext.replace("(typetransition init kernarium_sms_listener_exec process kernarium_sms_listener)\n", "")
    elif variant == "waker-entered-by-another":
        ext += "(typetransition shell kernarium_sms_wake_exec process kernarium_sms_wake)\n"
    elif variant == "waker-transition-by-another":
        ext += "(allow shell kernarium_sms_wake (process (transition)))\n"
    elif variant == "handoff-connect-by-another":
        ext += "(allow shell kernarium_sms_listener (unix_stream_socket (connectto)))\n"
    elif variant == "handoff-service-cannot-connect":
        ext = ext.replace("(allow kernarium_sms kernarium_sms_listener (unix_stream_socket (connectto)))\n", "")
    elif variant == "handoff-file-written-by-another":
        ext += "(allow untrusted_app kernarium_sms_socket (sock_file (write)))\n"
    elif variant == "handoff-holder-cannot-listen":
        ext = ext.replace("(unix_stream_socket (accept getattr getopt listen read write))",
                          "(unix_stream_socket (accept getattr getopt read write))")
    elif variant == "handoff-holder-cannot-accept":
        ext = ext.replace("(unix_stream_socket (accept getattr getopt listen read write))",
                          "(unix_stream_socket (getattr getopt listen read write))")
    elif variant == "handoff-holder-listens-by-its-own-name":
        # The same right, spelt with the holder's own name as the target instead of self: still the holder's.
        ext = ext.replace("(allow kernarium_sms_listener self (unix_stream_socket (accept getattr getopt listen read write)))",
                          "(allow kernarium_sms_listener self (unix_stream_socket (accept getattr getopt read write)))\n"
                          "(allow kernarium_sms_listener kernarium_sms_listener (unix_stream_socket (listen)))")
    elif variant == "handoff-service-cannot-write-the-file":
        ext = ext.replace("(allow kernarium_sms kernarium_sms_socket (sock_file (write)))\n", "")
    elif variant == "handoff-fd-use-by-another":
        ext += "(allow shell kernarium_sms_listener (fd (use)))\n"
    elif variant == "handoff-service-cannot-use-the-fd":
        ext = ext.replace("(allow kernarium_sms kernarium_sms_listener (fd (use)))\n", "")
    elif variant == "wake-property-by-another":
        ext += "(allow shell ctl_kernarium_sms_wake_prop (property_service (set)))\n"
    elif variant == "wake-property-through-attribute":
        ext += ("(typeattributeset base_typeattr_91 (shell))\n"
                "(allow base_typeattr_91 ctl_kernarium_sms_wake_prop (property_service (set)))\n")
    elif variant == "wake-property-by-nobody":
        ext = ext.replace("(allow kernarium_sms_listener ctl_kernarium_sms_wake_prop (property_service (set)))\n", "")
    elif variant == "sockets-through-domain":
        ext += "(allow domain kernarium_sms (vsock_socket (read write)))\n"
    elif variant == "dumpstate-reads":
        plat = plat.replace("(allow dumpstate domain (vsock_socket (getattr)))",
                            "(allow dumpstate domain (vsock_socket (getattr read)))")
    elif variant == "another-domain-getattr":
        plat += "(allow system_server domain (vsock_socket (getattr)))\n"
    elif variant == "dumpstate-getattr-over-a-smaller-group":
        plat = plat.replace("(allow dumpstate domain (vsock_socket (getattr)))",
                            "(allow dumpstate coredomain (vsock_socket (getattr)))")
    elif variant == "dumpstate-lookalike-source":
        plat = plat.replace("(allow dumpstate domain (vsock_socket (getattr)))",
                            "(allow dumpstate_299999 domain (vsock_socket (getattr)))")
    elif variant == "dumpstate-lookalike-target":
        plat = plat.replace("(allow dumpstate domain (vsock_socket (getattr)))",
                            "(typeattributeset domain_299999 (kernarium_sms))\n"
                            "(allow dumpstate domain_299999 (vsock_socket (getattr)))")
    elif variant == "dumpstate-getattr-of-ours":
        ext += "(allow dumpstate kernarium_sms (vsock_socket (getattr)))\n"
    elif variant == "dumpstate-getattr-repeated-outside-the-platform":
        ext += "(allow dumpstate domain (vsock_socket (getattr)))\n"
    elif variant == "outbound-to-a-group-with-the-service":
        ext += ("(typeattributeset base_typeattr_95 (kernarium_sms virtualizationservice))\n"
                "(allow kernarium_sms base_typeattr_95 (vsock_socket (getattr)))\n")
    elif variant == "inherited-find-service-only":
        plat += "(allow kernarium_sms credstore_service (service_manager (find)))\n"
    elif variant == "inherited-find-split-over-two-rules":
        plat += ("(typeattributeset base_typeattr_276 (kernarium_sms untrusted_app platform_app))\n"
                 "(typeattributeset base_typeattr_277 (kernarium_sms priv_app system_app))\n"
                 "(allow base_typeattr_276 credstore_service (service_manager (find)))\n"
                 "(allow base_typeattr_277 credstore_service (service_manager (find)))\n")
    elif variant in ("inherited-find-in-system-ext", "inherited-file-in-system-ext", "inherited-vsock-in-system-ext"):
        ext += inherited[{"find": "base_typeattr_275", "file": "base_typeattr_280",
                          "vsock": "base_typeattr_285"}[variant.split("-")[1]]]
    elif variant == "service-on-another-sockets":
        ext += "(allow kernarium_sms virtualizationservice (vsock_socket (read write)))\n"
    elif variant == "data-through-app-data-file-type":
        ext += "(allow untrusted_app app_data_file_type (file (read write open create)))\n"
    elif variant == "data-by-another-domain":
        ext += "(allow hal_audio_default kernarium_sms_data_file (file (getattr)))\n"
    elif variant == "property-through-attribute":
        plat += ("(typeattributeset base_typeattr_88 (shell))\n"
                 "(allow base_typeattr_88 system_prop (property_service (set)))\n")
    elif variant == "property-by-another-domain":
        plat += "(allow vendor_init system_prop (property_service (set)))\n"
    elif variant == "extra-service":
        ext += "(allow kernarium_sms window_service (service_manager (find)))\n"
    elif variant == "system-app-data":
        ext += "(allow kernarium_sms system_app_data_file (file (read)))\n"
    elif variant == "settings-in-the-domain":
        seapp_ext += "user=system seinfo=platform name=com.android.settings domain=kernarium_sms type=kernarium_sms_data_file\n"
    return {"plat": plat, "ext": ext, "seapp_plat": seapp_plat, "seapp_ext": seapp_ext,
            "property_context": property_context, "file_context": file_context}


_ORIENTATION_PROBE = None
ICONS_APK_ENTRY = "SYSTEM_EXT/app/KernariumIcons/KernariumIcons.apk"
ICONS_PERMISSION = "android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY"
# android.R.attr ids of the icon renderer's manifest attributes (the framework's attribute table gives them), which the
# compiled binary manifest carries beside the names.
ICONS_ATTRIBUTE_IDS = {
    "authorities": 0x01010018, "permission": 0x01010006, "isolatedProcess": 0x010103A9, "directBootAware": 0x01010505,
    "allowBackup": 0x01010280, "usesNonSdkApi": 0x0101058E, "persistent": 0x0101000D, "grantUriPermissions": 0x0101001B,
    "sharedUserId": 0x0101000B, "enabled": 0x0101000E, "readPermission": 0x01010007,
}
# The platform's seapp_contexts lines the icon renderer's two processes meet (a subset, in the platform's order).
ICONS_SEAPP_PLAT = (
    "user=_isolated domain=isolated_app levelFrom=user\n"
    "user=_isolated isIsolatedComputeApp=true domain=isolated_compute_app levelFrom=user\n"
    "user=_app seinfo=platform domain=platform_app_36 type=app_data_file levelFrom=user\n"
    "user=_app isPrivApp=true domain=priv_app_36 type=privapp_data_file levelFrom=user\n"
    "user=_app minTargetSdkVersion=37 domain=untrusted_app type=app_data_file levelFrom=all\n"
    "user=_app minTargetSdkVersion=34 domain=untrusted_app_34 type=app_data_file levelFrom=all\n"
    "user=_app minTargetSdkVersion=30 domain=untrusted_app_30 type=app_data_file levelFrom=all\n"
    "user=_app minTargetSdkVersion=29 domain=untrusted_app_29 type=app_data_file levelFrom=all\n"
    "user=_app domain=untrusted_app_25 type=app_data_file levelFrom=user\n"
    "user=_app minTargetSdkVersion=28 fromRunAs=true domain=runas_app levelFrom=all\n"
)


def icons_fixture(verifier: Path, variant: str | None) -> dict:
    """The icon renderer's part of a target-files fixture: its package with a compiled manifest, its line of the
    package signing table and the seapp_contexts lines its processes meet. A variant breaks one thing, in the way
    the verifier's check must catch: the baseline is the application as overlay/app_icons builds it."""
    global _ORIENTATION_PROBE
    if _ORIENTATION_PROBE is None:
        path = verifier.parents[3] / "tests" / "integration" / "android" / "orientation_probe_apk.py"
        spec = importlib.util.spec_from_file_location("orientation_probe_apk", path)
        _ORIENTATION_PROBE = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = _ORIENTATION_PROBE  # its dataclasses look their module up by name
        spec.loader.exec_module(_ORIENTATION_PROBE)
    binary_xml = _ORIENTATION_PROBE
    element = binary_xml.Element
    reference = binary_xml.Reference
    manifest = {"package": "com.kernarium.icons", "versionCode": 1}
    sdk = {"minSdkVersion": 36, "targetSdkVersion": 37}
    application = {"label": "Andarium icons", "allowBackup": False, "directBootAware": True, "usesNonSdkApi": True}
    provider = {"name": ".IconProvider", "authorities": "com.kernarium.icons", "exported": True,
                "permission": ICONS_PERMISSION, "directBootAware": True}
    service = {"name": ".IconRenderService", "exported": False, "isolatedProcess": True,
               "directBootAware": True}
    top, inside, providers = [], [], [provider]
    entries: dict[str, bytes | None] = {}
    certificate = "build/make/target/product/security/testkey.x509.pem"
    partition = "system_ext"
    apkcerts = True
    seapp_plat, seapp_ext = ICONS_SEAPP_PLAT, ""
    edits = {
        "package-changed": lambda: manifest.update(package="com.kernarium.other"),
        "shared-user": lambda: manifest.update(sharedUserId="android.uid.system"),
        "asks-a-permission": lambda: top.append(element("uses-permission", (("name", "android.permission.INTERNET"),))),
        "asks-a-permission-from-sdk-m": lambda: top.append(
            element("uses-permission-sdk-m", (("name", "android.permission.VIBRATE"),))),
        "asks-a-permission-from-sdk-23": lambda: top.append(
            element("uses-permission-sdk-23", (("name", "android.permission.VIBRATE"),))),
        "provider-enabled-by-reference": lambda: provider.update(enabled=reference(0x7F050001)),
        "provider-read-permission-by-reference": lambda: provider.update(readPermission=reference(0x7F050002)),
        "provider-class-changed": lambda: provider.update(name="com.kernarium.icons.BackupIconProvider"),
        "provider-class-of-another-package": lambda: provider.update(name="com.example.IconProvider"),
        "service-class-changed": lambda: service.update(name="com.kernarium.icons.IconRenderServiceTwo"),
        "service-app-zygote": lambda: service.update(useAppZygote=True),
        "has-an-activity": lambda: inside.append(element("activity", (("name", "com.kernarium.icons.Main"),))),
        "has-a-receiver": lambda: inside.append(element("receiver", (("name", "com.kernarium.icons.Boot"),))),
        "has-a-filter": lambda: inside.append(element("service", (("name", "com.kernarium.icons.Other"),), (
            element("intent-filter", (), (element("action", (("name", "x"),)),)),))),
        "second-provider": lambda: providers.append(dict(provider, name="com.kernarium.icons.Other", authorities="x")),
        "provider-not-exported": lambda: provider.update(exported=False),
        "provider-open": lambda: provider.pop("permission"),
        "provider-other-permission": lambda: provider.update(permission="android.permission.INTERACT_ACROSS_USERS"),
        "provider-grants-uris": lambda: provider.update(grantUriPermissions=True),
        "provider-disabled": lambda: provider.update(enabled=False),
        "provider-other-authority": lambda: provider.update(authorities="com.kernarium.icon"),
        "provider-not-direct-boot-aware": lambda: provider.pop("directBootAware"),
        "service-not-isolated": lambda: service.update(isolatedProcess=False),
        "service-exported": lambda: service.update(exported=True),
        "service-guarded": lambda: service.update(permission=ICONS_PERMISSION),
        "service-not-direct-boot-aware": lambda: service.pop("directBootAware"),
        "application-allows-backup": lambda: application.update(allowBackup=True),
        "application-not-direct-boot-aware": lambda: application.pop("directBootAware"),
        "application-closed-to-hidden-apis": lambda: application.pop("usesNonSdkApi"),
        "application-persistent": lambda: application.update(persistent=True),
        "no-target-sdk": lambda: sdk.pop("targetSdkVersion"),
    }
    if variant in edits:
        edits[variant]()
    elif variant == "privileged-copy":
        entries["SYSTEM_EXT/priv-app/KernariumIcons/KernariumIcons.apk"] = synthetic_apk(debuggable=False)
    elif variant == "missing":
        entries[ICONS_APK_ENTRY] = None
    elif variant == "not-an-apk":
        entries[ICONS_APK_ENTRY] = b"not a package"
    elif variant == "platform-key":
        certificate = "build/make/target/product/security/platform.x509.pem"
    elif variant == "platform-key-in-the-tree":
        certificate = "vendor/lineage/build/target/product/security/media.x509.pem"
    elif variant == "other-partition":
        partition = "system"
    elif variant == "no-signing-line":
        apkcerts = "other"
    elif variant == "no-signing-table":
        apkcerts = False
    elif variant == "seapp-by-name":
        seapp_ext = "user=_app name=com.kernarium.icons domain=untrusted_app type=app_data_file levelFrom=all\n"
    elif variant == "seapp-by-prefix":
        seapp_ext = "user=_app seinfo=default name=com.kernarium.* domain=kernarium_icons type=app_data_file levelFrom=all\n"
    elif variant == "seapp-app-domain-changed":
        seapp_plat = seapp_plat.replace("minTargetSdkVersion=37 domain=untrusted_app ", "minTargetSdkVersion=37 domain=platform_app ")
    elif variant == "seapp-privileged-domain":
        seapp_plat = seapp_plat.replace("minTargetSdkVersion=37 domain=untrusted_app ", "minTargetSdkVersion=37 domain=priv_app ")
    elif variant == "seapp-isolated-changed":
        seapp_plat = seapp_plat.replace("user=_isolated domain=isolated_app ", "user=_isolated domain=untrusted_app ")
    elif variant == "seapp-isolated-for-another-seinfo":
        seapp_plat = seapp_plat.replace("user=_isolated domain=isolated_app levelFrom=user\n",
                                        "user=_isolated seinfo=platform domain=isolated_app levelFrom=user\n")
    elif variant == "seapp-isolated-for-a-newer-target":
        seapp_plat = seapp_plat.replace("user=_isolated domain=isolated_app levelFrom=user\n",
                                        "user=_isolated minTargetSdkVersion=38 domain=isolated_app levelFrom=user\n")
    elif variant == "seapp-no-isolated":
        seapp_plat = seapp_plat.replace("user=_isolated domain=isolated_app levelFrom=user\n", "")
    elif variant is not None:
        fail(f"icons_fixture does not know the variant {variant}")
    ids = dict(binary_xml.ATTRIBUTES)
    ids.update(ICONS_ATTRIBUTE_IDS)
    components = [
        element("provider", tuple(item.items())) for item in providers
    ] + [element("service", tuple(service.items()))] + inside
    tree = element("manifest", tuple(manifest.items()), (
        element("uses-sdk", tuple(sdk.items())), *top, element("application", tuple(application.items()), tuple(components))))
    if ICONS_APK_ENTRY not in entries:
        package = io.BytesIO()
        with zipfile.ZipFile(package, "w") as apk:
            apk.writestr("AndroidManifest.xml", binary_xml.encode_manifest(tree, ids))
        entries[ICONS_APK_ENTRY] = package.getvalue()
    name = "KernariumIcons.apk" if apkcerts != "other" else "Other.apk"
    table = None if apkcerts is False else (
        f'name="{name}" certificate="{certificate}" private_key="{certificate.replace(".x509.pem", ".pk8")}" '
        f'partition="{partition}"\n'
        'name="Other.apk" certificate="build/make/target/product/security/platform.x509.pem" '
        'private_key="build/make/target/product/security/platform.pk8" partition="system"\n')
    return {"entries": entries, "apkcerts": table, "seapp_plat": seapp_plat, "seapp_ext": seapp_ext}


SMS_APK_ENTRY = "SYSTEM_EXT/app/KernariumSms/KernariumSms.apk"
SMS_ATTRIBUTE_IDS = {
    "allowBackup": 0x01010280, "persistent": 0x0101000D, "sharedUserId": 0x0101000B, "enabled": 0x0101000E,
    "permission": 0x01010006, "isolatedProcess": 0x010103A9, "directBootAware": 0x01010505,
    "foregroundServiceType": 0x01010599, "useAppZygote": 0x0101059E, "maxSdkVersion": 0x01010271,
}


def sms_service_fixture(verifier: Path, variant: str | None) -> dict[str, bytes | None]:
    """The text message service's files in a target-files fixture: the holder's binary, the init script of the
    holder and the waker, the waker's script and the application with its manifest, as overlay/sms and
    overlay/sms/listener build them. A variant breaks one thing, in the way check_sms_service_files must catch;
    None for a path leaves it out."""
    global _ORIENTATION_PROBE
    if _ORIENTATION_PROBE is None:
        path = verifier.parents[3] / "tests" / "integration" / "android" / "orientation_probe_apk.py"
        spec = importlib.util.spec_from_file_location("orientation_probe_apk", path)
        _ORIENTATION_PROBE = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = _ORIENTATION_PROBE  # its dataclasses look their module up by name
        spec.loader.exec_module(_ORIENTATION_PROBE)
    binary_xml = _ORIENTATION_PROBE
    element = binary_xml.Element
    listener = verifier.parents[1] / "overlay/sms/listener"
    rc = (listener / "kernarium_sms_listener.rc").read_text()
    script = (listener / "kernarium_sms_wake.sh").read_text()
    rc_edits = {
        "rc-missing-holder": ("service kernarium_sms_listener ", "service other_service "),
        "rc-holder-other-executable": ("/system_ext/bin/kernarium_sms_listener\n", "/system_ext/bin/other\n"),
        "rc-holder-not-in-main": ("    class main\n", "    class late_start\n"),
        "rc-holder-as-root": ("    user nobody\n", "    user root\n"),
        "rc-holder-without-a-group": ("    group nobody\n", ""),
        "rc-holder-in-group-root": ("    group nobody\n", "    group root\n"),
        "rc-holder-extra-group": ("    group nobody\n", "    group nobody system\n"),
        "rc-holder-without-the-socket": ("    socket kernarium_sms stream 0660 system system\n", ""),
        "rc-holder-socket-open": ("socket kernarium_sms stream 0660 system system", "socket kernarium_sms stream 0666 system system"),
        "rc-holder-socket-for-shell": ("socket kernarium_sms stream 0660 system system", "socket kernarium_sms stream 0660 shell shell"),
        "rc-holder-seclabel": ("    user nobody\n", "    user nobody\n    seclabel u:r:init:s0\n"),
        "rc-holder-capability": ("    user nobody\n", "    user nobody\n    capabilities NET_ADMIN\n"),
        "rc-holder-oneshot": ("    user nobody\n", "    user nobody\n    oneshot\n"),
        "rc-missing-waker": ("service kernarium_sms_wake ", "service other_wake "),
        "rc-waker-other-executable": ("/system_ext/bin/kernarium_sms_wake.sh\n", "/system_ext/bin/other.sh\n"),
        "rc-waker-as-root": ("    user system\n    group system\n", "    user root\n    group shell\n"),
        "rc-waker-not-oneshot": ("    oneshot\n", ""),
        "rc-waker-not-disabled": ("    disabled\n", ""),
        "rc-waker-capability": ("    disabled\n", "    disabled\n    capabilities SYS_ADMIN\n"),
    }
    if variant in rc_edits:
        old, new = rc_edits[variant]
        if old not in rc:
            fail(f"the text message init script lacks '{old}' for the {variant} variant")
        rc = rc.replace(old, new)
    elif variant == "rc-trigger":
        rc += "\non property:sys.boot_completed=1\n    start kernarium_sms_wake\n"
    elif variant == "rc-third-service":
        rc += "\nservice kernarium_sms_other /system_ext/bin/other\n    class main\n"
    script_edits = {
        "script-other-command": ("cmd activity start-service", "cmd activity start-foreground-service"),
        "script-other-component": ("android.kernarium.sms/.HostService", "android.kernarium.sms/.Other"),
        "script-other-user": ("--user 0", "--user all"),
        "script-keeps-output": (">/dev/null 2>&1", ""),
    }
    if variant in script_edits:
        old, new = script_edits[variant]
        if old not in script:
            fail(f"the waker's script lacks '{old}' for the {variant} variant")
        script = script.replace(old, new)
    elif variant == "script-extra-command":
        script += "setprop sys.kernarium.woke 1\n"
    manifest = {"package": "android.kernarium.sms", "sharedUserId": "android.uid.system"}
    application = {"label": "Andarium SMS", "allowBackup": False}
    service = {"name": ".HostService", "exported": False}
    # The build injects <uses-sdk> into every packaged manifest (the manifest fixer of the Soong application rule): the
    # SDK levels of the platform it was built with, which a platform application takes as codenames or numbers.
    uses_sdk = {"minSdkVersion": 37, "targetSdkVersion": 37}
    top, inside, services, children = [], [], [service], []
    edits = {
        "apk-uses-sdk-codenames": lambda: uses_sdk.update(minSdkVersion="CANARY", targetSdkVersion="CANARY"),
        "apk-uses-sdk-max": lambda: uses_sdk.update(maxSdkVersion=40),
        "apk-uses-sdk-twice": lambda: top.append(element("uses-sdk", tuple(uses_sdk.items()))),
        "apk-uses-sdk-named": lambda: uses_sdk.update(name="android.permission.INTERNET"),
        "apk-uses-sdk-permission": lambda: uses_sdk.update(permission="android.permission.DUMP"),
        "apk-other-package": lambda: manifest.update(package="android.kernarium.other"),
        "apk-no-shared-user": lambda: manifest.pop("sharedUserId"),
        "apk-persistent": lambda: application.update(persistent=True),
        "apk-persistent-false": lambda: application.update(persistent=False),
        "apk-application-class": lambda: application.update(name=".HostChannel"),
        "apk-service-exported": lambda: service.update(exported=True),
        "apk-service-exported-unset": lambda: service.pop("exported"),
        "apk-service-other-class": lambda: service.update(name=".Other"),
        "apk-service-guarded": lambda: service.update(permission="android.permission.DUMP"),
        "apk-service-own-process": lambda: service.update(process=":other"),
        "apk-service-isolated": lambda: service.update(isolatedProcess=True),
        "apk-service-disabled": lambda: service.update(enabled=False),
        "apk-service-direct-boot-aware": lambda: service.update(directBootAware=True),
        "apk-service-foreground": lambda: service.update(foregroundServiceType="dataSync"),
        "apk-second-service": lambda: services.append({"name": ".Second", "exported": False}),
        "apk-has-an-activity": lambda: inside.append(element("activity", (("name", ".Main"),))),
        "apk-has-a-receiver": lambda: inside.append(element("receiver", (("name", ".Boot"),))),
        "apk-has-a-provider": lambda: inside.append(element("provider", (("name", ".Data"),))),
        "apk-asks-a-permission": lambda: top.append(element("uses-permission", (("name", "android.permission.INTERNET"),))),
        "apk-service-filter": lambda: children.append(
            element("intent-filter", (), (element("action", (("name", "x"),)),))),
    }
    if variant in edits:
        edits[variant]()
    elif variant is not None and variant.startswith(("apk-", "rc-", "script-")) and variant not in rc_edits \
            and variant not in script_edits and variant not in ("rc-trigger", "rc-third-service", "script-extra-command",
                                                                 "apk-not-an-apk", "apk-no-service"):
        fail(f"sms_service_fixture does not know the variant {variant}")
    ids = dict(binary_xml.ATTRIBUTES)
    ids.update(SMS_ATTRIBUTE_IDS)
    components = tuple(element("service", tuple(item.items()), tuple(children) if index == 0 else ())
                       for index, item in enumerate([] if variant == "apk-no-service" else services)) + tuple(inside)
    tree = element("manifest", tuple(manifest.items()), (
        element("uses-sdk", tuple(uses_sdk.items())), *top, element("application", tuple(application.items()), components)))
    package = io.BytesIO()
    with zipfile.ZipFile(package, "w") as apk:
        apk.writestr("AndroidManifest.xml", binary_xml.encode_manifest(tree, ids))
    return {
        SMS_APK_ENTRY: b"not a package" if variant == "apk-not-an-apk" else package.getvalue(),
        "SYSTEM_EXT/bin/kernarium_sms_listener": synthetic_aarch64_elf(),
        "SYSTEM_EXT/bin/kernarium_sms_wake.sh": script.encode(),
        "SYSTEM_EXT/etc/init/kernarium_sms_listener.rc": rc.encode(),
    }


def tracked_fstab(android_root: Path) -> str:
    """The UML fstab the product installs, which the verifiers compare against."""
    return (android_root / "overlay/config/fstab.uml").read_text()


def ext4_system_fstab(fstab: str) -> str:
    """`fstab` with /system declared ext4, which the compressed-EROFS contract forbids."""
    changed = fstab.replace("/system erofs", "/system ext4", 1)
    if changed == fstab:
        fail("the tracked fstab has no erofs /system line to corrupt")
    return changed


def tracked_erofs_parameters(android_root: Path) -> tuple[str, str]:
    """The compressor and the cluster size the product makefile sets, once each."""
    text = (android_root / "overlay/lineage_andarium.mk").read_text()
    found = []
    for name in ("UML_ANDROID_EROFS_COMPRESSOR", "UML_ANDROID_EROFS_PCLUSTER_SIZE"):
        values = re.findall(rf"^{name} := (\S+)$", text, re.MULTILINE)
        if len(values) != 1:
            fail(f"lineage_andarium.mk does not set {name} exactly once")
        found.append(values[0])
    return found[0], found[1]


def synthetic_resource_table(
    arrays: list[tuple[str, list[str]]], *, utf8: bool = True,
    booleans: list[tuple[str, bool]] = (), compact: bool = False,
    integers: list[tuple[str, int]] = (), overlayable: bool = False, qualified: frozenset[str] = frozenset(),
    strings: list[tuple[str, str | None]] = (),
) -> bytes:
    """A compiled resource table whose one package holds the given string-array resources, bool
    resources, integer resources and plain string resources (the bools and integers as compact entries
    when `compact` is set, as a recent aapt2 writes them), and an <overlayable> section when
    `overlayable` is set. A plain string whose text is None has a reference to another resource as its
    value, as `<string name=...>@string/other</string>` compiles. The resources named in `qualified`
    are defined under a resource configuration (portrait), not by default."""

    def pool(strings: list[str]) -> bytes:
        data, offsets = b"", []
        for text in strings:
            offsets.append(len(data))
            if utf8:
                raw = text.encode()
                data += bytes([len(text), len(raw)]) + raw + b"\x00"
            else:
                data += struct.pack("<H", len(text)) + text.encode("utf-16-le") + b"\x00\x00"
        data += b"\x00" * (-len(data) % 4)
        start = 28 + 4 * len(strings)
        return (struct.pack("<HHIIIIII", 1, 28, start + len(data), len(strings), 0,
                            0x100 if utf8 else 0, start, 0)
                + struct.pack(f"<{len(strings)}I", *offsets) + data)

    values = sorted({item for _, items in arrays for item in items} | {text for _, text in strings if text})
    names = ([name for name, _ in arrays] + [name for name, _ in booleans] + [name for name, _ in integers]
             + [name for name, _ in strings])
    configuration = struct.pack("<I", 64) + b"\x00" * 60
    portrait = struct.pack("<I", 64) + b"\x00" * 8 + b"\x01" + b"\x00" * 51  # the orientation qualifier
    type_header = 20 + len(configuration)

    def type_chunk(type_id: int, entries: list[bytes], configuration: bytes = configuration) -> bytes:
        offsets, body = [], b""
        for entry in entries:
            offsets.append(len(body))
            body += entry
        return (struct.pack("<HHIBBHII", 0x0201, type_header,
                            type_header + 4 * len(entries) + len(body), type_id, 0, 0, len(entries),
                            type_header + 4 * len(entries))
                + configuration + struct.pack(f"<{len(entries)}I", *offsets) + body)

    array_entries = []
    for index, (_, items) in enumerate(arrays):
        entry = struct.pack("<HHIII", 16, 1, index, 0, len(items))
        for item in items:
            entry += struct.pack("<IHBBI", 0x02000001, 8, 0, 3, values.index(item))
        array_entries.append(entry)
    bool_entries = []
    for index, (_, value) in enumerate(booleans, start=len(arrays)):
        data = 0xFFFFFFFF if value else 0
        if compact:  # key in the size field; the value type in the flags' high byte; the data
            bool_entries.append(struct.pack("<HHI", index, 0x1200 | 0x08, data))
        else:
            bool_entries.append(struct.pack("<HHI", 8, 0, index) + struct.pack("<HBBI", 8, 0, 0x12, data))
    integer_entries = []
    for index, (_, value) in enumerate(integers, start=len(arrays) + len(booleans)):
        data = value & 0xFFFFFFFF
        if compact:
            integer_entries.append(struct.pack("<HHI", index, 0x1000 | 0x08, data))
        else:
            integer_entries.append(struct.pack("<HHI", 8, 0, index) + struct.pack("<HBBI", 8, 0, 0x10, data))

    string_entries = []
    for index, (_, text) in enumerate(strings, start=len(arrays) + len(booleans) + len(integers)):
        value = (struct.pack("<HBBI", 8, 0, 0x03, values.index(text)) if text is not None
                 else struct.pack("<HBBI", 8, 0, 0x01, 0x7F0A0001))  # TYPE_STRING, or TYPE_REFERENCE
        string_entries.append(struct.pack("<HHI", 8, 0, index) + value)

    def configured(type_id: int, entries: list[bytes], entry_names: list[str], *, always: bool) -> bytes:
        """The type's chunk of default definitions and, when some are qualified, a chunk of those."""
        plain = [entry for entry, name in zip(entries, entry_names) if name not in qualified]
        under = [entry for entry, name in zip(entries, entry_names) if name in qualified]
        chunks = type_chunk(type_id, plain) if plain or (always and not under) else b""
        return chunks + (type_chunk(type_id, under, portrait) if under else b"")

    chunks = configured(1, array_entries, [name for name, _ in arrays], always=True)
    if overlayable:  # the <overlayable> section: its name and actor (256 UTF-16 units each) and no policy
        chunks += struct.pack("<HHI", 0x0204, 1032, 1032) + b"\x00" * 1024
    type_names = ["array"]
    for type_name, typed, typed_names in (("bool", bool_entries, [name for name, _ in booleans]),
                                          ("integer", integer_entries, [name for name, _ in integers]),
                                          ("string", string_entries, [name for name, _ in strings])):
        if typed:
            type_names.append(type_name)
            chunks += configured(len(type_names), typed, typed_names, always=False)
    type_pool, key_pool = pool(type_names), pool(names)
    package_header = 288
    package = (struct.pack("<HHII", 0x0200, package_header,
                           package_header + len(type_pool) + len(key_pool) + len(chunks), 0x7F)
               + b"\x00" * 256
               + struct.pack("<IIIII", package_header, 0, package_header + len(type_pool), 0, 0)
               + type_pool + key_pool + chunks)
    global_pool = pool(values)
    return struct.pack("<HHII", 2, 12, 12 + len(global_pool) + len(package), 1) + global_pool + package


def synthetic_apex(filesystem: str = "erofs", content: bytes = b"", **superblock) -> bytes:
    """An APEX container whose payload has the superblock of `filesystem` and then `content`."""
    if filesystem == "ext4":
        payload = bytearray(1024 + 0x38 + 2)
        payload[1024 + 0x38 : 1024 + 0x38 + 2] = b"\x53\xef"
    elif filesystem == "erofs":
        payload = bytearray(synthetic_erofs_image(**superblock))
    elif filesystem == "unknown":
        payload = bytearray(4096)
    else:
        fail(f"unsupported synthetic APEX filesystem: {filesystem}")
    payload += content
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as apex:
        apex.writestr("apex_payload.img", payload)
    return output.getvalue()


def synthetic_kernel_elf() -> bytearray:
    """Return a 4 KiB AArch64 ET_DYN kernel: R+X and R+W loads and a GNU stack."""
    payload = bytearray(64 + 56 * 3)
    payload[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHI", payload, 16, 3, 183, 1)
    struct.pack_into("<Q", payload, 32, 64)
    struct.pack_into("<HHH", payload, 52, 64, 56, 3)
    for index, (kind, flags, address, size, alignment) in enumerate(
        (
            (1, 5, 0, len(payload), 0x1000),
            (1, 6, 0x10000, len(payload), 0x1000),
            (0x6474E551, 6, 0, 0, 16),
        )
    ):
        struct.pack_into(
            "<IIQQQQQQ", payload, 64 + 56 * index,
            kind, flags, 0, address, address, size, size, alignment,
        )
    return payload


def verify_kernel_elf_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-kernel-elf-") as temporary:
        kernel = Path(temporary) / "vmlinux"

        def verify(payload: bytes) -> subprocess.CompletedProcess:
            kernel.write_bytes(payload)
            return run_tool(verifier, kernel, stderr=subprocess.PIPE)

        accepted = verify(synthetic_kernel_elf())
        if accepted.returncode != 0:
            fail(f"kernel ELF verifier rejected a valid kernel: {accepted.stderr}")
        load, data, stack = 64, 120, 176
        for layout, offset, value, diagnostic in (
            ("<B", 4, 1, "little-endian ELF64 image"),
            ("<B", 5, 2, "little-endian ELF64 image"),
            ("<B", 6, 0, "little-endian ELF64 image"),
            ("<H", 16, 2, "ET_DYN"),
            ("<H", 18, 62, "AArch64"),
            ("<H", 54, 32, "program-header"),
            ("<H", 56, 1000, "program-header"),
            ("<Q", load + 40, 1, "program header 0 has file size above memory size"),
            ("<Q", data + 8, 0x40000, "segment 1 is outside the file"),
            ("<I", stack + 4, 7, "GNU stack is executable"),
            ("<I", data + 4, 7, "load segment 1 is writable and executable"),
            ("<Q", load + 48, 0x800, "load segment 0 has invalid alignment 0x800"),
            ("<Q", load + 48, 0x6000, "load segment 0 has invalid alignment 0x6000"),
            ("<Q", data + 16, 0x10010, "alignment congruence"),
            ("<I", data, 0, "separated executable and writable"),
            ("<I", data + 4, 4, "separated executable and writable"),
        ):
            payload = synthetic_kernel_elf()
            struct.pack_into(layout, payload, offset, value)
            rejected = verify(payload)
            if rejected.returncode == 0 or (
                "Android kernel ELF verifier: " not in rejected.stderr
                or diagnostic not in rejected.stderr
            ):
                fail(
                    f"kernel ELF verifier did not reject with {diagnostic!r}: "
                    f"{rejected.stderr}"
                )


def adjust_archive(path: Path, entries: dict[str, bytes | None]) -> None:
    """Rewrite a fixture archive: drop the entries mapped to None and add or
    replace the others."""
    with zipfile.ZipFile(path) as source:
        kept = [
            (info.filename, source.read(info))
            for info in source.infolist()
            if info.filename not in entries
        ]
    with zipfile.ZipFile(path, "w") as archive:
        for name, payload in kept:
            archive.writestr(name, payload)
        for name, payload in entries.items():
            if payload is not None:
                archive.writestr(name, payload)


def corrupt_archive_member(path: Path, name: str) -> None:
    """Flips the last stored byte of a member, so that its CRC no longer matches (the archive is written
    uncompressed)."""
    with zipfile.ZipFile(path) as source:
        info = source.getinfo(name)
    data = bytearray(path.read_bytes())
    name_length, extra_length = struct.unpack_from("<HH", data, info.header_offset + 26)
    last = info.header_offset + 30 + name_length + extra_length + info.compress_size - 1
    data[last] ^= 0xFF
    path.write_bytes(bytes(data))


def verify_target_files_fixtures(verifier: Path) -> None:
    sys.path.insert(0, str(verifier.parent))
    from release_common import BUILD_VARIANTS, configured_build_variant
    import camera_layouts

    tool = load_tool(verifier)
    # Init's tokens: what the motion-property guard compares, and what it must not mistake for a command.
    for text, commands in (
        ('on boot\n    setprop ro.boot.kernarium.compass 1  # a host\n',
         [["on", "boot"], ["setprop", "ro.boot.kernarium.compass", "1"]]),
        ('setprop "ro.boot.kernarium.compass" 1\n', [["setprop", "ro.boot.kernarium.compass", "1"]]),
        ('"setprop" ro.boot.kernarium.orientation 1\n', [["setprop", "ro.boot.kernarium.orientation", "1"]]),
        ("setprop \\\n    ro.boot.kernarium.compass \\\n 1\n", [["setprop", "ro.boot.kernarium.compass", "1"]]),
        ("# setprop ro.boot.kernarium.compass 1\n", []),
        ('setprop persist.x "a b" #c\n', [["setprop", "persist.x", "a b"]]),
        ('setprop "$marker" 1\n', [["setprop", "$marker", "1"]]),
        ("setprop\r\n  ro.x 1\r\n", [["setprop"], ["ro.x", "1"]]),
    ):
        if tool.init_commands(text) != commands:
            fail(f"init_commands({text!r}) is {tool.init_commands(text)}, not {commands}")
    # The scan of a policy's CIL rules decodes nothing (a query decodes the names it compares, when it
    # compares them, so a rule it never reaches may hold any byte), and it takes the rule patterns of the
    # file, neverallow and allow rules: three groups (source, target, permissions) or four (with an
    # object class). A pattern of another count is refused, not mapped onto those.
    scanned = tool.cil_rules(b"(allow a b\xff (file (read \xff)))\n(allow c d (file (open)))\n",
                             tool.CIL_FILE_ALLOW_RULE)
    if scanned != ((b"a", b"b\xff", b"", (b"read", b"\xff")), (b"c", b"d", b"", (b"open",))):
        fail(f"the CIL rule scan of a file allow rule is {scanned}")
    scanned = tool.cil_rules(b"(neverallow a b (property_service (set \xff)))\n", tool.CIL_NEVERALLOW_RULE)
    if scanned != ((b"a", b"b", b"property_service", (b"set", b"\xff")),):
        fail(f"the CIL rule scan of a neverallow rule is {scanned}")
    for groups in (0, 1, 2, 5):
        odd = re.compile(rb"".join(rb"(\S+) " for _ in range(groups)))
        try:
            tool.cil_rules(b"x y\n", odd)
        except ValueError as refused:
            if str(odd.pattern) not in str(refused) or f"has {groups}" not in str(refused):
                fail(f"the CIL rule scan refuses a pattern of {groups} groups without saying so: {refused}")
        else:
            fail(f"the CIL rule scan takes a pattern of {groups} groups")
    # The camera HAL's grant on the composer's file descriptors, in the forms the compiled vendor policy has (the
    # attribute, and the six-digit version of a platform symbol) and in the near misses it must not accept.
    camera_fence = b"(allow hal_camera_default hal_graphics_composer (fd (use)))"
    for accepted, line in (
        ("the rule", camera_fence),
        ("more permissions", camera_fence.replace(b"(use)", b"(use getattr)")),
        ("versioned names", b"(allow hal_camera_default_202604 hal_graphics_composer_202604 (fd (use)))"),
    ):
        if not tool.CAMERA_HAL_COMPOSER_FENCE.search(b"(type x)\n" + line + b"\n"):
            fail(f"the camera fence check rejects {accepted}")
    for rejected, line in (
        ("another numeric source symbol", camera_fence.replace(b"hal_camera_default ", b"hal_camera_default_2 ")),
        ("another numeric target symbol", camera_fence.replace(b"hal_graphics_composer ", b"hal_graphics_composer_2 ")),
        ("a five-digit source suffix", camera_fence.replace(b"hal_camera_default ", b"hal_camera_default_20260 ")),
        ("a seven-digit target suffix", camera_fence.replace(b"composer ", b"composer_2026040 ")),
        ("a named source suffix", camera_fence.replace(b"hal_camera_default ", b"hal_camera_default_xx ")),
        ("the composer's type", camera_fence.replace(b"hal_graphics_composer ", b"hal_graphics_composer_default ")),
        ("the camera attribute", camera_fence.replace(b"hal_camera_default ", b"hal_camera ")),
        ("another permission", camera_fence.replace(b"(use)", b"(getattr)")),
        ("another class", camera_fence.replace(b"(fd (use))", b"(file (use))")),
        ("a dontaudit", camera_fence.replace(b"(allow", b"(dontaudit")),
        ("a neverallow", camera_fence.replace(b"(allow", b"(neverallow")),
        ("a comment", b";;" + camera_fence),
        ("two rules on a line", camera_fence + b" " + camera_fence),
    ):
        if tool.CAMERA_HAL_COMPOSER_FENCE.search(b"(type x)\n" + line + b"\n"):
            fail(f"the camera fence check accepts {rejected}")
    pinner_overlay_entry = tool.PINNER_OVERLAY
    wifi_overlay_entry = tool.WIFI_OVERLAY
    wifi_never = tool.WIFI_NEVER_DISABLED_THRESHOLD
    wifi_permanent, wifi_temporary = tool.WIFI_NO_INTERNET_THRESHOLDS
    statistics_apks = tool.STATISTICS_APKS
    wifi_tracker_apks = tool.WIFI_TRACKER_APKS
    # The Setup Wizard overlay's artwork, as the build would carry it: the raw animation unchanged and
    # the compiled vector holding its path data as strings.
    wizard_art_root = verifier.parents[1] / "overlay/overlay/packages/apps/SetupWizard/app/src/main/res"
    wizard_paths = re.findall(
        r'android:pathData="([^"]+)"', (wizard_art_root / "drawable/logo.xml").read_text(encoding="utf-8")
    )
    wizard_welcome = (wizard_art_root / "raw/lottie_welcome.json").read_bytes()
    # The two wizard scripts the overlay carries, as raw files reach the overlay: unchanged.
    wizard_scripts = {
        name: (wizard_art_root / "raw" / name).read_bytes() for name in tool.SETUP_WIZARD_FLOWS
    }
    owner_name, user_name = wizard_scripts
    launcher_apps = sorted(tool.LAUNCHER_APPS)
    launcher_dock = tool.LAUNCHER_DOCK
    launcher_grids = tool.LAUNCHER_GRIDS
    launcher_overlay_entry = tool.LAUNCHER_OVERLAY
    # The reader of compiled array resources sees the same items through either string pool.
    tables = [("config_ntpServers", ["none"]), ("config_autoTimeSourcesPriority", ["telephony", "external"]),
              ("config_ntpServers", ["ntp://ntp.example", "x"]), ("empty", [])]
    expected_arrays = {"config_ntpServers": [["none"], ["ntp://ntp.example", "x"]],
                       "config_autoTimeSourcesPriority": [["telephony", "external"]], "empty": [[]]}
    for utf8 in (True, False):
        if tool.resource_arrays(synthetic_resource_table(tables, utf8=utf8)) != expected_arrays:
            fail(f"the resource array reader misreads a {'UTF-8' if utf8 else 'UTF-16'} string pool")
    # ... and a bool resource, whether the table keeps it as a full or as a compact entry.
    flags = [("config_flipToScreenOffEnabled", False), ("other", True), ("config_flipToScreenOffEnabled", True)]
    for compact in (False, True):
        table = synthetic_resource_table(tables, booleans=flags, compact=compact)
        if tool.resource_booleans(table) != {"config_flipToScreenOffEnabled": [False, True], "other": [True]}:
            fail(f"the resource bool reader misreads {'compact' if compact else 'full'} entries")
        if tool.resource_arrays(table) != expected_arrays:
            fail(f"the resource array reader misreads a table that also holds {'compact' if compact else 'full'} bools")
    # ... and an integer resource, signed, in a table with or without bools, full or compact.
    numbers = [("config_threshold", 2**31 - 1), ("config_low", 1), ("config_threshold", 0), ("config_negative", -1)]
    expected_integers = {"config_threshold": [2**31 - 1, 0], "config_low": [1], "config_negative": [-1]}
    for compact in (False, True):
        for typed_booleans in ([], flags):
            table = synthetic_resource_table(tables, booleans=typed_booleans, integers=numbers, compact=compact)
            if tool.resource_integers(table) != expected_integers:
                fail(f"the resource integer reader misreads {'compact' if compact else 'full'} entries")
            if tool.resource_arrays(table) != expected_arrays:
                fail("the resource array reader misreads a table that also holds integers")
    # ... and an <overlayable> section is seen in a table that holds the other resource types, however they are stored.
    for compact in (False, True):
        for declared in (False, True):
            table = synthetic_resource_table(tables, booleans=flags, integers=numbers, compact=compact, overlayable=declared)
            if tool.resource_table_defines_overlayable(table) != declared:
                fail(f"the overlayable reader misreads a {'compact' if compact else 'full'} table "
                     f"{'with' if declared else 'without'} the section")
    # ... and which resources a table defines by default: the ones outside a resource configuration only.
    for compact in (False, True):
        table = synthetic_resource_table(
            tables, booleans=flags + [("only_portrait", True)], integers=numbers, compact=compact,
            qualified=frozenset({"only_portrait", "config_threshold", "config_ntpServers"}))
        if (tool.resource_default_names(table, "bool"), tool.resource_default_names(table, "integer"),
                tool.resource_default_names(table, "array")) != (
                {"config_flipToScreenOffEnabled", "other"}, {"config_low", "config_negative"},
                {"config_autoTimeSourcesPriority", "empty"}):
            fail(f"the resource reader misreads which {'compact' if compact else 'full'} entries are in the default configuration")
        if tool.resource_booleans(table)["only_portrait"] != [True]:
            fail("the resource bool reader drops a definition under a resource configuration")
    # A table that does not hold together is no table without the section: the header, the declared size and
    # every chunk's bounds are checked.
    good = synthetic_resource_table(tables, booleans=flags, overlayable=False)
    package_at = struct.unpack_from("<H", good, 2)[0] + struct.unpack_from("<I", good, struct.unpack_from("<H", good, 2)[0] + 4)[0]
    broken_tables = {
        "empty": b"",
        "text": b"not a resource table",
        "wrong type": struct.pack("<H", 3) + good[2:],
        "short header": struct.pack("<HHI", 2, 4, len(good)) + good[8:],
        "declared size beyond the data": good[:-4],
        "declared size below the header": good[:4] + struct.pack("<I", 8) + good[8:],
        "chunk without a header": good[:12] + struct.pack("<HHI", 1, 4, 100) + good[20:],
        "chunk past the table": good[:package_at] + struct.pack("<HHI", 0x0200, 288, len(good)) + good[package_at + 8:],
    }
    for label, broken in broken_tables.items():
        try:
            found = tool.resource_table_defines_overlayable(broken)
        except ValueError:
            continue
        fail(f"the overlayable reader reads a table with a {label} as one {'with' if found else 'without'} the section")
    if tool.resource_table_defines_overlayable(good):
        fail("the overlayable reader finds a section in the good table")
    # The attributes of a compiled manifest's <overlay> element, through either string pool: a string, a boolean, or none.
    for utf8 in (True, False):
        for target, static in (("org.example.app", True), ("org.example.app", False), (None, True), ("org.example.app", None)):
            expected = {key: value for key, value in (("targetPackage", target), ("isStatic", static)) if value is not None}
            found = tool.manifest_element_attributes(synthetic_overlay_manifest(target, static, utf8=utf8), "overlay")
            if found != expected:
                fail(f"the overlay manifest reader reads {found} for {expected}")
    if tool.manifest_element_attributes(synthetic_compiled_manifest(None), "overlay") is not None:
        fail("the overlay manifest reader finds an <overlay> element in a manifest without one")
    # The icon renderer's check reads every element of a compiled manifest and asks the seapp_contexts which domain the
    # platform gives the application's two processes.
    with zipfile.ZipFile(io.BytesIO(icons_fixture(verifier, None)["entries"][ICONS_APK_ENTRY])) as package:
        icons_elements = tool.manifest_elements(package.read("AndroidManifest.xml"))
    if [name for name, _ in icons_elements] != ["manifest", "uses-sdk", "application", "provider", "service"]:
        fail(f"the manifest reader finds the elements {[name for name, _ in icons_elements]}")
    icons_attributes = dict(icons_elements)
    if (icons_attributes["manifest"].get("package") != "com.kernarium.icons"
            or icons_attributes["uses-sdk"].get("targetSdkVersion") != 37
            or icons_attributes["application"].get("usesNonSdkApi") is not True
            or icons_attributes["application"].get("allowBackup") is not False
            or icons_attributes["provider"].get("permission") != ICONS_PERMISSION
            or icons_attributes["service"].get("isolatedProcess") is not True):
        fail(f"the manifest reader reads {icons_attributes}")
    try:
        tool.manifest_elements(b"not a manifest")
        fail("the manifest reader takes a file that is not compiled XML")
    except ValueError:
        pass
    seapp = tool.parse_seapp_contexts(ICONS_SEAPP_PLAT)
    for target, expected in ((38, "untrusted_app"), (37, "untrusted_app"), (36, "untrusted_app_34"), (34, "untrusted_app_34"),
                             (33, "untrusted_app_30"), (30, "untrusted_app_30"), (29, "untrusted_app_29"), (28, "untrusted_app_25")):
        found = tool.seapp_app_domain(seapp, "default", "com.kernarium.icons", target)
        if found != expected:
            fail(f"the seapp_contexts lookup gives an application of target SDK {target} the domain {found}, not {expected}")
    if tool.seapp_app_domain(seapp, "platform", "com.kernarium.icons", 37) != "platform_app_36":
        fail("the seapp_contexts lookup does not follow the seinfo")
    named = seapp + tool.parse_seapp_contexts(
        "user=_app name=com.kernarium.icons domain=named type=app_data_file\n"
        "user=_app name=com.vendor.* domain=prefixed type=app_data_file\n")
    for package_name, expected in (("com.kernarium.icons", "named"), ("com.kernarium.other", "untrusted_app"),
                                   ("com.vendor.app", "prefixed"), ("com.vendorx", "untrusted_app")):
        found = tool.seapp_app_domain(named, "default", package_name, 37)
        if found != expected:
            fail(f"the seapp_contexts lookup gives {package_name} the domain {found}, not {expected}")
    if tool.seapp_app_domain(tool.parse_seapp_contexts("user=_app isPrivApp=true domain=priv_app\n"
                                                       "user=_app isEphemeralApp=true domain=ephemeral_app\n"
                                                       "user=_app fromRunAs=true domain=runas_app\n"
                                                       "user=system domain=system_app\n"), "default", "x.y", 37) is not None:
        fail("the seapp_contexts lookup takes a line for privileged, ephemeral, run-as or system-user processes")
    if tool.seapp_isolated_domain(seapp, "default", "com.kernarium.icons", 37) != "isolated_app":
        fail("the seapp_contexts lookup does not give an isolated service isolated_app")
    if tool.seapp_isolated_domain(tool.parse_seapp_contexts("user=_isolated isIsolatedComputeApp=true domain=isolated_compute_app\n"),
                                  "default", "x.y", 37) is not None or tool.seapp_isolated_domain([], "default", "x.y", 37) is not None:
        fail("the seapp_contexts lookup takes a line for a shared isolated process, or invents one")
    if tool.seapp_isolated_domain(seapp + tool.parse_seapp_contexts("user=_isolated name=x.y domain=special\n"),
                                  "default", "x.y", 37) != "special":
        fail("the seapp_contexts lookup does not prefer a line by name")
    for lines, seinfo, target, expected in (
            ("user=_isolated seinfo=platform domain=isolated_platform\n", "default", 37, None),
            ("user=_isolated seinfo=platform domain=isolated_platform\n", "platform", 37, "isolated_platform"),
            ("user=_isolated minTargetSdkVersion=38 domain=newer\n", "default", 37, None),
            ("user=_isolated maxTargetSdkVersion=36 domain=older\n", "default", 37, None),
            ("user=_isolated seinfo=platform domain=by_info\nuser=_isolated domain=plain\n", "default", 37, "plain"),
            ("user=_isolated domain=plain\nuser=_isolated seinfo=default domain=by_info\n", "default", 37, "by_info")):
        found = tool.seapp_isolated_domain(tool.parse_seapp_contexts(lines), seinfo, "x.y", target)
        if found != expected:
            fail(f"the seapp_contexts lookup gives an isolated process of seinfo {seinfo} and target SDK {target} "
                 f"{found!r} for {lines!r}, not {expected!r}")
    for package_name, name, expected in (("a.b", ".C", "a.b.C"), ("a.b", "C", "a.b.C"), ("a.b", "x.y.C", "x.y.C"),
                                         ("a.b", "", None), ("a.b", None, None), ("a.b", 5, None)):
        if tool.manifest_component_class(package_name, name) != expected:
            fail(f"the manifest's component class for {name!r} in {package_name} is not {expected!r}")
    release_manifest = synthetic_compiled_manifest(None, debuggable=False)
    release_apk = synthetic_apk(debuggable=False)
    preopt_apks = sorted(tool.REQUIRED_PREOPT_APKS)
    plain_apks = sorted(
        entry for entry in tool.REQUIRED_ENTRIES
        if entry.endswith(".apk") and entry not in tool.REQUIRED_PREOPT_APKS
        and entry != pinner_overlay_entry
        and entry != wifi_overlay_entry  # written with its own resources
        and entry != tool.CAMERA_APP  # written with its own resources
        and entry != tool.ICONS_APK  # written with its own manifest
        and entry != SMS_APK_ENTRY  # written with its own manifest
    )
    arm_preopt_apks = sorted(tool.REQUIRED_ARM_PREOPT_APKS)
    uml_fstab = tracked_fstab(verifier.parents[1])
    erofs_compressor, erofs_pcluster_size = tracked_erofs_parameters(verifier.parents[1])
    erofs_compress_hints = (
        verifier.parents[1] / "overlay/config/erofs_compress_hints.txt"
    ).read_bytes()
    tracked_variant = configured_build_variant(verifier.parents[1])
    other_variant = next(name for name in BUILD_VARIANTS if name != tracked_variant)
    with tempfile.TemporaryDirectory(prefix="android-target-files-") as temporary:
        root = Path(temporary)
        (root / "kernel").mkdir()
        kernel = root / "kernel/vmlinux"
        kernel.write_bytes(synthetic_aarch64_elf())
        kernel_config = root / "kernel/config"
        # The options the virtual Wi-Fi and the EROFS mounts need, and the ways a configuration
        # can lack one.
        wifi_kernel_options = {
            "CONFIG_MACVLAN": "y", "CONFIG_CFG80211": "y", "CONFIG_VIRT_WIFI": "y",
            "CONFIG_EROFS_FS": "y", "CONFIG_EROFS_FS_ZIP": "y",
            "CONFIG_EROFS_FS_ZIP_ZSTD": "y",
        }

        def kernel_config_text(variant: str | None) -> str:
            options = dict(wifi_kernel_options)
            lines = []
            if variant is not None:
                kind, name = variant.rsplit(":", 1)
                if kind == "missing":
                    del options[name]
                elif kind == "module":
                    options[name] = "m"
                elif kind == "unset":
                    del options[name]
                    lines.append(f"# {name} is not set\n")
                else:
                    fail(f"unknown kernel configuration variant {variant}")
            lines.extend(f"{name}={value}\n" for name, value in options.items())
            return "CONFIG_PAGE_SIZE_4KB=y\n# CONFIG_MODULES is not set\n" + "".join(lines)

        kernel_config.write_text(kernel_config_text(None))
        kernel_release = root / "kernel/kernel.release"
        kernel_release.write_text("6.18.21-uml-fixture\n")
        kernel_options = [
            "--kernel-elf",
            str(kernel),
            "--kernel-config",
            str(kernel_config),
            "--kernel-release-file",
            str(kernel_release),
        ]

        video_service = tool.VIDEO_ENCODER_SERVICE
        video_codecs = tool.VIDEO_ENCODER_CODECS
        video_ueventd = tool.VIDEO_ENCODER_UEVENTD
        video_seccomp_default = tool.VIDEO_ENCODER_SECCOMP_DEFAULT
        video_seccomp_extended = tool.VIDEO_ENCODER_SECCOMP_EXTENDED
        good_video_properties = {
            **tool.VIDEO_ENCODER_PROPERTIES,
            **tool.VIDEO_DECODER_PROPERTIES,
        }
        good_video_rc = (
            f"service vendor.media.c2 /vendor/bin/hw/{video_service}\n"
            "    class hal\n"
            "    user mediacodec\n"
            "    group mediadrm drmrpc camera\n"
        )
        good_video_seccomp = "".join(
            f"{call}: 1\n"
            for call in ("read", *tool.VIDEO_ENCODER_SECCOMP_CALLS,
                         *tool.VIDEO_CRASH_HANDLER_SECCOMP_CALLS)
        )
        # The vendor file contexts as the builder emits them: the serial device, the label
        # of the network setup tool, and the label of the video codec service.
        good_net_setup_label = (
            "/vendor/bin/kernarium_net_setup  u:object_r:kernarium_net_setup_exec:s0\n"
        )
        good_vibrator_label = (
            "/(vendor|system/vendor)/bin/hw/android\\.hardware\\.vibrator-service\\.andarium"
            "  u:object_r:hal_vibrator_default_exec:s0\n"
        )
        good_gnss_label = (
            "/(vendor|system/vendor)/bin/hw/android\\.hardware\\.gnss-service\\.andarium"
            "  u:object_r:hal_gnss_default_exec:s0\n"
        )
        good_thermal_label = (
            "/(vendor|system/vendor)/bin/hw/android\\.hardware\\.thermal-service\\.andarium"
            "  u:object_r:hal_thermal_default_exec:s0\n"
        )
        good_camera_label = (
            "/vendor/bin/hw/android\\.hardware\\.camera\\.provider@2\\.7-service-google-lazy"
            " u:object_r:hal_camera_default_exec:s0\n"
        )
        good_gamepad_label = (
            "/vendor/bin/kernarium_gamepad  u:object_r:kernarium_gamepad_exec:s0\n"
        )
        good_video_contexts = (
            "/dev/hvc0  u:object_r:serial_device:s0\n"
            + good_net_setup_label
            + "/(vendor|system/vendor)/bin/hw/android\\.hardware\\.media\\.c2-service-v4l2"
            "  u:object_r:mediacodec_exec:s0\n"
            + good_vibrator_label
            + good_gnss_label
            + good_thermal_label
            + good_camera_label
            + good_gamepad_label
        )
        # The GNSS HAL: declared on every device by one vendor fragment, and started at boot with its
        # class.
        good_gnss_manifest = (
            '<manifest version="1.0" type="device"><hal format="aidl">'
            "<name>android.hardware.gnss</name><version>7</version>"
            "<fqname>IGnss/default</fqname></hal></manifest>"
        )
        good_gnss_rc = (
            "service vendor.gnss-andarium /vendor/bin/hw/android.hardware.gnss-service.andarium\n"
            "    interface aidl android.hardware.gnss.IGnss/default\n"
            "    class hal\n    user gps\n    group gps\n"
        )
        gnss_rc_path = "VENDOR/etc/init/android.hardware.gnss-service.andarium.rc"
        gnss_fragment_path = "VENDOR/etc/vintf/manifest/android.hardware.gnss-service.andarium.xml"
        # The host's cameras: the lazy provider (a service that the layouts' init file starts when cameraserver runs), its one
        # declaration, the HWL library that holds the V4L2 source (the names only route p0030 puts in
        # it), the characteristics template, and the layouts' files (the generator's own).
        camera_provider = "android.hardware.camera.provider@2.7-service-google-lazy"
        camera_provider_path = f"VENDOR/bin/hw/{camera_provider}"
        camera_rc_path = f"VENDOR/etc/init/{camera_provider}.rc"
        camera_fragment_path = "VENDOR/etc/vintf/manifest/android.hardware.camera.provider@2.7-service-google.xml"
        camera_library_path = "VENDOR/lib64/libgooglecamerahwl_impl.so"
        camera_template_path = "VENDOR/etc/config/emu_camera_v4l2.json"
        camera_init_path = f"VENDOR/etc/init/{camera_layouts.INIT_FILE}"
        camera_provider_init_path = f"SYSTEM_EXT/etc/init/{camera_layouts.PROVIDER_INIT_FILE}"
        camera_init = camera_layouts.installed()[camera_provider_init_path].decode()
        camera_service = camera_layouts.PROVIDER_SERVICE
        camera_with_cameras = [layout.name for layout in camera_layouts.all_layouts() if layout.has_front]
        camera_start_action = (f"\non {camera_layouts.provider_trigger(camera_with_cameras[0])}\n"
                               f"    {camera_layouts.PROVIDER_COMMAND} {camera_service}\n")
        camera_app_path = "PRODUCT/app/Aperture/Aperture.apk"
        camera_overlay_path = tool.CAMERA_APP_OVERLAY
        camera_app_package = tool.CAMERA_APP_PACKAGE
        good_camera_rc = (
            f"service vendor.camera-provider-2-7-google /vendor/bin/hw/{camera_provider}\n"
            "    interface aidl android.hardware.camera.provider.ICameraProvider/internal/0\n"
            "    oneshot\n    disabled\n    class hal\n    user system\n    group system\n"
            "    capabilities SYS_NICE\n"
        )
        good_camera_manifest = (
            '<manifest version="1.0" type="device"><hal format="aidl">'
            "<name>android.hardware.camera.provider</name><version>4</version>"
            "<interface><name>ICameraProvider</name><instance>internal/0</instance></interface>"
            "</hal></manifest>"
        )

        def camera_library(*markers: bytes) -> bytes:
            return synthetic_aarch64_elf() + b"".join(marker + b"\x00" for marker in markers)

        camera_layout_files = camera_layouts.installed()
        camera_front_path = "VENDOR/etc/permissions/sku_front/kernarium_camera.xml"
        camera_init_path = "VENDOR/etc/init/init.kernarium_camera.rc"
        good_camera_markers = tuple(tool.CAMERA_HWL_MARKERS)
        good_camera_template = json.dumps({"android.lens.facing": [1], "android.sensor.orientation": [90]})
        # The thermal HAL: declared on every device by one vendor fragment, started at boot with its
        # class. The default display configuration holds the one point that holds the render rate to
        # 30 Hz from the SEVERE status on.
        good_thermal_manifest = (
            '<manifest version="1.0" type="device"><hal format="aidl">'
            "<name>android.hardware.thermal</name><version>3</version>"
            "<fqname>IThermal/default</fqname></hal></manifest>"
        )
        good_thermal_rc = (
            "service vendor.thermal-andarium /vendor/bin/hw/android.hardware.thermal-service.andarium\n"
            "    interface aidl android.hardware.thermal.IThermal/default\n"
            "    class hal\n    user nobody\n    group system\n"
        )
        # The gamepad relay: a daemon that init starts with its class, in a domain of its own.
        good_gamepad_rc = (
            "service kernarium_gamepad /vendor/bin/kernarium_gamepad\n"
            "    class main\n    user nobody\n    group uhid\n"
        )
        gamepad_binary_path = "VENDOR/bin/kernarium_gamepad"
        gamepad_rc_path = "VENDOR/etc/init/kernarium_gamepad.rc"
        thermal_rc_path = "VENDOR/etc/init/android.hardware.thermal-service.andarium.rc"
        thermal_fragment_path = "VENDOR/etc/vintf/manifest/android.hardware.thermal-service.andarium.xml"
        # The device configuration of the host's keyboard and mouse (overlay/input), and where each lands.
        input_device_files = {
            "Vendor_1af4_Product_0002.kl": "VENDOR/usr/keylayout/Vendor_1af4_Product_0002.kl",
            "Vendor_1af4_Product_0003.idc": "VENDOR/usr/idc/Vendor_1af4_Product_0003.idc",
            "Vendor_1af4_Product_0004.idc": "VENDOR/usr/idc/Vendor_1af4_Product_0004.idc",
            "Vendor_1af4_Product_0005.idc": "VENDOR/usr/idc/Vendor_1af4_Product_0005.idc",
        }
        side_key_layout = (verifier.parents[1] / "overlay/input/Vendor_1af4_Product_0002.kl").read_text()
        display_config_path = "VENDOR/etc/displayconfig/default.xml"
        good_display_config = (
            '<?xml version="1.0" encoding="utf-8"?>\n<!-- comment -->\n<displayConfiguration>\n'
            "  <thermalThrottling>\n    <refreshRateThrottlingMap>\n"
            "      <refreshRateThrottlingPoint>\n        <thermalStatus>moderate</thermalStatus>\n"
            "        <refreshRateRange>\n          <minimum>0</minimum>\n          <maximum>60</maximum>\n"
            "        </refreshRateRange>\n      </refreshRateThrottlingPoint>\n"
            "      <refreshRateThrottlingPoint>\n        <thermalStatus>severe</thermalStatus>\n"
            "        <refreshRateRange>\n          <minimum>0</minimum>\n          <maximum>30</maximum>\n"
            "        </refreshRateRange>\n      </refreshRateThrottlingPoint>\n"
            "    </refreshRateThrottlingMap>\n  </thermalThrottling>\n</displayConfiguration>\n"
        )
        # The vibrator HAL: declared by the ODM manifest of the haptics SKU alone, and started by init
        # for that SKU alone.
        good_vibrator_manifest = (
            '<manifest version="1.0" type="device"><hal format="aidl">'
            "<name>android.hardware.vibrator</name><version>4</version>"
            "<fqname>IVibrator/default</fqname></hal></manifest>"
        )
        # What the motion sub-HAL's bytes hold: the log line that names both properties (its prefix is the
        # one the older runner reads) and the magnetometer's name.
        good_motion_orientation_text = b"Andarium motion: ro.boot.kernarium.orientation=%d, %zu sensors"
        good_motion_sub_hal_text = (
            good_motion_orientation_text + b", ro.boot.kernarium.compass=%d\0Host Magnetometer\0")
        good_vibrator_rc = (
            "service vendor.vibrator-andarium /vendor/bin/hw/android.hardware.vibrator-service.andarium\n"
            "    interface aidl android.hardware.vibrator.IVibrator/default\n"
            "    class hal\n    user nobody\n    group nobody\n    disabled\n\n"
            "on boot && property:ro.boot.product.hardware.sku=haptics\n"
            "    enable vendor.vibrator-andarium\n    start vendor.vibrator-andarium\n"
        )
        vibrator_rc_path = "VENDOR/etc/init/android.hardware.vibrator-service.andarium.rc"

        def software_update(name: str, hevc_rate: str, vp9_size: str, vp9_blocks: str) -> str:
            # The software decoders' 3840x2160 update (config/media_codecs_c2.xml).
            if name == "c2.android.vp9.decoder":
                return (
                    f'    <MediaCodec name="{name}" type="video/x-vnd.on2.vp9" update="true">\n'
                    f'      <Limit name="size" min="2x2" max="{vp9_size}"/>\n'
                    f'      <Limit name="block-count" range="{vp9_blocks}"/>\n'
                    '      <Limit name="blocks-per-second" range="1-2000000"/>\n'
                    "    </MediaCodec>\n"
                )
            return (
                f'    <MediaCodec name="{name}" type="video/hevc" update="true">\n'
                f'      <Limit name="blocks-per-second" range="{hevc_rate}"/>\n'
                "    </MediaCodec>\n"
            )

        def video_codec_list(*, decoders: tuple[str, ...] = (),
                             hardware_decoders: dict[str, str] | None = None,
                             decoder_features: tuple[str, ...] = (
                                 "adaptive-playback", "dynamic-color-aspects"),
                             encoders: dict[str, str] | None = None,
                             performance_points: bool = True,
                             measured_frame_rates: bool = True,
                             performance_point: str = "1920x1080 30-45",
                             decoder_performance_points: bool = True,
                             decoder_measured_frame_rates: bool = True,
                             decoder_performance_point: str = "3840x2160 49-55",
                             encoder_minimum_size: str = "64x64",
                             encoder_minimum_bitrate: str = "20000",
                             software_updates: tuple[str, ...] = (
                                 "c2.android.hevc.decoder", "c2.android.vp9.decoder"),
                             hevc_rate: str = "1-16711680",
                             vp9_size: str = "4096x4096",
                             vp9_blocks: str = "1-65536") -> bytes:
            encoders = video_codecs if encoders is None else encoders
            hardware_decoders = (tool.VIDEO_DECODER_CODECS if hardware_decoders is None
                                 else hardware_decoders)
            limits = (
                f'      <Limit name="size" min="{encoder_minimum_size}" max="4096x4096"/>\n'
                f'      <Limit name="bitrate" range="{encoder_minimum_bitrate}-60000000"/>\n'
                + (f'      <Limit name="performance-point-{performance_point.split()[0]}"'
                 f' range="{performance_point.split()[1]}"/>\n'
                 if performance_points else "")
                + ('      <Limit name="measured-frame-rate-1920x1080" range="31-45"/>\n'
                   if measured_frame_rates else "")
            )
            decoder_limits = (
                (f'      <Limit name="performance-point-{decoder_performance_point.split()[0]}"'
                 f' range="{decoder_performance_point.split()[1]}"/>\n'
                 if decoder_performance_points else "")
                + ('      <Limit name="measured-frame-rate-3840x2160" range="49-55"/>\n'
                   if decoder_measured_frame_rates else "")
            )
            return (
                "<MediaCodecs>\n"
                "  <Decoders>\n"
                + "".join(software_update(name, hevc_rate, vp9_size, vp9_blocks)
                          for name in software_updates)
                + "".join(f'    <MediaCodec name="{name}" type="{kind}">\n'
                          '      <Limit name="size" min="64x64" max="4096x4096"/>\n'
                          + decoder_limits
                          + "".join(f'      <Feature name="{feature}"/>\n'
                                    for feature in decoder_features)
                          + "    </MediaCodec>\n"
                          for name, kind in hardware_decoders.items())
                + "".join(f'    <MediaCodec name="{name}" type="video/avc"/>\n'
                          for name in decoders)
                + "  </Decoders>\n  <Encoders>\n"
                + "".join(f'    <MediaCodec name="{name}" type="{kind}">\n{limits}'
                          "    </MediaCodec>\n"
                          for name, kind in encoders.items())
                + "  </Encoders>\n</MediaCodecs>\n"
            ).encode()

        def write_fixture(path: Path, *, module: bool = False,
                          module_loader: bool = False, dlkm_image: bool = False,
                          dynamic_image: bool = False, ota_tool: bool = False,
                          virtual_ab: bool = False,
                          wrong_fstab: bool = False, stock_fstab: bool = False,
                          wrong_filesystem: bool = False,
                          misc_info_edits: dict[str, str | None] | None = None,
                          wrong_userdata_inodes: bool = False,
                          missing_userdata_quota: bool = False,
                          wrong_apex_filesystem: bool = False,
                          seriallogging: bool = False,
                          boot_control: bool = False,
                          context_hub: bool = False,
                          unsupported_hal: bool = False,
                          unsupported_feature: bool = False,
                          missing_multitouch_feature: str | None = None,
                          missing_location_feature: str | None = None,
                          missing_microphone_feature: bool = False,
                          missing_companion_feature: bool = False,
                          missing_midi_feature: bool = False,
                          missing_mic_policy: bool = False,
                          extra_input_device: bool = False,
                          wrong_kernel_config: bool = False,
                          kernel_config_variant: str | None = None,
                          wrong_kernel_release: bool = False,
                          missing_memfd_policy: bool = False,
                          missing_spl_policy: bool = False,
                          blocker_variant: str | None = None,
                          missing_blocker_binary: bool = False,
                          net_setup_variant: str | None = None,
                          missing_net_setup_binary: bool = False,
                          missing_net_library: bool = False,
                          net_policy_variant: str | None = None,
                          gamepad_policy_variant: str | None = None,
                          sms_policy_variant: str | None = None,
                          sms_service_variant: str | None = None,
                          wifi_network_variant: str | None = None,
                          wifi_hal_variant: str | None = None,
                          wifi_dirs_variant: str | None = None,
                          supplicant_variant: str | None = None,
                          missing_wifi_feature: bool = False,
                          wifi_feature_file: str | None = None,
                          missing_wifi_network_script: bool = False,
                          missing_adb_prop_context: bool = False,
                          missing_adb_prop_policy: bool = False,
                          core_sets_vendor_prop: bool = False,
                          missing_audio_binder_policy: bool = False,
                          missing_wifi_debug_policy: bool = False,
                          camera_fence_variant: str | None = None,
                          clearkey_memfd_variant: str | None = None,
                          oversize_platform_policy: bool = False,
                          media_codecs_variant: str | None = None,
                          system_ext_core_sets_vendor_prop: bool = False,
                          adb_prop_not_system: bool = False,
                          adb_prop_unreadable_by_vendor_init: bool = False,
                          lineage_policy: str | None = None,
                          missing_mediaserver_audit: bool = False,
                          missing_shell_density_audit: bool = False,
                          missing_virtio_console: bool = False,
                          unsupported_hvc: bool = False,
                          missing_mesa_gles: bool = False,
                          bad_mesa_gles_elf: bool = False,
                          missing_gallium_dependency: bool = False,
                          retired_egl_library: bool = False,
                          retired_dri_driver: bool = False,
                          vendor_hal_property: str | None = None,
                          system_other: bool = False,
                          missing_sdk_apex: bool = False,
                          removed_payload: bool = False,
                          missing_required_preopt: bool = False,
                          half_preopt_pair: bool = False,
                          wrong_audio_defaults: bool = False,
                          wrong_product_model: bool = False,
                          old_product_identity: bool = False,
                          wrong_vendor_identity: bool = False,
                          vendor_build_identity: str | None = None,
                          fingerprint_variant: str | None = None,
                          user_permissive_su: bool = False,
                          usb_debugging_default: bool = False,
                          adb_secure_variant: str | None = None,
                          missing_lockscreen_default: bool = False,
                          use_fiemap_variant: str | None = None,
                          lmkd_variant: str | None = None,
                          userdata_grow_variant: str | None = None,
                          userdata_grow_policy_variant: str | None = None,
                          discrete_ops_variant: str | None = None,
                          pinner_overlay_variant: str | None = None,
                          wifi_overlay_variant: str | None = None,
                          statistics_variant: str | None = None,
                          wizard_branding_variant: str | None = None,
                          navigation_variant: str | None = None,
                          launcher_variant: str | None = None,
                          easter_egg_variant: str | None = None,
                          accessibility_variant: str | None = None,
                          network_time_variant: str | None = None,
                          flip_variant: str | None = None,
                          reclaim_variant: str | None = None,
                          time_zone_variant: str | None = None,
                          keyboard_default_variant: str | None = None,
                          oldswitch_variant: str | None = None,
                          appshell_variant: str | None = None,
                          icons_variant: str | None = None,
                          wallpaper_variant: str | None = None,
                          cas_hal_variant: str | None = None,
                          adbd_variant: str | None = None,
                          services_jar_variant: str | None = None,
                          oldapp_variant: str | None = None,
                          input_flinger_variant: str | None = None,
                          network_stack_variant: str | None = None,
                          wifi_tracker_variant: str | None = None,
                          ota_certificates: str | None = None,
                          signed: bool = False,
                          adbd_launch_unreadable: bool = False,
                          adb_key_variant: str | None = None,
                          venus_policy_variant: str | None = None,
                          vendor_property_variant: str | None = None,
                          audio_ring_variant: str | None = None,
                          soc_variant: str | None = None,
                          board_variant: str | None = None,
                          video_properties: dict[str, str] | None = None,
                          missing_video_ueventd: bool = False,
                          mesa_gles_alignment: int = 16384,
                          elf_edit: tuple[str, int, int] | None = None,
                          abilist32: str = "armeabi-v7a,armeabi",
                          vendor_abilist32: str = "armeabi-v7a,armeabi",
                          zygote: str = "zygote64_32",
                          dex2oat64: bool = True,
                          arm_alignment: int = 4096,
                          arm_flags: int = 0x05000200,
                          arm_edit: tuple[str, int, int] | None = None,
                          missing_arm_gallium_dependency: bool = False,
                          camera_variant: str | None = None,
                          entries: dict[str, bytes | None] | None = None,
                          corrupt_member: str | None = None,
                          init_variant: str | None = None,
                          extra_entry: str | None = None,
                          build_variant: str = tracked_variant,
                          alignment: int = 16384) -> None:
            # The UML build's configuration and the packaged copy agree unless the
            # fixture is the wrong-configuration one, so that each variant is judged
            # on the options it lacks and not on a difference.
            kernel_config.write_text(kernel_config_text(kernel_config_variant))
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("BOOT/kernel", kernel.read_bytes())
                archive.writestr("INIT_BOOT/RAMDISK/init", b"init")
                archive.writestr("META/init_boot_filesystem_config.txt", "")
                archive.writestr(
                    "META/kernel_configs.txt",
                    "CONFIG_PAGE_SIZE_4KB=n\n"
                    if wrong_kernel_config
                    else kernel_config.read_bytes(),
                )
                archive.writestr(
                    "META/kernel_version.txt",
                    "6.18.21-wrong\n"
                    if wrong_kernel_release
                    else kernel_release.read_bytes(),
                )
                misc_info = {
                    "system_fs_type": "ext4" if wrong_filesystem else "erofs",
                    "system_ext_fs_type": "erofs",
                    "product_fs_type": "erofs",
                    "vendor_fs_type": "erofs",
                    "odm_fs_type": "erofs",
                    "erofs_default_compressor": erofs_compressor,
                    "erofs_pcluster_size": erofs_pcluster_size,
                    "erofs_default_compress_hints": "device/kernarium/andarium/config/erofs_compress_hints.txt",
                    "erofs_sparse_flag": "-s",
                    "userdata_extfs_inode_count": (
                        "32768" if wrong_userdata_inodes else "8192"
                    ),
                    "vintf_odm_manifest_skus": "haptics",
                }
                for key, value in (misc_info_edits or {}).items():
                    if value is None:
                        del misc_info[key]
                    else:
                        misc_info[key] = value
                archive.writestr(
                    "META/misc_info.txt",
                    "".join(f"{key}={value}\n" for key, value in misc_info.items()),
                )
                archive.writestr(
                    "META/erofs_default_compress_hints.txt", erofs_compress_hints
                )
                archive.writestr("META/ramdisk_node_list", "")
                for image in ("init_boot.img", "recovery.img", "userdata.img"):
                    archive.writestr(f"IMAGES/{image}", b"image")
                # AOSP writes the EROFS images sparse; one of them is here, to read both forms.
                for image in ("odm", "product", "system", "system_ext", "vendor"):
                    archive.writestr(
                        f"IMAGES/{image}.img",
                        synthetic_erofs_image(sparse=image == "product"),
                    )
                archive.writestr("ROOT/system_dlkm/", b"")
                archive.writestr("SYSTEM/system_dlkm/etc/build.prop", "")
                virtual_ab_property = "ro.virtual_ab.enabled=true\n" if virtual_ab else ""
                # The system partition is AOSP's generic system and says so; the product's
                # identity is in the product and vendor partitions and the public fingerprint.
                system_identity = {
                    "brand": "Android",
                    "manufacturer": "Android",
                    "device": "generic",
                    "name": "generic_system",
                    "model": "Generic System",
                }
                system_fingerprint = (
                    f"Android/generic_system/generic:17/FIXTURE/7:{build_variant}/release-keys"
                )
                product_identity = {
                    "brand": "Android" if old_product_identity else "Andarium",
                    "manufacturer": "Kernarium",
                    "device": "darwin" if old_product_identity else "andarium",
                    "name": "lineage_darwin" if old_product_identity else "lineage_andarium",
                    "model": "Darwin UML" if old_product_identity else "Andarium Virtual Device",
                }
                vendor_identity = dict(
                    product_identity,
                    **({"model": "Android 17 on Andarium"} if wrong_vendor_identity else {}),
                )
                public_fingerprint = (
                    f"{product_identity['brand']}/{product_identity['name']}/"
                    f"{product_identity['device']}:17/FIXTURE/7:{build_variant}/release-keys"
                )
                public_fingerprint_property = {
                    None: f"ro.build.fingerprint={public_fingerprint}\n",
                    "missing": "",
                    # The system partition's own fingerprint is not the public one.
                    "generic-system": f"ro.build.fingerprint={system_fingerprint}\n",
                    "wrong-type": "ro.build.fingerprint="
                    + public_fingerprint.replace(
                        f":{build_variant}/",
                        f":{'userdebug' if build_variant == 'user' else 'user'}/",
                    )
                    + "\n",
                }[fingerprint_variant]

                def abi_properties(partition: str, thirty_two: str) -> str:
                    abilist = "arm64-v8a" + ("," + thirty_two if thirty_two else "")
                    return (
                        f"ro.{partition}.product.cpu.abilist={abilist}\n"
                        f"ro.{partition}.product.cpu.abilist32={thirty_two}\n"
                        f"ro.{partition}.product.cpu.abilist64=arm64-v8a\n"
                    )

                archive.writestr(
                    "SYSTEM/build.prop",
                    "ro.product.cpu.abi=arm64-v8a\n"
                    + abi_properties("system", abilist32)
                    + "".join(
                        f"{name}={value}\n"
                        for name, value in BUILD_VARIANTS[build_variant].items()
                    )
                    + "".join(
                        f"ro.product.system.{key}={value}\n"
                        for key, value in system_identity.items()
                    )
                    + f"ro.system.build.fingerprint={system_fingerprint}\n"
                    + public_fingerprint_property
                    + (
                        f"ro.adb.secure={'0' if adb_secure_variant == 'system' else '1'}\n"
                        if build_variant == "user" else ""
                    )
                    + f"{virtual_ab_property}",
                )
                archive.writestr(
                    "PRODUCT/etc/build.prop",
                    "aaudio.mmap_policy=1\n"
                    "aaudio.mmap_exclusive_policy=1\n"
                    "ro.config.vc_call_vol_steps=15\n"
                    "ro.config.vc_call_vol_default=15\n"
                    f"ro.config.media_vol_default="
                    f"{'5' if wrong_audio_defaults else '15'}\n"
                    "ro.config.alarm_vol_default=7\n"
                    "ro.config.system_vol_default=7\n"
                    "ro.config.assistant_vol_default=15\n"
                    + ("persist.sys.usb.config=adb\n" if usb_debugging_default else "")
                    + {
                        "product": "ro.adb.secure=0\n",
                        "product-false": "ro.adb.secure=false\n",
                    }.get(adb_secure_variant, "")
                    + ("" if missing_lockscreen_default else "ro.lockscreen.disable.default=true\n")
                    + {
                        "property-missing": "",
                        "property-off": "ro.lmk.gpu_reclaim=false\n",
                    }.get(lmkd_variant, "ro.lmk.gpu_reclaim=true\n")
                    + ("ro.boot.vendor.overlay.theme=com.android.internal.systemui.navbar.gestural\n"
                       if navigation_variant == "overlay-theme-property" else "")
                    + {
                        "lazy-property-missing": "",
                        "lazy-property-off": "ro.camera.enableLazyHal=false\n",
                    }.get(camera_variant, "ro.camera.enableLazyHal=true\n")
                    + {
                        None: "apexd.config.use_fiemap=false\n",
                        "missing": "",
                        "true": "apexd.config.use_fiemap=true\n",
                    }[use_fiemap_variant]
                    + "".join(
                        f"ro.product.product.{key}={value}\n"
                        for key, value in dict(
                            product_identity,
                            **({"model": "Android 17 on Andarium"} if wrong_product_model else {}),
                        ).items()
                    ),
                )
                # lmkd with the GPU reclaim of route p0027: the names only that topic puts in it.
                if lmkd_variant != "missing-binary":
                    archive.writestr(
                        "SYSTEM/bin/lmkd",
                        b"\x00" * 8
                        + b"".join(
                            marker + b"\x00"
                            for variant, marker in (
                                ("no-property-name", b"ro.lmk.gpu_reclaim"),
                                ("no-report-path", b"/sys/class/drm/card0/gpu_memory_report"),
                                ("no-kill-reason", b"graphics memory pressure"),
                            )
                            if lmkd_variant != variant and lmkd_variant != "old-binary"
                        ),
                    )
                archive.writestr(
                    "SYSTEM/bin/android-fixture",
                    synthetic_aarch64_elf(alignment, edit=elf_edit),
                )
                for tool in ("e2fsck", "resize2fs", "sgdisk"):
                    archive.writestr(
                        f"SYSTEM/bin/{tool}",
                        synthetic_aarch64_elf(alignment),
                    )
                # The 32-bit runtime: the second zygote's process, the
                # bootstrap linker, the BoringSSL self test and the ART boot
                # image of the arm ISA.
                archive.writestr(
                    "SYSTEM/bin/app_process32",
                    synthetic_arm_elf(arm_alignment, edit=arm_edit, flags=arm_flags),
                )
                for entry in (
                    "SYSTEM/bin/bootstrap/linker",
                    "SYSTEM/bin/boringssl_self_test32",
                    "SYSTEM/framework/arm/boot.oat",
                    "SYSTEM/framework/arm/boot-framework.oat",
                ):
                    archive.writestr(entry, synthetic_arm_elf())
                archive.writestr(
                    "SYSTEM/etc/init/hw/init.zygote64_32.rc", "service zygote_secondary\n"
                )
                archive.writestr(
                    "SYSTEM/apex/com.android.fixture.apex",
                    synthetic_apex("ext4" if wrong_apex_filesystem else "erofs"),
                )
                # The presigned CTS shim keeps its checked-in ext4 payload.
                archive.writestr(
                    "SYSTEM/apex/com.android.apex.cts.shim.apex", synthetic_apex("ext4")
                )
                required_sdk_apexes = (
                    "com.android.bt",
                    "com.android.devicelock",
                    "com.android.nfcservices",
                    "com.android.rkpd",
                    "com.android.telephonycore",
                    "com.android.tethering",
                    "com.android.uwb",
                    "com.android.virt",
                    "com.android.wifi",
                )
                for index, apex in enumerate(required_sdk_apexes):
                    if missing_sdk_apex and index == 0:
                        continue
                    archive.writestr(
                        f"SYSTEM/apex/{apex}.apex", synthetic_apex()
                    )
                fstab = ext4_system_fstab(uml_fstab) if wrong_fstab else uml_fstab
                if missing_userdata_quota:
                    fstab = fstab.replace(",quota", "")
                archive.writestr("VENDOR/etc/fstab.uml", fstab)
                archive.writestr(
                    "VENDOR_BOOT/RAMDISK/first_stage_ramdisk/fstab.uml",
                    fstab,
                )
                # The hardware timeout multiplier the launcher passes, which the
                # copy defaults to 5 for a launch that passes none.
                timeout_copy = {
                    "missing-timeout-copy": "",
                    "timeout-copy-without-default": (
                        "    setprop ro.hw_timeout_multiplier "
                        "${ro.boot.hw_timeout_multiplier}\n"
                    ),
                }.get(
                    init_variant,
                    "    setprop ro.hw_timeout_multiplier "
                    "${ro.boot.hw_timeout_multiplier:-5}\n",
                )
                graphics = "".join(
                    f"    setprop ro.hardware.{name} ${{ro.boot.hardware.{name}}}\n"
                    for name in ("egl", "gralloc", "vulkan")
                    if not (init_variant == "missing-hal-copy" and name == "vulkan")
                )
                if init_variant != "missing-gles-copy":
                    graphics += "    setprop ro.opengles.version ${ro.boot.opengles.version}\n"
                # The Venus driver option that makes vkAllocateMemory wait for the host's reply.
                venus_sync = "    setprop debug.mesa.vn.perf no_async_mem_alloc\n"
                if init_variant not in ("missing-venus-sync", "venus-sync-at-boot", "venus-sync-other-value"):
                    graphics += venus_sync
                elif init_variant == "venus-sync-other-value":
                    graphics += "    setprop debug.mesa.vn.perf no_async_mem_alloc,log_ctx_info\n"
                if init_variant == "angle-copy":
                    graphics += (
                        "    setprop debug.angle.feature_overrides_enabled "
                        "${ro.boot.hardware.angle_feature_overrides_enabled}\n"
                    )
                density = (
                    "on early-init\n"
                    if init_variant == "ungated-density"
                    else "on early-init && property:ro.boot.lcd_density=*\n"
                ) + "    setprop ro.sf.lcd_density ${ro.boot.lcd_density}\n"
                # The restore of normal RCU grace periods, which belongs to
                # boot completion, follows the boot action's commands.
                # Swap goes on at the same point.
                swapon = "    swapon_all /vendor/etc/fstab.uml\n"
                rcu_restore = "    write /sys/kernel/rcu_expedited 0\n"
                if init_variant == "missing-swapon":
                    swapon = ""
                if init_variant == "missing-rcu-restore":
                    rcu_restore = ""
                if init_variant == "swapon-also-at-boot":
                    rcu_restore = (
                        "    swapon_all /vendor/etc/fstab.uml\n"
                        "on property:sys.boot_completed=1\n" + rcu_restore + swapon
                    )
                elif init_variant == "swapon-twice":
                    rcu_restore = (
                        "on property:sys.boot_completed=1\n" + rcu_restore + swapon + swapon
                    )
                elif init_variant == "rcu-restore-at-boot":
                    rcu_restore = rcu_restore + swapon
                else:
                    rcu_restore = "on property:sys.boot_completed=1\n" + rcu_restore + swapon
                # The directories the Wi-Fi HAL and the supplicant use.
                wifi_dirs = (
                    ""
                    if wifi_dirs_variant == "missing"
                    else "    mkdir /data/vendor/wifi 0770 wifi wifi\n"
                    + (
                        "    mkdir /data/vendor/wifi/wpa 0770 system system\n"
                        if wifi_dirs_variant == "wrong-owner"
                        else "    mkdir /data/vendor/wifi/wpa 0770 wifi wifi\n"
                    )
                    + "    mkdir /data/vendor/wifi/wpa/sockets 0770 wifi wifi\n"
                )
                # The wakelock that keeps the guest awake while an adb host is connected.
                blocker_rules = ""
                if blocker_variant != "missing-service":
                    blocker_rules += (
                        "service suspend_blocker /vendor/bin/suspend_blocker\n"
                        "    user root\n    group system\n"
                        + ("    class early_hal\n" if blocker_variant == "class-started"
                           else "    disabled\n")
                    )
                blocker_rules += (
                    "on property:sys.kernarium.adb.host_connected=1\n"
                    "    start suspend_blocker\n"
                )
                if blocker_variant != "no-stop":
                    blocker_rules += (
                        "on property:sys.kernarium.adb.host_connected=0\n"
                        "    stop suspend_blocker\n"
                    )
                if blocker_variant == "started-at-boot":
                    blocker_rules += "on boot\n    start suspend_blocker\n"
                # The network setup tool: init limits ARP, then waits for the tool.
                net_rules = ""
                if net_setup_variant != "missing-service":
                    net_rules += (
                        "service kernarium_net_setup /vendor/bin/kernarium_net_setup\n"
                        "    user root\n"
                        + ("" if net_setup_variant == "not-oneshot" else "    oneshot\n")
                        + ("" if net_setup_variant == "not-disabled" else "    disabled\n")
                    )
                sysctls = (
                    "    write /proc/sys/net/ipv4/conf/all/arp_ignore 1\n"
                    + ("" if net_setup_variant == "no-announce" else
                       "    write /proc/sys/net/ipv4/conf/all/arp_announce 2\n")
                )
                start_tool = "    exec_start kernarium_net_setup\n"
                if net_setup_variant == "sysctl-after-start":
                    net_rules += "on post-fs-data\n" + start_tool + sysctls
                elif net_setup_variant == "no-exec-start":
                    net_rules += "on post-fs-data\n" + sysctls
                elif net_setup_variant == "started-at-boot":
                    net_rules += "on post-fs-data\n" + sysctls + "on boot\n" + start_tool
                else:
                    net_rules += "on post-fs-data\n" + sysctls + start_tool
                online = "    exec - root system -- /system/bin/resize2fs /dev/block/by-name/userdata\n"
                offline = "    exec -- /system/bin/resize2fs /dev/block/by-name/userdata\n"
                forced = "    exec - root system -- /system/bin/resize2fs -f /dev/block/by-name/userdata\n"
                userdata_grow_late = {
                    "offline-also": offline,
                    "offline-only": offline,
                }.get(userdata_grow_variant, "")
                userdata_grow_data = {
                    None: online,
                    "missing": "",
                    "offline-only": "",
                    "offline-also": online,
                    "twice": online + online,
                    "forced": online + forced,
                    "no-group": offline,
                    "at-boot": online,
                }[userdata_grow_variant]
                userdata_grow_block = (
                    "on post-fs-data\n" + userdata_grow_data if userdata_grow_data else ""
                ) + ("on boot\n" + online if userdata_grow_variant == "at-boot" else "")
                if init_variant == "venus-sync-at-boot":
                    wifi_dirs += venus_sync
                elif init_variant == "venus-sync-cleared-at-boot":
                    wifi_dirs += "    setprop debug.mesa.vn.perf \"\"\n"
                archive.writestr(
                    "VENDOR/etc/init/init.kernarium.rc",
                    "on early-init\n" + timeout_copy + graphics + density +
                    "on fs\n    mount_all --early\n"
                    "on late-fs\n"
                    + userdata_grow_late +
                    "    mount_all --late\n"
                    + userdata_grow_block +
                    "on boot\n"
                    "    setprop ro.hardware.audio.primary goldfish\n"
                    + wifi_dirs
                    + "    setprop service.adb.tcp.port 5555\n"
                    "    start adbd\n"
                    + rcu_restore + blocker_rules + net_rules
                    + ("" if init_variant == "no-adbd-keep-alive"
                       else "on property:init.svc.adbd=stopped\n    start adbd\n")
                    # Cuttlefish's bugreport service, which the product keeps without its keychord.
                    + "service bugreport /system/bin/dumpstate -d -p -z\n    class main\n    user root\n"
                      "    disabled\n    oneshot\n"
                    + ("    keycodes 30 48\n" if init_variant == "bugreport-keychord" else ""),
                )
                if extra_entry is not None:
                    archive.writestr(extra_entry, "on early-init\n")
                if build_variant != "user":
                    # The diagnostics build carries the root shell.
                    archive.writestr("SYSTEM/xbin/su", synthetic_aarch64_elf(alignment))
                archive.writestr(
                    "VENDOR/etc/audio_effects_config.xml",
                    '<audio_effects_conf version="2.0"/>\n',
                )
                archive.writestr(
                    "VENDOR/etc/audio_policy_configuration.xml",
                    '<?xml version="1.0"?><audioPolicyConfiguration '
                    'xmlns:xi="http://www.w3.org/2001/XInclude">'
                    '<xi:include href="primary_audio_policy_configuration.xml"/>'
                    '<xi:include href="r_submix_audio_policy_configuration.xml"/>'
                    '<xi:include href="audio_policy_volumes.xml"/>'
                    '<xi:include href="default_volume_tables.xml"/>'
                    "</audioPolicyConfiguration>",
                )
                archive.writestr(
                    "VENDOR/etc/primary_audio_policy_configuration.xml",
                    '<module name="primary" halVersion="3.0">'
                    '<attachedDevices><item>Speaker</item>'
                    + ("" if missing_mic_policy else "<item>Built-In Mic</item>")
                    + '</attachedDevices>'
                    '<defaultOutputDevice>Speaker</defaultOutputDevice>'
                    '<mixPorts><mixPort name="primary output" role="source" '
                    'flags="AUDIO_OUTPUT_FLAG_PRIMARY"><profile name="" '
                    'format="AUDIO_FORMAT_PCM_16_BIT" samplingRates="48000" '
                    'channelMasks="AUDIO_CHANNEL_OUT_STEREO"/></mixPort>'
                    + (
                        ""
                        if missing_mic_policy
                        else '<mixPort name="primary input" role="sink"><profile '
                        'name="" format="AUDIO_FORMAT_PCM_16_BIT" '
                        'samplingRates="48000" channelMasks="AUDIO_CHANNEL_IN_STEREO '
                        'AUDIO_CHANNEL_IN_MONO"/></mixPort>'
                    )
                    + '</mixPorts>'
                    '<devicePorts><devicePort tagName="Speaker" '
                    'type="AUDIO_DEVICE_OUT_SPEAKER" role="sink"/>'
                    + (
                        ""
                        if missing_mic_policy
                        else '<devicePort tagName="Built-In Mic" '
                        'type="AUDIO_DEVICE_IN_BUILTIN_MIC" address="bottom" '
                        'role="source"/>'
                    )
                    + (
                        '<devicePort tagName="Back Mic" '
                        'type="AUDIO_DEVICE_IN_BACK_MIC" address="back" '
                        'role="source"/>'
                        if extra_input_device
                        else ""
                    )
                    + '</devicePorts>'
                    '<routes><route type="mix" sink="Speaker" '
                    'sources="primary output"/>'
                    + (
                        ""
                        if missing_mic_policy
                        else '<route type="mix" sink="primary input" '
                        'sources="Built-In Mic"/>'
                    )
                    + '</routes></module>',
                )
                archive.writestr(
                    "VENDOR/etc/audio_policy_volumes.xml",
                    "<volumes/>\n",
                )
                archive.writestr(
                    "VENDOR/etc/default_volume_tables.xml",
                    "<volumes/>\n",
                )
                archive.writestr(
                    "VENDOR/etc/r_submix_audio_policy_configuration.xml",
                    '<module name="r_submix" halVersion="3.0"/>\n',
                )
                archive.writestr(
                    "VENDOR/bin/hw/android.hardware.health-service.example",
                    b"health",
                )
                if not missing_blocker_binary:
                    archive.writestr("VENDOR/bin/suspend_blocker", b"blocker")
                if not missing_net_setup_binary:
                    archive.writestr("VENDOR/bin/kernarium_net_setup", b"net setup")
                if not missing_net_library:
                    archive.writestr("VENDOR/lib64/cuttlefish_net.so", b"net library")
                # The script that saves VirtWifi once, with its init file and labels.
                if not missing_wifi_network_script:
                    script = (
                        "#!/system/bin/sh\n# cmd wifi set-wifi-enabled in a comment is fine\n"
                        'marker=persist.sys.kernarium.virtwifi_saved\n'
                        + (
                            ""
                            if wifi_network_variant == "no-add"
                            else "cmd wifi add-network VirtWifi open\n"
                        )
                        + ("" if wifi_network_variant == "no-list" else "cmd wifi list-networks\n")
                        + ("" if wifi_network_variant == "no-status" else "cmd wifi status\n")
                        + ('setprop "$marker" 1\n' if wifi_network_variant != "no-marker" else "")
                        + (
                            'setprop "$marker" 2\n'
                            if wifi_network_variant not in ("no-marker", "no-joined-marker")
                            else ""
                        )
                        + {
                            "enables-wifi": "cmd wifi set-wifi-enabled enabled\n",
                            "connects": "cmd wifi connect-network VirtWifi open\n",
                            "settings-put": "settings put global wifi_on 1\n",
                        }.get(wifi_network_variant or "", "")
                    )
                    archive.writestr("SYSTEM_EXT/bin/kernarium_wifi_network.sh", script)
                if wifi_network_variant != "missing-rc":
                    wifi_trigger = (
                        "on boot\n"
                        if wifi_network_variant == "started-at-boot"
                        else "on property:sys.boot_completed=1\n"
                    )
                    archive.writestr(
                        "SYSTEM_EXT/etc/init/kernarium_wifi_network.rc",
                        (
                            ""
                            if wifi_network_variant == "missing-service"
                            else "service kernarium_wifi_network "
                            "/system_ext/bin/kernarium_wifi_network.sh\n    user root\n"
                            + ("" if wifi_network_variant == "not-oneshot" else "    oneshot\n")
                            + ("" if wifi_network_variant == "not-disabled" else "    disabled\n")
                        )
                        + (
                            ""
                            if wifi_network_variant == "no-start"
                            else wifi_trigger + "    start kernarium_wifi_network\n"
                        ),
                    )
                # The script that kills the cached processes of the reviewed boot-started apps, with its
                # init file: the tracked ones, and the ways each can go wrong.
                reclaim_script = (
                    verifier.parents[1] / "overlay/tools/kernarium_reclaim_boot_apps.sh"
                ).read_text()
                reclaim_rc = (
                    verifier.parents[1] / "overlay/tools/kernarium_reclaim_boot_apps.rc"
                ).read_text()
                for variant, (old, new) in {
                    "no-kill": ('cmd activity kill --user 0 "$pkg"', "true"),
                    "kill-all": ("tag=kernarium_reclaim_boot_apps\n", "tag=kernarium_reclaim_boot_apps\ncmd activity kill-all\n"),
                    "force-stop": ("tag=kernarium_reclaim_boot_apps\n", 'tag=kernarium_reclaim_boot_apps\ncmd activity force-stop "$pkg"\n'),
                    "kill-9": ("tag=kernarium_reclaim_boot_apps\n", "tag=kernarium_reclaim_boot_apps\nkill -9 1234\n"),
                    "pkill": ("tag=kernarium_reclaim_boot_apps\n", 'tag=kernarium_reclaim_boot_apps\npkill -f "$pkg"\n'),
                    "kill-pid": ("tag=kernarium_reclaim_boot_apps\n", "tag=kernarium_reclaim_boot_apps\nkill -TERM 1234\n"),
                    "am-command": ("tag=kernarium_reclaim_boot_apps\n", 'tag=kernarium_reclaim_boot_apps\nam kill "$pkg"\n'),
                    "second-kill": ("tag=kernarium_reclaim_boot_apps\n", 'tag=kernarium_reclaim_boot_apps\ncmd activity kill --user 0 "$pkg"\n'),
                    "second-list": ("tag=kernarium_reclaim_boot_apps\n", 'reclaim_list="$reclaim_list\ncom.example.app com.example.app"\ntag=kernarium_reclaim_boot_apps\n'),
                    "extra-package": ("com.android.rkpdapp com.android.rkpdapp\n",
                                      "com.android.rkpdapp com.android.rkpdapp\ncom.example.app com.example.app\n"),
                    "missing-package": ("com.stevesoltys.seedvault com.stevesoltys.seedvault\n", ""),
                    "missing-messaging": ("com.android.messaging com.android.messaging\n", ""),
                    "missing-telephony-provider": ("com.android.providers.telephony com.android.phone\n", ""),
                    "missing-icons": ("com.kernarium.icons com.kernarium.icons\n", ""),
                    "telephony-package-as-process": (
                        "com.android.providers.telephony com.android.phone",
                        "com.android.providers.telephony com.android.providers.telephony"),
                    "missing-process": ("android.ext.services .adservices android.ext.services",
                                        "android.ext.services android.ext.services"),
                    "no-settle": ("settle=120", "settle=0"),
                    "no-lru": ("dumpsys activity lru", "dumpsys activity processes"),
                    "no-list": ("reclaim_list='", "reclaim_apps='"),
                }.items():
                    if reclaim_variant == variant:
                        if old not in reclaim_script:
                            fail(f"the reclaim script lacks '{old}' for the {variant} variant")
                        reclaim_script = reclaim_script.replace(old, new)
                if reclaim_variant != "missing-script":
                    archive.writestr("SYSTEM_EXT/bin/kernarium_reclaim_boot_apps.sh", reclaim_script)
                for variant, (old, new) in {
                    "missing-service": ("service kernarium_reclaim_boot_apps ", "service other_service "),
                    "not-oneshot": ("    oneshot\n", ""),
                    "not-disabled": ("    disabled\n", ""),
                    "started-at-boot": ("on property:sys.boot_completed=1", "on boot"),
                    "no-start": ("    start kernarium_reclaim_boot_apps\n", ""),
                }.items():
                    if reclaim_variant == variant:
                        if old not in reclaim_rc:
                            fail(f"the reclaim init file lacks '{old}' for the {variant} variant")
                        reclaim_rc = reclaim_rc.replace(old, new)
                if reclaim_variant != "missing-rc":
                    archive.writestr("SYSTEM_EXT/etc/init/kernarium_reclaim_boot_apps.rc", reclaim_rc)
                # The script that applies the host's time zone, with its init file: the tracked ones,
                # and the ways each can go wrong.
                time_zone_script = (
                    verifier.parents[1] / "overlay/tools/kernarium_time_zone.sh"
                ).read_text()
                time_zone_rc = (
                    verifier.parents[1] / "overlay/tools/kernarium_time_zone.rc"
                ).read_text()
                for variant, (old, new) in {
                    "no-property": ("getprop ro.boot.kernarium.timezone", "getprop ro.boot.other"),
                    "no-auto-read": ("cmd settings get global auto_time_zone", "true"),
                    "no-set": ('cmd alarm set-timezone "$zone"', "true"),
                    "no-marker": ('setprop "$marker" "$zone"', "true"),
                    "writes-setting": ("tag=kernarium_time_zone\n",
                                       "tag=kernarium_time_zone\ncmd settings put global auto_time_zone 0\n"),
                    "setprop-zone": ("tag=kernarium_time_zone\n",
                                     "tag=kernarium_time_zone\nsetprop persist.sys.timezone GMT\n"),
                    "detector": ("tag=kernarium_time_zone\n",
                                 "tag=kernarium_time_zone\n"
                                 "cmd time_zone_detector set_auto_detection_enabled true\n"),
                }.items():
                    if time_zone_variant == variant:
                        if old not in time_zone_script:
                            fail(f"the time zone script lacks '{old}' for the {variant} variant")
                        time_zone_script = time_zone_script.replace(old, new)
                if time_zone_variant != "missing-script":
                    archive.writestr("SYSTEM_EXT/bin/kernarium_time_zone.sh", time_zone_script)
                for variant, (old, new) in {
                    "missing-service": ("service kernarium_time_zone ", "service other_service "),
                    "not-oneshot": ("    oneshot\n", ""),
                    "not-disabled": ("    disabled\n", ""),
                    "started-at-boot": ("on property:sys.boot_completed=1", "on boot"),
                    "no-start": ("    start kernarium_time_zone\n", ""),
                }.items():
                    if time_zone_variant == variant:
                        if old not in time_zone_rc:
                            fail(f"the time zone init file lacks '{old}' for the {variant} variant")
                        time_zone_rc = time_zone_rc.replace(old, new)
                if time_zone_variant != "missing-rc":
                    archive.writestr("SYSTEM_EXT/etc/init/kernarium_time_zone.rc", time_zone_rc)
                # The script that moves an older guest to the on-screen keyboard default, with its init
                # file: the tracked ones, and the ways each can go wrong.
                keyboard_script = (
                    verifier.parents[1] / "overlay/tools/kernarium_keyboard_default.sh"
                ).read_text()
                keyboard_rc = (
                    verifier.parents[1] / "overlay/tools/kernarium_keyboard_default.rc"
                ).read_text()
                for variant, (old, new) in {
                    "no-read": ("cmd settings get secure show_ime_with_hard_keyboard", "true"),
                    "no-put": ("cmd settings put secure show_ime_with_hard_keyboard 0", "true"),
                    "no-marker": ('setprop "$marker" 1', "true"),
                    "turns-it-on": ("tag=kernarium_keyboard_default\n",
                                    "tag=kernarium_keyboard_default\n"
                                    "cmd settings put secure show_ime_with_hard_keyboard 1\n"),
                    "writes-another-setting": ("tag=kernarium_keyboard_default\n",
                                               "tag=kernarium_keyboard_default\n"
                                               "cmd settings put system screen_off_timeout 1000\n"),
                    "deletes-the-setting": ("tag=kernarium_keyboard_default\n",
                                            "tag=kernarium_keyboard_default\n"
                                            "cmd settings delete secure show_ime_with_hard_keyboard\n"),
                    "other-user": ("tag=kernarium_keyboard_default\n",
                                   "tag=kernarium_keyboard_default\n"
                                   "cmd settings --user 10 get secure show_ime_with_hard_keyboard\n"),
                }.items():
                    if keyboard_default_variant == variant:
                        if old not in keyboard_script:
                            fail(f"the keyboard default script lacks '{old}' for the {variant} variant")
                        keyboard_script = keyboard_script.replace(old, new)
                if keyboard_default_variant != "missing-script":
                    archive.writestr("SYSTEM_EXT/bin/kernarium_keyboard_default.sh", keyboard_script)
                for variant, (old, new) in {
                    "missing-service": ("service kernarium_keyboard_default ", "service other_service "),
                    "not-oneshot": ("    oneshot\n", ""),
                    "not-disabled": ("    disabled\n", ""),
                    "started-at-boot": ("on property:sys.boot_completed=1", "on boot"),
                    "no-start": ("    start kernarium_keyboard_default\n", ""),
                }.items():
                    if keyboard_default_variant == variant:
                        if old not in keyboard_rc:
                            fail(f"the keyboard default init file lacks '{old}' for the {variant} variant")
                        keyboard_rc = keyboard_rc.replace(old, new)
                if keyboard_default_variant != "missing-rc":
                    archive.writestr("SYSTEM_EXT/etc/init/kernarium_keyboard_default.rc", keyboard_rc)
                # The shell command of Old app compatibility: the tracked wrapper script, and a jar whose
                # code holds the command's class (a build may strip the code into the odex instead).
                oldapp_wrapper = (
                    verifier.parents[1] / "overlay/oldapp_switch/kernarium_oldapp"
                ).read_text()
                for variant, (old, new) in {
                    "wrapper-no-classpath": ("export CLASSPATH=$base/framework/kernarium_oldapp.jar\n", ""),
                    "wrapper-wrong-jar": ("framework/kernarium_oldapp.jar", "framework/other.jar"),
                    "wrapper-wrong-class": ("com.kernarium.oldapp.Main",
                                            "com.kernarium.oldapp.Other"),
                    "wrapper-no-exec": ("exec app_process", "app_process"),
                    "wrapper-system-base": ("base=/system_ext", "base=/system"),
                }.items():
                    if oldswitch_variant == variant:
                        if old not in oldapp_wrapper:
                            fail(f"the Old app wrapper lacks '{old}' for the {variant} variant")
                        oldapp_wrapper = oldapp_wrapper.replace(old, new)
                if oldswitch_variant != "missing-wrapper":
                    archive.writestr("SYSTEM_EXT/bin/kernarium_oldapp", oldapp_wrapper)
                oldapp_jar = io.BytesIO()
                with zipfile.ZipFile(oldapp_jar, "w") as jar:
                    jar.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
                    if oldswitch_variant not in ("jar-stripped", "preopt-code", "preopt-half"):
                        jar.writestr(
                            "classes.dex",
                            b"dex\n035\0"
                            + (b"Lcom/kernarium/oldapp/Other;" if oldswitch_variant == "jar-without-class"
                               else b"Lcom/kernarium/oldapp/Main;"),
                        )
                if oldswitch_variant in ("preopt-code", "preopt-half"):
                    # Dexpreopting strips the code from the jar into the files beside it.
                    for suffix in ("odex", "vdex") if oldswitch_variant == "preopt-code" else ("vdex",):
                        archive.writestr(f"SYSTEM_EXT/framework/oat/arm64/kernarium_oldapp.{suffix}", b"oat")
                if oldswitch_variant == "jar-not-zip":
                    archive.writestr("SYSTEM_EXT/framework/kernarium_oldapp.jar", b"not a jar")
                elif oldswitch_variant != "missing-jar":
                    archive.writestr("SYSTEM_EXT/framework/kernarium_oldapp.jar", oldapp_jar.getvalue())
                # The launcher-application shell command of Add to Home Screen: the tracked wrapper script, and
                # a jar whose code holds the command's class.
                apps_wrapper = (verifier.parents[1] / "overlay/app_shortcuts/kernarium_apps").read_text()
                for variant, (old, new) in {
                    "wrapper-no-classpath": ("export CLASSPATH=$base/framework/kernarium_apps.jar\n", ""),
                    "wrapper-wrong-jar": ("framework/kernarium_apps.jar", "framework/other.jar"),
                    "wrapper-wrong-class": ("com.kernarium.apps.Main", "com.kernarium.apps.Other"),
                    "wrapper-no-exec": ("exec app_process", "app_process"),
                }.items():
                    if appshell_variant == variant:
                        if old not in apps_wrapper:
                            fail(f"the launcher-application wrapper lacks '{old}' for the {variant} variant")
                        apps_wrapper = apps_wrapper.replace(old, new)
                if appshell_variant != "missing-wrapper":
                    archive.writestr("SYSTEM_EXT/bin/kernarium_apps", apps_wrapper)
                apps_jar = io.BytesIO()
                with zipfile.ZipFile(apps_jar, "w") as jar:
                    jar.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
                    if appshell_variant != "jar-stripped":
                        jar.writestr(
                            "classes.dex",
                            b"dex\n035\0"
                            + (b"Lcom/kernarium/apps/Other;" if appshell_variant == "jar-without-class"
                               else b"Lcom/kernarium/apps/Main;"),
                        )
                if appshell_variant == "jar-not-zip":
                    archive.writestr("SYSTEM_EXT/framework/kernarium_apps.jar", b"not a jar")
                elif appshell_variant != "missing-jar":
                    archive.writestr("SYSTEM_EXT/framework/kernarium_apps.jar", apps_jar.getvalue())
                # The device configuration of the host's keyboard and mouse: the tracked files.
                for name, destination in input_device_files.items():
                    archive.writestr(destination, (verifier.parents[1] / "overlay/input" / name).read_text())
                archive.writestr(
                    "SYSTEM_EXT/etc/selinux/system_ext_file_contexts",
                    "/(system_ext|system/system_ext)/bin/other\\.sh  u:object_r:system_file:s0\n"
                    + (
                        ""
                        if wifi_network_variant == "missing-label"
                        else "/(system_ext|system/system_ext)/bin/kernarium_wifi_network\\.sh  "
                        "u:object_r:kernarium_wifi_network_exec:s0\n"
                    )
                    + (
                        ""
                        if reclaim_variant == "missing-label"
                        else "/(system_ext|system/system_ext)/bin/kernarium_reclaim_boot_apps\\.sh  "
                        "u:object_r:kernarium_reclaim_boot_apps_exec:s0\n"
                    )
                    + (
                        ""
                        if time_zone_variant == "missing-label"
                        else "/(system_ext|system/system_ext)/bin/kernarium_time_zone\\.sh  "
                        "u:object_r:kernarium_time_zone_exec:s0\n"
                    )
                    + (
                        ""
                        if keyboard_default_variant == "missing-label"
                        else "/(system_ext|system/system_ext)/bin/kernarium_keyboard_default\\.sh  "
                        "u:object_r:kernarium_keyboard_default_exec:s0\n"
                    )
                    + sms_policy_fixture(verifier, sms_policy_variant)["file_context"],
                )
                sms_policy = sms_policy_fixture(verifier, sms_policy_variant, build_variant)
                icons = icons_fixture(verifier, icons_variant)
                archive.writestr("SYSTEM/etc/selinux/plat_seapp_contexts", sms_policy["seapp_plat"] + icons["seapp_plat"])
                archive.writestr("SYSTEM_EXT/etc/selinux/system_ext_seapp_contexts", sms_policy["seapp_ext"] + icons["seapp_ext"])
                # Platform, vendor and system_ext policy as the builder emits them: the
                # platform's core domains, a vendor-owned property that only
                # vendor_init sets, and the system-owned property adbd reports its host in.
                # The real platform policy is 3.4 MB; the filler keeps the fixture realistic in size.
                archive.writestr(
                    "SYSTEM/etc/selinux/plat_sepolicy.cil",
                    "(typeattributeset coredomain (adbd init system_server lmkd"
                    + (" hal_audio_default" if audio_ring_variant == "core-hal" else "")
                    + "))\n"
                    # The concrete types that run applications, as the platform lists them.
                    + "(typeattributeset appdomain (runas_app untrusted_app untrusted_app_34 "
                    "untrusted_app_30 untrusted_app_29 untrusted_app_25 gmscore_app mediaprovider_app "
                    "platform_app platform_app_36 priv_app priv_app_36 isolated_app ephemeral_app "
                    "system_app sdk_sandbox_34 sdk_sandbox_audit sdk_sandbox_next shell))\n"
                    + "(typeattributeset untrusted_app_all (runas_app untrusted_app untrusted_app_34 "
                    "untrusted_app_30 untrusted_app_29 untrusted_app_25))\n"
                    + "(typeattributeset sdk_sandbox_all (sdk_sandbox_34 sdk_sandbox_audit sdk_sandbox_next))\n"
                    + "(typeattributeset domain ("
                    + ("" if adbd_launch_unreadable else "adbd ")
                    + "init system_server vendor_init untrusted_app surfaceflinger lmkd hal_audio_default"
                    + ("" if venus_policy_variant == "shell-unreadable" else " shell")
                    + "))\n"
                    # The Venus driver option is a debug property that vendor_init sets and every
                    # domain reads.
                    + "(allow domain debug_prop (file (read getattr map open)))\n"
                    + ("" if venus_policy_variant == "vendor-init-cannot-set"
                       else "(allow vendor_init debug_prop (property_service (set)))\n")
                    # Every vendor domain reads vendor_default_prop, through the attribute the
                    # compiler generates for the platform's rule.
                    + "(typeattributeset base_typeattr_410 (and (domain ) "
                    "(not (appdomain coredomain ))))\n"
                    + ("" if audio_ring_variant == "hal-unreadable"
                       else "(allow base_typeattr_410 vendor_default_prop "
                       "(file (read getattr map open)))\n")
                    + ("" if audio_ring_variant == "vendor-init-cannot-set"
                       else "(allow vendor_init vendor_default_prop (property_service (set)))\n")
                    +
                    "(allow domain boot_status_prop (file (read getattr map open)))\n"
                    "(allow domain bootloader_prop (file (read getattr map open)))\n"
                    "(allow domain system_prop (file (read getattr map open)))\n"
                    "(allow domain system_adbd_prop (file (read getattr map open)))\n"
                    "(allow domain init_service_status_prop (file (read getattr map open)))\n"
                    # The platform lets its core domains read lmkd's settings, and declares the
                    # sysfs types, the virtio-gpu device's among them.
                    + ("" if lmkd_variant == "property-unreadable"
                       else "(allow coredomain lmkd_config_prop (file (getattr open read map)))\n")
                    + "(typeattributeset sysfs_type (sysfs sysfs_gpu))\n"
                    "(typeattributeset mlstrustedsubject (artd installd"
                    + (" mediaprovider_app" if lineage_policy == "mediaprovider-trusted" else "")
                    + "))\n"
                    + ("(typepermissive su)\n"
                       if build_variant != "user" or user_permissive_su else "")
                    + sms_policy["plat"]
                    + "; padding to the size of a real platform policy\n" * (
                        (17 << 20 if oversize_platform_policy else 3_441_569) // 48
                    ),
                )
                archive.writestr(
                    "SYSTEM/etc/selinux/plat_property_contexts",
                    "sys.boot_completed    u:object_r:boot_status_prop:s0 exact bool\n"
                    "ro.boot.              u:object_r:bootloader_prop:s0\n"
                    "persist.sys.usb.config  u:object_r:system_prop:s0 exact string\n"
                    "persist.adb.tls_server.enable  u:object_r:system_adbd_prop:s0 exact bool\n"
                    # As the platform types them: every status is private (coredomain reads it) but a few.
                    "init.svc.  u:object_r:init_service_status_private_prop:s0\n"
                    "init.svc.adbd  u:object_r:init_service_status_prop:s0 exact string\n"
                    "init.svc.zygote  u:object_r:init_service_status_prop:s0 exact string\n"
                    "debug.  u:object_r:debug_prop:s0\n"
                    "ro.hardware.  u:object_r:vendor_default_prop:s0\n"
                    "ro.hardware.audio.primary  u:object_r:exported_default_prop:s0 exact string\n"
                    + sms_policy["property_context"]
                    + ("ro.lmk.low  u:object_r:lmkd_config_prop:s0 exact int\n"
                       if lmkd_variant == "no-property-label" else
                       "ro.lmk.gpu_reclaim  u:object_r:lmkd_config_prop:s0 exact bool\n"),
                )
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_property_contexts",
                    "ro.boot.lcd_density   u:object_r:vendor_graphics_config_prop:s0 exact int\n",
                )
                archive.writestr(
                    "SYSTEM_EXT/etc/selinux/system_ext_property_contexts",
                    "ro.vendor.aware_available  u:object_r:vendor_aware_available_prop:s0\n"
                    + (
                        ""
                        if missing_adb_prop_context
                        else "sys.kernarium.adb.host_connected    "
                        "u:object_r:kernarium_adb_host_prop:s0 exact bool\n"
                    )
                    + (
                        ""
                        if adb_key_variant == "bootloader-label"
                        else "ro.boot.kernarium.adb_key    "
                        "u:object_r:kernarium_adb_key_prop:s0 exact string\n"
                    )
                    + (
                        ""
                        if wifi_network_variant == "missing-marker-context"
                        else "persist.sys.kernarium.virtwifi_saved    "
                        "u:object_r:kernarium_wifi_network_prop:s0 exact "
                        + {
                            "marker-type-bool": "bool",
                            "marker-type-enum-one": "enum 1",
                        }.get(wifi_network_variant or "", "enum 1 2")
                        + "\n"
                    )
                    + (
                        ""
                        if time_zone_variant == "missing-marker-context"
                        else "persist.sys.kernarium.timezone_applied    "
                        "u:object_r:kernarium_time_zone_prop:s0 exact string\n"
                    )
                    + (
                        ""
                        if keyboard_default_variant == "missing-marker-context"
                        else "persist.sys.kernarium.keyboard_default    "
                        "u:object_r:kernarium_keyboard_default_prop:s0 exact bool\n"
                    )
                    + {
                        "missing-context": "",
                        "exact-context": "persist.sys.kernarium.oldapp.    "
                        "u:object_r:kernarium_oldapp_prop:s0 exact string\n",
                        "wrong-type-context": "persist.sys.kernarium.oldapp.    "
                        "u:object_r:kernarium_time_zone_prop:s0\n",
                    }.get(oldswitch_variant or "",
                          "persist.sys.kernarium.oldapp.    u:object_r:kernarium_oldapp_prop:s0\n"),
                )
                archive.writestr(
                    "SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil",
                    sms_policy["ext"]
                    + "(typeattributeset vendor_property_type (vendor_aware_available_prop ))\n"
                    # The report file's type (sysfs_gpu_memory.te) and what lmkd may do with it and
                    # with the directories of the virtio-gpu device that lead to it.
                    + ("" if lmkd_variant == "type-not-sysfs"
                       else "(typeattributeset sysfs_type (sysfs_gpu_memory))\n")
                    + ("" if lmkd_variant == "no-dir-search"
                       else "(allow lmkd sysfs_gpu (dir (search)))\n")
                    + {
                        "no-report-rule": "",
                        "no-report-write": "(allow lmkd sysfs_gpu_memory (file (getattr open read ioctl lock map)))\n",
                    }.get(lmkd_variant,
                          "(allow lmkd sysfs_gpu_memory (file (getattr open read ioctl lock map watch "
                          "watch_reads write append)))\n")
                    # A relabelto on a contextmount type is allowed; only the Lineage rules are not.
                    + "(typeattributeset fs_type (fixture_rootfs sdcard_posix))\n"
                    "(typeattributeset contextmount_type (fixture_rootfs))\n"
                    "(allow init fixture_rootfs (filesystem (relabelto)))\n"
                    + (
                        ""
                        if missing_mediaserver_audit
                        else "(dontaudit mediaserver vendor_default_prop (file (read)))\n"
                    )
                    + (
                        ""
                        if missing_shell_density_audit
                        else "(dontaudit shell qemu_sf_lcd_density_prop (file (read)))\n"
                    )
                    + (
                        "(allow fsck_untrusted self (capability (sys_admin)))\n"
                        if lineage_policy == "fsck-sys-admin"
                        else ""
                    )
                    + (
                        "(allow vold sdcard_posix (filesystem (relabelfrom relabelto)))\n"
                        if lineage_policy == "vold-relabelto"
                        else ""
                    )
                    + (
                        "(typepermissive backuptool)\n"
                        if lineage_policy == "permissive-backuptool"
                        else ""
                    )
                    + (
                        "(allow mediaprovider_app privapp_data_file (file (create unlink)))\n"
                        if lineage_policy == "mediaprovider-privapp"
                        else ""
                    )
                    + "(typeattributeset system_property_type (apexd_prop "
                    + ("" if adb_prop_not_system else "kernarium_adb_host_prop ")
                    + ("" if wifi_network_variant == "marker-not-system"
                       else "kernarium_wifi_network_prop ")
                    + ("" if time_zone_variant == "marker-not-system"
                       else "kernarium_time_zone_prop ")
                    + ("" if keyboard_default_variant == "marker-not-system"
                       else "kernarium_keyboard_default_prop ")
                    + ("" if oldswitch_variant == "not-system" else "kernarium_oldapp_prop ")
                    + "))\n"
                    # The scope of the Old app switch's property: restricted, readable by every
                    # application domain, which an internal one is not for the non-core ones.
                    + (
                        "(typeattributeset system_internal_property_type (kernarium_oldapp_prop))\n"
                        if oldswitch_variant == "internal-scope"
                        else ""
                    )
                    + (
                        ""
                        if oldswitch_variant in ("not-restricted", "internal-scope")
                        else "(typeattributeset system_restricted_property_type (kernarium_oldapp_prop))\n"
                    )
                    + (
                        ""
                        if wifi_network_variant == "missing-transition"
                        else "(typetransition init kernarium_wifi_network_exec process "
                        "kernarium_wifi_network)\n"
                    )
                    + (
                        ""
                        if wifi_network_variant == "missing-marker-rule"
                        else "(allow kernarium_wifi_network kernarium_wifi_network_prop "
                        "(property_service (set)))\n"
                    )
                    + (
                        ""
                        if wifi_network_variant == "missing-binder-transfer"
                        else "(allow kernarium_wifi_network system_server (binder (transfer)))\n"
                    )
                    + (
                        ""
                        if wifi_network_variant == "missing-binder-call"
                        else "(allow system_server kernarium_wifi_network (binder (call)))\n"
                    )
                    + "".join(
                        rule
                        for variant, rule in (
                            ("missing-transition",
                             "(typetransition init kernarium_reclaim_boot_apps_exec process "
                             "kernarium_reclaim_boot_apps)\n"),
                            ("missing-find",
                             "(allow kernarium_reclaim_boot_apps activity_service "
                             "(service_manager (find)))\n"),
                            ("missing-transfer",
                             "(allow kernarium_reclaim_boot_apps system_server (binder (transfer)))\n"),
                            ("missing-call",
                             "(allow system_server kernarium_reclaim_boot_apps (binder (call)))\n"),
                            ("missing-fd",
                             "(allow system_server kernarium_reclaim_boot_apps (fd (use)))\n"),
                            ("missing-fifo",
                             "(allow system_server kernarium_reclaim_boot_apps "
                             "(fifo_file (read write getattr)))\n"),
                        )
                        if reclaim_variant != variant
                    )
                    + "".join(
                        rule
                        for variant, rule in (
                            ("missing-transition",
                             "(typetransition init kernarium_time_zone_exec process "
                             "kernarium_time_zone)\n"),
                            ("missing-settings-find",
                             "(allow kernarium_time_zone settings_service "
                             "(service_manager (find)))\n"),
                            ("missing-alarm-find",
                             "(allow kernarium_time_zone alarm_service "
                             "(service_manager (find)))\n"),
                            ("missing-transfer",
                             "(allow kernarium_time_zone system_server (binder (transfer)))\n"),
                            ("missing-call",
                             "(allow system_server kernarium_time_zone (binder (call)))\n"),
                            ("missing-fd",
                             "(allow system_server kernarium_time_zone (fd (use)))\n"),
                            ("missing-fifo",
                             "(allow system_server kernarium_time_zone "
                             "(fifo_file (read write getattr)))\n"),
                        )
                        if time_zone_variant != variant
                    )
                    + (
                        ""
                        if time_zone_variant == "missing-marker-rule"
                        else "(allow kernarium_time_zone kernarium_time_zone_prop "
                        "(property_service (set)))\n"
                    )
                    + (
                        "(allow kernarium_time_zone timezone_prop (property_service (set)))\n"
                        if time_zone_variant == "sets-zone-property"
                        else ""
                    )
                    + "".join(
                        rule
                        for variant, rule in (
                            ("missing-transition",
                             "(typetransition init kernarium_keyboard_default_exec process "
                             "kernarium_keyboard_default)\n"),
                            ("missing-settings-find",
                             "(allow kernarium_keyboard_default settings_service "
                             "(service_manager (find)))\n"),
                            ("missing-transfer",
                             "(allow kernarium_keyboard_default system_server (binder (transfer)))\n"),
                            ("missing-call",
                             "(allow system_server kernarium_keyboard_default (binder (call)))\n"),
                            ("missing-fd",
                             "(allow system_server kernarium_keyboard_default (fd (use)))\n"),
                            ("missing-fifo",
                             "(allow system_server kernarium_keyboard_default "
                             "(fifo_file (read write getattr)))\n"),
                        )
                        if keyboard_default_variant != variant
                    )
                    + (
                        ""
                        if keyboard_default_variant == "missing-marker-rule"
                        else "(allow kernarium_keyboard_default kernarium_keyboard_default_prop "
                        "(property_service (set)))\n"
                    )
                    + (
                        "(allow kernarium_keyboard_default timezone_prop (property_service (set)))\n"
                        if keyboard_default_variant == "sets-another-property"
                        else ""
                    )
                    + (
                        ""
                        if oldswitch_variant == "no-shell-set"
                        else "(allow shell kernarium_oldapp_prop (property_service (set)))\n"
                    )
                    + "(allow shell kernarium_oldapp_prop (file (read getattr map open)))\n"
                    # The command's lock file, in the shell user's own data directory.
                    + (
                        ""
                        if oldswitch_variant == "no-lock-file"
                        else "(allow shell shell_data_file (file (create open read write lock getattr)))\n"
                        if oldswitch_variant == "no-lock-setattr"
                        else "(allow shell shell_data_file (file (create open read write lock getattr setattr)))\n"
                    )
                    + (
                        ""
                        if oldswitch_variant == "no-lock-dir"
                        else "(allow shell shell_data_file (dir (search add_name write)))\n"
                    )
                    + (
                        ""
                        if oldswitch_variant == "no-app-read"
                        else "(allow appdomain kernarium_oldapp_prop (file (read)))\n"
                        if oldswitch_variant == "partial-app-read"
                        else "".join(
                            f"(allow {domain} kernarium_oldapp_prop (file (read getattr map open)))\n"
                            for domain in (
                                "runas_app", "untrusted_app", "untrusted_app_34", "untrusted_app_30",
                                "untrusted_app_29", "untrusted_app_25", "gmscore_app", "mediaprovider_app",
                                "platform_app", "platform_app_36", "priv_app", "priv_app_36", "isolated_app",
                                "ephemeral_app", "system_app", "sdk_sandbox_34", "sdk_sandbox_audit",
                                "sdk_sandbox_next", "kernarium_sms",
                            )
                        )
                        if oldswitch_variant == "per-domain-reads"
                        else "(allow appdomain kernarium_oldapp_prop (file (read getattr map open)))\n"
                    )
                    + {
                        "untrusted-app-sets": "(allow untrusted_app kernarium_oldapp_prop (property_service (set)))\n",
                        "platform-app-sets": "(allow platform_app_36_202604 kernarium_oldapp_prop_202604 "
                        "(property_service (set)))\n",
                        "appdomain-sets": "(allow appdomain kernarium_oldapp_prop (property_service (set)))\n",
                        "runas-app-sets": "(allow runas_app kernarium_oldapp_prop (property_service (set)))\n",
                    }.get(oldswitch_variant or "", "")
                    # What the restricted macro emits for the type (the non-core domains may not
                    # set it), and the neverallow of the application domains.
                    + (
                        ""
                        if oldswitch_variant == "no-neverallow"
                        else "(typeattributeset base_typeattr_241 (and (domain ) (not (coredomain ))))\n"
                        "(neverallow base_typeattr_241 kernarium_oldapp_prop (property_service (set)))\n"
                    )
                    + (
                        ""
                        if oldswitch_variant in ("no-neverallow", "vendor-style-neverallow")
                        else "(typeattributeset base_typeattr_955 (untrusted_app_all ))\n"
                        if oldswitch_variant == "partial-neverallow"
                        else "(typeattributeset base_typeattr_955 (and (appdomain ) (not (shell ))))\n"
                    )
                    + (
                        ""
                        if oldswitch_variant in ("no-neverallow", "vendor-style-neverallow")
                        else "(neverallow base_typeattr_955 kernarium_oldapp_prop (property_service (set)))\n"
                    )
                    + (
                        ""
                        if adbd_launch_unreadable or adb_key_variant in ("adbd-unreadable", "bootloader-label")
                        else "(allow adbd kernarium_adb_key_prop (file (getattr open read map)))\n"
                    )
                    + {
                        "untrusted-app-reads": "(allow untrusted_app kernarium_adb_key_prop (file (getattr open read map)))\n",
                        "appdomain-reads": "(allow appdomain kernarium_adb_key_prop (file (getattr open read map)))\n",
                        "untrusted-app-34-reads": "(allow untrusted_app_34 kernarium_adb_key_prop (file (getattr open read map)))\n",
                        "untrusted-app-29-reads": "(allow untrusted_app_29_202604 kernarium_adb_key_prop_202604 (file (read)))\n",
                        "runas-app-reads": "(allow runas_app kernarium_adb_key_prop (file (open read)))\n",
                        "sdk-sandbox-reads": "(allow sdk_sandbox_audit kernarium_adb_key_prop (file (read)))\n",
                        "shell-reads": "(allow shell kernarium_adb_key_prop (file (read)))\n",
                        "generated-attribute-reads": "(typeattributeset base_typeattr_77 (untrusted_app_34 gmscore_app))\n"
                        "(allow base_typeattr_77 kernarium_adb_key_prop (file (read)))\n",
                    }.get(adb_key_variant or "", "")
                    + (
                        ""
                        if missing_adb_prop_policy
                        else "(allow adbd kernarium_adb_host_prop (property_service (set)))\n"
                    )
                    + (
                        ""
                        if adb_prop_unreadable_by_vendor_init
                        else "(allow vendor_init kernarium_adb_host_prop (file (getattr open read map)))\n"
                    )
                    + (
                        "(allow system_server vendor_aware_available_prop (property_service (set)))\n"
                        if system_ext_core_sets_vendor_prop
                        else ""
                    ),
                )
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_sepolicy.cil",
                    "(policycap open_perms)\n"
                    if missing_memfd_policy
                    else "(policycap memfd_class)\n"
                    + (
                        ""
                        if missing_spl_policy
                        else "(allow appdomain vendor_security_patch_level_prop_202604 "
                        "(file (read getattr map open)))\n"
                    )
                    + "(typeattributeset vendor_property_type (vendor_mesa_prop "
                    "virtual_face_hal_prop_202604))\n"
                    "(allow vendor_init_202604 vendor_mesa_prop (property_service (set)))\n"
                    "(allow vendor_init_202604 vendor_graphics_config_prop "
                    "(file (read getattr map open)))\n"
                    "(allow init virtual_face_hal_prop_202604 (property_service (set)))\n"
                    + (
                        ""
                        if missing_audio_binder_policy
                        else "(allow hal_audio_default hal_audio_default (binder (call transfer)))\n"
                    )
                    + (
                        ""
                        if missing_wifi_debug_policy
                        else "(allow hal_wifi_default persist_vendor_debug_wifi_prop_202604 "
                        "(file (read getattr map open)))\n"
                    )
                    + {
                        None: "(allow hal_camera_default hal_graphics_composer (fd (use)))\n",
                        "missing": "",
                        # A numeric suffix is the version of a platform symbol (_202604, six digits); on a
                        # vendor type or any other length it is a different symbol, which the rule does not cover.
                        "source-other-type": "(type hal_camera_default_2)\n"
                        "(allow hal_camera_default_2 hal_graphics_composer (fd (use)))\n",
                        "target-other-type": "(type hal_graphics_composer_2)\n"
                        "(allow hal_camera_default hal_graphics_composer_2 (fd (use)))\n",
                        "source-short-suffix": "(allow hal_camera_default_20260 hal_graphics_composer (fd (use)))\n",
                        "target-long-suffix": "(allow hal_camera_default hal_graphics_composer_2026040 (fd (use)))\n",
                    }[camera_fence_variant]
                    + (
                        "(allow adbd_202604 virtual_face_hal_prop_202604 (property_service (set)))\n"
                        if core_sets_vendor_prop
                        else ""
                    )
                    # init enters the network setup domain through its executable's label.
                    + (
                        ""
                        if net_policy_variant == "missing-transition"
                        else "(typetransition init_202604 kernarium_net_setup_exec process "
                        "kernarium_net_setup)\n"
                    )
                    # The network setup domain turns IPv6 off through the sysctl files.
                    + (
                        ""
                        if net_policy_variant == "no-sysctl-write"
                        else "(allow kernarium_net_setup proc_net_202604 (file (open write)))\n"
                    )
                    # The gamepad relay: init enters its domain through its executable's label, and
                    # the domain may use the uhid node and vsock, as an unconstrained vsock user.
                    + (
                        ""
                        if gamepad_policy_variant == "missing-transition"
                        else "(typetransition init_202604 kernarium_gamepad_exec process "
                        "kernarium_gamepad)\n"
                    )
                    + (
                        ""
                        if gamepad_policy_variant == "no-uhid"
                        else "(allow kernarium_gamepad uhid_device_202604 (chr_file ("
                        + ("getattr open read ioctl lock map" if gamepad_policy_variant == "uhid-read-only"
                           else "getattr open read ioctl lock map append write")
                        + ")))\n"
                    )
                    + (
                        ""
                        if gamepad_policy_variant == "no-vsock"
                        else "(allow kernarium_gamepad self (vsock_socket ("
                        + ("read write create getattr bind accept getopt shutdown"
                           if gamepad_policy_variant == "vsock-no-listen"
                           else "read write create getattr bind listen accept getopt shutdown")
                        + ")))\n"
                    )
                    + "(typeattributeset unconstrained_vsock_violators (hal_sensors_default"
                    + ("" if gamepad_policy_variant == "no-attribute" else " kernarium_gamepad")
                    + "))\n"
                    # The ClearKey HAL's rule for the memfd files of application domains: the
                    # generated attribute names the application domains less the isolated ones.
                    + (
                        ""
                        if clearkey_memfd_variant == "missing"
                        else "(typeattributeset base_typeattr_9_202604 (and (appdomain) "
                        "(not (" + ("isolated_app_202604" if clearkey_memfd_variant == "isolated-only"
                                    else "isolated_app_all_202604") + "))))\n"
                        "(allow hal_drm_clearkey_aidl "
                        + ("appdomain_202604" if clearkey_memfd_variant == "all-apps"
                           else "base_typeattr_9_202604")
                        + " (memfd_file ("
                        + ("getattr map read" if clearkey_memfd_variant == "no-write"
                           else "getattr map read write")
                        + ")))\n"
                    )
                    # The online grow of userdata by resize2fs, in the fsck domain.
                    + (
                        ""
                        if userdata_grow_policy_variant == "no-capability"
                        else "(allow fsck_202604 self (capability (sys_resource)))\n"
                    )
                    + (
                        ""
                        if userdata_grow_policy_variant == "no-dir-open"
                        else "(allow fsck_202604 system_data_root_file_202604 (dir (ioctl read open)))\n"
                    )
                    + (
                        ""
                        if userdata_grow_policy_variant == "no-ioctl"
                        else "(allowx fsck_202604 system_data_root_file_202604 (ioctl dir ("
                        + ("0x6610 0x5401" if userdata_grow_policy_variant == "more-ioctls" else "0x6610")
                        + ")))\n"
                    )
                    # The virtio-gpu device's sysfs tree, and the report file in it that has a type of its own.
                    + '(genfscon sysfs "/devices/virtio-uml-cmdline/virtio-uml.0" '
                    "(u object_r sysfs_gpu ((s0) (s0))))\n"
                    + {
                        "no-genfs": "",
                        "genfs-wrong-type":
                            '(genfscon sysfs "/devices/virtio-uml-cmdline/virtio-uml.0/drm/card0/'
                            'gpu_memory_report" (u object_r sysfs_gpu ((s0) (s0))))\n',
                        "genfs-other-path":
                            '(genfscon sysfs "/devices/virtio-uml-cmdline/virtio-uml.0/gpu_memory_report" '
                            "(u object_r sysfs_gpu_memory ((s0) (s0))))\n",
                    }.get(lmkd_variant,
                          '(genfscon sysfs "/devices/virtio-uml-cmdline/virtio-uml.0/drm/card0/'
                          'gpu_memory_report" (u object_r sysfs_gpu_memory_202604 ((s0) (s0))))\n'),
                )
                ueventd = "/dev/ttyS0 0660 root shell\n"
                if not missing_virtio_console:
                    ueventd += "/dev/hvc0 0660 root shell\n"
                if unsupported_hvc:
                    ueventd += "/dev/hvc1 0660 root shell\n"
                if not missing_video_ueventd:
                    ueventd += video_ueventd + "\n"
                archive.writestr("VENDOR/etc/ueventd.rc", ueventd)
                # The hardware video encoders: the Codec2 service, its plugin
                # store, the codec list, its seccomp policies and labels.
                archive.writestr(
                    f"VENDOR/bin/hw/{video_service}", synthetic_aarch64_elf(alignment)
                )
                archive.writestr("VENDOR/lib64/libc2plugin_store.so",
                                 synthetic_aarch64_elf(alignment))
                archive.writestr(
                    f"VENDOR/etc/init/{video_service}.rc", good_video_rc
                )
                if media_codecs_variant != "missing":
                    archive.writestr(
                        "VENDOR/etc/media_codecs_c2.xml",
                        b"<broken" if media_codecs_variant == "malformed" else video_codec_list(
                            software_updates=(
                                ("c2.android.vp9.decoder",) if media_codecs_variant == "no-hevc"
                                else ("c2.android.hevc.decoder", "c2.android.vp9.decoder")),
                            hevc_rate=("1-3000000" if media_codecs_variant == "low-hevc-rate"
                                       else "1-16711680"),
                            vp9_size=("2048x2048" if media_codecs_variant == "small-vp9-size"
                                      else "4096x4096"),
                            vp9_blocks=("1-16384" if media_codecs_variant == "small-vp9-blocks"
                                        else "1-65536")),
                    )
                archive.writestr(
                    f"VENDOR/etc/seccomp_policy/{video_seccomp_default}",
                    "read: 1\n",
                )
                archive.writestr(
                    f"VENDOR/etc/seccomp_policy/{video_seccomp_extended}",
                    good_video_seccomp,
                )
                archive.writestr(
                    "VENDOR/etc/selinux/vendor_file_contexts",
                    good_video_contexts.replace(good_net_setup_label, "")
                    if net_policy_variant == "missing-label" else good_video_contexts,
                )
                archive.writestr(
                    f"VENDOR/etc/vintf/manifest/{video_service}.xml",
                    '<manifest version="1.0" type="device"/>\n',
                )
                def application_payload(apk: str) -> bytes:
                    # Settings and SystemUI store their dex files uncompressed; the key that the p0033
                    # WifiTrackerLib topic's predicate reads is in the bytes.
                    if apk in wifi_tracker_apks:
                        variant = "settings-no-marker" if "/Settings/" in apk else "systemui-no-marker"
                        users = io.BytesIO()
                        with zipfile.ZipFile(users, "w", zipfile.ZIP_STORED) as application:
                            application.writestr("AndroidManifest.xml", release_manifest)
                            application.writestr(
                                "classes.dex",
                                b"dex\n039\x00"
                                + (b"" if wifi_tracker_variant == variant else b"networkProbesAttempted\x00"),
                            )
                        return users.getvalue()
                    # LineageParts and the Setup Wizard are real archives, so the check that
                    # LineageOS's statistics uploader is gone has members to search; the other
                    # applications stay the placeholder release archive.
                    if apk not in statistics_apks:
                        return release_apk
                    parts = apk.endswith("/LineageParts.apk")
                    marker = ""
                    dex = b"dex\n035\0 neutral"
                    table = b"\x00config_fixture\x00"
                    catalogue = "parts_catalog"
                    if statistics_variant == "parts-manifest" and parts:
                        marker = "org.lineageos.lineageparts.lineagestats.ReportingServiceManager"
                    if statistics_variant == "parts-dex" and parts:
                        dex += b" Lorg/lineageos/lineageparts/lineagestats/AnonymousStats;"
                    if statistics_variant == "parts-endpoint" and parts:
                        table += b"https://stats.lineageos.org/api/v1/stats\x00"
                    if statistics_variant == "parts-catalogue" and parts:
                        catalogue += " lineagestats"
                    if statistics_variant == "wizard-setting" and not parts:
                        dex += b" stats_collection"
                    # R8 keeps method names, so a wizard that still forces gestural navigation
                    # names the function in its dex.
                    dex += b" usesGestureNavigation" if not parts else b""
                    if navigation_variant == "dex-forces-gestural" and not parts:
                        dex += b" enableGesturalNavigation"
                    payload = io.BytesIO()
                    with zipfile.ZipFile(payload, "w") as application:
                        # The compiled manifest's string pool is UTF-16, so a marker named there is
                        # found in its UTF-16 form; the manifest is otherwise a release one.
                        application.writestr(
                            "AndroidManifest.xml",
                            synthetic_compiled_manifest(None, debuggable=False, permissions=(marker,))
                            if marker else release_manifest,
                        )
                        application.writestr("classes.dex", dex)
                        application.writestr("resources.arsc", table)
                        application.writestr("res/xml/parts_catalog.xml", catalogue.encode("utf-16-le"))
                    return payload.getvalue()

                for apk in preopt_apks:
                    archive.writestr(apk, application_payload(apk))
                    apk_path = Path(apk)
                    artifact = (
                        apk_path.parent / "oat" / "arm64" / apk_path.stem
                    )
                    if not (
                        missing_required_preopt
                        and apk.endswith("/CredentialManager.apk")
                    ):
                        archive.writestr(f"{artifact}.odex", b"odex")
                        if not (
                            half_preopt_pair and apk.endswith("/LatinIME.apk")
                        ):
                            archive.writestr(f"{artifact}.vdex", b"vdex")
                for apk in plain_apks:
                    archive.writestr(apk, application_payload(apk))
                # The text message service: the holder, the init script, the waker's script and the application.
                for entry, payload in sms_service_fixture(verifier, sms_service_variant).items():
                    if payload is not None:
                        archive.writestr(entry, payload)
                # The icon renderer: its own manifest, and its line of the package signing table.
                for entry, payload in icons["entries"].items():
                    if payload is not None:
                        archive.writestr(entry, payload)
                if icons["apkcerts"] is not None:
                    archive.writestr("META/apkcerts.txt", icons["apkcerts"])
                # The overlay that releases the home app's pin: built for SDK 28, with the
                # resource it sets in its table.
                pinner_overlay = io.BytesIO()
                with zipfile.ZipFile(pinner_overlay, "w") as overlay:
                    overlay.writestr(
                        "AndroidManifest.xml",
                        b"not a compiled manifest" if pinner_overlay_variant == "not-compiled"
                        else synthetic_compiled_manifest(
                            None if pinner_overlay_variant == "no-target-sdk"
                            else 29 if pinner_overlay_variant == "target-sdk-29"
                            else 37 if pinner_overlay_variant == "target-sdk-current"
                            else 28,
                            string_value=pinner_overlay_variant == "named-target-sdk",
                        ),
                    )
                    if pinner_overlay_variant != "no-table":
                        overlay.writestr(
                            "resources.arsc",
                            b"\x00" if pinner_overlay_variant == "no-resource"
                            else b"\x00config_pinnerHomePinBytes\x00",
                        )
                archive.writestr(pinner_overlay_entry, pinner_overlay.getvalue())
                # The Wi-Fi module's overlay: the thresholds that keep the guest's one network
                # enabled, and the ways the table can lack or lower one.
                permanent, temporary, never = wifi_permanent, wifi_temporary, wifi_never
                wifi_thresholds = {
                    None: [(permanent, never), (temporary, never)],
                    "compact": [(permanent, never), (temporary, never)],
                    "missing-permanent": [(temporary, never)],
                    "missing-temporary": [(permanent, never)],
                    "missing-both": [],
                    "no-table": [],
                    "permanent-one": [(permanent, 1), (temporary, never)],
                    "temporary-zero": [(permanent, never), (temporary, 0)],
                    "temporary-large-not-largest": [(permanent, never), (temporary, never - 1)],
                    "permanent-negative": [(permanent, -1), (temporary, never)],
                    "qualified-low": [(permanent, never), (temporary, never), (permanent, 1)],
                }[wifi_overlay_variant]
                wifi_overlay = io.BytesIO()
                with zipfile.ZipFile(wifi_overlay, "w") as overlay:
                    overlay.writestr("AndroidManifest.xml", release_manifest)
                    if wifi_overlay_variant != "no-table":
                        overlay.writestr(
                            "resources.arsc",
                            synthetic_resource_table(
                                [("config_fixture", [])], integers=wifi_thresholds,
                                compact=wifi_overlay_variant == "compact"),
                        )
                archive.writestr(wifi_overlay_entry, wifi_overlay.getvalue())
                # The runtime resource overlay Soong generates from the product's
                # SimpleDeviceConfig overlays. Its string pool holds each DeviceConfig list:
                # LineageOS's, then the product's, which adds the ops of AOSP's default.
                lineage_ops = (
                    "0,109,1,108,26,101,27,102,106,100,8,9,6,7,4,5,62,59,60,90,77,111,114,112,51,65,"
                    "13,6,7,52,53,54,69,74,79,113,20,16,14,19,18,57"
                )
                device_ops = (
                    "0,1,2,4,5,6,7,8,9,13,14,16,18,19,20,25,26,27,41,42,51,52,53,54,57,59,60,62,63,"
                    "65,69,73,74,77,79,88,89,90,100,101,102,106,108,109,111,112,113,114,120,136,141,"
                    "147,149,150,152,170,171,172,173"
                )
                if discrete_ops_variant == "short-device-list":
                    device_ops = device_ops.replace(",141,", ",")
                if discrete_ops_variant != "missing-overlay":
                    key = b"privacy/discrete_history_ops_cslist="
                    table = b"\x00" + key + lineage_ops.encode() + b"\x00"
                    if discrete_ops_variant != "lineage-list-only":
                        table += key + device_ops.encode() + b"\x00"
                    overlay_apk = io.BytesIO()
                    with zipfile.ZipFile(overlay_apk, "w") as overlay:
                        overlay.writestr("AndroidManifest.xml", release_manifest)
                        overlay.writestr("resources.arsc", table)
                    archive.writestr(
                        "PRODUCT/overlay/"
                        "SimpleDeviceConfig__lineage_andarium__auto_generated_rro_product.apk",
                        overlay_apk.getvalue(),
                    )
                if wizard_branding_variant != "missing-overlay":
                    # The Setup Wizard's overlay: the default-locale strings are in its table, as
                    # UTF-16 when the build packs the pool that way.
                    wizard_table = (
                        b"\x00os_name\x00Andarium features\x00Update Andarium Recovery\x00"
                        b"Updates Andarium Recovery on first boot subsequent to every update.\x00"
                    )
                    if wizard_branding_variant == "utf16-table":
                        wizard_table = wizard_table.decode().encode("utf-16-le")
                    if wizard_branding_variant == "lineage-name":
                        wizard_table += b"LineageOS features\x00"
                    if wizard_branding_variant == "lineage-name-utf16":
                        wizard_table += "Update Lineage Recovery".encode("utf-16-le")
                    if wizard_branding_variant == "no-recovery-strings":
                        wizard_table = b"\x00os_name\x00Andarium features\x00"
                    if wizard_branding_variant == "lineage-features":
                        wizard_table = wizard_table.replace(b"Andarium features", b"Features")
                    wizard_logo = b"\x00".join(path.encode() for path in wizard_paths)
                    wizard_animation = wizard_welcome
                    if wizard_branding_variant == "stock-logo":
                        wizard_logo = b"\x00M0,0L1,1\x00"
                    elif wizard_branding_variant == "logo-lacks-a-path":
                        wizard_logo = b"\x00".join(path.encode() for path in wizard_paths[:-1])
                    elif wizard_branding_variant == "stock-animation":
                        wizard_animation = wizard_welcome.replace(b"Andarium", b"LineageOS", 1) + b" "
                    wizard_apk = io.BytesIO()
                    with zipfile.ZipFile(wizard_apk, "w") as overlay:
                        overlay.writestr("AndroidManifest.xml", release_manifest)
                        overlay.writestr("resources.arsc", wizard_table)
                        if wizard_branding_variant != "no-logo":
                            overlay.writestr("res/drawable-v24/logo.xml", wizard_logo)
                        if wizard_branding_variant != "no-animation":
                            overlay.writestr("res/raw/lottie_welcome.json", wizard_animation)
                        for script_name, script in wizard_scripts.items():
                            if wizard_branding_variant == "no-script" and script_name == owner_name:
                                continue
                            if wizard_branding_variant == "no-user-script" and script_name == user_name:
                                continue
                            if wizard_branding_variant == "stock-script" and script_name == owner_name:
                                script = script.replace(b'wizard:action="datetime"', b'wizard:action="network_setup"')
                            if wizard_branding_variant == "stock-user-script" and script_name == user_name:
                                script += b"\n"
                            overlay.writestr(f"res/raw/{script_name}", script)
                    archive.writestr(
                        "PRODUCT/overlay/"
                        "LineageSetupWizard__lineage_andarium__auto_generated_rro_product.apk",
                        wizard_apk.getvalue(),
                    )
                # The two navigation mode overlays stay in the product partition, so that Settings can
                # switch either way; no configuration enables one.
                for mode in ("3Button", "Gestural"):
                    if navigation_variant == f"no-{mode.lower()}-overlay":
                        continue
                    archive.writestr(
                        f"PRODUCT/overlay/NavigationBarMode{mode}/NavigationBarMode{mode}Overlay.apk",
                        release_apk,
                    )
                navigation_configuration = {
                    "config-enables-gestural": (
                        "PRODUCT/overlay/config/config.xml",
                        '<config><overlay package="com.android.internal.systemui.navbar.gestural" enabled="true"/></config>',
                    ),
                    "config-enables-3button-undisabled": (
                        "VENDOR/overlay/config/config.xml",
                        '<config><merge path="x"/><overlay package="com.android.internal.systemui.navbar.threebutton"/></config>',
                    ),
                    "config-unreadable": ("SYSTEM_EXT/overlay/config/config.xml", "<config>"),
                    "config-disables-gestural": (
                        "PRODUCT/overlay/config/config.xml",
                        '<config><overlay package="com.android.internal.systemui.navbar.gestural" enabled="false"/>'
                        '<overlay package="com.android.theme.icon_pack.circular.android" enabled="true"/></config>',
                    ),
                }.get(navigation_variant)
                if navigation_configuration is not None:
                    archive.writestr(*navigation_configuration)
                # The launcher's default workspace: DocumentsUI is the one of the six applications whose APK the
                # required list does not name, and the overlay holds a compiled layout per grid, whose strings
                # are the component names.
                if launcher_variant != "no-documentsui":
                    archive.writestr("SYSTEM/priv-app/DocumentsUI/DocumentsUI.apk", release_apk)
                # The easter egg that the platform logo screen opens; the framework's list installs it
                # as a platform application.
                if easter_egg_variant is None:
                    archive.writestr("SYSTEM/app/EasterEgg/EasterEgg.apk", release_apk)
                elif easter_egg_variant == "product-partition":
                    archive.writestr("PRODUCT/app/EasterEgg/EasterEgg.apk", release_apk)
                elif easter_egg_variant == "wrong-directory":
                    archive.writestr("SYSTEM/app/Egg/EasterEgg.apk", release_apk)
                # TalkBack and eSpeak NG, the image build's two APKs.
                if accessibility_variant not in ("missing-talkback", "talkback-in-system"):
                    archive.writestr("PRODUCT/app/TalkBack/TalkBack.apk", release_apk)
                if accessibility_variant == "talkback-in-system":
                    archive.writestr("SYSTEM/app/TalkBack/TalkBack.apk", release_apk)
                if accessibility_variant != "missing-espeak":
                    archive.writestr("PRODUCT/app/eSpeakNG/eSpeakNG.apk", release_apk)
                if launcher_variant != "no-overlay":
                    layout_strings = [name for pair in launcher_dock for name in pair]
                    if launcher_variant == "six-application-layout":
                        layout_strings = [name for pair in launcher_apps for name in pair]
                    if launcher_variant == "stock-layout":
                        layout_strings.append("com.google.android.googlequicksearchbox")
                    if launcher_variant == "lacks-files":
                        layout_strings.remove("com.android.documentsui.LauncherActivity")
                    launcher_apk = io.BytesIO()
                    with zipfile.ZipFile(launcher_apk, "w") as overlay:
                        overlay.writestr("AndroidManifest.xml", release_manifest)
                        overlay.writestr("resources.arsc", b"\x00config_fixture\x00")
                        for grid in launcher_grids:
                            if launcher_variant == "no-6x5" and grid == "6x5":
                                continue
                            strings = "\x00".join(layout_strings)
                            overlay.writestr(
                                f"res/xml/default_workspace_{grid}.xml",
                                strings.encode("utf-16-le" if launcher_variant == "utf16-layout" else "utf-8"),
                            )
                    archive.writestr(launcher_overlay_entry, launcher_apk.getvalue())
                if wallpaper_variant != "missing-overlay":
                    # The framework's runtime resource overlay: Soong lists the tablet default
                    # wallpapers with the version qualifier aapt2 adds to a smallest-width one.
                    def png_header(width: int, height: int) -> bytes:
                        header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
                        return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + header
                                + struct.pack(">I", zlib.crc32(b"IHDR" + header)))

                    wallpapers = {
                        "res/drawable-sw600dp-nodpi-v13/default_wallpaper.png": png_header(1024, 1024),
                        "res/drawable-sw720dp-nodpi-v13/default_wallpaper.png": png_header(1024, 1024),
                        "res/drawable-xxxhdpi-v4/default_wallpaper.png": png_header(2560, 2560),
                    }
                    if wallpaper_variant == "missing-variant":
                        del wallpapers["res/drawable-sw720dp-nodpi-v13/default_wallpaper.png"]
                    elif wallpaper_variant == "lineage-size":
                        wallpapers["res/drawable-sw600dp-nodpi-v13/default_wallpaper.png"] = png_header(1920, 1920)
                    elif wallpaper_variant == "not-square":
                        wallpapers["res/drawable-sw720dp-nodpi-v13/default_wallpaper.png"] = png_header(1024, 768)
                    elif wallpaper_variant == "not-png":
                        wallpapers["res/drawable-sw720dp-nodpi-v13/default_wallpaper.png"] = b"not a png at all, just bytes"
                    # The product's network time: no NTP server the client accepts, and no network origin.
                    network_time = {
                        "config_ntpServers": ["none"],
                        "config_autoTimeSourcesPriority": ["telephony", "external"],
                    }
                    extra_definitions: list[tuple[str, list[str]]] = []
                    if network_time_variant == "ntp-server":
                        network_time["config_ntpServers"] = ["ntp://time.android.com"]
                    elif network_time_variant == "ntp-server-in-a-qualified-table":
                        extra_definitions.append(("config_ntpServers", ["ntp://ntp.aliyun.com"]))
                    elif network_time_variant == "ntp-server-in-capitals":
                        network_time["config_ntpServers"] = ["NTP://time.example"]
                    elif network_time_variant == "ntp-empty":
                        network_time["config_ntpServers"] = []
                    elif network_time_variant == "ntp-missing":
                        del network_time["config_ntpServers"]
                    elif network_time_variant == "origin-network":
                        network_time["config_autoTimeSourcesPriority"] = ["network", "telephony", "external"]
                    elif network_time_variant == "origin-empty":
                        network_time["config_autoTimeSourcesPriority"] = []
                    elif network_time_variant == "origin-without-external":
                        network_time["config_autoTimeSourcesPriority"] = ["telephony"]
                    elif network_time_variant == "origin-missing":
                        del network_time["config_autoTimeSourcesPriority"]
                    # The product's flip-to-screen-off switch, off in the default configuration.
                    flip_values = [False]
                    if flip_variant == "enabled":
                        flip_values = [True]
                    elif flip_variant == "enabled-in-a-qualified-table":
                        flip_values = [False, True]
                    elif flip_variant == "missing":
                        flip_values = []
                    toggle_values = {
                        "toggle-off": [False],
                        "toggle-missing": [],
                        "toggle-duplicate": [True, False],
                    }.get(camera_variant, [True])
                    # The reminder activities of the sensor privacy service stay at the platform's defaults:
                    # a variant defines one, as a literal or a reference, by default or under a qualifier.
                    # Another sensor privacy string is present in every fixture: only these names count.
                    reminder_strings = [("config_sensorStateChangedActivity", "com.example/.State")] + {
                        "reminder-string": [("config_sensorUseStartedActivity", "com.example/.Quiet")],
                        "reminder-hardware-string": [("config_sensorUseStartedActivity_hwToggle", "com.example/.Quiet")],
                        "reminder-reference": [("config_sensorUseStartedActivity_hwToggle", None)],
                        "reminder-qualified": [("config_sensorUseStartedActivity", "com.example/.Quiet")],
                    }.get(camera_variant, [])
                    network_table = synthetic_resource_table(
                        [*network_time.items(), *extra_definitions], utf8=network_time_variant != "utf16-pool",
                        booleans=[("config_flipToScreenOffEnabled", value) for value in flip_values]
                        + [("config_supportsHardwareCamToggle", value) for value in toggle_values],
                        compact=flip_variant == "compact",
                        qualified=frozenset({"config_supportsHardwareCamToggle"})
                        if camera_variant == "toggle-only-qualified"
                        else frozenset({"config_sensorUseStartedActivity"})
                        if camera_variant == "reminder-qualified" else frozenset(),
                        strings=reminder_strings,
                    )
                    framework_apk = io.BytesIO()
                    with zipfile.ZipFile(framework_apk, "w") as overlay:
                        # The generated overlay declares itself static and has no resource map; the variant
                        # carries a real compiled manifest that does.
                        overlay.writestr("AndroidManifest.xml",
                                         RESOURCES_MAP_OVERLAY_MANIFEST if camera_variant == "reminder-resource-map"
                                         else synthetic_overlay_manifest("android", True))
                        overlay.writestr("resources.arsc", network_table)
                        for name, payload in wallpapers.items():
                            overlay.writestr(name, payload)
                    archive.writestr(
                        "PRODUCT/overlay/"
                        "framework-res__lineage_andarium__auto_generated_rro_product.apk",
                        framework_apk.getvalue(),
                    )
                for apk in arm_preopt_apks:
                    apk_path = Path(apk)
                    for isa in ("arm64", "arm"):
                        artifact = apk_path.parent / "oat" / isa / apk_path.stem
                        archive.writestr(f"{artifact}.odex", b"odex")
                        archive.writestr(f"{artifact}.vdex", b"vdex")
                archive.writestr("VENDOR/bin/hw/android.hardware.sensors-service.multihal", synthetic_aarch64_elf())
                archive.writestr("VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so",
                                 synthetic_aarch64_elf() + good_motion_sub_hal_text)
                # The compass feature, in the directory SystemConfig reads for a host with a magnetometer.
                archive.writestr("VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml",
                                 '<permissions><feature name="android.hardware.sensor.compass"/></permissions>')
                archive.writestr("VENDOR/etc/sensors/hals.conf",
                                 "/vendor/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so\n")
                archive.writestr("VENDOR/etc/init/android.hardware.sensors-service-multihal.rc",
                                 "service vendor.sensors-hal-multihal /vendor/bin/hw/android.hardware.sensors-service.multihal\n")
                archive.writestr("VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml",
                                 '<manifest version="1.0" type="device"><hal format="aidl">'
                                 '<name>android.hardware.sensors</name><version>3</version>'
                                 '<fqname>ISensors/default</fqname></hal></manifest>')
                archive.writestr("VENDOR/bin/hw/android.hardware.vibrator-service.andarium", synthetic_aarch64_elf())
                archive.writestr(vibrator_rc_path, good_vibrator_rc)
                archive.writestr("ODM/etc/vintf/manifest_haptics.xml", good_vibrator_manifest)
                archive.writestr("VENDOR/bin/hw/android.hardware.gnss-service.andarium", synthetic_aarch64_elf())
                archive.writestr(gnss_rc_path, good_gnss_rc)
                archive.writestr(gnss_fragment_path, good_gnss_manifest)
                for camera_entry, camera_payload in camera_layouts.installed().items():
                    archive.writestr(camera_entry, camera_payload)
                archive.writestr(camera_provider_path, synthetic_aarch64_elf())
                archive.writestr(camera_rc_path, good_camera_rc)
                archive.writestr(camera_fragment_path, good_camera_manifest)
                archive.writestr(camera_library_path, camera_library(*good_camera_markers))
                archive.writestr(camera_template_path, good_camera_template)
                # Aperture as the build leaves it: its own resources keep the defaults (the lens settings
                # off, no ignored ID, free zoom on) and declare no overlayable section; the product's values
                # are in the static overlay beside the other auto-generated ones. With the values on, each
                # lens of the host is its own camera device, a host's camera 2 stays out of the list, and
                # the lens buttons are the only zoom.
                aux_booleans = {
                    None: [("config_enableAuxCameras", True)],
                    "aux-off": [("config_enableAuxCameras", False)],
                    "aux-missing": [],
                    "aux-duplicate": [("config_enableAuxCameras", True), ("config_enableAuxCameras", False)],
                }[camera_variant if camera_variant in {"aux-off", "aux-missing", "aux-duplicate"} else None]
                ignored_ids = {
                    "ignored-extra": [["2", "3"]],
                    "ignored-none": [[]],
                    "ignored-missing": [],
                }.get(camera_variant, [["2"]])
                zoom_booleans = {
                    "zoom-on": [("config_enableZoom", True)],
                    "zoom-missing": [],
                    "zoom-duplicate": [("config_enableZoom", False), ("config_enableZoom", True)],
                }.get(camera_variant, [("config_enableZoom", False)])
                # "...-only-qualified": the one definition sits under a resource configuration (portrait), so
                # it applies on a device in that orientation alone, whatever its value says.
                overlay_qualified = {
                    "aux-only-qualified": frozenset({"config_enableAuxCameras"}),
                    "ignored-only-qualified": frozenset({"config_ignoredAuxCameraIds"}),
                    "zoom-only-qualified": frozenset({"config_enableZoom"}),
                }.get(camera_variant, frozenset())
                overlay_table = synthetic_resource_table(
                    [("config_ignoredAuxCameraIds", items) for items in ignored_ids],
                    booleans=aux_booleans + zoom_booleans, compact=camera_variant == "compact",
                    qualified=overlay_qualified,
                )
                # "app-values": the old assumption, the values in the application's own table and no overlay
                # "app-no-aux", "app-no-ignored", "app-no-zoom": the application lost a name the overlay replaces (an
                # uprev renamed it), so the overlay applies to nothing; "app-aux-qualified", "app-zoom-qualified": it
                # keeps one under a qualifier only.
                aperture_table = synthetic_resource_table(
                    [] if camera_variant == "app-no-ignored" else
                    [("config_ignoredAuxCameraIds", ["2"] if camera_variant == "app-values" else [])],
                    booleans=([] if camera_variant == "app-no-aux" else
                              [("config_enableAuxCameras", camera_variant == "app-values")])
                    + ([] if camera_variant == "app-no-zoom" else [("config_enableZoom", True)]),
                    compact=camera_variant == "compact", overlayable=camera_variant == "app-overlayable",
                    qualified=frozenset({"config_enableAuxCameras"}) if camera_variant == "app-aux-qualified"
                    else frozenset({"config_enableZoom"}) if camera_variant == "app-zoom-qualified"
                    else frozenset(),
                )
                # ... and a file that is no resource table, or one cut short, must not read as a table
                # without an overlayable section.
                if camera_variant == "app-not-a-table":
                    aperture_table = b"not a resource table"
                elif camera_variant == "app-truncated-table":
                    aperture_table = aperture_table[:len(aperture_table) // 2]
                aperture_apk = io.BytesIO()
                with zipfile.ZipFile(aperture_apk, "w") as apk:
                    apk.writestr("AndroidManifest.xml", synthetic_compiled_manifest(None))
                    apk.writestr("resources.arsc", aperture_table)
                archive.writestr(camera_app_path, aperture_apk.getvalue())
                overlay_apk = io.BytesIO()
                overlay_target, overlay_static = {
                    "overlay-target": ("com.example.other", True),
                    "overlay-not-static": (camera_app_package, False),
                    "overlay-static-missing": (camera_app_package, None),
                    "overlay-target-missing": (None, True),
                }.get(camera_variant, (camera_app_package, True))
                with zipfile.ZipFile(overlay_apk, "w") as apk:
                    apk.writestr("AndroidManifest.xml", synthetic_overlay_manifest(
                        overlay_target, overlay_static, utf8=camera_variant == "compact"))
                    apk.writestr("resources.arsc", overlay_table)
                if camera_variant not in {"app-values", "overlay-missing"}:
                    archive.writestr(camera_overlay_path, overlay_apk.getvalue())
                archive.writestr("VENDOR/bin/hw/android.hardware.thermal-service.andarium", synthetic_aarch64_elf())
                archive.writestr(thermal_rc_path, good_thermal_rc)
                archive.writestr(thermal_fragment_path, good_thermal_manifest)
                archive.writestr(display_config_path, good_display_config)
                archive.writestr(gamepad_binary_path, synthetic_aarch64_elf())
                archive.writestr(gamepad_rc_path, good_gamepad_rc)
                # The sample file of the platform: comments only, no assistance server.
                archive.writestr("SYSTEM/etc/gps_debug.conf", "# SUPL_HOST=supl.example\n# NTP_SERVER=ntp.example\n\n")
                advertised_features = ["android.hardware.touchscreen",
                                       "android.hardware.sensor.accelerometer",
                                       "android.hardware.sensor.gyroscope",
                                       "android.hardware.telephony.messaging"]
                advertised_features.extend(
                    feature for feature in (
                        "android.hardware.touchscreen.multitouch",
                        "android.hardware.touchscreen.multitouch.distinct",
                        "android.hardware.touchscreen.multitouch.jazzhand",
                    ) if feature != missing_multitouch_feature
                )
                advertised_features.extend(
                    feature for feature in ("android.hardware.location", "android.hardware.location.gps")
                    if feature != missing_location_feature
                )
                if not missing_microphone_feature:
                    advertised_features.append("android.hardware.microphone")
                if not missing_companion_feature:
                    advertised_features.append("android.software.companion_device_setup")
                if not missing_midi_feature:
                    advertised_features.append("android.software.midi")
                if unsupported_feature:
                    advertised_features.append("android.hardware.camera")
                # The virtual Wi-Fi's feature declaration, a file of the platform's own.
                if not missing_wifi_feature:
                    archive.writestr(
                        "VENDOR/etc/permissions/android.hardware.wifi.xml",
                        '<permissions><feature name="android.hardware.wifi"/></permissions>',
                    )
                if wifi_feature_file is not None:
                    archive.writestr(
                        f"VENDOR/etc/permissions/{wifi_feature_file}.xml",
                        f'<permissions><feature name="{wifi_feature_file}"/></permissions>',
                    )
                # The GLES claim's two platform files: the extension pack and
                # the qualified dEQP level.
                archive.writestr(
                    "VENDOR/etc/permissions/android.hardware.opengles.aep.xml",
                    '<permissions><feature name="android.hardware.opengles.aep"/></permissions>',
                )
                archive.writestr(
                    "VENDOR/etc/permissions/android.software.opengles.deqp.level-latest.xml",
                    '<permissions><feature name="android.software.opengles.deqp.level" '
                    'version="132776705"/></permissions>',
                )
                archive.writestr(
                    "VENDOR/etc/permissions/handheld_core_hardware.xml",
                    "<permissions>"
                    + "".join(f'<feature name="{name}"/>' for name in advertised_features)
                    + "</permissions>",
                )
                archive.writestr(
                    "VENDOR/etc/permissions/bluetooth_excluded_hardware.xml",
                    '<permissions><unavailable-feature '
                    'name="android.hardware.bluetooth"/></permissions>',
                )
                archive.writestr(
                    "RECOVERY/RAMDISK/system/bin/recovery", b"recovery"
                )
                archive.writestr(
                    "RECOVERY/RAMDISK/system/bin/hw/"
                    "android.hardware.health-service.example_recovery",
                    b"health-recovery",
                )
                archive.writestr(
                    "VENDOR/etc/vintf/compatibility_matrix.xml",
                    "<compatibility-matrix/>\n",
                )
                archive.writestr("VENDOR/etc/vintf/manifest.xml", "<manifest/>\n")
                mesa_gles_entries = (
                    "VENDOR/lib64/venus_gallium_dri.so",
                    "VENDOR/lib64/egl/libEGL_kernarium.so",
                    "VENDOR/lib64/egl/libGLESv1_CM_kernarium.so",
                    "VENDOR/lib64/egl/libGLESv2_kernarium.so",
                )
                for index, entry in enumerate(mesa_gles_entries):
                    if missing_mesa_gles and index == 0:
                        continue
                    payload = (
                        b"not an ELF"
                        if bad_mesa_gles_elf and index == 0
                        else synthetic_aarch64_elf(
                            mesa_gles_alignment,
                            ()
                            if missing_gallium_dependency or index == 0
                            else ("venus_gallium_dri.so",),
                            imports=LARGE_FILE_IMPORTS_64,
                        )
                    )
                    archive.writestr(entry, payload)
                for index, entry in enumerate((
                    "VENDOR/lib/venus_gallium_dri.so",
                    "VENDOR/lib/egl/libEGL_kernarium.so",
                    "VENDOR/lib/egl/libGLESv1_CM_kernarium.so",
                    "VENDOR/lib/egl/libGLESv2_kernarium.so",
                )):
                    archive.writestr(
                        entry,
                        synthetic_arm_elf(
                            needed=()
                            if missing_arm_gallium_dependency or index == 0
                            else ("venus_gallium_dri.so",),
                            imports=LARGE_FILE_IMPORTS_32,
                        ),
                    )
                for hal in ("vulkan.virtio", "gralloc.minigbm", "mapper.minigbm"):
                    archive.writestr(
                        f"VENDOR/lib64/hw/{hal}.so",
                        synthetic_aarch64_elf(imports=LARGE_FILE_IMPORTS_64),
                    )
                    archive.writestr(
                        f"VENDOR/lib/hw/{hal}.so", synthetic_arm_elf(imports=LARGE_FILE_IMPORTS_32)
                    )
                if retired_egl_library:
                    archive.writestr(
                        "VENDOR/lib64/egl/libGLES_mesa.so", synthetic_aarch64_elf()
                    )
                if retired_dri_driver:
                    archive.writestr(
                        "VENDOR/lib64/dri/gallium_dri.so", synthetic_aarch64_elf()
                    )
                archive.writestr(
                    "VENDOR/build.prop",
                    # The vendor partition's own build identity: the build's, or (vendor_build_identity)
                    # one that an earlier build left behind.
                    {
                        None: (
                            f"ro.vendor.build.fingerprint={public_fingerprint}\n"
                            f"ro.vendor.build.type={build_variant}\n"
                            "ro.vendor.build.tags=release-keys\n"
                            "ro.vendor.build.id=FIXTURE\n"
                            "ro.vendor.build.version.incremental=7\n"
                        ),
                        "eng": (
                            "ro.vendor.build.fingerprint=Andarium/lineage_andarium/andarium:17/FIXTURE/eng.build:userdebug/release-keys\n"
                            "ro.vendor.build.type=userdebug\n"
                            "ro.vendor.build.tags=release-keys\n"
                            "ro.vendor.build.id=FIXTURE\n"
                            "ro.vendor.build.version.incremental=eng.build\n"
                        ),
                        "missing": "",
                    }[vendor_build_identity]
                    + "".join(
                        f"ro.product.vendor.{key}={value}\n"
                        for key, value in vendor_identity.items()
                    )
                    + abi_properties("vendor", vendor_abilist32)
                    + f"ro.zygote={zygote}\n"
                    "# Removed by post_process_props.py because overridden by "
                    "ro.zygote=zygote64_32\n#ro.zygote?=zygote32\n"
                    + ("dalvik.vm.dex2oat64.enabled=true\n" if dex2oat64 else "")
                    + "ro.bionic.2nd_arch=arm\n"
                    "ro.bionic.2nd_cpu_variant=cortex-a53\n"
                    "dalvik.vm.isa.arm.variant=cortex-a53\n"
                    # post_process_props.py comments out a denylisted vendor property; the
                    # product denylists the unreliable present fence of the inherited config.
                    "#ro.vendor.hwc.drm.present_fence_not_reliable=true\n"
                    # The board's own name, and the inherited SoC values, which the denylist
                    # comments out of the vendor partition (the odm file states the SoC).
                    + {
                        None: "ro.product.board=andarium\n",
                        "cutf": "ro.product.board=cutf\n",
                        "missing": "",
                    }[board_variant]
                    + "#ro.soc.manufacturer=Google\n#ro.soc.model=vsoc_arm64_pgagnostic\n"
                    + {
                        "config-property-missing": "",
                        "config-property-stock": "ro.vendor.camera.config=back\n",
                        "extensions-property": "ro.vendor.camera.config=v4l2\nro.camerax.extensions.enabled=true\n",
                    }.get(camera_variant, "ro.vendor.camera.config=v4l2\n")
                    + {
                        "vendor-manufacturer": "ro.soc.manufacturer=Google\n",
                        "vendor-model": "ro.soc.model=vsoc_arm64_pgagnostic\n",
                        "vendor-other": "ro.soc.et=true\n",
                        "vendor-same": "ro.soc.manufacturer=Kernarium\nro.soc.model=Virtual CPU\n",
                    }.get(soc_variant, "")
                    + {
                        "present-fence-unreliable":
                            "ro.vendor.hwc.drm.present_fence_not_reliable=true\n",
                        "no-incremental": "",
                        "incremental-off": "ro.incremental.enable=0\n",
                    }.get(vendor_property_variant, "ro.incremental.enable=1\n")
                    # The primary audio HAL's ALSA ring: four periods of one HAL buffer each.
                    + {
                        "missing": "",
                        "default-count": "ro.hardware.audio.tinyalsa.period_count=2\n"
                        "ro.hardware.audio.tinyalsa.period_size_multiplier=4\n",
                    }.get(
                        audio_ring_variant,
                        "ro.hardware.audio.tinyalsa.period_count=4\n"
                        "ro.hardware.audio.tinyalsa.period_size_multiplier=4\n",
                    )
                    + (
                        f"ro.hardware.{vendor_hal_property}=selected\n"
                        if vendor_hal_property
                        else ""
                    )
                    + "".join(
                        f"{name}={value}\n"
                        for name, value in (
                            good_video_properties
                            if video_properties is None
                            else video_properties
                        ).items()
                    ),
                )
                soc_properties = {
                    "ro.soc.manufacturer": "Kernarium",
                    "ro.soc.model": "Virtual CPU",
                }
                soc_properties.update({
                    None: {},
                    "missing-manufacturer": {"ro.soc.manufacturer": None},
                    "missing-model": {"ro.soc.model": None},
                    "cuttlefish": {"ro.soc.manufacturer": "Google",
                                   "ro.soc.model": "vsoc_arm64_pgagnostic"},
                    "joined-model": {"ro.soc.model": "Kernarium Virtual CPU"},
                    "lowercase-model": {"ro.soc.model": "Virtual cpu"},
                    "unknown-model": {"ro.soc.model": "unknown"},
                    "vendor-manufacturer": {},
                    "vendor-model": {},
                    "vendor-other": {},
                    "vendor-same": {},
                }[soc_variant])
                archive.writestr(
                    "ODM/etc/build.prop",
                    abi_properties("odm", "armeabi-v7a,armeabi")
                    + "".join(
                        f"{name}={value}\n"
                        for name, value in soc_properties.items()
                        if value is not None
                    ),
                )
                if module:
                    archive.writestr("VENDOR/lib/modules/forbidden.ko", b"module")
                if module_loader:
                    archive.writestr("VENDOR/bin/dlkm_loader", b"loader")
                if dlkm_image:
                    archive.writestr("SYSTEM_DLKM/etc/build.prop", "")
                if dynamic_image:
                    archive.writestr("IMAGES/super_empty.img", b"dynamic")
                if ota_tool:
                    archive.writestr("OTA/bin/updater", b"updater")
                if stock_fstab:
                    archive.writestr("VENDOR/etc/fstab.cf.ext4.hctr2", b"stock")
                if seriallogging:
                    archive.writestr("VENDOR/etc/init/seriallogging.rc", b"stock")
                if boot_control:
                    archive.writestr(
                        "VENDOR/apex/com.android.hardware.boot.apex", b"apex"
                    )
                if context_hub:
                    archive.writestr(
                        "VENDOR/apex/com.android.hardware.contexthub.apex", b"apex"
                    )
                if unsupported_hal:
                    archive.writestr("VENDOR/apex/com.google.cf.light.apex", b"apex")
                # The adb daemon in its APEX: bin/adbd is a file of the payload, stored uncompressed (the payload
                # hints), and holds the names of the launch properties that the p0011 topics read.
                adbd_content = b"\x00" * 8
                if adbd_variant != "no-key":
                    adbd_content += b"ro.boot.kernarium.adb_key\x00"
                if adbd_variant != "no-tcp":
                    adbd_content += b"ro.boot.kernarium.adb_tcp\x00"
                if adbd_variant != "no-debugging-setting":
                    adbd_content += b"persist.sys.usb.config\x00persist.adb.tls_server.enable\x00"
                if adbd_variant != "no-key-rule":
                    adbd_content += b"kernarium: key refused, debugging is off\x00"
                # The adb_root service of the same route: its log line for a build that is not debuggable.
                if adbd_variant != "no-root-service":
                    archive.writestr(
                        "SYSTEM_EXT/bin/adb_root",
                        b"\x00" * 8
                        + (b"" if adbd_variant == "no-root-guard"
                           else b"kernarium: rooted debugging stays off, this build is not debuggable\x00"),
                    )
                if adbd_variant != "missing-apex":
                    archive.writestr(
                        "SYSTEM/apex/com.android.adbd.apex",
                        synthetic_apex(
                            "erofs", adbd_content,
                            files=("etc/init.rc",) if adbd_variant == "no-binary" else ("bin/adbd",),
                        ),
                    )
                # The framework's services.jar stores its dex files uncompressed; the p0007 AdbService
                # topic's log line is in the bytes.
                if services_jar_variant != "missing":
                    services_jar = io.BytesIO()
                    with zipfile.ZipFile(services_jar, "w", zipfile.ZIP_STORED) as jar:
                        jar.writestr(
                            "classes.dex",
                            b"dex\n039\x00"
                            + (b"" if services_jar_variant == "no-marker"
                               else b"kernarium: USB debugging is now \x00")
                            + (b"" if services_jar_variant == "no-display-marker"
                               else b"RefreshRateThrottling: using the map of the default configuration\x00")
                            + (b"" if services_jar_variant == "no-compass-marker"
                               else b"ro.boot.kernarium.compass\x00")
                            + (b"" if services_jar_variant == "no-compass-directory-marker"
                               else b"kernarium_compass\x00")
                            + (b"" if services_jar_variant == "no-sensor-reminder-marker"
                               else b"kernarium: no reminder for a camera that the hardware toggle blocks (\x00")
                        )
                    archive.writestr("SYSTEM/framework/services.jar", services_jar.getvalue())
                # The old-application layer: a 32-bit ARM shared object with a JNI_OnLoad, and the
                # framework's ActivityThread topic that loads it (framework.jar stores its dex files
                # uncompressed, so the topic's strings are in the bytes).
                if oldapp_variant != "missing-library":
                    archive.writestr(
                        "SYSTEM/lib/libkernarium_oldapp.so",
                        (synthetic_aarch64_elf() if oldapp_variant == "not-arm" else synthetic_arm_elf())
                        + (b"" if oldapp_variant == "no-entry" else b"\x00JNI_OnLoad\x00"),
                    )
                if oldapp_variant != "missing-framework":
                    framework_jar = io.BytesIO()
                    with zipfile.ZipFile(framework_jar, "w", zipfile.ZIP_STORED) as jar:
                        jar.writestr(
                            "classes.dex",
                            b"dex\n039\x00"
                            + (b"" if oldapp_variant == "no-load-marker"
                               else b"/system/lib/libkernarium_oldapp.so\x00")
                            + (b"" if oldapp_variant == "no-log-marker"
                               else b"kernarium: old-application layer not loaded\x00")
                            + (b"" if oldapp_variant == "no-switch-marker"
                               else b"persist.sys.kernarium.oldapp.\x00"),
                        )
                    archive.writestr("SYSTEM/framework/framework.jar", framework_jar.getvalue())
                # libinputflinger.so holds the p0022 pointer capture LED topic's dump line.
                if input_flinger_variant != "missing":
                    archive.writestr(
                        "SYSTEM/lib64/libinputflinger.so",
                        b"\x00" * 8
                        + (b"" if input_flinger_variant == "no-marker"
                           else b"      PointerCaptureLed: %s\n\x00"),
                    )
                # NetworkStack's APK stores its dex file uncompressed; the p0029 NetworkMonitor topic's
                # validation log lines are in the bytes.
                if network_stack_variant != "missing":
                    network_stack = io.BytesIO()
                    with zipfile.ZipFile(network_stack, "w", zipfile.ZIP_STORED) as apk:
                        apk.writestr("AndroidManifest.xml", synthetic_compiled_manifest(None))
                        apk.writestr(
                            "classes.dex",
                            b"dex\n039\x00"
                            + (b"" if network_stack_variant == "no-marker"
                               else b"Wi-Fi is validated before its internet check\x00")
                            + (b"" if network_stack_variant in ("no-marker", "no-check-marker")
                               else b"Wi-Fi internet check failed, the network stays validated\x00"),
                        )
                    archive.writestr("SYSTEM/priv-app/NetworkStack/NetworkStack.apk", network_stack.getvalue())
                # The OTA certificates, the system's and the recovery's: an unsigned build of either variant holds
                # the AOSP test certificate under its file name, and the signer names the release certificate after
                # the release key's path.
                ota_state = ota_certificates or ("release" if signed else "build")
                state_member = "keys/releasekey.x509.pem" if signed else "testkey.x509.pem"
                for ota_name in ("SYSTEM/etc/security/otacerts.zip",
                                 "RECOVERY/RAMDISK/system/etc/security/otacerts.zip"):
                    if ota_state == "missing-recovery" and ota_name.startswith("RECOVERY"):
                        continue
                    members = {
                        "build": ["testkey.x509.pem"],
                        "release": ["keys/releasekey.x509.pem"],
                        "test-key-path": ["build/make/target/product/security/testkey.x509.pem"],
                        "extra": [state_member, "build/make/target/product/security/testkey.x509.pem"],
                        "missing-recovery": [state_member],
                    }[ota_state]
                    certificates = io.BytesIO()
                    with zipfile.ZipFile(certificates, "w") as bundle:
                        for member in members:
                            bundle.writestr(member, "certificate\n")
                    archive.writestr(ota_name, certificates.getvalue())
                # The Wi-Fi HAL and the supplicant (with hostapd), packaged the same way.
                def hal_apex(apex, binary, name, fqname, service, variant):
                    content = b"\x00" * 8
                    files = ("etc/init.rc",)
                    if variant != "no-binary":
                        files += (f"bin/hw/{binary.decode()}",)
                    if variant != "no-fragment":
                        content += (
                            b'<manifest version="1.0" type="device"><hal format="aidl">'
                            b"<name>" + name + b"</name><fqname>" + fqname
                            + b"</fqname></hal></manifest>"
                        )
                    if variant != "no-service":
                        content += (
                            b"service " + service + b" /apex/" + apex + b"/bin/hw/" + binary
                            + b" \\\n    -O/data/vendor/wifi/wpa/sockets\n"
                        )
                    if variant != "missing-apex":
                        archive.writestr(
                            f"VENDOR/apex/{apex.decode()}.apex",
                            synthetic_apex("erofs", content, files=files),
                        )

                hal_apex(b"com.android.hardware.wifi", b"android.hardware.wifi-service",
                         b"android.hardware.wifi", b"IWifi/default", b"vendor.wifi_hal_legacy",
                         wifi_hal_variant)
                hal_apex(b"com.google.cf.wpa_supplicant", b"wpa_supplicant",
                         b"android.hardware.wifi.supplicant", b"ISupplicant/default",
                         b"wpa_supplicant", supplicant_variant)
                # The default CAS HAL, packaged the same way, and the ClearKey CAS plugin it loads
                # from /vendor/lib*/mediacas for both ISAs.
                cas_binary = b"android.hardware.cas-service.example"
                cas_content = b"\x00" * 8
                cas_files = ("etc/init.rc",)
                if cas_hal_variant != "no-binary":
                    cas_files += (f"bin/hw/{cas_binary.decode()}",)
                if cas_hal_variant != "no-fragment":
                    cas_content += (
                        b'<manifest version="1.0" type="device"><hal format="aidl">'
                        b"<name>android.hardware.cas</name>"
                        b"<fqname>IMediaCasService/default</fqname></hal></manifest>"
                    )
                if cas_hal_variant != "no-service":
                    cas_content += (
                        b"service vendor.cas-default "
                        b"/apex/com.android.hardware.cas/bin/hw/" + cas_binary + b"\n"
                    )
                if cas_hal_variant != "missing-apex":
                    archive.writestr(
                        "VENDOR/apex/com.android.hardware.cas.apex",
                        synthetic_apex("erofs", cas_content, files=cas_files),
                    )
                if cas_hal_variant != "no-plugin64":
                    archive.writestr(
                        "VENDOR/lib64/mediacas/libclearkeycasplugin.so",
                        synthetic_aarch64_elf(alignment),
                    )
                if cas_hal_variant != "no-plugin32":
                    archive.writestr(
                        "VENDOR/lib/mediacas/libclearkeycasplugin.so", synthetic_arm_elf()
                    )
                if system_other:
                    archive.writestr(
                        "SYSTEM_OTHER/app/Fixture/Fixture.apk", b"apk"
                    )
                if removed_payload:
                    archive.writestr(
                        "SYSTEM/apex/com.android.hardware.rebootescrow.apex",
                        synthetic_apex(),
                    )
            if entries:
                adjust_archive(path, entries)
            if corrupt_member:
                corrupt_archive_member(path, corrupt_member)

        good = root / "good.zip"
        write_fixture(good)
        run_tool(verifier, "--target-files", good, *kernel_options, check=True)
        # A recent aapt2 writes a resource that holds one value as a compact entry.
        compact_good = root / "compact-good.zip"
        write_fixture(compact_good, flip_variant="compact", camera_variant="compact",
                      wifi_overlay_variant="compact")
        run_tool(verifier, "--target-files", compact_good, *kernel_options, check=True)
        # Signed target files are checked with --signed: the release certificate alone is trusted,
        # and only a user build is signed. The unsigned fixtures above hold the test certificate's
        # member, which each variant's unsigned build writes.
        signed_good = root / "signed-good.zip"
        write_fixture(signed_good, signed=True)
        run_tool(verifier, "--target-files", signed_good, "--signed", *kernel_options, check=True)
        # A configuration that names a navigation mode overlay without enabling it enables nothing.
        disabled_configuration = root / "navigation-configuration-disabled.zip"
        write_fixture(disabled_configuration, navigation_variant="config-disables-gestural")
        run_tool(verifier, "--target-files", disabled_configuration, *kernel_options, check=True)
        # The framework's product list decides which application directory holds the easter egg.
        egg_in_product = root / "easter-egg-in-product.zip"
        write_fixture(egg_in_product, easter_egg_variant="product-partition")
        run_tool(verifier, "--target-files", egg_in_product, *kernel_options, check=True)
        # The overlay's string pools may be UTF-16, as the build packs them.
        utf16_layouts = root / "launcher-layouts-utf16.zip"
        write_fixture(utf16_layouts, launcher_variant="utf16-layout")
        run_tool(verifier, "--target-files", utf16_layouts, *kernel_options, check=True)
        for variant in BUILD_VARIANTS:
            signed_fixture = root / f"{variant}-signed-check.zip"
            write_fixture(signed_fixture, build_variant=variant, signed=True)
            result = run_tool(verifier, "--target-files", signed_fixture, "--build-variant", variant,
                              "--signed", *kernel_options)
            if (result.returncode == 0) != (variant == "user") or (
                variant != "user" and "is not signed with the release keys" not in result.stdout
            ):
                fail(f"a {variant} build verified as signed wrongly: {result.returncode} {result.stdout}")
            result = run_tool(verifier, "--target-files", signed_fixture, "--build-variant", variant,
                              *kernel_options)
            if result.returncode == 0 or "not an unsigned build" not in result.stdout:
                fail(f"a signed {variant} archive verified as an unsigned build: {result.returncode} {result.stdout}")
        # Every supported variant verifies as itself and is rejected as any
        # other: the build-type properties follow the requested variant. These
        # run as processes, like the good archive: they state the command line.
        for variant in BUILD_VARIANTS:
            for requested in BUILD_VARIANTS:
                fixture = root / f"{variant}-as-{requested}.zip"
                write_fixture(fixture, build_variant=variant)
                result = run_tool(
                    verifier, "--target-files", fixture,
                    "--build-variant", requested, *kernel_options,
                )
                if (result.returncode == 0) != (variant == requested):
                    fail(
                        f"a {variant} build verified as {requested}: "
                        f"{result.returncode} {result.stdout}"
                    )

        # The root shell of a debuggable build is the platform's: it may be called through binder from every domain,
        # the holder's and the waker's among them, with call and transfer, and with neither less nor more.
        for sms_variant, reason in (
            ("su-binder-widened",
             r"kernarium_sms_listener holds \['impersonate'\] on su of the binder class in effect"),
            ("su-binder-removed",
             r"kernarium_sms_listener holds \[\] on su of the binder class in effect in a userdebug build"),
            ("su-binder-call-only",
             r"kernarium_sms_listener holds \['call'\] on su of the binder class in effect in a userdebug build"),
            ("su-binder-transfer-only",
             r"kernarium_sms_listener holds \['transfer'\] on su of the binder class in effect in a userdebug build"),
        ):
            su_fixture = root / f"userdebug-{sms_variant}.zip"
            write_fixture(su_fixture, build_variant="userdebug", sms_policy_variant=sms_variant)
            result = run_tool(verifier, "--target-files", su_fixture, "--build-variant", "userdebug", *kernel_options)
            if result.returncode == 0 or not re.search(reason, result.stdout):
                fail(f"a userdebug build with the {sms_variant} policy verified or failed for another reason: "
                     f"{result.returncode} {result.stdout}")

        # Only a user build refuses a debuggable application: the diagnostics
        # variant is debuggable as a whole.
        for variant in BUILD_VARIANTS:
            fixture = root / f"{variant}-with-a-debuggable-application.zip"
            write_fixture(
                fixture, build_variant=variant,
                entries={"PRODUCT/app/Fixture/Fixture.apk": synthetic_apk(debuggable=True)},
            )
            result = run_tool(
                verifier, "--target-files", fixture,
                "--build-variant", variant, *kernel_options,
            )
            if (result.returncode == 0) != (variant != "user"):
                fail(
                    f"a {variant} build with a debuggable application verified wrongly: "
                    f"{result.returncode} {result.stdout}"
                )

        # Every variant refuses an application that requests the trade-in permission, and only a
        # request: an application that declares it (framework-res.apk declares every platform
        # permission) is not an application that can start the mode, and the shell package holds
        # the permission for what a person types over adb.
        trade_in_permission = "android.permission.ENTER_TRADE_IN_MODE"
        for variant in BUILD_VARIANTS:
            for slug, description, entry, apk, accepted in (
                ("requests", "an application that requests the permission",
                 "SYSTEM/priv-app/Fixture/Fixture.apk", synthetic_apk(permissions=(trade_in_permission,)), False),
                ("declares", "an application that declares the permission",
                 "SYSTEM/priv-app/Fixture/Fixture.apk", synthetic_apk(declared=(trade_in_permission,)), True),
                ("shell", "the shell package requesting the permission",
                 "SYSTEM/priv-app/Shell/Shell.apk", synthetic_apk(permissions=(trade_in_permission,)), True),
            ):
                fixture = root / f"{variant}-with-trade-in-permission-{slug}.zip"
                write_fixture(fixture, build_variant=variant, entries={entry: apk})
                result = run_tool(
                    verifier, "--target-files", fixture,
                    "--build-variant", variant, *kernel_options,
                )
                if (result.returncode == 0) != accepted:
                    fail(
                        f"a {variant} build with {description} verified wrongly: "
                        f"{result.returncode} {result.stdout}"
                    )

        # TalkBack keeps the permissions of its other features, Bluetooth and location among them,
        # and the guard of its unexported receivers, and an application that declares the network
        # permission does not request it: every variant accepts both. None of the kept
        # permissions is a network permission.
        for variant in BUILD_VARIANTS:
            fixture = root / f"{variant}-with-talkback-permissions-it-keeps.zip"
            write_fixture(
                fixture, build_variant=variant,
                entries={"PRODUCT/app/TalkBack/TalkBack.apk": synthetic_apk(
                    permissions=tuple(f"android.permission.{name}" for name in (
                        "USE_FINGERPRINT", "READ_PHONE_STATE", "VIBRATE", "WAKE_LOCK",
                        "MODIFY_AUDIO_SETTINGS", "RECORD_AUDIO", "QUERY_ALL_PACKAGES",
                        "RECEIVE_BOOT_COMPLETED", "FOREGROUND_SERVICE", "POST_NOTIFICATIONS",
                        "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION", "BLUETOOTH",
                        "BLUETOOTH_ADMIN", "BLUETOOTH_CONNECT", "BLUETOOTH_SCAN"))
                    + (TALKBACK_GUARD_PERMISSION,),
                    sdk23=("android.permission.WAKE_LOCK",),
                    declared=("android.permission.INTERNET",))},
            )
            result = run_tool(
                verifier, "--target-files", fixture,
                "--build-variant", variant, *kernel_options,
            )
            if result.returncode != 0:
                fail(
                    f"a {variant} build with TalkBack requesting only its other permissions "
                    f"verified wrongly: {result.returncode} {result.stdout}"
                )

        # A rejection fixture is built and verified once however many tables
        # state it: the cases whose options are the same share one run, which
        # must satisfy every assertion of every case (the status and the
        # verifier's own message of the first table, the named cause of the
        # second).
        rejections: dict[str, dict[str, object]] = {}

        def canonical(value: object) -> str:
            if isinstance(value, dict):
                return "{" + ",".join(
                    f"{key!r}:{canonical(item)}" for key, item in sorted(value.items())
                ) + "}"
            return repr(value)

        def reject(label: str, options: dict[str, object], reason: str | None = None) -> None:
            case = rejections.setdefault(
                canonical(options),
                {"options": options, "labels": [], "reasons": [], "message": False},
            )
            case["labels"].append(label)
            if reason is None:
                case["message"] = True
            else:
                case["reasons"].append(reason)

        for label, options in (
            ("module", {"module": True}),
            ("module-loader", {"module_loader": True}),
            ("dlkm-image", {"dlkm_image": True}),
            ("dynamic-image", {"dynamic_image": True}),
            ("ota-tool", {"ota_tool": True}),
            ("virtual-ab", {"virtual_ab": True}),
            ("no-32-bit-abi", {"abilist32": ""}),
            ("no-32-bit-vendor-abi", {"vendor_abilist32": ""}),
            ("single-zygote", {"zygote": "zygote64"}),
            ("no-64-bit-compiler", {"dex2oat64": False}),
            ("missing-32-bit-runtime",
             {"entries": {"SYSTEM/bin/app_process32": None}}),
            ("missing-sgdisk",
             {"entries": {"SYSTEM/bin/sgdisk": None}}),
            ("missing-32-bit-boot-image",
             {"entries": {"SYSTEM/framework/arm/boot-framework.oat": None}}),
            ("missing-32-bit-gallium-driver",
             {"entries": {"VENDOR/lib/venus_gallium_dri.so": None}}),
            ("missing-32-bit-egl",
             {"entries": {"VENDOR/lib/egl/libGLESv2_kernarium.so": None}}),
            ("missing-32-bit-vulkan-hal",
             {"entries": {"VENDOR/lib/hw/vulkan.virtio.so": None}}),
            ("missing-32-bit-gralloc",
             {"entries": {"VENDOR/lib/hw/gralloc.minigbm.so": None}}),
            ("missing-32-bit-mapper",
             {"entries": {"VENDOR/lib/hw/mapper.minigbm.so": None}}),
            ("32-bit-gallium-dependency", {"missing_arm_gallium_dependency": True}),
            ("32-bit-egl-is-aarch64",
             {"entries": {"VENDOR/lib/egl/libEGL_kernarium.so": synthetic_aarch64_elf()}}),
            ("64-bit-egl-is-arm",
             {"entries": {"VENDOR/lib64/egl/libEGL_kernarium.so": synthetic_arm_elf()}}),
            ("missing-webview-32-bit-odex",
             {"entries": {"PRODUCT/app/webview/oat/arm/webview.odex": None}}),
            ("missing-webview-32-bit-vdex",
             {"entries": {"PRODUCT/app/webview/oat/arm/webview.vdex": None}}),
            ("32-bit-odex-without-arm64",
             {"entries": {"PRODUCT/app/Orphan/oat/arm/Orphan.odex": b"odex",
                          "PRODUCT/app/Orphan/oat/arm/Orphan.vdex": b"vdex"}}),
            ("unknown-isa",
             {"entries": {"PRODUCT/app/LatinIME/oat/x86_64/LatinIME.odex": b"odex"}}),
            ("32-bit-elf-in-lib64",
             {"entries": {"VENDOR/lib64/libfixture.so": synthetic_arm_elf()}}),
            ("64-bit-elf-in-lib",
             {"entries": {"SYSTEM/lib/libfixture.so": synthetic_aarch64_elf()}}),
            ("32-bit-daemon",
             {"entries": {"SYSTEM/bin/mediaserver": synthetic_arm_elf()}}),
            ("32-bit-vendor-daemon",
             {"entries": {"VENDOR/bin/hw/android.hardware.fixture-service": synthetic_arm_elf()}}),
            ("missing-motion-backend", {"entries": {
                "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so": None}}),
            ("foreign-motion-backend", {"entries": {
                "VENDOR/etc/sensors/hals.conf": b"/vendor/lib64/hw/other.so\n"}}),
            ("missing-motion-instance", {"entries": {
                "VENDOR/etc/vintf/manifest/android.hardware.sensors-multihal.xml": b'<manifest/>'}}),
            ("wrong-motion-service", {"entries": {
                "VENDOR/etc/init/android.hardware.sensors-service-multihal.rc": b'service sensor /vendor/bin/other\n'}}),
            ("32-bit-alignment", {"arm_alignment": 2048}),
            ("32-bit-hard-float", {"arm_flags": 0x05000400}),
            ("alignment", {"alignment": 4096}),
            ("untracked-variant", {"build_variant": other_variant}),
            ("wrong-fstab", {"wrong_fstab": True}),
            ("wrong-filesystem", {"wrong_filesystem": True}),
            ("wrong-erofs-compressor",
             {"misc_info_edits": {"erofs_default_compressor": "lz4hc,9"}}),
            ("wrong-erofs-level",
             {"misc_info_edits": {"erofs_default_compressor": "zstd,19"}}),
            ("wrong-erofs-cluster",
             {"misc_info_edits": {"erofs_pcluster_size": "4096"}}),
            ("missing-erofs-compressor",
             {"misc_info_edits": {"erofs_default_compressor": None}}),
            ("erofs-dedupe", {"misc_info_edits": {"erofs_enable_dedupe": "true"}}),
            ("erofs-shared-blocks",
             {"misc_info_edits": {"erofs_share_dup_blocks": "true"}}),
            ("erofs-legacy-compression",
             {"misc_info_edits": {"erofs_use_legacy_compression": "true"}}),
            ("erofs-block-size", {"misc_info_edits": {"erofs_blocksize": "16384"}}),
            ("erofs-system-block-size",
             {"misc_info_edits": {"system_erofs_blocksize": "16384"}}),
            ("erofs-vendor-compressor",
             {"misc_info_edits": {"vendor_erofs_compressor": "lz4hc,9"}}),
            ("erofs-system-ext-cluster",
             {"misc_info_edits": {"system_ext_erofs_pcluster_size": "65536"}}),
            ("missing-erofs-hints",
             {"entries": {"META/erofs_default_compress_hints.txt": None}}),
            ("wrong-erofs-hints",
             {"entries": {"META/erofs_default_compress_hints.txt": b"0 .*\\.img$\n"}}),
            ("ext4-system-image",
             {"entries": {"IMAGES/system.img": bytes(4096)}}),
            ("lz4-vendor-image",
             {"entries": {"IMAGES/vendor.img": synthetic_erofs_image(algorithms=1 << 0)}}),
            ("lzma-product-image",
             {"entries": {"IMAGES/product.img": synthetic_erofs_image(
                 algorithms=1 << 1, sparse=True)}}),
            ("two-algorithm-odm-image",
             {"entries": {"IMAGES/odm.img": synthetic_erofs_image(
                 algorithms=(1 << 0) | (1 << 3))}}),
            ("small-cluster-system-ext-image",
             {"entries": {"IMAGES/system_ext.img": synthetic_erofs_image(features=0x1)}}),
            ("tail-packing-system-image",
             {"entries": {"IMAGES/system.img": synthetic_erofs_image(features=0x3 | 0x10)}}),
            ("fragments-vendor-image",
             {"entries": {"IMAGES/vendor.img": synthetic_erofs_image(features=0x3 | 0x20)}}),
            ("16k-block-system-image",
             {"entries": {"IMAGES/system.img": synthetic_erofs_image(block_bits=14)}}),
            ("wrong-userdata-inodes", {"wrong_userdata_inodes": True}),
            ("missing-userdata-quota", {"missing_userdata_quota": True}),
            ("wrong-apex-filesystem", {"wrong_apex_filesystem": True}),
            ("apex-lz4-payload",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 algorithms=1 << 0)}}),
            ("apex-small-cluster-payload",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 features=0x1)}}),
            ("apex-extended-payload",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 features=0x3 | 0x20)}}),
            ("apex-16k-block-payload",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 block_bits=14)}}),
            ("apex-unknown-payload",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex("unknown")}}),
            ("shim-lookalike-ext4-payload",
             {"entries": {"SYSTEM/apex/com.android.apex.cts.shim2.apex": synthetic_apex("ext4")}}),
            ("stock-fstab", {"stock_fstab": True}),
            ("seriallogging", {"seriallogging": True}),
            ("boot-control", {"boot_control": True}),
            ("context-hub", {"context_hub": True}),
            ("unsupported-hal", {"unsupported_hal": True}),
            ("unsupported-feature", {"unsupported_feature": True}),
            *((f"missing-{feature}", {"missing_multitouch_feature": feature})
              for feature in (
                  "android.hardware.touchscreen.multitouch",
                  "android.hardware.touchscreen.multitouch.distinct",
                  "android.hardware.touchscreen.multitouch.jazzhand",
              )),
            ("missing-microphone-feature", {"missing_microphone_feature": True}),
            ("missing-companion-feature", {"missing_companion_feature": True}),
            ("missing-midi-feature", {"missing_midi_feature": True}),
            ("missing-mic-policy", {"missing_mic_policy": True}),
            ("extra-input-device", {"extra_input_device": True}),
            ("kernel-config", {"wrong_kernel_config": True}),
            ("kernel-no-macvlan", {"kernel_config_variant": "missing:CONFIG_MACVLAN"}),
            ("kernel-no-cfg80211", {"kernel_config_variant": "unset:CONFIG_CFG80211"}),
            ("kernel-no-virt-wifi", {"kernel_config_variant": "missing:CONFIG_VIRT_WIFI"}),
            ("kernel-macvlan-module", {"kernel_config_variant": "module:CONFIG_MACVLAN"}),
            ("kernel-cfg80211-module", {"kernel_config_variant": "module:CONFIG_CFG80211"}),
            ("kernel-virt-wifi-module", {"kernel_config_variant": "module:CONFIG_VIRT_WIFI"}),
            ("kernel-no-erofs", {"kernel_config_variant": "missing:CONFIG_EROFS_FS"}),
            ("kernel-no-erofs-zstd",
             {"kernel_config_variant": "unset:CONFIG_EROFS_FS_ZIP_ZSTD"}),
            ("kernel-erofs-zip-module", {"kernel_config_variant": "module:CONFIG_EROFS_FS_ZIP"}),
            ("kernel-release", {"wrong_kernel_release": True}),
            ("memfd-policy", {"missing_memfd_policy": True}),
            ("spl-policy", {"missing_spl_policy": True}),
            ("blocker-missing-service", {"blocker_variant": "missing-service"}),
            ("blocker-class-started", {"blocker_variant": "class-started"}),
            ("blocker-started-at-boot", {"blocker_variant": "started-at-boot"}),
            ("blocker-no-stop", {"blocker_variant": "no-stop"}),
            ("blocker-binary", {"missing_blocker_binary": True}),
            ("net-setup-missing-service", {"net_setup_variant": "missing-service"}),
            ("net-setup-not-oneshot", {"net_setup_variant": "not-oneshot"}),
            ("net-setup-not-disabled", {"net_setup_variant": "not-disabled"}),
            ("net-setup-no-exec-start", {"net_setup_variant": "no-exec-start"}),
            ("net-setup-sysctl-after-start", {"net_setup_variant": "sysctl-after-start"}),
            ("net-setup-no-announce", {"net_setup_variant": "no-announce"}),
            ("net-setup-started-at-boot", {"net_setup_variant": "started-at-boot"}),
            ("net-setup-binary", {"missing_net_setup_binary": True}),
            ("net-library", {"missing_net_library": True}),
            ("net-policy-label", {"net_policy_variant": "missing-label"}),
            ("net-policy-transition", {"net_policy_variant": "missing-transition"}),
            ("net-policy-sysctl-write", {"net_policy_variant": "no-sysctl-write"}),
            ("wifi-network-missing-rc", {"wifi_network_variant": "missing-rc"}),
            ("wifi-network-missing-service", {"wifi_network_variant": "missing-service"}),
            ("wifi-network-not-oneshot", {"wifi_network_variant": "not-oneshot"}),
            ("wifi-network-not-disabled", {"wifi_network_variant": "not-disabled"}),
            ("wifi-network-no-start", {"wifi_network_variant": "no-start"}),
            ("wifi-network-started-at-boot", {"wifi_network_variant": "started-at-boot"}),
            ("wifi-network-no-add", {"wifi_network_variant": "no-add"}),
            ("wifi-network-no-list", {"wifi_network_variant": "no-list"}),
            ("wifi-network-no-status", {"wifi_network_variant": "no-status"}),
            ("wifi-network-no-marker", {"wifi_network_variant": "no-marker"}),
            ("wifi-network-no-joined-marker", {"wifi_network_variant": "no-joined-marker"}),
            ("wifi-network-enables-wifi", {"wifi_network_variant": "enables-wifi"}),
            ("wifi-network-connects", {"wifi_network_variant": "connects"}),
            ("wifi-network-settings-put", {"wifi_network_variant": "settings-put"}),
            ("wifi-network-missing-label", {"wifi_network_variant": "missing-label"}),
            ("wifi-network-missing-transition", {"wifi_network_variant": "missing-transition"}),
            ("wifi-network-marker-not-system", {"wifi_network_variant": "marker-not-system"}),
            ("wifi-network-missing-marker-rule", {"wifi_network_variant": "missing-marker-rule"}),
            ("wifi-network-missing-binder-transfer", {"wifi_network_variant": "missing-binder-transfer"}),
            ("wifi-network-missing-binder-call", {"wifi_network_variant": "missing-binder-call"}),
            ("wifi-network-missing-marker-context", {"wifi_network_variant": "missing-marker-context"}),
            ("wifi-network-marker-type-bool", {"wifi_network_variant": "marker-type-bool"}),
            ("wifi-network-marker-type-enum-one", {"wifi_network_variant": "marker-type-enum-one"}),
            ("wifi-network-script", {"missing_wifi_network_script": True}),
            ("wificond-present", {"entries": {"SYSTEM/bin/wificond": synthetic_aarch64_elf()}}),
            ("wifi-dirs-missing", {"wifi_dirs_variant": "missing"}),
            ("wifi-dirs-wrong-owner", {"wifi_dirs_variant": "wrong-owner"}),
            ("missing-wifi-feature", {"missing_wifi_feature": True}),
            ("missing-wifi-overlay", {"entries": {"VENDOR/overlay/KernariumWifiOverlay.apk": None}}),
            ("wifi-overlay-missing-permanent", {"wifi_overlay_variant": "missing-permanent"}),
            ("wifi-overlay-missing-temporary", {"wifi_overlay_variant": "missing-temporary"}),
            ("wifi-overlay-missing-both", {"wifi_overlay_variant": "missing-both"}),
            ("wifi-overlay-permanent-one", {"wifi_overlay_variant": "permanent-one"}),
            ("wifi-overlay-temporary-zero", {"wifi_overlay_variant": "temporary-zero"}),
            ("wifi-overlay-temporary-large-not-largest",
             {"wifi_overlay_variant": "temporary-large-not-largest"}),
            ("wifi-overlay-permanent-negative", {"wifi_overlay_variant": "permanent-negative"}),
            ("wifi-overlay-qualified-low", {"wifi_overlay_variant": "qualified-low"}),
            ("wifi-overlay-no-table", {"wifi_overlay_variant": "no-table"}),
            ("ethernet-feature", {"wifi_feature_file": "android.hardware.ethernet"}),
            ("wifi-aware-feature", {"wifi_feature_file": "android.hardware.wifi.aware"}),
            ("wifi-rtt-feature", {"wifi_feature_file": "android.hardware.wifi.rtt"}),
            ("wifi-direct-feature", {"wifi_feature_file": "android.hardware.wifi.direct"}),
            ("wifi-passpoint-feature", {"wifi_feature_file": "android.hardware.wifi.passpoint"}),
            ("missing-wifi-hal-apex", {"wifi_hal_variant": "missing-apex"}),
            ("wifi-hal-without-binary", {"wifi_hal_variant": "no-binary"}),
            ("wifi-hal-without-fragment", {"wifi_hal_variant": "no-fragment"}),
            ("wifi-hal-without-service", {"wifi_hal_variant": "no-service"}),
            ("missing-supplicant-apex", {"supplicant_variant": "missing-apex"}),
            ("supplicant-without-binary", {"supplicant_variant": "no-binary"}),
            ("supplicant-without-fragment", {"supplicant_variant": "no-fragment"}),
            ("supplicant-without-service", {"supplicant_variant": "no-service"}),
            ("adb-prop-context", {"missing_adb_prop_context": True}),
            ("adb-prop-policy", {"missing_adb_prop_policy": True}),
            ("core-sets-vendor-prop", {"core_sets_vendor_prop": True}),
            ("audio-binder-policy", {"missing_audio_binder_policy": True}),
            ("wifi-debug-property-policy", {"missing_wifi_debug_policy": True}),
            ("camera-fence-policy", {"camera_fence_variant": "missing"}),
            ("camera-fence-source-other-type", {"camera_fence_variant": "source-other-type"}),
            ("camera-fence-target-other-type", {"camera_fence_variant": "target-other-type"}),
            ("camera-fence-source-short-suffix", {"camera_fence_variant": "source-short-suffix"}),
            ("camera-fence-target-long-suffix", {"camera_fence_variant": "target-long-suffix"}),
            ("clearkey-memfd-missing", {"clearkey_memfd_variant": "missing"}),
            ("clearkey-memfd-no-write", {"clearkey_memfd_variant": "no-write"}),
            ("clearkey-memfd-isolated-apps", {"clearkey_memfd_variant": "all-apps"}),
            ("clearkey-memfd-wrong-exclusion", {"clearkey_memfd_variant": "isolated-only"}),
            ("oversize-platform-policy", {"oversize_platform_policy": True}),
            ("media-codecs-missing", {"media_codecs_variant": "missing"}),
            ("media-codecs-malformed", {"media_codecs_variant": "malformed"}),
            ("media-codecs-no-hevc", {"media_codecs_variant": "no-hevc"}),
            ("media-codecs-low-hevc-rate", {"media_codecs_variant": "low-hevc-rate"}),
            ("media-codecs-small-vp9-size", {"media_codecs_variant": "small-vp9-size"}),
            ("media-codecs-small-vp9-blocks", {"media_codecs_variant": "small-vp9-blocks"}),
            ("system-ext-core-sets-vendor-prop", {"system_ext_core_sets_vendor_prop": True}),
            ("adb-prop-not-system", {"adb_prop_not_system": True}),
            ("adb-prop-unreadable-by-vendor-init", {"adb_prop_unreadable_by_vendor_init": True}),
            ("lineage-mediaprovider-trusted", {"lineage_policy": "mediaprovider-trusted"}),
            ("lineage-mediaprovider-privapp", {"lineage_policy": "mediaprovider-privapp"}),
            ("lineage-fsck-sys-admin", {"lineage_policy": "fsck-sys-admin"}),
            ("lineage-vold-relabelto", {"lineage_policy": "vold-relabelto"}),
            ("lineage-permissive-backuptool", {"lineage_policy": "permissive-backuptool"}),
            ("missing-mediaserver-audit", {"missing_mediaserver_audit": True}),
            ("missing-shell-density-audit", {"missing_shell_density_audit": True}),
            ("missing-virtio-console", {"missing_virtio_console": True}),
            ("unsupported-hvc", {"unsupported_hvc": True}),
            ("missing-mesa-gles", {"missing_mesa_gles": True}),
            ("mesa-gles-not-elf", {"bad_mesa_gles_elf": True}),
            ("gallium-dependency", {"missing_gallium_dependency": True}),
            ("retired-egl-library", {"retired_egl_library": True}),
            ("retired-dri-driver", {"retired_dri_driver": True}),
            ("vendor-egl-property", {"vendor_hal_property": "egl"}),
            ("vendor-vulkan-property", {"vendor_hal_property": "vulkan"}),
            ("vendor-gralloc-property", {"vendor_hal_property": "gralloc"}),
            ("system-other", {"system_other": True}),
            ("missing-sdk-apex", {"missing_sdk_apex": True}),
            ("removed-payload", {"removed_payload": True}),
            ("missing-preopt", {"missing_required_preopt": True}),
            ("half-preopt-pair", {"half_preopt_pair": True}),
            ("audio-defaults", {"wrong_audio_defaults": True}),
            ("product-model", {"wrong_product_model": True}),
            ("old-product-identity", {"old_product_identity": True}),
            ("wrong-vendor-identity", {"wrong_vendor_identity": True}),
            ("vendor-build-identity-of-an-eng-build", {"vendor_build_identity": "eng"}),
            ("vendor-without-a-build-identity", {"vendor_build_identity": "missing"}),
            ("missing-public-fingerprint", {"fingerprint_variant": "missing"}),
            ("generic-system-fingerprint", {"fingerprint_variant": "generic-system"}),
            ("wrong-type-fingerprint", {"fingerprint_variant": "wrong-type"}),
            ("user-permissive-su", {"user_permissive_su": True}),
            ("usb-debugging-on-by-default", {"usb_debugging_default": True}),
            ("adb-secure-off-in-product", {"adb_secure_variant": "product"}),
            ("adb-secure-false-in-product", {"adb_secure_variant": "product-false"}),
            ("adb-secure-off-in-system", {"adb_secure_variant": "system"}),
            ("user-carries-su", {"extra_entry": "SYSTEM/xbin/su"}),
            ("user-carries-remount", {"extra_entry": "SYSTEM/bin/remount"}),
            ("user-carries-procmem", {"extra_entry": "SYSTEM/bin/procmem"}),
            ("missing-lockscreen-default", {"missing_lockscreen_default": True}),
            ("missing-use-fiemap", {"use_fiemap_variant": "missing"}),
            ("use-fiemap-true", {"use_fiemap_variant": "true"}),
            ("missing-lmkd-binary", {"lmkd_variant": "missing-binary"}),
            ("lmkd-without-the-gpu-reclaim", {"lmkd_variant": "old-binary"}),
            ("lmkd-without-the-setting-name", {"lmkd_variant": "no-property-name"}),
            ("lmkd-without-the-report-path", {"lmkd_variant": "no-report-path"}),
            ("lmkd-without-the-kill-reason", {"lmkd_variant": "no-kill-reason"}),
            ("missing-lmkd-gpu-reclaim-property", {"lmkd_variant": "property-missing"}),
            ("lmkd-gpu-reclaim-property-off", {"lmkd_variant": "property-off"}),
            ("unlabelled-lmkd-gpu-reclaim-property", {"lmkd_variant": "no-property-label"}),
            ("unreadable-lmkd-gpu-reclaim-property", {"lmkd_variant": "property-unreadable"}),
            ("gpu-report-type-not-a-sysfs-type", {"lmkd_variant": "type-not-sysfs"}),
            ("gpu-report-not-labelled", {"lmkd_variant": "no-genfs"}),
            ("gpu-report-labelled-as-the-device", {"lmkd_variant": "genfs-wrong-type"}),
            ("gpu-report-labelled-at-another-path", {"lmkd_variant": "genfs-other-path"}),
            ("lmkd-cannot-use-the-gpu-report", {"lmkd_variant": "no-report-rule"}),
            ("lmkd-cannot-write-the-gpu-report", {"lmkd_variant": "no-report-write"}),
            ("lmkd-cannot-search-the-gpu-device", {"lmkd_variant": "no-dir-search"}),
            ("userdata-grow-missing", {"userdata_grow_variant": "missing"}),
            ("userdata-grow-offline-only", {"userdata_grow_variant": "offline-only"}),
            ("userdata-grow-offline-also", {"userdata_grow_variant": "offline-also"}),
            ("userdata-grow-twice", {"userdata_grow_variant": "twice"}),
            ("userdata-grow-forced", {"userdata_grow_variant": "forced"}),
            ("userdata-grow-no-group", {"userdata_grow_variant": "no-group"}),
            ("userdata-grow-at-boot", {"userdata_grow_variant": "at-boot"}),
            ("userdata-grow-policy-no-capability", {"userdata_grow_policy_variant": "no-capability"}),
            ("userdata-grow-policy-no-dir-open", {"userdata_grow_policy_variant": "no-dir-open"}),
            ("userdata-grow-policy-no-ioctl", {"userdata_grow_policy_variant": "no-ioctl"}),
            ("userdata-grow-policy-more-ioctls", {"userdata_grow_policy_variant": "more-ioctls"}),
            ("missing-discrete-ops-overlay", {"discrete_ops_variant": "missing-overlay"}),
            ("missing-framework-overlay", {"wallpaper_variant": "missing-overlay"}),
            ("missing-default-wallpaper-variant", {"wallpaper_variant": "missing-variant"}),
            ("lineage-size-default-wallpaper", {"wallpaper_variant": "lineage-size"}),
            ("non-square-default-wallpaper", {"wallpaper_variant": "not-square"}),
            ("default-wallpaper-not-png", {"wallpaper_variant": "not-png"}),
            ("unreliable-present-fence", {"vendor_property_variant": "present-fence-unreliable"}),
            ("missing-incremental-property", {"vendor_property_variant": "no-incremental"}),
            ("disabled-incremental-property", {"vendor_property_variant": "incremental-off"}),
            ("cuttlefish-board", {"board_variant": "cutf"}),
            ("missing-board", {"board_variant": "missing"}),
            ("missing-odm-soc-manufacturer", {"soc_variant": "missing-manufacturer"}),
            ("missing-odm-soc-model", {"soc_variant": "missing-model"}),
            ("cuttlefish-soc", {"soc_variant": "cuttlefish"}),
            ("joined-soc-model", {"soc_variant": "joined-model"}),
            ("lowercase-soc-model", {"soc_variant": "lowercase-model"}),
            ("unknown-soc-model", {"soc_variant": "unknown-model"}),
            ("vendor-soc-manufacturer", {"soc_variant": "vendor-manufacturer"}),
            ("vendor-soc-model", {"soc_variant": "vendor-model"}),
            ("vendor-soc-other", {"soc_variant": "vendor-other"}),
            ("vendor-soc-same-as-odm", {"soc_variant": "vendor-same"}),
            ("missing-cas-apex", {"cas_hal_variant": "missing-apex"}),
            ("cas-apex-without-binary", {"cas_hal_variant": "no-binary"}),
            ("cas-apex-without-fragment", {"cas_hal_variant": "no-fragment"}),
            ("cas-apex-without-service", {"cas_hal_variant": "no-service"}),
            ("missing-adbd-apex", {"adbd_variant": "missing-apex"}),
            ("adbd-apex-without-binary", {"adbd_variant": "no-binary"}),
            ("adbd-without-host-key", {"adbd_variant": "no-key"}),
            ("adbd-without-tcp-policy", {"adbd_variant": "no-tcp"}),
            ("adbd-without-debugging-setting", {"adbd_variant": "no-debugging-setting"}),
            ("adbd-without-key-rule", {"adbd_variant": "no-key-rule"}),
            ("adb-root-service-without-the-debuggable-rule", {"adbd_variant": "no-root-guard"}),
            ("missing-adb-root-service", {"adbd_variant": "no-root-service"}),
            ("services-jar-without-the-adb-topics", {"services_jar_variant": "no-marker"}),
            ("services-jar-without-the-display-topic", {"services_jar_variant": "no-display-marker"}),
            ("input-flinger-without-the-pointer-capture-led", {"input_flinger_variant": "no-marker"}),
            ("missing-input-flinger", {"input_flinger_variant": "missing"}),
            ("network-stack-without-the-wifi-topic", {"network_stack_variant": "no-marker"}),
            ("network-stack-without-the-wifi-check-report", {"network_stack_variant": "no-check-marker"}),
            ("settings-without-the-wifi-tracker-topic", {"wifi_tracker_variant": "settings-no-marker"}),
            ("system-ui-without-the-wifi-tracker-topic", {"wifi_tracker_variant": "systemui-no-marker"}),
            ("signed-with-the-test-key", {"signed": True, "ota_certificates": "build"}),
            ("signed-with-the-test-key-path", {"signed": True, "ota_certificates": "test-key-path"}),
            ("signed-with-an-extra-certificate", {"signed": True, "ota_certificates": "extra"}),
            ("signed-without-recovery-certificates", {"signed": True, "ota_certificates": "missing-recovery"}),
            ("unsigned-with-the-release-member", {"ota_certificates": "release"}),
            ("unsigned-with-an-extra-certificate", {"ota_certificates": "extra"}),
            ("unsigned-without-recovery-certificates", {"ota_certificates": "missing-recovery"}),
            ("missing-services-jar", {"services_jar_variant": "missing"}),
            ("missing-network-stack", {"network_stack_variant": "missing"}),
            ("adbd-cannot-read-launch-properties", {"adbd_launch_unreadable": True}),
            ("adb-key-with-the-bootloader-type", {"adb_key_variant": "bootloader-label"}),
            ("adb-key-adbd-cannot-read", {"adb_key_variant": "adbd-unreadable"}),
            ("adb-key-untrusted-app-reads", {"adb_key_variant": "untrusted-app-reads"}),
            ("adb-key-appdomain-reads", {"adb_key_variant": "appdomain-reads"}),
            ("adb-key-untrusted-app-34-reads", {"adb_key_variant": "untrusted-app-34-reads"}),
            ("adb-key-untrusted-app-29-reads", {"adb_key_variant": "untrusted-app-29-reads"}),
            ("adb-key-runas-app-reads", {"adb_key_variant": "runas-app-reads"}),
            ("adb-key-sdk-sandbox-reads", {"adb_key_variant": "sdk-sandbox-reads"}),
            ("adb-key-shell-reads", {"adb_key_variant": "shell-reads"}),
            ("adb-key-generated-attribute-reads", {"adb_key_variant": "generated-attribute-reads"}),
            ("missing-cas-plugin-64", {"cas_hal_variant": "no-plugin64"}),
            ("missing-cas-plugin-32", {"cas_hal_variant": "no-plugin32"}),
            ("lineage-discrete-ops-list-only", {"discrete_ops_variant": "lineage-list-only"}),
            ("short-discrete-ops-list", {"discrete_ops_variant": "short-device-list"}),
            ("missing-pinner-overlay", {"entries": {"VENDOR/overlay/KernariumPinnerOverlay.apk": None}}),
            ("pinner-overlay-target-sdk-29", {"pinner_overlay_variant": "target-sdk-29"}),
            ("pinner-overlay-target-sdk-current", {"pinner_overlay_variant": "target-sdk-current"}),
            ("pinner-overlay-no-target-sdk", {"pinner_overlay_variant": "no-target-sdk"}),
            ("pinner-overlay-named-target-sdk", {"pinner_overlay_variant": "named-target-sdk"}),
            ("missing-setup-wizard-overlay", {"wizard_branding_variant": "missing-overlay"}),
            ("setup-wizard-names-lineageos", {"wizard_branding_variant": "lineage-name"}),
            ("setup-wizard-names-lineage-in-utf16", {"wizard_branding_variant": "lineage-name-utf16"}),
            ("setup-wizard-without-recovery-strings", {"wizard_branding_variant": "no-recovery-strings"}),
            ("setup-wizard-without-andarium-features", {"wizard_branding_variant": "lineage-features"}),
            ("setup-wizard-without-logo", {"wizard_branding_variant": "no-logo"}),
            ("setup-wizard-with-another-logo", {"wizard_branding_variant": "stock-logo"}),
            ("setup-wizard-logo-lacking-a-path", {"wizard_branding_variant": "logo-lacks-a-path"}),
            ("setup-wizard-without-animation", {"wizard_branding_variant": "no-animation"}),
            ("setup-wizard-with-another-animation", {"wizard_branding_variant": "stock-animation"}),
            ("setup-wizard-without-its-script", {"wizard_branding_variant": "no-script"}),
            ("setup-wizard-without-its-user-script", {"wizard_branding_variant": "no-user-script"}),
            ("setup-wizard-with-another-script", {"wizard_branding_variant": "stock-script"}),
            ("setup-wizard-with-another-user-script", {"wizard_branding_variant": "stock-user-script"}),
            ("setup-wizard-forces-gestural-navigation", {"navigation_variant": "dex-forces-gestural"}),
            ("missing-gestural-navigation-overlay", {"navigation_variant": "no-gestural-overlay"}),
            ("missing-three-button-navigation-overlay", {"navigation_variant": "no-3button-overlay"}),
            ("overlay-configuration-enables-gestural", {"navigation_variant": "config-enables-gestural"}),
            ("overlay-configuration-enables-three-button",
             {"navigation_variant": "config-enables-3button-undisabled"}),
            ("overlay-configuration-unreadable", {"navigation_variant": "config-unreadable"}),
            ("vendor-overlay-theme-property", {"navigation_variant": "overlay-theme-property"}),
            ("missing-launcher-overlay", {"launcher_variant": "no-overlay"}),
            ("launcher-overlay-without-a-grid", {"launcher_variant": "no-6x5"}),
            ("launcher-overlay-with-a-stock-layout", {"launcher_variant": "stock-layout"}),
            ("launcher-layout-without-files", {"launcher_variant": "lacks-files"}),
            ("launcher-overlay-with-the-six-application-layout", {"launcher_variant": "six-application-layout"}),
            ("launcher-layout-names-an-application-the-image-lacks", {"launcher_variant": "no-documentsui"}),
            ("missing-easter-egg", {"easter_egg_variant": "missing"}),
            ("missing-talkback", {"accessibility_variant": "missing-talkback"}),
            ("missing-espeak-ng", {"accessibility_variant": "missing-espeak"}),
            ("talkback-in-the-system-partition", {"accessibility_variant": "talkback-in-system"}),
            ("easter-egg-in-a-directory-of-another-name", {"easter_egg_variant": "wrong-directory"}),
            ("ntp-server-left", {"network_time_variant": "ntp-server"}),
            ("ntp-server-left-in-a-qualified-definition",
             {"network_time_variant": "ntp-server-in-a-qualified-table"}),
            ("ntp-server-left-in-capitals", {"network_time_variant": "ntp-server-in-capitals"}),
            ("ntp-server-list-empty", {"network_time_variant": "ntp-empty"}),
            ("ntp-server-list-missing", {"network_time_variant": "ntp-missing"}),
            ("network-time-origin-left", {"network_time_variant": "origin-network"}),
            ("time-origin-list-empty", {"network_time_variant": "origin-empty"}),
            ("time-origin-list-without-external", {"network_time_variant": "origin-without-external"}),
            ("time-origin-list-missing", {"network_time_variant": "origin-missing"}),
            ("flip-to-screen-off-enabled", {"flip_variant": "enabled"}),
            ("flip-to-screen-off-enabled-in-a-qualified-definition",
             {"flip_variant": "enabled-in-a-qualified-table"}),
            ("flip-to-screen-off-missing", {"flip_variant": "missing"}),
            ("statistics-uploader-in-parts-manifest", {"statistics_variant": "parts-manifest"}),
            ("statistics-uploader-in-parts-dex", {"statistics_variant": "parts-dex"}),
            ("statistics-endpoint-in-parts-resources", {"statistics_variant": "parts-endpoint"}),
            ("statistics-part-in-parts-catalogue", {"statistics_variant": "parts-catalogue"}),
            ("statistics-setting-in-setup-wizard", {"statistics_variant": "wizard-setting"}),
            ("pinner-overlay-no-resource", {"pinner_overlay_variant": "no-resource"}),
            ("missing-reclaim-script", {"reclaim_variant": "missing-script"}),
            ("missing-reclaim-rc", {"reclaim_variant": "missing-rc"}),
            ("reclaim-missing-service", {"reclaim_variant": "missing-service"}),
            ("reclaim-not-oneshot", {"reclaim_variant": "not-oneshot"}),
            ("reclaim-not-disabled", {"reclaim_variant": "not-disabled"}),
            ("reclaim-started-at-boot", {"reclaim_variant": "started-at-boot"}),
            ("reclaim-no-start", {"reclaim_variant": "no-start"}),
            ("reclaim-no-kill", {"reclaim_variant": "no-kill"}),
            ("reclaim-kill-all", {"reclaim_variant": "kill-all"}),
            ("reclaim-force-stop", {"reclaim_variant": "force-stop"}),
            ("reclaim-kill-9", {"reclaim_variant": "kill-9"}),
            ("reclaim-pkill", {"reclaim_variant": "pkill"}),
            ("reclaim-kill-pid", {"reclaim_variant": "kill-pid"}),
            ("reclaim-am-command", {"reclaim_variant": "am-command"}),
            ("reclaim-second-kill", {"reclaim_variant": "second-kill"}),
            ("reclaim-second-list", {"reclaim_variant": "second-list"}),
            ("reclaim-extra-package", {"reclaim_variant": "extra-package"}),
            ("reclaim-missing-package", {"reclaim_variant": "missing-package"}),
            ("reclaim-missing-messaging", {"reclaim_variant": "missing-messaging"}),
            ("reclaim-missing-telephony-provider", {"reclaim_variant": "missing-telephony-provider"}),
            ("reclaim-missing-icons", {"reclaim_variant": "missing-icons"}),
            ("reclaim-telephony-package-as-process", {"reclaim_variant": "telephony-package-as-process"}),
            ("reclaim-missing-process", {"reclaim_variant": "missing-process"}),
            ("reclaim-no-settle", {"reclaim_variant": "no-settle"}),
            ("reclaim-no-lru", {"reclaim_variant": "no-lru"}),
            ("reclaim-no-list", {"reclaim_variant": "no-list"}),
            ("reclaim-missing-label", {"reclaim_variant": "missing-label"}),
            ("reclaim-missing-transition", {"reclaim_variant": "missing-transition"}),
            ("reclaim-missing-find", {"reclaim_variant": "missing-find"}),
            ("reclaim-missing-transfer", {"reclaim_variant": "missing-transfer"}),
            ("reclaim-missing-call", {"reclaim_variant": "missing-call"}),
            ("reclaim-missing-fd", {"reclaim_variant": "missing-fd"}),
            ("reclaim-missing-fifo", {"reclaim_variant": "missing-fifo"}),
            ("missing-time-zone-script", {"time_zone_variant": "missing-script"}),
            ("missing-time-zone-rc", {"time_zone_variant": "missing-rc"}),
            ("time-zone-missing-service", {"time_zone_variant": "missing-service"}),
            ("time-zone-not-oneshot", {"time_zone_variant": "not-oneshot"}),
            ("time-zone-not-disabled", {"time_zone_variant": "not-disabled"}),
            ("time-zone-started-at-boot", {"time_zone_variant": "started-at-boot"}),
            ("time-zone-no-start", {"time_zone_variant": "no-start"}),
            ("time-zone-no-property", {"time_zone_variant": "no-property"}),
            ("time-zone-no-auto-read", {"time_zone_variant": "no-auto-read"}),
            ("time-zone-no-set", {"time_zone_variant": "no-set"}),
            ("time-zone-no-marker", {"time_zone_variant": "no-marker"}),
            ("time-zone-writes-setting", {"time_zone_variant": "writes-setting"}),
            ("time-zone-setprop-zone", {"time_zone_variant": "setprop-zone"}),
            ("time-zone-detector", {"time_zone_variant": "detector"}),
            ("time-zone-missing-label", {"time_zone_variant": "missing-label"}),
            ("time-zone-missing-transition", {"time_zone_variant": "missing-transition"}),
            ("time-zone-missing-settings-find", {"time_zone_variant": "missing-settings-find"}),
            ("time-zone-missing-alarm-find", {"time_zone_variant": "missing-alarm-find"}),
            ("time-zone-missing-transfer", {"time_zone_variant": "missing-transfer"}),
            ("time-zone-missing-call", {"time_zone_variant": "missing-call"}),
            ("time-zone-missing-fd", {"time_zone_variant": "missing-fd"}),
            ("time-zone-missing-fifo", {"time_zone_variant": "missing-fifo"}),
            ("time-zone-sets-zone-property", {"time_zone_variant": "sets-zone-property"}),
            ("time-zone-missing-marker-rule", {"time_zone_variant": "missing-marker-rule"}),
            ("time-zone-marker-not-system", {"time_zone_variant": "marker-not-system"}),
            ("time-zone-missing-marker-context", {"time_zone_variant": "missing-marker-context"}),
            ("mesa-gles-alignment", {"mesa_gles_alignment": 4096}),
            ("missing-hal-copy", {"init_variant": "missing-hal-copy"}),
            ("missing-gles-copy", {"init_variant": "missing-gles-copy"}),
            ("missing-timeout-copy", {"init_variant": "missing-timeout-copy"}),
            ("missing-venus-sync", {"init_variant": "missing-venus-sync"}),
            ("venus-sync-at-boot", {"init_variant": "venus-sync-at-boot"}),
            ("venus-sync-other-value", {"init_variant": "venus-sync-other-value"}),
            ("venus-sync-cleared-at-boot", {"init_variant": "venus-sync-cleared-at-boot"}),
            ("vendor-init-cannot-set-venus-option", {"venus_policy_variant": "vendor-init-cannot-set"}),
            ("shell-cannot-read-venus-option", {"venus_policy_variant": "shell-unreadable"}),
            ("missing-audio-ring", {"audio_ring_variant": "missing"}),
            ("default-audio-ring-count", {"audio_ring_variant": "default-count"}),
            ("audio-hal-cannot-read-ring", {"audio_ring_variant": "hal-unreadable"}),
            ("core-audio-hal-cannot-read-ring", {"audio_ring_variant": "core-hal"}),
            ("vendor-init-cannot-set-ring", {"audio_ring_variant": "vendor-init-cannot-set"}),
            ("timeout-copy-without-default", {"init_variant": "timeout-copy-without-default"}),
            ("angle-copy", {"init_variant": "angle-copy"}),
            ("ungated-density", {"init_variant": "ungated-density"}),
            ("missing-rcu-restore", {"init_variant": "missing-rcu-restore"}),
            ("rcu-restore-at-boot", {"init_variant": "rcu-restore-at-boot"}),
            ("missing-swapon", {"init_variant": "missing-swapon"}),
            ("no-adbd-keep-alive", {"init_variant": "no-adbd-keep-alive"}),
            ("swapon-also-at-boot", {"init_variant": "swapon-also-at-boot"}),
            ("swapon-twice", {"init_variant": "swapon-twice"}),
            ("cuttlefish-graphics-rc",
             {"extra_entry": "VENDOR/etc/init/init_graphics.vendor.rc"}),
            ("product-init-rc", {"extra_entry": "PRODUCT/etc/init/init.rc"}),
            ("legacy-ueventd", {"extra_entry": "VENDOR/ueventd.rc"}),
            ("missing-setup-wizard",
             {"entries": {"SYSTEM_EXT/priv-app/LineageSetupWizard/LineageSetupWizard.apk": None}}),
            ("auto-provisioner-bypasses-setup",
             {"extra_entry": "SYSTEM/priv-app/Provision/Provision.apk"}),
            ("gamepad-probe-left-in-the-image",
             {"entries": {"SYSTEM/app/AndariumGamepadProbe/AndariumGamepadProbe.apk":
                          synthetic_apk(debuggable=False)}}),
            ("motion-probe-left-in-the-image",
             {"entries": {"SYSTEM/app/AndariumMotionProbe/AndariumMotionProbe.apk":
                          synthetic_apk(debuggable=False)}}),
            ("webgl-probe-left-in-the-image",
             {"entries": {"SYSTEM/app/AndariumWebGLProbe/AndariumWebGLProbe.apk":
                          synthetic_apk(debuggable=False)}}),
            ("storage-test-left-in-the-system-image",
             {"entries": {"SYSTEM/app/AndariumStorageTests/AndariumStorageTests.apk":
                          synthetic_apk(debuggable=False)}}),
            ("storage-test-left-in-the-data-image",
             {"entries": {"DATA/app/AndariumStorageTests/AndariumStorageTests.apk":
                          synthetic_apk(debuggable=False)}}),
            ("debuggable-application-in-a-user-build",
             {"entries": {"PRODUCT/app/Fixture/Fixture.apk": synthetic_apk(debuggable=True)}}),
            ("debuggable-application-by-reference",
             {"entries": {"PRODUCT/app/Fixture/Fixture.apk": synthetic_apk(debuggable="reference")}}),
            ("application-that-starts-trade-in-mode",
             {"entries": {"SYSTEM/priv-app/Fixture/Fixture.apk":
                          synthetic_apk(permissions=("android.permission.ENTER_TRADE_IN_MODE",))}}),
            ("talkback-that-requests-the-network",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("android.permission.RECORD_AUDIO",
                                                     "android.permission.INTERNET",
                                                     "android.permission.ACCESS_NETWORK_STATE"))}}),
            ("talkback-that-requests-a-google-service-permission",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=(
                              "com.google.android.providers.gsf.permission.READ_GSERVICES",
                              "com.google.android.apps.aicore.service.BIND_SERVICE"))}}),
            ("espeak-that-requests-the-local-network",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=("android.permission.ACCESS_LOCAL_NETWORK",))}}),
            ("talkback-that-requests-a-play-store-permission",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("com.android.vending.BILLING",))}}),
            ("talkback-that-requests-the-network-as-an-sdk-23-permission",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(sdk23=("android.permission.INTERNET",))}}),
            ("talkback-that-requests-the-old-guard-permission",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=(OLD_GUARD_PERMISSION,))}}),
            ("espeak-that-requests-the-old-guard-permission",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=(OLD_GUARD_PERMISSION,))}}),
            ("talkback-that-requests-the-network-settings-permission",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("android.permission.NETWORK_SETTINGS",))}}),
            ("talkback-that-requests-a-network-permission-no-name-rule-catches",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk": synthetic_apk(permissions=(
                 "android.permission.MANAGE_IPSEC_TUNNELS", "android.permission.ACCESS_LOWPAN_STATE",
                 "android.permission.PACKET_KEEPALIVE_OFFLOAD",
                 "android.permission.REQUEST_COMPANION_USE_DATA_IN_BACKGROUND"))}}),
            ("talkback-that-requests-an-ordinary-permission-outside-its-list",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("android.permission.VIBRATE",
                                                     "android.permission.CAMERA"))}}),
            ("espeak-that-requests-a-permission-at-all",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=("android.permission.VIBRATE",))}}),
            ("espeak-that-requests-the-guard-permission-of-talkbacks-package",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=(TALKBACK_GUARD_PERMISSION,))}}),
            ("talkback-that-requests-a-permission-by-resource-reference",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("@0x7f0a0001",))}}),
            ("device-diagnostics-in-the-image",
             {"extra_entry": "SYSTEM/priv-app/DeviceDiagnostics/DeviceDiagnostics.apk"}),
            ("application-without-a-manifest",
             {"entries": {"PRODUCT/app/Fixture/Fixture.apk": b"not an archive"}}),
            ("curated-out-canvas",
             {"extra_entry": "PRODUCT/app/Canvas/Canvas.apk"}),
            ("curated-out-updater",
             {"extra_entry": "SYSTEM_EXT/priv-app/Updater/Updater.apk"}),
            ("fastbootd", {"extra_entry": "SYSTEM/bin/fastbootd"}),
            ("curated-out-profiles",
             {"extra_entry": "SYSTEM_EXT/app/Profiles/Profiles.apk"}),
            ("curated-out-jelly",
             {"extra_entry": "PRODUCT/app/Jelly/Jelly.apk"}),
            ("curated-out-fdroid",
             {"extra_entry": "PRODUCT/app/F-Droid/F-Droid.apk"}),
            ("curated-out-fdroid-extension",
             {"extra_entry": "PRODUCT/priv-app/F-DroidPrivilegedExtension/"
                             "F-DroidPrivilegedExtension.apk"}),
            ("curated-out-fdroid-permissions",
             {"extra_entry": "PRODUCT/etc/permissions/"
                             "privapp-permissions-org.fdroid.fdroid.privileged.xml"}),
            ("missing-video-encoder-service",
             {"entries": {f"VENDOR/bin/hw/{video_service}": None}}),
            ("missing-video-plugin-store",
             {"entries": {"VENDOR/lib64/libc2plugin_store.so": None}}),
            ("missing-video-codec-list",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": None}}),
            ("video-codec-list-lacks-hevc",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 encoders={"c2.v4l2.avc.encoder": "video/avc"})}}),
            ("video-codec-list-lacks-hevc-decoder",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 hardware_decoders={"c2.v4l2.avc.decoder": "video/avc"})}}),
            ("video-codec-list-lacks-vp9-decoder",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 hardware_decoders={name: kind for name, kind in tool.VIDEO_DECODER_CODECS.items()
                                    if name != "c2.v4l2.vp9.decoder"})}}),
            ("video-unapproved-hardware-decoder",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 hardware_decoders={**tool.VIDEO_DECODER_CODECS,
                                    "c2.v4l2.vp8.decoder": "video/x-vnd.on2.vp8"})}}),
            ("video-decoder-without-adaptive-playback",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_features=("dynamic-color-aspects",))}}),
            ("video-software-decoder-listed-not-updated",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoders=("c2.android.avc.decoder",))}}),
            ("video-codec-list-invalid",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": b"<MediaCodecs>"}}),
            ("video-encoder-without-performance-point",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 performance_points=False)}}),
            ("video-encoder-without-measured-frame-rate",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 measured_frame_rates=False)}}),
            ("video-encoder-below-the-size-floor",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 encoder_minimum_size="16x16")}}),
            ("video-encoder-below-the-bitrate-floor",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 encoder_minimum_bitrate="1")}}),
            ("video-performance-point-above-capacity",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 performance_point="3840x2160 30-60")}}),
            ("video-performance-point-malformed",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 performance_point="3840x2160 fast")}}),
            ("video-decoder-without-performance-point",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_performance_points=False)}}),
            ("video-decoder-without-measured-frame-rate",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_measured_frame_rates=False)}}),
            ("video-decoder-point-above-capacity",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_performance_point="3840x2160 60-120")}}),
            ("video-decoder-points-far-below-capacity",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_performance_point="1280x720 60-90")}}),
            ("video-pixel-rate-property",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.video_pixel_rate": "1000"}}),
            ("video-decode-pixel-rate-property",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.video_decode_pixel_rate": "1000"}}),
            ("video-unapproved-decoder-property",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.decoder.supported.av1": "true"}}),
            ("missing-video-decoder-property",
             {"video_properties": {
                 name: value for name, value in good_video_properties.items()
                 if not name.endswith("decoder.supported.h264")}}),
            ("missing-video-vp9-decoder-property",
             {"video_properties": {
                 name: value for name, value in good_video_properties.items()
                 if not name.endswith("decoder.supported.vp9")}}),
            ("video-decode-concurrency-property",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.decode_concurrent_instances": "1"}}),
            ("missing-video-hevc-property",
             {"video_properties": {
                 name: value for name, value in good_video_properties.items()
                 if not name.endswith("supported.hevc")}}),
            ("video-concurrency-property",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.encode_concurrent_instances": "1"}}),
            ("missing-video-ueventd", {"missing_video_ueventd": True}),
            ("video-service-without-camera-group",
             {"entries": {f"VENDOR/etc/init/{video_service}.rc": good_video_rc.replace(
                 " camera", "").encode()}}),
            ("video-seccomp-lacks-eventfd",
             {"entries": {
                 f"VENDOR/etc/seccomp_policy/{video_seccomp_extended}":
                 b"read: 1\n# eventfd2: 1\n"}}),
            ("video-seccomp-lacks-crash-handler-call",
             {"entries": {
                 f"VENDOR/etc/seccomp_policy/{video_seccomp_extended}":
                 good_video_seccomp.replace("uname: 1\n", "").encode()}}),
            ("video-service-label",
             {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                          good_video_contexts.replace("mediacodec_exec", "vendor_file").encode()}}),
        ):
            reject(label, options)

        # Rejections that must name their cause: kernel module files and
        # metadata in any case, and a target ELF whose header is not an
        # AArch64 little-endian ELF64 with whole program-header entries.
        for label, options, reason in (
            ("module-object-suffix", {"extra_entry": "VENDOR/etc/fixture.ko.xz"}, "kernel module"),
            ("module-metadata", {"extra_entry": "VENDOR/etc/modules.dep"}, "kernel module"),
            ("module-metadata-case", {"extra_entry": "VENDOR/etc/MODULES.Dep"}, "kernel module"),
            ("module-blocklist", {"extra_entry": "VENDOR/etc/modules.blocklist"}, "kernel module"),
            ("elf-class", {"elf_edit": ("<B", 4, 3)}, "ELF32 or ELF64"),
            ("elf-big-endian", {"elf_edit": ("<B", 5, 2)}, "little-endian ELF32 or ELF64"),
            ("elf-machine", {"elf_edit": ("<H", 18, 62)}, "not AArch64"),
            ("elf-program-header-size", {"elf_edit": ("<H", 54, 32)}, "program.header"),
            ("missing-armeabi", {"abilist32": "armeabi-v7a"}, "unexpected ABI"),
            ("wrong-abi-order", {"abilist32": "armeabi,armeabi-v7a"}, "unexpected ABI"),
            ("unsupported-abi", {"abilist32": "armeabi-v7a,armeabi,x86"}, "unexpected ABI"),
            ("no-32-bit-abi-cause", {"abilist32": ""}, "unexpected ABI"),
            ("product-model-cause", {"wrong_product_model": True},
             "unexpected product identity ro.product.product.model: 'Android 17 on Andarium'"),
            ("old-product-identity-cause", {"old_product_identity": True},
             "unexpected product identity ro.product.product.brand: 'Android'"),
            ("wrong-vendor-identity-cause", {"wrong_vendor_identity": True},
             "unexpected product identity ro.product.vendor.model: 'Android 17 on Andarium'"),
            ("vendor-build-identity-of-an-eng-build-cause", {"vendor_build_identity": "eng"},
             "the vendor partition does not carry the build's identity: ro.vendor.build.fingerprint"),
            ("vendor-without-a-build-identity-cause", {"vendor_build_identity": "missing"},
             "the vendor partition does not carry the build's identity: ro.vendor.build.fingerprint is None"),
            ("missing-public-fingerprint-cause", {"fingerprint_variant": "missing"},
             "unexpected build fingerprint for a .* build: ''"),
            ("generic-system-fingerprint-cause", {"fingerprint_variant": "generic-system"},
             "unexpected build fingerprint for a .* build: 'Android/generic_system/generic:"),
            ("wrong-type-fingerprint-cause", {"fingerprint_variant": "wrong-type"},
             "unexpected build fingerprint for a .* build: 'Andarium/lineage_andarium/andarium:"),
            ("single-zygote-cause", {"zygote": "zygote64"}, "zygote"),
            ("blocker-missing-service-cause", {"blocker_variant": "missing-service"},
             "does not declare the suspend_blocker service"),
            ("blocker-class-started-cause", {"blocker_variant": "class-started"},
             "starts suspend_blocker with its class"),
            ("blocker-started-at-boot-cause", {"blocker_variant": "started-at-boot"},
             "starts suspend_blocker under 'boot'"),
            ("blocker-no-stop-cause", {"blocker_variant": "no-stop"},
             "does not stop suspend_blocker when the last adb host leaves"),
            ("blocker-binary-cause", {"missing_blocker_binary": True},
             "VENDOR/bin/suspend_blocker"),
            ("net-setup-missing-service-cause", {"net_setup_variant": "missing-service"},
             "does not declare the kernarium_net_setup service"),
            ("net-setup-not-oneshot-cause", {"net_setup_variant": "not-oneshot"},
             "does not declare kernarium_net_setup as oneshot"),
            ("net-setup-not-disabled-cause", {"net_setup_variant": "not-disabled"},
             "does not declare kernarium_net_setup as disabled"),
            ("net-setup-no-exec-start-cause", {"net_setup_variant": "no-exec-start"},
             "does not wait for kernarium_net_setup at post-fs-data"),
            ("net-setup-sysctl-after-start-cause", {"net_setup_variant": "sysctl-after-start"},
             "arp_ignore 1' before kernarium_net_setup"),
            ("net-setup-no-announce-cause", {"net_setup_variant": "no-announce"},
             "arp_announce 2' before kernarium_net_setup"),
            ("net-setup-started-at-boot-cause", {"net_setup_variant": "started-at-boot"},
             "starts kernarium_net_setup under 'boot'"),
            ("net-setup-binary-cause", {"missing_net_setup_binary": True},
             "VENDOR/bin/kernarium_net_setup"),
            ("net-library-cause", {"missing_net_library": True},
             "VENDOR/lib64/cuttlefish_net.so"),
            ("net-policy-label-cause", {"net_policy_variant": "missing-label"},
             "do not label /vendor/bin/kernarium_net_setup"),
            ("net-policy-transition-cause", {"net_policy_variant": "missing-transition"},
             "does not move init into the network setup domain"),
            ("net-policy-sysctl-write-cause", {"net_policy_variant": "no-sysctl-write"},
             "does not let kernarium_net_setup open and write proc_net files"),
            ("wifi-network-missing-rc-cause", {"wifi_network_variant": "missing-rc"},
             "SYSTEM_EXT/etc/init/kernarium_wifi_network.rc"),
            ("wifi-network-missing-service-cause", {"wifi_network_variant": "missing-service"},
             "does not declare the kernarium_wifi_network service"),
            ("wifi-network-not-oneshot-cause", {"wifi_network_variant": "not-oneshot"},
             "does not declare kernarium_wifi_network as oneshot"),
            ("wifi-network-not-disabled-cause", {"wifi_network_variant": "not-disabled"},
             "does not declare kernarium_wifi_network as disabled"),
            ("wifi-network-no-start-cause", {"wifi_network_variant": "no-start"},
             "does not start kernarium_wifi_network when boot completes"),
            ("wifi-network-started-at-boot-cause", {"wifi_network_variant": "started-at-boot"},
             "starts kernarium_wifi_network under 'boot'"),
            ("wifi-network-no-add-cause", {"wifi_network_variant": "no-add"},
             "does not contain 'cmd wifi add-network VirtWifi open'"),
            ("wifi-network-no-list-cause", {"wifi_network_variant": "no-list"},
             "does not contain 'cmd wifi list-networks'"),
            ("wifi-network-no-status-cause", {"wifi_network_variant": "no-status"},
             "does not contain 'cmd wifi status'"),
            ("wifi-network-no-marker-cause", {"wifi_network_variant": "no-marker"},
             re.escape("does not contain 'setprop \"$marker\" 1'")),
            ("wifi-network-no-joined-marker-cause", {"wifi_network_variant": "no-joined-marker"},
             re.escape("does not contain 'setprop \"$marker\" 2'")),
            ("wifi-network-enables-wifi-cause", {"wifi_network_variant": "enables-wifi"},
             "contains 'set-wifi-enabled'"),
            ("wifi-network-connects-cause", {"wifi_network_variant": "connects"},
             "contains 'connect-network'"),
            ("wifi-network-settings-put-cause", {"wifi_network_variant": "settings-put"},
             "contains 'settings put'"),
            ("wifi-network-missing-label-cause", {"wifi_network_variant": "missing-label"},
             "do not label /system_ext/bin/kernarium_wifi_network.sh"),
            ("wifi-network-missing-transition-cause", {"wifi_network_variant": "missing-transition"},
             "does not move init into the Wi-Fi network domain"),
            ("wifi-network-marker-not-system-cause", {"wifi_network_variant": "marker-not-system"},
             "does not define the system-owned kernarium_wifi_network_prop"),
            ("wifi-network-missing-marker-rule-cause", {"wifi_network_variant": "missing-marker-rule"},
             "does not let the Wi-Fi network domain set persist.sys.kernarium.virtwifi_saved"),
            ("wifi-network-missing-binder-transfer-cause", {"wifi_network_variant": "missing-binder-transfer"},
             "does not let the Wi-Fi network domain transfer binders to system_server"),
            ("wifi-network-missing-binder-call-cause", {"wifi_network_variant": "missing-binder-call"},
             "does not let system_server call the Wi-Fi network domain's binder"),
            ("wifi-network-missing-marker-context-cause", {"wifi_network_variant": "missing-marker-context"},
             "do not label persist.sys.kernarium.virtwifi_saved"),
            ("wifi-network-marker-type-bool-cause", {"wifi_network_variant": "marker-type-bool"},
             "type persist.sys.kernarium.virtwifi_saved as 'exact bool', which rejects 2"),
            ("wifi-network-marker-type-enum-one-cause", {"wifi_network_variant": "marker-type-enum-one"},
             "type persist.sys.kernarium.virtwifi_saved as 'exact enum 1', which rejects 2"),
            ("wifi-network-script-cause", {"missing_wifi_network_script": True},
             "SYSTEM_EXT/bin/kernarium_wifi_network.sh"),
            ("wificond-present-cause", {"entries": {"SYSTEM/bin/wificond": synthetic_aarch64_elf()}},
             "nonstatic image or metadata is forbidden: SYSTEM/bin/wificond"),
            ("kernel-no-macvlan-cause", {"kernel_config_variant": "missing:CONFIG_MACVLAN"},
             "packaged kernel configuration lacks CONFIG_MACVLAN=y"),
            ("kernel-no-cfg80211-cause", {"kernel_config_variant": "unset:CONFIG_CFG80211"},
             "packaged kernel configuration lacks CONFIG_CFG80211=y"),
            ("kernel-no-virt-wifi-cause", {"kernel_config_variant": "missing:CONFIG_VIRT_WIFI"},
             "packaged kernel configuration lacks CONFIG_VIRT_WIFI=y"),
            ("kernel-macvlan-module-cause", {"kernel_config_variant": "module:CONFIG_MACVLAN"},
             "packaged kernel configuration lacks CONFIG_MACVLAN=y"),
            ("kernel-cfg80211-module-cause", {"kernel_config_variant": "module:CONFIG_CFG80211"},
             "packaged kernel configuration lacks CONFIG_CFG80211=y"),
            ("kernel-virt-wifi-module-cause", {"kernel_config_variant": "module:CONFIG_VIRT_WIFI"},
             "packaged kernel configuration lacks CONFIG_VIRT_WIFI=y"),
            ("kernel-no-erofs-cause", {"kernel_config_variant": "missing:CONFIG_EROFS_FS"},
             "packaged kernel configuration lacks CONFIG_EROFS_FS=y"),
            ("kernel-no-erofs-zstd-cause",
             {"kernel_config_variant": "unset:CONFIG_EROFS_FS_ZIP_ZSTD"},
             "packaged kernel configuration lacks CONFIG_EROFS_FS_ZIP_ZSTD=y"),
            ("kernel-erofs-zip-module-cause",
             {"kernel_config_variant": "module:CONFIG_EROFS_FS_ZIP"},
             "packaged kernel configuration lacks CONFIG_EROFS_FS_ZIP=y"),
            ("wifi-dirs-missing-cause", {"wifi_dirs_variant": "missing"},
             "does not run 'mkdir /data/vendor/wifi 0770 wifi wifi' at boot"),
            ("wifi-dirs-wrong-owner-cause", {"wifi_dirs_variant": "wrong-owner"},
             "does not run 'mkdir /data/vendor/wifi/wpa 0770 wifi wifi' at boot"),
            ("missing-wifi-feature-cause", {"missing_wifi_feature": True},
             "required feature is not declared: android.hardware.wifi"),
            ("missing-gles-copy-cause", {"init_variant": "missing-gles-copy"},
             "does not copy the OpenGL ES version at early-init"),
            ("missing-timeout-copy-cause", {"init_variant": "missing-timeout-copy"},
             "does not copy the hardware timeout multiplier at early-init"),
            ("missing-venus-sync-cause", {"init_variant": "missing-venus-sync"},
             "does not run 'setprop debug.mesa.vn.perf no_async_mem_alloc' at early-init"),
            ("venus-sync-at-boot-cause", {"init_variant": "venus-sync-at-boot"},
             "runs 'setprop debug.mesa.vn.perf no_async_mem_alloc' at 'boot', not"),
            ("venus-sync-other-value-cause", {"init_variant": "venus-sync-other-value"},
             "runs 'setprop debug.mesa.vn.perf no_async_mem_alloc,log_ctx_info' at 'early-init', not"),
            ("venus-sync-cleared-at-boot-cause", {"init_variant": "venus-sync-cleared-at-boot"},
             "runs 'setprop debug.mesa.vn.perf \"\"' at 'boot', not"),
            ("vendor-init-cannot-set-venus-option-cause", {"venus_policy_variant": "vendor-init-cannot-set"},
             "vendor_init may not set debug.mesa.vn.perf, labelled debug_prop"),
            ("shell-cannot-read-venus-option-cause", {"venus_policy_variant": "shell-unreadable"},
             "shell may not read debug.mesa.vn.perf, labelled debug_prop"),
            ("missing-audio-ring-cause", {"audio_ring_variant": "missing"},
             r"unexpected vendor audio property ro\.hardware\.audio\.tinyalsa\.period_count: \[\]"),
            ("default-audio-ring-count-cause", {"audio_ring_variant": "default-count"},
             r"unexpected vendor audio property ro\.hardware\.audio\.tinyalsa\.period_count: "
             r"\['2'\]"),
            ("audio-hal-cannot-read-ring-cause", {"audio_ring_variant": "hal-unreadable"},
             "hal_audio_default may not read ro.hardware.audio.tinyalsa.period_count, "
             "labelled vendor_default_prop"),
            ("core-audio-hal-cannot-read-ring-cause", {"audio_ring_variant": "core-hal"},
             "hal_audio_default may not read ro.hardware.audio.tinyalsa.period_count"),
            ("vendor-init-cannot-set-ring-cause", {"audio_ring_variant": "vendor-init-cannot-set"},
             "vendor_init may not set ro.hardware.audio.tinyalsa.period_count, "
             "labelled vendor_default_prop"),
            ("timeout-copy-without-default-cause",
             {"init_variant": "timeout-copy-without-default"},
             "runs 'setprop ro.hw_timeout_multiplier \\$\\{ro.boot.hw_timeout_multiplier\\}' "
             "at 'early-init', not"),
            ("missing-aep-feature-cause",
             {"entries": {"VENDOR/etc/permissions/android.hardware.opengles.aep.xml": None}},
             "required feature is not declared: android.hardware.opengles.aep"),
            ("missing-gles-deqp-level-cause",
             {"entries": {"VENDOR/etc/permissions/"
                          "android.software.opengles.deqp.level-latest.xml": None}},
             "must be declared once at the qualified level 132776705: \\[\\]"),
            ("unqualified-gles-deqp-level-cause",
             {"entries": {"VENDOR/etc/permissions/"
                          "android.software.opengles.deqp.level-latest.xml":
                          b'<permissions><feature name="android.software.opengles.deqp.level" '
                          b'version="132842241"/></permissions>'}},
             "qualified level 132776705: \\['132842241'\\]"),
            ("vulkan-deqp-level-cause",
             {"entries": {"VENDOR/etc/permissions/"
                          "android.software.vulkan.deqp.level-latest.xml":
                          b'<permissions><feature name="android.software.vulkan.deqp.level" '
                          b'version="132776705"/></permissions>'}},
             "a Vulkan dEQP level is declared"),
            ("missing-wifi-overlay-cause",
             {"entries": {"VENDOR/overlay/KernariumWifiOverlay.apk": None}},
             "VENDOR/overlay/KernariumWifiOverlay.apk"),
            ("wifi-overlay-missing-permanent-cause", {"wifi_overlay_variant": "missing-permanent"},
             "KernariumWifiOverlay.apk does not set config_wifiDisableReasonNoInternetPermanentThreshold: "
             "the platform would disable the guest's one network"),
            ("wifi-overlay-missing-temporary-cause", {"wifi_overlay_variant": "missing-temporary"},
             "KernariumWifiOverlay.apk does not set config_wifiDisableReasonNoInternetTemporaryThreshold"),
            ("wifi-overlay-missing-both-cause", {"wifi_overlay_variant": "missing-both"},
             "KernariumWifiOverlay.apk does not set config_wifiDisableReasonNoInternetPermanentThreshold"),
            ("wifi-overlay-permanent-one-cause", {"wifi_overlay_variant": "permanent-one"},
             "config_wifiDisableReasonNoInternetPermanentThreshold is not 2147483647 in .*"
             "KernariumWifiOverlay.apk: a lower threshold is reached by the first failed internet check"),
            ("wifi-overlay-temporary-zero-cause", {"wifi_overlay_variant": "temporary-zero"},
             "config_wifiDisableReasonNoInternetTemporaryThreshold is not 2147483647 in .*KernariumWifiOverlay"),
            ("wifi-overlay-temporary-large-not-largest-cause",
             {"wifi_overlay_variant": "temporary-large-not-largest"},
             "config_wifiDisableReasonNoInternetTemporaryThreshold is not 2147483647 in .*KernariumWifiOverlay"),
            ("wifi-overlay-permanent-negative-cause", {"wifi_overlay_variant": "permanent-negative"},
             "config_wifiDisableReasonNoInternetPermanentThreshold is not 2147483647 in .*KernariumWifiOverlay"),
            ("wifi-overlay-qualified-low-cause", {"wifi_overlay_variant": "qualified-low"},
             "config_wifiDisableReasonNoInternetPermanentThreshold is not 2147483647 in .*KernariumWifiOverlay"),
            ("wifi-overlay-no-table-cause", {"wifi_overlay_variant": "no-table"},
             "KernariumWifiOverlay.apk has no readable resource table"),
            ("ethernet-feature-cause", {"wifi_feature_file": "android.hardware.ethernet"},
             "VENDOR/etc/permissions/android.hardware.ethernet.xml"),
            ("wifi-aware-feature-cause", {"wifi_feature_file": "android.hardware.wifi.aware"},
             "absent hardware feature declaration is forbidden"),
            ("wifi-rtt-feature-cause", {"wifi_feature_file": "android.hardware.wifi.rtt"},
             "absent hardware feature declaration is forbidden"),
            ("wifi-direct-feature-cause", {"wifi_feature_file": "android.hardware.wifi.direct"},
             "absent hardware feature declaration is forbidden"),
            ("wifi-passpoint-feature-cause", {"wifi_feature_file": "android.hardware.wifi.passpoint"},
             "absent hardware feature declaration is forbidden"),
            ("missing-wifi-hal-apex-cause", {"wifi_hal_variant": "missing-apex"},
             "com.android.hardware.wifi.apex is missing: the product needs the Wi-Fi HAL"),
            ("wifi-hal-without-binary-cause", {"wifi_hal_variant": "no-binary"},
             "does not hold the service binary bin/hw/android.hardware.wifi-service"),
            ("wifi-hal-without-fragment-cause", {"wifi_hal_variant": "no-fragment"},
             "does not declare IWifi/default in a VINTF fragment"),
            ("wifi-hal-without-service-cause", {"wifi_hal_variant": "no-service"},
             "does not start the vendor.wifi_hal_legacy service"),
            ("missing-supplicant-apex-cause", {"supplicant_variant": "missing-apex"},
             "com.google.cf.wpa_supplicant.apex is missing: the product needs the Wi-Fi supplicant"),
            ("supplicant-without-binary-cause", {"supplicant_variant": "no-binary"},
             "does not hold the service binary bin/hw/wpa_supplicant"),
            ("supplicant-without-fragment-cause", {"supplicant_variant": "no-fragment"},
             "does not declare ISupplicant/default in a VINTF fragment"),
            ("supplicant-without-service-cause", {"supplicant_variant": "no-service"},
             "does not start the wpa_supplicant service"),
            ("adb-prop-context-cause", {"missing_adb_prop_context": True},
             "do not label sys.kernarium.adb.host_connected"),
            ("adb-prop-policy-cause", {"missing_adb_prop_policy": True},
             "does not let adbd set sys.kernarium.adb.host_connected"),
            ("media-codecs-missing-cause", {"media_codecs_variant": "missing"},
             "VENDOR/etc/media_codecs_c2.xml"),
            ("media-codecs-malformed-cause", {"media_codecs_variant": "malformed"},
             "media_codecs_c2.xml is not (valid XML|well-formed)"),
            ("media-codecs-no-hevc-cause", {"media_codecs_variant": "no-hevc"},
             "does not update c2.android.hevc.decoder"),
            ("media-codecs-low-hevc-rate-cause", {"media_codecs_variant": "low-hevc-rate"},
             "allows the HEVC decoder only 3000000 blocks per second"),
            ("media-codecs-small-vp9-size-cause", {"media_codecs_variant": "small-vp9-size"},
             "limits the VP9 decoder to 2048x2048"),
            ("media-codecs-small-vp9-blocks-cause", {"media_codecs_variant": "small-vp9-blocks"},
             "allows the VP9 decoder 16384 blocks"),
            ("oversize-platform-policy-cause", {"oversize_platform_policy": True},
             "SYSTEM/etc/selinux/plat_sepolicy.cil"),
            ("audio-binder-policy-cause", {"missing_audio_binder_policy": True},
             "audio HAL processes call each other's binder"),
            ("wifi-debug-property-policy-cause", {"missing_wifi_debug_policy": True},
             "Wi-Fi HAL read persist_vendor_debug_wifi_prop"),
            ("camera-fence-policy-cause", {"camera_fence_variant": "missing"},
             "camera HAL use the display composer's file descriptors"),
            ("camera-fence-source-other-type-cause", {"camera_fence_variant": "source-other-type"},
             "camera HAL use the display composer's file descriptors"),
            ("camera-fence-target-other-type-cause", {"camera_fence_variant": "target-other-type"},
             "camera HAL use the display composer's file descriptors"),
            ("camera-fence-source-short-suffix-cause", {"camera_fence_variant": "source-short-suffix"},
             "camera HAL use the display composer's file descriptors"),
            ("camera-fence-target-long-suffix-cause", {"camera_fence_variant": "target-long-suffix"},
             "camera HAL use the display composer's file descriptors"),
            ("clearkey-memfd-missing-cause", {"clearkey_memfd_variant": "missing"},
             "ClearKey DRM HAL read, write and map the memfd files"),
            ("clearkey-memfd-no-write-cause", {"clearkey_memfd_variant": "no-write"},
             "ClearKey DRM HAL read, write and map the memfd files"),
            ("clearkey-memfd-isolated-apps-cause", {"clearkey_memfd_variant": "all-apps"},
             "other than isolated apps"),
            ("core-sets-vendor-prop-cause", {"core_sets_vendor_prop": True},
             "lets the core domain adbd set the vendor property type virtual_face_hal_prop"),
            ("system-ext-core-sets-vendor-prop-cause", {"system_ext_core_sets_vendor_prop": True},
             "system_ext policy lets the core domain system_server set the vendor property type"),
            ("adb-prop-not-system-cause", {"adb_prop_not_system": True},
             "does not define the system-owned kernarium_adb_host_prop"),
            ("lineage-mediaprovider-trusted-cause", {"lineage_policy": "mediaprovider-trusted"},
             "makes mediaprovider_app an mlstrustedsubject"),
            ("lineage-mediaprovider-privapp-cause", {"lineage_policy": "mediaprovider-privapp"},
             "lets mediaprovider_app use privapp_data_file"),
            ("lineage-fsck-sys-admin-cause", {"lineage_policy": "fsck-sys-admin"},
             "gives fsck_untrusted sys_admin"),
            ("lineage-vold-relabelto-cause", {"lineage_policy": "vold-relabelto"},
             "lets vold relabel a filesystem to sdcard_posix"),
            ("lineage-permissive-backuptool-cause", {"lineage_policy": "permissive-backuptool"},
             "makes the domain backuptool permissive"),
            ("missing-mediaserver-audit-cause", {"missing_mediaserver_audit": True},
             "does not silence the denial of mediaserver reading vendor_default_prop"),
            ("missing-shell-density-audit-cause", {"missing_shell_density_audit": True},
             "does not silence the denial of the shell domain reading qemu_sf_lcd_density_prop"),
            ("adb-prop-unreadable-by-vendor-init-cause",
             {"adb_prop_unreadable_by_vendor_init": True},
             "names sys.kernarium.adb.host_connected, labelled kernarium_adb_host_prop, "
             "which vendor_init may not read"),
            ("missing-lockscreen-default-cause", {"missing_lockscreen_default": True},
             "unexpected product property ro.lockscreen.disable.default"),
            ("missing-use-fiemap-cause", {"use_fiemap_variant": "missing"},
             "unexpected product property apexd.config.use_fiemap: \\[\\]"),
            ("use-fiemap-true-cause", {"use_fiemap_variant": "true"},
             "unexpected product property apexd.config.use_fiemap: \\['true'\\]"),
            ("missing-lmkd-binary-cause", {"lmkd_variant": "missing-binary"},
             "SYSTEM/bin/lmkd is missing"),
            ("lmkd-without-the-gpu-reclaim-cause", {"lmkd_variant": "old-binary"},
             "SYSTEM/bin/lmkd lacks 'ro.lmk.gpu_reclaim': the p0027 topic is missing from the build"),
            ("lmkd-without-the-setting-name-cause", {"lmkd_variant": "no-property-name"},
             "SYSTEM/bin/lmkd lacks 'ro.lmk.gpu_reclaim'"),
            ("lmkd-without-the-report-path-cause", {"lmkd_variant": "no-report-path"},
             "SYSTEM/bin/lmkd lacks '/sys/class/drm/card0/gpu_memory_report'"),
            ("lmkd-without-the-kill-reason-cause", {"lmkd_variant": "no-kill-reason"},
             "SYSTEM/bin/lmkd lacks 'graphics memory pressure'"),
            ("missing-lmkd-gpu-reclaim-property-cause", {"lmkd_variant": "property-missing"},
             "unexpected product property ro.lmk.gpu_reclaim: \\[\\]"),
            ("lmkd-gpu-reclaim-property-off-cause", {"lmkd_variant": "property-off"},
             "unexpected product property ro.lmk.gpu_reclaim: \\['false'\\]"),
            ("unlabelled-lmkd-gpu-reclaim-property-cause", {"lmkd_variant": "no-property-label"},
             "lmkd may not read ro.lmk.gpu_reclaim, labelled default_prop"),
            ("unreadable-lmkd-gpu-reclaim-property-cause", {"lmkd_variant": "property-unreadable"},
             "lmkd may not read ro.lmk.gpu_reclaim, labelled lmkd_config_prop"),
            ("gpu-report-type-not-a-sysfs-type-cause", {"lmkd_variant": "type-not-sysfs"},
             "sysfs_gpu_memory is not a sysfs type of the policy"),
            ("gpu-report-not-labelled-cause", {"lmkd_variant": "no-genfs"},
             "the vendor policy does not label /devices/virtio-uml-cmdline/virtio-uml.0/drm/card0/gpu_memory_report sysfs_gpu_memory \\(it has None\\)"),
            ("gpu-report-labelled-as-the-device-cause", {"lmkd_variant": "genfs-wrong-type"},
             "\\(it has sysfs_gpu\\)"),
            ("gpu-report-labelled-at-another-path-cause", {"lmkd_variant": "genfs-other-path"},
             "the vendor policy does not label /devices/virtio-uml-cmdline/virtio-uml.0/drm/card0/gpu_memory_report"),
            ("lmkd-cannot-use-the-gpu-report-cause", {"lmkd_variant": "no-report-rule"},
             "lmkd may not open, read and write sysfs_gpu_memory"),
            ("lmkd-cannot-write-the-gpu-report-cause", {"lmkd_variant": "no-report-write"},
             "lmkd may not open, read and write sysfs_gpu_memory"),
            ("lmkd-cannot-search-the-gpu-device-cause", {"lmkd_variant": "no-dir-search"},
             "lmkd may not search sysfs_gpu directories"),
            ("no-adbd-keep-alive-cause", {"init_variant": "no-adbd-keep-alive"},
             "does not run 'start adbd' at property:init.svc.adbd=stopped"),
            ("missing-swapon-cause", {"init_variant": "missing-swapon"},
             "does not run 'swapon_all /vendor/etc/fstab.uml' once at property:sys.boot_completed=1"),
            ("swapon-twice-cause", {"init_variant": "swapon-twice"},
             "does not run 'swapon_all /vendor/etc/fstab.uml' once at property:sys.boot_completed=1"),
            ("swapon-also-at-boot-cause", {"init_variant": "swapon-also-at-boot"},
             "turns swap on under 'boot': swapon_all /vendor/etc/fstab.uml"),
            ("userdata-grow-missing-cause", {"userdata_grow_variant": "missing"},
             "does not run 'exec - root system -- /system/bin/resize2fs [^']*' once at "
             "post-fs-data"),
            ("userdata-grow-offline-only-cause", {"userdata_grow_variant": "offline-only"},
             "does not run 'exec - root system -- /system/bin/resize2fs [^']*' once at "
             "post-fs-data"),
            ("userdata-grow-offline-also-cause", {"userdata_grow_variant": "offline-also"},
             "handles userdata differently under 'late-fs': exec -- /system/bin/resize2fs "
             "/dev/block/by-name/userdata"),
            ("userdata-grow-twice-cause", {"userdata_grow_variant": "twice"},
             "does not run 'exec - root system -- /system/bin/resize2fs [^']*' once at "
             "post-fs-data"),
            ("userdata-grow-forced-cause", {"userdata_grow_variant": "forced"},
             "handles userdata differently under 'post-fs-data': exec - root system -- "
             "/system/bin/resize2fs -f /dev/block/by-name/userdata"),
            ("userdata-grow-no-group-cause", {"userdata_grow_variant": "no-group"},
             "does not run 'exec - root system -- /system/bin/resize2fs [^']*' once at "
             "post-fs-data"),
            ("userdata-grow-at-boot-cause", {"userdata_grow_variant": "at-boot"},
             "handles userdata differently under 'boot': exec - root system -- "
             "/system/bin/resize2fs /dev/block/by-name/userdata"),
            ("userdata-grow-policy-no-capability-cause", {"userdata_grow_policy_variant": "no-capability"},
             "does not let the fsck domain have \\['sys_resource'\\] on self:capability"),
            ("userdata-grow-policy-no-dir-open-cause", {"userdata_grow_policy_variant": "no-dir-open"},
             "does not let the fsck domain have \\['ioctl', 'open', 'read'\\] on "
             "system_data_root_file:dir"),
            ("userdata-grow-policy-no-ioctl-cause", {"userdata_grow_policy_variant": "no-ioctl"},
             "does not let the fsck domain issue EXT4_IOC_RESIZE_FS"),
            ("userdata-grow-policy-more-ioctls-cause", {"userdata_grow_policy_variant": "more-ioctls"},
             "lets the fsck domain issue more ioctls on /data than EXT4_IOC_RESIZE_FS"),
            ("adb-secure-off-in-product-cause", {"adb_secure_variant": "product"},
             "PRODUCT/etc/build.prop sets ro.adb.secure=0: adb authorization is the stock one"),
            ("adb-secure-false-in-product-cause", {"adb_secure_variant": "product-false"},
             "PRODUCT/etc/build.prop sets ro.adb.secure=false: adb authorization is the stock one"),
            ("adb-secure-off-in-system-cause", {"adb_secure_variant": "system"},
             "SYSTEM/build.prop sets ro.adb.secure=0: adb authorization is the stock one"),
            ("unreliable-present-fence-cause", {"vendor_property_variant": "present-fence-unreliable"},
             "marks the present fences unreliable: ro.vendor.hwc.drm.present_fence_not_reliable=true"),
            ("missing-incremental-property-cause", {"vendor_property_variant": "no-incremental"},
             "VENDOR/build.prop lacks ro.incremental.enable=1: None"),
            ("disabled-incremental-property-cause", {"vendor_property_variant": "incremental-off"},
             "VENDOR/build.prop lacks ro.incremental.enable=1: '0'"),
            ("cuttlefish-board-cause", {"board_variant": "cutf"},
             "unexpected board name ro.product.board in VENDOR/build.prop: 'cutf', "
             "not 'andarium'"),
            ("missing-board-cause", {"board_variant": "missing"},
             "unexpected board name ro.product.board in VENDOR/build.prop: None"),
            ("missing-odm-soc-manufacturer-cause", {"soc_variant": "missing-manufacturer"},
             "unexpected SoC identity ro.soc.manufacturer in ODM/etc/build.prop: None, "
             "not 'Kernarium'"),
            ("missing-odm-soc-model-cause", {"soc_variant": "missing-model"},
             "unexpected SoC identity ro.soc.model in ODM/etc/build.prop: None, "
             "not 'Virtual CPU'"),
            ("cuttlefish-soc-cause", {"soc_variant": "cuttlefish"},
             "unexpected SoC identity ro.soc.manufacturer in ODM/etc/build.prop: 'Google'"),
            ("joined-soc-model-cause", {"soc_variant": "joined-model"},
             "unexpected SoC identity ro.soc.model in ODM/etc/build.prop: "
             "'Kernarium Virtual CPU', not 'Virtual CPU'"),
            ("lowercase-soc-model-cause", {"soc_variant": "lowercase-model"},
             "unexpected SoC identity ro.soc.model in ODM/etc/build.prop: 'Virtual cpu'"),
            ("unknown-soc-model-cause", {"soc_variant": "unknown-model"},
             "unexpected SoC identity ro.soc.model in ODM/etc/build.prop: 'unknown'"),
            ("vendor-soc-manufacturer-cause", {"soc_variant": "vendor-manufacturer"},
             "VENDOR/build.prop sets ro.soc.manufacturer=Google: the SoC identity is the ODM"),
            ("vendor-soc-model-cause", {"soc_variant": "vendor-model"},
             "VENDOR/build.prop sets ro.soc.model=vsoc_arm64_pgagnostic: the SoC identity is the ODM"),
            ("vendor-soc-other-cause", {"soc_variant": "vendor-other"},
             "VENDOR/build.prop sets ro.soc.et=true: the SoC identity is the ODM"),
            ("vendor-soc-same-as-odm-cause", {"soc_variant": "vendor-same"},
             "VENDOR/build.prop sets ro.soc.manufacturer=Kernarium: the SoC identity is the ODM"),
            ("missing-adbd-apex-cause", {"adbd_variant": "missing-apex"},
             "SYSTEM/apex/com.android.adbd.apex is missing: the product needs the adb daemon"),
            ("adbd-apex-without-binary-cause", {"adbd_variant": "no-binary"},
             "SYSTEM/apex/com.android.adbd.apex does not hold the adb daemon bin/adbd"),
            ("adbd-without-host-key-cause", {"adbd_variant": "no-key"},
             "holds an adbd that does not read ro.boot.kernarium.adb_key"),
            ("adbd-without-tcp-policy-cause", {"adbd_variant": "no-tcp"},
             "holds an adbd that does not read ro.boot.kernarium.adb_tcp"),
            ("adbd-without-debugging-setting-cause", {"adbd_variant": "no-debugging-setting"},
             "holds an adbd that does not read persist.sys.usb.config"),
            ("adbd-without-key-rule-cause", {"adbd_variant": "no-key-rule"},
             "holds an adbd without the key rule of route p0011"),
            ("adb-root-service-without-the-debuggable-rule-cause", {"adbd_variant": "no-root-guard"},
             "SYSTEM_EXT/bin/adb_root does not hold the debuggable-build rule of route p0011"),
            ("missing-adb-root-service-cause", {"adbd_variant": "no-root-service"},
             "SYSTEM_EXT/bin/adb_root is missing: the product needs LineageOS's adb_root service"),
            ("signed-with-the-test-key-cause", {"signed": True, "ota_certificates": "build"},
             "holds the members .* not only keys/releasekey.x509.pem: the target files are not "
             "signed with the release keys"),
            ("signed-with-the-test-key-path-cause", {"signed": True, "ota_certificates": "test-key-path"},
             "not signed with the release keys"),
            ("signed-with-an-extra-certificate-cause", {"signed": True, "ota_certificates": "extra"},
             "not signed with the release keys"),
            ("signed-without-recovery-certificates-cause", {"signed": True, "ota_certificates": "missing-recovery"},
             "RECOVERY/RAMDISK/system/etc/security/otacerts.zip is missing from the signed"),
            ("unsigned-with-the-release-member-cause", {"ota_certificates": "release"},
             "holds the members .* not only testkey.x509.pem: the target files are not an unsigned build"),
            ("unsigned-with-an-extra-certificate-cause", {"ota_certificates": "extra"},
             "not an unsigned build"),
            ("unsigned-without-recovery-certificates-cause", {"ota_certificates": "missing-recovery"},
             "RECOVERY/RAMDISK/system/etc/security/otacerts.zip is missing from the unsigned"),
            ("services-jar-without-the-adb-topics-cause", {"services_jar_variant": "no-marker"},
             "lacks the AdbService topics of route p0007"),
            ("services-jar-without-the-display-topic-cause", {"services_jar_variant": "no-display-marker"},
             "lacks the display topic that reads the default configuration's refresh rate throttling map "
             "of route p0007"),
            ("missing-services-jar-cause", {"services_jar_variant": "missing"},
             "SYSTEM/framework/services.jar is missing"),
            ("missing-old-application-library", {"oldapp_variant": "missing-library"},
             "required entries are missing: .*SYSTEM/lib/libkernarium_oldapp.so"),
            ("old-application-library-is-64-bit", {"oldapp_variant": "not-arm"},
             "64-bit ELF in a 32-bit library directory: SYSTEM/lib/libkernarium_oldapp.so"),
            ("old-application-library-without-jni-onload", {"oldapp_variant": "no-entry"},
             "libkernarium_oldapp.so has no JNI_OnLoad"),
            ("missing-framework-jar", {"oldapp_variant": "missing-framework"},
             "SYSTEM/framework/framework.jar is missing"),
            ("framework-jar-without-the-old-application-hook", {"oldapp_variant": "no-load-marker"},
             "lacks the ActivityThread topic that loads the old-application layer of route p0007"),
            ("framework-jar-without-the-old-application-log-line", {"oldapp_variant": "no-log-marker"},
             "lacks the log line of that topic of route p0007"),
            ("framework-jar-loads-the-old-application-layer-for-every-app", {"oldapp_variant": "no-switch-marker"},
             "lacks the per-application switch .* of that topic of route p0007"),
            ("input-flinger-without-the-pointer-capture-led-cause", {"input_flinger_variant": "no-marker"},
             "lacks the pointer capture LED topic of route p0022"),
            ("missing-input-flinger-cause", {"input_flinger_variant": "missing"},
             "SYSTEM/lib64/libinputflinger.so is missing"),
            ("network-stack-without-the-wifi-topic-cause", {"network_stack_variant": "no-marker"},
             "lacks the NetworkMonitor topic of route p0029"),
            ("network-stack-without-the-wifi-check-report-cause", {"network_stack_variant": "no-check-marker"},
             "'Wi-Fi internet check failed, the network stays validated' is not in its dex files"),
            ("settings-without-the-wifi-tracker-topic-cause", {"wifi_tracker_variant": "settings-no-marker"},
             "SYSTEM_EXT/priv-app/Settings/Settings.apk lacks the WifiTrackerLib topic of route p0033"),
            ("system-ui-without-the-wifi-tracker-topic-cause", {"wifi_tracker_variant": "systemui-no-marker"},
             "SYSTEM_EXT/priv-app/SystemUI/SystemUI.apk lacks the WifiTrackerLib topic of route p0033"),
            ("missing-network-stack-cause", {"network_stack_variant": "missing"},
             "SYSTEM/priv-app/NetworkStack/NetworkStack.apk is missing"),
            ("adbd-cannot-read-launch-properties-cause", {"adbd_launch_unreadable": True},
             "adbd may not read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-with-the-bootloader-type-cause", {"adb_key_variant": "bootloader-label"},
             "ro.boot.kernarium.adb_key is labelled bootloader_prop, not kernarium_adb_key_prop"),
            ("adb-key-adbd-cannot-read-cause", {"adb_key_variant": "adbd-unreadable"},
             "adbd may not read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-untrusted-app-reads-cause", {"adb_key_variant": "untrusted-app-reads"},
             "untrusted_app may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-appdomain-reads-cause", {"adb_key_variant": "appdomain-reads"},
             "appdomain may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-untrusted-app-34-reads-cause", {"adb_key_variant": "untrusted-app-34-reads"},
             "untrusted_app_34 may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-untrusted-app-29-reads-cause", {"adb_key_variant": "untrusted-app-29-reads"},
             "untrusted_app_29 may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-runas-app-reads-cause", {"adb_key_variant": "runas-app-reads"},
             "runas_app may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-sdk-sandbox-reads-cause", {"adb_key_variant": "sdk-sandbox-reads"},
             "sdk_sandbox_audit may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-shell-reads-cause", {"adb_key_variant": "shell-reads"},
             "shell may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("adb-key-generated-attribute-reads-cause", {"adb_key_variant": "generated-attribute-reads"},
             "gmscore_app may read ro.boot.kernarium.adb_key, labelled kernarium_adb_key_prop"),
            ("missing-cas-apex-cause", {"cas_hal_variant": "missing-apex"},
             "com.android.hardware.cas.apex is missing: the product needs the default CAS HAL"),
            ("cas-apex-without-binary-cause", {"cas_hal_variant": "no-binary"},
             "does not hold the service binary bin/hw/android.hardware.cas-service.example"),
            ("cas-apex-without-fragment-cause", {"cas_hal_variant": "no-fragment"},
             "does not declare IMediaCasService/default in a VINTF fragment"),
            ("cas-apex-without-service-cause", {"cas_hal_variant": "no-service"},
             "does not start the vendor.cas-default service"),
            ("missing-cas-plugin-64-cause", {"cas_hal_variant": "no-plugin64"},
             "the ClearKey CAS plugin is missing: VENDOR/lib64/mediacas/libclearkeycasplugin.so"),
            ("missing-cas-plugin-32-cause", {"cas_hal_variant": "no-plugin32"},
             "the ClearKey CAS plugin is missing: VENDOR/lib/mediacas/libclearkeycasplugin.so"),
            ("missing-discrete-ops-overlay-cause", {"discrete_ops_variant": "missing-overlay"},
             "SimpleDeviceConfig__lineage_andarium__auto_generated_rro_product.apk is missing"),
            ("missing-framework-overlay-cause", {"wallpaper_variant": "missing-overlay"},
             "framework-res__lineage_andarium__auto_generated_rro_product.apk is missing"),
            ("missing-default-wallpaper-variant-cause", {"wallpaper_variant": "missing-variant"},
             "has no res/drawable-sw720dp-nodpi-v13/default_wallpaper.png"),
            ("lineage-size-default-wallpaper-cause", {"wallpaper_variant": "lineage-size"},
             "res/drawable-sw600dp-nodpi-v13/default_wallpaper.png in .* is 1920x1920: the tablet "
             "default wallpaper must be a square of at most 1024 pixels"),
            ("non-square-default-wallpaper-cause", {"wallpaper_variant": "not-square"},
             "is 1024x768: the tablet default wallpaper must be a square"),
            ("default-wallpaper-not-png-cause", {"wallpaper_variant": "not-png"},
             "default_wallpaper.png in .* is not a PNG with a leading IHDR chunk"),
            ("lineage-discrete-ops-list-only-cause", {"discrete_ops_variant": "lineage-list-only"},
             "keeps AOSP's default ops; the closest lacks 2, 25, 41, 42, 63, 73, 88, 89, 120, "
             "136, 141, 147, 149, 150, 152, 170, 171, 172, 173"),
            ("short-discrete-ops-list-cause", {"discrete_ops_variant": "short-device-list"},
             "the closest lacks 141$"),
            ("missing-pinner-overlay-cause",
             {"entries": {"VENDOR/overlay/KernariumPinnerOverlay.apk": None}},
             "VENDOR/overlay/KernariumPinnerOverlay.apk"),
            ("gamepad-probe-left-in-the-image-cause",
             {"entries": {"SYSTEM/app/AndariumGamepadProbe/AndariumGamepadProbe.apk":
                          synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: "
             "SYSTEM/app/AndariumGamepadProbe"),
            ("motion-probe-left-in-the-image-cause",
             {"entries": {"SYSTEM/app/AndariumMotionProbe/AndariumMotionProbe.apk":
                          synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: "
             "SYSTEM/app/AndariumMotionProbe"),
            ("webgl-probe-left-in-the-image-cause",
             {"entries": {"SYSTEM/app/AndariumWebGLProbe/AndariumWebGLProbe.apk":
                          synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: "
             "SYSTEM/app/AndariumWebGLProbe"),
            ("storage-test-left-in-the-system-image-cause",
             {"entries": {"SYSTEM/app/AndariumStorageTests/AndariumStorageTests.apk":
                          synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: "
             "SYSTEM/app/AndariumStorageTests"),
            ("storage-test-left-in-the-data-image-cause",
             {"entries": {"DATA/app/AndariumStorageTests/AndariumStorageTests.apk":
                          synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: "
             "DATA/app/AndariumStorageTests"),
            ("debuggable-application-in-a-user-build-cause",
             {"entries": {"PRODUCT/app/Fixture/Fixture.apk": synthetic_apk(debuggable=True)}},
             "a user build must not carry a debuggable application: "
             "PRODUCT/app/Fixture/Fixture.apk"),
            ("debuggable-application-by-reference-cause",
             {"entries": {"PRODUCT/app/Fixture/Fixture.apk": synthetic_apk(debuggable="reference")}},
             "PRODUCT/app/Fixture/Fixture.apk has no readable manifest: "
             "android:debuggable is not a literal boolean"),
            ("application-that-starts-trade-in-mode-cause",
             {"entries": {"SYSTEM/priv-app/Fixture/Fixture.apk":
                          synthetic_apk(permissions=("android.permission.ENTER_TRADE_IN_MODE",))}},
             "SYSTEM/priv-app/Fixture/Fixture.apk requests android.permission.ENTER_TRADE_IN_MODE: "
             "trade-in mode would start on an unprovisioned first boot"),
            ("talkback-that-requests-the-network-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("android.permission.RECORD_AUDIO",
                                                     "android.permission.INTERNET",
                                                     "android.permission.ACCESS_NETWORK_STATE"))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests android.permission.ACCESS_NETWORK_STATE, "
             "android.permission.INTERNET: TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-a-google-service-permission-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=(
                              "com.google.android.providers.gsf.permission.READ_GSERVICES",
                              "com.google.android.apps.aicore.service.BIND_SERVICE"))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests "
             "com.google.android.apps.aicore.service.BIND_SERVICE, "
             "com.google.android.providers.gsf.permission.READ_GSERVICES: TalkBack and eSpeak NG"),
            ("espeak-that-requests-the-local-network-cause",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=("android.permission.ACCESS_LOCAL_NETWORK",))}},
             "PRODUCT/app/eSpeakNG/eSpeakNG.apk requests android.permission.ACCESS_LOCAL_NETWORK: "
             "TalkBack and eSpeak NG hold no network access and no Google service permission"),
            ("talkback-that-requests-a-play-store-permission-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("com.android.vending.BILLING",))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests com.android.vending.BILLING: "
             "TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-the-network-as-an-sdk-23-permission-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(sdk23=("android.permission.INTERNET",))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests android.permission.INTERNET: "
             "TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-the-old-guard-permission-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=(OLD_GUARD_PERMISSION,))}},
             f"PRODUCT/app/TalkBack/TalkBack.apk requests {OLD_GUARD_PERMISSION}: "
             "TalkBack and eSpeak NG hold no network access"),
            ("espeak-that-requests-the-old-guard-permission-cause",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=(OLD_GUARD_PERMISSION,))}},
             f"PRODUCT/app/eSpeakNG/eSpeakNG.apk requests {OLD_GUARD_PERMISSION}: "
             "TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-the-network-settings-permission-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("android.permission.NETWORK_SETTINGS",))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests android.permission.NETWORK_SETTINGS: "
             "TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-a-network-permission-no-name-rule-catches-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk": synthetic_apk(permissions=(
                 "android.permission.MANAGE_IPSEC_TUNNELS", "android.permission.ACCESS_LOWPAN_STATE",
                 "android.permission.PACKET_KEEPALIVE_OFFLOAD",
                 "android.permission.REQUEST_COMPANION_USE_DATA_IN_BACKGROUND"))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests android.permission.ACCESS_LOWPAN_STATE, "
             "android.permission.MANAGE_IPSEC_TUNNELS, android.permission.PACKET_KEEPALIVE_OFFLOAD, "
             "android.permission.REQUEST_COMPANION_USE_DATA_IN_BACKGROUND: "
             "TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-an-ordinary-permission-outside-its-list-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("android.permission.VIBRATE",
                                                     "android.permission.CAMERA"))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests android.permission.CAMERA: "
             "TalkBack and eSpeak NG hold no network access and no Google service permission, "
             "and request only the permissions the build is known to need"),
            ("espeak-that-requests-a-permission-at-all-cause",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=("android.permission.VIBRATE",))}},
             "PRODUCT/app/eSpeakNG/eSpeakNG.apk requests android.permission.VIBRATE: "
             "TalkBack and eSpeak NG hold no network access"),
            ("espeak-that-requests-the-guard-permission-of-talkbacks-package-cause",
             {"entries": {"PRODUCT/app/eSpeakNG/eSpeakNG.apk":
                          synthetic_apk(permissions=(TALKBACK_GUARD_PERMISSION,))}},
             f"PRODUCT/app/eSpeakNG/eSpeakNG.apk requests {TALKBACK_GUARD_PERMISSION}: "
             "TalkBack and eSpeak NG hold no network access"),
            ("talkback-that-requests-a-permission-by-resource-reference-cause",
             {"entries": {"PRODUCT/app/TalkBack/TalkBack.apk":
                          synthetic_apk(permissions=("@0x7f0a0001",))}},
             "PRODUCT/app/TalkBack/TalkBack.apk requests @resource 0x7f0a0001: "
             "TalkBack and eSpeak NG hold no network access"),
            ("device-diagnostics-in-the-image-cause",
             {"extra_entry": "SYSTEM/priv-app/DeviceDiagnostics/DeviceDiagnostics.apk"},
             "unneeded built-in application payload is forbidden: "
             "SYSTEM/priv-app/DeviceDiagnostics"),
            ("application-without-a-manifest-cause",
             {"entries": {"PRODUCT/app/Fixture/Fixture.apk": b"not an archive"}},
             "PRODUCT/app/Fixture/Fixture.apk has no readable manifest: "),
            ("pinner-overlay-target-sdk-29-cause", {"pinner_overlay_variant": "target-sdk-29"},
             "KernariumPinnerOverlay.apk targets SDK 29: .* 28 or lower"),
            ("pinner-overlay-target-sdk-current-cause", {"pinner_overlay_variant": "target-sdk-current"},
             "KernariumPinnerOverlay.apk targets SDK 37: .* 28 or lower"),
            ("missing-setup-wizard-overlay-cause", {"wizard_branding_variant": "missing-overlay"},
             "LineageSetupWizard__lineage_andarium__auto_generated_rro_product.apk is missing"),
            ("setup-wizard-names-lineageos-cause", {"wizard_branding_variant": "lineage-name"},
             "still names LineageOS or Lineage"),
            ("setup-wizard-names-lineage-in-utf16-cause", {"wizard_branding_variant": "lineage-name-utf16"},
             "still names LineageOS or Lineage"),
            ("setup-wizard-without-recovery-strings-cause", {"wizard_branding_variant": "no-recovery-strings"},
             "does not set the string 'Update Andarium Recovery'"),
            ("setup-wizard-without-andarium-features-cause", {"wizard_branding_variant": "lineage-features"},
             "does not set the string 'Andarium features'"),
            ("setup-wizard-without-logo-cause", {"wizard_branding_variant": "no-logo"},
             "LineageSetupWizard.* has no res/drawable/logo.xml that holds the product's logo"),
            ("setup-wizard-with-another-logo-cause", {"wizard_branding_variant": "stock-logo"},
             "LineageSetupWizard.* has no res/drawable/logo.xml that holds the product's logo"),
            ("setup-wizard-logo-lacking-a-path-cause", {"wizard_branding_variant": "logo-lacks-a-path"},
             "LineageSetupWizard.* has no res/drawable/logo.xml that holds the product's logo"),
            ("setup-wizard-without-animation-cause", {"wizard_branding_variant": "no-animation"},
             "LineageSetupWizard.* has no res/raw/lottie_welcome.json"),
            ("setup-wizard-with-another-animation-cause", {"wizard_branding_variant": "stock-animation"},
             "res/raw/lottie_welcome.json is not the product's animation"),
            ("setup-wizard-without-its-script-cause", {"wizard_branding_variant": "no-script"},
             "LineageSetupWizard.* has no res/raw/lineage_wizard_script.xml"),
            ("setup-wizard-without-its-user-script-cause", {"wizard_branding_variant": "no-user-script"},
             "LineageSetupWizard.* has no res/raw/lineage_wizard_script_user.xml"),
            ("setup-wizard-with-another-script-cause", {"wizard_branding_variant": "stock-script"},
             "res/raw/lineage_wizard_script.xml is not the product's script"),
            ("setup-wizard-with-another-user-script-cause", {"wizard_branding_variant": "stock-user-script"},
             "res/raw/lineage_wizard_script_user.xml is not the product's script"),
            ("setup-wizard-forces-gestural-navigation-cause", {"navigation_variant": "dex-forces-gestural"},
             "LineageSetupWizard.apk still forces gestural navigation: classes.dex contains "
             "enableGesturalNavigation"),
            ("missing-gestural-navigation-overlay-cause", {"navigation_variant": "no-gestural-overlay"},
             "overlay under PRODUCT/overlay/NavigationBarModeGestural/ is missing"),
            ("missing-three-button-navigation-overlay-cause", {"navigation_variant": "no-3button-overlay"},
             "overlay under PRODUCT/overlay/NavigationBarMode3Button/ is missing"),
            ("overlay-configuration-enables-gestural-cause", {"navigation_variant": "config-enables-gestural"},
             "PRODUCT/overlay/config/config.xml enables the navigation mode overlay "
             "com.android.internal.systemui.navbar.gestural"),
            ("overlay-configuration-enables-three-button-cause",
             {"navigation_variant": "config-enables-3button-undisabled"},
             "VENDOR/overlay/config/config.xml enables the navigation mode overlay "
             "com.android.internal.systemui.navbar.threebutton"),
            ("overlay-configuration-unreadable-cause", {"navigation_variant": "config-unreadable"},
             "SYSTEM_EXT/overlay/config/config.xml is not well-formed XML"),
            ("vendor-overlay-theme-property-cause", {"navigation_variant": "overlay-theme-property"},
             "PRODUCT/etc/build.prop sets ro.boot.vendor.overlay.theme=com.android.internal.systemui.navbar.gestural"),
            ("missing-launcher-overlay-cause", {"launcher_variant": "no-overlay"},
             "Launcher3QuickStep__lineage_andarium__auto_generated_rro_product.apk is missing"),
            ("launcher-overlay-without-a-grid-cause", {"launcher_variant": "no-6x5"},
             "has no res/xml/default_workspace_6x5.xml: a 6x5 guest would show the stock layout"),
            ("launcher-overlay-with-a-stock-layout-cause", {"launcher_variant": "stock-layout"},
             "res/xml/default_workspace_4x5.xml is a stock layout: it names google"),
            ("launcher-layout-without-files-cause", {"launcher_variant": "lacks-files"},
             "res/xml/default_workspace_4x5.xml does not name com.android.documentsui.LauncherActivity"),
            ("launcher-overlay-with-the-six-application-layout-cause", {"launcher_variant": "six-application-layout"},
             "res/xml/default_workspace_4x5.xml names com.android.calculator2.Calculator: the home screen "
             "holds only the dock"),
            ("missing-talkback-cause", {"accessibility_variant": "missing-talkback"},
             "TalkBack.apk is missing from the image: the image build supplies TalkBack"),
            ("missing-espeak-ng-cause", {"accessibility_variant": "missing-espeak"},
             "eSpeakNG.apk is missing from the image: the image build supplies TalkBack"),
            ("talkback-in-the-system-partition-cause", {"accessibility_variant": "talkback-in-system"},
             "TalkBack.apk is missing from the image: the image build supplies TalkBack"),
            ("missing-easter-egg-cause", {"easter_egg_variant": "missing"},
             "EasterEgg.apk is missing from the image: the Android version row opens the logo screen"),
            ("easter-egg-in-a-directory-of-another-name-cause", {"easter_egg_variant": "wrong-directory"},
             "EasterEgg.apk is missing from the image: the Android version row opens the logo screen"),
            ("launcher-layout-names-an-application-the-image-lacks-cause", {"launcher_variant": "no-documentsui"},
             "SYSTEM/priv-app/DocumentsUI/DocumentsUI.apk is missing: the launcher's default workspace"),
            ("ntp-server-left-cause", {"network_time_variant": "ntp-server"},
             "config_ntpServers names the NTP server ntp://time.android.com in .*framework-res"),
            ("ntp-server-left-in-a-qualified-definition-cause",
             {"network_time_variant": "ntp-server-in-a-qualified-table"},
             "config_ntpServers names the NTP server ntp://ntp.aliyun.com in .*framework-res"),
            ("ntp-server-left-in-capitals-cause", {"network_time_variant": "ntp-server-in-capitals"},
             "config_ntpServers names the NTP server NTP://time.example in .*framework-res"),
            ("ntp-server-list-empty-cause", {"network_time_variant": "ntp-empty"},
             "config_ntpServers is empty in .*framework-res.*NtpConfig throws"),
            ("ntp-server-list-missing-cause", {"network_time_variant": "ntp-missing"},
             "framework-res.* does not set config_ntpServers: the platform would query time.android.com"),
            ("network-time-origin-left-cause", {"network_time_variant": "origin-network"},
             "config_autoTimeSourcesPriority lists the network in .*framework-res"),
            ("time-origin-list-empty-cause", {"network_time_variant": "origin-empty"},
             "config_autoTimeSourcesPriority is empty in .*framework-res.*platform default holds the network"),
            ("time-origin-list-without-external-cause", {"network_time_variant": "origin-without-external"},
             "config_autoTimeSourcesPriority in .*framework-res.* lacks external"),
            ("time-origin-list-missing-cause", {"network_time_variant": "origin-missing"},
             "framework-res.* does not set config_autoTimeSourcesPriority"),
            ("flip-to-screen-off-enabled-cause", {"flip_variant": "enabled"},
             "config_flipToScreenOffEnabled is true in .*framework-res.*FaceDownDetector"),
            ("flip-to-screen-off-enabled-in-a-qualified-definition-cause",
             {"flip_variant": "enabled-in-a-qualified-table"},
             "config_flipToScreenOffEnabled is true in .*framework-res.*FaceDownDetector"),
            ("flip-to-screen-off-missing-cause", {"flip_variant": "missing"},
             "framework-res.* does not set config_flipToScreenOffEnabled: .*FaceDownDetector"),
            ("statistics-uploader-in-parts-manifest-cause", {"statistics_variant": "parts-manifest"},
             "LineageParts.apk still carries LineageOS's statistics uploader: AndroidManifest.xml "
             "contains lineagestats"),
            ("statistics-uploader-in-parts-dex-cause", {"statistics_variant": "parts-dex"},
             "LineageParts.apk still carries LineageOS's statistics uploader: classes.dex "
             "contains lineagestats"),
            ("statistics-endpoint-in-parts-resources-cause", {"statistics_variant": "parts-endpoint"},
             "LineageParts.apk still carries LineageOS's statistics uploader: resources.arsc "
             "contains stats.lineageos.org"),
            ("statistics-part-in-parts-catalogue-cause", {"statistics_variant": "parts-catalogue"},
             "LineageParts.apk still carries LineageOS's statistics uploader: "
             "res/xml/parts_catalog.xml contains lineagestats"),
            ("statistics-setting-in-setup-wizard-cause", {"statistics_variant": "wizard-setting"},
             "LineageSetupWizard.apk still carries LineageOS's statistics uploader: classes.dex "
             "contains stats_collection"),
            ("pinner-overlay-no-target-sdk-cause", {"pinner_overlay_variant": "no-target-sdk"},
             "KernariumPinnerOverlay.apk targets SDK None: .* 28 or lower"),
            ("pinner-overlay-named-target-sdk-cause", {"pinner_overlay_variant": "named-target-sdk"},
             "KernariumPinnerOverlay.apk targets SDK None: .* 28 or lower"),
            ("pinner-overlay-no-resource-cause", {"pinner_overlay_variant": "no-resource"},
             "KernariumPinnerOverlay.apk does not set config_pinnerHomePinBytes"),
            ("pinner-overlay-no-table-cause", {"pinner_overlay_variant": "no-table"},
             "KernariumPinnerOverlay.apk has no readable manifest and resource table"),
            ("pinner-overlay-not-compiled-cause", {"pinner_overlay_variant": "not-compiled"},
             "KernariumPinnerOverlay.apk has an unreadable manifest: not a compiled XML file"),
            ("missing-reclaim-script-cause", {"reclaim_variant": "missing-script"},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_reclaim_boot_apps.sh"),
            ("missing-reclaim-rc-cause", {"reclaim_variant": "missing-rc"},
             "required entries are missing: .*SYSTEM_EXT/etc/init/kernarium_reclaim_boot_apps.rc"),
            ("reclaim-missing-service-cause", {"reclaim_variant": "missing-service"},
             "does not declare the kernarium_reclaim_boot_apps service"),
            ("reclaim-not-oneshot-cause", {"reclaim_variant": "not-oneshot"},
             "does not declare kernarium_reclaim_boot_apps as oneshot"),
            ("reclaim-not-disabled-cause", {"reclaim_variant": "not-disabled"},
             "does not declare kernarium_reclaim_boot_apps as disabled"),
            ("reclaim-started-at-boot-cause", {"reclaim_variant": "started-at-boot"},
             "starts kernarium_reclaim_boot_apps under 'boot'"),
            ("reclaim-no-start-cause", {"reclaim_variant": "no-start"},
             "does not start kernarium_reclaim_boot_apps when boot completes"),
            ("reclaim-no-kill-cause", {"reclaim_variant": "no-kill"},
             "does not contain 'cmd activity kill --user 0 \"\\$pkg\"'"),
            ("reclaim-kill-all-cause", {"reclaim_variant": "kill-all"},
             "contains 'kill-all': it may only kill the cached processes"),
            ("reclaim-force-stop-cause", {"reclaim_variant": "force-stop"},
             "contains 'force-stop': it may only kill the cached processes"),
            ("reclaim-kill-9-cause", {"reclaim_variant": "kill-9"},
             "contains 'kill -9': it may only kill the cached processes"),
            ("reclaim-pkill-cause", {"reclaim_variant": "pkill"},
             "contains 'pkill': it may only kill the cached processes"),
            ("reclaim-kill-pid-cause", {"reclaim_variant": "kill-pid"},
             "contains 'kill': it may only kill the cached processes"),
            ("reclaim-am-command-cause", {"reclaim_variant": "am-command"},
             "contains 'am': it may only kill the cached processes"),
            ("reclaim-second-kill-cause", {"reclaim_variant": "second-kill"},
             "holds the reviewed kill 2 times, not once"),
            ("reclaim-second-list-cause", {"reclaim_variant": "second-list"},
             "assigns reclaim_list 2 times, not once"),
            ("reclaim-extra-package-cause", {"reclaim_variant": "extra-package"},
             "lists .*com.example.app.*not the reviewed packages"),
            ("reclaim-missing-package-cause", {"reclaim_variant": "missing-package"},
             "not the reviewed packages"),
            ("reclaim-missing-messaging-cause", {"reclaim_variant": "missing-messaging"},
             "not the reviewed packages"),
            ("reclaim-missing-telephony-provider-cause", {"reclaim_variant": "missing-telephony-provider"},
             "not the reviewed packages"),
            ("reclaim-missing-icons-cause", {"reclaim_variant": "missing-icons"},
             "not the reviewed packages"),
            ("reclaim-telephony-package-as-process-cause", {"reclaim_variant": "telephony-package-as-process"},
             "not the reviewed packages"),
            ("reclaim-missing-process-cause", {"reclaim_variant": "missing-process"},
             "not the reviewed packages"),
            ("reclaim-no-settle-cause", {"reclaim_variant": "no-settle"},
             "does not contain 'settle=120'"),
            ("reclaim-no-lru-cause", {"reclaim_variant": "no-lru"},
             "does not contain 'dumpsys activity lru'"),
            ("reclaim-no-list-cause", {"reclaim_variant": "no-list"},
             "has no reclaim_list block"),
            ("reclaim-missing-label-cause", {"reclaim_variant": "missing-label"},
             "system_ext file contexts do not label /system_ext/bin/kernarium_reclaim_boot_apps.sh"),
            ("reclaim-missing-transition-cause", {"reclaim_variant": "missing-transition"},
             "does not move init into the reclaim domain"),
            ("reclaim-missing-find-cause", {"reclaim_variant": "missing-find"},
             "does not allow kernarium_reclaim_boot_apps activity_service service_manager find"),
            ("reclaim-missing-transfer-cause", {"reclaim_variant": "missing-transfer"},
             "does not allow kernarium_reclaim_boot_apps system_server binder transfer"),
            ("reclaim-missing-call-cause", {"reclaim_variant": "missing-call"},
             "does not allow system_server kernarium_reclaim_boot_apps binder call"),
            ("reclaim-missing-fd-cause", {"reclaim_variant": "missing-fd"},
             "does not allow system_server kernarium_reclaim_boot_apps fd use"),
            ("reclaim-missing-fifo-cause", {"reclaim_variant": "missing-fifo"},
             "does not allow system_server kernarium_reclaim_boot_apps fifo_file write"),
            ("missing-time-zone-script-cause", {"time_zone_variant": "missing-script"},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_time_zone.sh"),
            ("missing-time-zone-rc-cause", {"time_zone_variant": "missing-rc"},
             "required entries are missing: .*SYSTEM_EXT/etc/init/kernarium_time_zone.rc"),
            ("time-zone-missing-service-cause", {"time_zone_variant": "missing-service"},
             "does not declare the kernarium_time_zone service"),
            ("time-zone-not-oneshot-cause", {"time_zone_variant": "not-oneshot"},
             "does not declare kernarium_time_zone as oneshot"),
            ("time-zone-not-disabled-cause", {"time_zone_variant": "not-disabled"},
             "does not declare kernarium_time_zone as disabled"),
            ("time-zone-started-at-boot-cause", {"time_zone_variant": "started-at-boot"},
             "starts kernarium_time_zone under 'boot'"),
            ("time-zone-no-start-cause", {"time_zone_variant": "no-start"},
             "does not start kernarium_time_zone when boot completes"),
            ("time-zone-no-property-cause", {"time_zone_variant": "no-property"},
             "does not contain 'ro.boot.kernarium.timezone'"),
            ("time-zone-no-auto-read-cause", {"time_zone_variant": "no-auto-read"},
             "does not contain 'cmd settings get global auto_time_zone'"),
            ("time-zone-no-set-cause", {"time_zone_variant": "no-set"},
             "does not contain 'cmd alarm set-timezone \"\\$zone\"'"),
            ("time-zone-no-marker-cause", {"time_zone_variant": "no-marker"},
             "does not contain 'setprop \"\\$marker\" \"\\$zone\"'"),
            ("time-zone-writes-setting-cause", {"time_zone_variant": "writes-setting"},
             "contains 'settings put': it may only read the user's automatic time zone setting"),
            ("time-zone-setprop-zone-cause", {"time_zone_variant": "setprop-zone"},
             "contains 'setprop persist.sys.timezone': it may only read"),
            ("time-zone-detector-cause", {"time_zone_variant": "detector"},
             "contains 'time_zone_detector': it may only read the user's automatic time zone setting"),
            ("time-zone-missing-label-cause", {"time_zone_variant": "missing-label"},
             "system_ext file contexts do not label /system_ext/bin/kernarium_time_zone.sh"),
            ("time-zone-missing-transition-cause", {"time_zone_variant": "missing-transition"},
             "does not move init into the time zone domain"),
            ("time-zone-missing-settings-find-cause", {"time_zone_variant": "missing-settings-find"},
             "does not allow kernarium_time_zone settings_service service_manager find"),
            ("time-zone-missing-alarm-find-cause", {"time_zone_variant": "missing-alarm-find"},
             "does not allow kernarium_time_zone alarm_service service_manager find"),
            ("time-zone-missing-transfer-cause", {"time_zone_variant": "missing-transfer"},
             "does not allow kernarium_time_zone system_server binder transfer"),
            ("time-zone-missing-call-cause", {"time_zone_variant": "missing-call"},
             "does not allow system_server kernarium_time_zone binder call"),
            ("time-zone-missing-fd-cause", {"time_zone_variant": "missing-fd"},
             "does not allow system_server kernarium_time_zone fd use"),
            ("time-zone-missing-fifo-cause", {"time_zone_variant": "missing-fifo"},
             "does not allow system_server kernarium_time_zone fifo_file write"),
            ("time-zone-sets-zone-property-cause", {"time_zone_variant": "sets-zone-property"},
             "lets the time zone domain set .*timezone_prop.*not only its own marker"),
            ("time-zone-missing-marker-rule-cause", {"time_zone_variant": "missing-marker-rule"},
             "does not let the time zone domain set persist.sys.kernarium.timezone_applied"),
            ("time-zone-marker-not-system-cause", {"time_zone_variant": "marker-not-system"},
             "does not define the system-owned kernarium_time_zone_prop"),
            ("time-zone-missing-marker-context-cause", {"time_zone_variant": "missing-marker-context"},
             "do not label persist.sys.kernarium.timezone_applied"),
            ("missing-userdata-quota-cause", {"missing_userdata_quota": True},
             "userdata entry lacks the fs_mgr quota flag"),
            ("wrong-apex-filesystem-cause", {"wrong_apex_filesystem": True},
             "APEX payload must use compressed erofs, not ext4: SYSTEM/apex/com.android.fixture.apex"),
            ("apex-lz4-payload-cause",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 algorithms=1 << 0)}},
             "APEX payload SYSTEM/apex/com.android.fixture.apex is not compressed with zstd alone: "
             "algorithms 0x1"),
            ("apex-small-cluster-payload-cause",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 features=0x1)}},
             "APEX payload SYSTEM/apex/com.android.fixture.apex has no physical cluster larger "
             "than a block"),
            ("apex-extended-payload-cause",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 features=0x3 | 0x20)}},
             "APEX payload SYSTEM/apex/com.android.fixture.apex uses an extended option"),
            ("apex-16k-block-payload-cause",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex(
                 block_bits=14)}},
             "APEX payload SYSTEM/apex/com.android.fixture.apex does not use 4 KiB blocks"),
            ("apex-unknown-payload-cause",
             {"entries": {"SYSTEM/apex/com.android.fixture.apex": synthetic_apex("unknown")}},
             "APEX payload must use compressed erofs: SYSTEM/apex/com.android.fixture.apex"),
            ("shim-lookalike-ext4-payload-cause",
             {"entries": {"SYSTEM/apex/com.android.apex.cts.shim2.apex": synthetic_apex("ext4")}},
             "APEX payload must use compressed erofs, not ext4: "
             "SYSTEM/apex/com.android.apex.cts.shim2.apex"),
            ("wrong-filesystem-cause", {"wrong_filesystem": True},
             "system_fs_type must select read-only erofs images"),
            ("wrong-erofs-compressor-cause",
             {"misc_info_edits": {"erofs_default_compressor": "lz4hc,9"}},
             re.escape(f"erofs_default_compressor must be {erofs_compressor}: 'lz4hc,9'")),
            ("wrong-erofs-level-cause",
             {"misc_info_edits": {"erofs_default_compressor": "zstd,19"}},
             re.escape(f"erofs_default_compressor must be {erofs_compressor}: 'zstd,19'")),
            ("wrong-erofs-cluster-cause",
             {"misc_info_edits": {"erofs_pcluster_size": "4096"}},
             re.escape(f"erofs_pcluster_size must be {erofs_pcluster_size}: '4096'")),
            ("missing-erofs-compressor-cause",
             {"misc_info_edits": {"erofs_default_compressor": None}},
             "erofs_default_compressor must be .*: None"),
            ("erofs-dedupe-cause", {"misc_info_edits": {"erofs_enable_dedupe": "true"}},
             "policy beyond the product's two parameters is set: erofs_enable_dedupe"),
            ("erofs-shared-blocks-cause",
             {"misc_info_edits": {"erofs_share_dup_blocks": "true"}},
             "policy beyond the product's two parameters is set: erofs_share_dup_blocks"),
            ("erofs-legacy-compression-cause",
             {"misc_info_edits": {"erofs_use_legacy_compression": "true"}},
             "policy beyond the product's two parameters is set: erofs_use_legacy_compression"),
            ("erofs-block-size-cause", {"misc_info_edits": {"erofs_blocksize": "16384"}},
             "policy beyond the product's two parameters is set: erofs_blocksize"),
            ("erofs-system-block-size-cause",
             {"misc_info_edits": {"system_erofs_blocksize": "16384"}},
             "policy beyond the product's two parameters is set: system_erofs_blocksize"),
            ("erofs-vendor-compressor-cause",
             {"misc_info_edits": {"vendor_erofs_compressor": "lz4hc,9"}},
             "policy beyond the product's two parameters is set: vendor_erofs_compressor"),
            ("erofs-system-ext-cluster-cause",
             {"misc_info_edits": {"system_ext_erofs_pcluster_size": "65536"}},
             "policy beyond the product's two parameters is set: system_ext_erofs_pcluster_size"),
            ("missing-erofs-hints-cause",
             {"entries": {"META/erofs_default_compress_hints.txt": None}},
             "erofs_default_compress_hints.txt is missing: the .apex files would be compressed"),
            ("wrong-erofs-hints-cause",
             {"entries": {"META/erofs_default_compress_hints.txt": b"0 .*\\.img$\n"}},
             "EROFS compress hints differ from the tracked product file"),
            ("ext4-system-image-cause",
             {"entries": {"IMAGES/system.img": bytes(4096)}},
             "IMAGES/system.img is not an EROFS image"),
            ("lz4-vendor-image-cause",
             {"entries": {"IMAGES/vendor.img": synthetic_erofs_image(algorithms=1 << 0)}},
             "IMAGES/vendor.img is not compressed with zstd alone: algorithms 0x1"),
            ("lzma-product-image-cause",
             {"entries": {"IMAGES/product.img": synthetic_erofs_image(
                 algorithms=1 << 1, sparse=True)}},
             "IMAGES/product.img is not compressed with zstd alone: algorithms 0x2"),
            ("two-algorithm-odm-image-cause",
             {"entries": {"IMAGES/odm.img": synthetic_erofs_image(
                 algorithms=(1 << 0) | (1 << 3))}},
             "IMAGES/odm.img is not compressed with zstd alone: algorithms 0x9"),
            ("small-cluster-system-ext-image-cause",
             {"entries": {"IMAGES/system_ext.img": synthetic_erofs_image(features=0x1)}},
             "IMAGES/system_ext.img has no physical cluster larger than a block"),
            ("tail-packing-system-image-cause",
             {"entries": {"IMAGES/system.img": synthetic_erofs_image(features=0x3 | 0x10)}},
             "IMAGES/system.img uses an extended option"),
            ("fragments-vendor-image-cause",
             {"entries": {"IMAGES/vendor.img": synthetic_erofs_image(features=0x3 | 0x20)}},
             "IMAGES/vendor.img uses an extended option"),
            ("16k-block-system-image-cause",
             {"entries": {"IMAGES/system.img": synthetic_erofs_image(block_bits=14)}},
             "IMAGES/system.img does not use 4 KiB blocks"),
            ("missing-microphone-feature-cause", {"missing_microphone_feature": True},
             "feature is not declared: android.hardware.microphone"),
            ("missing-companion-feature-cause", {"missing_companion_feature": True},
             "feature is not declared: android.software.companion_device_setup"),
            ("missing-midi-feature-cause", {"missing_midi_feature": True},
             "feature is not declared: android.software.midi"),
            ("camera-feature-cause", {"unsupported_feature": True},
             "a camera feature is declared outside the camera layouts.*android.hardware.camera"),
            ("missing-mic-policy-cause", {"missing_mic_policy": True},
             "ordinary mixer input"),
            ("extra-input-device-cause", {"extra_input_device": True},
             "only its speaker and built-in microphone"),
            ("no-64-bit-compiler-cause", {"dex2oat64": False}, "dalvik.vm.dex2oat64.enabled"),
            ("missing-32-bit-runtime-cause",
             {"entries": {"SYSTEM/bin/app_process32": None}}, "required entries are missing"),
            ("missing-sgdisk-cause",
             {"entries": {"SYSTEM/bin/sgdisk": None}}, "SYSTEM/bin/sgdisk"),
            ("32-bit-gallium-dependency-cause",
             {"missing_arm_gallium_dependency": True}, "does not link the gallium driver"),
            ("32-bit-egl-is-aarch64-cause",
             {"entries": {"VENDOR/lib/egl/libEGL_kernarium.so": synthetic_aarch64_elf()}},
             "64-bit ELF in a 32-bit library directory"),
            ("64-bit-egl-is-arm-cause",
             {"entries": {"VENDOR/lib64/egl/libEGL_kernarium.so": synthetic_arm_elf()}},
             "32-bit ELF outside"),
            ("missing-webview-32-bit-odex-cause",
             {"entries": {"PRODUCT/app/webview/oat/arm/webview.odex": None}},
             "32-bit application dexpreopt artifact is missing"),
            ("32-bit-odex-without-arm64-cause",
             {"entries": {"PRODUCT/app/Orphan/oat/arm/Orphan.odex": b"odex",
                          "PRODUCT/app/Orphan/oat/arm/Orphan.vdex": b"vdex"}},
             "has no ARM64 one"),
            ("unknown-isa-cause",
             {"entries": {"PRODUCT/app/LatinIME/oat/x86_64/LatinIME.odex": b"odex"}},
             "not for an ISA of this product"),
            ("32-bit-elf-in-lib64-cause",
             {"entries": {"VENDOR/lib64/libfixture.so": synthetic_arm_elf()}},
             "32-bit ELF outside"),
            ("64-bit-elf-in-lib-cause",
             {"entries": {"SYSTEM/lib/libfixture.so": synthetic_aarch64_elf()}},
             "64-bit ELF in a 32-bit library directory"),
            ("32-bit-daemon-cause",
             {"entries": {"SYSTEM/bin/mediaserver": synthetic_arm_elf()}},
             "32-bit ELF outside"),
            ("32-bit-alignment-cause", {"arm_alignment": 2048}, "below 4 KiB"),
            ("32-bit-hard-float-cause", {"arm_flags": 0x05000400}, "hard-float"),
            ("32-bit-eabi-version", {"arm_flags": 0x04000000}, "EABI version 5"),
            ("32-bit-machine", {"arm_edit": ("<H", 18, 62)}, "not Arm"),
            ("32-bit-big-endian", {"arm_edit": ("<B", 5, 2)}, "little-endian ELF32 or ELF64"),
            ("32-bit-program-header-size", {"arm_edit": ("<H", 42, 16)}, "program.header"),
            # The Venus ICD built without -D_FILE_OFFSET_BITS=64 imports the
            # 32-bit-offset mmap and lseek, and fails to map a DRM offset.
            ("32-bit-vulkan-icd-plain-mmap",
             {"entries": {"VENDOR/lib/hw/vulkan.virtio.so": synthetic_arm_elf(
                 imports=("open", "mmap", "lseek64", "munmap"))}},
             r"32-bit VENDOR/lib/hw/vulkan.virtio.so imports mmap instead of mmap64"),
            ("32-bit-gallium-plain-lseek",
             {"entries": {"VENDOR/lib/venus_gallium_dri.so": synthetic_arm_elf(
                 imports=("open", "mmap64", "lseek"))}},
             r"venus_gallium_dri.so imports lseek instead of lseek64"),
            ("32-bit-gralloc-both-plain",
             {"entries": {"VENDOR/lib/hw/gralloc.minigbm.so": synthetic_arm_elf(
                 imports=("mmap", "lseek"))}},
             r"gralloc.minigbm.so imports lseek, mmap instead of lseek64, mmap64"),
            ("32-bit-stack-without-symbols",
             {"entries": {"VENDOR/lib/egl/libEGL_kernarium.so": synthetic_arm_elf(
                 needed=("venus_gallium_dri.so",))}},
             r"no readable section headers"),
            ("missing-video-encoder-service-cause",
             {"entries": {f"VENDOR/bin/hw/{video_service}": None}},
             "required entries are missing"),
            ("video-codec-list-lacks-hevc-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 encoders={"c2.v4l2.avc.encoder": "video/avc"})}},
             "exactly the hardware encoders"),
            ("video-software-decoder-listed-not-updated-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoders=("c2.android.avc.decoder",))}},
             "outside the hardware codecs and the software updates"),
            ("video-encoder-without-performance-point-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 performance_points=False)}},
             "publishes no performance-point-"),
            ("video-encoder-without-measured-frame-rate-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 measured_frame_rates=False)}},
             "publishes no measured-frame-rate-"),
            ("video-performance-point-above-capacity-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 performance_point="3840x2160 30-60")}},
             "above the pixel rate capacity 108000000"),
            ("video-performance-point-malformed-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 performance_point="3840x2160 fast")}},
             "publishes a malformed performance-point-3840x2160"),
            ("video-encoder-below-the-size-floor-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 encoder_minimum_size="16x16")}},
             r"c2\.v4l2\.\w+\.encoder must start at 64x64 pixels"),
            ("video-encoder-below-the-bitrate-floor-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 encoder_minimum_bitrate="1")}},
             r"c2\.v4l2\.\w+\.encoder must start at 20000 bit/s"),
            ("video-decoder-without-performance-point-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_performance_points=False)}},
             r"c2\.v4l2\.\w+\.decoder publishes no performance-point-"),
            ("video-decoder-without-measured-frame-rate-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_measured_frame_rates=False)}},
             r"c2\.v4l2\.\w+\.decoder publishes no measured-frame-rate-"),
            ("video-decoder-point-above-capacity-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_performance_point="3840x2160 60-120")}},
             "above the decode pixel rate capacity 456192000"),
            ("video-decoder-points-far-below-capacity-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_performance_point="1280x720 60-90")}},
             "below 60% of the capacity 456192000"),
            ("video-pixel-rate-property-cause",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.video_pixel_rate": "1000"}},
             "lacks ro.vendor.v4l2_codec2.video_pixel_rate=108000000"),
            ("video-decode-pixel-rate-property-cause",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.video_decode_pixel_rate": "1000"}},
             "lacks ro.vendor.v4l2_codec2.video_decode_pixel_rate=456192000"),
            ("video-codec-list-lacks-hevc-decoder-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 hardware_decoders={"c2.v4l2.avc.decoder": "video/avc"})}},
             "must name the hardware decoders"),
            ("video-codec-list-lacks-vp9-decoder-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 hardware_decoders={name: kind for name, kind in tool.VIDEO_DECODER_CODECS.items()
                                    if name != "c2.v4l2.vp9.decoder"})}},
             "must name the hardware decoders"),
            ("video-unapproved-hardware-decoder-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 hardware_decoders={**tool.VIDEO_DECODER_CODECS,
                                    "c2.v4l2.vp8.decoder": "video/x-vnd.on2.vp8"})}},
             "must name the hardware decoders"),
            ("video-decoder-without-adaptive-playback-cause",
             {"entries": {"VENDOR/etc/media_codecs_c2.xml": video_codec_list(
                 decoder_features=("dynamic-color-aspects",))}},
             "does not publish adaptive-playback"),
            ("video-unapproved-decoder-property-cause",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.decoder.supported.av1": "true"}},
             "outside the approved set"),
            ("missing-video-vp9-decoder-property-cause",
             {"video_properties": {
                 name: value for name, value in good_video_properties.items()
                 if not name.endswith("decoder.supported.vp9")}},
             "lacks ro.vendor.v4l2_codec2.decoder.supported.vp9"),
            ("missing-video-decoder-property-cause",
             {"video_properties": {
                 name: value for name, value in good_video_properties.items()
                 if not name.endswith("decoder.supported.h264")}},
             "lacks ro.vendor.v4l2_codec2.decoder.supported.h264"),
            ("video-decode-concurrency-property-cause",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.decode_concurrent_instances": "1"}},
             "lacks ro.vendor.v4l2_codec2.decode_concurrent_instances=4"),
            ("missing-video-hevc-property-cause",
             {"video_properties": {
                 name: value for name, value in good_video_properties.items()
                 if not name.endswith("supported.hevc")}},
             "lacks ro.vendor.v4l2_codec2.encoder.supported.hevc"),
            ("video-concurrency-property-cause",
             {"video_properties": {
                 **good_video_properties,
                 "ro.vendor.v4l2_codec2.encode_concurrent_instances": "1"}},
             "lacks ro.vendor.v4l2_codec2.encode_concurrent_instances=8"),
            ("missing-video-ueventd-cause",
             {"missing_video_ueventd": True}, "video nodes' group"),
            ("video-service-without-camera-group-cause",
             {"entries": {f"VENDOR/etc/init/{video_service}.rc": good_video_rc.replace(
                 " camera", "").encode()}},
             "does not join the group"),
            ("video-seccomp-lacks-eventfd-cause",
             {"entries": {
                 f"VENDOR/etc/seccomp_policy/{video_seccomp_extended}":
                 b"read: 1\n# eventfd2: 1\n"}},
             "does not allow eventfd2"),
            ("video-seccomp-lacks-crash-handler-call-cause",
             {"entries": {
                 f"VENDOR/etc/seccomp_policy/{video_seccomp_extended}":
                 good_video_seccomp.replace("uname: 1\n", "").encode()}},
             "does not allow uname"),
            ("video-service-label-cause",
             {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                          good_video_contexts.replace("mediacodec_exec", "vendor_file").encode()}},
             "not labeled mediacodec_exec"),
            ("missing-vibrator-service", {"entries": {
                "VENDOR/bin/hw/android.hardware.vibrator-service.andarium": None}},
             "required entries are missing"),
            ("missing-haptics-manifest", {"entries": {"ODM/etc/vintf/manifest_haptics.xml": None}},
             "required entries are missing"),
            ("always-declared-vibrator", {"entries": {
                "VENDOR/etc/vintf/manifest.xml": good_vibrator_manifest.encode()}},
             "declares a vibrator HAL"),
            ("vibrator-in-a-vendor-fragment", {"entries": {
                "VENDOR/etc/vintf/manifest/android.hardware.vibrator.xml": good_vibrator_manifest.encode()}},
             "declares a vibrator HAL"),
            ("vibrator-in-the-plain-odm-manifest", {"entries": {
                "ODM/etc/vintf/manifest.xml": good_vibrator_manifest.encode()}},
             "declares a vibrator HAL"),
            ("vibrator-manager-in-the-haptics-manifest", {"entries": {
                "ODM/etc/vintf/manifest_haptics.xml": good_vibrator_manifest.replace(
                    "IVibrator/default", "IVibratorManager/default").encode()}},
             "must declare exactly the AIDL IVibrator/default at version 4"),
            ("vibrator-version-3-in-the-haptics-manifest", {"entries": {
                "ODM/etc/vintf/manifest_haptics.xml": good_vibrator_manifest.replace(
                    "<version>4", "<version>3").encode()}},
             "must declare exactly the AIDL IVibrator/default at version 4"),
            ("second-odm-sku-manifest", {"entries": {
                "ODM/etc/vintf/manifest_other.xml": b'<manifest version="1.0" type="device"/>'}},
             "ODM SKU manifests must be exactly the haptics one"),
            ("odm-skus-in-the-build", {"misc_info_edits": {"vintf_odm_manifest_skus": "haptics other"}},
             "must name only the haptics ODM manifest SKU"),
            ("no-odm-sku-in-the-build", {"misc_info_edits": {"vintf_odm_manifest_skus": None}},
             "must name only the haptics ODM manifest SKU"),
            ("cuttlefish-vibrator-apex", {"entries": {
                "VENDOR/apex/com.android.hardware.vibrator.apex": b"apex"}},
             "nonstatic image or metadata is forbidden"),
            ("vibrator-service-not-disabled", {"entries": {
                vibrator_rc_path: good_vibrator_rc.replace("    disabled\n", "").encode()}},
             "must carry 'disabled'"),
            ("vibrator-service-without-interface", {"entries": {
                vibrator_rc_path: good_vibrator_rc.replace(
                    "    interface aidl android.hardware.vibrator.IVibrator/default\n", "").encode()}},
             "must carry 'interface aidl"),
            ("vibrator-service-for-any-sku", {"entries": {
                vibrator_rc_path: good_vibrator_rc.replace(
                    "on boot && property:ro.boot.product.hardware.sku=haptics", "on boot").encode()}},
             "must start on 'on boot && property:ro.boot.product.hardware.sku=haptics'"),
            ("vibrator-service-at-early-hal", {"entries": {
                vibrator_rc_path: (good_vibrator_rc + "\non early-hal\n    start vendor.vibrator-andarium\n").encode()}},
             "may have no trigger but the haptics SKU's boot trigger"),
            ("vibrator-service-wrong-binary", {"entries": {
                vibrator_rc_path: good_vibrator_rc.replace(
                    "android.hardware.vibrator-service.andarium", "android.hardware.vibrator-service.example").encode()}},
             "must define one service that runs the Andarium vibrator HAL"),
            ("vibrator-label", {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                good_video_contexts.replace("hal_vibrator_default_exec", "vendor_file").encode()}},
             "hal_vibrator_default_exec label"),
            ("missing-gnss-service", {"entries": {
                "VENDOR/bin/hw/android.hardware.gnss-service.andarium": None}},
             "required entries are missing"),
            ("missing-gnss-rc", {"entries": {gnss_rc_path: None}}, "required entries are missing"),
            ("missing-gnss-fragment", {"entries": {gnss_fragment_path: None}},
             "required entries are missing"),
            ("gnss-in-the-vendor-manifest", {"entries": {
                "VENDOR/etc/vintf/manifest.xml": good_gnss_manifest.encode()}},
             "declares a GNSS HAL"),
            ("gnss-in-a-second-fragment", {"entries": {
                "VENDOR/etc/vintf/manifest/android.hardware.gnss.xml": good_gnss_manifest.encode()}},
             "declares a GNSS HAL"),
            ("gnss-in-the-plain-odm-manifest", {"entries": {
                "ODM/etc/vintf/manifest.xml": good_gnss_manifest.encode()}},
             "declares a GNSS HAL"),
            ("gnss-fragment-version-6", {"entries": {
                gnss_fragment_path: good_gnss_manifest.replace("<version>7", "<version>6").encode()}},
             "must declare exactly the AIDL IGnss/default at version 7"),
            ("gnss-fragment-with-a-second-hal", {"entries": {
                gnss_fragment_path: good_gnss_manifest.replace(
                    "</manifest>", '<hal format="aidl"><name>android.hardware.light</name>'
                    "<version>2</version><fqname>ILights/default</fqname></hal></manifest>").encode()}},
             "must declare exactly the AIDL IGnss/default at version 7"),
            ("gnss-fragment-other-instance", {"entries": {
                gnss_fragment_path: good_gnss_manifest.replace("IGnss/default", "IGnss/other").encode()}},
             "must declare exactly the AIDL IGnss/default at version 7"),
            ("cuttlefish-gnss-apex", {"entries": {
                "VENDOR/apex/com.android.hardware.gnss.apex": b"apex"}},
             "nonstatic image or metadata is forbidden"),
            ("gnss-service-disabled", {"entries": {
                gnss_rc_path: (good_gnss_rc + "    disabled\n").encode()}},
             "neither disabled nor oneshot"),
            ("gnss-service-oneshot", {"entries": {
                gnss_rc_path: (good_gnss_rc + "    oneshot\n").encode()}},
             "neither disabled nor oneshot"),
            ("gnss-service-triggered", {"entries": {
                gnss_rc_path: (good_gnss_rc + "\non boot\n    start vendor.gnss-andarium\n").encode()}},
             "may have no trigger"),
            ("gnss-service-without-interface", {"entries": {
                gnss_rc_path: good_gnss_rc.replace(
                    "    interface aidl android.hardware.gnss.IGnss/default\n", "").encode()}},
             "must carry 'interface aidl"),
            ("gnss-service-wrong-user", {"entries": {
                gnss_rc_path: good_gnss_rc.replace("user gps", "user root").encode()}},
             "must carry 'user gps'"),
            ("gnss-service-wrong-group", {"entries": {
                gnss_rc_path: good_gnss_rc.replace("group gps", "group system").encode()}},
             "must carry 'group gps'"),
            ("gnss-service-wrong-binary", {"entries": {
                gnss_rc_path: good_gnss_rc.replace(
                    "android.hardware.gnss-service.andarium", "android.hardware.gnss-service.example").encode()}},
             "must define one service that runs the Andarium GNSS HAL"),
            ("gnss-label", {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                good_video_contexts.replace("hal_gnss_default_exec", "vendor_file").encode()}},
             "hal_gnss_default_exec label"),
            ("gnss-supl-server", {"entries": {
                "SYSTEM/etc/gps_debug.conf": b"SUPL_HOST=supl.example\nSUPL_PORT=7275\n"}},
             "configures an assistance server"),
            ("gnss-ntp-server", {"entries": {
                "SYSTEM/etc/gps_debug.conf": b"# comment\nNTP_SERVER=time.example\n"}},
             "configures an assistance server"),
            ("missing-thermal-service", {"entries": {
                "VENDOR/bin/hw/android.hardware.thermal-service.andarium": None}},
             "required entries are missing"),
            ("missing-thermal-rc", {"entries": {thermal_rc_path: None}}, "required entries are missing"),
            ("missing-thermal-fragment", {"entries": {thermal_fragment_path: None}},
             "required entries are missing"),
            ("missing-display-config", {"entries": {display_config_path: None}},
             "required entries are missing"),
            ("thermal-in-the-vendor-manifest", {"entries": {
                "VENDOR/etc/vintf/manifest.xml": good_thermal_manifest.encode()}},
             "declares android.hardware.thermal"),
            ("thermal-in-a-second-fragment", {"entries": {
                "VENDOR/etc/vintf/manifest/android.hardware.thermal.xml": good_thermal_manifest.encode()}},
             "declares android.hardware.thermal"),
            ("thermal-in-the-plain-odm-manifest", {"entries": {
                "ODM/etc/vintf/manifest.xml": good_thermal_manifest.encode()}},
             "declares android.hardware.thermal"),
            ("cuttlefish-thermal-apex", {"entries": {
                "VENDOR/apex/com.android.hardware.thermal.apex": b"apex"}},
             "nonstatic image or metadata is forbidden"),
            ("thermal-fragment-version-2", {"entries": {
                thermal_fragment_path: good_thermal_manifest.replace("<version>3", "<version>2").encode()}},
             "must declare exactly the AIDL IThermal/default at version 3"),
            ("thermal-fragment-with-a-second-hal", {"entries": {
                thermal_fragment_path: good_thermal_manifest.replace(
                    "</manifest>", '<hal format="aidl"><name>android.hardware.light</name>'
                    "<version>2</version><fqname>ILights/default</fqname></hal></manifest>").encode()}},
             "must declare exactly the AIDL IThermal/default at version 3"),
            ("thermal-fragment-other-instance", {"entries": {
                thermal_fragment_path: good_thermal_manifest.replace("IThermal/default", "IThermal/other").encode()}},
             "must declare exactly the AIDL IThermal/default at version 3"),
            ("thermal-service-disabled", {"entries": {
                thermal_rc_path: (good_thermal_rc + "    disabled\n").encode()}},
             "neither disabled nor oneshot"),
            ("thermal-service-oneshot", {"entries": {
                thermal_rc_path: (good_thermal_rc + "    oneshot\n").encode()}},
             "neither disabled nor oneshot"),
            ("thermal-service-triggered", {"entries": {
                thermal_rc_path: (good_thermal_rc + "\non boot\n    start vendor.thermal-andarium\n").encode()}},
             "may have no trigger"),
            ("thermal-service-without-interface", {"entries": {
                thermal_rc_path: good_thermal_rc.replace(
                    "    interface aidl android.hardware.thermal.IThermal/default\n", "").encode()}},
             "must carry 'interface aidl"),
            ("thermal-service-wrong-user", {"entries": {
                thermal_rc_path: good_thermal_rc.replace("user nobody", "user root").encode()}},
             "must carry 'user nobody'"),
            ("thermal-service-wrong-group", {"entries": {
                thermal_rc_path: good_thermal_rc.replace("group system", "group root").encode()}},
             "must carry 'group system'"),
            ("thermal-service-wrong-binary", {"entries": {
                thermal_rc_path: good_thermal_rc.replace(
                    "android.hardware.thermal-service.andarium", "android.hardware.thermal-service.example").encode()}},
             "must define one service that runs the Andarium thermal HAL"),
            ("thermal-label", {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                good_video_contexts.replace("hal_thermal_default_exec", "vendor_file").encode()}},
             "hal_thermal_default_exec label"),
            ("display-config-in-the-product-partition", {"entries": {
                "PRODUCT/etc/displayconfig/default.xml": good_display_config.encode()}},
             "would hide VENDOR/etc/displayconfig/default.xml"),
            ("display-config-malformed", {"entries": {display_config_path: b"<displayConfiguration>"}},
             "cannot parse VENDOR/etc/displayconfig/default.xml"),
            ("display-config-with-another-setting", {"entries": {
                display_config_path: good_display_config.replace(
                    "<thermalThrottling>", "<name>display</name><thermalThrottling>").encode()}},
             "with thermalThrottling and nothing else"),
            ("display-config-with-a-brightness-map", {"entries": {
                display_config_path: good_display_config.replace(
                    "</thermalThrottling>", "<brightnessThrottlingMap/></thermalThrottling>").encode()}},
             "must hold the one default refreshRateThrottlingMap"),
            ("display-config-with-a-named-map", {"entries": {
                display_config_path: good_display_config.replace(
                    "<refreshRateThrottlingMap>", '<refreshRateThrottlingMap id="other">').encode()}},
             "must hold the one default refreshRateThrottlingMap"),
            ("display-config-with-a-third-point", {"entries": {
                display_config_path: good_display_config.replace(
                    "</refreshRateThrottlingMap>",
                    "<refreshRateThrottlingPoint><thermalStatus>critical</thermalStatus>"
                    "<refreshRateRange><minimum>0</minimum><maximum>20</maximum></refreshRateRange>"
                    "</refreshRateThrottlingPoint></refreshRateThrottlingMap>").encode()}},
             "must hold the 2 refreshRateThrottlingPoints"),
            ("display-config-without-the-moderate-point", {"entries": {
                display_config_path: good_display_config.replace("moderate", "severe").encode()}},
             "must hold the moderate point with the refresh rate range 0 to 60 Hz"),
            ("display-config-critical-point", {"entries": {
                display_config_path: good_display_config.replace("severe", "critical").encode()}},
             "must hold the moderate point with the refresh rate range 0 to 60 Hz"),
            ("display-config-severe-at-60-hz", {"entries": {
                display_config_path: good_display_config.replace("<maximum>30", "<maximum>60").encode()}},
             "must hold the moderate point with the refresh rate range 0 to 60 Hz"),
            ("display-config-moderate-at-30-hz", {"entries": {
                display_config_path: good_display_config.replace("<maximum>60", "<maximum>30").encode()}},
             "must hold the moderate point with the refresh rate range 0 to 60 Hz"),
            ("display-config-minimum-30", {"entries": {
                display_config_path: good_display_config.replace("<minimum>0", "<minimum>30").encode()}},
             "must hold the moderate point with the refresh rate range 0 to 60 Hz"),
            # The first maximum is the right one and a second follows it: the point has exactly one
            # minimum and one maximum, not whatever else the range element carries.
            ("display-config-second-maximum", {"entries": {
                display_config_path: good_display_config.replace(
                    "<maximum>60</maximum>", "<maximum>60</maximum><maximum>90</maximum>").encode()}},
             "holds a refreshRateRange that is not one minimum and one maximum"),
            ("display-config-second-minimum", {"entries": {
                display_config_path: good_display_config.replace(
                    "<minimum>0</minimum>", "<minimum>0</minimum><minimum>15</minimum>", 1).encode()}},
             "holds a refreshRateRange that is not one minimum and one maximum"),
            ("display-config-range-without-a-maximum", {"entries": {
                display_config_path: good_display_config.replace(
                    "<maximum>60</maximum>", "", 1).encode()}},
             "holds a refreshRateRange that is not one minimum and one maximum"),
            ("missing-gamepad-binary", {"entries": {gamepad_binary_path: None}}, "required entries are missing"),
            ("missing-gamepad-rc", {"entries": {gamepad_rc_path: None}}, "required entries are missing"),
            ("gamepad-service-disabled", {"entries": {
                gamepad_rc_path: (good_gamepad_rc + "    disabled\n").encode()}},
             "neither disabled nor oneshot"),
            ("gamepad-service-oneshot", {"entries": {
                gamepad_rc_path: (good_gamepad_rc + "    oneshot\n").encode()}},
             "neither disabled nor oneshot"),
            ("gamepad-service-triggered", {"entries": {
                gamepad_rc_path: (good_gamepad_rc + "\non boot\n    start kernarium_gamepad\n").encode()}},
             "may have no trigger"),
            ("gamepad-service-with-a-seclabel", {"entries": {
                gamepad_rc_path: (good_gamepad_rc + "    seclabel u:r:init:s0\n").encode()}},
             "takes its domain from its file label"),
            ("gamepad-service-with-a-capability", {"entries": {
                gamepad_rc_path: (good_gamepad_rc + "    capabilities SYS_ADMIN\n").encode()}},
             "takes its domain from its file label"),
            ("gamepad-service-wrong-user", {"entries": {
                gamepad_rc_path: good_gamepad_rc.replace("user nobody", "user root").encode()}},
             "must carry 'user nobody'"),
            ("gamepad-service-wrong-group", {"entries": {
                gamepad_rc_path: good_gamepad_rc.replace("group uhid", "group nobody").encode()}},
             "must carry 'group uhid'"),
            ("gamepad-service-wrong-class", {"entries": {
                gamepad_rc_path: good_gamepad_rc.replace("class main", "class hal").encode()}},
             "must carry 'class main'"),
            ("gamepad-service-wrong-binary", {"entries": {
                gamepad_rc_path: good_gamepad_rc.replace(
                    "/vendor/bin/kernarium_gamepad", "/vendor/bin/other").encode()}},
             "must define the one service kernarium_gamepad"),
            ("gamepad-service-twice", {"entries": {
                gamepad_rc_path: (good_gamepad_rc + "service second /vendor/bin/kernarium_gamepad\n").encode()}},
             "must define the one service kernarium_gamepad"),
            ("gamepad-label", {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                good_video_contexts.replace(good_gamepad_label, "").encode()}},
             "do not label /vendor/bin/kernarium_gamepad"),
            ("gamepad-label-of-another-domain", {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                good_video_contexts.replace("kernarium_gamepad_exec", "vendor_file").encode()}},
             "do not label /vendor/bin/kernarium_gamepad"),
            ("gamepad-policy-transition", {"gamepad_policy_variant": "missing-transition"},
             "does not move init into the gamepad relay domain"),
            ("gamepad-policy-without-uhid", {"gamepad_policy_variant": "no-uhid"},
             "does not let the gamepad relay open, read and write the uhid device"),
            ("gamepad-policy-read-only-uhid", {"gamepad_policy_variant": "uhid-read-only"},
             "does not let the gamepad relay open, read and write the uhid device"),
            ("gamepad-policy-without-vsock", {"gamepad_policy_variant": "no-vsock"},
             "does not give the gamepad relay vsock sockets to listen on"),
            ("gamepad-policy-vsock-without-listen", {"gamepad_policy_variant": "vsock-no-listen"},
             "does not give the gamepad relay vsock sockets to listen on"),
            ("gamepad-policy-without-the-vsock-attribute", {"gamepad_policy_variant": "no-attribute"},
             "does not classify the gamepad relay as an unconstrained vsock user"),
            # The text message service: only the host can cause a delivery. Each row is one defect
            # of the policy that the baseline fixture does not have.
            ("sms-listener-through-an-attribute", {"sms_policy_variant": "listener-through-attribute"},
             "vsock socket are not the reviewed ones \\(unreviewed: \\['untrusted_app"),
            ("sms-vsock-creator-by-name", {"sms_policy_variant": "creator-by-name"},
             "unreviewed: \\['hal_audio_default'\\]"),
            ("sms-holder-without-its-vsock-rule", {"sms_policy_variant": "listener-without-the-holder"},
             "reviewed and missing: \\['kernarium_sms_listener'\\]"),
            ("sms-sockets-through-domain", {"sms_policy_variant": "sockets-through-domain"},
             "hold \\['read', 'write'\\] on a vsock socket of kernarium_sms"),
            # The platform's grant of getattr to dumpstate over domain is accepted by the baseline; each of
            # these differs from it by one thing and is refused.
            ("sms-dumpstate-reads", {"sms_policy_variant": "dumpstate-reads"},
             "\\['dumpstate'\\] hold \\['getattr', 'read'\\] on a vsock socket of kernarium_sms"),
            ("sms-another-domain-getattr", {"sms_policy_variant": "another-domain-getattr"},
             "\\['system_server'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms"),
            ("sms-dumpstate-getattr-over-a-smaller-group",
             {"sms_policy_variant": "dumpstate-getattr-over-a-smaller-group"},
             "\\['dumpstate'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms"),
            # A type or attribute whose name differs from dumpstate or domain by a platform version suffix is
            # another type: the rule must be the platform's own, token for token.
            ("sms-dumpstate-lookalike-source", {"sms_policy_variant": "dumpstate-lookalike-source"},
             "\\['dumpstate'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms"),
            ("sms-dumpstate-lookalike-target", {"sms_policy_variant": "dumpstate-lookalike-target"},
             "\\['dumpstate'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms"),
            ("sms-dumpstate-getattr-of-ours", {"sms_policy_variant": "dumpstate-getattr-of-ours"},
             "\\['dumpstate'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms"),
            ("sms-dumpstate-getattr-repeated-outside-the-platform",
             {"sms_policy_variant": "dumpstate-getattr-repeated-outside-the-platform"},
             "\\['dumpstate'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms"),
            # What the service inherits: the baseline has the platform's rules over groups of the application
            # domains, which hold all four ordinary ones. A group without one of them, rules that split
            # the four, the service alone, and the same rule in the system_ext policy are the service's own.
            ("sms-find-service-only-in-the-platform-policy", {"sms_policy_variant": "inherited-find-service-only"},
             "may find \\['credstore_service'\\]"),
            ("sms-find-split-over-two-rules", {"sms_policy_variant": "inherited-find-split-over-two-rules"},
             "may find \\['credstore_service'\\]"),
            # A type that differs from one of the four by a platform version suffix is another type.
            ("sms-find-group-with-a-lookalike-of-priv-app", {"sms_policy_variant": "inherited-find-with-a-lookalike"},
             "may find \\['window_service'\\]"),
            ("sms-find-group-without-untrusted-app", {"sms_policy_variant": "inherited-find-without-untrusted-app"},
             "may find \\['keystore_service'\\]"),
            ("sms-find-group-without-platform-app", {"sms_policy_variant": "inherited-find-without-platform-app"},
             "may find \\['keystore_service'\\]"),
            ("sms-find-group-without-priv-app", {"sms_policy_variant": "inherited-find-without-priv-app"},
             "may find \\['keystore_service'\\]"),
            ("sms-find-group-without-system-app", {"sms_policy_variant": "inherited-find-without-system-app"},
             "may find \\['keystore_service'\\]"),
            ("sms-file-group-without-priv-app", {"sms_policy_variant": "inherited-file-without-priv-app"},
             "has a rule on system_app_data_file"),
            ("sms-vsock-group-without-platform-app", {"sms_policy_variant": "inherited-vsock-without-platform-app"},
             "kernarium_sms holds .* on the vsock sockets of"),
            ("sms-find-in-system-ext", {"sms_policy_variant": "inherited-find-in-system-ext"},
             "may find \\['keystore_service'\\]"),
            ("sms-file-in-system-ext", {"sms_policy_variant": "inherited-file-in-system-ext"},
             "has a rule on system_app_data_file"),
            ("sms-vsock-in-system-ext", {"sms_policy_variant": "inherited-vsock-in-system-ext"},
             "kernarium_sms holds .* on the vsock sockets of"),
            # A target group that holds the service and another type is the other type's socket too.
            ("sms-vsock-outbound-to-a-group-with-the-service",
             {"sms_policy_variant": "outbound-to-a-group-with-the-service"},
             "kernarium_sms holds .* on the vsock sockets of \\['virtualizationservice'\\]"),
            ("sms-service-on-other-sockets", {"sms_policy_variant": "service-on-another-sockets"},
             "kernarium_sms holds .* on the vsock sockets of"),
            ("sms-data-through-app-data-file-type", {"sms_policy_variant": "data-through-app-data-file-type"},
             "may .* kernarium_sms_data_file"),
            ("sms-data-by-another-domain", {"sms_policy_variant": "data-by-another-domain"},
             "hold rights on kernarium_sms_data_file"),
            ("sms-property-through-attribute", {"sms_policy_variant": "property-through-attribute"},
             "may set service.adb.listen_addrs"),
            ("sms-property-by-another-domain", {"sms_policy_variant": "property-by-another-domain"},
             "\\['vendor_init'\\] may set service.adb.listen_addrs"),
            ("sms-property-without-a-label", {"sms_policy_variant": "no-property-label"},
             "do not label service.adb.listen_addrs"),
            ("sms-service-finds-another-service", {"sms_policy_variant": "extra-service"},
             "may find \\['window_service'\\]"),
            ("sms-service-on-system-app-data", {"sms_policy_variant": "system-app-data"},
             "has a rule on system_app_data_file"),
            ("sms-service-not-a-core-domain", {"sms_policy_variant": "no-core-domain"},
             "kernarium_sms is not in coredomain"),
            ("sms-service-not-an-app-domain", {"sms_policy_variant": "no-app-domain"},
             "kernarium_sms is not in appdomain"),
            ("sms-service-without-the-vsock-attribute", {"sms_policy_variant": "no-vsock-attribute"},
             "kernarium_sms is not in unconstrained_vsock_violators"),
            ("sms-service-not-mls-trusted", {"sms_policy_variant": "no-mls-trust"},
             "kernarium_sms is not in mlstrustedsubject: the directories of the user it runs for carry categories"),
            ("sms-holder-mls-trusted", {"sms_policy_variant": "holder-mls-trusted"},
             "kernarium_sms_listener is in mlstrustedsubject: it searches no directory of the user"),
            ("sms-waker-mls-trusted", {"sms_policy_variant": "waker-mls-trusted"},
             "kernarium_sms_wake is in mlstrustedsubject: it searches no directory of the user"),
            ("sms-holder-mls-trusted-through-expressions", {"sms_policy_variant": "holder-mls-trusted-through-expressions"},
             "kernarium_sms_listener is in mlstrustedsubject: it searches no directory of the user"),
            ("sms-waker-mls-trusted-through-expressions", {"sms_policy_variant": "waker-mls-trusted-through-expressions"},
             "kernarium_sms_wake is in mlstrustedsubject: it searches no directory of the user"),
            ("sms-package-selects-no-domain", {"sms_policy_variant": "no-seapp-line"},
             "seapp_contexts do not select kernarium_sms"),
            ("sms-domain-for-another-package", {"sms_policy_variant": "settings-in-the-domain"},
             "seapp_contexts select .* for com.android.settings"),
            # The holder of port 5560 and the waker (overlay/sms/listener): the holder alone creates, binds and listens
            # on the vsock socket and never accepts, reads, writes or connects; the service alone accepts on it, and
            # makes none; neither daemon is the framework's client but the waker, for one call; init alone enters
            # their domains; the handoff is the service's alone and the control property that starts the waker is the
            # holder's and init's alone.
            ("sms-service-creates-vsock", {"sms_policy_variant": "service-creates-vsock"},
             "unreviewed: \\['kernarium_sms'\\]"),
            ("sms-service-accepts-on-its-own", {"sms_policy_variant": "service-accepts-on-its-own"},
             "kernarium_sms holds \\['accept', 'read', 'write'\\] on its own vsock sockets"),
            ("sms-service-holds-more-on-the-holder", {"sms_policy_variant": "service-holds-more-on-the-holder"},
             "kernarium_sms holds \\['connect'\\] on the vsock sockets of kernarium_sms_listener: only"),
            ("sms-holder-sockets-through-domain", {"sms_policy_variant": "holder-sockets-through-domain"},
             "hold \\['read', 'write'\\] on a vsock socket of kernarium_sms_listener"),
            ("sms-holder-sockets-by-another-domain", {"sms_policy_variant": "holder-sockets-by-another-domain"},
             "\\['system_server'\\] hold \\['read'\\] on a vsock socket of kernarium_sms_listener"),
            ("sms-holder-connects-out", {"sms_policy_variant": "holder-connects-out"},
             "kernarium_sms_listener holds \\['getattr'\\] on the vsock sockets of \\['virtualizationservice'\\]: "
             "it connects to none"),
            *(
                (f"sms-holder-holds-{permission}", {"sms_policy_variant": f"holder-holds-{permission}"},
                 f"kernarium_sms_listener holds \\['{permission}'\\] on its vsock sockets: it never accepts")
                for permission in ("accept", "read", "write", "connect", "shutdown")
            ),
            ("sms-holder-without-the-vsock-attribute", {"sms_policy_variant": "holder-without-the-vsock-attribute"},
             "kernarium_sms_listener is not in unconstrained_vsock_violators"),
            ("sms-holder-not-a-core-domain", {"sms_policy_variant": "holder-not-a-core-domain"},
             "kernarium_sms_listener is not in coredomain"),
            ("sms-waker-not-a-core-domain", {"sms_policy_variant": "waker-not-a-core-domain"},
             "kernarium_sms_wake is not in coredomain"),
            ("sms-holder-app-domain", {"sms_policy_variant": "holder-app-domain"}, "are plain daemons"),
            ("sms-waker-with-vsock", {"sms_policy_variant": "waker-with-vsock"}, "unreviewed: \\['kernarium_sms_wake'\\]"),
            ("sms-waker-on-the-holder", {"sms_policy_variant": "waker-on-the-holder"},
             "\\['kernarium_sms_wake'\\] hold \\['getattr'\\] on a vsock socket of kernarium_sms_listener"),
            ("sms-waker-finds-another-service", {"sms_policy_variant": "waker-finds-another-service"},
             "kernarium_sms_wake may find \\['activity_service', 'window_service'\\]"),
            ("sms-waker-finds-nothing", {"sms_policy_variant": "waker-finds-nothing"},
             "kernarium_sms_wake may not find activity_service"),
            # What each holds in effect, through any group: the platform's grants to every domain are the reviewed
            # set and nothing more is accepted.
            ("sms-holder-binder-through-a-group", {"sms_policy_variant": "holder-binder-through-a-group"},
             "kernarium_sms_listener holds \\['call'\\] on servicemanager of the binder class in effect"),
            ("sms-holder-binder-grant-widened", {"sms_policy_variant": "holder-binder-grant-widened"},
             "kernarium_sms_listener holds \\['transfer'\\] on system_server of the binder class in effect"),
            ("sms-holder-finds-a-service-through-a-group", {"sms_policy_variant": "holder-finds-a-service-through-a-group"},
             "kernarium_sms_listener holds \\['find'\\] on activity_service of the service_manager class in effect"),
            ("sms-holder-hwservice-through-domain", {"sms_policy_variant": "holder-hwservice-through-domain"},
             "kernarium_sms_listener holds rights of the hwservice_manager class in effect"),
            ("sms-holder-capability-through-a-group", {"sms_policy_variant": "holder-capability-through-a-group"},
             "kernarium_sms_listener holds rights of the capability class in effect"),
            ("sms-holder-vendor-binder-device", {"sms_policy_variant": "holder-vendor-binder-device"},
             "kernarium_sms_listener may use the vendor binder device in effect"),
            ("sms-holder-binder-device-widened", {"sms_policy_variant": "holder-binder-device-widened"},
             "kernarium_sms_listener holds \\['setattr'\\] on binder_device of the chr_file class in effect"),
            ("sms-holder-hwbinder-device-widened", {"sms_policy_variant": "holder-hwbinder-device-widened"},
             "kernarium_sms_listener holds \\['execute'\\] on hwbinder_device of the chr_file class in effect"),
            ("sms-waker-binder-device-widened", {"sms_policy_variant": "waker-binder-device-widened"},
             "kernarium_sms_wake holds \\['unlink'\\] on binder_device of the chr_file class in effect"),
            ("sms-waker-hwbinder-device-widened", {"sms_policy_variant": "waker-hwbinder-device-widened"},
             "kernarium_sms_wake holds \\['relabelto'\\] on hwbinder_device of the chr_file class in effect"),
            ("sms-waker-binder-to-another", {"sms_policy_variant": "waker-binder-to-another"},
             "kernarium_sms_wake holds \\['call'\\] on surfaceflinger of the binder class in effect"),
            ("sms-waker-binder-widened", {"sms_policy_variant": "waker-binder-widened"},
             "kernarium_sms_wake holds \\['set_context_mgr'\\] on servicemanager of the binder class in effect"),
            ("sms-waker-binder-through-a-group", {"sms_policy_variant": "waker-binder-through-a-group"},
             "kernarium_sms_wake holds \\['call'\\] on system_app of the binder class in effect"),
            ("sms-su-binder-in-a-user-build", {"sms_policy_variant": "su-binder-in-a-user-build"},
             "kernarium_sms_listener holds \\['call', 'transfer'\\] on su of the binder class in effect"),
            ("sms-waker-finds-another-service-through-a-group",
             {"sms_policy_variant": "waker-finds-another-service-through-a-group"},
             "kernarium_sms_wake holds \\['find'\\] on window_service of the service_manager class in effect"),
            ("sms-waker-hwservice-through-a-group", {"sms_policy_variant": "waker-hwservice-through-a-group"},
             "kernarium_sms_wake holds rights of the hwservice_manager class in effect"),
            # The control keys: init authorises start, stop and restart through the legacy ctl.<service> key as well
            # as the qualified one, and every message through the qualified one.
            ("sms-control-legacy-unlabelled", {"sms_policy_variant": "control-legacy-unlabelled"},
             "no exact entry for ctl.kernarium_sms_listener"),
            ("sms-control-legacy-not-exact", {"sms_policy_variant": "control-legacy-not-exact"},
             "no exact entry for ctl.kernarium_sms_listener"),
            ("sms-control-legacy-generic", {"sms_policy_variant": "control-legacy-generic"},
             "\\['system_app', 'system_server'\\] may start kernarium_sms_listener through ctl.kernarium_sms_listener, "
             "labelled ctl_default_prop: only \\['init'\\] may"),
            ("sms-control-legacy-waker-generic", {"sms_policy_variant": "control-legacy-waker-generic"},
             "\\['system_app', 'system_server'\\] may start kernarium_sms_wake through ctl.kernarium_sms_wake, "
             "labelled ctl_default_prop: only \\['init', 'kernarium_sms_listener'\\] may"),
            ("sms-wake-property-not-exact", {"sms_policy_variant": "wake-property-not-exact"},
             "no exact entry for ctl.start\\$kernarium_sms_wake"),
            ("sms-wake-property-exact-in-a-later-field", {"sms_policy_variant": "wake-property-exact-in-a-later-field"},
             "no exact entry for ctl.start\\$kernarium_sms_wake"),
            ("sms-control-legacy-exact-in-a-later-field",
             {"sms_policy_variant": "control-legacy-exact-in-a-later-field"},
             "no exact entry for ctl.kernarium_sms_listener"),
            ("sms-control-oneshot-exact-in-a-later-field",
             {"sms_policy_variant": "control-oneshot-exact-in-a-later-field"},
             "no exact entry for ctl.oneshot_on\\$kernarium_sms_listener"),
            ("sms-control-oneshot-unlabelled", {"sms_policy_variant": "control-oneshot-unlabelled"},
             "no exact entry for ctl.oneshot_on\\$kernarium_sms_listener"),
            ("sms-control-oneshot-generic", {"sms_policy_variant": "control-oneshot-generic"},
             "\\['system_app', 'system_server'\\] may oneshot_on kernarium_sms_listener through "
             "ctl.oneshot_on\\$kernarium_sms_listener, labelled ctl_default_prop"),
            ("sms-control-stop-by-another", {"sms_policy_variant": "control-stop-by-another"},
             "\\['system_app'\\] may stop kernarium_sms_listener through ctl.stop\\$kernarium_sms_listener, "
             "labelled ctl_stop_prop"),
            ("sms-control-start-by-another", {"sms_policy_variant": "control-start-by-another"},
             "\\['system_app'\\] may start kernarium_sms_listener through ctl.start\\$kernarium_sms_listener, "
             "labelled ctl_start_prop"),
            ("sms-control-sigstop-by-another", {"sms_policy_variant": "control-sigstop-by-another"},
             "\\['system_app'\\] may sigstop_on kernarium_sms_listener through ctl.sigstop_on\\$kernarium_sms_listener"),
            ("sms-control-restart-through-attribute", {"sms_policy_variant": "control-restart-through-attribute"},
             "\\['shell'\\] may restart kernarium_sms_listener through ctl.restart\\$kernarium_sms_listener"),
            ("sms-control-locked-set-by-another", {"sms_policy_variant": "control-locked-set-by-another"},
             "\\['system_app'\\] may start kernarium_sms_listener through ctl.kernarium_sms_listener, "
             "labelled ctl_kernarium_sms_locked_prop"),
            ("sms-control-locked-set-by-the-holder", {"sms_policy_variant": "control-locked-set-by-the-holder"},
             "\\['kernarium_sms_listener'\\] may start kernarium_sms_listener through ctl.kernarium_sms_listener"),
            ("sms-control-stop-by-the-holder", {"sms_policy_variant": "control-stop-by-the-holder"},
             "\\['kernarium_sms_listener'\\] may stop kernarium_sms_listener through ctl.stop\\$kernarium_sms_listener"),
            ("sms-holder-calls-binder", {"sms_policy_variant": "holder-calls-binder"},
             "kernarium_sms_listener holds rights of \\['binder'\\] classes"),
            ("sms-holder-finds-a-service", {"sms_policy_variant": "holder-finds-a-service"},
             "kernarium_sms_listener holds rights of \\['service_manager'\\] classes"),
            ("sms-holder-owns-files", {"sms_policy_variant": "holder-owns-files"},
             "kernarium_sms_listener holds rights of \\['dir'\\] classes"),
            ("sms-holder-entered-by-another", {"sms_policy_variant": "holder-entered-by-another"},
             "kernarium_sms_listener is entered by \\[\\('init', 'kernarium_sms_listener_exec'\\), "
             "\\('shell', 'kernarium_sms_listener_exec'\\)\\]"),
            ("sms-holder-entered-through-another-file", {"sms_policy_variant": "holder-entered-through-another-file"},
             "kernarium_sms_listener is entered by .*'system_file'"),
            ("sms-holder-never-entered", {"sms_policy_variant": "holder-never-entered"},
             "kernarium_sms_listener is entered by \\[\\]"),
            ("sms-waker-entered-by-another", {"sms_policy_variant": "waker-entered-by-another"},
             "kernarium_sms_wake is entered by .*'shell'"),
            ("sms-waker-transition-by-another", {"sms_policy_variant": "waker-transition-by-another"},
             "\\['shell'\\] may transition into kernarium_sms_wake: only init does"),
            ("sms-handoff-connect-by-another", {"sms_policy_variant": "handoff-connect-by-another"},
             "\\['shell'\\] may connect to the socket of kernarium_sms_listener: only kernarium_sms does"),
            ("sms-handoff-service-cannot-connect", {"sms_policy_variant": "handoff-service-cannot-connect"},
             "kernarium_sms may not connect to the socket of kernarium_sms_listener"),
            ("sms-handoff-file-written-by-another", {"sms_policy_variant": "handoff-file-written-by-another"},
             "\\['untrusted_app'\\] may write kernarium_sms_socket: only kernarium_sms does"),
            ("sms-handoff-holder-cannot-listen", {"sms_policy_variant": "handoff-holder-cannot-listen"},
             "kernarium_sms_listener may not \\['listen'\\] on its own unix socket"),
            ("sms-handoff-holder-cannot-accept", {"sms_policy_variant": "handoff-holder-cannot-accept"},
             "kernarium_sms_listener may not \\['accept'\\] on its own unix socket"),
            ("sms-handoff-service-cannot-write-the-file", {"sms_policy_variant": "handoff-service-cannot-write-the-file"},
             "kernarium_sms may not write kernarium_sms_socket"),
            ("sms-handoff-fd-use-by-another", {"sms_policy_variant": "handoff-fd-use-by-another"},
             "\\['shell'\\] may use the descriptors of kernarium_sms_listener"),
            ("sms-handoff-service-cannot-use-the-fd", {"sms_policy_variant": "handoff-service-cannot-use-the-fd"},
             "kernarium_sms may not use the listening descriptor of kernarium_sms_listener"),
            ("sms-holder-denial-not-silenced", {"sms_policy_variant": "no-holder-dontaudit"},
             "does not silence the denial of kernarium_sms_listener setting the legacy key ctl.kernarium_sms_wake, "
             "labelled ctl_kernarium_sms_locked_prop"),
            ("sms-holder-denial-silenced-for-another", {"sms_policy_variant": "holder-dontaudit-for-another"},
             "does not silence the denial of kernarium_sms_listener setting the legacy key ctl.kernarium_sms_wake"),
            # The one rule names the holder and the locked type exactly, and the holder has no other property_service
            # dontaudit in the product's policies.
            ("sms-holder-denial-silenced-over-an-attribute", {"sms_policy_variant": "holder-dontaudit-over-an-attribute"},
             "silences a property_service denial of kernarium_sms_listener beyond its start of the waker: "
             "\\(dontaudit kernarium_sms_listener property_type "),
            ("sms-holder-denial-silenced-over-an-attribute-too",
             {"sms_policy_variant": "holder-dontaudit-over-an-attribute-too"},
             "silences a property_service denial of kernarium_sms_listener beyond its start of the waker"),
            ("sms-holder-dontaudit-of-another-type", {"sms_policy_variant": "holder-dontaudit-of-another-type"},
             "silences a property_service denial of kernarium_sms_listener beyond its start of the waker: "
             "\\(dontaudit kernarium_sms_listener ctl_kernarium_sms_wake_prop "),
            ("sms-dontaudit-for-every-domain", {"sms_policy_variant": "holder-dontaudit-for-every-domain"},
             "silences a property_service denial of kernarium_sms_listener beyond its start of the waker: "
             "\\(dontaudit domain system_prop "),
            ("sms-wake-property-generic", {"sms_policy_variant": "wake-property-generic"},
             "label ctl.start\\$kernarium_sms_wake ctl_start_prop, not ctl_kernarium_sms_wake_prop"),
            ("sms-wake-property-by-another", {"sms_policy_variant": "wake-property-by-another"},
             "\\['shell'\\] may set ctl.start\\$kernarium_sms_wake, labelled ctl_kernarium_sms_wake_prop: only "
             "kernarium_sms_listener does, and init"),
            ("sms-wake-property-through-attribute", {"sms_policy_variant": "wake-property-through-attribute"},
             "\\['shell'\\] may set ctl.start\\$kernarium_sms_wake"),
            ("sms-wake-property-by-nobody", {"sms_policy_variant": "wake-property-by-nobody"},
             "kernarium_sms_listener may not set ctl.start\\$kernarium_sms_wake"),
            ("sms-holder-unlabelled", {"sms_policy_variant": "holder-unlabelled"},
             "system_ext file contexts do not label /system_ext/bin/kernarium_sms_listener as kernarium_sms_listener_exec"),
            ("sms-waker-unlabelled", {"sms_policy_variant": "waker-unlabelled"},
             "system_ext file contexts do not label /system_ext/bin/kernarium_sms_wake.sh as kernarium_sms_wake_exec"),
            ("sms-socket-unlabelled", {"sms_policy_variant": "socket-unlabelled"},
             "system_ext file contexts do not label /dev/socket/kernarium_sms as kernarium_sms_socket"),
            # The text message service's files: the holder's binary, the init script (the holder with its class, the
            # waker through its property only), the waker's one command, and the application with one service that
            # no application can start and nothing persistent.
            ("sms-missing-holder-binary", {"entries": {"SYSTEM_EXT/bin/kernarium_sms_listener": None}},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_sms_listener"),
            ("sms-missing-waker-script", {"entries": {"SYSTEM_EXT/bin/kernarium_sms_wake.sh": None}},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_sms_wake.sh"),
            ("sms-missing-init-script", {"entries": {"SYSTEM_EXT/etc/init/kernarium_sms_listener.rc": None}},
             "required entries are missing: .*SYSTEM_EXT/etc/init/kernarium_sms_listener.rc"),
            ("sms-rc-missing-holder", {"sms_service_variant": "rc-missing-holder"},
             "text message init script must define the services"),
            ("sms-rc-missing-waker", {"sms_service_variant": "rc-missing-waker"},
             "text message init script must define the services"),
            ("sms-rc-third-service", {"sms_service_variant": "rc-third-service"},
             "text message init script must define the services"),
            ("sms-rc-holder-other-executable", {"sms_service_variant": "rc-holder-other-executable"},
             "kernarium_sms_listener runs /system_ext/bin/other, not /system_ext/bin/kernarium_sms_listener"),
            ("sms-rc-waker-other-executable", {"sms_service_variant": "rc-waker-other-executable"},
             "kernarium_sms_wake runs /system_ext/bin/other.sh, not /system_ext/bin/kernarium_sms_wake.sh"),
            *(
                (f"sms-{name}", {"sms_service_variant": name}, f"service {service} has the options")
                for name, service in (
                    ("rc-holder-not-in-main", "kernarium_sms_listener"), ("rc-holder-as-root", "kernarium_sms_listener"),
                    ("rc-holder-without-a-group", "kernarium_sms_listener"),
                    ("rc-holder-in-group-root", "kernarium_sms_listener"),
                    ("rc-holder-extra-group", "kernarium_sms_listener"),
                    ("rc-holder-without-the-socket", "kernarium_sms_listener"),
                    ("rc-holder-socket-open", "kernarium_sms_listener"),
                    ("rc-holder-socket-for-shell", "kernarium_sms_listener"),
                    ("rc-holder-seclabel", "kernarium_sms_listener"), ("rc-holder-capability", "kernarium_sms_listener"),
                    ("rc-holder-oneshot", "kernarium_sms_listener"), ("rc-waker-as-root", "kernarium_sms_wake"),
                    ("rc-waker-not-oneshot", "kernarium_sms_wake"), ("rc-waker-not-disabled", "kernarium_sms_wake"),
                    ("rc-waker-capability", "kernarium_sms_wake"))
            ),
            ("sms-rc-trigger", {"sms_service_variant": "rc-trigger"}, "init script has a trigger"),
            *(
                (f"sms-{name}", {"sms_service_variant": name}, "must run exactly")
                for name in ("script-other-command", "script-other-component", "script-other-user", "script-keeps-output",
                             "script-extra-command")
            ),
            ("sms-apk-not-an-apk", {"sms_service_variant": "apk-not-an-apk"}, "KernariumSms.apk has no readable manifest"),
            ("sms-apk-other-package", {"sms_service_variant": "apk-other-package"},
             "is the package android.kernarium.other, not android.kernarium.sms"),
            ("sms-apk-no-shared-user", {"sms_service_variant": "apk-no-shared-user"}, "does not share the system uid"),
            ("sms-apk-persistent", {"sms_service_variant": "apk-persistent"}, "the application sets persistent"),
            ("sms-apk-persistent-false", {"sms_service_variant": "apk-persistent-false"},
             "the application sets persistent"),
            ("sms-apk-application-class", {"sms_service_variant": "apk-application-class"},
             "the application names a class of its own"),
            ("sms-apk-service-exported", {"sms_service_variant": "apk-service-exported"},
             "the service's exported is True, not false"),
            ("sms-apk-service-exported-unset", {"sms_service_variant": "apk-service-exported-unset"},
             "the service's exported is None, not false"),
            ("sms-apk-service-other-class", {"sms_service_variant": "apk-service-other-class"},
             "the service is '.Other', not .HostService"),
            *(
                (f"sms-{name}", {"sms_service_variant": name}, f"the service sets {attribute}")
                for name, attribute in (
                    ("apk-service-guarded", "permission"), ("apk-service-own-process", "process"),
                    ("apk-service-isolated", "isolatedProcess"), ("apk-service-disabled", "enabled"),
                    ("apk-service-direct-boot-aware", "directBootAware"),
                    ("apk-service-foreground", "foregroundServiceType"))
            ),
            ("sms-apk-second-service", {"sms_service_variant": "apk-second-service"}, "has 2 service elements, not 1"),
            ("sms-apk-no-service", {"sms_service_variant": "apk-no-service"}, "has 0 service elements, not 1"),
            ("sms-apk-has-an-activity", {"sms_service_variant": "apk-has-an-activity"},
             "has \\['activity'\\]: the application has one service"),
            ("sms-apk-has-a-receiver", {"sms_service_variant": "apk-has-a-receiver"},
             "has \\['receiver'\\]: the application has one service"),
            ("sms-apk-has-a-provider", {"sms_service_variant": "apk-has-a-provider"},
             "has \\['provider'\\]: the application has one service"),
            ("sms-apk-asks-a-permission", {"sms_service_variant": "apk-asks-a-permission"},
             "has \\['uses-permission'\\]: the application has one service"),
            ("sms-apk-uses-sdk-twice", {"sms_service_variant": "apk-uses-sdk-twice"},
             "has 2 uses-sdk elements, not at most 1"),
            ("sms-apk-uses-sdk-named", {"sms_service_variant": "apk-uses-sdk-named"},
             "the uses-sdk element sets \\['name'\\]: it carries SDK levels and nothing else"),
            ("sms-apk-uses-sdk-permission", {"sms_service_variant": "apk-uses-sdk-permission"},
             "the uses-sdk element sets \\['permission'\\]: it carries SDK levels and nothing else"),
            ("sms-apk-service-filter", {"sms_service_variant": "apk-service-filter"},
             "has \\['action', 'intent-filter'\\]: the application has one service"),
            ("missing-location-feature", {"missing_location_feature": "android.hardware.location"},
             "feature is not declared: android.hardware.location(?!\\.gps)"),
            ("missing-gps-feature", {"missing_location_feature": "android.hardware.location.gps"},
             "feature is not declared: android.hardware.location.gps"),
            ("location-probe-left-in-the-image", {"entries": {
                "SYSTEM/app/AndariumLocationProbe/AndariumLocationProbe.apk": synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: SYSTEM/app/AndariumLocationProbe"),
            ("keymouse-probe-left-in-the-image", {"entries": {
                "SYSTEM/app/AndariumKeyMouseProbe/AndariumKeyMouseProbe.apk": synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: SYSTEM/app/AndariumKeyMouseProbe"),
            ("sms-probe-left-in-the-image", {"entries": {
                "SYSTEM/app/AndariumSmsProbe/AndariumSmsProbe.apk": synthetic_apk(debuggable=False)}},
             "unneeded built-in application payload is forbidden: SYSTEM/app/AndariumSmsProbe"),
            ("bugreport-keychord", {"init_variant": "bugreport-keychord"},
             "starts bugreport on a keychord \\('keycodes 30 48'\\)"),
            ("keyboard-default-missing-script", {"keyboard_default_variant": "missing-script"},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_keyboard_default.sh"),
            ("keyboard-default-missing-rc", {"keyboard_default_variant": "missing-rc"},
             "required entries are missing: .*SYSTEM_EXT/etc/init/kernarium_keyboard_default.rc"),
            ("keyboard-default-missing-service", {"keyboard_default_variant": "missing-service"},
             "does not declare the kernarium_keyboard_default service"),
            ("keyboard-default-not-oneshot", {"keyboard_default_variant": "not-oneshot"},
             "does not declare kernarium_keyboard_default as oneshot"),
            ("keyboard-default-not-disabled", {"keyboard_default_variant": "not-disabled"},
             "does not declare kernarium_keyboard_default as disabled"),
            ("keyboard-default-started-at-boot", {"keyboard_default_variant": "started-at-boot"},
             "starts kernarium_keyboard_default under 'boot'"),
            ("keyboard-default-no-start", {"keyboard_default_variant": "no-start"},
             "does not start kernarium_keyboard_default when boot completes"),
            ("keyboard-default-no-read", {"keyboard_default_variant": "no-read"},
             "does not contain 'cmd settings get secure show_ime_with_hard_keyboard'"),
            ("keyboard-default-no-put", {"keyboard_default_variant": "no-put"},
             "does not contain 'cmd settings put secure show_ime_with_hard_keyboard 0'"),
            ("keyboard-default-no-marker", {"keyboard_default_variant": "no-marker"},
             "does not contain 'setprop \"\\$marker\" 1'"),
            ("keyboard-default-turns-it-on", {"keyboard_default_variant": "turns-it-on"},
             "contains 'settings put secure show_ime_with_hard_keyboard 1'"),
            ("keyboard-default-writes-another-setting", {"keyboard_default_variant": "writes-another-setting"},
             "contains 'settings put system'"),
            ("keyboard-default-deletes-the-setting", {"keyboard_default_variant": "deletes-the-setting"},
             "contains 'settings delete'"),
            ("keyboard-default-other-user", {"keyboard_default_variant": "other-user"},
             "contains '--user'"),
            ("keyboard-default-missing-label", {"keyboard_default_variant": "missing-label"},
             "do not label /system_ext/bin/kernarium_keyboard_default.sh"),
            ("keyboard-default-missing-transition", {"keyboard_default_variant": "missing-transition"},
             "does not move init into the keyboard default domain"),
            ("keyboard-default-missing-settings-find", {"keyboard_default_variant": "missing-settings-find"},
             "does not allow kernarium_keyboard_default settings_service service_manager find"),
            ("keyboard-default-missing-transfer", {"keyboard_default_variant": "missing-transfer"},
             "does not allow kernarium_keyboard_default system_server binder transfer"),
            ("keyboard-default-missing-call", {"keyboard_default_variant": "missing-call"},
             "does not allow system_server kernarium_keyboard_default binder call"),
            ("keyboard-default-missing-fd", {"keyboard_default_variant": "missing-fd"},
             "does not allow system_server kernarium_keyboard_default fd use"),
            ("keyboard-default-missing-fifo", {"keyboard_default_variant": "missing-fifo"},
             "does not allow system_server kernarium_keyboard_default fifo_file write"),
            ("keyboard-default-marker-not-system", {"keyboard_default_variant": "marker-not-system"},
             "does not define the system-owned kernarium_keyboard_default_prop"),
            ("keyboard-default-missing-marker-rule", {"keyboard_default_variant": "missing-marker-rule"},
             "does not let the keyboard default domain set persist.sys.kernarium.keyboard_default"),
            ("keyboard-default-sets-another-property", {"keyboard_default_variant": "sets-another-property"},
             "lets the keyboard default domain set"),
            ("keyboard-default-missing-marker-context", {"keyboard_default_variant": "missing-marker-context"},
             "do not label persist.sys.kernarium.keyboard_default"),
            ("oldapp-missing-wrapper", {"oldswitch_variant": "missing-wrapper"},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_oldapp"),
            ("oldapp-missing-jar", {"oldswitch_variant": "missing-jar"},
             "required entries are missing: .*SYSTEM_EXT/framework/kernarium_oldapp.jar"),
            ("oldapp-wrapper-no-classpath", {"oldswitch_variant": "wrapper-no-classpath"},
             "kernarium_oldapp does not contain 'CLASSPATH=.base/framework/kernarium_oldapp.jar'"),
            ("oldapp-wrapper-wrong-jar", {"oldswitch_variant": "wrapper-wrong-jar"},
             "kernarium_oldapp does not contain 'CLASSPATH=.base/framework/kernarium_oldapp.jar'"),
            ("oldapp-wrapper-wrong-class", {"oldswitch_variant": "wrapper-wrong-class"},
             "kernarium_oldapp does not contain 'exec app_process .base/bin com.kernarium.oldapp.Main'"),
            ("oldapp-wrapper-no-exec", {"oldswitch_variant": "wrapper-no-exec"},
             "kernarium_oldapp does not contain 'exec app_process"),
            ("oldapp-wrapper-system-base", {"oldswitch_variant": "wrapper-system-base"},
             "kernarium_oldapp does not contain 'base=/system_ext'"),
            ("oldapp-jar-not-zip", {"oldswitch_variant": "jar-not-zip"},
             "kernarium_oldapp.jar is not a readable jar"),
            ("oldapp-jar-without-class", {"oldswitch_variant": "jar-without-class"},
             "holds code without the class com.kernarium.oldapp.Main"),
            ("apps-command-missing-wrapper", {"appshell_variant": "missing-wrapper"},
             "required entries are missing: .*SYSTEM_EXT/bin/kernarium_apps"),
            ("apps-command-missing-jar", {"appshell_variant": "missing-jar"},
             "required entries are missing: .*SYSTEM_EXT/framework/kernarium_apps.jar"),
            ("apps-command-wrapper-no-classpath", {"appshell_variant": "wrapper-no-classpath"},
             "kernarium_apps does not contain 'CLASSPATH=.base/framework/kernarium_apps.jar'"),
            ("apps-command-wrapper-wrong-jar", {"appshell_variant": "wrapper-wrong-jar"},
             "kernarium_apps does not contain 'CLASSPATH=.base/framework/kernarium_apps.jar'"),
            ("apps-command-wrapper-wrong-class", {"appshell_variant": "wrapper-wrong-class"},
             "kernarium_apps does not contain 'exec app_process .base/bin com.kernarium.apps.Main'"),
            ("apps-command-wrapper-no-exec", {"appshell_variant": "wrapper-no-exec"},
             "kernarium_apps does not contain 'exec app_process"),
            ("apps-command-jar-not-zip", {"appshell_variant": "jar-not-zip"},
             "kernarium_apps.jar is not a readable jar"),
            ("apps-command-jar-without-class", {"appshell_variant": "jar-without-class"},
             "holds code without the class com.kernarium.apps.Main"),
            ("apps-command-jar-stripped", {"appshell_variant": "jar-stripped"},
             "kernarium_apps.jar has no classes.dex and no preoptimized odex and vdex"),
            ("icons-app-missing", {"icons_variant": "missing"},
             "required entries are missing: .*SYSTEM_EXT/app/KernariumIcons/KernariumIcons.apk"),
            ("icons-app-privileged-copy", {"icons_variant": "privileged-copy"},
             "priv-app/KernariumIcons/KernariumIcons.apk: the icon renderer is an ordinary application of app/"),
            ("icons-app-not-an-apk", {"icons_variant": "not-an-apk"}, "KernariumIcons.apk has no readable manifest"),
            ("icons-app-package-changed", {"icons_variant": "package-changed"},
             "is the package com.kernarium.other, not com.kernarium.icons"),
            ("icons-app-shared-user", {"icons_variant": "shared-user"}, "the manifest sets sharedUserId"),
            ("icons-app-asks-a-permission", {"icons_variant": "asks-a-permission"},
             "has uses-permission: the renderer asks for no permission"),
            ("icons-app-asks-a-permission-from-sdk-m", {"icons_variant": "asks-a-permission-from-sdk-m"},
             "has uses-permission-sdk-m: the renderer asks for no permission"),
            ("icons-app-asks-a-permission-from-sdk-23", {"icons_variant": "asks-a-permission-from-sdk-23"},
             "has uses-permission-sdk-23: the renderer asks for no permission"),
            ("icons-provider-enabled-by-reference", {"icons_variant": "provider-enabled-by-reference"},
             "the provider sets enabled"),
            ("icons-provider-read-permission-by-reference", {"icons_variant": "provider-read-permission-by-reference"},
             "the provider sets readPermission"),
            ("icons-provider-class-changed", {"icons_variant": "provider-class-changed"},
             "the provider is com.kernarium.icons.BackupIconProvider, not com.kernarium.icons.IconProvider"),
            ("icons-provider-class-of-another-package", {"icons_variant": "provider-class-of-another-package"},
             "the provider is com.example.IconProvider, not com.kernarium.icons.IconProvider"),
            ("icons-service-class-changed", {"icons_variant": "service-class-changed"},
             "the service is com.kernarium.icons.IconRenderServiceTwo, not com.kernarium.icons.IconRenderService"),
            ("icons-service-app-zygote", {"icons_variant": "service-app-zygote"}, "the service sets useAppZygote"),
            ("icons-seapp-isolated-for-another-seinfo", {"icons_variant": "seapp-isolated-for-another-seinfo"},
             "put the icon renderer's isolated service in None, not isolated_app"),
            ("icons-seapp-isolated-for-a-newer-target", {"icons_variant": "seapp-isolated-for-a-newer-target"},
             "put the icon renderer's isolated service in None, not isolated_app"),
            ("icons-app-has-an-activity", {"icons_variant": "has-an-activity"}, "has activity: the renderer asks for no permission"),
            ("icons-app-has-a-receiver", {"icons_variant": "has-a-receiver"}, "has receiver: the renderer asks for no permission"),
            ("icons-app-has-a-filter", {"icons_variant": "has-a-filter"}, "has intent-filter: the renderer asks for no permission"),
            ("icons-app-second-provider", {"icons_variant": "second-provider"}, "has 2 provider elements, not 1"),
            ("icons-provider-not-exported", {"icons_variant": "provider-not-exported"}, "the provider's exported is False, not True"),
            ("icons-provider-open", {"icons_variant": "provider-open"},
             "the provider's permission is None, not 'android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY'"),
            ("icons-provider-other-permission", {"icons_variant": "provider-other-permission"},
             "the provider's permission is 'android.permission.INTERACT_ACROSS_USERS'"),
            ("icons-provider-grants-uris", {"icons_variant": "provider-grants-uris"}, "the provider sets grantUriPermissions"),
            ("icons-provider-disabled", {"icons_variant": "provider-disabled"}, "the provider sets enabled"),
            ("icons-provider-other-authority", {"icons_variant": "provider-other-authority"},
             "the provider's authorities is 'com.kernarium.icon'"),
            ("icons-provider-not-direct-boot-aware", {"icons_variant": "provider-not-direct-boot-aware"},
             "the provider's directBootAware is None, not True"),
            ("icons-service-not-isolated", {"icons_variant": "service-not-isolated"}, "the service's isolatedProcess is False, not True"),
            ("icons-service-exported", {"icons_variant": "service-exported"}, "the service's exported is True, not False"),
            ("icons-service-guarded", {"icons_variant": "service-guarded"}, "the service sets permission"),
            ("icons-service-not-direct-boot-aware", {"icons_variant": "service-not-direct-boot-aware"},
             "the service's directBootAware is None, not True"),
            ("icons-application-allows-backup", {"icons_variant": "application-allows-backup"},
             "the application's allowBackup is not false"),
            ("icons-application-not-direct-boot-aware", {"icons_variant": "application-not-direct-boot-aware"},
             "the application's directBootAware is not true"),
            ("icons-application-closed-to-hidden-apis", {"icons_variant": "application-closed-to-hidden-apis"},
             "the application's usesNonSdkApi is not true"),
            ("icons-application-persistent", {"icons_variant": "application-persistent"}, "the application sets persistent"),
            ("icons-app-no-target-sdk", {"icons_variant": "no-target-sdk"}, "has no integer targetSdkVersion"),
            ("icons-app-platform-key", {"icons_variant": "platform-key"},
             "KernariumIcons.apk is signed with 'build/make/target/product/security/platform.x509.pem'"),
            ("icons-app-media-key", {"icons_variant": "platform-key-in-the-tree"}, "KernariumIcons.apk is signed with"),
            ("icons-app-other-partition", {"icons_variant": "other-partition"}, "on 'system': the renderer takes the default key"),
            ("icons-app-no-signing-line", {"icons_variant": "no-signing-line"}, "apkcerts.txt has no line for KernariumIcons.apk"),
            ("icons-app-no-signing-table", {"icons_variant": "no-signing-table"}, "META/apkcerts.txt is missing"),
            ("icons-seapp-by-name", {"icons_variant": "seapp-by-name"},
             "seapp_contexts select untrusted_app for the icon renderer by name \\(com.kernarium.icons\\)"),
            ("icons-seapp-by-prefix", {"icons_variant": "seapp-by-prefix"},
             "seapp_contexts select kernarium_icons for the icon renderer by name \\(com.kernarium.\\*\\)"),
            ("icons-seapp-app-domain-changed", {"icons_variant": "seapp-app-domain-changed"},
             "put the icon renderer's provider in platform_app, not an untrusted_app domain"),
            ("icons-seapp-privileged-domain", {"icons_variant": "seapp-privileged-domain"},
             "put the icon renderer's provider in priv_app, not an untrusted_app domain"),
            ("icons-seapp-isolated-changed", {"icons_variant": "seapp-isolated-changed"},
             "put the icon renderer's isolated service in untrusted_app, not isolated_app"),
            ("icons-seapp-no-isolated", {"icons_variant": "seapp-no-isolated"},
             "put the icon renderer's isolated service in None, not isolated_app"),
            ("oldapp-missing-context", {"oldswitch_variant": "missing-context"},
             "is labelled default_prop, not kernarium_oldapp_prop"),
            ("oldapp-exact-context", {"oldswitch_variant": "exact-context"},
             "is labelled default_prop, not kernarium_oldapp_prop"),
            ("oldapp-wrong-type-context", {"oldswitch_variant": "wrong-type-context"},
             "is labelled kernarium_time_zone_prop, not kernarium_oldapp_prop"),
            ("oldapp-not-system", {"oldswitch_variant": "not-system"},
             "does not define the system-owned kernarium_oldapp_prop"),
            ("oldapp-no-shell-set", {"oldswitch_variant": "no-shell-set"},
             "the shell domain may not set persist.sys.kernarium.oldapp."),
            ("oldapp-no-app-read", {"oldswitch_variant": "no-app-read"},
             "may not read persist.sys.kernarium.oldapp."),
            ("oldapp-untrusted-app-sets", {"oldswitch_variant": "untrusted-app-sets"},
             "untrusted_app may set persist.sys.kernarium.oldapp."),
            ("oldapp-platform-app-sets", {"oldswitch_variant": "platform-app-sets"},
             "platform_app_36 may set persist.sys.kernarium.oldapp."),
            ("oldapp-appdomain-sets", {"oldswitch_variant": "appdomain-sets"},
             "may set persist.sys.kernarium.oldapp."),
            ("oldapp-runas-app-sets", {"oldswitch_variant": "runas-app-sets"},
             "runas_app may set persist.sys.kernarium.oldapp."),
            ("oldapp-no-neverallow", {"oldswitch_variant": "no-neverallow"},
             "no neverallow that keeps application domains from setting kernarium_oldapp_prop"),
            # The restricted macro's own rule is of the vendor kind (domain without the core
            # domains) and covers the two non-core application domains alone.
            ("oldapp-vendor-style-neverallow", {"oldswitch_variant": "vendor-style-neverallow"},
             "no neverallow that keeps application domains from setting kernarium_oldapp_prop"),
            ("oldapp-partial-neverallow", {"oldswitch_variant": "partial-neverallow"},
             "no neverallow that keeps application domains from setting kernarium_oldapp_prop: none covers"),
            ("oldapp-internal-scope", {"oldswitch_variant": "internal-scope"},
             "kernarium_oldapp_prop is a system_internal_prop"),
            ("oldapp-not-restricted", {"oldswitch_variant": "not-restricted"},
             "kernarium_oldapp_prop is not declared with system_restricted_prop"),
            ("oldapp-partial-app-read", {"oldswitch_variant": "partial-app-read"},
             "may not read persist.sys.kernarium.oldapp.<package>, labelled kernarium_oldapp_prop "
             r"\(getattr, map, open, read on the file\)"),
            ("oldapp-no-lock-file", {"oldswitch_variant": "no-lock-file"},
             "shell domain may not create and lock files of shell_data_file"),
            ("oldapp-no-lock-setattr", {"oldswitch_variant": "no-lock-setattr"},
             "shell domain may not create and lock files of shell_data_file"),
            ("oldapp-no-lock-dir", {"oldswitch_variant": "no-lock-dir"},
             "shell domain may not create and lock files of shell_data_file"),
            ("oldapp-jar-stripped", {"oldswitch_variant": "jar-stripped"},
             "kernarium_oldapp.jar has no classes.dex and no preoptimized odex and vdex"),
            ("oldapp-preopt-half", {"oldswitch_variant": "preopt-half"},
             "kernarium_oldapp.jar has no classes.dex and no preoptimized odex and vdex"),
            ("missing-side-key-layout", {"entries": {input_device_files["Vendor_1af4_Product_0002.kl"]: None}},
             "required entries are missing"),
            ("missing-keyboard-configuration", {"entries": {input_device_files["Vendor_1af4_Product_0003.idc"]: None}},
             "required entries are missing"),
            ("missing-mouse-configuration", {"entries": {input_device_files["Vendor_1af4_Product_0004.idc"]: None}},
             "required entries are missing"),
            ("missing-pointer-configuration", {"entries": {input_device_files["Vendor_1af4_Product_0005.idc"]: None}},
             "required entries are missing"),
            ("side-key-layout-with-a-sixth-key", {"entries": {
                input_device_files["Vendor_1af4_Product_0002.kl"]: (side_key_layout + "key 30   A\n").encode()}},
             "must map the five side keys alone"),
            ("side-key-layout-without-home", {"entries": {
                input_device_files["Vendor_1af4_Product_0002.kl"]: side_key_layout.replace(
                    "key 172   HOME\n", "").encode()}},
             "must map the five side keys alone"),
            ("side-key-layout-with-another-label", {"entries": {
                input_device_files["Vendor_1af4_Product_0002.kl"]: side_key_layout.replace(
                    "key 172   HOME", "key 172   MOVE_HOME").encode()}},
             "must map the five side keys alone"),
            ("side-key-layout-with-a-flag", {"entries": {
                input_device_files["Vendor_1af4_Product_0002.kl"]: side_key_layout.replace(
                    "key 116   POWER", "key 116   POWER   WAKE").encode()}},
             "must be a 'key <scan code> <label>' line"),
            ("side-key-layout-with-an-axis", {"entries": {
                input_device_files["Vendor_1af4_Product_0002.kl"]: (side_key_layout + "axis 0x00 X\n").encode()}},
             "must be a 'key <scan code> <label>' line"),
            ("side-key-layout-maps-a-key-twice", {"entries": {
                input_device_files["Vendor_1af4_Product_0002.kl"]: (side_key_layout + "key 116   SLEEP\n").encode()}},
             "maps scan code 116 twice"),
            ("keyboard-configuration-internal", {"entries": {
                input_device_files["Vendor_1af4_Product_0003.idc"]: b"device.internal = 1\n"}},
             "must set \\{'device.internal': '0'\\} and nothing else"),
            ("keyboard-configuration-with-a-layout", {"entries": {
                input_device_files["Vendor_1af4_Product_0003.idc"]: b"device.internal = 0\nkeyboard.layout = Generic\n"}},
             "must set .* and nothing else"),
            ("mouse-configuration-empty", {"entries": {input_device_files["Vendor_1af4_Product_0004.idc"]: b"# none\n"}},
             "must set .* and nothing else"),
            ("pointer-configuration-without-wake", {"entries": {
                input_device_files["Vendor_1af4_Product_0005.idc"]: b"touch.orientationAware = 1\n"}},
             "must set .* and nothing else"),
            ("pointer-configuration-orientation-unaware", {"entries": {
                input_device_files["Vendor_1af4_Product_0005.idc"]: b"touch.orientationAware = 0\ntouch.wake = 1\n"}},
             "must set .* and nothing else"),
            ("pointer-configuration-malformed", {"entries": {
                input_device_files["Vendor_1af4_Product_0005.idc"]: b"touch.orientationAware 1\n"}},
             "must be a 'name = value' line"),
            ("pointer-configuration-sets-a-property-twice", {"entries": {
                input_device_files["Vendor_1af4_Product_0005.idc"]: (
                    b"touch.orientationAware = 1\ntouch.wake = 1\ntouch.wake = 0\n")}},
             "sets touch.wake twice"),
            ("keyboard-with-a-key-layout-of-its-own", {"entries": {
                "VENDOR/usr/keylayout/Vendor_1af4_Product_0003.kl": b"key 30   A\n"}},
             "a key layout or character map of its own"),
            ("mouse-with-a-versioned-key-layout", {"entries": {
                "VENDOR/usr/keylayout/Vendor_1af4_Product_0004_Version_0001.kl": b"key 30   A\n"}},
             "a key layout or character map of its own"),
            ("pointer-with-a-character-map", {"entries": {
                "VENDOR/usr/keychars/Vendor_1af4_Product_0005.kcm": b"type FULL\n"}},
             "a key layout or character map of its own"),
            # The host's cameras. The layouts' files are the generator's, byte for byte, and nothing
            # else uses the vendor SKU axis.
            ("camera-feature-in-a-permission-file", {"entries": {
                "VENDOR/etc/permissions/android.hardware.camera.xml":
                    b'<permissions><feature name="android.hardware.camera"/></permissions>'}},
             "a camera feature is declared outside the camera layouts.*android.hardware.camera"),
            ("camera-feature-in-a-layout-of-another-name", {"entries": {
                "VENDOR/etc/permissions/kernarium_camera.xml":
                    camera_layouts.permissions_file(camera_layouts.by_name()["front"])}},
             "a camera feature is declared outside the camera layouts"),
            ("stock-camera-feature-file", {"entries": {
                "VENDOR/etc/permissions/android.hardware.camera.front.xml": b"<permissions/>"}},
             "absent hardware feature declaration is forbidden"),
            ("missing-camera-layout", {"entries": {camera_front_path: None}},
             "sku_front/kernarium_camera.xml is missing"),
            ("camera-layout-with-more-features", {"entries": {camera_front_path: camera_layout_files[
                "VENDOR/etc/permissions/sku_bf-small/kernarium_camera.xml"]}},
             "differs from what tools/camera_layouts.py generates"),
            ("camera-layout-edited", {"entries": {camera_front_path: camera_layout_files[camera_front_path].replace(
                b"android.hardware.camera.any", b"android.hardware.camera.level.full")}},
             "differs from what tools/camera_layouts.py generates"),
            ("camera-sku-directory-of-another-name", {"entries": {
                "VENDOR/etc/permissions/sku_extra/kernarium_camera.xml": b"<permissions/>"}},
             "is not a file of a camera layout"),
            ("camera-sku-directory-with-another-file", {"entries": {
                "VENDOR/etc/permissions/sku_front/other.xml": b"<permissions/>"}},
             "is not a file of a camera layout"),
            ("camera-sku-sysconfig", {"entries": {"VENDOR/etc/sysconfig/sku_front/other.xml": b"<config/>"}},
             "SKU configuration beside the camera layouts"),
            ("missing-camera-media-profiles", {"entries": {"VENDOR/etc/media_profiles_camsmall.xml": None}},
             "media_profiles_camsmall.xml is missing"),
            ("camera-media-profiles-edited", {"entries": {
                "VENDOR/etc/media_profiles_camlarge.xml": b"<MediaSettings/>"}},
             "media_profiles_camlarge.xml differs from what tools/camera_layouts.py generates"),
            ("camera-media-profiles-of-no-layout", {"entries": {
                "VENDOR/etc/media_profiles_camhuge.xml": b"<MediaSettings/>"}},
             "media profiles variant that no camera layout selects"),
            ("missing-camera-init", {"entries": {camera_init_path: None}},
             "init.kernarium_camera.rc is missing"),
            ("camera-init-edited", {"entries": {camera_init_path: camera_layout_files[camera_init_path].replace(
                b"_camsmall", b"_camlarge")}},
             "init.kernarium_camera.rc differs from what tools/camera_layouts.py generates"),
            ("camera-layout-manifest", {"entries": {"VENDOR/etc/vintf/manifest_bf-small.xml": b"<manifest/>"}},
             "VINTF manifest of a camera layout"),
            # The lazy provider and the HWL library of route p0030.
            ("missing-camera-provider", {"entries": {camera_provider_path: None}},
             "required entries are missing"),
            ("missing-camera-provider-rc", {"entries": {camera_rc_path: None}},
             "required entries are missing"),
            ("missing-camera-library", {"entries": {camera_library_path: None}},
             "required entries are missing"),
            ("missing-camera-template", {"entries": {camera_template_path: None}},
             "required entries are missing"),
            ("eager-camera-provider", {"entries": {
                "VENDOR/bin/hw/android.hardware.camera.provider@2.7-service-google": b"x"}},
             "only the lazy camera provider runs"),
            ("eager-camera-provider-rc", {"entries": {
                "VENDOR/etc/init/android.hardware.camera.provider@2.7-service-google.rc": b"# x\n"}},
             "only the lazy camera provider runs"),
            ("camera-provider-not-an-elf", {"entries": {camera_provider_path: b"not an ELF file"}},
             "is not an ELF file"),
            ("camera-library-not-an-elf", {"entries": {camera_library_path: b"not an ELF file"}},
             "is not an ELF file"),
            *((f"camera-library-without-{index}", {"entries": {camera_library_path: camera_library(
                *(other for other in good_camera_markers if other != marker))}},
              f"lacks {marker.decode()!r}") for index, marker in enumerate(good_camera_markers)),
            ("camera-provider-enabled", {"entries": {
                camera_rc_path: good_camera_rc.replace("    disabled\n", "").encode()}},
             "must carry 'disabled'"),
            ("camera-provider-not-oneshot", {"entries": {
                camera_rc_path: good_camera_rc.replace("    oneshot\n", "").encode()}},
             "must carry 'oneshot'"),
            ("camera-provider-without-interface", {"entries": {
                camera_rc_path: good_camera_rc.replace(
                    "    interface aidl android.hardware.camera.provider.ICameraProvider/internal/0\n", "").encode()}},
             "must carry 'interface aidl android.hardware.camera.provider.ICameraProvider/internal/0'"),
            ("camera-provider-wrong-user", {"entries": {
                camera_rc_path: good_camera_rc.replace("user system", "user root").encode()}},
             "must carry 'user system'"),
            ("camera-provider-wrong-binary", {"entries": {
                camera_rc_path: good_camera_rc.replace(camera_provider, "android.hardware.camera.provider@2.7-service-google").encode()}},
             "must define one service that runs the lazy provider"),
            ("camera-provider-triggered", {"entries": {
                camera_rc_path: (good_camera_rc + "\non boot\n    start vendor.camera-provider-2-7-google\n").encode()}},
             "may have no trigger"),
            ("camera-provider-service-renamed", {"entries": {
                camera_rc_path: good_camera_rc.replace("vendor.camera-provider-2-7-google", "vendor.camera-provider").encode()}},
             "which the init file of the camera layouts starts"),
            # The contract in the form that catches the defects of the launches that listed no camera: a start only at
            # boot, a start only in the profile action, a layout with cameras that lacks its action, an action on another
            # property than cameraserver's state, and a start anywhere else.
            ("camera-provider-started-at-boot-only", {"entries": {
                camera_provider_init_path: camera_init.replace("on property:init.svc.cameraserver=running && ", "on ").encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-started-in-the-profile-action", {"entries": {
                camera_init_path: camera_layout_files[camera_init_path].decode().replace(
                    f"on property:{camera_layouts.SKU_PROPERTY}=front\n",
                    f"on property:{camera_layouts.SKU_PROPERTY}=front\n    start {camera_service}\n", 1).encode()}},
             "init reads a vendor script in the vendor_init context"),
            ("camera-layout-without-its-provider-action", {"entries": {
                camera_provider_init_path: camera_init.replace(camera_start_action, "").encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-action-on-another-property", {"entries": {
                camera_provider_init_path: camera_init.replace(f"property:{camera_layouts.SERVER_PROPERTY}=running",
                                                      "property:sys.boot_completed=1").encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-action-without-the-sku", {"entries": {
                camera_provider_init_path: camera_init.replace(
                    f"on {camera_layouts.provider_trigger(camera_with_cameras[0])}\n",
                    f"on property:{camera_layouts.SERVER_PROPERTY}=running\n").encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-started-for-a-layout-without-cameras", {"entries": {
                camera_provider_init_path: (camera_init + f"\non {camera_layouts.provider_trigger('none')}\n"
                                   f"    {camera_layouts.PROVIDER_COMMAND} {camera_service}\n").encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-started-twice", {"entries": {
                camera_provider_init_path: camera_init.replace(
                    camera_start_action, camera_start_action + f"    {camera_layouts.PROVIDER_COMMAND} {camera_service}\n", 1).encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-started-not-restarted", {"entries": {
                camera_provider_init_path: camera_init.replace(
                    f"    {camera_layouts.PROVIDER_COMMAND} {camera_service}\n", f"    start {camera_service}\n").encode()}},
             "must restart the camera provider in one action for each layout"),
            ("camera-provider-init-file-missing", {"entries": {camera_provider_init_path: None}},
             "kernarium_camera_provider.rc is missing"),
            ("camera-provider-started-in-the-vendor-file", {"entries": {
                camera_init_path: (camera_layout_files[camera_init_path].decode() + camera_start_action).encode()}},
             "init reads a vendor script in the vendor_init context"),
            ("camera-provider-trigger-in-another-vendor-file", {"entries": {
                "VENDOR/etc/init/other.rc": camera_start_action.encode()}},
             "other.rc: the trigger 'on property:init.svc.cameraserver=running"),
            ("camera-provider-trigger-in-an-odm-file", {"entries": {
                "ODM/etc/init/other.rc": camera_start_action.encode()}},
             "other.rc: the trigger 'on property:init.svc.cameraserver=running"),
            ("vendor-trigger-on-a-private-service-status", {"entries": {
                "VENDOR/etc/init/other.rc": b"on property:init.svc.cameraserver=running\n    setprop a b\n"}},
             "labelled init_service_status_private_prop, which vendor_init may not read"),
            ("camera-provider-label", {"entries": {"VENDOR/etc/selinux/vendor_file_contexts":
                good_video_contexts.replace("hal_camera_default_exec", "vendor_file").encode()}},
             "hal_camera_default_exec label"),
            ("missing-camera-declaration", {"entries": {camera_fragment_path: None}},
             "must be declared exactly once"),
            ("camera-declared-twice", {"entries": {
                "VENDOR/etc/vintf/manifest.xml": good_camera_manifest.encode()}},
             "must be declared exactly once"),
            ("camera-declared-in-the-odm", {"entries": {
                camera_fragment_path: None, "ODM/etc/vintf/manifest.xml": good_camera_manifest.encode()}},
             "in the vendor"),
            ("camera-declared-for-another-instance", {"entries": {
                camera_fragment_path: good_camera_manifest.replace("internal/0", "external/0").encode()}},
             "must declare the AIDL ICameraProvider/internal/0"),
            ("camera-declared-as-hidl", {"entries": {
                camera_fragment_path: good_camera_manifest.replace('format="aidl"', 'format="hidl"').encode()}},
             "must declare the AIDL ICameraProvider/internal/0"),
            # The characteristics template, the properties and the vendor camera extensions.
            ("camera-template-not-json", {"entries": {camera_template_path: b"{"}},
             "cannot parse VENDOR/etc/config/emu_camera_v4l2.json"),
            ("camera-template-without-characteristics", {"entries": {camera_template_path: b"{}"}},
             "not a camera characteristics template"),
            ("stock-emulated-camera", {"entries": {"VENDOR/etc/config/emu_camera_back.json": b"{}"}},
             "a camera of the stock emulated HAL"),
            ("camera-extensions-declaration", {"entries": {
                "SYSTEM_EXT/etc/permissions/sample_camera_extensions.xml": b"<permissions/>"}},
             "is a vendor camera extension"),
            ("camera-extensions-library", {"entries": {
                "SYSTEM_EXT/framework/androidx.camera.extensions.impl.jar": b"jar"}},
             "is a vendor camera extension"),
            ("lazy-camera-property-missing", {"camera_variant": "lazy-property-missing"},
             "lacks ro.camera.enableLazyHal=true"),
            ("lazy-camera-property-off", {"camera_variant": "lazy-property-off"},
             "lacks ro.camera.enableLazyHal=true"),
            ("camera-config-property-missing", {"camera_variant": "config-property-missing"},
             "lacks ro.vendor.camera.config=v4l2"),
            ("camera-config-property-stock", {"camera_variant": "config-property-stock"},
             "lacks ro.vendor.camera.config=v4l2"),
            ("camera-extensions-property", {"camera_variant": "extensions-property"},
             "sets ro.camerax.extensions.enabled"),
            # The camera application and the camera switch.
            ("missing-camera-application", {"entries": {camera_app_path: None}},
             "required entries are missing"),
            ("camera-lens-launcher", {"entries": {
                "PRODUCT/app/ApertureLensLauncher/ApertureLensLauncher.apk": synthetic_apk()}},
             "unneeded built-in application payload is forbidden"),
            ("stock-camera-application", {"entries": {"PRODUCT/app/Camera2/Camera2.apk": synthetic_apk()}},
             "unneeded built-in application payload is forbidden"),
            # The lens settings are the product overlay's: the application's own table holds the defaults.
            ("camera-values-only-in-the-application", {"camera_variant": "app-values"},
             "Aperture__lineage_andarium__auto_generated_rro_product.apk is missing"),
            ("camera-overlay-missing", {"camera_variant": "overlay-missing"},
             "Aperture__lineage_andarium__auto_generated_rro_product.apk is missing"),
            ("camera-overlay-other-target", {"camera_variant": "overlay-target"}, "must be a static overlay of"),
            ("camera-overlay-not-static", {"camera_variant": "overlay-not-static"}, "must be a static overlay of"),
            ("camera-overlay-static-missing", {"camera_variant": "overlay-static-missing"},
             "must be a static overlay of"),
            ("camera-overlay-target-missing", {"camera_variant": "overlay-target-missing"},
             "must be a static overlay of"),
            ("camera-application-overlayable", {"camera_variant": "app-overlayable"},
             "declares an overlayable section"),
            ("motion-backend-without-orientation", {"entries": {
                "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so": synthetic_aarch64_elf()}},
             "lacks the device-orientation sensor"),
            # The marker counts only inside a library the image can load.
            ("motion-backend-marker-only", {"entries": {
                "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so": good_motion_sub_hal_text}},
             "is not an ELF file"),
            ("motion-backend-32-bit", {"entries": {
                "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so":
                    synthetic_arm_elf() + good_motion_sub_hal_text}},
             "32-bit ELF outside the 32-bit library and runtime paths"),
            ("motion-backend-corrupt", {
                "corrupt_member": "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so"},
             "cannot read VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so from the archive"),
            # Only the launch states whether the host feeds the sensor.
            ("image-sets-the-orientation-property", {"entries": {
                "SYSTEM_EXT/etc/build.prop": b"ro.boot.kernarium.orientation=1\n"}},
             "the launch alone states whether the host feeds the device-orientation sensor"),
            # The magnetometer and the compass feature are the host's to state: the launch passes
            # androidboot.kernarium.compass=1, and an image of the older kind lists neither.
            ("motion-backend-without-compass", {"entries": {
                "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so":
                    synthetic_aarch64_elf() + good_motion_orientation_text + b"\0"}},
             "lacks the property that decides whether it lists the magnetometer"),
            ("motion-backend-without-magnetometer", {"entries": {
                "VENDOR/lib64/hw/android.hardware.sensors@2.1-impl.andarium.so":
                    synthetic_aarch64_elf() + good_motion_orientation_text + b", ro.boot.kernarium.compass=%d\0"}},
             "lacks the magnetometer"),
            ("image-sets-the-compass-property", {"entries": {
                "SYSTEM_EXT/etc/build.prop": b"ro.boot.kernarium.compass=1\n"}},
             "the launch alone states whether the host has a magnetometer"),
            ("image-sets-the-compass-property-false", {"entries": {
                "ODM/etc/build.prop": b"ro.boot.kernarium.compass=0\n"}},
             "the launch alone states whether the host has a magnetometer"),
            ("missing-compass-feature", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml": None}},
             "android.hardware.sensor.compass.xml is missing: a host with a magnetometer would have no compass"),
            ("compass-feature-declares-another", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass"/>'
                    b'<feature name="android.hardware.sensor.barometer"/></permissions>'}},
             "declares .* not android.hardware.sensor.compass alone"),
            ("compass-feature-file-declares-nothing", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml":
                    b"<permissions/>"}},
             "declares \\[\\], not android.hardware.sensor.compass alone"),
            ("compass-directory-holds-another-file", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/other.xml": b"<permissions/>"}},
             "holds the compass feature alone"),
            # Declared anywhere else, every launch would claim a compass, including a host with none.
            ("compass-feature-in-the-vendor-permissions", {"entries": {
                "VENDOR/etc/permissions/android.hardware.sensor.compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass"/></permissions>'}},
             "the compass feature is declared outside VENDOR/etc/permissions/kernarium_compass"),
            ("compass-feature-in-the-product-permissions", {"entries": {
                "PRODUCT/etc/permissions/android.hardware.sensor.compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass"/></permissions>'}},
             "the compass feature is declared outside VENDOR/etc/permissions/kernarium_compass"),
            ("compass-feature-in-a-vendor-sku-directory", {"entries": {
                "VENDOR/etc/permissions/sku_haptics/android.hardware.sensor.compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass"/></permissions>'}},
             "the compass feature is declared outside VENDOR/etc/permissions/kernarium_compass"),
            # SystemConfig reads etc/sysconfig of every partition with the flags that allow features as well.
            ("compass-feature-in-the-system-sysconfig", {"entries": {
                "SYSTEM/etc/sysconfig/extra-compass.xml":
                    b'<config><feature name="android.hardware.sensor.compass"/></config>'}},
             "the compass feature is declared in the sysconfig file SYSTEM/etc/sysconfig/extra-compass.xml"),
            ("compass-feature-in-the-vendor-sysconfig", {"entries": {
                "VENDOR/etc/sysconfig/extra-compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass"/></permissions>'}},
             "the compass feature is declared in the sysconfig file VENDOR/etc/sysconfig/extra-compass.xml"),
            # The one file is the permissions root with the one feature, and nothing that takes a feature away.
            ("compass-feature-file-with-another-root", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml":
                    b'<config><feature name="android.hardware.sensor.compass"/></config>'}},
             "has the root element <config>, not <permissions>"),
            ("compass-feature-file-with-an-unavailable-feature", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass"/>'
                    b'<unavailable-feature name="android.hardware.sensor.light"/></permissions>'}},
             "holds an unavailable-feature element"),
            ("compass-feature-with-another-attribute", {"entries": {
                "VENDOR/etc/permissions/kernarium_compass/android.hardware.sensor.compass.xml":
                    b'<permissions><feature name="android.hardware.sensor.compass" notLowRam="true"/></permissions>'}},
             "is not one <feature name=...> in <permissions>"),
            ("init-sets-the-compass-property", {"entries": {
                "SYSTEM/etc/init/early-compass.rc":
                    b"on early-init\n    setprop ro.boot.kernarium.compass 1  # a host with a magnetometer\n"}},
             "early-compass.rc sets ro.boot.kernarium.compass"),
            ("init-sets-the-orientation-property", {"entries": {
                "SYSTEM/etc/init/early-orientation.rc":
                    b"on early-init\n    setprop ro.boot.kernarium.orientation 1\n"}},
             "early-orientation.rc sets ro.boot.kernarium.orientation"),
            # Init reads a command by its tokens: a quoted word and a continued line are the command they are.
            ("init-sets-the-quoted-compass-property", {"entries": {
                "SYSTEM/etc/init/quoted-property.rc":
                    b'on early-init\n    setprop "ro.boot.kernarium.compass" 1\n'}},
             "quoted-property.rc sets ro.boot.kernarium.compass"),
            ("init-quotes-setprop-itself", {"entries": {
                "SYSTEM/etc/init/quoted-command.rc":
                    b'on early-init\n    "setprop" ro.boot.kernarium.orientation 1\n'}},
             "quoted-command.rc sets ro.boot.kernarium.orientation"),
            ("init-continues-the-setprop-line", {"entries": {
                "SYSTEM/etc/init/continued.rc":
                    b"on early-init\n    setprop \\\n        ro.boot.kernarium.compass \\\n        1\n"}},
             "continued.rc sets ro.boot.kernarium.compass"),
            ("services-jar-without-the-compass-property", {"services_jar_variant": "no-compass-marker"},
             "lacks the SystemConfig topic that reads the compass feature for a host with a magnetometer"),
            ("services-jar-without-the-compass-directory", {"services_jar_variant": "no-compass-directory-marker"},
             "lacks the directory that SystemConfig topic reads"),
            ("camera-aux-cameras-off", {"camera_variant": "aux-off"}, "must set config_enableAuxCameras to true"),
            ("camera-aux-cameras-missing", {"camera_variant": "aux-missing"},
             "must set config_enableAuxCameras to true"),
            ("camera-aux-cameras-defined-twice", {"camera_variant": "aux-duplicate"},
             "must set config_enableAuxCameras to true"),
            ("camera-aux-cameras-only-qualified", {"camera_variant": "aux-only-qualified"},
             "defines config_enableAuxCameras only under a resource configuration"),
            ("camera-ignored-ids-only-qualified", {"camera_variant": "ignored-only-qualified"},
             "defines config_ignoredAuxCameraIds only under a resource configuration"),
            ("camera-application-lost-aux-cameras", {"camera_variant": "app-no-aux"},
             "does not define the bool resource config_enableAuxCameras in its default configuration"),
            ("camera-application-lost-ignored-ids", {"camera_variant": "app-no-ignored"},
             "does not define the array resource config_ignoredAuxCameraIds in its default configuration"),
            ("camera-application-aux-cameras-only-qualified", {"camera_variant": "app-aux-qualified"},
             "does not define the bool resource config_enableAuxCameras in its default configuration"),
            ("camera-application-not-a-resource-table", {"camera_variant": "app-not-a-table"},
             "Aperture.apk has no readable resource table: not a resource table"),
            ("camera-application-truncated-resource-table", {"camera_variant": "app-truncated-table"},
             "Aperture.apk has no readable resource table: not a resource table"),
            ("camera-ignored-ids-extra", {"camera_variant": "ignored-extra"},
             "must list the camera IDs"),
            ("camera-ignored-ids-empty", {"camera_variant": "ignored-none"}, "must list the camera IDs"),
            ("camera-ignored-ids-missing", {"camera_variant": "ignored-missing"}, "must list the camera IDs"),
            ("camera-zoom-on", {"camera_variant": "zoom-on"}, "must set config_enableZoom to false"),
            ("camera-zoom-missing", {"camera_variant": "zoom-missing"}, "must set config_enableZoom to false"),
            ("camera-zoom-defined-twice", {"camera_variant": "zoom-duplicate"},
             "must set config_enableZoom to false"),
            ("camera-zoom-only-qualified", {"camera_variant": "zoom-only-qualified"},
             "defines config_enableZoom only under a resource configuration"),
            ("camera-application-lost-zoom", {"camera_variant": "app-no-zoom"},
             "does not define the bool resource config_enableZoom in its default configuration"),
            ("camera-application-zoom-only-qualified", {"camera_variant": "app-zoom-qualified"},
             "does not define the bool resource config_enableZoom in its default configuration"),
            ("camera-toggle-off", {"camera_variant": "toggle-off"},
             "must set config_supportsHardwareCamToggle to true"),
            ("camera-toggle-missing", {"camera_variant": "toggle-missing"},
             "must set config_supportsHardwareCamToggle to true"),
            ("camera-toggle-defined-twice", {"camera_variant": "toggle-duplicate"},
             "must set config_supportsHardwareCamToggle to true"),
            ("camera-toggle-only-qualified", {"camera_variant": "toggle-only-qualified"},
             "defines config_supportsHardwareCamToggle only under a resource configuration"),
            ("camera-reminder-activity-defined", {"camera_variant": "reminder-string"},
             "defines config_sensorUseStartedActivity in the default configuration"),
            ("camera-reminder-hardware-activity-defined", {"camera_variant": "reminder-hardware-string"},
             "defines config_sensorUseStartedActivity_hwToggle in the default configuration"),
            ("camera-reminder-activity-defined-by-reference", {"camera_variant": "reminder-reference"},
             "defines config_sensorUseStartedActivity_hwToggle in the default configuration"),
            ("camera-reminder-activity-defined-under-a-qualifier", {"camera_variant": "reminder-qualified"},
             "defines config_sensorUseStartedActivity under a resource configuration"),
            ("camera-reminder-activities-mapped-by-a-resource-map", {"camera_variant": "reminder-resource-map"},
             "declares a resource map"),
            ("services-jar-without-the-sensor-reminder-topic", {"services_jar_variant": "no-sensor-reminder-marker"},
             "lacks the SensorPrivacyService topic that withholds the reminder of a camera under the hardware toggle"),
        ):
            reject(label, options, reason)

        # What the checks accept besides the baseline: the grant to every application domain by its
        # own rule and not through the attribute, and the command's code preoptimized beside a jar
        # that holds none.
        system_ext_policy_entry = "SYSTEM_EXT/etc/selinux/system_ext_sepolicy.cil"
        for label, options, appended in (
            ("oldapp-per-domain-reads", {"oldswitch_variant": "per-domain-reads"}, b""),
            ("oldapp-preoptimized-code", {"oldswitch_variant": "preopt-code"}, b""),
            # The holder's right to listen on its own handoff socket, spelt with its own name as the target.
            ("sms-handoff-holder-listens-by-its-own-name", {"sms_policy_variant": "handoff-holder-listens-by-its-own-name"}, b""),
            # The built text message application's manifest carries the <uses-sdk> the build injects, with the
            # platform's levels as numbers (the baseline) or as codenames, and a maximum level.
            ("sms-uses-sdk-codenames", {"sms_service_variant": "apk-uses-sdk-codenames"}, b""),
            ("sms-uses-sdk-max", {"sms_service_variant": "apk-uses-sdk-max"}, b""),
            # A byte that is no UTF-8 in a rule that no check looks at (a type of no check, a
            # permission none compares) is no reason to refuse the policy: the checks decode what
            # they compare, when they compare it.
            ("oldapp-unrelated-non-utf8-rule", {}, b"(neverallow zz_unrelated foo (property_service (set \xff)))\n"),
        ):
            fixture = root / f"good-{label}.zip"
            write_fixture(fixture, **options)
            if appended:
                with zipfile.ZipFile(fixture) as written:
                    policy = written.read(system_ext_policy_entry)
                adjust_archive(fixture, {system_ext_policy_entry: policy + appended})
            result = run_in_process(tool, "--target-files", fixture, *kernel_options)
            if result.returncode != 0:
                fail(f"target-files verifier rejected the {label} fixture: {result.stdout}")

        for index, case in enumerate(rejections.values()):
            label = "/".join(case["labels"])
            fixture = root / f"bad-{case['labels'][0]}.zip"
            write_fixture(fixture, **case["options"])
            signed_option = ("--signed",) if case["options"].get("signed") else ()
            result = run_in_process(tool, "--target-files", fixture, *signed_option, *kernel_options)
            if index == 0:
                # The harness answers as the script does: the first case also
                # runs as a process.
                process = run_tool(verifier, "--target-files", fixture, *signed_option, *kernel_options)
                if (process.returncode, process.stdout) != (result.returncode, result.stdout):
                    fail(
                        f"the in-process run of the {label} fixture differs from the "
                        f"script's: {result.returncode} {result.stdout!r} against "
                        f"{process.returncode} {process.stdout!r}"
                    )
            if result.returncode == 0:
                fail(f"target-files verifier accepted the {label} fixture")
            if result.returncode != 1 or (
                case["message"]
                and "Android target-files verifier: " not in result.stdout
            ):
                fail(f"target-files verifier did not reject the {label} fixture: {result.stdout}")
            for reason in case["reasons"]:
                if not re.search(reason, result.stdout):
                    fail(
                        f"target-files verifier did not reject the {label} fixture "
                        f"for {reason!r}: {result.stdout}"
                    )


def verify_net_setup_tool(android_root: Path) -> None:
    """RTM_NEWLINK with a flag mask replaces the new link's flags with ifi_flags, and with none
    given that clears IFF_MULTICAST (ether_setup sets it), which the IPv6 neighbour and router
    discovery of buried_eth0 and wlan0 need. The tool leaves ifi_change at 0; the library's
    AddIfInfo sets IFF_UP."""
    source = (android_root / "overlay/tools/kernarium_net_setup.cpp").read_text()
    if re.search(r"\bifi_change\s*=", source):
        fail("kernarium_net_setup.cpp assigns ifi_change: a link-creation flag mask clears IFF_MULTICAST")
    verify_net_setup_ipv6(source)


def verify_net_setup_ipv6(source: str) -> None:
    """eth0 and the macvlan carry no IPv6, so no link-local address is built from their MACs. The
    tool turns it off on eth0 once it exists and on the macvlan before that comes up, and a failure
    to do so is fatal."""
    for name in ("kLowerName", "kMacvlanName"):
        if not re.search(rf"if \(!DisableIpv6\({name}\)\) \{{[^}}]*return 1;", source):
            fail(f"kernarium_net_setup.cpp does not turn IPv6 off on {name} and fail when it cannot")
    macvlan = source.find("DisableIpv6(kMacvlanName)")
    bring_up = source.find("BringUp(*netlink, macvlan_index)")
    if not 0 < macvlan < bring_up:
        fail("kernarium_net_setup.cpp turns IPv6 off on the macvlan after it brings the macvlan up")


def verify_erofs_board(android_root: Path) -> None:
    """The five read-only partitions are compressed EROFS from one pair of parameters.

    The compressor and the cluster size are written once, in the product makefile; the board
    file hands them on, and neither it nor the build script repeats a value. The fstabs mount the
    partitions as EROFS, read-only, and the hints keep every .apex uncompressed."""
    compressor, pcluster_size = tracked_erofs_parameters(android_root)
    if not compressor.startswith("zstd,") or int(pcluster_size) % 4096 != 0:
        fail(f"unexpected EROFS parameters: {compressor} {pcluster_size}")
    board = (android_root / "overlay/BoardConfig.mk").read_text()
    for partition in ("SYSTEM", "SYSTEM_EXT", "PRODUCT", "VENDOR", "ODM"):
        line = f"BOARD_{partition}IMAGE_FILE_SYSTEM_TYPE := erofs\n"
        if board.count(line) != 1:
            fail(f"BoardConfig.mk does not select erofs for {partition} exactly once")
    for line in (
        "BOARD_EROFS_COMPRESSOR := $(UML_ANDROID_EROFS_COMPRESSOR)\n",
        "BOARD_EROFS_PCLUSTER_SIZE := $(UML_ANDROID_EROFS_PCLUSTER_SIZE)\n",
        "BOARD_EROFS_COMPRESS_HINTS := device/kernarium/andarium/config/erofs_compress_hints.txt\n",
    ):
        if board.count(line) != 1:
            fail(f"BoardConfig.mk does not contain {line.strip()!r} exactly once")
    for repeated in (compressor, pcluster_size):
        for name in ("overlay/BoardConfig.mk", "build/build-aosp.sh"):
            if re.search(rf"(?<![0-9A-Za-z_]){re.escape(repeated)}(?![0-9A-Za-z_])",
                         (android_root / name).read_text()):
                fail(f"{name} repeats the EROFS parameter {repeated}")
    for name, flags in (
        ("overlay/config/fstab.uml", "wait,first_stage_mount"),
        ("overlay/config/recovery.fstab", "wait"),
    ):
        text = (android_root / name).read_text()
        for partition in ("system", "system_ext", "product", "vendor", "odm"):
            expected = f"/dev/block/by-name/{partition} /{partition} erofs ro {flags}\n"
            if text.count(expected) != 1:
                fail(f"{name} does not mount {partition} as '{expected.strip()}'")
    hints = (android_root / "overlay/config/erofs_compress_hints.txt").read_text()
    if [line for line in hints.splitlines() if line and not line.startswith("#")] != [
        "0 .*\\.apex$"
    ]:
        fail("erofs_compress_hints.txt is not the single rule that stores .apex uncompressed")
    payload_hints = (android_root / "overlay/config/erofs_apex_payload_hints.txt").read_text()
    if [line for line in payload_hints.splitlines() if line and not line.startswith("#")] != [
        "0 .*\\.rc$",
        "0 .*\\.xml$",
        "0 .*bin/adbd$",
    ]:
        fail(
            "erofs_apex_payload_hints.txt is not the rules that store init scripts, VINTF "
            "fragments and the adb daemon uncompressed"
        )


def verify_userdata_grow(android_root: Path) -> None:
    """The product grows userdata online, on every boot, once /data is mounted.

    An offline resize2fs refuses an unmounted filesystem mounted after its last full check, and
    mount_all's e2fsck skips a clean one, so it works on the first boot and fails on every later
    one. The kernel's online resize needs neither; the product's own init script runs it at
    post-fs-data, and the fsck domain, which runs resize2fs, holds exactly the rules it needs."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    script = (android_root / "overlay/config/init.vendor.rc").read_text()
    verifier.check_userdata_grow(verifier.init_actions(script))
    offline = script.replace(
        "    exec - root system -- /system/bin/resize2fs /dev/block/by-name/userdata\n",
        "    exec -- /system/bin/resize2fs /dev/block/by-name/userdata\n",
    )
    if offline == script:
        fail("init.vendor.rc does not contain the online resize2fs this guard looks for")
    try:
        verifier.check_userdata_grow(verifier.init_actions(offline))
    except SystemExit:
        pass
    else:
        fail("a resize2fs without root's group passes the userdata grow check")
    # The overlay's fsck rules, in the CIL form the vendor policy has them in.
    rules = (android_root / "overlay/sepolicy/vendor/fsck.te").read_text()
    for needle in (
        "allow fsck self:capability sys_resource;",
        "allow fsck system_data_root_file:dir { open read ioctl };",
        "allowxperm fsck system_data_root_file:dir ioctl 0x6610;",
    ):
        if needle not in rules:
            fail(f"overlay/sepolicy/vendor/fsck.te does not contain '{needle}'")
    for line in rules.splitlines():
        if line.startswith("allow") and "fsck_untrusted" in line:
            fail("overlay/sepolicy/vendor/fsck.te grants the untrusted fsck domain something")
    policy = (
        b"(allow fsck_202604 self (capability (sys_resource)))\n"
        b"(allow fsck_202604 system_data_root_file_202604 (dir (ioctl read open)))\n"
        b"(allowx fsck_202604 system_data_root_file_202604 (ioctl dir (0x6610)))\n"
    )
    verifier.check_userdata_grow_policy(policy)


def verify_zram_fstab(android_root: Path) -> None:
    """The fstab names one zram swap entry that sizes itself from the RAM; init turns it on at boot completion."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    fstab = (android_root / "overlay/config/fstab.uml").read_bytes()
    verifier.check_zram_fstab(fstab, "tracked")
    entry = b"/dev/block/zram0 none swap defaults zramsize=50%\n"
    if entry not in fstab:
        fail("overlay/config/fstab.uml does not contain the zram swap entry this guard looks for")
    for label, edited in (
        ("no swap entry", fstab.replace(entry, b"")),
        ("wrong device", fstab.replace(b"/dev/block/zram0", b"/dev/block/zram1")),
        ("no size", fstab.replace(b"zramsize=50%", b"max_comp_streams=4")),
        ("absolute size", fstab.replace(b"zramsize=50%", b"zramsize=768M")),
        ("two entries", fstab + entry),
    ):
        try:
            verifier.check_zram_fstab(edited, label)
        except SystemExit:
            continue
        fail(f"a fstab with {label} passes the zram swap check")


def verify_product_identity(android_root: Path) -> None:
    """The identity is checked where the product keeps it, not in the generic system partition.

    The system partition is AOSP's generic system (generic_system.mk) and reports Android /
    generic / Generic System; the product and vendor partitions and the public fingerprint
    carry the Andarium identity, which is what Build.* reports."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    identity = verifier.PRODUCT_IDENTITY
    if identity["brand"] != "Andarium" or identity["model"] != "Andarium Virtual Device":
        fail(f"the product identity is {identity}")
    generic = {
        "ro.product.system.brand": "Android",
        "ro.product.system.device": "generic",
        "ro.product.system.manufacturer": "Android",
        "ro.product.system.model": "Generic System",
        "ro.product.system.name": "generic_system",
        "ro.system.build.fingerprint":
            "Android/generic_system/generic:17/CP2A.260605.016/eng.build:user/test-keys",
        "ro.build.fingerprint":
            "Andarium/lineage_andarium/andarium:17/CP2A.260605.016/eng.build:user/test-keys",
    }
    product = {f"ro.product.product.{key}": value for key, value in identity.items()}
    # The vendor partition states the build's identity too (ro.vendor.build.*), the same as the
    # public fingerprint: a vendor image that kept an earlier build's says so in these properties.
    vendor_build = {
        "ro.vendor.build.fingerprint": generic["ro.build.fingerprint"],
        "ro.vendor.build.type": "user",
        "ro.vendor.build.tags": "test-keys",
        "ro.vendor.build.id": "CP2A.260605.016",
        "ro.vendor.build.version.incremental": "eng.build",
    }
    vendor_product = {f"ro.product.vendor.{key}": value for key, value in identity.items()}
    vendor = dict(vendor_product, **vendor_build)
    verifier.check_product_identity(generic, product, vendor, "user")
    # The generic system's own identity is not constrained: it may also carry the product's.
    verifier.check_product_identity(
        dict(generic, **{f"ro.product.system.{key}": value for key, value in identity.items()}),
        product,
        vendor,
        "user",
    )
    for label, system, product_map, vendor_map, variant in (
        ("a wrong product model", generic,
         dict(product, **{"ro.product.product.model": "Generic System"}), vendor, "user"),
        ("a wrong product brand", generic,
         dict(product, **{"ro.product.product.brand": "Android"}), vendor, "user"),
        ("no product identity", generic, {}, vendor, "user"),
        ("a wrong vendor device", generic, product,
         dict(vendor, **{"ro.product.vendor.device": "generic"}), "user"),
        ("no vendor identity", generic, product, {}, "user"),
        ("no vendor build identity", generic, product, vendor_product, "user"),
        ("the vendor build identity of a canary build", generic, product, dict(vendor, **{
            "ro.vendor.build.fingerprint":
                "Andarium/lineage_andarium/andarium:17/CP2A.260605.016/eng.build:userdebug/test-keys",
            "ro.vendor.build.type": "userdebug"}), "user"),
        ("a vendor build id of another build", generic, product,
         dict(vendor, **{"ro.vendor.build.id": "CP2A.250101.001"}), "user"),
        ("a vendor build number of another build", generic, product,
         dict(vendor, **{"ro.vendor.build.version.incremental": "2026110100"}), "user"),
        ("a vendor build signed with other keys", generic, product,
         dict(vendor, **{"ro.vendor.build.tags": "release-keys"}), "user"),
        ("the generic system's fingerprint", dict(generic, **{
            "ro.build.fingerprint": generic["ro.system.build.fingerprint"]}),
         product, vendor, "user"),
        ("no public fingerprint", {key: value for key, value in generic.items()
                                   if key != "ro.build.fingerprint"},
         product, vendor, "user"),
        ("a fingerprint of the other build type", generic, product, vendor, "userdebug"),
    ):
        try:
            verifier.check_product_identity(system, product_map, vendor_map, variant)
        except SystemExit:
            continue
        fail(f"a build with {label} passes the product identity check")


def odm_properties(android_root: Path) -> dict[str, str]:
    """The properties of the overlay's odm property file, which TARGET_ODM_PROP hands to the build."""
    properties: dict[str, str] = {}
    for line in (android_root / "overlay/config/odm.prop").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            key, separator, value = line.partition("=")
            if not separator or key in properties:
                fail(f"odm.prop has a line that is not a new key=value: {line!r}")
            properties[key] = value
    return properties


def verify_hardware_identity(android_root: Path) -> None:
    """The board is the device's and the SoC is the virtual CPU, stated where the build reads them.

    The inherited Cuttlefish product states Google and its device name as the SoC, in the vendor
    properties, and names its board cutf. The product drops the vendor values with the denylist,
    states its own in an odm property file (a make list cannot carry a value with a space) and
    renames the board; the verifier requires the result in the image."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    soc = {"ro.soc.manufacturer": "Kernarium", "ro.soc.model": "Virtual CPU"}
    if verifier.SOC_IDENTITY != soc or verifier.BOARD_NAME != "andarium":
        fail(f"the verifier expects {verifier.SOC_IDENTITY} on board {verifier.BOARD_NAME}")
    if odm_properties(android_root) != soc:
        fail(f"overlay/config/odm.prop sets {odm_properties(android_root)}, not {soc}")
    board = (android_root / "overlay/BoardConfig.mk").read_text()
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    for text, name, line in (
        (board, "BoardConfig.mk", "TARGET_BOOTLOADER_BOARD_NAME := andarium\n"),
        (board, "BoardConfig.mk", "TARGET_ODM_PROP += device/kernarium/andarium/config/odm.prop\n"),
        (product, "lineage_andarium.mk",
         "PRODUCT_VENDOR_PROPERTY_BLACKLIST += ro.soc.manufacturer ro.soc.model\n"),
    ):
        if text.count(line) != 1:
            fail(f"{name} does not contain {line.strip()!r} exactly once")
    if "TARGET_ODM_PROP :=" in board or "TARGET_BOOTLOADER_BOARD_NAME +=" in board:
        fail("BoardConfig.mk assigns the odm property list or the board name in another way")
    # The vendor properties of the product must not state the SoC again.
    if re.search(r"^\s*ro\.soc\.\w+\s*=", product, re.MULTILINE):
        fail("lineage_andarium.mk assigns a ro.soc property in a list that the denylist empties")
    script = (android_root / "build/build-aosp.sh").read_text()
    for needle in ("get_build_var TARGET_BOOTLOADER_BOARD_NAME", "get_build_var TARGET_ODM_PROP"):
        if needle not in script:
            fail(f"build-aosp.sh does not check the build variable: {needle}")

    # What CTS' BuildTest accepts, and the values the product sets pass; the helper names why not.
    manufacturer, model = verifier.SOC_PATTERNS["ro.soc.manufacturer"], verifier.SOC_PATTERNS["ro.soc.model"]
    for value, pattern in (("Kernarium", manufacturer), ("Virtual CPU", model), ("SM8550", model),
                           ("a.b_c/d+e-f 1", model), ("andarium", verifier.BOARD_PATTERN)):
        problems = verifier.cts_build_value_problems("x", value, pattern)
        if problems:
            fail(f"CTS would accept {value!r}, the helper says {problems}")
    for value, pattern, reason in (
        (" Kernarium", manufacturer, "surrounding whitespace"),
        ("Kernarium ", manufacturer, "surrounding whitespace"),
        ("Kernarium, Inc.", manufacturer, "does not match"),
        ("Kernarium_Inc", manufacturer, "does not match"),
        ("", manufacturer, "does not match"),
        ("unknown", manufacturer, "is unknown"),
        ("Unknown", model, "is unknown"),
        ("Virtual CPU ", model, "surrounding whitespace"),
        ("Virtual:CPU", model, "does not match"),
        ("Virtual\tCPU", model, "does not match"),
        ("Virtual CPU \u00e9", model, "does not match"),
        ("two words", verifier.BOARD_PATTERN, "does not match"),
    ):
        if not any(reason in problem for problem in verifier.cts_build_value_problems("x", value, pattern)):
            fail(f"the CTS value check accepts {value!r}, which {reason}")

    odm = dict(soc)
    vendor = {"ro.product.board": "andarium"}
    verifier.check_hardware_identity(odm, vendor)
    # An inherited key with a comment sign is not a property; a value the vendor drops is absent.
    verifier.check_hardware_identity(odm, dict(vendor, **{"#ro.soc.model": "vsoc_arm64_pgagnostic"}))
    for label, odm_map, vendor_map in (
        ("no SoC identity", {}, vendor),
        ("the Cuttlefish SoC", {"ro.soc.manufacturer": "Google", "ro.soc.model": "vsoc_arm64_pgagnostic"}, vendor),
        ("a manufacturer of a real chip vendor", dict(odm, **{"ro.soc.manufacturer": "Qualcomm"}), vendor),
        ("the joined model", dict(odm, **{"ro.soc.model": "Kernarium Virtual CPU"}), vendor),
        ("a SoC in the vendor partition", odm, dict(vendor, **{"ro.soc.model": "Virtual CPU"})),
        ("another SoC property in the vendor partition", odm, dict(vendor, **{"ro.soc.et": "true"})),
        ("the Cuttlefish board", odm, {"ro.product.board": "cutf"}),
        ("no board", odm, {}),
    ):
        try:
            verifier.check_hardware_identity(odm_map, vendor_map)
        except SystemExit:
            continue
        fail(f"a build with {label} passes the hardware identity check")
    # The expected values are checked against CTS too: a value CTS would refuse fails the image.
    original = verifier.SOC_IDENTITY
    try:
        verifier.SOC_IDENTITY = {"ro.soc.manufacturer": "Kernarium", "ro.soc.model": "unknown"}
        try:
            verifier.check_hardware_identity(dict(verifier.SOC_IDENTITY), vendor)
        except SystemExit:
            pass
        else:
            fail("an expected SoC model of 'unknown' passes the hardware identity check")
    finally:
        verifier.SOC_IDENTITY = original


def verify_host_cpu_name(root: Path, android_root: Path) -> None:
    """The image and the host name the same virtual CPU.

    The host gives its CPU one name (darwin/cpu/guest_identity.hpp): an x86-64 guest reads it as
    the CPUID brand string and the /proc/cpuinfo model name, and every kernel's SMBIOS tables
    carry it. The Android image states it the way a chip vendor does, as a maker and a chip
    (ro.soc.manufacturer and ro.soc.model), which a space joins; the pair must not drift from the
    host's name."""
    header = (root / "darwin/cpu/guest_identity.hpp").read_text()
    names = re.findall(r'guest_cpu_model_name_storage\[\]\s*=\s*"([^"\\]*)"\s*;', header)
    if len(names) != 1:
        fail(f"guest_identity.hpp does not define the CPU name exactly once: {names}")
    host = names[0]
    if not host or len(host) >= 48:
        fail(f"the host CPU name {host!r} does not fit a CPUID brand string")
    soc = odm_properties(android_root)
    image = soc["ro.soc.manufacturer"] + " " + soc["ro.soc.model"]
    if image != host:
        fail(f"the image names its CPU {image!r} (overlay/config/odm.prop), the host {host!r}")
    expected = load_tool(android_root / "tools/verify-target-files.py").SOC_IDENTITY
    if expected["ro.soc.manufacturer"] + " " + expected["ro.soc.model"] != host:
        fail(f"the verifier expects a CPU other than the host's {host!r}: {expected}")


def verify_wifi_network(android_root: Path) -> None:
    """The tracked Wi-Fi network script, its init file and the module's overlay meet the rules the
    verifier holds the built image to, and the overlay sets exactly the thresholds it requires."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    tools = android_root / "overlay/tools"
    verifier.check_wifi_network(
        (tools / "kernarium_wifi_network.rc").read_text(encoding="utf-8"),
        (tools / "kernarium_wifi_network.sh").read_bytes(),
    )
    module = android_root / "overlay/rro/KernariumWifiOverlay"
    thresholds = {
        element.get("name"): int(element.text)
        for element in ET.parse(module / "res/values/config.xml").getroot().iter("integer")
    }
    for name in verifier.WIFI_NO_INTERNET_THRESHOLDS:
        if thresholds.get(name) != verifier.WIFI_NEVER_DISABLED_THRESHOLD:
            fail(
                f"the Wi-Fi overlay sets {name} to {thresholds.get(name)}, not "
                f"{verifier.WIFI_NEVER_DISABLED_THRESHOLD}"
            )
    # The marker holds 1 and 2: the verifier's reading of a property type accepts both for an
    # enum that lists them and rejects 2 for the types that cannot hold it.
    accepts = verifier.property_type_accepts
    for tokens, value, expected in (
        (["exact", "enum", "1", "2"], "2", True),
        (["exact", "enum", "1"], "2", False),
        (["exact", "bool"], "2", False),
        (["exact", "bool"], "1", True),
        (["exact", "string"], "2", True),
        (["exact", "int"], "2", True),
        (["exact", "uint"], "-1", False),
    ):
        if accepts(tokens, value) is not expected:
            fail(f"the verifier reads {' '.join(tokens)} as {'not ' * expected}accepting {value}")
    # The script's waits and loops are bounded: nothing in it waits for ever. This is a presence pin of the
    # structure. What the script decides, and when it sets the marker, is not judged from its text: the
    # android17-wifi-network-script test runs this tracked file on a fake Wi-Fi framework, and Soong copies
    # it into the image unchanged (overlay/tools/Android.bp).
    script = (tools / "kernarium_wifi_network.sh").read_text(encoding="utf-8")
    code = "\n".join(line for line in script.splitlines() if not line.lstrip().startswith("#"))
    for needle in ("settle=", "looks=", 'wait_joined "$settle"', '"$waited" -ge "$1"', '"$tries" -lt 30',
                   '"$seen" -lt "$looks"', 'timeout "$limit" cmd wifi list-networks',
                   'timeout "$dumplimit" dumpsys -t'):
        if needle not in code:
            fail(f"the Wi-Fi network script no longer bounds its wait: {needle!r} is gone")


def verify_pinner_overlay(android_root: Path) -> None:
    """The product releases the home app's memory pin with an overlay built for SDK 28.

    The pin sizes are not overlayable, so the overlay applies only while it targets 28 or lower;
    the verifier reads the built manifest, and these checks keep its reader and the module's
    sources in step."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    module = android_root / "overlay/rro/KernariumPinnerOverlay"
    values = re.findall(
        r'<integer name="([a-zA-Z_]+)">([^<]*)</integer>',
        (module / "res/values/config.xml").read_text(),
    )
    if values != [("config_pinnerHomePinBytes", "0")]:
        fail(f"the pinner overlay sets {values}, not the home pin size alone and to zero")
    if verifier.PINNER_OVERLAY_RESOURCE.decode() != values[0][0]:
        fail("the verifier looks for a different pin resource than the overlay sets")
    manifest = (module / "AndroidManifest.xml").read_text()
    target = re.search(r'android:targetSdkVersion="(\d+)"', manifest)
    if target is None or int(target.group(1)) > verifier.PINNER_OVERLAY_MAX_TARGET_SDK:
        fail("the pinner overlay manifest does not target SDK 28 or lower")
    for needle in ('android:targetPackage="android"', 'android:isStatic="true"'):
        if needle not in manifest:
            fail(f"the pinner overlay manifest lacks {needle}")
    blueprint = (module / "Android.bp").read_text()
    for needle in ('name: "KernariumPinnerOverlay"', "vendor: true,", 'sdk_version: "28",'):
        if needle not in blueprint:
            fail(f"the pinner overlay module lacks {needle}")
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    if len(re.findall(r"^PRODUCT_PACKAGES \+= KernariumPinnerOverlay$", product, re.M)) != 1:
        fail("lineage_andarium.mk does not add KernariumPinnerOverlay to PRODUCT_PACKAGES exactly once")
    if verifier.PINNER_OVERLAY != "VENDOR/overlay/KernariumPinnerOverlay.apk":
        fail("the verifier expects the pinner overlay somewhere other than the vendor overlay directory")

    for label, blob, expected in (
        ("SDK 28", synthetic_compiled_manifest(28), 28),
        ("SDK 37", synthetic_compiled_manifest(37), 37),
        ("no attribute", synthetic_compiled_manifest(None), None),
        ("a name", synthetic_compiled_manifest(28, string_value=True), None),
    ):
        if verifier.manifest_target_sdk(blob) != expected:
            fail(f"the manifest reader does not read {label} as {expected}")
    request, declaration = ("android.permission.ENTER_TRADE_IN_MODE",), ("android.permission.BACKUP",)
    for utf8 in (False, True):
        encoding = "UTF-8" if utf8 else "UTF-16"
        for label, blob, expected in (
            ("none", synthetic_compiled_manifest(28, utf8=utf8), set()),
            ("one", synthetic_compiled_manifest(28, permissions=request, utf8=utf8), set(request)),
            ("two", synthetic_compiled_manifest(None, permissions=request + declaration, utf8=utf8),
             set(request + declaration)),
            ("a declaration", synthetic_compiled_manifest(28, declared=request, utf8=utf8), set()),
        ):
            if verifier.manifest_requested_permissions(blob) != expected:
                fail(f"the manifest reader does not read {label} in a {encoding} pool as {expected}")
    good = synthetic_compiled_manifest(28)
    for label, blob in (
        ("empty", b""),
        ("not compiled XML", b"<manifest/>" + bytes(8)),
        ("truncated", good[:-6]),
    ):
        for reader in (verifier.manifest_target_sdk, verifier.manifest_requested_permissions):
            try:
                reader(blob)
            except (ValueError, struct.error):
                continue
            fail(f"the manifest reader accepts a {label} manifest")


def secondary_zygote_preloader_problems(added: str) -> list[str]:
    """What is wrong with the added text of the topic that preloads the secondary zygote on demand.

    system_server waits for the preload task, and the stock task catches everything, so a failure
    of the package lookup must fall back to the preload instead of reaching the task's future:
    that rethrows into startOtherServices and restarts system_server."""

    def body(signature: str) -> str:
        start = added.find(signature)
        if start < 0:
            return ""
        depth = 0
        for end in range(added.index("{", start), len(added)):
            depth += added[end] == "{"
            depth -= added[end] == "}"
            if depth == 0:
                return added[start : end + 1]
        return ""

    problems = []
    run, check, preload = body("void run()"), body("private void check("), body("private void preload()")
    for name, text in (("run", run), ("check", check), ("preload", preload)):
        if not text:
            problems.append(f"the topic has no {name}()")
    if run and not re.search(r"catch \(Exception \w+\) \{[^}]*\bpreload\(\);", run):
        problems.append("run() does not fall back to the preload when the package lookup throws")
    if check:
        if not re.search(r"catch \(Exception \w+\) \{[^}]*\bneeded = true;", check):
            problems.append("check() does not fall back to the preload when the lookup throws")
        if "getPackageStateInternal(" not in check or "getPackageStates(" in check:
            problems.append("check() must read the one package's state, not copy every state")
    if preload and not re.search(r"try \{\s*mPackageManager\.removePackageListObserver\(this\);\s*\} catch \(Exception", preload):
        problems.append("preload() lets a failure to remove the package observer skip the preload")
    return problems


def verify_secondary_zygote_topic(android_root: Path) -> None:
    """The frameworks/base topic that preloads the secondary zygote on demand cannot fail system_server."""
    patches = sorted((android_root / "patches/projects/p0007/series").glob("zygote-preload-the-secondary-zygote-*.patch"))
    if len(patches) != 1:
        fail(f"p0007 has {len(patches)} secondary zygote preload patches, not one")
    text = patches[0].read_text()
    added = "\n".join(line[1:] for line in text.splitlines() if line.startswith("+") and not line.startswith("+++"))
    problems = secondary_zygote_preloader_problems(added)
    if problems:
        fail("the secondary zygote preload topic: " + "; ".join(problems))
    for label, edited in (
        ("run() without its catch", re.sub(r"\} catch \(Exception ex\) \{\n\s*Slog\.e\(TAG, \"Unable to tell whether a package", "} finally { Slog.e(TAG, \"Unable to tell whether a package", added, count=1)),
        ("check() that copies every state", added.replace("getPackageStateInternal(packageName)", "getPackageStates().get(packageName)")),
        ("check() without its fallback", added.replace("needed = true;", "needed = false;")),
        ("preload() that skips on a failed removal", added.replace("try {\n                mPackageManager.removePackageListObserver(this);\n            } catch (Exception ex) {", "{\n                mPackageManager.removePackageListObserver(this);\n            } if (false) {")),
    ):
        if edited == added or not secondary_zygote_preloader_problems(edited):
            fail(f"the secondary zygote preload check accepts {label}")


DECODE_DELAYS_HEADER = "components/include/v4l2_codec2/components/DecodeDelays.h"
DECODE_INTERFACE_SOURCE = "components/DecodeInterface.cpp"
DECODE_INTERFACE_HEADER = "components/include/v4l2_codec2/components/DecodeInterface.h"
DECODE_COMPONENT_SOURCE = "v4l2/V4L2DecodeComponent.cpp"
DECODE_FACTORY_SOURCE = "v4l2/V4L2ComponentFactory.cpp"
DECODE_README = "README.md"

# What the framework does with the two delays, as the sum of them and the stall rule: the framework reads both once
# with one query when it starts, sizes the capture queue to the output delay plus 4, and stops feeding the decoder once
# the works that wait for an output, with their input released, reach the pipeline delay plus the output delay plus 4,
# which stalls a stream whose reorder depth R is that limit or more (it plays while R <= pipeline + output + 3, as
# the edge clips of the cand32 image measured). The model runs the header's own function.
DECODE_DELAY_MODEL = r"""
#include <cstdint>
#include <cstdio>
#include "DecodeDelays.h"
using namespace android;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAILED line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)
static bool stalls(DecodeDelays d, unsigned depth) { return depth >= d.pipeline + d.output + 4; }
static unsigned captureSlots(DecodeDelays d) { return d.output + 4; }
struct Size { uint32_t w, h; };
int main() {
    const Size sizes[] = {{0, 0}, {320, 240}, {1280, 720}, {1920, 1080}, {1920, 1088}, {1920, 1089}, {2048, 1080},
                          {2560, 1440}, {3840, 2160}, {7680, 4320}, {0xFFFFFFFFu, 0xFFFFFFFFu}};
    for (Size c : sizes) {
        const DecodeDelays d = getReorderingDelays(c.w, c.h);
        CHECK(d.output + d.pipeline == kReorderingDelaySum);
        for (unsigned depth = 0; depth <= 16; ++depth) CHECK(!stalls(d, depth));
    }
    // The size configured and the size the stream's SPS declares on the two sides of the line: the framework sizes
    // its pools from what it read at the start (the configured size), and a later read from the declared size gives
    // another split of the same sum, so no depth stalls with either, and the pools are the configured size's.
    const Size pairs[][2] = {{{1920, 1080}, {3840, 2160}}, {{3840, 2160}, {1920, 1080}}, {{1280, 720}, {3840, 2160}},
                             {{3840, 2160}, {1280, 720}}, {{1920, 1088}, {1920, 1089}}, {{1920, 1089}, {1920, 1088}}};
    for (const auto& pair : pairs) {
        const DecodeDelays configured = getReorderingDelays(pair[0].w, pair[0].h);
        const DecodeDelays declared = getReorderingDelays(pair[1].w, pair[1].h);
        for (unsigned depth = 0; depth <= 16; ++depth) CHECK(!stalls(configured, depth) && !stalls(declared, depth));
        CHECK(configured.output + configured.pipeline == declared.output + declared.pipeline);
        CHECK(captureSlots(configured) == (pair[0].w * pair[0].h <= kSmallPictureMaxLumaSamples
                                                   ? captureSlots(kSmallPictureDelays)
                                                   : captureSlots(kLargePictureDelays)));
    }
    CHECK(getReorderingDelays(1920, 1088).output == kSmallPictureDelays.output);
    CHECK(getReorderingDelays(1920, 1089).output == kLargePictureDelays.output);
    CHECK(getReorderingDelays(3840, 2160).output == kLargePictureDelays.output);
    // The two floors the measurements gave: a 4K stream keeps the saving (8 capture buffers), and a picture up to 1080p
    // has 12 or more, where a 60 fps stream decoded as many frames as with 14 on a busy guest.
    CHECK(captureSlots(kLargePictureDelays) <= 8);
    CHECK(captureSlots(kSmallPictureDelays) >= 12 && captureSlots(kSmallPictureDelays) <= 14);
    return failures == 0 ? 0 : 1;
}
"""


def decode_delay_model_problems(header_text: str) -> list[str]:
    """Compile the delay header with the framework model and run it."""
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        return ["no C++ compiler to run the decoder delay model"]
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "DecodeDelays.h").write_text(header_text)
        (Path(tmp) / "model.cpp").write_text(DECODE_DELAY_MODEL)
        built = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", tmp, str(Path(tmp) / "model.cpp"), "-o", str(Path(tmp) / "model")],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120,
        )
        if built.returncode != 0:
            return ["the delay header does not compile with the model: " + built.stderr.strip().splitlines()[-1][:200] if built.stderr.strip() else "the delay header does not compile with the model"]
        ran = subprocess.run([str(Path(tmp) / "model")], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
        if ran.returncode != 0:
            return ["the delay model fails: " + "; ".join(ran.stdout.strip().splitlines()[:3])]
    return []


def decoder_delay_problems(files: dict[str, tuple[str, str]], message: str, run_model: bool = True) -> list[str]:
    """What is wrong with the topic that sets the H.264 and HEVC decoder delays (its lines per file, and its commit message).

    The framework stops feeding the decoder once the works that wait for an output, with their input released, reach the
    pipeline delay plus the output delay plus 4, and this device releases a picture once more than the stream's reorder
    depth R of them wait, so a stream stalls only when R exceeds the sum of the two delays by more than 3 (the input delay
    is 0). R is at most 16, which needs a sum of at least 13. The framework sizes the capture queue from the output delay
    and the input queue from the pipeline delay, reads both once with one query when it starts, and only a finished work
    can raise a delay later, so the sum is a constant and the split is chosen from the picture size the client configures:
    the size the stream's SPS declares later changes nothing the framework has sized, on either side of the 1080p line.
    A capture buffer costs six times an input block at 4K, so a picture above 1920x1088 spends the sum on the pipeline
    delay (4 and 9, 8 capture buffers); a smaller one keeps 12 capture buffers or more, which a 60 fps stream needs when the
    guest is busy (8 decoded 10 to 13 percent fewer frames than 14 on a 2 vCPU guest, 12 as many as 14)."""
    problems = []
    expected = {DECODE_DELAYS_HEADER, DECODE_INTERFACE_SOURCE, DECODE_INTERFACE_HEADER, DECODE_COMPONENT_SOURCE, DECODE_FACTORY_SOURCE, DECODE_README}
    if set(files) != expected:
        return [f"the topic touches {sorted(set(files) ^ expected)} beyond or short of the delay header, the interface, the component, the factory and the README"]
    header = files[DECODE_DELAYS_HEADER][0]
    everything = "\n".join(added for added, _ in files.values())
    total = re.search(r"constexpr uint32_t kReorderingDelaySum = (\d+);", header)
    large = re.search(r"constexpr DecodeDelays kLargePictureDelays = \{(\d+), (\d+)\};", header)
    small = re.search(r"constexpr DecodeDelays kSmallPictureDelays = \{(\d+), (\d+)\};", header)
    if not total or not large or not small:
        problems.append("the header does not define kReorderingDelaySum, kLargePictureDelays and kSmallPictureDelays")
    else:
        sums = {int(large.group(1)) + int(large.group(2)), int(small.group(1)) + int(small.group(2)), int(total.group(1))}
        if len(sums) != 1:
            problems.append(f"the two splits and the sum do not agree: {sorted(sums)}")
        if int(total.group(1)) + 3 < 16:
            problems.append(f"a sum of {total.group(1)} stalls a stream of reorder depth 16")
        if int(large.group(1)) + 4 > 8:
            problems.append("a picture above 1080p keeps more than 8 capture buffers, which gives up the 4K saving")
        if not 12 <= int(small.group(1)) + 4 <= 14:
            problems.append("a picture of 1080p or less is not at 12 to 14 capture buffers, the pool a 60 fps stream was measured to need")
    if "static_assert(kLargePictureDelays.output + kLargePictureDelays.pipeline == kReorderingDelaySum" not in header or "static_assert(kSmallPictureDelays.output + kSmallPictureDelays.pipeline == kReorderingDelaySum" not in header:
        problems.append("the header does not assert that both splits keep the sum")
    if "constexpr uint64_t kSmallPictureMaxLumaSamples = 1920ull * 1088ull;" not in header:
        problems.append("the line between the splits is not 1920x1088 luma samples")
    if not re.search(r"constexpr DecodeDelays getReorderingDelays\(uint32_t width, uint32_t height\) \{\s*return static_cast<uint64_t>\(width\) \* height <= kSmallPictureMaxLumaSamples", header):
        problems.append("the split is not a function of the configured width and height alone, compared in 64 bits")
    if re.search(r"#include\s*<(?!cstdint>)", header) or "property_get" in everything or "getprop" in everything.lower():
        problems.append("the header includes more than <cstdint>, or a delay is read from a property")
    interface = files[DECODE_INTERFACE_SOURCE][0]
    for setter in ("OutputDelaySetter", "PipelineDelaySetter"):
        if f".calculatedAs({setter}, mSize)" not in interface:
            problems.append(f"{setter} is not calculated from the picture size alone")
    if interface.count("getReorderingDelays(size.v.width, size.v.height)") != 2:
        problems.append("the setters do not both split by the size they are given")
    if "withConstValue(new C2PortDelayTuning::output(" not in interface or "withConstValue(new C2PipelineDelayTuning(" not in interface:
        problems.append("VP8, VP9 and AV1 no longer keep constant delays")
    if interface.count("return getReorderingDelays(width, height).pipeline;") != 1:
        problems.append("H.264 and HEVC do not take the pipeline delay of the split")
    if interface.count("return getReorderingDelays(width, height).output;") != 2:
        problems.append("H.264 and HEVC do not both take the output delay of the split")
    if "kReorderingOutputDelay" in everything or "kReorderingPipelineDelay" in everything:
        problems.append("a constant delay of the old topic remains")
    component = files[DECODE_COMPONENT_SOURCE][0]
    if "getMinNumOutputBuffers(mIntfImpl->getConfiguredOutputDelay())" not in component or "size_t getMinNumOutputBuffers(uint32_t outputDelay)" not in component:
        problems.append("the V4L2 capture queue is not sized from the delay the instance reports")
    factory = files[DECODE_FACTORY_SOURCE][0]
    if "getPipelineDelay(c, 0, 0) -" not in factory or "kBasePipelineDelay);" not in factory:
        problems.append("the input notification interval no longer follows the output delay plus the pipeline delay beyond the base one")
    if "configUpdate" in everything:
        problems.append("a delay is reported to the framework with a work: only the configured size may decide")
    readme = files[DECODE_README][0]
    for needed in ("whose SPS declares 4K keeps the 12 capture buffers", "SPS declares 1080p keeps 8", "reads them once when it starts"):
        if needed.replace("\n", " ") not in " ".join(readme.split()):
            problems.append(f"the README does not state: {needed}")
    flat = " ".join(message.split())
    for needed in ("Only the configured size decides", "whose SPS declares 4K keeps its 12 capture buffers", "SPS declares 1080p keeps its 8", "reorders on the host"):
        if needed not in flat:
            problems.append(f"the commit message does not state: {needed}")
    if run_model:
        problems += decode_delay_model_problems(header)
    return problems


def verify_decoder_output_delay_topic(android_root: Path) -> None:
    """The v4l2_codec2 topic that sets the H.264 and HEVC delays keeps every stream decodable, whatever size its SPS declares."""
    patches = sorted((android_root / "patches/projects/p0018/series").glob("v4l2-hold-fewer-capture-buffers-*.patch"))
    if len(patches) != 1:
        fail(f"p0018 has {len(patches)} output delay patches, not one")
    files, message = secure_element_topic_files(patches[0].read_text())
    problems = decoder_delay_problems(files, message)
    if problems:
        fail("the decoder delay topic: " + "; ".join(problems))
    header = DECODE_DELAYS_HEADER
    interface = DECODE_INTERFACE_SOURCE

    def edit(path: str, old: str, new: str) -> tuple[dict[str, tuple[str, str]], str]:
        added, removed = files[path]
        if old not in added:
            fail(f"the decoder delay mutation anchor is gone from {path}: {old!r}")
        return {**files, path: (added.replace(old, new, 1), removed)}, message

    for label, (edited_files, edited_message) in (
        ("a small split that sums to 12", edit(header, "kSmallPictureDelays = {8, 5};", "kSmallPictureDelays = {8, 4};")),
        ("a large split that sums to 12", edit(header, "kLargePictureDelays = {4, 9};", "kLargePictureDelays = {4, 8};")),
        ("a 4K split that keeps 14 capture buffers", edit(header, "kLargePictureDelays = {4, 9};", "kLargePictureDelays = {10, 3};")),
        ("a small split of 8 capture buffers", edit(header, "kSmallPictureDelays = {8, 5};", "kSmallPictureDelays = {4, 9};")),
        ("a line at 1080 rows", edit(header, "1920ull * 1088ull", "1920ull * 1080ull")),
        ("a 32-bit product", edit(header, "static_cast<uint64_t>(width) * height", "width * height")),
        ("a split that takes the codec", edit(header, "getReorderingDelays(uint32_t width, uint32_t height)", "getReorderingDelays(uint32_t width, uint32_t height, int codec)")),
        ("a split that follows a frame rate", edit(interface, ".calculatedAs(OutputDelaySetter, mSize)", ".calculatedAs(OutputDelaySetter, mSize, mFrameRate)")),
        ("a pipeline delay that ignores the size", edit(interface, ".calculatedAs(PipelineDelaySetter, mSize)", ".withSetter(Setter<decltype(*mPipelineDelay)>::NonStrictValueWithNoDeps)")),
        ("a capture queue sized from the codec", edit(DECODE_COMPONENT_SOURCE, "getMinNumOutputBuffers(mIntfImpl->getConfiguredOutputDelay())", "getMinNumOutputBuffers(32)")),
        ("a delay reported with a work", edit(interface, "return C2R::Ok();\n}\n\n// static\nC2R DecodeInterface::PipelineDelaySetter", "work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(*mOutputDelay));\n    return C2R::Ok();\n}\n\n// static\nC2R DecodeInterface::PipelineDelaySetter")),
        ("a delay read from a property", edit(interface, "return C2R::Ok();\n}\n\n// static\nC2R DecodeInterface::PipelineDelaySetter", 'property_get_int32("ro.vendor.v4l2_codec2.output_delay", 10);\n    return C2R::Ok();\n}\n\n// static\nC2R DecodeInterface::PipelineDelaySetter')),
        ("an interval that ignores the pipeline delay", edit(DECODE_FACTORY_SOURCE, "getPipelineDelay(c, 0, 0) -", "0 -")),
    ):
        if edited_files == files or not decoder_delay_problems(edited_files, edited_message):
            fail(f"the decoder delay check accepts {label}")
    for label, text in (
        ("a message without the configured size rule", message.replace("Only the configured size decides", "The size decides")),
        ("a message that drops the reason", message.replace("reorders on the host", "reorders")),
    ):
        if text == message or not decoder_delay_problems(files, text, run_model=False):
            fail(f"the decoder delay check accepts {label}")


SECURE_ELEMENT_MANIFEST = "AndroidManifest.xml"
SECURE_ELEMENT_APPLICATION = "src/com/android/se/SEApplication.java"


def secure_element_topic_problems(files: dict[str, tuple[str, str]], message: str) -> list[str]:
    """What is wrong with the SecureElement topic (its lines per file, and its commit message).

    The topic makes the OMAPI service start when a client binds it. It removes the one attribute that
    starts the process at boot, and the application's own startService call: the system starts the
    process of a bound service in the background and refuses a background startService, so with the
    call the application crashed in onCreate on every bind and no client ever connected (cand15). The
    bind creates the service, which registers itself in its own onCreate. The topic changes nothing else
    of the manifest: the service, its exported intent filter and its shared uid stay, so a client that
    binds the intent still connects. Nothing else may start, bind or pin the service from the application:
    a started service is sticky, and the system would restart the process after reclaiming it."""
    problems = []
    if set(files) != {SECURE_ELEMENT_MANIFEST, SECURE_ELEMENT_APPLICATION}:
        return [f"the topic touches {sorted(files)}, not the manifest and SEApplication.java"]
    added, removed = files[SECURE_ELEMENT_MANIFEST]
    if added.strip():
        problems.append("the topic adds manifest lines")
    if removed.strip() != 'android:persistent="true"':
        problems.append("the topic removes something other than the persistent attribute")
    added, removed = files[SECURE_ELEMENT_APPLICATION]
    if "startService(serviceIntent);" not in removed:
        problems.append("the topic leaves the application's startService call")
    for call in ("startService", "startForegroundService", "bindService"):
        if call + "(" in added:
            problems.append(f"the topic adds a {call} call to the application")
    if "try" in added.split() or "catch" in added:
        problems.append("the topic keeps the start and catches its refusal instead of removing it")
    if "exactly as before" in message:
        problems.append("the message claims that onCreate starts the service as before")
    for needed in ("startService", "BackgroundServiceStartNotAllowedException"):
        if needed not in message:
            problems.append(f"the message does not mention {needed}")
    return problems


def secure_element_topic_files(text: str) -> tuple[dict[str, tuple[str, str]], str]:
    """The added and removed lines per file of a one-commit patch, and its commit message."""
    head, _, body = text.partition("\n---\n")
    message = head.split("\n\n", 1)[1] if "\n\n" in head else ""
    files: dict[str, tuple[str, str]] = {}
    for chunk in re.split(r"^diff --git a/", body, flags=re.MULTILINE)[1:]:
        path = chunk.split(" b/", 1)[0]
        lines = chunk.splitlines()
        added = "\n".join(line[1:] for line in lines if line.startswith("+") and not line.startswith("+++"))
        removed = "\n".join(line[1:] for line in lines if line.startswith("-") and not line.startswith("---"))
        files[path] = (added, removed)
    return files, message


def verify_secure_element_topic(android_root: Path) -> None:
    """SecureElement stays in the product, not persistent: it costs nothing at idle and still answers a bind.

    CTS's OmapiTest#testGetOmapiVersion fails when the SEService intent finds no package, so the package
    is kept; the memory it cost at idle (about 5.7 MiB) is saved by starting it on a bind."""
    patches = sorted((android_root / "patches/projects/p0024/series").glob("SecureElement-*.patch"))
    if len(patches) != 1:
        fail(f"p0024 has {len(patches)} SecureElement patches, not one")
    files, message = secure_element_topic_files(patches[0].read_text())
    problems = secure_element_topic_problems(files, message)
    if problems:
        fail("the SecureElement topic: " + "; ".join(problems))
    manifest_added, manifest_removed = files[SECURE_ELEMENT_MANIFEST]
    app_added, app_removed = files[SECURE_ELEMENT_APPLICATION]

    def variant(manifest=None, application=None, message_edit=None, extra_file=None, drop_file=None):
        edited = dict(files)
        if manifest is not None:
            edited[SECURE_ELEMENT_MANIFEST] = manifest
        if application is not None:
            edited[SECURE_ELEMENT_APPLICATION] = application
        if extra_file:
            edited["src/com/android/se/SecureElementService.java"] = ("", "x")
        if drop_file:
            edited.pop(drop_file)
        return secure_element_topic_problems(edited, message_edit if message_edit is not None else message)

    for label, found in (
        ("a topic that adds the attribute back", variant(manifest=(manifest_added + 'android:persistent="true"', manifest_removed))),
        ("a topic that also drops the exported service",
         variant(manifest=(manifest_added, manifest_removed + '\n<service android:name=".SecureElementService"'))),
        ("a topic that removes another attribute",
         variant(manifest=(manifest_added, manifest_removed.replace("persistent", "directBootAware")))),
        ("a topic that removes nothing from the manifest", variant(manifest=(manifest_added, ""))),
        ("a topic that keeps the application's startService", variant(application=(app_added, ""))),
        ("a topic that adds a catch around the start",
         variant(application=(app_added + "\ntry { startService(serviceIntent); } catch (IllegalStateException e) { }", app_removed))),
        ("a topic that starts the service in a foreground service",
         variant(application=(app_added + "\nstartForegroundService(serviceIntent);", app_removed))),
        ("a topic that binds the service from the application",
         variant(application=(app_added + "\nbindService(serviceIntent, c, 1);", app_removed))),
        ("a topic that touches the service as well", variant(extra_file=True)),
        ("a topic without the application change", variant(drop_file=SECURE_ELEMENT_APPLICATION)),
        ("a message that claims the start works as before",
         variant(message_edit=message + "\nonCreate starts and registers the service exactly as before.")),
        ("a message that does not give the crash", variant(message_edit=message.replace("BackgroundServiceStartNotAllowedException", "an exception"))),
    ):
        if not found:
            fail(f"the SecureElement topic check accepts {label}")
    # The product keeps the package (the package assertions are verified in verify_package_assertions) and
    # the image verifier does not forbid its directory.
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    removed_block = re.search(
        r"^KERNARIUM_REMOVED_PACKAGES := \\\n((?:    \S+ \\\n)+    \S+)$", product, re.MULTILINE
    )
    if removed_block is None or "SecureElement" in removed_block.group(1).replace("\\", " ").split():
        fail("the product removes SecureElement")
    if "SecureElement" in load_tool(android_root / "tools/verify-target-files.py").FORBIDDEN_APP_DIRECTORIES:
        fail("the target-files verifier forbids the SecureElement directory")
    if "route `p0024`" not in (android_root / "README.md").read_text():
        fail("the README does not describe the SecureElement topic (route p0024)")


def verify_sms_holder(android_root: Path) -> None:
    """The text message service starts on demand: the sources that produce the holder, the waker, the policy
    and the service are what the target-files verifier looks for.

    The target-files fixtures prove what the verifier rejects; this proves that the tracked sources say it:
    the holder's policy is the socket's creation and nothing else of vsock, the waker holds none, the service
    makes no socket, and the product carries the files the verifier requires."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    overlay = android_root / "overlay"
    private = overlay / "sepolicy/system_ext/private"
    holder = (private / "kernarium_sms_listener.te").read_text()
    waker = (private / "kernarium_sms_wake.te").read_text()
    service = (private / "kernarium_sms.te").read_text()
    for needle in (
        "type kernarium_sms_listener, domain;",
        "typeattribute kernarium_sms_listener coredomain;",
        "typeattribute kernarium_sms_listener unconstrained_vsock_violators;",
        "init_daemon_domain(kernarium_sms_listener)",
        "allow kernarium_sms_listener self:vsock_socket { create bind listen };",
        "allow init kernarium_sms_listener:unix_stream_socket { create bind };",
        "allow kernarium_sms_listener self:unix_stream_socket { accept getattr getopt listen read write };",
        "system_internal_prop(ctl_kernarium_sms_wake_prop)",
        "set_prop(kernarium_sms_listener, ctl_kernarium_sms_wake_prop)",
        "system_internal_prop(ctl_kernarium_sms_locked_prop)",
        "dontaudit kernarium_sms_listener ctl_kernarium_sms_locked_prop:property_service set;",
    ):
        if needle not in holder:
            fail(f"the holder's policy lacks '{needle}'")
    for line in holder.splitlines():
        if not line.startswith("#") and "vsock_socket" in line and "self:vsock_socket { create bind listen };" not in line:
            fail(f"the holder's policy has a vsock rule beyond the socket's creation: {line}")
    for needle in ("binder", "appdomain", "service_manager"):
        if re.search(rf"^[^#\n]*{needle}", holder, re.M):
            fail(f"the holder's policy has {needle}: it is a plain daemon and not the framework's client")
    for needle in (
        "type kernarium_sms_wake, domain;",
        "typeattribute kernarium_sms_wake coredomain;",
        "init_daemon_domain(kernarium_sms_wake)",
        "allow kernarium_sms_wake activity_service:service_manager find;",
        "allow kernarium_sms_wake system_server:binder transfer;",
        "allow system_server kernarium_sms_wake:binder call;",
        "allow system_server kernarium_sms_wake:fd use;",
    ):
        if needle not in waker:
            fail(f"the waker's policy lacks '{needle}'")
    if re.search(r"^[^#\n]*(vsock_socket|unconstrained_vsock_violators)", waker, re.M):
        fail("the waker's policy touches a vsock socket")
    for needle in (
        "allow kernarium_sms kernarium_sms_listener:vsock_socket { accept read write shutdown getattr getopt };",
        "allow kernarium_sms kernarium_sms_listener:fd use;",
        "unix_socket_connect(kernarium_sms, kernarium_sms, kernarium_sms_listener)",
        "typeattribute kernarium_sms unconstrained_vsock_violators;",
        "typeattribute kernarium_sms mlstrustedsubject;",
    ):
        if needle not in service:
            fail(f"the service's policy lacks '{needle}'")
    for name, text in (("holder", holder), ("waker", waker)):
        if re.search(r"^[^#\n]*mlstrustedsubject", text, re.M):
            fail(f"the {name}'s policy makes it an mlstrustedsubject: it searches no directory of the user")
    if re.search(r"^(?!neverallow)[^#\n]*self:vsock_socket", service, re.M):
        fail("the service's policy gives it rights on a vsock socket of its own: it makes none")
    contexts = (private / "file_contexts").read_text()
    for path, label in (
        (r"/(system_ext|system/system_ext)/bin/kernarium_sms_listener", "kernarium_sms_listener_exec"),
        (r"/(system_ext|system/system_ext)/bin/kernarium_sms_wake\.sh", "kernarium_sms_wake_exec"),
        (verifier.SMS_SOCKET_PATH, verifier.SMS_SOCKET_TYPE),
    ):
        if f"{path}  u:object_r:{label}:s0" not in contexts:
            fail(f"the system_ext file contexts do not label {path} as {label}")
    properties = (private / "property_contexts").read_text()
    if f"{verifier.SMS_WAKE_PROPERTY}    u:object_r:{verifier.SMS_WAKE_PROPERTY_TYPE}:s0" not in properties:
        fail("the system_ext property contexts do not label the control property that starts the waker")
    # Every control key of the two services that the product labels carries an exact entry; all but the one the holder
    # sets are the locked type, which nothing in the policy sources lets any domain set but init.
    for key in verifier.SMS_CONTROL_OWN_KEYS:
        if not verifier.property_entry_is_exact(key, properties.splitlines()):
            fail(f"the system_ext property contexts have no exact entry for {key}")
        wanted = verifier.SMS_WAKE_PROPERTY_TYPE if key == verifier.SMS_WAKE_PROPERTY else "ctl_kernarium_sms_locked_prop"
        if verifier.property_label(key, properties.splitlines()) != wanted:
            fail(f"the system_ext property contexts do not label {key} as {wanted}")
    # The one other line that may name it is the holder's dontaudit of its own denied start of the waker (a dontaudit
    # grants nothing).
    if re.search(r"ctl_kernarium_sms_locked_prop", "\n".join(
            line for line in (holder + waker + service).splitlines() if not line.lstrip().startswith("#")
            and "system_internal_prop(ctl_kernarium_sms_locked_prop)" not in line
            and line.strip() != "dontaudit kernarium_sms_listener ctl_kernarium_sms_locked_prop:property_service set;")):
        fail("a policy source names ctl_kernarium_sms_locked_prop beyond its declaration: nothing sets it but init")
    listener = overlay / "sms/listener"
    blueprint = (listener / "Android.bp").read_text()
    for needle in (
        'name: "kernarium_sms_listener"',
        'init_rc: ["kernarium_sms_listener.rc"]',
        "system_ext_specific: true,",
        'name: "kernarium_sms_wake_script"',
        'src: "kernarium_sms_wake.sh"',
    ):
        if needle not in blueprint:
            fail(f"overlay/sms/listener/Android.bp lacks {needle}")
    product = (overlay / "lineage_andarium.mk").read_text()
    for package in ("KernariumSms", "kernarium_sms_listener", "kernarium_sms_wake_script"):
        if len(re.findall(rf"^PRODUCT_PACKAGES \+= .*\b{package}\b", product, re.M)) != 1:
            fail(f"lineage_andarium.mk does not add {package} to PRODUCT_PACKAGES exactly once")
    guard = (android_root / "build/build-aosp.sh").read_text()
    for package in ("kernarium_sms_listener", "kernarium_sms_wake_script"):
        if f'*" {package} "*' not in guard:
            fail(f"build-aosp.sh does not guard {package}")
    sources = overlay / "sms/src/android/kernarium/sms"
    if (sources / "HostChannel.java").exists() or not (sources / "HostService.java").exists():
        fail("the text message service is not the on-demand HostService")
    for source in sorted(sources.glob("*.java")):
        text = source.read_text()
        if re.search(r"\bOs\.(socket|bind|listen)\(|\bnew (Server|Vsock)Socket\b", text):
            fail(f"{source.name} makes a socket: the holder makes the vsock socket")
    manifest = (overlay / "sms/AndroidManifest.xml").read_text()
    if re.search(r"android:persistent|<(activity|receiver|provider|uses-permission)\b|android:name=\"\.[A-Za-z]*Application\"",
                 re.sub(r"<!--.*?-->", "", manifest, flags=re.S)):
        fail("the text message manifest declares more than its one service, or is persistent")
    if len(re.findall(r"<service\b", manifest)) != 1:
        fail("the text message manifest does not declare exactly one service")

    # The waker runs its one command: with a stub for cmd, the script asks the activity manager to start the service.
    script = listener / "kernarium_sms_wake.sh"
    with tempfile.TemporaryDirectory(prefix="sms-wake-") as temporary:
        stubs = Path(temporary)
        calls = stubs / "calls"
        (stubs / "cmd").write_text(f'#!/bin/sh\necho "cmd $*" >> {calls}\n')
        (stubs / "cmd").chmod(0o755)
        result = subprocess.run(["/bin/sh", str(script)], env={"PATH": f"{stubs}:/usr/bin:/bin"},
                                capture_output=True, text=True)
        if result.returncode != 0 or result.stdout or result.stderr:
            fail(f"the waker's script exited {result.returncode} with output {result.stdout!r} {result.stderr!r}")
        if calls.read_text().splitlines() != ["cmd activity start-service --user 0 -n android.kernarium.sms/.HostService"]:
            fail(f"the waker's script ran {calls.read_text().splitlines()}")
    if verifier.SMS_WAKE_COMMAND not in script.read_text():
        fail("the waker's script is not the command the verifier requires")


def verify_reclaim_boot_apps(android_root: Path) -> None:
    """The reclaim script kills only the cached processes of the listed packages.

    The script runs with stub tools: dumpsys prints a process list, cmd records the kill it is asked
    for and log records what the script says. The list covers a cached process, one that is not
    cached and is not touched by `kill` either, one that `kill` would take though it is not cached,
    a package that is not running and a package that is not listed."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    tools = android_root / "overlay/tools"
    script = tools / "kernarium_reclaim_boot_apps.sh"
    # The sepolicy sources hold what the built-policy check looks for.
    policy = (android_root / "overlay/sepolicy/system_ext/private/kernarium_reclaim_boot_apps.te").read_text()
    for needle in (
        "init_daemon_domain(kernarium_reclaim_boot_apps)",
        "allow kernarium_reclaim_boot_apps activity_service:service_manager find;",
        "allow kernarium_reclaim_boot_apps system_server:binder transfer;",
        "allow system_server kernarium_reclaim_boot_apps:binder call;",
        "allow system_server kernarium_reclaim_boot_apps:fd use;",
        "allow system_server kernarium_reclaim_boot_apps:fifo_file { getattr read write };",
        "allow kernarium_reclaim_boot_apps system_file:file execute_no_trans;",
    ):
        if needle not in policy:
            fail(f"the reclaim domain lacks '{needle}'")
    contexts = (android_root / "overlay/sepolicy/system_ext/private/file_contexts").read_text()
    if (
        r"/(system_ext|system/system_ext)/bin/kernarium_reclaim_boot_apps\.sh  "
        "u:object_r:kernarium_reclaim_boot_apps_exec:s0"
    ) not in contexts:
        fail("the system_ext file contexts do not label the reclaim script")
    blueprint = (tools / "Android.bp").read_text()
    for needle in (
        'name: "kernarium_reclaim_boot_apps_script"',
        'src: "kernarium_reclaim_boot_apps.sh"',
        'init_rc: ["kernarium_reclaim_boot_apps.rc"]',
    ):
        if needle not in blueprint:
            fail(f"overlay/tools/Android.bp lacks {needle}")
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    if len(re.findall(r"^PRODUCT_PACKAGES \+= kernarium_reclaim_boot_apps_script$", product, re.M)) != 1:
        fail("lineage_andarium.mk does not add the reclaim script to PRODUCT_PACKAGES exactly once")
    reclaim_rc = (tools / "kernarium_reclaim_boot_apps.rc").read_text()
    verifier.check_reclaim_boot_apps(reclaim_rc, script.read_bytes())
    script_text = script.read_text()
    packages = [entry[0] for entry in verifier.RECLAIM_APPS]

    def listing(*rows: str) -> str:
        return "\n".join(("ACTIVITY MANAGER LRU PROCESSES (dumpsys activity lru)", "  Other:", *rows)) + "\n"

    base = "\n".join((
        "ACTIVITY MANAGER LRU PROCESSES (dumpsys activity lru)",
        "  Activities:",
        "  #30: fg       TOP  LCMNFUATI 1155:com.android.launcher3/u0a86 act:activities|recents",
        "  Other:",
        # cached, with a process of the same package that the system keeps visible
        "  #29: cch  + 5 CEM  --------- 1587:.adservices/u0a104",
        "  #28: vis      BFGS LCMNFUATI 1321:android.ext.services/u0a104",
        # cached, one digit index
        "  # 9: cch  +15 CEM  --------- 1583:com.android.localtransport/1000",
        # a started service: the activity manager would end it, so the package is left alone
        "  #20: svc      SVC  --------- 1800:com.android.rkpdapp/u0a121",
        "  #21: cch  + 5 CEM  --------- 1840:com.android.statementservice/u0a89",
        # not cached and the activity manager leaves it: nothing to reclaim
        "  #22: prcp + 1 IMPB -------TI 1632:com.stevesoltys.seedvault/u0a85",
        # not listed
        "  #23: cch  + 5 CEM  --------- 1999:com.example.unlisted/u0a200",
        "  #24: cch  + 5 CEM  --------- 1744:com.android.ondevicepersonalization.services.extra/u0a113",
        # Messaging, cached since its boot receiver ran
        "  #31: cch  + 7 CEM  --------- 1870:com.android.messaging/u0a95",
        # the SMS provider's process is not named after its package
        "  #32: cch  + 8 CEM  --------- 1860:com.android.phone/1001",
        # the icon renderer's provider, cached after the last listing; its isolated worker is another process
        "  #33: cch  + 9 CEM  --------- 1900:com.kernarium.icons/u0a30",
        "  #34: cch  +10 CEM  --------- 1901:com.kernarium.icons:IconRenderService/u0i1",
    )) + "\n"
    # (name, listings, expected kills, log lines). The listings are what `dumpsys activity lru` answers to the 1st,
    # 2nd, ... call; the last one answers every later call.
    scenarios = (
        ("the cached processes of the listed packages", [base], [
            "android.ext.services", "com.android.localtransport", "com.android.statementservice",
            "com.android.messaging", "com.android.providers.telephony", "com.kernarium.icons"], [
            "com.android.rkpdapp left alone, not cached: com.android.rkpdapp:svc",
            "com.stevesoltys.seedvault has no cached process (kept: com.stevesoltys.seedvault:prcp)",
            "android.ext.services asked to end cached .adservices (kept: android.ext.services:vis)",
            "com.android.federatedcompute.services has no cached process (kept: none)",
            "com.android.messaging asked to end cached com.android.messaging (kept: none)",
            "com.android.providers.telephony asked to end cached com.android.phone (kept: none)",
            "com.kernarium.icons asked to end cached com.kernarium.icons (kept: none)"]),
        # An icon renderer that a listing holds (the shell tool's hold on its provider keeps the process above cached)
        # is left alone, and Messaging in the foreground is too.
        ("a held renderer and a foreground Messaging", [listing(
            "  #33: fg       FGS  --------- 1900:com.kernarium.icons/u0a30",
            "  #34: cch  +10 CEM  --------- 1901:com.kernarium.icons:IconRenderService/u0i1",
            "  #35: fg       TOP  LCMNFUATI 1870:com.android.messaging/u0a95 act:activities|recents")], [], [
            "com.kernarium.icons has no cached process (kept: com.kernarium.icons:fg)",
            "com.android.messaging has no cached process (kept: com.android.messaging:fg)"]),
        # The process name is compared whole: another application's private process is named
        # <its package>:<name>, and it neither makes the provider's process look busy nor cached.
        ("another application's private process named like the provider's, in the foreground, listed first", [listing(
            "  #40: fg       SVC  --------- 2300:com.example.app:com.android.phone/u0a200",
            "  #32: cch  + 8 CEM  --------- 1860:com.android.phone/1001")], ["com.android.providers.telephony"], [
            "com.android.providers.telephony asked to end cached com.android.phone (kept: none)"]),
        ("another application's private process named like the provider's, cached, listed first", [listing(
            "  #40: cch  + 5 CEM  --------- 2300:com.example.app:com.android.phone/u0a200",
            "  #32: prev     LAST --------I 1860:com.android.phone/1001")], [], [
            "com.android.providers.telephony left alone, not cached: com.android.phone:prev"]),
        # Only user 0's rows count: the kill is for user 0.
        ("another user's cached Messaging listed before user 0's previous Messaging", [listing(
            "  #40: cch  + 5 CEM  --------- 2400:com.android.messaging/u10a86",
            "  #26: prev     LAST --------I 1870:com.android.messaging/u0a95")], [], [
            "com.android.messaging left alone, not cached: com.android.messaging:prev"]),
        ("another user's previous Messaging listed before user 0's cached Messaging", [listing(
            "  #40: prev     LAST --------I 2400:com.android.messaging/u10a86",
            "  #26: cch  + 7 CEM  --------- 1870:com.android.messaging/u0a95")], ["com.android.messaging"], [
            "com.android.messaging asked to end cached com.android.messaging (kept: none)"]),
        # Every row of the process counts: one that is not cached keeps the package, whatever comes first.
        ("two user 0 rows of a process, the cached one first", [listing(
            "  #40: cch  + 5 CEM  --------- 2500:com.android.messaging/u0a99",
            "  #26: svc      SVC  --------- 1870:com.android.messaging/u0a95")], [], [
            "com.android.messaging left alone, not cached: com.android.messaging:svc"]),
        # A process name counts as cached only when every user 0 row that has it is cached: another uid's process that
        # took the name must not stand for the real one, whichever row comes first.
        ("the real Messaging in the foreground listed first, another uid's cached row of the same name after", [listing(
            "  #35: fg       TOP  LCMNFUATI 1870:com.android.messaging/u0a95 act:activities|recents",
            "  #40: cch  + 5 CEM  --------- 2300:com.android.messaging/u0a99")], [], [
            "com.android.messaging has no cached process (kept: com.android.messaging:fg)"]),
        ("another uid's cached row of the name of Messaging first, the real one in the foreground after", [listing(
            "  #40: cch  + 5 CEM  --------- 2300:com.android.messaging/u0a99",
            "  #35: fg       TOP  LCMNFUATI 1870:com.android.messaging/u0a95 act:activities|recents")], [], [
            "com.android.messaging has no cached process (kept: com.android.messaging:fg)"]),
        ("the provider's process perceptible beside another uid's cached row of its name", [listing(
            "  #32: prcp + 1 IMPB -------TI 1860:com.android.phone/1001",
            "  #40: cch  + 5 CEM  --------- 3000:com.android.phone/u0a200")], [], [
            "com.android.providers.telephony has no cached process (kept: com.android.phone:prcp)"]),
        # The one kill is the package's: cached processes of sibling packages and of private processes do not add to it.
        ("the reviewed packages cached together with siblings by prefix", [listing(
            "  #31: cch  + 7 CEM  --------- 1870:com.android.messaging/u0a95",
            "  #32: cch  + 8 CEM  --------- 1860:com.android.phone/1001",
            "  #40: cch  + 5 CEM  --------- 2600:com.android.messaging.demo/u0a97",
            "  #41: cch  + 5 CEM  --------- 2602:com.android.phone.helper/u0a99",
            "  #42: cch  + 5 CEM  --------- 2603:com.android.messaging:push/u0a95")],
         ["com.android.messaging", "com.android.providers.telephony"], [
            "com.android.messaging asked to end cached com.android.messaging (kept: none)",
            "com.android.providers.telephony asked to end cached com.android.phone (kept: none)"]),
        # A package or a process name that only starts or ends like a listed one is not that process.
        ("processes whose names start or end like a listed process", [listing(
            "  #40: cch  + 5 CEM  --------- 2600:com.android.messaging.demo/u0a97",
            "  #41: cch  + 5 CEM  --------- 2601:org.example.com.android.messaging/u0a98",
            "  #42: cch  + 5 CEM  --------- 2602:com.android.phone.helper/u0a99",
            "  #43: cch  + 5 CEM  --------- 2603:x.com.android.phone/u0a99")], [], [
            "com.android.messaging has no cached process (kept: none)",
            "com.android.providers.telephony has no cached process (kept: none)"]),
        # Each package is decided from a listing made for it: Messaging is cached in the first listing and the
        # previous app in every later one.
        ("a process that leaves the cached state after the first listing", [listing(
            "  #31: cch  + 7 CEM  --------- 1870:com.android.messaging/u0a95",
            "  #32: cch  + 8 CEM  --------- 1860:com.android.phone/1001"), listing(
            "  #31: prev     LAST --------I 1870:com.android.messaging/u0a95",
            "  #32: cch  + 8 CEM  --------- 1860:com.android.phone/1001")], ["com.android.providers.telephony"], [
            "com.android.messaging left alone, not cached: com.android.messaging:prev"]),
        # No process list, no kills.
        ("no process list", [""], [], [f"{package} left alone: no process list" for package in packages]),
    )

    def run_reclaim(text: str, listings: list[str]) -> tuple[int, list[str], list[str], list[str], int]:
        """The script on stub tools. dumpsys answers each call with the next listing; cmd records the kill it is
        asked for and log what the script says. A tool that ends a process and that the script has no business
        with is a stub that records the call and fails, first in PATH, and the shell's own kill is a function: a
        mutated script never runs the host's tools."""
        with tempfile.TemporaryDirectory(prefix="reclaim-script-") as temporary:
            stubs = Path(temporary)
            calls = stubs / "calls"
            for number, answer in enumerate(listings, 1):
                (stubs / f"lru.{number}").write_text(answer)
            bodies = {
                "dumpsys": (
                    '[ "$*" = "activity lru" ] || exit 2\n'
                    f'echo "dumpsys $*" >> {calls}\n'
                    f'n=$(cat {stubs}/count 2>/dev/null || echo 0); n=$((n + 1)); echo $n > {stubs}/count\n'
                    f'last={len(listings)}; [ $n -le $last ] || n=$last\n'
                    f'cat {stubs}/lru.$n\n'),
                "cmd": f'echo "cmd $*" >> {calls}\necho Killed\n',
                "log": f'echo "log $*" >> {calls}\n',
                "sleep": f'echo "sleep $*" >> {calls}\n',
            }
            for name in ("kill", "pkill", "killall", "pidof", "ps", "am", "toybox", "toolbox"):
                bodies[name] = f'echo "FORBIDDEN {name} $*" >> {calls}\nexit 99\n'
            for name, body in bodies.items():
                (stubs / name).write_text("#!/bin/sh\n" + body)
                (stubs / name).chmod(0o755)
            wrapper = (f'kill() {{ echo "FORBIDDEN kill $*" >> {calls}; return 99; }}; . "$1"')
            (stubs / "script.sh").write_text(text)
            result = subprocess.run(["/bin/sh", "-c", wrapper, "sh", str(stubs / "script.sh")],
                                    env={"PATH": f"{stubs}:/usr/bin:/bin"}, capture_output=True, text=True,
                                    input="", cwd=temporary)
            recorded = calls.read_text().splitlines() if calls.exists() else []
            kills = [line for line in recorded if line.startswith("cmd activity kill --user 0 ")]
            other = [line for line in recorded if line.startswith("cmd ") and not line.startswith("cmd activity kill --user 0 ")]
            forbidden = [line for line in recorded if line.startswith("FORBIDDEN")] + other
            listings_taken = sum(1 for line in recorded if line.startswith("dumpsys "))
            return result.returncode, kills, recorded, forbidden, listings_taken

    def reclaim_problem(text: str, first_only: bool = False) -> str | None:
        """What the script does wrong on the scenarios (all of them, one line each; with first_only up to the first
        scenario that goes wrong), or None."""
        problems = []
        for name, listings, expected, needles in scenarios:
            if first_only and problems:
                break
            code, kills, recorded, forbidden, taken = run_reclaim(text, listings)
            joined = "\n".join(recorded)
            if forbidden:
                problems.append(f"{name}: the script ran something outside the reviewed kill: {forbidden}")
            if recorded[:1] != ["sleep 120"]:
                problems.append(f"{name}: the script does not wait 120 s first: {recorded[:1]}")
            # The complete call, so that a second package or an extra word in the one command is a failure.
            wanted = [f"cmd activity kill --user 0 {package}" for package in expected]
            if kills != wanted:
                problems.append(f"{name}: the script asked for {kills}, not {wanted}")
            if name == "no process list":
                if code == 0:
                    problems.append(f"{name}: the script exits 0 without a process list")
            elif code != 0:
                problems.append(f"{name}: the script exited {code}")
            for needle in needles:
                if needle not in joined:
                    problems.append(f"{name}: the script did not log '{needle}'")
            if "killed cached" in joined:
                problems.append(f"{name}: the script claims a kill that only the activity manager can say happened")
            # One listing for each package, taken for it.
            if taken != len(packages):
                problems.append(f"{name}: the script took {taken} listings for {len(packages)} packages")
        return "\n  ".join(problems) or None

    problem = reclaim_problem(script_text)
    if problem:
        fail(f"the reclaim script:\n  {problem}")

    # Every mutation of the script that the review named must fail the verifier or a scenario above. A fix that no
    # test would notice losing is not held.
    kill_line = 'out=$(cmd activity kill --user 0 "$pkg" 2>&1 </dev/null)'
    mutations = {
        "an extra pkill by pattern": (kill_line, 'pkill -0 -f "^never-$pkg-x$"\n        ' + kill_line),
        "an extra kill of the pid of a package": (kill_line, 'kill -TERM $(pidof "$pkg")\n        ' + kill_line),
        "an extra am command": (kill_line, 'am kill "$pkg"\n        ' + kill_line),
        "a second assignment to the list": (
            "tag=kernarium_reclaim_boot_apps\n",
            'reclaim_list="$reclaim_list\ncom.example.extra com.example.extra"\ntag=kernarium_reclaim_boot_apps\n'),
        "a process name matched by its end": (
            '[ "$row_name" = "$proc" ]', '[ "${row_name%"$proc"}" != "$row_name" ]'),
        "a process name matched by its start": (
            '[ "$row_name" = "$proc" ]', '[ "${row_name#"$proc"}" != "$row_name" ]'),
        "rows of every user": ('[ -n "$row_user0" ]', "true"),
        "the first row of a process decides": (
            '                *) kept="$kept $proc:$label"; not_cached=1 ;;\n            esac\n',
            '                *) kept="$kept $proc:$label"; not_cached=1 ;;\n            esac\n            break\n'),
        "a log line that claims a kill": ("asked to end cached", "killed cached"),
        "a cached row of the name stands although another row of it is not cached": (
            'if [ -n "$is_cached" ] && [ -z "$not_cached" ]; then', 'if [ -n "$is_cached" ]; then'),
        "the package named twice in the one kill": (kill_line, kill_line.replace('"$pkg" 2>&1', '"$pkg" "$pkg" 2>&1')),
        "a loop over the cached siblings around the one kill": [
            ('    cached=""\n', '    cached=""\n    siblings=""\n'),
            ('            parse_row "$line"\n',
             '            parse_row "$line"\n            case "$row_name:$label" in\n'
             '                "$pkg".*:cch*) siblings="$siblings $row_name" ;;\n            esac\n'),
            (kill_line, 'for pkg in "$pkg" $siblings; do\n            ' + kill_line + '\n        done')],
        "one listing for every package": (
            "lru=$(dumpsys activity lru 2>/dev/null </dev/null)",
            '[ -n "$lru" ] || lru=$(dumpsys activity lru 2>/dev/null </dev/null)'),
    }
    for title, edits in mutations.items():
        mutated = script_text
        for old, new in (edits if isinstance(edits, list) else [edits]):
            if old not in mutated:
                fail(f"the reclaim script lacks '{old}' for the mutation '{title}'")
            mutated = mutated.replace(old, new, 1)
        try:
            verifier.check_reclaim_boot_apps(reclaim_rc, mutated.encode())
            caught = reclaim_problem(mutated, first_only=True)
        except (SystemExit, Exception) as problem_found:  # the verifier's fail() raises its own error
            caught = f"the verifier: {problem_found}"
        if not caught:
            fail(f"neither the verifier nor the scenarios notice: {title}")


def verify_time_zone(android_root: Path) -> None:
    """The time zone script follows the host only while nobody in the guest has chosen a zone.

    The script runs with stub tools: getprop and setprop read and write a table of properties,
    `cmd settings get` prints the automatic time zone setting, `cmd alarm set-timezone` records the
    call and, as the platform does, takes a name its tzdata has and refuses any other, and log
    records what the script says."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    tools = android_root / "overlay/tools"
    script = tools / "kernarium_time_zone.sh"
    # The sepolicy sources hold what the built-policy check looks for.
    policy = (android_root / "overlay/sepolicy/system_ext/private/kernarium_time_zone.te").read_text()
    for needle in (
        "init_daemon_domain(kernarium_time_zone)",
        "allow kernarium_time_zone settings_service:service_manager find;",
        "allow kernarium_time_zone alarm_service:service_manager find;",
        "allow kernarium_time_zone system_server:binder transfer;",
        "allow system_server kernarium_time_zone:binder call;",
        "allow system_server kernarium_time_zone:fd use;",
        "allow system_server kernarium_time_zone:fifo_file { getattr read write };",
        "allow kernarium_time_zone system_file:file execute_no_trans;",
        "system_internal_prop(kernarium_time_zone_prop)",
        "set_prop(kernarium_time_zone, kernarium_time_zone_prop)",
    ):
        if needle not in policy:
            fail(f"the time zone domain lacks '{needle}'")
    if len(re.findall(r"^set_prop\(", policy, re.M)) != 1:
        fail("the time zone domain sets a property besides its marker")
    contexts = (android_root / "overlay/sepolicy/system_ext/private/file_contexts").read_text()
    if (
        r"/(system_ext|system/system_ext)/bin/kernarium_time_zone\.sh  "
        "u:object_r:kernarium_time_zone_exec:s0"
    ) not in contexts:
        fail("the system_ext file contexts do not label the time zone script")
    properties = (android_root / "overlay/sepolicy/system_ext/private/property_contexts").read_text()
    if (
        "persist.sys.kernarium.timezone_applied    u:object_r:kernarium_time_zone_prop:s0 exact string"
    ) not in properties:
        fail("the system_ext property contexts do not label the time zone marker")
    blueprint = (tools / "Android.bp").read_text()
    for needle in (
        'name: "kernarium_time_zone_script"',
        'src: "kernarium_time_zone.sh"',
        'init_rc: ["kernarium_time_zone.rc"]',
    ):
        if needle not in blueprint:
            fail(f"overlay/tools/Android.bp lacks {needle}")
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    if len(re.findall(r"^PRODUCT_PACKAGES \+= kernarium_time_zone_script$", product, re.M)) != 1:
        fail("lineage_andarium.mk does not add the time zone script to PRODUCT_PACKAGES exactly once")
    verifier.check_time_zone((tools / "kernarium_time_zone.rc").read_text(), script.read_bytes())

    # What the guest's tzdata knows.
    known = ("GMT", "Asia/Tokyo", "Europe/Paris", "Europe/Berlin", "America/Argentina/Buenos_Aires", "Etc/GMT+5")
    marker = "persist.sys.kernarium.timezone_applied"

    def run(zone: str | None, auto: str, current: str, applied: str = "") -> tuple[
        subprocess.CompletedProcess, list[str], str, str
    ]:
        """Runs the script once; returns the process, the recorded calls, the final zone and marker."""
        with tempfile.TemporaryDirectory(prefix="time-zone-script-") as temporary:
            stubs = Path(temporary)
            calls = stubs / "calls"
            properties = stubs / "properties"
            properties.mkdir()
            (properties / "ro.boot.kernarium.timezone").write_text((zone or "") + "\n")
            (properties / "persist.sys.timezone").write_text(current + "\n")
            (properties / marker).write_text(applied + "\n")
            for name, body in {
                "getprop": f'cat {properties}/"$1" 2>/dev/null || echo\n',
                "setprop": f'echo "setprop $*" >> {calls}\necho "$2" > {properties}/"$1"\n',
                "cmd": (
                    f'echo "cmd $*" >> {calls}\n'
                    'if [ "$1 $2 $3 $4" = "settings get global auto_time_zone" ]; then\n'
                    f'    [ -z "{auto}" ] || echo "{auto}"\n'
                    'elif [ "$1 $2" = "alarm set-timezone" ]; then\n'
                    f'    case " {" ".join(known)} " in\n'
                    f'        *" $3 "*) echo "$3" > {properties}/persist.sys.timezone ;;\n'
                    "    esac\n"
                    "else\n    exit 2\nfi\n"
                ),
                "log": f'echo "log $*" >> {calls}\n',
                "sleep": f'echo "sleep $*" >> {calls}\n',
            }.items():
                (stubs / name).write_text("#!/bin/sh\n" + body)
                (stubs / name).chmod(0o755)
            env = {"PATH": f"{stubs}:/usr/bin:/bin"}
            result = subprocess.run(["/bin/sh", str(script)], env=env, capture_output=True, text=True)
            recorded = calls.read_text().splitlines() if calls.exists() else []
            return (
                result,
                recorded,
                (properties / "persist.sys.timezone").read_text().strip(),
                (properties / marker).read_text().strip(),
            )

    def sets(recorded: list[str]) -> list[str]:
        return [line for line in recorded if line.startswith("cmd alarm set-timezone")]

    def notes(recorded: list[str]) -> list[str]:
        return [line for line in recorded if line.startswith("log ")]

    # A new guest (GMT, or no zone yet, and no marker) takes the host's zone once, with the setting
    # on or unset, and records it; the script says so in one line.
    for current in ("GMT", ""):
        for auto in ("1", "null"):
            result, recorded, final, applied = run("Asia/Tokyo", auto, current)
            if (
                result.returncode != 0
                or final != "Asia/Tokyo"
                or applied != "Asia/Tokyo"
                or sets(recorded) != ["cmd alarm set-timezone Asia/Tokyo"]
                or len(notes(recorded)) != 1
            ):
                fail(f"the time zone script (auto {auto}, was {current!r}) did not take Asia/Tokyo: "
                     f"{recorded} {final} {applied}")
    # It follows a changed host zone while the zone is still the one it applied, and takes every
    # character a name may have.
    for zone in ("Europe/Paris", "America/Argentina/Buenos_Aires", "Etc/GMT+5"):
        result, recorded, final, applied = run(zone, "1", "Asia/Tokyo", "Asia/Tokyo")
        if result.returncode != 0 or final != zone or applied != zone:
            fail(f"the time zone script did not follow the host to {zone}: {recorded} {final} {applied}")
    # A zone the user picked (Settings' or the wizard's picker, which leave the setting on) is
    # never replaced, whether the script had applied one before or the guest predates the marker.
    for applied in ("Asia/Tokyo", ""):
        result, recorded, final, marker_value = run("Europe/Paris", "1", "Europe/Berlin", applied)
        if (
            result.returncode != 0
            or final != "Europe/Berlin"
            or marker_value != applied
            or sets(recorded)
            or len(notes(recorded)) != 1
        ):
            fail(f"the time zone script replaced a chosen zone (marker {applied!r}): "
                 f"{recorded} {final} {marker_value}")
    # With the automatic time zone off nothing is touched, whatever the zone is.
    for applied, current in (("Asia/Tokyo", "Asia/Tokyo"), ("", "GMT"), ("Asia/Tokyo", "Europe/Berlin")):
        result, recorded, final, marker_value = run("Europe/Paris", "0", current, applied)
        if (
            result.returncode != 0
            or final != current
            or marker_value != applied
            or sets(recorded)
            or any(line.startswith("setprop") for line in recorded)
            or len(notes(recorded)) != 1
        ):
            fail(f"the time zone script touched the guest with the setting off: {recorded} {final}")
    # A zone that is already the host's is not set again, and the marker follows it, so a guest whose
    # user picked the host's zone follows the host again.
    for applied in ("", "Asia/Tokyo", "Europe/Berlin"):
        result, recorded, final, marker_value = run("Asia/Tokyo", "1", "Asia/Tokyo", applied)
        if result.returncode != 0 or sets(recorded) or final != "Asia/Tokyo" or marker_value != "Asia/Tokyo":
            fail(f"the time zone script mishandled the host's own zone (marker {applied!r}): "
                 f"{recorded} {final} {marker_value}")
    # A name the guest's tzdata lacks is refused by the platform: the zone and the marker stay, one
    # line says so, and the boot goes on.
    result, recorded, final, applied = run("Pacific/Coyhaique", "1", "Europe/Paris", "Europe/Paris")
    if (
        result.returncode != 0
        or final != "Europe/Paris"
        or applied != "Europe/Paris"
        or sets(recorded) != ["cmd alarm set-timezone Pacific/Coyhaique"]
        or len(notes(recorded)) != 1
        or "Pacific/Coyhaique" not in notes(recorded)[0]
    ):
        fail(f"the time zone script did not keep the zone for an unknown name: {recorded} {final} {applied}")
    # No zone from the host (the standalone launcher, an older application): nothing is read or set.
    result, recorded, final, applied = run(None, "1", "Asia/Tokyo")
    if result.returncode != 0 or recorded or final != "Asia/Tokyo":
        fail(f"the time zone script acted without a host zone: {recorded}")
    # A setting that cannot be read leaves the zone alone and says so.
    result, recorded, final, applied = run("Europe/Paris", "", "GMT")
    if result.returncode == 0 or sets(recorded) or final != "GMT":
        fail(f"the time zone script acted on an unreadable setting: {recorded} {final}")
    # A name that is not a time zone name never reaches cmd.
    for bad in ("Asia/Tokyo;reboot", "../etc", "/Asia/Tokyo", "Asia//Tokyo", "Asia/Tokyo/", "a b", "$(id)",
                "Asia/To`id`"):
        result, recorded, final, applied = run(bad, "1", "GMT")
        if result.returncode != 0 or any(line.startswith("cmd ") for line in recorded) or final != "GMT":
            fail(f"the time zone script passed {bad!r} to cmd: {recorded}")


def verify_setup_wizard_flow(android_root: Path) -> None:
    """A wizard script is the reviewed flow in order, and no page the product hides is in it.

    The scripts are raw files, so the overlay carries them as they are tracked; the verifier walks
    the tracked file from its first action. These cases rewrite the flow the way a careless
    edit or a rebase onto LineageOS's own script would."""
    tool = load_tool(android_root / "tools/verify-target-files.py")
    raw = android_root / "overlay/overlay/packages/apps/SetupWizard/app/src/main/res/raw"
    namespace = tool.SETUP_WIZARD_NAMESPACE
    for name, reviewed in tool.SETUP_WIZARD_FLOWS.items():
        tool.check_setup_wizard_script(name, (raw / name).read_text(encoding="utf-8"), reviewed)
    if set(tool.SETUP_WIZARD_FLOWS["lineage_wizard_script.xml"]) & set(tool.SETUP_WIZARD_HIDDEN):
        fail("the reviewed flow names a page the product hides")
    for page in ("network_setup", "restore", "lineage_settings", "lockscreen_settings", "navigation_settings",
                 "location_settings"):
        if page not in tool.SETUP_WIZARD_HIDDEN:
            fail(f"the wizard page {page} is not on the hidden list")

    def script(*actions: tuple[str, str | None], first: str | None = None, root: str = "WizardScript") -> str:
        body = "".join(
            f'<WizardAction id="{identifier}" wizard:uri="intent:#Intent;action=x;end">'
            + (f'<result wizard:action="{following}" />' if following else "")
            + "</WizardAction>"
            for identifier, following in actions
        )
        start = f' wizard:firstAction="{actions[0][0] if first is None else first}"' if first != "" else ""
        return f'<{root} xmlns:wizard="{namespace}"{start}>{body}</{root}>'

    def refuses(label: str, pattern: str, name: str, text: str) -> None:
        try:
            tool.check_setup_wizard_script(name, text, tool.SETUP_WIZARD_FLOWS[name])
        except SystemExit as stop:
            if not re.search(pattern, str(stop)):
                fail(f"{label}: the refusal does not name its cause: {stop}")
        else:
            fail(f"{label}: the wizard script check accepts it")

    owner = "lineage_wizard_script.xml"
    user = "lineage_wizard_script_user.xml"
    # Theme is the last page: its Finish button ends the wizard, so no closing page follows it.
    flow = [("welcome", "locale"), ("locale", "datetime"), ("datetime", "theme_settings"),
            ("theme_settings", None)]
    tool.check_setup_wizard_script(owner, script(*flow), tool.SETUP_WIZARD_FLOWS[owner])
    for page in tool.SETUP_WIZARD_HIDDEN:
        inserted = [("welcome", "locale"), ("locale", page), (page, "datetime"), ("datetime", "theme_settings"),
                    ("theme_settings", None)]
        refuses(f"{page}-in-the-flow", f"defines pages the product hides: {page}", owner, script(*inserted))
        refuses(f"{page}-left-unreached", f"defines pages the product hides: {page}", owner,
                script(*flow, (page, None)))
    refuses("a-closing-page-after-theme",
            "reaches welcome > locale > datetime > theme_settings > finish, not the reviewed", owner,
            script(("welcome", "locale"), ("locale", "datetime"), ("datetime", "theme_settings"),
                   ("theme_settings", "finish"), ("finish", None)))
    refuses("a-reviewed-page-dropped", "reaches welcome > locale > theme_settings, not the reviewed",
            owner, script(("welcome", "locale"), ("locale", "theme_settings"), ("theme_settings", None)))
    refuses("pages-reordered", "reaches welcome > locale > theme_settings > datetime, not the reviewed",
            owner, script(("welcome", "locale"), ("locale", "theme_settings"), ("theme_settings", "datetime"),
                          ("datetime", None)))
    refuses("a-new-page", "reaches welcome > locale > about > datetime", owner,
            script(("welcome", "locale"), ("locale", "about"), ("about", "datetime"), ("datetime", "theme_settings"),
                   ("theme_settings", None)))
    refuses("an-unreached-page", "defines pages its flow never reaches: about", owner, script(*flow, ("about", None)))
    refuses("a-dangling-result", "the flow reaches 'theme_settings', which the script does not define", owner,
            script(("welcome", "locale"), ("locale", "datetime"), ("datetime", "theme_settings")))
    refuses("no-first-action", "names no firstAction", owner, script(*flow, first=""))
    refuses("a-first-action-that-is-not-defined", "reaches 'start'", owner, script(*flow, first="start"))
    refuses("a-repeated-id", "without a unique id", owner, script(*flow, ("theme_settings", None)))
    refuses("not-a-wizard-script", "is not a WizardScript", owner, script(*flow, root="Other"))
    refuses("not-xml", "is not well-formed XML", owner, "<WizardScript>")
    # The secondary user's flow is welcome and theme.
    user_flow = [("welcome", "theme_settings"), ("theme_settings", None)]
    tool.check_setup_wizard_script(user, script(*user_flow), tool.SETUP_WIZARD_FLOWS[user])
    refuses("a-closing-page-in-the-users-flow", "reaches welcome > theme_settings > finish, not the reviewed",
            user, script(("welcome", "theme_settings"), ("theme_settings", "finish"), ("finish", None)))
    refuses("the-owners-flow-for-a-secondary-user",
            "reaches welcome > locale > datetime > theme_settings, not the reviewed welcome > theme_settings",
            user, script(*flow))
    refuses("the-owners-script-with-the-users-name", "not the reviewed welcome > theme_settings",
            user, script(*flow))
    refuses("location-in-the-users-flow", "defines pages the product hides: location_settings", user,
            script(("welcome", "location_settings"), ("location_settings", "theme_settings"),
                   ("theme_settings", None)))


def verify_launcher_layouts(android_root: Path) -> None:
    """The launcher's default home screen is the dock of Files and Settings and an empty workspace.

    The layouts are compiled into the launcher's overlay, so the verifier reads the tracked text.
    These cases rewrite a layout the way an edit or a return to an earlier layout would."""
    tool = load_tool(android_root / "tools/verify-target-files.py")
    layouts = android_root / "overlay/overlay/packages/apps/Launcher3/res/xml"
    names = sorted(path.name for path in layouts.glob("*.xml"))
    if names != [f"default_workspace_{grid}.xml" for grid in tool.LAUNCHER_GRIDS]:
        fail(f"the tracked launcher layouts are {names}, not one per reviewed grid")
    for grid in tool.LAUNCHER_GRIDS:
        text = (layouts / f"default_workspace_{grid}.xml").read_text(encoding="utf-8")
        tool.check_launcher_layout(grid, grid, text)
        # Files then Settings, centred in the dock: the pair of slots follows the grid's hotseat.
        placed = re.findall(r'launcher:container="(-?\d+)"\s+launcher:screen="(\d+)"\s+launcher:x="(\d+)"\s+launcher:y="(\d+)"', text)
        slots = {"4x5": (1, 2), "5x5": (1, 3), "5x6": (1, 3), "5x7": (1, 3), "6x5": (2, 3), "6x6": (2, 3)}[grid]
        if placed != [("-101", str(slot), str(slot), "0") for slot in slots]:
            fail(f"the {grid} layout does not put Files and Settings in slots {slots} of its dock: {placed}")
        # No item on the workspace: every favorite is in the hotseat container.
        if len(re.findall(r"<favorite\b", text)) != 2 or re.search(r"<(?:appwidget|folder|resolve|shortcut)\b", text):
            fail(f"the {grid} layout holds more than the two dock items")
    sample = (layouts / "default_workspace_5x5.xml").read_text(encoding="utf-8")
    favorites = re.findall(r"    <favorite\n(?:.*\n){5}.*/>\n", sample)
    if len(favorites) != 2:
        fail("the 5x5 layout has no two favorites to rewrite")
    files, settings = favorites

    def refuses(label: str, pattern: str, text: str, grid: str = "5x5") -> None:
        try:
            tool.check_launcher_layout(grid, label, text)
        except SystemExit as stop:
            if not re.search(pattern, str(stop)):
                fail(f"{label}: the refusal does not name its cause: {stop}")
        else:
            fail(f"{label}: the launcher layout check accepts it")

    def with_item(item: str) -> str:
        return sample.replace("</favorites>", item + "\n</favorites>")

    app = ('<favorite launcher:packageName="{p}" launcher:className="{c}" launcher:container="{k}" '
           'launcher:screen="{s}" launcher:x="{x}" launcher:y="{y}"{extra} />')
    calendar = app.format(p="org.lineageos.etar", c="com.android.calendar.AllInOneActivity", k="-101",
                          s=0, x=0, y=0, extra="")
    refuses("an-item-on-the-workspace", "in container -100: the workspace is empty, and only the dock holds items",
            sample.replace('launcher:container="-101"', 'launcher:container="-100"', 1))
    refuses("an-item-with-no-container", "has a favorite with attributes",
            sample.replace(files.splitlines()[3] + "\n", "", 1))
    refuses("a-gallery-in-the-dock", "puts com.android.calendar.AllInOneActivity in the dock",
            with_item(calendar))
    refuses("a-widget", r"holds a <appwidget>",
            with_item('<appwidget launcher:packageName="org.lineageos.twelve" launcher:className="x" '
                      'launcher:container="-100" launcher:screen="0" launcher:x="0" launcher:y="3" '
                      'launcher:spanX="4" launcher:spanY="1" />'))
    refuses("a-resolve-item", r"holds a <resolve>",
            with_item('<resolve launcher:container="-101" launcher:screen="0" launcher:x="0" launcher:y="0" />'))
    refuses("a-folder", r"holds a <folder>",
            with_item('<folder launcher:container="-101" launcher:screen="0" launcher:x="0" launcher:y="0" />'))
    refuses("a-uri-item", "has a favorite with attributes",
            with_item('<favorite launcher:uri="#Intent;action=android.intent.action.MAIN;end" launcher:container="-101" '
                      'launcher:screen="0" launcher:x="0" launcher:y="0" />'))
    refuses("google-play", "names 'vending'",
            with_item(app.format(p="com.android.vending", c="com.android.vending.AssetBrowserActivity", k="-101",
                                 s=0, x=0, y=0, extra="")))
    refuses("a-google-application", "names 'google'",
            with_item(app.format(p="com.google.android.gm", c="com.google.android.gm.ConversationListActivityGmail",
                                 k="-101", s=0, x=0, y=0, extra="")))
    refuses("the-clock", "names 'deskclock'",
            with_item(app.format(p="com.android.deskclock", c="com.android.deskclock.DeskClock", k="-101",
                                 s=0, x=0, y=0, extra="")))
    refuses("a-string-reference", "names '@string/'",
            sample.replace("</favorites>", '<folder launcher:title="@string/x" />\n</favorites>'))
    refuses("an-application-the-image-lacks-a-table-row-for", "not in the reviewed table",
            with_item(app.format(p="org.lineageos.camelot", c="org.lineageos.camelot.MainActivity", k="-101",
                                 s=0, x=0, y=0, extra="")))
    refuses("a-repeated-application", "twice", with_item(files.replace('launcher:screen="1"', 'launcher:screen="0"')
                                                         .replace('launcher:x="1"', 'launcher:x="0"')))
    refuses("files-missing", "lacks com.android.documentsui.LauncherActivity: the dock holds exactly Files and Settings",
            sample.replace(files, ""))
    refuses("settings-missing", "lacks com.android.settings.Settings: the dock holds exactly Files and Settings",
            sample.replace(settings, ""))
    refuses("a-slot-outside-the-dock", r"dock slot 5, outside the 5 slots of the 5x5 dock",
            sample.replace('launcher:screen="3"', 'launcher:screen="5"').replace('launcher:x="3"', 'launcher:x="5"'))
    refuses("a-column-that-is-not-the-slot", r"at \(2, 0\) in slot 3",
            sample.replace('launcher:x="3"', 'launcher:x="2"'))
    refuses("a-dock-row-that-is-not-0", r"at \(3, 1\) in slot 3",
            sample.replace('launcher:y="0" />\n\n</favorites>', 'launcher:y="1" />\n\n</favorites>'))
    refuses("two-items-in-a-slot", r"two items in dock slot 1",
            sample.replace('launcher:screen="3"', 'launcher:screen="1"').replace('launcher:x="3"', 'launcher:x="1"'))
    refuses("a-pair-that-is-not-centred", r"in slots 1 and 2, not the centred 1 and 3 of the 5 slots",
            sample.replace('launcher:screen="3"', 'launcher:screen="2"').replace('launcher:x="3"', 'launcher:x="2"'))
    refuses("files-and-settings-swapped", r"in slots 3 and 1, not the centred 1 and 3 of the 5 slots",
            sample.replace('launcher:screen="1"', 'launcher:screen="9"').replace('launcher:x="1"', 'launcher:x="9"')
            .replace('launcher:screen="3"', 'launcher:screen="1"').replace('launcher:x="3"', 'launcher:x="1"')
            .replace('launcher:screen="9"', 'launcher:screen="3"').replace('launcher:x="9"', 'launcher:x="3"'))
    refuses("a-cell-that-is-not-a-number", "is not a number", sample.replace('launcher:x="1"', 'launcher:x="one"', 1))
    refuses("a-favorite-without-a-class", "has a favorite with attributes",
            sample.replace('        launcher:className="com.android.settings.Settings"\n', "", 1))
    refuses("an-attribute-outside-the-launcher-namespace", "outside the launcher namespace",
            sample.replace('launcher:x="1"', 'x="1"', 1))
    refuses("not-xml", "is not well-formed XML", "<favorites>")
    refuses("not-a-layout", "is not a favorites layout", "<other />")
    # A grid's slots come from its name: the 5x5 pair is not the 6x6 pair.
    refuses("a-wider-dock", r"not the centred 2 and 3 of the 6 slots", sample, grid="6x6")


def verify_default_wallpaper(android_root: Path) -> None:
    """The overlay replaces LineageOS's 1920 pixel tablet wallpaper with a bounded square.

    SystemUI fixes the wallpaper surface to the bitmap, so the 1920 pixel default kept a 14 MiB
    buffer alive behind a window the display's long side wide; the overlay's copy is 1024 pixels
    and carries the same image (the checked source and output digests pin that). The copies live in
    the priority overlay directory, which the product makefile lists before LineageOS's."""
    tool = load_tool(android_root / "tools/default_wallpaper.py")
    tool.check_files(android_root)
    resources = Path(tool.OVERLAY_RESOURCES)
    with tempfile.TemporaryDirectory(prefix="default-wallpaper-") as temporary:
        copy = Path(temporary)
        shutil.copytree(android_root / resources, copy / resources)

        def refuses(label: str, pattern: str, mutate) -> None:
            for directory in tool.VARIANT_DIRECTORIES:
                shutil.copyfile(
                    android_root / resources / directory / tool.FILE_NAME,
                    copy / resources / directory / tool.FILE_NAME,
                )
            mutate()
            try:
                tool.check_files(copy)
            except tool.WallpaperError as error:
                if not re.search(pattern, str(error)):
                    fail(f"{label}: the refusal does not name its cause: {error}")
            else:
                fail(f"{label}: the default wallpaper check accepts it")

        def png(width: int, height: int, depth: int = 8, color: int = 2) -> bytes:
            header = struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, 0)
            return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + header
                    + struct.pack(">I", zlib.crc32(b"IHDR" + header)))

        sw600 = copy / resources / tool.VARIANT_DIRECTORIES[0] / tool.FILE_NAME
        sw720 = copy / resources / tool.VARIANT_DIRECTORIES[1] / tool.FILE_NAME
        refuses("lineage-size", r"is 1920x1920: .*square of at most 1024",
                lambda: sw600.write_bytes(png(1920, 1920)))
        refuses("not-square", r"is 1024x768", lambda: sw720.write_bytes(png(1024, 768)))
        refuses("alpha", r"8-bit RGB without alpha", lambda: sw720.write_bytes(png(1024, 1024, 8, 6)))
        refuses("not-png", r"not a PNG", lambda: sw720.write_bytes(b"bytes"))
        refuses("variants-differ", r"differ", lambda: sw720.write_bytes(png(1024, 1024)))
        refuses("missing-variant", r"has no drawable-sw720dp-nodpi/default_wallpaper.png", sw720.unlink)
        refuses("changed-bytes", r"digest is .*: regenerate it",
                lambda: [path.write_bytes(path.read_bytes() + b"\0") for path in (sw600, sw720)])
    # Where the makefile lists the overlay decides who wins: LineageOS's overlay names the same
    # resource, Soong ranks the earlier directory higher, and inherit-product expands the inherited
    # list where its call stands: a directory listed after the calls ranks under LineageOS's.
    tool.check_precedence(android_root)
    makefile_text = (android_root / tool.PRODUCT_MAKEFILE).read_text(encoding="utf-8")
    priority_line = f"PRODUCT_PACKAGE_OVERLAYS += {tool.PRIORITY_OVERLAY}\n"
    ordinary_line = f"PRODUCT_PACKAGE_OVERLAYS += {tool.ORDINARY_OVERLAY}\n"
    first_inherit = "$(call inherit-product, device/google/cuttlefish/vsoc_arm64_pgagnostic/phone/aosp_cf.mk)\n"
    if makefile_text.count(priority_line) != 1 or makefile_text.count(ordinary_line) != 1 \
            or makefile_text.count(first_inherit) != 1:
        fail("the product makefile no longer has the lines the precedence cases rewrite")
    if makefile_text.index(priority_line) > makefile_text.index(first_inherit):
        fail("the product makefile lists the priority overlay after its inherit-product calls")
    if makefile_text.index(ordinary_line) < makefile_text.index(first_inherit):
        fail("the product makefile lists the ordinary overlay before its inherit-product calls")
    with tempfile.TemporaryDirectory(prefix="default-wallpaper-order-") as temporary:
        copy = Path(temporary)
        (copy / "overlay").mkdir()

        def orders(label: str, pattern: str, text: str, ordinary_wallpaper: bool = False) -> None:
            (copy / tool.PRODUCT_MAKEFILE).write_text(text, encoding="utf-8")
            stray = copy / tool.ORDINARY_RESOURCES / tool.VARIANT_DIRECTORIES[0] / tool.FILE_NAME
            if ordinary_wallpaper:
                stray.parent.mkdir(parents=True, exist_ok=True)
                stray.write_bytes(b"x")
            try:
                tool.check_precedence(copy)
            except tool.WallpaperError as error:
                if not re.search(pattern, str(error)):
                    fail(f"{label}: the refusal does not name its cause: {error}")
            else:
                fail(f"{label}: the overlay order check accepts it")
            if stray.exists():
                stray.unlink()

        without = makefile_text.replace(priority_line, "")
        orders("priority-after-inherit", r"after its first inherit-product call",
               without.replace(first_inherit, first_inherit + priority_line))
        orders("priority-missing", r"exactly once", without)
        orders("priority-twice", r"exactly once", makefile_text.replace(priority_line, priority_line * 2))
        orders("priority-commented-out", r"exactly once",
               makefile_text.replace(priority_line, "# " + priority_line))
        orders("ordinary-before-inherit", r"after its inherit-product calls",
               makefile_text.replace(ordinary_line, "").replace(first_inherit, ordinary_line + first_inherit))
        orders("wallpaper-also-in-ordinary", r"also in the ordinary overlay", makefile_text,
               ordinary_wallpaper=True)
        orders("no-inherit-call", r"no inherit-product call",
               makefile_text.replace("$(call inherit-product,", "$(call not-inherit,"))
    # The copy is pinned to the source's mini bitmap: the texels SystemUI's 112 x 112 nearest-neighbour
    # reduction reads from the 1024 pixel image hold what the 1920 pixel image's own samples read, so the
    # Monet palette is the one LineageOS's wallpaper gives. The positions are the ones read on a guest.
    source_side, output_side = tool.SOURCE_SIDE, tool.MAX_SIDE
    if [tool.sample_texels(source_side, k)[0] for k in range(5)] != [8, 25, 42, 59, 77] \
            or tool.sample_texels(source_side, tool.MINI_SIDE - 1) != (1911, 1911) \
            or [tool.sample_texels(output_side, k) for k in range(5)] \
            != [(4, 4), (13, 13), (22, 22), (31, 32), (41, 41)] \
            or tool.sample_texels(output_side, tool.MINI_SIDE - 1) != (1019, 1019):
        fail("the mini bitmap sample positions differ from the ones read on a guest")
    pattern = random.Random(20261031).randbytes(source_side * source_side * 3)
    pinned = bytearray(output_side * output_side * 3)
    changed = tool.pin_samples(pinned, output_side, pattern, source_side)
    columns = [x for k in range(tool.MINI_SIDE) for x in range(tool.sample_texels(output_side, k)[0],
                                                              tool.sample_texels(output_side, k)[1] + 1)]
    if changed != len(columns) ** 2 or sum(1 for i in range(0, len(pinned), 3) if pinned[i:i + 3] != b"\0\0\0") != changed:
        fail(f"the pinned copy changed {changed} texel values, not exactly the {len(columns) ** 2} sample texels")
    for row in range(tool.MINI_SIDE):
        for column in range(tool.MINI_SIDE):
            start = (tool.sample_texels(source_side, row)[0] * source_side
                     + tool.sample_texels(source_side, column)[0]) * 3
            for y in range(tool.sample_texels(output_side, row)[0], tool.sample_texels(output_side, row)[1] + 1):
                for x in range(tool.sample_texels(output_side, column)[0], tool.sample_texels(output_side, column)[1] + 1):
                    if pinned[(y * output_side + x) * 3:(y * output_side + x) * 3 + 3] != pattern[start:start + 3]:
                        fail(f"sample ({row}, {column}) of the pinned copy does not hold the source's sample")
    # A source that is not LineageOS's own file cannot regenerate the committed copy.
    with tempfile.TemporaryDirectory(prefix="default-wallpaper-") as temporary:
        source = Path(temporary) / "source.png"
        source.write_bytes(png(tool.SOURCE_SIDE, tool.SOURCE_SIDE))
        try:
            tool.generate(android_root, source, "magick")
        except tool.WallpaperError as error:
            if "is not LineageOS's default wallpaper" not in str(error):
                fail(f"the regeneration refusal does not name its cause: {error}")
        else:
            fail("a wallpaper that is not LineageOS's regenerates the committed copy")


def verify_overlay_sepolicy(android_root: Path) -> None:
    """The overlay's sepolicy sources keep core domains off vendor-owned properties."""
    tool = android_root / "tools/check-overlay-sepolicy.py"
    overlay = android_root / "overlay"
    subprocess.run([str(tool), "--overlay", str(overlay)], check=True)
    with tempfile.TemporaryDirectory(prefix="overlay-sepolicy-") as scratch:
        copy = Path(scratch) / "overlay"
        shutil.copytree(overlay / "sepolicy", copy / "sepolicy")
        (copy / "config").mkdir()
        shutil.copy(overlay / "config/init.vendor.rc", copy / "config/init.vendor.rc")
        bad = copy / "sepolicy/vendor/adb_host_prop.te"
        bad.write_text(
            "vendor_public_prop(vendor_kernarium_adb_prop)\n"
            "set_prop(adbd, vendor_kernarium_adb_prop)\n"
        )
        result = subprocess.run(
            [str(tool), "--overlay", str(copy)], capture_output=True, text=True
        )
        if result.returncode == 0 or "adbd may be a core domain" not in result.stderr:
            fail(f"the overlay sepolicy check accepted a core domain on a vendor property: {result.stderr!r}")
        # A domain the overlay declares itself, and vendor_init, may set a vendor property.
        bad.write_text(
            "type kernarium_helper, domain;\n"
            "vendor_public_prop(vendor_kernarium_helper_prop)\n"
            "set_prop(kernarium_helper, vendor_kernarium_helper_prop)\n"
            "set_prop(vendor_init, vendor_kernarium_helper_prop)\n"
        )
        subprocess.run([str(tool), "--overlay", str(copy)], check=True)
        bad.write_text(
            "vendor_public_prop(vendor_kernarium_adb_prop)\n"
            "allow system_server vendor_kernarium_adb_prop:property_service { set };\n"
        )
        result = subprocess.run(
            [str(tool), "--overlay", str(copy)], capture_output=True, text=True
        )
        if result.returncode == 0 or "system_server may be a core domain" not in result.stderr:
            fail("the overlay sepolicy check accepted an allow rule of a core domain")
        # A property that a product init trigger names must be readable by vendor_init.
        system_ext = copy / "sepolicy/system_ext/private"
        good = (system_ext / "adb_host_prop.te").read_text()
        for broken, needle in (
            (good.replace("system_public_prop(", "system_internal_prop("), "system_internal_prop"),
            (good.replace("get_prop(vendor_init, kernarium_adb_host_prop)", ""), "lacks get_prop(vendor_init"),
        ):
            bad.unlink(missing_ok=True)
            (system_ext / "adb_host_prop.te").write_text(broken)
            result = subprocess.run(
                [str(tool), "--overlay", str(copy)], capture_output=True, text=True
            )
            if result.returncode == 0 or needle not in result.stderr:
                fail(f"the overlay sepolicy check accepted a trigger on an unreadable property ({needle}): "
                     f"{result.stderr!r}")


def verify_lmkd_gpu_reclaim(android_root: Path) -> None:
    """The overlay, the routed topics and the verifier agree on lmkd's GPU reclaim.

    The target-files fixtures prove what the verifier rejects; this proves that the sources
    that produce the policy, the property and the lmkd binary are what the verifier looks for."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    overlay = android_root / "overlay"
    patches = android_root / "patches/projects"
    # The report file's type is public and a sysfs type; the vendor labels the file with it.
    public = (overlay / "sepolicy/system_ext/public/sysfs_gpu_memory.te").read_text()
    if f"type {verifier.LMKD_GPU_REPORT_TYPE}, fs_type, sysfs_type;" not in public:
        fail("the public system_ext policy does not declare the report file's type as a sysfs type")
    if re.search(r"^\s*(allow|neverallow|dontaudit)\b", public, re.MULTILINE):
        fail("the public system_ext policy holds a rule: only the private policy may")
    genfs = (overlay / "sepolicy/vendor/genfs_contexts").read_text()
    label = (
        f"genfscon sysfs {verifier.LMKD_GPU_REPORT_GENFS_PATH} "
        f"u:object_r:{verifier.LMKD_GPU_REPORT_TYPE}:s0"
    )
    if genfs.splitlines().count(label) != 1:
        fail(f"overlay/sepolicy/vendor/genfs_contexts lacks the line '{label}' exactly once")
    device = f"genfscon sysfs {verifier.LMKD_GPU_REPORT_GENFS_PATH.rsplit('/drm/', 1)[0]} "
    if not any(line.startswith(device) for line in genfs.splitlines()):
        fail("the report file's path is not under a labelled virtio-gpu device")
    private = (overlay / "sepolicy/system_ext/private/lmkd_gpu_reclaim.te").read_text()
    for needle in (
        f"allow lmkd {verifier.LMKD_GPU_DEVICE_TYPE}:dir search;",
        f"allow lmkd {verifier.LMKD_GPU_REPORT_TYPE}:file rw_file_perms;",
    ):
        if needle not in private:
            fail(f"the private system_ext policy lacks '{needle}'")
    board = (overlay / "BoardConfig.mk").read_text()
    if len(re.findall(
        r"^SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS \+= device/kernarium/andarium/sepolicy/system_ext/public$",
        board, re.MULTILINE,
    )) != 1:
        fail("BoardConfig.mk does not add the public system_ext policy directory exactly once")
    # The product turns the feature on, and the build refuses a product that does not.
    product = (overlay / "lineage_andarium.mk").read_text()
    setting = verifier.LMKD_GPU_RECLAIM_PROPERTY
    if len(re.findall(
        rf"^PRODUCT_PRODUCT_PROPERTIES \+= \\\n    {re.escape(setting)}=true$", product, re.MULTILINE
    )) != 1:
        fail(f"lineage_andarium.mk does not set {setting}=true in the product properties exactly once")
    script = (android_root / "build/build-aosp.sh").read_text()
    if f'*" {setting}=true "*' not in script:
        fail(f"build-aosp.sh does not require {setting}=true")
    # The routed topics put in the binary the names the verifier looks for, and label the setting.
    lmkd = "".join(path.read_text() for path in sorted((patches / "p0027/series").glob("*.patch")))
    for marker in verifier.LMKD_GPU_MARKERS:
        if f'"{marker.decode()}"' not in lmkd:
            fail(f"the p0027 topic does not hold the string {marker.decode()!r} that the verifier requires")
    sepolicy = "".join(path.read_text() for path in sorted((patches / "p0020/series").glob("*.patch")))
    if f"+{setting}" not in sepolicy or "lmkd_config_prop" not in sepolicy:
        fail(f"the p0020 topics do not label {setting}")
    projects = (android_root / "patches/projects.tsv").read_text().splitlines()
    if [line.split("\t")[4] for line in projects if line.startswith("25\t")] != ["system/memory/lmkd"]:
        fail("the route table does not route system/memory/lmkd as its 25th route")


NETWORK_STACK_PROJECT = "platform/packages/modules/NetworkStack"
NETWORK_MONITOR_FILE = "src/com/android/server/connectivity/NetworkMonitor.java"


def network_stack_topic_problems(patch: str, markers: list[str]) -> list[str]:
    """What the p0029 patch lacks of the Wi-Fi check: it changes NetworkMonitor alone, it holds the
    names the verifier looks for, it applies to the Wi-Fi transport and to nothing else (not to a
    network under the test URLs that CTS sets), it posts the check after validation, it takes the
    check's result apart from the portal states, it notes the HTTPS probe of a failed check, and it
    takes no transition out of the evaluation."""
    problems = []
    touched = re.findall(r"^diff --git a/(\S+) b/", patch, re.MULTILINE)
    if touched != [NETWORK_MONITOR_FILE]:
        problems.append(f"it changes {touched}, not {NETWORK_MONITOR_FILE} alone")
    added = "\n".join(
        line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")
    )
    removed = "\n".join(
        line[1:] for line in patch.splitlines() if line.startswith("-") and not line.startswith("---")
    )
    for marker in markers:
        if f'"{marker}"' not in added:
            problems.append(f"it does not add the string {marker!r} that the verifier requires")
    for needle, what in (
        ("hasTransport(NetworkCapabilities.TRANSPORT_WIFI)", "apply to the Wi-Fi transport"),
        ("!mNetworkCapabilities.hasTransport(TRANSPORT_TEST)", "leave a test network alone"),
        ("hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)", "leave a VPN alone"),
        ("mTestCaptivePortalHttpsUrl == null", "leave a network under the HTTPS test URL alone"),
        ("mTestCaptivePortalHttpUrl == null", "leave a network under the HTTP test URL alone"),
        ("mTestCaptivePortalFallbackUrl == null", "leave a network under the fallback test URL alone"),
        ("sendMessage(CMD_WIFI_CHECK)", "post the check once the network is validated"),
        ("transitionTo(mProbingState)", "probe the network when a check is due"),
        ("handleWifiCheckResult(probeResult)", "take a check's result apart from the portal states"),
        ("noteProbeResult(NETWORK_VALIDATION_PROBE_HTTPS", "note the HTTPS probe of a failed check"),
        ("&& !isWifiExemptFromValidation()", "keep a Terms and Conditions page from entering the portal state"),
        ("&& isValidationRequiredByStockRules();", "leave a network that the stock rules do not validate alone"),
        ("isWifiExemptFromValidation() && getCurrentState() == mValidatedState",
         "force a check only on a network that is in the validated state"),
        ("// An evaluation that reaches private DNS is not a check of a validated network.\n"
         "            mWifiCheckDue = false;",
         "drop the check that is due when an evaluation reaches private DNS"),
        ("if (isWifiExemptFromValidation() && mWifiCheckDue) {\n"
         "                        deferMessage(message);",
         "defer a request to evaluate private DNS that reaches a check"),
        ("sendMessageDelayed(CMD_WIFI_CHECK, delayMs)", "retry a failed check after a back-off"),
        ("if (hasMessages(CMD_WIFI_CHECK)) return;", "keep the time of a check that is waiting"),
        ('validationLog("Wi-Fi internet check failed, the network stays validated");\n'
         "        }\n        transitionToPrivateDnsEvaluationSuccessState();",
         "return from a completed check to the validated state without evaluating private DNS"),
        ("privateDnsResults != 0);", "keep the last private DNS probe's result in the report of a check"),
    ):
        if needle not in added:
            problems.append(f"it does not {what}")
    check_result = added.split("private void handleWifiCheckResult", 1)[-1].split("return !isWifiExemptFromValidation()", 1)[0]
    if "mEvaluatingPrivateDnsState" in check_result:
        problems.append("a completed check evaluates private DNS again")
    if "transitionTo" in removed:
        problems.append("it removes a transition of the evaluation")
    return problems


def verify_camerax_denylist_assertion(android_root: Path) -> None:
    """build-aosp.sh requires the inherited CameraX extensions switch in the vendor property denylist.

    The switch stays in PRODUCT_VENDOR_PROPERTIES, which the product cannot edit: the denylist drops it
    when the build writes the vendor property file, and the target-files verifier requires its absence
    there. The statement is run on its own, with a stubbed get_build_var, against the cases that decide
    it: the entry present beside others, absent, empty and a near match."""
    script = (android_root / "build/build-aosp.sh").read_text()
    property_name = "ro.camerax.extensions.enabled"
    statement = re.search(
        r'^\[\[ " \$\(get_build_var PRODUCT_VENDOR_PROPERTY_BLACKLIST\) " == \*" '
        + re.escape(property_name) + r' "\* \]\] \|\|\n\tfail "[^"\n]*"$',
        script, re.MULTILINE)
    if statement is None:
        fail("build-aosp.sh no longer requires the CameraX extensions switch in the vendor property denylist")
    if re.search(r'PRODUCT_VENDOR_PROPERTIES" != \*" ' + re.escape(property_name), script):
        fail("build-aosp.sh requires the inherited CameraX extensions switch to be absent from "
             "PRODUCT_VENDOR_PROPERTIES, which an inherited property never is")
    for denylist, accepted in (
        (property_name, True),
        (f"ro.a {property_name} ro.b", True),
        ("", False),
        ("ro.a ro.b", False),
        (f"{property_name}2", False),
        (f"x{property_name}", False),
        ("ro.camerax.extensions", False),
    ):
        result = subprocess.run(
            ["bash", "-c", 'get_build_var() { [[ $1 == PRODUCT_VENDOR_PROPERTY_BLACKLIST ]] && printf %s "$DENYLIST"; }\n'
             'fail() { echo "failed: $*"; exit 1; }\n' + statement.group(0)],
            env={**os.environ, "DENYLIST": denylist}, capture_output=True, text=True, check=False)
        if (result.returncode == 0) != accepted:
            fail(f"build-aosp.sh's CameraX denylist assertion answers {result.returncode} for the denylist "
                 f"{denylist!r}, not {'0' if accepted else '1'}: {result.stdout}{result.stderr}")


def snapshot_tree(root: Path) -> dict[str, tuple[str, object]]:
    """Every entry under root: a file's bytes, a symlink's target, a FIFO or a directory.

    Symlinked directories are listed, never entered."""
    state: dict[str, tuple[str, object]] = {}
    for directory, names, files in os.walk(root):
        for name in names + files:
            path = Path(directory) / name
            key = path.relative_to(root).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                state[key] = ("link", os.readlink(path))
            elif stat.S_ISREG(mode):
                state[key] = ("file", path.read_bytes())
            elif stat.S_ISFIFO(mode):
                state[key] = ("fifo", None)
            else:
                state[key] = ("directory", None)
    return state


def verify_stale_build_properties(android_root: Path) -> None:
    """build-aosp.sh removes the property files an earlier build number left in the output directory
    and refuses a build that leaves one.

    The step is build/build-number-properties.sh, which the script sources. It runs here against a
    stand-in output tree, with stand-ins for GNU timeout (absent on a macOS host) and for find, which
    refuses to run outside the timeout so that no listing is unbounded. The same tree is checked
    against two build numbers, so that a comparison fixed to one of them fails."""
    build = android_root / "build"
    product_path = "target/product/andarium"
    numbers = ("26", "27")

    def prop(*lines: str) -> bytes:
        return ("ro.build.id=CP2A\n" + "".join(f"{line}\n" for line in lines) + "ro.build.version.sdk=37\n").encode()

    def entry(relative: str, content: bytes, stale_26: bool, stale_27: bool) -> tuple[str, bytes, dict[str, bool]]:
        return relative, content, {"26": stale_26, "27": stale_27}

    key = "ro.build.version.incremental"
    odm_key = "ro.odm.build.version.incremental"
    vendor_key = "ro.vendor.build.version.incremental"
    product_key = "ro.product.build.version.incremental"
    system_ext_key = "ro.system_ext.build.version.incremental"
    entries = [
        # Installed property files that name another number than 26, or than 27.
        entry(f"{product_path}/vendor/build.prop", prop(f"{vendor_key}=25"), True, True),
        entry(f"{product_path}/system/build.prop", prop(f"{key}=25"), True, True),
        entry(f"{product_path}/product/build.prop", prop(f"{product_key}=2"), True, True),
        entry(f"{product_path}/system_ext/build.prop", prop(f"{system_ext_key}=260"), True, True),
        entry(f"{product_path}/ramdisk/prop.default", prop(f"{key}=eng.builder"), True, True),
        entry(f"{product_path}/odm dir/build.prop", prop(f"{key}=25"), True, True),
        entry(f"{product_path}/odm/build.prop", prop(f"{odm_key}=26"), False, True),
        entry(f"{product_path}/vendor/etc/prop.default", prop(f"{key}=26"), False, True),
        entry(f"{product_path}/etc7/build.prop", prop(f"{key}=27"), True, False),
        # How a property is read: split at the first "=", blanks and a carriage return trimmed, the
        # whole value compared as a string.
        entry(f"{product_path}/etc-blanks/build.prop", prop(f"{odm_key} = 25"), True, True),
        entry(f"{product_path}/etc-second-equals/build.prop", prop(f"{odm_key}=26=old"), True, True),
        entry(f"{product_path}/etc-crlf/build.prop", f"{odm_key}=26\r\nro.x=1\r\n".encode(), False, True),
        entry(f"{product_path}/etc-trailing/build.prop",
              prop(f"{odm_key}=26   ", f"  {vendor_key}  =  26  "), False, True),
        entry(f"{product_path}/etc-leading-zero/build.prop", prop(f"{key}=026"), True, True),
        # A stale key between current ones, and the other way round.
        entry(f"{product_path}/etc-middle/build.prop",
              prop(f"{key}=26", f"{product_key}=25", f"{system_ext_key}=26"), True, True),
        entry(f"{product_path}/etc-first/build.prop",
              prop(f"{key}=25", f"{product_key}=26", f"{system_ext_key}=26"), True, True),
        # No incremental property, or one of another name, names no number.
        entry(f"{product_path}/vendor/etc/build.prop",
              b"ro.vendor.build.id=CP2A\nro.vendor.build.version.incrementalx=25\nro.incremental.enable=1\n",
              False, False),
        # A file of another name is not a property file, and obj/ is the target-files rule's.
        entry(f"{product_path}/vendor/build.prop.bak", prop(f"{key}=25"), False, False),
        entry(f"{product_path}/obj/PACKAGING/target_files_intermediates/x/SYSTEM/build.prop",
              prop(f"{key}=25"), False, False),
        entry(f"{product_path}/obj/ETC/vendor_intermediates/build.prop", prop(f"{key}=25"), False, False),
        # Soong's intermediates are searched before the build only.
        entry("soong/.intermediates/build/soong/system-build.prop/android_common/build.prop",
              prop(f"{key}=25"), True, True),
        entry("soong/.intermediates/build/soong/ramdisk-prop.default/android_common/prop.default",
              prop(f"{key}=25"), True, True),
        entry("soong/.intermediates/build/soong/a dir\nwith newline/build.prop", prop(f"{key}=25"), True, True),
        entry("soong/.intermediates/build/soong/odm-build.prop/android_common/build.prop",
              prop(f"{odm_key}=26"), False, True),
        # Nothing else of the output directory is searched.
        entry("host/linux-x86/build.prop", prop(f"{key}=25"), False, False),
    ]

    def is_installed(relative: str) -> bool:
        return relative.startswith(f"{product_path}/") and not relative.startswith(f"{product_path}/obj/")

    sibling_files = (
        "aosp/out-other/target/product/andarium/vendor/build.prop",
        "aosp/out-other/soong/.intermediates/x/build.prop",
        "outside/linked/build.prop",
    )

    real_find = shutil.which("find")
    if real_find is None:
        fail("the host has no find")
    shims = {
        "timeout": '#!/bin/bash\n'
                   'printf "%s\\n" "$*" >>"$SHIM_LOG"\n'
                   '[[ "${1:-}" != --foreground ]] || shift\n'
                   'if [[ "${1:-}" == -k ]]; then shift 2; fi\n'
                   '[[ "${1:-}" =~ ^[1-9][0-9]*$ ]] || exit 125\n'
                   'shift\n'
                   '[[ -z "${SHIM_TIMEOUT_STATUS:-}" ]] || exit "$SHIM_TIMEOUT_STATUS"\n'
                   '[[ -z "${SHIM_TIMEOUT_HANG:-}" ]] || exec sleep 60\n'
                   'if [[ -n "${SHIM_TIMEOUT_AFTER:-}" ]]; then\n'
                   '    UNDER_TIMEOUT=1 "$@"\n'
                   '    exit "$SHIM_TIMEOUT_AFTER"\n'
                   'fi\n'
                   'UNDER_TIMEOUT=1 exec "$@"\n',
        "find": '#!/bin/bash\n'
                '[[ -n "${UNDER_TIMEOUT:-}" ]] || { echo "find ran without a timeout" >&2; exit 97; }\n'
                'unset UNDER_TIMEOUT\n'
                f'exec {shlex.quote(real_find)} "$@"\n',
    }
    failing_rm = '#!/bin/bash\n' \
                 'for argument in "$@"; do [[ "$argument" != "$SHIM_RM_FAILS" ]] || exit 1; done\n' \
                 'exec /bin/rm "$@"\n'

    with tempfile.TemporaryDirectory(prefix="stale-build-properties-") as temporary:
        base = Path(temporary).resolve()
        shim_dir = base / "shims"
        shim_dir.mkdir()
        for name, text in shims.items():
            (shim_dir / name).write_text(text)
            (shim_dir / name).chmod(0o755)
        rm_dir = base / "failing-rm"
        rm_dir.mkdir()
        (rm_dir / "rm").write_text(failing_rm)
        (rm_dir / "rm").chmod(0o755)
        log = base / "timeout.log"

        def build_tree(name: str) -> tuple[Path, Path, Path]:
            root = base / name
            out = root / "aosp/out-user"
            for relative, content, _ in entries:
                path = out / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            for relative in sibling_files:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(prop(f"{key}=25"))
            (out / product_path / "odm/prop.default").symlink_to(root / "outside/linked/build.prop")
            return root, out, out / product_path

        environment_base = {**os.environ, "PATH": f"{shim_dir}{os.pathsep}{os.environ['PATH']}", "SHIM_LOG": str(log)}

        def command(function: str, out: object, product: object, number: str) -> list[str]:
            return ["bash", "-c", f'set -euo pipefail\nsource "$1"\nsource "$2"\n{function} "$3" "$4" "$5"',
                    "build-aosp.sh", str(build / "common.sh"), str(build / "build-number-properties.sh"),
                    str(out), str(product), number]

        def run(function: str, out: object, product: object, number: str = "26",
                **environment: str) -> subprocess.CompletedProcess[str]:
            log.write_text("")
            return subprocess.run(command(function, out, product, number), env={**environment_base, **environment},
                                  stdin=subprocess.DEVNULL, capture_output=True, text=True, check=False, timeout=120)

        def listings() -> list[str]:
            lines = log.read_text().splitlines()
            if any(not re.fullmatch(r"--foreground -k 10 1800 find \. .*", line) for line in lines):
                fail(f"a listing is not bounded by timeout --foreground, or not run on relative patterns: {lines}")
            if any(str(base) in line for line in lines):
                fail(f"a listing passes an absolute path to find: {lines}")
            return lines

        def expected_stale(number: str, installed_only: bool = False) -> list[str]:
            return [relative for relative, _, stale in entries
                    if stale[number] and (is_installed(relative) or not installed_only)]

        def named_in(result: subprocess.CompletedProcess[str]) -> list[str]:
            return sorted(line.strip() for line in result.stderr.splitlines() if line.startswith("\t"))

        def scratch_files(root: Path) -> list[str]:
            return [name for _, names, files in os.walk(root) for name in names + files
                    if ".build-number-properties" in name]

        # Before the build, for each of two numbers on the same tree, in an output path that holds
        # glob characters and blanks: the stale files go by their found paths and nothing else changes.
        for directory, number in (("remove-26", "26"), ("remove-27", "27"), ("glob [x]*? dir/remove", "26")):
            root, out, product = build_tree(directory)
            before = snapshot_tree(root)
            removed = expected_stale(number)
            for relative in removed:
                del before[f"aosp/out-user/{relative}"]
            result = run("remove_stale_build_properties", out, product, number)
            if result.returncode != 0:
                fail(f"removing the property files that name another number than {number} failed: "
                     f"{result.stdout}{result.stderr}")
            if snapshot_tree(root) != before:
                fail(f"for build number {number} in {directory!r}, the removal left a stale file or touched "
                     "a current one, a file of another name, obj/, a link, another output directory "
                     "or the outside of the product's installed tree and Soong's intermediates")
            for relative in removed:
                if str(out / relative) not in result.stdout:
                    fail(f"the removal for build number {number} does not name {relative}: {result.stdout}")
            if f"removed {len(removed)} property files that name another build number than {number}" \
                    not in result.stdout:
                fail(f"the removal for build number {number} does not count its files: {result.stdout}")
            lines = listings()
            if len(lines) != 2 or " -path ./obj -prune -o " not in lines[0] or "-prune" in lines[1]:
                fail(f"the removal does not list the installed tree without obj/, then Soong's intermediates: {lines}")
            again = run("remove_stale_build_properties", out, product, number)
            if (again.returncode, again.stdout, again.stderr) != (0, "", ""):
                fail(f"a second removal is not a no-op: {again.returncode} {again.stdout}{again.stderr}")

        # The same output through a link, as a symlinked workspace is: only the search roots must be
        # physical directories of the output.
        root, out, product = build_tree("alias")
        alias = root / "alias-out"
        alias.symlink_to(out)
        before = snapshot_tree(root)
        for relative in expected_stale("26"):
            del before[f"aosp/out-user/{relative}"]
        result = run("remove_stale_build_properties", alias, alias / product_path)
        if result.returncode != 0 or snapshot_tree(root) != before:
            fail(f"an output directory reached through a link is not cleaned: {result.stdout}{result.stderr}")

        # The same when the output's own name ends in a newline, beside a directory named without it:
        # the physical path keeps the newline, so the step works and never enters the other directory.
        root, out, product = build_tree("newline")
        out.rename(out.with_name(out.name + "\n"))
        out.mkdir()
        alias = root / "alias-out"
        alias.symlink_to(out.with_name(out.name + "\n"))
        before = snapshot_tree(root)
        for relative in expected_stale("26"):
            del before[f"aosp/out-user\n/{relative}"]
        result = run("remove_stale_build_properties", alias, alias / product_path)
        if result.returncode != 0 or snapshot_tree(root) != before:
            fail(f"an output directory whose name ends in a newline is not cleaned: {result.stdout}{result.stderr}")

        # With no output directory, or one the build has not filled, it does nothing.
        missing = base / "missing/out-user"
        result = run("remove_stale_build_properties", missing, missing / product_path)
        if (result.returncode, result.stdout, result.stderr) != (0, "", "") or missing.exists() or listings():
            fail(f"an absent output directory is not left alone: {result.returncode} {result.stdout}{result.stderr}")
        empty = base / "empty/out-user"
        empty.mkdir(parents=True)
        for function in ("remove_stale_build_properties", "verify_build_number_properties"):
            result = run(function, empty, empty / product_path)
            if (result.returncode, result.stdout, result.stderr) != (0, "", "") or any(empty.iterdir()):
                fail(f"{function} leaves an empty output directory changed: "
                     f"{result.returncode} {result.stdout}{result.stderr}")
        root, out, product = build_tree("outside-product")
        result = run("remove_stale_build_properties", out, root / "outside" / "product")
        if result.returncode == 0 or "not inside the output directory" not in result.stderr:
            fail(f"a product output outside the output directory is accepted: {result.stdout}{result.stderr}")

        # A search root that is a link, or behind one, or not a normalized path, fails the step
        # before any listing, for the removal and for the scan, and nothing is deleted anywhere.
        for label, link_relative, link_target, functions in (
            ("soong", "soong", "aosp/out-other/soong", ("remove_stale_build_properties",)),
            ("target", "target", "aosp/out-other/target",
             ("remove_stale_build_properties", "verify_build_number_properties")),
            ("product", product_path, "aosp/out-other/target/product/andarium",
             ("remove_stale_build_properties", "verify_build_number_properties")),
        ):
            root, out, product = build_tree(f"link-{label}")
            shutil.rmtree(out / link_relative)
            (out / link_relative).symlink_to(root / link_target)
            before = snapshot_tree(root)
            for function in functions:
                result = run(function, out, product)
                if result.returncode == 0 or f"is behind a symbolic link: {out / link_relative}" not in result.stderr:
                    fail(f"{function} accepts a symlinked {label} directory, or does not name it: "
                         f"{result.returncode} {result.stdout}{result.stderr}")
                if snapshot_tree(root) != before or listings():
                    fail(f"{function} listed or changed a tree whose {label} directory is a link")
        root, out, product = build_tree("dot-dot")
        before = snapshot_tree(root)
        for function in ("remove_stale_build_properties", "verify_build_number_properties"):
            result = run(function, out, f"{out}/target/product/../product/andarium")
            if result.returncode == 0 or "not a normalized path" not in result.stderr \
                    or snapshot_tree(root) != before or listings():
                fail(f"{function} accepts a product path with a '..' component: {result.stdout}{result.stderr}")

        # A removal that fails fails the script, naming the file, and leaves no scratch file.
        root, out, product = build_tree("remove-fails")
        before = snapshot_tree(root)
        victim = out / f"{product_path}/vendor/build.prop"
        result = run("remove_stale_build_properties", out, product,
                     PATH=f"{rm_dir}{os.pathsep}{shim_dir}{os.pathsep}{os.environ['PATH']}",
                     SHIM_RM_FAILS=str(victim))
        if result.returncode == 0 or f"cannot remove the property file that names another build number: {victim}" \
                not in result.stderr or not result.stderr.startswith("android/lineage-24/build/build-aosp.sh: "):
            fail(f"a failed removal does not fail the script: {result.returncode} {result.stdout}{result.stderr}")
        if not victim.exists() or scratch_files(root):
            fail("a failed removal leaves its file removed or a scratch file behind")
        if any(name not in before for name in snapshot_tree(root)):
            fail("a failed removal created a file")

        # A listing that fails or times out is an error, even after find ran, and removes nothing.
        root, out, product = build_tree("listing-fails")
        before = snapshot_tree(root)
        for function in ("remove_stale_build_properties", "verify_build_number_properties"):
            for variable in ("SHIM_TIMEOUT_STATUS", "SHIM_TIMEOUT_AFTER"):
                result = run(function, out, product, **{variable: "124"})
                if result.returncode == 0 or "cannot list the property files" not in result.stderr \
                        or "exit status 124" not in result.stderr:
                    fail(f"{function} accepts a listing that timed out ({variable}): "
                         f"{result.returncode} {result.stdout}{result.stderr}")
                if snapshot_tree(root) != before:
                    fail(f"{function} changed the tree, or left a scratch file, after a listing that timed out "
                         f"({variable})")

        # A property file that cannot be read is an error, not a file that is current.
        if os.geteuid() != 0:
            root, out, product = build_tree("unreadable")
            unreadable = out / f"{product_path}/vendor/build.prop"
            unreadable.chmod(0)
            try:
                for function in ("remove_stale_build_properties", "verify_build_number_properties"):
                    result = run(function, out, product)
                    if result.returncode == 0 or f"cannot read the property file {unreadable}" not in result.stderr:
                        fail(f"{function} accepts a property file it cannot read: "
                             f"{result.returncode} {result.stdout}{result.stderr}")
                    if scratch_files(root):
                        fail(f"{function} leaves a scratch file behind after an unreadable property file")
            finally:
                unreadable.chmod(0o644)

        # No fixed scratch name: a FIFO or a link at the name an earlier version used neither blocks
        # the step nor is written through, and two runs do not share a list.
        root, out, product = build_tree("scratch-fifo")
        os.mkfifo(out / ".build-number-properties.list")
        before = snapshot_tree(root)
        for relative in expected_stale("26"):
            del before[f"aosp/out-user/{relative}"]
        result = run("remove_stale_build_properties", out, product)
        if result.returncode != 0 or snapshot_tree(root) != before:
            fail(f"a FIFO in the output directory changes the removal: {result.returncode} {result.stdout}{result.stderr}")
        root, out, product = build_tree("scratch-link")
        victim_file = root / "outside/victim.txt"
        victim_file.write_text("keep\n")
        (out / ".build-number-properties.list").symlink_to(victim_file)
        before = snapshot_tree(root)
        result = run("verify_build_number_properties", out, product)
        if result.returncode == 0 or victim_file.read_text() != "keep\n" or snapshot_tree(root) != before:
            fail(f"a link in the output directory is written through, or changes the scan: {result.stderr}")

        # An interrupt during a listing ends the step and removes its scratch files.
        root, out, product = build_tree("interrupted")
        before = snapshot_tree(root)
        log.write_text("")
        process = subprocess.Popen(
            command("remove_stale_build_properties", out, product, "26"),
            env={**environment_base, "SHIM_TIMEOUT_HANG": "1"}, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            deadline = time.monotonic() + 30
            while len(scratch_files(root)) < 2 and time.monotonic() < deadline:
                time.sleep(0.05)
            if len(scratch_files(root)) < 2:
                fail("the step creates no scratch files while it lists")
            os.killpg(process.pid, signal.SIGTERM)
            deadline = time.monotonic() + 30
            while scratch_files(root) and time.monotonic() < deadline:
                time.sleep(0.05)
            if scratch_files(root) or snapshot_tree(root) != before:
                fail("an interrupted step leaves a scratch file behind, or changed the tree")
        finally:
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=30)

        # After the build, for each of two numbers: an installed file that names another number refuses
        # the build, naming exactly those files, and only the installed tree is scanned.
        for number in numbers:
            root, out, product = build_tree(f"verify-{number}")
            before = snapshot_tree(root)
            result = run("verify_build_number_properties", out, product, number)
            expected = sorted(str(out / relative) for relative in expected_stale(number, installed_only=True))
            if result.returncode == 0 or named_in(result) != expected:
                fail(f"the scan for build number {number} does not refuse exactly the stale installed files: "
                     f"{result.returncode} {named_in(result)} {result.stdout}{result.stderr}")
            if snapshot_tree(root) != before or len(listings()) != 1:
                fail("the scan changes the tree or does not list the installed tree exactly once")
        result = run("remove_stale_build_properties", out, product, "27")
        if result.returncode != 0:
            fail(f"the removal failed on the scan's tree: {result.stdout}{result.stderr}")
        result = run("verify_build_number_properties", out, product, "27")
        if (result.returncode, result.stdout, result.stderr) != (0, "", ""):
            fail(f"the scan refuses a tree whose stale files were removed: {result.stdout}{result.stderr}")
        stray = out / product_path / "system/build.prop"
        stray.write_bytes(prop(f"{key}=26"))
        result = run("verify_build_number_properties", out, product, "27")
        if result.returncode == 0 or named_in(result) != [str(stray)]:
            fail(f"the scan does not name exactly the stale file left by the build: {result.stderr}")

    # build-aosp.sh sources the helper, and its steps keep their order: removal after the number is
    # exported and before the first build, the scan after the last one, and neither without a number.
    script = (build / "build-aosp.sh").read_text()
    guard = 'if [[ -n "$BUILD_NUMBER_VALUE" ]]; then\n\t'
    positions = [
        script.find('source "$SCRIPT_DIR/build-number-properties.sh"'),
        script.find("export BUILD_NUMBER=$BUILD_NUMBER_VALUE"),
        script.find(guard + 'remove_stale_build_properties "$OUT_DIR_ABSOLUTE" "$PRODUCT_OUT_ABSOLUTE" '
                    '"$BUILD_NUMBER_VALUE"\nfi\n'),
        script.find('m -j"$JOBS" target-files-package check_target_files_vintf'),
        script.rfind('m -j"$JOBS" target-files-package\n'),
        script.find(guard + 'verify_build_number_properties "$OUT_DIR_ABSOLUTE" "$PRODUCT_OUT_ABSOLUTE" '
                    '"$BUILD_NUMBER_VALUE"\nfi\n'),
        script.find('DEXPREOPT_CONFIG="'),
    ]
    if -1 in positions or any(earlier >= later for earlier, later in zip(positions, positions[1:])):
        fail(f"build-aosp.sh does not remove the stale property files before its build and scan after it: {positions}")


def verify_network_stack_topic(android_root: Path) -> None:
    """The routed NetworkMonitor topic and the verifier agree on the Wi-Fi check.

    The target-files fixtures prove what the verifier rejects; this proves that the topic puts in the
    APK the names the verifier looks for, and that its check reaches a Wi-Fi network and no other."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    rows = [
        line.split("\t")
        for line in (android_root / "patches/projects.tsv").read_text().splitlines()
        if line and not line.startswith("#")
    ][1:]
    routes = [row for row in rows if row[3] == NETWORK_STACK_PROJECT]
    if [(row[1], row[4]) for row in routes] != [("p0029", "packages/modules/NetworkStack")]:
        fail("the route table does not route packages/modules/NetworkStack once, as p0029")
    patches = sorted((android_root / "patches/projects/p0029/series").glob("*.patch"))
    if len(patches) != 1:
        fail(f"p0029 has {len(patches)} patches, not one")
    patch = patches[0].read_text()
    markers = [marker.decode() for marker in verifier.NETWORK_STACK_MARKERS]
    if len(markers) != 2:
        fail("the verifier no longer names exactly two NetworkMonitor markers")
    problems = network_stack_topic_problems(patch, markers)
    if problems:
        fail("the NetworkMonitor topic: " + "; ".join(problems))
    mutations = [
        (f"a topic without the verifier's string {marker!r}", patch.replace(marker, "Wi-Fi is checked"))
        for marker in markers
    ]
    mutations += [
        ("a topic that skips every transport",
         patch.replace("hasTransport(NetworkCapabilities.TRANSPORT_WIFI)", "hasTransport(0)")),
        ("a topic that exempts test networks",
         patch.replace("!mNetworkCapabilities.hasTransport(TRANSPORT_TEST)", "true")),
        ("a topic that exempts a VPN",
         patch.replace("hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)", "isEmpty()")),
        ("a topic that exempts a network under the HTTPS test URL",
         patch.replace("mTestCaptivePortalHttpsUrl == null", "true")),
        ("a topic that exempts a network under the HTTP test URL",
         patch.replace("mTestCaptivePortalHttpUrl == null", "true")),
        ("a topic that exempts a network under the fallback test URL",
         patch.replace("mTestCaptivePortalFallbackUrl == null", "true")),
        ("a topic that never posts the check", patch.replace("sendMessage(CMD_WIFI_CHECK)", "sendMessage(0)")),
        ("a topic that never probes for a check", patch.replace("transitionTo(mProbingState)", "")),
        ("a topic that gives the check's result the portal states",
         patch.replace("handleWifiCheckResult(probeResult)", "probeResult.isPortal()")),
        ("a topic that leaves a failed check's HTTPS probe unnoted",
         patch.replace("noteProbeResult(NETWORK_VALIDATION_PROBE_HTTPS",
                       "noteProbeResult(NETWORK_VALIDATION_PROBE_HTTP")),
        ("a topic that lets a Terms and Conditions page enter the portal state",
         patch.replace("&& !isWifiExemptFromValidation()", "")),
        ("a topic that exempts a network that the stock rules do not validate",
         patch.replace("&& isValidationRequiredByStockRules();", ";")),
        ("a topic that checks when an evaluation is forced, in any state",
         patch.replace("isWifiExemptFromValidation() && getCurrentState() == mValidatedState",
                       "isWifiExemptFromValidation()")),
        ("a topic that checks when an evaluation is forced after the first validation, in any state",
         patch.replace("getCurrentState() == mValidatedState", "mValidations > 0")),
        ("a topic that checks when an evaluation is forced, with the state condition dropped",
         patch.replace("isWifiExemptFromValidation() && getCurrentState() == mValidatedState",
                       "isWifiExemptFromValidation() && true")),
        ("a topic that keeps a check due when an evaluation reaches private DNS",
         patch.replace("+            // An evaluation that reaches private DNS is not a check of a validated network.\n"
                       "+            mWifiCheckDue = false;\n", "")),
        ("a topic that drops the request to evaluate private DNS that reaches a check",
         patch.replace("+                        deferMessage(message);\n+                        return HANDLED;\n",
                       "+                        return HANDLED;\n")),
        ("a topic that never retries a failed check",
         patch.replace("sendMessageDelayed(CMD_WIFI_CHECK, delayMs)", "sendMessage(0, delayMs)")),
        ("a topic that puts a waiting check off",
         patch.replace("if (hasMessages(CMD_WIFI_CHECK)) return;", "removeMessages(CMD_WIFI_CHECK);")),
        ("a topic that evaluates private DNS after every check",
         patch.replace("+        transitionToPrivateDnsEvaluationSuccessState();",
                       "+        transitionTo(mEvaluatingPrivateDnsState);")),
        ("a topic that drops the private DNS probe's result from the report of a check",
         patch.replace("privateDnsResults != 0", "false")),
        ("a topic that also changes another file",
         patch + "diff --git a/src/com/android/networkstack/util/NetworkStackUtils.java b/x\n"),
        ("a topic that removes a transition",
         patch + "-                            transitionTo(mEvaluatingPrivateDnsState);\n"),
    ]
    for label, edited in mutations:
        if not network_stack_topic_problems(edited, markers):
            fail(f"the NetworkMonitor topic check accepted {label}")


WIFI_TRACKER_PROJECT = "platform/frameworks/opt/net/wifi"
WIFI_TRACKER_FILES = [
    "libs/WifiTrackerLib/src/com/android/wifitrackerlib/Utils.java",
    "libs/WifiTrackerLib/src/com/android/wifitrackerlib/WifiEntry.java",
    "libs/WifiTrackerLib/src/com/android/wifitrackerlib/WifiPickerTracker.java",
]
# What the verifier looks for in Settings and SystemUI, and the constant of the connectivity report that
# the topic reads, whose value it is (ConnectivityDiagnosticsManager.ConnectivityReport).
WIFI_TRACKER_MARKER_CONSTANTS = {"networkProbesAttempted": "KEY_NETWORK_PROBES_ATTEMPTED_BITMASK"}


def wifi_tracker_topic_problems(patch: str, markers: list[str]) -> list[str]:
    """What the p0033 patch lacks of the warning: it changes the library's Utils, WifiEntry and
    WifiPickerTracker alone, it reads the key that the verifier looks for, it reads a report that says
    valid with the HTTPS probe attempted and not succeeded, leaves a network that is not validated,
    partial or behind a sign-in page to the stock rules, the icon and the summary both take it, and the
    picker tracker tells its listeners when a report moves the mark."""
    problems = []
    touched = re.findall(r"^diff --git a/(\S+) b/", patch, re.MULTILINE)
    if touched != WIFI_TRACKER_FILES:
        problems.append(f"it changes {touched}, not {WIFI_TRACKER_FILES}")
    added = "\n".join(
        line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")
    )
    for marker in markers:
        constant = WIFI_TRACKER_MARKER_CONSTANTS.get(marker)
        if constant is None or constant not in added:
            problems.append(f"it does not read the key {marker!r} (by {constant}) that the verifier requires")
    for needle, what in (
        ("NET_CAPABILITY_VALIDATED", "require a validated network"),
        ("NET_CAPABILITY_CAPTIVE_PORTAL", "leave a network behind a sign-in page to the stock rules"),
        ("NET_CAPABILITY_PARTIAL_CONNECTIVITY", "leave a partial network to the stock rules"),
        ("NETWORK_VALIDATION_RESULT_VALID", "require a report that says valid, not skipped"),
        ("KEY_NETWORK_PROBES_ATTEMPTED_BITMASK", "read the probes the report says were attempted"),
        ("KEY_NETWORK_PROBES_SUCCEEDED_BITMASK", "read the probes the report says succeeded"),
        ("(attempted & NETWORK_PROBE_HTTPS) != 0 && (succeeded & NETWORK_PROBE_HTTPS) == 0",
         "ask for the HTTPS probe attempted and not succeeded"),
        ("|| isInternetCheckFailed() ||", "show the exclamation mark for a failed check"),
        ("isPartialConnectivity || isInternetCheckFailed", "write the limited connection summary for a failed check"),
        ("markChanged |= markShown != entry.shouldShowXLevelIcon();", "see whether a report moves the mark"),
        ("if (markChanged) {\n            notifyOnWifiEntriesChanged(WIFI_ENTRIES_CHANGED_REASON_GENERAL);",
         "tell the listeners of the tracker when a report moves the mark"),
    ):
        if needle not in added:
            problems.append(f"it does not {what}")
    return problems


def verify_wifi_tracker_topic(android_root: Path) -> None:
    """The routed WifiTrackerLib topic and the verifier agree on the warning for a failed check.

    The target-files fixtures prove what the verifier rejects; this proves that the topic puts in
    Settings and SystemUI the name the verifier looks for, and that the predicate holds for the
    report a failed check leaves and for no other."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    rows = [
        line.split("\t")
        for line in (android_root / "patches/projects.tsv").read_text().splitlines()
        if line and not line.startswith("#")
    ][1:]
    routes = [row for row in rows if row[3] == WIFI_TRACKER_PROJECT]
    if [(row[1], row[4]) for row in routes] != [("p0033", "frameworks/opt/net/wifi")]:
        fail("the route table does not route frameworks/opt/net/wifi once, as p0033")
    patches = sorted((android_root / "patches/projects/p0033/series").glob("*.patch"))
    if len(patches) != 1:
        fail(f"p0033 has {len(patches)} patches, not one")
    patch = patches[0].read_text()
    markers = [marker.decode() for marker in verifier.WIFI_TRACKER_MARKERS]
    if markers != ["networkProbesAttempted"]:
        fail("the verifier no longer looks for the key the WifiTrackerLib predicate reads")
    if tuple(verifier.WIFI_TRACKER_APKS) != (
        "SYSTEM_EXT/priv-app/Settings/Settings.apk",
        "SYSTEM_EXT/priv-app/SystemUI/SystemUI.apk",
    ):
        fail("the verifier no longer checks Settings and SystemUI for the WifiTrackerLib topic")
    problems = wifi_tracker_topic_problems(patch, markers)
    if problems:
        fail("the WifiTrackerLib topic: " + "; ".join(problems))
    for label, edited in (
        ("a topic that reads no key the verifier looks for",
         patch.replace("KEY_NETWORK_PROBES_ATTEMPTED_BITMASK", "KEY_NETWORK_PROBES_SUCCEEDED_BITMASK")),
        ("a topic that shows an unvalidated network",
         patch.replace("NET_CAPABILITY_VALIDATED", "NET_CAPABILITY_INTERNET")),
        ("a topic that shows a sign-in network",
         patch.replace("NET_CAPABILITY_CAPTIVE_PORTAL", "NET_CAPABILITY_INTERNET")),
        ("a topic that shows a partial network",
         patch.replace("NET_CAPABILITY_PARTIAL_CONNECTIVITY", "NET_CAPABILITY_INTERNET")),
        ("a topic that shows a skipped report",
         patch.replace("NETWORK_VALIDATION_RESULT_VALID", "NETWORK_VALIDATION_RESULT_SKIPPED")),
        ("a topic that ignores the attempted probes",
         patch.replace("(attempted & NETWORK_PROBE_HTTPS) != 0 &&", "")),
        ("a topic that shows a network whose HTTPS probe succeeded",
         patch.replace("(succeeded & NETWORK_PROBE_HTTPS) == 0", "(succeeded & NETWORK_PROBE_HTTPS) != 0")),
        ("a topic whose icon ignores the check", patch.replace("|| isInternetCheckFailed() ||", "||")),
        ("a topic whose summary ignores the check",
         patch.replace("isPartialConnectivity || isInternetCheckFailed", "isPartialConnectivity")),
        ("a topic whose tracker never tells its listeners",
         patch.replace("if (markChanged) {", "if (false) {")),
        ("a topic whose tracker does not see the mark move",
         patch.replace("markShown != entry.shouldShowXLevelIcon()", "false")),
        ("a topic that also changes another file",
         patch + "diff --git a/libs/WifiTrackerLib/src/com/android/wifitrackerlib/SavedNetworkTracker.java b/x\n"),
    ):
        if not wifi_tracker_topic_problems(edited, markers):
            fail(f"the WifiTrackerLib topic check accepted {label}")


def verify_media_codecs_overlay(android_root: Path) -> None:
    """The tracked software decoder update satisfies the verifier that checks the image."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    verifier.check_media_codecs_c2(
        (android_root / "overlay/config/media_codecs_c2.xml").read_bytes()
    )


def verify_package_assertions(android_root: Path) -> None:
    """The producer's package assertions agree with the product's removed-package list."""
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    removed_block = re.search(
        r"^KERNARIUM_REMOVED_PACKAGES := \\\n((?:    \S+ \\\n)+    \S+)$", product, re.MULTILINE
    )
    if removed_block is None:
        fail("lineage_andarium.mk has no KERNARIUM_REMOVED_PACKAGES list")
    removed = set(removed_block.group(1).replace("\\", " ").split())
    script = (android_root / "build/build-aosp.sh").read_text()
    excluded_block = re.search(
        r'^PRODUCT_PACKAGES_EXCLUDE=.*\nfor package in \\\n((?:\t\S+ \\\n)+\t\S+); do$', script, re.MULTILINE
    )
    if excluded_block is None:
        fail("build-aosp.sh has no list of packages asserted to be excluded")
    asserted_excluded = set(excluded_block.group(1).replace("\\", " ").split())
    for package in sorted(asserted_excluded - removed):
        fail(f"build-aosp.sh asserts {package} is excluded, but the product does not remove it")
    present = set(re.findall(r'\[\[ "\$PRODUCT_PACKAGES" == \*" (\S+) "\* \]\]', script))
    for package in sorted(present & removed):
        fail(f"build-aosp.sh asserts {package} is present, but the product removes it")
    for package in (
        "android.hardware.sensors-service.multihal",
        "android.hardware.sensors@2.1-impl.andarium",
        "android.hardware.thermal-service.andarium",
        "kernarium_gamepad",
        "com.android.hardware.cas",
        "libclearkeycasplugin",
        "com.android.hardware.wifi",
        "com.google.cf.wpa_supplicant",
        "KernariumWifiOverlay",
        "KernariumPinnerOverlay",
        "KernariumSms",
        "messaging",
        "TelephonyProvider",
        "SecureElement",
        "EasterEgg",
        "kernarium_net_setup",
        "kernarium_wifi_network_script",
        "kernarium_reclaim_boot_apps_script",
        "kernarium_time_zone_script",
        "kernarium_keyboard_default_script",
    ):
        if package not in present:
            fail(f"build-aosp.sh does not assert that {package} is present")


def verify_keyboard_default(android_root: Path) -> None:
    """A new guest hides Android's on-screen keyboard while a hardware keyboard is attached, and a guest
    that an older release created with it on is moved once.

    The script runs with stub tools: getprop and setprop read and write a table of properties, `cmd
    settings get` prints the stored setting (nothing while the service is not up) and `cmd settings put`
    stores a value or, for the refusal case, does not, and log and sleep record what the script says."""
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    tools = android_root / "overlay/tools"
    script = tools / "kernarium_keyboard_default.sh"
    defaults = ET.fromstring(
        (android_root / "overlay/overlay/frameworks/base/packages/SettingsProvider/"
         "res/values/defaults.xml").read_text()
    )
    values = [
        element.text for element in defaults.findall("bool")
        if element.get("name") == "def_show_ime_with_hard_keyboard"
    ]
    if values != ["false"]:
        fail(f"the SettingsProvider overlay must default the on-screen keyboard with a hardware keyboard off: {values}")
    if not os.access(script, os.X_OK) or not script.read_text().startswith("#!/system/bin/sh\n"):
        fail("kernarium_keyboard_default.sh must be an executable /system/bin/sh script, as its siblings are")
    # The sepolicy sources hold what the built-policy check looks for.
    policy = (android_root / "overlay/sepolicy/system_ext/private/kernarium_keyboard_default.te").read_text()
    for needle in (
        "init_daemon_domain(kernarium_keyboard_default)",
        "allow kernarium_keyboard_default settings_service:service_manager find;",
        "allow kernarium_keyboard_default system_server:binder transfer;",
        "allow system_server kernarium_keyboard_default:binder call;",
        "allow system_server kernarium_keyboard_default:fd use;",
        "allow system_server kernarium_keyboard_default:fifo_file { getattr read write };",
        "allow kernarium_keyboard_default system_file:file execute_no_trans;",
        "system_internal_prop(kernarium_keyboard_default_prop)",
        "set_prop(kernarium_keyboard_default, kernarium_keyboard_default_prop)",
    ):
        if needle not in policy:
            fail(f"the keyboard default domain lacks '{needle}'")
    if len(re.findall(r"^set_prop\(", policy, re.M)) != 1:
        fail("the keyboard default domain sets a property besides its marker")
    contexts = (android_root / "overlay/sepolicy/system_ext/private/file_contexts").read_text()
    if (
        r"/(system_ext|system/system_ext)/bin/kernarium_keyboard_default\.sh  "
        "u:object_r:kernarium_keyboard_default_exec:s0"
    ) not in contexts:
        fail("the system_ext file contexts do not label the keyboard default script")
    properties = (android_root / "overlay/sepolicy/system_ext/private/property_contexts").read_text()
    if (
        "persist.sys.kernarium.keyboard_default    u:object_r:kernarium_keyboard_default_prop:s0 exact bool"
    ) not in properties:
        fail("the system_ext property contexts do not label the keyboard default marker")
    blueprint = (tools / "Android.bp").read_text()
    for needle in (
        'name: "kernarium_keyboard_default_script"',
        'src: "kernarium_keyboard_default.sh"',
        'init_rc: ["kernarium_keyboard_default.rc"]',
    ):
        if needle not in blueprint:
            fail(f"overlay/tools/Android.bp lacks {needle}")
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    if len(re.findall(r"^PRODUCT_PACKAGES \+= kernarium_keyboard_default_script$", product, re.M)) != 1:
        fail("lineage_andarium.mk does not add the keyboard default script to PRODUCT_PACKAGES exactly once")
    verifier.check_keyboard_default((tools / "kernarium_keyboard_default.rc").read_text(), script.read_bytes())

    marker = "persist.sys.kernarium.keyboard_default"

    def run(stored: str | None, applied: str = "", *, refuse: bool = False, silent_reads: int = 0,
            runs: int = 1) -> tuple[subprocess.CompletedProcess, list[str], str | None, str]:
        """Runs the script `runs` times on one guest; returns the last process, every recorded call, the
        stored setting (None when there is none) and the marker."""
        with tempfile.TemporaryDirectory(prefix="keyboard-default-script-") as temporary:
            stubs = Path(temporary)
            calls = stubs / "calls"
            state = stubs / "state"
            state.mkdir()
            properties = state / "properties"
            properties.mkdir()
            (properties / marker).write_text(applied + "\n")
            if stored is not None:
                (state / "setting").write_text(stored + "\n")
            (state / "silent").write_text(f"{silent_reads}\n")
            for name, body in {
                "getprop": f'cat {properties}/"$1" 2>/dev/null || echo\n',
                "setprop": f'echo "setprop $*" >> {calls}\necho "$2" > {properties}/"$1"\n',
                "cmd": (
                    f'echo "cmd $*" >> {calls}\n'
                    'if [ "$1 $2 $3 $4" = "settings get secure show_ime_with_hard_keyboard" ]; then\n'
                    f'    silent=$(cat {state}/silent)\n'
                    '    if [ "$silent" -gt 0 ]; then\n'
                    f'        echo $((silent - 1)) > {state}/silent\n'
                    "        exit 1\n"
                    "    fi\n"
                    f'    if [ -f {state}/setting ]; then cat {state}/setting; else echo null; fi\n'
                    'elif [ "$1 $2 $3 $4 $5" = "settings put secure show_ime_with_hard_keyboard $5" ]; then\n'
                    + ("    :\n" if refuse else f'    echo "$5" > {state}/setting\n')
                    + "else\n    exit 2\nfi\n"
                ),
                "log": f'echo "log $*" >> {calls}\n',
                "sleep": f'echo "sleep $*" >> {calls}\n',
            }.items():
                (stubs / name).write_text("#!/bin/sh\n" + body)
                (stubs / name).chmod(0o755)
            env = {"PATH": f"{stubs}:/usr/bin:/bin"}
            result = None
            for _ in range(runs):
                result = subprocess.run(["/bin/sh", str(script)], env=env, capture_output=True, text=True)
            assert result is not None
            recorded = calls.read_text().splitlines() if calls.exists() else []
            return (
                result,
                recorded,
                (state / "setting").read_text().strip() if (state / "setting").exists() else None,
                (properties / marker).read_text().strip(),
            )

    def puts(recorded: list[str]) -> list[str]:
        return [line for line in recorded if line.startswith("cmd settings put")]

    def notes(recorded: list[str]) -> list[str]:
        return [line for line in recorded if line.startswith("log ")]

    # A guest that an older release created holds 1: it becomes 0, once, and the marker records it.
    result, recorded, final, marked = run("1")
    if (
        result.returncode != 0
        or final != "0"
        or marked != "1"
        or puts(recorded) != ["cmd settings put secure show_ime_with_hard_keyboard 0"]
        or len(notes(recorded)) != 1
    ):
        fail(f"the keyboard default script did not turn a stored 1 off: {recorded} {final} {marked}")
    # A guest that already has the default (0, or no row) is not written, and the marker is set.
    for stored in ("0", None):
        result, recorded, final, marked = run(stored)
        if result.returncode != 0 or puts(recorded) or final != stored or marked != "1" or len(notes(recorded)) != 1:
            fail(f"the keyboard default script touched a guest at the default ({stored!r}): "
                 f"{recorded} {final} {marked}")
    # It acts once: a person who turned the setting on afterwards keeps it, and a second run reads and
    # writes nothing.
    result, recorded, final, marked = run("1", "1")
    if result.returncode != 0 or recorded or final != "1" or marked != "1":
        fail(f"the keyboard default script acted again after its marker: {recorded} {final}")
    result, recorded, final, marked = run("1", runs=2)
    if result.returncode != 0 or len(puts(recorded)) != 1 or final != "0" or marked != "1":
        fail(f"the keyboard default script is not idempotent: {recorded} {final}")
    # It writes only the one setting, and never anything but 0.
    for line in recorded:
        if line.startswith("cmd settings put") and line != "cmd settings put secure show_ime_with_hard_keyboard 0":
            fail(f"the keyboard default script wrote something else: {line}")
    # A service that is not up yet is waited for: the script retries, then acts.
    result, recorded, final, marked = run("1", silent_reads=5)
    if (
        result.returncode != 0
        or final != "0"
        or marked != "1"
        or len([line for line in recorded if line.startswith("sleep")]) != 5
    ):
        fail(f"the keyboard default script did not wait for the settings service: {recorded} {final} {marked}")
    # A setting that cannot be read at all leaves everything alone, sets no marker and fails the
    # script (not the boot), so the next boot tries again.
    result, recorded, final, marked = run("1", silent_reads=1000)
    if result.returncode == 0 or puts(recorded) or final != "1" or marked != "":
        fail(f"the keyboard default script acted on an unreadable setting: {recorded} {final} {marked}")
    # A write the platform refuses leaves the 1 and no marker, and the next boot tries again.
    result, recorded, final, marked = run("1", refuse=True)
    if result.returncode == 0 or len(puts(recorded)) != 1 or final != "1" or marked != "":
        fail(f"the keyboard default script claimed a write the platform refused: {recorded} {final} {marked}")


def verify_debugging_defaults(android_root: Path) -> None:
    """Developer options and USB debugging are off on a fresh data partition."""
    defaults = ET.fromstring(
        (android_root / "overlay/overlay/frameworks/base/packages/SettingsProvider/"
         "res/values/defaults.xml").read_text()
    )
    values = [
        element.text for element in defaults.findall("bool")
        if element.get("name") == "def_development_settings_enabled"
    ]
    if values != ["false"]:
        fail(f"the SettingsProvider overlay must default Developer options off: {values}")
    product = (android_root / "overlay/lineage_andarium.mk").read_text()
    for line in product.splitlines():
        stripped = line.strip().rstrip("\\").strip()
        if stripped.startswith("persist.sys.usb.config=") and not stripped.endswith(",$(PRODUCT_PRODUCT_PROPERTIES))"):
            fail(f"lineage_andarium.mk must not set USB debugging on: {stripped}")
        if stripped.startswith("ro.adb.secure="):
            fail(f"lineage_andarium.mk must leave ro.adb.secure to the build type: {stripped}")
    # The inherited Cuttlefish user block adds ro.adb.secure=0 to the product partition. The product
    # makefile cannot filter an inherited entry and the board configuration cannot assign a product
    # property list (read-only there), so the Cuttlefish topic drops it at its source on request.
    if "UML_ANDROID_KEEP_ADB_SECURE := true" not in product.splitlines():
        fail("lineage_andarium.mk must set UML_ANDROID_KEEP_ADB_SECURE := true")
    board = (android_root / "overlay/BoardConfig.mk").read_text()
    for number, line in enumerate(board.splitlines(), 1):
        if re.match(r"\s*PRODUCT_[A-Z_]*PROPERTIES\s*(:|\+|\?)?=", line):
            fail(f"BoardConfig.mk:{number} assigns a product property list, which is read-only there: {line.strip()}")


def adb_secure_topic_problems(added: list[str]) -> list[str]:
    """What the Cuttlefish topic must add to shared/device.mk so UML_ANDROID_KEEP_ADB_SECURE has a reader."""
    problems = []
    if "ifeq ($(UML_ANDROID_KEEP_ADB_SECURE),true)" not in added:
        problems.append("the filter is not conditional on UML_ANDROID_KEEP_ADB_SECURE")
    if "PRODUCT_PRODUCT_PROPERTIES := $(filter-out ro.adb.secure=%,$(PRODUCT_PRODUCT_PROPERTIES))" not in added:
        problems.append("it does not filter ro.adb.secure out of PRODUCT_PRODUCT_PROPERTIES")
    if "endif" not in added:
        problems.append("the conditional is not closed")
    return problems


def lunch_topic_problems(patch: str) -> list[str]:
    """The build/make topic must take both roomservice.py calls out of lunch and add none."""
    lines = patch.splitlines()
    removed = [line for line in lines if line.startswith("-") and not line.startswith("---") and "roomservice.py" in line]
    added = [line for line in lines if line.startswith("+") and not line.startswith("+++") and "roomservice.py" in line]
    problems = []
    if len(removed) != 2:
        problems.append(f"it removes {len(removed)} roomservice.py calls, not the two of lunch")
    if added:
        problems.append("it adds a roomservice.py call")
    return problems


def verify_adb_secure_and_lunch_topics(android_root: Path) -> None:
    """The topics that make the product's ro.adb.secure and a failing lunch what the contracts say."""
    keep = sorted((android_root / "patches/projects/p0001/series").glob("cuttlefish-let-a-product-keep-ro.adb.secure-*.patch"))
    if len(keep) != 1:
        fail(f"p0001 has {len(keep)} patches that read UML_ANDROID_KEEP_ADB_SECURE, not one")
    text = keep[0].read_text()
    added = [line[1:] for line in text.splitlines() if line.startswith("+") and not line.startswith("+++")]
    problems = adb_secure_topic_problems(added)
    if problems:
        fail(f"{keep[0].name}: " + "; ".join(problems))
    for label, edited in (
        ("a filter that is always on", [line for line in added if "UML_ANDROID_KEEP_ADB_SECURE)" not in line]),
        ("a filter of another property", [line.replace("ro.adb.secure=%", "persist.adb.tcp.port=%") for line in added]),
        ("a conditional that is never closed", [line for line in added if line != "endif"]),
    ):
        if not adb_secure_topic_problems(edited):
            fail(f"the Cuttlefish topic check accepts {label}")
    lunch = sorted((android_root / "patches/projects/p0002/series").glob("build-never-fetch-device-trees-from-lunch.patch"))
    if len(lunch) != 1:
        fail(f"p0002 has {len(lunch)} lunch topics, not one")
    patch = lunch[0].read_text()
    problems = lunch_topic_problems(patch)
    if problems:
        fail(f"{lunch[0].name}: " + "; ".join(problems))
    lines = patch.splitlines(keepends=True)
    first = next((n for n, line in enumerate(lines) if line.startswith("-") and not line.startswith("---") and "roomservice.py" in line), None)
    if first is None:
        fail(f"{lunch[0].name} removes no roomservice.py call")
    for label, edited in (
        ("a patch that keeps a call", "".join(lines[:first] + lines[first + 1:])),
        ("a patch that adds a call", patch + "+    vendor/lineage/build/tools/roomservice.py $product\n"),
    ):
        if edited == patch or not lunch_topic_problems(edited):
            fail(f"the lunch topic check accepts {label}")


def verify_hal_selections(android_root: Path) -> None:
    # The boot command line is the one writer of each graphics HAL selection,
    # so it must name the modules the target-files verifier expects.
    verifier = load_tool(android_root / "tools/verify-target-files.py")
    command_line = (
        android_root / "support/boot/kernel-command-line.txt"
    ).read_text().split()
    for name, expected in verifier.BOOT_HAL_SELECTIONS.items():
        values = [
            token.partition("=")[2]
            for token in command_line
            if token.partition("=")[0] == f"androidboot.hardware.{name}"
        ]
        if values != [expected]:
            fail(f"boot command line selects {name} HAL {values}, not {expected}")
    metadata = (android_root / "third_party/mesa-venus/metadata.env").read_text()
    module = f"vulkan.{verifier.BOOT_HAL_SELECTIONS['vulkan']}"
    if f"MESA_VENUS_HAL_MODULE='{module}'" not in metadata.splitlines():
        fail(f"the Vulkan HAL selection does not name the Venus module {module}")

    # The command line is also the one writer of ro.opengles.version, the GLES
    # version the CTS qualifies the product's features and dEQP level against.
    versions = [
        token.partition("=")[2]
        for token in command_line
        if token.partition("=")[0] == "androidboot.opengles.version"
    ]
    if versions != [str(verifier.BOOT_OPENGLES_VERSION)]:
        fail(
            f"boot command line declares OpenGL ES {versions}, not "
            f"{verifier.BOOT_OPENGLES_VERSION}"
        )

    # ro.serialno comes from androidboot.serialno, and Build.getSerial() must match
    # ^[a-zA-Z0-9]+$ (CTS AccessSerialNumberTest#testGetSerialReturnsExpectedFormat).
    serials = [
        token.partition("=")[2]
        for token in command_line
        if token.partition("=")[0] == "androidboot.serialno"
    ]
    if len(serials) != 1 or not re.fullmatch(r"[a-zA-Z0-9]+", serials[0]):
        fail(f"boot command line serial number {serials} is not one alphanumeric value")

    # A setprop that expands a missing ro.boot property fails at every boot.
    # The product init script may therefore expand only properties the command
    # line sets, or a host-supplied one inside an action gated on its presence.
    provided = {token.partition("=")[0] for token in command_line}
    script = (android_root / "overlay/config/init.vendor.rc").read_text()
    for trigger, commands in verifier.init_actions(script).items():
        gated = set(re.findall(r"property:ro\.boot\.([\w.]+)=\*", trigger))
        for command in commands:
            for name in re.findall(r"\$\{ro\.boot\.([\w.]+)\}", command):
                if name not in gated and f"androidboot.{name}" not in provided:
                    fail(
                        f"init action '{trigger}' expands ro.boot.{name}, "
                        "which the boot command line does not set"
                    )

    # Every launcher sets the hardware timeout multiplier from the guest's vCPU
    # count, so the tracked command line must not, and the product init script
    # defaults the copy to 5 for a launch that passes none.
    if any(
        token.startswith("androidboot.hw_timeout_multiplier=") for token in command_line
    ):
        fail("boot command line sets the hardware timeout multiplier, which launchers set")
    if verifier.UML_INIT_TIMEOUT_COPY not in verifier.init_actions(script).get("early-init", []):
        fail("product init script does not default the hardware timeout multiplier to 5")

    # Boot runs with expedited RCU grace periods, and the product init script
    # restores normal ones once boot completes.
    if command_line.count("rcupdate.rcu_expedited=1") != 1:
        fail("boot command line does not expedite RCU grace periods once")
    actions = verifier.init_actions(script)
    if verifier.UML_INIT_RCU_RESTORE not in actions.get(verifier.UML_INIT_RCU_TRIGGER, []):
        fail("product init script does not restore normal RCU grace periods after boot")

    # Android uses no network block device, and the desktop kernels' built-in
    # driver otherwise creates 16 devices with a stack-holding receiver thread
    # each.
    if command_line.count("nbd.nbds_max=0") != 1:
        fail("boot command line does not turn the network block devices off once")

    # The primary audio output's ALSA period is one 5 ms HAL buffer (route p0005 and the
    # product's tinyalsa properties), which the Virtio sound driver admits only with its
    # minimum period lowered from 10 ms.
    if command_line.count("virtio_snd.pcm_period_ms_min=5") != 1:
        fail("boot command line does not lower the Virtio sound minimum period to 5 ms once")


def newc_entry(name: str, mode: int, payload: bytes, inode: int) -> bytes:
    fields = (
        inode,
        mode,
        0,
        0,
        1,
        0,
        len(payload),
        0,
        0,
        0,
        0,
        len(name.encode()) + 1,
        0,
    )
    result = bytearray(b"070701" + b"".join(f"{value:08x}".encode() for value in fields))
    result.extend(name.encode() + b"\0")
    result.extend(b"\0" * (-len(result) % 4))
    result.extend(payload)
    result.extend(b"\0" * (-len(result) % 4))
    return bytes(result)


def synthetic_initramfs(fstab: str, *, wrong_fstab: bool = False,
                        module_loader: bool = False,
                        duplicate_file: bool = False,
                        module_file: str | None = None) -> bytes:
    if wrong_fstab:
        fstab = ext4_system_fstab(fstab)
    entries = [
        newc_entry("dev", 0o40755, b"", 1),
        newc_entry("dev", 0o40755, b"", 2),
        newc_entry("init", 0o100750, b"init", 3),
        newc_entry(
            "first_stage_ramdisk/system/etc/fstab.uml",
            0o100644,
            fstab.encode(),
            4,
        ),
    ]
    if module_loader:
        entries.append(newc_entry("system/bin/modprobe", 0o120777, b"toybox", 5))
    if duplicate_file:
        entries.append(newc_entry("init", 0o100750, b"init", 6))
    if module_file is not None:
        entries.append(newc_entry(f"lib/modules/{module_file}", 0o100644, b"module", 7))
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    return b"".join(entries)


def verify_initramfs_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-initramfs-") as temporary:
        root = Path(temporary)
        good = root / "ramdisk.img"
        fstab = tracked_fstab(verifier.parents[1])
        good.write_bytes(synthetic_initramfs(fstab))
        run_tool(verifier, good, check=True)
        for label, options in (
            ("wrong-fstab", {"wrong_fstab": True}),
            ("module-loader", {"module_loader": True}),
            ("duplicate-file", {"duplicate_file": True}),
        ):
            fixture = root / f"bad-{label}.img"
            fixture.write_bytes(synthetic_initramfs(fstab, **options))
            result = run_tool(verifier, fixture)
            if result.returncode == 0:
                fail(f"initramfs verifier accepted the {label} fixture")
        # Kernel module files and metadata, in any case, name their cause.
        for module_file in (
            "fixture.ko", "fixture.ko.xz", "modules.dep", "MODULES.Dep",
            "modules.devname", "modules.symbols", "modules.blocklist",
        ):
            fixture = root / "bad-module.img"
            fixture.write_bytes(synthetic_initramfs(fstab, module_file=module_file))
            result = run_tool(verifier, fixture)
            if result.returncode == 0 or "loadable-module payload" not in result.stdout:
                fail(f"initramfs verifier did not reject {module_file} as a module: {result.stdout}")


def synthetic_boot_image(
    ramdisk: bytes = b"fixture-initramfs",
    *,
    kernel_size: int = 0,
    header_version: int = 4,
) -> bytes:
    header = bytearray(4096)
    header[:8] = b"ANDROID!"
    struct.pack_into("<IIII", header, 8, kernel_size, len(ramdisk), 0, 1584)
    struct.pack_into("<I", header, 40, header_version)
    struct.pack_into("<I", header, 1580, 0)
    return bytes(header) + ramdisk


def verify_boot_image_fixtures(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="android-boot-image-") as temporary:
        root = Path(temporary)
        ramdisk = root / "ramdisk.img"
        ramdisk.write_bytes(b"fixture-initramfs")
        good = root / "init_boot.img"
        good.write_bytes(synthetic_boot_image(ramdisk.read_bytes()))
        run_tool(verifier, "verify", "--image", good, "--ramdisk", ramdisk, check=True)
        selected_range = run_tool(
            verifier, "range", "--image", good,
            check=True, stdout=subprocess.PIPE, text=True,
        )
        parsed = json.loads(selected_range.stdout)
        if parsed["ramdisk_offset"] != 4096 or parsed["ramdisk_size"] != ramdisk.stat().st_size:
            fail("boot-image verifier reported the wrong ramdisk range")

        for label, payload in (
            ("embedded-kernel", synthetic_boot_image(kernel_size=1)),
            ("old-header", synthetic_boot_image(header_version=3)),
        ):
            fixture = root / f"bad-{label}.img"
            fixture.write_bytes(payload)
            result = run_tool(verifier, "verify", "--image", fixture)
            if result.returncode == 0:
                fail(f"boot-image verifier accepted {label}")


def lz4_literal_block(payload: bytes) -> bytes:
    """One LZ4 block that carries `payload` as a single literal run."""
    length = len(payload)
    token = min(length, 15) << 4
    extension = bytearray()
    if length >= 15:
        remaining = length - 15
        while remaining >= 255:
            extension.append(255)
            remaining -= 255
        extension.append(remaining)
    return bytes([token]) + bytes(extension) + payload


def lz4_legacy_frame(payload: bytes, block_size: int = 1 << 20) -> bytes:
    """An LZ4 legacy frame, the format of Android's compressed recovery ramdisk."""
    frame = bytearray(b"\x02\x21\x4c\x18")
    for offset in range(0, len(payload), block_size):
        block = lz4_literal_block(payload[offset:offset + block_size])
        frame += struct.pack("<I", len(block)) + block
    return bytes(frame)


def synthetic_selinux_policy(
    *, udp: tuple[str, ...] = ("create", "ioctl"), ioctls: tuple[int, ...] = (0x8916, 0x891C),
    capabilities: tuple[str, ...] = ("net_admin",), domain: str = "recovery", version: int = 30,
) -> bytes:
    """A kernel binary policy (version 30) with two types, a capability class and a UDP socket
    class that inherits the socket class's permissions, and the given rules of `domain` on itself."""

    def word(*values: int) -> bytes:
        return struct.pack(f"<{len(values)}I", *values)

    def named(text: str) -> bytes:
        return text.encode()

    empty_bitmap = word(64, 0, 0)
    one_bit_bitmap = word(64, 1, 1) + word(0) + struct.pack("<Q", 2)
    table_header = lambda count: word(count, count)  # noqa: E731 - primary names, elements
    common_permissions = {"ioctl": 1, "read": 2, "write": 3, "create": 4}
    udp_permissions = {"name_bind": 5}
    capability_permissions = {"chown": 1, "net_admin": 13}

    def permissions(table: dict[str, int]) -> bytes:
        return b"".join(word(len(name), value) + named(name) for name, value in table.items())

    # One constraint with an attribute comparison and a name list, so that the reader skips both.
    constraint = (
        word(1, 2) + word(4, 1, 1) + word(5, 2, 1) + one_bit_bitmap + empty_bitmap + empty_bitmap + word(0)
    )
    commons = table_header(1) + word(6, 1, 4, 4) + named("socket") + permissions(common_permissions)
    classes = table_header(2) + (
        word(10, 0, 1, 2, 2, 0) + named("capability") + permissions(capability_permissions)
        + word(0) + word(0, 0, 0, 0)
        + word(10, 6, 2, 5, 1, 1) + named("udp_socket") + named("socket")
        + permissions(udp_permissions) + constraint + word(0) + word(0, 0, 0, 0)
    )
    roles = table_header(1) + word(8, 1, 0) + named("object_r") + empty_bitmap + one_bit_bitmap
    types = table_header(2) + word(8, 1, 1, 0) + named("recovery") + word(5, 2, 1, 0) + named("other")
    users = (
        table_header(1) + word(8, 1, 0) + named("system_u") + one_bit_bitmap
        + word(1) + word(1) + empty_bitmap + word(1) + empty_bitmap
    )
    booleans = table_header(1) + word(1, 0, 6) + named("a_bool")
    sensitivities = table_header(1) + word(2, 0) + named("s0") + word(1) + empty_bitmap
    categories = table_header(1) + word(2, 1, 0) + named("c0")

    type_ids = {"recovery": 1, "other": 2}
    source = type_ids[domain]
    class_ids = {"capability": 1, "udp_socket": 2}

    def allow(klass: str, target: int, mask: int) -> bytes:
        return struct.pack("<4H", source, target, class_ids[klass], 0x0001) + word(mask)

    rules = [
        # The domain's own rules, and a decoy that grants another type's access to it.
        allow("capability", source, sum(1 << (capability_permissions[name] - 1) for name in capabilities)),
        allow("udp_socket", source, sum(1 << ({**common_permissions, **udp_permissions}[name] - 1) for name in udp)),
        struct.pack("<4H", 2, source, 2, 0x0001) + word(0xFF),
    ]
    by_driver: dict[int, int] = {}
    for command in ioctls:
        by_driver[command >> 8] = by_driver.get(command >> 8, 0) | 1 << (command & 0xFF)
    for driver, bits in sorted(by_driver.items()):
        rules.append(
            struct.pack("<4H", source, source, class_ids["udp_socket"], 0x0100)
            + struct.pack("<BB", 1, driver) + bits.to_bytes(32, "little")
        )
    return (
        word(0xF97CFF8C, 8) + b"SE Linux" + word(version, 1, 8, 0) + empty_bitmap + empty_bitmap
        + commons + classes + roles + types + users + booleans + sensitivities + categories
        + word(len(rules)) + b"".join(rules)
        # What follows the rules is never read.
        + word(0, 0)
    )


def synthetic_lineage_ramdisk_for(
    fstab: bytes, *, alignment: int = 16384, omit_dependency: bool = False,
    interface: bytes = b"eth0\0", compression: str = "lz4",
    module: str | None = None, no_memfd: bool = False,
    elf_edit: tuple[str, int, int] | None = None,
    pixel_format: bytes = b"BGRX_8888",
    art: dict[str, bytes] | None = None,
    script: bytes = b"", imported: bytes | None = b"service console /system/bin/sh\n",
    script_name: str = "init.recovery.cutf_cvm.rc", policy: bytes | None = None,
    no_policy: bool = False,
) -> bytes:
    entries = [
        newc_entry(
            "init", 0o100750, synthetic_aarch64_elf(alignment, edit=elf_edit), 1
        ),
        newc_entry(
            "prop.default",
            0o100644,
            b"ro.product.system.device=andarium\n"
            b"persist.adb.tcp.port=5555\n"
            + (b"" if no_memfd else b"sys.use_memfd=true\n")
            + b"ro.minui.pixel_format=" + pixel_format + b"\n",
            2,
        ),
        newc_entry(
            "system/bin/adbd", 0o100755,
            synthetic_aarch64_elf(alignment, needed=("libfixture.so",)), 3,
        ),
        *([] if no_policy else [
            newc_entry("sepolicy", 0o100644, policy if policy is not None else synthetic_selinux_policy(), 16)
        ]),
        newc_entry("system/bin/e2fsck", 0o100755, b"tool", 4),
        newc_entry("system/bin/mke2fs", 0o100755, b"tool", 5),
        newc_entry(
            "system/bin/recovery", 0o100755,
            synthetic_aarch64_elf(alignment, needed=("librecovery_ui.so",)), 6,
        ),
        newc_entry("system/bin/resize2fs", 0o100755, b"tool", 7),
        newc_entry("system/bin/tune2fs", 0o100755, b"tool", 8),
        newc_entry("system/etc/recovery.fstab", 0o100644, fstab, 9),
        newc_entry(
            "system/lib64/librecovery_ui.so", 0o100644,
            synthetic_aarch64_elf(alignment), 10,
        ),
        newc_entry(
            "system/lib64/librecovery_ui_ext.so", 0o100644,
            synthetic_aarch64_elf(alignment)
            + interface + b"10.0.2.15\0" + b"255.255.255.0\0", 11,
        ),
        *([] if omit_dependency else [
            newc_entry(
                "system/lib64/libfixture.so", 0o100644,
                synthetic_aarch64_elf(alignment), 12,
            )
        ]),
        *([newc_entry(f"lib/modules/{module}", 0o100644, b"module", 13)] if module else []),
        *[
            newc_entry(f"res/images/{name}", 0o100644, payload, 100 + index)
            for index, (name, payload) in enumerate(sorted((art or {}).items()))
        ],
        # The product's recovery script, and the Cuttlefish one it imports.
        *([newc_entry(script_name, 0o100644, script, 14)] if script_name else []),
        *([newc_entry("init.recovery.cuttlefish.rc", 0o100644, imported, 15)]
          if imported is not None else []),
    ]
    entries.append(newc_entry("TRAILER!!!", 0, b"", len(entries) + 1))
    archive = b"".join(entries)
    if compression == "gzip":
        return gzip.compress(archive, mtime=0)
    if compression == "lz4":
        return lz4_legacy_frame(archive)
    return archive


def verify_lineage_recovery_fixtures(verifier: Path, android_root: Path) -> None:
    fstab = (android_root / "overlay/config/recovery.fstab").read_bytes()
    script = (android_root / "overlay/config/init.recovery.rc").read_bytes()
    module = load_tool(verifier)
    # The product's recovery artwork, which the ramdisk of a good image carries.
    art = {
        path.name: path.read_bytes()
        for path in sorted((android_root / module.ART_DIRECTORY).iterdir())
    }
    if len(art) != 33 or not set(module.ART_REQUIRED) <= art.keys():
        fail(f"the product's recovery artwork is {len(art)} files, not the 33 of the design")

    def ramdisk_for(fstab_bytes: bytes, **options: object) -> bytes:
        return synthetic_lineage_ramdisk_for(
            fstab_bytes, **{"script": script, "art": art, **options}
        )

    # A match whose offset is shorter than its length repeats the pattern.
    if module.lz4_block(b"\x35abc\x03\x00\x20de", 1 << 20) != b"abcabcabcabcde":
        fail("LZ4 block decoder mis-decoded an overlapping match")
    for label, block in (
        ("offset past the output", b"\x15a\x02\x00\x10b"),
        ("zero offset", b"\x15a\x00\x00\x10b"),
        ("literal run past the block", b"\x50ab"),
        ("truncated length", b"\xf0"),
    ):
        try:
            module.lz4_block(block, 1 << 20)
        except module.VerificationError:
            continue
        fail(f"LZ4 block decoder accepted {label}")

    def run(image: Path, root: Path = android_root) -> subprocess.CompletedProcess[str]:
        return run_tool(
            verifier, "--android-root", root, "--image", image
        )

    with tempfile.TemporaryDirectory(prefix="android-lineage-recovery-") as temporary:
        root = Path(temporary)
        for compression in ("lz4", "gzip"):
            good = root / f"recovery-{compression}.img"
            good.write_bytes(
                synthetic_boot_image(
                    ramdisk_for(fstab, compression=compression)
                )
            )
            result = run(good)
            if result.returncode != 0:
                fail(f"Lineage recovery verifier rejected a {compression} fixture: {result.stdout}")
        for label, ramdisk in (
            ("a 4 KiB-only ELF", ramdisk_for(fstab, alignment=4096)),
            (
                "a missing ELF dependency",
                ramdisk_for(fstab, omit_dependency=True),
            ),
            (
                "the Cuttlefish interface",
                ramdisk_for(fstab, interface=b"eth1\0"),
            ),
            (
                "a loadable module",
                ramdisk_for(fstab, module="fixture.ko"),
            ),
            (
                "properties without sys.use_memfd",
                ramdisk_for(fstab, no_memfd=True),
            ),
            (
                "the Cuttlefish ABGR_8888 pixel format",
                ramdisk_for(fstab, pixel_format=b"ABGR_8888"),
            ),
            (
                "a changed fstab",
                ramdisk_for(fstab + b"# changed\n"),
            ),
            (
                "an uncompressed ramdisk",
                ramdisk_for(fstab, compression="none"),
            ),
            (
                "a ramdisk without the product's artwork",
                ramdisk_for(fstab, art={}),
            ),
            (
                "a ramdisk without the header image",
                ramdisk_for(fstab, art={k: v for k, v in art.items() if k != "logo_image_switch.png"}),
            ),
            (
                "LineageOS's header image",
                ramdisk_for(fstab, art={**art, "logo_image_switch.png": b"the wire logo"}),
            ),
            (
                "a missing progress frame",
                ramdisk_for(fstab, art={k: v for k, v in art.items() if k != "loop00017.png"}),
            ),
            (
                "a stock progress frame the product does not supply",
                ramdisk_for(fstab, art={**art, "loop00030.png": b"stock"}),
            ),
        ):
            bad = root / "recovery-bad.img"
            bad.write_bytes(synthetic_boot_image(ramdisk))
            if run(bad).returncode == 0:
                fail(f"Lineage recovery verifier accepted {label}")
        # Rejections that must name their cause: kernel module files and
        # metadata in any case, and an ELF whose header is not an AArch64
        # little-endian ELF64 with whole program-header entries.
        for label, options, reason in (
            ("a module object suffix", {"module": "fixture.ko.xz"}, "loadable-module payload"),
            ("module metadata", {"module": "modules.dep"}, "loadable-module payload"),
            ("module metadata in capitals", {"module": "MODULES.Dep"}, "loadable-module payload"),
            ("a module blocklist", {"module": "modules.blocklist"}, "loadable-module payload"),
            ("a module device-name index", {"module": "modules.devname"}, "loadable-module payload"),
            ("a module symbol index", {"module": "modules.symbols.bin"}, "loadable-module payload"),
            (
                "a built-in alias index",
                {"module": "modules.builtin.alias.bin"},
                "loadable-module payload",
            ),
            ("a missing logo", {"art": {k: v for k, v in art.items() if k != "logo_image.png"}},
             "lacks the product's artwork res/images/logo_image.png"),
            ("a stock fastbootd image", {"art": {**art, "fastbootd.png": b"stock"}},
             "res/images/fastbootd.png is not the product's artwork"),
            ("a surplus frame", {"art": {**art, "loop00031.png": b"stock"}},
             "animation frames the product does not supply: .*loop00031.png"),
            ("a ramdisk without a policy", {"no_policy": True},
             "missing required paths.*sepolicy"),
            ("a policy that is not a policy", {"policy": b"not a policy at all"},
             "not a readable kernel policy: not a kernel binary SELinux policy"),
            ("a policy of an unsupported version", {"policy": synthetic_selinux_policy(version=29)},
             "not a readable kernel policy: policy version 29"),
            ("a truncated policy", {"policy": synthetic_selinux_policy()[:300]},
             "not a readable kernel policy: the policy ends inside a record"),
            ("a recovery domain without the UDP socket", {"policy": synthetic_selinux_policy(udp=())},
             r"does not let the recovery domain \['create', 'ioctl'\] on udp_socket"),
            ("a recovery domain that cannot create the UDP socket",
             {"policy": synthetic_selinux_policy(udp=("ioctl",))},
             r"does not let the recovery domain \['create'\] on udp_socket"),
            ("a recovery domain that cannot issue ioctls on the UDP socket",
             {"policy": synthetic_selinux_policy(udp=("create",))},
             r"does not let the recovery domain \['ioctl'\] on udp_socket"),
            ("a UDP socket with no extended ioctl rule", {"policy": synthetic_selinux_policy(ioctls=())},
             r"issue the ioctls \[\] on a UDP socket, not exactly \['0x8916', '0x891c'\]"),
            ("a UDP socket that lacks the netmask ioctl",
             {"policy": synthetic_selinux_policy(ioctls=(0x8916,))},
             r"issue the ioctls \['0x8916'\] on a UDP socket"),
            ("a UDP socket that allows another ioctl",
             {"policy": synthetic_selinux_policy(ioctls=(0x8916, 0x891C, 0x8927))},
             r"issue the ioctls \['0x8916', '0x891c', '0x8927'\] on a UDP socket"),
            ("a recovery domain without CAP_NET_ADMIN", {"policy": synthetic_selinux_policy(capabilities=())},
             r"does not let the recovery domain \['net_admin'\] on capability"),
            ("a policy that grants another domain the socket",
             {"policy": synthetic_selinux_policy(domain="other")},
             r"does not let the recovery domain \['create', 'ioctl'\] on udp_socket"),
            ("a 32-bit ELF", {"elf_edit": ("<B", 4, 1)}, "little-endian ELF64"),
            ("a big-endian ELF", {"elf_edit": ("<B", 5, 2)}, "little-endian ELF64"),
            ("a foreign machine", {"elf_edit": ("<H", 18, 62)}, "not AArch64"),
            ("a short program-header entry", {"elf_edit": ("<H", 54, 32)}, "program.header"),
        ):
            bad = root / "recovery-bad.img"
            bad.write_bytes(
                synthetic_boot_image(ramdisk_for(fstab, **options))
            )
            result = run(bad)
            if result.returncode == 0 or not re.search(reason, result.stdout):
                fail(f"Lineage recovery verifier did not reject {label} for {reason!r}: {result.stdout}")
        # The recovery script that publishes the display density: Cuttlefish's
        # script back under the product's name, the product's without the
        # density, and either script absent.
        no_density = b"".join(
            line for line in script.splitlines(keepends=True) if b"lcd_density" not in line
        )
        if no_density == script:
            fail("the recovery script fixture did not remove the density publication")
        changed = "differs from the checked device tree"
        for label, options, reason in (
            (
                "Cuttlefish's recovery script", {"script": b"service console /system/bin/sh\n"},
                changed,
            ),
            ("a recovery script without the density", {"script": no_density}, changed),
            ("a changed recovery script", {"script": script + b"# changed\n"}, changed),
            ("no recovery script", {"script_name": ""}, "missing required paths.*cutf_cvm"),
            ("no imported Cuttlefish script", {"imported": None}, "missing required paths.*cuttlefish"),
        ):
            bad = root / "recovery-bad.img"
            bad.write_bytes(synthetic_boot_image(ramdisk_for(fstab, **options)))
            result = run(bad)
            if result.returncode == 0 or not re.search(reason, result.stdout):
                fail(f"Lineage recovery verifier did not reject {label} for {reason!r}: {result.stdout}")
        # The checked script itself must import Cuttlefish's and publish the
        # density, whatever a ramdisk holds.
        tree = root / "tree"
        (tree / "overlay/config").mkdir(parents=True)
        (tree / "overlay/config/recovery.fstab").write_bytes(fstab)
        for label, checked, reason in (
            ("a checked script without the density", no_density, "does not run 'setprop ro.sf.lcd_density"),
            (
                "a checked script with the density under another trigger",
                script.replace(b"on early-init &&", b"on init &&"),
                "does not run 'setprop ro.sf.lcd_density .* at 'on early-init",
            ),
            (
                "a checked script without the import",
                script.replace(b"import /init.recovery.cuttlefish.rc", b"# no import"),
                "does not import init.recovery.cuttlefish.rc",
            ),
        ):
            if checked == script:
                fail(f"the recovery script fixture for {label} did not change the script")
            (tree / "overlay/config/init.recovery.rc").write_bytes(checked)
            bad = root / "recovery-bad.img"
            bad.write_bytes(synthetic_boot_image(synthetic_lineage_ramdisk_for(fstab, script=checked)))
            result = run(bad, tree)
            if result.returncode == 0 or not re.search(reason, result.stdout):
                fail(f"Lineage recovery verifier did not reject {label} for {reason!r}: {result.stdout}")


def synthetic_sparse_image() -> bytes:
    block_size = 4096
    raw = b"A" * block_size
    return b"".join(
        (
            struct.pack(
                "<IHHHHIIII",
                0xED26FF3A,
                1,
                0,
                28,
                12,
                block_size,
                3,
                3,
                0,
            ),
            struct.pack("<HHII", 0xCAC1, 0, 1, 12 + block_size),
            raw,
            struct.pack("<HHII", 0xCAC2, 0, 1, 16),
            b"\x12\x34\x56\x78",
            struct.pack("<HHII", 0xCAC3, 0, 1, 12),
        )
    )


SPARSE_BLOCK = 4096


def sparse_encode(raw: bytes, *, crc_chunk: bool = False) -> bytes:
    """Encode a block-multiple image the way img2simg does.

    Runs of zero blocks become DONT_CARE, runs of one repeated four-byte
    pattern become FILL, and the rest is RAW. `crc_chunk` appends the trailing
    CRC32 chunk some writers emit.
    """
    if not raw or len(raw) % SPARSE_BLOCK:
        fail("a sparse fixture must be a whole number of blocks")
    # Each chunk is (kind, the fill pattern or the RAW blocks, block count).
    chunks: list[tuple[str, bytes | list[bytes], int]] = []
    for offset in range(0, len(raw), SPARSE_BLOCK):
        block = raw[offset:offset + SPARSE_BLOCK]
        if block == bytes(SPARSE_BLOCK):
            kind, key = "dont_care", b""
        elif block == block[:4] * (SPARSE_BLOCK // 4):
            kind, key = "fill", block[:4]
        else:
            kind, key = "raw", b""
        if chunks and chunks[-1][0] == kind and (kind != "fill" or chunks[-1][1] == key):
            last = chunks[-1]
            if kind == "raw":
                last[1].append(block)
            chunks[-1] = (kind, last[1], last[2] + 1)
        else:
            chunks.append((kind, [block] if kind == "raw" else key, 1))
    body = bytearray()
    for kind, payload, blocks in chunks:
        if kind == "raw":
            data = b"".join(payload)
            body += struct.pack("<HHII", 0xCAC1, 0, blocks, 12 + len(data)) + data
        elif kind == "fill":
            body += struct.pack("<HHII", 0xCAC2, 0, blocks, 16) + payload
        else:
            body += struct.pack("<HHII", 0xCAC3, 0, blocks, 12)
    count = len(chunks)
    if crc_chunk:
        body += struct.pack("<HHII", 0xCAC4, 0, 0, 16) + struct.pack("<I", 0x12345678)
        count += 1
    header = struct.pack(
        "<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, SPARSE_BLOCK,
        len(raw) // SPARSE_BLOCK, count, 0,
    )
    return header + bytes(body)


def sparse_fixture_image(name: str, blocks: int) -> bytes:
    """A block-multiple filesystem image with zero, fill, and data runs."""
    seed = sum(name.encode())
    out = bytearray()
    for index in range(blocks):
        if index % 5 in (1, 2):
            out += bytes(SPARSE_BLOCK)
        elif index % 5 == 3:
            out += bytes(((seed + index) & 0xFF, 0x5A, 0xA5, 0x01)) * (SPARSE_BLOCK // 4)
        else:
            out += bytes((seed * 7 + index * 13 + i * 31) & 0xFF for i in range(SPARSE_BLOCK))
    return bytes(out)


def verify_ubd_sparse_inputs(
    tool: Path, production_policy: Path, entries: list[dict[str, object]], root: Path
) -> None:
    """The signed target files carry the five filesystems as Android sparse
    images, so the disk the assembler writes holds the expanded bytes, and is
    the disk the raw images would have produced."""
    filesystems = ("system", "system_ext", "product", "vendor", "odm")
    blocks = {"system": 9, "system_ext": 6, "product": 7, "vendor": 5, "odm": 1}
    raw_images = root / "sparse-raw-images"
    sparse_images = root / "sparse-images"
    raw_images.mkdir()
    sparse_images.mkdir()
    for entry in entries:
        name = entry["name"]
        if name in filesystems:
            raw = sparse_fixture_image(name, blocks[name])
            encoded = sparse_encode(raw, crc_chunk=name == "product")
            if len(encoded) >= len(raw) and name != "odm":
                fail("the sparse fixture is not smaller than its expansion")
        else:
            # The boot images stay raw in the target files and are not block
            # multiples.
            raw = encoded = bytes((i * 3 + len(name)) & 0xFF for i in range(5000))
        (raw_images / entry["image"]).write_bytes(raw)
        (sparse_images / entry["image"]).write_bytes(encoded)

    outputs = {}
    for label, images in (("raw", raw_images), ("sparse", sparse_images)):
        out = root / f"sparse-{label}-out"
        run_tool(
            tool, "assemble", "--policy", production_policy,
            "--images-dir", images, "--output-dir", out, check=True,
        )
        run_tool(
            tool, "verify", "--layout", out / "partition-layout.json",
            "--disk", out / "system.img", "--hash",
            "--policy", production_policy, check=True,
        )
        outputs[label] = out
    if (outputs["raw"] / "system.img").read_bytes() != (
        outputs["sparse"] / "system.img"
    ).read_bytes():
        fail("the disk assembled from sparse images differs from the raw one")
    if (outputs["raw"] / "partition-layout.json").read_bytes() != (
        outputs["sparse"] / "partition-layout.json"
    ).read_bytes():
        fail("the layout assembled from sparse images differs from the raw one")
    layout = json.loads((outputs["sparse"] / "partition-layout.json").read_text())
    by_name = {item["name"]: item for item in layout["system_disk"]["partitions"]}
    for name in filesystems:
        if by_name[name]["image_bytes"] != blocks[name] * SPARSE_BLOCK or \
                by_name[name]["size_bytes"] != blocks[name] * SPARSE_BLOCK:
            fail(f"partition {name} does not hold its expanded image exactly")
    if by_name["init_boot"]["image_bytes"] != 5000:
        fail("a raw boot image changed size next to the sparse filesystems")
    with (outputs["sparse"] / "system.img").open("rb") as disk:
        for name in filesystems:
            disk.seek(by_name[name]["start_bytes"])
            if disk.read(by_name[name]["image_bytes"]) != (raw_images / f"{name}.img").read_bytes():
                fail(f"partition {name} is not the expanded image")
    # The override path takes sparse images too.
    override_out = root / "sparse-override-out"
    run_tool(
        tool, "assemble", "--policy", production_policy,
        "--images-dir", raw_images, "--output-dir", override_out,
        "--image", f"vendor={sparse_images / 'vendor.img'}", check=True,
    )
    if (override_out / "system.img").read_bytes() != (
        outputs["raw"] / "system.img"
    ).read_bytes():
        fail("a sparse override differs from the raw image")

    # A hand-written image with one RAW, one FILL, and one DONT_CARE chunk.
    hand_written = root / "hand-written-sparse.img"
    hand_written.write_bytes(synthetic_sparse_image())
    hand_out = root / "sparse-hand-written-out"
    run_tool(
        tool, "assemble", "--policy", production_policy,
        "--images-dir", raw_images, "--output-dir", hand_out,
        "--image", f"vendor={hand_written}", check=True,
    )
    hand_layout = json.loads((hand_out / "partition-layout.json").read_text())
    hand_vendor = next(
        item for item in hand_layout["system_disk"]["partitions"] if item["name"] == "vendor"
    )
    if hand_vendor["image_bytes"] != 3 * SPARSE_BLOCK:
        fail("the hand-written sparse image did not expand to three blocks")
    with (hand_out / "system.img").open("rb") as disk:
        disk.seek(hand_vendor["start_bytes"])
        if disk.read(3 * SPARSE_BLOCK) != (
            b"A" * SPARSE_BLOCK + b"\x12\x34\x56\x78" * (SPARSE_BLOCK // 4) + bytes(SPARSE_BLOCK)
        ):
            fail("the hand-written sparse image expanded to the wrong bytes")

    good = (sparse_images / "vendor.img").read_bytes()
    header_size = struct.calcsize("<IHHHHIIII")
    short_blocks = bytearray(good)
    struct.pack_into("<I", short_blocks, 16, blocks["vendor"] + 1)
    chunk_count = bytearray(good)
    struct.pack_into("<I", chunk_count, 20, struct.unpack_from("<I", good, 20)[0] - 1)
    unknown_chunk = bytearray(good)
    struct.pack_into("<H", unknown_chunk, header_size, 0xCAC9)
    huge = bytearray(good)
    struct.pack_into("<I", huge, 16, 1 << 30)
    for label, damaged in (
        ("truncated", good[:-100]),
        ("trailing data", good + b"x"),
        ("more blocks than chunks", bytes(short_blocks)),
        ("fewer chunks than declared", bytes(chunk_count)),
        ("unknown chunk type", bytes(unknown_chunk)),
        ("absurd expanded size", bytes(huge)),
    ):
        bad_images = root / f"sparse-bad-{label.replace(' ', '-')}"
        shutil.copytree(sparse_images, bad_images)
        (bad_images / "vendor.img").write_bytes(damaged)
        bad_out = root / f"sparse-bad-out-{label.replace(' ', '-')}"
        rejected = run_tool(
            tool, "assemble", "--policy", production_policy,
            "--images-dir", bad_images, "--output-dir", bad_out,
        )
        if rejected.returncode == 0:
            fail(f"the assembler accepted a sparse image with {label}")
        if bad_out.exists() and any(bad_out.iterdir()):
            fail(f"the assembler left files behind after a sparse image with {label}")


def verify_ubd_disk_fixtures(tool: Path, production_policy: Path) -> None:
    """The system disk is exact fit and its layout file proves every byte."""
    run_tool(tool, "validate", "--policy", production_policy, check=True)
    policy = json.loads(production_policy.read_text())
    system = policy["system_disk"]
    entries = system["boot_partitions"] + system["filesystem_partitions"]

    with tempfile.TemporaryDirectory(prefix="android-ubd-disk-") as temporary:
        root = Path(temporary)
        images = root / "images"
        images.mkdir()
        sizes = {
            "init_boot": 5000,
            "recovery": 4096,
            "system": 3 * 4096 + 1,
            "system_ext": 4096 * 5,
            "product": 4096 * 2 + 17,
            "vendor": 4096 * 3,
            "odm": 4097,
        }
        for entry in entries:
            payload = bytes((sum(entry["name"].encode()) + i) & 0xFF
                            for i in range(251)) * (sizes[entry["name"]] // 251 + 1)
            (images / entry["image"]).write_bytes(payload[: sizes[entry["name"]]])
        out = root / "out"
        run_tool(
            tool, "assemble", "--policy", production_policy,
            "--images-dir", images, "--output-dir", out, check=True,
        )
        disk = out / "system.img"
        layout_path = out / "partition-layout.json"
        run_tool(
            tool, "verify", "--layout", layout_path, "--disk", disk, "--hash",
            "--policy", production_policy, check=True,
        )
        layout = json.loads(layout_path.read_text())
        listed = layout["system_disk"]["partitions"]
        names = [item["name"] for item in listed]
        if names[:2] != ["init_boot", "recovery"] or sorted(names[2:]) != sorted(
            entry["name"] for entry in system["filesystem_partitions"]
        ):
            fail(f"system disk partition order is wrong: {names}")
        filesystem_sizes = [item["size_bytes"] for item in listed[2:]]
        if filesystem_sizes != sorted(filesystem_sizes, reverse=True):
            fail("the read-only filesystems are not packed largest first")
        end = 0
        for item in listed:
            if item["start_bytes"] % 4096 or item["size_bytes"] % 4096:
                fail(f"partition {item['name']} is not 4 KiB aligned")
            if item["size_bytes"] - item["image_bytes"] >= 4096:
                fail(f"partition {item['name']} is not exact fit")
            if item["image_bytes"] != sizes[item["name"]]:
                fail(f"partition {item['name']} records the wrong image size")
            if end and item["start_bytes"] != end:
                fail(f"partition {item['name']} is not packed against its predecessor")
            end = item["start_bytes"] + item["size_bytes"]
        if listed[0]["start_bytes"] != 40 * 512:
            fail("the first partition does not follow the 20 KiB primary table")
        expected_size = (end + 128 * 128 + 512 + 4095) // 4096 * 4096
        if layout["system_disk"]["size_bytes"] != expected_size:
            fail("the system disk is not exact fit")
        if disk.stat().st_size != expected_size:
            fail("assembled system disk has the wrong size")
        with disk.open("rb") as image:
            for item in listed:
                image.seek(item["start_bytes"])
                if image.read(item["image_bytes"]) != (
                    images / next(e["image"] for e in entries if e["name"] == item["name"])
                ).read_bytes():
                    fail(f"partition {item['name']} payload is not at its offset")
        if layout["data_disk"] != policy["data_disk"]:
            fail("the layout does not carry the data disk template")

        # The data disk is the reference the host's own builder is held to.
        data_disk = root / "data.img"
        run_tool(
            tool, "data", "--layout", layout_path, "--capacity-gib", "4",
            "--output", data_disk, check=True,
        )
        run_tool(tool, "verify-data", "--layout", layout_path, "--disk", data_disk, check=True)
        if data_disk.stat().st_size != 4 << 30:
            fail("the data disk has the wrong size")
        with data_disk.open("rb") as image:
            image.seek(1 << 20)
            if image.read(2048) != b"\0" * 2048:
                fail("misc is not zero")
            image.seek(64 << 20)
            if image.read(4096) != b"\0" * 4096:
                fail("userdata is not zero")
        if data_disk.stat().st_blocks * 512 > 1 << 20:
            fail("the data disk is not sparse")
        for capacity in ("3", "129"):
            if run_tool(
                tool, "data", "--layout", layout_path, "--capacity-gib", capacity,
                "--output", root / f"data-{capacity}.img",
            ).returncode == 0:
                fail("the data disk accepted a capacity outside the template's range")
        if run_tool(
            tool, "data", "--layout", layout_path, "--output", data_disk
        ).returncode == 0:
            fail("the data disk tool replaced an existing disk")
        with data_disk.open("r+b") as image:
            image.seek(512 + 20)
            image.write(b"\xff")
        if run_tool(
            tool, "verify-data", "--layout", layout_path, "--disk", data_disk
        ).returncode == 0:
            fail("the data disk verifier accepted a corrupted table")

        existing = run_tool(
            tool, "assemble", "--policy", production_policy,
            "--images-dir", images, "--output-dir", out,
        )
        if existing.returncode == 0:
            fail("the system disk assembler replaced an existing release")

        missing = run_tool(
            tool, "assemble", "--policy", production_policy,
            "--images-dir", images / "nonexistent", "--output-dir", root / "missing",
        )
        if missing.returncode == 0:
            fail("the assembler accepted a missing images directory")

        verify_ubd_sparse_inputs(tool, production_policy, entries, root)

        for label, edit in (
            ("primary GPT", lambda f: (f.seek(512 + 20), f.write(b"\xff"))),
            ("backup GPT", lambda f: (f.seek(disk.stat().st_size - 512 + 20), f.write(b"\xff"))),
            ("partition payload", lambda f: (f.seek(listed[2]["start_bytes"] + 7), f.write(b"\xff"))),
        ):
            corrupted = root / f"corrupted-{label.replace(' ', '-')}.img"
            shutil.copyfile(disk, corrupted)
            with corrupted.open("r+b") as image:
                edit(image)
            rejected = run_tool(
                tool, "verify", "--layout", layout_path, "--disk", corrupted, "--hash"
            )
            if rejected.returncode == 0:
                fail(f"the verifier accepted a corrupted {label}")

        # The slack beyond the last partition must be zero.
        padded = root / "padded.img"
        shutil.copyfile(disk, padded)
        last = listed[-1]
        with padded.open("r+b") as image:
            image.seek(last["start_bytes"] + last["size_bytes"] + 1)
            image.write(b"\x01")
        if run_tool(
            tool, "verify", "--layout", layout_path, "--disk", padded, "--hash"
        ).returncode == 0:
            fail("the verifier accepted nonzero slack")

        # A layout that is not what the tracked policy produces is refused.
        tampered = json.loads(layout_path.read_text())
        tampered["data_disk"]["capacity_gib"]["maximum"] += 1
        tampered_path = root / "tampered-layout.json"
        tampered_path.write_text(json.dumps(tampered))
        if run_tool(
            tool, "verify", "--layout", tampered_path, "--disk", disk,
            "--policy", production_policy,
        ).returncode == 0:
            fail("the verifier accepted a data disk template the policy does not produce")

        # An unpacked layout (a partition that leaves room) is not exact fit.
        loose = json.loads(layout_path.read_text())
        loose["system_disk"]["partitions"][1]["size_bytes"] += 4096
        loose_path = root / "loose-layout.json"
        loose_path.write_text(json.dumps(loose))
        if run_tool(tool, "verify", "--layout", loose_path, "--disk", disk).returncode == 0:
            fail("the verifier accepted a layout that is not exact fit")

        # A data disk partition may not share a name with a system partition,
        # because first-stage init takes a name from any attached disk.
        clash = json.loads(production_policy.read_text())
        clash["system_disk"]["filesystem_partitions"][4]["name"] = "misc"
        clash_path = root / "clash-policy.json"
        clash_path.write_text(json.dumps(clash))
        if run_tool(tool, "validate", "--policy", clash_path).returncode == 0:
            fail("the policy accepted a data partition that repeats a system name")

        # userdata must fill the disk: a fixed size is refused.
        fixed = json.loads(production_policy.read_text())
        fixed["data_disk"]["partitions"][2]["size_bytes"] = 1 << 30
        fixed_path = root / "fixed-policy.json"
        fixed_path.write_text(json.dumps(fixed))
        if run_tool(tool, "validate", "--policy", fixed_path).returncode == 0:
            fail("the policy accepted a fixed-size userdata partition")


def verify_pin_manifest(tool: Path) -> None:
    """Only a project the manifest reads at a branch needs pinning."""
    commit = "1" * 40
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        base = root / "base-manifest.xml"
        base.write_text(
            '<manifest><remote name="r" fetch=".."/><default remote="r"/>'
            f'<project name="tag" path="a/tag" revision="{commit}" '
            'upstream="refs/tags/fixture"/>'
            f'<project name="branch" path="a/branch" revision="{commit}" '
            'upstream="refs/heads/fixture"/>'
            f'<project name="branch" path="b/branch" revision="{commit}" '
            'upstream="refs/heads/other"/></manifest>'
        )
        pin = root / "local_manifests/pin.xml"
        run_tool(tool, "--base-manifest", base, "--output", pin, check=True)
        expected = (
            '<?xml version="1.0" encoding="UTF-8"?>\n<manifest>\n'
            f'  <extend-project name="branch" path="a/branch" revision="{commit}" '
            'upstream="refs/heads/fixture" />\n'
            f'  <extend-project name="branch" path="b/branch" revision="{commit}" '
            'upstream="refs/heads/other" />\n</manifest>\n'
        )
        if pin.read_text() != expected:
            fail(f"pin manifest differs from the expected local manifest: {pin.read_text()}")
        base.write_text(base.read_text().replace(commit, "refs/heads/moving", 1))
        refused = run_tool(
            tool, "--base-manifest", base, "--output", root / "refused.xml"
        )
        if refused.returncode == 0 or "not pinned to a commit" not in refused.stdout:
            fail("pin manifest rendering accepted a project not pinned to a commit")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument(
        "--part",
        choices=("all", "verifiers", "target-files"),
        default="all",
        help="run the target-files fixtures alone, everything else alone, or both",
    )
    args = parser.parse_args()
    root = args.source_root.resolve()

    android_root = root / "android/lineage-24"
    kernel_root = root / "kernel/mainline-6.18"

    if args.part == "target-files":
        verify_target_files_fixtures(android_root / "tools/verify-target-files.py")
        print("Android 17 target-files verifier fixtures passed")
        return

    subprocess.run(
        [
            str(android_root / "tools/verify-aosp-patches.py"),
            "--android-root",
            str(android_root),
        ],
        check=True,
    )
    verify_kernel_elf_fixtures(android_root / "build/verify-kernel-elf.py")
    if args.part == "all":
        verify_target_files_fixtures(android_root / "tools/verify-target-files.py")
    verify_hal_selections(android_root)
    verify_net_setup_tool(android_root)
    verify_erofs_board(android_root)
    verify_userdata_grow(android_root)
    verify_zram_fstab(android_root)
    verify_product_identity(android_root)
    verify_hardware_identity(android_root)
    verify_host_cpu_name(root, android_root)
    verify_pinner_overlay(android_root)
    verify_wifi_network(android_root)
    verify_secondary_zygote_topic(android_root)
    verify_decoder_output_delay_topic(android_root)
    verify_secure_element_topic(android_root)
    verify_reclaim_boot_apps(android_root)
    verify_sms_holder(android_root)
    verify_time_zone(android_root)
    verify_keyboard_default(android_root)
    verify_default_wallpaper(android_root)
    verify_setup_wizard_flow(android_root)
    verify_launcher_layouts(android_root)
    verify_overlay_sepolicy(android_root)
    verify_lmkd_gpu_reclaim(android_root)
    verify_network_stack_topic(android_root)
    verify_wifi_tracker_topic(android_root)
    verify_camerax_denylist_assertion(android_root)
    verify_stale_build_properties(android_root)
    verify_media_codecs_overlay(android_root)
    verify_package_assertions(android_root)
    verify_debugging_defaults(android_root)
    verify_adb_secure_and_lunch_topics(android_root)
    verify_pin_manifest(android_root / "tools/render-pin-manifest.py")
    verify_initramfs_fixtures(android_root / "tools/verify-initramfs.py")
    verify_boot_image_fixtures(android_root / "tools/boot-image.py")
    verify_lineage_recovery_fixtures(
        android_root / "tools/verify-lineage-recovery.py", android_root
    )
    verify_ubd_disk_fixtures(
        android_root / "tools/ubd-disk.py",
        android_root / "support/image/disk-policy.json",
    )

    scripts = list((android_root / "build").rglob("*.sh"))
    scripts += list((android_root / "tools").rglob("*.py"))
    forbidden_commands = re.compile(
        r"(^|[;&|()\s])(ssh|scp|sftp|rsync|rsh)([;&|()\s]|$)", re.MULTILINE
    )
    forbidden_endpoint = re.compile(
        r"(BUILDER_(HOST|USER|PORT)|SSH_|CREDENTIAL|PRIVATE_KEY|REMOTE_URI)"
    )
    for script in scripts:
        text = script.read_text()
        if forbidden_commands.search(text):
            fail(f"tracked producer contains a transport command: {script}")
        if forbidden_endpoint.search(text):
            fail(f"tracked producer contains endpoint or credential state: {script}")
        if "/home/build/" in text:
            fail(f"tracked producer stores a builder-specific path: {script}")

    tracked_policy_text = "\n".join(
        path.read_text(errors="surrogateescape")
        for path in (
            android_root / "README.md",
            android_root / "metadata/lineage.env",
            kernel_root / "README.md",
            root / "kernel/common/config/darwin-uml.fragment",
            kernel_root / "config/android.fragment",
            kernel_root / "patches/metadata.env",
        )
    )
    for token in ("BUILDER_ALIAS", "/home/build/", "BUILDER_HOST", "PRIVATE_KEY"):
        if token in tracked_policy_text:
            fail(f"tracked Android policy stores operator state: {token}")

    print("Android 17 builder lane verifiers passed")


if __name__ == "__main__":
    main()
