// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What `STAT` knows about a path on the device.
public struct AdbFileStat: Equatable, Sendable {
    public var mode: UInt32
    public var size: UInt64
    /// Seconds since the epoch.
    public var modified: Int64

    public init(mode: UInt32, size: UInt64, modified: Int64 = 0) {
        self.mode = mode
        self.size = size
        self.modified = modified
    }

    public var exists: Bool { mode != 0 }
    public var isDirectory: Bool { mode & AdbFileMode.typeMask == AdbFileMode.directory }
}

/// The type bits of a POSIX mode.
public enum AdbFileMode {
    public static let typeMask: UInt32 = 0o170000
    public static let directory: UInt32 = 0o040000
    public static let regular: UInt32 = 0o100000
    public static let symlink: UInt32 = 0o120000
}

/// One entry of a directory the sync service listed.
public struct AdbDirectoryEntry: Equatable, Sendable {
    public var name: String
    public var mode: UInt32
    public var size: UInt64
    /// Seconds since the epoch.
    public var modified: Int64

    public init(name: String, mode: UInt32, size: UInt64, modified: Int64) {
        self.name = name
        self.mode = mode
        self.size = size
        self.modified = modified
    }

    public var isDirectory: Bool { mode & AdbFileMode.typeMask == AdbFileMode.directory }
    public var isSymlink: Bool { mode & AdbFileMode.typeMask == AdbFileMode.symlink }
}

/// The sync service (AOSP `adb/SYNC.TXT`): file transfer on a stream of
/// `id, length` requests. `STAT`, `LIST`, `RECV`, `SEND` and `QUIT`, each
/// with the version that carries 64-bit sizes where the device has one.
public final class AdbSync {
    /// A `DATA` payload is at most this long.
    public static let maxChunk = 64 * 1024
    /// `send` writes this many bytes of `DATA` frames at a time.
    public static let sendBatch = 256 * 1024

    private let stream: AdbStream
    private let usesStatV2: Bool
    private let usesListV2: Bool
    private var buffer = Data()
    private var bufferOffset = 0

    /// Opens `sync:` on `connection`.
    public init(connection: AdbConnection) throws {
        stream = try connection.open("sync:")
        usesStatV2 = connection.banner.features.contains("stat_v2")
        usesListV2 = connection.banner.features.contains("ls_v2")
    }

    // MARK: Requests

    private func request(_ id: String, path: String) throws {
        let name = Data(path.utf8)
        var bytes = Data(id.utf8)
        bytes.appendLittleEndian(UInt32(name.count))
        bytes.append(name)
        try stream.write(bytes)
    }

    /// Reads exactly `count` bytes of the reply, which may span messages.
    private func take(_ count: Int) throws -> Data {
        while buffer.count - bufferOffset < count {
            guard let more = try stream.read() else { throw AdbError.guestGone }
            buffer.append(more)
        }
        let start = buffer.startIndex + bufferOffset
        let taken = Data(buffer[start..<(start + count)])
        bufferOffset += count
        // What was taken goes once it is most of the buffer, so a long reply
        // is not copied at every read.
        if bufferOffset == buffer.count {
            buffer.removeAll(keepingCapacity: true)
            bufferOffset = 0
        } else if bufferOffset >= 1 << 20 {
            buffer = Data(buffer[(buffer.startIndex + bufferOffset)...])
            bufferOffset = 0
        }
        return taken
    }

    /// The error a `FAIL` of `length` bytes carries.
    private func failure(length: Int) throws -> AdbError {
        guard length <= 64 * 1024 else { throw AdbError.protocolViolation("a FAIL of \(length) bytes") }
        return AdbError.remote(String(decoding: try take(length), as: UTF8.self))
    }

    /// The size and mode of `path`. A path that is not there has mode 0.
    public func stat(_ path: String) throws -> AdbFileStat {
        try request(usesStatV2 ? "STA2" : "STAT", path: path)
        if usesStatV2 {
            // id, error, dev, ino, mode, nlink, uid, gid, size, atime, mtime, ctime
            let reply = try take(72)
            guard String(decoding: reply.prefix(4), as: UTF8.self) == "STA2" else {
                throw AdbError.protocolViolation("an unexpected reply to STA2")
            }
            guard reply.littleEndian32(at: 4) == 0 else { return AdbFileStat(mode: 0, size: 0) }
            return AdbFileStat(
                mode: reply.littleEndian32(at: 24), size: reply.littleEndian64(at: 40),
                modified: Int64(bitPattern: reply.littleEndian64(at: 56)))
        }
        // id, mode, size, mtime
        let reply = try take(16)
        guard String(decoding: reply.prefix(4), as: UTF8.self) == "STAT" else {
            throw AdbError.protocolViolation("an unexpected reply to STAT")
        }
        return AdbFileStat(
            mode: reply.littleEndian32(at: 4), size: UInt64(reply.littleEndian32(at: 8)),
            modified: Int64(reply.littleEndian32(at: 12)))
    }

