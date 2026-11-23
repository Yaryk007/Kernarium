// SPDX-License-Identifier: GPL-2.0-or-later

import GuestHostCore
import SwiftUI

// The update notice's views, which each application compiles into its own
// module: the detail a card or an Info row opens, and Info's Updates section.
// Their words are `UpdateWords`, which hold the application's name as a
// parameter; the application supplies its mark and, through the tint, its
// accent. The card itself is the application's: it sits among the
// application's own notices (Andarium's Home) or heads its settings.

/// The rounded square that marks a row: a white glyph on the tint.
private struct UpdateTile: View {
    let symbol: String

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 14, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 30, height: 30)
            .background(Color.accentColor.gradient, in: .rect(cornerRadius: 8, style: .continuous))
            // A row's words say it all.
            .accessibilityHidden(true)
    }
}

/// A row that leaves the application for a sideloading app or the browser.
private struct UpdateLinkRow: View {
    let symbol: String
    let title: String
    let identifier: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                UpdateTile(symbol: symbol)
                Text(title).foregroundStyle(.primary)
                Spacer()
                Image(systemName: "arrow.up.right")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
            }
        }
        .accessibilityAddTraits(.isLink)
        .accessibilityIdentifier(identifier)
    }
}

/// What a card or an Info row opens: the version, its day, its size, the iOS
/// it needs and the one installed, from the file; then the ways to update the
/// device has, and Skip This Version. The sentences are the application's own.
/// With no sideloading app to hand the file to, two steps take their place.
struct UpdateDetailList<Glyph: View>: View {
    @Environment(\.dismiss) private var dismiss
    let notice: UpdateNotice
    /// Andarium's mark, or Kernarium's icon.
    @ViewBuilder let glyph: () -> Glyph

    var body: some View {
        Group {
            if let offer = notice.offer {
                list(offer)
            } else {
                // A read that ends while the detail is open can withdraw the
                // version; there is nothing left to show, so the detail
                // closes (a sheet's Done goes with it).
                Color.clear
            }
        }
        .onAppear { if notice.offer == nil { dismiss() } }
        .onChange(of: notice.offer == nil) { _, withdrawn in
            if withdrawn { dismiss() }
        }
    }

    private func list(_ offer: UpdateVersion) -> some View {
        let tools = notice.tools
        let title = UpdateWords.detailTitle(app: notice.app.name, version: offer.version)
        return List {
            Section {
                HStack(spacing: 16) {
                    glyph()
                        .frame(width: 80)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(notice.app.name).font(.title3.weight(.semibold))
                        Text(UpdateWords.versionLine(offer.label))
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 6)
                .accessibilityElement(children: .combine)
                LabeledContent(UpdateWords.released, value: offer.released.formatted())
                LabeledContent(UpdateWords.size, value: UpdateWords.sizeText(offer))
                if let minimum = offer.minimumOS {
                    LabeledContent(UpdateWords.requires, value: UpdateWords.requiresValue(minimum))
                }
                LabeledContent(UpdateWords.installed, value: notice.app.installedLabel)
            }
            if tools.isEmpty {
                Section(UpdateWords.stepsTitle) {
                    ForEach(Array(UpdateWords.steps.enumerated()), id: \.offset) { index, step in
                        HStack(alignment: .firstTextBaseline, spacing: 12) {
                            Text(verbatim: "\(index + 1)")
                                .font(.subheadline.weight(.semibold).monospacedDigit())
                                .foregroundStyle(Color.accentColor)
                                .frame(width: 30)
                            Text(step)
                        }
                        .accessibilityElement(children: .combine)
                    }
                }
            }
            Section {
                ForEach(tools, id: \.self) { tool in
                    UpdateLinkRow(
                        symbol: "arrow.down.app.fill", title: UpdateWords.updateWith(tool),
                        identifier: "update-with-\(tool.scheme)", action: { notice.update(with: tool) })
                }
                UpdateLinkRow(
                    symbol: "arrow.down.circle.fill", title: UpdateWords.downloadAppFile,
                    identifier: "update-app-file", action: { notice.openAppFile() })
                if offer.notes != nil {
                    UpdateLinkRow(
                        symbol: "safari.fill", title: UpdateWords.openReleasePage,
                        identifier: "update-release-page", action: { notice.openReleasePage() })
                }
            } footer: {
                Text(UpdateWords.sameAppleID)
            }
            if notice.showsCard {
                Section {
                    Button(UpdateWords.skip) {
                        notice.hideCard()
                        dismiss()
                    }
                    .foregroundStyle(Color.accentColor)
                    .accessibilityIdentifier("update-skip")
                }
            }
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
    }
}

/// The detail in a sheet of its own, with Done.
struct UpdateDetailSheet<Glyph: View>: View {
    @Environment(\.dismiss) private var dismiss
    let notice: UpdateNotice
    @ViewBuilder let glyph: () -> Glyph

    var body: some View {
        NavigationStack {
            UpdateDetailList(notice: notice, glyph: glyph)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button(UpdateWords.done) { dismiss() }
                    }
                }
        }
        .presentationSizing(.form)
    }
}

/// Info's Updates section, in a build that has an address: the update while
/// there is one (which opens the detail), the Check for Updates switch, and
/// under it what the check reads and sends.
struct UpdateInfoSection<Glyph: View>: View {
    let notice: UpdateNotice
    @ViewBuilder let glyph: () -> Glyph

    var body: some View {
        Section {
            if let offer = notice.offer {
                NavigationLink {
                    UpdateDetailList(notice: notice, glyph: glyph)
                } label: {
                    HStack(spacing: 12) {
                        UpdateTile(symbol: "arrow.down.app.fill")
                        Text(UpdateWords.infoRow)
                        Spacer()
                        Text(offer.label).foregroundStyle(.secondary)
                    }
                }
                .accessibilityIdentifier("update-info-row")
            }
            Toggle(
                isOn: Binding(get: { notice.checks }, set: { notice.setChecks($0) })
            ) {
                HStack(spacing: 12) {
                    UpdateTile(symbol: "arrow.triangle.2.circlepath")
                    Text(UpdateWords.checkSwitch)
                }
            }
            .accessibilityIdentifier("update-check-switch")
        } header: {
            Text(UpdateWords.infoHeader)
        } footer: {
            Text(UpdateWords.checkFootnote)
        }
    }
}
