// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// A guest instance of the Android release: the data disk (`data.img`) and
/// `instance.json`, which says which release the disk was last started with.
/// The rules are those of `android/lineage-24/README.md#guest-instance-storage`;
/// `tools/android_instance.py` is the reference implementation and
/// `support/instance/update-decisions.json` holds the vectors both reproduce.

public enum InstanceError: Error, Equatable, LocalizedError {
    case release(String)
    case state(String)
    case refused(String)
    case cannotClone(String)

    public var errorDescription: String? {
        switch self {
        case .release(let message), .state(let message), .refused(let message): message
        case .cannotClone(let message): "The virtual machine's data cannot be snapshotted (it needs an APFS volume): \(message)"
        }
    }
}

/// How build numbers order releases: as integers, except that the numbers of
/// the counter the first public releases were built with, `legacyRange`, are
/// older than every number outside it.
public enum BuildNumber {
    public static let legacyRange: ClosedRange<Int> = 2_026_110_800...2_026_110_899

    /// The oldest build number there is: a placeholder release with it is
    /// never newer than any data.
    public static var oldest: Int { legacyRange.lowerBound }

    public static func isOlder(_ number: Int, than other: Int) -> Bool {
        let legacy = legacyRange.contains(number)
        if legacy != legacyRange.contains(other) { return legacy }
        return number < other
    }
}

/// What an instance records about the release it starts with, read from the
/// release's manifest (schema 6) and its partition layout (schema 2).
public struct ReleaseIdentity: Equatable, Sendable {
    public var releaseID: String
    public var buildFingerprint: String
    public var buildNumber: Int
    public var securityPatch: String
    public var systemDiskSHA256: String
    public var dataLayout: Int
    public var minimumGiB: Int
    public var defaultGiB: Int
    public var maximumGiB: Int
    /// Whether the manifest says the system is an unsigned proof image
    /// (`acceptance` "unsigned-proof", staged by `Andarium/scripts/
    /// stage-system-image.py --unsigned-proof` for a developer build). Such a
    /// system never updates an instance that has data, and an instance it
    /// created, which records so (`InstanceState.isUnsignedProof`), never
    /// updates to another system (`InstanceRules.decide`). Nothing else
    /// decides it: not the release ID.
    public var isUnsignedProof: Bool

    public init(
        releaseID: String, buildFingerprint: String, buildNumber: Int, securityPatch: String,
        systemDiskSHA256: String, dataLayout: Int, minimumGiB: Int, defaultGiB: Int, maximumGiB: Int,
        isUnsignedProof: Bool = false
    ) {
        self.releaseID = releaseID
        self.buildFingerprint = buildFingerprint
        self.buildNumber = buildNumber
        self.securityPatch = securityPatch
        self.systemDiskSHA256 = systemDiskSHA256
        self.dataLayout = dataLayout
        self.minimumGiB = minimumGiB
        self.defaultGiB = defaultGiB
        self.maximumGiB = maximumGiB
        self.isUnsignedProof = isUnsignedProof
    }

    /// The identity from a release's `release-manifest.json` and
    /// `guest/partition-layout.json`. It is never computed by hashing the
    /// disk at launch.
    public init(manifest: Data, layout: Data) throws {
        func object(_ data: Data, _ label: String) throws -> [String: Any] {
            guard let value = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                throw InstanceError.release("The release's \(label) cannot be read.")
            }
            return value
        }
        func integer(_ value: Any?) -> Int? {
            guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
            let result = number.intValue
            return result >= 1 ? result : nil
        }
        let manifest = try object(manifest, "manifest")
        let layout = try object(layout, "partition layout")
        guard
            let id = manifest["release_id"] as? String, Self.releaseIDIsValid(id),
            let android = manifest["android"] as? [String: Any],
            let fingerprint = android["fingerprint"] as? String, !fingerprint.isEmpty,
            let buildNumber = integer(android["build_number"]),
            let patch = android["security_patch"] as? String,
            let system = layout["system_disk"] as? [String: Any],
            let sha = system["sha256"] as? String, Self.sha256IsValid(sha),
            let data = layout["data_disk"] as? [String: Any],
            let dataLayout = integer(data["layout_version"]),
            let capacity = data["capacity_gib"] as? [String: Any],
            let minimum = integer(capacity["minimum"]),
            let standard = integer(capacity["default"]),
            let maximum = integer(capacity["maximum"])
        else {
            throw InstanceError.release("This copy of Andarium carries a system release without a usable identity.")
        }
        self.init(
            releaseID: id, buildFingerprint: fingerprint, buildNumber: buildNumber, securityPatch: patch,
            systemDiskSHA256: sha, dataLayout: dataLayout, minimumGiB: minimum, defaultGiB: standard,
            maximumGiB: maximum, isUnsignedProof: manifest["acceptance"] as? String == "unsigned-proof"
        )
    }

    static func releaseIDIsValid(_ id: String) -> Bool {
        id.range(of: "^[a-z0-9][a-z0-9._-]{0,63}$", options: .regularExpression) != nil
    }

    static func sha256IsValid(_ text: String) -> Bool {
        text.range(of: "^[0-9a-f]{64}$", options: .regularExpression) != nil
    }
}

