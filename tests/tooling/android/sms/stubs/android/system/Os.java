// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for a class of android.system, only so that the service's vsock glue (VsockListener) compiles on a
// plain JVM; test_sms_service.py compiles it with the service, and the tests script the calls through Os.backend.
package android.system;

import java.io.FileDescriptor;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketException;

/**
 * The calls the glue makes, each handed to the test's {@link Backend}. What is public here is the platform's
 * public API, with its signatures, and nothing else: the service is compiled against the platform's stubs, which
 * leave out everything @hide. A hidden member is declared without {@code public}, so that code outside
 * android.system cannot call it (javac refuses, as the image's compile does) and reflection still finds it.
 * Socket, bind and listen are not here at all: the service never makes or binds a socket (the holder of the port
 * does), so a call to one does not compile. test_sms_service.py holds the list of what is public.
 */
public final class Os {
    private Os() {}

    /** What a test scripts: the world the calls see. */
    public interface Backend {
        FileDescriptor accept(FileDescriptor fd, SocketAddress peer) throws ErrnoException, SocketException;

        int poll(StructPollfd[] fds, int timeoutMillis) throws ErrnoException;

        int read(FileDescriptor fd, byte[] buffer, int offset, int length) throws ErrnoException, IOException;

        int write(FileDescriptor fd, byte[] buffer, int offset, int length) throws ErrnoException, IOException;

        void shutdown(FileDescriptor fd, int how) throws ErrnoException;

        void close(FileDescriptor fd) throws ErrnoException;

        int fcntlInt(FileDescriptor fd, int command, int argument) throws ErrnoException;
    }

    public static volatile Backend backend;

    /** The public overload: the platform fills an InetSocketAddress only; null asks for no address. */
    public static FileDescriptor accept(FileDescriptor fd, InetSocketAddress peerAddress)
            throws ErrnoException, SocketException {
        return backend.accept(fd, peerAddress);
    }

    /** @hide on the platform: the overload that fills a VmSocketAddress. Reached by reflection only. */
    static FileDescriptor accept(FileDescriptor fd, SocketAddress peerAddress) throws ErrnoException, SocketException {
        return backend.accept(fd, peerAddress);
    }

    /**
     * What the guest's libjavacore does for an AF_VSOCK socket: it converts the address of getsockname and
     * getpeername for AF_UNIX, AF_INET, AF_INET6, AF_NETLINK and AF_PACKET only, and fails for any other
     * family. No test can script it to answer: a peer's CID comes from the address that accept fills.
     */
    public static SocketAddress getpeername(FileDescriptor fd) throws ErrnoException {
        throw new IllegalArgumentException("unsupported ss_family: " + OsConstants.AF_VSOCK);
    }

    public static int poll(StructPollfd[] fds, int timeoutMillis) throws ErrnoException {
        return backend.poll(fds, timeoutMillis);
    }

    public static int read(FileDescriptor fd, byte[] buffer, int offset, int length)
            throws ErrnoException, IOException {
        return backend.read(fd, buffer, offset, length);
    }

    public static int write(FileDescriptor fd, byte[] buffer, int offset, int length)
            throws ErrnoException, IOException {
        return backend.write(fd, buffer, offset, length);
    }

    public static void shutdown(FileDescriptor fd, int how) throws ErrnoException {
        backend.shutdown(fd, how);
    }

    public static void close(FileDescriptor fd) throws ErrnoException {
        backend.close(fd);
    }

    public static int fcntlInt(FileDescriptor fd, int command, int argument) throws ErrnoException {
        return backend.fcntlInt(fd, command, argument);
    }
}
