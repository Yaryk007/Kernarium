#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synchronize the private Virtio-input registration header into a kernel tree.

Every run, with or without --verify, also checks that no event type of the
header is one the tree's own input-event-codes.h already defines, so a kernel
base that grows a colliding type is caught here instead of on the wire.
"""
__import__("sys").dont_write_bytecode = True

import argparse
import re
from pathlib import Path

HEADER = "virtio_input_uml.h"
DESTINATION = "drivers/virtio/virtio_input_uml.h"
UPSTREAM = "include/uapi/linux/input-event-codes.h"

DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)\s+(.+?)\s*$")
SAFE = re.compile(r"^[0-9a-fA-FxX<>()|+\-uUlL ]+$")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def numbers(text, prefix):
    """Return {name: value} for the integer macros of text whose names start with prefix."""
    values = {}
    for line in strip_comments(text).splitlines():
        define = DEFINE.match(line)
        if not define or not define.group(1).startswith(prefix):
            continue
        expression = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+\b", "", define.group(2).strip())
        if not SAFE.match(expression):
            continue
        try:
            values[define.group(1)] = int(eval(expression, {"__builtins__": {}}, {}))  # noqa: S307 (digits and operators only)
        except (SyntaxError, ValueError, TypeError):
            continue
    return values


def collisions(ours, theirs):
    return [
        f"{name} (event type {value:#x}) is also {other} in {UPSTREAM}"
        for name, value in ours.items()
        for other, other_value in theirs.items()
        if other_value == value
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-source", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).with_name(HEADER)
    destination = args.kernel_source / DESTINATION
    upstream = args.kernel_source / UPSTREAM
    if not (args.kernel_source / "Makefile").is_file() or not destination.parent.is_dir():
        parser.error("--kernel-source must name a prepared Linux source tree")
    if args.verify:
        if not destination.is_file() or source.read_bytes() != destination.read_bytes():
            parser.error("Virtio-input registration header copy is stale")
    else:
        destination.write_bytes(source.read_bytes())
    if not upstream.is_file():
        parser.error(f"{upstream} is missing")
    ours = numbers(source.read_text(), "VIRTIO_INPUT_UML_EV_")
    if not ours:
        parser.error(f"{HEADER} defines no event type")
    problems = collisions(ours, numbers(upstream.read_text(), "EV_"))
    if problems:
        parser.error("Virtio-input registration codes collide with the kernel:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
