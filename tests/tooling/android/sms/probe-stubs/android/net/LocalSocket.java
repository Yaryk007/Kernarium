// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for android.net.LocalSocket (see LocalSocketAddress). connect answers as the test scripts it through
// the system property probe.connect: "ok", or the message of the IOException that the platform's connect throws (its
// strerror text), and names the address it was given on standard error.
package android.net;

import java.io.IOException;

public class LocalSocket {
    public LocalSocket() {
    }

    public void connect(LocalSocketAddress endpoint) throws IOException {
        System.err.println("LocalSocket.connect " + endpoint.getNamespace() + " " + endpoint.getName());
        final String answer = System.getProperty("probe.connect", "ok");
        if (!answer.equals("ok")) {
            throw new IOException(answer);
        }
    }

    public void close() throws IOException {
    }
}
