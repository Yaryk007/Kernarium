# SPDX-License-Identifier: GPL-2.0-or-later
"""The host's end of the gamepad relay's wire (vsock port 5558), for the runner that plays the host application.

The contract is android/lineage-24/overlay/gamepad/gamepad_protocol.hpp, whose frames the golden file in
tests/unit/andarium/fixtures pins. Every number is little endian; a frame is the 8-byte header ``AGP1``, a type and a
payload length of at most 4400, and the payload. The host encodes CREATE, DESTROY, INPUT and the two replies; the
guest's HELLO, CREATED, EVENT, OUTPUT, GET_REPORT and SET_REPORT are read by GuestReader.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import struct
from dataclasses import dataclass

PORT = 5558
MAGIC = b"AGP1"
HEADER_SIZE = 8
MAXIMUM_PAYLOAD = 4400
SLOT_COUNT = 8
MAXIMUM_DESCRIPTOR = 4096
MAXIMUM_REPORT = 4096
NAME_SIZE = 128
UNIQ_SIZE = 64
PROTOCOL_VERSION = 1

CREATE, DESTROY, INPUT, GET_REPORT_REPLY, SET_REPORT_REPLY = 1, 2, 3, 4, 5
HELLO, CREATED, EVENT, OUTPUT, GET_REPORT, SET_REPORT = 16, 17, 18, 19, 20, 21
EVENT_KINDS = ("start", "stop", "open", "close")
# uhid's report types, as the OUTPUT, GET_REPORT and SET_REPORT frames carry them.
FEATURE_REPORT, OUTPUT_REPORT, INPUT_REPORT = 0, 1, 2

BUS_USB = 3
BUS_BLUETOOTH = 5


class WireError(ValueError):
    pass


def frame(frame_type: int, payload: bytes) -> bytes:
    if len(payload) > MAXIMUM_PAYLOAD:
        raise WireError(f"a payload of {len(payload)} bytes is over the limit")
    return MAGIC + struct.pack("<HH", frame_type, len(payload)) + payload


def fixed_text(text: str, size: int) -> bytes:
    """A NUL-padded field that holds `text` and always ends in a NUL."""
    raw = text.encode("utf-8")
    if len(raw) >= size:
        raise WireError(f"{text!r} does not fit a field of {size} bytes")
    return raw.ljust(size, b"\0")


def create(slot: int, bus: int, vendor: int, product: int, version: int, country: int, name: str, uniq: str,
           descriptor: bytes, flags: int = 0) -> bytes:
    if not 1 <= len(descriptor) <= MAXIMUM_DESCRIPTOR:
        raise WireError("a descriptor is 1 through 4096 bytes")
    payload = (struct.pack("<BBHIIII", slot, flags, bus, vendor, product, version, country)
               + fixed_text(name, NAME_SIZE) + fixed_text(uniq, UNIQ_SIZE)
               + struct.pack("<H", len(descriptor)) + descriptor)
    return frame(CREATE, payload)


def destroy(slot: int) -> bytes:
    return frame(DESTROY, struct.pack("<B3x", slot))


def input_report(slot: int, report: bytes) -> bytes:
    if not 1 <= len(report) <= MAXIMUM_REPORT:
        raise WireError("a report is 1 through 4096 bytes")
    return frame(INPUT, struct.pack("<BxH", slot, len(report)) + report)


def get_report_reply(slot: int, request_id: int, data: bytes, error: int = 0) -> bytes:
    if len(data) > MAXIMUM_REPORT:
        raise WireError("a reply is at most 4096 bytes")
    return frame(GET_REPORT_REPLY, struct.pack("<BxHIH", slot, error, request_id, len(data)) + data)


def set_report_reply(slot: int, request_id: int, error: int = 0) -> bytes:
    return frame(SET_REPORT_REPLY, struct.pack("<BxHI", slot, error, request_id))


# ---- What the guest sends.

@dataclass(frozen=True)
class Hello:
    version: int
    slots: int


@dataclass(frozen=True)
class Created:
    slot: int
    result: int


@dataclass(frozen=True)
class Event:
    slot: int
    kind: str


@dataclass(frozen=True)
class Output:
    slot: int
    report_type: int
    data: bytes


@dataclass(frozen=True)
class GetReport:
    slot: int
    report_type: int
    number: int
    request_id: int


@dataclass(frozen=True)
class SetReport:
    slot: int
    report_type: int
    number: int
    request_id: int
    data: bytes


GuestFrame = Hello | Created | Event | Output | GetReport | SetReport


def decode_guest_frame(frame_type: int, payload: bytes) -> GuestFrame:
    """The guest's frame of `frame_type`, or WireError for one the relay never sends or one that is malformed."""
    if frame_type == HELLO and len(payload) == 4:
        version, slots = struct.unpack("<HBx", payload)
        return Hello(version, slots)
    if frame_type == CREATED and len(payload) == 8:
        slot, result = struct.unpack("<BxxxI", payload)
        return Created(slot, result - (1 << 32) if result >= 1 << 31 else result)
    if frame_type == EVENT and len(payload) == 4:
        slot, kind = struct.unpack("<BBxx", payload)
        if kind >= len(EVENT_KINDS):
            raise WireError(f"event kind {kind}")
        return Event(slot, EVENT_KINDS[kind])
    if frame_type == OUTPUT and len(payload) >= 4:
        slot, report_type, length = struct.unpack("<BBH", payload[:4])
        if len(payload) != 4 + length:
            raise WireError("OUTPUT length does not fill the payload")
        return Output(slot, report_type, payload[4:])
    if frame_type == GET_REPORT and len(payload) == 8:
        slot, report_type, number, request_id = struct.unpack("<BBBxI", payload)
        return GetReport(slot, report_type, number, request_id)
    if frame_type == SET_REPORT and len(payload) >= 10:
        slot, report_type, number, request_id, length = struct.unpack("<BBBxIH", payload[:10])
        if len(payload) != 10 + length:
            raise WireError("SET_REPORT length does not fill the payload")
        return SetReport(slot, report_type, number, request_id, payload[10:])
    raise WireError(f"frame type {frame_type} with a payload of {len(payload)} bytes is not one the relay sends")


class GuestReader:
    """The guest's frames out of a byte stream that arrives in any pieces. A frame that is not the relay's raises
    WireError, and the reader is of no more use."""

    def __init__(self) -> None:
        self.buffer = b""

    def feed(self, data: bytes) -> list[GuestFrame]:
        self.buffer += data
        frames: list[GuestFrame] = []
        while len(self.buffer) >= HEADER_SIZE:
            magic, frame_type, length = struct.unpack("<4sHH", self.buffer[:HEADER_SIZE])
            if magic != MAGIC or length > MAXIMUM_PAYLOAD:
                raise WireError(f"a header that is not the relay's: {self.buffer[:HEADER_SIZE].hex()}")
            if len(self.buffer) < HEADER_SIZE + length:
                break
            payload = self.buffer[HEADER_SIZE:HEADER_SIZE + length]
            self.buffer = self.buffer[HEADER_SIZE + length:]
            frames.append(decode_guest_frame(frame_type, payload))
        return frames
