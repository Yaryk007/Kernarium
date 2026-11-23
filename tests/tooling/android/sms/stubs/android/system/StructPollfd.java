// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for a class of android.system, only so that the service's vsock glue (VsockListener) compiles on a
// plain JVM; test_sms_service.py compiles it with the service, and the tests script the calls through Os.backend.
package android.system;

import java.io.FileDescriptor;

public final class StructPollfd {
    public FileDescriptor fd;
    public short events;
    public short revents;
}
