// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
@testable import Andarium

/// Create bug report and Install App as the model plays them: Settings has
/// dismissed, the overlay covers the screens, a Cancel stops the work, and the
/// result is a share sheet, a row or an alert. A stub stands for the runtime.
/// The tests share the staging folder, which one of them sweeps, so they run
/// one at a time.
@Suite(.serialized)
@MainActor
struct AndroidOperationTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func running(_ mode: StartMode = .normal) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(mode)
        return (model, driver)
    }

    /// Waits, up to a bound, for `condition` on the main actor.
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    private func stagedFile() throws -> URL {
        try FileManager.default.createDirectory(at: BugReportStaging.directory, withIntermediateDirectories: true)
        let url = BugReportStaging.directory.appendingPathComponent("test-\(UUID().uuidString).zip")
        try Data([0x50, 0x4B, 5, 6]).write(to: url)
        return url
    }

    /// A gate the stub waits on until the test opens it.
    private nonisolated final class Gate: @unchecked Sendable {
        private let lock = NSLock()
        private var open = false
        func release() { lock.withLock { open = true } }
        var isOpen: Bool { lock.withLock { open } }
    }

    // MARK: Bug report

    @Test func aBugReportCoversTheScreensThenOffersTheFile() async throws {
        let (model, driver) = running()
        let file = try stagedFile()
        driver.bugReport = { update in
            update(BusyUpdate(progress: 0.45))
            update(BusyUpdate(progress: 0.3))  // never moves back
            update(BusyUpdate(progress: 0.9, detail: "Copying the report\u{2026}"))
            return file
        }
        var offered: URL?
        var finishedSharing: (@MainActor () -> Void)?
        model.presentShare = { url, done in
            offered = url
            finishedSharing = done
            return true
        }
        model.createBugReport()
        #expect(model.busy?.title == "Creating bug report")
        #expect(model.busy?.cancel != nil)
        #expect(await eventually { offered != nil })
        #expect(offered == file)
        #expect(model.busy == nil && model.alert == nil)
        // The file stays for the sheet, and goes on any outcome of it.
        #expect(FileManager.default.fileExists(atPath: file.path))
        finishedSharing?()
        #expect(!FileManager.default.fileExists(atPath: file.path))
    }

    /// A report the driver finishes though the person cancelled (the cancel came
    /// as it completed) is not offered, and its file does not stay behind.
    @Test func aCancelledReportIsNotOfferedAndItsFileGoes() async throws {
        let (model, driver) = running()
        let file = try stagedFile()
        let gate = Gate()
        let started = Gate()
        driver.bugReport = { _ in
            started.release()
            // Ignores the cancel: a separate task waits, the report is returned.
            await Task.detached { while !gate.isOpen { try? await Task.sleep(for: .milliseconds(5)) } }.value
            return file
        }
        var offered = false
        model.presentShare = { _, _ in
            offered = true
            return true
        }
        model.createBugReport()
        #expect(await eventually { started.isOpen })
        try #require(model.busy?.cancel).action()
        gate.release()
        #expect(await eventually { model.busy == nil })
        #expect(!offered && model.alert == nil)
        #expect(!FileManager.default.fileExists(atPath: file.path))
    }

    /// `finished` is for an operation that was not cancelled, whatever its work
    /// returned.
    @Test func aCancelledOperationNeverRunsItsFinish() async throws {
        let (model, _) = running()
        let gate = Gate()
        let started = Gate()
        var finished: [Int] = []
        model.runOperation(
            BusyState(title: "Working", detail: "Detail", progress: 0), failure: "Not done",
            { _ in
                started.release()
                await Task.detached { while !gate.isOpen { try? await Task.sleep(for: .milliseconds(5)) } }.value
                return 7
            },
            finished: { finished.append($0) })
        #expect(await eventually { started.isOpen })
        try #require(model.busy?.cancel).action()
        gate.release()
        #expect(await eventually { model.busy == nil })
        #expect(finished.isEmpty && model.alert == nil)
        // The next operation, not cancelled, finishes as before.
        model.runOperation(
            BusyState(title: "Working", detail: "Detail", progress: 0), failure: "Not done",
            { _ in 8 }, finished: { finished.append($0) })
        #expect(await eventually { finished == [8] })
    }

    @Test func progressOnlyMovesForward() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.bugReport = { update in
            update(BusyUpdate(progress: 0.5))
            update(BusyUpdate(progress: 0.2))
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
            throw CancellationError()
        }
        model.createBugReport()
        #expect(await eventually { model.busy?.progress == 0.5 })
        gate.release()
        #expect(await eventually { model.busy == nil })
    }

    @Test func cancellingStopsTheReportAndReturnsToTheSession() async throws {
        let (model, driver) = running()
        let stopped = Gate()
        driver.bugReport = { _ in
            do {
                while true { try await Task.sleep(for: .milliseconds(5)) }
            } catch {
                stopped.release()
                throw error
            }
        }
        model.createBugReport()
        let cancel = try #require(model.busy?.cancel)
        cancel.action()
        // The overlay stays while the guest is cleaned up, with no way to cancel again.
        #expect(model.busy?.cancel == nil)
        #expect(model.busy?.detail == "Cancelling\u{2026}")
        #expect(await eventually { model.busy == nil })
        #expect(stopped.isOpen)
        #expect(model.alert == nil)
    }

    @Test func aFailureIsAnAlertAndTheOverlayGoes() async throws {
        let (model, driver) = running()
        driver.bugReport = { _ in throw AdbFailure(.remote("There is an existing bugreport in progress"), activity: .bugReport) }
        model.createBugReport()
        #expect(await eventually { model.alert != nil })
        #expect(model.alert?.title == "Bug report not created")
        #expect(model.alert?.message == "The virtual machine is already making a bug report.")
        #expect(model.busy == nil)
    }

    @Test func recoveryOffersNoBugReport() {
        let (model, _) = running(.recovery)
        model.createBugReport()
        #expect(model.busy == nil)
    }

    @Test func aSecondOperationWaitsForTheFirst() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.bugReport = { _ in
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
            throw CancellationError()
        }
        model.createBugReport()
        let first = model.busy
        model.createBugReport()
        #expect(model.busy == first)
        gate.release()
        #expect(await eventually { model.busy == nil })
    }

    @Test func theEndOfTheRunTakesTheOperationWithoutAnAlert() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.bugReport = { _ in
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
            throw AdbFailure(.guestGone, activity: .bugReport)
        }
        model.createBugReport()
        #expect(model.busy != nil)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 1)
        #expect(model.busy == nil)
        gate.release()
        try await Task.sleep(for: .milliseconds(100))
        #expect(model.alert == nil && model.busy == nil)
    }

    @Test func aDumpThatMayGoOnIsStoppedAfterAnyEndOfTheStreamButNotAfterALocalFailure() {
        #expect(BugReportOperation.dumpMayGoOn(after: CancellationError()))
        for error in [AdbError.cancelled, .timedOut, .guestGone, .streamClosed] {
            #expect(BugReportOperation.dumpMayGoOn(after: error), "\(error)")
        }
        for error in [AdbError.refused, .notAnswering, .keyRejected, .io(ENOSPC), .remote("no")] {
            #expect(!BugReportOperation.dumpMayGoOn(after: error), "\(error)")
        }
        #expect(!BugReportOperation.dumpMayGoOn(after: BugReportStaging.NoRoom(bytes: 1 << 20, device: "iPad")))
    }

    @Test func theFailuresHaveTheirWords() {
        #expect(AdbFailure(.refused, activity: .bugReport).message
            == "The virtual machine isn\u{2019}t ready to make a bug report. Try again in a moment.")
        #expect(AdbFailure(.notAnswering, activity: .installApp).message
            == "The virtual machine isn\u{2019}t answering yet. Try again once it has started.")
        #expect(AdbFailure(.keyRejected, activity: .bugReport).message
            == "The virtual machine didn\u{2019}t accept Andarium\u{2019}s key. Restart the virtual machine and try again.")
        #expect(AdbFailure(.guestGone, activity: .installApp).message
            == "The virtual machine\u{2019}s debugging service restarted before the app was installed. Try again.")
        #expect(AdbFailure(.streamClosed, activity: .bugReport).message
            == "The virtual machine\u{2019}s debugging service restarted before the bug report was finished. Try again.")
        // Turning USB debugging off restarts adbd under a transfer; the browser
        // works either way, so it is not told to check the setting.
        #expect(AdbFailure(.guestGone, activity: .files).message
            == "The virtual machine\u{2019}s debugging service restarted before the transfer finished. Try again.")
        #expect(BugReportStaging.NoRoom(bytes: 300 << 20, device: "iPad").errorDescription
            == "There isn\u{2019}t enough free space on this iPad for the report (300 MiB).")
    }

    // MARK: Staging

    @Test func theStagedReportKeepsTheReserveOfEveryOtherTransfer() throws {
        #expect(BugReportStaging.reserveBytes == Int64(TransferRoom.deviceReserveBytes))
        // Staging may not take the device to where the run is paused for space.
        #expect(BugReportStaging.reserveBytes > Int64(RunSpace.pauseBelowBytes))
        let size: UInt64 = 1 << 30
        let reserve = BugReportStaging.reserveBytes
        #expect(throws: BugReportStaging.NoRoom(bytes: size, device: "iPad")) {
            try BugReportStaging.makeSink(
                remotePath: "/data/b/bugreport-x.zip", size: size, device: "iPad",
                available: Int64(size) + reserve - 1)
        }
        let sink = try BugReportStaging.makeSink(
            remotePath: "/data/b/bugreport-x.zip", size: size, device: "iPad", available: Int64(size) + reserve)
        defer { BugReportStaging.sweep() }
        sink.close()
    }

    @Test func thePullsProgressReachesTheOverlayNoMoreOftenThanItNeedsIt() {
        let log = UpdateLog()
        let progress = BugReportOperation.pullingProgress(update: log.add)
        let total: UInt64 = 64 << 20
        var received: UInt64 = 0
        progress.report(0, total)
        while received < total {
            received += 64 << 10
            progress.report(received, total)
        }
        // 1,025 reports of the pull: the first, the last and a few between.
        #expect(log.progress.count < 40, "\(log.progress.count) updates")
        #expect(log.progress.first == BugReportOperation.generatingShare)
        #expect(abs((log.progress.last ?? 0) - 1) < 1e-9)
        #expect(log.progress == log.progress.sorted())
    }

    @Test func theStagedReportNeedsRoomBeyondItsOwnSize() throws {
        #expect(throws: BugReportStaging.NoRoom(bytes: 2_000_000_000, device: "iPad")) {
            try BugReportStaging.makeSink(
                remotePath: "/data/b/bugreport-x.zip", size: 2_000_000_000, device: "iPad",
                available: 2_000_000_000 + BugReportStaging.reserveBytes - 1)
        }
        let sink = try BugReportStaging.makeSink(
            remotePath: "/data/b/bugreport-x.zip", size: 10, device: "iPad", available: 2 << 30)
        defer { BugReportStaging.sweep() }
        try sink.write(Data("PK".utf8))
        sink.close()
        #expect(sink.url.deletingLastPathComponent() == BugReportStaging.directory)
        #expect(sink.url.lastPathComponent == "bugreport-x.zip")
        BugReportStaging.sweep()
        #expect(!FileManager.default.fileExists(atPath: sink.url.path))
    }

    // MARK: Install

    private func apk(named name: String) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString)-\(name)")
        try Data(count: 3_000_000).write(to: url)
        return url
    }

    @Test func anApkInstallsBehindTheOverlayAndTheResultStaysUntilItIsClosed() async throws {
        let (model, driver) = running()
        driver.install = { update in
            update(BusyUpdate(progress: 0.4))
            update(BusyUpdate(progress: 0.8, detail: "The virtual machine is installing the app\u{2026}", cancellable: false))
        }
        let url = try apk(named: "Signal-7.21.4.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(model.busy?.title == "Installing app")
        #expect(model.busy?.result == nil)
        #expect(await eventually { model.busy?.result != nil })
        // The overlay is still up, and says how it went.
        let busy = try #require(model.busy)
        #expect(busy.title == "App installed")
        #expect(busy.detail.hasSuffix("Signal-7.21.4.apk is installed in the virtual machine."))
        #expect(busy.result?.failed == false)
        #expect(busy.progress == nil && busy.cancel == nil)
        #expect(model.alert == nil)
        // Nothing else may start while it is up.
        model.createBugReport()
        #expect(model.busy?.title == "App installed")
        busy.result?.dismiss.action()
        #expect(model.busy == nil)
    }

    @Test func theCancelButtonGoesOnceTheLastByteIsSent() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.install = { update in
            update(BusyUpdate(progress: 0.8, detail: "The virtual machine is installing the app\u{2026}", cancellable: false))
            while !gate.isOpen { try await Task.sleep(for: .milliseconds(5)) }
        }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(model.busy?.cancel != nil)
        #expect(await eventually { model.busy?.cancel == nil })
        #expect(model.busy?.detail == "The virtual machine is installing the app\u{2026}")
        gate.release()
        #expect(await eventually { model.busy?.result != nil })
    }

    @Test func anInstallThatCopiesDataGetsItsCancelButtonBackForTheCopy() async throws {
        let (model, driver) = running()
        let gate = Gate()
        driver.install = { update in
            update(BusyUpdate(progress: 0.5, detail: "The virtual machine is installing the app\u{2026}", cancellable: false))
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(detail: "Copying app data\u{2026}", cancellable: true))
            // What the real driver does with a cancel once the app is in: it does not throw, it says what is left.
            while !gate.isOpen { try? await Task.sleep(for: .milliseconds(5)) }
        }
        driver.installOutcome = .installedWithoutData
        let url = try apk(named: "Game.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.cancel == nil })
        #expect(await eventually { model.busy?.cancel != nil && model.busy?.detail == "Copying app data\u{2026}" })
        // Pressing it stops the operation as it does anywhere else.
        model.busy?.cancel?.action()
        #expect(model.busy?.cancel == nil)
        gate.release()
        // The app is in, so the person is told what the cancel left: the result, not a vanished overlay.
        #expect(await eventually { model.busy?.result != nil })
        let busy = try #require(model.busy)
        #expect(busy.title == "App installed, some data missing" && busy.result?.style == .warning)
        #expect(busy.detail.hasSuffix("is installed in the virtual machine, but not all of its data files were copied. The app may not work without them. Install the file again to copy them."))
        #expect(model.alert == nil)
    }

    @Test func anAppInstalledWithoutAllItsDataSaysSoOnTheResult() async throws {
        let (model, driver) = running()
        driver.installOutcome = .installedWithoutData
        let url = try apk(named: "Game.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        let busy = try #require(model.busy)
        #expect(busy.title == "App installed, some data missing")
        #expect(busy.detail.hasSuffix("is installed in the virtual machine, but not all of its data files were copied. The app may not work without them. Install the file again to copy them."))
        #expect(busy.result?.style == .warning && busy.result?.failed == false)
        busy.result?.dismiss.action()
        #expect(model.busy == nil)
    }

    @Test func dataThatIsNeverCopiedIsNotToldToInstallAgain() async throws {
        let (model, driver) = running()
        driver.installOutcome = .installedDataNotPlaced
        let url = try apk(named: "Game.xapk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        let busy = try #require(model.busy)
        #expect(busy.title == "App installed, some data missing")
        #expect(busy.detail.hasSuffix("is installed in the virtual machine, but not all of its data files were copied. The app may not work without them."))
        #expect(busy.result?.style == .warning && busy.result?.failed == false)
        busy.result?.dismiss.action()
        #expect(model.busy == nil)
    }

    /// The three ways an install ends are three presentations: the plain check, the warning for an app that
    /// is in with some of its data missing (both of its outcomes), and the failure.
    @Test func anInstallEndsAsASuccessAWarningOrAFailureAndTheyLookDifferent() async throws {
        var styles: [BusyResult.Style] = []
        var titles: [String] = []
        for outcome in [ApkInstallOutcome.installed, .installedWithoutData, .installedDataNotPlaced] {
            let (model, driver) = running()
            driver.installOutcome = outcome
            let url = try apk(named: "Game.xapk")
            defer { try? FileManager.default.removeItem(at: url) }
            model.installPicked(url)
            #expect(await eventually { model.busy?.result != nil })
            let busy = try #require(model.busy)
            styles.append(try #require(busy.result?.style))
            titles.append(busy.title)
        }
        #expect(styles == [.success, .warning, .warning])
        #expect(titles == ["App installed", "App installed, some data missing", "App installed, some data missing"])
        // A refusal is the failure.
        let (model, driver) = running()
        driver.install = { _ in throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_VERSION_DOWNGRADE")) }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.result?.style == .failure)
        // The words of an outcome say the same on their own, and the card draws the three differently.
        #expect(ApkInstallOutcome.installed.words(fileName: "x").style == .success)
        #expect(ApkInstallOutcome.installedWithoutData.words(fileName: "x").style == .warning)
        #expect(ApkInstallOutcome.installedDataNotPlaced.words(fileName: "x").style == .warning)
        let drawn = [BusyResult.Style.success, .warning, .failure].map { (BusyOverlay.symbol(for: $0), BusyOverlay.color(for: $0)) }
        #expect(drawn[0].0 == "checkmark.circle.fill" && drawn[1].0 == "exclamationmark.triangle.fill" && drawn[2].0 == "exclamationmark.triangle.fill")
        #expect(drawn[1].1 == .orange && drawn[2].1 == .red && drawn[0].1 != drawn[1].1 && drawn[1].1 != drawn[2].1)
    }

    @Test func cancellingTheInstallLeavesNoResultAndNoAlert() async throws {
        let (model, driver) = running()
        driver.install = { _ in
            while true { try await Task.sleep(for: .milliseconds(5)) }
        }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        model.busy?.cancel?.action()
        #expect(await eventually { model.busy == nil })
        #expect(model.alert == nil)
    }

    @Test func aRefusalByTheGuestIsTheResultOnTheOverlayNotAnAlert() async throws {
        let (model, driver) = running()
        driver.install = { _ in
            throw AdbFailure(message: PackageInstallMessages.message(forCode: "FAILED_VERSION_DOWNGRADE"))
        }
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.title == "App not installed")
        #expect(model.busy?.detail == "A newer version of this app is already installed.")
        #expect(model.busy?.result?.failed == true)
        #expect(model.alert == nil)
        model.busy?.result?.dismiss.action()
        #expect(model.busy == nil)
    }

    @Test func aFileThatIsNotAnAppIsRefusedAtOnceOnTheSameOverlay() throws {
        let (model, driver) = running()
        let expected: [(String, InstallRefusal)] = [
            ("Trip.zip", .notApk), ("app.aab", .notApk), ("app.apk.zip", .notApk), ("notes.txt", .notApk),
        ]
        for (name, refusal) in expected {
            let url = try apk(named: name)
            defer { try? FileManager.default.removeItem(at: url) }
            model.installPicked(url)
            let busy = try #require(model.busy, "\(name)")
            #expect(busy.title == "App not installed", "\(name)")
            #expect(busy.detail == refusal.message, "\(name)")
            #expect(busy.result?.failed == true && busy.cancel == nil && busy.progress == nil, "\(name)")
            #expect(model.alert == nil)
            busy.result?.dismiss.action()
            #expect(model.busy == nil)
        }
        // Nothing was sent to the guest.
        #expect(driver.installed == 0)
    }

    @Test func anApkTooLargeForAndroidIsRefusedAtOnceOnTheSameOverlayAndNothingIsSent() throws {
        let (model, driver) = running()
        let url = try makeSparseFile(bytes: Int(InstallRefusal.singleApkLimit), name: "Huge.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        let busy = try #require(model.busy)
        #expect(busy.title == "App not installed")
        #expect(busy.detail == InstallRefusal.tooLarge.message)
        #expect(busy.result?.failed == true && busy.cancel == nil && busy.progress == nil)
        #expect(model.alert == nil)
        busy.result?.dismiss.action()
        #expect(model.busy == nil)
        #expect(driver.installed == 0)
    }

    @Test func aFileWithNoSizeYetShowsItsNameAloneUntilTheInstallHasIt() throws {
        let (model, _) = running()
        // What the picker reads of an iCloud file that is not down.
        let url = try makeSparseFile(bytes: 0, name: "Cloud.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(model.busy?.detail == url.lastPathComponent)
    }

    @Test func theEndOfTheRunTakesAFinishedResultAway() async throws {
        let (model, _) = running()
        let url = try apk(named: "a.apk")
        defer { try? FileManager.default.removeItem(at: url) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.busy == nil)
    }
}

/// ADB over network as the model plays it: the toggle, the guest's USB
/// debugging setting, the address, and the end of a run.
@MainActor
struct AdbOverNetworkModelTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func running(_ mode: StartMode = .normal) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(mode)
        model.adbWatchInterval = .milliseconds(20)
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    @Test func theToggleWaitsForTheGuestsUsbDebuggingToBeKnownOn() async {
        let (model, driver) = running()
        #expect(!model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && driver.starts == 0)
        driver.debugging = false
        await model.refreshUsbDebugging()
        #expect(model.session.usbDebugging == false && !model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && driver.starts == 0)
        driver.debugging = true
        await model.refreshUsbDebugging()
        #expect(model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && driver.listening && driver.starts == 1)
        #expect(model.session.adbAddress == "192.0.2.23")
        // Again changes nothing.
        model.setAdbOverNetwork(true)
        #expect(driver.starts == 1)
        model.setAdbOverNetwork(false)
        #expect(!model.session.adb && !driver.listening && model.session.adbAddress == nil)
    }

    @Test func usbDebuggingGoingOffSwitchesItOffAndClosesTheComputersConnected() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        #expect(model.session.adb)
        driver.connectedComputers = 2
        driver.debugging = nil
        await model.refreshUsbDebugging()
        #expect(model.session.adb && driver.listening, "a setting that cannot be read changes nothing")
        #expect(driver.closedComputers == 0)
        driver.debugging = false
        await model.refreshUsbDebugging()
        #expect(!model.session.adb && !driver.listening)
        // The listener goes and so does every connection already made.
        #expect(driver.closedComputers == 2 && driver.connectedComputers == 0)
        #expect(!model.adbOverNetworkAvailable)
    }

    @Test func theWatcherNoticesUsbDebuggingGoingOffWithSettingsClosed() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        driver.connectedComputers = 1
        driver.debugging = false
        #expect(await eventually { !model.session.adb && !driver.listening })
        #expect(driver.closedComputers == 1)
    }

    @Test func theAddressFollowsTheNetworkAndMayBeMissing() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        driver.address = nil
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && model.session.adbAddress == nil)
        driver.address = "198.51.100.8"
        #expect(await eventually { model.session.adbAddress == "198.51.100.8" })
        driver.address = nil
        #expect(await eventually { model.session.adbAddress == nil })
        model.setAdbOverNetwork(false)
    }

    @Test func aPortInUseIsSaidAndTheToggleStaysOff() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        driver.startError = AdbError.io(EADDRINUSE)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && !driver.listening)
        #expect(model.session.adbProblem == "Port 5555 is already in use on this device.")
        driver.startError = POSIXError(.EPERM)
        model.setAdbOverNetwork(true)
        #expect(model.session.adbProblem == "ADB over network could not start.")
        driver.startError = nil
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && model.session.adbProblem == nil)
        model.setAdbOverNetwork(false)
    }

    @Test func theEndOfARunClosesItAndForgetsTheSetting() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 10)
        #expect(!model.session.adb && !driver.listening)
        #expect(model.session.usbDebugging == nil)
    }

    @Test func aRestartClosesItToo() async {
        let (model, driver) = running()
        await model.refreshUsbDebugging()
        model.setAdbOverNetwork(true)
        model.restart()
        #expect(!model.session.adb && !driver.listening)
        #expect(model.session.usbDebugging == nil)
    }

    @Test func recoveryOffersNoAdb() async {
        let (model, driver) = running(.recovery)
        await model.refreshUsbDebugging()
        #expect(model.session.usbDebugging == nil)
        model.setAdbOverNetwork(true)
        #expect(!model.session.adb && driver.starts == 0)
    }

    @Test func thePreviewHasNoGuestToAskAndSaysYes() {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .preview,
            createRequirementBytes: 1
        )
        model.phase = .running(.normal)
        #expect(model.adbOverNetworkAvailable)
        model.setAdbOverNetwork(true)
        #expect(model.session.adb && model.session.adbAddress == SessionState.previewAddress)
        model.setAdbOverNetwork(false)
        model.session.usbDebugging = false
        #expect(!model.adbOverNetworkAvailable)
    }
}

