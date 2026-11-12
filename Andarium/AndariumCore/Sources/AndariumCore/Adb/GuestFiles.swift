// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Where a guest file lives. The browser starts at `root`, which the guest's
/// own Files app shows as internal storage, and never goes above it.
public enum GuestFilePath {
    public static let root = "/storage/emulated/0"

    public static func child(_ name: String, of folder: String) -> String {
        folder == "/" ? "/" + name : folder + "/" + name
    }

    /// The last component.
    public static func name(of path: String) -> String {
        path.split(separator: "/").last.map(String.init) ?? ""
    }

    /// The folder holding `path`.
    public static func parent(of path: String) -> String {
        guard let slash = path.lastIndex(of: "/"), slash != path.startIndex else { return "/" }
        return String(path[path.startIndex..<slash])
    }

    /// Whether `path` is `root` or below it, by components: `..` and `.`
    /// never appear, and an empty component never does either.
    public static func isInsideRoot(_ path: String) -> Bool {
        guard path == root || path.hasPrefix(root + "/") else { return false }
        return path.dropFirst(root.count).split(separator: "/", omittingEmptySubsequences: false)
            .dropFirst()
            .allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
    }

    /// Whether `name` can be a file's name in a folder: one component of at
    /// most 255 bytes, which is what Android's file system holds.
    public static func isValidName(_ name: String) -> Bool {
        !name.isEmpty && name != "." && name != ".." && !name.contains("/") && !name.contains("\0")
            && name.utf8.count <= 255
    }
}

/// How an imported name is chosen when it is taken.
public enum GuestFileNames {
    /// "Clip.mov" as "Clip (1).mov", the way Android names a second copy, with
    /// the first number whose name is not in `taken`.
    public static func freeName(for name: String, taken: Set<String>) -> String {
        let base = (name as NSString).deletingPathExtension
        let suffix = (name as NSString).pathExtension
        var count = 1
        while true {
            let candidate = suffix.isEmpty ? "\(base) (\(count))" : "\(base) (\(count)).\(suffix)"
            if !taken.contains(candidate) { return candidate }
            count += 1
        }
    }

    /// `names` with every repeat renamed to a free name, in order. The first
    /// of a name keeps it, and a rename never takes a name that is in `taken`
    /// or one of the others.
    public static func distinct(_ names: [String], avoiding taken: Set<String> = []) -> [String] {
        var used = taken.union(names)
        var seen = Set<String>()
        return names.map { name in
            if seen.insert(name).inserted { return name }
            let free = freeName(for: name, taken: used)
            used.insert(free)
            return free
        }
    }
}

/// One entry of a guest folder, as the browser shows it.
public struct GuestEntry: Equatable, Sendable {
    public var name: String
    public var isFolder: Bool
    public var size: UInt64
    public var modified: Date

    public init(name: String, isFolder: Bool, size: UInt64, modified: Date) {
        self.name = name
        self.isFolder = isFolder
        self.size = size
        self.modified = modified
    }
}

/// A file or folder below a folder that is being exported.
public struct GuestTreeItem: Equatable, Sendable {
    /// From the exported folder, with `/` between components.
    public var relativePath: String
    public var isFolder: Bool
    public var size: UInt64
    public var modified: Date
}

/// Why a file operation did not do what was asked, besides the adb channel's
/// own failures (`AdbError`). The application words each case.
public enum GuestFilesError: Error, Equatable, Sendable {
    /// Android does not let the shell user open this folder.
    case refused
    /// The path is not there.
    case missing
    /// The destination name is taken by a file and the import does not
    /// replace.
    case exists
    /// The destination name is a folder, which an import never replaces.
    case folderInTheWay
    /// The file is not the length it was when the transfer began.
    case changed
    /// The guest has less free room than the transfer needs.
    case guestFull(needed: UInt64, free: UInt64)
    /// The device has less free room than the transfer needs (`TransferRoom`).
    case deviceFull(needed: UInt64, free: UInt64)
    /// The path is not inside internal storage, or its name is not one a file
    /// can have.
    case badPath
}

