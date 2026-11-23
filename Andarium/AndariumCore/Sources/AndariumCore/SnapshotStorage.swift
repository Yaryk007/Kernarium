// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The guest's snapshots (`SnapshotStore`) with what only the guest's storage
/// knows: its choices, which a restore settles, and the figures it measures.
/// None of these checks whether the guest runs; the application's state does
/// (`SnapshotRules.gate`), before any of them is called.
extension TwoDiskStorage {
    var snapshotStore: SnapshotStore {
        SnapshotStore(locations: locations, release: release, io: snapshotIO)
    }

    /// The snapshots, newest first, with their figures and what may be done.
    /// It measures every snapshot and the space they use all together, which
    /// takes a fraction of a second for a large guest: call it off the main
    /// actor. `measured: false` reads only the small files, so it returns at
    /// once: every figure is then zero as a lower bound and `isMeasured` is
    /// false. A task that is cancelled while it measures stops between two
    /// measures and answers the same, unmeasured.
    public func snapshotListing(measured: Bool = true) -> SnapshotListing {
        let store = snapshotStore
        let entries = store.entries()
        let calls = snapshotIO.measure
        // What else holds blocks the snapshots hold: the live disk, once there is one, and an update's copy.
        let live = FileManager.default.fileExists(atPath: files.data.path) ? [files.data] : []
        let copies = updateCopies()
        let wholeSnapshots = entries.filter(\.hasData).map { store.dataURL($0.id) }

        var listing = SnapshotListing(
            createRefusal: store.createRefusal(), unfinishedRestore: store.unfinishedRestore(), dataRelease: dataRelease(),
            isMeasured: measured)
        func unmeasured() -> SnapshotListing {
            let unknown = SnapshotFigure(bytes: 0, isLowerBound: true)
            listing.isMeasured = false
            listing.items = entries.map { summary(of: $0, frees: unknown, kept: unknown) }
            listing.space = SnapshotSpace(snapshotsUse: unknown, updateCopy: copies.isEmpty ? nil : unknown)
            return listing
        }
        guard measured, !Task.isCancelled else { return unmeasured() }
        var items: [SnapshotSummary] = []
        for entry in entries {
            if Task.isCancelled { return unmeasured() }
            let (frees, kept) = figures(
                of: entry, store: store, calls: calls,
                keeping: live + copies + wholeSnapshots.filter { $0 != store.dataURL(entry.id) })
            items.append(summary(of: entry, frees: frees, kept: kept))
        }
        // What deleting them all frees: what they hold together that neither
        // the live disk nor an update's copy holds.
        if Task.isCancelled { return unmeasured() }
        var use = SnapshotMeasure.freed(by: wholeSnapshots, keeping: live + copies, using: calls)
        use.bytes += entries.reduce(0) { total, entry in
            total + store.smallFiles(entry.id).reduce(0) { $0 + SnapshotMeasure.allocatedBytes(of: $1) }
        }
        if Task.isCancelled { return unmeasured() }
        let copyFigure = copies.isEmpty ? nil : SnapshotMeasure.freed(by: copies, keeping: live + wholeSnapshots, using: calls)
        if Task.isCancelled { return unmeasured() }
        listing.items = items
        listing.space = SnapshotSpace(snapshotsUse: use, updateCopy: copyFigure)
        return listing
    }

    /// The release the guest's data is on now, from its record, whatever a
    /// start or a restore would be: nil when there is no record to read.
    private func dataRelease() -> SnapshotRelease? {
        guard let state = try? files.readState() else { return nil }
        return SnapshotRelease(releaseID: state.releaseID, buildNumber: Int(state.buildIncremental) ?? 0)
    }

    /// What deleting a snapshot frees and what it would take alone. `others`
    /// are the files that stay when it goes: the live disk, an update's copies
    /// and the other snapshots. With the file system's private size they are
    /// not needed; with only the extent map they say what the others still
    /// hold, and with neither the figure is a lower bound.
    private func figures(
        of entry: SnapshotEntry, store: SnapshotStore, calls: SnapshotMeasure.Primitives, keeping others: [URL]
    ) -> (frees: SnapshotFigure, kept: SnapshotFigure) {
        // A damaged snapshot that still has its disk holds space, which
        // deleting it frees.
        let data = store.dataURL(entry.id)
        let small = store.smallFiles(entry.id).reduce(0) { $0 + SnapshotMeasure.allocatedBytes(of: $1) }
        var frees = SnapshotFigure(bytes: small)
        var kept = SnapshotFigure(bytes: small)
        if entry.hasData {
            frees = SnapshotMeasure.freed(by: [data], keeping: others, using: calls)
            frees.bytes += small
            kept = SnapshotFigure(bytes: SnapshotMeasure.allocatedBytes(of: data))
        }
        return (frees, kept)
    }

