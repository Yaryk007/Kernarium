// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import android.app.Service;
import android.content.Intent;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.SELinux;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.function.Consumer;

/**
 * The service's process, which runs only while text messages are in flight. The holder of vsock port 5560
 * (kernarium_sms_listener, a small native daemon that never reads the port) starts it when a connection waits,
 * through the waker (kernarium_sms_wake.sh), and the session takes the listening descriptor from the holder over
 * the holder's own socket. It then serves the host's requests exactly as a service that listens itself would, in
 * the order the kernel queued them, hands each message to Android once (DeliveryRecord), and ends its own process
 * once the port has been quiet for the idle window. A connection that arrives after that waits in the holder's
 * queue for the next session.
 *
 * <p>The record is in device-protected storage: it is usable at boot, whether or not the application is
 * direct-boot aware, and Erase removes it with the data partition. It is read and rewritten whole for each
 * request, so a process that is started again by the next message has lost nothing.
 *
 * <p>The connection to the holder is kept open for the life of the process and never closed here: the holder takes
 * its end as the end of the session, so that two sessions never accept from the port at once.
 */
public final class HostService extends Service {
    private static final String TAG = "AndariumSms";
    /** The socket init makes for the holder (/dev/socket/kernarium_sms). */
    private static final String HANDOFF = "kernarium_sms";
    /** How long the port may be quiet before the session ends. */
    private static final long IDLE_MILLIS = 30_000;
    private static final int HANDOFF_WAIT_MILLIS = 10_000;
    private static final long RETRY_MILLIS = 1000;
    /** The longest the process stays after the session ended, if the service does not stop it first. */
    private static final long LINGER_MILLIS = 2000;

    private final Object lock = new Object();
    private Thread session;
    /** Never closed: its end is the end of the session, and the garbage collector must not close it. */
    private LocalSocket handoff;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        synchronized (lock) {
            if (session == null) {
                session = new Thread(this::run, "andarium-sms-host");
                session.start();
            }
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        // The process ends with its session: nothing else keeps a cached process worth its memory.
        Process.killProcess(Process.myPid());
    }

    private void run() {
        final Consumer<String> log = line -> Log.i(TAG, line);
        try {
            final FileDescriptor listening = receiveListener();
            try {
                serve(listening, log);
            } finally {
                try {
                    Os.close(listening);
                } catch (ErrnoException e) {
                    // our copy only: the holder keeps the port
                }
            }
        } catch (IOException | RuntimeException e) {
            Log.e(TAG, "the host channel session failed", e);
        } finally {
            new Handler(Looper.getMainLooper()).post(this::stopSelf);
            try {
                Thread.sleep(LINGER_MILLIS);
            } catch (InterruptedException e) {
                // ends here all the same
            }
            Process.killProcess(Process.myPid());
        }
    }

    /** Connects to the holder and takes the listening descriptor it passes. */
    private FileDescriptor receiveListener() throws IOException {
        final LocalSocket socket = new LocalSocket();
        handoff = socket;
        socket.connect(new LocalSocketAddress(HANDOFF, LocalSocketAddress.Namespace.RESERVED));
        socket.setSoTimeout(HANDOFF_WAIT_MILLIS);
        if (socket.getInputStream().read() < 0) throw new IOException("the holder sent nothing");
        final FileDescriptor[] received = socket.getAncillaryFileDescriptors();
        if (received == null || received.length != 1 || received[0] == null) {
            throw new IOException("the holder passed no listening descriptor");
        }
        return received[0];
    }

    private void serve(FileDescriptor listening, Consumer<String> log) throws IOException {
        final File directory = createDeviceProtectedStorageContext().getFilesDir();
        log.accept("record in " + directory + ", labelled " + SELinux.getFileContext(directory.getPath()));
        final DeliveryRecord record = new DeliveryRecord(new RecordFiles.Nio(directory.toPath()),
                RecordStore.CAPACITY, new Delivery(this), log);
        final Server server = new Server(new VsockListener(listening, IDLE_MILLIS, log, System::nanoTime),
                new DeliveryService(record), log, System::nanoTime, RETRY_MILLIS);
        log.accept("serving the host's requests");
        server.run();
    }
}