/// `instance.json`: schema 1, or schema 2 for an instance an unsigned proof
/// created, which also says so (`unsigned_proof`). A reader of schema 1 alone
/// refuses a schema 2 record, so an app version before the proof rule cannot
/// update a proof's instance to a signed release.
public struct InstanceState: Equatable, Sendable {
    public struct Completed: Equatable, Sendable, Codable {
        public var releaseID: String
        public var buildIncremental: String
        public var at: String

        private enum CodingKeys: String, CodingKey {
            case releaseID = "release_id"
            case buildIncremental = "build_incremental"
            case at
        }
    }

    /// The identity the disk was last started with before an update.
    public struct Origin: Equatable, Sendable, Codable {
        public var releaseID: String
        public var buildFingerprint: String
        public var buildIncremental: String
        public var systemDiskSHA256: String
        public var dataLayout: Int
        public var lastBooted: String

        private enum CodingKeys: String, CodingKey {
            case releaseID = "release_id"
            case buildFingerprint = "build_fingerprint"
            case buildIncremental = "build_incremental"
            case systemDiskSHA256 = "system_disk_sha256"
            case dataLayout = "data_layout"
            case lastBooted = "last_booted"
        }
    }

    /// An update whose first start has not reached `sys.boot_completed`.
    public struct Pending: Equatable, Sendable, Codable {
        public var toReleaseID: String
        public var snapshot: String
        public var from: Origin

        private enum CodingKeys: String, CodingKey {
            case toReleaseID = "to_release_id"
            case snapshot
            case from
        }
    }

    public var dataLayout: Int
    public var releaseID: String
    public var buildFingerprint: String
    public var buildIncremental: String
    public var systemDiskSHA256: String
    public var createdWith: String
    public var lastBooted: String
    public var lastCompleted: Completed?
    public var pendingUpdate: Pending?
    /// Whether an unsigned proof created the instance: the record says so
    /// itself, whatever the release ID looks like.
    public var isUnsignedProof: Bool

    public init(
        dataLayout: Int, releaseID: String, buildFingerprint: String, buildIncremental: String,
        systemDiskSHA256: String, createdWith: String, lastBooted: String,
        lastCompleted: Completed? = nil, pendingUpdate: Pending? = nil, isUnsignedProof: Bool = false
    ) {
        self.dataLayout = dataLayout
        self.releaseID = releaseID
        self.buildFingerprint = buildFingerprint
        self.buildIncremental = buildIncremental
        self.systemDiskSHA256 = systemDiskSHA256
        self.createdWith = createdWith
        self.lastBooted = lastBooted
        self.lastCompleted = lastCompleted
        self.pendingUpdate = pendingUpdate
        self.isUnsignedProof = isUnsignedProof
    }

    public var buildNumber: Int { Int(buildIncremental) ?? 0 }

    /// A new instance created for `release` at `when`.
    public init(creating release: ReleaseIdentity, at when: String) {
        self.init(
            dataLayout: release.dataLayout, releaseID: release.releaseID, buildFingerprint: release.buildFingerprint,
            buildIncremental: String(release.buildNumber), systemDiskSHA256: release.systemDiskSHA256,
            createdWith: release.releaseID, lastBooted: when, isUnsignedProof: release.isUnsignedProof
        )
    }

