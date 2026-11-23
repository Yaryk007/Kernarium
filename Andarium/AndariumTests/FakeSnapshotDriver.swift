// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
@testable import Andarium

/// A snapshot driver a test plays: the list it holds, what it refuses and
/// when, an operation it holds half way until the test lets it go, and a log
/// of what the screens asked. It reports its activity as the runtime's does,
/// and counts finished operations in `revision`.
@MainActor
final class FakeSnapshotDriver: SnapshotDriver {
    var items: [SnapshotSummary]
    /// What Create would say on the guest's files.
    var createRefusal: SnapshotRefusal?
    var unfinishedRestore: SnapshotUnfinishedRestore?
    /// Why the operations cannot run now, as with a guest that runs.
    var gate: SnapshotRefusal?
    /// What the next operation ends with, once.
    var nextRefusal: SnapshotRefusal?
    var nextFailure: (any Error)?
    /// The next restore refuses as one that took effect and could not finish.
    var restoreLeavesUnfinished = false
    /// The operation held at its start until `release()`.
    var holding: SnapshotWork?
    /// The measured reading waits until `releaseMeasuring()`.
    var holdMeasuring = false
    /// What a restore does to the files besides its data, as a test plays it
    /// (the files now say what a start would be).
    var onRestore: (@MainActor () -> Void)?
    /// Runs at the instant the operation is over for the screens (the runtime
    /// unlocks the guest's files) and before the result reaches the caller: a
    /// tap delivered now lands between the unlock and the model's adoption.
    var atUnlock: (@MainActor () -> Void)?

    private(set) var log: [String] = []
    private(set) var measuredReads = 0
    private(set) var quickReads = 0
    /// Measured readings that were cancelled while they waited: like the
    /// runtime's, they answer unmeasured.
    private(set) var cancelledMeasures = 0
    private var waiting: CheckedContinuation<Void, Never>?
    private var measuringWaiter: CheckedContinuation<Void, Never>?

    private(set) var activity = SnapshotActivity() {
        didSet { if activity != oldValue { activityChanged?(activity) } }
    }
    var activityChanged: (@MainActor (SnapshotActivity) -> Void)?

    init(items: [SnapshotSummary] = []) {
        self.items = items
    }

    var snapshotGate: SnapshotRefusal? {
        gate ?? (activity.operation != nil ? .busy : nil)
    }

    /// Lets the held operation go on.
    func release() {
        waiting?.resume()
        waiting = nil
    }

    func releaseMeasuring() {
        holdMeasuring = false
        measuringWaiter?.resume()
        measuringWaiter = nil
    }

    /// Another operation, seen from outside, finished: the listing is older.
    func finishedElsewhere() {
        activity.revision += 1
    }

    /// A run begins or ends, as the runtime reports it.
    func setRuntimeActive(_ active: Bool) {
        activity.runtimeActive = active
    }

    func snapshotListing(measured asked: Bool) async -> SnapshotListing {
        // The state the reading starts from, as the runtime's reads it before it measures.
        let revision = activity.revision
        var measured = asked
        if asked {
            measuredReads += 1
            if holdMeasuring { await withCheckedContinuation { measuringWaiter = $0 } }
            if Task.isCancelled {
                cancelledMeasures += 1
                measured = false
            }
        } else {
            quickReads += 1
        }
        var shown = items
        if !measured {
            let unknown = SnapshotFigure(bytes: 0, isLowerBound: true)
            for index in shown.indices {
                shown[index].kept = unknown
                shown[index].freesWhenDeleted = unknown
            }
        }
        let sum = items.reduce(0) { $0 + $1.freesWhenDeleted.bytes }
        return SnapshotListing(
            items: shown.sorted { $0.created > $1.created },
            space: measured ? SnapshotSpace(snapshotsUse: SnapshotFigure(bytes: sum)) : SnapshotSpace(
                snapshotsUse: SnapshotFigure(bytes: 0, isLowerBound: true)),
            createRefusal: createRefusal, whileNotStopped: snapshotGate, unfinishedRestore: unfinishedRestore,
            isMeasured: measured, revision: revision)
    }

