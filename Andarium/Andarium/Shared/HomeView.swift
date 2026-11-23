// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// The only screen before a run: the guest's storage, processors and memory,
/// its options and Start. Without a guest it offers to set one up.
struct HomeView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    /// The notes zoom out of the ribbon and back into it.
    @Namespace private var whatsNewZoom
    let hints: ScreenHints

    var body: some View {
        @Bindable var model = model
        NavigationStack {
            GeometryReader { geo in
                let wide = geo.size.width >= 700
                Group {
                    if model.guest != nil, let fold = model.fold,
                        let pages = HomePages(fold, frame: geo.frame(in: .global))
                    {
                        pagesLayout(pages)
                    } else if model.guest != nil {
                        ScrollViewReader { proxy in
                            ScrollView {
                                if wide && geo.size.width > geo.size.height {
                                    wideLayout
                                } else if wide {
                                    tallLayout
                                } else {
                                    narrowLayout
                                }
                            }
                            .onAppear {
                                if let target = hints.scrollTarget {
                                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                                        proxy.scrollTo(target, anchor: .bottom)
                                    }
                                }
                            }
                        }
                        .safeAreaBar(edge: .bottom) { startBar(wide: wide) }
                    } else {
                        WelcomeView()
                    }
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Andarium")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    HomeInfoButton()
                }
            }
            // Looks for a newer version of Andarium a few seconds after Home
            // shows; a start cancels it, and it never stands before Start.
            .task { await model.readAppUpdateWhenHomeShows() }
        }
        .sheet(item: $model.sheet) { kind in
            // On a Mac, SwiftUI reads a sheet's presentation preferences
            // before the sheet inherits this view's environment, so every
            // sheet is handed the model it reads.
            Group {
                switch kind {
                case .create: NewGuestSheet()
                case .resize: ResizeSheet()
                case .system: SystemSheet()
                case .resolution: ResolutionSheet()
                case .info: InfoSheet(page: hints.infoPage)
                case .snapshots: SnapshotsSheet()
                case .whatsNew:
                    WhatsNewSheet(releases: model.whatsNew.announced)
                        .modifier(ZoomFrom(id: Self.ribbonID, namespace: whatsNewZoom, enabled: !reduceMotion))
                case .appUpdate:
                    if let notice = model.appUpdate {
                        UpdateDetailSheet(notice: notice) { StackMark(lit: true, width: 62) }
                    }
                }
            }
            .environment(model)
            .foldSheet(model)
        }
        .microphoneAccessAlert()
        .locationAccessAlert()
        .cameraAccessAlert()
        .rootWarningAlert()
        .alert("Erase virtual machine?", isPresented: $model.confirmingErase) {
            Button("Erase virtual machine", role: .destructive) { model.erase() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(SnapshotWords.eraseMessage(snapshots: model.snapshots.count).keepingTogether)
        }
        .alert(StartSpaceWords.title, isPresented: $model.startBlocked) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(StartSpaceWords.message(
                neededBytes: Limits.startReserveBytes, freeBytes: model.host.freeStorageBytes, device: model.host.name).keepingTogether)
        }
    }

    // MARK: Layouts

    /// The inner display of a folding iPhone while it is bent at the fold: one
    /// page of Home on each side of it, each scrolling on its own, and Start at
    /// the foot of the first. Upright, the virtual machine and Start on the
    /// leading page and what it may use on the trailing one; turned, the
    /// virtual machine and Start above the fold and the rest below it. Flat,
    /// Home is laid out as on an iPad: `HomePages` is nil.
    @ViewBuilder
    private func pagesLayout(_ pages: HomePages) -> some View {
        let first = ScrollView {
            VStack(spacing: 22) {
                notices
                SystemSection()
                if pages.upright { developmentSections }
            }
            .padding(.horizontal, pages.upright ? 20 : 28)
            .padding(.top, 8)
            .padding(.bottom, 20)
        }
        .safeAreaBar(edge: .bottom) { startBar(wide: false) }
        let second = ScrollViewReader { proxy in
            ScrollView {
                VStack(spacing: 22) {
                    if pages.upright {
                        guestSections
                    } else {
                        developmentSections
                        guestSections
                    }
                    EraseCard()
                        .id("erase")
                }
                .padding(.horizontal, pages.upright ? 20 : 28)
                .padding(.top, pages.upright ? 8 : 20)
                .padding(.bottom, 20)
            }
            .onAppear {
                if let target = hints.scrollTarget {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                        proxy.scrollTo(target, anchor: .bottom)
                    }
                }
            }
        }
        if pages.upright {
            HStack(spacing: 0) {
                first.frame(width: pages.first)
                Color.clear.frame(width: pages.gap)
                second.frame(width: pages.second)
            }
        } else {
            VStack(spacing: 0) {
                first.frame(height: pages.first)
                Color.clear.frame(height: pages.gap)
                second.frame(height: pages.second)
            }
        }
    }

    private var narrowLayout: some View {
        VStack(spacing: 22) {
            notices
            SystemSection()
            developmentSections
            guestSections
            EraseCard()
                .id("erase")
        }
        .padding(.horizontal, 16)
        .padding(.top, 4)
        .padding(.bottom, 20)
    }

    /// iPad in portrait or a tall window: the controls across the top, the
    /// options and the Erase card beneath.
    private var tallLayout: some View {
        VStack(spacing: 28) {
            notices
            SystemSection()
            developmentSections
            guestSections
            EraseCard()
                .id("erase")
        }
        .frame(maxWidth: 680)
        .padding(.horizontal, 28)
        .padding(.top, 12)
        .padding(.bottom, 20)
        .frame(maxWidth: .infinity)
    }

    private var wideLayout: some View {
        HStack(alignment: .top, spacing: 24) {
            VStack(spacing: 22) {
                notices
                SystemSection()
                developmentSections
            }
            .frame(maxWidth: 520)
            VStack(spacing: 22) {
                guestSections
                EraseCard()
                    .id("erase")
            }
            .frame(maxWidth: 400)
        }
        .padding(.horizontal, 28)
        .padding(.top, 8)
        .padding(.bottom, 20)
        .frame(maxWidth: .infinity)
    }

    /// Performance (120 Hz, AOT where the build carries it, and JIT), right below
    /// Virtual machine.
    @ViewBuilder
    private var developmentSections: some View {
        HomePerformanceSection()
    }

    /// Capabilities, Root and Backup: what the guest may use.
    @ViewBuilder
    private var guestSections: some View {
        CapabilitiesSection()
        RootSection()
        BackupSection()
    }

    @ViewBuilder
    private var notices: some View {
        @Bindable var model = model
        // A newer version of Andarium comes first among the notices.
        AppUpdateCard()
        // An app an icon, Shortcuts, Search or Siri asked to open waits for the
        // answer the notices below ask: the card says which, above them.
        if let pending = model.androidApps.open.pending, case let .waiting(hold, _) = pending.stage, hold != .snapshot {
            AppOpenWaitingCard(request: pending.request, hold: hold)
        }
        if !model.host.runsGuestGraphics {
            NoticeCard(
                symbol: "cpu", color: .orange,
                title: DeviceRequirement.title,
                text: DeviceRequirement.explanation(deviceName: model.host.name)
            )
        }
        if let restored = model.restoredSnapshot {
            NoticeCard(
                symbol: "clock.arrow.circlepath", color: .accentColor,
                title: SnapshotWords.restoredTitle, text: SnapshotWords.restoredText(restored),
                onClose: { withAnimation { model.restoredSnapshot = nil } }
            )
        }
        if let text = model.snapshots.unfinishedRestoreText {
            NoticeCard(
                symbol: "exclamationmark.arrow.triangle.2.circlepath", color: .orange,
                title: SnapshotWords.unfinishedTitle, text: text
            )
        }
        if model.lowSpace {
            NoticeCard(
                symbol: "externaldrive.badge.exclamationmark", color: .orange,
                title: String(localized: "\(Formats.size(bytes: model.host.freeStorageBytes)) free on this \(model.host.name)"),
                text: SnapshotWords.lowSpaceText(
                    hasSnapshots: model.snapshots.count > 0, startReserve: Formats.size(bytes: Limits.startReserveBytes),
                    device: model.host.name)
            )
        }
        if let guest = model.guest, model.host.memory.isAboveSafe(guest.memoryMiB, vCPUs: guest.cores) {
            NoticeCard(
                symbol: "exclamationmark.triangle.fill", color: .orange,
                title: MemoryWarning.title,
                text: MemoryWarning.explanation(
                    memoryMiB: guest.memoryMiB,
                    safeMiB: model.host.memory.safeMemoryMiB(vCPUs: guest.cores),
                    deviceName: model.host.name
                )
            )
        }
        if let denial = model.networkDenialShowing {
            NoticeCard(
                symbol: NetworkWords.symbol(for: denial), color: .orange,
                title: NetworkWords.title(for: denial), text: NetworkWords.text(for: denial),
                action: NoticeAction(
                    label: NetworkWords.settingsLabel, hint: NetworkWords.settingsHint,
                    identifier: "network-notice-settings", perform: { model.openSettings() }),
                closeLabel: NetworkWords.dismissLabel,
                onClose: { withAnimation { model.dismissNetworkNotice() } }
            )
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("home-network-notice")
        }
        switch model.plan {
        case .update(_, let release) where model.updateNoticeIsShown:
            // A restored older snapshot says itself that the next start updates it.
            NoticeCard(
                symbol: "arrow.down.circle.fill", color: .accentColor,
                title: String(localized: "System image updated"),
                text: String(localized: "Andarium system image is updated to \(ReleaseLabel.revision(of: release)). The next start finishes the update and takes a little longer.")
            )
        case .unfinishedUpdate(let release, let canRetry, let canRestore):
            UnfinishedUpdateCard(release: release, canRetry: canRetry, canRestore: canRestore)
        case .blocked(let reason):
            NoticeCard(
                symbol: "exclamationmark.triangle.fill", color: .orange,
                title: reason.title,
                text: model.snapshots.blockedText(reason)
            )
        case .firstStart, .normal, .update:
            EmptyView()
        }
        if let notice = model.notice {
            switch notice {
            case .closedInBackground:
                NoticeCard(
                    symbol: "moon.zzz.fill", color: .secondary,
                    title: String(localized: "iOS closed Andarium"),
                    text: String(localized: "The virtual machine was shut down in the background."),
                    onClose: { withAnimation { model.notice = nil } }
                )
            case .sessionEnded(let ended):
                NoticeCard(
                    symbol: "exclamationmark.triangle.fill", color: .orange,
                    title: ended.title, text: ended.text,
                    onClose: { withAnimation { model.notice = nil } }
                )
            }
        }
        RootNoticeCard()
    }

    // MARK: Start

    private static let ribbonID = "whats-new-ribbon"

    /// The main action always starts Android normally. After an update with
    /// notes, the ribbon rides above it: Start keeps its place at the foot of
    /// the screen, and the bar grows upwards.
    private func startBar(wide: Bool) -> some View {
        VStack(spacing: 10) {
            if model.whatsNew.showsRibbon {
                WhatsNewRibbon(
                    entries: model.whatsNew.pendingEntries,
                    open: { model.sheet = .whatsNew },
                    close: { withAnimation(.snappy) { model.whatsNew.putAway() } }
                )
                .matchedTransitionSource(id: Self.ribbonID, in: whatsNewZoom)
                .transition(
                    reduceMotion ? .opacity : .move(edge: .bottom).combined(with: .opacity)
                )
            }
            StartButton()
        }
        .frame(maxWidth: wide ? 520 : .infinity)
        .padding(.horizontal, 16)
        .padding(.top, 14)
        .padding(.bottom, 6)
        .frame(maxWidth: .infinity)
        .background {
            // Content fades out under the button instead of showing through,
            // and is gone by the ribbon's top, whose words read on glass.
            let ribbon = model.whatsNew.showsRibbon
            LinearGradient(
                stops: [
                    .init(color: Color(.systemGroupedBackground).opacity(0), location: 0),
                    .init(color: Color(.systemGroupedBackground).opacity(0.92), location: ribbon ? 0.12 : 0.45),
                    .init(color: Color(.systemGroupedBackground), location: ribbon ? 0.22 : 1),
                    .init(color: Color(.systemGroupedBackground), location: 1),
                ],
                startPoint: .top, endPoint: .bottom
            )
            .ignoresSafeArea(edges: .bottom)
            .allowsHitTesting(false)
        }
    }
}

