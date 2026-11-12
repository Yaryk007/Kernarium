// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI

/// A file operation's failure in the words a transfer shows: the browser's
/// own errors, or an adb failure worded for the files.
nonisolated enum GuestFilesFailure {
    /// `device` is "iPad", "iPhone" or "Mac".
    static func presented(_ error: Error, device: String) -> Error {
        guard let files = error as? GuestFilesError else {
            return AdbOperationRunner.presented(error, activity: .files)
        }
        return AdbFailure(message: message(files, device: device))
    }

    static func message(_ error: GuestFilesError, device: String) -> String {
        switch error {
        case .refused: "Android doesn\u{2019}t allow this folder to be opened."
        case .missing: "It is no longer in the guest."
        case .exists: "A file with that name is already there."
        case .folderInTheWay: "A folder with that name is already there."
        case .changed: "The file changed while it was being copied."
        case .guestFull: "Not enough space in the guest."
        case .deviceFull: "Not enough space on this \(device)."
        case .badPath: "The guest can\u{2019}t take a file with this name."
        }
    }
}

/// Reads a file the picker returned in pieces, never whole in memory.
nonisolated final class LocalFileSource: AdbSource, @unchecked Sendable {
    private let handle: FileHandle

    init(_ url: URL) throws {
        handle = try FileHandle(forReadingFrom: url)
    }

    deinit { try? handle.close() }

    func read(limit: Int) throws -> Data? {
        guard let data = try handle.read(upToCount: limit), !data.isEmpty else { return nil }
        return data
    }
}

/// Passes on a transfer's progress no more often than the screen needs it.
nonisolated final class ProgressThrottle: @unchecked Sendable {
    private let lock = NSLock()
    private let interval: Duration
    private var last: ContinuousClock.Instant
    private let deliver: @Sendable (UInt64, UInt64) -> Void

    init(interval: Duration = .milliseconds(100), deliver: @escaping @Sendable (UInt64, UInt64) -> Void) {
        self.interval = interval
        self.deliver = deliver
        last = ContinuousClock.now - interval
    }

    /// The first report, the last one (`moved` at `total`) and one in each
    /// interval between are delivered.
    func report(_ moved: UInt64, _ total: UInt64) {
        let due = lock.withLock { () -> Bool in
            let now = ContinuousClock.now
            guard moved >= total || now - last >= interval else { return false }
            last = now
            return true
        }
        if due { deliver(moved, total) }
    }
}

/// One import: the file goes to a hidden temporary name in its folder, and one
/// move gives it its own and has the media index complete its row
/// (`GuestFiles.upload`).
nonisolated enum FileUploadOperation {
    static let idleLimit = Duration.seconds(60)

    static func run(
        session: AdbSession, coordinator: FileCoordinatorBox, pick: FilePick, path: String, temporary: String,
        replacing: Bool, progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) throws {
        guard let url = pick.url else { throw AdbError.io(ENOENT) }
        var result: Result<Void, Error> = .success(())
        var coordinationError: NSError?
        // The read is coordinated, which brings an iCloud file down first.
        coordinator.current.coordinate(readingItemAt: url, options: [], error: &coordinationError) { readable in
            result = Result {
                try send(
                    session: session, file: readable, modifiedFallback: pick.modified, path: path,
                    temporary: temporary, replacing: replacing, progress: progress)
            }
        }
        if let coordinationError { throw coordinationError }
        try result.get()
    }

    private static func send(
        session: AdbSession, file: URL, modifiedFallback: Date, path: String, temporary: String, replacing: Bool,
        progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) throws {
        let values = try file.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey, .isDirectoryKey])
        if values.isDirectory == true {
            throw AdbFailure(message: "Folders and packages can\u{2019}t be imported.")
        }
        let size = UInt64(max(values.fileSize ?? 0, 0))
        let source = try LocalFileSource(file)
        let watchdog = session.watchdog
        progress(0, size)
        try session.run { connection in
            watchdog.arm(idle: idleLimit)
            try GuestFiles.upload(
                source, modified: values.contentModificationDate ?? modifiedFallback, to: path,
                temporary: temporary, replacing: replacing, on: connection,
                progress: { sent in
                    watchdog.touch()
                    progress(sent, size)
                })
        }
    }
}

