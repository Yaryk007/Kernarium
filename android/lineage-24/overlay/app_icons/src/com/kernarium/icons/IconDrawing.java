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

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import android.os.LocaleList;
import android.util.DisplayMetrics;

import java.io.ByteArrayOutputStream;
import java.util.Locale;

/**
 * Draws one application's icon from the files its resources come from, in the isolated process: opens them as the
 * framework adds them for the application itself, inflates the icon at the largest density, draws it and encodes
 * a PNG. Everything an application's own files can make it do (XML, vectors, bitmaps, nine-patches) happens here
 * and nowhere else.
 *
 * <p>The configuration is fixed: the largest density, day mode and one locale, so that one resource id is one
 * picture for a version of the package, whatever the device's own state.
 */
final class IconDrawing {
    /** What one drawing came to. */
    static final class Result {
        final int size;
        final boolean adaptive;
        final boolean alpha;
        final byte[] png;

        Result(int size, boolean adaptive, boolean alpha, byte[] png) {
            this.size = size;
            this.adaptive = adaptive;
            this.alpha = alpha;
            this.png = png;
        }
    }

    private IconDrawing() {}

    /**
     * The icon of {@code resource} at the largest size whose PNG is within the bound, or null when it cannot be
     * had: a file that cannot be opened, a resource that is no drawable, an encoder that fails, a PNG over the
     * bound at every size, or running out of memory. A size is stepped down only for a PNG that is too big, never
     * after a failure.
     */
    static Result render(int resource, int[] kinds, String[] paths) {
        AssetManager assets = new AssetManager();
        try {
            for (int i = 0; i < kinds.length; i++) {
                int cookie;
                switch (kinds[i]) {
                    case IconProtocol.KIND_LIBRARY:
                        cookie = assets.addAssetPathAsSharedLibrary(paths[i]);
                        break;
                    case IconProtocol.KIND_OVERLAY:
                        cookie = assets.addOverlayPath(paths[i]);
                        break;
                    default:
                        cookie = assets.addAssetPath(paths[i]);
                        break;
                }
                // The base and the splits must open; a library or an overlay that does not is skipped, as the framework does.
                if (cookie == 0 && kinds[i] != IconProtocol.KIND_LIBRARY && kinds[i] != IconProtocol.KIND_OVERLAY) {
                    return null;
                }
            }
            DisplayMetrics metrics = new DisplayMetrics();
            metrics.setToDefaults();
            metrics.densityDpi = IconProtocol.DENSITY;
            metrics.density = IconProtocol.DENSITY / (float) DisplayMetrics.DENSITY_DEFAULT;
            Resources resources = new Resources(assets, metrics, configuration());
            Drawable drawable = resources.getDrawableForDensity(resource, IconProtocol.DENSITY, null);
            if (drawable == null) {
                return null;
            }
            for (int size = IconProtocol.SIZES[0]; size != 0; size = IconProtocol.smaller(size)) {
                Result drawn = draw(drawable, size);
                if (drawn == null) {
                    return null;
                }
                if (drawn.png.length <= IconProtocol.MAX_PNG_BYTES) {
                    return drawn;
                }
            }
            return null;
        } catch (Exception | OutOfMemoryError failed) {
            return null;
        } finally {
            assets.close();
        }
    }

    private static Configuration configuration() {
        Configuration configuration = new Configuration();
        configuration.densityDpi = IconProtocol.DENSITY;
        configuration.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        configuration.setLocales(new LocaleList(Locale.US));
        return configuration;
    }

    private static Result draw(Drawable drawable, int size) {
        Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(bitmap);
            boolean adaptive = drawable instanceof AdaptiveIconDrawable;
            if (adaptive) {
                // The layers over the window the launcher shows, with no mask: iOS puts its own on.
                AdaptiveIconDrawable icon = (AdaptiveIconDrawable) drawable;
                int outset = IconProtocol.outset(size);
                for (Drawable layer : new Drawable[] {icon.getBackground(), icon.getForeground()}) {
                    if (layer != null) {
                        layer.setBounds(-outset, -outset, size + outset, size + outset);
                        layer.draw(canvas);
                    }
                }
            } else {
                drawable.setBounds(0, 0, size, size);
                drawable.draw(canvas);
            }
            int[] pixels = new int[size * size];
            bitmap.getPixels(pixels, 0, size, 0, 0, size, size);
            ByteArrayOutputStream png = new ByteArrayOutputStream();
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, png)) {
                return null;
            }
            return new Result(size, adaptive, IconProtocol.hasAlpha(pixels), png.toByteArray());
        } finally {
            bitmap.recycle();
        }
    }
}
