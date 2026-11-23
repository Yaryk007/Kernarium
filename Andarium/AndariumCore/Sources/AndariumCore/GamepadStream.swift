// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// The private gamepad connection to the guest's relay on vsock 5558: the host
/// writes `GamepadHostFrame`s and reads the relay's `GamepadGuestFrame`s. It
/// writes without blocking, holds no timer while connected, and bounds what it
/// keeps. A stop closes the connection and a connection completing after a
/// stop cannot revive it. The opener must bound its connect wait (the app uses
/// 250 ms). A connection that fails, drops or breaks the format is tried again
/// after `retryDelay`.
///
/// **What is never dropped.** A `CREATE`, a `DESTROY` and the replies to the
/// driver reach the relay or the connection is closed: a part of a frame cannot
/// be taken back, and the relay makes every device again on the next one.
/// Anything written before them goes first.
///
/// **What is.** An input report is only a state. When the connection cannot
/// take it, the newest report of each slot is kept (at most 8, a few bytes
/// each) and written when the connection can, so a guest that is paused or slow
/// loses only the states between, and never has a queue behind it. What the
/// connection takes ahead of a guest that reads nothing is bounded here by
/// `sendBufferBytes`, a few reports of each slot; the runtime's own end of the
/// connection (and the guest's credit) holds more, which this side cannot
/// bound.
public final class GamepadStream: @unchecked Sendable {
    /// The send buffer of the connection's descriptor: a few input reports for
    /// each slot, and more than any frame the session writes whole (a `CREATE`
    /// of the profiles here is a few hundred bytes).
    static let sendBufferBytes: Int32 = 4096

    public enum Event: Sendable {
        /// A connection was made.
        case connected
        /// A connection attempt failed (the guest's relay is not there yet, or at all).
        case attemptFailed
        /// The connection ended without a stop.
        case lost
        /// A frame of the guest's relay.
        case frame(GamepadGuestFrame)
    }

    private struct PendingKey: Hashable {
        var slot: Int
        var reportID: UInt8
    }

    private enum Flush { case done, blocked, failed }

