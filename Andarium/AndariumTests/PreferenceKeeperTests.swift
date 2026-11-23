// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// What the person sets is kept across launches, and a value the launch
/// decided for its own run leaves what the person stored alone. Each test
/// keeps into a defaults database of its own and awaits the write an observed
/// change queues, so nothing waits on a clock.
@MainActor
struct PreferenceKeeperTests {
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

    /// Waits for the write the change just made has queued.
    private func written(by keeper: PreferenceKeeper) async throws {
        let write = try #require(keeper.pending, "the change queued no write")
        await write.value
    }

    @Test func aFreshModelKeepsTheStatisticsOffAndTheDisplayWithoutAChoice() {
        let model = model()
        #expect(model.showStatsToKeep == false)
        #expect(model.displayOptionsToKeep == DisplayOptions())
        model.session.showStats = true
        #expect(model.showStatsToKeep == true)
    }

    @Test func aValueTheLaunchDecidedIsNotKept() {
        let model = model()
        model.session.showStats = true
        model.statsDecidedByLaunch = true
        #expect(model.showStatsToKeep == nil)

        model.chooseResolution(.native)
        #expect(model.displayOptionsToKeep == DisplayOptions(chosenScale: .native))
        model.launchScale = .points
        #expect(model.displayOptionsToKeep == nil)
    }

    @Test func theStatisticsToggleIsWrittenWhenItChanges() async throws {
        try await withDefaults { defaults in
            let model = model()
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.session.showStats = true
            try await written(by: keeper)
            #expect(StatsPreference(defaults: defaults).isOn)

            // The write re-armed the observation.
            model.session.showStats = false
            try await written(by: keeper)
            #expect(!StatsPreference(defaults: defaults).isOn)
        }
    }

