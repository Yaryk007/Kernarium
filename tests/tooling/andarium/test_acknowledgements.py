#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium acknowledges every framework and prefix library it embeds.

The Acknowledgements resource must name a project, its copyright holders and
its license, with the license text, for each framework the Andarium project
embeds and each component of the iOS device prefixes' contracts. The checks run
on the checked-in resource and on copies with one acknowledgement removed, so a
framework or library added without one fails here.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium" / "scripts" / "acknowledgements.py"
sys.path.insert(0, str(SCRIPT.parent))
import acknowledgements as ack  # noqa: E402

NOT_ACKNOWLEDGED = ("Andarium", "System", "User-Mode Linux")

# The paragraph the owner approved for the page of Android Open Source Project
# (British spelling as approved), and the title of the row that opens the
# notices. The application's code and catalog carry them word for word.
APPROVED_PARAGRAPH = (
    "Andarium contains machine code translated from the libraries of the Android system it runs, "
    "so that those libraries start and run faster. The translated code is derived from the original "
    "libraries and stays under their licences: Apache License 2.0 for the Android Open Source Project "
    "and LineageOS, and the BSD, MIT, zlib and other licences of the libraries they include. Every "
    "notice that applies to a translated library is listed under Translated system libraries."
)
ROW_TITLE = "Translated system libraries"
SWIFT = REPOSITORY / "Andarium/Andarium/Shared/Acknowledgements.swift"
CATALOG = REPOSITORY / "Andarium/Localization/Localizable.xcstrings"


class AcknowledgementsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = json.loads(ack.RESOURCE.read_text(encoding="utf-8"))
        self.frameworks = ack.embedded_frameworks()
        self.components = ack.device_components()

    def problems(self, document: dict) -> list[str]:
        return ack.check_document(document, self.frameworks, self.components)

    def test_the_resource_covers_what_the_app_embeds(self) -> None:
        self.assertEqual(self.problems(self.document), [])

    def test_the_inputs_are_not_empty(self) -> None:
        # A checker that finds nothing to check proves nothing.
        self.assertGreaterEqual(len(self.frameworks), 5)
        self.assertGreaterEqual(len(self.components), 13)
        self.assertIn("KRuntime_Android_6_18_4K", self.frameworks)
        self.assertIn("mesa", self.components)
        self.assertIn("libslirp", self.components)

    def test_an_unacknowledged_framework_fails(self) -> None:
        for framework in sorted(self.frameworks):
            with self.subTest(framework=framework):
                document = copy.deepcopy(self.document)
                for project in document["projects"]:
                    project["frameworks"] = [name for name in project["frameworks"] if name != framework]
                found = self.problems(document)
                self.assertTrue(
                    any(f"framework {framework} has no acknowledgement" in line for line in found), found
                )

    def test_an_unacknowledged_library_fails(self) -> None:
        for component in sorted(self.components):
            with self.subTest(component=component):
                document = copy.deepcopy(self.document)
                for project in document["projects"]:
                    project["components"] = [name for name in project["components"] if name != component]
                found = self.problems(document)
                self.assertTrue(
                    any(f"library {component} has no acknowledgement" in line for line in found), found
                )

    def test_a_new_framework_or_library_fails(self) -> None:
        found = ack.check_document(self.document, self.frameworks | {"NewFramework"}, self.components | {"newlib"})
        self.assertTrue(any("NewFramework" in line for line in found), found)
        self.assertTrue(any("newlib" in line for line in found), found)

    def test_a_stale_entry_fails(self) -> None:
        found = ack.check_document(self.document, self.frameworks - {"LibZstd"}, self.components - {"zstd-decompression"})
        self.assertTrue(any("LibZstd is not embedded" in line for line in found), found)
        self.assertTrue(any("zstd-decompression is in no iOS device prefix" in line for line in found), found)

    def test_a_project_needs_its_fields_and_text(self) -> None:
        for field in ("name", "copyright", "license", "spdx"):
            with self.subTest(field=field):
                document = copy.deepcopy(self.document)
                document["projects"][-1][field] = " "
                self.assertTrue(any(f"no {field}" in line for line in self.problems(document)))
        document = copy.deepcopy(self.document)
        document["projects"][-1]["texts"] = []
        document["projects"][-1]["licenses"] = []
        self.assertTrue(any("no license text" in line for line in self.problems(document)))
        document = copy.deepcopy(self.document)
        document["licenses"]["apache-2.0"]["text"] = ""
        self.assertTrue(any("'apache-2.0' is missing" in line for line in self.problems(document)))

    def test_every_project_names_a_group_and_an_id_once(self) -> None:
        identifiers = [project["id"] for project in self.document["projects"]]
        self.assertEqual(len(identifiers), len(set(identifiers)))
        groups = {group["id"] for group in self.document["groups"]}
        self.assertTrue(all(project["group"] in groups for project in self.document["projects"]))

    def test_the_overlapping_and_unshipped_names_are_gone(self) -> None:
        names = {project["name"] for project in self.document["projects"]}
        self.assertEqual(names & set(NOT_ACKNOWLEDGED), set())
        for expected in ("Android Open Source Project", "LineageOS", "Linux", "Mesa", "virglrenderer"):
            self.assertIn(expected, names)

    def test_the_command_line_check_passes(self) -> None:
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--coverage"],
            check=False,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


