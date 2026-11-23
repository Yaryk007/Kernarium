// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The words that the frame rate is lowered while the device is hot, over the
/// top of the session, in the shape of the graphics notice: it takes touches
/// only where it is drawn, and it goes when the person dismisses it or after
/// `ThermalNotice.visibleFor`. The words, with a chevron, are a button that
/// asks to turn the frame rate limit off (`ask`); Dismiss only dismisses.
struct ThermalNoticeBanner: View {
    let ask: () -> Void
    let dismiss: () -> Void

    var body: some View {
        // The capsule is as wide as its words when they fit on one line, so a
        // short text in any language is not lost in a wide one; a longer text
        // wraps inside the cap.
        ViewThatFits(in: .horizontal) {
            banner(oneLine: true)
            banner(oneLine: false)
        }
        .frame(maxWidth: 640)
        .padding(.horizontal, 24)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("thermal-notice")
    }

    private func banner(oneLine: Bool) -> some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: "thermometer.high")
                .font(.title3)
                .foregroundStyle(Color.orange)
                .accessibilityHidden(true)
            Button(action: ask) {
                HStack(spacing: 6) {
                    Text(ThermalNotice.text)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.primary)
                        .fixedSize(horizontal: oneLine, vertical: true)
                    Image(systemName: "chevron.right")
                        .font(.footnote.weight(.bold))
                        .foregroundStyle(.secondary)
                        .accessibilityHidden(true)
                }
                .contentShape(.rect)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(verbatim: ThermalNotice.text))
            .accessibilityHint("Asks to turn off the frame rate limit")
            .accessibilityIdentifier("thermal-notice-ask")
            Button(ThermalNotice.dismissLabel, action: dismiss)
                .buttonStyle(.bordered)
                .controlSize(.small)
                .accessibilityIdentifier("thermal-notice-dismiss")
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }
}
