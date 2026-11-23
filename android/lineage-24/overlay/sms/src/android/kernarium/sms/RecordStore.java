// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.NoSuchFileException;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Pattern;
import java.util.zip.CRC32;

/**
 * The delivery record's file: which identifiers this service has handed to Android, are handing over, or
 * can no longer say it handed over, each with the digest of its content. It is ASCII:
 *
 * <pre>
 * kernarium-sms-record 1
 * &lt;state&gt; &lt;identifier&gt; &lt;digest&gt; &lt;parts&gt;      one line per entry, oldest first
 * end &lt;crc&gt;
 * </pre>
 *
 * The state is p (pending), h (handed over) or u (uncertain), the identifier 32 hex digits, the digest 64,
 * the parts 1 to 11, or 0 for a tombstone, which only u may carry; the crc is the CRC-32 of every byte
 * before the end line, as 8 hex digits. The record is read whole, verified, and replaced whole: the next
 * version is written to a file of its own, forced, renamed over the record, and the directory is forced.
 * So a process that ends at any point leaves the old record or the new one, never a torn one, and every
 * failure is an exception that the caller answers by failing closed.
 */
final class RecordStore {
    static final String RECORD = "sms-delivery-record";
    static final String NEXT = "sms-delivery-record.new";
    static final String CORRUPT = "sms-delivery-record.corrupt";
    static final String HEADER = "kernarium-sms-record 1";
    /** The entries kept; the oldest goes first. */
    static final int CAPACITY = 256;

    private static final int HEADER_BYTES = HEADER.length() + 1;
    /** A line: state, identifier, digest and parts of at most two digits, with their separators and the line feed. */
    private static final int LINE_BYTES = 1 + 1 + 32 + 1 + 64 + 1 + 2 + 1;
    private static final int TRAILER_BYTES = "end 00000000\n".length();
    private static final Pattern ENTRY = Pattern.compile("[phu] [0-9a-f]{32} [0-9a-f]{64} (0|[1-9]|1[01])");
    private static final Pattern TRAILER = Pattern.compile("end [0-9a-f]{8}\n");

    /** The most bytes a record of {@code capacity} entries has. */
    static int maximumBytes(int capacity) {
        return HEADER_BYTES + capacity * LINE_BYTES + TRAILER_BYTES;
    }

    /** One identifier's entry: its state, the digest of its content, and its parts (0 for a tombstone). */
    static final class Entry {
        /** 'p', 'h' or 'u'. */
        char state;
        final String digest;
        final int parts;

        Entry(char state, String digest, int parts) {
            this.state = state;
            this.digest = digest;
            this.parts = parts;
        }
    }

    /** The record fails verification. */
    static final class Damaged extends IOException {
        private static final long serialVersionUID = 1L;

        Damaged(String why) {
            super(why);
        }
    }

    private final RecordFiles files;
    private final int capacity;

    RecordStore(RecordFiles files, int capacity) {
        this.files = files;
        this.capacity = capacity;
    }

    int capacity() {
        return capacity;
    }

    /**
     * The record, oldest entry first; empty when there is none. A pending entry is returned as uncertain:
     * the request that wrote it ended without writing its end, and the caller holds the lock, so no other
     * request is running. Throws {@link Damaged} when the record fails verification, and an IOException
     * when it cannot be read.
     */
    LinkedHashMap<String, Entry> read() throws IOException {
        final LinkedHashMap<String, Entry> map = new LinkedHashMap<>();
        final RecordFiles.File file;
        try {
            file = files.openRead(RECORD);
        } catch (NoSuchFileException e) {
            return map;
        }
        final byte[] bytes;
        try {
            final long size = file.size();
            // Bounded before any buffer exists.
            if (size > maximumBytes(capacity)) throw new Damaged("too large: " + size + " bytes");
            bytes = new byte[(int) size];
            int have = 0;
            while (have < bytes.length) {
                final int n = file.read(bytes, have, bytes.length - have);
                if (n < 0) throw new Damaged("shorter than its size");
                have += n;
            }
        } finally {
            file.close();
        }
        return verify(bytes, map);
    }

    private LinkedHashMap<String, Entry> verify(byte[] bytes, LinkedHashMap<String, Entry> map) throws Damaged {
        for (byte b : bytes) {
            if (b < 0 || b > 0x7e) throw new Damaged("not ASCII");
        }
        if (bytes.length < HEADER_BYTES + TRAILER_BYTES) throw new Damaged("too short");
        final String text = new String(bytes, StandardCharsets.US_ASCII);
        final int end = text.length() - TRAILER_BYTES;
        final String trailer = text.substring(end);
        if (!TRAILER.matcher(trailer).matches()) throw new Damaged("no end line");
        final CRC32 crc = new CRC32();
        crc.update(bytes, 0, end);
        if (!trailer.equals(String.format("end %08x\n", crc.getValue()))) throw new Damaged("checksum");
        final String body = text.substring(0, end);
        if (!body.endsWith("\n")) throw new Damaged("last line unfinished");
        final String[] lines = body.split("\n", -1);
        if (!HEADER.equals(lines[0])) throw new Damaged("header");
        // lines ends with the empty string after the last line feed.
        if (lines.length - 2 > capacity) throw new Damaged("too many entries");
        for (int i = 1; i < lines.length - 1; ++i) {
            final String line = lines[i];
            if (!ENTRY.matcher(line).matches()) throw new Damaged("line " + i);
            final String identifier = line.substring(2, 34);
            final int parts = Integer.parseInt(line.substring(100));
            final char state = line.charAt(0);
            if (parts == 0 && state != 'u') throw new Damaged("no parts outside a tombstone");
            if (map.containsKey(identifier)) throw new Damaged("an identifier twice");
            map.put(identifier, new Entry(state == 'p' ? 'u' : state, line.substring(35, 99), parts));
        }
        return map;
    }

    /**
     * Replaces the record with {@code entries}: written whole to the next file, forced, renamed over the
     * record, and the directory forced. Every failure throws; a rename that happened before a later failure
     * stays.
     */
    void write(Map<String, Entry> entries) throws IOException {
        final StringBuilder out = new StringBuilder(HEADER).append('\n');
        for (Map.Entry<String, Entry> entry : entries.entrySet()) {
            final Entry value = entry.getValue();
            out.append(value.state).append(' ').append(entry.getKey()).append(' ').append(value.digest).append(' ')
                    .append(value.parts).append('\n');
        }
        final byte[] body = out.toString().getBytes(StandardCharsets.US_ASCII);
        final CRC32 crc = new CRC32();
        crc.update(body);
        final byte[] trailer = String.format("end %08x\n", crc.getValue()).getBytes(StandardCharsets.US_ASCII);
        final byte[] all = new byte[body.length + trailer.length];
        System.arraycopy(body, 0, all, 0, body.length);
        System.arraycopy(trailer, 0, all, body.length, trailer.length);
        final RecordFiles.File file = files.openWrite(NEXT);
        try {
            int done = 0;
            while (done < all.length) {
                final int n = file.write(all, done, all.length - done);
                if (n <= 0) throw new IOException("a write made no progress");
                done += n;
            }
            file.force();
        } finally {
            file.close();
        }
        files.move(NEXT, RECORD);
        files.forceDirectory();
    }

    /** Moves a record that failed verification aside, replacing the last one, and forces the directory. */
    void quarantine() throws IOException {
        files.move(RECORD, CORRUPT);
        files.forceDirectory();
    }
}
