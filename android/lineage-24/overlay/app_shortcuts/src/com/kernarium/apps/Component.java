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

package com.kernarium.apps;

/**
 * A launcher activity's name: the package and the activity class, in a grammar narrow enough that
 * no name can be taken for an option, a path, a quote or anything else a command line or a record
 * gives a meaning to. The tool lists only names that match, and takes no other.
 *
 * <ul>
 *   <li>package: 1 to 255 bytes, at least two dot-separated segments, each {@code [A-Za-z][A-Za-z0-9_]*};
 *   <li>activity: 1 to 255 bytes, the fully qualified class name, dot-separated segments, each
 *       {@code [A-Za-z_$][A-Za-z0-9_$]*}, no empty segment;
 *   <li>token: {@code package/activity}, exactly one slash.
 * </ul>
 */
final class Component {
    static final int MAX_PACKAGE = 255;
    static final int MAX_ACTIVITY = 255;

    final String packageName;
    final String activity;

    private Component(String packageName, String activity) {
        this.packageName = packageName;
        this.activity = activity;
    }

    /** The component a token names, or null when the token is not exactly {@code package/activity}. */
    static Component parse(String token) {
        if (token == null) {
            return null;
        }
        int slash = token.indexOf('/');
        if (slash < 0 || slash != token.lastIndexOf('/')) {
            return null;
        }
        String packageName = token.substring(0, slash);
        String activity = token.substring(slash + 1);
        return of(packageName, activity);
    }

    /** The component of a package and an activity class, or null when either is outside the grammar. */
    static Component of(String packageName, String activity) {
        if (packageName == null || activity == null || !isPackageName(packageName) || !isActivityName(activity)) {
            return null;
        }
        return new Component(packageName, activity);
    }

    static boolean isPackageName(String name) {
        if (name.length() < 1 || name.length() > MAX_PACKAGE) {
            return false;
        }
        int segments = 0;
        int start = 0;
        while (start <= name.length()) {
            int dot = name.indexOf('.', start);
            int end = dot < 0 ? name.length() : dot;
            if (end == start || !isLetter(name.charAt(start))) {
                return false;
            }
            for (int i = start + 1; i < end; i++) {
                char c = name.charAt(i);
                if (!isLetter(c) && !isDigit(c) && c != '_') {
                    return false;
                }
            }
            segments++;
            if (dot < 0) {
                break;
            }
            start = dot + 1;
        }
        return segments >= 2;
    }

    static boolean isActivityName(String name) {
        if (name.length() < 1 || name.length() > MAX_ACTIVITY) {
            return false;
        }
        int start = 0;
        while (start <= name.length()) {
            int dot = name.indexOf('.', start);
            int end = dot < 0 ? name.length() : dot;
            if (end == start) {
                return false;
            }
            for (int i = start; i < end; i++) {
                char c = name.charAt(i);
                boolean first = i == start;
                if (!isLetter(c) && c != '_' && c != '$' && (first || !isDigit(c))) {
                    return false;
                }
            }
            if (dot < 0) {
                break;
            }
            start = dot + 1;
        }
        return true;
    }

    String token() {
        return packageName + "/" + activity;
    }

    @Override
    public boolean equals(Object other) {
        return other instanceof Component
                && ((Component) other).packageName.equals(packageName)
                && ((Component) other).activity.equals(activity);
    }

    @Override
    public int hashCode() {
        return packageName.hashCode() * 31 + activity.hashCode();
    }

    @Override
    public String toString() {
        return token();
    }

    private static boolean isLetter(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    private static boolean isDigit(char c) {
        return c >= '0' && c <= '9';
    }
}
