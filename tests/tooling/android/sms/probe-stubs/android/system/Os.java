// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for android.system.Os (see android/net/LocalSocketAddress.java for what the stubs are). socket answers
// as the test scripts it through the system property probe.socket: "ok", or "errno:N" for an ErrnoException.
package android.system;

import java.io.FileDescriptor;

public final class Os {
    private Os() {
    }

    public static FileDescriptor socket(int domain, int type, int protocol) throws ErrnoException {
        final String answer = System.getProperty("probe.socket", "ok");
        if (!answer.equals("ok")) {
            throw new ErrnoException("socket", Integer.parseInt(answer.substring("errno:".length())));
        }
        return new FileDescriptor();
    }

    public static void close(FileDescriptor fd) throws ErrnoException {
    }
}
