// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// One release's notes: the build that first carried them and what is new
/// in it. The notes are written per release and bundled with the application;
/// a release with nothing worth announcing has none, and an update to it shows
/// nothing.
public struct WhatsNewRelease: Identifiable, Hashable, Decodable, Sendable {
    /// The build (`CFBundleVersion`) that first carried these notes. The
    /// TestFlight builds share one marketing version, so the build orders
    /// releases; every archive's is above the one before.
    public let build: Int
    /// The version as Info shows it: "1.0 (9)".
    public let version: String
    /// The day the release came out, "2026-11-12"; left out where the day is
    /// not known when the notes are written.
    public let date: String?
    /// The new features, the most important first. The ribbon names them in
    /// this order.
    public let entries: [WhatsNewEntry]

    public var id: Int { build }

    public init(build: Int, version: String, date: String? = nil, entries: [WhatsNewEntry]) {
        self.build = build
        self.version = version
        self.date = date
        self.entries = entries
    }

    private enum CodingKeys: String, CodingKey, CaseIterable { case build, version, date, entries }

    public init(from decoder: any Decoder) throws {
        try decoder.requireOnly(CodingKeys.self)
        let values = try decoder.container(keyedBy: CodingKeys.self)
        build = try values.decode(Int.self, forKey: .build)
        version = try values.decode(String.self, forKey: .version)
        date = try values.decodeIfPresent(String.self, forKey: .date)
        entries = try values.decode([WhatsNewEntry].self, forKey: .entries)
        if let date, Self.day(of: date) == nil {
            throw DecodingError.dataCorruptedError(
                forKey: .date, in: values, debugDescription: "\(date) is not a day written yyyy-mm-dd")
        }
    }

    /// The release with only the entries `keep` accepts.
    public func keeping(_ keep: (WhatsNewEntry) -> Bool) -> WhatsNewRelease {
        WhatsNewRelease(build: build, version: version, date: date, entries: entries.filter(keep))
    }

    /// The release day, which the reader's calendar then writes, or nil when
    /// the release has no day.
    public var day: Date? {
        date.flatMap { Self.day(of: $0) }
    }

    /// `text` is a day of the Gregorian calendar, whatever the reader's own.
    private static func day(of text: String) -> Date? {
        let calendar = Calendar(identifier: .gregorian)
        let parts = text.split(separator: "-", omittingEmptySubsequences: false).map { Int($0) }
        guard parts.count == 3, text.count == 10, let year = parts[0], let month = parts[1], let day = parts[2]
        else { return nil }
        let components = DateComponents(year: year, month: month, day: day)
        guard let date = calendar.date(from: components),
              calendar.dateComponents([.year, .month, .day], from: date) == components
        else { return nil }
        return date
    }
}

/// One new feature of a release, or a group of fixes. It has `text`, `points`
/// or both.
public struct WhatsNewEntry: Identifiable, Hashable, Decodable, Sendable {
    /// Stable across releases, for VoiceOver and tests: "file-browser".
    public let id: String
    /// The entry's name, which the ribbon lists: "File browser".
    public let title: String
    /// What it does, in a sentence or two, true on every device: an entry
    /// a device cannot use is still listed in Info's history. Nil for an
    /// entry that is only its points.
    public let text: String?
    /// Short points under the title, one line to two each, for an entry that
    /// is a list rather than a sentence: "Fixed a crash when ...". Empty for
    /// an entry with none.
    public let points: [String]
    /// The SF Symbol on the entry's accent tile.
    public let symbol: String
    /// Where the feature lives, for one the person finds outside Home.
    public var place: WhatsNewPlace?
    /// What the device must offer for the ribbon and its notes to show the
    /// entry at all.
    public var requirement: WhatsNewRequirement?

    public init(
        id: String, title: String, text: String? = nil, points: [String] = [], symbol: String,
        place: WhatsNewPlace? = nil, requirement: WhatsNewRequirement? = nil
    ) {
        self.id = id
        self.title = title
        self.text = text
        self.points = points
        self.symbol = symbol
        self.place = place
        self.requirement = requirement
    }

    private enum CodingKeys: String, CodingKey, CaseIterable {
        case id, title, text, points, symbol, place, requirement
    }

