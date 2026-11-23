// SPDX-License-Identifier: GPL-2.0-or-later
//
// The delivery record (DeliveryRecord over RecordStore over RecordFiles) on a plain JVM: that no identifier
// is handed over twice whatever ends, overlaps or fails, with stand-in steps that log each hand-over apart
// from anything the record stores. The rows are R1 to R24 of the record's design; each one asserts what it
// observes, and the runner exits non-zero when any check fails. A child JVM (RecordChild) is ended at each
// point a request can end at, and holds the lock for the two-process row. test_sms_service.py runs it.

package android.kernarium.sms;

import static android.kernarium.sms.Checks.check;
import static android.kernarium.sms.Checks.same;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.stream.Stream;

public final class RecordTest {
    private static final String X = Rig.X;
    private static final String Y = Rig.Y;
    private static Path root;
    private static int rows;

    private RecordTest() {}

    private static Path fresh(String name) throws IOException {
        ++rows;
        return Files.createDirectory(root.resolve(name));
    }

    private static String handed(String id, int parts) {
        return "2.handed-over." + id + "." + parts;
    }

    private static String earlier(String id, int parts) {
        return "2.handed-over-earlier." + id + "." + parts;
    }

    private static String uncertain(String id, String reason) {
        return "2.uncertain." + id + "." + reason;
    }

    private static String notHanded(String id, String reason) {
        return "2.not-handed-over." + id + "." + reason;
    }

    private static String unsettled(String id) {
        return "2.unsettled." + id + ".store";
    }

    private static boolean waitUntil(BooleanSupplier condition, long millis) throws InterruptedException {
        final long deadline = System.nanoTime() + millis * 1_000_000L;
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(2);
        return condition.getAsBoolean();
    }

    private static Process child(Path dir, String point, Path release, String attempt) throws IOException {
        final String java = Paths.get(System.getProperty("java.home"), "bin", "java").toString();
        return new ProcessBuilder(java, "-cp", System.getProperty("java.class.path"),
                "android.kernarium.sms.RecordChild", dir.toString(), point, release == null ? "-" : release.toString(),
                attempt).inheritIO().start();
    }

    /** R1: a first attempt ended at each of the 13 points; then a retry and a repeated first attempt. */
    private static void endingPoints() throws Exception {
        // point, the retry's answer, hand-overs then, the repeated first attempt's answer, hand-overs then, X on disk
        final String[][] table = {
            {"parsed", uncertain(X, "unknown"), "0", uncertain(X, "unknown"), "0", "u"},
            {"locked", uncertain(X, "unknown"), "0", uncertain(X, "unknown"), "0", "u"},
            {"read", uncertain(X, "unknown"), "0", uncertain(X, "unknown"), "0", "u"},
            {"planned", uncertain(X, "unknown"), "0", uncertain(X, "unknown"), "0", "u"},
            {"pending-written", uncertain(X, "unknown"), "0", uncertain(X, "unknown"), "0", "u"},
            {"pending-renamed", uncertain(X, "interrupted"), "0", uncertain(X, "interrupted"), "0", "p"},
            {"pending-forced", uncertain(X, "interrupted"), "0", uncertain(X, "interrupted"), "0", "p"},
            {"after-step-1", uncertain(X, "interrupted"), "1", uncertain(X, "interrupted"), "1", "p"},
            {"after-step-2", uncertain(X, "interrupted"), "1", uncertain(X, "interrupted"), "1", "p"},
            {"final-written", uncertain(X, "interrupted"), "1", uncertain(X, "interrupted"), "1", "p"},
            {"final-renamed", earlier(X, 1), "1", earlier(X, 1), "1", "h"},
            {"final-forced", earlier(X, 1), "1", earlier(X, 1), "1", "h"},
            {"answered", earlier(X, 1), "1", earlier(X, 1), "1", "h"},
        };
        for (String[] row : table) {
            final Path dir = fresh("halt-" + row[0]);
            final int exit = child(dir, row[0], null, "first").waitFor();
            check(exit == 137, row[0] + ": the child was ended, exit " + exit);
            final Rig rig = new Rig(dir);
            final DeliveryRecord record = rig.record();
            final String retry = Rig.answer(record, true, X, "Bank", "hello");
            same(row[1], retry, row[0] + ": the retry");
            same(Long.parseLong(row[2]), rig.handovers(), row[0] + ": hand-overs after the retry");
            final String again = Rig.answer(record, false, X, "Bank", "hello");
            same(row[3], again, row[0] + ": the repeated first attempt");
            same(Long.parseLong(row[4]), rig.handovers(), row[0] + ": hand-overs after it");
            same(row[5].charAt(0), rig.stateOf(X) == null ? '-' : rig.stateOf(X), row[0] + ": X on disk");
            check(rig.stepRuns.get() == 0, row[0] + ": the retry and the repeat ran no step");
        }
    }

