// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What the driver is doing with the guest's files that keeps a snapshot
/// operation from starting.
enum SnapshotWork: Equatable {
    case create, restore, delete, rename
    /// The guest is being set up or erased.
    case guestStorage
}

/// What a screen's controls follow: the driver sets it around each operation
/// (`AppModel.snapshotActivity`).
struct SnapshotActivity: Equatable {
    /// The runtime holds the guest's disk or is about to: a run is pending,
    /// launching, preparing its files or running (`SnapshotRules.runtimeActive`).
    var runtimeActive = false
    /// The operation under way, or nil.
    var operation: SnapshotWork?
    /// The snapshot operations finished so far, including a guest's setup and
    /// erase. A listing older than this count is stale: read again.
    var revision = 0

    /// An operation that holds the guest's files is under way: nothing else
    /// that changes them starts beside it, and the screens offer none. A
    /// rename is not one; it is allowed at any time, and a start or another
    /// operation waits for it (`SnapshotDriver.renameSnapshot`).
    var holdsGuestFiles: Bool {
        guard let operation else { return false }
        return operation != .rename
    }
}

/// What the screens call to work with the snapshots of the guest's data disk
/// (`Andarium/README.md#model-and-storage`). The runtime's driver acts on the
/// guest's files; the preview's plays mock snapshots. Nothing here is words:
/// a refusal is a `SnapshotRefusal`, and each screen says it in its own.
@MainActor
protocol SnapshotDriver: AnyObject {
    /// What the driver is doing now. The model keeps its own copy current
    /// through `activityChanged`, which the driver calls on each change.
    var activity: SnapshotActivity { get }
    var activityChanged: (@MainActor (SnapshotActivity) -> Void)? { get set }

    /// Why Create, Restore and Delete cannot run now, or nil when the guest
    /// is stopped and nothing else is under way: Home with no run active,
    /// pending or launching. Rename and the list do not depend on it. Every
    /// operation checks it again and refuses with it.
    var snapshotGate: SnapshotRefusal? { get }
    /// The snapshots newest first, each with what deleting it frees and what
    /// it would take alone, the space they use together and what Create would
    /// say. The figures are measured off the main actor. `measured: false`
    /// leaves the figures out and returns without measuring, so the names can
    /// show first (`SnapshotListing.isMeasured`). A caller that is cancelled
    /// stops the measuring and gets the names, unmeasured.
    ///
    /// A listing is never older than the last finished operation: it waits for
    /// one under way and reads again if one started or finished while it read.
    /// It does not hold the gate: an operation can start during a listing, and
    /// the listing's `revision` is then behind the driver's `activity`.
    func snapshotListing(measured: Bool) async -> SnapshotListing
    /// A snapshot of the data as it is. Allowed only while the guest is
    /// stopped and after Android's own shut down.
    func createSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary>
    /// Puts the snapshot's data back in place of the guest's. `guest` is the
    /// guest as the screens hold it; the result is the guest with the capacity
    /// the restored disk needs. The state it replaces is not kept. A restore
    /// that put the data in place and could not finish is refused with
    /// `restoreUnfinished` and completes at the next start or launch.
    func restoreSnapshot(_ id: SnapshotID, guest: Guest) async -> SnapshotResult<Guest>
    /// Deletes a snapshot, which frees what it alone held.
    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void>
    /// Renames a snapshot, as one flushed operation after the one under way;
    /// allowed at any time. Screens debounce typing.
    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void>
}

extension SnapshotDriver {
    /// The listing with its figures.
    func snapshotListing() async -> SnapshotListing {
        await snapshotListing(measured: true)
    }
}

/// The screens' side of the snapshots: the driver's operations with what the
/// model must do after each. With no driver (a model that plays nothing),
/// every operation refuses with `noData`.
extension AppModel {
    func snapshotListing(measured: Bool = true) async -> SnapshotListing {
        guard let snapshotDriver else { return SnapshotListing(createRefusal: .noData, whileNotStopped: .noData) }
        return await snapshotDriver.snapshotListing(measured: measured)
    }

    /// Why Create, Restore and Delete cannot run now (`SnapshotDriver.snapshotGate`),
    /// from the phase and the driver's activity, both of which a view observes.
    var snapshotGate: SnapshotRefusal? {
        guard snapshotDriver != nil else { return .noData }
        return SnapshotRules.gate(
            phase: phase, runtimeActive: snapshotActivity.runtimeActive, operationUnderWay: snapshotActivity.holdsGuestFiles)
    }

    func takeSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary> {
        guard let snapshotDriver else { return .refused(.noData) }
        return await snapshotDriver.createSnapshot(named: name)
    }

    /// Restores the snapshot and adopts what changed: the guest's capacity and
    /// its used space, and what a start would now be (an older snapshot
    /// updates at the next start). No start begins meanwhile (`start`).
    func restoreSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        guard let snapshotDriver, let guest else { return .refused(.noData) }
        // The choices on disk are the ones the restore settles; a change
        // made a moment ago is not lost.
        persistNow()
        // A request to open an Android app that waits for the restore is
        // released only once this has adopted what the restore left.
        androidApps.restoring = true
        defer { androidAppsRestoreFinished() }
        let result = await snapshotDriver.restoreSnapshot(id, guest: guest)
        switch result {
        case .done(let restored):
            lifecycle.guest = restored
            refreshPlan()
            // The preview reads no files: it plays the plan the restore leaves.
            if self.driver == nil, let planAfterRestore { plan = planAfterRestore }
            return .done(())
        case .refused(let refusal):
            // A restore that took effect and did not finish changed the files:
            // the data disk is the snapshot's already, whether or not its
            // record could be written. The recovery that ends the restore at
            // the next start settles the settings on disk, and this guest is
            // saved over them before it, so it forgets the Magisk app now.
            if refusal == .restoreUnfinished {
                self.guest?.root.dataDiskReplaced()
                persistNow()
                refreshPlan()
            }
            return .refused(refusal)
        case .failed(let error):
            refreshPlan()
            return .failed(error)
        }
    }

    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        guard let snapshotDriver else { return .refused(.noData) }
        return await snapshotDriver.deleteSnapshot(id)
    }

    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void> {
        guard let snapshotDriver else { return .refused(.noData) }
        return await snapshotDriver.renameSnapshot(id, to: name)
    }

    /// Home's "System image updated" card gives way to the note of a restored
    /// older snapshot, or of a restore that did not finish, that the next start
    /// updates it: the card would say it again.
    var updateNoticeIsShown: Bool { restoredSnapshot?.updatesTo == nil && !snapshots.unfinishedRestoreUpdates }

    /// The guest was erased or made anew: nothing read about its snapshots holds.
    func forgetSnapshots() {
        snapshots.reset()
        restoredSnapshot = nil
    }
}
