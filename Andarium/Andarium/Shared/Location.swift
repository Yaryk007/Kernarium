// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import CoreLocation
import Foundation

/// What the system says about Andarium's use of the device's location, and the
/// one call that can ask. The device application and the Simulator preview
/// read iOS (`SystemLocationAccess`); a test gives its own.
@MainActor
protocol LocationAccessSource: AnyObject {
    /// The access now. Reading it never shows a prompt.
    var state: LocationAuthorization { get }
    /// Asks the person, which iOS does only while they have not answered
    /// before. The answer arrives through `changed`.
    func request()
    /// Runs when iOS changes the access: the person answered the question,
    /// changed Andarium's Location setting or the device's Location Services,
    /// or Precise Location.
    var changed: (@MainActor () -> Void)? { get set }
}

/// The Location setting's conversation with the system, for the device
/// application and the Simulator preview alike: turning it on asks iOS at
/// once, the first time; an answer, or access the person gave or took away in
/// Settings, goes to the model, which keeps the switch; and the guest hears a
/// position only while the switch shows on and iOS allows it
/// (`LocationSession`).
@MainActor
final class LocationController {
    private weak var model: AppModel?
    private let source: any LocationAccessSource
    private let applied: @MainActor () -> Void

    /// `applied` runs after every change of the switch or of the access, so
    /// the one who runs the guest can set its session to match.
    init(model: AppModel, source: any LocationAccessSource, applied: @escaping @MainActor () -> Void) {
        self.model = model
        self.source = source
        self.applied = applied
        model.locationAuthorization = source.state
        source.changed = { [weak self] in self?.refresh() }
    }

    /// The switch changed, and the model has stored the choice.
    func setOn(_ on: Bool) {
        let access = source.state
        model?.locationAccessChanged(access)
        if on, access == .notDetermined { source.request() }
        applied()
    }

    /// Looks at the permission again: at launch and whenever Andarium becomes
    /// active, since the person may have changed it in Settings.
    func refresh() {
        model?.locationAccessChanged(source.state)
        applied()
    }
}

/// The words of the Location setting: the row's detail line and the alerts.
/// They are one place so the text can change without touching the logic.
enum LocationWords {
    static let title = String(localized: "Location")
    static let denied = String(localized: "Not allowed in Settings")
    static let restricted = String(localized: "Restricted on this device")
    static let servicesOff = String(localized: "Location Services are off")
    static let approximate = String(localized: "Approximate location")

    /// The line under Location: why it cannot be used, or that it is
    /// approximate while it is on; none otherwise.
    static func detail(for access: LocationAuthorization, shownOn: Bool) -> String? {
        switch access {
        case .denied: denied
        case .restricted: restricted
        case .servicesOff: servicesOff
        case .approximate: shownOn ? approximate : nil
        case .precise, .notDetermined: nil
        }
    }

    /// What VoiceOver adds to the row.
    static func hint(for access: LocationAuthorization, shownOn: Bool) -> String {
        switch access {
        case .denied:
            String(localized: "Andarium is not allowed to use the location. Turning this on offers to open Settings.")
        case .servicesOff:
            String(localized: "Location Services are off on this device. Turning this on explains how to turn them on.")
        case .restricted:
            String(localized: "The location is restricted on this device. Turning this on explains why.")
        case .approximate where shownOn:
            String(localized: "iOS gives only an approximate location. Activate the line under the title to see why.")
        case .approximate, .precise, .notDetermined:
            ""
        }
    }

    /// An alert about the location: its title and message, and whether
    /// Settings has the way out.
    struct Alert: Equatable {
        var title: String
        var message: String
        var offersSettings: Bool
    }

    /// The alert for turning Location on while iOS refuses it, or for the
    /// tap on Approximate; nil for any other access.
    static func alert(for access: LocationAuthorization) -> Alert? {
        switch access {
        case .denied:
            Alert(
                title: String(localized: "Allow location access"),
                message: String(localized: "iOS is not allowing Andarium to use this device\u{2019}s location. Turn on Location for Andarium in Settings, then come back to use it."),
                offersSettings: true)
        case .servicesOff:
            Alert(
                title: String(localized: "Location Services are off"),
                message: String(localized: "Location Services are turned off on this device. Turn them on in Settings, under Privacy & Security, then come back to use Location."),
                offersSettings: true)
        case .restricted:
            Alert(
                title: String(localized: "Location is restricted"),
                message: String(localized: "This device\u{2019}s settings, such as Screen Time or a profile, do not let apps use the location, so Andarium cannot share it."),
                offersSettings: false)
        case .approximate:
            Alert(
                title: String(localized: "Approximate location"),
                message: String(localized: "iOS is giving Andarium only the area you are in, because Precise Location is off for it. Apps in the virtual machine see a wide area. To share the exact position, turn on Precise Location for Andarium in Settings."),
                offersSettings: true)
        case .precise, .notDetermined:
            nil
        }
    }
}

extension LocationAuthorization {
    /// What Core Location's three answers make of the access: Location
    /// Services off for the whole device dominates what the app was given
    /// (restricted excepted, which is the same wherever it is read), and a
    /// reduced accuracy makes allowed access approximate.
    init(status: CLAuthorizationStatus, accuracy: CLAccuracyAuthorization, servicesEnabled: Bool) {
        if status == .restricted {
            self = .restricted
        } else if !servicesEnabled {
            self = .servicesOff
        } else {
            switch status {
            case .notDetermined: self = .notDetermined
            case .authorizedAlways, .authorizedWhenInUse: self = accuracy == .reducedAccuracy ? .approximate : .precise
            default: self = .denied
            }
        }
    }
}
