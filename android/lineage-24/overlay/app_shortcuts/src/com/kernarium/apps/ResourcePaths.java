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

import java.util.ArrayList;
import java.util.List;

/**
 * The APK files an application's resources are read from, in the order the framework's own resource
 * loading adds them (LoadedApk and ResourcesManager): the base package, its splits, its shared libraries
 * (only those that are APKs) and its runtime overlays (the overlay paths in their order, then the legacy
 * resource directories that are not among them, each once: ResourcesManager's order, which is the overlays'
 * priority). The platform gives the facts as {@code ApplicationInfo} holds them for the user.
 */
final class ResourcePaths {
    enum Kind {
        BASE, SPLIT, LIBRARY, OVERLAY
    }

    /** One file and how it is added. */
    static final class Item {
        final Kind kind;
        final String path;

        Item(Kind kind, String path) {
            this.kind = kind;
            this.path = path;
        }
    }

    private ResourcePaths() {}

    /**
     * The files, base first. The base is the public source directory (the one a process of another uid reads),
     * else the source directory; the splits are the public ones, else the source ones. A list that does not
     * start with the base means the application's resources cannot be opened.
     */
    static List<Item> of(String publicSourceDir, String sourceDir, String[] splitPublicSourceDirs,
            String[] splitSourceDirs, String[] sharedLibraryFiles, String[] resourceDirs, String[] overlayPaths) {
        List<Item> items = new ArrayList<>();
        String base = present(publicSourceDir) ? publicSourceDir : sourceDir;
        if (present(base)) {
            items.add(new Item(Kind.BASE, base));
        }
        String[] splits = splitPublicSourceDirs != null ? splitPublicSourceDirs : splitSourceDirs;
        if (splits != null) {
            for (String split : splits) {
                if (present(split)) {
                    items.add(new Item(Kind.SPLIT, split));
                }
            }
        }
        if (sharedLibraryFiles != null) {
            for (String library : sharedLibraryFiles) {
                if (present(library) && library.endsWith(".apk")) {
                    items.add(new Item(Kind.LIBRARY, library));
                }
            }
        }
        List<String> overlays = new ArrayList<>();
        for (String[] group : new String[][] {overlayPaths, resourceDirs}) {
            if (group != null) {
                for (String overlay : group) {
                    if (present(overlay) && !overlays.contains(overlay)) {
                        overlays.add(overlay);
                    }
                }
            }
        }
        for (String overlay : overlays) {
            items.add(new Item(Kind.OVERLAY, overlay));
        }
        return items;
    }

    /** Whether the list can be opened at all: it starts with the base. */
    static boolean hasBase(List<Item> items) {
        return !items.isEmpty() && items.get(0).kind == Kind.BASE;
    }

    /** A fingerprint of the files (kind and path): it changes when a split, library or overlay comes or goes. */
    static String fingerprint(List<Item> items) {
        StringBuilder text = new StringBuilder("kernarium_apps-resources-1\n");
        for (Item item : items) {
            text.append(item.kind.name()).append('\t').append(item.path).append('\n');
        }
        return Listing.sha256Hex(text.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8)).substring(0, 16);
    }

    private static boolean present(String path) {
        return path != null && !path.isEmpty();
    }
}
