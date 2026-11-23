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

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * The text of a label as the listing carries it. A label is an application's own text, so it is
 * made safe here and not trusted anywhere after: nothing in it can end a line or a field, reorder
 * the text around it, or hide in it.
 *
 * <ul>
 *   <li>control characters (C0, DEL, C1), line and paragraph separators and every space character
 *       become one space, and runs of spaces collapse;
 *   <li>bidirectional controls (U+061C, U+200E, U+200F, U+202A to U+202E, U+2066 to U+2069), the other
 *       invisible format characters (category Cf) except U+200C and U+200D (which spell words in
 *       some scripts and emoji), and byte order marks are dropped;
 *   <li>an unpaired surrogate or a non-character becomes U+FFFD;
 *   <li>the ends are trimmed, and the text is cut to {@link #MAX_CODE_POINTS} code points and
 *       {@link #MAX_BYTES} UTF-8 bytes, ending in an ellipsis where it was cut.
 * </ul>
 */
final class LabelText {
    static final int MAX_CODE_POINTS = 80;
    static final int MAX_BYTES = 320;
    static final int ELLIPSIS = 0x2026;
    /** UTF-16 units of cleaned text past which cut() cuts whatever follows (a label can be megabytes). */
    static final int CLEAN_LIMIT = 2 * MAX_BYTES;

    private LabelText() {}

    /** The safe label for {@code raw}; {@code fallback} (cut the same way) when nothing is left. */
    static String sanitize(String raw, String fallback) {
        String text = raw == null ? "" : clean(raw);
        if (text.isEmpty()) {
            text = clean(fallback == null ? "" : fallback);
        }
        return cut(text);
    }

    private static String clean(String raw) {
        StringBuilder out = new StringBuilder(Math.min(raw.length(), CLEAN_LIMIT + 2));
        boolean pendingSpace = false;
        int i = 0;
        while (i < raw.length()) {
            if (out.length() > CLEAN_LIMIT) {
                break; // past both bounds already: cut() keeps less than this, so the rest cannot change the label
            }
            int cp = raw.codePointAt(i);
            i += Character.charCount(cp);
            int replacement = classify(cp);
            if (replacement == DROP) {
                continue;
            }
            if (replacement == SPACE) {
                pendingSpace = out.length() > 0;
                continue;
            }
            if (pendingSpace) {
                out.append(' ');
                pendingSpace = false;
            }
            out.appendCodePoint(replacement);
        }
        return out.toString();
    }

    private static final int DROP = -1;
    private static final int SPACE = -2;

    /** What a code point becomes: itself, {@link #SPACE}, {@link #DROP} or U+FFFD. */
    private static int classify(int cp) {
        int type = Character.getType(cp);
        if (type == Character.SURROGATE || (cp & 0xFFFE) == 0xFFFE || (cp >= 0xFDD0 && cp <= 0xFDEF)) {
            return 0xFFFD;
        }
        if (type == Character.CONTROL || type == Character.LINE_SEPARATOR
                || type == Character.PARAGRAPH_SEPARATOR || type == Character.SPACE_SEPARATOR
                || cp == 0x85 || cp == 0x1680 || Character.isWhitespace(cp)) {
            return SPACE;
        }
        if (cp == 0x200C || cp == 0x200D) {
            return cp;
        }
        if (type == Character.FORMAT || cp == 0x061C || cp == 0x200E || cp == 0x200F
                || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF) {
            return DROP;
        }
        return cp;
    }

    private static String cut(String text) {
        List<Integer> points = new ArrayList<>();
        int bytes = 0;
        for (int i = 0; i < text.length(); ) {
            int cp = text.codePointAt(i);
            i += Character.charCount(cp);
            points.add(cp);
            bytes += new String(Character.toChars(cp)).getBytes(StandardCharsets.UTF_8).length;
        }
        if (points.size() <= MAX_CODE_POINTS && bytes <= MAX_BYTES) {
            return text;
        }
        // Room for the ellipsis (one code point, three bytes) is left in both bounds.
        StringBuilder out = new StringBuilder();
        int count = 0;
        int used = 0;
        for (int cp : points) {
            int width = new String(Character.toChars(cp)).getBytes(StandardCharsets.UTF_8).length;
            if (count + 1 > MAX_CODE_POINTS - 1 || used + width > MAX_BYTES - 3) {
                break;
            }
            out.appendCodePoint(cp);
            count++;
            used += width;
        }
        int end = out.length();
        while (end > 0 && out.charAt(end - 1) == ' ') {
            end--;
        }
        out.setLength(end);
        out.appendCodePoint(ELLIPSIS);
        return out.toString();
    }
}
