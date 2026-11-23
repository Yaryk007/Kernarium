// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The words of Android's screen reader in the session's Settings, on the card
/// that asks about it and on Android's display. "TalkBack" is the title of the
/// control and of the card, which a person looks for by that name; every
/// explanation says "Android's screen reader". Each text is the one shown
/// and the one VoiceOver speaks.
nonisolated enum ScreenReaderWords {
    // MARK: Settings

    static var section: String { String(localized: "Accessibility") }
    /// The control's and the card's title.
    static var name: String { String(localized: "TalkBack") }
    static var androidSettings: String { String(localized: "Android\u{2019}s accessibility settings") }
    /// What the screen reader does, what changes for VoiceOver while it is on,
    /// and the way back to the keys' edge.
    static func footer(_ edge: KeysEdge?) -> String {
        sentences(
            String(localized: "Android\u{2019}s screen reader speaks what you touch on Android\u{2019}s screen. While it is on, VoiceOver is silent there and your touches go to the screen reader."),
            wayBack(edge))
    }
    static var footerUnavailable: String { noReader }

    // MARK: The line under the switch

    static var checking: String { String(localized: "Checking\u{2026}") }
    static var couldntCheck: String { String(localized: "Couldn\u{2019}t check") }
    static var notInImage: String { String(localized: "Not in this system image") }
    static var waitsForTransfer: String { String(localized: "Available when the file transfer ends") }
    static var turningOn: String { String(localized: "Turning on\u{2026}") }
    static var turningOff: String { String(localized: "Turning off\u{2026}") }
    static var tryAgain: String { String(localized: "Try Again") }
    static var noReader: String { String(localized: "This system image has no screen reader for Android.") }
    static var notReady: String { String(localized: "The virtual machine isn\u{2019}t ready yet. Try again in a moment.") }

    static func didntChange(turningOn on: Bool) -> String {
        on ? String(localized: "Didn\u{2019}t turn on") : String(localized: "Didn\u{2019}t turn off")
    }

    // MARK: What VoiceOver says of a change

    /// What VoiceOver says when the screen reader is on (or has been found on):
    /// that it is, and how to come back to VoiceOver.
    static func announcement(on: Bool, edge: KeysEdge?) -> String {
        on ? sentences(String(localized: "Android\u{2019}s screen reader is on."), wayBack(edge))
            : String(localized: "Android\u{2019}s screen reader is off.")
    }

    /// The failure as it is said: what did not happen, then why.
    static func changeFailure(turningOn on: Bool, reason: String) -> String {
        let title = on ? String(localized: "Android\u{2019}s screen reader didn\u{2019}t turn on.")
            : String(localized: "Android\u{2019}s screen reader didn\u{2019}t turn off.")
        return "\(title) \(reason)"
    }

    // MARK: The card

    static var askTitle: String { String(localized: "Turn on TalkBack?") }
    static var cantRead: String { String(localized: "VoiceOver can\u{2019}t read Android\u{2019}s screen.") }
    static var canRead: String {
        String(localized: "Android\u{2019}s screen reader can: it speaks in its own voice, and your touches on Android\u{2019}s screen go to it.")
    }
    /// How to come back to VoiceOver: the side keys take VoiceOver's touches
    /// even over a direct-touch display. Where they are on an edge it is named,
    /// since a person finds them by sweeping the border.
    static func wayBack(_ edge: KeysEdge?) -> String {
        switch edge {
        case .right: String(localized: "To come back to VoiceOver, touch the side keys on the right edge.")
        case .left: String(localized: "To come back to VoiceOver, touch the side keys on the left edge.")
        case nil: String(localized: "To come back to VoiceOver, touch the side keys.")
        }
    }
    /// Two sentences as one text, joined as the language joins sentences.
    static func sentences(_ first: String, _ second: String) -> String {
        String(localized: "\(first) \(second)")
    }
    static var turnOn: String { String(localized: "Turn On") }
    static var notNow: String { String(localized: "Not Now") }
    static var ok: String { String(localized: "OK") }

    // MARK: Android's screen under VoiceOver

    static var displayLabel: String { String(localized: "Android screen") }
    static var displayHintDirect: String { String(localized: "Touches go to Android.") }
    static var displayHintOff: String {
        String(localized: "Open Settings from the side keys to turn on Android\u{2019}s screen reader.")
    }

    /// What the display says of the screen reader, from the session's state.
    @MainActor static func displayValue(_ reader: ScreenReaderSession) -> String {
        if let on = reader.changing {
            return on ? String(localized: "Turning on Android\u{2019}s screen reader\u{2026}")
                : String(localized: "Turning off Android\u{2019}s screen reader\u{2026}")
        }
        guard let state = reader.state else {
            return reader.readFailed ? String(localized: "Couldn\u{2019}t check Android\u{2019}s screen reader")
                : String(localized: "Checking Android\u{2019}s screen reader\u{2026}")
        }
        if !state.isInstalled { return String(localized: "No screen reader for Android in this system image") }
        return state.isOn ? String(localized: "Android\u{2019}s screen reader on")
            : String(localized: "Android\u{2019}s screen reader off")
    }

    /// The display's hint: what touching it does now and how to come back to
    /// VoiceOver, or where to turn the screen reader on; nothing where the
    /// image has none.
    @MainActor static func displayHint(_ reader: ScreenReaderSession, edge: KeysEdge?) -> String? {
        guard let state = reader.state, state.isInstalled else { return nil }
        return state.isOn ? sentences(displayHintDirect, wayBack(edge)) : displayHintOff
    }
}
