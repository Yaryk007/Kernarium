// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreLocation
import Foundation

/// Gives Android the iPhone's location while Location is on: Android's
/// test providers (gps, network and fused) stand in for the radios the guest
/// has none of, and each fix iOS reports is set on all three through adb.
/// The shell may install test providers once it holds the mock location app
/// op, which it grants itself.
@MainActor
final class GuestLocation: NSObject, CLLocationManagerDelegate {
    private let manager = CLLocationManager()
    private let shell: @MainActor (String) async -> Bool
    private var running = false
    private var providersReady = false
    private var lastSent: Date = .distantPast
    private var sending = false
    private var pending: CLLocation?

    static let providers = ["gps", "network", "fused"]

    init(shell: @escaping @MainActor (String) async -> Bool) {
        self.shell = shell
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBest
        manager.distanceFilter = 5
    }

    /// Starts or stops sharing. Called when the guest has booted and when
    /// the switch or the app's state changes.
    func update(enabled: Bool, guestReady: Bool) {
        let want = enabled && guestReady
        guard want != running else { return }
        running = want
        if want {
            if manager.authorizationStatus == .notDetermined { manager.requestWhenInUseAuthorization() }
            manager.startUpdatingLocation()
        } else {
            manager.stopUpdatingLocation()
            if providersReady {
                providersReady = false
                let command = Self.providers.map { "cmd location providers remove-test-provider \($0)" }
                    .joined(separator: "; ")
                Task { _ = await shell(command + "; echo ok") }
            }
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let location = locations.last else { return }
        Task { @MainActor in self.received(location) }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: any Error) {
        print("Andarium: location: \(error.localizedDescription)")
    }

    private func received(_ location: CLLocation) {
        guard running else { return }
        pending = location
        // One adb session at a time, at most one fix a second.
        guard !sending, Date().timeIntervalSince(lastSent) >= 1 else { return }
        Task { await flush() }
    }

    private func flush() async {
        while running, let location = pending {
            sending = true
            pending = nil
            if !providersReady { providersReady = await shell(Self.setUpCommand) }
            if providersReady {
                _ = await shell(Self.locationCommand(location))
                lastSent = Date()
            }
            sending = false
            if pending != nil { try? await Task.sleep(for: .seconds(1)) }
        }
    }

    static var setUpCommand: String {
        var parts = [
            "appops set com.android.shell android:mock_location allow",
            "cmd location set-location-enabled true",
        ]
        for provider in providers {
            parts.append("cmd location providers add-test-provider \(provider) --supportsAltitude --supportsSpeed --supportsBearing")
            parts.append("cmd location providers set-test-provider-enabled \(provider) true")
        }
        return parts.joined(separator: "; ") + "; echo ok"
    }

    static func locationCommand(_ location: CLLocation) -> String {
        let c = location.coordinate
        let accuracy = max(1, Int(location.horizontalAccuracy.rounded()))
        let fix = String(format: "%.7f,%.7f", c.latitude, c.longitude)
        return providers.map { "cmd location providers set-test-provider-location \($0) --location \(fix) --accuracy \(accuracy)" }
            .joined(separator: "; ") + "; echo ok"
    }
}
