#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The icon renderer (android/lineage-24/overlay/app_icons, the app KernariumIcons) as far as a host can tell.

kernarium_apps runs as the adb shell user and draws no icon: it names an application's files to this app, whose
provider only the shell user may use and whose service runs in an isolated process, and it checks the PNG that comes
back. What the two share (IconProtocol: the shell user's uid, the shape of a request, a path's rules, the sizes and
bounds) does not touch the platform: IconProtocolTest.java runs it on a plain JVM, and again on copies of the source
with one rule removed at a time, where the rows named here must fail. The app's own code (provider, worker, drawing,
service) cannot run on a JVM: what it must and must not do is checked on its text, each rule with a mutant, and its
manifest and build file are read back for the facts that make it an ordinary, unprivileged, direct-boot-aware
application with a guarded provider and an isolated service. The drawing and the processes' domains are exercised by
the runner on an image. Skipped (status 77) where the host has no JDK; the text checks run regardless.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import concurrent.futures
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[2]
LINEAGE = REPOSITORY / "android" / "lineage-24"
APP = LINEAGE / "overlay" / "app_icons"
SOURCES = APP / "src" / "com" / "kernarium" / "icons"
PRODUCT_MAKEFILE = LINEAGE / "overlay" / "lineage_andarium.mk"
BUILD_SCRIPT = LINEAGE / "build" / "build-aosp.sh"
VERIFIER = LINEAGE / "tools" / "verify-target-files.py"
TEST = HERE / "icon_renderer" / "com" / "kernarium" / "icons" / "IconProtocolTest.java"
ANDROID = "{http://schemas.android.com/apk/res/android}"
SKIP = 77
GLUE = ("IconDrawing.java", "IconProvider.java", "IconRenderService.java", "IconWorker.java")
#: The files that need no platform: shared with the shell tool or run on a JVM here.
PLATFORM_FREE = ("IconProtocol.java", "IconWorkerCore.java")

# One rule of IconProtocol removed per entry: (name, the text that is replaced, its replacement, the rows that must fail).
PROTOCOL_MUTATIONS = [
    ("shell-uid-by-app-id", "uid == SHELL_UID", "uid % 100000 == SHELL_UID", {"protocol-shell-uid"}),
    ("every-uid-is-the-shell", "uid == SHELL_UID", "uid >= 0", {"protocol-shell-uid"}),
    ("shell-uid-changed", "SHELL_UID = 2000;", "SHELL_UID = 2001;", {"protocol-shell-uid"}),
    ("resource-zero-allowed", "resource == 0 || kinds == null", "kinds == null", {"request-needs-a-resource"}),
    ("resource-sign-bit-refused", "resource == 0 ||", "resource <= 0 ||", {"request-resource-ids-with-the-sign-bit-are-valid",
                                                                         "request-shapes-that-are-accepted"}),
    ("provider-class-changed", "PROVIDER_CLASS = \"com.kernarium.icons.IconProvider\";",
     "PROVIDER_CLASS = \"com.kernarium.icons.IconProviderX\";", {"protocol-names-are-the-contract"}),
    ("null-kinds-allowed", "kinds == null || paths == null ||", "", {"request-arrays-agree"}),
    ("unequal-arrays-allowed", "kinds.length != paths.length ||", "", {"request-arrays-agree"}),
    ("empty-request-allowed", "kinds.length == 0\n", "false\n", {"request-arrays-agree"}),
    ("file-count-unbounded", "kinds.length > MAX_FILES", "false", {"request-file-count-is-bounded"}),
    ("file-bound-off-by-one", "MAX_FILES = 128;", "MAX_FILES = 129;", {"request-file-count-is-bounded", "sizes-and-bounds"}),
    ("first-file-may-be-no-base", "|| kinds[0] != KIND_BASE", "", {"request-starts-with-exactly-one-base"}),
    ("second-base-allowed", "(i > 0 && kinds[i] <= KIND_BASE)", "false", {"request-starts-with-exactly-one-base"}),
    ("kind-range-unchecked", "kinds[i] < KIND_BASE || kinds[i] > KIND_OVERLAY", "false", {"request-kinds-are-the-known-ones"}),
    ("kind-past-overlay-allowed", "kinds[i] > KIND_OVERLAY", "kinds[i] > KIND_OVERLAY + 1", {"request-kinds-are-the-known-ones"}),
    ("order-unchecked", "(i > 0 && kinds[i] < kinds[i - 1])", "false", {"request-order-is-the-frameworks"}),
    ("paths-unchecked", "|| !validPath(paths[i])", "", {"request-names-valid-paths-only"}),
    ("path-may-be-relative", "|| path.charAt(0) != '/'", "", {"path-is-absolute-and-not-empty"}),
    ("path-may-be-empty", "path == null || path.isEmpty() ||", "path == null ||", {"path-is-absolute-and-not-empty"}),
    ("path-may-be-null", "path == null || path.isEmpty()", "path.isEmpty()", {"path-is-absolute-and-not-empty"}),
    ("path-length-unbounded", "path.length() > MAX_PATH_CHARS", "false", {"path-length-is-bounded"}),
    ("path-bound-off-by-one", "MAX_PATH_CHARS = 1024;", "MAX_PATH_CHARS = 1025;", {"path-length-is-bounded"}),
    ("path-controls-allowed", "c < 0x20 || c > 0x7E", "c > 0x7E", {"path-is-printable-ascii"}),
    ("path-delete-allowed", "c < 0x20 || c > 0x7E", "c < 0x20 || c > 0x7F", {"path-is-printable-ascii"}),
    ("path-non-ascii-allowed", "c < 0x20 || c > 0x7E", "c < 0x20", {"path-is-printable-ascii"}),
    ("path-empty-segment-allowed", "segment.isEmpty() ||", "", {"path-has-no-empty-or-dot-segments"}),
    ("path-dot-segment-allowed", "segment.equals(\".\") ||", "", {"path-has-no-empty-or-dot-segments"}),
    ("path-dotdot-segment-allowed", "segment.equals(\"..\")", "false", {"path-has-no-empty-or-dot-segments"}),
    ("sizes-changed", "SIZES = {192, 128, 96};", "SIZES = {192, 128, 64};", {"sizes-and-bounds", "size-steps-down-through-the-list"}),
    ("png-bound-changed", "MAX_PNG_BYTES = 98304;", "MAX_PNG_BYTES = 98305;", {"sizes-and-bounds"}),
    ("density-changed", "DENSITY = 640;", "DENSITY = 480;", {"sizes-and-bounds"}),
    ("every-size-has-a-next", "SIZES[i] == size", "true", {"size-steps-down-through-the-list"}),
    ("outset-is-half", "Math.round(size * LAYER_OUTSET)", "Math.round(size * 0.5f)", {"layer-outset-is-a-quarter"}),
    ("outset-constant-changed", "LAYER_OUTSET = 0.25f;", "LAYER_OUTSET = 0.3f;", {"layer-outset-is-a-quarter"}),
    ("alpha-only-when-clear", "(pixel >>> 24) != 0xFF", "(pixel >>> 24) == 0", {"alpha-is-any-pixel-not-opaque"}),
    ("authority-changed", "AUTHORITY = \"com.kernarium.icons\";", "AUTHORITY = \"com.kernarium.icon\";", {"protocol-names-are-the-contract"}),
    ("status-error-changed", "STATUS_ERROR = 1;", "STATUS_ERROR = 2;", {"protocol-names-are-the-contract"}),
    ("kinds-reordered", "KIND_LIBRARY = 2;", "KIND_LIBRARY = 3;", {"protocol-names-are-the-contract"}),
]

