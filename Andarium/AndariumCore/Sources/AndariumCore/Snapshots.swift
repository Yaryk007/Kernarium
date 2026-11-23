// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Snapshots of the guest's data disk: what one keeps, when each operation is
/// allowed and why one is refused. Everything here is pure; `SnapshotStore`
/// does the files, `TwoDiskStorage` the guest's choices around them, and the
/// application's driver the stopped-guest gate.
///
/// A snapshot keeps `data.img` and the record `instance.json` of the Android
/// that last started on it, as an APFS clone, so it costs nothing when made
/// and then the blocks the guest rewrites. The guest's choices, the system
/// disk, the adb key and the session records are not in it. It is made only
/// after Android's own shut down, so it is always a shut-down disk, and a
/// restore never keeps the state it replaces.

/// A snapshot's identity: a lowercase UUID, which also names its directory, so
/// no name from a file or a caller can reach outside the store.
public struct SnapshotID: Hashable, Sendable, Codable, CustomStringConvertible {
    public let rawValue: String

    public init() {
        rawValue = UUID().uuidString.lowercased()
    }

    public init?(_ text: String) {
        guard Self.isValid(text) else { return nil }
        rawValue = text
    }

    public init(from decoder: Decoder) throws {
        let text = try decoder.singleValueContainer().decode(String.self)
        guard let id = SnapshotID(text) else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: decoder.codingPath, debugDescription: "Not a snapshot identity."))
        }
        self = id
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(rawValue)
    }

    public var description: String { rawValue }

    static func isValid(_ text: String) -> Bool {
        text.range(
            of: "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", options: .regularExpression
        ) != nil
    }
}

/// `snapshot.json`, schema 1: what the store knows about a snapshot beyond its
/// two copies. The release and build are those of the record it copied, kept
/// here for the list; the copy of `instance.json` decides what a restore does.
public struct SnapshotRecord: Equatable, Sendable {
    public var name: String
    public var created: Date
    public var releaseID: String
    public var buildIncremental: String
    /// The data disk's size, a whole number of GiB.
    public var diskGiB: Int

    public init(name: String, created: Date, releaseID: String, buildIncremental: String, diskGiB: Int) {
        self.name = name
        self.created = created
        self.releaseID = releaseID
        self.buildIncremental = buildIncremental
        self.diskGiB = diskGiB
    }

    public var buildNumber: Int { Int(buildIncremental) ?? 0 }

    static func timestamp(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }

    static func date(_ text: String) -> Date? {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        if let date = formatter.date(from: text) { return date }
        formatter.formatOptions = [.withInternetDateTime]
        return formatter.date(from: text)
    }
}

extension SnapshotRecord: Codable {
    private enum CodingKeys: String, CodingKey {
        case schema, name, created
        case releaseID = "release_id"
        case buildIncremental = "build_incremental"
        case diskGiB = "disk_gib"
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        guard try values.decode(Int.self, forKey: .schema) == 1 else {
            throw InstanceError.state("A snapshot's snapshot.json is of a schema Andarium does not know.")
        }
        guard let created = Self.date(try values.decode(String.self, forKey: .created)) else {
            throw InstanceError.state("A snapshot's snapshot.json has an invalid creation time.")
        }
        let disk = try values.decode(Int.self, forKey: .diskGiB)
        guard disk >= 1 else { throw InstanceError.state("A snapshot's snapshot.json has an invalid disk size.") }
        self.init(
            name: try values.decode(String.self, forKey: .name), created: created,
            releaseID: try values.decode(String.self, forKey: .releaseID),
            buildIncremental: try values.decode(String.self, forKey: .buildIncremental), diskGiB: disk
        )
    }

    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(1, forKey: .schema)
        try values.encode(name, forKey: .name)
        try values.encode(Self.timestamp(created), forKey: .created)
        try values.encode(releaseID, forKey: .releaseID)
        try values.encode(buildIncremental, forKey: .buildIncremental)
        try values.encode(diskGiB, forKey: .diskGiB)
    }
}

