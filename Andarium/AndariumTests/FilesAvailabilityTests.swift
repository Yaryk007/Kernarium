// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The file browser waits for Android's storage: Android reports its boot
/// done before `/storage/emulated/0` is mounted, and until then an empty root
/// and a refused write would be false. It waits for nothing else, and the
/// guest's USB debugging setting is not its business.
@MainActor
struct FilesAvailabilityTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")

    private var download: String { GuestPath.child("Download", of: GuestPath.root) }

    /// A running guest whose browser stands in `Download`, listed.
    private func running() async -> (AppModel, StubDriver, FileBrowserModel) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.refreshPlan()
        let backend = FileBrowserModelTests.ScriptedBackend()
        let browser = FileBrowserModel(backend: backend, center: NotificationCenter())
        model.fileBrowser = browser
        model.start(.normal)
        model.finishBoot(.normal)
        browser.route = [FolderRoute(path: GuestPath.root), FolderRoute(path: download)]
        await browser.load(download)
        return (model, driver, browser)
    }

    @Test func theBrowserWaitsUntilAndroidSaysItsStorageIsUp() async {
        let (model, driver, _) = await running()
        // Not read yet, and read as starting: not available.
        #expect(!model.filesAvailable)
        driver.storage = false
        await model.refreshStorage()
        #expect(model.session.storageReady == false && !model.filesAvailable)
        // A reading that fails changes nothing.
        driver.storage = nil
        await model.refreshStorage()
        #expect(model.session.storageReady == false)
        // Once it is up the browser is available, with no reopening.
        driver.storage = true
        await model.refreshStorage()
        #expect(model.session.storageReady == true && model.filesAvailable)
    }

    @Test func usbDebuggingIsNotTheBrowsersBusiness() async {
        let (model, driver, _) = await running()
        driver.storage = true
        await model.refreshStorage()
        for setting in [nil, false, true] as [Bool?] {
            model.session.usbDebugging = setting
            #expect(model.filesAvailable, "USB debugging \(String(describing: setting))")
        }
        // ADB over network keeps its gate: it exposes adb to other computers.
        model.session.usbDebugging = false
        #expect(!model.adbOverNetworkAvailable)
        model.session.usbDebugging = true
        #expect(model.adbOverNetworkAvailable)
    }

    @Test func theStorageReadingNeitherWaitsForUsbDebuggingNorNeedsItOn() async throws {
        let (model, driver, _) = await running()
        driver.debugging = false
        driver.holdsUsbDebuggingReads = true
        driver.storage = true
        let watcher = Task { await model.watchSettings() }
        defer { watcher.cancel() }
        let deadline = ContinuousClock.now + .seconds(10)
        while !model.filesAvailable, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        // The setting's reading is still waiting for an answer.
        #expect(model.session.usbDebugging == nil)
        #expect(model.session.storageReady == true && model.filesAvailable)
        driver.holdsUsbDebuggingReads = false
        while model.session.usbDebugging == nil, ContinuousClock.now < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        // Read as off, and the browser is available all the same.
        #expect(model.session.usbDebugging == false && model.filesAvailable)
    }

    @Test func onceUpStorageIsNotAskedAgainAndTheRunEndingForgetsIt() async {
        let (model, driver, _) = await running()
        driver.storage = true
        await model.refreshStorage()
        driver.storage = false
        await model.refreshStorage()
        #expect(model.session.storageReady == true, "up stays up for the run")
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.session.storageReady == nil)
    }

    @Test func nothingIsImportedOrExportedWhileItIsStarting() async throws {
        let (model, driver, browser) = await running()
        driver.storage = false
        await model.refreshStorage()
        let file = GuestFile(path: GuestPath.child("a.txt", of: download), isFolder: false, size: 1, modified: .now)
        browser.export([file])
        browser.received([FilePick(name: "b.txt", size: 1)])
        #expect(browser.transfer == nil && browser.conflict == nil)
        #expect(!browser.canImport(into: download))
        // The same browser takes work as soon as the storage is up.
        driver.storage = true
        await model.refreshStorage()
        #expect(browser.canImport(into: download))
        browser.received([FilePick(name: "b.txt", size: 1)])
        #expect(browser.transfer != nil)
        browser.closeTransfer()
    }

    @Test func thePreviewHasNoGuestToAskAndSaysYesUnlessAScreenSaysNo() {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .preview, createRequirementBytes: 1)
        #expect(model.filesAvailable)
        model.session.storageReady = false
        #expect(!model.filesAvailable)
    }
}
