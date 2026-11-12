// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation
import UIKit

/// One folder on the Settings sheet's navigation stack.
struct FolderRoute: Hashable {
    var path: String
}

enum FolderListing: Equatable {
    case loading
    case loaded([GuestFile])
    case refused
    case failed(String)
}

enum FileSort: String, CaseIterable, Identifiable {
    case name, date, size

    var id: String { rawValue }

    var title: String {
        switch self {
        case .name: "Name"
        case .date: "Date Modified"
        case .size: "Size"
        }
    }
}

/// An import that waits for an answer: some of its names are in the folder.
struct ImportConflict {
    var picks: [FilePick]
    var clashing: [FilePick]
    var folder: String
    /// The names that are folders there, which an import never replaces.
    var folders: Set<String> = []
}

enum ConflictChoice {
    case skip, keepBoth, replace
}

/// One import or export. There is one at a time; it runs while the person
/// goes on browsing.
@Observable
final class FileTransfer: Identifiable {
    enum Direction {
        case importing, exporting
    }

    enum Stage: Equatable {
        case running
        /// An export is staged and waits for the system's action menu.
        case ready
        case finished
        case failed
    }

    enum ItemState: Equatable {
        case waiting, moving, done
        case failed(String)
    }

    struct Item: Identifiable, Equatable {
        let id = UUID()
        var name: String
        var size: UInt64
        var isFolder = false
        var moved: UInt64 = 0
        var state: ItemState = .waiting
        /// Where an imported file goes in the guest.
        var path: String?
    }

    let id = UUID()
    let direction: Direction
    /// Where an import goes; an export names the folder it was started from.
    let folder: String
    var items: [Item]
    var stage = Stage.running
    /// The folders of an export are still being walked for their sizes.
    var measuring = false
    /// The card shows every item.
    var expanded = false
    /// The staged copies of an export, for the action menu.
    var staged: [URL] = []
    /// A held screen stays as it is.
    var stays = false
    /// The export's own folder of the staging area, which holds every copy.
    @ObservationIgnored var directory: URL?
    /// Why the transfer was stopped from outside, which its files then say.
    @ObservationIgnored var interruption: String?
    @ObservationIgnored var task: Task<Void, Never>?

    init(direction: Direction, folder: String, items: [Item]) {
        self.direction = direction
        self.folder = folder
        self.items = items
    }

    var running: Bool { stage == .running }
    var totalBytes: UInt64 { items.reduce(0) { $0 + $1.size } }
    var movedBytes: UInt64 { items.reduce(0) { $0 + $1.moved } }

    var fraction: Double {
        guard totalBytes > 0 else { return 0 }
        return min(1, Double(movedBytes) / Double(totalBytes))
    }

    private var failures: [Item] {
        items.filter { if case .failed = $0.state { true } else { false } }
    }

    /// "Clip.mov", "3 files" or "2 items" when a folder is among them.
    private func what(_ items: [Item]) -> String {
        if items.count == 1, let only = items.first { return only.name }
        return "\(items.count) \(items.contains(where: \.isFolder) ? "items" : "files")"
    }

    private var verb: String { direction == .importing ? "Importing" : "Exporting" }
    private var done: String { direction == .importing ? "imported" : "exported" }

    /// Short where buttons share the line: the name of a single file moves to
    /// the detail line.
    var title: String {
        let single = items.count == 1
        switch stage {
        case .running: return "\(verb) \(what(items))"
        case .ready: return single ? "Ready to share" : "\(what(items)) ready"
        case .finished: return "\(what(items)) \(done)"
        case .failed: return single ? "Not \(done)" : "\(failures.count) of \(items.count) not \(done)"
        }
    }

    var detail: String {
        switch stage {
        case .running:
            if measuring { return "Counting files\u{2026}" }
            let moved = "\(Formats.size(bytes: movedBytes)) of \(Formats.size(bytes: totalBytes))"
            guard items.count > 1, let index = items.firstIndex(where: { $0.state == .moving }) else { return moved }
            return "\(index + 1) of \(items.count) \u{00B7} \(moved)"
        case .ready:
            let size = Formats.size(bytes: totalBytes)
            if items.count == 1, let only = items.first { return "\(only.name) \u{00B7} \(size)" }
            return size
        case .finished:
            return direction == .importing ? "In \(GuestPath.name(of: folder))" : Formats.size(bytes: totalBytes)
        case .failed:
            if case let .failed(reason)? = failures.first?.state { return reason }
            return ""
        }
    }
}