    /// A text field is never empty, and the notes say something: `text`,
    /// `points` or both, and a list of points has a point.
    public init(from decoder: any Decoder) throws {
        try decoder.requireOnly(CodingKeys.self)
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decodeText(forKey: .id)
        title = try values.decodeText(forKey: .title)
        text = try values.decodeTextIfPresent(forKey: .text)
        if let listed = try values.decodeIfPresent([String].self, forKey: .points) {
            guard !listed.isEmpty, !listed.contains(where: \.isEmpty) else {
                throw DecodingError.dataCorruptedError(
                    forKey: .points, in: values, debugDescription: "a list of points is empty or has an empty point")
            }
            points = listed
        } else {
            points = []
        }
        guard text != nil || !points.isEmpty else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: decoder.codingPath, debugDescription: "an entry has neither text nor points"))
        }
        symbol = try values.decodeText(forKey: .symbol)
        place = try values.decodeIfPresent(WhatsNewPlace.self, forKey: .place)
        requirement = try values.decodeIfPresent(WhatsNewRequirement.self, forKey: .requirement)
    }
}

extension KeyedDecodingContainer {
    fileprivate func decodeText(forKey key: Key) throws -> String {
        let text = try decode(String.self, forKey: key)
        guard !text.isEmpty else {
            throw DecodingError.dataCorruptedError(
                forKey: key, in: self, debugDescription: "\(key.stringValue) is empty")
        }
        return text
    }

    fileprivate func decodeTextIfPresent(forKey key: Key) throws -> String? {
        contains(key) ? try decodeText(forKey: key) : nil
    }
}

/// Where a feature is found, said under its text.
public enum WhatsNewPlace: String, Hashable, Decodable, Sendable {
    /// Session Settings, which opens from the side keys while the guest runs
    /// in a normal session.
    case sessionSettings

    public var words: String {
        switch self {
        case .sessionSettings: "In Settings while the guest runs"
        }
    }

    /// The SF Symbol before the words.
    public var symbol: String {
        switch self {
        case .sessionSettings: "gearshape"
        }
    }
}

/// What a device must offer for an entry to be announced on it. A field the
/// code does not know, or no field at all, is a mistake in the notes and
/// refuses them.
public struct WhatsNewRequirement: Hashable, Decodable, Sendable {
    /// The device offers a guest more RAM than this, in MiB, at its vCPU
    /// count.
    public var memoryAboveMiB: Int?

    public init(memoryAboveMiB: Int? = nil) {
        self.memoryAboveMiB = memoryAboveMiB
    }

    private enum CodingKeys: String, CodingKey, CaseIterable { case memoryAboveMiB }

    public init(from decoder: any Decoder) throws {
        try decoder.requireOnly(CodingKeys.self)
        let values = try decoder.container(keyedBy: CodingKeys.self)
        memoryAboveMiB = try values.decodeIfPresent(Int.self, forKey: .memoryAboveMiB)
        guard memoryAboveMiB != nil else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: decoder.codingPath, debugDescription: "a requirement states nothing"))
        }
    }

    public func isMet(by host: HostDevice, vCPUs: Int) -> Bool {
        if let memoryAboveMiB, host.memory.maximumMiB(vCPUs: vCPUs) <= memoryAboveMiB { return false }
        return true
    }
}

private struct AnyKey: CodingKey {
    let stringValue: String
    var intValue: Int? { nil }
    init?(stringValue: String) { self.stringValue = stringValue }
    init?(intValue: Int) { nil }
}

extension Decoder {
    /// Refuses a key that `keys` does not name: notes that say more than the
    /// code understands must fail where they are bundled, not be shown
    /// without what they meant.
    fileprivate func requireOnly<Key: CodingKey & CaseIterable>(_ keys: Key.Type) throws {
        let known = Set(Key.allCases.map(\.stringValue))
        let container = try container(keyedBy: AnyKey.self)
        if let unknown = container.allKeys.first(where: { !known.contains($0.stringValue) }) {
            throw DecodingError.dataCorrupted(
                .init(codingPath: codingPath, debugDescription: "\(unknown.stringValue) is not a field of the notes"))
        }
    }
}

/// The bundled notes: a JSON array of releases.
public enum WhatsNewCatalog {
    public enum Problem: Error, Equatable, Sendable {
        case releaseBeforeNotes(build: Int)
        case duplicateBuild(Int)
        case emptyRelease(build: Int)
        case duplicateEntry(String)
    }