    func createSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary> {
        await perform(.create, "create \(name)", gated: true) {
            if self.unfinishedRestore != nil { return .refused(.restoreUnfinished) }
            if let refusal = self.createRefusal { return .refused(refusal) }
            let made = SnapshotSummary(
                id: SnapshotID(), name: name, created: Date(), releaseID: "20261109-los24-r20", buildNumber: 1,
                diskGiB: 16, restore: .asIs, kept: SnapshotFigure(bytes: 4096), freesWhenDeleted: SnapshotFigure(bytes: 4096))
            self.items.append(made)
            return .done(made)
        }
    }

    func restoreSnapshot(_ id: SnapshotID, guest: Guest) async -> SnapshotResult<Guest> {
        await perform(.restore, "restore", gated: true) {
            if self.unfinishedRestore != nil { return .refused(.restoreUnfinished) }
            guard let item = self.items.first(where: { $0.id == id }) else { return .refused(.missing) }
            if case .refused(let reason) = item.restore { return .refused(reason) }
            if self.restoreLeavesUnfinished {
                self.unfinishedRestore = SnapshotUnfinishedRestore(
                    snapshot: id, lastFailure: SnapshotFailure(POSIXError(.ENOSPC)))
                return .refused(.restoreUnfinished)
            }
            var restored = guest
            restored.usedBytes = item.kept.bytes
            self.onRestore?()
            return .done(restored)
        }
    }

    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        await perform(.delete, "delete", gated: true) {
            guard self.items.contains(where: { $0.id == id }) else { return .refused(.missing) }
            if let unfinished = self.unfinishedRestore, unfinished.snapshot == id || unfinished.snapshot == nil {
                return .refused(.restoreUnfinished)
            }
            self.items.removeAll { $0.id == id }
            return .done(())
        }
    }

    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void> {
        await perform(.rename, "rename \(name)", gated: false) {
            guard let name = SnapshotRules.normalizedName(name) else { return .refused(.invalidName) }
            guard let index = self.items.firstIndex(where: { $0.id == id }) else { return .refused(.missing) }
            self.items[index].name = name
            return .done(())
        }
    }

    private func perform<Value: Sendable>(
        _ work: SnapshotWork, _ entry: String, gated: Bool, _ body: @MainActor () -> SnapshotResult<Value>
    ) async -> SnapshotResult<Value> {
        if gated, let refusal = snapshotGate { return .refused(refusal) }
        log.append(entry)
        activity.operation = work
        if holding == work {
            holding = nil
            await withCheckedContinuation { waiting = $0 }
        }
        let result: SnapshotResult<Value>
        if let refusal = nextRefusal {
            nextRefusal = nil
            result = .refused(refusal)
        } else if let failure = nextFailure {
            nextFailure = nil
            result = .failed(failure)
        } else {
            result = body()
        }
        activity.operation = nil
        activity.revision += 1
        atUnlock?()
        return result
    }
}

/// Snapshots as the screens list them, for tests.
enum SnapshotSample {
    static func item(
        _ name: String, release: String = "20261109-los24-r20", ago: Double = 1, frees: UInt64 = 1 << 20,
        kept: UInt64 = 1 << 30, restore: SnapshotRestorePlan? = nil
    ) -> SnapshotSummary {
        let plan = restore ?? (release == "20261109-los24-r20"
            ? .asIs : .updatesAtNextStart(from: release, to: "20261109-los24-r20"))
        return SnapshotSummary(
            id: SnapshotID(), name: name, created: Date(timeIntervalSince1970: 1_794_000_000 - ago * 86_400),
            releaseID: release, buildNumber: 1, diskGiB: 16, restore: plan,
            kept: SnapshotFigure(bytes: kept), freesWhenDeleted: SnapshotFigure(bytes: frees))
    }
}
