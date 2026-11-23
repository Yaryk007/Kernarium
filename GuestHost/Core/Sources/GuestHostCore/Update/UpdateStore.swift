// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the update notice keeps between launches.
public struct UpdateRecord: Equatable, Codable, Sendable {
    /// When the last read was made, whether it gave an answer or not: a read
    /// that failed waits for the next day like one that did not.
    public var attempted: Date?
    /// What the last read that gave an answer found: the version to offer, or
    /// none. A read that gives no answer changes nothing.
    public var found: UpdateVersion?
    /// The build whose card the person put away or whose Skip This Version
    /// they chose. Only that build is hidden: the next one brings the card back.
    public var hiddenBuild: Int?

    public init(attempted: Date? = nil, found: UpdateVersion? = nil, hiddenBuild: Int? = nil) {
        self.attempted = attempted
        self.found = found
        self.hiddenBuild = hiddenBuild
    }

    /// Each part is read on its own, so that one that is not valid (a version
    /// the rules of the file would not keep) is dropped and costs only itself.
    public init(from decoder: any Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        attempted = (try? container.decodeIfPresent(Date.self, forKey: .attempted)) ?? nil
        found = (try? container.decodeIfPresent(UpdateVersion.self, forKey: .found)) ?? nil
        hiddenBuild = (try? container.decodeIfPresent(Int.self, forKey: .hiddenBuild)) ?? nil
    }

    private enum CodingKeys: String, CodingKey {
        case attempted, found, hiddenBuild
    }
}

/// Where the record and the switch are kept.
@MainActor
public protocol UpdateStore: AnyObject {
    var record: UpdateRecord { get set }
    /// Whether the application checks for updates: on until the person turns it off.
    var checks: Bool { get set }
}

/// The record in `UserDefaults`.
@MainActor
public final class DefaultsUpdateStore: UpdateStore {
    public static let recordKey = "UpdateNotice.record"
    public static let checksKey = "UpdateNotice.checks"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// An unreadable record is an empty one.
    public var record: UpdateRecord {
        get {
            defaults.data(forKey: Self.recordKey).flatMap { try? JSONDecoder().decode(UpdateRecord.self, from: $0) }
                ?? UpdateRecord()
        }
        set {
            defaults.set(try? JSONEncoder().encode(newValue), forKey: Self.recordKey)
        }
    }

    public var checks: Bool {
        get { defaults.object(forKey: Self.checksKey) as? Bool ?? true }
        set { defaults.set(newValue, forKey: Self.checksKey) }
    }
}

/// When a read is due: at most one in 24 hours.
public nonisolated enum UpdateSchedule {
    public static let interval: TimeInterval = 24 * 60 * 60

    /// A record with no read is due; so is one stamped in the future, which a
    /// clock set back leaves behind.
    public static func isDue(attempted: Date?, now: Date) -> Bool {
        guard let attempted else { return true }
        let elapsed = now.timeIntervalSince(attempted)
        return elapsed < 0 || elapsed >= interval
    }
}
