// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Manage on Home's Snapshots row: every snapshot, newest first, with Take
/// snapshot at the foot as Start is on Home. On iPhone a snapshot's page is
/// pushed; on iPad the list and the selected page stand side by side. The
/// sheet opens only from Home, which exists only while the guest is stopped.
struct SnapshotsSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        @Bindable var store = model.snapshots
        GeometryReader { geo in
            let wide = geo.size.width >= 660 && !store.items.isEmpty
            NavigationStack(path: $store.route) {
                Group {
                    if wide {
                        SnapshotsSplit()
                    } else {
                        SnapshotList(selectable: false)
                    }
                }
                .background(Color(.systemGroupedBackground))
                .navigationTitle(SnapshotWords.title)
                .navigationSubtitle(subtitle)
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) {
                        if store.isWorking {
                            ProgressView().accessibilityLabel("Working")
                        }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                            .accessibilityIdentifier("snapshots-done")
                    }
                }
                .navigationDestination(for: SnapshotPage.self) { page in
                    switch page {
                    case .snapshot(let id): SnapshotDetail(id: id, pushed: true).id(id)
                    case .space: SnapshotSpacePage(pushed: true)
                    }
                }
            }
        }
        .modifier(SnapshotPrompts())
        .presentationSizing(FixedSheetSizing(width: 780, height: 820))
        .task { await store.reload() }
        // The sheet opens at its list again next time.
        .onDisappear {
            Task { await store.flushName() }
            store.route = []
            store.problem = nil
        }
    }

    /// "4.6 GiB on this iPhone" under the title; nothing while there are none.
    private var subtitle: String {
        let store = model.snapshots
        guard !store.items.isEmpty, let total = store.total else { return "" }
        return SnapshotWords.sheetSubtitle(total: total, device: model.host.name)
    }
}

/// iPad: the list on the left, the selected snapshot or the space page on
/// the right.
struct SnapshotsSplit: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        HStack(spacing: 0) {
            SnapshotList(selectable: true)
                .frame(width: 340)
            Divider().ignoresSafeArea(edges: .bottom)
            Group {
                switch model.snapshots.selection {
                case .snapshot(let id): SnapshotDetail(id: id, pushed: false).id(id)
                case .space: SnapshotSpacePage(pushed: false)
                case nil:
                    ContentUnavailableView("No snapshot selected", systemImage: "clock.arrow.circlepath")
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color(.systemGroupedBackground))
        }
    }
}

/// The snapshots, newest first, then the link to how they use space; the
/// empty state when there are none. Take snapshot rides at the foot.
struct SnapshotList: View {
    @Environment(AppModel.self) private var model
    /// iPad: rows select instead of pushing.
    let selectable: Bool

    var body: some View {
        @Bindable var store = model.snapshots
        Group {
            if store.listing == nil {
                ProgressView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if store.items.isEmpty {
                ContentUnavailableView {
                    Label("No snapshots", systemImage: "clock.arrow.circlepath")
                } description: {
                    Text("Take one to keep the virtual machine\u{2019}s data as it is now and go back to it later.")
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(selection: selectable ? $store.selection : nil) {
                    Section {
                        ForEach(store.items) { item in
                            row(item)
                        }
                    }
                    Section {
                        link(.space) {
                            Label(SnapshotWords.spaceLink, systemImage: "chart.pie")
                        }
                        .accessibilityIdentifier("snapshots-space")
                    }
                }
            }
        }
        .safeAreaBar(edge: .bottom) { SnapshotTakeBar() }
        // The take prompt's keyboard covers the bar instead of lifting it.
        .ignoresSafeArea(.keyboard)
    }

    @ViewBuilder
    private func row(_ item: SnapshotSummary) -> some View {
        let store = model.snapshots
        let restoreRefusal = store.restoreRefusal(of: item)
        link(.snapshot(item.id)) {
            SnapshotRow(item: item, frees: store.frees(item))
        }
        .listRowBackground(store.fresh == item.id ? Color.accentColor.opacity(0.16) : nil)
        .animation(.default, value: store.fresh)
        .swipeActions(edge: .trailing) {
            Button {
                store.deleteTarget = item.id
            } label: {
                Label("Delete", systemImage: "trash")
            }
            .tint(.red)
            .disabled(store.deleteRefusal(of: item) != nil)
        }
        .contextMenu {
            Button {
                store.restoreTarget = item.id
            } label: {
                Label("Restore\u{2026}", systemImage: "clock.arrow.circlepath")
            }
            .disabled(restoreRefusal != nil)
            Button {
                store.requestRename(item.id)
            } label: {
                Label("Rename\u{2026}", systemImage: "pencil")
            }
            Button(role: .destructive) {
                store.deleteTarget = item.id
            } label: {
                Label("Delete\u{2026}", systemImage: "trash")
            }
            .disabled(store.deleteRefusal(of: item) != nil)
        }
        .accessibilityIdentifier("snapshot-row")
    }

    /// A row that pushes its page on iPhone and selects it on iPad.
    @ViewBuilder
    private func link<Label: View>(_ page: SnapshotPage, @ViewBuilder label: () -> Label) -> some View {
        if selectable {
            label().tag(page)
        } else {
            NavigationLink(value: page, label: label)
        }
    }
}

/// Facts of a row that never break apart: each choice is a set of lines that
/// do not wrap, and the first that fits the column is the one shown, so the
/// release never stands alone after a dangling dot and a time of day (which
/// Chinese writes as "晚上7:20") never splits in two. `wrapped` is the
/// last resort, for a column narrower than every choice.
private struct Facts: View {
    let one: [String]
    var two: [String]?
    var three: [String]?
    let wrapped: String

    var body: some View {
        ViewThatFits(in: .horizontal) {
            lines(one)
            lines(two ?? one)
            lines(three ?? two ?? one)
            Text(wrapped)
        }
    }

    private func lines(_ texts: [String]) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(texts.enumerated()), id: \.offset) { _, text in
                Text(text).lineLimit(1)
            }
        }
    }
}

