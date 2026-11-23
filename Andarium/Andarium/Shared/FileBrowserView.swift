// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// One folder of the guest's internal storage, pushed on the Settings sheet.
/// A tap goes into a folder and selects a file: there is nothing else a file
/// can do here, so selecting needs no mode.
struct FolderPage: View {
    @Environment(AppModel.self) private var model
    @Environment(\.filesBarHeight) private var barHeight
    let browser: FileBrowserModel
    let route: FolderRoute
    let close: () -> Void
    @State private var wide = false
    /// The Modified column's width, from the widest date this language writes.
    @State private var dateWidth = FileRow.minimumDateWidth

    private var path: String { route.path }

    var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(.systemGroupedBackground))
            .background { FileDateProbe(width: $dateWidth) }
            .onGeometryChange(for: Bool.self) { $0.size.width >= 520 } action: { wide = $0 }
            .navigationTitle(GuestPath.name(of: path))
            .navigationSubtitle(subtitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { toolbar }
            .task(id: LoadKey(path: path, available: model.filesAvailable)) {
                if model.filesAvailable { await browser.load(path) }
            }
    }

    /// What a folder is read again for: another folder, or Android's storage
    /// coming up.
    private struct LoadKey: Hashable {
        var path: String
        var available: Bool
    }

    /// Android's storage is not known yet: Settings reads it as it opens.
    private var checking: Bool {
        model.driver != nil && model.session.storageReady == nil
    }

    /// The selection when there is one, otherwise the folder.
    private var subtitle: String {
        guard model.filesAvailable else { return "" }
        if !browser.selection.isEmpty { return browser.selectionSummary }
        guard case let .loaded(files) = browser.listing(path) else { return "" }
        let items = String(localized: "\(files.count) items")
        guard let free = browser.freeBytes else { return items }
        return String(localized: "\(items) \u{00B7} \(Formats.size(bytes: free)) free")
    }

    @ViewBuilder private var content: some View {
        if checking {
            loading
        } else if !model.filesAvailable {
            ContentUnavailableView {
                Label("Android is still starting", systemImage: "hourglass")
            } description: {
                Text("Its storage isn\u{2019}t ready yet. The files appear here in a moment.")
            }
        } else {
            switch browser.listing(path) {
            case nil:
                Color.clear
            case .loading:
                loading
            case .refused:
                ContentUnavailableView {
                    Label("No access", systemImage: "lock.fill")
                } description: {
                    Text("Android doesn\u{2019}t allow this folder to be opened.")
                }
            case let .failed(reason):
                ContentUnavailableView {
                    Label("Couldn\u{2019}t open this folder", systemImage: "exclamationmark.triangle.fill")
                } description: {
                    Text(reason)
                } actions: {
                    Button("Try Again") { Task { await browser.load(path) } }
                }
            case .loaded:
                let files = browser.entries(in: path)
                if files.isEmpty {
                    ContentUnavailableView("Empty folder", systemImage: "folder")
                } else {
                    list(files)
                }
            }
        }
    }

    private func list(_ files: [GuestFile]) -> some View {
        List {
            Section {
                ForEach(files) { file in
                    row(file)
                }
            } header: {
                if wide { columnHeader }
            }
        }
        .listSectionSpacing(.compact)
        .contentMargins(.top, wide ? 0 : 6, for: .scrollContent)
        .endingAboveFilesBar(barHeight)
    }

    @ViewBuilder private func row(_ file: GuestFile) -> some View {
        let selected = browser.isSelected(file)
        let label = FileRow(
            file: file, selected: selected, fresh: browser.fresh.contains(file.path), wide: wide,
            dateWidth: dateWidth,
            toggle: { withAnimation(.snappy) { browser.toggle(file) } }
        )
        Button {
            if file.isFolder {
                browser.route.append(FolderRoute(path: file.path))
            } else {
                withAnimation(.snappy) { browser.toggle(file) }
            }
        } label: {
            label
        }
        .listRowBackground(selected ? Color.accentColor.opacity(0.14) : Color(.secondarySystemGroupedBackground))
        .accessibilityAddTraits(selected ? .isSelected : [])
        .contextMenu {
            if file.isFolder {
                Button(selected ? "Deselect" : "Select", systemImage: selected ? "minus.circle" : "checkmark.circle") {
                    withAnimation(.snappy) { browser.toggle(file) }
                }
            }
            Button("Export", systemImage: "square.and.arrow.up") {
                browser.export([file])
            }
            .disabled(browser.busy)
        }
    }

    private var columnHeader: some View {
        HStack(spacing: 12) {
            Color.clear.frame(width: FileRow.tileSize, height: 1)
            columnButton(.name, title: String(localized: "Name"))
                .frame(maxWidth: .infinity, alignment: .leading)
            columnButton(.date, title: String(localized: "Modified"))
                .frame(width: dateWidth, alignment: .leading)
            columnButton(.size, title: String(localized: "Size"))
                .frame(width: FileRow.sizeWidth, alignment: .trailing)
            Color.clear.frame(width: FileRow.chevronWidth, height: 1)
        }
        .textCase(nil)
    }

    private func columnButton(_ sort: FileSort, title: String) -> some View {
        Button {
            withAnimation(.snappy) { browser.sort = sort }
        } label: {
            HStack(spacing: 3) {
                Text(title)
                if browser.sort == sort {
                    Image(systemName: "chevron.down")
                        .font(.caption2.weight(.bold))
                }
            }
            .font(.footnote.weight(browser.sort == sort ? .semibold : .regular))
            .foregroundStyle(browser.sort == sort ? Color(.label) : Color(.secondaryLabel))
        }
        .buttonStyle(.plain)
    }

    private var loading: some View {
        List {
            Section {
                ForEach(0..<9, id: \.self) { index in
                    FileRow.placeholder(index: index, wide: wide, dateWidth: dateWidth)
                }
            } header: {
                if wide { columnHeader.unredacted() }
            }
        }
        .listSectionSpacing(.compact)
        .contentMargins(.top, wide ? 0 : 6, for: .scrollContent)
        .endingAboveFilesBar(barHeight)
        .redacted(reason: .placeholder)
        .allowsHitTesting(false)
    }

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        ToolbarItem(placement: .topBarTrailing) {
            @Bindable var browser = browser
            Menu {
                if browser.allSelected(in: path) {
                    Button("Clear Selection", systemImage: "checkmark.circle.badge.xmark") {
                        withAnimation(.snappy) { browser.clearSelection() }
                    }
                } else {
                    Button("Select All", systemImage: "checkmark.circle") {
                        withAnimation(.snappy) { browser.selectAll(in: path) }
                    }
                    .disabled(browser.entries(in: path).allSatisfy(\.isFolder))
                }
                Picker("Sort By", systemImage: "arrow.up.arrow.down", selection: $browser.sort.animation(.snappy)) {
                    ForEach(FileSort.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.menu)
            } label: {
                Label("More", systemImage: "ellipsis")
            }
            .disabled(!model.filesAvailable)
        }
        // On the folding iPhone's cover the buttons stand in a column beside
        // the camera, and Import joins them as a round button after More and
        // before Done. It is the bar's Import (`FilesBar`) moved: the same
        // action, the same disabled states, and it gives way to Export, which
        // stays in the bar, while something is selected.
        if model.foldPose.importIsInToolbar, model.filesAvailable, browser.selection.isEmpty {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    browser.picking = true
                } label: {
                    Label("Import", systemImage: "square.and.arrow.down")
                        .labelStyle(.iconOnly)
                }
                .disabled(browser.busy || !browser.canImport(into: browser.current))
            }
        }
        ToolbarSpacer(.fixed, placement: .topBarTrailing)
        ToolbarItem(placement: .confirmationAction) {
            Button("Done", systemImage: "checkmark", action: close)
        }
    }
}

