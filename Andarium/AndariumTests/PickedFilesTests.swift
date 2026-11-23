// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Observation
import SwiftUI
import Testing
import UIKit
import UniformTypeIdentifiers
@testable import Andarium

/// The files the document pickers hand over. The copy-mode picker puts a copy
/// of each file in the application's inbox, and the presenter takes it into a
/// folder of ours (`PickedFolder`) that lives as long as the install or the
/// import that uses it. These tests make the system's copies themselves, since
/// the Files sheet cannot be driven from a test, and run the real callers on
/// them: a plain temporary URL, which carries no security scope, goes through
/// both.
private enum Copies {
    /// What the picker leaves in the application's inbox for a picked file.
    static func systemCopy(_ name: String, bytes: Int = 100) throws -> URL {
        let inbox = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(UUID().uuidString)-Inbox", isDirectory: true)
        try FileManager.default.createDirectory(at: inbox, withIntermediateDirectories: true)
        let url = inbox.appendingPathComponent(name)
        try Data(count: bytes).write(to: url)
        return url
    }

    /// The picks of copies taken into a folder, as `CopyDocumentPicker` hands
    /// them to the file browser, and where the folder is. The folder lives in
    /// the picks alone.
    static func picks(_ names: [String]) throws -> (picks: [FilePick], directory: URL) {
        let folder = try PickedFolder()
        let urls = try names.map { try folder.adopt(systemCopy($0)) }
        return (urls.map { FilePick.picked($0, staged: folder) }, folder.directory)
    }

    static func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }

    /// Whether the directory entry itself is a link to `target`, whatever the
    /// target is (`exists` follows the link).
    static func isLink(_ url: URL, to target: URL) -> Bool {
        (try? FileManager.default.destinationOfSymbolicLink(atPath: url.path)) == target.path
    }

    /// A file outside the temporary directory, where a provider that handed
    /// over an original keeps it.
    static func original(_ name: String = "\(UUID().uuidString).apk") throws -> URL {
        let caches = try FileManager.default.url(
            for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        let url = caches.appendingPathComponent(name)
        try Data(count: 10).write(to: url)
        return url
    }

    static func eventually(seconds: Int = 15, _ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(20))
        }
        return condition()
    }
}

// MARK: The folder

@Suite(.serialized)
struct PickedFolderTests {
    @Test func aCopyIsMovedIntoTheFolderAndTheFolderGoesWithItsLastHolder() throws {
        let copy = try Copies.systemCopy("Gears.apk", bytes: 1234)
        let directory: URL
        let moved: URL
        do {
            let folder = try PickedFolder()
            directory = folder.directory
            moved = try folder.adopt(copy)
            #expect(moved.deletingLastPathComponent().standardizedFileURL == directory.standardizedFileURL)
            #expect(moved.lastPathComponent == "Gears.apk")
            #expect(!Copies.exists(copy), "the copy is moved, not left in the inbox")
            #expect(try moved.resourceValues(forKeys: [.fileSizeKey]).fileSize == 1234)
            #expect(directory.deletingLastPathComponent().lastPathComponent == "PickedFiles")
            #expect(Copies.exists(moved))
        }
        #expect(!Copies.exists(directory) && !Copies.exists(moved))
    }

    @Test func twoCopiesOfOneNameAreToldApart() throws {
        let folder = try PickedFolder()
        let first = try folder.adopt(Copies.systemCopy("IMG.jpg"))
        let second = try folder.adopt(Copies.systemCopy("IMG.jpg"))
        #expect(first.lastPathComponent == "IMG.jpg" && second.lastPathComponent == "IMG (1).jpg")
        #expect(Copies.exists(first) && Copies.exists(second))
    }

