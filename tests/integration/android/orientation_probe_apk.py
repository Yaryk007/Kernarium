# SPDX-License-Identifier: GPL-2.0-or-later
"""An application that only asks Android for a screen orientation, built without Android's SDK.

run_android_orientation.py needs a foreground activity that requests a given orientation (landscape,
sensor landscape, portrait, full sensor), and a host can build no APK with Android's tools. The package
this module writes has no code: each activity is a framework class (``android.app.Activity``,
``ListActivity`` and the like), which ActivityThread loads from the boot class path, so a manifest with the
``screenOrientation`` of each is the whole application. The manifest is Android's binary XML, written here;
the signature is the JAR signature (scheme v1) that ``jarsigner`` makes, which Android accepts for an
application that targets below API 30 (the target is 29, which every release since 14 installs).

The runner's tooling test reads the manifest back with ``decode_manifest``.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import struct
import subprocess
import tempfile
import zipfile
from dataclasses import dataclass

PACKAGE = "org.example.andarium.orientationprobe"
TARGET_SDK = 29
MIN_SDK = 29

# android.R.attr ids of the attributes the manifest uses. The binary format lists them in this order, first
# in the string pool, as the resource map that follows the pool.
ATTRIBUTES = {
    "label": 0x01010001,
    "name": 0x01010003,
    "sharedUserId": 0x0101000B,
    "hasCode": 0x0101000C,
    "debuggable": 0x0101000F,
    "exported": 0x01010010,
    "process": 0x01010011,
    "screenOrientation": 0x0101001E,
    "minSdkVersion": 0x0101020C,
    "versionCode": 0x0101021B,
    "targetSdkVersion": 0x01010270,
}
ANDROID_NAMESPACE = "http://schemas.android.com/apk/res/android"

# ActivityInfo.SCREEN_ORIENTATION_*: the values of the manifest's screenOrientation.
ORIENTATIONS = {
    "landscape": 0,
    "portrait": 1,
    "sensorLandscape": 6,
    "fullSensor": 10,
}
# One framework activity class for each request: a class that needs no code of the package's own.
ACTIVITY_CLASSES = {
    "landscape": "android.app.Activity",
    "portrait": "android.app.ListActivity",
    "sensorLandscape": "android.app.ExpandableListActivity",
    "fullSensor": "android.app.TabActivity",
}

TYPE_XML = 0x0003
TYPE_STRING_POOL = 0x0001
TYPE_RESOURCE_MAP = 0x0180
TYPE_START_NAMESPACE = 0x0100
TYPE_END_NAMESPACE = 0x0101
TYPE_START_ELEMENT = 0x0102
TYPE_END_ELEMENT = 0x0103
VALUE_REFERENCE = 0x01
VALUE_STRING = 0x03
VALUE_INT = 0x10
VALUE_BOOLEAN = 0x12
NONE = 0xFFFFFFFF


@dataclass(frozen=True)
class Reference:
    """An attribute value that names a resource (``@string/label``): its id."""
    id: int


@dataclass(frozen=True)
class Element:
    tag: str
    attributes: tuple[tuple[str, str | int | bool | Reference], ...] = ()
    children: tuple["Element", ...] = ()
    #: Attributes in the android namespace; the others (``package``) have none.
    plain: tuple[str, ...] = ("package",)


def manifest_tree() -> Element:
    """The package's manifest: one launcher activity for each orientation request."""
    activities = tuple(
        Element("activity", (
            ("name", ACTIVITY_CLASSES[request]),
            ("exported", True),
            ("screenOrientation", ORIENTATIONS[request]),
        ), (Element("intent-filter", (), (
            Element("action", (("name", "android.intent.action.MAIN"),)),
            Element("category", (("name", "android.intent.category.LAUNCHER"),)),
        )),))
        for request in ORIENTATIONS
    )
    return Element("manifest", (("package", PACKAGE), ("versionCode", 1)), (
        Element("uses-sdk", (("minSdkVersion", MIN_SDK), ("targetSdkVersion", TARGET_SDK))),
        Element("application", (("label", "Orientation Probe"), ("hasCode", False)), activities),
    ))


