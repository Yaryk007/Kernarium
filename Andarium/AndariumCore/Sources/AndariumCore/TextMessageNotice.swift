// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// What the notice's writes ask of the file system, in the order the notice
/// makes them: replace the file, flush it, flush its directory. The tests
/// record and fail them; `DiskNoticeFiles` is the real one.
public protocol NoticeFiles: Sendable {
    /// The file's bytes; nil when there is none.
    func read(_ url: URL) throws -> Data?
    /// Replaces the file with `data`, whole or not at all.
    func replace(_ data: Data, at url: URL) throws
    /// Makes the file's contents survive a power loss.
    func flushFile(at url: URL) throws
    /// Makes the directory's entries survive a power loss.
    func flushDirectory(at url: URL) throws
}

/// The file system, as it is. A flush is `F_FULLFSYNC` where the volume has it,
/// else `fsync`; one that fails is an error, a directory's included.
public struct DiskNoticeFiles: NoticeFiles {
    private let fullSync: @Sendable (Int32) -> Int32
    private let sync: @Sendable (Int32) -> Int32

    public init(
        fullSync: @escaping @Sendable (Int32) -> Int32 = { fcntl($0, F_FULLFSYNC) },
        sync: @escaping @Sendable (Int32) -> Int32 = { fsync($0) }
    ) {
        self.fullSync = fullSync
        self.sync = sync
    }

    public func read(_ url: URL) throws -> Data? {
        do { return try Data(contentsOf: url) } catch let error as CocoaError where error.code == .fileReadNoSuchFile {
            return nil
        }
    }

    public func replace(_ data: Data, at url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: url, options: .atomic)
    }

    public func flushFile(at url: URL) throws { try flush(url) }

    public func flushDirectory(at url: URL) throws { try flush(url) }

    private func flush(_ url: URL) throws {
        let descriptor = open(url.path, O_RDONLY)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(descriptor) }
        if fullSync(descriptor) != 0, sync(descriptor) != 0 {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
    }
}

/// Files that live in memory: the notice of a preview, which has no guest and
/// outlives nothing, and the base of the tests' recording files.
public final class MemoryNoticeFiles: NoticeFiles, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [URL: Data] = [:]

    public init() {}

    public func read(_ url: URL) throws -> Data? { lock.withLock { stored[url] } }
    public func replace(_ data: Data, at url: URL) throws { lock.withLock { stored[url] = data } }
    public func flushFile(at url: URL) throws {}
    public func flushDirectory(at url: URL) throws {}
}

/// The notice that keeps "a message may have arrived" until the person has been
/// told, whatever ends the session first. A send of the current session adds
/// its identifier before it is dispatched, and the identifier leaves once its
/// result has been shown. Whatever a session leaves behind (the run ends, the
/// guest restarts, the application ends) becomes one flag, "cut off", and the
/// next time the bar opens it shows that and clears the flag.
///
/// The file is `text-message-pending` in the guest instance's directory
/// (`InstanceFiles.textMessageNotice`), ASCII:
///
///     kernarium-sms-pending 1
///     cut-off                      (optional: a leftover waits to be shown)
///     <32 hex digits>              (one line per entry, at most 17)
///
/// Leftovers are one flag, not a list: nothing resolves a leftover but
/// showing it. A file that is damaged or cannot be read counts as a leftover.
/// Every write comes from memory, on a serial actor, never from a
/// read-modify-write of the file; a write that fails leaves the earlier file whole.
public final class TextMessageNotice: @unchecked Sendable {
    public static let fileName = "text-message-pending"
    public static let header = "kernarium-sms-pending 1"
    public static let cutOffLine = "cut-off"
    /// At most 16 unresolved sends and the one result not yet shown.
    public static let capacity = 17

    public enum Failure: Error, Equatable {
        /// Seventeen entries are held already.
        case full
    }

    public static func url(in directory: URL) -> URL { directory.appendingPathComponent(fileName) }

    private let url: URL
    private let files: any NoticeFiles
    private let lock = NSLock()
    private var leftover = false
    private var entries: [String] = []
    private let writer = Writer()

    /// Reads the file at once; it has at most 593 bytes.
    public init(directory: URL, files: any NoticeFiles = DiskNoticeFiles()) {
        url = Self.url(in: directory)
        self.files = files
        do {
            if let data = try files.read(url) {
                if let parsed = Self.decode(data) {
                    leftover = parsed.cutOff || !parsed.entries.isEmpty
                } else {
                    leftover = true
                }
            }
        } catch {
            leftover = true
        }
    }

