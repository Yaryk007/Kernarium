// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Opens a connection to the guest's text message service: the one place the
/// application names its vsock port (5560). A call blocks up to the timeout;
/// the descriptor it returns is a connected `SOCK_STREAM` socket the caller
/// owns. It throws a `POSIXError`: `ECONNREFUSED`, `ECONNRESET` and
/// `ETIMEDOUT` mean nothing is listening yet and the channel asks again,
/// `ENODEV` means there is no guest, and the channel stops at any other.
public protocol GuestSmsConnector: Sendable {
    func open(timeoutMilliseconds: UInt32) throws -> Int32
}

/// What the exchange asks of the system: the clock, waiting, and the
/// descriptor calls. The tests script it; `DarwinGuestSmsSystem` is the real one.
public protocol GuestSmsSystem: Sendable {
    /// A monotonic clock.
    func nowNanoseconds() -> UInt64
    func sleep(nanoseconds: UInt64)
    /// Waits until `descriptor` can be written (`forWriting`) or read, has
    /// ended or has failed, or the timeout passes. False when it passed.
    func wait(_ descriptor: Int32, forWriting: Bool, timeoutMilliseconds: Int32) -> Bool
    /// As `read(2)`: the bytes read, 0 at the end of the stream, or minus the error.
    func read(_ descriptor: Int32, into buffer: UnsafeMutableRawBufferPointer) -> Int
    /// As `write(2)`: the bytes written, or minus the error.
    func write(_ descriptor: Int32, from buffer: UnsafeRawBufferPointer) -> Int
    /// Makes a connected descriptor non-blocking and a write to it that failed
    /// an error, not SIGPIPE.
    func prepare(_ descriptor: Int32)
    func shutdown(_ descriptor: Int32)
    func close(_ descriptor: Int32)
}

/// The system, as it is.
public struct DarwinGuestSmsSystem: GuestSmsSystem {
    public init() {}

    public func nowNanoseconds() -> UInt64 { DispatchTime.now().uptimeNanoseconds }

    public func sleep(nanoseconds: UInt64) {
        var request = timespec(tv_sec: Int(nanoseconds / 1_000_000_000), tv_nsec: Int(nanoseconds % 1_000_000_000))
        var remaining = timespec()
        while nanosleep(&request, &remaining) != 0, errno == EINTR { request = remaining }
    }

    public func wait(_ descriptor: Int32, forWriting: Bool, timeoutMilliseconds: Int32) -> Bool {
        var entry = pollfd(fd: descriptor, events: Int16(forWriting ? POLLOUT : POLLIN), revents: 0)
        while true {
            let ready = poll(&entry, 1, max(0, timeoutMilliseconds))
            if ready < 0, errno == EINTR { continue }
            return ready != 0
        }
    }

    public func read(_ descriptor: Int32, into buffer: UnsafeMutableRawBufferPointer) -> Int {
        let count = Darwin.read(descriptor, buffer.baseAddress, buffer.count)
        return count >= 0 ? count : -Int(errno)
    }

    public func write(_ descriptor: Int32, from buffer: UnsafeRawBufferPointer) -> Int {
        let count = Darwin.write(descriptor, buffer.baseAddress, buffer.count)
        return count >= 0 ? count : -Int(errno)
    }

    public func prepare(_ descriptor: Int32) {
        let flags = fcntl(descriptor, F_GETFL)
        if flags >= 0 { _ = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) }
        var on: Int32 = 1
        _ = setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
    }

    public func shutdown(_ descriptor: Int32) { _ = Darwin.shutdown(descriptor, SHUT_RDWR) }

    public func close(_ descriptor: Int32) { _ = Darwin.close(descriptor) }
}

/// The exchange with the guest's text message service: one request frame to
/// vsock port 5560, one answer frame back. The service listens itself and
/// takes a request only from the host (android/lineage-24/overlay/sms,
/// `HostFrame.java`, owns the other end of the frame).
///
/// Frame, both ways: `KSMS`, the frame version (u16 little endian, 1), the
/// payload length (u16 little endian), then the payload. A request carries 1 to
/// 8192 bytes and an answer 0 to 4096; the payload's own bytes are
/// `GuestSmsRequest.payload` and `GuestSms.answer`. A zero-length answer means
/// the service took the request and refused it with no effect.
public struct GuestSmsChannel: Sendable {
    /// What is known of one request.
    public enum Outcome: Equatable, Sendable {
        /// Zero request bytes were written: it did not reach the guest.
        case notSent
        /// The whole frame was written and the service answered with nothing:
        /// the request had no record effect and no delivery effect.
        case notTaken
        /// At least one byte was written and no usable answer came: the
        /// request may or may not have been dispatched, and may have acted.
        case noAnswer
        /// The service dispatched the request and these bytes are its answer.
        case answered(Data)
    }

