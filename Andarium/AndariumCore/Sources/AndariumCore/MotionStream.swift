// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Private host-to-guest motion protocol on vsock 5556. Each 20-byte record is
/// little endian: "ANS1", Android sensor type (1, 2 or 4) as a uint16, a status
/// byte (0 for every type but 2, which carries 0 to 3), a zero byte, and three
/// IEEE float32 values in SI units (microtesla for type 2). The guest
/// timestamps receipt in its own CLOCK_BOOTTIME domain. The guest answers on
/// the same connection with `MotionDemand` frames, which say which sensors an
/// application uses. A guest that has the device-orientation sensor also takes
/// records of type 27 (`OrientationRecord`), and one that lists a magnetometer
/// (it sends "ANC3") takes records of type 2.
public struct MotionSample: Sendable {
    /// The raw value is Android's sensor type.
    public enum Sensor: UInt32, Sendable { case accelerometer = 1, magnetometer = 2, gyroscope = 4 }
    /// The host interface orientation the virtual device is mounted to
    /// (`MotionMount` decides when it changes).
    public enum Axes: Equatable, Sendable { case portrait, portraitUpsideDown, landscapeLeft, landscapeRight }
    public let sensor: Sensor
    public let x: Float
    public let y: Float
    public let z: Float
    /// How far the magnetic field can be trusted, as Android's SensorStatus: 0
    /// unreliable, 1 low, 2 medium, 3 high. Zero for every other sensor.
    public let status: UInt8

    /// `x`, `y` and `z` are in the device's frame and the sensor's unit (g,
    /// rad/s, microtesla), and are given the axes of the mount. The magnetic
    /// field has no sign convention of its own, so it keeps its sign like the
    /// rotation rate, and the same rotation about z the accelerometer takes
    /// turns it, so the guest's gravity and field stay in one frame.
    public init(sensor: Sensor, x: Double, y: Double, z: Double, axes: Axes = .portrait, status: UInt8 = 0) {
        self.sensor = sensor
        // Core Motion reports gravity in g with the opposite acceleration
        // sign to Android. Both use right-handed, fixed device axes.
        let scale = sensor == .accelerometer ? -9.80665 : 1.0
        let (horizontal, vertical): (Double, Double) = switch axes {
        case .portrait: (x, y)
        case .portraitUpsideDown: (-x, -y)
        case .landscapeLeft: (y, -x)
        case .landscapeRight: (-y, x)
        }
        self.x = Float(horizontal * scale)
        self.y = Float(vertical * scale)
        self.z = Float(z * scale)
        self.status = sensor == .magnetometer ? min(status, 3) : 0
    }

    private init(sensor: Sensor, x: Float, y: Float, z: Float, status: UInt8) {
        self.sensor = sensor
        self.x = x
        self.y = y
        self.z = z
        self.status = status
    }

    /// The same reading, with another status.
    public func withStatus(_ status: UInt8) -> MotionSample {
        MotionSample(sensor: sensor, x: x, y: y, z: z, status: sensor == .magnetometer ? min(status, 3) : 0)
    }

    /// The reading can be sent: its components are finite.
    public var isUsable: Bool { x.isFinite && y.isFinite && z.isFinite }

    public var encoded: Data? {
        guard isUsable else { return nil }
        var data = Data([0x41, 0x4e, 0x53, 0x31])
        for word in [sensor.rawValue | UInt32(status) << 16, x.bitPattern, y.bitPattern, z.bitPattern] {
            var little = word.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        return data
    }
}

/// The private motion connection to the guest on vsock 5556: the host sends
/// samples and reads the guest's `MotionDemand` frames. It sends each sample
/// as it arrives, with bounded memory and no blocking writes (a write that
/// would block closes the connection, so a stalled or paused guest never has
/// old motion queued behind it), and holds no timer while connected. A stop
/// closes the connection and a connection completing after a stop cannot
/// revive it. The opener must bound its connect wait (the app uses 250 ms). A
/// connection that fails or drops is tried again after `retryDelay`.
///
/// A magnetic-field record is sent only while the guest's latest frame on the
/// connection in use says the field is wanted (it has bit 3 of an "ANC3"
/// frame). That is decided where the frames are read and the record is sent,
/// under one lock, so no reading captured earlier reaches a guest that has
/// since said otherwise, or a connection to another service: an older guest
/// ends its connection on a type-2 record.
public final class MotionStream: @unchecked Sendable {
    public static let port: UInt32 = 5556
    private let opener: any GuestStreamOpener
    private let connectionChanged: @Sendable (Bool) -> Void
    private let demandChanged: @Sendable (MotionDemand) -> Void
    private let retryDelay: DispatchTimeInterval
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "andarium-motion-stream")
    private var running = false
    private var generation: UInt64 = 0
    private var descriptor: Int32 = -1
    private var reader: DispatchSourceRead?
    /// The start of a frame that has not all arrived.
    private var partial = Data()
    /// The guest's latest frame on this connection asks for the magnetic field.
    private var magneticWanted = false