# One rule of IconWorkerCore removed per entry: (name, the text that is replaced, its replacement, the rows that must fail).
LOCK_FOREVER = "mTurn.tryLock(Long.MAX_VALUE / 4, TimeUnit.MILLISECONDS)"
NO_LOCK = ("new ReentrantLock(true) {\n        @Override\n        public boolean tryLock(long time, TimeUnit unit) {\n            return true;\n        }\n\n"
           "        @Override\n        public boolean tryLock() {\n            return true;\n        }\n\n"
           "        @Override\n        public void unlock() {\n        }\n    };\n")
WORKER_MUTATIONS = [
    ("turn-waits-without-a-deadline", "mTurn.tryLock(left(deadline), TimeUnit.MILLISECONDS)", LOCK_FOREVER,
     {"worker-a-request-that-queued-past-its-deadline-never-draws"}),
    ("no-turn-at-all", "new ReentrantLock(true);\n", NO_LOCK, {"worker-requests-draw-one-at-a-time"}),
    ("queued-request-starts-after-its-deadline",
     "            if (left(deadline) <= 0) {\n                return answer; // the deadline passed while it queued: nothing is started\n            }\n", "",
     {"worker-a-deadline-that-has-passed-starts-nothing"}),
    ("bind-eats-the-deadline-unchecked",
     "            if (remaining <= 0) {\n                return answer; // the bind took the rest: an immediate timeout, and the worker is bound and idle\n            }\n", "",
     {"worker-a-bind-that-takes-the-rest-of-the-deadline-draws-nothing", "worker-no-time-left-after-the-bind-starts-no-draw"}),
    ("nothing-left-is-waited-for", "            if (remaining <= 0) {\n                return answer; // the bind took the rest",
     "            if (remaining < 0) {\n                return answer; // the bind took the rest",
     {"worker-no-time-left-after-the-bind-starts-no-draw"}),
    ("drawing-gets-more-than-what-is-left", "answer = transact(session, resource, kinds, paths, remaining);",
     "answer = transact(session, resource, kinds, paths, mLimits.deadlineMillis);",
     {"worker-one-deadline-covers-the-wait-for-the-turn"}),
    ("budget-is-read-twice", "answer = transact(session, resource, kinds, paths, remaining);",
     "answer = transact(session, resource, kinds, paths, left(deadline));",
     {"worker-the-budget-is-read-once"}),
    ("unbind-is-waited-for-by-the-answer", "        release.setDaemon(true);\n        release.start();\n",
     "        release.setDaemon(true);\n        release.run();\n", {"worker-a-slow-unbind-does-not-spend-the-answer-s-time"}),
    ("a-new-bind-does-not-wait-for-the-old-release",
     "        if (!released(deadline)) {\n            return null; // the earlier worker is still being ended: this request has no time left to wait for it\n        }\n", "",
     {"worker-a-slow-unbind-does-not-spend-the-answer-s-time", "worker-a-release-that-does-not-return-fails-the-next-request-by-its-deadline"}),
    ("a-new-bind-waits-past-its-deadline", "out.await(Math.max(left(deadline), 0), TimeUnit.MILLISECONDS)",
     "out.await(60_000, TimeUnit.MILLISECONDS)", {"worker-a-release-that-does-not-return-fails-the-next-request-by-its-deadline"}),
    ("a-release-that-is-out-is-ignored-at-the-deadline", "        if (!out.await(Math.max(left(deadline), 0), TimeUnit.MILLISECONDS)) {\n            return false;\n        }\n",
     "        out.await(Math.max(left(deadline), 0), TimeUnit.MILLISECONDS);\n",
     {"worker-a-release-that-does-not-return-fails-the-next-request-by-its-deadline"}),
    ("a-failed-bind-stays-attached", "        mHandle = null;\n        mConnection = null;\n        if (handle == null) {\n            return;\n        }\n        final CountDownLatch",
     "        if (handle == null) {\n            return;\n        }\n        final CountDownLatch",
     {"worker-a-slow-unbind-does-not-spend-the-answer-s-time", "worker-failure-unbinds-and-the-next-request-binds-again"}),
    ("a-failed-bind-is-released-twice", "                handle.unbind();\n            } finally {\n                done.countDown();",
     "                handle.unbind();\n                handle.unbind();\n            } finally {\n                done.countDown();",
     {"worker-a-slow-unbind-does-not-spend-the-answer-s-time"}),
    ("hung-drawing-is-waited-for", "transaction.join(millis);", "transaction.join(millis + 3_000);",
     {"worker-hung-drawing-fails-at-the-deadline-and-unbinds"}),
    ("bind-wait-ignores-the-deadline", "Math.min(mLimits.bindMillis, left(deadline))", "mLimits.bindMillis",
     {"worker-bind-is-capped-by-the-deadline"}),
    ("bind-wait-is-longer", "connection.mConnected.await(wait, TimeUnit.MILLISECONDS)",
     "connection.mConnected.await(wait + 2_000, TimeUnit.MILLISECONDS)", {"worker-bind-timeout-fails-and-releases"}),
    ("bind-timeout-keeps-the-bind", "        if (wait <= 0 || !connection.mConnected.await(wait, TimeUnit.MILLISECONDS)) {\n            disconnect();\n",
     "        if (wait <= 0 || !connection.mConnected.await(wait, TimeUnit.MILLISECONDS)) {\n",
     {"worker-bind-timeout-fails-and-releases"}),
    ("refused-bind-is-used", "        if (mHandle == null) {\n            disconnect();\n            return null;\n        }\n",
     "", {"worker-a-bind-that-is-refused-is-a-failure"}),
    ("failed-worker-is-kept", "if (asked && !answer.ok()) {", "if (false) {",
     {"worker-failure-unbinds-and-the-next-request-binds-again", "worker-an-exception-is-a-failure",
      "worker-hung-drawing-fails-at-the-deadline-and-unbinds"}),
    ("dead-binder-is-trusted", "if (current != null && current.alive()) {", "if (current != null) {",
     {"worker-rebinds-a-dead-binder"}),
    ("oversized-reply-is-passed-on", " || reply.png.length > IconProtocol.MAX_PNG_BYTES", "",
     {"worker-reply-over-the-bound-is-not-passed-on"}),
    ("reply-without-a-png-is-passed-on", " || reply.png == null", "", {"worker-reply-over-the-bound-is-not-passed-on"}),
    ("idle-cleanup-waits-for-the-turn", "            if (!mTurn.tryLock()) {\n",
     "            mTurn.lock();\n            if (false) {\n", {"worker-idle-cleanup-never-holds-the-main-thread-for-a-request"}),
    ("idle-cleanup-ignores-a-newer-request", "if (mScheduledIn == mGeneration) { // else", "if (true) { // else",
     {"worker-idle-cleanup-leaves-a-request-that-has-come-since", "worker-a-cleanup-of-an-older-request-does-nothing"}),
    ("a-contended-cleanup-is-lost", "if (mRetries++ < MAX_CLEANUP_RETRIES) {", "if (false) {",
     {"worker-a-cleanup-that-keeps-finding-the-turn-held-by-another-request-gives-up"}),
    ("a-contended-cleanup-looks-again-for-ever", "if (mRetries++ < MAX_CLEANUP_RETRIES) {", "if (mRetries++ < Integer.MAX_VALUE) {",
     {"worker-a-cleanup-that-keeps-finding-the-turn-held-by-another-request-gives-up"}),
    ("a-cleanup-is-posted-before-the-turn-is-released",
     "            mTurn.unlock();\n            if (cleanup != null) {\n                mPoster.postDelayed(cleanup, mLimits.idleMillis); // after the turn is released: not as its holder\n            }\n",
     "            if (cleanup != null) {\n                mPoster.postDelayed(cleanup, mLimits.idleMillis);\n            }\n            mTurn.unlock();\n",
     {"worker-the-request-is-never-the-holder-its-own-cleanup-waits-for", "worker-a-cleanup-is-not-lost-however-long-the-last-request-is-paused"}),
    ("generation-does-not-move", "            mGeneration++;\n", "", {"worker-idle-cleanup-leaves-a-request-that-has-come-since"}),
    ("old-cleanup-is-not-removed", "                mPoster.remove(mIdle);\n", "", {"worker-idle-cleanup-leaves-a-request-that-has-come-since"}),
    ("idle-worker-is-never-scheduled", "                mPoster.postDelayed(cleanup, mLimits.idleMillis); // after the turn is released: not as its holder\n", "",
     {"worker-an-idle-worker-is-unbound"}),
    ("idle-time-changed", "PRODUCTION = new Limits(7_000, 1_000, 5_000);", "PRODUCTION = new Limits(7_000, 1_000, 6_000);",
     {"worker-production-limits-nest-in-the-tools"}),
    ("deadline-past-the-tools", "PRODUCTION = new Limits(7_000, 1_000, 5_000);", "PRODUCTION = new Limits(9_000, 1_000, 5_000);",
     {"worker-production-limits-nest-in-the-tools"}),
    ("bind-limit-changed", "PRODUCTION = new Limits(7_000, 1_000, 5_000);", "PRODUCTION = new Limits(7_000, 1_500, 5_000);",
     {"worker-production-limits-nest-in-the-tools"}),
]

