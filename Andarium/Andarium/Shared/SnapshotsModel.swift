// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation

/// A page of the Snapshots sheet: one snapshot, or how snapshots use space.
enum SnapshotPage: Hashable {
    case snapshot(SnapshotID)
    case space
}

/// What Home says after a restore, until the guest starts or the person
/// puts it away.
struct RestoredSnapshotNotice: Equatable {
    /// The snapshot's name; empty when it had none.
    var name: String
    var created: Date
    /// The revision the next start updates the restored data to ("r20").
    var updatesTo: String?
}

/// The snapshot screens' state and what they do with the driver
/// (`AppModel.snapshotListing` and the operations beside it): the listing
/// Home and the sheet both show, the prompts that are up, and the outcome of
/// each operation. Words come from `SnapshotWords`. A driver's refusal, plan
/// and figure reach the screens only through here.
@MainActor @Observable
final class SnapshotsModel {
    @ObservationIgnored weak var app: AppModel?
    /// How long a name waits for the next key before it is saved.
    @ObservationIgnored var renameDelay: Duration = .milliseconds(700)
    /// How long a snapshot just taken stays marked.
    @ObservationIgnored var freshDuration: Duration = .seconds(2)

    /// What the driver last returned; nil until the first reading.
    private(set) var listing: SnapshotListing?
    /// An operation finished or a run began or ended since the listing was
    /// read: its figures describe a disk that is not there any more.
    private(set) var stale = false
    /// The figures last measured, kept while a newer reading of a disk that
    /// has not changed has none yet, so a row shows its last figure and not a
    /// gap. An operation or a run clears them (`invalidate`).
    private var knownFigures: [SnapshotID: SnapshotFigure] = [:]
    private var knownKept: [SnapshotID: SnapshotFigure] = [:]
    private var knownTotal: SnapshotFigure?
    private var knownUpdateCopy: SnapshotFigure?

    // What the prompts hold.
    var taking = false
    var takeName = ""
    var renaming: SnapshotID?
    var renameText = ""
    var restoreTarget: SnapshotID?
    var deleteTarget: SnapshotID?
    /// An operation that did not go ahead.
    var problem: AppAlert?

    // Where the sheet is.
    /// The pushed pages (iPhone).
    var route: [SnapshotPage] = []
    /// The page beside the list (iPad).
    var selection: SnapshotPage?
    /// The name field of the open snapshot takes the keyboard (the preview).
    var focusName = false
    /// The snapshot just taken, marked for a moment.
    private(set) var fresh: SnapshotID?

    /// The operation that was under way last, for the revision change that ends
    /// it: the driver clears the operation before it counts it as finished.
    @ObservationIgnored private var lastOperation: SnapshotWork?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var inFlight: (revision: Int, task: Task<Void, Never>)?
    @ObservationIgnored private var pendingName: (id: SnapshotID, name: String)?
    /// The debounce of the name being typed.
    @ObservationIgnored private var renameTask: Task<Void, Never>?
    /// The last rename handed to the driver; each waits for the one before it.
    @ObservationIgnored private var renameTail: Task<Void, Never>?
    @ObservationIgnored private var freshTask: Task<Void, Never>?

    // MARK: What the listing says

    var items: [SnapshotSummary] { listing?.items ?? [] }
    var count: Int { items.count }

    func item(_ id: SnapshotID?) -> SnapshotSummary? {
        guard let id else { return nil }
        return items.first { $0.id == id }
    }

    /// The listing's figures are measured and describe the disk as it is now:
    /// nothing finished or ran since it was read.
    var isMeasured: Bool {
        guard let listing, listing.isMeasured, !stale else { return false }
        return listing.revision == app?.snapshotActivity.revision
    }

