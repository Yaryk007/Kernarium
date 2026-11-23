#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the Virtio-input registration synchronizer against fixture kernel trees.

The tool copies the wire header into a kernel tree and refuses a tree whose own
input-event-codes.h already defines the header's event type. The fixtures cover
the forms that file uses (plain and parenthesized numbers, a computed count)
and each way the tool must refuse: a collision, a stale copy, a missing
upstream header and a directory that is not a kernel tree.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

UPSTREAM = """\
#define EV_SYN			0x00
#define EV_KEY			0x01
#define EV_LED			0x11
#define EV_FF_STATUS		0x17
#define EV_MAX			0x1f
#define EV_CNT			(EV_MAX+1)
#define KEY_ESC			1
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--header", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    failures = []

    def check(name, passed, detail=""):
        if not passed:
            failures.append(f"{name}: {detail}")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        # The tool finds its header beside itself.
        tool_dir = root / "input"
        tool_dir.mkdir()
        tool = tool_dir / "sync-protocol.py"
        shutil.copy(arguments.tool, tool)
        header = tool_dir / "virtio_input_uml.h"
        shutil.copy(arguments.header, header)

        def tree(name, upstream=UPSTREAM):
            path = root / name
            (path / "include/uapi/linux").mkdir(parents=True)
            (path / "drivers/virtio").mkdir(parents=True)
            (path / "Makefile").write_text("")
            if upstream is not None:
                (path / "include/uapi/linux/input-event-codes.h").write_text(upstream)
            return path

        def run(path, *flags):
            return subprocess.run([sys.executable, "-B", str(tool), "--kernel-source", str(path), *flags],
                                  stdin=subprocess.DEVNULL, capture_output=True, text=True)

        good = tree("good")
        copied = good / "drivers/virtio/virtio_input_uml.h"

        result = run(good)
        check("sync", result.returncode == 0, result.stderr)
        check("sync copies the header", copied.is_file() and copied.read_bytes() == header.read_bytes())

        result = run(good, "--verify")
        check("verify", result.returncode == 0, result.stderr)

        copied.write_bytes(copied.read_bytes() + b"/* local edit */\n")
        result = run(good, "--verify")
        check("stale copy", result.returncode != 0 and "stale" in result.stderr, result.stderr)
        result = run(good)
        check("sync repairs a stale copy", result.returncode == 0, result.stderr)

        # The event type the header owns, taken by the tree.
        result = run(tree("collides", UPSTREAM + "#define EV_NOTICE_OTHER 0x1e\n"))
        check("event type collision", result.returncode != 0 and "VIRTIO_INPUT_UML_EV_NOTICE" in result.stderr
              and "EV_NOTICE_OTHER" in result.stderr and "collide" in result.stderr, result.stderr)
        result = run(tree("collides-parenthesized", UPSTREAM + "#define EV_NOTICE_OTHER (0x1d + 1)\n"))
        check("parenthesized event type collision", result.returncode != 0 and "EV_NOTICE_OTHER" in result.stderr,
              result.stderr)
        # A code of another class with the same number is not a collision.
        result = run(tree("same-number-elsewhere", UPSTREAM + "#define KEY_OTHER 0x1e\n"))
        check("a number in another class", result.returncode == 0, result.stderr)

        result = run(tree("no-upstream", upstream=None))
        check("missing upstream header", result.returncode != 0 and "input-event-codes.h" in result.stderr,
              result.stderr)
        result = run(root / "not-a-tree")
        check("not a kernel tree", result.returncode != 0 and "prepared Linux source tree" in result.stderr,
              result.stderr)

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