/// Why an operation on the snapshots does not go ahead. A value, never
/// words: each screen says it in its own.
public enum SnapshotRefusal: Error, Equatable, Sendable {
    /// The guest is not stopped: it boots, runs (also paused out of the
    /// foreground or for want of space), restarts, or ended in failure and
    /// still holds its disk until the application is reopened.
    case guestNotStopped
    /// Another snapshot operation, or an erase or a setup, is under way.
    case busy
    /// The guest has no data disk yet: it has not started once.
    case noData
    /// `instance.json` cannot be read.
    case unreadableRecord
    /// The last start did not end with Android's own shut down, so the disk
    /// is not in a state a snapshot may keep.
    case notShutDown
    /// An update started and did not finish; its data may be half upgraded.
    case unfinishedUpdate
    /// The guest's files do not match this app version: another system disk
    /// under the same release, data nothing recorded, or an unsigned proof
    /// and data of another system (`StartBlock.mismatch`).
    case mismatch
    /// The volume cannot clone files (a Mac home that is not APFS).
    case volumeCannotClone
    /// The volume has no room for the small files a snapshot adds.
    case notEnoughSpace
    /// The snapshot's Android is newer than the one this app carries; a newer
    /// app can restore it.
    case newerAndroid
    /// The snapshot's data layout is older than this app's and no migration
    /// takes it forward (`StartBlock.newerDataLayout`, the app's side).
    case olderDataLayout
    /// The snapshot was taken on a system disk this app's release does not
    /// carry; also a snapshot of another system under an unsigned proof, and
    /// a proof's snapshot under another system.
    case differentSystem
    /// The snapshot's disk size is outside what this app's release accepts.
    case diskSizeNotAccepted(gib: Int)
    /// There is no such snapshot.
    case missing
    /// The snapshot is damaged: a file is missing or cannot be read. It can
    /// be deleted.
    case incomplete
    /// A restore did not finish and completes at the next launch or start.
    case restoreUnfinished
    /// The name has a control character or is longer than `SnapshotRules.maximumNameLength`.
    case invalidName
}

/// What restoring a snapshot would do, from its record against the Android
/// this app carries.
public enum SnapshotRestorePlan: Equatable, Sendable {
    /// The data is put back as it is and the next start boots the snapshot's
    /// own Android.
    case asIs
    /// The data is put back and the next start updates it to the Android this
    /// app carries, as any older guest is updated.
    case updatesAtNextStart(from: String, to: String)
    case refused(SnapshotRefusal)
}

public enum SnapshotRules {
    public static let maximumNameLength = 100

    /// The name as stored: trimmed, or nil when it cannot be one. An empty
    /// name is allowed; a screen shows the date for it.
    public static func normalizedName(_ raw: String) -> String? {
        let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard name.count <= maximumNameLength else { return nil }
        guard name.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) }) else { return nil }
        return name
    }

    /// Whether the last normal start on the disk reached `sys.boot_completed`
    /// and ended with Android's own power off: `lastCompleted` names the
    /// record's release and is later than the last start (a start in the same
    /// second as the completion is not shut down: the clock has no finer
    /// word). Everything else, from iOS closing a paused guest to a Recovery
    /// session, leaves the disk as a phone is left by a power cut.
    public static func wasShutDown(_ state: InstanceState) -> Bool {
        guard state.pendingUpdate == nil, let completed = state.lastCompleted,
            completed.releaseID == state.releaseID,
            let completedAt = SnapshotRecord.date(completed.at), let bootedAt = SnapshotRecord.date(state.lastBooted)
        else { return false }
        return completedAt > bootedAt
    }

    /// Why Create refuses on files with `state` (nil: no record) and whether
    /// the data disk exists, or nil when it may go ahead. The refusals of a
    /// guest that is not stopped, of a restore under way and of the volume
    /// are the caller's.
    ///
    /// A newer Android that waits to update the data, an app older than the
    /// data and an app whose data layout cannot take the data forward all
    /// allow it: the data is intact, and the snapshot is what a careful person
    /// takes before an update.
    public static func createRefusal(
        state: InstanceState?, dataDiskExists: Bool, release: ReleaseIdentity
    ) -> SnapshotRefusal? {
        guard dataDiskExists else { return .noData }
        guard let state else { return .mismatch }
        if state.pendingUpdate != nil { return .unfinishedUpdate }
        let plan = InstanceRules.plan(state: state, dataDiskExists: true, snapshotExists: false, release: release)
        if case .blocked(.mismatch) = plan { return .mismatch }
        return wasShutDown(state) ? nil : .notShutDown
    }

    /// What a restore of the snapshot with this copy of the record and disk
    /// size does on this app's release. A newer Android or data layout, or a
    /// disk size the release refuses, cannot be restored here; the snapshot
    /// stays listed until a newer app can.
    public static func restorePlan(of record: InstanceState, diskGiB: Int, release: ReleaseIdentity) -> SnapshotRestorePlan {
        guard record.pendingUpdate == nil else { return .refused(.incomplete) }
        let plan = InstanceRules.plan(state: record, dataDiskExists: true, snapshotExists: false, release: release)
        let result: SnapshotRestorePlan
        switch plan {
        case .normal:
            result = .asIs
        case .update(let from, let to):
            result = .updatesAtNextStart(from: from, to: to)
        case .blocked(.olderAndroid):
            return .refused(.newerAndroid)
        case .blocked(.newerDataLayout):
            return .refused(.olderDataLayout)
        case .blocked(.mismatch):
            return .refused(.differentSystem)
        case .firstStart, .unfinishedUpdate:
            return .refused(.incomplete)
        }
        guard (release.minimumGiB...release.maximumGiB).contains(diskGiB) else {
            return .refused(.diskSizeNotAccepted(gib: diskGiB))
        }
        return result
    }

    /// Whether the runtime holds the guest's disk or is about to: a run is
    /// pending for the display, launching, preparing its files or running.
    /// Each of the four keeps the disk from a snapshot operation.
    public static func runtimeActive(running: Bool, pending: Bool, launching: Bool, preparing: Bool) -> Bool {
        running || pending || launching || preparing
    }

    /// Whether a snapshot operation may start now, from the application's
    /// state: stopped means Home with no run active, pending or launching in
    /// the runtime (`runtimeActive`), and nothing else under way. No file
    /// lock says this on iOS, so the application's own state is the lock.
    public static func gate(phase: Phase, runtimeActive: Bool, operationUnderWay: Bool) -> SnapshotRefusal? {
        if phase != .home || runtimeActive { return .guestNotStopped }
        if operationUnderWay { return .busy }
        return nil
    }
}

