#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/check-release-archive.py holds a Release archive to what any archive needs.

Each case builds a fixture application bundle, changes one thing and runs the
script on it. The Release archive's build phase runs the script; Debug builds,
the Preview and an ordinary Release build do not, so a fixture with the example
handle stays usable everywhere else. The rows for the executable and the
frameworks link small libraries with the iPhone SDK's compiler: what the build
host leaves in them (a linker debug map, a home, volume or temporary path), a
guest section that carries such paths on purpose, the test bundles and
frameworks that run tests, and a linked test-support library.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[3] / "Andarium" / "scripts" / "check-release-archive.py"
HANDLE_KEY = "AndariumFollowXHandle"


def run(app: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT), str(app)], check=False, stdin=subprocess.DEVNULL,
        capture_output=True, text=True, timeout=120)


class Fixtures(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.count = 0

    def app(self, **info: object) -> Path:
        """A bundle with an Info.plist of `info`; a value of None leaves the key out."""
        self.count += 1
        app = self.root / f"fixture-{self.count}" / "Andarium.app"
        app.mkdir(parents=True)
        keys = {"CFBundleIdentifier": "org.example.Fixture", **info}
        (app / "Info.plist").write_bytes(plistlib.dumps({k: v for k, v in keys.items() if v is not None}))
        return app

    def passes(self, app: Path) -> None:
        done = run(app)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def refuses(self, app: Path, expect: str) -> None:
        done = run(app)
        self.assertNotEqual(done.returncode, 0, done.stdout)
        self.assertIn(expect, done.stderr)
        self.assertNotIn("App Store", done.stdout + done.stderr)


class FollowXHandle(Fixtures):
    """The profile link the row opens is the developer's own, or there is none."""

    def test_a_bundle_with_no_handle_has_no_link(self) -> None:
        self.passes(self.app())

    def test_an_empty_handle_has_no_link(self) -> None:
        self.passes(self.app(**{HANDLE_KEY: ""}))

    def test_a_well_formed_handle_passes(self) -> None:
        for handle in ("real_handle", "a", "A1_b2", "abcdefghijklmno", "_underscore_"):
            with self.subTest(handle=handle):
                self.passes(self.app(**{HANDLE_KEY: handle}))

    def test_a_malformed_handle_is_refused(self) -> None:
        for handle in ("abcdefghijklmnop", "with.dot", "with space", "-dash", "x/y", "@name", "héllo", " ", "a\nb"):
            with self.subTest(handle=handle):
                self.refuses(self.app(**{HANDLE_KEY: handle}), HANDLE_KEY)

    def test_a_handle_that_is_not_a_string_is_refused(self) -> None:
        for value in (7, True, ["real_handle"]):
            with self.subTest(value=value):
                self.refuses(self.app(**{HANDLE_KEY: value}), HANDLE_KEY)

    def test_the_example_handle_is_refused_in_every_case(self) -> None:
        # What Developer.local.xcconfig.example names: copied unchanged, it would ship a link to it.
        for handle in ("example_handle", "EXAMPLE_HANDLE", "Example_Handle"):
            with self.subTest(handle=handle):
                self.refuses(self.app(**{HANDLE_KEY: handle}), "example")

    def test_a_bundle_without_an_info_plist_is_refused(self) -> None:
        app = self.root / "bare" / "Andarium.app"
        app.mkdir(parents=True)
        self.refuses(app, "Info.plist")


class UpdateAddress(Fixtures):
    """The update notice reads the maker's own address, or the build has none."""

    KEYS = {
        "UpdateSourceHost": "updates.maker.test",
        "UpdateSourcePath": "/apps/source.json",
        "UpdateLinkHosts": "*.cdn.maker.test files.maker.test",
        "UpdateAppIdentifier": "org.example.Fixture",
        "LSApplicationQueriesSchemes": ["altstore", "sidestore"],
    }

    def update(self, **changes: object) -> Path:
        return self.app(**{**self.KEYS, **changes})

    def test_a_bundle_with_no_address_has_no_notice(self) -> None:
        self.passes(self.app())
        self.passes(self.app(UpdateSourceHost=""))

    def test_a_complete_address_passes(self) -> None:
        self.passes(self.update())
        self.passes(self.update(UpdateLinkHosts=""))
        self.passes(self.update(UpdateLinkHosts=None))
        self.passes(self.update(UpdateLinkHosts="Files.Maker.Test"))
        self.passes(self.update(LSApplicationQueriesSchemes=["sidestore", "altstore", "other"]))

    def test_a_host_the_notice_does_not_read_is_refused(self) -> None:
        for host in ("Maker.Test", "maker", "a b.test", "-a.test", "a-.test", "a..test", ".test", "maker.test.",
                     "maker.test:8443", "https://maker.test", "maker.test/x", "user@maker.test", "mäker.test",
                     "a" * 64 + ".test", "a." * 130 + "test", 7):
            with self.subTest(host=host):
                self.refuses(self.update(UpdateSourceHost=host), "UpdateSourceHost")

    def test_the_example_address_is_refused(self) -> None:
        # What Update.local.xcconfig.example names: copied unchanged, it would ship a read of it.
        for host in ("example.com", "example.org", "example.net", "www.example.com", "a.b.example.org"):
            with self.subTest(host=host):
                self.refuses(self.update(UpdateSourceHost=host), "example")

    def test_a_path_the_notice_does_not_read_is_refused(self) -> None:
        for path in ("source.json", "", "/a b", "/a?b=1", "/a#b", "/" + "a" * 512, 7, "/a\n"):
            with self.subTest(path=path):
                self.refuses(self.update(UpdateSourcePath=path), "UpdateSourcePath")
        self.refuses(self.update(UpdateSourcePath=None), "UpdateSourcePath")

    def test_a_link_host_that_is_no_host_or_is_the_example_is_refused(self) -> None:
        for hosts in ("*", "*.", "a_b.c", "*.example.com", "example.org", "files.maker.test *.example.net",
                      "https://maker.test", "maker.test:443", 7):
            with self.subTest(hosts=hosts):
                self.refuses(self.update(UpdateLinkHosts=hosts), "UpdateLinkHosts")

    def test_the_original_identifier_is_the_bundles_own(self) -> None:
        # The build copies PRODUCT_BUNDLE_IDENTIFIER into the key; a tool re-signs the bundle, not the key.
        for identifier in (None, "", "org.example.Other", "org example", 7):
            with self.subTest(identifier=identifier):
                self.refuses(self.update(UpdateAppIdentifier=identifier), "UpdateAppIdentifier")

    def test_the_sideloading_apps_must_be_askable(self) -> None:
        # Without the two query schemes the system never says a tool is installed, and the rows never show.
        for schemes in (None, [], ["altstore"], ["sidestore"], ["other"], "altstore sidestore"):
            with self.subTest(schemes=schemes):
                self.refuses(self.update(LSApplicationQueriesSchemes=schemes), "LSApplicationQueriesSchemes")

    def test_every_problem_is_named(self) -> None:
        done = run(self.update(UpdateSourceHost="example.com", UpdateAppIdentifier="org.example.Other"))
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("UpdateSourceHost", done.stderr)
        self.assertIn("UpdateAppIdentifier", done.stderr)


# Libraries of one thing each, linked for the iPhone SDK. A path in a string is
# what a dependency compiles in from the prefix it was built under.
CLEAN = "int fixture(int value) { return value + 1; }\n"
HOME_PATH = 'const char *fixture(void) { return "/Users/NAME/work/project/file.c"; }\n'
VOLUME_PATH = 'const char *fixture(void) { return "/Volumes/NAME/prefix/share/locale"; }\n'
VOLUME_NAME_WITH_SPACE = 'const char *fixture(void) { return "/Volumes/NAME 2/prefix/lib"; }\n'
TEMPORARY_PATH = (
    "const char *fixture(void) {"
    ' return "/private" "/var/folders/NAME/T/source-build.abc123/stage"; }\n'
)
# Strings that are no path of a build: the volumes directory alone, a format for
# one, a different directory of a temporary tree, and the neutral directory the
# dependency builds compile in.
HARMLESS_PATHS = (
    "const char *const fixture[] = {\n"
    '  "/Volumes/", "/Volumes/%s", "/Volumes/%s/lib", "/Users/%s/Library", "/var/folders/", "/tmp/mesa_%s",\n'
    # Patterns and templates, as a tool's own help or matching code carries them.
    '  "/Users/[^/\\\\s\\"\']+", "/Users/<name>/Library", "/Users/$USER/Library", "/Volumes/{volume}/data",\n'
    '  "/Users/${USER}/x", "/Volumes/[^/]+/x", "/Users/(\\\\w+)/x", "/Users/\\\\w+/x", "/Users/.*/x",\n'
    '  "/Users/<user>/x", "/Users/%1$s/x", "/Volumes/%@/x",\n'
    '  "/nonexistent/share/locale",\n'
    "};\n"
)
# A symbol of the test-support library, as Swift mangles a type of its module.
TEST_SUPPORT_SYMBOL = "int _s23AndariumCoreTestSupport13GamepadScriptV7controllerSayAA0A10ControllerVGvgZ(void) { return 1; }\n"
SDK = ["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0"]


def tool(command: list[str], cwd: Path | None = None) -> None:
    done = subprocess.run(command, cwd=cwd, check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=120)
    if done.returncode != 0:
        raise AssertionError(f"{' '.join(command[:4])} failed:\n{done.stdout}")


class Libraries(Fixtures):
    """Application bundles around compiled libraries: the executable, and a framework named Runtime."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.libraries = Path(tempfile.mkdtemp(prefix="andarium-archive-"))
        cls.addClassCleanup(shutil.rmtree, cls.libraries, True)

    @classmethod
    def build(cls, name: str, source: str, *extra: str) -> Path:
        output = cls.libraries / f"lib{name}.dylib"
        if not output.exists():
            (cls.libraries / f"{name}.c").write_text(source, encoding="utf-8")
            tool([*SDK, "-dynamiclib", str(cls.libraries / f"{name}.c"), "-o", str(output),
                  f"-Wl,-install_name,@rpath/{output.name}", *extra])
        return output

    @classmethod
    def stub(cls, name: str, install_name: str) -> Path:
        """A library of one function that installs itself as `install_name`."""
        return cls.build(f"stub-{name}", f"int stub_{name.replace('-', '_')}(void) {{ return 1; }}\n",
                         f"-Wl,-install_name,{install_name}")

    @classmethod
    def build_loading(cls, name: str, *stubs: Path) -> Path:
        """A library that links the `stubs`, so that its load commands name their install names."""
        output = cls.libraries / f"lib{name}.dylib"
        if not output.exists():
            (cls.libraries / f"{name}.c").write_text(CLEAN, encoding="utf-8")
            tool([*SDK, "-dynamiclib", str(cls.libraries / f"{name}.c"), *map(str, stubs), "-o", str(output),
                  f"-Wl,-install_name,@rpath/{output.name}"])
        return output

    @classmethod
    def build_with_debug_map(cls, name: str, directory: str | None = None, work_name: str = "Volumes/NAME",
                             stubs: tuple[Path, ...] = (), source: str = CLEAN) -> Path:
        """A library linked from an object compiled with -g: the linker keeps a
        debug map whose N_OSO entry names the object file, which sits in
        `work_name` below the fixtures' directory (by default one that looks
        like a mounted volume). `directory` is the compilation directory the
        map's N_SO entries name. `stubs` are linked in."""
        output = cls.libraries / f"lib{name}.dylib"
        if not output.exists():
            work = cls.libraries / work_name
            work.mkdir(parents=True, exist_ok=True)
            (work / f"{name}.c").write_text(source, encoding="utf-8")
            tool([*SDK, "-g", *([f"-fdebug-compilation-dir={directory}"] if directory else []),
                  "-c", f"{name}.c" if directory else str(work / f"{name}.c"), "-o", str(work / f"{name}.o")],
                 cwd=work)
            tool([*SDK, "-dynamiclib", str(work / f"{name}.o"), *map(str, stubs), "-o", str(output),
                  f"-Wl,-install_name,@rpath/{output.name}"])
            assert f"/{work_name}/".encode() in output.read_bytes(), "the debug map names no object under the directory"
        return output

    @classmethod
    def strip_debug_map(cls, library: Path) -> Path:
        """The same library after `strip -S`, as the packaging leaves it."""
        stripped = library.with_name(f"{library.stem}-stripped.dylib")
        if not stripped.exists():
            shutil.copyfile(library, stripped)
            tool(["xcrun", "--sdk", "iphoneos", "strip", "-S", str(stripped)])
        return stripped

    def bundle(self, executable: Path, framework: Path | None = None) -> Path:
        """An application whose executable is `executable` and which embeds `framework` as Runtime."""
        self.count += 1
        app = self.root / f"bundle-{self.count}" / "Andarium.app"
        app.mkdir(parents=True)
        shutil.copyfile(executable, app / "Andarium")
        (app / "Info.plist").write_bytes(plistlib.dumps(
            {"CFBundleIdentifier": "org.example.Fixture", "CFBundleExecutable": "Andarium"}))
        if framework is not None:
            runtime = app / "Frameworks" / "Runtime.framework"
            runtime.mkdir(parents=True)
            shutil.copyfile(framework, runtime / "Runtime")
            (runtime / "Info.plist").write_bytes(plistlib.dumps(
                {"CFBundleIdentifier": "org.example.Runtime", "CFBundleExecutable": "Runtime"}))
        return app


