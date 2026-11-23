// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Rises over Android's screen while VoiceOver runs and Android's screen reader
/// is off: it says that VoiceOver cannot read Android's screen, what the
/// screen reader does instead and how to come back to VoiceOver, and asks.
/// Where the system image has no screen reader it says so, once.
struct ScreenReaderCard: View {
    @Environment(AppModel.self) private var model
    @Environment(\.horizontalSizeClass) private var sizeClass
    let card: ScreenReaderSession.Card
    /// VoiceOver's focus on the title: the card is what the person is to deal
    /// with, so it takes the focus as it appears, where a screen change posted
    /// before it was on screen left the cursor on the display.
    @AccessibilityFocusState private var titleFocused: Bool

    var body: some View {
        let reader = model.session.reader
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                switch card {
                case .ask: ask(reader)
                case .unavailable: unavailable
                }
            }
            .padding(20)
            .frame(maxWidth: sizeClass == .regular ? 520 : 380)
            // Glass alone shows whatever Android draws behind it: over a light
            // screen the body measured 4.05:1 in dark mode. The opaque backing
            // the app's other cards use keeps the words readable over any screen.
            .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 26))
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
            .onCardHalf()
        }
        .transition(.opacity)
        // The card is what the person is to deal with: VoiceOver reaches
        // nothing behind it, and lands on its title.
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
        .accessibilityIdentifier("screen-reader-card")
        .task {
            // The focus can move only to an element VoiceOver has been told of.
            try? await Task.sleep(for: .milliseconds(300))
            titleFocused = true
        }
    }

    private func title(_ text: String) -> some View {
        Text(text.keepingTogether)
            .font(.title3.weight(.bold))
            .accessibilityAddTraits(.isHeader)
            .accessibilityFocused($titleFocused)
    }

    /// The card's text is primary: it is read over a screen the person cannot
    /// choose, which secondary text on the glass does not hold against.
    private func body(_ text: String) -> some View {
        Text(text.keepingTogether)
            .font(.subheadline)
            .foregroundStyle(.primary)
            .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder
    private func ask(_ reader: ScreenReaderSession) -> some View {
        title(ScreenReaderWords.askTitle)
        body(ScreenReaderWords.cantRead)
        body(ScreenReaderWords.canRead)
        body(ScreenReaderWords.wayBack(model.session.keysEdge))
        if let reason = reader.cardFailure {
            // The reason in primary text, with the warning symbol beside it:
            // red text on the glass does not hold its contrast.
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.red)
                    .accessibilityHidden(true)
                Text(reason.keepingTogether)
                    .foregroundStyle(.primary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("screen-reader-card-failure")
            }
            .font(.subheadline)
        }
        HStack(spacing: 10) {
            Spacer()
            Button(ScreenReaderWords.notNow) { model.declineScreenReaderCard() }
                .buttonStyle(.bordered)
                .tint(.primary)
                .disabled(reader.changing != nil)
                .accessibilityIdentifier("screen-reader-card-not-now")
            turnOn(reader)
        }
    }

    /// Turn On, Try Again after a failure, and "Turning on…" while the change
    /// runs. The button is not disabled for that: a disabled button dims its
    /// label out of reach of the contrast, and a press while it runs says
    /// "Turning on…" and starts nothing (`AppModel.turnOnScreenReaderFromCard`).
    @ViewBuilder
    private func turnOn(_ reader: ScreenReaderSession) -> some View {
        let button = Button {
            model.turnOnScreenReaderFromCard()
        } label: {
            if reader.changing == true {
                HStack(spacing: 8) {
                    ProgressView()
                    // One line, shrunk where a language runs long (Japanese).
                    Text(ScreenReaderWords.turningOn)
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                }
            } else {
                Text(reader.cardFailure != nil ? ScreenReaderWords.tryAgain : ScreenReaderWords.turnOn)
            }
        }
        if reader.changing == true {
            button.buttonStyle(.bordered).tint(.primary).accessibilityIdentifier("screen-reader-card-turn-on")
        } else {
            button.buttonStyle(.borderedProminent).accessibilityIdentifier("screen-reader-card-turn-on")
        }
    }

    @ViewBuilder
    private var unavailable: some View {
        title(ScreenReaderWords.cantRead)
        body(ScreenReaderWords.noReader)
        HStack {
            Spacer()
            Button(ScreenReaderWords.ok) { model.dismissScreenReaderCard() }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("screen-reader-card-ok")
        }
    }
}
