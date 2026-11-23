// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CoreLocation
import Foundation
import Testing
@testable import Andarium

#if targetEnvironment(simulator)
/// Opt-in: the real `HostLocation` against the Simulator's own simulated
/// location, never a real one. The Simulator reads no location of the Mac's: it
/// delivers what `xcrun simctl location <device> set` gave it, and nothing when
/// none was set. Before the run, grant Andarium the access and set a location
/// on the Simulator the tests run on:
///
///     xcrun simctl privacy <device> grant location com.imxieyi.andarium
///     xcrun simctl location <device> set 37.3349,-122.0090
///
/// then run with `TEST_RUNNER_ANDARIUM_LOCATION_SIMULATOR=1`. Without the
/// variable the test does nothing.
@MainActor
struct HostLocationSimulatorTests {
    nonisolated static var enabled: Bool { ProcessInfo.processInfo.environment["ANDARIUM_LOCATION_SIMULATOR"] == "1" }

    private func eventually(_ seconds: Double, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(100))
        }
        return condition()
    }

    @Test(.enabled(if: HostLocationSimulatorTests.enabled), .timeLimit(.minutes(2)))
    func aSimulatedLocationReachesTheGuestsReceiverOnlyWhileItsPowered() async throws {
        let access = SystemLocationAccess()
        try #require(access.state == .precise, "grant the access with simctl privacy before the run")
        let host = HostLocation(access: access)
        let receiver = RecordingReceiver()
        let session = LocationSession(updates: host, sink: receiver)
        host.deliver = { session.deliver($0) }

        // Nothing is read until the guest's receiver powers up.
        session.update(enabled: true, authorization: access.state, foreground: true, running: true)
        try await Task.sleep(for: .seconds(2))
        #expect(receiver.reports.isEmpty)

        session.receiverPower(true)
        try #require(await eventually(45) { !receiver.reports.isEmpty }, "no position from the Simulator: set one with simctl location")
        let fix = try #require(receiver.reports.last)
        #expect(!fix.approximate)
        #expect(abs(fix.latitude - 37.3349) < 0.01 && abs(fix.longitude + 122.009) < 0.01)
        #expect((fix.horizontalAccuracy ?? -1) >= 0)
        let age = Date().timeIntervalSince1970 - Double(fix.utcNanoseconds) / 1e9
        #expect(abs(age) < 120)

        // The runtime's form of it, as the guest's receiver takes it.
        let runtime = RuntimeLocationSink.runtimeFix(fix)
        #expect(runtime.flags & UInt32(KERNARIUM_RUNTIME_LOCATION_APPROXIMATE) == 0)

        // Powered off: the reading stops and the held position is dropped.
        let clears = receiver.clears
        session.receiverPower(false)
        #expect(!receiver.reading)
        #expect(receiver.clears > clears)
        let count = receiver.reports.count
        try await Task.sleep(for: .seconds(3))
        #expect(receiver.reports.count == count)
        print("LOCATION-SIM: fix \(fix.latitude),\(fix.longitude) accuracy \(fix.horizontalAccuracy ?? -1) m, \(count) reports")
    }
}
#endif
