#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The String Catalogs of Andarium: check them, and bring them up to date with the code.

Andarium is localized in Simplified Chinese (zh-Hans), Traditional Chinese
(zh-Hant) and Japanese (ja). English is the development language and its
text is the key. The strings live in:

  Andarium/Localization/Localizable.xcstrings   the application's screens
  Andarium/Localization/InfoPlist.xcstrings     the permission explanations
  Andarium/Localization/WhatsNew.xcstrings      the release notes, Shared/WhatsNew.json
  Andarium/Localization/JIT.xcstrings           the JIT texts (table "JIT")
  Andarium/Localization/FollowX.xcstrings       the text of Follow developer on X (table "FollowX"), carried by the builds that name a handle only
  Andarium/Localization/AppShortcuts.xcstrings  the phrases Siri and Search understand for Add to Home Screen (table "AppShortcuts")
  Andarium/AndariumCore/Sources/AndariumCore/Resources/Localizable.xcstrings
                                                the words the model says
  GuestHost/Core/Sources/GuestHostCore/Resources/Localizable.xcstrings
                                                the words both applications say the same way (the update notice)

    localization.py check [--root DIR]
    localization.py sync --stringsdata DIR [--stringsdata DIR ...] [--check] [--root DIR]

`check` fails on anything a person would see as a defect in a translation: a
key with no value, or a value that is not marked translated, in a language; a
stale key; a format specifier or plural category that does not fit the source
or the language; an unescaped `%`; a Google name, but for the notice that
Android has no Google apps, which names Google and Google Play services (below);
a leaf (a plural form and an Info.plist value included) left as one of the
English forms of its key, or
different from one only in case, without the key being on the list in
Localization/english-only.json; a word the glossary retired (the guest is
虚拟机 and 虛擬機器, never 客户机 or 客體; Chinese says 你, never 您); a space
between Chinese or Japanese text and a Latin word or number (iOS writes none);
a half-width question mark, exclamation mark or comma after Chinese or
Japanese text; an Info.plist permission text, or a note of either What's New
file, without a catalog entry. A key is its English text, or a name with the English
text as its `en` value.

