#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The licence notice of each library, as the image's own NOTICE states it.

A partition carries etc/NOTICE.xml.gz and an APEX carries assets/NOTICE.html.gz.
Both list, per file, the notice texts that apply to it. The rewriter records
what the notice says for every library it rewrites in a generated file beside
the framework; nothing from the image is tracked. A library the notice does
not list is refused, because the build could not say under which terms the
rewritten text is built.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import gzip
import html
import pathlib
import re
import zipfile


class NoticeError(Exception):
    pass


def _xml(data: str) -> dict[str, dict]:
    """path -> {labels, notices}"""
    texts = {m.group(1): html.unescape(m.group(2)) for m in re.finditer(
        r'<file-content contentId="(\w+)"><!\[CDATA\[(.*?)\]\]></file-content>',
        data, re.S)}
    files: dict[str, dict] = {}
    for m in re.finditer(r'<file-name contentId="(\w+)" lib="([^"]*)">'
                         r'([^<]*)</file-name>', data):
        entry = files.setdefault(m.group(3), {"labels": set(), "ids": []})
        entry["labels"].add(m.group(2))
        if m.group(1) not in entry["ids"]:
            entry["ids"].append(m.group(1))
    return {path: {"labels": sorted(entry["labels"]),
                   "notices": [{"id": i, "text": texts.get(i, "")}
                               for i in entry["ids"]]}
            for path, entry in files.items()}


def _apex_html(data: str, apex: str) -> dict[str, dict]:
    texts = {}
    for m in re.finditer(r'<a id="(\w+)"/><pre class="license-text">(.*?)</pre>',
                         data, re.S):
        texts[m.group(1)] = html.unescape(m.group(2))
    files: dict[str, dict] = {}
    for m in re.finditer(r'<li id="id\d+"><strong>([^<]+)</strong>\s*<ul>(.*?)'
                         r'</ul>', data, re.S):
        path = "/apex/" + m.group(1)
        entry = files.setdefault(path, {"labels": set(), "ids": []})
        for link in re.finditer(r'<a href="#(\w+)">([^<]*)</a>', m.group(2)):
            entry["labels"].add(link.group(2))
            if link.group(1) not in entry["ids"]:
                entry["ids"].append(link.group(1))
    return {path: {"labels": sorted(entry["labels"]),
                   "notices": [{"id": i, "text": texts.get(i, "")}
                               for i in entry["ids"]]}
            for path, entry in files.items()}


def notices(root: pathlib.Path, mounts: dict[str, pathlib.Path]
            ) -> dict[str, dict]:
    """Every file's notice, keyed by guest path, from the trees in use."""
    result: dict[str, dict] = {}
    seen = {}
    candidates = [("", root / "system" / "etc" / "NOTICE.xml.gz")]
    for guest, tree in mounts.items():
        if guest.startswith("/apex/"):
            apex = root / "system" / "apex" / (guest[len("/apex/"):] + ".apex")
            candidates.append((guest, apex))
        else:
            candidates.append((guest, tree / "etc" / "NOTICE.xml.gz"))
    for guest, source in candidates:
        if str(source) in seen:
            continue
        seen[str(source)] = True
        if not source.exists():
            continue
        if source.suffix == ".apex":
            with zipfile.ZipFile(source) as archive:
                data = gzip.decompress(archive.read("assets/NOTICE.html.gz")
                                       ).decode()
            result.update(_apex_html(data, source.stem))
        else:
            result.update(_xml(gzip.open(source, "rt").read()))
    return result


def record(root: pathlib.Path, mounts: dict[str, pathlib.Path],
           libraries: list[str]) -> list[dict]:
    """One entry per library: its notice labels and texts, or an error."""
    table = notices(root, mounts)
    out = []
    for path in libraries:
        entry = table.get(path)
        if entry is None or not entry["notices"]:
            raise NoticeError(
                f"{path}: the image's NOTICE does not list this file")
        out.append({"path": path, "labels": entry["labels"],
                    "notices": entry["notices"]})
    return out
