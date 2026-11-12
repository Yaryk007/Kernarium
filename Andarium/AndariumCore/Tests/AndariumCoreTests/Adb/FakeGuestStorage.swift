// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// A guest's internal storage on a fake adbd: a table of paths that its sync
/// service and shell answer from, the way a real guest's adbd does (checked on
/// one): a folder lists its entries without `.` and `..`, an empty one lists
/// nothing, and so does one the shell user cannot open, which only the shell's
/// `ls` tells apart.
final class FakeGuestStorage: @unchecked Sendable {
    enum Node: Equatable {
        case file(Data, mtime: Int64)
        /// A file only its size and time exist for: listings and stats.
        case large(UInt64, mtime: Int64)
        case folder
        case link(String)
        /// A folder `opendir` refuses.
        case refused
        /// A readable folder that lists `..` and `.` and nothing else, as the
        /// FUSE view of `Android/data` does.
        case dotted
    }

    /// What the sync service was asked, in order.
    enum Request: Equatable {
        case stat(String), list(String), receive(String), send(String, mode: UInt32), quit
    }

    private let lock = NSLock()
    private var table: [String: Node]
    private var asked: [Request] = []
    private var commands: [String] = []
    private var sentBytes: [String: (data: Data, mtime: UInt32)] = [:]

    /// The reply to a `SEND`, after its `DONE`: nil for `OKAY`, else a `FAIL`.
    var sendFailure: String?
    /// Closes the stream at the first frame of a `SEND`, after a `FAIL`.
    var refuseSend: String?
    /// Keeps the last byte of a sent file back, as a full disk would.
    var truncatesSends = false
    /// What the shell answers: the exit status for a command. Without one it
    /// answers the `ls` that checks a folder, and succeeds at anything else.
    var shell: (@Sendable (String) -> (exit: Int, stderr: String))?

    init(_ nodes: [String: Node] = [:]) {
        table = nodes
    }

    var requests: [Request] { lock.withLock { asked } }
    var shellCommands: [String] { lock.withLock { commands } }

    func sent(_ path: String) -> (data: Data, mtime: UInt32)? { lock.withLock { sentBytes[path] } }

    func set(_ path: String, _ node: Node?) { lock.withLock { table[path] = node } }

    var services: [String: FakeAdbd.Service] {
        [
            "sync:": { [self] stream in try serveSync(stream) },
            "shell,v2,raw:*": { [self] stream in
                let command = String(stream.service.dropFirst("shell,v2,raw:".count))
                lock.withLock { commands.append(command) }
                let result = shell?(command) ?? answer(command)
                if !result.stderr.isEmpty {
                    try stream.send(ShellPacket(id: ShellPacket.stderr, data: Data(result.stderr.utf8)).encoded())
                }
                try stream.send(ShellPacket(id: ShellPacket.exit, data: Data([UInt8(result.exit)])).encoded())
            },
        ]
    }

    private func answer(_ command: String) -> (exit: Int, stderr: String) {
        let prefix = "ls -A -- '"
        guard command.hasPrefix(prefix), command.hasSuffix("' >/dev/null") else { return (0, "") }
        let path = String(command.dropFirst(prefix.count).dropLast("' >/dev/null".count))
        switch lock.withLock({ table[path] }) {
        case nil: return (1, "ls: \(path): No such file or directory\n")
        case .refused?: return (1, "ls: \(path): Permission denied\n")
        default: return (0, "")
        }
    }

    // MARK: Sync

    private func note(_ request: Request) { lock.withLock { asked.append(request) } }

    private func node(_ path: String) -> Node? {
        lock.withLock {
            guard var node = table[path] else { return nil }
            var hops = 0
            while case let .link(target) = node, hops < 8 {
                guard let next = table[target] else { return nil }
                node = next
                hops += 1
            }
            return node
        }
    }

    private func mode(of node: Node) -> UInt32 {
        switch node {
        case .file, .large: 0o100644
        case .folder, .refused, .dotted: 0o040755
        case .link: 0o120777
        }
    }

    private func size(of node: Node) -> UInt64 {
        if case let .file(data, _) = node { return UInt64(data.count) }
        if case let .large(size, _) = node { return size }
        return 0
    }

    private func mtime(of node: Node) -> Int64 {
        if case let .file(_, time) = node { return time }
        if case let .large(_, time) = node { return time }
        return 1_700_000_000
    }

    private func children(of folder: String) -> [(name: String, node: Node)] {
        lock.withLock {
            table.compactMap { path, node in
                guard GuestFilePath.parent(of: path) == folder, path != folder else { return nil }
                return (GuestFilePath.name(of: path), node)
            }.sorted { $0.name < $1.name }
        }
    }

    private func serveSync(_ stream: FakeStream) throws {
        while true {
            let header = try stream.receive(exactly: 8)
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            let length = Int(header.littleEndian32(at: 4))
            let payload = String(decoding: try stream.receive(exactly: length), as: UTF8.self)
            switch id {
            case "QUIT":
                note(.quit)
                return
            case "STA2":
                note(.stat(payload))
                try stream.send(statV2(node(payload)))
            case "STAT":
                note(.stat(payload))
                try stream.send(statV1(node(payload)))
            case "LIS2", "LIST":
                note(.list(payload))
                try stream.send(listing(payload, v2: id == "LIS2"))
            case "RECV":
                note(.receive(payload))
                try serveReceive(stream, payload)
            case "SEND":
                let comma = payload.lastIndex(of: ",") ?? payload.endIndex
                let mode = UInt32(payload[payload.index(after: comma)...]) ?? 0
                let path = String(payload[payload.startIndex..<comma])
                note(.send(path, mode: mode))
                try serveSend(stream, path)
            default:
                try stream.send(reply("FAIL", Data("unknown request".utf8)))
            }
        }
    }

