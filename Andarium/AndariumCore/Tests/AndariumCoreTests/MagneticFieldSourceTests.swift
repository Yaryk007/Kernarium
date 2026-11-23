// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The decisions of the device's magnetic field, on a backend and a timer a test
/// drives: what a start delivers, when it has failed, what a movement notice
/// changes, and which callbacks are too old to count.
struct MagneticFieldSourceTests {
    /// Core Motion's device motion as the source sees it.
    final class Backend: MagneticFieldBackend, @unchecked Sendable {
        private let lock = NSLock()
        private var listeners: [@Sendable (MagneticFieldEvent) -> Void] = []
        private var intervals: [TimeInterval] = []
        private var ended = 0
        private var retimed: [TimeInterval] = []
        var begins: Int { lock.withLock { listeners.count } }
        var begunIntervals: [TimeInterval] { lock.withLock { intervals } }
        var retimes: [TimeInterval] { lock.withLock { retimed } }
        var ends: Int { lock.withLock { ended } }

        func begin(interval: TimeInterval, events: @escaping @Sendable (MagneticFieldEvent) -> Void) {
            lock.withLock {
                listeners.append(events)
                intervals.append(interval)
            }
        }
        func retime(interval: TimeInterval) { lock.withLock { retimed.append(interval) } }
        func end() { lock.withLock { ended += 1 } }

        /// An event that the begin number `start` (from 0) is delivering, whatever has happened since.
        func emit(_ event: MagneticFieldEvent, start: Int = 0) {
            let listener = lock.withLock { listeners[start] }
            listener(event)
        }
    }

    /// The watchdog's timer: nothing fires until a test says.
    final class Timer: @unchecked Sendable {
        private let lock = NSLock()
        private var scheduled: [(DispatchTimeInterval, @Sendable () -> Void)] = []
        var count: Int { lock.withLock { scheduled.count } }
        var delays: [DispatchTimeInterval] { lock.withLock { scheduled.map { $0.0 } } }
        func schedule(_ delay: DispatchTimeInterval, _ block: @escaping @Sendable () -> Void) {
            lock.withLock { scheduled.append((delay, block)) }
        }
        func fire(_ index: Int = 0) {
            let block = lock.withLock { scheduled[index].1 }
            block()
        }
        var after: MagneticFieldSource.Timer { { [self] delay, block in schedule(delay, block) } }
    }

    final class Notes: @unchecked Sendable {
        private let lock = NSLock()
        private var seen: [MagneticFieldNote] = []
        var all: [MagneticFieldNote] { lock.withLock { seen } }
        func add(_ note: MagneticFieldNote) { lock.withLock { seen.append(note) } }
    }

    /// What a start delivered to its handler and its failure.
    final class Outcome: @unchecked Sendable {
        private let lock = NSLock()
        private var fields: [[Double]] = []
        private var failed = 0
        var readings: [[Double]] { lock.withLock { fields } }
        var failures: Int { lock.withLock { failed } }
        func reading(_ x: Double, _ y: Double, _ z: Double, _ status: UInt8) {
            lock.withLock { fields.append([x, y, z, Double(status)]) }
        }
        func failure() { lock.withLock { failed += 1 } }
    }

    private func make(
        _ backend: Backend, _ timer: Timer, _ notes: Notes = Notes()
    ) -> MagneticFieldSource {
        MagneticFieldSource(backend: backend, after: timer.after, log: { notes.add($0) })
    }

    private func start(_ source: MagneticFieldSource, _ outcome: Outcome, interval: TimeInterval = 0.01) {
        source.start(
            interval: interval, handler: { outcome.reading($0, $1, $2, $3) }, failure: { outcome.failure() })
    }

    @Test func aStartBeginsDeviceMotionAtTheIntervalAndDeliversItsReadings() {
        let backend = Backend()
        let timer = Timer()
        let outcome = Outcome()
        let source = make(backend, timer)
        start(source, outcome, interval: 0.02)
        #expect(backend.begins == 1 && backend.begunIntervals == [0.02] && backend.retimes.isEmpty)
        #expect(timer.delays == [MagneticFieldSource.silence])
        backend.emit(.reading(x: 20, y: -5, z: -40, status: 3))
        #expect(outcome.readings == [[20, -5, -40, 3]] && outcome.failures == 0)
        source.stop()
        #expect(backend.ends == 1)
    }

    @Test func silenceWithoutANoticeIsAFailureAfterTheBound() {
        let backend = Backend()
        let timer = Timer()
        let notes = Notes()
        let outcome = Outcome()
        let source = make(backend, timer, notes)
        start(source, outcome)
        #expect(outcome.failures == 0)
        timer.fire()
        #expect(outcome.failures == 1 && notes.all == [.silent])
        // Said once: the timer is not asked again.
        timer.fire()
        #expect(outcome.failures == 1)
    }

    @Test func aReadingBeforeTheBoundMeansTheStartWorks() {
        let backend = Backend()
        let timer = Timer()
        let outcome = Outcome()
        let source = make(backend, timer)
        start(source, outcome)
        backend.emit(.reading(x: 1, y: 2, z: 3, status: 1))
        timer.fire()
        #expect(outcome.failures == 0)
    }

