# SPDX-License-Identifier: GPL-2.0-or-later
"""How a frame of the synthetic camera's chart is turned.

The chart (darwin/platform/synthetic_camera) carries a marker on its middle row, read left to right in 64 cells
(a tag 0x5aNN, a counter and a check word), colour patches above it, a horizontal ramp along its top, and a slanted
edge in its bottom quarter. Of the eight turns and mirrors of a frame, only the one that shows the chart as the
camera drew it decodes a marker whose tag has the high byte 0x5a (the check word alone also holds for a marker
read from the other end) and has the edge at the bottom. Turns are clockwise; the mirror is left to right after
the turn.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from typing import Callable

MARKER_CELLS = 64
MARKER_HEIGHT = 8
TAG_HIGH_BYTE = 0x5A

Sample = Callable[[int, int], int]


def view(luma: bytes, width: int, height: int, turn: int, mirror: bool) -> tuple[int, int, Sample]:
    """The frame turned clockwise by `turn` (0, 90, 180 or 270) and then mirrored: its width, its height and
    its luma at (x, y)."""
    if turn not in (0, 90, 180, 270):
        raise ValueError(f"{turn} is not a quarter turn")
    turned_width, turned_height = (height, width) if turn in (90, 270) else (width, height)

    def sample(x: int, y: int) -> int:
        if mirror:
            x = turned_width - 1 - x
        if turn == 0:
            source_x, source_y = x, y
        elif turn == 90:
            source_x, source_y = y, height - 1 - x
        elif turn == 180:
            source_x, source_y = width - 1 - x, height - 1 - y
        else:
            source_x, source_y = width - 1 - y, x
        return luma[source_y * width + source_x]

    return turned_width, turned_height, sample


def decode_marker(width: int, height: int, sample: Sample) -> tuple[bool, int, int]:
    """(check word valid, tag, counter) of the marker on the middle row of a view."""
    row = height // 2
    bits = 0
    for index in range(MARKER_CELLS):
        bits = (bits << 1) | (1 if sample((2 * index + 1) * width // (2 * MARKER_CELLS), row) >= 128 else 0)
    check = bits & 0xFFFF
    first48 = bits >> 16
    tag, counter = first48 >> 32, first48 & 0xFFFFFFFF
    return check == ((tag ^ (first48 >> 16) ^ first48) & 0xFFFF), tag, counter


def edge_at_bottom(width: int, height: int, sample: Sample) -> int:
    """How much more the left and right ends of the bottom rows differ than those of the top rows. The slanted
    edge (40 on the left, 220 on the right) makes the bottom differ by about 180 and the ramp the top by about
    160, so a view with the chart upright is positive and one turned half around negative."""
    step = max(1, width // 64)
    left = range(width // 16, width // 4, step)
    right = range(width * 3 // 4, width * 15 // 16, step)

    def spread(y: int) -> int:
        return abs(sum(sample(x, y) for x in right) - sum(sample(x, y) for x in left)) // max(len(left), 1)

    return spread(height * 15 // 16) - spread(height // 32)


def upright_views(luma: bytes, width: int, height: int, tag: int | None = None) -> list[tuple[int, bool]]:
    """The (turn, mirror) pairs that show the chart as the camera drew it: the marker's check word holds, its tag
    is the chart's and the edge is at the bottom. A frame of the chart has exactly one. With `tag`, the marker
    must carry exactly that tag (the camera's own: 0x5a00 and the camera's platform index), not any chart's."""
    found = []
    for turn in (0, 90, 180, 270):
        for mirror in (False, True):
            turned_width, turned_height, sample = view(luma, width, height, turn, mirror)
            valid, read, _ = decode_marker(turned_width, turned_height, sample)
            named = read == tag if tag is not None else read >> 8 == TAG_HIGH_BYTE
            if valid and named and edge_at_bottom(turned_width, turned_height, sample) > 0:
                found.append((turn, mirror))
    return found


def luma_of_nv21(frame: bytes, width: int, height: int) -> bytes:
    """The luma plane of an NV21 frame; a frame shorter than NV21 of that size is refused."""
    if len(frame) < width * height * 3 // 2:
        raise ValueError(f"{len(frame)} bytes are fewer than an NV21 frame of {width}x{height}")
    return frame[:width * height]