# What the app's code must and must not do, as text (it cannot run on a JVM): file -> [(message, test of the
# comment-free text)]. Each has a mutant below: (name, file, text replaced, replacement, the message that must be reported).
def order(text: str, *parts: str) -> bool:
    """Every part is in the text, and they come in that order."""
    positions = [text.find(part) for part in parts]
    return all(position >= 0 for position in positions) and positions == sorted(positions)


RULES = {
    "IconProvider.java": [
        ("the caller is the shell user, checked before anything is read or drawn",
         lambda t: "IconProtocol.fromShell(Binder.getCallingUid())" in t
         and order(t, "IconProtocol.fromShell(", "IconProtocol.validRequest(", "mWorker.render(")
         and 'throw new SecurityException("only the shell user may ask for icons")' in t),
        ("only the render method is served",
         lambda t: "IconProtocol.METHOD_RENDER.equals(method)" in t),
        ("a request is validated before the worker sees it",
         lambda t: "!IconProtocol.validRequest(resource, kinds, paths)" in t),
        ("both call overloads go through the one check",
         lambda t: t.count("return handle(method, extras);") == 2),
        ("no data is served or opened",
         lambda t: all(word not in t for word in ("openFile", "openAssetFile", "openTypedAssetFile", "getContext().getContentResolver"))
         and len(re.findall(r"return null;", t)) == 3 and "return 0;" in t),
        ("it opens and parses nothing of an application's files",
         lambda t: all(word not in t for word in ("AssetManager", "Resources", "ApkAssets", "PackageManager", "BitmapFactory",
                                                  "XmlPull", "ZipFile", "java.io.File", "Bitmap", "Canvas", "Drawable"))),
    ],
    "IconWorker.java": [
        ("a worker is bound by the class of the service in this package, and a dead binder is not trusted",
         lambda t: "new Intent().setClassName(IconProtocol.PACKAGE, IconProtocol.SERVICE_CLASS)" in t
         and "mWorker.isBinderAlive()" in t and "Context.BIND_AUTO_CREATE" in t),
        ("one transaction for one request: nothing is asked again of a worker",
         lambda t: t.count("mWorker.transact(") == 1 and "retry" not in t.lower() and "while (" not in t and "for (int" not in t),
        ("the reply is an exception header, then the fields; the parcels are the transaction's own and recycled by it",
         lambda t: "reply.readException();" in t and re.search(r"finally \{\s*data\.recycle\(\);[^}]*reply\.recycle\(\);", t) is not None),
        ("the idle cleanup runs on the main looper and the limits are the product's",
         lambda t: "new Handler(Looper.getMainLooper())" in t and "IconWorkerCore.PRODUCTION" in t
         and "main.removeCallbacks(task)" in t and "main.postDelayed(task, millis)" in t),
        ("a bind that failed is released", lambda t: re.search(r"if \(!bound\) \{\s*handle\.unbind\(\);", t) is not None
         and "mContext.unbindService(connection)" in t),
        ("a failed reply is the error status and nothing else",
         lambda t: re.search(r"if \(!reply\.ok\(\)\) \{\s*return failed\(\);", t) is not None
         and "answer.putInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR);" in t),
        ("the rules of the hold are IconWorkerCore's: this class has no timing of its own",
         lambda t: all(word not in t for word in ("Thread", "CountDownLatch", "synchronized", "postDelayed(mIdle", "join("))),
        ("it opens and parses nothing of an application's files",
         lambda t: all(word not in t for word in ("AssetManager", "Resources", "ApkAssets", "PackageManager", "BitmapFactory",
                                                  "XmlPull", "ZipFile", "java.io.File", "Bitmap", "Canvas", "Drawable"))),
    ],
    "IconRenderService.java": [
        ("the isolated process checks what it is sent before it opens a file",
         lambda t: "IconProtocol.validRequest(resource, kinds, paths)" in t
         and order(t, "IconProtocol.validRequest(", "IconDrawing.render(")),
        ("one transaction is served",
         lambda t: "code != TRANSACTION_RENDER" in t and "return super.onTransact(code, data, reply, flags);" in t),
        ("an answer is a status, then the fields",
         lambda t: "reply.writeNoException();" in t and t.count("reply.writeInt(") == 5 and "reply.writeByteArray(drawn.png)" in t),
        ("a failed drawing is the error status",
         lambda t: "reply.writeInt(IconProtocol.STATUS_ERROR);" in t and "reply.writeInt(IconProtocol.STATUS_OK);" in t),
    ],
    "IconDrawing.java": [
        ("a size is stepped down only for a PNG that is over the bound, never after a failure",
         lambda t: re.search(r"if \(drawn == null\) \{\s*return null;", t) is not None
         and re.search(r"if \(drawn\.png\.length <= IconProtocol\.MAX_PNG_BYTES\) \{\s*return drawn;", t) is not None
         and t.count("IconProtocol.smaller(") == 1),
        ("any failure to draw, running out of memory too, is no icon",
         lambda t: re.search(r"catch \(Exception \| OutOfMemoryError failed\) \{\s*return null;", t) is not None),
        ("the files are closed whatever happens", lambda t: re.search(r"finally \{\s*assets\.close\(\);", t) is not None),
        ("the picture is drawn for a fixed configuration: the largest density, day mode, one locale",
         lambda t: "UI_MODE_NIGHT_NO" in t and "new LocaleList(Locale.US)" in t and "configuration.densityDpi = IconProtocol.DENSITY" in t
         and "getDrawableForDensity(resource, IconProtocol.DENSITY, null)" in t and re.search(r"new Configuration\(\s*\w", t) is None),
        ("every file is added the way the framework adds it for the application itself",
         lambda t: all(word in t for word in ("addAssetPathAsSharedLibrary(", "addOverlayPath(", "addAssetPath("))
         and "IconProtocol.KIND_LIBRARY" in t and "IconProtocol.KIND_OVERLAY" in t),
        ("a base or a split that cannot be added is no icon; a library or an overlay is skipped, as the framework does",
         lambda t: re.search(r"if \(cookie == 0 && kinds\[i\] != IconProtocol\.KIND_LIBRARY\s*&& kinds\[i\] != IconProtocol\.KIND_OVERLAY\) \{\s*return null;", t)
         is not None),
        ("an adaptive icon's layers are drawn, outset, without a mask",
         lambda t: "icon.getBackground()" in t and "icon.getForeground()" in t and "IconProtocol.outset(size)" in t
         and "getIconMask" not in t and "setBounds(-outset, -outset, size + outset, size + outset)" in t),
        ("the alpha flag is read from the pixels", lambda t: "IconProtocol.hasAlpha(pixels)" in t and "getPixels(" in t),
    ],
}

