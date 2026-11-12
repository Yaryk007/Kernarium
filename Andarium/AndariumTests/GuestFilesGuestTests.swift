// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import CryptoKit
import Foundation
import Testing
@testable import Andarium

/// The file browser on a real guest, through the production model, backend and
/// adb channel (`GuestFilesTests` in AndariumCore has the protocol against a
/// fake adbd). Runs alone with ANDARIUM_FILES_TESTS=1 and
/// ANDARIUM_GUEST_INSTANCE=files, on a Mac (Designed for iPad: the Simulator
/// has no Android guest). The large-file test also needs
/// ANDARIUM_FILES_BIG=1 and a guest disk of 12 GiB or more
/// (KERNARIUM_ANDROID_DISK_SIZE_GIB), and runs in a guest of its own. The
/// storage-readiness test runs alone with ANDARIUM_FILES_STORAGE=1 and
/// ANDARIUM_GUEST_INSTANCE=filex-store (a first boot and a restart in place).
/// The background test needs a device: it runs alone on an iPad with
/// ANDARIUM_FILES_BACKGROUND=1 and ANDARIUM_GUEST_INSTANCE=filex-bg, and a
/// harness that sends the application to the background and back when the
/// test says so (the lines marked `FILEX-BG:`, also kept in the
/// application's `Documents/filex-bg.log`).
///
/// Every file the tests make is in a folder of their own in Download, which the
/// guest cleanup removes, and in the temporary directory of the host.
@MainActor
struct GuestFilesGuestTests {
    private static let environment = ProcessInfo.processInfo.environment
    private static let enabled = environment["ANDARIUM_FILES_TESTS"] == "1"
    private static let bigEnabled = enabled && environment["ANDARIUM_FILES_BIG"] == "1"
    private static let storageEnabled = enabled && environment["ANDARIUM_FILES_STORAGE"] == "1"
    private static let backgroundEnabled = enabled && environment["ANDARIUM_FILES_BACKGROUND"] == "1"
    /// A rehearsal on a Mac, where nothing sends the application away: the
    /// test stops the transfers the way the notification does.
    private static let simulatesBackground = environment["ANDARIUM_FILES_BACKGROUND_SIMULATE"] == "1"
    private static let folder = GuestPath.child("FilexTest", of: GuestPath.child("Download", of: GuestPath.root))
    private static let cleanup = "rm -rf '\(folder)' /storage/emulated/0/Pictures/fx_*; su 0 rm -rf /data/media/0/FilexRefused"
    private let mib = Limits.bytesPerMiB

    // MARK: Small flows

    @Test(.timeLimit(.minutes(40)))
    func browsingImportingAndExportingOnARealGuest() async throws {
        guard Self.enabled, !Self.bigEnabled, !Self.storageEnabled, !Self.backgroundEnabled else { return }
        try #require(Self.environment["ANDARIUM_GUEST_INSTANCE"] == "files")
        try await withAndroidGuest(cleanupCommand: Self.cleanup) { model, operations in
            let browser = try #require(model.fileBrowser)
            let bench = try Bench()
            defer { bench.remove() }
            let guest = GuestSide(operations)
            let work = Self.folder
            #expect(await guest.waitForStorage(), "internal storage did not come up")
            let watcher = try await openSettingsOnTheFiles(model, operations)
            defer { watcher.cancel() }
            _ = try await guest.run("rm -rf '\(work)'; mkdir -p '\(work)'")
            browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: work)]

            // MARK: Browsing
            await browser.load(GuestPath.root)
            guard case let .loaded(top)? = browser.listing(GuestPath.root) else {
                Issue.record("internal storage did not list: \(String(describing: browser.listing(GuestPath.root)))")
                return
            }
            let names = Set(top.map(\.name))
            note("internal storage: \(names.sorted())")
            #expect(names.isSuperset(of: ["Download", "DCIM", "Pictures", "Music"]))
            #expect(top.allSatisfy { !$0.name.hasPrefix(".") }, "hidden entries are not listed")
            #expect(top.filter { ["Download", "DCIM"].contains($0.name) }.allSatisfy { $0.isFolder })
            #expect((browser.freeBytes ?? 0) > 0)

            // An empty folder is empty, a missing one says so, and a folder
            // Android refuses is told from an empty one.
            _ = try await guest.run("mkdir -p '\(work)/Empty'")
            await browser.load(work + "/Empty")
            #expect(browser.listing(work + "/Empty") == .loaded([]))
            await browser.load(work + "/Nothing here")
            if case .failed? = browser.listing(work + "/Nothing here") {} else {
                Issue.record("a missing folder: \(String(describing: browser.listing(work + "/Nothing here")))")
            }
            // adbd lists a folder the shell cannot open as an empty one, which is
            // why a listing with nothing in it is asked of the shell's ls.
            let denied = try await guest.run("ls -A /data/data; echo rc=$?")
            note("ls /data/data as the shell: \(denied.text) \(String(decoding: denied.stderr, as: UTF8.self))")
            let raw = try await AdbOperationRunner.run(
                identity: operations.identity, clock: operations.clock, connector: operations.connector
            ) { session in
                try session.run { connection in
                    session.watchdog.arm(idle: .seconds(30), overall: .seconds(60))
                    let sync = try AdbSync(connection: connection)
                    defer { sync.quit() }
                    return try sync.list("/data/data").map(\.name)
                }
            }
            note("adbd lists /data/data as \(raw)")
            #expect(raw.filter { $0 != "." && $0 != ".." }.isEmpty, "adbd answers a refused folder with no entries")
            #expect(denied.text.contains("rc=1") || denied.text.contains("rc=2") || !denied.stderr.isEmpty, "the shell refuses it")
            // Inside internal storage: a folder made unreadable, if the storage
            // emulation lets a mode be set at all.
            let attempt = try await guest.run(
                "mkdir '\(work)/Locked'; chmod 000 '\(work)/Locked'; echo chmod=$?; ls -A '\(work)/Locked'; echo ls=$?")
            note("a folder made unreadable in internal storage: \(attempt.text) \(String(decoding: attempt.stderr, as: UTF8.self))")
            await browser.load(work + "/Locked")
            note("it lists as: \(String(describing: browser.listing(work + "/Locked")))")
            if attempt.text.contains("ls=0") {
                #expect(browser.listing(work + "/Locked") == .loaded([]))
            } else {
                #expect(browser.listing(work + "/Locked") == .refused)
            }
            _ = try await guest.run("chmod 700 '\(work)/Locked'; rmdir '\(work)/Locked'")