class TranslatedLibrariesTests(unittest.TestCase):
    """The notices of the translated libraries are the artifact's, the paragraph is the owner's."""

    def setUp(self) -> None:
        self.document = json.loads(ack.RESOURCE.read_text(encoding="utf-8"))
        self.project = next(p for p in self.document["projects"] if p["id"] == ack.TRANSLATED_ID)

    def test_the_resource_lists_the_notices_under_android(self) -> None:
        self.assertEqual(self.project["parent"], "android")
        self.assertEqual(self.project["requires_framework"], ack.AOT_FRAMEWORK)
        self.assertEqual(self.project["name"], ROW_TITLE)
        self.assertEqual(len(self.project["texts"]), 30)
        titles = [notice["title"] for notice in self.project["texts"]]
        self.assertTrue(all(title.strip() for title in titles))
        self.assertEqual(titles, sorted(titles, key=str.lower))
        android = next(p for p in self.document["projects"] if p["id"] == "android")
        self.assertIn(ack.AOT_FRAMEWORK, android["frameworks"])

    def test_a_parent_or_framework_that_is_not_there_fails(self) -> None:
        frameworks = ack.embedded_frameworks()
        components = ack.device_components()
        document = copy.deepcopy(self.document)
        next(p for p in document["projects"] if p["id"] == ack.TRANSLATED_ID)["parent"] = "nothing"
        self.assertTrue(any("parent 'nothing' is not a project" in line
                            for line in ack.check_document(document, frameworks, components)))
        self.assertTrue(any("requires framework UmlArm64Aot that the application does not embed" in line
                            for line in ack.check_document(self.document, frameworks - {ack.AOT_FRAMEWORK}, components)))
        document = copy.deepcopy(self.document)
        for project in document["projects"]:
            project["frameworks"] = [name for name in project["frameworks"] if name != ack.AOT_FRAMEWORK]
        self.assertTrue(any("requires framework UmlArm64Aot that no project acknowledges" in line
                            for line in ack.check_document(document, frameworks, components)))

    def artifact(self, libraries: list) -> pathlib.Path:
        directory = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        (directory / "licences.json").write_text(
            json.dumps({"release_id": "x", "libraries": libraries}), encoding="utf-8")
        return directory

    def test_each_distinct_notice_is_listed_once_in_a_fixed_order(self) -> None:
        apache = "\n   Apache License\n   Version 2.0\n\n   The terms of the license apply to every file.\n"
        padded = apache.replace("\n\n", "\n  \n")
        libraries = [
            {"path": "/system/lib64/a.so", "labels": [], "notices": [
                {"id": "b2", "text": "Permission is hereby granted, free of charge.\n"},
                {"id": "a1", "text": padded}]},
            {"path": "/system/lib64/b.so", "labels": [], "notices": [
                {"id": "a1", "text": apache}, {"id": "c3", "text": "=====\nZ notice\n"}]},
        ]
        with mock.patch.object(ack.release_artifact, "verify", return_value={}):
            notices = ack.translated_notices(self.artifact(libraries))
        self.assertEqual([n["title"] for n in notices],
                         ["Apache License", "Permission is hereby granted, free of charge.", "Z notice"])
        self.assertEqual(notices[0]["text"].split(), apache.split())
        # The same notice under one identifier with texts that differ in white space: one entry.
        with mock.patch.object(ack.release_artifact, "verify", return_value={}):
            self.assertEqual(ack.translated_notices(self.artifact(list(reversed(libraries)))), notices)

    def test_an_artifact_that_is_not_one_or_has_no_notice_is_refused(self) -> None:
        empty = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        with self.assertRaises(ack.Problem) as raised:
            ack.translated_notices(empty)
        self.assertIn("is not an AOT artifact", str(raised.exception))
        with mock.patch.object(ack.release_artifact, "verify", return_value={}):
            with self.assertRaises(ack.Problem):
                ack.translated_notices(self.artifact([{"path": "/p", "labels": [], "notices": []}]))

    def test_without_an_accepted_artifact_one_must_be_named(self) -> None:
        root = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        with self.assertRaises(ack.Problem) as raised:
            ack.aot_artifact_directory(root, None)
        self.assertIn("--aot-artifact", str(raised.exception))
        named = root / "artifact"
        self.assertEqual(ack.aot_artifact_directory(root, named), named)

    def test_the_applications_text_is_the_approved_paragraph_in_four_languages(self) -> None:
        self.assertIn(APPROVED_PARAGRAPH, SWIFT.read_text(encoding="utf-8"))
        self.assertIn(f'"{ROW_TITLE}"', SWIFT.read_text(encoding="utf-8"))
        strings = json.loads(CATALOG.read_text(encoding="utf-8"))["strings"]
        for key in (APPROVED_PARAGRAPH, ROW_TITLE):
            entry = strings[key]
            self.assertEqual(sorted(entry["localizations"]), ["ja", "zh-Hans", "zh-Hant"])
            for language, localization in entry["localizations"].items():
                self.assertEqual(localization["stringUnit"]["state"], "translated", language)
                self.assertTrue(localization["stringUnit"]["value"].strip(), language)


