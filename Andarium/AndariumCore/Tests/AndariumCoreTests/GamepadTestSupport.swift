// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation
import Testing
@testable import AndariumCore
import AndariumCoreTestSupport

/// The guest's end of the gamepad connection, for the stream and session tests:
/// it accepts while `accepting`, keeps the peer of every connection, and
/// speaks and reads frames as the relay does. Nothing here touches a real
/// device or a real guest.
final class GamepadFakeGuest: GuestStreamOpener, @unchecked Sendable {
    private let lock = NSLock()
    private var attemptCount = 0
    private var peerList: [Int32] = []
    private var acceptingNow = true
    private var sendBuffer: Int32?
    private var hostReader = GamepadHostFrameReader()
    private var readerPeer: Int32 = -1

    var accepting: Bool {
        get { lock.withLock { acceptingNow } }
        set { lock.withLock { acceptingNow = newValue } }
    }
    var attempts: Int { lock.withLock { attemptCount } }
    var connections: Int { lock.withLock { peerList.count } }
    var latest: Int32? { lock.withLock { peerList.last.flatMap { $0 >= 0 ? $0 : nil } } }

    /// A small send buffer for the host's end of every later connection, so a
    /// test can make the connection full without sending megabytes.
    func limitHostSendBuffer(to bytes: Int32) { lock.withLock { sendBuffer = bytes } }

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
            if var size = sendBuffer {
                setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, socklen_t(MemoryLayout.size(ofValue: size)))
                setsockopt(sockets[1], SOL_SOCKET, SO_RCVBUF, &size, socklen_t(MemoryLayout.size(ofValue: size)))
            }
            peerList.append(sockets[1])
            return sockets[0]
        }
    }

    // MARK: Speaking

    func send(_ bytes: [UInt8]) {
        guard let peer = latest else { return }
        _ = bytes.withUnsafeBytes { Darwin.send(peer, $0.baseAddress, $0.count, 0) }
    }

    func send(_ frame: GamepadGuestFrame) { send(frame.encoded) }

    // MARK: Reading

    /// The raw bytes of the latest connection, up to `count`, waiting at most `timeout`.
    func read(count: Int, timeout: TimeInterval = 3) -> [UInt8] {
        guard let peer = latest else { return [] }
        var result: [UInt8] = []
        let deadline = Date().addingTimeInterval(timeout)
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

    /// The host's next frame on the latest connection, or nil after `timeout`.
    func nextFrame(timeout: TimeInterval = 3) -> GamepadHostFrame? {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let queued: GamepadHostFrame? = lock.withLock {
                guard let peer = peerList.last, peer >= 0 else { return nil }
                if peer != readerPeer {
                    readerPeer = peer
                    hostReader = GamepadHostFrameReader()
                    pendingFrames.removeAll()
                }
                return pendingFrames.isEmpty ? nil : pendingFrames.removeFirst()
            }
            if let queued { return queued }
            guard let peer = latest else {
                usleep(2000)
                continue
            }
            var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, 10) > 0 else { continue }
            var bytes = [UInt8](repeating: 0, count: 8192)
            let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
            guard size > 0 else { return nil }
            lock.withLock {
                let output = hostReader.feed(bytes.prefix(size))
                pendingFrames.append(contentsOf: output.frames)
            }
        }
        return nil
    }

    private var pendingFrames: [GamepadHostFrame] = []

    /// Whether the host closed the latest connection.
    func closedByHost(timeout: TimeInterval = 3) -> Bool {
        guard let peer = latest else { return false }
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, 20) > 0 else { continue }
            var bytes = [UInt8](repeating: 0, count: 8192)
            let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
            if size == 0 { return true }
            if size < 0 && errno != EAGAIN && errno != EINTR { return true }
        }
        return false
    }
}

/// Polls for `condition` for at most `timeout` seconds.
func gamepadEventually(_ timeout: TimeInterval = 3, _ condition: () -> Bool) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if condition() { return true }
        usleep(2000)
    }
    return condition()
}

/// A clock the test moves.
final class GamepadTestClock: @unchecked Sendable {
    private let lock = NSLock()
    private var nanoseconds: UInt64 = 1_000_000_000_000
    func read() -> UInt64 { lock.withLock { nanoseconds } }
    func advance(milliseconds: UInt64) { lock.withLock { nanoseconds += milliseconds * 1_000_000 } }
}

/// The session's timer, run by the test: work waits until `fire`.
final class GamepadTestScheduler: @unchecked Sendable {
    private let lock = NSLock()
    private var waiting: [(UInt64, @Sendable () -> Void)] = []

    var schedule: @Sendable (UInt64, @escaping @Sendable () -> Void) -> Void {
        { [self] delay, work in lock.withLock { waiting.append((delay, work)) } }
    }

    var delays: [UInt64] { lock.withLock { waiting.map(\.0) } }

    /// Runs everything waiting, oldest first, and answers how many ran.
    @discardableResult
    func fire() -> Int {
        let work = lock.withLock { () -> [(UInt64, @Sendable () -> Void)] in
            defer { waiting.removeAll() }
            return waiting
        }
        for (_, item) in work { item() }
        return work.count
    }
}

/// A haptic player that records what it was asked.
final class GamepadFakePlayer: HapticPlayer, @unchecked Sendable {
    enum Call: Equatable {
        case play(HapticScore)
        case intensity(Float, HapticTarget)
        case stop(HapticTarget)
        case forget(HapticTarget)
        case stopAll
        case prepare
    }

    private let lock = NSLock()
    private var recorded: [Call] = []
    var result = HapticPlayResult.played
    /// Whether a tone is there for an intensity to change: false after
    /// something else stopped the actuator.
    var toneAlive: Bool {
        get { lock.withLock { alive } }
        set { lock.withLock { alive = newValue } }
    }
    private var alive = true

    var calls: [Call] { lock.withLock { recorded } }
    func clear() { lock.withLock { recorded.removeAll() } }
    private func record(_ call: Call) { lock.withLock { recorded.append(call) } }

    func play(_ score: HapticScore) -> HapticPlayResult {
        record(.play(score))
        return result
    }
    func setToneIntensity(_ intensity: Float, on target: HapticTarget) -> Bool {
        record(.intensity(intensity, target))
        return toneAlive
    }
    func stop(_ target: HapticTarget) { record(.stop(target)) }
    func forget(_ target: HapticTarget) { record(.forget(target)) }
    func stopAll() { record(.stopAll) }
    func prepare() { record(.prepare) }
}

/// What the session asked of a controller's light and player number.
final class GamepadFakeIndicators: GamepadIndicators, @unchecked Sendable {
    enum Call: Equatable {
        case light(controller: Int, red: UInt8, green: UInt8, blue: UInt8)
        case player(controller: Int, index: Int?)
    }

    private let lock = NSLock()
    private var recorded: [Call] = []
    var calls: [Call] { lock.withLock { recorded } }
    func clear() { lock.withLock { recorded.removeAll() } }

    func setLight(controller: Int, red: UInt8, green: UInt8, blue: UInt8) {
        lock.withLock { recorded.append(.light(controller: controller, red: red, green: green, blue: blue)) }
    }
    func setPlayerIndex(controller: Int, index: Int?) {
        lock.withLock { recorded.append(.player(controller: controller, index: index)) }
    }
}

/// A counter that is safe to bump from the session's queue.
final class GamepadCounter: @unchecked Sendable {
    private let lock = NSLock()
    private var value = 0
    var count: Int { lock.withLock { value } }
    func bump() { lock.withLock { value += 1 } }
}
