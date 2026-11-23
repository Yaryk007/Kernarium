#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/localization.py checks the String Catalogs and syncs them with the code.

The real tree must pass its own check. A small fixture tree (one catalog of
each kind, the project's permission text, one What's New note in each track) passes it, and
each defect the script's header lists is made in the fixture alone and must
be found. `sync` runs Apple's `xcstringstool` on a hand-written
`.stringsdata`, so that part needs a Mac and skips elsewhere.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium/scripts/localization.py"
BOUND_SECONDS = 120

NOTE = "Browse the virtual machine’s storage."
PERMISSION = "Andarium records from the microphone only when you turn on Microphone."
LANGS = ("ja", "zh-Hans", "zh-Hant")
PHRASE = "Open ${target} in ${applicationName}"


def load_script():
    spec = importlib.util.spec_from_file_location("localization", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["localization"] = module
    spec.loader.exec_module(module)
    return module


def unit(value: str, state: str = "translated") -> dict:
    return {"stringUnit": {"state": state, "value": value}}


def phrases(*values: str, state: str = "translated") -> dict:
    """An App Shortcut's phrases in one language."""
    return {"stringSet": {"state": state, "values": list(values)}}


def entry(**values: str) -> dict:
    return {"localizations": {lang: unit(values[lang.replace("-", "_")]) for lang in LANGS}}


def catalog(strings: dict) -> dict:
    return {"sourceLanguage": "en", "strings": strings, "version": "1.0"}


class Tree:
    """A fixture repository: the paths the script reads, with one valid entry in each."""

    def __init__(self, root: pathlib.Path, script) -> None:
        self.root = root
        self.script = script
        self.files = {
            "Localizable": catalog({
                "Virtual machine": entry(ja="仮想マシン", zh_Hans="虚拟机", zh_Hant="虛擬機器"),
                "%@ of %@": entry(ja="%1$@ / %2$@", zh_Hans="%1$@ / %2$@", zh_Hant="%1$@ / %2$@"),
                "storage.of": {"localizations": {
                    "en": unit("%1$@ of %2$@"),
                    "ja": unit("%1$@ / %2$@"), "zh-Hans": unit("%2$@中的%1$@"), "zh-Hant": unit("%2$@中的%1$@"),
                }},
                "%lld snapshots": {"localizations": {
                    "en": {"variations": {"plural": {"one": unit("%lld snapshot"), "other": unit("%lld snapshots")}}},
                    **{lang: {"variations": {"plural": {"other": unit("%lld X")}}} for lang in LANGS},
                }},
                "Retina": {"shouldTranslate": False},
            }),
            "JIT": catalog({"Preparing JIT": entry(ja="JITを準備中", zh_Hans="正在准备JIT", zh_Hant="正在準備JIT")}),
            "FollowX": catalog({"Follow developer on %@": entry(
                ja="%@でデベロッパをフォロー", zh_Hans="在%@上关注开发者", zh_Hant="在%@上追蹤開發者")}),
            "AppShortcuts": catalog({PHRASE: {"localizations": {
                "en": phrases("Open ${target} in ${applicationName}", "Open an Android app in ${applicationName}"),
                "ja": phrases("${applicationName}で${target}を開く", "${applicationName}でAndroidアプリを開く"),
                "zh-Hans": phrases("在${applicationName}中打开${target}", "在${applicationName}中打开Android应用"),
                "zh-Hant": phrases("在${applicationName}中打開${target}", "在${applicationName}中打開Android應用程式"),
            }}}),
            "AndariumCore": catalog({"Paused": entry(ja="一時停止", zh_Hans="已暂停", zh_Hant="已暫停")}),
            "GuestHostCore": catalog({"Later": entry(ja="後で", zh_Hans="稍后", zh_Hant="稍後")}),
            "InfoPlist": catalog({"NSMicrophoneUsageDescription": {"localizations": {
                "en": unit(PERMISSION), **{lang: unit(f"{lang}: 麦克风") for lang in LANGS}}}}),
            "WhatsNew": catalog({
                NOTE: {"extractionState": "manual", **entry(
                    ja="ストレージを参照します。", zh_Hans="浏览存储空间。", zh_Hant="瀏覽儲存空間。")},
            }),
        }
        self.english_only = {
            "identical": [{"catalog": "Localizable", "key": "Retina", "reason": "a name"}],
            "english": [{"text": "JIT", "file": "Andarium/Andarium/Shared/Dev.swift", "reason": "development only"}],
        }
        self.sources = {"Andarium/Andarium/Shared/Dev.swift": 'Text(verbatim: "JIT")\n'}
        self.whats_new = [{"entries": [{"text": NOTE}]}]
        self.project = (
            '{ "targets": [ {\n  "name": "Andarium",\n  "id": "A1",\n  "settings": {\n'
            f'    "INFOPLIST_KEY_NSMicrophoneUsageDescription": "{PERMISSION}"\n  }} }},\n'
            '  { "name": "AndariumPreview",\n    "id": "A2", "settings": {} } ] }\n'
        )

    def write(self) -> pathlib.Path:
        for name, relative in self.script.CATALOGS.items():
            self.put(relative, json.dumps(self.files[name], ensure_ascii=False, indent=2))
        self.put(self.script.ENGLISH_ONLY, json.dumps(self.english_only, indent=2))
        self.put(self.script.WHATS_NEW, json.dumps(self.whats_new, ensure_ascii=False))
        self.put(self.script.PROJECT, self.project)
        for relative, text in self.sources.items():
            self.put(relative, text)
        return self.root

    def put(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def string(self, catalog_name: str, key: str) -> dict:
        return self.files[catalog_name]["strings"][key]


class CheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self.script = load_script()
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.tree = Tree(pathlib.Path(scratch.name), self.script)

    def problems(self) -> list[str]:
        return self.script.check(self.tree.write())

    def assertFinds(self, text: str) -> None:
        found = self.problems()
        self.assertTrue(any(text in problem for problem in found), f"{text!r} not reported; got {found}")

    def test_the_real_tree_passes_its_own_check(self) -> None:
        run = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "check", "--root", str(REPOSITORY)],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=BOUND_SECONDS, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("the String Catalogs are complete", run.stdout)

    def test_the_fixture_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_a_missing_language(self) -> None:
        del self.tree.string("Localizable", "Virtual machine")["localizations"]["ja"]
        self.assertFinds("has no ja translation")

    def test_a_value_not_marked_translated(self) -> None:
        self.tree.string("AndariumCore", "Paused")["localizations"]["zh-Hant"] = unit("已暫停", "needs_review")
        self.assertFinds("not 'translated'")

    def test_a_new_value(self) -> None:
        self.tree.string("Localizable", "Virtual machine")["localizations"]["zh-Hans"] = unit("虚拟机", "new")
        self.assertFinds("not 'translated'")

    def test_an_empty_value(self) -> None:
        self.tree.string("Localizable", "Virtual machine")["localizations"]["ja"] = unit(" ")
        self.assertFinds("is empty")

    def test_a_stale_key(self) -> None:
        self.tree.string("Localizable", "Virtual machine")["extractionState"] = "stale"
        self.assertFinds("is stale")

    def test_a_missing_format_specifier(self) -> None:
        self.tree.string("Localizable", "%@ of %@")["localizations"]["ja"] = unit("%1$@中")
        self.assertFinds("are not those of the source")

    def test_a_named_key_takes_its_specifiers_from_its_english_value(self) -> None:
        self.tree.string("Localizable", "storage.of")["localizations"]["ja"] = unit("%1$@")
        self.assertFinds("are not those of the source")

    def test_reordered_specifiers_need_positions(self) -> None:
        self.tree.string("Localizable", "%@ of %@")["localizations"]["zh-Hant"] = unit("%2$@/%@")
        self.assertFinds("mixes positional and plain")

    def test_positions_that_skip_a_number(self) -> None:
        self.tree.string("Localizable", "%@ of %@")["localizations"]["ja"] = unit("%1$@/%3$@")
        self.assertFinds("are not 1 to 2")

    def test_a_plural_category_a_language_does_not_have(self) -> None:
        plural = self.tree.string("Localizable", "%lld snapshots")["localizations"]["ja"]["variations"]["plural"]
        plural["one"] = unit("%lld X")
        self.assertFinds("plural category 'one', which ja does not have")

    def test_a_plural_without_other(self) -> None:
        plural = self.tree.string("Localizable", "%lld snapshots")["localizations"]["ja"]["variations"]["plural"]
        plural["many"] = plural.pop("other")
        self.assertFinds("plural variations without 'other'")

    def test_app_shortcut_phrases_pass_and_each_is_checked(self) -> None:
        self.assertEqual(self.problems(), [])
        self.tree.string("AppShortcuts", PHRASE)["localizations"]["ja"] = phrases(
            "${applicationName}で${target}を開く", "Open an Android app in ${applicationName}")
        self.assertFinds("(ja/phrase 2): is an English form of the key")

    def test_app_shortcut_phrases_not_marked_translated(self) -> None:
        self.tree.string("AppShortcuts", PHRASE)["localizations"]["zh-Hant"] = phrases(
            "在${applicationName}中打開${target}", "在${applicationName}中打開Android應用程式", state="new")
        self.assertFinds("is in the state 'new'")

    def test_app_shortcut_phrases_missing_a_language(self) -> None:
        del self.tree.string("AppShortcuts", PHRASE)["localizations"]["zh-Hans"]
        self.assertFinds("has no zh-Hans translation")

    def test_a_google_name_in_a_value(self) -> None:
        self.tree.string("Localizable", "Virtual machine")["localizations"]["zh-Hans"] = unit("Google 虚拟机")
        self.assertFinds("names a Google product")

    def test_a_google_name_in_a_key(self) -> None:
        self.tree.files["Localizable"]["strings"]["Open Google Play"] = entry(ja="開く", zh_Hans="打开", zh_Hant="開啟")
        self.assertFinds("names a Google product")

    def put_notice(self, catalog_name: str, translations: dict[str, str] | None = None) -> str:
        """The no-Google-apps notice's own title in a catalog, as the application carries it."""
        key = "No Google apps"
        self.assertIn(key, self.script.GOOGLE_NOTICE_KEYS[catalog_name])
        values = {"ja": "Googleのアプリは意図的に非搭載", "zh_Hans": "有意不含Google应用", "zh_Hant": "刻意不內建Google應用程式"}
        values.update(translations or {})
        self.tree.files[catalog_name]["strings"][key] = entry(**values)
        return key

    def test_the_notice_may_name_google_in_every_language(self) -> None:
        self.put_notice("Localizable")
        body = next(iter(sorted(self.script.GOOGLE_NOTICE_KEYS["Localizable"] - {"No Google apps"})))
        self.tree.files["Localizable"]["strings"][body] = entry(
            ja="Googleは、自社のアプリとGoogle Play開発者サービスを、認定済みデバイスのメーカーにのみライセンス供与しています。",
            zh_Hans="Google仅向制造商授权其应用和Google Play服务。",
            zh_Hant="Google只向製造商授權其應用程式和Google Play服務。")
        self.assertEqual(self.problems(), [])

    def test_the_notice_names_no_other_google_product(self) -> None:
        self.put_notice("Localizable", {"zh_Hans": "有意不含Google应用"})
        self.assertEqual(self.problems(), [])  # a name the notice may use, with Latin letters
        for name in ("Gmail", "Play Store", "YouTube", "google", "Google Play Store", "Google Play商店",
                     "Google Play Protect", "Google Maps", "Google One", "GoogleTV", "Google Play"):
            with self.subTest(name=name):
                self.put_notice("Localizable", {"ja": f"{name}は意図的に非搭載"})
                self.assertFinds("names a Google product")

    def test_the_notice_may_name_google_play_services_in_each_language(self) -> None:
        for name in ("Google Play services", "Google Play服务", "Google Play服務", "Google Play開発者サービス", "Google"):
            with self.subTest(name=name):
                self.put_notice("Localizable", {"ja": f"{name}は意図的に非搭載"})
                self.assertEqual(self.problems(), [])

    def test_the_notice_keeps_google_in_latin_letters(self) -> None:
        self.put_notice("Localizable", {"zh_Hans": "有意不含谷歌应用"})
        self.assertFinds("names a Google product")
        self.put_notice("Localizable", {"zh_Hans": "有意不含Google应用", "ja": "グーグルのアプリは意図的に非搭載"})
        self.assertFinds("names a Google product")

    def test_another_key_may_not_name_google_beside_the_notice(self) -> None:
        self.put_notice("Localizable")
        self.tree.files["Localizable"]["strings"]["Open Google Play"] = entry(ja="開く", zh_Hans="打开", zh_Hant="開啟")
        self.assertFinds("names a Google product")

    def test_the_notice_in_another_catalog_may_not_name_google(self) -> None:
        # The notice's key in another catalog is not the notice.
        key = self.put_notice("Localizable")
        del self.tree.files["Localizable"]["strings"][key]
        self.tree.files["JIT"]["strings"][key] = entry(
            ja="Googleのアプリが見つかりませんか？", zh_Hans="找不到Google应用？", zh_Hant="找不到Google應用程式？")
        self.assertFinds("names a Google product")

    def test_english_left_untranslated(self) -> None:
        self.tree.files["Localizable"]["strings"]["Erase virtual machine"] = entry(
            ja="Erase virtual machine", zh_Hans="抹掉虚拟机", zh_Hant="清除虛擬機器")
        self.assertFinds("is an English form of the key")

    def test_a_plural_form_left_in_english(self) -> None:
        plural = self.tree.string("Localizable", "%lld snapshots")["localizations"]["ja"]["variations"]["plural"]
        plural["other"] = unit("%lld snapshots")
        self.assertFinds("is an English form of the key")

    def test_a_translation_that_is_the_singular_english_form(self) -> None:
        plural = self.tree.string("Localizable", "%lld snapshots")["localizations"]["zh-Hans"]["variations"]["plural"]
        plural["other"] = unit("%lld snapshot")
        self.assertFinds("is an English form of the key")

    def test_a_translation_that_differs_from_the_english_only_in_case(self) -> None:
        self.tree.string("Localizable", "Virtual machine")["localizations"]["ja"] = unit("Virtual Machine")
        self.assertFinds("differs from the English text")

    def test_a_named_key_left_as_its_english_value(self) -> None:
        localizations = self.tree.string("Localizable", "storage.of")["localizations"]
        localizations["en"] = unit("%1$@ out of %2$@")
        localizations["ja"] = unit("%1$@ out of %2$@")
        self.assertFinds("is an English form of the key")

    def test_an_info_plist_value_left_in_english(self) -> None:
        self.tree.string("InfoPlist", "NSMicrophoneUsageDescription")["localizations"]["ja"] = unit(PERMISSION)
        self.assertFinds("is an English form of the key")

    def test_an_unescaped_percent_in_a_value(self) -> None:
        self.tree.string("AndariumCore", "Paused")["localizations"]["ja"] = unit("100% 一時停止")
        self.assertFinds("write `%%`")

    def test_an_escaped_percent_is_fine(self) -> None:
        self.tree.string("AndariumCore", "Paused")["localizations"]["ja"] = unit("100%%一時停止")
        self.assertEqual(self.problems(), [])

    def test_an_unescaped_percent_in_a_key(self) -> None:
        self.tree.files["AndariumCore"]["strings"]["Up to 100%"] = entry(ja="最大100%%", zh_Hans="最多100%%", zh_Hant="最多100%%")
        self.assertFinds("write `%%`")

    def test_a_retired_word_for_the_virtual_machine(self) -> None:
        for language, word in (("zh-Hans", "客户机"), ("zh-Hant", "客體"), ("ja", "ゲスト")):
            with self.subTest(language=language):
                self.setUp()
                self.tree.string("Localizable", "Virtual machine")["localizations"][language] = unit(f"{word}の画面")
                self.assertFinds("which the glossary retired")

    def test_the_polite_pronoun_in_chinese(self) -> None:
        for language in ("zh-Hans", "zh-Hant"):
            with self.subTest(language=language):
                self.setUp()
                self.tree.string("Localizable", "Virtual machine")["localizations"][language] = unit("请您等待")
                self.assertFinds("which the glossary retired")

    def test_a_space_between_chinese_and_a_latin_word_or_a_number(self) -> None:
        for text in ("内存 Andarium", "Andarium 内存", "内存 2.5 GiB", "共 %lld 个"):
            with self.subTest(text=text):
                self.setUp()
                self.tree.string("Localizable", "Virtual machine")["localizations"]["zh-Hans"] = unit(text)
                self.assertFinds("space between Chinese or Japanese text and a Latin word")

    def test_a_number_keeps_the_space_before_its_unit(self) -> None:
        self.tree.string("Localizable", "Virtual machine")["localizations"]["zh-Hans"] = unit("内存2.5 GiB")
        self.assertEqual(self.problems(), [])

    def test_half_width_punctuation_after_chinese_or_japanese(self) -> None:
        for text in ("要抹掉吗?", "好!", "一,二", "仮想マシン;"):
            with self.subTest(text=text):
                self.setUp()
                self.tree.string("Localizable", "Virtual machine")["localizations"]["ja"] = unit(text)
                self.assertFinds("half-width")

    def test_the_names_of_the_application_need_no_translation_in_info_plist(self) -> None:
        self.tree.files["InfoPlist"]["strings"]["CFBundleDisplayName"] = {"shouldTranslate": False}
        self.tree.files["InfoPlist"]["strings"]["CFBundleName"] = {"shouldTranslate": False}
        self.assertEqual(self.problems(), [])

    def test_a_missing_jit_catalog(self) -> None:
        self.tree.write()
        (self.tree.root / self.script.CATALOGS["JIT"]).unlink()
        self.assertTrue(any("is missing" in problem for problem in self.script.check(self.tree.root)))

    def test_a_jit_string_without_a_language(self) -> None:
        del self.tree.string("JIT", "Preparing JIT")["localizations"]["zh-Hant"]
        self.assertFinds("has no zh-Hant translation")

    def test_an_untranslated_key_off_the_list(self) -> None:
        self.tree.files["Localizable"]["strings"]["Metal"] = {"shouldTranslate": False}
        self.assertFinds("is not translated but is not on the list")

    def test_a_list_entry_for_a_key_that_is_gone(self) -> None:
        self.tree.english_only["identical"].append({"catalog": "Localizable", "key": "Gone", "reason": "none"})
        self.assertFinds("names 'Gone'")

    def test_an_english_text_no_longer_in_its_file(self) -> None:
        self.tree.sources["Andarium/Andarium/Shared/Dev.swift"] = "// nothing\n"
        self.assertFinds("is no longer in")

    def test_an_english_text_that_a_catalog_translates(self) -> None:
        self.tree.files["Localizable"]["strings"]["JIT"] = entry(ja="JIT", zh_Hans="即时编译", zh_Hant="即時編譯")
        self.assertFinds("a catalog translates it")

    def test_a_note_with_the_text_of_an_english_text_translates_nothing(self) -> None:
        self.tree.files["WhatsNew"]["strings"]["JIT"] = entry(ja="JIT", zh_Hans="JIT", zh_Hant="JIT")
        self.tree.whats_new[0]["entries"].append({"title": "JIT", "text": NOTE})
        self.tree.english_only["identical"].append({"catalog": "WhatsNew", "key": "JIT", "reason": "a term"})
        self.assertEqual(self.problems(), [])

    def test_a_permission_text_without_an_entry(self) -> None:
        del self.tree.files["InfoPlist"]["strings"]["NSMicrophoneUsageDescription"]
        self.assertFinds("has no entry for NSMicrophoneUsageDescription")

    def test_a_permission_entry_that_differs_from_the_project(self) -> None:
        self.tree.string("InfoPlist", "NSMicrophoneUsageDescription")["localizations"]["en"] = unit("Something else.")
        self.assertFinds("differs from the project's text")

    def test_a_permission_text_of_another_target_is_not_expected(self) -> None:
        self.tree.project = self.tree.project.replace(
            '"settings": {} } ] }',
            f'"settings": {{ "INFOPLIST_KEY_NSCameraUsageDescription": "Camera." }} }} ] }}')
        self.assertEqual(self.problems(), [])

    def test_a_whats_new_note_without_an_entry(self) -> None:
        self.tree.whats_new[0]["entries"].append({"title": "More", "points": ["A new point"]})
        self.assertFinds("has no entry for the note 'A new point'")

    def test_a_whats_new_entry_that_is_no_note(self) -> None:
        self.tree.files["WhatsNew"]["strings"]["Removed note"] = entry(ja="a", zh_Hans="b", zh_Hant="c")
        self.assertFinds("is no note of")

    def test_a_missing_notes_file(self) -> None:
        self.tree.write()
        relative = self.script.WHATS_NEW
        (self.tree.root / relative).unlink()
        self.assertTrue(any(relative in problem and "is missing" in problem for problem in self.script.check(self.tree.root)))

    def test_a_catalog_in_another_source_language(self) -> None:
        self.tree.files["AndariumCore"]["sourceLanguage"] = "de"
        self.assertFinds("the source language is 'de'")

    def test_a_missing_catalog(self) -> None:
        self.tree.write()
        (self.tree.root / self.script.CATALOGS["WhatsNew"]).unlink()
        self.assertTrue(any("is missing" in problem for problem in self.script.check(self.tree.root)))


@unittest.skipUnless(shutil.which("xcrun"), "xcstringstool is part of Xcode")
class SyncTests(unittest.TestCase):
    def setUp(self) -> None:
        self.script = load_script()
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.scratch = pathlib.Path(scratch.name)
        self.tree = Tree(self.scratch / "tree", self.script)
        self.source = "Andarium/Andarium/Shared/Home.swift"
        self.tree.sources[self.source] = "// strings are extracted from here\n"
        self.root = self.tree.write()
        self.data = self.scratch / "build" / "Home.stringsdata"

    def extract(self, *keys: str, jit: tuple[str, ...] = ("Preparing JIT",),
                follow: tuple[str, ...] = ("Follow developer on %@",),
                shortcuts: tuple[str, ...] = (PHRASE,)) -> None:
        self.data.parent.mkdir(parents=True, exist_ok=True)

        def table(texts: tuple[str, ...]) -> list[dict]:
            return [{"comment": "", "key": key, "location": {"startingColumn": 1, "startingLine": 1}} for key in texts]

        body = {"source": str(self.root / self.source), "tables": {
            "Localizable": table(keys), "JIT": table(jit),
            "FollowX": table(follow),
            "AppShortcuts": [
                {"key": key, "location": {"startingColumn": 1, "startingLine": 1},
                 "values": [key, "Open an Android app in ${applicationName}"] if key == PHRASE else [key]}
                for key in shortcuts]}, "version": 1}
        self.data.write_text(json.dumps(body), encoding="utf-8")

    def sync(self, *extra: str) -> list[str]:
        return self.script.sync(self.root, [self.data.parent], "--check" in extra)

    def catalog(self) -> dict:
        return json.loads((self.root / self.script.CATALOGS["Localizable"]).read_text(encoding="utf-8"))["strings"]

    def test_a_new_string_is_added_untranslated_and_the_check_then_fails(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of", "Erase virtual machine")
        problems = self.sync("--check")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("1 to add ['Erase virtual machine']", problems[0])
        self.assertNotIn("Erase virtual machine", self.catalog(), "--check must change nothing")
        self.assertEqual(self.sync(), [])
        self.assertNotIn("localizations", self.catalog()["Erase virtual machine"])
        self.assertEqual(self.sync("--check"), [])
        self.assertTrue(any("has no ja translation" in problem for problem in self.script.check(self.root)))

    def test_a_string_the_code_dropped_becomes_stale(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "Retina", "storage.of")
        self.assertEqual(self.sync(), [])
        self.assertEqual(self.catalog()["%lld snapshots"].get("extractionState"), "stale")
        self.assertTrue(any("is stale" in problem for problem in self.script.check(self.root)))

    def test_a_tree_in_step_has_nothing_to_change(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of")
        self.assertEqual(self.sync("--check"), [])

    def test_a_jit_string_of_the_development_build_is_in_step(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of")
        self.assertEqual(self.sync("--check"), [])

    def test_a_missing_jit_string_is_found(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of", jit=("Preparing JIT", "A new JIT text"))
        problems = self.sync("--check")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("JIT: is out of step with the code: 1 to add ['A new JIT text']", problems[0])

    def test_a_follow_x_string_of_the_arranged_build_is_in_step(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of")
        self.assertEqual(self.sync("--check"), [])

    def test_a_missing_follow_x_string_is_found(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of",
                     follow=("Follow developer on %@", "A new row text"))
        problems = self.sync("--check")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("FollowX: is out of step with the code: 1 to add ['A new row text']", problems[0])

    def test_a_development_build_without_the_row_is_refused_not_read_as_stale(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of", follow=())
        problems = self.sync("--check")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("FollowX: has no extracted strings: build with FOLLOW_X_HANDLE=example_handle", problems[0])

    def test_an_app_shortcut_phrase_of_the_arranged_build_is_in_step(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of")
        self.assertEqual(self.sync("--check"), [])

    def test_a_missing_app_shortcut_phrase_is_found(self) -> None:
        self.extract("Virtual machine", "%@ of %@", "%lld snapshots", "Retina", "storage.of",
                     shortcuts=(PHRASE, "Close ${target} in ${applicationName}"))
        problems = self.sync("--check")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("AppShortcuts: is out of step with the code: 1 to add ['Close ${target} in ${applicationName}']", problems[0])

    def test_a_build_without_extracted_strings(self) -> None:
        empty = self.scratch / "empty"
        empty.mkdir()
        problems = self.script.sync(self.root, [empty], True)
        self.assertEqual(len(problems), 1)
        self.assertIn("no .stringsdata", problems[0])


if __name__ == "__main__":
    unittest.main()
