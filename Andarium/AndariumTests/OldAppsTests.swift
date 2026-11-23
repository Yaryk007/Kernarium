// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Old app compatibility as the model plays it with a stub for the runtime and
/// a scripted list for the image's command: when the virtual machine is asked,
/// what a switch asks the person before it writes, what the image's answers
/// leave, and the hint after Install anyway.
@Suite(.serialized)
@MainActor
struct OldAppsModelTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let system = SystemInfo(
        version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
    private let pixel = ScriptedOldApps.sampleApps[0]
    private let rally = ScriptedOldApps.sampleApps[1]

    /// A running session, Android's storage up, and an image whose list is
    /// scripted. Rally 2011 is open.
    private func running(storageReady: Bool? = true, mode: StartMode = .normal) -> (AppModel, StubDriver, ScriptedOldApps) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.phase = .running(mode)
        model.session.storageReady = storageReady
        let scripted = ScriptedOldApps(open: ["com.example.rally"])
        scripted.delay = .milliseconds(1)
        model.scriptedOldApps = scripted
        return (model, driver, scripted)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    // MARK: When the virtual machine is asked

    @Test func theFirstListingWaitsForAndroidsStorageAndIsAskedOnce() async {
        let (model, _, scripted) = running(storageReady: false)
        let state = model.session.oldApps
        await model.listOldAppsOnce()
        #expect(scripted.listings == 0 && !state.offered)
        model.session.storageReady = true
        await model.listOldAppsOnce()
        #expect(scripted.listings == 1)
        #expect(state.offered && state.apps.map(\.package) == [pixel.package, rally.package])
        // The row is offered once; opening Settings again asks nothing more.
        await model.listOldAppsOnce()
        #expect(scripted.listings == 1)
    }

    @Test func aListingThatGotNoAnswerIsAskedAgain() async {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        scripted.listingAnswer = .failed
        await model.listOldAppsOnce()
        #expect(state.failed && !state.listed && !state.offered)
        await model.listOldAppsOnce()
        #expect(scripted.listings == 2)
        scripted.listingAnswer = nil
        await model.listOldAppsOnce()
        #expect(state.listed && !state.failed && state.offered && scripted.listings == 3)
    }

    @Test func aFailureAfterAListingKeepsTheRowAndSaysSoOnThePage() async {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        scripted.listingAnswer = .failed
        await model.refreshOldApps()
        #expect(state.failed && state.offered && state.apps.count == 2)
        scripted.listingAnswer = nil
        await model.refreshOldApps()
        #expect(!state.failed)
    }

    @Test func aMissingCommandIsAskedOnceAndOffersNothing() async {
        let (model, _, scripted) = running()
        scripted.listingAnswer = .unavailable
        await model.listOldAppsOnce()
        await model.listOldAppsOnce()
        await model.refreshOldApps()
        #expect(scripted.listings == 1)
        #expect(!model.session.oldApps.offered && model.session.oldApps.unavailable)
    }

    @Test func aListWithNoAppOffersNothingButTheEmptyPageStaysReachable() async {
        let (model, _, scripted) = running()
        scripted.listingAnswer = .apps([])
        await model.refreshOldApps()
        #expect(!model.session.oldApps.offered && model.session.oldApps.listed && model.session.oldApps.apps.isEmpty)
    }

    @Test func recoveryAsksNothing() async {
        let (model, _, scripted) = running(mode: .recovery)
        #expect(await model.refreshOldApps() == nil)
        await model.listOldAppsOnce()
        #expect(scripted.listings == 0 && !model.session.oldApps.offered)
    }

    @Test func aRunThatEndsTakesTheListWithIt() async {
        let (model, _, _) = running()
        await model.refreshOldApps()
        #expect(model.session.oldApps.offered)
        model.session.oldApps.reset()
        #expect(!model.session.oldApps.offered && !model.session.oldApps.listed)
    }

    // MARK: The switch

    @Test func turningOnIsAskedAndNothingIsWrittenUntilTheAnswer() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        await model.requestOldApp(pixel.package, on: true)
        #expect(state.prompt == .turnOn(pixel, open: false))
        #expect(scripted.writes.isEmpty && state.busy == nil && state.apps.allSatisfy { $0.package != pixel.package || !$0.isOn })
        // Cancel changes nothing.
        await model.answerOldApps(try #require(state.prompt), confirmed: false)
        #expect(state.prompt == nil && scripted.writes.isEmpty)
        // Turn on writes, and the page then shows it on.
        await model.requestOldApp(pixel.package, on: true)
        await model.answerOldApps(try #require(state.prompt), confirmed: true)
        #expect(scripted.writes.count == 1 && scripted.writes[0].package == pixel.package && scripted.writes[0].on)
        let written = try #require(state.apps.first { $0.package == pixel.package })
        #expect(written.isOn && written.turnedOn > 0 && state.busy == nil && state.prompt == nil)
        #expect(state.anyOn)
    }

    @Test func anOpenAppIsToldItClosesNow() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        scripted.open.insert(pixel.package)
        await model.refreshOldApps()
        await model.requestOldApp(pixel.package, on: true)
        let prompt = try #require(state.prompt)
        #expect(prompt == .turnOn(pixel, open: true))
        #expect(OldAppsWords.title(for: prompt) == "Turn on for \u{201C}Pixel Knights\u{201D}?")
        let message = OldAppsWords.message(for: prompt)
        #expect(message.hasSuffix("\u{201C}Pixel Knights\u{201D} is open and will close now."))
        #expect(message.contains("You can turn it off again here."))
        // An app that is not open is told nothing of the kind.
        scripted.open = []
        state.prompt = nil
        await model.requestOldApp(pixel.package, on: true)
        let quiet = OldAppsWords.message(for: try #require(state.prompt))
        #expect(!quiet.contains("will close now") && quiet.hasSuffix("You can turn it off again here."))
    }

    @Test func turningOffAsksOnlyForAnAppThatIsOpen() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        // Rally 2011 is on and open.
        await model.requestOldApp(rally.package, on: false)
        let prompt = try #require(state.prompt)
        #expect(prompt == .turnOff(rally))
        #expect(OldAppsWords.title(for: prompt) == "Turn off for \u{201C}Rally 2011\u{201D}?")
        #expect(
            OldAppsWords.message(for: prompt)
                == "\u{201C}Rally 2011\u{201D} is open and will close now. It will open without Old app compatibility next time.")
        #expect(scripted.writes.isEmpty)
        await model.answerOldApps(prompt, confirmed: true)
        #expect(scripted.writes.count == 1 && !scripted.writes[0].on && !state.anyOn)
        // Not open: no question, the switch goes at once.
        await model.requestOldApp(pixel.package, on: true)
        await model.answerOldApps(try #require(state.prompt), confirmed: true)
        scripted.open = []
        await model.requestOldApp(pixel.package, on: false)
        #expect(state.prompt == nil && scripted.writes.count == 3 && !scripted.writes[2].on)
        #expect(state.apps.allSatisfy { !$0.isOn })
    }

    @Test func aSwitchThatIsAlreadyWhereItIsAsksNothing() async {
        let (model, _, scripted) = running()
        await model.refreshOldApps()
        await model.requestOldApp(rally.package, on: true)
        await model.requestOldApp("com.example.unknown", on: true)
        #expect(model.session.oldApps.prompt == nil && scripted.writes.isEmpty)
    }

    @Test func aChangeTheImageRefusesIsSaidAndTheListIsReadAgain() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        scripted.writeAnswer = .failed
        await model.requestOldApp(pixel.package, on: true)
        await model.answerOldApps(try #require(state.prompt), confirmed: true)
        #expect(state.prompt == .notChanged)
        #expect(OldAppsWords.title(for: .notChanged) == "Old app compatibility wasn\u{2019}t changed")
        #expect(OldAppsWords.message(for: .notChanged) == "The virtual machine didn\u{2019}t accept the change. Try again.")
        #expect(scripted.listings == 2 && state.busy == nil)
        #expect(state.apps.first { $0.package == pixel.package }?.isOn == false)
        // OK changes nothing more.
        await model.answerOldApps(.notChanged, confirmed: true)
        #expect(state.prompt == nil && scripted.writes.count == 1)
    }

    @Test func anAppThatIsNoLongerEligibleLeavesTheList() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        scripted.writeAnswer = .notEligible
        scripted.remove(pixel.package)
        await model.requestOldApp(pixel.package, on: true)
        await model.answerOldApps(try #require(state.prompt), confirmed: true)
        #expect(state.prompt == .notChanged)
        #expect(state.apps.map(\.package) == [rally.package])
    }

    @Test func onlyOneChangeIsAskedAtATime() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        scripted.delay = .milliseconds(150)
        await model.refreshOldApps()
        let first = Task { await model.requestOldApp(pixel.package, on: true) }
        #expect(await eventually { state.busy == pixel.package })
        await model.requestOldApp(rally.package, on: false)
        await first.value
        #expect(state.prompt == .turnOn(pixel, open: false))
        // A question stands: no other is asked until it is answered.
        await model.requestOldApp(rally.package, on: false)
        #expect(state.prompt == .turnOn(pixel, open: false) && scripted.writes.isEmpty)
    }

    // MARK: The run's first listing

    /// A session whose boot screen has not ended: the run's own start, then the image's list scripted.
    private func booting(
        storage: Bool? = true, capabilities: Capabilities = .device
    ) -> (AppModel, StubDriver, ScriptedOldApps) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: capabilities, createRequirementBytes: 1)
        let driver = StubDriver(plan: .normal)
        driver.storage = storage
        model.driver = driver
        model.refreshPlan()
        let scripted = ScriptedOldApps(open: [])
        scripted.delay = .milliseconds(1)
        model.scriptedOldApps = scripted
        // The attempts at the first listing wait 20, 40, 80, 160 and 320 ms here.
        model.session.oldApps.retryInterval = .milliseconds(20)
        return (model, driver, scripted)
    }

    @Test func aRunListsOnceWhenAndroidIsUpAndSettingsIsNeverOpened() async {
        let (model, _, scripted) = booting()
        let state = model.session.oldApps
        model.start(.normal)
        model.finishBoot(.normal)
        #expect(await eventually { state.offered })
        // One listing, and nothing is left running to ask again.
        try? await Task.sleep(for: .milliseconds(150))
        #expect(scripted.listings == 1 && state.first == nil && state.listed)
        #expect(scripted.limits == [nil])
    }

    @Test func theFirstListingWaitsForAndroidsStorageThenAsksOnce() async {
        let (model, driver, scripted) = booting(storage: false)
        let state = model.session.oldApps
        model.start(.normal)
        model.finishBoot(.normal)
        try? await Task.sleep(for: .milliseconds(50))
        #expect(scripted.listings == 0 && state.first != nil && !state.offered)
        driver.storage = true
        #expect(await eventually { state.offered })
        try? await Task.sleep(for: .milliseconds(100))
        #expect(scripted.listings == 1 && state.first == nil)
    }

    @Test func aFirstListingWithNoAnswerIsAskedAFewTimesAndThenStops() async {
        let (model, _, scripted) = booting()
        let state = model.session.oldApps
        scripted.listingAnswer = .failed
        model.start(.normal)
        model.finishBoot(.normal)
        #expect(await eventually { state.first == nil })
        #expect(scripted.listings == OldAppsState.startAttempts && !state.listed && state.failed)
        // Nothing wakes afterwards: a run with an unanswered listing asks nothing more by itself.
        try? await Task.sleep(for: .milliseconds(150))
        #expect(scripted.listings == OldAppsState.startAttempts)
    }

    @Test func theFirstListingEndsAtTheFirstAnswerFromAnywhere() async {
        let (model, _, scripted) = booting(storage: false)
        let state = model.session.oldApps
        state.retryInterval = .seconds(30)
        model.start(.normal)
        model.finishBoot(.normal)
        #expect(await eventually { state.first != nil })
        // Settings, opened meanwhile, listed: the waiting task is gone, not left to wake.
        model.session.storageReady = true
        await model.listOldAppsOnce()
        #expect(state.listed && state.first == nil && scripted.listings == 1)
    }

    @Test func recoveryAndAnImageWithoutAdbListNothingAtTheStart() async {
        let (model, _, scripted) = booting()
        model.start(.recovery)
        model.finishBoot(.recovery)
        let (bare, _, bareScripted) = booting(
            capabilities: Capabilities(adbOverNetwork: false, apkInstall: false, bugReport: false, statistics: false, guestFiles: false,
                textMessages: false, screenReader: false))
        bare.start(.normal)
        bare.finishBoot(.normal)
        try? await Task.sleep(for: .milliseconds(150))
        #expect(scripted.listings == 0 && model.session.oldApps.first == nil && !model.session.oldApps.offered)
        #expect(bareScripted.listings == 0 && bare.session.oldApps.first == nil)
    }

    // MARK: A listing is not older than a change

    @Test func aListingAskedBeforeAChangeFinishedDoesNotOverwriteIt() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        // The image answers with Pixel Knights off, as it was when it was asked, but only after a while.
        scripted.listDelay = .milliseconds(250)
        let slow = Task { await model.refreshOldApps() }
        #expect(await eventually { scripted.listings == 2 })
        await model.requestOldApp(pixel.package, on: true)
        await model.answerOldApps(try #require(state.prompt), confirmed: true)
        #expect(state.apps.first { $0.package == pixel.package }?.isOn == true)
        scripted.listDelay = nil
        _ = await slow.value
        // The older answer was dropped and the image asked again, which has the change.
        #expect(state.apps.first { $0.package == pixel.package }?.isOn == true)
        #expect(scripted.listings == 3 && !state.listing)
    }

    // MARK: A page that is gone

    @Test func nothingIsAskedOfAPageThatWasClosedMeanwhile() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        state.pageOpened()
        scripted.delay = .milliseconds(120)
        let ask = Task { await model.requestOldApp(pixel.package, on: true) }
        #expect(await eventually { state.busy == pixel.package })
        state.pageClosed()
        await ask.value
        // No alert for a page nobody sees, and the switches are not left dead.
        #expect(state.prompt == nil && state.busy == nil && scripted.writes.isEmpty)
        scripted.delay = .milliseconds(1)
        state.pageOpened()
        await model.requestOldApp(pixel.package, on: true)
        #expect(state.prompt == .turnOn(pixel, open: false))
    }

    @Test func aRefusalThatComesAfterThePageWasClosedRaisesNoAlert() async throws {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        state.pageOpened()
        await model.requestOldApp(pixel.package, on: true)
        let prompt = try #require(state.prompt)
        scripted.writeAnswer = .failed
        scripted.delay = .milliseconds(120)
        let answer = Task { await model.answerOldApps(prompt, confirmed: true) }
        #expect(await eventually { state.busy == pixel.package })
        state.pageClosed()
        await answer.value
        #expect(state.prompt == nil && state.busy == nil && scripted.writes.count == 1)
        // The list was read again all the same.
        #expect(scripted.listings == 2)
    }

    @Test func anAlertLeftOverIsGoneWhenThePageOpens() async {
        let (model, _, _) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        state.prompt = .notChanged
        state.pageOpened()
        #expect(state.prompt == nil)
        await model.requestOldApp(pixel.package, on: true)
        #expect(state.prompt == .turnOn(pixel, open: false))
    }

    // MARK: A virtual machine that cannot be asked

    @Test func aSwitchIsRefusedAtOnceWhileTheVirtualMachineCannotBeAsked() async {
        let (model, _, scripted) = running()
        let state = model.session.oldApps
        await model.refreshOldApps()
        #expect(model.canAskOldApps)
        model.phase = .running(.recovery)
        #expect(!model.canAskOldApps)
        await model.requestOldApp(pixel.package, on: true)
        #expect(state.prompt == nil && state.busy == nil && scripted.writes.isEmpty)
        // Paused for space is the same.
        model.phase = .running(.normal)
        model.spacePause = SpacePause(freeBytes: 820 * Limits.bytesPerMiB, stage: .pausing)
        await model.requestOldApp(pixel.package, on: true)
        #expect(!model.canAskOldApps && state.prompt == nil && state.busy == nil)
    }

    // MARK: After Install anyway

    private let refused = LegacyTarget(appLevel: 7, requiredLevel: 24)
    private let installed = "Gears.apk is installed in the virtual machine."

    private func apk() throws -> URL {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let url = folder.appendingPathComponent("Gears.apk")
        try Data(count: 3 << 20).write(to: url)
        return url
    }

    /// The first attempt is refused for the version; Install anyway runs `second`.
    private func refusing(
        _ driver: StubDriver, target: LegacyTarget, then second: @escaping @Sendable () async -> Void = {}
    ) {
        driver.installAttempt = { bypass, _ in
            if !bypass { throw LegacyApkRefused(target: target) }
            await second()
        }
    }

    /// Plays the install through Install anyway and returns the card it ends on.
    private func installAnyway(_ model: AppModel) async throws -> BusyState? {
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        model.installPicked(url)
        #expect(await eventually { model.legacyInstallQuestion != nil })
        model.answerLegacyInstall(installAnyway: true)
        #expect(await eventually { model.busy?.result != nil })
        return model.busy
    }

    @Test func installAnywayOfAnOldAppSaysWhereTheSwitchIsWhenTheListingNamesTheApp() async throws {
        let (model, driver, scripted) = running()
        let seen = Counter()
        refusing(driver, target: refused) {
            // The listing before the file went again is what the one after is compared with.
            seen.set(await scripted.listings)
            await scripted.install(OldApp(package: "com.example.gears", label: "Gears", isOn: false, lastUpdate: 1_800_000_000))
        }
        let card = try await installAnyway(model)
        #expect(card?.title == "App installed")
        #expect(card?.detail == "\(installed) \(OldAppsWords.installHint)")
        #expect(card?.detail.hasSuffix("open Settings from the side keys and choose Old app compatibility.") == true)
        #expect(card?.result?.failed == false)
        #expect(seen.value == 1 && scripted.listings == 2)
        // The two listings around the install wait only a few seconds: the install's result waits for them.
        #expect(scripted.limits == [.seconds(5), .seconds(5)])
        // The listing that named the app is the row's: it is offered now.
        #expect(model.session.oldApps.apps.map(\.package).contains("com.example.gears"))
    }

    @Test func theHintFollowsTheWarningOfAnAppWhoseDataWasNotAllCopied() async throws {
        let (model, driver, scripted) = running()
        driver.installOutcome = .installedWithoutData
        refusing(driver, target: refused) {
            await scripted.install(OldApp(package: "com.example.gears", label: "Gears", isOn: false, lastUpdate: 1_800_000_000))
        }
        let card = try await installAnyway(model)
        #expect(card?.title == "App installed, some data missing")
        #expect(card?.detail.hasSuffix(OldAppsWords.installHint) == true)
        #expect(card?.detail.contains("Install the file again to copy them.") == true)
        #expect(card?.result?.style == .warning)
    }

    @Test func noHintForAnAppFromAnAndroidThatIsNotBeforeFourPointZero() async throws {
        let (model, driver, scripted) = running()
        refusing(driver, target: LegacyTarget(appLevel: 14, requiredLevel: 24)) {
            await scripted.install(OldApp(package: "com.example.gears", label: "Gears", isOn: false, lastUpdate: 1_800_000_000))
        }
        let card = try await installAnyway(model)
        #expect(card?.detail == installed)
        // The refusal that names no level gets none either.
        let (second, secondDriver, secondScripted) = running()
        refusing(secondDriver, target: LegacyTarget(appLevel: nil, requiredLevel: 24)) {
            await secondScripted.install(OldApp(package: "com.example.gears", label: "Gears", isOn: false, lastUpdate: 1_800_000_000))
        }
        #expect(try await installAnyway(second)?.detail == installed)
    }

    @Test func noHintWhenTheListingNamesNoNewApp() async throws {
        // The installed app is one the switch does not apply to (say it is 64-bit): the list is as it was.
        let (model, driver, _) = running()
        refusing(driver, target: refused)
        #expect(try await installAnyway(model)?.detail == installed)
        // Nor when the image has no command, or does not answer.
        let (gone, goneDriver, goneScripted) = running()
        goneScripted.listingAnswer = .unavailable
        refusing(goneDriver, target: refused)
        #expect(try await installAnyway(gone)?.detail == installed)
        let (mute, muteDriver, muteScripted) = running()
        muteScripted.listingAnswer = .failed
        refusing(muteDriver, target: refused)
        #expect(try await installAnyway(mute)?.detail == installed)
        // A listing around the install that timed out is no hint and nothing else: the page does not
        // say the virtual machine did not answer, and no listing is left in doubt.
        let (quiet, _, quietScripted) = running()
        quietScripted.listingAnswer = .failed
        #expect(await quiet.oldAppsBeforeInstall() == nil)
        #expect(await quiet.installAddedOldApp(listedBefore: []) == false)
        #expect(!quiet.session.oldApps.failed && !quiet.session.oldApps.listing && quietScripted.limits == [.seconds(5), .seconds(5)])
    }

    @Test func anOrdinaryInstallListsAgainAndSaysNothingMore() async throws {
        let (model, _, scripted) = running()
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.detail == installed)
        #expect(await eventually { scripted.listings == 1 })
        #expect(await eventually { model.session.oldApps.offered })
    }

    @Test func aFailedInstallListsNothing() async throws {
        let (model, driver, scripted) = running()
        driver.install = { _ in throw AdbFailure(message: "no") }
        let url = try apk()
        defer { try? FileManager.default.removeItem(at: url.deletingLastPathComponent()) }
        model.installPicked(url)
        #expect(await eventually { model.busy?.result != nil })
        #expect(model.busy?.result?.failed == true)
        try await Task.sleep(for: .milliseconds(50))
        #expect(scripted.listings == 0)
    }

    private final class Counter: @unchecked Sendable {
        private let lock = NSLock()
        private var number = 0
        var value: Int { lock.withLock { number } }
        func set(_ value: Int) { lock.withLock { number = value } }
    }
}