MUTATIONS = [
    ("provider-skips-the-uid-check", "IconProvider.java",
     "        if (!IconProtocol.fromShell(Binder.getCallingUid())) {\n            throw new SecurityException(\"only the shell user may ask for icons\");\n        }\n",
     "", "the caller is the shell user, checked before anything is read or drawn"),
    ("provider-checks-the-uid-after-validating", "IconProvider.java",
     "        if (!IconProtocol.fromShell(Binder.getCallingUid())) {",
     "        if (IconProtocol.validRequest(1, new int[] {0}, new String[] {\"/a\"}) && !IconProtocol.fromShell(Binder.getCallingUid())) {",
     "the caller is the shell user, checked before anything is read or drawn"),
    ("provider-serves-any-method", "IconProvider.java",
     "if (!IconProtocol.METHOD_RENDER.equals(method) || extras == null) {", "if (extras == null) {",
     "only the render method is served"),
    ("provider-skips-validation", "IconProvider.java",
     "        if (!IconProtocol.validRequest(resource, kinds, paths)) {\n            return IconWorker.failed();\n        }\n",
     "", "a request is validated before the worker sees it"),
    ("provider-overload-without-the-check", "IconProvider.java",
     "    public Bundle call(String method, String arg, Bundle extras) {\n        return handle(method, extras);",
     "    public Bundle call(String method, String arg, Bundle extras) {\n        return mWorker.render(1, new int[] {0}, new String[] {\"/a\"});",
     "both call overloads go through the one check"),
    ("provider-serves-a-file", "IconProvider.java",
     "    public String getType(Uri uri) {", "    public android.os.ParcelFileDescriptor openFile(Uri uri, String mode) {\n        return null;\n    }\n\n    @Override\n    public String getType(Uri uri) {",
     "no data is served or opened"),
    ("provider-opens-resources", "IconProvider.java",
     "    private IconWorker mWorker;", "    private IconWorker mWorker;\n    private android.content.res.Resources mResources;",
     "it opens and parses nothing of an application's files"),
    ("worker-trusts-a-dead-binder", "IconWorker.java",
     "return mWorker.isBinderAlive();", "return true;",
     "a worker is bound by the class of the service in this package, and a dead binder is not trusted"),
    ("worker-binds-another-class", "IconWorker.java",
     "setClassName(IconProtocol.PACKAGE, IconProtocol.SERVICE_CLASS)", "setClassName(IconProtocol.PACKAGE, \"com.kernarium.icons.Other\")",
     "a worker is bound by the class of the service in this package, and a dead binder is not trusted"),
    ("worker-asks-again", "IconWorker.java",
     "                mWorker.transact(IconRenderService.TRANSACTION_RENDER, data, reply, 0);\n",
     "                mWorker.transact(IconRenderService.TRANSACTION_RENDER, data, reply, 0);\n"
     "                mWorker.transact(IconRenderService.TRANSACTION_RENDER, data, reply, 0);\n",
     "one transaction for one request: nothing is asked again of a worker"),
    ("worker-reads-no-exception-header", "IconWorker.java",
     "                reply.readException();\n", "",
     "the reply is an exception header, then the fields; the parcels are the transaction's own and recycled by it"),
    ("worker-leaks-its-parcels", "IconWorker.java",
     "                data.recycle(); // this thread's own parcels: it ends when the worker answers or dies\n                reply.recycle();\n", "",
     "the reply is an exception header, then the fields; the parcels are the transaction's own and recycled by it"),
    ("worker-has-limits-of-its-own", "IconWorker.java",
     "IconWorkerCore.PRODUCTION", "new IconWorkerCore.Limits(60_000, 60_000, 60_000)",
     "the idle cleanup runs on the main looper and the limits are the product's"),
    ("worker-cleans-up-on-a-thread-of-its-own", "IconWorker.java",
     "new Handler(Looper.getMainLooper())", "new Handler(Looper.myLooper())",
     "the idle cleanup runs on the main looper and the limits are the product's"),
    ("worker-keeps-a-failed-bind", "IconWorker.java",
     "                handle.unbind(); // a failed bind is still released\n", "",
     "a bind that failed is released"),
    ("worker-passes-a-failed-reply", "IconWorker.java",
     "        if (!reply.ok()) {\n            return failed();\n        }\n", "",
     "a failed reply is the error status and nothing else"),
    ("worker-waits-on-its-own", "IconWorker.java",
     "    private final IconWorkerCore mCore;", "    private final IconWorkerCore mCore;\n    private final Object lock = new Object();\n    void waitHere() throws InterruptedException {\n        synchronized (lock) {\n            lock.wait(1);\n        }\n    }",
     "the rules of the hold are IconWorkerCore's: this class has no timing of its own"),
    ("service-draws-before-checking", "IconRenderService.java",
     "IconProtocol.validRequest(resource, kinds, paths)\n                    ? IconDrawing.render(resource, kinds, paths) : null;",
     "IconDrawing.render(resource, kinds, paths);",
     "the isolated process checks what it is sent before it opens a file"),
    ("service-serves-any-code", "IconRenderService.java",
     "if (code != TRANSACTION_RENDER) {", "if (false) {", "one transaction is served"),
    ("service-leaves-out-the-exception-header", "IconRenderService.java",
     "            reply.writeNoException();\n", "", "an answer is a status, then the fields"),
    ("service-answers-ok-for-a-failure", "IconRenderService.java",
     "reply.writeInt(IconProtocol.STATUS_ERROR);", "reply.writeInt(IconProtocol.STATUS_OK);",
     "a failed drawing is the error status"),
    ("drawing-steps-down-after-a-failure", "IconDrawing.java",
     "                if (drawn == null) {\n                    return null;\n                }\n", "                if (drawn == null) {\n                    continue;\n                }\n",
     "a size is stepped down only for a PNG that is over the bound, never after a failure"),
    ("drawing-takes-a-png-over-the-bound", "IconDrawing.java",
     "if (drawn.png.length <= IconProtocol.MAX_PNG_BYTES) {", "if (drawn.png.length <= IconProtocol.MAX_PNG_BYTES * 2) {",
     "a size is stepped down only for a PNG that is over the bound, never after a failure"),
    ("drawing-lets-memory-kill-the-process", "IconDrawing.java",
     "catch (Exception | OutOfMemoryError failed) {", "catch (Exception failed) {",
     "any failure to draw, running out of memory too, is no icon"),
    ("drawing-leaves-the-files-open", "IconDrawing.java",
     "        } finally {\n            assets.close();\n        }", "        } finally {\n        }",
     "the files are closed whatever happens"),
    ("drawing-for-the-night", "IconDrawing.java",
     "Configuration.UI_MODE_NIGHT_NO", "Configuration.UI_MODE_NIGHT_YES",
     "the picture is drawn for a fixed configuration: the largest density, day mode, one locale"),
    ("drawing-for-the-device-configuration", "IconDrawing.java",
     "        Configuration configuration = new Configuration();", "        Configuration configuration = new Configuration(new Configuration());",
     "the picture is drawn for a fixed configuration: the largest density, day mode, one locale"),
    ("drawing-skips-overlays", "IconDrawing.java",
     "cookie = assets.addOverlayPath(paths[i]);", "cookie = 1;",
     "every file is added the way the framework adds it for the application itself"),
    ("drawing-needs-every-overlay", "IconDrawing.java",
     "if (cookie == 0 && kinds[i] != IconProtocol.KIND_LIBRARY && kinds[i] != IconProtocol.KIND_OVERLAY) {",
     "if (cookie == 0 && kinds[i] != IconProtocol.KIND_LIBRARY) {",
     "a base or a split that cannot be added is no icon; a library or an overlay is skipped, as the framework does"),
    ("drawing-ignores-a-split-failure", "IconDrawing.java",
     "if (cookie == 0 && kinds[i] != IconProtocol.KIND_LIBRARY && kinds[i] != IconProtocol.KIND_OVERLAY) {",
     "if (false) {",
     "a base or a split that cannot be added is no icon; a library or an overlay is skipped, as the framework does"),
    ("drawing-masks-the-layers", "IconDrawing.java",
     "IconProtocol.outset(size);", "IconProtocol.outset(size); icon.getIconMask();",
     "an adaptive icon's layers are drawn, outset, without a mask"),
    ("drawing-guesses-the-alpha", "IconDrawing.java",
     "IconProtocol.hasAlpha(pixels)", "false", "the alpha flag is read from the pixels"),
]

