#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-video-suspend: one V4L2 codec session per run.

    video_suspend_probe.py LABEL decode PACKET FOURCC [FLAGFILE]
    video_suspend_probe.py LABEL encode [FLAGFILE]
    video_suspend_probe.py LABEL decode-stream PACKET FOURCC FLAGFILE
    video_suspend_probe.py LABEL encode-stream FLAGFILE

A decode session opens the Virtio video decoder, queues one coded access
unit, waits for the source-change event, sets up the capture queue that event
describes and waits for one decoded picture. An encode session queues raw
frames until one coded buffer returns. Both then stop their queues and close
the node, which returns only if the device handed every buffer back.

With FLAGFILE the node is opened first, `VSUSP LABEL ready` is printed, and
the session starts once the file exists: a descriptor opened before a suspend
and used after it, as a VA-API driver's pool of nodes opened ahead of use is.

A stream session holds in the middle of its stream instead: it gets one
picture or coded buffer, returns to the state between two pictures (every
coded input back with the process, every output buffer queued with the
device, which holds them as requests), prints `ready`, and waits for the file.
After it, the next picture or coded buffer must arrive, and the queues stop
and the node closes.

Every line the probe prints starts with `VSUSP LABEL`, and the last one is
`ok` or `FAIL: reason`; the exit status says the same. Every wait is bounded.
"""

import ctypes as C
import errno
import fcntl
import glob
import mmap
import os
import select
import sys
import time

WAIT = 20.0
FLAG_WAIT = 300.0

label = "probe"


def say(*parts):
    print("VSUSP", label, *parts, flush=True)


class Failure(Exception):
    pass


def fourcc(text):
    return ord(text[0]) | ord(text[1]) << 8 | ord(text[2]) << 16 | ord(text[3]) << 24


def ioc(direction, number, size):
    return (direction << 30) | (size << 16) | (ord("V") << 8) | number


class PlanePix(C.Structure):
    _pack_ = 1
    _fields_ = [("sizeimage", C.c_uint32), ("bytesperline", C.c_uint32), ("reserved", C.c_uint16 * 6)]


class PixMp(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("width", C.c_uint32), ("height", C.c_uint32), ("pixelformat", C.c_uint32),
        ("field", C.c_uint32), ("colorspace", C.c_uint32), ("plane_fmt", PlanePix * 8),
        ("num_planes", C.c_uint8), ("flags", C.c_uint8), ("ycbcr_enc", C.c_uint8),
        ("quantization", C.c_uint8), ("xfer_func", C.c_uint8), ("reserved", C.c_uint8 * 7),
    ]


class FmtUnion(C.Union):
    _fields_ = [("pix_mp", PixMp), ("raw", C.c_uint8 * 200), ("align", C.c_uint64)]


class Format(C.Structure):
    _fields_ = [("type", C.c_uint32), ("fmt", FmtUnion)]


class Capability(C.Structure):
    _fields_ = [
        ("driver", C.c_char * 16), ("card", C.c_char * 32), ("bus_info", C.c_char * 32),
        ("version", C.c_uint32), ("capabilities", C.c_uint32), ("device_caps", C.c_uint32),
        ("reserved", C.c_uint32 * 3),
    ]


class RequestBuffers(C.Structure):
    _fields_ = [
        ("count", C.c_uint32), ("type", C.c_uint32), ("memory", C.c_uint32),
        ("capabilities", C.c_uint32), ("flags", C.c_uint8), ("reserved", C.c_uint8 * 3),
    ]


class Timeval(C.Structure):
    _fields_ = [("sec", C.c_long), ("usec", C.c_long)]


class Timecode(C.Structure):
    _fields_ = [
        ("type", C.c_uint32), ("flags", C.c_uint32), ("frames", C.c_uint8), ("seconds", C.c_uint8),
        ("minutes", C.c_uint8), ("hours", C.c_uint8), ("userbits", C.c_uint8 * 4),
    ]


class PlaneM(C.Union):
    _fields_ = [("mem_offset", C.c_uint32), ("userptr", C.c_ulong), ("fd", C.c_int32)]


class Plane(C.Structure):
    _fields_ = [
        ("bytesused", C.c_uint32), ("length", C.c_uint32), ("m", PlaneM),
        ("data_offset", C.c_uint32), ("reserved", C.c_uint32 * 11),
    ]


class BufM(C.Union):
    _fields_ = [("offset", C.c_uint32), ("userptr", C.c_ulong), ("planes", C.c_void_p), ("fd", C.c_int32)]


class Buffer(C.Structure):
    _fields_ = [
        ("index", C.c_uint32), ("type", C.c_uint32), ("bytesused", C.c_uint32), ("flags", C.c_uint32),
        ("field", C.c_uint32), ("timestamp", Timeval), ("timecode", Timecode), ("sequence", C.c_uint32),
        ("memory", C.c_uint32), ("m", BufM), ("length", C.c_uint32), ("reserved2", C.c_uint32),
        ("request_fd", C.c_int32),
    ]


class EventU(C.Union):
    _fields_ = [("data", C.c_uint8 * 64), ("align", C.c_int64)]


class Timespec(C.Structure):
    _fields_ = [("sec", C.c_long), ("nsec", C.c_long)]


class Event(C.Structure):
    _fields_ = [
        ("type", C.c_uint32), ("u", EventU), ("pending", C.c_uint32), ("sequence", C.c_uint32),
        ("timestamp", Timespec), ("id", C.c_uint32), ("reserved", C.c_uint32 * 8),
    ]


class Subscription(C.Structure):
    _fields_ = [("type", C.c_uint32), ("id", C.c_uint32), ("flags", C.c_uint32), ("reserved", C.c_uint32 * 5)]


for structure, size in ((PixMp, 192), (Format, 208), (Capability, 104), (RequestBuffers, 20),
                        (Plane, 64), (Buffer, 88), (Event, 136), (Subscription, 32)):
    assert C.sizeof(structure) == size, (structure.__name__, C.sizeof(structure))

QUERYCAP = ioc(2, 0, 104)
S_FMT = ioc(3, 5, 208)
G_FMT = ioc(3, 4, 208)
TRY_FMT = ioc(3, 64, 208)
REQBUFS = ioc(3, 8, 20)
QUERYBUF = ioc(3, 9, 88)
QBUF = ioc(3, 15, 88)
DQBUF = ioc(3, 17, 88)
STREAMON = ioc(1, 18, 4)
STREAMOFF = ioc(1, 19, 4)
DQEVENT = ioc(2, 89, 136)
SUBSCRIBE = ioc(1, 90, 32)

CAPTURE = 9
OUTPUT = 10
MMAP = 1
EVENT_EOS = 2
EVENT_SOURCE_CHANGE = 5
FLAG_ERROR = 0x40
NV12M = fourcc("NM12")


def find(card):
    for path in sorted(glob.glob("/dev/video*")):
        try:
            fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        except OSError:
            continue
        capability = Capability()
        try:
            fcntl.ioctl(fd, QUERYCAP, capability)
        except OSError:
            os.close(fd)
            continue
        if capability.card.decode() == card:
            say("opened", path, card)
            return fd
        os.close(fd)
    raise Failure(f"no {card} node")


def call(fd, request, value, name):
    try:
        fcntl.ioctl(fd, request, value)
    except OSError as error:
        raise Failure(f"{name}: {error}") from None


def try_call(fd, request, value):
    try:
        fcntl.ioctl(fd, request, value)
        return 0
    except OSError as error:
        return -error.errno


def stream(fd, request, queue):
    return try_call(fd, request, C.c_uint32(queue))


class Queue:
    def __init__(self, fd, queue, count):
        self.fd = fd
        self.queue = queue
        request = RequestBuffers(count=count, type=queue, memory=MMAP)
        call(fd, REQBUFS, request, "REQBUFS")
        self.maps = []
        for index in range(request.count):
            planes = (Plane * 8)()
            buffer = Buffer(index=index, type=queue, memory=MMAP, length=8)
            buffer.m.planes = C.addressof(planes)
            call(fd, QUERYBUF, buffer, "QUERYBUF")
            self.maps.append([
                mmap.mmap(fd, planes[i].length, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                          offset=planes[i].m.mem_offset)
                for i in range(buffer.length)
            ])
        say(f"queue {queue}: {len(self.maps)} buffers")

    def put(self, index, used=None, stamp=0):
        planes = (Plane * 8)()
        buffer = Buffer(index=index, type=self.queue, memory=MMAP, length=len(self.maps[index]))
        buffer.timestamp.sec = stamp
        for i, plane in enumerate(self.maps[index]):
            planes[i].length = len(plane)
            planes[i].bytesused = used[i] if used else 0
        buffer.m.planes = C.addressof(planes)
        return try_call(self.fd, QBUF, buffer)

    def take(self):
        planes = (Plane * 8)()
        buffer = Buffer(type=self.queue, memory=MMAP, length=8)
        buffer.m.planes = C.addressof(planes)
        result = try_call(self.fd, DQBUF, buffer)
        if result:
            return result, None, None
        return 0, buffer, [planes[i].bytesused for i in range(buffer.length)]


def events(fd):
    found = []
    while True:
        event = Event()
        if try_call(fd, DQEVENT, event):
            return found
        found.append(event.type)


def wait(fd, seconds, mask=select.POLLIN | select.POLLOUT | select.POLLPRI):
    poller = select.poll()
    poller.register(fd, mask)
    return poller.poll(int(seconds * 1000))


def subscribe(fd, kind):
    call(fd, SUBSCRIBE, Subscription(type=kind), "SUBSCRIBE")


def set_format(fd, queue, pixelformat, width=0, height=0, sizeimage=0):
    format = Format(type=queue)
    format.fmt.pix_mp.width = width
    format.fmt.pix_mp.height = height
    format.fmt.pix_mp.pixelformat = pixelformat
    format.fmt.pix_mp.num_planes = 1
    format.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage
    call(fd, TRY_FMT, format, "TRY_FMT")
    call(fd, S_FMT, format, "S_FMT")
    return format.fmt.pix_mp


def hold_for(path):
    say("ready")
    deadline = time.monotonic() + FLAG_WAIT
    while not os.path.exists(path):
        if time.monotonic() > deadline:
            raise Failure("the runner never released the probe")
        time.sleep(0.1)
    say("released")


def finish(fd):
    """Stops both queues and closes the node; each call blocks until the
    device has returned every buffer it held."""
    for name, queue in (("output", OUTPUT), ("capture", CAPTURE)):
        result = stream(fd, STREAMOFF, queue)
        if result:
            raise Failure(f"STREAMOFF {name}: {os.strerror(-result)}")
    os.close(fd)


# ---- decoder


def decode_picture(fd, inputs, outputs, packet):
    """Feeds the access unit again whenever a coded buffer is free and waits
    for one decoded picture; returns the capture buffer's index."""
    deadline = time.monotonic() + WAIT
    while time.monotonic() < deadline:
        while inputs.free:
            index = inputs.free.pop()
            inputs.maps[index][0][: len(packet)] = packet
            result = inputs.put(index, [len(packet)], inputs.stamp)
            inputs.stamp += 1
            if result:
                raise Failure(f"QBUF coded: {os.strerror(-result)}")
        wait(fd, 0.2)
        events(fd)
        take_back(inputs)
        result, buffer, used = outputs.take()
        if result == -errno.EAGAIN:
            continue
        if result:
            raise Failure(f"DQBUF capture: {os.strerror(-result)}")
        if buffer.flags & FLAG_ERROR:
            raise Failure("a capture buffer came back with the error flag")
        if used and used[0]:
            say(f"picture bytes={used}")
            return buffer.index
    raise Failure(f"no picture in {WAIT:g} s")


