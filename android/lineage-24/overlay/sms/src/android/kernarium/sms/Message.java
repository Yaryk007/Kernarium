// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Base64;
import java.util.regex.Pattern;

/**
 * One request from the host: the attempt (a first one or a retry), the identifier the host chose for the
 * message, its sender and its text. The request is the payload of a frame, printable ASCII, fields joined
 * by dots: {@code 2.<first|retry>.<identifier>.<sender64>.<text64>}, the sender and the text each canonical
 * padded base64 of strict UTF-8, so that no character of either can break the form.
 *
 * <p>{@link #parse} only reads the form. What a sender and a text may be is {@link #broken()}, which the
 * record applies to a first attempt alone.
 */
final class Message {
    /** The version of the payload, independent of the frame's. */
    static final int VERSION = 2;
    /** A number of at most 20 digits, international when it starts with +. */
    static final Pattern NUMBER = Pattern.compile("\\+?[0-9]{1,20}");
    /** A name of at most 11 letters, digits and spaces, as a network sends one. */
    static final Pattern NAME = Pattern.compile("[A-Za-z0-9 ]{1,11}");
    /** The longest text, in UTF-16 units. */
    static final int MAXIMUM_UNITS = 670;
    /**
     * The most parts a text of {@link #MAXIMUM_UNITS} takes. A part holds 67
     * units; DeliverPdu gives one back for the U+FEFF it puts before a part
     * that starts with one, and a part ends a unit early rather than split a
     * surrogate pair. So every part but the last takes at least 65 units of
     * the text, and 670 units take at most 11 parts. The host counts the
     * same (GuestSms.maximumParts).
     */
    static final int MAXIMUM_PARTS = 11;
    /** The identifier: 128 bits the host drew, in lower-case hex. */
    static final Pattern IDENTIFIER = Pattern.compile("[0-9a-f]{32}");
    private static final Pattern VERSION_FIELD = Pattern.compile("0|[1-9][0-9]{0,8}");

    /** What a payload is. */
    enum Kind {
        /** A version 2 request, whole and well formed. */
        REQUEST,
        /** A request whose first field is another version: the service answers with its own. */
        REFUSED_VERSION,
        /** Anything else: nothing is touched and the answer is empty. */
        NOT_A_REQUEST
    }

    /** What {@link #parse} made of a payload; {@link #message} is set for a request alone. */
    static final class Parsed {
        final Kind kind;
        final Message message;

        private Parsed(Kind kind, Message message) {
            this.kind = kind;
            this.message = message;
        }
    }

    private static final Parsed NOT_A_REQUEST = new Parsed(Kind.NOT_A_REQUEST, null);
    private static final Parsed REFUSED_VERSION = new Parsed(Kind.REFUSED_VERSION, null);

    /** True for a retry: an earlier request may have handed the identifier over, and this one never does. */
    final boolean retry;
    final String identifier;
    final String sender;
    final String body;

    Message(boolean retry, String identifier, String sender, String body) {
        this.retry = retry;
        this.identifier = identifier;
        this.sender = sender;
        this.body = body;
    }

    static Parsed parse(byte[] payload) {
        if (payload == null) return NOT_A_REQUEST;
        for (byte b : payload) {
            if (b < 0x21 || b > 0x7e) return NOT_A_REQUEST;
        }
        final String[] fields = new String(payload, StandardCharsets.US_ASCII).split("\\.", -1);
        if (!VERSION_FIELD.matcher(fields[0]).matches()) return NOT_A_REQUEST;
        if (Integer.parseInt(fields[0]) != VERSION) return REFUSED_VERSION;
        if (fields.length != 5) return NOT_A_REQUEST;
        final boolean retry;
        if (fields[1].equals("first")) {
            retry = false;
        } else if (fields[1].equals("retry")) {
            retry = true;
        } else {
            return NOT_A_REQUEST;
        }
        if (!IDENTIFIER.matcher(fields[2]).matches()) return NOT_A_REQUEST;
        final String sender = decode(fields[3]);
        final String body = decode(fields[4]);
        if (sender == null || body == null) return NOT_A_REQUEST;
        return new Parsed(Kind.REQUEST, new Message(retry, fields[2], sender, body));
    }

    /**
     * Why this message may not be delivered, or null when it may: the sender is a number or a name, and the
     * text has 1 to {@link #MAXIMUM_UNITS} UTF-16 units and no U+FFFE.
     */
    String broken() {
        if (!NUMBER.matcher(sender).matches() && !NAME.matcher(sender).matches() || sender.trim().isEmpty()) {
            return "not a phone number or a sender name";
        }
        if (body.isEmpty() || body.length() > MAXIMUM_UNITS) {
            return "the text must have 1 to " + MAXIMUM_UNITS + " characters";
        }
        // Android's SMS code cannot return U+FFFE (see DeliverPdu).
        if (body.indexOf('￾') >= 0) return "the text has U+FFFE";
        return null;
    }

    /** The field's bytes as a string, or null unless it is non-empty canonical base64 of strict UTF-8. */
    private static String decode(String field) {
        if (field.isEmpty()) return null;
        final byte[] bytes;
        try {
            bytes = Base64.getDecoder().decode(field);
        } catch (IllegalArgumentException e) {
            return null;
        }
        // Java's decoder takes unpadded input and non-zero pad bits: the canonical form is the one it makes.
        if (!Base64.getEncoder().encodeToString(bytes).equals(field)) return null;
        final String text = new String(bytes, StandardCharsets.UTF_8);
        // A malformed or overlong sequence, or an encoded surrogate, decodes to U+FFFD and encodes differently.
        if (!Arrays.equals(text.getBytes(StandardCharsets.UTF_8), bytes)) return null;
        return text;
    }
}
