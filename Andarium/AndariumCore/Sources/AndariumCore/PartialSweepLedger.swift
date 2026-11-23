// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Whether a guest's internal storage may hold partial files of imports that
/// earlier runs cut short (`GuestFiles.sweepCommand`), kept on the host beside
/// the guest's other files (`partial-sweep.json`). A run walks the storage to
/// remove them only when this says it may: the walk costs the guest seconds of
/// its CPU for a large storage, which an ordinary start must not pay.
///
/// A guest owes the walk
///  - by default: a guest with no record, which is every guest that an earlier
///    version of the application made, may hold partial files of any name;
///  - from before an import's first byte until that import has ended and its
///    own partial file is known to be gone, so that an application that iOS
///    ends in the middle of an import, or a guest that stops under it, leaves
///    the record behind;
///  - after anything that puts an older data disk in place (`markOwed`).
///
/// It does not owe it after a sweep that finished, and a new data disk
/// (`markSettled`) holds nothing.
///
/// A record is written with the atomic replacement of its file and then
/// flushed, before the call that changes it returns, so a record that has been
/// written survives the application being ended the next moment.
public final class PartialSweepLedger: @unchecked Sendable {
    public static let fileName = "partial-sweep.json"

    /// What the file says. `owed` is a cause that predates this process (the
    /// default, a restore, an earlier process's imports); `imports` says that
    /// an import of this process has not ended cleanly yet.
    struct Record: Codable, Equatable {
        var owed: Bool
        var imports: Bool

        /// A guest with no readable record.
        static let unknown = Record(owed: true, imports: false)
    }

    public let url: URL
    private let lock = NSLock()
    /// The imports of this process that have begun and not ended, by the mark
    /// of the run whose name their partial file carries.
    private var live: [GuestRunMark: Int] = [:]
    /// The marks of imports that ended with their partial file possibly in
    /// place: removing it failed, or could not be tried.
    private var cut: Set<GuestRunMark> = []

    /// Every read and write of a record, of any ledger: a restore and an
    /// import end can meet.
    private static let files = NSLock()

    /// A record that an earlier process left with an import in it was ended by
    /// that process's end: it is owed from here on.
    public init(directory: URL) {
        url = Self.url(in: directory)
        try? Self.update(url) {
            guard $0.imports else { return }
            $0.owed = true
            $0.imports = false
        }
    }

    public static func url(in directory: URL) -> URL {
        directory.appendingPathComponent(fileName)
    }

    // MARK: What a start asks

    /// Whether the next walk is owed.
    public var isOwed: Bool {
        let record = Self.read(url)
        return record.owed || record.imports
    }

    /// Whether the guest whose files are in `directory` owes the walk.
    public static func isOwed(in directory: URL) -> Bool {
        let record = read(url(in: directory))
        return record.owed || record.imports
    }

    // MARK: What puts the disk in a known state

    /// An older data disk is about to be put in place: it may hold partial
    /// files of any run. Call it before the step that cannot be taken back.
    public static func markOwed(in directory: URL) throws {
        try update(url(in: directory)) { $0.owed = true }
    }

    /// A new data disk: it holds nothing, whatever an earlier one owed.
    public static func markSettled(in directory: URL) throws {
        try update(url(in: directory)) { $0 = Record(owed: false, imports: false) }
    }

    // MARK: Imports

    /// An import of the run `mark` is about to write its partial file. Throws
    /// when the record cannot be written, and then the import must not begin.
    public func importBegan(mark: GuestRunMark) throws {
        try lock.withLock {
            try Self.update(url) { $0.imports = true }
            live[mark, default: 0] += 1
        }
    }

    /// The data disk was erased: no import of this process has a partial file
    /// on the new one, and what it kept of them is of no use.
    public func diskErased() {
        lock.withLock {
            live.removeAll()
            cut.removeAll()
        }
    }

    /// The import ended. `clean` is true when its file was moved into place,
    /// or when its partial file was removed and the removal succeeded.
    public func importEnded(mark: GuestRunMark, clean: Bool) {
        lock.withLock {
            guard let count = live[mark] else { return }
            live[mark] = count > 1 ? count - 1 : nil
            if !clean { cut.insert(mark) }
            // A record that cannot be written stays as it was, which is owed.
            try? Self.update(url) { $0.imports = !(live.isEmpty && cut.isEmpty) }
        }
    }

    // MARK: Sweeps

    /// What a sweep covers: the imports that were cut off before it began and
    /// carry another mark than the one it spares.
    public struct Sweep: Sendable {
        let covered: Set<GuestRunMark>
    }

    /// A sweep that spares the files of the run `mark` is beginning.
    public func sweepBegan(excluding mark: GuestRunMark) -> Sweep {
        lock.withLock { Sweep(covered: cut.subtracting([mark])) }
    }

    /// The sweep's walk finished: what it covers is gone, and so is whatever
    /// the record owed from before. An import that is still running, and one
    /// that ended after the sweep began or carries the mark it spared, keeps
    /// the record owed.
    public func sweepFinished(_ sweep: Sweep) {
        lock.withLock {
            cut.subtract(sweep.covered)
            try? Self.update(url) {
                $0.owed = false
                $0.imports = !(live.isEmpty && cut.isEmpty)
            }
        }
    }

    /// Runs `walk`, which removes the partial files that do not carry `mark`
    /// and returns how many, and settles the record when it returns. A walk
    /// that throws leaves the record as it was, so the next run walks.
    public func sweeping(excluding mark: GuestRunMark, _ walk: () async throws -> Int) async throws -> Int {
        let sweep = sweepBegan(excluding: mark)
        let removed = try await walk()
        sweepFinished(sweep)
        return removed
    }

    // MARK: The file

    private static func read(_ url: URL) -> Record {
        files.withLock { readLocked(url) }
    }

    private static func readLocked(_ url: URL) -> Record {
        guard let data = try? Data(contentsOf: url), let record = try? JSONDecoder().decode(Record.self, from: data)
        else { return .unknown }
        return record
    }

    /// Changes the record and writes it if the change made a difference.
    private static func update(_ url: URL, _ change: (inout Record) -> Void) throws {
        try files.withLock {
            let before = readLocked(url)
            var after = before
            change(&after)
            guard after != before else { return }
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys]
            let directory = url.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try encoder.encode(after).write(to: url, options: .atomic)
            try flush(url)
            try? flush(directory)
        }
    }

    /// Makes the contents of a file, or the entries of a directory, survive a
    /// power loss: `F_FULLFSYNC` where the volume has it, else `fsync`.
    private static func flush(_ url: URL) throws {
        let descriptor = open(url.path, O_RDONLY)
        guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        defer { close(descriptor) }
        if fcntl(descriptor, F_FULLFSYNC) != 0, fsync(descriptor) != 0 {
            throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        }
    }
}
