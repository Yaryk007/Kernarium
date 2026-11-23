// SPDX-License-Identifier: GPL-2.0-or-later

/// What a call that writes the guest's storage back reported: the runtime's
/// `sync`, `quiesce` and `quiesce_resident` (`kernarium_runtime_settle`).
/// Both applications log and decide from this one value, so neither prints
/// "settled" for a guest that did not confirm it.
///
/// The guest's own page cache is outside the host's reach: only the guest can
/// write it back, so the storage counts as settled only when the guest said it
/// had (or when no guest ran, which leaves nothing dirty).
public struct GuestSettle: Equatable, Sendable {
    /// What the guest confirmed about the writeback of its storage, in the
    /// order of `KERNARIUM_RUNTIME_GUEST_STORAGE_*`. The zero value claims
    /// nothing.
    public enum Storage: UInt32, Sendable {
        /// Nothing was reported: the call did not fill the report, as when it
        /// failed.
        case notReported = 0
        /// The guest ran sync(2) to its return and said so, in that call: it
        /// answered the sync request, or its suspend reply stated that the
        /// suspend's own sync had returned. A suspend reply that says nothing
        /// of it confirms nothing.
        case confirmed = 1
        /// No guest was running: there was nothing to settle.
        case noGuest = 2
        /// The guest was asked and did not confirm in time, or the host could
        /// not ask because another lifecycle call was busy.
        case unconfirmed = 3
        /// The guest refused the request, or its reply was no confirmation.
        case refused = 4
        /// A guest was running and its management console could not be
        /// reached.
        case unreachable = 5
    }

    /// Whether the call put the guest to sleep, in the order of
    /// `KERNARIUM_RUNTIME_GUEST_SUSPENSION_*`.
    public enum Suspension: UInt32, Sendable {
        case notAsked = 0
        case suspended = 1
        case notSuspended = 2
    }

    /// The host files the call could not flush; negative when no runtime is
    /// loaded or the call failed.
    public let unflushed: Int32
    public let storage: Storage
    public let suspension: Suspension

    public init(unflushed: Int32, storage: Storage, suspension: Suspension) {
        self.unflushed = unflushed
        self.storage = storage
        self.suspension = suspension
    }

    /// From a runtime's own values. One this build does not know (a newer
    /// runtime) claims nothing: the storage is unconfirmed and the guest not
    /// suspended.
    public init(unflushed: Int32, rawStorage: UInt32, rawSuspension: UInt32) {
        self.init(
            unflushed: unflushed,
            storage: Storage(rawValue: rawStorage) ?? .unconfirmed,
            suspension: Suspension(rawValue: rawSuspension) ?? .notSuspended
        )
    }

    /// Whether the guest reported a completed sync pass, or no guest ran.
    /// A task can still write after that pass, so this does not say that no
    /// dirty page remains in the guest.
    public var guestStorageSettled: Bool {
        storage == .confirmed || storage == .noGuest
    }

    /// Whether everything is settled: the guest's storage, and every host file
    /// it can write to.
    public var isSettled: Bool {
        guestStorageSettled && unflushed <= 0
    }

    /// The line an application logs for this result, without its own prefix.
    /// It says "settled" only for a result that is, and names what was not
    /// confirmed otherwise. `kept` follows a settled line, such as the guest's
    /// RAM being kept in the process.
    public func logLine(reason: String, kept: String = "") -> String {
        var line: String
        if unflushed > 0 {
            line = "\(reason) sync left \(unflushed) guest file(s) unflushed"
            if !guestStorageSettled { line += "; " + Self.notConfirmed(storage) }
        } else if storage == .noGuest {
            return "no guest running on \(reason): nothing to settle"
        } else if storage == .confirmed {
            line = "guest storage settled on \(reason)"
            if !kept.isEmpty { line += ", " + kept }
        } else {
            line = "guest storage NOT confirmed on \(reason): " + Self.notConfirmed(storage)
        }
        if suspension == .notSuspended { line += " (guest not suspended)" }
        return line
    }

    private static func notConfirmed(_ storage: Storage) -> String {
        switch storage {
        case .notReported: "the runtime reported nothing"
        case .unconfirmed: "the guest did not confirm its writeback"
        case .refused: "the guest refused the writeback"
        case .unreachable: "the guest's console could not be reached"
        case .confirmed, .noGuest: "confirmed"
        }
    }
}