            // MARK: Import
            let day: TimeInterval = 86_400
            let picks = [
                try bench.pick("It's a note (1).txt", bytes: 4_096, seed: 1, modified: Date(timeIntervalSince1970: 1_577_966_400)),
                try bench.jpeg("fx_photo.jpg", modified: Date(timeIntervalSince1970: 1_609_545_600)),
                try bench.pick("blob.bin", bytes: 3 * Int(mib) + 17, seed: 2, modified: Date(timeIntervalSince1970: 1_640_000_000 + day)),
            ]
            await browser.load(work)
            browser.received(picks)
            var transfer = try #require(browser.transfer)
            #expect(browser.busy)
            // The guest's other operations wait for it.
            let busyBefore = model.busy
            model.createBugReport()
            #expect(model.busy == busyBefore, "a bug report does not start during a transfer")
            try await settle(transfer)
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            #expect(transfer.items.allSatisfy { $0.state == .done && $0.moved == $0.size })
            for pick in picks {
                let path = GuestPath.child(pick.name, of: work)
                #expect(try await guest.sha256(path) == bench.sha256(pick), "\(pick.name) arrived whole")
                #expect(try await guest.size(path) == pick.size)
                let modified = try await guest.modified(path)
                #expect(modified == Int(pick.modified.timeIntervalSince1970), "\(pick.name) keeps its date: \(modified)")
            }
            let afterImport = try await guest.names(work)
            #expect(!afterImport.contains(where: GuestFiles.isPartial), "\(afterImport)")
            #expect(Set(afterImport) == ["It's a note (1).txt", "fx_photo.jpg", "blob.bin", "Empty"], "\(afterImport)")
            await browser.load(work)
            #expect(Set(browser.entries(in: work).map(\.name)) == Set(afterImport))
            // Every import, of whatever type, has a finished row: the guest's
            // apps see only those. On a guest that has just booted the media
            // provider's first scan of storage can complete a row itself, so
            // the imports made later (Replace, Skip) are the ones that tell.
            let indexedAll = await guest.waitFor(guest.finishedRows(in: "Download/FilexTest/"), contains: "_display_name=blob.bin, _size=\(3 * Int(mib) + 17)", seconds: 30)
            let finished = try await guest.run(guest.finishedRows(in: "Download/FilexTest/")).text
            note("rows of the imports: \(finished)")
            #expect(indexedAll, "the media index has no finished row for blob.bin: \(finished)")
            for name in ["It's a note (1).txt", "fx_photo.jpg", "blob.bin"] {
                #expect(finished.contains("_display_name=\(name),"), "no finished row for \(name): \(finished)")
            }
            // The gallery's index knows the photo, and the Files app lists it.
            let indexed = await guest.waitFor(
                "content query --uri content://media/external/images/media --projection _display_name:_size --where \"_display_name='fx_photo.jpg'\"",
                contains: "_display_name=fx_photo.jpg", seconds: 30)
            #expect(indexed, "the media index has no row for the imported photo")
            let shown = await guest.filesAppShows(folder: "Download/FilexTest", names: ["fx_photo.jpg", "blob.bin"])
            note("the Files app lists the imports: \(shown)")
            #expect(shown, "the guest's Files app does not list the imported files")

            // MARK: Keep Both
            let again = [
                try bench.pick("It's a note (1).txt", bytes: 5_000, seed: 11, modified: .now),
                try bench.pick("fx_new.txt", bytes: 100, seed: 12, modified: .now),
            ]
            browser.received(again)
            let conflict = try #require(browser.conflict, "a name that is taken asks")
            #expect(conflict.clashing.map(\.name) == ["It's a note (1).txt"])
            browser.resolveConflict(.keepBoth)
            transfer = try #require(browser.transfer)
            try await settle(transfer)
            #expect(transfer.stage == .finished)
            let copy = GuestFileNames.freeName(for: "It's a note (1).txt", taken: Set(afterImport))
            let original = GuestPath.child("It's a note (1).txt", of: work)
            #expect(try await guest.sha256(original) == bench.sha256(picks[0]), "Keep Both leaves the first file")
            #expect(try await guest.sha256(GuestPath.child(copy, of: work)) == bench.sha256(again[0]), "the copy is \(copy)")
            #expect(try await guest.sha256(GuestPath.child("fx_new.txt", of: work)) == bench.sha256(again[1]))

            // MARK: Replace
            let replacement = try bench.pick("blob.bin", bytes: 2 * Int(mib) + 5, seed: 21, modified: Date(timeIntervalSince1970: 1_700_000_000))
            _ = try await guest.run("mkdir -p '\(work)/Clash'; echo keep > '\(work)/Clash/inside.txt'")
            await browser.load(work)
            let overFolder = try bench.pick("Clash", bytes: 64, seed: 22, modified: .now)
            browser.received([replacement, overFolder])
            let replaceConflict = try #require(browser.conflict)
            #expect(Set(replaceConflict.clashing.map(\.name)) == ["blob.bin", "Clash"])
            #expect(replaceConflict.folders.contains("Clash") && !replaceConflict.folders.contains("blob.bin"))
            browser.resolveConflict(.replace)
            transfer = try #require(browser.transfer)
            try await settle(transfer)
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            let blob = GuestPath.child("blob.bin", of: work)
            #expect(try await guest.sha256(blob) == bench.sha256(replacement), "Replace puts the new file in place")
            #expect(try await guest.size(blob) == replacement.size)
            // The replaced file's row is finished again, at the new size.
            let replaced = await guest.waitFor(
                guest.finishedRows(in: "Download/FilexTest/"), contains: "_display_name=blob.bin, _size=\(replacement.size)", seconds: 30)
            #expect(replaced, "the replaced file's row did not come back finished")
            #expect(try await guest.run("cat '\(work)/Clash/inside.txt'").text == "keep\n", "a folder is never replaced")
            #expect(try await guest.sha256(GuestPath.child("Clash (1)", of: work)) == bench.sha256(overFolder), "the file goes beside the folder")
            await guest.expectNoPartials(work, "after Replace")

            // MARK: Skip
            browser.received([try bench.pick("fx_new.txt", bytes: 7, seed: 31, modified: .now), try bench.pick("fx_skip.txt", bytes: 7, seed: 32, modified: .now)])
            #expect(browser.conflict?.clashing.map(\.name) == ["fx_new.txt"])
            browser.resolveConflict(.skip)
            transfer = try #require(browser.transfer)
            try await settle(transfer)
            #expect(transfer.items.map(\.name) == ["fx_skip.txt"])
            #expect(try await guest.names(work).contains("fx_skip.txt"))
            let skipped = await guest.waitFor(
                guest.finishedRows(in: "Download/FilexTest/"), contains: "_display_name=fx_skip.txt, _size=7", seconds: 30)
            #expect(skipped, "the row of the file imported last is not finished")
            #expect(try await guest.sha256(GuestPath.child("fx_new.txt", of: work)) == bench.sha256(again[1]), "Skip leaves the file alone")

