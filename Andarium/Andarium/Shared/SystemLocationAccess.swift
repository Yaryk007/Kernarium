// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreLocation
import Foundation

/// What iOS says about Andarium's location access, through the process's one
/// location manager. The device application's `HostLocation` reads positions
/// through it as well; the Simulator preview uses it alone, to run the
/// Location switch against the Simulator's own permission.
///
/// Creating the manager, reading the access and asking never use the
/// location, and only a call to `request` can show iOS's question.
@MainActor
final class SystemLocationAccess: NSObject, CLLocationManagerDelegate, LocationAccessSource {
    let manager = CLLocationManager()
    /// The device's Location Services setting as last read. It is read off the
    /// main thread (the call can block), so it starts as "on" and corrects
    /// itself within a moment.
    private var servicesEnabled = true
    var changed: (@MainActor () -> Void)?
    /// The positions the manager delivers once something has started it, in
    /// the order they were read. Nothing starts it here.
    var delivered: (@MainActor ([CLLocation]) -> Void)?

    override init() {
        super.init()
        manager.delegate = self
        readServices()
    }

    var state: LocationAuthorization {
        LocationAuthorization(
            status: manager.authorizationStatus, accuracy: manager.accuracyAuthorization,
            servicesEnabled: servicesEnabled)
    }

    func request() {
        guard manager.authorizationStatus == .notDetermined else { return }
        manager.requestWhenInUseAuthorization()
    }

    private func readServices() {
        Task.detached(priority: .utility) {
            let enabled = CLLocationManager.locationServicesEnabled()
            await MainActor.run { [weak self] in
                guard let self, enabled != servicesEnabled else { return }
                servicesEnabled = enabled
                changed?()
            }
        }
    }

    // MARK: Core Location

    /// Called on the main thread, where the manager was made: when it is
    /// first given its delegate, and whenever the access or the accuracy the
    /// person allows changes.
    nonisolated func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        MainActor.assumeIsolated {
            readServices()
            changed?()
        }
    }

    nonisolated func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        MainActor.assumeIsolated { delivered?(locations) }
    }

    /// A position that could not be read is no change to what the guest is
    /// told: the receiver reports no fix when it has none to repeat, and the
    /// access, if that is why, arrives through `changed`.
    nonisolated func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {}
}
