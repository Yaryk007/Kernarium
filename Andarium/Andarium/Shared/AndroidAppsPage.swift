// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AppIntents
import SwiftUI

/// The Apps section's row that opens Add to Home Screen. Settings offers it
/// only while Android runs (`AppModel.offersAddToHomeScreen`).
struct AddToHomeScreenRow: View {
    let open: () -> Void

    var body: some View {
        Button(action: open) {
            HStack(spacing: 12) {
                IconTile(symbol: "plus.app.fill")
                Text(AndroidAppsWords.featureName)
                    .foregroundStyle(Color.primary)
                    .layoutPriority(1)
                Spacer(minLength: 0)
                Text(AndroidAppsWords.rowDetail)
                    .font(.subheadline)
                    .foregroundStyle(Color(.secondaryLabel))
                    .lineLimit(1)
                Image(systemName: "chevron.forward")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Color(.tertiaryLabel))
                    .accessibilityHidden(true)
            }
        }
        .accessibilityIdentifier("settings-add-to-home-screen")
    }
}

/// Settings > Apps > Add to Home Screen: the apps Android lists with a launcher
/// icon, installed ones first, then the ones built into Android. A tap on an
/// app leads to the guide that adds its icon to the Home Screen through Apple's
/// Shortcuts. The list is the cache's, shown at once while Android is asked for
/// a fresher one, and the switch for Search is at the foot.
struct AddToHomeScreenPage: View {
    @Environment(AppModel.self) private var model
    @State private var query: String
    @State private var guided: AndroidApp?

    init(query: String = "") {
        _query = State(initialValue: query)
    }