struct PackageInstallMessagesTests {
    @Test func everyCodeInTheTableHasItsMessage() {
        let expected: [String: String] = [
            "PARSE_FAILED_NOT_APK": "The file isn\u{2019}t a valid app.",
            "FAILED_INVALID_APK": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_BAD_MANIFEST": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_MANIFEST_MALFORMED": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_UNEXPECTED_EXCEPTION": "The file isn\u{2019}t a valid app.",
            "PARSE_FAILED_NO_CERTIFICATES": "The app isn\u{2019}t signed correctly.",
            "PARSE_FAILED_INCONSISTENT_CERTIFICATES": "The app isn\u{2019}t signed correctly.",
            "PARSE_FAILED_CERTIFICATE_ENCODING": "The app isn\u{2019}t signed correctly.",
            "FAILED_UPDATE_INCOMPATIBLE": "A version of this app signed by someone else is installed. Uninstall it in the virtual machine first.",
            "FAILED_SHARED_USER_INCOMPATIBLE": "A version of this app signed by someone else is installed. Uninstall it in the virtual machine first.",
            "FAILED_VERSION_DOWNGRADE": "A newer version of this app is already installed.",
            "FAILED_OLDER_SDK": "This app needs a newer system than this virtual machine runs.",
            "FAILED_DEPRECATED_SDK_VERSION": "This app was made for a system version that this virtual machine no longer installs.",
            "FAILED_NO_MATCHING_ABIS": "This app has no code for this device\u{2019}s processor. Choose a version of the app made for ARM processors.",
            "FAILED_INSUFFICIENT_STORAGE": "The virtual machine doesn\u{2019}t have enough free space. Free some up in the virtual machine, or give it more storage with Resize.",
            "FAILED_MISSING_SPLIT": "Some parts of this app are missing. Choose an .apks, .xapk or .apkm file that holds all of them.",
            "FAILED_MISSING_SHARED_LIBRARY": "The app needs a system library this virtual machine doesn\u{2019}t have.",
            "FAILED_DUPLICATE_PERMISSION": "The app conflicts with an app that\u{2019}s already installed.",
            "FAILED_CONFLICTING_PROVIDER": "The app conflicts with an app that\u{2019}s already installed.",
            "FAILED_DUPLICATE_PACKAGE": "The app conflicts with an app that\u{2019}s already installed.",
            "FAILED_USER_RESTRICTED": "The virtual machine refused to install the app.",
            "FAILED_VERIFICATION_FAILURE": "The virtual machine refused to install the app.",
            "FAILED_ABORTED": "The virtual machine refused to install the app.",
        ]
        #expect(Set(PackageInstallMessages.mappedCodes) == Set(expected.keys))
        for (code, message) in expected {
            #expect(PackageInstallMessages.message(forCode: code) == message, "\(code)")
            let output = "Failure [INSTALL_\(code): something detailed]\n"
            #expect(PackageInstallMessages.failure(inOutput: output) == message, "\(code)")
        }
    }

