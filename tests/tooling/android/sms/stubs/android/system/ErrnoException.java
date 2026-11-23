// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for a class of android.system, only so that the service's vsock glue (VsockListener) compiles on a
// plain JVM; test_sms_service.py compiles it with the service, and the tests script the calls through Os.backend.
package android.system;

public final class ErrnoException extends Exception {
    private static final long serialVersionUID = 1L;
    public final String functionName;
    public final int errno;

    public ErrnoException(String functionName, int errno) {
        super(functionName + " failed, errno " + errno);
        this.functionName = functionName;
        this.errno = errno;
    }
}
