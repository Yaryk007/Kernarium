# SPDX-License-Identifier: GPL-2.0-or-later
"""The shared-verification rows of the AOT guest runner.

aot_guest_share.cpp (loaded into a stock process with LD_PRELOAD) runs the
rewritten libz.so in many processes: the library of the read-only file system,
a byte-identical copy on a writable one, a page of the library copied by a
write (in the children, or in their parent before they are made), one whose
protection changed with and without a store, a library whose pages the kernel
was made to reclaim between rounds, and one whose pages were dropped from the
page cache. On the study's own privileged guest (`--study-eviction`) one more
mode evicts the library's pages from every address space that maps them.
Every mode runs once without the component and once with it, and
the lines it prints must be equal. In a diagnostics build the runner also
reads how often the framework hashed a page, how often a verdict it kept
answered instead (`shared_hits`) and how often it refused a page whose mapping
lacks the kernel's immutable fact, around each mode, and each mode states what
those counts must show.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import re
from typing import Callable

PROGRAM = "/data/local/tmp/libaotshare.so"
MODES = ("fork", "copy", "cow", "cow-parent", "mprotect", "mprotect-write", "pressure", "evict")
# The mode that needs a kernel which pages out file pages whoever maps them
# (the study's, never shipped); the runner adds it only on request.
STUDY_MODES = ("reclaim",)
# What a study eviction must show, in each of its rounds: pages the guest held
# before it, the share of them it lost, and over the rounds the pages the
# children afterwards must have hashed again (half the pages lost, and four a
# round at the least; the engine hashes more pages than it lost, and one that
# answers for a frame it gave back hashes a fifth of them).
RECLAIM_ROUNDS = 3
RECLAIM_MIN_CACHED = 16
RECLAIM_MIN_HASHED = 4
# Pages the framework must have hashed again because the epoch of their
# frame moved, in each round after the first (the page cache gives frames
# back to the pages that held them, which a verdict of the frame would
# answer for were its epoch not to move).
RECLAIM_MIN_EPOCH_REHASHES = 1
MODE_SECONDS = 300
# The children of a mode, one after another or together, and the sums they print.
CHILD = re.compile(r"^(\S+(?: \S+)*) child (\d+) sum=0x[0-9a-f]+$")
# The least children whose pages the kernel marks immutable that the framework
# must have found verified (fork: sixteen children each run the same pages).
FORK_MIN_SHARED = 16


class ShareFailure(RuntimeError):
    pass


# The reclaim mode runs as one invocation per round, so that the framework's
# counters can be read around each: the children of a round, in the first the
# ones that precede its eviction as well.
RECLAIM_ROUND_CHILDREN = (6, 3, 3)


def command(mode: str, round_: int | None = None) -> str:
    # The names of the rounds are digits: a round is one of RECLAIM_ROUNDS.
    choice = "" if round_ is None else f"AOT_SHARE_ROUND={round_} "
    return (f"cd /data/local/tmp && AOT_SHARE_MODE={mode} {choice}LD_PRELOAD={PROGRAM} "
            f"/system/bin/toybox true; echo rc=$?")


def invocations(mode: str) -> tuple[int | None, ...]:
    """The invocations a mode makes: one, or one per round."""
    return tuple(range(RECLAIM_ROUNDS)) if mode == "reclaim" else (None,)


EXPECTED_CHILDREN = {"fork": 16, "copy": 16, "cow": 10, "cow-parent": 7, "mprotect": 6,
                     "mprotect-write": 6, "pressure": 15, "evict": 6, "reclaim": 12}
# Children that write a text page of their own copy, in the modes that make them,
# and the children that run the library's unchanged pages.
COW_WRITERS = {"cow": 3, "cow-parent": 7, "mprotect-write": 3}
UNCHANGED_CHILDREN = {"cow": 7, "cow-parent": 0, "mprotect": 6, "mprotect-write": 3, "pressure": 15, "evict": 6,
                      "reclaim": 12}


def modes(study: bool) -> tuple[str, ...]:
    """The modes a run makes: the study's own only when it is asked for."""
    return MODES + STUDY_MODES if study else MODES