    @Test func aMovementNoticeFollowedBySilenceIsNoFailure() {
        let backend = Backend()
        let timer = Timer()
        let notes = Notes()
        let outcome = Outcome()
        let source = make(backend, timer, notes)
        start(source, outcome)
        backend.emit(.needsMovement)
        backend.emit(.needsMovement)
        // The notice is told once however often Core Motion says it.
        #expect(notes.all == [.needsMovement])
        timer.fire()
        #expect(outcome.failures == 0 && notes.all == [.needsMovement])
        // The field flows when the device is moved and Core Motion delivers.
        backend.emit(.reading(x: 30, y: 1, z: -35, status: 0))
        #expect(outcome.readings == [[30, 1, -35, 0]] && outcome.failures == 0)
    }

    @Test func aNoticeThatArrivesAfterAnotherStartBelongsToNoOne() {
        let backend = Backend()
        let timer = Timer()
        let notes = Notes()
        let outcome = Outcome()
        let source = make(backend, timer, notes)
        start(source, outcome)
        source.stop()
        start(source, outcome)
        backend.emit(.needsMovement, start: 0)
        #expect(notes.all.isEmpty)
        // The second start has no notice, so it is judged on its own.
        timer.fire(1)
        #expect(outcome.failures == 1)
    }

    @Test func repeatedErrorsAreOneFailureAndAFailedStartDeliversNothingMore() {
        let backend = Backend()
        let timer = Timer()
        let notes = Notes()
        let outcome = Outcome()
        let source = make(backend, timer, notes)
        start(source, outcome)
        backend.emit(.reading(x: 1, y: 2, z: 3, status: 3))
        backend.emit(.failed(reason: "CMErrorDomain 104"))
        backend.emit(.failed(reason: "CMErrorDomain 104"))
        backend.emit(.failed(reason: "CMErrorDomain 105"))
        #expect(outcome.failures == 1 && notes.all == [.failed(reason: "CMErrorDomain 104")])
        backend.emit(.reading(x: 4, y: 5, z: 6, status: 3))
        #expect(outcome.readings == [[1, 2, 3, 3]])
        // The watchdog of a failed start does nothing more.
        timer.fire()
        #expect(outcome.failures == 1)
    }

    @Test func aStaleGenerationDeliversAndFailsNothing() {
        let backend = Backend()
        let timer = Timer()
        let outcome = Outcome()
        let source = make(backend, timer)
        start(source, outcome)
        source.stop()
        start(source, outcome)
        #expect(backend.begins == 2 && backend.ends == 1)
        backend.emit(.reading(x: 9, y: 9, z: 9, status: 3), start: 0)
        backend.emit(.failed(reason: "late"), start: 0)
        timer.fire(0)
        #expect(outcome.readings.isEmpty && outcome.failures == 0)
        // The current one is not affected.
        backend.emit(.reading(x: 1, y: 2, z: 3, status: 2), start: 1)
        #expect(outcome.readings == [[1, 2, 3, 2]])
        // A stop ends every callback of the start, the late ones included.
        source.stop()
        backend.emit(.reading(x: 7, y: 7, z: 7, status: 3), start: 1)
        backend.emit(.failed(reason: "after the stop"), start: 1)
        timer.fire(1)
        #expect(outcome.readings.count == 1 && outcome.failures == 0)
    }

    @Test func aStopFromAnotherSourceOnTheSameManagerStopsNothing() {
        let backend = Backend()
        let timer = Timer()
        let running = Outcome()
        let first = make(backend, timer)
        start(first, running)
        // The application builds more than one device object: another one's
        // stop, and its release, leave the running one alone.
        do {
            let second = make(backend, timer)
            second.stop()
        }
        #expect(backend.ends == 0)
        backend.emit(.reading(x: 1, y: 2, z: 3, status: 3))
        #expect(running.readings.count == 1)
        first.stop()
        #expect(backend.ends == 1)
        first.stop()
        #expect(backend.ends == 1)
    }

    @Test func aStartWhileItRunsOnlyRetimesAndUsesTheNewHandlers() {
        let backend = Backend()
        let timer = Timer()
        let first = Outcome()
        let second = Outcome()
        let source = make(backend, timer)
        start(source, first, interval: 0.01)
        start(source, second, interval: 0.05)
        #expect(backend.begins == 1 && backend.retimes == [0.05] && backend.ends == 0)
        // The bound is the start's, not the retime's.
        #expect(timer.count == 1)
        backend.emit(.reading(x: 1, y: 2, z: 3, status: 3))
        #expect(first.readings.isEmpty && second.readings == [[1, 2, 3, 3]])
        timer.fire()
        #expect(second.failures == 0)
    }

    @Test func aFieldThatCannotBeSentIsNotDeliveredAndIsNotAReading() {
        let backend = Backend()
        let timer = Timer()
        let outcome = Outcome()
        let source = make(backend, timer)
        start(source, outcome)
        backend.emit(.reading(x: .nan, y: 2, z: 3, status: 3))
        backend.emit(.reading(x: 1, y: .infinity, z: 3, status: 3))
        backend.emit(.reading(x: 1, y: 2, z: 1e39, status: 3))
        #expect(outcome.readings.isEmpty)
        // Unusable data does not satisfy the watchdog.
        timer.fire()
        #expect(outcome.failures == 1)
    }

    @Test func aStartAfterAFailureIsAFreshOne() {
        let backend = Backend()
        let timer = Timer()
        let outcome = Outcome()
        let source = make(backend, timer)
        start(source, outcome)
        backend.emit(.failed(reason: "x"))
        source.stop()
        start(source, outcome)
        backend.emit(.reading(x: 1, y: 2, z: 3, status: 3), start: 1)
        #expect(outcome.readings == [[1, 2, 3, 3]])
        backend.emit(.failed(reason: "y"), start: 1)
        #expect(outcome.failures == 2)
    }
}