/// One export: a file, or a folder with its whole tree, copied into a folder
/// of the app's own.
nonisolated enum FileDownloadOperation {
    static let idleLimit = Duration.seconds(60)

    static func measure(session: AdbSession, file: GuestFile) throws -> UInt64 {
        guard file.isFolder else { return file.size }
        let watchdog = session.watchdog
        return try session.run { connection in
            watchdog.arm(idle: idleLimit)
            try GuestFiles.checkReadable(file.path, on: connection)
            let sync = try AdbSync(connection: connection)
            defer { sync.quit() }
            let items = try GuestFiles.walk(file.path, using: sync, visit: { watchdog.touch() })
            return items.reduce(0) { $0 + $1.size }
        }
    }

    /// Copies `file` into `directory` and returns what it made. On a failure
    /// nothing made stays.
    static func run(
        session: AdbSession, file: GuestFile, into directory: URL,
        progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) throws -> URL {
        let manager = FileManager.default
        let output = directory.appendingPathComponent(file.name, isDirectory: file.isFolder)
        let watchdog = session.watchdog
        do {
            try session.run { connection in
                watchdog.arm(idle: idleLimit)
                if file.isFolder { try GuestFiles.checkReadable(file.path, on: connection) }
                let sync = try AdbSync(connection: connection)
                defer { sync.quit() }
                if file.isFolder {
                    try copyTree(file, to: output, using: sync, session: session, progress: progress)
                } else {
                    try copyFile(file.path, size: file.size, modified: file.modified, to: output, using: sync, watchdog: watchdog) {
                        progress($0, file.size)
                    }
                }
            }
        } catch {
            try? manager.removeItem(at: output)
            throw error
        }
        return output
    }

    private static func copyTree(
        _ folder: GuestFile, to output: URL, using sync: AdbSync, session: AdbSession,
        progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) throws {
        let watchdog = session.watchdog
        let items = try GuestFiles.walk(folder.path, using: sync, visit: { watchdog.touch() })
        let total = items.reduce(0) { $0 + $1.size }
        let manager = FileManager.default
        try manager.createDirectory(at: output, withIntermediateDirectories: true)
        var done: UInt64 = 0
        progress(0, total)
        for item in items {
            let local = output.appendingPathComponent(item.relativePath, isDirectory: item.isFolder)
            if item.isFolder {
                try manager.createDirectory(at: local, withIntermediateDirectories: true)
                continue
            }
            let before = done
            try copyFile(
                GuestFilePath.child(item.relativePath, of: folder.path), size: item.size, modified: item.modified,
                to: local, using: sync, watchdog: watchdog
            ) { progress(before + $0, total) }
            done += item.size
        }
        progress(total, total)
    }

    private static func copyFile(
        _ path: String, size: UInt64, modified: Date, to local: URL, using sync: AdbSync, watchdog: AdbWatchdog,
        progress: @escaping (UInt64) -> Void
    ) throws {
        let manager = FileManager.default
        guard manager.createFile(atPath: local.path, contents: nil) else { throw CocoaError(.fileWriteUnknown) }
        let sink = try StagedFileSink(url: local)
        defer { sink.close() }
        try GuestFiles.download(path, using: sync, into: sink) { received in
            watchdog.touch()
            progress(received)
        }
        sink.close()
        try? manager.setAttributes([.modificationDate: modified], ofItemAtPath: local.path)
    }
}