    /** R2: a new store over the same directory answers from the record. */
    private static void newStore() throws Exception {
        final Rig rig = new Rig(fresh("new-store"));
        same(handed(X, 1), Rig.answer(rig.record(), false, X, "Bank", "hello"), "the first attempt");
        final DeliveryRecord next = rig.record();
        same(earlier(X, 1), Rig.answer(next, true, X, "Bank", "hello"), "the retry from a new store");
        same(earlier(X, 1), Rig.answer(next, false, X, "Bank", "hello"), "the first attempt again");
        same(1L, rig.handovers(), "one hand-over");
    }

    /** R3: another content under the identifier changes nothing. */
    private static void otherContent() throws Exception {
        final Rig rig = new Rig(fresh("conflict"));
        final DeliveryRecord record = rig.record();
        Rig.answer(record, false, X, "Bank", "hello");
        final byte[] before = rig.recordBytes();
        same(uncertain(X, "conflict"), Rig.answer(record, true, X, "Bank", "hellO"), "a retry with another text");
        same(uncertain(X, "conflict"), Rig.answer(record, false, X, "Bonk", "hello"), "a first attempt with another sender");
        check(Arrays.equals(before, rig.recordBytes()), "the record is byte for byte as it was");
        same(earlier(X, 1), Rig.answer(rig.record(), true, X, "Bank", "hello"), "the same content, from a new store");
        same(1L, rig.handovers(), "one hand-over");
    }

    /** R4: requests in one process wait for each other, so nothing is handed over twice. */
    private static void overlapInProcess() throws Exception {
        final Rig rig = new Rig(fresh("overlap"));
        final CountDownLatch inside = new CountDownLatch(1);
        final CountDownLatch release = new CountDownLatch(1);
        rig.hook = (id, step) -> {
            if (id.equals(X) && step == 1) {
                inside.countDown();
                if (!release.await(30, TimeUnit.SECONDS)) throw new IllegalStateException("never released");
            }
        };
        final DeliveryRecord record = rig.record();
        final AtomicReference<String> a = new AtomicReference<>();
        final AtomicReference<String> b = new AtomicReference<>();
        final AtomicReference<String> c = new AtomicReference<>();
        final Thread ta = new Thread(() -> a.set(Rig.answer(record, false, X, "Bank", "hello")));
        ta.start();
        check(inside.await(20, TimeUnit.SECONDS), "A is inside its hand-over");
        final Thread tb = new Thread(() -> b.set(Rig.answer(record, true, X, "Bank", "hello")));
        final Thread tc = new Thread(() -> c.set(Rig.answer(record, false, Y, "Bank", "other")));
        tb.start();
        tc.start();
        final boolean held = waitUntil(() -> tb.getState() == Thread.State.BLOCKED && tc.getState() == Thread.State.BLOCKED
                || tb.getState() == Thread.State.TERMINATED || tc.getState() == Thread.State.TERMINATED, 10_000);
        check(held, "B and C reached a wait or an end");
        check(tb.getState() != Thread.State.TERMINATED, "B was held while A was being handed over");
        check(tc.getState() != Thread.State.TERMINATED, "C was held while A was being handed over");
        release.countDown();
        ta.join(30_000);
        tb.join(30_000);
        tc.join(30_000);
        same(handed(X, 1), a.get(), "A");
        same(earlier(X, 1), b.get(), "B, the retry of X");
        same(handed(Y, 1), c.get(), "C, another message");
        same(1L, rig.handovers(X), "X handed over once");
        same(1L, rig.handovers(Y), "Y handed over once");
    }