    /// The keys of a schema 1 record; a schema 2 record has `unsigned_proof` too.
    static let keys: Set<String> = [
        "schema", "data_layout", "release_id", "build_fingerprint", "build_incremental", "system_disk_sha256",
        "created_with", "last_booted", "last_completed", "pending_update",
    ]
}

extension InstanceState: Codable {
    private enum CodingKeys: String, CodingKey {
        case schema
        case dataLayout = "data_layout"
        case releaseID = "release_id"
        case buildFingerprint = "build_fingerprint"
        case buildIncremental = "build_incremental"
        case systemDiskSHA256 = "system_disk_sha256"
        case createdWith = "created_with"
        case lastBooted = "last_booted"
        case lastCompleted = "last_completed"
        case pendingUpdate = "pending_update"
        case unsignedProof = "unsigned_proof"
    }

    public init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        // Schema 1 is every signed release's record; schema 2 is a proof's, and
        // says `unsigned_proof` true.
        let schema = try values.decode(Int.self, forKey: .schema)
        let proof = schema == 2 ? try values.decodeIfPresent(Bool.self, forKey: .unsignedProof) : nil
        guard schema == 1 || proof == true else {
            throw InstanceError.state("This virtual machine's instance.json is of a schema Andarium does not know.")
        }
        let build = try values.decode(String.self, forKey: .buildIncremental)
        guard build.range(of: "^[1-9][0-9]{0,9}$", options: .regularExpression) != nil else {
            throw InstanceError.state("This virtual machine's instance.json has an invalid build number.")
        }
        self.init(
            dataLayout: try values.decode(Int.self, forKey: .dataLayout),
            releaseID: try values.decode(String.self, forKey: .releaseID),
            buildFingerprint: try values.decode(String.self, forKey: .buildFingerprint),
            buildIncremental: build,
            systemDiskSHA256: try values.decode(String.self, forKey: .systemDiskSHA256),
            createdWith: try values.decode(String.self, forKey: .createdWith),
            lastBooted: try values.decode(String.self, forKey: .lastBooted),
            lastCompleted: try values.decodeIfPresent(Completed.self, forKey: .lastCompleted),
            pendingUpdate: try values.decodeIfPresent(Pending.self, forKey: .pendingUpdate),
            isUnsignedProof: proof == true
        )
    }

    /// The two optional fields are written as `null`, as the schema has them.
    public func encode(to encoder: Encoder) throws {
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(isUnsignedProof ? 2 : 1, forKey: .schema)
        try values.encode(dataLayout, forKey: .dataLayout)
        try values.encode(releaseID, forKey: .releaseID)
        try values.encode(buildFingerprint, forKey: .buildFingerprint)
        try values.encode(buildIncremental, forKey: .buildIncremental)
        try values.encode(systemDiskSHA256, forKey: .systemDiskSHA256)
        try values.encode(createdWith, forKey: .createdWith)
        try values.encode(lastBooted, forKey: .lastBooted)
        if let lastCompleted { try values.encode(lastCompleted, forKey: .lastCompleted) } else { try values.encodeNil(forKey: .lastCompleted) }
        if let pendingUpdate { try values.encode(pendingUpdate, forKey: .pendingUpdate) } else { try values.encodeNil(forKey: .pendingUpdate) }
        if isUnsignedProof { try values.encode(true, forKey: .unsignedProof) }
    }
}

public enum InstanceAction: String, Equatable, Sendable {
    case create, boot, update, migrate, retry, restore, reset
    case refuseDowngrade = "refuse_downgrade"
    case refuseConflict = "refuse_conflict"
    case refusePending = "refuse_pending"

    public var isRefusal: Bool {
        switch self {
        case .refuseDowngrade, .refuseConflict, .refusePending: true
        default: false
        }
    }
}

public struct InstanceFlags: Equatable, Sendable {
    public var retry = false
    public var restoreSnapshot = false
    public var reset = false

    public init(retry: Bool = false, restoreSnapshot: Bool = false, reset: Bool = false) {
        self.retry = retry
        self.restoreSnapshot = restoreSnapshot
        self.reset = reset
    }
}

