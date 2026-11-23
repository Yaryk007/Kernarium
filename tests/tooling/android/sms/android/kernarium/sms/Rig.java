// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the record's rows share: a directory, stand-in steps that log their hand-overs apart from anything
// the record stores, and a RecordFiles that delegates to the real one while it records each call when it
// returns, writes a byte at a time on demand, runs a hook after the lock, or fails one named call.

package android.kernarium.sms;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

final class Rig {
    static final String X = "0123456789abcdef0123456789abcdef";
    static final String Y = "fedcba9876543210fedcba9876543210";

    static String id(int number) {
        return String.format("%032x", number);
    }

    /** A file in the directory that the stand-in steps append to: the hand-overs, counted apart from the record. */
    static final String HANDOVERS = "handovers.log";

    final Path dir;
    final List<String> log = Collections.synchronizedList(new ArrayList<>());
    /** How many times a step ran, and the planner. */
    final AtomicInteger stepRuns = new AtomicInteger();
    final AtomicInteger plans = new AtomicInteger();
    /** A step to run in place of a stand-in: (identifier, step number) to its behaviour, or null. */
    volatile StepHook hook;

    interface StepHook {
        void run(String identifier, int step) throws Exception;
    }

    Rig(Path dir) {
        this.dir = dir;
    }

    final Consumer<String> logger = line -> log.add(line);

    DeliveryRecord.Planner planner = null;

    /** The plan of two stand-in steps: the deliver broadcast, then the received one. */
    DeliveryRecord.Planner defaultPlanner() {
        return message -> {
            plans.incrementAndGet();
            final List<DeliveryRecord.Step> steps = new ArrayList<>();
            for (int n = 1; n <= 2; ++n) {
                final int step = n;
                steps.add(() -> {
                    stepRuns.incrementAndGet();
                    final StepHook h = hook;
                    if (h != null) h.run(message.identifier, step);
                    Files.write(dir.resolve(HANDOVERS),
                            ((step == 1 ? "deliver " : "received ") + message.identifier + "\n")
                                    .getBytes(StandardCharsets.US_ASCII),
                            StandardOpenOption.CREATE, StandardOpenOption.APPEND);
                });
            }
            return new DeliveryRecord.Plan(1, steps);
        };
    }

    DeliveryRecord record(RecordFiles files, int capacity) {
        return new DeliveryRecord(files, capacity, planner != null ? planner : defaultPlanner(), logger);
    }

    DeliveryRecord record() {
        return record(new RecordFiles.Nio(dir), RecordStore.CAPACITY);
    }

    static String answer(DeliveryRecord record, boolean retry, String id, String sender, String body) {
        return new String(record.handle(new Message(retry, id, sender, body)), StandardCharsets.US_ASCII);
    }

    /** The hand-overs (the first step's lines) of every identifier, or of one. */
    long handovers() throws IOException {
        return handovers(null);
    }

    long handovers(String identifier) throws IOException {
        final Path file = dir.resolve(HANDOVERS);
        if (!Files.exists(file)) return 0;
        long count = 0;
        for (String line : Files.readAllLines(file)) {
            if (line.startsWith("deliver ") && (identifier == null || line.equals("deliver " + identifier))) ++count;
        }
        return count;
    }

    /** The state letter of an identifier in the record file, or null. */
    Character stateOf(String identifier) throws IOException {
        final Path file = dir.resolve(RecordStore.RECORD);
        if (!Files.exists(file)) return null;
        for (String line : Files.readAllLines(file)) {
            if (line.length() > 34 && line.substring(2, 34).equals(identifier)) return line.charAt(0);
        }
        return null;
    }

    byte[] recordBytes() throws IOException {
        final Path file = dir.resolve(RecordStore.RECORD);
        return Files.exists(file) ? Files.readAllBytes(file) : null;
    }

    /** A valid record with the given lines, its CRC computed here. */
    static byte[] craft(String... lines) {
        final StringBuilder out = new StringBuilder(RecordStore.HEADER).append('\n');
        for (String line : lines) out.append(line).append('\n');
        final byte[] body = out.toString().getBytes(StandardCharsets.US_ASCII);
        final java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        crc.update(body);
        final byte[] trailer = String.format("end %08x\n", crc.getValue()).getBytes(StandardCharsets.US_ASCII);
        final byte[] all = new byte[body.length + trailer.length];
        System.arraycopy(body, 0, all, 0, body.length);
        System.arraycopy(trailer, 0, all, body.length, trailer.length);
        return all;
    }