    /// The releases in `data`. A release at or before the last build without
    /// a record (`WhatsNewRules.buildBeforeRecords`) has no notes, a release
    /// with no entry is not written, and a build or an entry's id appears
    /// once, so a mistake fails the test that reads the bundled file.
    public static func decode(_ data: Data) throws -> [WhatsNewRelease] {
        let releases = try JSONDecoder().decode([WhatsNewRelease].self, from: data)
        var builds = Set<Int>()
        var ids = Set<String>()
        for release in releases {
            guard release.build > WhatsNewRules.buildBeforeRecords else {
                throw Problem.releaseBeforeNotes(build: release.build)
            }
            guard builds.insert(release.build).inserted else { throw Problem.duplicateBuild(release.build) }
            guard !release.entries.isEmpty else { throw Problem.emptyRelease(build: release.build) }
            for entry in release.entries where !ids.insert(entry.id).inserted {
                throw Problem.duplicateEntry(entry.id)
            }
        }
        return releases
    }
}

/// What Andarium keeps of the notes between launches.
public struct WhatsNewRecord: Codable, Equatable, Sendable {
    /// The newest build whose notes the person has read, put away or been
    /// shown on every launch the ribbon gets.
    public var seenBuild: Int
    /// The build the ribbon is counting launches for, and the count.
    public var ribbonBuild: Int?
    public var ribbonLaunches = 0

    public init(seenBuild: Int, ribbonBuild: Int? = nil, ribbonLaunches: Int = 0) {
        self.seenBuild = seenBuild
        self.ribbonBuild = ribbonBuild
        self.ribbonLaunches = ribbonLaunches
    }
}

/// The record, kept for the install in the defaults database.
public struct WhatsNewStore {
    /// The stored value's key in the defaults database.
    public static let key = "andarium.whatsnew"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    /// The last record kept; nil on an install that has kept none, or whose
    /// record cannot be read.
    public var record: WhatsNewRecord? {
        defaults.data(forKey: Self.key).flatMap { try? JSONDecoder().decode(WhatsNewRecord.self, from: $0) }
    }

    public func keep(_ record: WhatsNewRecord) {
        defaults.set(try? JSONEncoder().encode(record), forKey: Self.key)
    }
}

/// When the ribbon offers notes, and which. Pure, so a host test can hold
/// every case.
public enum WhatsNewRules {
    /// The last build before Andarium kept a record, and the first public
    /// release: no notes are written for it or an earlier build. An install
    /// with a guest and no record was updated from it or from an earlier one;
    /// one with neither is new.
    public static let buildBeforeRecords = 5
    /// Launches the ribbon is offered on before it goes as if put away.
    public static let ribbonLaunchLimit = 3

    public struct Launch: Equatable, Sendable {
        /// The releases the ribbon offers, newest first; empty for none.
        public var pending: [WhatsNewRelease]
        /// What to keep for the next launch.
        public var record: WhatsNewRecord
    }

    /// This launch's ribbon. A new install announces nothing, since all of
    /// it is new. An update offers the notes of every release after the
    /// last one seen, up to this build, newest first, with only the entries
    /// this device can use; it offers nothing when none is left, and nothing
    /// after `ribbonLaunchLimit` launches. A build older than the last one
    /// seen (TestFlight installs any) offers nothing and keeps the record.
    public static func launch(
        releases: [WhatsNewRelease], record: WhatsNewRecord?, currentBuild: Int, hasGuest: Bool,
        offers: (WhatsNewEntry) -> Bool
    ) -> Launch {
        guard let seen = record?.seenBuild ?? (hasGuest ? buildBeforeRecords : nil) else {
            return Launch(pending: [], record: WhatsNewRecord(seenBuild: currentBuild))
        }
        guard seen < currentBuild else {
            return Launch(pending: [], record: record ?? WhatsNewRecord(seenBuild: seen))
        }
        let pending = releases
            .filter { $0.build > seen && $0.build <= currentBuild }
            .map { $0.keeping(offers) }
            .filter { !$0.entries.isEmpty }
            .sorted { $0.build > $1.build }
        let launches = (record?.ribbonBuild == currentBuild ? record?.ribbonLaunches ?? 0 : 0) + 1
        guard !pending.isEmpty, launches <= ribbonLaunchLimit else {
            return Launch(pending: [], record: WhatsNewRecord(seenBuild: currentBuild))
        }
        return Launch(
            pending: pending,
            record: WhatsNewRecord(seenBuild: seen, ribbonBuild: currentBuild, ribbonLaunches: launches)
        )
    }
}
