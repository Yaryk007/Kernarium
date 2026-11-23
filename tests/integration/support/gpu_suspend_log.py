# SPDX-License-Identifier: GPL-2.0-or-later
"""What the launcher's log says of the GPU across a guest suspend.

When the host resets its virtio-gpu device across a suspend, every context and
resource the guest holds is gone: the guest kernel's console shows the host
refusing its commands as
`[drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1204 (command 0x202)`
and the host says `Virtio-GPU submit rejected unknown context: id=3 bytes=44`.
The guest's applications then abort in the graphics stack and the framework
follows. Neither of those lines names the reset: a renderer that refuses a
context's creation, or an out-of-memory response (0x1201), prints the same
kinds of line without any reset. Only the host's own report of the reset says
so, and it begins with `RESET_LINE`. A runner that suspends the guest reads
the launcher's log from where it stood before the suspend, and a suspend
whose interval shows

- the host's reset line fails with `RESET_LABEL` and that line: the known
  host defect, told apart from the failure it causes in the runner's subject;
- no reset line but a GPU error line (the kernel's refused command, the
  host's refusal of an unknown context) fails with `ERRORS_LABEL` and the
  first such line, and says the cause is not established: on a launcher
  that does not print the reset line this is also what the known defect
  looks like.

Output can arrive after the step that suspended the guest has returned (the
first graphics work after the wake prints a refusal when it reaches a missing
context), so the mark is kept pending through the checks that follow the wake:
`Pending.peek` scans it when the step returns, `Pending.settle` scans it and
ends it at the end of the section or check (also when that failed), and
`Pending.begin` scans an earlier one before it takes a new mark. No sleep
stands in for a boundary.

The runner does not quiet the screen before the suspend: the reset is a defect
of the host that the product may share, and hiding it from a proof would hide
it from the product.
"""

from __future__ import annotations

import pathlib
from collections.abc import Callable

RESET_LINE = "Vhost-user: device reset at ring disable:"
KERNEL = "virtio_gpu_dequeue_ctrl_func"
HOST = "submit rejected unknown context"
RESET_LABEL = "host GPU reset across the suspend"
ERRORS_LABEL = "GPU errors across the suspend"
NOT_ESTABLISHED = (f"the cause is not established: the log has no \"{RESET_LINE}\" line; on a launcher that does "
                   "not print it, the known host reset looks like this")


def diagnosis(text: str) -> str | None:
    """The failure message for a launcher log taken since a suspend, or None: the host's reset line (a line that
    begins with RESET_LINE) is the reset, the first GPU error line without it is only a GPU error."""
    first_error = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(RESET_LINE):
            return f"{RESET_LABEL}: {stripped[:300]}"
        if first_error is None and ((KERNEL in line and "ERROR" in line) or HOST in line):
            first_error = stripped[:300]
    return None if first_error is None else f"{ERRORS_LABEL}: {first_error} ({NOT_ESTABLISHED})"


def log_size(log: pathlib.Path) -> int:
    """How much of the launcher's log (the host's messages and the guest kernel's console) is written now."""
    try:
        return log.stat().st_size
    except OSError:
        return 0


def log_since(log: pathlib.Path, offset: int) -> str:
    with log.open("rb") as stream:
        stream.seek(offset)
        return stream.read().decode(errors="replace")


class Pending:
    """The launcher's log position at the start of a suspend, kept until the checks that follow the wake have
    settled. `size` and `since` read the log, so a runner's guest supplies its own."""

    def __init__(self, size: Callable[[], int], since: Callable[[int], str]) -> None:
        self.size, self.since = size, since
        self.mark: int | None = None

    def begin(self) -> str | None:
        """Takes the mark of a new suspend. A mark an earlier suspend left pending is scanned and ended first, and
        its diagnosis, if any, is returned: it must not be lost by being replaced."""
        earlier = self.settle()
        self.mark = self.size()
        return earlier

    def peek(self) -> str | None:
        """The diagnosis of the log since the mark, with the mark still pending."""
        return None if self.mark is None else diagnosis(self.since(self.mark))

    def settle(self) -> str | None:
        """The diagnosis of the log since the mark, and the mark ended (a diagnosis is reported once)."""
        found = self.peek()
        self.mark = None
        return found