    public static let port: UInt32 = 5560
    public static let magic: [UInt8] = Array("KSMS".utf8)
    public static let frameVersion: UInt16 = 1
    public static let headerLength = 8
    public static let maximumRequest = 8192
    public static let maximumAnswer = 4096

    /// One send has one deadline, from the entry to `send`, and each stage
    /// ends at its own cap or at the deadline, whichever is first.
    public static let deadlineNanoseconds: UInt64 = 30_000_000_000
    public static let connectCapNanoseconds: UInt64 = 5_000_000_000
    public static let writeCapNanoseconds: UInt64 = 2_000_000_000
    /// One connect attempt, and the wait between two.
    public static let attemptNanoseconds: UInt64 = 250_000_000

    private let connector: any GuestSmsConnector
    private let system: any GuestSmsSystem

    public init(connector: any GuestSmsConnector, system: any GuestSmsSystem = DarwinGuestSmsSystem()) {
        self.connector = connector
        self.system = system
    }

    // MARK: Frames

    private static func header(payloadLength: Int) -> [UInt8] {
        magic + [UInt8(frameVersion & 0xFF), UInt8(frameVersion >> 8), UInt8(payloadLength & 0xFF), UInt8(payloadLength >> 8)]
    }

    /// The frame of a request payload; nil unless the payload has 1 to 8192 bytes.
    public static func frame(request payload: Data) -> Data? {
        guard (1...maximumRequest).contains(payload.count) else { return nil }
        return Data(header(payloadLength: payload.count)) + payload
    }

    /// The frame of an answer payload, which the service writes; nil above 4096 bytes.
    public static func frame(answer payload: Data) -> Data? {
        guard payload.count <= maximumAnswer else { return nil }
        return Data(header(payloadLength: payload.count)) + payload
    }

    /// The payload length an answer header announces; nil unless it is whole,
    /// has the magic and the frame version, and announces at most 4096 bytes.
    public static func answerLength(header bytes: [UInt8]) -> Int? {
        guard bytes.count == headerLength, Array(bytes[0..<4]) == magic,
              UInt16(bytes[4]) | UInt16(bytes[5]) << 8 == frameVersion else { return nil }
        let length = Int(bytes[6]) | Int(bytes[7]) << 8
        return length <= maximumAnswer ? length : nil
    }

    // MARK: The exchange

    /// Sends one request and waits for its answer. Cancelling the task ends
    /// the exchange at once: `notSent` while no byte was written, `noAnswer` after.
    public func send(_ payload: Data) async -> Outcome {
        let exchange = GuestSmsExchange(connector: connector, system: system)
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                Thread.detachNewThread { continuation.resume(returning: exchange.run(payload)) }
            }
        } onCancel: {
            exchange.cancel()
        }
    }
}

/// One exchange on a thread of its own. `cancel` may come from any thread: it
/// only shuts the descriptor down, which wakes a wait; the worker alone closes it.
public final class GuestSmsExchange: @unchecked Sendable {
    private let connector: any GuestSmsConnector
    private let system: any GuestSmsSystem
    private let lock = NSLock()
    private var ended = false
    /// The descriptor the worker is using; -1 before it connects and after it let go.
    private var inUse: Int32 = -1

    public init(connector: any GuestSmsConnector, system: any GuestSmsSystem) {
        self.connector = connector
        self.system = system
    }

    private var isEnded: Bool { lock.withLock { ended } }

    /// Ends the exchange from any thread. It never closes: a close does not wake
    /// a wait on that descriptor, a shutdown does, and the worker alone knows
    /// when the number is no longer its own.
    public func cancel() {
        lock.withLock {
            ended = true
            if inUse >= 0 { system.shutdown(inUse) }
        }
    }

