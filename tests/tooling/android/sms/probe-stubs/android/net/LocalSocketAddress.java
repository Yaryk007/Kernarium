// SPDX-License-Identifier: GPL-2.0-or-later
//
// A stand-in for android.net.LocalSocketAddress, only so that the shell's probe (tests/integration/android/sms/tool)
// compiles on a plain JVM. Everything declared here is the platform's public API with its signatures, and nothing the
// probe does not use: the probe is compiled against the platform's public stubs, so a member that is not here is a
// member to look up in the public API before the probe may call it (test_sms_runner.py, ProbeSourceTests).
package android.net;

public class LocalSocketAddress {
    public enum Namespace {
        ABSTRACT,
        RESERVED,
        FILESYSTEM
    }

    private final String name;
    private final Namespace namespace;

    public LocalSocketAddress(String name, Namespace namespace) {
        this.name = name;
        this.namespace = namespace;
    }

    public String getName() {
        return name;
    }

    public Namespace getNamespace() {
        return namespace;
    }
}
