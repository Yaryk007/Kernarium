// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Darwin
import Foundation
import Testing
@testable import Andarium

/// The session's one notice that the frame rate is lowered while the device
/// is hot, in the model and through the thermal session: it shows once the
/// session screen is up, once in a run, and goes when dismissed or after its
/// time. When it is due is `ThermalPolicy`'s (tested in `andarium-core`): at
/// iOS's critical state and not before.
@MainActor
struct ThermalNoticeFlowTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(_ phase: Phase = .running(.normal)) -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KiB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        model.phase = phase
        return model
    }

    /// Holds the notice's wait until a test lets it go.
    @MainActor
    private final class Wait {
        private(set) var waits: [Duration] = []
        private var held: CheckedContinuation<Void, Never>?

        func sleep(_ duration: Duration) async {
            waits.append(duration)
            await withTaskCancellationHandler {
                await withCheckedContinuation { held = $0 }
            } onCancel: {
                Task { @MainActor in self.release() }
            }
        }

        var isWaiting: Bool { held != nil }

        func release() {
            let continuation = held
            held = nil
            continuation?.resume()
        }
    }

    private func until(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(5)
        while !condition() {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(1))
        }
        return true
    }

    // MARK: The model

    @Test func theNoticeShowsAtOnceOnTheSessionScreenAndDismissingEndsIt() {
        let model = model()
        #expect(!model.thermalNotice.isShowing)
        model.thermalCapStarted()
        #expect(model.thermalNotice.isShowing)
        model.dismissThermalNotice()
        #expect(!model.thermalNotice.isShowing)
    }

    @Test func aCapAnnouncedDuringTheBootShowsWhenTheSessionScreenIsUp() {
        let model = model(.booting(.normal))
        model.thermalCapStarted()
        #expect(!model.thermalNotice.isShowing, "the notice was on the boot screen")
        model.phase = .running(.normal)
        #expect(model.thermalNotice.isShowing)
        model.dismissThermalNotice()
        model.phase = .running(.normal)
        #expect(!model.thermalNotice.isShowing, "the notice came back")
    }

    @Test func aNoticeNobodyDismissesGoesAfterThirtySeconds() async {
        let wait = Wait()
        let model = model()
        model.thermalNoticeSleep = { await wait.sleep($0) }
        model.thermalCapStarted()
        #expect(await until { wait.isWaiting })
        #expect(wait.waits == [.seconds(30)])
        #expect(model.thermalNotice.isShowing)
        wait.release()
        #expect(await until { !model.thermalNotice.isShowing })
    }

    @Test func dismissingTheNoticeEndsItsWait() async {
        let wait = Wait()
        let model = model()
        model.thermalNoticeSleep = { await wait.sleep($0) }
        model.thermalCapStarted()
        #expect(await until { wait.isWaiting })
        model.dismissThermalNotice()
        #expect(await until { !wait.isWaiting })
        #expect(!model.thermalNotice.isShowing)
    }

    @Test func theEndOfTheRunTakesTheNoticeAwayAndAnUnshownOneWithIt() {
        let model = model()
        model.thermalCapStarted()
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(!model.thermalNotice.isShowing)

        let booting = self.model(.booting(.normal))
        booting.thermalCapStarted()
        booting.runEnded(RunEnd(result: 0), uptimeSeconds: 5)
        booting.phase = .running(.normal)
        #expect(!booting.thermalNotice.isShowing, "a notice of the run that ended showed in the next")
    }

    @Test func aRestartTakesTheNoticeAwayAndTheRunDoesNotAnnounceAgain() {
        let model = model()
        model.thermalCapStarted()
        #expect(model.thermalNotice.isShowing)
        model.restart()
        #expect(!model.thermalNotice.isShowing)
        model.phase = .running(.normal)
        #expect(!model.thermalNotice.isShowing)
    }

    @Test func recoveryHasNoNotice() {
        let model = model(.running(.recovery))
        model.thermalCapStarted()
        #expect(!model.thermalNotice.isShowing)
    }

    // MARK: Through the session

    /// The guest's end of the thermal connection.
    private final class Hal: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var peer: Int32 = -1
        func openStream() throws -> Int32 {
            var sockets: [Int32] = [-1, -1]
            guard socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0 else { throw POSIXError(.EIO) }
            lock.withLock { peer = sockets[1] }
            return sockets[0]
        }
        deinit { if peer >= 0 { close(peer) } }

        /// The next record, waiting for at most three seconds.
        func record() -> [UInt8] {
            let deadline = Date().addingTimeInterval(3)
            while lock.withLock({ peer }) < 0, Date() < deadline { usleep(2000) }
            let peer = lock.withLock { self.peer }
            var result: [UInt8] = []
            while result.count < ThermalProtocol.recordSize, Date() < deadline {
                var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
                guard poll(&descriptor, 1, 10) > 0 else { continue }
                var bytes = [UInt8](repeating: 0, count: ThermalProtocol.recordSize - result.count)
                let size = recv(peer, &bytes, bytes.count, MSG_DONTWAIT)
                guard size > 0 else { break }
                result.append(contentsOf: bytes.prefix(size))
            }
            return result
        }

        /// Whether another record arrives within a moment.
        func hasRecord() -> Bool {
            let peer = lock.withLock { self.peer }
            guard peer >= 0 else { return false }
            var descriptor = pollfd(fd: peer, events: Int16(POLLIN), revents: 0)
            return poll(&descriptor, 1, 150) > 0
        }
    }

    private nonisolated final class Clock: @unchecked Sendable {
        private let lock = NSLock()
        private var nanoseconds: UInt64 = 5_000_000_000
        func read() -> UInt64 { lock.withLock { nanoseconds } }
        func advance(seconds: UInt64) { lock.withLock { nanoseconds += seconds * 1_000_000_000 } }
    }

    private nonisolated final class Timers: @unchecked Sendable {
        private let lock = NSLock()
        private var latest: (@Sendable () -> Void)?
        func schedule(_ work: @escaping @Sendable () -> Void) { lock.withLock { latest = work } }
        func fire() { lock.withLock { latest }?() }
    }

    private func frame(_ severity: AndroidThermalSeverity) -> [UInt8] { Array(ThermalProtocol.record(severity)) }

    @Test func theNoticeAppearsOnceAtTheFirstCriticalAndNotAtFairOrSeriousOrAgainAfterCoolingAndHeatingAgain() async {
        let model = model()
        let hal = Hal()
        let clock = Clock()
        let timers = Timers()
        let session = ThermalSession(
            opener: hal, clock: { clock.read() }, schedule: { _, work in timers.schedule(work) },
            capStarted: { Task { @MainActor in model.thermalCapStarted() } })
        session.update(running: true, booted: true)
        #expect(hal.record() == frame(.none))

        session.observe(.fair)
        session.settle()
        #expect(!hal.hasRecord(), "fair told the guest")
        try? await Task.sleep(for: .milliseconds(60))
        #expect(!model.thermalNotice.isShowing, "announced at fair")

        session.observe(.serious)
        session.settle()
        #expect(!hal.hasRecord(), "serious told the guest")
        try? await Task.sleep(for: .milliseconds(60))
        #expect(!model.thermalNotice.isShowing, "announced at serious")

        session.observe(.critical)
        #expect(hal.record() == frame(.critical))
        #expect(await until { model.thermalNotice.isShowing })
        model.dismissThermalNotice()

        // Serious within the hold keeps critical and tells the guest nothing.
        session.observe(.serious)
        session.settle()
        #expect(!hal.hasRecord(), "the fall to serious told the guest at once")
        clock.advance(seconds: 60)
        session.observe(.critical)
        session.settle()
        #expect(!hal.hasRecord(), "the return to critical told the guest again")
        try? await Task.sleep(for: .milliseconds(60))
        #expect(!model.thermalNotice.isShowing, "a second critical announced a second cap")

        session.observe(.nominal)
        session.settle()
        clock.advance(seconds: 130)
        timers.fire()
        #expect(hal.record() == frame(.none))
        session.observe(.serious)
        session.settle()
        #expect(!hal.hasRecord(), "serious after the fall told the guest")
        session.observe(.critical)
        #expect(hal.record() == frame(.critical))
        session.settle()
        try? await Task.sleep(for: .milliseconds(60))
        #expect(!model.thermalNotice.isShowing, "announced twice in a run")
        session.update(running: false, booted: false)
    }

    @Test func aRunThatNeverReachesCriticalShowsNoNoticeAndTellsTheGuestNothing() async {
        let model = model()
        let hal = Hal()
        let session = ThermalSession(
            opener: hal, capStarted: { Task { @MainActor in model.thermalCapStarted() } })
        session.update(running: true, booted: true)
        #expect(hal.record() == frame(.none))
        for state in [HostThermalState.fair, .serious, .nominal, .serious, .fair, .fair, .nominal] {
            session.observe(state)
        }
        session.settle()
        #expect(!hal.hasRecord(), "a state below critical told the guest")
        try? await Task.sleep(for: .milliseconds(60))
        #expect(!model.thermalNotice.isShowing)
        session.update(running: false, booted: false)
    }

    // MARK: The override

    /// A launch that states `word` drives the thermal session through the
    /// monitor; `body` reads what the guest hears after its first record.
    private func overridden(by word: String, model: AppModel, _ body: (Hal) async -> Void) async {
        let hal = Hal()
        let session = ThermalSession(
            opener: hal, capStarted: { Task { @MainActor in model.thermalCapStarted() } })
        let environment = ["ANDARIUM_THERMAL_STATE": word]
        let monitor = ThermalMonitor(
            center: NotificationCenter(), read: { ThermalOverride.stated(in: environment) ?? .nominal },
            deliver: { session.observe($0) })
        session.update(running: true, booted: true)
        // A connection starts with the severity of the moment it is made, so the
        // monitor's reading is delivered after the first record is read: none,
        // then the reading's, however soon the connection is made.
        #expect(hal.record() == frame(.none))
        monitor.start()
        await body(hal)
        monitor.stop()
        session.update(running: false, booted: false)
    }

    @Test func aLaunchOverrideOfSeriousTellsTheGuestNothingAndShowsNoNotice() async {
        let model = model()
        await overridden(by: "serious", model: model) { hal in
            #expect(!hal.hasRecord(), "serious told the guest")
            try? await Task.sleep(for: .milliseconds(60))
            #expect(!model.thermalNotice.isShowing)
        }
    }

    @Test func theLaunchOverrideOfCriticalSendsCriticalAndShowsTheNotice() async {
        let model = model()
        await overridden(by: "critical", model: model) { hal in
            #expect(hal.record() == frame(.critical))
            #expect(await until { model.thermalNotice.isShowing })
        }
    }

    @Test func aLaunchOverrideOfFairTellsTheGuestNothingAndShowsNoNotice() async {
        let model = model()
        await overridden(by: "fair", model: model) { hal in
            #expect(!hal.hasRecord(), "fair told the guest")
            try? await Task.sleep(for: .milliseconds(60))
            #expect(!model.thermalNotice.isShowing)
        }
    }

    @Test func theMonitorReadsAgainOnRefreshAndOnTheSystemsNotification() {
        let center = NotificationCenter()
        var states: [HostThermalState] = [.nominal, .fair, .serious]
        var delivered: [HostThermalState] = []
        let monitor = ThermalMonitor(
            center: center, read: { states.isEmpty ? .nominal : states.removeFirst() },
            deliver: { delivered.append($0) })
        monitor.start()
        #expect(delivered == [.nominal])
        monitor.refresh()
        #expect(delivered == [.nominal, .fair])
        center.post(name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
        #expect(delivered == [.nominal, .fair, .serious])
        monitor.stop()
        center.post(name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
        #expect(delivered.count == 3, "a stopped monitor read the state")
    }

    @Test func theSourceFollowsTheProcessEnvironment() {
        let name = ThermalOverride.environmentName
        let before = getenv(name).map { String(cString: $0) }
        defer {
            if let before { setenv(name, before, 1) } else { unsetenv(name) }
        }
        setenv(name, "critical", 1)
        #expect(ThermalSource.current() == .critical)
        setenv(name, "fair", 1)
        #expect(ThermalSource.current() == .fair)
        unsetenv(name)
        #expect(ThermalSource.current() == HostThermalState(ProcessInfo.processInfo.thermalState))
    }
}