    /// What deleting a snapshot frees, for a row: the figure measured now, or
    /// the last one while a new reading of a disk that has not changed
    /// measures; nil (the row shows it is measuring) otherwise.
    func frees(_ item: SnapshotSummary) -> SnapshotFigure? {
        isMeasured ? item.freesWhenDeleted : knownFigures[item.id]
    }

    /// What deleting a snapshot frees, only where the listing is measured and
    /// current: the Delete alert names a size from this and from nothing else.
    func freesNow(_ item: SnapshotSummary) -> SnapshotFigure? {
        isMeasured ? item.freesWhenDeleted : nil
    }

    /// What deleting every snapshot frees, last measured; nil before any.
    var total: SnapshotFigure? {
        guard let listing else { return nil }
        return isMeasured ? listing.space.snapshotsUse : knownTotal
    }

    /// What it would take on its own, as `frees` has it: measured now, or the
    /// last figure while a disk that did not change is read again; nil otherwise.
    func kept(_ item: SnapshotSummary) -> SnapshotFigure? {
        isMeasured ? item.kept : knownKept[item.id]
    }

    /// What an unfinished update's own copy holds; nil when none is.
    var updateCopy: SnapshotFigure? {
        guard let listing else { return nil }
        return isMeasured ? listing.space.updateCopy : knownUpdateCopy
    }

    var unfinishedRestore: SnapshotUnfinishedRestore? { listing?.unfinishedRestore }

    /// What is known of the snapshot an unfinished restore names: a snapshot
    /// that is whole, which Start finishes; a record that cannot be read; or a
    /// snapshot that is gone or damaged, which a start drops. A refusal for a
    /// restore the listing does not show is worded as unreadable.
    var unfinishedRestoreKind: UnfinishedRestoreKind {
        guard let id = unfinishedRestore?.snapshot else { return .unreadable }
        guard let item = item(id), !item.isDamaged else { return .lost }
        return .named
    }

    /// The snapshot an unfinished restore names, where Start can finish it.
    var unfinishedRestoreSnapshot: SnapshotSummary? {
        unfinishedRestoreKind == .named ? item(unfinishedRestore?.snapshot) : nil
    }

    /// The unfinished restore's own card says the next start updates the
    /// restored data, as the update card would.
    var unfinishedRestoreUpdates: Bool {
        if case .some(.updatesAtNextStart) = unfinishedRestoreSnapshot?.restore { return true }
        return false
    }

    /// What Home's card says of a guest this app version cannot start: the
    /// way out through an older snapshot is named where there is one.
    func blockedText(_ reason: StartBlock) -> String {
        SnapshotWords.blockedText(reason, hasOlderSnapshot: hasRestorableSnapshot)
    }

    /// What Home's card says of a restore that did not finish; nil when none does.
    var unfinishedRestoreText: String? {
        guard let unfinished = unfinishedRestore else { return nil }
        return SnapshotWords.unfinishedText(unfinishedRestoreKind, item: unfinishedRestoreSnapshot, failure: unfinished.lastFailure)
    }

    /// A restorable snapshot is a way out of a guest this app version is too
    /// old for: one this app can restore, as the guest is on a newer Android.
    var hasRestorableSnapshot: Bool {
        items.contains { item in
            switch item.restore {
            case .asIs, .updatesAtNextStart: true
            case .refused: false
            }
        }
    }

    // MARK: What can be done now

    /// Why Take snapshot is unavailable now, or nil when it is available.
    /// Before the first reading nothing is offered.
    var takeRefusal: SnapshotRefusal? {
        guard let listing else { return .busy }
        if let gate = app?.snapshotGate { return gate }
        if listing.unfinishedRestore != nil { return .restoreUnfinished }
        return listing.createRefusal
    }

    var takeReason: String? {
        takeRefusal.flatMap { SnapshotWords.takeReason($0, restore: unfinishedRestoreKind) }
    }

