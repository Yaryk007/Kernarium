#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher-application shell command's logic on a plain JVM, and what the image wires of it.

android/lineage-24/overlay/app_shortcuts is a Java tool that the image runs through app_process as the adb
shell user (Andarium's Add to Home Screen lists the launcher applications and starts one through it). Its
grammar, the label escaping, the listing's rule and bounds, the framing of every answer, the icon records, what is
checked of the renderer's answer, the launch's refusals and the exit statuses do not touch the platform:
AppsCommandTest.java drives them against a fake platform. The sources compile here without the Android glue
(AndroidPlatform and Main, which need the framework), and with IconProtocol, which the tool shares with the icon
renderer app (test_icon_renderer.py has the renderer); the Android calls are compiled by the build host's request and
exercised by the runner on an image.

The test also runs the Java rows on copies of the sources with one rule removed at a time (the escaping, a bound,
the launch refusal, the grammar, the digest's inputs, each check of the renderer's PNG, a renderer that is missing
or gone, the deadlines, the child process's drain and reaping), and requires the rows named here to fail: a row that
cannot tell the rule from its absence is no row. The Android glue (AndroidPlatform) cannot run on a JVM: what it must
not do (take a substitute for a lookup that failed, read the manifest's enabled flag, use the wall clock, read a label
for the device's own configuration, add resource files its own way, leave the image's build time out of a built-in
application's icon key, kill a child by anything but its pid, draw or decode anything of an application's) and how it
must reach the renderer (the provider held for the foreground user and released, a dead provider asked for again
once, the request that is the resource id and the files and nothing else) is checked on its text, and each of those
checks has its own mutant. The Java rows run on a 64 MiB heap, so that a label of megabytes does not fit a copy of
itself (the cleaning's bound is a row). The rows start real processes, some that ignore SIGTERM, and end each by
SIGKILL through its handle before the row returns. Each JVM also runs as the leader of a process group of its own,
which pins ownership: a process of that group that is still alive once the JVM has ended and a short grace has passed
is a leftover of this test and no one else's, so the test fails on it after ending the group by SIGKILL while the
JVM, not yet reaped, still holds the group's number (a mutant that stops a row halfway must not leave one behind). A
process listing is read for numbers only and a pid taken from it is never signalled; a listing that fails fails the
test; a timeout or an interrupt of the driver ends the groups too. Skipped (status 77) where the host has no JDK.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import concurrent.futures
import os
import pathlib
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
LINEAGE = REPOSITORY / "android" / "lineage-24"
TOOL = LINEAGE / "overlay" / "app_shortcuts"
SOURCES = TOOL / "src"
PROTOCOL = LINEAGE / "overlay" / "app_icons" / "src" / "com" / "kernarium" / "icons" / "IconProtocol.java"
TEST = HERE / "app_shortcuts" / "com" / "kernarium" / "apps" / "AppsCommandTest.java"
ANDROID_GLUE = ("AndroidPlatform.java", "Main.java")
PACKAGE_DIR = pathlib.Path("com") / "kernarium" / "apps"
SKIP = 77

# One rule removed per entry: (name, source file, the text that is replaced, its replacement, the rows that must fail).
MUTATIONS = [
    ("label-controls-pass", "LabelText.java",
     "if (type == Character.CONTROL || type == Character.LINE_SEPARATOR",
     "if (false || type == Character.LINE_SEPARATOR",
     {"label-controls-become-space", "list-hostile-labels"}),
    ("label-separators-pass", "LabelText.java",
     "|| type == Character.PARAGRAPH_SEPARATOR || type == Character.SPACE_SEPARATOR\n"
     "                || cp == 0x85 || cp == 0x1680 || Character.isWhitespace(cp)) {",
     "|| false) {",
     {"label-controls-become-space", "list-hostile-labels"}),
    ("label-bidi-passes", "LabelText.java",
     "        if (type == Character.FORMAT || cp == 0x061C || cp == 0x200E || cp == 0x200F\n"
     "                || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF) {",
     "        if (false) {",
     {"label-bidi-and-format-dropped", "list-hostile-labels"}),
    ("label-not-escaped-in-the-listing", "AppsCommand.java",
     "return LabelText.sanitize(platform.label(entry), entry.packageName);",
     "String raw = platform.label(entry);\n            return raw == null ? entry.packageName : raw;",
     {"list-hostile-labels"}),
    ("label-cleaned-whole", "LabelText.java",
     "            if (out.length() > CLEAN_LIMIT) {\n"
     "                break; // past both bounds already: cut() keeps less than this, so the rest cannot change the label\n"
     "            }\n",
     "",
     {"label-huge-is-cut-without-copying-it", "list-huge-label-is-one-row"}),
    ("label-cleaning-stops-too-soon", "LabelText.java",
     "static final int CLEAN_LIMIT = 2 * MAX_BYTES;",
     "static final int CLEAN_LIMIT = 10;",
     {"label-cleaning-stops-without-changing-the-label", "label-huge-is-cut-without-copying-it"}),
    ("label-out-of-memory-not-contained", "AppsCommand.java",
     "} catch (Exception | OutOfMemoryError problem) {",
     "} catch (Exception problem) {",
     {"list-label-out-of-memory-uses-the-package"}),
    ("label-failure-fails-the-listing", "AppsCommand.java",
     "            return LabelText.sanitize(null, entry.packageName);\n        }\n    }",
     "            throw new IllegalStateException(problem);\n        }\n    }",
     {"list-label-unreadable-uses-the-package", "list-label-out-of-memory-uses-the-package"}),
    ("label-surrogates-pass", "LabelText.java",
     "if (type == Character.SURROGATE || (cp & 0xFFFE) == 0xFFFE",
     "if (false || (cp & 0xFFFE) == 0xFFFE",
     {"label-surrogates-and-nonchars"}),
    ("label-not-cut", "LabelText.java",
     "static final int MAX_CODE_POINTS = 80;",
     "static final int MAX_CODE_POINTS = 8000;",
     {"label-bounds", "exit-statuses-are-the-contract"}),
    ("label-empty-not-replaced", "LabelText.java",
     "        if (text.isEmpty()) {\n            text = clean(fallback == null ? \"\" : fallback);\n        }\n",
     "",
     {"label-empty-uses-the-package"}),
    ("per-package-bound-removed", "Listing.java",
     "static final int MAX_PER_PACKAGE = 8;",
     "static final int MAX_PER_PACKAGE = 80000;",
     {"listing-per-package-bound", "exit-statuses-are-the-contract"}),
    ("row-bound-removed", "Listing.java",
     "static final int MAX_ROWS = 1024;",
     "static final int MAX_ROWS = 102400;",
     {"listing-row-bound", "list-sizes-within-the-bounds", "exit-statuses-are-the-contract"}),
    ("exported-not-required", "Entry.java",
     "return exported && enabled && permitted;",
     "return enabled && permitted;",
     {"listing-order-and-filter", "icons-not-launchable-is-missing", "launch-refuses-what-is-not-listed"}),
    ("enabled-not-required", "Entry.java",
     "return exported && enabled && permitted;",
     "return exported && permitted;",
     {"listing-order-and-filter", "icons-not-launchable-is-missing", "launch-refuses-what-is-not-listed"}),
    ("permission-not-required", "Entry.java",
     "return exported && enabled && permitted;",
     "return exported && enabled;",
     {"listing-order-and-filter", "icons-not-launchable-is-missing", "launch-refuses-what-is-not-listed"}),
    ("launch-refusal-removed", "AppsCommand.java",
     "if (find(platform.launcherActivities(user), component) == null) {",
     "if (false) {",
     {"launch-refuses-what-is-not-listed"}),
    ("launch-grammar-removed", "Component.java",
     "if (!isLetter(c) && c != '_' && c != '$' && (first || !isDigit(c))) {",
     "if (false) {",
     {"component-grammar-rejects", "launch-refuses-hostile-arguments-before-anything-runs"}),
    ("package-grammar-removed", "Component.java",
     "if (!isLetter(c) && !isDigit(c) && c != '_') {",
     "if (false) {",
     {"component-grammar-rejects", "launch-refuses-hostile-arguments-before-anything-runs"}),
    ("component-refusal-after-platform", "AppsCommand.java",
     "                if (component == null) {\n                    return refuse(err, \"not a component name\");\n                }\n",
     "                if (component == null) {\n                    continue;\n                }\n",
     {"launch-refuses-hostile-arguments-before-anything-runs", "icons-refuses-a-bad-component"}),
    ("launch-argv-through-a-shell", "AppsCommand.java",
     "\"-n\", component.token());",
     "\"-n\", \"'\" + component.token() + \"'\");",
     {"launch-starts-exactly-the-component"}),
    ("launch-error-line-ignored", "StartOutcome.java",
     "boolean error = line.startsWith(\"Error\");",
     "boolean error = false;",
     {"launch-failures", "start-outcome-kinds"}),
    ("error-matched-anywhere-in-a-line", "StartOutcome.java",
     "boolean error = line.startsWith(\"Error\");",
     "boolean error = line.contains(\"Error\");",
     {"launch-component-named-like-an-error", "start-outcome-kinds"}),
    ("exception-matched-anywhere-in-a-line", "StartOutcome.java",
     "if (error || line.startsWith(\"Exception\")) {",
     "if (error || line.contains(\"Exception\")) {",
     {"launch-component-named-like-an-error", "start-outcome"}),
    ("start-reason-copies-the-commands-text", "StartOutcome.java",
     "return ascii(\"cmd activity start-activity: \" + reason, MAX_REASON);",
     "return ascii(\"cmd activity start-activity: \" + reason + output, MAX_REASON);",
     {"start-outcome-kinds", "launch-failures"}),
    ("framework-message-printed", "AppsCommand.java",
     "return \"unexpected \" + kind;",
     "return String.valueOf(problem.getMessage());",
     {"failure-reasons-are-fixed"}),
    ("digest-ignores-language", "Listing.java",
     "        text.append(\"locales\\t\").append(locales).append('\\n');\n",
     "",
     {"digest-follows-the-set"}),
    ("digest-ignores-versions", "Listing.java",
     "'\\0' + entry.versionCode + '\\0' + entry.lastUpdateMillis + '\\0'",
     "'\\0' + '\\0' + '\\0'",
     {"digest-follows-the-set", "icon-key-follows-the-icon"}),
    ("digest-follows-the-order", "Listing.java",
     "        Collections.sort(eligible, ORDER);\n",
     "",
     {"listing-order-and-filter", "digest-follows-the-set"}),
    ("renderer-answer-unchecked", "AppsCommand.java",
     "made[0] = PngCheck.problem(icon) == null ? icon : null;",
     "made[0] = icon;",
     {"icons-answer-is-checked", "icons-statuses", "icons-bad-answer-costs-that-icon-alone"}),
    ("png-check-size-unchecked", "PngCheck.java",
     "        if (!sized) {\n            return \"a size that was not asked for\";\n        }\n",
     "",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-size-set-widened", "PngCheck.java",
     "sized |= icon.size == size;",
     "sized |= icon.size == size || icon.size == 64;",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-length-bound-removed", "PngCheck.java",
     "|| png.length > IconProtocol.MAX_PNG_BYTES) {",
     "|| false) {",
     {"png-check-length-bounds-are-exact", "png-check-refuses-what-a-renderer-must-not-be-believed-on"}),
    ("png-check-length-bound-off-by-one", "PngCheck.java",
     "|| png.length > IconProtocol.MAX_PNG_BYTES) {",
     "|| png.length > IconProtocol.MAX_PNG_BYTES + 1) {",
     {"png-check-length-bounds-are-exact"}),
    ("png-check-header-length-not-checked", "PngCheck.java",
     "png == null || png.length < HEADER_BYTES ||",
     "png == null ||",
     {"png-check-refuses-a-short-answer-without-failing"}),
    ("png-check-null-png-not-checked", "PngCheck.java",
     "png == null || png.length < HEADER_BYTES",
     "png.length < HEADER_BYTES",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on"}),
    ("png-check-signature-unchecked", "PngCheck.java",
     "            if (png[i] != AppsCommand.PNG_SIGNATURE[i]) {",
     "            if (false) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-signature-prefix-only", "PngCheck.java",
     "i < AppsCommand.PNG_SIGNATURE.length; i++",
     "i < AppsCommand.PNG_SIGNATURE.length - 1; i++",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-header-length-field-unchecked", "PngCheck.java",
     "if (integer(png, 8) != 13 || integer(png, 12) != 0x49484452) {",
     "if (integer(png, 12) != 0x49484452) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-header-type-unchecked", "PngCheck.java",
     "if (integer(png, 8) != 13 || integer(png, 12) != 0x49484452) {",
     "if (integer(png, 8) != 13) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-crc-unchecked", "PngCheck.java",
     "if (crc.getValue() != (integer(png, 29) & 0xFFFFFFFFL)) {",
     "if (false) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-crc-sign-extended", "PngCheck.java",
     "if (crc.getValue() != (integer(png, 29) & 0xFFFFFFFFL)) {",
     "if (crc.getValue() != integer(png, 29)) {",
     {"png-check-accepts-what-a-renderer-may-send", "icons-answer-is-checked"}),
    ("png-check-crc-over-too-little", "PngCheck.java",
     "crc.update(png, 12, 17);",
     "crc.update(png, 12, 16);",
     {"png-check-accepts-what-a-renderer-may-send", "icons-answer-is-checked"}),
    ("png-check-width-unchecked", "PngCheck.java",
     "if (integer(png, 16) != icon.size || integer(png, 20) != icon.size) {",
     "if (integer(png, 20) != icon.size) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-height-unchecked", "PngCheck.java",
     "if (integer(png, 16) != icon.size || integer(png, 20) != icon.size) {",
     "if (integer(png, 16) != icon.size) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-depth-unchecked", "PngCheck.java",
     "if (depth != 8 || (colour != 2 && colour != 6)",
     "if ((colour != 2 && colour != 6)",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-colour-type-unchecked", "PngCheck.java",
     "|| (colour != 2 && colour != 6) ||",
     "||",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-grey-with-alpha-accepted", "PngCheck.java",
     "(colour != 2 && colour != 6)",
     "(colour != 2 && colour != 6 && colour != 4)",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-compression-unchecked", "PngCheck.java",
     "|| png[26] != 0 || png[27] != 0 || png[28] != 0) {",
     "|| png[27] != 0 || png[28] != 0) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-filter-unchecked", "PngCheck.java",
     "|| png[26] != 0 || png[27] != 0 || png[28] != 0) {",
     "|| png[26] != 0 || png[28] != 0) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-interlace-unchecked", "PngCheck.java",
     "|| png[26] != 0 || png[27] != 0 || png[28] != 0) {",
     "|| png[26] != 0 || png[27] != 0) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("png-check-alpha-claim-unchecked", "PngCheck.java",
     "if (colour == 2 && icon.hasAlpha) {",
     "if (false) {",
     {"png-check-refuses-what-a-renderer-must-not-be-believed-on", "icons-answer-is-checked"}),
    ("renderer-gone-is-an-icon-error", "AppsCommand.java",
     "if (render.gone || render.unanswered) {",
     "if (render.unanswered) {",
     {"icons-renderer-lost-mid-batch-fails-the-command", "icons-renderer-is-released"}),
    ("an-unanswered-render-is-an-icon-error", "AppsCommand.java",
     "if (render.gone || render.unanswered) {",
     "if (render.gone) {",
     {"icons-an-unanswered-render-means-there-is-no-renderer"}),
    ("an-unanswered-render-defers-the-rest-and-fails-nothing", "AppsCommand.java",
     "if (render.gone || render.unanswered) {\n                        throw new PlatformException(ProviderLink.UNAVAILABLE);\n                    }\n",
     "if (render.gone) {\n                        throw new PlatformException(ProviderLink.UNAVAILABLE);\n                    }\n                    if (render.unanswered) {\n                        render.icon = null;\n                    }\n",
     {"icons-an-unanswered-render-means-there-is-no-renderer"}),
    ("an-answer-that-is-not-an-icon-is-no-renderer", "AppsCommand.java",
     "            render.icon = made[0];\n            render.gone = gone[0];",
     "            render.icon = made[0];\n            render.gone = gone[0];\n            render.unanswered = made[0] == null;",
     {"icons-a-reported-timeout-of-one-draw-is-that-icons-error", "icons-a-worker-that-failed-costs-that-icon-alone",
      "icons-bad-answer-costs-that-icon-alone"}),
    ("renderer-opened-for-the-foreground-user-by-now", "AppsCommand.java",
     "try (Renderer renderer = platform.openRenderer(user)) {",
     "try (Renderer renderer = platform.openRenderer(platform.currentUser())) {",
     {"icons-the-renderer-is-opened-for-the-listed-user"}),
    ("renderer-not-released", "AppsCommand.java",
     "try (Renderer renderer = platform.openRenderer(user)) {",
     "{\n            Renderer renderer = platform.openRenderer(user);",
     {"icons-renderer-is-released"}),
    ("renderer-opened-after-the-listing-for-drawn-icons-only", "AppsCommand.java",
     "        try (Renderer renderer = platform.openRenderer(user)) {\n            answer.line(\"kernarium_apps \" + FORMAT_VERSION);",
     "        try (Renderer renderer = components.stream().allMatch(c -> find(reported, c) == null || find(reported, c).iconResource == 0)\n                ? null : platform.openRenderer(user)) {\n            answer.line(\"kernarium_apps \" + FORMAT_VERSION);",
     {"icons-renderer-missing-fails-the-command"}),
    ("failed-icon-is-asked-again", "AppsCommand.java",
     "                Icon icon = renderer.render(entry);\n",
     "                Icon icon;\n                try {\n                    icon = renderer.render(entry);\n                } catch (RendererGone lost) {\n                    throw lost;\n                } catch (Exception again) {\n                    icon = renderer.render(entry);\n                }\n",
     {"icons-a-worker-that-failed-costs-that-icon-alone"}),
    ("renderer-lost-is-an-icon-error", "AppsCommand.java",
     "            } catch (RendererGone lost) {\n                gone[0] = true;\n            } catch (Throwable problem) {",
     "            } catch (Throwable problem) {",
     {"icons-renderer-lost-mid-batch-fails-the-command"}),
    ("icon-deadline-removed", "AppsCommand.java",
     "if (platform.nowMillis() - begin > ICON_DEADLINE_MILLIS) {",
     "if (false) {",
     {"icons-deadline-defers-the-rest"}),
    ("icon-render-failure-escapes", "AppsCommand.java",
     "            } catch (Throwable problem) {\n                made[0] = null;",
     "            } catch (Exception problem) {\n                made[0] = null;",
     {"icons-failing-render-is-silent"}),
    ("icon-render-time-limit-removed", "AppsCommand.java",
     "worker.join(platform.renderLimitMillis());",
     "worker.join(3_000);",
     {"icons-an-unanswered-render-means-there-is-no-renderer"}),
    ("icons-count-unbounded", "AppsCommand.java",
     "return args.length >= 2 && args.length <= 1 + MAX_ICONS;",
     "return args.length >= 2;",
     {"icons-count-and-duplicates"}),
    ("icons-duplicates-allowed", "AppsCommand.java",
     "if (new HashSet<>(components).size() != components.size()) {",
     "if (false) {",
     {"icons-count-and-duplicates"}),
    ("failure-prints-answer", "AppsCommand.java",
     "        if (status != EXIT_OK) {\n            return status;\n        }\n        try {\n            answer.writeTo(out);",
     "        try {\n            answer.writeTo(out);",
     {"launch-refuses-what-is-not-listed", "launch-failures"}),
    ("usage-accepts-extra-arguments", "AppsCommand.java",
     "            case \"list\":\n                return args.length == 1;",
     "            case \"list\":\n                return args.length >= 1;",
     {"usage"}),
    ("end-line-dropped", "AppsCommand.java",
     "        answer.line(\"end\\t\" + listing.rows.size());\n",
     "",
     {"list-format", "list-empty", "digest-answer"}),
    ("stderr-unbounded", "StartOutcome.java",
     "for (int i = 0; i < text.length() && out.length() < limit; i++) {",
     "for (int i = 0; i < text.length(); i++) {",
     {"stderr-is-one-bounded-line"}),
    ("stderr-not-ascii", "StartOutcome.java",
     "out.append(c >= 0x20 && c < 0x7F ? c : '?');",
     "out.append(c);",
     {"stderr-is-one-bounded-line"}),
    ("locale-tag-limited-to-35", "Listing.java",
     "if (tag == null || tag.isEmpty() || !isTag(tag)) {",
     "if (tag == null || tag.isEmpty() || tag.length() > 35 || !isTag(tag)) {",
     {"locale-line"}),
    ("digest-ignores-the-rest-of-the-locales", "Listing.java",
     "if (!restOfLocales.isEmpty()) {",
     "if (false) {",
     {"digest-follows-the-whole-locale-list"}),
    ("icon-key-ignores-the-resource-files", "Listing.java",
     "(entry.builtin ? '1' : '0') + '\\0' + entry.resources;",
     "(entry.builtin ? '1' : '0');",
     {"icon-key-follows-the-icon", "digest-follows-the-set"}),
    ("icon-key-ignores-the-image-build", "Listing.java",
     "+ entry.lastUpdateMillis + '\\0' + entry.imageBuildMillis + '\\0'",
     "+ entry.lastUpdateMillis + '\\0'",
     {"icon-key-follows-the-icon", "digest-follows-the-set"}),
    ("render-version-not-bumped", "Listing.java",
     "static final int RENDER_VERSION = 3;",
     "static final int RENDER_VERSION = 2;",
     {"icon-key-follows-the-icon"}),
    ("resource-overlays-dropped", "ResourcePaths.java",
     "            items.add(new Item(Kind.OVERLAY, overlay));\n",
     "",
     {"resource-paths-follow-the-framework"}),
    ("resource-libraries-not-only-apks", "ResourcePaths.java",
     "if (present(library) && library.endsWith(\".apk\")) {",
     "if (present(library)) {",
     {"resource-paths-follow-the-framework"}),
    ("resource-base-is-the-private-source-dir", "ResourcePaths.java",
     "String base = present(publicSourceDir) ? publicSourceDir : sourceDir;",
     "String base = sourceDir;",
     {"resource-paths-follow-the-framework"}),
    ("resource-splits-dropped", "ResourcePaths.java",
     "String[] splits = splitPublicSourceDirs != null ? splitPublicSourceDirs : splitSourceDirs;",
     "String[] splits = null;",
     {"resource-paths-follow-the-framework"}),
    ("resource-overlay-order-reversed", "ResourcePaths.java",
     "new String[][] {overlayPaths, resourceDirs}",
     "new String[][] {resourceDirs, overlayPaths}",
     {"resource-paths-follow-the-framework"}),
    ("resource-overlays-not-deduplicated", "ResourcePaths.java",
     "if (present(overlay) && !overlays.contains(overlay)) {",
     "if (present(overlay)) {",
     {"resource-paths-follow-the-framework"}),
    ("child-output-not-drained", "ChildProcess.java",
     "            reader.start();\n",
     "",
     {"child-output-is-drained"}),
    ("child-not-terminated", "ChildProcess.java",
     "                ended = terminate(process, kill, graceMillis);\n",
     "",
     {"child-deadline-terminates-and-reaps", "child-is-reaped-when-the-run-is-interrupted",
      "child-that-ignores-termination-is-killed"}),
    ("child-kill-not-sent", "ChildProcess.java",
     "                    kill(process, kill);\n",
     "",
     {"child-that-ignores-termination-is-killed", "child-kill-is-asked-only-when-termination-did-not-end-it"}),
    ("child-kill-sent-even-when-termination-worked", "ChildProcess.java",
     "                if (!process.waitFor(graceMillis, TimeUnit.MILLISECONDS) && process.isAlive()) {\n                    kill(process, kill);",
     "                if (true) {\n                    kill(process, kill);",
     {"child-kill-is-asked-only-when-termination-did-not-end-it"}),
    ("child-kill-sent-to-a-child-that-ended-in-the-wait", "ChildProcess.java",
     "TimeUnit.MILLISECONDS) && process.isAlive()) {\n                    kill(process, kill);",
     "TimeUnit.MILLISECONDS)) {\n                    kill(process, kill);",
     {"child-is-not-signalled-after-it-has-ended"}),
    ("child-kill-sent-to-a-child-that-ended-before-the-interrupt-was-seen", "ChildProcess.java",
     "            if (process.isAlive()) {\n                kill(process, kill);\n            }\n            Thread.currentThread().interrupt();",
     "            kill(process, kill);\n            Thread.currentThread().interrupt();",
     {"child-is-not-signalled-after-it-has-ended"}),
    ("child-survivor-not-reported", "ChildProcess.java",
     "            if (!ended) {\n                throw new PlatformException(\"cmd activity start-activity could not be ended\");\n            }\n",
     "",
     {"child-that-survives-the-kill-is-reported", "child-kill-that-throws-leaves-the-wait-to-decide"}),
    ("child-final-wait-result-ignored", "ChildProcess.java",
     "            return !process.isAlive();\n        } catch (InterruptedException interrupted) {",
     "            return true;\n        } catch (InterruptedException interrupted) {",
     {"child-that-survives-the-kill-is-reported", "child-kill-that-throws-leaves-the-wait-to-decide"}),
    ("child-deadline-is-not-a-failure", "ChildProcess.java",
     "throw new PlatformException(\"cmd activity start-activity did not finish\");",
     "return new Result(-1, \"\");",
     {"child-deadline-terminates-and-reaps"}),
    ("link-acquires-for-another-user", "ProviderLink.java",
     "holding = mTransport.acquire(mUser);", "holding = mTransport.acquire(0);",
     {"link-the-user-is-one-for-acquiring-reacquiring-and-releasing", "link-takes-the-images-provider-only"}),
    ("link-releases-for-another-user", "ProviderLink.java",
     "        if (held) {\n            mTransport.release(mUser);", "        if (held) {\n            mTransport.release(0);",
     {"link-the-user-is-one-for-acquiring-reacquiring-and-releasing"}),
    ("link-releases-a-stranger-for-another-user", "ProviderLink.java",
     "mTransport.release(mUser); // an unexpected answerer", "mTransport.release(0); // an unexpected answerer",
     {"link-takes-the-images-provider-only", "link-checks-who-answers-on-every-acquisition"}),
    ("link-takes-any-package", "ProviderLink.java",
     "return IconProtocol.PACKAGE.equals(holding.packageName())\n                && ", "return true\n                && ",
     {"link-takes-the-images-provider-only"}),
    ("link-takes-any-class", "ProviderLink.java",
     "IconProtocol.PROVIDER_CLASS.equals(holding.className())", "true",
     {"link-takes-the-images-provider-only"}),
    ("link-takes-a-class-by-its-prefix", "ProviderLink.java",
     "IconProtocol.PROVIDER_CLASS.equals(holding.className())", "holding.className().startsWith(IconProtocol.PROVIDER_CLASS)",
     {"link-takes-the-images-provider-only"}),
    ("link-takes-an-installed-package", "ProviderLink.java",
     "IconProtocol.PROVIDER_CLASS.equals(holding.className()) && holding.system();", "IconProtocol.PROVIDER_CLASS.equals(holding.className());",
     {"link-takes-the-images-provider-only"}),
    ("link-takes-an-answerer-it-cannot-identify", "ProviderLink.java",
     "        boolean keep = false;", "        boolean keep = true;",
     {"link-takes-the-images-provider-only"}),
    ("link-keeps-a-stranger", "ProviderLink.java",
     "        mTransport.release(mUser); // an unexpected answerer, or the link was closed while it was asked: not kept\n",
     "",
     {"link-takes-the-images-provider-only", "link-checks-who-answers-on-every-acquisition",
      "link-a-hold-that-arrives-after-close-is-given-back"}),
    ("link-no-provider-is-taken", "ProviderLink.java",
     "        if (holding == null) {\n            throw new PlatformException(UNAVAILABLE);\n        }\n", "",
     {"link-takes-the-images-provider-only"}),
    ("link-says-the-frameworks-words", "ProviderLink.java",
     "throw new PlatformException(UNAVAILABLE, problem);\n        }\n        if (holding == null)",
     "throw new PlatformException(String.valueOf(problem.getMessage()), problem);\n        }\n        if (holding == null)",
     {"link-takes-the-images-provider-only"}),
    ("link-trusts-the-first-answerer-only", "ProviderLink.java",
     "        try {\n            return acquire();\n        } catch (PlatformException unreachable) {",
     "        try {\n            Holding unchecked = mTransport.acquire(mUser);\n            synchronized (this) {\n                mHolding = unchecked;\n                mHeld = true;\n            }\n            return unchecked;\n        } catch (Exception unreachable) {",
     {"link-checks-who-answers-on-every-acquisition"}),
    ("link-keeps-a-hold-after-close", "ProviderLink.java",
     "if (keep && !mClosed) {", "if (keep) {",
     {"link-a-hold-that-arrives-after-close-is-given-back"}),
    ("link-acquires-after-close", "ProviderLink.java",
     "        synchronized (this) {\n            if (mClosed) {\n                throw new PlatformException(UNAVAILABLE);\n            }\n        }\n        Holding holding;",
     "        Holding holding;",
     {"link-close-is-final-and-release-is-idempotent"}),
    ("link-close-is-not-final", "ProviderLink.java",
     "        synchronized (this) {\n            mClosed = true;\n        }\n        release();", "        release();",
     {"link-close-is-final-and-release-is-idempotent", "link-a-hold-that-arrives-after-close-is-given-back"}),
    ("link-release-is-not-idempotent", "ProviderLink.java",
     "            held = mHeld;\n            mHeld = false;\n", "            held = mHeld;\n",
     {"link-close-is-final-and-release-is-idempotent", "link-an-abandoned-call-takes-no-hold-after-close"}),
    ("link-releases-what-it-never-held", "ProviderLink.java",
     "        if (held) {\n            mTransport.release(mUser);", "        if (true) {\n            mTransport.release(mUser);",
     {"link-close-is-final-and-release-is-idempotent", "link-an-abandoned-call-takes-no-hold-after-close"}),
    ("link-a-death-is-not-released", "ProviderLink.java",
     "        } catch (ProviderDied dead) {\n            release(); // the next icon",
     "        } catch (ProviderDied dead) {\n            // the next icon",
     {"link-the-user-is-one-for-acquiring-reacquiring-and-releasing", "link-an-abandoned-call-takes-no-replacement-hold"}),
    ("link-a-death-takes-a-hold-for-nobody", "ProviderLink.java",
     "            release(); // the next icon takes a fresh hold; nothing takes one for an icon that nobody waits for\n",
     "            release();\n            renewed();\n",
     {"link-an-abandoned-call-takes-no-replacement-hold", "link-a-provider-that-died-costs-one-icon",
      "link-checks-who-answers-on-every-acquisition"}),
    ("link-a-death-is-not-asked-for-by-the-next-icon", "ProviderLink.java",
     "        if (holding == null) {\n            holding = renewed();\n        }\n",
     "        if (holding == null) {\n            throw new PlatformException(\"the icon renderer died\");\n        }\n",
     {"link-the-user-is-one-for-acquiring-reacquiring-and-releasing", "link-a-provider-that-died-costs-one-icon"}),
    ("link-a-provider-that-is-not-there-again-is-an-icon-error", "ProviderLink.java",
     "throw new AppsCommand.RendererGone(UNAVAILABLE, unreachable);", "return null;",
     {"link-a-provider-that-died-costs-one-icon", "link-checks-who-answers-on-every-acquisition"}),
    ("link-a-death-is-no-error-for-the-icon", "ProviderLink.java",
     "            throw new PlatformException(\"the icon renderer died\");", "            return null;",
     {"link-the-user-is-one-for-acquiring-reacquiring-and-releasing", "link-a-provider-that-died-costs-one-icon"}),
    ("link-an-answer-that-is-not-ok-is-taken", "ProviderLink.java",
     "        if (icon == null) {", "        if (false) {",
     {"link-an-answer-that-is-not-an-ok-keeps-the-hold"}),
    ("link-an-answer-that-is-not-ok-drops-the-hold", "ProviderLink.java",
     "        if (icon == null) {\n", "        if (icon == null) {\n            release();\n",
     {"link-an-answer-that-is-not-an-ok-keeps-the-hold"}),
]

# What the Android glue must and must not do, as text (it cannot run on a JVM): (message, test of the comment-free text).
# One mutant for each: (name, text replaced, replacement, the message that must be reported).
PLATFORM_RULES = [
    ("no substitute for the foreground user", lambda t: "UserHandle" not in t),
    ("no process default for the locales", lambda t: "LocaleList.getDefault" not in t),
    ("the foreground user's lookup fails the command", lambda t: 'throw new PlatformException("the foreground user is not known"' in t),
    ("a failed permission check fails the command", lambda t: 'throw new PlatformException("a permission could not be checked"' in t),
    ("a failed configuration read fails the command", lambda t: t.count('throw new PlatformException("the system\'s configuration cannot be read"') == 2),
    ("a package gone since the query is left out",
     lambda t: re.search(r"if \(packageInfo == null\) \{\s*continue;", t) is not None and "packageInfo == null ?" not in t),
    ("no catch swallows a failure",
     lambda t: re.search(r"catch \((?:Exception|RuntimeException|Throwable) \w+\) \{\s*\}", t) is None),
    ("enabled is the resolver's answer, not the manifest flag", lambda t: "isEnabled(" not in t and ".enabled" not in t
     and re.search(r"info\.exported,\s*true,\s*permitted\(", t) is not None),
    ("the icon deadline uses the monotonic clock", lambda t: "SystemClock.elapsedRealtime()" in t and "currentTimeMillis" not in t),
    ("labels are read for a fixed configuration and the digest's locales alone",
     lambda t: "UI_MODE_NIGHT_NO" in t and "fixedConfiguration(configuration().getLocales())" in t
     and "new Configuration(configuration())" not in t and "ICON_" not in t),
    ("no icon is drawn, decoded or parsed in this process: no image, drawable, canvas or XML of an application is touched",
     lambda t: all(word not in t for word in ("Bitmap", "Canvas", "Drawable", "getDrawable", "XmlResourceParser", "XmlPullParser",
                                              "getXml(", "openRawResource", "BitmapFactory", "ImageDecoder", "TypedValue",
                                              "getValueForDensity", "getResourceTypeName"))),
    ("the icon renderer's provider is held through the activity manager, for the user the command was opened for, and released",
     lambda t: "ActivityManager.getService().getContentProviderExternal( IconProtocol.AUTHORITY, user, mToken, AppsCommand.NAME)" in flat(t)
     and "removeContentProviderExternalAsUser(IconProtocol.AUTHORITY, mToken, user)" in t
     and "private final IBinder mToken = new Binder();" in t
     and "mLink = new ProviderLink(new ActivityManagerTransport(), user);" in t
     and "public AppsCommand.Renderer openRenderer(int user) throws Exception { return new ProviderRenderer(user);" in flat(t)
     and "public void close() { mLink.close();" in flat(t)
     and "mUser" not in t and "platform.currentUser()" not in t),
    ("a holder with no provider is released and is no provider",
     lambda t: re.search(r"if \(holder == null\) \{\s*return null;\s*\}\s*if \(holder\.provider == null\) \{\s*release\(user\);\s*"
                         r"return null;\s*\}", t) is not None),
    ("who answered is read from the holder: package, class and whether it is a system application",
     lambda t: "mHolder.info == null ? null : mHolder.info.packageName;" in t
     and "mHolder.info == null ? null : mHolder.info.name;" in t
     and re.search(r"mHolder\.info != null && mHolder\.info\.applicationInfo != null\s*"
                   r"&& \(mHolder\.info\.applicationInfo\.flags & ApplicationInfo\.FLAG_SYSTEM\) != 0;", t) is not None),
    ("the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them",
     lambda t: all(word in t for word in ("request.putInt(IconProtocol.KEY_RESOURCE, resource);",
                                          "request.putIntArray(IconProtocol.KEY_KINDS, kinds);",
                                          "request.putStringArray(IconProtocol.KEY_PATHS, paths);"))
     and t.count("request.put") == 3 and "items.size() > IconProtocol.MAX_FILES" in t and "!ResourcePaths.hasBase(items)" in t
     and "return mLink.render(entry.iconResource, kinds, paths);" in t
     and 'mHolder.provider.call(new AttributionSource(android.os.Process.myUid(), "com.android.shell", null), IconProtocol.AUTHORITY, '
         "IconProtocol.METHOD_RENDER, null, request)" in flat(t)),
    ("each kind of file is sent as its own kind",
     lambda t: all(f"kinds[i] = IconProtocol.KIND_{kind};" in t for kind in ("SPLIT", "LIBRARY", "OVERLAY", "BASE"))
     and all(f"case {kind}:" in t for kind in ("SPLIT", "LIBRARY", "OVERLAY"))),
    ("the answer is the status, the size and the two flags, and the bytes, and an answer that is not ok is no icon",
     lambda t: "answer.getInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR) != IconProtocol.STATUS_OK" in t
     and all(f"IconProtocol.KEY_{key}" in t for key in ("SIZE", "ADAPTIVE", "ALPHA", "PNG"))
     and "answer == null" in t),
    ("a provider that died is the link's to deal with: the call's death is reported as one",
     lambda t: re.search(r"catch \(DeadObjectException dead\) \{\s*throw new ProviderLink\.ProviderDied\(dead\);\s*\}", t) is not None),
    ("a built-in application that was never updated carries the image's build time as an input of its key",
     lambda t: re.search(r"FLAG_SYSTEM\) != 0;", t) is not None
     and re.search(r"long imageBuild = builtIn && \(application\.flags & ApplicationInfo\.FLAG_UPDATED_SYSTEM_APP\) == 0\s*"
                   r"\? Build\.TIME : 0;", t) is not None
     and re.search(r"packageInfo\.lastUpdateTime, imageBuild, info\.getIconResource\(\)", t) is not None
     and "Math.max(" not in t),
    ("a child that ignores termination is killed by its pid",
     lambda t: "android.os.Process.killProcess(field.getInt(process))" in t and 'getDeclaredField("pid")' in t
     and "destroyForcibly" not in t and "new KillByPid()" in t
     and re.search(r"public void kill\(Process process\) throws Exception \{\s*if \(!process\.isAlive\(\)\) \{\s*return;", t)
     is not None),
    ("every resource file the framework adds is added",
     lambda t: all(word in t for word in ("addAssetPathAsSharedLibrary(", "addOverlayPath(", "ResourcePaths.of(",
                                          "application.publicSourceDir", "application.splitPublicSourceDirs",
                                          "application.sharedLibraryFiles", "application.resourceDirs",
                                          "application.overlayPaths"))),
    ("a base or split that cannot be added leaves the resources unopened; a library or overlay is skipped, as the framework does",
     lambda t: re.search(r"complete = cookie != 0 \|\| item\.kind == ResourcePaths\.Kind\.OVERLAY\s*"
                         r"\|\| item\.kind == ResourcePaths\.Kind\.LIBRARY;", t) is not None),
    ("the system assets are built, without the framework's overlay idmaps, before the first AssetManager, and restored on failure",
     lambda t: "systemAssetsWithoutIdmaps();" in t and "new AssetManager()" in t
     and t.index("systemAssetsWithoutIdmaps();") < t.index("new AssetManager()")
     and all(word in t for word in ('"/system/framework/framework-res.apk"', '"/system/framework/org.lineageos.platform-res.apk"',
                                    '"sSystem"', '"sSystemApkAssets"', '"sSystemApkAssetsSet"',
                                    "set.set(null, previousSet);", "array.set(null, previousArray);",
                                    "setApkAssets(all, false)"))
     and "ApkAssets.loadFromPath(" in t),
    ("the system AssetManager is published last, and any failure, an allocation failure too, puts the statics back",
     lambda t: "manager.setApkAssets(all, false);" in t and "system.set(null, manager);" in t
     and t.index("manager.setApkAssets(all, false);") < t.index("system.set(null, manager);")
     and re.search(r"catch \(Exception \| LinkageError problem\) \{\s*restoreSystemAssets\(touched,", t) is not None
     and re.search(r"catch \(Error problem\) \{\s*restoreSystemAssets\(touched,[^;]*;\s*"
                   r"sSystemAssetsBuilt = false;\s*throw problem;", t) is not None
     and re.search(r"if \(!touched\) \{\s*return;", t) is not None),
    ("the start is run by ChildProcess", lambda t: "ChildProcess.run(AppsCommand.startArguments(" in t
     and "ProcessBuilder" not in t and "waitFor" not in t),
]

PLATFORM_MUTATIONS = [
    ("system-assets-left-to-the-framework",
     "            systemAssetsWithoutIdmaps();\n            AssetManager assets = new AssetManager();",
     "            AssetManager assets = new AssetManager();",
     "the system assets are built, without the framework's overlay idmaps, before the first AssetManager, and restored on failure"),
    ("system-assets-built-too-late",
     "            systemAssetsWithoutIdmaps();\n            AssetManager assets = new AssetManager();",
     "            AssetManager assets = new AssetManager();\n            systemAssetsWithoutIdmaps();",
     "the system assets are built, without the framework's overlay idmaps, before the first AssetManager, and restored on failure"),
    ("system-assets-failure-keeps-a-half-built-state",
     "        } catch (Exception | LinkageError problem) {\n            restoreSystemAssets(touched, array, previousArray, set, previousSet);\n        } catch (Error problem) {",
     "        } catch (Exception | LinkageError problem) {\n        } catch (Error problem) {",
     "the system AssetManager is published last, and any failure, an allocation failure too, puts the statics back"),
    ("system-assets-published-before-they-have-their-assets",
     "            manager.setApkAssets(all, false);\n            system.set(null, manager);",
     "            system.set(null, manager);\n            manager.setApkAssets(all, false);",
     "the system AssetManager is published last, and any failure, an allocation failure too, puts the statics back"),
    ("system-assets-allocation-failure-left-half-built",
     "        } catch (Error problem) {\n            restoreSystemAssets(touched, array, previousArray, set, previousSet);\n            sSystemAssetsBuilt = false;\n            throw problem;\n        }",
     "        }",
     "the system AssetManager is published last, and any failure, an allocation failure too, puts the statics back"),
    ("system-assets-restored-before-they-were-touched",
     "        if (!touched) {\n            return;\n        }\n",
     "",
     "the system AssetManager is published last, and any failure, an allocation failure too, puts the statics back"),
    ("foreground-user-substituted",
     "throw new PlatformException(\"the foreground user is not known\", problem);",
     "return android.os.UserHandle.myUserId();",
     "no substitute for the foreground user"),
    ("locales-default-substituted",
     "throw new PlatformException(\"the system's configuration cannot be read\", problem);",
     "return new Configuration();",
     "a failed configuration read fails the command"),
    ("permission-check-swallowed",
     "throw new PlatformException(\"a permission could not be checked\", problem);",
     "known = false;",
     "a failed permission check fails the command"),
    ("vanished-package-listed",
     "            if (packageInfo == null) {\n                continue; // removed since the query\n            }\n",
     "",
     "a package gone since the query is left out"),
    ("manifest-enabled-flag-read",
     "info.exported, true, permitted(info.permission)",
     "info.exported, info.isEnabled(), permitted(info.permission)",
     "enabled is the resolver's answer, not the manifest flag"),
    ("wall-clock-for-the-deadline",
     "return SystemClock.elapsedRealtime();",
     "return System.currentTimeMillis();",
     "the icon deadline uses the monotonic clock"),
    ("label-read-for-the-device-configuration",
     "fixedConfiguration(configuration().getLocales())",
     "new Configuration(configuration())",
     "labels are read for a fixed configuration and the digest's locales alone"),
    ("image-build-left-out-of-the-icon-key",
     "packageInfo.lastUpdateTime, imageBuild, info.getIconResource()",
     "packageInfo.lastUpdateTime, 0, info.getIconResource()",
     "a built-in application that was never updated carries the image's build time as an input of its key"),
    ("image-build-merged-into-the-update-time",
     "packageInfo.lastUpdateTime, imageBuild, info.getIconResource()",
     "Math.max(packageInfo.lastUpdateTime, imageBuild), 0, info.getIconResource()",
     "a built-in application that was never updated carries the image's build time as an input of its key"),
    ("updated-built-in-also-takes-the-image-build",
     "long imageBuild = builtIn && (application.flags & ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0\n"
     "                    ? Build.TIME : 0;",
     "long imageBuild = builtIn ? Build.TIME : 0;",
     "a built-in application that was never updated carries the image's build time as an input of its key"),
    ("kill-by-destroyForcibly",
     "android.os.Process.killProcess(field.getInt(process));",
     "process.destroyForcibly();",
     "a child that ignores termination is killed by its pid"),
    ("kill-by-pid-without-the-alive-check",
     "            if (!process.isAlive()) {\n                return; // a number that was the child's may be another process's now\n            }\n",
     "",
     "a child that ignores termination is killed by its pid"),
    ("kill-not-passed-to-the-child-run",
     "OUTPUT_LIMIT, new KillByPid());",
     "OUTPUT_LIMIT, process -> { });",
     "a child that ignores termination is killed by its pid"),
    ("overlays-not-added",
     "cookie = assets.addOverlayPath(item.path);",
     "cookie = 1;",
     "every resource file the framework adds is added"),
    ("split-failure-ignored",
     "complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY\n                        || item.kind == ResourcePaths.Kind.LIBRARY;",
     "complete = true;",
     "a base or split that cannot be added leaves the resources unopened; a library or overlay is skipped, as the framework does"),
    ("library-failure-fails-the-package",
     "complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY\n                        || item.kind == ResourcePaths.Kind.LIBRARY;",
     "complete = cookie != 0 || item.kind == ResourcePaths.Kind.OVERLAY;",
     "a base or split that cannot be added leaves the resources unopened; a library or overlay is skipped, as the framework does"),
    ("start-waits-before-reading",
     "ChildProcess.run(AppsCommand.startArguments(user, component),",
     "new ProcessBuilder(AppsCommand.startArguments(user, component)).start().waitFor(); ChildProcess.run(AppsCommand.startArguments(user, component),",
     "the start is run by ChildProcess"),
    ("renderer-reads-the-icon-in-the-shell",
     "    private static final class ProviderRenderer implements AppsCommand.Renderer {",
     "    private static android.graphics.Bitmap decoded(byte[] bytes) {\n        return android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.length);\n    }\n\n"
     "    private static final class ProviderRenderer implements AppsCommand.Renderer {",
     "no icon is drawn, decoded or parsed in this process: no image, drawable, canvas or XML of an application is touched"),
    ("renderer-parses-xml-in-the-shell",
     "    private static final class ProviderRenderer implements AppsCommand.Renderer {",
     "    private static Object parsed(Resources resources, int id) throws Exception {\n        return resources.getXml(id);\n    }\n\n"
     "    private static final class ProviderRenderer implements AppsCommand.Renderer {",
     "no icon is drawn, decoded or parsed in this process: no image, drawable, canvas or XML of an application is touched"),
    ("provider-held-for-another-user",
     "IconProtocol.AUTHORITY, user, mToken, AppsCommand.NAME)",
     "IconProtocol.AUTHORITY, 0, mToken, AppsCommand.NAME)",
     "the icon renderer's provider is held through the activity manager, for the user the command was opened for, and released"),
    ("provider-released-for-another-user",
     "removeContentProviderExternalAsUser(IconProtocol.AUTHORITY, mToken, user)",
     "removeContentProviderExternalAsUser(IconProtocol.AUTHORITY, mToken, 0)",
     "the icon renderer's provider is held through the activity manager, for the user the command was opened for, and released"),
    ("provider-never-released",
     "removeContentProviderExternalAsUser(IconProtocol.AUTHORITY, mToken, user)",
     "getContentProviderExternal(IconProtocol.AUTHORITY, user, mToken, AppsCommand.NAME)",
     "the icon renderer's provider is held through the activity manager, for the user the command was opened for, and released"),
    ("renderer-close-does-not-release",
     "        public void close() {\n            mLink.close();",
     "        public void close() {\n            mLink.hashCode();",
     "the icon renderer's provider is held through the activity manager, for the user the command was opened for, and released"),
    ("renderer-takes-the-foreground-user-again",
     "mLink = new ProviderLink(new ActivityManagerTransport(), user);",
     "mLink = new ProviderLink(new ActivityManagerTransport(), ActivityManager.getCurrentUser());",
     "the icon renderer's provider is held through the activity manager, for the user the command was opened for, and released"),
    ("holder-without-a-provider-is-kept",
     "            if (holder.provider == null) {\n                release(user);\n                return null;\n            }\n",
     "",
     "a holder with no provider is released and is no provider"),
    ("holder-without-a-provider-is-not-released",
     "                release(user);\n                return null;",
     "                return null;",
     "a holder with no provider is released and is no provider"),
    ("null-holder-passes",
     "            if (holder == null) {\n                return null;\n            }\n", "",
     "a holder with no provider is released and is no provider"),
    ("package-is-not-read-from-the-holder",
     "mHolder.info == null ? null : mHolder.info.packageName;",
     "IconProtocol.PACKAGE;",
     "who answered is read from the holder: package, class and whether it is a system application"),
    ("class-is-not-read-from-the-holder",
     "mHolder.info == null ? null : mHolder.info.name;",
     "IconProtocol.PROVIDER_CLASS;",
     "who answered is read from the holder: package, class and whether it is a system application"),
    ("provenance-is-not-read-from-the-holder",
     "&& (mHolder.info.applicationInfo.flags & ApplicationInfo.FLAG_SYSTEM) != 0;",
     "&& mHolder.info.applicationInfo.flags != 0;",
     "who answered is read from the holder: package, class and whether it is a system application"),
    ("request-leaves-out-the-resource",
     "            request.putInt(IconProtocol.KEY_RESOURCE, resource);\n", "",
     "the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them"),
    ("request-carries-the-label-too",
     "            request.putIntArray(IconProtocol.KEY_KINDS, kinds);",
     "            request.putIntArray(IconProtocol.KEY_KINDS, kinds);\n            request.putString(\"label\", \"x\");",
     "the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them"),
    ("request-unbounded",
     "|| items.size() > IconProtocol.MAX_FILES", "",
     "the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them"),
    ("request-without-a-base-check",
     "if (!ResourcePaths.hasBase(items) ||", "if (",
     "the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them"),
    ("request-called-as-another-package",
     "\"com.android.shell\", null)", "\"android\", null)",
     "the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them"),
    ("request-for-another-resource",
     "return mLink.render(entry.iconResource, kinds, paths);", "return mLink.render(0, kinds, paths);",
     "the request is the icon's resource id and the files, and nothing else, as the renderer's protocol names them"),
    ("overlays-sent-as-splits",
     "                    case OVERLAY:\n                        kinds[i] = IconProtocol.KIND_OVERLAY;",
     "                    case OVERLAY:\n                        kinds[i] = IconProtocol.KIND_SPLIT;",
     "each kind of file is sent as its own kind"),
    ("libraries-sent-as-overlays",
     "                    case LIBRARY:\n                        kinds[i] = IconProtocol.KIND_LIBRARY;",
     "                    case LIBRARY:\n                        kinds[i] = IconProtocol.KIND_OVERLAY;",
     "each kind of file is sent as its own kind"),
    ("an-answer-that-is-not-ok-is-taken",
     "answer.getInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR) != IconProtocol.STATUS_OK",
     "answer.getInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_OK) != IconProtocol.STATUS_OK",
     "the answer is the status, the size and the two flags, and the bytes, and an answer that is not ok is no icon"),
    ("a-missing-answer-is-taken",
     "answer == null ||", "",
     "the answer is the status, the size and the two flags, and the bytes, and an answer that is not ok is no icon"),
    ("a-dead-provider-is-no-death",
     "                throw new ProviderLink.ProviderDied(dead);\n",
     "                return null;\n",
     "a provider that died is the link's to deal with: the call's death is reported as one"),
]


def jdk_tool(name: str) -> str | None:
    """A working javac or java: the macOS /usr/bin stubs exist without a JDK, so ask it."""
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


# The rows start real processes, some of which ignore SIGTERM. Each JVM runs as the leader of a process group (and
# session) of its own, and that group is what pins ownership: its leader is not reaped until the group has been dealt
# with, so the kernel cannot give the group's number to another process, and the only signal ever sent is
# killpg(group, SIGKILL). A pid that was read from a process listing is never signalled: the listing says which
# processes are in the group (numeric columns only) and, asked separately, what they run (for the message).
SWEEP_SLACK = 10.0  # how long past its grace the sweep may take (the scans and the wait for the kill included)
_OWNED_LOCK = threading.Lock()
_OWNED_GROUPS: set[int] = set()  # the groups whose leader this driver has not reaped
_STOPPED = False  # the driver was interrupted: no group is started any more


class GroupError(RuntimeError):
    """A process group could not be looked at, so nothing can be said of what it holds."""


def _ps(arguments: list[str], limit: float) -> str:
    """The output of one ps, asked again (at most three times, within `limit` seconds) when it fails or does not
    answer; a ps that keeps failing is an error (GroupError) and never an empty listing. Known, and left: the
    attempts are counted and each is bounded, but their sum (and the half second a short `limit` is raised to) is not
    under one deadline shared with the sweep."""
    end = time.monotonic() + limit
    failure = ""
    for attempt in range(3):
        try:
            done = subprocess.run(["ps", *arguments], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, errors="replace",
                                  timeout=max(0.5, end - time.monotonic()), check=False)
        except (subprocess.TimeoutExpired, OSError) as problem:
            failure = str(problem)
        else:
            if done.returncode == 0:
                return done.stdout
            failure = f"exited {done.returncode}: {done.stderr.strip()}"
        if time.monotonic() >= end:
            break
        time.sleep(0.1)
    raise GroupError(f"ps {' '.join(arguments)}: {failure}")


def live_members(group: int, limit: float) -> list[int]:
    """The pids of the live (not zombie) processes of a group, from the numeric columns of one listing alone."""
    members = []
    for line in _ps(["-axo", "pid=,ppid=,pgid=,stat="], limit).splitlines():
        fields = line.split()
        if len(fields) != 4 or not all(field.isdigit() for field in fields[:3]):
            raise GroupError(f"unreadable line of the process listing: {line!r}")
        if int(fields[2]) == group and not fields[3].startswith("Z"):
            members.append(int(fields[0]))
    return members


def describe(pid: int, limit: float) -> str:
    """What a process runs, for the message only (the pid is not signalled; it may be another process's by now)."""
    try:
        text = _ps(["-o", "command=", "-p", str(pid)], limit)
    except GroupError:
        return "(gone)"
    return text.strip().replace("\n", "\\n")[:200]


def signal_group(group: int) -> None:
    """SIGKILL to every process of a group whose leader is not reaped, so that the number is still the group's."""
    try:
        os.killpg(group, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass  # only the leader's zombie is left in it (macOS answers EPERM for that); the scan after it decides


class _Pipe:
    """The read end of a child's output, read without blocking: what is there is taken, and the end is noted."""

    def __init__(self, stream) -> None:
        self.fd = stream.fileno()
        os.set_blocking(self.fd, False)
        self.data = bytearray()
        self.eof = False

    def pump(self, wait: float) -> None:
        """Takes what the pipe holds, waiting up to `wait` seconds for the first of it (all of them once it is at its end)."""
        if self.eof:
            time.sleep(wait)
            return
        if not select.select([self.fd], [], [], wait)[0]:
            return
        while True:
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                return
            if not chunk:
                self.eof = True
                return
            self.data += chunk


def _exited(pid: int) -> bool:
    """Whether a child has ended, without reaping it (the group keeps its number while the leader is a zombie)."""
    return os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT) is not None


def sweep_group(group: int, pipe: _Pipe, settle: float) -> list[str]:
    """What is left of a group whose leader has ended and is not reaped: one line for each process, ended.

    A process that ends by itself within the grace (a `sleep 1` whose shell was killed) is no leftover. The deciding
    look is a scan that starts after the grace, or any scan that finds the group empty, for nothing joins an empty
    group. Every scan is bounded (its time and the retries' are not under one deadline of the sweep: a scan is
    milliseconds, and the sweep ends before its own hard end) ; a scan that fails raises (GroupError). What is still
    there is ended by SIGKILL to the group, whose leader still holds its number, and then seen gone.
    """
    grace_end = time.monotonic() + settle
    hard_end = grace_end + SWEEP_SLACK
    while True:
        # The look that decides is one that starts after the grace: a snapshot that was taken before it and came back
        # after it says what was there then, and what it shows may have ended by itself since.
        deciding = time.monotonic() >= grace_end
        members = live_members(group, min(5.0, hard_end - time.monotonic()))
        if not members:
            return []
        if deciding:
            break
        pipe.pump(min(0.1, max(0.0, grace_end - time.monotonic())))
    left = [f"pid {pid}: {describe(pid, min(5.0, hard_end - time.monotonic()))}" for pid in members]
    while members:
        signal_group(group)
        if time.monotonic() >= hard_end:
            left += [f"pid {pid} could not be ended" for pid in members]
            break
        time.sleep(0.05)
        members = live_members(group, min(5.0, hard_end - time.monotonic()))
    return left


def end_all_owned() -> None:
    """The driver is interrupted: no group is started any more, and each whose leader is not reaped is ended."""
    global _STOPPED
    with _OWNED_LOCK:
        _STOPPED = True
        for group in _OWNED_GROUPS:
            signal_group(group)


def run_owned(argv: list[str], timeout: float, settle: float = 5.0) -> tuple[int, str, list[str]]:
    """Runs `argv` as the leader of a process group of its own: (status, output, problems).

    The problems are one line for each process of the group still alive after the leader ended and the grace (ended
    by SIGKILL to the group; the test fails on them), and a line when the group could not be looked at. However the
    run ends (a timeout, an interrupt, an error), the group is ended while its leader still holds its number, and
    only then is the leader reaped; each wait is bounded.
    """
    process = None
    pipe = None
    problems: list[str] = []
    # Known, and left: a signal that lands in the main thread between the fork inside Popen and the registration below
    # (the Python handler runs between two bytecodes) loses the child before it is owned. That is a window of a few
    # bytecodes, and the JVM then ends by itself within its own limits.
    try:
        with _OWNED_LOCK:
            if _STOPPED:
                raise KeyboardInterrupt("the driver was interrupted")
            process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            _OWNED_GROUPS.add(process.pid)
        pipe = _Pipe(process.stdout)
        deadline = time.monotonic() + timeout
        while not _exited(process.pid):
            pipe.pump(0.05)
            if time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(argv, timeout)
        pipe.pump(0)
        try:
            problems = sweep_group(process.pid, pipe, settle)
        except GroupError as error:
            problems = [f"the process group could not be checked: {error}"]
    finally:
        if process is not None:
            signal_group(process.pid)
            end = time.monotonic() + SWEEP_SLACK
            while pipe is not None and not pipe.eof and time.monotonic() < end:
                pipe.pump(0.05)
            with _OWNED_LOCK:
                _OWNED_GROUPS.discard(process.pid)
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
            process.stdout.close()
    return process.returncode, pipe.data.decode("utf-8", errors="replace"), problems


def _gone(pid: int, wait: float = 5.0) -> bool:
    """Whether the process of a number has ended, by signal 0 (which sends nothing; a zombie still answers)."""
    end = time.monotonic() + wait
    while True:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        if time.monotonic() >= end:
            return False
        time.sleep(0.05)


def check_leftover_detection() -> int:
    """The check on leftovers can tell a leftover from none, and its clean-up holds (a check that cannot is no check).

    Each case's fixture writes its own pid, and the result is judged by asking the kernel about that pid, not by what
    the check reports. A shell that leaves a process that ignores SIGTERM behind is reported and the process ended; a
    process that ends by itself within the grace is not reported, after the sweep waited for it (so it was in the
    group that the sweep looks at), nor when the scan that saw it comes back after the grace is over; a timeout and
    a listing that fails end the group too.
    """
    stubborn = 'echo $$ > "$1"; trap "" TERM; exec sleep 600'  # one process: exec keeps the pid and the ignored signal
    # A leader that ends when the process it left has written its pid (so the pid is there whatever the check does).
    leaves_one = (f"sh -c '{stubborn}' sh \"$1\" </dev/null >/dev/null 2>&1 & "
                  'until [ -s "$1" ]; do sleep 0.05; done; exit 0')
    with tempfile.TemporaryDirectory(prefix="uml-leftover-check-") as directory:
        def fixture(name: str) -> pathlib.Path:
            return pathlib.Path(directory) / name

        def pid_of(path: pathlib.Path) -> int | None:
            try:
                return int(path.read_text().strip())
            except (OSError, ValueError):
                return None

        left_behind = fixture("left")
        status, _, problems = run_owned(
            ["/bin/sh", "-c", leaves_one, "sh", str(left_behind)],
            30, settle=0.5)
        pid = pid_of(left_behind)
        if (status != 0 or pid is None or not any(line.startswith(f"pid {pid}: ") for line in problems)
                or any("could not" in line for line in problems) or not _gone(pid)):
            print(f"FAIL: a process left behind is not reported and ended (status {status}, pid {pid}): {problems}",
                  file=sys.stderr)
            return 1

        transient = fixture("transient")
        begin = time.monotonic()
        status, _, problems = run_owned(
            ["/bin/sh", "-c", 'sleep 2 </dev/null >/dev/null 2>&1 & echo $! > "$1"; exit 0', "sh", str(transient)],
            30, settle=5.0)
        pid = pid_of(transient)
        if (status != 0 or problems or pid is None or time.monotonic() - begin < 1.0 or not _gone(pid)):
            print(f"FAIL: a process that ends by itself is reported, or was not waited for (status {status}, pid "
                  f"{pid}, {time.monotonic() - begin:.1f} s): {problems}", file=sys.stderr)
            return 1

        timed_out = fixture("timeout")
        try:
            run_owned(["/bin/sh", "-c", stubborn, "sh", str(timed_out)], 1, settle=0.5)
        except subprocess.TimeoutExpired:
            pid = pid_of(timed_out)
            if pid is None or not _gone(pid):
                print(f"FAIL: a run that timed out left its process behind (pid {pid})", file=sys.stderr)
                return 1
        else:
            print("FAIL: a run that outlasts its timeout does not raise", file=sys.stderr)
            return 1

        late = fixture("late")
        real_live = globals()["live_members"]
        pending = [True]

        def late_scan(group: int, limit: float) -> list[int]:
            members = real_live(group, limit)
            if members and pending:
                pending.clear()
                time.sleep(2.2)  # a snapshot of the group that comes back after the grace is over
            return members

        globals()["live_members"] = late_scan
        try:
            status, _, problems = run_owned(
                ["/bin/sh", "-c", 'sleep 1 </dev/null >/dev/null 2>&1 & echo $! > "$1"; exit 0', "sh", str(late)],
                30, settle=2.0)
        finally:
            globals()["live_members"] = real_live
        pid = pid_of(late)
        if status != 0 or problems or pid is None or not _gone(pid):
            print(f"FAIL: a scan that came back late decided what is left, though its process had ended by itself "
                  f"(status {status}, pid {pid}): {problems}", file=sys.stderr)
            return 1

        blind = fixture("blind")
        real_ps = globals()["_ps"]

        def failing_ps(arguments: list[str], limit: float) -> str:
            raise GroupError("injected")

        globals()["_ps"] = failing_ps
        try:
            status, _, problems = run_owned(
                ["/bin/sh", "-c", leaves_one, "sh", str(blind)],
                30, settle=0.5)
        finally:
            globals()["_ps"] = real_ps
        pid = pid_of(blind)
        if not any("could not be checked" in line for line in problems) or pid is None or not _gone(pid):
            print(f"FAIL: a listing that fails is taken for an empty group, or the group is left (pid {pid}): {problems}",
                  file=sys.stderr)
            return 1
    return 0


def compile_and_run(javac: str, java: str, scratch: pathlib.Path,
                    sources: list[pathlib.Path]) -> tuple[int, str, list[str]]:
    """Compiles the sources and the test and runs it: (status, output, problems). The test's rows start real
    processes; the problems are those of run_owned (processes still alive when it has ended, which are ended)."""
    classes = scratch / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    compiled = subprocess.run(
        [javac, "-Xlint:all", "-Werror", "-encoding", "UTF-8", "-d", str(classes),
         *(str(path) for path in sources), str(TEST)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240,
        check=False)
    if compiled.returncode != 0:
        return -1, f"javac:\n{compiled.stdout}", []
    return run_owned([java, "-Xmx64m", "-Dfile.encoding=UTF-8", "-cp", str(classes),
                      "com.kernarium.apps.AppsCommandTest"], 120)


def failed_rows(output: str) -> set[str]:
    return {m.group(1) for m in re.finditer(r"^FAIL ([a-z0-9-]+)", output, re.MULTILINE)}


def unchecked_rows(output: str) -> set[str]:
    """The rows that could not check their processes (their listing failed every attempt): an error of the run."""
    return {m.group(1) for m in re.finditer(r"^ERROR ([a-z0-9-]+): could not be checked", output, re.MULTILINE)}


def judge_mutant(name: str, source: str, must_fail: set[str], status: int, output: str) -> tuple[bool, str]:
    """(caught, the line to print) for one mutant's run. A row that could not be checked is an error of the run and
    never a catch, whatever the other rows say: the run did not look at everything it is there to look at."""
    unchecked = unchecked_rows(output)
    if unchecked:
        return False, (f"FAIL: mutation {name} ({source}): the rows {sorted(unchecked)} could not check their processes "
                       f"(an error of the run, not a catch; the end of its output:\n{output[-1500:]})")
    missed = must_fail - failed_rows(output)
    if status == 0 or missed:
        return False, (f"FAIL: mutation {name} ({source}) was not caught by {sorted(missed) or 'any row'} "
                       f"(status {status}; the end of its output:\n{output[-1500:]})")
    return True, f"mutation {name}: caught by {sorted(failed_rows(output) & must_fail)}"


def check_judging() -> int:
    """A mutant that a row seems to catch is not credited when a row could not check its processes (the control: a
    behaviour-preserving mutant whose only failure is a listing that failed every attempt)."""
    must = {"row-a"}
    caught = judge_mutant("m", "S.java", must, 1, "ok row-b\nFAIL row-a: wrong\nFAILED 1\n")
    errored = judge_mutant("m", "S.java", must, 1, "ERROR row-a: could not be checked: the processes of this JVM could "
                           "not be listed (java.lang.RuntimeException: Cannot allocate memory)\nFAILED 1\n")
    errored_too = judge_mutant("m", "S.java", must, 1, "FAIL row-a: wrong\nERROR row-b: could not be checked: x\nFAILED 2\n")
    if not caught[0] or errored[0] or errored_too[0] or "could not check their processes" not in errored[1]:
        print(f"FAIL: a mutant is credited with a row that could not check its processes: {caught} {errored} {errored_too}",
              file=sys.stderr)
        return 1
    return 0


def strip_comments(text: str) -> str:
    """Java source without its comments (the glue has no string with a slash pair in it)."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def flat(text: str) -> str:
    """The text with every run of white space one space: a rule on a statement does not depend on its line breaks."""
    return re.sub(r"\s+", " ", text)


def platform_problems(text: str) -> list[str]:
    """The messages of the rules the Android glue's text breaks."""
    code = strip_comments(text)
    return [message for message, holds in PLATFORM_RULES if not holds(code)]


def check_platform_rules(platform: str) -> int:
    """The rules hold for the glue, and each rule fails for a copy of it that breaks it."""
    failures = 0
    for message in platform_problems(platform):
        print(f"FAIL: AndroidPlatform: {message}", file=sys.stderr)
        failures += 1
    for name, old, new, message in PLATFORM_MUTATIONS:
        if platform.count(old) != 1:
            print(f"FAIL: platform mutation {name}: the text to replace occurs {platform.count(old)} times", file=sys.stderr)
            failures += 1
        elif message not in platform_problems(platform.replace(old, new)):
            print(f"FAIL: platform mutation {name} was not caught by the rule '{message}'", file=sys.stderr)
            failures += 1
        else:
            print(f"platform mutation {name}: caught by '{message}'")
    return failures


def check_sources() -> int:
    """What the Java cannot tell on a JVM: the wiring, and that the tool writes and opens nothing."""
    problems: list[str] = []
    java_files = {path.name: path.read_text(encoding="utf-8") for path in SOURCES.rglob("*.java")}
    for name, text in java_files.items():
        if not text.startswith("// SPDX-License-Identifier: Apache-2.0\n"):
            problems.append(f"{name} does not carry the Apache-2.0 header of code that goes into the image")
        for forbidden in ("SystemProperties.set", "setProperty", "Settings.", "ContentResolver", "FileWriter",
                          "Files.write", "RandomAccessFile", "java.net.", "Socket", "Runtime.getRuntime",
                          "\"/system/bin/sh\"", "\"sh\"", "FileChannel", "Files.createFile",
                          "File.createTempFile", "Files.newOutputStream", "setComponentEnabledSetting",
                          "grantRuntimePermission", "installPackage"):
            if forbidden in text:
                problems.append(f"{name} uses {forbidden}: the command writes nothing and opens no network")
        if "FileOutputStream" in text and name != "Main.java":
            problems.append(f"{name} opens a FileOutputStream: only Main writes, to the descriptor of stdout")
        if "ProcessBuilder" in text and name != "ChildProcess.java":
            problems.append(f"{name} starts a process: only ChildProcess does")
        for imported in re.findall(r"^import\s+(?:static\s+)?([\w.]+)", strip_comments(text), re.MULTILINE):
            if imported.startswith(("android.graphics", "android.media", "org.xmlpull", "javax.xml", "org.w3c")) \
                    or imported in ("android.util.Xml", "android.content.res.XmlResourceParser", "android.util.TypedValue"):
                problems.append(f"{name} imports {imported}: an icon is drawn, decoded and parsed by the renderer app and nowhere in this tool")
    main = java_files["Main.java"]
    if re.findall(r"new FileOutputStream\((.*?)\)", main) != ["FileDescriptor.out"]:
        problems.append("Main writes anything but the descriptor of stdout")
    platform = java_files["AndroidPlatform.java"]
    child = java_files["ChildProcess.java"]
    if child.count("new ProcessBuilder(argv)") != 1 or "redirectErrorStream(true)" not in child:
        problems.append("ChildProcess starts a process other than the one with the caller's argument vector")
    if "finally" not in child or "terminate(process, kill, graceMillis)" not in child:
        problems.append("ChildProcess does not terminate its child in a finally")
    if check_platform_rules(platform):
        problems.append("the Android glue breaks a rule it must keep")
    commands = java_files["AppsCommand.java"]
    if '"/system/bin/cmd", "activity", "start-activity"' not in commands:
        problems.append("the start is not cmd activity start-activity")
    for flag in ("--es", "--ei", "--ez", "-d", "--data", "--grant-read-uri-permission", "-W"):
        if f'"{flag}"' in commands:
            problems.append(f"the start passes {flag}: it takes no extra, data or flag from outside")
    bp = (TOOL / "Android.bp").read_text(encoding="utf-8")
    for expected in ('java_binary {', 'name: "kernarium_apps"', 'wrapper: "kernarium_apps"',
                     '"src/**/*.java",', '":kernarium_icon_protocol",', 'platform_apis: true', 'system_ext_specific: true'):
        if expected not in bp:
            problems.append(f"Android.bp lacks {expected}")
    code = "\n".join(line for line in bp.splitlines() if not line.lstrip().startswith("//"))
    for forbidden in ("privileged", "certificate", "uses_libs", "permissions"):
        if forbidden in code:
            problems.append(f"Android.bp has {forbidden}: the tool holds nothing of its own")
    wrapper = (TOOL / "kernarium_apps").read_text(encoding="utf-8")
    for expected in ("#!/system/bin/sh", "base=/system_ext", "CLASSPATH=$base/framework/kernarium_apps.jar",
                     "exec app_process $base/bin com.kernarium.apps.Main \"$@\""):
        if expected not in wrapper:
            problems.append(f"the wrapper lacks {expected}")
    if not (TOOL / "kernarium_apps").stat().st_mode & 0o100:
        problems.append("the wrapper is not executable")
    makefile = (LINEAGE / "overlay" / "lineage_andarium.mk").read_text(encoding="utf-8")
    if not re.search(r"^PRODUCT_PACKAGES \+= kernarium_apps$", makefile, re.MULTILINE):
        problems.append("the product does not install kernarium_apps")
    build = (LINEAGE / "build" / "build-aosp.sh").read_text(encoding="utf-8")
    if '*" kernarium_apps "*' not in build:
        problems.append("the build does not check that kernarium_apps is in the product")
    verifier = (LINEAGE / "tools" / "verify-target-files.py").read_text(encoding="utf-8")
    for expected in ('"SYSTEM_EXT/bin/kernarium_apps"', '"SYSTEM_EXT/framework/kernarium_apps.jar"',
                     "check_apps_command("):
        if expected not in verifier:
            problems.append(f"the image verifier lacks {expected}")
    # The tool holds no permission: its one policy line silences a denial, and nothing else names it.
    sepolicy = LINEAGE / "overlay" / "sepolicy"
    policy = sepolicy / "system_ext" / "private" / "kernarium_apps.te"
    for path in sepolicy.rglob("*"):
        if path.is_file() and path != policy and "kernarium_apps" in path.read_text(encoding="utf-8", errors="replace"):
            problems.append(f"{path.relative_to(LINEAGE)} names kernarium_apps: its only policy is {policy.name}")
    rules = [line.strip() for line in policy.read_text(encoding="utf-8").splitlines()
             if line.strip() and not line.strip().startswith("#")]
    if rules != ["dontaudit shell qemu_sf_lcd_density_prop:file read;"]:
        problems.append(f"{policy.name} is not the one dontaudit line (no allow rule, no other policy): {rules}")
    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    return 1 if problems else 0


def _interrupted(number: int, frame) -> None:
    raise KeyboardInterrupt(f"signal {number}")


def main() -> int:
    """Runs the test; an interrupt (SIGINT, SIGTERM, SIGHUP) ends every process group it owns before it leaves."""
    for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(number, _interrupted)
    try:
        return run()
    except BaseException:
        end_all_owned()
        raise


def run() -> int:
    if check_sources() != 0:
        return 1
    javac, java = jdk_tool("javac"), jdk_tool("java")
    if javac is None or java is None:
        print("SKIP: no JDK (javac and java) on PATH")
        return SKIP
    if check_judging() != 0 or check_leftover_detection() != 0:
        return 1
    package_dir = SOURCES / PACKAGE_DIR
    sources = sorted(path for path in package_dir.glob("*.java") if path.name not in ANDROID_GLUE)
    expected = {"AppsCommand.java", "ChildProcess.java", "Component.java", "Entry.java", "Icon.java", "LabelText.java",
                "Listing.java", "PlatformException.java", "PngCheck.java", "ProviderLink.java", "ResourcePaths.java",
                "StartOutcome.java"}
    if {path.name for path in sources} != expected:
        print(f"FAIL: the platform-free sources are {sorted(path.name for path in sources)}, not {sorted(expected)}",
              file=sys.stderr)
        return 1
    for name in ANDROID_GLUE:
        if not (package_dir / name).is_file():
            print(f"FAIL: {name} is missing", file=sys.stderr)
            return 1
    with tempfile.TemporaryDirectory(prefix="uml-app-shortcuts-") as directory:
        scratch = pathlib.Path(directory)
        status, output, left = compile_and_run(javac, java, scratch / "base", [*sources, PROTOCOL])
        print(output, end="")
        if status != 0 or not output.rstrip().endswith("PASS") or unchecked_rows(output):
            print("FAIL: the command's test" + (f" (rows that could not check their processes: "
                                               f"{sorted(unchecked_rows(output))})" if unchecked_rows(output) else ""),
                  file=sys.stderr)
            return 1
        if left:
            print("FAIL: the command's test left processes behind (ended):\n  " + "\n  ".join(left), file=sys.stderr)
            return 1
        rows = set(re.findall(r"^ok ([a-z0-9-]+)", output, re.MULTILINE))

        def mutate(mutation: tuple) -> tuple[bool, str]:
            """One mutant: (caught, the line to print). Each has a scratch directory of its own."""
            name, source, old, new, must_fail = mutation
            unknown = must_fail - rows
            if unknown:
                return False, f"FAIL: mutation {name} names rows that do not exist: {sorted(unknown)}"
            copy = scratch / f"src-{name}"
            shutil.copytree(package_dir, copy)
            shutil.copy(PROTOCOL, copy / PROTOCOL.name)
            target = copy / source
            text = target.read_text(encoding="utf-8")
            if text.count(old) != 1:
                return False, f"FAIL: mutation {name}: the text to remove occurs {text.count(old)} times in {source}"
            target.write_text(text.replace(old, new), encoding="utf-8")
            mutated = sorted(path for path in copy.glob("*.java") if path.name not in ANDROID_GLUE)
            mutant_status, mutant_output, left = compile_and_run(javac, java, scratch / f"run-{name}", mutated)
            shutil.rmtree(copy)
            if mutant_status == -1:
                return False, f"FAIL: mutation {name} does not compile:\n{mutant_output}"
            if left:
                return False, (f"FAIL: mutation {name} ({source}) left processes behind (ended):\n  "
                               + "\n  ".join(left))
            return judge_mutant(name, source, must_fail, mutant_status, mutant_output)

        # The mutants are independent processes: a few at a time keeps the whole under the test's timeout.
        workers = max(1, min(3, (os.cpu_count() or 1) - 1))
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=workers)
        try:
            results = list(pool.map(mutate, MUTATIONS))
        except BaseException:
            pool.shutdown(wait=False, cancel_futures=True)  # no mutant starts after an interrupt
            end_all_owned()
            raise
        finally:
            pool.shutdown(wait=True)
        failures = 0
        for caught, line in results:
            print(line, file=sys.stdout if caught else sys.stderr)
            failures += 0 if caught else 1
        if failures:
            return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