    /// Runs the exchange to its end. Blocks.
    public func run(_ payload: Data) -> GuestSmsChannel.Outcome {
        guard let frame = GuestSmsChannel.frame(request: payload) else { return .notSent }
        let entry = system.nowNanoseconds()
        let deadline = entry + GuestSmsChannel.deadlineNanoseconds
        guard let descriptor = connect(entry: entry, deadline: deadline) else { return .notSent }
        defer {
            lock.withLock { inUse = -1 }
            system.close(descriptor)
        }
        system.prepare(descriptor)
        var written = 0
        switch write(frame, to: descriptor, deadline: deadline, written: &written) {
        case .failed: return written == 0 ? .notSent : .noAnswer
        case .done: break
        }
        guard let header = read(count: GuestSmsChannel.headerLength, from: descriptor, deadline: deadline),
              let length = GuestSmsChannel.answerLength(header: header) else { return .noAnswer }
        if length == 0 { return .notTaken }
        guard let body = read(count: length, from: descriptor, deadline: deadline) else { return .noAnswer }
        return .answered(Data(body))
    }

    /// Attempts of at most 250 ms until one connects, the stage's 5 s or the
    /// deadline end, or the error says to stop. Connecting only repeats the
    /// establishing of a connection: the frame is written on at most one.
    private func connect(entry: UInt64, deadline: UInt64) -> Int32? {
        let end = min(entry + GuestSmsChannel.connectCapNanoseconds, deadline)
        while true {
            if isEnded { return nil }
            let now = system.nowNanoseconds()
            guard now < end else { return nil }
            let left = end - now
            let attempt = UInt32(max(1, min(left, GuestSmsChannel.attemptNanoseconds) / 1_000_000))
            do {
                let descriptor = try connector.open(timeoutMilliseconds: attempt)
                // A connection that completed after a cancel is closed unused.
                let adopted: Bool = lock.withLock {
                    guard !ended else { return false }
                    inUse = descriptor
                    return true
                }
                if adopted { return descriptor }
                system.close(descriptor)
                return nil
            } catch let error as POSIXError where [.ECONNREFUSED, .ECONNRESET, .ETIMEDOUT].contains(error.code) {
                let after = system.nowNanoseconds()
                if after < end { system.sleep(nanoseconds: min(end - after, GuestSmsChannel.attemptNanoseconds)) }
            } catch {
                return nil
            }
        }
    }

    private enum Step { case done, failed }

    /// The whole frame, within 2 s of the first wait and the deadline. `written`
    /// counts the bytes that went, which is what decides the outcome of a failure.
    private func write(_ frame: Data, to descriptor: Int32, deadline: UInt64, written: inout Int) -> Step {
        let end = min(system.nowNanoseconds() + GuestSmsChannel.writeCapNanoseconds, deadline)
        while written < frame.count {
            if isEnded { return .failed }
            let now = system.nowNanoseconds()
            guard now < end else { return .failed }
            guard system.wait(descriptor, forWriting: true, timeoutMilliseconds: Self.sliceMilliseconds(end - now)) else { continue }
            // Dispatch is a decision of this thread: a cancel that came while it waited stops here.
            if isEnded { return .failed }
            let count = frame.withUnsafeBytes { buffer in
                system.write(descriptor, from: UnsafeRawBufferPointer(rebasing: buffer[written...]))
            }
            if count > 0 {
                written += count
            } else if count == 0 || -count == Int(EAGAIN) || -count == Int(EINTR) {
                continue
            } else {
                return .failed
            }
        }
        return .done
    }

    /// Exactly `count` bytes before the deadline; nil at the end of the
    /// stream, on an error, on a cancel or at the deadline.
    private func read(count: Int, from descriptor: Int32, deadline: UInt64) -> [UInt8]? {
        var bytes = [UInt8](repeating: 0, count: count)
        var done = 0
        while done < count {
            if isEnded { return nil }
            let now = system.nowNanoseconds()
            guard now < deadline else { return nil }
            guard system.wait(descriptor, forWriting: false, timeoutMilliseconds: Self.sliceMilliseconds(deadline - now)) else { continue }
            let got = bytes.withUnsafeMutableBytes { buffer in
                system.read(descriptor, into: UnsafeMutableRawBufferPointer(rebasing: buffer[done...]))
            }
            if got > 0 {
                done += got
            } else if got == 0 {
                return nil
            } else if -got == Int(EAGAIN) || -got == Int(EINTR) {
                continue
            } else {
                return nil
            }
        }
        return bytes
    }

    /// A wait of at most one second, so that the worker looks at `ended` and
    /// the deadline again however the wait was woken.
    private static func sliceMilliseconds(_ left: UInt64) -> Int32 {
        Int32(max(1, min(left / 1_000_000, 1000)))
    }
}