/// One snapshot in the list: its name (or when it was taken), when and from
/// which system image, and what deleting it frees.
struct SnapshotRow: View {
    let item: SnapshotSummary
    /// What deleting it frees; nil while it is not measured.
    let frees: SnapshotFigure?

    var body: some View {
        HStack(spacing: 12) {
            words
            Spacer(minLength: 8)
            size
        }
        .padding(.vertical, 2)
    }

    private var words: some View {
        VStack(alignment: .leading, spacing: 2) {
            if item.name.isEmpty {
                // The date is the title; the release is the line under it.
                Facts(
                    one: [SnapshotWords.taken(item.created)],
                    two: [SnapshotWords.takenDate(item.created), SnapshotWords.takenTime(item.created)],
                    wrapped: SnapshotWords.taken(item.created)
                )
                .fontWeight(.medium)
                Text(SnapshotWords.revision(of: item.releaseID)).font(.subheadline).foregroundStyle(.secondary)
            } else {
                Text(SnapshotWords.title(of: item))
                    .fontWeight(.medium)
                Facts(
                    one: [SnapshotWords.rowDetail(of: item)],
                    two: [SnapshotWords.taken(item.created), SnapshotWords.revision(of: item.releaseID)],
                    three: [
                        SnapshotWords.takenDate(item.created), SnapshotWords.takenTime(item.created),
                        SnapshotWords.revision(of: item.releaseID),
                    ],
                    wrapped: SnapshotWords.rowDetail(of: item)
                )
                .font(.subheadline)
                .foregroundStyle(.secondary)
            }
            if let flag = SnapshotWords.rowFlag(item.restore) {
                Text(flag)
                    .font(.subheadline)
                    .foregroundStyle(Color.orange)
            }
        }
        .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder
    private var size: some View {
        if let frees {
            Text(SnapshotWords.size(frees))
                .font(.subheadline)
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .accessibilityLabel(SnapshotWords.freesAccessibility(frees))
        } else {
            ProgressView()
                .controlSize(.small)
                .accessibilityLabel("Measuring")
        }
    }
}

/// Take snapshot at the foot of the list, and why it is unavailable when it is.
struct SnapshotTakeBar: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let store = model.snapshots
        let refusal = store.takeRefusal
        VStack(spacing: 10) {
            if let reason = store.takeReason {
                Text(reason)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 8)
                    .accessibilityIdentifier("snapshots-take-reason")
            }
            Button {
                store.requestTake()
            } label: {
                Label(SnapshotWords.take, systemImage: "plus")
                    .frame(maxWidth: .infinity)
                    .frame(minHeight: 50)
                    .contentShape(Capsule())
            }
            .buttonStyle(.plain)
            .font(.body.weight(.semibold))
            .foregroundStyle(.white)
            .background(Capsule().fill(Color.accentColor))
            .glassEffect(.regular.tint(Color.accentColor).interactive(), in: .capsule)
            .opacity(refusal == nil ? 1 : 0.4)
            .disabled(refusal != nil)
            .accessibilityIdentifier("snapshots-take")
        }
        .frame(maxWidth: 520)
        .padding(.horizontal, 16)
        .padding(.top, 14)
        .padding(.bottom, 6)
        .frame(maxWidth: .infinity)
        .background {
            LinearGradient(
                stops: [
                    .init(color: Color(.systemGroupedBackground).opacity(0), location: 0),
                    .init(color: Color(.systemGroupedBackground).opacity(0.92), location: 0.3),
                    .init(color: Color(.systemGroupedBackground), location: 0.6),
                ],
                startPoint: .top, endPoint: .bottom
            )
            .ignoresSafeArea(edges: .bottom)
            .allowsHitTesting(false)
        }
    }
}

