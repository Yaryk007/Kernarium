// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import UIKit

/// The snapshot screens of the preview: `-screen snapshots-…` opens each state
/// with mock snapshots behind the real screens, through the preview driver
/// (`PreviewSnapshotDriver`), so a capture needs no guest. Every operation the
/// screens offer works on the mock list.
enum SnapshotPreview {
    /// Every snapshot screen, for `-screen`.
    static let screens: [String] = [
        "snapshots-home-none", "snapshots-home", "snapshots-none", "snapshots", "snapshots-measuring",
        "snapshots-many", "snapshots-take", "snapshots-taken",
        "snapshots-unavailable-first-start", "snapshots-unavailable-not-shut-down",
        "snapshots-unavailable-update", "snapshots-unavailable-mismatch", "snapshots-unavailable-apfs",
        "snapshots-detail", "snapshots-detail-older", "snapshots-detail-newer", "snapshots-detail-damaged",
        "snapshots-restore", "snapshots-restore-older", "snapshots-restore-update",
        "snapshots-restored", "snapshots-restored-older",
        "snapshots-delete", "snapshots-deleted", "snapshots-rename", "snapshots-rename-alert",
        "snapshots-space", "snapshots-space-update", "snapshots-space-lower-bound",
        "snapshots-low-space", "snapshots-erase", "snapshots-blocked", "snapshots-backup",
        "snapshots-unfinished", "snapshots-unfinished-sheet", "snapshots-unavailable-running", "snapshots-failed",
        "snapshots-unfinished-unknown", "snapshots-unfinished-unknown-sheet", "snapshots-taken-measuring",
        "snapshots-unfinished-lost", "snapshots-unfinished-lost-sheet", "snapshots-unfinished-update",
    ]

    /// The device a screen needs instead of the idiom's own.
    static func host(for screen: String) -> HostDevice? {
        switch screen {
        case "snapshots-unavailable-apfs": PreviewData.host(lowSpace: false, device: "mac-64-gib")
        case "snapshots-low-space": PreviewData.host(lowSpace: true)
        default: nil
        }
    }

    /// Home's sections a screen scrolls to.
    static let scrollTargets: [String: String] = [
        "snapshots-erase": "erase", "snapshots-backup": "backup",
    ]

    private static var pad: Bool { UIDevice.current.userInterfaceIdiom == .pad }

    /// What a screen's driver starts as.
    static func seed(for screen: String) -> PreviewSnapshotSeed {
        var seed = PreviewSnapshotSeed.sample()
        switch screen {
        case "welcome", "create", "create-low-space", "home-new", "boot-first",
             "snapshots-home-none", "snapshots-none":
            seed.items = []
            seed.total = nil
        case "snapshots-unavailable-first-start":
            seed.items = []
            seed.total = nil
            seed.createRefusal = .noData
        case "snapshots-measuring":
            seed.measuringLatency = .seconds(3600)
        case "snapshots-many":
            seed = PreviewSnapshotSeed.many()
        case "snapshots-unavailable-not-shut-down", "home-closed":
            seed.createRefusal = screen.hasPrefix("snapshots") ? .notShutDown : nil
        case "snapshots-unavailable-update", "snapshots-restore-update", "snapshots-space-update":
            seed.createRefusal = .unfinishedUpdate
            seed.updateCopy = SnapshotFigure(bytes: UInt64(2.3 * Double(Limits.bytesPerGiB)))
        case "home-update-failed", "home-update-failed-restore":
            seed.createRefusal = .unfinishedUpdate
        case "snapshots-unavailable-mismatch":
            seed.createRefusal = .mismatch
        case "snapshots-unavailable-apfs":
            seed.createRefusal = .volumeCannotClone
        case "snapshots-unavailable-running":
            seed.gate = .guestNotStopped
        case "snapshots-detail-damaged":
            seed.items.insert(PreviewSnapshotSeed.damaged(), at: 1)
        case "snapshots-space-lower-bound":
            seed.total = SnapshotFigure(bytes: seed.total?.bytes ?? 0, isLowerBound: true)
        case "snapshots-blocked":
            seed.dataRelease = SnapshotRelease(releaseID: PreviewSnapshotSeed.newerRelease, buildNumber: 1_794_000_000)
        case "snapshots-unfinished", "snapshots-unfinished-sheet", "snapshots-unfinished-update":
            seed.unfinishedRestore = SnapshotUnfinishedRestore(
                snapshot: seed.items[3].id,
                lastFailure: SnapshotFailure(POSIXError(.ENOSPC)))
        case "snapshots-unfinished-unknown", "snapshots-unfinished-unknown-sheet":
            // Which snapshot it was cannot be read.
            seed.unfinishedRestore = SnapshotUnfinishedRestore()
        case "snapshots-unfinished-lost", "snapshots-unfinished-lost-sheet":
            // The record names a snapshot that is not there.
            seed.unfinishedRestore = SnapshotUnfinishedRestore(snapshot: SnapshotID())
        case "snapshots-failed":
            seed.failure = POSIXError(.EIO)
        default:
            break
        }
        return seed
    }

