// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The ribbon in Home's Start bar, just above Start, after an update with
/// notes: the new features' tiles and names on one line of glass tinted with
/// the accent, so it reads with Start rather than with the gray sections
/// above. Its sparkle bounces once, the first time Home shows it in a launch.
/// A tap opens the notes; the close button, or a swipe down, puts it away.
/// Start stays where it always is and is never covered.
struct WhatsNewRibbon: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorScheme) private var colorScheme

    let entries: [WhatsNewEntry]
    let open: () -> Void
    let close: () -> Void

    /// How far a swipe has pulled the ribbon down.
    @State private var pull: CGFloat = 0
    /// The sparkle bounces once, on arrival.
    @State private var sparkle = 0
    /// The ribbon has arrived in this launch; the sparkle does not bounce again.
    @MainActor private static var arrived = false
    /// The close button's circle, which grows with the text.
    @ScaledMetric(relativeTo: .footnote) private var closeSize: CGFloat = 30

    /// A swipe further down than this puts the ribbon away.
    private static let closingPull: CGFloat = 36

    var body: some View {
        HStack(spacing: 10) {
            Button(action: open) {
                HStack(spacing: 12) {
                    WhatsNewDeck(symbol: entries.first?.symbol ?? "sparkles", count: entries.count)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 1) {
                        HStack(spacing: 5) {
                            Image(systemName: "sparkles")
                                .foregroundStyle(Color.accentColor)
                                .symbolEffect(.bounce, value: sparkle)
                            Text("What\u{2019}s New")
                                .foregroundStyle(.primary)
                        }
                        .font(.subheadline.weight(.semibold))
                        summary
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("What\u{2019}s New")
            .accessibilityValue(ListWords.join(entries.map { WhatsNewWords.localized($0.title) }))
            .accessibilityAddTraits(.isButton)
            .accessibilityAction(named: "Close", close)
            .accessibilityIdentifier("whats-new-ribbon")

            Button(action: close) {
                Image(systemName: "xmark")
                    .font(.footnote.weight(.bold))
                    .foregroundStyle(.secondary)
                    .frame(width: closeSize, height: closeSize)
                    .background(Color(.tertiarySystemFill), in: .circle)
                    .contentShape(.circle)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close What\u{2019}s New")
            .accessibilityIdentifier("whats-new-close")
        }
        .padding(.leading, 10)
        .padding(.trailing, 10)
        .padding(.vertical, 9)
        .frame(minHeight: 52)
        // On a dark ground the tint needs more of the accent to read as jade.
        .glassEffect(
            .regular.tint(Color.accentColor.opacity(colorScheme == .dark ? 0.42 : 0.24)).interactive(), in: .capsule)
        .offset(y: pull)
        // A swipe that moves the ribbon is not also a tap on it.
        .highPriorityGesture(swipe)
        .onAppear(perform: arrive)
    }

    /// The first time Home shows the ribbon in a launch, the sparkle bounces
    /// once, a moment after Home appears. Without motion it does not.
    private func arrive() {
        guard !Self.arrived else { return }
        Self.arrived = true
        guard !reduceMotion else { return }
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(600))
            sparkle += 1
        }
    }

    /// The names on one line: all of them when they fit, else the first few
    /// and how many more.
    @ViewBuilder private var summary: some View {
        let titles = entries.map { WhatsNewWords.localized($0.title) }
        ViewThatFits(in: .horizontal) {
            ForEach(Array(stride(from: titles.count, through: 1, by: -1)), id: \.self) { named in
                Text(Self.summary(titles, naming: named)).lineLimit(1)
            }
            Text(Self.summary(titles, naming: 0)).lineLimit(1)
        }
    }

    /// "File browser · More memory", "File browser · More memory · 5 more",
    /// or "7 new features" when none is named.
    static func summary(_ titles: [String], naming named: Int, language: String = appLanguage) -> String {
        guard named > 0 else {
            return String(localized: "\(titles.count) new features")
        }
        let rest = titles.count - named
        return (titles.prefix(named) + (rest > 0 ? [String(localized: "\(rest) more")] : []))
            .joined(separator: separator(inLanguage: language))
    }

    /// The language the application's own words are in.
    static var appLanguage: String { Bundle.main.preferredLocalizations.first ?? "en" }

    /// What stands between two names: a dot. Where a language breaks lines at
    /// spaces, a no-break space before the dot keeps it at the end of its line
    /// and never at the start of the next. Chinese and Japanese break between
    /// any two characters, so a no-break space there only drags the last
    /// character of a name onto the next line with the dot; they take plain
    /// spaces, and a name is cut only where the line is full.
    static func separator(inLanguage language: String) -> String {
        let breaksBetweenCharacters = language.hasPrefix("zh") || language.hasPrefix("ja")
        return (breaksBetweenCharacters ? " " : "\u{00A0}") + "\u{00B7} "
    }

    /// A pull down that follows the finger, against some resistance, and
    /// puts the ribbon away past `closingPull`. Without motion the ribbon
    /// stays put and the swipe only decides.
    private var swipe: some Gesture {
        DragGesture(minimumDistance: 12)
            .onChanged { value in
                guard !reduceMotion else { return }
                pull = max(0, value.translation.height) * 0.5
            }
            .onEnded { value in
                let distance = max(value.translation.height, value.predictedEndTranslation.height * 0.5)
                if distance > Self.closingPull {
                    close()
                } else {
                    withAnimation(.snappy) { pull = 0 }
                }
            }
    }
}

/// The first new feature's tile on a small deck: one card behind it for
/// each further feature, up to two, so the ribbon shows at a glance that
/// there is more than one.
struct WhatsNewDeck: View {
    let symbol: String
    let count: Int

    /// How far each card behind shows past the one in front of it.
    private static let step: CGFloat = 5

    var body: some View {
        let behind = min(max(count - 1, 0), 2)
        ZStack(alignment: .leading) {
            ForEach((1...max(behind, 1)).reversed(), id: \.self) { depth in
                if depth <= behind {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .fill(Color.accentColor.opacity(depth == 1 ? 0.55 : 0.3))
                        .frame(width: 30, height: 30 - CGFloat(depth) * 6)
                        .offset(x: CGFloat(depth) * Self.step)
                }
            }
            IconTile(symbol: symbol)
        }
        .frame(width: 30 + CGFloat(behind) * Self.step, alignment: .leading)
    }
}