# One fact of the manifest changed per entry: (name, the text replaced, its replacement, what the problems must name).
MANIFEST_MUTATIONS = [
    ("a-permission-is-asked", 'package="com.kernarium.icons">',
     'package="com.kernarium.icons"><uses-permission android:name="android.permission.INTERNET" />', "uses-permission"),
    ("a-shared-user", 'package="com.kernarium.icons">', 'package="com.kernarium.icons" android:sharedUserId="android.uid.system">',
     "shares a user id"),
    ("another-package", 'package="com.kernarium.icons">', 'package="com.kernarium.other">', "package"),
    ("the-provider-is-not-exported", 'android:exported="true"', 'android:exported="false"', "provider's exported"),
    ("the-provider-is-open", '            android:permission="android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY"\n', "",
     "provider's permission"),
    ("the-provider-needs-another-permission", 'android:permission="android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY"',
     'android:permission="android.permission.INTERACT_ACROSS_USERS"', "provider's permission"),
    ("the-provider-grants-uris", 'android:directBootAware="true" />\n\n        <service', 'android:directBootAware="true" android:grantUriPermissions="true" />\n\n        <service',
     "provider sets grantUriPermissions"),
    ("the-service-is-not-isolated", 'android:isolatedProcess="true"', 'android:isolatedProcess="false"', "service's isolatedProcess"),
    ("the-service-is-exported", 'android:exported="false"', 'android:exported="true"', "service's exported"),
    ("the-service-has-a-process", 'android:isolatedProcess="true"', 'android:isolatedProcess="true" android:process=":x"',
     "names a process of its own"),
    ("the-application-is-not-direct-boot-aware", 'android:directBootAware="true"\n        android:usesNonSdkApi', "android:usesNonSdkApi",
     "application's directBootAware"),
    ("the-service-is-not-direct-boot-aware", 'android:isolatedProcess="true"\n            android:directBootAware="true"', 'android:isolatedProcess="true"',
     "service's directBootAware"),
    ("backup-is-allowed", 'android:allowBackup="false"', 'android:allowBackup="true"', "allowBackup"),
    ("hidden-apis-are-closed", '\n        android:usesNonSdkApi="true"', "", "usesNonSdkApi"),
    ("the-application-is-debuggable", 'android:allowBackup="false"', 'android:allowBackup="false" android:debuggable="true"', "debuggable"),
    ("an-activity", "    </application>", '    <activity android:name=".Main" /></application>', "components"),
    ("a-receiver", "    </application>", '    <receiver android:name=".Boot" /></application>', "components"),
    ("a-launcher-entry", '<service\n            android:name=".IconRenderService"',
     '<service\n            android:name=".IconRenderService"><intent-filter><action android:name="a" /></intent-filter></service><service\n            android:name=".IconRenderService2"',
     "intent-filter"),
    ("a-library", "    </application>", '    <uses-library android:name="x" /></application>', "uses-library"),
]