    var body: some View {
        let state = model.androidApps
        content(state)
            .navigationTitle(AndroidAppsWords.featureName)
            .navigationBarTitleDisplayMode(.inline)
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .automatic), prompt: Text(AndroidAppsWords.searchPrompt))
            .navigationDestination(item: $guided) { app in AddToHomeScreenGuide(app: app) }
            .task { await model.refreshAndroidApps() }
            .task {
                // A launch argument of the preview opens the guide of one app.
                guard let id = model.sessionPreset.addToHomeGuide else { return }
                try? await Task.sleep(for: .milliseconds(600))
                guided = state.apps.first { $0.id == id }
            }
    }

    private func matching(_ apps: [AndroidApp]) -> [AndroidApp] {
        let words = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !words.isEmpty else { return apps }
        return apps.filter { $0.label.localizedStandardContains(words) || $0.component.packageName.localizedStandardContains(words) }
    }

    @ViewBuilder
    private func content(_ state: AndroidAppsState) -> some View {
        let installed = matching(state.installed)
        let builtIn = matching(state.builtIn)
        if state.apps.isEmpty {
            empty(state)
        } else if installed.isEmpty, builtIn.isEmpty {
            ContentUnavailableView {
                Label(AndroidAppsWords.noResults(query), systemImage: "magnifyingglass")
            } description: {
                Text(AndroidAppsWords.noResultsText)
            }
        } else {
            List {
                if state.refreshing { refreshing }
                if state.failed { remembered(state) }
                if !installed.isEmpty {
                    Section(AndroidAppsWords.installed) {
                        ForEach(installed) { row($0) }
                    }
                }
                if !builtIn.isEmpty {
                    Section(AndroidAppsWords.builtIn) {
                        ForEach(builtIn) { row($0) }
                    }
                }
                if query.isEmpty { searchSection(state) }
            }
            .listSectionSpacing(.compact)
        }
    }

    // MARK: Rows

    private func row(_ app: AndroidApp) -> some View {
        Button { guided = app } label: {
            HStack(spacing: 12) {
                AndroidAppIconView(app: app, size: 40)
                VStack(alignment: .leading, spacing: 1) {
                    Text(verbatim: app.label).foregroundStyle(Color.primary)
                    Text(verbatim: app.component.packageName)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                Spacer(minLength: 4)
                Image(systemName: "plus.circle.fill")
                    .font(.title3)
                    .foregroundStyle(Color.accentColor)
                    .accessibilityHidden(true)
            }
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(AndroidAppsWords.addLabel(app.label)))
        .accessibilityIdentifier("android-app-\(app.id)")
    }

    /// Android is being asked for a fresher list; the one shown is the cache's.
    private var refreshing: some View {
        Section {
            HStack(spacing: 12) {
                ProgressView()
                Text(AndroidAppsWords.refreshing).foregroundStyle(.secondary)
            }
            .accessibilityElement(children: .combine)
            .accessibilityIdentifier("android-apps-refreshing")
        }
    }

    /// Android did not answer: the list is the last one it gave.
    private func remembered(_ state: AndroidAppsState) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 10) {
                Label {
                    Text(AndroidAppsWords.remembered(state.listedAt.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? ""))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } icon: {
                    Image(systemName: "clock.arrow.circlepath").foregroundStyle(.secondary)
                }
                Button(AndroidAppsWords.tryAgain) { Task { await model.refreshAndroidApps() } }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            }
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("android-apps-remembered")
        }
    }

    // MARK: Search

    private func searchSection(_ state: AndroidAppsState) -> some View {
        Section {
            Toggle(isOn: Binding(get: { state.searchOn }, set: { model.setAndroidAppsInSearch($0) })) {
                HStack(spacing: 12) {
                    IconTile(symbol: "magnifyingglass")
                    Text(AndroidAppsWords.searchSwitch)
                }
            }
            .accessibilityIdentifier("android-apps-search-switch")
        } footer: {
            Text(AndroidAppsWords.searchFooter)
        }
    }

    // MARK: No list

    /// No app is known: the list is being asked for, Android did not answer, or
    /// it lists none.
    @ViewBuilder
    private func empty(_ state: AndroidAppsState) -> some View {
        if state.refreshing || (!state.listed && !state.failed) {
            VStack(spacing: 12) {
                ProgressView()
                Text(AndroidAppsWords.refreshing).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .accessibilityElement(children: .combine)
            .accessibilityIdentifier("android-apps-refreshing")
        } else if state.failed {
            ContentUnavailableView {
                Label(AndroidAppsWords.failedTitle, systemImage: "apps.iphone")
            } description: {
                Text(OldAppsWords.failed)
            } actions: {
                Button(AndroidAppsWords.tryAgain) { Task { await model.refreshAndroidApps() } }
                    .buttonStyle(.bordered)
            }
        } else {
            ContentUnavailableView {
                Label(AndroidAppsWords.emptyTitle, systemImage: "apps.iphone")
            } description: {
                Text(AndroidAppsWords.emptyText)
            }
        }
    }
}

// MARK: - The guide