class RootProjectsTests(unittest.TestCase):
    """Magisk and BusyBox: shown only where the release carries root, each with
    the full text of its license and one statement of where the source is."""

    def setUp(self) -> None:
        self.document = json.loads(ack.RESOURCE.read_text(encoding="utf-8"))
        self.projects = {p["id"]: p for p in self.document["projects"]}

    def test_both_are_in_the_resource_for_root_only(self) -> None:
        for identifier, shared in (("magisk", "gpl-3.0"), ("busybox", "gpl-2.0")):
            with self.subTest(project=identifier):
                project = self.projects[identifier]
                self.assertIs(project["requires_root"], True)
                self.assertEqual(project["group"], "system")
                self.assertEqual(project["licenses"], [shared])
                self.assertTrue(self.document["licenses"][shared]["text"].strip())
        self.assertNotIn("requires_root", self.projects["linux"])

    def test_the_notice_lines_name_the_contributors_and_no_person(self) -> None:
        wanted = {
            "magisk": ("The Magisk contributors", "GNU GPL v3 or later", "GPL-3.0-or-later"),
            "busybox": ("The BusyBox contributors", "GNU GPL v2", "GPL-2.0-only"),
        }
        for identifier, (copyright, label, spdx) in wanted.items():
            with self.subTest(project=identifier):
                project = self.projects[identifier]
                self.assertEqual((project["copyright"], project["license"], project["spdx"]), (copyright, label, spdx))

    def test_each_names_the_magisk_project_and_the_version_as_the_source(self) -> None:
        for identifier in ("magisk", "busybox"):
            with self.subTest(project=identifier):
                notice = next(t for t in self.projects[identifier]["texts"] if t["title"] == "Source code")
                self.assertEqual(notice["text"], ack.MAGISK_SOURCE_NOTICE)
        self.assertIn(ack.MAGISK_SOURCE, ack.MAGISK_SOURCE_NOTICE)
        self.assertEqual(ack.MAGISK_SOURCE, "https://github.com/topjohnwu/Magisk")
        self.assertEqual(ack.MAGISK_SOURCE_NOTICE.count(ack.MAGISK_VERSION), 2)

    def test_requires_root_means_true(self) -> None:
        document = copy.deepcopy(self.document)
        next(p for p in document["projects"] if p["id"] == "magisk")["requires_root"] = False
        found = ack.check_document(document, ack.embedded_frameworks(), ack.device_components())
        self.assertTrue(any("magisk: requires_root is False" in line for line in found), found)

    def test_the_gpl3_text_is_taken_only_from_copies_that_agree(self) -> None:
        root = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        for name, data in (("one", b"GNU GENERAL PUBLIC LICENSE\r\nVersion 3\r\n"), ("two", b"GNU GENERAL PUBLIC LICENSE\nVersion 3\n"),
                           ("three", b"GNU GENERAL PUBLIC LICENSE\nVersion 3.\n")):
            (root / name).mkdir()
            (root / name / "COPYING").write_bytes(data)
        texts = ack.Texts(root, None)
        # Line endings are normalised away; nothing else is.
        self.assertEqual(texts.agreeing((("one", "COPYING"), ("two", "COPYING"))).split(), ["GNU", "GENERAL", "PUBLIC", "LICENSE", "Version", "3"])
        with self.assertRaises(ack.Problem) as raised:
            texts.agreeing((("one", "COPYING"), ("three", "COPYING")))
        self.assertIn("differ", str(raised.exception))
        self.assertEqual(len(ack.GPL3_COPIES), 2)
        self.assertEqual(len({source for source, _ in ack.GPL3_COPIES}), 2)


class ReflowTests(unittest.TestCase):
    def test_hard_wrapped_paragraphs_join_and_blocks_stay(self) -> None:
        wrapped = (
            "The licenses for most software are designed to take away your freedom to\n"
            "share and change it.  By contrast, the GNU General Public License is\n"
            "intended to guarantee your freedom to share and change free software."
        )
        block = "Juha Pirkola.\nGregory M. Christy.\nThe Regents of the University of California."
        text = f"{wrapped}\n\n{block}"
        result = ack.reflow(text)
        self.assertEqual(result.split("\n\n")[0].count("\n"), 0)
        self.assertEqual(result.split("\n\n")[1], block)
        self.assertEqual(result.split(), text.split())

    def test_a_changed_word_is_refused(self) -> None:
        with self.assertRaises(ack.Problem):
            ack.clean("one two three", original="one two four")


if __name__ == "__main__":
    unittest.main()