/// The words, in every language the application is in.
struct OldAppsWordsTests {
    private static let languages = ["ja", "zh-Hans", "zh-Hant"]

    private func text(_ key: String.LocalizationValue, in language: String) -> String {
        String(localized: LocalizedStringResource(key, locale: Locale(identifier: language), bundle: .main))
    }

    @Test(arguments: OldAppsWordsTests.languages)
    func everyWordIsTranslated(language: String) {
        let keys: [String.LocalizationValue] = [
            "Old app compatibility", "On", "Looking for old apps\u{2026}", "Built for Android before 4.0",
            "No app built for Android before 4.0 is installed.",
            "The virtual machine didn\u{2019}t answer. Try again.", "Try again", "Turn on", "Turn off",
            "Old app compatibility wasn\u{2019}t changed",
            "The virtual machine didn\u{2019}t accept the change. Try again.",
            "It will open without Old app compatibility next time.",
            "It was built for an Android version before 4.0. If it crashes, open Settings from the side keys and choose Old app compatibility.",
        ]
        for key in keys {
            let english = text(key, in: "en")
            #expect(text(key, in: language) != english, "\(language) shows \"\(english)\" in English")
        }
    }

    @Test(arguments: OldAppsWordsTests.languages)
    func theAppsNameStaysInTheAlertsAndInVoiceOver(language: String) {
        let label = "Pixel Knights"
        #expect(text("Turn on for \u{201C}\(label)\u{201D}?", in: language).contains(label))
        #expect(text("Turn off for \u{201C}\(label)\u{201D}?", in: language).contains(label))
        #expect(text("\u{201C}\(label)\u{201D} is open and will close now.", in: language).contains(label))
        #expect(text("\(label), Old app compatibility", in: language).contains(label))
    }