/// A snapshot's page: its name, what it is, Restore and Delete.
struct SnapshotDetail: View {
    @Environment(AppModel.self) private var model
    @FocusState private var nameFocused: Bool
    /// What the name field holds while it is being typed in; nil shows the
    /// snapshot's own name.
    @State private var draft: String?
    let id: SnapshotID
    /// Pushed on iPhone (it titles the bar); the right pane on iPad.
    let pushed: Bool

    var body: some View {
        let store = model.snapshots
        if let item = store.item(id) {
            let restoreRefusal = store.restoreRefusal(of: item)
            let title = title(of: item)
            List {
                if !pushed {
                    Section {
                        Text(title)
                            .font(.title2.weight(.bold))
                            .listRowBackground(Color.clear)
                            .listRowInsets(EdgeInsets(top: 4, leading: 4, bottom: 0, trailing: 4))
                    }
                }
                Section("Name") {
                    TextField("Name", text: nameBinding(item), prompt: Text("Add a name"))
                        .focused($nameFocused)
                        .submitLabel(.done)
                        .onSubmit { Task { await store.flushName() } }
                        .accessibilityIdentifier("snapshot-name")
                }
                Section {
                    LabeledContent("Taken", value: SnapshotWords.takenLong(item.created))
                    LabeledContent("System image", value: SnapshotWords.revision(of: item.releaseID))
                    LabeledContent("Frees if deleted") {
                        figure(store.frees(item))
                    }
                    LabeledContent("Virtual machine data") {
                        figure(store.kept(item))
                    }
                }
                Section {
                    Button {
                        store.restoreTarget = id
                    } label: {
                        Text("Restore")
                            .font(.body.weight(.semibold))
                            .frame(maxWidth: .infinity, minHeight: 34)
                    }
                    .buttonStyle(.glassProminent)
                    .disabled(restoreRefusal != nil)
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
                    .accessibilityIdentifier("snapshot-restore")
                } footer: {
                    Text(store.restoreNote(of: item))
                        .padding(.top, 6)
                        .accessibilityIdentifier("snapshot-restore-note")
                }
                Section {
                    Button("Delete snapshot\u{2026}", role: .destructive) {
                        store.deleteTarget = id
                    }
                    .disabled(store.deleteRefusal(of: item) != nil)
                    .accessibilityIdentifier("snapshot-delete")
                }
            }
            .modifier(PageTitle(title: title, pushed: pushed))
            .onAppear {
                if store.focusName {
                    store.focusName = false
                    Task { @MainActor in
                        try? await Task.sleep(for: .milliseconds(500))
                        nameFocused = true
                    }
                }
            }
            .onDisappear { Task { await store.flushName() } }
        } else {
            ContentUnavailableView("No snapshot selected", systemImage: "clock.arrow.circlepath")
        }
    }

    /// The name as typed, or the date where there is none.
    private func title(of item: SnapshotSummary) -> String {
        guard let draft else { return SnapshotWords.title(of: item) }
        let name = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        return name.isEmpty ? SnapshotWords.taken(item.created) : name
    }

    /// Renaming is as it is typed: the name is saved a moment after the last key.
    private func nameBinding(_ item: SnapshotSummary) -> Binding<String> {
        Binding(
            get: { draft ?? item.name },
            set: { value in
                let kept = String(value.prefix(SnapshotRules.maximumNameLength))
                draft = kept
                model.snapshots.edit(name: kept, of: id)
            }
        )
    }

    @ViewBuilder
    private func figure(_ figure: SnapshotFigure?) -> some View {
        if let figure {
            Text(SnapshotWords.size(figure)).monospacedDigit()
        } else {
            ProgressView().controlSize(.small)
        }
    }
}

