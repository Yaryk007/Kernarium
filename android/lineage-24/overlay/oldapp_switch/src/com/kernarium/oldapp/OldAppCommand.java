// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.kernarium.oldapp;

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The commands of {@code kernarium_oldapp}: lists the applications Old app compatibility can apply
 * to and turns it on or off for one. {@link Main} runs them on Android, as the adb shell user (see
 * the wrapper script of the same name); the choice is the property
 * {@link OldAppEligibility#propertyName}.
 *
 * <pre>
 * kernarium_oldapp list                       kernarium_oldapp 1, then one line per eligible application:
 *                                             package TAB label TAB on|off TAB turned-on TAB last-update
 * kernarium_oldapp set PACKAGE on|off         PACKAGE TAB on|off; exit 2 when PACKAGE cannot be switched on
 * </pre>
 *
 * The listing covers the owner user's applications. It also clears every stored choice that
 * {@link OldAppEligibility#outlived} says is no longer its application's, so a choice does not
 * outlive the installation it was made for by more than the time to the next listing. Turning
 * off is accepted for any name that can be a property name, installed or not. {@code set} and
 * that cleanup are serialised across processes by {@link Platform#lock}. The listing's snapshot
 * only says which choices look stale; inside the lock the cleanup reads the installation and the
 * value again and clears a value only when it is not its installation's now, so it never erases a
 * choice made after the snapshot, whether for a package that was not there or for a replacement.
 * A {@code set} that cannot take the lock in time fails and changes nothing, and a listing that
 * cannot lists the same lines without cleaning.
 *
 * <p>Exit status: 0 done, 1 any other failure, 2 refused (not eligible), 64 usage. The format grows
 * by trailing fields; {@link #FORMAT_VERSION} changes when a field's meaning does.
 */
final class OldAppCommand {
    static final int FORMAT_VERSION = 1;
    static final String NAME = "kernarium_oldapp";
    static final int EXIT_OK = 0;
    static final int EXIT_FAILURE = 1;
    static final int EXIT_REFUSED = 2;
    static final int EXIT_USAGE = 64;
    /** How long a command waits for the lock before it gives up. */
    static final long LOCK_WAIT_MILLIS = 5000;

    /** The platform the commands stand on; the Android one is {@link AndroidPlatform}. */
    interface Platform {
        /** The installed packages of the current user. */
        List<Candidate> installedPackages() throws Exception;

        /** One installed package, or null. */
        Candidate find(String packageName) throws Exception;

        /** The device's 32-bit ABIs. */
        List<String> abis32();

        /** The application's label in the system's current language. */
        String label(Candidate candidate) throws Exception;

        /** The system property's value, or the empty string. */
        String getProperty(String name);

        /** Sets a system property, and throws if the system did not take it. */
        void setProperty(String name, String value) throws Exception;

        /**
         * The package part (the end of the property name) of every property under the store's
         * prefix, whatever its value; the value of each is read with {@link #getProperty}.
         */
        Collection<String> storedPackages() throws Exception;

        /**
         * Takes the exclusive lock that serialises the commands' changes across processes, waiting
         * at most the given time. Returns null when it could not be had in time.
         */
        Lock lock(long timeoutMillis) throws Exception;

        /** Stops the application's processes, as {@code am force-stop} does. */
        void forceStop(String packageName) throws Exception;

        /** The system's clock in epoch seconds. */
        long nowSeconds();
    }

    /** A held lock. */
    interface Lock {
        void release() throws Exception;
    }

    /** Makes the platform once the arguments are known to be right. */
    interface PlatformFactory {
        Platform create() throws Exception;
    }

    private OldAppCommand() {}

    /** Whether the arguments are a command: the verb, the count and on|off, nothing about the package. */
    static boolean isUsage(String[] args) {
        return (args.length == 1 && args[0].equals("list"))
                || (args.length == 3 && args[0].equals("set") && (args[2].equals("on") || args[2].equals("off")));
    }

    static int run(String[] args, Platform platform, PrintWriter out, PrintWriter err) {
        return run(args, () -> platform, out, err);
    }

    /** The arguments are judged first: a usage error is 64 even when the platform cannot be made. */
    static int run(String[] args, PlatformFactory factory, PrintWriter out, PrintWriter err) {
        if (!isUsage(args)) {
            err.print("usage: " + NAME + " list\n       " + NAME + " set PACKAGE on|off\n");
            return EXIT_USAGE;
        }
        Platform platform;
        try {
            platform = factory.create();
        } catch (Exception problem) {
            err.print(NAME + ": " + problem + "\n");
            return EXIT_FAILURE;
        }
        return args[0].equals("list") ? list(platform, out, err) : set(platform, args[1], args[2].equals("on"), out, err);
    }

    private static int list(Platform platform, PrintWriter out, PrintWriter err) {
        try {
            List<String> abis32 = platform.abis32();
            Map<String, Candidate> installed = new HashMap<>();
            for (Candidate candidate : platform.installedPackages()) {
                installed.put(candidate.packageName, candidate);
            }
            Map<String, String> choices = new HashMap<>();
            for (String packageName : platform.storedPackages()) {
                if (OldAppEligibility.isPropertyKeyPart(packageName)) {
                    String value = platform.getProperty(OldAppEligibility.propertyName(packageName));
                    if (!value.isEmpty()) {
                        choices.put(packageName, value);
                    }
                }
            }
            clearStale(platform, abis32, installed, choices);
            List<Candidate> eligible = new ArrayList<>();
            for (Candidate candidate : installed.values()) {
                if (OldAppEligibility.isEligible(candidate, abis32)) {
                    eligible.add(candidate);
                }
            }
            Collections.sort(eligible, new Comparator<Candidate>() {
                @Override
                public int compare(Candidate a, Candidate b) {
                    return a.packageName.compareTo(b.packageName);
                }
            });
            StringBuilder text = new StringBuilder();
            text.append(NAME).append(' ').append(FORMAT_VERSION).append('\n');
            for (Candidate candidate : eligible) {
                OldAppEligibility.Choice choice = null;
                String value = choices.get(candidate.packageName);
                if (value != null) {
                    choice = OldAppEligibility.parseChoice(value);
                }
                text.append(candidate.packageName).append('\t')
                        .append(oneLine(label(platform, candidate))).append('\t')
                        .append(choice != null ? "on" : "off").append('\t')
                        .append(choice != null ? choice.turnedOnSeconds : 0).append('\t')
                        .append(candidate.lastUpdateSeconds).append('\n');
            }
            out.print(text);
            return EXIT_OK;
        } catch (Exception problem) {
            err.print(NAME + ": list failed: " + problem + "\n");
            return EXIT_FAILURE;
        }
    }

    /**
     * Clears the choices that are not their application's and takes them out of {@code choices},
     * which is what the listing then shows. The snapshot (the installed packages and the values
     * read after it) only says which choices look stale. Inside the lock each of those is judged
     * again on what is true then: the value as it is now against the installation as it is now,
     * so a choice made after the snapshot, for a package that was not there or for a replacement,
     * is kept, and one that has gone stale since is cleared. Without the lock nothing is cleared
     * and the listing is the same.
     */
    private static void clearStale(Platform platform, List<String> abis32, Map<String, Candidate> installed,
            Map<String, String> choices) throws Exception {
        List<String> suspects = new ArrayList<>();
        for (Map.Entry<String, String> choice : choices.entrySet()) {
            if (OldAppEligibility.outlived(installed.get(choice.getKey()), abis32, choice.getValue())) {
                suspects.add(choice.getKey());
            }
        }
        if (suspects.isEmpty()) {
            return;
        }
        Lock lock = null;
        try {
            lock = platform.lock(LOCK_WAIT_MILLIS);
        } catch (Exception problem) {
            // No lock, no cleaning: the listing below is the same.
        }
        if (lock == null) {
            choices.keySet().removeAll(suspects);
            return;
        }
        try {
            for (String packageName : suspects) {
                String name = OldAppEligibility.propertyName(packageName);
                String current = platform.getProperty(name);
                if (current.isEmpty()) {
                    choices.remove(packageName);
                } else if (OldAppEligibility.outlived(platform.find(packageName), abis32, current)) {
                    write(platform, name, "");
                    choices.remove(packageName);
                } else {
                    choices.put(packageName, current);
                }
            }
        } finally {
            lock.release();
        }
    }

    private static int set(Platform platform, String packageName, boolean on, PrintWriter out,
            PrintWriter err) {
        try {
            // Turning off is accepted for any name that can be a property name, so that a choice that
            // no rule would let be turned on can still be turned off; only that name is judged.
            if (!OldAppEligibility.isPropertyKeyPart(packageName)) {
                return refuse(err, packageName);
            }
            Candidate candidate;
            Lock lock = platform.lock(LOCK_WAIT_MILLIS);
            if (lock == null) {
                err.print(NAME + ": could not take the lock in " + LOCK_WAIT_MILLIS + " ms; nothing was changed\n");
                return EXIT_FAILURE;
            }
            try {
                candidate = platform.find(packageName);
                if (on && (candidate == null || !OldAppEligibility.isEligible(candidate, platform.abis32()))) {
                    return refuse(err, packageName);
                }
                if (on) {
                    write(platform, OldAppEligibility.propertyName(packageName),
                            OldAppEligibility.choiceValue(platform.nowSeconds(), candidate));
                } else {
                    clear(platform, packageName);
                }
            } finally {
                lock.release();
            }
            // After the write: an application that starts again after it is stopped finds the new value.
            if (candidate != null) {
                try {
                    platform.forceStop(packageName);
                } catch (Exception problem) {
                    err.print(NAME + ": " + packageName + " is " + (on ? "on" : "off")
                            + " but could not be stopped: " + problem + "\n");
                    return EXIT_FAILURE;
                }
            }
            out.print(packageName + "\t" + (on ? "on" : "off") + "\n");
            return EXIT_OK;
        } catch (Exception problem) {
            err.print(NAME + ": set failed: " + problem + "\n");
            return EXIT_FAILURE;
        }
    }

    private static int refuse(PrintWriter err, String packageName) {
        err.print(NAME + ": " + packageName + " is not an application Old app compatibility applies to\n");
        return EXIT_REFUSED;
    }

    /** Clears a choice; one that is not stored is left as it is, so that no property is made for it. */
    private static void clear(Platform platform, String packageName) throws Exception {
        String name = OldAppEligibility.propertyName(packageName);
        if (!platform.getProperty(name).isEmpty()) {
            write(platform, name, "");
        }
    }

    private static void write(Platform platform, String name, String value) throws Exception {
        platform.setProperty(name, value);
        if (!platform.getProperty(name).equals(value)) {
            throw new IllegalStateException("the system did not keep " + name);
        }
    }

    private static String label(Platform platform, Candidate candidate) {
        String label = null;
        try {
            label = platform.label(candidate);
        } catch (Exception problem) {
            // An application whose label cannot be read is listed under its package name.
        }
        return label == null || label.isEmpty() ? candidate.packageName : label;
    }

    /** A TAB or a line break becomes a space, so the text stays inside its field. */
    static String oneLine(String text) {
        StringBuilder result = new StringBuilder(text.length());
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            boolean separator = c == '\t' || c == '\n' || c == '\r' || c == '\u000B' || c == '\f'
                    || c == '\u0085' || c == ' ' || c == ' ';
            result.append(separator ? ' ' : c);
        }
        return result.toString();
    }
}
