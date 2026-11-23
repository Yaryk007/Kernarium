// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The notice on the boot screen that the virtual machine's Android has no
/// Google apps, on purpose, and why: one panel last in the boot's column,
/// below its own notes, while the person waits for Android's first screen. It
/// has no control and goes with the boot screen. Which start shows it is
/// `NoGoogleNotice.showsOnBoot`'s (a new installation's first Android boot);
/// this one's words are the only place in the application that names Google
/// (`scripts/localization.py` holds the exception to its rule). VoiceOver
/// reads the title and then the text as one element.
struct NoGoogleBootNote: View {
    static let title = String(localized: "No Google apps")
    static let text = String(
        localized: "Google licenses its apps and Google Play services only to makers of certified devices. Install apps from files you have; many work without them.")

    var body: some View {
        Label {
            VStack(alignment: .leading, spacing: 3) {
                Text(Self.title.keepingTogether)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.primary)
                Text(Self.text.keepingTogether)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } icon: {
            Image(systemName: "shippingbox.fill")
                .foregroundStyle(Color.accentColor)
        }
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("no-google-boot-note")
    }
}
