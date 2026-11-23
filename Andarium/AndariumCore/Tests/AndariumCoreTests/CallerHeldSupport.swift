// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Runs `work`, a loop of calls that must not wait for a peer, on a thread of
/// its own and answers whether it returned within `bound` seconds of the
/// moment the thread was running.
///
/// The loop is not given to `DispatchQueue.global()`. A full run of this
/// package keeps the process's thread pool busy with its other tests, and a
/// default-priority block can wait seconds there for a thread; the test would
/// count that wait as a stream that held its caller. A `Thread` starts at once,
/// so the bound covers what the stream does with the caller and nothing else.
/// The wait for the thread to be running has a bound too, of a size no start
/// reaches.
func returnsWithoutWaiting(within bound: TimeInterval = 30, _ work: @escaping @Sendable () -> Void) -> Bool {
    let started = DispatchSemaphore(value: 0)
    let finished = DispatchSemaphore(value: 0)
    Thread {
        started.signal()
        work()
        finished.signal()
    }.start()
    guard started.wait(timeout: .now() + 120) == .success else { return false }
    return finished.wait(timeout: .now() + bound) == .success
}
