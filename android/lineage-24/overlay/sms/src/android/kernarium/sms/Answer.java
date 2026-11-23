// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.nio.charset.StandardCharsets;

/**
 * The forms of an answer to a request: printable ASCII, fields joined by dots, the version first. The
 * longest is 58 bytes, {@code 2.not-handed-over.<identifier>.invalid}.
 */
final class Answer {
    /** The answer to a payload that is not a request: no bytes. The channel sends it as "not taken". */
    static final byte[] EMPTY = new byte[0];

    static final String INVALID = "invalid";
    static final String PLAN = "plan";
    static final String STORE = "store";
    static final String INTERRUPTED = "interrupted";
    static final String UNKNOWN = "unknown";
    static final String CONFLICT = "conflict";

    private Answer() {}

    /** This request handed the message to Android: every step returned. */
    static byte[] handedOver(String identifier, int parts) {
        return form("handed-over", identifier, Integer.toString(parts));
    }

    /** An earlier request with this identifier and content handed it over; this one handed nothing over. */
    static byte[] handedOverEarlier(String identifier, int parts) {
        return form("handed-over-earlier", identifier, Integer.toString(parts));
    }

    /** A first attempt's: nothing under this identifier was handed over, and nothing will be by this request. */
    static byte[] notHandedOver(String identifier, String reason) {
        return form("not-handed-over", identifier, reason);
    }

    /** The identifier is closed and will not be handed over again; its attempt may have acted. */
    static byte[] uncertain(String identifier, String reason) {
        return form("uncertain", identifier, reason);
    }

    /** A retry's: the record could not close the identifier durably, and handed nothing over. */
    static byte[] unsettled(String identifier) {
        return form("unsettled", identifier, STORE);
    }

    /** The answer to a request of another version: the service's own, whatever the request's. */
    static byte[] refusedVersion() {
        return (Message.VERSION + ".refused-version").getBytes(StandardCharsets.US_ASCII);
    }

    private static byte[] form(String outcome, String identifier, String detail) {
        return (Message.VERSION + "." + outcome + "." + identifier + "." + detail).getBytes(StandardCharsets.US_ASCII);
    }
}
