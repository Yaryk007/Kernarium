// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import UniformTypeIdentifiers

/// The glass at the foot of the browser, which stays put while folders slide
/// under it: the trail of folders with Import or Export beside it, and above
/// them the transfer or the question an import is waiting on.
struct FilesBar: View {
    static let height: CGFloat = 50
    /// How far the fade behind the glass reaches above it. The reach is part
    /// of the bar's layout, which the pages leave free at the end of a scroll
    /// (`filesBarHeight`): content fades under the glass only while it moves.
    static let scrimReach: CGFloat = 28

    @Bindable var browser: FileBrowserModel
    /// Android's storage is up, so the guest can be browsed: the trail and
    /// Import or Export show. A transfer or a question stays in view
    /// regardless.
    let usable: Bool
    /// The sheet's own bottom inset, measured outside the bar: read here, the
    /// bar's padding would feed its own measurement.
    let bottomInset: CGFloat
    @State private var trailScrolled = false

    var body: some View {
        GlassEffectContainer(spacing: 10) {
            VStack(spacing: 10) {
                if browser.conflict != nil || browser.transfer != nil {
                    FileTransferSlot(browser: browser)
                }
                if browser.browsing, usable {
                    HStack(spacing: 10) {
                        trail
                        action
                    }
                    .transition(.opacity)
                }
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, Self.scrimReach)
        // The sheet's own margin from the bottom edge, as at the sides.
        .padding(.bottom, 16 - bottomInset)
        .background(alignment: .bottom) { scrim }
        .background {
            GeometryReader { Color.clear.preference(key: FilesBarHeightKey.self, value: $0.size.height) }
        }
        .animation(.snappy, value: browser.selection.isEmpty)
        .animation(.snappy, value: browser.route)
        .animation(.snappy, value: browser.transfer?.id)
        .animation(.snappy, value: browser.conflict == nil)
        .modifier(ImportPickerPresenter(browser: browser))
    }

    /// Rows fade out under the glass, so a name never reads through it. It
    /// fills the bar's frame, whose top `scrimReach` is its own.
    private var scrim: some View {
        LinearGradient(
            stops: [
                .init(color: Color(.systemGroupedBackground).opacity(0), location: 0),
                .init(color: Color(.systemGroupedBackground).opacity(0.94), location: 0.45),
                .init(color: Color(.systemGroupedBackground), location: 1),
            ],
            startPoint: .top, endPoint: .bottom
        )
        .ignoresSafeArea(edges: .bottom)
        .allowsHitTesting(false)
    }

    // MARK: Trail

    private var trail: some View {
        HStack(spacing: 8) {
            if !browser.selection.isEmpty {
                selectionBadge
                    .transition(.scale.combined(with: .opacity))
            }
            ScrollViewReader { proxy in
                ScrollView(.horizontal) {
                    HStack(spacing: 5) {
                        ForEach(Array(browser.route.enumerated()), id: \.element.path) { index, route in
                            if index > 0 {
                                Image(systemName: "chevron.compact.forward")
                                    .font(.caption.weight(.semibold))
                                    .foregroundStyle(Color(.tertiaryLabel))
                                    .accessibilityHidden(true)
                            }
                            crumb(index, route: route)
                                .id(route.path)
                        }
                    }
                }
                .scrollIndicators(.hidden)
                .defaultScrollAnchor(.leading, for: .alignment)
                .defaultScrollAnchor(.trailing, for: .initialOffset)
                .defaultScrollAnchor(.trailing, for: .sizeChanges)
                .scrollEdgeEffectHidden()
                .onScrollGeometryChange(for: Bool.self) { $0.contentOffset.x > 1 } action: { _, scrolled in
                    trailScrolled = scrolled
                }
                .mask {
                    HStack(spacing: 0) {
                        LinearGradient(colors: [.clear, .black], startPoint: .leading, endPoint: .trailing)
                            .frame(width: trailScrolled ? 18 : 0)
                        Color.black
                    }
                }
                .onChange(of: browser.route) { _, route in
                    guard let last = route.last else { return }
                    withAnimation(.snappy) { proxy.scrollTo(last.path, anchor: .trailing) }
                }
            }
        }
        .padding(.leading, 12)
        .padding(.trailing, 16)
        .frame(height: Self.height)
        .glassEffect(.regular.interactive(), in: .capsule)
    }

    private func crumb(_ index: Int, route: FolderRoute) -> some View {
        let last = index == browser.route.count - 1
        return Button {
            browser.jump(to: index)
        } label: {
            Group {
                if index > 0 {
                    Text(GuestPath.name(of: route.path))
                } else if browser.route.count == 1 {
                    Label("Internal storage", systemImage: "internaldrive")
                        .labelStyle(.titleAndIcon)
                } else {
                    Image(systemName: "internaldrive")
                        .accessibilityLabel("Internal storage")
                }
            }
            .font(.subheadline.weight(last ? .semibold : .regular))
            .foregroundStyle(last ? Color(.label) : Color(.secondaryLabel))
            .lineLimit(1)
            .padding(.vertical, 12)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
    }

    private var selectionBadge: some View {
        Button {
            browser.reviewingSelection = true
        } label: {
            Text("\(browser.selection.count)")
                .font(.footnote.weight(.bold))
                .monospacedDigit()
                .foregroundStyle(.white)
                .contentTransition(.numericText())
                .frame(minWidth: 28, minHeight: 28)
                .padding(.horizontal, browser.selection.count > 9 ? 4 : 0)
                .background(Color.accentColor, in: .capsule)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(browser.selection.count) selected")
        .accessibilityHint("Shows what is selected")
        .popover(isPresented: $browser.reviewingSelection, arrowEdge: .bottom) {
            SelectionReview(browser: browser)
                .presentationCompactAdaptation(.popover)
        }
    }

    // MARK: Action

    /// Import until something is selected, then Export.
    @ViewBuilder private var action: some View {
        Group {
            if browser.selection.isEmpty {
                Button {
                    browser.picking = true
                } label: {
                    actionLabel("Import", symbol: "square.and.arrow.down")
                }
                .buttonStyle(.glass)
                .disabled(browser.busy || !browser.canImport(into: browser.current))
            } else {
                Button {
                    browser.exportSelection()
                } label: {
                    actionLabel("Export", symbol: "square.and.arrow.up")
                }
                .buttonStyle(.glassProminent)
                .disabled(browser.busy)
            }
        }
        .buttonBorderShape(.capsule)
    }

    private func actionLabel(_ title: String, symbol: String) -> some View {
        Label(title, systemImage: symbol)
            .font(.subheadline.weight(.semibold))
            .labelStyle(.titleAndIcon)
            .frame(height: Self.height - 16)
            .frame(minWidth: 76)
    }
}

/// The bar's layout height, with its fade; there is no value while the bar is
/// not shown.
private struct FilesBarHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0

    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = max(value, nextValue())
    }
}

extension EnvironmentValues {
    /// How much of the sheet's bottom the bar covers, 0 without a bar. The
    /// bar's safe area does not reach the lists on the sheet's navigation
    /// stack, so each ends above the bar by this margin, on top of its own
    /// safe area.
    @Entry var filesBarHeight: CGFloat = 0
}

extension View {
    /// Reads the height of the bar shown within `self`, 0 while there is none.
    func readFilesBarHeight(into height: Binding<CGFloat>) -> some View {
        onPreferenceChange(FilesBarHeightKey.self) { height.wrappedValue = $0 }
    }