    /// A leftover waits to be shown: the next opening of the bar says "cut off".
    public var cutOffPending: Bool { lock.withLock { leftover } }

    /// The identifiers of the current session held now.
    public var currentEntries: [String] { lock.withLock { entries } }

    // MARK: Format

    static func encode(cutOff: Bool, entries: [String]) -> Data {
        var text = header + "\n"
        if cutOff { text += cutOffLine + "\n" }
        for entry in entries { text += entry + "\n" }
        return Data(text.utf8)
    }

    /// nil unless the bytes are exactly the format: the header, the flag at
    /// most once and before the entries, entries of 32 lower-case hex digits
    /// without repeats, at most 17, each line ended, and nothing else.
    static func decode(_ data: Data) -> (cutOff: Bool, entries: [String])? {
        guard data.count <= 24 + 8 + capacity * 33, data.allSatisfy({ $0 == 0x0A || (0x20...0x7E).contains($0) }),
              data.last == 0x0A else { return nil }
        let lines = String(decoding: data, as: UTF8.self).split(separator: "\n", omittingEmptySubsequences: false).dropLast()
        guard lines.first.map(String.init) == header else { return nil }
        var rest = Array(lines.dropFirst().map(String.init))
        var cutOff = false
        if rest.first == cutOffLine {
            cutOff = true
            rest.removeFirst()
        }
        guard rest.count <= capacity, Set(rest).count == rest.count,
              rest.allSatisfy({ $0.wholeMatch(of: /[0-9a-f]{32}/) != nil }) else { return nil }
        return (cutOff, rest)
    }

    // MARK: Changes

    /// Adds `identifier` and writes the file whole, flushed. The send waits for
    /// this before it dispatches anything. When it fails, or the task was
    /// cancelled, the identifier is not held, and nothing may be dispatched for it.
    public func add(_ identifier: String) async throws {
        try lock.withLock {
            // The session cancels its send tasks before `endSession`: an add that arrives after that is
            // a send of a session that is gone, and must not join the next one's memory.
            if Task.isCancelled { throw CancellationError() }
            if entries.contains(identifier) { return }
            guard entries.count < Self.capacity else { throw Failure.full }
            entries.append(identifier)
        }
        do {
            try await writer.write(self)
        } catch {
            lock.withLock { entries.removeAll { $0 == identifier } }
            throw error
        }
    }

    /// The result of `identifier` has been shown: it leaves. A write that
    /// fails changes nothing for this session; the file keeps the entry until
    /// the next write, which only makes the next opening say "cut off" once more.
    public func resolve(_ identifier: String) async {
        lock.withLock { entries.removeAll { $0 == identifier } }
        try? await writer.write(self)
    }

    /// The bar closed before the result of `identifier` was shown: the next
    /// opening says "cut off" instead. Memory only: the file already holds it.
    public func leaveForCutOff(_ identifier: String) {
        lock.withLock {
            if entries.contains(identifier) {
                entries.removeAll { $0 == identifier }
                leftover = true
            }
        }
    }

    /// The session ended (the run, a restart): what it holds becomes the flag.
    /// Memory only: the file holds the entries.
    public func endSession() {
        lock.withLock {
            if !entries.isEmpty { leftover = true }
            entries = []
        }
    }

    /// The data the notice was about is gone (an erase deleted the file): nothing is owed.
    public func reset() {
        lock.withLock {
            leftover = false
            entries = []
        }
    }

    /// The cut-off line has been shown: the flag goes.
    public func clearCutOff() async {
        lock.withLock { leftover = false }
        try? await writer.write(self)
    }

    private func snapshot() -> Data {
        lock.withLock { Self.encode(cutOff: leftover, entries: entries) }
    }

    /// The writes, one at a time, off the main actor. Each takes what memory
    /// holds when its turn comes, so an old write never overwrites a newer state.
    private actor Writer {
        func write(_ notice: TextMessageNotice) throws {
            let data = notice.snapshot()
            try notice.files.replace(data, at: notice.url)
            try notice.files.flushFile(at: notice.url)
            try notice.files.flushDirectory(at: notice.url.deletingLastPathComponent())
        }
    }
}