    private func reply(_ id: String, _ payload: Data) -> Data {
        var bytes = Data(id.utf8)
        bytes.appendLittleEndian(UInt32(payload.count))
        bytes.append(payload)
        return bytes
    }

    private func statV2(_ node: Node?) -> Data {
        var bytes = Data("STA2".utf8)
        bytes.appendLittleEndian(UInt32(node == nil ? 2 : 0))
        bytes.appendLittleEndian(UInt64(0))
        bytes.appendLittleEndian(UInt64(0))
        bytes.appendLittleEndian(node.map(mode(of:)) ?? 0)
        bytes.appendLittleEndian(UInt32(1))
        bytes.appendLittleEndian(UInt32(2000))
        bytes.appendLittleEndian(UInt32(2000))
        bytes.appendLittleEndian(node.map(size(of:)) ?? 0)
        bytes.appendLittleEndian(UInt64(0))
        bytes.appendLittleEndian(UInt64(bitPattern: node.map(mtime(of:)) ?? 0))
        bytes.appendLittleEndian(UInt64(0))
        return bytes
    }

    private func statV1(_ node: Node?) -> Data {
        var bytes = Data("STAT".utf8)
        bytes.appendLittleEndian(node.map(mode(of:)) ?? 0)
        bytes.appendLittleEndian(UInt32(truncatingIfNeeded: node.map(size(of:)) ?? 0))
        bytes.appendLittleEndian(UInt32(truncatingIfNeeded: node.map(mtime(of:)) ?? 0))
        return bytes
    }

    private func dent(v2: Bool, id: String, name: String, node: Node?) -> Data {
        var bytes = Data(id.utf8)
        let mode = node.map(mode(of:)) ?? 0
        let size = node.map(size(of:)) ?? 0
        let time = node.map(mtime(of:)) ?? 0
        if v2 {
            bytes.appendLittleEndian(UInt32(0))
            bytes.appendLittleEndian(UInt64(0))
            bytes.appendLittleEndian(UInt64(0))
            bytes.appendLittleEndian(mode)
            bytes.appendLittleEndian(UInt32(1))
            bytes.appendLittleEndian(UInt32(2000))
            bytes.appendLittleEndian(UInt32(2000))
            bytes.appendLittleEndian(size)
            bytes.appendLittleEndian(UInt64(0))
            bytes.appendLittleEndian(UInt64(bitPattern: time))
            bytes.appendLittleEndian(UInt64(0))
        } else {
            bytes.appendLittleEndian(mode)
            bytes.appendLittleEndian(UInt32(truncatingIfNeeded: size))
            bytes.appendLittleEndian(UInt32(truncatingIfNeeded: time))
        }
        bytes.appendLittleEndian(UInt32(name.utf8.count))
        bytes.append(Data(name.utf8))
        return bytes
    }

    private func listing(_ folder: String, v2: Bool) -> Data {
        var bytes = Data()
        let tag = v2 ? "DNT2" : "DENT"
        let own = lock.withLock { table[folder] }
        if own == .dotted {
            bytes.append(dent(v2: v2, id: tag, name: "..", node: .folder))
            bytes.append(dent(v2: v2, id: tag, name: ".", node: .folder))
        }
        if own == .folder || own == .dotted {
            for child in children(of: folder) {
                bytes.append(dent(v2: v2, id: tag, name: child.name, node: child.node))
            }
        }
        bytes.append(dent(v2: v2, id: "DONE", name: "", node: nil))
        return bytes
    }

    private func serveReceive(_ stream: FakeStream, _ path: String) throws {
        guard case let .file(data, _)? = node(path) else {
            try stream.send(reply("FAIL", Data("open failed: No such file or directory".utf8)))
            return
        }
        var wire = Data()
        var offset = 0
        while offset < data.count {
            let end = min(offset + AdbSync.maxChunk, data.count)
            wire.append(reply("DATA", data[offset..<end]))
            offset = end
        }
        var done = Data("DONE".utf8)
        done.appendLittleEndian(UInt32(0))
        wire.append(done)
        // Messages that cut headers and payloads at odd places.
        var position = 0
        while position < wire.count {
            let end = min(position + 9_001, wire.count)
            try stream.send(wire[position..<end])
            position = end
        }
    }

    private func serveSend(_ stream: FakeStream, _ path: String) throws {
        if let refusal = refuseSend {
            try stream.send(reply("FAIL", Data(refusal.utf8)))
            try stream.close()
            return
        }
        var body = Data()
        while true {
            let header = try stream.receive(exactly: 8)
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            let value = header.littleEndian32(at: 4)
            if id == "DATA" {
                #expect(value <= UInt32(AdbSync.maxChunk))
                body.append(try stream.receive(exactly: Int(value)))
            } else {
                #expect(id == "DONE")
                if let failure = sendFailure {
                    try stream.send(reply("FAIL", Data(failure.utf8)))
                    return
                }
                if truncatesSends { body = Data(body.dropLast()) }
                lock.withLock {
                    sentBytes[path] = (body, value)
                    table[path] = .file(body, mtime: Int64(value))
                }
                try stream.send(reply("OKAY", Data()))
                return
            }
        }
    }
}

/// Gives a file's bytes out in pieces.
final class MemorySource: AdbSource, @unchecked Sendable {
    private let data: Data
    private var offset = 0
    private(set) var reads = 0

    init(_ data: Data) { self.data = data }

    func read(limit: Int) throws -> Data? {
        reads += 1
        guard offset < data.count else { return nil }
        let end = min(offset + limit, data.count)
        defer { offset = end }
        return data[offset..<end]
    }
}