/// Where an import's bytes come from.
public protocol AdbSource: AnyObject {
    /// Up to `limit` bytes, or nil at the end.
    func read(limit: Int) throws -> Data?
}

/// What the file browser asks of the guest's shell and sync service, on a
/// connection the caller made and arms the watchdog of.
public enum GuestFiles {
    // MARK: Listing

    /// Hidden files, which the guest's Files app does not show, and the
    /// partial files of an import.
    public static func isHidden(_ name: String) -> Bool { name.hasPrefix(".") }

    /// The folder's entries, hidden ones left out. adbd lists a folder it
    /// cannot open as it lists an empty one, with no entries (some folders
    /// list `.` and `..` and nothing else), so a listing with nothing in it is
    /// checked with the shell: `refused` when Android does not let the shell
    /// user open the folder, `missing` when it is not there.
    public static func list(_ folder: String, on connection: AdbConnection) throws -> [GuestEntry] {
        guard GuestFilePath.isInsideRoot(folder) else { throw GuestFilesError.badPath }
        let sync = try AdbSync(connection: connection)
        var raw: [AdbDirectoryEntry]
        var entries: [GuestEntry] = []
        do {
            raw = try sync.list(folder)
            raw.removeAll { $0.name == "." || $0.name == ".." }
            for item in raw where !isHidden(item.name) {
                if let entry = try entry(for: item, in: folder, using: sync) { entries.append(entry) }
            }
        } catch {
            sync.quit()
            throw error
        }
        sync.quit()
        if raw.isEmpty { try checkReadable(folder, on: connection) }
        return entries
    }

    /// Throws `refused` or `missing` unless the shell user can open `folder`.
    public static func checkReadable(_ folder: String, on connection: AdbConnection) throws {
        guard GuestFilePath.isInsideRoot(folder) else { throw GuestFilesError.badPath }
        let result = try connection.runShell("ls -A -- \(quotedForShell(folder)) >/dev/null")
        guard result.exitCode != 0 else { return }
        let text = String(decoding: result.stderr, as: UTF8.self)
        throw text.contains("No such file") || text.contains("Not a directory")
            ? GuestFilesError.missing : GuestFilesError.refused
    }

    /// A file or folder as listed. A link stands for what it points at,
    /// and one that points at nothing, or at something that is neither a file
    /// nor a folder, is left out.
    private static func entry(for item: AdbDirectoryEntry, in folder: String, using sync: AdbSync) throws -> GuestEntry? {
        var mode = item.mode
        var size = item.size
        var modified = item.modified
        if item.isSymlink {
            let target = try sync.stat(GuestFilePath.child(item.name, of: folder))
            guard target.exists else { return nil }
            mode = target.mode
            size = target.size
            modified = target.modified
        }
        let type = mode & AdbFileMode.typeMask
        guard type == AdbFileMode.directory || type == AdbFileMode.regular else { return nil }
        return GuestEntry(
            name: item.name, isFolder: type == AdbFileMode.directory, size: type == AdbFileMode.directory ? 0 : size,
            modified: Date(timeIntervalSince1970: TimeInterval(modified)))
    }

    /// Every file and folder below `folder`, parents before their children.
    /// Links are not followed, and a folder the shell user cannot open
    /// comes out empty, as adbd lists it. `visit` is called for each
    /// directory listed, to show signs of life on a large tree.
    public static func walk(_ folder: String, using sync: AdbSync, visit: () -> Void = {}) throws -> [GuestTreeItem] {
        guard GuestFilePath.isInsideRoot(folder) else { throw GuestFilesError.badPath }
        var items: [GuestTreeItem] = []
        var pending: [(path: String, relative: String)] = [(folder, "")]
        while let next = pending.popLast() {
            visit()
            let raw = try sync.list(next.path)
            var folders: [(String, String)] = []
            for item in raw where item.name != "." && item.name != ".." && !isPartial(item.name) {
                let relative = next.relative.isEmpty ? item.name : next.relative + "/" + item.name
                let type = item.mode & AdbFileMode.typeMask
                let modified = Date(timeIntervalSince1970: TimeInterval(item.modified))
                if type == AdbFileMode.directory {
                    items.append(GuestTreeItem(relativePath: relative, isFolder: true, size: 0, modified: modified))
                    folders.append((GuestFilePath.child(item.name, of: next.path), relative))
                } else if type == AdbFileMode.regular {
                    items.append(GuestTreeItem(relativePath: relative, isFolder: false, size: item.size, modified: modified))
                }
            }
            // A folder's own entries are listed when the walk reaches it.
            pending.append(contentsOf: folders.reversed())
        }
        return items
    }

