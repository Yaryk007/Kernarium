// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// The private thermal connection to the guest's HAL on vsock 5559
/// (`ThermalProtocol`): the host writes one record when it connects and one at
/// each change of the severity, and the guest sends nothing. It writes without
/// blocking, and a record that cannot be written whole closes the connection,
/// because the next connection starts with the latest severity. It holds no
/// timer while connected and keeps one value, the latest severity. A stop
/// closes the connection and a connection completing after a stop cannot
/// revive it. The opener must bound its connect wait (the app uses 250 ms). A
/// connection that fails or ends is tried again after `retryDelay`.
public final class ThermalStream: @unchecked Sendable {
    public enum Event: Sendable {
        /// A connection was made and the latest severity written to it.
        case connected
        /// A connection attempt failed (the guest's service is not there yet,
        /// or at all).
        case attemptFailed
        /// The connection ended without a stop.
        case lost
    }

    private let opener: any GuestStreamOpener
    private let retryDelay: DispatchTimeInterval
    private let events: @Sendable (Event) -> Void
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "andarium-thermal-stream")
    private var running = false
    private var generation: UInt64 = 0
    private var descriptor: Int32 = -1
    private var reader: DispatchSourceRead?
    private var latest = AndroidThermalSeverity.none

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
            _ = closeStream()
        }
    }

    /// Makes `severity` the one the guest is told: now, if connected, and at
    /// every connection after.
    public func send(_ severity: AndroidThermalSeverity) {
        let closed = lock.withLock { () -> UInt64? in
            latest = severity
            guard running, descriptor >= 0 else { return nil }
            return writeLatest() ? nil : (closeStream() ? generation : nil)
        }
        if let closed { lost(epoch: closed) }
    }

    /// Writes the latest severity; called with the lock held, with a
    /// connection. Answers whether the record was written whole.
    private func writeLatest() -> Bool {
        let record = ThermalProtocol.record(latest)
        while true {
            let sent = record.withUnsafeBytes { Darwin.send(descriptor, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            if sent == record.count { return true }
            if sent < 0, errno == EINTR { continue }
            return false
        }
    }

    // Called only with the lock held. The descriptor is closed by the reader's
    // cancel handler, after it can no longer be sent on, so a stop cannot
    // close a descriptor number that has been reused. Answers whether a
    // connection was closed.
    private func closeStream() -> Bool {
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
            // stream socket on Darwin: a guest that does not read would hold the
            // caller with the lock. The descriptor is this stream's own, so it is
            // made non-blocking (every wait is on readiness, and the reads and
            // writes here already take `EAGAIN`).
            var one: Int32 = 1
            guard setsockopt(opened, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout.size(ofValue: one))) == 0,
                fcntl(opened, F_SETFL, fcntl(opened, F_GETFL) | O_NONBLOCK) == 0
            else {
                Darwin.close(opened)
                return false
            }
            descriptor = opened
            guard writeLatest() else {
                _ = closeStream()
                return false
            }
            // The guest sends nothing; the source is there to see the end of
            // the connection, and discards what arrives.
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

    private func read(from descriptor: Int32, epoch: UInt64) {
        var buffer = [UInt8](repeating: 0, count: 64)
        let count = recv(descriptor, &buffer, buffer.count, MSG_DONTWAIT)
        if count < 0 && (errno == EAGAIN || errno == EINTR) { return }
        guard count <= 0 else { return }
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, self.descriptor == descriptor else { return false }
            return closeStream()
        }
        if dropped { lost(epoch: epoch) }
    }
}