public struct InstanceDecision: Equatable, Sendable {
    public var action: InstanceAction
    public var reason: String

    public init(_ action: InstanceAction, _ reason: String) {
        self.action = action
        self.reason = reason
    }
}

public enum InstanceRules {
    /// What to do before starting `release` on an instance. Pure: it looks at
    /// nothing else, so every implementation reproduces it from the vectors
    /// (the rule for an unsigned proof, below, is the application's own).
    public static func decide(
        state: InstanceState?, dataDiskExists: Bool, snapshotExists: Bool,
        release: ReleaseIdentity, flags: InstanceFlags = InstanceFlags()
    ) -> InstanceDecision {
        if [flags.retry, flags.restoreSnapshot, flags.reset].filter({ $0 }).count > 1 {
            return InstanceDecision(.refuseConflict, "Retry, restore and reset exclude each other.")
        }
        if flags.reset {
            return InstanceDecision(.reset, "The guest is deleted and created anew.")
        }
        guard let state else {
            if dataDiskExists {
                return InstanceDecision(.refuseConflict, "The virtual machine's data has no record of which system wrote it.")
            }
            return InstanceDecision(.create, "A new guest.")
        }
        // An unsigned proof is for a fresh or isolated instance: it never takes
        // over data another system wrote, and the data it wrote never goes to
        // another system. The instance is erased instead (the way out of every
        // refusal), which Home offers as for any other mismatch, in its words.
        // Two systems that share a release ID are two systems when only one is
        // a proof: the markers are compared as well as the IDs, and before a
        // retry or a restore of a pending update is looked at.
        if release.isUnsignedProof || state.isUnsignedProof,
           release.isUnsignedProof != state.isUnsignedProof || release.releaseID != state.releaseID,
           dataDiskExists || snapshotExists {
            return InstanceDecision(.refuseConflict, StartBlock.mismatch.explanation)
        }
        if let pending = state.pendingUpdate {
            if flags.restoreSnapshot {
                guard snapshotExists else {
                    return InstanceDecision(.refuseConflict, "The update's snapshot is missing.")
                }
                return InstanceDecision(.restore, "The data the update started from is put back.")
            }
            if flags.retry {
                guard release.releaseID == pending.toReleaseID, dataDiskExists else {
                    return InstanceDecision(.refuseConflict, "A retry starts the system whose update did not complete.")
                }
                return InstanceDecision(.retry, "The update starts again, its snapshot kept.")
            }
            if release.releaseID != state.releaseID, BuildNumber.isOlder(state.buildNumber, than: release.buildNumber) {
                guard snapshotExists else {
                    return InstanceDecision(.refuseConflict, "The update's snapshot is missing.")
                }
                if release.dataLayout < pending.from.dataLayout {
                    return InstanceDecision(.refuseDowngrade, "The release's data layout is older than the snapshot's.")
                }
                return InstanceDecision(
                    release.dataLayout == pending.from.dataLayout ? .update : .migrate,
                    "The unfinished update's data is restored before starting the newer system."
                )
            }
            return InstanceDecision(
                .refusePending,
                "The start of \(pending.toReleaseID) never finished: retry it, restore its snapshot, or erase the virtual machine."
            )
        }
        if flags.retry || flags.restoreSnapshot {
            return InstanceDecision(.refuseConflict, "No update is pending.")
        }
        if !dataDiskExists {
            return InstanceDecision(.create, "The guest has no data disk.")
        }
        if release.releaseID == state.releaseID {
            if release.systemDiskSHA256 != state.systemDiskSHA256 {
                return InstanceDecision(.refuseConflict, "One system release names two system disks.")
            }
            return InstanceDecision(.boot, "The system the data was last started with.")
        }
        if BuildNumber.isOlder(release.buildNumber, than: state.buildNumber) {
            return InstanceDecision(
                .refuseDowngrade,
                "Build \(release.buildNumber) is older than build \(state.buildNumber), which the data was started with: update the system, or erase the virtual machine."
            )
        }
        if release.buildNumber == state.buildNumber {
            return InstanceDecision(.refuseConflict, "Two system releases share one build number.")
        }
        if release.dataLayout == state.dataLayout {
            return InstanceDecision(.update, "A newer system on the same data layout.")
        }
        if release.dataLayout > state.dataLayout {
            return InstanceDecision(.migrate, "A newer system on a newer data layout.")
        }
        return InstanceDecision(.refuseDowngrade, "The release's data layout is older than the data's.")
    }
}

