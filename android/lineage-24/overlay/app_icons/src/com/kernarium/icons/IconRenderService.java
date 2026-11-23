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

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * Draws icons in an isolated process (the manifest's {@code isolatedProcess}): no permission, no network, no file
 * of its own, a user id of its own. {@link IconProvider} binds it, sends it one request at a time and kills it
 * (by unbinding) when it fails or takes too long. It is not exported, and it checks what it is sent as if
 * anything could send it.
 */
public final class IconRenderService extends Service {
    /** The one transaction: resource id, kinds, paths in; status, then size, adaptive, alpha and the PNG out. */
    static final int TRANSACTION_RENDER = IBinder.FIRST_CALL_TRANSACTION;

    private final IBinder mBinder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != TRANSACTION_RENDER) {
                return super.onTransact(code, data, reply, flags);
            }
            int resource = data.readInt();
            int[] kinds = data.createIntArray();
            String[] paths = data.createStringArray();
            IconDrawing.Result drawn = IconProtocol.validRequest(resource, kinds, paths)
                    ? IconDrawing.render(resource, kinds, paths) : null;
            reply.writeNoException();
            if (drawn == null) {
                reply.writeInt(IconProtocol.STATUS_ERROR);
            } else {
                reply.writeInt(IconProtocol.STATUS_OK);
                reply.writeInt(drawn.size);
                reply.writeInt(drawn.adaptive ? 1 : 0);
                reply.writeInt(drawn.alpha ? 1 : 0);
                reply.writeByteArray(drawn.png);
            }
            return true;
        }
    };

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }
}