/// A space figure that says whether it is exact.
public struct SnapshotFigure: Equatable, Sendable {
    public var bytes: UInt64
    /// The measure was refused where the exact figure needs it, so the real
    /// one is at least this.
    public var isLowerBound: Bool

    public init(bytes: UInt64, isLowerBound: Bool = false) {
        self.bytes = bytes
        self.isLowerBound = isLowerBound
    }
}

/// What the snapshots hold, as the device's free space sees it.
public struct SnapshotSpace: Equatable, Sendable {
    /// What deleting all the snapshots would free. A lower bound where the
    /// extent map is refused.
    public var snapshotsUse: SnapshotFigure
    /// What an unfinished update's own copy (`data.img.pre-*`) holds, apart
    /// from the snapshots; nil while no update is unfinished.
    public var updateCopy: SnapshotFigure?

    public init(snapshotsUse: SnapshotFigure = SnapshotFigure(bytes: 0), updateCopy: SnapshotFigure? = nil) {
        self.snapshotsUse = snapshotsUse
        self.updateCopy = updateCopy
    }
}

/// One snapshot as the screens list it.
public struct SnapshotSummary: Equatable, Sendable, Identifiable {
    public var id: SnapshotID
    /// The person's label; empty when they gave none.
    public var name: String
    public var created: Date
    public var releaseID: String
    public var buildNumber: Int
    public var diskGiB: Int
    /// What a restore would do on this app's release.
    public var restore: SnapshotRestorePlan
    /// The data it would take on its own (its allocated size), which is what
    /// it holds after an Android factory reset.
    public var kept: SnapshotFigure
    /// What deleting it alone frees. Small while it is new, and it grows while
    /// the guest runs, and when another snapshot is deleted.
    public var freesWhenDeleted: SnapshotFigure

    public init(
        id: SnapshotID, name: String, created: Date, releaseID: String, buildNumber: Int, diskGiB: Int,
        restore: SnapshotRestorePlan, kept: SnapshotFigure = SnapshotFigure(bytes: 0),
        freesWhenDeleted: SnapshotFigure = SnapshotFigure(bytes: 0)
    ) {
        self.id = id
        self.name = name
        self.created = created
        self.releaseID = releaseID
        self.buildNumber = buildNumber
        self.diskGiB = diskGiB
        self.restore = restore
        self.kept = kept
        self.freesWhenDeleted = freesWhenDeleted
    }

