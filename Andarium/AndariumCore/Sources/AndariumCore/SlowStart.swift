// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the boot screen says that a start is taking long, and what the person
/// has asked of it since.
///
/// A start is timed from the moment the person (or a launch) asks for it, to
/// the boot that reaches Android or Recovery. A guest that restarts itself in
/// place while the start is unfinished is the same start, so a slow loop of
/// restarts reaches the notice before the runtime's guard ends it; a restart
/// of a session that was already running is a new boot, timed from its own
/// beginning. The notice never ends the boot: it offers Stop, and the boot
/// goes on behind it until the person stops it or it completes.
///
/// A guest's first start, and the first start after an update, do work that a
/// later start does not (Android sets itself up, or optimizes its apps): they
/// are given longer before the notice, and the notice does not say the start
/// is slower than usual.
///
/// The type holds readings of one monotonic clock that does not count the time
/// the guest is held (`GuestRunClock`) and nothing else: whoever owns it
/// passes them, so a test needs no wait.
public struct SlowStart: Equatable, Sendable {
    /// How long an ordinary start runs before the boot screen says so.
    public static let threshold: TimeInterval = 180
    /// How long a start does that is the guest's first, or the first after an
    /// update.
    public static let firstStartThreshold: TimeInterval = 480

    /// The reading when the start under way began; nil when none is.
    public private(set) var began: TimeInterval?
    /// The start is one that is long by its nature: a first start, or the
    /// first after an update.
    public private(set) var longByNature = false
    /// The notice is up.
    public private(set) var showing = false
    /// The person asked to stop the start, and the run has not ended yet.
    public private(set) var stopping = false
    /// How many times the guest restarted itself in this start.
    public private(set) var restarts = 0

    public init() {}

    /// How long the start under way runs before the notice.
    public var patience: TimeInterval {
        longByNature ? Self.firstStartThreshold : Self.threshold
    }

    /// Whether a start of `kind` is long by its nature.
    public static func isLongByNature(_ kind: BootKind) -> Bool {
        kind == .firstStart || kind == .upgrade
    }

    /// A start of `kind` begins at `reading`: nothing has been said of it.
    /// `firstBootPending` says that the guest's first Android boot has not been
    /// seen to complete (`InstanceFiles.firstBootPending`): an ordinary start
    /// of such a guest is its first still, though the data disk exists. A
    /// Recovery start is not the boot that completes it.
    public mutating func begin(at reading: TimeInterval, kind: BootKind = .normal, firstBootPending: Bool = false) {
        self = SlowStart()
        began = reading
        longByNature = Self.isLongByNature(kind) || (kind == .normal && firstBootPending)
    }

    /// Android restarted the guest in place. The start continues when it was
    /// unfinished (`startUnfinished`), with its count of restarts; a running
    /// session's restart begins a boot of its own.
    public mutating func guestRestarted(at reading: TimeInterval, startUnfinished: Bool) {
        if startUnfinished, began != nil {
            restarts += 1
        } else {
            begin(at: reading, kind: .restart)
        }
    }

    /// The boot reached Android or Recovery, or the run ended.
    public mutating func finish() {
        self = SlowStart()
    }

    /// Seconds into the start at `reading`; zero before it began or for a
    /// reading before its beginning.
    public func elapsedSeconds(at reading: TimeInterval) -> Int {
        guard let began else { return 0 }
        return Int(max(reading - began, 0))
    }

    /// Looks at the clock. Returns true on the call that puts the notice up,
    /// once for each start.
    @discardableResult
    public mutating func update(at reading: TimeInterval) -> Bool {
        guard let began, !showing, reading - began >= patience else { return false }
        showing = true
        return true
    }

    /// The person chose Stop. Returns false when it was chosen before, which
    /// changes nothing.
    @discardableResult
    public mutating func requestStop() -> Bool {
        guard began != nil, !stopping else { return false }
        stopping = true
        return true
    }
}
