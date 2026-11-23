// SPDX-License-Identifier: GPL-2.0-or-later
//
// A child JVM of RecordTest: it runs one request against a directory and ends the process (Runtime.halt)
// at a named point, or holds the record's lock inside its first step until the parent says so. No hook
// exists in the service: the points are in this file's RecordFiles, planner and steps.
//
//   RecordChild <directory> <point or none> <release file or -> <first|retry>
//
// Points: parsed, locked, read, planned, pending-written, pending-renamed, pending-forced, after-step-1,
// after-step-2, final-written, final-renamed, final-forced, answered. The first update of the record is
// the "pending" one (a tombstone is one too), the second the "final" one.

package android.kernarium.sms;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;

public final class RecordChild {
    static final String X = Rig.X;
    private static String haltAt = "none";

    private RecordChild() {}

    private static void point(String name) {
        if (name.equals(haltAt)) Runtime.getRuntime().halt(137);
    }

    /** The real files, with a point after each call that a request ends at. */
    private static final class Halting implements RecordFiles {
        private final RecordFiles inner;
        private final Path dir;
        private int updates;

        Halting(Path dir) {
            this.dir = dir;
            this.inner = new RecordFiles.Nio(dir);
        }

        private String update() {
            return updates <= 1 ? "pending" : "final";
        }

        @Override
        public Lock lock() throws IOException {
            final Lock lock = inner.lock();
            point("locked");
            return () -> {
                // Inside the critical section, just before the lock is let go.
                Files.write(dir.resolve("unlocking"), new byte[0]);
                lock.release();
            };
        }

        @Override
        public File openRead(String name) throws IOException {
            final File file;
            try {
                file = inner.openRead(name);
            } catch (NoSuchFileException e) {
                point("read");
                throw e;
            }
            return new File() {
                @Override
                public long size() throws IOException {
                    return file.size();
                }

                @Override
                public int read(byte[] buffer, int offset, int length) throws IOException {
                    return file.read(buffer, offset, length);
                }

                @Override
                public int write(byte[] buffer, int offset, int length) throws IOException {
                    return file.write(buffer, offset, length);
                }

                @Override
                public void force() throws IOException {
                    file.force();
                }

                @Override
                public void close() throws IOException {
                    file.close();
                    point("read");
                }
            };
        }

        @Override
        public File openWrite(String name) throws IOException {
            ++updates;
            final File file = inner.openWrite(name);
            return new File() {
                @Override
                public long size() throws IOException {
                    return file.size();
                }

                @Override
                public int read(byte[] buffer, int offset, int length) throws IOException {
                    return file.read(buffer, offset, length);
                }

                @Override
                public int write(byte[] buffer, int offset, int length) throws IOException {
                    return file.write(buffer, offset, length);
                }

                @Override
                public void force() throws IOException {
                    // Every byte is in the file, and none is forced.
                    point(update() + "-written");
                    file.force();
                }

                @Override
                public void close() throws IOException {
                    file.close();
                }
            };
        }

        @Override
        public void move(String from, String to) throws IOException {
            inner.move(from, to);
            point(update() + "-renamed");
        }

        @Override
        public void forceDirectory() throws IOException {
            inner.forceDirectory();
            point(update() + "-forced");
        }
    }

    public static void main(String[] args) throws Exception {
        final Path dir = Paths.get(args[0]);
        haltAt = args[1];
        final Path waitFor = args[2].equals("-") ? null : Paths.get(args[2]);
        final boolean retry = args[3].equals("retry");
        final DeliveryRecord.Planner planner = message -> {
            final List<DeliveryRecord.Step> steps = new ArrayList<>();
            steps.add(() -> {
                if (waitFor != null) {
                    Files.write(dir.resolve("inside"), new byte[0]);
                    final long deadline = System.nanoTime() + 30_000_000_000L;
                    while (!Files.exists(waitFor) && System.nanoTime() < deadline) Thread.sleep(5);
                }
                Files.write(dir.resolve(Rig.HANDOVERS), ("deliver " + message.identifier + "\n")
                        .getBytes(StandardCharsets.US_ASCII), StandardOpenOption.CREATE, StandardOpenOption.APPEND);
                point("after-step-1");
            });
            steps.add(() -> {
                Files.write(dir.resolve(Rig.HANDOVERS), ("received " + message.identifier + "\n")
                        .getBytes(StandardCharsets.US_ASCII), StandardOpenOption.CREATE, StandardOpenOption.APPEND);
                point("after-step-2");
            });
            point("planned");
            return new DeliveryRecord.Plan(1, steps);
        };
        final DeliveryRecord record = new DeliveryRecord(new Halting(dir), RecordStore.CAPACITY, planner, line -> { });
        point("parsed");
        final byte[] answer = record.handle(new Message(retry, X, "Bank", "hello"));
        point("answered");
        Files.write(dir.resolve("child-answer"), answer);
    }
}