    // MARK: Download

    /// Copies the file at `path` into `sink` and returns its length. `progress`
    /// sees the running total.
    @discardableResult
    public static func download(
        _ path: String, using sync: AdbSync, into sink: any AdbSink, progress: (UInt64) -> Void
    ) throws -> UInt64 {
        guard GuestFilePath.isInsideRoot(path) else { throw GuestFilesError.badPath }
        let stat = try sync.stat(path)
        guard stat.exists else { throw GuestFilesError.missing }
        var received: UInt64 = 0
        progress(0)
        try sync.receive(path) { data in
            try sink.write(data)
            received += UInt64(data.count)
            progress(received)
        }
        guard received == stat.size else { throw GuestFilesError.changed }
        return received
    }

    // MARK: Upload

    /// Reads from `source` in pieces of this size.
    public static let uploadPiece = 256 * 1024

    /// The name a file is written under until it has all arrived: in the
    /// destination's folder, so the move is a rename, and hidden, so no
    /// listing shows it. `token` makes it the transfer's own.
    public static func temporaryPath(in folder: String, token: String) -> String {
        GuestFilePath.child(".andarium-\(token).part", of: folder)
    }

    public static func isPartial(_ name: String) -> Bool {
        name.hasPrefix(".andarium-") && name.hasSuffix(".part")
    }