    /// Ends a list's content above the bar, and leaves the list's own bottom
    /// margin alone while there is no bar.
    func endingAboveFilesBar(_ height: CGFloat) -> some View {
        contentMargins(.bottom, height > 0 ? height : nil, for: .scrollContent)
    }
}

private extension View {
    /// The trail scrolls inside its capsule, which needs no scroll-edge
    /// treatment of its own.
    func scrollEdgeEffectHidden() -> some View {
        scrollEdgeEffectStyle(.none, for: .horizontal)
    }
}

/// The system document picker, or what the preview shows in its place.
struct ImportPickerPresenter: ViewModifier {
    @Bindable var browser: FileBrowserModel

    func body(content: Content) -> some View {
        if let standIn = browser.backend.importPickerStandIn(deliver: { browser.received($0) }) {
            content.sheet(isPresented: $browser.picking) { standIn }
        } else {
            content.fileImporter(
                isPresented: $browser.picking, allowedContentTypes: [.item], allowsMultipleSelection: true
            ) { result in
                guard case let .success(urls) = result else { return }
                browser.received(urls.map(FilePick.picked))
            }
        }
    }
}

/// What sits above the trail: a question, or the transfer.
struct FileTransferSlot: View {
    let browser: FileBrowserModel

    var body: some View {
        if let conflict = browser.conflict {
            ConflictCard(conflict: conflict, browser: browser)
                .transition(.move(edge: .bottom).combined(with: .opacity))
        } else if let transfer = browser.transfer {
            TransferCapsule(transfer: transfer, browser: browser)
                .transition(.move(edge: .bottom).combined(with: .opacity))
        }
    }
}

/// The transfer while the person goes on browsing. The capsule fills as it
/// goes; a tap opens it to show every file. It ends here too.
struct TransferCapsule: View {
    let transfer: FileTransfer
    let browser: FileBrowserModel