def run_mode(shell: Callable[..., str], mode: str, round_: int | None = None) -> list[str]:
    """One run of a mode (or of one of its rounds); returns its record,
    checked for what it must show."""
    output = shell(command(mode, round_), MODE_SECONDS)
    lines = output.splitlines()
    status = [line for line in lines if line.startswith("rc=")]
    if not status or status[-1] != "rc=0":
        raise ShareFailure(f"{mode}: {status[-1] if status else 'no exit status'}:\n{output}")
    failed = [line for line in lines if line.startswith("FAIL") or re.search(r"(=|: )NO\b", line)]
    if failed:
        raise ShareFailure(f"{mode} reported:\n" + "\n".join(failed) + f"\n--- output:\n{output}")
    record = [line for line in lines if not line.startswith("rc=")]
    if f"done mode={mode}" not in record:
        raise ShareFailure(f"{mode}: no closing record:\n{output}")
    children = [line for line in record if CHILD.match(line)]
    expected = EXPECTED_CHILDREN[mode] if round_ is None else RECLAIM_ROUND_CHILDREN[round_]
    if len(children) != expected:
        raise ShareFailure(f"{mode}: {len(children)} children reported, "
                           f"not {expected}:\n{output}")
    return record


def informational(record: list[str]) -> list[str]:
    """The lines a mode prints for the reader, that depend on the guest's
    memory and not on the engine: they start with '# ' and are not compared."""
    return [line for line in record if line.startswith("# ")]


def compare(mode: str, off: list[str], on: list[str]) -> int:
    """The record without the component and with it must be equal; returns
    the number of lines compared."""
    off = [line for line in off if not line.startswith("# ")]
    on = [line for line in on if not line.startswith("# ")]
    if off != on:
        for number, (before, after) in enumerate(zip(off, on), 1):
            if before != after:
                raise ShareFailure(f"{mode} line {number}: {before!r} without the "
                                   f"component, {after!r} with it")
        raise ShareFailure(f"{mode}: {len(off)} lines without the component, {len(on)} with it")
    return len(on)


PRESSURE_ROUND = re.compile(
    r"^# round (\d+): (\d+) faults read libz\.so's pages back; the pressure child exited -?\d+$")


def check_readbacks(records: dict[str, list[str]]) -> str:
    """A read-only system file's residency is not observable in an unrooted
    guest except through the faults that reading its pages back takes, so the
    stock guest's pressure and evict rows can show a frame of libz.so was
    given up only by those faults. Whether the guest let a page go in those
    rows is chance (an unmapped page of a library the guest keeps running is
    dropped only if it stays unmapped long enough), so the count is printed
    and not judged: the reclaim row, on the study guest, and the unit rows
    hold the mechanism. The rows must still have printed it. Returns a line
    to print."""
    pressure = [m for m in map(PRESSURE_ROUND.match, records.get("pressure", [])) if m]
    libz = next((m for m in map(LIBZ.match, records.get("evict", [])) if m), None)
    if not pressure or libz is None:
        raise ShareFailure("the pressure and evict rows lack their read-back counts")
    from_pressure = sum(int(m.group(2)) for m in pressure)
    from_evict = int(libz.group(2))
    return (f"stock rows (informational): {from_pressure + from_evict} faults read libz.so's pages back "
            f"({from_pressure} after pressure, {from_evict} after the eviction)")


CONTROL = re.compile(r"^# control: (\d+) of 256 pages cached before, (\d+) after$")
LIBZ = re.compile(r"^# libz\.so: (\d+) faults read pages back before the eviction, (\d+) after$")
RECLAIM = re.compile(r"^# reclaim: libz\.so (\d+) pages cached before the eviction, (\d+) after; "
                     r"(\d+) faults read them back$")