/// A row of a folder. The icon is what selection shows on: it turns into a
/// check, on a file and on a folder alike. A tap on a folder's icon selects
/// it, since a tap anywhere else goes into it.
struct FileRow: View {
    static let tileSize: CGFloat = 32
    /// What the Modified column keeps in English; a language that writes its
    /// dates wider gets the width its widest date measures (`FileDateProbe`).
    static let minimumDateWidth: CGFloat = 104
    static let sizeWidth: CGFloat = 76
    static let chevronWidth: CGFloat = 14

    let file: GuestFile
    let selected: Bool
    let fresh: Bool
    let wide: Bool
    let dateWidth: CGFloat
    let toggle: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            tileControl
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text(file.name)
                        .font(.body)
                        .foregroundStyle(Color(.label))
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if fresh {
                        Circle()
                            .fill(Color.accentColor)
                            .frame(width: 7, height: 7)
                            .accessibilityLabel("New")
                    }
                }
                if !wide {
                    Text(details)
                        .font(.footnote)
                        .foregroundStyle(Color(.secondaryLabel))
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 8)
            if wide {
                // One line whatever the language: the column is as wide as the
                // widest date needs, and the scale factor takes up a digit that
                // is a hair wider than the measured one.
                Text(file.modified, format: FileDate.style)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                    .frame(width: dateWidth, alignment: .leading)
                Text(file.isFolder ? "\u{2013}" : Formats.size(bytes: file.size))
                    .frame(width: Self.sizeWidth, alignment: .trailing)
                    .monospacedDigit()
            }
            // In columns a file keeps the slot, so its date and size line up
            // with a folder's.
            if file.isFolder || wide {
                Image(systemName: "chevron.forward")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Color(.tertiaryLabel))
                    .opacity(file.isFolder ? 1 : 0)
                    .frame(width: Self.chevronWidth, alignment: .trailing)
                    .accessibilityHidden(true)
            }
        }
        .font(.subheadline)
        .foregroundStyle(Color(.secondaryLabel))
        .contentShape(.rect)
    }

    private var date: String { file.modified.formatted(FileDate.style) }

    private var details: String {
        file.isFolder ? date : "\(date) \u{00B7} \(Formats.size(bytes: file.size))"
    }

    @ViewBuilder private var tileControl: some View {
        if file.isFolder {
            Button(action: toggle) { glyph }
                .buttonStyle(.borderless)
                .accessibilityLabel(selected ? String(localized: "Deselect \(file.name)") : String(localized: "Select \(file.name)"))
        } else {
            glyph
        }
    }

    private var glyph: some View {
        FileGlyph(file: file, size: Self.tileSize, selected: selected)
    }

    /// A row the size of a real one, for the folder that has not answered.
    static func placeholder(index: Int, wide: Bool, dateWidth: CGFloat) -> some View {
        let names = ["IMG_20261102_140233", "Holiday recording", "IMG_20261031_0932", "Screen capture 04"]
        return FileRow(
            file: GuestFile(
                path: GuestPath.child(names[index % names.count], of: GuestPath.root), isFolder: false,
                size: 4_200_000, modified: .now
            ),
            selected: false, fresh: false, wide: wide, dateWidth: dateWidth, toggle: {}
        )
    }
}

