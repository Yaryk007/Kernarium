// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The notes the ribbon opens: the releases since the last one the person
/// saw, newest first. Done closes them, and the ribbon then goes.
struct WhatsNewSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    /// The releases as they were when the notes opened: the ribbon may be
    /// put away while they show.
    @State private var releases: [WhatsNewRelease]

    init(releases: [WhatsNewRelease]) {
        _releases = State(initialValue: releases)
    }

    var body: some View {
        NavigationStack {
            WhatsNewNotesList(releases: releases)
                .navigationTitle("What\u{2019}s New")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                            .accessibilityIdentifier("whats-new-done")
                    }
                }
        }
        .presentationSizing(.form)
        .onAppear { model.whatsNew.read() }
        .onDisappear {
            // The ribbon leaves once the notes are gone, so the zoom back
            // lands on it first.
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(350))
                withAnimation(.snappy) { model.whatsNew.putAway() }
            }
        }
    }
}

/// Every release's notes, in Info.
struct WhatsNewHistoryPage: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        WhatsNewNotesList(releases: model.whatsNew.releases)
            .navigationTitle("What\u{2019}s New")
            .navigationBarTitleDisplayMode(.inline)
    }
}

/// Releases as Home lays out its sections: a small heading with the version
/// and the day, and the entries in one glass panel beneath.
struct WhatsNewNotesList: View {
    let releases: [WhatsNewRelease]

    var body: some View {
        ScrollView {
            VStack(spacing: 24) {
                ForEach(releases) { release in
                    VStack(alignment: .leading, spacing: 8) {
                        WhatsNewReleaseHeading(release: release)
                        VStack(spacing: 0) {
                            ForEach(Array(release.entries.enumerated()), id: \.element.id) { index, entry in
                                if index > 0 { RowDivider() }
                                WhatsNewEntryRow(entry: entry)
                            }
                        }
                        .glassEffect(.regular, in: .rect(cornerRadius: 26))
                    }
                }
            }
            .frame(maxWidth: 560)
            .padding(.horizontal, 16)
            .padding(.top, 8)
            .padding(.bottom, 24)
            .frame(maxWidth: .infinity)
        }
        .background(Color(.systemGroupedBackground))
    }
}

/// "Version 1.1 (16)" and the day it came out, when the notes give one, side
/// by side, or stacked when the text is too large for one line. Stacked, each
/// may take as many lines as it needs, so no size cuts it short.
struct WhatsNewReleaseHeading: View {
    let release: WhatsNewRelease

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(alignment: .firstTextBaseline) {
                version(lines: 1)
                Spacer(minLength: 12)
                day(lines: 1)
            }
            VStack(alignment: .leading, spacing: 2) {
                version(lines: nil)
                day(lines: nil)
            }
        }
        .padding(.horizontal, 18)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private func version(lines: Int?) -> some View {
        Text("Version \(release.version)")
            .font(.subheadline.weight(.semibold))
            .foregroundStyle(.secondary)
            .lineLimit(lines)
    }

    @ViewBuilder private func day(lines: Int?) -> some View {
        if let day = release.day {
            Text(day, format: .dateTime.day().month(.wide).year())
                .font(.subheadline)
                .foregroundStyle(.tertiary)
                .lineLimit(lines)
        }
    }
}

/// One entry: its tile, name, words and points, and where to find it when it
/// is not on Home.
struct WhatsNewEntryRow: View {
    let entry: WhatsNewEntry

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            IconTile(symbol: entry.symbol)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text(WhatsNewWords.localized(entry.title))
                    .font(.body.weight(.semibold))
                if let text = entry.text {
                    Text(WhatsNewWords.localized(text))
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if !entry.points.isEmpty {
                    WhatsNewPoints(points: entry.points)
                }
                if let place = entry.place {
                    Label(place.words, systemImage: place.symbol)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .labelIconToTitleSpacing(5)
                        .padding(.top, 3)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 14)
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("whats-new-\(entry.id)")
    }
}

/// An entry's points as a compact bulleted list. A point that wraps lines up
/// under its first word, at every text size; VoiceOver skips the bullets and
/// reads the points one after another.
struct WhatsNewPoints: View {
    let points: [String]

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(Array(points.enumerated()), id: \.offset) { _, point in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text("\u{2022}")
                        .accessibilityHidden(true)
                    Text(WhatsNewWords.localized(point))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
        }
        .font(.subheadline)
        .foregroundStyle(.secondary)
    }
}

/// The notes are written in English in `WhatsNew.json` and translated in
/// `WhatsNew.xcstrings`, which holds each note under its English text; a note
/// with no entry is shown as written. A new build's notes are added to both.
enum WhatsNewWords {
    static func localized(_ note: String) -> String {
        String(localized: String.LocalizationValue(note), table: "WhatsNew")
    }
}
