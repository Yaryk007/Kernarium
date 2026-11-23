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
 * One launcher activity as the platform reports it, with the facts the rules judge. {@code source}
 * is the platform's own object for it (the framework's {@code ActivityInfo}), which only the
 * platform looks into.
 */
final class Entry {
    final String packageName;
    final String activity;
    /** The application is part of the image (including a built-in application updated since). */
    final boolean builtin;
    final boolean exported;
    final boolean enabled;
    /** The activity asks for no permission, or the shell user has the one it asks for. */
    final boolean permitted;
    final long versionCode;
    final long lastUpdateMillis;
    /**
     * The image's build time for a built-in application that was never updated, else 0. Such an application's update
     * time is its APK's file time (one fixed date in every image) or the wall time of the first boot that scanned
     * it, so neither says which image its artwork comes from: the build is an input of the icon key of its own.
     */
    final long imageBuildMillis;
    /** The icon resource of the activity, else of the application; 0 when neither declares one. */
    final int iconResource;
    /** A fingerprint of the APK files the application's resources come from (splits, libraries, overlays), for the icon key. */
    final String resources;
    final Object source;

    Entry(String packageName, String activity, boolean builtin, boolean exported, boolean enabled,
            boolean permitted, long versionCode, long lastUpdateMillis, long imageBuildMillis, int iconResource,
            String resources, Object source) {
        this.packageName = packageName;
        this.activity = activity;
        this.builtin = builtin;
        this.exported = exported;
        this.enabled = enabled;
        this.permitted = permitted;
        this.versionCode = versionCode;
        this.lastUpdateMillis = lastUpdateMillis;
        this.imageBuildMillis = imageBuildMillis;
        this.iconResource = iconResource;
        this.resources = resources;
        this.source = source;
    }

    /** Whether the shell user can start it as a stock launcher's tap does: the rule of the listing and of the launch. */
    boolean launchable() {
        return exported && enabled && permitted;
    }
}
