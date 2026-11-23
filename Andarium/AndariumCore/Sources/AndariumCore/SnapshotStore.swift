// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// A snapshot as the store finds it on disk, with what could be read.
struct SnapshotEntry: Sendable {
    var id: SnapshotID
    var record: SnapshotRecord?
    /// The copy of `instance.json`, as bytes and as the record it holds.
    var instanceBytes: Data?
    var state: InstanceState?
    var hasData: Bool

    var isComplete: Bool { record != nil && state != nil && hasData }
}

/// `restore.json`: a restore that has begun and finishes at the next launch
/// if the process ends first.
struct RestoreIntent: Codable, Equatable {
    var schema = 1
    var snapshot: SnapshotID
}

/// The user's snapshots of a guest's data disk, in `Guest/Snapshots/`:
///
/// ```text
/// Snapshots/
///   <uuid>/data.img  instance.json  snapshot.json
///   .new-<uuid>/  .gone-<uuid>/     a create or a delete that did not finish
///   restore.json                    a restore that did not finish
/// ```
///
/// Each operation is a sequence of steps that leave a state `recover` completes
/// or discards, so nothing reads a half-made snapshot. The store never looks
/// at the guest's choices, which `TwoDiskStorage` settles through the
/// closure of a restore, and it does not know whether the guest runs: the
/// application's state decides that (`SnapshotRules.gate`).
struct SnapshotStore: Sendable {
    static let directoryName = "Snapshots"
    static let restoreFile = "restore.json"
    static let metadataFile = "snapshot.json"
    static let stagingPrefix = ".new-"
    static let removalPrefix = ".gone-"

    var locations: GuestLocations
    var release: ReleaseIdentity
    /// The file calls; a test replaces them (`SnapshotFileOperations`).
    var io = SnapshotFileOperations.live

    private var files: InstanceFiles { InstanceFiles(directory: locations.directory) }
    var root: URL { files.snapshots }
    private var restoreURL: URL { root.appendingPathComponent(Self.restoreFile) }

    func directory(_ id: SnapshotID) -> URL {
        root.appendingPathComponent(id.rawValue, isDirectory: true)
    }

    /// A write that is on the volume when it returns: the atomic replacement,
    /// then the file and its directory flushed. A failure to flush the
    /// directory is not one, as the write itself is in place.
    private func durablyWrite(_ data: Data, to url: URL) throws {
        try io.write(data, url)
        try io.flush(url)
        try? io.flush(url.deletingLastPathComponent())
    }

    /// Keeps `url` out of iCloud and computer backups.
    static func excludeFromBackup(_ url: URL) {
        var target = url
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        try? target.setResourceValues(excluded)
    }

    /// An error that is a refusal as a value: no room, or a volume that cannot
    /// clone. Any other error stays what it is.
    private func translated(_ error: Error) -> Error {
        if let refusal = refusal(for: error) { return refusal }
        return error
    }

    func refusal(for error: Error) -> SnapshotRefusal? {
        if let failure = error as? CloneFile.Failure {
            switch failure.code {
            case ENOTSUP: return .volumeCannotClone
            case ENOSPC, EDQUOT: return .notEnoughSpace
            default: return nil
            }
        }
        if let cocoa = error as? CocoaError, cocoa.code == .fileWriteOutOfSpace { return .notEnoughSpace }
        if let posix = error as? POSIXError, posix.code == .ENOSPC || posix.code == .EDQUOT { return .notEnoughSpace }
        return nil
    }

    // MARK: Reading

