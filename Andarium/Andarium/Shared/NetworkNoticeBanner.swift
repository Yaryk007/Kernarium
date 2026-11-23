// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words of the notice that iOS denies Andarium the network, on Home and
/// in a session. They are one place so the text can change without touching
/// the logic. Android shows having no network at all by itself; this is only
/// for a network that iOS holds back from Andarium.
enum NetworkWords {
    static let settingsLabel = String(
        localized: "Open Settings",
        comment: "A button that opens Andarium's page in the iOS Settings app.")
    static let settingsHint = String(
        localized: "Opens Andarium\u{2019}s page in Settings.",
        comment: "VoiceOver hint of the network notice's Open Settings button.")
    static let dismissLabel = String(
        localized: "Dismiss", comment: "A button that closes a notice; VoiceOver reads it too.")
    /// The name with a non-breaking hyphen, so a line never ends in "Wi-".
    static let wifiName = "Wi\u{2011}Fi"

    static func title(for denial: NetworkDenial) -> String {
        switch denial {
        case .cellular:
            String(
                localized: "Cellular data is off for Andarium",
                comment: "Title of the notice shown when iOS blocks Andarium's cellular data. Cellular Data is iOS's own name of the setting.")
        case .wifi:
            String(
                localized: "\(wifiName) is off for Andarium",
                comment: "Title of the notice shown when iOS blocks Andarium's Wi-Fi. The argument is the name Wi-Fi, written with a non-breaking hyphen.")
        }
    }

    /// Where to allow it, which Open Settings goes to.
    static func text(for denial: NetworkDenial) -> String {
        switch denial {
        case .cellular:
            String(
                localized: "Android can\u{2019}t reach the internet over cellular data. Turn on Cellular Data for Andarium in Settings.",
                comment: "Text of the notice shown when iOS blocks Andarium's cellular data. Cellular Data is the name of the switch under Andarium in the iOS Settings app.")
        case .wifi:
            String(
                localized: "Android can\u{2019}t reach the internet over \(wifiName). Allow wireless data for Andarium in Settings.",
                comment: "Text of the notice shown when iOS blocks Andarium's Wi-Fi. The argument is the name Wi-Fi. Wireless Data is the name of the setting under Andarium in the iOS Settings app.")
        }
    }

    static func symbol(for denial: NetworkDenial) -> String {
        switch denial {
        case .cellular: "antenna.radiowaves.left.and.right.slash"
        case .wifi: "wifi.slash"
        }
    }
}

/// The notice over the top of the session, in the shape of the graphics
/// notice with the way out added: it takes touches only where it is drawn, and
/// it goes when the person dismisses it or the network is allowed again.
struct NetworkNoticeBanner: View {
    let denial: NetworkDenial
    let openSettings: () -> Void
    let dismiss: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: NetworkWords.symbol(for: denial))
                .font(.title3)
                .foregroundStyle(Color.orange)
                .frame(width: 28)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(NetworkWords.title(for: denial))
                        .font(.subheadline.weight(.semibold))
                    Text(NetworkWords.text(for: denial).keepingTogether)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack(spacing: 8) {
                    Button(NetworkWords.settingsLabel, action: openSettings)
                        .buttonStyle(.borderedProminent)
                        .accessibilityHint(NetworkWords.settingsHint)
                        .accessibilityIdentifier("network-notice-settings")
                    Button(NetworkWords.dismissLabel, action: dismiss)
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("network-notice-dismiss")
                }
                .controlSize(.small)
            }
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .frame(maxWidth: 480, alignment: .leading)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .padding(.horizontal, 24)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("network-notice")
    }
}
