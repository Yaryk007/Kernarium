// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// What the snapshot screens say, from the driver's values: no model, no view.
struct SnapshotWordsTests {
    private let appRevision = "r20"

    /// Every refusal the store has. The switch has no default, so a case added
    /// to `SnapshotRefusal` stops this file until the screens have words for it.
    nonisolated static let allRefusals: [SnapshotRefusal] = [
        .guestNotStopped, .busy, .noData, .unreadableRecord, .notShutDown, .unfinishedUpdate, .mismatch,
        .volumeCannotClone, .notEnoughSpace, .newerAndroid, .olderDataLayout, .differentSystem,
        .diskSizeNotAccepted(gib: 64), .missing, .incomplete, .restoreUnfinished, .invalidName,
    ]

    @Test func everyRefusalHasWords() {
        for refusal in Self.allRefusals {
            switch refusal {
            case .guestNotStopped, .busy, .noData, .unreadableRecord, .notShutDown, .unfinishedUpdate, .mismatch,
                 .volumeCannotClone, .notEnoughSpace, .newerAndroid, .olderDataLayout, .differentSystem,
                 .diskSizeNotAccepted, .missing, .incomplete, .restoreUnfinished, .invalidName:
                break
            }
            let text = SnapshotWords.refusal(refusal, snapshot: SnapshotSample.item("x", release: "20261110-los24-r22"),
                                             appRevision: appRevision, device: "iPhone")
            #expect(!text.isEmpty, "\(refusal)")
            #expect(!text.contains("AndariumCore"), "\(refusal)")
        }
    }

    @Test func theWordsForARefusedNameAreTheRuleItself() {
        let words = SnapshotWords.refusal(.invalidName)
        #expect(words == "A name has at most 100 characters, and no line breaks, tabs or other control characters.")
        #expect(words.contains("\(SnapshotRules.maximumNameLength) characters"))
        // What the rule refuses: a line break, a tab (a pasted one too), any other control character, too long.
        for name in ["a\nb", "a\tb", "a\u{7}b", String(repeating: "x", count: SnapshotRules.maximumNameLength + 1)] {
            #expect(SnapshotRules.normalizedName(name) == nil, "\(name.debugDescription)")
        }
        // What it accepts: spaces and the edges are trimmed.
        #expect(SnapshotRules.normalizedName("  Before r20\t\n") == "Before r20")
    }

