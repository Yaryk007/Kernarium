# SPDX-License-Identifier: GPL-2.0-or-later
"""A Camera API 1 application built without Android's SDK.

Apps that read a camera through android.hardware.Camera (face scanning, document scanning, video calls of an
older make) take the camera's orientation from ``CameraInfo.orientation`` and turn the preview frame they are
handed by it. The probe is that application's shape: the camera id comes from the intent (default 1, the front
camera), it opens the camera, writes ``info.txt`` (orientation, facing, preview size and format), starts a preview
into a dummy SurfaceTexture and writes each preview frame, as the NV21 bytes the application receives, to
``frame.nv21`` (through a temporary name, so a reader never sees half a frame). Java of this shape:

    public class Probe extends Activity implements Camera.PreviewCallback {
        protected void onCreate(Bundle b) {
            super.onCreate(b);
            int id = getIntent().getIntExtra("camera", 1);
            Camera.CameraInfo info = new Camera.CameraInfo();
            Camera.getCameraInfo(id, info);
            Camera camera = Camera.open(id);
            Camera.Parameters p = camera.getParameters();
            Camera.Size s = p.getPreviewSize();
            String t = "orientation=" + info.orientation + " facing=" + info.facing + " preview=" + s.width + "x"
                + s.height + " format=" + p.getPreviewFormat() + " camera=" + id + "\\n";
            FileOutputStream out = openFileOutput("info.txt", 0); out.write(t.getBytes()); out.close();
            camera.setPreviewTexture(new SurfaceTexture(7));
            camera.setPreviewCallback(this);
            camera.startPreview();
        }
        public void onPreviewFrame(byte[] data, Camera camera) {
            FileOutputStream out = openFileOutput("frame.tmp", 0); out.write(data); out.close();
            getFileStreamPath("frame.tmp").renameTo(getFileStreamPath("frame.nv21"));
        }
    }

The DEX layout follows property_probe_dex.py (version 035), extended with field references; the package is made
by orientation_probe_apk.build_apk (a throwaway key, JAR signature, target level 29).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import pathlib
import struct
import zlib

PACKAGE = "org.example.cameraprobe"
CLASS_NAME = PACKAGE + ".Probe"

OBJECT = "Ljava/lang/Object;"
STRING = "Ljava/lang/String;"
STRING_BUILDER = "Ljava/lang/StringBuilder;"
BYTES = "[B"
FILE = "Ljava/io/File;"
FILE_OUT = "Ljava/io/FileOutputStream;"
ACTIVITY = "Landroid/app/Activity;"
BUNDLE = "Landroid/os/Bundle;"
INTENT = "Landroid/content/Intent;"
CAMERA = "Landroid/hardware/Camera;"
CAMERA_INFO = "Landroid/hardware/Camera$CameraInfo;"
PARAMETERS = "Landroid/hardware/Camera$Parameters;"
SIZE = "Landroid/hardware/Camera$Size;"
CALLBACK = "Landroid/hardware/Camera$PreviewCallback;"
SURFACE_TEXTURE = "Landroid/graphics/SurfaceTexture;"

NO_INDEX = 0xFFFFFFFF
ACC_PUBLIC, ACC_PROTECTED, ACC_CONSTRUCTOR = 0x1, 0x4, 0x10000
TYPE_HEADER, TYPE_STRING_ID, TYPE_TYPE_ID, TYPE_PROTO_ID, TYPE_FIELD_ID = 0x0000, 0x0001, 0x0002, 0x0003, 0x0004
TYPE_METHOD_ID, TYPE_CLASS_DEF = 0x0005, 0x0006
TYPE_MAP_LIST, TYPE_TYPE_LIST, TYPE_CLASS_DATA, TYPE_CODE, TYPE_STRING_DATA = 0x1000, 0x1001, 0x2000, 0x2001, 0x2002


def descriptor(class_name: str) -> str:
    return "L" + class_name.replace(".", "/") + ";"


def uleb128(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def mutf8(text: str) -> bytes:
    if "\0" in text or any(ord(char) > 0xFFFF for char in text):
        raise ValueError("a string with a NUL or a supplementary character")
    return text.encode("utf-8")


def shorty(return_type: str, parameters: tuple[str, ...]) -> str:
    return "".join(("L" if item[0] in "L[" else item) for item in (return_type, *parameters))


class Method:
    def __init__(self, owner: str, name: str, return_type: str, parameters: tuple[str, ...] = ()) -> None:
        self.owner, self.name, self.return_type, self.parameters = owner, name, return_type, parameters

    def key(self) -> tuple:
        return ("method", self.owner, self.name, self.return_type, self.parameters)


class Field:
    def __init__(self, owner: str, name: str, kind: str) -> None:
        self.owner, self.name, self.kind = owner, name, kind

    def key(self) -> tuple:
        return ("field", self.owner, self.name, self.kind)


class Pools:
    def __init__(self) -> None:
        self.strings: set[str] = set()
        self.types: set[str] = set()
        self.protos: set[tuple[str, tuple[str, ...]]] = set()
        self.methods: dict[tuple, Method] = {}
        self.fields: dict[tuple, Field] = {}

    def string(self, text: str) -> str:
        self.strings.add(text)
        return text

    def type(self, name: str) -> str:
        self.types.add(name)
        self.strings.add(name)
        return name

    def method(self, owner: str, name: str, return_type: str, parameters: tuple[str, ...] = ()) -> Method:
        method = Method(owner, name, return_type, parameters)
        self.methods[method.key()] = method
        for item in (owner, return_type, *parameters):
            self.type(item)
        self.protos.add((return_type, parameters))
        self.strings.update((name, shorty(return_type, parameters)))
        return method

    def field(self, owner: str, name: str, kind: str) -> Field:
        field = Field(owner, name, kind)
        self.fields[field.key()] = field
        self.type(owner)
        self.type(kind)
        self.strings.add(name)
        return field

    def finish(self) -> None:
        self.sorted_strings = sorted(self.strings, key=mutf8)
        self.string_index = {text: number for number, text in enumerate(self.sorted_strings)}
        self.sorted_types = sorted(self.types, key=lambda name: self.string_index[name])
        self.type_index = {name: number for number, name in enumerate(self.sorted_types)}
        self.sorted_protos = sorted(
            self.protos, key=lambda item: (self.type_index[item[0]], [self.type_index[name] for name in item[1]]))
        self.proto_index = {item: number for number, item in enumerate(self.sorted_protos)}
        self.sorted_fields = sorted(
            self.fields.values(),
            key=lambda f: (self.type_index[f.owner], self.string_index[f.name], self.type_index[f.kind]))
        self.field_index = {f.key(): number for number, f in enumerate(self.sorted_fields)}
        self.sorted_methods = sorted(
            self.methods.values(),
            key=lambda m: (self.type_index[m.owner], self.string_index[m.name],
                           self.proto_index[(m.return_type, m.parameters)]))
        self.method_index = {m.key(): number for number, m in enumerate(self.sorted_methods)}


class Code:
    def __init__(self, registers: int, ins: int, outs: int) -> None:
        self.registers, self.ins, self.outs = registers, ins, outs
        self.items: list[tuple] = []

    def op(self, mnemonic: str, *operands: object) -> None:
        self.items.append((mnemonic, *operands))


OPCODES = {
    "return-void": (0x0E, "10x"),
    "const/4": (0x12, "11n"),
    "const-string": (0x1A, "21c-string"),
    "new-instance": (0x22, "21c-type"),
    "move-result": (0x0A, "11x"),
    "move-result-object": (0x0C, "11x"),
    "iget": (0x52, "22c-field"),
    "invoke-virtual": (0x6E, "35c"),
    "invoke-super": (0x6F, "35c"),
    "invoke-direct": (0x70, "35c"),
    "invoke-static": (0x71, "35c"),
}


def assemble(code: Code, pools: Pools) -> bytes:
    units: list[int] = []
    for item in code.items:
        opcode, form = OPCODES[item[0]]
        operands = item[1:]
        if form == "10x":
            units.append(opcode)
        elif form == "11n":
            register, literal = operands
            assert -8 <= literal <= 7 and register < 16
            units.append((literal & 0xF) << 12 | register << 8 | opcode)
        elif form == "11x":
            units.append(operands[0] << 8 | opcode)
        elif form == "21c-string":
            index = pools.string_index[operands[1]]
            assert index < 0x10000
            units += [operands[0] << 8 | opcode, index]
        elif form == "21c-type":
            units += [operands[0] << 8 | opcode, pools.type_index[operands[1]]]
        elif form == "22c-field":
            destination, objekt, field = operands
            assert destination < 16 and objekt < 16
            units += [objekt << 12 | destination << 8 | opcode, pools.field_index[field.key()]]
        elif form == "35c":
            method, registers = operands[0], list(operands[1:])
            assert len(registers) <= 5 and all(register < 16 for register in registers)
            padded = registers + [0] * (5 - len(registers))
            units += [len(registers) << 12 | padded[4] << 8 | opcode, pools.method_index[method.key()],
                      padded[3] << 12 | padded[2] << 8 | padded[1] << 4 | padded[0]]
        else:
            raise ValueError(form)
    body = struct.pack(f"<{len(units)}H", *units)
    header = struct.pack("<HHHHII", code.registers, code.ins, code.outs, 0, 0, len(units))
    return header + body


def build_dex() -> bytes:
    pools = Pools()
    owner = pools.type(descriptor(CLASS_NAME))
    for item in (ACTIVITY, CALLBACK, "V", "I", "Z"):
        pools.type(item)
    init = pools.method(owner, "<init>", "V")
    on_create = pools.method(owner, "onCreate", "V", (BUNDLE,))
    on_frame = pools.method(owner, "onPreviewFrame", "V", (BYTES, CAMERA))
    activity_init = pools.method(ACTIVITY, "<init>", "V")
    activity_create = pools.method(ACTIVITY, "onCreate", "V", (BUNDLE,))
    get_intent = pools.method(ACTIVITY, "getIntent", INTENT)
    open_file = pools.method(ACTIVITY, "openFileOutput", FILE_OUT, (STRING, "I"))
    file_path = pools.method(ACTIVITY, "getFileStreamPath", FILE, (STRING,))
    int_extra = pools.method(INTENT, "getIntExtra", "I", (STRING, "I"))
    info_init = pools.method(CAMERA_INFO, "<init>", "V")
    camera_info = pools.method(CAMERA, "getCameraInfo", "V", ("I", CAMERA_INFO))
    camera_open = pools.method(CAMERA, "open", CAMERA, ("I",))
    parameters = pools.method(CAMERA, "getParameters", PARAMETERS)
    preview_texture = pools.method(CAMERA, "setPreviewTexture", "V", (SURFACE_TEXTURE,))
    preview_callback = pools.method(CAMERA, "setPreviewCallback", "V", (CALLBACK,))
    start_preview = pools.method(CAMERA, "startPreview", "V")
    preview_size = pools.method(PARAMETERS, "getPreviewSize", SIZE)
    preview_format = pools.method(PARAMETERS, "getPreviewFormat", "I")
    builder_init = pools.method(STRING_BUILDER, "<init>", "V")
    append_text = pools.method(STRING_BUILDER, "append", STRING_BUILDER, (STRING,))
    append_int = pools.method(STRING_BUILDER, "append", STRING_BUILDER, ("I",))
    to_string = pools.method(STRING_BUILDER, "toString", STRING)
    get_bytes = pools.method(STRING, "getBytes", BYTES)
    write = pools.method(FILE_OUT, "write", "V", (BYTES,))
    close = pools.method(FILE_OUT, "close", "V")
    texture_init = pools.method(SURFACE_TEXTURE, "<init>", "V", ("I",))
    rename = pools.method(FILE, "renameTo", "Z", (FILE,))
    orientation = pools.field(CAMERA_INFO, "orientation", "I")
    facing = pools.field(CAMERA_INFO, "facing", "I")
    width = pools.field(SIZE, "width", "I")
    height = pools.field(SIZE, "height", "I")
    labels = ("orientation=", " facing=", " preview=", "x", " format=", " camera=", "\n")
    for text in ("camera", "info.txt", "frame.tmp", "frame.nv21", *labels):
        pools.string(text)

    constructor = Code(registers=1, ins=1, outs=1)
    constructor.op("invoke-direct", activity_init, 0)
    constructor.op("return-void")

    # v0 id, v1 info (later 0), v2 camera, v3 parameters, v4 size, v5 builder, v6/v7 temporaries; v8 this, v9 bundle.
    create = Code(registers=10, ins=2, outs=3)
    create.op("invoke-super", activity_create, 8, 9)
    create.op("invoke-virtual", get_intent, 8)
    create.op("move-result-object", 0)
    create.op("const-string", 1, "camera")
    create.op("const/4", 2, 1)
    create.op("invoke-virtual", int_extra, 0, 1, 2)
    create.op("move-result", 0)
    create.op("new-instance", 1, CAMERA_INFO)
    create.op("invoke-direct", info_init, 1)
    create.op("invoke-static", camera_info, 0, 1)
    create.op("invoke-static", camera_open, 0)
    create.op("move-result-object", 2)
    create.op("invoke-virtual", parameters, 2)
    create.op("move-result-object", 3)
    create.op("invoke-virtual", preview_size, 3)
    create.op("move-result-object", 4)
    create.op("new-instance", 5, STRING_BUILDER)
    create.op("invoke-direct", builder_init, 5)

    def text(value: str) -> None:
        create.op("const-string", 6, value)
        create.op("invoke-virtual", append_text, 5, 6)

    def number(register: int) -> None:
        create.op("invoke-virtual", append_int, 5, register)

    text("orientation=")
    create.op("iget", 6, 1, orientation)
    number(6)
    text(" facing=")
    create.op("iget", 6, 1, facing)
    number(6)
    text(" preview=")
    create.op("iget", 6, 4, width)
    number(6)
    text("x")
    create.op("iget", 6, 4, height)
    number(6)
    text(" format=")
    create.op("invoke-virtual", preview_format, 3)
    create.op("move-result", 6)
    number(6)
    text(" camera=")
    number(0)
    text("\n")
    create.op("invoke-virtual", to_string, 5)
    create.op("move-result-object", 6)
    create.op("invoke-virtual", get_bytes, 6)
    create.op("move-result-object", 6)
    create.op("const-string", 7, "info.txt")
    create.op("const/4", 1, 0)
    create.op("invoke-virtual", open_file, 8, 7, 1)
    create.op("move-result-object", 7)
    create.op("invoke-virtual", write, 7, 6)
    create.op("invoke-virtual", close, 7)
    create.op("new-instance", 6, SURFACE_TEXTURE)
    create.op("const/4", 7, 7)
    create.op("invoke-direct", texture_init, 6, 7)
    create.op("invoke-virtual", preview_texture, 2, 6)
    create.op("invoke-virtual", preview_callback, 2, 8)
    create.op("invoke-virtual", start_preview, 2)
    create.op("return-void")

    # v0, v1 temporaries; v2 this, v3 data, v4 camera.
    frame = Code(registers=5, ins=3, outs=3)
    frame.op("const-string", 0, "frame.tmp")
    frame.op("const/4", 1, 0)
    frame.op("invoke-virtual", open_file, 2, 0, 1)
    frame.op("move-result-object", 0)
    frame.op("invoke-virtual", write, 0, 3)
    frame.op("invoke-virtual", close, 0)
    frame.op("const-string", 0, "frame.tmp")
    frame.op("invoke-virtual", file_path, 2, 0)
    frame.op("move-result-object", 0)
    frame.op("const-string", 1, "frame.nv21")
    frame.op("invoke-virtual", file_path, 2, 1)
    frame.op("move-result-object", 1)
    frame.op("invoke-virtual", rename, 0, 1)
    frame.op("return-void")

    pools.finish()
    counts = {
        "string": len(pools.sorted_strings), "type": len(pools.sorted_types), "proto": len(pools.sorted_protos),
        "field": len(pools.sorted_fields), "method": len(pools.sorted_methods)}
    header_size = 0x70
    string_ids_off = header_size
    type_ids_off = string_ids_off + 4 * counts["string"]
    proto_ids_off = type_ids_off + 4 * counts["type"]
    field_ids_off = proto_ids_off + 12 * counts["proto"]
    method_ids_off = field_ids_off + 8 * counts["field"]
    class_defs_off = method_ids_off + 8 * counts["method"]
    data_off = class_defs_off + 32

    data = bytearray()
    sections: list[tuple[int, int, int]] = []

    def align(boundary: int) -> None:
        while (data_off + len(data)) % boundary:
            data.append(0)

    list_offsets: dict[tuple[str, ...], int] = {}
    lists = [parameters_ for _, parameters_ in pools.sorted_protos if parameters_]
    lists.append((CALLBACK,))
    align(4)
    first = data_off + len(data)
    for parameters_ in lists:
        if parameters_ in list_offsets:
            continue
        align(4)
        list_offsets[parameters_] = data_off + len(data)
        data += struct.pack("<I", len(parameters_)) + b"".join(
            struct.pack("<H", pools.type_index[item]) for item in parameters_)
    sections.append((TYPE_TYPE_LIST, len(list_offsets), first))

    offsets = {}
    align(4)
    code_first = data_off + len(data)
    for name, body in (("init", constructor), ("create", create), ("frame", frame)):
        align(4)
        offsets[name] = data_off + len(data)
        data += assemble(body, pools)
    sections.append((TYPE_CODE, 3, code_first))

    string_offsets = []
    string_first = data_off + len(data)
    for text_ in pools.sorted_strings:
        string_offsets.append(data_off + len(data))
        data += uleb128(len(text_)) + mutf8(text_) + b"\0"
    sections.append((TYPE_STRING_DATA, counts["string"], string_first))

    class_data_offset = data_off + len(data)
    virtuals = sorted(((pools.method_index[on_create.key()], ACC_PROTECTED, offsets["create"]),
                       (pools.method_index[on_frame.key()], ACC_PUBLIC, offsets["frame"])))
    data += uleb128(0) + uleb128(0) + uleb128(1) + uleb128(len(virtuals))
    data += uleb128(pools.method_index[init.key()]) + uleb128(ACC_PUBLIC | ACC_CONSTRUCTOR) + uleb128(offsets["init"])
    previous = 0
    for index, flags, offset in virtuals:
        data += uleb128(index - previous) + uleb128(flags) + uleb128(offset)
        previous = index
    sections.append((TYPE_CLASS_DATA, 1, class_data_offset))

    align(4)
    map_offset = data_off + len(data)
    entries = [(TYPE_HEADER, 1, 0), (TYPE_STRING_ID, counts["string"], string_ids_off),
               (TYPE_TYPE_ID, counts["type"], type_ids_off), (TYPE_PROTO_ID, counts["proto"], proto_ids_off),
               (TYPE_FIELD_ID, counts["field"], field_ids_off), (TYPE_METHOD_ID, counts["method"], method_ids_off),
               (TYPE_CLASS_DEF, 1, class_defs_off), *sections, (TYPE_MAP_LIST, 1, map_offset)]
    entries.sort(key=lambda entry: entry[2])
    data += struct.pack("<I", len(entries)) + b"".join(
        struct.pack("<HHII", kind, 0, count, offset) for kind, count, offset in entries)

    tables = bytearray()
    for offset in string_offsets:
        tables += struct.pack("<I", offset)
    for item in pools.sorted_types:
        tables += struct.pack("<I", pools.string_index[item])
    for return_type, parameters_ in pools.sorted_protos:
        tables += struct.pack("<III", pools.string_index[shorty(return_type, parameters_)],
                              pools.type_index[return_type], list_offsets.get(parameters_, 0))
    for field in pools.sorted_fields:
        tables += struct.pack("<HHI", pools.type_index[field.owner], pools.type_index[field.kind],
                              pools.string_index[field.name])
    for method in pools.sorted_methods:
        tables += struct.pack("<HHI", pools.type_index[method.owner],
                              pools.proto_index[(method.return_type, method.parameters)],
                              pools.string_index[method.name])
    tables += struct.pack("<IIIIIIII", pools.type_index[owner], ACC_PUBLIC, pools.type_index[ACTIVITY],
                          list_offsets[(CALLBACK,)], NO_INDEX, 0, class_data_offset, 0)

    total = data_off + len(data)
    header = bytearray(b"dex\n035\0" + bytes(24))
    header += struct.pack("<IIIII", total, header_size, 0x12345678, 0, 0)
    header += struct.pack("<I", map_offset)
    for count, offset in ((counts["string"], string_ids_off), (counts["type"], type_ids_off),
                          (counts["proto"], proto_ids_off), (counts["field"], field_ids_off),
                          (counts["method"], method_ids_off), (1, class_defs_off)):
        header += struct.pack("<II", count, offset)
    header += struct.pack("<II", len(data), data_off)
    assert len(header) == header_size, len(header)
    image = bytearray(header + tables + data)
    assert len(image) == total
    image[12:32] = hashlib.sha1(bytes(image[32:])).digest()
    image[8:12] = struct.pack("<I", zlib.adler32(bytes(image[12:])) & 0xFFFFFFFF)
    return bytes(image)


COMPONENT = PACKAGE + "/.Probe"


def build_probe_apk(destination: pathlib.Path) -> pathlib.Path:
    """Writes the signed probe package to `destination`."""
    import orientation_probe_apk as binary_xml

    attributes = dict(binary_xml.ATTRIBUTES)
    attributes["configChanges"] = 0x0101001F
    attributes = dict(sorted(attributes.items(), key=lambda item: item[1]))
    Element = binary_xml.Element
    tree = Element("manifest", (("package", PACKAGE), ("versionCode", 1)), (
        Element("uses-sdk", (("minSdkVersion", 29), ("targetSdkVersion", 29))),
        Element("uses-permission", (("name", "android.permission.CAMERA"),)),
        Element("application", (("label", "Camera Probe"), ("debuggable", True)), (
            Element("activity", (("name", CLASS_NAME), ("exported", True), ("configChanges", 0xFA0))),
        )),
    ))
    return binary_xml.build_apk(destination, None, tree, {"classes.dex": build_dex()}, attributes=attributes)
