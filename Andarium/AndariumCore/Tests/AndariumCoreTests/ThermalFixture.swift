// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

@testable import AndariumCore

/// The golden frames of the thermal wire, which the guest's thermal HAL and
/// the host both test against (`tests/unit/andarium/fixtures`).
enum ThermalFixture {
    struct Frame {
        var name: String
        var bytes: [UInt8]
    }

    static func frames() throws -> [Frame] {
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { url.deleteLastPathComponent() }
        url.append(path: "tests/unit/andarium/fixtures/thermal-golden-frames.txt")
        let text = try String(contentsOf: url, encoding: .utf8)
        return text.split(separator: "\n").compactMap { line -> Frame? in
            guard !line.hasPrefix("#"), let colon = line.firstIndex(of: ":") else { return nil }
            let bytes = line[line.index(after: colon)...].split(separator: " ").map { UInt8($0, radix: 16)! }
            return Frame(name: String(line[..<colon]), bytes: bytes)
        }
    }

    static func bytes(_ name: String) throws -> [UInt8] {
        guard let frame = try frames().first(where: { $0.name == name }) else {
            throw Missing(name: name)
        }
        return frame.bytes
    }

    struct Missing: Error { var name: String }
}

/// The guest's thermal HAL on the other end of real sockets: it accepts while
/// `accepting`, keeps every connection it made, and collects the records the
/// host writes.
final class FakeThermalGuest: GuestStreamOpener, @unchecked Sendable {
    private let lock = NSLock()
    private var attemptCount = 0
    private var peerList: [Int32] = []
    private var acceptingNow = true

    var accepting: Bool {
        get { lock.withLock { acceptingNow } }
        set { lock.withLock { acceptingNow = newValue } }
    }
    var attempts: Int { lock.withLock { attemptCount } }
    var connections: Int { lock.withLock { peerList.count } }
    var latest: Int32? { lock.withLock { peerList.last.flatMap { $0 >= 0 ? $0 : nil } } }

    deinit { for peer in peerList where peer >= 0 { close(peer) } }

    /// The guest's end of the latest connection goes away.
    func dropLatest() {
        lock.withLock {
            guard let peer = peerList.last, peer >= 0 else { return }
            peerList[peerList.count - 1] = -1
            close(peer)
        }
    }

    func openStream() throws -> Int32 {
        try lock.withLock {
            attemptCount += 1
            guard acceptingNow else { throw POSIXError(.ECONNREFUSED) }
            var sockets: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
            peerList.append(sockets[1])
            return sockets[0]
        }
    }

    /// The next `count` bytes of the latest connection, waiting for at most
    /// `timeout` seconds.
    func read(count: Int, timeout: TimeInterval = 3) -> [UInt8] {
        let deadline = Date().addingTimeInterval(timeout)
        while latest == nil, Date() < deadline { usleep(2000) }
        guard let peer = latest else { return [] }
        var result: [UInt8] = []
        while result.count < count, Date() < deadline {
            var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, 10) > 0 else { continue }
            var bytes = [UInt8](repeating: 0, count: count - result.count)
            let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
            guard size > 0 else { break }
            result.append(contentsOf: bytes.prefix(size))
        }
        return result
    }

    /// The next record of the latest connection.
    func record(timeout: TimeInterval = 3) -> [UInt8] {
        read(count: ThermalProtocol.recordSize, timeout: timeout)
    }

    /// Whether the latest connection has bytes to read now.
    func hasPending(waiting: TimeInterval = 0.1) -> Bool {
        guard let peer = latest else { return false }
        var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
        return poll(&descriptor, 1, Int32(waiting * 1000)) > 0
    }
}

/// A clock a test moves by hand, in uptime nanoseconds.
final class ThermalTestClock: @unchecked Sendable {
    private let lock = NSLock()
    private var nanoseconds: UInt64 = 5_000_000_000
    func read() -> UInt64 { lock.withLock { nanoseconds } }
    func advance(seconds: UInt64) { lock.withLock { nanoseconds += seconds * 1_000_000_000 } }
}

/// Polls for `condition` for at most `timeout` seconds.
func thermalEventually(_ timeout: TimeInterval = 3, _ condition: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if condition() { return true }
        usleep(2000)
    }
    return condition()
}
