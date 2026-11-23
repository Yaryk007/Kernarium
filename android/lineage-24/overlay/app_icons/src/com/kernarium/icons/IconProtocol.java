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

package com.kernarium.icons;

/**
 * What {@code kernarium_apps} (the adb shell user) and the icon renderer (KernariumIcons) agree on: the names,
 * the limits and the checks that need no platform. The shell tool and the app both compile this file.
 *
 * <p>One request draws one icon: the resource id of the application's icon and the files its resources come
 * from, each with how it is added ({@code kinds[i]} for {@code paths[i]}: the base first, then its splits, its
 * shared libraries, its overlays). The renderer opens those files in an isolated process, draws the icon at the
 * largest of {@link #SIZES} whose PNG is within {@link #MAX_PNG_BYTES}, and answers {@link #KEY_STATUS} with the
 * rest of the keys. The answer is not trusted by the tool; the request is not trusted by the renderer.
 */
public final class IconProtocol {
    public static final String PACKAGE = "com.kernarium.icons";
    public static final String AUTHORITY = "com.kernarium.icons";
    public static final String SERVICE_CLASS = "com.kernarium.icons.IconRenderService";
    public static final String METHOD_RENDER = "render";

    public static final String KEY_RESOURCE = "resource";
    public static final String KEY_KINDS = "kinds";
    public static final String KEY_PATHS = "paths";
    public static final String KEY_STATUS = "status";
    public static final String KEY_SIZE = "size";
    public static final String KEY_ADAPTIVE = "adaptive";
    public static final String KEY_ALPHA = "alpha";
    public static final String KEY_PNG = "png";

    public static final int STATUS_OK = 0;
    public static final int STATUS_ERROR = 1;

    /** How a file is added to the resources: the order the framework adds them for the application itself. */
    public static final int KIND_BASE = 0;
    public static final int KIND_SPLIT = 1;
    public static final int KIND_LIBRARY = 2;
    public static final int KIND_OVERLAY = 3;

    /** The sizes an icon is drawn at, largest first: the next is tried only when a PNG is over the bound. */
    public static final int[] SIZES = {192, 128, 96};
    /** The largest PNG an answer carries. */
    public static final int MAX_PNG_BYTES = 98304;
    /** The most files one request names, and the longest path. */
    public static final int MAX_FILES = 128;
    public static final int MAX_PATH_CHARS = 1024;
    /** The density, as the largest the platform has, so that the best bitmap an application has is the one used. */
    public static final int DENSITY = 640;
    /** An adaptive icon's layers are drawn 108 dp for the 72 dp window a launcher shows: a quarter more on each side. */
    public static final float LAYER_OUTSET = 0.25f;

    /** The adb shell user's uid: the shell is one user's, whichever Android user is in the foreground. */
    public static final int SHELL_UID = 2000;
    /** The class of the provider whose answers the tool takes. */
    public static final String PROVIDER_CLASS = "com.kernarium.icons.IconProvider";

    private IconProtocol() {}

    /** Whether the uid is the adb shell user's, exactly: another Android user's app id 2000 is another uid. */
    public static boolean fromShell(int uid) {
        return uid == SHELL_UID;
    }

    /**
     * Whether a request is one the renderer will open: a resource id (any but zero: the id of a resource package
     * from 0x80 up has the sign bit set, and the framework's lookup judges it), and one to {@link #MAX_FILES} files
     * that start with exactly one base, then splits, libraries and overlays in that order, each an absolute path of
     * printable characters without a dot segment.
     */
    public static boolean validRequest(int resource, int[] kinds, String[] paths) {
        if (resource == 0 || kinds == null || paths == null || kinds.length != paths.length || kinds.length == 0
                || kinds.length > MAX_FILES || kinds[0] != KIND_BASE) {
            return false;
        }
        for (int i = 0; i < kinds.length; i++) {
            if (kinds[i] < KIND_BASE || kinds[i] > KIND_OVERLAY || (i > 0 && kinds[i] <= KIND_BASE)
                    || (i > 0 && kinds[i] < kinds[i - 1]) || !validPath(paths[i])) {
                return false;
            }
        }
        return true;
    }

    /** Whether the path is one a request may name: absolute, printable ASCII, within the bound, no empty or dot segment. */
    public static boolean validPath(String path) {
        if (path == null || path.isEmpty() || path.length() > MAX_PATH_CHARS || path.charAt(0) != '/') {
            return false;
        }
        for (int i = 0; i < path.length(); i++) {
            char c = path.charAt(i);
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
        }
        for (String segment : path.substring(1).split("/", -1)) {
            if (segment.isEmpty() || segment.equals(".") || segment.equals("..")) {
                return false;
            }
        }
        return true;
    }

    /** How far an adaptive icon's layers reach beyond the square, at a size. */
    public static int outset(int size) {
        return Math.round(size * LAYER_OUTSET);
    }

    /** Whether any pixel (ARGB) is not fully opaque. */
    public static boolean hasAlpha(int[] pixels) {
        for (int pixel : pixels) {
            if ((pixel >>> 24) != 0xFF) {
                return true;
            }
        }
        return false;
    }

    /** The size to try after {@code size} when its PNG was over the bound, or 0 when there is none. */
    public static int smaller(int size) {
        for (int i = 0; i + 1 < SIZES.length; i++) {
            if (SIZES[i] == size) {
                return SIZES[i + 1];
            }
        }
        return 0;
    }
}