/// How snapshots use space, with the figures that say it.
struct SnapshotSpacePage: View {
    @Environment(AppModel.self) private var model
    let pushed: Bool

    var body: some View {
        let store = model.snapshots
        let device = model.host.name
        List {
            if !pushed {
                Section {
                    Text(SnapshotWords.spacePageTitle)
                        .font(.title2.weight(.bold))
                        .listRowBackground(Color.clear)
                        .listRowInsets(EdgeInsets(top: 4, leading: 4, bottom: 0, trailing: 4))
                }
            }
            Section {
                LabeledContent("All snapshots") {
                    if let total = store.total {
                        Text(SnapshotWords.size(total)).monospacedDigit()
                    } else {
                        ProgressView().controlSize(.small)
                    }
                }
                if let guest = model.guest {
                    LabeledContent("Virtual machine data", value: Formats.size(bytes: guest.usedBytes))
                }
                if let copy = store.updateCopy {
                    LabeledContent("Unfinished update", value: SnapshotWords.size(copy))
                }
                LabeledContent("Free on this \(device)", value: Formats.size(bytes: model.host.freeStorageBytes))
            }
            Section {
                ForEach(
                    SnapshotWords.spaceParagraphs(
                        device: device, lowerBound: store.total?.isLowerBound ?? false, hasUpdateCopy: store.updateCopy != nil),
                    id: \.self
                ) { paragraph in
                    Text(paragraph)
                }
            }
            .font(.subheadline)
            .foregroundStyle(.secondary)
        }
        .modifier(PageTitle(title: SnapshotWords.spacePageTitle, pushed: pushed))
    }
}

/// A pushed page titles the bar, inline; the right pane on iPad leaves the
/// sheet's own title alone and heads itself.
struct PageTitle: ViewModifier {
    let title: String
    let pushed: Bool

    func body(content: Content) -> some View {
        if pushed {
            content
                .navigationTitle(title)
                .navigationBarTitleDisplayMode(.inline)
        } else {
            content
        }
    }
}

/// The prompts of the Snapshots sheet: the name for a new snapshot, a new
/// name from the list's menu, the confirmations of Restore and Delete, and
/// an operation that did not go ahead.
struct SnapshotPrompts: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var store = model.snapshots
        let endsUpdate = store.endsUpdate
        content
            .alert(SnapshotWords.take, isPresented: $store.taking) {
                TextField("Name (optional)", text: $store.takeName)
                Button("Cancel", role: .cancel) {}
                Button("Take") { Task { await store.take(name: store.takeName) } }
            } message: {
                Text(SnapshotWords.takeAlertMessage)
            }
            .alert(
                "Rename snapshot", isPresented: present(\.renaming), presenting: store.item(store.renaming)
            ) { item in
                TextField("Name", text: $store.renameText)
                Button("Cancel", role: .cancel) {}
                Button("Save") { Task { await store.rename(item.id, to: store.renameText) } }
            }
            .alert(
                store.item(store.restoreTarget).map { SnapshotWords.restoreTitle(of: $0) } ?? "",
                isPresented: present(\.restoreTarget), presenting: store.item(store.restoreTarget)
            ) { item in
                Button("Restore", role: .destructive) { Task { await store.restore(item.id) } }
                Button("Cancel", role: .cancel) {}
            } message: { item in
                Text(SnapshotWords.restoreMessage(of: item, endsUpdate: endsUpdate))
            }
            .alert(
                store.item(store.deleteTarget).map { SnapshotWords.deleteTitle(of: $0) } ?? "",
                isPresented: present(\.deleteTarget), presenting: store.item(store.deleteTarget)
            ) { item in
                Button("Delete", role: .destructive) { Task { await store.delete(item.id) } }
                Button("Cancel", role: .cancel) {}
            } message: { item in
                Text(SnapshotWords.deleteMessage(frees: store.freesNow(item)))
            }
            .alert(
                store.problem?.title ?? "",
                isPresented: Binding(get: { store.problem != nil }, set: { if !$0 { store.problem = nil } }),
                presenting: store.problem
            ) { _ in
                Button("OK", role: .cancel) {}
            } message: { problem in
                Text(problem.message)
            }
    }

    /// A prompt is up while its target is set.
    private func present(_ key: ReferenceWritableKeyPath<SnapshotsModel, SnapshotID?>) -> Binding<Bool> {
        Binding(
            get: { model.snapshots[keyPath: key] != nil },
            set: { if !$0 { model.snapshots[keyPath: key] = nil } }
        )
    }
}
