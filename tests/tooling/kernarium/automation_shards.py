# SPDX-License-Identifier: GPL-2.0-or-later
"""Runs a unittest module whole, or one shard of it.

The iPad automation watch tests are pinned by real waits, so CTest runs each
of them as several processes side by side. `--shard INDEX/COUNT` (INDEX counts
from 0) runs the tests whose place among the module's tests, sorted by id, is
INDEX modulo COUNT: the COUNT processes together run every test once, and a
test added to the module joins a shard on its own. A shard that selects no test
is an error. Any other argument is unittest's.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import sys
import unittest


def flatten(suite: unittest.TestSuite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


class ShardLoader(unittest.TestLoader):
    def __init__(self, index: int, count: int) -> None:
        super().__init__()
        self.index = index
        self.count = count

    def loadTestsFromModule(self, module, *args, **kwargs):
        tests = sorted(flatten(super().loadTestsFromModule(module, *args, **kwargs)),
                       key=lambda test: test.id())
        chosen = [test for place, test in enumerate(tests) if place % self.count == self.index]
        if not chosen:
            # unittest ends such a run with status 5 only from Python 3.12; a shard that runs nothing
            # must not pass as a shard that ran its tests.
            raise SystemExit(f"shard {self.index}/{self.count} selects none of the {len(tests)} tests of "
                             f"{module.__name__}")
        return unittest.TestSuite(chosen)


def main(module: str = "__main__") -> None:
    arguments = sys.argv[1:]
    if arguments[:1] != ["--shard"]:
        unittest.main(module=module)
        return
    try:
        index, count = (int(part) for part in arguments[1].split("/"))
    except (IndexError, ValueError):
        index = count = 0
    if not 0 <= index < count:
        raise SystemExit("usage: --shard INDEX/COUNT, with 0 <= INDEX < COUNT")
    unittest.main(module=module, argv=[sys.argv[0], *arguments[2:]],
                  testLoader=ShardLoader(index, count))