/// `clonefile(2)`, the one call every APFS clone of a guest's disk goes
/// through.
enum CloneFile {
    /// A POSIX error to everything that bridges it: its words are `strerror`'s
    /// and its number the `errno`, not a Swift type name and a case index.
    struct Failure: Error, Equatable, CustomNSError {
        /// The `errno` of the refused call.
        let code: Int32

        static var errorDomain: String { NSPOSIXErrorDomain }
        var errorCode: Int { Int(code) }
        var errorUserInfo: [String: Any] { [NSLocalizedDescriptionKey: String(cString: strerror(code))] }
    }

    /// Clones `source` to the absent `destination`.
    static func clone(_ source: URL, to destination: URL) throws {
        guard clonefile(source.path, destination.path, 0) == 0 else { throw Failure(code: errno) }
    }
}

/// An instance's files: `data.img`, `instance.json`, the installation's
/// identity, the snapshot of an update and the text message notice
/// (`TextMessageNotice`), in one directory.
public struct InstanceFiles: Sendable {
    public static let dataDisk = "data.img"
    public static let stateFile = "instance.json"
    public static let snapshotPrefix = "data.img.pre-"
    /// Present from the guest's creation until its first start was seen to
    /// reach `sys.boot_completed`: a first start that ended before it, even one
    /// that ended before the kernel ran, is still the first when the guest
    /// starts again. A guest an earlier version made has none, and is past it.
    public static let firstBootMarker = "first-boot-pending"
    /// The installation's serial number and eth0 address, which the runtime
    /// makes beside the data disk (`darwin/device/block/android_identity.hpp`
    /// owns the file). It belongs to the data disk's life: an update, a resize,
    /// a snapshot restore and the guest's own factory reset leave it, and an
    /// erase deletes it, so the next installation gets values of its own. The
    /// temporary name a write of it uses starts with the same text.
    public static let identityFile = "identity.json"
    /// What the text message bar owes the person about a message whose outcome
    /// was not yet shown (`TextMessageNotice`). It belongs to the data disk's
    /// inbox: an erase and a new data disk delete it, since there is no inbox
    /// left to check.
    public static let textMessageNotice = TextMessageNotice.fileName

    public var directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public var data: URL { directory.appendingPathComponent(Self.dataDisk) }
    public var state: URL { directory.appendingPathComponent(Self.stateFile) }
    public var firstBoot: URL { directory.appendingPathComponent(Self.firstBootMarker) }
    /// Whether the guest's first Android boot has not been seen to complete.
    public var firstBootPending: Bool { FileManager.default.fileExists(atPath: firstBoot.path) }
    public var identity: URL { directory.appendingPathComponent(Self.identityFile) }
    /// The user's snapshots of the data (`SnapshotStore`), which an erase
    /// deletes with the rest.
    public var snapshots: URL { directory.appendingPathComponent(SnapshotStore.directoryName, isDirectory: true) }

    public func snapshot(for releaseID: String) -> URL {
        directory.appendingPathComponent(Self.snapshotPrefix + releaseID)
    }

    public func readState() throws -> InstanceState? {
        guard FileManager.default.fileExists(atPath: state.path) else { return nil }
        do {
            return try JSONDecoder().decode(InstanceState.self, from: Data(contentsOf: state))
        } catch let error as InstanceError {
            throw error
        } catch {
            throw InstanceError.state("This virtual machine's instance.json cannot be read: \(error.localizedDescription)")
        }
    }

