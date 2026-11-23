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

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;

/**
 * The door to the renderer for the adb shell user (kernarium_apps), nothing else: the manifest guards it with
 * ACCESS_CONTENT_PROVIDERS_EXTERNALLY, which only the shell and the system hold, and {@link #call} checks that
 * the caller is the shell user. It parses nothing of any application's files: it passes a request to the isolated
 * renderer ({@link IconWorker}) and the answer back. It has no data to query, insert, update or delete.
 */
public final class IconProvider extends ContentProvider {
    private IconWorker mWorker;

    @Override
    public boolean onCreate() {
        mWorker = new IconWorker(getContext());
        return true;
    }

    @Override
    public Bundle call(String authority, String method, String arg, Bundle extras) {
        return handle(method, extras);
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        return handle(method, extras);
    }

    private Bundle handle(String method, Bundle extras) {
        if (!IconProtocol.fromShell(Binder.getCallingUid())) {
            throw new SecurityException("only the shell user may ask for icons");
        }
        if (!IconProtocol.METHOD_RENDER.equals(method) || extras == null) {
            return IconWorker.failed();
        }
        int resource = extras.getInt(IconProtocol.KEY_RESOURCE);
        int[] kinds = extras.getIntArray(IconProtocol.KEY_KINDS);
        String[] paths = extras.getStringArray(IconProtocol.KEY_PATHS);
        if (!IconProtocol.validRequest(resource, kinds, paths)) {
            return IconWorker.failed();
        }
        return mWorker.render(resource, kinds, paths);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
