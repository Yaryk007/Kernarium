// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The update notice's screens in the Simulator preview (`-screen <name>`).
/// The version file is a made-up one on `example.com`, and the real rules
/// (`UpdateFile`) read it, so each screen offers what they choose from a file
/// that also lists another application, an older build, and a newer build that
/// needs a later iOS than the mock device has. Nothing is fetched: the read has
/// been made, as it is a day after any first launch.
@MainActor
enum UpdatePreview {
    /// The screens of the design, and what each one stands for.
    static let screens: Set<String> = [
        "update-card",                   // Home, with the card first among its notices
        "update-card-closed",            // the card put away: Home as always
        "update-detail",                 // the card's button, with AltStore and SideStore installed
        "update-detail-altstore",        // only AltStore installed
        "update-detail-steps",           // neither installed: the steps in words
        "update-detail-mac",             // an iPad app on a Mac or Vision Pro: file and page, no tool rows
        "update-info",                   // Info, with the update and the Check for Updates switch
        "update-info-current",           // Info with nothing newer: the switch alone
        "update-info-off",               // Info with the switch off
    ]

    /// The build the preview runs and the one the file offers.
    static let installedBuild = 15
    static let installedLabel = "1.0 (15)"
    /// A device that runs iOS 27: build 17 of the file needs 28.
    static let os = UpdateOSVersion(major: 27)

    static let configuration = UpdateConfiguration(
        address: URL(string: "https://example.com/source.json")!,
        appIdentifier: "com.example.andarium")

    /// An AltStore-format source as its maker would write it, on placeholder
    /// names, with the block of our own beside it.
    nonisolated static let file = Data(
        """
        {
          "name": "Sample Apps", "identifier": "com.example.source",
          "apps": [
            {
              "name": "Another App", "bundleIdentifier": "com.example.another",
              "developerName": "Sample", "localizedDescription": "Never shown.",
              "iconURL": "https://example.com/another.png",
              "versions": [
                { "version": "9.0", "buildVersion": "900", "date": "2026-12-20",
                  "downloadURL": "https://example.com/downloads/another-9.0.ipa", "size": 1000000 }
              ]
            },
            {
              "name": "Andarium", "bundleIdentifier": "com.example.andarium",
              "developerName": "Sample", "localizedDescription": "Never shown.",
              "iconURL": "https://example.com/icon.png",
              "versions": [
                { "version": "1.2", "buildVersion": "17", "date": "2026-12-04", "minOSVersion": "28.0",
                  "downloadURL": "https://example.com/downloads/andarium-1.2.ipa", "size": 692060160 },
                { "version": "1.1", "buildVersion": "16", "date": "2026-11-24", "minOSVersion": "26.0",
                  "downloadURL": "https://example.com/downloads/andarium-1.1.ipa", "size": 641728512 },
                { "version": "1.0", "buildVersion": "15", "date": "2026-11-10", "minOSVersion": "26.0",
                  "downloadURL": "https://example.com/downloads/andarium-1.0.ipa", "size": 633339904 }
              ]
            }
          ],
          "kernariumUpdates": {
            "format": 1,
            "apps": { "com.example.andarium": { "versions": { "16": { "notesURL": "https://example.com/releases/1.1" } } } }
          }
        }
        """.utf8)

    static func handles(_ screen: String) -> Bool { screens.contains(screen) }

    /// The sideloading apps a screen has installed.
    private static func installed(for screen: String) -> Set<String> {
        switch screen {
        case "update-detail-steps", "update-detail-mac": []
        case "update-detail-altstore": ["altstore"]
        default: ["altstore", "sidestore"]
        }
    }

    /// Gives the model the notice of a screen, as a launch of a build with an
    /// address gives it one, with the file already read.
    static func apply(screen: String, to model: AppModel) {
        guard handles(screen) else { return }
        model.system.appVersion = installedLabel
        let reading = UpdateFile.read(
            file, appIdentifier: configuration.appIdentifier, installedBuild: installedBuild, os: os,
            links: configuration.links)
        var record = UpdateRecord(attempted: Date())
        if case .read(let version) = reading, screen != "update-info-current" { record.found = version }
        if screen == "update-card-closed" { record.hiddenBuild = record.found?.build }
        let store = PreviewUpdateStore(record: record, checks: screen != "update-info-off")
        let tools = installed(for: screen)
        model.appUpdate = UpdateNotice(
            configuration: configuration,
            app: UpdateNotice.App(name: "Andarium", installedBuild: installedBuild, installedLabel: installedLabel),
            store: store, fetcher: SampleFetcher(), host: screen == "update-detail-mac" ? .mac : .device, os: os,
            canOpen: { url in url.scheme.map(tools.contains) ?? false },
            open: { _ in })
    }

    /// Opens the sheet a screen shows.
    static func applyDeferred(screen: String, model: AppModel) {
        switch screen {
        case "update-detail", "update-detail-altstore", "update-detail-steps", "update-detail-mac":
            model.sheet = .appUpdate
        case "update-info", "update-info-current", "update-info-off":
            model.sheet = .info
        default:
            break
        }
    }
}

/// The record and the switch of a preview, which a launch does not keep.
@MainActor
private final class PreviewUpdateStore: UpdateStore {
    var record: UpdateRecord
    var checks: Bool

    init(record: UpdateRecord, checks: Bool) {
        self.record = record
        self.checks = checks
    }
}

/// The preview's read: the sample file, whenever one is due.
private struct SampleFetcher: UpdateFetching {
    func fetch(_ address: URL) async -> UpdateFetch { .data(UpdatePreview.file) }
}
