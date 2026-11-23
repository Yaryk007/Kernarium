// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// What a preview's snapshots start as: the list, the figures around it, and
/// the state that gates the operations, so a screen's every state can be set
/// up from a launch argument or a test (`PreviewSnapshotDriver`).
struct PreviewSnapshotSeed {
    var items: [SnapshotSummary] = []
    /// The space the snapshots use all together. nil is the sum of what each
    /// frees; a design sets one to show a total above the sum, as blocks that
    /// two snapshots share make it, and a figure that grows when a neighbour
    /// is deleted.
    var total: SnapshotFigure?
    /// What an unfinished update's own copy holds; nil when none is.
    var updateCopy: SnapshotFigure?
    /// The space the snapshots use is shown as a lower bound, as where the
    /// extent map is refused (with no `total`).
    var useIsLowerBound = false
    /// Why Create refuses on the guest's files (`SnapshotListing.createRefusal`).
    var createRefusal: SnapshotRefusal?
    /// The release the guest's data is on now.
    var dataRelease: SnapshotRelease?
    /// A restore that did not finish (`SnapshotListing.unfinishedRestore`).
    var unfinishedRestore: SnapshotUnfinishedRestore?
    /// Why Create, Restore and Delete cannot run now, as with a guest that
    /// runs (`SnapshotDriver.snapshotGate`).
    var gate: SnapshotRefusal?
    /// The time a new snapshot is taken at, when the clock must not decide
    /// the order of a capture's rows; nil is now.
    var now: Date?
    /// How long an operation takes, which a capture of its overlay needs.
    var latency: Duration = .milliseconds(400)
    /// How long a measured listing takes, so a design can show the names
    /// first and the figures arriving.
    var measuringLatency: Duration = .zero
    /// The failure the next operation ends with, once.
    var failure: (any Error)?
}

extension PreviewSnapshotSeed {
    private static func gibibytes(_ count: Double) -> UInt64 { UInt64(count * Double(Limits.bytesPerGiB)) }
    private static func mebibytes(_ count: Double) -> UInt64 { UInt64(count * Double(Limits.bytesPerMiB)) }

    /// A day of the design's mock history, in 2026.
    static func day(_ month: Int, _ day: Int, _ hour: Int, _ minute: Int) -> Date {
        Calendar.current.date(from: DateComponents(year: 2026, month: month, day: day, hour: hour, minute: minute)) ?? Date()
    }

    /// The release of a snapshot a newer app took, which this build cannot restore.
    static let newerRelease = "20261110-los24-r22"
    static let oldestRelease = "20261020-los24-r17"

    static func item(
        _ name: String, at created: Date, release: String, frees: UInt64, kept: UInt64,
        restore: SnapshotRestorePlan? = nil
    ) -> SnapshotSummary {
        let plan: SnapshotRestorePlan
        if let restore {
            plan = restore
        } else if release == PreviewData.systemRelease {
            plan = .asIs
        } else {
            plan = .updatesAtNextStart(from: release, to: PreviewData.systemRelease)
        }
        return SnapshotSummary(
            id: SnapshotID(), name: name, created: created, releaseID: release, buildNumber: 1_793_463_800, diskGiB: 16,
            restore: plan, kept: SnapshotFigure(bytes: kept), freesWhenDeleted: SnapshotFigure(bytes: frees))
    }

    /// The design's guest in everyday use with five snapshots: two of this
    /// app's r20, one that a newer app took and this one cannot restore, the
    /// one taken before the update to r20 and the clean setup on r17. What
    /// they share is 380 MiB, which no row counts and the total does.
    static func sample() -> PreviewSnapshotSeed {
        let items = [
            item("Before trying Winlator", at: day(11, 11, 7, 45), release: PreviewData.systemRelease,
                 frees: mebibytes(214), kept: gibibytes(5.3)),
            item("", at: day(11, 10, 19, 20), release: PreviewData.systemRelease,
                 frees: mebibytes(96), kept: gibibytes(5.2)),
            item("Newer build", at: day(11, 10, 12, 10), release: newerRelease,
                 frees: mebibytes(640), kept: gibibytes(5.1), restore: .refused(.newerAndroid)),
            item("Before r20", at: day(11, 9, 8, 55), release: PreviewData.previousSystemRelease,
                 frees: gibibytes(1.9), kept: gibibytes(4.8)),
            item("Fresh setup", at: day(10, 24, 10, 30), release: oldestRelease,
                 frees: gibibytes(1.4), kept: gibibytes(2.1)),
        ]
        var seed = PreviewSnapshotSeed(
            items: items, dataRelease: SnapshotRelease(releaseID: PreviewData.systemRelease, buildNumber: 1_793_463_800))
        seed.total = SnapshotFigure(bytes: items.reduce(mebibytes(380)) { $0 + $1.freesWhenDeleted.bytes })
        seed.now = day(11, 11, 9, 2)
        return seed
    }

