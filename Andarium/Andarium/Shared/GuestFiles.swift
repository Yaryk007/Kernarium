// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI

/// Paths inside the guest's internal storage, which is all the browser shows.
nonisolated enum GuestPath {
    /// What the guest's Files app shows as internal storage. The browser
    /// cannot go above it.
    static let root = GuestFilePath.root

    static func child(_ name: String, of folder: String) -> String { GuestFilePath.child(name, of: folder) }

    /// What a folder is called on screen.
    static func name(of path: String) -> String {
        path == root ? "Internal storage" : GuestFilePath.name(of: path)
    }

    static func parent(of path: String) -> String {
        path == root ? root : GuestFilePath.parent(of: path)
    }

    /// The name of a file's folder, for a list that mixes folders.
    static func parentName(of path: String) -> String { name(of: parent(of: path)) }
}

/// One entry of a folder, as the sync service lists it.
nonisolated struct GuestFile: Identifiable, Hashable, Sendable {
    var path: String
    var isFolder: Bool
    var size: UInt64
    var modified: Date

    var id: String { path }
    var name: String { GuestPath.name(of: path) }
}

/// Why a folder has no listing.
nonisolated enum GuestFolderProblem: Error, Equatable {
    /// Android refuses the shell user this folder: `Android/data` and
    /// `Android/obb`.
    case refused
    case unavailable(String)
}

extension GuestFolderProblem: LocalizedError {
    var errorDescription: String? {
        switch self {
        case .refused: "Android doesn\u{2019}t allow this folder to be opened."
        case let .unavailable(reason): reason
        }
    }
}

/// Keeps the access the document picker gave to a file open for as long as
/// anything holds the pick, and ends it after.
nonisolated final class SecurityScope: @unchecked Sendable {
    private let url: URL
    private let started: Bool

    init(_ url: URL) {
        self.url = url
        started = url.startAccessingSecurityScopedResource()
    }

    deinit {
        if started { url.stopAccessingSecurityScopedResource() }
    }
}

/// A file the document picker gave back. The preview's have no URL.
nonisolated struct FilePick: Identifiable, Sendable {
    var url: URL? = nil
    var name: String
    var size: UInt64
    var modified = Date.now
    var scope: SecurityScope? = nil

    var id: String { name }

    /// A file the picker returned, whose size may not be known yet when it is
    /// still in iCloud: the transfer reads the real one once it has the file.
    static func picked(_ url: URL) -> FilePick {
        let scope = SecurityScope(url)
        let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
        return FilePick(
            url: url, name: url.lastPathComponent, size: UInt64(max(values?.fileSize ?? 0, 0)),
            modified: values?.contentModificationDate ?? .now, scope: scope)
    }
}

/// What the browser asks of the guest. The device's goes over the app's adb
/// channel (`Live/GuestFiles.swift`); the Simulator preview's is a tree of mock
/// files. Failures are `LocalizedError`s in the words the person sees, and a
/// cancelled task throws `CancellationError` after it has cleaned up.
protocol GuestFilesBackend: AnyObject {
    func list(_ folder: String) async throws -> [GuestFile]

    /// Free space of internal storage; nil when it cannot be read now.
    func freeBytes() async -> UInt64?

    /// Clears `folder` of what a stopped import left in it, and checks that
    /// the guest and this device can take `bytes` more. Throws, in words, when
    /// they cannot.
    func prepareImport(bytes: UInt64, into folder: String) async throws

    /// Checks that this device has room to stage `bytes` of export.
    func prepareExport(bytes: UInt64) async throws

    /// The bytes an export of `file` moves: a folder is walked.
    func measure(_ file: GuestFile) async throws -> UInt64

    /// Writes `pick` at `path`. A name that is taken is an error unless
    /// `replacing`, which never replaces a folder. On cancellation or failure
    /// nothing is left at `path` and an earlier file of that name is whole.
    /// `progress` is told the bytes sent and the file's real length.
    func send(
        _ pick: FilePick, to path: String, replacing: Bool,
        progress: @escaping @Sendable (_ moved: UInt64, _ total: UInt64) -> Void
    ) async throws

    /// Copies `file` into the local `directory`, a folder with its whole tree,
    /// and returns what it made. On cancellation or failure nothing is left in
    /// `directory`.
    func receive(
        _ file: GuestFile, into directory: URL,
        progress: @escaping @Sendable (_ moved: UInt64, _ total: UInt64) -> Void
    ) async throws -> URL

    /// What Import opens instead of the system's document picker: the
    /// Simulator has no documents to pick. Nil on a device.
    func importPickerStandIn(deliver: @escaping ([FilePick]) -> Void) -> AnyView?
}

/// Where an export is staged until the action menu has taken it, and the
/// room it needs there.
nonisolated enum GuestFileStaging {
    static var directory: URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("GuestFiles", isDirectory: true)
    }

    /// Removes everything an ended process staged.
    static func sweep() {
        try? FileManager.default.removeItem(at: directory)
    }

    /// A folder of its own for one export.
    static func makeTransferDirectory() throws -> URL {
        let url = directory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
}