/// The main action starts Android normally.
struct StartButton: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button {
            model.start(.normal)
        } label: {
            Label(title, systemImage: "play.fill")
                .frame(maxWidth: .infinity)
                .frame(height: 52)
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .font(.body.weight(.semibold))
        .foregroundStyle(.white)
        .background(Capsule().fill(Color.accentColor))
        .glassEffect(.regular.tint(Color.accentColor).interactive(), in: .capsule)
        .opacity(blocked ? 0.4 : 1)
        .disabled(blocked)
    }

    private var blocked: Bool { !model.plan.allowsStart || !model.host.runsGuestGraphics }

    private var title: String {
        guard model.guest != nil, model.plan.allowsStart else { return String(localized: "Start") }
        switch model.bootKind(.normal) {
        case .firstStart: return String(localized: "Start and set up")
        case .upgrade: return String(localized: "Update and start")
        default: return String(localized: "Start")
        }
    }
}

/// Home's Info button, the toolbar's trailing item.
struct HomeInfoButton: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button {
            model.sheet = .info
        } label: {
            Label("Info", systemImage: "info")
        }
    }
}

/// The rounded square that marks every row: a white glyph on the accent. The
/// glyph is an SF Symbol, or an image of the asset catalog that is already
/// white (it is shown as it is) or a template image (drawn white).
struct IconTile: View {
    private enum Glyph {
        case symbol(String)
        case asset(String)
    }

