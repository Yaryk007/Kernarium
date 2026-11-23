// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The reply budget of an install stream: guest output that arrives while the
/// host is still sending is held for the reader, so it is counted when it is
/// accepted and not only when the sending is over.
struct AdbStreamBudgetTests {
    /// An adbd that answers an open with OKAY and every write with OKAY, and
    /// says `flood` bytes in `chunk`-byte writes of its own before the first
    /// OKAY of a write.
    private final class FloodingAdbd: AdbTransport, @unchecked Sendable {
        private let lock = NSLock()
        private var inbound = Data()
        private var outbound = Data()
        let flood: Int
        let chunk: Int
        private var flooded = false

        init(flood: Int, chunk: Int) {
            self.flood = flood
            self.chunk = chunk
        }

        func readExactly(_ count: Int) throws -> Data {
            lock.lock()
            defer { lock.unlock() }
            guard inbound.count >= count else { throw AdbError.guestGone }
            let head = Data(inbound.prefix(count))
            // A slice keeps its start index, which the readers of a message do not expect.
            inbound = Data(inbound.dropFirst(count))
            return head
        }

        func writeAll(_ data: Data) throws {
            lock.lock()
            defer { lock.unlock() }
            outbound.append(data)
            while outbound.count >= AdbWire.headerSize {
                let header = try AdbMessage.Header(Data(outbound.prefix(AdbWire.headerSize)))
                guard outbound.count >= AdbWire.headerSize + header.length else { return }
                let payload = Data(outbound.dropFirst(AdbWire.headerSize).prefix(header.length))
                outbound = Data(outbound.dropFirst(AdbWire.headerSize + header.length))
                react(to: AdbMessage(command: header.command, arg0: header.arg0, arg1: header.arg1, payload: payload))
            }
        }

        private func react(to message: AdbMessage) {
            switch message.command {
            case AdbWire.open:
                enqueue(AdbMessage(command: AdbWire.okay, arg0: 7, arg1: message.arg0))
            case AdbWire.write:
                if !flooded {
                    flooded = true
                    var left = flood
                    while left > 0 {
                        let size = min(chunk, left)
                        enqueue(AdbMessage(
                            command: AdbWire.write, arg0: 7, arg1: message.arg0, payload: Data(repeating: 0x41, count: size)))
                        left -= size
                    }
                }
                enqueue(AdbMessage(command: AdbWire.okay, arg0: 7, arg1: message.arg0))
            default:
                break
            }
        }

        private func enqueue(_ message: AdbMessage) { inbound.append(message.encoded()) }

        func shutdown() {}
    }

    /// The stream holds its connection unowned: the caller keeps both.
    private func stream(flood: Int, limit: Int?) throws -> (AdbConnection, AdbStream) {
        let connection = AdbConnection(transport: FloodingAdbd(flood: flood, chunk: 4096))
        return (connection, try connection.open("abb_exec:package", outputLimit: limit))
    }

    @Test func outputThatArrivesWhileTheHostSendsIsHeldOnlyUpToTheBudget() throws {
        let (connection, stream) = try stream(flood: 65_537, limit: 64 * 1024)
        #expect(throws: AdbError.self) { try stream.write(Data(repeating: 0x42, count: 100)) }
        withExtendedLifetime(connection) {}
    }

    @Test func aReplyWithinTheBudgetIsKeptAndReadWhole() throws {
        let (connection, stream) = try stream(flood: 64 * 1024, limit: 64 * 1024)
        defer { withExtendedLifetime(connection) {} }
        try stream.write(Data(repeating: 0x42, count: 100))
        let first = try #require(try stream.read())
        #expect(first.count == 4096)
    }

    /// The sync stream of an upload (an `.xapk` data file, an import) has
    /// the budget too: a device that talks before the file is sent ends the
    /// upload before a byte of the file is read.
    @Test func anUploadOverASyncStreamThatFloodsEndsBeforeTheFileIsRead() throws {
        let connection = AdbConnection(transport: FloodingAdbd(flood: 256 * 1024, chunk: 4096))
        let source = CountingSource()
        let path = GuestFilePath.root + "/Download/a.obb"
        #expect(throws: AdbError.self) {
            try GuestFiles.upload(
                source, modified: .now, to: path, temporary: GuestFilePath.root + "/Download/.andarium-1.part",
                replacing: true, on: connection, progress: { _ in })
        }
        #expect(source.reads == 0)
    }

    @Test func aStreamWithNoBudgetKeepsWhatAnOrdinaryServiceSays() throws {
        // The shell and sync paths bound their own readers.
        let (connection, stream) = try stream(flood: 200 * 1024, limit: nil)
        defer { withExtendedLifetime(connection) {} }
        try stream.write(Data(repeating: 0x42, count: 100))
        let first = try #require(try stream.read())
        #expect(first.count == 4096)
    }
}

/// A source of ten bytes that counts how often it was asked for them.
private final class CountingSource: AdbSource, @unchecked Sendable {
    private let lock = NSLock()
    private var count = 0
    var reads: Int { lock.withLock { count } }

    func read(limit: Int) throws -> Data? {
        lock.withLock {
            count += 1
            return count == 1 ? Data(count: 10) : nil
        }
    }
}
