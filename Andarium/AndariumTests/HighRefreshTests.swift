// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// Home's "120 Hz" in the model, in what it keeps, in what a launch states
/// of it, in the boot request and in what Low Power Mode does to the guest's
/// render rate. The row is drawn by `HomePerformanceTests`, and Settings has none
/// (`SettingsOrderTests`).
@MainActor
struct HighRefreshTests {
    private func model() -> AppModel {
        AppModel(
            guest: nil, host: AppEntry.hostDevice(),
            system: SystemInfo(version: "17", release: "test", kernel: "Linux", architecture: "arm64", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
    }

    private func withDefaults(_ body: (UserDefaults) async throws -> Void) async rethrows {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        try await body(defaults)
    }

    // MARK: The model

    @Test func theSettingIsOffAndNotOfferedUntilTheScreenSaysItShowsAtLeastOneHundred() {
        let model = model()
        #expect(!model.highRefreshOn)
        #expect(!model.highRefreshOffered)
        #expect(model.screenMaximumFramesPerSecond == 60)
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        #expect(model.highRefreshOffered)
        // A reader of the window that does not read the rate leaves it.
        model.screenDidChange(nativeScale: 3)
        #expect(model.screenMaximumFramesPerSecond == 120)
        // A screen that shows 61 to 99 is given a 120 Hz mode that the link cannot pace.
        for rate in [61, 90, 99] {
            model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: rate)
            #expect(!model.highRefreshOffered, "a screen at \(rate)")
        }
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 100)
        #expect(model.highRefreshOffered)
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 60)
        #expect(!model.highRefreshOffered)
    }

    @Test func aMoveToAnotherScreenChangesTheOfferAndAnyReadingOfItsRate() {
        let model = model()
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        #expect(model.highRefreshOffered)
        // The scene moves to a 60 Hz external display, and back.
        model.screenDidChange(nativeScale: 2, maximumFramesPerSecond: 60)
        #expect(!model.highRefreshOffered)
        model.setHighRefresh(true)
        #expect(!model.asksForHighRefresh, "the old screen's maximum asked for the mode")
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        #expect(model.asksForHighRefresh)
    }

    @Test func aStatedScreenWinsOverTheReadOne() {
        let model = model()
        model.statedMaximumFramesPerSecond = 120
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 60)
        #expect(model.highRefreshOffered)
    }

    /// An iPad app on a Vision Pro (or on a Mac) has no 120 Hz display that the
    /// link can pace: the offer follows the played host, whatever the screen
    /// and the idiom say.
    @Test func theSettingIsNotOfferedToAnIPadAppOnAVisionProOrAMac() {
        let model = model()
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        model.playedPadAppHost = IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false)
        #expect(model.highRefreshOffered, "an iPhone or iPad was refused")
        for (name, host) in [
            ("a Vision Pro", IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: true)),
            ("a Mac", IPadAppHost(isiOSAppOnMac: true, isiOSAppOnVision: false)),
        ] {
            model.playedPadAppHost = host
            #expect(!model.highRefreshOffered, "\(name) was offered 120 Hz")
            model.setHighRefresh(true)
            #expect(!model.asksForHighRefresh, "\(name) asked for the 120 Hz display")
        }
        model.playedPadAppHost = IPadAppHost(isiOSAppOnMac: false, isiOSAppOnVision: false)
        #expect(model.highRefreshOffered && model.asksForHighRefresh)
    }

    @Test func theSettingIsNotOfferedOnAMac() {
        let model = model()
        model.host.idiom = .mac
        model.screenDidChange(nativeScale: 2, maximumFramesPerSecond: 120)
        #expect(!model.highRefreshOffered)
        model.setHighRefresh(true)
        #expect(!model.asksForHighRefresh, "a Mac asked for the 120 Hz display")
        model.host.idiom = .pad
        #expect(model.highRefreshOffered)
        #expect(model.asksForHighRefresh)
    }

    @Test func theSettingIsNotOfferedWhileTheDisplaySizeCannotStateIt() {
        let model = model()
        model.screenDidChange(nativeScale: 2, maximumFramesPerSecond: 120)
        // A 13-inch iPad Pro at its native size: 2752 x 2064 pixels.
        model.displayPointsDidChange(CGSize(width: 1376, height: 1032))
        model.chooseResolution(.native)
        #expect(model.resolutionLayout?.width == 2752)
        #expect(!model.highRefreshOffered, "the size's clock does not fit the EDID")
        model.setHighRefresh(true)
        #expect(!model.asksForHighRefresh)
        // The same iPad at a smaller resolution can state it.
        model.chooseResolution(.factor(1.5))
        #expect(model.highRefreshOffered)
        #expect(model.asksForHighRefresh)
        // A start decides by the size it boots at.
        #expect(!model.asksForHighRefresh(displaySize: (2752, 2064)))
        #expect(model.asksForHighRefresh(displaySize: (860, 1678)))
    }

    @Test func aStartAsksForTheDisplayOnlyWhenTheSettingIsOnAndTheScreenCanShowIt() {
        let model = model()
        model.setHighRefresh(true)
        #expect(model.highRefreshOn)
        #expect(!model.asksForHighRefresh, "a 60 Hz screen asked for 120 Hz")
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        #expect(model.asksForHighRefresh)
        model.setHighRefresh(false)
        #expect(!model.asksForHighRefresh)
    }

    @Test func theRunHasWhatTheRuntimeGrantedNotWhatTheStartAsked() {
        let model = model()
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        model.setHighRefresh(true)
        model.phase = .running(.normal)
        // The start asked and the runtime refused: the run has the 60 Hz display.
        model.runStarted(withHighRefresh: false)
        #expect(!model.runHasHighRefresh)
        // The runtime's grant, once known, is the run's.
        model.runStarted(withHighRefresh: true)
        #expect(model.runHasHighRefresh)
    }

    @Test func theSettingDoesNotChangeTheDisplayOfARunThatIsGoing() {
        let model = model()
        model.screenDidChange(nativeScale: 3, maximumFramesPerSecond: 120)
        model.phase = .running(.normal)
        model.runStarted(withHighRefresh: false)
        model.setHighRefresh(true)
        #expect(!model.runHasHighRefresh, "the running display cannot change")
        model.setHighRefresh(false)
        // After a start that asked, turning it off leaves the run's display too.
        model.setHighRefresh(true)
        model.runStarted(withHighRefresh: true)
        model.setHighRefresh(false)
        #expect(model.runHasHighRefresh)
    }

    // MARK: What is kept

    @Test func theChoiceIsWrittenWhenMadeAndASecondSessionReadsIt() async throws {
        try await withDefaults { defaults in
            let model = model()
            #expect(model.highRefreshToKeep == false)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setHighRefresh(true)
            let write = try #require(keeper.pending, "the change queued no write")
            await write.value
            #expect(HighRefreshPreference(defaults: defaults).isOn)
            let next = HighRefreshPreference(defaults: defaults).launch(automated: false)
            #expect(next.isOn && next.keepsChanges)
        }
    }

    @Test func aLaunchThatDecidedTheSettingLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            HighRefreshPreference(defaults: defaults).keep(true)
            let model = model()
            model.highRefreshOn = false
            model.highRefreshDecidedByLaunch = true
            #expect(model.highRefreshToKeep == nil)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()
            model.setHighRefresh(true)
            model.setHighRefresh(false)
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(HighRefreshPreference(defaults: defaults).isOn, "a harness launch replaced the person's choice")
        }
    }

    // MARK: A launch

    @Test func aLaunchStatesTheSettingWithOneOrZero() {
        #expect(HighRefreshLaunch.stated(in: ["ANDARIUM_HIGH_REFRESH": "1"]) == true)
        #expect(HighRefreshLaunch.stated(in: ["ANDARIUM_HIGH_REFRESH": "0"]) == false)
        #expect(HighRefreshLaunch.stated(in: ["ANDARIUM_HIGH_REFRESH": "yes"]) == nil)
        #expect(HighRefreshLaunch.stated(in: [:]) == nil)
    }

    @Test func aLaunchThatStatesTheSettingIsAHarnessLaunch() {
        let overrides = LaunchOverrides(environment: [:], processorCount: 4)
        #expect(AppEntry.isPersonsLaunch(overrides: overrides, environment: [:]))
        #expect(!AppEntry.isPersonsLaunch(overrides: overrides, environment: ["ANDARIUM_HIGH_REFRESH": "1"]))
    }

    // MARK: The boot request

    @Test func theBootRequestCarriesTheBitOnlyWhenAsked() {
        let bit = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HIGH_REFRESH)
        let haptics = UInt32(KERNARIUM_RUNTIME_ANDROID_HOST_HAPTICS)
        #expect(bit == 8)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, highRefresh: false) == 0)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: false, highRefresh: true) == bit)
        #expect(LiveRuntime.hostFeatures(hapticsAvailable: true, highRefresh: true) == haptics | bit)
    }

    // MARK: Low Power Mode

    @Test func theLowPowerMonitorReadsNowOnAChangeAndOnRefreshAndSaysNoneWhenItStops() {
        let center = NotificationCenter()
        var states = [false, true, false]
        var delivered: [Bool] = []
        let monitor = LowPowerMonitor(
            center: center, read: { states.isEmpty ? false : states.removeFirst() },
            deliver: { delivered.append($0) })
        monitor.start()
        #expect(delivered == [false])
        monitor.refresh()
        #expect(delivered == [false, true])
        center.post(name: .NSProcessInfoPowerStateDidChange, object: nil)
        #expect(delivered == [false, true, false])
        monitor.stop()
        // Stopping lifts any hold, and a stopped monitor reads nothing.
        #expect(delivered == [false, true, false, false])
        center.post(name: .NSProcessInfoPowerStateDidChange, object: nil)
        #expect(delivered.count == 4, "a stopped monitor read the power state")
    }
}
