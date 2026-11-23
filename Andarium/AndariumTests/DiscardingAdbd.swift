// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CryptoKit
import Darwin
import Foundation

/// An adbd that takes whatever a transfer sends and keeps none of it: the far
/// end of a socket pair for a test that moves a large file and watches the
/// application's memory, not the guest's. It speaks the wire format on a
/// thread of its own, one connection for each `connect`, and serves the three
/// services a transfer uses: `abb_exec` (the install stream: it counts the
/// bytes, then prints `installOutput`), `sync:` (`SEND`, `STA2`, `QUIT`; the
/// file's length is all it remembers) and `shell,v2,raw:` (`df` answers with
/// `guestFreeKiB`, `getprop ro.product.cpu.abilist` with `abiList`, anything
/// else exits 0). It also keeps the package manager's install sessions:
/// `install-create` makes one, `install-write` takes a file into it (name,
/// length and SHA-256 are what it remembers), `install-commit` and
/// `install-abandon` end it. Every message it handles is inside
/// an autorelease pool of its own, so the memory it uses is never the memory
/// the test measures.
nonisolated final class DiscardingAdbd: AdbConnector, @unchecked Sendable {
    private let lock = NSLock()
    private var transports: [DescriptorTransport] = []
    private var opened: [String] = []
    private var commands: [String] = []
    private var installedBytes: UInt64 = 0
    private var digests: [String] = []
    private var outputs: [String] = []
    private var sentBytes: UInt64 = 0
    private var connections = 0
    private var sessions: [String: FakeSession] = [:]
    private var nextSession = 1000
    private var committed: [CommittedSession] = []
    private var abandoned: [String] = []
    private var createAnswers: [String] = []
    private var commitAnswers: [String] = []
    private var pieceHook: (@Sendable (_ written: UInt64) -> Void)?
    private var syncRecords: [SyncRecord] = []
    private var syncSeen: UInt64 = 0
    private var syncHook: (@Sendable (_ seen: UInt64) -> Void)?
    private var syncError: String?
    private var hashing = false
    private var shellScript: [String: ShellAnswer] = [:]

    /// What a shell command prints and exits with.
    struct ShellAnswer: Equatable {
        var text: String
        var status: UInt8
    }

    /// A file a `SEND` wrote: where, how long, and (with `hashesSyncData`)
    /// the SHA-256 of what it carried.
    struct SyncRecord: Equatable {
        var path: String
        var bytes: UInt64
        var digest: String
    }

    /// A file written into a session.
    struct SessionPart: Equatable {
        var name: String
        var bytes: UInt64
        var digest: String
    }

    /// A session that was committed, as it stood at the commit.
    struct CommittedSession: Equatable {
        var id: String
        var createService: String
        var parts: [SessionPart]
    }

    private struct FakeSession {
        var createService: String
        var parts: [SessionPart] = []
    }

    /// What `install-create` prints when it makes a session; the id is the
    /// next number of the fake package manager's.
    var createOutputs: [String] {
        get { lock.withLock { createAnswers } }
        set { lock.withLock { createAnswers = newValue } }
    }
    /// What the next commits print, one each, before `Success` is the answer
    /// again. A commit ends its session whatever it prints.
    var commitOutputs: [String] {
        get { lock.withLock { commitAnswers } }
        set { lock.withLock { commitAnswers = newValue } }
    }
    /// What `getprop ro.product.cpu.abilist` prints.
    var abiList = "arm64-v8a,armeabi-v7a,armeabi"
    /// Told, on the thread that serves the connection, the running total of
    /// every byte an `install-write` has taken, after each piece it reads.
    var onWritten: (@Sendable (_ written: UInt64) -> Void)? {
        get { lock.withLock { pieceHook } }
        set { lock.withLock { pieceHook = newValue } }
    }
    /// Whether `SEND` keeps a digest of the bytes it carries (otherwise it
    /// only counts them, as a large import test needs).
    var hashesSyncData: Bool {
        get { lock.withLock { hashing } }
        set { lock.withLock { hashing = newValue } }
    }
    /// Answers every `SEND` with this failure instead of `OKAY`.
    var syncFailure: String? {
        get { lock.withLock { syncError } }
        set { lock.withLock { syncError = newValue } }
    }
    /// Told, on the thread that serves the connection, the running total of
    /// the bytes of `SEND` data, after each frame.
    var onSynced: (@Sendable (_ seen: UInt64) -> Void)? {
        get { lock.withLock { syncHook } }
        set { lock.withLock { syncHook = newValue } }
    }
    /// The files `SEND` wrote, in order.
    var syncedFiles: [SyncRecord] { lock.withLock { syncRecords } }
    /// The ids of the sessions made and neither committed nor abandoned.
    var openSessions: [String] { lock.withLock { sessions.keys.sorted() } }
    /// The sessions that were committed, in order.
    var committedSessions: [CommittedSession] { lock.withLock { committed } }
    /// The ids `install-abandon` removed, in order.
    var abandonedSessions: [String] { lock.withLock { abandoned } }
    /// The files of a session still open.
    func parts(ofSession id: String) -> [SessionPart]? { lock.withLock { sessions[id]?.parts } }

    /// What the install stream prints when its bytes have all arrived.
    var installOutput = "Success\n"
    /// What the next installs print, one each, before `installOutput` is the
    /// answer again: a refusal first and a success after it, say.
    var installOutputs: [String] {
        get { lock.withLock { outputs } }
        set { lock.withLock { outputs = newValue } }
    }
    /// Prints `installOutput` and ends the install stream after its first
    /// write instead of reading the whole file, as `createSession` failing
    /// does.
    var failsAtOnce = false
    /// The peer does not acknowledge the writes of an `install-write`: it answers and closes, as a package manager that is done with the file may.
    var closeSuccessfulWriteWithoutFinalAck = false
    /// The API level `getprop ro.build.version.sdk` answers.
    var sdkLevel = "37"
    /// The free space `df` reports for any path, in kibibytes.
    var guestFreeKiB: UInt64 = 64 << 20

    /// Every service a connection opened, in order.
    var servicesOpened: [String] { lock.withLock { opened } }
    /// The shell commands run.
    var shellCommands: [String] { lock.withLock { commands } }
    /// What the shell prints and exits with for a command, by its words; a
    /// command that is not here keeps the answers above (nothing, status 0).
    var shellAnswers: [String: ShellAnswer] {
        get { lock.withLock { shellScript } }
        set { lock.withLock { shellScript = newValue } }
    }
    /// The bytes the install stream took, and the bytes `SEND` frames carried.
    var installBytes: UInt64 { lock.withLock { installedBytes } }
    /// The SHA-256 of what each install stream took, in order (hex).
    var installDigests: [String] { lock.withLock { digests } }
    var syncBytes: UInt64 { lock.withLock { sentBytes } }
    /// How many times a client connected.
    var connectionCount: Int { lock.withLock { connections } }

    func connect() throws -> any AdbTransport {
        var pair: [Int32] = [0, 0]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { throw AdbError.io(errno) }
        let client = DescriptorTransport(descriptor: pair[0])
        let server = DescriptorTransport(descriptor: pair[1])
        lock.withLock {
            connections += 1
            transports += [client, server]
        }
        let thread = Thread { [self] in
            try? serve(server)
            server.shutdown()
        }
        thread.name = "discarding-adbd"
        thread.start()
        return client
    }

    func shutdown() {
        let all = lock.withLock { transports }
        for transport in all { transport.shutdown() }
    }

    // MARK: Wire

    private func receive(_ transport: DescriptorTransport) throws -> AdbMessage {
        let header = try transport.readExactly(AdbWire.headerSize)
        let length = Int(Self.le32(header, 12))
        let payload = length > 0 ? try transport.readExactly(length) : Data()
        return AdbMessage(
            command: Self.le32(header, 0), arg0: Self.le32(header, 4), arg1: Self.le32(header, 8), payload: payload)
    }

    private func send(_ transport: DescriptorTransport, _ message: AdbMessage) throws {
        try transport.writeAll(message.encoded())
    }

    private static func le32(_ data: Data, _ offset: Int) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { $0 | UInt32(data[data.startIndex + offset + $1]) << (8 * UInt32($1)) }
    }

    private static func bytes<T: FixedWidthInteger>(_ value: T) -> Data {
        withUnsafeBytes(of: value.littleEndian) { Data($0) }
    }

    private func serve(_ transport: DescriptorTransport) throws {
        let connect = try receive(transport)
        guard connect.command == AdbWire.cnxn else { return }
        try send(transport, AdbMessage(
            command: AdbWire.cnxn, arg0: AdbWire.version, arg1: UInt32(AdbWire.maxData),
            payload: Data("device::ro.product.name=fake;features=shell_v2,cmd,stat_v2,abb_exec\0".utf8)))
        var nextRemote: UInt32 = 100
        while true {
            let message = try autoreleasepool { try receive(transport) }
            guard message.command == AdbWire.open else { continue }
            var name = message.payload
            if name.last == 0 { name.removeLast() }
            let service = String(decoding: name, as: UTF8.self)
            lock.withLock { opened.append(service) }
            let stream = Channel(adbd: self, transport: transport, ownId: nextRemote, peerId: message.arg0)
            nextRemote += 1
            try send(transport, AdbMessage(command: AdbWire.okay, arg0: stream.ownId, arg1: stream.peerId))
            if service.hasPrefix("abb_exec:") {
                try serveAbb(stream, service: service)
            } else if service == "sync:" {
                try serveSync(stream)
            } else if service.hasPrefix("shell,v2,raw:") {
                try serveShell(stream, command: String(service.dropFirst("shell,v2,raw:".count)))
            }
            try stream.close()
        }
    }

    // MARK: Services

    /// An `abb_exec` command: a session verb or the single-file `install`.
    private func serveAbb(_ stream: Channel, service: String) throws {
        let words = service.split(separator: "\0", omittingEmptySubsequences: false).map(String.init)
        switch words.count > 1 ? words[1] : "" {
        case "install-create":
            let output = lock.withLock { () -> String in
                if !createAnswers.isEmpty { return createAnswers.removeFirst() }
                let id = String(nextSession)
                nextSession += 1
                sessions[id] = FakeSession(createService: service)
                return "Success: created install session [\(id)]\n"
            }
            try stream.send(Data(output.utf8))
        case "install-write":
            try serveSessionWrite(stream, words: words)
        case "install-commit":
            let id = words.count > 2 ? words[2] : ""
            let output = lock.withLock { () -> String in
                guard let session = sessions.removeValue(forKey: id) else { return Self.noAccess(verb: "install-commit") }
                committed.append(CommittedSession(id: id, createService: session.createService, parts: session.parts))
                return commitAnswers.isEmpty ? "Success\n" : commitAnswers.removeFirst()
            }
            try stream.send(Data(output.utf8))
        case "install-abandon":
            let id = words.count > 2 ? words[2] : ""
            let output = lock.withLock { () -> String in
                guard sessions.removeValue(forKey: id) != nil else { return Self.noAccess(verb: "install-abandon") }
                abandoned.append(id)
                return "Success\n"
            }
            try stream.send(Data(output.utf8))
        default:
            try serveInstall(stream, service: service)
        }
    }

    private static func noAccess(verb: String) -> String {
        "\nException occurred while executing '\(verb)':\njava.lang.SecurityException: Caller has no access to session\n"
    }

    /// `install-write -S <bytes> <session> <name> -`: takes the bytes, then
    /// answers; the file is part of the session only when all of it arrived.
    private func serveSessionWrite(_ stream: Channel, words: [String]) throws {
        guard words.count >= 7, let size = UInt64(words[3]) else { return }
        let id = words[4]
        let name = words[5]
        var received: UInt64 = 0
        var hash = SHA256()
        while received < size {
            guard let piece = try autoreleasepool(invoking: { try stream.receive(acknowledge: !closeSuccessfulWriteWithoutFinalAck) }) else { break }
            received += UInt64(piece.count)
            hash.update(data: piece)
            let hook = lock.withLock { () -> (@Sendable (UInt64) -> Void)? in
                installedBytes += UInt64(piece.count)
                return pieceHook
            }
            hook?(lock.withLock { installedBytes })
        }
        let digest = hash.finalize().map { String(format: "%02x", $0) }.joined()
        let output = lock.withLock { () -> String in
            guard received == size, sessions[id] != nil else {
                return sessions[id] == nil ? Self.noAccess(verb: "install-write") : "Failure [INSTALL_FAILED_INVALID_APK: short write]\n"
            }
            sessions[id]?.parts.append(SessionPart(name: name, bytes: size, digest: digest))
            return "Success: streamed \(size) bytes\n"
        }
        try stream.send(Data(output.utf8))
    }

    private func serveInstall(_ stream: Channel, service: String) throws {
        let size = service.split(separator: "\0", omittingEmptySubsequences: false).last.flatMap { UInt64($0) } ?? 0
        var received: UInt64 = 0
        var hash = SHA256()
        if failsAtOnce {
            // The first write is read and answered; then the exception.
            if let first = try autoreleasepool(invoking: { try stream.receive() }) {
                received += UInt64(first.count)
                hash.update(data: first)
            }
        } else {
            while received < size {
                guard let piece = try autoreleasepool(invoking: { try stream.receive() }) else { break }
                received += UInt64(piece.count)
                hash.update(data: piece)
            }
        }
        let output = lock.withLock {
            installedBytes += received
            digests.append(hash.finalize().map { String(format: "%02x", $0) }.joined())
            return outputs.isEmpty ? installOutput : outputs.removeFirst()
        }
        try stream.send(Data(output.utf8))
    }

    private func serveShell(_ stream: Channel, command: String) throws {
        lock.withLock { commands.append(command) }
        var out = Data()
        var status: UInt8 = 0
        if let answer = lock.withLock({ shellScript[command] }) {
            if !answer.text.isEmpty { out = Data([1]) + Self.bytes(UInt32(answer.text.utf8.count)) + Data(answer.text.utf8) }
            status = answer.status
        } else if command == "getprop ro.product.cpu.abilist" || command == "getprop ro.build.version.sdk" {
            let text = (command.hasSuffix("abilist") ? abiList : sdkLevel) + "\n"
            out = Data([1]) + Self.bytes(UInt32(text.utf8.count)) + Data(text.utf8)
        } else if command.hasPrefix("df -Pk") {
            let text = "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
                + "/dev/block/dm-7 67108864 1048576 \(guestFreeKiB) 2% /data\n"
            out = Data([1]) + Self.bytes(UInt32(text.utf8.count)) + Data(text.utf8)
        }
        let exit = Data([3]) + Self.bytes(UInt32(1)) + Data([status])
        try stream.send(out + exit)
    }

    private func serveSync(_ stream: Channel) throws {
        var lengths: [String: UInt64] = [:]
        while true {
            guard let header = try stream.take(8) else { return }
            let id = String(decoding: header.prefix(4), as: UTF8.self)
            let length = Int(Self.le32(header, 4))
            switch id {
            case "QUIT":
                return
            case "SEND":
                guard let request = try stream.take(length) else { return }
                let text = String(decoding: request, as: UTF8.self)
                let path = String(text[..<(text.lastIndex(of: ",") ?? text.endIndex)])
                var total: UInt64 = 0
                var hash = SHA256()
                let hashes = lock.withLock { hashing }
                frames: while true {
                    guard let frame = try stream.take(8) else { return }
                    let frameLength = Int(Self.le32(frame, 4))
                    switch String(decoding: frame.prefix(4), as: UTF8.self) {
                    case "DATA":
                        if hashes {
                            guard let data = try autoreleasepool(invoking: { try stream.take(frameLength) }) else { return }
                            hash.update(data: data)
                        } else {
                            guard try stream.skip(frameLength) else { return }
                        }
                        total += UInt64(frameLength)
                        let hook = lock.withLock { () -> (@Sendable (UInt64) -> Void)? in
                            syncSeen += UInt64(frameLength)
                            return syncHook
                        }
                        hook?(lock.withLock { syncSeen })
                    case "DONE":
                        break frames
                    default:
                        return
                    }
                }
                lengths[path] = total
                let failure = lock.withLock { () -> String? in
                    sentBytes += total
                    if syncError == nil {
                        syncRecords.append(SyncRecord(
                            path: path, bytes: total, digest: hashes ? hash.finalize().map { String(format: "%02x", $0) }.joined() : ""))
                    }
                    return syncError
                }
                if let failure {
                    try stream.send(Data("FAIL".utf8) + Self.bytes(UInt32(failure.utf8.count)) + Data(failure.utf8))
                } else {
                    try stream.send(Data("OKAY".utf8) + Self.bytes(UInt32(0)))
                }
            case "STA2":
                guard let request = try stream.take(length) else { return }
                let path = String(decoding: request, as: UTF8.self)
                let size = lengths[path]
                var reply = Data("STA2".utf8)
                reply += Self.bytes(UInt32(size == nil ? 2 : 0))
                reply += Self.bytes(UInt64(0)) + Self.bytes(UInt64(0))
                reply += Self.bytes(UInt32(size == nil ? 0 : 0o100664)) + Self.bytes(UInt32(1))
                reply += Self.bytes(UInt32(2000)) + Self.bytes(UInt32(2000))
                reply += Self.bytes(size ?? 0)
                reply += Self.bytes(UInt64(0)) + Self.bytes(UInt64(0)) + Self.bytes(UInt64(0))
                try stream.send(reply)
            default:
                try stream.send(Data("FAIL".utf8) + Self.bytes(UInt32(0)))
            }
        }
    }

    /// One stream of a connection, as a service sees it.
    private nonisolated final class Channel {
        unowned let adbd: DiscardingAdbd
        let transport: DescriptorTransport
        let ownId: UInt32
        let peerId: UInt32
        private var closed = false
        private var payload = Data()
        private var offset = 0

        init(adbd: DiscardingAdbd, transport: DescriptorTransport, ownId: UInt32, peerId: UInt32) {
            self.adbd = adbd
            self.transport = transport
            self.ownId = ownId
            self.peerId = peerId
        }

        /// The next write of the client, acknowledged; nil when it closed.
        func receive(acknowledge: Bool = true) throws -> Data? {
            while !closed {
                let message = try adbd.receive(transport)
                if message.command == AdbWire.write {
                    if acknowledge { try adbd.send(transport, AdbMessage(command: AdbWire.okay, arg0: ownId, arg1: peerId)) }
                    return message.payload
                }
                if message.command == AdbWire.close { closed = true }
            }
            return nil
        }

        /// Exactly `count` bytes of what the client wrote, or nil when it
        /// closed first.
        func take(_ count: Int) throws -> Data? {
            var taken = Data()
            while taken.count < count {
                if offset >= payload.count {
                    guard let next = try receive() else { return nil }
                    payload = next
                    offset = 0
                }
                let end = min(payload.count, offset + count - taken.count)
                taken.append(payload[(payload.startIndex + offset)..<(payload.startIndex + end)])
                offset = end
            }
            return taken
        }

        /// Drops `count` bytes of what the client wrote; false when it closed
        /// first.
        func skip(_ count: Int) throws -> Bool {
            var left = count
            while left > 0 {
                if offset >= payload.count {
                    guard let next = try autoreleasepool(invoking: { try receive() }) else { return false }
                    payload = next
                    offset = 0
                }
                let step = min(left, payload.count - offset)
                offset += step
                left -= step
            }
            return true
        }

        /// One write to the client, waiting for its acknowledgement. What the
        /// client writes meanwhile is acknowledged and dropped.
        func send(_ data: Data) throws {
            guard !closed else { return }
            try adbd.send(transport, AdbMessage(command: AdbWire.write, arg0: ownId, arg1: peerId, payload: data))
            while true {
                let message = try adbd.receive(transport)
                if message.command == AdbWire.okay { return }
                if message.command == AdbWire.write {
                    try adbd.send(transport, AdbMessage(command: AdbWire.okay, arg0: ownId, arg1: peerId))
                }
                if message.command == AdbWire.close {
                    closed = true
                    return
                }
            }
        }

        func close() throws {
            guard !closed else { return }
            closed = true
            try adbd.send(transport, AdbMessage(command: AdbWire.close, arg0: ownId, arg1: peerId))
        }
    }
}
