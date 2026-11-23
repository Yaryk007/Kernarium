// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.nio.charset.StandardCharsets;

/**
 * The frame of the host channel, both directions: an 8-byte header, then the payload. The header is the
 * magic {@code KSMS}, the frame version (1) as a u16 little endian and the payload's length as a u16 little
 * endian. A request carries 1 to {@link #MAXIMUM_REQUEST} bytes and an answer 0 to {@link #MAXIMUM_ANSWER};
 * a zero-length answer means that the request was not taken, and always carries a version 1 header. The
 * payload's own version (Message.VERSION) is independent of the frame's. The host's side is
 * Andarium/AndariumCore GuestSmsChannel.swift; tests/unit/andarium/fixtures/sms-golden-frames.txt holds the
 * bytes both sides test against.
 */
final class HostFrame {
    static final int HEADER_BYTES = 8;
    static final int VERSION = 1;
    static final int MAXIMUM_REQUEST = 8192;
    static final int MAXIMUM_ANSWER = 4096;
    private static final byte[] MAGIC = "KSMS".getBytes(StandardCharsets.US_ASCII);

    private HostFrame() {}

    /** The header and the payload. */
    static byte[] encode(byte[] payload) {
        final byte[] frame = new byte[HEADER_BYTES + payload.length];
        System.arraycopy(MAGIC, 0, frame, 0, MAGIC.length);
        frame[4] = VERSION;
        frame[5] = 0;
        frame[6] = (byte) payload.length;
        frame[7] = (byte) (payload.length >> 8);
        System.arraycopy(payload, 0, frame, HEADER_BYTES, payload.length);
        return frame;
    }

    /** The length a request's header announces, or -1 when the header is not one a request may have. */
    static int requestLength(byte[] header) {
        if (header.length != HEADER_BYTES) return -1;
        for (int i = 0; i < MAGIC.length; ++i) {
            if (header[i] != MAGIC[i]) return -1;
        }
        if ((header[4] & 0xff) != VERSION || header[5] != 0) return -1;
        final int length = (header[6] & 0xff) | (header[7] & 0xff) << 8;
        return length >= 1 && length <= MAXIMUM_REQUEST ? length : -1;
    }
}
