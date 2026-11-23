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

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.SystemClock;

/**
 * The provider's hold on the isolated renderer on Android: the platform's side of {@link IconWorkerCore}, which holds
 * the rules (one deadline for a request, the worker unbound after a failure and when idle). This binds the service of
 * this package, sends a request as one transaction and reads the reply's fields, and runs the idle cleanup on the main
 * looper.
 */
final class IconWorker {
    private final IconWorkerCore mCore;

    IconWorker(Context context) {
        final Handler main = new Handler(Looper.getMainLooper());
        mCore = new IconWorkerCore(new BoundLink(context), new IconWorkerCore.Poster() {
            @Override
            public void postDelayed(Runnable task, long millis) {
                main.postDelayed(task, millis);
            }

            @Override
            public void remove(Runnable task) {
                main.removeCallbacks(task);
            }
        }, SystemClock::elapsedRealtime, IconWorkerCore.PRODUCTION);
    }

    /** The answer for one icon: the reply's fields, or a failed status. */
    Bundle render(int resource, int[] kinds, String[] paths) {
        IconWorkerCore.Reply reply = mCore.render(resource, kinds, paths);
        if (!reply.ok()) {
            return failed();
        }
        Bundle answer = new Bundle();
        answer.putInt(IconProtocol.KEY_STATUS, reply.status);
        answer.putInt(IconProtocol.KEY_SIZE, reply.size);
        answer.putBoolean(IconProtocol.KEY_ADAPTIVE, reply.adaptive);
        answer.putBoolean(IconProtocol.KEY_ALPHA, reply.alpha);
        answer.putByteArray(IconProtocol.KEY_PNG, reply.png);
        return answer;
    }

    static Bundle failed() {
        Bundle answer = new Bundle();
        answer.putInt(IconProtocol.KEY_STATUS, IconProtocol.STATUS_ERROR);
        return answer;
    }

    /** Binds the isolated service of this package. */
    private static final class BoundLink implements IconWorkerCore.Link {
        private final Context mContext;

        BoundLink(Context context) {
            mContext = context;
        }

        @Override
        public IconWorkerCore.Handle bind(IconWorkerCore.Listener listener) {
            final ServiceConnection connection = new ServiceConnection() {
                @Override
                public void onServiceConnected(ComponentName name, IBinder service) {
                    listener.connected(new BinderSession(service));
                }

                @Override
                public void onServiceDisconnected(ComponentName name) {
                    listener.lost();
                }

                @Override
                public void onBindingDied(ComponentName name) {
                    listener.lost();
                }
            };
            Intent intent = new Intent().setClassName(IconProtocol.PACKAGE, IconProtocol.SERVICE_CLASS);
            boolean bound = mContext.bindService(intent, connection, Context.BIND_AUTO_CREATE);
            IconWorkerCore.Handle handle = () -> {
                try {
                    mContext.unbindService(connection);
                } catch (IllegalArgumentException notBound) {
                    // Already gone.
                }
            };
            if (!bound) {
                handle.unbind(); // a failed bind is still released
                return null;
            }
            return handle;
        }
    }

    /** One connected worker. */
    private static final class BinderSession implements IconWorkerCore.Session {
        private final IBinder mWorker;

        BinderSession(IBinder worker) {
            mWorker = worker;
        }

        @Override
        public IconWorkerCore.Reply render(int resource, int[] kinds, String[] paths) throws Exception {
            Parcel data = Parcel.obtain();
            Parcel reply = Parcel.obtain();
            try {
                data.writeInt(resource);
                data.writeIntArray(kinds);
                data.writeStringArray(paths);
                mWorker.transact(IconRenderService.TRANSACTION_RENDER, data, reply, 0);
                reply.setDataPosition(0);
                reply.readException();
                if (reply.readInt() != IconProtocol.STATUS_OK) {
                    return IconWorkerCore.Reply.FAILED;
                }
                int size = reply.readInt();
                boolean adaptive = reply.readInt() != 0;
                boolean alpha = reply.readInt() != 0;
                byte[] png = reply.createByteArray();
                return new IconWorkerCore.Reply(IconProtocol.STATUS_OK, size, adaptive, alpha, png);
            } finally {
                data.recycle(); // this thread's own parcels: it ends when the worker answers or dies
                reply.recycle();
            }
        }

        @Override
        public boolean alive() {
            return mWorker.isBinderAlive();
        }
    }
}
