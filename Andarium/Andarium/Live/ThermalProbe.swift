// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Darwin
import Foundation

/// A UI test's stand-in for the guest's thermal HAL (`ThermalProbeLaunch`):
/// the other end of the connection the thermal session makes, which reads
/// each record the host writes and says what Android would be told. The probe
/// session has no guest, so this is what lets a test see the notice, the
/// choice in Settings and the severity together.
nonisolated final class ThermalProbeHal: GuestStreamOpener, @unchecked Sendable {
    /// The reading of one connection's records, on the HAL's own queue.
    private final class Reader: @unchecked Sendable {
        var pending: [UInt8] = []
    }

    private let heard: @Sendable (AndroidThermalSeverity) -> Void
    private let queue = DispatchQueue(label: "andarium-thermal-probe-hal")
    private let lock = NSLock()
    private var sources: [DispatchSourceRead] = []

    /// `heard` is called on the HAL's queue with the severity of each record.
    init(heard: @escaping @Sendable (AndroidThermalSeverity) -> Void) {
        self.heard = heard
    }

    func openStream() throws -> Int32 {
        var pair: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { throw POSIXError(.EIO) }
        let peer = pair[1]
        let reader = Reader()
        let source = DispatchSource.makeReadSource(fileDescriptor: peer, queue: queue)
        source.setEventHandler { [heard, weak source] in
            var buffer = [UInt8](repeating: 0, count: 64)
            let count = recv(peer, &buffer, buffer.count, MSG_DONTWAIT)
            if count < 0 && (errno == EAGAIN || errno == EINTR) { return }
            guard count > 0 else {
                source?.cancel()
                return
            }
            reader.pending.append(contentsOf: buffer.prefix(count))
            while reader.pending.count >= ThermalProtocol.recordSize {
                let record = Array(reader.pending.prefix(ThermalProtocol.recordSize))
                reader.pending.removeFirst(ThermalProtocol.recordSize)
                if let severity = try? ThermalProtocol.severity(of: record) { heard(severity) }
            }
        }
        source.setCancelHandler { Darwin.close(peer) }
        lock.withLock { sources.append(source) }
        source.resume()
        return pair[0]
    }
}
