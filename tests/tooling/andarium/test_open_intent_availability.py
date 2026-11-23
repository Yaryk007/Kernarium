#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shortcuts lists an Android action from iOS 27 only.

Before iOS 27 an app of Andarium's is no entity (`IPadAppHost.offersAddToHomeScreen`),
so the action would ask for an app that no query can give, and the Find action
the system lists for an entity and its query would list none. The declarations
say where each is offered, and the App Intents metadata the build extracts says
it to the system: the action, the automatic shortcut, the entity and its query
carry an `LNPlatformNameIOS` introduced version of 27.0 beside the wildcard
that keeps them on the other platforms (a Mac and a Vision Pro of iOS 27 run
the action by name). This reads both: the sources for the four declarations'
availability, and the metadata of a built application when `--bundle` names
one.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import re
import sys
import unittest

SOURCE = "Andarium/Andarium/Shared/AndroidAppIntents.swift"
DECLARATIONS = (
    "struct OpenAndroidAppIntent",
    "struct AndroidAppShortcuts",
    "struct AndroidAppEntity",
    "struct AndroidAppQuery",
)
ACTION = "OpenAndroidAppIntent"
ENTITY = "AndroidAppEntity"
QUERY = "AndroidAppQuery"
# The iOS release that introduces the action: exactly it, since a later one
# would leave that release without the action.
FIRST_VERSION = (27,)
GATE = re.compile(r"@available\(iOS 27\.0, \*\)")
WILDCARD = "LNPlatformNameWildcard"
IOS = "LNPlatformNameIOS"


def version(text: str) -> tuple[int, ...]:
    """The numbers of `text` without trailing zeros, so that 27, 27.0 and 27.0.0 are one."""
    numbers = [int(part) for part in text.split(".")]
    while numbers and numbers[-1] == 0:
        numbers.pop()
    return tuple(numbers)


def availability_problem(annotations: dict | None) -> str | None:
    """Why `annotations` leaves the entry on iOS before 27, or None."""
    if not annotations:
        return "has no availability annotations"
    ios = annotations.get(IOS)
    if ios is None:
        return "names no iOS version, so every iOS offers it"
    introduced = ios.get("introducedVersion", "")
    if not re.fullmatch(r"\d+(\.\d+)*", introduced) or version(introduced) != FIRST_VERSION:
        return f"introduces it on iOS {introduced!r}, not 27.0"
    if WILDCARD not in annotations:
        return "drops the wildcard, which keeps it on the other platforms"
    return None


def metadata_problems(actions_data: dict) -> list[str]:
    problems = []
    action = actions_data.get("actions", {}).get(ACTION)
    if action is None:
        problems.append(f"the metadata has no {ACTION}")
    else:
        found = availability_problem(action.get("availabilityAnnotations"))
        if found:
            problems.append(f"action {ACTION} {found}")
    shortcuts = [
        entry for entry in actions_data.get("autoShortcuts", [])
        if entry.get("actionIdentifier") == ACTION
    ]
    if not shortcuts:
        problems.append(f"the metadata has no automatic shortcut for {ACTION}")
    for entry in shortcuts:
        found = availability_problem(entry.get("availabilityAnnotations"))
        if found:
            problems.append(f"the automatic shortcut for {ACTION} {found}")
    for section, kind, name in (("entities", "entity", ENTITY), ("queries", "query", QUERY)):
        entry = actions_data.get(section, {}).get(name)
        if entry is None:
            problems.append(f"the metadata has no {name}")
            continue
        found = availability_problem(entry.get("availabilityAnnotations"))
        if found:
            problems.append(f"{kind} {name} {found}")
    return problems


def source_problems(text: str) -> list[str]:
    """Each declaration is preceded, as an attribute line and not in a comment, by the gate."""
    problems = []
    lines = text.splitlines()
    for declaration in DECLARATIONS:
        index = next(
            (i for i, line in enumerate(lines) if re.match(rf"(?:nonisolated )?{declaration}\b", line)),
            None,
        )
        if index is None:
            problems.append(f"{SOURCE} has no {declaration}")
            continue
        gated = False
        for line in reversed(lines[:index]):
            stripped = line.strip()
            if stripped.startswith("//"):
                continue
            if not stripped.startswith("@"):
                break
            gated = gated or GATE.fullmatch(stripped) is not None
        if not gated:
            problems.append(f"{declaration} is not @available(iOS 27.0, *)")
    return problems


# What the build extracted before the declarations were gated, and after.
WILDCARD_ONLY = {WILDCARD: {"introducedVersion": "*"}}
FROM_27 = {IOS: {"introducedVersion": "27.0"}, WILDCARD: {"introducedVersion": "*"}}


def extraction(annotations: dict, entity_annotations: dict | None = None) -> dict:
    kept = annotations if entity_annotations is None else entity_annotations
    return {
        "actions": {ACTION: {"availabilityAnnotations": annotations}},
        "autoShortcuts": [{"actionIdentifier": ACTION, "availabilityAnnotations": annotations}],
        "entities": {ENTITY: {"availabilityAnnotations": kept}},
        "queries": {QUERY: {"availabilityAnnotations": kept}},
    }


