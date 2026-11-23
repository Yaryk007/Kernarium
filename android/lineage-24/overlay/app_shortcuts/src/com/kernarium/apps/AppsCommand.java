// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.kernarium.apps;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;

/**
 * The commands of {@code kernarium_apps}, which lists the launcher activities of the foreground
 * user with their labels and icons and starts one of them, for Andarium's Add to Home Screen. {@link
 * Main} runs them on Android, as the adb shell user (see the wrapper script of the same name); the
 * contract (the answers' framing, grammar and bounds) is the one the README of the image names.
 *
 * <pre>
 * kernarium_apps digest              the header block
 * kernarium_apps list                the header block, then one row per launcher activity
 * kernarium_apps icons C [C ...]     one PNG record per component, 1 to 8 components
 * kernarium_apps launch C            starts the component, exactly as a launcher's tap does
 * </pre>
 *
 * The answer goes to the output only when the command succeeds, whole, so that a status other than
 * 0 leaves nothing a reader might take for an answer. Nothing is written anywhere else: no file, no
 * property, no setting.
 *
 * <p>Exit status: 0 done, 1 any other failure, 2 refused (a component that is not a launcher
 * activity of the foreground user now), 64 usage. The format grows by trailing fields and header
 * lines; {@link #FORMAT_VERSION} changes when the meaning of a field does.
 */
final class AppsCommand {
    static final int FORMAT_VERSION = 1;
    static final String NAME = "kernarium_apps";
    static final int EXIT_OK = 0;
    static final int EXIT_FAILURE = 1;
    static final int EXIT_REFUSED = 2;
    static final int EXIT_USAGE = 64;

    /** The most components one {@code icons} takes. */
    static final int MAX_ICONS = 8;
    /** After this long {@code icons} starts no further icon: the rest are answered {@code deferred}. */
    static final long ICON_DEADLINE_MILLIS = 20_000;
    /**
     * How long one icon's render (all its sizes) may take, from the request to the renderer to its answer. The
     * renderer's provider answers within its own bound (a draw that takes too long is its reply of an error for that
     * icon), so a call that is unanswered after this means there is no renderer to talk to: the command fails with
     * the fixed reason and prints nothing, as it does for a renderer that is not there.
     */
    static final long ICON_RENDER_MILLIS = 8_000;
    /** How long {@code cmd activity start-activity} may take. */
    static final int START_SECONDS = 20;
    static final String START_FLAGS = "0x10200000";
    static final byte[] PNG_SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    /** The platform the commands stand on; the Android one is {@link AndroidPlatform}. */
    interface Platform {
        /** The foreground user's id; throws when the platform cannot say (there is no substitute for it). */
        int currentUser() throws Exception;

        /**
         * The activities of the user's applications that handle MAIN with the LAUNCHER category, with the
         * facts {@link Entry} carries. The commands judge them.
         */
        List<Entry> launcherActivities(int user) throws Exception;

        /** The system's locale list as BCP-47 tags; throws when the platform cannot say. */
        List<String> localeTags() throws Exception;

        /** The activity's label in the system's current language, or null when it has none. */
        String label(Entry entry) throws Exception;

        /**
         * The icon renderer, held for one {@code icons} command and for {@code user}, the user the applications were
         * listed for (the foreground user may be another by the time it is asked); throws a {@link PlatformException}
         * when there is none (the image lacks it, another package answers for it, or it cannot be reached). Icons are
         * drawn there and nowhere in this process.
         */
        Renderer openRenderer(int user) throws Exception;

        /** Runs {@link #startArguments} and returns how it ended and what it printed (at most a few KiB). */
        StartResult start(int user, Component component) throws Exception;

        /** A monotonic clock in milliseconds: the icons' deadline must not follow a correction of the wall clock. */
        long nowMillis();

        /** How long one icon's render may take, all its sizes together. */
        default long renderLimitMillis() {
            return ICON_RENDER_MILLIS;
        }
    }

    /** The renderer of one {@code icons} command. */
    interface Renderer extends AutoCloseable {
        /**
         * The activity's icon at the largest size whose PNG is within the bound, as the renderer answers it (the tool
         * checks the answer: {@link PngCheck}). Throws when it cannot be had, and a {@link RendererGone} when the
         * renderer cannot be reached any more, which fails the command.
         */
        Icon render(Entry entry) throws Exception;

        @Override
        void close();
    }

    /** The renderer is gone and could not be reached again: nothing more can be drawn. */
    static final class RendererGone extends Exception {
        private static final long serialVersionUID = 1L;

        RendererGone(String reason, Throwable cause) {
            super(reason, cause);
        }
    }

    /** How the start's process ended. */
    static final class StartResult {
        final int exit;
        final String output;

        StartResult(int exit, String output) {
            this.exit = exit;
            this.output = output;
        }
    }

    /** Makes the platform once the arguments are known to be a command. */
    interface PlatformFactory {
        Platform create() throws Exception;
    }

    private AppsCommand() {}