    /// A provider that handed over the original, not a copy, is never moved
    /// or deleted.
    @Test func aFileOutsideTheTemporaryDirectoryIsNoCopyAndStaysWhereItIs() throws {
        let caches = try FileManager.default.url(
            for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        let original = caches.appendingPathComponent("\(UUID().uuidString).apk")
        try Data(count: 10).write(to: original)
        defer { try? FileManager.default.removeItem(at: original) }
        #expect(!PickedFolder.isInTemporaryDirectory(original))
        let folder = try PickedFolder()
        #expect(try folder.adopt(original) == original)
        #expect(Copies.exists(original))
        PickedFolder.removeUntaken(original)
        #expect(Copies.exists(original))
        folder.release(original)
        #expect(Copies.exists(original))
    }

    /// A link outside the temporary directory to a copy inside it is the
    /// provider's own entry, which is never moved or deleted; the copy it names
    /// is left alone too.
    @Test func aLinkOutsideTheTemporaryDirectoryToAFileInsideIsNoCopyAndStaysWhereItIs() throws {
        let target = try Copies.systemCopy("Target.apk")
        let outside = try Copies.original()
        defer {
            try? FileManager.default.removeItem(at: target.deletingLastPathComponent())
            try? FileManager.default.removeItem(at: outside)
        }
        try FileManager.default.removeItem(at: outside)
        try FileManager.default.createSymbolicLink(at: outside, withDestinationURL: target)
        #expect(!PickedFolder.isInTemporaryDirectory(outside))
        let folder = try PickedFolder()
        #expect(try folder.adopt(outside) == outside)
        #expect(Copies.isLink(outside, to: target), "the entry is not moved")
        PickedFolder.removeUntaken(outside)
        folder.release(outside)
        #expect(Copies.isLink(outside, to: target), "the entry is not removed")
        #expect(Copies.exists(target))
        #expect(!Copies.exists(folder.directory.appendingPathComponent(outside.lastPathComponent)))
    }

    /// The other way: a link inside the temporary directory to a file outside
    /// it names an original, which is never moved or deleted either.
    @Test func aLinkInTheTemporaryDirectoryToAFileOutsideIsNoCopyAndStaysWhereItIs() throws {
        let original = try Copies.original()
        let inbox = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(UUID().uuidString)-Inbox", isDirectory: true)
        try FileManager.default.createDirectory(at: inbox, withIntermediateDirectories: true)
        let link = inbox.appendingPathComponent("Link.apk")
        defer {
            try? FileManager.default.removeItem(at: inbox)
            try? FileManager.default.removeItem(at: original)
        }
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: original)
        #expect(!PickedFolder.isInTemporaryDirectory(link))
        let folder = try PickedFolder()
        #expect(try folder.adopt(link) == link)
        PickedFolder.removeUntaken(link)
        #expect(Copies.isLink(link, to: original) && Copies.exists(original))
    }

    @Test func releaseRemovesOneFileOfTheFolderAndNothingElse() throws {
        let folder = try PickedFolder()
        let kept = try folder.adopt(Copies.systemCopy("kept.txt"))
        let skipped = try folder.adopt(Copies.systemCopy("skipped.txt"))
        let outside = try Copies.systemCopy("outside.txt")
        defer { try? FileManager.default.removeItem(at: outside.deletingLastPathComponent()) }
        folder.release(outside)
        folder.release(folder.directory.deletingLastPathComponent())
        folder.release(skipped)
        #expect(Copies.exists(kept) && !Copies.exists(skipped) && Copies.exists(outside))
        folder.discard()
        folder.discard()
        #expect(!Copies.exists(folder.directory))
    }

