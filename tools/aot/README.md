# AOT rewriter

Build tool for the ahead-of-time component (`darwin/cpu/arm64/aot`). It
unpacks the partitions of an accepted Android release and rewrites the arm64
libraries named by [`libraries.txt`](libraries.txt) and the release's boot
image into one position-independent blob (`rewrite.py`), which
`darwin/cpu/arm64/aot/CMakeLists.txt` wraps in the signed
`UmlArm64Aot.framework`. The contract is in
[`docs/architecture.md`](../../docs/architecture.md#ahead-of-time-code); the
blob and the framework are generated and never committed.

- `release_artifact.py` builds the blob of an installed release into an
  artifact directory (`blob.bin`, `manifest.json`, `licences.json` and
  `artifact.json`, which binds them to the release ID, its manifest and its
  system partition, and records the SHA-256 of every file the rewrite is a
  function of), verifies one (`verify`, against a release with `--release`,
  against the descriptor with `--accepted`), prints the descriptor that accepts
  it (`describe`, with `--archives yes` once the archive configurations may
  carry it) and installs it below `.build/android/aot`
  (`install-release.py --aot-artifact` does the same with the release). A
  product build wraps the accepted artifact, or the one `UML_AOT_ARTIFACT`
  names, and checks it again at build time; a tree whose tools differ from the
  artifact's fails the check.

- `elf64.py` reads program headers and function ranges (`.eh_frame`, `.plt`
  and `.iplt`, and for a library that has unwind tables the sized function
  symbols of `.symtab`, `.dynsym` and `.gnu_debugdata`, which add the
  assembly the tables omit; for an OAT file, which has no unwind table, the
  function symbols of its `.gnu_debugdata`).
- `a64decode.py` identifies an instruction exactly from
  `tests/unit/arm64/a64_instruction_decode_partitions.tsv`.
- `asm.py` encodes the words the rewriter emits and resolves labels.
- `rewrite.py` classifies each word of each function (inline or step), emits
  the code, the per-instruction entry table, the SHA-256 of each text page
  (what binds the code to the guest bytes), the page digests and the digest
  hash. Every instruction it does not inline is left to the engine, so it
  never refuses for correctness.

- `extract_image.py` unpacks the partitions and APEX payloads the libraries
  come from; `licences.py` records, per library, the notice the image itself
  states (generated beside the framework, not tracked).
- `fixture.py` writes the small libraries of the differential test (a
  diagnostics build only): page-edge branches, loops that update one cell,
  and one function per instruction form; and a file shaped like an OAT file
  (no unwind table, symbols in `.gnu_debugdata`, methods that touch, direct
  branches that reach below the library).

Boot code: every `boot*.oat` of the release that has an executable segment is
translated whole, with no profile (an OAT file with no compiled method has no
code and is listed in the manifest's `skipped`). A `--library` path may end in
a pattern, which names every file it matches in name order. Every component of
a `--library` path, a directory before the last one as well, is resolved inside
the image as the guest would resolve it (an absolute link from the image's root,
`..` stopping there); a link never reaches a host file outside the unpacked
image. A named file that is not a regular file of the image is refused, and a
match that is not is listed in `skipped`. The OAT files of
one boot image are mapped next to each other and call each other by direct
branches, so a branch from a later file into an earlier one reaches below the
library's own first byte: its target is formed as a full address and leaves by
the absolute branch exit.

One path names one distinct file. A unit binds by the content of its pages,
so it serves every identical copy that another APEX or partition ships; the
build refuses a path listed twice, and two paths whose files are the same
bytes.

Tools: building an artifact, or a generating build (`UML_AOT_IMAGE`), needs a
Python 3 and an `fsck.erofs` that decodes Zstandard (`UML_AOT_FSCK_EROFS`,
`--fsck-erofs`), as the Android build produces; wrapping one needs neither. The
Homebrew `erofs-utils` is built without libzstd and cannot read the release
(CMake fails the configure step when the tool's `--help` does not list
`zstd`).

The blob's magic names its layout and the word after it the layout's version.
A change a runtime built for the old layout would misread changes the magic; an
addition it can ignore raises the version (`BLOB_VERSION`), and a runtime
accepts any version at or above the one it was built for. The header names the
release the blob was made from: its ID (offset 0x30), the SHA-256 of its
release manifest (0x90) and the SHA-256 of its system partition (0xB0), which
the runtime compares with the release it boots (version 3); `verify` holds all
three to `artifact.json`.

The entry table holds 16 bits per source word (the offset of the word's native
code, in words, from its page's anchor) and one signed 32-bit anchor per text
page, half the bytes of a 32-bit entry per word; the blob's magic is
`UMLAOT16`. An indirect branch and a refill spend four words more to reach the
native code than a table of 32-bit entries would. A page that holds the end of
a function of 32 KiB or more (its out-of-line stubs follow it, so its entries
can lie further apart than 16 bits reach) is a row of branches, one per word,
instead of entries. A library whose table cannot be formed (a page whose
entries are still further apart, or more than 32 MiB of text) is not in the
blob: the engine runs it, the manifest's `refused` lists it, and the
diagnostics report counts it (`libraries_refused`, from the header;
`BLOB_VERSION` 2 or later).

`UML_AOT_INSTANCE_CAPACITY` (a CMake cache value, 2048 by default) is the
number of library instances each executor thread's binder holds before it
replaces the oldest. The binder's tables (the instances, their index and 8
page-state slots an instance) are one mapping of fresh pages, so a place
nothing has used is not held in memory; an instance also holds one byte per
text page of its library. The blob does not depend on the capacity.

Entry points: `ctest -R '^arm64-aot-tools$'` for the encoders, decoder and
assembler, and `ctest -R '^arm64-aot-differential$'` (a configuration with
`UML_AOT_DIAGNOSTICS=ON`) for the rewritten libraries against the gadget
engine.

Licence: the tool is GPL-2.0-or-later. The libraries it rewrites keep their
own licences, and each artifact's `licences.json` records the notice every
library states in the image.
