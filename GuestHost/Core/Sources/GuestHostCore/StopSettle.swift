// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a host-side end of a running guest (the person's Stop, a screen that
/// closes) does with the guest before the host stops it: asks it to write its
/// storage back and waits for its answer, for as long as a lifecycle edge's
/// work may take (four seconds in both applications) and no longer. A guest
/// that cannot answer (no console yet, a runtime that is not there, a guest
/// that does not reply) costs the bound at most, and the host's stop follows
/// it either way.
public enum StopSettle {
    /// How long the stop waits for the guest.
    public static let bound = DispatchTimeInterval.seconds(4)

    /// What the write-back of one stop came to.
    public enum Outcome: Equatable, Sendable {
        /// The runtime answered, as it reports.
        case answered(GuestSettle)
        /// The runtime did not answer within the bound; its call is left to
        /// end by itself.
        case timedOut
        /// The run had ended before the write-back began, so it was dropped:
        /// it would have reached whatever runs next.
        case runEnded

        /// Whether a guest was asked: it confirmed, refused, could not be
        /// reached or did not answer in time. A write-back that found no
        /// guest, or reported nothing, asked nobody, and a stop made again
        /// asks again.
        public var reachedGuest: Bool {
            switch self {
            case let .answered(settle):
                switch settle.storage {
                case .confirmed, .unconfirmed, .refused, .unreachable: true
                case .noGuest, .notReported: false
                }
            case .timedOut: true
            case .runEnded: false
            }
        }
    }

    /// Runs `work`, the runtime's write-back, on a thread of its own for the
    /// run `stop` belongs to, and waits for it at most `bound`. The thread
    /// asks the run's latch before it calls the runtime: a write-back that
    /// begins after the run ended is dropped. One that is inside the runtime's
    /// call when the bound passes is left to end by itself, and the caller
    /// goes on without it.
    public static func run(
        for stop: RunStop, within bound: DispatchTimeInterval = bound,
        _ work: @escaping @Sendable () -> GuestSettle
    ) -> Outcome {
        let result = Box()
        let finished = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .userInitiated).async {
            result.set(stop.isLive ? .answered(work()) : .runEnded)
            finished.signal()
        }
        guard finished.wait(timeout: .now() + bound) == .success else { return .timedOut }
        return result.value ?? .timedOut
    }

    private final class Box: @unchecked Sendable {
        private let lock = NSLock()
        private var outcome: Outcome?
        func set(_ outcome: Outcome) { lock.withLock { self.outcome = outcome } }
        var value: Outcome? { lock.withLock { outcome } }
    }
}