    /// Fourteen snapshots, for a list that scrolls.
    static func many() -> PreviewSnapshotSeed {
        let names = [
            "Before trying Winlator", "", "Game saves", "", "Before r20", "", "Banking app set up", "", "",
            "Clean launcher", "", "Before r18", "", "Fresh setup",
        ]
        var seed = sample()
        seed.items = names.enumerated().map { index, name in
            let release = index < 4 ? PreviewData.systemRelease : index < 11 ? PreviewData.previousSystemRelease : oldestRelease
            return item(
                name, at: day(11, 11, 8, 0).addingTimeInterval(Double(-index) * 86_400 * 1.6 - Double(index) * 3_700),
                release: release, frees: mebibytes(Double(40 + index * index * 9)), kept: gibibytes(5.3 - Double(index) * 0.2))
        }
        seed.total = SnapshotFigure(bytes: seed.items.reduce(mebibytes(520)) { $0 + $1.freesWhenDeleted.bytes })
        return seed
    }

    /// A snapshot whose files are missing: it can only be deleted.
    static func damaged() -> SnapshotSummary {
        item("Before the big cleanup", at: day(11, 10, 22, 5), release: PreviewData.systemRelease,
             frees: mebibytes(58), kept: gibibytes(5.2), restore: .refused(.incomplete))
    }
}

/// The preview's snapshots: seeded ones that the operations change, with the
/// refusals the real driver gives, the activity it reports and an optional
/// delay. Nothing touches a file.
@MainActor
final class PreviewSnapshotDriver: SnapshotDriver {
    private(set) var seed: PreviewSnapshotSeed
    private(set) var activity = SnapshotActivity() {
        didSet { if activity != oldValue { activityChanged?(activity) } }
    }
    var activityChanged: (@MainActor (SnapshotActivity) -> Void)?

    init(_ seed: PreviewSnapshotSeed = PreviewSnapshotSeed()) {
        self.seed = seed
        apply(gate: seed.gate)
    }

    /// Changes the state that gates the operations, as the guest starting or
    /// stopping would.
    func setGate(_ refusal: SnapshotRefusal?) {
        seed.gate = refusal
        apply(gate: refusal)
    }

    /// What the gate looks like in the activity the model follows: a guest
    /// that runs holds the disk, anything else under way is one operation.
    private func apply(gate: SnapshotRefusal?) {
        activity.runtimeActive = gate == .guestNotStopped
        if gate == .busy {
            activity.operation = .guestStorage
        } else if activity.operation == .guestStorage {
            activity.operation = nil
        }
    }

    /// Slows the measuring from now on, so a capture can show the figures
    /// giving way to the measuring state after an operation.
    func setMeasuringLatency(_ latency: Duration) {
        seed.measuringLatency = latency
    }

    var snapshotGate: SnapshotRefusal? {
        seed.gate ?? (activity.operation != nil ? .busy : nil)
    }

    func snapshotListing(measured: Bool) async -> SnapshotListing {
        let revision = activity.revision
        var items = seed.items.sorted { $0.created > $1.created }
        let unknown = SnapshotFigure(bytes: 0, isLowerBound: true)
        var space = SnapshotSpace(
            snapshotsUse: seed.total ?? SnapshotFigure(
                bytes: items.reduce(0) { $0 + $1.freesWhenDeleted.bytes }, isLowerBound: seed.useIsLowerBound),
            updateCopy: seed.updateCopy)
        if measured {
            if seed.measuringLatency > .zero { try? await Task.sleep(for: seed.measuringLatency) }
        } else {
            for index in items.indices {
                items[index].kept = unknown
                items[index].freesWhenDeleted = unknown
            }
            space = SnapshotSpace(snapshotsUse: unknown, updateCopy: seed.updateCopy == nil ? nil : unknown)
        }
        return SnapshotListing(
            items: items, space: space, createRefusal: seed.createRefusal, whileNotStopped: snapshotGate,
            unfinishedRestore: seed.unfinishedRestore, dataRelease: seed.dataRelease, isMeasured: measured, revision: revision)
    }