    private var radius: CGFloat { transfer.expanded ? 24 : FilesBar.height * 0.6 }

    var body: some View {
        VStack(spacing: 0) {
            if transfer.expanded {
                items
                Divider().padding(.horizontal, 16)
            }
            summary
        }
        .clipShape(.rect(cornerRadius: radius, style: .continuous))
        .glassEffect(glass, in: .rect(cornerRadius: radius, style: .continuous))
        .animation(.snappy, value: transfer.expanded)
        .animation(.snappy, value: transfer.stage)
    }

    private var glass: Glass {
        transfer.stage == .failed ? .regular.tint(.red.opacity(0.16)) : .regular
    }

    private var summary: some View {
        HStack(spacing: 12) {
            glyph
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(transfer.title)
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if transfer.items.count > 1 {
                        Image(systemName: transfer.expanded ? "chevron.down" : "chevron.up")
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.secondary)
                            .accessibilityHidden(true)
                    }
                }
                Text(transfer.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .monospacedDigit()
                    .contentTransition(.numericText())
            }
            Spacer(minLength: 6)
            actions
        }
        .padding(.leading, 12)
        .padding(.trailing, 14)
        .frame(minHeight: 60)
        .background(alignment: .leading) { progressFill }
        .contentShape(.rect)
        .onTapGesture { if transfer.items.count > 1 { transfer.expanded.toggle() } }
        .accessibilityAddTraits(transfer.items.count > 1 ? .isButton : [])
        .accessibilityHint(transfer.items.count > 1 ? (transfer.expanded ? "Hides the files" : "Shows the files") : "")
    }

    @ViewBuilder private var progressFill: some View {
        if transfer.running {
            GeometryReader { geometry in
                UnevenRoundedRectangle(bottomTrailingRadius: 10, topTrailingRadius: 10, style: .continuous)
                    .fill(Color.accentColor.opacity(0.2))
                    .frame(width: geometry.size.width * transfer.fraction)
                    .animation(.linear(duration: 0.15), value: transfer.fraction)
            }
        }
    }

    private var glyph: some View {
        Image(systemName: glyphName)
            .font(.system(size: 15, weight: .bold))
            .foregroundStyle(.white)
            .contentTransition(.symbolEffect(.replace))
            .frame(width: 36, height: 36)
            .background(glyphColor.gradient, in: .circle)
            .accessibilityHidden(true)
    }

    private var glyphName: String {
        switch transfer.stage {
        case .failed: "exclamationmark"
        case .finished: "checkmark"
        case .ready: "square.and.arrow.up"
        case .running: transfer.direction == .importing ? "arrow.down" : "arrow.up"
        }
    }

    private var glyphColor: Color {
        transfer.stage == .failed ? .red : .accentColor
    }

    @ViewBuilder private var actions: some View {
        switch transfer.stage {
        case .running:
            Button("Cancel") { browser.closeTransfer() }
                .font(.subheadline.weight(.medium))
                .buttonStyle(.borderless)
        case .ready:
            Button("Share") { browser.share(transfer) }
                .font(.subheadline.weight(.semibold))
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.capsule)
                .controlSize(.small)
            closeButton
        case .finished:
            closeButton
        case .failed:
            Button("Try Again") { browser.retryFailed() }
                .font(.subheadline.weight(.medium))
                .buttonStyle(.borderless)
            closeButton
        }
    }

    private var closeButton: some View {
        Button {
            browser.closeTransfer()
        } label: {
            Image(systemName: "xmark")
                .font(.footnote.weight(.semibold))
                .frame(width: 28, height: 36)
        }
        .buttonStyle(.borderless)
        .tint(Color(.secondaryLabel))
        .accessibilityLabel("Close")
    }

    private var items: some View {
        ScrollView {
            VStack(spacing: 0) {
                ForEach(transfer.items) { item in
                    row(item)
                }
            }
            .padding(.vertical, 8)
        }
        .frame(maxHeight: min(CGFloat(transfer.items.count) * 46 + 16, 240))
        .fixedSize(horizontal: false, vertical: true)
    }

    private func row(_ item: FileTransfer.Item) -> some View {
        HStack(spacing: 12) {
            FileGlyph(name: item.name, isFolder: item.isFolder, size: 26)
            VStack(alignment: .leading, spacing: 1) {
                Text(item.name)
                    .font(.subheadline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                if case let .failed(reason) = item.state {
                    Text(reason)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 8)
            status(item)
        }
        .padding(.horizontal, 16)
        .frame(minHeight: 44)
    }

    @ViewBuilder private func status(_ item: FileTransfer.Item) -> some View {
        switch item.state {
        case .waiting:
            Text(Formats.size(bytes: item.size))
                .font(.caption)
                .foregroundStyle(.secondary)
                .monospacedDigit()
        case .moving:
            Text("\(Formats.size(bytes: item.moved)) of \(Formats.size(bytes: item.size))")
                .font(.caption.weight(.medium))
                .foregroundStyle(Color.accentColor)
                .monospacedDigit()
        case .done:
            Image(systemName: "checkmark")
                .font(.footnote.weight(.bold))
                .foregroundStyle(Color.accentColor)
                .accessibilityLabel("Done")
        case .failed:
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.footnote)
                .foregroundStyle(.red)
                .accessibilityLabel("Failed")
        }
    }
}