    /// The entries of the directory at `path`, in the order the device sends
    /// them, `.` and `..` included. `LIS2` where the device has it, whose
    /// sizes are 64-bit; `LIST` otherwise, whose sizes wrap at 4 GiB. A
    /// reply that cannot be read as a listing is a violation.
    public func list(_ path: String) throws -> [AdbDirectoryEntry] {
        try request(usesListV2 ? "LIS2" : "LIST", path: path)
        // v1: id, mode, size, mtime, namelen. v2: id, error, dev, ino, mode,
        // nlink, uid, gid, size, atime, mtime, ctime, namelen.
        let fixed = usesListV2 ? 76 : 20
        let tag = usesListV2 ? "DNT2" : "DENT"
        var entries: [AdbDirectoryEntry] = []
        while true {
            let header = try take(fixed)
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            if id == "DONE" { return entries }
            guard id == tag else { throw AdbError.protocolViolation("a listing reply \(id)") }
            let mode: UInt32
            let size: UInt64
            let modified: Int64
            let nameLength: Int
            var failed = false
            if usesListV2 {
                failed = header.littleEndian32(at: 4) != 0
                mode = header.littleEndian32(at: 24)
                size = header.littleEndian64(at: 40)
                modified = Int64(bitPattern: header.littleEndian64(at: 56))
                nameLength = Int(header.littleEndian32(at: 72))
            } else {
                mode = header.littleEndian32(at: 4)
                size = UInt64(header.littleEndian32(at: 8))
                modified = Int64(header.littleEndian32(at: 12))
                nameLength = Int(header.littleEndian32(at: 16))
            }
            guard nameLength <= 1024 else { throw AdbError.protocolViolation("a name of \(nameLength) bytes") }
            let name = String(decoding: try take(nameLength), as: UTF8.self)
            // An entry the device could not stat has nothing to show.
            if failed { continue }
            entries.append(AdbDirectoryEntry(name: name, mode: mode, size: size, modified: modified))
        }
    }

    /// Reads `path`: `chunk` gets each `DATA` payload in order. A `FAIL` from
    /// the device is `AdbError.remote` with its text.
    public func receive(_ path: String, chunk: (Data) throws -> Void) throws {
        try request("RECV", path: path)
        while true {
            let header = try take(8)
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            let length = Int(header.littleEndian32(at: 4))
            switch id {
            case "DATA":
                guard length <= AdbWire.maxData else {
                    throw AdbError.protocolViolation("a DATA chunk of \(length) bytes")
                }
                try chunk(try take(length))
            case "DONE":
                return
            case "FAIL":
                throw try failure(length: length)
            default:
                throw AdbError.protocolViolation("a sync reply \(id)")
            }
        }
    }

    /// Writes a file at `path`: `next` gives its bytes in order and nil at the
    /// end, and `sent` is told the total after each batch the device took.
    /// The device creates the file with `mode` and sets its modification time
    /// to `modified`. A file of any length goes, since `DATA` frames carry no
    /// total. A failure is `AdbError.remote` with the device's text: adbd
    /// sends it after it has read to the `DONE`, or closes the stream at
    /// once. A file that failed part way stays as written, and removing it is
    /// the caller's.
    public func send(
        _ path: String, mode: UInt32, modified: UInt32, next: () throws -> Data?,
        sent: (UInt64) -> Void = { _ in }
    ) throws {
        try request("SEND", path: "\(path),\(mode)")
        var pending = Data()
        var total: UInt64 = 0
        do {
            while let data = try next() {
                var offset = data.startIndex
                while offset < data.endIndex {
                    let end = min(offset + Self.maxChunk, data.endIndex)
                    pending.append(contentsOf: "DATA".utf8)
                    pending.appendLittleEndian(UInt32(end - offset))
                    pending.append(data[offset..<end])
                    total += UInt64(end - offset)
                    offset = end
                    if pending.count >= Self.sendBatch {
                        try stream.write(pending)
                        pending.removeAll(keepingCapacity: true)
                        sent(total)
                    }
                }
            }
            pending.append(contentsOf: "DONE".utf8)
            pending.appendLittleEndian(modified)
            try stream.write(pending)
        } catch AdbError.streamClosed {
            // The device stopped listening; what it said first says why.
            guard let reason = try? readSendReply() else { throw AdbError.streamClosed }
            throw reason
        }
        if let reason = try readSendReply() { throw reason }
        sent(total)
    }

    /// The device's answer to a `SEND`: nil for `OKAY`, the failure otherwise.
    private func readSendReply() throws -> AdbError? {
        let header = try take(8)
        let id = String(decoding: header.prefix(4), as: UTF8.self)
        let length = Int(header.littleEndian32(at: 4))
        switch id {
        case "OKAY": return nil
        case "FAIL": return try failure(length: length)
        default: throw AdbError.protocolViolation("a sync reply \(id)")
        }
    }

    /// Ends the session politely.
    public func quit() {
        var bytes = Data("QUIT".utf8)
        bytes.appendLittleEndian(UInt32(0))
        try? stream.write(bytes)
        stream.close()
    }
}