    /** A record line for an identifier, with the digest of the given sender and text. */
    static String line(char state, String identifier, String sender, String body, int parts) {
        return state + " " + identifier + " " + DeliveryRecord.digest(sender, body) + " " + parts;
    }

    /** A RecordFiles that delegates and records, writes a byte at a time, hooks the lock, or fails a call. */
    static final class Recording implements RecordFiles {
        final RecordFiles inner;
        final List<String> ops = Collections.synchronizedList(new ArrayList<>());
        final AtomicInteger fired = new AtomicInteger();
        final AtomicInteger reads = new AtomicInteger();
        volatile int maxWrite = Integer.MAX_VALUE;
        volatile Runnable beforeLock = () -> { };
        volatile Runnable afterLock = () -> { };
        /** The call to fail: lock, size, read, write, force, move or forceDirectory; "" for none. */
        volatile String failOp = "";
        /** For a write: the bytes that go through before it fails. */
        volatile int failWriteAfter = 0;
        /** Whether the failure is live; a test may arm it from a step. */
        volatile boolean armed = true;
        private int written;

        Recording(RecordFiles inner) {
            this.inner = inner;
        }

        private void note(String op) {
            synchronized (ops) {
                final boolean repeated = !ops.isEmpty() && ops.get(ops.size() - 1).equals(op);
                if (!(repeated && (op.equals("write") || op.equals("read")))) ops.add(op);
            }
        }

        private void gate(String op) throws IOException {
            if (op.equals(failOp) && armed) {
                fired.incrementAndGet();
                ops.add("failed " + op);
                throw new IOException("injected " + op + " failure");
            }
        }

        @Override
        public Lock lock() throws IOException {
            beforeLock.run();
            gate("lock");
            final Lock lock = inner.lock();
            note("lock");
            afterLock.run();
            return () -> {
                lock.release();
                note("unlock");
            };
        }

        @Override
        public File openRead(String name) throws IOException {
            final File file = inner.openRead(name);
            return new File() {
                @Override
                public long size() throws IOException {
                    gate("size");
                    final long size = file.size();
                    note("size");
                    return size;
                }

                @Override
                public int read(byte[] buffer, int offset, int length) throws IOException {
                    gate("read");
                    reads.incrementAndGet();
                    final int n = file.read(buffer, offset, length);
                    note("read");
                    return n;
                }

                @Override
                public int write(byte[] buffer, int offset, int length) throws IOException {
                    throw new IOException("read only");
                }

                @Override
                public void force() throws IOException {
                    throw new IOException("read only");
                }

                @Override
                public void close() throws IOException {
                    file.close();
                }
            };
        }

        @Override
        public File openWrite(String name) throws IOException {
            final File file = inner.openWrite(name);
            written = 0;
            return new File() {
                @Override
                public long size() throws IOException {
                    return file.size();
                }

                @Override
                public int read(byte[] buffer, int offset, int length) throws IOException {
                    throw new IOException("write only");
                }

                @Override
                public int write(byte[] buffer, int offset, int length) throws IOException {
                    if ("write".equals(failOp) && armed && written >= failWriteAfter) gate("write");
                    int count = Math.min(length, maxWrite);
                    if ("write".equals(failOp) && armed) count = Math.min(count, failWriteAfter - written);
                    final int n = file.write(buffer, offset, count);
                    written += n;
                    note("write");
                    return n;
                }

                @Override
                public void force() throws IOException {
                    gate("force");
                    file.force();
                    note("force");
                }

                @Override
                public void close() throws IOException {
                    file.close();
                }
            };
        }

        @Override
        public void move(String from, String to) throws IOException {
            gate("move");
            inner.move(from, to);
            note("move");
        }

        @Override
        public void forceDirectory() throws IOException {
            gate("forceDirectory");
            inner.forceDirectory();
            note("forceDirectory");
        }
    }
}
