// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// The private haptics connection to the guest on vsock 5557: the host reads
/// the guest's `HapticsFrame`s and answers with acknowledgements. It writes
/// without blocking (an acknowledgement that cannot be written whole closes the
/// connection, and one that cannot be written at all is dropped, because the
/// guest never waits for it), holds no timer while connected, and bounds what
/// it keeps: one frame of 4 KiB at most. A stop closes the connection and a
/// connection completing after a stop cannot revive it. The opener must bound
/// its connect wait (the app uses 250 ms). A connection that fails, drops or
/// breaks the format is tried again after `retryDelay`.
public final class HapticsStream: @unchecked Sendable {
    public enum Event: Sendable {
        /// A connection was made.
        case connected
        /// A connection attempt failed (the guest's service is not there yet,
        /// or at all).
        case attemptFailed
        /// The connection ended without a stop.
        case lost
        /// A frame of the guest, with the clock's reading when its bytes arrived.
        case frame(HapticsFrame, receivedAt: UInt64)
    }

    private let opener: any GuestStreamOpener
    private let retryDelay: DispatchTimeInterval
    private let clock: @Sendable () -> UInt64
    private let events: @Sendable (Event) -> Void
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "andarium-haptics-stream")
    private var running = false
    private var generation: UInt64 = 0
    private var descriptor: Int32 = -1
    private var reader: DispatchSourceRead?
    private var frames = HapticsFrameReader()

    /// `events` is called on the stream's own queue, in order.
    public init(
        opener: any GuestStreamOpener, retryDelay: DispatchTimeInterval = .seconds(1),
        clock: @escaping @Sendable () -> UInt64 = HapticsStream.uptimeNanoseconds,
        events: @escaping @Sendable (Event) -> Void
    ) {
        self.opener = opener
        self.retryDelay = retryDelay
        self.clock = clock
        self.events = events
    }
    deinit { stop() }

    public static let uptimeNanoseconds: @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }

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

    /// Writes the acknowledgement if the connection can take it whole.
    public func send(_ acknowledgement: HapticsAcknowledgement) {
        let packet = acknowledgement.encoded
        let closed = lock.withLock { () -> UInt64? in
            guard running, descriptor >= 0 else { return nil }
            let sent = packet.withUnsafeBytes { Darwin.send(descriptor, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            if sent == packet.count { return nil }
            // Nothing written leaves the stream whole; a part of a frame
            // cannot be taken back.
            if sent < 0, errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR { return nil }
            return closeStream() ? generation : nil
        }
        if let closed { lost(epoch: closed) }
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

    /// The guest's bytes. An end of stream or a frame that breaks the format
    /// drops the connection; the frames that arrived before it still count.
    private func read(from descriptor: Int32, epoch: UInt64) {
        var buffer = [UInt8](repeating: 0, count: HapticsProtocol.maximumPayload)
        let count = recv(descriptor, &buffer, buffer.count, MSG_DONTWAIT)
        if count < 0 && (errno == EAGAIN || errno == EINTR) { return }
        let arrived = clock()
        var received: [HapticsFrame] = []
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, self.descriptor == descriptor else { return false }
            guard count > 0 else { return closeStream() }
            let output = frames.feed(buffer.prefix(count))
            received = output.frames
            return output.failure != nil ? closeStream() : false
        }
        for frame in received { events(.frame(frame, receivedAt: arrived)) }
        if dropped { lost(epoch: epoch) }
    }
}
