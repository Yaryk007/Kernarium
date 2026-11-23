// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.function.Consumer;

/**
 * Hands a message to Android at most once, whatever the process or the host does, and answers a request
 * for an identifier from what it recorded about it. The guarantee is that no identifier is handed over
 * twice: when the record cannot say whether an earlier attempt handed a message over, the answer is
 * uncertain and nothing is handed over. A rare loss is accepted.
 *
 * <p>A request runs under one monitor and the record's exclusive file lock, from the read of the record to
 * the final write:
 * <ol>
 * <li>an identifier the record does not hold, and a first attempt that passes the rules and the plan, is
 * written as pending, durably, before the first step runs;
 * <li>a retry never hands over: for an identifier the record does not hold it writes a tombstone, an
 * uncertain entry with no parts, so that a late first attempt finds the identifier closed;
 * <li>the final state, handed over or uncertain, is written after the last step.
 * </ol>
 * A failed final write never runs a step again and never turns a hand-over into "not handed over".
 */
final class DeliveryRecord {
    /** One side effect of a hand-over. It throws when it did not act or failed to. */
    interface Step {
        void run() throws Exception;
    }

    /** What a first attempt will do: its steps, in order, and how many parts the message takes. */
    static final class Plan {
        final int parts;
        final List<Step> steps;

        Plan(int parts, List<Step> steps) {
            this.parts = parts;
            this.steps = steps;
        }
    }

    /** Finds what a hand-over takes without acting. */
    interface Planner {
        Plan plan(Message message) throws Exception;
    }

    private final Object monitor = new Object();
    private final RecordFiles files;
    private final RecordStore store;
    private final Planner planner;
    private final Consumer<String> log;

    DeliveryRecord(RecordFiles files, int capacity, Planner planner, Consumer<String> log) {
        this.files = files;
        this.store = new RecordStore(files, capacity);
        this.planner = planner;
        this.log = log;
    }

    /**
     * Answers one request. Every failure of the store fails closed: a first attempt is answered "not handed
     * over" and a retry "unsettled", and no step runs.
     */
    byte[] handle(Message message) {
        synchronized (monitor) {
            final RecordFiles.Lock lock;
            try {
                lock = files.lock();
            } catch (IOException | RuntimeException e) {
                return storeFailed(message, "lock", e);
            }
            try {
                return locked(message);
            } finally {
                try {
                    lock.release();
                } catch (IOException | RuntimeException e) {
                    log.accept("record: the lock was not released: " + e.getClass().getSimpleName());
                }
            }
        }
    }

    private byte[] locked(Message message) {
        final String id = message.identifier;
        final LinkedHashMap<String, RecordStore.Entry> map;
        try {
            map = store.read();
        } catch (RecordStore.Damaged e) {
            log.accept("record: damaged (" + e.getMessage() + "), moved aside");
            try {
                store.quarantine();
            } catch (IOException | RuntimeException failure) {
                log.accept("record: not moved aside: " + failure.getClass().getSimpleName());
            }
            return storeAnswer(message);
        } catch (IOException | RuntimeException e) {
            return storeFailed(message, "read", e);
        }
        final String digest = digest(message.sender, message.body);
        final RecordStore.Entry known = map.get(id);
        if (known != null) {
            if (!known.digest.equals(digest)) return Answer.uncertain(id, Answer.CONFLICT);
            if (known.state == 'h') return Answer.handedOverEarlier(id, known.parts);
            return Answer.uncertain(id, known.parts == 0 ? Answer.UNKNOWN : Answer.INTERRUPTED);
        }
        if (message.retry) {
            // A retry never hands over; it closes the identifier, so that a first attempt arriving late finds it so.
            evict(map);
            map.put(id, new RecordStore.Entry('u', digest, 0));
            try {
                store.write(map);
            } catch (IOException | RuntimeException e) {
                return storeFailed(message, "tombstone", e);
            }
            return Answer.uncertain(id, Answer.UNKNOWN);
        }
        if (message.broken() != null) return Answer.notHandedOver(id, Answer.INVALID);
        final Plan plan;
        try {
            plan = planner.plan(message);
        } catch (Exception e) {
            log.accept("record: no plan for " + id.substring(0, 8) + ": " + e.getClass().getSimpleName());
            return Answer.notHandedOver(id, Answer.PLAN);
        }
        evict(map);
        final RecordStore.Entry entry = new RecordStore.Entry('p', digest, plan.parts);
        map.put(id, entry);
        try {
            store.write(map);
        } catch (IOException | RuntimeException e) {
            return storeFailed(message, "pending", e);
        }
        boolean allReturned = true;
        int number = 0;
        for (Step step : plan.steps) {
            // The marker of a hand-over: written before the step acts.
            log.accept("step " + (++number) + " of " + id.substring(0, 8));
            try {
                step.run();
            } catch (Throwable e) {
                log.accept("step " + number + " of " + id.substring(0, 8) + " failed: " + e.getClass().getSimpleName());
                allReturned = false;
                break;
            }
        }
        entry.state = allReturned ? 'h' : 'u';
        try {
            store.write(map);
        } catch (IOException | RuntimeException e) {
            // The entry stays pending, which a later request reads as uncertain; the answer is what the steps earned.
            log.accept("record: the final state was not written: " + e.getClass().getSimpleName());
        }
        return allReturned ? Answer.handedOver(id, plan.parts) : Answer.uncertain(id, Answer.INTERRUPTED);
    }

    /** Drops the oldest entries until there is room for one more. */
    private void evict(LinkedHashMap<String, RecordStore.Entry> map) {
        while (map.size() >= store.capacity()) map.remove(map.keySet().iterator().next());
    }

    private byte[] storeFailed(Message message, String what, Exception e) {
        log.accept("record: " + what + " failed: " + e.getClass().getSimpleName());
        return storeAnswer(message);
    }

    private static byte[] storeAnswer(Message message) {
        return message.retry ? Answer.unsettled(message.identifier) : Answer.notHandedOver(message.identifier, Answer.STORE);
    }

    /**
     * The SHA-256 of the sender and the text, each with its UTF-8 length before it, as 64 lower-case hex
     * digits. The record keeps no text; a short code could be recovered from the digest by trying them all,
     * so the record is as private as the message.
     */
    static String digest(String sender, String text) {
        final MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
        final byte[] s = sender.getBytes(StandardCharsets.UTF_8);
        final byte[] t = text.getBytes(StandardCharsets.UTF_8);
        sha.update("kernarium-sms-digest 1\0".getBytes(StandardCharsets.US_ASCII));
        sha.update(ByteBuffer.allocate(4).putInt(s.length).array());
        sha.update(s);
        sha.update(ByteBuffer.allocate(4).putInt(t.length).array());
        sha.update(t);
        final StringBuilder hex = new StringBuilder();
        for (byte b : sha.digest()) hex.append(String.format("%02x", b));
        return hex.toString();
    }
}