The list in english-only.json records decisions, not a census: `identical`
names the keys a catalog translates to their own English text on purpose, and
`english` names texts that stay English in the code and why. A string that is
nowhere on it and is not translated is found by the pseudo-language run
(Andarium/README.md#languages), which no list can replace.

`sync` is Xcode's catalog update, for a build that did not run in the editor:
it merges the strings the Swift compiler extracted (`*.stringsdata`, found
under each DIR after a build with `SWIFT_EMIT_LOC_STRINGS`) into the
catalogs, adding a new key in the state `new` and marking a key no code uses
`stale`. Give it the output of every target that shares a catalog. With
`--check` it changes nothing and fails when a catalog would change.

Follow developer on X (Andarium/README.md#follow-developer-on-x) has its own
FollowX table, built only where the build names a handle: a check needs
`FOLLOW_X_HANDLE=example_handle` on the `xcodebuild` line and fails on a build
that extracted no string of the table.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

LANGUAGES = ("ja", "zh-Hans", "zh-Hant")
SOURCE_LANGUAGE = "en"
# The plural categories of each language (CLDR): Japanese and Chinese have only "other".
PLURAL_CATEGORIES = {
    "en": {"one", "other"},
    "ja": {"other"},
    "zh-Hans": {"other"},
    "zh-Hant": {"other"},
}

CORE_CATALOG = "Andarium/AndariumCore/Sources/AndariumCore/Resources/Localizable.xcstrings"
GUEST_HOST_CATALOG = "GuestHost/Core/Sources/GuestHostCore/Resources/Localizable.xcstrings"
CATALOGS = {
    "Localizable": "Andarium/Localization/Localizable.xcstrings",
    "InfoPlist": "Andarium/Localization/InfoPlist.xcstrings",
    "WhatsNew": "Andarium/Localization/WhatsNew.xcstrings",
    "JIT": "Andarium/Localization/JIT.xcstrings",
    "FollowX": "Andarium/Localization/FollowX.xcstrings",
    "AppShortcuts": "Andarium/Localization/AppShortcuts.xcstrings",
    "AndariumCore": CORE_CATALOG,
    "GuestHostCore": GUEST_HOST_CATALOG,
}
ENGLISH_ONLY = "Andarium/Localization/english-only.json"
# The release notes the What's New table holds.
WHATS_NEW = "Andarium/Andarium/Shared/WhatsNew.json"
PROJECT = "Andarium/Andarium.xcodeproj/project.xcproj"
# The target of the application: its settings name the permission texts.
APPLICATION_TARGET = "Andarium"

# The tables of the features that are compiled in only where the build names their value, and how a development
# build names it.
FEATURE_TABLES = {
    "FollowX": "FOLLOW_X_HANDLE=example_handle to compile Follow developer on X in",
}

# Where the compiler-extracted strings of each source tree belong.
SYNC_SOURCES = {
    "Localizable": ("Andarium/Andarium/", "GuestHost/Sources/"),
    "JIT": ("Andarium/Andarium/",),
    "FollowX": ("Andarium/Andarium/",),
    "AppShortcuts": ("Andarium/Andarium/",),
    "AndariumCore": ("Andarium/AndariumCore/Sources/AndariumCore/",),
    "GuestHostCore": ("GuestHost/Core/Sources/GuestHostCore/",),
}

# Names the interface never carries. A guest's apps are the guest's own; the
# words of Andarium do not name another company's products.
GOOGLE_NAMES = re.compile(
    r"google|gboard|gmail|youtube|play\s*(?:store|services|protect)|\bgms\b|\bpixel\b|chromecast|"
    r"グーグル|ググ|谷歌|古狗",
    re.IGNORECASE,
)

# The one exception to that rule: the notice that Android has no Google apps, on
# purpose (the boot screen's panel, Andarium/README.md#runtime) names Google
# and Google Play services. Only
# these keys may, and only by these two names, in Latin letters in every
# language: "Google" not followed by another name ("Google Maps", "GoogleTV")
# and "Google Play" followed by the word for services in the language. The rule
# runs on what is left, so a Play Store, a Gmail, a "Google Play Store", a
# "Google Play商店" or a 谷歌 in them is still a problem.
GOOGLE_NOTICE_KEYS = {
    "Localizable": {
        "No Google apps",
        "Google licenses its apps and Google Play services only to makers of certified devices. "
        "Install apps from files you have; many work without them.",
    },
}
GOOGLE_NOTICE_NAMES = re.compile(r"Google Play(?: services|服务|服務|開発者サービス)|Google(?! ?[A-Z])")


def names_google(text: str, notice: bool) -> bool:
    """Whether `text` names a Google product; a key of the notice may name Google and Google Play services."""
    if notice:
        text = GOOGLE_NOTICE_NAMES.sub("", text)
    return bool(GOOGLE_NAMES.search(text))


# Words the glossary retired, by language, and the person's pronoun: the app says 你, as iOS does.
RETIRED_TERMS = {
    "zh-Hans": ("客户机", "您"),
    "zh-Hant": ("客體", "您"),
    "ja": ("ゲスト",),
}
# What an Info.plist catalog may also hold: the names of the application, which no language translates.
INFO_PLIST_NAMES = ("CFBundleName", "CFBundleDisplayName")
CJK = r"\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff"
CJK_LATIN_SPACE = re.compile(rf"[{CJK}] +[A-Za-z0-9%]|[A-Za-z0-9%@] +[{CJK}]")
CJK_ASCII_PUNCTUATION = re.compile(rf"[{CJK}][?!,;]")

SPECIFIER = re.compile(
    r"%(?:(?P<position>\d+)\$)?[-+ 0#]*(?:\d+|\*)?(?:\.\d+)?(?P<conversion>(?:hh|h|ll|l|q|L|z|t|j)?[@diuoxXfFeEgGcsSpaA])"
)


class Problems:
    def __init__(self) -> None:
        self.items: list[str] = []

    def add(self, where: str, message: str) -> None:
        self.items.append(f"{where}: {message}")


def read_json(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def specifiers(text: str) -> tuple[list[str], list[int] | None]:
    """The conversions of a format string in the order the arguments are given, and the explicit positions, if any.
    `%%` is not a conversion."""
    # A substitution (`%#@name@`) stands for another string of the entry, which is checked on its own.
    stripped = re.sub(r"%#@\w+@", "", text.replace("%%", ""))
    found = [match for match in SPECIFIER.finditer(stripped)]
    conversions = [match["conversion"] for match in found]
    positions = [int(match["position"]) for match in found if match["position"]]
    if positions and len(positions) != len(found):
        return conversions, [0]  # mixed positional and plain: never valid
    return conversions, positions or None


def format_problem(source: str, translation: str) -> str | None:
    """Why `translation` cannot be formatted with the arguments of `source`; None when it can."""
    want, _ = specifiers(source)
    have, positions = specifiers(translation)
    if positions == [0]:
        return "mixes positional and plain format specifiers"
    if positions is not None:
        if sorted(positions) != list(range(1, len(have) + 1)):
            return f"its positions {sorted(positions)} are not 1 to {len(have)}"
        ordered = [conversion for _, conversion in sorted(zip(positions, have))]
        have = ordered
        if sorted(have) != sorted(want):
            return f"its specifiers {have} are not those of the source {want}"
        return None
    if have != want and sorted(have) != sorted(want):
        return f"its specifiers {have} are not those of the source {want}"
    if have != want:
        return f"its specifiers {have} are in another order than the source's {want}; give them positions (%1$@)"
    return None


def leaves(localization: dict, path: str = ""):
    """Every string unit of one language's localization: (where, unit, plural category or None)."""
    if "stringUnit" in localization:
        yield path, localization["stringUnit"], None
    # An App Shortcut's phrases: one unit for each, in the state of the set.
    string_set = localization.get("stringSet", {})
    for index, value in enumerate(string_set.get("values", []), start=1):
        yield f"/phrase {index}", {"state": string_set.get("state"), "value": value}, None
    for name, substitution in localization.get("substitutions", {}).items():
        yield from leaves(substitution, f"{path}/substitution {name}")
    for kind, cases in localization.get("variations", {}).items():
        for case, inner in cases.items():
            for where, unit, category in leaves(inner, f"{path}/{kind} {case}"):
                yield where, unit, case if kind == "plural" and category is None else category


def plural_cases(localization: dict) -> set[str]:
    return set(localization.get("variations", {}).get("plural", {}))


def lone_percent(text: str) -> bool:
    """A `%` that is neither part of a format specifier nor `%%`: formatting the string would misread it."""
    stripped = re.sub(r"%#@\w+@", "", text.replace("%%", ""))
    stripped = SPECIFIER.sub("", stripped)
    return "%" in stripped


def source_text(key: str, english: dict | None) -> str:
    """The text a translation's specifiers are checked against: the key, or, for a named key, its English value."""
    if english is not None:
        values = [unit.get("value", "") for _, unit, _ in leaves(english)]
        if values and not SPECIFIER.search(key):
            return values[-1]
    return key


def english_forms(key: str, english: dict | None) -> list[str]:
    """Every form the English text takes: the key and each leaf of its English localization."""
    forms = [key]
    if english is not None:
        forms += [unit.get("value", "") for _, unit, _ in leaves(english)]
    return [form for form in dict.fromkeys(forms) if form.strip()]


def has_words(text: str) -> bool:
    """Whether a text has at least one run of three Latin letters outside its format specifiers."""
    return bool(re.search(r"[A-Za-z]{3}", SPECIFIER.sub("", text)))


def check_value(tag: str, language: str, key: str, source: str, forms: list[str], category: str | None,
                value: str, allowed_identical: set[str], used_identical: set[str], problems: Problems,
                notice: bool = False) -> None:
    if category is not None and category not in PLURAL_CATEGORIES[language]:
        problems.add(tag, f"uses the plural category {category!r}, which {language} does not have")
    if names_google(value, notice):
        problems.add(tag, "names a Google product; no string of Andarium does")
    problem = format_problem(source, value)
    if problem:
        problems.add(tag, problem)
    if lone_percent(value):
        problems.add(tag, "has a `%` that is not a format specifier; write `%%`")
    for term in RETIRED_TERMS[language]:
        if term in value:
            problems.add(tag, f"says {term}, which the glossary retired")
    if CJK_LATIN_SPACE.search(value):
        problems.add(tag, "has a space between Chinese or Japanese text and a Latin word or number; iOS writes none")
    if CJK_ASCII_PUNCTUATION.search(value):
        problems.add(tag, "has a half-width ? ! , or ; after Chinese or Japanese text; use the full-width mark")
    stripped = value.strip()
    for form in forms:
        if stripped == form.strip():
            if key in allowed_identical:
                used_identical.add(key)
            elif has_words(form):
                problems.add(tag, f"is an English form of the key; translate it, or list the key in {ENGLISH_ONLY}")
            break
        if has_words(form) and stripped.lower() == form.strip().lower():
            problems.add(tag, f"differs from the English text {form!r} only in case")
            break


def check_catalog(name: str, path: pathlib.Path, english_only: dict, problems: Problems) -> set[str]:
    """One catalog's rules; the keys it holds."""
    where = path.name if name == "Localizable" else f"{name} ({path.name})"
    try:
        catalog = read_json(path)
    except (OSError, ValueError) as error:
        problems.add(where, f"cannot be read: {error}")
        return set()
    if catalog.get("sourceLanguage") != SOURCE_LANGUAGE:
        problems.add(where, f"the source language is {catalog.get('sourceLanguage')!r}, not {SOURCE_LANGUAGE!r}")
    allowed_identical = {entry["key"] for entry in english_only.get("identical", []) if entry.get("catalog") == name}
    used_identical: set[str] = set()
    keys = set(catalog.get("strings", {}))
    for key, entry in catalog.get("strings", {}).items():
        label = f"{where}: {key!r}"
        notice = key in GOOGLE_NOTICE_KEYS.get(name, ())
        if names_google(key, notice):
            problems.add(label, "names a Google product; no string of Andarium does")
        if lone_percent(key):
            problems.add(label, "has a `%` that is not a format specifier; write `%%`")
        if entry.get("extractionState") == "stale":
            problems.add(label, "is stale: no code uses it")
        if name == "InfoPlist" and key in INFO_PLIST_NAMES:
            continue  # the application's name: no language translates it
        if entry.get("shouldTranslate") is False:
            if key not in allowed_identical:
                problems.add(label, f"is not translated but is not on the list in {ENGLISH_ONLY}")
            used_identical.add(key)
            continue
        localizations = entry.get("localizations", {})
        english = localizations.get(SOURCE_LANGUAGE)
        if english is not None:
            for place, unit, category in leaves(english):
                if category is not None and category not in PLURAL_CATEGORIES[SOURCE_LANGUAGE]:
                    problems.add(label, f"English has the plural category {category!r}, which English does not use")
                problem = format_problem(key, unit.get("value", ""))
                if problem and SPECIFIER.search(key):
                    problems.add(f"{label} (en{place})", problem)
        source = source_text(key, english)
        forms = english_forms(key, english)
        for language in LANGUAGES:
            localization = localizations.get(language)
            if localization is None:
                problems.add(label, f"has no {language} translation")
                continue
            values = list(leaves(localization))
            if not values:
                problems.add(label, f"{language} has no value")
            for place, unit, category in values:
                tag = f"{label} ({language}{place})"
                value = unit.get("value", "")
                if unit.get("state") != "translated":
                    problems.add(tag, f"is in the state {unit.get('state')!r}, not 'translated'")
                if not value.strip():
                    problems.add(tag, "is empty")
                    continue
                check_value(tag, language, key, source, forms, category, value, allowed_identical, used_identical, problems,
                            notice)
            cases = plural_cases(localization)
            if cases and "other" not in cases:
                problems.add(f"{label} ({language})", "has plural variations without 'other'")
    for key in sorted(allowed_identical - used_identical - keys):
        problems.add(where, f"the list in {ENGLISH_ONLY} names {key!r}, which is not a key of this catalog")
    return keys


def check_english_only(root: pathlib.Path, problems: Problems) -> dict:
    path = root / ENGLISH_ONLY
    if not path.is_file():
        problems.add(ENGLISH_ONLY, "is missing")
        return {}
    try:
        data = read_json(path)
    except ValueError as error:
        problems.add(ENGLISH_ONLY, f"cannot be read: {error}")
        return {}
    for entry in data.get("identical", []):
        if not {"catalog", "key", "reason"} <= set(entry) or entry["catalog"] not in CATALOGS:
            problems.add(ENGLISH_ONLY, f"an 'identical' entry needs a catalog (one of {sorted(CATALOGS)}), a key and a reason: {entry}")
    for entry in data.get("english", []):
        if not {"text", "file", "reason"} <= set(entry):
            problems.add(ENGLISH_ONLY, f"an 'english' entry needs text, file and reason: {entry}")
    return data


def check_english_texts(root: pathlib.Path, english_only: dict, all_keys: dict[str, set[str]], problems: Problems) -> None:
    """The user-visible strings decided to stay English: each is still in its file, and none is translated.
    The notes' table holds the release notes' texts, which no code looks up, so it translates none of them."""
    translated = set().union(*(keys for name, keys in all_keys.items() if name != "WhatsNew")) if all_keys else set()
    for entry in english_only.get("english", []):
        text = entry.get("text", "")
        source = root / entry.get("file", "")
        if not source.is_file():
            problems.add(ENGLISH_ONLY, f"{entry.get('file')!r} for {text!r} does not exist")
        elif text not in source.read_text(encoding="utf-8", errors="replace"):
            problems.add(ENGLISH_ONLY, f"{text!r} is no longer in {entry['file']}; remove the entry")
        if text in translated:
            problems.add(ENGLISH_ONLY, f"{text!r} is listed as staying English, and a catalog translates it")


def application_permission_texts(root: pathlib.Path) -> dict[str, str]:
    """INFOPLIST_KEY_NS*UsageDescription of the application target, from the project."""
    path = root / PROJECT
    if not path.is_file():
        return {}
    text = path.read_text(encoding="utf-8")
    # A target starts at its name and its id; the next target ends its settings.
    markers = [(match.start(), match["name"]) for match in re.finditer(r'"name": "(?P<name>\w+)",\s*"id": "', text)]
    block = ""
    for index, (start, name) in enumerate(markers):
        if name == APPLICATION_TARGET:
            end = markers[index + 1][0] if index + 1 < len(markers) else len(text)
            block = text[start:end]
    return {
        match["key"]: json.loads(f'"{match["value"]}"')
        for match in re.finditer(r'"INFOPLIST_KEY_(?P<key>NS\w+UsageDescription)": "(?P<value>(?:[^"\\]|\\.)*)"', block)
    }


def check_info_plist(root: pathlib.Path, info_keys: set[str], problems: Problems) -> None:
    expected = application_permission_texts(root)
    path = root / CATALOGS["InfoPlist"]
    if not path.is_file():
        return
    catalog = read_json(path)
    for key, value in expected.items():
        entry = catalog.get("strings", {}).get(key)
        if entry is None:
            problems.add("InfoPlist.xcstrings", f"has no entry for {key}, the permission text of the project")
            continue
        english = entry.get("localizations", {}).get(SOURCE_LANGUAGE, {}).get("stringUnit", {}).get("value")
        if english is not None and english != value:
            problems.add("InfoPlist.xcstrings", f"{key} differs from the project's text; the catalog's English is {english!r}")
    for key in sorted(info_keys - set(expected) - set(INFO_PLIST_NAMES)):
        problems.add("InfoPlist.xcstrings", f"{key} is not a permission text of the {APPLICATION_TARGET} target")


def whats_new_texts(root: pathlib.Path, relative: str) -> set[str]:
    """The notes of a file; none if it is missing (`check_whats_new` says so)."""
    path = root / relative
    if not path.is_file():
        return set()
    texts: set[str] = set()
    for release in read_json(path):
        for entry in release.get("entries", []):
            for field in ("title", "text"):
                if entry.get(field):
                    texts.add(entry[field])
            texts.update(entry.get("points", []))
    return texts


def check_whats_new(root: pathlib.Path, keys: set[str], problems: Problems) -> None:
    """The table holds the notes: each has an entry, and every entry is a note."""
    if not (root / WHATS_NEW).is_file():
        problems.add(WHATS_NEW, "is missing")
    texts = whats_new_texts(root, WHATS_NEW)
    for text in sorted(texts - keys):
        problems.add("WhatsNew.xcstrings", f"has no entry for the note {text!r} of {WHATS_NEW}")
    for key in sorted(keys - texts):
        problems.add("WhatsNew.xcstrings", f"{key!r} is no note of {WHATS_NEW}")


def check(root: pathlib.Path) -> list[str]:
    problems = Problems()
    english_only = check_english_only(root, problems)
    all_keys: dict[str, set[str]] = {}
    for name, relative in CATALOGS.items():
        path = root / relative
        if not path.is_file():
            problems.add(relative, "is missing")
            continue
        all_keys[name] = check_catalog(name, path, english_only, problems)
    check_info_plist(root, all_keys.get("InfoPlist", set()), problems)
    check_whats_new(root, all_keys.get("WhatsNew", set()), problems)
    check_english_texts(root, english_only, all_keys, problems)
    return problems.items


# MARK: sync


def stringsdata_files(directories: list[pathlib.Path]) -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for directory in directories:
        files.extend(sorted(directory.rglob("*.stringsdata")))
    return files


def partition(files: list[pathlib.Path], root: pathlib.Path) -> dict[str, list[pathlib.Path]]:
    """The extracted files by the catalog their source belongs to."""
    parts: dict[str, list[pathlib.Path]] = {name: [] for name in SYNC_SOURCES}
    for file in files:
        try:
            source = json.loads(file.read_text(encoding="utf-8")).get("source", "")
        except (OSError, ValueError):
            continue
        try:
            relative = str(pathlib.Path(source).resolve().relative_to(root.resolve()))
        except ValueError:
            continue
        for name, prefixes in SYNC_SOURCES.items():
            if any(relative.startswith(prefix) for prefix in prefixes):
                parts[name].append(file)
    return parts


def extracted_tables(files: list[pathlib.Path]) -> set[str]:
    """The tables the compiler extracted strings of."""
    tables: set[str] = set()
    for file in files:
        try:
            data = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        tables.update(name for name, entries in data.get("tables", {}).items() if entries)
    return tables


def sync(root: pathlib.Path, directories: list[pathlib.Path], check_only: bool) -> list[str]:
    files = stringsdata_files(directories)
    parts = partition(files, root)
    problems = Problems()
    if not any(parts.values()):
        problems.add("sync", "no .stringsdata of the application or the package found: build with SWIFT_EMIT_LOC_STRINGS=YES first")
        return problems.items
    # A feature is compiled in where the build names its receiver, identifiers or page: without its strings the
    # catalog cannot be compared.
    extracted = extracted_tables(files)
    for table, arrangement in FEATURE_TABLES.items():
        if table not in extracted:
            parts[table] = []
            problems.add(table, f"has no extracted strings: build with {arrangement}")
    with tempfile.TemporaryDirectory() as scratch:
        for name, data in parts.items():
            if not data:
                continue
            catalog = root / CATALOGS[name]
            target = pathlib.Path(scratch) / catalog.name if check_only else catalog
            if check_only:
                shutil.copy2(catalog, target)
            run = subprocess.run(
                ["xcrun", "xcstringstool", "sync", str(target), "--stringsdata", *map(str, data)],
                check=False, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=300,
            )
            if run.returncode != 0:
                problems.add(name, f"xcstringstool sync failed: {run.stderr.strip()}")
                continue
            if check_only and read_json(target) != read_json(catalog):
                before, after = read_json(catalog)["strings"], read_json(target)["strings"]
                added = sorted(set(after) - set(before))
                removed = sorted(set(before) - set(after))
                changed = sorted(key for key in set(before) & set(after) if before[key] != after[key])
                problems.add(
                    name,
                    f"is out of step with the code: {len(added)} to add {added[:5]}, {len(removed)} to remove {removed[:5]}, "
                    f"{len(changed)} changed {changed[:5]}",
                )
    return problems.items


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2],
                        help="the repository (default: the one this script is in)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check", parents=[common], help="check the catalogs")
    sync_parser = commands.add_parser("sync", parents=[common], help="merge the compiler's extracted strings into the catalogs")
    sync_parser.add_argument("--stringsdata", type=pathlib.Path, action="append", required=True, metavar="DIR")
    sync_parser.add_argument("--check", action="store_true", help="change nothing; fail when a catalog would change")
    args = parser.parse_args(argv)
    if args.command == "check":
        problems = check(args.root)
        what = "the String Catalogs are complete"
    else:
        problems = sync(args.root, args.stringsdata, args.check)
        what = "the String Catalogs match the code" if args.check else "the String Catalogs are updated"
    for problem in problems:
        print(f"localization.py: {problem}", file=sys.stderr)
    if problems:
        print(f"localization.py: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"localization.py: {what}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
