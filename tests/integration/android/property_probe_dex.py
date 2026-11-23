# SPDX-License-Identifier: GPL-2.0-or-later
"""The code of the property probe application, assembled by hand into a DEX file.

An application's code runs in the SELinux domain of an ordinary application only when the
framework starts it: `adb shell run-as` runs in `runas_app`, a domain of its own. The probe is a
broadcast receiver, so `am broadcast -n` makes the zygote fork a normal application process that
runs its `onReceive`. Android's tools (d8, javac, the SDK) are not available to the host, so this
module writes the one class as a DEX file (version 035), which every release since 14 loads from
an APK, and the runner's tooling test reads the layout back with an independent reader.

The class is Java of this shape (the order of the instructions below):

    public class Probe extends BroadcastReceiver {
        public void onReceive(Context context, Intent intent) {
            StringBuilder links = new StringBuilder();
            for (Enumeration e = NetworkInterface.getNetworkInterfaces(); e.hasMoreElements();) {
                NetworkInterface n = (NetworkInterface) e.nextElement();
                boolean found = false;
                for (Enumeration a = n.getInetAddresses(); a.hasMoreElements();) {
                    Object address = a.nextElement();
                    if (address instanceof Inet6Address && ((Inet6Address) address).isLinkLocalAddress())
                        found = true;
                }
                links.append(n.getName()).append(found ? "=yes " : "=no ");
            }
            Runtime.getRuntime().exec(new String[] {"/system/bin/sh", "-c", SCRIPT, "sh", links.toString()}).waitFor();
        }
    }

The process the receiver starts keeps the application's domain and uid, so the script it runs reads
the properties and writes its result file as the application. The interface listing is the one an
application can call (`NetworkInterface`), and the result names an interface and whether the
listing holds an IPv6 link-local address for it: never an address.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import struct
import zlib

CLASS_NAME = "org.example.andarium.propertyprobe.Probe"
OBJECT = "Ljava/lang/Object;"
STRING = "Ljava/lang/String;"
STRING_ARRAY = "[Ljava/lang/String;"
STRING_BUILDER = "Ljava/lang/StringBuilder;"
PROCESS = "Ljava/lang/Process;"
RUNTIME = "Ljava/lang/Runtime;"
ENUMERATION = "Ljava/util/Enumeration;"
NETWORK_INTERFACE = "Ljava/net/NetworkInterface;"
INET_ADDRESS = "Ljava/net/InetAddress;"
INET6_ADDRESS = "Ljava/net/Inet6Address;"
RECEIVER = "Landroid/content/BroadcastReceiver;"
CONTEXT = "Landroid/content/Context;"
INTENT = "Landroid/content/Intent;"


def descriptor(class_name: str) -> str:
    return "L" + class_name.replace(".", "/") + ";"


NO_INDEX = 0xFFFFFFFF
ACC_PUBLIC = 0x1
ACC_CONSTRUCTOR = 0x10000
TYPE_HEADER, TYPE_STRING_ID, TYPE_TYPE_ID, TYPE_PROTO_ID = 0x0000, 0x0001, 0x0002, 0x0003
TYPE_METHOD_ID, TYPE_CLASS_DEF = 0x0005, 0x0006
TYPE_MAP_LIST, TYPE_TYPE_LIST, TYPE_CLASS_DATA, TYPE_CODE, TYPE_STRING_DATA = 0x1000, 0x1001, 0x2000, 0x2001, 0x2002


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
    """Modified UTF-8: the NUL character and supplementary characters are special; this probe's strings use
    neither, so plain UTF-8 is the encoding."""
    if "\0" in text or any(ord(char) > 0xFFFF for char in text):
        raise ValueError("a string with a NUL or a supplementary character")
    return text.encode("utf-8")


def shorty(return_type: str, parameters: tuple[str, ...]) -> str:
    return "".join(("L" if item[0] in "L[" else item) for item in (return_type, *parameters))


class Method:
    def __init__(self, owner: str, name: str, return_type: str, parameters: tuple[str, ...] = ()) -> None:
        self.owner, self.name, self.return_type, self.parameters = owner, name, return_type, parameters

    def key(self) -> tuple:
        return (self.owner, self.name, self.return_type, self.parameters)


class Pools:
    """The string, type, prototype and method tables, indexed in the order the format requires."""

    def __init__(self) -> None:
        self.strings: set[str] = set()
        self.types: set[str] = set()
        self.protos: set[tuple[str, tuple[str, ...]]] = set()
        self.methods: dict[tuple, Method] = {}
        self.index: dict[str, dict] = {}

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
        self.type(owner)
        self.type(return_type)
        for item in parameters:
            self.type(item)
        self.protos.add((return_type, parameters))
        self.strings.update((name, shorty(return_type, parameters)))
        return method

    def finish(self) -> None:
        strings = sorted(self.strings, key=mutf8)
        string_index = {text: number for number, text in enumerate(strings)}
        types = sorted(self.types, key=lambda name: string_index[name])
        type_index = {name: number for number, name in enumerate(types)}
        protos = sorted(self.protos, key=lambda item: (type_index[item[0]], [type_index[name] for name in item[1]]))
        proto_index = {item: number for number, item in enumerate(protos)}
        methods = sorted(
            self.methods.values(),
            key=lambda m: (type_index[m.owner], string_index[m.name], proto_index[(m.return_type, m.parameters)]))
        method_index = {m.key(): number for number, m in enumerate(methods)}
        self.index = {"string": string_index, "type": type_index, "proto": proto_index, "method": method_index}
        self.sorted_strings, self.sorted_types, self.sorted_protos, self.sorted_methods = strings, types, protos, methods

    def string_id(self, text: str) -> int:
        return self.index["string"][text]

    def type_id(self, name: str) -> int:
        return self.index["type"][name]

    def method_id(self, method: Method) -> int:
        return self.index["method"][method.key()]


class Code:
    """A method body: instructions with symbolic operands, resolved when the pools are final."""

    def __init__(self, registers: int, ins: int, outs: int) -> None:
        self.registers, self.ins, self.outs = registers, ins, outs
        self.items: list[tuple] = []

    def label(self, name: str) -> None:
        self.items.append(("label", name))

    def op(self, mnemonic: str, *operands: object) -> None:
        self.items.append((mnemonic, *operands))


# mnemonic -> (opcode, format). Formats: 10x 11n 11x 12x 21c 21t 22t 22b 20t 22c 23x 35c.
OPCODES = {
    "return-void": (0x0E, "10x"),
    "return": (0x0F, "11x"),
    "return-object": (0x11, "11x"),
    "array-length": (0x21, "12x"),
    "const/4": (0x12, "11n"),
    "const-string": (0x1A, "21c-string"),
    "new-instance": (0x22, "21c-type"),
    "check-cast": (0x1F, "21c-type"),
    "instance-of": (0x20, "22c-type"),
    "new-array": (0x23, "22c-type"),
    "move-result": (0x0A, "11x"),
    "move-result-object": (0x0C, "11x"),
    "if-eqz": (0x38, "21t"),
    "if-ge": (0x35, "22t"),
    "add-int/lit8": (0xD8, "22b"),
    "and-int/2addr": (0xB5, "12x"),
    "int-to-byte": (0x8D, "12x"),
    "aget-byte": (0x48, "23x"),
    "aput-byte": (0x4F, "23x"),
    "goto/16": (0x29, "20t"),
    "aput-object": (0x4D, "23x"),
    "invoke-virtual": (0x6E, "35c"),
    "invoke-direct": (0x70, "35c"),
    "invoke-static": (0x71, "35c"),
    "invoke-interface": (0x72, "35c"),
}
SIZES = {"10x": 1, "11n": 1, "11x": 1, "12x": 1, "21c-string": 2, "21c-type": 2, "22c-type": 2, "21t": 2, "22t": 2,
         "22b": 2, "20t": 2, "23x": 2, "35c": 3}


def assemble(code: Code, pools: Pools) -> bytes:
    """The code_item of the body: its header and its instructions."""
    positions: dict[str, int] = {}
    offset = 0
    for item in code.items:
        if item[0] == "label":
            positions[item[1]] = offset
        else:
            offset += SIZES[OPCODES[item[0]][1]]
    units: list[int] = []
    offset = 0
    for item in code.items:
        if item[0] == "label":
            continue
        opcode, form = OPCODES[item[0]]
        operands = item[1:]
        if form == "10x":
            units.append(opcode)
        elif form == "11n":
            register, literal = operands
            units.append((literal & 0xF) << 12 | register << 8 | opcode)
        elif form == "11x":
            units.append(operands[0] << 8 | opcode)
        elif form == "12x":
            units.append(operands[1] << 12 | operands[0] << 8 | opcode)
        elif form == "21c-string":
            units += [operands[0] << 8 | opcode, pools.string_id(operands[1])]
        elif form == "21c-type":
            units += [operands[0] << 8 | opcode, pools.type_id(operands[1])]
        elif form == "22c-type":
            units += [operands[1] << 12 | operands[0] << 8 | opcode, pools.type_id(operands[2])]
        elif form == "21t":
            units += [operands[0] << 8 | opcode, (positions[operands[1]] - offset) & 0xFFFF]
        elif form == "22t":
            units += [operands[1] << 12 | operands[0] << 8 | opcode, (positions[operands[2]] - offset) & 0xFFFF]
        elif form == "22b":
            units += [operands[0] << 8 | opcode, (operands[2] & 0xFF) << 8 | operands[1]]
        elif form == "20t":
            units += [opcode, (positions[operands[0]] - offset) & 0xFFFF]
        elif form == "23x":
            units += [operands[0] << 8 | opcode, operands[2] << 8 | operands[1]]
        elif form == "35c":
            method, registers = operands[0], list(operands[1:])
            padded = registers + [0] * (5 - len(registers))
            units += [len(registers) << 12 | padded[4] << 8 | opcode, pools.method_id(method),
                      padded[3] << 12 | padded[2] << 8 | padded[1] << 4 | padded[0]]
        else:
            raise ValueError(form)
        offset += SIZES[form]
    body = struct.pack(f"<{len(units)}H", *units)
    header = struct.pack("<HHHHII", code.registers, code.ins, code.outs, 0, 0, len(units))
    return header + body


def script(package: str, control: str, key: str) -> str:
    """What the shell the receiver starts runs, as the application. $1 is the interface summary.
    The key's value is never written: only whether the property read as non-empty."""
    directory = f"/data/data/{package}/files"
    return (
        f"d={directory}; mkdir -p $d; "
        "{ "
        'echo "context=$(id -Z)"; '
        'echo "links=$1"; '
        f'[ -n "$(getprop {control})" ] && echo control=readable || echo control=empty; '
        f'[ -n "$(getprop {key})" ] && echo key=readable || echo key=empty; '
        "echo done; "
        "} > $d/result.tmp && mv $d/result.tmp $d/result")


