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
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * Runs one child process (the argument vector is a process's, never a shell's command line) and returns its
 * status and the first bytes of its output. The output is read while the process runs, so that a chatty
 * child cannot fill the pipe and stall; the whole run has one deadline; and the child is terminated and
 * reaped, in a {@code finally}, however the run ends (a deadline, an interrupt, a failure after the spawn).
 * A child that is still alive after the kill is a failure of its own: the run says so and does not claim
 * to have ended it.
 */
final class ChildProcess {
    /** How long the output is read on after the process has ended (a descendant may hold the pipe open). */
    static final long DRAIN_MILLIS = 2_000;
    /** How long a terminated child is given to end, once with SIGTERM and once with SIGKILL. */
    static final long GRACE_MILLIS = 2_000;

    /**
     * The kill for a child that SIGTERM did not end. It is the platform's: on the target libcore
     * {@code Process.destroyForcibly()} calls {@code destroy()} (SIGTERM) again and the class has no
     * {@code pid()}, so the platform sends SIGKILL by the child's pid. Whether the child is gone is judged by
     * waiting for it, not by what this call says. It is asked only of a child that is still alive when it is asked:
     * a number that was a reaped child's may be another process's by then.
     */
    interface Kill {
        void kill(Process process) throws Exception;
    }

    /** How the child ended and its first output (standard output and error together). */
    static final class Result {
        final int exit;
        final String output;

        Result(int exit, String output) {
            this.exit = exit;
            this.output = output;
        }
    }

    private ChildProcess() {}

    /**
     * Runs {@code argv}; keeps at most {@code keep} bytes of output; fails when it is not over after
     * {@code limitMillis}, or when a child that had to be ended is still alive after {@code kill}.
     */
    static Result run(List<String> argv, long limitMillis, int keep, Kill kill) throws Exception {
        return run(argv, limitMillis, keep, kill, GRACE_MILLIS);
    }

    /** As {@link #run(List, long, int, Kill)}, with the grace given to a terminated child (a test shortens it). */
    static Result run(List<String> argv, long limitMillis, int keep, Kill kill, long graceMillis) throws Exception {
        Process process = null;
        boolean ended = true;
        try {
            process = new ProcessBuilder(argv).redirectErrorStream(true).start();
            process.getOutputStream().close();
            final InputStream output = process.getInputStream();
            final ByteArrayOutputStream kept = new ByteArrayOutputStream();
            Thread reader = new Thread(() -> drain(output, kept, keep), "kernarium_apps-output");
            reader.setDaemon(true);
            reader.start();
            if (!process.waitFor(limitMillis, TimeUnit.MILLISECONDS)) {
                throw new PlatformException("cmd activity start-activity did not finish");
            }
            reader.join(DRAIN_MILLIS);
            String text;
            synchronized (kept) {
                text = new String(kept.toByteArray(), StandardCharsets.UTF_8);
            }
            return new Result(process.exitValue(), text);
        } finally {
            if (process != null) {
                ended = terminate(process, kill, graceMillis);
            }
            if (!ended) {
                throw new PlatformException("cmd activity start-activity could not be ended");
            }
        }
    }

    private static void drain(InputStream in, ByteArrayOutputStream kept, int keep) {
        byte[] buffer = new byte[4096];
        try {
            int n;
            while ((n = in.read(buffer)) != -1) {
                synchronized (kept) {
                    int room = keep - kept.size();
                    if (room > 0) {
                        kept.write(buffer, 0, Math.min(n, room));
                    }
                }
            }
        } catch (IOException ended) {
            // The pipe was closed under the reader: what was read is what there is.
        }
    }

    /**
     * Ends the child, if it is running: SIGTERM and a wait, then the kill and a wait (a bounded time each). The kill
     * is not asked of a child that ended during the wait, here or after an interrupt: its pid may be another's.
     * Returns whether the child is gone. The streams are closed.
     */
    static boolean terminate(Process process, Kill kill, long graceMillis) {
        try {
            if (process.isAlive()) {
                process.destroy();
                if (!process.waitFor(graceMillis, TimeUnit.MILLISECONDS) && process.isAlive()) {
                    kill(process, kill);
                    process.waitFor(graceMillis, TimeUnit.MILLISECONDS);
                }
            }
            return !process.isAlive();
        } catch (InterruptedException interrupted) {
            if (process.isAlive()) {
                kill(process, kill);
            }
            Thread.currentThread().interrupt();
            return !process.isAlive();
        } finally {
            try {
                process.getInputStream().close();
            } catch (IOException ignored) {
                // Nothing more to do with it.
            }
        }
    }

    /** One attempt to kill; its failure is not the answer, the wait that follows is. */
    private static void kill(Process process, Kill kill) {
        try {
            kill.kill(process);
        } catch (Exception | LinkageError failed) {
            // The child is waited for next: whether it is gone says whether the kill worked.
        }
    }
}