            // MARK: Cancel an import
            let large = try bench.pick("cancel me.bin", bytes: 700 * Int(mib), seed: 41, modified: .now)
            browser.received([large])
            transfer = try #require(browser.transfer)
            try await waitUntil("the import has moved 64 MiB", seconds: 120) { transfer.items[0].moved >= 64 * mib || !transfer.running }
            #expect(transfer.running, "the import ran for long enough to cancel: \(transfer.detail)")
            let whileMoving = try await guest.names(work)
            #expect(whileMoving.contains(where: GuestFiles.isPartial), "the partial file exists while it moves: \(whileMoving)")
            browser.closeTransfer()
            #expect(browser.transfer == nil)
            await guest.expectNoPartials(work, "after Cancel")
            let afterCancel = try await guest.names(work)
            #expect(!afterCancel.contains("cancel me.bin"), "Cancel leaves no file of that name")

            // MARK: Cancel a Replace: the original stays whole
            let before = try await guest.sha256(blob)
            let heavy = try bench.pick("blob.bin", bytes: 700 * Int(mib), seed: 42, modified: .now)
            await browser.load(work)
            browser.received([heavy])
            browser.resolveConflict(.replace)
            transfer = try #require(browser.transfer)
            try await waitUntil("the replacement has moved 64 MiB", seconds: 120) { transfer.items[0].moved >= 64 * mib || !transfer.running }
            #expect(transfer.running)
            browser.closeTransfer()
            await guest.expectNoPartials(work, "after cancelling a Replace")
            #expect(try await guest.sha256(blob) == before, "the file Replace would have replaced is whole")

            // MARK: Leaving the foreground
            browser.received([large])
            transfer = try #require(browser.transfer)
            try await waitUntil("the import has moved 64 MiB", seconds: 120) { transfer.items[0].moved >= 64 * mib || !transfer.running }
            #expect(transfer.running)
            browser.leftForeground()
            try await waitUntil("the transfer stops", seconds: 60) { !transfer.running }
            #expect(transfer.stage == .failed)
            #expect(transfer.items[0].state == .failed(FileBrowserModel.leftForegroundReason), "\(transfer.items[0].state)")
            await guest.expectNoPartials(work, "after leaving the foreground")
            let afterStop = try await guest.names(work)
            #expect(!afterStop.contains("cancel me.bin"), "\(afterStop)")
            // Try Again carries it out.
            browser.retryFailed()
            try await settle(transfer)
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            #expect(try await guest.sha256(GuestPath.child("cancel me.bin", of: work)) == bench.sha256(large))
            browser.closeTransfer()

            // MARK: Export
            _ = try await guest.run("""
                mkdir -p '\(work)/Tree/a/b' '\(work)/Tree/empty'; \
                printf 'one' > '\(work)/Tree/one.txt'; printf 'two two' > '\(work)/Tree/a/two.txt'; \
                head -c 300000 /dev/urandom > '\(work)/Tree/a/b/three.bin'; touch -d 2021-03-04 '\(work)/Tree/one.txt'
                """)
            await browser.load(work)
            let selection = browser.entries(in: work).filter { ["blob.bin", "It's a note (1).txt", "Tree"].contains($0.name) }
            #expect(selection.count == 3)
            browser.selection = selection
            var shared: [URL] = []
            var finishShare: (@MainActor (Bool) -> Void)?
            browser.presentShare = { files, finished in
                shared = files
                finishShare = finished
                return true
            }
            browser.exportSelection()
            transfer = try #require(browser.transfer)
            try await waitUntil("the export is ready", seconds: 180) { transfer.stage != .running }
            #expect(transfer.stage == .ready, "\(transfer.title) \(transfer.detail)")
            #expect(Set(shared.map(\.lastPathComponent)) == ["blob.bin", "It's a note (1).txt", "Tree"], "\(shared)")
            for url in shared where url.lastPathComponent != "Tree" {
                let remote = GuestPath.child(url.lastPathComponent, of: work)
                let remoteHash = try await guest.sha256(remote)
                #expect(try Bench.sha256(of: url) == remoteHash, "\(url.lastPathComponent) arrived whole")
            }
            let tree = try #require(shared.first { $0.lastPathComponent == "Tree" })
            let listing = try Bench.tree(tree)
            let remoteTree = try await guest.run("cd '\(work)/Tree' && find . | sort").text.split(separator: "\n").map(String.init).filter { $0 != "." }
            #expect(listing == remoteTree.map { String($0.dropFirst(2)) }, "the folder arrives as it is: \(listing) / \(remoteTree)")
            let threeHash = try await guest.sha256(work + "/Tree/a/b/three.bin")
            #expect(try Bench.sha256(of: tree.appendingPathComponent("a/b/three.bin")) == threeHash)
            let oneDate = try FileManager.default.attributesOfItem(atPath: tree.appendingPathComponent("one.txt").path)[.modificationDate] as? Date
            #expect(oneDate.map { Calendar.current.isDate($0, inSameDayAs: Date(timeIntervalSince1970: 1_614_816_000)) } == true, "\(String(describing: oneDate))")
            // The action menu ends it: the copies go.
            let staging = try #require(transfer.directory)
            finishShare?(true)
            #expect(transfer.stage == .finished)
            #expect(!FileManager.default.fileExists(atPath: staging.path), "the staged copies are removed")

            // MARK: Cancel an export
            _ = try await guest.run("dd if=/dev/urandom of='\(work)/pull.bin' bs=1048576 count=300 2>&1 | tail -1")
            await browser.load(work)
            let pull = try #require(browser.entries(in: work).first { $0.name == "pull.bin" })
            browser.export([pull])
            transfer = try #require(browser.transfer)
            try await waitUntil("the export has moved 16 MiB", seconds: 120) { transfer.items[0].moved >= 16 * mib || !transfer.running }
            #expect(transfer.running)
            let staged = try #require(transfer.directory)
            browser.closeTransfer()
            #expect(!FileManager.default.fileExists(atPath: staged.path), "Cancel removes the partial copy")

