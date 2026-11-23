// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore

/// File calls for the snapshot store that do what the live ones do, and in
/// addition (i) throw a real error at the call a test names, instead of making
/// it, so the store's own error paths run, and (ii) keep a copy of the guest's
/// directory from before each call (an APFS clone of the tree, so cheap), which
/// is the state a crash between two calls would leave. Every call is numbered
/// from 1 in the order the store makes it.
final class SnapshotFaults: @unchecked Sendable {
    struct Call: Equatable {
        var name: String
        var path: String
    }

    private let lock = NSLock()
    private let live = SnapshotFileOperations.live
    private let guestDirectory: URL
    private let captureRoot: URL?
    private var failure: (matches: @Sendable (Int, Call) -> Bool, error: any Error)?
    private var nextCall = 1
    private var recorded: [Call] = []
    private var captured: [URL] = []

    /// `captureTo`: a directory that takes a copy of `guestDirectory` before
    /// each call; nil keeps none.
    init(guestDirectory: URL, captureTo: URL? = nil) {
        self.guestDirectory = guestDirectory
        captureRoot = captureTo
        if let captureTo { try? FileManager.default.createDirectory(at: captureTo, withIntermediateDirectories: true) }
    }

    /// Fails call number `call` (counted from the last restart) with `error`, once.
    func fail(call: Int, with error: any Error = POSIXError(.EIO)) {
        lock.lock()
        defer { lock.unlock() }
        failure = ({ number, _ in number == call }, error)
    }

    /// Fails the first call `matches` accepts with `error`, once.
    func fail(when matches: @escaping @Sendable (Call) -> Bool, with error: any Error = POSIXError(.EIO)) {
        lock.lock()
        defer { lock.unlock() }
        failure = ({ _, call in matches(call) }, error)
    }

    /// Forgets the calls so far; the next one is number 1 again.
    func restart() {
        lock.lock()
        defer { lock.unlock() }
        nextCall = 1
        recorded = []
        failure = nil
    }

    var calls: [Call] {
        lock.lock()
        defer { lock.unlock() }
        return recorded
    }

    /// The copies taken, in order: copy `n` is the guest's directory before
    /// call `n + 1`.
    var captures: [URL] {
        lock.lock()
        defer { lock.unlock() }
        return captured
    }

    /// A copy of the guest's directory as it is now (after the last call).
    @discardableResult
    func captureNow() -> URL? {
        lock.lock()
        defer { lock.unlock() }
        return captureLocked()
    }

    private func captureLocked() -> URL? {
        guard let captureRoot else { return nil }
        let copy = captureRoot.appendingPathComponent(String(format: "%03d", captured.count))
        guard clonefile(guestDirectory.path, copy.path, 0) == 0 else { return nil }
        captured.append(copy)
        return copy
    }

    /// Counts a call, keeps the state before it, and says whether it fails.
    private func enter(_ name: String, _ url: URL) throws {
        lock.lock()
        defer { lock.unlock() }
        let number = nextCall
        nextCall += 1
        let call = Call(name: name, path: url.path)
        recorded.append(call)
        _ = captureLocked()
        if let failure, failure.matches(number, call) {
            self.failure = nil
            throw failure.error
        }
    }

    var operations: SnapshotFileOperations {
        let live = self.live
        return SnapshotFileOperations(
            createDirectory: { [self] in try enter("createDirectory", $0); try live.createDirectory($0) },
            clone: { [self] in try enter("clone", $1); try live.clone($0, $1) },
            rename: { [self] in try enter("rename", $1); try live.rename($0, $1) },
            write: { [self] in try enter("write", $1); try live.write($0, $1) },
            flush: { [self] in try enter("flush", $0); try live.flush($0) },
            remove: { [self] in try enter("remove", $0); try live.remove($0) },
            list: { [self] in try enter("list", $0); return try live.list($0) },
            volumeClones: { live.volumeClones($0) }
        )
    }
}

/// What a guest's directory holds, for comparing two states: each file by
/// its relative path and either its bytes or, for a disk, its fingerprint.
/// Directories are implied by their files; `leftovers` names what a crash
/// leaves under a name of its own.
enum GuestDirectory {
    static func describe(_ directory: URL) -> [String: String] {
        var result: [String: String] = [:]
        let manager = FileManager.default
        for path in (try? manager.subpathsOfDirectory(atPath: directory.path)) ?? [] {
            // The record of the walk the guest owes is not the guest's data: a
            // restore that fails after it was written leaves the walk owed, the
            // safe answer, and `PartialSweepLedgerTests` says when it changes.
            if path == PartialSweepLedger.fileName { continue }
            let url = directory.appendingPathComponent(path)
            var isDirectory: ObjCBool = false
            guard manager.fileExists(atPath: url.path, isDirectory: &isDirectory) else { continue }
            if isDirectory.boolValue {
                continue
            } else if (path as NSString).lastPathComponent.hasPrefix("data.img") {
                result[path] = "disk " + TestDisk.fingerprint(url)
            } else {
                result[path] = ((try? Data(contentsOf: url))?.base64EncodedString()) ?? "unreadable"
            }
        }
        return result
    }

    /// The same description with each snapshot's identity replaced by a
    /// placeholder numbered in the order of the identities, so two runs that
    /// made different snapshots compare equal.
    static func describeIgnoringIdentities(_ directory: URL) -> [String: String] {
        let pattern = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
        let plain = describe(directory)
        var identities = Set<String>()
        for path in plain.keys {
            var search = path.startIndex..<path.endIndex
            while let found = path.range(of: pattern, options: .regularExpression, range: search) {
                identities.insert(String(path[found]))
                search = found.upperBound..<path.endIndex
            }
        }
        let numbered = Dictionary(uniqueKeysWithValues: identities.sorted().enumerated().map { ($1, "<id\($0 + 1)>") })
        func replacing(_ text: String) -> String {
            numbered.reduce(text) { $0.replacingOccurrences(of: $1.key, with: $1.value) }
        }
        var result: [String: String] = [:]
        for (path, value) in plain { result[replacing(path)] = value }
        return result
    }

    /// The differences between two descriptions, for a failure message.
    static func difference(_ a: [String: String], _ b: [String: String]) -> String {
        var lines: [String] = []
        for key in Set(a.keys).union(b.keys).sorted() where a[key] != b[key] {
            lines.append("\(key): \(a[key].map { String($0.prefix(40)) } ?? "absent") vs \(b[key].map { String($0.prefix(40)) } ?? "absent")")
        }
        return lines.joined(separator: "\n")
    }

    /// The files a create, a delete or a restore that did not finish leaves
    /// under names of their own, which the next launch removes.
    static func leftovers(_ directory: URL) -> [String] {
        ((try? FileManager.default.subpathsOfDirectory(atPath: directory.path)) ?? [])
            .filter { $0.contains(".new-") || $0.contains(".gone-") || $0.hasSuffix(".tmp") }
            .sorted()
    }
}