class _Strings:
    """The string pool: the attribute names first (they carry the resource map), then the rest."""

    def __init__(self, attributes: dict[str, int]) -> None:
        self.items: list[str] = list(attributes)

    def index(self, text: str) -> int:
        if text not in self.items:
            self.items.append(text)
        return self.items.index(text)


def _pool(strings: list[str]) -> bytes:
    data = bytearray()
    offsets = []
    for text in strings:
        offsets.append(len(data))
        data += struct.pack("<H", len(text)) + text.encode("utf-16-le") + b"\0\0"
    while len(data) % 4:
        data += b"\0"
    start = 28 + 4 * len(strings)
    body = struct.pack("<IIIII", len(strings), 0, 0, start, 0)
    body += struct.pack(f"<{len(strings)}I", *offsets) + bytes(data)
    return struct.pack("<HHI", TYPE_STRING_POOL, 28, 8 + len(body)) + body


def encode_manifest(tree: Element | None = None, attributes: dict[str, int] | None = None) -> bytes:
    """Android's binary XML for the tree (default: this package's manifest). `attributes` is the table of the
    attribute names the tree uses and their android.R.attr ids (default: ATTRIBUTES)."""
    root = tree or manifest_tree()
    known = ATTRIBUTES if attributes is None else attributes
    strings = _Strings(known)
    android = strings.index("android")
    namespace = strings.index(ANDROID_NAMESPACE)
    body = bytearray()
    line = 1

    def walk(element: Element) -> None:
        nonlocal line
        attributes = bytearray()
        # Android reads the attributes in the order of their resource ids, the plain ones after them.
        for name, value in sorted(
                element.attributes, key=lambda item: (item[0] in element.plain, known.get(item[0], 0), item[0])):
            plain = name in element.plain
            raw = NONE
            if isinstance(value, Reference):
                kind, data = VALUE_REFERENCE, value.id
            elif isinstance(value, bool):
                kind, data = VALUE_BOOLEAN, 0xFFFFFFFF if value else 0
            elif isinstance(value, int):
                kind, data = VALUE_INT, value
            else:
                raw = strings.index(value)
                kind, data = VALUE_STRING, raw
            attributes += struct.pack(
                "<IIIHBBI", NONE if plain else namespace, strings.index(name), raw, 8, 0, kind, data)
        count = len(element.attributes)
        body.extend(struct.pack(
            "<HHIII", TYPE_START_ELEMENT, 16, 36 + 20 * count, line, NONE)
            + struct.pack("<IIHHHHHH", NONE, strings.index(element.tag), 20, 20, count, 0, 0, 0)
            + bytes(attributes))
        line += 1
        for child in element.children:
            walk(child)
        body.extend(struct.pack("<HHIII", TYPE_END_ELEMENT, 16, 24, line, NONE)
                    + struct.pack("<II", NONE, strings.index(element.tag)))
        line += 1

    walk(root)
    start = struct.pack("<HHIII", TYPE_START_NAMESPACE, 16, 24, 1, NONE) + struct.pack("<II", android, namespace)
    end = struct.pack("<HHIII", TYPE_END_NAMESPACE, 16, 24, line, NONE) + struct.pack("<II", android, namespace)
    resource_map = struct.pack("<HHI", TYPE_RESOURCE_MAP, 8, 8 + 4 * len(known)) + struct.pack(
        f"<{len(known)}I", *known.values())
    document = _pool(strings.items) + resource_map + start + bytes(body) + end
    return struct.pack("<HHI", TYPE_XML, 8, 8 + len(document)) + document


# ---- Reading it back (the tooling test checks the writer with this reader).

@dataclass
class Node:
    tag: str
    attributes: dict[str, str | int | bool | Reference]
    children: list["Node"]


