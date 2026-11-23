// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Every word of the root switch, in one place. The UI says "Android" or
/// "virtual machine", and names Magisk because the person grants root there.
enum RootWords {
    // MARK: Home

    static let sectionTitle: LocalizedStringKey = "Root"
    static let rowTitle = String(localized: "Root access")
    static let note = String(localized: "Apps you allow in Magisk can run as root. Takes effect at the next start.")
    /// The line under the switch while a failed start has paused root.
    static let paused = String(localized: "Paused after a failed start")

    // MARK: The warning before root is turned on

    static let warningTitle = String(localized: "Turn on root access?")
    static let warningBody = String(
        localized:
            "Apps you allow in Magisk can read and change everything in the virtual machine, including other apps’ data. Some apps don’t run on a rooted device. The official Magisk app is installed at the next start. You can turn root off whenever the virtual machine is stopped."
    )

    // MARK: The boot screen's Magisk app step

    static let stepTitle = String(localized: "Magisk app")
    static let stepDetail = String(localized: "Installs the official Magisk app")
    static let appFailedTitle = String(localized: "Magisk app not installed")
    static let appFailedMessage = String(
        localized:
            "The Magisk app couldn’t be installed. Root is on, but no app can be given root until it is. Andarium tries again at the next start."
    )

    // MARK: Home's cards after a start with root that did not finish

    static let pausedTitle = String(localized: "Root is paused")
    static let pausedText = String(
        localized:
            "Android did not finish starting with root, so it starts without root until you choose. Try root again, or turn it off."
    )
    static let tryAgain = String(localized: "Try with root again")
    static let turnOff = String(localized: "Turn off root")
    static let turnedOffTitle = String(localized: "Root turned off")
    static let turnedOffText = String(
        localized:
            "Android did not finish starting with root, so root was turned off. You can turn it on again under Root."
    )
    static let stoppedTitle = String(localized: "Start was stopped")
    static let stoppedText = String(
        localized:
            "You stopped a start with root before Android finished starting. If root is the problem, start once without it."
    )
    static let startWithoutRoot = String(localized: "Start without root")
}