    /** The argument vector of the start: separate strings for a process, never a shell's command line. */
    static List<String> startArguments(int user, Component component) {
        return Arrays.asList("/system/bin/cmd", "activity", "start-activity", "--user", Integer.toString(user),
                "-a", "android.intent.action.MAIN", "-c", "android.intent.category.LAUNCHER", "-f", START_FLAGS,
                "-n", component.token());
    }

    /** Whether the arguments are a command: the verb and the count, nothing about the components. */
    static boolean isUsage(String[] args) {
        if (args.length == 0) {
            return false;
        }
        switch (args[0]) {
            case "digest":
            case "list":
                return args.length == 1;
            case "icons":
                return args.length >= 2 && args.length <= 1 + MAX_ICONS;
            case "launch":
                return args.length == 2;
            default:
                return false;
        }
    }

    static int run(String[] args, Platform platform, OutputStream out, PrintWriter err) {
        return run(args, () -> platform, out, err);
    }

    /** The arguments are judged first: a usage error is 64 even when the platform cannot be made. */
    static int run(String[] args, PlatformFactory factory, OutputStream out, PrintWriter err) {
        if (!isUsage(args)) {
            err.print("usage: " + NAME + " digest\n       " + NAME + " list\n       " + NAME
                    + " icons PACKAGE/ACTIVITY ...\n       " + NAME + " launch PACKAGE/ACTIVITY\n");
            return EXIT_USAGE;
        }
        // The components are judged before the platform is touched: nothing that does not match the
        // grammar reaches the package manager or a process.
        List<Component> components = new ArrayList<>();
        if (args[0].equals("icons") || args[0].equals("launch")) {
            for (int i = 1; i < args.length; i++) {
                Component component = Component.parse(args[i]);
                if (component == null) {
                    return refuse(err, "not a component name");
                }
                components.add(component);
            }
            if (new HashSet<>(components).size() != components.size()) {
                err.print(NAME + ": the same component twice\n");
                return EXIT_USAGE;
            }
        }
        Platform platform;
        try {
            platform = factory.create();
        } catch (Throwable problem) {
            err.print(NAME + ": " + reason(problem) + "\n");
            return EXIT_FAILURE;
        }
        Answer answer = new Answer();
        int status;
        try {
            switch (args[0]) {
                case "digest":
                    status = digest(platform, answer);
                    break;
                case "list":
                    status = list(platform, answer);
                    break;
                case "icons":
                    status = icons(platform, components, answer);
                    break;
                default:
                    status = launch(platform, components.get(0), answer, err);
                    break;
            }
        } catch (Throwable problem) {
            err.print(NAME + ": " + args[0] + " failed: " + reason(problem) + "\n");
            return EXIT_FAILURE;
        }
        if (status != EXIT_OK) {
            return status;
        }
        try {
            answer.writeTo(out);
        } catch (IOException problem) {
            err.print(NAME + ": the answer could not be written\n");
            return EXIT_FAILURE;
        }
        return EXIT_OK;
    }

    /**
     * Why a failure happened, in words of this tool: the fixed reason of a {@link PlatformException}, else the
     * class of the exception and never its message (a framework's can carry a user id, a permission or a path).
     */
    static String reason(Throwable problem) {
        if (problem instanceof PlatformException) {
            return StartOutcome.ascii(String.valueOf(problem.getMessage()), 160);
        }
        StringBuilder kind = new StringBuilder();
        String name = problem.getClass().getName();
        for (int i = 0; i < name.length() && kind.length() < 100; i++) {
            char c = name.charAt(i);
            boolean part = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
                    || c == '$' || c == '.';
            kind.append(part ? c : '?');
        }
        return "unexpected " + kind;
    }

    private static int refuse(PrintWriter err, String reason) {
        err.print(NAME + ": refused: " + reason + "\n");
        return EXIT_REFUSED;
    }

    private static Listing listing(Platform platform) throws Exception {
        int user = platform.currentUser();
        return Listing.of(user, platform.localeTags(), platform.launcherActivities(user));
    }

    private static void header(Answer answer, Listing listing) {
        answer.line("kernarium_apps " + FORMAT_VERSION);
        answer.line("digest\t" + listing.digest);
        answer.line("count\t" + listing.rows.size());
        answer.line("skipped\t" + listing.skipped);
        answer.line("locales\t" + listing.locales);
        answer.line("user\t" + listing.user);
    }

    private static int digest(Platform platform, Answer answer) throws Exception {
        header(answer, listing(platform));
        return EXIT_OK;
    }

    private static int list(Platform platform, Answer answer) throws Exception {
        Listing listing = listing(platform);
        header(answer, listing);
        answer.line("rows");
        for (Entry entry : listing.rows) {
            answer.line(entry.packageName + "\t" + entry.activity + "\t" + label(platform, entry) + "\t"
                    + (entry.builtin ? "1" : "0") + "\t" + Listing.iconKey(entry));
        }
        answer.line("end\t" + listing.rows.size());
        return EXIT_OK;
    }

