// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for a class of android.system, only so that the service's vsock glue (VsockListener) compiles on a
// plain JVM; test_sms_service.py compiles it with the service, and the tests script the calls through Os.backend.
package android.system;

/** The Linux values of the constants the glue uses. */
public final class OsConstants {
    private OsConstants() {}

    public static final int AF_VSOCK = 40;
    public static final int VMADDR_CID_ANY = -1;
    public static final int VMADDR_CID_HOST = 2;
    public static final int POLLIN = 1;
    public static final int POLLOUT = 4;
    public static final int POLLERR = 8;
    public static final int POLLHUP = 16;
    public static final int EINTR = 4;
    public static final int EAGAIN = 11;
    public static final int ECONNABORTED = 103;
    public static final int F_SETFD = 2;
    public static final int FD_CLOEXEC = 1;
    public static final int F_SETFL = 4;
    public static final int O_NONBLOCK = 2048;
    public static final int SHUT_RDWR = 2;
}
