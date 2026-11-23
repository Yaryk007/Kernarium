// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for android.system.ErrnoException (see android/net/LocalSocketAddress.java for what the stubs are).
package android.system;

public final class ErrnoException extends Exception {
    public final int errno;

    public ErrnoException(String functionName, int errno) {
        super(functionName + " failed: " + errno);
        this.errno = errno;
    }
}