/// The file browser behind Settings: where the person is, what is selected
/// and the one transfer. It lasts for a run, and is emptied when Settings
/// closes (`sheetClosed`) and when the run ends (`reset`).
@Observable
final class FileBrowserModel {
    /// What a transfer's files say when iOS stopped the guest under it.
    static let leftForegroundReason = "Stopped when Andarium left the foreground."

    @ObservationIgnored let backend: any GuestFilesBackend
    /// Whether the guest can be browsed now (its storage is up). Nothing is
    /// imported or exported while it cannot.
    @ObservationIgnored var isAvailable: () -> Bool = { true }

    /// The Settings sheet's navigation path: empty on Settings itself, then
    /// internal storage and the folders below it.
    var route: [FolderRoute] = []
    var sort = FileSort.name
    /// What Export takes: the selection follows the person between folders.
    var selection: [GuestFile] = []
    var reviewingSelection = false
    var picking = false
    var conflict: ImportConflict?
    var transfer: FileTransfer?
    /// What the last import brought, marked in its folder.
    var fresh: Set<String> = []
    private(set) var freeBytes: UInt64?
    private var listings: [String: FolderListing] = [:]
    @ObservationIgnored private var retry: (() -> Void)?
    @ObservationIgnored private var observer: NSObjectProtocol?

    /// The system's action menu; a test or a preview may put its own here.
    @ObservationIgnored
    var presentShare: ([URL], @escaping @MainActor (Bool) -> Void) -> Bool = { files, finished in
        BugReportSharing.present(files, completed: finished)
    }

    @ObservationIgnored private let center: NotificationCenter

