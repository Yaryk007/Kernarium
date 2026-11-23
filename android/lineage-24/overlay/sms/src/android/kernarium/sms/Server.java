// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.io.IOException;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

/**
 * The host channel's server: one connection at a time, to completion, in the order the kernel queued them.
 * It reads a request only from a connection whose peer is the host, CID 2, and dispatches at most one
 * frame per connection, to the handler, on its own thread. Nothing here names a platform class: the
 * listener, the connection and the handler are interfaces (VsockListener is the listener on Android).
 *
 * <p>The order is a contract the record relies on: a request that was queued before another is dispatched,
 * or dropped, before it, so a retry never overtakes the first attempt it follows. Only a change to this
 * class, a second accept thread or a pool, can break it.
 */
final class Server implements Runnable {
    /** The CID of the host side of this virtual machine. */
    static final int HOST_CID = 2;
    /** The time a request has to arrive in, from the accept, and an answer to be written in. */
    static final long STEP_NANOS = 5_000_000_000L;

    /** Where connections come from. */
    interface Listener {
        /** Waits for the next connection; null when there will be none, which ends the server. */
        Conn accept() throws IOException;
    }

    /** One accepted connection. Every wait ends at the deadline it is given, with an IOException. */
    interface Conn {
        /** The peer's CID, as the accept reported it; throws when the accept reported none. */
        int peerCid() throws Exception;

        /** Reads 1 to {@code length} bytes; 0 at the end of the stream. */
        int read(byte[] buffer, int offset, int length, long deadlineNanos) throws IOException;

        /** Whether a read would return at once. */
        boolean readableNow() throws IOException;

        void write(byte[] bytes, long deadlineNanos) throws IOException;

        /** Shuts the connection down and closes it. */
        void close();
    }

    /** What a request comes to: the answer's payload, empty to say the request was not taken. */
    interface Handler {
        byte[] handle(byte[] payload);
    }

    private final Listener listener;
    private final Handler handler;
    private final Consumer<String> log;
    private final LongSupplier clock;
    private final long retryMillis;
    private long refusals;
    private long unaddressed;

    Server(Listener listener, Handler handler, Consumer<String> log, LongSupplier clock, long retryMillis) {
        this.listener = listener;
        this.handler = handler;
        this.log = log;
        this.clock = clock;
        this.retryMillis = retryMillis;
    }

    /** Serves until the listener has no more connections to give, or the thread is interrupted. */
    @Override
    public void run() {
        while (!Thread.currentThread().isInterrupted()) {
            try {
                if (!serveOne()) return;
            } catch (IOException | RuntimeException e) {
                log.accept("host channel: " + e.getClass().getSimpleName());
                try {
                    Thread.sleep(retryMillis);
                } catch (InterruptedException interrupted) {
                    return;
                }
            }
        }
    }

    /** Accepts one connection and serves it to completion; false when the listener gave none, for good. */
    boolean serveOne() throws IOException {
        final Conn conn = listener.accept();
        if (conn == null) return false;
        try {
            serve(conn);
        } finally {
            conn.close();
        }
        return true;
    }

    private void serve(Conn conn) {
        final int cid;
        try {
            cid = conn.peerCid();
        } catch (Exception e) {
            // No peer address: fail closed, nothing read, nothing written. Logged for the first connection, then
            // one in 256, so that a connection that never gets served is never silent.
            if (unaddressed++ % 256 == 0) {
                log.accept("refused a connection with no peer address: " + e.getClass().getSimpleName());
            }
            return;
        }
        if (cid != HOST_CID) {
            if (refusals++ % 256 == 0) log.accept("refused a connection from CID " + Integer.toUnsignedString(cid));
            return;
        }
        final long arrival = clock.getAsLong() + STEP_NANOS;
        final byte[] header = new byte[HostFrame.HEADER_BYTES];
        try {
            if (!readAll(conn, header, arrival)) return; // the end of the stream: no answer, no dispatch
            final int length = HostFrame.requestLength(header);
            if (length < 0) {
                notTaken(conn);
                return;
            }
            final byte[] payload = new byte[length];
            if (!readAll(conn, payload, arrival)) return;
            // The host that has cancelled or given up leaves the end of the stream, and a second frame or any
            // other byte is not a request this connection may make. Bytes that come after this look are never read.
            if (conn.readableNow()) {
                final byte[] extra = new byte[1];
                if (conn.read(extra, 0, 1, arrival) > 0) notTaken(conn);
                return;
            }
            dispatch(conn, payload);
        } catch (IOException e) {
            // the deadline, a reset or a failed write: closed without an answer
        }
    }

    private void dispatch(Conn conn, byte[] payload) throws IOException {
        final byte[] answer;
        try {
            answer = handler.handle(payload);
        } catch (Throwable e) {
            log.accept("host channel: the handler failed: " + e.getClass().getSimpleName());
            return;
        }
        if (answer == null || answer.length > HostFrame.MAXIMUM_ANSWER) {
            log.accept("host channel: the handler gave no usable answer");
            return;
        }
        conn.write(HostFrame.encode(answer), clock.getAsLong() + STEP_NANOS);
    }

    private void notTaken(Conn conn) throws IOException {
        conn.write(HostFrame.encode(new byte[0]), clock.getAsLong() + STEP_NANOS);
    }

    /** Fills the buffer; false at the end of the stream before it is full. */
    private static boolean readAll(Conn conn, byte[] buffer, long deadlineNanos) throws IOException {
        int have = 0;
        while (have < buffer.length) {
            final int n = conn.read(buffer, have, buffer.length - have, deadlineNanos);
            if (n <= 0) return false;
            have += n;
        }
        return true;
    }
}
