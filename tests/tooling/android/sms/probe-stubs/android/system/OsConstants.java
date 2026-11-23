// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for android.system.OsConstants (see android/net/LocalSocketAddress.java for what the stubs are).
package android.system;

public final class OsConstants {
    private OsConstants() {
    }

    public static final int SOCK_STREAM = 1;

    /** The platform names every errno; the stub names the ones the probe's tests use. */
    public static String errnoName(int errno) {
        switch (errno) {
            case 1: return "EPERM";
            case 2: return "ENOENT";
            case 13: return "EACCES";
            case 111: return "ECONNREFUSED";
            default: return null;
        }
    }
}
