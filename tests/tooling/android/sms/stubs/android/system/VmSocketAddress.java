// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for a class of android.system, only so that the service's vsock glue (VsockListener) compiles on a
// plain JVM; test_sms_service.py compiles it with the service, and the tests script the calls through Os.backend.
package android.system;

import java.net.SocketAddress;

/**
 * The constructor and getters the service calls. The platform's class keeps the two fields named here, and
 * libjavacore sets them in the address that Os.accept (or recvfrom) is given, to the connecting peer's port and
 * CID. It converts an AF_VSOCK address nowhere else, so a test that wants a peer to be known fills them as that
 * call does (ChannelTest.World).
 */
public final class VmSocketAddress extends SocketAddress {
    private static final long serialVersionUID = 1L;
    private final int svmPort;
    private final int svmCid;

    public VmSocketAddress(int port, int cid) {
        this.svmPort = port;
        this.svmCid = cid;
    }

    public int getSvmPort() {
        return svmPort;
    }

    public int getSvmCid() {
        return svmCid;
    }
}