/// How a file's date is written, in the Modified column and under the name.
enum FileDate {
    static let style = Date.FormatStyle(date: .abbreviated, time: .omitted)

    /// The dates that decide how wide the Modified column must be: the 28th of
    /// each of twelve consecutive months, which has two digits in every month,
    /// so the longest month name or number and a two-digit day are among them.
    static func widest(in calendar: Calendar, from now: Date = .now) -> [Date] {
        (0..<12).compactMap { offset in
            guard let month = calendar.date(byAdding: .month, value: -offset, to: now),
                let start = calendar.dateInterval(of: .month, for: month)?.start
            else { return nil }
            return calendar.date(byAdding: .day, value: 27, to: start)
        }
    }
}

/// Measures the widest date the Modified column can show, as the column draws
/// it: the same font, language and calendar, hidden and out of the layout.
/// `width` keeps at least the English width.
private struct FileDateProbe: View {
    @Binding var width: CGFloat
    @Environment(\.calendar) private var calendar

    var body: some View {
        ZStack {
            ForEach(FileDate.widest(in: calendar), id: \.self) { date in
                Text(date, format: FileDate.style)
            }
        }
        .font(.subheadline)
        .fixedSize()
        .onGeometryChange(for: CGFloat.self) { ceil($0.size.width) } action: {
            width = max(FileRow.minimumDateWidth, $0)
        }
        .hidden()
    }
}

/// Shows what is selected, from any folder, with a way to take each back.
struct SelectionReview: View {
    let browser: FileBrowserModel

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(browser.selectionSummary)
                    .font(.subheadline.weight(.semibold))
                Spacer()
                Button("Clear") { withAnimation(.snappy) { browser.clearSelection() } }
                    .font(.subheadline)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            Divider()
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(browser.selection) { file in
                        row(file)
                    }
                }
                .padding(.vertical, 4)
            }
        }
        .frame(width: 320, height: min(CGFloat(browser.selection.count) * 56 + 58, 340))
    }

    private func row(_ file: GuestFile) -> some View {
        HStack(spacing: 12) {
            FileGlyph(file: file, size: 28)
            VStack(alignment: .leading, spacing: 1) {
                Text(file.name)
                    .font(.subheadline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(GuestPath.parentName(of: file.path))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer(minLength: 4)
            Button {
                withAnimation(.snappy) { browser.toggle(file) }
                if browser.selection.isEmpty { browser.reviewingSelection = false }
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.body)
                    .foregroundStyle(Color(.tertiaryLabel))
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Remove \(file.name)")
        }
        .padding(.horizontal, 14)
        .frame(minHeight: 52)
    }
}