def decode_manifest(data: bytes) -> Node:
    """The tree in Android's binary XML (UTF-16 string pools, the form `encode_manifest` writes)."""
    kind, header, size = struct.unpack_from("<HHI", data, 0)
    if kind != TYPE_XML or size != len(data):
        raise ValueError("not a binary XML document")
    strings: list[str] = []
    stack: list[Node] = []
    root: Node | None = None
    offset = header
    while offset < len(data):
        kind, header, size = struct.unpack_from("<HHI", data, offset)
        if size < header or offset + size > len(data):
            raise ValueError(f"chunk {kind:#x} at {offset} has size {size}")
        if kind == TYPE_STRING_POOL:
            count, _, flags, start, _ = struct.unpack_from("<IIIII", data, offset + 8)
            if flags & 0x100:
                raise ValueError("UTF-8 pool")
            offsets = struct.unpack_from(f"<{count}I", data, offset + 28)
            for item in offsets:
                at = offset + start + item
                (length,) = struct.unpack_from("<H", data, at)
                strings.append(data[at + 2:at + 2 + 2 * length].decode("utf-16-le"))
        elif kind == TYPE_START_ELEMENT:
            name, attribute_start, _, count = struct.unpack_from("<4xiHHH", data, offset + 16)
            attributes: dict[str, str | int | bool | Reference] = {}
            for index in range(count):
                _, attribute, raw, _, _, value_kind, value = struct.unpack_from(
                    "<IIIHBBI", data, offset + 16 + attribute_start + 20 * index)
                if value_kind == VALUE_STRING:
                    attributes[strings[attribute]] = strings[raw]
                elif value_kind == VALUE_REFERENCE:
                    attributes[strings[attribute]] = Reference(value)
                elif value_kind == VALUE_BOOLEAN:
                    attributes[strings[attribute]] = bool(value)
                else:
                    attributes[strings[attribute]] = value
            node = Node(strings[name], attributes, [])
            if stack:
                stack[-1].children.append(node)
            else:
                root = node
            stack.append(node)
        elif kind == TYPE_END_ELEMENT:
            stack.pop()
        offset += size
    if root is None or stack:
        raise ValueError("no complete element tree")
    return root


def build_apk(destination: pathlib.Path, workdir: pathlib.Path | None = None,
              tree: Element | None = None, files: dict[str, bytes] | None = None,
              keystore: pathlib.Path | None = None, attributes: dict[str, int] | None = None) -> pathlib.Path:
    """Writes the signed package to `destination`: a throwaway key signs it, so the result belongs to no one.
    The package is this module's orientation probe, or the application of another manifest `tree` with the
    other entries `files` names, such as classes.dex (property_probe_apk.py). `keystore` names a throwaway
    key to share between packages (made on first use, and left in place): packages signed with one key can
    update each other. `attributes` is passed on to encode_manifest."""
    own = workdir is None
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="uml-orientation-apk-")) if own else workdir
    try:
        unsigned = scratch / "probe.apk"
        with zipfile.ZipFile(unsigned, "w", zipfile.ZIP_DEFLATED) as package:
            package.writestr("AndroidManifest.xml", encode_manifest(tree, attributes))
            for name, content in (files or {}).items():
                package.writestr(name, content)
        keystore = keystore or scratch / "throwaway.p12"
        run = lambda *command: subprocess.run(  # noqa: E731
            command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=120, check=True)
        if not keystore.exists():
            run("keytool", "-genkeypair", "-keystore", str(keystore), "-storetype", "PKCS12", "-storepass",
                "orientation", "-keypass", "orientation", "-alias", "probe", "-keyalg", "RSA", "-keysize", "2048",
                "-validity", "30", "-dname", "CN=Orientation Probe")
        run("jarsigner", "-keystore", str(keystore), "-storepass", "orientation", "-sigalg", "SHA256withRSA",
            "-digestalg", "SHA-256", str(unsigned), "probe")
        destination.write_bytes(unsigned.read_bytes())
        return destination
    finally:
        if own:
            for item in scratch.iterdir():
                item.unlink()
            scratch.rmdir()


def component(request: str) -> str:
    """The component to start for an orientation request."""
    return f"{PACKAGE}/{ACTIVITY_CLASSES[request]}"
