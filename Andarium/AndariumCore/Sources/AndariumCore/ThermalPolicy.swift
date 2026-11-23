// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// iOS's thermal state (`ProcessInfo.ThermalState`), as the policy reads it.
public enum HostThermalState: Int, CaseIterable, Comparable, Sendable {
    case nominal
    case fair
    case serious
    case critical

    public init(_ state: ProcessInfo.ThermalState) {
        switch state {
        case .nominal: self = .nominal
        case .fair: self = .fair
        case .serious: self = .serious
        case .critical: self = .critical
        // A state past critical is hotter than anything the host knows.
        @unknown default: self = .critical
        }
    }

    public static func < (lhs: HostThermalState, rhs: HostThermalState) -> Bool {
        lhs.rawValue < rhs.rawValue
    }

    /// What Android is told while the host is in this state: nothing at
    /// nominal, fair and serious, and critical at critical, where Android
    /// lowers the frame rate to 30. The host never reports Android's light,
    /// moderate, severe, emergency or shutdown.
    public var severity: AndroidThermalSeverity {
        switch self {
        case .nominal, .fair, .serious: .none
        case .critical: .critical
        }
    }
}

/// Android's `ThrottlingSeverity` as the thermal HAL accepts it over the wire:
/// 0 through 4. Android's emergency and shutdown are not here, so no value of
/// this type can make the guest shut down.
public enum AndroidThermalSeverity: UInt8, CaseIterable, Comparable, Sendable {
    case none = 0
    case light = 1
    case moderate = 2
    case severe = 3
    case critical = 4

    public static func < (lhs: AndroidThermalSeverity, rhs: AndroidThermalSeverity) -> Bool {
        lhs.rawValue < rhs.rawValue
    }
}

/// What the guest's thermal HAL is told, from the host's thermal state.
///
/// The severity is the hottest host state in effect at any moment of the last
/// `hold`. It rises at once and falls `hold` after the hotter state ended, so
/// a state that flickers around critical is one steady cap and not a stutter,
/// and a state that comes back restarts the wait. A state that Android is told
/// nothing of (nominal, fair and serious) has no hold. The policy is pure: the
/// caller gives every reading and the clock's reading with it, and asks
/// `nextExpiry` when to look again. The clock is uptime in nanoseconds; the
/// application uses `DispatchTime`'s.
///
/// A policy is one session's: it starts at nominal with no history, so a
/// session that begins at critical is capped from its first reading.
public struct ThermalPolicy: Sendable {
    /// How long after a hotter state ended the severity still counts it.
    public static let hold: UInt64 = 120_000_000_000
    /// The severity at which Android lowers the frame rate. The host reports
    /// critical, which is above it, and no state in between.
    public static let capSeverity = AndroidThermalSeverity.severe

    /// What Android is to be told now.
    public private(set) var severity = AndroidThermalSeverity.none

    private var current = HostThermalState.nominal
    /// When each state that Android is told of last ended, for the states that
    /// may still count.
    private var ended: [HostThermalState: UInt64] = [:]
    private var announced = false

    public init() {}

    /// The host is in `state` at `now`. Returns whether the severity changed.
    @discardableResult
    public mutating func observe(_ state: HostThermalState, at now: UInt64) -> Bool {
        if state != current {
            if current.severity > .none { ended[current] = now }
            ended[state] = nil
            current = state
        }
        return refresh(at: now)
    }

    /// Time has passed with the host in the same state. Returns whether the
    /// severity changed.
    @discardableResult
    public mutating func advance(to now: UInt64) -> Bool {
        refresh(at: now)
    }

    /// When a state that ended stops counting, which may lower the severity;
    /// nil when none counts.
    public var nextExpiry: UInt64? {
        ended.values.min().map { $0 &+ Self.hold }
    }

    /// Whether the cap that the severity now brings is to be announced: true
    /// once in the policy's life, at the first call made while the severity is
    /// at the cap or above. The caller makes the call when the announcement
    /// can be made (the guest can hear the severity), and a call made earlier
    /// or while the severity is lower consumes nothing.
    public mutating func claimAnnouncement() -> Bool {
        guard !announced, severity >= Self.capSeverity else { return false }
        announced = true
        return true
    }

    private mutating func refresh(at now: UInt64) -> Bool {
        ended = ended.filter { $0.value &+ Self.hold > now }
        let hottest = max(current, ended.keys.max() ?? .nominal)
        let next = hottest.severity
        defer { severity = next }
        return next != severity
    }
}
