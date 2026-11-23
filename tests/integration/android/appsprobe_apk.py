# SPDX-License-Identifier: GPL-2.0-or-later
"""The applications that `run_android_app_shortcuts.py` installs, built without Android's SDK.

The launcher-application command (android/lineage-24/overlay/app_shortcuts) lists the activities that handle
MAIN with the LAUNCHER category, with their labels, and starts one. Each activity of these packages is a
framework class (see orientation_probe_apk.py: the package has no code of its own), so a manifest is the whole
application, and the manifest is what the command reads:

- ``first`` has five activities: ``listed`` (a launcher activity whose label holds a TAB, a line break, a
  right-to-left override and a zero-width space, which the listing must escape), ``long`` (a launcher activity
  whose label is longer than the listing's bound), ``hidden`` (a launcher activity that is not exported, which
  the command must neither list nor start), ``plain`` (exported, with no launcher filter, which the command
  must not start either) and ``guarded`` (exported, with a launcher filter, behind a permission that nothing
  grants the shell user: not an entry, so neither listed, drawn nor started), and an ``activity-alias``
  (``alias``, a launcher alias that is disabled in the manifest: the command lists it, draws it and starts it
  only once ``pm enable`` has enabled it for the user). The package declares no icon, so the icon of its
  activities is ``noicon``;
- ``second`` has one launcher activity, a resource-backed one: its label is a string resource and its icon (the
  application's) is an adaptive icon of a colour resource and a bitmap layer, the case the command's resource
  path, source bound and composition exist for. It is installed and removed to change the set of applications;
- ``bad`` is ``second`` with a foreground layer that is not a picture: its icon cannot be drawn, so the renderer
  answers ``error`` for it and for it alone, among the icons of good applications asked in the same batch;
- ``first-v2`` is ``first`` with a higher version code, signed with the same key: an update in place;
- the caller is no launcher application: one exported broadcast receiver (the property probe's code) whose shell
  calls the icon renderer's provider as an ordinary application, which the platform must refuse. It is debuggable so
  that the runner reads the answer with ``run-as``, and its code is property_probe_dex.py's.

The three variants are test-only applications, installed with ``-t``: the shell may change the state of a component only for a
test-only package, which the alias's ``pm enable`` needs.

The runner's tooling test reads the manifests back with ``orientation_probe_apk.decode_manifest``.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import struct
import zlib

import orientation_probe_apk as binary_xml
import property_probe_dex

PACKAGE_PREFIX = "org.example.andarium.appsprobe"
VARIANTS = ("first", "second", "bad")
SUCCESSORS = ("first-v2",)
#: The variants whose icon is the resource-backed adaptive one (``bad`` with a foreground that does not decode).
RESOURCE_VARIANTS = ("second", "bad")

#: The framework classes the activities are, one for each activity of a package: a class that needs no code.
#: The attributes the probe's XML uses (the shared table's, and the icon, the drawable of a layer, enabled and the
#: alias's target), with their android.R.attr ids.
ATTRIBUTES = {**binary_xml.ATTRIBUTES, "icon": 0x01010002, "enabled": 0x0101000E, "drawable": 0x01010199,
              "targetActivity": 0x01010202,
              "permission": 0x01010006, "testOnly": 0x01010272}

#: The resource ids of the second package's table (package 0x7f; types 1 mipmap, 2 drawable, 3 color, 4 string).
RES_MIPMAP_ICON = 0x7F010000
RES_DRAWABLE_FOREGROUND = 0x7F020000
RES_COLOR_BACKGROUND = 0x7F030000
RES_STRING_LABEL = 0x7F040000
ICON_XML = "res/mipmap-anydpi-v26/ic_launcher.xml"
FOREGROUND_PNG = "res/drawable-xxxhdpi/layer_foreground.png"
#: The colours of the adaptive icon: the background fills the square, the foreground is a disc over its middle.
BACKGROUND_RGB = (0x33, 0x66, 0x99)
FOREGROUND_RGB = (0xFF, 0xC8, 0x00)
FOREGROUND_SIZE = 216
DISC_RADIUS = 60

CLASSES = {
    "listed": "android.app.Activity",
    "long": "android.app.TabActivity",
    "hidden": "android.app.ListActivity",
    "plain": "android.app.ExpandableListActivity",
    "guarded": "android.app.AliasActivity",
    "only": "android.app.Activity",
}
#: The permission that ``guarded`` asks for: nobody defines it, so the shell user does not hold it.
GUARD_PERMISSION = f"{PACKAGE_PREFIX}.permission.GUARD"
#: The launcher alias of ``first``: its own name, and the activity it stands for.
ALIAS_NAME = "org.example.andarium.appsprobe.first.Alias"
ALIAS_TARGET = CLASSES["listed"]
ALIAS_LABEL = "Alias Probe"

#: Labels as the manifest holds them (BMP characters only: the string pool counts UTF-16 units)...
HOSTILE_LABEL = "Apps\tProbe\nName‮ Evil​  Label "
LONG_LABEL = "Long" + "x" * 196
SECOND_LABEL = "Second Probe"
#: What ``bad`` holds where the foreground bitmap should be: the signature of a PNG and then no chunk at all.
BAD_FOREGROUND = b"\x89PNG\r\n\x1a\n" + b"this is not a picture" * 8
#: ...and what the listing must show for them.
LISTED_HOSTILE_LABEL = "Apps Probe Name Evil Label"
LISTED_LONG_LABEL = "Long" + "x" * 75 + "…"
LISTED_SECOND_LABEL = SECOND_LABEL
LISTED_ALIAS_LABEL = ALIAS_LABEL


def package_of(variant: str) -> str:
    return f"{PACKAGE_PREFIX}.{variant.split('-')[0]}"


def activity_names(variant: str) -> tuple[str, ...]:
    """The activities a variant's manifest declares."""
    return ("listed", "long", "hidden", "plain", "guarded") if variant.startswith("first") else ("only",)