    /// Why a snapshot cannot be restored now, or nil. What is wrong with the
    /// snapshot itself comes first, as it does not pass with time.
    func restoreRefusal(of item: SnapshotSummary) -> SnapshotRefusal? {
        if case .refused(let reason) = item.restore { return reason }
        if let gate = app?.snapshotGate { return gate }
        if listing?.unfinishedRestore != nil { return .restoreUnfinished }
        return nil
    }

    /// Why a snapshot cannot be deleted now, or nil. The snapshot a restore
    /// is finishing stays until it has.
    func deleteRefusal(of item: SnapshotSummary) -> SnapshotRefusal? {
        if let gate = app?.snapshotGate { return gate }
        if let unfinished = listing?.unfinishedRestore, unfinished.snapshot == nil || unfinished.snapshot == item.id {
            return .restoreUnfinished
        }
        return nil
    }

    /// The words under Restore.
    func restoreNote(of item: SnapshotSummary) -> String {
        guard let app else { return "" }
        if case .refused = item.restore {
            return SnapshotWords.restoreNote(of: item, appRevision: app.system.releaseRevision, endsUpdate: false)
        }
        if let refusal = restoreRefusal(of: item),
            let reason = SnapshotWords.nowReason(refusal, restore: unfinishedRestoreKind) {
            return reason
        }
        return SnapshotWords.restoreNote(of: item, appRevision: app.system.releaseRevision, endsUpdate: endsUpdate)
    }

    /// A restore ends the update that did not finish.
    var endsUpdate: Bool {
        if case .unfinishedUpdate = app?.plan { return true }
        return false
    }

    /// An operation that holds the guest's files is under way (also one on
    /// the guest's storage); a rename that follows the typing is not shown.
    var isWorking: Bool { app?.snapshotActivity.holdsGuestFiles ?? false }

    // MARK: Reading

    /// Reads the listing: the names first and the figures when they are
    /// measured. It does nothing when the listing is current and measured,
    /// unless `force`. A screen appearing does not ask for a new reading: the
    /// disk is read when nothing was, after an operation, and when a run that
    /// held it ends (`activityChanged`). While a run holds the disk only the
    /// names are read. A reading in flight for the same count of finished
    /// operations is joined.
    func reload(force: Bool = false) async {
        guard let app else { return }
        let revision = app.snapshotActivity.revision
        if let inFlight, inFlight.revision == revision { return await inFlight.task.value }
        if !force, isMeasured { return }
        generation += 1
        let mine = generation
        let task = Task<Void, Never> { @MainActor [weak self] in
            guard let self else { return }
            await self.read(generation: mine)
        }
        inFlight = (revision, task)
        await task.value
        if generation == mine { inFlight = nil }
    }

    private func read(generation mine: Int) async {
        guard let app else { return }
        // A few rounds at most: each is one more operation that finished while this read.
        for _ in 0..<3 {
            let quick = await app.snapshotListing(measured: false)
            guard mine == generation else { return }
            adopt(quick)
            // A run holds the disk: the names are enough until it ends.
            guard !app.snapshotActivity.runtimeActive else { return }
            let full = await app.snapshotListing(measured: true)
            guard mine == generation else { return }
            adopt(full)
            if full.revision == app.snapshotActivity.revision { break }
        }
    }

    private func adopt(_ new: SnapshotListing) {
        if new.isMeasured {
            stale = false
            knownFigures = Dictionary(uniqueKeysWithValues: new.items.map { ($0.id, $0.freesWhenDeleted) })
            knownKept = Dictionary(uniqueKeysWithValues: new.items.map { ($0.id, $0.kept) })
            knownTotal = new.space.snapshotsUse
            knownUpdateCopy = new.space.updateCopy
        }
        listing = new
        // A page that no longer has its snapshot goes.
        let ids = Set(new.items.map(\.id))
        route.removeAll { if case .snapshot(let id) = $0 { !ids.contains(id) } else { false } }
        if case .snapshot(let id) = selection, !ids.contains(id) { selection = nil }
        if selection == nil, let first = new.items.first { selection = .snapshot(first.id) }
    }