APP_FORBIDDEN = ("Runtime.getRuntime", "ProcessBuilder", "java.net.", "Socket", "SystemProperties", "Settings.",
                 "FileWriter", "FileOutputStream", "Files.write", "RandomAccessFile", "getSharedPreferences",
                 "openFileOutput", "getFilesDir", "getCacheDir", "getExternal", "getDataDir", "SQLite", "ClipboardManager")


def jdk_tool(name: str) -> str | None:
    """A working javac or java: the macOS /usr/bin stubs exist without a JDK, so ask it."""
    path = shutil.which(name)
    if path is None:
        return None
    probe = subprocess.run([path, "-version"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=60, check=False)
    return path if probe.returncode == 0 else None


def strip_comments(text: str) -> str:
    """Java source without its comments (the sources have no string with a slash pair in it)."""
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))


def problems_of(name: str, text: str) -> list[str]:
    code = strip_comments(text)
    return [message for message, holds in RULES[name] if not holds(code)]


def check_glue(sources: dict[str, str]) -> int:
    """The rules hold for the app's code, and each rule fails for a copy of it that breaks it."""
    failures = 0
    for name in GLUE:
        for message in problems_of(name, sources[name]):
            print(f"FAIL: {name}: {message}", file=sys.stderr)
            failures += 1
    for name, source, old, new, message in MUTATIONS:
        text = sources[source]
        if text.count(old) != 1:
            print(f"FAIL: mutation {name}: the text to replace occurs {text.count(old)} times in {source}", file=sys.stderr)
            failures += 1
        elif message not in problems_of(source, text.replace(old, new)):
            print(f"FAIL: mutation {name} was not caught by the rule '{message}'", file=sys.stderr)
            failures += 1
        else:
            print(f"mutation {name}: caught by '{message}'")
    return failures


