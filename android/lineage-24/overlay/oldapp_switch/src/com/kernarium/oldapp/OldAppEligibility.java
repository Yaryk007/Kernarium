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

import java.util.Collection;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The one place that says which applications Old app compatibility applies to, where the choice
 * is stored, what the stored value is and when a stored choice no longer belongs to the
 * application it was made for.
 *
 * <p>An application is eligible when it is not a system application, its primary ABI is a 32-bit
 * one, it targets an API level below 14 (Android before 4.0), it does not share a user ID and
 * its package name can be the end of a property name as it is. The framework hook
 * (frameworks/base, ActivityThread.handleBindApplication) applies the same rule to the
 * application it is starting, in the order that keeps the common case cheap: the target level,
 * then the process's bitness, then whether the property of {@link #propertyName} is not empty. It
 * does not need the other tests, which the command applies before it lets a property stand.
 *
 * <p>The stored value names the installation the choice was made for: the turn-on time in epoch
 * seconds, a space, and the installation's first-install time in epoch milliseconds. A choice
 * belongs to the installation whose first-install time equals the stored one (an update keeps it,
 * an uninstall and a new install make another), and no time is compared with another for order:
 * {@link #outlived} says when a choice is not its application's, and the listing clears it.
 */
final class OldAppEligibility {
    /** Android 4.0, Build.VERSION_CODES.ICE_CREAM_SANDWICH: a lower target level is "old". */
    static final int TARGET_LEVEL_BELOW = 14;

    /** The store: one persistent property per package; see {@link #choiceValue} for its value. */
    static final String STORE_PREFIX = "persist.sys.kernarium.oldapp.";

    /** The longest property key the rule accepts, in bytes (the names are ASCII). */
    static final int MAX_KEY_BYTES = 255;

    /**
     * What the property service accepts after the prefix (init's IsLegalPropertyName): letters,
     * digits and _ @ : -, in segments that no dot leaves empty, so no leading, trailing or
     * doubled dot. The package manager accepts a few names that break it (org.example..app).
     */
    private static final Pattern PROPERTY_KEY_PART =
            Pattern.compile("[A-Za-z0-9_@:-]+(\\.[A-Za-z0-9_@:-]+)*");

    /** The stored value: turn-on seconds, one space, first-install milliseconds. */
    private static final Pattern CHOICE = Pattern.compile("([0-9]{1,18}) ([0-9]{1,18})");

    private OldAppEligibility() {}

    /** A stored choice, read back. */
    static final class Choice {
        final long turnedOnSeconds;
        final long installationMillis;

        Choice(long turnedOnSeconds, long installationMillis) {
            this.turnedOnSeconds = turnedOnSeconds;
            this.installationMillis = installationMillis;
        }
    }

    /** @param abis32 the 32-bit ABIs of the device (Build.SUPPORTED_32_BIT_ABIS). */
    static boolean isEligible(Candidate candidate, Collection<String> abis32) {
        return !candidate.system
                && candidate.sharedUserId == null
                && candidate.targetSdkVersion < TARGET_LEVEL_BELOW
                && candidate.primaryCpuAbi != null
                && abis32.contains(candidate.primaryCpuAbi)
                && isPropertyKeyPart(candidate.packageName);
    }

    /**
     * Whether the text can follow {@link #STORE_PREFIX} in a property name as it is, and the
     * whole key stays within {@link #MAX_KEY_BYTES} (the length is judged first, so a huge
     * argument never reaches the pattern).
     */
    static boolean isPropertyKeyPart(String text) {
        return text.length() <= MAX_KEY_BYTES - STORE_PREFIX.length()
                && PROPERTY_KEY_PART.matcher(text).matches();
    }

    static String propertyName(String packageName) {
        return STORE_PREFIX + packageName;
    }

    /** The value that turns an installation on at a time. */
    static String choiceValue(long turnedOnSeconds, Candidate installation) {
        return turnedOnSeconds + " " + installation.firstInstallMillis;
    }

    /** The stored value read back, or null when it is not exactly two decimal numbers and a space. */
    static Choice parseChoice(String value) {
        Matcher match = CHOICE.matcher(value);
        if (!match.matches()) {
            return null;
        }
        return new Choice(Long.parseLong(match.group(1)), Long.parseLong(match.group(2)));
    }

    /**
     * Whether a stored choice no longer belongs to the application it was made for: its package
     * is gone, is not eligible any more (an update that raised the target level, a reinstall with
     * a shared user ID), or is another installation than the choice names. A value that does not
     * name an installation (the earlier one-number form, or anything else) cannot be told from a
     * stale one and counts as outlived.
     *
     * @param candidate the installed package, or null when it is not installed.
     * @param value the stored value.
     */
    static boolean outlived(Candidate candidate, Collection<String> abis32, String value) {
        if (candidate == null || !isEligible(candidate, abis32)) {
            return true;
        }
        Choice choice = parseChoice(value);
        return choice == null || choice.installationMillis != candidate.firstInstallMillis;
    }
}
