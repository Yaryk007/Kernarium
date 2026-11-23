// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import UIKit

/// The release notes and the stored record of the preview's What's New
/// screens. The notes of builds 16, 12, 11 and 9 are the bundled ones; those
/// of builds 5 and 4 are made up, to stand in for releases a person skipped
/// (the application bundles notes from build 9 on). A device keeps the record
/// in its defaults.
enum WhatsNewPreview {
    /// The build the preview's What's New screens run.
    static let currentBuild = 16
    static let appVersion = "1.1 (16)"

    static let releases: [WhatsNewRelease] = WhatsNewCatalog.load() + [
        WhatsNewRelease(build: 5, version: "1.0 (5)", date: "2026-11-09", entries: [
            WhatsNewEntry(
                id: "stats-vram", title: "Graphics memory in Stats",
                text: "Stats shows how much graphics memory Android is using, beside CPU and memory.",
                symbol: "chart.bar.fill", place: .sessionSettings
            ),
            WhatsNewEntry(
                id: "audio-delay", title: "Shorter sound delay",
                text: "Sound from Android apps plays sooner after they make it.",
                symbol: "speaker.wave.2.fill"
            ),
            WhatsNewEntry(
                id: "session-end", title: "Why Android stopped",
                text: "When Android stops without shutting down, Home says why.",
                symbol: "exclamationmark.bubble.fill"
            ),
        ]),
        WhatsNewRelease(build: 4, version: "1.0 (4)", date: "2026-11-08", entries: [
            WhatsNewEntry(
                id: "low-space-pause", title: "Pause when storage is full",
                text: "When the device runs out of space, Android pauses instead of failing, and resumes when there is room again.",
                symbol: "externaldrive.fill"
            ),
        ]),
    ]

    /// The screens of the design, and what each one stands for.
    static let screens: Set<String> = [
        "whatsnew",                // first launch after the update to build 16
        "whatsnew-open",           // its notes, opened from the ribbon
        "whatsnew-skipped",        // first launch after skipping builds 4 to 15
        "whatsnew-skipped-open",   // the notes of all six
        "whatsnew-update",         // the update also carries a new Android release
        "whatsnew-limited",        // a device that offers no more than 4 GiB
        "whatsnew-dismissed",      // the ribbon put away: Home as always
        "whatsnew-fresh",          // a new install: no guest, nothing announced
        "whatsnew-info",           // Info, with What's New
        "whatsnew-history",        // Info's What's New page
        "whatsnew-unsupported",    // a 60 Hz screen on iOS 26: no Add to Home Screen entry
        "whatsnew-unsupported-open", // its notes, opened from the ribbon
    ]

    /// The screens of a device on iOS 26 with a 60 Hz screen, which has no Add to
    /// Home Screen (its 120 Hz entry stays). Every other What's New screen is a
    /// ProMotion screen on iOS 27.
    private static let unsupportedScreens: Set<String> = ["whatsnew-unsupported", "whatsnew-unsupported-open"]

    /// The record a screen's launch finds, and whether the install has a
    /// guest: a record of build 12 is the update to 16, of build 5 the update
    /// from the first public release, of build 3 an update that skipped
    /// five releases.
    private static func stored(for screen: String) -> WhatsNewRecord? {
        switch screen {
        case "whatsnew-skipped", "whatsnew-skipped-open": WhatsNewRecord(seenBuild: 3)
        case "whatsnew-limited": WhatsNewRecord(seenBuild: 5)
        case "whatsnew-dismissed", "whatsnew-info", "whatsnew-history":
            WhatsNewRecord(seenBuild: currentBuild)
        case "whatsnew-fresh": nil
        default: WhatsNewRecord(seenBuild: 12)
        }
    }

    /// A device that offers a guest more than 4 GiB, so the memory entry is
    /// announced: an iPad of 16 GB or an iPhone of 12 GB, with made-up
    /// bounds, and the idiom's haptics: an iPhone has them, an iPad does not.
    /// "whatsnew-limited" keeps the idiom's usual mock, which stops at 4 GiB.
    static func host(for screen: String) -> HostDevice? {
        guard screens.contains(screen), screen != "whatsnew-limited" else { return nil }
        let pad = UIDevice.current.userInterfaceIdiom == .pad
        let limitMiB = pad ? 10_240 : 6_144
        let count = pad ? 8 : 6
        return HostDevice(
            name: pad ? "iPad" : "iPhone", processorCount: count,
            memory: MemoryPolicy(
                maximumsMiB: Array(repeating: pad ? 7_168 : 4_608, count: count),
                defaultsMiB: Array(repeating: pad ? 4_096 : 3_072, count: count),
                graphicsMiB: { memory, _ in max(limitMiB - 900 - memory, 352) }
            ),
            freeStorageBytes: UInt64((pad ? 118.3 : 41.6) * Double(Limits.bytesPerGiB)),
            idiom: pad ? .pad : .phone, hasHaptics: !pad, hasCompass: true, hasCamera: true
        )
    }

    /// The releases a screen plays. A launch with `-whatsNewFrom N` leaves out
    /// the first N entries of the newest release, so that a capture of the
    /// sheet can show the entries its first view cuts off, whole.
    private static func shownReleases() -> [WhatsNewRelease] {
        guard let skipped = UserDefaults.standard.string(forKey: "whatsNewFrom").flatMap({ Int($0) }),
              skipped > 0
        else { return releases }
        return releases.map { release in
            guard release.build == currentBuild else { return release }
            var seen = 0
            return release.keeping { _ in
                seen += 1
                return seen > skipped
            }
        }
    }

    /// Decides the screen's ribbon from its record, as a launch does.
    static func apply(screen: String, to model: AppModel) {
        guard screens.contains(screen) else { return }
        model.system.appVersion = appVersion
        if screen == "whatsnew-fresh" { model.guest = nil }
        if screen == "whatsnew-update" {
            model.plan = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        }
        // What the system offers decides the entry about Add to Home Screen;
        // the screen's rate shows on Home's 120 Hz switch.
        let shown = !unsupportedScreens.contains(screen)
        model.statedMaximumFramesPerSecond = shown ? 120 : 60
        model.screenDidChange(nativeScale: model.screenNativeScale, maximumFramesPerSecond: shown ? 120 : 60)
        model.playedPadAppHost = IPadAppHost(
            isiOSAppOnMac: false, isiOSAppOnVision: false, hasAppTilesInShortcuts: shown)
        let host = model.host
        let vCPUs = model.guest?.cores ?? host.processorCount
        model.whatsNew.launch(
            releases: shownReleases(), record: stored(for: screen),
            currentBuild: currentBuild, hasGuest: model.guest != nil,
            offers: { $0.requirement?.isMet(by: host, vCPUs: vCPUs) ?? true }
        )
    }
}