class HostTraces(Libraries):
    """A build leaves nothing of the machine that built it in the host code."""

    def test_clean_code_passes_as_the_executable_and_as_a_framework(self) -> None:
        clean = self.build("clean", CLEAN)
        self.passes(self.bundle(clean, framework=clean))

    def test_a_guest_section_may_carry_the_build_hosts_paths(self) -> None:
        # The kernel image rides in a section of its own, built elsewhere.
        guest = self.libraries / "guest.bin"
        guest.write_bytes(b"\0UML_ROOTFS\0/Users/NAME/built/here.c\0/Volumes/NAME/built/here.c\0")
        carrier = self.build("carrier", CLEAN, "-Wl,-sectcreate,__TEXT,__uml_elf," + str(guest))
        self.assertIn(b"/Users/NAME/built", carrier.read_bytes())
        self.passes(self.bundle(self.build("clean", CLEAN), framework=carrier))

    def test_a_home_directory_path_is_refused(self) -> None:
        homed, clean = self.build("homed", HOME_PATH), self.build("clean", CLEAN)
        self.refuses(self.bundle(homed), "/Users/NAME/work/project/file.c")
        self.refuses(self.bundle(clean, framework=homed),
                     "Runtime.framework/Runtime: names a path under a home directory")

    def test_a_volume_path_is_refused(self) -> None:
        volumed, spaced = self.build("volumed", VOLUME_PATH), self.build("spaced", VOLUME_NAME_WITH_SPACE)
        clean = self.build("clean", CLEAN)
        self.refuses(self.bundle(volumed), "/Volumes/NAME/prefix/share/locale")
        self.refuses(self.bundle(clean, framework=volumed),
                     "Runtime.framework/Runtime: names a path on a mounted volume")
        self.refuses(self.bundle(clean, framework=spaced), "/Volumes/NAME 2/prefix/lib")

    def test_a_volume_or_home_directory_of_any_name_is_refused(self) -> None:
        # Any character a path component can hold: spaces, parentheses, non-ASCII.
        clean = self.build("clean", CLEAN)
        for index, (kind, name) in enumerate((
            ("volume", "Build (CI)"), ("volume", "ビルド"), ("volume", "Build Data"),
            ("volume", "Build [2] & Co."), ("home", "Jane Doe"), ("home", "名前"), ("home", "o'brien (work)"),
            # A literal name that merely begins with a bracket, a brace, a
            # parenthesis or @ is a path: a disk called "[CI] Build SSD" is a
            # realistic build location. Only a recognizable pattern or
            # placeholder (below) is no path.
            ("volume", "[CI] Build SSD"), ("volume", "(Backup) Disk"), ("volume", "{CI} Disk"),
            ("volume", "@Build Disk"), ("volume", "{CI}"), ("home", "(work) jane"), ("home", "{jane} home"),
            ("home", "@jane"), ("home", "[ci] runner"), ("home", "<jane> home"), ("home", "$jane"),
        )):
            root = "Volumes" if kind == "volume" else "Users"
            named = self.build(f"named{index}", f'const char *fixture(void) {{ return "/{root}/{name}/prefix/lib"; }}\n')
            label = "a mounted volume" if kind == "volume" else "a home directory"
            with self.subTest(kind=kind, name=name):
                self.refuses(self.bundle(named), f"names a path on {label}" if kind == "volume" else f"names a path under {label}")
                self.refuses(self.bundle(clean, framework=named), "Runtime.framework/Runtime: names a path")

    def test_a_compiled_source_path_on_a_bracketed_volume_is_refused(self) -> None:
        # A framework's compiled __FILE__ string on a disk called "[CI] Build SSD".
        source = self.build("bracketed", 'const char *fixture(void) { return "/Volumes/[CI] Build SSD/checkout/source.c"; }\n')
        clean = self.build("clean", CLEAN)
        self.refuses(self.bundle(clean, framework=source),
                     "Runtime.framework/Runtime: names a path on a mounted volume (1): /Volumes/[CI] Build SSD/checkout/source.c")

    def test_a_temporary_directory_path_is_refused(self) -> None:
        temporary, clean = self.build("temporary", TEMPORARY_PATH), self.build("clean", CLEAN)
        self.refuses(self.bundle(temporary), "names a path in a temporary build directory")
        self.refuses(self.bundle(clean, framework=temporary), "/var/folders/NAME/T/source-build.abc123/stage")

    def test_strings_that_are_no_path_of_a_build_pass(self) -> None:
        harmless = self.build("harmless", HARMLESS_PATHS)
        self.passes(self.bundle(harmless, framework=harmless))

    def test_a_framework_with_a_debug_map_is_refused_and_the_executable_is_not(self) -> None:
        # Xcode strips the application's own executable when it archives, so
        # only an embedded framework's map is the check's concern.
        mapped, clean = self.build_with_debug_map("mapped"), self.build("clean", CLEAN)
        self.passes(self.bundle(mapped))
        self.refuses(self.bundle(clean, framework=mapped),
                     "Runtime.framework/Runtime: carries the linker's debug map")
        # The map is reported once, as the map: its object paths are no path of
        # the code.
        done = run(self.bundle(clean, framework=mapped))
        self.assertNotIn("mounted volume", done.stderr)

    def test_the_source_directory_of_a_debug_map_is_not_reported_as_a_path(self) -> None:
        mapped_home, clean = self.build_with_debug_map("mapped-home", "/Users/NAME/src"), self.build("clean", CLEAN)
        self.passes(self.bundle(mapped_home))
        app = self.bundle(clean, framework=mapped_home)
        self.refuses(app, "Runtime.framework/Runtime: carries the linker's debug map")
        self.assertNotIn("home directory", run(app).stderr)

    def test_a_framework_stripped_of_its_debug_map_passes(self) -> None:
        stripped = self.strip_debug_map(self.build_with_debug_map("mapped"))
        self.passes(self.bundle(stripped, framework=stripped))


