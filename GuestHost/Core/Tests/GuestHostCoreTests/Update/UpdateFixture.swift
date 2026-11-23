// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

@testable import GuestHostCore

/// A version file for the update notice's tests: an AltStore-format source on
/// placeholder names and `https://example.com/`, the only address a test uses.
enum UpdateFixture {
    static let appIdentifier = "com.example.andarium"
    static let source = URL(string: "https://example.com/source.json")!
    static let links = UpdateLinkPolicy(sourceHost: "example.com")
    static let os = UpdateOSVersion(major: 26, minor: 0)

    /// One entry of an app's `versions`, as the owner writes it.
    static func entry(
        version: String = "1.1", build: String = "16", date: String = "2026-11-20",
        url: String = "https://example.com/releases/andarium-1.1-16.ipa", size: String = "1390000000",
        minOS: String? = "\"26.0\"", extra: String = ""
    ) -> String {
        var fields = [
            "\"version\": \"\(version)\"", "\"buildVersion\": \(build.hasPrefix("#") ? String(build.dropFirst()) : "\"\(build)\"")",
            "\"date\": \"\(date)\"", "\"downloadURL\": \"\(url)\"", "\"size\": \(size)",
        ]
        if let minOS { fields.append("\"minOSVersion\": \(minOS)") }
        if !extra.isEmpty { fields.append(extra) }
        return "{ " + fields.joined(separator: ", ") + " }"
    }

    /// An app of the source.
    static func app(_ identifier: String = appIdentifier, versions: [String], extra: String = "") -> String {
        """
        { "name": "Placeholder", "bundleIdentifier": "\(identifier)", "developerName": "Example Developer",
          "localizedDescription": "Never shown.", "iconURL": "https://example.com/icon.png",
          "versions": [\(versions.joined(separator: ", "))]\(extra.isEmpty ? "" : ", " + extra) }
        """
    }

    /// A whole file: the apps and, if given, the block of our own.
    static func file(apps: [String], block: String? = nil, extra: String = "") -> Data {
        var parts = [
            "\"name\": \"Placeholder Apps\"", "\"identifier\": \"com.example.source\"",
            "\"apps\": [\(apps.joined(separator: ", "))]",
        ]
        if let block { parts.append("\"kernariumUpdates\": \(block)") }
        if !extra.isEmpty { parts.append(extra) }
        return Data(("{ " + parts.joined(separator: ", ") + " }").utf8)
    }

    static func read(
        _ data: Data, installed: Int = 15, os: UpdateOSVersion = UpdateFixture.os,
        links: UpdateLinkPolicy = UpdateFixture.links
    ) -> UpdateFile.Reading {
        UpdateFile.read(data, appIdentifier: appIdentifier, installedBuild: installed, os: os, links: links)
    }

    /// The build that a reading offers, or nil for none or for no answer.
    static func offered(_ reading: UpdateFile.Reading) -> Int? {
        if case .read(let version) = reading { return version?.build }
        return nil
    }
}