def build_dex(package: str, control: str, key: str, shell_script: str | None = None) -> bytes:
    """The DEX file of the probe class for an application of `package`. `shell_script` replaces the script
    the receiver's shell runs (the default is `script`; $1 is the interface summary either way)."""
    payload = shell_script if shell_script is not None else script(package, control, key)
    pools = Pools()
    owner = pools.type(descriptor(CLASS_NAME))
    pools.type(RECEIVER)
    init = pools.method(owner, "<init>", "V")
    on_receive = pools.method(owner, "onReceive", "V", (CONTEXT, INTENT))
    receiver_init = pools.method(RECEIVER, "<init>", "V")
    builder_init = pools.method(STRING_BUILDER, "<init>", "V")
    append = pools.method(STRING_BUILDER, "append", STRING_BUILDER, (STRING,))
    to_string = pools.method(STRING_BUILDER, "toString", STRING)
    interfaces = pools.method(NETWORK_INTERFACE, "getNetworkInterfaces", ENUMERATION)
    addresses = pools.method(NETWORK_INTERFACE, "getInetAddresses", ENUMERATION)
    name = pools.method(NETWORK_INTERFACE, "getName", STRING)
    has_more = pools.method(ENUMERATION, "hasMoreElements", "Z")
    next_element = pools.method(ENUMERATION, "nextElement", OBJECT)
    link_local = pools.method(INET_ADDRESS, "isLinkLocalAddress", "Z")
    get_runtime = pools.method(RUNTIME, "getRuntime", RUNTIME)
    execute = pools.method(RUNTIME, "exec", PROCESS, (STRING_ARRAY,))
    wait_for = pools.method(PROCESS, "waitFor", "I")
    for item in (INET6_ADDRESS, STRING_ARRAY, "V", "Z", "I"):
        pools.type(item)
    for text in ("=yes ", "=no ", "/system/bin/sh", "-c", "sh", payload):
        pools.string(text)

    constructor = Code(registers=1, ins=1, outs=1)
    constructor.op("invoke-direct", receiver_init, 0)
    constructor.op("return-void")

    # v0 builder, v1 enumeration of interfaces (later the argument array), v2 flag, v3 interface,
    # v4 enumeration of addresses, v5 found, v6 temporary; v8 this, v9 context, v10 intent.
    body = Code(registers=11, ins=3, outs=2)
    body.op("new-instance", 0, STRING_BUILDER)
    body.op("invoke-direct", builder_init, 0)
    body.op("invoke-static", interfaces)
    body.op("move-result-object", 1)
    body.label("outer")
    body.op("invoke-interface", has_more, 1)
    body.op("move-result", 2)
    body.op("if-eqz", 2, "done")
    body.op("invoke-interface", next_element, 1)
    body.op("move-result-object", 3)
    body.op("check-cast", 3, NETWORK_INTERFACE)
    body.op("invoke-virtual", addresses, 3)
    body.op("move-result-object", 4)
    body.op("const/4", 5, 0)
    body.label("inner")
    body.op("invoke-interface", has_more, 4)
    body.op("move-result", 2)
    body.op("if-eqz", 2, "after")
    body.op("invoke-interface", next_element, 4)
    body.op("move-result-object", 6)
    body.op("instance-of", 2, 6, INET6_ADDRESS)
    body.op("if-eqz", 2, "inner")
    body.op("check-cast", 6, INET6_ADDRESS)
    body.op("invoke-virtual", link_local, 6)
    body.op("move-result", 2)
    body.op("if-eqz", 2, "inner")
    body.op("const/4", 5, 1)
    body.op("goto/16", "inner")
    body.label("after")
    body.op("invoke-virtual", name, 3)
    body.op("move-result-object", 6)
    body.op("invoke-virtual", append, 0, 6)
    body.op("if-eqz", 5, "no")
    body.op("const-string", 6, "=yes ")
    body.op("goto/16", "append")
    body.label("no")
    body.op("const-string", 6, "=no ")
    body.label("append")
    body.op("invoke-virtual", append, 0, 6)
    body.op("goto/16", "outer")
    body.label("done")
    body.op("invoke-virtual", to_string, 0)
    body.op("move-result-object", 6)
    body.op("const/4", 0, 5)
    body.op("new-array", 1, 0, STRING_ARRAY)
    for number, text in enumerate(("/system/bin/sh", "-c", payload, "sh")):
        body.op("const/4", 0, number)
        body.op("const-string", 2, text)
        body.op("aput-object", 2, 1, 0)
    body.op("const/4", 0, 4)
    body.op("aput-object", 6, 1, 0)
    body.op("invoke-static", get_runtime)
    body.op("move-result-object", 2)
    body.op("invoke-virtual", execute, 2, 1)
    body.op("move-result-object", 2)
    body.op("invoke-virtual", wait_for, 2)
    body.op("return-void")

    pools.finish()
    string_count, type_count = len(pools.sorted_strings), len(pools.sorted_types)
    proto_count, method_count = len(pools.sorted_protos), len(pools.sorted_methods)

    header_size = 0x70
    string_ids_off = header_size
    type_ids_off = string_ids_off + 4 * string_count
    proto_ids_off = type_ids_off + 4 * type_count
    method_ids_off = proto_ids_off + 12 * proto_count
    class_defs_off = method_ids_off + 8 * method_count
    data_off = class_defs_off + 32

    data = bytearray()
    sections: list[tuple[int, int, int]] = []  # (type, count, offset) of the data items

    def align(boundary: int) -> None:
        while (data_off + len(data)) % boundary:
            data.append(0)

    # type lists (parameters of the prototypes)
    list_offsets: dict[tuple[str, ...], int] = {}
    align(4)
    first = data_off + len(data)
    for _, parameters in pools.sorted_protos:
        if parameters and parameters not in list_offsets:
            align(4)
            list_offsets[parameters] = data_off + len(data)
            data += struct.pack("<I", len(parameters)) + b"".join(struct.pack("<H", pools.type_id(item)) for item in parameters)
    if list_offsets:
        sections.append((TYPE_TYPE_LIST, len(list_offsets), first))

    # code items
    align(4)
    constructor_offset = data_off + len(data)
    data += assemble(constructor, pools)
    align(4)
    body_offset = data_off + len(data)
    data += assemble(body, pools)
    sections.append((TYPE_CODE, 2, constructor_offset))

    # string data
    string_offsets = []
    string_first = data_off + len(data)
    for text in pools.sorted_strings:
        string_offsets.append(data_off + len(data))
        data += uleb128(len(text)) + mutf8(text) + b"\0"
    sections.append((TYPE_STRING_DATA, string_count, string_first))

    # class data: the constructor is direct, onReceive virtual
    class_data_offset = data_off + len(data)
    init_id, receive_id = pools.method_id(init), pools.method_id(on_receive)
    data += uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1)
    data += uleb128(init_id) + uleb128(ACC_PUBLIC | ACC_CONSTRUCTOR) + uleb128(constructor_offset)
    data += uleb128(receive_id) + uleb128(ACC_PUBLIC) + uleb128(body_offset)
    sections.append((TYPE_CLASS_DATA, 1, class_data_offset))

    align(4)
    map_offset = data_off + len(data)
    entries = [(TYPE_HEADER, 1, 0), (TYPE_STRING_ID, string_count, string_ids_off), (TYPE_TYPE_ID, type_count, type_ids_off),
               (TYPE_PROTO_ID, proto_count, proto_ids_off), (TYPE_METHOD_ID, method_count, method_ids_off),
               (TYPE_CLASS_DEF, 1, class_defs_off), *sections, (TYPE_MAP_LIST, 1, map_offset)]
    entries.sort(key=lambda entry: entry[2])
    data += struct.pack("<I", len(entries)) + b"".join(struct.pack("<HHII", kind, 0, count, offset) for kind, count, offset in entries)

    tables = bytearray()
    for offset in string_offsets:
        tables += struct.pack("<I", offset)
    for item in pools.sorted_types:
        tables += struct.pack("<I", pools.string_id(item))
    for return_type, parameters in pools.sorted_protos:
        tables += struct.pack("<III", pools.string_id(shorty(return_type, parameters)), pools.type_id(return_type),
                              list_offsets.get(parameters, 0))
    for method in pools.sorted_methods:
        tables += struct.pack("<HHI", pools.type_id(method.owner), pools.index["proto"][(method.return_type, method.parameters)],
                              pools.string_id(method.name))
    tables += struct.pack("<IIIIIIII", pools.type_id(owner), ACC_PUBLIC, pools.type_id(RECEIVER), 0, NO_INDEX, 0,
                          class_data_offset, 0)

    total = data_off + len(data)
    header = bytearray(b"dex\n035\0" + bytes(24))
    header += struct.pack("<IIIII", total, header_size, 0x12345678, 0, 0)
    header += struct.pack("<I", map_offset)
    for count, offset in ((string_count, string_ids_off), (type_count, type_ids_off), (proto_count, proto_ids_off),
                          (0, 0), (method_count, method_ids_off), (1, class_defs_off)):
        header += struct.pack("<II", count, offset)
    header += struct.pack("<II", len(data), data_off)
    assert len(header) == header_size, len(header)
    image = bytearray(header + tables + data)
    assert len(image) == total
    image[12:32] = hashlib.sha1(bytes(image[32:])).digest()
    # The checksum (offset 8) covers everything after itself, the signature included.
    image[8:12] = struct.pack("<I", zlib.adler32(bytes(image[12:])) & 0xFFFFFFFF)
    return bytes(image)