    func createSnapshot(named name: String) async -> SnapshotResult<SnapshotSummary> {
        await perform(.create, gated: true) {
            guard let name = SnapshotRules.normalizedName(name) else { return .refused(.invalidName) }
            if self.seed.unfinishedRestore != nil { return .refused(.restoreUnfinished) }
            if let refusal = self.seed.createRefusal { return .refused(refusal) }
            let kept = self.seed.items.map(\.kept.bytes).max() ?? 0
            let made = SnapshotSummary(
                id: SnapshotID(), name: name, created: self.seed.now ?? Date(), releaseID: PreviewData.systemRelease,
                buildNumber: 1_793_463_800, diskGiB: 16, restore: .asIs, kept: SnapshotFigure(bytes: kept),
                freesWhenDeleted: SnapshotFigure(bytes: 16384))
            self.seed.items.append(made)
            return .done(made)
        }
    }

    func restoreSnapshot(_ id: SnapshotID, guest: Guest) async -> SnapshotResult<Guest> {
        await perform(.restore, gated: true) {
            if self.seed.unfinishedRestore != nil { return .refused(.restoreUnfinished) }
            guard let item = self.seed.items.first(where: { $0.id == id }) else { return .refused(.missing) }
            if case .refused(let reason) = item.restore { return .refused(reason) }
            var restored = guest
            restored.capacityGiB = max(guest.capacityGiB, item.diskGiB)
            restored.usedBytes = item.kept.bytes
            return .done(restored)
        }
    }

    func deleteSnapshot(_ id: SnapshotID) async -> SnapshotResult<Void> {
        await perform(.delete, gated: true) {
            guard self.seed.items.contains(where: { $0.id == id }) else { return .refused(.missing) }
            // The snapshot a restore under way names stays; every other can go.
            if self.seed.unfinishedRestore != nil, self.seed.unfinishedRestore?.snapshot == id || self.seed.unfinishedRestore?.snapshot == nil {
                return .refused(.restoreUnfinished)
            }
            self.hand(onFrom: id)
            self.seed.items.removeAll { $0.id == id }
            return .done(())
        }
    }

    /// Old data that two snapshots shared is the older one's alone once the
    /// newer one is gone: a figure that grows, and a total that shrinks by less
    /// than the one that went.
    private func hand(onFrom id: SnapshotID) {
        let ordered = seed.items.sorted { $0.created > $1.created }
        guard let at = ordered.firstIndex(where: { $0.id == id }), at + 1 < ordered.count, let total = seed.total
        else { return }
        let sum = ordered.reduce(0) { $0 + $1.freesWhenDeleted.bytes }
        let shared = total.bytes > sum ? total.bytes - sum : 0
        let handed = shared / 3
        guard let older = seed.items.firstIndex(where: { $0.id == ordered[at + 1].id }) else { return }
        seed.items[older].freesWhenDeleted.bytes += handed
        let gone = ordered[at].freesWhenDeleted.bytes
        let remaining = ordered.count - 1
        seed.total = SnapshotFigure(bytes: remaining < 2 ? sum - gone + handed : total.bytes - gone, isLowerBound: total.isLowerBound)
    }

    func renameSnapshot(_ id: SnapshotID, to name: String) async -> SnapshotResult<Void> {
        await perform(.rename, gated: false) {
            guard let name = SnapshotRules.normalizedName(name) else { return .refused(.invalidName) }
            guard let index = self.seed.items.firstIndex(where: { $0.id == id }) else { return .refused(.missing) }
            self.seed.items[index].name = name
            return .done(())
        }
    }

    /// Runs one operation: refused while the gate says so, after the seeded
    /// latency, or failed with the seeded failure.
    private func perform<Value: Sendable>(
        _ work: SnapshotWork, gated: Bool, _ body: @MainActor () -> SnapshotResult<Value>
    ) async -> SnapshotResult<Value> {
        if gated, let refusal = snapshotGate { return .refused(refusal) }
        activity.operation = work
        try? await Task.sleep(for: seed.latency)
        let result: SnapshotResult<Value>
        if let failure = seed.failure {
            seed.failure = nil
            result = .failed(failure)
        } else {
            result = body()
        }
        activity.operation = nil
        activity.revision += 1
        return result
    }
}
