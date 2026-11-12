// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation

/// The notes the screens show: every release's, for Info, and what the
/// ribbon above Start offers this launch. The notes, the rules and the
/// record are `AndariumCore`'s (`WhatsNewRules`).
@Observable
final class WhatsNewModel {
    /// Every release's notes, newest first. Info lists them; without any,
    /// Info has no What's New row.
    private(set) var releases: [WhatsNewRelease] = []
    /// What the ribbon offers, newest first; empty when it is not shown.
    private(set) var pending: [WhatsNewRelease] = []
    /// The build this launch runs.
    private(set) var currentBuild = 0
    /// Writes the record for the next launch: the device keeps it in its
    /// defaults. Nil keeps nothing, as in a harness launch and the preview.
    @ObservationIgnored var keep: ((WhatsNewRecord) -> Void)?

    var showsRibbon: Bool { !pending.isEmpty }

    /// The pending entries, newest release first, in each release's order.
    var pendingEntries: [WhatsNewEntry] { pending.flatMap(\.entries) }

    /// Lists the notes in Info and announces nothing: a launch that is not
    /// the person's own shows no ribbon and keeps no record.
    func list(_ releases: [WhatsNewRelease]) {
        self.releases = releases.sorted { $0.build > $1.build }
    }

    /// Decides this launch's ribbon and keeps the record.
    func launch(
        releases: [WhatsNewRelease], record: WhatsNewRecord?, currentBuild: Int, hasGuest: Bool,
        offers: (WhatsNewEntry) -> Bool
    ) {
        list(releases)
        self.currentBuild = currentBuild
        let launch = WhatsNewRules.launch(
            releases: releases, record: record, currentBuild: currentBuild, hasGuest: hasGuest, offers: offers
        )
        pending = launch.pending
        keep?(launch.record)
    }

    /// The person opened the notes: they are read, so no later launch offers
    /// them. The ribbon stays until `putAway`, which the notes' closing calls.
    func read() {
        guard showsRibbon else { return }
        keep?(WhatsNewRecord(seenBuild: currentBuild))
    }

    /// The ribbon goes: closed, swiped away or read.
    func putAway() {
        guard showsRibbon else { return }
        pending = []
        keep?(WhatsNewRecord(seenBuild: currentBuild))
    }
}

extension WhatsNewCatalog {
    /// The notes the application bundles, `WhatsNew.json`; none if the
    /// resource is missing or unreadable.
    nonisolated static func load(from bundle: Bundle = .main) -> [WhatsNewRelease] {
        guard let url = bundle.url(forResource: "WhatsNew", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let releases = try? decode(data)
        else {
            assertionFailure("WhatsNew.json is missing or unreadable")
            return []
        }
        return releases
    }
}
