// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// One release's notes: the build that first carried them and what is new
/// in it. The notes are written per release and bundled with the application;
/// a release with nothing worth announcing has none, and an update to it shows
/// nothing.
public struct WhatsNewRelease: Identifiable, Hashable, Decodable, Sendable {
    /// The build (`CFBundleVersion`) that first carried these notes. The
    /// marketing version can change between releases ("1.0", then "1.1"), so
    /// the build orders them; every archive's is above the one before.
    public let build: Int
    /// The version as Info shows it: "1.1 (16)".
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
        case .sessionSettings: String(localized: "Found in Settings while the virtual machine runs", bundle: .module)
        }
    }

    /// The SF Symbol before the words.
    public var symbol: String {
        switch self {
        case .sessionSettings: "gearshape"
        }
    }
}

/// What a device must offer for an entry to be announced. A
/// field the code does not know, a `device` or `feature` it does not know, a
/// field set to `null`, or no field at all is a mistake in the notes and
/// refuses them: the notes and the code that reads them ship in one build, so
/// such a value is caught where they are bundled and never shown without
/// what it meant. A requirement with more than one field needs all of them.
public struct WhatsNewRequirement: Hashable, Decodable, Sendable {
    /// The device offers a guest more RAM than this, in MiB, at its vCPU
    /// count.
    public var memoryAboveMiB: Int?
    /// The kind of device the feature is for.
    public var device: WhatsNewDevice?
    /// The feature the entry is about, for one that the application shows
    /// only where its own rule says so. The rule is the feature's, never the
    /// notes': it is asked each time the notes are drawn (`isShown`), not at
    /// launch (`isMet`).
    public var feature: WhatsNewFeature?
    /// `true` is an entry for a device that can give the guest a compass (an
    /// iPhone, an iPad), which a Mac and the Simulator cannot. `false` is no
    /// requirement and is refused.
    public var compass: Bool?
    /// `true` is an entry for a device that has a camera to give the guest (an
    /// iPhone, an iPad), which the iPad app on a Mac or on an Apple Vision Pro
    /// does not. `false` is no requirement and is refused.
    public var camera: Bool?

    public init(
        memoryAboveMiB: Int? = nil, device: WhatsNewDevice? = nil, feature: WhatsNewFeature? = nil,
        compass: Bool? = nil, camera: Bool? = nil
    ) {
        self.memoryAboveMiB = memoryAboveMiB
        self.device = device
        self.feature = feature
        self.compass = compass
        self.camera = camera
    }

    private enum CodingKeys: String, CodingKey, CaseIterable {
        case memoryAboveMiB, device, feature, compass, camera
    }

    public init(from decoder: any Decoder) throws {
        try decoder.requireOnly(CodingKeys.self)
        let values = try decoder.container(keyedBy: CodingKeys.self)
        // A key that is there must hold a value: `decodeIfPresent` would take
        // `null` for no key at all, and drop the requirement without a word.
        memoryAboveMiB = try values.contains(.memoryAboveMiB)
            ? values.decode(Int.self, forKey: .memoryAboveMiB) : nil
        device = try values.contains(.device) ? values.decode(WhatsNewDevice.self, forKey: .device) : nil
        feature = try values.contains(.feature) ? values.decode(WhatsNewFeature.self, forKey: .feature) : nil
        compass = try values.contains(.compass) ? values.decode(Bool.self, forKey: .compass) : nil
        camera = try values.contains(.camera) ? values.decode(Bool.self, forKey: .camera) : nil
        guard compass != false else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: decoder.codingPath, debugDescription: "\"compass\" is true or absent"))
        }
        guard camera != false else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: decoder.codingPath, debugDescription: "\"camera\" is true or absent"))
        }
        guard memoryAboveMiB != nil || device != nil || feature != nil || compass != nil || camera != nil else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: decoder.codingPath, debugDescription: "a requirement states nothing"))
        }
    }

    /// Whether the device offers all it asks for that a launch can know: its
    /// memory, kind, compass and camera. The feature is not asked here
    /// (`isShown`).
    public func isMet(by host: HostDevice, vCPUs: Int) -> Bool {
        if let memoryAboveMiB, host.memory.maximumMiB(vCPUs: vCPUs) <= memoryAboveMiB { return false }
        if let device, !device.isMet(by: host) { return false }
        if compass == true, !host.hasCompass { return false }
        if camera == true, !host.hasCamera { return false }
        return true
    }

    /// Whether the feature the entry is about is shown now, by the
    /// application's own rule for it (`shows`); true when it names none.
    public func isShown(by shows: (WhatsNewFeature) -> Bool) -> Bool {
        feature.map(shows) ?? true
    }
}

/// The kind of device an entry is for, written in the notes as `"pad"` or
/// `"haptics"`.
public enum WhatsNewDevice: String, Hashable, Decodable, Sendable {
    /// An iPad: the idiom the host reports, so a Mac running the application
    /// as an iPad app is not one.
    case pad
    /// A device whose Taptic Engine can play the guest's haptics, which the
    /// host reports (`HostDevice.hasHaptics`): an iPhone, not an iPad or a Mac.
    case haptics

    public func isMet(by host: HostDevice) -> Bool {
        switch self {
        case .pad: host.idiom == .pad
        case .haptics: host.hasHaptics
        }
    }
}

/// A feature that the application shows only where its own rule says so,
/// written in the notes as `"addToHomeScreen"`. An entry about one is left out
/// of the ribbon and its notes on a device that does not show the feature, and
/// stays in Info's history as written.
public enum WhatsNewFeature: String, Hashable, Decodable, Sendable, CaseIterable {
    /// Add to Home Screen, in Settings.
    case addToHomeScreen
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

/// The bundled notes: a JSON array of releases, one file for every build.
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
    /// seen offers nothing and keeps the record.
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