    private let glyph: Glyph
    let color: Color

    init(symbol: String, color: Color = .accentColor) {
        glyph = .symbol(symbol)
        self.color = color
    }

    init(asset: String, color: Color = .accentColor) {
        glyph = .asset(asset)
        self.color = color
    }

    var body: some View {
        glyphView
            .frame(width: 30, height: 30)
            .background(color.gradient, in: .rect(cornerRadius: 8, style: .continuous))
            // It marks a row whose words say it all.
            .accessibilityHidden(true)
    }

    @ViewBuilder private var glyphView: some View {
        switch glyph {
        case .symbol(let name):
            Image(systemName: name)
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(.white)
        case .asset(let name):
            Image(name)
                .resizable()
                .scaledToFit()
                .frame(width: 16, height: 16)
                .foregroundStyle(.white)
        }
    }
}

/// First launch: there is no guest yet.
struct WelcomeView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        // While the device is bent a half can be shorter than the column: it
        // scrolls only then.
        column
            .scrollsWhenTall()
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .onCardHalf(.welcome)
    }

    private var column: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 20)
            StackMark(lit: false, width: 150)
                .padding(.bottom, 34)
            Text("No virtual machine yet")
                .font(.title2.weight(.bold))
                .accessibilityAddTraits(.isHeader)
            if !model.host.runsGuestGraphics {
                NoticeCard(
                    symbol: "cpu", color: .orange,
                    title: DeviceRequirement.title,
                    text: DeviceRequirement.explanation(deviceName: model.host.name)
                )
                .frame(maxWidth: 400)
                .padding(.top, 20)
            }
            Button {
                model.sheet = .create
            } label: {
                Text("Set up virtual machine")
                    .font(.body.weight(.semibold))
                    .frame(width: 220, height: 50)
            }
            .buttonStyle(.glassProminent)
            .disabled(!model.host.runsGuestGraphics)
            .padding(.top, 28)
            Spacer(minLength: 20)
            Label("\(Formats.size(bytes: model.host.freeStorageBytes)) free on this \(model.host.name)", systemImage: "internaldrive")
                .font(.footnote)
                .foregroundStyle(model.canCreate ? Color.secondary : Color.orange)
                .padding(.bottom, 12)
        }
        .padding(.horizontal, 24)
    }
}