    /// The snapshot is damaged: delete is all that can be done with it.
    public var isDamaged: Bool { restore == .refused(.incomplete) }
}

/// An I/O error as a value, for a screen to say in its own words or to show
/// the system's description of.
public struct SnapshotFailure: Equatable, Sendable {
    public var domain: String
    public var code: Int
    /// The system's own description.
    public var description: String

    public init(_ error: any Error) {
        let bridged = error as NSError
        domain = bridged.domain
        code = bridged.code
        description = error.localizedDescription
    }
}

/// A restore that put the snapshot's data in place and did not finish. It
/// completes when the guest starts or Andarium is opened again; until it does,
/// Create and Restore are refused, and every snapshot but this one can be
/// deleted.
public struct SnapshotUnfinishedRestore: Equatable, Sendable {
    /// The snapshot being restored; nil when the record of the restore cannot be read.
    public var snapshot: SnapshotID?
    /// What stopped the last attempt to finish it in this process, if one did.
    public var lastFailure: SnapshotFailure?

    public init(snapshot: SnapshotID? = nil, lastFailure: SnapshotFailure? = nil) {
        self.snapshot = snapshot
        self.lastFailure = lastFailure
    }
}

/// The Android a guest's data, or a snapshot, is on.
public struct SnapshotRelease: Equatable, Sendable {
    public var releaseID: String
    public var buildNumber: Int

    public init(releaseID: String, buildNumber: Int) {
        self.releaseID = releaseID
        self.buildNumber = buildNumber
    }
}

/// The snapshots, their space and what may be done now.
public struct SnapshotListing: Equatable, Sendable {
    /// Newest first.
    public var items: [SnapshotSummary]
    public var space: SnapshotSpace
    /// Why Create refuses on the guest's files, or nil. It does not include
    /// `whileNotStopped`.
    public var createRefusal: SnapshotRefusal?
    /// Why Create, Restore and Delete cannot run now, from the application's
    /// state, or nil when the guest is stopped and idle. Rename and the list
    /// do not depend on it.
    public var whileNotStopped: SnapshotRefusal?
    /// A restore that did not finish, or nil.
    public var unfinishedRestore: SnapshotUnfinishedRestore?
    /// The release the guest's data is on now, from its record, also when a
    /// start would be blocked or a snapshot cannot be made; nil when there is
    /// no readable record.
    public var dataRelease: SnapshotRelease?
    /// Whether the figures were measured. In a listing that was not, every
    /// figure is zero as a lower bound: names, dates, releases and restore
    /// plans are all there, and the measured listing follows.
    public var isMeasured: Bool
    /// How many snapshot operations the driver had finished when this listing
    /// was read; a screen that holds an older one than the driver's current
    /// count reads again.
    public var revision: Int

    public init(
        items: [SnapshotSummary] = [], space: SnapshotSpace = SnapshotSpace(),
        createRefusal: SnapshotRefusal? = nil, whileNotStopped: SnapshotRefusal? = nil,
        unfinishedRestore: SnapshotUnfinishedRestore? = nil, dataRelease: SnapshotRelease? = nil,
        isMeasured: Bool = true, revision: Int = 0
    ) {
        self.items = items
        self.space = space
        self.createRefusal = createRefusal
        self.whileNotStopped = whileNotStopped
        self.unfinishedRestore = unfinishedRestore
        self.dataRelease = dataRelease
        self.isMeasured = isMeasured
        self.revision = revision
    }
}

/// How a snapshot operation ended. A refusal is a value; a failure is an I/O
/// error nobody foresaw, whose description is the system's.
public enum SnapshotResult<Value: Sendable>: Sendable {
    case done(Value)
    case refused(SnapshotRefusal)
    case failed(any Error)

    /// Runs `body`, turning a thrown refusal into `.refused` and any other
    /// error into `.failed`.
    public init(catching body: () throws -> Value) {
        do {
            self = .done(try body())
        } catch let refusal as SnapshotRefusal {
            self = .refused(refusal)
        } catch is SnapshotRestoreUnfinished {
            // The files are in place and the restore completes at the next
            // launch or start.
            self = .refused(.restoreUnfinished)
        } catch {
            self = .failed(error)
        }
    }

    public var value: Value? {
        if case .done(let value) = self { value } else { nil }
    }

    public var refusal: SnapshotRefusal? {
        if case .refused(let refusal) = self { refusal } else { nil }
    }
}
