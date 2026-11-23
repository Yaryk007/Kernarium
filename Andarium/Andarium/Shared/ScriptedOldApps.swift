// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// A list of old apps that plays, in place of the image's command: the
/// Simulator preview's, and the probe session's of the UI tests. It answers as
/// the command is specified to: a
/// listing, a write that the app accepts, refuses or loses, and an app that
/// is open or not. The app names are made up.
@MainActor
final class ScriptedOldApps: OldAppsSource {
    /// What a write answers.
    var writeAnswer = OldAppWrite.applied
    /// What a listing answers in place of the apps, when set.
    var listingAnswer: OldAppListing?
    /// The packages that have a process.
    var open: Set<String>
    /// How long each answer takes.
    var delay = Duration.milliseconds(120)
    /// How long a listing takes, when it differs from the other answers.
    var listDelay: Duration?
    private(set) var apps: [OldApp]
    private(set) var listings = 0
    /// The limit each listing was asked with.
    private(set) var limits: [Duration?] = []
    private(set) var writes: [(package: String, on: Bool)] = []

    init(apps: [OldApp] = ScriptedOldApps.sampleApps, open: Set<String> = []) {
        self.apps = apps
        self.open = open
    }

    /// Two apps, one of them on.
    static let sampleApps = [
        OldApp(package: "com.example.pixelknights", label: "Pixel Knights", isOn: false, lastUpdate: 1_762_000_000),
        OldApp(package: "com.example.rally", label: "Rally 2011", isOn: true, turnedOn: 1_763_200_000, lastUpdate: 1_700_000_000),
    ]

    /// An app the install just added or updated.
    func install(_ app: OldApp) {
        apps.removeAll { $0.package == app.package }
        apps.append(app)
    }

    /// An app the person uninstalls, or an update that the switch no longer applies to.
    func remove(_ package: String) {
        apps.removeAll { $0.package == package }
    }

    func listOldApps(limit: Duration?) async -> OldAppListing {
        listings += 1
        limits.append(limit)
        // The image answers with the apps as they are when it is asked.
        let answer = listingAnswer ?? .apps(apps.sorted { $0.label.localizedStandardCompare($1.label) == .orderedAscending })
        try? await Task.sleep(for: listDelay ?? delay)
        return answer
    }

    func setOldApp(_ package: String, on: Bool) async -> OldAppWrite {
        writes.append((package, on))
        try? await Task.sleep(for: delay)
        if writeAnswer == .applied, let index = apps.firstIndex(where: { $0.package == package }) {
            apps[index].isOn = on
            apps[index].turnedOn = on ? Int(Date().timeIntervalSince1970) : 0
            open.remove(package)
        }
        return writeAnswer
    }

    func isOldAppOpen(_ package: String) async -> Bool? {
        try? await Task.sleep(for: delay)
        return open.contains(package)
    }
}