/// A button under a card's message: the way to what the message asks.
struct NoticeAction {
    let label: String
    var hint: String?
    var identifier: String?
    let perform: () -> Void
}

/// A dismissible message above the controls.
struct NoticeCard: View {
    private func actionButton(_ action: NoticeAction) -> some View {
        Button(action.label, action: action.perform)
            .buttonStyle(.glass)
            .controlSize(.small)
            .accessibilityHint(action.hint ?? "")
            .accessibilityIdentifier(action.identifier ?? "")
    }

    let symbol: String
    let color: Color
    let title: String
    let text: String
    var action: NoticeAction?
    /// A second button beside the first (`RootNoticeCard`).
    var secondAction: NoticeAction?
    /// What VoiceOver says for the close button.
    var closeLabel = String(localized: "Dismiss")
    var onClose: (() -> Void)?

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.title3)
                .foregroundStyle(color)
                .frame(width: 28)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(title.keepingTogether).font(.subheadline.weight(.semibold))
                Text(text.keepingTogether)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let action, let secondAction {
                    // Two buttons share a line while both fit, and stack otherwise.
                    ViewThatFits(in: .horizontal) {
                        HStack(spacing: 8) { actionButton(action); actionButton(secondAction) }
                        VStack(alignment: .leading, spacing: 8) { actionButton(action); actionButton(secondAction) }
                    }
                    .padding(.top, 8)
                } else if let action {
                    actionButton(action)
                        .padding(.top, 8)
                }
            }
            // The title and its words are read as one.
            .accessibilityElement(children: .combine)
            Spacer(minLength: 0)
            if let onClose {
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.secondary)
                        // A 44-point target, drawn as the 28-point mark.
                        .frame(width: 44, height: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(-8)
                .accessibilityLabel(closeLabel)
            }
        }
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
        .accessibilityElement(children: .contain)
    }
}

