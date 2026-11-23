// SPDX-License-Identifier: GPL-2.0-or-later
//
// The launcher-application shell command (android/lineage-24/overlay/app_shortcuts) against a fake
// platform, on a plain JVM: the component grammar, the label's escaping and bounds, the listing's
// rule (order, filters, bounds, digest and icon keys), the framing of every answer read back by a
// parser of the test's own, the icon records (statuses, what is checked of the renderer's answer, a renderer
// that is missing, gone or fails one icon, the deadlines), the launch's refusals and the argument vector that
// reaches the process, how a start's output is read, the child process's drain, deadline and reaping (real
// processes), the resource files' order, and the exit statuses.
// Each row prints "ok <name>" or "FAIL <name>: why"; the last line is PASS when no row failed.
// A row that returns with a process of its own still there fails, and the process is ended by SIGKILL through its handle.
// The package is the command's, so that the test reaches what the command keeps to itself.
// test_app_shortcuts_command.py compiles and runs it, and runs it again on sources with one rule
// removed at a time, where the rows named there must fail.

package com.kernarium.apps;

import com.kernarium.icons.IconProtocol;

import java.io.ByteArrayOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.zip.CRC32;

public final class AppsCommandTest {
    private static int failures;
    private static String current;

    /** An assertion that failed in the current row. */
    private static final class Mismatch extends RuntimeException {
        private static final long serialVersionUID = 1L;

        Mismatch(String message) {
            super(message);
        }
    }

    private interface Row {
        void run() throws Exception;
    }

    /**
     * A listing of processes that failed every time it was asked: what a row has to say of its processes cannot be
     * checked. That is an error of the run, not a failure of the row (see {@link #outcome}).
     */
    private static final class ListingFailed extends RuntimeException {
        private static final long serialVersionUID = 1L;

        ListingFailed(String message, Throwable cause) {
            super(message, cause);
        }
    }

    /** How a row ended: "ok", "FAIL" (it failed) or "ERROR" (its processes could not be checked), and the text. */
    private static final class Outcome {
        final String kind;
        final String text;

        Outcome(String kind, String text) {
            this.kind = kind;
            this.text = text;
        }
    }

    private static void row(String name, Row body) {
        current = name;
        Outcome outcome = outcome(body);
        if (outcome.kind.equals("ok")) {
            System.out.println("ok " + name);
        } else {
            failures++;
            System.out.println(outcome.kind + " " + name + ": " + outcome.text);
        }
    }

    /**
     * Runs a row's body and sees to its processes. However the body ended, a process it started and left is ended
     * here, and the row fails on it: a row that stops halfway (a check that throws before its own clean-up) must not
     * leave a child that ignores SIGTERM. Whatever this does fails the row and never the run: the rows after it still
     * print. When the processes cannot be listed (every attempt failed) the row is an ERROR, which the driver does not
     * read as a failure that a mutant may be credited with: the row has not been checked.
     */
    private static Outcome outcome(Row body) {
        List<String> problems = new ArrayList<>();
        String unchecked = null;
        Set<Long> before = new HashSet<>(); // a JVM has no child between two rows, so none is the baseline when it cannot be listed
        try {
            before = childPids();
        } catch (ListingFailed failed) {
            unchecked = failed.getMessage();
        }
        try {
            body.run();
        } catch (ListingFailed failed) {
            unchecked = failed.getMessage();
        } catch (Mismatch mismatch) {
            problems.add(String.valueOf(mismatch.getMessage()));
        } catch (Throwable thrown) {
            problems.add(String.valueOf(thrown));
        }
        try {
            String stray = endStrays(before);
            if (stray != null) {
                problems.add(stray);
            }
        } catch (ListingFailed failed) {
            unchecked = failed.getMessage();
        } catch (Throwable thrown) {
            problems.add("could not end the processes it started: " + thrown);
        }
        if (unchecked != null) {
            return new Outcome("ERROR", "could not be checked: " + unchecked
                    + (problems.isEmpty() ? "" : "; its own result: " + String.join("; ", problems)));
        }
        return problems.isEmpty() ? new Outcome("ok", "") : new Outcome("FAIL", String.join("; ", problems));
    }

    private static final int LISTING_ATTEMPTS = 200;

    /**
     * The result of a listing of processes, asked again when it fails: the JDK lists the system's processes into a
     * buffer that it sizes first, and fails ("Cannot allocate memory") when processes are being created meanwhile, as
     * they are on a loaded host. A listing that keeps failing is a {@link ListingFailed}. The attempts are counted and
     * not timed: the time one listing takes is not under a deadline shared with the kill that follows (ten seconds,
     * {@link #endAll}); a listing is milliseconds, and a host on which it is not has failed the run already.
     */
    private static List<ProcessHandle> listing(java.util.function.Supplier<java.util.stream.Stream<ProcessHandle>> ask) {
        return listing(ask, LISTING_ATTEMPTS);
    }