def alias_component() -> str:
    """The component token of the launcher alias of ``first``."""
    return f"{package_of('first')}/{ALIAS_NAME}"


def component(variant: str, activity: str) -> str:
    """The component token (package/class) of one of a variant's activities."""
    return f"{package_of(variant)}/{CLASSES[activity]}"


def launcher_filter() -> binary_xml.Element:
    return binary_xml.Element("intent-filter", (), (
        binary_xml.Element("action", (("name", "android.intent.action.MAIN"),)),
        binary_xml.Element("category", (("name", "android.intent.category.LAUNCHER"),)),
    ))


def manifest_tree(variant: str) -> binary_xml.Element:
    version = 2 if variant.endswith("-v2") else 1
    activities: list[binary_xml.Element] = []
    resource_backed = variant in RESOURCE_VARIANTS
    for activity in activity_names(variant):
        attributes: list[tuple[str, str | int | bool | binary_xml.Reference]] = [("name", CLASSES[activity])]
        attributes.append(("exported", activity != "hidden"))
        if activity == "guarded":
            attributes.append(("permission", GUARD_PERMISSION))
        label: str | binary_xml.Reference | None = {"listed": HOSTILE_LABEL, "long": LONG_LABEL}.get(activity)
        if activity == "only":
            label = binary_xml.Reference(RES_STRING_LABEL)
        if label is not None:
            attributes.append(("label", label))
        has_filter = activity != "plain"
        activities.append(binary_xml.Element("activity", tuple(attributes), (launcher_filter(),) if has_filter else ()))
    if variant.startswith("first"):
        activities.append(binary_xml.Element("activity-alias", (
            ("name", ALIAS_NAME), ("targetActivity", ALIAS_TARGET), ("exported", True), ("enabled", False),
            ("label", ALIAS_LABEL)), (launcher_filter(),)))
    application: list[tuple[str, str | int | bool | binary_xml.Reference]] = [
        ("label", "Apps Probe Application"), ("hasCode", False), ("testOnly", True)]
    if resource_backed:
        application.append(("icon", binary_xml.Reference(RES_MIPMAP_ICON)))
    return binary_xml.Element("manifest", (("package", package_of(variant)), ("versionCode", version)), (
        binary_xml.Element("uses-sdk", (("minSdkVersion", binary_xml.MIN_SDK),
                                        ("targetSdkVersion", binary_xml.TARGET_SDK))),
        binary_xml.Element("application", tuple(application), tuple(activities)),
    ))


def icon_tree() -> binary_xml.Element:
    """The adaptive icon: a colour background and a bitmap foreground."""
    return binary_xml.Element("adaptive-icon", (), (
        binary_xml.Element("background", (("drawable", binary_xml.Reference(RES_COLOR_BACKGROUND)),)),
        binary_xml.Element("foreground", (("drawable", binary_xml.Reference(RES_DRAWABLE_FOREGROUND)),)),
    ))


def foreground_png() -> bytes:
    """An 8-bit RGBA PNG of FOREGROUND_SIZE pixels: a disc over transparency."""
    size, middle = FOREGROUND_SIZE, FOREGROUND_SIZE / 2
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            inside = (x + 0.5 - middle) ** 2 + (y + 0.5 - middle) ** 2 <= DISC_RADIUS ** 2
            rows += bytes((*FOREGROUND_RGB, 255)) if inside else b"\0\0\0\0"

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


# ---- The resource table (resources.arsc) of the resource-backed package.

