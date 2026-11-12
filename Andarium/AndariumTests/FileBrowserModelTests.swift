// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The file browser's model against a scripted backend: names that clash, the
/// three answers, a transfer that is cancelled or stopped, and what the
/// browser keeps. No guest is involved (`GuestFilesGuestTests` has the real
/// one).
@MainActor
struct FileBrowserModelTests {
    @MainActor
    final class ScriptedBackend: GuestFilesBackend {
        struct Failure: LocalizedError {
            var errorDescription: String?
        }

        var folders: [String: [GuestFile]] = [:]
        var listingFailure: Error?
        /// Names whose send or receive fails, with the reason.
        var failing: [String: String] = [:]
        /// Names whose send or receive waits until it is cancelled.
        var holding: Set<String> = []
        var prepareFailure: String?
        struct Sent {
            var name: String
            var path: String
            var replacing: Bool
        }

        private(set) var sent: [Sent] = []
        private(set) var received: [String] = []
        private(set) var cancelled: [String] = []
        private(set) var started: [String] = []

        func list(_ folder: String) async throws -> [GuestFile] {
            if let listingFailure { throw listingFailure }
            return folders[folder] ?? []
        }

        func freeBytes() async -> UInt64? { 5 * Limits.bytesPerGiB }

        func prepareImport(bytes: UInt64, into folder: String) async throws {
            if let prepareFailure { throw Failure(errorDescription: prepareFailure) }
        }

        func prepareExport(bytes: UInt64) async throws {
            if let prepareFailure { throw Failure(errorDescription: prepareFailure) }
        }

        func measure(_ file: GuestFile) async throws -> UInt64 { file.isFolder ? 1_000 : file.size }

        private func move(_ name: String) async throws {
            started.append(name)
            do {
                while holding.contains(name) { try await Task.sleep(for: .milliseconds(10)) }
            } catch {
                cancelled.append(name)
                throw error
            }
            if let reason = failing[name] { throw Failure(errorDescription: reason) }
        }

        func send(
            _ pick: FilePick, to path: String, replacing: Bool,
            progress: @escaping @Sendable (UInt64, UInt64) -> Void
        ) async throws {
            progress(pick.size / 2, pick.size)
            try await move(pick.name)
            sent.append(Sent(name: pick.name, path: path, replacing: replacing))
        }

        func receive(
            _ file: GuestFile, into directory: URL, progress: @escaping @Sendable (UInt64, UInt64) -> Void
        ) async throws -> URL {
            progress(file.size / 2, file.size)
            let url = directory.appendingPathComponent(file.name, isDirectory: file.isFolder)
            // The partial copy a stopped transfer must not leave.
            try Data("partial".utf8).write(to: url)
            do {
                try await move(file.name)
            } catch {
                try? FileManager.default.removeItem(at: url)
                throw error
            }
            received.append(file.name)
            return url
        }

        func importPickerStandIn(deliver: @escaping ([FilePick]) -> Void) -> AnyView? { nil }
    }

    private let root = GuestPath.root
    private var download: String { GuestPath.child("Download", of: root) }

    private func file(_ name: String, in folder: String, size: UInt64 = 10, isFolder: Bool = false) -> GuestFile {
        GuestFile(path: GuestPath.child(name, of: folder), isFolder: isFolder, size: size, modified: .now)
    }

    private func pick(_ name: String, size: UInt64 = 100) -> FilePick { FilePick(name: name, size: size) }

    /// A browser standing in `Download`, listed, with these entries.
    private func makeBrowser(
        _ entries: [GuestFile] = [], backend scripted: ScriptedBackend? = nil, center: NotificationCenter = NotificationCenter()
    ) async -> (FileBrowserModel, ScriptedBackend) {
        let backend = scripted ?? ScriptedBackend()
        backend.folders[download] = entries
        // Each browser hears only its own test's notifications.
        let browser = FileBrowserModel(backend: backend, center: center)
        browser.route = [FolderRoute(path: root), FolderRoute(path: download)]
        await browser.load(download)
        return (browser, backend)
    }

    private func settle(_ transfer: FileTransfer, seconds: Int = 20) async throws {
        try await waitUntil(seconds: seconds) { !transfer.running }
    }