    @Test func anUnknownCodeIsNamed() {
        #expect(PackageInstallMessages.failure(inOutput: "Failure [INSTALL_FAILED_TEST_ONLY: x]\n")
            == "The virtual machine couldn\u{2019}t install the app (FAILED_TEST_ONLY).")
    }

    @Test func successIsNil() {
        #expect(PackageInstallMessages.failure(inOutput: "Success\n") == nil)
        #expect(PackageInstallMessages.failure(inOutput: "Performing Streamed Install\nSuccess\n") == nil)
    }

    @Test func aSuccessLineInsideAFailureIsStillTheFailure() {
        // The diagnostic of an APK's own manifest: a name that is a newline and `Success`.
        let output = "Failure [INSTALL_PARSE_FAILED_MANIFEST_MALFORMED: Bad uses-sdk-library declaration name:\nSuccess version: -1]\n"
        #expect(PackageInstallMessages.failure(inOutput: output) == "The file isn\u{2019}t a valid app.")
        #expect(PackageInstallMessages.failure(inOutput: "Failure [INSTALL_FAILED_OLDER_SDK: x\nSuccess\n]\n")
            == PackageInstallMessages.message(forCode: "FAILED_OLDER_SDK"))
        // Anything but the answer being Success is not one.
        #expect(PackageInstallMessages.failure(inOutput: "warning: x\nSuccess\n") == "The virtual machine couldn\u{2019}t install the app.")
        #expect(PackageInstallMessages.failure(inOutput: "Exception occurred while executing 'install-commit':\njava.lang.IllegalStateException: x\nSuccess\n")
            == "The virtual machine couldn\u{2019}t install the app.")
    }

    @Test func theStorageExceptionOfCreateSessionIsTheStorageMessage() {
        let storage = PackageInstallMessages.message(forCode: "FAILED_INSUFFICIENT_STORAGE")
        let exception = """
            Exception occurred while executing 'install':
            android.os.ParcelableException: java.io.IOException: Requested internal only, but not enough space
            \tat android.util.ExceptionUtils.wrap(ExceptionUtils.java:34)

            """
        #expect(PackageInstallMessages.failure(inOutput: exception) == storage)
        #expect(PackageInstallMessages.failure(inOutput: "Error: java.io.IOException: No space left on device\n") == storage)
        // A line that names a code decides, whatever else is printed.
        #expect(PackageInstallMessages.failure(inOutput: "not enough space\nFailure [INSTALL_FAILED_OLDER_SDK]\n")
            == PackageInstallMessages.message(forCode: "FAILED_OLDER_SDK"))
        // Another exception names nothing.
        #expect(PackageInstallMessages.failure(inOutput: "Exception occurred while executing 'install':\njava.lang.SecurityException: x\n")
            == "The virtual machine couldn\u{2019}t install the app.")
    }

    @Test func outputWithoutAVerdictIsAFailure() {
        #expect(PackageInstallMessages.failure(inOutput: "") == "The virtual machine couldn\u{2019}t install the app.")
        #expect(PackageInstallMessages.failure(inOutput: "Failure\n") == "The virtual machine couldn\u{2019}t install the app.")
    }

    @Test func theCodeIsReadWhateverFollowsIt() {
        #expect(PackageInstallMessages.code(inFailure: "Failure [INSTALL_FAILED_OLDER_SDK]") == "FAILED_OLDER_SDK")
        #expect(PackageInstallMessages.code(inFailure: "Failure [FAILED_ABORTED: Session was abandoned]") == "FAILED_ABORTED")
        #expect(PackageInstallMessages.code(inFailure: "Failure") == nil)
    }
}
