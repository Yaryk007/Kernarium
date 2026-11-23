#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The pinned sources of TalkBack, eSpeak NG and libsonic, and their checkouts.

verify-accessibility-source.py agrees the metadata with dependencies/manifest.tsv
and the series each source replays, and verifies the materialized checkouts of
an Android source tree: each at its pin, TalkBack and eSpeak NG on the topic
branch at exactly the tree their series records, libsonic detached inside the
eSpeak NG checkout.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from source_fixture import commit, parse_env, run

BRANCH = "kernarium-soong"
SOURCES = {
    "TALKBACK": ("talkback", "https://example.invalid/talkback.git", "external/talkback"),
    "ESPEAK_NG": ("espeak-ng", "https://example.invalid/espeak-ng.git", "external/espeak-ng"),
    "SONIC": ("sonic", "https://example.invalid/sonic.git", "external/espeak-ng/sonic"),
}


def export(scripts: Path, source: Path, base: str, directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    run(
        sys.executable, "-B", str(scripts / "patch_series.py"), "export",
        "--source", str(source), "--base", base, "--head", "HEAD",
        "--series-dir", str(directory), "--tree",
    )


def topic(root: Path, scripts: Path, name: str, patches: int) -> dict[str, str]:
    """A fixture upstream with `patches` maintained commits, and its series."""
    source = root / "topics" / name
    run("git", "init", "-q", str(source))
    run("git", "switch", "-q", "-c", BRANCH, cwd=source)
    commit(source, "README", f"{name}\n", "fixture upstream", 1700000000)
    base = run("git", "rev-parse", "HEAD", cwd=source)
    tree = run("git", "rev-parse", "HEAD^{tree}", cwd=source)
    for number in range(patches):
        commit(source, f"port{number}.txt", f"{number}\n", f"Fixture patch {number}", 1700000010 + number)
    if patches:
        export(scripts, source, base, root / "dependencies/patches" / name)
    return {
        "source": str(source), "commit": base, "tree": tree,
        "final": run("git", "rev-parse", "HEAD^{tree}", cwd=source),
        "head": run("git", "rev-parse", "HEAD", cwd=source),
    }


def write_fixture(root: Path, source_root: Path) -> dict[str, dict[str, str]]:
    scripts = root / "scripts"
    scripts.mkdir()
    shutil.copy2(source_root / "scripts/patch_series.py", scripts / "patch_series.py")
    artifacts = root / "kernel/common/artifacts"
    artifacts.mkdir(parents=True)
    shutil.copy2(source_root / "kernel/common/artifacts/contract_checks.py", artifacts / "contract_checks.py")
    tools = root / "android/lineage-24/tools"
    tools.mkdir(parents=True)
    for name in ("verify-accessibility-source.py", "pinned_source.py"):
        shutil.copy2(source_root / "android/lineage-24/tools" / name, tools / name)
    (tools / "verify-accessibility-source.py").chmod(0o755)

    pins = {
        "TALKBACK": topic(root, scripts, "talkback", 2),
        "ESPEAK_NG": topic(root, scripts, "espeak-ng", 1),
        "SONIC": topic(root, scripts, "sonic", 0),
    }
    metadata = ["# SPDX-License-Identifier: Apache-2.0", "ACCESSIBILITY_SOURCE_SCHEMA=1"]
    manifest = ["# SPDX-License-Identifier: GPL-2.0-or-later"]
    for prefix, (name, repository, destination) in SOURCES.items():
        pin = pins[prefix]
        metadata += [
            f"{prefix}_UPSTREAM_REPOSITORY='{repository}'",
            f"{prefix}_UPSTREAM_RELEASE='fixture-{name}'",
            f"{prefix}_UPSTREAM_COMMIT='{pin['commit']}'",
            f"{prefix}_UPSTREAM_TREE='{pin['tree']}'",
        ]
        series = "-"
        if prefix != "SONIC":
            series = f"patches/{name}/series.tsv"
            metadata += [
                f"{prefix}_SERIES='dependencies/patches/{name}'",
                f"{prefix}_PATCH_BRANCH='{BRANCH}'",
            ]
        metadata.append(f"{prefix}_DESTINATION='{destination}'")
        manifest.append(
            f"{name}\tfixture-{name}\tandarium-accessibility\t{repository}\t{pin['commit']}\t.\t{series}"
        )
    directory = root / "android/lineage-24/third_party/accessibility"
    directory.mkdir(parents=True)
    (directory / "metadata.env").write_text("\n".join(metadata) + "\n")
    (root / "dependencies/manifest.tsv").write_text("\n".join(manifest) + "\n")
    return pins


def materialize(aosp: Path, pins: dict[str, dict[str, str]]) -> None:
    (aosp / ".repo").mkdir(parents=True)
    for prefix in ("TALKBACK", "ESPEAK_NG", "SONIC"):
        name, repository, destination = SOURCES[prefix]
        pin = pins[prefix]
        checkout = aosp / destination
        checkout.parent.mkdir(parents=True, exist_ok=True)
        run("git", "clone", "-q", "-b", BRANCH, pin["source"], str(checkout))
        run("git", "remote", "set-url", "origin", repository, cwd=checkout)
        if prefix == "SONIC":
            run("git", "switch", "-q", "--detach", pin["commit"], cwd=checkout)
        else:
            run("git", "switch", "-q", BRANCH, cwd=checkout)


def refused(command: tuple[str, ...], expected: str, case: str) -> None:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode == 0 or expected not in result.stdout:
        raise SystemExit(f"{case} was not refused with '{expected}': {result.stdout.strip()}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    source_root = parser.parse_args().source_root.resolve()

    with tempfile.TemporaryDirectory(prefix="accessibility-source-") as temporary:
        root = Path(temporary)
        pins = write_fixture(root, source_root)
        android_root = root / "android/lineage-24"
        verify = (
            sys.executable, "-B", str(android_root / "tools/verify-accessibility-source.py"),
            "--android-root", str(android_root),
        )
        run(*verify)
        aosp = root / "aosp"
        materialize(aosp, pins)
        with_checkouts = (*verify, "--aosp-source", str(aosp))
        run(*with_checkouts)
        run(*with_checkouts, "--only", "talkback")

        metadata_path = android_root / "third_party/accessibility/metadata.env"
        metadata = metadata_path.read_text()
        values = parse_env(metadata_path)
        manifest_path = root / "dependencies/manifest.tsv"
        manifest = manifest_path.read_text()

        # The metadata and the manifest pin one commit per source.
        manifest_path.write_text(manifest.replace(pins["SONIC"]["commit"], "0" * 40))
        refused(verify, "differs from dependencies/manifest.tsv", "a manifest that pins another commit")
        manifest_path.write_text(manifest)
        metadata_path.write_text(metadata.replace(values["TALKBACK_UPSTREAM_COMMIT"], "f" * 40))
        refused(verify, "differs from dependencies/manifest.tsv", "a metadata pin the manifest does not follow")
        metadata_path.write_text(metadata.replace(values["SONIC_UPSTREAM_TREE"], "f" * 40))
        refused((*with_checkouts, "--only", "sonic"), "libsonic tree differs", "a pinned tree the checkout lacks")
        metadata_path.write_text(metadata.replace("'external/talkback'", "'external/elsewhere'"))
        refused(verify, "destination is not external/talkback", "a moved destination")
        metadata_path.write_text(metadata)

        # A series that moves under the materialized checkout is refused.
        series = root / "dependencies/patches/espeak-ng/series.tsv"
        original = series.read_bytes()
        series.write_bytes(original.replace(pins["ESPEAK_NG"]["final"].encode(), pins["ESPEAK_NG"]["tree"].encode()))
        refused(with_checkouts, "final tree differs from its series", "a series the checkout does not replay")
        series.write_bytes(original)
        stray = root / "dependencies/patches/talkback/stray.patch"
        stray.write_text("stray\n")
        refused(verify, "stray.patch", "a patch the series does not list")
        stray.unlink()

        # The checkouts are the topic branch, clean, with nothing else untracked.
        talkback = aosp / "external/talkback"
        run("git", "switch", "-q", "--detach", "HEAD", cwd=talkback)
        refused(with_checkouts, "must be on TALKBACK_PATCH_BRANCH", "a detached topic")
        run("git", "switch", "-q", BRANCH, cwd=talkback)
        (talkback / "dirty.txt").write_text("dirty\n")
        refused(with_checkouts, "TalkBack checkout is dirty", "a dirty checkout")
        (talkback / "dirty.txt").unlink()
        espeak = aosp / "external/espeak-ng"
        (espeak / "stray.txt").write_text("stray\n")
        refused(with_checkouts, "eSpeak NG checkout is dirty", "an untracked file beside libsonic")
        (espeak / "stray.txt").unlink()
        run("git", "reset", "-q", "--hard", "HEAD~1", cwd=talkback)
        refused(with_checkouts, "final tree differs from its series", "a checkout behind its series")
        run("git", "reset", "-q", "--hard", pins["TALKBACK"]["head"], cwd=talkback)
        sonic = aosp / "external/espeak-ng/sonic"
        run("git", "switch", "-q", BRANCH, cwd=sonic)
        refused(with_checkouts, "libsonic checkout must be detached at its pin", "libsonic on a branch")
        run("git", "switch", "-q", "--detach", pins["SONIC"]["commit"], cwd=sonic)
        run("git", "remote", "set-url", "origin", "https://example.invalid/other.git", cwd=sonic)
        refused(with_checkouts, "libsonic origin differs from metadata", "libsonic from another origin")
        run("git", "remote", "set-url", "origin", SOURCES["SONIC"][1], cwd=sonic)
        shutil.rmtree(sonic)
        refused(with_checkouts, "libsonic checkout is missing", "no libsonic checkout")
        run(*with_checkouts, "--only", "talkback")

    print("Android accessibility source contract passed")


if __name__ == "__main__":
    main()