def check_counts(mode: str, delta: dict[str, int], fork_shared: int | None,
                 record: list[str] | None = None,
                 per_round: list[dict[str, int]] | None = None) -> str:
    """The framework's counters of libz.so around a mode in a diagnostics
    build: pages verified by their hash, verifications a verdict of another
    address space answered, and pages refused for lack of the kernel's
    immutable fact. Returns a line to print, or raises. `fork_shared` is
    `shared` of the fork mode; `record` is the mode's record; `per_round`
    is the counters around each invocation of a mode that has rounds."""
    shared = delta.get("shared", 0)
    hashed = delta.get("verified", 0)
    refused = delta.get("mutable", 0)
    text = (f"share row {mode}: libz.so pages verified by hash {hashed}, answered by a "
            f"shared verdict {shared}, refused for lack of the immutable fact {refused}")
    if mode == "fork":
        if shared < FORK_MIN_SHARED:
            raise ShareFailure(f"{text}: sixteen children over the read-only library "
                               f"found fewer than {FORK_MIN_SHARED} verdicts to reuse")
        if refused:
            raise ShareFailure(f"{text}: a page of the read-only library was refused")
    elif mode == "copy":
        if fork_shared is None or shared * 4 > fork_shared:
            raise ShareFailure(f"{text}: a copy on a writable file system shared "
                               f"verdicts (fork mode: {fork_shared})")
        # The kernel does not declare the copy's pages immutable: rewritten
        # code is not entered for them, and the page verification says so.
        if refused < 12:
            raise ShareFailure(f"{text}: the copy's pages were not refused")
    elif mode in ("cow", "cow-parent", "mprotect-write"):
        # A page that is a copy in the child's address space is not the file
        # system's, whatever its mapping says: each child that ran it must
        # have been refused it, and the pages nothing changed still shared.
        if refused < COW_WRITERS[mode]:
            raise ShareFailure(f"{text}: {COW_WRITERS[mode]} children ran a copied page and "
                               f"it was refused fewer times")
        if shared < max(UNCHANGED_CHILDREN[mode], 4):
            raise ShareFailure(f"{text}: the library's unchanged pages were not shared")
    elif mode == "mprotect":
        # A protection that went to writable and back with no store leaves the
        # page the file system's: nothing is refused, and the pages share.
        if refused:
            raise ShareFailure(f"{text}: a page nothing wrote was refused")
        if shared < UNCHANGED_CHILDREN[mode]:
            raise ShareFailure(f"{text}: the library's pages were not shared")
    elif mode == "pressure":
        if shared < UNCHANGED_CHILDREN[mode] or refused:
            raise ShareFailure(f"{text}: the pages of the read-only library were not shared "
                               "(or one was refused)")
    elif mode == "evict":
        text += evict_check(record or [], hashed, shared, refused)
    elif mode == "reclaim":
        text += reclaim_check(record or [], hashed, shared, refused, per_round)
    return text


def reclaim_evicted(record: list[str]) -> int:
    """What the reclaim mode's record alone must show, with or without the
    framework's counts: in each round the guest evicted most of libz.so's
    pages from every address space and reading them back took faults. A stock
    kernel does neither, so a run on one fails here however it was started.
    Returns the pages evicted over the rounds."""
    rounds = [m for m in map(RECLAIM.match, record) if m]
    if len(rounds) != RECLAIM_ROUNDS:
        raise ShareFailure(f"the reclaim record has {len(rounds)} rounds, not {RECLAIM_ROUNDS}")
    lost = 0
    for number, found in enumerate(rounds, 1):
        before, after, readbacks = (int(found.group(i)) for i in (1, 2, 3))
        if before < RECLAIM_MIN_CACHED:
            raise ShareFailure(f"round {number}: libz.so had {before} pages cached, fewer than "
                               f"{RECLAIM_MIN_CACHED}: the row cannot observe an eviction")
        if (before - after) * 2 < before:
            raise ShareFailure(f"round {number}: the guest evicted {before - after} of {before} pages "
                               "of libz.so: it is not the study's privileged one (a stock kernel "
                               "spares mapped file pages and reports every page of a read-only "
                               "file resident)")
        if readbacks < 1:
            raise ShareFailure(f"round {number}: {before - after} pages of libz.so were evicted and "
                               "reading them back took no fault: the pages never left the page cache")
        lost += before - after
    return lost