    func entry(_ id: SnapshotID) -> SnapshotEntry? {
        let manager = FileManager.default
        let folder = directory(id)
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: folder.path, isDirectory: &isDirectory), isDirectory.boolValue else { return nil }
        let record = (try? Data(contentsOf: folder.appendingPathComponent(Self.metadataFile)))
            .flatMap { try? JSONDecoder().decode(SnapshotRecord.self, from: $0) }
        let bytes = try? Data(contentsOf: folder.appendingPathComponent(InstanceFiles.stateFile))
        let state = bytes.flatMap { try? JSONDecoder().decode(InstanceState.self, from: $0) }
        let data = manager.fileExists(atPath: folder.appendingPathComponent(InstanceFiles.dataDisk).path)
        return SnapshotEntry(id: id, record: record, instanceBytes: bytes, state: state, hasData: data)
    }

    /// Every snapshot, complete or not, newest first; a damaged one (no
    /// readable record) is last.
    func entries() -> [SnapshotEntry] {
        let names = (try? io.list(root)) ?? []
        let found = names.compactMap { name in SnapshotID(name).flatMap { entry($0) } }
        return found.sorted {
            switch ($0.record?.created, $1.record?.created) {
            case let (a?, b?): a != b ? a > b : $0.id.rawValue < $1.id.rawValue
            case (nil, nil): $0.id.rawValue < $1.id.rawValue
            case (_?, nil): true
            case (nil, _?): false
            }
        }
    }

    /// The data disk of a snapshot, a file for the measures.
    func dataURL(_ id: SnapshotID) -> URL {
        directory(id).appendingPathComponent(InstanceFiles.dataDisk)
    }

    /// The small files of a snapshot.
    func smallFiles(_ id: SnapshotID) -> [URL] {
        [InstanceFiles.stateFile, Self.metadataFile].map { directory(id).appendingPathComponent($0) }
    }

    private enum Intent {
        case none
        case readable(SnapshotID)
        case unreadable
    }

    private func intent() -> Intent {
        guard FileManager.default.fileExists(atPath: restoreURL.path) else { return .none }
        guard let data = try? Data(contentsOf: restoreURL),
            let intent = try? JSONDecoder().decode(RestoreIntent.self, from: data), intent.schema == 1
        else { return .unreadable }
        return .readable(intent.snapshot)
    }

    /// Whether a restore began and did not finish.
    var restoreUnderWay: Bool {
        if case .none = intent() { false } else { true }
    }

    /// The restore that began and did not finish, or nil.
    func unfinishedRestore() -> SnapshotUnfinishedRestore? {
        switch intent() {
        case .none: nil
        case .unreadable: SnapshotUnfinishedRestore()
        case .readable(let id): SnapshotUnfinishedRestore(snapshot: id)
        }
    }

    /// The record a restore that began and did not finish puts in place when
    /// the next start or launch carries it through: the snapshot's copy of
    /// `instance.json`. Nil where nothing is carried through: no restore, an
    /// intent that cannot be read, or a snapshot that is no longer whole
    /// (`recover` forgets such a restore and leaves the files as they are).
    func recordOfUnfinishedRestore() -> InstanceState? {
        guard case .readable(let id) = intent(), let found = entry(id), found.isComplete else { return nil }
        return found.state
    }

    // MARK: Create

    private struct CreateInputs {
        var instanceBytes: Data
        var diskBytes: Int64
    }

    /// What Create needs from the guest's files, or the reason it refuses.
    private func checkCreate() throws -> CreateInputs {
        if restoreUnderWay { throw SnapshotRefusal.restoreUnfinished }
        var details = stat()
        let dataExists = lstat(files.data.path, &details) == 0 && (details.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG)
        // The volume's own answer: no state of the guest makes a clone work on one that cannot.
        if dataExists, io.volumeClones(locations.directory) == false { throw SnapshotRefusal.volumeCannotClone }
        var state: InstanceState?
        var bytes: Data?
        if FileManager.default.fileExists(atPath: files.state.path) {
            guard let read = try? Data(contentsOf: files.state),
                let decoded = try? JSONDecoder().decode(InstanceState.self, from: read)
            else { throw SnapshotRefusal.unreadableRecord }
            bytes = read
            state = decoded
        }
        if let refusal = SnapshotRules.createRefusal(state: state, dataDiskExists: dataExists, release: release) {
            throw refusal
        }
        guard let bytes else { throw SnapshotRefusal.mismatch }
        return CreateInputs(instanceBytes: bytes, diskBytes: Int64(details.st_size))
    }

    /// Why Create refuses on the guest's files now, or nil.
    func createRefusal() -> SnapshotRefusal? {
        do {
            _ = try checkCreate()
            return nil
        } catch let refusal as SnapshotRefusal {
            return refusal
        } catch {
            return .unreadableRecord
        }
    }

    /// A snapshot of the data as it is: a clone of `data.img` and a copy of
    /// `instance.json`, in a directory that takes its name only when it is
    /// whole.
    @discardableResult
    func create(name rawName: String, now: Date = Date()) throws -> SnapshotID {
        guard let name = SnapshotRules.normalizedName(rawName) else { throw SnapshotRefusal.invalidName }
        let inputs = try checkCreate()
        let state = try JSONDecoder().decode(InstanceState.self, from: inputs.instanceBytes)
        let id = SnapshotID()
        let staging = root.appendingPathComponent(Self.stagingPrefix + id.rawValue, isDirectory: true)
        let gib = Int64(Limits.bytesPerGiB)
        let record = SnapshotRecord(
            name: name, created: now, releaseID: state.releaseID, buildIncremental: state.buildIncremental,
            diskGiB: Int((inputs.diskBytes + gib - 1) / gib)
        )
        do {
            let rootIsNew = !FileManager.default.fileExists(atPath: root.path)
            try io.createDirectory(staging)
            Self.excludeFromBackup(root)
            Self.excludeFromBackup(staging)
            if rootIsNew { try? io.flush(locations.directory) }

            let clone = staging.appendingPathComponent(InstanceFiles.dataDisk)
            try io.clone(files.data, clone)
            Self.excludeFromBackup(clone)
            try io.flush(clone)

            let copy = staging.appendingPathComponent(InstanceFiles.stateFile)
            try durablyWrite(inputs.instanceBytes, to: copy)
            Self.excludeFromBackup(copy)

            let metadata = staging.appendingPathComponent(Self.metadataFile)
            try durablyWrite(Self.encoded(record), to: metadata)
            Self.excludeFromBackup(metadata)
            try? io.flush(staging)

            try io.rename(staging, directory(id))
            try? io.flush(root)
        } catch {
            try? io.remove(staging)
            throw translated(error)
        }
        return id
    }

    static func encoded(_ record: SnapshotRecord) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        return try encoder.encode(record)
    }

    // MARK: Delete and rename

    /// Deletes a snapshot: set aside by one rename, which is the commit, and
    /// then removed. A failure to remove it leaves `.gone-<id>` for the next
    /// launch and is not the person's failure. The snapshot a restore under
    /// way names stays, as the restore needs it; every other can go, which is
    /// how room is made for a restore that ran out of it.
    func delete(_ id: SnapshotID) throws {
        switch intent() {
        case .none:
            break
        case .unreadable:
            throw SnapshotRefusal.restoreUnfinished
        case .readable(let restoring):
            if restoring == id { throw SnapshotRefusal.restoreUnfinished }
        }
        guard FileManager.default.fileExists(atPath: directory(id).path) else { throw SnapshotRefusal.missing }
        let gone = root.appendingPathComponent(Self.removalPrefix + id.rawValue, isDirectory: true)
        try? io.remove(gone)
        try io.rename(directory(id), gone)
        try? io.flush(root)
        try? io.remove(gone)
    }

    /// Renames a snapshot: the write of its record is the commit, and the
    /// flushes after it are best effort.
    func rename(_ id: SnapshotID, to rawName: String) throws {
        guard let name = SnapshotRules.normalizedName(rawName) else { throw SnapshotRefusal.invalidName }
        guard let found = entry(id) else { throw SnapshotRefusal.missing }
        guard var record = found.record else { throw SnapshotRefusal.incomplete }
        record.name = name
        let metadata = directory(id).appendingPathComponent(Self.metadataFile)
        do {
            try io.write(Self.encoded(record), metadata)
        } catch {
            throw translated(error)
        }
        Self.excludeFromBackup(metadata)
        try? io.flush(metadata)
        try? io.flush(directory(id))
    }

    // MARK: Restore

    private var dataTemporary: URL { files.data.appendingPathExtension("tmp") }

    /// Puts the snapshot's data and record in place of the guest's. `settle`
    /// is the step that touches the guest's choices (the capacity, the
    /// backup policy); it runs once the files are in place and may run again
    /// after a crash. The snapshot stays, and the state it replaces is gone.
    ///
    /// The rename of the clone over `data.img` is the commit point. An error
    /// before it, in writing the intent too, forgets the restore and changes
    /// nothing; an error after it leaves the intent for roll-forward and is
    /// thrown as `SnapshotRestoreUnfinished`.
    func restore(_ id: SnapshotID, settle: (SnapshotRecord) throws -> Void) throws {
        if restoreUnderWay { throw SnapshotRefusal.restoreUnfinished }
        guard let found = entry(id) else { throw SnapshotRefusal.missing }
        guard found.isComplete, let state = found.state, let record = found.record else { throw SnapshotRefusal.incomplete }
        if case .refused(let reason) = SnapshotRules.restorePlan(of: state, diskGiB: record.diskGiB, release: release) {
            throw reason
        }
        do {
            try io.write(JSONEncoder().encode(RestoreIntent(snapshot: id)), restoreURL)
            try io.flush(restoreURL)
            try io.flush(root)
            Self.excludeFromBackup(restoreURL)
            try stageData(of: id)
            // The snapshot's data may hold partial files of earlier runs: the
            // record of the walk the guest owes says so before it is in place.
            try PartialSweepLedger.markOwed(in: locations.directory)
            try io.rename(dataTemporary, files.data)
        } catch {
            try? io.remove(dataTemporary)
            try? io.remove(restoreURL)
            throw translated(error)
        }
        do {
            try io.flush(files.data)
            try? io.flush(locations.directory)
            try finishRestore(found, settle: settle)
        } catch {
            throw SnapshotRestoreUnfinished(cause: error)
        }
    }

    /// The clone of the snapshot's data beside `data.img`, flushed.
    private func stageData(of id: SnapshotID) throws {
        try? io.remove(dataTemporary)
        try io.clone(dataURL(id), dataTemporary)
        try io.flush(dataTemporary)
    }

    /// The clone renamed over `data.img`, for a roll-forward.
    private func replaceData(of id: SnapshotID) throws {
        do {
            try stageData(of: id)
            try PartialSweepLedger.markOwed(in: locations.directory)
            try io.rename(dataTemporary, files.data)
        } catch {
            try? io.remove(dataTemporary)
            throw error
        }
        try io.flush(files.data)
        try? io.flush(locations.directory)
    }

    /// What follows the data: the record, the update copies, the guest's
    /// choices and the end of the intent. Each can run again with the same
    /// result while the snapshot is whole, because it is only read.
    private func finishRestore(_ found: SnapshotEntry, settle: (SnapshotRecord) throws -> Void) throws {
        guard let bytes = found.instanceBytes, let record = found.record else { throw SnapshotRefusal.incomplete }
        try durablyWrite(bytes, to: files.state)
        // The replaced state's update copy has no record naming it now.
        for name in try io.list(locations.directory) where name.hasPrefix(InstanceFiles.snapshotPrefix) {
            try io.remove(locations.directory.appendingPathComponent(name))
        }
        try settle(record)
        try io.remove(restoreURL)
        try? io.flush(root)
    }

    // MARK: Launch

    /// Completes what a crash left, before the guest is read: the leftovers of
    /// a create or a delete (and a clone's temporary file) are removed first,
    /// whatever follows, as they hold space; a restore that began is then
    /// carried through (the snapshot is intact, so every step can run again).
    /// A restore whose snapshot is gone or damaged cannot be carried on and is
    /// forgotten. A roll-forward that fails throws `SnapshotRestoreUnfinished`
    /// and leaves the intent.
    func recover(settle: (SnapshotRecord) throws -> Void) throws {
        try? io.remove(dataTemporary)
        for name in (try? io.list(root)) ?? []
        where name.hasPrefix(Self.stagingPrefix) || name.hasPrefix(Self.removalPrefix) {
            try? io.remove(root.appendingPathComponent(name))
        }
        switch intent() {
        case .none:
            break
        case .unreadable:
            try? io.remove(restoreURL)
        case .readable(let id):
            guard let found = entry(id), found.isComplete else {
                try? io.remove(restoreURL)
                return
            }
            do {
                try replaceData(of: id)
                try finishRestore(found, settle: settle)
            } catch {
                throw SnapshotRestoreUnfinished(cause: error)
            }
        }
    }
}

/// A restore that has put the snapshot's data in place and has not finished:
/// its intent stays, and the next launch or start carries it through. `cause`
/// is the error that stopped it.
public struct SnapshotRestoreUnfinished: Error, LocalizedError {
    public var cause: any Error

    public var errorDescription: String? { cause.localizedDescription }
}
