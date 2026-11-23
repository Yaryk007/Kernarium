#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the pinned sources of TalkBack and eSpeak NG and optional checkouts.

The image builds TalkBack and eSpeak NG (with libsonic) from source. Each is a
pinned upstream commit; TalkBack and eSpeak NG also carry a maintained series
under `dependencies/patches/`, each with its Soong build. The
metadata beside this tool pins the commits, the trees they hold and where each
source is materialized, and `dependencies/manifest.tsv` pins the same commits
for the topic checkouts that maintain the series. The two must agree, and the
tree a series records is the identity of the source the build sees.

With --aosp-source the materialized checkouts are verified as well (--only
limits that to one, while the others are being materialized): each on its
pinned commit (libsonic detached) or, for a series, the maintained topic branch
at exactly the series' tree, as a clean linear replay over the pinned commit.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

import pinned_source
from pinned_source import run as _run
# pinned_source puts scripts/ on sys.path.
import patch_series  # noqa: E402


class VerificationError(ValueError):
    pass


class Source:
    """One pinned source: its metadata prefix, manifest row and place."""

    def __init__(
        self,
        label: str,
        prefix: str,
        manifest_name: str,
        destination: str,
        series: str | None,
        nested: tuple[str, ...] = (),
    ) -> None:
        self.label = label
        self.prefix = prefix
        self.manifest_name = manifest_name
        self.destination = destination
        self.series = series
        # A destination inside this checkout is another source's.
        self.nested = nested


SOURCES = (
    Source("TalkBack", "TALKBACK", "talkback", "external/talkback",
           "dependencies/patches/talkback"),
    Source("eSpeak NG", "ESPEAK_NG", "espeak-ng", "external/espeak-ng",
           "dependencies/patches/espeak-ng", nested=("sonic",)),
    Source("libsonic", "SONIC", "sonic", "external/espeak-ng/sonic", None),
)
MANIFEST = "dependencies/manifest.tsv"
MANIFEST_PRODUCT = "andarium-accessibility"


def required_metadata() -> set[str]:
    keys = {"ACCESSIBILITY_SOURCE_SCHEMA"}
    for source in SOURCES:
        fields = ["UPSTREAM_REPOSITORY", "UPSTREAM_RELEASE", "UPSTREAM_COMMIT",
                  "UPSTREAM_TREE", "DESTINATION"]
        if source.series is not None:
            fields += ["SERIES", "PATCH_BRANCH"]
        keys |= {f"{source.prefix}_{field}" for field in fields}
    return keys


def run(*command: str, cwd: Path | None = None) -> str:
    return _run(*command, cwd=cwd, error=VerificationError)


def parse_metadata(path: Path) -> dict[str, str]:
    return pinned_source.read_metadata(
        path, required_metadata(), "accessibility source", VerificationError
    )


def manifest_rows(repository_root: Path) -> dict[str, list[str]]:
    rows: dict[str, list[str]] = {}
    path = repository_root / MANIFEST
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 7:
            raise VerificationError(f"{path}:{number}: malformed row")
        rows[fields[0]] = fields
    return rows


def validate_metadata(
    values: dict[str, str], repository_root: Path
) -> dict[str, patch_series.Series]:
    """Check the pins; return the series each pinned source replays."""
    if values["ACCESSIBILITY_SOURCE_SCHEMA"] != "1":
        raise VerificationError("unsupported accessibility source schema")
    rows = manifest_rows(repository_root)
    series: dict[str, patch_series.Series] = {}
    for source in SOURCES:
        label, prefix = source.label, source.prefix
        if values[f"{prefix}_DESTINATION"] != source.destination:
            raise VerificationError(f"{label} destination is not {source.destination}")
        pinned_source.check_object_ids(
            values, (f"{prefix}_UPSTREAM_COMMIT", f"{prefix}_UPSTREAM_TREE"),
            VerificationError,
        )
        row = rows.get(source.manifest_name)
        if row is None:
            raise VerificationError(f"{MANIFEST} has no {source.manifest_name} row")
        _, release, product, repository, commit, subdirectory, series_row = row
        if product != MANIFEST_PRODUCT or subdirectory != ".":
            raise VerificationError(f"the {source.manifest_name} row is not a source of {MANIFEST_PRODUCT}")
        if (
            values[f"{prefix}_UPSTREAM_REPOSITORY"] != repository
            or values[f"{prefix}_UPSTREAM_RELEASE"] != release
            or values[f"{prefix}_UPSTREAM_COMMIT"] != commit
        ):
            raise VerificationError(
                f"the pinned {label} source differs from {MANIFEST}: pin one commit in both"
            )
        if source.series is None:
            if series_row != "-":
                raise VerificationError(f"{label} is pinned unpatched but {MANIFEST} names a series")
            continue
        if values[f"{prefix}_SERIES"] != source.series or series_row != (
            f"patches/{source.manifest_name}/series.tsv"
        ):
            raise VerificationError(f"the {label} series moved")
        pinned_source.check_branch_name(
            values[f"{prefix}_PATCH_BRANCH"], label, VerificationError
        )
        try:
            replay = patch_series.read_series(repository_root / source.series)
        except patch_series.SeriesError as error:
            raise VerificationError(f"{label} series: {error}") from error
        if not replay.names or replay.tree is None:
            raise VerificationError(f"{label} series is empty or records no tree")
        series[source.prefix] = replay
    return series


