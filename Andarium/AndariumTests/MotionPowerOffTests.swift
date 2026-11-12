// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Opt-in real-guest test: Android powering itself off with Gyroscope on
/// leaves no motion problem on Home. Run with `ANDARIUM_MOTION_POWEROFF=1`
/// and `ANDARIUM_GUEST_INSTANCE=motion-poweroff`, on a Mac (Designed for
/// iPad) or a device.
///
/// The other guest tests turn the Gyroscope switch off before they send
/// `reboot -p`, so none of them saw the guest's sensors service end under a
/// running session. This one keeps a session on through the power-off, as the
/// application does, and stops it once the run has ended, as
/// `LiveRuntime.finished` stops `MotionSensors`. The Mac has no motion
/// sensors, so the model never starts its own session there: the session is
/// the one `MotionSensors` owns, over the runtime's vsock, with a device that
/// reads nothing, and the last problem it reported is what Home shows.
@Suite(.serialized)
@MainActor
struct MotionPowerOffTests {
    private final class Connector: GuestStreamOpener, @unchecked Sendable {
        private let lock = NSLock()
        private var count = 0
        var attempts: Int { lock.withLock { count } }
        func openStream() throws -> Int32 {
            lock.withLock { count += 1 }
            let descriptor = kernarium_runtime_loader_connect_vsock(MotionStream.port, 250)
            guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: -descriptor) ?? .EIO) }
            return descriptor
        }
    }

    private final class SilentDevice: MotionDevice, @unchecked Sendable {
        let hasAccelerometer = true
        let hasGyroscope = true
        func start(
            _ sensor: MotionSample.Sensor, interval: TimeInterval,
            handler: @escaping @Sendable (Double, Double, Double) -> Void, failure: @escaping @Sendable () -> Void
        ) {}
        func stop(_ sensor: MotionSample.Sensor) {}
    }

    private nonisolated final class Problems: @unchecked Sendable {
        private let lock = NSLock()
        private var seen: [MotionProblem?] = []
        func record(_ problem: MotionProblem?) { lock.withLock { seen.append(problem) } }
        var all: [MotionProblem?] { lock.withLock { seen } }
    }

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_MOTION_POWEROFF"] == "1"),
          .timeLimit(.minutes(20)))
    func androidPoweringOffWithTheSwitchOnLeavesNoProblem() async throws {
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "motion-poweroff")
        let connector = Connector()
        let problems = Problems()
        let session = MotionSession(opener: connector, device: SilentDevice(), problem: problems.record)
        defer { session.update(enabled: false, foreground: false, axes: .portrait) }
        try await withAndroidGuest { _, _ in
            session.update(enabled: true, foreground: true, axes: .portrait)
            // An on-demand service speaks on connect; an older one never does,
            // and its session reads both sensors at once.
            func connected() -> Bool { session.demand != nil || session.plan != .off }
            let deadline = ContinuousClock.now + .seconds(30)
            while !connected(), ContinuousClock.now < deadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            try #require(connected(), "The guest's motion service did not accept the connection")
            try #require(problems.all.allSatisfy { $0 == nil }, "\(problems.all)")
            // The fixture sends `reboot -p` as this returns, with the switch
            // still on for this session.
            try await Task.sleep(for: .seconds(2))
        }
        // The run has ended, powered off.
        session.update(enabled: false, foreground: false, axes: .portrait)
        session.settle()
        // The service's connection ended and the session went on asking for it,
        // so the power-off reached a session that was on.
        #expect(connector.attempts >= 2, "The guest's motion service connection did not end under the session")
        #expect(!problems.all.contains(.guestUnavailable), "\(problems.all)")
        #expect(problems.all.last == .some(nil), "\(problems.all)")
    }
}
