// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The version file, read for one application.
///
/// The file is an AltStore-format source (the one the sideloading tools read:
/// `apps[].bundleIdentifier` and `apps[].versions[]` with `version`,
/// `buildVersion`, `date`, `downloadURL`, `size`, `minOSVersion` and
/// `maxOSVersion`) with one block of our own, `kernariumUpdates`, which holds
/// the format of the reading and a release page per build. Everything else in
/// it is ignored. It is untrusted: a file that is not readable is no answer, a
/// bad entry costs only itself, and only the version, the build, the day, the
/// size and links that pass `UpdateLinkPolicy` are kept.
public nonisolated enum UpdateFile {
    /// The most a read takes, and the most a file may be.
    public static let largestBytes = 1_048_576
    /// The format of our block this reader knows.
    public static let format = 1

    public enum Reading: Equatable, Sendable {
        /// Not an answer: a file that is not plain JSON in UTF-8, whose top
        /// level is no object, that is over the limit, or whose block is of
        /// another format. The last good answer stays.
        case unreadable
        /// The version to offer, or nil when the file offers none.
        case read(UpdateVersion?)
    }

    /// What the file says about `appIdentifier`, the application's original
    /// bundle identifier: the highest build above `installedBuild` that runs on
    /// `os`, or no version.
    public static func read(
        _ data: Data, appIdentifier: String, installedBuild: Int, os: UpdateOSVersion, links: UpdateLinkPolicy
    ) -> Reading {
        guard data.count <= largestBytes else { return .unreadable }
        // Plain JSON only: Foundation's JSON5 mode traps on a byte that is no
        // UTF-8 where a key is expected, and the body is untrusted. A body
        // that is not UTF-8 is no JSON either, so it is refused first.
        guard String(data: data, encoding: .utf8) != nil else { return .unreadable }
        guard let root = try? JSONDecoder().decode(Root.self, from: data) else { return .unreadable }
        let notes: [String: String]
        switch root.block {
        case .unsupported: return .unreadable
        case .absent: notes = [:]
        case .supported(let block): notes = block.notes(for: appIdentifier)
        }
        var versions: [UpdateVersion] = []
        for app in root.apps.compactMap(\.value) where app.bundleIdentifier == appIdentifier {
            for entry in (app.versions ?? []).compactMap(\.value) {
                guard let version = entry.version(links: links, notes: notes) else { continue }
                versions.append(version)
            }
        }
        return .read(choose(from: versions, installedBuild: installedBuild, os: os))
    }

    /// The version to offer: of those whose build is above the installed one,
    /// whose lowest iOS is at most `os` and whose highest, if any, is at least
    /// it, the one with the highest build (the first of equal builds). Never a
    /// build at or below the installed one: an older application refuses a
    /// virtual machine a newer one has started.
    public static func choose(from versions: [UpdateVersion], installedBuild: Int, os: UpdateOSVersion) -> UpdateVersion? {
        var best: UpdateVersion?
        for version in versions where isOffered(version, installedBuild: installedBuild, os: os) {
            if best == nil || version.build > best!.build { best = version }
        }
        return best
    }

    /// Whether `version` is still an update for this build on this iOS.
    public static func isOffered(_ version: UpdateVersion, installedBuild: Int, os: UpdateOSVersion) -> Bool {
        version.build > installedBuild
            && (version.minimumOS.map { $0 <= os } ?? true)
            && (version.maximumOS.map { os <= $0 } ?? true)
    }

    // MARK: The file's shape

    /// Decodes an element or a value that may be wrong, and keeps nothing of
    /// it when it is: a bad entry skips only itself. It never throws, so an
    /// unkeyed container always moves past it.
    private struct Attempt<Value: Decodable>: Decodable {
        let value: Value?

        init(from decoder: any Decoder) {
            value = try? Value(from: decoder)
        }
    }

    private enum BlockState {
        case absent
        case supported(Block)
        case unsupported
    }

    private struct Root: Decodable {
        let apps: [Attempt<App>]
        let block: BlockState

        enum Keys: String, CodingKey {
            case apps
            case kernariumUpdates
        }

        init(from decoder: any Decoder) throws {
            let container = try decoder.container(keyedBy: Keys.self)
            apps = try container.decodeIfPresent([Attempt<App>].self, forKey: .apps) ?? []
            if container.contains(.kernariumUpdates) {
                if let block = try? container.decode(Block.self, forKey: .kernariumUpdates), block.format == UpdateFile.format {
                    self.block = .supported(block)
                } else {
                    self.block = .unsupported
                }
            } else {
                self.block = .absent
            }
        }
    }

    /// `apps.<identifier>.versions.<build>.notesURL`.
    private struct Block: Decodable {
        let format: Int
        let apps: [String: Attempt<BlockApp>]

        enum Keys: String, CodingKey {
            case format
            case apps
        }

        init(from decoder: any Decoder) throws {
            let container = try decoder.container(keyedBy: Keys.self)
            format = try container.decode(Int.self, forKey: .format)
            // The pages are optional: a block that lists them wrongly loses them, not the file.
            apps = (try? container.decodeIfPresent([String: Attempt<BlockApp>].self, forKey: .apps)).flatMap { $0 } ?? [:]
        }

        /// The release page text of each build of one application.
        func notes(for identifier: String) -> [String: String] {
            (apps[identifier]?.value?.versions ?? [:]).compactMapValues { $0.value?.notesURL }
        }
    }

    private struct BlockApp: Decodable {
        let versions: [String: Attempt<BlockVersion>]?
    }

    private struct BlockVersion: Decodable {
        let notesURL: String?
    }

    private struct App: Decodable {
        let bundleIdentifier: String
        let versions: [Attempt<Entry>]?
    }

    private struct Entry: Decodable {
        let version: String
        let buildVersion: String
        let date: String
        let downloadURL: String
        let size: Int64
        let minOSVersion: String?
        let maxOSVersion: String?

        /// The entry as a version, or nil when any field fails its rule.
        func version(links: UpdateLinkPolicy, notes: [String: String]) -> UpdateVersion? {
            guard UpdateVersion.isWellFormed(version: version),
                let build = UpdateVersion.build(from: buildVersion),
                let released = UpdateDay(date),
                size > 0, size <= UpdateVersion.largestSizeBytes,
                let appFile = links.link(downloadURL)
            else { return nil }
            var minimum: UpdateOSVersion?
            if let minOSVersion {
                guard let parsed = UpdateOSVersion(minOSVersion) else { return nil }
                minimum = parsed
            }
            var maximum: UpdateOSVersion?
            if let maxOSVersion {
                guard let parsed = UpdateOSVersion(maxOSVersion) else { return nil }
                maximum = parsed
            }
            // A release page that fails costs only its row.
            let page = notes[String(build)].flatMap { links.link($0) }
            return UpdateVersion(
                version: version, build: build, released: released, sizeBytes: UInt64(size),
                minimumOS: minimum, maximumOS: maximum, appFile: appFile, notes: page)
        }
    }
}
