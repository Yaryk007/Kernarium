// SPDX-License-Identifier: Apache-2.0
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.FileDescriptor;
import java.io.IOException;

/**
 * Tries to create a vsock socket as whoever runs it, for the shell's row of the text-message checks:
 * {@code CLASSPATH=/data/local/tmp/vsockprobe.dex app_process /system/bin VsockProbe}. It prints
 * {@code socket: errno=13 EACCES} when the domain may not, and {@code socket: ok} when it may. With
 * {@code unix PATH} it connects a {@link LocalSocket} to the file-system socket at PATH instead, for the handoff
 * socket's row, and prints {@code connect: errno=13 EACCES} when the domain may not and {@code connect: ok} when it
 * may. Only the platform's public API is used: build-apk.sh compiles this against the public android.jar.
 */
public final class VsockProbe {
    /** Linux's AF_VSOCK; android.system.OsConstants has no public name for it. */
    private static final int AF_VSOCK = 40;

    /**
     * What the platform's connect throws is an IOException whose message is the errno's strerror text, so the
     * errors this probe is asked about are told apart by it: Linux's EPERM, ENOENT, EACCES and ECONNREFUSED.
     */
    private static final String[] ERRNO_TEXT = {
        "Operation not permitted", "No such file or directory", "Permission denied", "Connection refused"
    };
    private static final int[] ERRNO_VALUE = {1, 2, 13, 111};

    private VsockProbe() {
    }

    public static void main(String[] arguments) {
        if (arguments.length == 2 && arguments[0].equals("unix")) {
            connect(arguments[1]);
            return;
        }
        try {
            final FileDescriptor descriptor = Os.socket(AF_VSOCK, OsConstants.SOCK_STREAM, 0);
            Os.close(descriptor);
            System.out.println("socket: ok");
        } catch (ErrnoException e) {
            System.out.println("socket: errno=" + e.errno + " " + OsConstants.errnoName(e.errno));
        }
    }

    private static void connect(String path) {
        final LocalSocket socket = new LocalSocket();
        try {
            socket.connect(new LocalSocketAddress(path, LocalSocketAddress.Namespace.FILESYSTEM));
            System.out.println("connect: ok");
        } catch (IOException e) {
            System.out.println("connect: " + describe(e.getMessage()));
        } finally {
            try {
                socket.close();
            } catch (IOException ignored) {
                // The answer is printed; closing a socket that is not connected has nothing to add.
            }
        }
    }

    private static String describe(String message) {
        for (int i = 0; i < ERRNO_TEXT.length; i++) {
            if (ERRNO_TEXT[i].equals(message)) {
                return "errno=" + ERRNO_VALUE[i] + " " + OsConstants.errnoName(ERRNO_VALUE[i]);
            }
        }
        return "error: " + message;
    }
}
