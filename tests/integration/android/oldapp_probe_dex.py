# SPDX-License-Identifier: GPL-2.0-or-later
"""The code of the old-application probe, assembled by hand into a DEX file.

The class is the Java side of oldapp_probe_apk.py (the native side is oldapp/oldapp_probe.cpp). It is
written as DEX because the host has no Android SDK; property_probe_dex.py's assembler and tables do
the work and this module only lays out a class with several methods. The Java it stands for:

    public class Probe extends BroadcastReceiver implements Runnable {
        public void onReceive(Context context, Intent intent) {
            String shim = intent.getStringExtra("shim");
            if (shim != null) System.loadLibrary(shim);       // only when the test loads the layer itself
            System.loadLibrary("koldprobe");
            setResultData(run(intent.getStringExtra("opts")));   // `am broadcast` prints the result data
        }
        public static native String run(String options);
        public static byte[] payload() { return PAYLOAD.getBytes(); }
        public static int echo(byte[] data) { return data.length; }
        public static int spin(int n) { int i = 0; while (i < n) { empty(); i++; } return i; }
        public static byte[] asset(int n, int v) { byte[] a = new byte[n]; Arrays.fill(a, (byte) v); return a; }
        public static int peek(byte[] a, int i) { return a[i]; }
        public static void poke(byte[] a, int i, int v) { a[i] = (byte) v; }
        public static byte[] same(byte[] a) { return a; }
        public static void collect() { Runtime.getRuntime().gc(); }
        public static int scene(int frames) {
            int ok = sceneInit();
            for (int i = 0; i < frames; i++) ok &= sceneFrame(i);
            return ok;
        }
        public static void render() {          // the render thread: a java.lang.Thread that Java starts
            Thread thread = new Thread(new Probe());
            thread.start();
            thread.join();
        }
        public void run() { sceneDone(scene(sceneFrames())); }
        public static native int tick(), tickArm(), tickReg(), tickRegArm(), recurse(int depth), abi0();
        public static native int sceneInit(), sceneFrame(int frame), sceneFrames();
        public static native void sceneDone(int status);
        public static native void throws_(), empty();
        ... the argument and result types the thunks must carry through (NATIVES below)
    }

`payload` is the byte[] the native code reads the way a Dalvik-era application does; `echo` is what it
passes the array back to. The other native methods are what the native code calls to see the end of a native
method (blocks that end when it returns) and that the thunks pass every argument and result through; `spin` is the
loop that times a native call. `asset` is what an application's asset reader does (a fresh array of a given length
for every call), `scene` the loop of its render thread: one native init method, then one native method per frame
(`render` starts that thread, a java.lang.Thread whose Runnable is the probe class, and waits for it);
`same` returns its argument (an array Java holds, obtained again), `poke` writes a byte, `collect` is Runtime.gc().
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import struct
import zlib

import property_probe_dex as base

CLASS_NAME = "org.example.andarium.oldappprobe.Probe"
PAYLOAD = "OLD-APP-PAYLOAD-0123456789abcdef"
SYSTEM = "Ljava/lang/System;"
ARRAYS = "Ljava/util/Arrays;"
RUNTIME = "Ljava/lang/Runtime;"
BYTES = "[B"
ACC_STATIC = 0x8
ACC_NATIVE = 0x100
RUNNABLE = "Ljava/lang/Runnable;"
THREAD = "Ljava/lang/Thread;"
ABI12 = ("I", "J", "D", "F", "I", "J", "D", "F", "I", "I", "I", "I")  # 64-bit arguments past the registers, aligned
#: (name, return type, parameters) of the static native methods besides `run`. tick/tickArm are bound by name (Thumb and
#: ARM functions of the library), tickReg/tickRegArm and abi12Arm by RegisterNatives.
NATIVES = (
    ("tick", "I", ()), ("tickArm", "I", ()), ("tickReg", "I", ()), ("tickRegArm", "I", ()),
    ("recurse", "I", ("I",)), ("throwing", "V", ()), ("empty", "V", ()),
    ("abi0", "I", ()), ("abi12", "J", ABI12), ("abi12Arm", "J", ABI12),
    ("retZ", "Z", ("Z",)), ("retB", "B", ("B",)), ("retC", "C", ("C",)), ("retS", "S", ("S",)), ("retI", "I", ("I",)),
    ("retJ", "J", ("J",)), ("retF", "F", ("F",)), ("retD", "D", ("D",)), ("retL", base.STRING, (base.STRING,)),
    ("retV", "V", ("I",)), ("readV", "I", ()),
    ("sceneInit", "I", ()), ("sceneFrame", "I", ("I",)), ("sceneFrames", "I", ()), ("sceneDone", "V", ("I",)),
)


def build_dex() -> bytes:
    """The DEX file with the one class."""
    pools = base.Pools()
    owner = pools.type(base.descriptor(CLASS_NAME))
    pools.type(base.RECEIVER)
    init = pools.method(owner, "<init>", "V")
    on_receive = pools.method(owner, "onReceive", "V", (base.CONTEXT, base.INTENT))
    run = pools.method(owner, "run", base.STRING, (base.STRING,))
    payload = pools.method(owner, "payload", BYTES)
    echo = pools.method(owner, "echo", "I", (BYTES,))
    receiver_init = pools.method(base.RECEIVER, "<init>", "V")
    get_extra = pools.method(base.INTENT, "getStringExtra", base.STRING, (base.STRING,))
    load_library = pools.method(SYSTEM, "loadLibrary", "V", (base.STRING,))
    set_result = pools.method(base.RECEIVER, "setResultData", "V", (base.STRING,))
    get_bytes = pools.method(base.STRING, "getBytes", BYTES)
    natives = {name: pools.method(owner, name, result, parameters) for name, result, parameters in NATIVES}
    spin = pools.method(owner, "spin", "I", ("I",))
    asset = pools.method(owner, "asset", BYTES, ("I", "I"))
    peek = pools.method(owner, "peek", "I", (BYTES, "I"))
    scene = pools.method(owner, "scene", "I", ("I",))
    render = pools.method(owner, "render", "V")
    run_thread = pools.method(owner, "run", "V")  # Runnable.run
    thread_init = pools.method(THREAD, "<init>", "V", (RUNNABLE,))
    thread_start = pools.method(THREAD, "start", "V")
    thread_join = pools.method(THREAD, "join", "V")
    fill = pools.method(ARRAYS, "fill", "V", (BYTES, "B"))
    poke = pools.method(owner, "poke", "V", (BYTES, "I", "I"))
    same = pools.method(owner, "same", BYTES, (BYTES,))
    collect = pools.method(owner, "collect", "V")
    get_runtime = pools.method(RUNTIME, "getRuntime", RUNTIME)
    gc = pools.method(RUNTIME, "gc", "V")
    for item in ("V", "I", BYTES, "Z", "B", "C", "S", "J", "F", "D", RUNNABLE, THREAD):
        pools.type(item)
    for text in ("shim", "koldprobe", "opts", PAYLOAD):
        pools.string(text)

    constructor = base.Code(registers=1, ins=1, outs=1)
    constructor.op("invoke-direct", receiver_init, 0)
    constructor.op("return-void")

    # v0 shim or temporary, v1 temporary; v4 this, v5 context, v6 intent.
    receive = base.Code(registers=7, ins=3, outs=2)
    receive.op("const-string", 0, "shim")
    receive.op("invoke-virtual", get_extra, 6, 0)
    receive.op("move-result-object", 0)
    receive.op("if-eqz", 0, "own")
    receive.op("invoke-static", load_library, 0)
    receive.label("own")
    receive.op("const-string", 1, "koldprobe")
    receive.op("invoke-static", load_library, 1)
    receive.op("const-string", 1, "opts")
    receive.op("invoke-virtual", get_extra, 6, 1)
    receive.op("move-result-object", 1)
    receive.op("invoke-static", run, 1)
    receive.op("move-result-object", 1)
    receive.op("invoke-virtual", set_result, 4, 1)
    receive.op("return-void")

    payload_body = base.Code(registers=1, ins=0, outs=1)
    payload_body.op("const-string", 0, PAYLOAD)
    payload_body.op("invoke-virtual", get_bytes, 0)
    payload_body.op("move-result-object", 0)
    payload_body.op("return-object", 0)

    echo_body = base.Code(registers=2, ins=1, outs=0)  # v1 is the argument
    echo_body.op("array-length", 0, 1)
    echo_body.op("return", 0)

    # v0 the counter, v2 the argument n: the loop that calls the empty native method n times.
    spin_body = base.Code(registers=3, ins=1, outs=0)
    spin_body.op("const/4", 0, 0)
    spin_body.label("loop")
    spin_body.op("if-ge", 0, 2, "done")
    spin_body.op("invoke-static", natives["empty"])
    spin_body.op("add-int/lit8", 0, 0, 1)
    spin_body.op("goto/16", "loop")
    spin_body.label("done")
    spin_body.op("return", 0)

    # v0 the array, v1 its length n, v2 the value v: a fresh array of n bytes, every byte v.
    asset_body = base.Code(registers=3, ins=2, outs=2)
    asset_body.op("new-array", 0, 1, BYTES)
    asset_body.op("int-to-byte", 2, 2)
    asset_body.op("invoke-static", fill, 0, 2)
    asset_body.op("return-object", 0)

    peek_body = base.Code(registers=3, ins=2, outs=0)  # v1 the array, v2 the index
    peek_body.op("aget-byte", 0, 1, 2)
    peek_body.op("return", 0)

    poke_body = base.Code(registers=4, ins=3, outs=0)  # v1 the array, v2 the index, v3 the value
    poke_body.op("int-to-byte", 3, 3)
    poke_body.op("aput-byte", 3, 1, 2)
    poke_body.op("return-void")

    same_body = base.Code(registers=2, ins=1, outs=0)  # v1 the array
    same_body.op("return-object", 1)

    collect_body = base.Code(registers=1, ins=0, outs=1)
    collect_body.op("invoke-static", get_runtime)
    collect_body.op("move-result-object", 0)
    collect_body.op("invoke-virtual", gc, 0)
    collect_body.op("return-void")

    # v0 the status, v1 the frame, v2 a temporary, v3 the number of frames: the render thread's loop.
    scene_body = base.Code(registers=4, ins=1, outs=1)
    scene_body.op("invoke-static", natives["sceneInit"])
    scene_body.op("move-result", 0)
    scene_body.op("const/4", 1, 0)
    scene_body.label("loop")
    scene_body.op("if-ge", 1, 3, "done")
    scene_body.op("invoke-static", natives["sceneFrame"], 1)
    scene_body.op("move-result", 2)
    scene_body.op("and-int/2addr", 0, 2)
    scene_body.op("add-int/lit8", 1, 1, 1)
    scene_body.op("goto/16", "loop")
    scene_body.label("done")
    scene_body.op("return", 0)

    # v0 a temporary, v1 the Thread, v2 the new probe object: render() starts the thread and waits for it.
    render_body = base.Code(registers=3, ins=0, outs=2)
    render_body.op("new-instance", 1, THREAD)
    render_body.op("new-instance", 2, owner)
    render_body.op("invoke-direct", init, 2)
    render_body.op("invoke-direct", thread_init, 1, 2)
    render_body.op("invoke-virtual", thread_start, 1)
    render_body.op("invoke-virtual", thread_join, 1)
    render_body.op("return-void")

    # v0 a temporary, v1 this: the Runnable of the render thread.
    run_body = base.Code(registers=2, ins=1, outs=1)
    run_body.op("invoke-static", natives["sceneFrames"])
    run_body.op("move-result", 0)
    run_body.op("invoke-static", scene, 0)
    run_body.op("move-result", 0)
    run_body.op("invoke-static", natives["sceneDone"], 0)
    run_body.op("return-void")

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
    sections: list[tuple[int, int, int]] = []

    def align(boundary: int) -> None:
        while (data_off + len(data)) % boundary:
            data.append(0)

    list_offsets: dict[tuple[str, ...], int] = {}
    align(4)
    first = data_off + len(data)
    for _, parameters in pools.sorted_protos:
        if parameters and parameters not in list_offsets:
            align(4)
            list_offsets[parameters] = data_off + len(data)
            data += struct.pack("<I", len(parameters)) + b"".join(
                struct.pack("<H", pools.type_id(item)) for item in parameters)
    interfaces = (RUNNABLE,)  # the class implements Runnable: the render thread's body
    if interfaces not in list_offsets:
        align(4)
        list_offsets[interfaces] = data_off + len(data)
        data += struct.pack("<I", len(interfaces)) + b"".join(struct.pack("<H", pools.type_id(item)) for item in interfaces)
    sections.append((base.TYPE_TYPE_LIST, len(list_offsets), first))

    bodies = {init: constructor, on_receive: receive, payload: payload_body, echo: echo_body, spin: spin_body,
              asset: asset_body, peek: peek_body, scene: scene_body, poke: poke_body, same: same_body,
              collect: collect_body, render: render_body, run_thread: run_body}
    code_offsets: dict[object, int] = {}
    first_code = None
    for method, body in bodies.items():
        align(4)
        code_offsets[method] = data_off + len(data)
        first_code = first_code if first_code is not None else code_offsets[method]
        data += base.assemble(body, pools)
    sections.append((base.TYPE_CODE, len(bodies), first_code))

    string_offsets = []
    string_first = data_off + len(data)
    for text in pools.sorted_strings:
        string_offsets.append(data_off + len(data))
        data += base.uleb128(len(text)) + base.mutf8(text) + b"\0"
    sections.append((base.TYPE_STRING_DATA, string_count, string_first))

    # Direct methods (constructor and the statics) and virtual methods, each in method-index order with
    # the index written as the difference from the one before.
    direct = sorted(
        ((pools.method_id(init), base.ACC_PUBLIC | base.ACC_CONSTRUCTOR, code_offsets[init]),
         (pools.method_id(run), base.ACC_PUBLIC | ACC_STATIC | ACC_NATIVE, 0),
         (pools.method_id(payload), base.ACC_PUBLIC | ACC_STATIC, code_offsets[payload]),
         (pools.method_id(echo), base.ACC_PUBLIC | ACC_STATIC, code_offsets[echo]),
         (pools.method_id(spin), base.ACC_PUBLIC | ACC_STATIC, code_offsets[spin]),
         (pools.method_id(asset), base.ACC_PUBLIC | ACC_STATIC, code_offsets[asset]),
         (pools.method_id(peek), base.ACC_PUBLIC | ACC_STATIC, code_offsets[peek]),
         (pools.method_id(scene), base.ACC_PUBLIC | ACC_STATIC, code_offsets[scene]),
         (pools.method_id(poke), base.ACC_PUBLIC | ACC_STATIC, code_offsets[poke]),
         (pools.method_id(same), base.ACC_PUBLIC | ACC_STATIC, code_offsets[same]),
         (pools.method_id(collect), base.ACC_PUBLIC | ACC_STATIC, code_offsets[collect]),
         (pools.method_id(render), base.ACC_PUBLIC | ACC_STATIC, code_offsets[render]),
         *((pools.method_id(method), base.ACC_PUBLIC | ACC_STATIC | ACC_NATIVE, 0) for method in natives.values())))
    virtual = sorted([(pools.method_id(on_receive), base.ACC_PUBLIC, code_offsets[on_receive]),
                      (pools.method_id(run_thread), base.ACC_PUBLIC, code_offsets[run_thread])])
    class_data_offset = data_off + len(data)
    data += base.uleb128(0) + base.uleb128(0) + base.uleb128(len(direct)) + base.uleb128(len(virtual))
    for group in (direct, virtual):
        previous = 0
        for index, access, code in group:
            data += base.uleb128(index - previous) + base.uleb128(access) + base.uleb128(code)
            previous = index
    sections.append((base.TYPE_CLASS_DATA, 1, class_data_offset))

    align(4)
    map_offset = data_off + len(data)
    entries = [(base.TYPE_HEADER, 1, 0), (base.TYPE_STRING_ID, string_count, string_ids_off),
               (base.TYPE_TYPE_ID, type_count, type_ids_off), (base.TYPE_PROTO_ID, proto_count, proto_ids_off),
               (base.TYPE_METHOD_ID, method_count, method_ids_off), (base.TYPE_CLASS_DEF, 1, class_defs_off),
               *sections, (base.TYPE_MAP_LIST, 1, map_offset)]
    entries.sort(key=lambda entry: entry[2])
    data += struct.pack("<I", len(entries)) + b"".join(
        struct.pack("<HHII", kind, 0, count, offset) for kind, count, offset in entries)

    tables = bytearray()
    for offset in string_offsets:
        tables += struct.pack("<I", offset)
    for item in pools.sorted_types:
        tables += struct.pack("<I", pools.string_id(item))
    for return_type, parameters in pools.sorted_protos:
        tables += struct.pack("<III", pools.string_id(base.shorty(return_type, parameters)),
                              pools.type_id(return_type), list_offsets.get(parameters, 0))
    for method in pools.sorted_methods:
        tables += struct.pack("<HHI", pools.type_id(method.owner),
                              pools.index["proto"][(method.return_type, method.parameters)],
                              pools.string_id(method.name))
    tables += struct.pack("<IIIIIIII", pools.type_id(owner), base.ACC_PUBLIC, pools.type_id(base.RECEIVER),
                          list_offsets[interfaces], base.NO_INDEX, 0, class_data_offset, 0)

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
    image[8:12] = struct.pack("<I", zlib.adler32(bytes(image[12:])) & 0xFFFFFFFF)
    return bytes(image)
