// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.TimeZone;

/**
 * The 3GPP TS 23.040 SMS-DELIVER PDUs a network would send for a message: UCS-2
 * text, one PDU when it fits 70 units and otherwise concatenated parts of 67,
 * never splitting a surrogate pair. Each PDU starts with an empty service
 * centre address, as the "3gpp" format of SmsMessage.createFromPdu expects.
 *
 * <p>Android decodes the user data of each PDU as "UTF-16", which reads a
 * leading byte order mark and drops it. A part that starts with U+FEFF would
 * lose that character, so such a part carries a second U+FEFF in front, which
 * the decoder takes as the mark; that unit comes out of the part's 67. U+FFFE
 * cannot be sent at all: at the start of a part the decoder reads it as the
 * reversed mark and swaps every byte after it, and elsewhere the JDK's
 * "UTF-16" decoder, which the host test reads the PDUs back with, takes it as
 * a malformed unit and replaces it with U+FFFD; what Android's own decoder
 * does with it there is not established. Message refuses it everywhere.
 */
final class DeliverPdu {
    private static final int SINGLE_UNITS = 70;
    private static final int PART_UNITS = 67;
    private static final char BYTE_ORDER_MARK = '\uFEFF';
    private static final char REVERSED_MARK = '\uFFFE';

    private DeliverPdu() {}

    static byte[][] encode(String sender, String body, long millis, TimeZone zone, int reference) {
        if (body.isEmpty()) throw new IllegalArgumentException("no text");
        if (body.indexOf(REVERSED_MARK) >= 0) throw new IllegalArgumentException("U+FFFE cannot be sent");
        final List<String> parts = split(body);
        if (parts.size() > Message.MAXIMUM_PARTS) throw new IllegalArgumentException("too many parts");
        final byte[][] pdus = new byte[parts.size()][];
        for (int i = 0; i < parts.size(); ++i) {
            pdus[i] = pdu(sender, parts.get(i), millis, zone,
                    parts.size() == 1 ? null : new int[] {reference & 0xff, parts.size(), i + 1});
        }
        return pdus;
    }

    /** The user data of each part, as the decoder will read it: a leading U+FEFF is doubled. */
    static List<String> split(String body) {
        final List<String> parts = new ArrayList<>();
        if (body.length() + markUnits(body, 0) <= SINGLE_UNITS) {
            parts.add(onWire(body));
            return parts;
        }
        int start = 0;
        while (start < body.length()) {
            final int budget = PART_UNITS - markUnits(body, start);
            int end = Math.min(start + budget, body.length());
            if (end < body.length() && Character.isHighSurrogate(body.charAt(end - 1))) --end;
            parts.add(onWire(body.substring(start, end)));
            start = end;
        }
        return parts;
    }

    /** How many units the part that starts at {@code start} adds for the decoder. */
    private static int markUnits(String body, int start) {
        return body.charAt(start) == BYTE_ORDER_MARK ? 1 : 0;
    }

    private static String onWire(String text) {
        return text.charAt(0) == BYTE_ORDER_MARK ? BYTE_ORDER_MARK + text : text;
    }

    private static byte[] pdu(String sender, String text, long millis, TimeZone zone, int[] concat) {
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(0x00); // no service centre address
        // SMS-DELIVER, no more messages to send, user data header when concatenated.
        out.write(0x04 | (concat != null ? 0x40 : 0));
        address(out, sender);
        out.write(0x00); // protocol identifier
        out.write(0x08); // UCS-2
        timestamp(out, millis, zone);
        final byte[] units = text.getBytes(java.nio.charset.StandardCharsets.UTF_16BE);
        if (concat == null) {
            out.write(units.length);
        } else {
            out.write(6 + units.length);
            out.write(0x05); // header length
            out.write(0x00); // concatenated short message, 8-bit reference
            out.write(0x03);
            out.write(concat[0]);
            out.write(concat[1]);
            out.write(concat[2]);
        }
        out.write(units, 0, units.length);
        return out.toByteArray();
    }

    private static void address(ByteArrayOutputStream out, String sender) {
        if (Message.NUMBER.matcher(sender).matches()) {
            final boolean international = sender.startsWith("+");
            final String digits = international ? sender.substring(1) : sender;
            out.write(digits.length());
            out.write(international ? 0x91 : 0x81);
            for (int i = 0; i < digits.length(); i += 2) {
                final int low = digits.charAt(i) - '0';
                final int high = i + 1 < digits.length() ? digits.charAt(i + 1) - '0' : 0x0f;
                out.write((high << 4) | low);
            }
            return;
        }
        // Alphanumeric: letters, digits and space have the same code in the
        // GSM default alphabet as in ASCII.
        final byte[] packed = pack(sender);
        out.write((sender.length() * 7 + 3) / 4); // useful semi-octets
        out.write(0xd0);
        out.write(packed, 0, packed.length);
    }

    private static byte[] pack(String septets) {
        final byte[] packed = new byte[(septets.length() * 7 + 7) / 8];
        int bit = 0;
        for (int i = 0; i < septets.length(); ++i) {
            final int value = septets.charAt(i) & 0x7f;
            final int index = bit / 8;
            final int shift = bit % 8;
            packed[index] |= (byte) (value << shift);
            if (shift > 1) packed[index + 1] |= (byte) (value >> (8 - shift));
            bit += 7;
        }
        return packed;
    }

    private static void timestamp(ByteArrayOutputStream out, long millis, TimeZone zone) {
        final java.util.Calendar time = java.util.Calendar.getInstance(zone);
        time.setTimeInMillis(millis);
        out.write(bcd(time.get(java.util.Calendar.YEAR) % 100));
        out.write(bcd(time.get(java.util.Calendar.MONTH) + 1));
        out.write(bcd(time.get(java.util.Calendar.DAY_OF_MONTH)));
        out.write(bcd(time.get(java.util.Calendar.HOUR_OF_DAY)));
        out.write(bcd(time.get(java.util.Calendar.MINUTE)));
        out.write(bcd(time.get(java.util.Calendar.SECOND)));
        final int quarters = zone.getOffset(millis) / (15 * 60 * 1000);
        out.write(bcd(Math.abs(quarters)) | (quarters < 0 ? 0x08 : 0));
    }

    /** Two decimal digits, the units digit in the high nibble. */
    private static int bcd(int value) {
        return ((value % 10) << 4) | (value / 10);
    }
}