    public init(
        opener: any GuestStreamOpener,
        retryDelay: DispatchTimeInterval = .seconds(1),
        connectionChanged: @escaping @Sendable (Bool) -> Void = { _ in },
        demandChanged: @escaping @Sendable (MotionDemand) -> Void = { _ in }
    ) {
        self.opener = opener
        self.retryDelay = retryDelay
        self.connectionChanged = connectionChanged
        self.demandChanged = demandChanged
    }
    deinit { stop() }

    @discardableResult
    public func start() -> UInt64 {
        let epoch: UInt64? = lock.withLock {
            guard !running else { return nil }
            running = true
            generation &+= 1
            return generation
        }
        guard let epoch else { return lock.withLock { generation } }
        queue.async { [self] in connect(epoch: epoch) }
        return epoch
    }

    public func stop() {
        lock.withLock {
            running = false
            generation &+= 1
            partial.removeAll()
            _ = closeStream()
        }
    }

    /// Sends a reading. Answers whether it was sent: it is not when the stream
    /// has stopped or another run's, when there is no connection, when the
    /// reading cannot be encoded, and for a magnetic field when the guest's
    /// latest frame does not ask for one.
    @discardableResult
    public func submit(_ sample: MotionSample, epoch: UInt64) -> Bool {
        guard let packet = sample.encoded else { return false }
        return send(packet, epoch: epoch, magnetic: sample.sensor == .magnetometer)
    }

    /// Gives the guest the device's orientation. Only for a guest whose
    /// "ANC2" frame has arrived (`MotionDemand.acceptsOrientation`).
    public func submit(_ orientation: GuestOrientation, epoch: UInt64) {
        send(OrientationRecord.encoded(orientation), epoch: epoch, magnetic: false)
    }

    @discardableResult
    private func send(_ packet: Data, epoch: UInt64, magnetic: Bool) -> Bool {
        var delivered = false
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, descriptor >= 0, !magnetic || magneticWanted else { return false }
            let sent = packet.withUnsafeBytes { Darwin.send(descriptor, $0.baseAddress, $0.count, MSG_DONTWAIT) }
            guard sent != packet.count else {
                delivered = true
                return false
            }
            // Closing discards a partial record and avoids queuing old motion
            // behind a stalled or paused guest.
            return closeStream()
        }
        if dropped { lost(epoch: epoch) }
        return delivered
    }

    // Called only with the lock held. The descriptor is closed by the reader's
    // cancel handler, after it can no longer be sent on, so a stop cannot
    // close a descriptor number that has been reused. Answers whether a
    // connection was closed.
    private func closeStream() -> Bool {
        guard descriptor >= 0 else { return false }
        let closing = descriptor
        descriptor = -1
        magneticWanted = false
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
            partial.removeAll()
            magneticWanted = false
            let source = DispatchSource.makeReadSource(fileDescriptor: opened, queue: queue)
            source.setEventHandler { [weak self] in self?.readFrames(from: opened, epoch: epoch) }
            source.setCancelHandler { Darwin.close(opened) }
            reader = source
            source.resume()
            return true
        }
        guard let connected else { return }
        connectionChanged(connected)
        if !connected { retry(epoch: epoch) }
    }

    /// The connection ended without a stop: report it and connect again.
    private func lost(epoch: UInt64) {
        connectionChanged(false)
        retry(epoch: epoch)
    }

    private func retry(epoch: UInt64) {
        queue.asyncAfter(deadline: .now() + retryDelay) { [self] in connect(epoch: epoch) }
    }

    /// The guest's frames. An end of stream or a bad frame drops the
    /// connection; the next one starts from what the service says on it.
    private func readFrames(from descriptor: Int32, epoch: UInt64) {
        var buffer = [UInt8](repeating: 0, count: 64)
        let count = recv(descriptor, &buffer, buffer.count, MSG_DONTWAIT)
        if count < 0 && (errno == EAGAIN || errno == EINTR) { return }
        var frames: [MotionDemand] = []
        let dropped = lock.withLock { () -> Bool in
            guard running, generation == epoch, self.descriptor == descriptor else { return false }
            guard count > 0 else { return closeStream() }
            partial.append(contentsOf: buffer.prefix(count))
            // The magic says how long the frame is ("ANC1" and "ANC2" 16 bytes,
            // "ANC3" 20), so a frame that has not all arrived is waited for and
            // a magic no frame has ends the connection at once.
            while true {
                switch MotionDemand.framing(of: partial) {
                case .incomplete:
                    return false
                case .invalid:
                    return closeStream()
                case .frame(let length):
                    guard partial.count >= length else { return false }
                    let frame = Data(partial.prefix(length))
                    partial.removeFirst(length)
                    guard let demand = MotionDemand(frame: frame) else { return closeStream() }
                    magneticWanted = demand.magnetometer != nil
                    frames.append(demand)
                }
            }
        }
        // Frames that arrived before a bad one still count.
        for frame in frames { demandChanged(frame) }
        if dropped { lost(epoch: epoch) }
    }
}