class TestBundles(Libraries):
    """An application that tests hosted carries none of what ran them."""

    MEMBERS = (
        ("PlugIns/FixtureTests.xctest", True),
        ("Frameworks/XCTest.framework", True),
        ("Frameworks/XCUIAutomation.framework", True),
        ("Frameworks/Testing.framework", True),
        ("Frameworks/_Testing_Foundation.framework", True),
        ("Frameworks/AndariumCoreTestSupport.framework", True),
        ("Frameworks/AppIntentsTesting.framework", True),
        ("Frameworks/Evaluations.framework", True),
        ("Frameworks/StoreKitTest.framework", True),
        ("Frameworks/XCTestSupport.framework", True),
        ("Frameworks/libXCTestBundleInject.dylib", False),
        ("Frameworks/lib_TestingInterop.dylib", False),
        ("Frameworks/libXCTestSwiftSupport.dylib", False),
        ("Frameworks/libAndariumCoreTestSupport.dylib", False),
    )

    def test_a_test_bundle_or_test_framework_is_refused(self) -> None:
        clean = self.build("clean", CLEAN)
        for relative, is_directory in self.MEMBERS:
            with self.subTest(relative):
                app = self.bundle(clean, framework=clean)
                member = app / relative
                if is_directory:
                    member.mkdir(parents=True)
                    (member / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "org.example.Test"}))
                else:
                    shutil.copyfile(clean, member)
                self.refuses(app, f"{relative}: a test bundle or test framework")

    def test_the_applications_own_frameworks_are_not_test_support(self) -> None:
        clean = self.build("clean", CLEAN)
        app = self.bundle(clean, framework=clean)
        (app / "Frameworks" / "Support.framework").mkdir()
        (app / "PlugIns").mkdir()
        self.passes(app)


