// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

/**
 * The entry point of a request: the payload of an admitted, complete frame in, the payload of the answer
 * out. It checks no caller of its own; the channel admits only the host (Server).
 *
 * <p>The answer is empty only for a payload that is not a version 2 request, which touches nothing: the
 * channel sends it as the zero-length answer, which the host reads as "not taken". Every other answer has 1
 * to 58 bytes. Nothing the record does is thrown from here: a step that fails is an uncertain answer, and a
 * store that fails closes the request. An exception that escaped anyway would be a defect, and the channel
 * would close the connection without an answer, which the host keeps the identifier for.
 */
final class DeliveryService implements Server.Handler {
    private final DeliveryRecord record;

    DeliveryService(DeliveryRecord record) {
        this.record = record;
    }

    byte[] answer(byte[] payload) {
        final Message.Parsed parsed = Message.parse(payload);
        switch (parsed.kind) {
            case REQUEST:
                return record.handle(parsed.message);
            case REFUSED_VERSION:
                return Answer.refusedVersion();
            default:
                return Answer.EMPTY;
        }
    }

    @Override
    public byte[] handle(byte[] payload) {
        return answer(payload);
    }
}