    private let opener: any GuestStreamOpener
    private let retryDelay: DispatchTimeInterval
    private let events: @Sendable (Event) -> Void
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "andarium-gamepad-stream")
    private var running = false
    private var generation: UInt64 = 0
    private var descriptor: Int32 = -1
    private var reader: DispatchSourceRead?
    private var writer: DispatchSourceWrite?
    private var frames = GamepadGuestFrameReader()
    /// The rest of a frame the connection took only a part of.
    private var partial: [UInt8] = []
    private var pending: [PendingKey: [UInt8]] = [:]
    private var order: [PendingKey] = []

    /// `events` is called on the stream's own queue, in order.
    public init(
        opener: any GuestStreamOpener, retryDelay: DispatchTimeInterval = .seconds(1),
        events: @escaping @Sendable (Event) -> Void
    ) {
        self.opener = opener
        self.retryDelay = retryDelay
        self.events = events
    }
    deinit { stop() }

    public func start() {
        let epoch: UInt64? = lock.withLock {
            guard !running else { return nil }
            running = true
            generation &+= 1
            return generation
        }
        guard let epoch else { return }
        queue.async { [self] in connect(epoch: epoch) }
    }

    public func stop() {
        lock.withLock {
            running = false
            generation &+= 1
            frames.reset()
            _ = closeStream()
        }
    }

    /// Closes the connection and connects again after the retry delay: the
    /// relay broke the protocol. Nothing happens when no connection is up.
    public func drop() {
        let closed = lock.withLock { () -> UInt64? in
            guard running else { return nil }
            return closeStream() ? generation : nil
        }
        if let closed { lost(epoch: closed) }
    }

    /// Whether anything is waiting to be written: a part of a frame or a report.
    public var hasPendingWrites: Bool { lock.withLock { !partial.isEmpty || !order.isEmpty } }

    /// Sends a frame. An input report goes without blocking, as described
    /// above; any other frame is written whole or the connection is closed,
    /// which answers false. A frame sent while no connection is up answers false.
    @discardableResult
    public func send(_ frame: GamepadHostFrame) -> Bool {
        let outcome: (sent: Bool, closed: UInt64?) = lock.withLock {
            guard running, descriptor >= 0 else { return (false, nil) }
            switch frame {
            case .input(let slot, let report):
                let closed = submitLocked(slot: slot, report: report, bytes: frame.encoded)
                return (closed == nil, closed)
            case .create, .destroy, .getReportReply, .setReportReply:
                let closed = sendControlLocked(frame)
                return (closed == nil, closed)
            }
        }
        if let closed = outcome.closed { lost(epoch: closed) }
        return outcome.sent
    }

    // MARK: Writing (the lock held)

    /// Writes what it can of `bytes`: how many it took, or -1 with the errno
    /// of the failure.
    private func write(_ bytes: [UInt8]) -> Int {
        while true {
            let sent = bytes.withUnsafeBytes { Darwin.send(descriptor, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            if sent < 0, errno == EINTR { continue }
            return sent
        }
    }

    private func wouldBlock() -> Bool { errno == EAGAIN || errno == EWOULDBLOCK }

    /// Writes the part of a frame the connection took before, then the waiting
    /// reports, until it takes no more.
    private func flushLocked() -> Flush {
        while true {
            if partial.isEmpty {
                guard let key = order.first else { return .done }
                order.removeFirst()
                guard let frame = pending.removeValue(forKey: key) else { continue }
                partial = frame
            }
            let sent = write(partial)
            if sent < 0 { return wouldBlock() ? .blocked : .failed }
            partial.removeFirst(sent)
            if !partial.isEmpty { return .blocked }
        }
    }

    /// Queues or writes an input report. Answers the epoch of a connection it
    /// closed because it could not be written at all.
    private func submitLocked(slot: Int, report: [UInt8], bytes: [UInt8]) -> UInt64? {
        let key = PendingKey(slot: slot, reportID: report.first ?? 0)
        if partial.isEmpty && order.isEmpty {
            let sent = write(bytes)
            if sent == bytes.count { return nil }
            if sent >= 0 {
                // The connection took a part of the frame: the rest goes first.
                partial = Array(bytes[sent...])
            } else if wouldBlock() {
                pending[key] = bytes
                order.append(key)
            } else {
                return closeStream() ? generation : nil
            }
        } else {
            if pending[key] == nil { order.append(key) }
            pending[key] = bytes
        }
        armWriterLocked()
        return nil
    }

    /// Writes a frame that is never dropped, after everything before it.
    private func sendControlLocked(_ frame: GamepadHostFrame) -> UInt64? {
        // The reports of a slot that is made or destroyed are of the device
        // that was; they are not written after the frame.
        switch frame {
        case .create, .destroy:
            for key in order where key.slot == frame.slot { pending[key] = nil }
            order.removeAll { $0.slot == frame.slot }
        default:
            break
        }
        if !partial.isEmpty || !order.isEmpty {
            guard flushLocked() == .done else { return closeStream() ? generation : nil }
            cancelWriterLocked()
        }
        let bytes = frame.encoded
        // A part of a frame cannot be taken back: one that is not written whole
        // ends the connection.
        guard write(bytes) == bytes.count else { return closeStream() ? generation : nil }
        return nil
    }

    private func armWriterLocked() {
        guard writer == nil, descriptor >= 0 else { return }
        let duplicate = dup(descriptor)
        guard duplicate >= 0 else { return }
        let epoch = generation
        let source = DispatchSource.makeWriteSource(fileDescriptor: duplicate, queue: queue)
        source.setEventHandler { [weak self] in self?.writable(epoch: epoch) }
        source.setCancelHandler { Darwin.close(duplicate) }
        writer = source
        source.resume()
    }

    private func cancelWriterLocked() {
        guard let writer else { return }
        self.writer = nil
        writer.cancel()
    }

    private func writable(epoch: UInt64) {
        let closed = lock.withLock { () -> UInt64? in
            guard running, generation == epoch, descriptor >= 0 else { return nil }
            switch flushLocked() {
            case .done:
                cancelWriterLocked()
                return nil
            case .blocked:
                return nil
            case .failed:
                return closeStream() ? generation : nil
            }
        }
        if let closed { lost(epoch: closed) }
    }

    // Called only with the lock held. The descriptor is closed by the reader's
    // cancel handler, after it can no longer be sent on, so a stop cannot
    // close a descriptor number that has been reused. Answers whether a
    // connection was closed.
    private func closeStream() -> Bool {
        partial.removeAll()
        pending.removeAll()
        order.removeAll()
        cancelWriterLocked()
        guard descriptor >= 0 else { return false }
        let closing = descriptor
        descriptor = -1
        if let reader {
            self.reader = nil
            reader.cancel()
        } else {
            Darwin.close(closing)
        }
        return true
    }

    // MARK: Connecting and reading

    private func connect(epoch: UInt64) {
        let current = lock.withLock { running && generation == epoch && descriptor < 0 }
        guard current else { return }
        let opened = try? opener.openStream()
        let connected = lock.withLock { () -> Bool? in
            guard running, generation == epoch else {
                if let opened { Darwin.close(opened) }
                return nil
            }
            guard let opened else { return false }
            // `MSG_DONTWAIT` alone does not keep a send from blocking on a local
            // stream socket on Darwin: a connection the guest does not read would
            // hold the caller. The descriptor is this stream's own, so it is made
            // non-blocking (every wait is on readiness).
            var one: Int32 = 1
            guard setsockopt(opened, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one))) == 0,
                fcntl(opened, F_SETFL, fcntl(opened, F_GETFL) | O_NONBLOCK) == 0
            else {
                Darwin.close(opened)
                return false
            }
            // Best effort: a larger buffer only holds older states ahead of a
            // guest that is not reading.
            var buffer = Self.sendBufferBytes
            _ = setsockopt(opened, SOL_SOCKET, SO_SNDBUF, &buffer, socklen_t(MemoryLayout.size(ofValue: buffer)))
            descriptor = opened
            frames.reset()
            let source = DispatchSource.makeReadSource(fileDescriptor: opened, queue: queue)
            source.setEventHandler { [weak self] in self?.read(from: opened, epoch: epoch) }
            source.setCancelHandler { Darwin.close(opened) }
            reader = source
            source.resume()
            return true
        }
        guard let connected else { return }
        events(connected ? .connected : .attemptFailed)
        if !connected { retry(epoch: epoch) }
    }

    /// The connection ended without a stop: report it and connect again.
    private func lost(epoch: UInt64) {
        queue.async { [self] in
            events(.lost)
            retry(epoch: epoch)
        }
    }

    private func retry(epoch: UInt64) {
        queue.asyncAfter(deadline: .now() + retryDelay) { [self] in connect(epoch: epoch) }
    }

    /// The relay's bytes. An end of stream or a frame that breaks the format
    /// drops the connection; the frames that arrived before it still count.
    private func read(from descriptor: Int32, epoch: UInt64) {
        var buffer = [UInt8](repeating: 0, count: 2 * (GamepadProtocol.headerSize + GamepadProtocol.maximumPayload))
        let count = recv(descriptor, &buffer, buffer.count, MSG_DONTWAIT)
        if count < 0 && (errno == EAGAIN || errno == EINTR) { return }
        var received: [GamepadGuestFrame] = []
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, self.descriptor == descriptor else { return false }
            guard count > 0 else { return closeStream() }
            let output = frames.feed(buffer.prefix(count))
            received = output.frames
            return output.failure != nil ? closeStream() : false
        }
        for frame in received { events(.frame(frame)) }
        if dropped { lost(epoch: epoch) }
    }
}