class Fixtures(unittest.TestCase):
    def test_a_wildcard_only_declaration_is_offered_on_ios_26(self) -> None:
        problems = metadata_problems(extraction(WILDCARD_ONLY))
        self.assertEqual(len(problems), 4, problems)
        self.assertTrue(all("names no iOS version" in problem for problem in problems), problems)

    def test_an_earlier_ios_version_is_offered_on_ios_26(self) -> None:
        early = {IOS: {"introducedVersion": "26.0"}, WILDCARD: {"introducedVersion": "*"}}
        self.assertEqual(len(metadata_problems(extraction(early))), 4)

    def test_the_action_and_the_shortcut_are_judged_each(self) -> None:
        data = extraction(FROM_27)
        data["autoShortcuts"][0]["availabilityAnnotations"] = WILDCARD_ONLY
        problems = metadata_problems(data)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("automatic shortcut", problems[0])

    def test_ios_27_and_the_wildcard_are_what_the_build_should_extract(self) -> None:
        self.assertEqual(metadata_problems(extraction(FROM_27)), [])

    def test_dropping_the_wildcard_takes_the_other_platforms_off(self) -> None:
        only_ios = {IOS: {"introducedVersion": "27.0"}}
        self.assertEqual(len(metadata_problems(extraction(only_ios))), 4)

    def test_an_entity_and_a_query_offered_on_ios_26_leave_shortcuts_a_find_action(self) -> None:
        problems = metadata_problems(extraction(FROM_27, WILDCARD_ONLY))
        self.assertEqual(len(problems), 2, problems)
        self.assertTrue(problems[0].startswith(f"entity {ENTITY}"), problems)
        self.assertTrue(problems[1].startswith(f"query {QUERY}"), problems)

    def test_a_missing_action_shortcut_entity_or_query_is_reported(self) -> None:
        self.assertEqual(len(metadata_problems({})), 4)

    def test_a_later_ios_27_version_leaves_ios_27_0_without_the_action(self) -> None:
        later = {IOS: {"introducedVersion": "27.1"}, WILDCARD: {"introducedVersion": "*"}}
        problems = metadata_problems(extraction(later))
        self.assertEqual(len(problems), 4, problems)
        self.assertTrue(all("27.1" in problem for problem in problems), problems)

    def test_27_written_other_ways_is_still_27_0(self) -> None:
        for written in ("27", "27.0", "27.0.0"):
            annotations = {IOS: {"introducedVersion": written}, WILDCARD: {"introducedVersion": "*"}}
            self.assertEqual(metadata_problems(extraction(annotations)), [], written)

    def test_an_annotation_in_a_comment_gates_nothing(self) -> None:
        commented = declarations("// @available(iOS 27.0, *)\n")
        self.assertEqual(len(source_problems(commented)), 4)
        doc_only = declarations("/// @available(iOS 27.0, *)\n")
        self.assertEqual(len(source_problems(doc_only)), 4)

    def test_other_versions_and_platforms_do_not_gate_it(self) -> None:
        for annotation in ("@available(iOS 26.0, *)", "@available(macOS 27.0, *)", "@available(iOS 27.1, *)"):
            self.assertEqual(len(source_problems(declarations(annotation + "\n"))), 4, annotation)

    def test_the_declarations_are_checked_in_the_source(self) -> None:
        self.assertEqual(source_problems(declarations("@available(iOS 27.0, *)\n")), [])
        for index in range(4):
            gated = ["@available(iOS 27.0, *)\n"] * 4
            gated[index] = ""
            self.assertEqual(len(source_problems(declarations(*gated))), 1, index)
        self.assertEqual(len(source_problems("")), 4)


def declarations(*annotations: str) -> str:
    """Source with the four declarations, each preceded by its annotation line ("" for none)."""
    heads = (
        "struct OpenAndroidAppIntent: OpenIntent {}\n",
        "nonisolated struct AndroidAppShortcuts: AppShortcutsProvider {}\n",
        "nonisolated struct AndroidAppEntity: AppEntity, IndexedEntity {}\n",
        "nonisolated struct AndroidAppQuery: EnumerableEntityQuery {}\n",
    )
    if len(annotations) == 1:
        annotations = annotations * len(heads)
    return "".join(f"/// A declaration.\n{note}{head}" for note, head in zip(annotations, heads))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--bundle", type=Path,
        help="a built Andarium.app: its extracted App Intents metadata is checked too")
    args, rest = parser.parse_known_args()
    checked = []
    checked += source_problems((args.root / SOURCE).read_text(encoding="utf-8"))
    if args.bundle is not None:
        path = args.bundle / "Metadata.appintents" / "extract.actionsdata"
        checked += metadata_problems(json.loads(path.read_text(encoding="utf-8")))
    if checked:
        sys.exit("test_open_intent_availability.py: " + "; ".join(checked))
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False)
    return 0 if result.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