def take_back(queue):
    """Collects the buffers the device returned into the queue's free list."""
    while True:
        result, buffer, _ = queue.take()
        if result:
            return
        queue.free.append(buffer.index)


def settle_decoder(fd, inputs, outputs, picture):
    """Feeds nothing more and returns to the state between two pictures:
    every coded buffer back, every capture buffer queued with the device."""
    outputs.put(picture)
    deadline = time.monotonic() + WAIT
    quiet = time.monotonic()
    while time.monotonic() < deadline:
        wait(fd, 0.2)
        events(fd)
        take_back(inputs)
        result, buffer, _ = outputs.take()
        if not result:
            outputs.put(buffer.index)
            quiet = time.monotonic()
        elif time.monotonic() - quiet > 1.0 and len(inputs.free) == len(inputs.maps):
            say("settled between two pictures")
            return
    raise Failure(f"the decoder did not settle in {WAIT:g} s")


def decode(packet_path, coded, flag, midstream=False):
    packet = open(packet_path, "rb").read()
    fd = find("stateful-decoder")
    try:
        if flag and not midstream:
            hold_for(flag)
        subscribe(fd, EVENT_SOURCE_CHANGE)
        set_format(fd, OUTPUT, coded, 128, 96, len(packet))
        inputs = Queue(fd, OUTPUT, 2)
        inputs.free = list(range(1, len(inputs.maps)))
        inputs.stamp = 8
        inputs.maps[0][0][: len(packet)] = packet
        for name, request in (("STREAMON output", stream(fd, STREAMON, OUTPUT)),
                              ("QBUF coded", inputs.put(0, [len(packet)], 7))):
            if request:
                raise Failure(f"{name}: {os.strerror(-request)}")
        deadline = time.monotonic() + WAIT
        source = False
        while time.monotonic() < deadline and not source:
            wait(fd, 0.2, select.POLLPRI)
            source = EVENT_SOURCE_CHANGE in events(fd)
        if not source:
            raise Failure(f"no source change in {WAIT:g} s")
        capture = Format(type=CAPTURE)
        call(fd, G_FMT, capture, "G_FMT capture")
        say(f"source change {capture.fmt.pix_mp.width}x{capture.fmt.pix_mp.height}")
        if (capture.fmt.pix_mp.width, capture.fmt.pix_mp.height) != (128, 96):
            raise Failure("the source change names another picture size")
        capture.fmt.pix_mp.pixelformat = NV12M
        call(fd, S_FMT, capture, "S_FMT capture")
        outputs = Queue(fd, CAPTURE, 4)
        for index in range(len(outputs.maps)):
            result = outputs.put(index)
            if result:
                raise Failure(f"QBUF capture: {os.strerror(-result)}")
        result = stream(fd, STREAMON, CAPTURE)
        if result:
            raise Failure(f"STREAMON capture: {os.strerror(-result)}")
        picture = decode_picture(fd, inputs, outputs, packet)
        if midstream:
            settle_decoder(fd, inputs, outputs, picture)
            hold_for(flag)
            decode_picture(fd, inputs, outputs, packet)
    except BaseException:
        os.close(fd)
        raise
    finish(fd)