    /// Sets the mock state a screen shows beside the snapshots: the guest's
    /// plan and what a restore leaves of it.
    @MainActor
    static func apply(screen: String, to model: AppModel) {
        let seed = seed(for: screen)
        model.snapshotDriver = PreviewSnapshotDriver(seed)
        switch screen {
        case "snapshots-unavailable-first-start":
            model.guest = Guest.fresh(on: model.host)
            model.plan = .firstStart
        case "snapshots-unavailable-update", "snapshots-restore-update", "snapshots-space-update":
            model.plan = .unfinishedUpdate(to: PreviewData.systemRelease, canRetry: true, canRestore: true)
        case "snapshots-unavailable-mismatch":
            model.plan = .blocked(.mismatch)
        case "snapshots-blocked":
            model.plan = .blocked(.olderAndroid)
        case "snapshots-unfinished-update":
            // What the start plan is with a restore of an older snapshot unfinished:
            // the update, which the card says; Home shows no update card beside it.
            model.plan = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "snapshots-restored":
            model.planAfterRestore = .normal
        case "snapshots-restored-older":
            model.planAfterRestore = .update(from: PreviewData.previousSystemRelease, to: PreviewData.systemRelease)
        case "snapshots-taken", "snapshots-failed", "snapshots-deleted", "snapshots-taken-measuring":
            model.snapshots.freshDuration = .seconds(3600)
        default:
            break
        }
    }

    /// Opens `page` as a tap would: pushed on iPhone, selected on iPad.
    @MainActor
    private static func open(_ page: SnapshotPage, in store: SnapshotsModel) {
        if pad {
            store.selection = page
        } else {
            store.route = [page]
        }
    }

    /// The sheet and what is open in it, after the first frame.
    @MainActor
    static func applyDeferred(screen: String, to model: AppModel) async {
        guard screen.hasPrefix("snapshots"),
              !["snapshots-home-none", "snapshots-home", "snapshots-low-space", "snapshots-blocked",
                "snapshots-backup", "snapshots-unfinished", "snapshots-unfinished-unknown", "snapshots-unfinished-lost",
                "snapshots-unfinished-update"].contains(screen)
        else { return }
        let store = model.snapshots
        if screen == "snapshots-measuring" {
            // The figures never arrive: the sheet shows the names and spinners.
            model.sheet = .snapshots
            return
        }
        // The names are read before the page that needs one is opened.
        await store.reload()
        if screen == "snapshots-erase" {
            model.confirmingErase = true
            return
        }
        let items = store.items
        guard items.count > 3 || ["snapshots-none", "snapshots-unavailable-first-start"].contains(screen) else { return }
        switch screen {
        case "snapshots-detail", "snapshots-restore", "snapshots-delete", "snapshots-restored":
            open(.snapshot(items[0].id), in: store)
        case "snapshots-detail-older", "snapshots-restore-older", "snapshots-restore-update", "snapshots-restored-older":
            open(.snapshot(items[3].id), in: store)
        case "snapshots-detail-newer":
            open(.snapshot(items[2].id), in: store)
        case "snapshots-detail-damaged":
            open(.snapshot(items[1].id), in: store)
        case "snapshots-rename":
            store.focusName = true
            open(.snapshot(items[1].id), in: store)
        case "snapshots-space", "snapshots-space-update", "snapshots-space-lower-bound":
            open(.space, in: store)
        default:
            break
        }
        model.sheet = .snapshots
        try? await Task.sleep(for: .milliseconds(700))
        switch screen {
        case "snapshots-take":
            store.requestTake()
        case "snapshots-restore", "snapshots-restore-older", "snapshots-restore-update":
            store.restoreTarget = selectedOrPushed(store)
        case "snapshots-delete":
            store.deleteTarget = selectedOrPushed(store)
        case "snapshots-rename-alert":
            store.requestRename(items[0].id)
        case "snapshots-taken":
            // The real operation: the new snapshot comes first and is marked.
            await store.take(name: "Before installing games")
        case "snapshots-taken-measuring":
            // After an operation the figures give way to the measuring state
            // until the disk is read again, which here takes an hour.
            (model.snapshotDriver as? PreviewSnapshotDriver)?.setMeasuringLatency(.seconds(3600))
            Task { await store.take(name: "Before installing games") }
        case "snapshots-failed":
            await store.take(name: "Before installing games")
        case "snapshots-deleted":
            // "Before r20" goes, and "Fresh setup" frees more once what the
            // two shared is its alone.
            await store.delete(items[3].id)
        case "snapshots-restored", "snapshots-restored-older":
            await store.restore(selectedOrPushed(store) ?? items[0].id)
        default:
            break
        }
    }

    /// The snapshot the open page shows.
    @MainActor
    private static func selectedOrPushed(_ store: SnapshotsModel) -> SnapshotID? {
        if case .snapshot(let id) = store.route.last ?? store.selection { return id }
        return nil
    }
}