def reclaim_check(record: list[str], hashed: int, shared: int, refused: int,
                  per_round: list[dict[str, int]] | None = None) -> str:
    """The reclaim mode: in each round the study kernel must have evicted most
    of libz.so's pages from every address space (the row fails on a guest that
    spares them) and reading them back must have taken faults; the children
    that run it again must have had pages verified by hash (never a verdict
    taken for a frame the page cache gave up, in the round that gives a frame
    back to the page it held two rounds before as much as in the first) and
    the later ones must share what the first left. Whether a frame was given
    back is not assumed of the guest's allocator: the framework counts the
    pages it hashed because the epoch of their frame had moved (a verdict of
    the same frame for the same page under an older epoch), and each round
    after the first must have had one. Returns what to add to the line."""
    lost = reclaim_evicted(record)
    if refused:
        raise ShareFailure("a page of the read-only library was refused")
    due = max(RECLAIM_MIN_HASHED * RECLAIM_ROUNDS, lost // 2)
    if hashed < due:
        raise ShareFailure(f"{lost} pages of libz.so were evicted in {RECLAIM_ROUNDS} rounds and the "
                           f"children that ran it again hashed only {hashed} pages (at least "
                           f"{due} are due): a verdict of a frame the page cache gave up "
                           "answered for the page read back")
    if shared < 2:
        raise ShareFailure("the children after the eviction shared no verdict")
    if per_round is None or len(per_round) != RECLAIM_ROUNDS or \
            any("rehash" not in counted for counted in per_round):
        raise ShareFailure("the reclaim row needs the framework's re-hash count of each round "
                           f"({RECLAIM_ROUNDS} rounds, from a diagnostics build)")
    forced = [counted["rehash"] for counted in per_round]
    for number, count in enumerate(forced[1:], 2):
        if count < RECLAIM_MIN_EPOCH_REHASHES:
            raise ShareFailure(f"round {number}: no page was hashed again because the epoch of its "
                               "frame had moved (re-hashes per round: "
                               f"{', '.join(map(str, forced))}): the page cache gave no frame back to "
                               "the page it held, so the row cannot observe the reuse")
    return (f"; libz.so lost {lost} pages in {RECLAIM_ROUNDS} rounds, hashed again because the epoch "
            f"moved {', '.join(map(str, forced))} (per round)")


def evict_check(record: list[str], hashed: int, shared: int, refused: int) -> str:
    """The evict mode: the control must have lost its pages (the program fails
    itself otherwise), whether libz.so lost any shows in the faults that read
    its pages back (a read-only system file's residency is not observable any
    other way), and the pages of the library that were not evicted still share
    their verdicts. Returns what to add to the line."""
    control = next((m for m in map(CONTROL.match, record) if m), None)
    libz = next((m for m in map(LIBZ.match, record) if m), None)
    if control is None or libz is None:
        raise ShareFailure("the evict record lacks its control or libz.so counts")
    if int(control.group(1)) < 128 or int(control.group(2)) != 0:
        raise ShareFailure("the control file was not evicted: the row cannot observe eviction")
    if refused:
        raise ShareFailure("a page of the read-only library was refused")
    if shared < 3:
        raise ShareFailure("the pages of the read-only library were not shared")
    return f"; the control lost its pages, {libz.group(2)} faults read libz.so's pages back"