    @Test func noWordNamesACompanyOrCallsTheMachineAGuest() {
        for language in ["en"] + Self.languages {
            let words = [
                text("Old app compatibility", in: language),
                text("Some apps this old crash because they rely on how Android used to work. Old app compatibility makes Android run an app more like older versions did.", in: language),
                text("Turn it on only for an app that crashes: it can make an app that works today behave differently. Turning it on or off closes the app if it\u{2019}s open. Only apps it can apply to are listed.", in: language),
                text("Android will run this app more like older versions did. This may help an app that crashes, but it can also make the app behave differently. You can turn it off again here.", in: language),
            ].joined(separator: " ")
            for forbidden in ["Google", "Apple", "VM ", "guest", "Guest"] {
                #expect(!words.contains(forbidden), "\(language) says \(forbidden)")
            }
        }
    }
}

/// Settings and the page on the real views in a window: the row exists only
/// when the last listing found an app, and says On when a switch is on.
///
/// SwiftUI publishes its accessibility elements only while application
/// accessibility is on (the Simulator preference `ApplicationAccessibilityEnabled`).
@Suite(.serialized)
@MainActor
struct OldAppsScreenTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(mode: StartMode = .normal, listing: OldAppListing? = .apps(ScriptedOldApps.sampleApps)) -> AppModel {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1)
        model.driver = StubDriver(plan: .normal)
        model.refreshPlan()
        model.fileBrowser = FileBrowserModel(backend: FileBrowserModelTests.ScriptedBackend(), center: NotificationCenter())
        model.start(mode)
        model.finishBoot(mode == .recovery ? .recovery : .normal)
        // What the page asks again when it opens is what the image said.
        let scripted = ScriptedOldApps()
        scripted.delay = .milliseconds(1)
        scripted.listingAnswer = listing ?? .failed
        model.scriptedOldApps = scripted
        if let listing { model.session.oldApps.apply(listing) }
        return model
    }

    /// The labels the view draws once it has settled.
    private func labels<Content: View>(of view: Content, in model: AppModel) async throws -> [String] {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let container = UIViewController()
        lease.window.rootViewController = container
        let hosting = UIHostingController(rootView: view.environment(model))
        container.addChild(hosting)
        hosting.view.frame = CGRect(x: 0, y: 0, width: 390, height: 2600)
        container.view.addSubview(hosting.view)
        hosting.didMove(toParent: container)
        defer {
            hosting.willMove(toParent: nil)
            hosting.view.removeFromSuperview()
            hosting.removeFromParent()
        }
        var found: [String] = []
        var steady = 0
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline, steady < 4 {
            try await Task.sleep(for: .milliseconds(250))
            hosting.view.setNeedsLayout()
            hosting.view.layoutIfNeeded()
            let now = SideKeysTouchTests.elements(in: hosting.view).sorted { $0.frame.minY < $1.frame.minY }.map(\.label)
            steady = !now.isEmpty && now == found ? steady + 1 : 0
            found = now
        }
        return found
    }

    private func settingsHas(_ text: String, model: AppModel) async throws -> Bool {
        try await labels(of: SessionSettingsSheet(large: true), in: model).contains { $0.contains(text) }
    }

    @Test func settingsOffersTheRowWhenTheListingFoundAnApp() async throws {
        let withApps = model()
        #expect(try await settingsHas("Old app compatibility", model: withApps))
        // A switch that is on is said in the row.
        #expect(try await settingsHas("Old app compatibility, On", model: withApps))
        let noneOn = model(listing: .apps([ScriptedOldApps.sampleApps[0]]))
        #expect(try await settingsHas("Old app compatibility", model: noneOn))
        #expect(!(try await settingsHas("Old app compatibility, On", model: noneOn)))
    }

    @Test func settingsHasNoRowWithoutAListingOrAnAppOrTheCommandOrInRecovery() async throws {
        #expect(!(try await settingsHas("Old app compatibility", model: model(listing: nil))))
        #expect(!(try await settingsHas("Old app compatibility", model: model(listing: .apps([])))))
        #expect(!(try await settingsHas("Old app compatibility", model: model(listing: .unavailable))))
        #expect(!(try await settingsHas("Old app compatibility", model: model(listing: .failed))))
        #expect(!(try await settingsHas("Old app compatibility", model: model(mode: .recovery))))
    }

    @Test func thePageListsEachAppWithItsSwitchAndItsPackageName() async throws {
        let labels = try await labels(of: NavigationStack { OldAppsPage() }, in: model())
        #expect(labels.contains("Pixel Knights, Old app compatibility"))
        #expect(labels.contains("Rally 2011, Old app compatibility"))
        #expect(labels.contains { $0.contains("Built for Android before 4.0") })
    }

    @Test func thePageSaysWhenNoAppIsInstalledAndWhenTheVirtualMachineDidNotAnswer() async throws {
        let empty = try await labels(of: NavigationStack { OldAppsPage() }, in: model(listing: .apps([])))
        #expect(empty.contains("No app built for Android before 4.0 is installed."))
        let failed = model(listing: .failed)
        let words = try await labels(of: NavigationStack { OldAppsPage() }, in: failed)
        #expect(words.contains("The virtual machine didn\u{2019}t answer. Try again."))
        #expect(words.contains("Try again"))
    }
}