            // An export interrupted by the foreground leaves nothing either, and runs again.
            browser.export([pull])
            transfer = try #require(browser.transfer)
            try await waitUntil("the export has moved 16 MiB", seconds: 120) { transfer.items[0].moved >= 16 * mib || !transfer.running }
            browser.leftForeground()
            try await waitUntil("the export stops", seconds: 60) { !transfer.running }
            #expect(transfer.stage == .failed)
            #expect(transfer.items[0].state == .failed(FileBrowserModel.leftForegroundReason))
            browser.retryFailed()
            try await waitUntil("the export is ready again", seconds: 240) { transfer.stage != .running }
            #expect(transfer.stage == .ready, "\(transfer.title) \(transfer.detail)")
            let pulledHash = try await guest.sha256(work + "/pull.bin")
            #expect(try Bench.sha256(of: try #require(transfer.staged.first)) == pulledHash)
            browser.closeTransfer()

            // MARK: USB debugging
            // The channel is the application's own and works either way, so the
            // browser does not follow the setting: it lists, imports and exports
            // with USB debugging off, and ADB over network alone still needs it.
            // Turning it off restarts adbd; turning it on does not.
            func setUsbDebugging(_ on: Bool) async throws {
                _ = try? await operations.shell(
                    "settings put global adb_enabled \(on ? 1 : 0); settings put global adb_wifi_enabled 0", limit: .seconds(30))
                try await waitUntil("USB debugging reads as \(on ? "on" : "off")", seconds: 90) {
                    await model.refreshUsbDebugging()
                    return model.session.usbDebugging == on
                }
            }
            func adbdPid() async -> String {
                ((try? await guest.run("pidof adbd", seconds: 30).text) ?? "?").trimmingCharacters(in: .whitespacesAndNewlines)
            }
            try await setUsbDebugging(true)
            #expect(model.filesAvailable && model.adbOverNetworkAvailable)

            // Turning it off restarts adbd under an import in flight. The import
            // fails in the words of AdbFailure and Try Again carries it out.
            let flight = try bench.pick("flight.bin", bytes: 700 * Int(mib), seed: 62, modified: .now)
            await browser.load(work)
            browser.received([flight])
            transfer = try #require(browser.transfer)
            try await waitUntil("the import has moved 32 MiB", seconds: 120) { transfer.items[0].moved >= 32 * mib || !transfer.running }
            #expect(transfer.running, "the import ran for long enough to be caught: \(transfer.detail)")
            let adbdBefore = await adbdPid()
            try await setUsbDebugging(false)
            try await waitUntil("the import ends", seconds: 300) { !transfer.running }
            note("turning USB debugging off: adbd \(adbdBefore) -> \(await adbdPid())")
            if case let .failed(reason) = transfer.items[0].state {
                note("the import under the restart failed: \(reason)")
                #expect(reason.contains("debugging service restarted"), "the failure is told in AdbFailure's words: \(reason)")
                var attempts = 0
                while transfer.stage != .finished, attempts < 6 {
                    attempts += 1
                    browser.retryFailed()
                    try await settle(transfer, seconds: 300)
                    if transfer.stage != .finished { try await Task.sleep(for: .seconds(5)) }
                }
                note("Try Again took \(attempts) attempt(s)")
            } else {
                note("the import was not interrupted by the restart")
            }
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            #expect(try await guest.sha256(GuestPath.child(flight.name, of: work)) == bench.sha256(flight), "the import is whole")
            await guest.expectNoPartials(work, "after the restart")
            browser.closeTransfer()

            // With USB debugging off, still: list, import and export.
            #expect(model.filesAvailable, "the browser is available with USB debugging off")
            #expect(!model.adbOverNetworkAvailable, "ADB over network still needs USB debugging")
            await browser.load(work)
            let offFolder = browser.entries(in: work)
            #expect(offFolder.contains { $0.name == "blob.bin" }, "it lists with USB debugging off: \(offFolder.map(\.name))")
            let offPick = try bench.pick("usb off.txt", bytes: 10_000, seed: 61, modified: Date(timeIntervalSince1970: 1_650_000_000))
            browser.received([offPick])
            transfer = try #require(browser.transfer)
            try await settle(transfer, seconds: 120)
            #expect(transfer.stage == .finished, "import, USB debugging off: \(transfer.title) \(transfer.detail)")
            #expect(try await guest.sha256(GuestPath.child(offPick.name, of: work)) == bench.sha256(offPick))
            browser.closeTransfer()
            let offEntry = try #require(browser.entries(in: work).first { $0.name == "blob.bin" })
            browser.export([offEntry])
            transfer = try #require(browser.transfer)
            try await waitUntil("the export is ready", seconds: 180) { transfer.stage != .running }
            #expect(transfer.stage == .ready, "export, USB debugging off: \(transfer.title) \(transfer.detail)")
            let offHash = try await guest.sha256(offEntry.path)
            #expect(try Bench.sha256(of: try #require(transfer.staged.first)) == offHash)
            browser.closeTransfer()
            note("USB debugging off: listed, imported and exported")

