// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The person's stop of one run, across the hand-over from the application to
/// the runtime's own thread.
///
/// The runtime drops a stop asked for before its run begins, which is some
/// time after the thread that runs it has started (it prepares the guest's
/// disk first). Without this a Stop in that gap is lost and the guest boots.
/// The latch keeps the request for the whole run: the thread asks it before it
/// enters the runtime (`enter`), and the request is delivered, again and again,
/// until the run has ended (`deliver`), so one that landed before the runtime
/// looked is made again after it did.
///
/// A delivery also carries the guest's write-back (`settle`) until one has
/// reached a guest: the guest is asked once, before the host stops it, and the
/// host's stop follows it whatever it came to. A write-back that found no guest
/// (the run has not started its guest yet) is not the one: the next delivery
/// asks again.
///
/// The type holds state and nothing else: whoever owns it runs the delivery,
/// off the main actor, because the request may wait on the runtime.
public final class RunStop: @unchecked Sendable {
    private let condition = NSCondition()
    private var requested = false
    private var entered = false
    private var ended = false
    private var delivering = false
    private var settled = false
    private var settling = false

    public init() {}

    /// The person chose Stop. Returns false when it was chosen before, which
    /// changes nothing.
    @discardableResult
    public func request() -> Bool {
        condition.withLock {
            guard !requested else { return false }
            requested = true
            return true
        }
    }

    public var isRequested: Bool {
        condition.withLock { requested }
    }

    /// Whether the run has not ended: work begun for this run's stop and not
    /// yet under way is dropped once it is false, so it cannot reach a later
    /// run.
    public var isLive: Bool {
        condition.withLock { !ended }
    }

    /// The run's thread is about to enter the runtime. Returns false when the
    /// person stopped first: the thread must not enter, and must `end`.
    public func enter() -> Bool {
        condition.withLock {
            guard !requested else { return false }
            entered = true
            return true
        }
    }

    /// Runs `request` when a stop was asked for, the thread has entered the
    /// runtime and the run has not ended: the delivery of a stop, which the
    /// runtime may wait on. Returns whether to deliver again later (the run
    /// has not ended); a run that has ended is never asked anything.
    ///
    /// `settle`, when there is one, runs first, until one has returned true:
    /// the guest was reached (it answered, refused or did not answer in time),
    /// which is the one write-back of the run's whole delivery. One that
    /// returns false found no guest to ask, and is made again by the next
    /// delivery. It runs only where the thread has entered the runtime (a run
    /// that never did has no guest to write back), and never twice at once.
    /// Whoever supplies it bounds it (`StopSettle`), and `end` waits for it as
    /// for the request.
    public func deliver(settle: (() -> Bool)? = nil, _ request: () -> Void) -> Bool {
        enum Step {
            case ended
            case wait
            case deliver(settling: Bool)
        }
        let step: Step = condition.withLock {
            guard !ended else { return .ended }
            guard requested, entered else { return .wait }
            delivering = true
            let asking = settle != nil && !settled && !settling
            if asking { settling = true }
            return .deliver(settling: asking)
        }
        switch step {
        case .ended:
            return false
        case .wait:
            return true
        case let .deliver(asking):
            let reached = asking ? settle?() ?? false : false
            if asking {
                condition.withLock {
                    settling = false
                    if reached { settled = true }
                }
            }
            request()
            condition.withLock {
                delivering = false
                condition.broadcast()
            }
            return true
        }
    }

    /// The run's thread is done with the runtime, however it ended. Waits for
    /// a delivery under way, so no request reaches a later run.
    public func end() {
        condition.withLock {
            ended = true
            while delivering { condition.wait() }
        }
    }
}