    @Test func whatTakeSnapshotSaysWhenItIsUnavailable() {
        #expect(SnapshotWords.takeReason(.volumeCannotClone) == "The virtual machine\u{2019}s data cannot be snapshotted (it needs an APFS volume).")
        #expect(SnapshotWords.takeReason(.noData) == "Start the virtual machine once first. Until then there is no data to keep.")
        #expect(SnapshotWords.takeReason(.mismatch) == "Not while the virtual machine\u{2019}s files do not match this app version.")
        #expect(SnapshotWords.takeReason(.unfinishedUpdate)
            == "Not while the system image update is unfinished. Update and start tries it again, or use Restore on Home.")
        #expect(SnapshotWords.takeReason(.notShutDown)
            == "Android wasn\u{2019}t shut down when the virtual machine last ran. Start the virtual machine and shut it down from Android, then take a snapshot.")
        // An operation under way shows itself and says nothing.
        #expect(SnapshotWords.takeReason(.busy) == nil)
        #expect(SnapshotWords.takeReason(.restoreUnfinished) != nil && SnapshotWords.takeReason(.guestNotStopped) != nil)
    }

    @Test func whatRestoreSaysItDoesOrWhyItCannot() {
        let current = SnapshotSample.item("now")
        let older = SnapshotSample.item("then", release: "20261108-los24-r18")
        let newer = SnapshotSample.item("later", release: "20261110-los24-r22", restore: .refused(.newerAndroid))
        #expect(SnapshotWords.restoreNote(of: current, appRevision: appRevision, endsUpdate: false)
            == "Replaces the virtual machine\u{2019}s current data with this snapshot.")
        #expect(SnapshotWords.restoreNote(of: current, appRevision: appRevision, endsUpdate: true)
            == "Replaces the virtual machine\u{2019}s current data with this snapshot. The unfinished system image update ends.")
        #expect(SnapshotWords.restoreNote(of: older, appRevision: appRevision, endsUpdate: false)
            == "Replaces the virtual machine\u{2019}s current data with this snapshot, which is from r18. The next start updates it to r20.")
        #expect(SnapshotWords.restoreNote(of: newer, appRevision: appRevision, endsUpdate: false)
            == "This snapshot is from r22, newer than this app\u{2019}s r20. Update Andarium to restore it.")
        for (plan, words) in [
            (SnapshotRestorePlan.refused(.olderDataLayout),
             "This snapshot\u{2019}s data uses a newer layout than this app version can open. Update Andarium to restore it."),
            (.refused(.differentSystem),
             "This snapshot was taken with a different system image of the same release, so this app version can\u{2019}t restore it."),
            (.refused(.incomplete),
             "Some of this snapshot\u{2019}s files are missing, so it can\u{2019}t be restored. It can only be deleted."),
        ] {
            let item = SnapshotSample.item("x", restore: plan)
            #expect(SnapshotWords.restoreNote(of: item, appRevision: appRevision, endsUpdate: true) == words)
        }
    }

    @Test func theFlagUnderARowThatCannotBeRestored() {
        #expect(SnapshotWords.rowFlag(.asIs) == nil)
        #expect(SnapshotWords.rowFlag(.updatesAtNextStart(from: "a", to: "b")) == nil)
        #expect(SnapshotWords.rowFlag(.refused(.newerAndroid)) == "Needs a newer Andarium")
        #expect(SnapshotWords.rowFlag(.refused(.olderDataLayout)) == "Needs a newer Andarium")
        #expect(SnapshotWords.rowFlag(.refused(.differentSystem)) == "Can\u{2019}t be restored")
        #expect(SnapshotWords.rowFlag(.refused(.diskSizeNotAccepted(gib: 64))) == "Can\u{2019}t be restored")
        #expect(SnapshotWords.rowFlag(.refused(.incomplete)) == "Damaged")
    }

    @Test func figuresSayWhenTheyAreAtLeast() {
        let exact = SnapshotFigure(bytes: UInt64(4.6 * Double(1 << 30)))
        let bound = SnapshotFigure(bytes: exact.bytes, isLowerBound: true)
        #expect(SnapshotWords.size(exact) == "4.6 GiB")
        #expect(SnapshotWords.size(bound) == "At least 4.6 GiB")
        #expect(SnapshotWords.sheetSubtitle(total: exact, device: "iPhone") == "4.6 GiB on this iPhone")
        #expect(SnapshotWords.sheetSubtitle(total: bound, device: "iPad") == "At least 4.6 GiB on this iPad")
        #expect(SnapshotWords.homeSubtitle(count: 0, total: nil) == "None yet")
        #expect(SnapshotWords.homeSubtitle(count: 1, total: nil) == "1 snapshot")
        #expect(SnapshotWords.homeSubtitle(count: 5, total: exact) == "5 snapshots \u{00B7} 4.6 GiB")
        #expect(SnapshotWords.homeSubtitle(count: 5, total: bound) == "5 snapshots \u{00B7} at least 4.6 GiB")
        #expect(SnapshotWords.freesAccessibility(SnapshotFigure(bytes: 214 << 20)) == "Frees 214 MiB if deleted")
        #expect(SnapshotWords.freesAccessibility(SnapshotFigure(bytes: 214 << 20, isLowerBound: true))
            == "Frees at least 214 MiB if deleted")
    }

    @Test func aRowNamesTheSnapshotOrWhenItWasTaken() {
        let named = SnapshotSample.item("Before r20", release: "20261108-los24-r18")
        let unnamed = SnapshotSample.item("")
        #expect(SnapshotWords.title(of: named) == "Before r20")
        #expect(SnapshotWords.rowDetail(of: named) == "\(SnapshotWords.taken(named.created)) \u{00B7} r18")
        #expect(SnapshotWords.title(of: unnamed) == SnapshotWords.taken(unnamed.created))
        #expect(SnapshotWords.rowDetail(of: unnamed) == "r20")
    }

    /// A column too narrow for the whole line shows the date and the time on
    /// their own lines; each half is part of what the whole line says.
    @Test func theDateAndTheTimeAreTheTwoHalvesOfWhenItWasTaken() {
        let created = SnapshotSample.item("").created
        let whole = SnapshotWords.taken(created)
        let date = SnapshotWords.takenDate(created), time = SnapshotWords.takenTime(created)
        #expect(!date.isEmpty && !time.isEmpty)
        #expect(whole.contains(date), "\(whole) does not contain \(date)")
        #expect(whole.contains(time), "\(whole) does not contain \(time)")
    }

    @Test func theConfirmationsSayWhatTheyDo() {
        let named = SnapshotSample.item("Before r20", release: "20261108-los24-r18")
        let unnamed = SnapshotSample.item("")
        let date = SnapshotWords.taken(named.created)
        #expect(SnapshotWords.restoreTitle(of: named) == "Restore \u{201C}Before r20\u{201D}?")
        #expect(SnapshotWords.restoreTitle(of: unnamed) == "Restore this snapshot?")
        #expect(SnapshotWords.restoreMessage(of: named, endsUpdate: false)
            == "The virtual machine\u{2019}s current data is replaced by this snapshot from \(date), and anything not in it is deleted. "
            + "Some apps may ask you to sign in again. The next start updates it to r20. This can\u{2019}t be undone.")
        #expect(SnapshotWords.restoreMessage(of: unnamed, endsUpdate: true).contains(" The unfinished system image update ends. This"))
        #expect(SnapshotWords.deleteTitle(of: named) == "Delete \u{201C}Before r20\u{201D}?")
        #expect(SnapshotWords.deleteTitle(of: unnamed) == "Delete the snapshot from \(SnapshotWords.taken(unnamed.created))?")
        #expect(SnapshotWords.deleteMessage(frees: SnapshotFigure(bytes: 214 << 20))
            == "Deleting it frees 214 MiB. This can\u{2019}t be undone.")
        #expect(SnapshotWords.deleteMessage(frees: SnapshotFigure(bytes: 214 << 20, isLowerBound: true))
            == "Deleting it frees at least 214 MiB. This can\u{2019}t be undone.")
        #expect(SnapshotWords.deleteMessage(frees: nil) == "This can\u{2019}t be undone.")
    }

    @Test func homeMentionsSnapshotsOnlyWhereTheyChangeADecision() {
        #expect(SnapshotWords.eraseMessage(snapshots: 0) == "Everything the virtual machine has stored is deleted. This can\u{2019}t be undone.")
        #expect(SnapshotWords.eraseMessage(snapshots: 1)
            == "Everything the virtual machine has stored is deleted, and its snapshot with it. This can\u{2019}t be undone.")
        #expect(SnapshotWords.eraseMessage(snapshots: 5)
            == "Everything the virtual machine has stored is deleted, and its 5 snapshots with it. This can\u{2019}t be undone.")
        #expect(SnapshotWords.lowSpaceText(hasSnapshots: false, startReserve: "2 GiB", device: "iPhone").hasSuffix("iPhone Storage."))
        #expect(SnapshotWords.lowSpaceText(hasSnapshots: true, startReserve: "2 GiB", device: "iPhone").hasSuffix("iPhone Storage, or delete snapshots."))
        #expect(SnapshotWords.backupNote(device: "iPhone", hasSnapshots: false) == "Puts the virtual machine’s apps and files in iCloud and computer backups of this iPhone.")
        #expect(SnapshotWords.backupNote(device: "iPhone", hasSnapshots: true).hasSuffix(" Snapshots are not included."))
        #expect(SnapshotWords.blockedText(.olderAndroid, hasOlderSnapshot: false) == StartBlock.olderAndroid.explanation)
        #expect(SnapshotWords.blockedText(.olderAndroid, hasOlderSnapshot: true).contains("restore an older snapshot"))
        #expect(SnapshotWords.blockedText(.mismatch, hasOlderSnapshot: true) == StartBlock.mismatch.explanation)
    }

    @Test func theNoticeAfterARestoreSaysWhatWasRestoredAndWhatTheNextStartDoes() {
        let created = Date(timeIntervalSince1970: 1_794_000_000)
        let date = SnapshotWords.taken(created)
        #expect(SnapshotWords.restoredText(RestoredSnapshotNotice(name: "Before r20", created: created, updatesTo: "r20"))
            == "The virtual machine\u{2019}s data is back to \u{201C}Before r20\u{201D} from \(date). The next start updates it to r20.")
        #expect(SnapshotWords.restoredText(RestoredSnapshotNotice(name: "", created: created))
            == "The virtual machine\u{2019}s data is back to the snapshot from \(date).")
        let failure = SnapshotFailure(POSIXError(.ENOSPC))
        #expect(SnapshotWords.unfinishedText(.named, item: SnapshotSample.item("x"), failure: failure).contains(failure.description))
    }

    @Test func aRestoreThatDidNotFinishSaysOnlyWhatIsKnown() {
        let older = SnapshotSample.item("Before r20", release: "20261108-los24-r18")
        // The snapshot is known and whole: Starting finishes it, and nothing says the data is back.
        let known = SnapshotWords.unfinishedText(.named, item: older, failure: nil)
        #expect(known == "Andarium could not finish restoring \u{201C}Before r20\u{201D} from \(SnapshotWords.taken(older.created)). "
            + "Starting finishes it. The next start updates it to r20.")
        #expect(!known.contains("is back"))
        let unnamed = SnapshotSample.item("")
        #expect(SnapshotWords.unfinishedText(.named, item: unnamed, failure: nil)
            == "Andarium could not finish restoring the snapshot from \(SnapshotWords.taken(unnamed.created)). Starting finishes it.")
        // Which snapshot it was cannot be read, or it names one that is gone or damaged: a start drops the
        // restore, so nothing is promised, and the two states are told apart.
        let may = " The virtual machine\u{2019}s data may be from before the restore or from the snapshot."
        let unreadable = "A snapshot restore did not finish, and Andarium cannot read which one."
        let lost = "A snapshot restore did not finish, and its snapshot is missing or damaged."
        for (kind, reason) in [(UnfinishedRestoreKind.unreadable, unreadable), (.lost, lost)] {
            let text = SnapshotWords.unfinishedText(kind, item: nil, failure: nil)
            #expect(text == reason + may)
            #expect(!text.contains("Starting finishes it") && !text.contains("is back"))
            // The reasons Take and Restore give, and the refusal, follow.
            #expect(SnapshotWords.unfinishedReason(kind) == reason)
            #expect(SnapshotWords.takeReason(.restoreUnfinished, restore: kind) == reason)
            #expect(SnapshotWords.nowReason(.restoreUnfinished, restore: kind) == reason)
            #expect(SnapshotWords.refusal(.restoreUnfinished, restore: kind) == reason)
        }
        for text in [
            SnapshotWords.takeReason(.restoreUnfinished), SnapshotWords.nowReason(.restoreUnfinished),
            SnapshotWords.refusal(.restoreUnfinished), SnapshotWords.unfinishedReason(.named),
        ] {
            #expect(text == "A restore did not finish. Starting finishes it.")
        }
    }

    @Test func theLastFailureEndsTheTextWithOneFullStop() {
        // The system's descriptions come with and without a full stop of their own.
        let own = SnapshotFailure(CocoaError(.fileWriteOutOfSpace))
        let bare = SnapshotFailure(POSIXError(.ENOSPC))
        #expect(own.description.hasSuffix(".") && !bare.description.hasSuffix("."))
        for failure in [own, bare] {
            let text = SnapshotWords.unfinishedText(.named, item: SnapshotSample.item("x"), failure: failure)
            #expect(text.hasSuffix("stopped: \(failure.description.hasSuffix(".") ? failure.description : failure.description + ".")"))
            #expect(!text.contains(".."))
        }
    }

    @Test func theSpacePageSaysWhatAppliesToTheDevice() {
        let plain = SnapshotWords.spaceParagraphs(device: "iPhone", lowerBound: false, hasUpdateCopy: false)
        let full = SnapshotWords.spaceParagraphs(device: "iPhone", lowerBound: true, hasUpdateCopy: true)
        #expect(plain.count == 4 && full.count == 6)
        #expect(full.contains { $0.contains("at least the size shown") } && full.contains { $0.contains("unfinished system image update") })
        #expect(plain.last == "Snapshots stay on this iPhone. They are not in backups, and Erase virtual machine deletes them.")
    }
}