# ---- encoder


def encode_frame(fd, coded, frames, sizes):
    """Queues raw frames until a coded buffer returns; returns its index."""
    deadline = time.monotonic() + WAIT
    while time.monotonic() < deadline:
        if frames.free:
            index = frames.free.pop()
            for plane, size in zip(frames.maps[index], sizes):
                plane[:size] = bytes([0x80]) * size
            result = frames.put(index, sizes, frames.stamp)
            frames.stamp += 1
            if result:
                raise Failure(f"QBUF raw: {os.strerror(-result)}")
        wait(fd, 0.2)
        events(fd)
        take_back(frames)
        result, buffer, used = coded.take()
        if result == -errno.EAGAIN:
            continue
        if result:
            raise Failure(f"DQBUF coded: {os.strerror(-result)}")
        if buffer.flags & FLAG_ERROR:
            raise Failure("a coded buffer came back with the error flag")
        if used and used[0]:
            say(f"coded bytes={used}")
            return buffer.index
    raise Failure(f"no coded data in {WAIT:g} s")


def settle_encoder(fd, coded, frames, buffer):
    """Feeds nothing more and returns to the state between two frames: every
    raw frame back, every coded buffer queued with the device."""
    coded.put(buffer)
    deadline = time.monotonic() + WAIT
    quiet = time.monotonic()
    while time.monotonic() < deadline:
        wait(fd, 0.2)
        events(fd)
        take_back(frames)
        result, returned, _ = coded.take()
        if not result:
            coded.put(returned.index)
            quiet = time.monotonic()
        elif time.monotonic() - quiet > 1.0 and len(frames.free) == len(frames.maps):
            say("settled between two frames")
            return
    raise Failure(f"the encoder did not settle in {WAIT:g} s")


