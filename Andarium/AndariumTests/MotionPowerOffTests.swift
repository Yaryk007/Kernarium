// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Opt-in real-guest test: Android powering itself off under a running motion
/// session ends the session's connection, which it asks for again until the
/// run ends. Run with `ANDARIUM_MOTION_POWEROFF=1` and
/// `ANDARIUM_GUEST_INSTANCE=motion-poweroff`, on a Mac (Designed for iPad) or a
/// device.
///
/// This keeps a session on through the power-off, as the application does,
/// and stops it once the run has ended, as `LiveRuntime.finished` stops
/// `MotionSensors`. The Mac has no motion sensors, so the model never starts
/// its own session there: the session is the one a `MotionSession` owns, over
/// the runtime's vsock, with a device that reads nothing.
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

    @Test(.enabled(if: ProcessInfo.processInfo.environment["ANDARIUM_MOTION_POWEROFF"] == "1"),
          .timeLimit(.minutes(20)))
    func androidPoweringOffUnderARunningSessionIsAskedForAgain() async throws {
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "motion-poweroff")
        let connector = Connector()
        let session = MotionSession(opener: connector, device: SilentDevice())
        defer { session.update(foreground: false, axes: .portrait) }
        try await withAndroidGuest { _, _ in
            session.update(foreground: true, axes: .portrait)
            // An on-demand service speaks on connect; an older one never does,
            // and its session reads both sensors at once.
            func connected() -> Bool { session.demand != nil || session.plan != .off }
            let deadline = ContinuousClock.now + .seconds(30)
            while !connected(), ContinuousClock.now < deadline {
                try await Task.sleep(for: .milliseconds(50))
            }
            try #require(connected(), "The guest's motion service did not accept the connection")
            // The fixture sends `reboot -p` as this returns, with the session
            // still on.
            try await Task.sleep(for: .seconds(2))
        }
        // The run has ended, powered off.
        session.update(foreground: false, axes: .portrait)
        session.settle()
        // The service's connection ended and the session went on asking for it,
        // so the power-off reached a session that was on.
        #expect(connector.attempts >= 2, "The guest's motion service connection did not end under the session")
    }
}
