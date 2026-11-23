// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// A folder of Andarium's own in the temporary directory that holds the files
/// of one pick, when the document picker made copies of them (`CopyDocumentPicker`). The
/// folder lives as long as anything holds a pick of it: the install or the
/// import that uses the files, and what its retry needs, and it is removed
/// when the last holder lets go. A process that ended first leaves it to the
/// next launch's sweep.
nonisolated final class PickedFolder: @unchecked Sendable {
    /// The one folder of the temporary directory that holds every pick's.
    static var directory: URL { directory(in: FileManager.default.temporaryDirectory) }

    /// The folder in `temporary`: a test (the launch's sweep) gives a root of its own.
    static func directory(in temporary: URL) -> URL {
        temporary.appendingPathComponent("PickedFiles", isDirectory: true)
    }

    /// Removes the folders an ended process left, by the exact path of the one
    /// folder that holds them. Nothing else in the temporary directory is
    /// touched.
    static func sweep(in root: URL = directory) {
        try? FileManager.default.removeItem(at: root)
    }

    /// This pick's own folder, in `root` and with a name no other pick has. A
    /// test gives a root of its own.
    let directory: URL

    init(in root: URL = PickedFolder.directory) throws {
        directory = root.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    deinit { discard() }

    /// Whether `file` is in the application's temporary directory, where the
    /// picker puts the copies it makes. The file of a provider that handed
    /// over the original is not: it is never moved or removed. A copy is the
    /// directory entry itself, so both the folder that holds the entry and
    /// what the entry names must be here: a link elsewhere to a file here, and
    /// a link here to a file elsewhere, are no copies of the picker's.
    static func isInTemporaryDirectory(_ file: URL) -> Bool {
        let root = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().path
        let holder = file.deletingLastPathComponent().resolvingSymlinksInPath().path
        let target = file.resolvingSymlinksInPath().path
        return (holder == root || holder.hasPrefix(root + "/")) && target.hasPrefix(root + "/")
    }

    /// Takes the copy the picker made (in the system's inbox of the
    /// application, which nothing of ours owns) into the folder by a move, and
    /// returns where it is now: the copy is not left to a place that is
    /// cleaned only when the application is not running. Two copies of one
    /// name are told apart as the guest's Files app does. A file that is not
    /// in the temporary directory is no copy of the picker's and stays where
    /// it is.
    func adopt(_ copy: URL) throws -> URL {
        guard Self.isInTemporaryDirectory(copy) else { return copy }
        let taken = Set((try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? [])
        var name = copy.lastPathComponent
        if taken.contains(name) { name = GuestFileNames.freeName(for: name, taken: taken) }
        let destination = directory.appendingPathComponent(name)
        try FileManager.default.moveItem(at: copy, to: destination)
        return destination
    }

    /// Removes a copy of the picker's that was not taken into a folder, by its
    /// own path. Only a file of the temporary directory is touched.
    static func removeUntaken(_ copy: URL) {
        guard isInTemporaryDirectory(copy) else { return }
        try? FileManager.default.removeItem(at: copy)
    }

    /// Removes one file of the folder at once, for a file the person chose not
    /// to import. Only a file that is in this folder is touched.
    func release(_ file: URL) {
        guard file.deletingLastPathComponent().standardizedFileURL.path == directory.standardizedFileURL.path else { return }
        try? FileManager.default.removeItem(at: file)
    }

    /// Removes the folder and what is in it; once is as good as many.
    func discard() {
        try? FileManager.default.removeItem(at: directory)
    }
}

/// Why the files of a pick did not arrive, in the words the alert shows.
nonisolated enum PickedFileFailure: Error, LocalizedError {
    /// The copy the picker made could not be taken into the staging folder.
    case notTaken

    var errorDescription: String? {
        String(localized: "Andarium couldn\u{2019}t take the file from Files.")
    }
}