def manifest_problems(text: str) -> list[str]:
    """What the manifest must say: an unprivileged, direct-boot-aware application with a guarded provider and an
    isolated, unexported service, and nothing else (no permission, activity, receiver, launcher entry or shared user)."""
    problems: list[str] = []
    root = ElementTree.fromstring(text)
    attr = lambda element, name: element.get(ANDROID + name)  # noqa: E731
    if root.tag != "manifest" or root.get("package") != "com.kernarium.icons":
        problems.append("the package is not com.kernarium.icons")
    if attr(root, "sharedUserId") is not None or attr(root, "sharedUserLabel") is not None:
        problems.append("the app shares a user id")
    for tag in ("uses-permission", "uses-permission-sdk-23", "permission", "uses-feature", "queries", "instrumentation"):
        if root.findall(tag):
            problems.append(f"the manifest has {tag}: the app asks for and declares nothing")
    applications = root.findall("application")
    if len(applications) != 1:
        return problems + ["the manifest has not exactly one application"]
    application = applications[0]
    expected = {"allowBackup": "false", "directBootAware": "true", "usesNonSdkApi": "true"}
    for name, value in expected.items():
        if attr(application, name) != value:
            problems.append(f"the application's {name} is not {value}")
    for name in ("debuggable", "persistent", "process", "sharedUserId", "usesCleartextTraffic", "testOnly", "backupAgent",
                 "allowClearUserData", "largeHeap", "requestLegacyExternalStorage", "hasFragileUserData"):
        if attr(application, name) is not None:
            problems.append(f"the application sets {name}")
    children = [child.tag for child in application]
    if sorted(children) != ["provider", "service"]:
        problems.append(f"the application's components are {children}: one provider and one service, no activity, receiver or alias")
    for element in root.iter():
        if element.tag in ("intent-filter", "meta-data", "uses-library", "uses-native-library", "grant-uri-permission",
                           "path-permission", "property"):
            problems.append(f"the manifest has {element.tag}")
        if element.tag in ("provider", "service") and attr(element, "process") is not None:
            problems.append(f"the {element.tag} names a process of its own")
    for provider in application.findall("provider"):
        facts = {"name": ".IconProvider", "authorities": "com.kernarium.icons", "exported": "true",
                 "permission": "android.permission.ACCESS_CONTENT_PROVIDERS_EXTERNALLY", "directBootAware": "true"}
        for name, value in facts.items():
            if attr(provider, name) != value:
                problems.append(f"the provider's {name} is not {value}")
        for name in ("readPermission", "writePermission", "grantUriPermissions", "multiprocess", "syncable",
                     "isolatedProcess", "enabled"):
            if attr(provider, name) is not None:
                problems.append(f"the provider sets {name}")
    for service in application.findall("service"):
        facts = {"name": ".IconRenderService", "exported": "false", "isolatedProcess": "true", "directBootAware": "true"}
        for name, value in facts.items():
            if attr(service, name) != value:
                problems.append(f"the service's {name} is not {value}")
        for name in ("permission", "enabled", "foregroundServiceType", "externalService", "useAppZygote"):
            if attr(service, name) is not None:
                problems.append(f"the service sets {name}")
    return problems


def wiring_problems(makefile: str, build: str, verifier: str) -> list[str]:
    """Whether the image, its build and its verifier all carry the app."""
    problems: list[str] = []
    if not re.search(r"^PRODUCT_PACKAGES \+= KernariumIcons$", makefile, re.MULTILINE):
        problems.append("lineage_andarium.mk does not install KernariumIcons")
    if re.search(r"^PRODUCT_PACKAGES \+= KernariumIcons$", makefile, re.MULTILINE) and \
            len(re.findall(r"^PRODUCT_PACKAGES \+= KernariumIcons$", makefile, re.MULTILINE)) != 1:
        problems.append("lineage_andarium.mk installs KernariumIcons more than once")
    if '[[ "$PRODUCT_PACKAGES" == *" KernariumIcons "* ]]' not in build:
        problems.append("build-aosp.sh does not require KernariumIcons in the product")
    if not re.search(r"^\s+check_icons_app\(", verifier, re.MULTILINE):
        problems.append("verify-target-files.py has no check of the icon renderer")
    if "def check_icons_app(" not in verifier:
        problems.append("verify-target-files.py does not define check_icons_app")
    if '"SYSTEM_EXT/app/KernariumIcons/KernariumIcons.apk"' not in verifier:
        problems.append("verify-target-files.py does not require the app's package in the system_ext image")
    return problems


def check_wiring() -> list[str]:
    makefile, build, verifier = (path.read_text(encoding="utf-8") for path in (PRODUCT_MAKEFILE, BUILD_SCRIPT, VERIFIER))
    problems = wiring_problems(makefile, build, verifier)
    mutations = (
        ("product-line-removed", "PRODUCT_PACKAGES += KernariumIcons\n", "", "does not install KernariumIcons", 0),
        ("product-line-doubled", "PRODUCT_PACKAGES += KernariumIcons\n", "PRODUCT_PACKAGES += KernariumIcons\nPRODUCT_PACKAGES += KernariumIcons\n",
         "more than once", 0),
        ("product-line-renamed", "PRODUCT_PACKAGES += KernariumIcons\n", "PRODUCT_PACKAGES += KernariumIcon\n", "does not install KernariumIcons", 0),
        ("build-requirement-removed", '[[ "$PRODUCT_PACKAGES" == *" KernariumIcons "* ]]', "true", "does not require KernariumIcons", 1),
        ("verifier-call-removed", "    check_icons_app(", "    skip_icons_app(", "has no check of the icon renderer", 2),
        ("verifier-definition-removed", "def check_icons_app(", "def skip_icons_app(", "does not define check_icons_app", 2),
        ("verifier-entry-removed", '"SYSTEM_EXT/app/KernariumIcons/KernariumIcons.apk"', '"SYSTEM_EXT/app/KernariumIcons/Other.apk"',
         "does not require the app's package", 2),
    )
    texts = [makefile, build, verifier]
    for name, old, new, expected, which in mutations:
        if texts[which].count(old) < 1:
            problems.append(f"wiring mutation {name}: the text to replace is absent")
            continue
        changed = list(texts)
        changed[which] = texts[which].replace(old, new)
        if not any(expected in problem for problem in wiring_problems(*changed)):
            problems.append(f"wiring mutation {name} was not caught as '{expected}'")
        else:
            print(f"wiring mutation {name}: caught as '{expected}'")
    return problems