    @Test func aChoiceOfResolutionIsWrittenWhenMade() async throws {
        try await withDefaults { defaults in
            let model = model()
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.chooseResolution(.points)
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: .points))
            model.chooseResolution(.native)
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: .native))
            model.chooseResolution(.factor(1.5))
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: .factor(1.5)))
        }
    }

    /// The bytes the released build wrote (`DisplayOptions` before the Resolution control), as the store reads
    /// them: no choice, whatever they hold.
    @Test func aStoredRetinaModeIsNoChoiceWhenTheOptionsAreRead() async throws {
        try await withDefaults { defaults in
            for json in [
                #"{"retina":false,"chosen":true}"#, #"{"retina":true,"chosen":true}"#,
                #"{"retina":false,"chosen":false}"#, #"{"retina":true}"#, #"{"retina":false}"#,
            ] {
                defaults.set(Data(json.utf8), forKey: DisplayOptions.storageKey)
                #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(), "\(json)")
            }
            defaults.removeObject(forKey: DisplayOptions.storageKey)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions())
            // The Resolution control's form stays the person's choice.
            for (json, scale) in [
                (#"{"scale":"native"}"#, GuestDisplayScale.native), (#"{"scale":1}"#, .points),
                (#"{"scale":2.5}"#, .factor(2.5)),
            ] {
                defaults.set(Data(json.utf8), forKey: DisplayOptions.storageKey)
                #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: scale), "\(json)")
            }
        }
    }

    /// Someone from the released build gets the default at their next launch, and what they choose after it is
    /// kept through the real keeper and read back at the launch after.
    @Test func aChoiceMadeAfterTheMigrationSurvivesASaveAndAReload() async throws {
        try await withDefaults { defaults in
            defaults.set(Data(#"{"retina":true,"chosen":true}"#.utf8), forKey: DisplayOptions.storageKey)
            let first = model()
            first.chosenScale = DisplayOptionsStore.load(from: defaults).chosenScale
            first.memoryLimitBytes = 8 << 30
            first.screenNativeScale = 3
            #expect(first.chosenScale == nil && first.resolution == .factor(2))
            let keeper = PreferenceKeeper(model: first, defaults: defaults)
            keeper.start()

            // A statistics toggle writes the display options too: still no choice.
            first.session.showStats = true
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions())

            first.chooseResolution(.factor(2.5))
            try await written(by: keeper)
            let stored = try #require(defaults.data(forKey: DisplayOptions.storageKey))
            #expect(String(decoding: stored, as: UTF8.self) == #"{"scale":2.5}"#)

            // The next launch, on a device whose limit changed the default.
            let second = model()
            second.chosenScale = DisplayOptionsStore.load(from: defaults).chosenScale
            second.memoryLimitBytes = 2 << 30
            second.screenNativeScale = 3
            #expect(second.chosenScale == .factor(2.5))
            #expect(second.resolution == .factor(2.5))
            // Native, and one pixel to a point, are kept as the choice too.
            first.chooseResolution(.native)
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: .native))
            first.chooseResolution(.points)
            try await written(by: keeper)
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: .points))
        }
    }

    @Test func aLaunchThatDecidedTheStatisticsWritesNone() async {
        await withDefaults { defaults in
            StatsPreference(defaults: defaults).keep(true)
            let model = model()
            model.statsDecidedByLaunch = true
            model.session.showStats = false
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.session.showStats = true
            model.session.showStats = false
            await keeper.pending?.value
            keeper.keep()
            #expect(StatsPreference(defaults: defaults).isOn)
        }
    }

    @Test func aLaunchThatStatedTheResolutionLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            DisplayOptionsStore.save(DisplayOptions(chosenScale: .points), to: defaults)
            // The launch states native over a stored choice of 1.0.
            let model = model()
            model.chosenScale = .points
            model.launchScale = .native
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            // What the window does afterwards, and any change a write follows.
            model.hostDrivesGuestRotation = true
            model.launchScale = .native
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(DisplayOptionsStore.load(from: defaults) == DisplayOptions(chosenScale: .points))
        }
    }

    @Test func thePointerChoiceIsWrittenWhenMade() async throws {
        try await withDefaults { defaults in
            let model = model()
            #expect(model.pointerAsTouchToKeep == false)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setPointerAsTouch(true)
            try await written(by: keeper)
            #expect(PointerPreference(defaults: defaults).isOn)
            model.setPointerAsTouch(false)
            try await written(by: keeper)
            #expect(!PointerPreference(defaults: defaults).isOn)
        }
    }

    @Test func theHeatLimitChoiceIsWrittenWhenMadeAndASecondSessionReadsIt() async throws {
        try await withDefaults { defaults in
            let model = model()
            #expect(model.heatLimitOn && model.heatLimitToKeep == true)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setHeatLimit(false)
            try await written(by: keeper)
            #expect(!HeatLimitPreference(defaults: defaults).isOn)
            // A new session starts as the person left it.
            let next = HeatLimitPreference(defaults: defaults).launch(automated: false)
            #expect(!next.isOn && next.keepsChanges)
            model.setHeatLimit(true)
            try await written(by: keeper)
            #expect(HeatLimitPreference(defaults: defaults).isOn)
        }
    }

    @Test func aLaunchThatDecidedTheHeatLimitLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            HeatLimitPreference(defaults: defaults).keep(false)
            let model = model()
            model.heatLimitOn = true
            model.heatLimitDecidedByLaunch = true
            #expect(model.heatLimitToKeep == nil)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setHeatLimit(false)
            model.setHeatLimit(true)
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(!HeatLimitPreference(defaults: defaults).isOn, "a harness launch replaced the person's choice")
        }
    }

    @Test func aLaunchThatDecidedThePointerLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            PointerPreference(defaults: defaults).keep(true)
            let model = model()
            model.pointerAsTouchDecidedByLaunch = true
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setPointerAsTouch(false)
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(PointerPreference(defaults: defaults).isOn)
        }
    }

    @Test func theJitChoiceIsWrittenWhenMade() async throws {
        try await withDefaults { defaults in
            let model = model()
            #expect(model.jitToKeep == false)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setJit(true)
            try await written(by: keeper)
            #expect(JitPreference(defaults: defaults).isOn)
            model.setJit(false)
            try await written(by: keeper)
            #expect(!JitPreference(defaults: defaults).isOn)
        }
    }

    @Test func aLaunchThatDecidedTheJitLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            JitPreference(defaults: defaults).keep(true)
            let model = model()
            model.jitDecidedByLaunch = true
            model.jitOn = false
            #expect(model.jitToKeep == nil)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setJit(true)
            model.setJit(false)
            await keeper.pending?.value
            keeper.keep()
            #expect(JitPreference(defaults: defaults).isOn)
        }
    }

    @Test func theTranslatedLibrariesChoiceIsWrittenWhenMadeAndASecondSessionReadsIt() async throws {
        try await withDefaults { defaults in
            let model = model()
            #expect(model.translatedLibrariesOn && model.translatedLibrariesToKeep == true)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setTranslatedLibraries(false)
            try await written(by: keeper)
            #expect(!TranslatedLibrariesPreference(defaults: defaults).isOn)
            // A new session starts as the person left it.
            let next = TranslatedLibrariesPreference(defaults: defaults).launch(automated: false)
            #expect(!next.isOn && next.keepsChanges)
            model.setTranslatedLibraries(true)
            try await written(by: keeper)
            #expect(TranslatedLibrariesPreference(defaults: defaults).isOn)
        }
    }

    @Test func aLaunchThatDecidedTheTranslatedLibrariesLeavesTheStoredChoice() async {
        await withDefaults { defaults in
            TranslatedLibrariesPreference(defaults: defaults).keep(false)
            let model = model()
            model.translatedLibrariesOn = true
            model.translatedLibrariesDecidedByLaunch = true
            #expect(model.translatedLibrariesToKeep == nil)
            let keeper = PreferenceKeeper(model: model, defaults: defaults)
            keeper.start()

            model.setTranslatedLibraries(false)
            model.setTranslatedLibraries(true)
            model.session.showStats = true
            await keeper.pending?.value
            keeper.keep()
            #expect(
                !TranslatedLibrariesPreference(defaults: defaults).isOn, "a harness launch replaced the person's choice")
        }
    }
}