    /**
     * The safe label of one application. A label that cannot be read, or that is too big to read or
     * clean (an application's own text can be megabytes), is that one application's failure: it is listed
     * under its package name, and the rest of the listing is unaffected.
     */
    private static String label(Platform platform, Entry entry) {
        try {
            return LabelText.sanitize(platform.label(entry), entry.packageName);
        } catch (Exception | OutOfMemoryError problem) {
            return LabelText.sanitize(null, entry.packageName);
        }
    }

    private static Entry find(List<Entry> reported, Component component) {
        for (Entry entry : reported) {
            if (entry.packageName.equals(component.packageName) && entry.activity.equals(component.activity)
                    && entry.launchable()) {
                return entry;
            }
        }
        return null;
    }

    private static int icons(Platform platform, List<Component> components, Answer answer) throws Exception {
        long begin = platform.nowMillis();
        int user = platform.currentUser();
        List<Entry> reported = platform.launcherActivities(user);
        try (Renderer renderer = platform.openRenderer(user)) {
            answer.line("kernarium_apps " + FORMAT_VERSION);
            answer.line("icons\t" + components.size());
            for (Component component : components) {
                if (platform.nowMillis() - begin > ICON_DEADLINE_MILLIS) {
                    answer.nonImage(component, "deferred");
                    continue;
                }
                Entry entry = find(reported, component);
                if (entry == null) {
                    answer.nonImage(component, "missing");
                } else if (entry.iconResource == 0) {
                    answer.nonImage(component, "noicon");
                } else {
                    Render render = renderWithin(platform, renderer, entry);
                    if (render.gone || render.unanswered) {
                        throw new PlatformException(ProviderLink.UNAVAILABLE);
                    }
                    if (render.icon == null) {
                        answer.nonImage(component, "error");
                    } else {
                        answer.image(component, render.icon);
                    }
                }
            }
            answer.line("end\t" + components.size());
            return EXIT_OK;
        }
    }

    /**
     * What one icon's render came to: the icon (checked), or none (an error), whether the renderer had not answered
     * when the time was up, and whether the renderer is gone.
     */
    private static final class Render {
        Icon icon;
        boolean unanswered;
        boolean gone;
    }

    /**
     * The render of one icon in a worker thread that the command waits for no longer than the limit: a render
     * that does not end in time is abandoned (the thread is a daemon and the process ends with the answer), and
     * nothing of it is used. What the renderer answers is checked before it is used: an answer that does not pass is
     * an error for that icon alone.
     */
    private static Render renderWithin(Platform platform, Renderer renderer, Entry entry) {
        final Icon[] made = new Icon[1];
        final boolean[] gone = new boolean[1];
        Thread worker = new Thread(() -> {
            try {
                Icon icon = renderer.render(entry);
                made[0] = PngCheck.problem(icon) == null ? icon : null;
            } catch (RendererGone lost) {
                gone[0] = true;
            } catch (Throwable problem) {
                made[0] = null;
            }
        }, NAME + "-icon");
        worker.setDaemon(true);
        worker.start();
        try {
            worker.join(platform.renderLimitMillis());
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
        Render render = new Render();
        if (worker.isAlive()) {
            worker.interrupt();
            render.unanswered = true; // whatever it makes later is not read
        } else {
            render.icon = made[0];
            render.gone = gone[0];
        }
        return render;
    }

    private static int launch(Platform platform, Component component, Answer answer, PrintWriter err)
            throws Exception {
        int user = platform.currentUser();
        if (find(platform.launcherActivities(user), component) == null) {
            return refuse(err, "not a launcher activity of the foreground user");
        }
        AppsCommand.StartResult result = platform.start(user, component);
        String failure = StartOutcome.failure(result.exit, result.output);
        if (failure != null) {
            err.print(NAME + ": " + failure + "\n");
            return EXIT_FAILURE;
        }
        answer.line("launched\t" + component.token());
        return EXIT_OK;
    }

    /** The answer as bytes, written whole once the command has succeeded. */
    static final class Answer {
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();

        void line(String text) {
            byte[] encoded = text.getBytes(StandardCharsets.UTF_8);
            bytes.write(encoded, 0, encoded.length);
            bytes.write('\n');
        }

        void nonImage(Component component, String status) {
            line("icon\t" + component.token() + "\t" + status + "\t0\t0\t-\t0\t0\t-");
        }

        void image(Component component, Icon icon) {
            line("icon\t" + component.token() + "\tok\t" + icon.size + "\t" + icon.size + "\t"
                    + (icon.adaptive ? "adaptive" : "legacy") + "\t" + (icon.hasAlpha ? 1 : 0) + "\t"
                    + icon.png.length + "\t" + Listing.sha256Hex(icon.png));
            bytes.write(icon.png, 0, icon.png.length);
            bytes.write('\n');
        }

        byte[] toByteArray() {
            return bytes.toByteArray();
        }

        void writeTo(OutputStream out) throws IOException {
            out.write(bytes.toByteArray());
            out.flush();
        }
    }
}
