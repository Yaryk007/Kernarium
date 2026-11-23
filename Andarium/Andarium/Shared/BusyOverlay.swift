// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Covers the screens while a long task runs, so nothing else is asked of the
/// guest's files meanwhile: a glass card with what is happening and how far.
/// When the task is over and has a result, the card stays and says how it
/// ended until the person closes it.
struct BusyOverlay: View {
    let state: BusyState
    /// The card is wider where the screen is, so a longer text in another
    /// language wraps no more than the English does.
    @Environment(\.horizontalSizeClass) private var sizeClass

    var body: some View {
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            // The card is as tall as its text. Where the text does not fit the screen (a long text in another
            // language) the text scrolls between the title and the button, so that the button stays on the
            // screen and the card can be closed.
            ViewThatFits(in: .vertical) {
                content(scrolls: false)
                content(scrolls: true)
            }
            .padding(20)
            .frame(maxWidth: sizeClass == .regular ? 520 : 360)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
            .onCardHalf()
        }
        .transition(.opacity)
        .animation(.easeInOut(duration: 0.2), value: state.cancel != nil)
        .accessibilityElement(children: .contain)
        // The card is what the person is to deal with: VoiceOver reaches nothing behind it, and says what is
        // happening when it appears, for a task (a copy of a large file) that can take minutes.
        .accessibilityAddTraits(.isModal)
        .onAppear { AccessibilityNotification.Announcement(state.title).post() }
        .onChange(of: state.result != nil) { _, ended in
            // How it ended, in place of what it was doing.
            if ended { VoiceOver.screenChanged("\(state.title). \(state.detail)") }
        }
    }

    private func content(scrolls: Bool) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            if let result = state.result {
                ended(result, scrolls: scrolls)
            } else {
                running(scrolls: scrolls)
            }
        }
    }

    /// The card's text: whole, or in a scroll view that takes the height the screen leaves.
    @ViewBuilder
    private func detail(scrolls: Bool) -> some View {
        let text = Text(state.detail.keepingTogether)
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
        if scrolls {
            ScrollView { text.frame(maxWidth: .infinity, alignment: .leading) }
                .scrollBounceBehavior(.basedOnSize)
        } else {
            text
        }
    }

    @ViewBuilder
    private func running(scrolls: Bool) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(state.title.keepingTogether)
                .font(.title3.weight(.bold))
                .accessibilityAddTraits(.isHeader)
            Spacer()
            if let progress = state.progress {
                Text("\(Int((progress * 100).rounded()))%")
                    .font(.system(.body, design: .rounded).weight(.semibold))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
                    // The bar says it.
                    .accessibilityHidden(true)
            }
        }
        ProgressBar(value: state.progress ?? 0, label: state.title)
        detail(scrolls: scrolls)
        if let cancel = state.cancel {
            Button("Cancel", role: .cancel, action: cancel.action)
                .buttonStyle(.bordered)
                .frame(maxWidth: .infinity, alignment: .trailing)
                .transition(.opacity)
        }
    }

    /// A check for what was done, the app's caution symbol in its caution colour for what was done in part,
    /// and the same symbol in red for what was not done.
    static func symbol(for style: BusyResult.Style) -> String {
        style == .success ? "checkmark.circle.fill" : "exclamationmark.triangle.fill"
    }

    static func color(for style: BusyResult.Style) -> Color {
        switch style {
        case .success: Color.accentColor
        case .warning: Color.orange
        case .failure: Color.red
        }
    }

    @ViewBuilder
    private func ended(_ result: BusyResult, scrolls: Bool) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            // The symbol is decorative: the title says it, as the app's other cautions do.
            Image(systemName: Self.symbol(for: result.style))
                .font(.title3)
                .foregroundStyle(Self.color(for: result.style))
                .accessibilityHidden(true)
            Text(state.title.keepingTogether)
                .font(.title3.weight(.bold))
                .accessibilityAddTraits(.isHeader)
        }
        detail(scrolls: scrolls)
        Button(result.failed ? "OK" : "Done", action: result.dismiss.action)
            .buttonStyle(.borderedProminent)
            .frame(maxWidth: .infinity, alignment: .trailing)
            .accessibilityIdentifier("busy-result-dismiss")
    }
}