def check_app() -> int:
    """The app's files: headers, what the code must not touch, the manifest and the build file."""
    problems: list[str] = []
    files = {path.relative_to(APP).as_posix(): path.read_text(encoding="utf-8") for path in sorted(APP.rglob("*")) if path.is_file()}
    expected_files = {"Android.bp", "AndroidManifest.xml", *(f"src/com/kernarium/icons/{name}" for name in (*GLUE, *PLATFORM_FREE))}
    if set(files) != expected_files:
        problems.append(f"the app's files are {sorted(files)}, not {sorted(expected_files)}")
    for name, text in files.items():
        if name.endswith((".java", ".bp")) and not text.startswith("// SPDX-License-Identifier: Apache-2.0\n"):
            problems.append(f"{name} does not carry the Apache-2.0 header of code that goes into the image")
        if name.endswith(".xml") and "<!-- SPDX-License-Identifier: Apache-2.0 -->" not in text:
            problems.append(f"{name} does not carry the Apache-2.0 header of code that goes into the image")
        if name.endswith(".java"):
            code = strip_comments(text)
            for forbidden in APP_FORBIDDEN:
                if forbidden in code:
                    problems.append(f"{name} uses {forbidden}: the app writes nothing, runs nothing and opens no network")
    for name in PLATFORM_FREE:
        free = strip_comments(files[f"src/com/kernarium/icons/{name}"])
        if re.search(r"^\s*import\s+(android|java\.io|java\.net)", free, re.MULTILINE):
            problems.append(f"{name} imports the platform, files or the network: it runs on a JVM")
    sources = {name: files[f"src/com/kernarium/icons/{name}"] for name in GLUE}
    if check_glue(sources):
        problems.append("the app's code breaks a rule it must keep")
    manifest = files["AndroidManifest.xml"]
    problems += manifest_problems(manifest)
    for name, old, new, expected in MANIFEST_MUTATIONS:
        if manifest.count(old) != 1:
            problems.append(f"manifest mutation {name}: the text to replace occurs {manifest.count(old)} times")
        elif not any(expected in problem for problem in manifest_problems(manifest.replace(old, new))):
            problems.append(f"manifest mutation {name} was not caught as '{expected}'")
        else:
            print(f"manifest mutation {name}: caught as '{expected}'")
    bp = files["Android.bp"]
    code = "\n".join(line for line in bp.splitlines() if not line.lstrip().startswith("//"))
    for expected in ('android_app {', 'name: "KernariumIcons"', 'manifest: "AndroidManifest.xml"', 'srcs: ["src/**/*.java"]',
                     'platform_apis: true', 'system_ext_specific: true', 'filegroup {', 'name: "kernarium_icon_protocol"',
                     'srcs: ["src/com/kernarium/icons/IconProtocol.java"]'):
        if expected not in code:
            problems.append(f"Android.bp lacks {expected}")
    for forbidden in ("privileged", "certificate", "uses_libs", "jni_libs", "required", "product_specific", "vendor",
                      "overrides", "dex_preopt", "use_embedded_native_libs", "test_only"):
        if forbidden in code:
            problems.append(f"Android.bp has {forbidden}: the app is an ordinary, default-key application of system_ext")
    problems += check_wiring()
    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    return 1 if problems else 0


def compile_and_run(javac: str, java: str, scratch: pathlib.Path, sources: list[pathlib.Path]) -> tuple[int, str]:
    classes = scratch / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    compiled = subprocess.run(
        [javac, "-Xlint:all", "-Werror", "-encoding", "UTF-8", "-d", str(classes), *map(str, sources), str(TEST)],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240, check=False)
    if compiled.returncode != 0:
        return -1, f"javac:\n{compiled.stdout}"
    ran = subprocess.run([java, "-Xmx64m", "-Dfile.encoding=UTF-8", "-cp", str(classes), "com.kernarium.icons.IconProtocolTest"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         timeout=120, check=False)
    return ran.returncode, ran.stdout


def failed_rows(output: str) -> set[str]:
    return {m.group(1) for m in re.finditer(r"^FAIL ([a-z0-9-]+)", output, re.MULTILINE)}


def main() -> int:
    if check_app() != 0:
        return 1
    javac, java = jdk_tool("javac"), jdk_tool("java")
    if javac is None or java is None:
        print("SKIP: no JDK (javac and java) on PATH")
        return SKIP
    sources = {name: SOURCES / name for name in PLATFORM_FREE}
    with tempfile.TemporaryDirectory(prefix="uml-icon-renderer-") as directory:
        scratch = pathlib.Path(directory)
        status, output = compile_and_run(javac, java, scratch / "base", list(sources.values()))
        print(output, end="")
        if status != 0 or not output.rstrip().endswith("PASS"):
            print("FAIL: the protocol's test", file=sys.stderr)
            return 1
        rows = set(re.findall(r"^ok ([a-z0-9-]+)", output, re.MULTILINE))

        def mutate(mutation: tuple) -> tuple[bool, str]:
            file, name, old, new, must_fail = mutation
            unknown = must_fail - rows
            if unknown:
                return False, f"FAIL: mutation {name} names rows that do not exist: {sorted(unknown)}"
            text = sources[file].read_text(encoding="utf-8")
            if text.count(old) != 1:
                return False, f"FAIL: mutation {name}: the text to replace occurs {text.count(old)} times in {file}"
            copy = scratch / f"src-{name}"
            copy.mkdir()
            target = copy / file
            target.write_text(text.replace(old, new), encoding="utf-8")
            others = [path for other, path in sources.items() if other != file]
            mutant_status, mutant_output = compile_and_run(javac, java, scratch / f"run-{name}", [target, *others])
            shutil.rmtree(copy)
            if mutant_status == -1:
                return False, f"FAIL: mutation {name} does not compile:\n{mutant_output}"
            missed = must_fail - failed_rows(mutant_output)
            if mutant_status == 0 or missed:
                return False, f"FAIL: mutation {name} was not caught by {sorted(missed) or 'any row'}"
            return True, f"mutation {name}: caught by {sorted(failed_rows(mutant_output) & must_fail)}"

        workers = max(1, min(3, (os.cpu_count() or 1) - 1))
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(mutate, [("IconProtocol.java", *m) for m in PROTOCOL_MUTATIONS]
                                + [("IconWorkerCore.java", *m) for m in WORKER_MUTATIONS]))
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