            // Turned back on, it works as it did.
            try await setUsbDebugging(true)
            #expect(model.filesAvailable && model.adbOverNetworkAvailable)
            await browser.load(work)
            #expect(browser.entries(in: work).contains { $0.name == "flight.bin" })
            let onEntry = try #require(browser.entries(in: work).first { $0.name == "usb off.txt" })
            browser.export([onEntry])
            transfer = try #require(browser.transfer)
            try await waitUntil("the export is ready", seconds: 180) { transfer.stage != .running }
            #expect(transfer.stage == .ready, "export, USB debugging on: \(transfer.title) \(transfer.detail)")
            browser.closeTransfer()
            browser.sheetClosed()
        }
    }

    // MARK: Storage readiness across boots

    /// Android reports its boot done before internal storage is mounted. The
    /// application's readiness check must never say yes while a write still
    /// fails or the standard folders are missing, must say yes soon after they
    /// are there, and the browser must follow without being reopened, on a
    /// first boot and on a boot Android restarts in place.
    @Test(.timeLimit(.minutes(30)))
    func theBrowserWaitsForStorageThroughABootAndARestart() async throws {
        guard Self.storageEnabled else { return }
        try #require(Self.environment["ANDARIUM_GUEST_INSTANCE"] == "filex-store")
        try await withAndroidGuest(cleanupCommand: Self.cleanup) { model, operations in
            let browser = try #require(model.fileBrowser)
            let bench = try Bench()
            defer { bench.remove() }
            let guest = GuestSide(operations)
            // The wizard would take the Files app's place; nothing else needs it.
            _ = try? await guest.run("pm disable-user --user 0 org.lineageos.setupwizard")
            // The check's two ways to be ready, on the guest's own shell: a guest
            // whose standard folders a person deleted is ready by its uptime, and
            // one that has neither is not.
            let noFolders = InternalStorage.command.replacingOccurrences(of: "$r/", with: "$r/none-")
            let byUptime = try await guest.run(noFolders.replacingOccurrences(of: "-gt 300", with: "-gt 0"))
            let neither = try await guest.run(noFolders.replacingOccurrences(of: "-gt 300", with: "-gt 999999"))
            #expect(InternalStorage.parse(byUptime) == true, "\(byUptime.text)")
            #expect(InternalStorage.parse(neither) == false, "\(neither.text)")
            // Settings is open: what it does while it is, every few seconds.
            let watcher = Task { await model.watchSettings() }
            defer { watcher.cancel() }

            for boot in ["boot", "restart in place"] {
                if boot == "restart in place" {
                    let before = (try? await guest.run("cut -d' ' -f1 /proc/uptime").text) ?? "?"
                    note("\(boot): rebooting at up=\(before.trimmingCharacters(in: .whitespacesAndNewlines))")
                    _ = try? await guest.run("reboot", seconds: 20)
                    var sawRestart = false
                    try await waitUntil("Android restarts in place", seconds: 180) {
                        if case .booting = model.phase { sawRestart = true }
                        return sawRestart
                    }
                    #expect(model.session.storageReady == nil, "a restart forgets the storage was up")
                    #expect(!model.filesAvailable)
                    try await waitUntil("the restarted Android finishes booting", seconds: 600) {
                        guard let up = try? await guest.run("cut -d' ' -f1 /proc/uptime", seconds: 15).text,
                              let seconds = Double(up.trimmingCharacters(in: .whitespacesAndNewlines)),
                              seconds < 150,
                              let done = try? await guest.run("getprop sys.boot_completed", seconds: 15).text
                        else { return false }
                        return done.trimmingCharacters(in: .whitespacesAndNewlines) == "1"
                    }
                }

                let began = ContinuousClock.now
                var firstReady: Duration?
                var firstAvailable: Duration?
                var firstWrite: Duration?
                var early = 0
                var readings: [Bool?] = []
                var wasUp = false
                var flippedBack = false
                var unavailableTransfers = 0
                while firstAvailable == nil, ContinuousClock.now - began < .seconds(240) {
                    let at = ContinuousClock.now - began
                    let reported = await operations.storageReady()
                    readings.append(reported)
                    let probe = try? await guest.run(
                        """
                        w=no; mkdir /storage/emulated/0/.fxprobe 2>/dev/null && { rmdir /storage/emulated/0/.fxprobe; w=yes; }
                        echo "up=$(cut -d' ' -f1 /proc/uptime) write=$w entries=$(ls -A /storage/emulated/0 2>/dev/null | wc -l) \
                        folders=$(for d in Download DCIM Pictures; do [ -d /storage/emulated/0/$d ] && printf y || printf n; done)"
                        """, seconds: 20).text
                    let wrote = probe?.contains("write=yes") == true
                    let folders = probe.map { !$0.contains("folders=nnn") } ?? false
                    if wrote, firstWrite == nil { firstWrite = at }
                    if reported == true {
                        if firstReady == nil { firstReady = at }
                        wasUp = true
                        // The guarantee: ready means a write works and the media provider has made folders.
                        if !(wrote && folders) { early += 1 }
                    } else if reported == false, wasUp {
                        flippedBack = true
                    }
                    note("\(boot): t=\(at) ready=\(String(describing: reported)) \(probe?.replacingOccurrences(of: "\n", with: " ") ?? "no answer") "
                        + "model: storage=\(String(describing: model.session.storageReady)) available=\(model.filesAvailable)")
                    // Until the model says it is ready nothing is imported or exported.
                    if !model.filesAvailable {
                        browser.received([try bench.pick("fx_early.txt", bytes: 64, seed: 7, modified: .now)])
                        if browser.transfer != nil || browser.conflict != nil { unavailableTransfers += 1 }
                    } else {
                        firstAvailable = at
                    }
                    try await Task.sleep(for: .seconds(1))
                }
                note("\(boot): first write ok at \(String(describing: firstWrite)), reported ready at \(String(describing: firstReady)), "
                    + "model available at \(String(describing: firstAvailable))")
                #expect(early == 0, "\(boot): the check said ready while a write failed or no standard folder was there")
                #expect(!flippedBack, "\(boot): once ready, always ready")
                #expect(unavailableTransfers == 0, "\(boot): a transfer started while the storage was not up")
                #expect(firstReady != nil && firstAvailable != nil, "\(boot): the storage never read as ready")
                // A boot whose first reading comes after the mount has no window to see.
                note("\(boot): the starting window was \(readings.contains { $0 == false } ? "observed" : "over before the first reading")")
                if let firstReady, let firstAvailable {
                    #expect(firstAvailable - firstReady < .seconds(10), "\(boot): the browser follows within a few seconds")
                }
                if let firstWrite, let firstReady {
                    #expect(firstReady - firstWrite < .seconds(30), "\(boot): ready soon after writes work")
                }

                // The same browser, never reopened, now lists and takes a file.
                let work = Self.folder
                _ = try await guest.run("rm -rf '\(work)'; mkdir -p '\(work)'")
                browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: work)]
                await browser.load(GuestPath.root)
                guard case let .loaded(top)? = browser.listing(GuestPath.root) else {
                    Issue.record("\(boot): internal storage did not list: \(String(describing: browser.listing(GuestPath.root)))")
                    return
                }
                #expect(Set(top.map(\.name)).isSuperset(of: ["Download", "DCIM", "Pictures", "Music"]), "\(boot): \(top.map(\.name))")
                await browser.load(work)
                let pick = try bench.pick("fx_\(boot.prefix(5)).txt", bytes: 2_048, seed: 9, modified: Date(timeIntervalSince1970: 1_600_000_000))
                browser.received([pick])
                let transfer = try #require(browser.transfer, "\(boot): the import started")
                try await settle(transfer, seconds: 120)
                #expect(transfer.stage == .finished, "\(boot): \(transfer.title) \(transfer.detail)")
                #expect(try await guest.sha256(GuestPath.child(pick.name, of: work)) == bench.sha256(pick))
                browser.closeTransfer()
                browser.sheetClosed()
            }
        }
    }

    // MARK: The application in the background

    /// The harness's cue: the test is mid-transfer and waits for the
    /// application to be sent to the background and brought back. The line is
    /// on the console and in `Documents/filex-bg.log`.
    private static func cue(_ text: String) {
        print("FILEX-BG: \(text)")
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
        let url = documents.appendingPathComponent("filex-bg.log")
        let line = Data("\(Date().timeIntervalSince1970) \(text)\n".utf8)
        if let handle = try? FileHandle(forWritingTo: url) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: line)
        } else {
            try? line.write(to: url)
        }
    }

    /// iOS stops a guest's application in the background, and a transfer with
    /// it. On a device, with the harness sending the application away and back
    /// in the middle of an import and again of an export: the files say why
    /// they stopped, no partial file is left in the guest or in the staged
    /// copies, and Try Again carries each one out whole.
    @Test(.timeLimit(.minutes(25)))
    func sendingTheApplicationToTheBackgroundMidTransferStopsItCleanly() async throws {
        guard Self.backgroundEnabled else { return }
        try #require(Self.environment["ANDARIUM_GUEST_INSTANCE"] == "filex-bg")
        // A folder of its own for each run: a run that was cut off leaves its files behind.
        let work = GuestPath.child("FilexBg-\(Int(Date().timeIntervalSince1970))", of: GuestPath.child("Download", of: GuestPath.root))
        try await withAndroidGuest(cleanupCommand: "\(Self.cleanup); rm -rf '\(work)'") { model, operations in
            let browser = try #require(model.fileBrowser)
            let bench = try Bench()
            defer { bench.remove() }
            let guest = GuestSide(operations)
            _ = try? await guest.run("pm disable-user --user 0 org.lineageos.setupwizard")
            _ = try await guest.run("settings put global adb_enabled 1")
            let watcher = Task { await model.watchSettings() }
            defer { watcher.cancel() }
            try await waitUntil("the browser is available", seconds: 240) { model.filesAvailable }
            _ = try await guest.run("rm -rf '\(work)'; mkdir -p '\(work)'")
            let leftovers = try await guest.names(work)
            #expect(leftovers.isEmpty, "a new folder is empty: \(leftovers)")
            browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: work)]
            await browser.load(work)
            var shared: [URL] = []
            var finishShare: (@MainActor (Bool) -> Void)?
            browser.presentShare = { files, finished in
                shared = files
                finishShare = finished
                return true
            }
            // The file keeps the cues of this run only.
            if let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
                try? FileManager.default.removeItem(at: documents.appendingPathComponent("filex-bg.log"))
            }
            Self.cue("ready")

            // MARK: A small import and export, in the foreground
            let small = [
                try bench.pick("small one.txt", bytes: 3_000, seed: 3, modified: Date(timeIntervalSince1970: 1_600_000_000)),
                try bench.pick("small two.bin", bytes: 2 * Int(mib) + 5, seed: 4, modified: Date(timeIntervalSince1970: 1_600_100_000)),
            ]
            browser.received(small)
            var transfer = try #require(browser.transfer)
            try await settle(transfer, seconds: 300)
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            for pick in small {
                #expect(try await guest.sha256(GuestPath.child(pick.name, of: work)) == bench.sha256(pick), "\(pick.name) arrived whole")
            }
            browser.closeTransfer()
            await browser.load(work)
            let back = try #require(browser.entries(in: work).first { $0.name == "small two.bin" })
            browser.export([back])
            transfer = try #require(browser.transfer)
            try await waitUntil("the small export is ready", seconds: 300) { transfer.stage != .running }
            #expect(transfer.stage == .ready, "\(transfer.title) \(transfer.detail)")
            #expect(try Bench.sha256(of: try #require(shared.first)) == bench.sha256(small[1]), "the export arrived whole")
            finishShare?(true)
            Self.cue("small import and export done")

            // MARK: Sent away mid-import
            let big = try bench.pick("background.bin", bytes: 768 * Int(mib), seed: 61, modified: Date(timeIntervalSince1970: 1_650_000_000))
            browser.received([big])
            transfer = try #require(browser.transfer)
            try await waitUntil("the import has moved 16 MiB", seconds: 300) { transfer.items[0].moved >= 16 * mib || !transfer.running }
            #expect(transfer.running, "the import is still running when the application is sent away")
            Self.cue("background now (import, moved \(transfer.items[0].moved))")
            if Self.simulatesBackground { browser.leftForeground() }
            try await waitUntil("the import stops", seconds: 600) { !transfer.running }
            let bigPath = GuestPath.child(big.name, of: work)
            #expect(transfer.stage == .failed, "\(transfer.title) \(transfer.detail)")
            #expect(transfer.items[0].state == .failed(FileBrowserModel.leftForegroundReason), "\(transfer.items[0].state)")
            Self.cue("import stopped: \(transfer.items[0].state)")
            await guest.expectNoPartials(work, "after the application was sent to the background")
            #expect(!(try await guest.names(work)).contains(big.name), "the stopped import left no file of that name")
            browser.retryFailed()
            try await settle(transfer, seconds: 900)
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            #expect(try await guest.sha256(bigPath) == bench.sha256(big), "Try Again brought the file whole")
            browser.closeTransfer()
            Self.cue("import retried and whole")

            // MARK: Sent away mid-export
            // The guest reads a file out faster than a harness can send the
            // application away, so the export is of 3 GiB the guest makes of the
            // import's own bytes (four copies, each of which differs by block).
            let pullPath = GuestPath.child("pull.bin", of: work)
            _ = try await guest.run("cat '\(bigPath)' '\(bigPath)' '\(bigPath)' '\(bigPath)' > '\(pullPath)'", seconds: 600)
            let pullBytes = 4 * UInt64(big.size)
            await browser.load(work)
            let pull = try #require(browser.entries(in: work).first { $0.name == "pull.bin" })
            #expect(pull.size == pullBytes)
            browser.export([pull])
            transfer = try #require(browser.transfer)
            try await waitUntil("the export has moved 16 MiB", seconds: 300) { transfer.items[0].moved >= 16 * mib || !transfer.running }
            #expect(transfer.running, "the export is still running when the application is sent away")
            Self.cue("background now (export, moved \(transfer.items[0].moved))")
            if Self.simulatesBackground { browser.leftForeground() }
            try await waitUntil("the export stops", seconds: 600) { !transfer.running }
            #expect(transfer.stage == .failed, "\(transfer.title) \(transfer.detail)")
            #expect(transfer.items[0].state == .failed(FileBrowserModel.leftForegroundReason), "\(transfer.items[0].state)")
            Self.cue("export stopped: \(transfer.items[0].state)")
            if let staging = transfer.directory {
                let left = (try? FileManager.default.contentsOfDirectory(atPath: staging.path)) ?? []
                #expect(left.isEmpty, "the stopped export left a partial copy: \(left)")
            }
            browser.retryFailed()
            try await waitUntil("the export is ready again", seconds: 900) { transfer.stage != .running }
            #expect(transfer.stage == .ready, "\(transfer.title) \(transfer.detail)")
            let copy = try #require(transfer.staged.first)
            let copySize = (try FileManager.default.attributesOfItem(atPath: copy.path)[.size] as? NSNumber)?.uint64Value
            #expect(copySize == pullBytes, "Try Again brought every byte: \(String(describing: copySize))")
            for block in [0, 767, 768, 2_000, 3_071] {
                let remote = try await guest.run("dd if='\(pullPath)' bs=1048576 skip=\(block) count=1 2>/dev/null | sha256sum", seconds: 120).text
                #expect(remote.hasPrefix(try Bench.sha256(of: copy, blockOffset: block)), "block \(block) of the retried export")
            }
            finishShare?(true)
            browser.sheetClosed()
            Self.cue("export retried and whole")
        }
        // The guest is off. A device keeps nothing of the test's guest.
        if let library = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first {
            try? FileManager.default.removeItem(at: library.appendingPathComponent("TestGuests/filex-bg", isDirectory: true))
        }
    }

    // MARK: A file over 4 GiB

    @Test(.timeLimit(.minutes(60)))
    func aFileOver4GiBGoesBothWays() async throws {
        guard Self.bigEnabled else { return }
        try #require(Self.environment["ANDARIUM_GUEST_INSTANCE"] == "files-big")
        try await withAndroidGuest(cleanupCommand: Self.cleanup) { model, operations in
            let browser = try #require(model.fileBrowser)
            let bench = try Bench()
            defer { bench.remove() }
            let guest = GuestSide(operations)
            let work = Self.folder
            #expect(await guest.waitForStorage(), "internal storage did not come up")
            let watcher = try await openSettingsOnTheFiles(model, operations)
            defer { watcher.cancel() }
            _ = try await guest.run("rm -rf '\(work)'; mkdir -p '\(work)'")
            browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: work)]
            await browser.load(work)

            let length = 4 * Int(Limits.bytesPerGiB) + 13 * Int(mib) + 1
            let big = try bench.pick("over 4 GiB.bin", bytes: length, seed: 51, modified: Date(timeIntervalSince1970: 1_650_000_000))
            let started = ContinuousClock.now
            browser.received([big])
            var transfer = try #require(browser.transfer)
            try await settle(transfer, seconds: 1_500)
            note("imported \(length) bytes in \(ContinuousClock.now - started)")
            #expect(transfer.stage == .finished, "\(transfer.title) \(transfer.detail)")
            let path = GuestPath.child("over 4 GiB.bin", of: work)
            #expect(try await guest.size(path) == UInt64(length), "the guest has every byte")
            // The first and the last megabytes, and one past 4 GiB, on the guest's side.
            for block in [0, 1_024, 4_096, 4_105] {
                let remote = try await guest.run("dd if='\(path)' bs=1048576 skip=\(block) count=1 2>/dev/null | sha256sum", seconds: 600).text
                let local = try Bench.sha256(of: bench.url(for: big), blockOffset: block)
                #expect(remote.hasPrefix(local), "block \(block) of the large file")
            }
            await browser.load(work)
            let listed = try #require(browser.entries(in: work).first { $0.name == "over 4 GiB.bin" })
            #expect(listed.size == UInt64(length), "the listing says \(listed.size): the sync v2 listing carries sizes past 4 GiB")

            // Out again, and the same bytes.
            browser.closeTransfer()
            browser.export([listed])
            transfer = try #require(browser.transfer)
            let pulled = ContinuousClock.now
            try await waitUntil("the export is ready", seconds: 2_400) { transfer.stage != .running }
            note("exported \(length) bytes in \(ContinuousClock.now - pulled)")
            #expect(transfer.stage == .ready, "\(transfer.title) \(transfer.detail)")
            let staged = try #require(transfer.staged.first)
            #expect(try Bench.sha256(of: staged) == bench.sha256(big), "the exported file is the imported one")
            browser.closeTransfer()
            browser.sheetClosed()
        }
    }

    // MARK: Support

    private func note(_ text: String) { print("FILEX: \(text)") }

    /// What the person does to have the browser: Settings is open, which reads
    /// the guest's storage, whatever its USB debugging setting. The browser
    /// takes no import or export before it says the storage is up.
    private func openSettingsOnTheFiles(_ model: AppModel, _ operations: AdbGuestOperations) async throws -> Task<Void, Never> {
        let watcher = Task { await model.watchSettings() }
        do {
            try await waitUntil("the browser is available", seconds: 240) { model.filesAvailable }
        } catch {
            watcher.cancel()
            throw error
        }
        return watcher
    }

    private func waitUntil(
        _ what: String, seconds: Int, _ condition: @MainActor () async -> Bool
    ) async throws {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while ContinuousClock.now < deadline {
            if await condition() { return }
            try await Task.sleep(for: .milliseconds(100))
        }
        Issue.record("timed out waiting for \(what)")
        throw CancellationError()
    }

    /// The transfer is over, whatever it ended in.
    private func settle(_ transfer: FileTransfer, seconds: Int = 600) async throws {
        try await waitUntil("the transfer ends", seconds: seconds) { !transfer.running }
    }

    /// The guest's side, through the application's own channel.
    private struct GuestSide {
        let operations: AdbGuestOperations

        init(_ operations: AdbGuestOperations) { self.operations = operations }

        @discardableResult
        func run(_ command: String, seconds: Int = 120) async throws -> ShellResult {
            try await operations.shell(command, limit: .seconds(seconds))
        }

        /// One word of the guest's shell.
        static func quoted(_ value: String) -> String {
            "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
        }

        func sha256(_ path: String) async throws -> String {
            let text = try await run("sha256sum \(Self.quoted(path))", seconds: 600).text
            return String(text.prefix { !$0.isWhitespace })
        }

        func size(_ path: String) async throws -> UInt64 {
            let text = try await run("stat -c %s \(Self.quoted(path))").text.trimmingCharacters(in: .whitespacesAndNewlines)
            return UInt64(text) ?? 0
        }

        func modified(_ path: String) async throws -> Int {
            let text = try await run("stat -c %Y \(Self.quoted(path))").text.trimmingCharacters(in: .whitespacesAndNewlines)
            return Int(text) ?? 0
        }

        /// Every name in a folder, hidden ones too.
        func names(_ folder: String) async throws -> [String] {
            try await run("ls -A '\(folder)'").text.split(separator: "\n").map(String.init)
        }

        /// Android has finished booting before its internal storage is mounted
        /// and the media provider has made the standard folders: the
        /// application's own readiness check says when they are.
        func waitForStorage(seconds: Int = 240) async -> Bool {
            let deadline = ContinuousClock.now + .seconds(seconds)
            while ContinuousClock.now < deadline {
                if await operations.storageReady() == true { return true }
                try? await Task.sleep(for: .seconds(2))
            }
            return false
        }

        /// The files of a folder (as `relative_path`, with its trailing slash)
        /// whose rows the media provider has finished.
        func finishedRows(in relativePath: String) -> String {
            "content query --uri content://media/external/file --projection _display_name:_size "
                + "--where \"relative_path='\(relativePath)' AND is_pending=0\""
        }

        func waitFor(_ command: String, contains needle: String, seconds: Int) async -> Bool {
            let deadline = ContinuousClock.now + .seconds(seconds)
            while ContinuousClock.now < deadline {
                if let text = try? await run(command).text, text.contains(needle) { return true }
                try? await Task.sleep(for: .seconds(2))
            }
            return false
        }

        /// A stopped transfer's temporary file is removed on a connection of
        /// its own, a moment after the transfer ends.
        func expectNoPartials(_ folder: String, _ when: String) async {
            let deadline = ContinuousClock.now + .seconds(60)
            var names: [String] = []
            while ContinuousClock.now < deadline {
                names = (try? await self.names(folder)) ?? []
                if !names.contains(where: GuestFiles.isPartial) { return }
                try? await Task.sleep(for: .seconds(1))
            }
            Issue.record("a partial file is left \(when): \(names)")
        }

        /// The guest's own Files app, opened on the folder, lists these names.
        func filesAppShows(folder: String, names: [String]) async -> Bool {
            let encoded = folder.replacingOccurrences(of: "/", with: "%2F")
            // A fresh guest waits in LineageOS's setup wizard, which covers every
            // other app; this guest is the test's own.
            _ = try? await run(
                "settings put global device_provisioned 1; settings put secure user_setup_complete 1; pm disable-user --user 0 org.lineageos.setupwizard; am force-stop org.lineageos.setupwizard; sleep 2; input keyevent KEYCODE_HOME; sleep 2")
            let started = try? await run(
                "am start -a android.intent.action.VIEW -t vnd.android.document/directory -d 'content://com.android.externalstorage.documents/document/primary%3A\(encoded)' 2>&1 | tail -3")
            let deadline = ContinuousClock.now + .seconds(90)
            var seen: [String] = []
            while ContinuousClock.now < deadline {
                try? await Task.sleep(for: .seconds(4))
                guard let dump = try? await run(
                    "uiautomator dump /data/local/tmp/filex-ui.xml >/dev/null 2>&1; cat /data/local/tmp/filex-ui.xml", seconds: 60).text
                else { continue }
                seen = dump.components(separatedBy: "text=\"").dropFirst().compactMap { $0.components(separatedBy: "\"").first }.filter { !$0.isEmpty }
                if names.allSatisfy({ dump.contains("text=\"\($0)\"") }) {
                    _ = try? await run("rm -f /data/local/tmp/filex-ui.xml; input keyevent KEYCODE_HOME")
                    return true
                }
            }
            print("FILEX: am start said: \(started?.text ?? "nothing"); the screen's texts: \(seen.prefix(40))")
            let top = try? await run("dumpsys activity activities | grep -E 'topResumedActivity' | head -3; pm list packages | grep -i documentsui")
            print("FILEX: resumed: \(top?.text ?? "unknown")")
            _ = try? await run("rm -f /data/local/tmp/filex-ui.xml; input keyevent KEYCODE_HOME")
            return false
        }
    }

    /// The host's side: files made for the tests, in a folder of their own.
    private struct Bench {
        let directory: URL

        init() throws {
            directory = FileManager.default.temporaryDirectory
                .appendingPathComponent("FilexBench-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        }

        func remove() { try? FileManager.default.removeItem(at: directory) }

        func url(for pick: FilePick) -> URL { pick.url ?? directory.appendingPathComponent(pick.name) }

        /// A folder of its own for each file, so two files of one name differ.
        private func slot(_ name: String) throws -> URL {
            let folder = directory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            return folder.appendingPathComponent(name)
        }

        /// A file of `bytes` bytes whose every megabyte differs, from `seed`.
        func pick(_ name: String, bytes: Int, seed: UInt8, modified: Date) throws -> FilePick {
            let url = try slot(name)
            let manager = FileManager.default
            manager.createFile(atPath: url.path, contents: nil)
            let handle = try FileHandle(forWritingTo: url)
            defer { try? handle.close() }
            var block = Data(count: 1 << 20)
            var state = UInt64(seed) &* 0x9E37_79B9_7F4A_7C15 | 1
            block.withUnsafeMutableBytes { buffer in
                for index in stride(from: 0, to: buffer.count, by: 8) {
                    state ^= state << 13
                    state ^= state >> 7
                    state ^= state << 17
                    buffer.storeBytes(of: state, toByteOffset: index, as: UInt64.self)
                }
            }
            var written = 0
            var counter: UInt64 = 0
            while written < bytes {
                let count = min(block.count, bytes - written)
                block.withUnsafeMutableBytes { $0.storeBytes(of: counter, toByteOffset: 0, as: UInt64.self) }
                try handle.write(contentsOf: block.prefix(count))
                written += count
                counter += 1
            }
            try manager.setAttributes([.modificationDate: modified], ofItemAtPath: url.path)
            return FilePick(url: url, name: name, size: UInt64(bytes), modified: modified)
        }

        /// A real, tiny JPEG.
        func jpeg(_ name: String, modified: Date) throws -> FilePick {
            let data = Data(base64Encoded: "/9j/4AAQSkZJRgABAQEASABIAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////wgALCAABAAEBAREA/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxA=")!
            let url = try slot(name)
            try data.write(to: url)
            try FileManager.default.setAttributes([.modificationDate: modified], ofItemAtPath: url.path)
            return FilePick(url: url, name: name, size: UInt64(data.count), modified: modified)
        }

        func sha256(_ pick: FilePick) throws -> String { try Self.sha256(of: url(for: pick)) }

        static func sha256(of url: URL, blockOffset: Int? = nil) throws -> String {
            let handle = try FileHandle(forReadingFrom: url)
            defer { try? handle.close() }
            var hasher = SHA256()
            if let blockOffset {
                try handle.seek(toOffset: UInt64(blockOffset) << 20)
                hasher.update(data: try handle.read(upToCount: 1 << 20) ?? Data())
            } else {
                while let chunk = try handle.read(upToCount: 4 << 20), !chunk.isEmpty {
                    hasher.update(data: chunk)
                }
            }
            return hasher.finalize().map { String(format: "%02x", $0) }.joined()
        }

        /// The paths below a folder, sorted, folders and files.
        static func tree(_ url: URL) throws -> [String] {
            let base = url.standardizedFileURL.path
            let enumerator = FileManager.default.enumerator(atPath: base)
            var paths: [String] = []
            while let path = enumerator?.nextObject() as? String { paths.append(path) }
            return paths.sorted()
        }
    }
}