/// One page, every time before Apple's Shortcuts: the app, the steps of
/// Shortcuts' own flow drawn as they look, and Apple's Shortcuts button at the
/// foot. The pictures are drawings of Apple's screens, marked as such; the
/// words carry the steps for VoiceOver.
struct AddToHomeScreenGuide: View {
    let app: AndroidApp

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                header
                VStack(alignment: .leading, spacing: 0) {
                    ShortcutsTag(text: AndroidAppsWords.inShortcuts)
                        .padding(.horizontal, 16)
                        .padding(.top, 14)
                        .padding(.bottom, 4)
                    step(1, AndroidAppsWords.step1, AndroidAppsWords.step1Detail) { EmptyView() }
                    Divider().padding(.leading, 52)
                    step(2, AndroidAppsWords.step2(app.label), AndroidAppsWords.step2Detail) { MiniTiles(app: app, scale: 0.8) }
                    Divider().padding(.leading, 52)
                    step(3, AndroidAppsWords.step3, AndroidAppsWords.step3Detail) { MiniMenu(scale: 0.8) }
                    Divider().padding(.leading, 52)
                    step(4, AndroidAppsWords.step4, AndroidAppsWords.step4Detail) { MiniHomeSheet(app: app, scale: 0.8) }
                }
                .glassEffect(.regular, in: .rect(cornerRadius: 26))
                Text(AndroidAppsWords.tapNote(app.label).keepingTogether)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 8)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 20)
            .frame(maxWidth: 560)
            .frame(maxWidth: .infinity)
        }
        .background(Color(.systemGroupedBackground))
        .safeAreaBar(edge: .bottom) { ShortcutsLinkBar(caption: AndroidAppsWords.linkCaption) }
        .navigationTitle(AndroidAppsWords.featureName)
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("add-to-home-screen-guide")
    }

    private var header: some View {
        HStack(spacing: 14) {
            AndroidAppIconView(app: app, size: 60)
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: app.label).font(.title2.weight(.bold))
                Text(AndroidAppsWords.androidApp).font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer(minLength: 0)
        }
        .padding(.top, 8)
        .padding(.horizontal, 4)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private func step(_ number: Int, _ title: String, _ detail: String, @ViewBuilder drawing: () -> some View) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Text(verbatim: "\(number)")
                .font(.subheadline.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 26, height: 26)
                .background(Color.accentColor, in: .circle)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.body.weight(.semibold))
                    Text(detail).font(.footnote).foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)
                drawing()
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
    }
}

/// Apple's Shortcuts button at the foot of the guide, with what it does. The
/// button and its words are Apple's: it opens Shortcuts on Andarium's page.
struct ShortcutsLinkBar: View {
    let caption: String

    var body: some View {
        VStack(spacing: 6) {
            ShortcutsLink()
                .accessibilityIdentifier("shortcuts-link")
            Text(caption.keepingTogether)
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(.horizontal, 24)
        .padding(.top, 12)
        .padding(.bottom, 6)
        .frame(maxWidth: .infinity)
        .background {
            LinearGradient(
                stops: [
                    .init(color: Color(.systemGroupedBackground).opacity(0), location: 0),
                    .init(color: Color(.systemGroupedBackground), location: 0.3),
                ],
                startPoint: .top, endPoint: .bottom
            )
            .ignoresSafeArea(edges: .bottom)
        }
    }
}

// MARK: Drawings of Shortcuts' screens

/// A rounded tag that marks a drawing of a screen that is iOS's, not Andarium's.
private struct ShortcutsTag: View {
    let text: String

    var body: some View {
        Label {
            Text(text)
        } icon: {
            Image(systemName: "square.2.layers.3d.fill")
                .foregroundStyle(
                    LinearGradient(
                        colors: [Color(red: 0.94, green: 0.31, blue: 0.43), Color(red: 0.36, green: 0.36, blue: 0.96)],
                        startPoint: .top, endPoint: .bottom))
        }
        .font(.caption.weight(.semibold))
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(Color(.tertiarySystemFill), in: .capsule)
        .accessibilityHidden(true)
    }
}

/// A tile of Andarium's page in Shortcuts that stands for another app.
private struct NeutralTileIcon: View {
    let size: CGFloat

    var body: some View {
        RoundedRectangle(cornerRadius: size * 0.2237, style: .continuous)
            .fill(Color(.systemGray4))
            .frame(width: size, height: size)
    }
}

/// Andarium's page in Shortcuts: the command's own tile, then the app's, held,
/// then another.
private struct MiniTiles: View {
    let app: AndroidApp
    var scale: CGFloat = 1

