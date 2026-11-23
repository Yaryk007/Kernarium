// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Reads a file in pieces with POSIX `read` into one buffer it keeps, and
/// hands each piece out as a `Data` of its own. A transfer reads a file of
/// any length on the thread of an adb operation. That thread turns no run
/// loop, so its autorelease pool is emptied only when the thread ends, and a
/// `FileHandle` read, which returns an autoreleased object, would keep every
/// piece of a 3 GiB file alive until then. A `Data` made here is released when
/// its last reference goes.
///
/// The size is that of the open file (`fstat`). A coordinated read that has
/// brought an iCloud file down opens the file it has now, so the size is
/// never one cached from before.
public final class FileChunkReader {
    /// The file's length; zero for anything but a regular file.
    public let size: UInt64
    public let isDirectory: Bool
    private let descriptor: Int32
    private let buffer: UnsafeMutableRawPointer
    private let capacity: Int

    /// Opens `url` for reading. `capacity` is the largest piece `read` gives.
    public convenience init(contentsOf url: URL, capacity: Int) throws {
        let opened = url.withUnsafeFileSystemRepresentation { path -> Int32 in
            guard let path else {
                errno = EINVAL
                return -1
            }
            return Darwin.open(path, O_RDONLY | O_CLOEXEC)
        }
        guard opened >= 0 else { throw Self.failure(errno) }
        try self.init(adopting: opened, capacity: capacity)
    }

    /// Opens the regular file at `path`, which is relative to the directory
    /// `base`, for reading, without following a link in any component of
    /// `path` and without waiting on a pipe. What is read is the file that
    /// was opened and checked here, whatever the path names afterwards.
    public convenience init(regularFileAt path: String, under base: URL, capacity: Int) throws {
        try self.init(adopting: Self.openRegularFile(path, under: base), capacity: capacity)
    }

    /// Takes over `opened`, which it closes when it cannot be read from.
    private init(adopting opened: Int32, capacity: Int) throws {
        precondition(capacity > 0)
        var status = stat()
        guard fstat(opened, &status) == 0 else {
            let code = errno
            Darwin.close(opened)
            throw Self.failure(code)
        }
        descriptor = opened
        isDirectory = status.st_mode & S_IFMT == S_IFDIR
        size = status.st_mode & S_IFMT == S_IFREG ? UInt64(max(status.st_size, 0)) : 0
        self.capacity = capacity
        buffer = UnsafeMutableRawPointer.allocate(byteCount: capacity, alignment: 16)
    }

    /// A descriptor for the regular file `path` names under `base`: each
    /// directory of the path is opened from the one before it and the file
    /// from the last, none by following a link (`base` itself is the
    /// caller's own folder). What is checked is the bytes that reach the
    /// filesystem, not Swift's view of the string: a NUL ends a C string, so
    /// a component such as two dots, a NUL and a suffix would name the parent
    /// directory to `openat`. A path with a NUL, an empty component or a
    /// component of one or two dots is refused.
    static func openRegularFile(_ path: String, under base: URL) throws -> Int32 {
        let bytes = Array(path.utf8)
        let dot = UInt8(ascii: ".")
        let names = bytes.split(separator: UInt8(ascii: "/"), omittingEmptySubsequences: false)
        guard !bytes.isEmpty, !bytes.contains(0), let last = names.last,
            !names.contains(where: { $0.isEmpty || $0.elementsEqual([dot]) || $0.elementsEqual([dot, dot]) })
        else { throw POSIXError(.EINVAL) }
        // A piece of valid text split at an ASCII slash is valid text, and
        // with no NUL it reaches `openat` as exactly these bytes.
        let leaf = String(decoding: last, as: UTF8.self)
        var directory = base.withUnsafeFileSystemRepresentation { path -> Int32 in
            guard let path else {
                errno = EINVAL
                return -1
            }
            return Darwin.open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        }
        guard directory >= 0 else { throw failure(errno) }
        for name in names.dropLast() {
            let next = openat(directory, String(decoding: name, as: UTF8.self), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            let code = errno
            Darwin.close(directory)
            guard next >= 0 else { throw failure(code) }
            directory = next
        }
        defer { Darwin.close(directory) }
        // O_NONBLOCK so that opening a pipe does not wait for its writer.
        let opened = openat(directory, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)
        guard opened >= 0 else { throw failure(errno) }
        var status = stat()
        guard fstat(opened, &status) == 0, status.st_mode & S_IFMT == S_IFREG else {
            Darwin.close(opened)
            throw failure(EFTYPE)
        }
        _ = fcntl(opened, F_SETFL, fcntl(opened, F_GETFL) & ~O_NONBLOCK)
        return opened
    }

    deinit {
        Darwin.close(descriptor)
        buffer.deallocate()
    }

    /// The next piece of at most `limit` bytes (and never more than the
    /// capacity), or nil at the end of the file.
    public func read(upTo limit: Int) throws -> Data? {
        let count = min(limit, capacity)
        guard count > 0 else { return nil }
        while true {
            let got = Darwin.read(descriptor, buffer, count)
            if got > 0 { return Data(bytes: buffer, count: got) }
            if got == 0 { return nil }
            if errno != EINTR { throw Self.failure(errno) }
        }
    }

    private static func failure(_ code: Int32) -> Error {
        POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
    }
}