    private static List<ProcessHandle> listing(java.util.function.Supplier<java.util.stream.Stream<ProcessHandle>> ask,
            int attempts) {
        RuntimeException failed = new RuntimeException("no attempt was made");
        for (int attempt = 0; attempt < attempts; attempt++) {
            try {
                return ask.get().collect(java.util.stream.Collectors.toList());
            } catch (RuntimeException problem) {
                failed = problem;
                try {
                    Thread.sleep(5);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
        throw new ListingFailed("the processes of this JVM could not be listed (" + failed + ")", failed);
    }

    /** The pids of the processes this JVM has started and that are still there. */
    private static Set<Long> childPids() {
        Set<Long> pids = new HashSet<>();
        for (ProcessHandle child : listing(() -> ProcessHandle.current().children())) {
            pids.add(child.pid());
        }
        return pids;
    }

    /**
     * The processes this JVM has started since {@code before} that are still there: its new children (each child
     * first: with it gone, it starts nothing more) and what each of them started.
     */
    private static List<ProcessHandle> startedSince(Set<Long> before) {
        // Known, and left: a child that exits and whose pid is reused between the first listing and the one of its
        // descendants would put the new owner's children on the list. The rows' fixtures are single processes, so no
        // descendant is expected, and the window is two listings wide.
        List<ProcessHandle> started = new ArrayList<>();
        for (ProcessHandle child : listing(() -> ProcessHandle.current().children())) {
            if (!before.contains(child.pid())) {
                started.add(child);
                started.addAll(listing(child::descendants));
            }
        }
        return started;
    }

    /**
     * SIGKILL to each of the processes through the handle this JVM holds of it (which checks the process's start time:
     * a pid that is read from somewhere is never signalled), and waits until each is gone, for ten seconds at most.
     * Returns whether they all are.
     */
    private static boolean endAll(List<ProcessHandle> handles) {
        // Known, and left: the JDK's forcible end checks the process's start time and then signals, as two steps; the
        // test cannot make it one (the Process objects stay inside ChildProcess.run, which does not change).
        for (ProcessHandle handle : handles) {
            handle.destroyForcibly();
        }
        long deadline = System.nanoTime() + 10_000_000_000L;
        boolean gone = false;
        while (!gone && System.nanoTime() < deadline) {
            gone = true;
            for (ProcessHandle handle : handles) {
                gone &= !handle.isAlive();
            }
            if (!gone) {
                try {
                    Thread.sleep(10);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
        }
        return gone;
    }

    /**
     * Ends every process this JVM started since {@code before} (see {@link #endAll}). Returns what it found, or null
     * when there was nothing.
     */
    private static String endStrays(Set<Long> before) {
        List<ProcessHandle> strays = startedSince(before);
        if (strays.isEmpty()) {
            return null;
        }
        StringBuilder found = new StringBuilder("left a process behind (ended by SIGKILL):");
        for (ProcessHandle stray : strays) {
            found.append(" pid ").append(stray.pid()).append(' ').append(stray.info().command().orElse("?"));
        }
        return endAll(strays) ? found.toString() : found + "; not gone after the kill";
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new Mismatch(what);
        }
    }

    private static void same(Object expected, Object actual, String what) {
        check(expected.equals(actual), what + ": expected <" + show(expected) + "> but got <" + show(actual) + ">");
    }

    private static String show(Object value) {
        return String.valueOf(value).replace("\n", "\\n").replace("\t", "\\t");
    }

    // ---------------------------------------------------------------------------------------------
    // The fake platform and the answers' reader.

    private static final byte[] PNG_HEAD = AppsCommand.PNG_SIGNATURE;
    /** The signature and the IHDR chunk: the least a PNG can be checked by. */
    private static final int PNG_HEADER_BYTES = 33;
    private static final int IHDR = 0x49484452;

    /**
     * A PNG-looking byte string of exactly {@code length} bytes (at least the 33 of the signature and the header)
     * whose header says {@code width} by {@code height}, {@code depth} bits, colour type {@code colour}, the
     * compression, filter and interlace methods given, with the chunk's length and type and its CRC as asked: the
     * tool checks the header, not the image.
     */
    private static byte[] pngOf(int width, int height, int depth, int colour, int compression, int filter, int interlace,
            int chunkLength, int chunkType, boolean validCrc, int length, int seed) {
        byte[] bytes = new byte[length];
        for (int i = 0; i < length; i++) {
            bytes[i] = i < PNG_HEAD.length ? PNG_HEAD[i] : (byte) (seed + i * 31);
        }
        put(bytes, 8, chunkLength);
        put(bytes, 12, chunkType);
        put(bytes, 16, width);
        put(bytes, 20, height);
        bytes[24] = (byte) depth;
        bytes[25] = (byte) colour;
        bytes[26] = (byte) compression;
        bytes[27] = (byte) filter;
        bytes[28] = (byte) interlace;
        CRC32 crc = new CRC32();
        crc.update(bytes, 12, 17);
        put(bytes, 29, (int) crc.getValue() ^ (validCrc ? 0 : 1));
        return bytes;
    }

    private static void put(byte[] bytes, int at, int value) {
        bytes[at] = (byte) (value >>> 24);
        bytes[at + 1] = (byte) (value >>> 16);
        bytes[at + 2] = (byte) (value >>> 8);
        bytes[at + 3] = (byte) value;
    }

    /** An 8-bit RGBA PNG of {@code size} by {@code size}, {@code length} bytes long. */
    private static byte[] png(int size, int length, int seed) {
        return pngOf(size, size, 8, 6, 0, 0, 0, 13, IHDR, true, length, seed);
    }

    private interface IconSource {
        Icon icon(Entry entry) throws Exception;
    }

    private static class Fake implements AppsCommand.Platform {
        int user = 0;
        List<Entry> entries = new ArrayList<>();
        List<String> locales = new ArrayList<>(Arrays.asList("en-US", "ja-JP"));
        Map<String, String> labels = new HashMap<>();
        Set<String> labelFails = new HashSet<>();
        Set<String> labelOutOfMemory = new HashSet<>();
        IconSource icons = entry -> new Icon(192, true, false, png(192, 300, 1));
        /** The renderer is missing: opening it fails with the platform's reason. */
        boolean noRenderer;
        int opened;
        int closed;
        /** The user the renderer was opened for. */
        int openedFor = -1;
        /** The foreground user once the applications were listed: a user switch in the middle of the command. */
        int switchAfterListing = -1;
        AppsCommand.StartResult startResult = new AppsCommand.StartResult(0, "Starting: Intent { }\n");
        final List<List<String>> started = new ArrayList<>();
        long clock;
        long clockStep;
        long renderLimit = AppsCommand.ICON_RENDER_MILLIS;
        /** The packages whose icons were asked of the renderer, in order. */
        final List<String> rendered = new ArrayList<>();

        Fake with(Entry... more) {
            entries.addAll(Arrays.asList(more));
            return this;
        }

        @Override
        public int currentUser() throws Exception {
            return user;
        }

        @Override
        public List<Entry> launcherActivities(int forUser) throws Exception {
            check(forUser == user, "the listing is asked for the foreground user");
            List<Entry> listed = new ArrayList<>(entries);
            if (switchAfterListing >= 0) {
                user = switchAfterListing;
            }
            return listed;
        }

        @Override
        public List<String> localeTags() throws Exception {
            return locales;
        }

        @Override
        public String label(Entry entry) throws Exception {
            if (labelFails.contains(entry.packageName)) {
                throw new IllegalStateException("no resources");
            }
            if (labelOutOfMemory.contains(entry.packageName)) {
                throw new OutOfMemoryError("a label too big to read");
            }
            return labels.get(entry.packageName + "/" + entry.activity);
        }

        @Override
        public AppsCommand.Renderer openRenderer(int forUser) throws Exception {
            if (noRenderer) {
                throw new PlatformException("the icon renderer is not available");
            }
            opened++;
            openedFor = forUser;
            return new AppsCommand.Renderer() {
                @Override
                public Icon render(Entry entry) throws Exception {
                    rendered.add(entry.packageName);
                    return icons.icon(entry);
                }

                @Override
                public void close() {
                    closed++;
                }
            };
        }

        @Override
        public AppsCommand.StartResult start(int forUser, Component component) {
            started.add(AppsCommand.startArguments(forUser, component));
            return startResult;
        }

        @Override
        public long nowMillis() {
            long now = clock;
            clock += clockStep;
            return now;
        }

        @Override
        public long renderLimitMillis() {
            return renderLimit;
        }
    }

    /** The activity manager as {@link ProviderLink} meets it: every acquisition and release, with the user. */
    private static final class LinkTransport implements ProviderLink.Transport {
        /** "acquire 10" and "release 10", in order. */
        final List<String> log = Collections.synchronizedList(new ArrayList<>());
        /** What each acquisition answers, in order; the last one repeats. */
        final List<ProviderLink.Holding> answers = new ArrayList<>();
        RuntimeException failure;
        /** An acquisition waits here when set: a slow answer of the activity manager. */
        volatile java.util.concurrent.CountDownLatch gate;
        volatile java.util.concurrent.CountDownLatch inside;
        int asked;

        @Override
        public ProviderLink.Holding acquire(int user) throws Exception {
            log.add("acquire " + user);
            java.util.concurrent.CountDownLatch waiting = gate;
            if (waiting != null) {
                inside.countDown();
                waiting.await();
            }
            if (failure != null) {
                throw failure;
            }
            ProviderLink.Holding answer = answers.get(Math.min(asked++, answers.size() - 1));
            return answer;
        }

        @Override
        public void release(int user) {
            log.add("release " + user);
        }

        int count(String kind) {
            int n = 0;
            synchronized (log) {
                for (String line : log) {
                    if (line.startsWith(kind)) {
                        n++;
                    }
                }
            }
            return n;
        }
    }

    /** A provider as the activity manager resolved it, and what it does with a request. */
    private static class LinkHolding implements ProviderLink.Holding {
        String packageName = IconProtocol.PACKAGE;
        String className = IconProtocol.PROVIDER_CLASS;
        boolean system = true;
        /** What a request gives: an icon, null for an answer that is not an ok, or an exception. */
        IconSource answer = entry -> new Icon(192, true, false, png(192, 300, 1));
        int asked;

        @Override
        public String packageName() {
            return packageName;
        }

        @Override
        public String className() {
            return className;
        }

        @Override
        public boolean system() {
            return system;
        }

        @Override
        public Icon render(int resource, int[] kinds, String[] paths) throws Exception {
            asked++;
            return answer.icon(null);
        }
    }

    private static LinkTransport transportOf(ProviderLink.Holding... answers) {
        LinkTransport transport = new LinkTransport();
        transport.answers.addAll(Arrays.asList(answers));
        return transport;
    }

    private static void waitUntil(java.util.function.BooleanSupplier condition, String what) throws Exception {
        long end = System.nanoTime() + 5_000_000_000L;
        while (!condition.getAsBoolean()) {
            check(System.nanoTime() < end, what);
            Thread.sleep(5);
        }
    }

    private static Icon ask(ProviderLink link) throws Exception {
        return link.render(0x7f010000, new int[] {IconProtocol.KIND_BASE}, new String[] {"/system/app/A/A.apk"});
    }

    private static Entry app(String packageName, String activity) {
        return new Entry(packageName, activity, false, true, true, true, 10, 1_000, 0, 0x7f010000, "", null);
    }

    private static Entry builtin(String packageName, String activity) {
        return new Entry(packageName, activity, true, true, true, true, 10, 1_000, 0, 0x7f010000, "", null);
    }

    private static Entry entry(String packageName, String activity, boolean builtin, boolean exported,
            boolean enabled, boolean permitted, long version, long updated, int icon) {
        return new Entry(packageName, activity, builtin, exported, enabled, permitted, version, updated, 0, icon, "", null);
    }

    /** What one run of the command gave. */
    private static final class Result {
        int status;
        byte[] out;
        String err;
        boolean platformMade;

        String text() {
            return new String(out, StandardCharsets.UTF_8);
        }
    }

    private static Result run(Fake platform, String... args) {
        Result result = new Result();
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        StringWriter err = new StringWriter();
        PrintWriter errors = new PrintWriter(err);
        result.status = AppsCommand.run(args, () -> {
            result.platformMade = true;
            return platform;
        }, out, errors);
        errors.flush();
        result.out = out.toByteArray();
        result.err = err.toString();
        return result;
    }

    /** The lines of a text answer. */
    private static List<String> lines(Result result) {
        String text = result.text();
        check(text.isEmpty() || text.endsWith("\n"), "every line ends with LF");
        List<String> lines = new ArrayList<>();
        for (String line : text.split("\n", -1)) {
            lines.add(line);
        }
        if (!lines.isEmpty() && lines.get(lines.size() - 1).isEmpty()) {
            lines.remove(lines.size() - 1);
        }
        return lines;
    }

    /** A listing as the host reads it: strictly, every shape checked. */
    private static final class Parsed {
        final Map<String, String> header = new HashMap<>();
        final List<String[]> rows = new ArrayList<>();
    }

    private static Parsed parseList(Result result, boolean withRows) {
        same(0, result.status, "exit status");
        check(result.out.length > 0, "an answer");
        check(!result.text().contains("\r"), "no CR in the answer");
        List<String> lines = lines(result);
        same("kernarium_apps 1", lines.get(0), "first line");
        Parsed parsed = new Parsed();
        int i = 1;
        String[] keys = {"digest", "count", "skipped", "locales", "user"};
        for (String key : keys) {
            String[] parts = lines.get(i++).split("\t", -1);
            same(2, parts.length, "header line " + key);
            same(key, parts[0], "header key");
            parsed.header.put(key, parts[1]);
        }
        check(parsed.header.get("digest").matches("[0-9a-f]{64}"), "digest is 64 hex");
        if (!withRows) {
            same(i, lines.size(), "the digest answer has nothing after the header");
            return parsed;
        }
        same("rows", lines.get(i++), "rows marker");
        int count = Integer.parseInt(parsed.header.get("count"));
        for (int r = 0; r < count; r++) {
            String[] fields = lines.get(i++).split("\t", -1);
            same(5, fields.length, "row " + r + " has five fields");
            check(Component.of(fields[0], fields[1]) != null, "row " + r + " names a component");
            check(fields[3].equals("0") || fields[3].equals("1"), "builtin flag");
            check(fields[4].matches("[0-9a-f]{16}"), "icon key");
            check(fields[2].length() > 0, "label is not empty");
            parsed.rows.add(fields);
        }
        same("end\t" + count, lines.get(i++), "end line");
        same(i, lines.size(), "nothing after the end line");
        return parsed;
    }

    /** One record of an icons answer. */
    private static final class IconRecord {
        String token;
        String status;
        int width;
        int height;
        String kind;
        String alpha;
        int length;
        String sha;
        byte[] data;
    }

    private static List<IconRecord> parseIcons(Result result) throws Exception {
        same(0, result.status, "exit status");
        byte[] bytes = result.out;
        int[] at = {0};
        same("kernarium_apps 1", readLine(bytes, at), "first line");
        String[] head = readLine(bytes, at).split("\t", -1);
        same(2, head.length, "icons line");
        same("icons", head[0], "icons key");
        int count = Integer.parseInt(head[1]);
        List<IconRecord> records = new ArrayList<>();
        for (int r = 0; r < count; r++) {
            String[] f = readLine(bytes, at).split("\t", -1);
            same(9, f.length, "record header fields");
            same("icon", f[0], "record marker");
            IconRecord record = new IconRecord();
            record.token = f[1];
            check(Component.parse(f[1]) != null, "record names a component");
            record.status = f[2];
            record.width = Integer.parseInt(f[3]);
            record.height = Integer.parseInt(f[4]);
            record.kind = f[5];
            record.alpha = f[6];
            record.length = Integer.parseInt(f[7]);
            record.sha = f[8];
            if (record.status.equals("ok")) {
                check(at[0] + record.length + 1 <= bytes.length, "the data and its LF are all there");
                record.data = Arrays.copyOfRange(bytes, at[0], at[0] + record.length);
                at[0] += record.length;
                same((int) '\n', (int) bytes[at[0]++], "LF after the data");
                same(Listing.sha256Hex(record.data), record.sha, "digest of the data");
                check(Arrays.equals(Arrays.copyOf(record.data, 8), AppsCommand.PNG_SIGNATURE), "PNG signature");
                same(record.width, record.height, "square");
                check(record.kind.equals("adaptive") || record.kind.equals("legacy"), "kind");
                check(record.alpha.equals("0") || record.alpha.equals("1"), "alpha flag");
                check(record.length <= IconProtocol.MAX_PNG_BYTES, "within the PNG bound");
            } else {
                same("0", String.valueOf(record.length), "no data");
                same("0", String.valueOf(record.width), "no width");
                same("-", record.kind, "no kind");
                same("-", record.sha, "no digest");
                check(Arrays.asList("missing", "noicon", "error", "deferred").contains(record.status),
                        "a known status: " + record.status);
            }
            records.add(record);
        }
        same("end\t" + count, readLine(bytes, at), "end line");
        same(bytes.length, at[0], "nothing after the end line");
        return records;
    }

    private static String readLine(byte[] bytes, int[] at) {
        int end = at[0];
        while (end < bytes.length && bytes[end] != '\n') {
            end++;
        }
        check(end < bytes.length, "a line ends with LF");
        String line = new String(bytes, at[0], end - at[0], StandardCharsets.UTF_8);
        at[0] = end + 1;
        return line;
    }

    private static String repeat(String text, int times) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < times; i++) {
            out.append(text);
        }
        return out.toString();
    }

    /** {@code count} copies of one UTF-16 unit, built without a second copy of it (the tests run on a small heap). */
    private static String big(char unit, int count) {
        char[] chars = new char[count];
        Arrays.fill(chars, unit);
        return new String(chars);
    }

    private static String sanitize(String raw) {
        return LabelText.sanitize(raw, "fallback.pkg");
    }

    // ---------------------------------------------------------------------------------------------

    public static void main(String[] args) {
        grammar();
        labels();
        listing();
        listAnswer();
        icons();
        launch();
        usageAndFailures();
        starts();
        children();
        sources();
        System.out.println(failures == 0 ? "PASS" : "FAILED " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void grammar() {
        row("component-grammar-accepts", () -> {
            String[][] good = {
                {"com.android.settings", "com.android.settings.Settings"},
                {"a.b", "A"},
                {"org.example.app_1", "org.example.app_1.Main$Launcher"},
                {"org.example.app", "_x.$y.Z9"},
                {"com.Example.APP", "com.Example.APP.MainActivity"},
            };
            for (String[] pair : good) {
                Component c = Component.parse(pair[0] + "/" + pair[1]);
                check(c != null, "accepted: " + pair[0] + "/" + pair[1]);
                same(pair[0], c.packageName, "package");
                same(pair[1], c.activity, "activity");
                same(pair[0] + "/" + pair[1], c.token(), "token");
            }
            check(Component.of(repeat("a", 127) + "." + repeat("b", 127), "A") != null, "package of 255 bytes");
            check(Component.of("a.b", repeat("A", 255)) != null, "activity of 255 bytes");
        });
        row("component-grammar-rejects", () -> {
            String[] bad = {
                "", "/", "a.b", "a.b/", "/A", "a/b", "singlesegment/Main", "-n/x", "a.b/c/d",
                "a.b/.Main", "a.b/Main.", "a.b/Ma..in", "a..b/Main", ".a.b/Main", "a.b./Main",
                "a.b/-Main", "--es.x/y", "a.b/9Main", "a.1b/Main", "a.b-c/Main",
                "a.b/Main'", "a.b/Main\"", "a.b/Main`", "a.b/Main;x", "a.b/Main|x", "a.b/Main&x", "a.b/Main<x",
                "a.b/Main>x", "a.b/Main(x)", "a.b/Main x", "a.b /Main", "a.b/Main\n", "a.b/Main\tx",
                "a.b/Main\r", "a.b/Main\u0000", "a.b/Main\\x", "a.b/Main$(id)", "a.b/${HOME}", "a.b/Main/x",
                "a.b/Ma\u00e9in", "a.\u00e9b/Main", "a.b/Main\u202e", "a.b/Main\uff0f", "a.b/Main%20",
                "a.b/Main#x", "a.b/Main=x", "a.b/Main:x", "a.b/Main*", "a.b/Main?", "a.b/Main~", "a.b/Main!",
                repeat("a", 128) + "." + repeat("b", 127) + "/Main",
                "a.b/" + repeat("A", 256),
            };
            for (String token : bad) {
                check(Component.parse(token) == null, "refused: <" + show(token) + ">");
            }
            check(Component.parse(null) == null, "null is refused");
            check(Component.of("a.b", null) == null, "null activity");
        });
    }

    private static void labels() {
        row("label-controls-become-space", () -> {
            same("a b c", sanitize("a\tb\nc"), "TAB and LF");
            same("a b", sanitize("a\r\nb"), "CRLF collapses");
            same("a b", sanitize("a\u000B\u000C\u0085\u2028\u2029b"), "VT FF NEL LS PS");
            same("a b", sanitize("a\u0001\u001F\u007F\u0080\u009Fb"), "C0 DEL C1");
            same("a b", sanitize("a\u00A0\u2003\u3000b"), "NBSP and other space characters");
            same("x", sanitize("  \t x \n "), "ends trimmed");
        });
        row("label-bidi-and-format-dropped", () -> {
            same("ab", sanitize("a\u202Eb"), "right-to-left override");
            same("ab", sanitize("\u202Aa\u202B\u202C\u202Db"), "embeddings and pop");
            same("ab", sanitize("a\u2066\u2067\u2068\u2069b"), "isolates");
            same("ab", sanitize("a\u200E\u200F\u061Cb"), "marks");
            same("ab", sanitize("\uFEFFa\u200Bb\u2060\u00AD"), "BOM, zero-width space, word joiner, soft hyphen");
            same("a\u200Db\u200Cc", sanitize("a\u200Db\u200Cc"), "the joiners stay");
        });
        row("label-surrogates-and-nonchars", () -> {
            same("a\uFFFDb", sanitize("a\uD800b"), "unpaired high surrogate");
            same("a\uFFFDb", sanitize("a\uDC00b"), "unpaired low surrogate");
            same("a\uFFFDb", sanitize("a\uFFFFb"), "non-character");
            same("a\uFFFDb", sanitize("a\uFDD0b"), "non-character in the Arabic block");
            same("a\uD83D\uDE00b", sanitize("a\uD83D\uDE00b"), "a paired surrogate is a character");
        });
        row("label-text-is-kept", () -> {
            same("Calculator", sanitize("Calculator"), "ASCII");
            same("\u8A08\u7B97\u6A5F \u2022 \u0645\u0631\u062D\u0628\u0627", sanitize("\u8A08\u7B97\u6A5F \u2022 \u0645\u0631\u062D\u0628\u0627"),
                    "CJK, Arabic, bullet");
            same("A B", sanitize("A \u0001 B"), "controls among spaces collapse to one");
            same("a b", sanitize("a    b"), "runs of spaces collapse");
        });
        row("label-bounds", () -> {
            String exact = repeat("x", 80);
            same(exact, sanitize(exact), "80 code points are kept");
            String longer = sanitize(repeat("x", 81));
            same(80, longer.codePointCount(0, longer.length()), "81 are cut to 80");
            check(longer.endsWith("\u2026"), "cut text ends with an ellipsis");
            check(sanitize(repeat("y", 5000)).length() <= 80, "very long text is cut");
            String emoji = repeat("\uD83D\uDE00", 80);
            same(emoji, sanitize(emoji), "80 four-byte characters are 320 bytes and kept");
            String cut = sanitize(repeat("\uD83D\uDE00", 200));
            check(cut.getBytes(StandardCharsets.UTF_8).length <= 320, "320 bytes at most");
            check(cut.codePointCount(0, cut.length()) <= 80, "80 code points at most");
            check(cut.endsWith("\u2026"), "ellipsis");
            String spaced = sanitize(repeat("ab ", 60));
            check(!spaced.contains("\u2026 ") && !spaced.endsWith(" \u2026"), "no space before the ellipsis");
        });
        row("label-empty-uses-the-package", () -> {
            same("fallback.pkg", LabelText.sanitize(null, "fallback.pkg"), "null");
            same("fallback.pkg", LabelText.sanitize("", "fallback.pkg"), "empty");
            same("fallback.pkg", LabelText.sanitize("\t\n\u202E\u200B", "fallback.pkg"), "nothing printable is left");
        });
        row("label-huge-is-cut-without-copying-it", () -> {
            // The run has a 64 MiB heap: a label of six million characters (12 MiB) does not fit a copy of
            // itself, a list of its code points and the rest of the process at once.
            same(repeat("\u8A08", 79) + "\u2026", sanitize(big('\u8A08', 6_000_000)), "cut to 79 code points and an ellipsis");
            same("fallback.pkg", LabelText.sanitize(big('\u200B', 6_000_000), "fallback.pkg"),
                    "six million characters of nothing printable leave the package name");
        });
        row("label-cleaning-stops-without-changing-the-label", () -> {
            // The text is cleaned only as far as the cut needs, and the label is what cleaning all of it gives.
            String[][] cases = {
                {"x", repeat("x", 79) + "\u2026"},
                {"\u8A08", repeat("\u8A08", 79) + "\u2026"},
                {" a", repeat("a ", 39) + "a\u2026"},
                {"a\u202Eb\u200B\u0001", repeat("ab ", 26) + "a\u2026"},
                {"ab\u00A0", repeat("ab ", 26) + "a\u2026"},
                {"\uD83D\uDE00", repeat("\uD83D\uDE00", 79) + "\u2026"},
                {"\t", "fallback.pkg"},
            };
            for (String[] each : cases) {
                same(each[1], sanitize(repeat(each[0], 400_000 / each[0].length())), "a label of repeated " + show(each[0]));
            }
        });
    }

    private static void listing() {
        row("listing-order-and-filter", () -> {
            Fake fake = new Fake().with(
                    app("org.zed.app", "org.zed.app.Main"),
                    app("com.alpha.app", "com.alpha.app.B"),
                    app("com.alpha.app", "com.alpha.app.A"),
                    entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1),
                    entry("com.off.app", "com.off.app.Main", false, true, false, true, 1, 1, 1),
                    entry("com.perm.app", "com.perm.app.Main", false, true, true, false, 1, 1, 1),
                    app("bad name", "x.Y"),
                    app("com.ok.app", "com.ok.app.Main"),
                    app("com.ok.app", "com.ok.app.Main"));
            Listing listing = Listing.of(0, fake.locales, fake.entries);
            List<String> tokens = new ArrayList<>();
            for (Entry e : listing.rows) {
                tokens.add(e.packageName + "/" + e.activity);
            }
            same(Arrays.asList("com.alpha.app/com.alpha.app.A", "com.alpha.app/com.alpha.app.B",
                    "com.ok.app/com.ok.app.Main", "org.zed.app/org.zed.app.Main"), tokens,
                    "sorted, without the not exported, disabled, not permitted, misnamed and doubled ones");
            same(1, listing.skipped, "only the misnamed launchable one is counted as skipped");
        });
        row("listing-per-package-bound", () -> {
            Fake fake = new Fake();
            for (int i = 0; i < 9; i++) {
                fake.with(app("com.many.app", "com.many.app.A" + i));
            }
            Listing listing = Listing.of(0, fake.locales, fake.entries);
            same(8, listing.rows.size(), "8 per package");
            same(1, listing.skipped, "the ninth is skipped");
            same("com.many.app.A7", listing.rows.get(7).activity, "the first eight in order");
        });
        row("listing-row-bound", () -> {
            Fake fake = new Fake();
            for (int i = 0; i < 1100; i++) {
                fake.with(app("com.bulk.p" + String.format("%04d", i), "com.bulk.Main"));
            }
            Listing listing = Listing.of(0, fake.locales, fake.entries);
            same(1024, listing.rows.size(), "1024 rows");
            same(76, listing.skipped, "the rest are skipped");
            same("com.bulk.p1023", listing.rows.get(1023).packageName, "the first 1024 in order");
        });
        row("digest-follows-the-set", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), builtin("com.b.app", "com.b.app.Main"));
            String base = Listing.of(0, fake.locales, fake.entries).digest;
            List<Entry> shuffled = new ArrayList<>(fake.entries);
            Collections.reverse(shuffled);
            same(base, Listing.of(0, fake.locales, shuffled).digest, "the order the platform reports in does not matter");
            same(base, Listing.of(0, fake.locales, fake.entries).digest, "the same set, the same digest");
            check(!base.equals(Listing.of(0, fake.locales, withAdded(fake.entries,
                    app("com.c.app", "com.c.app.Main"))).digest), "an installed app changes it");
            check(!base.equals(Listing.of(0, fake.locales, fake.entries.subList(0, 1)).digest), "a removed app changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, true, true, 11, 1_000, 0x7f010000))).digest),
                    "an update (version code) changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 2_000, 0x7f010000))).digest),
                    "a reinstall (install time) changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 0x7f020000))).digest),
                    "a new icon resource changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Main", false, true, false, true, 10, 1_000, 0x7f010000))).digest),
                    "a disabled app changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    entry("com.a.app", "com.a.app.Other", false, true, true, true, 10, 1_000, 0x7f010000))).digest),
                    "a renamed launcher activity changes it");
            check(!base.equals(Listing.of(0, Arrays.asList("fr-FR"), fake.entries).digest), "Android's language changes it");
            check(!base.equals(Listing.of(10, fake.locales, fake.entries).digest), "another foreground user changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    new Entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 0, 0x7f010000, "overlay", null))).digest),
                    "an overlay, split or library coming or going changes it");
            check(!base.equals(Listing.of(0, fake.locales, withReplaced(fake.entries, 0,
                    new Entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 5_000, 0x7f010000, "", null))).digest),
                    "the image's build, for a built-in application that was never updated, changes it");
        });
        row("digest-follows-the-whole-locale-list", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            List<String> six = Arrays.asList("a", "b", "c", "d", "e", "f");
            List<String> other = Arrays.asList("a", "b", "c", "d", "e", "g");
            Listing first = Listing.of(0, six, fake.entries);
            Listing second = Listing.of(0, other, fake.entries);
            same(first.locales, second.locales, "the header shows the same five tags");
            check(!first.digest.equals(second.digest), "the sixth locale changes the labels, so it changes the digest");
            check(!first.digest.equals(Listing.of(0, Arrays.asList("a", "b", "c", "d", "e"), fake.entries).digest),
                    "a sixth locale coming changes it");
            same(Listing.of(0, Arrays.asList("a", "b"), fake.entries).digest, Listing.of(0, Arrays.asList("a", "b"), fake.entries).digest,
                    "stable");
        });
        row("icon-key-follows-the-icon", () -> {
            Entry a = app("com.a.app", "com.a.app.Main");
            String key = Listing.iconKey(a);
            check(key.matches("[0-9a-f]{16}"), "16 hex characters");
            same(key, Listing.iconKey(app("com.a.app", "com.a.app.Main")), "stable");
            check(!key.equals(Listing.iconKey(entry("com.a.app", "com.a.app.Main", false, true, true, true, 11, 1_000, 0x7f010000))), "version");
            check(!key.equals(Listing.iconKey(entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_001, 0x7f010000))), "install time");
            check(!key.equals(Listing.iconKey(entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 5))), "icon resource");
            check(!key.equals(Listing.iconKey(app("com.a.app", "com.a.app.Other"))), "component");
            check(!key.equals(Listing.iconKey(builtin("com.a.app", "com.a.app.Main"))), "built-in flag");
            check(!key.equals(Listing.iconKey(new Entry("com.a.app", "com.a.app.Main", false, true, true, true, 10, 1_000, 0,
                    0x7f010000, "split", null))), "the files the resources come from");
            Entry never = new Entry("com.a.app", "com.a.app.Main", true, true, true, true, 10, 1_000, 5_000, 0x7f010000, "", null);
            Entry next = new Entry("com.a.app", "com.a.app.Main", true, true, true, true, 10, 1_000, 6_000, 0x7f010000, "", null);
            check(!Listing.iconKey(never).equals(Listing.iconKey(next)),
                    "the image's build is an input of its own: the update time may be equal across two images");
            check(!Listing.iconKey(never).equals(Listing.iconKey(builtin("com.a.app", "com.a.app.Main"))),
                    "a built-in application that was never updated and one that was are different inputs");
            same(3, Listing.RENDER_VERSION, "the renderer's version (the icons are drawn by the renderer app since 3)");
        });
        row("locale-line", () -> {
            same("en-US,ja-JP", Listing.localeLine(Arrays.asList("en-US", "ja-JP")), "tags joined");
            same("a,b,c,d,e", Listing.localeLine(Arrays.asList("a", "b", "c", "d", "e", "f", "g")), "five at most");
            same("en", Listing.localeLine(Arrays.asList("e n", "en\n", "", "en;x", "en")), "only letters, digits and hyphens");
            same("", Listing.localeLine(Collections.<String>emptyList()), "none");
            String longTag = repeat("a", 35);
            String joined = Listing.localeLine(Arrays.asList(longTag, longTag, longTag, longTag, longTag));
            check(joined.length() <= 128, "128 bytes at most");
            String valid41 = "zh-Hant-TW-u-ca-chinese-nu-hanidec-x-private";
            check(valid41.length() > 35 && valid41.length() <= 128, "a valid tag longer than 35 characters");
            same(valid41, Listing.localeLine(Arrays.asList(valid41)), "it fits the budget and is shown");
            same("", Listing.localeLine(Arrays.asList(repeat("a", 129))), "a tag over the budget is not");
        });
    }

    private static List<Entry> withAdded(List<Entry> base, Entry extra) {
        List<Entry> list = new ArrayList<>(base);
        list.add(extra);
        return list;
    }

    private static List<Entry> withReplaced(List<Entry> base, int index, Entry replacement) {
        List<Entry> list = new ArrayList<>(base);
        list.set(index, replacement);
        return list;
    }

    private static void listAnswer() {
        row("digest-answer", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            Result digest = run(fake, "digest");
            Parsed header = parseList(digest, false);
            Result list = run(fake, "list");
            Parsed listed = parseList(list, true);
            same(header.header, listed.header, "digest and list begin with the same header block");
            same("1", header.header.get("count"), "count");
            same("0", header.header.get("skipped"), "skipped");
            same("en-US,ja-JP", header.header.get("locales"), "locales");
            same("0", header.header.get("user"), "user");
            same(Listing.of(0, fake.locales, fake.entries).digest, header.header.get("digest"), "the digest");
            same(0, digest.err.length(), "nothing on stderr");
        });
        row("list-format", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"),
                    app("com.example.game", "com.example.game.Main$Launcher"));
            fake.labels.put("com.android.settings/com.android.settings.Settings", "Settings");
            fake.labels.put("com.example.game/com.example.game.Main$Launcher", "Fun Game");
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(2, parsed.rows.size(), "two rows");
            same("com.android.settings", parsed.rows.get(0)[0], "package");
            same("com.android.settings.Settings", parsed.rows.get(0)[1], "activity");
            same("Settings", parsed.rows.get(0)[2], "label");
            same("1", parsed.rows.get(0)[3], "built in");
            same("0", parsed.rows.get(1)[3], "installed");
            same(Listing.iconKey(fake.entries.get(0)), parsed.rows.get(0)[4], "icon key");
            same("Fun Game", parsed.rows.get(1)[2], "label");
        });
        row("list-hostile-labels", () -> {
            String[] hostile = {
                "A\tB", "A\nB", "A\r\nend\t0\nB", "rows\nend\t99", "x\u2028y", "p\u2029q", "\nkernarium_apps 1\ndigest\t0",
                "evil\u202E\u2066 label", "$(reboot)", "'; reboot; '", "\u0000nul", "tab\t\t\tmany",
            };
            Fake fake = new Fake();
            for (int i = 0; i < hostile.length; i++) {
                String packageName = "com.host" + i + ".app";
                fake.with(app(packageName, packageName + ".Main"));
                fake.labels.put(packageName + "/" + packageName + ".Main", hostile[i]);
            }
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(hostile.length, parsed.rows.size(), "one row per app, however the labels are written");
            String text = result.text();
            same(hostile.length + 8, lines(result).size(), "the lines are the header, rows, end and one line per app");
            check(!text.contains("\u202E") && !text.contains("\u2066") && !text.contains("\u2028")
                    && !text.contains("\u2029") && !text.contains("\u0000"), "no bidirectional, separator or NUL character reaches the answer");
        });
        row("list-label-unreadable-uses-the-package", () -> {
            Fake fake = new Fake().with(app("com.broken.app", "com.broken.app.Main"), app("com.none.app", "com.none.app.Main"));
            fake.labelFails.add("com.broken.app");
            Parsed parsed = parseList(run(fake, "list"), true);
            same("com.broken.app", parsed.rows.get(0)[2], "the label could not be read");
            same("com.none.app", parsed.rows.get(1)[2], "the application has no label");
        });
        row("list-huge-label-is-one-row", () -> {
            Fake fake = new Fake().with(app("com.big.app", "com.big.app.Main"), app("com.next.app", "com.next.app.Main"));
            fake.labels.put("com.big.app/com.big.app.Main", big('\u8A08', 6_000_000));
            fake.labels.put("com.next.app/com.next.app.Main", "Next");
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(2, parsed.rows.size(), "both applications are listed");
            same(repeat("\u8A08", 79) + "\u2026", parsed.rows.get(0)[2], "the big label is cut like any other");
            same("Next", parsed.rows.get(1)[2], "the next label");
            check(result.out.length < 4096, "the answer does not carry the label: " + result.out.length);
        });
        row("list-label-out-of-memory-uses-the-package", () -> {
            Fake fake = new Fake().with(app("com.big.app", "com.big.app.Main"), app("com.next.app", "com.next.app.Main"));
            fake.labelOutOfMemory.add("com.big.app");
            fake.labels.put("com.next.app/com.next.app.Main", "Next");
            Result result = run(fake, "list");
            same(0, result.status, "the command succeeds");
            Parsed parsed = parseList(result, true);
            same("com.big.app", parsed.rows.get(0)[2], "the label that does not fit is that application's failure");
            same("Next", parsed.rows.get(1)[2], "the others are listed as they are");
        });
        row("list-empty", () -> {
            Parsed parsed = parseList(run(new Fake(), "list"), true);
            same(0, parsed.rows.size(), "no rows");
            same("0", parsed.header.get("count"), "count");
        });
        row("list-sizes-within-the-bounds", () -> {
            Fake fake = new Fake();
            String longPackage = repeat("a", 127) + "." + repeat("b", 127);
            for (int i = 0; i < 1100; i++) {
                String activity = repeat("C", 251) + String.format("%04d", i);
                fake.with(app(i < 1 ? longPackage : repeat("p", 120) + "." + String.format("q%04d", i), activity));
                fake.labels.put(fake.entries.get(i).packageName + "/" + activity, repeat("\uD83D\uDE00", 90));
            }
            Result result = run(fake, "list");
            Parsed parsed = parseList(result, true);
            same(1024, parsed.rows.size(), "the row bound");
            for (String line : lines(result)) {
                check(line.getBytes(StandardCharsets.UTF_8).length <= 860, "a line is at most 860 bytes");
            }
            check(result.out.length < 900 * 1024, "the whole answer is under 900 KiB: " + result.out.length);
        });
    }

    /** What the renderer's answers are, good and bad: (name, icon). */
    private static final class Case {
        final String name;
        final Icon icon;

        Case(String name, Icon icon) {
            this.name = name;
            this.icon = icon;
        }
    }

    private static Icon icon(int size, byte[] png) {
        return new Icon(size, true, false, png);
    }

    /** Answers a renderer may give: they pass. */
    private static List<Case> goodAnswers() {
        List<Case> good = new ArrayList<>();
        for (int size : new int[] {192, 128, 96}) {
            good.add(new Case("size " + size, icon(size, png(size, 300, 1))));
        }
        good.add(new Case("the shortest PNG: the signature and the header", icon(192, png(192, 33, 1))));
        good.add(new Case("the longest PNG", icon(192, png(192, IconProtocol.MAX_PNG_BYTES, 1))));
        good.add(new Case("RGB without an alpha claim", icon(192, pngOf(192, 192, 8, 2, 0, 0, 0, 13, IHDR, true, 300, 1))));
        good.add(new Case("RGBA with the alpha claim", new Icon(192, false, true, png(192, 300, 1))));
        good.add(new Case("RGBA without it", new Icon(192, false, false, png(192, 300, 1))));
        return good;
    }

    /** Answers a renderer must not be believed on, each with one defect: (name, icon). */
    private static List<Case> badAnswers() {
        List<Case> bad = new ArrayList<>();
        bad.add(new Case("no icon", null));
        bad.add(new Case("size 64", icon(64, png(64, 300, 1))));
        bad.add(new Case("size 193", icon(193, png(193, 300, 1))));
        bad.add(new Case("size 0", icon(0, png(0, 300, 1))));
        bad.add(new Case("size 256", icon(256, png(256, 300, 1))));
        bad.add(new Case("no PNG", icon(192, null)));
        bad.add(new Case("an empty PNG", icon(192, new byte[0])));
        bad.add(new Case("a PNG one byte short of the header", icon(192, Arrays.copyOf(png(192, 300, 1), PNG_HEADER_BYTES - 1))));
        bad.add(new Case("a PNG of 20 bytes", icon(192, Arrays.copyOf(png(192, 300, 1), 20))));
        bad.add(new Case("a PNG one byte over the bound", icon(192, png(192, IconProtocol.MAX_PNG_BYTES + 1, 1))));
        bad.add(new Case("a text that is not a PNG", icon(192, "GIF89a not a png at all, but long enough to hold a header".getBytes(StandardCharsets.US_ASCII))));
        for (int at : new int[] {0, 1, 7}) {
            byte[] broken = png(192, 300, 1);
            broken[at] ^= 1;
            bad.add(new Case("byte " + at + " of the signature", icon(192, broken)));
        }
        bad.add(new Case("an IHDR chunk of 12 bytes", icon(192, pngOf(192, 192, 8, 6, 0, 0, 0, 12, IHDR, true, 300, 1))));
        bad.add(new Case("an IHDR chunk of 14 bytes", icon(192, pngOf(192, 192, 8, 6, 0, 0, 0, 14, IHDR, true, 300, 1))));
        bad.add(new Case("another chunk first", icon(192, pngOf(192, 192, 8, 6, 0, 0, 0, 13, 0x49444154, true, 300, 1))));
        bad.add(new Case("a damaged header checksum", icon(192, pngOf(192, 192, 8, 6, 0, 0, 0, 13, IHDR, false, 300, 1))));
        bad.add(new Case("a width that is not the size", icon(192, pngOf(191, 192, 8, 6, 0, 0, 0, 13, IHDR, true, 300, 1))));
        bad.add(new Case("a height that is not the size", icon(192, pngOf(192, 191, 8, 6, 0, 0, 0, 13, IHDR, true, 300, 1))));
        bad.add(new Case("a smaller size than asked", icon(192, png(128, 300, 1))));
        bad.add(new Case("a larger size than the answer says", icon(128, png(192, 300, 1))));
        bad.add(new Case("a width of 2^32 - 192", icon(192, pngOf(-192, 192, 8, 6, 0, 0, 0, 13, IHDR, true, 300, 1))));
        for (int depth : new int[] {1, 4, 16}) {
            bad.add(new Case(depth + " bits", icon(192, pngOf(192, 192, depth, 6, 0, 0, 0, 13, IHDR, true, 300, 1))));
        }
        for (int colour : new int[] {0, 3, 4}) {
            bad.add(new Case("colour type " + colour, icon(192, pngOf(192, 192, 8, colour, 0, 0, 0, 13, IHDR, true, 300, 1))));
        }
        bad.add(new Case("another compression method", icon(192, pngOf(192, 192, 8, 6, 1, 0, 0, 13, IHDR, true, 300, 1))));
        bad.add(new Case("another filter method", icon(192, pngOf(192, 192, 8, 6, 0, 1, 0, 13, IHDR, true, 300, 1))));
        bad.add(new Case("an interlaced picture", icon(192, pngOf(192, 192, 8, 6, 0, 0, 1, 13, IHDR, true, 300, 1))));
        bad.add(new Case("an unknown interlace method", icon(192, pngOf(192, 192, 8, 6, 0, 0, 255, 13, IHDR, true, 300, 1))));
        bad.add(new Case("an alpha claim for a picture that has none", new Icon(192, true, true,
                pngOf(192, 192, 8, 2, 0, 0, 0, 13, IHDR, true, 300, 1))));
        return bad;
    }

    private static String statusOf(Icon icon) throws Exception {
        Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
        fake.icons = entry -> icon;
        return parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main")).get(0).status;
    }

    private static void icons() {
        row("icons-record-framing", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"));
            fake.icons = entry -> {
                boolean a = entry.packageName.equals("com.a.app");
                return new Icon(a ? 192 : 128, a, !a, png(a ? 192 : 128, a ? 1000 : 2000, 7));
            };
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.b.app/com.b.app.Main");
            List<IconRecord> records = parseIcons(result);
            same(2, records.size(), "two records in the order asked");
            same("com.a.app/com.a.app.Main", records.get(0).token, "first");
            same("ok", records.get(0).status, "ok");
            same(192, records.get(0).width, "192 pixels");
            same("adaptive", records.get(0).kind, "adaptive");
            same("0", records.get(0).alpha, "opaque");
            same(1000, records.get(0).length, "length");
            same(128, records.get(1).width, "the size the renderer answered");
            same(128, records.get(1).height, "square");
            same("legacy", records.get(1).kind, "legacy");
            same("1", records.get(1).alpha, "alpha");
            same(2000, records.get(1).length, "length");
            same(0, result.err.length(), "nothing on stderr");
            same(Arrays.asList("com.a.app", "com.b.app"), fake.rendered, "each icon asked of the renderer once");
            same(1, fake.opened, "one renderer for the command");
            same(1, fake.closed, "released");
        });
        row("icons-order-is-the-order-asked", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"));
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.b.app/com.b.app.Main", "com.a.app/com.a.app.Main"));
            same("com.b.app/com.b.app.Main", records.get(0).token, "first asked, first answered");
            same("com.a.app/com.a.app.Main", records.get(1).token, "second");
        });
        row("icons-statuses", () -> {
            Fake fake = new Fake().with(app("com.ok.app", "com.ok.app.Main"),
                    entry("com.noicon.app", "com.noicon.app.Main", false, true, true, true, 1, 1, 0),
                    app("com.throws.app", "com.throws.app.Main"), app("com.notpng.app", "com.notpng.app.Main"),
                    app("com.badsize.app", "com.badsize.app.Main"), app("com.null.app", "com.null.app.Main"));
            fake.icons = entry -> {
                switch (entry.packageName) {
                    case "com.throws.app":
                        throw new OutOfMemoryError("a hostile icon");
                    case "com.notpng.app":
                        return new Icon(192, false, false, "GIF89a not a png".getBytes(StandardCharsets.US_ASCII));
                    case "com.badsize.app":
                        return new Icon(193, false, false, png(193, 100, 1));
                    case "com.null.app":
                        return null;
                    default:
                        return new Icon(192, false, false, png(192, 100, 1));
                }
            };
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.ok.app/com.ok.app.Main",
                    "com.gone.app/com.gone.app.Main", "com.noicon.app/com.noicon.app.Main",
                    "com.throws.app/com.throws.app.Main", "com.notpng.app/com.notpng.app.Main",
                    "com.badsize.app/com.badsize.app.Main", "com.null.app/com.null.app.Main"));
            same(Arrays.asList("ok", "missing", "noicon", "error", "error", "error", "error"),
                    statuses(records), "statuses");
        });
        row("icons-not-launchable-is-missing", () -> {
            Fake fake = new Fake().with(entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1),
                    entry("com.off.app", "com.off.app.Main", false, true, false, true, 1, 1, 1),
                    entry("com.perm.app", "com.perm.app.Main", false, true, true, false, 1, 1, 1));
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.hidden.app/com.hidden.app.Main",
                    "com.off.app/com.off.app.Main", "com.perm.app/com.perm.app.Main"));
            same(Arrays.asList("missing", "missing", "missing"), statuses(records), "no icon of what cannot be launched");
            same(0, fake.rendered.size(), "nothing rendered");
        });
        row("png-check-accepts-what-a-renderer-may-send", () -> {
            for (Case answer : goodAnswers()) {
                String problem = PngCheck.problem(answer.icon);
                check(problem == null, answer.name + " is refused: " + problem);
            }
        });
        row("png-check-refuses-what-a-renderer-must-not-be-believed-on", () -> {
            for (Case answer : badAnswers()) {
                check(PngCheck.problem(answer.icon) != null, "accepted: " + answer.name);
            }
        });
        row("png-check-refuses-a-short-answer-without-failing", () -> {
            // Every length from nothing to the header's last byte: the answer is refused, not read past its end.
            byte[] full = png(192, 300, 1);
            for (int length = 0; length < PNG_HEADER_BYTES; length++) {
                check(PngCheck.problem(icon(192, Arrays.copyOf(full, length))) != null, "a PNG of " + length + " bytes");
            }
            check(PngCheck.problem(icon(192, Arrays.copyOf(full, PNG_HEADER_BYTES))) == null, "the header alone");
        });
        row("png-check-length-bounds-are-exact", () -> {
            check(PngCheck.problem(icon(192, png(192, IconProtocol.MAX_PNG_BYTES, 1))) == null, "98304 bytes");
            check(PngCheck.problem(icon(192, png(192, IconProtocol.MAX_PNG_BYTES + 1, 1))) != null, "98305 bytes");
            check(PngCheck.problem(icon(192, png(192, PNG_HEADER_BYTES, 1))) == null, "33 bytes");
            check(PngCheck.problem(icon(192, png(192, PNG_HEADER_BYTES + 1, 1))) == null, "34 bytes");
            same(33, PngCheck.HEADER_BYTES, "the header's length");
        });
        row("icons-answer-is-checked", () -> {
            for (Case answer : goodAnswers()) {
                same("ok", statusOf(answer.icon), "the command takes: " + answer.name);
            }
            for (Case answer : badAnswers()) {
                same("error", statusOf(answer.icon), "the command refuses: " + answer.name);
            }
        });
        row("icons-bad-answer-costs-that-icon-alone", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.bad.app", "com.bad.app.Main"),
                    app("com.c.app", "com.c.app.Main"));
            fake.icons = entry -> entry.packageName.equals("com.bad.app")
                    ? icon(192, pngOf(192, 192, 8, 6, 0, 0, 1, 13, IHDR, true, 300, 1)) : icon(192, png(192, 300, 1));
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.bad.app/com.bad.app.Main", "com.c.app/com.c.app.Main");
            same(Arrays.asList("ok", "error", "ok"), statuses(parseIcons(result)), "the others are drawn");
            same(Arrays.asList("com.a.app", "com.bad.app", "com.c.app"), fake.rendered, "each asked once, none again");
        });
        row("icons-a-worker-that-failed-costs-that-icon-alone", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.dies.app", "com.dies.app.Main"),
                    app("com.c.app", "com.c.app.Main"));
            fake.icons = entry -> {
                if (entry.packageName.equals("com.dies.app")) {
                    throw new PlatformException("the icon could not be drawn");
                }
                return icon(192, png(192, 300, 1));
            };
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.dies.app/com.dies.app.Main", "com.c.app/com.c.app.Main");
            same(Arrays.asList("ok", "error", "ok"), statuses(parseIcons(result)), "the rest of the batch is drawn");
            same(Arrays.asList("com.a.app", "com.dies.app", "com.c.app"), fake.rendered,
                    "the failed icon is asked once: not again, and not at a smaller size");
            same(1, fake.opened, "one hold of the renderer");
        });
        row("icons-failing-render-is-silent", () -> {
            Throwable[] failures = {new OutOfMemoryError("a hostile bitmap"), new StackOverflowError(),
                new IllegalStateException("uid=2000 android.permission.SECRET"), new AssertionError("x")};
            java.io.PrintStream before = System.err;
            ByteArrayOutputStream captured = new ByteArrayOutputStream();
            System.setErr(new java.io.PrintStream(captured, true));
            List<String> got;
            try {
                got = new ArrayList<>();
                for (Throwable failure : failures) {
                    Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
                    fake.icons = entry -> {
                        throw sneaky(failure);
                    };
                    Result result = run(fake, "icons", "com.a.app/com.a.app.Main");
                    got.addAll(statuses(parseIcons(result)));
                    same(0, result.err.length(), "nothing on the command's stderr");
                }
            } finally {
                System.setErr(before);
            }
            same(Arrays.asList("error", "error", "error", "error"), got, "an exception or an error is that icon's error");
            same(0, captured.size(), "no stack trace of a dying worker reaches the process's stderr");
        });
        row("icons-renderer-missing-fails-the-command", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            fake.noRenderer = true;
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main");
            same(1, result.status, "exit status");
            same(0, result.out.length, "no stdout");
            same("kernarium_apps: icons failed: the icon renderer is not available\n", result.err, "one fixed line");
            Fake none = new Fake().with(entry("com.noicon.app", "com.noicon.app.Main", false, true, true, true, 1, 1, 0));
            none.noRenderer = true;
            Result nothingToDraw = run(none, "icons", "com.gone.app/com.gone.app.Main", "com.noicon.app/com.noicon.app.Main");
            same(1, nothingToDraw.status, "a renderer that is not there is seen at once, whatever is asked");
            same(0, nothingToDraw.out.length, "no stdout");
            Fake others = new Fake().with(app("com.a.app", "com.a.app.Main"));
            others.noRenderer = true;
            same(0, run(others, "list").status, "list does not need it");
            same(0, run(others, "digest").status, "digest does not need it");
            same(0, others.opened, "and does not ask for it");
        });
        row("icons-renderer-is-released", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main"));
            same(1, fake.closed, "after an answer");
            Fake failing = new Fake().with(app("com.a.app", "com.a.app.Main"));
            failing.icons = entry -> {
                throw new IllegalStateException("x");
            };
            parseIcons(run(failing, "icons", "com.a.app/com.a.app.Main"));
            same(1, failing.closed, "after an icon that failed");
            Fake gone = new Fake().with(app("com.a.app", "com.a.app.Main"));
            gone.icons = entry -> {
                throw new AppsCommand.RendererGone("gone", null);
            };
            same(1, run(gone, "icons", "com.a.app/com.a.app.Main").status, "gone");
            same(1, gone.closed, "after the renderer was lost");
            Fake late = new Fake().with(app("com.a.app", "com.a.app.Main"));
            late.clockStep = 40_000;
            parseIcons(run(late, "icons", "com.a.app/com.a.app.Main"));
            same(1, late.closed, "after the deadline");
        });
        row("icons-renderer-lost-mid-batch-fails-the-command", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.gone.app", "com.gone.app.Main"),
                    app("com.c.app", "com.c.app.Main"));
            fake.icons = entry -> {
                if (entry.packageName.equals("com.gone.app")) {
                    throw new AppsCommand.RendererGone("the icon renderer is not available", new RuntimeException("dead"));
                }
                return icon(192, png(192, 300, 1));
            };
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.gone.app/com.gone.app.Main", "com.c.app/com.c.app.Main");
            same(1, result.status, "exit status");
            same(0, result.out.length, "nothing of the first icon is printed: the answer is whole or not at all");
            same("kernarium_apps: icons failed: the icon renderer is not available\n", result.err, "one fixed line");
            same(Arrays.asList("com.a.app", "com.gone.app"), fake.rendered, "nothing is asked of a renderer that is gone");
        });
        row("icons-an-unanswered-render-means-there-is-no-renderer", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"),
                    entry("com.c.app", "com.c.app.Main", false, true, true, true, 1, 1, 0),
                    entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1));
            fake.renderLimit = 200;
            java.util.concurrent.CountDownLatch never = new java.util.concurrent.CountDownLatch(1);
            fake.icons = entry -> {
                if (entry.packageName.equals("com.a.app")) {
                    never.await();
                }
                return icon(192, png(192, 100, 1));
            };
            long begin = System.nanoTime();
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.b.app/com.b.app.Main",
                    "com.c.app/com.c.app.Main", "com.hidden.app/com.hidden.app.Main");
            long tookMillis = (System.nanoTime() - begin) / 1_000_000;
            same(1, result.status, "a provider that never answers is a renderer that is not there: exit status");
            same(0, result.out.length, "nothing is printed");
            same("kernarium_apps: icons failed: the icon renderer is not available\n", result.err, "one fixed line");
            check(tookMillis < 1500, "the command did not wait for the render that was never answered: " + tookMillis + " ms");
            same(Arrays.asList("com.a.app"), fake.rendered, "nothing is asked after it");
            same(1, fake.closed, "the renderer is released");
            never.countDown();
        });
        row("icons-a-reported-timeout-of-one-draw-is-that-icons-error", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.slow.app", "com.slow.app.Main"),
                    app("com.c.app", "com.c.app.Main"));
            fake.renderLimit = 5_000;
            fake.icons = entry -> {
                if (entry.packageName.equals("com.slow.app")) {
                    Thread.sleep(300); // the provider took its time, then answered: this draw timed out
                    throw new PlatformException("the icon could not be drawn");
                }
                return icon(192, png(192, 300, 1));
            };
            Result result = run(fake, "icons", "com.a.app/com.a.app.Main", "com.slow.app/com.slow.app.Main", "com.c.app/com.c.app.Main");
            same(Arrays.asList("ok", "error", "ok"), statuses(parseIcons(result)), "an answered timeout costs that icon alone");
            same(Arrays.asList("com.a.app", "com.slow.app", "com.c.app"), fake.rendered, "the rest are still drawn");
        });
        row("icons-the-renderer-is-opened-for-the-listed-user", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            fake.user = 0;
            fake.switchAfterListing = 10;
            parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main"));
            same(0, fake.openedFor, "the renderer's user is the one the applications were listed for, not the foreground by now");
            same(10, fake.user, "(the foreground user did change)");
        });
        row("link-the-user-is-one-for-acquiring-reacquiring-and-releasing", () -> {
            LinkHolding first = new LinkHolding();
            first.answer = entry -> {
                throw new ProviderLink.ProviderDied(new RuntimeException("dead"));
            };
            LinkTransport transport = transportOf(first, new LinkHolding());
            ProviderLink link = new ProviderLink(transport, 10);
            try {
                ask(link);
                check(false, "the death of the provider costs the icon in flight");
            } catch (PlatformException died) {
                same("the icon renderer died", died.getMessage(), "the icon's error");
            }
            check(ask(link) != null, "the next icon is asked of a fresh hold");
            link.close();
            same(Arrays.asList("acquire 10", "release 10", "acquire 10", "release 10"), transport.log,
                    "every hold is for the user the command was opened for, and given back for it");
        });
        row("link-takes-the-images-provider-only", () -> {
            String[][] strangers = {{"com.example.thief", IconProtocol.PROVIDER_CLASS}, {IconProtocol.PACKAGE, "com.kernarium.icons.OtherProvider"},
                {IconProtocol.PACKAGE, "com.kernarium.icons.IconProvider2"}, {null, IconProtocol.PROVIDER_CLASS},
                {IconProtocol.PACKAGE, null}, {"", ""}};
            for (String[] stranger : strangers) {
                LinkHolding holding = new LinkHolding();
                holding.packageName = stranger[0];
                holding.className = stranger[1];
                LinkTransport transport = transportOf(holding);
                try {
                    new ProviderLink(transport, 10);
                    check(false, "an answerer that is not the image's provider is not taken: " + Arrays.toString(stranger));
                } catch (PlatformException refused) {
                    same(ProviderLink.UNAVAILABLE, refused.getMessage(), "the fixed reason");
                }
                same(Arrays.asList("acquire 10", "release 10"), transport.log, "it is released unread");
                same(0, holding.asked, "nothing is asked of it");
            }
            LinkHolding installed = new LinkHolding();
            installed.system = false;
            LinkTransport transport = transportOf(installed);
            try {
                new ProviderLink(transport, 10);
                check(false, "the provider's package is not a system application: an installed package that took the name");
            } catch (PlatformException refused) {
                same(ProviderLink.UNAVAILABLE, refused.getMessage(), "the fixed reason");
            }
            same(Arrays.asList("acquire 10", "release 10"), transport.log, "released");
            LinkHolding unreadable = new LinkHolding() {
                @Override
                public String packageName() {
                    throw new IllegalStateException("no info");
                }
            };
            LinkTransport unknown = transportOf(unreadable);
            try {
                new ProviderLink(unknown, 10);
                check(false, "an answerer that cannot be identified is not taken");
            } catch (PlatformException refused) {
                same(ProviderLink.UNAVAILABLE, refused.getMessage(), "the fixed reason");
            }
            same(Arrays.asList("acquire 10", "release 10"), unknown.log, "released");
            LinkTransport nothing = transportOf((ProviderLink.Holding) null);
            try {
                new ProviderLink(nothing, 10);
                check(false, "no provider is no renderer");
            } catch (PlatformException refused) {
                same(ProviderLink.UNAVAILABLE, refused.getMessage(), "the fixed reason");
            }
            same(Arrays.asList("acquire 10"), nothing.log, "there was nothing to release");
            LinkTransport failing = transportOf(new LinkHolding());
            failing.failure = new IllegalStateException("user 10 may not open provider com.kernarium.icons");
            try {
                new ProviderLink(failing, 10);
                check(false, "a framework failure is no renderer");
            } catch (PlatformException refused) {
                same(ProviderLink.UNAVAILABLE, refused.getMessage(), "the fixed reason, not the framework's words");
            }
        });
        row("link-checks-who-answers-on-every-acquisition", () -> {
            LinkHolding first = new LinkHolding();
            first.answer = entry -> {
                throw new ProviderLink.ProviderDied(new RuntimeException("dead"));
            };
            LinkHolding impostor = new LinkHolding();
            impostor.packageName = "com.example.thief";
            LinkTransport transport = transportOf(first, impostor);
            ProviderLink link = new ProviderLink(transport, 10);
            try {
                ask(link);
                check(false, "the death costs the icon");
            } catch (PlatformException died) {
                same("the icon renderer died", died.getMessage(), "the icon in flight is the one lost");
            }
            same(Arrays.asList("acquire 10", "release 10"), transport.log, "no hold is taken for no icon");
            try {
                ask(link);
                check(false, "the next icon has no renderer to be drawn by");
            } catch (AppsCommand.RendererGone gone) {
                same(ProviderLink.UNAVAILABLE, gone.getMessage(), "the new answerer is not the provider: the renderer is gone");
            }
            same(0, impostor.asked, "nothing is asked of the new answerer");
            same(Arrays.asList("acquire 10", "release 10", "acquire 10", "release 10"), transport.log, "and it is released");
            link.close();
            same(4, transport.log.size(), "close has nothing left to give back");
        });
        row("link-a-provider-that-died-costs-one-icon", () -> {
            LinkHolding first = new LinkHolding();
            first.answer = entry -> {
                throw new ProviderLink.ProviderDied(new RuntimeException("dead"));
            };
            LinkHolding second = new LinkHolding();
            LinkTransport transport = transportOf(first, second);
            ProviderLink link = new ProviderLink(transport, 0);
            try {
                ask(link);
                check(false, "the icon in flight is lost");
            } catch (PlatformException died) {
                same("the icon renderer died", died.getMessage(), "fixed words");
            }
            same(Arrays.asList("acquire 0", "release 0"), transport.log, "a fresh hold is taken for the next icon, not before");
            check(ask(link) != null, "the next icon is drawn");
            same(1, second.asked, "by the fresh hold");
            LinkTransport gone = transportOf(first);
            ProviderLink alone = new ProviderLink(gone, 0);
            gone.answers.add(null);
            try {
                ask(alone);
                check(false, "the icon in flight is lost");
            } catch (PlatformException died) {
                same("the icon renderer died", died.getMessage(), "an error of that icon, not the end of the renderer");
            }
            try {
                ask(alone);
                check(false, "the provider is not there again");
            } catch (AppsCommand.RendererGone lost) {
                same(ProviderLink.UNAVAILABLE, lost.getMessage(), "the next icon finds the renderer gone, with the fixed reason");
            }
        });
        row("link-an-answer-that-is-not-an-ok-keeps-the-hold", () -> {
            LinkHolding holding = new LinkHolding();
            holding.answer = entry -> null;
            LinkTransport transport = transportOf(holding);
            ProviderLink link = new ProviderLink(transport, 0);
            try {
                ask(link);
                check(false, "an error answer is an error");
            } catch (PlatformException error) {
                same("the icon could not be drawn", error.getMessage(), "the icon's error");
            }
            holding.answer = entry -> new Icon(192, true, false, png(192, 300, 1));
            check(ask(link) != null, "the next icon is drawn by the same hold");
            same(Arrays.asList("acquire 0"), transport.log, "no release and no new acquisition");
            link.close();
            same(Arrays.asList("acquire 0", "release 0"), transport.log, "released once at the end");
        });
        row("link-close-is-final-and-release-is-idempotent", () -> {
            LinkTransport transport = transportOf(new LinkHolding());
            ProviderLink link = new ProviderLink(transport, 0);
            link.close();
            link.close();
            same(Arrays.asList("acquire 0", "release 0"), transport.log, "one release for one hold");
            try {
                ask(link);
                check(false, "a closed link draws nothing");
            } catch (AppsCommand.RendererGone gone) {
                same(ProviderLink.UNAVAILABLE, gone.getMessage(), "fixed reason");
            }
            same(2, transport.log.size(), "and takes no hold again");
        });
        row("link-an-abandoned-call-takes-no-hold-after-close", () -> {
            java.util.concurrent.CountDownLatch inCall = new java.util.concurrent.CountDownLatch(1);
            java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
            LinkHolding first = new LinkHolding();
            first.answer = entry -> {
                inCall.countDown();
                release.await();
                throw new ProviderLink.ProviderDied(new RuntimeException("dead"));
            };
            LinkTransport transport = transportOf(first, new LinkHolding());
            ProviderLink link = new ProviderLink(transport, 0);
            Throwable[] ended = new Throwable[1];
            Thread call = new Thread(() -> {
                try {
                    ask(link);
                } catch (Throwable problem) {
                    ended[0] = problem;
                }
            });
            call.start();
            check(inCall.await(5, java.util.concurrent.TimeUnit.SECONDS), "the call is in flight");
            link.close();
            release.countDown();
            call.join(5_000);
            check(!call.isAlive(), "the abandoned call ended");
            check(ended[0] instanceof PlatformException, "it ended as the error of its icon: " + ended[0]);
            same(Arrays.asList("acquire 0", "release 0"), transport.log,
                    "one hold, one release: the death delivered after close takes no hold and gives none back");
        });
        row("link-a-hold-that-arrives-after-close-is-given-back", () -> {
            LinkHolding first = new LinkHolding();
            first.answer = entry -> {
                throw new ProviderLink.ProviderDied(new RuntimeException("dead"));
            };
            LinkTransport transport = transportOf(first, new LinkHolding());
            ProviderLink link = new ProviderLink(transport, 0);
            transport.gate = new java.util.concurrent.CountDownLatch(1);
            transport.inside = new java.util.concurrent.CountDownLatch(1);
            Throwable[] ended = new Throwable[1];
            Thread call = new Thread(() -> {
                try {
                    try {
                        ask(link);
                    } catch (PlatformException died) {
                        // the icon in flight is lost; the next one asks for a fresh hold
                    }
                    ask(link);
                } catch (Throwable problem) {
                    ended[0] = problem;
                }
            });
            call.start();
            check(transport.inside.await(5, java.util.concurrent.TimeUnit.SECONDS), "the new hold is being asked for");
            link.close();
            transport.gate.countDown();
            call.join(5_000);
            check(!call.isAlive(), "the abandoned call ended");
            check(ended[0] instanceof AppsCommand.RendererGone, "it ended without drawing: " + ended[0]);
            same(transport.count("acquire"), transport.count("release"), "every hold was given back: " + transport.log);
        });
        row("link-an-abandoned-call-takes-no-replacement-hold", () -> {
            // The command stopped waiting for a call and interrupted it; the provider dies after that and before the
            // command closes the renderer. The abandoned call gives the dead hold back and takes no other: nobody could
            // use it, and only the next live request takes a hold.
            LinkHolding first = new LinkHolding();
            LinkTransport transport = transportOf(first, new LinkHolding());
            first.answer = entry -> {
                try {
                    Thread.sleep(60_000);
                } catch (InterruptedException interrupted) {
                    throw new ProviderLink.ProviderDied(interrupted);
                }
                return null;
            };
            ProviderLink link = new ProviderLink(transport, 0);
            Throwable[] ended = new Throwable[1];
            Thread call = new Thread(() -> {
                try {
                    ask(link);
                } catch (Throwable problem) {
                    ended[0] = problem;
                }
            });
            call.start();
            waitUntil(() -> first.asked == 1, "the call is in flight");
            transport.log.add("interrupt");
            call.interrupt();
            call.join(5_000);
            check(!call.isAlive(), "the abandoned call ended");
            check(ended[0] instanceof PlatformException, "as the error of that icon: " + ended[0]);
            transport.log.add("close");
            link.close();
            same(Arrays.asList("acquire 0", "interrupt", "release 0", "close"), transport.log,
                    "one hold, given back by the call that found it dead; no second hold, and close has nothing to give back");
        });
        row("icons-deadline-defers-the-rest", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"), app("com.b.app", "com.b.app.Main"),
                    app("com.c.app", "com.c.app.Main"));
            fake.clockStep = 12_000;
            List<IconRecord> records = parseIcons(run(fake, "icons", "com.a.app/com.a.app.Main", "com.b.app/com.b.app.Main",
                    "com.c.app/com.c.app.Main"));
            same(Arrays.asList("ok", "deferred", "deferred"), statuses(records), "no icon is started after 20 s");
            Fake quick = new Fake().with(app("com.a.app", "com.a.app.Main"));
            quick.clockStep = 20_000;
            same(Arrays.asList("ok"), statuses(parseIcons(run(quick, "icons", "com.a.app/com.a.app.Main"))), "20 s exactly is still in time");
        });
        row("icons-refuses-a-bad-component", () -> {
            for (String bad : new String[] {"a.b", "a.b/Main; reboot", "-x/y", "a.b/Main'", "com.a.app/com.a.app.Main\n"}) {
                Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
                Result result = run(fake, "icons", "com.a.app/com.a.app.Main", bad);
                same(2, result.status, "refused: " + show(bad));
                same(0, result.out.length, "no stdout");
                check(!result.platformMade, "nothing asked of the platform");
                check(!result.err.contains(bad) && !result.err.contains("reboot"), "the refused text is not echoed");
            }
        });
        row("icons-count-and-duplicates", () -> {
            Fake fake = new Fake().with(app("com.a.app", "com.a.app.Main"));
            same(64, run(fake, "icons").status, "none");
            List<String> nine = new ArrayList<>(Arrays.asList("icons"));
            for (int i = 0; i < 9; i++) {
                nine.add("com.a" + i + ".app/com.a" + i + ".Main");
            }
            same(64, run(fake, nine.toArray(new String[0])).status, "nine");
            List<String> eight = new ArrayList<>(nine.subList(0, 9));
            Result ok = run(fake, eight.toArray(new String[0]));
            same(0, ok.status, "eight");
            same(8, parseIcons(ok).size(), "eight records");
            Result twice = run(fake, "icons", "com.a.app/com.a.app.Main", "com.a.app/com.a.app.Main");
            same(64, twice.status, "the same component twice");
            same(0, twice.out.length, "no stdout");
            same("kernarium_apps: the same component twice\n", twice.err, "one line, not the usage text");
        });
        row("icons-answer-size", () -> {
            Fake fake = new Fake();
            List<String> arguments = new ArrayList<>(Arrays.asList("icons"));
            for (int i = 0; i < 8; i++) {
                String packageName = repeat("a", 127) + "." + repeat("b", 120) + i;
                String activity = repeat("C", 254) + i;
                fake.with(app(packageName, activity));
                arguments.add(packageName + "/" + activity);
            }
            fake.icons = entry -> new Icon(192, true, true, png(192, IconProtocol.MAX_PNG_BYTES, 2));
            Result result = run(fake, arguments.toArray(new String[0]));
            same(8, parseIcons(result).size(), "eight records");
            check(result.out.length < 800 * 1024, "under 800 KiB: " + result.out.length);
        });
    }

    /** Throws {@code failure} whatever its kind, as a platform's code that does not declare it would. */
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException sneaky(Throwable failure) throws T {
        throw (T) failure;
    }

    private static List<String> statuses(List<IconRecord> records) {
        List<String> statuses = new ArrayList<>();
        for (IconRecord record : records) {
            statuses.add(record.status);
        }
        return statuses;
    }

    private static void launch() {
        final String settings = "com.android.settings/com.android.settings.Settings";
        row("launch-starts-exactly-the-component", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"));
            fake.user = 10;
            Result result = run(fake, "launch", settings);
            same(0, result.status, "exit status");
            same("launched\t" + settings + "\n", result.text(), "the answer");
            same(1, fake.started.size(), "one start");
            same(Arrays.asList("/system/bin/cmd", "activity", "start-activity", "--user", "10", "-a",
                    "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-f", "0x10200000", "-n",
                    settings), fake.started.get(0), "the argument vector: separate strings, no shell");
            same(0, result.err.length(), "nothing on stderr");
        });
        row("launch-refuses-what-is-not-listed", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"),
                    entry("com.hidden.app", "com.hidden.app.Main", false, false, true, true, 1, 1, 1),
                    entry("com.off.app", "com.off.app.Main", false, true, false, true, 1, 1, 1),
                    entry("com.perm.app", "com.perm.app.Main", false, true, true, false, 1, 1, 1));
            String[] refused = {
                "com.android.settings/com.android.settings.Other", "com.other.app/com.other.app.Main",
                "com.android.settings/com.android.settings.Main", "com.hidden.app/com.hidden.app.Main",
                "com.off.app/com.off.app.Main", "com.perm.app/com.perm.app.Main",
            };
            for (String token : refused) {
                Result result = run(fake, "launch", token);
                same(2, result.status, "refused: " + token);
                same(0, result.out.length, "no stdout");
                check(result.err.startsWith("kernarium_apps: refused"), "the refusal is said: " + result.err);
            }
            same(0, fake.started.size(), "no process was started for any of them");
        });
        row("launch-refuses-hostile-arguments-before-anything-runs", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"));
            String[] hostile = {
                settings + "; reboot", settings + " --es x y", "-n " + settings, settings + "\n", settings + "'",
                "--user 0/" + settings, "com.android.settings/$(id)", "com.android.settings/`id`",
                "com.android.settings/com.android.settings.Settings|sh", "com.android.settings", "/", "",
                "com.android.settings//com.android.settings.Settings", "com.android.settings/.Settings",
                "com.android.settings/com.android.settings.Settings\u0000",
                "com.android;settings/com.android.settings.Settings", "com.android.set tings/x.Y",
                "com.android.set'tings/x.Y", "com.android$settings/x.Y", "com.android.set-tings/x.Y",
                "com.android.set\"tings/x.Y", "com.android.set(tings)/x.Y", "com.android.set\ntings/x.Y",
            };
            for (String token : hostile) {
                Result result = run(fake, "launch", token);
                same(2, result.status, "refused: " + show(token));
                same(0, result.out.length, "no stdout");
                check(!result.platformMade, "the platform is not even made: " + show(token));
            }
            same(0, fake.started.size(), "nothing started");
        });
        row("launch-failures", () -> {
            Fake fake = new Fake().with(builtin("com.android.settings", "com.android.settings.Settings"));
            fake.startResult = new AppsCommand.StartResult(255, "Exception occurred while executing 'start-activity':\n");
            Result exited = run(fake, "launch", settings);
            same(1, exited.status, "a status other than 0");
            same(0, exited.out.length, "no stdout");
            check(exited.err.length() <= 512, "stderr is bounded");
            fake.startResult = new AppsCommand.StartResult(0,
                    "Starting: Intent { }\nError type 3\nError: Activity class {com.x/com.x.Y} does not exist.\n");
            Result reported = run(fake, "launch", settings);
            same(1, reported.status, "an Error line with status 0");
            same(0, reported.out.length, "no stdout");
            fake.startResult = new AppsCommand.StartResult(0, "Starting: Intent { }\nWarning: Activity not started, its current task has been brought to the front\n");
            Result front = run(fake, "launch", settings);
            same(0, front.status, "an app already open is brought to the front");
            fake.startResult = new AppsCommand.StartResult(255, "Starting: Intent { }\n\nException occurred while executing 'start-activity':\n"
                    + "java.lang.SecurityException: Permission Denial: starting Intent { cmp=com.x/com.x.Y } from null (pid=1, uid=2000) requires android.permission.SECRET\n"
                    + "\tat com.android.server.am.ActivityManagerService.start(ActivityManagerService.java:1)\n");
            Result denied = run(fake, "launch", settings);
            same(1, denied.status, "a permission denial");
            same("kernarium_apps: cmd activity start-activity: permission denied (exit 255)\n", denied.err,
                    "fixed words: neither the uid nor the permission is copied");
        });
        row("launch-component-named-like-an-error", () -> {
            // The command echoes the intent it starts, component included: a launcher named like a failure starts.
            String[][] names = {
                {"com.example.viewer", "com.example.viewer.SecurityExceptionViewer"},
                {"com.example.errors", "com.example.errors.ErrorActivity"},
                {"com.example.perm", "com.example.perm.PermissionDenialMain"},
                {"com.example.exc", "com.example.exc.ExceptionList"},
                {"com.example.error", "com.example.error.Error"},
            };
            for (String[] name : names) {
                Fake fake = new Fake().with(app(name[0], name[1]));
                String token = name[0] + "/" + name[1];
                fake.startResult = new AppsCommand.StartResult(0, "Starting: Intent { act=android.intent.action.MAIN "
                        + "cat=[android.intent.category.LAUNCHER] flg=0x10200000 cmp=" + token + " }\n");
                Result result = run(fake, "launch", token);
                same(0, result.status, "a start of " + token);
                same("launched\t" + token + "\n", result.text(), "the answer");
                fake.startResult = new AppsCommand.StartResult(0, "Starting: Intent { cmp=" + token + " }\n"
                        + "Warning: Activity not started, its current task has been brought to the front\n");
                same(0, run(fake, "launch", token).status, "an app already open: " + token);
            }
        });
    }

    private static void starts() {
        row("start-outcome", () -> {
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.D }\n") == null, "plain start");
            check(StartOutcome.failure(0, "") == null, "no output");
            same("cmd activity start-activity: exited 1", StartOutcome.failure(1, ""), "status 1 and nothing said");
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.SecurityExceptionViewer }\n"
                    + "Warning: Activity not started, intent has been delivered to currently running top-most instance.\n") == null,
                    "a warning is not a failure");
            String reason = StartOutcome.failure(1, "Error: \u202E \u0001 bad \u00e9 " + repeat("x", 500) + "\n");
            same("cmd activity start-activity: start failed (exit 1)", reason, "an Error line gives a fixed reason, not its text");
            for (char c : reason.toCharArray()) {
                check(c >= 0x20 && c < 0x7F, "printable ASCII only");
            }
        });
        row("start-outcome-kinds", () -> {
            String denied = "cmd activity start-activity: permission denied";
            check(StartOutcome.failure(255, "Starting: Intent { }\n\nException occurred while executing 'start-activity':\n"
                    + "java.lang.SecurityException: Permission Denial: starting Intent { } from null (pid=9, uid=2000)\n").startsWith(denied),
                    "a security exception is a denial, the first 'Exception occurred' line notwithstanding");
            check(StartOutcome.failure(0, "Starting: Intent { }\nError: Activity not started, you do not have permission to access it.\n")
                    .startsWith(denied), "an Error line about a permission");
            check(StartOutcome.failure(0, "Permission Denial: starting Intent { } from null requires android.permission.X\n")
                    .startsWith(denied), "a Permission Denial line");
            String missing = "cmd activity start-activity: no such activity";
            check(StartOutcome.failure(0, "Starting: Intent { }\nError type 3\nError: Activity class {com.x/com.x.Y} does not exist.\n")
                    .startsWith(missing), "a class that does not exist");
            String failed = "cmd activity start-activity: start failed";
            check(StartOutcome.failure(0, "Starting: Intent { }\nError: Activity not started, unable to resolve Intent { }\n")
                    .startsWith(failed), "another Error line");
            check(StartOutcome.failure(255, "java.lang.IllegalStateException: boom\n").startsWith(failed), "another exception");
            check(StartOutcome.failure(1, "at com.android.Foo.bar(Foo.java:1)\n").equals("cmd activity start-activity: exited 1"),
                    "a stack frame alone says nothing");
            String reason = StartOutcome.failure(255, "Error: uid=2000 android.permission.SECRET /data/app/x\n");
            check(!reason.contains("2000") && !reason.contains("SECRET") && !reason.contains("/data"),
                    "nothing of the command's own words: " + reason);
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.ErrorActivity }\n") == null, "an echoed component");
            check(StartOutcome.failure(0, "Starting: Intent { cmp=a.b/c.Error }\nStarting: Exception\n") == null,
                    "Starting lines never count");
        });
    }

    private static void usageAndFailures() {
        row("usage", () -> {
            String[][] bad = {
                {}, {"list", "extra"}, {"digest", "x"}, {"help"}, {"--locale", "en"}, {"list", "--locale", "en"},
                {"launch"}, {"launch", "a.b/C", "d.e/F"}, {"LIST"}, {"icon", "a.b/C"}, {"set", "a.b", "on"},
                {""}, {"list\n"},
            };
            for (String[] args : bad) {
                Fake fake = new Fake();
                Result result = run(fake, args);
                same(64, result.status, "usage: " + show(Arrays.asList(args).toString()));
                same(0, result.out.length, "no stdout");
                check(result.err.startsWith("usage: kernarium_apps"), "the usage line");
                check(!result.platformMade, "the platform is not made");
            }
        });
        row("failure-leaves-no-stdout", () -> {
            Fake fake = new Fake() {
                @Override
                public List<Entry> launcherActivities(int forUser) throws Exception {
                    throw new PlatformException("the package manager gave no answer");
                }
            };
            for (String verb : new String[] {"digest", "list"}) {
                Result result = run(fake, verb);
                same(1, result.status, verb + " exit status");
                same(0, result.out.length, verb + " prints nothing");
                same("kernarium_apps: " + verb + " failed: the package manager gave no answer\n", result.err, "stderr says why");
            }
            Result icons = run(fake, "icons", "a.b/C");
            same(1, icons.status, "icons");
            same(0, icons.out.length, "icons prints nothing");
            Result launch = run(fake, "launch", "a.b/C");
            same(1, launch.status, "launch");
            same(0, launch.out.length, "launch prints nothing");
        });
        row("platform-cannot-be-made", () -> {
            StringWriter err = new StringWriter();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            int status = AppsCommand.run(new String[] {"list"}, () -> {
                throw new PlatformException("the package manager is not running");
            }, out, new PrintWriter(err));
            same(1, status, "exit status");
            same(0, out.size(), "no stdout");
            same("kernarium_apps: the package manager is not running\n", err.toString(), "stderr says why");
            same(64, AppsCommand.run(new String[] {"nope"}, () -> {
                throw new IllegalStateException("never made");
            }, out, new PrintWriter(new StringWriter())), "a usage error needs no platform");
        });
        row("failure-reasons-are-fixed", () -> {
            // A framework's message can carry a uid, a permission or a path: only the class of the exception is said.
            Fake fake = new Fake() {
                @Override
                public int currentUser() {
                    throw new SecurityException("Permission Denial: getCurrentUser() from pid=1, uid=2000 requires android.permission.INTERACT_ACROSS_USERS");
                }
            };
            for (String verb : new String[] {"digest", "list"}) {
                Result result = run(fake, verb);
                same(1, result.status, verb + " exit status");
                same("kernarium_apps: " + verb + " failed: unexpected java.lang.SecurityException\n", result.err, "the class only");
                same(0, result.out.length, "nothing on stdout");
            }
            Result icons = run(fake, "icons", "a.b/C");
            same(1, icons.status, "icons");
            same(0, icons.out.length, "icons prints nothing");
            same("kernarium_apps: icons failed: unexpected java.lang.SecurityException\n", icons.err, "icons says the class");
            Result launch = run(fake, "launch", "a.b/C");
            same(1, launch.status, "launch with no known foreground user");
            same(0, launch.out.length, "no stdout");
            same(0, fake.started.size(), "nothing is started for a user that is not known");
        });
        row("failure-of-any-question-is-a-failure", () -> {
            // The commands take no substitute: a configuration, a listing or a user that cannot be had is exit 1.
            final String[] broken = {"locales", "listing", "user"};
            for (String what : broken) {
                Fake fake = new Fake() {
                    @Override
                    public int currentUser() throws Exception {
                        if (what.equals("user")) {
                            throw new PlatformException("the foreground user is not known");
                        }
                        return super.currentUser();
                    }

                    @Override
                    public List<String> localeTags() throws Exception {
                        if (what.equals("locales")) {
                            throw new PlatformException("the system's configuration cannot be read");
                        }
                        return super.localeTags();
                    }

                    @Override
                    public List<Entry> launcherActivities(int forUser) throws Exception {
                        if (what.equals("listing")) {
                            throw new PlatformException("a permission could not be checked");
                        }
                        return super.launcherActivities(forUser);
                    }
                };
                fake.with(app("com.a.app", "com.a.app.Main"));
                for (String[] args : new String[][] {{"digest"}, {"list"}, {"launch", "com.a.app/com.a.app.Main"}}) {
                    if (what.equals("locales") && args[0].equals("launch")) {
                        continue; // a launch needs no locale
                    }
                    Result result = run(fake, args);
                    same(1, result.status, what + " broken: " + args[0]);
                    same(0, result.out.length, "no stdout");
                }
                same(0, fake.started.size(), what + " broken: nothing started");
            }
        });
        row("stderr-is-one-bounded-line", () -> {
            Fake fake = new Fake() {
                @Override
                public int currentUser() throws Exception {
                    throw new PlatformException(repeat("long message \u202E ", 500) + "\nsecond line");
                }
            };
            Result result = run(fake, "list");
            same(1, result.status, "exit status");
            check(result.err.getBytes(StandardCharsets.UTF_8).length <= 512, "512 bytes at most: " + result.err.length());
            same(1, result.err.split("\n").length, "one line");
            for (char c : result.err.toCharArray()) {
                check(c == '\n' || (c >= 0x20 && c < 0x7F), "printable ASCII");
            }
        });
        row("exit-statuses-are-the-contract", () -> {
            same(0, AppsCommand.EXIT_OK, "ok");
            same(1, AppsCommand.EXIT_FAILURE, "failure");
            same(2, AppsCommand.EXIT_REFUSED, "refused");
            same(64, AppsCommand.EXIT_USAGE, "usage");
            same(1, AppsCommand.FORMAT_VERSION, "format version");
            same("kernarium_apps", AppsCommand.NAME, "name");
            same(8, AppsCommand.MAX_ICONS, "components per icons");
            same(98304, IconProtocol.MAX_PNG_BYTES, "PNG bound");
            same(20_000L, AppsCommand.ICON_DEADLINE_MILLIS, "deadline");
            same(8_000L, AppsCommand.ICON_RENDER_MILLIS, "one icon's render");
            check(AppsCommand.ICON_DEADLINE_MILLIS + AppsCommand.ICON_RENDER_MILLIS <= 30_000, "the whole icons command in the host's 30 s");
            check(AppsCommand.ICON_RENDER_MILLIS > 6_500, "the tool's last word comes after the renderer's own bound");
            same(1024, Listing.MAX_ROWS, "rows");
            same(8, Listing.MAX_PER_PACKAGE, "per package");
            same(80, LabelText.MAX_CODE_POINTS, "label code points");
            same(320, LabelText.MAX_BYTES, "label bytes");
        });
    }

    /**
     * A child that ignores SIGTERM and writes its pid to the file named by its first argument, as one process: the
     * shell sets the trap and then becomes the sleep (exec keeps its pid and the ignored signal), so that it has no
     * child of its own to outlive it when it is killed.
     */
    private static final String STUBBORN = "trap '' TERM; echo $$ > \"$1\"; exec sleep 600";

    /** A shell command line as the argument vector of a process. */
    private static List<String> sh(String script, String... arguments) {
        List<String> argv = new ArrayList<>(Arrays.asList("/bin/sh", "-c", script, "sh"));
        argv.addAll(Arrays.asList(arguments));
        return argv;
    }

    /**
     * Whether a process that the child wrote its pid for is still there (a zombie is not reaped, so it counts). It
     * only asks: a child that is still there when the row returns is ended by {@link #row}, which fails the row on it.
     */
    private static boolean alive(java.io.File pidFile) throws Exception {
        String text = new String(java.nio.file.Files.readAllBytes(pidFile.toPath()), StandardCharsets.US_ASCII).trim();
        check(!text.isEmpty(), "the child wrote its pid");
        java.util.Optional<ProcessHandle> handle = ProcessHandle.of(Long.parseLong(text));
        return handle.isPresent() && handle.get().isAlive();
    }

    /**
     * Whether the child that wrote its pid to the file is among {@code started} and alive: the pid it recorded is
     * compared with the handles', so the observation is of that child and not of any new one. A file that is still
     * empty is a failure of the fixture. Nothing is signalled: the handles are what ends it ({@link #endAll}).
     */
    private static boolean survivorAmong(List<ProcessHandle> started, java.io.File pidFile) throws Exception {
        String text = new String(java.nio.file.Files.readAllBytes(pidFile.toPath()), StandardCharsets.US_ASCII).trim();
        check(!text.isEmpty(), "the child wrote its pid");
        long recorded = Long.parseLong(text);
        for (ProcessHandle handle : started) {
            if (handle.pid() == recorded) {
                return handle.isAlive();
            }
        }
        return false;
    }

    private static java.io.File pidFile() throws Exception {
        java.io.File file = java.io.File.createTempFile("kernarium-apps-pid", ".txt");
        file.deleteOnExit();
        return file;
    }

    /**
     * A child whose answers to isAlive are scripted. The first (the check on entry) is true; the second, which the
     * terminate code asks right before it would ask for a kill, is {@code aliveBeforeKill}; the rest are
     * {@code aliveAfter}. The timed wait never says the child ended: it says so false, or is interrupted.
     */
    private static final class ScriptedChild extends Process {
        private final boolean interrupted;
        private final boolean[] answers;
        private int asked;

        ScriptedChild(boolean interrupted, boolean aliveBeforeKill, boolean aliveAfter) {
            this.interrupted = interrupted;
            this.answers = new boolean[] {true, aliveBeforeKill, aliveAfter};
        }

        @Override
        public boolean isAlive() {
            return answers[Math.min(asked++, answers.length - 1)];
        }

        @Override
        public boolean waitFor(long timeout, java.util.concurrent.TimeUnit unit) throws InterruptedException {
            if (interrupted) {
                throw new InterruptedException("scripted");
            }
            return false;
        }

        @Override
        public java.io.OutputStream getOutputStream() {
            return java.io.OutputStream.nullOutputStream();
        }

        @Override
        public java.io.InputStream getInputStream() {
            return java.io.InputStream.nullInputStream();
        }

        @Override
        public java.io.InputStream getErrorStream() {
            return java.io.InputStream.nullInputStream();
        }

        @Override
        public int waitFor() {
            return 0;
        }

        @Override
        public int exitValue() {
            return 0;
        }

        @Override
        public void destroy() { }
    }

    /** A JVM's own kill, which is a real SIGKILL there (on the target libcore it is not: AndroidPlatform sends one by pid). */
    private static final ChildProcess.Kill JDK_KILL = Process::destroyForcibly;

    private static void children() {
        row("child-output-is-drained", () -> {
            // 330 kB of output and status 3: a parent that does not read while it waits stalls the child on a full pipe.
            ChildProcess.Result result = ChildProcess.run(sh("i=0; while [ $i -lt 30000 ]; do echo 0123456789; i=$((i+1)); done; exit 3"),
                    4_000, 4096, JDK_KILL);
            same(3, result.exit, "the child's status");
            same(4096, result.output.length(), "the first bytes are kept and no more");
            check(result.output.startsWith("0123456789\n"), "the output is the child's");
        });
        row("child-output-merges-error", () -> {
            ChildProcess.Result result = ChildProcess.run(sh("echo out; echo err >&2"), 4_000, 4096, JDK_KILL);
            same(0, result.exit, "status");
            check(result.output.contains("out") && result.output.contains("err"), "both streams: " + show(result.output));
        });
        row("child-deadline-terminates-and-reaps", () -> {
            java.io.File pid = pidFile();
            try {
                ChildProcess.run(sh("echo $$ > \"$1\"; exec sleep 60", pid.getPath()), 600, 4096, JDK_KILL);
                throw new Mismatch("a child past the deadline is a failure");
            } catch (PlatformException late) {
                same("cmd activity start-activity did not finish", late.getMessage(), "the reason is fixed");
            }
            check(!alive(pid), "the child is gone when run returns");
        });
        row("child-that-ignores-termination-is-killed", () -> {
            java.io.File pid = pidFile();
            long begin = System.nanoTime();
            try {
                ChildProcess.run(sh(STUBBORN, pid.getPath()), 600, 4096, JDK_KILL);
                throw new Mismatch("a child past the deadline is a failure");
            } catch (PlatformException late) {
                check(late.getMessage() != null, "failed");
            }
            long tookMillis = (System.nanoTime() - begin) / 1_000_000;
            check(!alive(pid), "SIGKILL ended it");
            check(tookMillis < 600 + 2 * ChildProcess.GRACE_MILLIS + 2_000, "bounded: " + tookMillis + " ms");
        });
        row("child-kill-is-asked-only-when-termination-did-not-end-it", () -> {
            int[] asked = new int[1];
            ChildProcess.Kill counting = process -> {
                asked[0]++;
                process.destroyForcibly();
            };
            java.io.File pid = pidFile();
            try {
                ChildProcess.run(sh("echo $$ > \"$1\"; exec sleep 60", pid.getPath()), 600, 4096, counting, 300);
            } catch (PlatformException late) {
                check(late.getMessage() != null, "the deadline");
            }
            same(0, asked[0], "SIGTERM ended it: no kill");
            java.io.File stubborn = pidFile();
            try {
                ChildProcess.run(sh(STUBBORN, stubborn.getPath()), 600, 4096, counting, 300);
            } catch (PlatformException late) {
                check(late.getMessage() != null, "the deadline");
            }
            same(1, asked[0], "it ignored SIGTERM: one kill");
            check(!alive(stubborn), "and it is gone");
        });
        row("child-is-not-signalled-after-it-has-ended", () -> {
            // The child ends between the timed wait and the kill (its pid may be another process's by then): no kill is
            // asked, on the deadline path and on the interrupted one. A child that stays alive is asked once.
            int[] asked = new int[1];
            ChildProcess.Kill counting = process -> asked[0]++;
            same(true, ChildProcess.terminate(new ScriptedChild(false, false, false), counting, 50), "ended in the wait: gone");
            same(0, asked[0], "ended during the wait: no kill");
            same(true, ChildProcess.terminate(new ScriptedChild(true, false, false), counting, 50), "ended before the interrupt was seen");
            Thread.interrupted(); // the interrupted path sets the flag again
            same(0, asked[0], "ended before the interrupt was seen: no kill");
            same(false, ChildProcess.terminate(new ScriptedChild(false, true, true), counting, 50), "alive after the kill: not gone");
            same(1, asked[0], "alive at the deadline: one kill");
            asked[0] = 0;
            same(false, ChildProcess.terminate(new ScriptedChild(true, true, true), counting, 50), "alive when interrupted: not gone");
            Thread.interrupted();
            same(1, asked[0], "alive when interrupted: one kill");
        });
        row("child-that-survives-the-kill-is-reported", () -> {
            // A kill that does not work, as destroyForcibly is not one on the target libcore: the final wait says so.
            Set<Long> before = childPids();
            java.io.File pid = pidFile();
            try {
                ChildProcess.run(sh(STUBBORN, pid.getPath()), 600, 4096,
                        process -> { }, 300);
                throw new Mismatch("a child that is still alive is a failure");
            } catch (PlatformException still) {
                same("cmd activity start-activity could not be ended", still.getMessage(), "the reason is fixed");
            } finally {
                // The child is this JVM's: it is ended through the handle it holds, and seen gone, before the row returns.
                List<ProcessHandle> survivors = startedSince(before);
                boolean survivor;
                Mismatch unidentified = null;
                try {
                    survivor = survivorAmong(survivors, pid);
                } catch (Mismatch refused) {
                    survivor = false;
                    unidentified = refused;
                }
                boolean ended = endAll(survivors);
                if (unidentified != null) {
                    throw unidentified;
                }
                check(ended, "the survivor was ended");
                check(survivor, "the child was alive when the run said so");
            }
        });
        row("a-row-whose-processes-cannot-be-listed-is-an-error-and-no-failure", () -> {
            // A listing that fails every time is ListingFailed; a row that meets it is ERROR (which the driver does not
            // credit a mutant with) and not FAIL; a row that fails is FAIL; one that passes is ok.
            same("ok", outcome(() -> { }).kind, "a row that passes");
            same("FAIL", outcome(() -> {
                throw new Mismatch("wrong");
            }).kind, "a row that fails");
            Outcome unchecked = outcome(() -> {
                throw new ListingFailed("injected", new RuntimeException("Cannot allocate memory"));
            });
            same("ERROR", unchecked.kind, "a row whose processes cannot be listed");
            check(unchecked.text.startsWith("could not be checked: injected"), "it says so: " + unchecked.text);
            int[] asked = new int[1];
            boolean exhausted = false;
            try {
                listing(() -> {
                    asked[0]++;
                    throw new RuntimeException("Cannot allocate memory");
                }, 3);
            } catch (ListingFailed expected) {
                exhausted = true;
            }
            check(exhausted, "a listing that fails every attempt is a ListingFailed");
            same(3, asked[0], "it was asked as often as it may be");
            asked[0] = 0;
            same(0, listing(() -> {
                if (asked[0]++ < 2) {
                    throw new RuntimeException("Cannot allocate memory");
                }
                return java.util.stream.Stream.empty();
            }, 3).size(), "a listing that fails and then answers is the answer");
        });
        row("child-survivor-is-told-by-its-recorded-pid", () -> {
            // The observation of a survivor is of the child that wrote its pid, not of any new child: an empty pid file
            // identifies nothing, and a pid that is not a new live child of this JVM's is no survivor.
            Set<Long> before = childPids();
            java.io.File empty = pidFile();
            java.io.File recorded = pidFile();
            Process other = new ProcessBuilder(sh("exec sleep 600")).start();
            try {
                List<ProcessHandle> started = startedSince(before);
                same(1, started.size(), "one new child");
                boolean refused = false;
                try {
                    survivorAmong(started, empty);
                } catch (Mismatch expected) {
                    refused = true;
                }
                check(refused, "an empty pid file identifies no survivor");
                java.nio.file.Files.write(recorded.toPath(), (ProcessHandle.current().pid() + "\n").getBytes(StandardCharsets.US_ASCII));
                same(false, survivorAmong(started, recorded), "a pid that is not a new child of this JVM is no survivor");
                java.nio.file.Files.write(recorded.toPath(), (other.pid() + "\n").getBytes(StandardCharsets.US_ASCII));
                same(true, survivorAmong(started, recorded), "the recorded pid of a live new child is the survivor");
            } finally {
                other.destroyForcibly();
                other.waitFor();
            }
        });
        row("child-kill-that-throws-leaves-the-wait-to-decide", () -> {
            Set<Long> before = childPids();
            java.io.File pid = pidFile();
            try {
                ChildProcess.run(sh(STUBBORN, pid.getPath()), 600, 4096,
                        process -> {
                            throw new IllegalStateException("no such field");
                        }, 300);
                throw new Mismatch("a child that is still alive is a failure");
            } catch (PlatformException still) {
                same("cmd activity start-activity could not be ended", still.getMessage(), "the reason is fixed");
            } finally {
                check(endAll(startedSince(before)), "the survivor was ended");
            }
        });
        row("child-is-reaped-when-the-run-is-interrupted", () -> {
            java.io.File pid = pidFile();
            Throwable[] ended = new Throwable[1];
            Thread runner = new Thread(() -> {
                try {
                    ChildProcess.run(sh("echo $$ > \"$1\"; exec sleep 60", pid.getPath()), 30_000, 4096, JDK_KILL);
                } catch (Throwable problem) {
                    ended[0] = problem;
                }
            });
            runner.start();
            while (pid.length() == 0 && runner.isAlive()) {
                Thread.sleep(20);
            }
            Thread.sleep(100);
            runner.interrupt();
            runner.join(10_000);
            check(!runner.isAlive(), "the run ended");
            check(ended[0] instanceof InterruptedException, "by the interrupt: " + ended[0]);
            check(!alive(pid), "the child is gone");
        });
        row("child-that-cannot-start-fails", () -> {
            try {
                ChildProcess.run(Arrays.asList("/nonexistent/kernarium-apps-test"), 1_000, 4096, JDK_KILL);
                throw new Mismatch("no such program");
            } catch (java.io.IOException expected) {
                check(true, "an exception, not a result");
            }
        });
    }

    private static void sources() {
        row("resource-paths-follow-the-framework", () -> {
            List<ResourcePaths.Item> items = ResourcePaths.of("/data/app/a/base.apk", "/data/app/a/private.apk",
                    new String[] {"/data/app/a/split_one.apk", "", null, "/data/app/a/split_two.apk"},
                    new String[] {"/data/app/a/private_split.apk"},
                    new String[] {"/system/framework/lib.jar", "/data/app/lib/lib.apk", null},
                    new String[] {"/o/legacy.apk", "/o/both.idmap"},
                    new String[] {"/o/both.idmap", "/o/new.idmap"});
            same(Arrays.asList("BASE /data/app/a/base.apk", "SPLIT /data/app/a/split_one.apk", "SPLIT /data/app/a/split_two.apk",
                    "LIBRARY /data/app/lib/lib.apk", "OVERLAY /o/both.idmap", "OVERLAY /o/new.idmap", "OVERLAY /o/legacy.apk"),
                    describe(items), "base, splits, libraries (APKs only), overlay paths in their order, then the legacy "
                    + "directories that are missing (each once)");
            check(ResourcePaths.hasBase(items), "it can be opened");
            same(Arrays.asList("BASE /data/app/a/private.apk", "SPLIT /data/app/a/private_split.apk"),
                    describe(ResourcePaths.of(null, "/data/app/a/private.apk", null, new String[] {"/data/app/a/private_split.apk"},
                            null, null, null)), "the source directories when no public ones are given");
            check(!ResourcePaths.hasBase(ResourcePaths.of(null, null, new String[] {"/x.apk"}, null, null, null, null)),
                    "no base, no resources");
            check(!ResourcePaths.hasBase(ResourcePaths.of("", "", null, null, null, null, null)), "an empty base");
            String key = ResourcePaths.fingerprint(items);
            same(key, ResourcePaths.fingerprint(items), "stable");
            check(key.matches("[0-9a-f]{16}"), "16 hex characters");
            check(!key.equals(ResourcePaths.fingerprint(ResourcePaths.of("/data/app/a/base.apk", null, null, null, null, null, null))),
                    "it follows the list");
            check(!ResourcePaths.fingerprint(ResourcePaths.of("/a.apk", null, new String[] {"/b.apk"}, null, null, null, null))
                    .equals(ResourcePaths.fingerprint(ResourcePaths.of("/a.apk", null, null, null, new String[] {"/b.apk"}, null, null))),
                    "a split is not a library");
        });
    }

    private static List<String> describe(List<ResourcePaths.Item> items) {
        List<String> described = new ArrayList<>();
        for (ResourcePaths.Item item : items) {
            described.add(item.kind + " " + item.path);
        }
        return described;
    }
}
