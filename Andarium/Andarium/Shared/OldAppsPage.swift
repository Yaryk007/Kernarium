// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The Apps section's row that opens the page, with "On" when the switch is on
/// for at least one app. Settings offers it only when the last listing found an
/// app the switch can apply to.
struct OldAppsRow: View {
    @Environment(AppModel.self) private var model
    let open: () -> Void

    var body: some View {
        Button(action: open) {
            HStack(spacing: 12) {
                IconTile(symbol: "clock.arrow.circlepath")
                Text(OldAppsWords.title)
                    .foregroundStyle(Color.primary)
                    .layoutPriority(1)
                Spacer(minLength: 0)
                if model.session.oldApps.anyOn {
                    Text(OldAppsWords.on)
                        .font(.subheadline)
                        .foregroundStyle(Color(.secondaryLabel))
                        .lineLimit(1)
                }
                Image(systemName: "chevron.forward")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Color(.tertiaryLabel))
                    .accessibilityHidden(true)
            }
        }
        .accessibilityIdentifier("settings-old-apps")
    }
}

/// Settings > Apps > Old app compatibility: the installed apps built for an
/// Android before 4.0 that the image can run more like it used to, each with
/// a switch. The image's listing decides which apps appear and whether each
/// switch is on; a change is written only after the person has been asked.
struct OldAppsPage: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let state = model.session.oldApps
        List {
            if state.failed {
                failure
            } else if state.apps.isEmpty {
                if state.listed { empty } else { loading }
            } else {
                appsSection(state)
            }
        }
        .listSectionSpacing(.compact)
        .navigationTitle(OldAppsWords.title)
        .navigationBarTitleDisplayMode(.inline)
        // The page lists afresh each time it opens, and an alert left over is gone.
        .onAppear { state.pageOpened() }
        .task { await model.refreshOldApps() }
        .onDisappear { state.pageClosed() }
        .background {
            Color.clear
                .accessibilityHidden(true)
                .alert(
                    state.prompt.map(OldAppsWords.title(for:)) ?? "",
                    isPresented: Binding(get: { state.prompt != nil }, set: { if !$0 { state.prompt = nil } }),
                    presenting: state.prompt
                ) { prompt in
                    switch prompt {
                    case .turnOn:
                        Button(OldAppsWords.cancel, role: .cancel) {}
                        Button(OldAppsWords.turnOn) { answer(prompt, confirmed: true) }
                    case .turnOff:
                        Button(OldAppsWords.cancel, role: .cancel) {}
                        Button(OldAppsWords.turnOff) { answer(prompt, confirmed: true) }
                    case .notChanged:
                        Button(OldAppsWords.ok, role: .cancel) {}
                    }
                } message: { prompt in
                    Text(OldAppsWords.message(for: prompt).keepingTogether)
                }
        }
    }

    private func answer(_ prompt: OldAppsPrompt, confirmed: Bool) {
        Task { await model.answerOldApps(prompt, confirmed: confirmed) }
    }

    // MARK: States

    private var loading: some View {
        Section {
            HStack(spacing: 12) {
                ProgressView()
                Text(OldAppsWords.loading)
                    .foregroundStyle(.secondary)
            }
            .accessibilityElement(children: .combine)
        }
    }

    private var empty: some View {
        Section {
            Text(OldAppsWords.empty)
                .foregroundStyle(.secondary)
        } footer: {
            Text(OldAppsWords.footer.keepingTogether)
        }
    }

    private var failure: some View {
        Section {
            Text(OldAppsWords.failed.keepingTogether)
            Button(OldAppsWords.tryAgain) {
                Task { await model.refreshOldApps() }
            }
            .accessibilityIdentifier("old-apps-try-again")
        }
    }

    private func appsSection(_ state: OldAppsState) -> some View {
        Section {
            ForEach(state.apps) { app in
                OldAppRow(app: app, busy: state.busy != nil || !model.canAskOldApps)
            }
        } header: {
            Text(OldAppsWords.sectionHeader)
        } footer: {
            Text(OldAppsWords.footer.keepingTogether)
        }
    }
}

/// One app: its name from Android, its package name beneath, and its switch.
private struct OldAppRow: View {
    @Environment(AppModel.self) private var model
    let app: OldApp
    /// A change is being asked about or written, or the virtual machine cannot
    /// be asked: no switch moves.
    let busy: Bool

    var body: some View {
        Toggle(
            isOn: Binding(
                get: { app.isOn },
                set: { on in Task { await model.requestOldApp(app.package, on: on) } })
        ) {
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: app.label)
                Text(verbatim: app.package)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
        .disabled(busy)
        .accessibilityLabel(OldAppsWords.switchLabel(app.label))
        .accessibilityIdentifier("old-app-\(app.package)")
    }
}