/// The operations the file browser asks of Android over adb, each on a
/// connection of its own, so a listing is never held up by a transfer.
extension AdbGuestOperations {
    private func perform<Output: Sendable>(
        cancellable: Bool = true, onCancel: @escaping @Sendable () -> Void = {},
        _ body: @escaping @Sendable (AdbSession) throws -> Output
    ) async throws -> Output {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .files) }
        let device = device
        do {
            return try await AdbOperationRunner.run(
                identity: identity, clock: clock, cancellable: cancellable, connector: connector,
                onCancel: onCancel, body)
        } catch {
            throw GuestFilesFailure.presented(error, device: device)
        }
    }

    /// The folder's entries. `GuestFilesError.refused` and `.missing` come as
    /// they are, for the browser's own states.
    func listFolder(_ folder: String) async throws -> [GuestEntry] {
        guard let identity else { throw AdbFailure(.keyRejected, activity: .files) }
        do {
            return try await AdbOperationRunner.run(identity: identity, clock: clock, connector: connector) { session in
                try session.run { connection in
                    session.watchdog.arm(idle: .seconds(30), overall: .seconds(180))
                    return try GuestFiles.list(folder, on: connection)
                }
            }
        } catch let error as GuestFilesError where error == .refused || error == .missing {
            throw error
        } catch {
            throw GuestFilesFailure.presented(error, device: device)
        }
    }

    /// The free bytes of internal storage, or nil when they cannot be read.
    func guestFreeBytes() async -> UInt64? {
        guard let result = try? await shell(GuestFiles.freeSpaceCommand(for: GuestFilePath.root), limit: .seconds(15)),
              result.exitCode == 0 else { return nil }
        return GuestFiles.parseFreeSpace(result.text)
    }

    /// Removes what a stopped import left in `folder` and checks the room an
    /// import of `bytes` needs in the guest and on this device (`hostFree`).
    func prepareImport(bytes: UInt64, into folder: String, hostFree: UInt64) async throws {
        guard GuestFilePath.isInsideRoot(folder) else { throw GuestFilesFailure.presented(GuestFilesError.badPath, device: device) }
        guard TransferRoom.deviceHasRoom(for: bytes, free: hostFree) else {
            throw GuestFilesFailure.presented(GuestFilesError.deviceFull(needed: bytes, free: hostFree), device: device)
        }
        let command = "\(GuestFiles.removeStaleCommand(in: folder)); \(GuestFiles.freeSpaceCommand(for: folder))"
        let result: ShellResult
        do {
            result = try await shell(command, limit: .seconds(30))
        } catch {
            throw GuestFilesFailure.presented(error, device: device)
        }
        // A reading that cannot be had does not stop an import: the guest
        // then says so itself, if it must.
        if let free = GuestFiles.parseFreeSpace(result.text), !GuestFiles.guestHasRoom(for: bytes, free: free) {
            throw GuestFilesFailure.presented(GuestFilesError.guestFull(needed: bytes, free: free), device: device)
        }
    }

    func measureTree(_ file: GuestFile) async throws -> UInt64 {
        try await perform { session in try FileDownloadOperation.measure(session: session, file: file) }
    }

    /// Writes the pick at `path`. A stopped or failed upload removes its
    /// temporary file, on a connection of its own that nothing cancels.
    func upload(
        _ pick: FilePick, to path: String, replacing: Bool,
        progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) async throws {
        let temporary = GuestFiles.temporaryPath(
            in: GuestFilePath.parent(of: path), token: String(UUID().uuidString.prefix(8)).lowercased())
        let coordinator = FileCoordinatorBox()
        let throttle = ProgressThrottle(deliver: progress)
        do {
            try await perform(onCancel: { coordinator.cancel() }) { session in
                try FileUploadOperation.run(
                    session: session, coordinator: coordinator, pick: pick, path: path, temporary: temporary,
                    replacing: replacing, progress: throttle.report)
            }
        } catch {
            removeInBackground(temporary)
            throw error
        }
    }

    /// Removes a partial file. It waits for a guest that is paused, and for
    /// one that is back, so it never holds a transfer's end up.
    private func removeInBackground(_ temporary: String) {
        let operations = self
        Task.detached {
            _ = try? await operations.perform(cancellable: false) { session in
                try session.run { connection in
                    session.watchdog.arm(idle: .seconds(15), overall: .seconds(30))
                    try connection.runShell(GuestFiles.removeCommand(for: temporary))
                }
            }
        }
    }

    func download(
        _ file: GuestFile, into directory: URL, progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) async throws -> URL {
        let throttle = ProgressThrottle(deliver: progress)
        return try await perform { session in
            try FileDownloadOperation.run(session: session, file: file, into: directory, progress: throttle.report)
        }
    }
}

/// The file browser's backend on the running guest.
final class LiveGuestFiles: GuestFilesBackend {
    private let operations: () -> AdbGuestOperations
    private let hostFree: @Sendable () -> UInt64

    /// `operations` is read for each call, since the key changes with each
    /// boot; `hostFree` is this device's free space.
    init(operations: @escaping () -> AdbGuestOperations, hostFree: @escaping @Sendable () -> UInt64) {
        self.operations = operations
        self.hostFree = hostFree
    }

    func list(_ folder: String) async throws -> [GuestFile] {
        do {
            return try await operations().listFolder(folder).map {
                GuestFile(
                    path: GuestPath.child($0.name, of: folder), isFolder: $0.isFolder, size: $0.size,
                    modified: $0.modified)
            }
        } catch GuestFilesError.refused {
            throw GuestFolderProblem.refused
        } catch GuestFilesError.missing {
            throw GuestFolderProblem.unavailable("The folder is no longer in the guest.")
        }
    }

    func freeBytes() async -> UInt64? {
        await operations().guestFreeBytes()
    }

    func prepareImport(bytes: UInt64, into folder: String) async throws {
        try await operations().prepareImport(bytes: bytes, into: folder, hostFree: hostFree())
    }

    func prepareExport(bytes: UInt64) async throws {
        let ops = operations()
        let free = hostFree()
        guard TransferRoom.deviceHasRoom(for: bytes, free: free) else {
            throw GuestFilesFailure.presented(GuestFilesError.deviceFull(needed: bytes, free: free), device: ops.device)
        }
    }

    func measure(_ file: GuestFile) async throws -> UInt64 {
        try await operations().measureTree(file)
    }

    func send(
        _ pick: FilePick, to path: String, replacing: Bool,
        progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) async throws {
        try await operations().upload(pick, to: path, replacing: replacing, progress: progress)
    }

    func receive(
        _ file: GuestFile, into directory: URL, progress: @escaping @Sendable (UInt64, UInt64) -> Void
    ) async throws -> URL {
        try await operations().download(file, into: directory, progress: progress)
    }

    func importPickerStandIn(deliver: @escaping ([FilePick]) -> Void) -> AnyView? { nil }
}
