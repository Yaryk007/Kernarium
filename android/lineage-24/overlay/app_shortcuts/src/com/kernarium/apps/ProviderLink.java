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

import com.kernarium.icons.IconProtocol;

/**
 * The tool's hold on the icon renderer's content provider, without the platform: who answers is checked on every
 * acquisition, the hold is made for one Android user and given back for that user, and a hold that is closed stays
 * closed.
 *
 * <p>The provider is the image's own (package, exact class, system application) or there is no renderer: any other
 * answerer of the authority (an installed package that took the name) is released unread and the fixed "not available"
 * result is given, so that nothing of an application's files is sent to it. The user is the one the command listed the
 * applications for, whatever the foreground user is by now: the files named in a request are that user's.
 *
 * <p>{@link #close()} is final. A call that was abandoned (the command stopped waiting for it) may still end later, with
 * a dead provider; it neither takes a new hold nor gives one back twice.
 */
final class ProviderLink {
    /** The fixed reason for a renderer that cannot be had, whatever the platform said. */
    static final String UNAVAILABLE = "the icon renderer is not available";

    /** The process that holds the provider ended: the call in flight is lost, and the hold is gone with it. */
    static final class ProviderDied extends Exception {
        private static final long serialVersionUID = 1L;

        ProviderDied(Throwable cause) {
            super(cause);
        }
    }

    /** The platform's way to hold and release the provider (the activity manager, with a token of its own). */
    interface Transport {
        /** Holds the provider for the user: null when there is none. The caller checks who it is. */
        Holding acquire(int user) throws Exception;

        /** Gives the hold back for the user. Fixed: this never throws. */
        void release(int user);
    }

    /** A held provider and who it is, as the platform resolved it. */
    interface Holding {
        String packageName();

        String className();

        /** Whether the provider's application is part of the system image. */
        boolean system();

        /**
         * One icon: the icon the provider answered with, or null when the answer is not an ok. Throws
         * {@link ProviderDied} when the provider's process died, and anything else when the call failed.
         */
        Icon render(int resource, int[] kinds, String[] paths) throws Exception;
    }

    private final Transport mTransport;
    private final int mUser;
    /** The current hold, or null when there is none to use (after a death, before the next acquisition). */
    private Holding mHolding;
    /** Whether the transport holds the provider for this link: the one thing {@link #release()} gives back. */
    private boolean mHeld;
    private boolean mClosed;

    /** Holds the provider for the user; throws a {@link PlatformException} with the fixed reason when none can be had. */
    ProviderLink(Transport transport, int user) throws PlatformException {
        mTransport = transport;
        mUser = user;
        acquire();
    }

    /** The provider the tool takes answers from: the image's own, by package, exact class and provenance. */
    private static boolean expected(Holding holding) {
        return IconProtocol.PACKAGE.equals(holding.packageName())
                && IconProtocol.PROVIDER_CLASS.equals(holding.className()) && holding.system();
    }

    private Holding acquire() throws PlatformException {
        synchronized (this) {
            if (mClosed) {
                throw new PlatformException(UNAVAILABLE);
            }
        }
        Holding holding;
        try {
            holding = mTransport.acquire(mUser);
        } catch (Exception | LinkageError problem) {
            throw new PlatformException(UNAVAILABLE, problem);
        }
        if (holding == null) {
            throw new PlatformException(UNAVAILABLE);
        }
        boolean keep = false;
        try {
            keep = expected(holding);
        } catch (RuntimeException | LinkageError problem) {
            // Who answered cannot be told: it is not taken.
        }
        synchronized (this) {
            if (keep && !mClosed) {
                mHolding = holding;
                mHeld = true;
                return holding;
            }
        }
        mTransport.release(mUser); // an unexpected answerer, or the link was closed while it was asked: not kept
        throw new PlatformException(UNAVAILABLE);
    }

    private void release() {
        boolean held;
        synchronized (this) {
            held = mHeld;
            mHeld = false;
            mHolding = null;
        }
        if (held) {
            mTransport.release(mUser);
        }
    }

    /**
     * The icon of one request. A provider that died costs this icon, and the hold is given back; the next icon takes a
     * fresh hold, and when none can be had the renderer is gone ({@link AppsCommand.RendererGone}). An answer that is
     * not an ok is an error for this icon alone, and the hold is kept.
     */
    Icon render(int resource, int[] kinds, String[] paths) throws Exception {
        Holding holding;
        synchronized (this) {
            holding = mHolding; // none after close: the acquisition below refuses
        }
        if (holding == null) {
            holding = renewed();
        }
        Icon icon;
        try {
            icon = holding.render(resource, kinds, paths);
        } catch (ProviderDied dead) {
            release(); // the next icon takes a fresh hold; nothing takes one for an icon that nobody waits for
            throw new PlatformException("the icon renderer died");
        }
        if (icon == null) {
            throw new PlatformException("the icon could not be drawn");
        }
        return icon;
    }

    /** A fresh hold, taken for the next icon after a death, as that icon is asked. */
    private Holding renewed() throws AppsCommand.RendererGone {
        try {
            return acquire();
        } catch (PlatformException unreachable) {
            throw new AppsCommand.RendererGone(UNAVAILABLE, unreachable);
        }
    }

    /** Gives the hold back, once, and takes no other. */
    void close() {
        synchronized (this) {
            mClosed = true;
        }
        release();
    }
}