/// The screens' model with a fake driver: each flow the sheet offers, the
/// refusals and the busy states.
@MainActor
final class SnapshotsModelTests {
    // The model of a test adopts the process's App Intent hub, which the suites
    // of Add to Home Screen replace: these take turns with them (`AndroidAppsHubLock`).
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private struct Setup {
        let model: AppModel
        let driver: FakeSnapshotDriver
        var store: SnapshotsModel { model.snapshots }
    }

    private func setup(_ items: [SnapshotSummary] = []) -> Setup {
        let host = HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)
        let system = SystemInfo(
            version: "17", release: "20261109-los24-r20", kernel: "Linux 6.18", architecture: "arm64", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1 << 30)
        let driver = FakeSnapshotDriver(items: items)
        model.snapshotDriver = driver
        // The sheet is open, as it is for what the screens do.
        model.sheet = .snapshots
        model.snapshots.renameDelay = .milliseconds(30)
        model.snapshots.freshDuration = .milliseconds(200)
        return Setup(model: model, driver: driver)
    }

    /// Lets the main actor run until `condition` holds, for at most two seconds.
    private func until(_ condition: () -> Bool) async -> Bool {
        for _ in 0..<400 {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(5))
        }
        return condition()
    }

    // MARK: Reading

    @Test func namesComeFirstAndFiguresWhenMeasured() async {
        let s = setup([SnapshotSample.item("a", frees: 5 << 20), SnapshotSample.item("b", ago: 2)])
        s.driver.holdMeasuring = true
        let reading = Task { await s.store.reload() }
        #expect(await until { s.store.listing != nil })
        #expect(s.store.items.count == 2 && !s.store.isMeasured)
        #expect(s.store.frees(s.store.items[0]) == nil && s.store.total == nil, "spinners, not zeros")
        s.driver.releaseMeasuring()
        await reading.value
        #expect(s.store.isMeasured)
        #expect(s.store.frees(s.store.items[0]) == SnapshotFigure(bytes: 5 << 20))
        #expect(s.store.total != nil)
    }

    @Test func aRereadOfADiskThatDidNotChangeKeepsTheLastFigure() async {
        let s = setup([SnapshotSample.item("a", frees: 5 << 20)])
        await s.store.reload()
        let id = s.store.items[0].id
        s.driver.holdMeasuring = true
        let reading = Task { await s.store.reload(force: true) }
        #expect(await until { !s.store.isMeasured })
        #expect(s.store.frees(s.store.item(id)!) == SnapshotFigure(bytes: 5 << 20), "nothing changed: the last figure stays")
        #expect(s.store.freesNow(s.store.item(id)!) == nil, "but it is not said as measured now")
        s.driver.releaseMeasuring()
        await reading.value
        #expect(s.store.isMeasured)
    }

    @Test func whatAnOperationChangedIsShownAsMeasuringUntilItIs() async {
        let old = SnapshotSample.item("old", frees: 5 << 20, kept: 2 << 30)
        let s = setup([old])
        await s.store.reload()
        #expect(s.store.frees(s.store.items[0]) == SnapshotFigure(bytes: 5 << 20))
        #expect(s.store.freesNow(s.store.items[0]) == SnapshotFigure(bytes: 5 << 20))
        #expect(s.store.total != nil && s.store.kept(s.store.items[0]) != nil)

        // A snapshot is taken: the earlier numbers are not the disk's now.
        s.driver.holdMeasuring = true
        let taking = Task { await s.store.take(name: "new") }
        #expect(await until { s.store.count == 2 })
        for item in s.store.items {
            #expect(s.store.frees(item) == nil && s.store.freesNow(item) == nil && s.store.kept(item) == nil, "\(item.name)")
        }
        #expect(s.store.total == nil && !s.store.isMeasured)
        s.driver.releaseMeasuring()
        await taking.value
        #expect(s.store.isMeasured && s.store.total != nil)
        #expect(s.store.items.allSatisfy { s.store.frees($0) != nil })

        // The same after a delete, and after an operation that finished somewhere else.
        for change in [0, 1] {
            s.driver.holdMeasuring = true
            if change == 0 {
                let deleting = Task { await s.store.delete(s.store.items[0].id) }
                #expect(await until { s.store.count == 1 })
                s.driver.releaseMeasuring()
                await deleting.value
            } else {
                s.driver.finishedElsewhere()
                #expect(!s.store.isMeasured && s.store.total == nil)
                #expect(s.store.frees(s.store.items[0]) == nil)
                s.driver.releaseMeasuring()
                #expect(await until { s.store.isMeasured })
            }
            #expect(s.store.isMeasured && s.store.frees(s.store.items[0]) != nil)
        }
    }

    @Test func aRenameChangesNoFigureAndTheFiguresStayShown() async {
        let a = SnapshotSample.item("a", frees: 5 << 20, kept: 2 << 30)
        let b = SnapshotSample.item("b", ago: 2, frees: 7 << 20, kept: 3 << 30)
        let s = setup([a, b])
        await s.store.reload()
        let total = s.store.total
        #expect(total != nil)
        // The name typed in a page is saved, and the disk is read again.
        s.driver.holdMeasuring = true
        s.store.edit(name: "A renamed", of: a.id)
        let saving = Task { await s.store.flushName() }
        #expect(await until { s.driver.log == ["rename A renamed"] && !s.store.isMeasured })
        for item in s.store.items {
            #expect(s.store.frees(item) != nil && s.store.kept(item) != nil, "\(item.name): no spinner for a rename")
        }
        #expect(s.store.frees(s.store.item(a.id)!) == SnapshotFigure(bytes: 5 << 20))
        #expect(s.store.kept(s.store.item(b.id)!) == SnapshotFigure(bytes: 3 << 30))
        #expect(s.store.total == total, "Home's row keeps its figure")
        #expect(s.store.freesNow(s.store.item(a.id)!) == nil, "the Delete alert's size waits for the new reading")
        s.driver.releaseMeasuring()
        await saving.value
        #expect(s.store.isMeasured && s.store.item(a.id)?.name == "A renamed")
        #expect(s.store.frees(s.store.item(a.id)!) == SnapshotFigure(bytes: 5 << 20))
        // An operation that does change figures still gives way to the measuring state.
        s.driver.holdMeasuring = true
        let deleting = Task { await s.store.delete(b.id) }
        #expect(await until { s.store.count == 1 })
        #expect(s.store.frees(s.store.items[0]) == nil && s.store.total == nil)
        s.driver.releaseMeasuring()
        await deleting.value
    }

    @Test func theDeleteAlertNamesASizeOnlyFromAListingMeasuredNow() async {
        let a = SnapshotSample.item("a", frees: 5 << 20)
        let s = setup([a, SnapshotSample.item("b", ago: 2)])
        await s.store.reload()
        let size = SnapshotFigure(bytes: 5 << 20)
        #expect(SnapshotWords.deleteMessage(frees: s.store.freesNow(s.store.item(a.id)!)) == SnapshotWords.deleteMessage(frees: size))
        // Something finished since: the number is not said, and the alert says only that it cannot be undone.
        s.driver.holdMeasuring = true
        s.driver.finishedElsewhere()
        let message = SnapshotWords.deleteMessage(frees: s.store.freesNow(s.store.item(a.id)!))
        #expect(message == "This can\u{2019}t be undone.")
        s.driver.releaseMeasuring()
        #expect(await until { s.store.isMeasured })
    }

    @Test func aListingThatIsCurrentIsNotReadAgainUnlessAsked() async {
        let s = setup([SnapshotSample.item("a")])
        await s.store.reload()
        let reads = s.driver.measuredReads
        await s.store.reload()
        #expect(s.driver.measuredReads == reads)
        await s.store.reload(force: true)
        #expect(s.driver.measuredReads == reads + 1)
    }

    @Test func startingTheGuestPutsAReadingAwayAndItsMeasureStops() async {
        let s = setup([SnapshotSample.item("a", frees: 5 << 20), SnapshotSample.item("b", ago: 2)])
        s.driver.holdMeasuring = true
        let reading = Task { await s.store.reload() }
        #expect(await until { s.store.listing != nil && s.driver.measuredReads == 1 })
        s.model.start(.normal)
        #expect(s.model.restoredSnapshot == nil)
        s.driver.releaseMeasuring()
        await reading.value
        #expect(s.driver.cancelledMeasures == 1, "the measure was told")
        #expect(!s.store.isMeasured, "and what it answered is not taken for figures")
        #expect(s.store.frees(s.store.items[0]) == nil && s.store.total == nil)
        #expect(s.driver.measuredReads == 1, "no read goes on")
    }

    @Test func nothingIsMeasuredWhileARunHoldsTheDiskAndTheEndOfTheRunMeasuresOnce() async {
        let s = setup([SnapshotSample.item("a", frees: 5 << 20)])
        await s.store.reload()
        let measured = s.driver.measuredReads
        s.driver.setRuntimeActive(true)
        #expect(!s.store.isMeasured && s.store.frees(s.store.items[0]) == nil, "the figures belong to the disk before the run")
        await s.store.reload()
        #expect(s.driver.measuredReads == measured, "only the names are read while it runs")
        #expect(s.store.count == 1)
        s.driver.setRuntimeActive(false)
        #expect(await until { s.store.isMeasured })
        #expect(s.driver.measuredReads == measured + 1)
        // Home appearing again, or the sheet opening, measures nothing more.
        await s.store.reload()
        await s.store.reload()
        #expect(s.driver.measuredReads == measured + 1)
    }

    @Test func theFirstReadingMeasuresWhenNothingWasRead() async {
        let s = setup([SnapshotSample.item("a")])
        #expect(s.store.listing == nil)
        await s.store.reload()
        #expect(s.driver.measuredReads == 1 && s.store.isMeasured)
    }

    @Test func anOperationThatFinishedElsewhereMakesTheListingReadAgain() async {
        let s = setup([SnapshotSample.item("a")])
        await s.store.reload()
        s.driver.items.append(SnapshotSample.item("b", ago: 2))
        s.driver.finishedElsewhere()
        #expect(await until { s.store.count == 2 && s.store.isMeasured })
    }

    /// A runtime driver that plays the guest's files, with the guest set up.
    private func withRuntime(_ s: Setup, plan: StartPlan = .normal) -> StubDriver {
        let stub = StubDriver(plan: plan)
        s.model.driver = stub
        s.model.refreshPlan()
        return stub
    }

    /// Everything the screens hold about the snapshots of a guest: read, shown, asked.
    private func holdsSomething(_ s: Setup) -> Bool {
        s.store.listing != nil || s.model.restoredSnapshot != nil || s.store.problem != nil || s.store.taking
            || s.store.restoreTarget != nil || s.store.deleteTarget != nil || s.store.renaming != nil || !s.store.route.isEmpty
    }

    private func readAndAskSomething(_ s: Setup) async {
        await s.store.reload()
        s.model.restoredSnapshot = RestoredSnapshotNotice(name: "", created: Date())
        s.store.route = [.snapshot(s.store.items[0].id)]
        s.store.requestTake()
        s.store.restoreTarget = s.store.items[0].id
        s.store.deleteTarget = s.store.items[0].id
        s.driver.nextRefusal = .missing
        await s.store.rename(s.store.items[0].id, to: "x")
        #expect(holdsSomething(s) && s.store.problem != nil)
    }

    @Test func erasingTheGuestForgetsWhatWasRead() async {
        let s = setup([SnapshotSample.item("a")])
        _ = withRuntime(s)
        await readAndAskSomething(s)
        s.model.erase()
        #expect(await until { s.model.busy == nil && s.model.guest == nil })
        #expect(!holdsSomething(s) && s.store.count == 0)
    }

    @Test func settingUpAnotherGuestForgetsWhatWasRead() async {
        let s = setup([SnapshotSample.item("a")])
        _ = withRuntime(s)
        await readAndAskSomething(s)
        s.model.create(Guest.fresh(on: s.model.host))
        #expect(await until { s.model.busy == nil })
        #expect(!holdsSomething(s) && s.store.count == 0)
    }

    @Test func aModelWithNoRuntimeForgetsToo() async {
        for action in 0..<2 {
            let s = setup([SnapshotSample.item("a")])
            await readAndAskSomething(s)
            if action == 0 { s.model.erase() } else { s.model.create(Guest.fresh(on: s.model.host)) }
            #expect(!holdsSomething(s) && s.store.count == 0, "\(action)")
        }
    }

    // MARK: Take

    @Test(arguments: ["Before trying Winlator", ""])
    func takingASnapshotWithOrWithoutAName(name: String) async throws {
        let old = SnapshotSample.item("old", ago: 3)
        let s = setup([old])
        await s.store.reload()
        s.store.requestTake()
        #expect(s.store.taking && s.store.takeName.isEmpty)
        s.store.takeName = name
        await s.store.take(name: s.store.takeName)
        #expect(s.driver.log == ["create \(name)"])
        #expect(s.store.count == 2 && s.store.problem == nil)
        let made = try #require(s.store.items.first { $0.id != old.id })
        #expect(s.store.fresh == made.id, "the new one is marked")
        #expect(await until { s.store.fresh == nil }, "and not for long")
        #expect(SnapshotWords.title(of: made) == (name.isEmpty ? SnapshotWords.taken(made.created) : name))
    }

    @Test(arguments: [
        SnapshotRefusal.volumeCannotClone, .noData, .mismatch, .unfinishedUpdate, .notShutDown,
    ])
    func takeSnapshotIsUnavailableWithTheReasonAboveIt(refusal: SnapshotRefusal) async {
        let s = setup([SnapshotSample.item("a")])
        s.driver.createRefusal = refusal
        await s.store.reload()
        #expect(s.store.takeRefusal == refusal)
        #expect(s.store.takeReason == SnapshotWords.takeReason(refusal))
        #expect(s.store.takeReason?.isEmpty == false)
        // A person who got through anyway is told in the same words.
        await s.store.take(name: "x")
        #expect(s.store.problem?.title == "The snapshot could not be taken")
        #expect(s.store.problem?.message == SnapshotWords.takeReason(refusal))
        #expect(s.store.count == 1)
    }

    @Test func takeSnapshotWaitsForTheFirstReading() {
        let s = setup()
        #expect(s.store.takeRefusal == .busy && s.store.takeReason == nil)
    }

    // MARK: Refusals and failures

    @Test(arguments: SnapshotWordsTests.allRefusals)
    func everyRefusalOfAnOperationIsToldInItsWords(refusal: SnapshotRefusal) async {
        let item = SnapshotSample.item("x", release: "20261110-los24-r22")
        let s = setup([item])
        await s.store.reload()
        for (operation, title) in [
            (SnapshotOperation.take, "The snapshot could not be taken"),
            (.restore, "The snapshot could not be restored"),
            (.delete, "The snapshot could not be deleted"),
            (.rename, "The snapshot could not be renamed"),
        ] {
            s.driver.nextRefusal = refusal
            switch operation {
            case .take: await s.store.take(name: "")
            case .restore: await s.store.restore(item.id)
            case .delete: await s.store.delete(item.id)
            case .rename: await s.store.rename(item.id, to: "y")
            }
            // The snapshot a refusal is about is named for Restore and Delete.
            let about = operation == .restore || operation == .delete ? item : nil
            #expect(s.store.problem?.title == title, "\(operation)")
            // The listing names no unfinished restore, so none is promised to finish.
            #expect(
                s.store.problem?.message == SnapshotWords.refusal(
                    refusal, snapshot: about, appRevision: "r20", device: "iPhone", restore: .unreadable),
                "\(operation) \(refusal)")
            s.store.problem = nil
        }
    }

    @Test func aFailureThatArrivesAfterTheSheetClosedGoesToTheAppsAlert() async {
        let item = SnapshotSample.item("old")
        let s = setup([item])
        await s.store.reload()
        // The name typed last is saved as the page goes, and the driver fails.
        s.store.edit(name: "New", of: item.id)
        s.model.sheet = nil
        s.driver.nextFailure = POSIXError(.EIO)
        await s.store.flushName()
        #expect(s.store.problem == nil, "nothing waits for the sheet to open again")
        #expect(s.model.alert?.title == "The snapshot could not be renamed")
        #expect(s.model.alert?.message == POSIXError(.EIO).localizedDescription)
        // With the sheet open the sheet says it.
        s.model.alert = nil
        s.model.sheet = .snapshots
        s.driver.nextRefusal = .missing
        await s.store.rename(item.id, to: "x")
        #expect(s.store.problem?.message == SnapshotWords.refusal(.missing) && s.model.alert == nil)
    }

    @Test func aGuestMadeAnewForgetsTheProblemAndThePromptsOfTheOneBefore() async {
        let item = SnapshotSample.item("old")
        let s = setup([item])
        await s.store.reload()
        s.driver.nextRefusal = .missing
        await s.store.delete(item.id)
        s.store.requestTake()
        s.store.takeName = "half typed"
        s.store.requestRename(item.id)
        s.store.restoreTarget = item.id
        s.store.deleteTarget = item.id
        #expect(s.store.problem != nil)
        s.model.forgetSnapshots()
        #expect(s.store.problem == nil && !s.store.taking && s.store.takeName.isEmpty)
        #expect(s.store.renaming == nil && s.store.restoreTarget == nil && s.store.deleteTarget == nil)
    }

    @Test func anIOErrorIsToldInTheSystemsWords() async {
        let s = setup([SnapshotSample.item("x")])
        await s.store.reload()
        s.driver.nextFailure = POSIXError(.EIO)
        await s.store.delete(s.store.items[0].id)
        #expect(s.store.problem?.title == "The snapshot could not be deleted")
        #expect(s.store.problem?.message == POSIXError(.EIO).localizedDescription)
        #expect(s.store.count == 1)
    }

    // MARK: Restore

    @Test func restoringAskedForConfirmationClosesTheSheetAndHomeSaysWhatHappened() async {
        let older = SnapshotSample.item("Before r20", release: "20261108-los24-r18", kept: 3 << 30)
        let s = setup([SnapshotSample.item("now"), older])
        // The files say what a start is: after the restore of an older snapshot, an update.
        let files = withRuntime(s)
        s.driver.onRestore = { files.plan = .update(from: "20261108-los24-r18", to: "20261109-los24-r20") }
        s.model.sheet = .snapshots
        await s.store.reload()
        s.store.route = [.snapshot(older.id)]
        // The alert holds the target until the person confirms.
        s.store.restoreTarget = older.id
        #expect(s.store.item(s.store.restoreTarget)?.name == "Before r20")
        #expect(s.driver.log.isEmpty, "nothing happens before Restore is pressed")
        await s.store.restore(older.id)
        #expect(s.driver.log == ["restore"])
        #expect(s.model.sheet == nil && s.store.route.isEmpty)
        #expect(s.model.restoredSnapshot == RestoredSnapshotNotice(name: "Before r20", created: older.created, updatesTo: "r20"))
        #expect(s.model.guest?.usedBytes == 3 << 30)
        #expect(s.model.plan == .update(from: "20261108-los24-r18", to: "20261109-los24-r20"), "read again from the files")
        #expect(s.store.problem == nil)
        // Home shows the notice, not the update card; Start is next, and starting clears it.
        #expect(!s.model.updateNoticeIsShown)
        s.model.start(.normal)
        #expect(s.model.restoredSnapshot == nil && s.model.updateNoticeIsShown)
        #expect(files.begun.count == 1)
    }

    @Test func aSnapshotThisAppCannotRestoreStaysAndSaysWhy() async {
        let newer = SnapshotSample.item("later", release: "20261110-los24-r22", restore: .refused(.newerAndroid))
        let s = setup([newer])
        s.model.sheet = .snapshots
        await s.store.reload()
        #expect(s.store.restoreRefusal(of: newer) == .newerAndroid)
        #expect(s.store.restoreNote(of: s.store.items[0])
            == "This snapshot is from r22, newer than this app\u{2019}s r20. Update Andarium to restore it.")
        await s.store.restore(newer.id)
        #expect(s.model.sheet == .snapshots, "the sheet stays")
        #expect(s.model.restoredSnapshot == nil)
        #expect(s.store.problem?.message == SnapshotWords.refusal(.newerAndroid, snapshot: newer, appRevision: "r20"))
        // Delete is still possible.
        #expect(s.store.deleteRefusal(of: s.store.items[0]) == nil)
    }

    @Test func aRestoreThatCouldNotFinishClosesTheSheetAndHomeKeepsSayingSo() async {
        let a = SnapshotSample.item("a")
        let b = SnapshotSample.item("b", ago: 2)
        let s = setup([a, b])
        s.model.sheet = .snapshots
        s.driver.restoreLeavesUnfinished = true
        await s.store.reload()
        await s.store.restore(a.id)
        #expect(s.model.sheet == nil)
        #expect(s.model.restoredSnapshot == nil, "Home's card for the unfinished restore says it")
        #expect(s.store.unfinishedRestore?.snapshot == a.id)
        #expect(s.store.unfinishedRestore?.lastFailure?.description == POSIXError(.ENOSPC).localizedDescription)
        #expect(s.store.problem == nil)
        // Nothing new until it finishes; the snapshot being restored stays, the other can go.
        #expect(s.store.takeRefusal == .restoreUnfinished)
        #expect(s.store.restoreRefusal(of: b) == .restoreUnfinished)
        #expect(s.store.deleteRefusal(of: a) == .restoreUnfinished)
        #expect(s.store.deleteRefusal(of: b) == nil)
        #expect(s.store.restoreNote(of: s.store.item(b.id)!) == SnapshotWords.nowReason(.restoreUnfinished))
        // A restore asked for anyway is refused in words, with the sheet staying.
        s.model.sheet = .snapshots
        await s.store.restore(b.id)
        #expect(s.model.sheet == .snapshots)
        #expect(s.store.problem?.message == SnapshotWords.refusal(.restoreUnfinished))
    }

    @Test func aRestoreWhoseSnapshotCannotBeToldPromisesNothing() async {
        let a = SnapshotSample.item("a")
        let damaged = SnapshotSample.item("", ago: 3, restore: .refused(.incomplete))
        let s = setup([a, damaged])
        await s.store.reload()
        // The record of the restore cannot be read; it names a snapshot that is damaged; it names one that is gone.
        for (unfinished, kind) in [
            (SnapshotUnfinishedRestore(), UnfinishedRestoreKind.unreadable),
            (SnapshotUnfinishedRestore(snapshot: damaged.id), .lost),
            (SnapshotUnfinishedRestore(snapshot: SnapshotID()), .lost),
        ] {
            let reason = SnapshotWords.unfinishedReason(kind)
            s.driver.unfinishedRestore = unfinished
            await s.store.reload(force: true)
            #expect(s.store.unfinishedRestoreKind == kind && s.store.unfinishedRestoreSnapshot == nil)
            #expect(s.store.unfinishedRestoreText?.hasPrefix(reason) == true)
            #expect(s.store.unfinishedRestoreText?.contains("Starting finishes it") == false)
            #expect(s.store.takeReason == reason)
            #expect(s.store.restoreNote(of: s.store.item(a.id)!) == reason)
            // A restore asked for anyway is told the same.
            s.model.sheet = .snapshots
            await s.store.restore(a.id)
            #expect(s.store.problem?.message == reason)
            s.store.problem = nil
        }
        // One that names a whole snapshot is known, and Starting finishes it.
        s.driver.unfinishedRestore = SnapshotUnfinishedRestore(snapshot: a.id)
        await s.store.reload(force: true)
        #expect(s.store.unfinishedRestoreKind == .named && s.store.unfinishedRestoreSnapshot?.id == a.id)
        #expect(s.store.takeReason == "A restore did not finish. Starting finishes it.")
        #expect(s.store.unfinishedRestoreText?.hasPrefix("Andarium could not finish restoring") == true)
    }

    @Test func aRestoreOrDeleteConfirmedAfterTheGuestStartedIsRefusedAndChangesNothing() async {
        let a = SnapshotSample.item("a", ago: 1)
        let b = SnapshotSample.item("b", ago: 2)
        let s = setup([a, b])
        await s.store.reload()
        // Both alerts are up, and the guest starts before the person answers.
        s.store.restoreTarget = a.id
        s.store.deleteTarget = b.id
        s.model.phase = .running(.normal)
        s.driver.gate = .guestNotStopped
        s.driver.setRuntimeActive(true)
        #expect(s.model.snapshotGate == .guestNotStopped)
        await s.store.restore(a.id)
        #expect(s.store.problem?.title == "The snapshot could not be restored")
        #expect(s.store.problem?.message == SnapshotWords.refusal(.guestNotStopped))
        #expect(s.model.restoredSnapshot == nil && s.model.sheet == .snapshots, "the sheet stays")
        s.store.problem = nil
        await s.store.delete(b.id)
        #expect(s.store.problem?.title == "The snapshot could not be deleted")
        #expect(s.store.problem?.message == SnapshotWords.refusal(.guestNotStopped))
        #expect(s.driver.items.count == 2 && s.driver.log.isEmpty, "nothing was done")
    }

    @Test func homeSaysTheWayOutThroughASnapshotOnlyWhereThereIsOne() async {
        let s = setup([SnapshotSample.item("later", release: "20261110-los24-r22", restore: .refused(.newerAndroid))])
        await s.store.reload()
        // A snapshot this app cannot restore is no way out.
        #expect(s.store.blockedText(.olderAndroid) == StartBlock.olderAndroid.explanation)
        s.driver.items.append(SnapshotSample.item("then", release: "20261108-los24-r18"))
        s.driver.finishedElsewhere()
        #expect(await until { s.store.hasRestorableSnapshot })
        #expect(s.store.blockedText(.olderAndroid).contains("restore an older snapshot"))
        #expect(s.store.blockedText(.mismatch) == StartBlock.mismatch.explanation, "erasing is the only way out of the rest")
    }

    @Test func homeShowsTheUpdateCardUnlessARestoredSnapshotSaysTheSame() {
        let s = setup()
        #expect(s.model.updateNoticeIsShown)
        s.model.restoredSnapshot = RestoredSnapshotNotice(name: "a", created: Date())
        #expect(s.model.updateNoticeIsShown, "a snapshot that updates nothing leaves the card")
        s.model.restoredSnapshot = RestoredSnapshotNotice(name: "a", created: Date(), updatesTo: "r20")
        #expect(!s.model.updateNoticeIsShown)
    }

    @Test func homeShowsNoUpdateCardBesideAnUnfinishedRestoreThatSaysTheSame() async {
        let now = SnapshotSample.item("now")
        let older = SnapshotSample.item("then", release: "20261108-los24-r18")
        let s = setup([now, older])
        await s.store.reload()
        #expect(s.model.updateNoticeIsShown)
        // A restore of the older snapshot stands: its card says the next start updates it to r20.
        s.driver.unfinishedRestore = SnapshotUnfinishedRestore(snapshot: older.id)
        await s.store.reload(force: true)
        #expect(s.store.unfinishedRestoreText?.contains("The next start updates it to r20.") == true)
        #expect(s.store.unfinishedRestoreUpdates && !s.model.updateNoticeIsShown)
        // A restore whose card says nothing of an update leaves the update card alone: a snapshot
        // of this release, one that cannot be told, one that is gone.
        for unfinished in [
            SnapshotUnfinishedRestore(snapshot: now.id), SnapshotUnfinishedRestore(),
            SnapshotUnfinishedRestore(snapshot: SnapshotID()),
        ] {
            s.driver.unfinishedRestore = unfinished
            await s.store.reload(force: true)
            #expect(s.store.unfinishedRestoreText?.contains("updates") == false)
            #expect(s.model.updateNoticeIsShown)
        }
        // And the restored note of a finished restore still gives way.
        s.driver.unfinishedRestore = nil
        await s.store.reload(force: true)
        s.model.restoredSnapshot = RestoredSnapshotNotice(name: "then", created: older.created, updatesTo: "r20")
        #expect(!s.model.updateNoticeIsShown)
    }

    @Test func homeShowsTheUnfinishedRestoreCardWhileTheListingSaysOneStands() async {
        let a = SnapshotSample.item("a")
        let s = setup([a])
        await s.store.reload()
        #expect(s.store.unfinishedRestoreText == nil)
        s.driver.unfinishedRestore = SnapshotUnfinishedRestore(snapshot: a.id, lastFailure: SnapshotFailure(POSIXError(.ENOSPC)))
        await s.store.reload(force: true)
        #expect(s.store.unfinishedRestoreText?.hasPrefix("Andarium could not finish restoring") == true)
        #expect(s.store.unfinishedRestoreText?.hasSuffix("No space left on device.") == true)
        s.driver.unfinishedRestore = nil
        await s.store.reload(force: true)
        #expect(s.store.unfinishedRestoreText == nil)
    }

    @Test func aRestoreEndsTheUpdateThatDidNotFinish() async {
        let s = setup([SnapshotSample.item("a")])
        await s.store.reload()
        #expect(!s.store.endsUpdate)
        s.model.plan = .unfinishedUpdate(to: "20261109-los24-r20", canRetry: true, canRestore: true)
        #expect(s.store.endsUpdate)
        #expect(s.store.restoreNote(of: s.store.items[0]).hasSuffix("The unfinished system image update ends."))
    }

    // MARK: Delete and rename

    @Test func deletingTakesThePageAndMovesTheSelectionToItsNeighbour() async {
        let items = [SnapshotSample.item("a", ago: 1), SnapshotSample.item("b", ago: 2), SnapshotSample.item("c", ago: 3)]
        let s = setup(items)
        await s.store.reload()
        let ordered = s.store.items.map(\.id)
        s.store.selection = .snapshot(ordered[1])
        s.store.route = [.snapshot(ordered[1])]
        s.store.deleteTarget = ordered[1]
        #expect(s.store.item(s.store.deleteTarget) != nil && s.driver.log.isEmpty)
        await s.store.delete(ordered[1])
        #expect(s.driver.log == ["delete"])
        #expect(s.store.items.map(\.id) == [ordered[0], ordered[2]])
        #expect(s.store.route.isEmpty)
        #expect(s.store.selection == .snapshot(ordered[2]), "the one that took its place")
        // The last one goes: nothing is selected and the list says it is empty.
        await s.store.delete(ordered[0])
        await s.store.delete(ordered[2])
        #expect(s.store.count == 0 && s.store.selection == nil && s.store.listing != nil)
    }

    @Test func aNameIsSavedAMomentAfterTheLastKeyAndOnlyOnce() async {
        let item = SnapshotSample.item("")
        let s = setup([item])
        await s.store.reload()
        s.store.edit(name: "B", of: item.id)
        s.store.edit(name: "Be", of: item.id)
        s.store.edit(name: "Before", of: item.id)
        #expect(s.driver.log.isEmpty, "nothing is saved while typing")
        #expect(await until { s.driver.log == ["rename Before"] })
        #expect(await until { s.store.item(item.id)?.name == "Before" })
        // A page that goes saves what waits at once.
        s.store.edit(name: "Before r20", of: item.id)
        await s.store.flushName()
        #expect(s.driver.log == ["rename Before", "rename Before r20"])
        await s.store.flushName()
        #expect(s.driver.log.count == 2, "nothing waits any more")
    }

    @Test func deletingASnapshotDropsWhatWasTypedInItsPage() async {
        let gone = SnapshotSample.item("old", ago: 1)
        let s = setup([gone, SnapshotSample.item("other", ago: 2)])
        await s.store.reload()
        s.store.edit(name: "Not wanted", of: gone.id)
        await s.store.delete(gone.id)
        #expect(await until { s.store.count == 1 })
        try? await Task.sleep(for: .milliseconds(120))
        #expect(s.driver.log == ["delete"], "the typed name is not saved afterwards")
        #expect(s.store.problem == nil, "and nothing says the snapshot is not there any more")
    }

    @Test(arguments: [SnapshotWork.delete, .restore, .create])
    func aRenameUnderWayIsWaitedForAndNotAnsweredWithBusy(operation: SnapshotWork) async {
        let a = SnapshotSample.item("a", ago: 1)
        let b = SnapshotSample.item("b", ago: 2)
        let s = setup([a, b])
        s.model.sheet = .snapshots
        await s.store.reload()
        s.driver.holding = .rename
        // The name typed and saved by the page going away.
        s.store.edit(name: "Renamed", of: a.id)
        let saving = Task { await s.store.flushName() }
        #expect(await until { s.model.snapshotActivity.operation == .rename })
        // The operation is asked for while the rename is under way.
        let asked = Task {
            switch operation {
            case .delete: await s.store.delete(b.id)
            case .restore: await s.store.restore(b.id)
            default: await s.store.take(name: "x")
            }
        }
        try? await Task.sleep(for: .milliseconds(60))
        #expect(s.driver.log == ["rename Renamed"], "it waits for the rename")
        s.driver.release()
        await saving.value
        await asked.value
        #expect(s.store.problem == nil, "and is not refused as busy")
        #expect(s.driver.log.count == 2 && s.driver.log[0] == "rename Renamed")
    }

    @Test func renamesReachTheDriverInTheOrderTheyWereAsked() async {
        let item = SnapshotSample.item("old")
        let s = setup([item])
        await s.store.reload()
        s.driver.holding = .rename
        let first = Task { await s.store.rename(item.id, to: "first") }
        #expect(await until { s.model.snapshotActivity.operation == .rename })
        let second = Task { await s.store.rename(item.id, to: "second") }
        try? await Task.sleep(for: .milliseconds(60))
        #expect(s.driver.log == ["rename first"])
        s.driver.release()
        await first.value
        await second.value
        #expect(s.driver.log == ["rename first", "rename second"])
        #expect(s.store.item(item.id)?.name == "second")
    }

    @Test func renamingFromTheListsPromptStartsFromTheCurrentName() async {
        let item = SnapshotSample.item("old")
        let s = setup([item])
        await s.store.reload()
        s.store.requestRename(item.id)
        #expect(s.store.renaming == item.id && s.store.renameText == "old")
        await s.store.rename(item.id, to: "new")
        #expect(s.store.item(item.id)?.name == "new")
        // A name the store refuses is told, and the old one stays.
        await s.store.rename(item.id, to: String(repeating: "x", count: SnapshotRules.maximumNameLength + 1))
        #expect(s.store.problem?.message == SnapshotWords.refusal(.invalidName))
        #expect(s.store.item(item.id)?.name == "new")
    }

    // MARK: Busy states

    @Test func whileAnOperationIsUnderWayNothingElseIsOfferedAndStartIsIgnored() async {
        let item = SnapshotSample.item("a")
        let s = setup([item])
        await s.store.reload()
        s.driver.holding = .create
        let taking = Task { await s.store.take(name: "x") }
        #expect(await until { s.model.snapshotActivity.operation == .create })
        #expect(s.store.isWorking)
        #expect(s.model.snapshotGate == .busy)
        #expect(s.store.takeRefusal == .busy && s.store.takeReason == nil, "the button dims and says nothing")
        let current = s.store.items[0]
        #expect(s.store.restoreRefusal(of: current) == .busy)
        #expect(s.store.deleteRefusal(of: current) == .busy)
        s.model.start(.normal)
        #expect(s.model.phase == .home, "a start is ignored")
        s.driver.release()
        await taking.value
        #expect(!s.store.isWorking && s.store.takeRefusal == nil)
        #expect(s.store.count == 2)
    }

    @Test func aRenameUnderWayDoesNotDimTheScreensOrStopAStart() async {
        let item = SnapshotSample.item("a")
        let s = setup([item])
        await s.store.reload()
        s.driver.holding = .rename
        let renaming = Task { await s.store.rename(item.id, to: "b") }
        #expect(await until { s.model.snapshotActivity.operation == .rename })
        #expect(s.model.snapshotGate == nil)
        #expect(s.store.takeRefusal == nil && s.store.restoreRefusal(of: s.store.items[0]) == nil)
        #expect(s.store.deleteRefusal(of: s.store.items[0]) == nil)
        #expect(!s.store.isWorking)
        // The start goes ahead; the driver makes it wait for the rename.
        s.model.start(.normal)
        #expect(s.model.phase != .home)
        s.driver.release()
        await renaming.value
        #expect(s.store.problem == nil)
    }

    @Test func whileTheGuestRunsNothingIsOfferedAndTheWordsSayWhy() async {
        let item = SnapshotSample.item("a")
        let s = setup([item])
        // The model's gate follows the phase; the driver's refuses an operation that is asked for anyway.
        s.model.phase = .running(.normal)
        s.driver.gate = .guestNotStopped
        await s.store.reload()
        #expect(s.store.takeRefusal == .guestNotStopped)
        #expect(s.store.takeReason == "Not while the virtual machine is starting or running.")
        #expect(s.store.restoreRefusal(of: item) == .guestNotStopped)
        #expect(s.store.restoreNote(of: item) == "Not while the virtual machine is starting or running.")
        #expect(s.store.deleteRefusal(of: item) == .guestNotStopped)
        // What is wrong with the snapshot itself comes before what passes.
        let newer = SnapshotSample.item("later", release: "20261110-los24-r22", restore: .refused(.newerAndroid))
        #expect(s.store.restoreRefusal(of: newer) == .newerAndroid)
    }

    @Test func anUnfinishedUpdateAndAnOlderGuestAreWaysOutThroughASnapshot() async {
        let older = SnapshotSample.item("then", release: "20261108-los24-r18")
        let newer = SnapshotSample.item("later", release: "20261110-los24-r22", restore: .refused(.newerAndroid))
        let s = setup([newer])
        await s.store.reload()
        #expect(!s.store.hasRestorableSnapshot, "only one this app cannot restore")
        s.driver.items.append(older)
        s.driver.finishedElsewhere()
        #expect(await until { s.store.hasRestorableSnapshot })
    }
}
