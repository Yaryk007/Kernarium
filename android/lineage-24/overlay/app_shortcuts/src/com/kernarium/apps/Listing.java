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
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The listing's rule: which launcher activities are offered, in what order, within what bounds,
 * and the digest and icon keys that say when what the host cached is out of date. Nothing here
 * opens an application's resources, so the digest is as cheap as the package manager's answer.
 */
final class Listing {
    /** The most launcher activities listed for one package. */
    static final int MAX_PER_PACKAGE = 8;
    /** The most rows a listing carries. */
    static final int MAX_ROWS = 1024;
    /** Changes when the way an icon is rendered does, so that the host fetches every icon again. */
    static final int RENDER_VERSION = 3;

    final int user;
    final String locales;
    /** The rows, in order. */
    final List<Entry> rows;
    /** Launcher activities that could be launched and are left out by a bound or by the grammar. */
    final int skipped;
    final String digest;

    private Listing(int user, String locales, List<Entry> rows, int skipped, String digest) {
        this.user = user;
        this.locales = locales;
        this.rows = rows;
        this.skipped = skipped;
        this.digest = digest;
    }

    /** The listing of what the platform reports; {@code localeTags} is the system's locale list. */
    static Listing of(int user, List<String> localeTags, List<Entry> reported) {
        List<Entry> eligible = new ArrayList<>();
        int skipped = 0;
        Set<String> seen = new HashSet<>();
        for (Entry entry : reported) {
            if (!entry.launchable()) {
                continue;
            }
            Component component = Component.of(entry.packageName, entry.activity);
            if (component == null) {
                skipped++;
                continue;
            }
            if (seen.add(component.token())) {
                eligible.add(entry);
            }
        }
        Collections.sort(eligible, ORDER);
        List<Entry> rows = new ArrayList<>();
        Map<String, Integer> perPackage = new HashMap<>();
        for (Entry entry : eligible) {
            Integer count = perPackage.get(entry.packageName);
            int have = count == null ? 0 : count;
            if (have >= MAX_PER_PACKAGE || rows.size() >= MAX_ROWS) {
                skipped++;
                continue;
            }
            perPackage.put(entry.packageName, have + 1);
            rows.add(entry);
        }
        String locales = localeLine(localeTags);
        return new Listing(user, locales, rows, skipped, digest(user, locales, restOfLocales(localeTags, locales), rows));
    }

    static final Comparator<Entry> ORDER = new Comparator<Entry>() {
        @Override
        public int compare(Entry a, Entry b) {
            int byPackage = a.packageName.compareTo(b.packageName);
            return byPackage != 0 ? byPackage : a.activity.compareTo(b.activity);
        }
    };

    /** The locale tags the header carries: at most 5, each of letters, digits and hyphens, at most 128 bytes together. */
    static String localeLine(List<String> tags) {
        StringBuilder out = new StringBuilder();
        int taken = 0;
        for (String tag : tags) {
            if (taken == 5) {
                break;
            }
            if (tag == null || tag.isEmpty() || !isTag(tag)) {
                continue;
            }
            if (out.length() + (out.length() == 0 ? 0 : 1) + tag.length() > 128) {
                break;
            }
            if (out.length() > 0) {
                out.append(',');
            }
            out.append(tag);
            taken++;
        }
        return out.toString();
    }

    /**
     * What the header's locale line leaves out of the system's whole locale list (more than five tags, a tag
     * over the budget, a tag of other characters), as a hash; empty when the line says it all. The labels
     * follow the whole list, so the digest does too.
     */
    static String restOfLocales(List<String> tags, String line) {
        StringBuilder all = new StringBuilder();
        for (String tag : tags) {
            if (all.length() > 0) {
                all.append(',');
            }
            all.append(tag);
        }
        return all.toString().equals(line) ? "" : sha256Hex(all.toString().getBytes(StandardCharsets.UTF_8));
    }

    private static boolean isTag(String tag) {
        for (int i = 0; i < tag.length(); i++) {
            char c = tag.charAt(i);
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
                return false;
            }
        }
        return true;
    }

    /**
     * The digest of the listing without its labels: the rows' identities, built-in flags and icon
     * keys, the locale line and the user, so that a language change is a change too.
     */
    static String digest(int user, String locales, String restOfLocales, List<Entry> rows) {
        StringBuilder text = new StringBuilder("kernarium_apps-digest-1\n");
        for (Entry entry : rows) {
            text.append(entry.packageName).append('\t').append(entry.activity).append('\t')
                    .append(entry.builtin ? '1' : '0').append('\t').append(iconKey(entry)).append('\n');
        }
        text.append("locales\t").append(locales).append('\n');
        if (!restOfLocales.isEmpty()) {
            text.append("localesall\t").append(restOfLocales).append('\n');
        }
        text.append("user\t").append(user).append('\n');
        return sha256Hex(text.toString().getBytes(StandardCharsets.UTF_8));
    }

    /**
     * 16 hex characters that change when the app's icon may: its version, update time, the image's build time (for
     * a built-in application that was never updated), icon resource, the files its resources come from and the
     * renderer. The icon is drawn for fixed inputs (density, night mode, locale),
     * so one key is one picture.
     */
    static String iconKey(Entry entry) {
        String text = "kernarium_apps-icon-" + RENDER_VERSION + '\0' + entry.packageName + '\0' + entry.activity
                + '\0' + entry.versionCode + '\0' + entry.lastUpdateMillis + '\0' + entry.imageBuildMillis + '\0'
                + entry.iconResource + '\0'
                + (entry.builtin ? '1' : '0') + '\0' + entry.resources;
        return sha256Hex(text.getBytes(StandardCharsets.UTF_8)).substring(0, 16);
    }

    static String sha256Hex(byte[] bytes) {
        try {
            byte[] hash = MessageDigest.getInstance("SHA-256").digest(bytes);
            StringBuilder hex = new StringBuilder(hash.length * 2);
            for (byte b : hash) {
                hex.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
            }
            return hex.toString();
        } catch (NoSuchAlgorithmException problem) {
            throw new IllegalStateException(problem);
        }
    }
}