    /// The guest starts: a reading in flight is put away, and its measure
    /// stops before it runs on into the boot.
    func cancelReads() {
        generation += 1
        inFlight?.task.cancel()
        inFlight = nil
    }

    /// What was measured no longer describes the disk: the figures shown go,
    /// and a reading in flight, which measures the disk as it was, is put away.
    private func invalidate() {
        generation += 1
        inFlight?.task.cancel()
        inFlight = nil
        stale = true
        knownFigures = [:]
        knownKept = [:]
        knownTotal = nil
        knownUpdateCopy = nil
    }

    /// Forgets everything read: the guest was erased or made anew.
    func reset() {
        invalidate()
        stale = false
        listing = nil
        route = []
        selection = nil
        fresh = nil
        pendingName = nil
        lastOperation = nil
        renameTask?.cancel()
        // What the screens held belongs to the guest that is gone.
        problem = nil
        taking = false
        takeName = ""
        renaming = nil
        renameText = ""
        restoreTarget = nil
        deleteTarget = nil
    }

    /// The driver finished one more operation, or a run began or ended: the
    /// listing is older than the disk. Its figures give way to the measuring
    /// state, and the disk is read again unless a run holds it. A rename that
    /// finished changed no figure: the names are read again and the figures
    /// stay shown (the Delete alert's size stays out until the new reading).
    func activityChanged(from old: SnapshotActivity, to new: SnapshotActivity) {
        if let operation = new.operation { lastOperation = operation }
        let finished = new.revision != old.revision
        let runChanged = new.runtimeActive != old.runtimeActive
        guard finished || runChanged else { return }
        let renamed = finished && !runChanged && lastOperation == .rename
        if finished { lastOperation = nil }
        if !renamed { invalidate() }
        guard listing != nil, !new.runtimeActive else { return }
        Task { @MainActor [weak self] in await self?.reload() }
    }

    // MARK: Prompts

    func requestTake() {
        takeName = ""
        taking = true
    }

    func requestRename(_ id: SnapshotID) {
        renameText = item(id)?.name ?? ""
        renaming = id
    }

    // MARK: Operations

    /// A snapshot of the guest's data as it is, with the name if given.
    func take(name: String) async {
        guard let app else { return }
        await settleRenames()
        switch await app.takeSnapshot(named: name) {
        case .done(let made):
            mark(fresh: made.id)
            // Beside the list (iPad) the new one opens.
            selection = .snapshot(made.id)
        case .refused(let refusal):
            tell(.take, refusal: refusal)
        case .failed(let error):
            tell(.take, failure: error)
        }
        await reload()
    }

    /// Puts the snapshot's data back. The sheet closes on Home, which says
    /// what happened and that Start is next.
    func restore(_ id: SnapshotID) async {
        guard let app else { return }
        await flushName()
        guard let item = item(id) else { return }
        var notice = RestoredSnapshotNotice(name: item.name, created: item.created)
        if case .updatesAtNextStart(_, let to) = item.restore { notice.updatesTo = SnapshotWords.revision(of: to) }
        switch await app.restoreSnapshot(id) {
        case .done:
            finishRestore(notice)
        case .refused(.restoreUnfinished):
            // Either the data went in place and the restore could not finish,
            // or a restore from before still stands: the listing says which.
            await reload(force: true)
            if listing?.unfinishedRestore?.snapshot == id {
                // Home's card says it, from the listing.
                finishRestore(nil)
            } else {
                tell(.restore, refusal: .restoreUnfinished)
            }
            return
        case .refused(let refusal):
            tell(.restore, refusal: refusal, snapshot: item)
        case .failed(let error):
            tell(.restore, failure: error)
        }
        await reload()
    }

    private func finishRestore(_ notice: RestoredSnapshotNotice?) {
        app?.restoredSnapshot = notice
        app?.sheet = nil
        route = []
    }

