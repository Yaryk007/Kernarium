// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Old app compatibility shell command (android/lineage-24/overlay/oldapp_switch) against a fake
// platform, on a plain JVM: the eligibility rule, the listing's format, the stored value that names
// an installation, the cleanup and its lock (judged on what is true under the lock), the names that
// the property listing gives and the values read by name, the lock file on real files, the writes,
// the order of the stop after the write and the exit statuses. The package is the command's, so that
// the test reaches what the command keeps to itself. test_oldapp_switch_command.py compiles and runs it.

package com.kernarium.oldapp;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringReader;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.locks.ReentrantLock;

public final class OldAppCommandTest {
    private static int failures;

    private static void check(boolean condition, String what) {
        if (!condition) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static void same(Object expected, Object actual, String what) {
        check(expected.equals(actual), what + ": expected <" + expected + "> but got <" + actual + ">");
    }

    private static final List<String> ABIS32 = Arrays.asList("armeabi-v7a", "armeabi");

    /** The first install is before every turn-on of the fixtures unless a test says otherwise. */
    private static final long FIRST_INSTALL = 50_000L;

    private static Candidate app(String name, boolean system, String abi, int target, String sharedUser,
            long updated) {
        return new Candidate(name, system, abi, target, sharedUser, FIRST_INSTALL, updated, name);
    }

    /** An eligible installation first installed at the given epoch milliseconds. */
    private static Candidate installedAt(String name, long firstInstallMillis) {
        return new Candidate(name, false, "armeabi-v7a", 10, null, firstInstallMillis, firstInstallMillis / 1000, name);
    }

    /** The stored value of a choice made at a time, for the installation first installed at another. */
    private static String choice(long turnedOnSeconds, long firstInstallMillis) {
        return turnedOnSeconds + " " + firstInstallMillis;
    }

    private static String letters(int count) {
        StringBuilder text = new StringBuilder();
        for (int i = 0; i < count; i++) {
            text.append('a');
        }
        return text.toString();
    }

    private static String key(String packageName) {
        return OldAppEligibility.propertyName(packageName);
    }

    /** A point at which the first listing to reach it stops until the test lets it go on. */
    private static final class Pause {
        final CountDownLatch reached = new CountDownLatch(1);
        final CountDownLatch resume = new CountDownLatch(1);
        private final AtomicBoolean taken = new AtomicBoolean();

        void hit() throws Exception {
            if (taken.compareAndSet(false, true)) {
                reached.countDown();
                if (!resume.await(10, TimeUnit.SECONDS)) {
                    throw new IllegalStateException("the test did not let the listing go on");
                }
            }
        }
    }

    private static final class Fake implements OldAppCommand.Platform {
        final List<Candidate> installed = new CopyOnWriteArrayList<>();
        final Map<String, String> labels = new HashMap<>();
        final Map<String, String> properties = new ConcurrentHashMap<>();
        final List<String> events = Collections.synchronizedList(new ArrayList<String>());
        final ReentrantLock monitor = new ReentrantLock();
        boolean keepWrites = true;
        boolean lockTimesOut;
        boolean lockFails;
        int locksTaken;
        int locksHeld;
        int writesOutsideTheLock;
        /** The listing stops after its snapshot of the installed packages, before it reads any choice. */
        Pause afterInstalled;
        /** The listing stops with its snapshot complete, when it asks for the lock. */
        Pause beforeLock;
        boolean stopFails;
        boolean listFails;
        long now = 1700000000L;

        @Override
        public List<Candidate> installedPackages() throws Exception {
            if (listFails) {
                throw new IllegalStateException("the package manager died");
            }
            return new ArrayList<>(installed);
        }

        @Override
        public Candidate find(String packageName) {
            for (Candidate candidate : installed) {
                if (candidate.packageName.equals(packageName)) {
                    return candidate;
                }
            }
            return null;
        }

        @Override
        public List<String> abis32() {
            return ABIS32;
        }

        @Override
        public String label(Candidate candidate) throws Exception {
            String label = labels.get(candidate.packageName);
            if (label != null && label.equals("<throws>")) {
                throw new IllegalStateException("no resources");
            }
            return label;
        }

        @Override
        public String getProperty(String name) {
            return properties.getOrDefault(name, "");
        }

        /** What {@code getprop} prints: a value is printed as it is, line breaks and all. */
        String getpropOutput() {
            StringBuilder text = new StringBuilder();
            for (Map.Entry<String, String> property : new TreeMap<>(properties).entrySet()) {
                text.append('[').append(property.getKey()).append("]: [").append(property.getValue()).append("]\n");
            }
            return text.toString();
        }

        @Override
        public Collection<String> storedPackages() throws Exception {
            if (afterInstalled != null) {
                afterInstalled.hit();
            }
            return PropertyListing.storedPackages(new BufferedReader(new StringReader(getpropOutput())));
        }

        @Override
        public OldAppCommand.Lock lock(long timeoutMillis) throws Exception {
            if (beforeLock != null) {
                beforeLock.hit();
            }
            if (lockFails) {
                throw new IllegalStateException("cannot create the lock file");
            }
            if (lockTimesOut || !monitor.tryLock(Math.min(timeoutMillis, 2000), TimeUnit.MILLISECONDS)) {
                return null;
            }
            locksTaken++;
            locksHeld++;
            return new OldAppCommand.Lock() {
                @Override
                public void release() {
                    locksHeld--;
                    monitor.unlock();
                }
            };
        }

        @Override
        public void setProperty(String name, String value) {
            if (!monitor.isHeldByCurrentThread()) {
                writesOutsideTheLock++;
            }
            events.add("set " + name + "=" + value);
            if (keepWrites) {
                properties.put(name, value);
            }
        }

        @Override
        public void forceStop(String packageName) {
            events.add("stop " + packageName);
            if (stopFails) {
                throw new IllegalStateException("cmd activity force-stop exited 255");
            }
        }

        @Override
        public long nowSeconds() {
            return now;
        }
    }

    private static final class Result {
        int status;
        String out;
        String err;
    }

    private static Result run(Fake platform, String... args) {
        StringWriter out = new StringWriter();
        StringWriter err = new StringWriter();
        Result result = new Result();
        PrintWriter outWriter = new PrintWriter(out);
        PrintWriter errWriter = new PrintWriter(err);
        result.status = OldAppCommand.run(args, platform, outWriter, errWriter);
        outWriter.flush();
        errWriter.flush();
        result.out = out.toString();
        result.err = err.toString();
        return result;
    }

    private static Fake fixture() {
        Fake fake = new Fake();
        fake.installed.add(app("org.example.zed", false, "armeabi-v7a", 10, null, 300L));
        fake.installed.add(app("org.example.alpha", false, "armeabi", 13, null, 100L));
        fake.installed.add(app("org.example.modern", false, "armeabi-v7a", 14, null, 5L));
        fake.installed.add(app("org.example.sixty_four", false, "arm64-v8a", 10, null, 5L));
        fake.installed.add(app("org.example.javaonly", false, null, 10, null, 5L));
        fake.installed.add(app("org.example.system", true, "armeabi-v7a", 10, null, 5L));
        fake.installed.add(app("org.example.shared", false, "armeabi-v7a", 10, "org.example.uid", 5L));
        fake.installed.add(app("org.example.nolabel", false, "armeabi-v7a", 4, null, 7L));
        fake.installed.add(app("org.example.broken", false, "armeabi-v7a", 4, null, 8L));
        fake.labels.put("org.example.zed", "Zed");
        fake.labels.put("org.example.alpha", "Alpha\tGame\nTwo\r\nLines End");
        fake.labels.put("org.example.broken", "<throws>");
        fake.properties.put("persist.sys.kernarium.oldapp.org.example.zed", choice(1699999000L, FIRST_INSTALL));
        return fake;
    }

    private static void eligibility() {
        same(true, OldAppEligibility.isEligible(app("a.b", false, "armeabi-v7a", 13, null, 0), ABIS32), "armeabi-v7a, 13");
        same(true, OldAppEligibility.isEligible(app("a.b", false, "armeabi", 0, null, 0), ABIS32), "armeabi, 0");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "armeabi-v7a", 14, null, 0), ABIS32), "target 14");
        same(false, OldAppEligibility.isEligible(app("a.b", true, "armeabi-v7a", 10, null, 0), ABIS32), "system");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "armeabi-v7a", 10, "uid", 0), ABIS32), "shared user");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "arm64-v8a", 10, null, 0), ABIS32), "64-bit");
        same(false, OldAppEligibility.isEligible(app("a.b", false, null, 10, null, 0), ABIS32), "no native code");
        same(false, OldAppEligibility.isEligible(app("a.b", false, "x86", 10, null, 0), ABIS32), "x86 on an arm device");
        same("persist.sys.kernarium.oldapp.a.b", OldAppEligibility.propertyName("a.b"), "property name");
        // A name the property service cannot take as it is makes the package not eligible.
        for (String good : new String[] {"a.b", "org.example.Foo_1", "com.a1.b2", "single", "1a.b", "a-b.c", "a@b:c"}) {
            check(OldAppEligibility.isPropertyKeyPart(good), "property key part accepted: " + good);
            check(OldAppEligibility.isEligible(app(good, false, "armeabi-v7a", 10, null, 0), ABIS32),
                    "eligible by its name: " + good);
        }
        for (String bad : new String[] {"", ".a", "a.", "a..b", "a b", "a.b/c", "a.b\n", "../x", "a.b;c",
                "org.example..oldapp", "org.example.oldapp."}) {
            check(!OldAppEligibility.isPropertyKeyPart(bad), "property key part refused: <" + bad + ">");
            check(!OldAppEligibility.isEligible(app(bad, false, "armeabi-v7a", 10, null, 0), ABIS32),
                    "not eligible by its name: <" + bad + ">");
        }
        // The whole key is bounded: 29 bytes of prefix and at most 226 of name.
        check(OldAppEligibility.isPropertyKeyPart(letters(226)), "226 characters make a 255-byte key");
        check(!OldAppEligibility.isPropertyKeyPart(letters(227)), "227 characters make a 256-byte key");
        check(OldAppEligibility.isPropertyKeyPart(letters(113) + "." + letters(112)), "dots count in the bound");
        check(!OldAppEligibility.isPropertyKeyPart(letters(113) + "." + letters(113)), "dots count in the bound, over");
        check(!OldAppEligibility.isPropertyKeyPart(letters(100_000)), "a huge name is refused without the pattern");
        same(255, OldAppEligibility.STORE_PREFIX.length() + 226, "the prefix and the longest name");
        check(OldAppEligibility.isEligible(app(letters(226), false, "armeabi-v7a", 10, null, 0), ABIS32), "eligible at the bound");
        check(!OldAppEligibility.isEligible(app(letters(227), false, "armeabi-v7a", 10, null, 0), ABIS32), "not eligible over it");
    }

    /** outlived: a stored choice belongs to the installation whose first-install time equals the stored one. */
    private static void installations() {
        Candidate current = installedAt("a.b", 1_000_007L);
        same(false, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 1_000_007L)), "the same installation");
        same(false, OldAppEligibility.outlived(current, ABIS32, choice(5L, 1_000_007L)),
                "the turn-on time is not compared with anything");
        same(false, OldAppEligibility.outlived(current, ABIS32, choice(2_000_000_000L, 1_000_007L)),
                "not even a turn-on time in the future");
        same(true, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 1_000_008L)), "another millisecond");
        same(true, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 999_000L)), "an earlier installation");
        same(true, OldAppEligibility.outlived(current, ABIS32, choice(1000L, 5_000_000L)), "a later installation");
        for (String value : new String[] {"1700000000", "on", "", "1 ", " 1 2", "1  2", "1 2 3", "-1 5", "1 -5", "a b",
                "1\t2", "1 2\n", "9999999999999999999 5", "5 9999999999999999999"}) {
            same(true, OldAppEligibility.outlived(current, ABIS32, value), "a value that names no installation: <" + value + ">");
        }
        same("1000 1000007", OldAppEligibility.choiceValue(1000L, current), "the stored form");
        OldAppEligibility.Choice read = OldAppEligibility.parseChoice("1000 1000007");
        check(read != null && read.turnedOnSeconds == 1000L && read.installationMillis == 1_000_007L, "read back");
        same(true, OldAppEligibility.outlived(null, ABIS32, choice(1000L, 1_000_007L)), "not installed");
        same(true, OldAppEligibility.outlived(app("a.b", false, "armeabi-v7a", 14, null, 0), ABIS32,
                choice(1000L, FIRST_INSTALL)), "target raised");
        same(true, OldAppEligibility.outlived(new Candidate("a.b", false, "armeabi-v7a", 10, "uid", 1_000_007L, 1L, "a.b"),
                ABIS32, choice(1000L, 1_000_007L)), "shared user ID");
    }

    private static void listing() {
        Fake fake = fixture();
        Result result = run(fake, "list");
        same(0, result.status, "list status");
        same("", result.err, "list stderr");
        String[] lines = result.out.split("\n", -1);
        same("kernarium_oldapp 1", lines[0], "list header");
        // Sorted by package name; only the eligible ones.
        same("org.example.alpha\tAlpha Game Two  Lines End\toff\t0\t100", lines[1], "alpha line");
        same("org.example.broken\torg.example.broken\toff\t0\t8", lines[2], "label that cannot be read");
        same("org.example.nolabel\torg.example.nolabel\toff\t0\t7", lines[3], "label that is null");
        same("org.example.zed\tZed\ton\t1699999000\t300", lines[4], "zed line");
        same("", lines[5], "list ends with a newline");
        same(6, lines.length, "list line count");
        for (String line : lines) {
            if (!line.isEmpty() && !line.startsWith("kernarium_oldapp")) {
                same(5, line.split("\t", -1).length, "five fields: " + line);
            }
        }
        same(0, fake.events.size(), "list writes nothing");
        same(0, fake.locksTaken, "a listing with nothing to clear does not use the lock");

        Fake none = new Fake();
        Result empty = run(none, "list");
        same("kernarium_oldapp 1\n", empty.out, "an empty listing is the header");
        same(0, empty.status, "empty listing status");

        Fake failing = fixture();
        failing.listFails = true;
        Result failed = run(failing, "list");
        same(1, failed.status, "list failure status");
        same("", failed.out, "list failure prints no listing");
        check(failed.err.startsWith("kernarium_oldapp: list failed:"), "list failure message: " + failed.err);
    }

    private static void switching() {
        Fake fake = fixture();
        Result on = run(fake, "set", "org.example.alpha", "on");
        same(0, on.status, "set on status");
        same("org.example.alpha\ton\n", on.out, "set on output");
        same("", on.err, "set on stderr");
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.alpha=" + choice(1700000000L, FIRST_INSTALL),
                "stop org.example.alpha"),
                fake.events, "write before stop");
        // The listing now shows it.
        check(run(fake, "list").out.contains("org.example.alpha\tAlpha Game Two  Lines End\ton\t1700000000\t100\n"),
                "listing after set on");

        fake.events.clear();
        Result off = run(fake, "set", "org.example.zed", "off");
        same(0, off.status, "set off status");
        same("org.example.zed\toff\n", off.out, "set off output");
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.zed=", "stop org.example.zed"), fake.events,
                "off clears then stops");
        check(run(fake, "list").out.contains("org.example.zed\tZed\toff\t0\t300\n"), "listing after set off");

        // On is refused for an application that is not eligible or not installed.
        for (String refused : new String[] {"org.example.modern", "org.example.sixty_four", "org.example.javaonly",
                "org.example.system", "org.example.shared", "org.example.unknown"}) {
            fake.events.clear();
            Result result = run(fake, "set", refused, "on");
            same(2, result.status, "refused status for " + refused);
            same("", result.out, "refused stdout for " + refused);
            same(1, result.err.split("\n").length, "one line on stderr for " + refused);
            check(result.err.contains(refused), "the refusal names " + refused + ": " + result.err);
            same(0, fake.events.size(), "a refusal writes and stops nothing: " + refused);
        }

        Fake dropped = fixture();
        dropped.keepWrites = false;
        Result notKept = run(dropped, "set", "org.example.alpha", "on");
        same(1, notKept.status, "a write the system did not keep");
        same("", notKept.out, "no success line");
        same(Arrays.asList("set persist.sys.kernarium.oldapp.org.example.alpha=" + choice(1700000000L, FIRST_INSTALL)),
                dropped.events,
                "nothing is stopped after a write that was not kept");

        Fake stuck = fixture();
        stuck.stopFails = true;
        Result stopFailed = run(stuck, "set", "org.example.alpha", "on");
        same(1, stopFailed.status, "a stop that failed");
        check(stopFailed.err.contains("could not be stopped"), "the stop failure is said: " + stopFailed.err);
        same("", stopFailed.out, "no success line after a failed stop");
    }

    private static void usage() {
        String[][] bad = {
            {}, {"help"}, {"list", "extra"}, {"set"}, {"set", "org.example.alpha"}, {"set", "org.example.alpha", "maybe"},
            {"set", "org.example.alpha", "ON"}, {"set", "--help"},
            {"set", "org.example.alpha", "on", "extra"}, {"LIST"}, {"get", "org.example.alpha"},
        };
        for (String[] args : bad) {
            Fake fake = fixture();
            Result result = run(fake, args);
            String shown = String.join(" ", args);
            same(64, result.status, "usage status for <" + shown + ">");
            check(result.err.startsWith("usage: kernarium_oldapp list\n"), "usage text for <" + shown + ">: " + result.err);
            same("", result.out, "usage stdout for <" + shown + ">");
            same(0, fake.events.size(), "usage writes nothing for <" + shown + ">");
        }
    }

    /** A name that cannot be a property name is not usage but a refusal, for on and for off. */
    private static void names() {
        for (String name : new String[] {"", ".a", "a.", "a..b", "a b", "../x", "a.b\n", "a.b;c",
                "org.example..oldapp", "org.example.oldapp."}) {
            for (String state : new String[] {"on", "off"}) {
                Fake fake = fixture();
                Result result = run(fake, "set", name, state);
                same(2, result.status, "refused status for <" + name + "> " + state);
                same("", result.out, "refused stdout for <" + name + "> " + state);
                same(0, fake.events.size(), "nothing is written for <" + name + "> " + state);
            }
        }
        // The reviewer's two installed names are not listed either.
        Fake fake = fixture();
        fake.installed.add(app("org.example..oldapp", false, "armeabi-v7a", 10, null, 9L));
        fake.installed.add(app("org.example.oldapp.", false, "armeabi-v7a", 10, null, 9L));
        String listing = run(fake, "list").out;
        check(!listing.contains("org.example..oldapp") && !listing.contains("org.example.oldapp."),
                "unrepresentable names are not listed: " + listing);
    }

    /** The cases of the amendments: a choice does not outlive the installation it was made for. */
    private static void lifecycle() {
        // Codex's first sequence: enabled at second 1000, the clock goes back, the package is uninstalled and
        // installed again at second 500. Another installation: cleared, whatever the clock says.
        Fake back = new Fake();
        back.installed.add(installedAt("org.example.again", 500_003L));
        back.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
        Result result = run(back, "list");
        same("kernarium_oldapp 1\norg.example.again\torg.example.again\toff\t0\t500\n", result.out,
                "a reinstall after the clock went back is listed off");
        same(Arrays.asList("set " + key("org.example.again") + "="), back.events, "and its choice is cleared");
        same(1, back.locksTaken, "a listing that clears a choice takes the lock once, for all it clears");
        same("", back.getProperty(key("org.example.again")), "the property is empty");
        back.events.clear();
        run(back, "list");
        same(0, back.events.size(), "a second listing writes nothing");

        // The second sequence: enabled at second 101, reinstalled within the same second.
        Fake same = new Fake();
        same.installed.add(installedAt("org.example.again", 101_900L));
        same.properties.put(key("org.example.again"), choice(101L, 101_250L));
        check(run(same, "list").out.contains("org.example.again\torg.example.again\toff\t0\t101\n"),
                "a reinstall in the same second is another installation");
        same("", same.getProperty(key("org.example.again")), "and its choice is cleared");

        // The third: the same installation, the clock corrected backwards, then a new enable.
        Fake corrected = new Fake();
        corrected.installed.add(installedAt("org.example.zone", 1_000_007L));
        corrected.now = 900L;
        same(0, run(corrected, "set", "org.example.zone", "on").status, "enable after the clock went back");
        same(choice(900L, 1_000_007L), corrected.getProperty(key("org.example.zone")), "the value names the installation");
        corrected.events.clear();
        same("kernarium_oldapp 1\norg.example.zone\torg.example.zone\ton\t900\t1000\n", run(corrected, "list").out,
                "the new choice is kept and its turn-on time is the one the person made");
        same(0, corrected.events.size(), "and nothing is cleared");

        // An in-place update keeps the first-install time (the last update moves): the choice stays.
        Fake updated = new Fake();
        updated.installed.add(new Candidate("org.example.upd", false, "armeabi-v7a", 10, null, 1_000_007L, 77_777L, "x"));
        updated.properties.put(key("org.example.upd"), choice(1000L, 1_000_007L));
        same("kernarium_oldapp 1\norg.example.upd\torg.example.upd\ton\t1000\t77777\n", run(updated, "list").out,
                "an update keeps the choice");
        same(0, updated.events.size(), "and writes nothing");

        // The earlier one-number form, and anything else that does not name an installation, is cleared.
        for (String value : new String[] {"1700000000", "on", "1 2 3"}) {
            Fake old = new Fake();
            old.installed.add(installedAt("org.example.old", 1_000_007L));
            old.properties.put(key("org.example.old"), value);
            same("kernarium_oldapp 1\norg.example.old\torg.example.old\toff\t0\t1000\n", run(old, "list").out,
                    "a value <" + value + "> is listed off");
            same("", old.getProperty(key("org.example.old")), "and cleared: <" + value + ">");
        }

        // An update that raised the target level, a reinstall with a shared user ID, a system application, an
        // application that is gone: each choice is cleared and none is listed; the one that still belongs to
        // its installation stays. A name that cannot be a property name is not named by the listing (the
        // property service would not hold it) and is left alone.
        Fake gone = new Fake();
        gone.installed.add(new Candidate("org.example.raised", false, "armeabi-v7a", 14, null, 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.shared", false, "armeabi-v7a", 10, "org.example.uid", 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.system", true, "armeabi-v7a", 10, null, 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.sixtyfour", false, "arm64-v8a", 10, null, 10_000L, 10L, "x"));
        gone.installed.add(new Candidate("org.example.keeps", false, "armeabi-v7a", 10, null, 10_000L, 10L, "x"));
        for (String name : new String[] {"org.example.raised", "org.example.shared", "org.example.system",
                "org.example.sixtyfour", "org.example.absent", "org.example..odd", "org.example.keeps"}) {
            gone.properties.put(key(name), choice(1700000000L, 10_000L));
        }
        Result cleaned = run(gone, "list");
        same(0, cleaned.status, "cleanup status");
        same("kernarium_oldapp 1\norg.example.keeps\torg.example.keeps\ton\t1700000000\t10\n", cleaned.out,
                "only the choice that still belongs to its application stays");
        same(5, gone.events.size(), "five choices are cleared");
        same(choice(1700000000L, 10_000L), gone.getProperty(key("org.example..odd")), "a name that is no key is not touched");
        for (String event : gone.events) {
            check(event.startsWith("set ") && event.endsWith("="), "a clearing write: " + event);
        }
        same(choice(1700000000L, 10_000L), gone.getProperty(key("org.example.keeps")), "the other choice is not touched");
        same(0, gone.writesOutsideTheLock, "every write was under the lock");
        same(0, gone.locksHeld, "and the lock is released");

        // A choice that cannot be cleared is not hidden: the listing fails and prints nothing.
        Fake stuck = new Fake();
        stuck.installed.add(installedAt("org.example.again", 500_003L));
        stuck.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
        stuck.keepWrites = false;
        Result failed = run(stuck, "list");
        same(1, failed.status, "a choice that stays fails the listing");
        same("", failed.out, "and prints no listing");
        check(failed.err.contains("did not keep"), "and says why: " + failed.err);
        same(0, stuck.locksHeld, "the lock is released after the failure");
    }

    /** A listing on another thread, started now. */
    private static Thread listing(final Fake fake, final Result[] into) {
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                into[0] = OldAppCommandTest.run(fake, "list");
            }
        });
        thread.start();
        return thread;
    }

    private static void finish(Thread thread, String what) throws Exception {
        thread.join(10_000);
        check(!thread.isAlive(), what + " finished");
    }

    /**
     * list and set are serialised, and the cleanup decides on what is true under the lock: the
     * installation and the value as they are then, not as the snapshot saw them.
     */
    private static void locking() throws Exception {
        // Codex's first interleaving. The listing has its snapshot (a stale value for the current
        // installation, first installed at 200 s) and asks for the lock; another command enables the
        // installation meanwhile; the listing must not erase the new choice.
        final Fake fake = new Fake();
        fake.installed.add(installedAt("org.example.race", 200_000L));
        fake.properties.put(key("org.example.race"), choice(100L, 100_000L));
        fake.beforeLock = new Pause();
        Result[] listed = new Result[1];
        Thread lister = listing(fake, listed);
        check(fake.beforeLock.reached.await(10, TimeUnit.SECONDS), "the listing reached the lock");
        fake.now = 500L;
        same(0, run(fake, "set", "org.example.race", "on").status, "the enable succeeded meanwhile");
        fake.beforeLock.resume.countDown();
        finish(lister, "the listing");
        same(0, listed[0].status, "the listing succeeded");
        same(choice(500L, 200_000L), fake.getProperty(key("org.example.race")), "the new choice is still there");
        check(listed[0].out.contains("org.example.race\torg.example.race\ton\t500\t200\n"),
                "and the listing shows it on: " + listed[0].out);
        check(!fake.events.contains("set " + key("org.example.race") + "="), "no clearing write was made");
        same(0, fake.writesOutsideTheLock, "every write was under the lock");
        same(0, fake.locksHeld, "the lock is released");

        // The value changed to another stale one meanwhile: it is judged as it is now and cleared.
        final Fake other = new Fake();
        other.installed.add(installedAt("org.example.race", 200_000L));
        other.properties.put(key("org.example.race"), choice(100L, 100_000L));
        other.beforeLock = new Pause();
        Result[] second = new Result[1];
        Thread again = listing(other, second);
        check(other.beforeLock.reached.await(10, TimeUnit.SECONDS), "the second listing reached the lock");
        other.properties.put(key("org.example.race"), choice(150L, 150_000L));
        other.beforeLock.resume.countDown();
        finish(again, "the second listing");
        same("", other.getProperty(key("org.example.race")), "a value that is stale now is cleared");
        check(second[0].out.contains("org.example.race\torg.example.race\toff\t0\t200\n"),
                "and the listing shows it off: " + second[0].out);

        // Codex's second interleaving: the package is not in the listing's snapshot of the installed
        // packages. It is installed and enabled meanwhile; the listing then reads the new choice,
        // finds no installation for it in its snapshot, and must still not erase it.
        Fake late = new Fake();
        late.afterInstalled = new Pause();
        Result[] third = new Result[1];
        Thread lateLister = listing(late, third);
        check(late.afterInstalled.reached.await(10, TimeUnit.SECONDS), "the third listing has its snapshot");
        late.installed.add(installedAt("org.example.late", 200_000L));
        late.now = 500L;
        same(0, run(late, "set", "org.example.late", "on").status, "a package installed after the snapshot is enabled");
        late.afterInstalled.resume.countDown();
        finish(lateLister, "the third listing");
        same(0, third[0].status, "the listing succeeded after the install");
        same(choice(500L, 200_000L), late.getProperty(key("org.example.late")), "the choice made after the snapshot stays");
        check(!late.events.contains("set " + key("org.example.late") + "="), "and nothing clears it");
        same(0, late.writesOutsideTheLock, "every write was under the lock (late)");
        same(0, late.locksHeld, "and the lock is released (late)");

        // The third: the package is in the snapshot, then replaced by another installation that is enabled.
        Fake replaced = new Fake();
        replaced.installed.add(installedAt("org.example.swap", 100_000L));
        replaced.properties.put(key("org.example.swap"), choice(100L, 100_000L));
        replaced.afterInstalled = new Pause();
        Result[] fourth = new Result[1];
        Thread swapLister = listing(replaced, fourth);
        check(replaced.afterInstalled.reached.await(10, TimeUnit.SECONDS), "the fourth listing has its snapshot");
        replaced.installed.clear();
        replaced.installed.add(installedAt("org.example.swap", 200_000L));
        replaced.now = 500L;
        same(0, run(replaced, "set", "org.example.swap", "on").status, "the replacement is enabled");
        replaced.afterInstalled.resume.countDown();
        finish(swapLister, "the fourth listing");
        same(0, fourth[0].status, "the listing succeeded after the replacement");
        same(choice(500L, 200_000L), replaced.getProperty(key("org.example.swap")), "the choice made for the replacement stays");
        check(!replaced.events.contains("set " + key("org.example.swap") + "="), "and nothing clears it (replacement)");
        check(fourth[0].out.contains("org.example.swap\torg.example.swap\ton\t500\t"),
                "and the listing shows it on: " + fourth[0].out);
        same(0, replaced.locksHeld, "the lock is released (replacement)");

        // A package replaced after the snapshot while the old installation's choice is still stored: the
        // choice is the old installation's and is cleared, because it is stale now.
        Fake stale = new Fake();
        stale.installed.add(installedAt("org.example.swap", 100_000L));
        stale.properties.put(key("org.example.swap"), choice(100L, 100_000L));
        stale.beforeLock = new Pause();
        stale.installed.clear();
        stale.installed.add(installedAt("org.example.swap", 300_000L));
        Result[] fifth = new Result[1];
        Thread staleLister = listing(stale, fifth);
        check(stale.beforeLock.reached.await(10, TimeUnit.SECONDS), "the fifth listing reached the lock");
        stale.beforeLock.resume.countDown();
        finish(staleLister, "the fifth listing");
        same("", stale.getProperty(key("org.example.swap")), "a choice for another installation is cleared");

        // A set waits for a listing's lock and then goes on.
        Fake held = new Fake();
        held.installed.add(installedAt("org.example.race", 200_000L));
        check(held.monitor.tryLock(), "the test holds the lock");
        final Result[] waiting = new Result[1];
        final Fake heldRef = held;
        Thread setter = new Thread(new Runnable() {
            @Override
            public void run() {
                waiting[0] = OldAppCommandTest.run(heldRef, "set", "org.example.race", "on");
            }
        });
        setter.start();
        Thread.sleep(300);
        same(0, held.events.size(), "nothing is written while another process holds the lock");
        held.monitor.unlock();
        setter.join(10_000);
        same(0, waiting[0].status, "the set goes on once the lock is free");
        same(2, held.events.size(), "the write and the stop were made after the wait");
    }

    /** Values that hold a line break reach the parser: they are stale, listed off and cleared. */
    private static void multilineValues() {
        String forged = "[" + key("org.example.victim") + "]: [" + choice(1L, FIRST_INSTALL) + "]";
        for (String value : new String[] {
                "1000\n1000007", choice(1000L, FIRST_INSTALL) + "\n", choice(1000L, FIRST_INSTALL) + "\r",
                choice(1000L, FIRST_INSTALL) + "\u2028", "1000\u20281000007", choice(1000L, FIRST_INSTALL) + "\u0085",
                choice(1000L, FIRST_INSTALL) + "\r\n" + forged, "x\n" + forged, "\n" + choice(1000L, FIRST_INSTALL),
                choice(1000L, FIRST_INSTALL) + "\n[" + key("org.example.zed") + "]: [" + choice(1L, FIRST_INSTALL) }) {
            String shown = value.replace("\n", "\\n").replace("\r", "\\r").replace("\u2028", "\\u2028").replace("\u0085", "\\u0085");
            Fake fake = new Fake();
            fake.installed.add(installedAt("org.example.multi", FIRST_INSTALL));
            fake.installed.add(installedAt("org.example.victim", FIRST_INSTALL));
            fake.properties.put(key("org.example.multi"), value);
            fake.properties.put(key("org.example.victim"), choice(7L, FIRST_INSTALL));
            Result result = run(fake, "list");
            same(0, result.status, "listing with the value <" + shown + ">");
            check(result.out.contains("org.example.multi\torg.example.multi\toff\t0\t50\n"),
                    "the package is listed off: <" + shown + ">: " + result.out);
            same("", fake.getProperty(key("org.example.multi")), "and its value is cleared: <" + shown + ">");
            check(result.out.contains("org.example.victim\torg.example.victim\ton\t7\t50\n"),
                    "a line inside the value does not touch another package: <" + shown + ">");
            same(choice(7L, FIRST_INSTALL), fake.getProperty(key("org.example.victim")), "its value is kept: <" + shown + ">");
            same(0, fake.writesOutsideTheLock, "every write is under the lock: <" + shown + ">");

            // Without the lock the value cannot be cleared, but it is listed off all the same.
            Fake locked = new Fake();
            locked.installed.add(installedAt("org.example.multi", FIRST_INSTALL));
            locked.properties.put(key("org.example.multi"), value);
            locked.lockTimesOut = true;
            check(run(locked, "list").out.contains("org.example.multi\torg.example.multi\toff\t0\t50\n"),
                    "listed off without the lock: <" + shown + ">");
            same(value, locked.getProperty(key("org.example.multi")), "and left alone: <" + shown + ">");
        }
        // The same package turned on again over a bad value writes a good one.
        Fake fake = new Fake();
        fake.installed.add(installedAt("org.example.multi", FIRST_INSTALL));
        fake.properties.put(key("org.example.multi"), "1000\n1000007");
        same(0, run(fake, "set", "org.example.multi", "on").status, "set on over a value with a line break");
        same(choice(1700000000L, FIRST_INSTALL), fake.getProperty(key("org.example.multi")), "writes a good value");
        fake.properties.put(key("org.example.multi"), "1000\n1000007");
        same(0, run(fake, "set", "org.example.multi", "off").status, "set off over a value with a line break");
        same("", fake.getProperty(key("org.example.multi")), "clears it");
    }

    private static Set<String> packagesOf(String output) throws IOException {
        return PropertyListing.storedPackages(new BufferedReader(new StringReader(output)));
    }

    /** What the property listing names, whatever the values hold. */
    private static void propertyListing() throws IOException {
        String p = OldAppEligibility.STORE_PREFIX;
        same(Collections.emptySet(), packagesOf(""), "nothing listed");
        same(new java.util.TreeSet<>(Arrays.asList("a.b", "c.d")),
                packagesOf("[ro.x]: [1]\n[" + p + "a.b]: [1 2]\n[" + p + "c.d]: []\n[sys.y]: [z]\n"),
                "names, with an empty value too");
        same(Collections.singleton("a.b"), packagesOf("[" + p + "a.b]: [1000\n1000007]\n"), "a value with an LF");
        same(Collections.singleton("a.b"), packagesOf("[" + p + "a.b]: [1000\r1000007]\n"), "a value with a CR");
        same(Collections.singleton("a.b"), packagesOf("[" + p + "a.b]: [1000\u20281000007]\n"), "a value with U+2028");
        same(Collections.singleton("a.b"), packagesOf("[" + p + "a.b]: [1000\u00851000007]\n"), "a value with U+0085");
        same(Collections.singleton("a.b"), packagesOf("[" + p + "a.b]: [1000\n\n\n]\n"), "blank lines in a value");
        // A value that looks like another property line names a package whose value reads as empty.
        Set<String> forged = packagesOf("[" + p + "a.b]: [1\n[" + p + "x.y]: [2 3]\n");
        check(forged.contains("a.b") && forged.contains("x.y") && forged.size() == 2, "a forged line only adds a name: " + forged);
        same(Collections.emptySet(), packagesOf("[" + p + "a..b]: [1]\n[" + p + ".a]: [1]\n[" + p + "a b]: [1]\n"
                + "[" + p + "]: [1]\n[" + p + "a]b]: [1]\n"), "names that cannot be keys are not named");
        same(Collections.emptySet(), packagesOf("[" + p.substring(1) + "a.b]: [1]\n[x" + p + "a.b]: [1]\n [" + p + "a.b]: [1]\n"
                + p + "a.b: 1\n[" + p + "a.b] [1]\n"), "other prefixes and other shapes are not named");
        same(Collections.singleton(letters(226)), packagesOf("[" + p + letters(226) + "]: [1]\n[" + p + letters(227) + "]: [1]\n"),
                "the 255-byte bound applies to what is named");
    }

    private static Set<PosixFilePermission> mode(String text) {
        return PosixFilePermissions.fromString(text);
    }

    private static Set<PosixFilePermission> modeOf(Path path) throws IOException {
        return Files.getPosixFilePermissions(path);
    }

    /** The lock file on real files: created owner-only, an unsafe one tightened or refused. */
    private static void lockFile() throws Exception {
        Path directory = Files.createTempDirectory("uml-oldapp-lock-");
        try {
            Path file = directory.resolve("kernarium_oldapp.lock");
            // Created owner-only, and kept in place after the release.
            OldAppCommand.Lock first = LockFile.acquire(file, 1000);
            check(first != null, "a new lock file can be locked");
            same(mode("rw-------"), modeOf(file), "the lock file is created owner-only");
            same(0L, Files.size(file), "and nothing is written to it");
            first.release();
            check(Files.exists(file), "the file is kept after the release");

            // An existing file with group or other permissions is tightened by its owner.
            for (String loose : new String[] {"rw-rw-rw-", "rw-r--r--", "rw-r-----", "rw----r--", "rwxrwxrwx", "rw-rw----"}) {
                Files.setPosixFilePermissions(file, mode(loose));
                OldAppCommand.Lock lock = LockFile.acquire(file, 1000);
                check(lock != null, "a file of mode " + loose + " can be locked once tightened");
                same(mode("rw-------"), modeOf(file), "a file of mode " + loose + " is tightened");
                if (lock != null) {
                    lock.release();
                }
            }
            // One that has no group or other permission is used as it is.
            Files.setPosixFilePermissions(file, mode("rwx------"));
            OldAppCommand.Lock owner = LockFile.acquire(file, 1000);
            check(owner != null, "an owner-only file can be locked");
            same(mode("rwx------"), modeOf(file), "and is not changed");
            owner.release();
            Files.setPosixFilePermissions(file, mode("rw-------"));

            // A directory is refused, and so is a missing directory above the file.
            Path folder = directory.resolve("folder.lock");
            Files.createDirectory(folder);
            expectRefusal(folder, "a directory at the path");
            expectRefusal(directory.resolve("missing").resolve("x.lock"), "no directory above the file");

            // A symbolic link is refused, whatever it leads to, and nothing it leads to is changed.
            Path target = directory.resolve("target");
            Files.write(target, "keep".getBytes(StandardCharsets.UTF_8));
            Files.setPosixFilePermissions(target, mode("rw-rw-rw-"));
            Path link = directory.resolve("link.lock");
            Files.createSymbolicLink(link, target);
            expectRefusal(link, "a link to a file");
            same(mode("rw-rw-rw-"), modeOf(target), "the file behind the link is not tightened");
            same("keep", new String(Files.readAllBytes(target), StandardCharsets.UTF_8), "nor written");
            Path dangling = directory.resolve("dangling.lock");
            Files.createSymbolicLink(dangling, directory.resolve("nothing"));
            expectRefusal(dangling, "a link to nothing");
            check(!Files.exists(directory.resolve("nothing")), "and the target is not created");

            // A pipe that the owner left there is refused (it is not a regular file) and does not block.
            Path pipe = directory.resolve("pipe.lock");
            if (makePipe(pipe)) {
                Files.setPosixFilePermissions(pipe, mode("rw-r--r--"));
                final Path pipeRef = pipe;
                final boolean[] refused = {false};
                Thread opener = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            OldAppCommand.Lock lock = LockFile.acquire(pipeRef, 200);
                            if (lock != null) {
                                lock.release();
                            }
                        } catch (IOException problem) {
                            refused[0] = true;
                        } catch (Exception problem) {
                            // Any other failure is not a refusal.
                        }
                    }
                });
                opener.setDaemon(true);
                opener.start();
                opener.join(10_000);
                check(!opener.isAlive(), "a pipe at the path does not block the command");
                check(refused[0], "a pipe at the path is refused");
                same(mode("rw-r--r--"), modeOf(pipe), "and the pipe is not tightened: it is refused before anything is changed");
            }

            // A file that is not a regular one is refused before anything is changed.
            Path device = Paths.get("/dev/null");
            if (Files.exists(device)) {
                Set<PosixFilePermission> before = modeOf(device);
                expectRefusal(device, "a device");
                same(before, modeOf(device), "and the device is not changed");
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static boolean makePipe(Path path) throws Exception {
        try {
            Process process = new ProcessBuilder("mkfifo", path.toString()).redirectErrorStream(true).start();
            process.getOutputStream().close();
            process.getInputStream().readAllBytes();
            return process.waitFor() == 0 && Files.exists(path);
        } catch (IOException missing) {
            return false;
        }
    }

    private static void expectRefusal(Path path, String what) throws Exception {
        try {
            OldAppCommand.Lock lock = LockFile.acquire(path, 200);
            check(false, what + " is refused, but the lock was " + (lock == null ? "not had" : "had"));
            if (lock != null) {
                lock.release();
            }
        } catch (IOException refused) {
            check(true, what);
        }
    }

    /** A held lock keeps the next one waiting for the bound, then it is not had; after the release it is. */
    private static void lockFileContention() throws Exception {
        Path directory = Files.createTempDirectory("uml-oldapp-lock-");
        try {
            Path file = directory.resolve("kernarium_oldapp.lock");
            OldAppCommand.Lock held = LockFile.acquire(file, 1000);
            check(held != null, "the first lock is had");
            long start = System.nanoTime();
            OldAppCommand.Lock waited = LockFile.acquire(file, 300);
            long millis = (System.nanoTime() - start) / 1_000_000L;
            check(waited == null, "a second lock is not had while the first is held");
            check(millis >= 250 && millis < 3000, "and it waited for the bound: " + millis + " ms");
            held.release();
            OldAppCommand.Lock after = LockFile.acquire(file, 1000);
            check(after != null, "the lock is had once released");
            if (after != null) {
                after.release();
            }
        } finally {
            deleteTree(directory);
        }
    }

    private static void deleteTree(Path directory) throws IOException {
        try (java.util.stream.Stream<Path> paths = Files.walk(directory)) {
            for (Path path : (Iterable<Path>) paths.sorted(Collections.reverseOrder())::iterator) {
                Files.deleteIfExists(path);
            }
        }
    }

    /** A lock that cannot be had: set fails and changes nothing, the listing lists without cleaning. */
    private static void lockUnavailable() {
        for (int how = 0; how < 2; how++) {
            Fake fake = fixture();
            fake.installed.add(installedAt("org.example.again", 500_003L));
            fake.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
            if (how == 0) {
                fake.lockTimesOut = true;
            } else {
                fake.lockFails = true;
            }
            for (String state : new String[] {"on", "off"}) {
                fake.events.clear();
                Result result = run(fake, "set", "org.example.zed", state);
                same(1, result.status, "set " + state + " without the lock (" + how + ")");
                same("", result.out, "no success line without the lock");
                check(result.err.length() > 0, "and the reason is said");
                same(0, fake.events.size(), "nothing is written or stopped without the lock");
            }
            Result listed = run(fake, "list");
            same(0, listed.status, "the listing succeeds without the lock (" + how + ")");
            same(0, fake.events.size(), "and cleans nothing");
            same(choice(1000L, 1_000_007L), fake.getProperty(key("org.example.again")), "the stale choice stays");
            // The same lines as with the lock.
            Fake clean = fixture();
            clean.installed.add(installedAt("org.example.again", 500_003L));
            clean.properties.put(key("org.example.again"), choice(1000L, 1_000_007L));
            same(run(clean, "list").out, listed.out, "the listing says nothing different on stdout (" + how + ")");
            check(listed.out.contains("org.example.again\torg.example.again\toff\t0\t500\n"), "the package is listed off");
        }
        // A usage error or a refusal needs no lock.
        Fake fake = fixture();
        fake.lockTimesOut = true;
        same(2, run(fake, "set", "org.example..oldapp", "off").status, "a refusal of the name needs no lock");
        same(64, run(fake, "set", "org.example.alpha").status, "usage needs no lock");
    }

    /** Turning off is accepted for any name that can be a property name. */
    private static void turningOff() {
        // Not installed, nothing stored: done, and no property is made for it.
        Fake fake = fixture();
        Result absent = run(fake, "set", "org.example.never", "off");
        same(0, absent.status, "off for a package that is not installed");
        same("org.example.never\toff\n", absent.out, "its line");
        same(0, fake.events.size(), "nothing is written or stopped");

        // Not installed, a choice stored: cleared, nothing to stop.
        fake.properties.put(key("org.example.gone"), choice(1700000000L, 10_000L));
        Result gone = run(fake, "set", "org.example.gone", "off");
        same(0, gone.status, "off for a package that is gone");
        same(Arrays.asList("set " + key("org.example.gone") + "="), fake.events, "only the property is cleared");

        // Installed and not eligible any more: cleared and stopped.
        fake.events.clear();
        fake.properties.put(key("org.example.modern"), choice(1700000000L, FIRST_INSTALL));
        Result modern = run(fake, "set", "org.example.modern", "off");
        same(0, modern.status, "off for an application that is not eligible");
        same("org.example.modern\toff\n", modern.out, "its line");
        same(Arrays.asList("set " + key("org.example.modern") + "=", "stop org.example.modern"), fake.events,
                "cleared, then stopped");

        // Installed and never switched on: stopped only.
        fake.events.clear();
        Result idle = run(fake, "set", "org.example.alpha", "off");
        same(0, idle.status, "off for an application that is off");
        same(Arrays.asList("stop org.example.alpha"), fake.events, "nothing to clear, so it is only stopped");

        // A write the system does not keep is a failure.
        Fake dropped = fixture();
        dropped.keepWrites = false;
        same(1, run(dropped, "set", "org.example.zed", "off").status, "a clearing the system did not keep");
        same(0, dropped.locksHeld, "and the lock is released");

        // The bound of the name: 226 characters are a legal key, 227 are not.
        Fake bound = fixture();
        same(0, run(bound, "set", letters(226), "off").status, "off at the bound");
        same(2, run(bound, "set", letters(227), "off").status, "off over the bound");
        same(2, run(bound, "set", letters(227), "on").status, "on over the bound");
        same(2, run(bound, "set", letters(100_000), "off").status, "a huge name");
    }

    /** The platform is made after the arguments are judged. */
    private static void factory() {
        final int[] made = {0};
        OldAppCommand.PlatformFactory broken = () -> {
            made[0]++;
            throw new IllegalStateException("the package manager is not running");
        };
        StringWriter err = new StringWriter();
        StringWriter out = new StringWriter();
        int status = OldAppCommand.run(new String[] {"set", "x"}, broken, new PrintWriter(out), new PrintWriter(err));
        same(64, status, "a usage error is 64 although there is no package manager");
        same(0, made[0], "and the platform was not made");
        status = OldAppCommand.run(new String[] {"list"}, broken, new PrintWriter(out), new PrintWriter(err));
        same(1, status, "a good command fails with 1 without the package manager");
        same(1, made[0], "the platform was asked for once");
        check(err.toString().contains("the package manager is not running"), "and the reason is said: " + err);
    }

    private static void oneLine() {
        same("a b c d e f g h i", OldAppCommand.oneLine("a\tb\nc\rd\u000Be\ff\u0085g h i"), "separators");
        same("plain text", OldAppCommand.oneLine("plain text"), "plain");
        same("", OldAppCommand.oneLine(""), "empty");
        same("日本語 ラベル", OldAppCommand.oneLine("日本語\tラベル"), "non-ASCII text is kept");
    }

    public static void main(String[] args) throws Exception {
        eligibility();
        installations();
        listing();
        switching();
        usage();
        names();
        lifecycle();
        locking();
        multilineValues();
        propertyListing();
        lockFile();
        lockFileContention();
        lockUnavailable();
        turningOff();
        factory();
        oneLine();
        if (failures != 0) {
            System.out.println(failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("PASS");
    }
}