    var body: some View {
        HStack(alignment: .top, spacing: 12 * scale) {
            tile(label: AndroidAppsWords.intentTile) {
                Image(systemName: "square.grid.2x2")
                    .font(.system(size: 17 * scale, weight: .medium))
                    .foregroundStyle(Color.accentColor)
                    .frame(width: 42 * scale, height: 42 * scale)
                    .background(Color.accentColor.opacity(0.16), in: .circle)
            }
            .opacity(0.45)
            tile(label: app.label) {
                AndroidAppIconView(app: app, size: 42 * scale)
                    .scaleEffect(1.12)
                    .shadow(color: .black.opacity(0.25), radius: 6 * scale, y: 3 * scale)
                    .overlay(alignment: .bottomTrailing) {
                        Image(systemName: "hand.point.up.left.fill")
                            .font(.system(size: 18 * scale))
                            .foregroundStyle(.white)
                            .shadow(color: .black.opacity(0.5), radius: 2)
                            .offset(x: 10 * scale, y: 10 * scale)
                    }
            }
            tile(label: " ") { NeutralTileIcon(size: 42 * scale) }
                .opacity(0.45)
        }
        .padding(.horizontal, 14 * scale)
        .padding(.vertical, 12 * scale)
        .background(Color(.systemGray6), in: .rect(cornerRadius: 16 * scale, style: .continuous))
        .accessibilityHidden(true)
    }

    private func tile(label: String, @ViewBuilder icon: () -> some View) -> some View {
        VStack(spacing: 5 * scale) {
            icon()
            Text(verbatim: label)
                .font(.system(size: 9 * scale))
                .foregroundStyle(.primary)
                .lineLimit(1)
                .frame(width: 54 * scale)
        }
    }
}

/// The menu a held tile opens.
private struct MiniMenu: View {
    var scale: CGFloat = 1

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            row(symbol: "plus.app", text: AndroidAppsWords.menuAdd)
                .background(Color.accentColor.opacity(0.16), in: .rect(cornerRadius: 10 * scale, style: .continuous))
            row(symbol: "square.2.layers.3d", text: AndroidAppsWords.menuNewShortcut)
                .opacity(0.5)
        }
        .padding(5 * scale)
        .frame(width: 200 * scale)
        .background(.regularMaterial, in: .rect(cornerRadius: 16 * scale, style: .continuous))
        .shadow(color: .black.opacity(0.12), radius: 8 * scale, y: 3 * scale)
        .accessibilityHidden(true)
    }

    private func row(symbol: String, text: String) -> some View {
        HStack(spacing: 10 * scale) {
            Image(systemName: symbol).font(.system(size: 13 * scale))
            Text(verbatim: text).font(.system(size: 13 * scale)).lineLimit(1)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 10 * scale)
        .padding(.vertical, 8 * scale)
    }
}

/// The top of Shortcuts' Home Screen sheet: Cancel, the title, the check mark,
/// and the app's icon and name already in it.
private struct MiniHomeSheet: View {
    let app: AndroidApp
    var scale: CGFloat = 1

    var body: some View {
        VStack(spacing: 8 * scale) {
            HStack {
                Image(systemName: "xmark")
                    .font(.system(size: 11 * scale, weight: .semibold))
                    .frame(width: 26 * scale, height: 26 * scale)
                    .background(Color(.systemGray5), in: .circle)
                Spacer()
                Text(verbatim: AndroidAppsWords.sheetTitle)
                    .font(.system(size: 12 * scale, weight: .semibold))
                Spacer()
                Image(systemName: "checkmark")
                    .font(.system(size: 12 * scale, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 26 * scale, height: 26 * scale)
                    .background(Color.accentColor, in: .circle)
                    .overlay(Circle().stroke(Color.accentColor.opacity(0.35), lineWidth: 5 * scale).padding(-5 * scale))
            }
            AndroidAppIconView(app: app, size: 40 * scale)
            Text(verbatim: app.label)
                .font(.system(size: 9 * scale, weight: .semibold))
                .foregroundStyle(.white)
                .lineLimit(1)
                .padding(.horizontal, 7 * scale)
                .padding(.vertical, 2 * scale)
                .background(Color(.systemGray), in: .capsule)
        }
        .padding(10 * scale)
        .frame(width: 210 * scale)
        .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 16 * scale, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 16 * scale, style: .continuous).stroke(Color(.separator), lineWidth: 0.5))
        .accessibilityHidden(true)
    }
}