    /** R5: another process holds the lock until the parent says so, and the parent's retry runs after it. */
    private static void twoProcesses() throws Exception {
        final Path dir = fresh("two-processes");
        final Path release = dir.resolve("release");
        final Process process = child(dir, "none", release, "first");
        check(waitUntil(() -> Files.exists(dir.resolve("inside")), 20_000), "the child is inside its first step");
        final Rig rig = new Rig(dir);
        final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(dir));
        final AtomicBoolean asked = new AtomicBoolean();
        final AtomicBoolean locked = new AtomicBoolean();
        final AtomicBoolean afterChild = new AtomicBoolean();
        files.beforeLock = () -> asked.set(true);
        files.afterLock = () -> {
            locked.set(true);
            afterChild.set(Files.exists(dir.resolve("unlocking")));
        };
        final DeliveryRecord parent = rig.record(files, RecordStore.CAPACITY);
        final AtomicReference<String> answer = new AtomicReference<>();
        final Thread thread = new Thread(() -> answer.set(Rig.answer(parent, true, X, "Bank", "hello")));
        thread.start();
        waitUntil(asked::get, 20_000);
        thread.join(500);
        final boolean heldBack = thread.isAlive();
        Files.write(release, new byte[0]);
        thread.join(30_000);
        process.waitFor();
        check(locked.get(), "the retry called the real lock and it returned");
        check(afterChild.get(), "it returned only after the child's critical section ended (ordering, no clock)");
        check(heldBack, "the retry still waited 500 ms after asking (one-sided, not relied on)");
        same(earlier(X, 1), answer.get(), "the retry's answer");
        same(1L, rig.handovers(), "one hand-over");
    }

    /** R6, R7: a step that acts and then throws, and one that fails. */
    private static void stepsThatFail() throws Exception {
        final Rig rig = new Rig(fresh("act-then-throw"));
        rig.hook = (id, step) -> {
            if (step == 2) throw new IllegalStateException("the second broadcast failed");
        };
        same(uncertain(X, "interrupted"), Rig.answer(rig.record(), false, X, "Bank", "hello"), "the first attempt");
        same('u', rig.stateOf(X), "u X");
        rig.hook = null;
        same(uncertain(X, "interrupted"), Rig.answer(rig.record(), true, X, "Bank", "hello"), "the retry");
        same(uncertain(X, "interrupted"), Rig.answer(rig.record(), false, X, "Bank", "hello"), "the repeated first attempt");
        same(1L, rig.handovers(), "one hand-over");

        final Rig insert = new Rig(fresh("no-row"));
        insert.hook = (id, step) -> {
            if (step == 1) throw new IllegalStateException("the inbox gave no row");
        };
        same(uncertain(X, "interrupted"), Rig.answer(insert.record(), false, X, "Bank", "hello"), "a step that fails");
        same(1, insert.stepRuns.get(), "the second step never ran");
        same(0L, insert.handovers(), "nothing was handed over");
        same('u', insert.stateOf(X), "u X after a failure");
    }

    /** R8: a plan that cannot be made hands nothing over and records nothing. */
    private static void planFails() throws Exception {
        final Rig rig = new Rig(fresh("plan-fails"));
        rig.planner = message -> {
            rig.plans.incrementAndGet();
            throw new IllegalStateException("no SMS application");
        };
        same(notHanded(X, "plan"), Rig.answer(rig.record(), false, X, "Bank", "hello"), "the first attempt");
        check(rig.recordBytes() == null, "no record");
        same(0, rig.stepRuns.get(), "no step");
        same(1, rig.plans.get(), "the plan was asked once");
        // With a record: byte for byte as it was.
        rig.planner = null;
        Rig.answer(rig.record(), false, Y, "Bank", "other");
        final byte[] before = rig.recordBytes();
        rig.planner = message -> {
            throw new IllegalStateException("no SMS application");
        };
        same(notHanded(X, "plan"), Rig.answer(rig.record(), false, X, "Bank", "hello"), "with a record");
        check(Arrays.equals(before, rig.recordBytes()), "the record is as it was");
    }

    /** R9: the pending update fails, call by call. */
    private static void pendingFails() throws Exception {
        final String[] ops = {"lock", "size", "read", "write", "write-after-10", "force", "move", "forceDirectory"};
        for (String op : ops) {
            final Rig rig = new Rig(fresh("pending-fails-" + op));
            Rig.answer(rig.record(), false, Y, "Bank", "other"); // a record to read
            final byte[] before = rig.recordBytes();
            rig.stepRuns.set(0);
            final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(rig.dir));
            files.failOp = op.startsWith("write") ? "write" : op;
            files.failWriteAfter = op.equals("write-after-10") ? 10 : 0;
            final String answer = Rig.answer(rig.record(files, RecordStore.CAPACITY), false, X, "Bank", "hello");
            check(files.fired.get() >= 1, op + ": the injected failure fired");
            same(notHanded(X, "store"), answer, op + ": the first attempt");
            same(0, rig.stepRuns.get(), op + ": no step ran");
            check(!Files.exists(rig.dir.resolve(RecordStore.CORRUPT)), op + ": a failed read is not damage");
            if (op.equals("forceDirectory")) {
                same('p', rig.stateOf(X), op + ": p X is visible");
                same(uncertain(X, "interrupted"), Rig.answer(rig.record(), true, X, "Bank", "hello"), op + ": a retry");
            } else {
                check(Arrays.equals(before, rig.recordBytes()), op + ": the record is as it was");
            }
            same(0L, rig.handovers(X), op + ": X was not handed over");
        }
    }

    /** R10: the final update fails after the last step: the answer is what the steps earned. */
    private static void finalFails() throws Exception {
        for (boolean steps : new boolean[] {true, false}) {
            for (String op : new String[] {"write", "force", "move", "forceDirectory"}) {
                final String name = (steps ? "returned-" : "threw-") + op;
                final Rig rig = new Rig(fresh("final-fails-" + name));
                final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(rig.dir));
                files.failOp = op;
                files.armed = false;
                rig.hook = (id, step) -> {
                    if (step == 2) {
                        files.armed = true; // the next update is the final one
                        if (!steps) throw new IllegalStateException("the second broadcast failed");
                    }
                };
                final String answer = Rig.answer(rig.record(files, RecordStore.CAPACITY), false, X, "Bank", "hello");
                check(files.fired.get() >= 1, name + ": the injected failure fired");
                same(steps ? handed(X, 1) : uncertain(X, "interrupted"), answer, name + ": the answer");
                same(2, rig.stepRuns.get(), name + ": the steps ran once");
                rig.hook = null;
                final String later = Rig.answer(rig.record(), true, X, "Bank", "hello");
                final String expected = steps && op.equals("forceDirectory") ? earlier(X, 1) : uncertain(X, "interrupted");
                same(expected, later, name + ": a later retry");
                check(!later.startsWith("2.not-handed-over") && !later.startsWith("2.unsettled"), name + ": never 'not handed over'");
                same(1L, rig.handovers(X), name + ": one hand-over");
            }
        }
    }

    private static byte[] flipped(byte[] bytes, int index) {
        final byte[] copy = bytes.clone();
        copy[index] = (byte) (copy[index] == 'a' ? 'b' : 'a');
        return copy;
    }

    private static byte[] plus(byte[] bytes, String tail) {
        final byte[] more = tail.getBytes(StandardCharsets.US_ASCII);
        final byte[] all = Arrays.copyOf(bytes, bytes.length + more.length);
        System.arraycopy(more, 0, all, bytes.length, more.length);
        return all;
    }

    private static long key(Path file) throws IOException {
        return Files.readAttributes(file, BasicFileAttributes.class).fileKey().hashCode();
    }

    /** R11: a damaged record is moved aside, the request fails closed, and the next starts empty. */
    private static void damagedRecord() throws Exception {
        final String valid = Rig.line('h', Y, "Bank", "other", 1);
        final List<String> many = new ArrayList<>();
        for (int i = 0; i < RecordStore.CAPACITY + 1; ++i) many.add(Rig.line('h', Rig.id(i + 1), "Bank", "m", 1));
        final byte[] good = Rig.craft(valid);
        final String[] names = {"garbage", "a byte changed", "a malformed line", "parts 0 on h", "an identifier twice",
            "too many entries", "bytes after end"};
        final byte[][] bad = {
            "garbage\n".getBytes(StandardCharsets.US_ASCII),
            flipped(good, 30),
            Rig.craft("x " + Y + " " + DeliveryRecord.digest("Bank", "other") + " 1"),
            Rig.craft(Rig.line('h', Y, "Bank", "other", 0)),
            Rig.craft(valid, valid),
            Rig.craft(many.toArray(new String[0])),
            plus(good, "x\n"),
        };
        for (int i = 0; i < names.length; ++i) {
            for (boolean retry : new boolean[] {false, true}) {
                final Rig rig = new Rig(fresh("damaged-" + i + (retry ? "-retry" : "-first")));
                Files.write(rig.dir.resolve(RecordStore.RECORD), bad[i]);
                final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(rig.dir));
                final String answer = Rig.answer(rig.record(files, RecordStore.CAPACITY), retry, X, "Bank", "hello");
                same(retry ? unsettled(X) : notHanded(X, "store"), answer, names[i] + ": the request fails closed");
                same(0, rig.stepRuns.get(), names[i] + ": no step");
                check(Files.exists(rig.dir.resolve(RecordStore.CORRUPT))
                        && Arrays.equals(bad[i], Files.readAllBytes(rig.dir.resolve(RecordStore.CORRUPT))),
                        names[i] + ": the old bytes are in .corrupt");
                check(rig.recordBytes() == null, names[i] + ": the record is gone");
                final List<String> seen = files.ops;
                final int move = seen.indexOf("move");
                final int directory = seen.indexOf("forceDirectory");
                final int unlock = seen.indexOf("unlock");
                check(move >= 0 && directory > move && unlock > directory,
                        names[i] + ": the move, then the directory force, before the answer: " + seen);
                if (!retry) {
                    same(handed(Rig.id(77), 1), Rig.answer(rig.record(), false, Rig.id(77), "Bank", "next"),
                            names[i] + ": the next first attempt is handed over");
                    same(uncertain(Y, "unknown"), Rig.answer(rig.record(), true, Y, "Bank", "other"),
                            names[i] + ": a retry of an identifier from the damaged record");
                }
            }
        }
        // The move aside, and the directory force after it, fail: the request fails closed, and the lock file stays.
        for (String op : new String[] {"move", "forceDirectory"}) {
            final Rig rig = new Rig(fresh("damaged-" + op + "-fails"));
            Files.write(rig.dir.resolve(RecordStore.RECORD), bad[0]);
            Rig.answer(rig.record(), true, Y, "Bank", "x"); // quarantines, and makes the lock file
            Files.write(rig.dir.resolve(RecordStore.RECORD), bad[0]);
            final long lockKey = key(rig.dir.resolve(RecordFiles.Nio.LOCK));
            final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(rig.dir));
            files.failOp = op;
            same(notHanded(X, "store"), Rig.answer(rig.record(files, RecordStore.CAPACITY), false, X, "Bank", "hello"),
                    op + " fails: the first attempt fails closed");
            check(files.fired.get() >= 1, op + ": the injected failure fired");
            same(0, rig.stepRuns.get(), op + ": no step");
            check(key(rig.dir.resolve(RecordFiles.Nio.LOCK)) == lockKey, op + ": the lock file is the same file");
            same(notHanded(X, "store"), Rig.answer(rig.record(files, RecordStore.CAPACITY), false, X, "Bank", "hello"),
                    op + " fails: and again");
        }
    }

    /** R12: a pending entry left by an ended process is uncertain, and the next write turns it into u. */
    private static void pendingLeft() throws Exception {
        final Rig rig = new Rig(fresh("pending-left"));
        Files.write(rig.dir.resolve(RecordStore.RECORD), Rig.craft(Rig.line('p', X, "Bank", "hello", 1)));
        final byte[] before = rig.recordBytes();
        same(uncertain(X, "interrupted"), Rig.answer(rig.record(), true, X, "Bank", "hello"), "the retry");
        same(uncertain(X, "interrupted"), Rig.answer(rig.record(), false, X, "Bank", "hello"), "the first attempt");
        check(Arrays.equals(before, rig.recordBytes()), "the lookups wrote nothing");
        same(handed(Y, 1), Rig.answer(rig.record(), false, Y, "Bank", "other"), "another message");
        same('u', rig.stateOf(X), "after the next write the record has u X");
        same(0L, rig.handovers(X), "X was never handed over");
    }

    private static int entries(Rig rig) throws IOException {
        final byte[] bytes = rig.recordBytes();
        if (bytes == null) return 0;
        return new String(bytes, StandardCharsets.US_ASCII).split("\n").length - 2;
    }

    /** R13: the oldest entries go first, and a retry of one that went is closed, not handed over. */
    private static void eviction() throws Exception {
        final Rig rig = new Rig(fresh("eviction"));
        final DeliveryRecord record = rig.record(new RecordFiles.Nio(rig.dir), 4);
        for (int i = 0; i < 5; ++i) Rig.answer(record, false, Rig.id(100 + i), "Bank", "m" + i);
        same(4, entries(rig), "four entries");
        check(rig.stateOf(Rig.id(100)) == null, "the oldest is gone");
        same(uncertain(Rig.id(100), "unknown"), Rig.answer(record, true, Rig.id(100), "Bank", "m0"), "a retry of the oldest");
        same(1L, rig.handovers(Rig.id(100)), "the oldest was handed over once");
        same(earlier(Rig.id(104), 1), Rig.answer(record, true, Rig.id(104), "Bank", "m4"), "the newest");
        same(4, entries(rig), "still four, with the tombstone");
        same('u', rig.stateOf(Rig.id(100)), "the tombstone");
        same(5L, rig.handovers(), "five hand-overs in all");
    }

    /** R14: a record over the bound is refused unread, and 300 attempts of 11 parts keep the bound. */
    private static void bounds() throws Exception {
        final Rig big = new Rig(fresh("oversize"));
        Files.write(big.dir.resolve(RecordStore.RECORD), new byte[RecordStore.maximumBytes(RecordStore.CAPACITY) + 1]);
        final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(big.dir));
        same(unsettled(X), Rig.answer(big.record(files, RecordStore.CAPACITY), true, X, "Bank", "hello"), "an oversize record");
        same(0, files.reads.get(), "not read");
        check(Files.exists(big.dir.resolve(RecordStore.CORRUPT)), "moved aside");
        same(26_404, RecordStore.maximumBytes(RecordStore.CAPACITY), "the bound");

        final Rig rig = new Rig(fresh("three-hundred"));
        rig.planner = message -> {
            final List<DeliveryRecord.Step> steps = new ArrayList<>();
            steps.add(() -> rig.stepRuns.incrementAndGet());
            return new DeliveryRecord.Plan(11, steps);
        };
        final DeliveryRecord record = rig.record();
        for (int i = 0; i < 300; ++i) Rig.answer(record, false, Rig.id(5000 + i), "Bank", "m" + i);
        same(RecordStore.CAPACITY, entries(rig), "exactly N entries after 300 attempts");
        same(26_404, rig.recordBytes().length, "at most 26,404 bytes, and exactly that with N entries of 11 parts");
        same(handed(Rig.id(9999), 11), Rig.answer(record, false, Rig.id(9999), "Bank", "last"), "11 parts");
    }

    /** R15: the real store's sequence of calls, with a write of one byte at a time. */
    private static void order() throws Exception {
        final Rig rig = new Rig(fresh("order"));
        final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(rig.dir));
        files.maxWrite = 1;
        rig.hook = (id, step) -> files.ops.add("step-" + step);
        final DeliveryRecord record = rig.record(files, RecordStore.CAPACITY);
        same(handed(X, 1), Rig.answer(record, false, X, "Bank", "hello"), "the first attempt");
        same(Arrays.asList("lock", "write", "force", "move", "forceDirectory", "step-1", "step-2", "write", "force",
                "move", "forceDirectory", "unlock"), new ArrayList<>(files.ops), "a first attempt");
        same(earlier(X, 1), Rig.answer(rig.record(), true, X, "Bank", "hello"), "every byte was written");
        files.ops.clear();
        Rig.answer(record, true, Y, "Bank", "other");
        same(Arrays.asList("lock", "size", "read", "write", "force", "move", "forceDirectory", "unlock"),
                new ArrayList<>(files.ops), "a tombstone");
        Files.write(rig.dir.resolve(RecordStore.RECORD), "garbage\n".getBytes(StandardCharsets.US_ASCII));
        files.ops.clear();
        Rig.answer(record, false, Rig.id(7), "Bank", "z");
        same(Arrays.asList("lock", "size", "read", "move", "forceDirectory", "unlock"), new ArrayList<>(files.ops),
                "a quarantine");
    }

    /** R17: a retry of an identifier the record does not hold closes it. */
    private static void tombstone() throws Exception {
        final Rig rig = new Rig(fresh("tombstone"));
        final DeliveryRecord record = rig.record();
        same(uncertain(X, "unknown"), Rig.answer(record, true, X, "Bank", "hello"), "the retry of an absent identifier");
        same('u', rig.stateOf(X), "u X");
        check(new String(rig.recordBytes(), StandardCharsets.US_ASCII).contains(" " + DeliveryRecord.digest("Bank", "hello") + " 0\n"),
                "with no parts");
        same(uncertain(X, "unknown"), Rig.answer(record, false, X, "Bank", "hello"), "the late first attempt");
        same(uncertain(X, "conflict"), Rig.answer(record, false, X, "Bank", "hellO"), "a late first attempt of other content");
        same(0L, rig.handovers(), "nothing handed over");
        same(0, rig.stepRuns.get(), "no step");
        same(0, rig.plans.get(), "no plan: a retry never asks one");

        // The tombstone cannot be written: the retry is unsettled, the late original hands over, the next retry learns.
        final Rig failing = new Rig(fresh("tombstone-fails"));
        final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(failing.dir));
        files.failOp = "write";
        same(unsettled(X), Rig.answer(failing.record(files, RecordStore.CAPACITY), true, X, "Bank", "hello"), "unsettled");
        check(files.fired.get() >= 1, "the injected failure fired");
        same(handed(X, 1), Rig.answer(failing.record(), false, X, "Bank", "hello"), "the late original");
        same(earlier(X, 1), Rig.answer(failing.record(), true, X, "Bank", "hello"), "the next retry");
        same(1L, failing.handovers(), "one hand-over");
    }

    /** R18: the record lost, and an older copy restored: a retry never hands over. */
    private static void loss() throws Exception {
        final Rig rig = new Rig(fresh("loss"));
        final DeliveryRecord record = rig.record();
        Rig.answer(record, false, Y, "Bank", "y");
        final byte[] older = rig.recordBytes();
        Rig.answer(record, false, X, "Bank", "hello");
        Files.delete(rig.dir.resolve(RecordStore.RECORD));
        same(uncertain(X, "unknown"), Rig.answer(record, true, X, "Bank", "hello"), "after the loss");
        Files.write(rig.dir.resolve(RecordStore.RECORD), older);
        same(uncertain(X, "unknown"), Rig.answer(record, true, X, "Bank", "hello"), "with the older copy");
        same(1L, rig.handovers(X), "X was handed over once");
    }

    /** R19: the cooperative protocol's boundary: a first attempt reused after the record lost its entry hands over. */
    private static void reuse() throws Exception {
        final Rig held = new Rig(fresh("reuse-held"));
        Rig.answer(held.record(), false, X, "Bank", "hello");
        same(earlier(X, 1), Rig.answer(held.record(), false, X, "Bank", "hello"), "with h X held");
        final Rig closed = new Rig(fresh("reuse-closed"));
        Rig.answer(closed.record(), true, X, "Bank", "hello");
        same(uncertain(X, "unknown"), Rig.answer(closed.record(), false, X, "Bank", "hello"), "with a tombstone");
        final Rig lost = new Rig(fresh("reuse-lost"));
        Rig.answer(lost.record(), false, X, "Bank", "hello");
        Files.delete(lost.dir.resolve(RecordStore.RECORD));
        same(handed(X, 1), Rig.answer(lost.record(), false, X, "Bank", "hello"), "after the record was lost");
        same(2L, lost.handovers(X), "a second hand-over: the boundary of the protocol");
    }

    /** R20: the digest, with fixed vectors (SHA-256 of the length-prefixed UTF-8 of sender and text). */
    private static void digest() {
        same("35dfc8aa85463b0b36567dc603805cfb487b5855dffbbaf6cf70ef0ff9aa4285", DeliveryRecord.digest("Bank", "hello"), "Bank / hello");
        same("4c4d991afaa947c78c569baf4b8f938e9b8608710f68d32e5bf5356d63d540db", DeliveryRecord.digest("+15551234567", "caf\u00E9 \uD83D\uDE00"), "a number and a text");
        same("d3baf49451436e834d2c79fff543d2f3e0fe317a552d6919b2a4721e91ceb68a", DeliveryRecord.digest("", ""), "nothing but the prefixes");
        check(!DeliveryRecord.digest("a", "bc").equals(DeliveryRecord.digest("ab", "c")), "the lengths keep the fields apart");
        check(!DeliveryRecord.digest("a", "b").equals(DeliveryRecord.digest("a", "B")), "the text counts");
    }

    /** R22: a payload that is not a request touches nothing; another version gets the service's own refusal. */
    private static void entryPoint() throws Exception {
        final Golden golden = Golden.load();
        final Rig rig = new Rig(fresh("entry-point"));
        final Rig.Recording files = new Rig.Recording(new RecordFiles.Nio(rig.dir));
        final DeliveryService service = new DeliveryService(rig.record(files, RecordStore.CAPACITY));
        for (String name : golden.names("refuse-")) {
            same(0, service.answer(Golden.bytes(golden.fields(name)[0])).length, name + ": an empty result");
        }
        same(0, service.answer(("2.first." + X + "." + Base64.getEncoder().encodeToString("Bank".getBytes(StandardCharsets.UTF_8)) + ".YQ").getBytes(StandardCharsets.US_ASCII)).length, "a text without padding");
        for (String name : golden.names("version-")) {
            same("2.refused-version", new String(service.answer(Golden.bytes(golden.fields(name)[0])), StandardCharsets.US_ASCII),
                    name + ": the refusal");
        }
        check(files.ops.isEmpty(), "the files saw no call: " + files.ops);
        same(0, rig.stepRuns.get(), "no step");
        same(0, rig.plans.get(), "no plan");
        check(rig.recordBytes() == null && !Files.exists(rig.dir.resolve(RecordFiles.Nio.LOCK)), "no file was made");
        // A request that passes the parser but breaks the rules is answered on a first attempt, and a retry
        // closes its identifier first.
        final DeliveryService rules = new DeliveryService(rig.record());
        same(notHanded(X, "invalid"), new String(rules.answer(SmsServiceTest.request("first", X, "Bank!!", "hello")),
                StandardCharsets.US_ASCII), "a sender that is not one");
        same(0, rig.plans.get(), "the plan was not asked");
        same(uncertain(Y, "unknown"), new String(rules.answer(SmsServiceTest.request("retry", Y, "Bank!!", "hello")),
                StandardCharsets.US_ASCII), "a retry runs no rules");
    }

    /** R24: the boundary the channel's order closes: served out of order in a record of one, the words arrive twice. */
    private static void boundary() throws Exception {
        final Rig rig = new Rig(fresh("boundary"));
        final DeliveryRecord record = rig.record(new RecordFiles.Nio(rig.dir), 1);
        same(uncertain(X, "unknown"), Rig.answer(record, true, X, "Bank", "hello"), "retry X");
        same(handed(Y, 1), Rig.answer(record, false, Y, "Bank", "hello"), "first Y, the same words");
        same(handed(X, 1), Rig.answer(record, false, X, "Bank", "hello"), "the late first X");
        same(1L, rig.handovers(X), "X acted once");
        same(1L, rig.handovers(Y), "Y acted once");
    }

    private static void delete(Path path) throws IOException {
        try (Stream<Path> walk = Files.walk(path)) {
            walk.sorted(java.util.Comparator.reverseOrder()).forEach(p -> p.toFile().delete());
        }
    }

    public static void main(String[] args) throws Exception {
        root = Files.createTempDirectory("sms-record-");
        try {
            endingPoints();
            newStore();
            otherContent();
            overlapInProcess();
            twoProcesses();
            stepsThatFail();
            planFails();
            pendingFails();
            finalFails();
            damagedRecord();
            pendingLeft();
            eviction();
            bounds();
            order();
            tombstone();
            loss();
            reuse();
            digest();
            entryPoint();
            boundary();
        } finally {
            delete(root);
        }
        System.out.println(rows + " scenarios");
        System.exit(Checks.finish());
    }
}