    /// The launch's sweep takes the pick folders and nothing else of the
    /// temporary directory. The sweep is run on a root of this test's own, since
    /// the real one holds the folders of tests that run at the same time.
    @Test func theSweepRemovesThePickFoldersAndNothingElse() throws {
        let tmp = FileManager.default.temporaryDirectory
        #expect(PickedFolder.directory == tmp.appendingPathComponent("PickedFiles", isDirectory: true))
        let root = tmp.appendingPathComponent("PickedFilesTest-\(UUID().uuidString)", isDirectory: true)
        let stale = try PickedFolder(in: root)
        _ = try stale.adopt(Copies.systemCopy("left.apk"))
        let other = try PickedFolder(in: root)
        let neighbour = tmp.appendingPathComponent("PickedFilesNeighbour-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: neighbour, withIntermediateDirectories: true)
        let note = neighbour.appendingPathComponent("note.txt")
        try Data(count: 3).write(to: note)
        let staging = try GuestFileStaging.makeTransferDirectory()
        defer {
            try? FileManager.default.removeItem(at: neighbour)
            try? FileManager.default.removeItem(at: staging)
        }
        PickedFolder.sweep(in: root)
        #expect(!Copies.exists(root) && !Copies.exists(stale.directory) && !Copies.exists(other.directory))
        #expect(Copies.exists(note) && Copies.exists(staging))
        // A folder made after the sweep is made as before.
        let fresh = try PickedFolder(in: root)
        #expect(Copies.exists(fresh.directory))
        fresh.discard()
        try? FileManager.default.removeItem(at: root)
    }
}

// MARK: The presenter

@MainActor
@Observable
private final class PickerProbe {
    var presenting = false
    var copies: PickedCopies?
    var failure: Error?
    var deliveries = 0
}

private struct PickerProbeView: View {
    @Bindable var probe: PickerProbe

    var body: some View {
        Color.clear.documentCopyPicker(isPresented: $probe.presenting, types: [.item], allowsMultipleSelection: true) { result in
            probe.deliveries += 1
            switch result {
            case let .success(copies): probe.copies = copies
            case let .failure(error): probe.failure = error
            }
        }
    }
}

@MainActor
@Suite(.serialized)
struct CopyDocumentPickerTests {
    /// The picker is on screen with `probe.presenting` set: the real remote
    /// picker, presented over the probe.
    private func present(_ probe: PickerProbe, in lease: HostWindow.Lease) async throws -> (UIViewController, UIDocumentPickerViewController) {
        let controller = UIHostingController(rootView: PickerProbeView(probe: probe))
        lease.window.rootViewController = controller
        try #require(await Copies.eventually { controller.viewIfLoaded?.window != nil })
        probe.presenting = true
        try #require(await Copies.eventually { controller.presentedViewController is UIDocumentPickerViewController })
        return (controller, try #require(controller.presentedViewController as? UIDocumentPickerViewController))
    }

    /// The picker's mode, which UIKit keeps as the deprecated `documentPickerMode`:
    /// 0 is `import`, the mode of a picker that hands over copies, and 1 is `open`,
    /// the one that opens in place.
    private func isCopyMode(_ picker: UIDocumentPickerViewController) -> Bool {
        (picker.value(forKey: "documentPickerMode") as? Int) == 0
    }

    private func answer(_ picker: UIDocumentPickerViewController, with urls: [URL]) {
        picker.delegate?.documentPicker?(picker, didPickDocumentsAt: urls)
    }

    @Test func theSystemPickerIsPresentedInCopyModeAndAPickGivesTheCopiesInOurFolder() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let probe = PickerProbe()
        let (controller, picker) = try await present(probe, in: lease)
        #expect(isCopyMode(picker), "the picker must hand over copies")
        #expect(picker.allowsMultipleSelection)
        let first = try Copies.systemCopy("a.apk", bytes: 10)
        let second = try Copies.systemCopy("b.apk", bytes: 20)
        answer(picker, with: [first, second])
        let directory: URL
        do {
            let copies = try #require(probe.copies)
            #expect(probe.deliveries == 1 && !probe.presenting)
            #expect(copies.urls.map(\.lastPathComponent) == ["a.apk", "b.apk"])
            #expect(copies.urls.allSatisfy { Copies.exists($0) && $0.deletingLastPathComponent().standardizedFileURL == copies.folder.directory.standardizedFileURL })
            #expect(!Copies.exists(first) && !Copies.exists(second))
            directory = copies.folder.directory
        }
        controller.dismiss(animated: false)
        // The pick is let go: the folder goes with it.
        probe.copies = nil
        #expect(!Copies.exists(directory))
    }