    private func waitUntil(seconds: Int = 20, _ condition: () -> Bool) async throws {
        let deadline = ContinuousClock.now + .seconds(seconds)
        while !condition() {
            try #require(ContinuousClock.now < deadline, "condition was not reached within \(seconds) seconds")
            try await Task.sleep(for: .milliseconds(10))
        }
    }

    // MARK: Names

    @Test func aFreeNameImportsAtOnce() async throws {
        let (browser, backend) = await makeBrowser([file("old.txt", in: download)])
        browser.received([pick("new.txt")])
        #expect(browser.conflict == nil)
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        #expect(transfer.stage == .finished)
        #expect(backend.sent.map(\.path) == [GuestPath.child("new.txt", of: download)])
        #expect(backend.sent.map(\.replacing) == [false])
        #expect(browser.fresh == [GuestPath.child("new.txt", of: download)])
    }

    @Test func picksOfOneNameAreToldApartAsTheFilesAppDoes() async throws {
        let (browser, backend) = await makeBrowser()
        browser.received([pick("IMG.jpg"), pick("IMG.jpg"), pick("IMG.jpg")])
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        #expect(backend.sent.map { GuestPath.name(of: $0.path) } == ["IMG.jpg", "IMG (1).jpg", "IMG (2).jpg"])
    }

    @Test func aNameThatIsTakenAsksFirst() async throws {
        let (browser, backend) = await makeBrowser([
            file("a.txt", in: download), file("Clash", in: download, isFolder: true),
        ])
        browser.received([pick("a.txt"), pick("Clash"), pick("b.txt")])
        let conflict = try #require(browser.conflict)
        #expect(browser.transfer == nil)
        #expect(Set(conflict.clashing.map(\.name)) == ["a.txt", "Clash"])
        #expect(conflict.folders == ["Clash"])
        #expect(browser.busy, "the question holds the guest's other operations back too")
        browser.cancelConflict()
        #expect(browser.conflict == nil && backend.sent.isEmpty)
    }

