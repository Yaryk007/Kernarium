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

import java.util.Locale;

/** Reads what {@code cmd activity start-activity} printed and how it ended. */
final class StartOutcome {
    /** The longest reason the tool prints. */
    static final int MAX_REASON = 200;

    /** The kinds of failure, worst first: the reasons the tool prints are these fixed texts and never the command's own. */
    private static final String DENIED = "permission denied";
    private static final String MISSING = "no such activity";
    private static final String FAILED = "start failed";

    private StartOutcome() {}

    /**
     * Why the start did not happen, as one printable ASCII line of fixed wording, or null when it did. The
     * shell command exits 0 for some refusals and prints an {@code Error} line, so the output is read as
     * well as the status. A line counts only by how it begins: the command echoes the intent it starts
     * ({@code Starting: Intent { ... cmp=PACKAGE/CLASS }}), and a launcher whose class is named like an
     * error must not turn a start into a failure. A task that was only brought to the front is a start:
     * that is what a tap on an app that is already open does.
     */
    static String failure(int exit, String output) {
        String kind = null;
        for (String line : output.split("\n")) {
            kind = worse(kind, classify(line.trim()));
        }
        if (exit == 0 && kind == null) {
            return null;
        }
        String reason = kind == null ? "exited " + exit : kind + (exit == 0 ? "" : " (exit " + exit + ")");
        return ascii("cmd activity start-activity: " + reason, MAX_REASON);
    }

    private static String worse(String current, String found) {
        if (found == null || DENIED.equals(current) || (MISSING.equals(current) && !DENIED.equals(found))) {
            return current;
        }
        return found;
    }

    /** What a line of the command's output says went wrong (judged by its start), or null. */
    private static String classify(String line) {
        boolean error = line.startsWith("Error");
        if (line.startsWith("Permission Denial") || (error && line.toLowerCase(Locale.ROOT).contains("permission"))) {
            return DENIED;
        }
        if (error && (line.startsWith("Error type 3") || line.contains("does not exist"))) {
            return MISSING;
        }
        if (error || line.startsWith("Exception")) {
            return FAILED;
        }
        // A stack trace's first line: the class of the exception, a colon, its message.
        int colon = line.indexOf(':');
        String head = colon < 0 ? line : line.substring(0, colon);
        if (isThrowable(head)) {
            return head.endsWith("SecurityException") ? DENIED : FAILED;
        }
        return null;
    }

    /** A qualified class name that ends in Exception or Error, such as {@code java.lang.SecurityException}. */
    private static boolean isThrowable(String head) {
        if (!(head.endsWith("Exception") || head.endsWith("Error")) || head.indexOf('.') < 0) {
            return false;
        }
        for (int i = 0; i < head.length(); i++) {
            char c = head.charAt(i);
            boolean part = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
                    || c == '$' || c == '.';
            if (!part) {
                return false;
            }
        }
        return head.charAt(0) != '.' && !head.endsWith(".");
    }

    /** Printable ASCII only, one line, at most {@code limit} characters. */
    static String ascii(String text, int limit) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length() && out.length() < limit; i++) {
            char c = text.charAt(i);
            out.append(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return out.toString();
    }
}
