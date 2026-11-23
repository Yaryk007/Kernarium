// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;

/**
 * The file operations of the delivery record (RecordStore, DeliveryRecord): the one place where the
 * record touches the file system, so that a test can see the order of the calls the shipped code makes and
 * can make any one of them fail. The files are named by their name in the record's directory.
 */
interface RecordFiles {
    /** An open file. */
    interface File {
        long size() throws IOException;

        /** Reads up to {@code length} bytes; -1 at the end of the file. */
        int read(byte[] buffer, int offset, int length) throws IOException;

        /** Writes at most {@code length} bytes and returns how many it wrote. */
        int write(byte[] buffer, int offset, int length) throws IOException;

        /** Forces the file's data and its metadata to the device. */
        void force() throws IOException;

        void close() throws IOException;
    }

    /** The exclusive lock on the lock file, held until {@link #release()}. */
    interface Lock {
        void release() throws IOException;
    }

    /** Takes the exclusive lock on the lock file, creating it, and waits for it. */
    Lock lock() throws IOException;

    /** Opens an existing file for reading; throws {@link NoSuchFileException} when there is none. */
    File openRead(String name) throws IOException;

    /** Creates the file, or empties it, and opens it for writing. */
    File openWrite(String name) throws IOException;

    /** Renames {@code from} to {@code to}, replacing it, in one step. */
    void move(String from, String to) throws IOException;

    /** Forces the directory, so that the renames made in it survive the device's power loss. */
    void forceDirectory() throws IOException;

    /** The operations on the files of one directory, with java.nio. */
    final class Nio implements RecordFiles {
        static final String LOCK = "sms-delivery-record.lock";

        private final Path directory;

        Nio(Path directory) {
            this.directory = directory;
        }

        @Override
        public Lock lock() throws IOException {
            final FileChannel channel = FileChannel.open(directory.resolve(LOCK), StandardOpenOption.CREATE,
                    StandardOpenOption.WRITE);
            final FileLock lock;
            try {
                lock = channel.lock();
            } catch (IOException | RuntimeException e) {
                channel.close();
                throw e;
            }
            return () -> {
                try {
                    lock.release();
                } finally {
                    channel.close();
                }
            };
        }

        @Override
        public File openRead(String name) throws IOException {
            return new Channel(FileChannel.open(directory.resolve(name), StandardOpenOption.READ));
        }

        @Override
        public File openWrite(String name) throws IOException {
            return new Channel(FileChannel.open(directory.resolve(name), StandardOpenOption.CREATE,
                    StandardOpenOption.WRITE, StandardOpenOption.TRUNCATE_EXISTING));
        }

        @Override
        public void move(String from, String to) throws IOException {
            Files.move(directory.resolve(from), directory.resolve(to), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
        }

        @Override
        public void forceDirectory() throws IOException {
            try (FileChannel channel = FileChannel.open(directory, StandardOpenOption.READ)) {
                channel.force(true);
            }
        }

        private static final class Channel implements File {
            private final FileChannel channel;

            Channel(FileChannel channel) {
                this.channel = channel;
            }

            @Override
            public long size() throws IOException {
                return channel.size();
            }

            @Override
            public int read(byte[] buffer, int offset, int length) throws IOException {
                return channel.read(ByteBuffer.wrap(buffer, offset, length));
            }

            @Override
            public int write(byte[] buffer, int offset, int length) throws IOException {
                return channel.write(ByteBuffer.wrap(buffer, offset, length));
            }

            @Override
            public void force() throws IOException {
                channel.force(true);
            }

            @Override
            public void close() throws IOException {
                channel.close();
            }
        }
    }
}