def encode(flag, midstream=False):
    fd = find("stateful-encoder")
    try:
        if flag and not midstream:
            hold_for(flag)
        subscribe(fd, EVENT_EOS)
        set_format(fd, CAPTURE, fourcc("H264"), 0, 0, 1 << 16)
        raw = Format(type=OUTPUT)
        raw.fmt.pix_mp.width = 128
        raw.fmt.pix_mp.height = 96
        raw.fmt.pix_mp.pixelformat = NV12M
        call(fd, TRY_FMT, raw, "TRY_FMT raw")
        call(fd, S_FMT, raw, "S_FMT raw")
        sizes = [raw.fmt.pix_mp.plane_fmt[i].sizeimage for i in range(raw.fmt.pix_mp.num_planes)]
        coded = Queue(fd, CAPTURE, 4)
        frames = Queue(fd, OUTPUT, 4)
        frames.free = list(range(len(frames.maps)))
        frames.stamp = 1
        for index in range(len(coded.maps)):
            result = coded.put(index)
            if result:
                raise Failure(f"QBUF coded: {os.strerror(-result)}")
        for name, queue in (("output", OUTPUT), ("capture", CAPTURE)):
            result = stream(fd, STREAMON, queue)
            if result:
                raise Failure(f"STREAMON {name}: {os.strerror(-result)}")
        buffer = encode_frame(fd, coded, frames, sizes)
        if midstream:
            settle_encoder(fd, coded, frames, buffer)
            hold_for(flag)
            encode_frame(fd, coded, frames, sizes)
    except BaseException:
        os.close(fd)
        raise
    finish(fd)


def main():
    global label
    label = sys.argv[1]
    kind = sys.argv[2]
    try:
        if kind in ("decode", "decode-stream"):
            decode(sys.argv[3], fourcc(sys.argv[4]), sys.argv[5] if len(sys.argv) > 5 else None,
                   kind == "decode-stream")
        elif kind in ("encode", "encode-stream"):
            encode(sys.argv[3] if len(sys.argv) > 3 else None, kind == "encode-stream")
        else:
            raise Failure(f"unknown session {kind}")
    except Failure as failure:
        say(f"FAIL: {failure}")
        return 1
    except Exception as error:
        say(f"FAIL: {type(error).__name__}: {error}")
        return 1
    say("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