/// The command through the application's own adb channel: the real operations
/// against an adbd that answers as the image's command is specified to.
@Suite(.serialized)
struct OldAppsOperationTests {
    private func adapter(_ adbd: DiscardingAdbd) throws -> OldAppCompatibility {
        let operations = AdbGuestOperations(
            identity: try throwawayIdentity(), clock: GuestRunClock(), device: "iPhone", connector: adbd,
            hostFree: { .max }, run: GuestRunMark.random())
        return OldAppCompatibility(shell: AdbGuestShell(operations: operations))
    }

    @Test func theCommandsRunInTheGuestsShellAndTheAnswersAreRead() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.shellAnswers[OldAppCompatibility.listCommand] = .init(
            text: "kernarium_oldapp 1\ncom.example.rally\tRally 2011\toff\t0\t1700000000\n", status: 0)
        adbd.shellAnswers["kernarium_oldapp set 'com.example.rally' on"] = .init(text: "com.example.rally\ton\n", status: 0)
        adbd.shellAnswers["pidof 'com.example.rally'"] = .init(text: "", status: 1)
        let old = try adapter(adbd)
        #expect(await old.list() == .apps([OldApp(package: "com.example.rally", label: "Rally 2011", isOn: false, lastUpdate: 1_700_000_000)]))
        #expect(await old.isOpen(package: "com.example.rally") == false)
        #expect(await old.set(package: "com.example.rally", on: true) == .applied)
        #expect(adbd.shellCommands == ["kernarium_oldapp list", "pidof 'com.example.rally'", "kernarium_oldapp set 'com.example.rally' on"])
    }

    @Test func aGuestWithoutTheCommandAnswersWithTheShellsNotFoundStatus() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.shellAnswers[OldAppCompatibility.listCommand] = .init(text: "", status: 127)
        #expect(await (try adapter(adbd)).list() == .unavailable)
    }

    @Test func aRefusalIsTheCommandsStatusTwo() async throws {
        let adbd = DiscardingAdbd()
        defer { adbd.shutdown() }
        adbd.shellAnswers["kernarium_oldapp set 'com.example.rally' on"] = .init(text: "", status: 2)
        adbd.shellAnswers["kernarium_oldapp set 'com.example.other' off"] = .init(text: "", status: 64)
        let old = try adapter(adbd)
        #expect(await old.set(package: "com.example.rally", on: true) == .notEligible)
        #expect(await old.set(package: "com.example.other", on: false) == .failed)
    }

    @Test func aGuestThatCannotBeReachedIsAFailure() async throws {
        let operations = AdbGuestOperations(
            identity: nil, clock: GuestRunClock(), device: "iPhone", hostFree: { .max }, run: GuestRunMark.random())
        let old = OldAppCompatibility(shell: AdbGuestShell(operations: operations))
        #expect(await old.list() == .failed)
    }
}