    @Test func cancellingGivesNothingAndLetsTheBindingGo() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let probe = PickerProbe()
        let (controller, picker) = try await present(probe, in: lease)
        picker.delegate?.documentPickerWasCancelled?(picker)
        #expect(!probe.presenting && probe.deliveries == 0)
        controller.dismiss(animated: false)
    }

    @Test func aPickTheModelTakesAwayDismissesThePickerAndSaysNothing() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let probe = PickerProbe()
        let (controller, picker) = try await present(probe, in: lease)
        probe.presenting = false
        try #require(await Copies.eventually { controller.presentedViewController == nil })
        // A late answer of the dismissed picker is not heard either.
        let copy = try Copies.systemCopy("late.apk")
        answer(picker, with: [copy])
        #expect(probe.deliveries == 0)
        try? FileManager.default.removeItem(at: copy.deletingLastPathComponent())
    }

    @Test func aCopyThatCannotBeMovedEndsInAFailureNotSilenceAndLeavesNothing() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let probe = PickerProbe()
        let (controller, picker) = try await present(probe, in: lease)
        // One copy is whole and one is not there: the system's copy failed.
        let wholeName = "whole-\(UUID().uuidString).apk"
        let whole = try Copies.systemCopy(wholeName)
        let missing = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-Inbox/gone.apk")
        answer(picker, with: [whole, missing])
        #expect(probe.copies == nil && probe.deliveries == 1)
        #expect(probe.failure is PickedFileFailure)
        #expect(probe.failure?.localizedDescription == "Andarium couldn\u{2019}t take the file from Files.")
        #expect(!probe.presenting)
        #expect(!Copies.exists(whole), "the copy that was whole is removed too")
        // Tests that run at the same time keep folders in the same place: this
        // pick's is the one that would hold its file.
        let folders = (try? FileManager.default.contentsOfDirectory(atPath: PickedFolder.directory.path)) ?? []
        #expect(!folders.contains { Copies.exists(PickedFolder.directory.appendingPathComponent($0).appendingPathComponent(wholeName)) },
                "no folder of the pick is left")
        controller.dismiss(animated: false)
    }

    /// The folder cannot be made (an ordinary file is where the folders go):
    /// the pick ends in a failure, and the system's copies are removed with
    /// it, while an original that is not in the temporary directory is not.
    @Test func aFolderThatCannotBeMadeEndsInAFailureAndLeavesNoCopy() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("PickedFilesTest-\(UUID().uuidString)")
        try Data(count: 1).write(to: root)
        let first = try Copies.systemCopy("first.apk")
        let second = try Copies.systemCopy("second.apk")
        let original = try Copies.original()
        defer {
            try? FileManager.default.removeItem(at: root)
            try? FileManager.default.removeItem(at: first.deletingLastPathComponent())
            try? FileManager.default.removeItem(at: second.deletingLastPathComponent())
            try? FileManager.default.removeItem(at: original)
        }
        #expect(throws: (any Error).self) { _ = try PickedFolder(in: root) }
        let result = CopyDocumentPicker.Coordinator.take([first, second, original], in: root)
        guard case let .failure(error)? = result else {
            Issue.record("a folder that cannot be made must end the pick in a failure")
            return
        }
        #expect(error is PickedFileFailure)
        #expect(!Copies.exists(first) && !Copies.exists(second), "no copy of the pick is left behind")
        #expect(Copies.exists(original), "an original outside the temporary directory is not ours to remove")
        // The sweep of the next launch has nothing of the pick to find either.
        PickedFolder.sweep(in: root)
        #expect(!Copies.exists(first) && !Copies.exists(second))
    }

    @Test func anEmptyPickIsNoPick() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let probe = PickerProbe()
        let (controller, picker) = try await present(probe, in: lease)
        answer(picker, with: [])
        #expect(!probe.presenting && probe.deliveries == 0)
        controller.dismiss(animated: false)
    }
}

// MARK: Install APK