class LoadedTestLibraries(Libraries):
    """No load command names a library that only tests load, whatever is embedded."""

    INSTALL_NAMES = (
        "@rpath/Testing.framework/Testing",
        "@rpath/_Testing_Foundation.framework/_Testing_Foundation",
        "@rpath/XCTest.framework/XCTest",
        "@rpath/AppIntentsTesting.framework/AppIntentsTesting",
        "@rpath/libXCTestSwiftSupport.dylib",
        "@rpath/lib_TestingInterop.dylib",
        "@rpath/AndariumCoreTestSupport.framework/AndariumCoreTestSupport",
        # The developer directory of a platform holds only developer libraries.
        "/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/Library/Frameworks/Some.framework/Some",
        "/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/usr/lib/libSome.dylib",
    )

    def test_a_load_command_for_a_test_library_is_refused_with_no_copy_embedded(self) -> None:
        clean = self.build("clean", CLEAN)
        for index, install_name in enumerate(self.INSTALL_NAMES):
            loading = self.build_loading(f"loading{index}", self.stub(f"test{index}", install_name))
            self.assertIn(install_name.encode(), loading.read_bytes())
            with self.subTest(install_name):
                self.refuses(self.bundle(loading), f"Andarium: loads {install_name}")
                self.refuses(self.bundle(clean, framework=loading), f"Runtime.framework/Runtime: loads {install_name}")

    def test_the_load_commands_of_a_clean_library_pass(self) -> None:
        # The system's libraries and an embedded framework of the application's own.
        own = self.stub("own", "@rpath/Runtime.framework/Runtime")
        loading = self.build_loading("loading-clean", own)
        self.passes(self.bundle(loading, framework=self.build("clean", CLEAN)))