/// An import whose names are already in the folder. It asks once, for all of
/// them; the files with new names go either way.
struct ConflictCard: View {
    let conflict: ImportConflict
    let browser: FileBrowserModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(.subheadline.weight(.semibold))
                        .fixedSize(horizontal: false, vertical: true)
                    if let others {
                        Text(others)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
                Button {
                    browser.cancelConflict()
                } label: {
                    Image(systemName: "xmark")
                        .font(.footnote.weight(.semibold))
                        .frame(width: 28, height: 28)
                }
                .buttonStyle(.borderless)
                .tint(Color(.secondaryLabel))
                .accessibilityLabel("Cancel import")
            }
            HStack(spacing: 8) {
                Button { browser.resolveConflict(.skip) } label: { choice("Skip") }
                    .buttonStyle(.bordered)
                    .tint(Color(.label))
                Button(role: .destructive) { browser.resolveConflict(.replace) } label: { choice("Replace") }
                    .buttonStyle(.bordered)
                    .tint(.red)
                Button { browser.resolveConflict(.keepBoth) } label: { choice("Keep Both") }
                    .buttonStyle(.borderedProminent)
            }
            .buttonBorderShape(.capsule)
        }
        .padding(16)
        .glassEffect(.regular, in: .rect(cornerRadius: 24, style: .continuous))
    }

    private func choice(_ title: String) -> some View {
        Text(title)
            .font(.subheadline.weight(.medium))
            .lineLimit(1)
            .minimumScaleFactor(0.85)
            .frame(maxWidth: .infinity)
    }

    private var folderName: String { GuestPath.name(of: conflict.folder) }

    private var title: String {
        if conflict.clashing.count == 1, let only = conflict.clashing.first {
            return "\u{201C}\(only.name)\u{201D} is already in \(folderName)"
        }
        return "\(conflict.clashing.count) names are already in \(folderName)"
    }

    /// The names a count stands for.
    private var others: String? {
        guard conflict.clashing.count > 1 else { return nil }
        let shown = conflict.clashing.prefix(2).map(\.name)
        let more = conflict.clashing.count - shown.count
        return ListFormatter.localizedString(byJoining: shown + (more > 0 ? ["\(more) more"] : []))
    }
}