@MainActor
@Suite(.serialized)
struct PickedFilesInstallTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)

    private func running() -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.launchApplied = true
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(.normal)
        return (model, driver)
    }

    /// Hands the model a copy as the Settings handler does, and gives back
    /// where the copy's folder is: the folder lives in the model's hands alone.
    private func installStaged(_ model: AppModel, _ name: String = "Gears.apk") throws -> (url: URL, directory: URL) {
        let folder = try PickedFolder()
        let url = try folder.adopt(Copies.systemCopy(name, bytes: 3_000_000))
        model.installPicked(url, staged: folder)
        return (url, folder.directory)
    }

    private let target = LegacyTarget(appLevel: 7, requiredLevel: 24)

    @Test func aPlainTemporaryURLInstallsAndStaysWhereItIs() async throws {
        let (model, driver) = running()
        let plain = try Copies.systemCopy("Plain.apk", bytes: 3_000_000)
        defer { try? FileManager.default.removeItem(at: plain.deletingLastPathComponent()) }
        model.installPicked(plain)
        try #require(await Copies.eventually { model.busy?.result != nil })
        #expect(driver.installURLs == [plain] && model.busy?.title == "App installed")
        #expect(Copies.exists(plain), "a file the install does not own is never removed")
    }

    @Test func theCopyIsRemovedWhenTheInstallHasFinished() async throws {
        let (model, driver) = running()
        let gate = OnceGate()
        driver.install = { _ in await gate.wait() }
        let (url, directory) = try installStaged(model)
        try #require(await Copies.eventually { driver.installed == 1 })
        #expect(Copies.exists(url), "the install reads the copy")
        gate.open()
        try #require(await Copies.eventually { model.busy?.result != nil })
        #expect(await Copies.eventually { !Copies.exists(directory) })
        #expect(model.busy?.title == "App installed")
    }

    @Test func theCopyIsRemovedWhenTheInstallFailed() async throws {
        let (model, driver) = running()
        driver.install = { _ in throw AdbFailure(message: "no room") }
        let (_, directory) = try installStaged(model)
        try #require(await Copies.eventually { model.busy?.result?.failed == true })
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func theCopyIsRemovedWhenTheInstallIsCancelled() async throws {
        let (model, driver) = running()
        driver.install = { _ in try await Task.sleep(for: .seconds(60)) }
        let (_, directory) = try installStaged(model)
        try #require(await Copies.eventually { driver.installed == 1 })
        #expect(Copies.exists(directory))
        model.busy?.cancel?.action()
        try #require(await Copies.eventually { model.busy == nil })
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func aFileThatIsRefusedAtOnceLeavesNoCopy() async throws {
        let (model, driver) = running()
        let (_, directory) = try installStaged(model, "photo.zip")
        #expect(model.busy?.result?.failed == true && driver.installAttempts.isEmpty)
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func aPickWhileAnotherOperationRunsLeavesNoCopy() async throws {
        let (model, driver) = running()
        driver.bugReport = { _ in
            try await Task.sleep(for: .seconds(60))
            throw CancellationError()
        }
        model.createBugReport()
        try #require(model.busy != nil)
        let (_, directory) = try installStaged(model)
        #expect(driver.installAttempts.isEmpty)
        #expect(await Copies.eventually { !Copies.exists(directory) })
        model.busy?.cancel?.action()
        try #require(await Copies.eventually { model.busy == nil })
    }

    /// Install anyway sends the same file again: the copy outlives the question
    /// and the first attempt, is read by both, and goes with the second.
    @Test func theCopySurvivesTheQuestionAndGoesWhenInstallAnywayHasFinished() async throws {
        let (model, driver) = running()
        let log = ExistsLog()
        let target = target
        driver.installAttempt = { bypass, _ in
            log.note(bypass)
            if !bypass { throw LegacyApkRefused(target: target) }
        }
        let (url, directory) = try installStaged(model)
        // The install starts when this test next waits, so the path is known by then.
        log.watch(url)
        try #require(await Copies.eventually { model.legacyInstallQuestion != nil })
        try await Task.sleep(for: .milliseconds(150))
        #expect(Copies.exists(url), "the person is still deciding")
        model.answerLegacyInstall(installAnyway: true)
        try #require(await Copies.eventually { model.busy?.result != nil })
        #expect(driver.installURLs == [url, url])
        #expect(log.all == [false: true, true: true], "the copy was whole at both attempts")
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func theCopyGoesWhenTheQuestionIsCancelledAndWhenTheGuestEnds() async throws {
        for ending in ["Cancel", "the guest ends"] {
            let (model, driver) = running()
            let target = target
            driver.installAttempt = { _, _ in throw LegacyApkRefused(target: target) }
            let (_, directory) = try installStaged(model)
            try #require(await Copies.eventually { model.legacyInstallQuestion != nil }, "\(ending)")
            #expect(Copies.exists(directory), "\(ending)")
            if ending == "Cancel" {
                model.answerLegacyInstall(installAnyway: false)
            } else {
                model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
            }
            #expect(await Copies.eventually { !Copies.exists(directory) }, "\(ending)")
        }
    }
}

private actor OnceGate {
    private var opened = false
    private var waiters: [CheckedContinuation<Void, Never>] = []

    nonisolated func open() { Task { await release() } }

    private func release() {
        opened = true
        for waiter in waiters { waiter.resume() }
        waiters = []
    }

    func wait() async {
        if opened { return }
        await withCheckedContinuation { waiters.append($0) }
    }
}

/// Whether the file an install reads is there at each attempt, by whether the
/// attempt carried the flag.
private nonisolated final class ExistsLog: @unchecked Sendable {
    private let lock = NSLock()
    private var path: String?
    private var seen: [Bool: Bool] = [:]
    func watch(_ url: URL) { lock.withLock { path = url.path } }
    func note(_ bypass: Bool) {
        lock.withLock { seen[bypass] = path.map { FileManager.default.fileExists(atPath: $0) } ?? false }
    }
    var all: [Bool: Bool] { lock.withLock { seen } }
}

// MARK: Import

@MainActor
@Suite(.serialized)
struct PickedFilesImportTests {
    private let root = GuestPath.root
    private var download: String { GuestPath.child("Download", of: root) }

    private func makeBrowser(
        _ names: [String] = [], backend scripted: FileBrowserModelTests.ScriptedBackend? = nil
    ) async -> (FileBrowserModel, FileBrowserModelTests.ScriptedBackend) {
        let backend = scripted ?? FileBrowserModelTests.ScriptedBackend()
        backend.folders[download] = names.map {
            GuestFile(path: GuestPath.child($0, of: download), isFolder: false, size: 10, modified: .now)
        }
        let browser = FileBrowserModel(backend: backend, center: NotificationCenter())
        browser.route = [FolderRoute(path: root), FolderRoute(path: download)]
        await browser.load(download)
        return (browser, backend)
    }

    private func settle(_ transfer: FileTransfer) async throws {
        try #require(await Copies.eventually(seconds: 20) { !transfer.running })
    }

    /// Hands the browser the picks of a folder of copies, as the presenter does,
    /// and gives back where the folder is: it lives in the browser's hands alone.
    private func pick(_ browser: FileBrowserModel, _ names: [String]) throws -> URL {
        let made = try Copies.picks(names)
        browser.received(made.picks)
        return made.directory
    }

    @Test func aPlainTemporaryURLIsImportedAndStaysWhereItIs() async throws {
        let (browser, backend) = await makeBrowser()
        let plain = try Copies.systemCopy("Plain.bin", bytes: 700)
        defer { try? FileManager.default.removeItem(at: plain.deletingLastPathComponent()) }
        let pick = FilePick.picked(plain)
        #expect(pick.size == 700 && pick.name == "Plain.bin" && pick.staged == nil)
        browser.received([pick])
        try await settle(try #require(browser.transfer))
        #expect(backend.sent.map(\.name) == ["Plain.bin"])
        #expect(Copies.exists(plain))
    }

    @Test func theFolderIsRemovedWhenTheImportHasFinished() async throws {
        let (browser, backend) = await makeBrowser()
        backend.holding = ["b.txt"]
        let directory = try pick(browser, ["a.txt", "b.txt"])
        let transfer = try #require(browser.transfer)
        try #require(await Copies.eventually { backend.started.contains("b.txt") })
        #expect(Copies.exists(directory), "the import is reading it")
        backend.holding = []
        try await settle(transfer)
        #expect(transfer.stage == .finished)
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func aFailedImportKeepsTheFilesForTryAgainAndRemovesThemAfterIt() async throws {
        let (browser, backend) = await makeBrowser()
        backend.failing = ["b.txt": "no"]
        let directory = try pick(browser, ["a.txt", "b.txt"])
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        #expect(transfer.stage == .failed)
        try await Task.sleep(for: .milliseconds(200))
        #expect(Copies.exists(directory), "Try Again still finds the files")
        backend.failing = [:]
        browser.retryFailed()
        try await settle(transfer)
        #expect(transfer.stage == .finished && backend.sent.map(\.name) == ["a.txt", "b.txt"])
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func cancellingTheImportRemovesTheFolder() async throws {
        let (browser, backend) = await makeBrowser()
        backend.holding = ["a.txt"]
        let directory = try pick(browser, ["a.txt"])
        try #require(await Copies.eventually { backend.started.contains("a.txt") })
        #expect(Copies.exists(directory))
        browser.closeTransfer()
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func closingSettingsOrEndingTheRunRemovesTheFolderOfAFailedImport() async throws {
        for ending in ["Settings closes", "the run ends", "a new import starts"] {
            let (browser, backend) = await makeBrowser()
            backend.failing = ["a.txt": "no"]
            let directory = try pick(browser, ["a.txt"])
            try await settle(try #require(browser.transfer))
            try await Task.sleep(for: .milliseconds(100))
            #expect(Copies.exists(directory), "\(ending)")
            switch ending {
            case "Settings closes": browser.sheetClosed()
            case "the run ends": browser.reset()
            default:
                backend.failing = [:]
                browser.received([FilePick(name: "other.txt", size: 1)])
            }
            #expect(await Copies.eventually { !Copies.exists(directory) }, "\(ending)")
        }
    }

    @Test func aPickWhileAnImportRunsIsDroppedWithItsFolder() async throws {
        let (browser, backend) = await makeBrowser()
        backend.holding = ["held.bin"]
        browser.received([FilePick(name: "held.bin", size: 1)])
        try #require(await Copies.eventually { backend.started.contains("held.bin") })
        let directory = try pick(browser, ["late.txt"])
        #expect(await Copies.eventually { !Copies.exists(directory) })
        browser.closeTransfer()
    }

    // MARK: Names that are taken

    @Test func cancellingTheQuestionRemovesTheFolder() async throws {
        let (browser, backend) = await makeBrowser(["a.txt"])
        let directory = try pick(browser, ["a.txt", "b.txt"])
        #expect(browser.conflict != nil && Copies.exists(directory))
        browser.cancelConflict()
        #expect(await Copies.eventually { !Copies.exists(directory) })
        #expect(backend.sent.isEmpty)
    }

    @Test func skippingEveryFileRemovesTheFolder() async throws {
        let (browser, _) = await makeBrowser(["a.txt"])
        let directory = try pick(browser, ["a.txt"])
        browser.resolveConflict(.skip)
        #expect(browser.transfer == nil)
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func skippingOneFileRemovesItsCopyAtOnceAndTheRestWithTheImport() async throws {
        let (browser, backend) = await makeBrowser(["a.txt"])
        backend.holding = ["b.txt"]
        var made = try Copies.picks(["a.txt", "b.txt"])
        let directory = made.directory
        let skipped = try #require(made.picks.first { $0.name == "a.txt" }?.url)
        let kept = try #require(made.picks.first { $0.name == "b.txt" }?.url)
        browser.received(made.picks)
        // The folder is in the browser's hands alone.
        made.picks = []
        browser.resolveConflict(.skip)
        #expect(!Copies.exists(skipped), "the file that is not imported has no copy")
        let transfer = try #require(browser.transfer)
        try #require(await Copies.eventually { backend.started.contains("b.txt") })
        #expect(Copies.exists(kept))
        backend.holding = []
        try await settle(transfer)
        #expect(backend.sent.map(\.name) == ["b.txt"])
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }

    @Test func keepingBothOrReplacingImportsTheCopyAndRemovesItAfter() async throws {
        for choice in [ConflictChoice.keepBoth, .replace] {
            let (browser, backend) = await makeBrowser(["a.txt"])
            let directory = try pick(browser, ["a.txt"])
            browser.resolveConflict(choice)
            try await settle(try #require(browser.transfer))
            #expect(backend.sent.count == 1)
            #expect(await Copies.eventually { !Copies.exists(directory) }, "\(choice)")
        }
    }

    @Test func closingSettingsOnTheQuestionRemovesTheFolder() async throws {
        let (browser, _) = await makeBrowser(["a.txt"])
        let directory = try pick(browser, ["a.txt"])
        #expect(browser.conflict != nil)
        browser.sheetClosed()
        #expect(await Copies.eventually { !Copies.exists(directory) })
    }
}

// MARK: Failures

@MainActor
@Suite(.serialized)
struct DocumentPickerFailureTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000)

    private func running() -> AppModel {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KB pages", appVersion: "1")
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = .running(.normal)
        model.session.sideKeysOpen = true
        return model
    }

    private struct Refused: LocalizedError {
        var errorDescription: String? { "The operation couldn\u{2019}t be completed." }
    }

    @Test func aFailedPickIsAnAlertWithTheErrorsWordsForBothPickers() {
        let model = running()
        model.documentPickerFailed(Refused(), title: AppModel.installFailure)
        #expect(model.documentPickerProblem?.title == "App not installed")
        #expect(model.documentPickerProblem?.message == "The operation couldn\u{2019}t be completed.")
        model.documentPickerProblem = nil
        model.documentPickerFailed(PickedFileFailure.notTaken, title: String(localized: "Not imported"))
        #expect(model.documentPickerProblem?.title == "Not imported")
        #expect(model.documentPickerProblem?.message == "Andarium couldn\u{2019}t take the file from Files.")
        #expect(model.alert == nil, "the screens beneath Settings say nothing")
    }

    private func alert(over controller: UIViewController) -> UIAlertController? {
        var top: UIViewController? = controller
        while let next = top?.presentedViewController { top = next }
        return top as? UIAlertController
    }

    /// Settings is a sheet over the session, and a pick fails while it is up:
    /// the alert is shown over it, and Settings stays open under it.
    @Test func theAlertIsShownOverTheSettingsSheet() async throws {
        let model = running()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        model.sessionSettingsOpen = true
        try #require(await Copies.eventually { controller.presentedViewController != nil }, "Settings never came up")
        try await Task.sleep(for: .seconds(1))
        model.documentPickerFailed(PickedFileFailure.notTaken, title: AppModel.installFailure)
        try #require(await Copies.eventually { alert(over: controller) != nil }, "the alert never came up over Settings")
        let shown = try #require(alert(over: controller))
        #expect(model.sessionSettingsOpen, "Settings is still open when the alert comes up")
        #expect(shown.presentingViewController !== controller, "the alert stands over the sheet, not the screens")
        #expect(shown.title == "App not installed")
        #expect(shown.message == "Andarium couldn\u{2019}t take the file from Files.")
        let button = try #require(shown.actions.first)
        #expect(shown.actions.count == 1 && button.title == "OK")
        model.documentPickerProblem = nil
        try #require(await Copies.eventually { alert(over: controller) == nil })
        #expect(model.sessionSettingsOpen, "Settings stays")
        model.sessionSettingsOpen = false
        #expect(await Copies.eventually { controller.presentedViewController == nil })
    }
}