TYPE_TABLE = 0x0002
TYPE_PACKAGE = 0x0200
TYPE_TYPE = 0x0201
TYPE_SPEC = 0x0202
DENSITY_ANY = 0xFFFE
DENSITY_XXXHIGH = 640
CONFIG_DENSITY = 0x100
CONFIG_VERSION = 0x400
VALUE_STRING = 0x03
VALUE_COLOR = 0x1C
CONFIG_SIZE = 52

#: (type id, type name, entry name, spec flags, density, sdk, value type, value) for every resource of the table.
RESOURCES = (
    (1, "mipmap", "ic_launcher", CONFIG_DENSITY | CONFIG_VERSION, DENSITY_ANY, 26, VALUE_STRING, ICON_XML),
    (2, "drawable", "layer_foreground", CONFIG_DENSITY, DENSITY_XXXHIGH, 0, VALUE_STRING, FOREGROUND_PNG),
    (3, "color", "layer_background", 0, 0, 0, VALUE_COLOR,
     0xFF000000 | BACKGROUND_RGB[0] << 16 | BACKGROUND_RGB[1] << 8 | BACKGROUND_RGB[2]),
    (4, "string", "probe_label", 0, 0, 0, VALUE_STRING, SECOND_LABEL),
)


def _config(density: int, sdk: int) -> bytes:
    """A ResTable_config of CONFIG_SIZE bytes, with only the density and the SDK version set."""
    return struct.pack("<IIIBBHBBBBHHHHBBHHH12sBBH", CONFIG_SIZE, 0, 0, 0, 0, density, 0, 0, 0, 0, 0, 0, sdk, 0, 0, 0,
                       0, 0, 0, b"\0" * 12, 0, 0, 0)


def build_resource_table(package: str) -> bytes:
    """resources.arsc for the package: the adaptive icon's mipmap (anydpi-v26), the foreground bitmap (xxxhdpi), the
    background colour and the label string (default configuration), each in the form aapt2 writes."""
    pool = binary_xml._pool
    values = [str(row[7]) for row in RESOURCES if row[6] == VALUE_STRING]
    type_names = [row[1] for row in RESOURCES]
    key_names = [row[2] for row in RESOURCES]
    chunks = bytearray()
    for index, (type_id, _, _, flags, density, sdk, kind, value) in enumerate(RESOURCES):
        chunks += struct.pack("<HHIBBHI", TYPE_SPEC, 16, 16 + 4, type_id, 0, 0, 1) + struct.pack("<I", flags)
        data = values.index(value) if kind == VALUE_STRING else value
        entry = struct.pack("<HHI", 8, 0, index) + struct.pack("<HBBI", 8, 0, kind, data)
        config = _config(density, sdk)
        header = 8 + 12 + len(config)
        body = struct.pack("<I", 0) + entry
        chunks += (struct.pack("<HHIBBHII", TYPE_TYPE, header, header + len(body), type_id, 0, 0, 1, header + 4)
                   + config + body)
    type_pool, key_pool = pool(type_names), pool(key_names)
    name = package.encode("utf-16-le")[:254].ljust(256, b"\0")
    package_header = 288
    package_chunk = (struct.pack("<HHII", TYPE_PACKAGE, package_header,
                                 package_header + len(type_pool) + len(key_pool) + len(chunks), 0x7F)
                     + name + struct.pack("<IIIII", package_header, len(type_names), package_header + len(type_pool),
                                          len(key_names), 0)
                     + type_pool + key_pool + bytes(chunks))
    global_pool = pool(values)
    return (struct.pack("<HHII", TYPE_TABLE, 12, 12 + len(global_pool) + len(package_chunk), 1) + global_pool
            + package_chunk)