/// An update that did not reach its end. Start tries it again; the one
/// button, Restore, puts the data back to what the update started from.
struct UnfinishedUpdateCard: View {
    @Environment(AppModel.self) private var model
    let release: String
    let canRetry: Bool
    let canRestore: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "exclamationmark.arrow.triangle.2.circlepath")
                    .font(.title3)
                    .foregroundStyle(Color.orange)
                    .frame(width: 28)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text("System image update didn’t finish").font(.subheadline.weight(.semibold))
                    Text(text)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .accessibilityElement(children: .combine)
            }
            if canRestore {
                Button("Restore") { Task { await model.restoreUpdate() } }
                    .buttonStyle(.glass)
                    .controlSize(.regular)
                    .accessibilityIdentifier("home-update-restore")
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }

    private var text: String {
        let revision = ReleaseLabel.revision(of: release)
        return switch (canRetry, canRestore) {
        case (true, true):
            String(localized: "The update to \(revision) did not finish. Update and start tries it again, or Restore puts back your data from before the update.")
        case (true, false):
            String(localized: "The update to \(revision) did not finish. Update and start tries it again, or erase the virtual machine to start over.")
        case (false, true):
            String(localized: "This app version carries a different release than the update that did not finish. Restore puts back your data from before it, or erase the virtual machine to start over.")
        case (false, false):
            String(localized: "The update to \(revision) did not finish, and nothing from before it is left to put back. Erase the virtual machine to start over.")
        }
    }
}

/// Erasing the guest: the one row, in a card of its own after the sections,
/// with no heading and the spacing the other cards have.
struct EraseCard: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button {
            model.confirmingErase = true
        } label: {
            HStack(spacing: 12) {
                IconTile(symbol: "trash.fill", color: .red)
                Text("Erase virtual machine…")
                    .foregroundStyle(.red)
                Spacer()
                if let guest = model.guest {
                    Text(Formats.size(bytes: guest.usedBytes))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 16)
            .frame(minHeight: 54)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .rect(cornerRadius: 26))
    }
}

/// A zoom from the view marked with `id`, or the system's own presentation
/// where Reduce Motion is on.
struct ZoomFrom: ViewModifier {
    let id: String
    let namespace: Namespace.ID
    let enabled: Bool

    func body(content: Content) -> some View {
        if enabled {
            content.navigationTransition(.zoom(sourceID: id, in: namespace))
        } else {
            content
        }
    }
}