    /// Writes `source` at `path`: the bytes go to `temporary` first, and one
    /// move puts them under their name once they have all arrived, so a
    /// transfer that stops leaves nothing at `path` and an earlier file of
    /// that name whole. The same command then has the guest's media index
    /// complete the file's row (`scanCommand`), which a file written through
    /// the storage emulation has only in part. Returns the length. Failing
    /// leaves `temporary` for the caller to remove (`removeCommand`), since
    /// the connection it was written on may be gone.
    ///
    /// With `replacing` an earlier file is replaced and a folder of that name
    /// is `folderInTheWay`; otherwise any earlier entry is `exists`.
    @discardableResult
    public static func upload(
        _ source: any AdbSource, modified: Date, to path: String, temporary: String, replacing: Bool,
        on connection: AdbConnection, progress: (UInt64) -> Void
    ) throws -> UInt64 {
        guard GuestFilePath.isInsideRoot(path), GuestFilePath.isInsideRoot(temporary),
              GuestFilePath.isValidName(GuestFilePath.name(of: path)),
              GuestFilePath.parent(of: path) == GuestFilePath.parent(of: temporary)
        else { throw GuestFilesError.badPath }
        let sync = try AdbSync(connection: connection)
        var sent: UInt64 = 0
        do {
            try sync.send(
                temporary, mode: AdbFileMode.regular | 0o664,
                modified: UInt32(clamping: max(0, Int64(modified.timeIntervalSince1970))),
                next: { try source.read(limit: uploadPiece) },
                sent: { sent = $0; progress($0) })
            let stat = try sync.stat(temporary)
            guard stat.exists, stat.size == sent else { throw GuestFilesError.changed }
        } catch {
            sync.quit()
            throw error
        }
        sync.quit()
        let result = try connection.runShell(finishCommand(temporary: temporary, path: path, replacing: replacing))
        switch result.exitCode {
        case 0: return sent
        case exitExists: throw GuestFilesError.exists
        case exitFolder: throw GuestFilesError.folderInTheWay
        default:
            let text = String(decoding: result.stderr, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            throw AdbError.remote(text.isEmpty ? "the move to the final name failed" : text)
        }
    }

    static let exitExists: Int = 17
    static let exitFolder: Int = 21

    /// The shell command that puts a finished upload under its name and then
    /// has the media index look at it. The scan runs only when the move
    /// succeeded, and its own outcome never changes the command's: a file in
    /// place is an import that worked.
    static func finishCommand(temporary: String, path: String, replacing: Bool) -> String {
        let from = quotedForShell(temporary)
        let to = quotedForShell(path)
        let move = replacing
            ? "if [ -d \(to) ]; then exit \(exitFolder); fi; mv -f -- \(from) \(to)"
            : "if [ -e \(to) ] || [ -L \(to) ]; then exit \(exitExists); fi; mv -- \(from) \(to)"
        return "\(move) && { \(scanCommand(for: path)) >/dev/null 2>&1; true; }"
    }

    /// Removes a partial file; the command succeeds when it is not there.
    public static func removeCommand(for temporary: String) -> String {
        "rm -f -- \(quotedForShell(temporary))"
    }

    /// Removes the partial files a stopped import left in `folder`.
    public static func removeStaleCommand(in folder: String) -> String {
        "rm -f -- \(quotedForShell(folder))/.andarium-*.part"
    }

    // MARK: Space

    /// Prints the room of the file system `path` is on, one line, in kibibytes.
    public static func freeSpaceCommand(for path: String) -> String {
        "df -Pk \(quotedForShell(path))"
    }

    /// The bytes available to the shell user in `df -Pk`'s output; nil when it
    /// cannot be read.
    public static func parseFreeSpace(_ output: String) -> UInt64? {
        let lines = output.split(whereSeparator: \.isNewline).map { $0.split(whereSeparator: { $0 == " " || $0 == "\t" }) }
        // Filesystem, 1024-blocks, Used, Available, Capacity, Mounted on.
        guard lines.count >= 2, let row = lines.last, row.count >= 6, let kib = UInt64(row[3]) else { return nil }
        let (bytes, overflow) = kib.multipliedReportingOverflow(by: 1024)
        return overflow ? nil : bytes
    }

    /// The room an import of `bytes` keeps free in the guest after it: the
    /// guest's data disk fails hard when it fills, and Android is unusable
    /// well before that.
    public static let guestReserveBytes: UInt64 = 512 * 1024 * 1024

    /// Whether the guest can take `bytes` and keep its reserve.
    public static func guestHasRoom(for bytes: UInt64, free: UInt64) -> Bool {
        free >= bytes && free - bytes >= guestReserveBytes
    }

    // MARK: Media

    /// Has the guest's media index complete the row of the file at `path`.
    /// The storage emulation indexes a file the shell user writes only in
    /// part: its row is pending, owned by the shell, and has no size or date,
    /// so MediaStore keeps the file from the guest's other apps, and a
    /// replaced file's row goes back to that state. The scanner broadcast
    /// completes the row, with the type and, for media, the metadata, and
    /// costs a fraction of the media provider's own `scan_file` call, which
    /// starts a Java process.
    ///
    /// The path travels as a URI, so every byte but the unreserved ones and
    /// `/` is percent-encoded.
    public static func scanCommand(for path: String) -> String {
        "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d \(quotedForShell("file://" + uriPath(path)))"
    }

    private static let uriSafe = CharacterSet(
        charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/")

    static func uriPath(_ path: String) -> String {
        path.addingPercentEncoding(withAllowedCharacters: uriSafe) ?? path
    }
}

/// What a transfer leaves free on the device. An import grows the guest's data
/// disk and an export stages a copy, and the run is paused for want of space
/// below `RunSpace.pauseBelowBytes`, so a transfer that would take the device
/// there is refused before it starts, not stopped half way by the pause. The
/// system and the share sheet need some room besides.
public enum TransferRoom {
    public static let deviceReserveBytes: UInt64 = RunSpace.pauseBelowBytes + 256 * Bytes.mib

    /// Whether the device, with `free` bytes, can take `bytes` and keep its
    /// reserve.
    public static func deviceHasRoom(for bytes: UInt64, free: UInt64) -> Bool {
        free >= bytes && free - bytes >= deviceReserveBytes
    }
}
