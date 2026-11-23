// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GuestHostCore

/// How an application delivers the host's stop of a run (`RunStop`) to the
/// runtime: off the main actor, on the queue of its other waiting runtime
/// calls, with the guest's write-back before it. Both applications end a
/// running guest through it, so the rules are one copy.
enum GuestStopDelivery {
    /// How long after a delivery the next is made, while the run lives.
    nonisolated static let interval = DispatchTimeInterval.seconds(1)

    /// The runtime's two calls a host stop makes: the guest's write-back and
    /// the stop itself. A test substitutes these and nothing else, so that
    /// what the application composes of them (the bound, the order, which
    /// outcome counts as having reached a guest) is the production one.
    nonisolated struct Runtime: Sendable {
        var sync: @Sendable () -> GuestSettle
        var requestStop: @Sendable () -> Void

        /// The loaded runtime's.
        static let loader = Runtime(
            sync: { GuestSettle.sync() },
            requestStop: { kernarium_runtime_loader_request_stop() }
        )
    }

    /// Delivers `stop` on `queue`, and again after `interval` until the run
    /// has ended (`RunStop.deliver`): the runtime drops a stop that arrives
    /// before its run begins, and nothing here can see that moment.
    ///
    /// The first delivery that reaches a guest asks it to write its storage
    /// back first (`settle`, which returns whether it reached one), once and
    /// for at most `StopSettle.bound`, because the host's stop ends the guest
    /// at once and what it has not written is lost. A delivery that finds no
    /// guest yet goes on to the host's stop (`request`), and the next one asks
    /// again; a run that has ended is asked nothing.
    nonisolated static func deliver(
        _ stop: RunStop, on queue: DispatchQueue, every interval: DispatchTimeInterval = interval,
        settle: @escaping @Sendable () -> Bool, request: @escaping @Sendable () -> Void
    ) {
        queue.async {
            guard stop.deliver(settle: settle, request) else { return }
            queue.asyncAfter(deadline: .now() + interval) {
                deliver(stop, on: queue, every: interval, settle: settle, request: request)
            }
        }
    }

    /// The runtime's write-back of the guest's storage, for the host stop of
    /// the run `stop` belongs to (`StopSettle` bounds it): the guest confirms
    /// it, or says it cannot, or there is none to ask, and the host's stop
    /// follows in each case. Returns whether a guest was reached. `prefix`
    /// starts the one line it logs.
    nonisolated static func settleBeforeStop(
        for stop: RunStop, logPrefix prefix: String, runtime: Runtime = .loader
    ) -> Bool {
        let outcome = StopSettle.run(for: stop) { runtime.sync() }
        // A stop that finds no guest is made again each second: only the one
        // that reached a guest is worth a line.
        guard outcome.reachedGuest else { return false }
        switch outcome {
        case let .answered(settle):
            print(prefix + settle.logLine(reason: "stop"))
        case .timedOut:
            print(prefix + "guest storage NOT confirmed on stop: no answer in time")
        case .runEnded:
            break
        }
        return true
    }
}
