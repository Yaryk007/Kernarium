#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that Andarium opens the files a person picks as copies.

Andarium's two document pickers (Install APK in Shared/Settings.swift and the
file browser's Import in Shared/FileBrowserBar.swift) present the copy-mode
picker of Shared/CopyDocumentPicker.swift in every build
(Andarium/README.md#configurations): Files hands over copies in the
application's own temporary folder, so the guest never holds a security-scoped
URL into someone else's storage. This reads the application's sources and
fails when:

  - any source calls SwiftUI's fileImporter, which has no copy mode;
  - a source other than CopyDocumentPicker.swift creates a document picker
    without `asCopy: true`;
  - CopyDocumentPicker.swift does not create its picker with `asCopy: true`, or
    does not define documentCopyPicker; or
  - one of the two call sites presents no documentCopyPicker.

Comments and string literals are not code. The build phase "Check document
pickers" runs it on every build; tests/tooling/andarium/test_document_pickers.py
holds the fixtures.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
from pathlib import Path

# Paths below the application's source directory (Andarium/Andarium).
CALL_SITES = ("Shared/Settings.swift", "Shared/FileBrowserBar.swift")
PRESENTER = "Shared/CopyDocumentPicker.swift"

IN_PLACE_IMPORTER = re.compile(r"\bfileImporter\s*\(")
DOCUMENT_PICKER = re.compile(r"\bUIDocumentPickerViewController(?:\s*\.\s*init)?\s*\(")
COPY_PICKER_CALL = re.compile(r"\.\s*documentCopyPicker\s*\(")
COPY_PICKER_DEFINITION = re.compile(r"\bfunc\s+documentCopyPicker\s*\(")
AS_COPY = re.compile(r"\basCopy\s*:\s*true\b")


def code_of(text: str) -> str:
    """`text` with every comment and the inside of every string literal blanked.

    Offsets and line numbers are kept. Block comments nest, as Swift's do; a
    multi-line string ends at its closing delimiter. Raw strings are not used
    in the application and are read as ordinary ones."""
    out: list[str] = []
    i, n = 0, len(text)

    def blank(chunk: str) -> str:
        return "".join(c if c == "\n" else " " for c in chunk)

    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(blank(text[i:end]))
            i = end
        elif text.startswith("/*", i):
            depth, j = 1, i + 2
            while j < n and depth:
                if text.startswith("/*", j):
                    depth, j = depth + 1, j + 2
                elif text.startswith("*/", j):
                    depth, j = depth - 1, j + 2
                else:
                    j += 1
            out.append(blank(text[i:j]))
            i = j
        elif text.startswith('"""', i):
            end = text.find('"""', i + 3)
            end = n if end < 0 else end + 3
            if end - i >= 6:
                out.append('"""' + blank(text[i + 3:end - 3]) + '"""')
            else:
                out.append(blank(text[i:end]))
            i = end
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] not in '"\n':
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            out.append('"' + blank(text[i + 1:j]) + '"')
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def call_arguments(text: str, start: int) -> str:
    """What is between the parentheses of the call whose opening one is at
    `start`; the rest of the text when they never close."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    return text[start + 1:]


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def problems(sources: dict[str, str]) -> list[str]:
    """What is wrong with the pickers of these sources, a path below the
    application's source directory mapped to its text."""
    found: list[str] = []
    code = {name: code_of(text) for name, text in sources.items()}

    for name, text in sorted(code.items()):
        for match in IN_PLACE_IMPORTER.finditer(text):
            found.append(
                f"{name}:{line_of(text, match.start())}: fileImporter opens the files in "
                "place; present documentCopyPicker (Shared/CopyDocumentPicker.swift)"
            )
        for match in DOCUMENT_PICKER.finditer(text):
            arguments = call_arguments(text, match.end() - 1)
            if not AS_COPY.search(arguments):
                found.append(
                    f"{name}:{line_of(text, match.start())}: a document picker without "
                    "`asCopy: true` opens the files in place"
                )
            elif name != PRESENTER:
                found.append(
                    f"{name}:{line_of(text, match.start())}: a document picker outside "
                    f"{PRESENTER}; present documentCopyPicker"
                )

    presenter = code.get(PRESENTER)
    if presenter is None:
        found.append(f"{PRESENTER}: the copy-mode presenter is missing")
    else:
        if not COPY_PICKER_DEFINITION.search(presenter):
            found.append(f"{PRESENTER}: the presenter defines no documentCopyPicker")
        # A creation without `asCopy: true` is reported above, with its line.
        if not DOCUMENT_PICKER.search(presenter):
            found.append(f"{PRESENTER}: the presenter creates no document picker")

    for name in CALL_SITES:
        text = code.get(name)
        if text is None:
            found.append(f"{name}: the call site is missing")
        elif not COPY_PICKER_CALL.search(text):
            found.append(f"{name}: presents no documentCopyPicker")
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "sources",
        type=Path,
        nargs="?",
        default=Path(__file__).resolve().parent.parent / "Andarium",
        help="the application's source directory (Andarium/Andarium)",
    )
    options = parser.parse_args()
    root: Path = options.sources
    if not root.is_dir():
        print(f"{root}: is not a directory", file=sys.stderr)
        return 1
    sources = {
        str(path.relative_to(root)): path.read_text(encoding="utf-8")
        for path in sorted(root.rglob("*.swift"))
    }
    failures = problems(sources)
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
