// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the shell tool (kernarium_apps) and the icon renderer (KernariumIcons) agree on, on a plain JVM: the shell
// user's uid, the shape of a request (the resource id and the files, in the order the framework adds them), a
// path's rules, the sizes and bounds, and the layers' outset and alpha rule; and the provider's hold on the isolated
// worker (IconWorkerCore) over stand-ins for the platform's bind, its main thread and a worker that answers, hangs or
// dies: one deadline for a request from its arrival, a failed or slow worker unbound, an idle worker unbound without
// ever making the main thread wait for the turn of a request.
// Each row prints "ok <name>" or "FAIL <name>: why"; the last line is PASS when no row failed.
// test_icon_renderer.py compiles and runs it, and runs it again on a copy of IconProtocol or IconWorkerCore with one
// rule removed at a time, where the rows named there must fail.

package com.kernarium.icons;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class IconProtocolTest {
    private static final class Mismatch extends RuntimeException {
        private static final long serialVersionUID = 1L;

        Mismatch(String message) {
            super(message);
        }
    }

    private interface Row {
        void run() throws Exception;
    }

    private static int failures;

    private static void row(String name, Row body) {
        try {
            body.run();
            System.out.println("ok " + name);
        } catch (Throwable problem) {
            failures++;
            System.out.println("FAIL " + name + ": " + problem);
        }
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new Mismatch(what);
        }
    }

    private static void same(Object expected, Object actual, String what) {
        if (!expected.equals(actual)) {
            throw new Mismatch(what + ": expected " + expected + ", got " + actual);
        }
    }

    private static String[] paths(int count) {
        String[] paths = new String[count];
        for (int i = 0; i < count; i++) {
            paths[i] = "/data/app/p" + i + "/base.apk";
        }
        return paths;
    }

    private static boolean valid(int[] kinds) {
        return IconProtocol.validRequest(0x7f010000, kinds, paths(kinds.length));
    }

    private static String repeat(char c, int count) {
        char[] chars = new char[count];
        Arrays.fill(chars, c);
        return new String(chars);
    }

    public static void main(String[] args) {
        names();
        shell();
        request();
        paths();
        sizes();
        drawing();
        worker();
        System.out.println(failures == 0 ? "PASS" : "FAILED " + failures);
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void names() {
        row("protocol-names-are-the-contract", () -> {
            same("com.kernarium.icons", IconProtocol.PACKAGE, "package");
            same("com.kernarium.icons", IconProtocol.AUTHORITY, "authority");
            same("com.kernarium.icons.IconRenderService", IconProtocol.SERVICE_CLASS, "service");
            same("com.kernarium.icons.IconProvider", IconProtocol.PROVIDER_CLASS, "provider");
            same("render", IconProtocol.METHOD_RENDER, "method");
            same(0, IconProtocol.STATUS_OK, "ok");
            same(1, IconProtocol.STATUS_ERROR, "error");
            same(Arrays.asList(0, 1, 2, 3), Arrays.asList(IconProtocol.KIND_BASE, IconProtocol.KIND_SPLIT,
                    IconProtocol.KIND_LIBRARY, IconProtocol.KIND_OVERLAY), "the kinds, in the order the framework adds them");
            check(new java.util.HashSet<>(Arrays.asList(IconProtocol.KEY_RESOURCE, IconProtocol.KEY_KINDS,
                    IconProtocol.KEY_PATHS, IconProtocol.KEY_STATUS, IconProtocol.KEY_SIZE, IconProtocol.KEY_ADAPTIVE,
                    IconProtocol.KEY_ALPHA, IconProtocol.KEY_PNG)).size() == 8, "eight keys, all different");
        });
    }

    private static void shell() {
        row("protocol-shell-uid", () -> {
            check(IconProtocol.fromShell(2000), "the shell user");
            for (int other : new int[] {0, 1000, 1001, 1999, 2001, 10000, 10125, 12000, 99000, 100000, 1000000, 99009, -1,
                -2000, -98000, 102000, 1002000, 2000 + 100000 * 9, Integer.MAX_VALUE, Integer.MIN_VALUE}) {
                check(!IconProtocol.fromShell(other), "uid " + other + " is not the adb shell user's");
            }
            same(2000, IconProtocol.SHELL_UID, "the adb shell user's uid");
        });
    }

    private static void request() {
        row("request-shapes-that-are-accepted", () -> {
            check(valid(new int[] {0}), "a base alone");
            check(valid(new int[] {0, 1, 1, 2, 3, 3}), "base, splits, library, overlays in that order");
            check(valid(new int[] {0, 3}), "a base and an overlay");
            check(valid(new int[] {0, 2}), "a base and a library");
            int[] most = new int[128];
            Arrays.fill(most, 1, most.length, 3);
            check(valid(most), "128 files");
            check(IconProtocol.validRequest(Integer.MAX_VALUE, new int[] {0}, paths(1)), "the largest resource id");
            check(IconProtocol.validRequest(1, new int[] {0}, paths(1)), "the smallest resource id");
            check(IconProtocol.validRequest(0x80010000, new int[] {0}, paths(1)), "a drawable of resource package 0x80");
            check(IconProtocol.validRequest(0xFF7F0000, new int[] {0, 3}, paths(2)), "a resource package 0xFF");
        });
        row("request-needs-a-resource", () -> {
            check(!IconProtocol.validRequest(0, new int[] {0}, paths(1)), "resource 0 is no resource");
        });
        row("request-resource-ids-with-the-sign-bit-are-valid", () -> {
            check(IconProtocol.validRequest(-1, new int[] {0}, paths(1)), "resource -1 (0xFFFFFFFF) is for the lookup to judge");
            check(IconProtocol.validRequest(Integer.MIN_VALUE, new int[] {0}, paths(1)), "the least resource id, 0x80000000");
            check(IconProtocol.validRequest(0x80000001, new int[] {0}, paths(1)), "0x80000001");
        });
        row("request-arrays-agree", () -> {
            check(!IconProtocol.validRequest(1, null, paths(1)), "no kinds");
            check(!IconProtocol.validRequest(1, new int[] {0}, null), "no paths");
            check(!IconProtocol.validRequest(1, new int[] {0, 3}, paths(1)), "more kinds than paths");
            check(!IconProtocol.validRequest(1, new int[] {0}, paths(2)), "more paths than kinds");
            check(!IconProtocol.validRequest(1, new int[0], new String[0]), "no files");
        });
        row("request-file-count-is-bounded", () -> {
            int[] over = new int[129];
            Arrays.fill(over, 1, over.length, 3);
            check(!valid(over), "129 files");
        });
        row("request-starts-with-exactly-one-base", () -> {
            check(!valid(new int[] {1}), "a split first");
            check(!valid(new int[] {3, 0}), "an overlay before the base");
            check(!valid(new int[] {2}), "a library first");
            check(!valid(new int[] {0, 0}), "two bases");
            check(!valid(new int[] {0, 1, 0}), "a base after a split");
        });
        row("request-kinds-are-the-known-ones", () -> {
            check(!valid(new int[] {0, 4}), "a kind past the overlays");
            check(!valid(new int[] {0, -1}), "a negative kind");
            check(!valid(new int[] {0, Integer.MAX_VALUE}), "an absurd kind");
        });
        row("request-order-is-the-frameworks", () -> {
            check(!valid(new int[] {0, 3, 1}), "a split after an overlay");
            check(!valid(new int[] {0, 2, 1}), "a split after a library");
            check(!valid(new int[] {0, 3, 2}), "a library after an overlay");
            check(valid(new int[] {0, 1, 2, 3}), "each kind once, in order");
        });
        row("request-names-valid-paths-only", () -> {
            for (String bad : new String[] {null, "", "/", "relative/base.apk", "/a/../b.apk"}) {
                String[] one = {bad};
                check(!IconProtocol.validRequest(1, new int[] {0}, one), "the path " + bad);
            }
            String[] second = {"/a/base.apk", "../x"};
            check(!IconProtocol.validRequest(1, new int[] {0, 3}, second), "a bad path that is not the first");
        });
    }

    private static void paths() {
        row("path-accepts", () -> {
            for (String good : new String[] {"/a", "/data/app/~~x==/com.a-y==/base.apk", "/a b", "/a/..b", "/a/.hidden",
                "/a/b..", "/product/overlay/A.apk", "/~", "/" + repeat('a', 1023)}) {
                check(IconProtocol.validPath(good), "the path " + good.substring(0, Math.min(40, good.length())));
            }
        });
        row("path-is-absolute-and-not-empty", () -> {
            check(!IconProtocol.validPath(null), "null");
            check(!IconProtocol.validPath(""), "the empty path");
            check(!IconProtocol.validPath("/"), "the root alone");
            check(!IconProtocol.validPath("a"), "one character, relative");
            check(!IconProtocol.validPath("ab"), "two characters, relative");
            check(!IconProtocol.validPath("data/app/base.apk"), "relative");
        });
        row("path-length-is-bounded", () -> {
            check(!IconProtocol.validPath("/" + repeat('a', 1024)), "1025 characters");
            same(1024, IconProtocol.MAX_PATH_CHARS, "the longest path");
        });
        row("path-is-printable-ascii", () -> {
            for (char bad : new char[] {0, 0x09, 0x0A, 0x0D, 0x1F, 0x7F, 0x80, 0xA0, 0xE9, 0x2028, 0xFFFF}) {
                check(!IconProtocol.validPath("/a" + bad + "b"), "the character U+" + Integer.toHexString(bad));
            }
            check(IconProtocol.validPath("/a b"), "a space is printable");
            check(IconProtocol.validPath("/a~b"), "a tilde is printable");
            check(!IconProtocol.validPath("/a😀b"), "an emoji");
        });
        row("path-has-no-empty-or-dot-segments", () -> {
            for (String bad : new String[] {"//a", "/a//b", "/a/", "/a/./b", "/./a", "/a/.", "/a/../b", "/../a", "/a/.."}) {
                check(!IconProtocol.validPath(bad), "the path " + bad);
            }
        });
    }

    private static void sizes() {
        row("sizes-and-bounds", () -> {
            same(Arrays.asList(192, 128, 96), Arrays.asList(IconProtocol.SIZES[0], IconProtocol.SIZES[1], IconProtocol.SIZES[2]),
                    "the sizes, largest first");
            same(3, IconProtocol.SIZES.length, "three sizes");
            same(98304, IconProtocol.MAX_PNG_BYTES, "the PNG bound");
            same(128, IconProtocol.MAX_FILES, "the file bound");
            same(640, IconProtocol.DENSITY, "the density");
        });
        row("size-steps-down-through-the-list", () -> {
            same(128, IconProtocol.smaller(192), "after 192");
            same(96, IconProtocol.smaller(128), "after 128");
            same(0, IconProtocol.smaller(96), "after 96 there is none");
            same(0, IconProtocol.smaller(100), "a size that is not one of them");
            same(0, IconProtocol.smaller(0), "zero");
            same(0, IconProtocol.smaller(256), "a larger one");
        });
    }

    private static void drawing() {
        row("layer-outset-is-a-quarter", () -> {
            same(48, IconProtocol.outset(192), "at 192");
            same(32, IconProtocol.outset(128), "at 128");
            same(24, IconProtocol.outset(96), "at 96");
            same(0, IconProtocol.outset(0), "at 0");
        });
        row("alpha-is-any-pixel-not-opaque", () -> {
            check(!IconProtocol.hasAlpha(new int[0]), "no pixels");
            check(!IconProtocol.hasAlpha(new int[] {0xFF000000, 0xFFFFFFFF, 0xFF123456}), "all opaque");
            check(IconProtocol.hasAlpha(new int[] {0xFF000000, 0x00000000}), "one transparent");
            check(IconProtocol.hasAlpha(new int[] {0xFF000000, 0xFEFFFFFF}), "one pixel barely transparent");
            check(IconProtocol.hasAlpha(new int[] {0x80FF0000}), "a half transparent pixel");
            check(IconProtocol.hasAlpha(new int[] {0x01FFFFFF}), "an almost invisible one");
        });
    }

    // ---- The provider's hold on the isolated worker, over stand-ins.

    private static final IconWorkerCore.Limits LIMITS = new IconWorkerCore.Limits(800, 200, 300);
    /** The same, with an idle time that no row waits for: an unbind that a row sees is the failure's, not the idle cleanup's. */
    private static final IconWorkerCore.Limits LONG_IDLE = new IconWorkerCore.Limits(800, 200, 60_000);
    private static final long SLACK = 350;

    private static byte[] png(int length) {
        return new byte[length];
    }

    private static IconWorkerCore.Reply good() {
        return new IconWorkerCore.Reply(IconProtocol.STATUS_OK, 192, true, false, png(100));
    }

    /** The platform as a worker sees it: a bind that connects on a "main" thread, a worker and a poster. */
    private static final class Rig {
        final ScheduledExecutorService main = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "main-stand-in");
            thread.setDaemon(true);
            return thread;
        });
        final AtomicInteger binds = new AtomicInteger();
        final AtomicInteger unbinds = new AtomicInteger();
        final AtomicInteger renders = new AtomicInteger();
        /** Unbinds that have begun and not returned, and binds that began while one of them had not returned. */
        final AtomicInteger unbinding = new AtomicInteger();
        final AtomicInteger boundDuringUnbind = new AtomicInteger();
        final AtomicInteger running = new AtomicInteger();
        final AtomicInteger mostRunning = new AtomicInteger();
        final List<IconWorkerCore.Listener> listeners = new ArrayList<>();
        final List<Runnable> posted = new ArrayList<>();
        final List<Runnable> removed = new ArrayList<>();
        final Map<Runnable, ScheduledFuture<?>> futures = new IdentityHashMap<>();
        /** Whether a posted idle cleanup runs by itself after its delay (else the row runs it). */
        volatile boolean autoIdle = true;
        /** When set, the first post parks the posting (request) thread here, still holding its turn, until released. */
        volatile CountDownLatch parkFirstPost;
        final CountDownLatch firstPostParked = new CountDownLatch(1);
        final java.util.concurrent.atomic.AtomicBoolean firstPost = new java.util.concurrent.atomic.AtomicBoolean();
        volatile boolean connects = true;
        volatile boolean refuses;
        volatile long connectMillis = 20;
        volatile long drawMillis = 20;
        volatile boolean hangs;
        volatile boolean throwsOnRender;
        volatile long unbindMillis;
        /** How long the platform's call to begin a bind takes, with the turn held. */
        volatile long bindCallMillis;
        /** Time that passes while a bind connects, so that a bind may take what is left of a deadline. */
        volatile long clockJumpAtConnect;
        volatile long clockOffset;
        volatile boolean alive = true;
        volatile IconWorkerCore.Reply reply = good();
        /** Counted down by an unbind: a hung worker dies with its process. */
        volatile CountDownLatch died = new CountDownLatch(1);
        final IconWorkerCore.Session session = new IconWorkerCore.Session() {
            @Override
            public IconWorkerCore.Reply render(int resource, int[] kinds, String[] paths) throws Exception {
                renders.incrementAndGet();
                int now = running.incrementAndGet();
                mostRunning.accumulateAndGet(now, Math::max);
                try {
                    if (throwsOnRender) {
                        throw new IllegalStateException("the worker died");
                    }
                    if (hangs) {
                        died.await();
                        throw new IllegalStateException("the worker's process ended");
                    }
                    Thread.sleep(drawMillis);
                    return reply;
                } finally {
                    running.decrementAndGet();
                }
            }

            @Override
            public boolean alive() {
                return alive;
            }
        };
        final IconWorkerCore.Link link = listener -> {
            binds.incrementAndGet();
            if (unbinding.get() > 0) {
                boundDuringUnbind.incrementAndGet();
            }
            synchronized (listeners) {
                listeners.add(listener);
            }
            if (bindCallMillis > 0) {
                try {
                    Thread.sleep(bindCallMillis);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                }
            }
            if (refuses) {
                return null;
            }
            if (connects) {
                main.schedule(() -> {
                    clockOffset += clockJumpAtConnect;
                    listener.connected(session);
                }, connectMillis, TimeUnit.MILLISECONDS);
            }
            return () -> {
                unbinds.incrementAndGet();
                unbinding.incrementAndGet();
                died.countDown();
                try {
                    Thread.sleep(unbindMillis);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                } finally {
                    unbinding.decrementAndGet();
                }
            };
        };
        final IconWorkerCore.Poster poster = new IconWorkerCore.Poster() {
            @Override
            public void postDelayed(Runnable task, long millis) {
                synchronized (posted) {
                    posted.add(task);
                }
                if (autoIdle) {
                    futures.put(task, main.schedule(task, millis, TimeUnit.MILLISECONDS));
                }
                CountDownLatch park = parkFirstPost;
                if (park != null && firstPost.compareAndSet(false, true)) {
                    firstPostParked.countDown();
                    try {
                        park.await();
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                    }
                }
            }

            @Override
            public void remove(Runnable task) {
                synchronized (removed) {
                    removed.add(task);
                }
                ScheduledFuture<?> future = futures.get(task);
                if (future != null) {
                    future.cancel(false);
                }
            }
        };

        IconWorkerCore core(IconWorkerCore.Limits limits) {
            return new IconWorkerCore(link, poster, () -> System.nanoTime() / 1_000_000 + clockOffset, limits);
        }

        Runnable lastPosted() {
            synchronized (posted) {
                return posted.get(posted.size() - 1);
            }
        }

        void shutdown() {
            died.countDown();
            main.shutdownNow();
        }
    }

    /** The result of one request run on a thread of its own: what it answered and how long it took. */
    private static final class Call {
        volatile IconWorkerCore.Reply reply;
        volatile long millis = -1;
        final Thread thread;

        Call(IconWorkerCore core, long startAfterMillis) {
            thread = new Thread(() -> {
                try {
                    Thread.sleep(startAfterMillis);
                    long begin = System.nanoTime();
                    reply = core.render(0x7f010000, new int[] {0}, new String[] {"/a/base.apk"});
                    millis = (System.nanoTime() - begin) / 1_000_000;
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                }
            }, "request");
            thread.setDaemon(true);
            thread.start();
        }

        Call await() throws InterruptedException {
            thread.join(10_000);
            check(millis >= 0, "the request did not end");
            return this;
        }

        boolean ok() {
            return reply.ok();
        }
    }

    /** A clock that reads what a row scripts, one reading per call, then the last one again; the real one when none. */
    private static final class ScriptedClock implements IconWorkerCore.Clock {
        volatile long[] script;
        final AtomicInteger readings = new AtomicInteger();

        void script(long... values) {
            readings.set(0);
            script = values;
        }

        @Override
        public long now() {
            long[] values = script;
            if (values == null) {
                return System.nanoTime() / 1_000_000;
            }
            return values[Math.min(readings.getAndIncrement(), values.length - 1)];
        }
    }

    private static IconWorkerCore.Reply ask(IconWorkerCore core) {
        return core.render(0x7f010000, new int[] {0}, new String[] {"/a/base.apk"});
    }

    private static void waitFor(java.util.function.BooleanSupplier condition, long millis, String what) throws Exception {
        long end = System.nanoTime() + millis * 1_000_000;
        while (!condition.getAsBoolean()) {
            check(System.nanoTime() < end, what);
            Thread.sleep(10);
        }
    }

    private static void worker() {
        row("worker-answers-and-keeps-a-good-worker", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                IconWorkerCore.Reply first = ask(core);
                check(first.ok() && first.size == 192 && first.adaptive && !first.alpha && first.png.length == 100, "the reply's fields");
                check(ask(core).ok(), "the second request");
                same(1, rig.binds.get(), "one bind for two requests");
                same(2, rig.renders.get(), "two draws");
                same(0, rig.unbinds.get(), "a good worker stays bound");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-rebinds-a-dead-binder", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                check(ask(core).ok(), "the first");
                rig.alive = false;
                check(ask(core).ok(), "the second is drawn by a new bind");
                same(2, rig.binds.get(), "a binder that is not alive is bound again");
                waitFor(() -> rig.unbinds.get() >= 1, 3000, "the dead bind is released");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-failure-unbinds-and-the-next-request-binds-again", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LONG_IDLE);
                rig.reply = IconWorkerCore.Reply.FAILED;
                check(!ask(core).ok(), "a failed drawing is a failed reply");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the worker that failed is unbound");
                rig.reply = good();
                check(ask(core).ok(), "the next");
                same(2, rig.binds.get(), "a fresh bind");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-an-exception-is-a-failure", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LONG_IDLE);
                rig.throwsOnRender = true;
                check(!ask(core).ok(), "a worker that died");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "unbound");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-reply-over-the-bound-is-not-passed-on", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                rig.reply = new IconWorkerCore.Reply(IconProtocol.STATUS_OK, 192, true, false, png(IconProtocol.MAX_PNG_BYTES));
                check(ask(core).ok(), "a PNG of exactly the bound");
                rig.reply = new IconWorkerCore.Reply(IconProtocol.STATUS_OK, 192, true, false, png(IconProtocol.MAX_PNG_BYTES + 1));
                check(!ask(core).ok(), "a PNG one byte over the bound");
                rig.reply = new IconWorkerCore.Reply(IconProtocol.STATUS_OK, 192, true, false, null);
                check(!ask(core).ok(), "no PNG");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-hung-drawing-fails-at-the-deadline-and-unbinds", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LONG_IDLE);
                rig.hangs = true;
                Call call = new Call(core, 0).await();
                check(!call.ok(), "a hung drawing is a failure");
                check(call.millis >= LIMITS.deadlineMillis - 100 && call.millis <= LIMITS.deadlineMillis + SLACK,
                        "it ends at the deadline: " + call.millis + " ms");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the hung worker is unbound, which ends it");
                rig.hangs = false;
                rig.died = new CountDownLatch(1);
                check(ask(core).ok(), "the next request is drawn by a new worker");
                same(2, rig.binds.get(), "a new bind");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-one-deadline-covers-the-wait-for-the-turn", () -> {
            // Two overlapping draws of 500 ms with a deadline of 800 ms: the second request can start at about 500 ms
            // and cannot end by 800 ms (its own deadline is at about 850 ms), so it fails by then and not at 1000 ms.
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                rig.drawMillis = 500;
                Call first = new Call(core, 0);
                Call second = new Call(core, 50);
                first.await();
                second.await();
                check(first.ok(), "the first is drawn");
                check(!second.ok(), "the second cannot end within its deadline and fails");
                check(second.millis <= LIMITS.deadlineMillis + SLACK, "it failed by its deadline: " + second.millis + " ms");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-request-that-queued-past-its-deadline-never-draws", () -> {
            // The first asks the platform for its bind and that call takes 2 s, so the turn is held well past the second's
            // deadline (asked 100 ms in, its deadline is at 900 ms): the second fails by then and is not served at all.
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                rig.bindCallMillis = 2000;
                Call first = new Call(core, 0);
                Call second = new Call(core, 100);
                second.await();
                check(!second.ok(), "the second fails");
                check(second.millis <= LIMITS.deadlineMillis + SLACK,
                        "by its deadline, not when the turn came free: " + second.millis + " ms");
                first.await();
                same(0, rig.renders.get(), "nothing was ever drawn");
                same(1, rig.binds.get(), "the second never asked for a bind");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-deadline-that-has-passed-starts-nothing", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(new IconWorkerCore.Limits(0, 200, 300));
                check(!ask(core).ok(), "no time at all");
                same(0, rig.renders.get(), "nothing is drawn");
                same(0, rig.binds.get(), "nothing is bound");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-requests-draw-one-at-a-time", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(new IconWorkerCore.Limits(3000, 200, 300));
                rig.drawMillis = 60;
                List<Call> calls = new ArrayList<>();
                for (int i = 0; i < 4; i++) {
                    calls.add(new Call(core, i * 5));
                }
                for (Call call : calls) {
                    check(call.await().ok(), "each is drawn");
                }
                same(1, rig.mostRunning.get(), "never two draws at once");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-bind-timeout-fails-and-releases", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LONG_IDLE);
                rig.connects = false;
                Call call = new Call(core, 0).await();
                check(!call.ok(), "a bind that never connects");
                check(call.millis >= LIMITS.bindMillis - 50 && call.millis <= LIMITS.bindMillis + SLACK,
                        "it ends at the bind limit: " + call.millis + " ms");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the bind is released");
                same(0, rig.renders.get(), "nothing was drawn");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-bind-is-capped-by-the-deadline", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(new IconWorkerCore.Limits(300, 3000, 300));
                rig.connects = false;
                Call call = new Call(core, 0).await();
                check(!call.ok(), "a bind that never connects");
                check(call.millis <= 300 + SLACK, "the bind wait is the deadline's what is left: " + call.millis + " ms");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-bind-that-takes-the-rest-of-the-deadline-draws-nothing", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                rig.clockJumpAtConnect = LIMITS.deadlineMillis;
                check(!ask(core).ok(), "the bind used the whole deadline");
                same(0, rig.renders.get(), "nothing is drawn after it");
                same(0, rig.unbinds.get(), "the worker that connected is not blamed");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-no-time-left-after-the-bind-starts-no-draw", () -> {
            // Nothing left, or less than nothing, once the worker is bound: an immediate timeout. No draw is started,
            // nothing is joined (a join of 0 waits for ever, a negative one throws), the worker stays bound and idle.
            for (long over : new long[] {0, 1}) {
                Rig rig = new Rig();
                try {
                    ScriptedClock clock = new ScriptedClock();
                    IconWorkerCore core = new IconWorkerCore(rig.link, rig.poster, clock, LIMITS);
                    check(ask(core).ok(), "a first request binds the worker");
                    int drawn = rig.renders.get();
                    rig.hangs = true;
                    long start = 10_000;
                    // readings: the deadline's start, the wait for the turn, the check after admission, then what is left
                    clock.script(start, start, start, start + LIMITS.deadlineMillis + over);
                    Call call = new Call(core, 0).await();
                    check(!call.ok(), "no time left is a failure, not an exception");
                    same(drawn, rig.renders.get(), "no draw was started");
                    same(0, rig.unbinds.get(), "the worker is left bound: it was not asked");
                    clock.script = null;
                    rig.hangs = false;
                    check(ask(core).ok(), "the next icon is still asked");
                    same(1, rig.binds.get(), "of the same worker");
                } finally {
                    rig.shutdown();
                }
            }
        });
        row("worker-the-budget-is-read-once", () -> {
            // 1 ms is left when the bind is checked and none (or less) by the time the draw would be waited for: the
            // one reading decides, so the draw is joined for that 1 ms and fails, it is not joined for nothing.
            for (long later : new long[] {0, 1}) {
                Rig rig = new Rig();
                try {
                    ScriptedClock clock = new ScriptedClock();
                    IconWorkerCore core = new IconWorkerCore(rig.link, rig.poster, clock, LIMITS);
                    check(ask(core).ok(), "a first request binds the worker");
                    rig.hangs = true;
                    long start = 10_000;
                    long almost = start + LIMITS.deadlineMillis - 1;
                    clock.script(start, start, start, almost, almost + 1 + later);
                    Call call = new Call(core, 0).await();
                    check(!call.ok(), "the hung draw fails");
                    check(call.millis < LIMITS.deadlineMillis, "at once, not when the worker ends: " + call.millis + " ms");
                    waitFor(() -> rig.unbinds.get() == 1, 3000, "a worker that did not answer is released");
                } finally {
                    rig.shutdown();
                }
            }
        });
        row("worker-a-slow-unbind-does-not-spend-the-answer-s-time", () -> {
            // A warm worker, a hung second drawing and an unbind that takes 1.2 s, with limits that leave room for it (as
            // the product's 7 s does against the tool's 8 s): the error for the second icon comes at the deadline, not after
            // the unbind, and the third icon is drawn by a new worker that is bound after the old one has ended.
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(new IconWorkerCore.Limits(3000, 500, 300));
                check(ask(core).ok(), "the first icon binds the worker");
                rig.hangs = true;
                rig.unbindMillis = 1200;
                Call second = new Call(core, 0).await();
                check(!second.ok(), "the second drawing hangs");
                check(second.millis <= 3000 + SLACK, "the error comes by the deadline, not after the unbind: " + second.millis + " ms");
                rig.hangs = false;
                Call third = new Call(core, 0).await();
                check(third.ok(), "the third icon is drawn");
                same(0, rig.boundDuringUnbind.get(), "no bind was made while the old worker was being ended");
                same(2, rig.binds.get(), "by a worker bound afresh: the failed one is not reused");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the failed worker is released");
                Thread.sleep(100);
                same(1, rig.unbinds.get(), "once");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-release-that-does-not-return-fails-the-next-request-by-its-deadline", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                check(ask(core).ok(), "the first icon binds the worker");
                rig.hangs = true;
                rig.unbindMillis = 3000;
                check(!ask(core).ok(), "the second drawing hangs");
                rig.hangs = false;
                Call third = new Call(core, 0).await();
                check(!third.ok(), "the old worker is still being ended: the third has nothing to draw with");
                check(third.millis <= LIMITS.deadlineMillis + SLACK, "it gives up by its own deadline: " + third.millis + " ms");
                same(1, rig.binds.get(), "and binds nothing while the old worker is still being ended");
                same(2, rig.renders.get(), "nothing was drawn for it");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-bind-that-is-refused-is-a-failure", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                rig.refuses = true;
                Call refused = new Call(core, 0).await();
                check(!refused.ok(), "no bind");
                check(refused.millis < LIMITS.bindMillis - 50, "it is seen at once, not after the bind limit: " + refused.millis + " ms");
                same(0, rig.renders.get(), "nothing drawn");
                rig.refuses = false;
                check(ask(core).ok(), "the next request binds");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-an-idle-worker-is-unbound", () -> {
            Rig rig = new Rig();
            try {
                IconWorkerCore core = rig.core(LIMITS);
                check(ask(core).ok(), "a request");
                same(0, rig.unbinds.get(), "not at once");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the idle worker was never unbound");
                check(ask(core).ok(), "a request after the idle cleanup");
                same(2, rig.binds.get(), "binds again");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-idle-cleanup-leaves-a-request-that-has-come-since", () -> {
            Rig rig = new Rig();
            try {
                rig.autoIdle = false;
                IconWorkerCore core = rig.core(LIMITS);
                check(ask(core).ok(), "the first");
                Runnable stale = rig.lastPosted();
                check(ask(core).ok(), "the second");
                check(rig.removed.contains(stale), "the second removes the cleanup the first scheduled");
                // The stale cleanup was dispatched already, as a late one can be: it must do nothing.
                rig.main.submit(stale).get();
                Thread.sleep(150); // an unbind is made on a thread of its own: give a wrong one the time to show
                same(0, rig.unbinds.get(), "a cleanup scheduled before the last request does not unbind its worker");
                rig.main.submit(rig.lastPosted()).get();
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the cleanup of the last request does");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-idle-cleanup-never-holds-the-main-thread-for-a-request", () -> {
            // A cleanup that has been dispatched runs on the main thread while a request waits for its bind to connect:
            // the connection is delivered on that same thread, so the cleanup must not wait for the request's turn.
            Rig rig = new Rig();
            try {
                rig.autoIdle = false;
                IconWorkerCore core = rig.core(new IconWorkerCore.Limits(2000, 600, 300));
                check(ask(core).ok(), "the first");
                Runnable cleanup = rig.lastPosted();
                rig.alive = false; // the worker died: the next request binds again
                rig.connectMillis = 100;
                Call second = new Call(core, 0);
                waitFor(() -> rig.binds.get() == 2, 3000, "the second request never bound");
                rig.alive = true;
                rig.main.submit(cleanup); // dispatched on the main thread before the connection arrives
                second.await();
                check(second.ok(), "the request's connection was delivered: it took " + second.millis + " ms");
                check(second.millis < 500, "without waiting for a timeout: " + second.millis + " ms");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-the-request-is-never-the-holder-its-own-cleanup-waits-for", () -> {
            // The request is paused right after it handed its cleanup to the platform: the cleanup is dispatched, and it
            // finds the turn free (the request has released it) and unbinds the worker, though the request is not back yet.
            Rig rig = new Rig();
            try {
                rig.parkFirstPost = new CountDownLatch(1);
                IconWorkerCore core = rig.core(LIMITS);
                Call call = new Call(core, 0);
                check(rig.firstPostParked.await(5, TimeUnit.SECONDS), "the request is paused after posting its cleanup");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the cleanup waits for the request that scheduled it");
                rig.parkFirstPost.countDown();
                call.await();
                check(call.ok(), "the request was answered");
                Thread.sleep(250);
                same(1, rig.unbinds.get(), "exactly once");
                check(ask(core).ok(), "and the next request binds again");
                same(2, rig.binds.get(), "a new worker");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-cleanup-is-not-lost-however-long-the-last-request-is-paused", () -> {
            // After one successful drawing the request is paused after it posts its idle cleanup; the delay and every retry
            // of the cleanup run out; then the request resumes and no more icons come. The worker is unbound, once.
            Rig rig = new Rig();
            try {
                rig.autoIdle = false;
                rig.parkFirstPost = new CountDownLatch(1);
                IconWorkerCore core = rig.core(IconWorkerCore.PRODUCTION);
                Call call = new Call(core, 0);
                check(rig.firstPostParked.await(5, TimeUnit.SECONDS), "the request is paused after posting its cleanup");
                for (int i = 0; i < IconWorkerCore.MAX_CLEANUP_RETRIES + 20; i++) {
                    // bounded: a cleanup that waits for the turn would hold the main thread, and this with it
                    rig.main.submit(rig.lastPosted()).get(5, TimeUnit.SECONDS);
                }
                rig.parkFirstPost.countDown();
                call.await();
                check(call.ok(), "the request was answered");
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the last request's cleanup was lost: the worker stays bound");
                Thread.sleep(250);
                same(1, rig.unbinds.get(), "exactly once");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-cleanup-of-an-older-request-does-nothing", () -> {
            Rig rig = new Rig();
            try {
                rig.autoIdle = false;
                IconWorkerCore core = rig.core(LIMITS);
                check(ask(core).ok(), "the first");
                Runnable stale = rig.lastPosted();
                rig.drawMillis = 400;
                Call second = new Call(core, 0);
                waitFor(() -> rig.running.get() == 1, 3000, "the second is drawing, with the turn");
                rig.main.submit(stale).get(5, TimeUnit.SECONDS); // dispatched while a newer request holds the turn
                Thread.sleep(150); // an unbind is made on a thread of its own: give a wrong one the time to show
                same(0, rig.unbinds.get(), "it unbinds nothing while the newer request holds the turn");
                second.await();
                check(second.ok(), "the second was answered by the worker it was using");
                rig.main.submit(stale).get(5, TimeUnit.SECONDS); // and now that the turn is free
                Thread.sleep(150);
                same(0, rig.unbinds.get(), "still nothing: its generation is not the current one");
                rig.main.submit(rig.lastPosted()).get(5, TimeUnit.SECONDS);
                waitFor(() -> rig.unbinds.get() == 1, 3000, "the cleanup of the last request does unbind");
            } finally {
                rig.shutdown();
            }
        });
        row("worker-a-cleanup-that-keeps-finding-the-turn-held-by-another-request-gives-up", () -> {
            Rig rig = new Rig();
            try {
                rig.autoIdle = false;
                IconWorkerCore core = rig.core(LIMITS);
                check(ask(core).ok(), "the first");
                Runnable cleanup = rig.lastPosted();
                rig.drawMillis = 3000;
                Call second = new Call(core, 0);
                waitFor(() -> rig.running.get() == 1, 3000, "the second is drawing, with the turn");
                int posts = rig.posted.size();
                for (int i = 0; i < IconWorkerCore.MAX_CLEANUP_RETRIES + 20; i++) {
                    // bounded: a cleanup that waits for the turn would hold the main thread, and this with it
                    rig.main.submit(cleanup).get(5, TimeUnit.SECONDS);
                }
                same(posts + IconWorkerCore.MAX_CLEANUP_RETRIES, rig.posted.size(), "a bounded number of looks again");
                same(0, rig.unbinds.get(), "and nothing is unbound");
                second.await();
            } finally {
                rig.shutdown();
            }
        });
        row("worker-production-limits-nest-in-the-tools", () -> {
            check(IconWorkerCore.PRODUCTION.deadlineMillis < 8_000, "the whole request ends before the tool's 8 s");
            check(IconWorkerCore.PRODUCTION.bindMillis + 1_000 <= IconWorkerCore.PRODUCTION.deadlineMillis,
                    "a bind leaves the drawing most of the deadline");
            same(5_000L, IconWorkerCore.PRODUCTION.idleMillis, "an idle worker is kept five seconds");
            same(1_000L, IconWorkerCore.PRODUCTION.bindMillis, "a bind may take a second");
            same(7_000L, IconWorkerCore.PRODUCTION.deadlineMillis, "a request has seven seconds");
        });
    }
}