    /// `center` is where the application's notifications arrive; a test gives
    /// its own, so that one model's test never stops another's.
    init(backend: any GuestFilesBackend, center: NotificationCenter = .default) {
        self.backend = backend
        self.center = center
        // iOS pauses the guest when the application enters the background,
        // so a transfer cannot go on through it.
        observer = center.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.leftForeground() }
        }
    }

    isolated deinit {
        if let observer { center.removeObserver(observer) }
    }

    var browsing: Bool { !route.isEmpty }
    var current: String { route.last?.path ?? GuestPath.root }
    var running: Bool { transfer?.running == true }

    /// A new import or export has to wait, and so do the guest's other
    /// operations, which share its adb channel.
    var busy: Bool { running || conflict != nil }

    // MARK: Folders

    func listing(_ folder: String) -> FolderListing? { listings[folder] }

    func entries(in folder: String) -> [GuestFile] {
        guard case let .loaded(files) = listings[folder] else { return [] }
        return files.sorted(by: order)
    }

    private func order(_ first: GuestFile, _ second: GuestFile) -> Bool {
        if first.isFolder != second.isFolder { return first.isFolder }
        switch sort {
        case .date where first.modified != second.modified:
            return first.modified > second.modified
        case .size where first.size != second.size:
            return first.size > second.size
        default:
            return first.name.localizedStandardCompare(second.name) == .orderedAscending
        }
    }

    /// Reads `folder` again. A folder that answers at once never shows a
    /// placeholder, and one that is listed already keeps its listing when the
    /// new reading fails.
    func load(_ folder: String) async {
        let placeholder = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(150))
            guard !Task.isCancelled, let self, self.listings[folder] == nil else { return }
            self.listings[folder] = .loading
        }
        defer { placeholder.cancel() }
        let free = Task { await backend.freeBytes() }
        do {
            listings[folder] = .loaded(try await backend.list(folder))
        } catch is CancellationError {
        } catch GuestFolderProblem.refused {
            listings[folder] = .refused
        } catch {
            if case .loaded = listings[folder] {} else { listings[folder] = .failed(error.localizedDescription) }
        }
        freeBytes = await free.value
    }

    func canImport(into folder: String) -> Bool {
        if case .loaded = listings[folder] { return !busy && isAvailable() }
        return false
    }

    /// The trail: back to the folder at `index` of the route.
    func jump(to index: Int) {
        guard route.indices.contains(index), index < route.count - 1 else { return }
        route = Array(route.prefix(index + 1))
    }

    // MARK: Selection

    func isSelected(_ file: GuestFile) -> Bool { selection.contains { $0.path == file.path } }

    func toggle(_ file: GuestFile) {
        if isSelected(file) {
            selection.removeAll { $0.path == file.path }
        } else {
            selection.append(file)
        }
    }

    /// The files of the folder; its folders are chosen one by one.
    func selectAll(in folder: String) {
        for file in entries(in: folder) where !file.isFolder && !isSelected(file) {
            selection.append(file)
        }
    }

    func allSelected(in folder: String) -> Bool {
        let files = entries(in: folder).filter { !$0.isFolder }
        return !files.isEmpty && files.allSatisfy(isSelected)
    }

    func clearSelection() {
        selection = []
        reviewingSelection = false
    }

    /// "3 selected · 2.4 GiB"; a folder's size is known once it is exported.
    var selectionSummary: String {
        let bytes = selection.filter { !$0.isFolder }.reduce(0) { $0 + $1.size }
        let hasFolders = selection.contains(where: \.isFolder)
        guard bytes > 0, !hasFolders else {
            return "\(selection.count) selected"
        }
        return "\(selection.count) selected \u{00B7} \(Formats.size(bytes: bytes))"
    }

    // MARK: Import

    /// One file of an import: where it goes and whether it takes the place of
    /// a file of that name.
    struct ImportStep {
        var pick: FilePick
        var path: String
        var replacing: Bool
    }

    /// The picker's result, for the folder on screen. Picks of one name from
    /// different places are told apart first, as the guest's Files app does.
    func received(_ picks: [FilePick]) {
        picking = false
        guard !picks.isEmpty, !busy, isAvailable() else { return }
        let folder = current
        let names = GuestFileNames.distinct(picks.map(\.name))
        let renamed = zip(picks, names).map { pick, name -> FilePick in
            var copy = pick
            copy.name = name
            return copy
        }
        let existing = Dictionary(
            entries(in: folder).map { ($0.name, $0.isFolder) }, uniquingKeysWith: { first, _ in first })
        let clashing = renamed.filter { existing[$0.name] != nil }
        guard clashing.isEmpty else {
            conflict = ImportConflict(
                picks: renamed, clashing: clashing, folder: folder,
                folders: Set(existing.filter(\.value).keys))
            return
        }
        startImport(renamed.map { ImportStep(pick: $0, path: GuestPath.child($0.name, of: folder), replacing: false) }, into: folder)
    }

    func resolveConflict(_ choice: ConflictChoice) {
        guard let conflict else { return }
        self.conflict = nil
        let clashing = Set(conflict.clashing.map(\.name))
        var taken = Set(entries(in: conflict.folder).map(\.name)).union(conflict.picks.map(\.name))
        var plan: [ImportStep] = []
        for pick in conflict.picks {
            var name = pick.name
            var replacing = false
            if clashing.contains(name) {
                switch choice {
                case .skip: continue
                case .replace:
                    // A folder is never replaced: the file goes beside it.
                    if conflict.folders.contains(name) {
                        name = GuestFileNames.freeName(for: name, taken: taken)
                        taken.insert(name)
                    } else {
                        replacing = true
                    }
                case .keepBoth:
                    name = GuestFileNames.freeName(for: name, taken: taken)
                    taken.insert(name)
                }
            }
            var renamed = pick
            renamed.name = name
            plan.append(ImportStep(pick: renamed, path: GuestPath.child(name, of: conflict.folder), replacing: replacing))
        }
        if !plan.isEmpty { startImport(plan, into: conflict.folder) }
    }

    func cancelConflict() { conflict = nil }

    private func startImport(_ plan: [ImportStep], into folder: String) {
        let transfer = FileTransfer(
            direction: .importing, folder: folder,
            items: plan.map { FileTransfer.Item(name: $0.pick.name, size: $0.pick.size, path: $0.path) }
        )
        replaceTransfer(with: transfer)
        runImport(transfer, plan: plan, retrying: Array(plan.indices))
    }

    private func runImport(_ transfer: FileTransfer, plan: [ImportStep], retrying indices: [Int]) {
        let backend = backend
        transfer.interruption = nil
        for index in indices {
            transfer.items[index].moved = 0
            transfer.items[index].state = .waiting
        }
        let again: ([Int]) -> Void = { [weak self] failed in self?.runImport(transfer, plan: plan, retrying: failed) }
        transfer.task = Task { [weak self] in
            guard let self else { return }
            do {
                let bytes = indices.reduce(0) { $0 + plan[$1].pick.size }
                try await backend.prepareImport(bytes: bytes, into: transfer.folder)
            } catch is CancellationError {
                self.stopped(transfer, rerun: again)
                return
            } catch {
                for index in indices { transfer.items[index].state = .failed(error.localizedDescription) }
                self.importEnded(transfer, plan: plan, rerun: again)
                return
            }
            for index in indices {
                if Task.isCancelled {
                    self.stopped(transfer, rerun: again)
                    return
                }
                transfer.items[index].state = .moving
                do {
                    try await backend.send(plan[index].pick, to: plan[index].path, replacing: plan[index].replacing) { moved, total in
                        Task { @MainActor in Self.progress(transfer, index, moved, total) }
                    }
                    transfer.items[index].moved = transfer.items[index].size
                    transfer.items[index].state = .done
                } catch is CancellationError {
                    self.stopped(transfer, rerun: again)
                    return
                } catch {
                    transfer.items[index].moved = 0
                    transfer.items[index].state = .failed(error.localizedDescription)
                }
            }
            self.importEnded(transfer, plan: plan, rerun: again)
        }
    }

    private static func progress(_ transfer: FileTransfer, _ index: Int, _ moved: UInt64, _ total: UInt64) {
        guard transfer.items.indices.contains(index), transfer.items[index].state == .moving else { return }
        transfer.items[index].moved = moved
        // The size a file has once it is here, which an iCloud file did not
        // say when it was picked.
        if total > 0, total != transfer.items[index].size { transfer.items[index].size = total }
    }

    private func importEnded(_ transfer: FileTransfer, plan: [ImportStep], rerun: @escaping ([Int]) -> Void) {
        guard self.transfer === transfer else { return }
        fresh = Set(transfer.items.compactMap { $0.state == .done ? $0.path : nil })
        Task { await load(transfer.folder) }
        if transfer.items.allSatisfy({ $0.state == .done }) {
            finish(transfer)
        } else {
            failed(transfer, rerun: rerun)
        }
    }

    // MARK: Export

    /// Copies the selection out of the guest; the action menu takes it from
    /// there.
    func exportSelection() {
        export(selection)
        selection = []
        reviewingSelection = false
    }

    /// The long-press's Export: this one item, whatever is selected.
    func export(_ files: [GuestFile]) {
        guard !files.isEmpty, !busy, isAvailable() else { return }
        let transfer = FileTransfer(
            direction: .exporting, folder: current,
            items: files.map { FileTransfer.Item(name: $0.name, size: $0.size, isFolder: $0.isFolder) }
        )
        transfer.measuring = files.contains(where: \.isFolder)
        replaceTransfer(with: transfer)
        runExport(transfer, files: files, retrying: Array(files.indices))
    }

    private func runExport(_ transfer: FileTransfer, files: [GuestFile], retrying indices: [Int]) {
        let backend = backend
        transfer.interruption = nil
        for index in indices {
            transfer.items[index].moved = 0
            transfer.items[index].state = .waiting
        }
        let again: ([Int]) -> Void = { [weak self] failed in self?.runExport(transfer, files: files, retrying: failed) }
        transfer.task = Task { [weak self] in
            guard let self else { return }
            do {
                if transfer.directory == nil { transfer.directory = try GuestFileStaging.makeTransferDirectory() }
            } catch {
                for index in indices {
                    transfer.items[index].state = .failed("Andarium couldn\u{2019}t make room for the copy.")
                }
                self.exportEnded(transfer, rerun: again)
                return
            }
            // A folder's size is what its files add up to.
            for index in indices where files[index].isFolder {
                do {
                    transfer.items[index].size = try await backend.measure(files[index])
                } catch is CancellationError {
                    self.stopped(transfer, rerun: again)
                    return
                } catch {
                    transfer.items[index].state = .failed(error.localizedDescription)
                }
            }
            transfer.measuring = false
            let moving = indices.filter { transfer.items[$0].state == .waiting }
            do {
                try await backend.prepareExport(bytes: moving.reduce(0) { $0 + transfer.items[$1].size })
            } catch is CancellationError {
                self.stopped(transfer, rerun: again)
                return
            } catch {
                for index in moving { transfer.items[index].state = .failed(error.localizedDescription) }
                self.exportEnded(transfer, rerun: again)
                return
            }
            for index in moving {
                if Task.isCancelled {
                    self.stopped(transfer, rerun: again)
                    return
                }
                guard let directory = transfer.directory?.appendingPathComponent(String(index), isDirectory: true) else { return }
                transfer.items[index].state = .moving
                do {
                    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                    let url = try await backend.receive(files[index], into: directory) { moved, total in
                        Task { @MainActor in Self.progress(transfer, index, moved, total) }
                    }
                    transfer.staged.append(url)
                    transfer.items[index].moved = transfer.items[index].size
                    transfer.items[index].state = .done
                } catch is CancellationError {
                    try? FileManager.default.removeItem(at: directory)
                    self.stopped(transfer, rerun: again)
                    return
                } catch {
                    try? FileManager.default.removeItem(at: directory)
                    transfer.items[index].moved = 0
                    transfer.items[index].state = .failed(error.localizedDescription)
                }
            }
            self.exportEnded(transfer, rerun: again)
        }
    }

    private func exportEnded(_ transfer: FileTransfer, rerun: @escaping ([Int]) -> Void) {
        guard self.transfer === transfer else { return }
        guard transfer.items.allSatisfy({ $0.state == .done }) else {
            failed(transfer, rerun: rerun)
            return
        }
        transfer.stage = .ready
        share(transfer)
    }

    /// The action menu on a finished export. The copies wait until it has
    /// done something with them or the person closes the capsule.
    func share(_ transfer: FileTransfer) {
        guard transfer.stage == .ready else { return }
        _ = presentShare(transfer.staged) { [weak self, weak transfer] completed in
            guard completed, let self, let transfer, self.transfer === transfer else { return }
            Self.removeStaged(transfer)
            self.finish(transfer)
        }
    }

    private static func removeStaged(_ transfer: FileTransfer) {
        if let directory = transfer.directory { try? FileManager.default.removeItem(at: directory) }
        transfer.directory = nil
        transfer.staged = []
    }

    // MARK: Ending

    func retryFailed() { retry?() }

    /// A new transfer takes the place of one that ended: an export nobody
    /// shared gives up its staged copies.
    private func replaceTransfer(with new: FileTransfer) {
        if let old = transfer { Self.removeStaged(old) }
        retry = nil
        transfer = new
    }

    private func finish(_ transfer: FileTransfer) {
        transfer.stage = .finished
        retry = nil
        guard !transfer.stays else { return }
        Task { [weak self, weak transfer] in
            try? await Task.sleep(for: .seconds(4))
            guard let self, let transfer, self.transfer === transfer else { return }
            self.transfer = nil
        }
    }

    /// The transfer ends with files that did not arrive; Try Again runs those
    /// again, and only those.
    private func failed(_ transfer: FileTransfer, rerun: @escaping ([Int]) -> Void) {
        let undone = transfer.items.indices.filter { transfer.items[$0].state != .done }
        transfer.stage = .failed
        retry = { [weak self, weak transfer] in
            guard let self, let transfer, self.transfer === transfer else { return }
            transfer.stage = .running
            rerun(undone)
        }
    }

    /// A transfer's task ended by cancellation. When the person cancelled,
    /// the transfer is already gone; when something stopped it (`interrupt`),
    /// its files say why, and keep what has arrived.
    private func stopped(_ transfer: FileTransfer, rerun: @escaping ([Int]) -> Void) {
        guard self.transfer === transfer, let reason = transfer.interruption else { return }
        transfer.measuring = false
        for index in transfer.items.indices {
            switch transfer.items[index].state {
            case .waiting, .moving:
                transfer.items[index].moved = 0
                transfer.items[index].state = .failed(reason)
            case .done, .failed:
                break
            }
        }
        failed(transfer, rerun: rerun)
    }

    /// Stops the running transfer from outside, for `reason`.
    func interrupt(because reason: String) {
        guard let transfer, transfer.running else { return }
        transfer.interruption = reason
        transfer.task?.cancel()
    }

    /// iOS stops the guest while Andarium is in the background, and the adb
    /// stream with it, which has no way to continue: the transfer stops and
    /// its files say so.
    func leftForeground() {
        interrupt(because: Self.leftForegroundReason)
    }

    /// Cancel, or the capsule's close: nothing partial stays behind. The
    /// backend removes what the stopped file left in the guest.
    func closeTransfer() {
        guard let transfer else { return }
        transfer.task?.cancel()
        Self.removeStaged(transfer)
        self.transfer = nil
        retry = nil
        if transfer.direction == .importing { Task { await load(transfer.folder) } }
    }

    // MARK: Lifetime

    /// Settings has closed: it opens again at its top, with nothing selected
    /// and every folder read afresh. A transfer that is over goes with it.
    func sheetClosed() {
        route = []
        selection = []
        reviewingSelection = false
        picking = false
        conflict = nil
        fresh = []
        listings = [:]
        freeBytes = nil
        if let transfer, !transfer.running {
            Self.removeStaged(transfer)
            self.transfer = nil
            retry = nil
        }
    }

    /// The run is over, or the guest cannot be reached any more: whatever the
    /// browser held belonged to it.
    func reset() {
        if let transfer {
            transfer.task?.cancel()
            Self.removeStaged(transfer)
        }
        transfer = nil
        retry = nil
        sheetClosed()
    }
}