    @Test func keepBothGivesTheNewFileAFreeName() async throws {
        let (browser, backend) = await makeBrowser([
            file("a.txt", in: download), file("a (1).txt", in: download), file("Clash", in: download, isFolder: true),
        ])
        browser.received([pick("a.txt"), pick("Clash"), pick("b.txt")])
        browser.resolveConflict(.keepBoth)
        try await settle(try #require(browser.transfer))
        #expect(backend.sent.map { GuestPath.name(of: $0.path) } == ["a (2).txt", "Clash (1)", "b.txt"])
        #expect(backend.sent.allSatisfy { !$0.replacing })
    }

    @Test func replaceTakesAFileButNeverAFolder() async throws {
        let (browser, backend) = await makeBrowser([
            file("a.txt", in: download), file("Clash", in: download, isFolder: true),
        ])
        browser.received([pick("a.txt"), pick("Clash"), pick("b.txt")])
        browser.resolveConflict(.replace)
        try await settle(try #require(browser.transfer))
        let steps = Dictionary(uniqueKeysWithValues: backend.sent.map { (GuestPath.name(of: $0.path), $0.replacing) })
        #expect(steps == ["a.txt": true, "Clash (1)": false, "b.txt": false])
    }

    @Test func skipImportsOnlyTheFreeNames() async throws {
        let (browser, backend) = await makeBrowser([file("a.txt", in: download)])
        browser.received([pick("a.txt"), pick("b.txt")])
        browser.resolveConflict(.skip)
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        #expect(transfer.items.map(\.name) == ["b.txt"])
        #expect(backend.sent.map(\.name) == ["b.txt"])
        // Skipping everything starts nothing.
        let (second, other) = await makeBrowser([file("a.txt", in: download)])
        second.received([pick("a.txt")])
        second.resolveConflict(.skip)
        #expect(second.transfer == nil && other.sent.isEmpty)
    }

    // MARK: Stopping

    @Test func leavingTheForegroundStopsTheTransferAndItsFilesSayWhy() async throws {
        let backend = ScriptedBackend()
        backend.holding = ["held.bin"]
        let center = NotificationCenter()
        let (browser, _) = await makeBrowser(backend: backend, center: center)
        browser.received([pick("done.txt"), pick("held.bin"), pick("later.txt")])
        let transfer = try #require(browser.transfer)
        try await waitUntil { backend.started.contains("held.bin") }
        center.post(name: UIApplication.didEnterBackgroundNotification, object: nil)
        try await settle(transfer)
        #expect(transfer.stage == .failed)
        let reason = FileBrowserModel.leftForegroundReason
        #expect(transfer.items.map(\.state) == [.done, .failed(reason), .failed(reason)], "\(transfer.items.map(\.state))")
        #expect(backend.cancelled == ["held.bin"], "the file in flight is stopped, not left to finish")
        #expect(backend.sent.map(\.name) == ["done.txt"], "what arrived stays")
        #expect(transfer.detail == reason)
        // Try Again runs the two that did not arrive, and only those.
        backend.holding = []
        browser.retryFailed()
        try await settle(transfer)
        #expect(transfer.stage == .finished)
        #expect(backend.sent.map(\.name) == ["done.txt", "held.bin", "later.txt"])
    }

    @Test func leavingTheForegroundWithNothingRunningDoesNothing() async throws {
        let (browser, _) = await makeBrowser()
        browser.received([pick("a.txt")])
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        browser.leftForeground()
        #expect(transfer.stage == .finished)
    }

    @Test func cancellingRemovesTheTransferAndStopsTheFileInFlight() async throws {
        let backend = ScriptedBackend()
        backend.holding = ["held.bin"]
        let (browser, _) = await makeBrowser(backend: backend)
        browser.received([pick("done.txt"), pick("held.bin")])
        try await waitUntil { backend.started.contains("held.bin") }
        browser.closeTransfer()
        #expect(browser.transfer == nil)
        try await waitUntil { backend.cancelled == ["held.bin"] }
    }

    @Test func aFailureKeepsTheRestAndTryAgainRunsOnlyTheFailedFile() async throws {
        let backend = ScriptedBackend()
        backend.failing = ["bad.bin": "Not enough space in the guest."]
        let (browser, _) = await makeBrowser(backend: backend)
        browser.received([pick("one.txt"), pick("bad.bin"), pick("two.txt")])
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        #expect(transfer.stage == .failed)
        #expect(transfer.items.map(\.state) == [.done, .failed("Not enough space in the guest."), .done])
        #expect(transfer.title == "1 of 3 not imported")
        backend.failing = [:]
        browser.retryFailed()
        try await settle(transfer)
        #expect(transfer.stage == .finished)
        #expect(backend.sent.map(\.name) == ["one.txt", "two.txt", "bad.bin"])
    }

    @Test func aRefusedPreparationFailsEveryFileWithItsReason() async throws {
        let backend = ScriptedBackend()
        backend.prepareFailure = "Not enough space in the guest."
        let (browser, _) = await makeBrowser(backend: backend)
        browser.received([pick("a.bin"), pick("b.bin")])
        let transfer = try #require(browser.transfer)
        try await settle(transfer)
        #expect(transfer.stage == .failed)
        #expect(transfer.items.allSatisfy { $0.state == .failed("Not enough space in the guest.") })
        #expect(backend.started.isEmpty, "nothing is sent that would not fit")
    }

    @Test func aNewTransferWaitsForTheRunningOne() async throws {
        let backend = ScriptedBackend()
        backend.holding = ["held.bin"]
        let (browser, _) = await makeBrowser(backend: backend)
        browser.received([pick("held.bin")])
        #expect(browser.busy && !browser.canImport(into: download))
        browser.received([pick("other.txt")])
        browser.export([file("x.bin", in: download)])
        #expect(browser.transfer?.items.map(\.name) == ["held.bin"])
        try await waitUntil { backend.started.contains("held.bin") }
        browser.closeTransfer()
        try await waitUntil { backend.cancelled == ["held.bin"] }
    }

    // MARK: Export

    @Test func anExportIsStagedForTheActionMenuAndRemovedAfter() async throws {
        let (browser, backend) = await makeBrowser([file("movie.mov", in: download, size: 2_000)])
        var shared: [URL] = []
        var finish: (@MainActor (Bool) -> Void)?
        browser.presentShare = { files, done in
            shared = files
            finish = done
            return true
        }
        browser.exportSelection()
        #expect(browser.transfer == nil, "nothing is selected")
        browser.selection = browser.entries(in: download)
        browser.exportSelection()
        let transfer = try #require(browser.transfer)
        try await waitUntil { transfer.stage == .ready }
        #expect(backend.received == ["movie.mov"])
        let directory = try #require(transfer.directory)
        #expect(shared.count == 1 && shared[0].path.hasPrefix(directory.path))
        #expect(FileManager.default.fileExists(atPath: shared[0].path))
        finish?(true)
        #expect(transfer.stage == .finished)
        #expect(!FileManager.default.fileExists(atPath: directory.path), "the staged copy goes with the menu")
    }

    @Test func cancellingAnExportLeavesNoPartialCopy() async throws {
        let backend = ScriptedBackend()
        backend.holding = ["movie.mov"]
        let (browser, _) = await makeBrowser([file("movie.mov", in: download, size: 2_000)], backend: backend)
        browser.export([browser.entries(in: download)[0]])
        let transfer = try #require(browser.transfer)
        try await waitUntil { backend.started.contains("movie.mov") }
        let directory = try #require(transfer.directory)
        #expect(FileManager.default.fileExists(atPath: directory.path))
        browser.closeTransfer()
        try await waitUntil { backend.cancelled == ["movie.mov"] }
        #expect(!FileManager.default.fileExists(atPath: directory.path))
    }

    @Test func anExportTheForegroundStoppedRunsAgain() async throws {
        let backend = ScriptedBackend()
        backend.holding = ["movie.mov"]
        let (browser, _) = await makeBrowser([file("movie.mov", in: download, size: 2_000)], backend: backend)
        browser.presentShare = { _, _ in true }
        browser.export([browser.entries(in: download)[0]])
        let transfer = try #require(browser.transfer)
        try await waitUntil { backend.started.contains("movie.mov") }
        browser.leftForeground()
        try await settle(transfer)
        #expect(transfer.stage == .failed)
        #expect(transfer.items[0].state == .failed(FileBrowserModel.leftForegroundReason))
        backend.holding = []
        browser.retryFailed()
        try await waitUntil { transfer.stage == .ready }
        #expect(transfer.staged.count == 1)
        browser.closeTransfer()
    }

    // MARK: Folders and lifetime

    @Test func aFolderAndARefusedFolderAreShownAsWhatTheyAre() async throws {
        let backend = ScriptedBackend()
        let (browser, _) = await makeBrowser([file("a.txt", in: download)], backend: backend)
        #expect(browser.entries(in: download).map(\.name) == ["a.txt"])
        backend.listingFailure = GuestFolderProblem.refused
        await browser.load(GuestPath.child("Android", of: root))
        #expect(browser.listing(GuestPath.child("Android", of: root)) == .refused)
        backend.listingFailure = nil
        await browser.load(GuestPath.child("Empty", of: root))
        #expect(browser.listing(GuestPath.child("Empty", of: root)) == .loaded([]))
    }

    @Test func aFailedRefreshKeepsTheListingThatIsShown() async throws {
        let backend = ScriptedBackend()
        let (browser, _) = await makeBrowser([file("a.txt", in: download)], backend: backend)
        backend.listingFailure = GuestFolderProblem.unavailable("down")
        await browser.load(download)
        #expect(browser.entries(in: download).map(\.name) == ["a.txt"])
        await browser.load(GuestPath.child("Other", of: root))
        #expect(browser.listing(GuestPath.child("Other", of: root)) == .failed("down"))
    }

    @Test func closingSettingsEmptiesTheBrowserButNotARunningTransfer() async throws {
        let backend = ScriptedBackend()
        backend.holding = ["held.bin"]
        let (browser, _) = await makeBrowser([file("a.txt", in: download)], backend: backend)
        browser.selection = browser.entries(in: download)
        browser.received([pick("held.bin")])
        let transfer = try #require(browser.transfer)
        browser.sheetClosed()
        #expect(browser.route.isEmpty && browser.selection.isEmpty && browser.listing(download) == nil)
        #expect(browser.transfer === transfer && transfer.running, "an import goes on behind the closed sheet")
        try await waitUntil { backend.started.contains("held.bin") }
        browser.reset()
        #expect(browser.transfer == nil)
        try await waitUntil { backend.cancelled == ["held.bin"] }
    }
}
