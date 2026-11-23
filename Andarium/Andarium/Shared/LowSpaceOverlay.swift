// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Covers the session while the guest is paused because the device is nearly
/// out of space: what is the matter, what to do, and the ways on. While it
/// pauses the guest the screen offers nothing; over a guest that holds still
/// it offers Resume, which reads the space again, and Shut down, which has
/// Android write what it has and power off. Over a guest that did not hold
/// still it says so, and offers Shut down and Check again, never a resume.
struct LowSpaceOverlay: View {
    let pause: SpacePause
    let resume: () -> Void
    let shutDown: () -> Void
    let checkAgain: () -> Void
    /// Wider where the screen is, as the other cards over the screens.
    @Environment(\.horizontalSizeClass) private var sizeClass

    var body: some View {
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.title3)
                        .foregroundStyle(Color.orange)
                        .accessibilityHidden(true)
                    Text(SpacePauseWords.title(pause).keepingTogether)
                        .font(.title3.weight(.bold))
                        .accessibilityAddTraits(.isHeader)
                }
                Text(SpacePauseWords.detail(pause).keepingTogether)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("space-pause-detail")
                buttons
            }
            .padding(20)
            .frame(maxWidth: sizeClass == .regular ? 520 : 380)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
            .onCardHalf()
        }
        .transition(.opacity)
        .accessibilityElement(children: .contain)
        .accessibilityAddTraits(.isModal)
        .accessibilityIdentifier("space-pause")
        .onAppear { VoiceOver.screenChanged(Self.spoken(pause)) }
    }

    /// What VoiceOver says as the screen appears: the words the screen shows.
    /// A test asks for a language by `locale`.
    static func spoken(_ pause: SpacePause, locale: Locale = .current) -> String {
        "\(SpacePauseWords.title(pause, locale: locale)). \(SpacePauseWords.detail(pause, locale: locale))"
    }

    @ViewBuilder
    private var buttons: some View {
        if let working = pause.working {
            progress(working == .resuming ? SpacePauseWords.resuming : SpacePauseWords.shuttingDown)
        } else {
            switch pause.stage {
            case .pausing:
                progress(SpacePauseWords.pausing)
            case .paused:
                HStack(spacing: 10) {
                    Spacer()
                    Button("Shut down", action: shutDown)
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("space-pause-shutdown")
                    Button("Resume", action: resume)
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("space-pause-resume")
                }
            case .notPaused:
                HStack(spacing: 10) {
                    Spacer()
                    Button("Check again", action: checkAgain)
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("space-pause-check-again")
                    Button("Shut down", action: shutDown)
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("space-pause-shutdown")
                }
            }
        }
    }

    private func progress(_ text: String) -> some View {
        HStack(spacing: 10) {
            ProgressView()
            Text(text)
                .font(.subheadline.weight(.semibold))
        }
        .frame(maxWidth: .infinity, alignment: .trailing)
    }
}