    private func summary(of entry: SnapshotEntry, frees: SnapshotFigure, kept: SnapshotFigure) -> SnapshotSummary {
        guard let record = entry.record, let state = entry.state, entry.hasData else {
            return SnapshotSummary(
                id: entry.id, name: entry.record?.name ?? "", created: entry.record?.created ?? .distantPast,
                releaseID: entry.record?.releaseID ?? "", buildNumber: entry.record?.buildNumber ?? 0,
                diskGiB: entry.record?.diskGiB ?? 0, restore: .refused(.incomplete), kept: kept, freesWhenDeleted: frees
            )
        }
        return SnapshotSummary(
            id: entry.id, name: record.name, created: record.created, releaseID: record.releaseID,
            buildNumber: record.buildNumber, diskGiB: record.diskGiB,
            restore: SnapshotRules.restorePlan(of: state, diskGiB: record.diskGiB, release: release),
            kept: kept, freesWhenDeleted: frees
        )
    }

    /// The files that keep blocks of a snapshot's data when it goes.
    private func holders(besides id: SnapshotID, store: SnapshotStore) -> [URL] {
        let live = FileManager.default.fileExists(atPath: files.data.path) ? [files.data] : []
        let others = store.entries().filter { $0.hasData && $0.id != id }.map { store.dataURL($0.id) }
        return live + updateCopies() + others
    }

    /// The copies an Android update keeps until its start has booted.
    private func updateCopies() -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: locations.directory.path)) ?? []
        return names.filter { $0.hasPrefix(InstanceFiles.snapshotPrefix) }.sorted()
            .map { locations.directory.appendingPathComponent($0) }
    }

    /// Takes a snapshot of the data as it is. The guest must have been shut
    /// down by Android itself and must not run; a refusal is thrown as a
    /// `SnapshotRefusal`.
    @discardableResult
    public func createSnapshot(named name: String, now: Date = Date()) throws -> SnapshotSummary {
        let store = snapshotStore
        let id = try store.create(name: name, now: now)
        guard let entry = store.entry(id) else { throw SnapshotRefusal.missing }
        let (frees, kept) = figures(
            of: entry, store: store, calls: snapshotIO.measure, keeping: holders(besides: id, store: store))
        return summary(of: entry, frees: frees, kept: kept)
    }

    public func deleteSnapshot(_ id: SnapshotID) throws {
        try snapshotStore.delete(id)
    }

    public func renameSnapshot(_ id: SnapshotID, to name: String) throws {
        try snapshotStore.rename(id, to: name)
    }

    /// Puts the snapshot's data and record in place of the guest's. The state
    /// it replaces is gone, and an unfinished update ends with its copy.
    /// `guest` is the guest as the application holds it; the one returned has
    /// the capacity the restored disk needs, with the settings written and
    /// the backup policy applied again to the restored files.
    public func restoreSnapshot(_ id: SnapshotID, guest: Guest) throws -> Guest {
        var settled = guest
        try snapshotStore.restore(id) { record in
            settled = try settle(guest, after: record) ?? guest
        }
        settled.usedBytes = measuredUsedBytes()
        return settled
    }

    /// Completes what a crash left, before the guest is loaded: a restore
    /// that began is carried through, and a create or delete that did not
    /// finish is removed. Call it at launch before `load()`.
    public func recoverSnapshots() throws {
        try snapshotStore.recover { record in _ = try settle(nil, after: record) }
    }

    /// The restore's step on the guest's choices: the data disk is now the
    /// snapshot's, so the capacity is at least its size (a larger pending
    /// growth stays), and the guest's backup policy is applied again, because
    /// the clone carried the snapshot's exclusion. What the application
    /// remembers of the Magisk app is forgotten too, since the snapshot may
    /// not have the app (`RootState.dataDiskReplaced`). `guest` is the choices in
    /// memory, or nil to read them from disk; with none there is nothing to
    /// settle.
    private func settle(_ guest: Guest?, after record: SnapshotRecord) throws -> Guest? {
        var current = guest
        if current == nil, FileManager.default.fileExists(atPath: locations.settings.path) {
            current = try JSONDecoder().decode(Guest.self, from: Data(contentsOf: locations.settings))
        }
        guard var settled = current else { return nil }
        settled.capacityGiB = max(settled.capacityGiB, record.diskGiB)
        if let pending = settled.pendingCapacityGiB, pending <= settled.capacityGiB { settled.pendingCapacityGiB = nil }
        settled.root.dataDiskReplaced()
        try save(settled)
        return settled
    }
}
