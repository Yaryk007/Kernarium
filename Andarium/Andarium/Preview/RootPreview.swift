// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The preview's states of the root switch: Home with the row off, on and
/// paused, the warning that asks before it turns on, the cards after a start
/// that did not finish, and the boot screen's step. The mock release carries root only in these
/// screens, so every other screen is what it was without the feature.
@MainActor
enum RootPreview {
    /// Home's screens, which scroll to the Root section.
    static let scrollTargets: [String: String] = [
        "home-root": "root", "home-root-on": "root", "home-root-warning": "root", "home-root-paused": "root",
        "home-root-app-alert": "root",
    ]

    /// Info's screens of a copy whose release carries root: Acknowledgements
    /// with Magisk and BusyBox, and Magisk's page.
    static let infoScreens: Set<String> = ["info-acknowledgements-root", "info-license-magisk"]

    static func handles(_ screen: String) -> Bool {
        screen.hasPrefix("home-root") || screen.hasPrefix("boot-root") || infoScreens.contains(screen)
    }

    static func apply(screen: String, to model: AppModel, freeze: Bool) {
        guard handles(screen) else { return }
        model.rootAvailable = true
        switch screen {
        case "home-root-on":
            model.guest?.root.setOn(true)
        case "home-root-warning":
            // The warning is asked for once the screen is up, as a tap on the
            // switch asks: one asked for in the first frame is presented before
            // the window has its scene.
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(1800))
                model.setRoot(true)
            }
        case "home-root-paused":
            model.guest?.root.setOn(true)
            model.guest?.root.attention = .failedStart
        case "home-root-notice-paused":
            model.guest?.root.setOn(true)
            model.guest?.root.attention = .failedStart
        case "home-root-notice-turned-off":
            model.guest?.root.attention = .turnedOffAfterFailure
        case "home-root-notice-stopped":
            model.guest?.root.setOn(true)
            model.guest?.root.attention = .stoppedStart
        case "home-root-app-alert":
            // The alert a start with root leaves when the Magisk app could not be put in.
            model.guest?.root.setOn(true)
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(1800))
                model.alert = AppAlert(title: RootWords.appFailedTitle, message: RootWords.appFailedMessage)
            }
        case "boot-root":
            // The boot screen of a start with root, at its fifth step: the app is being put in.
            model.guest?.root.setOn(true)
            model.guest?.root.startRooted = true
            model.rootAppInstalling = true
            model.frozenBootStage = freeze ? BootSteps.rootAppStep : nil
            model.frozenElapsed = 11
            model.phase = .booting(.normal)
        default:
            break
        }
    }
}
