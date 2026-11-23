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

package com.kernarium.oldapp;

import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.OpenOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFileAttributeView;
import java.nio.file.attribute.PosixFileAttributes;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/**
 * The exclusive lock that serialises the commands' changes across processes: a lock on a file that
 * only its owner can open.
 *
 * <p>The file lives in the shell user's data directory, whose entries an ordinary application
 * cannot create but whose existing files it can open when their mode lets it, and an application
 * that holds the lock keeps every {@code set} waiting. So the file is created readable and
 * writable by its owner alone (a reader could take a shared lock too), and an existing file is
 * used only once it is a regular file with no group or other permission: one with such
 * permissions is tightened by its owner, and anything that cannot be made so (a directory, a
 * device, a symbolic link, a file of another user) is an error, which the commands treat as a
 * lock that cannot be had. The file is never deleted and nothing is written to it.
 */
final class LockFile {
    static final long POLL_MILLIS = 50;
    private static final Set<PosixFilePermission> OWNER_ONLY = PosixFilePermissions.fromString("rw-------");
    private static final Set<OpenOption> OPEN = new HashSet<OpenOption>(Arrays.<OpenOption>asList(
            StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE,
            LinkOption.NOFOLLOW_LINKS));

    private LockFile() {}

    /**
     * Takes the lock on the file, polling until the time is up. Returns null when it could not be
     * had in time and throws when the file cannot be used. Reading and writing are both asked for
     * so that opening a pipe someone put there cannot block.
     */
    static OldAppCommand.Lock acquire(Path path, long timeoutMillis) throws Exception {
        final FileChannel channel = open(path);
        try {
            long deadline = System.nanoTime() + timeoutMillis * 1_000_000L;
            while (true) {
                final FileLock held = tryLock(channel);
                if (held != null) {
                    return new OldAppCommand.Lock() {
                        @Override
                        public void release() throws Exception {
                            try {
                                held.release();
                            } finally {
                                channel.close();
                            }
                        }
                    };
                }
                if (System.nanoTime() - deadline >= 0) {
                    channel.close();
                    return null;
                }
                Thread.sleep(POLL_MILLIS);
            }
        } catch (Exception | Error problem) {
            channel.close();
            throw problem;
        }
    }

    /** A lock that this process already holds answers like one that another process holds. */
    private static FileLock tryLock(FileChannel channel) throws IOException {
        try {
            return channel.tryLock();
        } catch (OverlappingFileLockException held) {
            return null;
        }
    }

    /** Opens the file, creating it owner-only, and makes sure that what was opened is safe to lock. */
    private static FileChannel open(Path path) throws IOException {
        FileChannel channel = FileChannel.open(path, OPEN, PosixFilePermissions.asFileAttribute(OWNER_ONLY));
        try {
            PosixFileAttributeView view = Files.getFileAttributeView(path, PosixFileAttributeView.class,
                    LinkOption.NOFOLLOW_LINKS);
            if (!view.readAttributes().isRegularFile()) {
                throw new IOException(path + " is not a regular file");
            }
            if (sharedWithOthers(view.readAttributes())) {
                // Only the owner may change the mode: a file of another user fails here.
                view.setPermissions(OWNER_ONLY);
            }
            return channel;
        } catch (IOException | RuntimeException | Error problem) {
            channel.close();
            throw problem;
        }
    }

    private static boolean sharedWithOthers(PosixFileAttributes attributes) {
        for (PosixFilePermission permission : attributes.permissions()) {
            if (!permission.name().startsWith("OWNER_")) {
                return true;
            }
        }
        return false;
    }
}