def verify_checkout(
    aosp_source: Path,
    values: dict[str, str],
    source: Source,
    series: patch_series.Series | None,
) -> None:
    label, prefix = source.label, source.prefix
    checkout = pinned_source.open_checkout(
        aosp_source, values, prefix, label, VerificationError, source.nested
    )
    commit = values[f"{prefix}_UPSTREAM_COMMIT"]
    tree = values[f"{prefix}_UPSTREAM_TREE"]
    branch = run("git", "branch", "--show-current", cwd=checkout)
    if series is None:
        if branch:
            raise VerificationError(f"the materialized {label} checkout must be detached at its pin")
        if run("git", "rev-parse", "HEAD", cwd=checkout) != commit:
            raise VerificationError(f"the materialized {label} checkout is not at the pinned commit")
        if run("git", "rev-parse", "HEAD^{tree}", cwd=checkout) != tree:
            raise VerificationError(f"the pinned {label} tree differs")
        return
    if branch != values[f"{prefix}_PATCH_BRANCH"]:
        raise VerificationError(
            f"the materialized {label} checkout is the maintained topic branch "
            f"and must be on {prefix}_PATCH_BRANCH"
        )
    if run("git", "rev-parse", "HEAD^{tree}", cwd=checkout) != series.tree:
        raise VerificationError(f"materialized {label} final tree differs from its series")
    count = len(series.names)
    base = run("git", "rev-parse", f"HEAD~{count}", cwd=checkout)
    if base != commit:
        raise VerificationError(f"the pinned upstream {label} commit is not the series base")
    if run("git", "rev-parse", f"{base}^{{tree}}", cwd=checkout) != tree:
        raise VerificationError(f"the pinned upstream {label} tree differs")
    history = run("git", "rev-list", "--parents", f"{base}..HEAD", cwd=checkout).splitlines()
    if len(history) != count or any(len(row.split()) != 2 for row in history):
        raise VerificationError(f"materialized {label} topic is not a clean linear series")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify the pinned TalkBack, eSpeak NG and libsonic sources"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path)
    parser.add_argument(
        "--only",
        choices=[source.manifest_name for source in SOURCES],
        help="verify the checkout of one source only (while the others are being materialized)",
    )
    args = parser.parse_args()

    try:
        root = args.android_root.resolve()
        metadata_path = root / "third_party/accessibility/metadata.env"
        if not metadata_path.is_file():
            raise VerificationError("accessibility source metadata is missing")
        values = parse_metadata(metadata_path)
        series = validate_metadata(values, root.parents[1])
        if args.aosp_source is not None:
            for source in SOURCES:
                if args.only in (None, source.manifest_name):
                    verify_checkout(
                        args.aosp_source.resolve(), values, source, series.get(source.prefix)
                    )
    except (OSError, UnicodeError, VerificationError) as error:
        raise SystemExit(f"verify-accessibility-source.py: {error}") from error

    suffix = ""
    if args.aosp_source is not None:
        suffix = " and materialized checkouts" if args.only is None else f" and the {args.only} checkout"
    print(f"verified pinned accessibility sources{suffix}")


if __name__ == "__main__":
    main()