    /// Replaces `instance.json` atomically.
    public func write(_ value: InstanceState) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(value).write(to: state, options: .atomic)
    }

    /// The decision's inputs as the files have them.
    public func decide(for release: ReleaseIdentity, flags: InstanceFlags = InstanceFlags()) throws -> InstanceDecision {
        let state = try readState()
        let manager = FileManager.default
        let snapshotExists = state?.pendingUpdate.map { manager.fileExists(atPath: directory.appendingPathComponent($0.snapshot).path) } ?? false
        return InstanceRules.decide(
            state: state, dataDiskExists: manager.fileExists(atPath: data.path), snapshotExists: snapshotExists,
            release: release, flags: flags
        )
    }

    /// An APFS clone renamed over `destination`: instant, it costs only the
    /// blocks that diverge, and a failure leaves `destination` as it was. The
    /// update's snapshot and the user's snapshots (`SnapshotStore`) share it.
    static func clone(_ source: URL, to destination: URL) throws {
        let temporary = destination.appendingPathExtension("tmp")
        try? FileManager.default.removeItem(at: temporary)
        do {
            try CloneFile.clone(source, to: temporary)
        } catch let failure as CloneFile.Failure {
            throw InstanceError.cannotClone(String(cString: strerror(failure.code)))
        }
        guard rename(temporary.path, destination.path) == 0 else {
            let message = String(cString: strerror(errno))
            try? FileManager.default.removeItem(at: temporary)
            throw InstanceError.cannotClone(message)
        }
    }

    public static func timestamp(_ date: Date = Date()) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }

    /// Deletes the data disk, the record, the installation's identity, the
    /// copies an update left, the user's snapshots and the text message
    /// notice, trying each of them; the first failure is thrown once the rest
    /// are done.
    public func removeAll() throws {
        let manager = FileManager.default
        var doomed = [data, state]
        for entry in (try? manager.contentsOfDirectory(atPath: directory.path)) ?? []
        where entry.hasPrefix(Self.snapshotPrefix) || entry.hasPrefix(Self.identityFile) {
            doomed.append(directory.appendingPathComponent(entry))
        }
        doomed.append(snapshots)
        // The record of the walk the guest owes (`PartialSweepLedger`) is the
        // disk's: a guest made after this one starts with a record of its own.
        doomed.append(PartialSweepLedger.url(in: directory))
        doomed.append(firstBoot)
        doomed.append(directory.appendingPathComponent(Self.textMessageNotice))
        var failure: Error?
        for url in doomed where manager.fileExists(atPath: url.path) {
            do { try manager.removeItem(at: url) } catch { failure = failure ?? error }
        }
        if let failure { throw failure }
    }

    /// Carries out `decision` before the guest starts and returns the new
    /// state. Every step leaves the instance in a state `decide` handles: the
    /// snapshot exists before `instance.json` records it.
    @discardableResult
    public func apply(_ decision: InstanceDecision, release: ReleaseIdentity, now: Date = Date()) throws -> InstanceState {
        let manager = FileManager.default
        let when = Self.timestamp(now)
        var action = decision.action
        if action.isRefusal { throw InstanceError.refused(decision.reason) }
        if action == .reset {
            try? removeAll()
            action = .create
        }
        if action == .restore {
            guard let current = try readState(), let pending = current.pendingUpdate else {
                throw InstanceError.refused("No update is pending.")
            }
            let snapshot = directory.appendingPathComponent(pending.snapshot)
            // The data it puts back may hold partial files of earlier runs.
            try PartialSweepLedger.markOwed(in: directory)
            try Self.clone(snapshot, to: data)
            var restored = InstanceState(
                dataLayout: pending.from.dataLayout, releaseID: pending.from.releaseID,
                buildFingerprint: pending.from.buildFingerprint, buildIncremental: pending.from.buildIncremental,
                systemDiskSHA256: pending.from.systemDiskSHA256, createdWith: current.createdWith,
                lastBooted: pending.from.lastBooted
            )
            restored.lastCompleted = current.lastCompleted
            try write(restored)
            try? manager.removeItem(at: snapshot)
            // The data is back and nothing else happens: the next start
            // decides afresh from the restored record, which for the release
            // that was updating is the update again.
            return restored
        }
        if action == .create {
            try manager.createDirectory(at: directory, withIntermediateDirectories: true)
            try? manager.removeItem(at: data)
            // A disk made at the next start is a new installation, whatever
            // identity an earlier one left beside it.
            for entry in (try? manager.contentsOfDirectory(atPath: directory.path)) ?? []
            where entry.hasPrefix(Self.identityFile) {
                try? manager.removeItem(at: directory.appendingPathComponent(entry))
            }
            // And no inbox: a notice about a message of the old disk is no longer true.
            try? manager.removeItem(at: directory.appendingPathComponent(Self.textMessageNotice))
            // A data disk that is made at the next start holds nothing.
            if !manager.fileExists(atPath: data.path) { try PartialSweepLedger.markSettled(in: directory) }
            let created = InstanceState(creating: release, at: when)
            // Before the record: a guest whose record exists has the marker,
            // and one without a record is created again.
            try Data().write(to: firstBoot, options: .atomic)
            try write(created)
            return created
        }
        guard var current = try readState() else { throw InstanceError.state("This virtual machine has no instance.json.") }
        if action == .boot || action == .retry {
            current.lastBooted = when
            try write(current)
            return current
        }
        precondition(action == .update || action == .migrate)
        if current.pendingUpdate != nil {
            try apply(InstanceDecision(.restore, "Restore the data before the unfinished update."), release: release, now: now)
            return try apply(decide(for: release), release: release, now: now)
        }
        if action == .migrate {
            // No data layout other than 1 exists, so there is no migration.
            throw InstanceError.refused("This system needs a newer data layout than Andarium can migrate to; erase the virtual machine.")
        }
        let name = Self.snapshotPrefix + release.releaseID
        try Self.clone(data, to: directory.appendingPathComponent(name))
        let pending = InstanceState.Pending(
            toReleaseID: release.releaseID, snapshot: name,
            from: InstanceState.Origin(
                releaseID: current.releaseID, buildFingerprint: current.buildFingerprint,
                buildIncremental: current.buildIncremental, systemDiskSHA256: current.systemDiskSHA256,
                dataLayout: current.dataLayout, lastBooted: current.lastBooted
            )
        )
        let updated = InstanceState(
            dataLayout: current.dataLayout, releaseID: release.releaseID, buildFingerprint: release.buildFingerprint,
            buildIncremental: String(release.buildNumber), systemDiskSHA256: release.systemDiskSHA256,
            createdWith: current.createdWith, lastBooted: when, lastCompleted: current.lastCompleted,
            pendingUpdate: pending
        )
        try write(updated)
        return updated
    }

    /// Records that a start of `release` reached `sys.boot_completed`. Call it
    /// at the moment the host first sees the guest boot. A pending update
    /// counts as applied from then on: it ends, its snapshot is deleted, and a
    /// newer system updates from the data as it is, with a snapshot of its
    /// own. Restoring the snapshot is for an update that never booted. An
    /// instance with no pending update is left as it is.
    @discardableResult
    public func booted(release: ReleaseIdentity) throws -> InstanceState {
        guard var current = try readState(), current.releaseID == release.releaseID else {
            throw InstanceError.state("The virtual machine was not started with this system.")
        }
        // Android booted: this was the guest's first boot, or it no longer is.
        try? FileManager.default.removeItem(at: firstBoot)
        guard let snapshot = current.pendingUpdate?.snapshot else { return current }
        current.pendingUpdate = nil
        try write(current)
        try? FileManager.default.removeItem(at: directory.appendingPathComponent(snapshot))
        return current
    }

    /// Records that `release` reached `sys.boot_completed` and the guest shut
    /// down cleanly (`lastCompleted`). A pending update that `booted` did not
    /// end is over: its snapshot is deleted.
    @discardableResult
    public func complete(release: ReleaseIdentity, now: Date = Date()) throws -> InstanceState {
        guard var current = try readState(), current.releaseID == release.releaseID else {
            throw InstanceError.state("The virtual machine was not started with this system.")
        }
        try? FileManager.default.removeItem(at: firstBoot)
        let snapshot = current.pendingUpdate?.snapshot
        current.lastCompleted = InstanceState.Completed(
            releaseID: release.releaseID, buildIncremental: String(release.buildNumber), at: Self.timestamp(now)
        )
        current.pendingUpdate = nil
        try write(current)
        if let snapshot { try? FileManager.default.removeItem(at: directory.appendingPathComponent(snapshot)) }
        return current
    }
}