class LinkedTestSupport(Libraries):
    """No library that only tests link is linked into the application."""

    def test_a_linked_test_support_library_is_refused(self) -> None:
        linked, clean = self.build("linked", TEST_SUPPORT_SYMBOL), self.build("clean", CLEAN)
        self.refuses(self.bundle(linked), "Andarium: links AndariumCoreTestSupport")
        self.refuses(self.bundle(clean, framework=linked), "Runtime.framework/Runtime: links AndariumCoreTestSupport")

    def test_the_name_in_a_debug_map_only_is_not_linkage(self) -> None:
        # A Derived Data directory of that name puts it in the object paths and
        # the source directory of the executable's debug map, which Xcode
        # strips when it archives.
        mapped = self.build_with_debug_map(
            "mapped-support", "/derived/AndariumCoreTestSupport-build/src", "derived/AndariumCoreTestSupport-build")
        self.assertIn(b"AndariumCoreTestSupport-build", mapped.read_bytes())
        self.passes(self.bundle(mapped))

    def test_a_real_load_command_still_fails_beside_the_debug_map(self) -> None:
        stub = self.stub("support", "@rpath/AndariumCoreTestSupport.framework/AndariumCoreTestSupport")
        mapped = self.build_with_debug_map(
            "mapped-loading", None, "derived/AndariumCoreTestSupport-build", stubs=(stub,))
        self.refuses(self.bundle(mapped), "Andarium: loads @rpath/AndariumCoreTestSupport.framework/AndariumCoreTestSupport")

    def test_a_symbol_of_the_library_still_fails_with_a_debug_map(self) -> None:
        # Symbol names that a function's debug stab shares with the symbol are evidence too.
        mapped = self.build_with_debug_map(
            "mapped-symbol", None, "derived/Other-build", source=TEST_SUPPORT_SYMBOL)
        self.refuses(self.bundle(mapped), "Andarium: links AndariumCoreTestSupport")


if __name__ == "__main__":
    unittest.main(verbosity=2)