def decode_resource_table(data: bytes) -> dict[int, tuple[str, str, int, int, int, str | int]]:
    """The table's resources by id: (type name, entry name, density, sdk, value type, value: the string or the number).
    Raises ValueError when a chunk's size, an offset or an index is not consistent (the reader the tooling test checks
    the writer with)."""
    kind, header, size, packages = struct.unpack_from("<HHII", data, 0)
    if kind != TYPE_TABLE or header != 12 or size != len(data) or packages != 1:
        raise ValueError("not a resource table of one package")

    def read_pool(at: int) -> tuple[list[str], int]:
        _, _, chunk_size, count, styles, flags, start, _ = struct.unpack_from("<HHIIIIII", data, at)
        if styles or flags:
            raise ValueError("a pool with styles or flags")
        offsets = struct.unpack_from(f"<{count}I", data, at + 28)
        items = []
        for offset in offsets:
            (length,) = struct.unpack_from("<H", data, at + start + offset)
            items.append(data[at + start + offset + 2:at + start + offset + 2 + 2 * length].decode("utf-16-le"))
        return items, at + chunk_size

    values, at = read_pool(12)
    kind, header, size, package_id = struct.unpack_from("<HHII", data, at)
    if kind != TYPE_PACKAGE or header != 288 or at + size != len(data) or package_id != 0x7F:
        raise ValueError("the package chunk is not the rest of the table")
    package_at = at
    type_strings, _, key_strings, _, _ = struct.unpack_from("<IIIII", data, at + 8 + 4 + 256)
    types, _ = read_pool(package_at + type_strings)
    keys, at = read_pool(package_at + key_strings)
    found: dict[int, tuple[str, str, int, int, int, str | int]] = {}
    while at < len(data):
        kind, header, size = struct.unpack_from("<HHI", data, at)
        if size < header or at + size > len(data):
            raise ValueError(f"chunk {kind:#x} at {at} has size {size}")
        if kind == TYPE_TYPE:
            type_id, _, _, count, entries = struct.unpack_from("<BBHII", data, at + 8)
            density = struct.unpack_from("<H", data, at + 20 + 14)[0]
            sdk = struct.unpack_from("<H", data, at + 20 + 24)[0]
            if entries != header + 4 * count or entries > size:
                raise ValueError("the entries do not follow the offsets")
            for index in range(count):
                (offset,) = struct.unpack_from("<I", data, at + header + 4 * index)
                if offset == 0xFFFFFFFF:
                    continue
                entry_size, flags, key = struct.unpack_from("<HHI", data, at + entries + offset)
                value_size, _, value_type, value = struct.unpack_from("<HBBI", data, at + entries + offset + 8)
                if entry_size != 8 or flags or value_size != 8:
                    raise ValueError("an entry that is not a plain value")
                found[0x7F000000 | type_id << 16 | index] = (
                    types[type_id - 1], keys[key], density, sdk, value_type,
                    values[value] if value_type == VALUE_STRING else value)
        at += size
    return found


def package_files(variant: str) -> dict[str, bytes]:
    """The files of the package besides its manifest: the resource table, the adaptive icon and the bitmap of the
    resource-backed variant."""
    if variant not in RESOURCE_VARIANTS:
        return {}
    return {
        "resources.arsc": build_resource_table(package_of(variant)),
        ICON_XML: binary_xml.encode_manifest(icon_tree(), ATTRIBUTES),
        FOREGROUND_PNG: BAD_FOREGROUND if variant == "bad" else foreground_png(),
    }


#: The caller: the package, its receiver, the file its shell writes (relative to the data directory, as `run-as` reads it)
#: and what that shell runs as the application: the shell user's own call of the renderer's provider.
CALLER_PACKAGE = f"{PACKAGE_PREFIX}.caller"
CALLER_RECEIVER = property_probe_dex.CLASS_NAME
CALLER_RESULT = "files/result"
RENDERER_AUTHORITY = "com.kernarium.icons"
CALL_RENDERER = f"content call --uri content://{RENDERER_AUTHORITY} --method render"
CALLER_SCRIPT = (f"d=/data/data/{CALLER_PACKAGE}/files; mkdir -p $d; "
                 f"{{ {CALL_RENDERER} 2>&1; echo \"status=$?\"; }} > $d/result.tmp && mv $d/result.tmp $d/result")


def caller_manifest() -> binary_xml.Element:
    return binary_xml.Element("manifest", (("package", CALLER_PACKAGE), ("versionCode", 1)), (
        binary_xml.Element("uses-sdk", (("minSdkVersion", binary_xml.MIN_SDK), ("targetSdkVersion", binary_xml.TARGET_SDK))),
        # The receiver lists the network interfaces first (property_probe_dex.py), which an application without the
        # permission cannot do.
        binary_xml.Element("uses-permission", (("name", "android.permission.INTERNET"),)),
        binary_xml.Element("application", (("label", "Apps Probe Caller"), ("debuggable", True)), (
            binary_xml.Element("receiver", (("name", CALLER_RECEIVER), ("exported", True))),
        )),
    ))


def build_caller_apk(destination: pathlib.Path, keystore: pathlib.Path | None = None) -> pathlib.Path:
    """The signed package of the caller."""
    dex = property_probe_dex.build_dex(CALLER_PACKAGE, "", "", shell_script=CALLER_SCRIPT)
    return binary_xml.build_apk(destination, tree=caller_manifest(), keystore=keystore, files={"classes.dex": dex})


def build_apk(destination: pathlib.Path, variant: str, keystore: pathlib.Path | None = None) -> pathlib.Path:
    """Writes the signed package of a variant. The variants of one package (``first`` and ``first-v2``) are signed with
    one key, so that the second installs over the first in place: name the same `keystore` for both."""
    return binary_xml.build_apk(destination, tree=manifest_tree(variant), keystore=keystore,
                                files=package_files(variant), attributes=ATTRIBUTES)