    func delete(_ id: SnapshotID) async {
        guard let app else { return }
        // What was typed in the page of the snapshot that goes is not saved: it
        // would be refused as missing. A rename under way, or of another
        // snapshot, finishes first; the driver refuses a delete beside it.
        if pendingName?.id == id {
            pendingName = nil
            renameTask?.cancel()
        }
        await flushName()
        let before = items.map(\.id)
        switch await app.deleteSnapshot(id) {
        case .done:
            route.removeAll { $0 == .snapshot(id) }
            if selection == .snapshot(id) {
                // The one that takes its place in the list.
                let at = before.firstIndex(of: id) ?? 0
                let rest = before.filter { $0 != id }
                selection = rest.isEmpty ? nil : .snapshot(rest[min(at, rest.count - 1)])
            }
        case .refused(let refusal):
            tell(.delete, refusal: refusal, snapshot: item(id))
        case .failed(let error):
            tell(.delete, failure: error)
        }
        await reload()
    }

    /// Renames at once: the list's Rename… prompt. A rename waits for the one
    /// before it, so they reach the driver in the order they were asked.
    func rename(_ id: SnapshotID, to name: String) async {
        guard let app else { return }
        let previous = renameTail
        let task = Task<Void, Never> { @MainActor [weak self] in
            await previous?.value
            switch await app.renameSnapshot(id, to: name) {
            case .done: break
            case .refused(let refusal): self?.tell(.rename, refusal: refusal)
            case .failed(let error): self?.tell(.rename, failure: error)
            }
        }
        renameTail = task
        await task.value
        if renameTail == task { renameTail = nil }
        await reload()
    }

    /// Waits for every rename handed to the driver: an operation that the
    /// driver refuses beside a rename starts after it.
    private func settleRenames() async {
        while let tail = renameTail {
            await tail.value
            if renameTail == tail { renameTail = nil }
        }
    }

    /// The name field of a page: the name is saved a moment after the last
    /// key, and by `flushName` when the page goes.
    func edit(name: String, of id: SnapshotID) {
        pendingName = (id, name)
        renameTask?.cancel()
        let delay = renameDelay
        renameTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: delay)
            guard !Task.isCancelled else { return }
            await self?.saveName()
        }
    }

    /// Saves a name that waits for its moment now, and waits for the renames
    /// under way.
    func flushName() async {
        renameTask?.cancel()
        await saveName()
        await settleRenames()
    }

    private func saveName() async {
        guard let (id, name) = pendingName else { return }
        pendingName = nil
        await rename(id, to: name)
    }

    // MARK: Outcomes

    private func tell(_ operation: SnapshotOperation, refusal: SnapshotRefusal, snapshot: SnapshotSummary? = nil) {
        guard let app else { return }
        show(AppAlert(
            title: SnapshotWords.problemTitle(operation),
            message: SnapshotWords.refusal(
                refusal, snapshot: snapshot, appRevision: app.system.releaseRevision, device: app.host.name,
                restore: unfinishedRestoreKind)))
    }

    private func tell(_ operation: SnapshotOperation, failure error: any Error) {
        show(AppAlert(title: SnapshotWords.problemTitle(operation), message: error.localizedDescription))
    }

    /// The sheet says a problem where it is open. An operation that ended
    /// after it closed (the name saved as the page went) is told by the app's
    /// own alert, which does not wait for the sheet to open again.
    private func show(_ alert: AppAlert) {
        guard let app else { return }
        if app.sheet == .snapshots {
            problem = alert
        } else {
            app.alert = alert
        }
    }

    private func mark(fresh id: SnapshotID) {
        fresh = id
        freshTask?.cancel()
        let duration = freshDuration
        freshTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: duration)
            guard !Task.isCancelled, self?.fresh == id else { return }
            self?.fresh = nil
        }
    }
}
